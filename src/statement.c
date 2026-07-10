/*-------------------------------------------------------------------------
 *
 * statement.c
 *	  ODBC Statement handle lifecycle and execution
 *
 * Copyright (c) 2026, Dave Cramer
 *
 *
 * IDENTIFICATION
 *	  src/statement.c
 *
 *-------------------------------------------------------------------------
 */
#include "statement.h"
#include "connection.h"
#include "error_mapping.h"
#include "query_parser.h"
#include "type_mapping.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ---- Internal Helpers ---- */

/*
 * Populate a per-position cast-suffix array from the statement's parameter
 * bindings, for use by query_translate_markers.
 *
 * casts_out must have MAX_PARAMETERS entries. Each entry is set to the cast
 * suffix (e.g. "::int4") implied by the bound parameter's SQL type, or NULL
 * when the parameter is unbound or needs no cast. The returned strings are
 * static constants owned by type_mapping and must not be freed.
 *
 * Applying casts at ExecDirect time — where bindings are known before the SQL
 * is translated — lets PostgreSQL interpret each value as the SQL type the
 * application declared, rather than as an untyped string literal. This mirrors
 * the original psqlodbc's use of sqltype_to_pgcast().
 */
static void build_parameter_casts(const OdbcStatement *statement,
                                  const char *casts_out[MAX_PARAMETERS])
{
    for (int index = 0; index < MAX_PARAMETERS; index++) {
        if (statement->parameter_bindings[index].is_bound) {
            casts_out[index] =
                type_mapping_get_param_cast(statement->parameter_bindings[index].sql_type);
        } else {
            casts_out[index] = NULL;
        }
    }
}

/*
 * Resolve the actual byte length of a SQL text argument.
 * Handles SQL_NTS (null-terminated string) by computing strlen.
 */
static size_t resolve_text_length(const char *text, SQLINTEGER declared_length)
{
    if (!text) {
        return 0;
    }
    if (declared_length == SQL_NTS) {
        return strlen(text);
    }
    if (declared_length < 0) {
        return 0;
    }
    return (size_t)declared_length;
}

/*
 * Clear the current result from the statement, freeing the PGresult if present.
 * Resets result-related fields but does NOT change the statement state — the
 * caller is responsible for updating state after calling this.
 */
static void clear_current_result(OdbcStatement *statement)
{
    if (statement->current_result) {
        PQclear(statement->current_result);
        statement->current_result = NULL;
    }
    if (statement->describe_result) {
        PQclear(statement->describe_result);
        statement->describe_result = NULL;
    }
    statement->affected_row_count = -1;
    statement->has_result_set = false;
    statement->current_row_position = -1;
}

/*
 * Process the PGresult from a query execution and update the statement
 * accordingly. This is the shared handler for both direct execution and
 * prepared statement execution.
 *
 * On PGRES_TUPLES_OK: retains the PGresult on the statement (for later fetch),
 * sets has_result_set=true, transitions to HAS_CURSOR state.
 *
 * On PGRES_COMMAND_OK: extracts the affected row count, clears the PGresult
 * (no data to retain), transitions to EXECUTED state.
 *
 * On error: adds a diagnostic record and returns SQL_ERROR.
 */
