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
#include "results.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ---- Internal Helpers ---- */

/* Execute every fragment of a multi-statement query and chain the results.
 * Defined below; forward-declared because statement_execute (earlier in the
 * file) also dispatches to it. */
static SQLRETURN execute_multi_statement(OdbcStatement *statement);

/* Post-process a plain "CALL proc(...)" result: copy OUT/INOUT values back and,
 * with FetchRefcursors enabled, expose returned cursors as result sets. Defined
 * below; forward-declared for the execute/exec-direct paths above it. */
static SQLRETURN handle_call_result(OdbcStatement *statement, SQLRETURN execution_result);
static bool translated_sql_is_call(const OdbcStatement *statement);

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
 * Build the query-parser options for this statement from its connection.
 * standard_conforming_strings affects backslash handling inside ordinary
 * string literals; ms_jet enables the MS Access boolean-comparison rewrite.
 * Both the single-statement analysis and the multi-statement splitter/executor
 * must interpret string literals identically, so they share this helper.
 */
static QueryParseOptions build_parse_options(const OdbcStatement *statement)
{
    QueryParseOptions options = {
        .standard_conforming_strings =
            connection_standard_conforming_strings(statement->parent_connection),
        .ms_jet = statement->parent_connection
                      ? statement->parent_connection->ms_jet
                      : false,
    };
    return options;
}

/*
 * Analyze the statement's SQL text and store the transformed SQL plus
 * procedure-call metadata on the statement. When apply_casts is true (used at
 * ExecDirect time, where parameter bindings are already known), ordinary
 * parameter markers receive SQL-type casts derived from the bindings; at
 * Prepare time bindings are not yet available so casts are omitted.
 *
 * Reads standard_conforming_strings and MS Access mode from the connection so
 * the parser interprets string literals and boolean predicates correctly.
 * Returns SQL_SUCCESS, or SQL_ERROR (with a diagnostic) on allocation failure.
 */
static SQLRETURN analyze_and_store(OdbcStatement *statement, bool apply_casts)
{
    QueryParseOptions options = build_parse_options(statement);

    const char *parameter_casts[MAX_PARAMETERS];
    const char *const *casts_argument = NULL;
    int casts_count = 0;
    if (apply_casts) {
        build_parameter_casts(statement, parameter_casts);
        casts_argument = parameter_casts;
        casts_count = MAX_PARAMETERS;
    }

    QueryAnalysis analysis;
    if (!query_analyze(statement->sql_text, &options,
                       casts_argument, casts_count, &analysis)) {
        diagnostics_add_record(&statement->diagnostics,
                               "HY001",  /* Memory allocation error */
                               0,
                               "Memory allocation failed during query analysis.");
        return SQL_ERROR;
    }

    free(statement->translated_sql);
    statement->translated_sql = analysis.transformed_sql;
    statement->detected_param_count = analysis.parameter_count;
    statement->is_procedure_call = analysis.is_procedure_call;
    statement->return_value_count = analysis.return_value_count;
    /* The statement's parameter_roles[MAX_PARAMETERS] and the analysis's
     * parameter_roles[QUERY_MAX_PARAMETERS] must have identical extents, or this
     * memcpy would over-read the source / under-fill the destination. The two
     * constants live in different headers (parameter.h and query_parser.h) to
     * avoid a dependency cycle, so pin them together at compile time. */
    _Static_assert(MAX_PARAMETERS == QUERY_MAX_PARAMETERS,
                   "MAX_PARAMETERS and QUERY_MAX_PARAMETERS must stay equal; "
                   "the parameter_roles arrays are memcpy'd between them.");
    memcpy(statement->parameter_roles, analysis.parameter_roles,
           sizeof(statement->parameter_roles));
    memcpy(statement->procedure_name, analysis.procedure_name,
           sizeof(statement->procedure_name));
    memcpy(statement->call_arguments, analysis.call_arguments,
           sizeof(statement->call_arguments));
    statement->call_argument_count = analysis.call_argument_count;

    /* When the Parse option is on, derive client-side column metadata from the
     * original SQL's select list (e.g. a string literal -> VARCHAR(length)). */
    statement->column_override_count = 0;
    if (statement->parent_connection &&
        statement->parent_connection->info.parse_statements) {
        query_parse_select_columns(statement->sql_text,
                                   statement->column_overrides,
                                   MAX_BOUND_COLUMNS,
                                   &statement->column_override_count);
    }
    return SQL_SUCCESS;
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

    /* Release any not-yet-consumed result sets from a multi-statement query.
     * Entries at or after pending_result_index are results the application has
     * not stepped to via SQLMoreResults; earlier entries were already promoted
     * to current_result and freed there, so we only free from the index. */
    if (statement->pending_results) {
        for (int index = statement->pending_result_index;
             index < statement->pending_result_count; index++) {
            if (statement->pending_results[index]) {
                PQclear(statement->pending_results[index]);
            }
        }
        free(statement->pending_results);
        statement->pending_results = NULL;
    }
    statement->pending_result_count = 0;
    statement->pending_result_index = 0;

    statement->affected_row_count = -1;
    statement->has_result_set = false;
    statement->current_row_position = -1;
}

/*
 * Discard any stored multi-statement fragments and clear the multi-statement
 * flag. Called before re-preparing or re-executing a statement (the fragment
 * list belongs to the current SQL text) and when the handle is freed. Does not
 * touch result sets — that is clear_current_result's job.
 */
