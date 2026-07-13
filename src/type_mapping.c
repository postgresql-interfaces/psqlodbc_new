/*-------------------------------------------------------------------------
 *
 * type_mapping.c
 *	  PostgreSQL OID to ODBC SQL type mapping
 *
 * Copyright (c) 2026, Dave Cramer
 *
 *
 * IDENTIFICATION
 *	  src/type_mapping.c
 *
 *-------------------------------------------------------------------------
 */
#include "type_mapping.h"

#include <stddef.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdint.h>

/* ---- Type Mapping Table ---- */

typedef struct PostgresTypeMapping {
    unsigned int postgres_oid;
    SQLSMALLINT  sql_type;
    SQLSMALLINT  default_c_type;
    const char  *type_name;
} PostgresTypeMapping;

static const PostgresTypeMapping type_table[] = {
    { PG_TYPE_BOOL,        SQL_BIT,              SQL_C_BIT,      "bool"        },
    { PG_TYPE_BYTEA,       SQL_LONGVARBINARY,    SQL_C_BINARY,   "bytea"       },
    { PG_TYPE_CHAR,        SQL_CHAR,             SQL_C_CHAR,     "char"        },
    { PG_TYPE_NAME,        SQL_VARCHAR,          SQL_C_CHAR,     "name"        },
    { PG_TYPE_INT8,        SQL_BIGINT,           SQL_C_SBIGINT,  "int8"        },
    { PG_TYPE_INT2,        SQL_SMALLINT,         SQL_C_SSHORT,   "int2"        },
    { PG_TYPE_INT4,        SQL_INTEGER,          SQL_C_SLONG,    "int4"        },
    { PG_TYPE_TEXT,        SQL_LONGVARCHAR,      SQL_C_CHAR,     "text"        },
    { PG_TYPE_OID,         SQL_INTEGER,          SQL_C_SLONG,    "oid"         },
    { PG_TYPE_FLOAT4,      SQL_REAL,             SQL_C_FLOAT,    "float4"      },
    { PG_TYPE_FLOAT8,      SQL_DOUBLE,           SQL_C_DOUBLE,   "float8"      },
    { PG_TYPE_BPCHAR,      SQL_CHAR,             SQL_C_CHAR,     "bpchar"      },
    { PG_TYPE_VARCHAR,     SQL_VARCHAR,          SQL_C_CHAR,     "varchar"     },
    { PG_TYPE_DATE,        SQL_TYPE_DATE,        SQL_C_CHAR,     "date"        },
    { PG_TYPE_TIME,        SQL_TYPE_TIME,        SQL_C_CHAR,     "time"        },
    { PG_TYPE_TIMESTAMP,   SQL_TYPE_TIMESTAMP,   SQL_C_CHAR,     "timestamp"   },
    { PG_TYPE_TIMESTAMPTZ, SQL_TYPE_TIMESTAMP,   SQL_C_CHAR,     "timestamptz" },
    { PG_TYPE_NUMERIC,     SQL_NUMERIC,          SQL_C_CHAR,     "numeric"     },
    { PG_TYPE_UUID,        SQL_GUID,             SQL_C_CHAR,     "uuid"        },
};

static const int TYPE_TABLE_SIZE = (int)(sizeof(type_table) / sizeof(type_table[0]));

/* ---- Internal Helpers ---- */

static const PostgresTypeMapping *find_mapping(unsigned int postgres_oid)
{
    for (int index = 0; index < TYPE_TABLE_SIZE; index++) {
        if (type_table[index].postgres_oid == postgres_oid) {
            return &type_table[index];
        }
    }
    return NULL;
}

/* ---- Public Interface ---- */

SQLSMALLINT type_mapping_get_sql_type(unsigned int postgres_oid)
{
    const PostgresTypeMapping *mapping = find_mapping(postgres_oid);
    if (mapping) {
        return mapping->sql_type;
    }
    return SQL_VARCHAR;
}

