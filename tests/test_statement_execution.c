/*-------------------------------------------------------------------------
 *
 * test_statement_execution.c
 *	  Integration tests for statement execution
 *
 * Copyright (c) 2026, Dave Cramer
 *
 *
 * IDENTIFICATION
 *	  tests/test_statement_execution.c
 *
 *-------------------------------------------------------------------------
 */
#include <assert.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sql.h>
#include <sqlext.h>

#ifndef DRIVER_LIBRARY_PATH
#error "DRIVER_LIBRARY_PATH must be defined at compile time"
#endif

/* Meson test skip exit code — signals the test was not run, not that it failed */
#define MESON_TEST_SKIP 77

/* Function pointer types */
typedef SQLRETURN (SQL_API *SQLAllocHandle_func)(SQLSMALLINT, SQLHANDLE, SQLHANDLE *);
typedef SQLRETURN (SQL_API *SQLFreeHandle_func)(SQLSMALLINT, SQLHANDLE);
typedef SQLRETURN (SQL_API *SQLFreeStmt_func)(SQLHSTMT, SQLUSMALLINT);
typedef SQLRETURN (SQL_API *SQLDriverConnect_func)(SQLHDBC, SQLHWND, SQLCHAR *, SQLSMALLINT,
                                                   SQLCHAR *, SQLSMALLINT, SQLSMALLINT *, SQLUSMALLINT);
typedef SQLRETURN (SQL_API *SQLDisconnect_func)(SQLHDBC);
typedef SQLRETURN (SQL_API *SQLPrepare_func)(SQLHSTMT, SQLCHAR *, SQLINTEGER);
typedef SQLRETURN (SQL_API *SQLExecute_func)(SQLHSTMT);
typedef SQLRETURN (SQL_API *SQLExecDirect_func)(SQLHSTMT, SQLCHAR *, SQLINTEGER);
typedef SQLRETURN (SQL_API *SQLGetDiagRec_func)(SQLSMALLINT, SQLHANDLE, SQLSMALLINT,
                                                SQLCHAR *, SQLINTEGER *, SQLCHAR *,
                                                SQLSMALLINT, SQLSMALLINT *);

static void *driver_handle;
static SQLAllocHandle_func fn_alloc_handle;
static SQLFreeHandle_func fn_free_handle;
static SQLFreeStmt_func fn_free_stmt;
static SQLDriverConnect_func fn_driver_connect;
static SQLDisconnect_func fn_disconnect;
static SQLPrepare_func fn_prepare;
static SQLExecute_func fn_execute;
static SQLExecDirect_func fn_exec_direct;
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
    fn_prepare = (SQLPrepare_func)dlsym(driver_handle, "SQLPrepare");
    fn_execute = (SQLExecute_func)dlsym(driver_handle, "SQLExecute");
    fn_exec_direct = (SQLExecDirect_func)dlsym(driver_handle, "SQLExecDirect");
    fn_get_diag_rec = (SQLGetDiagRec_func)dlsym(driver_handle, "SQLGetDiagRec");

    assert(fn_alloc_handle && fn_free_handle && fn_free_stmt);
    assert(fn_driver_connect && fn_disconnect);
    assert(fn_prepare && fn_execute && fn_exec_direct && fn_get_diag_rec);
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
        /* Print the diagnostic for debugging */
        SQLCHAR message[512];
        SQLCHAR sqlstate[6];
        SQLINTEGER native_error;
        SQLSMALLINT text_length;
        fn_get_diag_rec(SQL_HANDLE_DBC, connection_handle, 1,
                        sqlstate, &native_error, message, sizeof(message), &text_length);
        fprintf(stderr, "Connection failed: [%s] %s\n", sqlstate, message);
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

static void test_exec_direct_select(void)
{
    SQLHANDLE statement;
    SQLRETURN result;

    result = fn_alloc_handle(SQL_HANDLE_STMT, connection_handle, &statement);
    assert(result == SQL_SUCCESS);

    result = fn_exec_direct(statement, (SQLCHAR *)"SELECT 1 AS test_value", SQL_NTS);
    assert(result == SQL_SUCCESS);

    fn_free_stmt(statement, SQL_DROP);
    printf("  PASS: test_exec_direct_select\n");
}

static void test_exec_direct_invalid_sql(void)
{
    SQLHANDLE statement;
    SQLRETURN result;

    result = fn_alloc_handle(SQL_HANDLE_STMT, connection_handle, &statement);
    assert(result == SQL_SUCCESS);

    result = fn_exec_direct(statement, (SQLCHAR *)"SELEC TYPO FROM NOWHERE", SQL_NTS);
    assert(result == SQL_ERROR);

    /* Verify diagnostic record has a meaningful message */
    SQLCHAR message[512];
    SQLCHAR sqlstate[6];
    SQLINTEGER native_error;
    SQLSMALLINT text_length;
    result = fn_get_diag_rec(SQL_HANDLE_STMT, statement, 1,
                             sqlstate, &native_error, message, sizeof(message), &text_length);
    assert(result == SQL_SUCCESS);
    assert(text_length > 0);

    fn_free_stmt(statement, SQL_DROP);
    printf("  PASS: test_exec_direct_invalid_sql\n");
}

