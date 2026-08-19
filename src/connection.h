/*-------------------------------------------------------------------------
 *
 * connection.h
 *	  ODBC Connection handle management
 *
 * Copyright (c) 2026, Dave Cramer
 *
 *
 * IDENTIFICATION
 *	  src/connection.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PSQLODBC2_CONNECTION_H
#define PSQLODBC2_CONNECTION_H

#include "psqlodbc2.h"
#include "diagnostics.h"

#include <stdbool.h>
#include <libpq-fe.h>

/* Forward declaration — full definition is in environment.h */
struct OdbcEnvironment;
typedef struct OdbcEnvironment OdbcEnvironment;

/* Forward declaration — full definition is in statement.h */
struct OdbcStatement;

/* ---- Connection State ----
 *
 * Tracks the current state of the connection for lifecycle validation.
 * The ODBC spec defines specific function-sequence rules based on
 * connection state (e.g., you cannot execute statements on a disconnected
 * connection). */
typedef enum {
    CONNECTION_STATE_NOT_CONNECTED = 0,
    CONNECTION_STATE_CONNECTED,
    CONNECTION_STATE_BROKEN,
    CONNECTION_STATE_EXECUTING
} ConnectionState;

/* ---- Transaction State ----
 *
 * Tracks the state of the current transaction when autocommit is OFF.
 * PostgreSQL requires explicit BEGIN before DML changes are grouped into
 * a transaction, and explicit COMMIT/ROLLBACK to end it. The FAILED state
 * occurs when a query errors inside a transaction — PostgreSQL marks the
 * transaction as aborted and requires ROLLBACK before any further commands. */
typedef enum {
    TRANSACTION_STATE_IDLE = 0,      /* No active transaction */
    TRANSACTION_STATE_ACTIVE,        /* In a transaction (BEGIN was issued) */
    TRANSACTION_STATE_FAILED         /* Transaction aborted by error (needs ROLLBACK) */
} TransactionState;

/* ---- Connection Parameters ----
 *
 * Holds the parsed connection parameters that are translated into a libpq
 * connection string. Only the essential parameters are supported in this
 * initial implementation. The password field is heap-allocated separately
 * so it can be securely wiped (memset to zero) before being freed. */
typedef struct ConnectionInfo {
    char server[256];
    char port[16];
    char database[256];
    char username[256];
    char *password;              /* heap-allocated; cleared before free for security */
    char sslmode[32];
    char application_name[256];
    unsigned int connect_timeout;

    /* When true (the driver default, matching the original psqlodbc), PostgreSQL
     * boolean columns are described to the application as SQL_VARCHAR(5) rather
     * than SQL_BIT. Some applications (notably MS Access) cannot handle a true
     * BIT type and expect the textual "true"/"false"/"1"/"0" representation.
     * Controlled by the "BoolsAsChar" connection-string keyword. */
    bool bools_as_char;

    /* How the driver reports the size of variable-length columns whose maximum
     * length is not declared (e.g. unbounded text/varchar). Controlled by the
     * "UnknownSizes" connection-string keyword. See UNKNOWN_SIZES_* constants. */
    int unknown_sizes;

    /* Maximum size (in characters) reported for varchar/char columns without a
     * declared length limit. Controlled by the "MaxVarcharSize" keyword. */
    int max_varchar_size;

    /* When true (the "Parse" connection keyword), the driver parses SELECT
     * statements client-side to refine result-column metadata — notably to
     * report a string literal in the select list as VARCHAR(length) rather than
     * relying solely on PostgreSQL's "text" type. Matches the original driver's
     * Parse option used by MS Access. */
    bool parse_statements;

    /* When true (the "FetchRefcursors" connection keyword), a function/procedure
     * that returns refcursor OUT parameters has each cursor automatically
     * FETCH ALL'd and its rows exposed as successive result sets, walkable with
     * SQLMoreResults. When false (the default), the refcursor OUT value is
     * returned verbatim as its portal name. Because a refcursor is only valid
     * inside the transaction that opened it, this option requires the call to
     * run in a transaction (autocommit OFF), matching the original driver. */
    bool fetch_refcursors;

    /* How the driver reacts when a statement errors inside an explicit
     * (autocommit-OFF) transaction. Set from the "Protocol=7.4-N" connection
     * keyword, where N is this value. See the ROLLBACK_ON_ERROR_* constants.
     * A negative value means "not specified" — the driver then applies the
     * server-appropriate default (statement-level rollback on modern servers).
     * Matches the original psqlodbc's rollback_on_error option. */
    int rollback_on_error;

    /* When true, string column values delivered to SQL_C_CHAR / SQL_C_WCHAR have
     * every bare line feed ('\n') expanded to a carriage-return/line-feed pair
     * ("\r\n"). The reported (indicator) length counts the EXPANDED bytes. Some
     * Windows applications require CR+LF line endings in fetched text. Controlled
     * by the "LFConversion" keyword (and packed into the "CX" bitfield, bit
     * CONNECTION_LF_CONVERSION_BIT). Matches the original psqlodbc's
     * lf_conversion option. */
    bool lf_conversion;

    /* When true, an empty bound character string ("") targeting a date/time
     * column is sent to PostgreSQL as SQL NULL instead of the empty literal
     * (which PostgreSQL rejects as "invalid input syntax for type date").
     * Some applications (historically FoxPro) represent a null date this way.
     * Controlled by bit CONNECTION_CVT_NULL_DATE_BIT of the "AB" (extra options)
     * bitfield. Matches the original psqlodbc's cvt_null_date_string option. */
    bool cvt_null_date;
} ConnectionInfo;

