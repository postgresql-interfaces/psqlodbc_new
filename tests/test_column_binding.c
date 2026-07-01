/*-------------------------------------------------------------------------
 *
 * test_column_binding.c
 *	  Integration tests for column binding
 *
 * Copyright (c) 2026, Dave Cramer
 *
 *
 * IDENTIFICATION
 *	  tests/test_column_binding.c
 *
 *-------------------------------------------------------------------------
 */
#include <assert.h>
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
typedef SQLRETURN (SQL_API *SQLBindCol_func)(SQLHSTMT, SQLUSMALLINT, SQLSMALLINT,
                                             SQLPOINTER, SQLLEN, SQLLEN *);
typedef SQLRETURN (SQL_API *SQLGetDiagRec_func)(SQLSMALLINT, SQLHANDLE, SQLSMALLINT,
                                                SQLCHAR *, SQLINTEGER *, SQLCHAR *,
                                                SQLSMALLINT, SQLSMALLINT *);

static void *driver_handle;
static SQLAllocHandle_func fn_alloc_handle;
static SQLFreeHandle_func fn_free_handle;
static SQLFreeStmt_func fn_free_stmt;
static SQLDriverConnect_func fn_driver_connect;
static SQLDisconnect_func fn_disconnect;
static SQLExecDirect_func fn_exec_direct;
static SQLFetch_func fn_fetch;
static SQLGetData_func fn_get_data;
static SQLBindCol_func fn_bind_col;
static SQLGetDiagRec_func fn_get_diag_rec;

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
    fn_bind_col = (SQLBindCol_func)dlsym(driver_handle, "SQLBindCol");
    fn_get_diag_rec = (SQLGetDiagRec_func)dlsym(driver_handle, "SQLGetDiagRec");

    assert(fn_alloc_handle && fn_free_handle && fn_free_stmt);
    assert(fn_driver_connect && fn_disconnect && fn_exec_direct);
    assert(fn_fetch && fn_get_data && fn_bind_col && fn_get_diag_rec);
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
    if (result != SQL_SUCCESS) {
        SQLCHAR message[256];
        SQLSMALLINT length;
        fn_get_diag_rec(SQL_HANDLE_DBC, connection_handle, 1,
                        NULL, NULL, message, sizeof(message), &length);
        fprintf(stderr, "Connection failed: %s\n", message);
        return -1;
    }
    return 0;
}

static void disconnect_and_cleanup(void)
{
    fn_disconnect(connection_handle);
    fn_free_handle(SQL_HANDLE_DBC, connection_handle);
    fn_free_handle(SQL_HANDLE_ENV, environment_handle);
}

/* Test 1: Bind columns of different types, fetch, and verify values */
static void test_bind_and_fetch_typed_columns(void)
{
    SQLHANDLE statement;
    SQLRETURN result;
    SQLINTEGER int_value = 0;
    SQLCHAR str_buffer[64];
    SQLDOUBLE double_value = 0.0;
    SQLLEN int_indicator = 0;
    SQLLEN str_indicator = 0;
    SQLLEN double_indicator = 0;

    result = fn_alloc_handle(SQL_HANDLE_STMT, connection_handle, &statement);
    assert(result == SQL_SUCCESS);

    /* Bind columns before executing the query */
    result = fn_bind_col(statement, 1, SQL_C_SLONG, &int_value, sizeof(int_value), &int_indicator);
    assert(result == SQL_SUCCESS);

    result = fn_bind_col(statement, 2, SQL_C_CHAR, str_buffer, sizeof(str_buffer), &str_indicator);
    assert(result == SQL_SUCCESS);

    result = fn_bind_col(statement, 3, SQL_C_DOUBLE, &double_value, sizeof(double_value), &double_indicator);
    assert(result == SQL_SUCCESS);

    /* Execute a query that returns integer, string, and float columns */
    result = fn_exec_direct(statement,
                            (SQLCHAR *)"SELECT 42 AS int_val, 'hello' AS str_val, 3.14::float8 AS float_val",
                            SQL_NTS);
    assert(result == SQL_SUCCESS);

    /* Fetch — should populate bound buffers */
    result = fn_fetch(statement);
    assert(result == SQL_SUCCESS);

    /* Verify the bound buffers were populated correctly */
    assert(int_value == 42);
    assert(int_indicator == (SQLLEN)sizeof(SQLINTEGER));

    assert(strcmp((char *)str_buffer, "hello") == 0);
    assert(str_indicator == 5);

    assert(fabs(double_value - 3.14) < 0.001);
    assert(double_indicator == (SQLLEN)sizeof(SQLDOUBLE));

    fn_free_stmt(statement, SQL_DROP);
    printf("  PASS: test_bind_and_fetch_typed_columns\n");
}

