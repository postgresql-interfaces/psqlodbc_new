/*-------------------------------------------------------------------------
 *
 * test_statement_attributes.c
 *	  Tests for statement attributes
 *
 * Copyright (c) 2026, Dave Cramer
 *
 *
 * IDENTIFICATION
 *	  tests/test_statement_attributes.c
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include <sql.h>
#include <sqlext.h>
#include <sqltypes.h>

/* Platform-specific dynamic loading */
#if defined(_WIN32) || defined(_WIN64)
    #include <windows.h>
    #define LOAD_LIBRARY(path)       LoadLibraryA(path)
    #define GET_SYMBOL(lib, name)    GetProcAddress(lib, name)
    #define CLOSE_LIBRARY(lib)       FreeLibrary(lib)
    #define LIB_HANDLE               HMODULE
    #define LIB_ERROR_MSG()          "LoadLibrary failed"
#else
    #include "test_platform.h"
    #define LOAD_LIBRARY(path)       dlopen(path, RTLD_NOW)
    #define GET_SYMBOL(lib, name)    dlsym(lib, name)
    #define CLOSE_LIBRARY(lib)       dlclose(lib)
    #define LIB_HANDLE               void *
    #define LIB_ERROR_MSG()          dlerror()
#endif

#ifndef DRIVER_LIBRARY_PATH
#error "DRIVER_LIBRARY_PATH must be defined at compile time"
#endif

/* Function pointer types */
typedef SQLRETURN (SQL_API *SQLAllocHandle_func)(SQLSMALLINT, SQLHANDLE, SQLHANDLE *);
typedef SQLRETURN (SQL_API *SQLFreeHandle_func)(SQLSMALLINT, SQLHANDLE);
typedef SQLRETURN (SQL_API *SQLSetStmtAttr_func)(SQLHSTMT, SQLINTEGER, SQLPOINTER, SQLINTEGER);
typedef SQLRETURN (SQL_API *SQLGetStmtAttr_func)(SQLHSTMT, SQLINTEGER, SQLPOINTER, SQLINTEGER, SQLINTEGER *);
typedef SQLRETURN (SQL_API *SQLGetDiagRec_func)(SQLSMALLINT, SQLHANDLE, SQLSMALLINT,
                                                SQLCHAR *, SQLINTEGER *, SQLCHAR *,
                                                SQLSMALLINT, SQLSMALLINT *);

static LIB_HANDLE driver_library;
static SQLAllocHandle_func fn_alloc_handle;
static SQLFreeHandle_func fn_free_handle;
static SQLSetStmtAttr_func fn_set_stmt_attr;
static SQLGetStmtAttr_func fn_get_stmt_attr;
static SQLGetDiagRec_func fn_get_diag_rec;

static int tests_run = 0;
static int tests_passed = 0;

static void load_driver(void)
{
    driver_library = LOAD_LIBRARY(DRIVER_LIBRARY_PATH);
    if (!driver_library) {
        fprintf(stderr, "FATAL: Could not load driver: %s\n", LIB_ERROR_MSG());
        exit(1);
    }

    fn_alloc_handle = (SQLAllocHandle_func)GET_SYMBOL(driver_library, "SQLAllocHandle");
    fn_free_handle = (SQLFreeHandle_func)GET_SYMBOL(driver_library, "SQLFreeHandle");
    fn_set_stmt_attr = (SQLSetStmtAttr_func)GET_SYMBOL(driver_library, "SQLSetStmtAttr");
    fn_get_stmt_attr = (SQLGetStmtAttr_func)GET_SYMBOL(driver_library, "SQLGetStmtAttr");
    fn_get_diag_rec = (SQLGetDiagRec_func)GET_SYMBOL(driver_library, "SQLGetDiagRec");

    if (!fn_alloc_handle || !fn_free_handle || !fn_set_stmt_attr ||
        !fn_get_stmt_attr || !fn_get_diag_rec) {
        fprintf(stderr, "FATAL: Could not resolve required symbols: %s\n", LIB_ERROR_MSG());
        CLOSE_LIBRARY(driver_library);
        exit(1);
    }
}

/* Allocate env -> conn -> stmt handles for testing.
 * No database connection is needed — just handle allocation. */
static int setup_statement(SQLHANDLE *out_env, SQLHANDLE *out_conn, SQLHANDLE *out_stmt)
{
    SQLRETURN result;

    result = fn_alloc_handle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, out_env);
    if (result != SQL_SUCCESS) {
        fprintf(stderr, "    setup: alloc env failed\n");
        return -1;
    }

    result = fn_alloc_handle(SQL_HANDLE_DBC, *out_env, out_conn);
    if (result != SQL_SUCCESS) {
        fprintf(stderr, "    setup: alloc conn failed\n");
        fn_free_handle(SQL_HANDLE_ENV, *out_env);
        return -1;
    }

    result = fn_alloc_handle(SQL_HANDLE_STMT, *out_conn, out_stmt);
    if (result != SQL_SUCCESS) {
        fprintf(stderr, "    setup: alloc stmt failed\n");
        fn_free_handle(SQL_HANDLE_DBC, *out_conn);
        fn_free_handle(SQL_HANDLE_ENV, *out_env);
        return -1;
    }

    return 0;
}

