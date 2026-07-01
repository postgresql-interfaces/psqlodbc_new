/*-------------------------------------------------------------------------
 *
 * test_parameter_binding.c
 *	  Unit tests for parameter binding
 *
 * Copyright (c) 2026, Dave Cramer
 *
 *
 * IDENTIFICATION
 *	  tests/test_parameter_binding.c
 *
 *-------------------------------------------------------------------------
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#include "parameter.h"

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

#define ASSERT_NULL(pointer, message) do { \
    tests_run++; \
    if ((pointer) != NULL) { \
        fprintf(stderr, "  FAIL: %s — expected NULL (line %d)\n", message, __LINE__); \
        return TEST_FAIL; \
    } \
    tests_passed++; \
} while (0)

#define ASSERT_NOT_NULL(pointer, message) do { \
    tests_run++; \
    if ((pointer) == NULL) { \
        fprintf(stderr, "  FAIL: %s — expected non-NULL (line %d)\n", message, __LINE__); \
        return TEST_FAIL; \
    } \
    tests_passed++; \
} while (0)

/* ---- Test Cases ---- */

static int test_bind_single_parameter(void)
{
    printf("  test_bind_single_parameter...\n");

    ParameterBinding bindings[MAX_PARAMETERS];
    int bound_count = 0;
    memset(bindings, 0, sizeof(bindings));

    int32_t value = 42;
    SQLLEN indicator = sizeof(int32_t);

    SQLRETURN result = parameter_bind(bindings, &bound_count,
                                      1,                /* parameter_number */
                                      SQL_PARAM_INPUT,
                                      SQL_C_SLONG,
                                      SQL_INTEGER,
                                      0,                /* column_size */
                                      0,                /* decimal_digits */
                                      &value,
                                      sizeof(value),
                                      &indicator);

    ASSERT_INT_EQ(result, SQL_SUCCESS, "bind should succeed");
    ASSERT_INT_EQ(bound_count, 1, "bound count should be 1");
    ASSERT_TRUE(bindings[0].is_bound, "slot 0 should be bound");
    ASSERT_INT_EQ(bindings[0].parameter_number, 1, "parameter number should be 1");
    ASSERT_INT_EQ(bindings[0].c_type, SQL_C_SLONG, "c_type should be SQL_C_SLONG");
    ASSERT_TRUE(bindings[0].value_buffer == &value, "value_buffer should point to our variable");

    return TEST_PASS;
}

static int test_bind_multiple_parameters(void)
{
    printf("  test_bind_multiple_parameters...\n");

    ParameterBinding bindings[MAX_PARAMETERS];
    int bound_count = 0;
    memset(bindings, 0, sizeof(bindings));

    int32_t int_value = 100;
    char string_value[] = "hello";
    double double_value = 3.14;
    SQLLEN int_indicator = sizeof(int32_t);
    SQLLEN string_indicator = SQL_NTS;
    SQLLEN double_indicator = sizeof(double);

    parameter_bind(bindings, &bound_count, 1, SQL_PARAM_INPUT,
                   SQL_C_SLONG, SQL_INTEGER, 0, 0, &int_value, sizeof(int_value), &int_indicator);
    parameter_bind(bindings, &bound_count, 2, SQL_PARAM_INPUT,
                   SQL_C_CHAR, SQL_VARCHAR, 5, 0, string_value, sizeof(string_value), &string_indicator);
    parameter_bind(bindings, &bound_count, 3, SQL_PARAM_INPUT,
                   SQL_C_DOUBLE, SQL_DOUBLE, 0, 0, &double_value, sizeof(double_value), &double_indicator);

    ASSERT_INT_EQ(bound_count, 3, "bound count should be 3");
    ASSERT_TRUE(bindings[0].is_bound, "slot 0 should be bound");
    ASSERT_TRUE(bindings[1].is_bound, "slot 1 should be bound");
    ASSERT_TRUE(bindings[2].is_bound, "slot 2 should be bound");

    return TEST_PASS;
}

