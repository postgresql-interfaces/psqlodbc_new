/*-------------------------------------------------------------------------
 *
 * test_connection_lifecycle.c
 *	  Tests for connection handle lifecycle
 *
 * Copyright (c) 2026, Dave Cramer
 *
 *
 * IDENTIFICATION
 *	  tests/test_connection_lifecycle.c
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include <sql.h>
#include <sqltypes.h>
#include <sqlext.h>

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

/* Function pointer types matching the ODBC signatures */
typedef SQLRETURN (SQL_API *AllocHandleFunc)(SQLSMALLINT, SQLHANDLE, SQLHANDLE *);
typedef SQLRETURN (SQL_API *FreeHandleFunc)(SQLSMALLINT, SQLHANDLE);
typedef SQLRETURN (SQL_API *DisconnectFunc)(SQLHDBC);
typedef SQLRETURN (SQL_API *GetDiagRecFunc)(SQLSMALLINT, SQLHANDLE, SQLSMALLINT,
                                            SQLCHAR *, SQLINTEGER *, SQLCHAR *,
                                            SQLSMALLINT, SQLSMALLINT *);

#define TEST_PASS 0
#define TEST_FAIL 1

static LIB_HANDLE library = NULL;
static AllocHandleFunc alloc_handle = NULL;
static FreeHandleFunc free_handle = NULL;
static DisconnectFunc disconnect_func = NULL;
static GetDiagRecFunc get_diag_rec = NULL;

static int tests_run = 0;
static int tests_passed = 0;

static int load_driver(void)
{
    library = LOAD_LIBRARY(DRIVER_LIBRARY_PATH);
    if (!library) {
        fprintf(stderr, "FAIL: Could not load driver library '%s': %s\n",
                DRIVER_LIBRARY_PATH, LIB_ERROR_MSG());
        return TEST_FAIL;
    }

    alloc_handle = (AllocHandleFunc)GET_SYMBOL(library, "SQLAllocHandle");
    free_handle = (FreeHandleFunc)GET_SYMBOL(library, "SQLFreeHandle");
    disconnect_func = (DisconnectFunc)GET_SYMBOL(library, "SQLDisconnect");
    get_diag_rec = (GetDiagRecFunc)GET_SYMBOL(library, "SQLGetDiagRec");

    if (!alloc_handle || !free_handle || !disconnect_func || !get_diag_rec) {
        fprintf(stderr, "FAIL: Could not resolve required symbols: %s\n", LIB_ERROR_MSG());
        CLOSE_LIBRARY(library);
        return TEST_FAIL;
    }

    return TEST_PASS;
}

static int test_alloc_and_free_connection(void)
{
    printf("  test_alloc_and_free_connection...\n");
    tests_run++;

    SQLHANDLE env_handle = SQL_NULL_HENV;
    SQLHANDLE conn_handle = SQL_NULL_HDBC;
    SQLRETURN result;

    /* Allocate environment */
    result = alloc_handle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env_handle);
    if (result != SQL_SUCCESS) {
        fprintf(stderr, "    FAIL: SQLAllocHandle(ENV) returned %d\n", (int)result);
        return TEST_FAIL;
    }

    /* Allocate connection from environment */
    result = alloc_handle(SQL_HANDLE_DBC, env_handle, &conn_handle);
    if (result != SQL_SUCCESS) {
        fprintf(stderr, "    FAIL: SQLAllocHandle(DBC) returned %d, expected SQL_SUCCESS\n", (int)result);
        free_handle(SQL_HANDLE_ENV, env_handle);
        return TEST_FAIL;
    }

    if (conn_handle == SQL_NULL_HDBC) {
        fprintf(stderr, "    FAIL: connection handle is NULL after successful alloc\n");
        free_handle(SQL_HANDLE_ENV, env_handle);
        return TEST_FAIL;
    }

    /* Free connection */
    result = free_handle(SQL_HANDLE_DBC, conn_handle);
    if (result != SQL_SUCCESS) {
        fprintf(stderr, "    FAIL: SQLFreeHandle(DBC) returned %d, expected SQL_SUCCESS\n", (int)result);
        free_handle(SQL_HANDLE_ENV, env_handle);
        return TEST_FAIL;
    }

    /* Free environment */
    result = free_handle(SQL_HANDLE_ENV, env_handle);
    if (result != SQL_SUCCESS) {
        fprintf(stderr, "    FAIL: SQLFreeHandle(ENV) returned %d, expected SQL_SUCCESS\n", (int)result);
        return TEST_FAIL;
    }

    tests_passed++;
    return TEST_PASS;
}

