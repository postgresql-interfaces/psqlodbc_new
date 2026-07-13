# Plan: Results Module

## Task Description
Implement the results module for the psqlodbc2 driver: SQLFetch for advancing the cursor row-by-row, SQLGetData for retrieving column data from the current row, SQLNumResultCols for reporting the number of result columns, SQLDescribeCol for returning column metadata (name, type, size, scale, nullability), and SQLRowCount for reporting the number of affected rows from DML operations. This module reads from the PGresult already stored on the statement handle by the statement module.

## Objective
When this plan is complete:
1. Applications can call SQLNumResultCols after execution to learn how many columns a result set has
2. Applications can call SQLDescribeCol to get column name, SQL type, size, decimal digits, and nullability
3. Applications can call SQLFetch to advance row-by-row through a result set
4. Applications can call SQLGetData to retrieve individual column values as C types (initially: SQL_C_CHAR for text, SQL_C_LONG for integers, SQL_C_DOUBLE for floats)
5. Applications can call SQLRowCount after DML to get the number of affected rows
6. The cursor position is tracked on the statement handle and SQL_NO_DATA is returned at end of result set
7. NULL values are properly reported via the indicator/length parameter

## Problem Statement
The driver can now execute queries and store the PGresult, but applications have no way to read the data back. The results module is what makes the driver actually useful — without it, you can execute queries but cannot see the results.

## Solution Approach
The approach leverages libpq's PGresult API directly. Since PGresult stores all result data in memory (text format), our implementation:

1. **SQLNumResultCols** — calls `PQnfields(current_result)` and returns the count
2. **SQLDescribeCol** — calls `PQfname`, `PQftype`, `PQfmod` on the PGresult, then maps PostgreSQL OIDs to ODBC SQL types via a type mapping table
3. **SQLFetch** — increments the cursor row position; returns SQL_NO_DATA when past the last row
4. **SQLGetData** — calls `PQgetvalue`/`PQgetisnull` for the current row, then converts from PostgreSQL's text representation to the requested C type (SQL_C_CHAR, SQL_C_LONG, SQL_C_DOUBLE, etc.)
5. **SQLRowCount** — returns the `affected_row_count` already stored on the statement handle

The type mapping between PostgreSQL OIDs and ODBC SQL types is kept in a simple lookup table. For this initial implementation we support the most common types; unknown types default to SQL_VARCHAR.

Data conversion from PostgreSQL text format to C types:
- SQL_C_CHAR: return the text value as-is (with length)
- SQL_C_SLONG / SQL_C_LONG: `atoi()` / `atol()`
- SQL_C_DOUBLE: `strtod()`
- SQL_C_SSHORT: `(short)atoi()`
- SQL_C_FLOAT: `(float)strtod()`
- SQL_C_BIT: value is "t"/"f" or "1"/"0"
- SQL_C_DEFAULT: use the default C type for the column's SQL type

## Relevant Files

### Original Reference (~/projects/psqlodbc)
- `results.c` — PGAPI_Fetch, PGAPI_GetData, PGAPI_NumResultCols, PGAPI_DescribeCol, PGAPI_RowCount
- `columninfo.c` / `columninfo.h` — Column metadata extraction from PGresult
- `convert.c` / `convert.h` — Data type conversion between PG text format and ODBC C types
- `pgtypes.c` / `pgtypes.h` — PostgreSQL OID to ODBC SQL type mapping

### New Files (this project)
- `src/results.h` — Result retrieval function declarations and cursor state additions to the statement
- `src/results.c` — SQLFetch, SQLGetData, SQLNumResultCols, SQLDescribeCol, SQLRowCount implementations
- `src/type_mapping.h` — PostgreSQL OID to ODBC SQL type mapping table and conversion declarations
- `src/type_mapping.c` — Type mapping lookup, default C type for SQL type, column size/scale calculations
- `tests/test_results.c` — Integration tests for fetching results from a live database

