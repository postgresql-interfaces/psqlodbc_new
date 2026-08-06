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
#include "type_mapping.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/*
 * Return the maximum number of bytes a single character can occupy in the
 * given PostgreSQL client encoding name. Used to translate a column's
 * character-count size into a worst-case octet (byte) length.
 *
 * Only the multibyte encodings that differ from 1 are enumerated; anything
 * unrecognized (including all single-byte encodings such as LATIN1 and SQL_ASCII)
 * defaults to 1. Values match PostgreSQL's pg_wchar.h maxmblen table.
 */
static int max_bytes_per_char_for_encoding(const char *encoding_name)
{
    if (!encoding_name) {
        return 1;
    }
    /* UTF-8 is by far the common case and allows up to 4 bytes per character. */
    if (strcmp(encoding_name, "UTF8") == 0) {
        return 4;
    }
    /* Encodings that use up to 4 bytes per character. */
    if (strcmp(encoding_name, "GB18030") == 0) {
        return 4;
    }
    /* Encodings that use up to 3 bytes per character. */
    if (strcmp(encoding_name, "EUC_JP") == 0 ||
        strcmp(encoding_name, "EUC_CN") == 0 ||
        strcmp(encoding_name, "EUC_KR") == 0 ||
        strcmp(encoding_name, "EUC_TW") == 0 ||
        strcmp(encoding_name, "EUC_JIS_2004") == 0 ||
        strcmp(encoding_name, "MULE_INTERNAL") == 0) {
        return 3;
    }
    /* Encodings that use up to 2 bytes per character. */
    if (strcmp(encoding_name, "SJIS") == 0 ||
        strcmp(encoding_name, "BIG5") == 0 ||
        strcmp(encoding_name, "GBK") == 0 ||
        strcmp(encoding_name, "UHC") == 0 ||
        strcmp(encoding_name, "JOHAB") == 0 ||
        strcmp(encoding_name, "SHIFT_JIS_2004") == 0) {
        return 2;
    }
    return 1;
}

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

    /* Apply the driver's default connection options (BoolsAsChar on, size
     * reporting, etc.) before any connection string is parsed. calloc zeroed the
     * struct, which is NOT the correct default for every field — notably
     * bools_as_char must default to true — so initialize it explicitly here.
     * SQLDriverConnect/SQLConnect then overlay only the keywords the application
     * supplies. */
    connection_info_clear(&connection->info);
    connection->transaction_state = TRANSACTION_STATE_IDLE;
    connection->txn_isolation = SQL_TXN_READ_COMMITTED;  /* PostgreSQL default */
    connection->login_timeout = 0;
    connection->connection_timeout = 0;
    connection->access_mode = SQL_MODE_READ_WRITE;
    connection->batch_size = DEFAULT_BATCH_SIZE;

    /* No "lo" large-object domain is known until connect time discovers one
     * (connection_lookup_large_object_type). Start with InvalidOid so the
     * large-object code paths stay dormant on databases without a "lo" domain. */
    connection->large_object_type_oid = InvalidOid;
    connection->large_object_is_domain = false;

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

    /* Clean up all resources.
     * LIMITATION: explicit descriptors (SQLAllocHandle(SQL_HANDLE_DESC)) that the
     * application never freed with SQLFreeHandle are leaked here — the connection
     * does not track them in a list. Not exercised by Phase 1 tests; revisit when
     * explicit-descriptor lifetime management is fully implemented. */
    diagnostics_clear(&connection->diagnostics);
    connection_clear_notices(connection);
    connection_info_clear(&connection->info);

    /* Poison the magic number to detect use-after-free */
    connection->magic_number = 0;
    free(connection);

    return SQL_SUCCESS;
}

void connection_clear_notices(OdbcConnection *connection)
{
    if (!connection) {
        return;
    }
    for (int i = 0; i < connection->notice_count; i++) {
        free(connection->captured_notices[i]);
        connection->captured_notices[i] = NULL;
    }
    connection->notice_count = 0;
}

