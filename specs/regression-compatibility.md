# Plan: Regression Test Compatibility Fixes

## Task Description
Fix the compatibility gaps exposed by running the original psqlodbc regression tests against psqlodbc2. Six tests fail due to three core missing features:
1. **ODBC `?` parameter marker translation** — ODBC uses `?` as parameter markers, but PostgreSQL uses `$1`, `$2`, etc. The driver must translate `?` to `$N` before sending SQL to PostgreSQL.
2. **Metadata after SQLPrepare (before SQLExecute)** — `SQLNumResultCols` and `SQLDescribeCol` must work after `SQLPrepare` without requiring `SQLExecute`. This requires calling `PQdescribePrepared` to get column info from the server.
3. **SQL_C_INTERVAL type conversion** — `SQLGetData` with `SQL_C_INTERVAL_YEAR`, `SQL_C_INTERVAL_MONTH`, `SQL_C_INTERVAL_DAY` types is not implemented. The getresult test expects interval conversion.
4. **SQLFreeStmt(SQL_CLOSE) then re-execute pattern** — after closing a cursor, re-executing on the same statement must work even when autocommit is OFF and VACUUM is involved.

## Objective
When this plan is complete, the following regression tests pass:
- `connect` (already passes)
- `select` (already passes)
- `stmthandles` — interleaved prepared statements with NumResultCols before Execute
- `update` — INSERT with `?` parameter markers
- `prepare` — SELECT with `?` markers, bind, execute
- `params` — parameter binding with `?` markers
- `getresult` — interval type conversion via SQLGetData
- `commands` — VACUUM and DDL commands with autocommit toggling

## Problem Statement
The original psqlodbc test suite is the canonical compatibility test for PostgreSQL ODBC drivers. Currently 6/8 default tests fail, all due to missing features that real ODBC applications depend on.

## Solution Approach

### 1. Parameter Marker Translation (`?` → `$N`)
Before sending any SQL to PostgreSQL (in both `SQLPrepare` → `PQprepare` and `SQLExecDirect` → `PQexec`), scan the SQL text and replace each `?` with `$1`, `$2`, etc. Rules:
- `?` inside single-quoted string literals is NOT a parameter marker
- `?` inside double-quoted identifiers is NOT a parameter marker
- `?` inside `--` line comments or `/* */` block comments is NOT a parameter marker
- `??` is an escape for a literal `?` (becomes `?` in output) — this is the ODBC escape for `?` in LIKE patterns
- Each unquoted `?` gets replaced with `$N` where N increments from 1
- Store the parameter count so we can validate against the number of bound parameters

### 2. Metadata After Prepare (PQdescribePrepared)
After `PQprepare` succeeds, call `PQdescribePrepared(conn, stmt_name)` to get the result description (column names, types). Store this `PGresult` on the statement so `SQLNumResultCols` and `SQLDescribeCol` can read from it even before `SQLExecute`.

### 3. SQL_C_INTERVAL Type Conversion
Add cases to `convert_value_to_c_type` in `results.c` for:
- `SQL_C_INTERVAL_YEAR` — parse PostgreSQL interval text, extract year component
- `SQL_C_INTERVAL_MONTH` — extract month component
- `SQL_C_INTERVAL_DAY` — extract day component
- `SQL_C_INTERVAL_HOUR`, `SQL_C_INTERVAL_MINUTE`, `SQL_C_INTERVAL_SECOND` — extract time components

PostgreSQL returns intervals as text like "10 years", "11 mons", "12 days", "01:02:03" etc. Parse these into SQL_INTERVAL_STRUCT.

### 4. Statement Re-execution After Close
Ensure that `SQLFreeStmt(SQL_CLOSE)` properly resets state so the same statement handle can be re-used for new queries. Verify this works correctly when autocommit is off (the `commands` test toggles autocommit and runs VACUUM which requires not being in a transaction).

## Relevant Files

### Original Reference (~/projects/psqlodbc)
- `convert.c` — `copy_statement_with_parameters()` for `?` → `$N` translation, handles quoted strings and comments
- `statement.c` — `SC_describe()` uses `PQdescribePrepared` for metadata
- `execute.c` — statement execution with parameter resolution
- `results.c` — interval type handling in `copy_and_convert_field()`

