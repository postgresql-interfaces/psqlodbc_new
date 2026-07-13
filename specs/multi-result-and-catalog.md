# Plan: Multi-Statement Results, RETURNING, Refcursors, and Catalog Functions

## Task Description
Implement SQLMoreResults for multi-statement result iteration, INSERT/UPDATE...RETURNING result sets, refcursor auto-fetching, and the catalog metadata functions (SQLGetTypeInfo plus PrimaryKeys INCLUDE handling).

## Objective
When complete, these regression tests pass: `multistmt`, `premature`, `insertreturning`, `fetch-refcursors`, `catalogfunctions`, `primarykeys-include`, `identity`.

## Problem Statement
A single SQLExecDirect can contain multiple statements (`SELECT 1; SELECT 2`), each producing a result set the app iterates with SQLMoreResults. INSERT...RETURNING produces a fetchable result. Refcursor OUT params are auto-fetched into result sets. Catalog functions expose database metadata. SQLGetTypeInfo lists supported types.

## Solution Approach
1. **Multi-statement splitting + result chaining** — split the SQL into statements (respecting quotes/comments — reuse query_parser), execute each, chain the PGresults. SQLMoreResults advances to the next.
2. **SQLMoreResults** — move to the next result set in the chain; refresh column metadata.
3. **RETURNING** — INSERT/UPDATE/DELETE with RETURNING produce PGRES_TUPLES_OK; expose as a result set. SQLNumResultCols reports columns without premature execution.
4. **Premature execution guard** — SQLNumResultCols/SQLDescribeCol after prepare must use PQdescribePrepared (already done), NOT execute the statement.
5. **Refcursors** — FetchRefcursors option: when a function returns refcursor, auto-`FETCH ALL` from each cursor and expose as successive result sets.
6. **Catalog functions** — SQLGetTypeInfo (synthetic result set of supported types). Fix SQLPrimaryKeys to exclude PG11+ INCLUDE columns. Verify SQLTables/SQLColumns/etc. edge cases.
7. **@@IDENTITY** — parse INSERT to extract table, return last insert value.

## Relevant Files

### Original Reference (~/projects/psqlodbc)
- `results.c` — `PGAPI_MoreResults`, result chaining
- `info.c` — `PGAPI_GetTypeInfo`, catalog function family
- `statement.c`/`parse.c` — statement splitting, @@IDENTITY parsing
- `convert.c` — refcursor handling
- `test/src/multistmt-test.c`, `premature-test.c`, `insertreturning-test.c`, `fetch-refcursors-test.c`, `catalogfunctions-test.c`, `primarykeys-include-test.c`, `identity-test.c`

### Modified Files
- `src/statement.h/.c` — result-set chain, statement splitting, RETURNING detection, identity parsing
- `src/results.c` — SQLMoreResults, per-result metadata refresh
- `src/catalog.c` — SQLGetTypeInfo, PrimaryKeys INCLUDE fix
- `src/odbc_api.c` — SQLMoreResults, SQLGetTypeInfo exports
- `src/connection.c/.h` — FetchRefcursors option
- `psqlodbc2.def` — new exports

## Implementation Phases

### Phase 1: Multi-statement + SQLMoreResults (fixes multistmt, premature)
- Split multi-statement SQL (reuse query_parser quote/comment tracking)
- Execute each, chain results; SQLMoreResults advances
- Refresh column metadata per result
- Confirm no premature execution during describe

### Phase 2: RETURNING + identity (fixes insertreturning, identity)
- Detect RETURNING, expose result set
- SQLNumResultCols without premature execute
- @@IDENTITY parse and last-value retrieval

### Phase 3: Catalog + refcursors (fixes catalogfunctions, primarykeys-include, fetch-refcursors)
- SQLGetTypeInfo synthetic result set
- SQLPrimaryKeys excludes INCLUDE columns (PG11+)
- FetchRefcursors auto-fetch

## Team Orchestration
- Builder implements → Reviewer checks quality → Validator verifies correctness.

### Team Members
- Builder
  - Name: builder-multires
  - Role: Implement multi-result, RETURNING, refcursors, catalog functions
  - Agent Type: builder
  - Resume: true
- Reviewer
  - Name: reviewer-multires
  - Role: Code quality and C11 best practices review
  - Agent Type: reviewer
  - Resume: false
- Validator
  - Name: validator-multires
  - Role: Build and regression verification
  - Agent Type: validator
  - Resume: false

## Step by Step Tasks

### 1. Multi-statement + SQLMoreResults
- **Task ID**: implement-moreresults
- **Depends On**: none
- **Assigned To**: builder-multires
- **Agent Type**: builder
- **Parallel**: false
- Statement splitting, result chaining, SQLMoreResults
- Verify multistmt, premature pass

### 2. RETURNING + identity
- **Task ID**: implement-returning
- **Depends On**: implement-moreresults
- **Assigned To**: builder-multires
- **Agent Type**: builder
- **Parallel**: false
- RETURNING result sets, @@IDENTITY
- Verify insertreturning, identity pass

### 3. Catalog + refcursors
- **Task ID**: implement-catalog
- **Depends On**: none
- **Assigned To**: builder-multires
- **Agent Type**: builder
- **Parallel**: true
- SQLGetTypeInfo, PrimaryKeys INCLUDE, FetchRefcursors
- Verify catalogfunctions, primarykeys-include, fetch-refcursors pass

### 4. Validate
- **Task ID**: validate-all
- **Depends On**: implement-returning, implement-catalog
- **Assigned To**: validator-multires
- **Agent Type**: validator
- **Parallel**: false
- Build, unit tests, 7 target regression tests plus currently-passing suite

## Acceptance Criteria
- `multistmt`, `premature`, `insertreturning`, `fetch-refcursors`, `catalogfunctions`, `primarykeys-include`, `identity` pass
- No regression in currently-passing tests
- SQLMoreResults iterates all result sets in a batch
- Describing a prepared statement does not execute it

## Validation Commands
- `meson compile -C builddir`
- `meson test -C builddir`
- `export PATH="/usr/local/pgsql/18/bin:$PATH" && ./regress/run_regression.sh multistmt premature insertreturning fetch-refcursors catalogfunctions primarykeys-include identity`

## Notes
- Statement splitting must respect string literals, dollar-quotes, and comments — the query_parser already tracks these states, so extend it to emit statement boundaries.
- SQLGetTypeInfo returns a fixed synthetic result set describing each supported SQL type (name, SQL type, precision, etc.) — the original builds this in info.c.
- primarykeys-include is a small gate: exclude columns that are part of an INCLUDE (covering index) clause, only relevant PG11+.
