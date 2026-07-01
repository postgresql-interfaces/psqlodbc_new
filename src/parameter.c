/*-------------------------------------------------------------------------
 *
 * parameter.c
 *	  Parameter binding for SQLBindParameter
 *
 * Copyright (c) 2026, Dave Cramer
 *
 *
 * IDENTIFICATION
 *	  src/parameter.c
 *
 *-------------------------------------------------------------------------
 */
#include "parameter.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <inttypes.h>

/* Maximum buffer size for numeric-to-text conversions.
 * 64 bytes is sufficient for any integer or floating-point representation. */
#define NUMERIC_CONVERSION_BUFFER_SIZE 64

/* ---- Internal Helpers ---- */

/*
 * Convert a bound parameter value to a heap-allocated text string.
 * Returns NULL if the parameter represents SQL NULL (indicator is SQL_NULL_DATA).
 * Returns a malloc'd string on success (caller must free).
 * Sets *out_length to the byte length of the converted string.
 *
 * For SQL_C_CHAR: the buffer is treated as text and copied directly.
 * For numeric types: snprintf is used with appropriate format specifiers.
 * For SQL_C_BINARY: hex-encoded with PostgreSQL's bytea hex format (\x prefix).
 */
static char *convert_parameter_to_text(const ParameterBinding *binding, int *out_length)
{
    /* Check for NULL indicator */
    if (binding->indicator_or_length) {
        SQLLEN indicator_value = *binding->indicator_or_length;
        if (indicator_value == SQL_NULL_DATA) {
            *out_length = 0;
            return NULL;
        }
    }

    /* If the value buffer itself is NULL, treat as SQL NULL */
    if (!binding->value_buffer) {
        *out_length = 0;
        return NULL;
    }

    char conversion_buffer[NUMERIC_CONVERSION_BUFFER_SIZE];
    char *result = NULL;
    int length = 0;

    switch (binding->c_type) {
    case SQL_C_CHAR: {
        /* Determine the actual string length from the indicator/length value */
        size_t string_length;
        if (binding->indicator_or_length) {
            SQLLEN declared_length = *binding->indicator_or_length;
            if (declared_length == SQL_NTS) {
                string_length = strlen((const char *)binding->value_buffer);
            } else if (declared_length >= 0) {
                string_length = (size_t)declared_length;
            } else {
                /* Negative value other than SQL_NTS/SQL_NULL_DATA is invalid;
                 * fall back to treating as null-terminated */
                string_length = strlen((const char *)binding->value_buffer);
            }
        } else {
            /* No indicator pointer — assume null-terminated */
            string_length = strlen((const char *)binding->value_buffer);
        }

        result = malloc(string_length + 1);
        if (!result) {
            *out_length = 0;
            return NULL;
        }
        memcpy(result, binding->value_buffer, string_length);
        result[string_length] = '\0';
        *out_length = (int)string_length;
        return result;
    }

    case SQL_C_SLONG:
    case SQL_C_LONG:
        length = snprintf(conversion_buffer, sizeof(conversion_buffer),
                          "%d", *(const int32_t *)binding->value_buffer);
        break;

    case SQL_C_ULONG:
        length = snprintf(conversion_buffer, sizeof(conversion_buffer),
                          "%u", *(const uint32_t *)binding->value_buffer);
        break;

    case SQL_C_SSHORT:
    case SQL_C_SHORT:
        length = snprintf(conversion_buffer, sizeof(conversion_buffer),
                          "%hd", *(const int16_t *)binding->value_buffer);
        break;

    case SQL_C_USHORT:
        length = snprintf(conversion_buffer, sizeof(conversion_buffer),
                          "%hu", *(const uint16_t *)binding->value_buffer);
        break;

    case SQL_C_SBIGINT:
        length = snprintf(conversion_buffer, sizeof(conversion_buffer),
                          "%" PRId64, *(const int64_t *)binding->value_buffer);
        break;

    case SQL_C_UBIGINT:
        length = snprintf(conversion_buffer, sizeof(conversion_buffer),
                          "%" PRIu64, *(const uint64_t *)binding->value_buffer);
        break;

    case SQL_C_FLOAT:
        /* 9 significant digits is sufficient for SQLREAL (32-bit float) round-trip */
        length = snprintf(conversion_buffer, sizeof(conversion_buffer),
                          "%.9g", (double)*(const float *)binding->value_buffer);
        break;

    case SQL_C_DOUBLE:
        /* 17 significant digits is sufficient for SQLDOUBLE (64-bit) round-trip */
        length = snprintf(conversion_buffer, sizeof(conversion_buffer),
                          "%.17g", *(const double *)binding->value_buffer);
        break;

    case SQL_C_BIT:
        conversion_buffer[0] = (*(const unsigned char *)binding->value_buffer) ? '1' : '0';
        conversion_buffer[1] = '\0';
        length = 1;
        break;

    case SQL_C_STINYINT:
    case SQL_C_TINYINT:
        length = snprintf(conversion_buffer, sizeof(conversion_buffer),
                          "%d", (int)*(const int8_t *)binding->value_buffer);
        break;

    case SQL_C_UTINYINT:
        length = snprintf(conversion_buffer, sizeof(conversion_buffer),
                          "%u", (unsigned int)*(const uint8_t *)binding->value_buffer);
        break;

    case SQL_C_BINARY: {
        /* Encode as PostgreSQL hex bytea format: \x followed by hex pairs.
         * Determine the actual data length from the indicator. */
        size_t data_length;
        if (binding->indicator_or_length) {
            SQLLEN declared_length = *binding->indicator_or_length;
            if (declared_length >= 0) {
                data_length = (size_t)declared_length;
            } else {
                /* For binary, fall back to buffer_length if indicator is invalid */
                data_length = (size_t)binding->buffer_length;
            }
        } else {
            data_length = (size_t)binding->buffer_length;
        }

        /* Output format: \x followed by 2 hex chars per byte, plus null terminator */
        size_t hex_length = 2 + (data_length * 2);
        result = malloc(hex_length + 1);
        if (!result) {
            *out_length = 0;
            return NULL;
        }

        result[0] = '\\';
        result[1] = 'x';
        const unsigned char *binary_data = (const unsigned char *)binding->value_buffer;
        for (size_t byte_index = 0; byte_index < data_length; byte_index++) {
            snprintf(result + 2 + (byte_index * 2), 3, "%02x", binary_data[byte_index]);
        }
        result[hex_length] = '\0';
        *out_length = (int)hex_length;
        return result;
    }

    default:
        /* Unsupported C type — treat the buffer as a null-terminated string
         * as a best-effort fallback. Many ODBC applications pass SQL_C_DEFAULT
         * which should map to SQL_C_CHAR for character data. */
        length = (int)strlen((const char *)binding->value_buffer);
        result = malloc((size_t)length + 1);
        if (!result) {
            *out_length = 0;
            return NULL;
        }
        memcpy(result, binding->value_buffer, (size_t)length + 1);
        *out_length = length;
        return result;
    }

    /* Common path for numeric types that used conversion_buffer */
    if (length < 0) {
        /* snprintf encoding error — should not happen with valid data */
        *out_length = 0;
        return NULL;
    }

    result = malloc((size_t)length + 1);
    if (!result) {
        *out_length = 0;
        return NULL;
    }
    memcpy(result, conversion_buffer, (size_t)length + 1);
    *out_length = length;
    return result;
}

