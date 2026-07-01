/*-------------------------------------------------------------------------
 *
 * catalog.h
 *	  ODBC catalog function declarations
 *
 * Copyright (c) 2026, Dave Cramer
 *
 *
 * IDENTIFICATION
 *	  src/catalog.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PSQLODBC2_CATALOG_H
#define PSQLODBC2_CATALOG_H

#include "psqlodbc2.h"
#include "statement.h"

/*
 * List tables, views, and other relations matching the given patterns.
 * Returns a result set with columns: TABLE_CAT, TABLE_SCHEM, TABLE_NAME,
 * TABLE_TYPE, REMARKS.
 *
 * Pattern arguments support LIKE wildcards. NULL means "no filter" (match all).
 */
SQLRETURN catalog_tables(OdbcStatement *statement,
                         const SQLCHAR *catalog_name, SQLSMALLINT catalog_name_length,
                         const SQLCHAR *schema_pattern, SQLSMALLINT schema_length,
                         const SQLCHAR *table_pattern, SQLSMALLINT table_length,
                         const SQLCHAR *table_type, SQLSMALLINT type_length);

/*
 * Describe columns of tables matching the given patterns.
 * Returns a result set with 18 ODBC-standard columns including COLUMN_NAME,
 * DATA_TYPE, TYPE_NAME, COLUMN_SIZE, NULLABLE, COLUMN_DEF, ORDINAL_POSITION.
 *
 * Pattern arguments support LIKE wildcards. NULL means "no filter" (match all).
 */
SQLRETURN catalog_columns(OdbcStatement *statement,
                          const SQLCHAR *catalog_name, SQLSMALLINT catalog_name_length,
                          const SQLCHAR *schema_pattern, SQLSMALLINT schema_length,
                          const SQLCHAR *table_pattern, SQLSMALLINT table_length,
                          const SQLCHAR *column_pattern, SQLSMALLINT column_length);

/*
 * Get the primary key columns for a specific table.
 * Returns a result set with columns: TABLE_CAT, TABLE_SCHEM, TABLE_NAME,
 * COLUMN_NAME, KEY_SEQ, PK_NAME.
 *
 * Arguments are exact-match (no LIKE wildcards). table_name is required.
 */
SQLRETURN catalog_primary_keys(OdbcStatement *statement,
                               const SQLCHAR *catalog_name, SQLSMALLINT catalog_name_length,
                               const SQLCHAR *schema_name, SQLSMALLINT schema_length,
                               const SQLCHAR *table_name, SQLSMALLINT table_length);

/*
 * Get foreign key relationships. Can be queried in three modes:
 *   - pk_table specified: find all FKs that reference the given table (exported keys)
 *   - fk_table specified: find all FKs on the given table (imported keys)
 *   - both specified: find FKs from fk_table referencing pk_table (cross-reference)
 *
 * Returns a result set with 14 ODBC-standard columns including PKTABLE_NAME,
 * PKCOLUMN_NAME, FKTABLE_NAME, FKCOLUMN_NAME, KEY_SEQ, UPDATE_RULE, DELETE_RULE.
 *
 * Arguments are exact-match (no LIKE wildcards). At least one table must be specified.
 */
SQLRETURN catalog_foreign_keys(OdbcStatement *statement,
                               const SQLCHAR *pk_catalog, SQLSMALLINT pk_catalog_length,
                               const SQLCHAR *pk_schema, SQLSMALLINT pk_schema_length,
                               const SQLCHAR *pk_table, SQLSMALLINT pk_table_length,
                               const SQLCHAR *fk_catalog, SQLSMALLINT fk_catalog_length,
                               const SQLCHAR *fk_schema, SQLSMALLINT fk_schema_length,
                               const SQLCHAR *fk_table, SQLSMALLINT fk_table_length);

#endif /* PSQLODBC2_CATALOG_H */
