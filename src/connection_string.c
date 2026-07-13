/*-------------------------------------------------------------------------
 *
 * connection_string.c
 *	  ODBC connection string parsing
 *
 * Copyright (c) 2026, Dave Cramer
 *
 *
 * IDENTIFICATION
 *	  src/connection_string.c
 *
 *-------------------------------------------------------------------------
 */
#include "connection_string.h"
#include "dsn_config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Internal Helpers ---- */

/* Case-insensitive string comparison (portable; not all platforms have strcasecmp) */
static int case_insensitive_compare(const char *left, const char *right)
{
    while (*left && *right) {
        int diff = tolower((unsigned char)*left) - tolower((unsigned char)*right);
        if (diff != 0) {
            return diff;
        }
        left++;
        right++;
    }
    return tolower((unsigned char)*left) - tolower((unsigned char)*right);
}

/* Safely copy a string into a fixed-size buffer, always null-terminating */
static void safe_copy_to_buffer(char *destination, size_t destination_size, const char *source, size_t source_length)
{
    size_t copy_length = source_length;
    if (copy_length >= destination_size) {
        copy_length = destination_size - 1;
    }
    memcpy(destination, source, copy_length);
    destination[copy_length] = '\0';
}

/* Store a parsed key-value pair into the appropriate ConnectionInfo field */
static void store_connection_parameter(ConnectionInfo *info, const char *key, const char *value, size_t value_length)
{
    if (case_insensitive_compare(key, "Server") == 0 ||
        case_insensitive_compare(key, "Servername") == 0) {
        safe_copy_to_buffer(info->server, sizeof(info->server), value, value_length);

    } else if (case_insensitive_compare(key, "Port") == 0) {
        safe_copy_to_buffer(info->port, sizeof(info->port), value, value_length);

    } else if (case_insensitive_compare(key, "Database") == 0 ||
               case_insensitive_compare(key, "DB") == 0) {
        safe_copy_to_buffer(info->database, sizeof(info->database), value, value_length);

    } else if (case_insensitive_compare(key, "UID") == 0 ||
               case_insensitive_compare(key, "Username") == 0 ||
               case_insensitive_compare(key, "User") == 0) {
        safe_copy_to_buffer(info->username, sizeof(info->username), value, value_length);

    } else if (case_insensitive_compare(key, "PWD") == 0 ||
               case_insensitive_compare(key, "Password") == 0) {
        /* Password is heap-allocated so it can be securely wiped later */
        free(info->password);
        info->password = malloc(value_length + 1);
        if (info->password) {
            memcpy(info->password, value, value_length);
            info->password[value_length] = '\0';
        }

    } else if (case_insensitive_compare(key, "SSLmode") == 0) {
        safe_copy_to_buffer(info->sslmode, sizeof(info->sslmode), value, value_length);

    } else if (case_insensitive_compare(key, "ApplicationName") == 0 ||
               case_insensitive_compare(key, "Application_Name") == 0) {
        safe_copy_to_buffer(info->application_name, sizeof(info->application_name), value, value_length);

    } else if (case_insensitive_compare(key, "Timeout") == 0 ||
               case_insensitive_compare(key, "Connect_Timeout") == 0) {
        /* Parse timeout as an unsigned integer */
        char timeout_buffer[16];
        safe_copy_to_buffer(timeout_buffer, sizeof(timeout_buffer), value, value_length);
        info->connect_timeout = (unsigned int)strtoul(timeout_buffer, NULL, 10);

    } else if (case_insensitive_compare(key, "BoolsAsChar") == 0) {
        /* Any non-zero integer enables describing bool columns as VARCHAR(5).
         * A value of "0" turns it off, exposing bool as SQL_BIT instead. */
        char bool_buffer[16];
        safe_copy_to_buffer(bool_buffer, sizeof(bool_buffer), value, value_length);
        info->bools_as_char = (strtol(bool_buffer, NULL, 10) != 0);

    } else if (case_insensitive_compare(key, "UnknownSizes") == 0) {
        char size_buffer[16];
        safe_copy_to_buffer(size_buffer, sizeof(size_buffer), value, value_length);
        info->unknown_sizes = (int)strtol(size_buffer, NULL, 10);

    } else if (case_insensitive_compare(key, "MaxVarcharSize") == 0) {
        char size_buffer[16];
        safe_copy_to_buffer(size_buffer, sizeof(size_buffer), value, value_length);
        info->max_varchar_size = (int)strtol(size_buffer, NULL, 10);

    } else if (case_insensitive_compare(key, "Parse") == 0) {
        /* Enable client-side SELECT parsing for refined column metadata. */
        char parse_buffer[16];
        safe_copy_to_buffer(parse_buffer, sizeof(parse_buffer), value, value_length);
        info->parse_statements = (strtol(parse_buffer, NULL, 10) != 0);

    } else if (case_insensitive_compare(key, "FetchRefcursors") == 0) {
        /* When enabled, refcursor OUT parameters returned by a called function
         * are automatically FETCH ALL'd and exposed as successive result sets. */
        char refcursor_buffer[16];
        safe_copy_to_buffer(refcursor_buffer, sizeof(refcursor_buffer), value, value_length);
        info->fetch_refcursors = (strtol(refcursor_buffer, NULL, 10) != 0);

    } else if (case_insensitive_compare(key, "Fetch") == 0) {
        /* Cursor fetch cache size in the original driver. This modern driver
         * materializes whole result sets, so the cache size has no effect; the
         * keyword is accepted (and ignored) for connection-string compatibility
         * with applications and tests that set it. */

    } else if (case_insensitive_compare(key, "DisallowPremature") == 0) {
        /* Accepted for compatibility with the original driver; the modern
         * implementation always describes results after execution, so there is
         * no "premature" describe to disallow. No stored effect. */

    } else if (case_insensitive_compare(key, "DSN") == 0) {
        /* Resolve the DSN from odbc.ini to populate connection defaults.
         * Subsequent keys in the connection string will override these values
         * because parsing continues after this point. If DSN lookup fails
         * (no odbcinst, or DSN not found), fall back to using the DSN name
         * as the database name for backward compatibility. */
        if (!dsn_config_read(value, info)) {
            safe_copy_to_buffer(info->database, sizeof(info->database), value, value_length);
        }

    } else if (case_insensitive_compare(key, "Driver") == 0) {
        /* The Driver key is consumed by the Driver Manager to select the
         * driver shared library. By the time the driver sees it, it's irrelevant. */
    }
    /* Unknown keys are silently ignored for forward compatibility */
}

