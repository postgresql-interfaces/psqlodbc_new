/*-------------------------------------------------------------------------
 *
 * odbc_api.c
 *	  ODBC API dispatch layer
 *
 * Copyright (c) 2026, Dave Cramer
 *
 *
 * IDENTIFICATION
 *	  src/odbc_api.c
 *
 *-------------------------------------------------------------------------
 */
#include "psqlodbc2.h"
#include "environment.h"
#include "connection.h"
#include "connection_string.h"
#include "dsn_config.h"
#include "statement.h"
#include "parameter.h"
#include "column_binding.h"
#include "results.h"
#include "catalog.h"
#include "type_mapping.h"
#include "error_mapping.h"
#include "query_parser.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The ODBC 2.x SQLGetFunctions array has exactly 100 entries (indices 0-99).
 * This is defined by the ODBC spec but not exposed as a named constant in all
 * ODBC header sets, so we define it here. */
#define ODBC2_ALL_FUNCTIONS_ARRAY_SIZE 100

/* ---- Internal Helpers ---- */

/*
 * Get the DiagnosticRecords pointer from a handle based on its type.
 * Returns NULL if the handle type is unrecognized or the handle is invalid.
 */
static DiagnosticRecords *get_diagnostics_for_handle(SQLSMALLINT handle_type, SQLHANDLE handle)
{
    if (!handle) {
        return NULL;
    }

    switch (handle_type) {
    case SQL_HANDLE_ENV: {
        OdbcEnvironment *environment = (OdbcEnvironment *)handle;
        if (environment->magic_number != ENVIRONMENT_MAGIC_NUMBER) {
            return NULL;
        }
        return &environment->diagnostics;
    }
    case SQL_HANDLE_DBC: {
        OdbcConnection *connection = (OdbcConnection *)handle;
        if (connection->magic_number != CONNECTION_MAGIC_NUMBER) {
            return NULL;
        }
        return &connection->diagnostics;
    }
    case SQL_HANDLE_STMT: {
        OdbcStatement *statement = (OdbcStatement *)handle;
        if (statement->magic_number != STATEMENT_MAGIC_NUMBER) {
            return NULL;
        }
        return &statement->diagnostics;
    }
    default:
        return NULL;
    }
}

/*
 * Copy a string into a caller-provided SQLCHAR buffer, respecting the buffer
 * length and reporting the actual string length. Handles SQL_NTS input length.
 * Returns the actual length of the source string (excluding null terminator).
 */
static SQLSMALLINT copy_string_to_output(const char *source,
                                         SQLCHAR *output_buffer,
                                         SQLSMALLINT buffer_length,
                                         SQLSMALLINT *output_length)
{
    if (!source) {
        if (output_length) {
            *output_length = 0;
        }
        if (output_buffer && buffer_length > 0) {
            output_buffer[0] = '\0';
        }
        return 0;
    }

    SQLSMALLINT source_length = (SQLSMALLINT)strlen(source);

    if (output_length) {
        *output_length = source_length;
    }

    if (output_buffer && buffer_length > 0) {
        SQLSMALLINT copy_length = source_length;
        if (copy_length >= buffer_length) {
            copy_length = buffer_length - 1;
        }
        memcpy(output_buffer, source, (size_t)copy_length);
        output_buffer[copy_length] = '\0';
    }

    return source_length;
}

/*
 * Determine the actual byte length of a SQL string argument.
 * Handles SQL_NTS (null-terminated string) by computing strlen.
 */
static size_t resolve_sql_string_length(const SQLCHAR *string, SQLSMALLINT declared_length)
{
    if (!string) {
        return 0;
    }
    if (declared_length == SQL_NTS) {
        return strlen((const char *)string);
    }
    if (declared_length < 0) {
        return 0;
    }
    return (size_t)declared_length;
}

/* ---- ODBC API Exports ---- */

/**
 * SQLAllocHandle — Allocate an ODBC handle (environment, connection, statement, or descriptor).
 *
 * This is the universal handle allocation function introduced in ODBC 3.0.
 * The application specifies which kind of handle to allocate via handle_type.
 *
 * Parameters:
 *   handle_type   - The type of handle to allocate: SQL_HANDLE_ENV, SQL_HANDLE_DBC,
 *                   SQL_HANDLE_STMT, or SQL_HANDLE_DESC.
 *   input_handle  - The parent handle (SQL_NULL_HANDLE for environment handles,
 *                   an environment handle for connections, etc.).
 *   output_handle - Pointer to receive the newly allocated handle.
 *
 * Returns:
 *   SQL_SUCCESS        - Handle allocated successfully.
 *   SQL_ERROR          - Allocation failed or handle type not yet supported.
 *   SQL_INVALID_HANDLE - output_handle is NULL or handle_type is unrecognized.
 *
 * See: https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlallochandle-function
 */
PSQLODBC2_EXPORT SQLRETURN SQL_API
SQLAllocHandle(SQLSMALLINT handle_type,
               SQLHANDLE   input_handle,
               SQLHANDLE  *output_handle)
{
    if (!output_handle) {
        return SQL_INVALID_HANDLE;
    }

    switch (handle_type) {
    case SQL_HANDLE_ENV:
        return environment_allocate(output_handle);

    case SQL_HANDLE_DBC: {
        /* Validate that input_handle is a valid environment */
        OdbcEnvironment *environment = (OdbcEnvironment *)input_handle;
        if (!environment || environment->magic_number != ENVIRONMENT_MAGIC_NUMBER) {
            *output_handle = SQL_NULL_HANDLE;
            return SQL_INVALID_HANDLE;
        }
        return connection_allocate(environment, output_handle);
    }

    case SQL_HANDLE_STMT: {
        /* Validate that input_handle is a valid connection */
        OdbcConnection *connection = (OdbcConnection *)input_handle;
        if (!connection || connection->magic_number != CONNECTION_MAGIC_NUMBER) {
            *output_handle = SQL_NULL_HANDLE;
            return SQL_INVALID_HANDLE;
        }
        return statement_allocate(connection, output_handle);
    }

    case SQL_HANDLE_DESC:
        /* Descriptor handles will be implemented in a future module */
        *output_handle = SQL_NULL_HANDLE;
        return SQL_ERROR;

    default:
        *output_handle = SQL_NULL_HANDLE;
        return SQL_INVALID_HANDLE;
    }
}

/**
 * SQLFreeHandle — Free a previously allocated ODBC handle.
 *
 * Releases resources associated with the specified handle. The handle must
 * not be used after this call returns SQL_SUCCESS.
 *
 * Parameters:
 *   handle_type - The type of handle being freed (SQL_HANDLE_ENV, SQL_HANDLE_DBC, etc.).
 *   handle      - The handle to free.
 *
 * Returns:
 *   SQL_SUCCESS        - Handle freed successfully.
 *   SQL_ERROR          - Handle cannot be freed (e.g., still connected).
 *   SQL_INVALID_HANDLE - handle is NULL or handle_type is unrecognized.
 *
 * See: https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlfreehandle-function
 */
PSQLODBC2_EXPORT SQLRETURN SQL_API
SQLFreeHandle(SQLSMALLINT handle_type,
              SQLHANDLE   handle)
{
    if (!handle) {
        return SQL_INVALID_HANDLE;
    }

    switch (handle_type) {
    case SQL_HANDLE_ENV:
        return environment_free(handle);

    case SQL_HANDLE_DBC:
        return connection_free(handle);

    case SQL_HANDLE_STMT:
        return statement_free(handle);

    case SQL_HANDLE_DESC:
        /* Descriptor handles will be implemented in a future module */
        return SQL_ERROR;

    default:
        return SQL_INVALID_HANDLE;
    }
}

/**
 * SQLConnect — Establish a connection to a data source using DSN, user ID, and password.
 *
 * Looks up the named DSN in the ODBC configuration (odbc.ini on Unix, registry
 * on Windows) to resolve connection parameters (server, port, database, etc.).
 * If UID and/or PWD are provided explicitly, they override any values from the
 * DSN configuration.
 *
 * If DSN lookup fails (e.g., libodbcinst not available or DSN not found), the
 * DSN name is used as the database name as a fallback.
 *
 * Parameters:
 *   connection_handle - A valid allocated connection handle.
 *   server_name       - The data source name (DSN) to look up in odbc.ini.
 *   name_length1      - Length of server_name, or SQL_NTS.
 *   user_name         - The user identifier for authentication (overrides DSN).
 *   name_length2      - Length of user_name, or SQL_NTS.
 *   authentication    - The password for authentication (overrides DSN).
 *   name_length3      - Length of authentication, or SQL_NTS.
 *
 * Returns:
 *   SQL_SUCCESS        - Connection established successfully.
 *   SQL_ERROR          - Connection failed (check SQLGetDiagRec for details).
 *   SQL_INVALID_HANDLE - connection_handle is not a valid connection handle.
 *
 * See: https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlconnect-function
 */
PSQLODBC2_EXPORT SQLRETURN SQL_API
SQLConnect(SQLHDBC      connection_handle,
           SQLCHAR     *server_name,
           SQLSMALLINT  name_length1,
           SQLCHAR     *user_name,
           SQLSMALLINT  name_length2,
           SQLCHAR     *authentication,
           SQLSMALLINT  name_length3)
{
    OdbcConnection *connection = (OdbcConnection *)connection_handle;

    if (!connection || connection->magic_number != CONNECTION_MAGIC_NUMBER) {
        return SQL_INVALID_HANDLE;
    }

    diagnostics_clear(&connection->diagnostics);

    /* Extract the DSN name from the server_name argument */
    size_t dsn_length = resolve_sql_string_length(server_name, name_length1);
    char dsn_name[256] = "";
    if (server_name && dsn_length > 0) {
        size_t copy_len = dsn_length;
        if (copy_len >= sizeof(dsn_name)) {
            copy_len = sizeof(dsn_name) - 1;
        }
        memcpy(dsn_name, server_name, copy_len);
        dsn_name[copy_len] = '\0';
    }

    /* Attempt to resolve the DSN from odbc.ini BEFORE applying the explicit
     * UID/PWD arguments. This way, explicit arguments override DSN values. */
    bool dsn_resolved = dsn_config_read(dsn_name, &connection->info);

    /* If DSN lookup failed and no server was set by other means, fall back to
     * the old behavior of using the DSN name as the database name. This handles
     * the case where no odbc.ini is configured and the application passes the
     * database name as the "DSN" argument. */
    if (!dsn_resolved && dsn_name[0] != '\0' && connection->info.server[0] == '\0') {
        size_t copy_len = strlen(dsn_name);
        if (copy_len >= sizeof(connection->info.database)) {
            copy_len = sizeof(connection->info.database) - 1;
        }
        memcpy(connection->info.database, dsn_name, copy_len);
        connection->info.database[copy_len] = '\0';
    }

    /* Copy username — explicit UID overrides any value from the DSN */
    size_t uid_length = resolve_sql_string_length(user_name, name_length2);
    if (user_name && uid_length > 0) {
        size_t copy_len = uid_length;
        if (copy_len >= sizeof(connection->info.username)) {
            copy_len = sizeof(connection->info.username) - 1;
        }
        memcpy(connection->info.username, user_name, copy_len);
        connection->info.username[copy_len] = '\0';
    }

    /* Copy password — explicit PWD overrides any value from the DSN
     * (heap-allocated for secure clearing) */
    size_t pwd_length = resolve_sql_string_length(authentication, name_length3);
    if (authentication && pwd_length > 0) {
        free(connection->info.password);
        connection->info.password = malloc(pwd_length + 1);
        if (connection->info.password) {
            memcpy(connection->info.password, authentication, pwd_length);
            connection->info.password[pwd_length] = '\0';
        }
    }

    return connection_connect(connection);
}

/**
 * SQLDisconnect — Close a connection to a data source.
 *
 * Disconnects from the server. The connection handle remains allocated and
 * can be reused for a new connection or freed with SQLFreeHandle.
 *
 * Parameters:
 *   connection_handle - A valid connected connection handle.
 *
 * Returns:
 *   SQL_SUCCESS        - Disconnected successfully.
 *   SQL_ERROR          - Not currently connected or disconnect failed.
 *   SQL_INVALID_HANDLE - connection_handle is not a valid connection handle.
 *
 * See: https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqldisconnect-function
 */
PSQLODBC2_EXPORT SQLRETURN SQL_API
SQLDisconnect(SQLHDBC connection_handle)
{
    OdbcConnection *connection = (OdbcConnection *)connection_handle;

    if (!connection || connection->magic_number != CONNECTION_MAGIC_NUMBER) {
        return SQL_INVALID_HANDLE;
    }

    diagnostics_clear(&connection->diagnostics);

    return connection_disconnect(connection);
}

/**
 * SQLGetFunctions — Report which ODBC functions the driver supports.
 *
 * The Driver Manager calls this to determine driver capabilities. Currently
 * reports that no optional functions are supported (all set to SQL_FALSE),
 * since this is the minimal driver skeleton.
 *
 * Parameters:
 *   connection_handle - A valid connection handle (currently unused in stub).
 *   function_id       - The ODBC function ID to query, or SQL_API_ALL_FUNCTIONS
 *                       to query all functions at once.
 *   supported_flags   - Output array. For SQL_API_ODBC3_ALL_FUNCTIONS this is
 *                       a SQL_API_ODBC3_ALL_FUNCTIONS_SIZE-element bitmap array.
 *                       For SQL_API_ALL_FUNCTIONS this is a 100-element SQLUSMALLINT array.
 *                       For a single function ID, a single SQLUSMALLINT.
 *
 * Returns:
 *   SQL_SUCCESS        - supported_flags has been populated.
 *   SQL_ERROR          - supported_flags is NULL.
 *
 * See: https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlgetfunctions-function
 */
PSQLODBC2_EXPORT SQLRETURN SQL_API
SQLGetFunctions(SQLHDBC       connection_handle,
                SQLUSMALLINT  function_id,
                SQLUSMALLINT *supported_flags)
{
    (void)connection_handle;

    if (!supported_flags) {
        return SQL_ERROR;
    }

    /* List of ODBC function IDs this driver supports */
    static const SQLUSMALLINT supported_functions[] = {
        SQL_API_SQLALLOCHANDLE,
        SQL_API_SQLFREEHANDLE,
        SQL_API_SQLCONNECT,
        SQL_API_SQLDISCONNECT,
        SQL_API_SQLDRIVERCONNECT,
        SQL_API_SQLGETFUNCTIONS,
        SQL_API_SQLGETDIAGREC,
        SQL_API_SQLGETDIAGFIELD,
        SQL_API_SQLPREPARE,
        SQL_API_SQLEXECUTE,
        SQL_API_SQLEXECDIRECT,
        SQL_API_SQLFREESTMT,
        SQL_API_SQLNUMRESULTCOLS,
        SQL_API_SQLDESCRIBECOL,
        SQL_API_SQLROWCOUNT,
        SQL_API_SQLFETCH,
        SQL_API_SQLGETDATA,
        SQL_API_SQLBINDCOL,
        SQL_API_SQLBINDPARAMETER,
        SQL_API_SQLTABLES,
        SQL_API_SQLCOLUMNS,
        SQL_API_SQLPRIMARYKEYS,
        SQL_API_SQLFOREIGNKEYS,
        SQL_API_SQLSETCONNECTATTR,
        SQL_API_SQLGETCONNECTATTR,
        SQL_API_SQLSETENVATTR,
        SQL_API_SQLGETENVATTR,
        SQL_API_SQLSETSTMTATTR,
        SQL_API_SQLGETSTMTATTR,
        SQL_API_SQLENDTRAN,
        SQL_API_SQLDESCRIBEPARAM,
        SQL_API_SQLCOLATTRIBUTE,
        SQL_API_SQLGETINFO,
        SQL_API_SQLNUMPARAMS,
        SQL_API_SQLNATIVESQL,
    };
    int num_supported = (int)(sizeof(supported_functions) / sizeof(supported_functions[0]));

    if (function_id == SQL_API_ODBC3_ALL_FUNCTIONS) {
        /* ODBC 3.x bitmap: zero everything then set supported bits */
        memset(supported_flags, 0, SQL_API_ODBC3_ALL_FUNCTIONS_SIZE * sizeof(SQLUSMALLINT));
        for (int i = 0; i < num_supported; i++) {
            SQLUSMALLINT fid = supported_functions[i];
            supported_flags[fid >> 4] |= (1 << (fid & 0x000F));
        }
    } else if (function_id == SQL_API_ALL_FUNCTIONS) {
        /* ODBC 2.x array: set all to FALSE, then mark supported ones */
        for (int index = 0; index < ODBC2_ALL_FUNCTIONS_ARRAY_SIZE; index++) {
            supported_flags[index] = SQL_FALSE;
        }
        for (int i = 0; i < num_supported; i++) {
            if (supported_functions[i] < ODBC2_ALL_FUNCTIONS_ARRAY_SIZE) {
                supported_flags[supported_functions[i]] = SQL_TRUE;
            }
        }
    } else {
        /* Single function query */
        *supported_flags = SQL_FALSE;
        for (int i = 0; i < num_supported; i++) {
            if (supported_functions[i] == function_id) {
                *supported_flags = SQL_TRUE;
                break;
            }
        }
    }

    return SQL_SUCCESS;
}

