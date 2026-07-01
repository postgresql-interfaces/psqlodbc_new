/*-------------------------------------------------------------------------
 *
 * test_statement_lifecycle.c
 *	  Tests for statement handle lifecycle
 *
 * Copyright (c) 2026, Dave Cramer
 *
 *
 * IDENTIFICATION
 *	  tests/test_statement_lifecycle.c
 *
 *-------------------------------------------------------------------------
 */
#include <assert.h>
#include <dlfcn.h>
#include <stdio.h>
#include <sql.h>
#include <sqlext.h>

#ifndef DRIVER_LIBRARY_PATH
#error "DRIVER_LIBRARY_PATH must be defined at compile time"
#endif

/* Function pointer types for ODBC entry points */
typedef SQLRETURN (SQL_API *SQLAllocHandle_func)(SQLSMALLINT, SQLHANDLE, SQLHANDLE *);
typedef SQLRETURN (SQL_API *SQLFreeHandle_func)(SQLSMALLINT, SQLHANDLE);
typedef SQLRETURN (SQL_API *SQLFreeStmt_func)(SQLHSTMT, SQLUSMALLINT);

static void *driver_handle;
static SQLAllocHandle_func fn_alloc_handle;
static SQLFreeHandle_func fn_free_handle;
static SQLFreeStmt_func fn_free_stmt;

static void load_driver(void)
{
    driver_handle = dlopen(DRIVER_LIBRARY_PATH, RTLD_NOW);
    assert(driver_handle != NULL);

    fn_alloc_handle = (SQLAllocHandle_func)dlsym(driver_handle, "SQLAllocHandle");
    assert(fn_alloc_handle != NULL);

    fn_free_handle = (SQLFreeHandle_func)dlsym(driver_handle, "SQLFreeHandle");
    assert(fn_free_handle != NULL);

    fn_free_stmt = (SQLFreeStmt_func)dlsym(driver_handle, "SQLFreeStmt");
    assert(fn_free_stmt != NULL);
}

static void unload_driver(void)
{
    dlclose(driver_handle);
}

/* Helper: allocate env + connection (statement's parent handles) */
static void allocate_env_and_connection(SQLHANDLE *environment, SQLHANDLE *connection)
{
    SQLRETURN result;

    result = fn_alloc_handle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, environment);
    assert(result == SQL_SUCCESS);
    assert(*environment != SQL_NULL_HANDLE);

    result = fn_alloc_handle(SQL_HANDLE_DBC, *environment, connection);
    assert(result == SQL_SUCCESS);
    assert(*connection != SQL_NULL_HANDLE);
}

static void test_allocate_statement(void)
{
    SQLHANDLE environment, connection, statement;
    SQLRETURN result;

    allocate_env_and_connection(&environment, &connection);

    result = fn_alloc_handle(SQL_HANDLE_STMT, connection, &statement);
    assert(result == SQL_SUCCESS);
    assert(statement != SQL_NULL_HANDLE);

    /* Clean up */
    fn_free_handle(SQL_HANDLE_STMT, statement);
    fn_free_handle(SQL_HANDLE_DBC, connection);
    fn_free_handle(SQL_HANDLE_ENV, environment);

    printf("  PASS: test_allocate_statement\n");
}

static void test_free_statement(void)
{
    SQLHANDLE environment, connection, statement;
    SQLRETURN result;

    allocate_env_and_connection(&environment, &connection);

    result = fn_alloc_handle(SQL_HANDLE_STMT, connection, &statement);
    assert(result == SQL_SUCCESS);

    result = fn_free_handle(SQL_HANDLE_STMT, statement);
    assert(result == SQL_SUCCESS);

    /* Connection and env should still be freeable */
    result = fn_free_handle(SQL_HANDLE_DBC, connection);
    assert(result == SQL_SUCCESS);

    result = fn_free_handle(SQL_HANDLE_ENV, environment);
    assert(result == SQL_SUCCESS);

    printf("  PASS: test_free_statement\n");
}

static void test_cannot_free_connection_with_active_statements(void)
{
    SQLHANDLE environment, connection, statement;
    SQLRETURN result;

    allocate_env_and_connection(&environment, &connection);

    result = fn_alloc_handle(SQL_HANDLE_STMT, connection, &statement);
    assert(result == SQL_SUCCESS);

    /* Attempt to free connection while statement is still allocated */
    result = fn_free_handle(SQL_HANDLE_DBC, connection);
    assert(result == SQL_ERROR);

    /* Clean up properly */
    fn_free_handle(SQL_HANDLE_STMT, statement);
    fn_free_handle(SQL_HANDLE_DBC, connection);
    fn_free_handle(SQL_HANDLE_ENV, environment);

    printf("  PASS: test_cannot_free_connection_with_active_statements\n");
}