static SQLRETURN handle_execution_result(OdbcStatement *statement, PGresult *result)
{
    if (!result) {
        /* PQexec/PQexecPrepared returned NULL — usually means connection lost */
        diagnostics_add_record(&statement->diagnostics,
                               "08S01",  /* Communication link failure */
                               0,
                               "Query execution failed: NULL result from libpq (connection may be lost).");
        return SQL_ERROR;
    }

    ExecStatusType status = PQresultStatus(result);

    switch (status) {
    case PGRES_TUPLES_OK:
        /* SELECT or RETURNING query — result set available for fetching.
         * Keep the PGresult alive; the results module will read from it. */
        statement->current_result = result;
        statement->has_result_set = true;
        statement->affected_row_count = PQntuples(result);
        statement->state = STATEMENT_STATE_HAS_CURSOR;

        /* If the connection captured NOTICE messages during execution, promote
         * them to ODBC diagnostic records and return SQL_SUCCESS_WITH_INFO. */
        if (statement->parent_connection &&
            statement->parent_connection->notice_count > 0) {
            OdbcConnection *conn = statement->parent_connection;
            for (int i = 0; i < conn->notice_count; i++) {
                diagnostics_add_record(&statement->diagnostics,
                                       "00000",  /* Notice — matches original psqlodbc behavior */
                                       0,
                                       conn->captured_notices[i]);
            }
            connection_clear_notices(conn);
            return SQL_SUCCESS_WITH_INFO;
        }
        return SQL_SUCCESS;

    case PGRES_COMMAND_OK: {
        /* DML (INSERT/UPDATE/DELETE) or DDL — no result rows.
         * Extract affected row count from the status string. */
        const char *affected_rows_text = PQcmdTuples(result);
        if (affected_rows_text && affected_rows_text[0] != '\0') {
            statement->affected_row_count = atoi(affected_rows_text);
        } else {
            statement->affected_row_count = 0;
        }
        statement->has_result_set = false;
        statement->state = STATEMENT_STATE_EXECUTED;
        PQclear(result);

        /* Promote any captured NOTICE messages to diagnostics */
        if (statement->parent_connection &&
            statement->parent_connection->notice_count > 0) {
            OdbcConnection *conn = statement->parent_connection;
            for (int i = 0; i < conn->notice_count; i++) {
                diagnostics_add_record(&statement->diagnostics,
                                       "00000",  /* Notice — matches original psqlodbc behavior */
                                       0,
                                       conn->captured_notices[i]);
            }
            connection_clear_notices(conn);
            return SQL_SUCCESS_WITH_INFO;
        }
        return SQL_SUCCESS;
    }

    case PGRES_FATAL_ERROR: {
        /* Extract the actual SQLSTATE from PostgreSQL (e.g., "42601" for syntax
         * error, "23505" for unique violation) instead of generic "HY000". */
        diagnostics_clear(&statement->diagnostics);
        error_add_diagnostic_from_result_ctx(&statement->diagnostics, result, "HY000",
                                             "Error while executing the query");
        PQclear(result);

        /* Mark the transaction as failed so subsequent commands are rejected
         * until the application issues a ROLLBACK via SQLEndTran. */
        if (statement->parent_connection &&
            statement->parent_connection->transaction_state == TRANSACTION_STATE_ACTIVE) {
            statement->parent_connection->transaction_state = TRANSACTION_STATE_FAILED;
        }
        return SQL_ERROR;
    }

    default: {
        /* Unexpected result status — extract whatever error info is available */
        diagnostics_clear(&statement->diagnostics);
        error_add_diagnostic_from_result(&statement->diagnostics, result, "HY000");
        PQclear(result);
        return SQL_ERROR;
    }
    }
}

/*
 * Deallocate the server-side prepared statement from PostgreSQL.
 * Called during statement_free to avoid accumulating dead prepared statements
 * on long-lived connections.
 */
static void deallocate_server_prepared_statement(OdbcStatement *statement)
{
    if (!statement->is_prepared || statement->prepared_name[0] == '\0') {
        return;
    }

    /* Only deallocate if the connection is still active */
    if (!statement->parent_connection ||
        statement->parent_connection->state != CONNECTION_STATE_CONNECTED ||
        !statement->parent_connection->libpq_connection) {
        return;
    }

    /* Build and execute DEALLOCATE command.
     * We ignore errors here — the statement is being freed regardless. */
    char deallocate_command[MAX_PREPARED_NAME_LENGTH + 16];
    snprintf(deallocate_command, sizeof(deallocate_command),
             "DEALLOCATE %s", statement->prepared_name);

    PGresult *result = PQexec(statement->parent_connection->libpq_connection,
                              deallocate_command);
    if (result) {
        PQclear(result);
    }
}

/* ---- Public Interface ---- */

SQLRETURN statement_allocate(OdbcConnection *connection, SQLHANDLE *output_handle)
{
    if (!connection || !output_handle) {
        return SQL_ERROR;
    }

    OdbcStatement *statement = calloc(1, sizeof(OdbcStatement));
    if (!statement) {
        *output_handle = SQL_NULL_HSTMT;
        return SQL_ERROR;
    }

    statement->magic_number = STATEMENT_MAGIC_NUMBER;
    statement->state = STATEMENT_STATE_ALLOCATED;
    statement->parent_connection = connection;
    statement->affected_row_count = -1;
    statement->current_row_position = -1;

    /* Initialize statement attributes to ODBC-specified defaults */
    statement->cursor_type = SQL_CURSOR_FORWARD_ONLY;
    statement->concurrency = SQL_CONCUR_READ_ONLY;
    statement->query_timeout_seconds = 0;
    statement->max_rows = 0;
    statement->noscan = SQL_NOSCAN_OFF;
    statement->metadata_id = false;

    if (!connection_add_statement(connection, statement)) {
        /* Connection's statement array is full */
        free(statement);
        *output_handle = SQL_NULL_HSTMT;
        diagnostics_add_record(&connection->diagnostics,
                               "HY014",  /* Limit on the number of handles exceeded */
                               0,
                               "Maximum number of statement handles per connection exceeded.");
        return SQL_ERROR;
    }

    *output_handle = (SQLHANDLE)statement;
    return SQL_SUCCESS;
}

