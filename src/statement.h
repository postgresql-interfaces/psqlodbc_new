/*-------------------------------------------------------------------------
 *
 * statement.h
 *	  ODBC Statement handle management
 *
 * Copyright (c) 2026, Dave Cramer
 *
 *
 * IDENTIFICATION
 *	  src/statement.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PSQLODBC2_STATEMENT_H
#define PSQLODBC2_STATEMENT_H

#include "psqlodbc2.h"
#include "diagnostics.h"
#include "parameter.h"
#include "column_binding.h"
#include "query_parser.h"
#include "descriptor.h"

#include <stdbool.h>
#include <stdint.h>
#include <libpq-fe.h>

/* Forward declaration — full definition is in connection.h */
struct OdbcConnection;
typedef struct OdbcConnection OdbcConnection;

/* ---- Statement State ----
 *
 * Tracks the current state of the statement for lifecycle validation.
 * The ODBC spec defines function-sequence rules based on statement state
 * (e.g., SQLExecute requires the statement to be in PREPARED state). */
typedef enum {
    STATEMENT_STATE_ALLOCATED = 0,  /* Handle exists but no SQL text set */
    STATEMENT_STATE_PREPARED,       /* SQL text stored and server-side prepared */
    STATEMENT_STATE_EXECUTED,       /* DML/DDL executed successfully (no result set) */
    STATEMENT_STATE_HAS_CURSOR      /* SELECT executed; rows available for fetch */
} StatementState;

/* ---- Statement Handle ---- */

/* Magic number for runtime type checking of statement handles.
 * "STM2" in ASCII = 0x53544D32 */
#define STATEMENT_MAGIC_NUMBER 0x53544D32

/* The descriptor handle type (OdbcDescriptor), its DESCRIPTOR_MAGIC_NUMBER, and
 * the DescriptorRole enum now live in descriptor.h, included above, so the four
 * implicit descriptors below and the explicit-descriptor entry points can share
 * one definition. */

/* Maximum length for server-side prepared statement names.
 * Names are auto-generated as "_psqlodbc2_stmt_<counter>". */
#define MAX_PREPARED_NAME_LENGTH 64

/* ---- Keyset (updatable) cursor support ----
 *
 * PostgreSQL has no native updatable cursors, so a positioned UPDATE/DELETE is
 * turned into a searched statement keyed on the row's physical location: its
 * "ctid" ("(block,offset)"). When the application opens an updatable or
 * keyset-driven cursor, the driver rewrites the SELECT to also fetch each row's
 * ctid in a hidden trailing column, so every buffered row carries the key needed
 * to target it later. See rewrite_select_append_ctid / results_set_pos.
 *
 * Sentinel meaning "this statement has no hidden ctid column" (a read-only
 * cursor, or a query we could not safely rewrite). */
#define NO_HIDDEN_CTID_COLUMN (-1)

/* Per-row overlay for an updatable cursor. The buffered PGresult is immutable
 * (libpq owns it), so after a positioned UPDATE/DELETE/REFRESH we cannot mutate
 * PQgetvalue in place. Instead each row gets one of these overlay entries:
 *
 *   - deleted: the row was removed by a positioned DELETE. The fetch engine and
 *     SQLGetData treat it as if it is not present, and delete bookkeeping skips
 *     it, so a re-fetch of the still-open cursor never sees it again.
 *   - override_values: when non-NULL, the row's displayed values come from here
 *     instead of the base PGresult. Populated from an UPDATE/REFRESH ...
 *     RETURNING so a re-fetch shows the new committed values (and the new ctid,
 *     which changes on every UPDATE). The array has one entry per FULL result
 *     column (including the hidden ctid); a NULL entry means SQL NULL. */
typedef struct KeysetRow {
    bool deleted;
    char **override_values;   /* NULL, or full-width array of heap strings (NULL = SQL NULL) */
} KeysetRow;

/* Maximum length of a table name captured from an updatable cursor's SELECT, so
 * SQLSetPos can build "UPDATE <table> ... WHERE ctid=..." against it. */
#define MAX_KEYSET_TABLE_NAME_LENGTH 256

/* Maximum length of a SQL savepoint name we track for keyset-overlay snapshots. */
#define MAX_KEYSET_SAVEPOINT_NAME_LENGTH 64

/* Maximum number of named savepoints whose keyset overlay we snapshot at once.
 * The block-delete regression uses two ("yuuki", "miho"); a small cap suffices. */
#define MAX_KEYSET_SAVEPOINTS 8

