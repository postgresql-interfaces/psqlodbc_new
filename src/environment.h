/*-------------------------------------------------------------------------
 *
 * environment.h
 *	  ODBC Environment handle management
 *
 * Copyright (c) 2026, Dave Cramer
 *
 *
 * IDENTIFICATION
 *	  src/environment.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PSQLODBC2_ENVIRONMENT_H
#define PSQLODBC2_ENVIRONMENT_H

#include "psqlodbc2.h"
#include "diagnostics.h"

/* Forward declaration — full definition is in connection.h */
struct OdbcConnection;

/* ---- Environment Handle Structure ----
 *
 * The magic_number field serves as a type tag so we can validate that a
 * SQLHANDLE actually points to an environment (guards against use-after-free
 * or passing the wrong handle type).
 */

#define ENVIRONMENT_MAGIC_NUMBER 0x454E5632  /* "ENV2" in ASCII */

/* Maximum number of connections that a single environment can track.
 * This limit is generous for typical application usage and avoids dynamic
 * allocation for what is essentially a bookkeeping list. */
#define MAX_CONNECTIONS_PER_ENVIRONMENT 128

typedef struct OdbcEnvironment {
    unsigned int magic_number;   /* Must equal ENVIRONMENT_MAGIC_NUMBER when valid */
    int          odbc_version;   /* ODBC version requested by the application (e.g., SQL_OV_ODBC3) */
    DiagnosticRecords diagnostics;
    struct OdbcConnection *connections[MAX_CONNECTIONS_PER_ENVIRONMENT];
    int connection_count;
} OdbcEnvironment;

/* ---- Public Interface ---- */

/*
 * Allocate a new environment handle and store it in *output_handle.
 * Returns SQL_SUCCESS on success, SQL_ERROR on allocation failure.
 */
SQLRETURN environment_allocate(SQLHANDLE *output_handle);

/*
 * Free a previously allocated environment handle.
 * Returns SQL_SUCCESS on success, SQL_INVALID_HANDLE if the handle is null
 * or does not have the expected magic number.
 * Returns SQL_ERROR with a diagnostic record if connections still exist
 * (they must be freed first per the ODBC spec).
 */
SQLRETURN environment_free(SQLHANDLE handle);

/*
 * Register a connection as a child of this environment.
 * Returns true on success, false if the connection array is full.
 */
bool environment_add_connection(OdbcEnvironment *environment,
                                struct OdbcConnection *connection);

/*
 * Unregister a connection from this environment.
 * Returns true if the connection was found and removed, false otherwise.
 */
bool environment_remove_connection(OdbcEnvironment *environment,
                                   struct OdbcConnection *connection);

#endif /* PSQLODBC2_ENVIRONMENT_H */
