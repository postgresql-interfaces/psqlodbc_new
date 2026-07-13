# Plan: Statement Module

## Task Description
Implement the statement module for the psqlodbc2 driver: statement handle allocation/deallocation (SQL_HANDLE_STMT in SQLAllocHandle/SQLFreeHandle), SQLFreeStmt with all its option modes (SQL_CLOSE, SQL_DROP, SQL_UNBIND, SQL_RESET_PARAMS), SQLPrepare for storing a SQL statement for later execution, SQLExecute for executing a previously prepared statement, and SQLExecDirect for the combined prepare-and-execute path. The connection handle must track its child statements so it cannot be freed while statements are still allocated. The implementation sends SQL to PostgreSQL via libpq's PQexec (for direct execution) and PQprepare/PQexecPrepared (for prepared statements).

## Objective
When this plan is complete:
1. Applications can allocate a statement handle (SQLAllocHandle with SQL_HANDLE_STMT)
2. Applications can prepare SQL with SQLPrepare and execute it with SQLExecute
3. Applications can use SQLExecDirect for one-shot query execution
4. Applications can free/reset statements with SQLFreeStmt (SQL_DROP, SQL_CLOSE, SQL_UNBIND, SQL_RESET_PARAMS)
5. Applications can free statement handles with SQLFreeHandle(SQL_HANDLE_STMT)
6. The connection tracks its child statements and cannot be disconnected/freed while statements with open results exist
7. Query results (PGresult) are captured and stored for later retrieval by the results module
8. Statement diagnostics are populated on failure (queryable via SQLGetDiagRec)

## Problem Statement
The driver currently supports environment and connection handles but has no mechanism for executing SQL. Statements are the ODBC abstraction for query execution — every SELECT, INSERT, UPDATE, DELETE, and DDL command flows through a statement handle. Without this module, the driver cannot perform any actual database work.

## Solution Approach
Create a statement module following the existing handle pattern (magic number, parent linkage, diagnostics) with:

1. **Statement handle struct** — holds the parent connection reference, statement state, SQL text, prepared-statement name (for server-side prepared statements), the PGresult pointer from libpq, and diagnostic records
2. **Statement state machine** — mirrors the ODBC-defined states: ALLOCATED → PREPARED → EXECUTED → HAS_CURSOR (for SELECT results). State transitions enforce correct API call sequencing.
3. **Connection-statement linkage** — connection tracks its child statements in an array (same pattern as environment-connection). Disconnection is blocked while statements with active results exist.
4. **Execution strategy**:
   - SQLExecDirect: uses `PQexec` for simplicity (sends SQL directly to server)
   - SQLPrepare + SQLExecute: uses `PQprepare` (first call) + `PQexecPrepared` (subsequent calls) for server-side prepared statements. The prepared statement name is auto-generated as `_psqlodbc2_stmt_N`.
5. **Result handling**: PGresult is stored on the statement handle. The statement module does NOT interpret column data — that will be the results module's job. It only captures the result and determines whether the query produced a result set (SELECT) or an affected-row count (INSERT/UPDATE/DELETE).
6. **SQLFreeStmt options**:
   - SQL_DROP: fully deallocate the statement (same as SQLFreeHandle)
   - SQL_CLOSE: discard results but keep the statement allocated and prepared text intact
   - SQL_UNBIND: reset column bindings (stub for now — bindings not yet implemented)
   - SQL_RESET_PARAMS: reset parameter bindings (stub for now — parameters not yet implemented)

The original psqlodbc has an extremely complex StatementClass with ~80 fields covering cursors, bookmarks, descriptors, delegates, and batch execution. This initial implementation focuses only on the essential lifecycle and execution path. Parameter binding, cursor management, and descriptors will be added in subsequent modules.

## Relevant Files

### Original Reference (~/projects/psqlodbc)
- `statement.h` — StatementClass struct, STMT_Status enum, statement types, SC_* macros and prototypes
- `statement.c` — SC_Constructor, SC_Destructor, PGAPI_AllocStmt, PGAPI_FreeStmt, SC_recycle_statement
- `execute.c` — PGAPI_Prepare, PGAPI_Execute, PGAPI_ExecDirect, query execution via libpq
- `pgapi30.c` — ODBC 3.0 wrappers (SQLAllocHandle/SQLFreeHandle dispatch for SQL_HANDLE_STMT)

