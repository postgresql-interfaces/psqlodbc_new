/*
 * test_platform.h — cross-platform dynamic loading for tests
 *
 * On Windows, provides dlopen/dlsym/dlclose/dlerror macros that map to
 * LoadLibraryA/GetProcAddress/FreeLibrary. On Unix, just includes dlfcn.h.
 *
 * All handles are void* for source compatibility with existing tests.
 */
#ifndef TEST_PLATFORM_H
#define TEST_PLATFORM_H

#if defined(_WIN32) || defined(_WIN64)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

/* RTLD_NOW is not needed on Windows but used as dlopen flag in source */
#ifndef RTLD_NOW
#define RTLD_NOW 0
#endif

static inline void *platform_dlopen(const char *path, int flags) {
    (void)flags;
    return (void *)LoadLibraryA(path);
}

static inline void *platform_dlsym(void *lib, const char *name) {
    return (void *)(intptr_t)GetProcAddress((HMODULE)lib, name);
}

static inline int platform_dlclose(void *lib) {
    FreeLibrary((HMODULE)lib);
    return 0;
}

static inline const char *platform_dlerror(void) {
    return "LoadLibrary/GetProcAddress failed";
}

#define dlopen(path, flags) platform_dlopen(path, flags)
#define dlsym(lib, name) platform_dlsym(lib, name)
#define dlclose(lib) platform_dlclose(lib)
#define dlerror() platform_dlerror()

#else
#include <dlfcn.h>
#endif

#endif /* TEST_PLATFORM_H */