/* A snapshot of an updatable cursor's per-row deleted flags, taken when the
 * application issues "SAVEPOINT <name>". PostgreSQL has no server-side updatable
 * cursor here, so a positioned DELETE only marks the client overlay; when the
 * application later rolls the transaction back to this savepoint (which really
 * un-deletes the rows on the server), the driver must likewise restore the
 * overlay's deleted flags — and drop any rows added by SQL_ADD after the
 * savepoint — so the still-open cursor sees the same rows the server now does. */
typedef struct KeysetSavepoint {
    char name[MAX_KEYSET_SAVEPOINT_NAME_LENGTH];
    bool *deleted_flags;   /* Snapshot of KeysetRow.deleted, one per row */
    int row_count;         /* Overlay row count at snapshot time (rows added later are dropped on restore) */
} KeysetSavepoint;

/* Maximum length of an application-supplied cursor name, matching the value
 * reported by SQLGetInfo(SQL_MAX_CURSOR_NAME_LEN). The original psqlodbc caps
 * cursor names at 32 characters; we match it for drop-in compatibility. */
#define MAX_CURSOR_NAME_LENGTH 32

/* ---- Data-at-execution (SQL_DATA_AT_EXEC) streaming ----
 *
 * When a bound parameter's length/indicator is SQL_DATA_AT_EXEC (or the
 * SQL_LEN_DATA_AT_EXEC(length) form), the application does not supply the value
 * at SQLBindParameter/SQLExecute time. Instead SQLExecute returns SQL_NEED_DATA
 * and the application streams each such parameter's bytes afterward:
 *
 *   SQLExecute            -> SQL_NEED_DATA
 *   loop:
 *     SQLParamData(&token)  -> SQL_NEED_DATA, token = next param's value pointer
 *     SQLPutData(chunk,len) -> one or more times, appended to a growing buffer
 *   SQLParamData           -> executes the statement, returns SQL_SUCCESS etc.
 *
 * The per-parameter accumulation and the walk-through cursor below back that
 * protocol. See statement_data_at_exec_* helpers in statement.c. */

/* Sentinel for "no parameter is currently being filled" in the data-at-exec
 * walk. SQLParamData advances current_parameter_index to the next parameter
 * that still needs data; SQLPutData appends to that parameter. */
#define DATA_AT_EXEC_NO_CURRENT_PARAMETER (-1)

/* Accumulated bytes and status for one data-at-execution parameter. The buffer
 * grows via realloc as SQLPutData chunks arrive; when the application signals
 * SQL_NULL_DATA the parameter is marked null and further appends are rejected
 * (HY010) to match the ODBC function-sequence contract. */
typedef struct DataAtExecParameter {
    bool needs_data;          /* True if this parameter was bound SQL_DATA_AT_EXEC */
    bool data_started;        /* True once the first SQLPutData chunk arrived */
    bool is_null;             /* True if SQLPutData supplied SQL_NULL_DATA */
    char *buffer;             /* Growing heap buffer of accumulated bytes (NUL-terminated) */
    SQLLEN length;            /* Number of valid bytes in buffer (excludes the NUL) */
    size_t capacity;          /* Allocated size of buffer in bytes */

    /* ---- Large-object streaming ----
     *
     * When this parameter targets a "lo" (large object) column, its streamed
     * bytes are NOT accumulated in the buffer above; that could be arbitrarily
     * large. Instead the first SQLPutData chunk creates a large object and each
     * chunk is written straight to it via lo_write, so only the current chunk is
     * ever held in memory. At execution the parameter's value becomes the
     * large object's Oid (as decimal text), matching how PostgreSQL stores a
     * "lo" column. */
    bool is_large_object;     /* True if this deferred parameter writes to a large object */
    Oid large_object_oid;     /* Oid of the created large object (InvalidOid until first chunk) */
    int large_object_fd;      /* Open write descriptor, or -1 when none is open */
} DataAtExecParameter;

/* Statement-wide data-at-execution state, active between the SQLExecute that
 * returned SQL_NEED_DATA and the final SQLParamData that executes the query. */
typedef struct DataAtExecState {
    bool in_need_data;                 /* True while the NEED_DATA protocol is running */
    bool use_prepared;                 /* Execute via PQexecPrepared (true) or PQexecParams */
    int current_parameter_index;       /* 0-based param being filled, or the sentinel above */
    SQLULEN current_row;               /* Parameter set (row) being filled, for paramset > 1 */
    SQLULEN row_count;                 /* Number of parameter sets (snapshot of paramset_size) */
    DataAtExecParameter parameters[MAX_PARAMETERS];  /* Per-parameter accumulation */

    /* Tuple-bearing result sets collected one per executed parameter set, so a
     * data-at-execution array execution can chain them for SQLMoreResults
     * exactly like the ordinary array-execution path. Allocated at row_count
     * entries when the protocol begins. */
    PGresult **chained_results;
    int chained_count;
} DataAtExecState;

