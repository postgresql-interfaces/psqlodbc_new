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
 */
#include "query_parser.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* States for the single-pass SQL scanner. The scanner reads each character
 * and transitions between states based on SQL syntax delimiters. Only
 * parameter markers found in NORMAL state are translated. */
typedef enum {
    SCAN_STATE_NORMAL = 0,
    SCAN_STATE_IN_SINGLE_QUOTE,    /* Inside '...' string literal */
    SCAN_STATE_IN_ESCAPE_STRING,   /* Inside E'...' string where backslash escapes apply */
    SCAN_STATE_IN_DOUBLE_QUOTE,    /* Inside "..." quoted identifier */
    SCAN_STATE_IN_LINE_COMMENT,    /* After -- until newline */
    SCAN_STATE_IN_BLOCK_COMMENT    /* Inside slash-star ... star-slash block comment */
} ScanState;

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
        char first = input[cursor];
        bool is_ident_start = (first >= 'a' && first <= 'z') ||
                              (first >= 'A' && first <= 'Z') || first == '_';
        if (!is_ident_start) {
            return false;
        }
        cursor++;
        while (cursor < input_length) {
            char tag_char = input[cursor];
            bool is_ident = (tag_char >= 'a' && tag_char <= 'z') ||
                            (tag_char >= 'A' && tag_char <= 'Z') ||
                            (tag_char >= '0' && tag_char <= '9') || tag_char == '_';
            if (is_ident) {
                cursor++;
            } else {
                break;
            }
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
 * Check if the text at the given position starts with the specified keyword
 * (case-insensitive). The keyword must be followed by a space or end of input
 * to constitute a word boundary match.
 */
static bool match_escape_keyword(const char *input, size_t position, size_t input_length,
                                 const char *keyword, size_t keyword_length)
{
    if (position + keyword_length > input_length) {
        return false;
    }
    for (size_t i = 0; i < keyword_length; i++) {
        char input_char = input[position + i];
        char keyword_char = keyword[i];
        /* Case-insensitive comparison */
        if (input_char >= 'A' && input_char <= 'Z') {
            input_char = (char)(input_char + 32);
        }
        if (keyword_char >= 'A' && keyword_char <= 'Z') {
            keyword_char = (char)(keyword_char + 32);
        }
        if (input_char != keyword_char) {
            return false;
        }
    }
    /* Verify word boundary: next char must be space, tab, or open paren for "fn" */
    if (position + keyword_length < input_length) {
        char next = input[position + keyword_length];
        if (next != ' ' && next != '\t' && next != '(' && next != '\'') {
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

char *query_translate_markers(const char *sql_input, int *out_param_count,
                              const char *const *param_casts,
                              int param_casts_count)
{
    if (!sql_input) {
        if (out_param_count) {
            *out_param_count = 0;
        }
        return NULL;
    }

    size_t input_length = strlen(sql_input);

    /* Allocate output buffer. Worst case: every character is '?' which becomes
     * '$NNN' plus a cast suffix. The longest cast we emit is "::timestamp" (11
     * chars), so 4 (marker) + 12 = 16 bytes per '?' is a safe upper bound. The
     * ODBC escape processing only removes characters. */
    size_t output_capacity = (input_length * 16) + 64;
    char *output = malloc(output_capacity);
    if (!output) {
        if (out_param_count) {
            *out_param_count = 0;
        }
        return NULL;
    }

    ScanState state = SCAN_STATE_NORMAL;
    int param_number = 0;
    size_t output_position = 0;
    size_t input_index = 0;

    while (input_index < input_length) {
        char current_char = sql_input[input_index];

        switch (state) {
        case SCAN_STATE_NORMAL:
            if (current_char == '{') {
                /* ODBC escape sequence detected. Identify the type and strip
                 * the wrapper, copying only the inner content to the output. */
                size_t after_brace = input_index + 1;

                /* Skip whitespace after opening brace */
                while (after_brace < input_length &&
                       (sql_input[after_brace] == ' ' || sql_input[after_brace] == '\t')) {
                    after_brace++;
                }

                if (match_escape_keyword(sql_input, after_brace, input_length, "fn", 2)) {
                    /* {fn func(args)} → func(args)
                     * Skip "{fn " and process content normally until matching '}' */
                    input_index = after_brace + 2;
                    /* Skip space after "fn" */
                    while (input_index < input_length &&
                           (sql_input[input_index] == ' ' || sql_input[input_index] == '\t')) {
                        input_index++;
                    }
                    /* Process content until we find the matching closing brace.
                     * Track brace depth for nested escapes. */
                    int brace_depth = 1;
                    while (input_index < input_length && brace_depth > 0) {
                        char ch = sql_input[input_index];
                        if (ch == '{') {
                            brace_depth++;
                            output[output_position++] = ch;
                            input_index++;
                        } else if (ch == '}') {
                            brace_depth--;
                            if (brace_depth > 0) {
                                output[output_position++] = ch;
                            }
                            input_index++;
                        } else if (ch == '?' && brace_depth == 1) {
                            /* Parameter marker inside escape — translate normally */
                            if (input_index + 1 < input_length && sql_input[input_index + 1] == '?') {
                                output[output_position++] = '?';
                                input_index += 2;
                            } else {
                                param_number++;
                                int written = snprintf(output + output_position,
                                                       output_capacity - output_position,
                                                       "$%d%s", param_number,
                                                       cast_for_parameter(param_casts,
                                                                          param_casts_count,
                                                                          param_number - 1));
                                output_position += (size_t)written;
                                input_index++;
                            }
                        } else if (ch == '\'') {
                            /* String literal inside escape — copy verbatim */
                            output[output_position++] = ch;
                            input_index++;
                            while (input_index < input_length) {
                                char sc = sql_input[input_index];
                                if (sc == '\'' && input_index + 1 < input_length &&
                                    sql_input[input_index + 1] == '\'') {
                                    output[output_position++] = sc;
                                    output[output_position++] = sql_input[input_index + 1];
                                    input_index += 2;
                                } else if (sc == '\'') {
                                    output[output_position++] = sc;
                                    input_index++;
                                    break;
                                } else {
                                    output[output_position++] = sc;
                                    input_index++;
                                }
                            }
                        } else {
                            output[output_position++] = ch;
                            input_index++;
                        }
                    }
                } else if (match_escape_keyword(sql_input, after_brace, input_length, "ts", 2)) {
                    /* {ts 'YYYY-MM-DD HH:MM:SS'} → 'YYYY-MM-DD HH:MM:SS'
                     * Note: must check "ts" before "t" since "t" would match "ts" prefix */
                    input_index = after_brace + 2;
                    /* Skip whitespace */
                    while (input_index < input_length &&
                           (sql_input[input_index] == ' ' || sql_input[input_index] == '\t')) {
                        input_index++;
                    }
                    /* Copy content until closing brace */
                    while (input_index < input_length && sql_input[input_index] != '}') {
                        output[output_position++] = sql_input[input_index];
                        input_index++;
                    }
                    /* Skip the closing brace */
                    if (input_index < input_length) { input_index++; }
                } else if (match_escape_keyword(sql_input, after_brace, input_length, "t", 1)) {
                    /* {t 'HH:MM:SS'} → 'HH:MM:SS' */
                    input_index = after_brace + 1;
                    while (input_index < input_length &&
                           (sql_input[input_index] == ' ' || sql_input[input_index] == '\t')) {
                        input_index++;
                    }
                    while (input_index < input_length && sql_input[input_index] != '}') {
                        output[output_position++] = sql_input[input_index];
                        input_index++;
                    }
                    if (input_index < input_length) { input_index++; }
                } else if (match_escape_keyword(sql_input, after_brace, input_length, "d", 1)) {
                    /* {d 'YYYY-MM-DD'} → 'YYYY-MM-DD' */
                    input_index = after_brace + 1;
                    while (input_index < input_length &&
                           (sql_input[input_index] == ' ' || sql_input[input_index] == '\t')) {
                        input_index++;
                    }
                    while (input_index < input_length && sql_input[input_index] != '}') {
                        output[output_position++] = sql_input[input_index];
                        input_index++;
                    }
                    if (input_index < input_length) { input_index++; }
                } else if (match_escape_keyword(sql_input, after_brace, input_length, "oj", 2)) {
                    /* {oj table LEFT OUTER JOIN ...} → table LEFT OUTER JOIN ...
                     * Strip the {oj and } wrapper, keep the content */
                    input_index = after_brace + 2;
                    while (input_index < input_length &&
                           (sql_input[input_index] == ' ' || sql_input[input_index] == '\t')) {
                        input_index++;
                    }
                    /* Copy content, handling nested braces */
                    int brace_depth = 1;
                    while (input_index < input_length && brace_depth > 0) {
                        char ch = sql_input[input_index];
                        if (ch == '{') {
                            brace_depth++;
                            output[output_position++] = ch;
                        } else if (ch == '}') {
                            brace_depth--;
                            if (brace_depth > 0) {
                                output[output_position++] = ch;
                            }
                        } else {
                            output[output_position++] = ch;
                        }
                        input_index++;
                    }
                } else if (match_escape_keyword(sql_input, after_brace, input_length, "escape", 6)) {
                    /* {escape 'c'} → ESCAPE 'c' */
                    input_index = after_brace + 6;
                    while (input_index < input_length &&
                           (sql_input[input_index] == ' ' || sql_input[input_index] == '\t')) {
                        input_index++;
                    }
                    /* Write "ESCAPE " prefix */
                    memcpy(output + output_position, "ESCAPE ", 7);
                    output_position += 7;
                    /* Copy the escape character specification until closing brace */
                    while (input_index < input_length && sql_input[input_index] != '}') {
                        output[output_position++] = sql_input[input_index];
                        input_index++;
                    }
                    if (input_index < input_length) { input_index++; }
                } else {
                    /* Unrecognized escape — copy the brace literally */
                    output[output_position++] = current_char;
                    input_index++;
                }
            } else if (current_char == '?') {
                /* Check for ?? escape (produces literal ?) */
                if (input_index + 1 < input_length && sql_input[input_index + 1] == '?') {
                    output[output_position++] = '?';
                    input_index += 2;
                } else {
                    /* Parameter marker: replace ? with $N (plus optional cast) */
                    param_number++;
                    int written = snprintf(output + output_position,
                                           output_capacity - output_position,
                                           "$%d%s", param_number,
                                           cast_for_parameter(param_casts,
                                                              param_casts_count,
                                                              param_number - 1));
                    output_position += (size_t)written;
                    input_index++;
                }
            } else if ((current_char == 'E' || current_char == 'e') &&
                       input_index + 1 < input_length &&
                       sql_input[input_index + 1] == '\'') {
                /* Enter escape string literal E'...'. In these, a backslash
                 * escapes the following character (including quotes), so we
                 * must not treat \' as a string terminator. This is distinct
                 * from a regular '...' literal. */
                output[output_position++] = current_char;      /* E */
                output[output_position++] = sql_input[input_index + 1]; /* ' */
                state = SCAN_STATE_IN_ESCAPE_STRING;
                input_index += 2;
            } else if (current_char == '$') {
                /* Possible dollar-quoted string ($$...$$ or $tag$...$tag$).
                 * If it's not a valid dollar-quote opener (e.g. "$1" parameter
                 * placeholder or "a$1" identifier), copy the '$' literally. */
                size_t tag_length = 0;
                if (match_dollar_quote_open(sql_input, input_index, input_length, &tag_length)) {
                    /* Copy the opening delimiter, then scan for the matching
                     * closing delimiter of the same tag, copying verbatim.
                     * Dollar-quoted content is never subject to ? translation
                     * or escape processing. */
                    memcpy(output + output_position, sql_input + input_index, tag_length);
                    output_position += tag_length;
                    const char *tag_start = sql_input + input_index;
                    input_index += tag_length;
                    while (input_index < input_length) {
                        if (sql_input[input_index] == '$' &&
                            input_index + tag_length <= input_length &&
                            memcmp(sql_input + input_index, tag_start, tag_length) == 0) {
                            /* Found the closing delimiter */
                            memcpy(output + output_position, sql_input + input_index, tag_length);
                            output_position += tag_length;
                            input_index += tag_length;
                            break;
                        }
                        output[output_position++] = sql_input[input_index++];
                    }
                } else {
                    output[output_position++] = current_char;
                    input_index++;
                }
            } else if (current_char == '\'') {
                /* Enter single-quoted string literal */
                output[output_position++] = current_char;
                state = SCAN_STATE_IN_SINGLE_QUOTE;
                input_index++;
            } else if (current_char == '"') {
                /* Enter double-quoted identifier */
                output[output_position++] = current_char;
                state = SCAN_STATE_IN_DOUBLE_QUOTE;
                input_index++;
            } else if (current_char == '-' &&
                       input_index + 1 < input_length &&
                       sql_input[input_index + 1] == '-') {
                /* Enter line comment */
                output[output_position++] = current_char;
                output[output_position++] = sql_input[input_index + 1];
                state = SCAN_STATE_IN_LINE_COMMENT;
                input_index += 2;
            } else if (current_char == '/' &&
                       input_index + 1 < input_length &&
                       sql_input[input_index + 1] == '*') {
                /* Enter block comment */
                output[output_position++] = current_char;
                output[output_position++] = sql_input[input_index + 1];
                state = SCAN_STATE_IN_BLOCK_COMMENT;
                input_index += 2;
            } else {
                output[output_position++] = current_char;
                input_index++;
            }
            break;

        case SCAN_STATE_IN_SINGLE_QUOTE:
            if (current_char == '\'' &&
                input_index + 1 < input_length &&
                sql_input[input_index + 1] == '\'') {
                /* Escaped single quote ('') — stays in string */
                output[output_position++] = current_char;
                output[output_position++] = sql_input[input_index + 1];
                input_index += 2;
            } else if (current_char == '\'') {
                /* End of string literal */
                output[output_position++] = current_char;
                state = SCAN_STATE_NORMAL;
                input_index++;
            } else {
                output[output_position++] = current_char;
                input_index++;
            }
            break;

        case SCAN_STATE_IN_ESCAPE_STRING:
            if (current_char == '\\' && input_index + 1 < input_length) {
                /* Backslash escape: copy the backslash and the escaped char
                 * verbatim so an escaped quote (\') does not end the string. */
                output[output_position++] = current_char;
                output[output_position++] = sql_input[input_index + 1];
                input_index += 2;
            } else if (current_char == '\'' &&
                       input_index + 1 < input_length &&
                       sql_input[input_index + 1] == '\'') {
                /* Doubled quote ('') stays inside the string */
                output[output_position++] = current_char;
                output[output_position++] = sql_input[input_index + 1];
                input_index += 2;
            } else if (current_char == '\'') {
                output[output_position++] = current_char;
                state = SCAN_STATE_NORMAL;
                input_index++;
            } else {
                output[output_position++] = current_char;
                input_index++;
            }
            break;

        case SCAN_STATE_IN_DOUBLE_QUOTE:
            if (current_char == '"' &&
                input_index + 1 < input_length &&
                sql_input[input_index + 1] == '"') {
                /* Escaped double quote ("") — stays in identifier */
                output[output_position++] = current_char;
                output[output_position++] = sql_input[input_index + 1];
                input_index += 2;
            } else if (current_char == '"') {
                /* End of quoted identifier */
                output[output_position++] = current_char;
                state = SCAN_STATE_NORMAL;
                input_index++;
            } else {
                output[output_position++] = current_char;
                input_index++;
            }
            break;

        case SCAN_STATE_IN_LINE_COMMENT:
            output[output_position++] = current_char;
            if (current_char == '\n') {
                state = SCAN_STATE_NORMAL;
            }
            input_index++;
            break;

        case SCAN_STATE_IN_BLOCK_COMMENT:
            if (current_char == '*' &&
                input_index + 1 < input_length &&
                sql_input[input_index + 1] == '/') {
                /* End of block comment */
                output[output_position++] = current_char;
                output[output_position++] = sql_input[input_index + 1];
                state = SCAN_STATE_NORMAL;
                input_index += 2;
            } else {
                output[output_position++] = current_char;
                input_index++;
            }
            break;
        }
    }

    output[output_position] = '\0';

    if (out_param_count) {
        *out_param_count = param_number;
    }

    return output;
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

    /* Compare case-insensitively against commands that cannot run in a
     * transaction block. We only need to match the command keyword prefix. */
    size_t remaining = strlen(cursor);

    /* VACUUM (with optional parenthesized options like VACUUM (ANALYZE)) */
    if (remaining >= 6) {
        char upper[7];
        for (int i = 0; i < 6; i++) {
            upper[i] = (char)toupper((unsigned char)cursor[i]);
        }
        upper[6] = '\0';
        if (memcmp(upper, "VACUUM", 6) == 0) {
            /* Verify it's a word boundary (space, paren, end of string) */
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