static int test_rebind_same_parameter(void)
{
    printf("  test_rebind_same_parameter...\n");

    ParameterBinding bindings[MAX_PARAMETERS];
    int bound_count = 0;
    memset(bindings, 0, sizeof(bindings));

    int32_t value1 = 10;
    int32_t value2 = 20;
    SQLLEN indicator = sizeof(int32_t);

    parameter_bind(bindings, &bound_count, 1, SQL_PARAM_INPUT,
                   SQL_C_SLONG, SQL_INTEGER, 0, 0, &value1, sizeof(value1), &indicator);
    ASSERT_INT_EQ(bound_count, 1, "count should be 1 after first bind");

    /* Re-bind the same parameter number — should not increment count */
    parameter_bind(bindings, &bound_count, 1, SQL_PARAM_INPUT,
                   SQL_C_SLONG, SQL_INTEGER, 0, 0, &value2, sizeof(value2), &indicator);
    ASSERT_INT_EQ(bound_count, 1, "count should still be 1 after rebind");
    ASSERT_TRUE(bindings[0].value_buffer == &value2, "buffer should point to new value");

    return TEST_PASS;
}

static int test_unbind_all(void)
{
    printf("  test_unbind_all...\n");

    ParameterBinding bindings[MAX_PARAMETERS];
    int bound_count = 0;
    memset(bindings, 0, sizeof(bindings));

    int32_t value = 42;
    SQLLEN indicator = sizeof(int32_t);

    parameter_bind(bindings, &bound_count, 1, SQL_PARAM_INPUT,
                   SQL_C_SLONG, SQL_INTEGER, 0, 0, &value, sizeof(value), &indicator);
    parameter_bind(bindings, &bound_count, 5, SQL_PARAM_INPUT,
                   SQL_C_SLONG, SQL_INTEGER, 0, 0, &value, sizeof(value), &indicator);
    ASSERT_INT_EQ(bound_count, 2, "count should be 2 before unbind");

    parameter_unbind_all(bindings, &bound_count);

    ASSERT_INT_EQ(bound_count, 0, "count should be 0 after unbind");
    ASSERT_TRUE(!bindings[0].is_bound, "slot 0 should not be bound");
    ASSERT_TRUE(!bindings[4].is_bound, "slot 4 should not be bound");

    return TEST_PASS;
}

static int test_invalid_parameter_number(void)
{
    printf("  test_invalid_parameter_number...\n");

    ParameterBinding bindings[MAX_PARAMETERS];
    int bound_count = 0;
    memset(bindings, 0, sizeof(bindings));

    int32_t value = 1;
    SQLLEN indicator = sizeof(int32_t);

    /* Parameter 0 is invalid (1-based) */
    SQLRETURN result = parameter_bind(bindings, &bound_count, 0, SQL_PARAM_INPUT,
                                      SQL_C_SLONG, SQL_INTEGER, 0, 0, &value, sizeof(value), &indicator);
    ASSERT_INT_EQ(result, SQL_ERROR, "parameter 0 should fail");
    ASSERT_INT_EQ(bound_count, 0, "count should remain 0");

    /* Parameter > MAX_PARAMETERS is invalid */
    result = parameter_bind(bindings, &bound_count, MAX_PARAMETERS + 1, SQL_PARAM_INPUT,
                            SQL_C_SLONG, SQL_INTEGER, 0, 0, &value, sizeof(value), &indicator);
    ASSERT_INT_EQ(result, SQL_ERROR, "parameter beyond max should fail");
    ASSERT_INT_EQ(bound_count, 0, "count should remain 0");

    return TEST_PASS;
}

static int test_convert_integer(void)
{
    printf("  test_convert_integer...\n");

    ParameterBinding bindings[MAX_PARAMETERS];
    int bound_count = 0;
    memset(bindings, 0, sizeof(bindings));

    int32_t value = 12345;
    SQLLEN indicator = sizeof(int32_t);

    parameter_bind(bindings, &bound_count, 1, SQL_PARAM_INPUT,
                   SQL_C_SLONG, SQL_INTEGER, 0, 0, &value, sizeof(value), &indicator);

    const char **values = NULL;
    int *lengths = NULL;
    int *formats = NULL;
    int count = 0;

    SQLRETURN result = parameter_build_libpq_arrays(bindings, bound_count,
                                                    &values, &lengths, &formats, &count);

    ASSERT_INT_EQ(result, SQL_SUCCESS, "build arrays should succeed");
    ASSERT_INT_EQ(count, 1, "should have 1 parameter");
    ASSERT_NOT_NULL(values[0], "value should not be NULL");
    ASSERT_STREQ(values[0], "12345", "integer should convert to text");
    ASSERT_INT_EQ(formats[0], 0, "format should be text (0)");

    parameter_free_libpq_arrays(values, lengths, formats, count);
    return TEST_PASS;
}