/* Storage size for a cursor name: the maximum name length plus room for the
 * terminating NUL. Auto-generated fallback names ("SQL_CUR<hex-pointer>") also
 * fit comfortably within this bound. */
#define CURSOR_NAME_BUFFER_SIZE (MAX_CURSOR_NAME_LENGTH + 1)

typedef struct OdbcStatement {
    unsigned int magic_number;         /* Must equal STATEMENT_MAGIC_NUMBER when valid */
    StatementState state;
    OdbcConnection *parent_connection;
    DiagnosticRecords diagnostics;
    char *sql_text;                    /* Heap-allocated SQL string from SQLPrepare/SQLExecDirect */
    char prepared_name[MAX_PREPARED_NAME_LENGTH]; /* Server-side prepared statement name */
    bool is_prepared;                  /* True if PQprepare was called for this statement */
    char *translated_sql;              /* Heap-allocated SQL with ? translated to $N (NULL if none) */
    int detected_param_count;          /* Number of ? markers found during translation */

    /* PostgreSQL parameter type OIDs captured from PQdescribePrepared at prepare
     * time, one per marker (index N-1 for $N). Retained separately because the
     * describe PGresult is cleared before parameters are converted, and the
     * execute path needs these to recognize large-object ("lo") parameters,
     * whose values must be sent as a large object's Oid rather than as bytea.
     * A value of InvalidOid (0) means "unknown / not captured". */
    Oid parameter_type_oids[MAX_PARAMETERS];
    int parameter_type_oid_count;

    /* ---- Large-object read-back state (SQLGetData on a "lo" column) ----
     *
     * A "lo" column holds the Oid of a large object; reading it as SQL_C_BINARY
     * opens that object and streams its bytes back, possibly across several
     * SQLGetData calls (chunked retrieval). This tracks the object currently
     * open for read and how many bytes remain, so successive SQLGetData calls
     * continue where the previous one stopped, exactly like the original driver.
     *
     * large_object_read_column is the 0-based result column being streamed, or
     * -1 when no read is in progress; a change of column or row resets the read.
     * large_object_read_bytes_remaining is the number of unread bytes left in the
     * object, or -1 before the size has been probed. */
    int large_object_read_fd;                  /* Open read descriptor, or -1 */
    int large_object_read_column;              /* 0-based column being read, or -1 */
    int large_object_read_row;                 /* Row index the open read belongs to */
    long long large_object_read_bytes_remaining;   /* Unread bytes left, or -1 if unprobed */

    /* Procedure-call metadata from the ODBC-escape analysis of the SQL text.
     * When is_procedure_call is true, bound arguments are sent to PostgreSQL
     * with the "unknown" type so it can resolve function overloads, and result
     * columns are copied back into OUT/INOUT parameter buffers after execution.
     * return_value_count is 1 for "{ ? = call ... }" (a leading return-value
     * parameter that is not sent as an argument), else 0. parameter_roles is
     * indexed by zero-based parameter position. */
    bool is_procedure_call;
    int return_value_count;
    QueryParamRole parameter_roles[MAX_PARAMETERS];

    /* Procedure-call structure captured by the parser: the function name, the
     * SELECT-wrapper prefix (in translated_sql, up to and including the "("),
     * and the argument list. The executor rebuilds the call from these once
     * bindings are known. */
    char procedure_name[256];
    QueryCallArgument call_arguments[QUERY_MAX_CALL_ARGUMENTS];
    int call_argument_count;

    /* Client-side SELECT-list column metadata overrides, populated when the
     * connection's Parse option is on. Indexed by result column position. Used
     * by SQLDescribeCol to report string-literal columns as VARCHAR(length). */
    QueryColumnOverride column_overrides[MAX_BOUND_COLUMNS];
    int column_override_count;
    PGresult *describe_result;         /* Result from PQdescribePrepared (for pre-execute metadata) */
    PGresult *deferred_prepare_error;  /* If PQprepare failed, the error is deferred to SQLExecute
                                        * time (matches original psqlodbc). NULL if prepare succeeded. */
    PGresult *current_result;          /* libpq result from last execution (NULL if none) */

    /* Multi-statement support: a single SQLExecDirect of "SELECT 1; SELECT 2"
     * produces several result sets. We collect them all here and step through
     * them with SQLMoreResults. pending_results owns the results NOT yet made
     * current; current_result above is the one being fetched. */
    PGresult **pending_results;        /* Queue of not-yet-consumed result sets */
    int pending_result_count;          /* Number of entries in pending_results */
    int pending_result_index;          /* Index of the next result to promote */

    /* True when the SQL text contained more than one top-level statement
     * (separated by ';'). Multi-statement queries cannot use the extended query
     * protocol as a single command, so each statement fragment is analyzed and
     * executed separately and the results are chained (see pending_results).
     * SQLMoreResults walks the chain. */
    bool is_multi_statement;

    /* The individual statement fragments of a multi-statement query, in order.
     * Populated only when is_multi_statement is true; each fragment is executed
     * with its own slice of the bound parameters. Owned by the statement and
     * freed on re-prepare, re-exec-direct, and free. */
    QueryStatementList statement_fragments;
    int affected_row_count;            /* Row count for INSERT/UPDATE/DELETE (-1 if unknown) */
    bool has_result_set;               /* True if the last execution produced rows (SELECT) */
    int current_row_position;          /* Cursor: -1 = before first row; 0..N-1 = row index */
    ParameterBinding parameter_bindings[MAX_PARAMETERS]; /* Bound parameter descriptors */
    int bound_parameter_count;         /* Number of currently bound parameters */

    /* The four automatically-allocated (implicit) ODBC descriptors, embedded in
     * the statement. They are thin views over the statement's backing stores:
     * the ARD/APD read and write column_bindings/parameter_bindings, the IRD
     * reports the executed result's column metadata (read-only), and the IPD
     * names parameter markers. See descriptor.h for the role semantics. */
    OdbcDescriptor implicit_app_row_descriptor;    /* ARD */
    OdbcDescriptor implicit_app_param_descriptor;  /* APD */
    OdbcDescriptor implicit_row_descriptor;        /* IRD (read-only) */
    OdbcDescriptor implicit_param_descriptor;      /* IPD */

    /* The descriptors currently in effect for this statement. They default to
     * the embedded implicit ARD/APD above but can be swapped for an explicitly
     * allocated descriptor via SQLSetStmtAttr(SQL_ATTR_APP_ROW_DESC /
     * APP_PARAM_DESC), and are reverted to the implicit ones when that explicit
     * descriptor is freed or the application sets SQL_NULL_HDESC. */
    OdbcDescriptor *active_app_row_descriptor;
    OdbcDescriptor *active_app_param_descriptor;

    ColumnBinding column_bindings[MAX_BOUND_COLUMNS];  /* Bound column descriptors for SQLFetch */
    int bound_column_count;            /* Number of currently bound columns */

    /* Per-column ARD SQL_DESC_PRECISION overrides (1-based column N stored at
     * index N-1). -1 means "unset"; the value drives interval fractional-second
     * precision (clamped to <= 9 at use — see type_mapping_interval_fraction).
     * The application sets these via SQLSetDescField on the ARD handle. */
    int column_precision_override[MAX_BOUND_COLUMNS];

    /* Statement attributes (set via SQLSetStmtAttr).
     * These persist across SQL_CLOSE but are reset when the handle is freed. */
    SQLULEN cursor_type;               /* SQL_CURSOR_FORWARD_ONLY or SQL_CURSOR_STATIC.
                                        * STATIC cursors are scrollable, served from the
                                        * fully-buffered client-side result set. */
    SQLULEN concurrency;               /* SQL_CONCUR_READ_ONLY (only supported type) */
    SQLULEN query_timeout_seconds;     /* 0 = no timeout */
    SQLULEN max_rows;                  /* 0 = no limit */
    SQLULEN noscan;                    /* SQL_NOSCAN_OFF (default) or SQL_NOSCAN_ON */
    bool metadata_id;                  /* Whether identifiers are treated as case-insensitive */

    /* Application-supplied cursor name (via SQLSetCursorName). Empty string
     * means "not set" — in that case SQLGetCursorName auto-generates a unique
     * "SQL_CUR<...>" name and stores it here so repeated calls stay stable. */
    char cursor_name[CURSOR_NAME_BUFFER_SIZE];

    /* ---- Block (row-array) cursor attributes ---- */

    /* Number of rows the application wants fetched into its bound arrays per
     * SQLFetch/SQLFetchScroll call (SQL_ATTR_ROW_ARRAY_SIZE). 1 = single-row
     * fetch (the default). When > 1 each bound column buffer is treated as an
     * array of that many elements, filled with consecutive rows of the rowset. */
    SQLULEN row_array_size;

    /* Application buffer that receives the number of rows actually placed into
     * the bound arrays by the most recent fetch (SQL_ATTR_ROWS_FETCHED_PTR).
     * NULL when the application has not requested the count. */
    SQLULEN *rows_fetched_ptr;

    /* Application array (row_array_size elements) that receives the per-row
     * status (SQL_ROW_SUCCESS / SQL_ROW_NOROW / ...) after each fetch
     * (SQL_ATTR_ROW_STATUS_PTR). NULL when not requested. */
    SQLUSMALLINT *row_status_ptr;

    /* Number of parameter-value sets (rows) the application supplies per
     * execution (SQL_ATTR_PARAMSET_SIZE). Defaults to 1 (a single set). When
     * greater than 1 with bound parameters, statement_execute /
     * statement_exec_direct run the statement once per set (see the array-
     * execution path), reading each bound parameter's value at the set's stride
     * index. */
    SQLULEN paramset_size;

    /* SQL_ATTR_PARAM_STATUS_PTR: application array (paramset_size elements) that
     * receives per-row status after an array execution (SQL_PARAM_SUCCESS /
     * SQL_PARAM_SUCCESS_WITH_INFO / SQL_PARAM_ERROR / SQL_PARAM_UNUSED). NULL
     * when the application did not request per-row status. */
    SQLUSMALLINT *param_status_ptr;

    /* SQL_ATTR_PARAMS_PROCESSED_PTR: application buffer that receives the number
     * of parameter sets processed by an array execution. NULL when not
     * requested. */
    SQLULEN *params_processed_ptr;

    /* SQL_ATTR_PARAM_OPERATION_PTR: application array that can mark individual
     * parameter sets to be ignored (SQL_PARAM_IGNORE). Accepted and stored; the
     * tests do not exercise it deeply. NULL when not set. */
    SQLUSMALLINT *param_operation_ptr;

    /* SQL_ATTR_PARAM_BIND_TYPE: SQL_PARAM_BIND_BY_COLUMN (0, column-wise arrays,
     * the mode the tests use) or a nonzero row-wise structure size. Stored as
     * given; column-wise is what the array-execution path implements. */
    SQLULEN param_bind_type;

    /* ---- Bookmark support ---- */

    /* SQL_ATTR_USE_BOOKMARKS mode. SQL_UB_OFF (the default) disables bookmarks;
     * SQL_UB_ON / SQL_UB_VARIABLE enable them. When off, the driver rejects any
     * attempt to read column 0 (the bookmark column) with SQLSTATE 07009. */
    SQLULEN use_bookmarks;

    /* SQL_ATTR_FETCH_BOOKMARK_PTR: application pointer to the bookmark value that
     * SQLFetchScroll(SQL_FETCH_BOOKMARK, offset) positions relative to. The
     * bookmark stored there is a 4-byte Int4 (see SC_MAKE_INT4_BOOKMARK). */
    SQLPOINTER fetch_bookmark_ptr;

    /* Column-0 (bookmark) binding. Column 0 never uses the 1-based
     * column_bindings[] array, so its binding lives here on its own. When
     * bookmark_bound is true, a single-row fetch writes the current row's
     * bookmark into bookmark_buffer (and bookmark_indicator, if provided) exactly
     * as a bound data column would receive its value.
     *
     * LIMITATION (Phase 1): bookmark_buffer is a single scalar, not an array
     * indexed by rowset element, so bound bookmarks are NOT populated on the
     * block-cursor path (row_array_size > 1). See populate_bound_rowset. */
    bool bookmark_bound;
    /* C type the application bound column 0 as: SQL_C_BOOKMARK (fixed 4-byte) or
     * SQL_C_VARBOOKMARK (variable-length). Selects the buffer-length policy when
     * writing the bookmark (see write_row_bookmark). */
    SQLSMALLINT bookmark_target_type;
    SQLPOINTER bookmark_buffer;          /* Application buffer receiving the bookmark */
    SQLLEN bookmark_buffer_length;       /* Size of bookmark_buffer in bytes */
    SQLLEN *bookmark_indicator;          /* Receives sizeof(Int4), or NULL */

    /* ---- Keyset (updatable) cursor support ---- */

    /* True when the application asked for an updatable cursor — either a
     * keyset-driven cursor type or a writable concurrency (ROWVER/LOCK/VALUES).
     * When set, an executed simple SELECT is rewritten to capture each row's
     * hidden ctid so SQLSetPos can build positioned UPDATE/DELETE statements. */
    bool is_updatable_cursor;

    /* 0-based index of the hidden ctid column appended to the result set, or
     * NO_HIDDEN_CTID_COLUMN when none was added. The public column count seen by
     * the application (SQLNumResultCols/SQLDescribeCol/SQLGetData/SQLColAttribute)
     * excludes this column so the ctid stays invisible. */
    int hidden_ctid_column_index;

    /* Per-row overlay (deleted flag + updated-value override), allocated to
     * PQntuples of current_result when an updatable cursor executes. NULL for a
     * non-updatable cursor. keyset_row_count records the allocation size. */
    KeysetRow *keyset_rows;
    int keyset_row_count;

    /* Table name parsed from the updatable cursor's SELECT ("... FROM <table>"),
     * used to build the positioned UPDATE/DELETE/INSERT. Empty when unknown. */
    char keyset_table_name[MAX_KEYSET_TABLE_NAME_LENGTH];

    /* First row of the rowset delivered by the most recent block fetch, and how
     * many rows it contained. SQLSetPos's row_number argument is 1-based WITHIN
     * this rowset, so it maps to base_row = keyset_rowset_first_row + (n-1).
     * For a single-row fetch these are current_row_position and 1. */
    int keyset_rowset_first_row;
    int keyset_rowset_size;

    /* Keyset-overlay snapshots keyed by savepoint name (see KeysetSavepoint).
     * Populated when the application issues "SAVEPOINT <name>" on ANY statement
     * of this connection while an updatable cursor is open, and consulted on
     * "ROLLBACK TO <name>". Kept on the owning cursor statement. */
    KeysetSavepoint keyset_savepoints[MAX_KEYSET_SAVEPOINTS];
    int keyset_savepoint_count;

    /* Data-at-execution streaming state (SQL_DATA_AT_EXEC). Populated when
     * SQLExecute detects at least one deferred-value parameter and returns
     * SQL_NEED_DATA; consumed by SQLParamData / SQLPutData. Inactive
     * (in_need_data == false) at all other times. */
    DataAtExecState data_at_exec;
} OdbcStatement;

