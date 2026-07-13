# Plan: DSN Registry Support (ODBC.INI Reading)

## Task Description
Implement DSN (Data Source Name) lookup so that SQLConnect(DSN_name, ...) resolves connection parameters from the system's ODBC configuration (odbc.ini on Unix, registry on Windows). This is the standard ODBC mechanism for configuring data sources without embedding connection strings in application code.

## Objective
When this plan is complete:
1. SQLConnect("mydsn", user, pass) reads server/port/database/sslmode from odbc.ini
2. SQLDriverConnect with DSN=mydsn resolves parameters from odbc.ini before connecting
3. Parameters specified in the connection string override DSN defaults
4. On Unix: reads from ~/.odbc.ini and /etc/odbc.ini (or ODBCINI env var)
5. On Windows: reads from the ODBC registry via SQLGetPrivateProfileString

## Problem Statement
Currently SQLConnect uses the DSN name directly as the database name — a crude fallback. Real ODBC applications configure DSNs via `odbcinst` (or ODBC Administrator on Windows) and expect SQLConnect to resolve them. Without DSN support, the driver cannot be used with standard ODBC configuration tools.

## Solution Approach
1. **Use ODBC API for reading** — call `SQLGetPrivateProfileString()` (provided by the driver manager's odbcinst library) to read key-value pairs from the DSN section
2. **Read essential keys** — Server/Servername, Port, Database, Username, Password, SSLmode, ApplicationName, Description
3. **Merge with explicit params** — connection string parameters override DSN values (same precedence as original psqlodbc)
4. **Platform abstraction** — on Unix link against libodbcinst; on Windows the DM provides the functions

## Relevant Files

### Original Reference (~/projects/psqlodbc)
- `dlg_specific.c` — getDSNinfo() (lines 931-1159): reads ~50 keys from ODBC.INI via SQLGetPrivateProfileString
- `dlg_specific.h` — INI_SERVER, INI_PORT, INI_DATABASE, etc. (key name constants)
- `connection.c` — PGAPI_Connect calls getDSNinfo to fill ConnInfo before connecting

### New Files (this project)
- `src/dsn_config.h` — DSN reading declarations, INI key constants
- `src/dsn_config.c` — Implementation using SQLGetPrivateProfileString

### Modified Files
- `src/connection_string.c` — Add DSN lookup step when DSN key is present
- `src/odbc_api.c` — Update SQLConnect to resolve DSN before connecting
- `meson.build` — Find libodbcinst dependency (provides SQLGetPrivateProfileString on Unix)
- `src/meson.build` — Add dsn_config.c, link against odbcinst

## Implementation Phases

### Phase 1: Foundation
- Define INI key constants for the essential parameters
- Create dsn_config module with function to read DSN info into ConnectionInfo
- Find and link against libodbcinst (Unix) or the driver manager library

### Phase 2: Core Implementation
- Implement dsn_config_read(dsn_name, ConnectionInfo*): calls SQLGetPrivateProfileString for each key
- Handle missing keys gracefully (leave field empty)
- Implement merge logic: DSN values are defaults, explicit connection string params override
- Update SQLConnect: resolve DSN name → fill ConnectionInfo → connect
- Update SQLDriverConnect: if DSN= is in connection string, resolve DSN first, then apply remaining keys

### Phase 3: Integration & Polish
- Test with a temporary odbc.ini file (set ODBCINI env var for testing)
- Handle edge cases: DSN not found (return error), empty DSN, DSN with no server (use localhost)
- Document which INI keys are supported

## Team Orchestration

- You operate as the team lead and orchestrate the team to execute the plan.
- You NEVER operate directly on the codebase. You use `Task` and `Task*` tools to deploy team members.
- The flow is: Builder implements → Reviewer checks quality → Validator verifies correctness.

### Team Members

- Builder
  - Name: builder-dsn
  - Role: Implement DSN reading from ODBC.INI
  - Agent Type: builder
  - Resume: true
- Reviewer
  - Name: reviewer-dsn
  - Role: Code quality and C11 best practices review
  - Agent Type: reviewer
  - Resume: false
- Validator
  - Name: validator-dsn
  - Role: Build verification and behavioral correctness
  - Agent Type: validator
  - Resume: false

## Step by Step Tasks

- Execute every step in order, top to bottom.
- Before you start, run `TaskCreate` to create the initial task list.

### 1. Create DSN Config Module
- **Task ID**: create-dsn-module
- **Depends On**: none
- **Assigned To**: builder-dsn
- **Agent Type**: builder
- **Parallel**: false
- Create `src/dsn_config.h` with key constants and dsn_config_read declaration
- Create `src/dsn_config.c` implementing DSN reading via SQLGetPrivateProfileString
- Keys to read: Servername/Server, Port, Database, Username, Password, SSLmode, ApplicationName, Description, Timeout

### 2. Integrate with Connection Flow
- **Task ID**: integrate-dsn
- **Depends On**: create-dsn-module
- **Assigned To**: builder-dsn
- **Agent Type**: builder
- **Parallel**: false
- Update SQLConnect in odbc_api.c: call dsn_config_read before connection_connect
- Update connection_string_parse: when DSN key is found, call dsn_config_read to populate defaults, then let remaining keys override
- Find and link libodbcinst in meson.build

### 3. Test and Validate
- **Task ID**: validate-all
- **Depends On**: integrate-dsn
- **Assigned To**: validator-dsn
- **Agent Type**: validator
- **Parallel**: false
- Build the project
- Run all tests
- Test with a temp odbc.ini (set ODBCINI=/tmp/test_odbc.ini)

## Acceptance Criteria
- SQLConnect("testdsn", NULL, 0, NULL, 0, NULL, 0) reads parameters from odbc.ini and connects
- Connection string parameters override DSN values
- Missing DSN returns SQL_ERROR with appropriate diagnostic
- Password from DSN is read and used for authentication
- Works on macOS/Linux with unixODBC's libodbcinst

## Validation Commands
- `meson setup builddir --reconfigure` - Configure build
- `meson compile -C builddir` - Compile
- `meson test -C builddir` - Run tests

## Notes
- SQLGetPrivateProfileString is provided by the ODBC driver manager (unixODBC's libodbcinst on Unix, the system DM on Windows). We link against it at build time.
- On Unix, the function reads from files in this precedence: ODBCINI env var → ~/.odbc.ini → /etc/odbc.ini (or the system-configured location).
- The original psqlodbc reads ~50 INI keys. We start with the essential 8-10 and add more as features require them.
- Password in odbc.ini may be percent-encoded (spaces as %20, etc.). The original psqlodbc handles this — we should too.
- On Windows, the "ODBC.INI" is a registry path (HKCU\Software\ODBC\ODBC.INI). SQLGetPrivateProfileString abstracts this.
- If libodbcinst is not found at build time, DSN support can be disabled (compile without it, SQLConnect falls back to current behavior).
