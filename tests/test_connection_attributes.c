/*-------------------------------------------------------------------------
 *
 * test_connection_attributes.c
 *	  Tests for connection attributes
 *
 * Copyright (c) 2026, Dave Cramer
 *
 *
 * IDENTIFICATION
 *	  tests/test_connection_attributes.c
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
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
    #include <dlfcn.h>
    #define LOAD_LIBRARY(path)       dlopen(path, RTLD_NOW)
    #define GET_SYMBOL(lib, name)    dlsym(lib, name)
    #define CLOSE_LIBRARY(lib)       dlclose(lib)
    #define LIB_HANDLE               void *
    #define LIB_ERROR_MSG()          dlerror()
#endif

#ifndef DRIVER_LIBRARY_PATH
#error "DRIVER_LIBRARY_PATH must be defined at compile time"
#endif

/* Meson test skip exit code */
#define MESON_TEST_SKIP 77

/* Function pointer types */
typedef SQLRETURN (SQL_API *SQLAllocHandle_func)(SQLSMALLINT, SQLHANDLE, SQLHANDLE *);
typedef SQLRETURN (SQL_API *SQLFreeHandle_func)(SQLSMALLINT, SQLHANDLE);
typedef SQLRETURN (SQL_API *SQLDriverConnect_func)(SQLHDBC, SQLHWND, SQLCHAR *, SQLSMALLINT,
                                                   SQLCHAR *, SQLSMALLINT, SQLSMALLINT *, SQLUSMALLINT);
typedef SQLRETURN (SQL_API *SQLDisconnect_func)(SQLHDBC);
typedef SQLRETURN (SQL_API *SQLSetConnectAttr_func)(SQLHDBC, SQLINTEGER, SQLPOINTER, SQLINTEGER);
typedef SQLRETURN (SQL_API *SQLGetConnectAttr_func)(SQLHDBC, SQLINTEGER, SQLPOINTER, SQLINTEGER, SQLINTEGER *);
typedef SQLRETURN (SQL_API *SQLEndTran_func)(SQLSMALLINT, SQLHANDLE, SQLSMALLINT);
typedef SQLRETURN (SQL_API *SQLExecDirect_func)(SQLHSTMT, SQLCHAR *, SQLINTEGER);
typedef SQLRETURN (SQL_API *SQLFreeStmt_func)(SQLHSTMT, SQLUSMALLINT);
typedef SQLRETURN (SQL_API *SQLFetch_func)(SQLHSTMT);
typedef SQLRETURN (SQL_API *SQLGetData_func)(SQLHSTMT, SQLUSMALLINT, SQLSMALLINT,
                                             SQLPOINTER, SQLLEN, SQLLEN *);
typedef SQLRETURN (SQL_API *SQLGetDiagRec_func)(SQLSMALLINT, SQLHANDLE, SQLSMALLINT,
                                                SQLCHAR *, SQLINTEGER *, SQLCHAR *,
                                                SQLSMALLINT, SQLSMALLINT *);

static LIB_HANDLE driver_library;
static SQLAllocHandle_func fn_alloc_handle;
static SQLFreeHandle_func fn_free_handle;
static SQLDriverConnect_func fn_driver_connect;
static SQLDisconnect_func fn_disconnect;
static SQLSetConnectAttr_func fn_set_connect_attr;
static SQLGetConnectAttr_func fn_get_connect_attr;
static SQLEndTran_func fn_end_tran;
static SQLExecDirect_func fn_exec_direct;
static SQLFreeStmt_func fn_free_stmt;
static SQLFetch_func fn_fetch;
static SQLGetData_func fn_get_data;
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
    fn_driver_connect = (SQLDriverConnect_func)GET_SYMBOL(driver_library, "SQLDriverConnect");
    fn_disconnect = (SQLDisconnect_func)GET_SYMBOL(driver_library, "SQLDisconnect");
    fn_set_connect_attr = (SQLSetConnectAttr_func)GET_SYMBOL(driver_library, "SQLSetConnectAttr");
    fn_get_connect_attr = (SQLGetConnectAttr_func)GET_SYMBOL(driver_library, "SQLGetConnectAttr");
    fn_end_tran = (SQLEndTran_func)GET_SYMBOL(driver_library, "SQLEndTran");
    fn_exec_direct = (SQLExecDirect_func)GET_SYMBOL(driver_library, "SQLExecDirect");
    fn_free_stmt = (SQLFreeStmt_func)GET_SYMBOL(driver_library, "SQLFreeStmt");
    fn_fetch = (SQLFetch_func)GET_SYMBOL(driver_library, "SQLFetch");
    fn_get_data = (SQLGetData_func)GET_SYMBOL(driver_library, "SQLGetData");
    fn_get_diag_rec = (SQLGetDiagRec_func)GET_SYMBOL(driver_library, "SQLGetDiagRec");

    if (!fn_alloc_handle || !fn_free_handle || !fn_driver_connect || !fn_disconnect ||
        !fn_set_connect_attr || !fn_get_connect_attr || !fn_end_tran ||
        !fn_exec_direct || !fn_free_stmt || !fn_fetch || !fn_get_data || !fn_get_diag_rec) {
        fprintf(stderr, "FATAL: Could not resolve required symbols: %s\n", LIB_ERROR_MSG());
        CLOSE_LIBRARY(driver_library);
        exit(1);
    }
}