static void teardown_statement(SQLHANDLE env, SQLHANDLE conn, SQLHANDLE stmt)
{
    fn_free_handle(SQL_HANDLE_STMT, stmt);
    fn_free_handle(SQL_HANDLE_DBC, conn);
    fn_free_handle(SQL_HANDLE_ENV, env);
}

/* ---- Test Cases ---- */

static int test_cursor_type_forward_only_roundtrip(void)
{
    printf("  test_cursor_type_forward_only_roundtrip...\n");
    tests_run++;

    SQLHANDLE env, conn, stmt;
    if (setup_statement(&env, &conn, &stmt) != 0) return 1;

    /* Set FORWARD_ONLY — should succeed without info */
    SQLRETURN result = fn_set_stmt_attr(stmt, SQL_ATTR_CURSOR_TYPE,
                                        (SQLPOINTER)(uintptr_t)SQL_CURSOR_FORWARD_ONLY, 0);
    if (result != SQL_SUCCESS) {
        fprintf(stderr, "    FAIL: set FORWARD_ONLY returned %d\n", (int)result);
        teardown_statement(env, conn, stmt);
        return 1;
    }

    /* Get and verify */
    SQLULEN cursor_type = 99;
    result = fn_get_stmt_attr(stmt, SQL_ATTR_CURSOR_TYPE, &cursor_type, 0, NULL);
    if (result != SQL_SUCCESS || cursor_type != SQL_CURSOR_FORWARD_ONLY) {
        fprintf(stderr, "    FAIL: get cursor_type expected %lu, got %lu\n",
                (unsigned long)SQL_CURSOR_FORWARD_ONLY, (unsigned long)cursor_type);
        teardown_statement(env, conn, stmt);
        return 1;
    }

    teardown_statement(env, conn, stmt);
    tests_passed++;
    return 0;
}

static int test_cursor_type_dynamic_downgrade(void)
{
    printf("  test_cursor_type_dynamic_downgrade...\n");
    tests_run++;

    SQLHANDLE env, conn, stmt;
    if (setup_statement(&env, &conn, &stmt) != 0) return 1;

    /* Set DYNAMIC — should return SQL_SUCCESS_WITH_INFO and store STATIC.
     * The driver buffers the whole result set client-side, so it downgrades the
     * unsupported keyset/dynamic cursor types to STATIC (which is scrollable and
     * served from that buffer) rather than to FORWARD_ONLY. */
    SQLRETURN result = fn_set_stmt_attr(stmt, SQL_ATTR_CURSOR_TYPE,
                                        (SQLPOINTER)(uintptr_t)SQL_CURSOR_DYNAMIC, 0);
    if (result != SQL_SUCCESS_WITH_INFO) {
        fprintf(stderr, "    FAIL: set DYNAMIC should return SQL_SUCCESS_WITH_INFO, got %d\n",
                (int)result);
        teardown_statement(env, conn, stmt);
        return 1;
    }

    /* Verify SQLSTATE 01S02 diagnostic */
    SQLCHAR sqlstate[6] = {0};
    SQLINTEGER native_error;
    SQLCHAR message[256];
    SQLSMALLINT text_length;
    fn_get_diag_rec(SQL_HANDLE_STMT, stmt, 1,
                    sqlstate, &native_error, message, sizeof(message), &text_length);
    if (strcmp((const char *)sqlstate, "01S02") != 0) {
        fprintf(stderr, "    FAIL: expected SQLSTATE 01S02, got %s\n", sqlstate);
        teardown_statement(env, conn, stmt);
        return 1;
    }

    /* Get should return STATIC. SQL_ATTR_CURSOR_TYPE is a 32-bit SQLUINTEGER
     * value, so read it into a matching-width variable. */
    SQLUINTEGER cursor_type = 99;
    result = fn_get_stmt_attr(stmt, SQL_ATTR_CURSOR_TYPE, &cursor_type, 0, NULL);
    if (result != SQL_SUCCESS || cursor_type != SQL_CURSOR_STATIC) {
        fprintf(stderr, "    FAIL: after DYNAMIC downgrade, expected STATIC, got %lu\n",
                (unsigned long)cursor_type);
        teardown_statement(env, conn, stmt);
        return 1;
    }

    teardown_statement(env, conn, stmt);
    tests_passed++;
    return 0;
}

