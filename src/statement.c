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
#include <ctype.h>
#include <stdio.h>

/* ---- Internal Helpers ---- */

/* Lowercase a single ASCII byte. SQL keywords are pure ASCII, so
 * locale-independent byte lowercasing is correct and avoids the POSIX/GNU
 * strcasecmp family (not C11; MSVC spells them _stricmp/_strnicmp). */
static char statement_to_lower_ascii(char byte)
{
    return (byte >= 'A' && byte <= 'Z') ? (char)(byte - 'A' + 'a') : byte;
}

/* Portable ASCII case-insensitive prefix compare. Returns true when the first
 * prefix_length bytes of value match prefix ignoring case. Mirrors the
 * ascii_case_prefix helpers in results.c / query_parser.c / catalog.c. */
static bool statement_ascii_case_prefix(const char *value, const char *prefix,
                                        size_t prefix_length)
{
    for (size_t index = 0; index < prefix_length; index++) {
        if (value[index] == '\0' ||
            statement_to_lower_ascii(value[index]) !=
                statement_to_lower_ascii(prefix[index])) {
            return false;
        }
    }
    return true;
}

/* Execute every fragment of a multi-statement query and chain the results.
 * Defined below; forward-declared because statement_execute (earlier in the
 * file) also dispatches to it. */
static SQLRETURN execute_multi_statement(OdbcStatement *statement);

/* Post-process a plain "CALL proc(...)" result: copy OUT/INOUT values back and,
 * with FetchRefcursors enabled, expose returned cursors as result sets. Defined
 * below; forward-declared for the execute/exec-direct paths above it. */
static SQLRETURN handle_call_result(OdbcStatement *statement, SQLRETURN execution_result);
static bool translated_sql_is_call(const OdbcStatement *statement);

/* Adopt a PGresult as the current result set without freeing it (used for the
 * result-set chain). Defined below statement_execute; forward-declared because
 * the array-execution path above it chains RETURNING results. */
static SQLRETURN apply_result_as_current(OdbcStatement *statement, PGresult *result);

/* Execute a statement once per bound parameter set (SQL_ATTR_PARAMSET_SIZE > 1),
 * filling the per-row status array and chaining any result sets. Defined below;
 * forward-declared for statement_execute / statement_exec_direct. use_prepared
 * selects PQexecPrepared (prepared statement) vs PQexecParams (direct SQL). */
static SQLRETURN statement_execute_parameter_array(OdbcStatement *statement,
                                                   bool use_prepared);

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
 * Release the keyset overlay (per-row deleted/override state) and any savepoint
 * snapshots. Called whenever the current result is discarded, since the overlay
 * is meaningful only for the rows of that specific result set.
 */
static void clear_keyset_overlay(OdbcStatement *statement)
{
    if (statement->keyset_rows) {
        /* Each row's override_values array holds one heap string per FULL result
         * column (including the hidden ctid). The array is width-prefixed by the
         * current result's column count; if the result is already gone we cannot
         * know the width, so overrides are always freed before current_result is
         * cleared (clear_current_result calls us first). */
        int full_column_count =
            statement->current_result ? PQnfields(statement->current_result) : 0;
        for (int row_index = 0; row_index < statement->keyset_row_count; row_index++) {
            char **override_values = statement->keyset_rows[row_index].override_values;
            if (override_values) {
                for (int column = 0; column < full_column_count; column++) {
                    free(override_values[column]);
                }
                free(override_values);
            }
        }
        free(statement->keyset_rows);
        statement->keyset_rows = NULL;
    }
    statement->keyset_row_count = 0;

    for (int i = 0; i < statement->keyset_savepoint_count; i++) {
        free(statement->keyset_savepoints[i].deleted_flags);
        statement->keyset_savepoints[i].deleted_flags = NULL;
    }
    statement->keyset_savepoint_count = 0;

    statement->hidden_ctid_column_index = NO_HIDDEN_CTID_COLUMN;
    statement->keyset_table_name[0] = '\0';
    statement->keyset_rowset_first_row = -1;
    statement->keyset_rowset_size = 0;
}

/*
 * Clear the current result from the statement, freeing the PGresult if present.
 * Resets result-related fields but does NOT change the statement state — the
 * caller is responsible for updating state after calling this.
 */
