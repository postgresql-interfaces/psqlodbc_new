/*-------------------------------------------------------------------------
 *
 * connection_string.h
 *	  Connection string parser declarations
 *
 * Copyright (c) 2026, Dave Cramer
 *
 *
 * IDENTIFICATION
 *	  src/connection_string.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PSQLODBC2_CONNECTION_STRING_H
#define PSQLODBC2_CONNECTION_STRING_H

#include "psqlodbc2.h"
#include "connection.h"

#include <stdbool.h>
#include <stddef.h>

/*
 * Parse an ODBC connection string into a ConnectionInfo struct.
 *
 * The connection string format is semicolon-separated key=value pairs.
 * Values may be enclosed in braces to include literal semicolons:
 *   PWD={my;password}
 *
 * Recognized keys (case-insensitive):
 *   Server, Servername           -> info->server
 *   Port                         -> info->port
 *   Database, DB                 -> info->database
 *   UID, Username, User          -> info->username
 *   PWD, Password                -> info->password
 *   SSLmode                      -> info->sslmode
 *   ApplicationName, Application_Name -> info->application_name
 *   Timeout, Connect_Timeout     -> info->connect_timeout
 *   DSN                          -> resolves from odbc.ini via dsn_config_read()
 *   Driver                       -> ignored (used by driver manager, not the driver)
 *
 * string_length: length of odbc_connection_string, or SQL_NTS if null-terminated.
 *
 * Returns true on success, false if the input is invalid (NULL with non-zero length).
 */
bool connection_string_parse(const char *odbc_connection_string,
                             SQLSMALLINT string_length,
                             ConnectionInfo *out_info);

/* Maximum number of key-value pairs we can produce for PQconnectdbParams.
 * Currently 8 fields + 1 for the NULL terminator. */
#define LIBPQ_MAX_PARAMS 9

/*
 * Build parallel keyword/value arrays suitable for PQconnectdbParams().
 *
 * This avoids the quoting/escaping issues of building a connection string
 * manually — PQconnectdbParams handles values with spaces, quotes, and
 * special characters correctly.
 *
 * out_keywords and out_values must each have room for at least LIBPQ_MAX_PARAMS+1
 * pointers. The arrays are NULL-terminated as required by PQconnectdbParams.
 * Only non-empty fields are included.
 *
 * The pointers in the output arrays reference memory owned by the ConnectionInfo
 * struct (plus one static buffer for timeout). The caller must use the arrays
 * before the ConnectionInfo is modified or freed.
 */
void connection_info_build_libpq_params(const ConnectionInfo *info,
                                        const char **out_keywords,
                                        const char **out_values,
                                        int *out_param_count);

#endif /* PSQLODBC2_CONNECTION_STRING_H */
