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
    PGresult *current_result;          /* libpq result from last execution (NULL if none) */
    int affected_row_count;            /* Row count for INSERT/UPDATE/DELETE (-1 if unknown) */
    bool has_result_set;               /* True if the last execution produced rows (SELECT) */
    int current_row_position;          /* Cursor: -1 = before first row; 0..N-1 = row index */
    ParameterBinding parameter_bindings[MAX_PARAMETERS]; /* Bound parameter descriptors */
    int bound_parameter_count;         /* Number of currently bound parameters */
    ColumnBinding column_bindings[MAX_BOUND_COLUMNS];  /* Bound column descriptors for SQLFetch */
    int bound_column_count;            /* Number of currently bound columns */

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
 * Dispatch for SQLFreeStmt options:
 *   SQL_DROP         - Free the statement entirely (same as statement_free)
 *   SQL_CLOSE        - Close cursor / discard results
 *   SQL_UNBIND       - Reset column bindings (no-op in this implementation)
 *   SQL_RESET_PARAMS - Reset parameter bindings (no-op in this implementation)
 * Returns SQL_SUCCESS on success, SQL_ERROR on invalid option.
 */
SQLRETURN statement_free_stmt(OdbcStatement *statement, SQLUSMALLINT option);

#endif /* PSQLODBC2_STATEMENT_H */