### Modified Files
- `src/statement.h` — Add `current_row_position` field for cursor tracking
- `src/statement.c` — Initialize `current_row_position` to -1 (before first row); reset on statement_close_cursor
- `src/odbc_api.c` — Wire up SQLFetch, SQLGetData, SQLNumResultCols, SQLDescribeCol, SQLRowCount
- `src/meson.build` — Add results.c, type_mapping.c to driver_sources
- `psqlodbc2.def` — Add new exports
- `tests/meson.build` — Add new test executable

## Implementation Phases

### Phase 1: Foundation (Type Mapping + Statement Cursor State)
1. Create the PostgreSQL OID → ODBC SQL type mapping table
2. Implement helper functions: map OID to SQL type, get default C type for SQL type, compute column size and decimal digits from OID and typmod
3. Add `current_row_position` to OdbcStatement, initialize/reset appropriately

### Phase 2: Core Implementation (Fetch + GetData + Metadata)
1. Implement SQLNumResultCols — straightforward PQnfields wrapper
2. Implement SQLDescribeCol — PQfname for name, type mapping for SQL type, column size from typmod
3. Implement SQLRowCount — return affected_row_count from statement handle
4. Implement SQLFetch — increment row position, return SQL_NO_DATA at end
5. Implement SQLGetData — PQgetvalue + type conversion to requested C type

### Phase 3: Integration & Polish
1. Wire up all ODBC API entry points
2. Update exports
3. Write integration tests (live database)
4. Verify all existing tests still pass

## Code Examples

### Type mapping table:
```c
typedef struct PostgresTypeMapping {
    unsigned int postgres_oid;    /* PostgreSQL type OID */
    SQLSMALLINT  sql_type;        /* ODBC SQL type (SQL_INTEGER, SQL_VARCHAR, etc.) */
    SQLSMALLINT  default_c_type;  /* Default C type for SQLGetData with SQL_C_DEFAULT */
    const char  *type_name;       /* Human-readable name for diagnostics */
} PostgresTypeMapping;

/* Common PostgreSQL type OIDs (from pg_type.h) */
#define PG_TYPE_BOOL       16
#define PG_TYPE_INT2       21
#define PG_TYPE_INT4       23
#define PG_TYPE_INT8       20
#define PG_TYPE_FLOAT4    700
#define PG_TYPE_FLOAT8    701
#define PG_TYPE_NUMERIC  1700
#define PG_TYPE_VARCHAR  1043
#define PG_TYPE_TEXT       25
#define PG_TYPE_BPCHAR   1042
#define PG_TYPE_DATE     1082
#define PG_TYPE_TIME     1083
#define PG_TYPE_TIMESTAMP 1114
#define PG_TYPE_TIMESTAMPTZ 1184
```

### Cursor tracking (addition to statement.h):
```c
/* Add to OdbcStatement struct: */
    int current_row_position;    /* -1 = before first row; 0..N-1 = current row; N = past end */
```

### SQLFetch logic:
```c
SQLRETURN results_fetch(OdbcStatement *statement)
{
    /* Verify we have a result set */
    if (!statement->current_result || !statement->has_result_set) {
        /* error: no result set */
        return SQL_ERROR;
    }

    int total_rows = PQntuples(statement->current_result);
    statement->current_row_position++;

    if (statement->current_row_position >= total_rows) {
        /* Past the last row — return SQL_NO_DATA */
        return SQL_NO_DATA;
    }

    return SQL_SUCCESS;
}
```

### SQLGetData conversion logic:
```c
SQLRETURN results_get_data(OdbcStatement *statement,
                           SQLUSMALLINT column_number,  /* 1-based */
                           SQLSMALLINT target_type,
                           SQLPOINTER target_value,
                           SQLLEN buffer_length,
                           SQLLEN *indicator_or_length)
{
    int column_index = column_number - 1;  /* PQgetvalue uses 0-based */
    int row = statement->current_row_position;

    /* Check for NULL */
    if (PQgetisnull(statement->current_result, row, column_index)) {
        if (indicator_or_length) {
            *indicator_or_length = SQL_NULL_DATA;
        }
        return SQL_SUCCESS;
    }

    const char *raw_value = PQgetvalue(statement->current_result, row, column_index);
    /* Convert raw_value to target_type and copy to target_value... */
}
```

## Team Orchestration