SQLSMALLINT type_mapping_get_default_c_type(SQLSMALLINT sql_type)
{
    switch (sql_type) {
    case SQL_BIT:
        return SQL_C_BIT;
    case SQL_SMALLINT:
        return SQL_C_SSHORT;
    case SQL_INTEGER:
        return SQL_C_SLONG;
    case SQL_BIGINT:
        return SQL_C_SBIGINT;
    case SQL_REAL:
        return SQL_C_FLOAT;
    case SQL_DOUBLE:
    case SQL_FLOAT:
        return SQL_C_DOUBLE;
    case SQL_NUMERIC:
    case SQL_DECIMAL:
        return SQL_C_CHAR;
    case SQL_CHAR:
    case SQL_VARCHAR:
    case SQL_LONGVARCHAR:
        return SQL_C_CHAR;
    case SQL_TYPE_DATE:
        return SQL_C_CHAR;
    case SQL_TYPE_TIME:
        return SQL_C_CHAR;
    case SQL_TYPE_TIMESTAMP:
        return SQL_C_CHAR;
    case SQL_BINARY:
    case SQL_VARBINARY:
    case SQL_LONGVARBINARY:
        return SQL_C_BINARY;
    case SQL_GUID:
        return SQL_C_CHAR;
    default:
        return SQL_C_CHAR;
    }
}

SQLULEN type_mapping_get_column_size(unsigned int postgres_oid, int type_modifier)
{
    switch (postgres_oid) {
    case PG_TYPE_BOOL:
        return 1;
    case PG_TYPE_INT2:
        return 5;      /* -32768 to 32767 */
    case PG_TYPE_INT4:
    case PG_TYPE_OID:
        return 10;     /* -2147483648 to 2147483647 */
    case PG_TYPE_INT8:
        return 19;     /* 19 digits for int8 */
    case PG_TYPE_FLOAT4:
        return 7;      /* ~7 significant digits */
    case PG_TYPE_FLOAT8:
        return 15;     /* ~15 significant digits */
    case PG_TYPE_DATE:
        return 10;     /* YYYY-MM-DD */
    case PG_TYPE_TIME:
        return 8;      /* HH:MM:SS (without fractional seconds) */
    case PG_TYPE_TIMESTAMP:
    case PG_TYPE_TIMESTAMPTZ:
        return 26;     /* YYYY-MM-DD HH:MM:SS.ffffff */
    case PG_TYPE_UUID:
        return 36;     /* xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx */
    case PG_TYPE_CHAR:
        return 1;
    case PG_TYPE_NAME:
        return 63;     /* NAMEDATALEN - 1 in PostgreSQL */

    case PG_TYPE_VARCHAR:
    case PG_TYPE_BPCHAR:
        /* typmod for varchar/char(n) is declared_length + 4 (header overhead).
         * If typmod is -1, the type has no declared limit. */
        if (type_modifier > 4) {
            return (SQLULEN)(type_modifier - 4);
        }
        return 0;  /* Variable length, no declared limit */

    case PG_TYPE_NUMERIC:
        /* typmod for numeric(p,s): precision = ((typmod-4) >> 16) & 0xFFFF.
         * If typmod is -1, it's an unconstrained numeric. */
        if (type_modifier > 4) {
            return (SQLULEN)(((type_modifier - 4) >> 16) & 0xFFFF);
        }
        return 38;  /* Default precision for unconstrained numeric */

    case PG_TYPE_TEXT:
        /* PostgreSQL text has no declared limit, but the original psqlodbc
         * driver reports the fixed TEXT_FIELD_SIZE (8190) as the column size for
         * text / LONGVARCHAR columns rather than 0. Applications that lay out
         * fixed-width buffers from SQLDescribeCol rely on this non-zero size. */
        return TEXT_FIELD_COLUMN_SIZE;

    case PG_TYPE_BYTEA:
        return 0;  /* Variable length, no inherent limit */

    default:
        return 0;
    }
}

