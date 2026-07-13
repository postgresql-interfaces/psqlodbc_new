/*-------------------------------------------------------------------------
 *
 * parameter.h
 *	  Parameter binding struct and declarations
 *
 * Copyright (c) 2026, Dave Cramer
 *
 *
 * IDENTIFICATION
 *	  src/parameter.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PSQLODBC2_PARAMETER_H
#define PSQLODBC2_PARAMETER_H

#include "psqlodbc2.h"
#include "query_parser.h"
#include <stdbool.h>

/* Maximum number of parameters supported per statement.
 * PostgreSQL itself supports up to 65535 parameters, but 256 covers
 * virtually all real-world queries and avoids excessive stack usage. */
#define MAX_PARAMETERS 256

/* ---- Parameter Binding Descriptor ----
 *
 * Stores the information provided by the application in SQLBindParameter.
 * The value_buffer and indicator_or_length are pointers into application memory
 * that are read at execution time (deferred access). */
typedef struct ParameterBinding {
    SQLUSMALLINT parameter_number;   /* 1-based parameter position */
    SQLSMALLINT input_output_type;   /* SQL_PARAM_INPUT, SQL_PARAM_OUTPUT, SQL_PARAM_INPUT_OUTPUT */
    SQLSMALLINT c_type;              /* C data type of the application buffer (SQL_C_SLONG, SQL_C_CHAR, etc.) */
    SQLSMALLINT sql_type;            /* SQL data type hint for the server (SQL_INTEGER, SQL_VARCHAR, etc.) */
    SQLULEN column_size;             /* Precision / column size for the SQL type */
    SQLSMALLINT decimal_digits;      /* Scale / decimal digits for the SQL type */
    SQLPOINTER value_buffer;         /* Pointer to the application's data buffer (read at execute time) */
    SQLLEN buffer_length;            /* Size of the application's data buffer in bytes */
    SQLLEN *indicator_or_length;     /* Pointer to length/indicator variable (SQL_NULL_DATA, SQL_NTS, or byte count) */
    bool is_bound;                   /* True if this slot has been bound by the application */

    /* Optional parameter name, set via SQLSetDescField(IPD, SQL_DESC_NAME).
     * Used for procedure calls to emit named notation ("name" := value) and to
     * match OUT/INOUT result columns by name. Empty string when unnamed. */
    char name[64];
} ParameterBinding;

/* ---- Parameter Management Functions ---- */

/*
 * Store a parameter binding at the given position.
 *
 * The binding is stored in bindings[parameter_number - 1]. If this slot was
 * not previously bound, *bound_count is incremented.
 *
 * Returns SQL_SUCCESS on success, SQL_ERROR if parameter_number is out of range.
 */
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
                         SQLLEN *indicator_or_length);

/*
 * Clear all parameter bindings, resetting the array and count to zero.
 */
void parameter_unbind_all(ParameterBinding *bindings, int *bound_count);

/*
 * Convert a single bound parameter value to a heap-allocated PostgreSQL text
 * string (caller frees). Returns NULL for SQL NULL. Sets *out_length to the
 * byte length. Exposed so the procedure-call executor can build one argument
 * value at a time.
 */
char *convert_parameter_to_text(const ParameterBinding *binding, int *out_length);

/*
 * Build the parallel arrays required by PQexecPrepared / PQexecParams.
 *
 * Reads the current values from bound application buffers, converts them to
 * PostgreSQL text format strings, and allocates the output arrays.
 *
 * The caller must free the arrays with parameter_free_libpq_arrays() after use.
 *
 * Parameters:
 *   bindings    - The parameter binding array (MAX_PARAMETERS elements)
 *   bound_count - Number of parameters that have been bound
 *   out_values  - Output: array of text-format value strings (NULL for SQL NULL)
 *   out_lengths - Output: array of value lengths in bytes
 *   out_formats - Output: array of format codes (always 0 = text)
 *   out_count   - Output: number of parameters in the arrays
 *
 * Returns SQL_SUCCESS on success, SQL_ERROR on memory allocation failure.
 */
SQLRETURN parameter_build_libpq_arrays(const ParameterBinding *bindings,
                                       int bound_count,
                                       const char ***out_values,
                                       int **out_lengths,
                                       int **out_formats,
                                       int *out_count);

/*
 * Free the arrays allocated by parameter_build_libpq_arrays.
 * Safe to call with NULL pointers (no-op in that case).
 */
void parameter_free_libpq_arrays(const char **values,
                                 int *lengths,
                                 int *formats,
                                 int count);

/* PostgreSQL pseudo-type OID used when binding procedure-call arguments.
 * "unknown" lets PostgreSQL resolve function overloads from context. It is a
 * stable built-in OID. */
#define PG_OID_UNKNOWN 705

#endif /* PSQLODBC2_PARAMETER_H */
