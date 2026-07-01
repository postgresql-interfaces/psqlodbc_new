/*-------------------------------------------------------------------------
 *
 * test_dsn_config.c
 *	  Unit tests for DSN config reading
 *
 * Copyright (c) 2026, Dave Cramer
 *
 *
 * IDENTIFICATION
 *	  tests/test_dsn_config.c
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dsn_config.h"

/* Exit code 77 tells Meson the test was skipped (not failed) */
#define EXIT_SKIP 77

#define TEST_PASS 0
#define TEST_FAIL 1

static int tests_run = 0;
static int tests_passed = 0;

#define ASSERT_TRUE(condition, message) do { \
    tests_run++; \
    if (!(condition)) { \
        fprintf(stderr, "  FAIL: %s (line %d)\n", message, __LINE__); \
        return TEST_FAIL; \
    } \
    tests_passed++; \
} while (0)

#define ASSERT_FALSE(condition, message) do { \
    tests_run++; \
    if ((condition)) { \
        fprintf(stderr, "  FAIL: %s (line %d)\n", message, __LINE__); \
        return TEST_FAIL; \
    } \
    tests_passed++; \
} while (0)

#define ASSERT_STREQ(actual, expected, message) do { \
    tests_run++; \
    if (strcmp((actual), (expected)) != 0) { \
        fprintf(stderr, "  FAIL: %s — expected \"%s\", got \"%s\" (line %d)\n", \
                message, expected, actual, __LINE__); \
        return TEST_FAIL; \
    } \
    tests_passed++; \
} while (0)

#define ASSERT_INT_EQ(actual, expected, message) do { \
    tests_run++; \
    if ((actual) != (expected)) { \
        fprintf(stderr, "  FAIL: %s — expected %d, got %d (line %d)\n", \
                message, (int)(expected), (int)(actual), __LINE__); \
        return TEST_FAIL; \
    } \
    tests_passed++; \
} while (0)

/* Path for the temporary odbc.ini used by these tests */
static const char *TEMP_ODBC_INI = "/tmp/psqlodbc2_test_odbc.ini";

/*
 * Write a test odbc.ini file with known DSN entries.
 * Returns true on success.
 */
static bool create_test_ini_file(void)
{
    FILE *ini_file = fopen(TEMP_ODBC_INI, "w");
    if (!ini_file) {
        fprintf(stderr, "Failed to create temp odbc.ini at %s\n", TEMP_ODBC_INI);
        return false;
    }

    /* A fully-populated DSN for testing all fields */
    fprintf(ini_file, "[testdsn]\n");
    fprintf(ini_file, "Servername=localhost\n");
    fprintf(ini_file, "Port=5432\n");
    fprintf(ini_file, "Database=testdb\n");
    fprintf(ini_file, "Username=testuser\n");
    fprintf(ini_file, "Password=testpass\n");
    fprintf(ini_file, "SSLmode=prefer\n");
    fprintf(ini_file, "ApplicationName=myapp\n");
    fprintf(ini_file, "Timeout=30\n");
    fprintf(ini_file, "\n");

    /* A DSN with only some fields (tests partial population) */
    fprintf(ini_file, "[partialdsn]\n");
    fprintf(ini_file, "Server=dbhost.example.com\n");
    fprintf(ini_file, "Database=proddb\n");
    fprintf(ini_file, "\n");

    /* A DSN that uses the UID alias instead of Username */
    fprintf(ini_file, "[uiddsn]\n");
    fprintf(ini_file, "Server=uidhost\n");
    fprintf(ini_file, "UID=uiduser\n");
    fprintf(ini_file, "\n");

    fclose(ini_file);
    return true;
}

static void cleanup_test_ini_file(void)
{
    remove(TEMP_ODBC_INI);
}

static int test_read_full_dsn(void)
{
    printf("  test_read_full_dsn...\n");

    ConnectionInfo info;
    memset(&info, 0, sizeof(info));

    bool result = dsn_config_read("testdsn", &info);
    ASSERT_TRUE(result, "dsn_config_read should return true for existing DSN");

    ASSERT_STREQ(info.server, "localhost", "Server should be 'localhost'");
    ASSERT_STREQ(info.port, "5432", "Port should be '5432'");
    ASSERT_STREQ(info.database, "testdb", "Database should be 'testdb'");
    ASSERT_STREQ(info.username, "testuser", "Username should be 'testuser'");
    ASSERT_TRUE(info.password != NULL, "Password should be non-NULL");
    ASSERT_STREQ(info.password, "testpass", "Password should be 'testpass'");
    ASSERT_STREQ(info.sslmode, "prefer", "SSLmode should be 'prefer'");
    ASSERT_STREQ(info.application_name, "myapp", "ApplicationName should be 'myapp'");
    ASSERT_INT_EQ(info.connect_timeout, 30, "Timeout should be 30");

    free(info.password);
    return TEST_PASS;
}