### New Files (this project)
- `src/query_parser.h` — SQL text scanner declarations (parameter marker replacement)
- `src/query_parser.c` — Implementation of `?` → `$N` translation with quoting awareness

### Modified Files
- `src/statement.h` — Add `PGresult *describe_result` for pre-execute metadata
- `src/statement.c` — Call `PQdescribePrepared` after `PQprepare`; use describe_result for metadata
- `src/results.c` — Add SQL_C_INTERVAL_* cases to `convert_value_to_c_type`; update `results_num_result_cols` and `results_describe_col` to use describe_result when current_result is NULL
- `src/odbc_api.c` — Minor: ensure SQLExecDirect re-translates `?` markers if params are bound
- `src/meson.build` — Add query_parser.c

## Implementation Phases

### Phase 1: Parameter Marker Translation
- Create `src/query_parser.h` and `src/query_parser.c`
- Implement `query_translate_markers(const char *input, char *output, size_t output_size, int *param_count)`:
  - Walk the SQL character by character
  - Track state: normal, in single-quote string, in double-quote identifier, in line comment, in block comment
  - When `?` is found in normal state: replace with `$N`, increment N
  - Handle escape: `''` inside string, `""` inside identifier
  - Return the translated SQL and parameter count
- Update `statement_prepare` to translate before `PQprepare`
- Update `statement_exec_direct` to translate before `PQexec`/`PQexecParams` when parameters are bound

### Phase 2: Metadata After Prepare
- Add `PGresult *describe_result` field to OdbcStatement
- After successful `PQprepare` in `statement_prepare`: call `PQdescribePrepared(conn, stmt_name)`, store the result
- Update `results_num_result_cols`: if `current_result` is NULL but `describe_result` is not NULL, use `PQnfields(describe_result)`
- Update `results_describe_col`: if `current_result` is NULL, use `describe_result` for column name/type/size
- Free `describe_result` in `clear_current_result` and `statement_free`

### Phase 3: Interval Type Conversion
- Add parsing for PostgreSQL interval text format in `results.c`
- Support the `postgres` intervalstyle format: "X years Y mons Z days HH:MM:SS"
- Map to SQL_INTERVAL_STRUCT fields
- Handle SQL_C_INTERVAL_YEAR, _MONTH, _DAY, _HOUR, _MINUTE, _SECOND, _YEAR_TO_MONTH, _DAY_TO_SECOND

### Phase 4: Statement Re-execution & Commands Fix
- Verify `statement_close_cursor` properly allows re-execution
- For the `commands` test: when autocommit is OFF and the user issues VACUUM (which cannot run in a transaction), ensure the driver detects this is a non-transactional command OR properly reports the PostgreSQL error rather than crashing
- The fix may be: detect `PGRES_FATAL_ERROR` with SQLSTATE "25001" (active SQL transaction) and report it cleanly

## Team Orchestration

- You operate as the team lead and orchestrate the team to execute the plan.
- You NEVER operate directly on the codebase. You use `Task` and `Task*` tools to deploy team members.
- The flow is: Builder implements → Reviewer checks quality → Validator verifies correctness.

### Team Members

- Builder
  - Name: builder-compat
  - Role: Implement parameter marker translation, describe metadata, interval conversion, and statement re-execution fixes
  - Agent Type: builder
  - Resume: true
- Reviewer
  - Name: reviewer-compat
  - Role: Code quality and C11 best practices review
  - Agent Type: reviewer
  - Resume: false
- Validator
  - Name: validator-compat
  - Role: Build verification and regression test validation
  - Agent Type: validator
  - Resume: false

## Step by Step Tasks

- Execute every step in order, top to bottom.
- Before you start, run `TaskCreate` to create the initial task list.

### 1. Implement Parameter Marker Translation
- **Task ID**: implement-query-parser
- **Depends On**: none
- **Assigned To**: builder-compat
- **Agent Type**: builder
- **Parallel**: false
- Create `src/query_parser.h` with `query_translate_markers` declaration
- Create `src/query_parser.c` implementing the scanner with state machine (normal, in-string, in-identifier, in-line-comment, in-block-comment)
- Integrate into `statement_prepare` (translate before PQprepare)
- Integrate into `statement_exec_direct` (translate before PQexec/PQexecParams when params bound)
- Store translated SQL (heap-allocated) on the statement; free on close/re-prepare
- Store detected parameter count on the statement for validation