/**
 * SQLDriverConnect — Establish a connection to a data source using a connection string.
 *
 * This is the primary connection function for ODBC drivers. It accepts a
 * connection string with key=value pairs (e.g., "Server=localhost;Port=5432;Database=mydb;UID=user;PWD=pass").
 *
 * Only SQL_DRIVER_NOPROMPT is supported. Other completion modes (which require
 * a dialog box) return SQL_ERROR since we do not implement a GUI.
 *
 * Parameters:
 *   connection_handle       - A valid connection handle.
 *   window_handle           - Parent window handle for dialog prompts (unused; must be NULL).
 *   connection_string_in    - The input connection string.
 *   string_length_in        - Length of connection_string_in, or SQL_NTS if null-terminated.
 *   connection_string_out   - Buffer for the completed connection string (may be NULL).
 *   buffer_length           - Size of connection_string_out buffer in characters.
 *   string_length_out       - Output: actual length of the completed connection string.
 *   driver_completion       - Must be SQL_DRIVER_NOPROMPT (other modes return SQL_ERROR).
 *
 * Returns:
 *   SQL_SUCCESS           - Connected successfully.
 *   SQL_SUCCESS_WITH_INFO - Connected, but output string was truncated.
 *   SQL_ERROR             - Connection failed or unsupported driver_completion mode.
 *   SQL_INVALID_HANDLE    - connection_handle is not a valid connection handle.
 *
 * See: https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqldriverconnect-function
 */
PSQLODBC2_EXPORT SQLRETURN SQL_API
SQLDriverConnect(SQLHDBC       connection_handle,
                 SQLHWND       window_handle,
                 SQLCHAR      *connection_string_in,
                 SQLSMALLINT   string_length_in,
                 SQLCHAR      *connection_string_out,
                 SQLSMALLINT   buffer_length,
                 SQLSMALLINT  *string_length_out,
                 SQLUSMALLINT  driver_completion)
{
    (void)window_handle;

    OdbcConnection *connection = (OdbcConnection *)connection_handle;

    if (!connection || connection->magic_number != CONNECTION_MAGIC_NUMBER) {
        return SQL_INVALID_HANDLE;
    }

    diagnostics_clear(&connection->diagnostics);

    /* SQL_DRIVER_PROMPT requires a GUI dialog — not supported.
     * SQL_DRIVER_COMPLETE and SQL_DRIVER_COMPLETE_REQUIRED only prompt when
     * required parameters are missing; with no GUI, we proceed without prompting
     * (same behavior as the original psqlodbc on non-Windows platforms). */
    if (driver_completion == SQL_DRIVER_PROMPT && window_handle != NULL) {
        diagnostics_add_record(&connection->diagnostics,
                               "HYC00",  /* Optional feature not implemented */
                               0,
                               "SQL_DRIVER_PROMPT is not supported (no GUI dialog available).");
        return SQL_ERROR;
    }

    /* Parse the connection string into connection parameters */
    if (!connection_string_parse((const char *)connection_string_in,
                                 string_length_in,
                                 &connection->info)) {
        diagnostics_add_record(&connection->diagnostics,
                               "HY000",  /* General error */
                               0,
                               "Failed to parse connection string.");
        return SQL_ERROR;
    }

    /* Attempt the actual connection via libpq */
    SQLRETURN connect_result = connection_connect(connection);
    if (connect_result != SQL_SUCCESS) {
        return connect_result;
    }

    /* Build the output connection string if the caller provided a buffer.
     * The output string reflects the actual parameters used for the connection. */
    SQLRETURN final_result = SQL_SUCCESS;
    if (connection_string_out || string_length_out) {
        /* Build a representative connection string from the resolved parameters */
        char output_string[2048];
        size_t output_offset = 0;
        int written;

        output_string[0] = '\0';

        if (connection->info.server[0] != '\0') {
            written = snprintf(output_string + output_offset,
                               sizeof(output_string) - output_offset,
                               "Server=%s;", connection->info.server);
            if (written > 0) output_offset += (size_t)written;
        }
        if (connection->info.port[0] != '\0') {
            written = snprintf(output_string + output_offset,
                               sizeof(output_string) - output_offset,
                               "Port=%s;", connection->info.port);
            if (written > 0) output_offset += (size_t)written;
        }
        if (connection->info.database[0] != '\0') {
            written = snprintf(output_string + output_offset,
                               sizeof(output_string) - output_offset,
                               "Database=%s;", connection->info.database);
            if (written > 0) output_offset += (size_t)written;
        }
        if (connection->info.username[0] != '\0') {
            written = snprintf(output_string + output_offset,
                               sizeof(output_string) - output_offset,
                               "UID=%s;", connection->info.username);
            if (written > 0) output_offset += (size_t)written;
        }
        /* Password is intentionally omitted from output for security */
        if (connection->info.sslmode[0] != '\0') {
            written = snprintf(output_string + output_offset,
                               sizeof(output_string) - output_offset,
                               "SSLmode=%s;", connection->info.sslmode);
            if (written > 0) output_offset += (size_t)written;
        }

        /* Remove trailing semicolon */
        if (output_offset > 0 && output_string[output_offset - 1] == ';') {
            output_string[output_offset - 1] = '\0';
            output_offset--;
        }

        SQLSMALLINT actual_length = copy_string_to_output(
            output_string, connection_string_out, buffer_length, string_length_out);

        /* If the output was truncated, report SQL_SUCCESS_WITH_INFO */
        if (connection_string_out && buffer_length > 0 && actual_length >= buffer_length) {
            diagnostics_add_record(&connection->diagnostics,
                                   "01004",  /* String data, right truncated */
                                   0,
                                   "Output connection string was truncated.");
            final_result = SQL_SUCCESS_WITH_INFO;
        }
    }

    return final_result;
}

/**
 * SQLGetDiagRec — Retrieve a diagnostic record (error/warning message).
 *
 * Applications call this after an ODBC function returns SQL_ERROR or
 * SQL_SUCCESS_WITH_INFO to get the SQLSTATE, native error code, and
 * human-readable message text.
 *
 * Parameters:
 *   handle_type    - The type of handle to get diagnostics from.
 *   handle         - The handle that produced the diagnostic.
 *   record_number  - Which diagnostic record to retrieve (1-based).
 *   sql_state      - Output: 5-character SQLSTATE code (plus null terminator).
 *   native_error   - Output: driver-specific error code.
 *   message_text   - Output: buffer for the diagnostic message.
 *   buffer_length  - Size of message_text buffer in characters.
 *   text_length    - Output: actual length of the message (excluding null terminator).
 *
 * Returns:
 *   SQL_SUCCESS           - Record retrieved successfully.
 *   SQL_SUCCESS_WITH_INFO - Record retrieved but message was truncated.
 *   SQL_NO_DATA           - No record at the specified index.
 *   SQL_INVALID_HANDLE    - Handle is NULL or invalid.
 *   SQL_ERROR             - Invalid record_number or other error.
 *
 * See: https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlgetdiagrec-function
 */
PSQLODBC2_EXPORT SQLRETURN SQL_API
SQLGetDiagRec(SQLSMALLINT  handle_type,
              SQLHANDLE    handle,
              SQLSMALLINT  record_number,
              SQLCHAR     *sql_state,
              SQLINTEGER  *native_error,
              SQLCHAR     *message_text,
              SQLSMALLINT  buffer_length,
              SQLSMALLINT *text_length)
{
    if (!handle) {
        return SQL_INVALID_HANDLE;
    }

    if (record_number < 1) {
        return SQL_ERROR;
    }

    DiagnosticRecords *diagnostics = get_diagnostics_for_handle(handle_type, handle);
    if (!diagnostics) {
        return SQL_INVALID_HANDLE;
    }

    char state_buffer[SQLSTATE_LENGTH + 1];
    int error_code = 0;
    const char *message = NULL;

    if (!diagnostics_get_record(diagnostics, record_number, state_buffer, &error_code, &message)) {
        return SQL_NO_DATA;
    }

    /* Copy SQLSTATE to caller's buffer */
    if (sql_state) {
        memcpy(sql_state, state_buffer, SQLSTATE_LENGTH);
        sql_state[SQLSTATE_LENGTH] = '\0';
    }

    /* Copy native error code */
    if (native_error) {
        *native_error = (SQLINTEGER)error_code;
    }

    /* Copy message text with truncation handling */
    SQLSMALLINT actual_length = copy_string_to_output(
        message, message_text, buffer_length, text_length);

    /* If message was truncated, return SQL_SUCCESS_WITH_INFO */
    if (message_text && buffer_length > 0 && message && actual_length >= buffer_length) {
        return SQL_SUCCESS_WITH_INFO;
    }

    return SQL_SUCCESS;
}

/**
 * SQLGetDiagField — Retrieve a single field from a diagnostic record.
 *
 * More granular than SQLGetDiagRec — allows fetching individual fields
 * from both header and record-level diagnostic information.
 *
 * Supported header fields (record_number = 0):
 *   SQL_DIAG_NUMBER — Returns the number of diagnostic records.
 *
 * Supported record fields (record_number >= 1):
 *   SQL_DIAG_SQLSTATE     — The 5-character SQLSTATE code.
 *   SQL_DIAG_NATIVE       — The native error code.
 *   SQL_DIAG_MESSAGE_TEXT — The diagnostic message text.
 *
 * Parameters:
 *   handle_type    - The type of handle to get diagnostics from.
 *   handle         - The handle that produced the diagnostic.
 *   record_number  - Which diagnostic record (1-based; 0 for header fields).
 *   diag_id        - The diagnostic field identifier to retrieve.
 *   diag_info      - Output: buffer for the field value.
 *   buffer_length  - Size of diag_info buffer in bytes.
 *   string_length  - Output: actual length of string data (if applicable).
 *
 * Returns:
 *   SQL_SUCCESS        - Field retrieved successfully.
 *   SQL_NO_DATA        - No record at the specified index.
 *   SQL_INVALID_HANDLE - Handle is NULL or invalid.
 *   SQL_ERROR          - Invalid field identifier or parameters.
 *
 * See: https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlgetdiagfield-function
 */
PSQLODBC2_EXPORT SQLRETURN SQL_API
SQLGetDiagField(SQLSMALLINT  handle_type,
                SQLHANDLE    handle,
                SQLSMALLINT  record_number,
                SQLSMALLINT  diag_id,
                SQLPOINTER   diag_info,
                SQLSMALLINT  buffer_length,
                SQLSMALLINT *string_length)
{
    if (!handle) {
        return SQL_INVALID_HANDLE;
    }

    DiagnosticRecords *diagnostics = get_diagnostics_for_handle(handle_type, handle);
    if (!diagnostics) {
        return SQL_INVALID_HANDLE;
    }

    /* Header fields use record_number = 0 */
    if (record_number == 0) {
        if (diag_id == SQL_DIAG_NUMBER) {
            if (diag_info) {
                *(SQLINTEGER *)diag_info = (SQLINTEGER)diagnostics->record_count;
            }
            return SQL_SUCCESS;
        }
        /* Other header fields not yet implemented */
        return SQL_ERROR;
    }

    /* Record-level fields require a valid record */
    char state_buffer[SQLSTATE_LENGTH + 1];
    int error_code = 0;
    const char *message = NULL;

    if (!diagnostics_get_record(diagnostics, record_number, state_buffer, &error_code, &message)) {
        return SQL_NO_DATA;
    }

    switch (diag_id) {
    case SQL_DIAG_SQLSTATE:
        if (diag_info) {
            copy_string_to_output(state_buffer, (SQLCHAR *)diag_info, buffer_length, string_length);
        }
        return SQL_SUCCESS;

    case SQL_DIAG_NATIVE:
        if (diag_info) {
            *(SQLINTEGER *)diag_info = (SQLINTEGER)error_code;
        }
        return SQL_SUCCESS;

    case SQL_DIAG_MESSAGE_TEXT:
        if (diag_info) {
            copy_string_to_output(message, (SQLCHAR *)diag_info, buffer_length, string_length);
        }
        return SQL_SUCCESS;

    default:
        return SQL_ERROR;
    }
}

/**
 * SQLPrepare — Prepare a SQL statement for later execution.
 *
 * Stores the SQL text on the statement handle and sends a PQprepare to the
 * server to create a server-side prepared statement. The statement can then
 * be executed one or more times with SQLExecute.
 *
 * Parameters:
 *   statement_handle - A valid allocated statement handle.
 *   statement_text   - The SQL statement to prepare.
 *   text_length      - Length of statement_text in bytes, or SQL_NTS if null-terminated.
 *
 * Returns:
 *   SQL_SUCCESS        - Statement prepared successfully.
 *   SQL_ERROR          - Preparation failed (check SQLGetDiagRec for details).
 *   SQL_INVALID_HANDLE - statement_handle is not a valid statement handle.
 *
 * See: https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlprepare-function
 */
PSQLODBC2_EXPORT SQLRETURN SQL_API
SQLPrepare(SQLHSTMT    statement_handle,
           SQLCHAR    *statement_text,
           SQLINTEGER  text_length)
{
    OdbcStatement *statement = (OdbcStatement *)statement_handle;

    if (!statement || statement->magic_number != STATEMENT_MAGIC_NUMBER) {
        return SQL_INVALID_HANDLE;
    }

    diagnostics_clear(&statement->diagnostics);

    return statement_prepare(statement, (const char *)statement_text, text_length);
}

/**
 * SQLExecute — Execute a previously prepared statement.
 *
 * Sends the prepared statement to the server for execution using
 * PQexecPrepared. The statement must have been prepared with SQLPrepare.
 * Results are captured on the statement handle for retrieval via SQLFetch/SQLGetData.
 *
 * Parameters:
 *   statement_handle - A valid prepared statement handle.
 *
 * Returns:
 *   SQL_SUCCESS        - Statement executed successfully.
 *   SQL_ERROR          - Execution failed (check SQLGetDiagRec for details).
 *   SQL_INVALID_HANDLE - statement_handle is not a valid statement handle.
 *
 * See: https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlexecute-function
 */
PSQLODBC2_EXPORT SQLRETURN SQL_API
SQLExecute(SQLHSTMT statement_handle)
{
    OdbcStatement *statement = (OdbcStatement *)statement_handle;

    if (!statement || statement->magic_number != STATEMENT_MAGIC_NUMBER) {
        return SQL_INVALID_HANDLE;
    }

    diagnostics_clear(&statement->diagnostics);

    return statement_execute(statement);
}

/**
 * SQLExecDirect — Execute a SQL statement directly without preparing it.
 *
 * Combines the prepare and execute steps into a single call using PQexec.
 * This is the most common execution path for one-shot queries that are not
 * re-executed. Results are captured on the statement handle.
 *
 * Parameters:
 *   statement_handle - A valid allocated statement handle.
 *   statement_text   - The SQL statement to execute.
 *   text_length      - Length of statement_text in bytes, or SQL_NTS if null-terminated.
 *
 * Returns:
 *   SQL_SUCCESS        - Statement executed successfully.
 *   SQL_ERROR          - Execution failed (check SQLGetDiagRec for details).
 *   SQL_INVALID_HANDLE - statement_handle is not a valid statement handle.
 *
 * See: https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlexecdirect-function
 */
PSQLODBC2_EXPORT SQLRETURN SQL_API
SQLExecDirect(SQLHSTMT    statement_handle,
              SQLCHAR    *statement_text,
              SQLINTEGER  text_length)
{
    OdbcStatement *statement = (OdbcStatement *)statement_handle;

    if (!statement || statement->magic_number != STATEMENT_MAGIC_NUMBER) {
        return SQL_INVALID_HANDLE;
    }

    diagnostics_clear(&statement->diagnostics);

    return statement_exec_direct(statement, (const char *)statement_text, text_length);
}

/**
 * SQLFreeStmt — Free a statement handle or reset statement state.
 *
 * Performs one of several operations depending on the option parameter:
 *   SQL_DROP         - Free the statement handle entirely (equivalent to SQLFreeHandle).
 *   SQL_CLOSE        - Close the cursor and discard results, but keep the statement allocated.
 *   SQL_UNBIND       - Reset all column bindings (clears all SQLBindCol associations).
 *   SQL_RESET_PARAMS - Reset all parameter bindings (clears all SQLBindParameter associations).
 *
 * Parameters:
 *   statement_handle - A valid statement handle.
 *   option           - The operation to perform (SQL_DROP, SQL_CLOSE, SQL_UNBIND, SQL_RESET_PARAMS).
 *
 * Returns:
 *   SQL_SUCCESS        - Operation completed successfully.
 *   SQL_ERROR          - Invalid option or operation failed.
 *   SQL_INVALID_HANDLE - statement_handle is not a valid statement handle.
 *
 * See: https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlfreestmt-function
 */
