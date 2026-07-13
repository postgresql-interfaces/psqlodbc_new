# Plan: Connection Module

## Task Description
Implement the connection module for the psqlodbc2 driver: connection handle allocation/deallocation, connection string parsing, SQLConnect, SQLDriverConnect, and SQLDisconnect. This includes updating SQLAllocHandle/SQLFreeHandle to support SQL_HANDLE_DBC, adding a connection info structure for holding DSN parameters, implementing the actual libpq-based connection to PostgreSQL, and building out a diagnostic record system so connection errors are reportable via SQLGetDiagRec.

## Objective
When this plan is complete:
1. Applications can allocate a connection handle (SQLAllocHandle with SQL_HANDLE_DBC)
2. Applications can connect using SQLConnect(DSN, UID, PWD) or SQLDriverConnect(connection_string)
3. Applications can disconnect with SQLDisconnect
4. Applications can free the connection handle (SQLFreeHandle with SQL_HANDLE_DBC)
5. Connection errors are reportable via SQLGetDiagRec/SQLGetDiagField
6. The driver actually connects to a running PostgreSQL server via libpq

## Problem Statement
The driver currently only supports environment handles. No connections can be established, making the driver non-functional for any real ODBC workload. The connection module is the next critical piece — everything else (statements, queries, result sets) depends on having an active connection.

## Solution Approach
Create a connection module with:
1. **Connection handle struct** — holds libpq connection (PGconn*), connection state, connection info (DSN params), parent environment reference, and diagnostic records
2. **Connection info struct** — holds parsed connection parameters (server, port, database, username, password, sslmode, application_name, connect_timeout)
3. **Connection string parser** — parses `key=value;key=value` ODBC-style connection strings
4. **Diagnostic records** — a reusable diagnostic system (not connection-specific) that SQLGetDiagRec/SQLGetDiagField can query. Stored per-handle.
5. **Environment-connection linkage** — environment tracks its child connections; connection references its parent environment
6. **libpq integration** — Meson finds libpq dependency; CC_connect builds a PQconnectdb() call from parsed parameters

The original psqlodbc has a massive ConnInfo struct with ~50 fields and complex DSN registry logic. For this initial implementation we support the essential connection parameters only (server, port, database, username, password, sslmode, application_name, connect_timeout). Advanced options can be added incrementally.

## Relevant Files

### Original Reference (~/projects/psqlodbc)
- `connection.h` — ConnectionClass struct, connection states, error codes, CC_* prototypes
- `connection.c` — CC_Constructor, CC_Destructor, CC_connect, CC_cleanup, PGAPI_AllocConnect, PGAPI_Connect, PGAPI_Disconnect, PGAPI_FreeConnect
- `drvconn.c` — PGAPI_DriverConnect, connection string parsing
- `dlg_specific.h` — ConnInfo struct, DSN constants, INI_* defines
- `dlg_specific.c` — getDSNinfo, makeConnectString, copyConnAttributes
- `environ.c` — EN_add_connection, EN_remove_connection
- `environ.h` — EnvironmentClass with connection list

### New Files (this project)
- `src/connection.h` — OdbcConnection struct, ConnectionState enum, connection function declarations
- `src/connection.c` — Connection handle alloc/free, connect/disconnect using libpq, connection info management
- `src/connection_string.h` — Connection string parsing declarations
- `src/connection_string.c` — Parse ODBC connection strings (key=value pairs), build libpq connection strings
- `src/diagnostics.h` — Diagnostic record struct and queue, per-handle diagnostic API
- `src/diagnostics.c` — Diagnostic record allocation, storage, retrieval (for SQLGetDiagRec)
- `tests/test_connection.c` — Unit tests for connection string parsing and handle lifecycle