/* Bit within the packed "CX" connection-string bitfield that enables
 * LFConversion (see ConnectionInfo.lf_conversion). Matches the original
 * psqlodbc's BIT_LFCONVERSION. */
#define CONNECTION_LF_CONVERSION_BIT 0x01L

/* Bit within the "AB" (extra options) connection-string bitfield that enables
 * CvtNullDate (see ConnectionInfo.cvt_null_date). Matches the original
 * psqlodbc's BIT_CVT_NULL_DATE (1 << 3). */
#define CONNECTION_CVT_NULL_DATE_BIT 0x08L

/* Values for ConnectionInfo.rollback_on_error, matching the original psqlodbc.
 *   NOTHING:   leave the aborted transaction as-is; the application must issue
 *              its own ROLLBACK (via SQLEndTran) before continuing.
 *   TRANSACTION: automatically roll back the entire transaction on any error.
 *   STATEMENT: roll back only the failed statement (via a per-statement
 *              SAVEPOINT), preserving earlier successful work in the transaction.
 * UNSPECIFIED (-1) selects the default resolved at runtime from the server
 * version — STATEMENT on servers new enough to support SAVEPOINT. */
#define ROLLBACK_ON_ERROR_UNSPECIFIED  (-1)
#define ROLLBACK_ON_ERROR_NOTHING       0
#define ROLLBACK_ON_ERROR_TRANSACTION   1
#define ROLLBACK_ON_ERROR_STATEMENT     2

/* Name of the internal SAVEPOINT used to implement statement-level error
 * rollback (rollback_on_error == STATEMENT). One savepoint is (re)established
 * before each statement and rolled back to if that statement fails, so a single
 * fixed name suffices. */
#define PER_STATEMENT_SAVEPOINT_NAME "_psqlodbc2_stmt_svp_"

/* Values for ConnectionInfo.unknown_sizes, matching the original psqlodbc.
 * MAX:     report the driver's configured maximum size for the type.
 * DONTKNOW: report "size unknown" (0), letting the application decide.
 * LONGEST:  report the length of the longest value actually present in the
 *           result set (requires scanning the fetched rows). */
#define UNKNOWN_SIZES_MAX      0
#define UNKNOWN_SIZES_DONTKNOW 1
#define UNKNOWN_SIZES_LONGEST  2

/* Default maximum size (in characters) for varchar/char columns without a
 * declared length limit, matching the original psqlodbc's MAX_VARCHAR_SIZE. */
#define DEFAULT_MAX_VARCHAR_SIZE 255

