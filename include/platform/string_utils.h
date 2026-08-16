/*-------------------------------------------------------------------------
 *
 * string_utils.h
 *	  Portable ASCII string helpers shared across the driver
 *
 * Copyright (c) 2026, Dave Cramer
 *
 *
 * IDENTIFICATION
 *	  include/platform/string_utils.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PSQLODBC2_PLATFORM_STRING_UTILS_H
#define PSQLODBC2_PLATFORM_STRING_UTILS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/*
 * These helpers replace C-library functions that are either not part of
 * standard C11 or whose behavior varies in ways we cannot tolerate:
 *
 *   - strcasecmp / strncasecmp are POSIX, and MSVC spells them _stricmp /
 *     _strnicmp. None is standard C11, so calling them directly would force a
 *     per-platform #ifdef at every call site.
 *   - The standard tolower() is locale-sensitive: under some locales (e.g.
 *     Turkish, where 'I' does not lowercase to 'i') keyword matching would
 *     silently change behavior with the process locale. Every string we
 *     compare here — SQL keywords, ODBC function names, connection-attribute
 *     keys, interval unit words — is pure ASCII, so we fold with fixed ASCII
 *     arithmetic. That is deterministic and identical on every platform.
 *   - strdup was only standardized in C23; under strict C11 we allocate and
 *     copy ourselves.
 *
 * Defined static inline so each translation unit gets its own copy without a
 * separate object file, and consolidated here so the driver has exactly one
 * implementation of each rather than a copy per module.
 */

/* Lowercase a single ASCII letter; leaves every other byte unchanged. */
static inline char pg_ascii_tolower(char byte)
{
    return (byte >= 'A' && byte <= 'Z') ? (char)(byte - 'A' + 'a') : byte;
}

/*
 * ASCII case-insensitive compare with strcmp-style ordering: returns a
 * negative, zero, or positive value when left sorts before, equal to, or after
 * right. The comparison after the loop is what distinguishes equal-length
 * matches from prefixes — when one string ends first its terminating '\0'
 * compares less than the other's remaining character.
 */
static inline int pg_ascii_strcasecmp(const char *left, const char *right)
{
    while (*left && *right) {
        int diff = (unsigned char)pg_ascii_tolower(*left) -
                   (unsigned char)pg_ascii_tolower(*right);
        if (diff != 0) {
            return diff;
        }
        left++;
        right++;
    }
    return (unsigned char)pg_ascii_tolower(*left) -
           (unsigned char)pg_ascii_tolower(*right);
}

/* ASCII case-insensitive equality test; true when the whole strings match. */
static inline bool pg_ascii_case_equal(const char *left, const char *right)
{
    return pg_ascii_strcasecmp(left, right) == 0;
}

/*
 * ASCII case-insensitive prefix test: true when the first prefix_length bytes
 * of value match prefix ignoring case. A value shorter than prefix_length
 * (its '\0' reached early) does not match.
 */
static inline bool pg_ascii_case_prefix(const char *value, const char *prefix,
                                        size_t prefix_length)
{
    for (size_t index = 0; index < prefix_length; index++) {
        if (value[index] == '\0' ||
            pg_ascii_tolower(value[index]) != pg_ascii_tolower(prefix[index])) {
            return false;
        }
    }
    return true;
}

/*
 * Portable strdup. Returns a heap copy of source, or NULL on allocation
 * failure (callers treat NULL as an out-of-memory error).
 */
static inline char *pg_strdup(const char *source)
{
    size_t size = strlen(source) + 1;
    char *copy = malloc(size);
    if (copy) {
        memcpy(copy, source, size);
    }
    return copy;
}

#endif /* PSQLODBC2_PLATFORM_STRING_UTILS_H */