### Modified Files
- `src/environment.h` — Add connection tracking (list of child connections)
- `src/environment.c` — Implement add/remove connection from environment
- `src/odbc_api.c` — Wire up SQL_HANDLE_DBC in SQLAllocHandle/SQLFreeHandle, implement real SQLConnect/SQLDriverConnect/SQLDisconnect, implement real SQLGetDiagRec/SQLGetDiagField
- `src/meson.build` — Add new source files, add libpq dependency
- `meson.build` — Find libpq dependency
- `psqlodbc2.def` — Add SQLConnect, SQLDisconnect exports
- `tests/meson.build` — Add new test

## Implementation Phases

### Phase 1: Foundation (Diagnostics + Connection Struct)
1. Create the diagnostic record system (reusable across all handle types)
2. Define the connection handle struct with libpq pointer, state, parent env reference
3. Define the connection info struct with essential connection parameters
4. Update the environment handle to track child connections

### Phase 2: Core Implementation (Connect/Disconnect)
1. Implement connection string parsing (both `key=value;` ODBC style and DSN lookup — DSN lookup can be stubbed initially since it requires platform-specific ODBC.INI reading)
2. Implement SQLAllocHandle for SQL_HANDLE_DBC (allocate, link to parent environment)
3. Implement SQLConnect (look up DSN → get params → build libpq connstring → PQconnectdb)
4. Implement SQLDriverConnect (parse connection string → build libpq connstring → PQconnectdb)
5. Implement SQLDisconnect (PQfinish, reset state)
6. Implement SQLFreeHandle for SQL_HANDLE_DBC (unlink from environment, free)
7. Wire up SQLGetDiagRec/SQLGetDiagField to read from the diagnostic queue

### Phase 3: Integration & Polish
1. Add libpq as a Meson dependency
2. Update exports (psqlodbc2.def) 
3. Write tests: connection string parsing (pure unit tests), handle lifecycle (dlopen tests), live connection test (skipped if no PG server available)
4. Verify build passes on current platform

## Code Examples

### Connection state enum:
```c
typedef enum {
    CONNECTION_STATE_NOT_CONNECTED = 0,
    CONNECTION_STATE_CONNECTED,
    CONNECTION_STATE_BROKEN,
    CONNECTION_STATE_EXECUTING
} ConnectionState;
```

### Connection info struct (essential params only):
```c
typedef struct ConnectionInfo {
    char server[256];
    char port[16];
    char database[256];
    char username[256];
    char *password;          /* heap-allocated, cleared on free */
    char sslmode[32];
    char application_name[256];
    unsigned int connect_timeout;
} ConnectionInfo;
```

### Connection handle struct:
```c
#define CONNECTION_MAGIC_NUMBER 0x434F4E32  /* "CON2" in ASCII */

typedef struct OdbcConnection {
    unsigned int magic_number;
    ConnectionState state;
    OdbcEnvironment *parent_environment;
    PGconn *libpq_connection;
    ConnectionInfo info;
    DiagnosticRecords diagnostics;
    bool autocommit;
    int server_version_major;
    int server_version_minor;
} OdbcConnection;
```

### Diagnostic record struct:
```c
#define MAX_DIAGNOSTIC_RECORDS 16
#define SQLSTATE_LENGTH 5

typedef struct DiagnosticRecord {
    char sqlstate[SQLSTATE_LENGTH + 1];
    int native_error_code;
    char *message_text;       /* heap-allocated */
} DiagnosticRecord;

typedef struct DiagnosticRecords {
    DiagnosticRecord records[MAX_DIAGNOSTIC_RECORDS];
    int record_count;
} DiagnosticRecords;
```

### Connection string parsing:
```c
/* Parse "Server=localhost;Port=5432;Database=mydb;UID=user;PWD=pass"
 * into a ConnectionInfo struct. Keys are case-insensitive.
 * Recognized keys: Server/Servername, Port, Database, UID/Username,
 * PWD/Password, SSLmode, ApplicationName, Timeout */
bool connection_string_parse(const char *connection_string,
                             SQLSMALLINT string_length,
                             ConnectionInfo *out_info);

/* Build a libpq-compatible connection string from ConnectionInfo.
 * Output: "host=localhost port=5432 dbname=mydb user=user password=pass" */
bool connection_info_to_libpq_string(const ConnectionInfo *info,
                                     char *output_buffer,
                                     size_t buffer_size);
```