/* Maximum size (in characters) for text/longvarchar columns without a declared
 * length limit, matching the original psqlodbc's TEXT_FIELD_SIZE. */
#define DEFAULT_MAX_LONGVARCHAR_SIZE 8190

/* ---- Connection Handle ---- */

/* Magic number for runtime type checking of connection handles.
 * "CON2" in ASCII = 0x434F4E32 */
#define CONNECTION_MAGIC_NUMBER 0x434F4E32

/* Maximum number of statement handles that a single connection can track.
 * This matches typical ODBC application usage. The fixed-size array avoids
 * dynamic allocation overhead for what is a bookkeeping list. */
#define MAX_STATEMENTS_PER_CONNECTION 256

/* Maximum number of NOTICE messages captured between statement executions.
 * PostgreSQL can emit multiple notices during a single command (e.g., cascading
 * drops), so we store several but cap it to avoid unbounded memory growth. */
#define MAX_CAPTURED_NOTICES 16

typedef struct OdbcConnection {
    unsigned int magic_number;       /* Must equal CONNECTION_MAGIC_NUMBER when valid */
    ConnectionState state;
    OdbcEnvironment *parent_environment;
    PGconn *libpq_connection;
    ConnectionInfo info;
    DiagnosticRecords diagnostics;
    bool autocommit;
    int server_version_major;
    int server_version_minor;

    /* Maximum number of bytes a single character occupies in the client
     * encoding (e.g. 4 for UTF-8, 1 for LATIN1). Used to convert a column's
     * character-count size into a worst-case byte length (octet length).
     * Populated at connect time from the negotiated client encoding. */
    int max_bytes_per_char;
    struct OdbcStatement *statements[MAX_STATEMENTS_PER_CONNECTION];
    int statement_count;
    int next_statement_id;  /* Counter for generating unique prepared statement names */

    /* Transaction management */
    TransactionState transaction_state;

    /* True when an internal per-statement SAVEPOINT is currently established
     * inside the active transaction (statement-level error rollback, i.e.
     * rollback_on_error == STATEMENT). Reset whenever the transaction ends.
     * Used to decide whether the next statement must RELEASE-and-recreate the
     * savepoint versus create it for the first time. */
    bool statement_savepoint_active;
    SQLUINTEGER txn_isolation;       /* SQL_TXN_READ_COMMITTED, SQL_TXN_SERIALIZABLE, etc. */
    SQLUINTEGER login_timeout;       /* Seconds; applied at connect time via connect_timeout */
    SQLUINTEGER connection_timeout;  /* Seconds; 0 = no timeout (informational only for PG) */
    SQLUINTEGER access_mode;         /* SQL_MODE_READ_WRITE or SQL_MODE_READ_ONLY */

    /* NOTICE capture: libpq's notice receiver stores messages here between
     * statement executions. After execution, they are promoted to diagnostic
     * records with SQLSTATE "01000" (General warning). */
    char *captured_notices[MAX_CAPTURED_NOTICES];  /* heap-allocated strings */
    int notice_count;

    /* MS Access / Jet compatibility mode, toggled by SQLSetConnectAttr with
     * the driver-specific SQL_ATTR_PGOPT_MSJET (65549) attribute. Enables the
     * ("col" = 1) -> ("col"='1') boolean rewrite in the query parser. */
    bool ms_jet;

    /* Array-execution batch size, set via SQLSetConnectAttr with the
     * driver-specific SQL_ATTR_PGOPT_BATCHSIZE attribute. Controls how many
     * parameter sets (rows) an array-bound execution groups per server
     * round-trip: rows are partitioned into consecutive batches of this size,
     * and a batch that errors marks all of its rows ERROR and stops (remaining
     * rows UNUSED). Defaults to DEFAULT_BATCH_SIZE. A value <= 0 is treated as
     * the default. */
    int batch_size;

    /* PostgreSQL large objects are referenced from ordinary tables through a
     * per-database DOMAIN named "lo" built over the base type "oid". The domain
     * has no fixed OID (it is created per database), so we look it up at connect
     * time (see connection_lookup_large_object_type). large_object_type_oid holds
     * that domain's OID (InvalidOid when the database has no "lo" domain), and
     * large_object_is_domain records whether it is genuinely a domain over "oid"
     * (versus a bare base type). These let parameter and result handling
     * recognize large-object references. */
    Oid large_object_type_oid;
    bool large_object_is_domain;
} OdbcConnection;