PSQLODBC2_EXPORT SQLRETURN SQL_API
SQLFreeStmt(SQLHSTMT     statement_handle,
            SQLUSMALLINT option)
{
    OdbcStatement *statement = (OdbcStatement *)statement_handle;

    if (!statement || statement->magic_number != STATEMENT_MAGIC_NUMBER) {
        return SQL_INVALID_HANDLE;
    }

    /* Only clear diagnostics for non-DROP options — DROP frees the handle entirely */
    if (option != SQL_DROP) {
        diagnostics_clear(&statement->diagnostics);
    }

    return statement_free_stmt(statement, option);
}

/**
 * SQLNumResultCols — Get the number of columns in a result set.
 *
 * Returns the number of columns in the result set associated with the
 * statement. Can be called after SQLPrepare or SQLExecDirect.
 *
 * Parameters:
 *   statement_handle - A valid statement handle with an available result set.
 *   column_count     - Output: the number of columns in the result set.
 *
 * Returns:
 *   SQL_SUCCESS        - Column count retrieved successfully.
 *   SQL_ERROR          - No result set or other error.
 *   SQL_INVALID_HANDLE - statement_handle is not a valid statement handle.
 *
 * See: https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlnumresultcols-function
 */
PSQLODBC2_EXPORT SQLRETURN SQL_API
SQLNumResultCols(SQLHSTMT     statement_handle,
                 SQLSMALLINT *column_count)
{
    OdbcStatement *statement = (OdbcStatement *)statement_handle;

    if (!statement || statement->magic_number != STATEMENT_MAGIC_NUMBER) {
        return SQL_INVALID_HANDLE;
    }

    diagnostics_clear(&statement->diagnostics);

    return results_num_result_cols(statement, column_count);
}

/**
 * SQLDescribeCol — Describe a column in the result set.
 *
 * Returns the column name, SQL data type, column size (precision), decimal
 * digits (scale), and nullability for a specified column in the result set.
 * Column numbers are 1-based.
 *
 * Parameters:
 *   statement_handle  - A valid statement handle with a result set.
 *   column_number     - The column to describe (1-based).
 *   column_name       - Output buffer for the column name.
 *   name_buffer_length - Size of the column_name buffer.
 *   name_length       - Output: actual length of the column name.
 *   data_type         - Output: the SQL data type of the column.
 *   column_size       - Output: the size (precision) of the column.
 *   decimal_digits    - Output: the decimal digits (scale) of the column.
 *   nullable          - Output: whether the column allows NULLs.
 *
 * Returns:
 *   SQL_SUCCESS           - Column described successfully.
 *   SQL_SUCCESS_WITH_INFO - Column name was truncated.
 *   SQL_ERROR             - Invalid column number or no result set.
 *   SQL_INVALID_HANDLE    - statement_handle is not a valid statement handle.
 *
 * See: https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqldescribecol-function
 */
PSQLODBC2_EXPORT SQLRETURN SQL_API
SQLDescribeCol(SQLHSTMT      statement_handle,
               SQLUSMALLINT  column_number,
               SQLCHAR      *column_name,
               SQLSMALLINT   name_buffer_length,
               SQLSMALLINT  *name_length,
               SQLSMALLINT  *data_type,
               SQLULEN      *column_size,
               SQLSMALLINT  *decimal_digits,
               SQLSMALLINT  *nullable)
{
    OdbcStatement *statement = (OdbcStatement *)statement_handle;

    if (!statement || statement->magic_number != STATEMENT_MAGIC_NUMBER) {
        return SQL_INVALID_HANDLE;
    }

    diagnostics_clear(&statement->diagnostics);

    return results_describe_col(statement, column_number, column_name,
                                name_buffer_length, name_length, data_type,
                                column_size, decimal_digits, nullable);
}

/**
 * SQLRowCount — Get the number of rows affected by an INSERT, UPDATE, or DELETE.
 *
 * For SELECT statements, returns the number of rows in the result set
 * (implementation-defined behavior; some drivers return -1 for SELECT).
 *
 * Parameters:
 *   statement_handle - A valid statement handle after execution.
 *   row_count        - Output: the number of affected rows.
 *
 * Returns:
 *   SQL_SUCCESS        - Row count retrieved successfully.
 *   SQL_INVALID_HANDLE - statement_handle is not a valid statement handle.
 *
 * See: https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlrowcount-function
 */
PSQLODBC2_EXPORT SQLRETURN SQL_API
SQLRowCount(SQLHSTMT  statement_handle,
            SQLLEN   *row_count)
{
    OdbcStatement *statement = (OdbcStatement *)statement_handle;

    if (!statement || statement->magic_number != STATEMENT_MAGIC_NUMBER) {
        return SQL_INVALID_HANDLE;
    }

    diagnostics_clear(&statement->diagnostics);

    return results_row_count(statement, row_count);
}

/**
 * SQLFetch — Advance the cursor to the next row and return data for bound columns.
 *
 * Advances the cursor position by one row. After a successful fetch, the
 * application can call SQLGetData to retrieve column values for the current row.
 * Returns SQL_NO_DATA when past the last row.
 *
 * Parameters:
 *   statement_handle - A valid statement handle with a result set.
 *
 * Returns:
 *   SQL_SUCCESS        - A row was successfully fetched.
 *   SQL_NO_DATA        - No more rows available (past end of result set).
 *   SQL_ERROR          - No result set or fetch error.
 *   SQL_INVALID_HANDLE - statement_handle is not a valid statement handle.
 *
 * See: https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlfetch-function
 */
PSQLODBC2_EXPORT SQLRETURN SQL_API
SQLFetch(SQLHSTMT statement_handle)
{
    OdbcStatement *statement = (OdbcStatement *)statement_handle;

    if (!statement || statement->magic_number != STATEMENT_MAGIC_NUMBER) {
        return SQL_INVALID_HANDLE;
    }

    diagnostics_clear(&statement->diagnostics);

    return results_fetch(statement);
}

/**
 * SQLGetData — Retrieve data for a single column in the current row.
 *
 * Returns the value of a specified column in the current row, converting it
 * from PostgreSQL's text format to the requested C data type. Must be called
 * after a successful SQLFetch.
 *
 * For NULL values, *indicator_or_length is set to SQL_NULL_DATA.
 * For string data that exceeds the buffer, returns SQL_SUCCESS_WITH_INFO
 * with SQLSTATE "01004" and sets *indicator_or_length to the total length.
 *
 * Parameters:
 *   statement_handle   - A valid statement handle positioned on a row.
 *   column_number      - The column to retrieve (1-based).
 *   target_type        - The C data type to convert to (e.g., SQL_C_CHAR, SQL_C_SLONG).
 *   target_value       - Output buffer for the converted data.
 *   buffer_length      - Size of target_value buffer in bytes.
 *   indicator_or_length - Output: data length, or SQL_NULL_DATA for NULLs.
 *
 * Returns:
 *   SQL_SUCCESS           - Data retrieved successfully.
 *   SQL_SUCCESS_WITH_INFO - Data truncated (string too long for buffer).
 *   SQL_ERROR             - Invalid state, column, or unsupported type.
 *   SQL_INVALID_HANDLE    - statement_handle is not a valid statement handle.
 *
 * See: https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlgetdata-function
 */
PSQLODBC2_EXPORT SQLRETURN SQL_API
SQLGetData(SQLHSTMT     statement_handle,
           SQLUSMALLINT column_number,
           SQLSMALLINT  target_type,
           SQLPOINTER   target_value,
           SQLLEN       buffer_length,
           SQLLEN      *indicator_or_length)
{
    OdbcStatement *statement = (OdbcStatement *)statement_handle;

    if (!statement || statement->magic_number != STATEMENT_MAGIC_NUMBER) {
        return SQL_INVALID_HANDLE;
    }

    diagnostics_clear(&statement->diagnostics);

    return results_get_data(statement, column_number, target_type,
                            target_value, buffer_length, indicator_or_length);
}

/**
 * SQLTables — List tables, views, and other relations in the data source.
 *
 * Returns a result set with one row per matching relation. The result set
 * has columns: TABLE_CAT, TABLE_SCHEM, TABLE_NAME, TABLE_TYPE, REMARKS.
 * Pattern arguments support LIKE wildcards (% and _).
 *
 * Parameters:
 *   statement_handle   - A valid statement handle.
 *   catalog_name       - Catalog name (ignored; PostgreSQL has one catalog per connection).
 *   name_length1       - Length of catalog_name, or SQL_NTS.
 *   schema_name        - Schema name pattern (LIKE wildcards supported).
 *   name_length2       - Length of schema_name, or SQL_NTS.
 *   table_name         - Table name pattern (LIKE wildcards supported).
 *   name_length3       - Length of table_name, or SQL_NTS.
 *   table_type         - Comma-separated list of table types (e.g., "'TABLE','VIEW'").
 *   name_length4       - Length of table_type, or SQL_NTS.
 *
 * Returns:
 *   SQL_SUCCESS        - Result set is available for fetching.
 *   SQL_ERROR          - Query failed.
 *   SQL_INVALID_HANDLE - statement_handle is not valid.
 *
 * See: https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqltables-function
 */
PSQLODBC2_EXPORT SQLRETURN SQL_API
SQLTables(SQLHSTMT     statement_handle,
          SQLCHAR     *catalog_name,
          SQLSMALLINT  name_length1,
          SQLCHAR     *schema_name,
          SQLSMALLINT  name_length2,
          SQLCHAR     *table_name,
          SQLSMALLINT  name_length3,
          SQLCHAR     *table_type,
          SQLSMALLINT  name_length4)
{
    OdbcStatement *statement = (OdbcStatement *)statement_handle;

    if (!statement || statement->magic_number != STATEMENT_MAGIC_NUMBER) {
        return SQL_INVALID_HANDLE;
    }

    diagnostics_clear(&statement->diagnostics);

    return catalog_tables(statement, catalog_name, name_length1,
                          schema_name, name_length2,
                          table_name, name_length3,
                          table_type, name_length4);
}

/**
 * SQLColumns — Describe columns of tables matching the given patterns.
 *
 * Returns a result set with one row per matching column. The result set
 * has 18 ODBC-standard columns including COLUMN_NAME, DATA_TYPE, TYPE_NAME,
 * COLUMN_SIZE, NULLABLE, COLUMN_DEF, and ORDINAL_POSITION.
 * Pattern arguments support LIKE wildcards (% and _).
 *
 * Parameters:
 *   statement_handle   - A valid statement handle.
 *   catalog_name       - Catalog name (ignored).
 *   name_length1       - Length of catalog_name, or SQL_NTS.
 *   schema_name        - Schema name pattern.
 *   name_length2       - Length of schema_name, or SQL_NTS.
 *   table_name         - Table name pattern.
 *   name_length3       - Length of table_name, or SQL_NTS.
 *   column_name        - Column name pattern.
 *   name_length4       - Length of column_name, or SQL_NTS.
 *
 * Returns:
 *   SQL_SUCCESS        - Result set is available for fetching.
 *   SQL_ERROR          - Query failed.
 *   SQL_INVALID_HANDLE - statement_handle is not valid.
 *
 * See: https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlcolumns-function
 */
PSQLODBC2_EXPORT SQLRETURN SQL_API
SQLColumns(SQLHSTMT     statement_handle,
           SQLCHAR     *catalog_name,
           SQLSMALLINT  name_length1,
           SQLCHAR     *schema_name,
           SQLSMALLINT  name_length2,
           SQLCHAR     *table_name,
           SQLSMALLINT  name_length3,
           SQLCHAR     *column_name,
           SQLSMALLINT  name_length4)
{
    OdbcStatement *statement = (OdbcStatement *)statement_handle;

    if (!statement || statement->magic_number != STATEMENT_MAGIC_NUMBER) {
        return SQL_INVALID_HANDLE;
    }

    diagnostics_clear(&statement->diagnostics);

    return catalog_columns(statement, catalog_name, name_length1,
                           schema_name, name_length2,
                           table_name, name_length3,
                           column_name, name_length4);
}

/**
 * SQLPrimaryKeys — Get primary key columns for a specific table.
 *
 * Returns a result set with one row per primary key column, ordered by
 * KEY_SEQ. Columns: TABLE_CAT, TABLE_SCHEM, TABLE_NAME, COLUMN_NAME,
 * KEY_SEQ, PK_NAME.
 *
 * Arguments are exact-match (no LIKE wildcards). Table name is required.
 *
 * Parameters:
 *   statement_handle - A valid statement handle.
 *   catalog_name     - Catalog name (ignored).
 *   name_length1     - Length of catalog_name, or SQL_NTS.
 *   schema_name      - Schema name (exact match, or NULL for all non-system schemas).
 *   name_length2     - Length of schema_name, or SQL_NTS.
 *   table_name       - Table name (exact match, required).
 *   name_length3     - Length of table_name, or SQL_NTS.
 *
 * Returns:
 *   SQL_SUCCESS        - Result set is available for fetching.
 *   SQL_ERROR          - Table name not provided or query failed.
 *   SQL_INVALID_HANDLE - statement_handle is not valid.
 *
 * See: https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlprimarykeys-function
 */
PSQLODBC2_EXPORT SQLRETURN SQL_API
SQLPrimaryKeys(SQLHSTMT     statement_handle,
               SQLCHAR     *catalog_name,
               SQLSMALLINT  name_length1,
               SQLCHAR     *schema_name,
               SQLSMALLINT  name_length2,
               SQLCHAR     *table_name,
               SQLSMALLINT  name_length3)
{
    OdbcStatement *statement = (OdbcStatement *)statement_handle;

    if (!statement || statement->magic_number != STATEMENT_MAGIC_NUMBER) {
        return SQL_INVALID_HANDLE;
    }

    diagnostics_clear(&statement->diagnostics);

    return catalog_primary_keys(statement, catalog_name, name_length1,
                                schema_name, name_length2,
                                table_name, name_length3);
}

/**
 * SQLForeignKeys — Get foreign key relationships between tables.
 *
 * Can be called in three modes:
 *   - pk_table specified: returns all FKs that reference the given table (exported keys)
 *   - fk_table specified: returns all FKs on the given table (imported keys)
 *   - both specified: returns FKs from fk_table referencing pk_table (cross-reference)
 *
 * Returns a result set with 14 columns including PKTABLE_NAME, PKCOLUMN_NAME,
 * FKTABLE_NAME, FKCOLUMN_NAME, KEY_SEQ, UPDATE_RULE, DELETE_RULE, FK_NAME, PK_NAME.
 *
 * Arguments are exact-match (no LIKE wildcards). At least one table must be specified.
 *
 * Parameters:
 *   statement_handle - A valid statement handle.
 *   pk_catalog_name  - PK table catalog (ignored).
 *   pk_name_length1  - Length of pk_catalog_name, or SQL_NTS.
 *   pk_schema_name   - PK table schema (exact match).
 *   pk_name_length2  - Length of pk_schema_name, or SQL_NTS.
 *   pk_table_name    - PK table name (exact match).
 *   pk_name_length3  - Length of pk_table_name, or SQL_NTS.
 *   fk_catalog_name  - FK table catalog (ignored).
 *   fk_name_length1  - Length of fk_catalog_name, or SQL_NTS.
 *   fk_schema_name   - FK table schema (exact match).
 *   fk_name_length2  - Length of fk_schema_name, or SQL_NTS.
 *   fk_table_name    - FK table name (exact match).
 *   fk_name_length3  - Length of fk_table_name, or SQL_NTS.
 *
 * Returns:
 *   SQL_SUCCESS        - Result set is available for fetching.
 *   SQL_ERROR          - No table specified or query failed.
 *   SQL_INVALID_HANDLE - statement_handle is not valid.
 *
 * See: https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlforeignkeys-function
 */
PSQLODBC2_EXPORT SQLRETURN SQL_API
SQLForeignKeys(SQLHSTMT     statement_handle,
               SQLCHAR     *pk_catalog_name,
               SQLSMALLINT  pk_name_length1,
               SQLCHAR     *pk_schema_name,
               SQLSMALLINT  pk_name_length2,
               SQLCHAR     *pk_table_name,
               SQLSMALLINT  pk_name_length3,
               SQLCHAR     *fk_catalog_name,
               SQLSMALLINT  fk_name_length1,
               SQLCHAR     *fk_schema_name,
               SQLSMALLINT  fk_name_length2,
               SQLCHAR     *fk_table_name,
               SQLSMALLINT  fk_name_length3)
{
    OdbcStatement *statement = (OdbcStatement *)statement_handle;

    if (!statement || statement->magic_number != STATEMENT_MAGIC_NUMBER) {
        return SQL_INVALID_HANDLE;
    }

    diagnostics_clear(&statement->diagnostics);

    return catalog_foreign_keys(statement,
                                pk_catalog_name, pk_name_length1,
                                pk_schema_name, pk_name_length2,
                                pk_table_name, pk_name_length3,
                                fk_catalog_name, fk_name_length1,
                                fk_schema_name, fk_name_length2,
                                fk_table_name, fk_name_length3);
}

