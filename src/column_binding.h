/*-------------------------------------------------------------------------
 *
 * column_binding.h
 *	  Column binding struct and function declarations
 *
 * Copyright (c) 2026, Dave Cramer
 *
 *
 * IDENTIFICATION
 *	  src/column_binding.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PSQLODBC2_COLUMN_BINDING_H
#define PSQLODBC2_COLUMN_BINDING_H

#include "psqlodbc2.h"
#include <stdbool.h>

/* Maximum number of columns that can be bound simultaneously.
 * This covers virtually all real-world result sets. If a query returns more
 * columns than this, the extra columns can still be read via SQLGetData. */
#define MAX_BOUND_COLUMNS 256

/* ---- Column Binding Descriptor ----
 *
 * Stores the information provided by the application in SQLBindCol.
 * The target_buffer and indicator_or_length are pointers into application memory
 * that are written to during SQLFetch (immediate write, not deferred). */
typedef struct ColumnBinding {
    SQLUSMALLINT column_number;      /* 1-based column position in the result set */
    SQLSMALLINT target_type;         /* C data type for conversion (SQL_C_CHAR, SQL_C_SLONG, etc.) */
    SQLPOINTER target_buffer;        /* Pointer to application's receive buffer */
    SQLLEN buffer_length;            /* Size of target_buffer in bytes */
    SQLLEN *indicator_or_length;     /* Output: receives data length or SQL_NULL_DATA */
    bool is_bound;                   /* True if this slot is actively bound */
} ColumnBinding;

/* ---- Column Binding Management Functions ---- */

/*
 * Bind a result set column to an application buffer.
 *
 * The binding is stored at bindings[column_number - 1]. If target_buffer is
 * NULL, the column is unbound instead. If this slot was not previously bound,
 * *bound_count is incremented (or decremented on unbind).
 *
 * Returns SQL_SUCCESS on success, SQL_ERROR if column_number is out of range.
 */
SQLRETURN column_binding_bind(ColumnBinding *bindings,
                              int *bound_count,
                              SQLUSMALLINT column_number,
                              SQLSMALLINT target_type,
                              SQLPOINTER target_buffer,
                              SQLLEN buffer_length,
                              SQLLEN *indicator_or_length);

/*
 * Clear all column bindings (implements SQLFreeStmt SQL_UNBIND).
 * Resets every slot to unbound and sets *bound_count to 0.
 */
void column_binding_unbind_all(ColumnBinding *bindings, int *bound_count);

#endif /* PSQLODBC2_COLUMN_BINDING_H */
