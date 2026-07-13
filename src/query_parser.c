/*-------------------------------------------------------------------------
 *
 * query_parser.c
 *	  SQL query text transformation for ODBC-to-PostgreSQL compatibility
 *
 * Copyright (c) 2026, Dave Cramer
 *
 *
 * IDENTIFICATION
 *	  src/query_parser.c
 *
 *-------------------------------------------------------------------------
 *
 * This module rewrites the ODBC dialect that applications emit into SQL that
 * PostgreSQL accepts. It is a single-pass, hand-written scanner rather than a
 * full grammar: ODBC's transformations are all local (marker substitution,
 * brace-delimited escapes, a couple of keyword rewrites), so a state machine
 * over the character stream is sufficient and easy to audit.
 *
 * The scanner tracks which lexical context each character sits in (ordinary
 * SQL, a string literal, a quoted identifier, a comment, a dollar-quoted
 * body) so that a "?" or "{" inside a literal is copied verbatim instead of
 * being mistaken for a parameter marker or an escape.
 */
#include "query_parser.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* States for the single-pass SQL scanner. The scanner reads each character
 * and transitions between states based on SQL syntax delimiters. Only
 * parameter markers and escapes found in NORMAL state are transformed. */
typedef enum {
    SCAN_STATE_NORMAL = 0,
    SCAN_STATE_IN_SINGLE_QUOTE,    /* Inside '...' string literal */
    SCAN_STATE_IN_ESCAPE_STRING,   /* Inside E'...' string where backslash escapes apply */
    SCAN_STATE_IN_DOUBLE_QUOTE,    /* Inside "..." quoted identifier */
    SCAN_STATE_IN_LINE_COMMENT,    /* After -- until newline */
    SCAN_STATE_IN_BLOCK_COMMENT    /* Inside slash-star ... star-slash block comment */
} ScanState;

/* ---- Small character helpers ---- */

static bool is_identifier_start_char(char character)
{
    return (character >= 'a' && character <= 'z') ||
           (character >= 'A' && character <= 'Z') || character == '_';
}

static bool is_identifier_continuation_char(char character)
{
    return is_identifier_start_char(character) ||
           (character >= '0' && character <= '9');
}

/* Lowercase a single ASCII letter; leaves other bytes unchanged. */
static char to_lower_ascii(char character)
{
    if (character >= 'A' && character <= 'Z') {
        return (char)(character + ('a' - 'A'));
    }
    return character;
}

/*
 * A '$' only opens a dollar-quoted string when the character immediately
 * before it is not part of an identifier. Otherwise the '$' continues an
 * identifier (e.g. the "a$1" alias in "SELECT 1 a$1"), and treating it as a
 * dollar-quote opener would swallow the rest of the statement. output_length
 * is how many characters have already been emitted; 0 means '$' is the very
 * first character.
 */
static bool dollar_can_open_here(const char *output, size_t output_length)
{
    if (output_length == 0) {
        return true;
    }
    char previous = output[output_length - 1];
    return !is_identifier_continuation_char(previous);
}

/*
 * Detect a PostgreSQL dollar-quote tag beginning at input[position] (which
 * must be '$'). A dollar-quote opens with $tag$ where tag is empty or an
 * identifier (letters, digits, underscores) that does not start with a digit.
 *
 * On success, returns true and sets *tag_length to the full length of the
 * opening delimiter including both '$' characters (e.g. "$$"->2, "$foo$"->5).
 * On failure (e.g. "$1" which is a parameter reference / identifier char),
 * returns false.
 */
static bool match_dollar_quote_open(const char *input, size_t position,
                                    size_t input_length, size_t *tag_length)
{
    /* input[position] == '$' guaranteed by caller */
    size_t cursor = position + 1;
    /* First tag character (if any) must not be a digit */
    if (cursor < input_length && input[cursor] != '$') {
        if (!is_identifier_start_char(input[cursor])) {
            return false;
        }
        cursor++;
        while (cursor < input_length && is_identifier_continuation_char(input[cursor])) {
            cursor++;
        }
    }
    /* The tag must be terminated by a closing '$' */
    if (cursor < input_length && input[cursor] == '$') {
        *tag_length = (cursor - position) + 1;
        return true;
    }
    return false;
}

/*
 * Case-insensitive match of a keyword at input[position], requiring a word
 * boundary afterward so "ts" does not match the "ts" prefix of a longer word.
 * A boundary is end-of-input or a non-identifier character (space, '(', '\'',
 * etc.). Used to recognize ODBC escape keywords like fn, call, d, t, ts, oj.
 */
static bool match_keyword_at(const char *input, size_t position, size_t input_length,
                             const char *keyword)
{
    size_t keyword_length = strlen(keyword);
    if (position + keyword_length > input_length) {
        return false;
    }
    for (size_t i = 0; i < keyword_length; i++) {
        if (to_lower_ascii(input[position + i]) != to_lower_ascii(keyword[i])) {
            return false;
        }
    }
    if (position + keyword_length < input_length) {
        char next = input[position + keyword_length];
        if (is_identifier_continuation_char(next)) {
            return false;
        }
    }
    return true;
}

/*
 * Look up the cast suffix for the given zero-based parameter position, or ""
 * (empty string) when no cast applies. Never returns NULL so callers can use
 * the result directly with snprintf.
 */
static const char *cast_for_parameter(const char *const *param_casts,
                                      int param_casts_count,
                                      int zero_based_position)
{
    if (param_casts &&
        zero_based_position < param_casts_count &&
        param_casts[zero_based_position]) {
        return param_casts[zero_based_position];
    }
    return "";
}

/* ---- ODBC scalar function ({fn ...}) mapping ---- */

/*
 * Maps an ODBC scalar function to a PostgreSQL expression template. In the
 * template, "$N" is replaced by the Nth actual argument's already-transformed
 * text and "$*" by the full comma-separated argument list. A name beginning
 * with "%K" (K a digit) only matches when the call has exactly K arguments,
 * which is how LOCATE distinguishes its 2- and 3-argument forms. Names not
 * listed here are passed through unchanged (PostgreSQL has a same-named
 * built-in, e.g. lower/upper/abs).
 */
typedef struct {
    const char *odbc_name;
    const char *pg_template;
} OdbcFunctionMapping;

static const OdbcFunctionMapping ODBC_FUNCTION_MAPPINGS[] = {
    { "CONCAT",     "concat($1::text, $2::text)" },
    { "%2LOCATE",   "strpos($2, $1)" },                             /* LOCATE(needle, haystack) */
    { "%3LOCATE",   "strpos(substring($2 from $3), $1) + $3 - 1" }, /* LOCATE(needle, haystack, start) */
    { "SUBSTRING",  "substr($*)" },
    { "SPACE",      "repeat(' ', $1)" },
    { "LENGTH",     "char_length($*)" },
    { "LCASE",      "lower($*)" },
    { "UCASE",      "upper($*)" },
    { "IFNULL",     "coalesce($*)" },
    { NULL, NULL }
};

/*
 * Portable ASCII case-insensitive string comparison. Returns true when both
 * strings match ignoring letter case. Used instead of POSIX strcasecmp, which
 * is unavailable on MSVC (it spells it _stricmp); ODBC function names are pure
 * ASCII so byte-wise lowercasing is sufficient and locale-independent.
 */
static bool ascii_case_equal(const char *left, const char *right)
{
    while (*left && *right) {
        if (to_lower_ascii(*left) != to_lower_ascii(*right)) {
            return false;
        }
        left++;
        right++;
    }
    return *left == *right;
}