static void test_free_stmt_sql_close(void)
{
    SQLHANDLE environment, connection, statement;
    SQLRETURN result;

    allocate_env_and_connection(&environment, &connection);

    result = fn_alloc_handle(SQL_HANDLE_STMT, connection, &statement);
    assert(result == SQL_SUCCESS);

    /* SQL_CLOSE on a freshly allocated statement should succeed (no-op) */
    result = fn_free_stmt(statement, SQL_CLOSE);
    assert(result == SQL_SUCCESS);

    /* Statement should still be usable after SQL_CLOSE */
    result = fn_free_handle(SQL_HANDLE_STMT, statement);
    assert(result == SQL_SUCCESS);

    fn_free_handle(SQL_HANDLE_DBC, connection);
    fn_free_handle(SQL_HANDLE_ENV, environment);

    printf("  PASS: test_free_stmt_sql_close\n");
}

static void test_free_stmt_sql_drop(void)
{
    SQLHANDLE environment, connection, statement;
    SQLRETURN result;

    allocate_env_and_connection(&environment, &connection);

    result = fn_alloc_handle(SQL_HANDLE_STMT, connection, &statement);
    assert(result == SQL_SUCCESS);

    /* SQL_DROP should free the statement */
    result = fn_free_stmt(statement, SQL_DROP);
    assert(result == SQL_SUCCESS);

    /* Connection should now be freeable (no statements) */
    result = fn_free_handle(SQL_HANDLE_DBC, connection);
    assert(result == SQL_SUCCESS);

    fn_free_handle(SQL_HANDLE_ENV, environment);

    printf("  PASS: test_free_stmt_sql_drop\n");
}

static void test_free_stmt_sql_unbind_and_reset_params(void)
{
    SQLHANDLE environment, connection, statement;
    SQLRETURN result;

    allocate_env_and_connection(&environment, &connection);

    result = fn_alloc_handle(SQL_HANDLE_STMT, connection, &statement);
    assert(result == SQL_SUCCESS);

    /* SQL_UNBIND and SQL_RESET_PARAMS are no-ops but should return SQL_SUCCESS */
    result = fn_free_stmt(statement, SQL_UNBIND);
    assert(result == SQL_SUCCESS);

    result = fn_free_stmt(statement, SQL_RESET_PARAMS);
    assert(result == SQL_SUCCESS);

    fn_free_handle(SQL_HANDLE_STMT, statement);
    fn_free_handle(SQL_HANDLE_DBC, connection);
    fn_free_handle(SQL_HANDLE_ENV, environment);

    printf("  PASS: test_free_stmt_sql_unbind_and_reset_params\n");
}

static void test_free_stmt_invalid_option(void)
{
    SQLHANDLE environment, connection, statement;
    SQLRETURN result;

    allocate_env_and_connection(&environment, &connection);

    result = fn_alloc_handle(SQL_HANDLE_STMT, connection, &statement);
    assert(result == SQL_SUCCESS);

    /* Invalid option should return SQL_ERROR */
    result = fn_free_stmt(statement, 9999);
    assert(result == SQL_ERROR);

    fn_free_handle(SQL_HANDLE_STMT, statement);
    fn_free_handle(SQL_HANDLE_DBC, connection);
    fn_free_handle(SQL_HANDLE_ENV, environment);

    printf("  PASS: test_free_stmt_invalid_option\n");
}

static void test_allocate_multiple_statements(void)
{
    SQLHANDLE environment, connection;
    SQLHANDLE statements[10];
    SQLRETURN result;

    allocate_env_and_connection(&environment, &connection);

    /* Allocate 10 statements on the same connection */
    for (int index = 0; index < 10; index++) {
        result = fn_alloc_handle(SQL_HANDLE_STMT, connection, &statements[index]);
        assert(result == SQL_SUCCESS);
        assert(statements[index] != SQL_NULL_HANDLE);
    }

    /* Free them all */
    for (int index = 0; index < 10; index++) {
        result = fn_free_handle(SQL_HANDLE_STMT, statements[index]);
        assert(result == SQL_SUCCESS);
    }

    fn_free_handle(SQL_HANDLE_DBC, connection);
    fn_free_handle(SQL_HANDLE_ENV, environment);

    printf("  PASS: test_allocate_multiple_statements\n");
}

static void test_invalid_handle(void)
{
    SQLRETURN result;

    /* NULL handle */
    result = fn_free_handle(SQL_HANDLE_STMT, SQL_NULL_HANDLE);
    assert(result == SQL_INVALID_HANDLE);

    /* NULL handle for SQLFreeStmt */
    result = fn_free_stmt(SQL_NULL_HANDLE, SQL_CLOSE);
    assert(result == SQL_INVALID_HANDLE);

    printf("  PASS: test_invalid_handle\n");
}

int main(void)
{
    printf("test_statement_lifecycle:\n");

    load_driver();

    test_allocate_statement();
    test_free_statement();
    test_cannot_free_connection_with_active_statements();
    test_free_stmt_sql_close();
    test_free_stmt_sql_drop();
    test_free_stmt_sql_unbind_and_reset_params();
    test_free_stmt_invalid_option();
    test_allocate_multiple_statements();
    test_invalid_handle();

    unload_driver();

    printf("All statement lifecycle tests passed.\n");
    return 0;
}
