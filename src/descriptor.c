/*-------------------------------------------------------------------------
 *
 * descriptor.c
 *	  ODBC descriptor handle management (ARD / APD / IRD / IPD)
 *
 * Copyright (c) 2026, Dave Cramer
 *
 * A descriptor is the ODBC-level description of a set of result columns (row
 * descriptors) or parameters (parameter descriptors). This driver keeps the
 * authoritative per-column / per-parameter data in the statement's existing
 * column_bindings and parameter_bindings arrays; the descriptors are thin views
 * over those backing stores (see specs/descriptors-and-array-binding.md). That
 * avoids duplicating the binding state and keeps SQLBindCol/SQLBindParameter and
 * the descriptor API operating on exactly the same data.
 *
 *   ARD  writes/reads column_bindings   (how the app receives result columns)
 *   APD  writes/reads parameter_bindings (how the app's parameter buffers look)
 *   IRD  reads executed-result metadata  (read-only; column types/sizes/names)
 *   IPD  reads/writes parameter names    (SQL_DESC_NAME on parameter markers)
 *
 * IDENTIFICATION
 *	  src/descriptor.c
 *
 *-------------------------------------------------------------------------
 */
#include "descriptor.h"
#include "statement.h"
#include "connection.h"
#include "results.h"
#include "type_mapping.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <libpq-fe.h>

/* An explicit descriptor is allocated before it is attached to any statement,
 * so it must be able to hold record data on its own. We size its record array
 * to the larger of the column and parameter limits so it can later serve as
 * either an ARD or an APD without reallocation. */
#define EXPLICIT_DESCRIPTOR_RECORD_CAPACITY \
    ((MAX_BOUND_COLUMNS > MAX_PARAMETERS) ? MAX_BOUND_COLUMNS : MAX_PARAMETERS)

/* ---- Initialization / lifecycle ---- */

void descriptor_init_implicit(OdbcDescriptor *descriptor,
                              DescriptorRole role,
                              OdbcStatement *statement)
{
    descriptor->magic_number = DESCRIPTOR_MAGIC_NUMBER;
    descriptor->role = role;
    descriptor->is_explicit = false;
    descriptor->owning_connection = NULL;
    descriptor->owning_statement = statement;
    /* Implicit descriptors route to the statement's backing stores and never
     * use their own record array. */
    descriptor->records = NULL;
    descriptor->record_capacity = 0;
    descriptor->record_count = 0;
}

SQLRETURN descriptor_allocate_explicit(OdbcConnection *connection,
                                       SQLHANDLE *output_handle)
{
    if (!connection || !output_handle) {
        return SQL_ERROR;
    }

    OdbcDescriptor *descriptor = calloc(1, sizeof(OdbcDescriptor));
    if (!descriptor) {
        *output_handle = SQL_NULL_HANDLE;
        return SQL_ERROR;
    }

    descriptor->records = calloc(EXPLICIT_DESCRIPTOR_RECORD_CAPACITY,
                                 sizeof(DescriptorRecord));
    if (!descriptor->records) {
        free(descriptor);
        *output_handle = SQL_NULL_HANDLE;
        return SQL_ERROR;
    }

    descriptor->magic_number = DESCRIPTOR_MAGIC_NUMBER;
    /* An unattached explicit descriptor has no fixed role yet; it becomes an
     * active ARD or APD when a statement attaches it. Default to ARD so field
     * access before attachment behaves like an application row descriptor. */
    descriptor->role = DESCRIPTOR_ROLE_APP_ROW;
    descriptor->is_explicit = true;
    descriptor->owning_connection = connection;
    descriptor->owning_statement = NULL;
    descriptor->record_capacity = EXPLICIT_DESCRIPTOR_RECORD_CAPACITY;
    descriptor->record_count = 0;

    *output_handle = (SQLHANDLE)descriptor;
    return SQL_SUCCESS;
}

