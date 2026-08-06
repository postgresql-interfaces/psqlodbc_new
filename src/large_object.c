/*-------------------------------------------------------------------------
 *
 * large_object.c
 *	  PostgreSQL large object client wrappers
 *
 * Copyright (c) 2026, Dave Cramer
 *
 *
 * IDENTIFICATION
 *	  src/large_object.c
 *
 *-------------------------------------------------------------------------
 */
#include "large_object.h"
#include "connection.h"

#include <limits.h>

/*
 * Each wrapper forwards directly to the matching libpq lo_* function. The
 * value of the wrappers is naming, documentation, and a single choke point for
 * the transaction contract — not additional behavior. libpq already reports
 * failures through its documented sentinels (InvalidOid for lo_creat, -1 for
 * the descriptor/byte-count/offset returning calls), which we pass through
 * unchanged so callers can map them to ODBC diagnostics.
 */

Oid large_object_create(PGconn *connection)
{
    /* Create the object readable and writable so a caller can write it now and
     * read it back within the same transaction without reopening. */
    return lo_creat(connection, LARGE_OBJECT_MODE_READ_WRITE);
}

int large_object_open(PGconn *connection, Oid object_id, int mode)
{
    return lo_open(connection, object_id, mode);
}

int large_object_write(PGconn *connection, int descriptor,
                       const char *buffer, size_t length)
{
    /* libpq's lo_write carries the byte count as an int on the fast-path wire.
     * A size_t length above INT_MAX would silently truncate to a negative or
     * wrong value and corrupt the object, so reject it up front as a failure. */
    if (length > INT_MAX) {
        return -1;
    }
    return lo_write(connection, descriptor, buffer, length);
}

int large_object_read(PGconn *connection, int descriptor,
                      char *buffer, size_t length)
{
    /* lo_read likewise reports the byte count through an int; a request larger
     * than INT_MAX cannot be represented in the return value, so reject it
     * rather than let the count wrap. Callers should read in bounded chunks. */
    if (length > INT_MAX) {
        return -1;
    }
    return lo_read(connection, descriptor, buffer, length);
}

int large_object_seek(PGconn *connection, int descriptor,
                      int offset, int whence)
{
    return lo_lseek(connection, descriptor, offset, whence);
}

int large_object_close(PGconn *connection, int descriptor)
{
    return lo_close(connection, descriptor);
}

SQLRETURN large_object_ensure_transaction(OdbcConnection *connection)
{
    if (!connection) {
        return SQL_ERROR;
    }

    /* connection_ensure_transaction opens a transaction only when autocommit
     * is OFF; an autocommit-ON connection would otherwise run each lo_* call in
     * its own implicit transaction, invalidating the descriptor between calls.
     * For large objects we therefore force an explicit transaction regardless
     * of autocommit so the whole create/write/read/close sequence shares one.
     * The caller commits it (via the connection's commit path) once done. */
    if (connection->autocommit) {
        return connection_begin_transaction(connection);
    }

    return connection_ensure_transaction(connection);
}