### 2. Implement Metadata After Prepare
- **Task ID**: implement-describe-prepared
- **Depends On**: implement-query-parser
- **Assigned To**: builder-compat
- **Agent Type**: builder
- **Parallel**: false
- Add `PGresult *describe_result` to OdbcStatement
- After PQprepare succeeds: call `PQdescribePrepared`, store result
- Update `results_num_result_cols` to check describe_result when current_result is NULL
- Update `results_describe_col` to read from describe_result when current_result is NULL
- Free describe_result in clear_current_result and statement_free

### 3. Implement Interval Type Conversion
- **Task ID**: implement-interval-types
- **Depends On**: none
- **Assigned To**: builder-compat
- **Agent Type**: builder
- **Parallel**: true (independent of tasks 1-2)
- Add SQL_C_INTERVAL_* cases to `convert_value_to_c_type` in results.c
- Parse PostgreSQL interval text: extract year, month, day, hour, minute, second components
- Populate SQL_INTERVAL_STRUCT appropriately
- Handle both "postgres" and "postgres_verbose" intervalstyle formats

### 4. Fix Statement Re-execution and Commands
- **Task ID**: fix-reexecution
- **Depends On**: implement-query-parser
- **Assigned To**: builder-compat
- **Agent Type**: builder
- **Parallel**: false
- Verify SQLFreeStmt(SQL_CLOSE) → SQLExecDirect works on the same handle
- Ensure the `commands` test pattern works: ExecDirect → Close → ExecDirect (repeated)
- When autocommit toggles and VACUUM runs outside a transaction, ensure clean error reporting
- The commands test disables autocommit, does ExecDirect("VACUUM"), which should fail with a proper SQLSTATE (not crash)

### 5. Run Regression Tests
- **Task ID**: run-regression
- **Depends On**: implement-describe-prepared, implement-interval-types, fix-reexecution
- **Assigned To**: builder-compat
- **Agent Type**: builder
- **Parallel**: false
- Run the full default regression suite: `./regress/run_regression.sh`
- Fix any remaining issues
- Verify connect, select, stmthandles, update, prepare, params, getresult, commands all pass

### 6. Validate
- **Task ID**: validate-all
- **Depends On**: run-regression
- **Assigned To**: validator-compat
- **Agent Type**: validator
- **Parallel**: false
- Run `meson compile -C builddir` — no errors
- Run `meson test -C builddir` — all unit tests pass
- Run `./regress/run_regression.sh` — all 8 default tests pass
- Verify no regressions in existing test suite

## Acceptance Criteria
- `./regress/run_regression.sh` reports 8/8 passed for the default test set
- `?` in SQL is correctly translated to `$1`, `$2`, etc.
- `?` inside string literals, identifiers, and comments is NOT translated
- `SQLNumResultCols` returns correct count after `SQLPrepare` (before `SQLExecute`)
- `SQLDescribeCol` returns column name/type after `SQLPrepare` (before `SQLExecute`)
- `SQLGetData` with `SQL_C_INTERVAL_DAY` correctly parses "12 days" → day=12
- `meson test -C builddir` — all existing unit tests still pass
- No memory leaks in the query translation (translated SQL freed properly)

## Validation Commands
- `meson compile -C builddir` - Compile
- `meson test -C builddir` - Unit tests
- `./regress/run_regression.sh` - Original psqlodbc regression tests

## Notes
- The `?` → `$N` translation must be efficient since it runs on every SQL statement. A single-pass character scanner with state tracking is O(n) and sufficient.
- The translated SQL will be slightly longer than the original (e.g., `?` → `$10` adds 2 chars). Allocate output buffer as `2 * input_length` to be safe.
- `PQdescribePrepared` is a PostgreSQL extended query protocol feature available since PG 7.4. It returns a PGresult with column info but no rows — exactly what we need.
- The interval parser only needs to handle the output format PostgreSQL actually produces. With `intervalstyle=postgres`: "1 year 2 mons 3 days 04:05:06". With `postgres_verbose`: "@ 1 year 2 mons 3 days 4 hours 5 mins 6 secs".
- The `commands` test toggles autocommit OFF, runs VACUUM (which fails because it can't run in a transaction), expects the error, then re-enables autocommit. Our driver should report the PostgreSQL error cleanly rather than crashing.