/*
 * Find the PostgreSQL template for an ODBC function name given the number of
 * arguments in the call. Returns NULL when the function is not remapped (the
 * caller should then emit the function name verbatim).
 */
static const char *find_function_template(const char *function_name, int argument_count)
{
    for (int i = 0; ODBC_FUNCTION_MAPPINGS[i].odbc_name != NULL; i++) {
        const char *candidate = ODBC_FUNCTION_MAPPINGS[i].odbc_name;
        if (candidate[0] == '%') {
            /* "%K<name>": match only when argument_count == K */
            if ((candidate[1] - '0') == argument_count &&
                ascii_case_equal(candidate + 2, function_name)) {
                return ODBC_FUNCTION_MAPPINGS[i].pg_template;
            }
        } else if (ascii_case_equal(candidate, function_name)) {
            return ODBC_FUNCTION_MAPPINGS[i].pg_template;
        }
    }
    return NULL;
}

/* ---- Scanner working state ----
 *
 * A single Scanner struct threads the input, the growable output buffer, the
 * current lexical state, and the parameter bookkeeping through the recursive
 * escape handlers. Keeping it in one place avoids passing a dozen arguments.
 */
typedef struct {
    const char *input;
    size_t input_length;
    size_t input_index;

    char *output;
    size_t output_capacity;
    size_t output_position;

    ScanState state;
    int param_number;              /* Count of "?" markers seen so far */

    /* Nesting depth of the current block comment. Unlike C, PostgreSQL block
     * comments nest: an inner open-delimiter must be balanced by a matching
     * close-delimiter before the comment ends, so a single close-delimiter does
     * not necessarily end the comment — only the one that brings the depth back
     * to zero does. Meaningful only while state == SCAN_STATE_IN_BLOCK_COMMENT. */
    int block_comment_depth;

    const QueryParseOptions *options;
    const char *const *param_casts;
    int param_casts_count;

    QueryAnalysis *analysis;       /* Flags/roles are recorded here */
    bool out_of_memory;
} Scanner;

/*
 * Ensure the output buffer has room for `needed` more bytes plus a null
 * terminator, growing it if necessary. On allocation failure, sets
 * out_of_memory so the caller can abort the scan.
 */
static bool scanner_reserve(Scanner *scanner, size_t needed)
{
    size_t required = scanner->output_position + needed + 1;
    if (required <= scanner->output_capacity) {
        return true;
    }
    size_t new_capacity = scanner->output_capacity * 2;
    if (new_capacity < required) {
        new_capacity = required;
    }
    char *grown = realloc(scanner->output, new_capacity);
    if (!grown) {
        scanner->out_of_memory = true;
        return false;
    }
    scanner->output = grown;
    scanner->output_capacity = new_capacity;
    return true;
}

static void emit_char(Scanner *scanner, char character)
{
    if (!scanner_reserve(scanner, 1)) {
        return;
    }
    scanner->output[scanner->output_position++] = character;
}

static void emit_string(Scanner *scanner, const char *text, size_t length)
{
    if (!scanner_reserve(scanner, length)) {
        return;
    }
    memcpy(scanner->output + scanner->output_position, text, length);
    scanner->output_position += length;
}

/*
 * Emit a parameter marker for the "?" at the current input position. role
 * records whether this marker is an ordinary input or a procedure return
 * value; cast suffixes are only appended to ordinary inputs when the statement
 * is not a procedure call.
 */
static void emit_parameter_marker(Scanner *scanner, QueryParamRole role)
{
    int zero_based = scanner->param_number;
    scanner->param_number++;

    if (scanner->analysis && zero_based < QUERY_MAX_PARAMETERS) {
        scanner->analysis->parameter_roles[zero_based] = role;
    }

    /* Guard scanner->analysis the same way as above: casts are only applied to
     * ordinary input markers of a non-procedure-call statement. Treating a NULL
     * analysis as "not a procedure call" is safe — a NULL analysis only occurs
     * in hypothetical callers that don't track roles, and such callers still
     * want ordinary "?" -> "$N" translation with casts. */
    const char *cast = "";
    if (role == QUERY_PARAM_ROLE_INPUT &&
        (!scanner->analysis || !scanner->analysis->is_procedure_call)) {
        cast = cast_for_parameter(scanner->param_casts,
                                  scanner->param_casts_count, zero_based);
    }
    char marker[24];
    int written = snprintf(marker, sizeof(marker), "$%d", scanner->param_number);
    emit_string(scanner, marker, (size_t)written);
    if (cast[0] != '\0') {
        emit_string(scanner, cast, strlen(cast));
    }
}

/* Skip spaces, tabs, and newlines in the input starting at the current index. */
static void skip_input_whitespace(Scanner *scanner)
{
    while (scanner->input_index < scanner->input_length &&
           (scanner->input[scanner->input_index] == ' ' ||
            scanner->input[scanner->input_index] == '\t' ||
            scanner->input[scanner->input_index] == '\n' ||
            scanner->input[scanner->input_index] == '\r')) {
        scanner->input_index++;
    }
}

/* Forward declaration: the top-level per-character step, reused by escape
 * handlers that need to process nested content (e.g. {fn} arguments). */
static void scan_step(Scanner *scanner);

/*
 * Copy the body of a brace-delimited escape into the output, processing nested
 * "?" markers, string literals, and nested escapes, until the matching '}'.
 * The opening keyword has already been consumed by the caller; input_index is
 * positioned at the first body character. The closing '}' is consumed but not
 * emitted.
 */
static void copy_escape_body(Scanner *scanner)
{
    while (scanner->input_index < scanner->input_length && !scanner->out_of_memory) {
        if (scanner->state == SCAN_STATE_NORMAL &&
            scanner->input[scanner->input_index] == '}') {
            scanner->input_index++;   /* consume the closing brace */
            break;
        }
        scan_step(scanner);
    }
}

/*
 * Collect the arguments of a {fn NAME(arg, arg, ...)} call. Each argument is
 * transformed (markers/escapes applied) into a freshly grown buffer, and the
 * per-argument text is stored in argument_texts. Returns the argument count,
 * or -1 on error/out-of-memory. input_index must be positioned at the '('.
 *
 * argument_texts entries are heap-allocated; the caller frees them.
 */