/* Test 2: Fetch NULL value — indicator should be SQL_NULL_DATA */
static void test_bind_fetch_null_value(void)
{
    SQLHANDLE statement;
    SQLRETURN result;
    SQLINTEGER int_value = 999;  /* Sentinel to verify buffer is NOT overwritten */
    SQLLEN indicator = 0;

    result = fn_alloc_handle(SQL_HANDLE_STMT, connection_handle, &statement);
    assert(result == SQL_SUCCESS);

    result = fn_bind_col(statement, 1, SQL_C_SLONG, &int_value, sizeof(int_value), &indicator);
    assert(result == SQL_SUCCESS);

    result = fn_exec_direct(statement, (SQLCHAR *)"SELECT NULL::integer AS null_val", SQL_NTS);
    assert(result == SQL_SUCCESS);

    result = fn_fetch(statement);
    assert(result == SQL_SUCCESS);

    /* Indicator must report NULL; buffer should not be modified */
    assert(indicator == SQL_NULL_DATA);
    assert(int_value == 999);  /* Verify buffer was left untouched */

    fn_free_stmt(statement, SQL_DROP);
    printf("  PASS: test_bind_fetch_null_value\n");
}

/* Test 3: String truncation returns SQL_SUCCESS_WITH_INFO and indicator has full length */
static void test_bind_fetch_truncation(void)
{
    SQLHANDLE statement;
    SQLRETURN result;
    SQLCHAR small_buffer[6];  /* Room for 5 chars + null terminator */
    SQLLEN indicator = 0;

    result = fn_alloc_handle(SQL_HANDLE_STMT, connection_handle, &statement);
    assert(result == SQL_SUCCESS);

    result = fn_bind_col(statement, 1, SQL_C_CHAR, small_buffer, sizeof(small_buffer), &indicator);
    assert(result == SQL_SUCCESS);

    result = fn_exec_direct(statement, (SQLCHAR *)"SELECT 'abcdefghij'", SQL_NTS);
    assert(result == SQL_SUCCESS);

    result = fn_fetch(statement);
    /* Fetch should return SQL_SUCCESS_WITH_INFO because the string was truncated */
    assert(result == SQL_SUCCESS_WITH_INFO);

    /* Indicator holds the full untruncated length */
    assert(indicator == 10);
    /* Buffer holds what fits (5 chars + null) */
    assert(strcmp((char *)small_buffer, "abcde") == 0);

    fn_free_stmt(statement, SQL_DROP);
    printf("  PASS: test_bind_fetch_truncation\n");
}

