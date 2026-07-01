/*-------------------------------------------------------------------------
 *
 * test_results.c
 *	  Integration tests for result set retrieval
 *
 * Copyright (c) 2026, Dave Cramer
 *
 *
 * IDENTIFICATION
 *	  tests/test_results.c
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
typedef SQLRETURN (SQL_API *SQLNumResultCols_func)(SQLHSTMT, SQLSMALLINT *);
typedef SQLRETURN (SQL_API *SQLDescribeCol_func)(SQLHSTMT, SQLUSMALLINT, SQLCHAR *, SQLSMALLINT,
                                                 SQLSMALLINT *, SQLSMALLINT *, SQLULEN *,
                                                 SQLSMALLINT *, SQLSMALLINT *);
typedef SQLRETURN (SQL_API *SQLRowCount_func)(SQLHSTMT, SQLLEN *);
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
static SQLNumResultCols_func fn_num_result_cols;
static SQLDescribeCol_func fn_describe_col;
static SQLRowCount_func fn_row_count;
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
    fn_num_result_cols = (SQLNumResultCols_func)dlsym(driver_handle, "SQLNumResultCols");
    fn_describe_col = (SQLDescribeCol_func)dlsym(driver_handle, "SQLDescribeCol");
    fn_row_count = (SQLRowCount_func)dlsym(driver_handle, "SQLRowCount");
    fn_get_diag_rec = (SQLGetDiagRec_func)dlsym(driver_handle, "SQLGetDiagRec");

    assert(fn_alloc_handle && fn_free_handle && fn_free_stmt);
    assert(fn_driver_connect && fn_disconnect && fn_exec_direct);
    assert(fn_fetch && fn_get_data && fn_num_result_cols);
    assert(fn_describe_col && fn_row_count && fn_get_diag_rec);
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

static void test_num_result_cols(void)
{
    SQLHANDLE statement;
    SQLRETURN result;
    SQLSMALLINT column_count;

    result = fn_alloc_handle(SQL_HANDLE_STMT, connection_handle, &statement);
    assert(result == SQL_SUCCESS);

    result = fn_exec_direct(statement, (SQLCHAR *)"SELECT 1 AS a, 2 AS b, 3 AS c", SQL_NTS);
    assert(result == SQL_SUCCESS);

    result = fn_num_result_cols(statement, &column_count);
    assert(result == SQL_SUCCESS);
    assert(column_count == 3);

    fn_free_stmt(statement, SQL_DROP);
    printf("  PASS: test_num_result_cols\n");
}

static void test_describe_col_names(void)
{
    SQLHANDLE statement;
    SQLRETURN result;
    SQLCHAR column_name[128];
    SQLSMALLINT name_length;

    result = fn_alloc_handle(SQL_HANDLE_STMT, connection_handle, &statement);
    assert(result == SQL_SUCCESS);

    result = fn_exec_direct(statement, (SQLCHAR *)"SELECT 1 AS first_col, 2 AS second_col", SQL_NTS);
    assert(result == SQL_SUCCESS);

    result = fn_describe_col(statement, 1, column_name, sizeof(column_name),
                             &name_length, NULL, NULL, NULL, NULL);
    assert(result == SQL_SUCCESS);
    assert(strcmp((char *)column_name, "first_col") == 0);

    result = fn_describe_col(statement, 2, column_name, sizeof(column_name),
                             &name_length, NULL, NULL, NULL, NULL);
    assert(result == SQL_SUCCESS);
    assert(strcmp((char *)column_name, "second_col") == 0);

    fn_free_stmt(statement, SQL_DROP);
    printf("  PASS: test_describe_col_names\n");
}

static void test_describe_col_types(void)
{
    SQLHANDLE statement;
    SQLRETURN result;
    SQLSMALLINT data_type;
    SQLULEN column_size;

    result = fn_alloc_handle(SQL_HANDLE_STMT, connection_handle, &statement);
    assert(result == SQL_SUCCESS);

    result = fn_exec_direct(statement,
                            (SQLCHAR *)"SELECT 'hello'::varchar(50) AS name, 3.14::float8 AS pi",
                            SQL_NTS);
    assert(result == SQL_SUCCESS);

    /* Column 1: varchar(50) */
    result = fn_describe_col(statement, 1, NULL, 0, NULL, &data_type, &column_size, NULL, NULL);
    assert(result == SQL_SUCCESS);
    assert(data_type == SQL_VARCHAR);
    assert(column_size == 50);

    /* Column 2: float8 */
    result = fn_describe_col(statement, 2, NULL, 0, NULL, &data_type, &column_size, NULL, NULL);
    assert(result == SQL_SUCCESS);
    assert(data_type == SQL_DOUBLE);
    assert(column_size == 15);

    fn_free_stmt(statement, SQL_DROP);
    printf("  PASS: test_describe_col_types\n");
}

