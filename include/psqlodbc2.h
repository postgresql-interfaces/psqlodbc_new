/*-------------------------------------------------------------------------
 *
 * psqlodbc2.h
 *	  Master driver header
 *
 * Copyright (c) 2026, Dave Cramer
 *
 *
 * IDENTIFICATION
 *	  include/psqlodbc2.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PSQLODBC2_H
#define PSQLODBC2_H

/* ---- Version Information ---- */

#define PSQLODBC2_VERSION       "0.1.0"
#define PSQLODBC2_VERSION_MAJOR 0
#define PSQLODBC2_VERSION_MINOR 1
#define PSQLODBC2_VERSION_PATCH 0

/* ---- Platform Abstraction ---- */

#include "platform/platform_defs.h"

/* ---- ODBC Standard Headers ----
 *
 * These provide the SQL* type definitions (SQLHANDLE, SQLRETURN, etc.)
 * and constant definitions (SQL_SUCCESS, SQL_HANDLE_ENV, etc.) that
 * the driver implementation depends on.
 */

#include <sql.h>
#include <sqlext.h>
#include <sqltypes.h>

#endif /* PSQLODBC2_H */