/* Test 4: SQL_UNBIND clears all bindings; subsequent fetch doesn't write to old buffers */
static void test_unbind_all_columns(void)
{
    SQLHANDLE statement;
    SQLRETURN result;
    SQLINTEGER int_value = 999;
    SQLLEN indicator = 777;

    result = fn_alloc_handle(SQL_HANDLE_STMT, connection_handle, &statement);
    assert(result == SQL_SUCCESS);

    /* Bind and verify it works */
    result = fn_bind_col(statement, 1, SQL_C_SLONG, &int_value, sizeof(int_value), &indicator);
    assert(result == SQL_SUCCESS);

    /* Now unbind all columns */
    result = fn_free_stmt(statement, SQL_UNBIND);
    assert(result == SQL_SUCCESS);

    /* Execute and fetch — bindings are cleared, so buffers should NOT be modified */
    result = fn_exec_direct(statement, (SQLCHAR *)"SELECT 42::int4", SQL_NTS);
    assert(result == SQL_SUCCESS);

    result = fn_fetch(statement);
    assert(result == SQL_SUCCESS);

    /* Sentinel values should be unchanged because no columns are bound */
    assert(int_value == 999);
    assert(indicator == 777);

    fn_free_stmt(statement, SQL_DROP);
    printf("  PASS: test_unbind_all_columns\n");
}

/* Test 5: Mix bound columns and SQLGetData — bind col 1, use SQLGetData for col 2 */
static void test_mixed_bound_and_getdata(void)
{
    SQLHANDLE statement;
    SQLRETURN result;
    SQLINTEGER bound_value = 0;
    SQLLEN bound_indicator = 0;
    SQLCHAR getdata_buffer[64];
    SQLLEN getdata_indicator = 0;

    result = fn_alloc_handle(SQL_HANDLE_STMT, connection_handle, &statement);
    assert(result == SQL_SUCCESS);

    /* Only bind column 1; leave column 2 unbound for SQLGetData */
    result = fn_bind_col(statement, 1, SQL_C_SLONG, &bound_value, sizeof(bound_value), &bound_indicator);
    assert(result == SQL_SUCCESS);

    result = fn_exec_direct(statement,
                            (SQLCHAR *)"SELECT 100 AS bound_col, 'getdata_val' AS unbound_col",
                            SQL_NTS);
    assert(result == SQL_SUCCESS);

    result = fn_fetch(statement);
    assert(result == SQL_SUCCESS);

    /* Verify bound column was populated */
    assert(bound_value == 100);
    assert(bound_indicator == (SQLLEN)sizeof(SQLINTEGER));

    /* Retrieve unbound column via SQLGetData */
    result = fn_get_data(statement, 2, SQL_C_CHAR, getdata_buffer, sizeof(getdata_buffer), &getdata_indicator);
    assert(result == SQL_SUCCESS);
    assert(strcmp((char *)getdata_buffer, "getdata_val") == 0);
    assert(getdata_indicator == 11);

    fn_free_stmt(statement, SQL_DROP);
    printf("  PASS: test_mixed_bound_and_getdata\n");
}

/* Test 6: Unbind a single column by passing NULL target buffer */
static void test_unbind_single_column(void)
{
    SQLHANDLE statement;
    SQLRETURN result;
    SQLINTEGER col1_value = 999;
    SQLINTEGER col2_value = 0;
    SQLLEN col1_indicator = 777;
    SQLLEN col2_indicator = 0;

    result = fn_alloc_handle(SQL_HANDLE_STMT, connection_handle, &statement);
    assert(result == SQL_SUCCESS);

    /* Bind both columns */
    result = fn_bind_col(statement, 1, SQL_C_SLONG, &col1_value, sizeof(col1_value), &col1_indicator);
    assert(result == SQL_SUCCESS);
    result = fn_bind_col(statement, 2, SQL_C_SLONG, &col2_value, sizeof(col2_value), &col2_indicator);
    assert(result == SQL_SUCCESS);

    /* Unbind column 1 only by passing NULL buffer */
    result = fn_bind_col(statement, 1, SQL_C_SLONG, NULL, 0, NULL);
    assert(result == SQL_SUCCESS);

    result = fn_exec_direct(statement, (SQLCHAR *)"SELECT 10, 20", SQL_NTS);
    assert(result == SQL_SUCCESS);

    result = fn_fetch(statement);
    assert(result == SQL_SUCCESS);

    /* Column 1 should NOT be written (was unbound) */
    assert(col1_value == 999);
    assert(col1_indicator == 777);

    /* Column 2 should be populated */
    assert(col2_value == 20);
    assert(col2_indicator == (SQLLEN)sizeof(SQLINTEGER));

    fn_free_stmt(statement, SQL_DROP);
    printf("  PASS: test_unbind_single_column\n");
}