bool connection_standard_conforming_strings(const OdbcConnection *connection)
{
    if (!connection || !connection->libpq_connection) {
        return true;
    }
    const char *status = PQparameterStatus(connection->libpq_connection,
                                           "standard_conforming_strings");
    if (!status) {
        /* Pre-8.1 server or unknown — assume off would over-escape modern
         * strings, so default to on to match current PostgreSQL. */
        return true;
    }
    return strcmp(status, "on") == 0;
}

/*
 * Discover the database's "lo" large-object domain, if it exists. The "lo"
 * type ships with the lo contrib module as a DOMAIN over the base type "oid",
 * used so a table column can hold the OID of a large object. Because the domain
 * is created per database it has no fixed type OID, so we look it up here.
 *
 * On success sets large_object_type_oid to the domain's own OID and, when the
 * domain's base type is "oid", large_object_is_domain to true. When no such
 * domain exists the fields are left at their InvalidOid/false defaults and the
 * large-object code paths simply never trigger. A query failure is non-fatal —
 * connections that never use large objects must still succeed.
 */
static void connection_lookup_large_object_type(OdbcConnection *connection)
{
    /* typbasetype is the OID of the domain's underlying base type (0 for a
     * non-domain). We only support a "lo" domain built directly on "oid". */
    PGresult *result = PQexec(connection->libpq_connection,
                              "SELECT oid, typbasetype FROM pg_type "
                              "WHERE typname = 'lo'");
    if (!result) {
        return;
    }
    if (PQresultStatus(result) == PGRES_TUPLES_OK && PQntuples(result) > 0) {
        Oid domain_oid = (Oid)strtoul(PQgetvalue(result, 0, 0), NULL, 10);
        Oid base_type_oid = (Oid)strtoul(PQgetvalue(result, 0, 1), NULL, 10);

        if (base_type_oid == PG_TYPE_OID) {
            /* The supported case: a domain over "oid". */
            connection->large_object_type_oid = domain_oid;
            connection->large_object_is_domain = true;
        } else if (base_type_oid == InvalidOid) {
            /* A bare "lo" base type (no modern server ships one, but mirror the
             * original driver's handling): usable as-is, not a domain. */
            connection->large_object_type_oid = domain_oid;
            connection->large_object_is_domain = false;
        }
        /* A "lo" domain over some OTHER base type is not something we handle;
         * leave the fields at their defaults so the LO paths stay dormant. */
    }
    PQclear(result);
}

bool connection_type_is_large_object(const OdbcConnection *connection, Oid type_oid)
{
    if (!connection || connection->large_object_type_oid == InvalidOid) {
        return false;
    }

    /* A parameter bound to a "lo" column resolves to the domain's own OID. */
    if (type_oid == connection->large_object_type_oid) {
        return true;
    }

    /* On read-back a "lo" column's values report as the base type "oid" (the
     * domain collapses to its base in a result descriptor), so treat a plain
     * "oid" value as a large-object reference when a "lo" domain exists. */
    if (connection->large_object_is_domain && type_oid == PG_TYPE_OID) {
        return true;
    }

    return false;
}

/*
 * libpq notice receiver callback. Called by libpq whenever PostgreSQL sends
 * a NOTICE or WARNING message (e.g., "table already exists", implicit index
 * creation, etc.). We extract the message and store it on the connection for
 * later promotion to ODBC diagnostic records.
 *
 * We construct the message in the format "SEVERITY: primary_message" to match
 * the original psqlodbc driver's behavior.
 */