static int collect_function_arguments(Scanner *scanner,
                                      char *argument_texts[], int max_arguments)
{
    if (scanner->input_index >= scanner->input_length ||
        scanner->input[scanner->input_index] != '(') {
        return -1;
    }
    scanner->input_index++;   /* consume '(' */

    /* Temporarily redirect scanner output into private buffers so we can
     * capture each argument's transformed text separately. */
    char *saved_output = scanner->output;
    size_t saved_capacity = scanner->output_capacity;
    size_t saved_position = scanner->output_position;

    int argument_count = 0;
    int paren_depth = 1;
    bool argument_has_content = false;

    scanner->output_capacity = 64;
    scanner->output = malloc(scanner->output_capacity);
    scanner->output_position = 0;
    if (!scanner->output) {
        scanner->out_of_memory = true;
        goto restore_and_fail;
    }

    while (scanner->input_index < scanner->input_length && !scanner->out_of_memory) {
        char character = scanner->input[scanner->input_index];

        if (scanner->state == SCAN_STATE_NORMAL && character == '(') {
            paren_depth++;
            emit_char(scanner, character);
            scanner->input_index++;
        } else if (scanner->state == SCAN_STATE_NORMAL && character == ')') {
            paren_depth--;
            if (paren_depth == 0) {
                scanner->input_index++;   /* consume closing ')' */
                break;
            }
            emit_char(scanner, character);
            scanner->input_index++;
        } else if (scanner->state == SCAN_STATE_NORMAL && character == ',' &&
                   paren_depth == 1) {
            if (argument_count >= max_arguments) {
                goto restore_and_fail;
            }
            scanner->output[scanner->output_position] = '\0';
            argument_texts[argument_count++] = scanner->output;
            scanner->output_capacity = 64;
            scanner->output = malloc(scanner->output_capacity);
            scanner->output_position = 0;
            argument_has_content = false;
            if (!scanner->output) {
                scanner->out_of_memory = true;
                goto restore_and_fail;
            }
            scanner->input_index++;   /* consume ',' */
        } else {
            if (scanner->state == SCAN_STATE_NORMAL &&
                character != ' ' && character != '\t') {
                argument_has_content = true;
            }
            scan_step(scanner);
        }
    }

    if (scanner->out_of_memory) {
        goto restore_and_fail;
    }

    /* Finalize the last argument buffer. An empty argument list (e.g. SPACE())
     * yields zero arguments. */
    if (argument_count >= max_arguments) {
        goto restore_and_fail;
    }
    scanner->output[scanner->output_position] = '\0';
    if (argument_count == 0 && !argument_has_content) {
        free(scanner->output);
    } else {
        argument_texts[argument_count++] = scanner->output;
    }

    scanner->output = saved_output;
    scanner->output_capacity = saved_capacity;
    scanner->output_position = saved_position;
    return argument_count;

restore_and_fail:
    free(scanner->output);
    for (int i = 0; i < argument_count; i++) {
        free(argument_texts[i]);
    }
    scanner->output = saved_output;
    scanner->output_capacity = saved_capacity;
    scanner->output_position = saved_position;
    return -1;
}

/*
 * Expand a function template, substituting "$N" with argument_texts[N-1] and
 * "$*" with all arguments joined by ", ". Emits the result to the output.
 */
static void emit_function_template(Scanner *scanner, const char *template_text,
                                   char *argument_texts[], int argument_count)
{
    for (const char *cursor = template_text; *cursor != '\0'; cursor++) {
        if (*cursor != '$') {
            emit_char(scanner, *cursor);
            continue;
        }
        cursor++;
        if (*cursor == '*') {
            for (int i = 0; i < argument_count; i++) {
                if (i > 0) {
                    emit_string(scanner, ", ", 2);
                }
                emit_string(scanner, argument_texts[i], strlen(argument_texts[i]));
            }
        } else if (*cursor >= '1' && *cursor <= '9') {
            int index = (*cursor - '1');
            if (index >= 0 && index < argument_count) {
                emit_string(scanner, argument_texts[index], strlen(argument_texts[index]));
            }
        } else {
            /* Not a recognized placeholder; emit the '$' and current char. */
            emit_char(scanner, '$');
            emit_char(scanner, *cursor);
        }
    }
}

/*
 * Handle {fn NAME(args)} : if NAME is remapped, expand its template; otherwise
 * emit NAME(args) verbatim (transformed). input_index is positioned just after
 * "fn". Consumes through the matching '}'.
 */
static void handle_function_escape(Scanner *scanner)
{
    skip_input_whitespace(scanner);

    char function_name[64];
    size_t name_length = 0;
    while (scanner->input_index < scanner->input_length && name_length < sizeof(function_name) - 1) {
        char character = scanner->input[scanner->input_index];
        if (character == '(' || character == ' ' || character == '\t' || character == '}') {
            break;
        }
        function_name[name_length++] = character;
        scanner->input_index++;
    }
    function_name[name_length] = '\0';
    skip_input_whitespace(scanner);

    /* A "function constant" without parentheses (e.g. {fn CURRENT_DATE}) is
     * emitted verbatim. */
    if (scanner->input_index >= scanner->input_length ||
        scanner->input[scanner->input_index] != '(') {
        emit_string(scanner, function_name, name_length);
        while (scanner->input_index < scanner->input_length &&
               scanner->input[scanner->input_index] != '}') {
            scanner->input_index++;
        }
        if (scanner->input_index < scanner->input_length) {
            scanner->input_index++;
        }
        return;
    }

    enum { MAX_FN_ARGUMENTS = 16 };
    char *argument_texts[MAX_FN_ARGUMENTS] = { 0 };
    int argument_count = collect_function_arguments(scanner, argument_texts, MAX_FN_ARGUMENTS);
    if (argument_count < 0 || scanner->out_of_memory) {
        return;
    }

    const char *template_text = find_function_template(function_name, argument_count);
    if (template_text) {
        emit_function_template(scanner, template_text, argument_texts, argument_count);
    } else {
        /* Unmapped function: emit NAME(arg, arg, ...) as-is. */
        emit_string(scanner, function_name, name_length);
        emit_char(scanner, '(');
        for (int i = 0; i < argument_count; i++) {
            if (i > 0) {
                emit_string(scanner, ", ", 2);
            }
            emit_string(scanner, argument_texts[i], strlen(argument_texts[i]));
        }
        emit_char(scanner, ')');
    }

    /* Consume the closing '}' (collect_function_arguments stopped at the ')'). */
    skip_input_whitespace(scanner);
    if (scanner->input_index < scanner->input_length &&
        scanner->input[scanner->input_index] == '}') {
        scanner->input_index++;
    }

    for (int i = 0; i < argument_count; i++) {
        free(argument_texts[i]);
    }
}

/*
 * Handle {d '...'}, {t '...'}, {ts '...'} datetime literal escapes by emitting
 * the quoted literal with a PostgreSQL cast: {d '2014-12-21'} -> '2014-12-21'::date.
 * cast_suffix is "::date", "::time", or "::timestamp". input_index is
 * positioned just after the keyword. Consumes through the closing '}'.
 */
static void handle_datetime_escape(Scanner *scanner, const char *cast_suffix)
{
    skip_input_whitespace(scanner);
    while (scanner->input_index < scanner->input_length &&
           scanner->input[scanner->input_index] != '}') {
        char character = scanner->input[scanner->input_index];
        if (character == ' ' || character == '\t') {
            /* Drop trailing whitespace that sits directly before '}'. */
            size_t look = scanner->input_index;
            while (look < scanner->input_length &&
                   (scanner->input[look] == ' ' || scanner->input[look] == '\t')) {
                look++;
            }
            if (look < scanner->input_length && scanner->input[look] == '}') {
                scanner->input_index = look;
                break;
            }
        }
        emit_char(scanner, character);
        scanner->input_index++;
    }
    emit_string(scanner, cast_suffix, strlen(cast_suffix));
    if (scanner->input_index < scanner->input_length &&
        scanner->input[scanner->input_index] == '}') {
        scanner->input_index++;
    }
}

/*
 * Handle {escape 'c'} -> ESCAPE 'c'. input_index is positioned just after the
 * "escape" keyword. Consumes through the closing '}'.
 */
static void handle_like_escape(Scanner *scanner)
{
    skip_input_whitespace(scanner);
    emit_string(scanner, "ESCAPE ", 7);
    while (scanner->input_index < scanner->input_length &&
           scanner->input[scanner->input_index] != '}') {
        emit_char(scanner, scanner->input[scanner->input_index]);
        scanner->input_index++;
    }
    if (scanner->input_index < scanner->input_length) {
        scanner->input_index++;
    }
}