/* ---- Public Interface ---- */

SQLRETURN parameter_bind(ParameterBinding *bindings,
                         int *bound_count,
                         SQLUSMALLINT parameter_number,
                         SQLSMALLINT input_output_type,
                         SQLSMALLINT c_type,
                         SQLSMALLINT sql_type,
                         SQLULEN column_size,
                         SQLSMALLINT decimal_digits,
                         SQLPOINTER value_buffer,
                         SQLLEN buffer_length,
                         SQLLEN *indicator_or_length)
{
    if (parameter_number == 0 || parameter_number > MAX_PARAMETERS) {
        return SQL_ERROR;
    }

    /* Convert to zero-based index for array storage */
    int slot_index = parameter_number - 1;

    /* Track whether this is a new binding or a re-bind of an existing slot */
    bool was_previously_bound = bindings[slot_index].is_bound;

    bindings[slot_index].parameter_number = parameter_number;
    bindings[slot_index].input_output_type = input_output_type;
    bindings[slot_index].c_type = c_type;
    bindings[slot_index].sql_type = sql_type;
    bindings[slot_index].column_size = column_size;
    bindings[slot_index].decimal_digits = decimal_digits;
    bindings[slot_index].value_buffer = value_buffer;
    bindings[slot_index].buffer_length = buffer_length;
    bindings[slot_index].indicator_or_length = indicator_or_length;
    bindings[slot_index].is_bound = true;

    if (!was_previously_bound) {
        (*bound_count)++;
    }

    return SQL_SUCCESS;
}

