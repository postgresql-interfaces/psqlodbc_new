# Plan: Scrollable Cursors and SQLFetchScroll

## Task Description
Implement scrollable cursor support: the `SQLFetchScroll`/`SQLExtendedFetch` engine with all fetch orientations, cursor-type statement attributes, cursor naming, and the server-side DECLARE/FETCH mode. This is the foundation for the cursor family of regression tests.

## Objective
When complete, these regression tests pass: `cursors`, `cursor-movement`, `cursor-scrollable`, `cursor-commit`, `cursor-name`, `declare-fetch-commit`, `declare-fetch-block`. (Positioned update/delete and bookmarks are covered in the separate bulk-operations spec.)

## Problem Statement
The driver currently only supports forward-only `SQLFetch`. It caches the entire PGresult client-side already (via PQexec), so scrollable navigation over that cache is achievable. Server-side DECLARE/FETCH mode (UseDeclareFetch) is needed for the declare-fetch tests and for large result chunking.

## Solution Approach
1. **Client-side scrollable engine** — implement a single `extended_fetch` function that handles all orientations (NEXT, PRIOR, FIRST, LAST, ABSOLUTE, RELATIVE) over the cached PGresult by moving `current_row_position`. Forward-only cursors reject non-NEXT.
2. **SQLFetchScroll / SQLExtendedFetch** — export both, backed by extended_fetch.
3. **Cursor type attributes** — SQL_ATTR_CURSOR_TYPE (FORWARD_ONLY/STATIC), SQL_ATTR_CURSOR_SCROLLABLE (maps SCROLLABLE→STATIC, NONSCROLLABLE→FORWARD_ONLY). Already partially stubbed in statement attributes.
4. **Cursor naming** — SQLSetCursorName/SQLGetCursorName with auto-generated "SQL_CUR..." names, length validation (SQLSTATE 34000).
5. **Server-side DECLARE/FETCH** — when UseDeclareFetch=1, execute SELECT by issuing `DECLARE <name> [SCROLL] CURSOR [WITH HOLD] FOR <query>` inside a transaction, then `FETCH`/`MOVE` in chunks of the Fetch size. Cursor survival across COMMIT depends on WITH HOLD (Protocol=7.4-2).
6. **Cursor GetInfo** — SQL_CURSOR_COMMIT_BEHAVIOR, SQL_CURSOR_ROLLBACK_BEHAVIOR, SQL_MAX_CURSOR_NAME_LEN.

## Relevant Files

### Original Reference (~/projects/psqlodbc)
- `results.c` — `PGAPI_ExtendedFetch` (the orientation engine), `getNthValid`, `move_cursor_position_if_needed`
- `qresult.c` — `QR_next_tuple` (server MOVE/FETCH)
- `convert.c` — DECLARE CURSOR prepend logic (~line 3089)
- `statement.c` — cursor-type resolution, FETCH command building
- `test/src/cursor*-test.c`, `declare-fetch-*-test.c`

### New Files (this project)
- (Extend existing results.c/statement.c; no new module strictly required, but a `cursor.c` could hold the DECLARE/FETCH mode)

### Modified Files
- `src/results.c` — extended_fetch orientation engine
- `src/odbc_api.c` — SQLFetchScroll, SQLExtendedFetch, SQLSetCursorName, SQLGetCursorName exports; cursor GetInfo values
- `src/statement.h/.c` — cursor name storage, cursor type resolution, DECLARE/FETCH state
- `src/connection.c/.h` — UseDeclareFetch, Fetch size options (parse in connection_string.c)
- `psqlodbc2.def` — new exports

## Implementation Phases

### Phase 1: Client-side scrollable fetch (fixes cursor-movement, cursor-scrollable)
- Implement extended_fetch over the cached PGresult: NEXT/PRIOR/FIRST/LAST/ABSOLUTE/RELATIVE
- Track cursor position; handle BOF/EOF (return SQL_NO_DATA)
- Forward-only rejects non-NEXT with SQLSTATE HY106
- Export SQLFetchScroll and SQLExtendedFetch
- Wire SQL_ATTR_CURSOR_SCROLLABLE / SQL_ATTR_CURSOR_TYPE to enable scrolling

