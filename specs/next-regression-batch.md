# Plan: Next Regression Batch — SQLColAttribute, SQLGetInfo, Type Conversions, ODBC Escapes, SQLNumParams

## Task Description
Implement the next set of missing ODBC functions and features exposed by running the full psqlodbc regression test suite. This batch targets the features that will unlock the most tests with the least implementation complexity.

## Objective
When this plan is complete, the following additional regression tests pass (bringing total from 14 to ~30):
- `colattribute` — needs SQLColAttribute
- `multistmt` — needs SQLColAttribute (used by print_result helper) + multi-statement handling
- `parse` — needs SQLColAttribute
- `result-conversions` — needs boolean-to-numeric conversion fix + SQL_C_WCHAR stub
- `param-conversions` — needs boolean result fix ("t"/"f" → "1"/"0" for SQL_C_CHAR on boolean)
- `odbc-escapes` — needs `{fn ...}`, `{d '...'}`, `{t '...'}`, `{ts '...'}` escape processing
- `leading-literal-numparams` — needs ODBC escape processing (literal in ODBC escape syntax)
- `identity` — needs `{fn ...}` escape + `SQLGetInfo` for special columns
- `notice` — needs NOTICE capture via libpq notice processor
- `insertreturning` — needs SQLRowCount after INSERT...RETURNING + SQLColAttribute
- `dbms-version` — needs SQLGetInfo
- `odbc-conformance` — needs SQLGetInfo

## Problem Statement
14/61 tests pass. The biggest blockers are 4 missing exported functions (SQLColAttribute, SQLGetInfo, SQLNumParams, SQLNativeSql) and 2 missing features (ODBC escape clause processing, correct boolean type conversion). These affect 20+ tests.

## Solution Approach

### 1. SQLColAttribute (highest impact — used by test harness print_result_series)
Return column attributes from the result set (or describe_result after prepare). Key attributes:
- SQL_DESC_LABEL / SQL_DESC_NAME — column name
- SQL_DESC_TYPE / SQL_DESC_CONCISE_TYPE — SQL type
- SQL_DESC_DISPLAY_SIZE — display width
- SQL_DESC_OCTET_LENGTH — byte length
- SQL_DESC_PRECISION — precision for numerics
- SQL_DESC_SCALE — scale for numerics
- SQL_DESC_NULLABLE — nullability
- SQL_DESC_UNSIGNED — whether unsigned
- SQL_DESC_AUTO_UNIQUE_VALUE — auto-increment
- SQL_DESC_UPDATABLE — always SQL_ATTR_READWRITE_UNKNOWN for now
- SQL_DESC_COUNT — number of columns
- SQL_DESC_TABLE_NAME, SQL_DESC_SCHEMA_NAME — empty for now (expensive to determine)

### 2. SQLGetInfo (required by common.c helper and many tests)
Return driver and data source metadata. Essential info types:
- SQL_DBMS_NAME → "PostgreSQL"
- SQL_DBMS_VER → from PQserverVersion (format "MM.mm.pppp")
- SQL_DRIVER_NAME → "psqlodbc2w"
- SQL_DRIVER_VER → "00.01.0000"
- SQL_DRIVER_ODBC_VER → "03.80"
- SQL_IDENTIFIER_QUOTE_CHAR → "\""
- SQL_CATALOG_NAME_SEPARATOR → "."
- SQL_CATALOG_TERM → "database"
- SQL_SCHEMA_TERM → "schema"
- SQL_TABLE_TERM → "table"
- SQL_DATA_SOURCE_NAME → connection DSN
- SQL_SERVER_NAME → from PQhost
- SQL_DATABASE_NAME → from PQdb
- SQL_USER_NAME → from PQuser
- SQL_SEARCH_PATTERN_ESCAPE → "\\"
- SQL_MAX_IDENTIFIER_LEN → 63
- SQL_MAX_COLUMNS_IN_TABLE → 1600
- Various SQL_CVT_* (conversion) and SQL_*_FUNCTIONS bitmasks → 0 for now
- SQL_GETDATA_EXTENSIONS → SQL_GD_ANY_COLUMN | SQL_GD_ANY_ORDER

### 3. Boolean Type Conversion Fix
Currently when reading a PostgreSQL boolean column:
- As SQL_C_CHAR: we return "t" or "f" (raw PG text) — should return "1" or "0"
- As SQL_C_SLONG/SQL_C_SSHORT/etc: we parse with atoi("t") → 0 — should be 1 for true

Fix: in `convert_value_to_c_type`, detect boolean columns (PG OID 16) and convert "t"→"1"/"f"→"0" before applying type conversion. OR: detect boolean at the SQL_C_CHAR level and return "1"/"0".

### 4. ODBC Escape Clause Processing
ODBC defines escape sequences that the driver must translate to native SQL:
- `{fn function_name(args)}` → native function call (most map directly)
- `{d 'YYYY-MM-DD'}` → `'YYYY-MM-DD'::date` or just `'YYYY-MM-DD'`
- `{t 'HH:MM:SS'}` → `'HH:MM:SS'::time` or just `'HH:MM:SS'`
- `{ts 'YYYY-MM-DD HH:MM:SS'}` → `'YYYY-MM-DD HH:MM:SS'::timestamp` or just the literal
- `{oj table LEFT OUTER JOIN ...}` → strip `{oj` and `}`
- `{escape 'char'}` → `ESCAPE 'char'`