/*
 * Handle {oj ...} outer-join escape: strip the {oj and } wrapper, keeping the
 * inner join expression (transformed). input_index is positioned just after
 * "oj". Consumes through the matching '}'.
 */
static void handle_outer_join_escape(Scanner *scanner)
{
    skip_input_whitespace(scanner);
    copy_escape_body(scanner);
}

/*
 * Parse the comma-separated argument list of a procedure call into structured
 * QueryCallArgument entries, without emitting anything to the main output.
 * input_index is positioned just after the opening '('. On return it is
 * positioned just after the closing ')'.
 *
 * Each argument is classified as either a parameter marker ("?" with an
 * optional cast like "::text") or a literal expression (transformed text). The
 * executor uses these to rebuild the call, dropping OUT-only marker arguments
 * and applying named notation.
 */
static void parse_call_arguments(Scanner *scanner)
{
    QueryAnalysis *analysis = scanner->analysis;
    int paren_depth = 1;

    while (scanner->input_index < scanner->input_length && !scanner->out_of_memory) {
        skip_input_whitespace(scanner);
        if (scanner->input_index >= scanner->input_length) {
            break;
        }
        char character = scanner->input[scanner->input_index];
        if (character == ')') {
            scanner->input_index++;   /* consume ')' */
            break;
        }
        if (character == ',') {
            scanner->input_index++;   /* skip empty separator */
            continue;
        }

        QueryCallArgument argument = { 0 };

        if (character == '?' &&
            !(scanner->input_index + 1 < scanner->input_length &&
              scanner->input[scanner->input_index + 1] == '?')) {
            /* Parameter-marker argument. Record its 1-based ODBC position and
             * consume it. */
            scanner->param_number++;
            argument.parameter_number = scanner->param_number;
            if (scanner->param_number - 1 < QUERY_MAX_PARAMETERS) {
                analysis->parameter_roles[scanner->param_number - 1] =
                    QUERY_PARAM_ROLE_INPUT;
            }
            scanner->input_index++;   /* consume '?' */

            /* Capture a trailing cast ("::type") if present. */
            skip_input_whitespace(scanner);
            if (scanner->input_index + 1 < scanner->input_length &&
                scanner->input[scanner->input_index] == ':' &&
                scanner->input[scanner->input_index + 1] == ':') {
                size_t cast_len = 0;
                argument.cast_suffix[cast_len++] = ':';
                argument.cast_suffix[cast_len++] = ':';
                scanner->input_index += 2;
                while (scanner->input_index < scanner->input_length &&
                       cast_len < QUERY_MAX_CAST_LENGTH - 1) {
                    char type_char = scanner->input[scanner->input_index];
                    if (is_identifier_continuation_char(type_char)) {
                        argument.cast_suffix[cast_len++] = type_char;
                        scanner->input_index++;
                    } else {
                        break;
                    }
                }
                argument.cast_suffix[cast_len] = '\0';
            }
        } else {
            /* Literal expression argument (e.g. 'foo', 42). Copy its text up to
             * the next top-level ',' or ')'. */
            size_t literal_len = 0;
            while (scanner->input_index < scanner->input_length &&
                   literal_len < QUERY_MAX_CALL_LITERAL - 1) {
                char literal_char = scanner->input[scanner->input_index];
                if (literal_char == '(') {
                    paren_depth++;
                } else if (literal_char == ')') {
                    if (paren_depth == 1) {
                        break;
                    }
                    paren_depth--;
                } else if (literal_char == ',' && paren_depth == 1) {
                    break;
                }
                argument.literal_text[literal_len++] = literal_char;
                scanner->input_index++;
            }
            /* Trim trailing whitespace from the literal. */
            while (literal_len > 0 &&
                   (argument.literal_text[literal_len - 1] == ' ' ||
                    argument.literal_text[literal_len - 1] == '\t')) {
                literal_len--;
            }
            argument.literal_text[literal_len] = '\0';
        }

        if (analysis->call_argument_count < QUERY_MAX_CALL_ARGUMENTS) {
            analysis->call_arguments[analysis->call_argument_count++] = argument;
        }

        /* Advance to the next ',' or ')'. */
        skip_input_whitespace(scanner);
        if (scanner->input_index < scanner->input_length &&
            scanner->input[scanner->input_index] == ',') {
            scanner->input_index++;
        }
    }
}

/*
 * Handle a procedure call: "{ call NAME(args) }" or "{ ? = call NAME(args) }".
 * Emits "SELECT ..." wrapping the function call and records is_procedure_call
 * and return_value_count on the analysis. input_index is positioned at the
 * first non-space character after '{'.
 *
 * A leading "? =" is consumed as the return-value marker (counted as a
 * parameter but not emitted into the SQL). Consumes through the closing '}'.
 */
static void handle_call_escape(Scanner *scanner)
{
    scanner->analysis->is_procedure_call = true;

    /* Optional leading "? =" return value. */
    if (scanner->input_index < scanner->input_length &&
        scanner->input[scanner->input_index] == '?') {
        int zero_based = scanner->param_number;
        scanner->param_number++;
        if (scanner->analysis && zero_based < QUERY_MAX_PARAMETERS) {
            scanner->analysis->parameter_roles[zero_based] = QUERY_PARAM_ROLE_RETURN_VALUE;
        }
        scanner->analysis->return_value_count++;
        scanner->input_index++;   /* consume '?' */
        skip_input_whitespace(scanner);
        if (scanner->input_index < scanner->input_length &&
            scanner->input[scanner->input_index] == '=') {
            scanner->input_index++;   /* consume '=' */
        }
        skip_input_whitespace(scanner);
    }

    /* Expect and consume the "call" keyword. */
    if (match_keyword_at(scanner->input, scanner->input_index, scanner->input_length, "call")) {
        scanner->input_index += 4;
    }
    skip_input_whitespace(scanner);

    /* Read the function name. */
    QueryAnalysis *analysis = scanner->analysis;
    size_t name_length = 0;
    while (scanner->input_index < scanner->input_length &&
           name_length < sizeof(analysis->procedure_name) - 1) {
        char character = scanner->input[scanner->input_index];
        if (character == '(' || character == ' ' || character == '\t' ||
            character == '}' || character == '\n' || character == '\r') {
            break;
        }
        analysis->procedure_name[name_length++] = character;
        scanner->input_index++;
    }
    analysis->procedure_name[name_length] = '\0';
    skip_input_whitespace(scanner);

    /*
     * Parse the argument list into structured QueryCallArgument entries so the
     * executor can rebuild the call once bindings are known. The transformed
     * SQL only carries the SELECT wrapper up to (and including) the function
     * name and "("; the executor appends the surviving arguments and ")".
     *
     * A function requested with a return value ("{ ? = call f(...) }") reads
     * as a scalar select; a procedure with OUT/INOUT params uses
     * "SELECT * FROM f(...)" so its record result set can be fetched.
     */
    if (analysis->return_value_count > 0) {
        emit_string(scanner, "SELECT ", 7);
    } else {
        emit_string(scanner, "SELECT * FROM ", 14);
    }
    emit_string(scanner, analysis->procedure_name, name_length);

    if (scanner->input_index < scanner->input_length &&
        scanner->input[scanner->input_index] == '(') {
        scanner->input_index++;   /* consume '(' */
        parse_call_arguments(scanner);
    }

    /* Consume through the closing '}' of the call escape. */
    while (scanner->input_index < scanner->input_length &&
           scanner->input[scanner->input_index] != '}') {
        scanner->input_index++;
    }
    if (scanner->input_index < scanner->input_length) {
        scanner->input_index++;
    }
}