/**
 * SQLBindParameter — Bind a parameter marker to an application variable.
 *
 * Associates a C data buffer with a parameter marker ($1, $2, ...) in a
 * prepared or directly-executed SQL statement. The driver reads the buffer
 * contents at execution time, converts the C value to PostgreSQL text format,
 * and passes it to PQexecPrepared or PQexecParams.
 *
 * The binding persists on the statement handle until explicitly reset via
 * SQLFreeStmt(SQL_RESET_PARAMS) or until the statement handle is freed.
 *
 * Parameters:
 *   statement_handle    - A valid statement handle.
 *   parameter_number    - The parameter position (1-based, corresponding to $1, $2, etc.).
 *   input_output_type   - Direction: SQL_PARAM_INPUT, SQL_PARAM_OUTPUT, or SQL_PARAM_INPUT_OUTPUT.
 *   value_type          - The C data type of the parameter buffer (e.g., SQL_C_SLONG, SQL_C_CHAR).
 *   parameter_type      - The SQL data type hint for the server (e.g., SQL_INTEGER, SQL_VARCHAR).
 *   column_size         - Precision of the SQL parameter type.
 *   decimal_digits      - Scale of the SQL parameter type.
 *   parameter_value_ptr - Pointer to the application's data buffer (read at execute time).
 *   buffer_length       - Size of the parameter_value_ptr buffer in bytes.
 *   strlen_or_ind_ptr   - Pointer to a length/indicator variable. At execute time, this
 *                         must contain: the data length, SQL_NTS for null-terminated strings,
 *                         or SQL_NULL_DATA for NULL values.
 *
 * Returns:
 *   SQL_SUCCESS        - Parameter bound successfully.
 *   SQL_ERROR          - Invalid parameter number or statement handle.
 *   SQL_INVALID_HANDLE - statement_handle is not a valid statement handle.
 *
 * See: https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlbindparameter-function
 */
PSQLODBC2_EXPORT SQLRETURN SQL_API
SQLBindParameter(SQLHSTMT      statement_handle,
                 SQLUSMALLINT  parameter_number,
                 SQLSMALLINT   input_output_type,
                 SQLSMALLINT   value_type,
                 SQLSMALLINT   parameter_type,
                 SQLULEN       column_size,
                 SQLSMALLINT   decimal_digits,
                 SQLPOINTER    parameter_value_ptr,
                 SQLLEN        buffer_length,
                 SQLLEN       *strlen_or_ind_ptr)
{
    OdbcStatement *statement = (OdbcStatement *)statement_handle;

    if (!statement || statement->magic_number != STATEMENT_MAGIC_NUMBER) {
        return SQL_INVALID_HANDLE;
    }

    diagnostics_clear(&statement->diagnostics);

    /* Validate parameter number range */
    if (parameter_number == 0 || parameter_number > MAX_PARAMETERS) {
        diagnostics_add_record(&statement->diagnostics,
                               "07009",  /* Invalid descriptor index */
                               0,
                               "Parameter number is out of valid range (1 to 256).");
        return SQL_ERROR;
    }

    return parameter_bind(statement->parameter_bindings,
                          &statement->bound_parameter_count,
                          parameter_number,
                          input_output_type,
                          value_type,
                          parameter_type,
                          column_size,
                          decimal_digits,
                          parameter_value_ptr,
                          buffer_length,
                          strlen_or_ind_ptr);
}

/**
 * SQLDescribeParam — Describe a parameter marker in a prepared statement.
 *
 * Returns the SQL data type, size, decimal digits, and nullability of the
 * specified parameter marker. Requires the statement to have been prepared
 * via SQLPrepare so that PostgreSQL has inferred the parameter types.
 *
 * Parameters:
 *   statement_handle  - A valid prepared statement handle.
 *   parameter_number  - The parameter position (1-based).
 *   data_type_ptr     - Output: the SQL data type of the parameter.
 *   parameter_size_ptr - Output: the size (precision) of the parameter.
 *   decimal_digits_ptr - Output: the decimal digits (scale) of the parameter.
 *   nullable_ptr      - Output: whether the parameter allows NULLs.
 *
 * Returns:
 *   SQL_SUCCESS        - Parameter described successfully.
 *   SQL_ERROR          - Statement not prepared or parameter out of range.
 *   SQL_INVALID_HANDLE - statement_handle is not a valid statement handle.
 *
 * See: https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqldescribeparam-function
 */
PSQLODBC2_EXPORT SQLRETURN SQL_API
SQLDescribeParam(SQLHSTMT      statement_handle,
                 SQLUSMALLINT  parameter_number,
                 SQLSMALLINT  *data_type_ptr,
                 SQLULEN      *parameter_size_ptr,
                 SQLSMALLINT  *decimal_digits_ptr,
                 SQLSMALLINT  *nullable_ptr)
{
    OdbcStatement *statement = (OdbcStatement *)statement_handle;

    if (!statement || statement->magic_number != STATEMENT_MAGIC_NUMBER) {
        return SQL_INVALID_HANDLE;
    }

    diagnostics_clear(&statement->diagnostics);

    if (!statement->is_prepared) {
        diagnostics_add_record(&statement->diagnostics,
                               "HY010",  /* Function sequence error */
                               0,
                               "SQLDescribeParam requires a prepared statement.");
        return SQL_ERROR;
    }

    if (parameter_number < 1 || parameter_number > (SQLUSMALLINT)statement->detected_param_count) {
        diagnostics_add_record(&statement->diagnostics,
                               "07009",  /* Invalid descriptor index */
                               0,
                               "Parameter number is out of range.");
        return SQL_ERROR;
    }

    /* Use PQdescribePrepared result to get parameter type info.
     * PQparamtype returns the OID of the inferred parameter type. */
    if (!statement->parent_connection || !statement->parent_connection->libpq_connection) {
        diagnostics_add_record(&statement->diagnostics,
                               "08003", 0,
                               "Connection is not active.");
        return SQL_ERROR;
    }

    /* Get the parameter type OID from a fresh PQdescribePrepared call,
     * or use the describe_result we already have. */
    PGconn *libpq_conn = statement->parent_connection->libpq_connection;
    PGresult *param_desc = PQdescribePrepared(libpq_conn, statement->prepared_name);
    if (!param_desc || PQresultStatus(param_desc) != PGRES_COMMAND_OK) {
        if (param_desc) PQclear(param_desc);
        diagnostics_add_record(&statement->diagnostics,
                               "HY000", 0,
                               "PQdescribePrepared failed for parameter description.");
        return SQL_ERROR;
    }

    int param_count = PQnparams(param_desc);
    if ((int)parameter_number > param_count) {
        PQclear(param_desc);
        diagnostics_add_record(&statement->diagnostics,
                               "07009", 0,
                               "Parameter number exceeds the number of parameters in the statement.");
        return SQL_ERROR;
    }

    /* PQparamtype uses 0-based index */
    Oid param_oid = PQparamtype(param_desc, (int)(parameter_number - 1));
    PQclear(param_desc);

    /* Map PostgreSQL OID to ODBC SQL type */
    if (data_type_ptr) {
        *data_type_ptr = type_mapping_get_sql_type(param_oid);
    }

    if (parameter_size_ptr) {
        *parameter_size_ptr = type_mapping_get_column_size(param_oid, -1);
    }

    if (decimal_digits_ptr) {
        /* For integer types, return -1 to indicate "not applicable" rather than
         * 0 (which would imply exact zero decimal places). This matches the
         * behavior of the original psqlodbc driver. */
        SQLSMALLINT digits = type_mapping_get_decimal_digits(param_oid, -1);
        SQLSMALLINT sql_type = type_mapping_get_sql_type(param_oid);
        if (sql_type == SQL_INTEGER || sql_type == SQL_SMALLINT || sql_type == SQL_BIGINT) {
            digits = -1;
        }
        *decimal_digits_ptr = digits;
    }

    /* Parameters are always nullable from the driver's perspective */
    if (nullable_ptr) {
        *nullable_ptr = SQL_NULLABLE;
    }

    return SQL_SUCCESS;
}

/**
 * SQLBindCol — Bind a result set column to an application variable.
 *
 * After binding, SQLFetch will automatically write the column's value into
 * the bound buffer for each row fetched. The indicator/length variable
 * receives the data length or SQL_NULL_DATA for NULL values.
 *
 * Passing target_value=NULL unbinds the column. To unbind all columns at
 * once, use SQLFreeStmt(SQL_UNBIND).
 *
 * Parameters:
 *   statement_handle    - A valid statement handle.
 *   column_number       - The column to bind (1-based). Column 0 (bookmark) is not supported.
 *   target_type         - The C data type for the buffer (SQL_C_CHAR, SQL_C_SLONG, etc.).
 *   target_value        - Pointer to the buffer to receive data, or NULL to unbind.
 *   buffer_length       - Size of the target buffer in bytes.
 *   strlen_or_indicator - Pointer to receive data length or SQL_NULL_DATA.
 *
 * Returns:
 *   SQL_SUCCESS        - Column bound (or unbound) successfully.
 *   SQL_ERROR          - Invalid column number or statement handle.
 *   SQL_INVALID_HANDLE - statement_handle is not a valid statement handle.
 *
 * See: https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlbindcol-function
 */
PSQLODBC2_EXPORT SQLRETURN SQL_API
SQLBindCol(SQLHSTMT     statement_handle,
           SQLUSMALLINT column_number,
           SQLSMALLINT  target_type,
           SQLPOINTER   target_value,
           SQLLEN       buffer_length,
           SQLLEN      *strlen_or_indicator)
{
    OdbcStatement *statement = (OdbcStatement *)statement_handle;

    if (!statement || statement->magic_number != STATEMENT_MAGIC_NUMBER) {
        return SQL_INVALID_HANDLE;
    }

    diagnostics_clear(&statement->diagnostics);

    /* Column 0 is the bookmark column — not supported in this driver */
    if (column_number == 0) {
        diagnostics_add_record(&statement->diagnostics,
                               "HYC00",  /* Optional feature not implemented */
                               0,
                               "Bookmark columns (column 0) are not supported.");
        return SQL_ERROR;
    }

    /* Validate column number upper bound */
    if (column_number > MAX_BOUND_COLUMNS) {
        diagnostics_add_record(&statement->diagnostics,
                               "07009",  /* Invalid descriptor index */
                               0,
                               "Column number exceeds the maximum supported bound columns (256).");
        return SQL_ERROR;
    }

    return column_binding_bind(statement->column_bindings,
                               &statement->bound_column_count,
                               column_number,
                               target_type,
                               target_value,
                               buffer_length,
                               strlen_or_indicator);
}

/**
 * SQLSetConnectAttr — Set a connection attribute to control connection behavior.
 *
 * Allows applications to configure connection-level options such as autocommit
 * mode, transaction isolation level, login timeout, and access mode. Some
 * attributes take effect immediately (e.g., autocommit) while others are stored
 * and applied at connect time (e.g., login timeout).
 *
 * Parameters:
 *   connection_handle - A valid connection handle.
 *   attribute         - The attribute identifier (SQL_ATTR_AUTOCOMMIT, etc.).
 *   value_ptr         - The attribute value. For integer attributes, this is the
 *                       value cast to SQLPOINTER. For string attributes, this points
 *                       to a null-terminated string.
 *   string_length     - Length of value_ptr if it's a string, or SQL_IS_INTEGER
 *                       for integer values.
 *
 * Returns:
 *   SQL_SUCCESS           - Attribute set successfully.
 *   SQL_SUCCESS_WITH_INFO - Attribute set but driver substituted a value
 *                           (e.g., READ UNCOMMITTED mapped to READ COMMITTED).
 *   SQL_ERROR             - Invalid attribute or value.
 *   SQL_INVALID_HANDLE    - connection_handle is not a valid connection handle.
 *
 * See: https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlsetconnectattr-function
 */
PSQLODBC2_EXPORT SQLRETURN SQL_API
SQLSetConnectAttr(SQLHDBC     connection_handle,
                  SQLINTEGER  attribute,
                  SQLPOINTER  value_ptr,
                  SQLINTEGER  string_length)
{
    (void)string_length;

    OdbcConnection *connection = (OdbcConnection *)connection_handle;

    if (!connection || connection->magic_number != CONNECTION_MAGIC_NUMBER) {
        return SQL_INVALID_HANDLE;
    }

    diagnostics_clear(&connection->diagnostics);

    SQLULEN value_as_uint = (SQLULEN)(uintptr_t)value_ptr;

    switch (attribute) {
    case SQL_ATTR_AUTOCOMMIT: {
        bool new_autocommit = (value_as_uint == SQL_AUTOCOMMIT_ON);

        /* If switching from OFF to ON, end any active transaction.
         * If the transaction is in FAILED state (an error occurred), we must
         * ROLLBACK since PostgreSQL rejects COMMIT in an aborted transaction.
         * Otherwise, COMMIT per the ODBC spec: "If an application sets
         * SQL_ATTR_AUTOCOMMIT to SQL_AUTOCOMMIT_ON... any open transaction
         * on the connection is committed." */
        if (new_autocommit && !connection->autocommit) {
            if (connection->transaction_state != TRANSACTION_STATE_IDLE &&
                connection->state == CONNECTION_STATE_CONNECTED) {
                if (connection->transaction_state == TRANSACTION_STATE_FAILED) {
                    connection_rollback(connection);
                } else {
                    SQLRETURN commit_result = connection_commit(connection);
                    if (commit_result != SQL_SUCCESS) {
                        return commit_result;
                    }
                }
            }
        }

        connection->autocommit = new_autocommit;
        return SQL_SUCCESS;
    }

    case SQL_ATTR_TXN_ISOLATION: {
        SQLUINTEGER isolation_level = (SQLUINTEGER)value_as_uint;
        const char *isolation_sql = NULL;
        SQLRETURN result = SQL_SUCCESS;

        switch (isolation_level) {
        case SQL_TXN_READ_UNCOMMITTED:
            /* PostgreSQL does not support READ UNCOMMITTED — it silently
             * upgrades to READ COMMITTED. We accept the request but warn
             * the caller via SQL_SUCCESS_WITH_INFO. */
            isolation_sql = "SET SESSION CHARACTERISTICS AS TRANSACTION ISOLATION LEVEL READ COMMITTED";
            connection->txn_isolation = SQL_TXN_READ_COMMITTED;
            result = SQL_SUCCESS_WITH_INFO;
            diagnostics_add_record(&connection->diagnostics,
                                   "01S02",  /* Option value changed */
                                   0,
                                   "PostgreSQL does not support READ UNCOMMITTED; using READ COMMITTED instead.");
            break;
        case SQL_TXN_READ_COMMITTED:
            isolation_sql = "SET SESSION CHARACTERISTICS AS TRANSACTION ISOLATION LEVEL READ COMMITTED";
            connection->txn_isolation = SQL_TXN_READ_COMMITTED;
            break;
        case SQL_TXN_REPEATABLE_READ:
            isolation_sql = "SET SESSION CHARACTERISTICS AS TRANSACTION ISOLATION LEVEL REPEATABLE READ";
            connection->txn_isolation = SQL_TXN_REPEATABLE_READ;
            break;
        case SQL_TXN_SERIALIZABLE:
            isolation_sql = "SET SESSION CHARACTERISTICS AS TRANSACTION ISOLATION LEVEL SERIALIZABLE";
            connection->txn_isolation = SQL_TXN_SERIALIZABLE;
            break;
        default:
            diagnostics_add_record(&connection->diagnostics,
                                   "HY024",  /* Invalid attribute value */
                                   0,
                                   "Unsupported transaction isolation level.");
            return SQL_ERROR;
        }

        /* If connected, apply the isolation level to the session immediately */
        if (connection->state == CONNECTION_STATE_CONNECTED &&
            connection->libpq_connection && isolation_sql) {
            PGresult *pg_result = PQexec(connection->libpq_connection, isolation_sql);
            if (!pg_result || PQresultStatus(pg_result) != PGRES_COMMAND_OK) {
                diagnostics_clear(&connection->diagnostics);
                if (pg_result) {
                    error_add_diagnostic_from_result(&connection->diagnostics,
                                                    pg_result, "HY000");
                    PQclear(pg_result);
                } else {
                    diagnostics_add_record(&connection->diagnostics,
                                           "08S01", 0,
                                           "SET TRANSACTION ISOLATION failed: NULL result.");
                }
                return SQL_ERROR;
            }
            PQclear(pg_result);
        }

        return result;
    }

    case SQL_ATTR_LOGIN_TIMEOUT:
        connection->login_timeout = (SQLUINTEGER)value_as_uint;
        /* Also update the connect_timeout in ConnectionInfo so it takes
         * effect when the connection is actually established. */
        connection->info.connect_timeout = (unsigned int)connection->login_timeout;
        return SQL_SUCCESS;

    case SQL_ATTR_CONNECTION_TIMEOUT:
        /* PostgreSQL doesn't support changing statement/connection timeout
         * after connection is established in the same way ODBC expects.
         * We store it for reporting via SQLGetConnectAttr. */
        connection->connection_timeout = (SQLUINTEGER)value_as_uint;
        return SQL_SUCCESS;

    case SQL_ATTR_ACCESS_MODE: {
        SQLUINTEGER mode = (SQLUINTEGER)value_as_uint;
        if (mode != SQL_MODE_READ_WRITE && mode != SQL_MODE_READ_ONLY) {
            diagnostics_add_record(&connection->diagnostics,
                                   "HY024",  /* Invalid attribute value */
                                   0,
                                   "Access mode must be SQL_MODE_READ_WRITE or SQL_MODE_READ_ONLY.");
            return SQL_ERROR;
        }

        connection->access_mode = mode;

        /* If connected, apply the access mode to the session */
        if (connection->state == CONNECTION_STATE_CONNECTED &&
            connection->libpq_connection) {
            const char *mode_sql = (mode == SQL_MODE_READ_ONLY)
                ? "SET SESSION CHARACTERISTICS AS TRANSACTION READ ONLY"
                : "SET SESSION CHARACTERISTICS AS TRANSACTION READ WRITE";

            PGresult *pg_result = PQexec(connection->libpq_connection, mode_sql);
            if (!pg_result || PQresultStatus(pg_result) != PGRES_COMMAND_OK) {
                diagnostics_clear(&connection->diagnostics);
                if (pg_result) {
                    error_add_diagnostic_from_result(&connection->diagnostics,
                                                    pg_result, "HY000");
                    PQclear(pg_result);
                } else {
                    diagnostics_add_record(&connection->diagnostics,
                                           "08S01", 0,
                                           "SET TRANSACTION access mode failed: NULL result.");
                }
                return SQL_ERROR;
            }
            PQclear(pg_result);
        }

        return SQL_SUCCESS;
    }

    default:
        diagnostics_add_record(&connection->diagnostics,
                               "HY092",  /* Invalid attribute/option identifier */
                               0,
                               "Unsupported connection attribute.");
        return SQL_ERROR;
    }
}

