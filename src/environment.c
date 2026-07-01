/*-------------------------------------------------------------------------
 *
 * environment.c
 *	  ODBC Environment handle implementation
 *
 * Copyright (c) 2026, Dave Cramer
 *
 *
 * IDENTIFICATION
 *	  src/environment.c
 *
 *-------------------------------------------------------------------------
 */
#include "environment.h"

#include <stdlib.h>
#include <string.h>

SQLRETURN environment_allocate(SQLHANDLE *output_handle)
{
    OdbcEnvironment *environment = NULL;

    if (!output_handle) {
        return SQL_ERROR;
    }

    environment = malloc(sizeof(OdbcEnvironment));
    if (!environment) {
        *output_handle = SQL_NULL_HENV;
        return SQL_ERROR;
    }

    /* Zero-initialize and then set the type tag so we can validate this
     * handle in future calls. */
    memset(environment, 0, sizeof(OdbcEnvironment));
    environment->magic_number = ENVIRONMENT_MAGIC_NUMBER;

    *output_handle = (SQLHANDLE)environment;
    return SQL_SUCCESS;
}

SQLRETURN environment_free(SQLHANDLE handle)
{
    OdbcEnvironment *environment = (OdbcEnvironment *)handle;

    if (!environment) {
        return SQL_INVALID_HANDLE;
    }

    if (environment->magic_number != ENVIRONMENT_MAGIC_NUMBER) {
        /* The handle doesn't look like a valid environment — either it was
         * already freed, or the caller passed the wrong handle type. */
        return SQL_INVALID_HANDLE;
    }

    /* ODBC spec requires that all child connections be freed before the
     * environment can be freed. Report an error if connections still exist. */
    if (environment->connection_count > 0) {
        diagnostics_clear(&environment->diagnostics);
        diagnostics_add_record(&environment->diagnostics,
                               "HY010",  /* Function sequence error */
                               0,
                               "Cannot free environment: connections are still allocated");
        return SQL_ERROR;
    }

    /* Clean up diagnostic records (frees any heap-allocated message strings) */
    diagnostics_clear(&environment->diagnostics);

    /* Clear the magic number before freeing to poison use-after-free scenarios */
    environment->magic_number = 0;
    free(environment);

    return SQL_SUCCESS;
}

bool environment_add_connection(OdbcEnvironment *environment,
                                struct OdbcConnection *connection)
{
    if (!environment || !connection) {
        return false;
    }

    if (environment->connection_count >= MAX_CONNECTIONS_PER_ENVIRONMENT) {
        return false;
    }

    environment->connections[environment->connection_count] = connection;
    environment->connection_count++;
    return true;
}

bool environment_remove_connection(OdbcEnvironment *environment,
                                   struct OdbcConnection *connection)
{
    if (!environment || !connection) {
        return false;
    }

    /* Find the connection in the array and remove it by swapping with the
     * last element. Order doesn't matter for this list. */
    for (int index = 0; index < environment->connection_count; index++) {
        if (environment->connections[index] == connection) {
            environment->connection_count--;
            environment->connections[index] =
                environment->connections[environment->connection_count];
            environment->connections[environment->connection_count] = NULL;
            return true;
        }
    }

    return false;
}
