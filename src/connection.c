/*-------------------------------------------------------------------------
 *
 * connection.c
 *	  ODBC Connection handle lifecycle
 *
 * Copyright (c) 2026, Dave Cramer
 *
 *
 * IDENTIFICATION
 *	  src/connection.c
 *
 *-------------------------------------------------------------------------
 */
#include "connection.h"
#include "environment.h"
#include "connection_string.h"
#include "error_mapping.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

SQLRETURN connection_allocate(OdbcEnvironment *environment, SQLHANDLE *output_handle)
{
    if (!environment || !output_handle) {
        return SQL_ERROR;
    }

    OdbcConnection *connection = calloc(1, sizeof(OdbcConnection));
    if (!connection) {
        *output_handle = SQL_NULL_HDBC;
        return SQL_ERROR;
    }

    connection->magic_number = CONNECTION_MAGIC_NUMBER;
    connection->state = CONNECTION_STATE_NOT_CONNECTED;
    connection->parent_environment = environment;
    connection->autocommit = true;
    connection->transaction_state = TRANSACTION_STATE_IDLE;
    connection->txn_isolation = SQL_TXN_READ_COMMITTED;  /* PostgreSQL default */
    connection->login_timeout = 0;
    connection->connection_timeout = 0;
    connection->access_mode = SQL_MODE_READ_WRITE;

    if (!environment_add_connection(environment, connection)) {
        /* Environment's connection array is full */
        free(connection);
        *output_handle = SQL_NULL_HDBC;
        return SQL_ERROR;
    }

    *output_handle = (SQLHANDLE)connection;
    return SQL_SUCCESS;
}

SQLRETURN connection_free(SQLHANDLE handle)
{
    OdbcConnection *connection = (OdbcConnection *)handle;

    if (!connection) {
        return SQL_INVALID_HANDLE;
    }

    if (connection->magic_number != CONNECTION_MAGIC_NUMBER) {
        return SQL_INVALID_HANDLE;
    }

    /* ODBC spec: connection must be disconnected before it can be freed.
     * BROKEN state is also acceptable since the connection is already unusable. */
    if (connection->state == CONNECTION_STATE_CONNECTED ||
        connection->state == CONNECTION_STATE_EXECUTING) {
        diagnostics_clear(&connection->diagnostics);
        diagnostics_add_record(&connection->diagnostics,
                               "HY010",  /* Function sequence error */
                               0,
                               "Cannot free connection: still connected. Call SQLDisconnect first.");
        return SQL_ERROR;
    }

    /* All statement handles must be freed before the connection can be freed */
    if (connection->statement_count > 0) {
        diagnostics_clear(&connection->diagnostics);
        diagnostics_add_record(&connection->diagnostics,
                               "HY010",  /* Function sequence error */
                               0,
                               "Cannot free connection: statement handles still allocated. Free all statements first.");
        return SQL_ERROR;
    }

    /* Unlink from parent environment */
    if (connection->parent_environment) {
        environment_remove_connection(connection->parent_environment, connection);
    }

    /* Clean up all resources */
    diagnostics_clear(&connection->diagnostics);
    connection_info_clear(&connection->info);

    /* Poison the magic number to detect use-after-free */
    connection->magic_number = 0;
    free(connection);

    return SQL_SUCCESS;
}

SQLRETURN connection_connect(OdbcConnection *connection)
{
    if (!connection) {
        return SQL_ERROR;
    }

    if (connection->state == CONNECTION_STATE_CONNECTED) {
        diagnostics_clear(&connection->diagnostics);
        diagnostics_add_record(&connection->diagnostics,
                               "08002",  /* Connection name in use */
                               0,
                               "Already connected. Disconnect before reconnecting.");
        return SQL_ERROR;
    }

    /* Build keyword/value arrays for PQconnectdbParams. Using the array-based
     * API avoids quoting/escaping issues with values containing spaces,
     * single quotes, or backslashes. */
    const char *keywords[LIBPQ_MAX_PARAMS + 1];
    const char *values[LIBPQ_MAX_PARAMS + 1];
    int param_count = 0;

    connection_info_build_libpq_params(&connection->info, keywords, values, &param_count);

    /* Establish the connection via libpq */
    connection->libpq_connection = PQconnectdbParams(keywords, values, 0);

    if (PQstatus(connection->libpq_connection) != CONNECTION_OK) {
        /* Connection failed — extract error from the PGconn.
         * "08001" is the correct ODBC SQLSTATE for "client unable to establish
         * connection". PGconn doesn't carry a SQLSTATE field, but the error
         * message from PQerrorMessage provides the diagnostic detail. */
        diagnostics_clear(&connection->diagnostics);
        error_add_diagnostic_from_connection(&connection->diagnostics,
                                            connection->libpq_connection,
                                            "08001");

        /* Clean up the failed connection object from libpq */
        PQfinish(connection->libpq_connection);
        connection->libpq_connection = NULL;
        connection->state = CONNECTION_STATE_NOT_CONNECTED;
        return SQL_ERROR;
    }

    connection->state = CONNECTION_STATE_CONNECTED;

    /* Parse the server version for feature detection.
     * PQserverVersion returns an integer like 150002 for 15.0.2
     * (major * 10000 + minor * 100 + patch). */
    int version_number = PQserverVersion(connection->libpq_connection);
    connection->server_version_major = version_number / 10000;
    connection->server_version_minor = (version_number / 100) % 100;

    return SQL_SUCCESS;
}