SQLSMALLINT type_mapping_get_decimal_digits(unsigned int postgres_oid, int type_modifier)
{
    switch (postgres_oid) {
    case PG_TYPE_NUMERIC:
        /* typmod for numeric(p,s): scale = (typmod-4) & 0xFFFF */
        if (type_modifier > 4) {
            return (SQLSMALLINT)((type_modifier - 4) & 0xFFFF);
        }
        return 0;  /* Unconstrained numeric — scale unknown */

    case PG_TYPE_TIMESTAMP:
    case PG_TYPE_TIMESTAMPTZ:
        /* PostgreSQL timestamps have microsecond precision (6 fractional digits) */
        return 6;

    case PG_TYPE_TIME:
        return 6;

    case PG_TYPE_INT2:
    case PG_TYPE_INT4:
    case PG_TYPE_INT8:
    case PG_TYPE_OID:
        return 0;  /* Integer types have no fractional digits */

    default:
        return 0;
    }
}

const char *type_mapping_get_type_name(unsigned int postgres_oid)
{
    const PostgresTypeMapping *mapping = find_mapping(postgres_oid);
    if (mapping) {
        return mapping->type_name;
    }
    return "unknown";
}

void type_mapping_parse_numeric_text(const char *text,
                                     SQL_NUMERIC_STRUCT *numeric,
                                     bool *overflow)
{
    /* Number of decimal digits we can safely hold before accumulation. We store
     * every parsed digit so that scale (trailing fractional zeros) is preserved
     * exactly; a value wider than the 128-bit mantissa still parses but sets the
     * overflow flag. The 3x factor mirrors the original driver's generous cap. */
    char digits[SQL_MAX_NUMERIC_LEN * 3];

    *overflow = false;

    const char *cursor = text;

    /* Skip leading whitespace. */
    while (*cursor && isspace((unsigned char)*cursor)) {
        cursor++;
    }

    /* Sign: ODBC uses 1 for positive, 0 for negative. */
    numeric->sign = 1;
    if (*cursor == '-') {
        numeric->sign = 0;
        cursor++;
    } else if (*cursor == '+') {
        cursor++;
    }

    /* Leading zeros do not count toward precision. */
    while (*cursor == '0') {
        cursor++;
    }

    /* Collect significant digits, tracking scale once we pass the decimal point.
     * Digits beyond the buffer capacity in the integer part are dropped and flag
     * an overflow; extra fractional digits are simply ignored. */
    numeric->precision = 0;
    numeric->scale = 0;
    int digit_count = 0;
    bool seen_decimal_point = false;
    for (; *cursor; cursor++) {
        if (*cursor == '.') {
            if (seen_decimal_point) {
                break;
            }
            seen_decimal_point = true;
            continue;
        }
        if (!isdigit((unsigned char)*cursor)) {
            break;
        }

        if ((size_t)digit_count >= sizeof(digits)) {
            if (seen_decimal_point) {
                /* No room for more fractional digits — safe to stop. */
                break;
            }
            /* Another integer-part digit we cannot store: the value scales up by
             * a factor of ten we can no longer represent, so record overflow. */
            numeric->scale--;
            *overflow = true;
            continue;
        }

        if (seen_decimal_point) {
            numeric->scale++;
        }
        digits[digit_count++] = *cursor;
    }
    numeric->precision = (SQLCHAR)digit_count;

    /* Accumulate the collected digits into the little-endian 128-bit mantissa by
     * repeatedly computing mantissa = mantissa * 10 + digit across all 16 bytes. */
    memset(numeric->val, 0, sizeof(numeric->val));
    for (int digit_index = 0; digit_index < digit_count; digit_index++) {
        unsigned int carry = (unsigned int)(digits[digit_index] - '0');
        for (size_t byte_index = 0; byte_index < sizeof(numeric->val); byte_index++) {
            unsigned int product = (unsigned int)numeric->val[byte_index] * 10 + carry;
            numeric->val[byte_index] = (SQLCHAR)(product & 0xFF);
            carry = product >> 8;
        }
        /* A nonzero carry after the top byte means the value exceeded 2^128. */
        if (carry != 0) {
            *overflow = true;
        }
    }
}

