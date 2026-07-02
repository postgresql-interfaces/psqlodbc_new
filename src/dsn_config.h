/*-------------------------------------------------------------------------
 *
 * dsn_config.h
 *	  DSN configuration reader declarations
 *
 * Copyright (c) 2026, Dave Cramer
 *
 *
 * IDENTIFICATION
 *	  src/dsn_config.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PSQLODBC2_DSN_CONFIG_H
#define PSQLODBC2_DSN_CONFIG_H

#include "psqlodbc2.h"
#include "connection.h"
#include <stdbool.h>

/* INI key names used in odbc.ini DSN sections.
 * These match the standard psqlodbc key names for compatibility with
 * existing odbc.ini configurations created for the original driver. */
#define DSN_KEY_SERVERNAME   "Servername"
#define DSN_KEY_SERVER       "Server"
#define DSN_KEY_PORT         "Port"
#define DSN_KEY_DATABASE     "Database"
#define DSN_KEY_USERNAME     "Username"
#define DSN_KEY_UID          "UID"
#define DSN_KEY_PASSWORD     "Password"
#define DSN_KEY_SSLMODE      "SSLmode"
#define DSN_KEY_APP_NAME     "ApplicationName"
#define DSN_KEY_TIMEOUT      "Timeout"
#define DSN_KEY_DESCRIPTION  "Description"

/* ODBC INI file identifier passed to SQLGetPrivateProfileString.
 * On Unix (unixODBC), this causes the function to search the standard
 * odbc.ini locations. On Windows, it maps to the registry path. */
#define ODBC_INI_FILE "odbc.ini"

/*
 * Read connection parameters for the named DSN from the ODBC configuration
 * (odbc.ini on Unix, registry on Windows).
 *
 * Parameters read from the DSN section are stored into out_info. Only non-empty
 * values in the INI file are written — existing values in out_info are NOT
 * cleared, allowing the caller to pre-populate defaults or overlay DSN values
 * with connection string values afterward.
 *
 * Returns true if the DSN was found and at least one parameter was read.
 * Returns false if the DSN does not exist or no parameters could be read
 * (including when libodbcinst is not available at build time).
 */
bool dsn_config_read(const char *dsn_name, ConnectionInfo *out_info);

/*
 * Read connection parameters for the named DSN from a specific INI file.
 *
 * Same behavior as dsn_config_read, but reads from the specified file path
 * instead of the system ODBC configuration. On Windows, a full path causes
 * SQLGetPrivateProfileString to read from the file rather than the registry.
 * On Unix (unixODBC), the filename is passed directly.
 *
 * This is primarily useful for testing without modifying the system ODBC
 * configuration.
 */
bool dsn_config_read_file(const char *dsn_name, ConnectionInfo *out_info,
                          const char *ini_file);

#endif /* PSQLODBC2_DSN_CONFIG_H */
