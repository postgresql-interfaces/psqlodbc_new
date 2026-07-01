/*-------------------------------------------------------------------------
 *
 * test_error_mapping.c
 *	  Integration tests for error SQLSTATE mapping
 *
 * Copyright (c) 2026, Dave Cramer
 *
 *
 * IDENTIFICATION
 *	  tests/test_error_mapping.c
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
    fn_exec_direct = (SQLExecDirect_func)dlsym(driver_handle, "SQLExecDirect");
    fn_get_diag_rec = (SQLGetDiagRec_func)dlsym(driver_handle, "SQLGetDiagRec");

    assert(fn_alloc_handle && fn_free_handle && fn_free_stmt);
    assert(fn_driver_connect && fn_disconnect);
    assert(fn_exec_direct && fn_get_diag_rec);
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

/*
 * Test: syntax errors produce SQLSTATE "42601" (syntax_error).
 * PostgreSQL returns this when SQL cannot be parsed.
 */
static void test_syntax_error_produces_42601(void)
{
    SQLHANDLE statement;
    SQLRETURN result;

    result = fn_alloc_handle(SQL_HANDLE_STMT, connection_handle, &statement);
    assert(result == SQL_SUCCESS);

    /* "SELECTT" is not valid SQL — PostgreSQL will return 42601 */
    result = fn_exec_direct(statement, (SQLCHAR *)"SELECTT 1", SQL_NTS);
    assert(result == SQL_ERROR);

    SQLCHAR message[1024];
    SQLCHAR sqlstate[6];
    SQLINTEGER native_error;
    SQLSMALLINT text_length;
    result = fn_get_diag_rec(SQL_HANDLE_STMT, statement, 1,
                             sqlstate, &native_error, message, sizeof(message), &text_length);
    assert(result == SQL_SUCCESS);

    /* The SQLSTATE must be "42601" (syntax_error), not generic "HY000" */
    assert(strcmp((char *)sqlstate, "42601") == 0);

    /* The message should contain meaningful text from PostgreSQL */
    assert(text_length > 0);

    fn_free_stmt(statement, SQL_DROP);
    printf("  PASS: test_syntax_error_produces_42601\n");
}

/*
 * Test: unique constraint violations produce SQLSTATE "23505" (unique_violation).
 * This is critical for applications that detect duplicate-key conflicts.
 */
static void test_unique_violation_produces_23505(void)
{
    SQLHANDLE statement;
    SQLRETURN result;

    result = fn_alloc_handle(SQL_HANDLE_STMT, connection_handle, &statement);
    assert(result == SQL_SUCCESS);

    /* Create a table with a unique constraint */
    result = fn_exec_direct(statement,
        (SQLCHAR *)"CREATE TEMP TABLE test_unique_err(id INT PRIMARY KEY, name TEXT)",
        SQL_NTS);
    assert(result == SQL_SUCCESS);
    fn_free_stmt(statement, SQL_CLOSE);

    /* Insert a row */
    result = fn_exec_direct(statement,
        (SQLCHAR *)"INSERT INTO test_unique_err VALUES (1, 'first')",
        SQL_NTS);
    assert(result == SQL_SUCCESS);
    fn_free_stmt(statement, SQL_CLOSE);

    /* Insert a duplicate — should produce unique_violation */
    result = fn_exec_direct(statement,
        (SQLCHAR *)"INSERT INTO test_unique_err VALUES (1, 'duplicate')",
        SQL_NTS);
    assert(result == SQL_ERROR);

    SQLCHAR message[1024];
    SQLCHAR sqlstate[6];
    SQLINTEGER native_error;
    SQLSMALLINT text_length;
    result = fn_get_diag_rec(SQL_HANDLE_STMT, statement, 1,
                             sqlstate, &native_error, message, sizeof(message), &text_length);
    assert(result == SQL_SUCCESS);

    /* SQLSTATE must be "23505" (unique_violation) */
    assert(strcmp((char *)sqlstate, "23505") == 0);

    /* The message should mention the constraint or key values */
    assert(text_length > 0);

    /* DETAIL should be included in the message (PostgreSQL provides it for
     * unique violations: "Key (id)=(1) already exists.") */
    assert(strstr((char *)message, "DETAIL:") != NULL ||
           strstr((char *)message, "already exists") != NULL);

    fn_free_stmt(statement, SQL_DROP);
    printf("  PASS: test_unique_violation_produces_23505\n");
}