- You operate as the team lead and orchestrate the team to execute the plan.
- You NEVER operate directly on the codebase. You use `Task` and `Task*` tools to deploy team members.
- The flow is: Builder implements → Reviewer checks quality → Validator verifies correctness.

### Team Members

- Builder
  - Name: builder-results
  - Role: Implement results module, type mapping, cursor tracking, and tests
  - Agent Type: builder
  - Resume: true
- Reviewer
  - Name: reviewer-results
  - Role: Code quality, C11 compliance, type conversion correctness, ODBC spec compliance review
  - Agent Type: reviewer
  - Resume: false
- Validator
  - Name: validator-results
  - Role: Build verification, test execution, behavioral correctness validation
  - Agent Type: validator
  - Resume: false

## Step by Step Tasks

- Execute every step in order, top to bottom.
- Before you start, run `TaskCreate` to create the initial task list.

### 1. Create PostgreSQL Type Mapping Module
- **Task ID**: implement-type-mapping
- **Depends On**: none
- **Assigned To**: builder-results
- **Agent Type**: builder
- **Parallel**: false
- Create `src/type_mapping.h` with:
  - PostgreSQL OID constants for common types (bool, int2, int4, int8, float4, float8, numeric, varchar, text, bpchar, date, time, timestamp, timestamptz, bytea, oid, name, char)
  - PostgresTypeMapping struct (postgres_oid, sql_type, default_c_type, type_name)
  - Declare: `type_mapping_get_sql_type(unsigned int postgres_oid)` — returns ODBC SQL type for a PG OID
  - Declare: `type_mapping_get_default_c_type(SQLSMALLINT sql_type)` — returns the default C type for data retrieval
  - Declare: `type_mapping_get_column_size(unsigned int postgres_oid, int type_modifier)` — returns column display size (uses typmod for varchar length etc.)
  - Declare: `type_mapping_get_decimal_digits(unsigned int postgres_oid, int type_modifier)` — returns scale/precision for numeric types
  - Declare: `type_mapping_get_type_name(unsigned int postgres_oid)` — returns human-readable type name
- Create `src/type_mapping.c` implementing:
  - Static mapping table array covering the common PostgreSQL types
  - `type_mapping_get_sql_type`: linear scan of table, default to SQL_VARCHAR for unknown OIDs
  - `type_mapping_get_default_c_type`: switch on SQL type (SQL_INTEGER→SQL_C_SLONG, SQL_VARCHAR→SQL_C_CHAR, SQL_DOUBLE→SQL_C_DOUBLE, etc.)
  - `type_mapping_get_column_size`: for varchar/bpchar extract length from typmod (typmod - 4 for varchar); for int4 return 10; for int8 return 19; for float8 return 15; for text return 0 (variable); etc.
  - `type_mapping_get_decimal_digits`: for numeric extract scale from typmod; for integer types return 0; for float types return the implementation-defined precision
  - `type_mapping_get_type_name`: return the type_name field from the mapping

### 2. Add Cursor Position to Statement Handle
- **Task ID**: add-cursor-position
- **Depends On**: none
- **Assigned To**: builder-results
- **Agent Type**: builder
- **Parallel**: true (can run alongside task 1)
- Update `src/statement.h`: add `int current_row_position;` field to OdbcStatement struct (comment: -1 means before first row)
- Update `src/statement.c`:
  - In `statement_allocate`: set `current_row_position = -1`
  - In `clear_current_result` (static helper): reset `current_row_position = -1`
  - In `statement_close_cursor`: `current_row_position` is already reset via `clear_current_result`