SQLRETURN statement_free(SQLHANDLE handle)
{
    OdbcStatement *statement = (OdbcStatement *)handle;

    if (!statement) {
        return SQL_INVALID_HANDLE;
    }

    if (statement->magic_number != STATEMENT_MAGIC_NUMBER) {
        return SQL_INVALID_HANDLE;
    }

    /* Deallocate server-side prepared statement if one exists */
    deallocate_server_prepared_statement(statement);

    /* Release any deferred prepare error result */
    if (statement->deferred_prepare_error) {
        PQclear(statement->deferred_prepare_error);
        statement->deferred_prepare_error = NULL;
    }

    /* Clear the PGresult if any */
    clear_current_result(statement);

    /* Clear all parameter bindings */
    parameter_unbind_all(statement->parameter_bindings, &statement->bound_parameter_count);

    /* Clear all column bindings */
    column_binding_unbind_all(statement->column_bindings, &statement->bound_column_count);

    /* Unlink from parent connection */
    if (statement->parent_connection) {
        connection_remove_statement(statement->parent_connection, statement);
    }

    /* Free heap-allocated SQL text and translated SQL */
    free(statement->sql_text);
    statement->sql_text = NULL;
    free(statement->translated_sql);
    statement->translated_sql = NULL;

    /* Clear diagnostics */
    diagnostics_clear(&statement->diagnostics);

    /* Poison the magic number to detect use-after-free */
    statement->magic_number = 0;
    free(statement);

    return SQL_SUCCESS;
}