static int test_nonexistent_dsn(void)
{
    printf("  test_nonexistent_dsn...\n");

    ConnectionInfo info;
    memset(&info, 0, sizeof(info));

    bool result = dsn_config_read("nosuchdsn", &info);
    ASSERT_FALSE(result, "dsn_config_read should return false for non-existent DSN");

    /* Verify nothing was written */
    ASSERT_STREQ(info.server, "", "Server should remain empty");
    ASSERT_STREQ(info.port, "", "Port should remain empty");
    ASSERT_STREQ(info.database, "", "Database should remain empty");

    return TEST_PASS;
}

static int test_partial_dsn(void)
{
    printf("  test_partial_dsn...\n");

    ConnectionInfo info;
    memset(&info, 0, sizeof(info));

    bool result = dsn_config_read("partialdsn", &info);
    ASSERT_TRUE(result, "dsn_config_read should return true for partial DSN");

    ASSERT_STREQ(info.server, "dbhost.example.com", "Server should be 'dbhost.example.com'");
    ASSERT_STREQ(info.database, "proddb", "Database should be 'proddb'");

    /* Fields not in the DSN should remain empty */
    ASSERT_STREQ(info.port, "", "Port should remain empty");
    ASSERT_STREQ(info.username, "", "Username should remain empty");
    ASSERT_TRUE(info.password == NULL, "Password should remain NULL");

    return TEST_PASS;
}

static int test_uid_alias(void)
{
    printf("  test_uid_alias...\n");

    ConnectionInfo info;
    memset(&info, 0, sizeof(info));

    bool result = dsn_config_read("uiddsn", &info);
    ASSERT_TRUE(result, "dsn_config_read should return true for UID alias DSN");

    ASSERT_STREQ(info.server, "uidhost", "Server should be 'uidhost'");
    ASSERT_STREQ(info.username, "uiduser", "Username from UID key should be 'uiduser'");

    return TEST_PASS;
}

static int test_existing_values_not_overwritten_by_empty(void)
{
    printf("  test_existing_values_not_overwritten_by_empty...\n");

    /* Pre-populate the info struct with values. When we read a DSN that
     * doesn't have those keys, the existing values should be preserved. */
    ConnectionInfo info;
    memset(&info, 0, sizeof(info));
    strncpy(info.port, "9999", sizeof(info.port) - 1);
    strncpy(info.username, "existinguser", sizeof(info.username) - 1);

    /* partialdsn only has Server and Database — it should NOT overwrite
     * port or username since those keys are empty in the DSN. */
    bool result = dsn_config_read("partialdsn", &info);
    ASSERT_TRUE(result, "dsn_config_read should return true");

    ASSERT_STREQ(info.server, "dbhost.example.com", "Server should be updated from DSN");
    ASSERT_STREQ(info.database, "proddb", "Database should be updated from DSN");
    ASSERT_STREQ(info.port, "9999", "Port should remain as pre-populated value");
    ASSERT_STREQ(info.username, "existinguser", "Username should remain as pre-populated value");

    return TEST_PASS;
}

static int test_null_and_empty_dsn_name(void)
{
    printf("  test_null_and_empty_dsn_name...\n");

    ConnectionInfo info;
    memset(&info, 0, sizeof(info));

    ASSERT_FALSE(dsn_config_read(NULL, &info), "NULL dsn_name should return false");
    ASSERT_FALSE(dsn_config_read("", &info), "Empty dsn_name should return false");
    ASSERT_FALSE(dsn_config_read("testdsn", NULL), "NULL out_info should return false");

    return TEST_PASS;
}

int main(void)
{
#ifndef HAVE_ODBCINST
    /* libodbcinst is not available — skip the test entirely */
    printf("SKIP: libodbcinst not available (HAVE_ODBCINST not defined)\n");
    return EXIT_SKIP;
#else
    printf("Running DSN config tests...\n");

    /* Set up: create temp odbc.ini and point ODBCINI to it */
    if (!create_test_ini_file()) {
        fprintf(stderr, "Failed to create test INI file\n");
        return TEST_FAIL;
    }

    /* ODBCINI environment variable tells unixODBC where to find the user's
     * odbc.ini file. We override it to use our test file. */
    setenv("ODBCINI", TEMP_ODBC_INI, 1);

    int result = TEST_PASS;

    if (test_read_full_dsn() != TEST_PASS) result = TEST_FAIL;
    if (test_nonexistent_dsn() != TEST_PASS) result = TEST_FAIL;
    if (test_partial_dsn() != TEST_PASS) result = TEST_FAIL;
    if (test_uid_alias() != TEST_PASS) result = TEST_FAIL;
    if (test_existing_values_not_overwritten_by_empty() != TEST_PASS) result = TEST_FAIL;
    if (test_null_and_empty_dsn_name() != TEST_PASS) result = TEST_FAIL;

    /* Clean up */
    cleanup_test_ini_file();
    unsetenv("ODBCINI");

    printf("\n  Results: %d/%d tests passed\n", tests_passed, tests_run);
    return result;
#endif
}
