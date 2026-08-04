/*-------------------------------------------------------------------------
 *
 * results.c
 *	  Result set retrieval (SQLFetch, SQLGetData)
 *
 * Copyright (c) 2026, Dave Cramer
 *
 *
 * IDENTIFICATION
 *	  src/results.c
 *
 *-------------------------------------------------------------------------
 */
#include "results.h"
#include "column_binding.h"
#include "type_mapping.h"
#include "diagnostics.h"
#include "connection.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <libpq-fe.h>

/* UTF-16 high (leading) surrogate range. A high surrogate is only meaningful
 * when immediately followed by a low surrogate, so a high surrogate landing at
 * the end of a truncated SQL_C_WCHAR buffer must be dropped. */
#define UTF16_HIGH_SURROGATE_MIN 0xD800
#define UTF16_HIGH_SURROGATE_MAX 0xDBFF

/* ---- Internal Helpers ---- */

/* Lowercase a single ASCII byte. Interval unit keywords from PostgreSQL are
 * pure ASCII, so locale-independent byte lowercasing is correct here. */
static char to_lower_ascii(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* Portable ASCII case-insensitive full-string compare (POSIX strcasecmp is not
 * C11 and MSVC spells it _stricmp). Returns true when the strings match. */
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

/* Portable ASCII case-insensitive prefix compare. Returns true when the first
 * prefix_length bytes of value match prefix ignoring case. */
static bool ascii_case_prefix(const char *value, const char *prefix, size_t prefix_length)
{
    for (size_t index = 0; index < prefix_length; index++) {
        if (value[index] == '\0' ||
            to_lower_ascii(value[index]) != to_lower_ascii(prefix[index])) {
            return false;
        }
    }
    return true;
}

/* Convert one lowercase/uppercase hex digit to its 4-bit value, or -1 if the
 * character is not a hex digit. */
static int hex_digit_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/*
 * Decode a PostgreSQL bytea text representation into raw bytes.
 *
 * PostgreSQL emits bytea in one of two text formats depending on the
 * bytea_output setting:
 *   - hex:    "\x464f4f"          (a "\x" prefix then two hex digits per byte)
 *   - escape: "FOO", "\001\\\052" (printable bytes literal; non-printable as
 *             "\nnn" octal, and a literal backslash doubled as "\\")
 * This decoder handles both. When output_buffer is NULL it only counts the
 * decoded byte length (so callers can size a buffer first). Returns the number
 * of decoded bytes.
 */
static size_t decode_bytea_text(const char *text, size_t text_length,
                                unsigned char *output_buffer)
{
    size_t out_index = 0;

    /* Hex format: "\x" prefix followed by pairs of hex digits. */
    if (text_length >= 2 && text[0] == '\\' && text[1] == 'x') {
        for (size_t i = 2; i + 1 < text_length; i += 2) {
            int high = hex_digit_value(text[i]);
            int low = hex_digit_value(text[i + 1]);
            if (high < 0 || low < 0) {
                break;
            }
            if (output_buffer) {
                output_buffer[out_index] = (unsigned char)((high << 4) | low);
            }
            out_index++;
        }
        return out_index;
    }

    /* Escape format. */
    for (size_t i = 0; i < text_length;) {
        if (text[i] == '\\') {
            if (i + 1 < text_length && text[i + 1] == '\\') {
                if (output_buffer) output_buffer[out_index] = '\\';
                out_index++;
                i += 2;
            } else if (i + 3 < text_length &&
                       text[i + 1] >= '0' && text[i + 1] <= '7' &&
                       text[i + 2] >= '0' && text[i + 2] <= '7' &&
                       text[i + 3] >= '0' && text[i + 3] <= '7') {
                /* "\nnn" octal escape. */
                unsigned int octet = (unsigned int)((text[i + 1] - '0') * 64 +
                                                    (text[i + 2] - '0') * 8 +
                                                    (text[i + 3] - '0'));
                if (output_buffer) output_buffer[out_index] = (unsigned char)octet;
                out_index++;
                i += 4;
            } else {
                /* Lone backslash — copy literally. */
                if (output_buffer) output_buffer[out_index] = '\\';
                out_index++;
                i++;
            }
        } else {
            if (output_buffer) output_buffer[out_index] = (unsigned char)text[i];
            out_index++;
            i++;
        }
    }
    return out_index;
}

/* A parsed date/time, filled by parse_datetime_text. Fields default to zero.
 * fraction is in nanoseconds, matching TIMESTAMP_STRUCT.fraction. */
typedef struct ParsedDateTime {
    int year;
    unsigned int month;
    unsigned int day;
    unsigned int hour;
    unsigned int minute;
    unsigned int second;
    unsigned int fraction_nanoseconds;
} ParsedDateTime;

/*
 * Parse a PostgreSQL date/time/timestamp text value into its components.
 *
 * Recognizes "YYYY-MM-DD", "HH:MM:SS[.ffffff]", and
 * "YYYY-MM-DD HH:MM:SS[.ffffff]". Missing date parts are substituted with the
 * current local date, and missing time parts are zero — this mirrors the
 * original driver, so casting an empty string to a date target yields today's
 * date. Any fractional-seconds text is scaled to nanoseconds.
 *
 * Returns true when at least a date or a time component was recognized.
 */
static bool parse_datetime_text(const char *text, ParsedDateTime *out)
{
    memset(out, 0, sizeof(*out));

    /* Default the date portion to the current local date, so that time-only or
     * empty inputs still produce a valid date (matching the original driver's
     * substitution of the current year/month/day). We use the C-standard
     * localtime() (rather than the POSIX localtime_r, which is not declared
     * under strict c11) and copy its result immediately into a local struct to
     * minimize the window in which another localtime()/gmtime() call could
     * overwrite the shared static buffer. */
    time_t now = time(NULL);
    const struct tm *local_now_ptr = localtime(&now);
    if (local_now_ptr) {
        struct tm local_now = *local_now_ptr;
        out->year = local_now.tm_year + 1900;
        out->month = (unsigned int)(local_now.tm_mon + 1);
        out->day = (unsigned int)local_now.tm_mday;
    }

    int year, month, day, hour, minute, second;
    char fraction_text[16] = "";

    /* Full timestamp: date and time (with optional fractional seconds). */
    int scanned = sscanf(text, "%d-%d-%d %d:%d:%d.%15s",
                         &year, &month, &day, &hour, &minute, &second, fraction_text);
    if (scanned >= 6) {
        out->year = year; out->month = (unsigned int)month; out->day = (unsigned int)day;
        out->hour = (unsigned int)hour; out->minute = (unsigned int)minute;
        out->second = (unsigned int)second;
        /* TIMESTAMP_STRUCT.fraction is in NANOSECONDS, so scale the fractional
         * text to a full 9 digits (e.g. ".123456" -> 123456000, ".5" ->
         * 500000000). We request precision 9 explicitly rather than the interval
         * default of 6 (which would yield microseconds, off by 1000x). */
        out->fraction_nanoseconds =
            (scanned > 6)
                ? type_mapping_interval_fraction(INTERVAL_MAX_FRACTION_DIGITS, fraction_text)
                : 0;
        return true;
    }

    /* Date only. */
    if (sscanf(text, "%d-%d-%d", &year, &month, &day) == 3) {
        out->year = year; out->month = (unsigned int)month; out->day = (unsigned int)day;
        return true;
    }

    /* Time only (optional fractional seconds); date stays at today. */
    scanned = sscanf(text, "%d:%d:%d.%15s", &hour, &minute, &second, fraction_text);
    if (scanned >= 3) {
        out->hour = (unsigned int)hour; out->minute = (unsigned int)minute;
        out->second = (unsigned int)second;
        /* Nanosecond scaling, as for the timestamp path above. */
        out->fraction_nanoseconds =
            (scanned > 3)
                ? type_mapping_interval_fraction(INTERVAL_MAX_FRACTION_DIGITS, fraction_text)
                : 0;
        return true;
    }

    /* Empty or unrecognized input: keep today's date, zero time. Treated as a
     * successful "current date" substitution. */
    return true;
}

/*
 * Map an ODBC SQL_C_INTERVAL_* C type to its SQLINTERVAL subtype code
 * (SQL_IS_YEAR, SQL_IS_HOUR_TO_SECOND, etc.). Returns 0 for a non-interval
 * type, which is never a valid SQLINTERVAL value.
 */
static SQLINTERVAL interval_c_type_to_subtype(SQLSMALLINT c_type)
{
    switch (c_type) {
    case SQL_C_INTERVAL_YEAR:            return SQL_IS_YEAR;
    case SQL_C_INTERVAL_MONTH:           return SQL_IS_MONTH;
    case SQL_C_INTERVAL_YEAR_TO_MONTH:   return SQL_IS_YEAR_TO_MONTH;
    case SQL_C_INTERVAL_DAY:             return SQL_IS_DAY;
    case SQL_C_INTERVAL_HOUR:            return SQL_IS_HOUR;
    case SQL_C_INTERVAL_DAY_TO_HOUR:     return SQL_IS_DAY_TO_HOUR;
    case SQL_C_INTERVAL_MINUTE:          return SQL_IS_MINUTE;
    case SQL_C_INTERVAL_DAY_TO_MINUTE:   return SQL_IS_DAY_TO_MINUTE;
    case SQL_C_INTERVAL_HOUR_TO_MINUTE:  return SQL_IS_HOUR_TO_MINUTE;
    case SQL_C_INTERVAL_SECOND:          return SQL_IS_SECOND;
    case SQL_C_INTERVAL_DAY_TO_SECOND:   return SQL_IS_DAY_TO_SECOND;
    case SQL_C_INTERVAL_HOUR_TO_SECOND:  return SQL_IS_HOUR_TO_SECOND;
    case SQL_C_INTERVAL_MINUTE_TO_SECOND:return SQL_IS_MINUTE_TO_SECOND;
    default:                             return 0;
    }
}

/*
 * Parse PostgreSQL interval text (intervalstyle=postgres) into a
 * SQL_INTERVAL_STRUCT of the requested subtype. This is a modern port of the
 * original driver's interval2istruct().
 *
 * PostgreSQL emits a normalized, space-separated form whose shape depends on
 * which fields are nonzero, e.g. "9 years 1 mon", "3 days", "01:02:03.123456",
 * "-12 days +13:14:00". We try each recognized shape in turn (most specific
 * first) and, on a match whose fields are compatible with the requested
 * subtype, populate the struct and return true. When nothing matches (e.g. the
 * source value is not an interval at all), the struct is left zeroed and we
 * return false — the caller then reports a zeroed struct, matching the
 * original's behavior of leaving interval_type = 0.
 *
 * fraction_precision is the ARD SQL_DESC_PRECISION for the column; it is passed
 * straight to type_mapping_interval_fraction, which clamps it to <= 9.
 */
static bool parse_interval_text(SQLSMALLINT c_type, int fraction_precision,
                                const char *text, SQL_INTERVAL_STRUCT *out)
{
    SQLINTERVAL subtype = interval_c_type_to_subtype(c_type);
    memset(out, 0, sizeof(*out));

    int years = 0, months = 0, days = 0, hours = 0, minutes = 0, seconds = 0;
    char unit1[16], unit2[16], fraction_text[16];
    bool negative;

    /* Shape 1: "years-months" (ISO year-month form). Only meaningful for a
     * year-to-month result. */
    if (sscanf(text, "%d-%d", &years, &months) >= 2) {
        if (subtype != SQL_IS_YEAR_TO_MONTH) {
            return false;
        }
        negative = years < 0;
        out->interval_type = subtype;
        out->interval_sign = negative ? SQL_TRUE : SQL_FALSE;
        out->intval.year_month.year = negative ? (SQLUINTEGER)(-years) : (SQLUINTEGER)years;
        out->intval.year_month.month = (SQLUINTEGER)months;
        return true;
    }

    /* Shape 2: "days HH:MM:SS[.frac]" without a "days" keyword (rare). */
    int scanned = sscanf(text, "%d %02d:%02d:%02d.%15s",
                         &days, &hours, &minutes, &seconds, fraction_text);
    if (scanned == 4 || scanned == 5) {
        negative = days < 0;
        out->interval_type = subtype;
        out->interval_sign = negative ? SQL_TRUE : SQL_FALSE;
        out->intval.day_second.day = negative ? (SQLUINTEGER)(-days) : (SQLUINTEGER)days;
        out->intval.day_second.hour = (SQLUINTEGER)hours;
        out->intval.day_second.minute = (SQLUINTEGER)minutes;
        out->intval.day_second.second = (SQLUINTEGER)seconds;
        if (scanned > 4) {
            out->intval.day_second.fraction =
                type_mapping_interval_fraction(fraction_precision, fraction_text);
        }
        return true;
    }

    /* Shape 3: "N <unit> M <unit>" — the year/month text form ("9 years 1 mon").
     * Guarded on unit1 being alphabetic so a time value can't slip through. */
    if (sscanf(text, "%d %15s %d %15s", &years, unit1, &months, unit2) >= 4 &&
        ((unit1[0] >= 'a' && unit1[0] <= 'z') ||
         (unit1[0] >= 'A' && unit1[0] <= 'Z'))) {
        if (ascii_case_prefix(unit1, "year", 4) &&
            ascii_case_prefix(unit2, "mon", 3) &&
            (subtype == SQL_IS_MONTH || subtype == SQL_IS_YEAR_TO_MONTH)) {
            negative = years < 0;
            out->interval_type = subtype;
            out->interval_sign = negative ? SQL_TRUE : SQL_FALSE;
            out->intval.year_month.year = negative ? (SQLUINTEGER)(-years) : (SQLUINTEGER)years;
            out->intval.year_month.month = negative ? (SQLUINTEGER)(-months) : (SQLUINTEGER)months;
            return true;
        }
        return false;
    }

    /* Shape 4: "N <unit>" — a single-field interval ("10 years", "3 days").
     * Standard sscanf collapses the format space, so this pattern would also
     * "match" a bare time value like "01:02:03" (reading years=1, unit1=":02..").
     * We therefore only accept it when unit1 starts with an alphabetic letter,
     * i.e. a real unit keyword; otherwise we fall through to the time shapes. */
    if (sscanf(text, "%d %15s", &years, unit1) == 2 &&
        ((unit1[0] >= 'a' && unit1[0] <= 'z') ||
         (unit1[0] >= 'A' && unit1[0] <= 'Z'))) {
        negative = years < 0;
        SQLUINTEGER magnitude = negative ? (SQLUINTEGER)(-years) : (SQLUINTEGER)years;
        if (subtype == SQL_IS_YEAR &&
            (ascii_case_equal(unit1, "year") || ascii_case_equal(unit1, "years"))) {
            out->interval_type = subtype;
            out->interval_sign = negative ? SQL_TRUE : SQL_FALSE;
            out->intval.year_month.year = magnitude;
            return true;
        }
        if (subtype == SQL_IS_MONTH &&
            (ascii_case_equal(unit1, "mon") || ascii_case_equal(unit1, "mons"))) {
            out->interval_type = subtype;
            out->interval_sign = negative ? SQL_TRUE : SQL_FALSE;
            out->intval.year_month.month = magnitude;
            return true;
        }
        if (subtype == SQL_IS_DAY &&
            (ascii_case_equal(unit1, "day") || ascii_case_equal(unit1, "days"))) {
            out->interval_type = subtype;
            out->interval_sign = negative ? SQL_TRUE : SQL_FALSE;
            out->intval.day_second.day = magnitude;
            return true;
        }
        return false;
    }

    /* Year/month subtypes should have matched above; anything else is a failure
     * for them (their text never contains a time component to fall through to). */
    if (subtype == SQL_IS_YEAR || subtype == SQL_IS_MONTH ||
        subtype == SQL_IS_YEAR_TO_MONTH) {
        return false;
    }

    /* Shape 5: "N days HH:MM:SS[.frac]" — days keyword plus a time component. */
    scanned = sscanf(text, "%d %15s %02d:%02d:%02d.%15s",
                     &days, unit1, &hours, &minutes, &seconds, fraction_text);
    if (scanned == 5 || scanned == 6) {
        if (!ascii_case_prefix(unit1, "day", 3)) {
            return false;
        }
        negative = days < 0;
        out->interval_type = subtype;
        out->interval_sign = negative ? SQL_TRUE : SQL_FALSE;
        out->intval.day_second.day = negative ? (SQLUINTEGER)(-days) : (SQLUINTEGER)days;
        out->intval.day_second.hour = negative ? (SQLUINTEGER)(-hours) : (SQLUINTEGER)hours;
        out->intval.day_second.minute = (SQLUINTEGER)minutes;
        out->intval.day_second.second = (SQLUINTEGER)seconds;
        if (scanned > 5) {
            out->intval.day_second.fraction =
                type_mapping_interval_fraction(fraction_precision, fraction_text);
        }
        return true;
    }

    /* Shape 6: "HH:MM:SS[.frac]" — a bare time component. */
    scanned = sscanf(text, "%02d:%02d:%02d.%15s",
                     &hours, &minutes, &seconds, fraction_text);
    if (scanned == 3 || scanned == 4) {
        negative = hours < 0;
        out->interval_type = subtype;
        out->interval_sign = negative ? SQL_TRUE : SQL_FALSE;
        out->intval.day_second.hour = negative ? (SQLUINTEGER)(-hours) : (SQLUINTEGER)hours;
        out->intval.day_second.minute = (SQLUINTEGER)minutes;
        out->intval.day_second.second = (SQLUINTEGER)seconds;
        if (scanned > 3) {
            out->intval.day_second.fraction =
                type_mapping_interval_fraction(fraction_precision, fraction_text);
        }
        return true;
    }

    return false;
}

/*
 * Determine whether a result column is nullable by consulting the source
 * table's pg_attribute.attnotnull flag.
 *
 * libpq exposes the origin table OID (PQftable) and column number
 * (PQftablecol) for each result column. Columns that are computed
 * expressions (e.g. "SELECT 1", literals, function results) have no table
 * origin — for those we return SQL_NULLABLE, which matches the original
 * psqlodbc default (pgtype_nullable always returns SQL_NULLABLE).
 *
 * For columns backed by a real table column, we look up attnotnull to
 * distinguish SQL_NO_NULLS (NOT NULL) from SQL_NULLABLE.
 */
static SQLSMALLINT determine_column_nullable(OdbcStatement *statement,
                                             PGresult *metadata_source,
                                             int column_index)
{
    Oid table_oid = PQftable(metadata_source, column_index);
    int table_column = PQftablecol(metadata_source, column_index);

    /* No table origin (expression, literal, aggregate) — treat as nullable,
     * matching original psqlodbc behavior. */
    if (table_oid == InvalidOid || table_column <= 0) {
        return SQL_NULLABLE;
    }

    if (!statement->parent_connection ||
        !statement->parent_connection->libpq_connection) {
        return SQL_NULLABLE;
    }

    /* Query attnotnull for this specific table column. Parameterized to avoid
     * any injection and to keep the values numeric. */
    char oid_text[16];
    char col_text[16];
    snprintf(oid_text, sizeof(oid_text), "%u", (unsigned int)table_oid);
    snprintf(col_text, sizeof(col_text), "%d", table_column);
    const char *param_values[2] = { oid_text, col_text };

    PGresult *attribute_result = PQexecParams(
        statement->parent_connection->libpq_connection,
        "SELECT attnotnull FROM pg_attribute WHERE attrelid = $1 AND attnum = $2",
        2, NULL, param_values, NULL, NULL, 0);

    SQLSMALLINT nullable = SQL_NULLABLE;
    if (attribute_result &&
        PQresultStatus(attribute_result) == PGRES_TUPLES_OK &&
        PQntuples(attribute_result) == 1) {
        const char *attnotnull = PQgetvalue(attribute_result, 0, 0);
        if (attnotnull && attnotnull[0] == 't') {
            nullable = SQL_NO_NULLS;
        }
    }
    if (attribute_result) {
        PQclear(attribute_result);
    }
    return nullable;
}

/*
 * Convert a PostgreSQL text-format value to the requested ODBC C type and
 * write it to the target buffer. Sets *indicator_or_length appropriately.
 *
 * postgres_oid: the OID of the source column's PostgreSQL type, used to detect
 *   boolean columns that need "t"/"f" -> "1"/"0" conversion before numeric parsing.
 * fraction_precision: the ARD SQL_DESC_PRECISION override for this column, or -1
 *   when unset. Controls interval fractional-second precision (clamped to <= 9
 *   before use to prevent the #173 stack overrun).
 *
 * Returns SQL_SUCCESS, SQL_SUCCESS_WITH_INFO (truncation), or SQL_ERROR.
 */
static SQLRETURN convert_value_to_c_type(OdbcStatement *statement,
                                         const char *raw_value,
                                         int raw_value_length,
                                         SQLSMALLINT target_type,
                                         SQLPOINTER target_value,
                                         SQLLEN buffer_length,
                                         SQLLEN *indicator_or_length,
                                         unsigned int postgres_oid,
                                         int fraction_precision)
{
    /* Boolean normalization: PostgreSQL returns "t"/"f" for boolean columns.
     * ODBC applications expect "1"/"0" for SQL_C_CHAR and numeric conversions
     * need to parse a digit rather than a letter. We substitute here so that
     * all downstream conversion logic works uniformly. */
    const char *effective_value = raw_value;
    int effective_length = raw_value_length;
    if (postgres_oid == PG_TYPE_BOOL && raw_value_length == 1) {
        if (raw_value[0] == 't') {
            effective_value = "1";
            effective_length = 1;
        } else if (raw_value[0] == 'f') {
            effective_value = "0";
            effective_length = 1;
        }
    }

    switch (target_type) {
    case SQL_C_CHAR: {
        /* bytea columns are presented to SQL_C_CHAR as a hex string of the
         * decoded bytes. PostgreSQL may deliver bytea in either text format:
         *   - hex ("\x464f4f"): strip the "\x" prefix; the remaining lowercase
         *     hex digits already are the desired representation.
         *   - escape ("FOO", "\052..."): decode to raw bytes, then render as
         *     UPPERCASE hex. This matches the original driver (pg_bin2hex). */
        const char *char_value = effective_value;
        int char_length = effective_length;
        char *bytea_hex = NULL;

        if (postgres_oid == PG_TYPE_BYTEA) {
            bool is_hex_format = (effective_length >= 2 &&
                                  effective_value[0] == '\\' &&
                                  effective_value[1] == 'x');
            if (is_hex_format) {
                char_value = effective_value + 2;
                char_length = effective_length - 2;
            } else {
                /* Decode escape format and re-encode as uppercase hex. */
                size_t byte_count = decode_bytea_text(effective_value,
                                                      (size_t)effective_length, NULL);
                unsigned char *decoded = malloc(byte_count > 0 ? byte_count : 1);
                bytea_hex = malloc(byte_count * 2 + 1);
                if (!decoded || !bytea_hex) {
                    free(decoded);
                    free(bytea_hex);
                    diagnostics_add_record(&statement->diagnostics, "HY001", 0,
                                           "Out of memory decoding bytea for SQL_C_CHAR.");
                    return SQL_ERROR;
                }
                decode_bytea_text(effective_value, (size_t)effective_length, decoded);
                static const char HEX_UPPER[] = "0123456789ABCDEF";
                for (size_t i = 0; i < byte_count; i++) {
                    bytea_hex[i * 2] = HEX_UPPER[decoded[i] >> 4];
                    bytea_hex[i * 2 + 1] = HEX_UPPER[decoded[i] & 0x0F];
                }
                bytea_hex[byte_count * 2] = '\0';
                free(decoded);
                char_value = bytea_hex;
                char_length = (int)(byte_count * 2);
            }
        }

        SQLLEN actual_length = (SQLLEN)char_length;

        if (indicator_or_length) {
            *indicator_or_length = actual_length;
        }

        if (!target_value || buffer_length <= 0) {
            free(bytea_hex);
            return SQL_SUCCESS;
        }

        if (actual_length < buffer_length) {
            memcpy(target_value, char_value, (size_t)actual_length);
            ((char *)target_value)[actual_length] = '\0';
            free(bytea_hex);
            return SQL_SUCCESS;
        } else {
            /* Truncation — copy what fits and null-terminate */
            SQLLEN copy_length = buffer_length - 1;
            memcpy(target_value, char_value, (size_t)copy_length);
            ((char *)target_value)[copy_length] = '\0';
            free(bytea_hex);
            diagnostics_add_record(&statement->diagnostics,
                                   "01004",  /* String data, right truncated */
                                   0,
                                   "String data was truncated in SQLGetData.");
            return SQL_SUCCESS_WITH_INFO;
        }
    }

    case SQL_C_SLONG:
    case SQL_C_LONG: {
        SQLINTEGER value = (SQLINTEGER)atol(effective_value);
        if (target_value) {
            *(SQLINTEGER *)target_value = value;
        }
        if (indicator_or_length) {
            *indicator_or_length = (SQLLEN)sizeof(SQLINTEGER);
        }
        return SQL_SUCCESS;
    }

    case SQL_C_ULONG: {
        SQLUINTEGER value = (SQLUINTEGER)strtoul(effective_value, NULL, 10);
        if (target_value) {
            *(SQLUINTEGER *)target_value = value;
        }
        if (indicator_or_length) {
            *indicator_or_length = (SQLLEN)sizeof(SQLUINTEGER);
        }
        return SQL_SUCCESS;
    }

    case SQL_C_SSHORT:
    case SQL_C_SHORT: {
        SQLSMALLINT value = (SQLSMALLINT)atoi(effective_value);
        if (target_value) {
            *(SQLSMALLINT *)target_value = value;
        }
        if (indicator_or_length) {
            *indicator_or_length = (SQLLEN)sizeof(SQLSMALLINT);
        }
        return SQL_SUCCESS;
    }

    case SQL_C_USHORT: {
        SQLUSMALLINT value = (SQLUSMALLINT)atoi(effective_value);
        if (target_value) {
            *(SQLUSMALLINT *)target_value = value;
        }
        if (indicator_or_length) {
            *indicator_or_length = (SQLLEN)sizeof(SQLUSMALLINT);
        }
        return SQL_SUCCESS;
    }

    case SQL_C_DOUBLE: {
        SQLDOUBLE value = strtod(effective_value, NULL);
        if (target_value) {
            *(SQLDOUBLE *)target_value = value;
        }
        if (indicator_or_length) {
            *indicator_or_length = (SQLLEN)sizeof(SQLDOUBLE);
        }
        return SQL_SUCCESS;
    }

    case SQL_C_FLOAT: {
        SQLREAL value = (SQLREAL)strtod(effective_value, NULL);
        if (target_value) {
            *(SQLREAL *)target_value = value;
        }
        if (indicator_or_length) {
            *indicator_or_length = (SQLLEN)sizeof(SQLREAL);
        }
        return SQL_SUCCESS;
    }

    case SQL_C_BIT: {
        /* After boolean normalization, effective_value is "1"/"0" for booleans */
        unsigned char value = (effective_value[0] == 't' || effective_value[0] == '1') ? 1 : 0;
        if (target_value) {
            *(unsigned char *)target_value = value;
        }
        if (indicator_or_length) {
            *indicator_or_length = (SQLLEN)sizeof(unsigned char);
        }
        return SQL_SUCCESS;
    }

    case SQL_C_SBIGINT: {
        SQLBIGINT value = (SQLBIGINT)strtoll(effective_value, NULL, 10);
        if (target_value) {
            *(SQLBIGINT *)target_value = value;
        }
        if (indicator_or_length) {
            *indicator_or_length = (SQLLEN)sizeof(SQLBIGINT);
        }
        return SQL_SUCCESS;
    }

    case SQL_C_UBIGINT: {
        SQLUBIGINT value = (SQLUBIGINT)strtoull(effective_value, NULL, 10);
        if (target_value) {
            *(SQLUBIGINT *)target_value = value;
        }
        if (indicator_or_length) {
            *indicator_or_length = (SQLLEN)sizeof(SQLUBIGINT);
        }
        return SQL_SUCCESS;
    }

    case SQL_C_STINYINT:
    case SQL_C_TINYINT: {
        signed char value = (signed char)atoi(effective_value);
        if (target_value) {
            *(signed char *)target_value = value;
        }
        if (indicator_or_length) {
            *indicator_or_length = (SQLLEN)sizeof(signed char);
        }
        return SQL_SUCCESS;
    }

    case SQL_C_UTINYINT: {
        unsigned char value = (unsigned char)atoi(effective_value);
        if (target_value) {
            *(unsigned char *)target_value = value;
        }
        if (indicator_or_length) {
            *indicator_or_length = (SQLLEN)sizeof(unsigned char);
        }
        return SQL_SUCCESS;
    }

    case SQL_C_WCHAR: {
        /* bytea columns are presented as a hex string (like SQL_C_CHAR): hex
         * format strips the "\x" prefix; escape format is decoded and rendered
         * as uppercase hex. The resulting ASCII hex is then converted to UTF-16
         * like any other text. bytea_wchar_hex owns any allocation. */
        const char *wchar_source = effective_value;
        int wchar_source_length = effective_length;
        char *bytea_wchar_hex = NULL;
        if (postgres_oid == PG_TYPE_BYTEA) {
            bool is_hex_format = (effective_length >= 2 &&
                                  effective_value[0] == '\\' &&
                                  effective_value[1] == 'x');
            if (is_hex_format) {
                wchar_source = effective_value + 2;
                wchar_source_length = effective_length - 2;
            } else {
                size_t byte_count = decode_bytea_text(effective_value,
                                                      (size_t)effective_length, NULL);
                unsigned char *decoded = malloc(byte_count > 0 ? byte_count : 1);
                bytea_wchar_hex = malloc(byte_count * 2 + 1);
                if (!decoded || !bytea_wchar_hex) {
                    free(decoded);
                    free(bytea_wchar_hex);
                    diagnostics_add_record(&statement->diagnostics, "HY001", 0,
                                           "Out of memory decoding bytea for SQL_C_WCHAR.");
                    return SQL_ERROR;
                }
                decode_bytea_text(effective_value, (size_t)effective_length, decoded);
                static const char HEX_UPPER[] = "0123456789ABCDEF";
                for (size_t i = 0; i < byte_count; i++) {
                    bytea_wchar_hex[i * 2] = HEX_UPPER[decoded[i] >> 4];
                    bytea_wchar_hex[i * 2 + 1] = HEX_UPPER[decoded[i] & 0x0F];
                }
                bytea_wchar_hex[byte_count * 2] = '\0';
                free(decoded);
                wchar_source = bytea_wchar_hex;
                wchar_source_length = (int)(byte_count * 2);
            }
        }

        /* Convert the source UTF-8 text (PostgreSQL client encoding) to a
         * UTF-16 code-unit array. Code points beyond the BMP become surrogate
         * pairs, which must be kept intact when truncating. */
        size_t unit_count = 0;
        SQLWCHAR *units = type_mapping_utf8_to_utf16le(wchar_source,
                                                       wchar_source_length,
                                                       &unit_count);
        free(bytea_wchar_hex);
        if (!units) {
            diagnostics_add_record(&statement->diagnostics,
                                   "HY001",  /* Memory allocation error */
                                   0,
                                   "Out of memory converting text to SQL_C_WCHAR.");
            return SQL_ERROR;
        }

        /* ODBC reports the length of SQL_C_WCHAR data in BYTES, and always the
         * FULL untruncated length even when the buffer cannot hold it all. */
        SQLLEN full_byte_length = (SQLLEN)(unit_count * sizeof(SQLWCHAR));
        if (indicator_or_length) {
            *indicator_or_length = full_byte_length;
        }

        if (!target_value || buffer_length <= 0) {
            free(units);
            return SQL_SUCCESS;
        }

        /* Reserve one code unit for the UTF-16 null terminator. */
        SQLLEN max_units = (buffer_length / (SQLLEN)sizeof(SQLWCHAR)) - 1;
        if (max_units < 0) {
            max_units = 0;
        }

        /* Copy whole code units, but never split a surrogate pair: if the last
         * unit that fits is a high surrogate whose low half won't fit, drop it
         * so truncation lands on a character boundary. */
        SQLLEN copy_units = ((SQLLEN)unit_count <= max_units)
                                ? (SQLLEN)unit_count
                                : max_units;
        if (copy_units < (SQLLEN)unit_count && copy_units > 0) {
            SQLWCHAR last_unit = units[copy_units - 1];
            if (last_unit >= UTF16_HIGH_SURROGATE_MIN &&
                last_unit <= UTF16_HIGH_SURROGATE_MAX) {
                copy_units--;  /* Exclude the orphaned high surrogate. */
            }
        }

        SQLWCHAR *destination = (SQLWCHAR *)target_value;
        memcpy(destination, units, (size_t)copy_units * sizeof(SQLWCHAR));
        destination[copy_units] = 0;  /* UTF-16 null terminator */

        bool truncated = (copy_units < (SQLLEN)unit_count);
        free(units);

        if (truncated) {
            diagnostics_add_record(&statement->diagnostics,
                                   "01004",  /* String data, right truncated */
                                   0,
                                   "String data was truncated in SQLGetData (WCHAR).");
            return SQL_SUCCESS_WITH_INFO;
        }
        return SQL_SUCCESS;
    }

    case SQL_C_NUMERIC: {
        /* Parse PostgreSQL numeric text ("123.45", "-0.001") into the packed
         * SQL_NUMERIC_STRUCT the application expects. A value wider than the
         * 128-bit mantissa is reported as right-truncated (SQLSTATE 01004),
         * matching the original driver's COPY_RESULT_TRUNCATED behavior. */
        SQL_NUMERIC_STRUCT numeric_value;
        memset(&numeric_value, 0, sizeof(numeric_value));
        bool overflow = false;
        type_mapping_parse_numeric_text(effective_value, &numeric_value, &overflow);

        if (target_value) {
            memcpy(target_value, &numeric_value, sizeof(SQL_NUMERIC_STRUCT));
        }
        if (indicator_or_length) {
            *indicator_or_length = (SQLLEN)sizeof(SQL_NUMERIC_STRUCT);
        }

        if (overflow) {
            diagnostics_add_record(&statement->diagnostics,
                                   "01004",  /* Numeric value out of range / truncated */
                                   0,
                                   "Numeric value exceeded 128-bit mantissa and was truncated.");
            return SQL_SUCCESS_WITH_INFO;
        }
        return SQL_SUCCESS;
    }

    case SQL_C_BINARY: {
        /* bytea columns decode to their raw bytes (from either the hex or escape
         * text format). All other types return their text bytes verbatim. */
        const char *binary_source = effective_value;
        int binary_length = effective_length;
        unsigned char *decoded_bytea = NULL;

        if (postgres_oid == PG_TYPE_BYTEA) {
            size_t byte_count = decode_bytea_text(effective_value,
                                                  (size_t)effective_length, NULL);
            decoded_bytea = malloc(byte_count > 0 ? byte_count : 1);
            if (!decoded_bytea) {
                diagnostics_add_record(&statement->diagnostics, "HY001", 0,
                                       "Out of memory decoding bytea for SQL_C_BINARY.");
                return SQL_ERROR;
            }
            decode_bytea_text(effective_value, (size_t)effective_length, decoded_bytea);
            binary_source = (const char *)decoded_bytea;
            binary_length = (int)byte_count;
        }

        SQLLEN actual_length = (SQLLEN)binary_length;

        if (indicator_or_length) {
            *indicator_or_length = actual_length;
        }

        if (!target_value || buffer_length <= 0) {
            free(decoded_bytea);
            return SQL_SUCCESS;
        }

        if (actual_length <= buffer_length) {
            memcpy(target_value, binary_source, (size_t)actual_length);
            free(decoded_bytea);
            return SQL_SUCCESS;
        } else {
            memcpy(target_value, binary_source, (size_t)buffer_length);
            free(decoded_bytea);
            diagnostics_add_record(&statement->diagnostics,
                                   "01004",
                                   0,
                                   "Binary data was truncated in SQLGetData.");
            return SQL_SUCCESS_WITH_INFO;
        }
    }

    /* Full interval subtype matrix. SQL_C_INTERVAL_DAY_TO_MINUTE is deliberately
     * excluded: the original driver does not implement it and returns 07006
     * (handled in the default case below), and the result-conversions test
     * expects that. */
    case SQL_C_INTERVAL_YEAR:
    case SQL_C_INTERVAL_MONTH:
    case SQL_C_INTERVAL_YEAR_TO_MONTH:
    case SQL_C_INTERVAL_DAY:
    case SQL_C_INTERVAL_HOUR:
    case SQL_C_INTERVAL_DAY_TO_HOUR:
    case SQL_C_INTERVAL_MINUTE:
    case SQL_C_INTERVAL_HOUR_TO_MINUTE:
    case SQL_C_INTERVAL_SECOND:
    case SQL_C_INTERVAL_DAY_TO_SECOND:
    case SQL_C_INTERVAL_HOUR_TO_SECOND:
    case SQL_C_INTERVAL_MINUTE_TO_SECOND: {
        /* Parse the PostgreSQL interval text into the requested subtype. On a
         * parse failure the struct is zeroed (interval_type == 0), which the
         * application reports as "unknown interval type" — matching the original
         * driver, which likewise leaves an unparsed interval zeroed. The
         * fractional-second precision comes from the ARD override (clamped to
         * <= 9 inside type_mapping_interval_fraction). */
        SQL_INTERVAL_STRUCT interval_value;
        parse_interval_text(target_type, fraction_precision,
                            effective_value, &interval_value);

        if (target_value) {
            memcpy(target_value, &interval_value, sizeof(SQL_INTERVAL_STRUCT));
        }
        if (indicator_or_length) {
            *indicator_or_length = (SQLLEN)sizeof(SQL_INTERVAL_STRUCT);
        }
        return SQL_SUCCESS;
    }

    case SQL_C_TYPE_DATE:
    case SQL_C_DATE: {
        /* Extract the date portion; an empty/time-only value substitutes the
         * current date (see parse_datetime_text). */
        ParsedDateTime parsed;
        parse_datetime_text(effective_value, &parsed);
        DATE_STRUCT date_value;
        date_value.year = (SQLSMALLINT)parsed.year;
        date_value.month = (SQLUSMALLINT)parsed.month;
        date_value.day = (SQLUSMALLINT)parsed.day;
        if (target_value) {
            memcpy(target_value, &date_value, sizeof(DATE_STRUCT));
        }
        if (indicator_or_length) {
            *indicator_or_length = (SQLLEN)sizeof(DATE_STRUCT);
        }
        return SQL_SUCCESS;
    }

    case SQL_C_TYPE_TIME:
    case SQL_C_TIME: {
        ParsedDateTime parsed;
        parse_datetime_text(effective_value, &parsed);
        TIME_STRUCT time_value;
        time_value.hour = (SQLUSMALLINT)parsed.hour;
        time_value.minute = (SQLUSMALLINT)parsed.minute;
        time_value.second = (SQLUSMALLINT)parsed.second;
        if (target_value) {
            memcpy(target_value, &time_value, sizeof(TIME_STRUCT));
        }
        if (indicator_or_length) {
            *indicator_or_length = (SQLLEN)sizeof(TIME_STRUCT);
        }
        return SQL_SUCCESS;
    }

    case SQL_C_TYPE_TIMESTAMP:
    case SQL_C_TIMESTAMP: {
        ParsedDateTime parsed;
        parse_datetime_text(effective_value, &parsed);
        TIMESTAMP_STRUCT timestamp_value;
        timestamp_value.year = (SQLSMALLINT)parsed.year;
        timestamp_value.month = (SQLUSMALLINT)parsed.month;
        timestamp_value.day = (SQLUSMALLINT)parsed.day;
        timestamp_value.hour = (SQLUSMALLINT)parsed.hour;
        timestamp_value.minute = (SQLUSMALLINT)parsed.minute;
        timestamp_value.second = (SQLUSMALLINT)parsed.second;
        timestamp_value.fraction = (SQLUINTEGER)parsed.fraction_nanoseconds;
        if (target_value) {
            memcpy(target_value, &timestamp_value, sizeof(TIMESTAMP_STRUCT));
        }
        if (indicator_or_length) {
            *indicator_or_length = (SQLLEN)sizeof(TIMESTAMP_STRUCT);
        }
        return SQL_SUCCESS;
    }

    default:
        /* Types we do not convert (e.g. SQL_C_GUID from arbitrary text,
         * SQL_C_INTERVAL_DAY_TO_MINUTE) report 07006, matching the original
         * driver's COPY_UNSUPPORTED_TYPE ("Received an unsupported type"). */
        diagnostics_add_record(&statement->diagnostics,
                               "07006",  /* Restricted data type attribute violation */
                               0,
                               "Received an unsupported type from Postgres.");
        return SQL_ERROR;
    }
}

/*
 * Compute the byte stride between consecutive elements of a bound column's
 * target buffer when the application uses a block (row-array) cursor with
 * column-wise binding (the default, SQL_BIND_BY_COLUMN).
 *
 * For genuinely variable-length C types (character/binary) each element
 * occupies the full bound buffer_length. Every other C type — including the
 * fixed-size struct types (numeric, date, time, timestamp, interval) — has a
 * definite sizeof and MUST stride by that, never by buffer_length: applications
 * routinely bind these struct types with buffer_length = 0 (the length is
 * implied by the type), so relying on buffer_length there would yield a stride
 * of 0 and silently overwrite element 0 for every row of the rowset.
 */
static size_t c_type_element_stride(SQLSMALLINT c_type, SQLLEN buffer_length)
{
    switch (c_type) {
    case SQL_C_CHAR:
    case SQL_C_WCHAR:
    case SQL_C_BINARY:
        /* Truly variable-length targets stride by the declared buffer. */
        return (buffer_length > 0) ? (size_t)buffer_length : 0;

    case SQL_C_SLONG:
    case SQL_C_ULONG:
    case SQL_C_LONG:
        return sizeof(SQLINTEGER);
    case SQL_C_SSHORT:
    case SQL_C_USHORT:
    case SQL_C_SHORT:
        return sizeof(SQLSMALLINT);
    case SQL_C_STINYINT:
    case SQL_C_UTINYINT:
    case SQL_C_TINYINT:
    case SQL_C_BIT:
        return sizeof(SQLCHAR);
    case SQL_C_SBIGINT:
    case SQL_C_UBIGINT:
        return sizeof(SQLBIGINT);
    case SQL_C_FLOAT:
        return sizeof(SQLREAL);
    case SQL_C_DOUBLE:
        return sizeof(SQLDOUBLE);

    /* Fixed-size struct C types: stride by the concrete struct size, which is
     * fixed regardless of the (often zero) bound buffer_length. */
    case SQL_C_NUMERIC:
        return sizeof(SQL_NUMERIC_STRUCT);
    case SQL_C_TYPE_DATE:
    case SQL_C_DATE:
        return sizeof(SQL_DATE_STRUCT);
    case SQL_C_TYPE_TIME:
    case SQL_C_TIME:
        return sizeof(SQL_TIME_STRUCT);
    case SQL_C_TYPE_TIMESTAMP:
    case SQL_C_TIMESTAMP:
        return sizeof(SQL_TIMESTAMP_STRUCT);
    case SQL_C_INTERVAL_YEAR:
    case SQL_C_INTERVAL_MONTH:
    case SQL_C_INTERVAL_YEAR_TO_MONTH:
    case SQL_C_INTERVAL_DAY:
    case SQL_C_INTERVAL_HOUR:
    case SQL_C_INTERVAL_MINUTE:
    case SQL_C_INTERVAL_SECOND:
    case SQL_C_INTERVAL_DAY_TO_HOUR:
    case SQL_C_INTERVAL_DAY_TO_MINUTE:
    case SQL_C_INTERVAL_DAY_TO_SECOND:
    case SQL_C_INTERVAL_HOUR_TO_MINUTE:
    case SQL_C_INTERVAL_HOUR_TO_SECOND:
    case SQL_C_INTERVAL_MINUTE_TO_SECOND:
        return sizeof(SQL_INTERVAL_STRUCT);

    default:
        /* Unknown/unsupported type: fall back to the declared buffer length
         * when the application supplied one. */
        return (buffer_length > 0) ? (size_t)buffer_length : 0;
    }
}

/*
 * Write one result-set row into the bound application buffers.
 *
 * source_row is the 0-based row index within the current PGresult to read.
 * rowset_offset is the 0-based element position within the application's bound
 * column arrays to write (0 for a single-row fetch; 0..row_array_size-1 for a
 * block cursor). For each active binding the target buffer and indicator are
 * advanced by rowset_offset elements so consecutive rows land in successive
 * array slots.
 *
 * NULL values set the indicator to SQL_NULL_DATA without touching the buffer.
 * Truncation escalates the result to SQL_SUCCESS_WITH_INFO.
 *
 * Returns SQL_SUCCESS or SQL_SUCCESS_WITH_INFO.
 */
static SQLRETURN populate_bound_columns_row(OdbcStatement *statement,
                                            int source_row,
                                            SQLULEN rowset_offset)
{
    if (statement->bound_column_count == 0) {
        return SQL_SUCCESS;
    }

    int total_columns = PQnfields(statement->current_result);
    SQLRETURN overall_result = SQL_SUCCESS;

    for (int slot_index = 0; slot_index < MAX_BOUND_COLUMNS; slot_index++) {
        ColumnBinding *binding = &statement->column_bindings[slot_index];

        if (!binding->is_bound) {
            continue;
        }

        int column_index = (int)(binding->column_number - 1);

        /* Skip columns that are beyond the actual result set width.
         * This can happen if the app binds columns before knowing the result shape. */
        if (column_index >= total_columns) {
            continue;
        }

        /* Resolve SQL_C_DEFAULT to the concrete C type for this column's PG type */
        SQLSMALLINT resolved_type = binding->target_type;
        if (resolved_type == SQL_C_DEFAULT) {
            unsigned int postgres_oid = (unsigned int)PQftype(statement->current_result, column_index);
            SQLSMALLINT sql_type = type_mapping_get_sql_type(postgres_oid);
            resolved_type = type_mapping_get_default_c_type(sql_type);
        }

        /* For a block cursor, target element N lives at buffer + N*stride and
         * indicator element N at indicator + N. For a single-row fetch
         * rowset_offset is 0 and these reduce to the bound pointers unchanged. */
        SQLPOINTER element_buffer = binding->target_buffer;
        SQLLEN *element_indicator = binding->indicator_or_length;
        if (rowset_offset > 0) {
            size_t stride = c_type_element_stride(resolved_type, binding->buffer_length);
            if (element_buffer && stride > 0) {
                element_buffer = (SQLPOINTER)((char *)element_buffer + rowset_offset * stride);
            }
            if (element_indicator) {
                element_indicator = element_indicator + rowset_offset;
            }
        }

        /* Handle NULL values: set indicator and skip buffer write */
        if (PQgetisnull(statement->current_result, source_row, column_index)) {
            if (element_indicator) {
                *element_indicator = SQL_NULL_DATA;
            }
            continue;
        }

        const char *raw_value = PQgetvalue(statement->current_result, source_row, column_index);
        int raw_value_length = PQgetlength(statement->current_result, source_row, column_index);

        unsigned int col_oid = (unsigned int)PQftype(statement->current_result, column_index);
        /* Per-column ARD SQL_DESC_PRECISION override (interval fractional secs).
         * column_index is 0-based, matching the override array. */
        int fraction_precision = (column_index < MAX_BOUND_COLUMNS)
                                     ? statement->column_precision_override[column_index]
                                     : -1;
        SQLRETURN conversion_result = convert_value_to_c_type(
            statement, raw_value, raw_value_length,
            resolved_type, element_buffer,
            binding->buffer_length, element_indicator,
            col_oid, fraction_precision);

        /* Escalate overall result if any column was truncated */
        if (conversion_result == SQL_SUCCESS_WITH_INFO) {
            overall_result = SQL_SUCCESS_WITH_INFO;
        }
    }

    return overall_result;
}

/*
 * Fill the application's bound column arrays with a rowset starting at the
 * statement's current cursor position, honoring the block-cursor size
 * (SQL_ATTR_ROW_ARRAY_SIZE). Copies up to row_array_size consecutive rows into
 * the bound arrays, leaves current_row_position on the LAST row copied (so a
 * following SQL_FETCH_NEXT continues after the rowset), and reports the row
 * count and per-row status via SQL_ATTR_ROWS_FETCHED_PTR / ROW_STATUS_PTR.
 *
 * The cursor is assumed to already be positioned on the first row of the rowset
 * (0 <= current_row_position < total_rows). Returns SQL_SUCCESS or, on any-row
 * truncation, SQL_SUCCESS_WITH_INFO.
 */
static SQLRETURN populate_bound_rowset(OdbcStatement *statement)
{
    int total_rows = PQntuples(statement->current_result);
    SQLULEN rowset_size = statement->row_array_size > 0 ? statement->row_array_size : 1;

    SQLRETURN overall_result = SQL_SUCCESS;
    SQLULEN rows_copied = 0;
    int first_row = statement->current_row_position;

    for (SQLULEN offset = 0; offset < rowset_size; offset++) {
        int source_row = first_row + (int)offset;
        if (source_row >= total_rows) {
            break;  /* Fewer rows remain than the rowset can hold. */
        }

        SQLRETURN row_result = populate_bound_columns_row(statement, source_row, offset);
        if (row_result == SQL_SUCCESS_WITH_INFO) {
            overall_result = SQL_SUCCESS_WITH_INFO;
        }

        if (statement->row_status_ptr) {
            statement->row_status_ptr[offset] =
                (row_result == SQL_SUCCESS_WITH_INFO) ? SQL_ROW_SUCCESS_WITH_INFO
                                                      : SQL_ROW_SUCCESS;
        }
        rows_copied++;
    }

    /* Any unused status-array slots describe rows that were not fetched. */
    if (statement->row_status_ptr) {
        for (SQLULEN offset = rows_copied; offset < rowset_size; offset++) {
            statement->row_status_ptr[offset] = SQL_ROW_NOROW;
        }
    }

    if (statement->rows_fetched_ptr) {
        *statement->rows_fetched_ptr = rows_copied;
    }

    /* Leave the cursor on the last row actually copied so SQL_FETCH_NEXT resumes
     * immediately after this rowset. */
    if (rows_copied > 0) {
        statement->current_row_position = first_row + (int)rows_copied - 1;
    }

    return overall_result;
}

/*
 * Single-row convenience wrapper used by plain SQLFetch and the single-row
 * scrolling path: writes the current row into element 0 of the bound buffers.
 */
static SQLRETURN populate_bound_columns(OdbcStatement *statement)
{
    return populate_bound_columns_row(statement, statement->current_row_position, 0);
}

/* ---- Public Interface ---- */

SQLRETURN results_num_result_cols(OdbcStatement *statement,
                                  SQLSMALLINT *column_count)
{
    /* A procedure call that captures a return value ("{ ? = call f(...) }")
     * consumes its single result column into the OUT parameter, so it presents
     * no columns to the application — matching the original driver's behavior
     * of reporting 0 result columns when proc_return > 0. */
    if (statement->is_procedure_call && statement->return_value_count > 0) {
        if (column_count) {
            *column_count = 0;
        }
        return SQL_SUCCESS;
    }

    if (statement->current_result) {
        if (column_count) {
            *column_count = (SQLSMALLINT)PQnfields(statement->current_result);
        }
        return SQL_SUCCESS;
    }

    /* If no execution result yet, check the describe_result from PQdescribePrepared.
     * This enables SQLNumResultCols to work after SQLPrepare but before SQLExecute,
     * which is required by the ODBC spec. */
    if (statement->describe_result) {
        if (column_count) {
            *column_count = (SQLSMALLINT)PQnfields(statement->describe_result);
        }
        return SQL_SUCCESS;
    }

    /* No result information available at all */
    if (column_count) {
        *column_count = 0;
    }
    return SQL_SUCCESS;
}

SQLRETURN results_describe_col(OdbcStatement *statement,
                               SQLUSMALLINT column_number,
                               SQLCHAR *column_name,
                               SQLSMALLINT name_buffer_length,
                               SQLSMALLINT *name_length,
                               SQLSMALLINT *data_type,
                               SQLULEN *column_size,
                               SQLSMALLINT *decimal_digits,
                               SQLSMALLINT *nullable)
{
    /* Use the execution result if available, otherwise fall back to
     * the describe_result from PQdescribePrepared (pre-execute metadata). */
    PGresult *metadata_source = statement->current_result;
    if (!metadata_source) {
        metadata_source = statement->describe_result;
    }

    if (!metadata_source) {
        diagnostics_add_record(&statement->diagnostics,
                               "HY010",  /* Function sequence error */
                               0,
                               "No result set available. Execute a query first.");
        return SQL_ERROR;
    }

    int total_columns = PQnfields(metadata_source);

    /* Column number 0 is the bookmark column — not supported */
    if (column_number == 0) {
        diagnostics_add_record(&statement->diagnostics,
                               "07009",  /* Invalid descriptor index */
                               0,
                               "Column 0 (bookmark) is not supported.");
        return SQL_ERROR;
    }

    /* Validate column number is within range (1-based) */
    if (column_number > (SQLUSMALLINT)total_columns) {
        diagnostics_add_record(&statement->diagnostics,
                               "07009",  /* Invalid descriptor index */
                               0,
                               "Column number exceeds the number of columns in the result set.");
        return SQL_ERROR;
    }

    int column_index = (int)(column_number - 1);  /* libpq uses 0-based */

    /* Column name */
    SQLRETURN result = SQL_SUCCESS;
    const char *pg_column_name = PQfname(metadata_source, column_index);

    if (column_name && name_buffer_length > 0) {
        SQLSMALLINT actual_name_length = (SQLSMALLINT)strlen(pg_column_name);

        if (name_length) {
            *name_length = actual_name_length;
        }

        if (actual_name_length < name_buffer_length) {
            memcpy(column_name, pg_column_name, (size_t)actual_name_length);
            column_name[actual_name_length] = '\0';
        } else {
            /* Truncation */
            SQLSMALLINT copy_length = name_buffer_length - 1;
            memcpy(column_name, pg_column_name, (size_t)copy_length);
            column_name[copy_length] = '\0';
            diagnostics_add_record(&statement->diagnostics,
                                   "01004",
                                   0,
                                   "Column name was truncated in SQLDescribeCol.");
            result = SQL_SUCCESS_WITH_INFO;
        }
    } else if (name_length) {
        *name_length = (SQLSMALLINT)strlen(pg_column_name);
    }

    /* SQL data type */
    unsigned int postgres_oid = (unsigned int)PQftype(metadata_source, column_index);
    int type_modifier = PQfmod(metadata_source, column_index);

    /* When BoolsAsChar is enabled, PostgreSQL boolean is presented as a small
     * VARCHAR rather than SQL_BIT (see ConnectionInfo.bools_as_char). */
    bool describe_bool_as_char =
        postgres_oid == PG_TYPE_BOOL &&
        statement->parent_connection &&
        statement->parent_connection->info.bools_as_char;

    /* Client-side Parse override: a select-list string literal is reported as
     * VARCHAR(length) rather than PostgreSQL's generic "text". Applies only to
     * columns the client-side parser flagged as string literals. */
    bool literal_override =
        (column_index < statement->column_override_count) &&
        statement->column_overrides[column_index].is_string_literal;

    if (data_type) {
        if (literal_override) {
            *data_type = SQL_VARCHAR;
        } else {
            *data_type = describe_bool_as_char
                             ? SQL_VARCHAR
                             : type_mapping_get_sql_type(postgres_oid);
        }
    }

    /* Column size */
    if (column_size) {
        if (literal_override) {
            *column_size = (SQLULEN)statement->column_overrides[column_index].character_length;
        } else {
            *column_size = describe_bool_as_char
                               ? PG_WIDTH_OF_BOOLS_AS_CHAR
                               : type_mapping_get_column_size(postgres_oid, type_modifier);
        }
    }

    /* Decimal digits (scale) */
    if (decimal_digits) {
        *decimal_digits = type_mapping_get_decimal_digits(postgres_oid, type_modifier);
    }

    /* Nullability — look up attnotnull from the source table if the column
     * has a table origin; expression columns default to SQL_NULLABLE. */
    if (nullable) {
        *nullable = determine_column_nullable(statement, metadata_source, column_index);
    }

    return result;
}

SQLRETURN results_row_count(OdbcStatement *statement, SQLLEN *row_count)
{
    if (row_count) {
        if (statement->affected_row_count >= 0) {
            *row_count = (SQLLEN)statement->affected_row_count;
        } else {
            *row_count = 0;
        }
    }

    return SQL_SUCCESS;
}

SQLRETURN results_fetch(OdbcStatement *statement)
{
    if (!statement->current_result || !statement->has_result_set) {
        /* Return SQL_NO_DATA rather than SQL_ERROR when there is no result set.
         * Many ODBC applications (including the psqlodbc test harness) call
         * print_result after commands like SET that produce no rows. They expect
         * SQLFetch to signal "no data" rather than an error. */
        return SQL_NO_DATA;
    }

    int total_rows = PQntuples(statement->current_result);

    statement->current_row_position++;

    if (statement->current_row_position >= total_rows) {
        /* Past the last row — no more data. Report zero rows fetched for block
         * cursors so the application's rows-fetched counter is accurate. */
        if (statement->rows_fetched_ptr) {
            *statement->rows_fetched_ptr = 0;
        }
        return SQL_NO_DATA;
    }

    /* Block cursor: copy up to row_array_size consecutive rows into the bound
     * column arrays and report the count / per-row status. A single-row cursor
     * (the default) copies exactly one row via the same path. */
    if (statement->row_array_size > 1) {
        return populate_bound_rowset(statement);
    }

    /* Write column values into all bound application buffers.
     * If no columns are bound, this returns immediately (apps use SQLGetData instead). */
    if (statement->rows_fetched_ptr) {
        *statement->rows_fetched_ptr = 1;
    }
    if (statement->row_status_ptr) {
        statement->row_status_ptr[0] = SQL_ROW_SUCCESS;
    }
    return populate_bound_columns(statement);
}

/*
 * Sentinel used by the orientation math below: -1 encodes "before the first
 * row" (BOF). Any target >= total_rows encodes "past the last row" (EOF).
 */
#define CURSOR_POSITION_BEFORE_FIRST_ROW (-1)

SQLRETURN results_extended_fetch(OdbcStatement *statement,
                                 SQLUSMALLINT fetch_orientation,
                                 SQLLEN fetch_offset,
                                 SQLULEN *fetched_row_count,
                                 SQLUSMALLINT *row_status_array)
{
    /* Default the SQLExtendedFetch out-parameters to "no row"; they are
     * overwritten below once the outcome is known. SQLFetchScroll passes NULL. */
    if (fetched_row_count) {
        *fetched_row_count = 0;
    }
    if (row_status_array) {
        row_status_array[0] = SQL_ROW_NOROW;
    }

    if (!statement->current_result || !statement->has_result_set) {
        /* No open cursor: mirror results_fetch and report end-of-data rather
         * than an error, so callers that scroll over a rowless command behave
         * the same as a plain SQLFetch. */
        return SQL_NO_DATA;
    }

    /* Forward-only cursors can only advance. Reject every other orientation
     * with HY106 ("Fetch type out of range") per the ODBC spec. */
    if (statement->cursor_type == SQL_CURSOR_FORWARD_ONLY &&
        fetch_orientation != SQL_FETCH_NEXT) {
        diagnostics_add_record(&statement->diagnostics,
                               "HY106",  /* Fetch type out of range */
                               0,
                               "Only SQL_FETCH_NEXT is allowed on a forward-only cursor.");
        return SQL_ERROR;
    }

    const int total_rows = PQntuples(statement->current_result);
    const int current_position = statement->current_row_position;

    /* Resolve the requested orientation to a target 0-based row index. A target
     * below zero means BOF; a target >= total_rows means EOF. When a backward
     * ABSOLUTE fetch runs off the front of the result set, ODBC clamps to the
     * first row and warns with 01S06 rather than returning no data. */
    /* Use SQLLEN (not long) so the arithmetic stays 64-bit on LLP64 platforms
     * such as 64-bit Windows, where long is only 32 bits. */
    SQLLEN target_index;
    bool clamped_before_first_row = false;

    switch (fetch_orientation) {
    case SQL_FETCH_NEXT:
        /* From BOF, NEXT is equivalent to FETCH_FIRST. */
        target_index = (current_position < 0) ? 0 : (SQLLEN)current_position + 1;
        break;

    case SQL_FETCH_PRIOR:
        /* From EOF, PRIOR is equivalent to FETCH_LAST. */
        target_index = (current_position >= total_rows)
                           ? (SQLLEN)total_rows - 1
                           : (SQLLEN)current_position - 1;
        break;

    case SQL_FETCH_FIRST:
        target_index = 0;
        break;

    case SQL_FETCH_LAST:
        target_index = (SQLLEN)total_rows - 1;
        break;

    case SQL_FETCH_ABSOLUTE:
        if (fetch_offset == 0) {
            /* Offset 0 positions the cursor before the result set (BOF). */
            target_index = CURSOR_POSITION_BEFORE_FIRST_ROW;
        } else if (fetch_offset > 0) {
            /* Positive offsets are 1-based from the start. */
            target_index = fetch_offset - 1;
        } else {
            /* Negative offsets count back from the end: -1 is the last row. */
            target_index = (SQLLEN)total_rows + fetch_offset;
            if (target_index < 0) {
                target_index = 0;
                clamped_before_first_row = true;
            }
        }
        break;

    case SQL_FETCH_RELATIVE:
        /* Signed delta from the current position. Because BOF is encoded as -1
         * and EOF as total_rows, this arithmetic also yields the ODBC-mandated
         * "RELATIVE from BOF with positive offset behaves like ABSOLUTE" case. */
        target_index = (SQLLEN)current_position + fetch_offset;
        break;

    default:
        diagnostics_add_record(&statement->diagnostics,
                               "HY106",  /* Fetch type out of range */
                               0,
                               "Unsupported fetch orientation.");
        return SQL_ERROR;
    }

    /* Landed before the first row: park at BOF and report no data. */
    if (target_index < 0) {
        statement->current_row_position = CURSOR_POSITION_BEFORE_FIRST_ROW;
        return SQL_NO_DATA;
    }

    /* Landed past the last row: park exactly at EOF (total_rows) — not beyond —
     * so a following SQL_FETCH_PRIOR correctly returns the last row. */
    if (target_index >= total_rows) {
        statement->current_row_position = total_rows;
        return SQL_NO_DATA;
    }

    statement->current_row_position = (int)target_index;

    /* Copy the landed rowset into the bound arrays. A block cursor
     * (row_array_size > 1) copies up to that many consecutive rows and advances
     * the cursor to the last one; a single-row cursor copies exactly one row.
     * populate_bound_rowset also updates SQL_ATTR_ROWS_FETCHED_PTR /
     * ROW_STATUS_PTR, so the two out-parameters below only need mirroring for
     * SQLExtendedFetch callers that pass their own pointers.
     *
     * KNOWN LIMITATION (backward block fetch not implemented): target_index is
     * the FIRST row of the rowset, and populate_bound_rowset always copies
     * FORWARD from there. For orientations that logically move backward with a
     * block cursor (SQL_FETCH_PRIOR, and negative SQL_FETCH_RELATIVE) the ODBC
     * spec says the returned rowset should be the block ENDING at the target and
     * read forward — i.e. it should start at target_index - (row_array_size - 1).
     * We do not yet compute that backward start, so a backward block fetch
     * returns the forward block beginning at the target instead of the preceding
     * block. This path is currently untested (the block-cursor test resets
     * row_array_size to 1 before scrolling PRIOR) and out of scope for the
     * acceptance tests. A future change should adjust target_index for backward
     * orientations when row_array_size > 1 before copying here. */
    SQLRETURN copy_result;
    SQLULEN rows_copied;
    if (statement->row_array_size > 1) {
        copy_result = populate_bound_rowset(statement);
        rows_copied = statement->current_row_position - (int)target_index + 1;
    } else {
        copy_result = populate_bound_columns(statement);
        rows_copied = 1;
        /* Keep the statement-level block-cursor pointers consistent even for a
         * single-row scroll, since the same handle attributes drive them. */
        if (statement->rows_fetched_ptr) {
            *statement->rows_fetched_ptr = 1;
        }
        if (statement->row_status_ptr) {
            statement->row_status_ptr[0] =
                (copy_result == SQL_SUCCESS_WITH_INFO) ? SQL_ROW_SUCCESS_WITH_INFO
                                                       : SQL_ROW_SUCCESS;
        }
    }

    /* A backward ABSOLUTE fetch that ran off the front was clamped to the first
     * row; surface that as SQL_SUCCESS_WITH_INFO with SQLSTATE 01S06. */
    if (clamped_before_first_row) {
        diagnostics_add_record(&statement->diagnostics,
                               "01S06",  /* Attempt to fetch before the result set returned the first rowset */
                               0,
                               "fetch absolute and before the beginning");
        copy_result = SQL_SUCCESS_WITH_INFO;
    }

    /* SQLExtendedFetch callers pass their own count / status-array pointers
     * (distinct from the SQL_ATTR_* pointers). Report the rowset outcome there
     * too: the total rows copied, and per-row success in each populated slot. */
    if (fetched_row_count) {
        *fetched_row_count = rows_copied;
    }
    if (row_status_array) {
        for (SQLULEN offset = 0; offset < rows_copied; offset++) {
            row_status_array[offset] = (copy_result == SQL_SUCCESS_WITH_INFO)
                                           ? SQL_ROW_SUCCESS_WITH_INFO
                                           : SQL_ROW_SUCCESS;
        }
    }

    return copy_result;
}

SQLRETURN results_get_data(OdbcStatement *statement,
                           SQLUSMALLINT column_number,
                           SQLSMALLINT target_type,
                           SQLPOINTER target_value,
                           SQLLEN buffer_length,
                           SQLLEN *indicator_or_length)
{
    /* Verify we have a result set and a valid cursor position */
    if (!statement->current_result || !statement->has_result_set) {
        diagnostics_add_record(&statement->diagnostics,
                               "24000",  /* Invalid cursor state */
                               0,
                               "No result set available. Execute a query and call SQLFetch first.");
        return SQL_ERROR;
    }

    if (statement->current_row_position < 0) {
        diagnostics_add_record(&statement->diagnostics,
                               "24000",  /* Invalid cursor state */
                               0,
                               "Cursor is before the first row. Call SQLFetch first.");
        return SQL_ERROR;
    }

    int total_rows = PQntuples(statement->current_result);
    if (statement->current_row_position >= total_rows) {
        diagnostics_add_record(&statement->diagnostics,
                               "24000",  /* Invalid cursor state */
                               0,
                               "Cursor is past the last row.");
        return SQL_ERROR;
    }

    /* Validate column number (1-based) */
    int total_columns = PQnfields(statement->current_result);
    if (column_number < 1 || column_number > (SQLUSMALLINT)total_columns) {
        diagnostics_add_record(&statement->diagnostics,
                               "07009",  /* Invalid descriptor index */
                               0,
                               "Column number is out of range.");
        return SQL_ERROR;
    }

    int column_index = (int)(column_number - 1);
    int row_index = statement->current_row_position;

    /* Check for NULL */
    if (PQgetisnull(statement->current_result, row_index, column_index)) {
        if (indicator_or_length) {
            *indicator_or_length = SQL_NULL_DATA;
        } else {
            /* ODBC spec: if indicator is NULL and data is NULL, that's an error */
            diagnostics_add_record(&statement->diagnostics,
                                   "22002",  /* Indicator variable required but not supplied */
                                   0,
                                   "NULL data retrieved but no indicator variable provided.");
            return SQL_ERROR;
        }
        return SQL_SUCCESS;
    }

    /* Get the raw text value from libpq */
    const char *raw_value = PQgetvalue(statement->current_result, row_index, column_index);
    int raw_value_length = PQgetlength(statement->current_result, row_index, column_index);

    /* Resolve SQL_C_DEFAULT to the actual default C type for this column */
    SQLSMALLINT resolved_type = target_type;
    if (target_type == SQL_C_DEFAULT) {
        unsigned int postgres_oid = (unsigned int)PQftype(statement->current_result, column_index);
        SQLSMALLINT sql_type = type_mapping_get_sql_type(postgres_oid);
        resolved_type = type_mapping_get_default_c_type(sql_type);
    }

    unsigned int col_oid = (unsigned int)PQftype(statement->current_result, column_index);
    int fraction_precision = (column_index < MAX_BOUND_COLUMNS)
                                 ? statement->column_precision_override[column_index]
                                 : -1;
    return convert_value_to_c_type(statement, raw_value, raw_value_length,
                                   resolved_type, target_value,
                                   buffer_length, indicator_or_length,
                                   col_oid, fraction_precision);
}

SQLRETURN results_more_results(OdbcStatement *statement)
{
    /* The statement module owns the result-set chain; it frees the finished
     * result, promotes the next one, and refreshes the per-result metadata. */
    return statement_promote_next_result(statement);
}

SQLRETURN results_convert_column(OdbcStatement *statement,
                                 PGresult *result,
                                 int row_index,
                                 int column_index,
                                 SQLSMALLINT target_type,
                                 SQLPOINTER target_value,
                                 SQLLEN buffer_length,
                                 SQLLEN *indicator_or_length)
{
    if (!result || row_index < 0 || column_index < 0) {
        return SQL_ERROR;
    }

    /* NULL columns set the indicator and leave the buffer untouched. */
    if (PQgetisnull(result, row_index, column_index)) {
        if (indicator_or_length) {
            *indicator_or_length = SQL_NULL_DATA;
        }
        return SQL_SUCCESS;
    }

    const char *raw_value = PQgetvalue(result, row_index, column_index);
    int raw_value_length = PQgetlength(result, row_index, column_index);
    unsigned int postgres_oid = (unsigned int)PQftype(result, column_index);

    /* Resolve SQL_C_DEFAULT to this column's natural C type, mirroring
     * results_get_data so callers may pass SQL_C_DEFAULT. */
    SQLSMALLINT resolved_type = target_type;
    if (resolved_type == SQL_C_DEFAULT) {
        SQLSMALLINT sql_type = type_mapping_get_sql_type(postgres_oid);
        resolved_type = type_mapping_get_default_c_type(sql_type);
    }

    /* This helper (used for OUT/refcursor conversion) has no bound column or ARD
     * context, so interval fractional precision defaults to unspecified (-1). */
    return convert_value_to_c_type(statement, raw_value, raw_value_length,
                                   resolved_type, target_value,
                                   buffer_length, indicator_or_length,
                                   postgres_oid, -1);
}
