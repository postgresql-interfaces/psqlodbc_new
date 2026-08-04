/*-------------------------------------------------------------------------
 *
 * test_catalog.c
 *	  Integration tests for catalog functions
 *
 * Copyright (c) 2026, Dave Cramer
 *
 *
 * IDENTIFICATION
 *	  tests/test_catalog.c
 *
 *-------------------------------------------------------------------------
 */
#include <assert.h>
#include "test_platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include <sql.h>
#include <sqlext.h>

#ifndef DRIVER_LIBRARY_PATH
#error "DRIVER_LIBRARY_PATH must be defined at compile time"
#endif

#define MESON_TEST_SKIP 77

/* Function pointer types */
typedef SQLRETURN (SQL_API *SQLAllocHandle_func)(SQLSMALLINT, SQLHANDLE, SQLHANDLE *);
typedef SQLRETURN (SQL_API *SQLFreeHandle_func)(SQLSMALLINT, SQLHANDLE);
typedef SQLRETURN (SQL_API *SQLFreeStmt_func)(SQLHSTMT, SQLUSMALLINT);
typedef SQLRETURN (SQL_API *SQLDriverConnect_func)(SQLHDBC, SQLHWND, SQLCHAR *, SQLSMALLINT,
                                                   SQLCHAR *, SQLSMALLINT, SQLSMALLINT *, SQLUSMALLINT);
typedef SQLRETURN (SQL_API *SQLDisconnect_func)(SQLHDBC);
typedef SQLRETURN (SQL_API *SQLExecDirect_func)(SQLHSTMT, SQLCHAR *, SQLINTEGER);
typedef SQLRETURN (SQL_API *SQLFetch_func)(SQLHSTMT);
typedef SQLRETURN (SQL_API *SQLGetData_func)(SQLHSTMT, SQLUSMALLINT, SQLSMALLINT,
                                             SQLPOINTER, SQLLEN, SQLLEN *);
typedef SQLRETURN (SQL_API *SQLNumResultCols_func)(SQLHSTMT, SQLSMALLINT *);
typedef SQLRETURN (SQL_API *SQLTables_func)(SQLHSTMT, SQLCHAR *, SQLSMALLINT, SQLCHAR *, SQLSMALLINT,
                                            SQLCHAR *, SQLSMALLINT, SQLCHAR *, SQLSMALLINT);
typedef SQLRETURN (SQL_API *SQLColumns_func)(SQLHSTMT, SQLCHAR *, SQLSMALLINT, SQLCHAR *, SQLSMALLINT,
                                             SQLCHAR *, SQLSMALLINT, SQLCHAR *, SQLSMALLINT);
typedef SQLRETURN (SQL_API *SQLPrimaryKeys_func)(SQLHSTMT, SQLCHAR *, SQLSMALLINT,
                                                  SQLCHAR *, SQLSMALLINT, SQLCHAR *, SQLSMALLINT);
typedef SQLRETURN (SQL_API *SQLForeignKeys_func)(SQLHSTMT, SQLCHAR *, SQLSMALLINT, SQLCHAR *, SQLSMALLINT,
                                                  SQLCHAR *, SQLSMALLINT, SQLCHAR *, SQLSMALLINT,
                                                  SQLCHAR *, SQLSMALLINT, SQLCHAR *, SQLSMALLINT);

static void *driver_handle;
static SQLAllocHandle_func fn_alloc_handle;
static SQLFreeHandle_func fn_free_handle;
static SQLFreeStmt_func fn_free_stmt;
static SQLDriverConnect_func fn_driver_connect;
static SQLDisconnect_func fn_disconnect;
static SQLExecDirect_func fn_exec_direct;
static SQLFetch_func fn_fetch;
static SQLGetData_func fn_get_data;
static SQLNumResultCols_func fn_num_result_cols;
static SQLTables_func fn_tables;
static SQLColumns_func fn_columns;
static SQLPrimaryKeys_func fn_primary_keys;
static SQLForeignKeys_func fn_foreign_keys;

static SQLHANDLE environment_handle;
static SQLHANDLE connection_handle;