### Updated environment struct:
```c
#define MAX_CONNECTIONS_PER_ENVIRONMENT 128

typedef struct OdbcEnvironment {
    unsigned int magic_number;
    int odbc_version;
    DiagnosticRecords diagnostics;
    struct OdbcConnection *connections[MAX_CONNECTIONS_PER_ENVIRONMENT];
    int connection_count;
} OdbcEnvironment;
```

## Team Orchestration

- You operate as the team lead and orchestrate the team to execute the plan.
- You NEVER operate directly on the codebase. You use `Task` and `Task*` tools to deploy team members.
- The flow is: Builder implements → Reviewer checks quality → Validator verifies correctness.

### Team Members

- Builder
  - Name: builder-connection
  - Role: Implement connection module, diagnostics, connection string parser, and integrate with existing code
  - Agent Type: builder
  - Resume: true
- Reviewer
  - Name: reviewer-connection
  - Role: Code quality and C11 best practices review
  - Agent Type: reviewer
  - Resume: false
- Validator
  - Name: validator-connection
  - Role: Build verification and behavioral correctness
  - Agent Type: validator
  - Resume: false

## Step by Step Tasks

- Execute every step in order, top to bottom.
- Before you start, run `TaskCreate` to create the initial task list.

### 1. Implement Diagnostic Record System
- **Task ID**: implement-diagnostics
- **Depends On**: none
- **Assigned To**: builder-connection
- **Agent Type**: builder
- **Parallel**: false
- Create `src/diagnostics.h` with DiagnosticRecord and DiagnosticRecords structs as described in Code Examples
- Create `src/diagnostics.c` implementing:
  - `diagnostics_clear(DiagnosticRecords *records)` — reset record count, free message strings
  - `diagnostics_add_record(DiagnosticRecords *records, const char *sqlstate, int native_error, const char *message)` — add a record (up to MAX_DIAGNOSTIC_RECORDS)
  - `diagnostics_get_record(const DiagnosticRecords *records, int record_number, ...)` — retrieve record by 1-based index
- This is a reusable subsystem — environment and connection handles will both embed DiagnosticRecords

### 2. Update Environment Module for Connection Tracking
- **Task ID**: update-environment
- **Depends On**: implement-diagnostics
- **Assigned To**: builder-connection
- **Agent Type**: builder
- **Parallel**: false
- Update `src/environment.h`: add `DiagnosticRecords diagnostics` field to OdbcEnvironment, add connections array and connection_count
- Update `src/environment.c`: zero-init new fields in environment_allocate, clean diagnostics in environment_free
- Add functions: `environment_add_connection(OdbcEnvironment *, struct OdbcConnection *)` and `environment_remove_connection(OdbcEnvironment *, struct OdbcConnection *)`
- Validate that environment cannot be freed while connections exist (return SQL_ERROR with diagnostic)

### 3. Implement Connection Handle and Lifecycle
- **Task ID**: implement-connection-handle
- **Depends On**: update-environment
- **Assigned To**: builder-connection
- **Agent Type**: builder
- **Parallel**: false
- Create `src/connection.h` with:
  - ConnectionState enum (NOT_CONNECTED, CONNECTED, BROKEN, EXECUTING)
  - ConnectionInfo struct (server, port, database, username, password, sslmode, application_name, connect_timeout)
  - OdbcConnection struct (magic_number, state, parent_environment, libpq_connection, info, diagnostics, autocommit)
  - Declare: `connection_allocate(OdbcEnvironment *env, SQLHANDLE *output_handle)`
  - Declare: `connection_free(SQLHANDLE handle)`
  - Declare: `connection_connect(OdbcConnection *conn)` — use libpq to connect based on info
  - Declare: `connection_disconnect(OdbcConnection *conn)`
  - Declare: `connection_info_clear(ConnectionInfo *info)` — zero-out / free password
