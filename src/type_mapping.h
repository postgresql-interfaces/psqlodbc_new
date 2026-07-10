/*-------------------------------------------------------------------------
 *
 * type_mapping.h
 *	  Type mapping function declarations
 *
 * Copyright (c) 2026, Dave Cramer
 *
 *
 * IDENTIFICATION
 *	  src/type_mapping.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PSQLODBC2_TYPE_MAPPING_H
#define PSQLODBC2_TYPE_MAPPING_H

#include "psqlodbc2.h"

/* ---- PostgreSQL Type OIDs ----
 *
 * These are the fixed OID values assigned to built-in PostgreSQL types.
 * They are stable across PostgreSQL versions and defined in pg_type.h
 * in the PostgreSQL source. We define only the commonly-used subset. */

#define PG_TYPE_BOOL          16
#define PG_TYPE_BYTEA         17
#define PG_TYPE_CHAR          18   /* single-byte internal type "char" */
#define PG_TYPE_NAME          19
#define PG_TYPE_INT8          20
#define PG_TYPE_INT2          21
#define PG_TYPE_INT4          23
#define PG_TYPE_TEXT          25
#define PG_TYPE_OID           26
#define PG_TYPE_FLOAT4       700
#define PG_TYPE_FLOAT8       701
#define PG_TYPE_BPCHAR      1042   /* blank-padded character(n) */
#define PG_TYPE_VARCHAR     1043
#define PG_TYPE_DATE        1082
#define PG_TYPE_TIME        1083
#define PG_TYPE_TIMESTAMP   1114
#define PG_TYPE_TIMESTAMPTZ 1184
#define PG_TYPE_INTERVAL    1186
#define PG_TYPE_NUMERIC     1700
#define PG_TYPE_UUID        2950

/* Column size reported for a boolean column when the BoolsAsChar option is on.
 * Wide enough to hold the textual representation "false". Matches the original
 * psqlodbc driver's PG_WIDTH_OF_BOOLS_AS_CHAR. */
#define PG_WIDTH_OF_BOOLS_AS_CHAR 5

/* ---- Public Interface ---- */

/*
 * Map a PostgreSQL type OID to the corresponding ODBC SQL type constant.
 * Returns SQL_VARCHAR for unrecognized OIDs (safe fallback — all data can
 * be represented as character strings).
 */
SQLSMALLINT type_mapping_get_sql_type(unsigned int postgres_oid);

/*
 * Get the default C type that SQLGetData should use when the application
 * requests SQL_C_DEFAULT. The mapping follows ODBC conventions:
 * SQL_INTEGER → SQL_C_SLONG, SQL_VARCHAR → SQL_C_CHAR, etc.
 */
SQLSMALLINT type_mapping_get_default_c_type(SQLSMALLINT sql_type);

/*
 * Compute the column display size for a given PostgreSQL type and type modifier.
 * For varchar(N), typmod encodes the max length (typmod - 4 = max chars).
 * For numeric(p,s), typmod encodes precision.
 * Returns 0 for variable-length types without a declared limit (e.g., text).
 */
SQLULEN type_mapping_get_column_size(unsigned int postgres_oid, int type_modifier);

/*
 * Compute the number of decimal digits (scale) for a given type and modifier.
 * Meaningful for numeric(p,s) types; returns 0 for integer and string types.
 */
SQLSMALLINT type_mapping_get_decimal_digits(unsigned int postgres_oid, int type_modifier);

/*
 * Get a human-readable type name string for a PostgreSQL OID.
 * Returns "unknown" for unrecognized OIDs.
 */
const char *type_mapping_get_type_name(unsigned int postgres_oid);

/*
 * Return the PostgreSQL cast suffix (e.g. "::int4") for a bound parameter's
 * SQL type, or NULL when no cast is appropriate.
 *
 * When an application binds a parameter with an explicit SQL type via
 * SQLBindParameter (e.g. SQL_SMALLINT), the driver appends this suffix to the
 * corresponding "$N" marker so PostgreSQL interprets the value as that type
 * rather than as an untyped string literal. This makes comparisons behave
 * numerically and produces type-accurate server error messages, matching the
 * original psqlodbc's sqltype_to_pgcast() behavior.
 *
 * SQL_CHAR/SQL_VARCHAR intentionally return NULL: leaving them untyped lets
 * PostgreSQL treat the value as text, which is what applications expect for
 * character parameters.
 */
const char *type_mapping_get_param_cast(SQLSMALLINT sql_type);

#endif /* PSQLODBC2_TYPE_MAPPING_H */