static void test_row_count_dml(void)
{
    SQLHANDLE statement;
    SQLRETURN result;
    SQLLEN row_count;

    result = fn_alloc_handle(SQL_HANDLE_STMT, connection_handle, &statement);
    assert(result == SQL_SUCCESS);

    result = fn_exec_direct(statement,
                            (SQLCHAR *)"CREATE TEMP TABLE test_results_rc(id int)",
                            SQL_NTS);
    assert(result == SQL_SUCCESS);

    fn_free_stmt(statement, SQL_CLOSE);

    result = fn_exec_direct(statement,
                            (SQLCHAR *)"INSERT INTO test_results_rc VALUES (1),(2),(3)",
                            SQL_NTS);
    assert(result == SQL_SUCCESS);

    result = fn_row_count(statement, &row_count);
    assert(result == SQL_SUCCESS);
    assert(row_count == 3);

    fn_free_stmt(statement, SQL_DROP);
    printf("  PASS: test_row_count_dml\n");
}

static void test_fetch_rows(void)
{
    SQLHANDLE statement;
    SQLRETURN result;
    int fetch_count = 0;

    result = fn_alloc_handle(SQL_HANDLE_STMT, connection_handle, &statement);
    assert(result == SQL_SUCCESS);

    result = fn_exec_direct(statement, (SQLCHAR *)"SELECT generate_series(1,3)", SQL_NTS);
    assert(result == SQL_SUCCESS);

    /* Fetch 3 rows */
    while ((result = fn_fetch(statement)) == SQL_SUCCESS) {
        fetch_count++;
    }
    assert(result == SQL_NO_DATA);
    assert(fetch_count == 3);

    fn_free_stmt(statement, SQL_DROP);
    printf("  PASS: test_fetch_rows\n");
}

static void test_get_data_string(void)
{
    SQLHANDLE statement;
    SQLRETURN result;
    SQLCHAR buffer[256];
    SQLLEN indicator;

    result = fn_alloc_handle(SQL_HANDLE_STMT, connection_handle, &statement);
    assert(result == SQL_SUCCESS);

    result = fn_exec_direct(statement, (SQLCHAR *)"SELECT 'hello world'", SQL_NTS);
    assert(result == SQL_SUCCESS);

    result = fn_fetch(statement);
    assert(result == SQL_SUCCESS);

    result = fn_get_data(statement, 1, SQL_C_CHAR, buffer, sizeof(buffer), &indicator);
    assert(result == SQL_SUCCESS);
    assert(strcmp((char *)buffer, "hello world") == 0);
    assert(indicator == 11);

    fn_free_stmt(statement, SQL_DROP);
    printf("  PASS: test_get_data_string\n");
}

static void test_get_data_integer(void)
{
    SQLHANDLE statement;
    SQLRETURN result;
    SQLINTEGER value;
    SQLLEN indicator;

    result = fn_alloc_handle(SQL_HANDLE_STMT, connection_handle, &statement);
    assert(result == SQL_SUCCESS);

    result = fn_exec_direct(statement, (SQLCHAR *)"SELECT 42::int4", SQL_NTS);
    assert(result == SQL_SUCCESS);

    result = fn_fetch(statement);
    assert(result == SQL_SUCCESS);

    result = fn_get_data(statement, 1, SQL_C_SLONG, &value, sizeof(value), &indicator);
    assert(result == SQL_SUCCESS);
    assert(value == 42);
    assert(indicator == (SQLLEN)sizeof(SQLINTEGER));

    fn_free_stmt(statement, SQL_DROP);
    printf("  PASS: test_get_data_integer\n");
}

static void test_get_data_double(void)
{
    SQLHANDLE statement;
    SQLRETURN result;
    SQLDOUBLE value;
    SQLLEN indicator;

    result = fn_alloc_handle(SQL_HANDLE_STMT, connection_handle, &statement);
    assert(result == SQL_SUCCESS);

    result = fn_exec_direct(statement, (SQLCHAR *)"SELECT 3.14159::float8", SQL_NTS);
    assert(result == SQL_SUCCESS);

    result = fn_fetch(statement);
    assert(result == SQL_SUCCESS);

    result = fn_get_data(statement, 1, SQL_C_DOUBLE, &value, sizeof(value), &indicator);
    assert(result == SQL_SUCCESS);
    assert(fabs(value - 3.14159) < 0.00001);

    fn_free_stmt(statement, SQL_DROP);
    printf("  PASS: test_get_data_double\n");
}

static void test_get_data_null(void)
{
    SQLHANDLE statement;
    SQLRETURN result;
    SQLINTEGER value = 999;
    SQLLEN indicator;

    result = fn_alloc_handle(SQL_HANDLE_STMT, connection_handle, &statement);
    assert(result == SQL_SUCCESS);

    result = fn_exec_direct(statement, (SQLCHAR *)"SELECT NULL::int4", SQL_NTS);
    assert(result == SQL_SUCCESS);

    result = fn_fetch(statement);
    assert(result == SQL_SUCCESS);

    result = fn_get_data(statement, 1, SQL_C_SLONG, &value, sizeof(value), &indicator);
    assert(result == SQL_SUCCESS);
    assert(indicator == SQL_NULL_DATA);

    fn_free_stmt(statement, SQL_DROP);
    printf("  PASS: test_get_data_null\n");
}