static void test_exec_direct_ddl(void)
{
    SQLHANDLE statement;
    SQLRETURN result;

    result = fn_alloc_handle(SQL_HANDLE_STMT, connection_handle, &statement);
    assert(result == SQL_SUCCESS);

    result = fn_exec_direct(statement,
                            (SQLCHAR *)"CREATE TEMP TABLE test_stmt_exec(id int, name text)",
                            SQL_NTS);
    assert(result == SQL_SUCCESS);

    fn_free_stmt(statement, SQL_DROP);
    printf("  PASS: test_exec_direct_ddl\n");
}

static void test_exec_direct_dml(void)
{
    SQLHANDLE statement;
    SQLRETURN result;

    result = fn_alloc_handle(SQL_HANDLE_STMT, connection_handle, &statement);
    assert(result == SQL_SUCCESS);

    result = fn_exec_direct(statement,
                            (SQLCHAR *)"INSERT INTO test_stmt_exec VALUES (1, 'hello')",
                            SQL_NTS);
    assert(result == SQL_SUCCESS);

    fn_free_stmt(statement, SQL_DROP);
    printf("  PASS: test_exec_direct_dml\n");
}

static void test_prepare_and_execute(void)
{
    SQLHANDLE statement;
    SQLRETURN result;

    result = fn_alloc_handle(SQL_HANDLE_STMT, connection_handle, &statement);
    assert(result == SQL_SUCCESS);

    /* Prepare a parameterless query */
    result = fn_prepare(statement, (SQLCHAR *)"SELECT 42 AS answer", SQL_NTS);
    assert(result == SQL_SUCCESS);

    /* Execute the prepared statement */
    result = fn_execute(statement);
    assert(result == SQL_SUCCESS);

    /* Execute again (re-execution of a prepared statement) */
    result = fn_execute(statement);
    assert(result == SQL_SUCCESS);

    fn_free_stmt(statement, SQL_DROP);
    printf("  PASS: test_prepare_and_execute\n");
}

static void test_execute_without_prepare(void)
{
    SQLHANDLE statement;
    SQLRETURN result;

    result = fn_alloc_handle(SQL_HANDLE_STMT, connection_handle, &statement);
    assert(result == SQL_SUCCESS);

    /* Calling SQLExecute without SQLPrepare should fail */
    result = fn_execute(statement);
    assert(result == SQL_ERROR);

    fn_free_stmt(statement, SQL_DROP);
    printf("  PASS: test_execute_without_prepare\n");
}

static void test_prepare_invalid_sql(void)
{
    SQLHANDLE statement;
    SQLRETURN result;

    result = fn_alloc_handle(SQL_HANDLE_STMT, connection_handle, &statement);
    assert(result == SQL_SUCCESS);

    result = fn_prepare(statement, (SQLCHAR *)"INVALID SQL SYNTAX HERE!!!", SQL_NTS);
    assert(result == SQL_ERROR);

    /* Verify diagnostic */
    SQLCHAR message[512];
    SQLCHAR sqlstate[6];
    SQLINTEGER native_error;
    SQLSMALLINT text_length;
    result = fn_get_diag_rec(SQL_HANDLE_STMT, statement, 1,
                             sqlstate, &native_error, message, sizeof(message), &text_length);
    assert(result == SQL_SUCCESS);
    assert(text_length > 0);

    fn_free_stmt(statement, SQL_DROP);
    printf("  PASS: test_prepare_invalid_sql\n");
}

static void test_sql_close_then_reuse(void)
{
    SQLHANDLE statement;
    SQLRETURN result;

    result = fn_alloc_handle(SQL_HANDLE_STMT, connection_handle, &statement);
    assert(result == SQL_SUCCESS);

    /* Execute a query */
    result = fn_exec_direct(statement, (SQLCHAR *)"SELECT 1", SQL_NTS);
    assert(result == SQL_SUCCESS);

    /* Close the cursor */
    result = fn_free_stmt(statement, SQL_CLOSE);
    assert(result == SQL_SUCCESS);

    /* Should be able to execute another query on the same statement */
    result = fn_exec_direct(statement, (SQLCHAR *)"SELECT 2", SQL_NTS);
    assert(result == SQL_SUCCESS);

    fn_free_stmt(statement, SQL_DROP);
    printf("  PASS: test_sql_close_then_reuse\n");
}

int main(void)
{
    const char *connection_string = getenv("PSQLODBC2_TEST_CONNSTR");
    if (!connection_string || connection_string[0] == '\0') {
        printf("SKIP: PSQLODBC2_TEST_CONNSTR not set (no test database configured)\n");
        return MESON_TEST_SKIP;
    }

    printf("test_statement_execution:\n");

    load_driver();

    if (connect_to_database(connection_string) != 0) {
        fprintf(stderr, "SKIP: Could not connect to test database\n");
        dlclose(driver_handle);
        return MESON_TEST_SKIP;
    }

    test_exec_direct_select();
    test_exec_direct_invalid_sql();
    test_exec_direct_ddl();
    test_exec_direct_dml();
    test_prepare_and_execute();
    test_execute_without_prepare();
    test_prepare_invalid_sql();
    test_sql_close_then_reuse();

    disconnect_and_cleanup();
    dlclose(driver_handle);

    printf("All statement execution tests passed.\n");
    return 0;
}