/* ---- Helper: allocate env + connection and connect ---- */

static int setup_connected(const char *connection_string,
                           SQLHANDLE *out_env, SQLHANDLE *out_conn)
{
    SQLRETURN result;

    result = fn_alloc_handle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, out_env);
    if (result != SQL_SUCCESS) return -1;

    result = fn_alloc_handle(SQL_HANDLE_DBC, *out_env, out_conn);
    if (result != SQL_SUCCESS) return -1;

    result = fn_driver_connect(*out_conn, NULL,
                               (SQLCHAR *)connection_string, SQL_NTS,
                               NULL, 0, NULL, SQL_DRIVER_NOPROMPT);
    if (result != SQL_SUCCESS) {
        SQLCHAR message[512];
        SQLCHAR sqlstate[6];
        SQLINTEGER native_error;
        SQLSMALLINT text_length;
        fn_get_diag_rec(SQL_HANDLE_DBC, *out_conn, 1,
                        sqlstate, &native_error, message, sizeof(message), &text_length);
        fprintf(stderr, "    Connection failed: [%s] %s\n", sqlstate, message);
        return -1;
    }

    return 0;
}

static void teardown_connected(SQLHANDLE env, SQLHANDLE conn)
{
    fn_disconnect(conn);
    fn_free_handle(SQL_HANDLE_DBC, conn);
    fn_free_handle(SQL_HANDLE_ENV, env);
}

/* ---- No-DB Tests: attribute set/get before connecting ---- */

