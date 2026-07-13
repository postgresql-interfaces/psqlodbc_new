# Plan: Data-at-Execution and Large Objects

## Task Description
Implement the SQL_DATA_AT_EXEC streaming mechanism (SQLParamData/SQLPutData) for both in-memory binary parameters and PostgreSQL large objects, plus inline large-object insert/retrieve.

## Objective
When complete, these regression tests pass: `dataatexecution`, `large-object`, `large-object-data-at-exec`.

## Problem Statement
Applications stream large parameter values in chunks rather than providing them all at once. When a bound parameter has `StrLen_or_Ind = SQL_DATA_AT_EXEC`, SQLExecute returns SQL_NEED_DATA; the app then loops calling SQLParamData (which identifies the parameter) and SQLPutData (which supplies chunks). For SQL_LONGVARBINARY, the data goes into a PostgreSQL large object via the lo_* API; for SQL_VARBINARY, it's buffered in memory.

## Solution Approach
1. **Data-at-exec detection** — in statement execute, detect any bound parameter with SQL_DATA_AT_EXEC (or ≤ SQL_LEN_DATA_AT_EXEC_OFFSET). Return SQL_NEED_DATA.
2. **SQLParamData** — return the value pointer of the next parameter needing data; when all are supplied, execute the statement.
3. **SQLPutData** — append a chunk to the current parameter's buffer (in-memory realloc) or stream to a large object.
4. **Large object API** — wrap PostgreSQL's fast-path functions: lo_creat, lo_open, lowrite, loread, lo_close via libpq's `lo_*` client functions (`lo_creat`, `lo_open`, `lo_write`, `lo_read`, `lo_close` from libpq-fs.h, or PQfn).
5. **Inline LO** — SQL_LONGVARBINARY parameter supplied in one buffer: create the LO, write the bytes, store the OID.
6. **Error guards** — SQLPutData with negative length → HY024; append after SQL_NULL_DATA → HY010.

## Relevant Files

### Original Reference (~/projects/psqlodbc)
- `execute.c` — `PGAPI_Execute` (SQL_NEED_DATA detection), `PGAPI_ParamData`, `PGAPI_PutData` (handling_lo branch)
- `lobj.c` — `odbc_lo_creat`, `odbc_lo_open`, `odbc_lo_write`, `odbc_lo_read`, `odbc_lo_close` (via CC_send_function)
- `test/src/dataatexecution-test.c`, `large-object-test.c`, `large-object-data-at-exec-test.c`

### New Files (this project)
- `src/large_object.h/.c` — PostgreSQL large object client wrappers

### Modified Files
- `src/statement.h/.c` — data-at-exec state machine, per-parameter EXEC_buffer
- `src/parameter.c` — recognize SQL_DATA_AT_EXEC indicator
- `src/odbc_api.c` — SQLParamData, SQLPutData exports; SQLExecute returns SQL_NEED_DATA
- `src/type_mapping.c` — SQL_LONGVARBINARY → LO handling
- `psqlodbc2.def` — new exports

## Implementation Phases

### Phase 1: In-memory data-at-exec (fixes dataatexecution)
- Detect SQL_DATA_AT_EXEC in bound params; SQLExecute returns SQL_NEED_DATA
- SQLParamData returns next param; SQLPutData appends to a growing heap buffer
- Final SQLParamData executes with the assembled buffer
- Error guards: HY024 (negative length), HY010 (append after null)
- Works with array/paramset binding

### Phase 2: Large object client API (foundation)
- src/large_object.c wrapping lo_creat/lo_open/lo_write/lo_read/lo_close
- Use libpq's large-object functions inside a transaction

### Phase 3: Large object parameters (fixes large-object, large-object-data-at-exec)
- Inline SQL_LONGVARBINARY: create LO, write buffer, store OID
- Data-at-exec SQL_LONGVARBINARY: stream chunks via lo_write
- SQLGetData(SQL_C_BINARY) reads the LO back

## Team Orchestration
- Builder implements → Reviewer checks quality → Validator verifies correctness.

### Team Members
- Builder
  - Name: builder-lob
  - Role: Implement data-at-exec state machine and large object support
  - Agent Type: builder
  - Resume: true
- Reviewer
  - Name: reviewer-lob
  - Role: Code quality and C11 best practices review
  - Agent Type: reviewer
  - Resume: false
- Validator
  - Name: validator-lob
  - Role: Build and regression verification
  - Agent Type: validator
  - Resume: false

## Step by Step Tasks

### 1. In-memory Data-at-Execution
- **Task ID**: implement-data-at-exec
- **Depends On**: none
- **Assigned To**: builder-lob
- **Agent Type**: builder
- **Parallel**: false
- SQL_NEED_DATA flow, SQLParamData, SQLPutData with heap buffering
- Verify dataatexecution passes

### 2. Large Object Client API
- **Task ID**: implement-lo-api
- **Depends On**: none
- **Assigned To**: builder-lob
- **Agent Type**: builder
- **Parallel**: true
- src/large_object.c with lo_* wrappers

### 3. Large Object Parameters
- **Task ID**: implement-lo-params
- **Depends On**: implement-data-at-exec, implement-lo-api
- **Assigned To**: builder-lob
- **Agent Type**: builder
- **Parallel**: false
- Inline and streamed LO insert, LO read-back
- Verify large-object, large-object-data-at-exec pass

### 4. Validate
- **Task ID**: validate-all
- **Depends On**: implement-lo-params
- **Assigned To**: validator-lob
- **Agent Type**: validator
- **Parallel**: false
- Build, unit tests, 3 target regression tests plus currently-passing suite

## Acceptance Criteria
- `dataatexecution`, `large-object`, `large-object-data-at-exec` pass
- No regression in currently-passing tests
- SQLPutData negative length returns HY024; append after null returns HY010
- Large objects created, written, and read back correctly

## Validation Commands
- `meson compile -C builddir`
- `meson test -C builddir`
- `export PATH="/usr/local/pgsql/18/bin:$PATH" && ./regress/run_regression.sh dataatexecution large-object large-object-data-at-exec`

## Notes
- libpq provides `lo_creat`, `lo_open`, `lo_write`, `lo_read`, `lo_close`, `lo_lseek` directly (declared in libpq/libpq-fs.h). These must run inside a transaction block.
- Large objects need `INV_READ|INV_WRITE` mode constants (0x40000 | 0x20000).
- The data-at-exec state machine is the reusable core; large objects build on top of it plus the lo_* API.
