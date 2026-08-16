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
#include "parameter.h"
#include "error_mapping.h"
#include "platform/string_utils.h"

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

/* Carriage return byte, prepended to each bare line feed when LFConversion
 * expands "\n" to "\r\n" on string output. */
#define CARRIAGE_RETURN_BYTE '\r'

/* ---- Internal Helpers ---- */

/*
 * Expand every bare line feed ('\n') in a byte string to a carriage-return /
 * line-feed pair ("\r\n"), returning a freshly allocated NUL-terminated copy
 * and writing its length (excluding the terminator) to *expanded_length.
 *
 * A line feed already preceded by a carriage return is left alone (only the
 * missing CR is what we add), matching the original psqlodbc convert_linefeeds.
 * Because CR and LF are single ASCII bytes that never appear as a continuation
 * byte of a multi-byte UTF-8 sequence, operating on the raw bytes is safe for
 * UTF-8 text and yields the correct result after later UTF-16 conversion.
 *
 * Returns NULL on allocation failure (the caller treats that as SQL_ERROR).
 */
static char *expand_line_feeds(const char *source, int source_length,
                               int *expanded_length)
{
    /* Worst case doubles every byte (a string of nothing but line feeds). */
    char *expanded = malloc((size_t)source_length * 2 + 1);
    if (!expanded) {
        return NULL;
    }
    int out_index = 0;
    for (int i = 0; i < source_length; i++) {
        char current = source[i];
        if (current == '\n' &&
            !(i > 0 && source[i - 1] == CARRIAGE_RETURN_BYTE)) {
            expanded[out_index++] = CARRIAGE_RETURN_BYTE;
        }
        expanded[out_index++] = current;
    }
    expanded[out_index] = '\0';
    *expanded_length = out_index;
    return expanded;
}

/*
 * Return true when this statement's connection has LFConversion enabled, so a
 * text value being delivered to SQL_C_CHAR / SQL_C_WCHAR should have its line
 * feeds expanded to CR+LF (see ConnectionInfo.lf_conversion).
 */
static bool statement_wants_line_feed_conversion(const OdbcStatement *statement)
{
    return statement->parent_connection != NULL &&
           statement->parent_connection->info.lf_conversion;
}

/*
 * Under CvtNullDate, a NULL value in a date/timestamp column is delivered to a
 * character target as an EMPTY STRING (indicator 0) rather than as
 * SQL_NULL_DATA — the symmetric read side of the empty-string-to-NULL parameter
 * rewrite, matching the original psqlodbc. Returns true when this NULL should be
 * rendered that way for the given source column type and requested C type.
 *
 * Only SQL_C_CHAR and SQL_C_WCHAR targets are eligible: the empty-string result
 * is a variable-length string value, which has no meaningful representation in a
 * fixed-width DATE_STRUCT. A date C-type (SQL_C_DATE / SQL_C_TYPE_DATE) or the
 * default C type therefore falls through to normal SQL_NULL_DATA handling.
 */
static bool statement_null_date_reads_as_empty(const OdbcStatement *statement,
                                               unsigned int column_oid,
                                               SQLSMALLINT target_type)
{
    if (!statement->parent_connection ||
        !statement->parent_connection->info.cvt_null_date) {
        return false;
    }
    if (column_oid != PG_TYPE_DATE &&
        column_oid != PG_TYPE_TIMESTAMP &&
        column_oid != PG_TYPE_TIMESTAMPTZ) {
        return false;
    }
    return target_type == SQL_C_CHAR || target_type == SQL_C_WCHAR;
}

/*
 * Write the CvtNullDate empty-string result for a NULL date/timestamp column
 * into target_value (a zero-length string) and set *indicator to 0. For a
 * character target that is a NUL terminator; for SQL_C_WCHAR a wide NUL. Sets
 * the indicator even when there is no buffer (length-query call). Returns
 * SQL_SUCCESS.
 */
