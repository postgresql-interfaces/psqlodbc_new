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

/* Maximum number of ODBC parameter markers the analyzer tracks per statement.
 * Mirrors parameter.h's MAX_PARAMETERS; kept independent to avoid a header
 * dependency cycle. */
#define QUERY_MAX_PARAMETERS 256

/*
 * Per-parameter role after ODBC-escape analysis. A procedure call may bind
 * a leading "?" as the function's return value ("{ ? = call ... }") and may
 * bind OUT / INOUT parameters that are not sent as call arguments.
 *
 * These roles let the executor decide (a) which bound parameters become libpq
 * bind values and (b) which result columns are copied back into OUT buffers.
 */
typedef enum {
    QUERY_PARAM_ROLE_INPUT = 0,     /* Ordinary input parameter, sent as a value */
    QUERY_PARAM_ROLE_RETURN_VALUE,  /* Leading "?" capturing a function return value */
} QueryParamRole;

/* Maximum call arguments captured for a procedure call. */
#define QUERY_MAX_CALL_ARGUMENTS 32

/* Maximum length of a per-argument cast suffix (e.g. "::text"). */
#define QUERY_MAX_CAST_LENGTH 32

/* Maximum length of a literal procedure-call argument (e.g. "'foo'"). */
#define QUERY_MAX_CALL_LITERAL 128

/*
 * Describes one argument of an ODBC procedure call "{call f(a1, a2, ...)}".
 * An argument is either a parameter marker (optionally with a cast, e.g.
 * "?::text") or a literal expression (e.g. "'foo'"). We capture enough to let
 * the executor rebuild the call once bindings are known — dropping OUT-only
 * marker arguments, applying named notation, and renumbering the surviving
 * "$N" markers.
 */
typedef struct QueryCallArgument {
    int parameter_number;                    /* 1-based ODBC "?" position, or 0 if a literal */
    char cast_suffix[QUERY_MAX_CAST_LENGTH]; /* e.g. "::text"; empty when none */
    char literal_text[QUERY_MAX_CALL_LITERAL]; /* transformed literal text for a non-marker arg */
} QueryCallArgument;

/*
 * Options that influence how the parser interprets a query. These come from
 * the connection: whether standard_conforming_strings is on (affects backslash
 * escapes inside ordinary '...' literals) and whether MS Access / Jet quirks
 * are enabled (the "= 1" boolean rewrite).
 */
typedef struct QueryParseOptions {
    /* When false, a backslash inside an ordinary '...' string literal escapes
     * the following character (pre-9.1 / standard_conforming_strings=off).
     * When true (the modern default), backslashes are literal and only ''
     * escapes a quote. */
    bool standard_conforming_strings;

    /* When true, rewrite MS Access / Jet's ("col" = 1) boolean comparison to
     * ("col"='1') so PostgreSQL does not reject "boolean = integer". */
    bool ms_jet;
} QueryParseOptions;

/*
 * Result of analyzing (and transforming) a query. transformed_sql is a
 * heap-allocated string the caller must free. The remaining fields describe
 * structure the executor needs to run procedure calls and handle OUT params.
 */
typedef struct QueryAnalysis {
    char *transformed_sql;      /* Heap-allocated; caller frees. NULL on alloc failure. */
    int parameter_count;        /* Number of ODBC "?" markers found (including any return value). */

    /* True when the statement is an ODBC procedure call ({call ...} or
     * { ? = call ...}). Procedure calls send their arguments with the
     * PostgreSQL "unknown" type so the server can resolve function overloads,
     * and copy result columns back into OUT/INOUT parameter buffers. */
    bool is_procedure_call;

    /* Number of leading parameters that capture a function return value
     * (0 or 1). A return value corresponds to "{ ? = call ... }" and is not
     * sent to the server as a call argument. */
    int return_value_count;

    /* Per-parameter role, indexed by zero-based parameter position. Only the
     * first parameter_count entries are meaningful. */
    QueryParamRole parameter_roles[QUERY_MAX_PARAMETERS];

    /* For procedure calls only: the function name and its argument list. The
     * executor rebuilds the call from these once parameter bindings are known
     * (so it can drop OUT-only arguments and use named notation). transformed_sql
     * for a procedure call ends right after the function name and "(" — the
     * executor appends the argument list and closing ")". */
    char procedure_name[256];
    QueryCallArgument call_arguments[QUERY_MAX_CALL_ARGUMENTS];
    int call_argument_count;
} QueryAnalysis;

