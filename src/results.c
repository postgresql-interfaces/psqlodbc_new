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

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <libpq-fe.h>

/* ---- Internal Helpers ---- */

/*
 * Convert a PostgreSQL text-format value to the requested ODBC C type and
 * write it to the target buffer. Sets *indicator_or_length appropriately.
 *
 * Returns SQL_SUCCESS, SQL_SUCCESS_WITH_INFO (truncation), or SQL_ERROR.
 */
static SQLRETURN convert_value_to_c_type(OdbcStatement *statement,
                                         const char *raw_value,
                                         int raw_value_length,
                                         SQLSMALLINT target_type,
                                         SQLPOINTER target_value,
                                         SQLLEN buffer_length,
                                         SQLLEN *indicator_or_length)
{
    switch (target_type) {
    case SQL_C_CHAR: {
        SQLLEN actual_length = (SQLLEN)raw_value_length;

        if (indicator_or_length) {
            *indicator_or_length = actual_length;
        }

        if (!target_value || buffer_length <= 0) {
            return SQL_SUCCESS;
        }

        if (actual_length < buffer_length) {
            memcpy(target_value, raw_value, (size_t)actual_length);
            ((char *)target_value)[actual_length] = '\0';
            return SQL_SUCCESS;
        } else {
            /* Truncation — copy what fits and null-terminate */
            SQLLEN copy_length = buffer_length - 1;
            memcpy(target_value, raw_value, (size_t)copy_length);
            ((char *)target_value)[copy_length] = '\0';
            diagnostics_add_record(&statement->diagnostics,
                                   "01004",  /* String data, right truncated */
                                   0,
                                   "String data was truncated in SQLGetData.");
            return SQL_SUCCESS_WITH_INFO;
        }
    }

    case SQL_C_SLONG:
    case SQL_C_LONG: {
        SQLINTEGER value = (SQLINTEGER)atol(raw_value);
        if (target_value) {
            *(SQLINTEGER *)target_value = value;
        }
        if (indicator_or_length) {
            *indicator_or_length = (SQLLEN)sizeof(SQLINTEGER);
        }
        return SQL_SUCCESS;
    }

    case SQL_C_ULONG: {
        SQLUINTEGER value = (SQLUINTEGER)strtoul(raw_value, NULL, 10);
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
        SQLSMALLINT value = (SQLSMALLINT)atoi(raw_value);
        if (target_value) {
            *(SQLSMALLINT *)target_value = value;
        }
        if (indicator_or_length) {
            *indicator_or_length = (SQLLEN)sizeof(SQLSMALLINT);
        }
        return SQL_SUCCESS;
    }

    case SQL_C_USHORT: {
        SQLUSMALLINT value = (SQLUSMALLINT)atoi(raw_value);
        if (target_value) {
            *(SQLUSMALLINT *)target_value = value;
        }
        if (indicator_or_length) {
            *indicator_or_length = (SQLLEN)sizeof(SQLUSMALLINT);
        }
        return SQL_SUCCESS;
    }

    case SQL_C_DOUBLE: {
        SQLDOUBLE value = strtod(raw_value, NULL);
        if (target_value) {
            *(SQLDOUBLE *)target_value = value;
        }
        if (indicator_or_length) {
            *indicator_or_length = (SQLLEN)sizeof(SQLDOUBLE);
        }
        return SQL_SUCCESS;
    }

    case SQL_C_FLOAT: {
        SQLREAL value = (SQLREAL)strtod(raw_value, NULL);
        if (target_value) {
            *(SQLREAL *)target_value = value;
        }
        if (indicator_or_length) {
            *indicator_or_length = (SQLLEN)sizeof(SQLREAL);
        }
        return SQL_SUCCESS;
    }

    case SQL_C_BIT: {
        /* PostgreSQL returns "t"/"f" for boolean, or "1"/"0" */
        unsigned char value = (raw_value[0] == 't' || raw_value[0] == '1') ? 1 : 0;
        if (target_value) {
            *(unsigned char *)target_value = value;
        }
        if (indicator_or_length) {
            *indicator_or_length = (SQLLEN)sizeof(unsigned char);
        }
        return SQL_SUCCESS;
    }

    case SQL_C_SBIGINT: {
        SQLBIGINT value = (SQLBIGINT)strtoll(raw_value, NULL, 10);
        if (target_value) {
            *(SQLBIGINT *)target_value = value;
        }
        if (indicator_or_length) {
            *indicator_or_length = (SQLLEN)sizeof(SQLBIGINT);
        }
        return SQL_SUCCESS;
    }

    case SQL_C_UBIGINT: {
        SQLUBIGINT value = (SQLUBIGINT)strtoull(raw_value, NULL, 10);
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
        signed char value = (signed char)atoi(raw_value);
        if (target_value) {
            *(signed char *)target_value = value;
        }
        if (indicator_or_length) {
            *indicator_or_length = (SQLLEN)sizeof(signed char);
        }
        return SQL_SUCCESS;
    }

    case SQL_C_BINARY: {
        /* Return the raw text bytes as binary data */
        SQLLEN actual_length = (SQLLEN)raw_value_length;

        if (indicator_or_length) {
            *indicator_or_length = actual_length;
        }

        if (!target_value || buffer_length <= 0) {
            return SQL_SUCCESS;
        }

        if (actual_length <= buffer_length) {
            memcpy(target_value, raw_value, (size_t)actual_length);
            return SQL_SUCCESS;
        } else {
            memcpy(target_value, raw_value, (size_t)buffer_length);
            diagnostics_add_record(&statement->diagnostics,
                                   "01004",
                                   0,
                                   "Binary data was truncated in SQLGetData.");
            return SQL_SUCCESS_WITH_INFO;
        }
    }

    default:
        diagnostics_add_record(&statement->diagnostics,
                               "HY003",  /* Program type out of range */
                               0,
                               "Unsupported target C type in SQLGetData.");
        return SQL_ERROR;
    }
}