- Create `src/connection.c` implementing:
  - `connection_allocate` — malloc, zero-init, set magic, set state to NOT_CONNECTED, set autocommit=true, link to parent env via environment_add_connection
  - `connection_free` — verify state is NOT_CONNECTED or BROKEN (else error), unlink from env via environment_remove_connection, clear magic, free password, free struct
  - `connection_connect` — build libpq connection string from ConnectionInfo, call PQconnectdb, check PQstatus, store PGconn*, set state to CONNECTED, parse server version
  - `connection_disconnect` — call PQfinish, set state to NOT_CONNECTED, null PGconn*
  - `connection_info_clear` — zero server/port/database/username, free and null password

### 4. Implement Connection String Parser
- **Task ID**: implement-connection-string-parser
- **Depends On**: implement-connection-handle
- **Assigned To**: builder-connection
- **Agent Type**: builder
- **Parallel**: false
- Create `src/connection_string.h` declaring:
  - `connection_string_parse(const char *odbc_string, SQLSMALLINT length, ConnectionInfo *out_info)` — returns true on success
  - `connection_info_to_libpq_string(const ConnectionInfo *info, char *buffer, size_t buffer_size)` — returns true on success
- Create `src/connection_string.c` implementing:
  - Parser for `Key=Value;Key=Value` format (semicolon-separated, case-insensitive keys)
  - Support braces for values containing semicolons: `PWD={my;pass}`
  - Recognized keys with aliases: Server/Servername, Port, Database/DB, UID/Username/User, PWD/Password, SSLmode, ApplicationName/Application_Name, Timeout/Connect_Timeout, DSN (stored but DSN lookup is not implemented yet — just store the name)
  - DSN key: for now, just store the DSN name in a field; actual ODBC.INI lookup is a future feature (add a comment noting this)
  - Builder function that produces: `host=X port=Y dbname=Z user=U password=P sslmode=S application_name=A connect_timeout=T` (only non-empty fields)