### New Files (this project)
- `src/statement.h` — OdbcStatement struct, StatementState enum, statement function declarations
- `src/statement.c` — Statement handle alloc/free, prepare/execute/exec-direct implementation, result lifecycle

### Modified Files
- `src/connection.h` — Add statement tracking array and count; add statement management function declarations
- `src/connection.c` — Implement connection_add_statement/connection_remove_statement; block disconnect when open statements exist
- `src/odbc_api.c` — Wire up SQL_HANDLE_STMT in SQLAllocHandle/SQLFreeHandle; implement SQLPrepare, SQLExecute, SQLExecDirect, SQLFreeStmt
- `src/meson.build` — Add statement.c to driver_sources
- `psqlodbc2.def` — Add SQLPrepare, SQLExecute, SQLExecDirect, SQLFreeStmt exports
- `tests/meson.build` — Add new test executables

## Implementation Phases

### Phase 1: Foundation (Statement Struct + Connection Linkage)
1. Define the statement handle struct with state machine, parent connection, SQL text, PGresult storage, diagnostics
2. Update connection handle to track child statements (array + count)
3. Implement connection_add_statement / connection_remove_statement

### Phase 2: Core Implementation (Prepare + Execute)
1. Implement statement_allocate and statement_free
2. Implement statement_prepare (store SQL text, call PQprepare for server-side preparation)
3. Implement statement_execute (call PQexecPrepared or PQexec depending on preparation state)
4. Implement statement_exec_direct (store SQL text + PQexec in one shot)
5. Implement statement_close_cursor (discard PGresult, reset state to PREPARED or ALLOCATED)
6. Implement SQLFreeStmt option dispatch

### Phase 3: Integration & Polish
1. Wire up all ODBC API entry points in odbc_api.c
2. Update exports in psqlodbc2.def
3. Add statement.c to meson build
4. Write tests: handle lifecycle (dlopen), prepare/execute path (requires live database — can be skipped), state machine enforcement (unit tests)
5. Verify all existing tests still pass

## Code Examples

### Statement state enum:
```c
typedef enum {
    STATEMENT_STATE_ALLOCATED = 0,    /* Handle exists but no SQL set */
    STATEMENT_STATE_PREPARED,         /* SQL text stored; PQprepare sent to server */
    STATEMENT_STATE_EXECUTED,         /* Query executed; result available */
    STATEMENT_STATE_HAS_CURSOR        /* SELECT result with rows available for fetch */
} StatementState;
```

### Statement handle struct:
```c
#define STATEMENT_MAGIC_NUMBER 0x53544D32  /* "STM2" in ASCII */
#define MAX_PREPARED_NAME_LENGTH 64

typedef struct OdbcStatement {
    unsigned int magic_number;           /* Must equal STATEMENT_MAGIC_NUMBER when valid */
    StatementState state;
    OdbcConnection *parent_connection;
    DiagnosticRecords diagnostics;
    char *sql_text;                      /* Heap-allocated; the SQL string from SQLPrepare/SQLExecDirect */
    char prepared_name[MAX_PREPARED_NAME_LENGTH]; /* Server-side prepared statement name (empty if not prepared) */
    bool is_prepared;                    /* True if PQprepare was called for this statement */
    PGresult *current_result;            /* libpq result from last execution (NULL if none) */
    int affected_row_count;              /* Row count for INSERT/UPDATE/DELETE (-1 if unknown) */
    bool has_result_set;                 /* True if the last execution produced a result set (SELECT) */
} OdbcStatement;
```

### Connection statement tracking (additions to connection.h):
```c
#define MAX_STATEMENTS_PER_CONNECTION 256

/* Add these fields to OdbcConnection struct: */
    struct OdbcStatement *statements[MAX_STATEMENTS_PER_CONNECTION];
    int statement_count;

/* Add these function declarations: */
bool connection_add_statement(OdbcConnection *connection, struct OdbcStatement *statement);
bool connection_remove_statement(OdbcConnection *connection, struct OdbcStatement *statement);
```

### Execution flow for SQLExecDirect:
```c
SQLRETURN statement_exec_direct(OdbcStatement *statement, const char *sql_text, SQLINTEGER text_length)
{
    /* 1. Close any existing result */
    /* 2. Store the SQL text on the statement */
    /* 3. Get the PGconn from parent connection */
    /* 4. Call PQexec(pgconn, sql_text) */
    /* 5. Check PQresultStatus: */
    /*    - PGRES_COMMAND_OK → DML/DDL succeeded, store affected rows */
    /*    - PGRES_TUPLES_OK → SELECT succeeded, store PGresult, set has_result_set=true */
    /*    - PGRES_FATAL_ERROR → Add diagnostic from PQresultErrorMessage, return SQL_ERROR */
    /* 6. Update state to EXECUTED or HAS_CURSOR */
    /* 7. Return SQL_SUCCESS */
}
```