static void load_driver(void)
{
    driver_handle = dlopen(DRIVER_LIBRARY_PATH, RTLD_NOW);
    assert(driver_handle != NULL);

    fn_alloc_handle = (SQLAllocHandle_func)dlsym(driver_handle, "SQLAllocHandle");
    fn_free_handle = (SQLFreeHandle_func)dlsym(driver_handle, "SQLFreeHandle");
    fn_free_stmt = (SQLFreeStmt_func)dlsym(driver_handle, "SQLFreeStmt");
    fn_driver_connect = (SQLDriverConnect_func)dlsym(driver_handle, "SQLDriverConnect");
    fn_disconnect = (SQLDisconnect_func)dlsym(driver_handle, "SQLDisconnect");
    fn_exec_direct = (SQLExecDirect_func)dlsym(driver_handle, "SQLExecDirect");
    fn_fetch = (SQLFetch_func)dlsym(driver_handle, "SQLFetch");
    fn_get_data = (SQLGetData_func)dlsym(driver_handle, "SQLGetData");
    fn_num_result_cols = (SQLNumResultCols_func)dlsym(driver_handle, "SQLNumResultCols");
    fn_tables = (SQLTables_func)dlsym(driver_handle, "SQLTables");
    fn_columns = (SQLColumns_func)dlsym(driver_handle, "SQLColumns");
    fn_primary_keys = (SQLPrimaryKeys_func)dlsym(driver_handle, "SQLPrimaryKeys");
    fn_foreign_keys = (SQLForeignKeys_func)dlsym(driver_handle, "SQLForeignKeys");

    assert(fn_alloc_handle && fn_free_handle && fn_free_stmt);
    assert(fn_driver_connect && fn_disconnect && fn_exec_direct);
    assert(fn_fetch && fn_get_data && fn_num_result_cols);
    assert(fn_tables && fn_columns && fn_primary_keys && fn_foreign_keys);
}

static int connect_to_database(const char *connection_string)
{
    SQLRETURN result;
    result = fn_alloc_handle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &environment_handle);
    assert(result == SQL_SUCCESS);
    result = fn_alloc_handle(SQL_HANDLE_DBC, environment_handle, &connection_handle);
    assert(result == SQL_SUCCESS);
    result = fn_driver_connect(connection_handle, NULL,
                               (SQLCHAR *)connection_string, SQL_NTS,
                               NULL, 0, NULL, SQL_DRIVER_NOPROMPT);
    return (result == SQL_SUCCESS) ? 0 : -1;
}

static void setup_test_tables(void)
{
    SQLHANDLE statement;
    fn_alloc_handle(SQL_HANDLE_STMT, connection_handle, &statement);

    fn_exec_direct(statement,
        (SQLCHAR *)"CREATE TABLE IF NOT EXISTS test_cat_parent("
                   "id int PRIMARY KEY, name varchar(100) NOT NULL)", SQL_NTS);
    fn_free_stmt(statement, SQL_CLOSE);

    fn_exec_direct(statement,
        (SQLCHAR *)"CREATE TABLE IF NOT EXISTS test_cat_child("
                   "id int PRIMARY KEY, "
                   "parent_id int REFERENCES test_cat_parent(id) ON DELETE CASCADE)", SQL_NTS);
    fn_free_stmt(statement, SQL_DROP);
}

static void cleanup_test_tables(void)
{
    SQLHANDLE statement;
    fn_alloc_handle(SQL_HANDLE_STMT, connection_handle, &statement);
    fn_exec_direct(statement, (SQLCHAR *)"DROP TABLE IF EXISTS test_cat_child", SQL_NTS);
    fn_free_stmt(statement, SQL_CLOSE);
    fn_exec_direct(statement, (SQLCHAR *)"DROP TABLE IF EXISTS test_cat_parent", SQL_NTS);
    fn_free_stmt(statement, SQL_DROP);
}

static void test_tables_returns_results(void)
{
    SQLHANDLE statement;
    SQLRETURN result;
    SQLSMALLINT column_count;

    fn_alloc_handle(SQL_HANDLE_STMT, connection_handle, &statement);

    result = fn_tables(statement, NULL, 0, (SQLCHAR *)"public", SQL_NTS,
                       (SQLCHAR *)"test_cat_%", SQL_NTS, NULL, 0);
    assert(result == SQL_SUCCESS);

    result = fn_num_result_cols(statement, &column_count);
    assert(result == SQL_SUCCESS);
    assert(column_count == 5);

    /* Should have at least 2 rows (test_cat_parent and test_cat_child) */
    int row_count = 0;
    while (fn_fetch(statement) == SQL_SUCCESS) {
        row_count++;
    }
    assert(row_count >= 2);

    fn_free_stmt(statement, SQL_DROP);
    printf("  PASS: test_tables_returns_results\n");
}