/* ---- Bookmark encoding ----
 *
 * A bookmark is a 4-byte signed integer: a 1-based row index derived from the
 * 0-based client-side cursor position. Matching the original driver
 * (statement.h: BOOKMARK_SHIFT), a non-negative row R is stored as R + 1 so the
 * value is never zero (zero is reserved to mean "no bookmark"); negative
 * sentinels pass through unchanged. SQLFetchScroll(SQL_FETCH_BOOKMARK) reverses
 * the shift to recover the base 0-based row. */
#define BOOKMARK_ROW_INDEX_SHIFT 1

/* 4-byte signed bookmark value, matching the original driver's Int4 bookmark. */
typedef int32_t Int4Bookmark;

/* Encode a 0-based cursor row as its on-the-wire bookmark (row + 1 for real
 * rows; negative sentinels unchanged). */
static inline Int4Bookmark statement_make_int4_bookmark(int zero_based_row)
{
    return (zero_based_row < 0)
               ? (Int4Bookmark)zero_based_row
               : (Int4Bookmark)(zero_based_row + BOOKMARK_ROW_INDEX_SHIFT);
}

/* Decode an on-the-wire bookmark back to its 0-based cursor row. */
static inline int statement_resolve_int4_bookmark(Int4Bookmark bookmark)
{
    return (bookmark < 0)
               ? (int)bookmark
               : (int)(bookmark - BOOKMARK_ROW_INDEX_SHIFT);
}

