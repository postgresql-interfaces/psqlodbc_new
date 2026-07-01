/*-------------------------------------------------------------------------
 *
 * dsn_config_stub.c
 *	  Stub for dsn_config_read used by connection string unit tests
 *
 * Copyright (c) 2026, Dave Cramer
 *
 *
 * IDENTIFICATION
 *	  tests/dsn_config_stub.c
 *
 *-------------------------------------------------------------------------
 */
#include "dsn_config.h"

/* Always returns false — DSN lookup is not exercised in connection string
 * parsing unit tests. This avoids a dependency on libodbcinst and prevents
 * segfaults in CI environments without a configured odbc.ini. */
bool dsn_config_read(const char *dsn_name, ConnectionInfo *out_info)
{
    (void)dsn_name;
    (void)out_info;
    return false;
}