int type_mapping_format_numeric_text(const SQL_NUMERIC_STRUCT *numeric,
                                     char *out_buffer,
                                     size_t out_buffer_size)
{
    if (out_buffer_size < NUMERIC_TEXT_BUFFER_SIZE) {
        return -1;
    }

    /* A precision of zero means the application supplied no significant digits;
     * the numeric value is simply zero. */
    if (numeric->precision == 0) {
        out_buffer[0] = '0';
        out_buffer[1] = '\0';
        return 1;
    }

    int precision = numeric->precision;
    if (precision > NUMERIC_MAX_MANTISSA_DIGITS) {
        precision = NUMERIC_MAX_MANTISSA_DIGITS;
    }

    /* Repeatedly divide the little-endian mantissa by 10 to extract decimal
     * digits, least-significant first, into extracted_digits. */
    SQLCHAR working_value[SQL_MAX_NUMERIC_LEN];
    memcpy(working_value, numeric->val, SQL_MAX_NUMERIC_LEN);

    char extracted_digits[NUMERIC_MAX_MANTISSA_DIGITS];
    int extracted_count = 0;
    int significant_length = SQL_MAX_NUMERIC_LEN;
    do {
        unsigned int remainder = 0;
        int highest_nonzero_byte = -1;
        /* Long division from the most-significant byte down, carrying the
         * remainder into the next-lower byte. */
        for (int byte_index = significant_length - 1; byte_index >= 0; byte_index--) {
            unsigned int value = (unsigned int)working_value[byte_index] + (remainder << 8);
            working_value[byte_index] = (SQLCHAR)(value / 10);
            remainder = value % 10;
            if (working_value[byte_index] != 0 && highest_nonzero_byte == -1) {
                highest_nonzero_byte = byte_index;
            }
        }
        extracted_digits[extracted_count++] = (char)remainder;
        significant_length = highest_nonzero_byte + 1;
    } while (significant_length > 0 && extracted_count < precision);

    /* extracted_digits[0] is the least-significant digit. Emit the integer part
     * (digits at index >= scale) then, if there is a scale, the fractional part.
     * Positions past the extracted digits contribute leading/place-holder zeros. */
    int length = 0;
    if (numeric->sign == 0) {
        out_buffer[length++] = '-';
    }

    int digit_index = extracted_count - 1;
    if (digit_index < numeric->scale) {
        digit_index = numeric->scale;  /* Ensure at least one integer digit ('0'). */
    }
    for (; digit_index >= numeric->scale; digit_index--) {
        out_buffer[length++] =
            (digit_index >= extracted_count) ? '0' : (char)(extracted_digits[digit_index] + '0');
    }

    if (numeric->scale > 0) {
        out_buffer[length++] = '.';
        for (; digit_index >= 0; digit_index--) {
            out_buffer[length++] =
                (digit_index >= extracted_count) ? '0' : (char)(extracted_digits[digit_index] + '0');
        }
    }

    if (extracted_count == 0) {
        out_buffer[length++] = '0';
    }
    out_buffer[length] = '\0';
    return length;
}

/* Unicode boundary constants for surrogate-pair (UTF-16) encoding. Code points
 * at or above this base are represented in UTF-16 as a high+low surrogate pair;
 * below it they occupy a single 16-bit unit. */
#define UNICODE_SUPPLEMENTARY_PLANE_BASE 0x10000u
#define UTF16_HIGH_SURROGATE_START       0xD800u
#define UTF16_LOW_SURROGATE_START        0xDC00u
#define UTF16_SURROGATE_END              0xDFFFu
/* The 10-bit halves of a supplementary code point are packed into the low bits
 * of each surrogate. */
#define UTF16_SURROGATE_HALF_MASK        0x3FFu
#define UTF16_SURROGATE_HALF_BITS        10

