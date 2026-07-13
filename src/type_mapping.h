/*-------------------------------------------------------------------------
 *
 * type_mapping.h
 *	  Type mapping function declarations
 *
 * Copyright (c) 2026, Dave Cramer
 *
 *
 * IDENTIFICATION
 *	  src/type_mapping.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PSQLODBC2_TYPE_MAPPING_H
#define PSQLODBC2_TYPE_MAPPING_H

#include "psqlodbc2.h"

#include <stddef.h>
#include <stdbool.h>

/* ---- PostgreSQL Type OIDs ----
 *
 * These are the fixed OID values assigned to built-in PostgreSQL types.
 * They are stable across PostgreSQL versions and defined in pg_type.h
 * in the PostgreSQL source. We define only the commonly-used subset. */

#define PG_TYPE_BOOL          16
#define PG_TYPE_BYTEA         17
#define PG_TYPE_CHAR          18   /* single-byte internal type "char" */
#define PG_TYPE_NAME          19
#define PG_TYPE_INT8          20
#define PG_TYPE_INT2          21
#define PG_TYPE_INT4          23
#define PG_TYPE_TEXT          25
#define PG_TYPE_OID           26
#define PG_TYPE_FLOAT4       700
#define PG_TYPE_FLOAT8       701
#define PG_TYPE_BPCHAR      1042   /* blank-padded character(n) */
#define PG_TYPE_VARCHAR     1043
#define PG_TYPE_DATE        1082
#define PG_TYPE_TIME        1083
#define PG_TYPE_TIMESTAMP   1114
#define PG_TYPE_TIMESTAMPTZ 1184
#define PG_TYPE_INTERVAL    1186
#define PG_TYPE_NUMERIC     1700
#define PG_TYPE_REFCURSOR   1790   /* cursor returned by a function/procedure OUT param */
#define PG_TYPE_UUID        2950

/* Column size reported for a boolean column when the BoolsAsChar option is on.
 * Wide enough to hold the textual representation "false". Matches the original
 * psqlodbc driver's PG_WIDTH_OF_BOOLS_AS_CHAR. */
#define PG_WIDTH_OF_BOOLS_AS_CHAR 5

/* Column size reported for unbounded text / LONGVARCHAR columns. PostgreSQL's
 * text type has no declared length, but the original psqlodbc driver reports a
 * fixed size (its TEXT_FIELD_SIZE) so applications get a concrete precision from
 * SQLDescribeCol. */
#define TEXT_FIELD_COLUMN_SIZE 8190

/* ---- Public Interface ---- */

/*
 * Map a PostgreSQL type OID to the corresponding ODBC SQL type constant.
 * Returns SQL_VARCHAR for unrecognized OIDs (safe fallback — all data can
 * be represented as character strings).
 */
SQLSMALLINT type_mapping_get_sql_type(unsigned int postgres_oid);

/*
 * Get the default C type that SQLGetData should use when the application
 * requests SQL_C_DEFAULT. The mapping follows ODBC conventions:
 * SQL_INTEGER → SQL_C_SLONG, SQL_VARCHAR → SQL_C_CHAR, etc.
 */
SQLSMALLINT type_mapping_get_default_c_type(SQLSMALLINT sql_type);

/*
 * Compute the column display size for a given PostgreSQL type and type modifier.
 * For varchar(N), typmod encodes the max length (typmod - 4 = max chars).
 * For numeric(p,s), typmod encodes precision.
 * Returns 0 for variable-length types without a declared limit (e.g., text).
 */
SQLULEN type_mapping_get_column_size(unsigned int postgres_oid, int type_modifier);

/*
 * Compute the number of decimal digits (scale) for a given type and modifier.
 * Meaningful for numeric(p,s) types; returns 0 for integer and string types.
 */
SQLSMALLINT type_mapping_get_decimal_digits(unsigned int postgres_oid, int type_modifier);

/*
 * Get a human-readable type name string for a PostgreSQL OID.
 * Returns "unknown" for unrecognized OIDs.
 */
const char *type_mapping_get_type_name(unsigned int postgres_oid);

/* ---- Numeric (SQL_NUMERIC_STRUCT) Conversion ----
 *
 * ODBC's SQL_C_NUMERIC exchanges exact decimals as a packed SQL_NUMERIC_STRUCT:
 *   sign      1 = positive, 0 = negative (note: opposite of the intuitive sense)
 *   precision total count of significant decimal digits
 *   scale     count of digits to the right of the decimal point
 *   val[16]   the unscaled integer value as a 128-bit LITTLE-ENDIAN mantissa
 *
 * A 16-byte mantissa can hold at most 2^128 - 1 = 340282366920938463463374607431768211455,
 * which is 39 decimal digits. */
#define NUMERIC_MAX_MANTISSA_DIGITS 39

/* Upper bound on the text length produced by type_mapping_format_numeric_text.
 * Worst case is a leading '-', up to the maximum signed-char scale (127) worth
 * of fractional digits, a decimal point, and up to NUMERIC_MAX_MANTISSA_DIGITS
 * integer digits. 256 comfortably covers this. */
#define NUMERIC_TEXT_BUFFER_SIZE 256

/*
 * Parse a PostgreSQL numeric text value (e.g. "123.45", "-0.001", "7.70") into
 * a packed SQL_NUMERIC_STRUCT.
 *
 * Leading whitespace, an optional sign, and leading zeros are consumed first;
 * every remaining decimal digit contributes to precision, and digits after the
 * decimal point additionally contribute to scale (trailing zeros are kept, so
 * "7.70" yields precision 3 / scale 2). The unscaled digits are accumulated
 * into the little-endian 128-bit mantissa.
 *
 * *overflow is set to true when the unscaled value does not fit in 128 bits; in
 * that case the mantissa holds the low 128 bits (the carry is discarded), which
 * matches the original psqlodbc behavior.
 */