static int test_convert_negative_integer(void)
{
    printf("  test_convert_negative_integer...\n");

    ParameterBinding bindings[MAX_PARAMETERS];
    int bound_count = 0;
    memset(bindings, 0, sizeof(bindings));

    int32_t value = -9999;
    SQLLEN indicator = sizeof(int32_t);

    parameter_bind(bindings, &bound_count, 1, SQL_PARAM_INPUT,
                   SQL_C_SLONG, SQL_INTEGER, 0, 0, &value, sizeof(value), &indicator);

    const char **values = NULL;
    int *lengths = NULL;
    int *formats = NULL;
    int count = 0;

    parameter_build_libpq_arrays(bindings, bound_count, &values, &lengths, &formats, &count);

    ASSERT_STREQ(values[0], "-9999", "negative integer should convert correctly");

    parameter_free_libpq_arrays(values, lengths, formats, count);
    return TEST_PASS;
}

static int test_convert_string_nts(void)
{
    printf("  test_convert_string_nts...\n");

    ParameterBinding bindings[MAX_PARAMETERS];
    int bound_count = 0;
    memset(bindings, 0, sizeof(bindings));

    char value[] = "hello world";
    SQLLEN indicator = SQL_NTS;

    parameter_bind(bindings, &bound_count, 1, SQL_PARAM_INPUT,
                   SQL_C_CHAR, SQL_VARCHAR, 11, 0, value, sizeof(value), &indicator);

    const char **values = NULL;
    int *lengths = NULL;
    int *formats = NULL;
    int count = 0;

    parameter_build_libpq_arrays(bindings, bound_count, &values, &lengths, &formats, &count);

    ASSERT_STREQ(values[0], "hello world", "SQL_NTS string should pass through");
    ASSERT_INT_EQ(lengths[0], 11, "length should be 11");

    parameter_free_libpq_arrays(values, lengths, formats, count);
    return TEST_PASS;
}

static int test_convert_string_explicit_length(void)
{
    printf("  test_convert_string_explicit_length...\n");

    ParameterBinding bindings[MAX_PARAMETERS];
    int bound_count = 0;
    memset(bindings, 0, sizeof(bindings));

    /* Buffer contains "hello world" but we declare length as 5 (only "hello") */
    char value[] = "hello world";
    SQLLEN indicator = 5;

    parameter_bind(bindings, &bound_count, 1, SQL_PARAM_INPUT,
                   SQL_C_CHAR, SQL_VARCHAR, 5, 0, value, sizeof(value), &indicator);

    const char **values = NULL;
    int *lengths = NULL;
    int *formats = NULL;
    int count = 0;

    parameter_build_libpq_arrays(bindings, bound_count, &values, &lengths, &formats, &count);

    ASSERT_STREQ(values[0], "hello", "explicit length should truncate string");
    ASSERT_INT_EQ(lengths[0], 5, "length should be 5");

    parameter_free_libpq_arrays(values, lengths, formats, count);
    return TEST_PASS;
}

static int test_convert_null_indicator(void)
{
    printf("  test_convert_null_indicator...\n");

    ParameterBinding bindings[MAX_PARAMETERS];
    int bound_count = 0;
    memset(bindings, 0, sizeof(bindings));

    int32_t value = 42;
    SQLLEN indicator = SQL_NULL_DATA;

    parameter_bind(bindings, &bound_count, 1, SQL_PARAM_INPUT,
                   SQL_C_SLONG, SQL_INTEGER, 0, 0, &value, sizeof(value), &indicator);

    const char **values = NULL;
    int *lengths = NULL;
    int *formats = NULL;
    int count = 0;

    parameter_build_libpq_arrays(bindings, bound_count, &values, &lengths, &formats, &count);

    ASSERT_INT_EQ(count, 1, "should have 1 parameter");
    ASSERT_NULL(values[0], "NULL indicator should produce NULL value");

    parameter_free_libpq_arrays(values, lengths, formats, count);
    return TEST_PASS;
}