/**
 * SQLGetConnectAttr — Retrieve the current value of a connection attribute.
 *
 * Returns the current setting of the specified connection attribute. Integer
 * attributes are written directly to value_ptr; string attributes are copied
 * to the buffer with length reported via string_length_ptr.
 *
 * Parameters:
 *   connection_handle  - A valid connection handle.
 *   attribute          - The attribute identifier to query.
 *   value_ptr          - Output buffer for the attribute value.
 *   buffer_length      - Size of value_ptr buffer (for string attributes).
 *   string_length_ptr  - Output: actual length of string data (NULL for integer attrs).
 *
 * Returns:
 *   SQL_SUCCESS        - Attribute value retrieved successfully.
 *   SQL_ERROR          - Invalid or unsupported attribute.
 *   SQL_INVALID_HANDLE - connection_handle is not a valid connection handle.
 *
 * See: https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlgetconnectattr-function
 */
PSQLODBC2_EXPORT SQLRETURN SQL_API
SQLGetConnectAttr(SQLHDBC     connection_handle,
                  SQLINTEGER  attribute,
                  SQLPOINTER  value_ptr,
                  SQLINTEGER  buffer_length,
                  SQLINTEGER *string_length_ptr)
{
    (void)buffer_length;

    OdbcConnection *connection = (OdbcConnection *)connection_handle;

    if (!connection || connection->magic_number != CONNECTION_MAGIC_NUMBER) {
        return SQL_INVALID_HANDLE;
    }

    diagnostics_clear(&connection->diagnostics);

    switch (attribute) {
    case SQL_ATTR_AUTOCOMMIT:
        if (value_ptr) {
            *(SQLUINTEGER *)value_ptr = connection->autocommit
                ? SQL_AUTOCOMMIT_ON
                : SQL_AUTOCOMMIT_OFF;
        }
        if (string_length_ptr) {
            *string_length_ptr = (SQLINTEGER)sizeof(SQLUINTEGER);
        }
        return SQL_SUCCESS;

    case SQL_ATTR_TXN_ISOLATION:
        if (value_ptr) {
            *(SQLUINTEGER *)value_ptr = connection->txn_isolation;
        }
        if (string_length_ptr) {
            *string_length_ptr = (SQLINTEGER)sizeof(SQLUINTEGER);
        }
        return SQL_SUCCESS;

    case SQL_ATTR_CONNECTION_DEAD:
        if (value_ptr) {
            /* Check whether the underlying libpq connection is still alive */
            if (!connection->libpq_connection ||
                PQstatus(connection->libpq_connection) != CONNECTION_OK) {
                *(SQLUINTEGER *)value_ptr = SQL_CD_TRUE;  /* Connection is dead */
            } else {
                *(SQLUINTEGER *)value_ptr = SQL_CD_FALSE; /* Connection is alive */
            }
        }
        if (string_length_ptr) {
            *string_length_ptr = (SQLINTEGER)sizeof(SQLUINTEGER);
        }
        return SQL_SUCCESS;

    case SQL_ATTR_LOGIN_TIMEOUT:
        if (value_ptr) {
            *(SQLUINTEGER *)value_ptr = connection->login_timeout;
        }
        if (string_length_ptr) {
            *string_length_ptr = (SQLINTEGER)sizeof(SQLUINTEGER);
        }
        return SQL_SUCCESS;

    case SQL_ATTR_CONNECTION_TIMEOUT:
        if (value_ptr) {
            *(SQLUINTEGER *)value_ptr = connection->connection_timeout;
        }
        if (string_length_ptr) {
            *string_length_ptr = (SQLINTEGER)sizeof(SQLUINTEGER);
        }
        return SQL_SUCCESS;

    case SQL_ATTR_ACCESS_MODE:
        if (value_ptr) {
            *(SQLUINTEGER *)value_ptr = connection->access_mode;
        }
        if (string_length_ptr) {
            *string_length_ptr = (SQLINTEGER)sizeof(SQLUINTEGER);
        }
        return SQL_SUCCESS;

    default:
        diagnostics_add_record(&connection->diagnostics,
                               "HY092",  /* Invalid attribute/option identifier */
                               0,
                               "Unsupported connection attribute.");
        return SQL_ERROR;
    }
}

/**
 * SQLEndTran — Request a commit or rollback operation for all active transactions.
 *
 * Commits or rolls back the current transaction on a connection handle or
 * all connections associated with an environment handle. When autocommit is
 * OFF, this is the only way to end a transaction (aside from switching
 * autocommit back to ON).
 *
 * Parameters:
 *   handle_type     - SQL_HANDLE_DBC for a single connection, or SQL_HANDLE_ENV
 *                     to commit/rollback all connections on the environment.
 *   handle          - The connection or environment handle.
 *   completion_type - SQL_COMMIT to commit, or SQL_ROLLBACK to roll back.
 *
 * Returns:
 *   SQL_SUCCESS        - Transaction completed (or no transaction was active).
 *   SQL_ERROR          - Commit/rollback failed.
 *   SQL_INVALID_HANDLE - handle is NULL or not a valid handle of the specified type.
 *
 * See: https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlendtran-function
 */
PSQLODBC2_EXPORT SQLRETURN SQL_API
SQLEndTran(SQLSMALLINT handle_type,
           SQLHANDLE   handle,
           SQLSMALLINT completion_type)
{
    if (!handle) {
        return SQL_INVALID_HANDLE;
    }

    switch (handle_type) {
    case SQL_HANDLE_DBC: {
        OdbcConnection *connection = (OdbcConnection *)handle;
        if (connection->magic_number != CONNECTION_MAGIC_NUMBER) {
            return SQL_INVALID_HANDLE;
        }

        diagnostics_clear(&connection->diagnostics);

        if (completion_type == SQL_COMMIT) {
            return connection_commit(connection);
        } else if (completion_type == SQL_ROLLBACK) {
            return connection_rollback(connection);
        } else {
            diagnostics_add_record(&connection->diagnostics,
                                   "HY012",  /* Invalid transaction operation code */
                                   0,
                                   "completion_type must be SQL_COMMIT or SQL_ROLLBACK.");
            return SQL_ERROR;
        }
    }

    case SQL_HANDLE_ENV: {
        OdbcEnvironment *environment = (OdbcEnvironment *)handle;
        if (environment->magic_number != ENVIRONMENT_MAGIC_NUMBER) {
            return SQL_INVALID_HANDLE;
        }

        /* Iterate all connections on this environment and commit/rollback each.
         * Per ODBC spec, if any connection's operation fails, we continue with
         * the rest and return SQL_ERROR at the end. */
        SQLRETURN overall_result = SQL_SUCCESS;

        for (int index = 0; index < MAX_CONNECTIONS_PER_ENVIRONMENT; index++) {
            OdbcConnection *conn = (OdbcConnection *)environment->connections[index];
            if (!conn) {
                continue;
            }

            SQLRETURN result;
            if (completion_type == SQL_COMMIT) {
                result = connection_commit(conn);
            } else if (completion_type == SQL_ROLLBACK) {
                result = connection_rollback(conn);
            } else {
                diagnostics_add_record(&environment->diagnostics,
                                       "HY012", 0,
                                       "completion_type must be SQL_COMMIT or SQL_ROLLBACK.");
                return SQL_ERROR;
            }

            if (result != SQL_SUCCESS) {
                overall_result = SQL_ERROR;
            }
        }

        return overall_result;
    }

    default:
        return SQL_INVALID_HANDLE;
    }
}

/**
 * SQLSetStmtAttr — Set a statement attribute to control statement behavior.
 *
 * Allows applications to configure statement-level options such as cursor type,
 * concurrency, query timeout, max rows, and escape clause scanning. This driver
 * supports only forward-only cursors and read-only concurrency; requests for
 * other types are accepted but silently downgraded with SQL_SUCCESS_WITH_INFO.
 *
 * Parameters:
 *   statement_handle - A valid statement handle.
 *   attribute        - The attribute identifier (SQL_ATTR_CURSOR_TYPE, etc.).
 *   value_ptr        - The attribute value. For integer attributes, this is the
 *                      value cast to SQLPOINTER. For string attributes, this points
 *                      to a null-terminated string.
 *   string_length    - Length of value_ptr if it's a string, or SQL_IS_INTEGER
 *                      for integer values.
 *
 * Returns:
 *   SQL_SUCCESS           - Attribute set successfully.
 *   SQL_SUCCESS_WITH_INFO - Attribute set but driver substituted a different value
 *                           (e.g., DYNAMIC cursor downgraded to FORWARD_ONLY).
 *   SQL_ERROR             - Invalid attribute identifier (SQLSTATE HY092).
 *   SQL_INVALID_HANDLE    - statement_handle is not a valid statement handle.
 *
 * See: https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlsetstmtattr-function
 */
PSQLODBC2_EXPORT SQLRETURN SQL_API
SQLSetStmtAttr(SQLHSTMT   statement_handle,
               SQLINTEGER attribute,
               SQLPOINTER value_ptr,
               SQLINTEGER string_length)
{
    (void)string_length;

    OdbcStatement *statement = (OdbcStatement *)statement_handle;

    if (!statement || statement->magic_number != STATEMENT_MAGIC_NUMBER) {
        return SQL_INVALID_HANDLE;
    }

    diagnostics_clear(&statement->diagnostics);

    SQLULEN value_as_ulen = (SQLULEN)(uintptr_t)value_ptr;

    switch (attribute) {
    case SQL_ATTR_CURSOR_TYPE:
        if (value_as_ulen == SQL_CURSOR_FORWARD_ONLY) {
            statement->cursor_type = SQL_CURSOR_FORWARD_ONLY;
            return SQL_SUCCESS;
        }
        /* All other cursor types (STATIC, KEYSET_DRIVEN, DYNAMIC) are not
         * supported — downgrade to FORWARD_ONLY per ODBC convention. */
        statement->cursor_type = SQL_CURSOR_FORWARD_ONLY;
        diagnostics_add_record(&statement->diagnostics,
                               "01S02",  /* Option value changed */
                               0,
                               "Cursor type changed to forward-only.");
        return SQL_SUCCESS_WITH_INFO;

    case SQL_ATTR_CONCURRENCY:
        if (value_as_ulen == SQL_CONCUR_READ_ONLY) {
            statement->concurrency = SQL_CONCUR_READ_ONLY;
            return SQL_SUCCESS;
        }
        /* Optimistic/pessimistic concurrency not supported — downgrade. */
        statement->concurrency = SQL_CONCUR_READ_ONLY;
        diagnostics_add_record(&statement->diagnostics,
                               "01S02",  /* Option value changed */
                               0,
                               "Concurrency changed to read-only.");
        return SQL_SUCCESS_WITH_INFO;

    case SQL_ATTR_QUERY_TIMEOUT:
        statement->query_timeout_seconds = value_as_ulen;
        return SQL_SUCCESS;

    case SQL_ATTR_MAX_ROWS:
        statement->max_rows = value_as_ulen;
        return SQL_SUCCESS;

    case SQL_ATTR_NOSCAN:
        statement->noscan = value_as_ulen;
        return SQL_SUCCESS;

    case SQL_ATTR_METADATA_ID:
        statement->metadata_id = (value_as_ulen != 0);
        return SQL_SUCCESS;

    case SQL_ATTR_CURSOR_SCROLLABLE:
        if (value_as_ulen == SQL_NONSCROLLABLE) {
            return SQL_SUCCESS;
        }
        /* Scrollable cursors not supported — report downgrade. */
        diagnostics_add_record(&statement->diagnostics,
                               "01S02",  /* Option value changed */
                               0,
                               "Scrollable cursors are not supported; using non-scrollable.");
        return SQL_SUCCESS_WITH_INFO;

    case SQL_ATTR_CURSOR_SENSITIVITY:
        if (value_as_ulen == SQL_UNSPECIFIED) {
            return SQL_SUCCESS;
        }
        /* SENSITIVE/INSENSITIVE not supported — downgrade to UNSPECIFIED. */
        diagnostics_add_record(&statement->diagnostics,
                               "01S02",  /* Option value changed */
                               0,
                               "Cursor sensitivity changed to unspecified.");
        return SQL_SUCCESS_WITH_INFO;

    default:
        diagnostics_add_record(&statement->diagnostics,
                               "HY092",  /* Invalid attribute/option identifier */
                               0,
                               "Unsupported statement attribute.");
        return SQL_ERROR;
    }
}

/**
 * SQLGetStmtAttr — Retrieve the current value of a statement attribute.
 *
 * Returns the current setting of the specified statement attribute. Integer
 * attributes are written as SQLULEN values to value_ptr. Descriptor handle
 * attributes return SQL_NULL_HANDLE (descriptors are not yet implemented).
 *
 * Parameters:
 *   statement_handle  - A valid statement handle.
 *   attribute         - The attribute identifier to query.
 *   value_ptr         - Output buffer for the attribute value.
 *   buffer_length     - Size of value_ptr buffer (for string attributes).
 *   string_length_ptr - Output: actual byte length of the value (NULL for integer attrs).
 *
 * Returns:
 *   SQL_SUCCESS        - Attribute value retrieved successfully.
 *   SQL_ERROR          - Invalid or unsupported attribute (SQLSTATE HY092).
 *   SQL_INVALID_HANDLE - statement_handle is not a valid statement handle.
 *
 * See: https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlgetstmtattr-function
 */