SQLRETURN connection_disconnect(OdbcConnection *connection)
{
    if (!connection) {
        return SQL_ERROR;
    }

    if (connection->state != CONNECTION_STATE_CONNECTED &&
        connection->state != CONNECTION_STATE_EXECUTING) {
        diagnostics_clear(&connection->diagnostics);
        diagnostics_add_record(&connection->diagnostics,
                               "08003",  /* Connection does not exist */
                               0,
                               "Cannot disconnect: not currently connected.");
        return SQL_ERROR;
    }

    /* Close the libpq connection */
    if (connection->libpq_connection) {
        PQfinish(connection->libpq_connection);
        connection->libpq_connection = NULL;
    }

    connection->state = CONNECTION_STATE_NOT_CONNECTED;
    connection->server_version_major = 0;
    connection->server_version_minor = 0;

    return SQL_SUCCESS;
}

void connection_info_clear(ConnectionInfo *info)
{
    if (!info) {
        return;
    }

    /* Securely wipe password before freeing to prevent memory remnants */
    if (info->password) {
        size_t password_length = strlen(info->password);
        memset(info->password, 0, password_length);
        free(info->password);
        info->password = NULL;
    }

    /* Zero out all fixed-size fields */
    memset(info->server, 0, sizeof(info->server));
    memset(info->port, 0, sizeof(info->port));
    memset(info->database, 0, sizeof(info->database));
    memset(info->username, 0, sizeof(info->username));
    memset(info->sslmode, 0, sizeof(info->sslmode));
    memset(info->application_name, 0, sizeof(info->application_name));
    info->connect_timeout = 0;
}

bool connection_add_statement(OdbcConnection *connection,
                              struct OdbcStatement *statement)
{
    if (!connection || !statement) {
        return false;
    }

    if (connection->statement_count >= MAX_STATEMENTS_PER_CONNECTION) {
        return false;
    }

    /* Find the first empty slot */
    for (int index = 0; index < MAX_STATEMENTS_PER_CONNECTION; index++) {
        if (connection->statements[index] == NULL) {
            connection->statements[index] = statement;
            connection->statement_count++;
            return true;
        }
    }

    return false;
}

bool connection_remove_statement(OdbcConnection *connection,
                                 struct OdbcStatement *statement)
{
    if (!connection || !statement) {
        return false;
    }

    for (int index = 0; index < MAX_STATEMENTS_PER_CONNECTION; index++) {
        if (connection->statements[index] == statement) {
            connection->statements[index] = NULL;
            connection->statement_count--;
            return true;
        }
    }

    return false;
}

/* ---- Transaction Management ---- */

SQLRETURN connection_begin_transaction(OdbcConnection *connection)
{
    if (!connection) {
        return SQL_ERROR;
    }

    if (!connection->libpq_connection ||
        connection->state != CONNECTION_STATE_CONNECTED) {
        diagnostics_clear(&connection->diagnostics);
        diagnostics_add_record(&connection->diagnostics,
                               "08003",  /* Connection does not exist */
                               0,
                               "Cannot begin transaction: connection is not active.");
        return SQL_ERROR;
    }

    /* No-op if already in a transaction */
    if (connection->transaction_state == TRANSACTION_STATE_ACTIVE) {
        return SQL_SUCCESS;
    }

    PGresult *result = PQexec(connection->libpq_connection, "BEGIN");
    if (!result || PQresultStatus(result) != PGRES_COMMAND_OK) {
        diagnostics_clear(&connection->diagnostics);
        if (result) {
            error_add_diagnostic_from_result(&connection->diagnostics, result, "HY000");
            PQclear(result);
        } else {
            diagnostics_add_record(&connection->diagnostics,
                                   "08S01",  /* Communication link failure */
                                   0,
                                   "BEGIN command failed: NULL result from libpq.");
        }
        return SQL_ERROR;
    }

    PQclear(result);
    connection->transaction_state = TRANSACTION_STATE_ACTIVE;
    return SQL_SUCCESS;
}