static int test_convert_null_buffer(void)
{
    printf("  test_convert_null_buffer...\n");

    ParameterBinding bindings[MAX_PARAMETERS];
    int bound_count = 0;
    memset(bindings, 0, sizeof(bindings));

    SQLLEN indicator = SQL_NULL_DATA;

    /* Binding with NULL buffer should also yield NULL */
    parameter_bind(bindings, &bound_count, 1, SQL_PARAM_INPUT,
                   SQL_C_SLONG, SQL_INTEGER, 0, 0, NULL, 0, &indicator);

    const char **values = NULL;
    int *lengths = NULL;
    int *formats = NULL;
    int count = 0;

    parameter_build_libpq_arrays(bindings, bound_count, &values, &lengths, &formats, &count);

    ASSERT_NULL(values[0], "NULL buffer should produce NULL value");

    parameter_free_libpq_arrays(values, lengths, formats, count);
    return TEST_PASS;
}

static int test_convert_double_precision(void)
{
    printf("  test_convert_double_precision...\n");

    ParameterBinding bindings[MAX_PARAMETERS];
    int bound_count = 0;
    memset(bindings, 0, sizeof(bindings));

    double value = 3.141592653589793;
    SQLLEN indicator = sizeof(double);

    parameter_bind(bindings, &bound_count, 1, SQL_PARAM_INPUT,
                   SQL_C_DOUBLE, SQL_DOUBLE, 0, 0, &value, sizeof(value), &indicator);

    const char **values = NULL;
    int *lengths = NULL;
    int *formats = NULL;
    int count = 0;

    parameter_build_libpq_arrays(bindings, bound_count, &values, &lengths, &formats, &count);

    /* Verify the converted string can round-trip back to the original value */
    double parsed = strtod(values[0], NULL);
    ASSERT_TRUE(fabs(parsed - value) < 1e-15, "double should round-trip through text");

    parameter_free_libpq_arrays(values, lengths, formats, count);
    return TEST_PASS;
}

static int test_convert_float(void)
{
    printf("  test_convert_float...\n");

    ParameterBinding bindings[MAX_PARAMETERS];
    int bound_count = 0;
    memset(bindings, 0, sizeof(bindings));

    float value = 2.718281828f;
    SQLLEN indicator = sizeof(float);

    parameter_bind(bindings, &bound_count, 1, SQL_PARAM_INPUT,
                   SQL_C_FLOAT, SQL_REAL, 0, 0, &value, sizeof(value), &indicator);

    const char **values = NULL;
    int *lengths = NULL;
    int *formats = NULL;
    int count = 0;

    parameter_build_libpq_arrays(bindings, bound_count, &values, &lengths, &formats, &count);

    /* Verify the converted string can round-trip back to the original float value */
    float parsed = strtof(values[0], NULL);
    ASSERT_TRUE(fabsf(parsed - value) < 1e-6f, "float should round-trip through text");

    parameter_free_libpq_arrays(values, lengths, formats, count);
    return TEST_PASS;
}

static int test_convert_bigint(void)
{
    printf("  test_convert_bigint...\n");

    ParameterBinding bindings[MAX_PARAMETERS];
    int bound_count = 0;
    memset(bindings, 0, sizeof(bindings));

    int64_t value = 9223372036854775807LL;  /* INT64_MAX */
    SQLLEN indicator = sizeof(int64_t);

    parameter_bind(bindings, &bound_count, 1, SQL_PARAM_INPUT,
                   SQL_C_SBIGINT, SQL_BIGINT, 0, 0, &value, sizeof(value), &indicator);

    const char **values = NULL;
    int *lengths = NULL;
    int *formats = NULL;
    int count = 0;

    parameter_build_libpq_arrays(bindings, bound_count, &values, &lengths, &formats, &count);

    ASSERT_STREQ(values[0], "9223372036854775807", "INT64_MAX should convert correctly");

    parameter_free_libpq_arrays(values, lengths, formats, count);
    return TEST_PASS;
}

static int test_convert_bit(void)
{
    printf("  test_convert_bit...\n");

    ParameterBinding bindings[MAX_PARAMETERS];
    int bound_count = 0;
    memset(bindings, 0, sizeof(bindings));

    unsigned char true_value = 1;
    unsigned char false_value = 0;
    SQLLEN indicator = sizeof(unsigned char);

    parameter_bind(bindings, &bound_count, 1, SQL_PARAM_INPUT,
                   SQL_C_BIT, SQL_BIT, 0, 0, &true_value, sizeof(true_value), &indicator);
    parameter_bind(bindings, &bound_count, 2, SQL_PARAM_INPUT,
                   SQL_C_BIT, SQL_BIT, 0, 0, &false_value, sizeof(false_value), &indicator);

    const char **values = NULL;
    int *lengths = NULL;
    int *formats = NULL;
    int count = 0;

    parameter_build_libpq_arrays(bindings, bound_count, &values, &lengths, &formats, &count);

    ASSERT_STREQ(values[0], "1", "true bit should be '1'");
    ASSERT_STREQ(values[1], "0", "false bit should be '0'");

    parameter_free_libpq_arrays(values, lengths, formats, count);
    return TEST_PASS;
}

