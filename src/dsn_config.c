/*-------------------------------------------------------------------------
 *
 * dsn_config.c
 *	  DSN registry reading from odbc.ini
 *
 * Copyright (c) 2026, Dave Cramer
 *
 *
 * IDENTIFICATION
 *	  src/dsn_config.c
 *
 *-------------------------------------------------------------------------
 */
#include "dsn_config.h"
#include <string.h>
#include <stdlib.h>

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#endif

#ifdef HAVE_ODBCINST
#include <odbcinst.h>

/* Buffer size for reading individual INI values. This matches the typical
 * maximum value length in odbc.ini files. */
#define INI_VALUE_BUFFER_SIZE 512

/*
 * Read a single string value from the DSN section of the specified INI file.
 * Returns the number of characters written to the buffer (excluding null
 * terminator), or 0 if the key does not exist or has an empty value.
 */
static int read_ini_string_from(const char *dsn_name, const char *key,
                                char *buffer, int buffer_size,
                                const char *ini_file)
{
    /* SQLGetPrivateProfileString returns the number of characters written.
     * If the DSN section or key doesn't exist, it writes the default value
     * (empty string here) and returns 0.
     *
     * On Windows, SQLGetPrivateProfileString always reads from the registry
     * regardless of the filename argument. For file-based reads (used in
     * testing), we use GetPrivateProfileStringA directly. */
#if defined(_WIN32) || defined(_WIN64)
    if (ini_file && strcmp(ini_file, ODBC_INI_FILE) != 0) {
        /* File-based read — use Win32 API directly */
        return (int)GetPrivateProfileStringA(
            dsn_name, key, "", buffer, (DWORD)buffer_size, ini_file);
    }
#endif
    int chars_written = SQLGetPrivateProfileString(
        dsn_name, key, "", buffer, buffer_size, ini_file);
    return chars_written;
}

/*
 * Internal implementation shared by dsn_config_read and dsn_config_read_file.
 */
static bool dsn_config_read_impl(const char *dsn_name, ConnectionInfo *out_info,
                                 const char *ini_file)
{
    if (!dsn_name || dsn_name[0] == '\0' || !out_info) {
        return false;
    }

    char buffer[INI_VALUE_BUFFER_SIZE];
    bool found_any_value = false;

    /* Read server name — try "Servername" first (psqlodbc convention),
     * then fall back to "Server" (common alias) */
    if (read_ini_string_from(dsn_name, DSN_KEY_SERVERNAME, buffer, sizeof(buffer), ini_file) > 0) {
        strncpy(out_info->server, buffer, sizeof(out_info->server) - 1);
        out_info->server[sizeof(out_info->server) - 1] = '\0';
        found_any_value = true;
    } else if (read_ini_string_from(dsn_name, DSN_KEY_SERVER, buffer, sizeof(buffer), ini_file) > 0) {
        strncpy(out_info->server, buffer, sizeof(out_info->server) - 1);
        out_info->server[sizeof(out_info->server) - 1] = '\0';
        found_any_value = true;
    }

    /* Read port */
    if (read_ini_string_from(dsn_name, DSN_KEY_PORT, buffer, sizeof(buffer), ini_file) > 0) {
        strncpy(out_info->port, buffer, sizeof(out_info->port) - 1);
        out_info->port[sizeof(out_info->port) - 1] = '\0';
        found_any_value = true;
    }

    /* Read database name */
    if (read_ini_string_from(dsn_name, DSN_KEY_DATABASE, buffer, sizeof(buffer), ini_file) > 0) {
        strncpy(out_info->database, buffer, sizeof(out_info->database) - 1);
        out_info->database[sizeof(out_info->database) - 1] = '\0';
        found_any_value = true;
    }

    /* Read username — try "Username" first, then "UID" as alias */
    if (read_ini_string_from(dsn_name, DSN_KEY_USERNAME, buffer, sizeof(buffer), ini_file) > 0) {
        strncpy(out_info->username, buffer, sizeof(out_info->username) - 1);
        out_info->username[sizeof(out_info->username) - 1] = '\0';
        found_any_value = true;
    } else if (read_ini_string_from(dsn_name, DSN_KEY_UID, buffer, sizeof(buffer), ini_file) > 0) {
        strncpy(out_info->username, buffer, sizeof(out_info->username) - 1);
        out_info->username[sizeof(out_info->username) - 1] = '\0';
        found_any_value = true;
    }

    /* Read password — heap-allocated for secure clearing.
     * Only overwrite if the INI value is non-empty. */
    if (read_ini_string_from(dsn_name, DSN_KEY_PASSWORD, buffer, sizeof(buffer), ini_file) > 0) {
        size_t password_length = strlen(buffer);
        char *new_password = malloc(password_length + 1);
        if (new_password) {
            memcpy(new_password, buffer, password_length + 1);
            /* Free any existing password before replacing */
            free(out_info->password);
            out_info->password = new_password;
            found_any_value = true;
        }
        /* Wipe the local buffer since it contained a password */
        memset(buffer, 0, sizeof(buffer));
    }

    /* Read SSL mode */
    if (read_ini_string_from(dsn_name, DSN_KEY_SSLMODE, buffer, sizeof(buffer), ini_file) > 0) {
        strncpy(out_info->sslmode, buffer, sizeof(out_info->sslmode) - 1);
        out_info->sslmode[sizeof(out_info->sslmode) - 1] = '\0';
        found_any_value = true;
    }

    /* Read application name */
    if (read_ini_string_from(dsn_name, DSN_KEY_APP_NAME, buffer, sizeof(buffer), ini_file) > 0) {
        strncpy(out_info->application_name, buffer, sizeof(out_info->application_name) - 1);
        out_info->application_name[sizeof(out_info->application_name) - 1] = '\0';
        found_any_value = true;
    }

    /* Read connection timeout (stored as a decimal integer string) */
    if (read_ini_string_from(dsn_name, DSN_KEY_TIMEOUT, buffer, sizeof(buffer), ini_file) > 0) {
        unsigned long timeout_value = strtoul(buffer, NULL, 10);
        out_info->connect_timeout = (unsigned int)timeout_value;
        found_any_value = true;
    }

    return found_any_value;
}

bool dsn_config_read(const char *dsn_name, ConnectionInfo *out_info)
{
    return dsn_config_read_impl(dsn_name, out_info, ODBC_INI_FILE);
}

bool dsn_config_read_file(const char *dsn_name, ConnectionInfo *out_info,
                          const char *ini_file)
{
    if (!ini_file || ini_file[0] == '\0')
        return dsn_config_read(dsn_name, out_info);
    return dsn_config_read_impl(dsn_name, out_info, ini_file);
}

#else /* !HAVE_ODBCINST */

/* DSN lookup is not available without libodbcinst.
 * The driver still compiles and works — it just cannot resolve DSN names
 * from odbc.ini. Connection strings and explicit parameters still work. */
bool dsn_config_read(const char *dsn_name, ConnectionInfo *out_info)
{
    (void)dsn_name;
    (void)out_info;
    return false;
}

bool dsn_config_read_file(const char *dsn_name, ConnectionInfo *out_info,
                          const char *ini_file)
{
    (void)dsn_name;
    (void)out_info;
    (void)ini_file;
    return false;
}

#endif /* HAVE_ODBCINST */