static int test_set_get_attributes_before_connect(void)
{
    printf("  test_set_get_attributes_before_connect...\n");
    tests_run++;

    SQLHANDLE env, conn;
    SQLRETURN result;

    result = fn_alloc_handle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env);
    if (result != SQL_SUCCESS) {
        fprintf(stderr, "    FAIL: alloc env\n");
        return 1;
    }
    result = fn_alloc_handle(SQL_HANDLE_DBC, env, &conn);
    if (result != SQL_SUCCESS) {
        fprintf(stderr, "    FAIL: alloc conn\n");
        fn_free_handle(SQL_HANDLE_ENV, env);
        return 1;
    }

    /* Default autocommit should be ON */
    SQLUINTEGER attr_value = 0;
    result = fn_get_connect_attr(conn, SQL_ATTR_AUTOCOMMIT, &attr_value, 0, NULL);
    if (result != SQL_SUCCESS || attr_value != SQL_AUTOCOMMIT_ON) {
        fprintf(stderr, "    FAIL: default autocommit should be ON, got %u\n", (unsigned)attr_value);
        fn_free_handle(SQL_HANDLE_DBC, conn);
        fn_free_handle(SQL_HANDLE_ENV, env);
        return 1;
    }

    /* Set autocommit OFF */
    result = fn_set_connect_attr(conn, SQL_ATTR_AUTOCOMMIT,
                                 (SQLPOINTER)(uintptr_t)SQL_AUTOCOMMIT_OFF, 0);
    if (result != SQL_SUCCESS) {
        fprintf(stderr, "    FAIL: set autocommit OFF returned %d\n", (int)result);
        fn_free_handle(SQL_HANDLE_DBC, conn);
        fn_free_handle(SQL_HANDLE_ENV, env);
        return 1;
    }

    /* Verify autocommit is now OFF */
    attr_value = 99;
    result = fn_get_connect_attr(conn, SQL_ATTR_AUTOCOMMIT, &attr_value, 0, NULL);
    if (result != SQL_SUCCESS || attr_value != SQL_AUTOCOMMIT_OFF) {
        fprintf(stderr, "    FAIL: autocommit should be OFF, got %u\n", (unsigned)attr_value);
        fn_free_handle(SQL_HANDLE_DBC, conn);
        fn_free_handle(SQL_HANDLE_ENV, env);
        return 1;
    }

    /* Set and get login timeout */
    result = fn_set_connect_attr(conn, SQL_ATTR_LOGIN_TIMEOUT,
                                 (SQLPOINTER)(uintptr_t)30, 0);
    if (result != SQL_SUCCESS) {
        fprintf(stderr, "    FAIL: set login timeout\n");
        fn_free_handle(SQL_HANDLE_DBC, conn);
        fn_free_handle(SQL_HANDLE_ENV, env);
        return 1;
    }
    attr_value = 0;
    result = fn_get_connect_attr(conn, SQL_ATTR_LOGIN_TIMEOUT, &attr_value, 0, NULL);
    if (result != SQL_SUCCESS || attr_value != 30) {
        fprintf(stderr, "    FAIL: login timeout should be 30, got %u\n", (unsigned)attr_value);
        fn_free_handle(SQL_HANDLE_DBC, conn);
        fn_free_handle(SQL_HANDLE_ENV, env);
        return 1;
    }

    /* Set and get isolation level (stored locally before connect) */
    result = fn_set_connect_attr(conn, SQL_ATTR_TXN_ISOLATION,
                                 (SQLPOINTER)(uintptr_t)SQL_TXN_SERIALIZABLE, 0);
    if (result != SQL_SUCCESS) {
        fprintf(stderr, "    FAIL: set isolation level\n");
        fn_free_handle(SQL_HANDLE_DBC, conn);
        fn_free_handle(SQL_HANDLE_ENV, env);
        return 1;
    }
    attr_value = 0;
    result = fn_get_connect_attr(conn, SQL_ATTR_TXN_ISOLATION, &attr_value, 0, NULL);
    if (result != SQL_SUCCESS || attr_value != SQL_TXN_SERIALIZABLE) {
        fprintf(stderr, "    FAIL: isolation should be SERIALIZABLE, got %u\n", (unsigned)attr_value);
        fn_free_handle(SQL_HANDLE_DBC, conn);
        fn_free_handle(SQL_HANDLE_ENV, env);
        return 1;
    }

    fn_free_handle(SQL_HANDLE_DBC, conn);
    fn_free_handle(SQL_HANDLE_ENV, env);

    tests_passed++;
    return 0;
}

static int test_invalid_attribute_returns_error(void)
{
    printf("  test_invalid_attribute_returns_error...\n");
    tests_run++;

    SQLHANDLE env, conn;
    SQLRETURN result;

    result = fn_alloc_handle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env);
    if (result != SQL_SUCCESS) { fprintf(stderr, "    FAIL: alloc env\n"); return 1; }
    result = fn_alloc_handle(SQL_HANDLE_DBC, env, &conn);
    if (result != SQL_SUCCESS) { fprintf(stderr, "    FAIL: alloc conn\n"); return 1; }

    /* Setting a bogus attribute should return SQL_ERROR */
    result = fn_set_connect_attr(conn, 99999, (SQLPOINTER)(uintptr_t)1, 0);
    if (result != SQL_ERROR) {
        fprintf(stderr, "    FAIL: invalid attr set should return SQL_ERROR, got %d\n", (int)result);
        fn_free_handle(SQL_HANDLE_DBC, conn);
        fn_free_handle(SQL_HANDLE_ENV, env);
        return 1;
    }

    /* Verify SQLSTATE HY092 */
    SQLCHAR sqlstate[6] = {0};
    SQLINTEGER native_error;
    SQLCHAR message[256];
    SQLSMALLINT text_length;
    fn_get_diag_rec(SQL_HANDLE_DBC, conn, 1,
                    sqlstate, &native_error, message, sizeof(message), &text_length);
    if (strcmp((const char *)sqlstate, "HY092") != 0) {
        fprintf(stderr, "    FAIL: expected SQLSTATE HY092, got %s\n", sqlstate);
        fn_free_handle(SQL_HANDLE_DBC, conn);
        fn_free_handle(SQL_HANDLE_ENV, env);
        return 1;
    }

    /* Getting a bogus attribute should also return SQL_ERROR */
    SQLUINTEGER dummy = 0;
    result = fn_get_connect_attr(conn, 99999, &dummy, 0, NULL);
    if (result != SQL_ERROR) {
        fprintf(stderr, "    FAIL: invalid attr get should return SQL_ERROR, got %d\n", (int)result);
        fn_free_handle(SQL_HANDLE_DBC, conn);
        fn_free_handle(SQL_HANDLE_ENV, env);
        return 1;
    }

    fn_free_handle(SQL_HANDLE_DBC, conn);
    fn_free_handle(SQL_HANDLE_ENV, env);

    tests_passed++;
    return 0;
}

