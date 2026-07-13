# Plan: Column Binding (SQLBindCol)

## Task Description
Implement column binding so applications can bind result set columns to C variables before calling SQLFetch. When fetching rows, the driver automatically writes column data into the bound buffers, eliminating the need for per-column SQLGetData calls.

## Objective
When this plan is complete:
1. Applications can call SQLBindCol to bind C variables to result set columns
2. SQLFetch writes column values directly into bound buffers during fetch
3. Indicator/length variables are populated with data length or SQL_NULL_DATA
4. SQLFreeStmt(SQL_UNBIND) clears all column bindings
5. Re-binding and unbinding individual columns works correctly

## Problem Statement
Currently SQLFetch only advances the cursor position — applications must call SQLGetData for each column individually. Column binding is the standard bulk-fetch pattern used by most ODBC applications (including database tools, BI software, and ORMs). Without it, the driver is impractical for real workloads.

## Solution Approach
1. **ColumnBinding struct** — stores per-column binding: target C type, buffer pointer, buffer length, indicator/length pointer
2. **Binding array on the statement** — dynamically sized (grows to max column bound)
3. **Fetch integration** — after advancing the cursor, iterate bound columns, call the existing `convert_value_to_c_type` logic (from results.c) to fill each bound buffer
4. **SQLFreeStmt(SQL_UNBIND)** — clear all column bindings

## Relevant Files

### Original Reference (~/projects/psqlodbc)
- `bind.c` — PGAPI_BindCol implementation (lines 160-309)
- `bind.h` — BindInfoClass struct
- `descriptor.h` — ARDFields struct (bindings array, allocated count)
- `results.c` — SC_fetch uses bindings during row retrieval

### New Files (this project)
- `src/column_binding.h` — ColumnBinding struct, binding management declarations
- `src/column_binding.c` — Bind, unbind, and fetch-into-bindings logic

### Modified Files
- `src/statement.h` — Add column bindings array to OdbcStatement
- `src/results.c` — Update results_fetch to populate bound column buffers
- `src/odbc_api.c` — Add SQLBindCol export, update SQL_UNBIND handling
- `src/meson.build` — Add column_binding.c
- `psqlodbc2.def` — Add SQLBindCol export

## Implementation Phases

### Phase 1: Foundation
- Define ColumnBinding struct (column_number, target_type, buffer, buffer_length, indicator_ptr)
- Add bindings array (pointer + allocated count) to OdbcStatement
- Implement bind/unbind logic with dynamic array growth

### Phase 2: Core Implementation
- Implement fetch-with-bindings: after cursor advance, iterate all bound columns, convert and store each value
- Reuse the existing convert_value_to_c_type logic from results.c (factor into shared helper if needed)
- Handle NULL values (set indicator to SQL_NULL_DATA, don't write to buffer)
- Handle column 0 (bookmark — return error since bookmarks are not supported)

### Phase 3: Integration & Polish
- Wire up SQLBindCol in odbc_api.c
- Implement SQL_UNBIND in statement_free_stmt
- Tests: bind columns, fetch, verify data in buffers
- Edge cases: unbinding mid-fetch, binding to column beyond result set width

## Team Orchestration

- You operate as the team lead and orchestrate the team to execute the plan.
- You NEVER operate directly on the codebase. You use `Task` and `Task*` tools to deploy team members.
- The flow is: Builder implements → Reviewer checks quality → Validator verifies correctness.

### Team Members

- Builder
  - Name: builder-colbind
  - Role: Implement column binding and fetch-into-bindings
  - Agent Type: builder
  - Resume: true
- Reviewer
  - Name: reviewer-colbind
  - Role: Code quality and C11 best practices review
  - Agent Type: reviewer
  - Resume: false
- Validator
  - Name: validator-colbind
  - Role: Build verification and behavioral correctness
  - Agent Type: validator
  - Resume: false

## Step by Step Tasks

- Execute every step in order, top to bottom.
- Before you start, run `TaskCreate` to create the initial task list.

### 1. Create Column Binding Module
- **Task ID**: create-colbind-module
- **Depends On**: none
- **Assigned To**: builder-colbind
- **Agent Type**: builder
- **Parallel**: false
- Create `src/column_binding.h` with ColumnBinding struct and function declarations
- Create `src/column_binding.c` with bind, unbind_all, and populate_from_row functions

### 2. Integrate with Statement and Results
- **Task ID**: integrate-fetch
- **Depends On**: create-colbind-module
- **Assigned To**: builder-colbind
- **Agent Type**: builder
- **Parallel**: false
- Add column binding storage to OdbcStatement
- Update results_fetch to call populate_from_row after cursor advance
- Implement SQL_UNBIND in statement_free_stmt
- Free column bindings in statement_free

### 3. Wire Up ODBC API and Test
- **Task ID**: wire-and-test
- **Depends On**: integrate-fetch
- **Assigned To**: builder-colbind
- **Agent Type**: builder
- **Parallel**: false
- Add SQLBindCol to odbc_api.c with doc comment and spec URL
- Update psqlodbc2.def and src/meson.build
- Write tests for bind/fetch/unbind cycle

### 4. Validate
- **Task ID**: validate-all
- **Depends On**: wire-and-test
- **Assigned To**: validator-colbind
- **Agent Type**: validator
- **Parallel**: false
- Build the project
- Run all tests
- Verify behavioral compatibility with original

## Acceptance Criteria
- SQLBindCol stores binding info on the statement handle
- SQLFetch populates bound buffers with converted data
- NULL columns set indicator to SQL_NULL_DATA without modifying buffer
- String truncation sets indicator to full length and returns SQL_SUCCESS_WITH_INFO
- SQLFreeStmt(SQL_UNBIND) clears all column bindings
- Unbinding a single column (buffer=NULL) works
- Columns not bound still work via SQLGetData after fetch

## Validation Commands
- `meson setup builddir --reconfigure` - Configure build
- `meson compile -C builddir` - Compile
- `meson test -C builddir` - Run tests

## Notes
- Row-wise binding (SQL_ATTR_ROW_BIND_TYPE) is a future enhancement. Initial implementation supports column-wise binding only.
- Array fetching (SQL_ATTR_ROW_ARRAY_SIZE > 1) is a future enhancement.
- The existing convert_value_to_c_type in results.c should be factored into a shared module if it isn't already reusable from column_binding.c.