SQLRETURN descriptor_free_explicit(OdbcDescriptor *descriptor)
{
    if (!descriptor || descriptor->magic_number != DESCRIPTOR_MAGIC_NUMBER ||
        !descriptor->is_explicit) {
        return SQL_INVALID_HANDLE;
    }

    /* Revert every statement that currently uses this descriptor back to its own
     * implicit ARD/APD, so freeing the handle never leaves a statement holding a
     * dangling pointer (pinned by the descriptors-free regression). */
    OdbcConnection *connection = descriptor->owning_connection;
    if (connection) {
        /* connection->statements[] is a SPARSE array: freeing a statement NULLs
         * its slot and decrements statement_count WITHOUT compacting, so
         * statement_count is a population count, not a high-water index. Iterate
         * the full capacity (the NULL guard below skips empty slots) or a
         * statement in a higher slot than the population count would be missed
         * and left holding a dangling active-descriptor pointer. Same pattern as
         * statement.c:2339. */
        for (int i = 0; i < MAX_STATEMENTS_PER_CONNECTION; i++) {
            OdbcStatement *statement = connection->statements[i];
            if (!statement) {
                continue;
            }
            if (statement->active_app_row_descriptor == descriptor) {
                statement->active_app_row_descriptor =
                    &statement->implicit_app_row_descriptor;
            }
            if (statement->active_app_param_descriptor == descriptor) {
                statement->active_app_param_descriptor =
                    &statement->implicit_app_param_descriptor;
            }
        }
    }

    descriptor->magic_number = 0;  /* Poison to catch use-after-free. */
    free(descriptor->records);
    free(descriptor);
    return SQL_SUCCESS;
}

/* ---- IRD (read-only result metadata) helpers ---- */

/*
 * SQL_DESC_OCTET_LENGTH for a result column as reported through the IRD. This is
 * the maximum buffer size, in bytes, needed to hold the column's value in its
 * default binary/struct C representation — which is DIFFERENT from the
 * transfer-octet length SQLColAttribute reports (that one is text-oriented and
 * returns 0 for fixed-width binary types). Mirrors the original psqlodbc IRD
 * buffer-length semantics:
 *   - 8-byte binary types (bigint, float8) report 8;
 *   - numeric(p,s) reports p + 2 (room for sign and decimal point);
 *   - character types report coef * column_size (client encoding max bytes per
 *     char times the declared length);
 *   - everything else (int4, int2, text without a bound, date/time, interval)
 *     reports 0, meaning "no fixed byte cap".
 * These are exactly the values the descrec regression expects (int4=0,
 * numeric(4,2)=6, varchar(10)=40, bigint=8).
 */
static SQLLEN ird_column_octet_length(const OdbcStatement *statement,
                                       unsigned int postgres_oid,
                                       int type_modifier)
{
    switch (postgres_oid) {
    case PG_TYPE_INT8:
    case PG_TYPE_FLOAT8:
        return 8;  /* sizeof(SQLBIGINT) / sizeof(SQLDOUBLE) */

    case PG_TYPE_NUMERIC: {
        /* Precision + 2 (sign and decimal point). Unconstrained numeric has no
         * declared precision, so report 0 rather than a made-up size. */
        SQLULEN precision = type_mapping_get_column_size(postgres_oid, type_modifier);
        return (precision > 0) ? (SQLLEN)(precision + 2) : 0;
    }

    case PG_TYPE_VARCHAR:
    case PG_TYPE_BPCHAR: {
        /* Declared character length times the client encoding's worst-case bytes
         * per character. An unbounded varchar/char (no typmod) has no size to
         * scale, so report 0. */
        if (type_modifier <= 4) {
            return 0;
        }
        SQLULEN character_length =
            type_mapping_get_column_size(postgres_oid, type_modifier);
        int bytes_per_char =
            (statement->parent_connection)
                ? statement->parent_connection->max_bytes_per_char : 1;
        if (bytes_per_char < 1) {
            bytes_per_char = 1;
        }
        return (SQLLEN)character_length * bytes_per_char;
    }

    default:
        /* Fixed-width narrow types and text-transferred types report no cap,
         * matching SQLColAttribute's transfer-octet result for them. */
        return 0;
    }
}

