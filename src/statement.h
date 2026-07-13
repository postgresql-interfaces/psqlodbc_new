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

#include <stdbool.h>
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

/* Magic number for descriptor handles. "DSC2" in ASCII = 0x44534332.
 * The driver exposes only the implicit parameter descriptor (IPD), embedded in
 * each statement, so applications can set parameter names via SQLSetDescField. */
#define DESCRIPTOR_MAGIC_NUMBER 0x44534332

/* Distinguishes which embedded descriptor an OdbcDescriptor handle refers to,
 * so SQLSetDescField can route a field to the right target (parameter names for
 * the IPD, per-column precision for the ARD). */
typedef enum DescriptorRole {
    DESCRIPTOR_ROLE_IMPLICIT_PARAM,   /* IPD: names parameter markers */
    DESCRIPTOR_ROLE_APP_ROW           /* ARD: per-column result formatting (precision) */
} DescriptorRole;

/* ---- Embedded Descriptor (IPD / ARD) ----
 *
 * A minimal descriptor. As the IPD it lets an application name parameter markers
 * via SQLSetDescField(hIpd, N, SQL_DESC_NAME, ...); as the ARD it carries the
 * per-column SQL_DESC_PRECISION override used when formatting result values
 * (e.g. interval fractional-second precision). Both are embedded in the owning
 * statement (never allocated separately); owner points back so descriptor calls
 * can update the statement's bindings/overrides. */
typedef struct OdbcDescriptor {
    unsigned int magic_number;      /* DESCRIPTOR_MAGIC_NUMBER when valid */
    DescriptorRole role;            /* Which descriptor this handle represents */
    struct OdbcStatement *owner;    /* Statement this descriptor belongs to */
} OdbcDescriptor;

/* Maximum length for server-side prepared statement names.
 * Names are auto-generated as "_psqlodbc2_stmt_<counter>". */
#define MAX_PREPARED_NAME_LENGTH 64

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
    OdbcDescriptor implicit_param_descriptor; /* IPD handle returned by SQLGetStmtAttr */
    OdbcDescriptor app_row_descriptor;        /* ARD handle returned by SQLGetStmtAttr */
    ColumnBinding column_bindings[MAX_BOUND_COLUMNS];  /* Bound column descriptors for SQLFetch */
    int bound_column_count;            /* Number of currently bound columns */

    /* Per-column ARD SQL_DESC_PRECISION overrides (1-based column N stored at
     * index N-1). -1 means "unset"; the value drives interval fractional-second
     * precision (clamped to <= 9 at use — see type_mapping_interval_fraction).
     * The application sets these via SQLSetDescField on the ARD handle. */
    int column_precision_override[MAX_BOUND_COLUMNS];

    /* Statement attributes (set via SQLSetStmtAttr).
     * These persist across SQL_CLOSE but are reset when the handle is freed. */
    SQLULEN cursor_type;               /* SQL_CURSOR_FORWARD_ONLY (only supported type) */
    SQLULEN concurrency;               /* SQL_CONCUR_READ_ONLY (only supported type) */
    SQLULEN query_timeout_seconds;     /* 0 = no timeout */
    SQLULEN max_rows;                  /* 0 = no limit */
    SQLULEN noscan;                    /* SQL_NOSCAN_OFF (default) or SQL_NOSCAN_ON */
    bool metadata_id;                  /* Whether identifiers are treated as case-insensitive */
} OdbcStatement;

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

#endif /* PSQLODBC2_STATEMENT_H */