static void reset_multi_statement(OdbcStatement *statement)
{
    query_statement_list_free(&statement->statement_fragments);
    statement->is_multi_statement = false;
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
    case PGRES_TUPLES_OK: {
        /* SELECT or DML-with-RETURNING query — result set available for fetching.
         * Keep the PGresult alive; the results module will read from it. */
        statement->current_result = result;
        statement->has_result_set = true;
        statement->affected_row_count = PQntuples(result);
        statement->state = STATEMENT_STATE_HAS_CURSOR;

        /* Detect a searched UPDATE or DELETE with a RETURNING clause that matched
         * no rows. Per the ODBC spec, SQLExecute/SQLExecDirect must return
         * SQL_NO_DATA when a searched update or delete affects zero rows.
         *
         * We key off the command tag from PQcmdStatus ("UPDATE <n>" / "DELETE <n>")
         * rather than PQcmdTuples: in modern PostgreSQL PQcmdTuples is also
         * populated for SELECT, so a legitimately empty SELECT would otherwise be
         * misreported as SQL_NO_DATA. Restricting to UPDATE/DELETE matches the
         * spec exactly and leaves SELECT and INSERT...RETURNING untouched. */
        const char *command_tag = PQcmdStatus(result);
        bool is_searched_update_or_delete =
            command_tag &&
            (strncmp(command_tag, "UPDATE ", 7) == 0 ||
             strncmp(command_tag, "DELETE ", 7) == 0);
        bool is_zero_row_dml =
            is_searched_update_or_delete && PQntuples(result) == 0;

        /* If the connection captured NOTICE messages during execution, promote
         * them to ODBC diagnostic records. */
        bool had_notices = false;
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
            had_notices = true;
        }

        if (is_zero_row_dml) {
            return SQL_NO_DATA;
        }
        return had_notices ? SQL_SUCCESS_WITH_INFO : SQL_SUCCESS;
    }

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

    /* The implicit parameter descriptor is embedded in the statement; it
     * points back so SQLSetDescField can update this statement's bindings. */
    statement->implicit_param_descriptor.magic_number = DESCRIPTOR_MAGIC_NUMBER;
    statement->implicit_param_descriptor.role = DESCRIPTOR_ROLE_IMPLICIT_PARAM;
    statement->implicit_param_descriptor.owner = statement;

    /* The application row descriptor carries per-column precision overrides. */
    statement->app_row_descriptor.magic_number = DESCRIPTOR_MAGIC_NUMBER;
    statement->app_row_descriptor.role = DESCRIPTOR_ROLE_APP_ROW;
    statement->app_row_descriptor.owner = statement;

    /* No column precision overrides set initially (-1 = unset). */
    for (int column_index = 0; column_index < MAX_BOUND_COLUMNS; column_index++) {
        statement->column_precision_override[column_index] = -1;
    }

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

    /* Clear the PGresult if any (also frees any queued multi-statement results) */
    clear_current_result(statement);

    /* Free any multi-statement fragment list */
    reset_multi_statement(statement);

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

/*
 * Prepare and describe only the first fragment of a multi-statement query so
 * SQLNumResultCols/SQLDescribeCol can report the leading result set's columns
 * before execution — without running any of the (possibly side-effecting)
 * later fragments. The server-side prepared statement is used ONLY for
 * describing here; actual execution goes through execute_multi_statement, which
 * runs each fragment with PQexec/PQexecParams. Leaves the statement in the
 * PREPARED state. Returns SQL_SUCCESS (a describe failure is non-fatal, matching
 * the single-statement path).
 */
