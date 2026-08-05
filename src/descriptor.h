/*-------------------------------------------------------------------------
 *
 * descriptor.h
 *	  ODBC descriptor handle management (ARD / APD / IRD / IPD)
 *
 * Copyright (c) 2026, Dave Cramer
 *
 *
 * IDENTIFICATION
 *	  src/descriptor.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PSQLODBC2_DESCRIPTOR_H
#define PSQLODBC2_DESCRIPTOR_H

#include "psqlodbc2.h"

#include <stdbool.h>

/* Forward declarations — the full definitions live in statement.h /
 * connection.h. A descriptor only ever holds pointers to these, so a forward
 * declaration avoids a circular include. */
struct OdbcStatement;
struct OdbcConnection;

/* Magic number for runtime type checking of descriptor handles.
 * "DSC2" in ASCII = 0x44534332. Stored in every descriptor (embedded or
 * explicitly allocated) and poisoned on free to catch use-after-free. */
#define DESCRIPTOR_MAGIC_NUMBER 0x44534332

/* Maximum length, including the null terminator, of a descriptor record's
 * SQL_DESC_NAME. Sized to comfortably hold PostgreSQL's NAMEDATALEN (63) column
 * and parameter identifiers. */
#define DESCRIPTOR_NAME_MAX 64

/* An ODBC statement owns four automatically-allocated ("implicit") descriptors,
 * one per role. The role decides which backing store a descriptor operation
 * reads from or writes to, and whether the descriptor is writable:
 *
 *   ARD (application row descriptor)   — how the app wants result columns
 *                                        delivered; backed by column_bindings.
 *   APD (application parameter descriptor) — how the app's parameter buffers are
 *                                        laid out; backed by parameter_bindings.
 *   IRD (implementation row descriptor) — read-only result-column metadata,
 *                                        derived from the executed result set.
 *   IPD (implementation parameter descriptor) — parameter metadata; the one
 *                                        writable field is SQL_DESC_NAME, used
 *                                        to name parameter markers.
 *
 * The enumerators keep their historical names (APP_ROW, IMPLICIT_PARAM) so
 * existing SQLSetDescField behavior that switches on the role is unchanged. */
typedef enum DescriptorRole {
    DESCRIPTOR_ROLE_APP_ROW,          /* ARD */
    DESCRIPTOR_ROLE_APP_PARAM,        /* APD */
    DESCRIPTOR_ROLE_IMPL_ROW,         /* IRD (read-only) */
    DESCRIPTOR_ROLE_IMPLICIT_PARAM    /* IPD */
} DescriptorRole;

/* One descriptor record: the ODBC-level description of a single result column
 * (row descriptors) or parameter (parameter descriptors). For the statement's
 * implicit ARD/APD/IPD these fields are NOT the authoritative store — the
 * statement's column_bindings / parameter_bindings arrays are (see the Notes in
 * specs/descriptors-and-array-binding.md) — so implicit descriptors leave this
 * array unallocated and route straight to those backing stores. An explicitly
 * allocated descriptor that is not yet attached to a statement has no backing
 * store, so it keeps its record values here instead. */
typedef struct DescriptorRecord {
    SQLSMALLINT concise_type;      /* SQL_DESC_TYPE / SQL_DESC_CONCISE_TYPE */
    SQLPOINTER data_ptr;           /* SQL_DESC_DATA_PTR */
    SQLLEN octet_length;           /* SQL_DESC_OCTET_LENGTH (buffer size in bytes) */
    SQLLEN length;                 /* SQL_DESC_LENGTH */
    SQLSMALLINT precision;         /* SQL_DESC_PRECISION */
    SQLSMALLINT scale;             /* SQL_DESC_SCALE */
    SQLLEN *indicator_ptr;         /* SQL_DESC_INDICATOR_PTR */
    SQLLEN *octet_length_ptr;      /* SQL_DESC_OCTET_LENGTH_PTR */
    SQLSMALLINT nullable;          /* SQL_DESC_NULLABLE (implementation descriptors) */
    char name[DESCRIPTOR_NAME_MAX]; /* SQL_DESC_NAME */
} DescriptorRecord;

/* ---- Descriptor Handle ----
 *
 * A descriptor is either:
 *   - Implicit: embedded by value in an OdbcStatement (one per role).
 *     is_explicit is false, owning_statement points at that statement, and all
 *     record data is read/written through the statement's backing stores.
 *   - Explicit: heap-allocated by SQLAllocHandle(SQL_HANDLE_DESC) and owned by a
 *     connection. is_explicit is true and owning_connection is set. Its role is
 *     not fixed until it is attached to a statement as the active ARD or APD via
 *     SQLSetStmtAttr, at which point owning_statement is set and operations
 *     route to that statement's backing store. While detached it stores field
 *     values in the heap-allocated records array. */
