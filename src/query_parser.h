/*-------------------------------------------------------------------------
 *
 * query_parser.h
 *	  SQL query text transformation for ODBC-to-PostgreSQL compatibility
 *
 * Copyright (c) 2026, Dave Cramer
 *
 *
 * IDENTIFICATION
 *	  src/query_parser.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PSQLODBC2_QUERY_PARSER_H
#define PSQLODBC2_QUERY_PARSER_H

#include <stdbool.h>

/*
 * Translate ODBC-style '?' parameter markers to PostgreSQL-style '$N' markers.
 *
 * Properly handles SQL syntax elements that should NOT be translated:
 *   - String literals ('...' with '' as escaped quote)
 *   - Quoted identifiers ("..." with "" as escaped quote)
 *   - Line comments (-- to end of line)
 *   - Block comments (slash-star ... star-slash)
 *   - The ?? escape sequence (produces a literal ? in the output)
 *
 * Returns a heap-allocated string containing the translated SQL. The caller
 * must free this string. Returns NULL on memory allocation failure.
 *
 * If out_param_count is not NULL, stores the number of parameter markers found.
 *
 * param_casts, when non-NULL, supplies a PostgreSQL cast suffix (e.g. "::int4")
 * to append to each parameter marker. It is indexed by zero-based parameter
 * position, so param_casts[0] applies to the first "?" encountered. A NULL
 * entry (or a param_casts of NULL) means "no cast" for that position. The
 * array must have at least param_casts_count entries; positions at or beyond
 * that count receive no cast. This lets the driver bind parameters with an
 * explicit SQL type (from SQLBindParameter) so PostgreSQL interprets the value
 * as that type rather than as an untyped string literal.
 */
char *query_translate_markers(const char *sql_input, int *out_param_count,
                              const char *const *param_casts,
                              int param_casts_count);

/*
 * Determine whether a SQL command should NOT be wrapped in a transaction.
 *
 * Certain PostgreSQL commands (VACUUM, CREATE DATABASE, CLUSTER, REINDEX
 * with CONCURRENTLY, etc.) cannot run inside a transaction block. When
 * autocommit is OFF, the driver normally issues an implicit BEGIN before
 * executing statements. This function detects commands that must bypass
 * that behavior.
 *
 * Returns true if the command must run outside a transaction block.
 */
bool query_is_transaction_exempt(const char *sql_text);

#endif /* PSQLODBC2_QUERY_PARSER_H */