/* ---- Public Interface ---- */

/*
 * Allocate a new statement handle linked to the given connection.
 * Stores the new handle in *output_handle.
 * Returns SQL_SUCCESS on success, SQL_ERROR on failure.
 */
SQLRETURN statement_allocate(OdbcConnection *connection, SQLHANDLE *output_handle);

/*
 * Free a statement handle. Deallocates any server-side prepared statement,
 * clears the PGresult, unlinks from the parent connection, and frees memory.
 * Returns SQL_SUCCESS on success, SQL_INVALID_HANDLE if the handle is invalid.
 */
SQLRETURN statement_free(SQLHANDLE handle);

/*
 * Prepare a SQL statement for later execution via SQLExecute.
 * Sends PQprepare to the server to create a server-side prepared statement.
 * The SQL text is stored on the handle for reference.
 * Returns SQL_SUCCESS on success, SQL_ERROR on failure (with diagnostic set).
 */
SQLRETURN statement_prepare(OdbcStatement *statement,
                            const char *sql_text,
                            SQLINTEGER text_length);

/*
 * Execute a previously prepared statement (must have called statement_prepare first).
 * Sends PQexecPrepared to the server and captures the result.
 * Returns SQL_SUCCESS on success, SQL_ERROR on failure (with diagnostic set).
 */