SQLRETURN statement_prepare(OdbcStatement *statement,
                            const char *sql_text,
                            SQLINTEGER text_length)
{
    if (!statement) {
        return SQL_ERROR;
    }

    /* Verify the parent connection is active */
    if (!statement->parent_connection ||
        statement->parent_connection->state != CONNECTION_STATE_CONNECTED ||
        !statement->parent_connection->libpq_connection) {
        diagnostics_add_record(&statement->diagnostics,
                               "08003",  /* Connection does not exist */
                               0,
                               "Cannot prepare: connection is not active.");
        return SQL_ERROR;
    }

    /* If re-preparing, deallocate the previous server-side prepared statement */
    deallocate_server_prepared_statement(statement);

    /* Close any existing result */
    clear_current_result(statement);

    /* Resolve and store the SQL text */
    size_t actual_length = resolve_text_length(sql_text, text_length);
    if (actual_length == 0) {
        diagnostics_add_record(&statement->diagnostics,
                               "HY009",  /* Invalid use of null pointer */
                               0,
                               "Cannot prepare: SQL text is NULL or empty.");
        return SQL_ERROR;
    }

    free(statement->sql_text);
    statement->sql_text = malloc(actual_length + 1);
    if (!statement->sql_text) {
        diagnostics_add_record(&statement->diagnostics,
                               "HY001",  /* Memory allocation error */
                               0,
                               "Cannot prepare: memory allocation failed for SQL text.");
        return SQL_ERROR;
    }
    memcpy(statement->sql_text, sql_text, actual_length);
    statement->sql_text[actual_length] = '\0';

    /* Translate ODBC '?' parameter markers to PostgreSQL '$N' markers.
     * At SQLPrepare time the application has not yet bound parameters (binding
     * follows prepare), so no cast information is available; PostgreSQL infers
     * the parameter types from the prepared statement instead. */
    free(statement->translated_sql);
    statement->translated_sql = query_translate_markers(statement->sql_text,
                                                        &statement->detected_param_count,
                                                        NULL, 0);
    if (!statement->translated_sql) {
        diagnostics_add_record(&statement->diagnostics,
                               "HY001",  /* Memory allocation error */
                               0,
                               "Cannot prepare: memory allocation failed for parameter translation.");
        return SQL_ERROR;
    }

    /* Generate a unique server-side prepared statement name */
    snprintf(statement->prepared_name, MAX_PREPARED_NAME_LENGTH,
             "_psqlodbc2_stmt_%d", statement->parent_connection->next_statement_id++);

    /* Send PQprepare to the server using the translated SQL (with $N markers) */
    PGconn *libpq_connection = statement->parent_connection->libpq_connection;
    PGresult *prepare_result = PQprepare(libpq_connection,
                                         statement->prepared_name,
                                         statement->translated_sql,
                                         0,    /* number of parameters */
                                         NULL  /* let server infer parameter types */);

    if (!prepare_result || PQresultStatus(prepare_result) != PGRES_COMMAND_OK) {
        /* Defer parse/prepare errors to SQLExecute time. ODBC applications
         * commonly SQLPrepare, then SQLBindParameter, then SQLExecute, and
         * expect the diagnostic to surface at execution (the original psqlodbc
         * driver behaves this way with deferred/server-side prepare). We keep
         * the failing PGresult so SQLExecute can report it with the proper
         * "Error while preparing parameters" context. */
        if (statement->deferred_prepare_error) {
            PQclear(statement->deferred_prepare_error);
        }
        statement->deferred_prepare_error = prepare_result;  /* may be NULL */
        statement->prepared_name[0] = '\0';
        /* Mark as prepared so SQLExecute runs and emits the deferred error
         * instead of a "function sequence" error. */
        statement->is_prepared = true;
        statement->state = STATEMENT_STATE_PREPARED;
        return SQL_SUCCESS;
    }

    if (statement->deferred_prepare_error) {
        PQclear(statement->deferred_prepare_error);
        statement->deferred_prepare_error = NULL;
    }
    PQclear(prepare_result);
    statement->is_prepared = true;
    statement->state = STATEMENT_STATE_PREPARED;

    /* Call PQdescribePrepared to get column metadata before execution.
     * This enables SQLNumResultCols and SQLDescribeCol to work after
     * SQLPrepare but before SQLExecute (required by ODBC spec). */
    PGresult *describe_result = PQdescribePrepared(libpq_connection,
                                                    statement->prepared_name);
    if (describe_result && PQresultStatus(describe_result) == PGRES_COMMAND_OK) {
        statement->describe_result = describe_result;
    } else {
        /* If describe fails, we still consider the prepare successful —
         * metadata just won't be available until after execution. */
        if (describe_result) {
            PQclear(describe_result);
        }
    }

    return SQL_SUCCESS;
}