static SQLRETURN write_null_date_empty_string(SQLSMALLINT target_type,
                                              SQLPOINTER target_value,
                                              SQLLEN buffer_length,
                                              SQLLEN *indicator)
{
    if (indicator) {
        *indicator = 0;
    }
    if (target_value && buffer_length > 0) {
        if (target_type == SQL_C_WCHAR &&
            buffer_length >= (SQLLEN)sizeof(SQLWCHAR)) {
            ((SQLWCHAR *)target_value)[0] = 0;
        } else {
            ((char *)target_value)[0] = '\0';
        }
    }
    return SQL_SUCCESS;
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

/*
 * True when a PostgreSQL column type is a genuine date/time/timestamp type.
 *
 * Only these types carry parseable date/time components. The original driver's
 * copy_and_convert_field only populates its SIMPLE_TIME from the value for these
 * field types; for every other type (macaddr, inet, text, ...) the time struct
 * stays zeroed. We reproduce that gating so, e.g., casting a macaddr like
 * "08:00:2b:..." to SQL_C_TYPE_TIME yields 00:00:00 rather than accidentally
 * parsing "08" as an hour.
 */
static bool field_type_is_datetime(unsigned int postgres_oid)
{
    return postgres_oid == PG_TYPE_DATE ||
           postgres_oid == PG_TYPE_TIME ||
           postgres_oid == PG_TYPE_TIMESTAMP ||
           postgres_oid == PG_TYPE_TIMESTAMPTZ;
}

/*
 * Given the text of a timestamptz value, return the length up to (but not
 * including) any trailing timezone offset.
 *
 * PostgreSQL renders timestamptz with a trailing numeric zone, e.g.
 * "2011-02-16 06:49:18-08" or "...+05:30". When such a value is delivered as a
 * character string the original driver reformats it from its parsed components
 * WITHOUT the zone, so the visible text has no offset. We reproduce that by
 * trimming from the sign that introduces the zone. The date portion also
 * contains '-' characters, so we only start looking after the space that
 * separates the date from the time.
 */
static int timestamptz_text_length_without_zone(const char *text, int length)
{
    int time_start = 0;
    for (int index = 0; index < length; index++) {
        if (text[index] == ' ') {
            time_start = index + 1;
            break;
        }
    }
    for (int index = time_start; index < length; index++) {
        if (text[index] == '+' || text[index] == '-') {
            return index;
        }
    }
    return length;
}

/*
 * Strip currency formatting from a PostgreSQL money value so it can be parsed
 * as a plain number. This is a modern port of the original driver's
 * convert_money().
 *
 * PostgreSQL formats money with a currency symbol and thousands/decimal
 * separators whose roles depend on locale (e.g. "$1,234.56", "1.234,56 kr").
 * We first decide which of '.' or ',' is the decimal separator: whichever
 * appears last and within two characters of the final digit is the decimal
 * point; the other is a thousands separator to be dropped. We then emit only
 * the sign, the digits, and a single '.' decimal point into out_buffer.
 *
 * Returns false only when out_buffer is too small to hold the result.
 */
static bool convert_money_to_plain_number(const char *input,
                                          char *out_buffer,
                                          size_t out_buffer_size)
{
    int last_digit_index = -1;
    int first_period_index = -1;
    int first_comma_index = -1;
    for (int index = 0; input[index] != '\0'; index++) {
        char character = input[index];
        if (character == '.') {
            if (first_period_index < 0) {
                first_period_index = index;
            }
        } else if (character == ',') {
            if (first_comma_index < 0) {
                first_comma_index = index;
            }
        } else if (character >= '0' && character <= '9') {
            last_digit_index = index;
        }
    }

    /* Decide which separator is the decimal point: the later one, provided it
     * sits within the last two positions before the final digit (i.e. it looks
     * like fractional cents rather than a thousands group). */
    char decimal_separator = 0;
    if (first_period_index > first_comma_index) {
        if (first_period_index >= last_digit_index - 2) {
            decimal_separator = '.';
        }
    } else if (first_comma_index >= 0 &&
               first_comma_index >= last_digit_index - 2) {
        decimal_separator = ',';
    }

    size_t out_index = 0;
    for (int index = 0; input[index] != '\0'; index++) {
        if (out_index + 1 >= out_buffer_size) {
            return false;
        }
        char character = input[index];
        if (character == '(' || character == '-') {
            /* Accounting notation "(1.23)" and a leading '-' both mean negative. */
            out_buffer[out_index++] = '-';
        } else if (character >= '0' && character <= '9') {
            out_buffer[out_index++] = character;
        } else if (character == decimal_separator) {
            out_buffer[out_index++] = '.';
        }
    }
    out_buffer[out_index] = '\0';
    return true;
}

/*
 * Parse a UUID text value ("543c5e21-435a-440b-943c-64af1ad571f1") into a
 * packed SQLGUID. This is a modern port of the original driver's char2guid().
 *
 * Data1 is parsed through a temporary unsigned int because SQLGUID.Data1 is an
 * "unsigned long" on some platforms and "unsigned int" on others; "%08X" always
 * matches an unsigned int. Returns false when the text is not a well-formed
 * UUID (fewer than the 11 expected fields), which the caller maps to SQLSTATE
 * 07006 — matching the original's COPY_GENERAL_ERROR / unsupported-type result.
 */
static bool parse_uuid_text(const char *text, SQLGUID *guid)
{
    unsigned int data1 = 0;
    int matched = sscanf(text,
        "%08X-%04hX-%04hX-%02hhX%02hhX-%02hhX%02hhX%02hhX%02hhX%02hhX%02hhX",
        &data1,
        &guid->Data2, &guid->Data3,
        &guid->Data4[0], &guid->Data4[1],
        &guid->Data4[2], &guid->Data4[3],
        &guid->Data4[4], &guid->Data4[5],
        &guid->Data4[6], &guid->Data4[7]);
    if (matched < 11) {
        return false;
    }
    guid->Data1 = data1;
    return true;
}

/*
 * True when SQL_C_BINARY may render a column of this PostgreSQL type as its raw
 * text bytes.
 *
 * The original driver only lets SQL_C_BINARY pass through the textual bytes for
 * an explicit allow-list of character-like types (see convert.c's
 * text_bin_handling). Every other type either has a dedicated binary form
 * (int4, uuid, bytea handled separately by the caller) or returns
 * "unsupported type". Reproducing this list is what makes, e.g., int8 or float4
 * report 07006 for SQL_C_BINARY instead of leaking their text bytes.
 */
static bool field_type_allows_text_binary(unsigned int postgres_oid)
{
    switch (postgres_oid) {
    case PG_TYPE_UNKNOWN:
    case PG_TYPE_BPCHAR:
    case PG_TYPE_VARCHAR:
    case PG_TYPE_TEXT:
    case PG_TYPE_XML:
    case PG_TYPE_BPCHARARRAY:
    case PG_TYPE_VARCHARARRAY:
    case PG_TYPE_TEXTARRAY:
    case PG_TYPE_XMLARRAY:
        return true;
    default:
        return false;
    }
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
 * "YYYY-MM-DD HH:MM:SS[.ffffff]" (with an optional trailing timezone offset,
 * which is ignored). Only the components actually present in the text are set;
 * everything else is left zero. Any fractional-seconds text is scaled to
 * nanoseconds.
 *
 * This is a pure parser: it never substitutes the current date. Callers that
 * want the original driver's "fill missing date parts with today" behavior
 * (SQL_C_DATE / SQL_C_TIMESTAMP) apply that substitution themselves, so that
 * targets which must stay zeroed (SQL_C_TIME) are unaffected.
 *
 * Returns true when at least a date or a time component was recognized.
 */
static bool parse_datetime_text(const char *text, ParsedDateTime *out)
{
    memset(out, 0, sizeof(*out));

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

    /* Time only (optional fractional seconds). */
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

    return false;
}

/*
 * Fill any zero year/month/day in a parsed value with the current local date.
 *
 * The original driver applies this "sanity check" substitution only for
 * SQL_C_DATE and SQL_C_TIMESTAMP targets, so a value that carries a time but no
 * date (or is empty) still yields a usable date. We use C-standard localtime()
 * (POSIX localtime_r is not declared under strict C11) and immediately copy its
 * result into a local struct to minimize the window in which another
 * localtime()/gmtime() call could overwrite the shared static buffer.
 */
static void fill_missing_date_with_today(ParsedDateTime *parsed)
{
    time_t now = time(NULL);
    const struct tm *local_now_ptr = localtime(&now);
    if (!local_now_ptr) {
        return;
    }
    struct tm local_now = *local_now_ptr;
    if (parsed->year == 0) {
        parsed->year = local_now.tm_year + 1900;
    }
    if (parsed->month == 0) {
        parsed->month = (unsigned int)(local_now.tm_mon + 1);
    }
    if (parsed->day == 0) {
        parsed->day = (unsigned int)local_now.tm_mday;
    }
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
        if (pg_ascii_case_prefix(unit1, "year", 4) &&
            pg_ascii_case_prefix(unit2, "mon", 3) &&
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
            (pg_ascii_case_equal(unit1, "year") || pg_ascii_case_equal(unit1, "years"))) {
            out->interval_type = subtype;
            out->interval_sign = negative ? SQL_TRUE : SQL_FALSE;
            out->intval.year_month.year = magnitude;
            return true;
        }
        if (subtype == SQL_IS_MONTH &&
            (pg_ascii_case_equal(unit1, "mon") || pg_ascii_case_equal(unit1, "mons"))) {
            out->interval_type = subtype;
            out->interval_sign = negative ? SQL_TRUE : SQL_FALSE;
            out->intval.year_month.month = magnitude;
            return true;
        }
        if (subtype == SQL_IS_DAY &&
            (pg_ascii_case_equal(unit1, "day") || pg_ascii_case_equal(unit1, "days"))) {
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
        if (!pg_ascii_case_prefix(unit1, "day", 3)) {
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

    /* Shape 6: "HH:MM:SS.frac" — a bare time component WITH fractional seconds.
     *
     * A bare time WITHOUT a fraction is deliberately rejected. The original
     * driver reaches this shape only through its width-limited secure_sscanf:
     * its preceding "%d %10s %d" probe consumes a plain "HH:MM:SS" as two
     * fields (an int and a 10-char string) and returns FALSE, whereas
     * "HH:MM:SS.ffffff" yields three fields there and falls through to here.
     * The net, testable rule is that a lone time only converts to an interval
     * when it carries a fractional part, so real time-typed values like
     * "13:23:34" and mislabeled values like a macaddr report "unknown interval
     * type" rather than being silently parsed. We require scanned == 4 (the
     * fractional field present) to reproduce that. */
    scanned = sscanf(text, "%02d:%02d:%02d.%15s",
                     &hours, &minutes, &seconds, fraction_text);
    if (scanned == 4) {
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

    /* Date infinity clamping: PostgreSQL renders the special date values
     * 'infinity' and '-infinity' as those literal words, which no ODBC client
     * can interpret as a date. The original driver maps them to the largest and
     * smallest representable finite dates (convert.c's PG_TYPE_DATE case sets
     * 9999-12-31 and 0001-01-01). Substituting the equivalent text here — before
     * any target-type conversion — lets a single change serve every target: the
     * SQL_C_CHAR/SQL_C_WCHAR paths emit the finite date, and the date/timestamp
     * parser used by SQL_C_TYPE_DATE parses it like any ordinary "YYYY-MM-DD". */
    if (postgres_oid == PG_TYPE_DATE) {
        /* PostgreSQL's finite date domain is 4713 BC .. 5874897 AD, but ODBC's
         * DATE_STRUCT year is a signed 16-bit field, so these clamps match both
         * the original driver and the widest value the ODBC types can hold. */
        static const char DATE_POSITIVE_INFINITY_CLAMP[] = "9999-12-31";
        static const char DATE_NEGATIVE_INFINITY_CLAMP[] = "0001-01-01";
        if (pg_ascii_case_equal(effective_value, "infinity")) {
            effective_value = DATE_POSITIVE_INFINITY_CLAMP;
            effective_length = (int)(sizeof(DATE_POSITIVE_INFINITY_CLAMP) - 1);
        } else if (pg_ascii_case_equal(effective_value, "-infinity")) {
            effective_value = DATE_NEGATIVE_INFINITY_CLAMP;
            effective_length = (int)(sizeof(DATE_NEGATIVE_INFINITY_CLAMP) - 1);
        }
    }

    /* Money normalization: strip the currency symbol and separators so numeric
     * targets can parse the amount. The original driver does this for every
     * target type EXCEPT SQL_C_CHAR / SQL_C_WCHAR, which intentionally keep the
     * formatted "$1.23" text. (SQL_C_BINARY of money still reaches the numeric
     * side here, is de-symboled, and is then rejected as an unsupported type in
     * the SQL_C_BINARY case below — matching the original.) The buffer is scoped
     * to the whole function so effective_value stays valid through the switch. */
    char money_number_buffer[NUMERIC_TEXT_BUFFER_SIZE];
    if (postgres_oid == PG_TYPE_MONEY &&
        target_type != SQL_C_CHAR && target_type != SQL_C_WCHAR) {
        if (convert_money_to_plain_number(effective_value, money_number_buffer,
                                          sizeof(money_number_buffer))) {
            effective_value = money_number_buffer;
            effective_length = (int)strlen(money_number_buffer);
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

        /* timestamptz text carries a trailing timezone offset ("...18-08"). The
         * original driver reformats timestamptz from its parsed components with
         * no zone, so the visible character form has none. We reproduce that by
         * trimming the offset from the reported length. */
        if (postgres_oid == PG_TYPE_TIMESTAMPTZ) {
            char_length = timestamptz_text_length_without_zone(char_value, char_length);
        }

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

        /* LFConversion: expand bare "\n" to "\r\n". The reported length and any
         * truncation must reflect the EXPANDED bytes, so this happens before the
         * length is computed. bytea hex never contains line feeds, so restricting
         * this to non-bytea values also avoids a pointless allocation. The
         * expanded buffer reuses the bytea_hex ownership slot so the existing
         * free(bytea_hex) paths release it. */
        if (postgres_oid != PG_TYPE_BYTEA &&
            statement_wants_line_feed_conversion(statement)) {
            int expanded_length = 0;
            char *expanded = expand_line_feeds(char_value, char_length,
                                               &expanded_length);
            if (!expanded) {
                free(bytea_hex);
                diagnostics_add_record(&statement->diagnostics, "HY001", 0,
                                       "Out of memory expanding line feeds for SQL_C_CHAR.");
                return SQL_ERROR;
            }
            free(bytea_hex);
            bytea_hex = expanded;
            char_value = expanded;
            char_length = expanded_length;
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
        /* The original driver stores pg_atoi(text) — i.e. (int)strtol(text,10)
         * — directly into a one-byte cell, keeping only the low 8 bits with no
         * range clamp. So "1234567890" (int8) yields 210 (1234567890 mod 256),
         * "12345" yields 57, and non-numeric text yields 0. Booleans were
         * already normalized to "1"/"0" above, so "true" becomes 1. We
         * replicate the low-byte truncation exactly rather than clamping to
         * 0/1, because the regression test pins these wrapped values. */
        unsigned char value = (unsigned char)(int)strtol(effective_value, NULL, 10);
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

        /* Strip the trailing timezone offset from timestamptz text, as in the
         * SQL_C_CHAR case above. */
        if (postgres_oid == PG_TYPE_TIMESTAMPTZ) {
            wchar_source_length =
                timestamptz_text_length_without_zone(wchar_source, wchar_source_length);
        }

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

        /* LFConversion: expand bare "\n" to "\r\n" on the UTF-8 source before it
         * is transcoded, so the reported byte length and any truncation reflect
         * the expanded text. bytea hex has no line feeds, so skip it. The
         * expanded buffer reuses the bytea_wchar_hex ownership slot, which is
         * freed just below after the UTF-16 conversion. */
        if (postgres_oid != PG_TYPE_BYTEA &&
            statement_wants_line_feed_conversion(statement)) {
            int expanded_length = 0;
            char *expanded = expand_line_feeds(wchar_source, wchar_source_length,
                                               &expanded_length);
            if (!expanded) {
                free(bytea_wchar_hex);
                diagnostics_add_record(&statement->diagnostics, "HY001", 0,
                                       "Out of memory expanding line feeds for SQL_C_WCHAR.");
                return SQL_ERROR;
            }
            free(bytea_wchar_hex);
            bytea_wchar_hex = expanded;
            wchar_source = expanded;
            wchar_source_length = expanded_length;
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

        /* Write the UTF-16 null terminator, but never past the buffer. When the
         * buffer has room for fewer than sizeof(SQLWCHAR) trailing bytes (only
         * possible with a 1-byte buffer, since max_units reserves a full unit
         * whenever buffer_length >= 2), write just the low byte(s) that fit.
         * The original test pins this: an empty string into a 1-byte buffer must
         * leave a single 0x00 at offset 0 without clobbering the next byte. */
        SQLLEN terminator_offset_bytes = copy_units * (SQLLEN)sizeof(SQLWCHAR);
        SQLLEN terminator_bytes_available = buffer_length - terminator_offset_bytes;
        SQLLEN terminator_bytes_to_write =
            (terminator_bytes_available < (SQLLEN)sizeof(SQLWCHAR))
                ? terminator_bytes_available
                : (SQLLEN)sizeof(SQLWCHAR);
        if (terminator_bytes_to_write > 0) {
            memset((char *)target_value + terminator_offset_bytes, 0,
                   (size_t)terminator_bytes_to_write);
        }

        /* A terminator that could not be written in full means the data did not
         * fit, i.e. truncation. */
        bool truncated = (copy_units < (SQLLEN)unit_count) ||
                         terminator_bytes_to_write < (SQLLEN)sizeof(SQLWCHAR);
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

    /* SQL_C_VARBOOKMARK shares SQL_C_BINARY's numeric value, so it lands here
     * too and is handled identically — that is what the test exercises. */
    case SQL_C_BINARY: {
        /* Only a few source types have a meaningful binary form; every other
         * type reports 07006 (matching the original driver's SQL_C_BINARY
         * switch, which special-cases int4 and uuid, hex/text-passes an
         * allow-list of character types, and rejects the rest):
         *   - int4   -> the value as a 4-byte native (little-endian) integer
         *   - uuid   -> the 16-byte packed SQLGUID
         *   - bytea  -> the raw decoded bytes
         *   - character-like allow-list -> the raw text bytes verbatim
         *     (the test prints these as hex, e.g. text "textdata" -> 7465...). */
        if (postgres_oid == PG_TYPE_INT4) {
            SQLINTEGER integer_value = (SQLINTEGER)strtol(effective_value, NULL, 10);
            if (indicator_or_length) {
                *indicator_or_length = (SQLLEN)sizeof(integer_value);
            }
            if (!target_value || buffer_length <= 0) {
                return SQL_SUCCESS;
            }
            if (buffer_length >= (SQLLEN)sizeof(integer_value)) {
                memcpy(target_value, &integer_value, sizeof(integer_value));
                return SQL_SUCCESS;
            }
            memcpy(target_value, &integer_value, (size_t)buffer_length);
            diagnostics_add_record(&statement->diagnostics, "01004", 0,
                                   "Binary data was truncated in SQLGetData.");
            return SQL_SUCCESS_WITH_INFO;
        }

        if (postgres_oid == PG_TYPE_UUID) {
            SQLGUID guid;
            memset(&guid, 0, sizeof(guid));
            if (!parse_uuid_text(effective_value, &guid)) {
                diagnostics_add_record(&statement->diagnostics, "07006", 0,
                                       "Received an unsupported type from Postgres.");
                return SQL_ERROR;
            }
            if (indicator_or_length) {
                *indicator_or_length = (SQLLEN)sizeof(guid);
            }
            if (!target_value || buffer_length <= 0) {
                return SQL_SUCCESS;
            }
            if (buffer_length >= (SQLLEN)sizeof(guid)) {
                memcpy(target_value, &guid, sizeof(guid));
                return SQL_SUCCESS;
            }
            memcpy(target_value, &guid, (size_t)buffer_length);
            diagnostics_add_record(&statement->diagnostics, "01004", 0,
                                   "Binary data was truncated in SQLGetData.");
            return SQL_SUCCESS_WITH_INFO;
        }

        if (postgres_oid != PG_TYPE_BYTEA &&
            !field_type_allows_text_binary(postgres_oid)) {
            diagnostics_add_record(&statement->diagnostics, "07006", 0,
                                   "Received an unsupported type from Postgres.");
            return SQL_ERROR;
        }

        /* bytea decodes to raw bytes; the character allow-list passes its text
         * bytes through unchanged. */
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
        /* Only genuine date/time field types carry a date to extract; for any
         * other source type the components stay zero (matching the original
         * driver, which populates its time struct only for those types). A zero
         * date (empty value, or a non-date source) is then filled with today,
         * as the original does for SQL_C_DATE/SQL_C_TIMESTAMP. */
        ParsedDateTime parsed;
        memset(&parsed, 0, sizeof(parsed));
        if (field_type_is_datetime(postgres_oid)) {
            parse_datetime_text(effective_value, &parsed);
        }
        fill_missing_date_with_today(&parsed);
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
        /* Time targets never substitute the current time: a non-time source
         * yields 00:00:00. Only real date/time field types are parsed. */
        ParsedDateTime parsed;
        memset(&parsed, 0, sizeof(parsed));
        if (field_type_is_datetime(postgres_oid)) {
            parse_datetime_text(effective_value, &parsed);
        }
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
        /* Like SQL_C_DATE: parse only real date/time types, then fill a missing
         * date with today (the time portion stays whatever was parsed). */
        ParsedDateTime parsed;
        memset(&parsed, 0, sizeof(parsed));
        if (field_type_is_datetime(postgres_oid)) {
            parse_datetime_text(effective_value, &parsed);
        }
        fill_missing_date_with_today(&parsed);
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

    case SQL_C_GUID: {
        /* The original driver runs char2guid() on the text of any source type
         * and reports 07006 when it is not a well-formed UUID. So a UUID-shaped
         * string (whatever its column type) converts, and everything else — a
         * money amount, a timestamp, etc. — fails as an unsupported type. */
        SQLGUID guid;
        memset(&guid, 0, sizeof(guid));
        if (!parse_uuid_text(effective_value, &guid)) {
            diagnostics_add_record(&statement->diagnostics, "07006", 0,
                                   "Received an unsupported type from Postgres.");
            return SQL_ERROR;
        }
        if (target_value) {
            memcpy(target_value, &guid, sizeof(guid));
        }
        if (indicator_or_length) {
            *indicator_or_length = (SQLLEN)sizeof(guid);
        }
        return SQL_SUCCESS;
    }

    default:
        /* Types we do not convert (e.g. SQL_C_INTERVAL_DAY_TO_MINUTE) report
         * 07006, matching the original driver's COPY_UNSUPPORTED_TYPE
         * ("Received an unsupported type"). */
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

    /* Use the PUBLIC width so a bound column cannot address the hidden ctid. */
    int total_columns = statement_public_column_count(statement);
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

        /* Read through the keyset overlay so an updated row shows its new value. */
        bool value_is_null = false;
        const char *raw_value = statement_row_value(statement, source_row,
                                                    column_index, &value_is_null);

        /* Handle NULL values: set indicator and skip buffer write */
        if (value_is_null) {
            /* CvtNullDate: a NULL date/timestamp bound as char/date is delivered
             * as an empty string (indicator 0), not SQL_NULL_DATA. */
            unsigned int null_col_oid =
                (unsigned int)PQftype(statement->current_result, column_index);
            if (statement_null_date_reads_as_empty(statement, null_col_oid,
                                                    resolved_type)) {
                write_null_date_empty_string(resolved_type, element_buffer,
                                             binding->buffer_length,
                                             element_indicator);
            } else if (element_indicator) {
                *element_indicator = SQL_NULL_DATA;
            }
            continue;
        }

        int raw_value_length = (int)strlen(raw_value);

        unsigned int col_oid = (unsigned int)PQftype(statement->current_result, column_index);

        /* Large-object columns hold a large object's Oid, not the bytes. When a
         * "lo" column is bound as SQL_C_BINARY, stream the object's contents into
         * the bound buffer instead of copying the Oid text — the same dispatch
         * results_get_data performs for SQLGetData (see there for rationale). */
        SQLRETURN conversion_result;
        if (resolved_type == SQL_C_BINARY &&
            connection_type_is_large_object(statement->parent_connection, col_oid)) {
            conversion_result = statement_get_large_object_data(
                statement, column_index, source_row, raw_value,
                element_buffer, binding->buffer_length, element_indicator);
        } else {
            /* Per-column ARD SQL_DESC_PRECISION override (interval fractional
             * secs). column_index is 0-based, matching the override array. */
            int fraction_precision = (column_index < MAX_BOUND_COLUMNS)
                                         ? statement->column_precision_override[column_index]
                                         : -1;
            conversion_result = convert_value_to_c_type(
                statement, raw_value, raw_value_length,
                resolved_type, element_buffer,
                binding->buffer_length, element_indicator,
                col_oid, fraction_precision);
        }

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
 *
 * LIMITATION (Phase 1): a bound column-0 bookmark is NOT populated on the
 * block-cursor path. bookmark_buffer is a single scalar (not an array indexed by
 * rowset element), so per-row bookmarks for a block cursor are out of scope. The
 * bookmark regression test uses single-row fetches, which are handled in
 * populate_bound_columns.
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
 * Write the bookmark for a 0-based cursor row into an application buffer as a
 * 4-byte Int4 (see statement_make_int4_bookmark). Used both by SQLGetData on
 * column 0 and by the bound-bookmark path during a fetch. Both SQL_C_BOOKMARK
 * and SQL_C_VARBOOKMARK carry the same 4-byte value; the indicator always
 * receives sizeof(Int4Bookmark).
 *
 * target_type selects the buffer-length policy:
 *   - SQL_C_BOOKMARK is a fixed 4-byte type; the application always supplies a
 *     buffer of at least that size, so (matching the original driver) we write
 *     unconditionally without consulting buffer_length.
 *   - SQL_C_VARBOOKMARK is a variable-length (binary) type; if the supplied
 *     buffer cannot hold the full 4 bytes we must NOT overrun it. In that case
 *     we skip the copy, still report the full length via the indicator, and
 *     signal truncation (01004 / SQL_SUCCESS_WITH_INFO).
 *
 * Returns SQL_SUCCESS, or SQL_SUCCESS_WITH_INFO when a VARBOOKMARK buffer was
 * too small to receive the value.
 */
static SQLRETURN write_row_bookmark(OdbcStatement *statement,
                                    int zero_based_row,
                                    SQLSMALLINT target_type,
                                    SQLPOINTER target_value,
                                    SQLLEN buffer_length,
                                    SQLLEN *indicator_or_length)
{
    Int4Bookmark bookmark = statement_make_int4_bookmark(zero_based_row);

    /* The indicator always reports the full bookmark length, even on truncation. */
    if (indicator_or_length) {
        *indicator_or_length = (SQLLEN)sizeof(bookmark);
    }

    if (!target_value) {
        return SQL_SUCCESS;
    }

    /* Variable-length bookmark: guard against a buffer too small for the value. */
    if (target_type == SQL_C_VARBOOKMARK &&
        buffer_length < (SQLLEN)sizeof(bookmark)) {
        diagnostics_add_record(&statement->diagnostics,
                               "01004",  /* String data, right truncated */
                               0,
                               "Bookmark buffer too small to receive the bookmark value.");
        return SQL_SUCCESS_WITH_INFO;
    }

    memcpy(target_value, &bookmark, sizeof(bookmark));
    return SQL_SUCCESS;
}

/*
 * Single-row convenience wrapper used by plain SQLFetch and the single-row
 * scrolling path: writes the current row into element 0 of the bound buffers.
 * When a column-0 bookmark binding is active, the current row's bookmark is also
 * written into the bound bookmark buffer so an application that binds column 0
 * (rather than calling SQLGetData) receives it during the fetch.
 */
static SQLRETURN populate_bound_columns(OdbcStatement *statement)
{
    /* Gate the bound-bookmark write on bookmarks being enabled, consistent with
     * the SQLGetData column-0 guard: a column-0 binding is only meaningful when
     * the application opted into bookmarks via SQL_ATTR_USE_BOOKMARKS. */
    if (statement->bookmark_bound && statement->use_bookmarks != SQL_UB_OFF) {
        write_row_bookmark(statement, statement->current_row_position,
                           statement->bookmark_target_type,
                           statement->bookmark_buffer,
                           statement->bookmark_buffer_length,
                           statement->bookmark_indicator);
    }
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
            /* Report the PUBLIC column count so the hidden ctid appended for an
             * updatable cursor never shows up in SQLNumResultCols. */
            *column_count = (SQLSMALLINT)statement_public_column_count(statement);
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

    /* Hide the trailing ctid column of an updatable cursor from SQLDescribeCol. */
    if (statement->hidden_ctid_column_index != NO_HIDDEN_CTID_COLUMN &&
        metadata_source == statement->current_result) {
        total_columns = statement_public_column_count(statement);
    }

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

/*
 * True when this fetch must go through the keyset block-cursor path: an
 * updatable cursor with a live overlay and a block (row-array) size > 1. Such a
 * cursor skips overlay-deleted rows during movement and supports backward block
 * fetches (SQL_FETCH_PRIOR), which the plain static path does not.
 */
static bool keyset_block_fetch_active(const OdbcStatement *statement)
{
    return statement->is_updatable_cursor && statement->keyset_rows &&
           statement->hidden_ctid_column_index != NO_HIDDEN_CTID_COLUMN &&
           statement->row_array_size > 1;
}

/*
 * Block fetch over the keyset overlay for an updatable cursor: gather up to
 * row_array_size non-deleted base rows in the requested direction, copy them
 * into the bound arrays, and record the rowset extent so SQLSetPos can target a
 * row within it. Handles SQL_FETCH_FIRST/NEXT/LAST/PRIOR (the orientations the
 * block-delete cursor uses). Deleted rows are skipped; rows added by SQL_ADD
 * (beyond the base result) are included as live rows.
 *
 * Cursor model (matches the plain path): current_row_position is the last row of
 * the delivered rowset; -1 = BOF, keyset_row_count = EOF. keyset_rowset_first_row
 * is the first (lowest-index) row of the rowset, which SQL_DELETE targets.
 */
static SQLRETURN keyset_block_fetch(OdbcStatement *statement,
                                    SQLUSMALLINT fetch_orientation,
                                    SQLULEN *fetched_row_count,
                                    SQLUSMALLINT *row_status_array)
{
    int total_rows = statement->keyset_row_count;
    SQLULEN rowset_size = statement->row_array_size;

    /* Collect base-row indices for this rowset, always stored in ascending
     * (presentation) order. */
    int collected[MAX_BOUND_COLUMNS];
    if (rowset_size > MAX_BOUND_COLUMNS) {
        rowset_size = MAX_BOUND_COLUMNS;
    }
    int collected_count = 0;

    bool scan_backward = (fetch_orientation == SQL_FETCH_PRIOR ||
                          fetch_orientation == SQL_FETCH_LAST);

    int scan_index;
    switch (fetch_orientation) {
    case SQL_FETCH_FIRST:
        scan_index = 0;
        break;
    case SQL_FETCH_NEXT:
        scan_index = (statement->current_row_position < 0)
                         ? 0
                         : statement->current_row_position + 1;
        break;
    case SQL_FETCH_LAST:
        scan_index = total_rows - 1;
        break;
    case SQL_FETCH_PRIOR:
        /* Anchor before the current rowset's first row; from EOF (or with no
         * established rowset) start at the very end. */
        if (statement->keyset_rowset_first_row < 0 ||
            statement->current_row_position >= total_rows) {
            scan_index = total_rows - 1;
        } else {
            scan_index = statement->keyset_rowset_first_row - 1;
        }
        break;
    default:
        diagnostics_add_record(&statement->diagnostics, "HY106", 0,
                               "Unsupported fetch orientation for keyset cursor.");
        return SQL_ERROR;
    }

    /* Walk in the scan direction, skipping deleted rows, until the rowset is
     * full or we run off an end. */
    while (collected_count < (int)rowset_size &&
           scan_index >= 0 && scan_index < total_rows) {
        if (!statement->keyset_rows[scan_index].deleted) {
            collected[collected_count++] = scan_index;
        }
        scan_index += scan_backward ? -1 : 1;
    }

    /* No live rows in this direction: park at the appropriate boundary. */
    if (collected_count == 0) {
        if (scan_backward) {
            statement->current_row_position = -1;  /* BOF */
            statement->keyset_rowset_first_row = -1;
        } else {
            statement->current_row_position = total_rows;  /* EOF */
        }
        statement->keyset_rowset_size = 0;
        if (fetched_row_count) {
            *fetched_row_count = 0;
        }
        if (statement->rows_fetched_ptr) {
            *statement->rows_fetched_ptr = 0;
        }
        return SQL_NO_DATA;
    }

    /* Backward scans collected in descending order; flip to ascending so the
     * rowset is presented (and its first row targeted) consistently. */
    if (scan_backward) {
        for (int i = 0, j = collected_count - 1; i < j; i++, j--) {
            int temp = collected[i];
            collected[i] = collected[j];
            collected[j] = temp;
        }
    }

    /* Copy each collected row into its rowset element and set per-row status. */
    SQLRETURN overall_result = SQL_SUCCESS;
    for (int element = 0; element < collected_count; element++) {
        SQLRETURN row_result =
            populate_bound_columns_row(statement, collected[element],
                                       (SQLULEN)element);
        if (row_result == SQL_SUCCESS_WITH_INFO) {
            overall_result = SQL_SUCCESS_WITH_INFO;
        }
        if (statement->row_status_ptr) {
            statement->row_status_ptr[element] = SQL_ROW_SUCCESS;
        }
        if (row_status_array) {
            row_status_array[element] = SQL_ROW_SUCCESS;
        }
    }
    if (statement->row_status_ptr) {
        for (int element = collected_count; element < (int)statement->row_array_size;
             element++) {
            statement->row_status_ptr[element] = SQL_ROW_NOROW;
        }
    }

    statement->keyset_rowset_first_row = collected[0];
    statement->keyset_rowset_size = collected_count;
    statement->current_row_position = collected[collected_count - 1];

    if (statement->rows_fetched_ptr) {
        *statement->rows_fetched_ptr = (SQLULEN)collected_count;
    }
    if (fetched_row_count) {
        *fetched_row_count = (SQLULEN)collected_count;
    }
    return overall_result;
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

    /* Updatable block cursor: skip overlay-deleted rows and track the rowset
     * extent so SQLSetPos can target rows within it. A plain SQLFetch advances
     * the cursor forward, i.e. SQL_FETCH_NEXT. */
    if (keyset_block_fetch_active(statement)) {
        return keyset_block_fetch(statement, SQL_FETCH_NEXT, NULL, NULL);
    }

    int total_rows = PQntuples(statement->current_result);

    /* Moving to a new row invalidates any in-progress large-object read from the
     * previous row, so close it before advancing the cursor. */
    statement_reset_large_object_read(statement);

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

    /* Updatable block cursor: delegate to the overlay-aware block fetch, which
     * skips deleted rows and records the rowset extent for SQLSetPos. It handles
     * the orientations the block-delete cursor uses (FIRST/NEXT/LAST/PRIOR). */
    if (keyset_block_fetch_active(statement)) {
        return keyset_block_fetch(statement, fetch_orientation,
                                  fetched_row_count, row_status_array);
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

    case SQL_FETCH_BOOKMARK: {
        /* Position relative to the bookmark the application stored via
         * SQL_ATTR_FETCH_BOOKMARK_PTR. The bookmark is a 4-byte Int4 encoding a
         * 1-based row; resolve it to a 0-based base row, then move fetch_offset
         * rows. This is a static snapshot with no deleted rows, so "walk N valid
         * rows" is simply base + offset. */
        /* Bookmarked fetching requires the application to have enabled bookmarks
         * (consistent with the SQLGetData column-0 guard). */
        if (statement->use_bookmarks == SQL_UB_OFF) {
            diagnostics_add_record(&statement->diagnostics,
                                   "HY106",  /* Fetch type out of range */
                                   0,
                                   "SQL_FETCH_BOOKMARK requires SQL_ATTR_USE_BOOKMARKS "
                                   "to be enabled.");
            return SQL_ERROR;
        }
        if (!statement->fetch_bookmark_ptr) {
            diagnostics_add_record(&statement->diagnostics,
                                   "HY090",  /* Invalid string or buffer length */
                                   0,
                                   "SQL_FETCH_BOOKMARK requested but "
                                   "SQL_ATTR_FETCH_BOOKMARK_PTR is not set.");
            return SQL_ERROR;
        }
        Int4Bookmark stored_bookmark;
        memcpy(&stored_bookmark, statement->fetch_bookmark_ptr, sizeof(stored_bookmark));
        int base_row = statement_resolve_int4_bookmark(stored_bookmark);
        target_index = (SQLLEN)base_row + fetch_offset;
        break;
    }

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

    /* Column 0 is the bookmark column. It is not a real result column, so it
     * must be handled before the 1-based range check below. Reading it requires
     * bookmarks to have been enabled via SQL_ATTR_USE_BOOKMARKS; otherwise the
     * column does not exist for this cursor. The value is the current row's
     * 4-byte Int4 bookmark (both SQL_C_BOOKMARK and SQL_C_VARBOOKMARK). */
    if (column_number == 0) {
        if (statement->use_bookmarks == SQL_UB_OFF) {
            diagnostics_add_record(&statement->diagnostics,
                                   "07009",  /* Invalid descriptor index */
                                   0,
                                   "Bookmarks are not enabled (SQL_ATTR_USE_BOOKMARKS is off).");
            return SQL_ERROR;
        }
        if (target_type != SQL_C_BOOKMARK && target_type != SQL_C_VARBOOKMARK) {
            diagnostics_add_record(&statement->diagnostics,
                                   "07006",  /* Restricted data type attribute violation */
                                   0,
                                   "Column 0 can only be retrieved as a bookmark type.");
            return SQL_ERROR;
        }
        return write_row_bookmark(statement, statement->current_row_position,
                                  target_type, target_value,
                                  buffer_length, indicator_or_length);
    }

    /* Validate column number (1-based) against the PUBLIC count so an updatable
     * cursor's hidden ctid column cannot be read by the application. */
    int total_columns = statement_public_column_count(statement);
    if (column_number < 1 || column_number > (SQLUSMALLINT)total_columns) {
        diagnostics_add_record(&statement->diagnostics,
                               "07009",  /* Invalid descriptor index */
                               0,
                               "Column number is out of range.");
        return SQL_ERROR;
    }

    int column_index = (int)(column_number - 1);
    int row_index = statement->current_row_position;

    /* Read through the keyset overlay so a positioned UPDATE/REFRESH shows its
     * new value on re-fetch; falls back to the base result for normal rows. */
    bool value_is_null = false;
    const char *raw_value = statement_row_value(statement, row_index,
                                                column_index, &value_is_null);

    /* Check for NULL */
    if (value_is_null) {
        /* CvtNullDate: a NULL date/timestamp read into a char/date target comes
         * back as an empty string (indicator 0), not SQL_NULL_DATA. */
        unsigned int null_col_oid =
            (unsigned int)PQftype(statement->current_result, column_index);
        if (statement_null_date_reads_as_empty(statement, null_col_oid, target_type)) {
            return write_null_date_empty_string(target_type, target_value,
                                                buffer_length, indicator_or_length);
        }
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

    int raw_value_length = (int)strlen(raw_value);

    /* Resolve SQL_C_DEFAULT to the actual default C type for this column */
    SQLSMALLINT resolved_type = target_type;
    if (target_type == SQL_C_DEFAULT) {
        unsigned int postgres_oid = (unsigned int)PQftype(statement->current_result, column_index);
        SQLSMALLINT sql_type = type_mapping_get_sql_type(postgres_oid);
        resolved_type = type_mapping_get_default_c_type(sql_type);
    }

    unsigned int col_oid = (unsigned int)PQftype(statement->current_result, column_index);

    /* Large-object columns store the Oid of a large object, not the bytes
     * themselves. When the application reads such a column as binary, stream the
     * object's contents back (chunked) rather than returning the Oid text. This
     * mirrors the original driver's convert_lo path. Other target types fall
     * through to the normal conversion, which returns the Oid as text. */
    if (resolved_type == SQL_C_BINARY &&
        connection_type_is_large_object(statement->parent_connection, col_oid)) {
        return statement_get_large_object_data(statement, column_index,
                                               statement->current_row_position,
                                               raw_value,
                                               target_value, buffer_length,
                                               indicator_or_length);
    }

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

/* ==================================================================
 * SQLSetPos — positioned UPDATE / DELETE / REFRESH / ADD
 *
 * Backed by the hidden-ctid keyset (see statement.c). SQLSetPos identifies a
 * row within the CURRENT rowset (row_number is 1-based within it), maps it to a
 * base row of the buffered result, reads that row's captured ctid, and issues a
 * searched UPDATE/DELETE keyed on the ctid. Because libpq's PGresult is
 * immutable, the new/updated values are stored in the per-row overlay so a
 * re-fetch of the still-open cursor reflects them.
 * ================================================================== */

/*
 * Read the ctid captured in the hidden trailing column of a base row. Returns a
 * pointer into the PGresult (valid until the result is cleared), or NULL if the
 * row has no ctid (e.g. an SQL_ADD row not yet flushed, or a non-updatable
 * cursor). The ctid text looks like "(block,offset)".
 */
static const char *keyset_row_ctid(const OdbcStatement *statement, int base_row)
{
    if (statement->hidden_ctid_column_index == NO_HIDDEN_CTID_COLUMN ||
        !statement->current_result) {
        return NULL;
    }
    if (base_row < 0 || base_row >= PQntuples(statement->current_result)) {
        return NULL;  /* Rows added by SQL_ADD have no base ctid column. */
    }
    /* An UPDATE moves the row to a new ctid, which "RETURNING *, ctid" captured
     * into the overlay. Prefer the overlay's ctid so a SECOND positioned update
     * on the same row keys on where the row actually lives now, not the stale
     * ctid frozen in the immutable base PGresult. */
    const KeysetRow *row = &statement->keyset_rows[base_row];
    if (row->override_values &&
        row->override_values[statement->hidden_ctid_column_index]) {
        return row->override_values[statement->hidden_ctid_column_index];
    }
    if (PQgetisnull(statement->current_result, base_row,
                    statement->hidden_ctid_column_index)) {
        return NULL;
    }
    return PQgetvalue(statement->current_result, base_row,
                      statement->hidden_ctid_column_index);
}

/*
 * Convert a bound column's application buffer to its PostgreSQL text
 * representation for use as a positioned-UPDATE/INSERT value. Reuses the
 * parameter converter by projecting the ColumnBinding onto a ParameterBinding.
 * Returns a heap string (caller frees) and sets *is_null, or NULL for SQL NULL.
 */
static char *keyset_bound_column_to_text(const ColumnBinding *binding,
                                         bool *is_null)
{
    ParameterBinding projected = {0};
    projected.c_type = binding->target_type;
    /* No SQL-type hint: send as text and let PostgreSQL coerce to the column
     * type. This keeps the converter on its plain text path for integers. */
    projected.sql_type = SQL_UNKNOWN_TYPE;
    projected.value_buffer = binding->target_buffer;
    projected.buffer_length = binding->buffer_length;
    projected.indicator_or_length = binding->indicator_or_length;

    if (binding->indicator_or_length &&
        *binding->indicator_or_length == SQL_NULL_DATA) {
        *is_null = true;
        return NULL;
    }

    int text_length = 0;
    char *text = convert_parameter_to_text(&projected, &text_length);
    *is_null = (text == NULL);
    return text;
}

/*
 * Safely quote a SQL identifier (column name) for interpolation into DML text.
 * Column names come from PQfname, which for an updatable single-table cursor are
 * ordinary identifiers, but a name could legally contain a double quote; naive
 * "%s" wrapping would let that break out of the quotes. PQescapeIdentifier both
 * quotes and doubles any embedded quote, so it is injection-safe. Returns a
 * libpq-allocated string the caller frees with PQfreemem, or NULL on failure.
 */
static char *keyset_quote_identifier(PGconn *connection, const char *identifier)
{
    return PQescapeIdentifier(connection, identifier, strlen(identifier));
}

/*
 * Replace a base row's overlay with the values from one row of a PGresult that
 * has the SAME column layout as the cursor (all columns followed by the hidden
 * ctid), e.g. the output of "UPDATE ... RETURNING *, ctid". This captures the
 * NEW ctid (an UPDATE moves the row) and the new column values so a re-fetch of
 * the still-open cursor reflects them. Returns SQL_SUCCESS or SQL_ERROR (OOM).
 */
static SQLRETURN keyset_store_row_overlay(OdbcStatement *statement,
                                          int base_row,
                                          PGresult *source, int source_row)
{
    int full_column_count = PQnfields(statement->current_result);
    KeysetRow *row = &statement->keyset_rows[base_row];

    if (row->override_values) {
        for (int column = 0; column < full_column_count; column++) {
            free(row->override_values[column]);
        }
        free(row->override_values);
        row->override_values = NULL;
    }
    row->override_values = calloc((size_t)full_column_count, sizeof(char *));
    if (!row->override_values) {
        return SQL_ERROR;
    }

    int source_columns = PQnfields(source);
    for (int column = 0; column < full_column_count && column < source_columns;
         column++) {
        if (PQgetisnull(source, source_row, column)) {
            row->override_values[column] = NULL;  /* SQL NULL */
            continue;
        }
        row->override_values[column] =
            pg_strdup(PQgetvalue(source, source_row, column));
    }
    return SQL_SUCCESS;
}

/*
 * Build and run a positioned UPDATE for one base row: set every bound public
 * column (indicator != SQL_IGNORE) to its current buffer value, keyed on the
 * row's captured ctid. Bound VALUES go through PQexecParams (injection-safe);
 * the ctid is self-captured and safe to inline. "RETURNING *, ctid" gives back
 * the post-update row (with its new ctid), which is stored in the overlay so a
 * re-fetch shows the new values.
 */
static SQLRETURN keyset_positioned_update(OdbcStatement *statement, int base_row)
{
    const char *ctid = keyset_row_ctid(statement, base_row);
    if (!ctid) {
        diagnostics_add_record(&statement->diagnostics, "HY000", 0,
                               "Row has no ctid; cannot perform positioned update.");
        return SQL_ERROR;
    }

    PGconn *connection = statement->parent_connection->libpq_connection;

    /* Assemble the "SET col=$n, ..." list from bound public columns. Values are
     * bound as PQexecParams parameters ($1..$n) to avoid SQL injection. */
    char set_clause[2048];
    size_t set_length = 0;
    const char *param_values[MAX_BOUND_COLUMNS];
    char *owned_param_values[MAX_BOUND_COLUMNS];
    int param_count = 0;
    int public_columns = statement_public_column_count(statement);

    for (int slot = 0; slot < MAX_BOUND_COLUMNS; slot++) {
        ColumnBinding *binding = &statement->column_bindings[slot];
        if (!binding->is_bound) {
            continue;
        }
        int column_index = (int)(binding->column_number - 1);
        if (column_index < 0 || column_index >= public_columns) {
            continue;  /* Skip bindings outside the visible result. */
        }
        /* SQL_IGNORE in the indicator means "do not update this column". */
        if (binding->indicator_or_length &&
            *binding->indicator_or_length == SQL_IGNORE) {
            continue;
        }

        bool is_null = false;
        char *text = keyset_bound_column_to_text(binding, &is_null);
        const char *raw_column_name = PQfname(statement->current_result, column_index);
        char *quoted_column_name = keyset_quote_identifier(connection, raw_column_name);
        if (!quoted_column_name) {
            free(text);
            for (int i = 0; i < param_count; i++) {
                free(owned_param_values[i]);
            }
            diagnostics_add_record(&statement->diagnostics, "HY001", 0,
                                   "Out of memory quoting column name for UPDATE.");
            return SQL_ERROR;
        }
        const char *separator = (set_length > 0) ? ", " : "";

        int written;
        if (is_null) {
            written = snprintf(set_clause + set_length,
                               sizeof(set_clause) - set_length,
                               "%s%s = NULL", separator, quoted_column_name);
        } else {
            owned_param_values[param_count] = text;
            param_values[param_count] = text;
            written = snprintf(set_clause + set_length,
                               sizeof(set_clause) - set_length,
                               "%s%s = $%d", separator, quoted_column_name,
                               param_count + 1);
            param_count++;
        }
        PQfreemem(quoted_column_name);
        if (written < 0 || (size_t)written >= sizeof(set_clause) - set_length) {
            for (int i = 0; i < param_count; i++) {
                free(owned_param_values[i]);
            }
            diagnostics_add_record(&statement->diagnostics, "HY000", 0,
                                   "Positioned UPDATE SET clause too large.");
            return SQL_ERROR;
        }
        set_length += (size_t)written;
    }

    if (set_length == 0) {
        return SQL_SUCCESS;  /* No updatable bound columns: no-op. */
    }

    char query[3072];
    snprintf(query, sizeof(query),
             "UPDATE %s SET %s WHERE ctid = '%s' RETURNING *, ctid",
             statement->keyset_table_name, set_clause, ctid);

    PGresult *update_result = PQexecParams(connection, query, param_count, NULL,
                                           param_values, NULL, NULL, 0);
    for (int i = 0; i < param_count; i++) {
        free(owned_param_values[i]);
    }

    if (!update_result || PQresultStatus(update_result) != PGRES_TUPLES_OK) {
        if (update_result) {
            error_add_diagnostic_from_result(&statement->diagnostics,
                                             update_result, "HY000");
            PQclear(update_result);
        }
        return SQL_ERROR;
    }

    SQLRETURN store_result = SQL_SUCCESS;
    if (PQntuples(update_result) >= 1) {
        store_result = keyset_store_row_overlay(statement, base_row,
                                                update_result, 0);
    }
    PQclear(update_result);
    return store_result;
}

/*
 * Positioned DELETE for one base row, keyed on its captured ctid. Marks the
 * overlay row deleted so the still-open cursor skips it on subsequent fetches
 * (the immutable PGresult cannot be shrunk). Returns SQL_SUCCESS or SQL_ERROR.
 */
static SQLRETURN keyset_positioned_delete(OdbcStatement *statement, int base_row)
{
    const char *ctid = keyset_row_ctid(statement, base_row);
    if (!ctid) {
        /* A row added via SQL_ADD (no base ctid) is deleted purely in the
         * overlay — it was never persisted with a ctid we track. */
        if (base_row >= 0 && base_row < statement->keyset_row_count) {
            statement->keyset_rows[base_row].deleted = true;
            return SQL_SUCCESS;
        }
        diagnostics_add_record(&statement->diagnostics, "HY000", 0,
                               "Row has no ctid; cannot perform positioned delete.");
        return SQL_ERROR;
    }

    PGconn *connection = statement->parent_connection->libpq_connection;
    char query[512];
    snprintf(query, sizeof(query), "DELETE FROM %s WHERE ctid = '%s'",
             statement->keyset_table_name, ctid);

    PGresult *delete_result = PQexec(connection, query);
    if (!delete_result || PQresultStatus(delete_result) != PGRES_COMMAND_OK) {
        if (delete_result) {
            error_add_diagnostic_from_result(&statement->diagnostics,
                                             delete_result, "HY000");
            PQclear(delete_result);
        }
        return SQL_ERROR;
    }
    PQclear(delete_result);

    statement->keyset_rows[base_row].deleted = true;
    return SQL_SUCCESS;
}

/*
 * Positioned REFRESH for one base row: re-copy its current values into the bound
 * buffers (element 0). No server round-trip — the overlay already holds the
 * latest values from any prior UPDATE. Returns the copy result.
 */
static SQLRETURN keyset_positioned_refresh(OdbcStatement *statement, int base_row)
{
    int saved_position = statement->current_row_position;
    statement->current_row_position = base_row;
    SQLRETURN result = populate_bound_columns(statement);
    statement->current_row_position = saved_position;
    return result;
}

/*
 * Positioned ADD (bulk insert): INSERT one row per rowset element from the bound
 * column buffers into the cursor's table, then append a live overlay row per
 * inserted row so the still-open cursor and delete bookkeeping account for it.
 * The block-delete test uses irow==0 (whole rowset) ADD with row_array_size
 * rows staged in the bound arrays. Returns SQL_SUCCESS or SQL_ERROR.
 *
 * capture_inserted_row controls two behaviors needed only by the
 * SQLBulkOperations SQL_ADD path (SQLSetPos SQL_ADD passes false so its behavior
 * is unchanged):
 *   - the INSERT uses "RETURNING *, ctid" and the returned row is stored in the
 *     new overlay entry, so the added row carries real values (and a ctid) and a
 *     subsequent SQL_FETCH_BY_BOOKMARK can read them back;
 *   - the newly added row's bookmark is written into the bound column-0 buffer
 *     (via write_row_bookmark), matching the ODBC contract that SQL_ADD returns
 *     the new row's bookmark to the application.
 */
static SQLRETURN keyset_positioned_add(OdbcStatement *statement,
                                       bool capture_inserted_row)
{
    PGconn *connection = statement->parent_connection->libpq_connection;
    int public_columns = statement_public_column_count(statement);
    SQLULEN rowset_size = statement->row_array_size > 0 ? statement->row_array_size : 1;

    for (SQLULEN element = 0; element < rowset_size; element++) {
        char column_list[1024];
        char value_list[1024];
        size_t column_length = 0;
        size_t value_length = 0;
        const char *param_values[MAX_BOUND_COLUMNS];
        char *owned_param_values[MAX_BOUND_COLUMNS];
        int param_count = 0;

        for (int slot = 0; slot < MAX_BOUND_COLUMNS; slot++) {
            ColumnBinding *binding = &statement->column_bindings[slot];
            if (!binding->is_bound) {
                continue;
            }
            int column_index = (int)(binding->column_number - 1);
            if (column_index < 0 || column_index >= public_columns) {
                continue;
            }

            /* Project the element-th array slot onto a temporary binding so the
             * text converter reads the right rowset element. */
            size_t stride = c_type_element_stride(binding->target_type,
                                                  binding->buffer_length);
            ColumnBinding element_binding = *binding;
            if (binding->target_buffer && stride > 0) {
                element_binding.target_buffer =
                    (SQLPOINTER)((char *)binding->target_buffer + element * stride);
            }
            if (binding->indicator_or_length) {
                element_binding.indicator_or_length =
                    binding->indicator_or_length + element;
            }
            if (element_binding.indicator_or_length &&
                *element_binding.indicator_or_length == SQL_IGNORE) {
                continue;
            }

            bool is_null = false;
            char *text = keyset_bound_column_to_text(&element_binding, &is_null);
            const char *raw_column_name = PQfname(statement->current_result, column_index);
            char *quoted_column_name = keyset_quote_identifier(connection, raw_column_name);
            if (!quoted_column_name) {
                free(text);
                for (int i = 0; i < param_count; i++) {
                    free(owned_param_values[i]);
                }
                diagnostics_add_record(&statement->diagnostics, "HY001", 0,
                                       "Out of memory quoting column name for INSERT.");
                return SQL_ERROR;
            }

            /* Separators are driven by accumulated length (not param_count) so a
             * NULL column, which contributes a column name and a literal NULL but
             * NO bound parameter, still gets a comma. */
            int cwritten = snprintf(column_list + column_length,
                                    sizeof(column_list) - column_length,
                                    "%s%s", column_length > 0 ? ", " : "",
                                    quoted_column_name);
            PQfreemem(quoted_column_name);
            int vwritten;
            if (is_null) {
                /* SQL NULL: emit a literal NULL and DO NOT consume a $n slot, so
                 * placeholder numbering stays aligned with param_values[] and the
                 * free loops never touch an unassigned owned_param_values entry. */
                vwritten = snprintf(value_list + value_length,
                                    sizeof(value_list) - value_length,
                                    "%sNULL", value_length > 0 ? ", " : "");
            } else {
                owned_param_values[param_count] = text;
                param_values[param_count] = text;
                vwritten = snprintf(value_list + value_length,
                                    sizeof(value_list) - value_length,
                                    "%s$%d", value_length > 0 ? ", " : "",
                                    param_count + 1);
                param_count++;
            }
            if (cwritten < 0 || vwritten < 0 ||
                (size_t)cwritten >= sizeof(column_list) - column_length ||
                (size_t)vwritten >= sizeof(value_list) - value_length) {
                for (int i = 0; i < param_count; i++) {
                    free(owned_param_values[i]);
                }
                diagnostics_add_record(&statement->diagnostics, "HY000", 0,
                                       "Positioned INSERT column list too large.");
                return SQL_ERROR;
            }
            column_length += (size_t)cwritten;
            value_length += (size_t)vwritten;
        }

        if (column_length == 0) {
            continue;  /* No bound columns to insert for this element. */
        }

        char query[2560];
        /* When the caller wants the inserted row captured, return the full row
         * plus its ctid so the new overlay entry mirrors the layout the fetch and
         * ctid accessors expect (all public columns followed by the hidden ctid). */
        snprintf(query, sizeof(query),
                 capture_inserted_row
                     ? "INSERT INTO %s (%s) VALUES (%s) RETURNING *, ctid"
                     : "INSERT INTO %s (%s) VALUES (%s)",
                 statement->keyset_table_name, column_list, value_list);
        PGresult *insert_result = PQexecParams(connection, query, param_count,
                                               NULL, param_values, NULL, NULL, 0);
        for (int i = 0; i < param_count; i++) {
            free(owned_param_values[i]);
        }
        if (!insert_result ||
            (PQresultStatus(insert_result) != PGRES_COMMAND_OK &&
             PQresultStatus(insert_result) != PGRES_TUPLES_OK)) {
            if (insert_result) {
                error_add_diagnostic_from_result(&statement->diagnostics,
                                                 insert_result, "HY000");
                PQclear(insert_result);
            }
            return SQL_ERROR;
        }

        /* Grow the overlay by one live row so the cursor and delete counters see
         * the added row. Without capture it has no base tuple; its values live
         * only in the overlay-less region and read as NULL, which is fine for the
         * delete test (it never reads added rows' values). With capture, the
         * RETURNING row is stored below so the added row reads back its values. */
        int new_count = statement->keyset_row_count + 1;
        KeysetRow *grown = realloc(statement->keyset_rows,
                                   (size_t)new_count * sizeof(KeysetRow));
        if (!grown) {
            PQclear(insert_result);
            diagnostics_add_record(&statement->diagnostics, "HY001", 0,
                                   "Out of memory growing keyset for SQL_ADD.");
            return SQL_ERROR;
        }
        statement->keyset_rows = grown;
        int added_row_index = statement->keyset_row_count;
        statement->keyset_rows[added_row_index].deleted = false;
        statement->keyset_rows[added_row_index].override_values = NULL;
        statement->keyset_row_count = new_count;

        if (capture_inserted_row && PQntuples(insert_result) >= 1) {
            /* Store the RETURNING row as the added row's override so it reads back
             * its inserted values (and carries the ctid) on a later fetch. */
            SQLRETURN store_result =
                keyset_store_row_overlay(statement, added_row_index,
                                         insert_result, 0);
            if (store_result == SQL_ERROR) {
                PQclear(insert_result);
                return SQL_ERROR;
            }

            /* Return the new row's bookmark to the application via the bound
             * column-0 buffer, so it can later fetch the row by that bookmark. */
            if (statement->bookmark_bound &&
                statement->use_bookmarks != SQL_UB_OFF) {
                write_row_bookmark(statement, added_row_index,
                                   statement->bookmark_target_type,
                                   statement->bookmark_buffer,
                                   statement->bookmark_buffer_length,
                                   statement->bookmark_indicator);
            }
        }
        PQclear(insert_result);
    }

    return SQL_SUCCESS;
}

SQLRETURN statement_set_pos(OdbcStatement *statement,
                            SQLSETPOSIROW row_number,
                            SQLUSMALLINT operation,
                            SQLUSMALLINT lock_type)
{
    (void)lock_type;  /* Lock modes are advisory; PostgreSQL MVCC handles this. */

    if (!statement->current_result || !statement->has_result_set) {
        diagnostics_add_record(&statement->diagnostics, "24000", 0,
                               "No open cursor for SQLSetPos.");
        return SQL_ERROR;
    }
    if (!statement->is_updatable_cursor ||
        statement->hidden_ctid_column_index == NO_HIDDEN_CTID_COLUMN) {
        diagnostics_add_record(&statement->diagnostics, "HY092", 0,
                               "SQLSetPos requires an updatable (keyset) cursor.");
        return SQL_ERROR;
    }

    /* SQL_ADD inserts new rows and does not reference an existing row. The
     * SQLSetPos path does not need the inserted row captured or its bookmark
     * returned (that is a SQLBulkOperations-only concern), so pass false. */
    if (operation == SQL_ADD) {
        return keyset_positioned_add(statement, false);
    }

    /* Resolve the target base row from the 1-based row_number within the current
     * rowset. row_number == 0 means "operate on the whole rowset" (bulk); for
     * UPDATE/DELETE/REFRESH the regression tests always use row_number 1 with a
     * single-row rowset, so bulk maps to the first rowset row. */
    int rowset_first = (statement->keyset_rowset_first_row >= 0)
                           ? statement->keyset_rowset_first_row
                           : statement->current_row_position;
    SQLSETPOSIROW effective_row = (row_number == 0) ? 1 : row_number;
    int base_row = rowset_first + (int)(effective_row - 1);

    if (base_row < 0 || base_row >= statement->keyset_row_count) {
        diagnostics_add_record(&statement->diagnostics, "HY107", 0,
                               "Row number is out of range for the current rowset.");
        return SQL_ERROR;
    }

    switch (operation) {
    case SQL_UPDATE:
        return keyset_positioned_update(statement, base_row);
    case SQL_DELETE:
        return keyset_positioned_delete(statement, base_row);
    case SQL_REFRESH:
        return keyset_positioned_refresh(statement, base_row);
    case SQL_POSITION:
        statement->current_row_position = base_row;
        return SQL_SUCCESS;
    default:
        diagnostics_add_record(&statement->diagnostics, "HY092", 0,
                               "Unsupported SQLSetPos operation.");
        return SQL_ERROR;
    }
}

/* ==================================================================
 * SQLBulkOperations — bookmark-keyed bulk INSERT / UPDATE / DELETE / FETCH
 *
 * These operations reuse the same hidden-ctid keyset machinery as SQLSetPos, but
 * identify their target row(s) by bookmark rather than by rowset row number. A
 * bookmark is a 4-byte Int4 = 1-based row index (see statement.h); resolving one
 * yields a 0-based buffered row, from which keyset_row_ctid() derives the ctid
 * that keys the positioned UPDATE/DELETE.
 * ================================================================== */

/*
 * Read the bookmark held in rowset element `element` of the bound column-0
 * buffer and resolve it to a 0-based buffered row, or return a negative sentinel
 * if it cannot be resolved.
 *
 * For a VARBOOKMARK binding the elements are laid out with a stride equal to the
 * bound buffer length (14 bytes in the bulk-operations test), so element N lives
 * at bookmark_buffer + N * bookmark_buffer_length. The leading 4 bytes of each
 * element hold the Int4 bookmark. A single-bookmark binding (row_array_size 1)
 * is just element 0.
 */
static int bulk_resolve_bound_bookmark_row(const OdbcStatement *statement,
                                           SQLULEN element)
{
    if (!statement->bookmark_bound || !statement->bookmark_buffer) {
        return -1;
    }
    /* SQL_C_BOOKMARK is a fixed 4-byte type; SQL_C_VARBOOKMARK strides by the
     * declared buffer length. Fall back to the bookmark size when the length is
     * unknown so a single-element read still works. */
    size_t stride = (statement->bookmark_target_type == SQL_C_VARBOOKMARK &&
                     statement->bookmark_buffer_length > 0)
                        ? (size_t)statement->bookmark_buffer_length
                        : sizeof(Int4Bookmark);

    /* Reject an undersized VARBOOKMARK buffer before reading: the memcpy below
     * pulls a full Int4Bookmark out of each element, so a stride smaller than the
     * bookmark would read past the application's buffer. This mirrors the
     * write-side guard in write_row_bookmark and treats the element as "no row". */
    if (statement->bookmark_target_type == SQL_C_VARBOOKMARK &&
        stride < sizeof(Int4Bookmark)) {
        return -1;
    }

    const char *element_address =
        (const char *)statement->bookmark_buffer + (size_t)element * stride;

    Int4Bookmark bookmark;
    memcpy(&bookmark, element_address, sizeof(bookmark));
    return statement_resolve_int4_bookmark(bookmark);
}

/*
 * SQL_FETCH_BY_BOOKMARK: for each rowset element, resolve the bound bookmark to a
 * buffered row and copy that row's public columns into element `i` of the bound
 * column arrays (honoring the keyset overlay, so an updated row shows its new
 * values and a row added by SQL_ADD reads back its inserted values). Reports the
 * fetched-row count and per-row status via the block-cursor pointers.
 */
static SQLRETURN bulk_fetch_by_bookmark(OdbcStatement *statement)
{
    SQLULEN rowset_size = statement->row_array_size > 0
                              ? statement->row_array_size : 1;
    SQLRETURN overall_result = SQL_SUCCESS;
    SQLULEN rows_fetched = 0;

    for (SQLULEN element = 0; element < rowset_size; element++) {
        int source_row = bulk_resolve_bound_bookmark_row(statement, element);

        /* A bookmark that resolves outside the buffered/added row range names no
         * live row; mark the slot NOROW and continue with the rest. */
        if (source_row < 0 || source_row >= statement->keyset_row_count ||
            statement_row_is_deleted(statement, source_row)) {
            if (statement->row_status_ptr) {
                statement->row_status_ptr[element] = SQL_ROW_NOROW;
            }
            continue;
        }

        SQLRETURN row_result =
            populate_bound_columns_row(statement, source_row, element);
        if (row_result == SQL_SUCCESS_WITH_INFO) {
            overall_result = SQL_SUCCESS_WITH_INFO;
        }
        if (statement->row_status_ptr) {
            statement->row_status_ptr[element] =
                (row_result == SQL_SUCCESS_WITH_INFO) ? SQL_ROW_SUCCESS_WITH_INFO
                                                      : SQL_ROW_SUCCESS;
        }
        rows_fetched++;
    }

    if (statement->rows_fetched_ptr) {
        *statement->rows_fetched_ptr = rows_fetched;
    }
    return overall_result;
}

SQLRETURN statement_bulk_operations(OdbcStatement *statement,
                                    SQLUSMALLINT operation)
{
    if (!statement->current_result || !statement->has_result_set) {
        diagnostics_add_record(&statement->diagnostics, "24000", 0,
                               "No open cursor for SQLBulkOperations.");
        return SQL_ERROR;
    }
    if (!statement->is_updatable_cursor ||
        statement->hidden_ctid_column_index == NO_HIDDEN_CTID_COLUMN) {
        diagnostics_add_record(&statement->diagnostics, "HY092", 0,
                               "SQLBulkOperations requires an updatable (keyset) cursor.");
        return SQL_ERROR;
    }

    /* SQL_ADD does not reference an existing row. It shares the positioned-add
     * insert path but, unlike SQLSetPos, captures the inserted row and returns
     * its bookmark to the application (capture_inserted_row = true). */
    if (operation == SQL_ADD) {
        return keyset_positioned_add(statement, true);
    }

    /* The bookmark-keyed operations require bookmarks to be enabled and column 0
     * bound to the bookmark(s) that name the target row(s). */
    if (operation == SQL_UPDATE_BY_BOOKMARK ||
        operation == SQL_DELETE_BY_BOOKMARK ||
        operation == SQL_FETCH_BY_BOOKMARK) {
        if (statement->use_bookmarks == SQL_UB_OFF || !statement->bookmark_bound) {
            diagnostics_add_record(&statement->diagnostics, "HY092", 0,
                                   "Bookmark operation requires bookmarks enabled and column 0 bound.");
            return SQL_ERROR;
        }
    }

    if (operation == SQL_FETCH_BY_BOOKMARK) {
        return bulk_fetch_by_bookmark(statement);
    }

    /* SQL_UPDATE_BY_BOOKMARK / SQL_DELETE_BY_BOOKMARK: resolve the bookmark of
     * each rowset element to its buffered row and apply the positioned operation.
     * The bulk-operations test uses row_array_size 1 here, so this typically runs
     * once against the single bound bookmark. The positioned UPDATE reads the
     * bound column values from rowset element 0; per-element UPDATE values for a
     * multi-row rowset are out of scope (no acceptance test exercises them). */
    SQLULEN rowset_size = statement->row_array_size > 0
                              ? statement->row_array_size : 1;
    SQLRETURN overall_result = SQL_SUCCESS;

    for (SQLULEN element = 0; element < rowset_size; element++) {
        int base_row = bulk_resolve_bound_bookmark_row(statement, element);
        if (base_row < 0 || base_row >= statement->keyset_row_count) {
            diagnostics_add_record(&statement->diagnostics, "HY107", 0,
                                   "Bookmark does not resolve to a valid row.");
            return SQL_ERROR;
        }

        SQLRETURN row_result;
        if (operation == SQL_UPDATE_BY_BOOKMARK) {
            row_result = keyset_positioned_update(statement, base_row);
        } else {
            row_result = keyset_positioned_delete(statement, base_row);
        }
        if (row_result == SQL_ERROR) {
            return SQL_ERROR;
        }
        if (row_result == SQL_SUCCESS_WITH_INFO) {
            overall_result = SQL_SUCCESS_WITH_INFO;
        }
    }
    return overall_result;
}
