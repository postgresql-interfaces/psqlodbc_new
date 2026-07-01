/*-------------------------------------------------------------------------
 *
 * test_connection_string.c
 *	  Unit tests for connection string parsing
 *
 * Copyright (c) 2026, Dave Cramer
 *
 *
 * IDENTIFICATION
 *	  tests/test_connection_string.c
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Include the connection string module directly so we can test without
 * going through the full ODBC API layer */
#include "connection_string.h"

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

static int test_standard_parse(void)
{
    printf("  test_standard_parse...\n");

    ConnectionInfo info;
    memset(&info, 0, sizeof(info));

    const char *conn_str = "Server=localhost;Port=5432;Database=testdb;UID=user;PWD=pass";
    ASSERT_TRUE(connection_string_parse(conn_str, SQL_NTS, &info),
                "parse should succeed");

    ASSERT_STREQ(info.server, "localhost", "server should be localhost");
    ASSERT_STREQ(info.port, "5432", "port should be 5432");
    ASSERT_STREQ(info.database, "testdb", "database should be testdb");
    ASSERT_STREQ(info.username, "user", "username should be user");
    ASSERT_TRUE(info.password != NULL, "password should not be NULL");
    ASSERT_STREQ(info.password, "pass", "password should be pass");

    free(info.password);
    return TEST_PASS;
}

static int test_case_insensitive_keys(void)
{
    printf("  test_case_insensitive_keys...\n");

    ConnectionInfo info;
    memset(&info, 0, sizeof(info));

    const char *conn_str = "SERVER=myhost;port=1234;DATABASE=mydb;uid=admin";
    ASSERT_TRUE(connection_string_parse(conn_str, SQL_NTS, &info),
                "parse should succeed");

    ASSERT_STREQ(info.server, "myhost", "server should match (case-insensitive key)");
    ASSERT_STREQ(info.port, "1234", "port should match (lowercase key)");
    ASSERT_STREQ(info.database, "mydb", "database should match (mixed case key)");
    ASSERT_STREQ(info.username, "admin", "username should match (lowercase uid)");

    return TEST_PASS;
}

static int test_brace_enclosed_values(void)
{
    printf("  test_brace_enclosed_values...\n");

    ConnectionInfo info;
    memset(&info, 0, sizeof(info));

    const char *conn_str = "Server=localhost;PWD={pass;word};Database=testdb";
    ASSERT_TRUE(connection_string_parse(conn_str, SQL_NTS, &info),
                "parse should succeed with braces");

    ASSERT_STREQ(info.server, "localhost", "server should be localhost");
    ASSERT_STREQ(info.database, "testdb", "database should be testdb");
    ASSERT_TRUE(info.password != NULL, "password should not be NULL");
    ASSERT_STREQ(info.password, "pass;word", "password should include semicolon");

    free(info.password);
    return TEST_PASS;
}

static int test_empty_and_null_input(void)
{
    printf("  test_empty_and_null_input...\n");

    ConnectionInfo info;
    memset(&info, 0, sizeof(info));

    /* NULL connection string should succeed (nothing to parse) */
    ASSERT_TRUE(connection_string_parse(NULL, SQL_NTS, &info),
                "NULL string should return true");

    /* Empty string should succeed */
    ASSERT_TRUE(connection_string_parse("", SQL_NTS, &info),
                "empty string should return true");

    /* Zero-length should succeed */
    ASSERT_TRUE(connection_string_parse("Server=x", 0, &info),
                "zero-length should return true");
    ASSERT_STREQ(info.server, "", "server should remain empty with zero length");

    return TEST_PASS;
}

static int test_key_aliases(void)
{
    printf("  test_key_aliases...\n");

    ConnectionInfo info;
    memset(&info, 0, sizeof(info));

    const char *conn_str = "Servername=dbhost;DB=mydb;Username=joe;Password=secret;Application_Name=myapp;Connect_Timeout=30";
    ASSERT_TRUE(connection_string_parse(conn_str, SQL_NTS, &info),
                "parse should succeed with aliases");

    ASSERT_STREQ(info.server, "dbhost", "Servername alias should work");
    ASSERT_STREQ(info.database, "mydb", "DB alias should work");
    ASSERT_STREQ(info.username, "joe", "Username alias should work");
    ASSERT_TRUE(info.password != NULL, "password should not be NULL");
    ASSERT_STREQ(info.password, "secret", "Password alias should work");
    ASSERT_STREQ(info.application_name, "myapp", "Application_Name alias should work");
    ASSERT_INT_EQ(info.connect_timeout, 30, "Connect_Timeout should be 30");

    free(info.password);
    return TEST_PASS;
}

