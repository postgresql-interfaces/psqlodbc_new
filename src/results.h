/*-------------------------------------------------------------------------
 *
 * results.h
 *	  Result set function declarations
 *
 * Copyright (c) 2026, Dave Cramer
 *
 *
 * IDENTIFICATION
 *	  src/results.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PSQLODBC2_RESULTS_H
#define PSQLODBC2_RESULTS_H

#include "psqlodbc2.h"
#include "statement.h"

/*
 * Get the number of columns in the current result set.
 * Stores the count in *column_count.
 * Returns SQL_SUCCESS on success, SQL_ERROR if no result set exists.
 */
SQLRETURN results_num_result_cols(OdbcStatement *statement,
                                  SQLSMALLINT *column_count);

/*
 * Describe a single column in the result set: name, SQL type, size, scale,
 * and nullability. Column numbers are 1-based (matching ODBC convention).
 *
 * Any output pointer may be NULL if the caller doesn't need that field.
 * Returns SQL_SUCCESS on success, SQL_SUCCESS_WITH_INFO if the column name
 * was truncated, SQL_ERROR if no result set or invalid column number.
 */
SQLRETURN results_describe_col(OdbcStatement *statement,
                               SQLUSMALLINT column_number,
                               SQLCHAR *column_name,
                               SQLSMALLINT name_buffer_length,
                               SQLSMALLINT *name_length,
                               SQLSMALLINT *data_type,
                               SQLULEN *column_size,
                               SQLSMALLINT *decimal_digits,
                               SQLSMALLINT *nullable);

/*
 * Get the number of rows affected by the last INSERT/UPDATE/DELETE.
 * For SELECT statements, returns the number of rows in the result set.
 * Stores the count in *row_count.
 * Returns SQL_SUCCESS always.
 */
SQLRETURN results_row_count(OdbcStatement *statement, SQLLEN *row_count);

/*
 * Advance the cursor to the next row in the result set.
 * Returns SQL_SUCCESS if a row is available, SQL_NO_DATA if past the last
 * row, SQL_ERROR if no result set exists.
 */
SQLRETURN results_fetch(OdbcStatement *statement);

/*
 * Retrieve data for a single column from the current row.
 * Column numbers are 1-based. The value is converted from PostgreSQL's text
 * representation to the requested target C type.
 *
 * For NULL values, *indicator_or_length is set to SQL_NULL_DATA.
 * For string truncation, returns SQL_SUCCESS_WITH_INFO with SQLSTATE "01004".
 *
 * Returns SQL_SUCCESS, SQL_SUCCESS_WITH_INFO, or SQL_ERROR.
 */
SQLRETURN results_get_data(OdbcStatement *statement,
                           SQLUSMALLINT column_number,
                           SQLSMALLINT target_type,
                           SQLPOINTER target_value,
                           SQLLEN buffer_length,
                           SQLLEN *indicator_or_length);

/*
 * Advance to the next result set of a multi-statement query, making it the
 * current result for subsequent SQLNumResultCols / SQLDescribeCol / SQLFetch.
 *
 * Returns SQL_SUCCESS (or SQL_SUCCESS_WITH_INFO) when a next result set becomes
 * available, SQL_NO_DATA when there are no more result sets, and SQL_ERROR if
 * the next result set reported an execution error.
 */
SQLRETURN results_more_results(OdbcStatement *statement);

/*
 * Convert a single value from an arbitrary PGresult cell into the requested
 * ODBC C type and write it to target_value / indicator_or_length, using the
 * same conversion rules as SQLGetData. Exposed so the statement module can copy
 * procedure OUT/INOUT parameter values (which live in a call's result row) back
 * into application buffers with correct type conversion.
 *
 * NULL cells set *indicator_or_length to SQL_NULL_DATA. target_type may be
 * SQL_C_DEFAULT. Returns SQL_SUCCESS, SQL_SUCCESS_WITH_INFO (truncation), or
 * SQL_ERROR.
 */
SQLRETURN results_convert_column(OdbcStatement *statement,
                                 PGresult *result,
                                 int row_index,
                                 int column_index,
                                 SQLSMALLINT target_type,
                                 SQLPOINTER target_value,
                                 SQLLEN buffer_length,
                                 SQLLEN *indicator_or_length);

#endif /* PSQLODBC2_RESULTS_H */