### 5. Wire Up ODBC API Layer
- **Task ID**: wire-odbc-api
- **Depends On**: implement-connection-string-parser
- **Assigned To**: builder-connection
- **Agent Type**: builder
- **Parallel**: false
- Update `src/odbc_api.c`:
  - SQLAllocHandle for SQL_HANDLE_DBC: validate input_handle is a valid environment (check magic), call connection_allocate
  - SQLFreeHandle for SQL_HANDLE_DBC: validate handle magic, call connection_free
  - Implement SQLConnect: cast handle, populate ConnectionInfo from DSN/UID/PWD args (for now, DSN is used as database name since we don't have DSN registry lookup), call connection_connect. On failure set diagnostic.
  - Implement SQLDriverConnect: cast handle, call connection_string_parse, call connection_connect. On failure set diagnostic. Build output connection string if buffer provided.
  - Implement SQLDisconnect: validate state is CONNECTED, call connection_disconnect
  - SQLGetDiagRec: determine handle type, get DiagnosticRecords from handle, return requested record
  - SQLGetDiagField: support SQL_DIAG_NUMBER (record count), SQL_DIAG_SQLSTATE, SQL_DIAG_NATIVE, SQL_DIAG_MESSAGE_TEXT
- Add new ODBC exports: SQLConnect, SQLDisconnect
- Update `psqlodbc2.def`: add SQLConnect, SQLDisconnect with ordinals
- Update `src/meson.build`: add new source files (connection.c, connection_string.c, diagnostics.c), add libpq dependency
- Update root `meson.build`: find libpq dependency via pkg-config ('libpq')

### 6. Write Tests
- **Task ID**: implement-tests
- **Depends On**: wire-odbc-api
- **Assigned To**: builder-connection
- **Agent Type**: builder
- **Parallel**: false
- Create `tests/test_connection_string.c`:
  - Test parsing standard connection string: `Server=localhost;Port=5432;Database=testdb;UID=user;PWD=pass`
  - Test case-insensitive keys: `SERVER=localhost;port=5432`
  - Test brace-enclosed values: `PWD={pass;word}`
  - Test missing values handled gracefully
  - Test building libpq string from ConnectionInfo
- Create `tests/test_connection_lifecycle.c`:
  - dlopen driver, allocate env, allocate connection, verify handle is valid
  - Free connection, free env — verify no leaks
  - Attempt to free env while connection exists — verify SQL_ERROR
  - Attempt to disconnect when not connected — verify appropriate behavior
- Update `tests/meson.build`: add both new test executables
- The connection string tests do NOT require a live database (pure parsing tests)
- The lifecycle tests do NOT require a live database (handle management only)

### 7. Review Code Quality
- **Task ID**: review-code-quality
- **Depends On**: implement-tests
- **Assigned To**: reviewer-connection
- **Agent Type**: reviewer
- **Parallel**: false
- Review all new and modified files for C11 compliance
- Verify naming conventions (descriptive names, no abbreviations)
- Check ODBC API functions have doc comments with spec URLs
- Verify no raw platform ifdefs outside platform/ and driver_main.c
- Check memory safety: password cleared before free, no buffer overflows in string parsing
- Check that connection_string parser handles edge cases (empty string, null, no semicolons, trailing semicolons)
- Verify diagnostic record system correctly manages heap-allocated message strings

### 8. Validate Build and Tests
- **Task ID**: validate-all
- **Depends On**: review-code-quality
- **Assigned To**: validator-connection
- **Agent Type**: validator
- **Parallel**: false
- Run `meson setup builddir` (with reconfigure if needed) — must find libpq
- Run `meson compile -C builddir` — must compile with no errors or warnings
- Run `meson test -C builddir` — all tests must pass
- Verify exports: SQLConnect, SQLDisconnect, SQLDriverConnect are exported
- Verify the existing driver_load test still passes (backward compatible)

## Acceptance Criteria
- `meson setup builddir` configures successfully, finding both ODBC and libpq dependencies
- `meson compile -C builddir` produces the shared library with zero errors/warnings
- `meson test -C builddir` — all tests pass (connection string parsing, handle lifecycle, original driver_load test)
- SQLAllocHandle(SQL_HANDLE_DBC) returns SQL_SUCCESS when given a valid environment handle
- SQLFreeHandle(SQL_HANDLE_DBC) returns SQL_SUCCESS for an unconnected connection handle
- SQLDriverConnect with a valid connection string to a running PostgreSQL returns SQL_SUCCESS (tested manually; automated test skips if no server)
- SQLDisconnect returns SQL_SUCCESS on a connected handle
- SQLGetDiagRec returns meaningful error messages after a failed connection attempt
- Password is securely cleared (memset to zero) before memory is freed
- No memory leaks in handle create/destroy cycle

## Validation Commands
- `meson setup builddir --reconfigure` - Reconfigure build (picks up new deps)
- `meson compile -C builddir` - Compile
- `meson test -C builddir` - Run tests
- `nm -gU builddir/src/libpsqlodbc2w.dylib | grep SQL` - Verify exports (macOS)

## Notes
- The original psqlodbc uses DSN registry lookup (ODBC.INI on Unix, Windows registry on Windows) to resolve DSN names to connection parameters. For this initial implementation, DSN lookup is NOT implemented. If a user passes `DSN=mydsn` we just store it; the actual lookup will be a future enhancement. SQLConnect uses the DSN parameter as the database name as a simplified fallback.
- The original psqlodbc has extensive connection option handling (~50 ConnInfo fields). We support only the essential 8 parameters. Additional options will be added as needed when implementing features that require them.
- libpq handles SSL/TLS negotiation, so we just pass sslmode through to libpq.
- The original driver has Windows dialog support for prompting missing credentials. We skip this entirely — SQL_DRIVER_PROMPT returns SQL_ERROR on non-Windows and is not implemented yet.
- Thread safety: the initial implementation does not add mutexes to the connection handle. This will be addressed when multithreading support is implemented.
- The password field is heap-allocated separately from the struct so we can memset it to zero before freeing, avoiding password remnants in memory.