SQLRETURN statement_execute(OdbcStatement *statement);

/*
 * Execute a SQL statement directly without preparing it first.
 * Combines the prepare and execute steps using PQexec.
 * Returns SQL_SUCCESS on success, SQL_ERROR on failure (with diagnostic set).
 */
SQLRETURN statement_exec_direct(OdbcStatement *statement,
                                const char *sql_text,
                                SQLINTEGER text_length);

/*
 * Close the cursor (discard the current result set) without freeing the
 * statement handle. Resets state to PREPARED (if the statement was prepared)
 * or ALLOCATED (if it was a direct execution).
 * Returns SQL_SUCCESS on success, SQL_ERROR if no cursor is open.
 */
SQLRETURN statement_close_cursor(OdbcStatement *statement);

/*
 * Advance to the next result set in a multi-statement chain.
 *
 * Frees the current result set (if any) and promotes the next queued result
 * (see the pending_results fields) to become current, refreshing the
 * row-count, has-result-set flag, and cursor position for the new result.
 * Any stale client-side column-metadata overrides are cleared so SQLDescribeCol
 * reports the new result's columns.
 *
 * Returns:
 *   SQL_SUCCESS / SQL_SUCCESS_WITH_INFO - a next result became current
 *   SQL_ERROR                           - the next result reported an error
 *   SQL_NO_DATA                         - the chain is exhausted
 *
 * This backs SQLMoreResults. It does NOT free the whole pending queue (that is
 * done by statement_close_cursor); it only frees the result it replaces.
 */