/* ---- DB-Required Tests ---- */

static int test_autocommit_roundtrip_connected(const char *connection_string)
{
    printf("  test_autocommit_roundtrip_connected...\n");
    tests_run++;

    SQLHANDLE env, conn;
    if (setup_connected(connection_string, &env, &conn) != 0) {
        fprintf(stderr, "    SKIP: could not connect\n");
        return 0;
    }

    /* Set autocommit OFF */
    SQLRETURN result = fn_set_connect_attr(conn, SQL_ATTR_AUTOCOMMIT,
                                           (SQLPOINTER)(uintptr_t)SQL_AUTOCOMMIT_OFF, 0);
    if (result != SQL_SUCCESS) {
        fprintf(stderr, "    FAIL: set autocommit OFF\n");
        teardown_connected(env, conn);
        return 1;
    }

    /* Verify it's OFF */
    SQLUINTEGER attr_value = 99;
    result = fn_get_connect_attr(conn, SQL_ATTR_AUTOCOMMIT, &attr_value, 0, NULL);
    if (result != SQL_SUCCESS || attr_value != SQL_AUTOCOMMIT_OFF) {
        fprintf(stderr, "    FAIL: autocommit should be OFF\n");
        teardown_connected(env, conn);
        return 1;
    }

    /* Set back to ON */
    result = fn_set_connect_attr(conn, SQL_ATTR_AUTOCOMMIT,
                                 (SQLPOINTER)(uintptr_t)SQL_AUTOCOMMIT_ON, 0);
    if (result != SQL_SUCCESS) {
        fprintf(stderr, "    FAIL: set autocommit ON\n");
        teardown_connected(env, conn);
        return 1;
    }

    attr_value = 0;
    result = fn_get_connect_attr(conn, SQL_ATTR_AUTOCOMMIT, &attr_value, 0, NULL);
    if (result != SQL_SUCCESS || attr_value != SQL_AUTOCOMMIT_ON) {
        fprintf(stderr, "    FAIL: autocommit should be ON\n");
        teardown_connected(env, conn);
        return 1;
    }

    teardown_connected(env, conn);
    tests_passed++;
    return 0;
}