### Phase 2: Cursor naming (fixes cursor-name)
- SQLSetCursorName / SQLGetCursorName
- Auto-generate "SQL_CUR<n>" names
- Reject over-length names (SQLSTATE 34000)
- SQL_MAX_CURSOR_NAME_LEN in SQLGetInfo

### Phase 3: Server-side DECLARE/FETCH (fixes cursors, cursor-commit, declare-fetch-commit, declare-fetch-block)
- Parse UseDeclareFetch and Fetch connection options
- When enabled, wrap SELECT in DECLARE CURSOR and fetch in chunks
- Handle COMMIT behavior (WITH HOLD when Protocol=7.4-2)
- Block cursors: SQL_ATTR_ROW_ARRAY_SIZE, SQL_ATTR_ROWS_FETCHED_PTR
- Cursor commit/rollback behavior GetInfo

## Team Orchestration
- Builder implements → Reviewer checks quality → Validator verifies correctness.

### Team Members
- Builder
  - Name: builder-cursors
  - Role: Implement scrollable cursor engine and DECLARE/FETCH mode
  - Agent Type: builder
  - Resume: true
- Reviewer
  - Name: reviewer-cursors
  - Role: Code quality and C11 best practices review
  - Agent Type: reviewer
  - Resume: false
- Validator
  - Name: validator-cursors
  - Role: Build and regression verification
  - Agent Type: validator
  - Resume: false

## Step by Step Tasks

### 1. Client-side Scrollable Fetch
- **Task ID**: implement-scroll-engine
- **Depends On**: none
- **Assigned To**: builder-cursors
- **Agent Type**: builder
- **Parallel**: false
- extended_fetch with all orientations; SQLFetchScroll/SQLExtendedFetch exports
- Verify cursor-movement, cursor-scrollable pass

### 2. Cursor Naming
- **Task ID**: implement-cursor-name
- **Depends On**: implement-scroll-engine
- **Assigned To**: builder-cursors
- **Agent Type**: builder
- **Parallel**: false
- SQLSetCursorName/SQLGetCursorName, length validation
- Verify cursor-name passes

### 3. Server-side DECLARE/FETCH
- **Task ID**: implement-declare-fetch
- **Depends On**: implement-scroll-engine
- **Assigned To**: builder-cursors
- **Agent Type**: builder
- **Parallel**: false
- UseDeclareFetch/Fetch options, DECLARE CURSOR wrapping, chunked FETCH
- Verify cursors, cursor-commit, declare-fetch-commit, declare-fetch-block pass

### 4. Validate
- **Task ID**: validate-all
- **Depends On**: implement-cursor-name, implement-declare-fetch
- **Assigned To**: validator-cursors
- **Agent Type**: validator
- **Parallel**: false
- Build, unit tests, all 7 target regression tests plus currently-passing suite

## Acceptance Criteria
- `cursors`, `cursor-movement`, `cursor-scrollable`, `cursor-commit`, `cursor-name`, `declare-fetch-commit`, `declare-fetch-block` pass
- No regression in currently-passing tests
- SQLFetchScroll handles all orientations; forward-only rejects non-NEXT
- Cursor names auto-generate and validate length

## Validation Commands
- `meson compile -C builddir`
- `meson test -C builddir`
- `export PATH="/usr/local/pgsql/18/bin:$PATH" && ./regress/run_regression.sh cursors cursor-movement cursor-scrollable cursor-commit cursor-name declare-fetch-commit declare-fetch-block`

## Notes
- The client-side engine is simpler and unlocks the most tests — do it first.
- For DECLARE/FETCH, cursors require an open transaction; when autocommit is on, the driver opens an implicit transaction for the cursor and closes it appropriately.
- Positioned updates (SQLSetPos UPDATE/DELETE) and keyset-driven cursors are OUT OF SCOPE here — see the bulk-operations spec.