/*
 * Fill a DescriptorRecord for a 1-based IRD column from the executed result's
 * metadata, reusing the same type-mapping code path as SQLDescribeCol so the
 * reported type/precision/scale/nullability stay identical. Returns SQL_ERROR
 * (with a diagnostic on the statement) when there is no result set or the column
 * number is out of range; SQL_NO_DATA when the column is beyond the result but
 * within the record array (matches the reference's per-record probing).
 */
static SQLRETURN ird_describe_record(OdbcStatement *statement,
                                     SQLSMALLINT record_number,
                                     DescriptorRecord *out_record)
{
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

    int public_columns = statement_public_column_count(statement);
    if (public_columns == 0) {
        public_columns = PQnfields(metadata_source);
    }

    if (record_number < 1) {
        diagnostics_add_record(&statement->diagnostics,
                               "07009",  /* Invalid descriptor index */
                               0,
                               "Column number is out of range in the row descriptor.");
        return SQL_ERROR;
    }
    if (record_number > public_columns) {
        return SQL_NO_DATA;
    }

    int column_index = record_number - 1;

    /* Reuse the shared SQLDescribeCol path for type/size/scale/nullability so
     * the IRD and SQLDescribeCol never disagree. */
    SQLSMALLINT data_type = 0;
    SQLULEN column_size = 0;
    SQLSMALLINT decimal_digits = 0;
    SQLSMALLINT nullable = SQL_NULLABLE;
    SQLRETURN describe_result =
        results_describe_col(statement, (SQLUSMALLINT)record_number,
                             (SQLCHAR *)out_record->name,
                             (SQLSMALLINT)sizeof(out_record->name), NULL,
                             &data_type, &column_size, &decimal_digits, &nullable);
    if (describe_result == SQL_ERROR) {
        return SQL_ERROR;
    }

    unsigned int postgres_oid = (unsigned int)PQftype(metadata_source, column_index);
    int type_modifier = PQfmod(metadata_source, column_index);

    out_record->concise_type = data_type;
    /* Numeric precision/scale come straight from the type modifier; for
     * non-numeric types the reference reports 0 (the test expects PREC=0/SCALE=0
     * for int4/varchar/bigint), so only carry them for numeric. */
    if (postgres_oid == PG_TYPE_NUMERIC) {
        /* PostgreSQL caps NUMERIC precision at 1000, well within SQLSMALLINT's
         * range, so narrowing column_size (SQLULEN) here cannot overflow. */
        out_record->precision = (SQLSMALLINT)column_size;
        out_record->scale = decimal_digits;
    } else {
        out_record->precision = 0;
        out_record->scale = 0;
    }
    out_record->octet_length =
        ird_column_octet_length(statement, postgres_oid, type_modifier);
    out_record->nullable = nullable;
    return SQL_SUCCESS;
}

/* ---- Field get/set ---- */

/*
 * Bind (or unbind) a result column through the ARD, writing straight into the
 * statement's column_bindings backing store so a later SQLFetch fills the app
 * buffer. record_number is the 1-based column position.
 */
static SQLRETURN ard_bind_column(OdbcStatement *statement,
                                 SQLSMALLINT record_number,
                                 SQLSMALLINT c_type,
                                 SQLPOINTER data_ptr,
                                 SQLLEN octet_length,
                                 SQLLEN *indicator_ptr)
{
    if (record_number < 1 || record_number > MAX_BOUND_COLUMNS) {
        diagnostics_add_record(&statement->diagnostics,
                               "07009",  /* Invalid descriptor index */
                               0,
                               "Column number is out of range in the row descriptor.");
        return SQL_ERROR;
    }
    return column_binding_bind(statement->column_bindings,
                               &statement->bound_column_count,
                               (SQLUSMALLINT)record_number, c_type,
                               data_ptr, octet_length, indicator_ptr);
}