/*
 * Apply the MS Access / Jet boolean rewrite. VBA turns a boolean predicate
 * ("col" = True) into ("col" = 1); PostgreSQL rejects "boolean = integer".
 * When ms_jet is enabled and the output tail matches  "<...>" = 1  (with the
 * closing identifier quote, an '=', and a lone '1' just emitted), rewrite the
 * trailing "1" to "'1'". Invoked when a ')' is about to be emitted.
 */
static void maybe_rewrite_msaccess_boolean(Scanner *scanner)
{
    if (!scanner->options || !scanner->options->ms_jet) {
        return;
    }
    size_t position = scanner->output_position;
    if (position == 0 || scanner->output[position - 1] != '1') {
        return;
    }
    size_t one_index = position - 1;
    size_t index = one_index;
    while (index > 0 && (scanner->output[index - 1] == ' ' || scanner->output[index - 1] == '\t')) {
        index--;
    }
    if (index == 0 || scanner->output[index - 1] != '=') {
        return;
    }
    index--;   /* before '=' */
    while (index > 0 && (scanner->output[index - 1] == ' ' || scanner->output[index - 1] == '\t')) {
        index--;
    }
    if (index == 0 || scanner->output[index - 1] != '"') {
        return;
    }
    /* Wrap the trailing digit in single quotes: "...= 1" -> "...= '1'". */
    if (!scanner_reserve(scanner, 2)) {
        return;
    }
    scanner->output[one_index] = '\'';
    scanner->output[one_index + 1] = '1';
    scanner->output[one_index + 2] = '\'';
    scanner->output_position = one_index + 3;
}

/*
 * Return true if the statement begins with the INSERT command keyword. Used to
 * gate the DEFAULT VALUES rewrite to INSERT statements.
 */
static bool statement_is_insert(const Scanner *scanner)
{
    size_t index = 0;
    while (index < scanner->input_length &&
           (scanner->input[index] == ' ' || scanner->input[index] == '\t' ||
            scanner->input[index] == '\n' || scanner->input[index] == '\r')) {
        index++;
    }
    return match_keyword_at(scanner->input, index, scanner->input_length, "insert");
}

/*
 * Handle INSERT ... () VALUES () -> INSERT ... DEFAULT VALUES. Detects the
 * empty column list "()" immediately followed (ignoring whitespace) by
 * "VALUES ()". Checked when a '(' is seen in NORMAL state. Returns true and
 * consumes the whole "() VALUES ()" run (emitting "DEFAULT VALUES") on match.
 */
static bool maybe_rewrite_insert_default_values(Scanner *scanner)
{
    size_t index = scanner->input_index;   /* at the first '(' */
    if (scanner->input[index] != '(') {
        return false;
    }
    index++;
    while (index < scanner->input_length &&
           (scanner->input[index] == ' ' || scanner->input[index] == '\t')) {
        index++;
    }
    if (index >= scanner->input_length || scanner->input[index] != ')') {
        return false;
    }
    index++;
    while (index < scanner->input_length &&
           (scanner->input[index] == ' ' || scanner->input[index] == '\t' ||
            scanner->input[index] == '\n' || scanner->input[index] == '\r')) {
        index++;
    }
    if (!match_keyword_at(scanner->input, index, scanner->input_length, "values")) {
        return false;
    }
    index += 6;
    while (index < scanner->input_length &&
           (scanner->input[index] == ' ' || scanner->input[index] == '\t' ||
            scanner->input[index] == '\n' || scanner->input[index] == '\r')) {
        index++;
    }
    if (index >= scanner->input_length || scanner->input[index] != '(') {
        return false;
    }
    index++;
    while (index < scanner->input_length &&
           (scanner->input[index] == ' ' || scanner->input[index] == '\t')) {
        index++;
    }
    if (index >= scanner->input_length || scanner->input[index] != ')') {
        return false;
    }
    index++;   /* consume final ')' */

    emit_string(scanner, "DEFAULT VALUES", 14);
    scanner->input_index = index;
    return true;
}

/*
 * Handle an ODBC escape sequence starting at the '{' in NORMAL state.
 * Dispatches on the escape keyword. input_index is positioned at the '{'.
 * Unknown escapes emit the '{' literally so the scanner continues normally.
 */
static void handle_open_brace(Scanner *scanner)
{
    size_t keyword_index = scanner->input_index + 1;
    while (keyword_index < scanner->input_length &&
           (scanner->input[keyword_index] == ' ' || scanner->input[keyword_index] == '\t')) {
        keyword_index++;
    }

    if (match_keyword_at(scanner->input, keyword_index, scanner->input_length, "fn")) {
        scanner->input_index = keyword_index + 2;
        handle_function_escape(scanner);
    } else if (match_keyword_at(scanner->input, keyword_index, scanner->input_length, "ts")) {
        scanner->input_index = keyword_index + 2;
        handle_datetime_escape(scanner, "::timestamp");
    } else if (match_keyword_at(scanner->input, keyword_index, scanner->input_length, "d")) {
        scanner->input_index = keyword_index + 1;
        handle_datetime_escape(scanner, "::date");
    } else if (match_keyword_at(scanner->input, keyword_index, scanner->input_length, "t")) {
        scanner->input_index = keyword_index + 1;
        handle_datetime_escape(scanner, "::time");
    } else if (match_keyword_at(scanner->input, keyword_index, scanner->input_length, "oj")) {
        scanner->input_index = keyword_index + 2;
        handle_outer_join_escape(scanner);
    } else if (match_keyword_at(scanner->input, keyword_index, scanner->input_length, "escape")) {
        scanner->input_index = keyword_index + 6;
        handle_like_escape(scanner);
    } else if (match_keyword_at(scanner->input, keyword_index, scanner->input_length, "call") ||
               (keyword_index < scanner->input_length && scanner->input[keyword_index] == '?')) {
        scanner->input_index = keyword_index;
        handle_call_escape(scanner);
    } else {
        emit_char(scanner, '{');
        scanner->input_index++;
    }
}

/*
 * Process exactly one lexical unit from the input, updating state and emitting
 * transformed output. This is the heart of the scanner; escape handlers call
 * back into it to transform nested content.
 */
