# Plan: Statement Attributes (SQLSetStmtAttr / SQLGetStmtAttr)

## Task Description
Implement SQLSetStmtAttr and SQLGetStmtAttr to allow applications to configure statement behavior — cursor type, concurrency, query timeout, row array size, and metadata ID handling.

## Objective
When this plan is complete:
1. Applications can set/get SQL_ATTR_CURSOR_TYPE (forward-only is the only supported type initially)
2. Applications can set/get SQL_ATTR_CONCURRENCY (read-only is the only supported type initially)
3. Applications can set/get SQL_ATTR_QUERY_TIMEOUT (passed to PostgreSQL's statement_timeout)
4. Applications can set/get SQL_ATTR_MAX_ROWS (limit result set size)
5. Applications can query SQL_ATTR_ROW_NUMBER (current cursor position)

## Problem Statement
Many ODBC applications call SQLSetStmtAttr immediately after allocating a statement handle — even if just to set cursor type to SQL_CURSOR_FORWARD_ONLY. Without these functions exported, those applications fail at the first call.

## Solution Approach
1. **Statement options struct** — group configurable attributes on the statement handle
2. **Supported attributes** — implement the most common ones; return SQL_SUCCESS_WITH_INFO with option-value-changed for attributes we accept but silently downgrade (e.g., setting DYNAMIC cursor type → forward-only)
3. **Query timeout** — translatable to PostgreSQL's `SET statement_timeout` before execution

## Relevant Files

### Original Reference (~/projects/psqlodbc)
- `pgapi30.c` — PGAPI_SetStmtAttr, PGAPI_GetStmtAttr
- `options.c` — PGAPI_SetStmtOption, PGAPI_GetStmtOption
- `statement.h` — StatementOptions struct

### New Files (this project)
- (No new files — modifications to existing modules)

### Modified Files
- `src/statement.h` — Add statement options/attributes fields
- `src/odbc_api.c` — Add SQLSetStmtAttr, SQLGetStmtAttr exports
- `psqlodbc2.def` — Add new exports

## Implementation Phases

### Phase 1: Foundation
- Add statement options fields to OdbcStatement: cursor_type, concurrency, query_timeout, max_rows, row_number, metadata_id

### Phase 2: Core Implementation
- Implement SQLSetStmtAttr dispatcher:
  - SQL_ATTR_CURSOR_TYPE: accept FORWARD_ONLY, downgrade others with SQL_SUCCESS_WITH_INFO
  - SQL_ATTR_CONCURRENCY: accept READ_ONLY, downgrade others
  - SQL_ATTR_QUERY_TIMEOUT: store value, apply via SET statement_timeout before execution
  - SQL_ATTR_MAX_ROWS: store value, append LIMIT to queries (or use after-fetch truncation)
  - SQL_ATTR_METADATA_ID: store boolean
  - SQL_ATTR_NOSCAN: accept (controls escape clause scanning — no-op for now)
- Implement SQLGetStmtAttr dispatcher for same attributes plus SQL_ATTR_ROW_NUMBER

### Phase 3: Integration & Polish
- Wire up exports, add tests
- Ensure statement attributes survive cursor close (per ODBC spec)

## Team Orchestration

- You operate as the team lead and orchestrate the team to execute the plan.
- You NEVER operate directly on the codebase. You use `Task` and `Task*` tools to deploy team members.
- The flow is: Builder implements → Reviewer checks quality → Validator verifies correctness.

### Team Members

- Builder
  - Name: builder-stmtattr
  - Role: Implement statement attributes set/get
  - Agent Type: builder
  - Resume: true
- Reviewer
  - Name: reviewer-stmtattr
  - Role: Code quality and C11 best practices review
  - Agent Type: reviewer
  - Resume: false
- Validator
  - Name: validator-stmtattr
  - Role: Build verification and behavioral correctness
  - Agent Type: validator
  - Resume: false

## Step by Step Tasks

- Execute every step in order, top to bottom.
- Before you start, run `TaskCreate` to create the initial task list.

### 1. Add Statement Options Fields
- **Task ID**: add-stmt-options
- **Depends On**: none
- **Assigned To**: builder-stmtattr
- **Agent Type**: builder
- **Parallel**: false
- Add fields to OdbcStatement: cursor_type (SQLULEN), concurrency (SQLULEN), query_timeout_seconds (SQLULEN), max_rows (SQLULEN), metadata_id (bool), noscan (bool)
- Initialize defaults in statement_allocate: cursor_type=SQL_CURSOR_FORWARD_ONLY, concurrency=SQL_CONCUR_READ_ONLY, query_timeout=0, max_rows=0

### 2. Implement Set/Get StmtAttr
- **Task ID**: implement-stmtattr
- **Depends On**: add-stmt-options
- **Assigned To**: builder-stmtattr
- **Agent Type**: builder
- **Parallel**: false
- Implement SQLSetStmtAttr with supported attribute dispatch
- Implement SQLGetStmtAttr with supported attribute dispatch
- Handle option-value-changed for downgraded cursor types
- Return HY092 for unknown attributes

### 3. Wire Up and Test
- **Task ID**: wire-and-test
- **Depends On**: implement-stmtattr
- **Assigned To**: builder-stmtattr
- **Agent Type**: builder
- **Parallel**: false
- Add exports to odbc_api.c, psqlodbc2.def
- Write tests: set/get round-trip for each attribute, downgrade behavior

### 4. Validate
- **Task ID**: validate-all
- **Depends On**: wire-and-test
- **Assigned To**: validator-stmtattr
- **Agent Type**: validator
- **Parallel**: false
- Build the project
- Run all tests
- Verify behavioral compatibility with original

## Acceptance Criteria
- SQLSetStmtAttr stores values retrievable by SQLGetStmtAttr
- Setting unsupported cursor types returns SQL_SUCCESS_WITH_INFO (option value changed)
- Query timeout of 0 means no timeout (default)
- Attributes persist across statement close (SQL_CLOSE) but not free
- Unknown attributes return SQL_ERROR with HY092

## Validation Commands
- `meson setup builddir --reconfigure` - Configure build
- `meson compile -C builddir` - Compile
- `meson test -C builddir` - Run tests

## Notes
- Scrollable cursors (STATIC, KEYSET, DYNAMIC) are complex features involving server-side DECLARE CURSOR. We accept the constant but downgrade to FORWARD_ONLY.
- SQL_ATTR_ROW_ARRAY_SIZE and array fetching is a future enhancement (requires column binding + row status arrays).
- The original psqlodbc uses descriptor handles (ARD, APD, IRD, IPD) extensively. We defer full descriptor support — statement attributes are stored directly on the statement for now.