SQLRETURN statement_execute(OdbcStatement *statement)
{
    if (!statement) {
        return SQL_ERROR;
    }

    /* SQLExecute requires the statement to have been prepared */
    if (!statement->is_prepared) {
        diagnostics_add_record(&statement->diagnostics,
                               "HY010",  /* Function sequence error */
                               0,
                               "Cannot execute: statement has not been prepared. Call SQLPrepare first.");
        return SQL_ERROR;
    }

    /* Verify the parent connection is active */
    if (!statement->parent_connection ||
        statement->parent_connection->state != CONNECTION_STATE_CONNECTED ||
        !statement->parent_connection->libpq_connection) {
        diagnostics_add_record(&statement->diagnostics,
                               "08003",  /* Connection does not exist */
                               0,
                               "Cannot execute: connection is not active.");
        return SQL_ERROR;
    }

    /* If SQLPrepare failed to parse the statement, the error was deferred to
     * now (see statement_prepare). Report it with the preparing-parameters
     * context that ODBC applications expect. */
    if (statement->deferred_prepare_error) {
        diagnostics_clear(&statement->diagnostics);
        error_add_diagnostic_from_result_ctx(&statement->diagnostics,
                                             statement->deferred_prepare_error,
                                             "42000",
                                             "Error while preparing parameters");
        PQclear(statement->deferred_prepare_error);
        statement->deferred_prepare_error = NULL;
        return SQL_ERROR;
    }

    /* If autocommit is OFF, ensure we're inside a transaction — UNLESS the
     * command is transaction-exempt (e.g., VACUUM). Use the original sql_text
     * for detection since translated_sql might have $N markers but same keywords. */
    const char *sql_for_check = statement->sql_text ? statement->sql_text : "";
    if (!query_is_transaction_exempt(sql_for_check)) {
        SQLRETURN txn_result = connection_ensure_transaction(statement->parent_connection);
        if (txn_result != SQL_SUCCESS) {
            diagnostics_add_record(&statement->diagnostics,
                                   "HY000",
                                   0,
                                   "Failed to begin implicit transaction.");
            return SQL_ERROR;
        }
    }

    /* Close any previous result from a prior execution */
    clear_current_result(statement);

    PGconn *libpq_connection = statement->parent_connection->libpq_connection;
    PGresult *result = NULL;

    if (statement->bound_parameter_count > 0 && statement->detected_param_count > 0) {
        /* Build parameter arrays from bound values for PQexecPrepared.
         * Cap the parameter count to what the SQL actually requires — apps may
         * have stale bindings from a previous prepare that used more params. */
        const char **param_values = NULL;
        int *param_lengths = NULL;
        int *param_formats = NULL;
        int param_count = 0;

        SQLRETURN build_result = parameter_build_libpq_arrays(
            statement->parameter_bindings,
            statement->bound_parameter_count,
            &param_values, &param_lengths, &param_formats, &param_count);

        if (build_result != SQL_SUCCESS) {
            diagnostics_add_record(&statement->diagnostics,
                                   "HY001",  /* Memory allocation error */
                                   0,
                                   "Failed to build parameter arrays for execution.");
            return SQL_ERROR;
        }

        /* Only send as many params as the prepared SQL expects */
        int effective_param_count = param_count;
        if (effective_param_count > statement->detected_param_count) {
            effective_param_count = statement->detected_param_count;
        }

        result = PQexecPrepared(libpq_connection,
                                statement->prepared_name,
                                effective_param_count,
                                param_values,
                                param_lengths,
                                param_formats,
                                0  /* result format: text */);

        parameter_free_libpq_arrays(param_values, param_lengths, param_formats, param_count);
    } else {
        /* No parameters bound or no parameter markers — execute without parameters */
        result = PQexecPrepared(libpq_connection,
                                statement->prepared_name,
                                0,     /* number of parameters */
                                NULL,  /* parameter values */
                                NULL,  /* parameter lengths */
                                NULL,  /* parameter formats */
                                0      /* result format: text */);
    }

    return handle_execution_result(statement, result);
}