static void scan_step(Scanner *scanner)
{
    char current = scanner->input[scanner->input_index];

    switch (scanner->state) {
    case SCAN_STATE_NORMAL:
        if (current == '@' &&
            match_keyword_at(scanner->input, scanner->input_index,
                             scanner->input_length, "@@identity")) {
            /* Translate the MS SQL Server "@@IDENTITY" pseudo-variable to
             * PostgreSQL's lastval(), which returns the value most recently
             * obtained from a sequence in the current session. Applications use
             * "SELECT @@IDENTITY" immediately after an INSERT into a serial /
             * identity table to retrieve the generated key. The token is not
             * valid PostgreSQL syntax otherwise, and match_keyword_at requires a
             * trailing word boundary, so this never disturbs ordinary SQL. */
            emit_string(scanner, "lastval()", 9);
            scanner->input_index += strlen("@@identity");
        } else if (current == '{') {
            handle_open_brace(scanner);
        } else if (current == '?') {
            if (scanner->input_index + 1 < scanner->input_length &&
                scanner->input[scanner->input_index + 1] == '?') {
                /* "??" escape produces a literal '?'. */
                emit_char(scanner, '?');
                scanner->input_index += 2;
            } else {
                emit_parameter_marker(scanner, QUERY_PARAM_ROLE_INPUT);
                scanner->input_index++;
            }
        } else if ((current == 'E' || current == 'e') &&
                   scanner->input_index + 1 < scanner->input_length &&
                   scanner->input[scanner->input_index + 1] == '\'') {
            emit_char(scanner, current);
            emit_char(scanner, '\'');
            scanner->state = SCAN_STATE_IN_ESCAPE_STRING;
            scanner->input_index += 2;
        } else if (current == '$' &&
                   dollar_can_open_here(scanner->output, scanner->output_position)) {
            size_t tag_length = 0;
            if (match_dollar_quote_open(scanner->input, scanner->input_index,
                                        scanner->input_length, &tag_length)) {
                emit_string(scanner, scanner->input + scanner->input_index, tag_length);
                const char *tag_start = scanner->input + scanner->input_index;
                scanner->input_index += tag_length;
                while (scanner->input_index < scanner->input_length) {
                    if (scanner->input[scanner->input_index] == '$' &&
                        scanner->input_index + tag_length <= scanner->input_length &&
                        memcmp(scanner->input + scanner->input_index, tag_start, tag_length) == 0) {
                        emit_string(scanner, scanner->input + scanner->input_index, tag_length);
                        scanner->input_index += tag_length;
                        break;
                    }
                    emit_char(scanner, scanner->input[scanner->input_index]);
                    scanner->input_index++;
                }
            } else {
                emit_char(scanner, current);
                scanner->input_index++;
            }
        } else if (current == '\'') {
            emit_char(scanner, current);
            scanner->state = SCAN_STATE_IN_SINGLE_QUOTE;
            scanner->input_index++;
        } else if (current == '"') {
            emit_char(scanner, current);
            scanner->state = SCAN_STATE_IN_DOUBLE_QUOTE;
            scanner->input_index++;
        } else if (current == '-' &&
                   scanner->input_index + 1 < scanner->input_length &&
                   scanner->input[scanner->input_index + 1] == '-') {
            emit_char(scanner, current);
            emit_char(scanner, '-');
            scanner->state = SCAN_STATE_IN_LINE_COMMENT;
            scanner->input_index += 2;
        } else if (current == '/' &&
                   scanner->input_index + 1 < scanner->input_length &&
                   scanner->input[scanner->input_index + 1] == '*') {
            emit_char(scanner, current);
            emit_char(scanner, '*');
            scanner->state = SCAN_STATE_IN_BLOCK_COMMENT;
            scanner->block_comment_depth = 1;
            scanner->input_index += 2;
        } else if (current == '(' && statement_is_insert(scanner) &&
                   maybe_rewrite_insert_default_values(scanner)) {
            /* handled: "() VALUES ()" consumed and rewritten. */
        } else if (current == ')') {
            maybe_rewrite_msaccess_boolean(scanner);
            emit_char(scanner, ')');
            scanner->input_index++;
        } else {
            emit_char(scanner, current);
            scanner->input_index++;
        }
        break;

    case SCAN_STATE_IN_SINGLE_QUOTE:
        if (current == '\\' && !scanner->options->standard_conforming_strings &&
            scanner->input_index + 1 < scanner->input_length) {
            /* Backslash escape inside an ordinary literal (scs off): copy the
             * backslash and the escaped char so an escaped quote does not end
             * the string. */
            emit_char(scanner, current);
            emit_char(scanner, scanner->input[scanner->input_index + 1]);
            scanner->input_index += 2;
        } else if (current == '\'' &&
                   scanner->input_index + 1 < scanner->input_length &&
                   scanner->input[scanner->input_index + 1] == '\'') {
            emit_char(scanner, current);
            emit_char(scanner, '\'');
            scanner->input_index += 2;
        } else if (current == '\'') {
            emit_char(scanner, current);
            scanner->state = SCAN_STATE_NORMAL;
            scanner->input_index++;
        } else {
            emit_char(scanner, current);
            scanner->input_index++;
        }
        break;

    case SCAN_STATE_IN_ESCAPE_STRING:
        if (current == '\\' && scanner->input_index + 1 < scanner->input_length) {
            emit_char(scanner, current);
            emit_char(scanner, scanner->input[scanner->input_index + 1]);
            scanner->input_index += 2;
        } else if (current == '\'' &&
                   scanner->input_index + 1 < scanner->input_length &&
                   scanner->input[scanner->input_index + 1] == '\'') {
            emit_char(scanner, current);
            emit_char(scanner, '\'');
            scanner->input_index += 2;
        } else if (current == '\'') {
            emit_char(scanner, current);
            scanner->state = SCAN_STATE_NORMAL;
            scanner->input_index++;
        } else {
            emit_char(scanner, current);
            scanner->input_index++;
        }
        break;

    case SCAN_STATE_IN_DOUBLE_QUOTE:
        if (current == '"' &&
            scanner->input_index + 1 < scanner->input_length &&
            scanner->input[scanner->input_index + 1] == '"') {
            emit_char(scanner, current);
            emit_char(scanner, '"');
            scanner->input_index += 2;
        } else if (current == '"') {
            emit_char(scanner, current);
            scanner->state = SCAN_STATE_NORMAL;
            scanner->input_index++;
        } else {
            emit_char(scanner, current);
            scanner->input_index++;
        }
        break;

    case SCAN_STATE_IN_LINE_COMMENT:
        emit_char(scanner, current);
        if (current == '\n') {
            scanner->state = SCAN_STATE_NORMAL;
        }
        scanner->input_index++;
        break;

    case SCAN_STATE_IN_BLOCK_COMMENT:
        if (current == '/' &&
            scanner->input_index + 1 < scanner->input_length &&
            scanner->input[scanner->input_index + 1] == '*') {
            /* A nested block-comment opener adds a level; PostgreSQL requires a
             * matching close-delimiter for each before the comment ends. */
            emit_char(scanner, current);
            emit_char(scanner, '*');
            scanner->block_comment_depth++;
            scanner->input_index += 2;
        } else if (current == '*' &&
                   scanner->input_index + 1 < scanner->input_length &&
                   scanner->input[scanner->input_index + 1] == '/') {
            emit_char(scanner, current);
            emit_char(scanner, '/');
            scanner->block_comment_depth--;
            if (scanner->block_comment_depth == 0) {
                scanner->state = SCAN_STATE_NORMAL;
            }
            scanner->input_index += 2;
        } else {
            emit_char(scanner, current);
            scanner->input_index++;
        }
        break;
    }
}

/* ---- Public Interface ---- */