static int test_libpq_string_builder(void)
{
    printf("  test_libpq_string_builder...\n");

    ConnectionInfo info;
    memset(&info, 0, sizeof(info));

    strcpy(info.server, "localhost");
    strcpy(info.port, "5432");
    strcpy(info.database, "testdb");
    strcpy(info.username, "user");
    info.password = malloc(5);
    memcpy(info.password, "pass", 5);
    strcpy(info.sslmode, "require");
    info.connect_timeout = 10;

    const char *keywords[LIBPQ_MAX_PARAMS + 1];
    const char *values[LIBPQ_MAX_PARAMS + 1];
    int param_count = 0;

    connection_info_build_libpq_params(&info, keywords, values, &param_count);

    /* All 7 non-empty fields should produce params */
    ASSERT_TRUE(param_count == 7, "should have 7 params");

    /* Verify expected keyword/value pairs are present */
    int found_host = 0, found_port = 0, found_dbname = 0, found_user = 0;
    int found_password = 0, found_sslmode = 0, found_timeout = 0;
    for (int i = 0; i < param_count; i++) {
        if (strcmp(keywords[i], "host") == 0 && strcmp(values[i], "localhost") == 0) found_host = 1;
        if (strcmp(keywords[i], "port") == 0 && strcmp(values[i], "5432") == 0) found_port = 1;
        if (strcmp(keywords[i], "dbname") == 0 && strcmp(values[i], "testdb") == 0) found_dbname = 1;
        if (strcmp(keywords[i], "user") == 0 && strcmp(values[i], "user") == 0) found_user = 1;
        if (strcmp(keywords[i], "password") == 0 && strcmp(values[i], "pass") == 0) found_password = 1;
        if (strcmp(keywords[i], "sslmode") == 0 && strcmp(values[i], "require") == 0) found_sslmode = 1;
        if (strcmp(keywords[i], "connect_timeout") == 0 && strcmp(values[i], "10") == 0) found_timeout = 1;
    }
    ASSERT_TRUE(found_host, "should contain host=localhost");
    ASSERT_TRUE(found_port, "should contain port=5432");
    ASSERT_TRUE(found_dbname, "should contain dbname=testdb");
    ASSERT_TRUE(found_user, "should contain user=user");
    ASSERT_TRUE(found_password, "should contain password=pass");
    ASSERT_TRUE(found_sslmode, "should contain sslmode=require");
    ASSERT_TRUE(found_timeout, "should contain connect_timeout=10");

    /* Verify NULL termination */
    ASSERT_TRUE(keywords[param_count] == NULL, "keywords should be NULL-terminated");
    ASSERT_TRUE(values[param_count] == NULL, "values should be NULL-terminated");

    free(info.password);
    return TEST_PASS;
}

static int test_libpq_string_skips_empty_fields(void)
{
    printf("  test_libpq_string_skips_empty_fields...\n");

    ConnectionInfo info;
    memset(&info, 0, sizeof(info));

    /* Only set server and database */
    strcpy(info.server, "myhost");
    strcpy(info.database, "mydb");

    const char *keywords[LIBPQ_MAX_PARAMS + 1];
    const char *values[LIBPQ_MAX_PARAMS + 1];
    int param_count = 0;

    connection_info_build_libpq_params(&info, keywords, values, &param_count);

    /* Only 2 non-empty fields */
    ASSERT_TRUE(param_count == 2, "should have 2 params");

    /* Verify only the set fields appear */
    int found_host = 0, found_dbname = 0;
    for (int i = 0; i < param_count; i++) {
        if (strcmp(keywords[i], "host") == 0) found_host = 1;
        if (strcmp(keywords[i], "dbname") == 0) found_dbname = 1;
    }
    ASSERT_TRUE(found_host, "should contain host");
    ASSERT_TRUE(found_dbname, "should contain dbname");

    /* Verify empty fields are NOT present */
    for (int i = 0; i < param_count; i++) {
        ASSERT_TRUE(strcmp(keywords[i], "port") != 0, "should not contain port");
        ASSERT_TRUE(strcmp(keywords[i], "user") != 0, "should not contain user");
        ASSERT_TRUE(strcmp(keywords[i], "password") != 0, "should not contain password");
    }

    return TEST_PASS;
}

static int test_explicit_length(void)
{
    printf("  test_explicit_length...\n");

    ConnectionInfo info;
    memset(&info, 0, sizeof(info));

    /* Pass explicit length shorter than actual string — should only parse the prefix.
     * "Server=localhost;Port=9999" is 26 characters. */
    const char *conn_str = "Server=localhost;Port=9999;Database=extra";
    ASSERT_TRUE(connection_string_parse(conn_str, 26, &info),
                "parse with explicit length should succeed");

    ASSERT_STREQ(info.server, "localhost", "server should be parsed");
    ASSERT_STREQ(info.port, "9999", "port should be parsed");
    ASSERT_STREQ(info.database, "", "database should not be parsed (beyond length)");

    return TEST_PASS;
}

int main(void)
{
    int result = TEST_PASS;

    printf("Running connection string tests...\n");

    if (test_standard_parse() != TEST_PASS) result = TEST_FAIL;
    if (test_case_insensitive_keys() != TEST_PASS) result = TEST_FAIL;
    if (test_brace_enclosed_values() != TEST_PASS) result = TEST_FAIL;
    if (test_empty_and_null_input() != TEST_PASS) result = TEST_FAIL;
    if (test_key_aliases() != TEST_PASS) result = TEST_FAIL;
    if (test_libpq_string_builder() != TEST_PASS) result = TEST_FAIL;
    if (test_libpq_string_skips_empty_fields() != TEST_PASS) result = TEST_FAIL;
    if (test_explicit_length() != TEST_PASS) result = TEST_FAIL;

    printf("\nResults: %d/%d tests passed.\n", tests_passed, tests_run);

    if (result == TEST_PASS) {
        printf("All connection string tests passed.\n");
    } else {
        fprintf(stderr, "Some tests FAILED.\n");
    }

    return result;
}