static int test_rollback_discards_insert(const char *connection_string)
{
    printf("  test_rollback_discards_insert...\n");
    tests_run++;

    SQLHANDLE env, conn;
    if (setup_connected(connection_string, &env, &conn) != 0) {
        fprintf(stderr, "    SKIP: could not connect\n");
        return 0;
    }

    SQLHANDLE stmt;
    SQLRETURN result;

    result = fn_alloc_handle(SQL_HANDLE_STMT, conn, &stmt);
    if (result != SQL_SUCCESS) {
        fprintf(stderr, "    FAIL: alloc stmt\n");
        teardown_connected(env, conn);
        return 1;
    }

    /* Create a temp table */
    result = fn_exec_direct(stmt, (SQLCHAR *)"CREATE TEMP TABLE test_rollback(id int)", SQL_NTS);
    if (result != SQL_SUCCESS) {
        fprintf(stderr, "    FAIL: create temp table\n");
        fn_free_stmt(stmt, SQL_DROP);
        teardown_connected(env, conn);
        return 1;
    }
    fn_free_stmt(stmt, SQL_CLOSE);

    /* Turn off autocommit */
    result = fn_set_connect_attr(conn, SQL_ATTR_AUTOCOMMIT,
                                 (SQLPOINTER)(uintptr_t)SQL_AUTOCOMMIT_OFF, 0);
    if (result != SQL_SUCCESS) {
        fprintf(stderr, "    FAIL: set autocommit OFF\n");
        fn_free_stmt(stmt, SQL_DROP);
        teardown_connected(env, conn);
        return 1;
    }

    /* Insert a row (should be inside an implicit transaction) */
    result = fn_exec_direct(stmt, (SQLCHAR *)"INSERT INTO test_rollback VALUES (42)", SQL_NTS);
    if (result != SQL_SUCCESS) {
        fprintf(stderr, "    FAIL: insert\n");
        fn_free_stmt(stmt, SQL_DROP);
        teardown_connected(env, conn);
        return 1;
    }
    fn_free_stmt(stmt, SQL_CLOSE);

    /* Rollback the transaction */
    result = fn_end_tran(SQL_HANDLE_DBC, conn, SQL_ROLLBACK);
    if (result != SQL_SUCCESS) {
        fprintf(stderr, "    FAIL: rollback returned %d\n", (int)result);
        fn_free_stmt(stmt, SQL_DROP);
        teardown_connected(env, conn);
        return 1;
    }

    /* Verify the row is NOT present */
    result = fn_exec_direct(stmt, (SQLCHAR *)"SELECT COUNT(*) FROM test_rollback WHERE id = 42", SQL_NTS);
    if (result != SQL_SUCCESS) {
        fprintf(stderr, "    FAIL: select count after rollback\n");
        fn_free_stmt(stmt, SQL_DROP);
        teardown_connected(env, conn);
        return 1;
    }

    result = fn_fetch(stmt);
    if (result != SQL_SUCCESS) {
        fprintf(stderr, "    FAIL: fetch\n");
        fn_free_stmt(stmt, SQL_DROP);
        teardown_connected(env, conn);
        return 1;
    }

    char count_str[32] = {0};
    SQLLEN indicator = 0;
    result = fn_get_data(stmt, 1, SQL_C_CHAR, count_str, sizeof(count_str), &indicator);
    if (result != SQL_SUCCESS) {
        fprintf(stderr, "    FAIL: get data\n");
        fn_free_stmt(stmt, SQL_DROP);
        teardown_connected(env, conn);
        return 1;
    }

    if (strcmp(count_str, "0") != 0) {
        fprintf(stderr, "    FAIL: expected 0 rows after rollback, got '%s'\n", count_str);
        fn_free_stmt(stmt, SQL_DROP);
        teardown_connected(env, conn);
        return 1;
    }

    fn_free_stmt(stmt, SQL_DROP);
    teardown_connected(env, conn);
    tests_passed++;
    return 0;
}