bool query_analyze(const char *sql_input,
                   const QueryParseOptions *options,
                   const char *const *param_casts,
                   int param_casts_count,
                   QueryAnalysis *out_analysis)
{
    static const QueryParseOptions default_options = {
        .standard_conforming_strings = true,
        .ms_jet = false,
    };
    if (!options) {
        options = &default_options;
    }

    memset(out_analysis, 0, sizeof(*out_analysis));

    if (!sql_input) {
        return false;
    }

    size_t input_length = strlen(sql_input);

    Scanner scanner = {
        .input = sql_input,
        .input_length = input_length,
        .input_index = 0,
        .output_capacity = input_length + 64,
        .output_position = 0,
        .state = SCAN_STATE_NORMAL,
        .param_number = 0,
        .options = options,
        .param_casts = param_casts,
        .param_casts_count = param_casts_count,
        .analysis = out_analysis,
        .out_of_memory = false,
    };
    scanner.output = malloc(scanner.output_capacity);
    if (!scanner.output) {
        return false;
    }

    while (scanner.input_index < scanner.input_length && !scanner.out_of_memory) {
        scan_step(&scanner);
    }

    if (scanner.out_of_memory) {
        free(scanner.output);
        out_analysis->transformed_sql = NULL;
        return false;
    }

    scanner.output[scanner.output_position] = '\0';
    out_analysis->transformed_sql = scanner.output;
    out_analysis->parameter_count = scanner.param_number;
    return true;
}

char *query_translate_markers(const char *sql_input, int *out_param_count,
                              const char *const *param_casts,
                              int param_casts_count)
{
    QueryAnalysis analysis;
    if (!query_analyze(sql_input, NULL, param_casts, param_casts_count, &analysis)) {
        if (out_param_count) {
            *out_param_count = 0;
        }
        return NULL;
    }
    if (out_param_count) {
        *out_param_count = analysis.parameter_count;
    }
    return analysis.transformed_sql;
}

/*
 * Skip a bracketed/quoted region or ordinary token while scanning a select
 * list, keeping track of parenthesis depth. Returns the index just past the
 * consumed region. Handles '...' literals, "..." identifiers, and nested ().
 */
static size_t skip_select_token(const char *sql, size_t index, size_t length,
                                int *paren_depth)
{
    char character = sql[index];
    if (character == '\'') {
        index++;
        while (index < length) {
            if (sql[index] == '\'' && index + 1 < length && sql[index + 1] == '\'') {
                index += 2;
            } else if (sql[index] == '\'') {
                index++;
                break;
            } else {
                index++;
            }
        }
        return index;
    }
    if (character == '"') {
        index++;
        while (index < length) {
            if (sql[index] == '"' && index + 1 < length && sql[index + 1] == '"') {
                index += 2;
            } else if (sql[index] == '"') {
                index++;
                break;
            } else {
                index++;
            }
        }
        return index;
    }
    if (character == '(') {
        (*paren_depth)++;
    } else if (character == ')') {
        (*paren_depth)--;
    }
    return index + 1;
}

/*
 * Compute the unescaped character length of the '...' string literal starting
 * at sql[index] (which must be the opening quote). A doubled quote ('') counts
 * as one character. Returns the count and advances *end_index past the literal.
 */
static int measure_string_literal(const char *sql, size_t index, size_t length,
                                  size_t *end_index)
{
    int count = 0;
    index++;   /* skip opening quote */
    while (index < length) {
        if (sql[index] == '\'' && index + 1 < length && sql[index + 1] == '\'') {
            count++;
            index += 2;
        } else if (sql[index] == '\'') {
            index++;
            break;
        } else {
            count++;
            index++;
        }
    }
    *end_index = index;
    return count;
}

void query_parse_select_columns(const char *sql_input,
                                QueryColumnOverride *overrides,
                                int max_columns,
                                int *out_count)
{
    *out_count = 0;
    if (!sql_input) {
        return;
    }
    size_t length = strlen(sql_input);
    size_t index = 0;

    /* Require a leading SELECT. */
    while (index < length && isspace((unsigned char)sql_input[index])) {
        index++;
    }
    if (!match_keyword_at(sql_input, index, length, "select")) {
        return;
    }
    index += 6;

    int column_index = 0;
    int paren_depth = 0;
    bool at_column_start = true;
    bool column_is_literal = false;
    int literal_length = 0;

    while (index < length && column_index < max_columns) {
        /* Skip leading whitespace at the start of a column. */
        if (at_column_start) {
            while (index < length && isspace((unsigned char)sql_input[index])) {
                index++;
            }
            if (index >= length) {
                break;
            }
            /* A column that begins with a string literal is reported as
             * VARCHAR(length). */
            if (sql_input[index] == '\'') {
                size_t end_index = index;
                literal_length = measure_string_literal(sql_input, index, length, &end_index);
                column_is_literal = true;
                index = end_index;
                at_column_start = false;
                continue;
            }
            column_is_literal = false;
            at_column_start = false;
        }

        char character = sql_input[index];

        /* A top-level FROM ends the select list. */
        if (paren_depth == 0 &&
            (character == 'f' || character == 'F') &&
            match_keyword_at(sql_input, index, length, "from")) {
            /* Record the final column before stopping. */
            if (column_index < max_columns) {
                overrides[column_index].is_string_literal = column_is_literal;
                overrides[column_index].character_length = literal_length;
                column_index++;
            }
            break;
        }

        if (paren_depth == 0 && character == ',') {
            if (column_index < max_columns) {
                overrides[column_index].is_string_literal = column_is_literal;
                overrides[column_index].character_length = literal_length;
                column_index++;
            }
            index++;
            at_column_start = true;
            column_is_literal = false;
            literal_length = 0;
            continue;
        }

        index = skip_select_token(sql_input, index, length, &paren_depth);
    }

    /* If the select list ran to end-of-string without a FROM or trailing comma,
     * record the last column. */
    if (index >= length && !at_column_start && column_index < max_columns) {
        overrides[column_index].is_string_literal = column_is_literal;
        overrides[column_index].character_length = literal_length;
        column_index++;
    }

    *out_count = column_index;
}

/* ---- Multi-statement splitting ---- */

/*
 * Append a trimmed copy of input[start..end) to the statement list as one
 * fragment. Leading and trailing ASCII whitespace is removed; a fragment that
 * is empty after trimming (e.g. from ";;;" or a trailing ";") is dropped so it
 * never becomes an executable statement. Returns false on allocation failure.
 */
static bool statement_list_append_range(QueryStatementList *list,
                                        const char *input,
                                        size_t start, size_t end)
{
    /* Trim leading whitespace. */
    while (start < end &&
           (input[start] == ' ' || input[start] == '\t' ||
            input[start] == '\n' || input[start] == '\r')) {
        start++;
    }
    /* Trim trailing whitespace. */
    while (end > start &&
           (input[end - 1] == ' ' || input[end - 1] == '\t' ||
            input[end - 1] == '\n' || input[end - 1] == '\r')) {
        end--;
    }
    if (end <= start) {
        return true;   /* Empty fragment — drop it. */
    }

    size_t length = end - start;
    char *fragment = malloc(length + 1);
    if (!fragment) {
        return false;
    }
    memcpy(fragment, input + start, length);
    fragment[length] = '\0';

    /* Grow the backing array by one slot. Multi-statement queries are short,
     * so a linear realloc-per-append is simpler than tracking capacity and is
     * not a hot path. */
    char **grown = realloc(list->statements,
                           sizeof(char *) * (size_t)(list->count + 1));
    if (!grown) {
        free(fragment);
        return false;
    }
    list->statements = grown;
    list->statements[list->count++] = fragment;
    return true;
}