SQLRETURN statement_promote_next_result(OdbcStatement *statement);

/*
 * Dispatch for SQLFreeStmt options:
 *   SQL_DROP         - Free the statement entirely (same as statement_free)
 *   SQL_CLOSE        - Close cursor / discard results
 *   SQL_UNBIND       - Reset column bindings (no-op in this implementation)
 *   SQL_RESET_PARAMS - Reset parameter bindings (no-op in this implementation)
 * Returns SQL_SUCCESS on success, SQL_ERROR on invalid option.
 */
SQLRETURN statement_free_stmt(OdbcStatement *statement, SQLUSMALLINT option);

/* ---- Keyset (updatable) cursor helpers ---- */

/*
 * Number of columns visible to the application. This is PQnfields of the current
 * result minus the hidden ctid column (if the cursor is updatable), so the ctid
 * we appended to the SELECT never appears in SQLNumResultCols / SQLDescribeCol /
 * SQLColAttribute / SQLGetData. Returns 0 when there is no result.
 */
int statement_public_column_count(const OdbcStatement *statement);

/*
 * Read a value for the current result set through the keyset overlay: if the row
 * has an updated-value override (from a positioned UPDATE/REFRESH), return that;
 * otherwise return the base PGresult value. Sets *is_null. Used by the fetch and
 * SQLGetData paths so an updated row shows its new value on re-fetch.
 */
const char *statement_row_value(const OdbcStatement *statement,
                                int row_index, int column_index, bool *is_null);

/*
 * Return true if the given 0-based base row was deleted via a positioned DELETE
 * and must be skipped by fetching and bookkeeping. Always false for a
 * non-updatable cursor.
 */
bool statement_row_is_deleted(const OdbcStatement *statement, int row_index);

/*
 * Perform an ODBC SQLSetPos operation (SQL_UPDATE / SQL_DELETE / SQL_REFRESH /
 * SQL_ADD / SQL_POSITION) on the current updatable cursor. Implemented in
 * results.c but declared here so the SQLSetPos entry point can delegate.
 */
SQLRETURN statement_set_pos(OdbcStatement *statement,
                            SQLSETPOSIROW row_number,
                            SQLUSMALLINT operation,
                            SQLUSMALLINT lock_type);

/*
 * Perform an ODBC SQLBulkOperations request (SQL_ADD, SQL_UPDATE_BY_BOOKMARK,
 * SQL_DELETE_BY_BOOKMARK, or SQL_FETCH_BY_BOOKMARK) on the current updatable
 * cursor. Unlike SQLSetPos, the bookmark-based operations identify their target
 * row(s) by the bookmark(s) held in the bound column-0 buffer rather than by a
 * rowset row number. Implemented in results.c; declared here so the
 * SQLBulkOperations entry point can delegate.
 */
SQLRETURN statement_bulk_operations(OdbcStatement *statement,
                                    SQLUSMALLINT operation);

