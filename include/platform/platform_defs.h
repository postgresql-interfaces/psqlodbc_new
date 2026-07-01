/*-------------------------------------------------------------------------
 *
 * platform_defs.h
 *	  Platform detection and export macros
 *
 * Copyright (c) 2026, Dave Cramer
 *
 *
 * IDENTIFICATION
 *	  include/platform/platform_defs.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PSQLODBC2_PLATFORM_DEFS_H
#define PSQLODBC2_PLATFORM_DEFS_H

/* ---- Platform Detection ---- */

#if defined(_WIN32) || defined(_WIN64)
    #define PSQLODBC2_PLATFORM_WINDOWS 1
#elif defined(__APPLE__) && defined(__MACH__)
    #define PSQLODBC2_PLATFORM_MACOS 1
#else
    #define PSQLODBC2_PLATFORM_LINUX 1
#endif

/* ---- Shared Library Symbol Export ----
 *
 * PSQLODBC2_EXPORT marks functions that must be visible outside the shared
 * library (i.e., ODBC entry points).
 *
 * On Windows: exports are handled by the .def file (psqlodbc2.def), not by
 * __declspec(dllexport). Using dllexport would conflict with the ODBC function
 * declarations in sql.h/sqlext.h which don't have dllexport, causing MSVC
 * error C2375 "redefinition; different linkage".
 *
 * On Unix: uses visibility("default") paired with -fvisibility=hidden at the
 * compiler level (set via meson's gnu_symbol_visibility).
 */

#if defined(PSQLODBC2_PLATFORM_WINDOWS)
    #define PSQLODBC2_EXPORT
#elif defined(__GNUC__) || defined(__clang__)
    #define PSQLODBC2_EXPORT __attribute__((visibility("default")))
#else
    #define PSQLODBC2_EXPORT
#endif

#endif /* PSQLODBC2_PLATFORM_DEFS_H */