bool query_split_statements(const char *sql_input,
                            const QueryParseOptions *options,
                            QueryStatementList *out_list)
{
    out_list->statements = NULL;
    out_list->count = 0;

    if (!sql_input) {
        return true;
    }

    bool standard_conforming_strings =
        options ? options->standard_conforming_strings : true;

    size_t input_length = strlen(sql_input);
    size_t index = 0;
    size_t fragment_start = 0;
    ScanState state = SCAN_STATE_NORMAL;
    /* Nesting depth of the current block comment; see the transforming scanner
     * for why PostgreSQL block comments must be matched pair-for-pair. Only
     * meaningful while state == SCAN_STATE_IN_BLOCK_COMMENT. */
    int block_comment_depth = 0;

    /* Single pass over the input tracking the same lexical states as the
     * transforming scanner. A ';' only ends a statement in NORMAL state; inside
     * a literal, identifier, comment, or dollar-quoted body it is ordinary text. */
    while (index < input_length) {
        char current = sql_input[index];

        switch (state) {
        case SCAN_STATE_NORMAL:
            if (current == ';') {
                if (!statement_list_append_range(out_list, sql_input,
                                                 fragment_start, index)) {
                    query_statement_list_free(out_list);
                    return false;
                }
                index++;
                fragment_start = index;
            } else if ((current == 'E' || current == 'e') &&
                       index + 1 < input_length && sql_input[index + 1] == '\'') {
                state = SCAN_STATE_IN_ESCAPE_STRING;
                index += 2;
            } else if (current == '$' &&
                       dollar_can_open_here(sql_input, index)) {
                size_t tag_length = 0;
                if (match_dollar_quote_open(sql_input, index, input_length,
                                            &tag_length)) {
                    /* Consume the entire dollar-quoted body up to and including
                     * the matching closing tag so any ';' inside is ignored. */
                    const char *tag_start = sql_input + index;
                    index += tag_length;
                    while (index < input_length) {
                        if (sql_input[index] == '$' &&
                            index + tag_length <= input_length &&
                            memcmp(sql_input + index, tag_start, tag_length) == 0) {
                            index += tag_length;
                            break;
                        }
                        index++;
                    }
                } else {
                    index++;
                }
            } else if (current == '\'') {
                state = SCAN_STATE_IN_SINGLE_QUOTE;
                index++;
            } else if (current == '"') {
                state = SCAN_STATE_IN_DOUBLE_QUOTE;
                index++;
            } else if (current == '-' && index + 1 < input_length &&
                       sql_input[index + 1] == '-') {
                state = SCAN_STATE_IN_LINE_COMMENT;
                index += 2;
            } else if (current == '/' && index + 1 < input_length &&
                       sql_input[index + 1] == '*') {
                state = SCAN_STATE_IN_BLOCK_COMMENT;
                block_comment_depth = 1;
                index += 2;
            } else {
                index++;
            }
            break;

        case SCAN_STATE_IN_SINGLE_QUOTE:
            if (current == '\\' && !standard_conforming_strings &&
                index + 1 < input_length) {
                index += 2;   /* Backslash escapes the next char (scs off). */
            } else if (current == '\'' && index + 1 < input_length &&
                       sql_input[index + 1] == '\'') {
                index += 2;   /* Doubled quote is an escaped quote. */
            } else if (current == '\'') {
                state = SCAN_STATE_NORMAL;
                index++;
            } else {
                index++;
            }
            break;

        case SCAN_STATE_IN_ESCAPE_STRING:
            if (current == '\\' && index + 1 < input_length) {
                index += 2;
            } else if (current == '\'' && index + 1 < input_length &&
                       sql_input[index + 1] == '\'') {
                index += 2;
            } else if (current == '\'') {
                state = SCAN_STATE_NORMAL;
                index++;
            } else {
                index++;
            }
            break;

        case SCAN_STATE_IN_DOUBLE_QUOTE:
            if (current == '"' && index + 1 < input_length &&
                sql_input[index + 1] == '"') {
                index += 2;
            } else if (current == '"') {
                state = SCAN_STATE_NORMAL;
                index++;
            } else {
                index++;
            }
            break;

        case SCAN_STATE_IN_LINE_COMMENT:
            if (current == '\n') {
                state = SCAN_STATE_NORMAL;
            }
            index++;
            break;

        case SCAN_STATE_IN_BLOCK_COMMENT:
            if (current == '/' && index + 1 < input_length &&
                sql_input[index + 1] == '*') {
                /* Nested comment opener — needs its own matching close. */
                block_comment_depth++;
                index += 2;
            } else if (current == '*' && index + 1 < input_length &&
                       sql_input[index + 1] == '/') {
                block_comment_depth--;
                if (block_comment_depth == 0) {
                    state = SCAN_STATE_NORMAL;
                }
                index += 2;
            } else {
                index++;
            }
            break;
        }
    }

    /* Emit the final fragment after the last ';' (or the whole input if there
     * was no top-level ';'). */
    if (!statement_list_append_range(out_list, sql_input,
                                     fragment_start, input_length)) {
        query_statement_list_free(out_list);
        return false;
    }

    return true;
}

void query_statement_list_free(QueryStatementList *list)
{
    if (!list) {
        return;
    }
    for (int index = 0; index < list->count; index++) {
        free(list->statements[index]);
    }
    free(list->statements);
    list->statements = NULL;
    list->count = 0;
}

bool query_is_transaction_exempt(const char *sql_text)
{
    if (!sql_text) {
        return false;
    }

    /* Skip leading whitespace to find the first keyword */
    const char *cursor = sql_text;
    while (*cursor == ' ' || *cursor == '\t' || *cursor == '\n' || *cursor == '\r') {
        cursor++;
    }

    size_t remaining = strlen(cursor);

    /* VACUUM (with optional parenthesized options like VACUUM (ANALYZE)) */
    if (remaining >= 6) {
        char upper[7];
        for (int i = 0; i < 6; i++) {
            upper[i] = (char)toupper((unsigned char)cursor[i]);
        }
        upper[6] = '\0';
        if (memcmp(upper, "VACUUM", 6) == 0) {
            if (remaining == 6 || cursor[6] == ' ' || cursor[6] == '(' ||
                cursor[6] == '\t' || cursor[6] == '\n') {
                return true;
            }
        }
    }

    /* CLUSTER (without arguments or with a table name) */
    if (remaining >= 7) {
        char upper[8];
        for (int i = 0; i < 7; i++) {
            upper[i] = (char)toupper((unsigned char)cursor[i]);
        }
        upper[7] = '\0';
        if (memcmp(upper, "CLUSTER", 7) == 0) {
            if (remaining == 7 || cursor[7] == ' ' || cursor[7] == ';' ||
                cursor[7] == '\t' || cursor[7] == '\n') {
                return true;
            }
        }
    }

    /* CREATE DATABASE */
    if (remaining >= 15) {
        char upper[16];
        for (int i = 0; i < 15; i++) {
            upper[i] = (char)toupper((unsigned char)cursor[i]);
        }
        upper[15] = '\0';
        if (memcmp(upper, "CREATE DATABASE", 15) == 0) {
            return true;
        }
    }

    /* DROP DATABASE */
    if (remaining >= 13) {
        char upper[14];
        for (int i = 0; i < 13; i++) {
            upper[i] = (char)toupper((unsigned char)cursor[i]);
        }
        upper[13] = '\0';
        if (memcmp(upper, "DROP DATABASE", 13) == 0) {
            return true;
        }
    }

    /* DISCARD ALL */
    if (remaining >= 11) {
        char upper[12];
        for (int i = 0; i < 11; i++) {
            upper[i] = (char)toupper((unsigned char)cursor[i]);
        }
        upper[11] = '\0';
        if (memcmp(upper, "DISCARD ALL", 11) == 0) {
            return true;
        }
    }

    return false;
}
