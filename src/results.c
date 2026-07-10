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
#include <libpq-fe.h>

/* ---- Internal Helpers ---- */

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
                                         unsigned int postgres_oid)
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
        /* For bytea columns, PostgreSQL returns hex format with a \x prefix
         * (e.g., "\x0102030405"). ODBC applications expect the raw hex digits
         * without the prefix. Detect and strip the \x prefix. */
        const char *char_value = effective_value;
        int char_length = effective_length;
        if (effective_length >= 2 && effective_value[0] == '\\' && effective_value[1] == 'x') {
            char_value = effective_value + 2;
            char_length = effective_length - 2;
        }

        SQLLEN actual_length = (SQLLEN)char_length;

        if (indicator_or_length) {
            *indicator_or_length = actual_length;
        }

        if (!target_value || buffer_length <= 0) {
            return SQL_SUCCESS;
        }

        if (actual_length < buffer_length) {
            memcpy(target_value, char_value, (size_t)actual_length);
            ((char *)target_value)[actual_length] = '\0';
            return SQL_SUCCESS;
        } else {
            /* Truncation — copy what fits and null-terminate */
            SQLLEN copy_length = buffer_length - 1;
            memcpy(target_value, char_value, (size_t)copy_length);
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
        /* Minimal SQL_C_WCHAR support: convert ASCII/Latin-1 chars to UTF-16LE.
         * Each source byte becomes a 2-byte UTF-16 code unit (works for BMP characters).
         * This covers the common case where PostgreSQL data is ASCII-compatible. */
        const char *src = effective_value;
        int src_len = effective_length;

        /* Number of SQLWCHAR (2-byte) characters needed */
        SQLLEN char_count = (SQLLEN)src_len;
        SQLLEN byte_count = char_count * (SQLLEN)sizeof(SQLWCHAR);

        if (indicator_or_length) {
            *indicator_or_length = byte_count;
        }

        if (!target_value || buffer_length <= 0) {
            return SQL_SUCCESS;
        }

        /* How many SQLWCHAR fit in the buffer (reserve room for null terminator) */
        SQLLEN max_chars = (buffer_length / (SQLLEN)sizeof(SQLWCHAR)) - 1;
        if (max_chars < 0) { max_chars = 0; }

        SQLLEN copy_chars = (char_count <= max_chars) ? char_count : max_chars;
        SQLWCHAR *dest = (SQLWCHAR *)target_value;

        for (SQLLEN i = 0; i < copy_chars; i++) {
            dest[i] = (SQLWCHAR)(unsigned char)src[i];
        }
        dest[copy_chars] = 0;  /* null terminator */

        if (char_count > max_chars) {
            diagnostics_add_record(&statement->diagnostics,
                                   "01004",
                                   0,
                                   "String data was truncated in SQLGetData (WCHAR).");
            return SQL_SUCCESS_WITH_INFO;
        }
        return SQL_SUCCESS;
    }

    case SQL_C_BINARY: {
        /* Return the raw text bytes as binary data */
        SQLLEN actual_length = (SQLLEN)effective_length;

        if (indicator_or_length) {
            *indicator_or_length = actual_length;
        }

        if (!target_value || buffer_length <= 0) {
            return SQL_SUCCESS;
        }

        if (actual_length <= buffer_length) {
            memcpy(target_value, effective_value, (size_t)actual_length);
            return SQL_SUCCESS;
        } else {
            memcpy(target_value, effective_value, (size_t)buffer_length);
            diagnostics_add_record(&statement->diagnostics,
                                   "01004",
                                   0,
                                   "Binary data was truncated in SQLGetData.");
            return SQL_SUCCESS_WITH_INFO;
        }
    }

    case SQL_C_INTERVAL_YEAR:
    case SQL_C_INTERVAL_MONTH:
    case SQL_C_INTERVAL_DAY:
    case SQL_C_INTERVAL_HOUR:
    case SQL_C_INTERVAL_MINUTE:
    case SQL_C_INTERVAL_SECOND: {
        /* Parse PostgreSQL interval text format (intervalstyle=postgres).
         * Examples: "10 years", "11 mons", "12 days", "1 year 2 mons 3 days 04:05:06"
         * We extract the component matching the requested interval type. */
        SQL_INTERVAL_STRUCT interval_value;
        memset(&interval_value, 0, sizeof(interval_value));
        interval_value.interval_sign = 0;

        /* Parse components from the interval text. PostgreSQL outputs space-separated
         * tokens like "N years", "N mons", "N days", and "HH:MM:SS" for time. */
        unsigned int years = 0, months = 0, days = 0;
        unsigned int hours = 0, minutes = 0, seconds = 0;
        const char *parse_cursor = effective_value;

        while (parse_cursor && *parse_cursor) {
            /* Skip leading whitespace */
            while (*parse_cursor == ' ') {
                parse_cursor++;
            }
            if (*parse_cursor == '\0') {
                break;
            }

            /* Try to parse a time component (HH:MM:SS) */
            unsigned int hh, mm, ss;
            int chars_consumed = 0;
            if (sscanf(parse_cursor, "%u:%u:%u%n", &hh, &mm, &ss, &chars_consumed) == 3 && chars_consumed > 0) {
                hours = hh;
                minutes = mm;
                seconds = ss;
                parse_cursor += chars_consumed;
                continue;
            }

            /* Try to parse a numeric value followed by a unit keyword */
            int numeric_value = 0;
            chars_consumed = 0;
            if (sscanf(parse_cursor, "%d%n", &numeric_value, &chars_consumed) == 1 && chars_consumed > 0) {
                parse_cursor += chars_consumed;

                /* Skip whitespace between number and unit */
                while (*parse_cursor == ' ') {
                    parse_cursor++;
                }

                /* Match unit keyword (case-insensitive prefix matching) */
                if (*parse_cursor == 'y' || *parse_cursor == 'Y') {
                    years = (unsigned int)(numeric_value >= 0 ? numeric_value : -numeric_value);
                    if (numeric_value < 0) interval_value.interval_sign = 1;
                } else if ((*parse_cursor == 'm' || *parse_cursor == 'M') &&
                           (*(parse_cursor + 1) == 'o' || *(parse_cursor + 1) == 'O')) {
                    months = (unsigned int)(numeric_value >= 0 ? numeric_value : -numeric_value);
                    if (numeric_value < 0) interval_value.interval_sign = 1;
                } else if (*parse_cursor == 'd' || *parse_cursor == 'D') {
                    days = (unsigned int)(numeric_value >= 0 ? numeric_value : -numeric_value);
                    if (numeric_value < 0) interval_value.interval_sign = 1;
                } else if ((*parse_cursor == 'h' || *parse_cursor == 'H')) {
                    hours = (unsigned int)(numeric_value >= 0 ? numeric_value : -numeric_value);
                    if (numeric_value < 0) interval_value.interval_sign = 1;
                } else if ((*parse_cursor == 'm' || *parse_cursor == 'M') &&
                           (*(parse_cursor + 1) == 'i' || *(parse_cursor + 1) == 'I')) {
                    minutes = (unsigned int)(numeric_value >= 0 ? numeric_value : -numeric_value);
                    if (numeric_value < 0) interval_value.interval_sign = 1;
                } else if (*parse_cursor == 's' || *parse_cursor == 'S') {
                    seconds = (unsigned int)(numeric_value >= 0 ? numeric_value : -numeric_value);
                    if (numeric_value < 0) interval_value.interval_sign = 1;
                }

                /* Advance past the unit word */
                while (*parse_cursor && *parse_cursor != ' ' && *parse_cursor != '\0') {
                    parse_cursor++;
                }
            } else {
                /* Skip unrecognized characters to avoid infinite loop */
                parse_cursor++;
            }
        }

        /* Fill in the interval struct based on the requested type */
        switch (target_type) {
        case SQL_C_INTERVAL_YEAR:
            interval_value.interval_type = SQL_IS_YEAR;
            interval_value.intval.year_month.year = years;
            break;
        case SQL_C_INTERVAL_MONTH:
            interval_value.interval_type = SQL_IS_MONTH;
            interval_value.intval.year_month.month = months;
            break;
        case SQL_C_INTERVAL_DAY:
            interval_value.interval_type = SQL_IS_DAY;
            interval_value.intval.day_second.day = days;
            break;
        case SQL_C_INTERVAL_HOUR:
            interval_value.interval_type = SQL_IS_HOUR;
            interval_value.intval.day_second.hour = hours;
            break;
        case SQL_C_INTERVAL_MINUTE:
            interval_value.interval_type = SQL_IS_MINUTE;
            interval_value.intval.day_second.minute = minutes;
            break;
        case SQL_C_INTERVAL_SECOND:
            interval_value.interval_type = SQL_IS_SECOND;
            interval_value.intval.day_second.second = seconds;
            break;
        default:
            break;
        }

        if (target_value) {
            memcpy(target_value, &interval_value, sizeof(SQL_INTERVAL_STRUCT));
        }
        if (indicator_or_length) {
            *indicator_or_length = (SQLLEN)sizeof(SQL_INTERVAL_STRUCT);
        }
        return SQL_SUCCESS;
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

        unsigned int col_oid = (unsigned int)PQftype(statement->current_result, column_index);
        SQLRETURN conversion_result = convert_value_to_c_type(
            statement, raw_value, raw_value_length,
            resolved_type, binding->target_buffer,
            binding->buffer_length, binding->indicator_or_length,
            col_oid);

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

    if (data_type) {
        *data_type = describe_bool_as_char
                         ? SQL_VARCHAR
                         : type_mapping_get_sql_type(postgres_oid);
    }

    /* Column size */
    if (column_size) {
        *column_size = describe_bool_as_char
                           ? PG_WIDTH_OF_BOOLS_AS_CHAR
                           : type_mapping_get_column_size(postgres_oid, type_modifier);
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

    unsigned int col_oid = (unsigned int)PQftype(statement->current_result, column_index);
    return convert_value_to_c_type(statement, raw_value, raw_value_length,
                                   resolved_type, target_value,
                                   buffer_length, indicator_or_length,
                                   col_oid);
}
