# Plan: Bulk Operations, Bookmarks, and Positioned Updates

## Task Description
Implement bookmarks (row identifiers), positioned updates/deletes via SQLSetPos, keyset-driven updatable cursors, and SQLBulkOperations. Depends on scrollable cursors being in place.

## Objective
When complete, these regression tests pass: `bookmark`, `bulkoperations`, `positioned-update`, `cursor-block-delete`. (`ard-bookmark-oom` already passes.)

## Problem Statement
Updatable cursors let applications modify the current row without writing UPDATE/DELETE SQL. PostgreSQL has no native updatable cursors, so the driver emulates them: it captures each row's `ctid` (physical row id) as a keyset, and positioned operations become `UPDATE/DELETE ... WHERE ctid = '(block,offset)'`. Bookmarks are opaque row identifiers the app saves and re-navigates to.

## Solution Approach
1. **Bookmarks** — column 0 as SQL_C_VARBOOKMARK (variable-length) or SQL_INTEGER. Retrieve via SQLGetData(0) or SQLBindCol(0). Navigate via SQLFetchScroll(SQL_FETCH_BOOKMARK, offset) using SQL_ATTR_FETCH_BOOKMARK_PTR.
2. **Keyset** — for keyset-driven cursors, add the `ctid` (and possibly a unique key) to the SELECT so each row carries its physical identifier.
3. **SQLSetPos** — SQL_UPDATE/SQL_DELETE/SQL_ADD/SQL_REFRESH. UPDATE/DELETE build `... WHERE ctid = ?` from the current row's keyset; ADD inserts; REFRESH re-reads.
4. **SQLBulkOperations** — SQL_UPDATE_BY_BOOKMARK, SQL_DELETE_BY_BOOKMARK, SQL_ADD, SQL_FETCH_BY_BOOKMARK operate on arrays of bookmarks (rowset).
5. **Block cursors** — SQL_ATTR_ROW_ARRAY_SIZE fetches multiple rows into bound arrays.

## Relevant Files

### Original Reference (~/projects/psqlodbc)
- `results.c` — `PGAPI_SetPos`, `SC_pos_update`, `SC_pos_delete`, `SC_pos_add`, `PGAPI_BulkOperations`, bookmark handling (`SC_Create_bookmark`, `SC_Resolve_bookmark`), keyset build
- `statement.h` — bookmark macros (`SC_make_int4_bookmark`, `SC_resolve_int4_bookmark`)
- `test/src/bookmark-test.c`, `bulkoperations-test.c`, `positioned-update-test.c`, `cursor-block-delete-test.c`

### Modified Files
- `src/results.c` — SetPos/BulkOperations, bookmark retrieval, block fetch into arrays
- `src/statement.h/.c` — keyset storage, bookmark mode, row array size
- `src/odbc_api.c` — SQLSetPos, SQLBulkOperations exports; column 0 handling in SQLGetData/SQLBindCol
- `src/connection.c/.h` — UpdatableCursors option
- `psqlodbc2.def` — new exports

## Implementation Phases

### Phase 1: Bookmarks (fixes bookmark)
- Column 0 as SQL_C_VARBOOKMARK: return the row's identifier (use row index or ctid)
- SQLBindCol(0) and SQLGetData(0) paths
- SQLFetchScroll(SQL_FETCH_BOOKMARK, offset) with SQL_ATTR_FETCH_BOOKMARK_PTR
- Requires a static/scrollable cursor (from the cursors spec)

### Phase 2: Keyset + SQLSetPos (fixes positioned-update, cursor-block-delete)
- Add ctid to keyset-driven cursor SELECTs
- SQLSetPos SQL_UPDATE/SQL_DELETE/SQL_REFRESH via ctid WHERE clause
- SQL_ATTR_ROW_ARRAY_SIZE block fetch, SQL_ATTR_ROWS_FETCHED_PTR
- Savepoint/rollback logging for positioned ops

### Phase 3: SQLBulkOperations (fixes bulkoperations)
- SQL_ADD, SQL_UPDATE_BY_BOOKMARK, SQL_DELETE_BY_BOOKMARK, SQL_FETCH_BY_BOOKMARK
- Array-of-bookmarks operation

## Team Orchestration
- Builder implements → Reviewer checks quality → Validator verifies correctness.

### Team Members
- Builder
  - Name: builder-bulk
  - Role: Implement bookmarks, SetPos, BulkOperations, keyset cursors
  - Agent Type: builder
  - Resume: true
- Reviewer
  - Name: reviewer-bulk
  - Role: Code quality and C11 best practices review
  - Agent Type: reviewer
  - Resume: false
- Validator
  - Name: validator-bulk
  - Role: Build and regression verification
  - Agent Type: validator
  - Resume: false

## Step by Step Tasks

### 1. Bookmarks
- **Task ID**: implement-bookmarks
- **Depends On**: none (but requires scrollable-cursors spec already built)
- **Assigned To**: builder-bulk
- **Agent Type**: builder
- **Parallel**: false
- Column 0 bookmark retrieval and navigation
- Verify bookmark passes

### 2. Keyset + SQLSetPos
- **Task ID**: implement-setpos
- **Depends On**: implement-bookmarks
- **Assigned To**: builder-bulk
- **Agent Type**: builder
- **Parallel**: false
- ctid keyset, positioned update/delete/refresh, block fetch
- Verify positioned-update, cursor-block-delete pass

### 3. SQLBulkOperations
- **Task ID**: implement-bulkops
- **Depends On**: implement-setpos
- **Assigned To**: builder-bulk
- **Agent Type**: builder
- **Parallel**: false
- Bulk add/update/delete/fetch by bookmark
- Verify bulkoperations passes

### 4. Validate
- **Task ID**: validate-all
- **Depends On**: implement-bulkops
- **Assigned To**: validator-bulk
- **Agent Type**: validator
- **Parallel**: false
- Build, unit tests, 4 target regression tests plus currently-passing suite

## Acceptance Criteria
- `bookmark`, `bulkoperations`, `positioned-update`, `cursor-block-delete` pass
- No regression in currently-passing tests
- Positioned update/delete correctly modify the right row via ctid

## Validation Commands
- `meson compile -C builddir`
- `meson test -C builddir`
- `export PATH="/usr/local/pgsql/18/bin:$PATH" && ./regress/run_regression.sh bookmark bulkoperations positioned-update cursor-block-delete`

## Notes
- DEPENDS ON the scrollable-cursors spec — build that first.
- ctid is PostgreSQL's physical row identifier `(block,offset)`; it changes on UPDATE, so keyset cursors re-read after modification.
- Bookmarks can be as simple as the 1-based row index into the cached result for static cursors; the original uses ctid-based keysets for keyset-driven cursors.
- This is the most complex cluster; positioned operations need transaction/savepoint handling to be correct.