/* Default array-execution batch size when the application has not set
 * SQL_ATTR_PGOPT_BATCHSIZE. Matches the original psqlodbc default. */
#define DEFAULT_BATCH_SIZE 100

/* Driver-specific connection attribute (from the original psqlodbc's
 * pgapifunc.h) that MS Access sets to request Jet-compatibility quirks. */
#define SQL_ATTR_PGOPT_MSJET 65549

/* Driver-specific attribute (psqlodbc pgapifunc.h) that sets the server-side
 * declare/fetch batch size. This driver buffers every result set client-side,
 * so the value has no effect, but the attribute must be accepted so applications
 * that tune fetch size don't fail. */
#define SQL_ATTR_PGOPT_FETCH 65541

/* Driver-specific attribute (psqlodbc pgapifunc.h) that sets the array-execution
 * batch size — how many bound parameter sets are grouped per server round-trip
 * (see OdbcConnection.batch_size). */
#define SQL_ATTR_PGOPT_BATCHSIZE 65550

/*
 * Clear all captured notices, freeing the heap-allocated message strings.
 * Called after notices have been promoted to diagnostic records on a statement.
 */
void connection_clear_notices(OdbcConnection *connection);

/*
 * Return whether the server currently has standard_conforming_strings on.
 * Read from libpq's tracked parameter status, which reflects any SET the
 * application issued. Defaults to true (the modern PostgreSQL default) when
 * the status is unavailable. Controls how the query parser treats backslashes
 * inside ordinary '...' string literals.
 */
bool connection_standard_conforming_strings(const OdbcConnection *connection);

/*
 * Return whether the given PostgreSQL type OID denotes a large-object reference
 * for this connection. True when the OID is the database's "lo" domain OID (as
 * reported for a bound parameter), or — when that domain exists over "oid" — the
 * base "oid" type itself (as a "lo" column's values report on read-back). Always
 * false when the database has no "lo" domain. Mirrors the original psqlodbc's
 * lobj_type / lo_is_domain checks.
 */
bool connection_type_is_large_object(const OdbcConnection *connection, Oid type_oid);

/* ---- Public Interface ---- */

/*
 * Allocate a new connection handle linked to the given environment.
 * Stores the new handle in *output_handle.
 * Returns SQL_SUCCESS on success, SQL_ERROR on failure.
 */
SQLRETURN connection_allocate(OdbcEnvironment *environment, SQLHANDLE *output_handle);

/*
 * Free a connection handle. The connection must be in NOT_CONNECTED or BROKEN
 * state (i.e., disconnect must be called first if connected).
 * Returns SQL_SUCCESS on success, SQL_ERROR if still connected,
 * SQL_INVALID_HANDLE if the handle is invalid.
 */
SQLRETURN connection_free(SQLHANDLE handle);

/*
 * Establish a connection to PostgreSQL using the parameters in the
 * connection's info struct. Calls PQconnectdb with a libpq-formatted
 * connection string built from ConnectionInfo fields.
 * Returns SQL_SUCCESS on success, SQL_ERROR on failure (with diagnostic set).
 */
SQLRETURN connection_connect(OdbcConnection *connection);

/*
 * Disconnect from PostgreSQL. Calls PQfinish and resets state to NOT_CONNECTED.
 * Returns SQL_SUCCESS on success, SQL_ERROR if not currently connected.
 */
SQLRETURN connection_disconnect(OdbcConnection *connection);

/*
 * Clear all fields in a ConnectionInfo struct, securely wiping the password
 * (memset to zero) before freeing it.
 */
void connection_info_clear(ConnectionInfo *info);

/*
 * Register a statement as a child of this connection.
 * Returns true on success, false if the statement array is full.
 */