SQLRETURN descriptor_set_field(OdbcDescriptor *descriptor,
                               SQLSMALLINT record_number,
                               SQLSMALLINT field_identifier,
                               SQLPOINTER value,
                               SQLINTEGER buffer_length)
{
    if (!descriptor || descriptor->magic_number != DESCRIPTOR_MAGIC_NUMBER) {
        return SQL_INVALID_HANDLE;
    }

    OdbcStatement *statement = descriptor->owning_statement;

    /* A detached explicit descriptor has no backing store; keep the value in its
     * own record array so it round-trips (and is copied to a statement on
     * attach / SQLCopyDesc). */
    if (!statement) {
        if (record_number < 1 || record_number > descriptor->record_capacity) {
            return SQL_ERROR;
        }
        DescriptorRecord *record = &descriptor->records[record_number - 1];
        if (record_number > descriptor->record_count) {
            descriptor->record_count = record_number;
        }
        switch (field_identifier) {
        case SQL_DESC_TYPE:
        case SQL_DESC_CONCISE_TYPE:
            record->concise_type = (SQLSMALLINT)(intptr_t)value;
            break;
        case SQL_DESC_DATA_PTR:
            record->data_ptr = value;
            break;
        case SQL_DESC_OCTET_LENGTH:
            record->octet_length = (SQLLEN)(intptr_t)value;
            break;
        case SQL_DESC_LENGTH:
            record->length = (SQLLEN)(intptr_t)value;
            break;
        case SQL_DESC_PRECISION:
            record->precision = (SQLSMALLINT)(intptr_t)value;
            break;
        case SQL_DESC_SCALE:
            record->scale = (SQLSMALLINT)(intptr_t)value;
            break;
        case SQL_DESC_INDICATOR_PTR:
            record->indicator_ptr = (SQLLEN *)value;
            break;
        case SQL_DESC_OCTET_LENGTH_PTR:
            record->octet_length_ptr = (SQLLEN *)value;
            break;
        default:
            break;  /* Accept and ignore other fields. */
        }
        return SQL_SUCCESS;
    }

    switch (descriptor->role) {
    case DESCRIPTOR_ROLE_APP_ROW:
        /* On the ARD, SQL_DESC_PRECISION overrides the fractional-second
         * precision used when formatting an interval result column. This
         * behavior predates the descriptor module and must be preserved. The
         * stored value is later clamped to <= 9 at use, so an arbitrarily large
         * value here cannot overrun any buffer. */
        if (field_identifier == SQL_DESC_PRECISION) {
            if (record_number < 1 || record_number > MAX_BOUND_COLUMNS) {
                diagnostics_add_record(&statement->diagnostics, "07009", 0,
                    "Column number is out of range in SQLSetDescField.");
                return SQL_ERROR;
            }
            statement->column_precision_override[record_number - 1] =
                (int)(intptr_t)value;
            return SQL_SUCCESS;
        }
        /* Setting the data pointer through the ARD (re)binds the column. The
         * caller must set SQL_DESC_OCTET_LENGTH before or after; we read the
         * current binding's buffer length so either order works. */
        if (field_identifier == SQL_DESC_DATA_PTR) {
            if (record_number < 1 || record_number > MAX_BOUND_COLUMNS) {
                diagnostics_add_record(&statement->diagnostics, "07009", 0,
                    "Column number is out of range in SQLSetDescField.");
                return SQL_ERROR;
            }
            ColumnBinding *existing =
                &statement->column_bindings[record_number - 1];
            return ard_bind_column(statement, record_number,
                                   existing->target_type, value,
                                   existing->buffer_length,
                                   existing->indicator_or_length);
        }
        return SQL_SUCCESS;  /* Other ARD fields accepted and ignored. */

    case DESCRIPTOR_ROLE_APP_PARAM:
        /* Application parameter descriptor: Phase 1 exposes it so it can be
         * attached/detached and freed, but individual field sets are accepted
         * and ignored (parameter binding still goes through SQLBindParameter). */
        return SQL_SUCCESS;

    case DESCRIPTOR_ROLE_IMPL_ROW:
        /* The IRD is read-only. */
        diagnostics_add_record(&statement->diagnostics,
                               "HY091",  /* Invalid descriptor field identifier */
                               0,
                               "The implementation row descriptor is read-only.");
        return SQL_ERROR;

    case DESCRIPTOR_ROLE_IMPLICIT_PARAM:
        /* IPD: the one writable field is SQL_DESC_NAME, which names a parameter
         * marker so procedure calls can bind by name. This behavior predates the
         * descriptor module and must be preserved. */
        if (field_identifier != SQL_DESC_NAME) {
            return SQL_SUCCESS;
        }
        if (record_number < 1 || record_number > MAX_PARAMETERS) {
            diagnostics_add_record(&statement->diagnostics, "07009", 0,
                "Parameter number is out of range in SQLSetDescField.");
            return SQL_ERROR;
        }
        {
            ParameterBinding *binding =
                &statement->parameter_bindings[record_number - 1];
            if (!value) {
                binding->name[0] = '\0';
                return SQL_SUCCESS;
            }
            size_t name_length = (buffer_length == SQL_NTS)
                ? strlen((const char *)value)
                : (size_t)buffer_length;
            if (name_length >= sizeof(binding->name)) {
                name_length = sizeof(binding->name) - 1;
            }
            memcpy(binding->name, value, name_length);
            binding->name[name_length] = '\0';
        }
        return SQL_SUCCESS;
    }

    return SQL_SUCCESS;
}