/* Test 7: Binding column 0 (bookmark) should return error */
static void test_bind_column_zero_error(void)
{
    SQLHANDLE statement;
    SQLRETURN result;
    SQLINTEGER dummy;
    SQLLEN indicator;

    result = fn_alloc_handle(SQL_HANDLE_STMT, connection_handle, &statement);
    assert(result == SQL_SUCCESS);

    result = fn_bind_col(statement, 0, SQL_C_SLONG, &dummy, sizeof(dummy), &indicator);
    assert(result == SQL_ERROR);

    /* Verify SQLSTATE is HYC00 (Optional feature not implemented) */
    SQLCHAR sqlstate[6];
    fn_get_diag_rec(SQL_HANDLE_STMT, statement, 1, sqlstate, NULL, NULL, 0, NULL);
    assert(strcmp((char *)sqlstate, "HYC00") == 0);

    fn_free_stmt(statement, SQL_DROP);
    printf("  PASS: test_bind_column_zero_error\n");
}

/* Test 8: Multiple rows — verify bindings update on each fetch */
static void test_bind_fetch_multiple_rows(void)
{
    SQLHANDLE statement;
    SQLRETURN result;
    SQLINTEGER int_value = 0;
    SQLLEN indicator = 0;

    result = fn_alloc_handle(SQL_HANDLE_STMT, connection_handle, &statement);
    assert(result == SQL_SUCCESS);

    result = fn_bind_col(statement, 1, SQL_C_SLONG, &int_value, sizeof(int_value), &indicator);
    assert(result == SQL_SUCCESS);

    result = fn_exec_direct(statement, (SQLCHAR *)"SELECT generate_series(1,3)", SQL_NTS);
    assert(result == SQL_SUCCESS);

    /* Fetch row 1 */
    result = fn_fetch(statement);
    assert(result == SQL_SUCCESS);
    assert(int_value == 1);

    /* Fetch row 2 */
    result = fn_fetch(statement);
    assert(result == SQL_SUCCESS);
    assert(int_value == 2);

    /* Fetch row 3 */
    result = fn_fetch(statement);
    assert(result == SQL_SUCCESS);
    assert(int_value == 3);

    /* No more rows */
    result = fn_fetch(statement);
    assert(result == SQL_NO_DATA);

    fn_free_stmt(statement, SQL_DROP);
    printf("  PASS: test_bind_fetch_multiple_rows\n");
}

int main(void)
{
    const char *connection_string = getenv("PSQLODBC2_TEST_CONNSTR");
    if (!connection_string || connection_string[0] == '\0') {
        printf("SKIP: PSQLODBC2_TEST_CONNSTR not set (no test database configured)\n");
        return MESON_TEST_SKIP;
    }

    printf("test_column_binding:\n");

    load_driver();

    if (connect_to_database(connection_string) != 0) {
        fprintf(stderr, "SKIP: Could not connect to test database\n");
        dlclose(driver_handle);
        return MESON_TEST_SKIP;
    }

    test_bind_and_fetch_typed_columns();
    test_bind_fetch_null_value();
    test_bind_fetch_truncation();
    test_unbind_all_columns();
    test_mixed_bound_and_getdata();
    test_unbind_single_column();
    test_bind_column_zero_error();
    test_bind_fetch_multiple_rows();

    disconnect_and_cleanup();
    dlclose(driver_handle);

    printf("All column binding tests passed.\n");
    return 0;
}