SQLWCHAR *type_mapping_utf8_to_utf16le(const char *utf8_text,
                                       int utf8_length,
                                       size_t *out_unit_count)
{
    if (!utf8_text) {
        if (out_unit_count) {
            *out_unit_count = 0;
        }
        return NULL;
    }

    size_t byte_count = (utf8_length == SQL_NTS)
                            ? strlen(utf8_text)
                            : (size_t)utf8_length;

    /* Worst case is one UTF-16 unit per input byte (all-ASCII); supplementary
     * code points consume 4 input bytes but emit only 2 units, so this bound is
     * always safe. Allocate room for a trailing NUL the caller may want. */
    SQLWCHAR *units = malloc((byte_count + 1) * sizeof(SQLWCHAR));
    if (!units) {
        if (out_unit_count) {
            *out_unit_count = 0;
        }
        return NULL;
    }

    const unsigned char *input = (const unsigned char *)utf8_text;
    size_t position = 0;
    size_t unit_index = 0;

    while (position < byte_count) {
        unsigned char lead = input[position];
        uint32_t code_point;
        size_t sequence_length;

        /* Decode the UTF-8 sequence length from the lead byte. A continuation
         * or invalid lead byte is treated as a lone 1-byte unit (lenient). */
        if (lead < 0x80) {
            code_point = lead;
            sequence_length = 1;
        } else if ((lead & 0xE0) == 0xC0) {
            code_point = lead & 0x1Fu;
            sequence_length = 2;
        } else if ((lead & 0xF0) == 0xE0) {
            code_point = lead & 0x0Fu;
            sequence_length = 3;
        } else if ((lead & 0xF8) == 0xF0) {
            code_point = lead & 0x07u;
            sequence_length = 4;
        } else {
            code_point = lead;
            sequence_length = 1;
        }

        /* If the declared sequence runs past the end of input, fall back to
         * emitting the lead byte alone rather than reading out of bounds. */
        if (position + sequence_length > byte_count) {
            units[unit_index++] = (SQLWCHAR)lead;
            position++;
            continue;
        }

        /* Fold in the continuation bytes (each contributes 6 low bits). */
        for (size_t offset = 1; offset < sequence_length; offset++) {
            code_point = (code_point << 6) | (input[position + offset] & 0x3Fu);
        }
        position += sequence_length;

        if (code_point >= UNICODE_SUPPLEMENTARY_PLANE_BASE) {
            /* Emit a surrogate pair for code points beyond the BMP. */
            uint32_t adjusted = code_point - UNICODE_SUPPLEMENTARY_PLANE_BASE;
            units[unit_index++] = (SQLWCHAR)(UTF16_HIGH_SURROGATE_START +
                (adjusted >> UTF16_SURROGATE_HALF_BITS));
            units[unit_index++] = (SQLWCHAR)(UTF16_LOW_SURROGATE_START +
                (adjusted & UTF16_SURROGATE_HALF_MASK));
        } else {
            units[unit_index++] = (SQLWCHAR)code_point;
        }
    }

    if (out_unit_count) {
        *out_unit_count = unit_index;
    }
    return units;
}

char *type_mapping_utf16le_to_utf8(const SQLWCHAR *utf16_text,
                                   size_t unit_count,
                                   int *out_byte_length)
{
    if (!utf16_text) {
        if (out_byte_length) {
            *out_byte_length = 0;
        }
        return NULL;
    }

    /* A single code point encodes to at most 4 UTF-8 bytes, so 4 bytes per input
     * unit is a safe upper bound (a surrogate pair is 2 units -> 4 bytes). */
    char *utf8 = malloc(unit_count * 4 + 1);
    if (!utf8) {
        if (out_byte_length) {
            *out_byte_length = 0;
        }
        return NULL;
    }

    size_t byte_length = 0;
    for (size_t index = 0; index < unit_count; index++) {
        uint32_t code_point = utf16_text[index];

        /* Recombine a well-formed high+low surrogate pair into one code point. */
        if (code_point >= UTF16_HIGH_SURROGATE_START &&
            code_point < UTF16_LOW_SURROGATE_START &&
            index + 1 < unit_count &&
            utf16_text[index + 1] >= UTF16_LOW_SURROGATE_START &&
            utf16_text[index + 1] <= UTF16_SURROGATE_END) {
            uint32_t high = code_point - UTF16_HIGH_SURROGATE_START;
            uint32_t low = utf16_text[index + 1] - UTF16_LOW_SURROGATE_START;
            code_point = UNICODE_SUPPLEMENTARY_PLANE_BASE +
                ((high << UTF16_SURROGATE_HALF_BITS) | low);
            index++;  /* Consumed the low surrogate as well. */
        }

        if (code_point < 0x80) {
            utf8[byte_length++] = (char)code_point;
        } else if (code_point < 0x800) {
            utf8[byte_length++] = (char)(0xC0 | (code_point >> 6));
            utf8[byte_length++] = (char)(0x80 | (code_point & 0x3F));
        } else if (code_point < UNICODE_SUPPLEMENTARY_PLANE_BASE) {
            utf8[byte_length++] = (char)(0xE0 | (code_point >> 12));
            utf8[byte_length++] = (char)(0x80 | ((code_point >> 6) & 0x3F));
            utf8[byte_length++] = (char)(0x80 | (code_point & 0x3F));
        } else {
            utf8[byte_length++] = (char)(0xF0 | (code_point >> 18));
            utf8[byte_length++] = (char)(0x80 | ((code_point >> 12) & 0x3F));
            utf8[byte_length++] = (char)(0x80 | ((code_point >> 6) & 0x3F));
            utf8[byte_length++] = (char)(0x80 | (code_point & 0x3F));
        }
    }

    utf8[byte_length] = '\0';
    if (out_byte_length) {
        *out_byte_length = (int)byte_length;
    }
    return utf8;
}