static void test_tables_with_type_filter(void)
{
    SQLHANDLE statement;
    SQLRETURN result;
    SQLCHAR table_type[64];
    SQLLEN indicator;

    fn_alloc_handle(SQL_HANDLE_STMT, connection_handle, &statement);

    result = fn_tables(statement, NULL, 0, (SQLCHAR *)"public", SQL_NTS,
                       (SQLCHAR *)"test_cat_%", SQL_NTS,
                       (SQLCHAR *)"TABLE", SQL_NTS);
    assert(result == SQL_SUCCESS);

    /* All results should be TABLE type */
    while (fn_fetch(statement) == SQL_SUCCESS) {
        result = fn_get_data(statement, 4, SQL_C_CHAR, table_type, sizeof(table_type), &indicator);
        assert(result == SQL_SUCCESS);
        assert(strcmp((char *)table_type, "TABLE") == 0);
    }

    fn_free_stmt(statement, SQL_DROP);
    printf("  PASS: test_tables_with_type_filter\n");
}

static void test_columns_returns_metadata(void)
{
    SQLHANDLE statement;
    SQLRETURN result;
    SQLSMALLINT column_count;
    SQLCHAR column_name[128];
    SQLLEN indicator;

    fn_alloc_handle(SQL_HANDLE_STMT, connection_handle, &statement);

    result = fn_columns(statement, NULL, 0, (SQLCHAR *)"public", SQL_NTS,
                        (SQLCHAR *)"test_cat_parent", SQL_NTS, NULL, 0);
    assert(result == SQL_SUCCESS);

    result = fn_num_result_cols(statement, &column_count);
    assert(result == SQL_SUCCESS);
    /* 18 ODBC-standard SQLColumns columns plus 8 driver-specific trailing
     * columns (DISPLAY_SIZE, FIELD_TYPE, AUTO_INCREMENT, PHYSICAL NUMBER,
     * TABLE OID, BASE TYPEID, TYPMOD, TABLE INFO) that mirror the original
     * psqlodbc's synthesized result set. See catalog_columns in catalog.c. */
    assert(column_count == 26);

    /* Should have 2 columns: id and name */
    int row_count = 0;
    while (fn_fetch(statement) == SQL_SUCCESS) {
        fn_get_data(statement, 4, SQL_C_CHAR, column_name, sizeof(column_name), &indicator);
        row_count++;
    }
    assert(row_count == 2);

    fn_free_stmt(statement, SQL_DROP);
    printf("  PASS: test_columns_returns_metadata\n");
}

static void test_columns_with_column_pattern(void)
{
    SQLHANDLE statement;
    SQLRETURN result;
    SQLCHAR column_name[128];
    SQLLEN indicator;

    fn_alloc_handle(SQL_HANDLE_STMT, connection_handle, &statement);

    result = fn_columns(statement, NULL, 0, (SQLCHAR *)"public", SQL_NTS,
                        (SQLCHAR *)"test_cat_parent", SQL_NTS,
                        (SQLCHAR *)"id", SQL_NTS);
    assert(result == SQL_SUCCESS);

    /* Should only return the "id" column */
    int row_count = 0;
    while (fn_fetch(statement) == SQL_SUCCESS) {
        fn_get_data(statement, 4, SQL_C_CHAR, column_name, sizeof(column_name), &indicator);
        assert(strcmp((char *)column_name, "id") == 0);
        row_count++;
    }
    assert(row_count == 1);

    fn_free_stmt(statement, SQL_DROP);
    printf("  PASS: test_columns_with_column_pattern\n");
}

static void test_primary_keys(void)
{
    SQLHANDLE statement;
    SQLRETURN result;
    SQLCHAR column_name[128];
    SQLSMALLINT key_seq;
    SQLLEN indicator;

    fn_alloc_handle(SQL_HANDLE_STMT, connection_handle, &statement);

    result = fn_primary_keys(statement, NULL, 0, (SQLCHAR *)"public", SQL_NTS,
                             (SQLCHAR *)"test_cat_parent", SQL_NTS);
    assert(result == SQL_SUCCESS);

    /* Should return 1 PK column: "id" with KEY_SEQ=1 */
    result = fn_fetch(statement);
    assert(result == SQL_SUCCESS);

    fn_get_data(statement, 4, SQL_C_CHAR, column_name, sizeof(column_name), &indicator);
    assert(strcmp((char *)column_name, "id") == 0);

    fn_get_data(statement, 5, SQL_C_SSHORT, &key_seq, sizeof(key_seq), &indicator);
    assert(key_seq == 1);

    /* No more rows */
    result = fn_fetch(statement);
    assert(result == SQL_NO_DATA);

    fn_free_stmt(statement, SQL_DROP);
    printf("  PASS: test_primary_keys\n");
}