### Execution flow for SQLPrepare + SQLExecute:
```c
/* SQLPrepare: */
SQLRETURN statement_prepare(OdbcStatement *statement, const char *sql_text, SQLINTEGER text_length)
{
    /* 1. Close any existing result */
    /* 2. Store the SQL text */
    /* 3. Generate a unique prepared name: "_psqlodbc2_stmt_<counter>" */
    /* 4. Call PQprepare(pgconn, prepared_name, sql_text, 0, NULL) */
    /* 5. Check result — if error, add diagnostic, return SQL_ERROR */
    /* 6. Set is_prepared = true, state = PREPARED */
    /* 7. Return SQL_SUCCESS */
}

/* SQLExecute: */
SQLRETURN statement_execute(OdbcStatement *statement)
{
    /* 1. Verify state is PREPARED (or re-executing after EXECUTED) */
    /* 2. Close any previous result */
    /* 3. Call PQexecPrepared(pgconn, prepared_name, 0, NULL, NULL, NULL, 0) */
    /* 4. Check result status, store PGresult */
    /* 5. Update state */
    /* 6. Return SQL_SUCCESS or SQL_ERROR with diagnostic */
}
```

## Team Orchestration

- You operate as the team lead and orchestrate the team to execute the plan.
- You NEVER operate directly on the codebase. You use `Task` and `Task*` tools to deploy team members.
- The flow is: Builder implements → Reviewer checks quality → Validator verifies correctness.

### Team Members

- Builder
  - Name: builder-statement
  - Role: Implement statement module, connection-statement linkage, ODBC API wiring, and tests
  - Agent Type: builder
  - Resume: true
- Reviewer
  - Name: reviewer-statement
  - Role: Code quality, C11 compliance, ODBC spec correctness, naming and documentation review
  - Agent Type: reviewer
  - Resume: false
- Validator
  - Name: validator-statement
  - Role: Build verification, test execution, behavioral correctness validation
  - Agent Type: validator
  - Resume: false

## Step by Step Tasks

- Execute every step in order, top to bottom.
- Before you start, run `TaskCreate` to create the initial task list.

### 1. Update Connection Handle for Statement Tracking
- **Task ID**: connection-statement-tracking
- **Depends On**: none
- **Assigned To**: builder-statement
- **Agent Type**: builder
- **Parallel**: false
- Add to `src/connection.h`: forward declare `struct OdbcStatement`; add `statements[MAX_STATEMENTS_PER_CONNECTION]` array and `statement_count` field to `OdbcConnection`; add `next_statement_id` counter (used for generating unique prepared statement names); declare `connection_add_statement()` and `connection_remove_statement()`
- Add to `src/connection.c`: implement `connection_add_statement()` — find empty slot, store pointer, increment count, return true/false; implement `connection_remove_statement()` — find and null the slot, decrement count, return true/false
- Update `connection_disconnect()`: check if any statements have open results (state == EXECUTED or HAS_CURSOR). If so, close their cursors first (clear PGresult, reset state), then proceed with disconnect. This matches the original psqlodbc behavior where disconnect implicitly closes cursors.
- Ensure `connection_free()` still works: already blocks if connected, and statements should be freed before the connection (ODBC spec). Add a check: if statement_count > 0, add diagnostic "HY010" and return SQL_ERROR.

### 2. Implement Statement Handle Struct and Lifecycle
- **Task ID**: implement-statement-handle
- **Depends On**: connection-statement-tracking
- **Assigned To**: builder-statement
- **Agent Type**: builder
- **Parallel**: false
- Create `src/statement.h` with:
  - StatementState enum (ALLOCATED, PREPARED, EXECUTED, HAS_CURSOR)
  - STATEMENT_MAGIC_NUMBER definition (0x53544D32 = "STM2")
  - MAX_PREPARED_NAME_LENGTH (64)
  - MAX_STATEMENTS_PER_CONNECTION (256) — or reference from connection.h
  - OdbcStatement struct (magic_number, state, parent_connection, diagnostics, sql_text, prepared_name, is_prepared, current_result, affected_row_count, has_result_set)
  - Declare: `statement_allocate(OdbcConnection *connection, SQLHANDLE *output_handle)`
  - Declare: `statement_free(SQLHANDLE handle)`
  - Declare: `statement_prepare(OdbcStatement *statement, const char *sql_text, SQLINTEGER text_length)`
  - Declare: `statement_execute(OdbcStatement *statement)`
  - Declare: `statement_exec_direct(OdbcStatement *statement, const char *sql_text, SQLINTEGER text_length)`
  - Declare: `statement_close_cursor(OdbcStatement *statement)` — discards result, resets state
  - Declare: `statement_free_stmt(OdbcStatement *statement, SQLUSMALLINT option)` — dispatches SQL_CLOSE/SQL_DROP/SQL_UNBIND/SQL_RESET_PARAMS