static void test_get_data_string_truncation(void)
{
    SQLHANDLE statement;
    SQLRETURN result;
    SQLCHAR small_buffer[6];  /* Only room for 5 chars + null */
    SQLLEN indicator;

    result = fn_alloc_handle(SQL_HANDLE_STMT, connection_handle, &statement);
    assert(result == SQL_SUCCESS);

    result = fn_exec_direct(statement, (SQLCHAR *)"SELECT 'abcdefghij'", SQL_NTS);
    assert(result == SQL_SUCCESS);

    result = fn_fetch(statement);
    assert(result == SQL_SUCCESS);

    result = fn_get_data(statement, 1, SQL_C_CHAR, small_buffer, sizeof(small_buffer), &indicator);
    assert(result == SQL_SUCCESS_WITH_INFO);
    assert(indicator == 10);  /* Full string is 10 chars */
    assert(strcmp((char *)small_buffer, "abcde") == 0);  /* Truncated to 5 chars */

    fn_free_stmt(statement, SQL_DROP);
    printf("  PASS: test_get_data_string_truncation\n");
}

static void test_fetch_after_close_and_reuse(void)
{
    SQLHANDLE statement;
    SQLRETURN result;
    SQLCHAR buffer[64];
    SQLLEN indicator;

    result = fn_alloc_handle(SQL_HANDLE_STMT, connection_handle, &statement);
    assert(result == SQL_SUCCESS);

    /* Execute first query and fetch */
    result = fn_exec_direct(statement, (SQLCHAR *)"SELECT 'first'", SQL_NTS);
    assert(result == SQL_SUCCESS);
    result = fn_fetch(statement);
    assert(result == SQL_SUCCESS);
    result = fn_get_data(statement, 1, SQL_C_CHAR, buffer, sizeof(buffer), &indicator);
    assert(result == SQL_SUCCESS);
    assert(strcmp((char *)buffer, "first") == 0);

    /* Close cursor */
    result = fn_free_stmt(statement, SQL_CLOSE);
    assert(result == SQL_SUCCESS);

    /* Execute new query on same statement */
    result = fn_exec_direct(statement, (SQLCHAR *)"SELECT 'second'", SQL_NTS);
    assert(result == SQL_SUCCESS);
    result = fn_fetch(statement);
    assert(result == SQL_SUCCESS);
    result = fn_get_data(statement, 1, SQL_C_CHAR, buffer, sizeof(buffer), &indicator);
    assert(result == SQL_SUCCESS);
    assert(strcmp((char *)buffer, "second") == 0);

    fn_free_stmt(statement, SQL_DROP);
    printf("  PASS: test_fetch_after_close_and_reuse\n");
}

static void test_get_data_bigint(void)
{
    SQLHANDLE statement;
    SQLRETURN result;
    SQLBIGINT value;
    SQLLEN indicator;

    result = fn_alloc_handle(SQL_HANDLE_STMT, connection_handle, &statement);
    assert(result == SQL_SUCCESS);

    result = fn_exec_direct(statement, (SQLCHAR *)"SELECT 9223372036854775807::int8", SQL_NTS);
    assert(result == SQL_SUCCESS);

    result = fn_fetch(statement);
    assert(result == SQL_SUCCESS);

    result = fn_get_data(statement, 1, SQL_C_SBIGINT, &value, sizeof(value), &indicator);
    assert(result == SQL_SUCCESS);
    assert(value == 9223372036854775807LL);

    fn_free_stmt(statement, SQL_DROP);
    printf("  PASS: test_get_data_bigint\n");
}

int main(void)
{
    const char *connection_string = getenv("PSQLODBC2_TEST_CONNSTR");
    if (!connection_string || connection_string[0] == '\0') {
        printf("SKIP: PSQLODBC2_TEST_CONNSTR not set (no test database configured)\n");
        return MESON_TEST_SKIP;
    }

    printf("test_results:\n");

    load_driver();

    if (connect_to_database(connection_string) != 0) {
        fprintf(stderr, "SKIP: Could not connect to test database\n");
        dlclose(driver_handle);
        return MESON_TEST_SKIP;
    }

    test_num_result_cols();
    test_describe_col_names();
    test_describe_col_types();
    test_row_count_dml();
    test_fetch_rows();
    test_get_data_string();
    test_get_data_integer();
    test_get_data_double();
    test_get_data_null();
    test_get_data_string_truncation();
    test_fetch_after_close_and_reuse();
    test_get_data_bigint();

    disconnect_and_cleanup();
    dlclose(driver_handle);

    printf("All results tests passed.\n");
    return 0;
}