static int test_convert_binary(void)
{
    printf("  test_convert_binary...\n");

    ParameterBinding bindings[MAX_PARAMETERS];
    int bound_count = 0;
    memset(bindings, 0, sizeof(bindings));

    unsigned char binary_data[] = {0xDE, 0xAD, 0xBE, 0xEF};
    SQLLEN indicator = 4;

    parameter_bind(bindings, &bound_count, 1, SQL_PARAM_INPUT,
                   SQL_C_BINARY, SQL_VARBINARY, 4, 0, binary_data, sizeof(binary_data), &indicator);

    const char **values = NULL;
    int *lengths = NULL;
    int *formats = NULL;
    int count = 0;

    parameter_build_libpq_arrays(bindings, bound_count, &values, &lengths, &formats, &count);

    ASSERT_STREQ(values[0], "\\xdeadbeef", "binary should be hex-encoded with \\x prefix");

    parameter_free_libpq_arrays(values, lengths, formats, count);
    return TEST_PASS;
}

static int test_unbound_gap_produces_null(void)
{
    printf("  test_unbound_gap_produces_null...\n");

    ParameterBinding bindings[MAX_PARAMETERS];
    int bound_count = 0;
    memset(bindings, 0, sizeof(bindings));

    int32_t value = 42;
    SQLLEN indicator = sizeof(int32_t);

    /* Bind parameter 1 and 3, leaving parameter 2 unbound */
    parameter_bind(bindings, &bound_count, 1, SQL_PARAM_INPUT,
                   SQL_C_SLONG, SQL_INTEGER, 0, 0, &value, sizeof(value), &indicator);
    parameter_bind(bindings, &bound_count, 3, SQL_PARAM_INPUT,
                   SQL_C_SLONG, SQL_INTEGER, 0, 0, &value, sizeof(value), &indicator);

    const char **values = NULL;
    int *lengths = NULL;
    int *formats = NULL;
    int count = 0;

    parameter_build_libpq_arrays(bindings, bound_count, &values, &lengths, &formats, &count);

    ASSERT_INT_EQ(count, 3, "array should cover positions 1-3");
    ASSERT_NOT_NULL(values[0], "parameter 1 should be non-NULL");
    ASSERT_NULL(values[1], "unbound parameter 2 should be NULL");
    ASSERT_NOT_NULL(values[2], "parameter 3 should be non-NULL");

    parameter_free_libpq_arrays(values, lengths, formats, count);
    return TEST_PASS;
}

static int test_no_parameters_empty_arrays(void)
{
    printf("  test_no_parameters_empty_arrays...\n");

    ParameterBinding bindings[MAX_PARAMETERS];
    int bound_count = 0;
    memset(bindings, 0, sizeof(bindings));

    const char **values = NULL;
    int *lengths = NULL;
    int *formats = NULL;
    int count = 99;  /* Should be set to 0 */

    SQLRETURN result = parameter_build_libpq_arrays(bindings, bound_count,
                                                    &values, &lengths, &formats, &count);

    ASSERT_INT_EQ(result, SQL_SUCCESS, "empty build should succeed");
    ASSERT_INT_EQ(count, 0, "count should be 0 with no bindings");
    ASSERT_NULL(values, "values should be NULL");
    ASSERT_NULL(lengths, "lengths should be NULL");
    ASSERT_NULL(formats, "formats should be NULL");

    return TEST_PASS;
}

static int test_convert_unsigned_long(void)
{
    printf("  test_convert_unsigned_long...\n");

    ParameterBinding bindings[MAX_PARAMETERS];
    int bound_count = 0;
    memset(bindings, 0, sizeof(bindings));

    uint32_t value = 4294967295U;  /* UINT32_MAX */
    SQLLEN indicator = sizeof(uint32_t);

    parameter_bind(bindings, &bound_count, 1, SQL_PARAM_INPUT,
                   SQL_C_ULONG, SQL_INTEGER, 0, 0, &value, sizeof(value), &indicator);

    const char **values = NULL;
    int *lengths = NULL;
    int *formats = NULL;
    int count = 0;

    parameter_build_libpq_arrays(bindings, bound_count, &values, &lengths, &formats, &count);

    ASSERT_STREQ(values[0], "4294967295", "UINT32_MAX should convert correctly");

    parameter_free_libpq_arrays(values, lengths, formats, count);
    return TEST_PASS;
}