SQLRETURN connection_commit(OdbcConnection *connection)
{
    if (!connection) {
        return SQL_ERROR;
    }

    /* Per ODBC spec: committing when no transaction is active is a no-op */
    if (connection->transaction_state == TRANSACTION_STATE_IDLE) {
        return SQL_SUCCESS;
    }

    if (!connection->libpq_connection ||
        connection->state != CONNECTION_STATE_CONNECTED) {
        diagnostics_clear(&connection->diagnostics);
        diagnostics_add_record(&connection->diagnostics,
                               "08003",  /* Connection does not exist */
                               0,
                               "Cannot commit: connection is not active.");
        return SQL_ERROR;
    }

    PGresult *result = PQexec(connection->libpq_connection, "COMMIT");
    if (!result || PQresultStatus(result) != PGRES_COMMAND_OK) {
        diagnostics_clear(&connection->diagnostics);
        if (result) {
            error_add_diagnostic_from_result(&connection->diagnostics, result, "HY000");
            PQclear(result);
        } else {
            diagnostics_add_record(&connection->diagnostics,
                                   "08S01",  /* Communication link failure */
                                   0,
                                   "COMMIT command failed: NULL result from libpq.");
        }
        /* Even if COMMIT fails, the transaction is over from PG's perspective */
        connection->transaction_state = TRANSACTION_STATE_IDLE;
        return SQL_ERROR;
    }

    PQclear(result);
    connection->transaction_state = TRANSACTION_STATE_IDLE;
    return SQL_SUCCESS;
}

SQLRETURN connection_rollback(OdbcConnection *connection)
{
    if (!connection) {
        return SQL_ERROR;
    }

    /* Per ODBC spec: rolling back when no transaction is active is a no-op */
    if (connection->transaction_state == TRANSACTION_STATE_IDLE) {
        return SQL_SUCCESS;
    }

    if (!connection->libpq_connection ||
        connection->state != CONNECTION_STATE_CONNECTED) {
        diagnostics_clear(&connection->diagnostics);
        diagnostics_add_record(&connection->diagnostics,
                               "08003",  /* Connection does not exist */
                               0,
                               "Cannot rollback: connection is not active.");
        return SQL_ERROR;
    }

    PGresult *result = PQexec(connection->libpq_connection, "ROLLBACK");
    if (!result || PQresultStatus(result) != PGRES_COMMAND_OK) {
        diagnostics_clear(&connection->diagnostics);
        if (result) {
            error_add_diagnostic_from_result(&connection->diagnostics, result, "HY000");
            PQclear(result);
        } else {
            diagnostics_add_record(&connection->diagnostics,
                                   "08S01",  /* Communication link failure */
                                   0,
                                   "ROLLBACK command failed: NULL result from libpq.");
        }
        /* Rollback failure still resets transaction state — PG aborts the txn */
        connection->transaction_state = TRANSACTION_STATE_IDLE;
        return SQL_ERROR;
    }

    PQclear(result);
    connection->transaction_state = TRANSACTION_STATE_IDLE;
    return SQL_SUCCESS;
}

SQLRETURN connection_ensure_transaction(OdbcConnection *connection)
{
    if (!connection) {
        return SQL_ERROR;
    }

    /* When autocommit is ON, each statement is its own transaction —
     * PostgreSQL handles this implicitly, no explicit BEGIN needed. */
    if (connection->autocommit) {
        return SQL_SUCCESS;
    }

    /* If we're already in a transaction (ACTIVE or FAILED), no BEGIN needed.
     * Note: FAILED state means the app must call SQLEndTran(ROLLBACK) before
     * executing more statements — the server will reject commands anyway. */
    if (connection->transaction_state != TRANSACTION_STATE_IDLE) {
        return SQL_SUCCESS;
    }

    return connection_begin_transaction(connection);
}