SQLRETURN statement_exec_direct(OdbcStatement *statement,
                                const char *sql_text,
                                SQLINTEGER text_length)
{
    if (!statement) {
        return SQL_ERROR;
    }

    /* Verify the parent connection is active */
    if (!statement->parent_connection ||
        statement->parent_connection->state != CONNECTION_STATE_CONNECTED ||
        !statement->parent_connection->libpq_connection) {
        diagnostics_add_record(&statement->diagnostics,
                               "08003",  /* Connection does not exist */
                               0,
                               "Cannot execute: connection is not active.");
        return SQL_ERROR;
    }

    /* If this statement previously had a server-side prepared statement,
     * deallocate it since ExecDirect does not use prepared statements */
    deallocate_server_prepared_statement(statement);

    /* Drop any deferred prepare error from a prior SQLPrepare — ExecDirect
     * replaces the statement text entirely. */
    if (statement->deferred_prepare_error) {
        PQclear(statement->deferred_prepare_error);
        statement->deferred_prepare_error = NULL;
    }

    /* Close any existing result */
    clear_current_result(statement);

    /* Resolve and store the SQL text */
    size_t actual_length = resolve_text_length(sql_text, text_length);
    if (actual_length == 0) {
        diagnostics_add_record(&statement->diagnostics,
                               "HY009",  /* Invalid use of null pointer */
                               0,
                               "Cannot execute: SQL text is NULL or empty.");
        return SQL_ERROR;
    }

    free(statement->sql_text);
    statement->sql_text = malloc(actual_length + 1);
    if (!statement->sql_text) {
        diagnostics_add_record(&statement->diagnostics,
                               "HY001",  /* Memory allocation error */
                               0,
                               "Cannot execute: memory allocation failed for SQL text.");
        return SQL_ERROR;
    }
    memcpy(statement->sql_text, sql_text, actual_length);
    statement->sql_text[actual_length] = '\0';

    /* Translate ODBC '?' parameter markers to PostgreSQL '$N' markers.
     * Bindings are already known at ExecDirect time, so we append type casts
     * ($N::int4, etc.) derived from each bound parameter's declared SQL type. */
    const char *parameter_casts[MAX_PARAMETERS];
    build_parameter_casts(statement, parameter_casts);

    free(statement->translated_sql);
    statement->translated_sql = query_translate_markers(statement->sql_text,
                                                        &statement->detected_param_count,
                                                        parameter_casts,
                                                        MAX_PARAMETERS);
    if (!statement->translated_sql) {
        diagnostics_add_record(&statement->diagnostics,
                               "HY001",  /* Memory allocation error */
                               0,
                               "Cannot execute: memory allocation failed for parameter translation.");
        return SQL_ERROR;
    }

    /* If autocommit is OFF, ensure we're inside a transaction — UNLESS the
     * command is transaction-exempt (e.g., VACUUM, CREATE DATABASE). Those
     * commands cannot run inside a transaction block. */
    if (!query_is_transaction_exempt(statement->translated_sql)) {
        SQLRETURN txn_result = connection_ensure_transaction(statement->parent_connection);
        if (txn_result != SQL_SUCCESS) {
            diagnostics_add_record(&statement->diagnostics,
                                   "HY000",
                                   0,
                                   "Failed to begin implicit transaction.");
            return SQL_ERROR;
        }
    }

    /* Direct execution does not create a server-side prepared statement */
    statement->is_prepared = false;
    statement->prepared_name[0] = '\0';

    PGconn *libpq_connection = statement->parent_connection->libpq_connection;
    PGresult *result = NULL;

    /* Only use PQexecParams if the SQL actually contains parameter markers.
     * The bound_parameter_count reflects what the app bound, but the actual
     * SQL may have fewer (or zero) markers. For example, after executing a
     * prepared statement with params, the app might do ExecDirect on a plain
     * SQL string without resetting params — we must not send params in that case. */
    if (statement->detected_param_count > 0 && statement->bound_parameter_count > 0) {
        /* Use PQexecParams when the SQL has parameter markers and params are bound */
        const char **param_values = NULL;
        int *param_lengths = NULL;
        int *param_formats = NULL;
        int param_count = 0;

        SQLRETURN build_result = parameter_build_libpq_arrays(
            statement->parameter_bindings,
            statement->bound_parameter_count,
            &param_values, &param_lengths, &param_formats, &param_count);

        if (build_result != SQL_SUCCESS) {
            diagnostics_add_record(&statement->diagnostics,
                                   "HY001",  /* Memory allocation error */
                                   0,
                                   "Failed to build parameter arrays for execution.");
            return SQL_ERROR;
        }

        result = PQexecParams(libpq_connection,
                              statement->translated_sql,
                              param_count,
                              NULL,          /* Let PostgreSQL infer parameter types */
                              param_values,
                              param_lengths,
                              param_formats,
                              0              /* Result format: text */);

        parameter_free_libpq_arrays(param_values, param_lengths, param_formats, param_count);
    } else {
        /* No parameter markers in SQL — use simple PQexec */
        result = PQexec(libpq_connection, statement->translated_sql);
    }

    return handle_execution_result(statement, result);
}

SQLRETURN statement_close_cursor(OdbcStatement *statement)
{
    if (!statement) {
        return SQL_ERROR;
    }

    /* Clear the current result regardless of state — SQL_CLOSE is always
     * valid per ODBC spec (it's a no-op if there's nothing to close) */
    clear_current_result(statement);

    /* Reset state: if the statement was prepared, go back to PREPARED;
     * otherwise go back to ALLOCATED */
    if (statement->is_prepared) {
        statement->state = STATEMENT_STATE_PREPARED;
    } else {
        statement->state = STATEMENT_STATE_ALLOCATED;
    }

    return SQL_SUCCESS;
}

SQLRETURN statement_free_stmt(OdbcStatement *statement, SQLUSMALLINT option)
{
    if (!statement) {
        return SQL_INVALID_HANDLE;
    }

    switch (option) {
    case SQL_DROP:
        return statement_free((SQLHANDLE)statement);

    case SQL_CLOSE:
        return statement_close_cursor(statement);

    case SQL_UNBIND:
        column_binding_unbind_all(statement->column_bindings, &statement->bound_column_count);
        return SQL_SUCCESS;

    case SQL_RESET_PARAMS:
        parameter_unbind_all(statement->parameter_bindings, &statement->bound_parameter_count);
        return SQL_SUCCESS;

    default:
        diagnostics_add_record(&statement->diagnostics,
                               "HY092",  /* Invalid attribute/option identifier */
                               0,
                               "Invalid option passed to SQLFreeStmt.");
        return SQL_ERROR;
    }
}