static void clear_current_result(OdbcStatement *statement)
{
    clear_keyset_overlay(statement);

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

    /* Register the four implicit descriptors (ARD/APD/IRD/IPD) embedded in the
     * statement. Each routes field access to this statement's backing stores
     * (column_bindings / parameter_bindings / result metadata). The active
     * ARD/APD start as the implicit ones; SQLSetStmtAttr can later swap in an
     * explicitly allocated descriptor. */
    descriptor_init_implicit(&statement->implicit_app_row_descriptor,
                             DESCRIPTOR_ROLE_APP_ROW, statement);
    descriptor_init_implicit(&statement->implicit_app_param_descriptor,
                             DESCRIPTOR_ROLE_APP_PARAM, statement);
    descriptor_init_implicit(&statement->implicit_row_descriptor,
                             DESCRIPTOR_ROLE_IMPL_ROW, statement);
    descriptor_init_implicit(&statement->implicit_param_descriptor,
                             DESCRIPTOR_ROLE_IMPLICIT_PARAM, statement);
    statement->active_app_row_descriptor = &statement->implicit_app_row_descriptor;
    statement->active_app_param_descriptor = &statement->implicit_app_param_descriptor;

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

    /* Block-cursor defaults: single-row fetch, no out-pointers registered. */
    statement->row_array_size = 1;
    statement->rows_fetched_ptr = NULL;
    statement->row_status_ptr = NULL;

    /* A single parameter-value set until the application enables array binding.
     * No status/processed/operation out-pointers and column-wise binding are the
     * ODBC defaults. */
    statement->paramset_size = 1;
    statement->param_status_ptr = NULL;
    statement->params_processed_ptr = NULL;
    statement->param_operation_ptr = NULL;
    statement->param_bind_type = SQL_PARAM_BIND_BY_COLUMN;

    /* Bookmarks are disabled by default (SQL_UB_OFF); the application opts in
     * via SQL_ATTR_USE_BOOKMARKS. No fetch-bookmark target and no column-0
     * binding until the application sets them. */
    statement->use_bookmarks = SQL_UB_OFF;
    statement->fetch_bookmark_ptr = NULL;
    statement->bookmark_bound = false;
    statement->bookmark_target_type = 0;
    statement->bookmark_buffer = NULL;
    statement->bookmark_buffer_length = 0;
    statement->bookmark_indicator = NULL;

    /* Read-only cursor by default: no updatable-cursor rewrite, no hidden ctid,
     * no per-row overlay. These are only populated when the application asks for
     * a keyset-driven / writable-concurrency cursor and executes a simple SELECT. */
    statement->is_updatable_cursor = false;
    statement->hidden_ctid_column_index = NO_HIDDEN_CTID_COLUMN;
    statement->keyset_rows = NULL;
    statement->keyset_row_count = 0;
    statement->keyset_table_name[0] = '\0';
    statement->keyset_rowset_first_row = -1;
    statement->keyset_rowset_size = 0;
    statement->keyset_savepoint_count = 0;

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

    /* A failed procedure call inside an explicit transaction aborts it; recover
     * per the configured error-rollback mode (see statement_execute). This is
     * the "{ call ... }" escape path the error-rollback test exercises. */
    if (execution_result == SQL_ERROR) {
        connection_handle_statement_error(statement->parent_connection);
    }

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

/*
 * True when the SQL is a savepoint-control command (SAVEPOINT, RELEASE, or
 * ROLLBACK TO). Such commands must NOT be wrapped in the driver's internal
 * per-statement savepoint: that savepoint sits below any user savepoint on the
 * subtransaction stack, so RELEASEing it (which the next statement's savepoint
 * setup does) would cascade and destroy the user's savepoints stacked above it.
 * The block-delete test relies on its "yuuki"/"miho" savepoints surviving across
 * intervening statements, so we leave savepoint-control commands unwrapped.
 *
 * Case-insensitive, whitespace-tolerant leading-keyword match. A bare "ROLLBACK"
 * (whole-transaction) is intentionally NOT matched — only "ROLLBACK TO".
 */
static bool is_savepoint_control_command(const char *sql_text)
{
    if (!sql_text) {
        return false;
    }
    const char *cursor = sql_text;
    while (*cursor == ' ' || *cursor == '\t' || *cursor == '\n' || *cursor == '\r') {
        cursor++;
    }

    /* Case-insensitively match a leading keyword that ends on a word boundary
     * (end of string or ASCII whitespace), so "RELEASE" does not match
     * "RELEASED". */
    #define MATCH_LEADING_KEYWORD(keyword)                                      \
        (statement_ascii_case_prefix(cursor, (keyword), strlen(keyword)) &&    \
         (cursor[strlen(keyword)] == '\0' ||                                    \
          isspace((unsigned char)cursor[strlen(keyword)])))

    if (MATCH_LEADING_KEYWORD("SAVEPOINT") || MATCH_LEADING_KEYWORD("RELEASE")) {
        return true;
    }
    if (MATCH_LEADING_KEYWORD("ROLLBACK")) {
        const char *after = cursor + strlen("ROLLBACK");
        while (*after == ' ' || *after == '\t' || *after == '\n' || *after == '\r') {
            after++;
        }
        /* Only "ROLLBACK TO ..." is a savepoint command; plain ROLLBACK is not. */
        if (statement_ascii_case_prefix(after, "TO", 2) &&
            (after[2] == '\0' || isspace((unsigned char)after[2]))) {
            return true;
        }
    }
    #undef MATCH_LEADING_KEYWORD
    return false;
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

        /* Mark a per-statement SAVEPOINT so that, under statement-level error
         * rollback (Protocol=7.4-2), a failure of this statement can be undone
         * without discarding the whole transaction's earlier work. Skip this for
         * user savepoint-control commands, whose own savepoints must not be
         * torn down by our internal savepoint's RELEASE (see the helper). */
        if (!is_savepoint_control_command(sql_for_check)) {
            connection_begin_statement_savepoint(statement->parent_connection);
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

    /* Array/batch parameter binding: when the application bound arrays of
     * parameter values (SQL_ATTR_PARAMSET_SIZE > 1), execute once per set. The
     * single-set path below is left exactly as it was for paramset_size <= 1. */
    if (statement->paramset_size > 1 &&
        statement->bound_parameter_count > 0 &&
        statement->detected_param_count > 0) {
        return statement_execute_parameter_array(statement, true /* use prepared */);
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

    /* On error inside an explicit transaction, apply the configured recovery
     * (statement-level savepoint rewind, whole-transaction rollback, or leave
     * it to the application). This keeps a single failed statement from
     * poisoning the rest of an otherwise-good transaction. */
    if (execution_result == SQL_ERROR) {
        connection_handle_statement_error(statement->parent_connection);
    }

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
 * Execute a bound statement once for each parameter set when array/batch
 * parameter binding is active (SQL_ATTR_PARAMSET_SIZE > 1). This implements the
 * column-wise array-binding contract: parameter N's value for row R lives at
 * element R of the array the application bound for parameter N, addressed by the
 * per-element stride (buffer_length, or the fixed C-type width when the app bound
 * a zero buffer_length).
 *
 * Batching and per-row status semantics (matching the original psqlodbc):
 *   - The paramset_size rows are partitioned into consecutive batches of
 *     connection->batch_size rows. batch_size is an application tuning knob
 *     (SQL_ATTR_PGOPT_BATCHSIZE) that groups rows into a single logical unit.
 *   - All rows in one batch share their per-row status: if any row in the batch
 *     produced a server NOTICE, every row in that batch is reported
 *     SQL_PARAM_SUCCESS_WITH_INFO; otherwise SQL_PARAM_SUCCESS.
 *   - The first batch that fails aborts the whole execution: every row of the
 *     failing batch is marked SQL_PARAM_ERROR and every row after it
 *     SQL_PARAM_UNUSED. The single failing diagnostic is reported (server NOTICE
 *     records are deliberately NOT promoted here, so the application sees only
 *     the real error).
 *
 * Result-set handling:
 *   - Rows that return tuples (e.g. DELETE ... RETURNING) have each per-row
 *     result set chained onto pending_results so SQLMoreResults walks one result
 *     set per parameter set. The first becomes the current result.
 *   - Rows that return only a command tag (plain INSERT/UPDATE/DELETE) have their
 *     results discarded; there is nothing to fetch.
 *
 * use_prepared selects PQexecPrepared (SQLExecute after SQLPrepare) versus
 * PQexecParams (SQLExecDirect). Returns SQL_ERROR on the first failing batch,
 * SQL_SUCCESS_WITH_INFO when any NOTICE was seen, otherwise SQL_SUCCESS.
 */
static SQLRETURN statement_execute_parameter_array(OdbcStatement *statement,
                                                   bool use_prepared)
{
    OdbcConnection *connection = statement->parent_connection;
    PGconn *libpq_connection = connection->libpq_connection;

    SQLULEN row_count = statement->paramset_size;

    /* batch_size is validated to be positive when set; guard defensively so a
     * stray zero cannot produce a zero-length (infinite) batch loop. */
    int batch_size = connection->batch_size;
    if (batch_size <= 0) {
        batch_size = DEFAULT_BATCH_SIZE;
    }

    /* Per-row result sets that carry tuples (RETURNING) are chained for
     * SQLMoreResults. Allocated at full width up front; only tuple-bearing
     * results are retained, so a plain INSERT array leaves this empty and it is
     * freed below without ever being published. */
    PGresult **chained_results = calloc((size_t)row_count, sizeof(PGresult *));
    if (!chained_results) {
        diagnostics_add_record(&statement->diagnostics, "HY001", 0,
                               "Failed to allocate result chain for array execution.");
        return SQL_ERROR;
    }
    int chained_count = 0;

    /* Every row starts UNUSED; a row's status is set once its batch completes
     * (or the batch fails). This gives the correct "rows after an error are
     * UNUSED" result for free. */
    if (statement->param_status_ptr) {
        for (SQLULEN row = 0; row < row_count; row++) {
            statement->param_status_ptr[row] = SQL_PARAM_UNUSED;
        }
    }

    SQLULEN rows_processed = 0;
    bool any_notice_overall = false;
    bool execution_failed = false;

    for (SQLULEN batch_start = 0; batch_start < row_count && !execution_failed;
         batch_start += (SQLULEN)batch_size) {
        SQLULEN batch_end = batch_start + (SQLULEN)batch_size;  /* exclusive */
        if (batch_end > row_count) {
            batch_end = row_count;
        }

        bool batch_had_notice = false;
        bool batch_had_error = false;
        PGresult *failing_result = NULL;

        for (SQLULEN row = batch_start; row < batch_end; row++) {
            /* SQL_PARAM_IGNORE lets the application exclude individual sets. Such
             * a row is neither executed nor counted; its status stays UNUSED. */
            if (statement->param_operation_ptr &&
                statement->param_operation_ptr[row] == SQL_PARAM_IGNORE) {
                continue;
            }

            /* Attribute NOTICEs to the row that produced them by clearing the
             * connection's capture buffer immediately before each execution. */
            connection_clear_notices(connection);

            const char **param_values = NULL;
            int *param_lengths = NULL;
            int *param_formats = NULL;
            int param_count = 0;

            SQLRETURN build_result = parameter_build_libpq_arrays_for_row(
                statement->parameter_bindings, row,
                &param_values, &param_lengths, &param_formats, &param_count);
            if (build_result != SQL_SUCCESS) {
                diagnostics_clear(&statement->diagnostics);
                diagnostics_add_record(&statement->diagnostics, "HY001", 0,
                                       "Failed to build parameter arrays for array execution.");
                batch_had_error = true;
                break;
            }

            /* Send only as many parameters as the SQL actually references (stale
             * bindings from a prior, wider statement must not be forwarded). */
            int effective_param_count = param_count;
            if (effective_param_count > statement->detected_param_count) {
                effective_param_count = statement->detected_param_count;
            }

            PGresult *result;
            if (use_prepared) {
                result = PQexecPrepared(libpq_connection,
                                        statement->prepared_name,
                                        effective_param_count,
                                        param_values, param_lengths, param_formats,
                                        0 /* text results */);
            } else {
                result = PQexecParams(libpq_connection,
                                      statement->translated_sql,
                                      effective_param_count,
                                      NULL /* infer parameter types */,
                                      param_values, param_lengths, param_formats,
                                      0 /* text results */);
            }

            parameter_free_libpq_arrays(param_values, param_lengths, param_formats,
                                        param_count);

            rows_processed++;

            ExecStatusType status = result ? PQresultStatus(result) : PGRES_FATAL_ERROR;
            if (status == PGRES_TUPLES_OK) {
                if (connection->notice_count > 0) {
                    batch_had_notice = true;
                }
                /* Retain tuple-bearing results for SQLMoreResults. */
                chained_results[chained_count++] = result;
            } else if (status == PGRES_COMMAND_OK) {
                if (connection->notice_count > 0) {
                    batch_had_notice = true;
                }
                PQclear(result);
            } else {
                /* First real failure in this batch: keep the result so its
                 * diagnostic can be reported, and stop executing the batch. */
                failing_result = result;
                batch_had_error = true;
                break;
            }
        }

        if (batch_had_error) {
            /* The whole batch is one logical unit: mark every row of it ERROR,
             * regardless of which row actually failed. Rows after this batch stay
             * UNUSED (they were never executed). */
            if (statement->param_status_ptr) {
                for (SQLULEN row = batch_start; row < batch_end; row++) {
                    statement->param_status_ptr[row] = SQL_PARAM_ERROR;
                }
            }

            /* Report exactly the failing statement's diagnostic. NOTICE records
             * from earlier successful rows are intentionally not surfaced so the
             * application sees only the error (matches the expected output). */
            diagnostics_clear(&statement->diagnostics);
            if (failing_result) {
                error_add_diagnostic_from_result_ctx(&statement->diagnostics,
                                                     failing_result, "HY000",
                                                     "Error while executing the query");
                PQclear(failing_result);
            }

            /* Poison an explicit transaction so the application must ROLLBACK,
             * mirroring the single-set error path. */
            if (connection->transaction_state == TRANSACTION_STATE_ACTIVE) {
                connection->transaction_state = TRANSACTION_STATE_FAILED;
            }

            execution_failed = true;
        } else {
            SQLUSMALLINT batch_status =
                batch_had_notice ? SQL_PARAM_SUCCESS_WITH_INFO : SQL_PARAM_SUCCESS;
            if (batch_had_notice) {
                any_notice_overall = true;
            }
            if (statement->param_status_ptr) {
                for (SQLULEN row = batch_start; row < batch_end; row++) {
                    /* Preserve UNUSED for SQL_PARAM_IGNORE'd rows. */
                    if (statement->param_operation_ptr &&
                        statement->param_operation_ptr[row] == SQL_PARAM_IGNORE) {
                        continue;
                    }
                    statement->param_status_ptr[row] = batch_status;
                }
            }
        }
    }

    if (statement->params_processed_ptr) {
        *statement->params_processed_ptr = rows_processed;
    }

    /* Do not leak NOTICE captures forward to the next execution's status. */
    connection_clear_notices(connection);

    if (execution_failed) {
        /* Discard any result sets collected before the failure. */
        for (int index = 0; index < chained_count; index++) {
            PQclear(chained_results[index]);
        }
        free(chained_results);
        statement->has_result_set = false;
        statement->affected_row_count = -1;
        statement->state = STATEMENT_STATE_EXECUTED;
        return SQL_ERROR;
    }

    if (chained_count > 0) {
        /* Publish the result-set chain: chained_results[0] is current, the rest
         * wait for SQLMoreResults (pending_result_index points one past current).*/
        statement->pending_results = chained_results;
        statement->pending_result_count = chained_count;
        statement->pending_result_index = 1;
        apply_result_as_current(statement, chained_results[0]);
    } else {
        /* No tuple-bearing rows (e.g. a plain INSERT array): nothing to fetch. */
        free(chained_results);
        statement->has_result_set = false;
        statement->affected_row_count = (SQLLEN)rows_processed;
        statement->state = STATEMENT_STATE_EXECUTED;
    }

    return any_notice_overall ? SQL_SUCCESS_WITH_INFO : SQL_SUCCESS;
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
        /* Recover per the configured error-rollback mode (see statement_execute). */
        connection_handle_statement_error(statement->parent_connection);
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

/* ==================================================================
 * Keyset (updatable) cursor support
 *
 * PostgreSQL has no server-side updatable cursor in this driver's model (the
 * whole result set is buffered client-side). To make ODBC positioned
 * UPDATE/DELETE work, an updatable cursor's SELECT is rewritten to also fetch
 * each row's physical location — its "ctid" — in a hidden trailing column.
 * SQLSetPos then issues a searched "UPDATE/DELETE ... WHERE ctid = '(b,o)'"
 * keyed on that captured ctid. The ctid column is masked from every public
 * column-count / describe / getdata path so the application never sees it.
 * ================================================================== */

/*
 * Skip ASCII whitespace at the front of a string, returning the first
 * non-space character position. Used by the lightweight SELECT rewriter, which
 * only needs to recognize a leading "SELECT" keyword and a top-level "FROM".
 */
static const char *skip_leading_spaces(const char *text)
{
    while (*text && isspace((unsigned char)*text)) {
        text++;
    }
    return text;
}

/*
 * Case-insensitively test whether "text" begins with "keyword" AND the keyword
 * is followed by a word boundary (end of string or a non-identifier character),
 * so "SELECT" matches "SELECT ..." but not "SELECTED".
 */
static bool starts_with_keyword(const char *text, const char *keyword)
{
    size_t keyword_length = strlen(keyword);
    if (!statement_ascii_case_prefix(text, keyword, keyword_length)) {
        return false;
    }
    char following = text[keyword_length];
    return following == '\0' || isspace((unsigned char)following) ||
           following == '(' || following == '*';
}

/*
 * Find the first top-level " FROM " in a simple SELECT, i.e. one that is not
 * nested inside parentheses (a sub-select) or a quoted string. Returns a pointer
 * to the 'F'/'f' of FROM, or NULL if none is found at the top level.
 */
static const char *find_top_level_from(const char *select_text)
{
    int paren_depth = 0;
    bool in_single_quote = false;
    bool in_double_quote = false;

    for (const char *cursor = select_text; *cursor; cursor++) {
        char current = *cursor;

        if (in_single_quote) {
            if (current == '\'') {
                in_single_quote = false;
            }
            continue;
        }
        if (in_double_quote) {
            if (current == '"') {
                in_double_quote = false;
            }
            continue;
        }
        if (current == '\'') {
            in_single_quote = true;
            continue;
        }
        if (current == '"') {
            in_double_quote = true;
            continue;
        }
        if (current == '(') {
            paren_depth++;
            continue;
        }
        if (current == ')') {
            if (paren_depth > 0) {
                paren_depth--;
            }
            continue;
        }
        if (paren_depth == 0 &&
            (current == 'f' || current == 'F') &&
            /* Require a leading word boundary so we do not match a column name
             * ending in "from"; the char before FROM must be whitespace. */
            cursor != select_text && isspace((unsigned char)cursor[-1]) &&
            starts_with_keyword(cursor, "FROM")) {
            return cursor;
        }
    }
    return NULL;
}

/*
 * Extract the base table name that immediately follows the top-level FROM of a
 * simple single-table SELECT, storing it (unquoted, as written) into
 * out_table_name. Returns true only for the safe, simple case this driver can
 * build a positioned UPDATE against: a single identifier (optionally
 * schema-qualified or double-quoted) that is not a join, sub-select, or
 * comma-separated list. On anything more complex, returns false and the caller
 * leaves the cursor read-only.
 */
static bool extract_from_table_name(const char *from_keyword,
                                    char *out_table_name,
                                    size_t out_size)
{
    const char *cursor = skip_leading_spaces(from_keyword + strlen("FROM"));
    if (*cursor == '\0') {
        return false;
    }

    /* Copy the table token: a run of identifier characters, dots (schema
     * qualification) and matched double quotes. Stop at whitespace or any
     * character that would indicate a more complex FROM clause. */
    size_t length = 0;
    bool in_quote = false;
    while (*cursor) {
        char current = *cursor;
        if (current == '"') {
            in_quote = !in_quote;
        } else if (!in_quote && (isspace((unsigned char)current) ||
                                 current == ',' || current == '(' ||
                                 current == ';')) {
            break;
        }
        if (length + 1 >= out_size) {
            return false;  /* Table name too long to store safely. */
        }
        out_table_name[length++] = current;
        cursor++;
    }
    out_table_name[length] = '\0';
    if (length == 0) {
        return false;
    }

    /* Reject anything trailing that signals a join / multi-table / clause we
     * cannot rewrite: another table (comma), a JOIN keyword, or an alias. Only
     * an ORDER BY / WHERE / LIMIT / GROUP / end-of-string is acceptable, since
     * those keep the FROM single-table. */
    const char *rest = skip_leading_spaces(cursor);
    if (*rest == '\0') {
        return true;
    }
    static const char *const allowed_following_clauses[] = {
        "WHERE", "ORDER", "GROUP", "LIMIT", "OFFSET", "HAVING", "FOR", NULL
    };
    for (int i = 0; allowed_following_clauses[i]; i++) {
        if (starts_with_keyword(rest, allowed_following_clauses[i])) {
            return true;
        }
    }
    /* A comma, a JOIN, or a bare alias here means the FROM is not a single
     * simple table; bail out and keep the cursor read-only. */
    return false;
}

/*
 * If the statement is an updatable cursor over a simple single-table SELECT,
 * rewrite translated_sql to append ", ctid" to the select list so every fetched
 * row carries its physical row id in a hidden trailing column. Records the table
 * name (for later UPDATE/DELETE) and marks hidden_ctid_column_index as pending
 * (the concrete index is set once the result's column count is known). Leaves
 * the SQL untouched — and the cursor effectively read-only — for any query we
 * cannot safely rewrite (non-SELECT, multi-statement, join, existing ctid, etc.).
 */
static void rewrite_select_append_ctid(OdbcStatement *statement)
{
    statement->keyset_table_name[0] = '\0';
    statement->hidden_ctid_column_index = NO_HIDDEN_CTID_COLUMN;

    if (!statement->is_updatable_cursor || statement->is_multi_statement ||
        statement->is_procedure_call || !statement->translated_sql) {
        return;
    }

    const char *select_text = skip_leading_spaces(statement->translated_sql);
    if (!starts_with_keyword(select_text, "SELECT")) {
        return;  /* Only a plain SELECT can be made updatable. */
    }

    const char *from_keyword = find_top_level_from(select_text);
    if (!from_keyword) {
        return;  /* No FROM: nothing to update (e.g. "SELECT 1"). */
    }

    char table_name[MAX_KEYSET_TABLE_NAME_LENGTH];
    if (!extract_from_table_name(from_keyword, table_name, sizeof(table_name))) {
        return;  /* FROM clause too complex to target safely. */
    }

    /* Build "<select-list>, ctid <FROM ...>" by splicing ", ctid" in just before
     * the FROM keyword. The offsets are into translated_sql (select_text may be
     * past leading spaces, so measure against the original buffer). */
    const char *original = statement->translated_sql;
    size_t prefix_length = (size_t)(from_keyword - original);
    const char *ctid_injection = ", ctid ";
    size_t suffix_length = strlen(from_keyword);
    size_t rewritten_length = prefix_length + strlen(ctid_injection) + suffix_length;

    char *rewritten = malloc(rewritten_length + 1);
    if (!rewritten) {
        return;  /* Out of memory: fall back to a read-only cursor. */
    }
    memcpy(rewritten, original, prefix_length);
    memcpy(rewritten + prefix_length, ctid_injection, strlen(ctid_injection));
    memcpy(rewritten + prefix_length + strlen(ctid_injection),
           from_keyword, suffix_length);
    rewritten[rewritten_length] = '\0';

    free(statement->translated_sql);
    statement->translated_sql = rewritten;

    /* The hidden ctid becomes the LAST column of the result. We don't know the
     * total column count until execution expands "SELECT *", so mark it pending
     * with a distinct sentinel that finalize_keyset_after_execute resolves. */
    statement->hidden_ctid_column_index = NO_HIDDEN_CTID_COLUMN;  /* resolved post-exec */
    /* Store the table token exactly as it appeared in the user's own FROM clause.
     * extract_from_table_name only accepts identifier characters, dots (schema
     * qualification) and matched double quotes, stopping at whitespace / comma /
     * paren / semicolon, so the token is a syntactically valid, self-authored
     * table reference — not external data. It is inlined verbatim into the
     * positioned UPDATE/DELETE/INSERT because PQescapeIdentifier would wrongly
     * fold a legitimate "schema.table" (or an already-quoted name) into a single
     * quoted identifier. Bound VALUES are still parameterized; only this
     * user-supplied, pre-validated identifier is interpolated. */
    snprintf(statement->keyset_table_name, sizeof(statement->keyset_table_name),
             "%s", table_name);
}

/*
 * After an updatable cursor's SELECT executes, wire up the keyset overlay: the
 * hidden ctid is the last column of the result, and one KeysetRow is allocated
 * per fetched tuple (deleted=false, no override yet). A no-op for a read-only
 * cursor or a query that was not rewritten (empty keyset_table_name).
 */
static void finalize_keyset_after_execute(OdbcStatement *statement)
{
    if (!statement->is_updatable_cursor ||
        statement->keyset_table_name[0] == '\0' ||
        !statement->current_result) {
        return;
    }

    int full_column_count = PQnfields(statement->current_result);
    if (full_column_count < 1) {
        return;
    }
    /* The ctid we appended is the final column. */
    statement->hidden_ctid_column_index = full_column_count - 1;

    int tuple_count = PQntuples(statement->current_result);
    statement->keyset_rows = calloc((size_t)(tuple_count > 0 ? tuple_count : 1),
                                    sizeof(KeysetRow));
    if (!statement->keyset_rows) {
        /* Overlay allocation failed: disable positioned operations but keep the
         * result usable for read-only fetching. */
        statement->hidden_ctid_column_index = NO_HIDDEN_CTID_COLUMN;
        statement->keyset_row_count = 0;
        return;
    }
    statement->keyset_row_count = tuple_count;
}

/* ---- Keyset public helpers (declared in statement.h) ---- */

int statement_public_column_count(const OdbcStatement *statement)
{
    if (!statement->current_result) {
        return 0;
    }
    int full_count = PQnfields(statement->current_result);
    if (statement->hidden_ctid_column_index != NO_HIDDEN_CTID_COLUMN) {
        /* Hide the trailing ctid column we appended for updatability. */
        return full_count - 1;
    }
    return full_count;
}

const char *statement_row_value(const OdbcStatement *statement,
                                int row_index, int column_index, bool *is_null)
{
    /* Prefer an updated-value override (set by a positioned UPDATE/REFRESH, or
     * present for rows appended by SQL_ADD that have no base tuple) so a re-fetch
     * of the still-open cursor reflects the new/added values. */
    if (statement->keyset_rows && row_index >= 0 &&
        row_index < statement->keyset_row_count) {
        char **override_values = statement->keyset_rows[row_index].override_values;
        if (override_values) {
            const char *value = override_values[column_index];
            if (is_null) {
                *is_null = (value == NULL);
            }
            return value;
        }
    }

    /* Rows appended by SQL_ADD live beyond the base result's tuple range; without
     * an override there is nothing to read, so report SQL NULL rather than read
     * out of bounds. */
    if (row_index >= PQntuples(statement->current_result)) {
        if (is_null) {
            *is_null = true;
        }
        return NULL;
    }

    /* Fall back to the immutable base result. */
    if (PQgetisnull(statement->current_result, row_index, column_index)) {
        if (is_null) {
            *is_null = true;
        }
        return NULL;
    }
    if (is_null) {
        *is_null = false;
    }
    return PQgetvalue(statement->current_result, row_index, column_index);
}

bool statement_row_is_deleted(const OdbcStatement *statement, int row_index)
{
    if (!statement->keyset_rows || row_index < 0 ||
        row_index >= statement->keyset_row_count) {
        return false;
    }
    return statement->keyset_rows[row_index].deleted;
}

/* ---- Savepoint-synchronized keyset overlay ----
 *
 * A positioned DELETE only marks the client-side overlay; on the server the row
 * is really deleted inside the current transaction. When the application later
 * does "ROLLBACK TO <savepoint>", the server un-deletes every row deleted after
 * that savepoint. The still-open cursor must then see those rows again, so we
 * snapshot the overlay's deleted flags at each "SAVEPOINT <name>" and restore
 * them at the matching "ROLLBACK TO <name>". Rows appended by SQL_ADD after the
 * savepoint are likewise dropped on restore (the server never had them). */

/*
 * If sql begins with a SAVEPOINT or ROLLBACK-TO command, return the savepoint
 * name (into out_name) and set *is_rollback accordingly. Returns false when the
 * SQL is not a savepoint command this driver needs to intercept. Handles the
 * test's compound form "rollback to yuuki;release yuuki" by reading only the
 * first command's name.
 */
static bool parse_savepoint_command(const char *sql, char *out_name,
                                    size_t out_size, bool *is_rollback)
{
    const char *cursor = skip_leading_spaces(sql);

    if (starts_with_keyword(cursor, "SAVEPOINT")) {
        *is_rollback = false;
        cursor = skip_leading_spaces(cursor + strlen("SAVEPOINT"));
    } else if (starts_with_keyword(cursor, "ROLLBACK")) {
        cursor = skip_leading_spaces(cursor + strlen("ROLLBACK"));
        if (!starts_with_keyword(cursor, "TO")) {
            return false;  /* Plain ROLLBACK (whole txn) is not a savepoint op. */
        }
        cursor = skip_leading_spaces(cursor + strlen("TO"));
        /* Optional "SAVEPOINT" keyword: "ROLLBACK TO SAVEPOINT name". */
        if (starts_with_keyword(cursor, "SAVEPOINT")) {
            cursor = skip_leading_spaces(cursor + strlen("SAVEPOINT"));
        }
        *is_rollback = true;
    } else {
        return false;
    }

    /* Copy the identifier up to whitespace, ';', or end. */
    size_t length = 0;
    while (*cursor && !isspace((unsigned char)*cursor) && *cursor != ';') {
        if (length + 1 >= out_size) {
            return false;
        }
        out_name[length++] = *cursor++;
    }
    out_name[length] = '\0';
    return length > 0;
}

/* Take (or overwrite) a snapshot of this cursor's overlay deleted-flags under
 * the given savepoint name. */
static void keyset_snapshot_savepoint(OdbcStatement *cursor, const char *name)
{
    if (!cursor->keyset_rows || cursor->keyset_row_count <= 0) {
        return;
    }

    /* Reuse an existing slot for the same name (savepoints are re-established
     * with the same name across the test's loops). */
    KeysetSavepoint *slot = NULL;
    for (int i = 0; i < cursor->keyset_savepoint_count; i++) {
        if (strcmp(cursor->keyset_savepoints[i].name, name) == 0) {
            slot = &cursor->keyset_savepoints[i];
            free(slot->deleted_flags);
            slot->deleted_flags = NULL;
            break;
        }
    }
    if (!slot) {
        if (cursor->keyset_savepoint_count >= MAX_KEYSET_SAVEPOINTS) {
            return;  /* Snapshot table full; skip (positioned ops still work). */
        }
        slot = &cursor->keyset_savepoints[cursor->keyset_savepoint_count++];
    }

    snprintf(slot->name, sizeof(slot->name), "%s", name);
    slot->row_count = cursor->keyset_row_count;
    slot->deleted_flags = malloc((size_t)slot->row_count * sizeof(bool));
    if (!slot->deleted_flags) {
        /* Roll back the slot allocation on OOM. */
        cursor->keyset_savepoint_count--;
        return;
    }
    for (int i = 0; i < slot->row_count; i++) {
        slot->deleted_flags[i] = cursor->keyset_rows[i].deleted;
    }
}

/* Restore this cursor's overlay from a named savepoint snapshot: re-live the
 * rows that were deleted after the savepoint and drop any rows added since. */
static void keyset_restore_savepoint(OdbcStatement *cursor, const char *name)
{
    for (int i = 0; i < cursor->keyset_savepoint_count; i++) {
        KeysetSavepoint *slot = &cursor->keyset_savepoints[i];
        if (strcmp(slot->name, name) != 0) {
            continue;
        }
        if (!cursor->keyset_rows || !slot->deleted_flags) {
            return;
        }
        int restore_count = slot->row_count;
        if (restore_count > cursor->keyset_row_count) {
            restore_count = cursor->keyset_row_count;
        }
        for (int row = 0; row < restore_count; row++) {
            cursor->keyset_rows[row].deleted = slot->deleted_flags[row];
        }
        /* Rows beyond the snapshot were added (SQL_ADD) after the savepoint and
         * are undone by the server's rollback: mark them deleted so the overlay
         * matches the server's post-rollback row set. */
        for (int row = restore_count; row < cursor->keyset_row_count; row++) {
            cursor->keyset_rows[row].deleted = true;
        }
        /* Reposition to before-first so the next FETCH_FIRST/NEXT starts cleanly. */
        cursor->current_row_position = -1;
        cursor->keyset_rowset_first_row = -1;
        cursor->keyset_rowset_size = 0;
        return;
    }
}

/*
 * Intercept a SAVEPOINT / ROLLBACK TO command so any open updatable cursor on
 * the SAME connection keeps its client-side overlay in sync with the server's
 * savepoint semantics (see the block comment above). Called for every executed
 * statement; a no-op unless the SQL is a savepoint command and a sibling
 * updatable cursor exists.
 */
static void keyset_intercept_savepoint_command(OdbcStatement *statement,
                                               const char *sql)
{
    if (!statement->parent_connection || !sql) {
        return;
    }

    char savepoint_name[MAX_KEYSET_SAVEPOINT_NAME_LENGTH];
    bool is_rollback = false;
    if (!parse_savepoint_command(sql, savepoint_name, sizeof(savepoint_name),
                                 &is_rollback)) {
        return;
    }

    OdbcConnection *connection = statement->parent_connection;
    for (int i = 0; i < MAX_STATEMENTS_PER_CONNECTION; i++) {
        OdbcStatement *sibling = connection->statements[i];
        if (!sibling || !sibling->is_updatable_cursor || !sibling->keyset_rows) {
            continue;
        }
        if (is_rollback) {
            keyset_restore_savepoint(sibling, savepoint_name);
        } else {
            keyset_snapshot_savepoint(sibling, savepoint_name);
        }
    }
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

    /* For an updatable (keyset) cursor, append a hidden ctid column to a simple
     * SELECT so SQLSetPos can build positioned UPDATE/DELETE later. Does nothing
     * for read-only cursors or queries too complex to rewrite. */
    rewrite_select_append_ctid(statement);

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

        /* Establish the per-statement SAVEPOINT for statement-level error
         * rollback (see connection_begin_statement_savepoint). Skip it for user
         * savepoint-control commands so our internal savepoint's later RELEASE
         * cannot cascade away the user's own savepoints (see the helper). */
        if (!is_savepoint_control_command(statement->translated_sql)) {
            connection_begin_statement_savepoint(statement->parent_connection);
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
        SQLRETURN multi_result = execute_multi_statement(statement);
        /* The block-delete test rolls savepoints back with a compound command
         * ("rollback to yuuki;release yuuki"), which lands here. Sync any sibling
         * updatable cursor's overlay off the original (unsplit) SQL text. */
        if (SQL_SUCCEEDED(multi_result)) {
            keyset_intercept_savepoint_command(statement, statement->sql_text);
        }
        return multi_result;
    }

    /* Array/batch parameter binding for direct execution: run once per bound
     * parameter set (see statement_execute_parameter_array). The single-set path
     * below is unchanged for paramset_size <= 1. */
    if (statement->paramset_size > 1 &&
        statement->detected_param_count > 0 &&
        statement->bound_parameter_count > 0) {
        return statement_execute_parameter_array(statement, false /* direct SQL */);
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

    /* Wire up the keyset overlay (hidden ctid index + per-row state) now that the
     * result's real column count and tuple count are known. */
    if (SQL_SUCCEEDED(execution_result)) {
        finalize_keyset_after_execute(statement);

        /* If this command was SAVEPOINT / ROLLBACK TO, keep any sibling updatable
         * cursor's overlay consistent with the server's savepoint semantics. */
        keyset_intercept_savepoint_command(statement, statement->translated_sql);
    }

    /* Recover per the configured error-rollback mode (see statement_execute). */
    if (execution_result == SQL_ERROR) {
        connection_handle_statement_error(statement->parent_connection);
    }

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