static int test_commit_persists_insert(const char *connection_string)
{
    printf("  test_commit_persists_insert...\n");
    tests_run++;

    SQLHANDLE env, conn;
    if (setup_connected(connection_string, &env, &conn) != 0) {
        fprintf(stderr, "    SKIP: could not connect\n");
        return 0;
    }

    SQLHANDLE stmt;
    SQLRETURN result;

    result = fn_alloc_handle(SQL_HANDLE_STMT, conn, &stmt);
    if (result != SQL_SUCCESS) {
        fprintf(stderr, "    FAIL: alloc stmt\n");
        teardown_connected(env, conn);
        return 1;
    }

    /* Create a temp table */
    result = fn_exec_direct(stmt, (SQLCHAR *)"CREATE TEMP TABLE test_commit(id int)", SQL_NTS);
    if (result != SQL_SUCCESS) {
        fprintf(stderr, "    FAIL: create temp table\n");
        fn_free_stmt(stmt, SQL_DROP);
        teardown_connected(env, conn);
        return 1;
    }
    fn_free_stmt(stmt, SQL_CLOSE);

    /* Turn off autocommit */
    result = fn_set_connect_attr(conn, SQL_ATTR_AUTOCOMMIT,
                                 (SQLPOINTER)(uintptr_t)SQL_AUTOCOMMIT_OFF, 0);
    if (result != SQL_SUCCESS) {
        fprintf(stderr, "    FAIL: set autocommit OFF\n");
        fn_free_stmt(stmt, SQL_DROP);
        teardown_connected(env, conn);
        return 1;
    }

    /* Insert a row */
    result = fn_exec_direct(stmt, (SQLCHAR *)"INSERT INTO test_commit VALUES (99)", SQL_NTS);
    if (result != SQL_SUCCESS) {
        fprintf(stderr, "    FAIL: insert\n");
        fn_free_stmt(stmt, SQL_DROP);
        teardown_connected(env, conn);
        return 1;
    }
    fn_free_stmt(stmt, SQL_CLOSE);

    /* Commit the transaction */
    result = fn_end_tran(SQL_HANDLE_DBC, conn, SQL_COMMIT);
    if (result != SQL_SUCCESS) {
        fprintf(stderr, "    FAIL: commit returned %d\n", (int)result);
        fn_free_stmt(stmt, SQL_DROP);
        teardown_connected(env, conn);
        return 1;
    }

    /* Verify the row IS present */
    result = fn_exec_direct(stmt, (SQLCHAR *)"SELECT COUNT(*) FROM test_commit WHERE id = 99", SQL_NTS);
    if (result != SQL_SUCCESS) {
        fprintf(stderr, "    FAIL: select count after commit\n");
        fn_free_stmt(stmt, SQL_DROP);
        teardown_connected(env, conn);
        return 1;
    }

    result = fn_fetch(stmt);
    if (result != SQL_SUCCESS) {
        fprintf(stderr, "    FAIL: fetch\n");
        fn_free_stmt(stmt, SQL_DROP);
        teardown_connected(env, conn);
        return 1;
    }

    char count_str[32] = {0};
    SQLLEN indicator = 0;
    result = fn_get_data(stmt, 1, SQL_C_CHAR, count_str, sizeof(count_str), &indicator);
    if (result != SQL_SUCCESS) {
        fprintf(stderr, "    FAIL: get data\n");
        fn_free_stmt(stmt, SQL_DROP);
        teardown_connected(env, conn);
        return 1;
    }

    if (strcmp(count_str, "1") != 0) {
        fprintf(stderr, "    FAIL: expected 1 row after commit, got '%s'\n", count_str);
        fn_free_stmt(stmt, SQL_DROP);
        teardown_connected(env, conn);
        return 1;
    }

    fn_free_stmt(stmt, SQL_DROP);
    teardown_connected(env, conn);
    tests_passed++;
    return 0;
}

static int test_connection_dead_attribute(const char *connection_string)
{
    printf("  test_connection_dead_attribute...\n");
    tests_run++;

    SQLHANDLE env, conn;
    if (setup_connected(connection_string, &env, &conn) != 0) {
        fprintf(stderr, "    SKIP: could not connect\n");
        return 0;
    }

    /* A live connection should report SQL_CD_FALSE */
    SQLUINTEGER dead_flag = 99;
    SQLRETURN result = fn_get_connect_attr(conn, SQL_ATTR_CONNECTION_DEAD, &dead_flag, 0, NULL);
    if (result != SQL_SUCCESS) {
        fprintf(stderr, "    FAIL: get CONNECTION_DEAD returned %d\n", (int)result);
        teardown_connected(env, conn);
        return 1;
    }

    if (dead_flag != SQL_CD_FALSE) {
        fprintf(stderr, "    FAIL: live connection should report SQL_CD_FALSE, got %u\n",
                (unsigned)dead_flag);
        teardown_connected(env, conn);
        return 1;
    }

    teardown_connected(env, conn);
    tests_passed++;
    return 0;
}

