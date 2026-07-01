/*-------------------------------------------------------------------------
 *
 * driver_main.c
 *	  Shared library entry point
 *
 * Copyright (c) 2026, Dave Cramer
 *
 *
 * IDENTIFICATION
 *	  src/driver_main.c
 *
 *-------------------------------------------------------------------------
 */
#include "psqlodbc2.h"

#include <stdbool.h>

/* Forward declarations for the platform-neutral lifecycle functions */
static void global_initialize(void);
static void global_cleanup(void);

/* Track whether initialization has already occurred to guard against
 * redundant calls (e.g., if DllMain is called multiple times). */
static bool driver_initialized = false;

/*
 * global_initialize — One-time setup when the driver library is loaded.
 *
 * Currently a placeholder for future work (mutex init, logging setup, etc.).
 * Called from DllMain(DLL_PROCESS_ATTACH) on Windows or __attribute__((constructor))
 * on POSIX.
 */
static void global_initialize(void)
{
    if (driver_initialized) {
        return;
    }
    driver_initialized = true;

    /* Future: initialize global mutexes, logging subsystem, etc. */
}

/*
 * global_cleanup — One-time teardown when the driver library is unloaded.
 *
 * Releases any resources allocated in global_initialize().
 * Called from DllMain(DLL_PROCESS_DETACH) on Windows or __attribute__((destructor))
 * on POSIX.
 */
static void global_cleanup(void)
{
    if (!driver_initialized) {
        return;
    }
    driver_initialized = false;

    /* Future: destroy global mutexes, flush logs, etc. */
}

/* ---- Platform-Specific Entry Points ---- */

#if defined(PSQLODBC2_PLATFORM_WINDOWS)

#include <windows.h>

BOOL WINAPI DllMain(HINSTANCE instance_handle, DWORD call_reason, LPVOID reserved)
{
    (void)instance_handle;
    (void)reserved;

    switch (call_reason) {
    case DLL_PROCESS_ATTACH:
        global_initialize();
        break;
    case DLL_PROCESS_DETACH:
        global_cleanup();
        break;
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
        /* No per-thread initialization needed at this time */
        break;
    }
    return TRUE;
}

#elif defined(__GNUC__) || defined(__clang__)
/* GCC and Clang support constructor/destructor attributes for automatic
 * initialization when the shared library is loaded/unloaded. */

__attribute__((constructor))
static void posix_library_load(void)
{
    global_initialize();
}

__attribute__((destructor))
static void posix_library_unload(void)
{
    global_cleanup();
}

#else
    #error "Unsupported compiler: need DllMain (MSVC) or __attribute__((constructor)) (GCC/Clang)"
#endif