bool connection_add_statement(OdbcConnection *connection,
                              struct OdbcStatement *statement);

/*
 * Unregister a statement from this connection.
 * Returns true if the statement was found and removed, false otherwise.
 */
bool connection_remove_statement(OdbcConnection *connection,
                                 struct OdbcStatement *statement);

/* ---- Transaction Management ---- */

/*
 * Begin an explicit transaction by issuing "BEGIN" to PostgreSQL.
 * Sets transaction_state to ACTIVE on success.
 * Returns SQL_SUCCESS on success, SQL_ERROR if the BEGIN command fails.
 */
SQLRETURN connection_begin_transaction(OdbcConnection *connection);

/*
 * Commit the current transaction by issuing "COMMIT" to PostgreSQL.
 * Sets transaction_state back to IDLE on success.
 * Returns SQL_SUCCESS on success (or if no transaction is active),
 * SQL_ERROR if the COMMIT command fails.
 */
SQLRETURN connection_commit(OdbcConnection *connection);

/*
 * Rollback the current transaction by issuing "ROLLBACK" to PostgreSQL.
 * Sets transaction_state back to IDLE on success.
 * Returns SQL_SUCCESS on success (or if no transaction is active),
 * SQL_ERROR if the ROLLBACK command fails.
 */
SQLRETURN connection_rollback(OdbcConnection *connection);

/*
 * Ensure a transaction is active when autocommit is OFF.
 * If autocommit is ON, this is a no-op. If autocommit is OFF and
 * no transaction is in progress (IDLE state), issues an implicit BEGIN.
 * Called before every statement execution to implement ODBC's autocommit-off
 * semantics where all statements run within a transaction.
 * Returns SQL_SUCCESS on success, SQL_ERROR if BEGIN fails.
 */
SQLRETURN connection_ensure_transaction(OdbcConnection *connection);

/*
 * Resolve the effective rollback-on-error mode for this connection.
 * Returns the ConnectionInfo.rollback_on_error value when the application set
 * one explicitly; otherwise returns the server-appropriate default
 * (ROLLBACK_ON_ERROR_STATEMENT on modern servers that support SAVEPOINT).
 */
int connection_effective_rollback_on_error(const OdbcConnection *connection);

/*
 * Establish the per-statement SAVEPOINT before executing a statement, when the
 * connection is configured for statement-level error rollback and a transaction
 * is active. If a savepoint from the previous statement is still standing, it is
 * released and re-created so it always marks the point just before this
 * statement. A no-op for other rollback modes, autocommit-ON, or when no
 * transaction is active yet. Errors here are non-fatal and left unreported —
 * the statement still executes.
 */
void connection_begin_statement_savepoint(OdbcConnection *connection);

/*
 * React to a statement failure according to the rollback-on-error mode:
 *   STATEMENT   - roll back to the per-statement SAVEPOINT, undoing only the
 *                 failed statement and clearing the transaction's FAILED state
 *                 so subsequent statements can run.
 *   TRANSACTION - roll back the entire transaction.
 *   NOTHING     - leave the transaction aborted for the application to handle.
 * Called after a statement execution reports an error while autocommit is OFF.
 */
void connection_handle_statement_error(OdbcConnection *connection);

/*
 * Unwind an in-progress COPY that libpq handed back as PGRES_COPY_IN,
 * PGRES_COPY_OUT, or PGRES_COPY_BOTH. This driver does not implement the COPY
 * streaming sub-protocol, so a COPY ... FROM STDIN / TO STDOUT must not be left
 * mid-stream: doing so would block the backend (copy-in) and cause the next
 * command the driver issues to terminate the copy abnormally, poisoning the
 * whole transaction. Ends copy-in with an error, drains copy-out, and consumes
 * the trailing result(s) so the connection returns to a usable state. When the
 * unwound copy left the transaction aborted, the connection is marked FAILED so
 * connection_handle_statement_error can rewind to the per-statement savepoint.
 */
void connection_abort_active_copy(OdbcConnection *connection, ExecStatusType copy_status);

#endif /* PSQLODBC2_CONNECTION_H */