unsigned int type_mapping_interval_fraction(int requested_precision,
                                            const char *fraction_digits)
{
    /* Fixed nine-slot scratch (nanosecond precision) plus terminator. Because
     * the precision is clamped below before it ever indexes this buffer, a
     * hostile ARD precision of 20 cannot write past fraction[9]. */
    char fraction[INTERVAL_MAX_FRACTION_DIGITS + 1] = "000000000";

    int precision = requested_precision;
    if (precision < 0) {
        precision = INTERVAL_DEFAULT_FRACTION_DIGITS;  /* unspecified -> microseconds */
    }
    if (precision == 0) {
        return 0;  /* Caller wants no fractional part. */
    }

    /* Copy the supplied digits over the zero template, but never more than the
     * nine slots available. */
    size_t copy_length = fraction_digits ? strlen(fraction_digits) : 0;
    if (copy_length > INTERVAL_MAX_FRACTION_DIGITS) {
        copy_length = INTERVAL_MAX_FRACTION_DIGITS;
    }
    memcpy(fraction, fraction_digits, copy_length);

    /* MANDATORY clamp: cap precision at nine so fraction[precision] stays within
     * bounds regardless of the application-supplied value (see #173). */
    if (precision > INTERVAL_MAX_FRACTION_DIGITS) {
        precision = INTERVAL_MAX_FRACTION_DIGITS;
    }
    fraction[precision] = '\0';

    return (unsigned int)strtoul(fraction, NULL, 10);
}

const char *type_mapping_get_param_cast(SQLSMALLINT sql_type)
{
    switch (sql_type) {
    case SQL_BINARY:
    case SQL_VARBINARY:
        return "::bytea";
    case SQL_TYPE_DATE:
    case SQL_DATE:
        return "::date";
    case SQL_DECIMAL:
    case SQL_NUMERIC:
        return "::numeric";
    case SQL_BIGINT:
        return "::int8";
    case SQL_INTEGER:
        return "::int4";
    case SQL_REAL:
        return "::float4";
    /* SQL_FLOAT and SQL_DOUBLE are intentionally left uncast, matching the
     * original driver. PostgreSQL infers the parameter type from context
     * (e.g. the numeric literal on the other side of a comparison), which
     * yields correct results and a "numeric" error for malformed input. */
    case SQL_SMALLINT:
    case SQL_TINYINT:
        return "::int2";
    case SQL_TIME:
    case SQL_TYPE_TIME:
        return "::time";
    case SQL_TIMESTAMP:
    case SQL_TYPE_TIMESTAMP:
        return "::timestamp";
    case SQL_GUID:
        return "::uuid";
    default:
        /* SQL_CHAR, SQL_VARCHAR, SQL_LONGVARCHAR and anything else: no cast,
         * so the value is treated as text by PostgreSQL. */
        return NULL;
    }
}