bool connection_string_parse(const char *odbc_connection_string,
                             SQLSMALLINT string_length,
                             ConnectionInfo *out_info)
{
    if (!out_info) {
        return false;
    }

    /* Handle NULL or empty input gracefully */
    if (!odbc_connection_string) {
        return true;  /* Nothing to parse is not an error */
    }

    size_t actual_length;
    if (string_length == SQL_NTS) {
        actual_length = strlen(odbc_connection_string);
    } else if (string_length < 0) {
        return false;
    } else {
        actual_length = (size_t)string_length;
    }

    if (actual_length == 0) {
        return true;
    }

    /* Work with a null-terminated copy so we can use string functions safely */
    char *working_copy = malloc(actual_length + 1);
    if (!working_copy) {
        return false;
    }
    memcpy(working_copy, odbc_connection_string, actual_length);
    working_copy[actual_length] = '\0';

    const char *position = working_copy;
    const char *end = working_copy + actual_length;

    while (position < end) {
        /* Skip leading whitespace and semicolons between pairs */
        while (position < end && (*position == ';' || *position == ' ' || *position == '\t')) {
            position++;
        }
        if (position >= end) {
            break;
        }

        /* Extract key: everything up to '=' */
        const char *key_start = position;
        while (position < end && *position != '=') {
            position++;
        }
        if (position >= end) {
            /* Key with no '=' — skip it */
            break;
        }

        /* Trim trailing whitespace from key */
        const char *key_end = position;
        while (key_end > key_start && (*(key_end - 1) == ' ' || *(key_end - 1) == '\t')) {
            key_end--;
        }

        /* Null-terminate key in our working copy */
        size_t key_length = (size_t)(key_end - key_start);
        char key_buffer[256];
        if (key_length >= sizeof(key_buffer)) {
            key_length = sizeof(key_buffer) - 1;
        }
        memcpy(key_buffer, key_start, key_length);
        key_buffer[key_length] = '\0';

        /* Skip the '=' */
        position++;

        /* Skip leading whitespace in value */
        while (position < end && (*position == ' ' || *position == '\t')) {
            position++;
        }

        /* Extract value: handle brace-enclosed values for embedded semicolons */
        const char *value_start;
        size_t value_length;

        if (position < end && *position == '{') {
            /* Brace-enclosed value: read until closing brace */
            position++;  /* skip opening brace */
            value_start = position;
            while (position < end && *position != '}') {
                position++;
            }
            value_length = (size_t)(position - value_start);
            if (position < end) {
                position++;  /* skip closing brace */
            }
            /* Skip trailing semicolons after the closing brace */
            while (position < end && (*position == ';' || *position == ' ')) {
                position++;
            }
        } else {
            /* Unquoted value: read until semicolon or end */
            value_start = position;
            while (position < end && *position != ';') {
                position++;
            }
            /* Trim trailing whitespace from value */
            const char *value_end = position;
            while (value_end > value_start && (*(value_end - 1) == ' ' || *(value_end - 1) == '\t')) {
                value_end--;
            }
            value_length = (size_t)(value_end - value_start);
        }

        /* Store the parsed parameter */
        if (key_length > 0) {
            /* Create a temporary null-terminated value for store function */
            char *value_buffer = malloc(value_length + 1);
            if (value_buffer) {
                memcpy(value_buffer, value_start, value_length);
                value_buffer[value_length] = '\0';
                store_connection_parameter(out_info, key_buffer, value_buffer, value_length);
                free(value_buffer);
            }
        }
    }

    free(working_copy);
    return true;
}