static void notice_receiver_callback(void *context, const PGresult *notice_result)
{
    OdbcConnection *connection = (OdbcConnection *)context;
    if (!connection || connection->notice_count >= MAX_CAPTURED_NOTICES) {
        return;
    }

    /* Extract individual message fields for clean formatting */
    const char *severity = PQresultErrorField(notice_result, PG_DIAG_SEVERITY);
    const char *primary = PQresultErrorField(notice_result, PG_DIAG_MESSAGE_PRIMARY);

    if (!primary || primary[0] == '\0') {
        return;
    }

    /* Format: "NOTICE: message text" (single space after colon) */
    const char *sev = severity ? severity : "NOTICE";
    size_t sev_len = strlen(sev);
    size_t msg_len = strlen(primary);
    /* "SEVERITY: message\0" */
    size_t total_len = sev_len + 2 + msg_len;

    char *copy = malloc(total_len + 1);
    if (!copy) {
        return;
    }
    memcpy(copy, sev, sev_len);
    copy[sev_len] = ':';
    copy[sev_len + 1] = ' ';
    memcpy(copy + sev_len + 2, primary, msg_len);
    copy[total_len] = '\0';

    connection->captured_notices[connection->notice_count] = copy;
    connection->notice_count++;
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

    /* Install notice receiver to capture NOTICE/WARNING messages from PostgreSQL.
     * These are promoted to ODBC diagnostic records after statement execution. */
    PQsetNoticeReceiver(connection->libpq_connection, notice_receiver_callback, connection);

    /* Parse the server version for feature detection.
     * PQserverVersion returns an integer like 150002 for 15.0.2
     * (major * 10000 + minor * 100 + patch). */
    int version_number = PQserverVersion(connection->libpq_connection);
    connection->server_version_major = version_number / 10000;
    connection->server_version_minor = (version_number / 100) % 100;

    /* Determine the worst-case bytes-per-character for the negotiated client
     * encoding, used when reporting octet lengths of character columns. */
    connection->max_bytes_per_char =
        max_bytes_per_char_for_encoding(pg_encoding_to_char(PQclientEncoding(connection->libpq_connection)));

    /* Detect the database's "lo" large-object domain, if any, so parameter and
     * result handling can recognize large-object references. Runs as its own
     * simple query outside any transaction (autocommit is ON at connect), so it
     * cannot disturb an application transaction. */
    connection_lookup_large_object_type(connection);

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

    /* The "lo" domain OID is database-specific; a future reconnect (possibly to
     * a different database) must rediscover it rather than reuse a stale value. */
    connection->large_object_type_oid = InvalidOid;
    connection->large_object_is_domain = false;

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

    /* BoolsAsChar defaults to on, matching the original psqlodbc driver. */
    info->bools_as_char = true;

    /* Size-reporting defaults match the original psqlodbc driver. */
    info->unknown_sizes = UNKNOWN_SIZES_MAX;
    info->max_varchar_size = DEFAULT_MAX_VARCHAR_SIZE;

    /* No explicit error-rollback mode until the connection string sets one via
     * "Protocol=7.4-N"; the driver then falls back to its server-based default. */
    info->rollback_on_error = ROLLBACK_ON_ERROR_UNSPECIFIED;
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
    /* A fresh transaction has no per-statement savepoint yet. */
    connection->statement_savepoint_active = false;
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
        connection->statement_savepoint_active = false;
        return SQL_ERROR;
    }

    PQclear(result);
    connection->transaction_state = TRANSACTION_STATE_IDLE;
    connection->statement_savepoint_active = false;
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
        connection->statement_savepoint_active = false;
        return SQL_ERROR;
    }

    PQclear(result);
    connection->transaction_state = TRANSACTION_STATE_IDLE;
    connection->statement_savepoint_active = false;
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

int connection_effective_rollback_on_error(const OdbcConnection *connection)
{
    if (!connection) {
        return ROLLBACK_ON_ERROR_STATEMENT;
    }

    /* An explicit "Protocol=7.4-N" wins. */
    if (connection->info.rollback_on_error != ROLLBACK_ON_ERROR_UNSPECIFIED) {
        return connection->info.rollback_on_error;
    }

    /* Default: statement-level rollback. Every server this driver targets
     * supports SAVEPOINT (PostgreSQL 8.0+), so we always default to the
     * finest-grained recovery, matching the original driver's behavior on
     * modern servers. */
    return ROLLBACK_ON_ERROR_STATEMENT;
}