/* Write a string field value into the application's buffer with ODBC
 * truncation semantics, reporting the untruncated length in *string_length. */
static SQLRETURN copy_string_field(const char *source,
                                   SQLPOINTER value,
                                   SQLINTEGER buffer_length,
                                   SQLINTEGER *string_length)
{
    SQLINTEGER source_length = (SQLINTEGER)strlen(source);
    if (string_length) {
        *string_length = source_length;
    }
    if (value && buffer_length > 0) {
        SQLINTEGER copy_length = source_length;
        if (copy_length >= buffer_length) {
            copy_length = buffer_length - 1;
        }
        memcpy(value, source, (size_t)copy_length);
        ((char *)value)[copy_length] = '\0';
    }
    return SQL_SUCCESS;
}

SQLRETURN descriptor_get_field(OdbcDescriptor *descriptor,
                               SQLSMALLINT record_number,
                               SQLSMALLINT field_identifier,
                               SQLPOINTER value,
                               SQLINTEGER buffer_length,
                               SQLINTEGER *string_length)
{
    if (!descriptor || descriptor->magic_number != DESCRIPTOR_MAGIC_NUMBER) {
        return SQL_INVALID_HANDLE;
    }

    OdbcStatement *statement = descriptor->owning_statement;

    /* Detached explicit descriptor: read back from its own record array. */
    if (!statement) {
        if (record_number < 1 || record_number > descriptor->record_capacity) {
            if (string_length) {
                *string_length = 0;
            }
            return SQL_ERROR;
        }
        DescriptorRecord *record = &descriptor->records[record_number - 1];
        switch (field_identifier) {
        case SQL_DESC_TYPE:
        case SQL_DESC_CONCISE_TYPE:
            if (value) {
                *(SQLSMALLINT *)value = record->concise_type;
            }
            break;
        case SQL_DESC_OCTET_LENGTH:
            if (value) {
                *(SQLLEN *)value = record->octet_length;
            }
            break;
        case SQL_DESC_PRECISION:
            if (value) {
                *(SQLSMALLINT *)value = record->precision;
            }
            break;
        case SQL_DESC_SCALE:
            if (value) {
                *(SQLSMALLINT *)value = record->scale;
            }
            break;
        case SQL_DESC_NAME:
            return copy_string_field(record->name, value, buffer_length,
                                     string_length);
        default:
            break;
        }
        return SQL_SUCCESS;
    }

    switch (descriptor->role) {
    case DESCRIPTOR_ROLE_IMPL_ROW: {
        DescriptorRecord record;
        memset(&record, 0, sizeof(record));
        SQLRETURN result = ird_describe_record(statement, record_number, &record);
        if (result != SQL_SUCCESS) {
            return result;
        }
        switch (field_identifier) {
        case SQL_DESC_TYPE:
        case SQL_DESC_CONCISE_TYPE:
            if (value) {
                *(SQLSMALLINT *)value = record.concise_type;
            }
            break;
        case SQL_DESC_OCTET_LENGTH:
            if (value) {
                *(SQLLEN *)value = record.octet_length;
            }
            break;
        case SQL_DESC_PRECISION:
            if (value) {
                *(SQLSMALLINT *)value = record.precision;
            }
            break;
        case SQL_DESC_SCALE:
            if (value) {
                *(SQLSMALLINT *)value = record.scale;
            }
            break;
        case SQL_DESC_NULLABLE:
            if (value) {
                *(SQLSMALLINT *)value = record.nullable;
            }
            break;
        case SQL_DESC_NAME:
            return copy_string_field(record.name, value, buffer_length,
                                     string_length);
        default:
            if (string_length) {
                *string_length = 0;
            }
            break;
        }
        return SQL_SUCCESS;
    }

    case DESCRIPTOR_ROLE_IMPLICIT_PARAM:
        /* IPD: only SQL_DESC_NAME is meaningful. */
        if (field_identifier != SQL_DESC_NAME) {
            if (string_length) {
                *string_length = 0;
            }
            return SQL_SUCCESS;
        }
        if (record_number < 1 || record_number > MAX_PARAMETERS) {
            return SQL_ERROR;
        }
        return copy_string_field(
            statement->parameter_bindings[record_number - 1].name,
            value, buffer_length, string_length);

    case DESCRIPTOR_ROLE_APP_ROW: {
        /* Report the current column binding's fields. */
        if (record_number < 1 || record_number > MAX_BOUND_COLUMNS) {
            if (string_length) {
                *string_length = 0;
            }
            return SQL_ERROR;
        }
        ColumnBinding *binding = &statement->column_bindings[record_number - 1];
        switch (field_identifier) {
        case SQL_DESC_TYPE:
        case SQL_DESC_CONCISE_TYPE:
            if (value) {
                *(SQLSMALLINT *)value = binding->target_type;
            }
            break;
        case SQL_DESC_DATA_PTR:
            if (value) {
                *(SQLPOINTER *)value = binding->target_buffer;
            }
            break;
        case SQL_DESC_OCTET_LENGTH:
            if (value) {
                *(SQLLEN *)value = binding->buffer_length;
            }
            break;
        case SQL_DESC_INDICATOR_PTR:
        case SQL_DESC_OCTET_LENGTH_PTR:
            if (value) {
                *(SQLLEN **)value = binding->indicator_or_length;
            }
            break;
        case SQL_DESC_PRECISION:
            if (value) {
                *(SQLSMALLINT *)value =
                    (SQLSMALLINT)statement->column_precision_override[record_number - 1];
            }
            break;
        default:
            if (string_length) {
                *string_length = 0;
            }
            break;
        }
        return SQL_SUCCESS;
    }

    case DESCRIPTOR_ROLE_APP_PARAM:
        /* Report the current parameter binding's fields. */
        if (record_number < 1 || record_number > MAX_PARAMETERS) {
            if (string_length) {
                *string_length = 0;
            }
            return SQL_ERROR;
        }
        {
            ParameterBinding *binding =
                &statement->parameter_bindings[record_number - 1];
            switch (field_identifier) {
            case SQL_DESC_TYPE:
            case SQL_DESC_CONCISE_TYPE:
                if (value) {
                    *(SQLSMALLINT *)value = binding->c_type;
                }
                break;
            case SQL_DESC_DATA_PTR:
                if (value) {
                    *(SQLPOINTER *)value = binding->value_buffer;
                }
                break;
            case SQL_DESC_OCTET_LENGTH:
                if (value) {
                    *(SQLLEN *)value = binding->buffer_length;
                }
                break;
            default:
                if (string_length) {
                    *string_length = 0;
                }
                break;
            }
        }
        return SQL_SUCCESS;
    }

    if (string_length) {
        *string_length = 0;
    }
    return SQL_SUCCESS;
}