static int test_env_free_blocked_by_connection(void)
{
    printf("  test_env_free_blocked_by_connection...\n");
    tests_run++;

    SQLHANDLE env_handle = SQL_NULL_HENV;
    SQLHANDLE conn_handle = SQL_NULL_HDBC;
    SQLRETURN result;

    /* Allocate environment and connection */
    alloc_handle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env_handle);
    alloc_handle(SQL_HANDLE_DBC, env_handle, &conn_handle);

    /* Attempt to free environment while connection exists — should fail */
    result = free_handle(SQL_HANDLE_ENV, env_handle);
    if (result != SQL_ERROR) {
        fprintf(stderr, "    FAIL: SQLFreeHandle(ENV) returned %d, expected SQL_ERROR (%d)\n",
                (int)result, (int)SQL_ERROR);
        /* Clean up anyway */
        free_handle(SQL_HANDLE_DBC, conn_handle);
        free_handle(SQL_HANDLE_ENV, env_handle);
        return TEST_FAIL;
    }

    /* Verify a diagnostic record was set on the environment */
    SQLCHAR sql_state[6];
    SQLINTEGER native_error = 0;
    SQLCHAR message[256];
    SQLSMALLINT message_length = 0;

    result = get_diag_rec(SQL_HANDLE_ENV, env_handle, 1,
                          sql_state, &native_error, message, sizeof(message), &message_length);
    if (result != SQL_SUCCESS) {
        fprintf(stderr, "    FAIL: SQLGetDiagRec returned %d after blocked free, expected SQL_SUCCESS\n",
                (int)result);
        free_handle(SQL_HANDLE_DBC, conn_handle);
        free_handle(SQL_HANDLE_ENV, env_handle);
        return TEST_FAIL;
    }

    /* SQLSTATE should be HY010 (function sequence error) */
    if (strcmp((const char *)sql_state, "HY010") != 0) {
        fprintf(stderr, "    FAIL: Expected SQLSTATE HY010, got %s\n", sql_state);
        free_handle(SQL_HANDLE_DBC, conn_handle);
        free_handle(SQL_HANDLE_ENV, env_handle);
        return TEST_FAIL;
    }

    /* Now free properly: connection first, then environment */
    free_handle(SQL_HANDLE_DBC, conn_handle);
    result = free_handle(SQL_HANDLE_ENV, env_handle);
    if (result != SQL_SUCCESS) {
        fprintf(stderr, "    FAIL: SQLFreeHandle(ENV) returned %d after freeing connection\n", (int)result);
        return TEST_FAIL;
    }

    tests_passed++;
    return TEST_PASS;
}

static int test_disconnect_when_not_connected(void)
{
    printf("  test_disconnect_when_not_connected...\n");
    tests_run++;

    SQLHANDLE env_handle = SQL_NULL_HENV;
    SQLHANDLE conn_handle = SQL_NULL_HDBC;
    SQLRETURN result;

    alloc_handle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &env_handle);
    alloc_handle(SQL_HANDLE_DBC, env_handle, &conn_handle);

    /* Disconnect should fail because we never connected */
    result = disconnect_func((SQLHDBC)conn_handle);
    if (result != SQL_ERROR) {
        fprintf(stderr, "    FAIL: SQLDisconnect returned %d, expected SQL_ERROR when not connected\n",
                (int)result);
        free_handle(SQL_HANDLE_DBC, conn_handle);
        free_handle(SQL_HANDLE_ENV, env_handle);
        return TEST_FAIL;
    }

    /* Clean up */
    free_handle(SQL_HANDLE_DBC, conn_handle);
    free_handle(SQL_HANDLE_ENV, env_handle);

    tests_passed++;
    return TEST_PASS;
}

static int test_alloc_connection_with_invalid_env(void)
{
    printf("  test_alloc_connection_with_invalid_env...\n");
    tests_run++;

    SQLHANDLE conn_handle = SQL_NULL_HDBC;
    SQLRETURN result;

    /* NULL environment should fail */
    result = alloc_handle(SQL_HANDLE_DBC, SQL_NULL_HANDLE, &conn_handle);
    if (result != SQL_INVALID_HANDLE) {
        fprintf(stderr, "    FAIL: SQLAllocHandle(DBC, NULL) returned %d, expected SQL_INVALID_HANDLE\n",
                (int)result);
        return TEST_FAIL;
    }

    tests_passed++;
    return TEST_PASS;
}

int main(void)
{
    int result = TEST_PASS;

    printf("Running connection lifecycle tests...\n");

    if (load_driver() != TEST_PASS) {
        return TEST_FAIL;
    }

    if (test_alloc_and_free_connection() != TEST_PASS) result = TEST_FAIL;
    if (test_env_free_blocked_by_connection() != TEST_PASS) result = TEST_FAIL;
    if (test_disconnect_when_not_connected() != TEST_PASS) result = TEST_FAIL;
    if (test_alloc_connection_with_invalid_env() != TEST_PASS) result = TEST_FAIL;

    CLOSE_LIBRARY(library);

    printf("\nResults: %d/%d tests passed.\n", tests_passed, tests_run);

    if (result == TEST_PASS) {
        printf("All connection lifecycle tests passed.\n");
    } else {
        fprintf(stderr, "Some tests FAILED.\n");
    }

    return result;
}
