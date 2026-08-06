/*-------------------------------------------------------------------------
 *
 * large_object.h
 *	  PostgreSQL large object client wrappers
 *
 * Copyright (c) 2026, Dave Cramer
 *
 *
 * IDENTIFICATION
 *	  src/large_object.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PSQLODBC2_LARGE_OBJECT_H
#define PSQLODBC2_LARGE_OBJECT_H

#include "psqlodbc2.h"   /* SQLRETURN and the ODBC SQL_* return codes */

#include <stddef.h>
#include <stdio.h>       /* SEEK_SET / SEEK_CUR / SEEK_END for large_object_seek */
#include <libpq-fe.h>
/* INV_READ / INV_WRITE inversion-mode flags live here. We include the header
 * rather than redefining the bit values so the constants stay in sync with
 * whatever libpq version we build against. */
#include <libpq/libpq-fs.h>

/* ---- Why large objects, and why these wrappers ----
 *
 * PostgreSQL stores binary blobs that exceed the practical size of a bytea
 * column as "large objects": rows in the system pg_largeobject table addressed
 * by an Oid. libpq exposes a POSIX-file-like client API (lo_creat, lo_open,
 * lo_read, lo_write, lo_lseek, lo_close) that talks to the server over the
 * fast-path protocol. The ODBC driver uses large objects to back
 * SQL_LONGVARBINARY parameters, including data streamed in chunks via
 * SQLPutData.
 *
 * These wrappers are deliberately thin: they exist to give the rest of the
 * driver descriptive, self-documenting names and one place to document the
 * transaction contract and error sentinels, rather than to add behavior. */

/* ---- Transaction contract (IMPORTANT) ----
 *
 * Every large object operation below MUST run inside an open transaction
 * block. libpq's lo_* functions use the fast-path protocol, and the server
 * requires large object descriptors (the int "fd" values) to live within a
 * single transaction — a descriptor opened in one transaction is invalid in
 * the next, and calling lo_* with no transaction open fails outright with
 * "invalid large-object descriptor" / "must be called inside a transaction".
 *
 * This module does NOT open or close the transaction itself; the caller owns
 * that so a whole create-write-close (or open-read-close) sequence shares one
 * transaction. On this project's connection, autocommit-OFF callers already
 * sit inside a transaction, and autocommit-ON callers must wrap the sequence
 * themselves. Use large_object_ensure_transaction() below to establish one in
 * a way that respects the connection's autocommit setting. */

/* Inversion access modes for large_object_open(). Aliased to libpq's INV_*
 * flags (from <libpq/libpq-fs.h>) so callers do not sprinkle raw bit values
 * (0x40000 / 0x20000) through the code. Combine with bitwise OR for
 * read/write access. */
#define LARGE_OBJECT_MODE_READ  INV_READ
#define LARGE_OBJECT_MODE_WRITE INV_WRITE

/* Convenience mode: a freshly created large object is opened for both reading
 * and writing, matching the mode lo_creat is invoked with. */
#define LARGE_OBJECT_MODE_READ_WRITE (INV_READ | INV_WRITE)

/* Forward declaration — full definition is in connection.h. Declared here so
 * large_object_ensure_transaction can take a connection handle without this
 * header depending on the whole connection module. */
struct OdbcConnection;
typedef struct OdbcConnection OdbcConnection;

/*
 * large_object_create
 *	  Create a new, empty large object and return its identifying Oid.
 *
 * Wraps libpq's lo_creat with LARGE_OBJECT_MODE_READ_WRITE so the object is
 * immediately usable for both reading and writing.
 *
 * connection: an open libpq connection sitting inside a transaction block.
 *
 * Returns the new large object's Oid on success, or InvalidOid (0) on failure.
 * The caller can inspect PQerrorMessage(connection) for the reason.
 */
Oid large_object_create(PGconn *connection);

/*
 * large_object_open
 *	  Open an existing large object and return a descriptor for it.
 *
 * Wraps libpq's lo_open.
 *
 * connection: an open libpq connection sitting inside a transaction block.
 * object_id:  the Oid returned by large_object_create (or otherwise known).
 * mode:       LARGE_OBJECT_MODE_READ, LARGE_OBJECT_MODE_WRITE, or their OR.
 *
 * Returns a non-negative descriptor ("fd") valid only for the lifetime of the
 * current transaction, or -1 on failure.
 */
int large_object_open(PGconn *connection, Oid object_id, int mode);

/*
 * large_object_write
 *	  Write a buffer to an open large object at its current position.
 *
 * Wraps libpq's lo_write.
 *
 * connection: an open libpq connection sitting inside a transaction block.
 * descriptor: an fd from large_object_open opened with write access.
 * buffer:     the bytes to write.
 * length:     number of bytes to write from buffer.
 *
 * Returns the number of bytes actually written on success, or -1 on failure.
 */
int large_object_write(PGconn *connection, int descriptor,
                       const char *buffer, size_t length);

/*
 * large_object_read
 *	  Read up to length bytes from an open large object at its current position.
 *
 * Wraps libpq's lo_read.
 *
 * connection: an open libpq connection sitting inside a transaction block.
 * descriptor: an fd from large_object_open opened with read access.
 * buffer:     destination for the bytes read; must hold at least length bytes.
 * length:     maximum number of bytes to read.
 *
 * Returns the number of bytes actually read (0 at end of object) on success,
 * or -1 on failure.
 */
int large_object_read(PGconn *connection, int descriptor,
                      char *buffer, size_t length);

/*
 * large_object_seek
 *	  Reposition the read/write offset of an open large object.
 *
 * Wraps libpq's lo_lseek. Useful for reading a large object back from the
 * start after writing it (seek to offset 0, whence SEEK_SET).
 *
 * connection: an open libpq connection sitting inside a transaction block.
 * descriptor: an fd from large_object_open.
 * offset:     signed byte offset relative to whence.
 * whence:     SEEK_SET, SEEK_CUR, or SEEK_END (from <stdio.h>).
 *
 * Returns the resulting absolute offset on success, or -1 on failure.
 */
int large_object_seek(PGconn *connection, int descriptor,
                      int offset, int whence);

/*
 * large_object_close
 *	  Close an open large object descriptor.
 *
 * Wraps libpq's lo_close. The descriptor is also implicitly closed when the
 * transaction ends, but closing explicitly frees the server-side descriptor
 * sooner.
 *
 * connection: an open libpq connection sitting inside a transaction block.
 * descriptor: an fd from large_object_open.
 *
 * Returns 0 on success, or -1 on failure.
 */
int large_object_close(PGconn *connection, int descriptor);

/*
 * large_object_ensure_transaction
 *	  Guarantee that a transaction block is open before large object work.
 *
 * Large object descriptors are only valid inside a transaction (see the
 * transaction contract above). This helper lets Phase 3 callers open a
 * transaction that respects the connection's autocommit setting without
 * duplicating that logic.
 *
 * Because autocommit-ON connections have no persistent transaction, this
 * helper begins an explicit one so a create/write/close sequence stays within
 * a single transaction; the caller is then responsible for committing it
 * (e.g. via the connection's commit path) once the sequence completes. On an
 * autocommit-OFF connection it reuses the already-open transaction.
 *
 * Returns SQL_SUCCESS if a transaction is open on return, or SQL_ERROR if one
 * could not be established.
 */
SQLRETURN large_object_ensure_transaction(OdbcConnection *connection);

#endif /* PSQLODBC2_LARGE_OBJECT_H */