Processing: scan the SQL before sending to PG, expand escape sequences. Add this to `query_parser.c` — process escapes in the same pass as `?` translation.

### 5. SQLNumParams
After SQLPrepare, return the number of parameters detected in the SQL. We already store `detected_param_count` from the query parser. Just need to export the function.

### 6. NOTICE Capture
Set a libpq notice receiver on the connection that captures NOTICE messages. Store them so that after SQLExecDirect/SQLExecute, the application can retrieve them via SQLGetDiagRec with SQL_SUCCESS_WITH_INFO.

## Relevant Files

### Original Reference (~/projects/psqlodbc)
- `info.c` — PGAPI_GetInfo (massive switch statement for 200+ info types)
- `columninfo.c` — Column attribute retrieval
- `pgapi30.c` — PGAPI_ColAttribute
- `convert.c` — ODBC escape processing in `copy_statement_with_parameters`
- `connection.c` — `receive_libpq_notice` notice handler

### New Files (this project)
- (No new source files — extend existing modules)

### Modified Files
- `src/odbc_api.c` — Add SQLColAttribute, SQLGetInfo, SQLNumParams, SQLNativeSql exports
- `src/query_parser.c` — Add ODBC escape clause processing
- `src/results.c` — Fix boolean conversion in convert_value_to_c_type
- `src/connection.c` — Add libpq notice receiver
- `src/connection.h` — Add notice storage
- `src/type_mapping.c` — Add helper to detect boolean OID
- `psqlodbc2.def` — Add new exports

## Implementation Phases

### Phase 1: SQLColAttribute + SQLGetInfo (unlocks most tests)
- Implement SQLColAttribute in odbc_api.c using PQfname, PQftype, PQfmod from current_result or describe_result
- Implement SQLGetInfo with the essential info types listed above
- These two unlock: colattribute, multistmt, parse, dbms-version, odbc-conformance

### Phase 2: Boolean Fix + Type Conversions (fixes result-conversions, param-conversions)
- Fix boolean column retrieval: detect OID 16 (bool), convert "t"→1, "f"→0
- For SQL_C_CHAR on boolean: return "1" or "0" (not "t"/"f")
- Add SQL_C_WCHAR minimal support (convert to UTF-16LE from char) OR return error gracefully

### Phase 3: ODBC Escape Processing (fixes odbc-escapes, leading-literal-numparams, identity)
- Extend query_parser to recognize and expand `{fn ...}`, `{d ...}`, `{t ...}`, `{ts ...}`, `{oj ...}`, `{escape ...}`
- `{fn func(args)}` → `func(args)` (PostgreSQL supports most standard functions)
- Date/time literals: strip `{d }`/`{t }`/`{ts }` wrappers, keep the quoted literal
- `{oj ...}` → strip wrapper
- Process before `?` → `$N` translation (escapes may contain `?` markers)

### Phase 4: SQLNumParams + NOTICE Capture
- Export SQLNumParams (trivial — read detected_param_count from statement)
- Add libpq notice receiver: on connection_connect, call PQsetNoticeReceiver. Store notices. Return SQL_SUCCESS_WITH_INFO from execute when notices were received.

## Team Orchestration

- You operate as the team lead and orchestrate the team to execute the plan.
- You NEVER operate directly on the codebase. You use `Task` and `Task*` tools to deploy team members.
- The flow is: Builder implements → Reviewer checks quality → Validator verifies correctness.

### Team Members

- Builder
  - Name: builder-batch2
  - Role: Implement SQLColAttribute, SQLGetInfo, boolean fix, ODBC escapes, SQLNumParams, notices
  - Agent Type: builder
  - Resume: true
- Reviewer
  - Name: reviewer-batch2
  - Role: Code quality and C11 best practices review
  - Agent Type: reviewer
  - Resume: false
- Validator
  - Name: validator-batch2
  - Role: Build verification and regression test validation
  - Agent Type: validator
  - Resume: false

## Step by Step Tasks

- Execute every step in order, top to bottom.
- Before you start, run `TaskCreate` to create the initial task list.

### 1. Implement SQLColAttribute
- **Task ID**: implement-colattribute
- **Depends On**: none
- **Assigned To**: builder-batch2
- **Agent Type**: builder
- **Parallel**: false
- Add SQLColAttribute to odbc_api.c with full doc comment
- Use PQfname for SQL_DESC_LABEL/SQL_DESC_NAME
- Use type_mapping for SQL_DESC_TYPE/SQL_DESC_CONCISE_TYPE
- Use type_mapping_get_column_size for SQL_DESC_DISPLAY_SIZE/SQL_DESC_OCTET_LENGTH
- Use PQfmod for SQL_DESC_PRECISION/SQL_DESC_SCALE
- Return SQL_NULLABLE_UNKNOWN for SQL_DESC_NULLABLE
- Support reading from describe_result when current_result is NULL
- Update psqlodbc2.def and SQLGetFunctions list