typedef struct OdbcDescriptor {
    unsigned int magic_number;          /* DESCRIPTOR_MAGIC_NUMBER when valid */
    DescriptorRole role;                /* Which descriptor role this handle plays */
    bool is_explicit;                   /* True if allocated via SQLAllocHandle */
    struct OdbcConnection *owning_connection; /* Set for explicit descriptors */
    struct OdbcStatement *owning_statement;   /* Statement whose backing store this
                                               * descriptor reads/writes; NULL for a
                                               * detached explicit descriptor */
    DescriptorRecord *records;          /* Field storage for a detached explicit
                                         * descriptor; NULL for implicit ones */
    int record_capacity;                /* Number of allocated entries in records */
    int record_count;                   /* Highest record number ever set (SQL_DESC_COUNT) */
} OdbcDescriptor;

/* ---- Public Interface ---- */

/*
 * Initialize a statement's embedded (implicit) descriptor for the given role.
 * Called once per role from statement_allocate. The descriptor routes all field
 * access to the owning statement's backing stores, so it needs no record array.
 */
void descriptor_init_implicit(OdbcDescriptor *descriptor,
                              DescriptorRole role,
                              struct OdbcStatement *statement);

/*
 * Allocate an explicit descriptor handle on a connection
 * (SQLAllocHandle(SQL_HANDLE_DESC)). The handle starts unattached: its role is
 * provisionally ARD and it becomes an active ARD or APD only when a statement
 * attaches it via SQLSetStmtAttr. Stores the new handle in *output_handle.
 * Returns SQL_SUCCESS on success, SQL_ERROR on allocation failure.
 */
SQLRETURN descriptor_allocate_explicit(struct OdbcConnection *connection,
                                       SQLHANDLE *output_handle);

/*
 * Free an explicitly allocated descriptor. Before releasing memory, every
 * statement on the owning connection that currently uses this descriptor as its
 * active ARD or APD is reverted to its own implicit descriptor, so no statement
 * is left holding a dangling pointer (the descriptors-free regression pins this
 * behavior). Returns SQL_SUCCESS, or SQL_INVALID_HANDLE if the handle is not a
 * valid explicit descriptor.
 */
SQLRETURN descriptor_free_explicit(OdbcDescriptor *descriptor);

/*
 * Set a single descriptor field (backs SQLSetDescField). Honors the fields ODBC
 * requires per role; unsupported-but-harmless fields are accepted and ignored so
 * applications need not special-case this driver.
 */
SQLRETURN descriptor_set_field(OdbcDescriptor *descriptor,
                               SQLSMALLINT record_number,
                               SQLSMALLINT field_identifier,
                               SQLPOINTER value,
                               SQLINTEGER buffer_length);

/*
 * Get a single descriptor field (backs SQLGetDescField).
 */
SQLRETURN descriptor_get_field(OdbcDescriptor *descriptor,
                               SQLSMALLINT record_number,
                               SQLSMALLINT field_identifier,
                               SQLPOINTER value,
                               SQLINTEGER buffer_length,
                               SQLINTEGER *string_length);

/*
 * Set the common fields of a descriptor record in one call (backs
 * SQLSetDescRec). On an ARD this binds a result column (write-through to
 * column_bindings) so a subsequent SQLFetch fills the buffer; on an APD it binds
 * a parameter. Rejected on the read-only IRD.
 */
SQLRETURN descriptor_set_rec(OdbcDescriptor *descriptor,
                             SQLSMALLINT record_number,
                             SQLSMALLINT type,
                             SQLSMALLINT sub_type,
                             SQLLEN length,
                             SQLSMALLINT precision,
                             SQLSMALLINT scale,
                             SQLPOINTER data_ptr,
                             SQLLEN *string_length_ptr,
                             SQLLEN *indicator_ptr);

/*
 * Read the common fields of a descriptor record in one call (backs
 * SQLGetDescRec). Name and Nullable are only meaningful on the implementation
 * descriptors (IRD/IPD) and are left untouched otherwise, matching the spec.
 */
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
                             SQLSMALLINT *nullable);

/*
 * Copy all descriptor records and fields from source to target (backs
 * SQLCopyDesc). The IRD is a valid source but never a valid target (it is
 * read-only). Returns SQL_SUCCESS, or SQL_ERROR with a diagnostic on the target
 * if the copy is not permitted.
 */
SQLRETURN descriptor_copy(OdbcDescriptor *source, OdbcDescriptor *target);

#endif /* PSQLODBC2_DESCRIPTOR_H */