static int test_isolation_level_roundtrip(const char *connection_string)
{
    printf("  test_isolation_level_roundtrip...\n");
    tests_run++;

    SQLHANDLE env, conn;
    if (setup_connected(connection_string, &env, &conn) != 0) {
        fprintf(stderr, "    SKIP: could not connect\n");
        return 0;
    }

    /* Set to SERIALIZABLE */
    SQLRETURN result = fn_set_connect_attr(conn, SQL_ATTR_TXN_ISOLATION,
                                           (SQLPOINTER)(uintptr_t)SQL_TXN_SERIALIZABLE, 0);
    if (result != SQL_SUCCESS) {
        fprintf(stderr, "    FAIL: set isolation SERIALIZABLE returned %d\n", (int)result);
        teardown_connected(env, conn);
        return 1;
    }

    SQLUINTEGER iso_value = 0;
    result = fn_get_connect_attr(conn, SQL_ATTR_TXN_ISOLATION, &iso_value, 0, NULL);
    if (result != SQL_SUCCESS || iso_value != SQL_TXN_SERIALIZABLE) {
        fprintf(stderr, "    FAIL: isolation should be SERIALIZABLE, got %u\n", (unsigned)iso_value);
        teardown_connected(env, conn);
        return 1;
    }

    /* Set to REPEATABLE READ */
    result = fn_set_connect_attr(conn, SQL_ATTR_TXN_ISOLATION,
                                 (SQLPOINTER)(uintptr_t)SQL_TXN_REPEATABLE_READ, 0);
    if (result != SQL_SUCCESS) {
        fprintf(stderr, "    FAIL: set isolation REPEATABLE_READ returned %d\n", (int)result);
        teardown_connected(env, conn);
        return 1;
    }

    iso_value = 0;
    result = fn_get_connect_attr(conn, SQL_ATTR_TXN_ISOLATION, &iso_value, 0, NULL);
    if (result != SQL_SUCCESS || iso_value != SQL_TXN_REPEATABLE_READ) {
        fprintf(stderr, "    FAIL: isolation should be REPEATABLE_READ, got %u\n", (unsigned)iso_value);
        teardown_connected(env, conn);
        return 1;
    }

    /* Set READ UNCOMMITTED — should return SQL_SUCCESS_WITH_INFO and store READ_COMMITTED */
    result = fn_set_connect_attr(conn, SQL_ATTR_TXN_ISOLATION,
                                 (SQLPOINTER)(uintptr_t)SQL_TXN_READ_UNCOMMITTED, 0);
    if (result != SQL_SUCCESS_WITH_INFO) {
        fprintf(stderr, "    FAIL: READ_UNCOMMITTED should return SQL_SUCCESS_WITH_INFO, got %d\n",
                (int)result);
        teardown_connected(env, conn);
        return 1;
    }

    iso_value = 0;
    result = fn_get_connect_attr(conn, SQL_ATTR_TXN_ISOLATION, &iso_value, 0, NULL);
    if (result != SQL_SUCCESS || iso_value != SQL_TXN_READ_COMMITTED) {
        fprintf(stderr, "    FAIL: after READ_UNCOMMITTED, stored value should be READ_COMMITTED, got %u\n",
                (unsigned)iso_value);
        teardown_connected(env, conn);
        return 1;
    }

    teardown_connected(env, conn);
    tests_passed++;
    return 0;
}

int main(void)
{
    int failures = 0;

    printf("test_connection_attributes:\n");

    load_driver();

    /* Tests that do NOT require a database */
    printf("\n  -- No-DB tests --\n");
    failures += test_set_get_attributes_before_connect();
    failures += test_invalid_attribute_returns_error();

    /* Tests that require a live database */
    const char *connection_string = getenv("PSQLODBC2_TEST_CONNSTR");
    if (connection_string && connection_string[0] != '\0') {
        printf("\n  -- DB-required tests --\n");
        failures += test_autocommit_roundtrip_connected(connection_string);
        failures += test_rollback_discards_insert(connection_string);
        failures += test_commit_persists_insert(connection_string);
        failures += test_connection_dead_attribute(connection_string);
        failures += test_isolation_level_roundtrip(connection_string);
    } else {
        printf("\n  SKIP: PSQLODBC2_TEST_CONNSTR not set; DB tests skipped.\n");
    }

    CLOSE_LIBRARY(driver_library);

    printf("\nResults: %d/%d tests passed.\n", tests_passed, tests_run);

    if (failures > 0) {
        fprintf(stderr, "Some tests FAILED.\n");
        return 1;
    }

    printf("All connection attribute tests passed.\n");
    return 0;
}