PSQLODBC2_EXPORT SQLRETURN SQL_API
SQLGetStmtAttr(SQLHSTMT    statement_handle,
               SQLINTEGER  attribute,
               SQLPOINTER  value_ptr,
               SQLINTEGER  buffer_length,
               SQLINTEGER *string_length_ptr)
{
    (void)buffer_length;

    OdbcStatement *statement = (OdbcStatement *)statement_handle;

    if (!statement || statement->magic_number != STATEMENT_MAGIC_NUMBER) {
        return SQL_INVALID_HANDLE;
    }

    diagnostics_clear(&statement->diagnostics);

    switch (attribute) {
    case SQL_ATTR_CURSOR_TYPE:
        if (value_ptr) {
            *(SQLULEN *)value_ptr = statement->cursor_type;
        }
        if (string_length_ptr) {
            *string_length_ptr = (SQLINTEGER)sizeof(SQLULEN);
        }
        return SQL_SUCCESS;

    case SQL_ATTR_CONCURRENCY:
        if (value_ptr) {
            *(SQLULEN *)value_ptr = statement->concurrency;
        }
        if (string_length_ptr) {
            *string_length_ptr = (SQLINTEGER)sizeof(SQLULEN);
        }
        return SQL_SUCCESS;

    case SQL_ATTR_QUERY_TIMEOUT:
        if (value_ptr) {
            *(SQLULEN *)value_ptr = statement->query_timeout_seconds;
        }
        if (string_length_ptr) {
            *string_length_ptr = (SQLINTEGER)sizeof(SQLULEN);
        }
        return SQL_SUCCESS;

    case SQL_ATTR_MAX_ROWS:
        if (value_ptr) {
            *(SQLULEN *)value_ptr = statement->max_rows;
        }
        if (string_length_ptr) {
            *string_length_ptr = (SQLINTEGER)sizeof(SQLULEN);
        }
        return SQL_SUCCESS;

    case SQL_ATTR_NOSCAN:
        if (value_ptr) {
            *(SQLULEN *)value_ptr = statement->noscan;
        }
        if (string_length_ptr) {
            *string_length_ptr = (SQLINTEGER)sizeof(SQLULEN);
        }
        return SQL_SUCCESS;

    case SQL_ATTR_METADATA_ID:
        if (value_ptr) {
            *(SQLULEN *)value_ptr = statement->metadata_id ? 1 : 0;
        }
        if (string_length_ptr) {
            *string_length_ptr = (SQLINTEGER)sizeof(SQLULEN);
        }
        return SQL_SUCCESS;

    case SQL_ATTR_CURSOR_SCROLLABLE:
        /* Only forward-only cursors are supported */
        if (value_ptr) {
            *(SQLULEN *)value_ptr = SQL_NONSCROLLABLE;
        }
        if (string_length_ptr) {
            *string_length_ptr = (SQLINTEGER)sizeof(SQLULEN);
        }
        return SQL_SUCCESS;

    case SQL_ATTR_CURSOR_SENSITIVITY:
        if (value_ptr) {
            *(SQLULEN *)value_ptr = SQL_UNSPECIFIED;
        }
        if (string_length_ptr) {
            *string_length_ptr = (SQLINTEGER)sizeof(SQLULEN);
        }
        return SQL_SUCCESS;

    case SQL_ATTR_ROW_NUMBER:
        /* Return 1-based row number, or 0 if no cursor is positioned */
        if (value_ptr) {
            if (statement->state == STATEMENT_STATE_HAS_CURSOR &&
                statement->current_row_position >= 0) {
                *(SQLULEN *)value_ptr = (SQLULEN)(statement->current_row_position + 1);
            } else {
                *(SQLULEN *)value_ptr = 0;
            }
        }
        if (string_length_ptr) {
            *string_length_ptr = (SQLINTEGER)sizeof(SQLULEN);
        }
        return SQL_SUCCESS;

    case SQL_ATTR_IMP_ROW_DESC:
    case SQL_ATTR_IMP_PARAM_DESC:
    case SQL_ATTR_APP_ROW_DESC:
    case SQL_ATTR_APP_PARAM_DESC:
        /* Descriptor handles are not yet implemented */
        if (value_ptr) {
            *(SQLHANDLE *)value_ptr = SQL_NULL_HANDLE;
        }
        if (string_length_ptr) {
            *string_length_ptr = (SQLINTEGER)sizeof(SQLHANDLE);
        }
        return SQL_SUCCESS;

    default:
        diagnostics_add_record(&statement->diagnostics,
                               "HY092",  /* Invalid attribute/option identifier */
                               0,
                               "Unsupported statement attribute.");
        return SQL_ERROR;
    }
}

/**
 * SQLSetEnvAttr — Set an environment attribute.
 *
 * The most critical attribute is SQL_ATTR_ODBC_VERSION which must be set
 * before allocating connection handles.
 *
 * See: https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlsetenvattr-function
 */
PSQLODBC2_EXPORT SQLRETURN SQL_API
SQLSetEnvAttr(SQLHENV     environment_handle,
              SQLINTEGER  attribute,
              SQLPOINTER  value_ptr,
              SQLINTEGER  string_length)
{
    (void)string_length;

    OdbcEnvironment *environment = (OdbcEnvironment *)environment_handle;

    if (!environment || environment->magic_number != ENVIRONMENT_MAGIC_NUMBER) {
        return SQL_INVALID_HANDLE;
    }

    switch (attribute) {
    case SQL_ATTR_ODBC_VERSION:
        environment->odbc_version = (int)(intptr_t)value_ptr;
        return SQL_SUCCESS;

    case SQL_ATTR_CONNECTION_POOLING:
    case SQL_ATTR_CP_MATCH:
    case SQL_ATTR_OUTPUT_NTS:
        return SQL_SUCCESS;

    default:
        diagnostics_add_record(&environment->diagnostics,
                               "HY092", 0,
                               "Invalid or unsupported environment attribute.");
        return SQL_ERROR;
    }
}

/**
 * SQLGetEnvAttr — Get an environment attribute value.
 *
 * See: https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlgetenvattr-function
 */
PSQLODBC2_EXPORT SQLRETURN SQL_API
SQLGetEnvAttr(SQLHENV     environment_handle,
              SQLINTEGER  attribute,
              SQLPOINTER  value_ptr,
              SQLINTEGER  buffer_length,
              SQLINTEGER *string_length_ptr)
{
    (void)buffer_length;
    (void)string_length_ptr;

    OdbcEnvironment *environment = (OdbcEnvironment *)environment_handle;

    if (!environment || environment->magic_number != ENVIRONMENT_MAGIC_NUMBER) {
        return SQL_INVALID_HANDLE;
    }

    switch (attribute) {
    case SQL_ATTR_ODBC_VERSION:
        if (value_ptr) {
            *(SQLINTEGER *)value_ptr = (SQLINTEGER)environment->odbc_version;
        }
        return SQL_SUCCESS;

    case SQL_ATTR_CONNECTION_POOLING:
        if (value_ptr) {
            *(SQLUINTEGER *)value_ptr = SQL_CP_OFF;
        }
        return SQL_SUCCESS;

    case SQL_ATTR_OUTPUT_NTS:
        if (value_ptr) {
            *(SQLUINTEGER *)value_ptr = SQL_TRUE;
        }
        return SQL_SUCCESS;

    default:
        diagnostics_add_record(&environment->diagnostics,
                               "HY092", 0,
                               "Invalid or unsupported environment attribute.");
        return SQL_ERROR;
    }
}

/* PostgreSQL encodes an interval's field set (YEAR, MONTH, DAY ... SECOND) in
 * the high bits of its type modifier. These bit positions match the server's
 * INTERVAL_MASK definitions and let us report the precise "interval <fields>"
 * type name that the application expects. */
#define INTERVAL_MONTH_BIT  (1 << 17)
#define INTERVAL_YEAR_BIT   (1 << 18)
#define INTERVAL_DAY_BIT    (1 << 19)
#define INTERVAL_HOUR_BIT   (1 << 26)
#define INTERVAL_MINUTE_BIT (1 << 27)
#define INTERVAL_SECOND_BIT (1 << 28)

/*
 * Map a PostgreSQL interval type modifier to its SQL type-name string
 * (e.g. "interval day to second"). Returns the generic "interval" when the
 * modifier does not restrict the field set (typmod -1) or is unrecognized.
 */
static const char *interval_type_name(int type_modifier)
{
    if (type_modifier == -1) {
        return "interval";
    }
    if (type_modifier & INTERVAL_YEAR_BIT) {
        return (type_modifier & INTERVAL_MONTH_BIT) ? "interval year to month"
                                                    : "interval year";
    }
    if (type_modifier & INTERVAL_MONTH_BIT) {
        return "interval month";
    }
    if (type_modifier & INTERVAL_DAY_BIT) {
        if (type_modifier & INTERVAL_SECOND_BIT) { return "interval day to second"; }
        if (type_modifier & INTERVAL_MINUTE_BIT) { return "interval day to minute"; }
        if (type_modifier & INTERVAL_HOUR_BIT)   { return "interval day to hour"; }
        return "interval day";
    }
    if (type_modifier & INTERVAL_HOUR_BIT) {
        if (type_modifier & INTERVAL_SECOND_BIT) { return "interval hour to second"; }
        if (type_modifier & INTERVAL_MINUTE_BIT) { return "interval hour to minute"; }
        return "interval hour";
    }
    if (type_modifier & INTERVAL_MINUTE_BIT) {
        return (type_modifier & INTERVAL_SECOND_BIT) ? "interval minute to second"
                                                     : "interval minute";
    }
    if (type_modifier & INTERVAL_SECOND_BIT) {
        return "interval second";
    }
    return "interval";
}

/*
 * Scan the fetched rows of a result column and return the maximum value length
 * in characters. Used for UnknownSizes=LONGEST reporting. Returns 0 when there
 * are no rows or the column is entirely NULL.
 *
 * Note: this reports byte length from libpq; for the octet-length computation
 * that is exactly what we want, and for character counts it is an upper bound.
 */
static int longest_value_length(PGresult *result, int column_index)
{
    if (!result) {
        return 0;
    }
    int longest = 0;
    int row_count = PQntuples(result);
    for (int row = 0; row < row_count; row++) {
        if (PQgetisnull(result, row, column_index)) {
            continue;
        }
        int length = PQgetlength(result, row, column_index);
        if (length > longest) {
            longest = length;
        }
    }
    return longest;
}

/*
 * Compute the column size, in characters, for a character/unbounded type,
 * honoring the connection's UnknownSizes setting. This mirrors the original
 * psqlodbc getCharColumnSizeX() logic:
 *   - A declared length (typmod > 0) is always used as-is.
 *   - Otherwise the value depends on UnknownSizes: MAX reports the configured
 *     maximum for the type; LONGEST reports the longest value in the result;
 *     DONTKNOW reports "unknown" (returns -1).
 */
static int char_column_size_chars(unsigned int postgres_oid, int type_modifier,
                                   const OdbcConnection *connection,
                                   PGresult *result, int column_index)
{
    /* varchar(n)/char(n) declare their limit in the type modifier. */
    if (type_modifier > 4 &&
        (postgres_oid == PG_TYPE_VARCHAR || postgres_oid == PG_TYPE_BPCHAR)) {
        return type_modifier - 4;
    }

    int unknown_sizes = connection ? connection->info.unknown_sizes : UNKNOWN_SIZES_MAX;
    int max_varchar = connection ? connection->info.max_varchar_size
                                 : DEFAULT_MAX_VARCHAR_SIZE;

    /* text/longvarchar use the larger long-varchar limit; varchar/char use the
     * varchar limit. (text_as_longvarchar is on by default in the original.) */
    int max_size = (postgres_oid == PG_TYPE_TEXT) ? DEFAULT_MAX_LONGVARCHAR_SIZE
                                                   : max_varchar;

    /* Only LONGEST changes the reported octet length; both MAX and DONTKNOW
     * report the configured maximum size for the type (matching the original
     * driver's observed octet-length behavior). */
    if (unknown_sizes == UNKNOWN_SIZES_LONGEST) {
        int longest = longest_value_length(result, column_index);
        if (longest > 0) {
            return longest;
        }
        /* No data to measure — fall back to the maximum size. */
    }
    return max_size;
}

/*
 * Compute SQL_DESC_OCTET_LENGTH (maximum byte size) for a column, mirroring the
 * original psqlodbc pgtype_attr_transfer_octet_length():
 *   - Fixed-width types (integers, floats, dates, interval, etc.) transfer as
 *     their native binary/text form and report octet length 0.
 *   - Character types report coef * column_size, where coef is the client
 *     encoding's max bytes per character, capped so a value that fits in
 *     max_varchar characters is not inflated past max_varchar bytes.
 */
static SQLLEN compute_octet_length(unsigned int postgres_oid, int type_modifier,
                                   const OdbcConnection *connection,
                                   PGresult *result, int column_index)
{
    switch (postgres_oid) {
    case PG_TYPE_VARCHAR:
    case PG_TYPE_BPCHAR:
    case PG_TYPE_TEXT: {
        int column_size = char_column_size_chars(postgres_oid, type_modifier,
                                                  connection, result, column_index);
        if (column_size < 0) {
            return 0;  /* size unknown */
        }
        int coef = connection ? connection->max_bytes_per_char : 1;
        if (coef <= 1) {
            return column_size;
        }
        int max_varchar = connection ? connection->info.max_varchar_size
                                     : DEFAULT_MAX_VARCHAR_SIZE;
        /* Don't inflate a value that already fits in max_varchar characters
         * beyond max_varchar bytes — matches the original's cap. */
        if (column_size <= max_varchar && column_size * coef > max_varchar) {
            return max_varchar;
        }
        return (SQLLEN)coef * column_size;
    }
    case PG_TYPE_BYTEA:
        /* bytea has no fixed maximum; report unknown. */
        return 0;
    default:
        /* Fixed-width and date/time/interval types transfer without a byte cap. */
        return 0;
    }
}

/**
 * SQLColAttribute — Retrieve a column attribute from the result set metadata.
 *
 * Returns descriptor information (name, type, display size, nullability, etc.)
 * for a specific column in the result set. String attributes are written to
 * character_attribute; numeric attributes are written to numeric_attribute.
 *
 * Parameters:
 *   statement_handle    - A valid statement handle with an available result set.
 *   column_number       - The column to describe (1-based).
 *   field_identifier    - Which attribute to retrieve (SQL_DESC_NAME, SQL_DESC_TYPE, etc.).
 *   character_attribute - Output buffer for string-valued attributes.
 *   buffer_length       - Size of character_attribute buffer in bytes.
 *   string_length       - Output: actual length of string data (excluding null terminator).
 *   numeric_attribute   - Output: value for numeric-valued attributes.
 *
 * Returns:
 *   SQL_SUCCESS           - Attribute retrieved successfully.
 *   SQL_SUCCESS_WITH_INFO - String attribute was truncated.
 *   SQL_ERROR             - No result set, invalid column, or invalid field identifier.
 *   SQL_INVALID_HANDLE    - statement_handle is not a valid statement handle.
 *
 * See: https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlcolattribute-function
 */