### 3. Implement Results Module (Fetch, GetData, Metadata)
- **Task ID**: implement-results
- **Depends On**: implement-type-mapping, add-cursor-position
- **Assigned To**: builder-results
- **Agent Type**: builder
- **Parallel**: false
- Create `src/results.h` declaring:
  - `results_num_result_cols(OdbcStatement *statement, SQLSMALLINT *column_count)` — returns SQL_SUCCESS/SQL_ERROR
  - `results_describe_col(OdbcStatement *statement, SQLUSMALLINT column_number, SQLCHAR *column_name, SQLSMALLINT name_buffer_length, SQLSMALLINT *name_length, SQLSMALLINT *data_type, SQLULEN *column_size, SQLSMALLINT *decimal_digits, SQLSMALLINT *nullable)` — returns SQL_SUCCESS/SQL_ERROR
  - `results_row_count(OdbcStatement *statement, SQLLEN *row_count)` — returns SQL_SUCCESS
  - `results_fetch(OdbcStatement *statement)` — returns SQL_SUCCESS/SQL_NO_DATA/SQL_ERROR
  - `results_get_data(OdbcStatement *statement, SQLUSMALLINT column_number, SQLSMALLINT target_type, SQLPOINTER target_value, SQLLEN buffer_length, SQLLEN *indicator_or_length)` — returns SQL_SUCCESS/SQL_SUCCESS_WITH_INFO/SQL_ERROR