/* ---- Record get/set (multi-field convenience) ---- */

SQLRETURN descriptor_set_rec(OdbcDescriptor *descriptor,
                             SQLSMALLINT record_number,
                             SQLSMALLINT type,
                             SQLSMALLINT sub_type,
                             SQLLEN length,
                             SQLSMALLINT precision,
                             SQLSMALLINT scale,
                             SQLPOINTER data_ptr,
                             SQLLEN *string_length_ptr,
                             SQLLEN *indicator_ptr)
{
    (void)sub_type;

    if (!descriptor || descriptor->magic_number != DESCRIPTOR_MAGIC_NUMBER) {
        return SQL_INVALID_HANDLE;
    }

    OdbcStatement *statement = descriptor->owning_statement;

    if (statement && descriptor->role == DESCRIPTOR_ROLE_IMPL_ROW) {
        diagnostics_add_record(&statement->diagnostics, "HY016", 0,
            "Cannot modify an implementation row descriptor.");
        return SQL_ERROR;
    }

    /* On the ARD, binding a record must write through to column_bindings so a
     * subsequent SQLFetch fills the application buffer — this is what the
     * descrec ARD-binding test relies on. */
    if (statement && descriptor->role == DESCRIPTOR_ROLE_APP_ROW) {
        SQLRETURN bind_result = ard_bind_column(statement, record_number, type,
                                                data_ptr, length, indicator_ptr);
        if (bind_result == SQL_ERROR) {
            return SQL_ERROR;
        }
        if (record_number >= 1 && record_number <= MAX_BOUND_COLUMNS) {
            statement->column_precision_override[record_number - 1] = precision;
        }
        return SQL_SUCCESS;
    }

    /* For every other descriptor, route each field through descriptor_set_field
     * so detached explicit descriptors and the IPD behave consistently. */
    descriptor_set_field(descriptor, record_number, SQL_DESC_TYPE,
                         (SQLPOINTER)(intptr_t)type, 0);
    descriptor_set_field(descriptor, record_number, SQL_DESC_OCTET_LENGTH,
                         (SQLPOINTER)(intptr_t)length, 0);
    descriptor_set_field(descriptor, record_number, SQL_DESC_PRECISION,
                         (SQLPOINTER)(intptr_t)precision, 0);
    descriptor_set_field(descriptor, record_number, SQL_DESC_SCALE,
                         (SQLPOINTER)(intptr_t)scale, 0);
    descriptor_set_field(descriptor, record_number, SQL_DESC_DATA_PTR, data_ptr, 0);
    descriptor_set_field(descriptor, record_number, SQL_DESC_INDICATOR_PTR,
                         indicator_ptr, 0);
    descriptor_set_field(descriptor, record_number, SQL_DESC_OCTET_LENGTH_PTR,
                         string_length_ptr, 0);
    return SQL_SUCCESS;
}