/* ---- Data-at-execution (SQL_DATA_AT_EXEC) helpers ---- */

/*
 * Return true if a length/indicator value marks a data-at-execution parameter,
 * i.e. it is SQL_DATA_AT_EXEC or the SQL_LEN_DATA_AT_EXEC(length) form (any
 * value at or below SQL_LEN_DATA_AT_EXEC_OFFSET). Used by both the execute
 * paths (to decide whether to defer) and by parameter binding.
 */
bool statement_length_is_data_at_exec(SQLLEN length_or_indicator);

/*
 * Scan the statement's bound parameters for the given parameter set (row) and
 * return true if any of them was bound with a data-at-execution indicator. Used
 * by statement_execute / statement_exec_direct to decide whether to return
 * SQL_NEED_DATA instead of executing immediately.
 */
bool statement_row_has_data_at_exec(const OdbcStatement *statement, SQLULEN row_index);

/*
 * Enter the data-at-execution NEED_DATA state: reset the per-parameter
 * accumulation buffers and mark which parameters of the given row need data.
 * use_prepared records whether the eventual execution should use PQexecPrepared
 * (SQLExecute) or PQexecParams (SQLExecDirect). After this the statement is
 * ready for the SQLParamData / SQLPutData loop.
 */
void statement_begin_data_at_exec(OdbcStatement *statement, bool use_prepared,
                                  SQLULEN row_index);

/*
 * Release all data-at-execution accumulation buffers and reset the state to
 * inactive. Safe to call when the state is already inactive. Called when the
 * protocol completes, when the cursor is closed, and when the handle is freed.
 */
void statement_reset_data_at_exec(OdbcStatement *statement);

/*
 * Advance the data-at-execution walk to the next parameter still needing data.
 *
 * Backs SQLParamData. When another parameter of the current row needs data,
 * sets *value_pointer_out to that parameter's application value pointer (the
 * "token" the app passed as ParameterValuePtr) and returns SQL_NEED_DATA. When
 * every deferred parameter of the current row has been supplied, executes the
 * statement (advancing to the next parameter set for paramset binding) and
 * returns the execution result.
 */
SQLRETURN statement_param_data(OdbcStatement *statement, SQLPOINTER *value_pointer_out);

/*
 * Append one chunk of streamed data to the current data-at-execution parameter.
 *
 * Backs SQLPutData. Handles SQL_NTS (strlen), SQL_NULL_DATA (mark the parameter
 * null), and SQL_DEFAULT_PARAM. Rejects an unrecognized negative length with
 * SQLSTATE HY024, and an append after the parameter was set null with HY010.
 */
SQLRETURN statement_put_data(OdbcStatement *statement, SQLPOINTER data_pointer,
                             SQLLEN length_or_indicator);

/*
 * Stream a large-object column value back to the application (SQLGetData with
 * target type SQL_C_BINARY on a "lo" column).
 *
 * raw_oid_text is the column's text value: the decimal Oid of the large object.
 * The first call for a given row/column opens the object and probes its size;
 * successive calls continue reading where the previous one left off, so a value
 * larger than the buffer is retrieved in chunks. On each call the indicator (if
 * provided) receives the number of bytes still remaining before this call, per
 * the ODBC chunked-retrieval contract.
 *
 * row_index is the result-set row being read. The in-progress read state is
 * keyed on it so a chunked read resumes only for the same row/column: callers
 * pass current_row_position for SQLGetData, or the actual populated row
 * (first_row + offset) on the block-cursor bound-column path, where
 * current_row_position stays pinned until the rowset completes.
 *
 * Returns SQL_SUCCESS when the whole (remaining) value fit,
 * SQL_SUCCESS_WITH_INFO with SQLSTATE 01004 when it was truncated (more chunks
 * remain), SQL_NO_DATA when there is nothing left to read, or SQL_ERROR.
 */
SQLRETURN statement_get_large_object_data(OdbcStatement *statement,
                                          int column_index,
                                          int row_index,
                                          const char *raw_oid_text,
                                          SQLPOINTER target_value,
                                          SQLLEN buffer_length,
                                          SQLLEN *indicator_or_length);

/*
 * Reset any in-progress large-object read (close the descriptor and forget the
 * remaining-byte count). Called when the cursor moves to another row or column,
 * when the result set is cleared, and when the handle is freed, so a stale
 * descriptor from a previous read cannot bleed into the next one.
 */
void statement_reset_large_object_read(OdbcStatement *statement);

#endif /* PSQLODBC2_STATEMENT_H */