void parameter_unbind_all(ParameterBinding *bindings, int *bound_count)
{
    memset(bindings, 0, sizeof(ParameterBinding) * MAX_PARAMETERS);
    *bound_count = 0;
}

SQLRETURN parameter_build_libpq_arrays(const ParameterBinding *bindings,
                                       int bound_count,
                                       const char ***out_values,
                                       int **out_lengths,
                                       int **out_formats,
                                       int *out_count)
{
    (void)bound_count;  /* Used only for the early-exit optimization below */

    /* Find the highest bound parameter number to determine array size.
     * We must provide entries for all positions up to the highest bound one,
     * even if intermediate positions are unbound (those become NULL). */
    int highest_bound_position = 0;
    for (int index = MAX_PARAMETERS - 1; index >= 0; index--) {
        if (bindings[index].is_bound) {
            highest_bound_position = index + 1;  /* Convert back to 1-based count */
            break;
        }
    }

    if (highest_bound_position == 0) {
        /* No parameters bound — return empty arrays */
        *out_values = NULL;
        *out_lengths = NULL;
        *out_formats = NULL;
        *out_count = 0;
        return SQL_SUCCESS;
    }

    int parameter_count = highest_bound_position;

    /* Allocate the three parallel arrays */
    const char **values = calloc((size_t)parameter_count, sizeof(const char *));
    int *lengths = calloc((size_t)parameter_count, sizeof(int));
    int *formats = calloc((size_t)parameter_count, sizeof(int));

    if (!values || !lengths || !formats) {
        free(values);
        free(lengths);
        free(formats);
        *out_values = NULL;
        *out_lengths = NULL;
        *out_formats = NULL;
        *out_count = 0;
        return SQL_ERROR;
    }

    /* Convert each bound parameter to its text representation */
    for (int index = 0; index < parameter_count; index++) {
        if (!bindings[index].is_bound) {
            /* Unbound parameter positions are sent as NULL to PostgreSQL */
            values[index] = NULL;
            lengths[index] = 0;
            formats[index] = 0;
            continue;
        }

        int value_length = 0;
        values[index] = convert_parameter_to_text(&bindings[index], &value_length);
        lengths[index] = value_length;
        formats[index] = 0;  /* Always text format */
    }

    *out_values = values;
    *out_lengths = lengths;
    *out_formats = formats;
    *out_count = parameter_count;
    return SQL_SUCCESS;
}

void parameter_free_libpq_arrays(const char **values,
                                 int *lengths,
                                 int *formats,
                                 int count)
{
    if (values) {
        for (int index = 0; index < count; index++) {
            /* Each non-NULL value string was heap-allocated by convert_parameter_to_text */
            free((void *)values[index]);
        }
        free(values);
    }
    free(lengths);
    free(formats);
}
