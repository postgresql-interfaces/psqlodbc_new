# Plan: Meson Build System and Minimal ODBC Driver

## Task Description
Set up the Meson build system, project directory structure, and a minimal hello-world ODBC driver that compiles as a shared library on Linux, macOS, and Windows. The driver will export a handful of stub ODBC entry points sufficient for the ODBC Driver Manager to load it and call SQLGetFunctions. This establishes the foundational build infrastructure that all future work depends on.

## Objective
When this plan is complete, running `meson setup builddir && meson compile -C builddir && meson test -C builddir` will:
1. Configure a cross-platform C11 build
2. Compile a shared library named `psqlodbc2w` (`.so` / `.dylib` / `.dll`)
3. Run a test that dynamically loads the library and calls SQLDriverConnect (receiving SQL_ERROR since there's no backend — proving the export works)

## Problem Statement
The project currently has only `CLAUDE.md` and a `specs/` directory. There is no build system, no source code, and no way to compile anything. We need the foundational skeleton before any ODBC functionality can be implemented.

## Solution Approach
Use Meson as the build system (as specified in CLAUDE.md). Create a clean project structure with:
- Platform abstraction headers so future code never uses raw `#ifdef WIN32`
- A single `odbc_api.c` dispatch file that exports all ODBC entry points (stubs for now)
- An environment module as the first real subsystem (since `SQLAllocHandle(SQL_HANDLE_ENV, ...)` is the first call any application makes)
- A test that loads the driver shared library and exercises the export table

The Meson build will:
- Require C11
- Find the ODBC headers (unixODBC / iODBC on Unix, Windows SDK on Windows)
- Produce a shared library with the correct symbol visibility
- Define platform macros consistently

## Relevant Files

### Original Reference (~/projects/psqlodbc)
- `psqlodbc.c` — DLL entry point, global initialization
- `psqlodbc.h` — Master include, type definitions, memory wrappers
- `environ.c` / `environ.h` — Environment handle management
- `odbcapi.c` — ODBC API dispatch layer
- `psqlodbc.def` — Windows export definitions
- `loadlib.c` / `loadlib.h` — Dynamic library loading
- `version.h` — Version constants
- `configure.ac` — Build configuration (autoconf — we replace with Meson)

### New Files (this project)
- `meson.build` — Root build definition (project name, version, C standard, subdir calls)
- `meson.options` — Build options (driver_manager choice: unixodbc/iodbc/none)
- `src/meson.build` — Source directory build (library target, sources, dependencies)
- `src/driver_main.c` — Shared library entry point (DllMain on Windows, constructor/destructor on POSIX)
- `src/odbc_api.c` — All exported ODBC function stubs (single dispatch file per conventions)
- `src/environment.c` — Environment handle alloc/free implementation
- `src/environment.h` — Environment handle public interface
- `include/psqlodbc2.h` — Master driver header (version, common types, platform detection)
- `include/platform/platform_defs.h` — Platform detection macros and DLL export attributes
- `tests/meson.build` — Test build definition
- `tests/test_driver_load.c` — Integration test: load library, call SQLAllocHandle + SQLFreeHandle
- `psqlodbc2.def` — Windows module definition file (exported symbols with ordinals)

## Implementation Phases

### Phase 1: Foundation
Set up the Meson build system and platform abstraction:
1. Create root `meson.build` with project declaration (C11, version 0.1.0)
2. Create `meson.options` with `driver_manager` option
3. Create `include/psqlodbc2.h` with version constants and common includes
4. Create `include/platform/platform_defs.h` with `PSQLODBC_EXPORT` macro and platform detection
5. Create `psqlodbc2.def` for Windows exports

### Phase 2: Core Implementation
Implement the minimal driver source:
1. Create `src/driver_main.c` — entry point with global init/cleanup
2. Create `src/environment.h` / `src/environment.c` — environment handle struct, alloc, free
3. Create `src/odbc_api.c` — exported ODBC stubs: `SQLAllocHandle`, `SQLFreeHandle`, `SQLGetFunctions`, `SQLDriverConnect`, `SQLGetDiagRec` (all return SQL_ERROR except AllocHandle for ENV)
4. Create `src/meson.build` — build the shared library

### Phase 3: Integration & Polish
Wire up tests and verify:
1. Create `tests/test_driver_load.c` — dlopen/LoadLibrary the built driver, resolve SQLAllocHandle, call it
2. Create `tests/meson.build` — test executable linked against ODBC
3. Verify `meson setup builddir && meson compile -C builddir && meson test -C builddir` passes

## Code Examples

### Platform export macro (`include/platform/platform_defs.h`):
```c
#ifndef PSQLODBC2_PLATFORM_DEFS_H
#define PSQLODBC2_PLATFORM_DEFS_H

#if defined(_WIN32) || defined(_WIN64)
    #define PSQLODBC2_PLATFORM_WINDOWS 1
    #define PSQLODBC2_EXPORT __declspec(dllexport)
#elif defined(__APPLE__)
    #define PSQLODBC2_PLATFORM_MACOS 1
    #define PSQLODBC2_EXPORT __attribute__((visibility("default")))
#else
    #define PSQLODBC2_PLATFORM_LINUX 1
    #define PSQLODBC2_EXPORT __attribute__((visibility("default")))
#endif

#endif
```

### Minimal ODBC stub pattern (`src/odbc_api.c`):
```c
#include <sql.h>
#include <sqlext.h>
#include "psqlodbc2.h"
#include "environment.h"

SQLRETURN SQL_API SQLAllocHandle(SQLSMALLINT handle_type,
                                 SQLHANDLE input_handle,
                                 SQLHANDLE *output_handle)
{
    if (!output_handle)
        return SQL_INVALID_HANDLE;

    switch (handle_type) {
    case SQL_HANDLE_ENV:
        return environment_allocate(output_handle);
    case SQL_HANDLE_DBC:
    case SQL_HANDLE_STMT:
    case SQL_HANDLE_DESC:
        /* Not yet implemented */
        return SQL_ERROR;
    default:
        return SQL_INVALID_HANDLE;
    }
}
```

### Root meson.build pattern:
```meson
project('psqlodbc2', 'c',
    version : '0.1.0',
    default_options : ['c_std=c11', 'warning_level=2'],
    license : 'LGPL-2.1-or-later',
)

# Find ODBC dependency
odbc_dep = dependency('odbc', required : false)
if not odbc_dep.found()
    # Try pkg-config for unixODBC
    odbc_dep = dependency('unixODBC', required : false)
endif
if not odbc_dep.found()
    # Fall back to checking for the header directly
    cc = meson.get_compiler('c')
    assert(cc.has_header('sql.h'), 'ODBC headers not found')
    odbc_dep = declare_dependency()
endif

subdir('src')
subdir('tests')
```

## Team Orchestration

- You operate as the team lead and orchestrate the team to execute the plan.
- You NEVER operate directly on the codebase. You use `Task` and `Task*` tools to deploy team members.
- The flow is: Builder implements → Reviewer checks quality → Validator verifies correctness.

### Team Members

- Builder
  - Name: builder-scaffold
  - Role: Create all project files — build system, source, headers, and tests
  - Agent Type: builder
  - Resume: true
- Reviewer
  - Name: reviewer-build
  - Role: Code quality and C11 best practices review
  - Agent Type: reviewer
  - Resume: false
- Validator
  - Name: validator-build
  - Role: Build verification and behavioral correctness
  - Agent Type: validator
  - Resume: false

## Step by Step Tasks

- Execute every step in order, top to bottom.
- Before you start, run `TaskCreate` to create the initial task list.

### 1. Create Build System and Project Structure
- **Task ID**: create-build-system
- **Depends On**: none
- **Assigned To**: builder-scaffold
- **Agent Type**: builder
- **Parallel**: false
- Create root `meson.build` with project declaration, C11 standard, ODBC dependency detection
- Create `meson.options` with option `driver_manager` (combo: `['auto', 'unixodbc', 'iodbc', 'none']`, default: `'auto'`)
- Create `include/psqlodbc2.h` with version defines (`PSQLODBC2_VERSION "0.1.0"`, major/minor/patch), common includes (`<sql.h>`, `<sqlext.h>`, `<sqltypes.h>`)
- Create `include/platform/platform_defs.h` with platform detection and `PSQLODBC2_EXPORT` macro
- Create `psqlodbc2.def` with minimal Windows exports (SQLAllocHandle, SQLFreeHandle, SQLGetFunctions, SQLDriverConnect, SQLGetDiagRec)
- Create directory structure: `src/`, `include/`, `include/platform/`, `tests/`

### 2. Implement Minimal Driver Source
- **Task ID**: implement-driver-source
- **Depends On**: create-build-system
- **Assigned To**: builder-scaffold
- **Agent Type**: builder
- **Parallel**: false
- Create `src/driver_main.c` with DllMain (Windows) and `__attribute__((constructor))` / `__attribute__((destructor))` (POSIX) for global init/cleanup
- Create `src/environment.h` declaring: `environment_allocate(SQLHANDLE *output_handle)`, `environment_free(SQLHANDLE handle)`, and the environment struct
- Create `src/environment.c` implementing environment alloc (malloc + zero-init) and free
- Create `src/odbc_api.c` with exported stubs for: `SQLAllocHandle`, `SQLFreeHandle`, `SQLGetFunctions`, `SQLDriverConnect`, `SQLGetDiagRec`, `SQLGetDiagField`. All stubs return `SQL_ERROR` except `SQLAllocHandle` for `SQL_HANDLE_ENV` (which delegates to environment module) and `SQLFreeHandle` for `SQL_HANDLE_ENV`. `SQLGetFunctions` should report that no functions are supported (set all to SQL_FALSE).
- Create `src/meson.build` that builds a `shared_library('psqlodbc2w', ...)` with correct symbol visibility, links ODBC, installs to lib dir
- All source files must include ODBC API doc comments with spec reference URLs on the exported functions
- Use descriptive variable names throughout (no abbreviations)

### 3. Implement Test Suite
- **Task ID**: implement-tests
- **Depends On**: implement-driver-source
- **Assigned To**: builder-scaffold
- **Agent Type**: builder
- **Parallel**: false
- Create `tests/test_driver_load.c` that: uses dlopen/LoadLibrary to load the built `psqlodbc2w` shared library, resolves `SQLAllocHandle` symbol, calls it with `SQL_HANDLE_ENV`, verifies return is `SQL_SUCCESS`, calls `SQLFreeHandle`, verifies return is `SQL_SUCCESS`
- Create `tests/meson.build` that builds and registers the test executable
- Ensure the test can find the library (use meson's build directory path or `LD_LIBRARY_PATH`)

### 4. Review Code Quality
- **Task ID**: review-code-quality
- **Depends On**: implement-tests
- **Assigned To**: reviewer-build
- **Agent Type**: reviewer
- **Parallel**: false
- Review all files for C11 compliance (no GNU extensions without guards)
- Verify naming conventions match CLAUDE.md requirements (descriptive names, no abbreviations)
- Check that ODBC API functions have doc comments with spec URLs
- Verify platform abstraction is clean (no raw `#ifdef WIN32` in source files)
- Check Meson build for correctness (proper dependency handling, install paths)
- Ensure no security issues (buffer overflows, null pointer dereferences)

### 5. Validate Build and Tests
- **Task ID**: validate-all
- **Depends On**: review-code-quality
- **Assigned To**: validator-build
- **Agent Type**: validator
- **Parallel**: false
- Run `meson setup builddir` — must succeed
- Run `meson compile -C builddir` — must produce shared library with no errors
- Run `meson test -C builddir` — test must pass
- Verify the shared library exports the expected symbols (`nm -D` on Linux/macOS or `dumpbin /exports` on Windows)
- Verify behavioral compatibility: the driver should be loadable by an ODBC Driver Manager

## Acceptance Criteria
- `meson setup builddir` configures without errors on the current platform
- `meson compile -C builddir` produces `libpsqlodbc2w.so` (Linux), `libpsqlodbc2w.dylib` (macOS), or `psqlodbc2w.dll` (Windows)
- `meson test -C builddir` passes — the driver loads and SQLAllocHandle(SQL_HANDLE_ENV) returns SQL_SUCCESS
- The shared library exports at minimum: SQLAllocHandle, SQLFreeHandle, SQLGetFunctions, SQLDriverConnect, SQLGetDiagRec
- All source files use C11 with no compiler warnings at warning_level=2
- Code follows CLAUDE.md naming and commenting conventions
- No raw platform ifdefs outside of `include/platform/` and `src/driver_main.c`

## Validation Commands
- `meson setup builddir` - Configure build
- `meson compile -C builddir` - Compile
- `meson test -C builddir` - Run tests
- `nm -D builddir/src/libpsqlodbc2w.so | grep SQL` - Verify exports (Linux)
- `nm -gU builddir/src/libpsqlodbc2w.dylib | grep SQL` - Verify exports (macOS)

## Notes
- The original psqlodbc uses autoconf/automake. We replace this entirely with Meson for cleaner cross-platform support.
- The original driver is named `psqlodbc35w` (35 = ODBC 3.5, w = Unicode). We name ours `psqlodbc2w` (2 = this rewrite, w = Unicode).
- On macOS, unixODBC is typically installed via Homebrew: `brew install unixodbc`. The meson build should find it via pkg-config.
- The `.def` file is only used on Windows builds. Meson handles this via the `vs_module_defs` keyword argument.
- Future plans will add real connection, statement, and descriptor modules. This plan only creates the skeleton that proves the build works end-to-end.