void connection_begin_statement_savepoint(OdbcConnection *connection)
{
    if (!connection || !connection->libpq_connection) {
        return;
    }

    /* Savepoints only make sense inside an active (non-failed) transaction and
     * only when statement-level rollback was requested. */
    if (connection->transaction_state != TRANSACTION_STATE_ACTIVE) {
        return;
    }
    if (connection_effective_rollback_on_error(connection) != ROLLBACK_ON_ERROR_STATEMENT) {
        return;
    }

    /* Re-issuing "SAVEPOINT <name>" without releasing the prior one would leak:
     * PostgreSQL does NOT overwrite a same-named savepoint — it shadows the old
     * one and keeps it on the subtransaction stack, so a long transaction that
     * runs many statements would accumulate one dead subtransaction per
     * statement. So when a savepoint from the previous statement is still
     * standing, RELEASE it first (which also destroys any shadowed ones),
     * then create a fresh one marking the point just before this statement.
     * Sent as a single combined command to avoid an extra round-trip. */
    const char *savepoint_command;
    if (connection->statement_savepoint_active) {
        savepoint_command =
            "RELEASE SAVEPOINT " PER_STATEMENT_SAVEPOINT_NAME ";"
            "SAVEPOINT " PER_STATEMENT_SAVEPOINT_NAME;
    } else {
        savepoint_command = "SAVEPOINT " PER_STATEMENT_SAVEPOINT_NAME;
    }

    PGresult *result = PQexec(connection->libpq_connection, savepoint_command);
    if (result && PQresultStatus(result) == PGRES_COMMAND_OK) {
        connection->statement_savepoint_active = true;
    } else {
        /* If the combined RELEASE+SAVEPOINT failed (e.g. the previous savepoint
         * was already gone), the standing savepoint can no longer be relied on.
         * Fall back to a plain SAVEPOINT so this statement still gets savepoint
         * protection where possible. */
        if (result) {
            PQclear(result);
        }
        result = PQexec(connection->libpq_connection,
                        "SAVEPOINT " PER_STATEMENT_SAVEPOINT_NAME);
        connection->statement_savepoint_active =
            (result && PQresultStatus(result) == PGRES_COMMAND_OK);
    }
    /* On outright failure we simply proceed without a savepoint — the statement
     * still runs; worst case an error falls back to whole-transaction failure. */
    if (result) {
        PQclear(result);
    }
}

void connection_handle_statement_error(OdbcConnection *connection)
{
    if (!connection || !connection->libpq_connection) {
        return;
    }

    /* Only relevant inside an explicit transaction that a statement just
     * aborted. In autocommit-ON mode PostgreSQL already rolled the implicit
     * transaction back for us. */
    if (connection->transaction_state != TRANSACTION_STATE_FAILED) {
        return;
    }

    int mode = connection_effective_rollback_on_error(connection);

    if (mode == ROLLBACK_ON_ERROR_STATEMENT && connection->statement_savepoint_active) {
        /* Undo just the failed statement by rewinding to the savepoint taken
         * before it. This clears the aborted state so the transaction — and
         * its earlier successful statements — can continue. */
        PGresult *result = PQexec(connection->libpq_connection,
                                  "ROLLBACK TO " PER_STATEMENT_SAVEPOINT_NAME);
        if (result && PQresultStatus(result) == PGRES_COMMAND_OK) {
            connection->transaction_state = TRANSACTION_STATE_ACTIVE;
        }
        if (result) {
            PQclear(result);
        }
        return;
    }

    if (mode == ROLLBACK_ON_ERROR_TRANSACTION) {
        /* Discard the whole transaction automatically. */
        connection_rollback(connection);
        return;
    }

    /* ROLLBACK_ON_ERROR_NOTHING (or STATEMENT without an active savepoint):
     * leave the transaction FAILED for the application to resolve with its own
     * SQLEndTran(ROLLBACK). */
}