/*
 * After advancing the cursor, write column values into all bound buffers.
 *
 * For each active column binding, retrieves the raw PG text value and converts
 * it using convert_value_to_c_type. NULL values set the indicator to SQL_NULL_DATA
 * without touching the buffer. If any conversion produces truncation, the overall
 * result is escalated to SQL_SUCCESS_WITH_INFO.
 *
 * Returns SQL_SUCCESS or SQL_SUCCESS_WITH_INFO.
 */
static SQLRETURN populate_bound_columns(OdbcStatement *statement)
{
    if (statement->bound_column_count == 0) {
        return SQL_SUCCESS;
    }

    int total_columns = PQnfields(statement->current_result);
    int row_index = statement->current_row_position;
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

        /* Handle NULL values: set indicator and skip buffer write */
        if (PQgetisnull(statement->current_result, row_index, column_index)) {
            if (binding->indicator_or_length) {
                *binding->indicator_or_length = SQL_NULL_DATA;
            }
            continue;
        }

        const char *raw_value = PQgetvalue(statement->current_result, row_index, column_index);
        int raw_value_length = PQgetlength(statement->current_result, row_index, column_index);

        /* Resolve SQL_C_DEFAULT to the concrete C type for this column's PG type */
        SQLSMALLINT resolved_type = binding->target_type;
        if (resolved_type == SQL_C_DEFAULT) {
            unsigned int postgres_oid = (unsigned int)PQftype(statement->current_result, column_index);
            SQLSMALLINT sql_type = type_mapping_get_sql_type(postgres_oid);
            resolved_type = type_mapping_get_default_c_type(sql_type);
        }

        SQLRETURN conversion_result = convert_value_to_c_type(
            statement, raw_value, raw_value_length,
            resolved_type, binding->target_buffer,
            binding->buffer_length, binding->indicator_or_length);

        /* Escalate overall result if any column was truncated */
        if (conversion_result == SQL_SUCCESS_WITH_INFO) {
            overall_result = SQL_SUCCESS_WITH_INFO;
        }
    }

    return overall_result;
}

/* ---- Public Interface ---- */

SQLRETURN results_num_result_cols(OdbcStatement *statement,
                                  SQLSMALLINT *column_count)
{
    if (!statement->current_result) {
        /* Per ODBC spec, SQLNumResultCols can be called after prepare
         * (before execute) for some drivers. We require execution first. */
        if (column_count) {
            *column_count = 0;
        }
        return SQL_SUCCESS;
    }

    if (column_count) {
        *column_count = (SQLSMALLINT)PQnfields(statement->current_result);
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
    if (!statement->current_result && !statement->has_result_set) {
        diagnostics_add_record(&statement->diagnostics,
                               "HY010",  /* Function sequence error */
                               0,
                               "No result set available. Execute a query first.");
        return SQL_ERROR;
    }

    int total_columns = PQnfields(statement->current_result);

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
    const char *pg_column_name = PQfname(statement->current_result, column_index);

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
    unsigned int postgres_oid = (unsigned int)PQftype(statement->current_result, column_index);
    int type_modifier = PQfmod(statement->current_result, column_index);

    if (data_type) {
        *data_type = type_mapping_get_sql_type(postgres_oid);
    }

    /* Column size */
    if (column_size) {
        *column_size = type_mapping_get_column_size(postgres_oid, type_modifier);
    }

    /* Decimal digits (scale) */
    if (decimal_digits) {
        *decimal_digits = type_mapping_get_decimal_digits(postgres_oid, type_modifier);
    }

    /* Nullability — we cannot determine this without querying pg_attribute,
     * which is expensive. Report unknown per ODBC convention. */
    if (nullable) {
        *nullable = SQL_NULLABLE_UNKNOWN;
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
        diagnostics_add_record(&statement->diagnostics,
                               "24000",  /* Invalid cursor state */
                               0,
                               "No result set available for fetching.");
        return SQL_ERROR;
    }

    int total_rows = PQntuples(statement->current_result);

    statement->current_row_position++;

    if (statement->current_row_position >= total_rows) {
        /* Past the last row — no more data */
        return SQL_NO_DATA;
    }

    /* Write column values into all bound application buffers.
     * If no columns are bound, this returns immediately (apps use SQLGetData instead). */
    return populate_bound_columns(statement);
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

    return convert_value_to_c_type(statement, raw_value, raw_value_length,
                                   resolved_type, target_value,
                                   buffer_length, indicator_or_length);
}