- Create `src/statement.c` implementing:
  - `statement_allocate` — calloc, set magic, state=ALLOCATED, link to parent connection, call connection_add_statement, store in output_handle
  - `statement_free` — validate magic, verify no active execution (state != EXECUTING if we add that), call connection_remove_statement, clear PGresult if any, free sql_text, deallocate server-side prepared statement via PQexec("DEALLOCATE name") if is_prepared, clear diagnostics, poison magic, free struct
  - `statement_close_cursor` — if current_result != NULL: PQclear(current_result), set current_result=NULL; reset state to PREPARED (if is_prepared) or ALLOCATED; reset affected_row_count and has_result_set
  - `statement_free_stmt` dispatch: SQL_DROP calls statement_free; SQL_CLOSE calls statement_close_cursor; SQL_UNBIND and SQL_RESET_PARAMS are no-ops for now (parameter/column binding not yet implemented) returning SQL_SUCCESS

### 3. Implement Statement Execution (Prepare + Execute + ExecDirect)
- **Task ID**: implement-statement-execution
- **Depends On**: implement-statement-handle
- **Assigned To**: builder-statement
- **Agent Type**: builder
- **Parallel**: false
- Implement `statement_prepare()`:
  - Close any existing cursor/result via statement_close_cursor
  - Free previous sql_text if any
  - Resolve text_length (handle SQL_NTS)
  - Allocate and copy sql_text
  - Generate unique prepared name: snprintf(prepared_name, MAX_PREPARED_NAME_LENGTH, "_psqlodbc2_stmt_%d", parent_connection->next_statement_id++)
  - Get PGconn from parent_connection->libpq_connection
  - Call PQprepare(pgconn, prepared_name, sql_text, 0, NULL) — no parameter types for now
  - Check result: if PQresultStatus != PGRES_COMMAND_OK, add diagnostic from PQresultErrorMessage, PQclear result, return SQL_ERROR
  - PQclear the prepare result (it's just a status, not data)
  - Set is_prepared = true, state = PREPARED
  - Return SQL_SUCCESS
- Implement `statement_execute()`:
  - Verify is_prepared == true and state is PREPARED or EXECUTED/HAS_CURSOR (re-execution). If not prepared, add diagnostic "HY010" (function sequence error), return SQL_ERROR
  - Close any existing result via statement_close_cursor (but keep prepared state)
  - Call PQexecPrepared(pgconn, prepared_name, 0, NULL, NULL, NULL, 0) — no params for now
  - Handle result (shared helper — see below)
  - Return SQL_SUCCESS or SQL_ERROR
- Implement `statement_exec_direct()`:
  - Close any existing cursor/result
  - Free previous sql_text if any
  - Resolve text_length, allocate and copy sql_text
  - Set is_prepared = false (direct execution does not create a server-side prepared statement)
  - Call PQexec(pgconn, sql_text)
  - Handle result (shared helper)
  - Return SQL_SUCCESS or SQL_ERROR
- Implement shared result handler (static helper function `handle_execution_result`):
  - ExecStatusType status = PQresultStatus(result)
  - PGRES_COMMAND_OK: store affected_row_count from PQcmdTuples (atoi), has_result_set=false, state=EXECUTED, PQclear result (no data to keep), return SQL_SUCCESS
  - PGRES_TUPLES_OK: has_result_set=true, current_result=result (keep it — results module will read from it), affected_row_count = PQntuples, state=HAS_CURSOR, return SQL_SUCCESS
  - PGRES_FATAL_ERROR: add diagnostic ("HY000", PQresultErrorMessage), PQclear result, state stays unchanged, return SQL_ERROR
  - Any other status: treat as error
- IMPORTANT: Every ODBC API function (SQLPrepare, SQLExecute, SQLExecDirect) must have a doc comment with description, parameters, return values, and Microsoft ODBC spec reference URL

### 4. Wire Up ODBC API Layer
- **Task ID**: wire-odbc-api
- **Depends On**: implement-statement-execution
- **Assigned To**: builder-statement
- **Agent Type**: builder
- **Parallel**: false
- Update `src/odbc_api.c`:
  - Add `#include "statement.h"`
  - SQLAllocHandle for SQL_HANDLE_STMT: validate input_handle is a valid connection (check CONNECTION_MAGIC_NUMBER), call statement_allocate
  - SQLFreeHandle for SQL_HANDLE_STMT: validate handle (check STATEMENT_MAGIC_NUMBER), call statement_free
  - Implement SQLPrepare: validate handle, clear diagnostics, call statement_prepare. Doc comment with spec URL.
  - Implement SQLExecute: validate handle, clear diagnostics, call statement_execute. Doc comment with spec URL.
  - Implement SQLExecDirect: validate handle, clear diagnostics, call statement_exec_direct. Doc comment with spec URL.
  - Implement SQLFreeStmt: validate handle, call statement_free_stmt with the option. Doc comment with spec URL.
  - Update get_diagnostics_for_handle to support SQL_HANDLE_STMT (check STATEMENT_MAGIC_NUMBER, return &statement->diagnostics)
- Update `psqlodbc2.def` — add exports:
  - SQLPrepare @9
  - SQLExecute @10
  - SQLExecDirect @11
  - SQLFreeStmt @12
- Update `src/meson.build` — add 'statement.c' to driver_sources

### 5. Write Tests
- **Task ID**: implement-tests
- **Depends On**: wire-odbc-api
- **Assigned To**: builder-statement
- **Agent Type**: builder
- **Parallel**: false
- Create `tests/test_statement_lifecycle.c`:
  - dlopen driver, get function pointers for SQLAllocHandle, SQLFreeHandle, SQLFreeStmt
  - Test: allocate env → allocate connection → allocate statement → verify handle is not NULL
  - Test: free statement → free connection → free env (happy path)
  - Test: allocate statement, attempt to free connection without freeing statement first → expect SQL_ERROR
  - Test: SQLFreeStmt with SQL_CLOSE on a freshly allocated statement → expect SQL_SUCCESS (no-op)
  - Test: SQLFreeStmt with SQL_DROP → statement is freed
  - Test: allocate maximum statements (or a reasonable number like 10) → verify all succeed
  - Test: SQLFreeStmt with invalid option → expect SQL_ERROR
- Create `tests/test_statement_execution.c` (requires live database — mark as skippable):
  - If environment variable `PSQLODBC2_TEST_DSN` or `PSQLODBC2_TEST_CONNSTR` is not set, print "SKIP: no test database configured" and exit 77 (meson skip code)
  - Connect via SQLDriverConnect using the test connection string
  - Test SQLExecDirect: execute "SELECT 1 AS test_value" → expect SQL_SUCCESS
  - Test SQLPrepare + SQLExecute: prepare "SELECT $1::int" → expect SQL_SUCCESS (note: without param binding this won't work yet — instead test with a parameterless query like "SELECT 42")
  - Test SQLExecDirect with invalid SQL: "SELEC TYPO" → expect SQL_ERROR, verify SQLGetDiagRec returns a meaningful message
  - Test SQLExecDirect with DDL: "CREATE TEMP TABLE test_stmt(id int)" → expect SQL_SUCCESS, affected_row_count irrelevant for DDL
  - Test SQLExecDirect with DML: "INSERT INTO test_stmt VALUES (1)" → expect SQL_SUCCESS
  - Disconnect and clean up
- Update `tests/meson.build`: add both new test executables. The execution test gets the connection string via env var (use `env` parameter in test() call or skip within the test)

### 6. Review Code Quality
- **Task ID**: review-code-quality
- **Depends On**: implement-tests
- **Assigned To**: reviewer-statement
- **Agent Type**: reviewer
- **Parallel**: false
- Review all new and modified files for C11 compliance
- Verify naming conventions: all names descriptive and self-documenting (no abbreviations like `stmt`, `conn` in new code — use `statement`, `connection`)
- Check ODBC API functions have doc comments with spec reference URLs
- Verify memory safety: sql_text freed properly, PGresult always PQclear'd on error paths, no double-free scenarios
- Check state machine: verify all state transitions are correct per ODBC spec
- Check that statement_free deallocates the server-side prepared statement (DEALLOCATE)
- Verify connection cannot be freed while statements exist
- Verify connection disconnect behavior with active statements is reasonable
- Check for resource leaks: what happens if statement_prepare is called twice? (should deallocate the old prepared statement first)

### 7. Validate Build and Tests
- **Task ID**: validate-all
- **Depends On**: review-code-quality
- **Assigned To**: validator-statement
- **Agent Type**: validator
- **Parallel**: false
- Run `meson setup builddir --reconfigure` — must succeed
- Run `meson compile -C builddir` — must compile with zero errors or warnings
- Run `meson test -C builddir` — all tests must pass (statement_lifecycle tests run without a database; statement_execution skips gracefully if no database)
- Verify exports: `nm -gU builddir/src/libpsqlodbc2w.dylib | grep SQL` shows SQLPrepare, SQLExecute, SQLExecDirect, SQLFreeStmt
- Verify existing tests (driver_load, connection_string, connection_lifecycle) still pass
- Readability check: verify all names are descriptive, comments explain intent, ODBC API functions have spec reference URLs
- ODBC API documentation check: verify SQLPrepare, SQLExecute, SQLExecDirect, SQLFreeStmt all have proper doc comments

## Acceptance Criteria
- `meson compile -C builddir` produces the shared library with zero errors/warnings
- `meson test -C builddir` — all tests pass (lifecycle tests run without database; execution tests skip gracefully)
- SQLAllocHandle(SQL_HANDLE_STMT) returns SQL_SUCCESS with a valid connection handle as input
- SQLFreeHandle(SQL_HANDLE_STMT) returns SQL_SUCCESS for an allocated statement
- SQLFreeStmt(SQL_DROP) frees the statement; SQLFreeStmt(SQL_CLOSE) resets result state
- SQLPrepare stores SQL text and creates a server-side prepared statement
- SQLExecute runs a previously prepared statement and captures the result
- SQLExecDirect executes SQL directly and captures the result
- SQLGetDiagRec returns meaningful error messages after execution failures
- Connection cannot be freed while statements are allocated (returns SQL_ERROR with diagnostic)
- PGresult is always properly PQclear'd (no memory leaks)
- Server-side prepared statements are DEALLOCATE'd when the statement handle is freed
- All existing tests continue to pass (backward compatible)
- All ODBC API functions have doc comments with Microsoft spec reference URLs
- All names are descriptive — no cryptic abbreviations

## Validation Commands
- `meson setup builddir --reconfigure` - Reconfigure build (picks up new source files)
- `meson compile -C builddir` - Compile
- `meson test -C builddir` - Run tests
- `nm -gU builddir/src/libpsqlodbc2w.dylib | grep SQL` - Verify exports (macOS)
- `nm -gU builddir/src/libpsqlodbc2w.dylib | grep -c SQL` - Count exports (should be 12)

## Notes
- The original psqlodbc's PGAPI_Execute is extremely complex (~600 lines) because it handles parameter arrays, server-side cursors, batch execution, and locking. Our initial implementation handles only the simplest case: no parameters, no cursors, single execution. Parameter binding and cursor support will come in subsequent modules.
- PQprepare with 0 parameter types lets PostgreSQL infer types. This is acceptable for now since we have no parameter binding yet. When parameter binding is added, we will pass the OID array.
- The `next_statement_id` counter on the connection ensures unique prepared statement names across the connection's lifetime. It never resets — even after a statement is freed, its name is not reused. This avoids any race conditions with the server's prepared statement cache.
- Server-side prepared statements persist until the session ends or they are explicitly DEALLOCATE'd. We DEALLOCATE on statement_free to avoid accumulating dead prepared statements on long-lived connections.
- SQLFreeStmt(SQL_UNBIND) and SQLFreeStmt(SQL_RESET_PARAMS) are no-ops that return SQL_SUCCESS. They will gain real behavior when column binding (results module) and parameter binding are implemented.
- The test for live database execution uses meson's test skip mechanism (exit code 77) so CI passes without a PostgreSQL server. Developers can set PSQLODBC2_TEST_CONNSTR to run the full suite locally.
- For PGRES_TUPLES_OK results, we KEEP the PGresult on the statement handle (do not PQclear). The results module (SQLFetch, SQLGetData) will read from it. It gets PQclear'd on statement_close_cursor, statement_free, or next execution.
