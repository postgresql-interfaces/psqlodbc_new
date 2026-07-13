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

/*
 * Return a result set describing the data types supported by the driver, per
 * the ODBC SQLGetTypeInfo contract. Each row describes one SQL type with its
 * name, SQL data type, precision, literal prefix/suffix, create parameters,
 * nullability, case sensitivity, searchability, and related attributes.
 *
 * sql_type is SQL_ALL_TYPES to return every supported type, or a specific SQL
 * type constant (e.g. SQL_VARCHAR) to return only that type's row(s).
 */
SQLRETURN catalog_get_type_info(OdbcStatement *statement, SQLSMALLINT sql_type);

/*
 * SQLColumnPrivileges: privileges granted on the columns of a single table.
 * Columns: table_cat, table_schem, table_name, column_name, grantor, grantee,
 * privilege, is_grantable. Table name is required.
 */
SQLRETURN catalog_column_privileges(OdbcStatement *statement,
                                    const SQLCHAR *catalog_name, SQLSMALLINT catalog_length,
                                    const SQLCHAR *schema_name, SQLSMALLINT schema_length,
                                    const SQLCHAR *table_name, SQLSMALLINT table_length,
                                    const SQLCHAR *column_name, SQLSMALLINT column_length);

/*
 * SQLTablePrivileges: privileges granted on tables matching the patterns.
 * Columns: TABLE_CAT, TABLE_SCHEM, TABLE_NAME, GRANTOR, GRANTEE, PRIVILEGE,
 * IS_GRANTABLE.
 */
SQLRETURN catalog_table_privileges(OdbcStatement *statement,
                                   const SQLCHAR *catalog_name, SQLSMALLINT catalog_length,
                                   const SQLCHAR *schema_pattern, SQLSMALLINT schema_length,
                                   const SQLCHAR *table_pattern, SQLSMALLINT table_length);

/*
 * SQLStatistics: index columns for a table. Columns: TABLE_CAT, TABLE_SCHEM,
 * TABLE_NAME, NON_UNIQUE, INDEX_QUALIFIER, INDEX_NAME, TYPE, ORDINAL_POSITION,
 * COLUMN_NAME, ASC_OR_DESC, CARDINALITY, PAGES, FILTER_CONDITION.
 */
SQLRETURN catalog_statistics(OdbcStatement *statement,
                             const SQLCHAR *catalog_name, SQLSMALLINT catalog_length,
                             const SQLCHAR *schema_name, SQLSMALLINT schema_length,
                             const SQLCHAR *table_name, SQLSMALLINT table_length,
                             SQLUSMALLINT unique, SQLUSMALLINT reserved);

/*
 * SQLProcedures: functions/procedures matching the patterns. Columns:
 * procedure_cat, procedure_schem, procedure_name, num_input_params,
 * num_output_params, num_result_sets, remarks, procedure_type.
 */
SQLRETURN catalog_procedures(OdbcStatement *statement,
                             const SQLCHAR *catalog_name, SQLSMALLINT catalog_length,
                             const SQLCHAR *schema_pattern, SQLSMALLINT schema_length,
                             const SQLCHAR *proc_pattern, SQLSMALLINT proc_length);

/*
 * SQLProcedureColumns: input/output/result columns of matching procedures.
 */
SQLRETURN catalog_procedure_columns(OdbcStatement *statement,
                                    const SQLCHAR *catalog_name, SQLSMALLINT catalog_length,
                                    const SQLCHAR *schema_pattern, SQLSMALLINT schema_length,
                                    const SQLCHAR *proc_pattern, SQLSMALLINT proc_length,
                                    const SQLCHAR *column_pattern, SQLSMALLINT column_length);

/*
 * SQLSpecialColumns: best row identifier (SQL_BEST_ROWID) or row-version
 * (SQL_ROWVER) columns of a table. Columns: SCOPE, COLUMN_NAME, DATA_TYPE,
 * TYPE_NAME, COLUMN_SIZE, BUFFER_LENGTH, DECIMAL_DIGITS, PSEUDO_COLUMN.
 */
SQLRETURN catalog_special_columns(OdbcStatement *statement,
                                  SQLUSMALLINT identifier_type,
                                  const SQLCHAR *catalog_name, SQLSMALLINT catalog_length,
                                  const SQLCHAR *schema_name, SQLSMALLINT schema_length,
                                  const SQLCHAR *table_name, SQLSMALLINT table_length,
                                  SQLUSMALLINT scope, SQLUSMALLINT nullable);

#endif /* PSQLODBC2_CATALOG_H */
