/*-------------------------------------------------------------------------
 *
 * test_driver_load.c
 *	  Integration test for driver loading
 *
 * Copyright (c) 2026, Dave Cramer
 *
 *
 * IDENTIFICATION
 *	  tests/test_driver_load.c
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#endif
#include <sql.h>
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

/* Function pointer types matching the ODBC signatures */
typedef SQLRETURN (SQL_API *AllocHandleFunc)(SQLSMALLINT, SQLHANDLE, SQLHANDLE *);
typedef SQLRETURN (SQL_API *FreeHandleFunc)(SQLSMALLINT, SQLHANDLE);

/* Exit codes */
#define TEST_PASS 0
#define TEST_FAIL 1

int main(void)
{
    LIB_HANDLE library = NULL;
    AllocHandleFunc alloc_handle = NULL;
    FreeHandleFunc free_handle = NULL;
    SQLHANDLE environment_handle = SQL_NULL_HENV;
    SQLRETURN result;

    /* Step 1: Load the driver shared library */
    library = LOAD_LIBRARY(DRIVER_LIBRARY_PATH);
    if (!library) {
        fprintf(stderr, "FAIL: Could not load driver library '%s': %s\n",
                DRIVER_LIBRARY_PATH, LIB_ERROR_MSG());
        return TEST_FAIL;
    }
    printf("PASS: Driver library loaded from '%s'\n", DRIVER_LIBRARY_PATH);

    /* Step 2: Resolve SQLAllocHandle */
    alloc_handle = (AllocHandleFunc)GET_SYMBOL(library, "SQLAllocHandle");
    if (!alloc_handle) {
        fprintf(stderr, "FAIL: Could not resolve symbol 'SQLAllocHandle': %s\n",
                LIB_ERROR_MSG());
        CLOSE_LIBRARY(library);
        return TEST_FAIL;
    }
    printf("PASS: SQLAllocHandle symbol resolved\n");

    /* Step 3: Allocate an environment handle */
    result = alloc_handle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &environment_handle);
    if (result != SQL_SUCCESS) {
        fprintf(stderr, "FAIL: SQLAllocHandle(SQL_HANDLE_ENV) returned %d, expected %d (SQL_SUCCESS)\n",
                (int)result, (int)SQL_SUCCESS);
        CLOSE_LIBRARY(library);
        return TEST_FAIL;
    }
    if (environment_handle == SQL_NULL_HENV) {
        fprintf(stderr, "FAIL: SQLAllocHandle returned SQL_SUCCESS but output handle is NULL\n");
        CLOSE_LIBRARY(library);
        return TEST_FAIL;
    }
    printf("PASS: SQLAllocHandle(SQL_HANDLE_ENV) returned SQL_SUCCESS\n");

    /* Step 4: Resolve SQLFreeHandle */
    free_handle = (FreeHandleFunc)GET_SYMBOL(library, "SQLFreeHandle");
    if (!free_handle) {
        fprintf(stderr, "FAIL: Could not resolve symbol 'SQLFreeHandle': %s\n",
                LIB_ERROR_MSG());
        CLOSE_LIBRARY(library);
        return TEST_FAIL;
    }
    printf("PASS: SQLFreeHandle symbol resolved\n");

    /* Step 5: Free the environment handle */
    result = free_handle(SQL_HANDLE_ENV, environment_handle);
    if (result != SQL_SUCCESS) {
        fprintf(stderr, "FAIL: SQLFreeHandle(SQL_HANDLE_ENV) returned %d, expected %d (SQL_SUCCESS)\n",
                (int)result, (int)SQL_SUCCESS);
        CLOSE_LIBRARY(library);
        return TEST_FAIL;
    }
    printf("PASS: SQLFreeHandle(SQL_HANDLE_ENV) returned SQL_SUCCESS\n");

    /* Cleanup */
    CLOSE_LIBRARY(library);
    printf("\nAll tests passed.\n");
    return TEST_PASS;
}