void connection_info_build_libpq_params(const ConnectionInfo *info,
                                        const char **out_keywords,
                                        const char **out_values,
                                        int *out_param_count)
{
    int count = 0;

    if (!info || !out_keywords || !out_values || !out_param_count) {
        if (out_param_count) {
            *out_param_count = 0;
        }
        return;
    }

    /* Only include parameters that have non-empty values.
     * PQconnectdbParams handles quoting/escaping internally, so values
     * with spaces, special characters, or embedded quotes work correctly. */

    if (info->server[0] != '\0') {
        out_keywords[count] = "host";
        out_values[count] = info->server;
        count++;
    }

    if (info->port[0] != '\0') {
        out_keywords[count] = "port";
        out_values[count] = info->port;
        count++;
    }

    if (info->database[0] != '\0') {
        out_keywords[count] = "dbname";
        out_values[count] = info->database;
        count++;
    }

    if (info->username[0] != '\0') {
        out_keywords[count] = "user";
        out_values[count] = info->username;
        count++;
    }

    if (info->password && info->password[0] != '\0') {
        out_keywords[count] = "password";
        out_values[count] = info->password;
        count++;
    }

    if (info->sslmode[0] != '\0') {
        out_keywords[count] = "sslmode";
        out_values[count] = info->sslmode;
        count++;
    }

    if (info->application_name[0] != '\0') {
        out_keywords[count] = "application_name";
        out_values[count] = info->application_name;
        count++;
    }

    /* connect_timeout is numeric — use a static buffer since the ConnectionInfo
     * outlives this call (caller uses the arrays immediately then discards). */
    static char timeout_buffer[16];
    if (info->connect_timeout > 0) {
        snprintf(timeout_buffer, sizeof(timeout_buffer), "%u", info->connect_timeout);
        out_keywords[count] = "connect_timeout";
        out_values[count] = timeout_buffer;
        count++;
    }

    /* Null-terminate the arrays as required by PQconnectdbParams */
    out_keywords[count] = NULL;
    out_values[count] = NULL;

    *out_param_count = count;
}
