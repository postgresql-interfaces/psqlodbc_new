/*-------------------------------------------------------------------------
 *
 * error_mapping.h
 *	  Error mapping function declarations
 *
 * Copyright (c) 2026, Dave Cramer
 *
 *
 * IDENTIFICATION
 *	  src/error_mapping.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PSQLODBC2_ERROR_MAPPING_H
#define PSQLODBC2_ERROR_MAPPING_H

#include "psqlodbc2.h"
#include "diagnostics.h"
#include <libpq-fe.h>

/* Maximum size for a formatted error message that combines the primary message,
 * DETAIL, and HINT fields from PostgreSQL. This is generous to avoid truncation
 * of long constraint-violation messages that include column names and values. */
#define MAX_ERROR_MESSAGE_LENGTH 4096

/*
 * Extract SQLSTATE and a rich error message from a PGresult.
 *
 * Combines the primary message, DETAIL, and HINT fields from the result into
 * a single formatted string. The format is:
 *   "<primary message>\nDETAIL: <detail>\nHINT: <hint>"
 * Only non-NULL fields are included.
 *
 * out_sqlstate: buffer for 5-char SQLSTATE + null (must be >= 6 bytes).
 *              Receives the PostgreSQL SQLSTATE if available, otherwise
 *              receives fallback_sqlstate.
 * out_message: buffer for the formatted message.
 * message_buffer_size: size of out_message buffer.
 * fallback_sqlstate: used when the PGresult has no SQLSTATE field (e.g.,
 *                    connection-level errors or incomplete results).
 */
void error_extract_from_result(const PGresult *result,
                               const char *fallback_sqlstate,
                               char *out_sqlstate,
                               char *out_message,
                               size_t message_buffer_size);

/*
 * Extract error information from a PGconn (for connection-level errors
 * where no PGresult is available).
 *
 * PGconn does not carry SQLSTATE — only a text error message via
 * PQerrorMessage. This function copies that message (trimming the trailing
 * newline that libpq always appends) and uses fallback_sqlstate for the
 * SQLSTATE output.
 *
 * out_sqlstate: buffer for 5-char SQLSTATE + null (must be >= 6 bytes).
 * out_message: buffer for the error message text.
 * message_buffer_size: size of out_message buffer.
 * fallback_sqlstate: the SQLSTATE to use (e.g., "08001" for connection failure).
 */
void error_extract_from_connection(const PGconn *connection,
                                   const char *fallback_sqlstate,
                                   char *out_sqlstate,
                                   char *out_message,
                                   size_t message_buffer_size);

/*
 * Convenience: extract error from a PGresult and add it as a diagnostic record.
 *
 * Extracts SQLSTATE, message (with detail/hint), and statement position
 * from the result, then calls diagnostics_add_record. The native_error_code
 * is set to the character position from PG_DIAG_STATEMENT_POSITION if available
 * (useful for editor integration), or 0 if not.
 *
 * fallback_sqlstate: used when the PGresult has no SQLSTATE field.
 */
void error_add_diagnostic_from_result(DiagnosticRecords *diagnostics,
                                      const PGresult *result,
                                      const char *fallback_sqlstate);

/*
 * Like error_add_diagnostic_from_result, but appends a driver-level context
 * string to the message (joined with ";\n"), matching the original psqlodbc
 * diagnostic format. driver_context describes the operation that failed, e.g.
 * "Error while executing the query" or "Error while preparing parameters".
 * Pass NULL for driver_context to behave identically to the non-ctx variant.
 */
void error_add_diagnostic_from_result_ctx(DiagnosticRecords *diagnostics,
                                          const PGresult *result,
                                          const char *fallback_sqlstate,
                                          const char *driver_context);

/*
 * Convenience: extract error from a PGconn and add it as a diagnostic record.
 *
 * For connection-level errors where no PGresult is available. Uses
 * PQerrorMessage for the message text and fallback_sqlstate for SQLSTATE.
 * native_error_code is always 0 (no position information at connection level).
 */
void error_add_diagnostic_from_connection(DiagnosticRecords *diagnostics,
                                          const PGconn *connection,
                                          const char *fallback_sqlstate);

#endif /* PSQLODBC2_ERROR_MAPPING_H */