static void test_primary_keys_column_count(void)
{
    SQLHANDLE statement;
    SQLRETURN result;
    SQLSMALLINT column_count;

    fn_alloc_handle(SQL_HANDLE_STMT, connection_handle, &statement);

    result = fn_primary_keys(statement, NULL, 0, (SQLCHAR *)"public", SQL_NTS,
                             (SQLCHAR *)"test_cat_parent", SQL_NTS);
    assert(result == SQL_SUCCESS);

    result = fn_num_result_cols(statement, &column_count);
    assert(result == SQL_SUCCESS);
    assert(column_count == 6);

    fn_free_stmt(statement, SQL_DROP);
    printf("  PASS: test_primary_keys_column_count\n");
}

static void test_foreign_keys_imported(void)
{
    SQLHANDLE statement;
    SQLRETURN result;
    SQLCHAR pk_table[128], fk_column[128];
    SQLLEN indicator;

    fn_alloc_handle(SQL_HANDLE_STMT, connection_handle, &statement);

    /* Get imported keys for test_cat_child */
    result = fn_foreign_keys(statement,
                             NULL, 0, NULL, 0, NULL, 0,
                             NULL, 0, (SQLCHAR *)"public", SQL_NTS,
                             (SQLCHAR *)"test_cat_child", SQL_NTS);
    assert(result == SQL_SUCCESS);

    /* Should return 1 FK: parent_id referencing test_cat_parent.id */
    result = fn_fetch(statement);
    assert(result == SQL_SUCCESS);

    fn_get_data(statement, 3, SQL_C_CHAR, pk_table, sizeof(pk_table), &indicator);
    assert(strcmp((char *)pk_table, "test_cat_parent") == 0);

    fn_get_data(statement, 8, SQL_C_CHAR, fk_column, sizeof(fk_column), &indicator);
    assert(strcmp((char *)fk_column, "parent_id") == 0);

    fn_free_stmt(statement, SQL_DROP);
    printf("  PASS: test_foreign_keys_imported\n");
}

static void test_foreign_keys_column_count(void)
{
    SQLHANDLE statement;
    SQLRETURN result;
    SQLSMALLINT column_count;

    fn_alloc_handle(SQL_HANDLE_STMT, connection_handle, &statement);

    result = fn_foreign_keys(statement,
                             NULL, 0, (SQLCHAR *)"public", SQL_NTS,
                             (SQLCHAR *)"test_cat_parent", SQL_NTS,
                             NULL, 0, NULL, 0, NULL, 0);
    assert(result == SQL_SUCCESS);

    result = fn_num_result_cols(statement, &column_count);
    assert(result == SQL_SUCCESS);
    assert(column_count == 14);

    fn_free_stmt(statement, SQL_DROP);
    printf("  PASS: test_foreign_keys_column_count\n");
}

int main(void)
{
    const char *connection_string = getenv("PSQLODBC2_TEST_CONNSTR");
    if (!connection_string || connection_string[0] == '\0') {
        printf("SKIP: PSQLODBC2_TEST_CONNSTR not set (no test database configured)\n");
        return MESON_TEST_SKIP;
    }

    printf("test_catalog:\n");

    load_driver();

    if (connect_to_database(connection_string) != 0) {
        fprintf(stderr, "SKIP: Could not connect to test database\n");
        dlclose(driver_handle);
        return MESON_TEST_SKIP;
    }

    setup_test_tables();

    test_tables_returns_results();
    test_tables_with_type_filter();
    test_columns_returns_metadata();
    test_columns_with_column_pattern();
    test_primary_keys();
    test_primary_keys_column_count();
    test_foreign_keys_imported();
    test_foreign_keys_column_count();

    cleanup_test_tables();

    fn_disconnect(connection_handle);
    fn_free_handle(SQL_HANDLE_DBC, connection_handle);
    fn_free_handle(SQL_HANDLE_ENV, environment_handle);
    dlclose(driver_handle);

    printf("All catalog tests passed.\n");
    return 0;
}