- Create `src/results.c` implementing:
  - `results_num_result_cols`: verify current_result exists, call PQnfields, store in *column_count
  - `results_describe_col`:
    - Validate column_number is 1-based and within range (1..PQnfields)
    - Get column name via PQfname(result, column_number - 1), copy to output buffer with truncation handling
    - Get PG OID via PQftype(result, column_number - 1)
    - Get typmod via PQfmod(result, column_number - 1)
    - Map OID to SQL type via type_mapping_get_sql_type
    - Get column_size via type_mapping_get_column_size(oid, typmod)
    - Get decimal_digits via type_mapping_get_decimal_digits(oid, typmod)
    - Nullable: report SQL_NULLABLE_UNKNOWN (we don't have table metadata to determine this accurately without an extra query)
    - Return SQL_SUCCESS (or SQL_SUCCESS_WITH_INFO if name truncated)
  - `results_row_count`: return statement->affected_row_count via *row_count; if affected_row_count == -1, set *row_count to 0
  - `results_fetch`:
    - Verify has_result_set is true and current_result is not NULL; if not, set diagnostic and return SQL_ERROR
    - Increment current_row_position
    - If current_row_position >= PQntuples(current_result), return SQL_NO_DATA
    - Return SQL_SUCCESS
  - `results_get_data`:
    - Validate: current_result exists, current_row_position >= 0 and < PQntuples
    - Validate: column_number is 1-based and <= PQnfields
    - Check PQgetisnull: if null, set *indicator_or_length = SQL_NULL_DATA, return SQL_SUCCESS
    - Get raw text value via PQgetvalue
    - Get value length via PQgetlength
    - Resolve target_type: if SQL_C_DEFAULT, look up default C type from column's PG OID
    - Convert raw text to target C type:
      - SQL_C_CHAR / SQL_C_WCHAR: copy text to buffer with null terminator, set *indicator_or_length to actual string length; if truncated return SQL_SUCCESS_WITH_INFO with "01004" diagnostic
      - SQL_C_SLONG / SQL_C_LONG: atol(raw_value), write to *(SQLINTEGER*)target_value, set *indicator_or_length = sizeof(SQLINTEGER)
      - SQL_C_ULONG: strtoul(raw_value), similar pattern
      - SQL_C_SSHORT: (SQLSMALLINT)atoi(raw_value), *indicator_or_length = sizeof(SQLSMALLINT)
      - SQL_C_DOUBLE: strtod(raw_value), *indicator_or_length = sizeof(SQLDOUBLE)
      - SQL_C_FLOAT: (float)strtod(raw_value), *indicator_or_length = sizeof(SQLREAL)
      - SQL_C_BIT: value "t" or "1" → 1, else → 0
      - SQL_C_SBIGINT: strtoll(raw_value), *indicator_or_length = sizeof(SQLBIGINT)
      - For unsupported target types: add diagnostic "HY003" (Program type out of range), return SQL_ERROR

### 4. Wire Up ODBC API Layer
- **Task ID**: wire-odbc-api
- **Depends On**: implement-results
- **Assigned To**: builder-results
- **Agent Type**: builder
- **Parallel**: false
- Update `src/odbc_api.c`:
  - Add `#include "results.h"`
  - Implement SQLNumResultCols: validate statement handle, clear diagnostics, call results_num_result_cols. Doc comment with spec URL: https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlnumresultcols-function
  - Implement SQLDescribeCol: validate statement handle, clear diagnostics, call results_describe_col. Doc comment with spec URL: https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqldescribecol-function
  - Implement SQLRowCount: validate statement handle, clear diagnostics, call results_row_count. Doc comment with spec URL: https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlrowcount-function
  - Implement SQLFetch: validate statement handle, clear diagnostics, call results_fetch. Doc comment with spec URL: https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlfetch-function
  - Implement SQLGetData: validate statement handle, clear diagnostics, call results_get_data. Doc comment with spec URL: https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlgetdata-function
- Update `psqlodbc2.def` — add exports:
  - SQLNumResultCols @13
  - SQLDescribeCol   @14
  - SQLRowCount      @15
  - SQLFetch         @16
  - SQLGetData       @17
- Update `src/meson.build` — add 'results.c' and 'type_mapping.c' to driver_sources

### 5. Write Tests
- **Task ID**: implement-tests
- **Depends On**: wire-odbc-api
- **Assigned To**: builder-results
- **Agent Type**: builder
- **Parallel**: false
- Create `tests/test_results.c` (requires live database — skips if PSQLODBC2_TEST_CONNSTR not set):
  - Test SQLNumResultCols: execute "SELECT 1 AS a, 2 AS b, 3 AS c" → expect column_count = 3
  - Test SQLDescribeCol: on same result, describe column 1 → expect name="a", type=SQL_INTEGER (or SQL_SMALLINT, depends on PG inference), size > 0
  - Test SQLDescribeCol: describe column 2 → verify name="b"
  - Test SQLDescribeCol with typed columns: execute "SELECT 'hello'::varchar(50) AS name, 3.14::float8 AS pi" → verify name column is SQL_VARCHAR with size=50, pi column is SQL_DOUBLE
  - Test SQLRowCount after INSERT: create temp table, insert rows, verify row_count = number inserted
  - Test SQLRowCount after SELECT: should be number of rows (or implementation-defined)
  - Test SQLFetch: execute "SELECT generate_series(1,3)" → fetch 3 times expecting SQL_SUCCESS, 4th fetch returns SQL_NO_DATA
  - Test SQLGetData with SQL_C_CHAR: execute "SELECT 'hello world'" → fetch → get_data → verify string matches
  - Test SQLGetData with SQL_C_SLONG: execute "SELECT 42" → fetch → get_data as SQL_C_SLONG → verify integer value is 42
  - Test SQLGetData with SQL_C_DOUBLE: execute "SELECT 3.14159" → fetch → get_data → verify value ≈ 3.14159
  - Test SQLGetData NULL handling: execute "SELECT NULL::int" → fetch → get_data → verify indicator = SQL_NULL_DATA
  - Test SQLGetData string truncation: get a long string into a small buffer → verify SQL_SUCCESS_WITH_INFO and indicator shows full length
  - Test SQLFetch after SQL_CLOSE: execute query, fetch, SQL_CLOSE, verify next fetch on new query works
- Update `tests/meson.build`: add new test executable

### 6. Review Code Quality
- **Task ID**: review-code-quality
- **Depends On**: implement-tests
- **Assigned To**: reviewer-results
- **Agent Type**: reviewer
- **Parallel**: false
- Review all new and modified files for C11 compliance
- Verify naming: all names descriptive and self-documenting (no abbreviations)
- Check ODBC API functions have doc comments with spec reference URLs
- Verify type mapping coverage: ensure all common PG types are mapped
- Verify GetData conversion is safe: no buffer overflows, handles edge cases (empty strings, max-length values)
- Check NULL handling is correct per ODBC spec (indicator must be set to SQL_NULL_DATA)
- Verify SQL_SUCCESS_WITH_INFO is returned correctly for string truncation with "01004" SQLSTATE
- Check that SQLDescribeCol handles column_number=0 (bookmark column) appropriately (error or stub)
- Verify SQLFetch returns SQL_NO_DATA correctly and doesn't allow fetching before first row

### 7. Validate Build and Tests
- **Task ID**: validate-all
- **Depends On**: review-code-quality
- **Assigned To**: validator-results
- **Agent Type**: validator
- **Parallel**: false
- Run `meson setup builddir --reconfigure` — must succeed
- Run `meson compile -C builddir` — must compile with zero errors or warnings
- Run `meson test -C builddir` — all tests must pass (results tests skip gracefully if no database)
- Verify exports: `nm -gU builddir/src/libpsqlodbc2w.dylib | grep SQL` shows all 17 functions
- Verify existing tests (driver_load, connection_string, connection_lifecycle, statement_lifecycle) still pass
- Readability check: verify all names are descriptive, comments explain intent
- ODBC API documentation check: verify SQLFetch, SQLGetData, SQLNumResultCols, SQLDescribeCol, SQLRowCount all have spec URLs

## Acceptance Criteria
- `meson compile -C builddir` produces the shared library with zero errors/warnings
- `meson test -C builddir` — all tests pass (results tests skip gracefully without database)
- SQLNumResultCols returns the correct column count for SELECT queries
- SQLDescribeCol returns correct column names, SQL types, and sizes for common PostgreSQL types (int4, int8, varchar, text, float8, bool, numeric, timestamp)
- SQLRowCount returns the affected row count for INSERT/UPDATE/DELETE
- SQLFetch advances row-by-row and returns SQL_NO_DATA at end of result set
- SQLGetData correctly converts PostgreSQL text-format values to SQL_C_CHAR, SQL_C_SLONG, SQL_C_DOUBLE, SQL_C_SSHORT, SQL_C_FLOAT, SQL_C_BIT, SQL_C_SBIGINT
- SQLGetData reports NULL values via SQL_NULL_DATA indicator
- SQLGetData reports string truncation via SQL_SUCCESS_WITH_INFO with "01004" SQLSTATE
- SQLGetDiagRec works on statement handles after results errors
- All existing tests continue to pass (backward compatible)
- All ODBC API functions have doc comments with Microsoft spec reference URLs
- All names are descriptive — no cryptic abbreviations
- Type mapping table covers at least: bool, int2, int4, int8, float4, float8, numeric, varchar, text, bpchar, date, time, timestamp, timestamptz, bytea

## Validation Commands
- `meson setup builddir --reconfigure` - Reconfigure build
- `meson compile -C builddir` - Compile
- `meson test -C builddir` - Run tests
- `nm -gU builddir/src/libpsqlodbc2w.dylib | grep SQL` - Verify exports (macOS)
- `nm -gU builddir/src/libpsqlodbc2w.dylib | grep -c SQL` - Count exports (should be 17)

## Notes
- The original psqlodbc has extremely complex data conversion logic in convert.c (~3000 lines) handling every possible PG-to-C type combination, encodings, binary format, bookmarks, and large objects. Our initial implementation handles only text-format results with the most common C type conversions. Binary format and complex types (arrays, composites, intervals) will be added later.
- PGresult stores all data in text format by default (we pass format=0 to PQexecPrepared). This means all values come back as strings. Conversion to C types happens in results_get_data.
- SQLDescribeCol's nullable field reports SQL_NULLABLE_UNKNOWN because determining actual column nullability requires querying pg_attribute, which is expensive and not always possible (e.g., for computed columns). The original psqlodbc does this via its FIELD_INFO cache, which we don't have yet.
- For varchar(N) columns, PQfmod returns N+4 (typmod includes a 4-byte header). So column size = PQfmod - 4. For unmodified types (PQfmod == -1), column size is reported as 0 or a type-specific default.
- For numeric(precision,scale), typmod encodes both: precision = ((typmod-4) >> 16) & 0xFFFF, scale = (typmod-4) & 0xFFFF.
- SQLFetch in this implementation does NOT support column binding (SQLBindCol). Data retrieval is only via SQLGetData. Column binding will be added when we implement the ARD (Application Row Descriptor).
- The cursor position starts at -1 (before the first row). Each SQLFetch increments it. When current_row_position reaches PQntuples, SQL_NO_DATA is returned. There is no backward scrolling (SQLFetchScroll) in this implementation.
- SQL_C_DEFAULT for target_type means "use the default C type for this column's SQL type." This is looked up via type_mapping_get_default_c_type.