PSQLODBC2_EXPORT SQLRETURN SQL_API
SQLColAttribute(SQLHSTMT     statement_handle,
                SQLUSMALLINT column_number,
                SQLUSMALLINT field_identifier,
                SQLPOINTER   character_attribute,
                SQLSMALLINT  buffer_length,
                SQLSMALLINT *string_length,
                SQLLEN      *numeric_attribute)
{
    OdbcStatement *statement = (OdbcStatement *)statement_handle;

    if (!statement || statement->magic_number != STATEMENT_MAGIC_NUMBER) {
        return SQL_INVALID_HANDLE;
    }

    diagnostics_clear(&statement->diagnostics);

    /* Use the execution result if available, otherwise fall back to
     * the describe_result from PQdescribePrepared (pre-execute metadata). */
    PGresult *metadata_source = statement->current_result;
    if (!metadata_source) {
        metadata_source = statement->describe_result;
    }

    if (!metadata_source) {
        diagnostics_add_record(&statement->diagnostics,
                               "HY010",  /* Function sequence error */
                               0,
                               "No result set available for SQLColAttribute.");
        return SQL_ERROR;
    }

    int total_columns = PQnfields(metadata_source);

    /* SQL_DESC_COUNT is special — it doesn't require a valid column_number */
    if (field_identifier == SQL_DESC_COUNT) {
        if (numeric_attribute) {
            *numeric_attribute = (SQLLEN)total_columns;
        }
        return SQL_SUCCESS;
    }

    /* Validate column number (1-based) */
    if (column_number < 1 || column_number > (SQLUSMALLINT)total_columns) {
        diagnostics_add_record(&statement->diagnostics,
                               "07009",  /* Invalid descriptor index */
                               0,
                               "Column number is out of range in SQLColAttribute.");
        return SQL_ERROR;
    }

    int column_index = (int)(column_number - 1);  /* libpq uses 0-based */
    unsigned int postgres_oid = (unsigned int)PQftype(metadata_source, column_index);
    int type_modifier = PQfmod(metadata_source, column_index);

    /* --- Dispatch on field_identifier --- */

    switch (field_identifier) {
    /* --- String attributes --- */
    case SQL_DESC_LABEL:
    case SQL_DESC_NAME:
    case SQL_COLUMN_NAME: { /* ODBC 2.x compatibility alias */
        const char *col_name = PQfname(metadata_source, column_index);
        SQLSMALLINT name_len = (SQLSMALLINT)strlen(col_name);
        if (string_length) { *string_length = name_len; }
        if (character_attribute && buffer_length > 0) {
            SQLSMALLINT copy_len = (name_len < buffer_length) ? name_len : (buffer_length - 1);
            memcpy(character_attribute, col_name, (size_t)copy_len);
            ((char *)character_attribute)[copy_len] = '\0';
            if (name_len >= buffer_length) {
                diagnostics_add_record(&statement->diagnostics,
                                       "01004", 0,
                                       "String data truncated in SQLColAttribute.");
                return SQL_SUCCESS_WITH_INFO;
            }
        }
        return SQL_SUCCESS;
    }

    case SQL_DESC_TYPE_NAME: {
        /* Interval reports a field-set-specific name (e.g. "interval day to
         * second") decoded from its type modifier. */
        const char *type_name = (postgres_oid == PG_TYPE_INTERVAL)
                                    ? interval_type_name(type_modifier)
                                    : type_mapping_get_type_name(postgres_oid);
        SQLSMALLINT name_len = (SQLSMALLINT)strlen(type_name);
        if (string_length) { *string_length = name_len; }
        if (character_attribute && buffer_length > 0) {
            SQLSMALLINT copy_len = (name_len < buffer_length) ? name_len : (buffer_length - 1);
            memcpy(character_attribute, type_name, (size_t)copy_len);
            ((char *)character_attribute)[copy_len] = '\0';
            if (name_len >= buffer_length) {
                diagnostics_add_record(&statement->diagnostics,
                                       "01004", 0,
                                       "String data truncated in SQLColAttribute.");
                return SQL_SUCCESS_WITH_INFO;
            }
        }
        return SQL_SUCCESS;
    }

    case SQL_DESC_TABLE_NAME:
    case SQL_DESC_SCHEMA_NAME:
    case SQL_DESC_CATALOG_NAME:
    case SQL_DESC_LOCAL_TYPE_NAME: {
        /* These would require expensive pg_attribute lookups; return empty */
        if (string_length) { *string_length = 0; }
        if (character_attribute && buffer_length > 0) {
            ((char *)character_attribute)[0] = '\0';
        }
        return SQL_SUCCESS;
    }

    case SQL_DESC_LITERAL_PREFIX:
    case SQL_DESC_LITERAL_SUFFIX: {
        /* String types use quote delimiters; numeric types use none */
        SQLSMALLINT sql_type = type_mapping_get_sql_type(postgres_oid);
        const char *literal = "";
        if (sql_type == SQL_CHAR || sql_type == SQL_VARCHAR ||
            sql_type == SQL_LONGVARCHAR || sql_type == SQL_TYPE_DATE ||
            sql_type == SQL_TYPE_TIME || sql_type == SQL_TYPE_TIMESTAMP) {
            literal = "'";
        }
        SQLSMALLINT lit_len = (SQLSMALLINT)strlen(literal);
        if (string_length) { *string_length = lit_len; }
        if (character_attribute && buffer_length > 0) {
            SQLSMALLINT copy_len = (lit_len < buffer_length) ? lit_len : (buffer_length - 1);
            memcpy(character_attribute, literal, (size_t)copy_len);
            ((char *)character_attribute)[copy_len] = '\0';
        }
        return SQL_SUCCESS;
    }

    /* --- Numeric attributes --- */
    case SQL_DESC_TYPE:
    case SQL_DESC_CONCISE_TYPE:
        if (numeric_attribute) {
            *numeric_attribute = (SQLLEN)type_mapping_get_sql_type(postgres_oid);
        }
        return SQL_SUCCESS;

    case SQL_DESC_DISPLAY_SIZE: {
        SQLULEN col_size = type_mapping_get_column_size(postgres_oid, type_modifier);
        /* For variable-length types with no declared limit, use a sensible default */
        if (col_size == 0) { col_size = 255; }
        if (numeric_attribute) { *numeric_attribute = (SQLLEN)col_size; }
        return SQL_SUCCESS;
    }

    case SQL_DESC_OCTET_LENGTH: {
        if (numeric_attribute) {
            *numeric_attribute = compute_octet_length(postgres_oid, type_modifier,
                                                      statement->parent_connection,
                                                      statement->current_result,
                                                      column_index);
        }
        return SQL_SUCCESS;
    }

    case SQL_DESC_LENGTH: {
        SQLULEN col_size = type_mapping_get_column_size(postgres_oid, type_modifier);
        if (col_size == 0) { col_size = 255; }
        if (numeric_attribute) { *numeric_attribute = (SQLLEN)col_size; }
        return SQL_SUCCESS;
    }

    case SQL_DESC_PRECISION:
    case SQL_COLUMN_PRECISION: {
        SQLULEN col_size = type_mapping_get_column_size(postgres_oid, type_modifier);
        if (col_size == 0) { col_size = 255; }
        if (numeric_attribute) { *numeric_attribute = (SQLLEN)col_size; }
        return SQL_SUCCESS;
    }

    case SQL_DESC_SCALE:
    case SQL_COLUMN_SCALE:
        if (numeric_attribute) {
            *numeric_attribute = (SQLLEN)type_mapping_get_decimal_digits(postgres_oid, type_modifier);
        }
        return SQL_SUCCESS;

    case SQL_DESC_NULLABLE:
    case SQL_COLUMN_NULLABLE:
        if (numeric_attribute) { *numeric_attribute = SQL_NULLABLE_UNKNOWN; }
        return SQL_SUCCESS;

    case SQL_DESC_UNSIGNED: {
        /* Numeric types in PostgreSQL are all signed */
        SQLSMALLINT sql_type = type_mapping_get_sql_type(postgres_oid);
        int is_unsigned = SQL_FALSE;
        if (sql_type == SQL_CHAR || sql_type == SQL_VARCHAR ||
            sql_type == SQL_LONGVARCHAR || sql_type == SQL_TYPE_DATE ||
            sql_type == SQL_TYPE_TIME || sql_type == SQL_TYPE_TIMESTAMP ||
            sql_type == SQL_LONGVARBINARY || sql_type == SQL_GUID) {
            /* Non-numeric types: the "unsigned" concept doesn't apply, but
             * ODBC convention is to return SQL_TRUE for non-applicable types */
            is_unsigned = SQL_TRUE;
        }
        if (numeric_attribute) { *numeric_attribute = is_unsigned; }
        return SQL_SUCCESS;
    }

    case SQL_DESC_AUTO_UNIQUE_VALUE:
        if (numeric_attribute) { *numeric_attribute = SQL_FALSE; }
        return SQL_SUCCESS;

    case SQL_DESC_UPDATABLE:
        if (numeric_attribute) { *numeric_attribute = SQL_ATTR_READWRITE_UNKNOWN; }
        return SQL_SUCCESS;

    case SQL_DESC_SEARCHABLE:
        if (numeric_attribute) { *numeric_attribute = SQL_PRED_SEARCHABLE; }
        return SQL_SUCCESS;

    case SQL_DESC_CASE_SENSITIVE: {
        SQLSMALLINT sql_type = type_mapping_get_sql_type(postgres_oid);
        int is_case_sensitive = SQL_FALSE;
        if (sql_type == SQL_CHAR || sql_type == SQL_VARCHAR ||
            sql_type == SQL_LONGVARCHAR) {
            is_case_sensitive = SQL_TRUE;
        }
        if (numeric_attribute) { *numeric_attribute = is_case_sensitive; }
        return SQL_SUCCESS;
    }

    case SQL_DESC_FIXED_PREC_SCALE:
        if (numeric_attribute) { *numeric_attribute = SQL_FALSE; }
        return SQL_SUCCESS;

    case SQL_DESC_NUM_PREC_RADIX: {
        SQLSMALLINT sql_type = type_mapping_get_sql_type(postgres_oid);
        int radix = 0;
        if (sql_type == SQL_SMALLINT || sql_type == SQL_INTEGER ||
            sql_type == SQL_BIGINT || sql_type == SQL_REAL ||
            sql_type == SQL_DOUBLE || sql_type == SQL_NUMERIC) {
            radix = 10;
        }
        if (numeric_attribute) { *numeric_attribute = radix; }
        return SQL_SUCCESS;
    }

    case SQL_DESC_COUNT:
        if (numeric_attribute) { *numeric_attribute = (SQLLEN)total_columns; }
        return SQL_SUCCESS;

    default:
        diagnostics_add_record(&statement->diagnostics,
                               "HY091",  /* Invalid descriptor field identifier */
                               0,
                               "Unsupported field identifier in SQLColAttribute.");
        return SQL_ERROR;
    }
}

/**
 * SQLGetInfo — Return general information about the driver and data source.
 *
 * Applications and the Driver Manager call this to determine driver capabilities,
 * data source characteristics, and supported SQL grammar. Different info_type
 * values return either a string (copied to info_value with length in string_length)
 * or an integer (written as SQLUINTEGER/SQLUSMALLINT to info_value).
 *
 * Parameters:
 *   connection_handle - A valid connection handle.
 *   info_type         - The information type requested (SQL_DBMS_NAME, SQL_DRIVER_VER, etc.).
 *   info_value        - Output buffer for the requested information.
 *   buffer_length     - Size of info_value buffer in bytes (for string types).
 *   string_length     - Output: actual byte length of string data (for string types).
 *
 * Returns:
 *   SQL_SUCCESS           - Information retrieved successfully.
 *   SQL_SUCCESS_WITH_INFO - String data was truncated.
 *   SQL_ERROR             - Invalid info_type or other error.
 *   SQL_INVALID_HANDLE    - connection_handle is not a valid connection handle.
 *
 * See: https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlgetinfo-function
 */