static int test_convert_short(void)
{
    printf("  test_convert_short...\n");

    ParameterBinding bindings[MAX_PARAMETERS];
    int bound_count = 0;
    memset(bindings, 0, sizeof(bindings));

    int16_t value = -32768;  /* INT16_MIN */
    SQLLEN indicator = sizeof(int16_t);

    parameter_bind(bindings, &bound_count, 1, SQL_PARAM_INPUT,
                   SQL_C_SSHORT, SQL_SMALLINT, 0, 0, &value, sizeof(value), &indicator);

    const char **values = NULL;
    int *lengths = NULL;
    int *formats = NULL;
    int count = 0;

    parameter_build_libpq_arrays(bindings, bound_count, &values, &lengths, &formats, &count);

    ASSERT_STREQ(values[0], "-32768", "INT16_MIN should convert correctly");

    parameter_free_libpq_arrays(values, lengths, formats, count);
    return TEST_PASS;
}

static int test_convert_tinyint(void)
{
    printf("  test_convert_tinyint...\n");

    ParameterBinding bindings[MAX_PARAMETERS];
    int bound_count = 0;
    memset(bindings, 0, sizeof(bindings));

    int8_t value = -128;  /* INT8_MIN */
    SQLLEN indicator = sizeof(int8_t);

    parameter_bind(bindings, &bound_count, 1, SQL_PARAM_INPUT,
                   SQL_C_STINYINT, SQL_TINYINT, 0, 0, &value, sizeof(value), &indicator);

    const char **values = NULL;
    int *lengths = NULL;
    int *formats = NULL;
    int count = 0;

    parameter_build_libpq_arrays(bindings, bound_count, &values, &lengths, &formats, &count);

    ASSERT_STREQ(values[0], "-128", "INT8_MIN should convert correctly");

    parameter_free_libpq_arrays(values, lengths, formats, count);
    return TEST_PASS;
}

/* ---- Main ---- */

int main(void)
{
    int result = TEST_PASS;

    printf("Running parameter binding tests...\n");

    if (test_bind_single_parameter() != TEST_PASS) result = TEST_FAIL;
    if (test_bind_multiple_parameters() != TEST_PASS) result = TEST_FAIL;
    if (test_rebind_same_parameter() != TEST_PASS) result = TEST_FAIL;
    if (test_unbind_all() != TEST_PASS) result = TEST_FAIL;
    if (test_invalid_parameter_number() != TEST_PASS) result = TEST_FAIL;
    if (test_convert_integer() != TEST_PASS) result = TEST_FAIL;
    if (test_convert_negative_integer() != TEST_PASS) result = TEST_FAIL;
    if (test_convert_string_nts() != TEST_PASS) result = TEST_FAIL;
    if (test_convert_string_explicit_length() != TEST_PASS) result = TEST_FAIL;
    if (test_convert_null_indicator() != TEST_PASS) result = TEST_FAIL;
    if (test_convert_null_buffer() != TEST_PASS) result = TEST_FAIL;
    if (test_convert_double_precision() != TEST_PASS) result = TEST_FAIL;
    if (test_convert_float() != TEST_PASS) result = TEST_FAIL;
    if (test_convert_bigint() != TEST_PASS) result = TEST_FAIL;
    if (test_convert_bit() != TEST_PASS) result = TEST_FAIL;
    if (test_convert_binary() != TEST_PASS) result = TEST_FAIL;
    if (test_unbound_gap_produces_null() != TEST_PASS) result = TEST_FAIL;
    if (test_no_parameters_empty_arrays() != TEST_PASS) result = TEST_FAIL;
    if (test_convert_unsigned_long() != TEST_PASS) result = TEST_FAIL;
    if (test_convert_short() != TEST_PASS) result = TEST_FAIL;
    if (test_convert_tinyint() != TEST_PASS) result = TEST_FAIL;

    printf("\nResults: %d/%d tests passed.\n", tests_passed, tests_run);

    if (result == TEST_PASS) {
        printf("All parameter binding tests passed.\n");
    } else {
        fprintf(stderr, "Some tests FAILED.\n");
    }

    return result;
}