static int test_query_timeout_roundtrip(void)
{
    printf("  test_query_timeout_roundtrip...\n");
    tests_run++;

    SQLHANDLE env, conn, stmt;
    if (setup_statement(&env, &conn, &stmt) != 0) return 1;

    /* Set timeout to 30 seconds */
    SQLRETURN result = fn_set_stmt_attr(stmt, SQL_ATTR_QUERY_TIMEOUT,
                                        (SQLPOINTER)(uintptr_t)30, 0);
    if (result != SQL_SUCCESS) {
        fprintf(stderr, "    FAIL: set query timeout returned %d\n", (int)result);
        teardown_statement(env, conn, stmt);
        return 1;
    }

    /* Get and verify */
    SQLULEN timeout = 0;
    result = fn_get_stmt_attr(stmt, SQL_ATTR_QUERY_TIMEOUT, &timeout, 0, NULL);
    if (result != SQL_SUCCESS || timeout != 30) {
        fprintf(stderr, "    FAIL: query timeout expected 30, got %lu\n", (unsigned long)timeout);
        teardown_statement(env, conn, stmt);
        return 1;
    }

    teardown_statement(env, conn, stmt);
    tests_passed++;
    return 0;
}

static int test_max_rows_roundtrip(void)
{
    printf("  test_max_rows_roundtrip...\n");
    tests_run++;

    SQLHANDLE env, conn, stmt;
    if (setup_statement(&env, &conn, &stmt) != 0) return 1;

    /* Set max_rows to 100 */
    SQLRETURN result = fn_set_stmt_attr(stmt, SQL_ATTR_MAX_ROWS,
                                        (SQLPOINTER)(uintptr_t)100, 0);
    if (result != SQL_SUCCESS) {
        fprintf(stderr, "    FAIL: set max rows returned %d\n", (int)result);
        teardown_statement(env, conn, stmt);
        return 1;
    }

    /* Get and verify */
    SQLULEN max_rows = 0;
    result = fn_get_stmt_attr(stmt, SQL_ATTR_MAX_ROWS, &max_rows, 0, NULL);
    if (result != SQL_SUCCESS || max_rows != 100) {
        fprintf(stderr, "    FAIL: max rows expected 100, got %lu\n", (unsigned long)max_rows);
        teardown_statement(env, conn, stmt);
        return 1;
    }

    teardown_statement(env, conn, stmt);
    tests_passed++;
    return 0;
}

static int test_concurrency_read_only_roundtrip(void)
{
    printf("  test_concurrency_read_only_roundtrip...\n");
    tests_run++;

    SQLHANDLE env, conn, stmt;
    if (setup_statement(&env, &conn, &stmt) != 0) return 1;

    /* Set READ_ONLY — should succeed */
    SQLRETURN result = fn_set_stmt_attr(stmt, SQL_ATTR_CONCURRENCY,
                                        (SQLPOINTER)(uintptr_t)SQL_CONCUR_READ_ONLY, 0);
    if (result != SQL_SUCCESS) {
        fprintf(stderr, "    FAIL: set READ_ONLY returned %d\n", (int)result);
        teardown_statement(env, conn, stmt);
        return 1;
    }

    /* Get and verify */
    SQLULEN concurrency = 99;
    result = fn_get_stmt_attr(stmt, SQL_ATTR_CONCURRENCY, &concurrency, 0, NULL);
    if (result != SQL_SUCCESS || concurrency != SQL_CONCUR_READ_ONLY) {
        fprintf(stderr, "    FAIL: concurrency expected READ_ONLY, got %lu\n",
                (unsigned long)concurrency);
        teardown_statement(env, conn, stmt);
        return 1;
    }

    teardown_statement(env, conn, stmt);
    tests_passed++;
    return 0;
}