SQLRETURN descriptor_get_rec(OdbcDescriptor *descriptor,
                             SQLSMALLINT record_number,
                             SQLCHAR *name,
                             SQLSMALLINT name_buffer_length,
                             SQLSMALLINT *name_length,
                             SQLSMALLINT *type,
                             SQLSMALLINT *sub_type,
                             SQLLEN *length,
                             SQLSMALLINT *precision,
                             SQLSMALLINT *scale,
                             SQLSMALLINT *nullable)
{
    (void)sub_type;

    if (!descriptor || descriptor->magic_number != DESCRIPTOR_MAGIC_NUMBER) {
        return SQL_INVALID_HANDLE;
    }

    bool is_implementation_desc =
        descriptor->role == DESCRIPTOR_ROLE_IMPL_ROW ||
        descriptor->role == DESCRIPTOR_ROLE_IMPLICIT_PARAM;

    if (type) {
        SQLSMALLINT type_value = 0;
        SQLRETURN result = descriptor_get_field(descriptor, record_number,
                                                SQL_DESC_TYPE, &type_value, 0, NULL);
        if (result != SQL_SUCCESS) {
            return result;
        }
        *type = type_value;
    }
    if (length) {
        SQLLEN length_value = 0;
        SQLRETURN result = descriptor_get_field(descriptor, record_number,
                                                SQL_DESC_OCTET_LENGTH,
                                                &length_value, 0, NULL);
        if (result != SQL_SUCCESS) {
            return result;
        }
        *length = length_value;
    }
    if (precision) {
        SQLSMALLINT precision_value = 0;
        SQLRETURN result = descriptor_get_field(descriptor, record_number,
                                                SQL_DESC_PRECISION,
                                                &precision_value, 0, NULL);
        if (result != SQL_SUCCESS) {
            return result;
        }
        *precision = precision_value;
    }
    if (scale) {
        SQLSMALLINT scale_value = 0;
        SQLRETURN result = descriptor_get_field(descriptor, record_number,
                                                SQL_DESC_SCALE,
                                                &scale_value, 0, NULL);
        if (result != SQL_SUCCESS) {
            return result;
        }
        *scale = scale_value;
    }
    /* Nullable and Name are only meaningful for the implementation descriptors,
     * matching the reference PGAPI_GetDescRec. */
    if (nullable && is_implementation_desc) {
        SQLSMALLINT nullable_value = SQL_NULLABLE;
        SQLRETURN result = descriptor_get_field(descriptor, record_number,
                                                SQL_DESC_NULLABLE,
                                                &nullable_value, 0, NULL);
        if (result != SQL_SUCCESS) {
            return result;
        }
        *nullable = nullable_value;
    }
    if (name && is_implementation_desc) {
        SQLINTEGER string_length = 0;
        SQLRETURN result = descriptor_get_field(descriptor, record_number,
                                                SQL_DESC_NAME, name,
                                                name_buffer_length, &string_length);
        if (result != SQL_SUCCESS) {
            return result;
        }
        if (name_length) {
            *name_length = (SQLSMALLINT)string_length;
        }
    }
    return SQL_SUCCESS;
}

