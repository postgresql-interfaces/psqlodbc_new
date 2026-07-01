/*-------------------------------------------------------------------------
 *
 * column_binding.c
 *	  Column binding for SQLBindCol
 *
 * Copyright (c) 2026, Dave Cramer
 *
 *
 * IDENTIFICATION
 *	  src/column_binding.c
 *
 *-------------------------------------------------------------------------
 */
#include "column_binding.h"

#include <string.h>

SQLRETURN column_binding_bind(ColumnBinding *bindings,
                              int *bound_count,
                              SQLUSMALLINT column_number,
                              SQLSMALLINT target_type,
                              SQLPOINTER target_buffer,
                              SQLLEN buffer_length,
                              SQLLEN *indicator_or_length)
{
    /* Column numbers are 1-based; index into the array at [column_number - 1] */
    if (column_number < 1 || column_number > MAX_BOUND_COLUMNS) {
        return SQL_ERROR;
    }

    int slot_index = (int)(column_number - 1);
    ColumnBinding *slot = &bindings[slot_index];

    if (target_buffer == NULL) {
        /* NULL buffer means unbind this column */
        if (slot->is_bound) {
            slot->is_bound = false;
            (*bound_count)--;
        }
        /* Zero out the slot for cleanliness */
        memset(slot, 0, sizeof(ColumnBinding));
        return SQL_SUCCESS;
    }

    /* Store the binding — increment count only if this slot was not already bound */
    if (!slot->is_bound) {
        (*bound_count)++;
    }

    slot->column_number = column_number;
    slot->target_type = target_type;
    slot->target_buffer = target_buffer;
    slot->buffer_length = buffer_length;
    slot->indicator_or_length = indicator_or_length;
    slot->is_bound = true;

    return SQL_SUCCESS;
}

void column_binding_unbind_all(ColumnBinding *bindings, int *bound_count)
{
    memset(bindings, 0, sizeof(ColumnBinding) * MAX_BOUND_COLUMNS);
    *bound_count = 0;
}