static SQLRETURN prepare_describe_first_fragment(OdbcStatement *statement)
{
    statement->is_prepared = true;
    statement->state = STATEMENT_STATE_PREPARED;

    if (statement->statement_fragments.count == 0) {
        return SQL_SUCCESS;
    }

    QueryParseOptions options = build_parse_options(statement);
    QueryAnalysis analysis;
    if (!query_analyze(statement->statement_fragments.statements[0], &options,
                       NULL, 0, &analysis)) {
        /* Non-fatal: metadata simply won't be available before execution. */
        return SQL_SUCCESS;
    }

    PGconn *libpq_connection = statement->parent_connection->libpq_connection;
    snprintf(statement->prepared_name, MAX_PREPARED_NAME_LENGTH,
             "_psqlodbc2_stmt_%d", statement->parent_connection->next_statement_id++);

    PGresult *prepare_result = PQprepare(libpq_connection,
                                         statement->prepared_name,
                                         analysis.transformed_sql,
                                         0, NULL);
    free(analysis.transformed_sql);

    if (!prepare_result || PQresultStatus(prepare_result) != PGRES_COMMAND_OK) {
        /* The describe is best-effort; a failure here should not block the
         * later real execution, which reports its own errors. */
        if (prepare_result) {
            PQclear(prepare_result);
        }
        statement->prepared_name[0] = '\0';
        return SQL_SUCCESS;
    }
    PQclear(prepare_result);

    PGresult *describe_result = PQdescribePrepared(libpq_connection,
                                                   statement->prepared_name);
    if (describe_result && PQresultStatus(describe_result) == PGRES_COMMAND_OK) {
        statement->describe_result = describe_result;
    } else if (describe_result) {
        PQclear(describe_result);
    }

    /* Deallocate the describe-only prepared statement immediately; the real
     * execution does not reuse it, and we don't want to leak server-side
     * prepared statements. Errors are ignored — the statement is transient. */
    char deallocate_command[MAX_PREPARED_NAME_LENGTH + 16];
    snprintf(deallocate_command, sizeof(deallocate_command),
             "DEALLOCATE %s", statement->prepared_name);
    PGresult *deallocate_result = PQexec(libpq_connection, deallocate_command);
    if (deallocate_result) {
        PQclear(deallocate_result);
    }
    /* We keep prepared_name populated so statement_free's deallocation is a
     * harmless no-op; clear it so we don't attempt a second DEALLOCATE. */
    statement->prepared_name[0] = '\0';

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

    /* Split into statement fragments. A multi-statement prepared query is
     * executed fragment-by-fragment at SQLExecute time (the extended protocol
     * cannot prepare more than one command), and only the first fragment is
     * described here so SQLNumResultCols/SQLDescribeCol report the first
     * result's columns WITHOUT executing any statement (premature-execution
     * guard). */
    reset_multi_statement(statement);
    {
        QueryParseOptions split_options = build_parse_options(statement);
        if (!query_split_statements(statement->sql_text, &split_options,
                                    &statement->statement_fragments)) {
            diagnostics_add_record(&statement->diagnostics, "HY001", 0,
                                   "Memory allocation failed splitting statements.");
            return SQL_ERROR;
        }
        statement->is_multi_statement = statement->statement_fragments.count > 1;
    }

    /* Analyze the SQL: translate '?' markers to '$N', process ODBC escapes, and
     * record procedure-call metadata. At SQLPrepare time the application has not
     * yet bound parameters, so no cast information is available; PostgreSQL
     * infers the parameter types from the prepared statement instead. */
    if (analyze_and_store(statement, false) != SQL_SUCCESS) {
        return SQL_ERROR;
    }

    /* For a multi-statement query, prepare and describe only the FIRST fragment.
     * This yields the leading result set's column metadata (what SQLNumResultCols
     * must report before execution) while guaranteeing that later fragments —
     * which may have side effects, e.g. an INSERT-performing function — are not
     * sent to the server until SQLExecute. Actual execution runs every fragment
     * via execute_multi_statement. */
    if (statement->is_multi_statement) {
        return prepare_describe_first_fragment(statement);
    }

    /* Procedure calls are prepared lazily at SQLExecute time: their argument
     * types (unknown vs. void for OUT params) depend on the bindings, which are
     * not known until after SQLPrepare. Defer server-side preparation so we can
     * build the correct parameter arrays once parameters are bound. */
    if (statement->is_procedure_call) {
        statement->is_prepared = true;
        statement->prepared_name[0] = '\0';
        statement->state = STATEMENT_STATE_PREPARED;
        return SQL_SUCCESS;
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

/*
 * Copy a procedure's result columns back into the application's OUT/INOUT
 * parameter buffers after a successful call.
 *
 * PostgreSQL returns one result row whose columns are the function's OUT and
 * INOUT parameters (and, for a scalar function, its single return column). We
 * match each such parameter to a result column: by name when the application
 * named the parameter (via SQLSetDescField), otherwise by the left-to-right
 * order of output parameters. Values are copied as text (SQL_C_CHAR), which is
 * what the ODBC test binds; the indicator receives the (untruncated) byte
 * length.
 *
 * Returns true if any OUT/INOUT value was truncated to fit its buffer, so the
 * caller can report SQLSTATE 01004 and SQL_SUCCESS_WITH_INFO per the ODBC spec.
 */
static bool populate_output_parameters(OdbcStatement *statement, PGresult *result)
{
    if (!statement->is_procedure_call || !result ||
        PQresultStatus(result) != PGRES_TUPLES_OK || PQntuples(result) < 1) {
        return false;
    }

    bool truncated_any = false;

    int result_column_count = PQnfields(result);
    int next_positional_column = 0;   /* Next result column for unnamed OUT params */

    for (int index = 0; index < statement->detected_param_count && index < MAX_PARAMETERS; index++) {
        ParameterBinding *binding = &statement->parameter_bindings[index];
        QueryParamRole role = statement->parameter_roles[index];

        bool is_output =
            (role == QUERY_PARAM_ROLE_RETURN_VALUE) ||
            (binding->is_bound &&
             (binding->input_output_type == SQL_PARAM_OUTPUT ||
              binding->input_output_type == SQL_PARAM_INPUT_OUTPUT));
        if (!is_output || !binding->is_bound) {
            continue;
        }

        /* Choose the source result column: by name when the parameter is named,
         * else the next output column in order. */
        int column_index;
        if (binding->name[0] != '\0') {
            column_index = PQfnumber(result, binding->name);
            if (column_index < 0) {
                continue;   /* No matching output column for this name */
            }
        } else {
            if (next_positional_column >= result_column_count) {
                continue;
            }
            column_index = next_positional_column++;
        }

        if (!binding->value_buffer) {
            continue;
        }

        if (PQgetisnull(result, 0, column_index)) {
            if (binding->indicator_or_length) {
                *binding->indicator_or_length = SQL_NULL_DATA;
            }
            continue;
        }

        const char *text = PQgetvalue(result, 0, column_index);
        size_t text_length = (size_t)PQgetlength(result, 0, column_index);

        /* Copy as text into the application buffer, null-terminating and
         * respecting the declared buffer length. */
        if (binding->buffer_length > 0) {
            size_t copy_length = text_length;
            if (copy_length > (size_t)binding->buffer_length - 1) {
                copy_length = (size_t)binding->buffer_length - 1;
                truncated_any = true;
            }
            memcpy(binding->value_buffer, text, copy_length);
            ((char *)binding->value_buffer)[copy_length] = '\0';
        }
        if (binding->indicator_or_length) {
            *binding->indicator_or_length = (SQLLEN)text_length;
        }
    }

    return truncated_any;
}

/*
 * Execute an ODBC procedure call ({call ...} or { ? = call ...}).
 *
 * The parser captured the function name, the SELECT-wrapper prefix (in
 * translated_sql, ending just after the "("), and the structured argument
 * list. Here — where parameter bindings are finally known — we build the full
 * call:
 *   - OUT-only marker arguments are dropped (PostgreSQL fills OUT parameters
 *     from the function body, so they are not passed as call arguments);
 *   - IN / INOUT marker arguments and literal arguments are emitted, using
 *     named notation ("name" := $K) when the parameter was named via
 *     SQLSetDescField, otherwise positional;
 *   - surviving markers are renumbered contiguously from $1 and their values
 *     bound with the PostgreSQL "unknown" type so the server can resolve the
 *     function overload.
 *
 * After a successful call, result columns are copied into OUT/INOUT parameter
 * buffers. Returns the result of handle_execution_result.
 */
static SQLRETURN execute_procedure_call(OdbcStatement *statement)
{
    PGconn *libpq_connection = statement->parent_connection->libpq_connection;

    /* Build the SQL: start from the wrapper prefix + funcname + "(" that the
     * parser already produced in translated_sql. */
    size_t sql_capacity = strlen(statement->translated_sql) + 256;
    char *call_sql = malloc(sql_capacity);
    if (!call_sql) {
        diagnostics_add_record(&statement->diagnostics, "HY001", 0,
                               "Memory allocation failed building procedure call.");
        return SQL_ERROR;
    }
    size_t sql_length = strlen(statement->translated_sql);
    memcpy(call_sql, statement->translated_sql, sql_length + 1);

    /* The parser emits the SELECT wrapper up to (but not including) the opening
     * parenthesis of the argument list. Emit it here so the executor controls
     * the full "(args)" text (dropping OUT-only markers, renumbering, etc.). */
    call_sql[sql_length++] = '(';
    call_sql[sql_length] = '\0';

    /* Bind arrays for the surviving marker arguments. */
    unsigned int bind_types[MAX_PARAMETERS];
    const char *bind_values[MAX_PARAMETERS];
    int bind_lengths[MAX_PARAMETERS];
    int bind_formats[MAX_PARAMETERS];
    int bind_count = 0;
    bool build_failed = false;

    int emitted_arguments = 0;
    for (int arg_index = 0; arg_index < statement->call_argument_count; arg_index++) {
        const QueryCallArgument *argument = &statement->call_arguments[arg_index];

        /* Buffer for one argument fragment. */
        char fragment[128 + QUERY_MAX_CAST_LENGTH];
        size_t fragment_length = 0;

        if (argument->parameter_number > 0) {
            /* Marker argument — look up its binding to decide IN/OUT/INOUT. */
            int slot = argument->parameter_number - 1;
            const ParameterBinding *binding =
                (slot < MAX_PARAMETERS) ? &statement->parameter_bindings[slot] : NULL;

            /* Drop OUT-only parameters from the call argument list. */
            if (binding && binding->is_bound &&
                binding->input_output_type == SQL_PARAM_OUTPUT) {
                continue;
            }

            /* Convert the bound value to text (unknown-typed). */
            int value_length = 0;
            char *value_text = NULL;
            if (binding && binding->is_bound) {
                value_text = convert_parameter_to_text(binding, &value_length);
            }
            bind_types[bind_count] = PG_OID_UNKNOWN;
            bind_values[bind_count] = value_text;   /* NULL means SQL NULL */
            bind_lengths[bind_count] = value_length;
            bind_formats[bind_count] = 0;
            bind_count++;

            /* Emit ["name" := ] $K [cast]. */
            if (binding && binding->name[0] != '\0') {
                fragment_length += (size_t)snprintf(fragment + fragment_length,
                                                    sizeof(fragment) - fragment_length,
                                                    "\"%s\" := ", binding->name);
            }
            fragment_length += (size_t)snprintf(fragment + fragment_length,
                                                sizeof(fragment) - fragment_length,
                                                "$%d%s", bind_count, argument->cast_suffix);
        } else {
            /* Literal argument — emit verbatim. */
            fragment_length += (size_t)snprintf(fragment, sizeof(fragment),
                                                "%s", argument->literal_text);
        }

        /* Append ", " separator between emitted arguments. */
        size_t needed = sql_length + fragment_length + 4;
        if (needed >= sql_capacity) {
            sql_capacity = needed * 2;
            char *grown = realloc(call_sql, sql_capacity);
            if (!grown) {
                build_failed = true;
                break;
            }
            call_sql = grown;
        }
        if (emitted_arguments > 0) {
            memcpy(call_sql + sql_length, ", ", 2);
            sql_length += 2;
        }
        memcpy(call_sql + sql_length, fragment, fragment_length);
        sql_length += fragment_length;
        call_sql[sql_length] = '\0';
        emitted_arguments++;
    }

    if (build_failed) {
        for (int i = 0; i < bind_count; i++) {
            free((void *)bind_values[i]);
        }
        free(call_sql);
        diagnostics_add_record(&statement->diagnostics, "HY001", 0,
                               "Memory allocation failed building procedure call.");
        return SQL_ERROR;
    }

    /* Append the closing ')'. */
    if (sql_length + 2 >= sql_capacity) {
        char *grown = realloc(call_sql, sql_length + 2);
        if (!grown) {
            for (int i = 0; i < bind_count; i++) {
                free((void *)bind_values[i]);
            }
            free(call_sql);
            diagnostics_add_record(&statement->diagnostics, "HY001", 0,
                                   "Memory allocation failed building procedure call.");
            return SQL_ERROR;
        }
        call_sql = grown;
    }
    call_sql[sql_length++] = ')';
    call_sql[sql_length] = '\0';

    PGresult *result = PQexecParams(libpq_connection,
                                    call_sql,
                                    bind_count,
                                    bind_count > 0 ? bind_types : NULL,
                                    bind_count > 0 ? bind_values : NULL,
                                    bind_count > 0 ? bind_lengths : NULL,
                                    bind_count > 0 ? bind_formats : NULL,
                                    0  /* result format: text */);

    /* Copy OUT/INOUT values back before the shared handler frees the result. */
    bool output_truncated = populate_output_parameters(statement, result);

    for (int i = 0; i < bind_count; i++) {
        free((void *)bind_values[i]);
    }
    free(call_sql);

    SQLRETURN execution_result = handle_execution_result(statement, result);

    /* If any OUT/INOUT value did not fit its bound buffer, the ODBC spec
     * requires reporting SQLSTATE 01004 (string data, right-truncated) and
     * SQL_SUCCESS_WITH_INFO. The indicator already carries the untruncated
     * length. Only escalate a successful execution — never mask an error. */
    if (output_truncated && SQL_SUCCEEDED(execution_result)) {
        diagnostics_add_record(&statement->diagnostics,
                               "01004",  /* String data, right-truncated */
                               0,
                               "Output parameter value was truncated to fit the bound buffer.");
        return SQL_SUCCESS_WITH_INFO;
    }

    return execution_result;
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

    /* Discard notices left over from a prior execution so this run only reports
     * the NOTICE/WARNING messages it actually produces. (Error paths do not
     * promote-and-clear notices, so stale ones could otherwise leak forward.) */
    connection_clear_notices(statement->parent_connection);

    /* Procedure calls are executed via PQexecParams with unknown/void typed
     * arguments (see execute_procedure_call), not via a server-side prepared
     * statement. */
    if (statement->is_procedure_call) {
        return execute_procedure_call(statement);
    }

    /* A prepared multi-statement runs each fragment now (only the first was
     * prepared, for describe). Fragments consume the bound parameters in order
     * and the results are chained for SQLMoreResults. */
    if (statement->is_multi_statement) {
        return execute_multi_statement(statement);
    }

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

    SQLRETURN execution_result = handle_execution_result(statement, result);

    /* A plain SQL "CALL proc(...)" returns its INOUT/OUT values as a result
     * row; copy them back and, when enabled, fetch any refcursor OUT values. */
    if (translated_sql_is_call(statement)) {
        return handle_call_result(statement, execution_result);
    }
    return execution_result;
}

/*
 * Adopt a PGresult as the statement's current result set, updating the
 * row-count, has-result-set flag, cursor position, and statement state from the
 * result's status. Unlike handle_execution_result, this NEVER frees the result
 * — it is used for results that live in the multi-statement chain and are freed
 * only when the chain is torn down (clear_current_result) or replaced by the
 * next promotion. Returns SQL_SUCCESS for a usable result, SQL_ERROR for a
 * failed one.
 */
static SQLRETURN apply_result_as_current(OdbcStatement *statement, PGresult *result)
{
    statement->current_result = result;
    statement->current_row_position = -1;

    /* Stale client-side column overrides belong to a previous result set; the
     * chained results derive metadata directly from their own PGresult. */
    statement->column_override_count = 0;

    ExecStatusType status = result ? PQresultStatus(result) : PGRES_FATAL_ERROR;

    if (status == PGRES_TUPLES_OK) {
        statement->has_result_set = true;
        statement->affected_row_count = PQntuples(result);
        statement->state = STATEMENT_STATE_HAS_CURSOR;
        return SQL_SUCCESS;
    }
    if (status == PGRES_COMMAND_OK) {
        const char *affected_rows_text = PQcmdTuples(result);
        statement->affected_row_count =
            (affected_rows_text && affected_rows_text[0] != '\0')
                ? atoi(affected_rows_text)
                : 0;
        statement->has_result_set = false;
        statement->state = STATEMENT_STATE_EXECUTED;
        return SQL_SUCCESS;
    }

    statement->has_result_set = false;
    statement->affected_row_count = -1;
    return SQL_ERROR;
}

/*
 * Return true if any column of the CALL result is a PostgreSQL refcursor. Such
 * a result means the called function/procedure handed back open cursors as OUT
 * values, which the FetchRefcursors option turns into fetchable result sets.
 */
static bool result_has_refcursor_column(PGresult *result)
{
    if (!result || PQresultStatus(result) != PGRES_TUPLES_OK) {
        return false;
    }
    int column_count = PQnfields(result);
    for (int column_index = 0; column_index < column_count; column_index++) {
        if ((unsigned int)PQftype(result, column_index) == PG_TYPE_REFCURSOR) {
            return true;
        }
    }
    return false;
}

/*
 * Implement the FetchRefcursors option for a CALL that returned refcursor OUT
 * parameters. For each non-NULL refcursor value in the (single-row) call result,
 * issue "FETCH ALL IN \"<portal>\"" and collect the rows as a result set. The
 * collected result sets are chained (first becomes current, rest queued) so the
 * application walks them with SQLMoreResults, exactly like a multi-statement
 * query.
 *
 * A refcursor is only usable inside the transaction that opened it, so this
 * requires an active transaction; otherwise it fails with the same message the
 * original driver emits. NULL refcursor columns are skipped, which lets a
 * procedure return a variable number of cursors.
 *
 * call_result is consumed here (freed) on success. Returns the SQLRETURN for the
 * first fetched result set, SQL_ERROR on failure, or — when no refcursor was
 * actually open (all NULL) — leaves an empty result set current and returns
 * SQL_SUCCESS.
 */
static SQLRETURN fetch_refcursor_results(OdbcStatement *statement, PGresult *call_result)
{
    OdbcConnection *connection = statement->parent_connection;
    PGconn *libpq_connection = connection->libpq_connection;

    /* Ownership of call_result was transferred to this function; every exit
     * path below must free it (the success/all-NULL paths free it explicitly). */

    /* Refcursors are only valid within the transaction that declared them. */
    if (connection->transaction_state != TRANSACTION_STATE_ACTIVE) {
        diagnostics_add_record(&statement->diagnostics, "HY000", 0,
                               "Query must be executed in a transaction when "
                               "FetchRefcursors setting is enabled.");
        PQclear(call_result);
        return SQL_ERROR;
    }

    int column_count = PQnfields(call_result);

    /* At most one fetched result set per refcursor column. */
    PGresult **results = calloc((size_t)column_count, sizeof(PGresult *));
    if (!results) {
        diagnostics_add_record(&statement->diagnostics, "HY001", 0,
                               "Memory allocation failed fetching refcursors.");
        PQclear(call_result);
        return SQL_ERROR;
    }

    int collected = 0;
    bool failed = false;

    for (int column_index = 0; column_index < column_count; column_index++) {
        if ((unsigned int)PQftype(call_result, column_index) != PG_TYPE_REFCURSOR) {
            continue;
        }
        /* Skip NULL cursors so a procedure can return fewer cursors than it
         * declares OUT parameters. */
        if (PQgetisnull(call_result, 0, column_index)) {
            continue;
        }

        const char *portal_name = PQgetvalue(call_result, 0, column_index);

        /* FETCH ALL IN "<portal>". The portal name comes from PostgreSQL, but
         * double any embedded quote defensively before wrapping in identifier
         * quotes. */
        char fetch_command[256];
        size_t offset = 0;
        offset += (size_t)snprintf(fetch_command + offset, sizeof(fetch_command) - offset,
                                   "FETCH ALL IN \"");
        for (const char *cursor = portal_name;
             *cursor && offset + 2 < sizeof(fetch_command); cursor++) {
            if (*cursor == '"' && offset + 3 < sizeof(fetch_command)) {
                fetch_command[offset++] = '"';
            }
            fetch_command[offset++] = *cursor;
        }
        if (offset + 2 < sizeof(fetch_command)) {
            fetch_command[offset++] = '"';
        }
        fetch_command[offset] = '\0';

        PGresult *fetch_result = PQexec(libpq_connection, fetch_command);
        ExecStatusType status = fetch_result ? PQresultStatus(fetch_result)
                                             : PGRES_FATAL_ERROR;
        if (status != PGRES_TUPLES_OK && status != PGRES_COMMAND_OK) {
            diagnostics_clear(&statement->diagnostics);
            error_add_diagnostic_from_result_ctx(&statement->diagnostics, fetch_result,
                                                 "HY000",
                                                 "Error while fetching refcursor");
            if (fetch_result) {
                PQclear(fetch_result);
            }
            failed = true;
            break;
        }
        results[collected++] = fetch_result;
    }

    if (failed) {
        for (int index = 0; index < collected; index++) {
            PQclear(results[index]);
        }
        free(results);
        PQclear(call_result);
        return SQL_ERROR;
    }

    /* The original CALL result (portal names) is replaced by the fetched rows. */
    PQclear(call_result);

    if (collected == 0) {
        /* All refcursors were NULL — present an empty result set. The empty
         * PGresult is produced by a no-op query so metadata calls stay valid. */
        free(results);
        PGresult *empty = PQexec(libpq_connection, "SELECT WHERE false");
        return apply_result_as_current(statement, empty ? empty : NULL) == SQL_SUCCESS
                   ? SQL_SUCCESS
                   : SQL_ERROR;
        /* Note: the empty result is owned by current_result and freed by
         * clear_current_result. */
    }

    /* Chain the fetched result sets: first current, rest queued for
     * SQLMoreResults (identical ownership model to execute_multi_statement). */
    statement->pending_results = results;
    statement->pending_result_count = collected;
    statement->pending_result_index = 1;

    return apply_result_as_current(statement, results[0]);
}

/*
 * Copy a bare "CALL proc(...)" statement's single result row back into bound
 * OUT / INOUT parameter buffers, with type-aware conversion (e.g. an INOUT
 * integer bound as SQL_C_LONG). PostgreSQL returns one row whose columns are the
 * procedure's INOUT/OUT parameters in declaration order; we map bound OUT/INOUT
 * parameters to those columns positionally. This mirrors
 * populate_output_parameters (used by the {call} escape path) but converts to
 * the bound C type rather than copying raw text, and applies to the plain-CALL
 * path where is_procedure_call is false.
 */
static void populate_call_output_parameters(OdbcStatement *statement, PGresult *result)
{
    if (!result || PQresultStatus(result) != PGRES_TUPLES_OK || PQntuples(result) < 1) {
        return;
    }

    int result_column_count = PQnfields(result);
    int next_column = 0;

    for (int index = 0;
         index < statement->bound_parameter_count && index < MAX_PARAMETERS; index++) {
        ParameterBinding *binding = &statement->parameter_bindings[index];
        if (!binding->is_bound) {
            continue;
        }
        if (binding->input_output_type != SQL_PARAM_OUTPUT &&
            binding->input_output_type != SQL_PARAM_INPUT_OUTPUT) {
            continue;
        }
        if (next_column >= result_column_count) {
            break;
        }
        int column_index = next_column++;
        if (!binding->value_buffer) {
            continue;
        }
        results_convert_column(statement, result, 0, column_index,
                               binding->c_type, binding->value_buffer,
                               binding->buffer_length, binding->indicator_or_length);
    }
}

/*
 * Post-process the result of a plain "CALL proc(...)" execution: copy OUT/INOUT
 * values back to the application, and — when the FetchRefcursors option is on
 * and the call returned refcursor OUT values — replace the portal-name result
 * with the rows fetched from each cursor (chained for SQLMoreResults).
 *
 * Only acts on a successful TUPLES_OK result whose statement text is a CALL.
 * Returns the (possibly adjusted) SQLRETURN to propagate to the caller.
 */
static SQLRETURN handle_call_result(OdbcStatement *statement, SQLRETURN execution_result)
{
    if (!SQL_SUCCEEDED(execution_result) || !statement->current_result ||
        PQresultStatus(statement->current_result) != PGRES_TUPLES_OK) {
        return execution_result;
    }

    /* Copy OUT/INOUT parameter values back (e.g. an INOUT counter). */
    populate_call_output_parameters(statement, statement->current_result);

    /* Auto-fetch refcursors only when the option is enabled and the call
     * actually returned cursor values. */
    if (statement->parent_connection &&
        statement->parent_connection->info.fetch_refcursors &&
        result_has_refcursor_column(statement->current_result)) {
        PGresult *call_result = statement->current_result;
        statement->current_result = NULL;   /* ownership moves to fetch helper */
        return fetch_refcursor_results(statement, call_result);
    }

    return execution_result;
}

/*
 * Return true when the (already ODBC-translated) SQL text is a plain SQL-standard
 * CALL statement, i.e. begins with the CALL keyword. Used to decide whether to
 * run CALL post-processing (OUT-parameter copyback / refcursor fetching). The
 * ODBC "{call ...}" escape is handled separately via is_procedure_call.
 */
static bool translated_sql_is_call(const OdbcStatement *statement)
{
    const char *cursor = statement->translated_sql;
    if (!cursor) {
        return false;
    }
    while (*cursor == ' ' || *cursor == '\t' || *cursor == '\n' || *cursor == '\r') {
        cursor++;
    }
    /* Case-insensitive "call" followed by a non-identifier boundary. */
    static const char keyword[] = "call";
    for (int i = 0; i < 4; i++) {
        char lowered = (cursor[i] >= 'A' && cursor[i] <= 'Z')
                           ? (char)(cursor[i] + ('a' - 'A'))
                           : cursor[i];
        if (lowered != keyword[i]) {
            return false;
        }
    }
    char after = cursor[4];
    return after == ' ' || after == '\t' || after == '\n' || after == '\r' ||
           after == '(' || after == '\0';
}

/*
 * Execute a single statement fragment of a multi-statement query and return its
 * raw PGresult (caller owns it and must PQclear). The fragment is analyzed on
 * its own so its ODBC "?" markers translate to "$1..$N" independently of the
 * other fragments.
 *
 * param_offset is the zero-based index into the statement's global parameter
 * bindings where this fragment's parameters begin (fragments consume the bound
 * parameters left to right). *out_fragment_param_count receives the number of
 * markers found in this fragment so the caller can advance the offset.
 *
 * Returns NULL only on allocation failure (with a diagnostic set).
 */
static PGresult *execute_one_fragment(OdbcStatement *statement,
                                      const char *fragment_sql,
                                      int param_offset,
                                      int *out_fragment_param_count)
{
    *out_fragment_param_count = 0;

    QueryParseOptions options = build_parse_options(statement);

    /* Per-fragment cast suffixes, indexed by the fragment's local (0-based)
     * parameter position. Each maps to the global binding at param_offset+i so
     * a value bound as, say, SQL_INTEGER is cast to ::int4 as in the
     * single-statement path. */
    const char *fragment_casts[MAX_PARAMETERS];
    for (int local_index = 0; local_index < MAX_PARAMETERS; local_index++) {
        int global_index = param_offset + local_index;
        if (global_index < MAX_PARAMETERS &&
            statement->parameter_bindings[global_index].is_bound) {
            fragment_casts[local_index] = type_mapping_get_param_cast(
                statement->parameter_bindings[global_index].sql_type);
        } else {
            fragment_casts[local_index] = NULL;
        }
    }

    QueryAnalysis analysis;
    if (!query_analyze(fragment_sql, &options,
                       fragment_casts, MAX_PARAMETERS, &analysis)) {
        diagnostics_add_record(&statement->diagnostics, "HY001", 0,
                               "Memory allocation failed analyzing statement fragment.");
        return NULL;
    }

    int fragment_param_count = analysis.parameter_count;
    *out_fragment_param_count = fragment_param_count;

    PGconn *libpq_connection = statement->parent_connection->libpq_connection;
    PGresult *result = NULL;

    /* Send parameters only when the fragment actually has markers and the
     * application bound values for this fragment's slice. */
    bool slice_has_bound_param = false;
    for (int local_index = 0;
         local_index < fragment_param_count &&
         (param_offset + local_index) < MAX_PARAMETERS; local_index++) {
        if (statement->parameter_bindings[param_offset + local_index].is_bound) {
            slice_has_bound_param = true;
            break;
        }
    }

    if (fragment_param_count > 0 && slice_has_bound_param) {
        /* Build parallel libpq arrays from this fragment's parameter slice. */
        const char **param_values = calloc((size_t)fragment_param_count, sizeof(char *));
        int *param_lengths = calloc((size_t)fragment_param_count, sizeof(int));
        int *param_formats = calloc((size_t)fragment_param_count, sizeof(int));

        if (!param_values || !param_lengths || !param_formats) {
            free(param_values);
            free(param_lengths);
            free(param_formats);
            free(analysis.transformed_sql);
            diagnostics_add_record(&statement->diagnostics, "HY001", 0,
                                   "Memory allocation failed building fragment parameters.");
            return NULL;
        }

        for (int local_index = 0; local_index < fragment_param_count; local_index++) {
            int global_index = param_offset + local_index;
            const ParameterBinding *binding =
                (global_index < MAX_PARAMETERS)
                    ? &statement->parameter_bindings[global_index]
                    : NULL;
            int value_length = 0;
            if (binding && binding->is_bound) {
                param_values[local_index] =
                    convert_parameter_to_text(binding, &value_length);
            } else {
                param_values[local_index] = NULL;   /* Unbound slot -> SQL NULL */
            }
            param_lengths[local_index] = value_length;
            param_formats[local_index] = 0;   /* text format */
        }

        result = PQexecParams(libpq_connection,
                              analysis.transformed_sql,
                              fragment_param_count,
                              NULL,   /* let PostgreSQL infer parameter types */
                              param_values,
                              param_lengths,
                              param_formats,
                              0       /* result format: text */);

        for (int local_index = 0; local_index < fragment_param_count; local_index++) {
            free((void *)param_values[local_index]);
        }
        free(param_values);
        free(param_lengths);
        free(param_formats);
    } else {
        /* No parameters for this fragment — a plain command. */
        result = PQexec(libpq_connection, analysis.transformed_sql);
    }

    free(analysis.transformed_sql);
    return result;
}

/*
 * Execute every fragment of a multi-statement query in order, chaining the
 * PGresults on the statement so SQLMoreResults can walk them. The first
 * result becomes current; the rest are queued in pending_results.
 *
 * statement->statement_fragments must already be populated. Parameters are
 * consumed left to right across the fragments (fragment 1 takes the first N1
 * bound parameters, fragment 2 the next N2, and so on).
 *
 * Returns the SQLRETURN for the first (current) result, or SQL_ERROR if any
 * fragment fails to execute (the failing fragment's diagnostic is recorded).
 */
static SQLRETURN execute_multi_statement(OdbcStatement *statement)
{
    int fragment_count = statement->statement_fragments.count;
    if (fragment_count == 0) {
        diagnostics_add_record(&statement->diagnostics, "HY000", 0,
                               "Multi-statement query contained no executable statements.");
        return SQL_ERROR;
    }

    PGresult **results = calloc((size_t)fragment_count, sizeof(PGresult *));
    if (!results) {
        diagnostics_add_record(&statement->diagnostics, "HY001", 0,
                               "Memory allocation failed for multi-statement results.");
        return SQL_ERROR;
    }

    int collected = 0;
    int param_offset = 0;
    bool fragment_error = false;

    for (int fragment_index = 0; fragment_index < fragment_count; fragment_index++) {
        int fragment_param_count = 0;
        PGresult *result = execute_one_fragment(
            statement,
            statement->statement_fragments.statements[fragment_index],
            param_offset,
            &fragment_param_count);
        param_offset += fragment_param_count;

        if (!result) {
            /* Allocation failure inside the fragment executor. */
            fragment_error = true;
            break;
        }

        results[collected++] = result;

        ExecStatusType status = PQresultStatus(result);
        if (status == PGRES_FATAL_ERROR ||
            (status != PGRES_TUPLES_OK && status != PGRES_COMMAND_OK)) {
            /* Stop at the first failing fragment, mirroring how PQexec aborts a
             * multi-command string on the first error. */
            diagnostics_clear(&statement->diagnostics);
            error_add_diagnostic_from_result_ctx(&statement->diagnostics, result,
                                                 "HY000",
                                                 "Error while executing the query");
            if (statement->parent_connection &&
                statement->parent_connection->transaction_state == TRANSACTION_STATE_ACTIVE) {
                statement->parent_connection->transaction_state = TRANSACTION_STATE_FAILED;
            }
            fragment_error = true;
            break;
        }
    }

    if (fragment_error) {
        for (int index = 0; index < collected; index++) {
            PQclear(results[index]);
        }
        free(results);
        return SQL_ERROR;
    }

    /* Publish the chain: results[0] is current; results[1..] wait in the queue.
     * pending_results holds ALL collected results; pending_result_index points
     * one past the current result so SQLMoreResults promotes results[1] next. */
    statement->pending_results = results;
    statement->pending_result_count = collected;
    statement->pending_result_index = 1;

    SQLRETURN first_result = apply_result_as_current(statement, results[0]);

    /* Promote any NOTICE/WARNING messages captured during execution. */
    if (statement->parent_connection &&
        statement->parent_connection->notice_count > 0) {
        OdbcConnection *connection = statement->parent_connection;
        for (int index = 0; index < connection->notice_count; index++) {
            diagnostics_add_record(&statement->diagnostics, "00000", 0,
                                   connection->captured_notices[index]);
        }
        connection_clear_notices(connection);
        if (SQL_SUCCEEDED(first_result)) {
            return SQL_SUCCESS_WITH_INFO;
        }
    }

    return first_result;
}

SQLRETURN statement_promote_next_result(OdbcStatement *statement)
{
    if (!statement) {
        return SQL_ERROR;
    }

    /* Free the result the application just finished with. In a multi-statement
     * chain the current result is owned by pending_results, so drop our
     * reference here and clear the slot to avoid a double free when the chain
     * is later torn down. */
    if (statement->current_result) {
        int current_slot = statement->pending_result_index - 1;
        if (current_slot >= 0 && current_slot < statement->pending_result_count &&
            statement->pending_results &&
            statement->pending_results[current_slot] == statement->current_result) {
            statement->pending_results[current_slot] = NULL;
        }
        PQclear(statement->current_result);
        statement->current_result = NULL;
    }

    /* Any pre-execute describe metadata is no longer relevant once we have
     * started walking real result sets. */
    if (statement->describe_result) {
        PQclear(statement->describe_result);
        statement->describe_result = NULL;
    }

    statement->has_result_set = false;
    statement->current_row_position = -1;

    /* Exhausted the chain: no more result sets. */
    if (!statement->pending_results ||
        statement->pending_result_index >= statement->pending_result_count) {
        return SQL_NO_DATA;
    }

    PGresult *next = statement->pending_results[statement->pending_result_index];
    statement->pending_result_index++;

    return apply_result_as_current(statement, next);
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

    /* Discard notices left over from a prior execution so this run only reports
     * the NOTICE/WARNING messages it actually produces. */
    connection_clear_notices(statement->parent_connection);

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

    /* Split the SQL into individual statement fragments. A query with more than
     * one top-level statement (e.g. "SELECT 1; SELECT 2") cannot be sent as a
     * single extended-protocol command; each fragment is executed separately
     * and the results are chained for SQLMoreResults. */
    reset_multi_statement(statement);
    {
        QueryParseOptions split_options = build_parse_options(statement);
        if (!query_split_statements(statement->sql_text, &split_options,
                                    &statement->statement_fragments)) {
            diagnostics_add_record(&statement->diagnostics, "HY001", 0,
                                   "Memory allocation failed splitting statements.");
            return SQL_ERROR;
        }
        statement->is_multi_statement = statement->statement_fragments.count > 1;
    }

    /* Analyze the SQL and process ODBC escapes. Bindings are already known at
     * ExecDirect time, so ordinary parameter markers receive type casts derived
     * from each bound parameter's declared SQL type. Procedure calls skip casts
     * (they send arguments as the unknown type). */
    if (analyze_and_store(statement, true) != SQL_SUCCESS) {
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

    /* Procedure calls run through the dedicated call path (unknown/void typed
     * arguments, OUT-parameter copy-back). */
    if (statement->is_procedure_call) {
        return execute_procedure_call(statement);
    }

    /* Multi-statement queries execute each fragment separately and chain the
     * results for SQLMoreResults (the extended protocol rejects a multi-command
     * string as a single prepared statement). */
    if (statement->is_multi_statement) {
        return execute_multi_statement(statement);
    }

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

        /* Send only as many parameters as the SQL actually references. The
         * application may reuse a statement handle without SQL_RESET_PARAMS,
         * leaving stale bindings from a prior statement that used more
         * parameters; passing those extras would make PostgreSQL reject the
         * bind message ("supplies N parameters, but ... requires M"). */
        int effective_param_count = param_count;
        if (effective_param_count > statement->detected_param_count) {
            effective_param_count = statement->detected_param_count;
        }

        result = PQexecParams(libpq_connection,
                              statement->translated_sql,
                              effective_param_count,
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

    SQLRETURN execution_result = handle_execution_result(statement, result);

    /* A plain SQL "CALL proc(...)" returns its INOUT/OUT values as a result
     * row; copy them back and, when enabled, fetch any refcursor OUT values. */
    if (translated_sql_is_call(statement)) {
        return handle_call_result(statement, execution_result);
    }
    return execution_result;
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