void type_mapping_parse_numeric_text(const char *text,
                                     SQL_NUMERIC_STRUCT *numeric,
                                     bool *overflow);

/*
 * Format a packed SQL_NUMERIC_STRUCT back into decimal text (the inverse of
 * type_mapping_parse_numeric_text), writing a null-terminated string into
 * out_buffer (which must be at least NUMERIC_TEXT_BUFFER_SIZE bytes).
 *
 * Returns the string length written, or -1 if the buffer is too small.
 */
int type_mapping_format_numeric_text(const SQL_NUMERIC_STRUCT *numeric,
                                     char *out_buffer,
                                     size_t out_buffer_size);

/* ---- UTF-8 <-> UTF-16LE Conversion (SQL_C_WCHAR) ----
 *
 * ODBC's SQL_C_WCHAR exchanges text as UTF-16 code units (SQLWCHAR, 2 bytes on
 * every platform this driver targets). PostgreSQL's client encoding for these
 * tests is UTF-8, so the driver transcodes between UTF-8 and UTF-16 at the
 * result and parameter boundaries.
 *
 * All target platforms (Linux/macOS on x86-64 or arm64, Windows) are
 * little-endian, so a native SQLWCHAR array is already laid out as UTF-16LE.
 * The helpers therefore store/read SQLWCHAR values in native order; on a
 * hypothetical big-endian host they would need byte-swapping. */

/*
 * Transcode a UTF-8 byte string into a freshly heap-allocated array of UTF-16
 * code units (caller frees). Code points above the BMP (> U+FFFF) are encoded
 * as a high/low surrogate pair, so one such code point yields two units.
 *
 * utf8_length is the number of input bytes, or SQL_NTS to measure with strlen.
 * *out_unit_count receives the number of UTF-16 code units produced; the
 * returned array is NOT null-terminated (the caller adds a terminator when it
 * copies into the application buffer).
 *
 * Malformed or truncated UTF-8 is decoded leniently: an invalid lead byte is
 * passed through as a single code unit so conversion never fails or loops.
 * Returns NULL only on allocation failure or NULL input.
 */
SQLWCHAR *type_mapping_utf8_to_utf16le(const char *utf8_text,
                                       int utf8_length,
                                       size_t *out_unit_count);

/*
 * Transcode an array of UTF-16 code units into a freshly heap-allocated,
 * null-terminated UTF-8 byte string (caller frees). High/low surrogate pairs
 * are recombined into a single code point > U+FFFF.
 *
 * unit_count is the number of SQLWCHAR units to read. *out_byte_length receives
 * the UTF-8 byte length (excluding the null terminator). Returns NULL only on
 * allocation failure or NULL input.
 */
char *type_mapping_utf16le_to_utf8(const SQLWCHAR *utf16_text,
                                   size_t unit_count,
                                   int *out_byte_length);

/* ---- Interval Fractional-Second Precision ----
 *
 * SQL_INTERVAL_STRUCT stores fractional seconds as a whole number of units in
 * a 9-digit (nanosecond) field. An interval like "01:02:03.123456" carries the
 * fractional text "123456"; the requested precision decides how many of the
 * nine digit positions are significant.
 *
 * SECURITY (GitHub psqlodbc #173): the application controls this precision via
 * ARD SQL_DESC_PRECISION and can set an arbitrarily large value (the
 * interval-overflow test sets 20). The original getPrecisionPart() formatted
 * into a fixed 10-byte stack buffer ("000000000" + NUL); a precision above 9
 * indexed past the terminator and overran the stack. We therefore CLAMP the
 * effective precision to at most 9 (INTERVAL_MAX_FRACTION_DIGITS) before it can
 * size or index any buffer. A negative precision means "unspecified" and
 * defaults to microsecond (6-digit) precision, matching the original. */
#define INTERVAL_MAX_FRACTION_DIGITS 9
#define INTERVAL_DEFAULT_FRACTION_DIGITS 6

/*
 * Convert the fractional-seconds digit string of an interval (e.g. "123456")
 * into the integer nanosecond-scale value expected in
 * SQL_INTERVAL_STRUCT.intval.day_second.fraction, honoring the requested
 * decimal precision.
 *
 * requested_precision is the ARD SQL_DESC_PRECISION (or a negative value when
 * unset). It is defaulted and then CLAMPED to [0, INTERVAL_MAX_FRACTION_DIGITS]
 * before use, so no caller-supplied precision can drive an out-of-bounds write
 * (see the #173 note above). The digit string is right-padded with zeros to
 * nine digits, truncated to the effective precision, and parsed as an integer.
 */
unsigned int type_mapping_interval_fraction(int requested_precision,
                                            const char *fraction_digits);

/*
 * Return the PostgreSQL cast suffix (e.g. "::int4") for a bound parameter's
 * SQL type, or NULL when no cast is appropriate.
 *
 * When an application binds a parameter with an explicit SQL type via
 * SQLBindParameter (e.g. SQL_SMALLINT), the driver appends this suffix to the
 * corresponding "$N" marker so PostgreSQL interprets the value as that type
 * rather than as an untyped string literal. This makes comparisons behave
 * numerically and produces type-accurate server error messages, matching the
 * original psqlodbc's sqltype_to_pgcast() behavior.
 *
 * SQL_CHAR/SQL_VARCHAR intentionally return NULL: leaving them untyped lets
 * PostgreSQL treat the value as text, which is what applications expect for
 * character parameters.
 */
const char *type_mapping_get_param_cast(SQLSMALLINT sql_type);

#endif /* PSQLODBC2_TYPE_MAPPING_H */