SQLRETURN descriptor_copy(OdbcDescriptor *source, OdbcDescriptor *target)
{
    if (!source || source->magic_number != DESCRIPTOR_MAGIC_NUMBER ||
        !target || target->magic_number != DESCRIPTOR_MAGIC_NUMBER) {
        return SQL_INVALID_HANDLE;
    }

    /* The IRD is read-only and can never be a copy target. */
    if (target->owning_statement &&
        target->role == DESCRIPTOR_ROLE_IMPL_ROW) {
        diagnostics_add_record(&target->owning_statement->diagnostics, "HY016", 0,
            "Cannot copy into an implementation row descriptor.");
        return SQL_ERROR;
    }

    /* Copy record by record through the field API so the target's backing store
     * (statement bindings, or its own record array) is updated correctly for
     * whatever role it currently plays. We copy up to the source's known record
     * count, or the full column range for an implicit source.
     * LIMITATION: an implicit ARD/APD source has no record_count, so we scan the
     * entire MAX_BOUND_COLUMNS range. That is acceptable for Phase 1 (SQLCopyDesc
     * is not on a hot path); a future pass could track a per-statement high-water
     * bound-record index to avoid the full walk. */
    int record_limit = source->records ? source->record_count : MAX_BOUND_COLUMNS;
    if (record_limit < 1) {
        record_limit = MAX_BOUND_COLUMNS;
    }

    for (int record_number = 1; record_number <= record_limit; record_number++) {
        SQLSMALLINT type = 0, precision = 0, scale = 0, nullable = 0;
        SQLLEN length = 0;
        SQLRETURN get_result = descriptor_get_rec(source, (SQLSMALLINT)record_number,
                                                  NULL, 0, NULL, &type, NULL,
                                                  &length, &precision, &scale,
                                                  &nullable);
        if (get_result == SQL_NO_DATA) {
            break;  /* Past the last defined source record. */
        }
        if (get_result != SQL_SUCCESS) {
            continue;
        }
        SQLPOINTER data_ptr = NULL;
        descriptor_get_field(source, (SQLSMALLINT)record_number, SQL_DESC_DATA_PTR,
                             &data_ptr, 0, NULL);
        descriptor_set_rec(target, (SQLSMALLINT)record_number, type, 0, length,
                           precision, scale, data_ptr, NULL, NULL);
    }
    return SQL_SUCCESS;
}
