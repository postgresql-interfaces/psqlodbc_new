/*-------------------------------------------------------------------------
 *
 * error_mapping.c
 *	  SQLSTATE extraction from libpq errors
 *
 * Copyright (c) 2026, Dave Cramer
 *
 *
 * IDENTIFICATION
 *	  src/error_mapping.c
 *
 *-------------------------------------------------------------------------
 */
#include "error_mapping.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Length of a valid SQLSTATE code (always exactly 5 characters) */
#define SQLSTATE_CODE_LENGTH 5

void error_extract_from_result(const PGresult *result,
                               const char *fallback_sqlstate,
                               char *out_sqlstate,
                               char *out_message,
                               size_t message_buffer_size)
{
    if (!out_sqlstate || !out_message || message_buffer_size == 0) {
        return;
    }

    /* Default the outputs in case we return early */
    out_message[0] = '\0';

    /* Extract SQLSTATE from the result.
     * PostgreSQL sets this for all server-generated errors. It will be NULL
     * only for client-side errors (e.g., out-of-memory in libpq itself). */
    const char *pg_sqlstate = NULL;
    if (result) {
        pg_sqlstate = PQresultErrorField(result, PG_DIAG_SQLSTATE);
    }

    if (pg_sqlstate && strlen(pg_sqlstate) == SQLSTATE_CODE_LENGTH) {
        memcpy(out_sqlstate, pg_sqlstate, SQLSTATE_CODE_LENGTH);
        out_sqlstate[SQLSTATE_CODE_LENGTH] = '\0';
    } else {
        /* No SQLSTATE available — use the caller-provided fallback */
        if (fallback_sqlstate) {
            strncpy(out_sqlstate, fallback_sqlstate, SQLSTATE_CODE_LENGTH);
            out_sqlstate[SQLSTATE_CODE_LENGTH] = '\0';
        } else {
            memcpy(out_sqlstate, "HY000", SQLSTATE_CODE_LENGTH + 1);
        }
    }

    if (!result) {
        snprintf(out_message, message_buffer_size, "Unknown error (no result available).");
        return;
    }

    /* Extract the message components from the PGresult.
     * PG_DIAG_MESSAGE_PRIMARY is always set for errors; DETAIL and HINT
     * are optional and provide additional context. */
    const char *primary_message = PQresultErrorField(result, PG_DIAG_MESSAGE_PRIMARY);
    const char *detail_message = PQresultErrorField(result, PG_DIAG_MESSAGE_DETAIL);
    const char *hint_message = PQresultErrorField(result, PG_DIAG_MESSAGE_HINT);

    if (!primary_message) {
        /* Fallback to the full error message string if no primary is available */
        primary_message = PQresultErrorMessage(result);
    }

    if (!primary_message) {
        snprintf(out_message, message_buffer_size, "Query execution failed.");
        return;
    }

    /* Build the combined message. Start with the primary message, then
     * append DETAIL and HINT on separate lines if available. This matches
     * what psql displays and what applications expect. */
    int written = snprintf(out_message, message_buffer_size, "%s", primary_message);

    if (detail_message && written > 0 && (size_t)written < message_buffer_size) {
        written += snprintf(out_message + written,
                            message_buffer_size - (size_t)written,
                            "\nDETAIL: %s", detail_message);
    }

    if (hint_message && written > 0 && (size_t)written < message_buffer_size) {
        snprintf(out_message + written,
                 message_buffer_size - (size_t)written,
                 "\nHINT: %s", hint_message);
    }
}

void error_extract_from_connection(const PGconn *connection,
                                   const char *fallback_sqlstate,
                                   char *out_sqlstate,
                                   char *out_message,
                                   size_t message_buffer_size)
{
    if (!out_sqlstate || !out_message || message_buffer_size == 0) {
        return;
    }

    /* PGconn does not carry SQLSTATE information — only a text error message.
     * Use the caller's fallback (e.g., "08001" for connection failure). */
    if (fallback_sqlstate) {
        strncpy(out_sqlstate, fallback_sqlstate, SQLSTATE_CODE_LENGTH);
        out_sqlstate[SQLSTATE_CODE_LENGTH] = '\0';
    } else {
        memcpy(out_sqlstate, "HY000", SQLSTATE_CODE_LENGTH + 1);
    }

    const char *error_text = NULL;
    if (connection) {
        error_text = PQerrorMessage(connection);
    }

    if (!error_text || error_text[0] == '\0') {
        snprintf(out_message, message_buffer_size, "Connection error (no details available).");
        return;
    }

    /* Copy the message, trimming the trailing newline that libpq always appends.
     * This keeps diagnostic messages consistent (our other messages don't end
     * with newlines, and ODBC applications don't expect them). */
    size_t message_length = strlen(error_text);
    while (message_length > 0 &&
           (error_text[message_length - 1] == '\n' || error_text[message_length - 1] == '\r')) {
        message_length--;
    }

    if (message_length >= message_buffer_size) {
        message_length = message_buffer_size - 1;
    }

    memcpy(out_message, error_text, message_length);
    out_message[message_length] = '\0';
}

void error_add_diagnostic_from_result(DiagnosticRecords *diagnostics,
                                      const PGresult *result,
                                      const char *fallback_sqlstate)
{
    if (!diagnostics) {
        return;
    }

    char sqlstate[SQLSTATE_CODE_LENGTH + 1];
    char message[MAX_ERROR_MESSAGE_LENGTH];

    error_extract_from_result(result, fallback_sqlstate, sqlstate, message, sizeof(message));

    /* Use PG_DIAG_STATEMENT_POSITION as the native error code.
     * This is the character offset (1-based) in the query where the error
     * was detected — useful for highlighting the error location in tools.
     * Returns NULL if position is not available (e.g., runtime errors). */
    int native_error_code = 0;
    if (result) {
        const char *position_text = PQresultErrorField(result, PG_DIAG_STATEMENT_POSITION);
        if (position_text) {
            native_error_code = atoi(position_text);
        }
    }

    diagnostics_add_record(diagnostics, sqlstate, native_error_code, message);
}

void error_add_diagnostic_from_connection(DiagnosticRecords *diagnostics,
                                          const PGconn *connection,
                                          const char *fallback_sqlstate)
{
    if (!diagnostics) {
        return;
    }

    char sqlstate[SQLSTATE_CODE_LENGTH + 1];
    char message[MAX_ERROR_MESSAGE_LENGTH];

    error_extract_from_connection(connection, fallback_sqlstate, sqlstate, message, sizeof(message));

    /* No position information available at the connection level */
    diagnostics_add_record(diagnostics, sqlstate, 0, message);
}