/*
 * Test: referencing a non-existent table produces SQLSTATE "42P01" (undefined_table).
 */
static void test_undefined_table_produces_42P01(void)
{
    SQLHANDLE statement;
    SQLRETURN result;

    result = fn_alloc_handle(SQL_HANDLE_STMT, connection_handle, &statement);
    assert(result == SQL_SUCCESS);

    result = fn_exec_direct(statement,
        (SQLCHAR *)"SELECT * FROM this_table_does_not_exist_xyz",
        SQL_NTS);
    assert(result == SQL_ERROR);

    SQLCHAR message[1024];
    SQLCHAR sqlstate[6];
    SQLINTEGER native_error;
    SQLSMALLINT text_length;
    result = fn_get_diag_rec(SQL_HANDLE_STMT, statement, 1,
                             sqlstate, &native_error, message, sizeof(message), &text_length);
    assert(result == SQL_SUCCESS);

    /* SQLSTATE must be "42P01" (undefined_table) */
    assert(strcmp((char *)sqlstate, "42P01") == 0);

    /* The message should mention the table name */
    assert(strstr((char *)message, "this_table_does_not_exist_xyz") != NULL);

    fn_free_stmt(statement, SQL_DROP);
    printf("  PASS: test_undefined_table_produces_42P01\n");
}

/*
 * Test: error message contains meaningful text from PostgreSQL.
 * Verifies that the primary message is not lost during extraction.
 */
static void test_error_message_contains_primary_text(void)
{
    SQLHANDLE statement;
    SQLRETURN result;

    result = fn_alloc_handle(SQL_HANDLE_STMT, connection_handle, &statement);
    assert(result == SQL_SUCCESS);

    /* Division by zero produces a clear error message from PostgreSQL */
    result = fn_exec_direct(statement, (SQLCHAR *)"SELECT 1/0", SQL_NTS);
    assert(result == SQL_ERROR);

    SQLCHAR message[1024];
    SQLCHAR sqlstate[6];
    SQLINTEGER native_error;
    SQLSMALLINT text_length;
    result = fn_get_diag_rec(SQL_HANDLE_STMT, statement, 1,
                             sqlstate, &native_error, message, sizeof(message), &text_length);
    assert(result == SQL_SUCCESS);

    /* SQLSTATE should be "22012" (division_by_zero) */
    assert(strcmp((char *)sqlstate, "22012") == 0);

    /* Message should contain "division by zero" */
    assert(strstr((char *)message, "division by zero") != NULL);

    fn_free_stmt(statement, SQL_DROP);
    printf("  PASS: test_error_message_contains_primary_text\n");
}

int main(void)
{
    const char *connection_string = getenv("PSQLODBC2_TEST_CONNSTR");
    if (!connection_string || connection_string[0] == '\0') {
        printf("SKIP: PSQLODBC2_TEST_CONNSTR not set (no test database configured)\n");
        return MESON_TEST_SKIP;
    }

    printf("test_error_mapping:\n");

    load_driver();

    if (connect_to_database(connection_string) != 0) {
        fprintf(stderr, "SKIP: Could not connect to test database\n");
        dlclose(driver_handle);
        return MESON_TEST_SKIP;
    }

    test_syntax_error_produces_42601();
    test_unique_violation_produces_23505();
    test_undefined_table_produces_42P01();
    test_error_message_contains_primary_text();

    disconnect_and_cleanup();
    dlclose(driver_handle);

    printf("All error mapping tests passed.\n");
    return 0;
}