static int test_concurrency_lock_downgrade(void)
{
    printf("  test_concurrency_lock_downgrade...\n");
    tests_run++;

    SQLHANDLE env, conn, stmt;
    if (setup_statement(&env, &conn, &stmt) != 0) return 1;

    /* Set LOCK — should return SQL_SUCCESS_WITH_INFO and store READ_ONLY */
    SQLRETURN result = fn_set_stmt_attr(stmt, SQL_ATTR_CONCURRENCY,
                                        (SQLPOINTER)(uintptr_t)SQL_CONCUR_LOCK, 0);
    if (result != SQL_SUCCESS_WITH_INFO) {
        fprintf(stderr, "    FAIL: set LOCK should return SQL_SUCCESS_WITH_INFO, got %d\n",
                (int)result);
        teardown_statement(env, conn, stmt);
        return 1;
    }

    /* Verify SQLSTATE 01S02 */
    SQLCHAR sqlstate[6] = {0};
    SQLINTEGER native_error;
    SQLCHAR message[256];
    SQLSMALLINT text_length;
    fn_get_diag_rec(SQL_HANDLE_STMT, stmt, 1,
                    sqlstate, &native_error, message, sizeof(message), &text_length);
    if (strcmp((const char *)sqlstate, "01S02") != 0) {
        fprintf(stderr, "    FAIL: expected SQLSTATE 01S02, got %s\n", sqlstate);
        teardown_statement(env, conn, stmt);
        return 1;
    }

    /* Get should return READ_ONLY */
    SQLULEN concurrency = 99;
    result = fn_get_stmt_attr(stmt, SQL_ATTR_CONCURRENCY, &concurrency, 0, NULL);
    if (result != SQL_SUCCESS || concurrency != SQL_CONCUR_READ_ONLY) {
        fprintf(stderr, "    FAIL: after LOCK downgrade, expected READ_ONLY, got %lu\n",
                (unsigned long)concurrency);
        teardown_statement(env, conn, stmt);
        return 1;
    }

    teardown_statement(env, conn, stmt);
    tests_passed++;
    return 0;
}

static int test_invalid_attribute_returns_error(void)
{
    printf("  test_invalid_attribute_returns_error...\n");
    tests_run++;

    SQLHANDLE env, conn, stmt;
    if (setup_statement(&env, &conn, &stmt) != 0) return 1;

    /* Set a bogus attribute — should return SQL_ERROR */
    SQLRETURN result = fn_set_stmt_attr(stmt, 99999,
                                        (SQLPOINTER)(uintptr_t)1, 0);
    if (result != SQL_ERROR) {
        fprintf(stderr, "    FAIL: invalid attr set should return SQL_ERROR, got %d\n",
                (int)result);
        teardown_statement(env, conn, stmt);
        return 1;
    }

    /* Verify SQLSTATE HY092 */
    SQLCHAR sqlstate[6] = {0};
    SQLINTEGER native_error;
    SQLCHAR message[256];
    SQLSMALLINT text_length;
    fn_get_diag_rec(SQL_HANDLE_STMT, stmt, 1,
                    sqlstate, &native_error, message, sizeof(message), &text_length);
    if (strcmp((const char *)sqlstate, "HY092") != 0) {
        fprintf(stderr, "    FAIL: expected SQLSTATE HY092, got %s\n", sqlstate);
        teardown_statement(env, conn, stmt);
        return 1;
    }

    /* Get a bogus attribute — should also return SQL_ERROR */
    SQLULEN dummy = 0;
    result = fn_get_stmt_attr(stmt, 99999, &dummy, 0, NULL);
    if (result != SQL_ERROR) {
        fprintf(stderr, "    FAIL: invalid attr get should return SQL_ERROR, got %d\n",
                (int)result);
        teardown_statement(env, conn, stmt);
        return 1;
    }

    teardown_statement(env, conn, stmt);
    tests_passed++;
    return 0;
}

static int test_row_number_before_fetch(void)
{
    printf("  test_row_number_before_fetch...\n");
    tests_run++;

    SQLHANDLE env, conn, stmt;
    if (setup_statement(&env, &conn, &stmt) != 0) return 1;

    /* Before any fetch, SQL_ATTR_ROW_NUMBER should return 0 */
    SQLULEN row_number = 99;
    SQLRETURN result = fn_get_stmt_attr(stmt, SQL_ATTR_ROW_NUMBER, &row_number, 0, NULL);
    if (result != SQL_SUCCESS) {
        fprintf(stderr, "    FAIL: get ROW_NUMBER returned %d\n", (int)result);
        teardown_statement(env, conn, stmt);
        return 1;
    }

    if (row_number != 0) {
        fprintf(stderr, "    FAIL: ROW_NUMBER before fetch should be 0, got %lu\n",
                (unsigned long)row_number);
        teardown_statement(env, conn, stmt);
        return 1;
    }

    teardown_statement(env, conn, stmt);
    tests_passed++;
    return 0;
}

int main(void)
{
    int failures = 0;

    printf("test_statement_attributes:\n");

    load_driver();

    failures += test_cursor_type_forward_only_roundtrip();
    failures += test_cursor_type_dynamic_downgrade();
    failures += test_query_timeout_roundtrip();
    failures += test_max_rows_roundtrip();
    failures += test_concurrency_read_only_roundtrip();
    failures += test_concurrency_lock_downgrade();
    failures += test_invalid_attribute_returns_error();
    failures += test_row_number_before_fetch();

    CLOSE_LIBRARY(driver_library);

    printf("\nResults: %d/%d tests passed.\n", tests_passed, tests_run);

    if (failures > 0) {
        fprintf(stderr, "Some tests FAILED.\n");
        return 1;
    }

    printf("All statement attribute tests passed.\n");
    return 0;
}