### 2. Implement SQLGetInfo
- **Task ID**: implement-getinfo
- **Depends On**: none
- **Assigned To**: builder-batch2
- **Agent Type**: builder
- **Parallel**: true
- Add SQLGetInfo to odbc_api.c with essential info types
- String infos: write to output buffer, report length
- Integer infos: write SQLUINTEGER/SQLUSMALLINT to output pointer
- Must handle connected and disconnected state (some infos available before connect)
- Update psqlodbc2.def and SQLGetFunctions list

### 3. Fix Boolean Type Conversion
- **Task ID**: fix-boolean-conversion
- **Depends On**: none
- **Assigned To**: builder-batch2
- **Agent Type**: builder
- **Parallel**: true
- In convert_value_to_c_type (results.c): add PG OID parameter or detect boolean from the "t"/"f" pattern
- For SQL_C_CHAR on boolean: return "1"/"0" not "t"/"f"
- For SQL_C_SLONG/SQL_C_SHORT/etc on boolean: parse "t"→1, "f"→0
- For SQL_C_BIT: already handled (returns 1/0)

### 4. Implement ODBC Escape Processing
- **Task ID**: implement-odbc-escapes
- **Depends On**: none
- **Assigned To**: builder-batch2
- **Agent Type**: builder
- **Parallel**: true
- Extend query_translate_markers in query_parser.c to also process ODBC escapes
- Detect `{` in NORMAL state, identify escape type (fn/d/t/ts/oj/escape)
- For `{fn func(args)}`: output `func(args)`
- For `{d 'val'}`, `{t 'val'}`, `{ts 'val'}`: output `'val'`
- For `{oj content}`: output `content`
- For `{escape 'c'}`: output `ESCAPE 'c'`
- Must handle nested braces and escapes inside string literals

### 5. Implement SQLNumParams + NOTICE Capture
- **Task ID**: implement-numparams-notices
- **Depends On**: implement-odbc-escapes
- **Assigned To**: builder-batch2
- **Agent Type**: builder
- **Parallel**: false
- Export SQLNumParams: return statement->detected_param_count
- Add notice receiver: in connection_connect, call PQsetNoticeReceiver with a callback that stores notices
- Add notice storage to OdbcConnection (circular buffer or simple array)
- After statement execution: if notices were received, return SQL_SUCCESS_WITH_INFO and populate diagnostics with SQLSTATE "01000" + notice text

### 6. Run Regression Tests
- **Task ID**: run-regression
- **Depends On**: implement-numparams-notices
- **Assigned To**: builder-batch2
- **Agent Type**: builder
- **Parallel**: false
- Run full regression suite
- Fix any remaining issues in the target tests
- Verify no regressions in previously-passing tests

### 7. Validate
- **Task ID**: validate-all
- **Depends On**: run-regression
- **Assigned To**: validator-batch2
- **Agent Type**: validator
- **Parallel**: false
- Run `meson compile -C builddir`
- Run `meson test -C builddir`
- Run `./regress/run_regression.sh` with all 61 tests
- Verify at least 25 tests pass (up from 14)
- Verify no regressions

## Acceptance Criteria
- SQLColAttribute returns column name, type, size, scale, nullability
- SQLGetInfo returns SQL_DBMS_NAME="PostgreSQL", SQL_DBMS_VER with proper format
- Boolean columns return "1"/"0" for SQL_C_CHAR and proper integers for numeric types
- ODBC escape `{fn now()}` translates to `now()`
- ODBC escape `{d '2024-01-01'}` translates to `'2024-01-01'`
- SQLNumParams returns the count of `?` markers found during prepare
- NOTICE messages are available via SQLGetDiagRec after execution
- At least 25/61 regression tests pass (target: 30)
- All 13 existing unit tests still pass

## Validation Commands
- `meson compile -C builddir` - Compile
- `meson test -C builddir` - Unit tests
- `export PATH="/usr/local/pgsql/18/bin:$PATH" && ./regress/run_regression.sh <all tests>` - Regression

## Notes
- SQLColAttribute is the #1 missing function — the test harness `print_result_series` calls it for column names when printing results. Without it, most tests that print results fail.
- SQLGetInfo has 200+ info types. We implement ~30 essential ones and return SQL_ERROR for unknown types. This is enough for real applications.
- Boolean conversion: PostgreSQL sends "t"/"f" but ODBC applications expect "1"/"0" for SQL_C_CHAR on boolean columns. The original psqlodbc driver always converts these.
- ODBC escape processing should happen BEFORE parameter marker translation, since escape content may contain `?` markers.
- For SQL_C_WCHAR: a full implementation requires UTF-8 → UTF-16 conversion. For now, returning SQL_ERROR with HY003 is acceptable for tests that explicitly test WCHAR. The `result-conversions` test will partially pass (non-WCHAR conversions) but WCHAR lines will show errors.
- The `notice` test expects NOTICE messages to be returned as diagnostics with SQL_SUCCESS_WITH_INFO after executing a statement that raises a NOTICE. libpq's PQsetNoticeReceiver is the mechanism for this.