PSQLODBC2_EXPORT SQLRETURN SQL_API
SQLGetInfo(SQLHDBC      connection_handle,
           SQLUSMALLINT info_type,
           SQLPOINTER   info_value,
           SQLSMALLINT  buffer_length,
           SQLSMALLINT *string_length)
{
    OdbcConnection *connection = (OdbcConnection *)connection_handle;

    if (!connection || connection->magic_number != CONNECTION_MAGIC_NUMBER) {
        return SQL_INVALID_HANDLE;
    }

    diagnostics_clear(&connection->diagnostics);

    /* Internal helper: copy a string to the info_value buffer and report length */
    #define GETINFO_RETURN_STRING(str) do { \
        const char *_s = (str) ? (str) : ""; \
        SQLSMALLINT _len = (SQLSMALLINT)strlen(_s); \
        if (string_length) { *string_length = _len; } \
        if (info_value && buffer_length > 0) { \
            SQLSMALLINT _copy = (_len < buffer_length) ? _len : (buffer_length - 1); \
            memcpy(info_value, _s, (size_t)_copy); \
            ((char *)info_value)[_copy] = '\0'; \
            if (_len >= buffer_length) { \
                diagnostics_add_record(&connection->diagnostics, \
                                       "01004", 0, \
                                       "String data truncated in SQLGetInfo."); \
                return SQL_SUCCESS_WITH_INFO; \
            } \
        } \
        return SQL_SUCCESS; \
    } while (0)

    #define GETINFO_RETURN_UINT(val) do { \
        if (info_value) { *(SQLUINTEGER *)info_value = (SQLUINTEGER)(val); } \
        if (string_length) { *string_length = (SQLSMALLINT)sizeof(SQLUINTEGER); } \
        return SQL_SUCCESS; \
    } while (0)

    #define GETINFO_RETURN_USHORT(val) do { \
        if (info_value) { *(SQLUSMALLINT *)info_value = (SQLUSMALLINT)(val); } \
        if (string_length) { *string_length = (SQLSMALLINT)sizeof(SQLUSMALLINT); } \
        return SQL_SUCCESS; \
    } while (0)

    switch (info_type) {
    /* --- String info types --- */
    case SQL_DBMS_NAME:
        GETINFO_RETURN_STRING("PostgreSQL");

    case SQL_DBMS_VER: {
        /* Format: "MM.mm.pppp" from PQserverVersion.
         * PG >= 10: version = major*10000 + revision (e.g. 180004 = 18.4)
         *   → format as "major.revision.0" so ODBC sees "18.04.0000"
         * PG < 10: version = major*10000 + minor*100 + patch (e.g. 90608 = 9.6.8)
         *   → format as "major.minor.patch" so ODBC sees "09.06.0008" */
        char version_buffer[32];
        int server_ver = 0;
        if (connection->libpq_connection) {
            server_ver = PQserverVersion(connection->libpq_connection);
        }
        int major = server_ver / 10000;
        if (major >= 10) {
            int revision = server_ver % 10000;
            snprintf(version_buffer, sizeof(version_buffer),
                     "%02d.%02d.%04d", major, revision, 0);
        } else {
            int minor = (server_ver / 100) % 100;
            int patch = server_ver % 100;
            snprintf(version_buffer, sizeof(version_buffer),
                     "%02d.%02d.%04d", major, minor, patch);
        }
        GETINFO_RETURN_STRING(version_buffer);
    }

    case SQL_DRIVER_NAME:
        GETINFO_RETURN_STRING("psqlodbc2w");

    case SQL_DRIVER_VER:
        GETINFO_RETURN_STRING("00.01.0000");

    case SQL_DRIVER_ODBC_VER:
        GETINFO_RETURN_STRING("03.80");

    case SQL_IDENTIFIER_QUOTE_CHAR:
        GETINFO_RETURN_STRING("\"");

    case SQL_CATALOG_NAME_SEPARATOR:
        GETINFO_RETURN_STRING(".");

    case SQL_CATALOG_TERM:
        GETINFO_RETURN_STRING("database");

    case SQL_SCHEMA_TERM:
        GETINFO_RETURN_STRING("schema");

    case SQL_TABLE_TERM:
        GETINFO_RETURN_STRING("table");

    case SQL_PROCEDURE_TERM:
        GETINFO_RETURN_STRING("function");

    case SQL_SEARCH_PATTERN_ESCAPE:
        GETINFO_RETURN_STRING("\\");

    case SQL_DATA_SOURCE_NAME:
        GETINFO_RETURN_STRING("");

    case SQL_SERVER_NAME:
        if (connection->libpq_connection) {
            GETINFO_RETURN_STRING(PQhost(connection->libpq_connection));
        }
        GETINFO_RETURN_STRING("");

    case SQL_DATABASE_NAME:
        if (connection->libpq_connection) {
            GETINFO_RETURN_STRING(PQdb(connection->libpq_connection));
        }
        GETINFO_RETURN_STRING("");

    case SQL_USER_NAME:
        if (connection->libpq_connection) {
            GETINFO_RETURN_STRING(PQuser(connection->libpq_connection));
        }
        GETINFO_RETURN_STRING("");

    case SQL_COLUMN_ALIAS:
        GETINFO_RETURN_STRING("Y");

    case SQL_KEYWORDS:
        GETINFO_RETURN_STRING("");

    case SQL_SPECIAL_CHARACTERS:
        GETINFO_RETURN_STRING("");

    case SQL_CATALOG_NAME:
        GETINFO_RETURN_STRING("Y");

    case SQL_ORDER_BY_COLUMNS_IN_SELECT:
        GETINFO_RETURN_STRING("N");

    case SQL_INTEGRITY:
        GETINFO_RETURN_STRING("Y");

    case SQL_ACCESSIBLE_TABLES:
        GETINFO_RETURN_STRING("N");

    case SQL_ACCESSIBLE_PROCEDURES:
        GETINFO_RETURN_STRING("N");

    case SQL_EXPRESSIONS_IN_ORDERBY:
        GETINFO_RETURN_STRING("Y");

    case SQL_LIKE_ESCAPE_CLAUSE:
        GETINFO_RETURN_STRING("Y");

    case SQL_MULT_RESULT_SETS:
        GETINFO_RETURN_STRING("Y");

    case SQL_MULTIPLE_ACTIVE_TXN:
        GETINFO_RETURN_STRING("Y");

    case SQL_OUTER_JOINS:
        GETINFO_RETURN_STRING("Y");

    case SQL_PROCEDURES:
        GETINFO_RETURN_STRING("Y");

    case SQL_ROW_UPDATES:
        GETINFO_RETURN_STRING("N");

    case SQL_NEED_LONG_DATA_LEN:
        GETINFO_RETURN_STRING("N");

    case SQL_MAX_ROW_SIZE_INCLUDES_LONG:
        GETINFO_RETURN_STRING("N");

    case SQL_DATA_SOURCE_READ_ONLY:
        GETINFO_RETURN_STRING("N");

    case SQL_DESCRIBE_PARAMETER:
        GETINFO_RETURN_STRING("Y");

    /* --- Integer info types (SQLUSMALLINT) --- */
    case SQL_MAX_IDENTIFIER_LEN:
        GETINFO_RETURN_USHORT(63);

    case SQL_MAX_COLUMNS_IN_TABLE:
        GETINFO_RETURN_USHORT(1600);

    case SQL_MAX_CATALOG_NAME_LEN:
        GETINFO_RETURN_USHORT(63);

    case SQL_MAX_SCHEMA_NAME_LEN:
        GETINFO_RETURN_USHORT(63);

    case SQL_MAX_TABLE_NAME_LEN:
        GETINFO_RETURN_USHORT(63);

    case SQL_MAX_COLUMN_NAME_LEN:
        GETINFO_RETURN_USHORT(63);

    case SQL_MAX_CURSOR_NAME_LEN:
        GETINFO_RETURN_USHORT(63);

    case SQL_MAX_PROCEDURE_NAME_LEN:
        GETINFO_RETURN_USHORT(63);

    case SQL_MAX_USER_NAME_LEN:
        GETINFO_RETURN_USHORT(63);

    /* --- Integer info types (SQLUINTEGER) --- */
    case SQL_GETDATA_EXTENSIONS:
        GETINFO_RETURN_UINT(SQL_GD_ANY_COLUMN | SQL_GD_ANY_ORDER);

    case SQL_TXN_CAPABLE:
        GETINFO_RETURN_USHORT(SQL_TC_ALL);

    case SQL_CONCAT_NULL_BEHAVIOR:
        GETINFO_RETURN_USHORT(SQL_CB_NULL);

    case SQL_CURSOR_COMMIT_BEHAVIOR:
        GETINFO_RETURN_USHORT(SQL_CB_CLOSE);

    case SQL_CURSOR_ROLLBACK_BEHAVIOR:
        GETINFO_RETURN_USHORT(SQL_CB_CLOSE);

    case SQL_DEFAULT_TXN_ISOLATION:
        GETINFO_RETURN_UINT(SQL_TXN_READ_COMMITTED);

    case SQL_TXN_ISOLATION_OPTION:
        GETINFO_RETURN_UINT(SQL_TXN_READ_COMMITTED | SQL_TXN_REPEATABLE_READ |
                            SQL_TXN_SERIALIZABLE | SQL_TXN_READ_UNCOMMITTED);

    case SQL_QUOTED_IDENTIFIER_CASE:
        GETINFO_RETURN_USHORT(SQL_IC_SENSITIVE);

    case SQL_NON_NULLABLE_COLUMNS:
        GETINFO_RETURN_USHORT(SQL_NNC_NON_NULL);

    case SQL_CORRELATION_NAME:
        GETINFO_RETURN_USHORT(SQL_CN_ANY);

    case SQL_GROUP_BY:
        GETINFO_RETURN_USHORT(SQL_GB_GROUP_BY_EQUALS_SELECT);

    case SQL_IDENTIFIER_CASE:
        GETINFO_RETURN_USHORT(SQL_IC_LOWER);

    case SQL_NULL_COLLATION:
        GETINFO_RETURN_USHORT(SQL_NC_HIGH);

    case SQL_CURSOR_SENSITIVITY:
        GETINFO_RETURN_UINT(SQL_UNSPECIFIED);

    case SQL_FILE_USAGE:
        GETINFO_RETURN_USHORT(SQL_FILE_NOT_SUPPORTED);

    case SQL_CATALOG_LOCATION:
        GETINFO_RETURN_USHORT(SQL_CL_START);

    case SQL_MAX_COLUMNS_IN_GROUP_BY:
    case SQL_MAX_COLUMNS_IN_INDEX:
    case SQL_MAX_COLUMNS_IN_ORDER_BY:
    case SQL_MAX_COLUMNS_IN_SELECT:
    case SQL_MAX_TABLES_IN_SELECT:
        GETINFO_RETURN_USHORT(0);  /* No specific limit */

    case SQL_MAX_INDEX_SIZE:
    case SQL_MAX_ROW_SIZE:
    case SQL_MAX_STATEMENT_LEN:
    case SQL_MAX_CHAR_LITERAL_LEN:
    case SQL_MAX_BINARY_LITERAL_LEN:
        GETINFO_RETURN_UINT(0);  /* No specific limit */

    case SQL_ACTIVE_ENVIRONMENTS:
        GETINFO_RETURN_USHORT(0);  /* No limit */

    case SQL_BOOKMARK_PERSISTENCE:
        GETINFO_RETURN_UINT(0);  /* No bookmark support */

    case SQL_DYNAMIC_CURSOR_ATTRIBUTES1:
    case SQL_DYNAMIC_CURSOR_ATTRIBUTES2:
    case SQL_KEYSET_CURSOR_ATTRIBUTES1:
    case SQL_KEYSET_CURSOR_ATTRIBUTES2:
        GETINFO_RETURN_UINT(0);

    case SQL_FORWARD_ONLY_CURSOR_ATTRIBUTES1:
        GETINFO_RETURN_UINT(SQL_CA1_NEXT);

    case SQL_FORWARD_ONLY_CURSOR_ATTRIBUTES2:
        GETINFO_RETURN_UINT(SQL_CA2_READ_ONLY_CONCURRENCY);

    case SQL_STATIC_CURSOR_ATTRIBUTES1:
        GETINFO_RETURN_UINT(SQL_CA1_NEXT);

    case SQL_STATIC_CURSOR_ATTRIBUTES2:
        GETINFO_RETURN_UINT(SQL_CA2_READ_ONLY_CONCURRENCY);

    case SQL_SCROLL_OPTIONS:
        GETINFO_RETURN_UINT(SQL_SO_FORWARD_ONLY);

    case SQL_ALTER_TABLE:
        GETINFO_RETURN_UINT(SQL_AT_ADD_COLUMN | SQL_AT_DROP_COLUMN);

    case SQL_CREATE_TABLE:
        GETINFO_RETURN_UINT(SQL_CT_CREATE_TABLE);

    case SQL_DROP_TABLE:
        GETINFO_RETURN_UINT(SQL_DT_DROP_TABLE);

    case SQL_CREATE_VIEW:
        GETINFO_RETURN_UINT(SQL_CV_CREATE_VIEW);

    case SQL_DROP_VIEW:
        GETINFO_RETURN_UINT(SQL_DV_DROP_VIEW);

    case SQL_SUBQUERIES:
        GETINFO_RETURN_UINT(SQL_SQ_CORRELATED_SUBQUERIES | SQL_SQ_COMPARISON |
                            SQL_SQ_EXISTS | SQL_SQ_IN | SQL_SQ_QUANTIFIED);

    case SQL_UNION:
        GETINFO_RETURN_UINT(SQL_U_UNION | SQL_U_UNION_ALL);

    case SQL_OJ_CAPABILITIES:
        GETINFO_RETURN_UINT(SQL_OJ_LEFT | SQL_OJ_RIGHT | SQL_OJ_FULL |
                            SQL_OJ_NESTED | SQL_OJ_NOT_ORDERED |
                            SQL_OJ_INNER | SQL_OJ_ALL_COMPARISON_OPS);

    case SQL_SQL_CONFORMANCE:
        GETINFO_RETURN_UINT(SQL_SC_SQL92_ENTRY);

    case SQL_ODBC_INTERFACE_CONFORMANCE:
        GETINFO_RETURN_UINT(SQL_OIC_CORE);

    case SQL_SQL92_PREDICATES:
        GETINFO_RETURN_UINT(SQL_SP_EXISTS | SQL_SP_ISNOTNULL | SQL_SP_ISNULL |
                            SQL_SP_LIKE | SQL_SP_IN | SQL_SP_BETWEEN |
                            SQL_SP_COMPARISON | SQL_SP_QUANTIFIED_COMPARISON);

    case SQL_SQL92_RELATIONAL_JOIN_OPERATORS:
        GETINFO_RETURN_UINT(SQL_SRJO_CROSS_JOIN | SQL_SRJO_INNER_JOIN |
                            SQL_SRJO_LEFT_OUTER_JOIN | SQL_SRJO_RIGHT_OUTER_JOIN |
                            SQL_SRJO_FULL_OUTER_JOIN | SQL_SRJO_NATURAL_JOIN);

    case SQL_SQL92_VALUE_EXPRESSIONS:
        GETINFO_RETURN_UINT(SQL_SVE_CASE | SQL_SVE_CAST | SQL_SVE_COALESCE |
                            SQL_SVE_NULLIF);

    case SQL_DATETIME_LITERALS:
        GETINFO_RETURN_UINT(SQL_DL_SQL92_DATE | SQL_DL_SQL92_TIME |
                            SQL_DL_SQL92_TIMESTAMP);

    case SQL_AGGREGATE_FUNCTIONS:
        GETINFO_RETURN_UINT(SQL_AF_ALL | SQL_AF_AVG | SQL_AF_COUNT |
                            SQL_AF_DISTINCT | SQL_AF_MAX | SQL_AF_MIN | SQL_AF_SUM);

    case SQL_NUMERIC_FUNCTIONS:
        GETINFO_RETURN_UINT(SQL_FN_NUM_ABS | SQL_FN_NUM_CEILING | SQL_FN_NUM_FLOOR |
                            SQL_FN_NUM_MOD | SQL_FN_NUM_POWER | SQL_FN_NUM_ROUND |
                            SQL_FN_NUM_SIGN | SQL_FN_NUM_SQRT | SQL_FN_NUM_TRUNCATE);

    case SQL_STRING_FUNCTIONS:
        GETINFO_RETURN_UINT(SQL_FN_STR_CONCAT | SQL_FN_STR_LENGTH | SQL_FN_STR_LOCATE |
                            SQL_FN_STR_LTRIM | SQL_FN_STR_RTRIM | SQL_FN_STR_SUBSTRING |
                            SQL_FN_STR_LCASE | SQL_FN_STR_UCASE | SQL_FN_STR_REPLACE);

    case SQL_SYSTEM_FUNCTIONS:
        GETINFO_RETURN_UINT(SQL_FN_SYS_IFNULL | SQL_FN_SYS_USERNAME);

    case SQL_TIMEDATE_FUNCTIONS:
        GETINFO_RETURN_UINT(SQL_FN_TD_CURDATE | SQL_FN_TD_CURTIME | SQL_FN_TD_NOW |
                            SQL_FN_TD_EXTRACT | SQL_FN_TD_YEAR | SQL_FN_TD_MONTH |
                            SQL_FN_TD_DAYOFMONTH | SQL_FN_TD_HOUR | SQL_FN_TD_MINUTE |
                            SQL_FN_TD_SECOND);

    case SQL_CONVERT_FUNCTIONS:
        GETINFO_RETURN_UINT(SQL_FN_CVT_CAST);

    /* Conversion support flags — PostgreSQL supports most conversions via CAST */
    case SQL_CONVERT_BIGINT:
    case SQL_CONVERT_INTEGER:
    case SQL_CONVERT_SMALLINT:
    case SQL_CONVERT_TINYINT:
    case SQL_CONVERT_FLOAT:
    case SQL_CONVERT_REAL:
    case SQL_CONVERT_DOUBLE:
    case SQL_CONVERT_NUMERIC:
    case SQL_CONVERT_DECIMAL:
    case SQL_CONVERT_CHAR:
    case SQL_CONVERT_VARCHAR:
    case SQL_CONVERT_LONGVARCHAR:
    case SQL_CONVERT_DATE:
    case SQL_CONVERT_TIME:
    case SQL_CONVERT_TIMESTAMP:
    case SQL_CONVERT_BINARY:
    case SQL_CONVERT_VARBINARY:
    case SQL_CONVERT_LONGVARBINARY:
    case SQL_CONVERT_BIT:
        GETINFO_RETURN_UINT(SQL_CVT_CHAR | SQL_CVT_VARCHAR | SQL_CVT_LONGVARCHAR |
                            SQL_CVT_INTEGER | SQL_CVT_SMALLINT | SQL_CVT_BIGINT |
                            SQL_CVT_FLOAT | SQL_CVT_REAL | SQL_CVT_DOUBLE |
                            SQL_CVT_NUMERIC | SQL_CVT_DECIMAL);

    case SQL_POS_OPERATIONS:
    case SQL_LOCK_TYPES:
    case SQL_POSITIONED_STATEMENTS:
    case SQL_BATCH_ROW_COUNT:
    case SQL_BATCH_SUPPORT:
    case SQL_PARAM_ARRAY_ROW_COUNTS:
    case SQL_PARAM_ARRAY_SELECTS:
    case SQL_INFO_SCHEMA_VIEWS:
    case SQL_DDL_INDEX:
    case SQL_CREATE_SCHEMA:
    case SQL_DROP_SCHEMA:
    case SQL_CREATE_ASSERTION:
    case SQL_DROP_ASSERTION:
    case SQL_CREATE_CHARACTER_SET:
    case SQL_CREATE_COLLATION:
    case SQL_CREATE_DOMAIN:
    case SQL_CREATE_TRANSLATION:
    case SQL_DROP_CHARACTER_SET:
    case SQL_DROP_COLLATION:
    case SQL_DROP_DOMAIN:
    case SQL_DROP_TRANSLATION:
    case SQL_INDEX_KEYWORDS:
    case SQL_INSERT_STATEMENT:
    case SQL_STANDARD_CLI_CONFORMANCE:
    case SQL_ASYNC_MODE:
        GETINFO_RETURN_UINT(0);

    case SQL_FETCH_DIRECTION:
        GETINFO_RETURN_UINT(SQL_FD_FETCH_NEXT);

    case SQL_MAX_DRIVER_CONNECTIONS:
    case SQL_MAX_CONCURRENT_ACTIVITIES:
        GETINFO_RETURN_USHORT(0);  /* No limit */

    default:
        diagnostics_add_record(&connection->diagnostics,
                               "HY096",  /* Information type out of range */
                               0,
                               "Unsupported information type in SQLGetInfo.");
        return SQL_ERROR;
    }

    #undef GETINFO_RETURN_STRING
    #undef GETINFO_RETURN_UINT
    #undef GETINFO_RETURN_USHORT
}

/**
 * SQLNumParams — Return the number of parameters in a prepared statement.
 *
 * Returns the count of parameter markers ('?') that were found during
 * SQL text translation. Requires the statement to have been prepared
 * (or at least to have had its SQL text translated via SQLPrepare or
 * SQLExecDirect).
 *
 * Parameters:
 *   statement_handle - A valid statement handle.
 *   param_count_ptr  - Output: the number of parameter markers found.
 *
 * Returns:
 *   SQL_SUCCESS        - Parameter count retrieved successfully.
 *   SQL_INVALID_HANDLE - statement_handle is not a valid statement handle.
 *
 * See: https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlnumparams-function
 */
PSQLODBC2_EXPORT SQLRETURN SQL_API
SQLNumParams(SQLHSTMT     statement_handle,
             SQLSMALLINT *param_count_ptr)
{
    OdbcStatement *statement = (OdbcStatement *)statement_handle;

    if (!statement || statement->magic_number != STATEMENT_MAGIC_NUMBER) {
        return SQL_INVALID_HANDLE;
    }

    diagnostics_clear(&statement->diagnostics);

    if (param_count_ptr) {
        *param_count_ptr = (SQLSMALLINT)statement->detected_param_count;
    }

    return SQL_SUCCESS;
}

/**
 * SQLNativeSql — Return the SQL string as modified by the driver.
 *
 * Shows the translated SQL that the driver would send to the data source.
 * This driver translates ODBC '?' parameter markers to PostgreSQL '$N'
 * markers and processes ODBC escape sequences.
 *
 * Parameters:
 *   connection_handle  - A valid connection handle.
 *   sql_text_in        - The input SQL string to translate.
 *   text_length_in     - Length of sql_text_in, or SQL_NTS.
 *   sql_text_out       - Output buffer for the translated SQL.
 *   buffer_length      - Size of sql_text_out buffer.
 *   text_length_out    - Output: actual length of the translated SQL.
 *
 * Returns:
 *   SQL_SUCCESS           - Translation successful.
 *   SQL_SUCCESS_WITH_INFO - Output was truncated.
 *   SQL_ERROR             - Input is NULL or allocation failed.
 *   SQL_INVALID_HANDLE    - connection_handle is not valid.
 *
 * See: https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlnativesql-function
 */
PSQLODBC2_EXPORT SQLRETURN SQL_API
SQLNativeSql(SQLHDBC     connection_handle,
             SQLCHAR    *sql_text_in,
             SQLINTEGER  text_length_in,
             SQLCHAR    *sql_text_out,
             SQLINTEGER  buffer_length,
             SQLINTEGER *text_length_out)
{
    OdbcConnection *connection = (OdbcConnection *)connection_handle;

    if (!connection || connection->magic_number != CONNECTION_MAGIC_NUMBER) {
        return SQL_INVALID_HANDLE;
    }

    diagnostics_clear(&connection->diagnostics);

    if (!sql_text_in) {
        diagnostics_add_record(&connection->diagnostics,
                               "HY009", 0,
                               "Input SQL text is NULL.");
        return SQL_ERROR;
    }

    /* Resolve input length */
    size_t input_length = (text_length_in == SQL_NTS)
        ? strlen((const char *)sql_text_in)
        : (size_t)text_length_in;

    /* Make a null-terminated copy for the translator */
    char *input_copy = malloc(input_length + 1);
    if (!input_copy) {
        diagnostics_add_record(&connection->diagnostics,
                               "HY001", 0,
                               "Memory allocation failed.");
        return SQL_ERROR;
    }
    memcpy(input_copy, sql_text_in, input_length);
    input_copy[input_length] = '\0';

    /* Translate markers and escape sequences */
    int param_count = 0;
    char *translated = query_translate_markers(input_copy, &param_count, NULL, 0);
    free(input_copy);

    if (!translated) {
        diagnostics_add_record(&connection->diagnostics,
                               "HY001", 0,
                               "Memory allocation failed during translation.");
        return SQL_ERROR;
    }

    SQLINTEGER translated_length = (SQLINTEGER)strlen(translated);

    if (text_length_out) {
        *text_length_out = translated_length;
    }

    SQLRETURN result = SQL_SUCCESS;
    if (sql_text_out && buffer_length > 0) {
        SQLINTEGER copy_length = translated_length;
        if (copy_length >= buffer_length) {
            copy_length = buffer_length - 1;
            diagnostics_add_record(&connection->diagnostics,
                                   "01004", 0,
                                   "Output SQL text was truncated.");
            result = SQL_SUCCESS_WITH_INFO;
        }
        memcpy(sql_text_out, translated, (size_t)copy_length);
        sql_text_out[copy_length] = '\0';
    }

    free(translated);
    return result;
}