/*
 * Analyze and transform a query in one pass:
 *   - translate ODBC "?" markers to PostgreSQL "$N" markers,
 *   - process ODBC escape sequences ({fn ...}, {call ...}, {d/t/ts ...},
 *     {oj ...}, {escape ...}),
 *   - apply connection-specific rewrites (MS Access "= 1", INSERT ... ()
 *     VALUES () -> DEFAULT VALUES).
 *
 * Correctly skips SQL elements that must not be transformed: '...' literals
 * (with '' and, when standard_conforming_strings is off, backslash escapes),
 * E'...' escape strings, "..." quoted identifiers (with "" escapes),
 * $tag$...$tag$ dollar-quoted strings, line comments (-- ...) and block
 * comments. A '$' that continues an identifier (e.g. a$1) is not treated as a
 * dollar-quote opener.
 *
 * options may be NULL, in which case standard_conforming_strings defaults to
 * true and ms_jet to false.
 *
 * param_casts, when non-NULL, supplies a PostgreSQL cast suffix (e.g. "::int4")
 * appended to each ordinary parameter marker, indexed by zero-based parameter
 * position. A NULL entry (or NULL array) means "no cast" for that position.
 * Casts are never applied to procedure-call arguments (those are sent as the
 * unknown type) or to a return-value marker.
 *
 * On success, fills *out_analysis (transformed_sql is heap-allocated; caller
 * frees it) and returns true. On allocation failure, sets
 * out_analysis->transformed_sql to NULL and returns false.
 */
bool query_analyze(const char *sql_input,
                   const QueryParseOptions *options,
                   const char *const *param_casts,
                   int param_casts_count,
                   QueryAnalysis *out_analysis);

/*
 * Backward-compatible convenience wrapper around query_analyze that returns
 * only the transformed SQL (with default options: standard_conforming_strings
 * on, MS Access off). Used by SQLNativeSql and other callers that only need
 * the "?"-to-"$N" translation and escape processing.
 *
 * Returns a heap-allocated string (caller frees), or NULL on failure. Stores
 * the parameter count in *out_param_count when non-NULL.
 */
char *query_translate_markers(const char *sql_input, int *out_param_count,
                              const char *const *param_casts,
                              int param_casts_count);

/*
 * Per-output-column metadata override produced by client-side SELECT parsing
 * (the "Parse" connection option). Currently the only refinement is detecting
 * a string-literal column so it can be described as VARCHAR(length) instead of
 * the generic "text" type PostgreSQL reports.
 */
typedef struct QueryColumnOverride {
    bool is_string_literal;   /* True when the select-list column is a '...' literal */
    int character_length;     /* Unescaped character length of the literal */
} QueryColumnOverride;

/*
 * Parse a SELECT statement's select list client-side to derive per-column
 * metadata overrides. Fills overrides[0..*out_count-1], one entry per top-level
 * select-list column, in order. Columns that need no override have
 * is_string_literal == false.
 *
 * Only SELECT statements are analyzed; for anything else *out_count is set to 0.
 * overrides must have at least max_columns entries; columns beyond that are
 * ignored. This is a best-effort helper used only when the Parse option is on.
 */
void query_parse_select_columns(const char *sql_input,
                                QueryColumnOverride *overrides,
                                int max_columns,
                                int *out_count);

/*
 * A list of individual SQL statement fragments produced by splitting a
 * multi-statement query (e.g. "SELECT 1; SELECT 2") on top-level ';'
 * separators. Each entry is the raw (untransformed) SQL text of one statement,
 * with surrounding whitespace trimmed. Empty fragments (from spurious
 * semicolons like ";;;" or a trailing ";") are dropped.
 *
 * The fragments are raw rather than transformed because each must be analyzed
 * independently: ODBC "?" markers restart at $1 in every fragment, and each
 * fragment is executed as its own libpq command (the extended query protocol
 * rejects multiple commands in a single prepared/parameterized statement).
 */
typedef struct QueryStatementList {
    char **statements;   /* Heap array of heap-allocated fragment strings */
    int count;           /* Number of fragments */
} QueryStatementList;

/*
 * Split a (possibly multi-statement) SQL string into its individual statement
 * fragments on top-level ';' separators, honoring lexical context so a ';'
 * inside a string literal, quoted identifier, dollar-quoted body, or comment
 * does not split the statement. Reuses the same lexer states as query_analyze.
 *
 * options may be NULL (standard_conforming_strings defaults to true); only the
 * standard_conforming_strings flag affects splitting (backslash handling inside
 * ordinary '...' literals).
 *
 * On success fills *out_list (caller frees with query_statement_list_free) and
 * returns true. Returns false only on allocation failure. A single statement
 * with no trailing ';' yields a list of count 1; an all-whitespace or empty
 * input yields count 0.
 */
bool query_split_statements(const char *sql_input,
                            const QueryParseOptions *options,
                            QueryStatementList *out_list);

/*
 * Free the fragment strings and backing array allocated by
 * query_split_statements. Safe to call on a zeroed/empty list.
 */
void query_statement_list_free(QueryStatementList *list);

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
