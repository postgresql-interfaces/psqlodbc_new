# Plan: Catalog Functions Module

## Task Description
Implement the catalog functions module for the psqlodbc2 driver: SQLTables (list tables/views in the database), SQLColumns (describe columns of a table), SQLPrimaryKeys (get primary key columns), and SQLForeignKeys (get foreign key relationships). These functions query PostgreSQL's pg_catalog system tables internally and present the results as standard ODBC result sets that the application reads with SQLFetch/SQLGetData.

## Objective
When this plan is complete:
1. Applications can call SQLTables to list tables, views, and other relations matching a pattern
2. Applications can call SQLColumns to get column metadata (name, type, size, nullability, defaults) for a table
3. Applications can call SQLPrimaryKeys to discover primary key columns for a table
4. Applications can call SQLForeignKeys to discover foreign key relationships between tables
5. Each catalog function returns a well-defined ODBC result set with the standard column layout specified by the ODBC spec
6. Pattern matching (SQL LIKE patterns with % and _) is supported for table/schema/column name arguments
7. Schema qualification is supported (catalog_name, schema_name, table_name)

## Problem Statement
ODBC applications (especially GUI tools like DBeaver, DataGrip, Excel, and report generators) heavily rely on catalog functions to discover database structure. Without these functions, the driver cannot be used with any tool that needs to browse tables, inspect columns, or understand relationships. These four functions are the minimum set needed for basic schema discovery.

## Solution Approach
Each catalog function follows the same pattern:

1. **Build a SQL query** against PostgreSQL's `pg_catalog` system tables (pg_class, pg_namespace, pg_attribute, pg_type, pg_constraint, pg_index)
2. **Apply filter criteria** from the function arguments (table name pattern, schema pattern, etc.) using WHERE clauses with LIKE or = operators
3. **Execute the query** directly on the connection's libpq handle via PQexec
4. **Store the PGresult** on the statement handle (replacing any existing result) so the standard SQLFetch/SQLGetData/SQLDescribeCol path works

This approach is simpler than the original psqlodbc (which builds result sets manually tuple-by-tuple) because our driver already has working result retrieval infrastructure. We just need to craft the right SQL and execute it internally.

Key design decisions:
- Use `pg_catalog` tables directly (not `information_schema`) for performance and to access PostgreSQL-specific metadata
- The catalog query results have column names matching the ODBC spec (TABLE_CAT, TABLE_SCHEM, TABLE_NAME, etc.) via SQL AS aliases
- Pattern arguments (% and _) map directly to PostgreSQL's LIKE operator
- NULL pattern arguments mean "no filter" (match all)
- Empty string pattern means "match only empty string" (effectively matches nothing for schema/table names)

## Relevant Files

### Original Reference (~/projects/psqlodbc)
- `info.c` — PGAPI_Tables, PGAPI_Columns, PGAPI_PrimaryKeys, PGAPI_ForeignKeys (query construction and result building)
- `pgapi30.c` — ODBC 3.0 wrappers that call into the PGAPI_ functions

### New Files (this project)
- `src/catalog.h` — Catalog function declarations
- `src/catalog.c` — Implementation of SQLTables, SQLColumns, SQLPrimaryKeys, SQLForeignKeys using pg_catalog queries
- `tests/test_catalog.c` — Integration tests (requires live database)

### Modified Files
- `src/odbc_api.c` — Wire up SQLTables, SQLColumns, SQLPrimaryKeys, SQLForeignKeys
- `src/meson.build` — Add catalog.c to driver_sources
- `psqlodbc2.def` — Add new exports
- `tests/meson.build` — Add test_catalog executable

## Implementation Phases

### Phase 1: Foundation (Internal Execution Helper)
1. Create a helper function that builds a SQL string, executes it via PQexec on the statement's parent connection, and stores the resulting PGresult on the statement handle — replacing whatever was there before. This is the shared "execute catalog query" pattern all four functions use.
2. Create helper for escaping pattern arguments for safe use in LIKE/= clauses.

### Phase 2: Core Implementation (Four Catalog Functions)
1. SQLTables — query pg_class + pg_namespace with relkind filtering for TABLE/VIEW/MATERIALIZED VIEW/FOREIGN TABLE
2. SQLColumns — query pg_attribute + pg_type + pg_attrdef + pg_class + pg_namespace to get column metadata with ODBC-conformant column names
3. SQLPrimaryKeys — query pg_index + pg_attribute + pg_class + pg_constraint to find primary key columns
4. SQLForeignKeys — query pg_constraint with contype='f' to find foreign key relationships

### Phase 3: Integration & Polish
1. Wire up ODBC API entry points
2. Update exports
3. Write integration tests
4. Verify all existing tests still pass

## Code Examples

### Internal catalog query execution pattern:
```c
static SQLRETURN execute_catalog_query(OdbcStatement *statement, const char *query)
{
    /* Clear any previous result */
    if (statement->current_result) {
        PQclear(statement->current_result);
        statement->current_result = NULL;
    }

    PGconn *pgconn = statement->parent_connection->libpq_connection;
    PGresult *result = PQexec(pgconn, query);

    if (PQresultStatus(result) != PGRES_TUPLES_OK) {
        /* Set diagnostic, clean up */
        diagnostics_add_record(...);
        PQclear(result);
        return SQL_ERROR;
    }

    statement->current_result = result;
    statement->has_result_set = true;
    statement->affected_row_count = PQntuples(result);
    statement->current_row_position = -1;
    statement->state = STATEMENT_STATE_HAS_CURSOR;
    return SQL_SUCCESS;
}
```

### SQLTables query:
```sql
SELECT
    current_database() AS "TABLE_CAT",
    n.nspname AS "TABLE_SCHEM",
    c.relname AS "TABLE_NAME",
    CASE c.relkind
        WHEN 'r' THEN 'TABLE'
        WHEN 'v' THEN 'VIEW'
        WHEN 'm' THEN 'MATERIALIZED VIEW'
        WHEN 'f' THEN 'FOREIGN TABLE'
        WHEN 'p' THEN 'TABLE'
    END AS "TABLE_TYPE",
    pg_catalog.obj_description(c.oid, 'pg_class') AS "REMARKS"
FROM pg_catalog.pg_class c
JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace
WHERE c.relkind IN ('r','v','m','f','p')
  AND n.nspname NOT IN ('pg_catalog','information_schema','pg_toast')
  -- Pattern filters added dynamically
ORDER BY "TABLE_TYPE", "TABLE_SCHEM", "TABLE_NAME"
```

### SQLColumns query:
```sql
SELECT
    current_database() AS "TABLE_CAT",
    n.nspname AS "TABLE_SCHEM",
    c.relname AS "TABLE_NAME",
    a.attname AS "COLUMN_NAME",
    ... (SQL type mapping via CASE) AS "DATA_TYPE",
    t.typname AS "TYPE_NAME",
    ... (column size calculation) AS "COLUMN_SIZE",
    ... AS "BUFFER_LENGTH",
    ... AS "DECIMAL_DIGITS",
    ... AS "NUM_PREC_RADIX",
    CASE WHEN a.attnotnull THEN 0 ELSE 1 END AS "NULLABLE",
    pg_catalog.col_description(c.oid, a.attnum) AS "REMARKS",
    pg_catalog.pg_get_expr(d.adbin, d.adrelid) AS "COLUMN_DEF",
    ... AS "SQL_DATA_TYPE",
    ... AS "SQL_DATETIME_SUB",
    ... AS "CHAR_OCTET_LENGTH",
    a.attnum AS "ORDINAL_POSITION",
    CASE WHEN a.attnotnull THEN 'NO' ELSE 'YES' END AS "IS_NULLABLE"
FROM pg_catalog.pg_attribute a
JOIN pg_catalog.pg_class c ON a.attrelid = c.oid
JOIN pg_catalog.pg_namespace n ON c.relnamespace = n.oid
JOIN pg_catalog.pg_type t ON a.atttypid = t.oid
LEFT JOIN pg_catalog.pg_attrdef d ON (a.attrelid = d.adrelid AND a.attnum = d.adnum)
WHERE a.attnum > 0
  AND NOT a.attisdropped
  AND c.relkind IN ('r','v','m','f','p')
  -- Pattern filters added dynamically
ORDER BY "TABLE_SCHEM", "TABLE_NAME", "ORDINAL_POSITION"
```

### SQLPrimaryKeys query:
```sql
SELECT
    current_database() AS "TABLE_CAT",
    n.nspname AS "TABLE_SCHEM",
    c.relname AS "TABLE_NAME",
    a.attname AS "COLUMN_NAME",
    (array_position(i.indkey, a.attnum))::smallint AS "KEY_SEQ",
    ic.relname AS "PK_NAME"
FROM pg_catalog.pg_index i
JOIN pg_catalog.pg_class c ON c.oid = i.indrelid
JOIN pg_catalog.pg_class ic ON ic.oid = i.indexrelid
JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace
JOIN pg_catalog.pg_attribute a ON a.attrelid = c.oid AND a.attnum = ANY(i.indkey)
WHERE i.indisprimary
  AND n.nspname = '<schema>'
  AND c.relname = '<table>'
ORDER BY "KEY_SEQ"
```

### SQLForeignKeys query:
```sql
SELECT
    current_database() AS "PKTABLE_CAT",
    pn.nspname AS "PKTABLE_SCHEM",
    pc.relname AS "PKTABLE_NAME",
    pa.attname AS "PKCOLUMN_NAME",
    current_database() AS "FKTABLE_CAT",
    fn.nspname AS "FKTABLE_SCHEM",
    fc.relname AS "FKTABLE_NAME",
    fa.attname AS "FKCOLUMN_NAME",
    (row_number() OVER (PARTITION BY con.oid ORDER BY ordinality))::smallint AS "KEY_SEQ",
    CASE con.confupdtype WHEN 'a' THEN 3 WHEN 'r' THEN 0 WHEN 'c' THEN 0 WHEN 'n' THEN 2 WHEN 'd' THEN 4 END AS "UPDATE_RULE",
    CASE con.confdeltype WHEN 'a' THEN 3 WHEN 'r' THEN 0 WHEN 'c' THEN 0 WHEN 'n' THEN 2 WHEN 'd' THEN 4 END AS "DELETE_RULE",
    con.conname AS "FK_NAME",
    (SELECT ic.relname FROM pg_catalog.pg_index i JOIN pg_catalog.pg_class ic ON ic.oid = i.indexrelid WHERE i.indrelid = con.confrelid AND i.indisprimary) AS "PK_NAME",
    7 AS "DEFERRABILITY"
FROM pg_catalog.pg_constraint con
JOIN pg_catalog.pg_class fc ON fc.oid = con.conrelid
JOIN pg_catalog.pg_namespace fn ON fn.oid = fc.relnamespace
JOIN pg_catalog.pg_class pc ON pc.oid = con.confrelid
JOIN pg_catalog.pg_namespace pn ON pn.oid = pc.relnamespace
CROSS JOIN LATERAL unnest(con.conkey, con.confkey) WITH ORDINALITY AS cols(fk_attnum, pk_attnum, ordinality)
JOIN pg_catalog.pg_attribute fa ON fa.attrelid = con.conrelid AND fa.attnum = cols.fk_attnum
JOIN pg_catalog.pg_attribute pa ON pa.attrelid = con.confrelid AND pa.attnum = cols.pk_attnum
WHERE con.contype = 'f'
  -- Filter by PK table or FK table
ORDER BY "PKTABLE_SCHEM", "PKTABLE_NAME", "FK_NAME", "KEY_SEQ"
```

## Team Orchestration

- You operate as the team lead and orchestrate the team to execute the plan.
- You NEVER operate directly on the codebase. You use `Task` and `Task*` tools to deploy team members.
- The flow is: Builder implements → Reviewer checks quality → Validator verifies correctness.

### Team Members

- Builder
  - Name: builder-catalog
  - Role: Implement catalog functions, internal query execution, and tests
  - Agent Type: builder
  - Resume: true
- Reviewer
  - Name: reviewer-catalog
  - Role: Code quality, SQL correctness, ODBC spec column layout compliance, naming review
  - Agent Type: reviewer
  - Resume: false
- Validator
  - Name: validator-catalog
  - Role: Build verification, test execution, behavioral correctness validation
  - Agent Type: validator
  - Resume: false

## Step by Step Tasks

- Execute every step in order, top to bottom.
- Before you start, run `TaskCreate` to create the initial task list.

### 1. Implement Catalog Module with Internal Execution Helper
- **Task ID**: implement-catalog-foundation
- **Depends On**: none
- **Assigned To**: builder-catalog
- **Agent Type**: builder
- **Parallel**: false
- Create `src/catalog.h` declaring:
  - `catalog_tables(OdbcStatement *statement, const SQLCHAR *catalog_name, SQLSMALLINT catalog_name_length, const SQLCHAR *schema_pattern, SQLSMALLINT schema_length, const SQLCHAR *table_pattern, SQLSMALLINT table_length, const SQLCHAR *table_type, SQLSMALLINT type_length)` — returns SQL_SUCCESS/SQL_ERROR
  - `catalog_columns(OdbcStatement *statement, const SQLCHAR *catalog_name, SQLSMALLINT catalog_name_length, const SQLCHAR *schema_pattern, SQLSMALLINT schema_length, const SQLCHAR *table_pattern, SQLSMALLINT table_length, const SQLCHAR *column_pattern, SQLSMALLINT column_length)` — returns SQL_SUCCESS/SQL_ERROR
  - `catalog_primary_keys(OdbcStatement *statement, const SQLCHAR *catalog_name, SQLSMALLINT catalog_name_length, const SQLCHAR *schema_name, SQLSMALLINT schema_length, const SQLCHAR *table_name, SQLSMALLINT table_length)` — returns SQL_SUCCESS/SQL_ERROR
  - `catalog_foreign_keys(OdbcStatement *statement, const SQLCHAR *pk_catalog, SQLSMALLINT pk_catalog_length, const SQLCHAR *pk_schema, SQLSMALLINT pk_schema_length, const SQLCHAR *pk_table, SQLSMALLINT pk_table_length, const SQLCHAR *fk_catalog, SQLSMALLINT fk_catalog_length, const SQLCHAR *fk_schema, SQLSMALLINT fk_schema_length, const SQLCHAR *fk_table, SQLSMALLINT fk_table_length)` — returns SQL_SUCCESS/SQL_ERROR
- Create `src/catalog.c` with:
  - Static helper `execute_catalog_query(OdbcStatement *statement, const char *query)`: clear any previous result on statement, PQexec on parent connection, check for PGRES_TUPLES_OK, store PGresult on statement, set has_result_set=true, set current_row_position=-1, set state=HAS_CURSOR, set affected_row_count=PQntuples. On error: add diagnostic, PQclear, return SQL_ERROR.
  - Static helper `resolve_pattern_length(const SQLCHAR *value, SQLSMALLINT declared_length)`: if value is NULL return 0; if declared_length == SQL_NTS return strlen; if negative return 0; else return declared_length. Returns size_t.
  - Static helper `append_pattern_filter(char *buffer, size_t buffer_size, size_t *offset, const char *column_expression, const SQLCHAR *pattern_value, SQLSMALLINT pattern_length, const char *conjunction)`: if pattern_value is NULL, do nothing (no filter). Otherwise resolve the length, escape single quotes in the value (double them), and append ` <conjunction> <column_expression> LIKE '<escaped_value>'` to the buffer. For exact match (PrimaryKeys/ForeignKeys), use `= '<escaped_value>'` instead. Use a boolean parameter or separate helper for LIKE vs exact.
  - Static helper `escape_sql_literal(const char *input, size_t input_length, char *output, size_t output_size)`: copies input to output, doubling any single quotes. Returns the output length. This prevents SQL injection in the pattern arguments.

### 2. Implement SQLTables
- **Task ID**: implement-sql-tables
- **Depends On**: implement-catalog-foundation
- **Assigned To**: builder-catalog
- **Agent Type**: builder
- **Parallel**: false
- Implement `catalog_tables()` in `src/catalog.c`:
  - Build the SQL query selecting from pg_class + pg_namespace with the ODBC-required columns aliased:
    - `TABLE_CAT` (current_database())
    - `TABLE_SCHEM` (n.nspname)
    - `TABLE_NAME` (c.relname)
    - `TABLE_TYPE` (CASE on relkind: 'r'/'p' → 'TABLE', 'v' → 'VIEW', 'm' → 'MATERIALIZED VIEW', 'f' → 'FOREIGN TABLE')
    - `REMARKS` (obj_description)
  - Base WHERE: `c.relkind IN ('r','v','m','f','p') AND n.nspname NOT IN ('pg_catalog','information_schema','pg_toast')`
  - If schema_pattern is not NULL: add `AND n.nspname LIKE '<pattern>'`
  - If table_pattern is not NULL: add `AND c.relname LIKE '<pattern>'`
  - If table_type is not NULL: parse comma-separated types ('TABLE','VIEW', etc.), map back to relkinds, add filter like `AND c.relkind IN (...)`. For example: `'TABLE'` → `'r','p'`, `'VIEW'` → `'v'`, `'MATERIALIZED VIEW'` → `'m'`, `'FOREIGN TABLE'` → `'f'`
  - Handle special cases per ODBC spec:
    - If catalog_name = SQL_ALL_CATALOGS ("%" string) and schema/table/type are all empty strings → return one row per catalog (just current_database())
    - If schema_pattern = SQL_ALL_SCHEMAS ("%" string) and catalog/table/type are all empty → return list of schemas
    - If table_type = SQL_ALL_TABLE_TYPES ("%" string) and catalog/schema/table empty → return list of table types
    - For the initial implementation, these special enumerations can be simplified: return the standard query result filtered appropriately
  - ORDER BY TABLE_TYPE, TABLE_SCHEM, TABLE_NAME
  - Call execute_catalog_query with the built SQL
  - Return result

### 3. Implement SQLColumns
- **Task ID**: implement-sql-columns
- **Depends On**: implement-catalog-foundation
- **Assigned To**: builder-catalog
- **Agent Type**: builder
- **Parallel**: false
- Implement `catalog_columns()` in `src/catalog.c`:
  - Build the SQL query selecting from pg_attribute + pg_class + pg_namespace + pg_type + pg_attrdef with ODBC-required columns:
    - `TABLE_CAT` — current_database()
    - `TABLE_SCHEM` — n.nspname
    - `TABLE_NAME` — c.relname
    - `COLUMN_NAME` — a.attname
    - `DATA_TYPE` — CASE on t.oid mapping PG types to ODBC SQL type constants (use same OID→int mapping as our type_mapping module, but embedded in SQL as a CASE expression)
    - `TYPE_NAME` — t.typname
    - `COLUMN_SIZE` — CASE logic: for varchar use atttypmod-4; for numeric use (atttypmod-4)>>16; for int4 use 10; for int8 use 19; for float8 use 15; etc.
    - `BUFFER_LENGTH` — similar CASE based on type
    - `DECIMAL_DIGITS` — for numeric use (atttypmod-4)&0xFFFF; for timestamps use 6; else 0
    - `NUM_PREC_RADIX` — 10 for numeric types, NULL for others
    - `NULLABLE` — CASE WHEN a.attnotnull THEN 0 ELSE 1 END (SQL_NO_NULLS=0, SQL_NULLABLE=1)
    - `REMARKS` — col_description(c.oid, a.attnum)
    - `COLUMN_DEF` — pg_get_expr(d.adbin, d.adrelid)
    - `SQL_DATA_TYPE` — same as DATA_TYPE for most types
    - `SQL_DATETIME_SUB` — NULL (simplification)
    - `CHAR_OCTET_LENGTH` — for character types, same as COLUMN_SIZE; else NULL
    - `ORDINAL_POSITION` — a.attnum
    - `IS_NULLABLE` — 'YES' or 'NO'
  - Base WHERE: `a.attnum > 0 AND NOT a.attisdropped AND c.relkind IN ('r','v','m','f','p')`
  - If schema_pattern not NULL: add LIKE filter on n.nspname
  - If table_pattern not NULL: add LIKE filter on c.relname
  - If column_pattern not NULL: add LIKE filter on a.attname
  - Exclude pg_catalog/information_schema/pg_toast schemas (same as Tables)
  - ORDER BY TABLE_SCHEM, TABLE_NAME, ORDINAL_POSITION
  - Call execute_catalog_query

### 4. Implement SQLPrimaryKeys
- **Task ID**: implement-sql-primary-keys
- **Depends On**: implement-catalog-foundation
- **Assigned To**: builder-catalog
- **Agent Type**: builder
- **Parallel**: false
- Implement `catalog_primary_keys()` in `src/catalog.c`:
  - Build the SQL query selecting from pg_index + pg_class + pg_attribute + pg_namespace + pg_constraint with ODBC-required columns:
    - `TABLE_CAT` — current_database()
    - `TABLE_SCHEM` — n.nspname
    - `TABLE_NAME` — c.relname
    - `COLUMN_NAME` — a.attname
    - `KEY_SEQ` — (array_position(i.indkey, a.attnum))::smallint
    - `PK_NAME` — ic.relname (the index name serving as PK name)
  - JOINs: pg_index i JOIN pg_class c (indrelid) JOIN pg_class ic (indexrelid) JOIN pg_namespace n JOIN pg_attribute a (on c.oid with attnum in indkey)
  - WHERE: i.indisprimary = true
  - Exact match (= not LIKE) for schema_name and table_name since ODBC spec says PrimaryKeys uses ordinary arguments, not patterns
  - If table_name is NULL or empty: add diagnostic and return SQL_ERROR (table is required)
  - If schema_name is NULL: default to searching all non-system schemas or use current schema
  - ORDER BY KEY_SEQ
  - Call execute_catalog_query

### 5. Implement SQLForeignKeys
- **Task ID**: implement-sql-foreign-keys
- **Depends On**: implement-catalog-foundation
- **Assigned To**: builder-catalog
- **Agent Type**: builder
- **Parallel**: false
- Implement `catalog_foreign_keys()` in `src/catalog.c`:
  - Build the SQL query selecting from pg_constraint + pg_class + pg_namespace + pg_attribute with ODBC-required columns:
    - `PKTABLE_CAT`, `PKTABLE_SCHEM`, `PKTABLE_NAME`, `PKCOLUMN_NAME`
    - `FKTABLE_CAT`, `FKTABLE_SCHEM`, `FKTABLE_NAME`, `FKCOLUMN_NAME`
    - `KEY_SEQ` — position within the multi-column FK (use LATERAL unnest with ORDINALITY)
    - `UPDATE_RULE` — map confupdtype: 'a'→SQL_NO_ACTION(3), 'r'→SQL_RESTRICT(1), 'c'→SQL_CASCADE(0), 'n'→SQL_SET_NULL(2), 'd'→SQL_SET_DEFAULT(4)
    - `DELETE_RULE` — same mapping on confdeltype
    - `FK_NAME` — con.conname
    - `PK_NAME` — the primary key index name (subquery on pg_index where indisprimary)
    - `DEFERRABILITY` — CASE: con.condeferrable AND con.condeferred → SQL_INITIALLY_DEFERRED(5); con.condeferrable → SQL_INITIALLY_IMMEDIATE(6); else SQL_NOT_DEFERRABLE(7)
  - Use LATERAL unnest(con.conkey, con.confkey) WITH ORDINALITY to expand multi-column FKs into rows
  - Support two modes per ODBC spec:
    - If pk_table is specified (not NULL/empty): find all FKs that reference the given PK table (exported keys)
    - If fk_table is specified (not NULL/empty): find all FKs on the given FK table (imported keys)
    - If both are specified: find FKs from fk_table that reference pk_table (cross-reference)
    - If neither is specified: add diagnostic and return SQL_ERROR
  - Exact match (= not LIKE) for all name arguments
  - ORDER BY appropriate columns per mode (FKTABLE_SCHEM, FKTABLE_NAME, KEY_SEQ for exported keys; PKTABLE_SCHEM, PKTABLE_NAME, KEY_SEQ for imported keys)
  - Call execute_catalog_query

### 6. Wire Up ODBC API Layer
- **Task ID**: wire-odbc-api
- **Depends On**: implement-sql-tables, implement-sql-columns, implement-sql-primary-keys, implement-sql-foreign-keys
- **Assigned To**: builder-catalog
- **Agent Type**: builder
- **Parallel**: false
- Update `src/odbc_api.c`:
  - Add `#include "catalog.h"`
  - Implement SQLTables: validate statement handle, clear diagnostics, call catalog_tables. Doc comment with spec URL: https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqltables-function
  - Implement SQLColumns: validate statement handle, clear diagnostics, call catalog_columns. Doc comment with spec URL: https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlcolumns-function
  - Implement SQLPrimaryKeys: validate statement handle, clear diagnostics, call catalog_primary_keys. Doc comment with spec URL: https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlprimarykeys-function
  - Implement SQLForeignKeys: validate statement handle, clear diagnostics, call catalog_foreign_keys. Doc comment with spec URL: https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/sqlforeignkeys-function
- Update `psqlodbc2.def` — add exports:
  - SQLTables       @18
  - SQLColumns      @19
  - SQLPrimaryKeys  @20
  - SQLForeignKeys  @21
- Update `src/meson.build` — add 'catalog.c' to driver_sources

### 7. Write Tests
- **Task ID**: implement-tests
- **Depends On**: wire-odbc-api
- **Assigned To**: builder-catalog
- **Agent Type**: builder
- **Parallel**: false
- Create `tests/test_catalog.c` (requires live database — skips if PSQLODBC2_TEST_CONNSTR not set):
  - Setup: create temp table `test_cat_parent(id int PRIMARY KEY, name text)` and `test_cat_child(id int PRIMARY KEY, parent_id int REFERENCES test_cat_parent(id))`
  - Test SQLTables: call with schema pattern matching the temp schema → verify at least test_cat_parent and test_cat_child appear; fetch rows and verify TABLE_NAME column contains expected names
  - Test SQLTables with table type filter: call with table_type="TABLE" → verify results contain TABLEs
  - Test SQLColumns: call for test_cat_parent → verify 2 columns returned (id, name); verify column names, types, nullability
  - Test SQLColumns with column pattern: call with column_pattern="id" → verify only id columns returned
  - Test SQLPrimaryKeys: call for test_cat_parent → verify returns "id" with KEY_SEQ=1
  - Test SQLForeignKeys (imported keys): call with fk_table=test_cat_child → verify returns parent_id referencing test_cat_parent.id
  - Test SQLNumResultCols on catalog results: verify correct column count (5 for Tables, 18 for Columns, 6 for PrimaryKeys, 14 for ForeignKeys)
  - Cleanup: drop temp tables
- Update `tests/meson.build`: add test_catalog executable

### 8. Review Code Quality
- **Task ID**: review-code-quality
- **Depends On**: implement-tests
- **Assigned To**: reviewer-catalog
- **Agent Type**: reviewer
- **Parallel**: false
- Review all new and modified files for C11 compliance
- Verify SQL queries are correct: proper JOINs, correct column aliases per ODBC spec
- Check for SQL injection safety: all string inputs are escaped via the escape helper
- Verify naming: all names descriptive, no abbreviations
- Check ODBC API functions have doc comments with spec reference URLs
- Verify buffer overflow safety: query buffers are sized appropriately or dynamically allocated
- Check that NULL arguments are handled correctly (no filter applied)
- Verify error paths: diagnostics set with meaningful messages

### 9. Validate Build and Tests
- **Task ID**: validate-all
- **Depends On**: review-code-quality
- **Assigned To**: validator-catalog
- **Agent Type**: validator
- **Parallel**: false
- Run `meson setup builddir --reconfigure` — must succeed
- Run `meson compile -C builddir` — must compile with zero errors or warnings
- Run `meson test -C builddir` — all tests must pass (catalog tests skip gracefully without database)
- Verify exports: `nm -gU builddir/src/libpsqlodbc2w.dylib | grep SQL` shows all 21 functions
- Verify existing tests (driver_load, connection_string, connection_lifecycle, statement_lifecycle, statement_execution, results) still pass
- Readability check: verify all names are descriptive, comments explain intent
- ODBC API documentation check: verify all 4 new functions have spec URLs

## Acceptance Criteria
- `meson compile -C builddir` produces the shared library with zero errors/warnings
- `meson test -C builddir` — all tests pass (catalog tests skip gracefully without database)
- SQLTables returns rows with correct ODBC column layout (TABLE_CAT, TABLE_SCHEM, TABLE_NAME, TABLE_TYPE, REMARKS)
- SQLTables filters by schema pattern, table pattern, and table type
- SQLColumns returns correct column metadata matching ODBC spec column layout (18 columns)
- SQLColumns correctly reports column type, size, nullability, and default values
- SQLPrimaryKeys returns primary key columns with correct KEY_SEQ ordering
- SQLForeignKeys returns FK relationships with correct UPDATE_RULE/DELETE_RULE mapping
- Pattern arguments (% and _) work correctly via LIKE
- NULL pattern arguments match all (no filter applied)
- SQL injection is prevented by escaping single quotes in pattern values
- All existing tests continue to pass (backward compatible)
- All ODBC API functions have doc comments with Microsoft spec reference URLs
- All names are descriptive — no cryptic abbreviations
- Exported function count is 21

## Validation Commands
- `meson setup builddir --reconfigure` - Reconfigure build
- `meson compile -C builddir` - Compile
- `meson test -C builddir` - Run tests
- `nm -gU builddir/src/libpsqlodbc2w.dylib | grep SQL` - Verify exports (macOS)
- `nm -gU builddir/src/libpsqlodbc2w.dylib | grep -c SQL` - Count exports (should be 21)

## Notes
- The original psqlodbc's catalog functions are extremely complex (info.c is 5000+ lines) because they handle ODBC 2.x vs 3.x column naming, Unicode, bookmarks, internal result set building, and very old PostgreSQL versions. Our implementation is much simpler because:
  1. We only support ODBC 3.x column names
  2. We use PQexec and let the standard result infrastructure handle fetching
  3. We target PostgreSQL 12+ only (modern pg_catalog features available)
  4. We skip bookmark columns and Unicode complications
- The ODBC spec defines exact column layouts for each catalog function. The SQL queries MUST return columns with the exact names specified (case-sensitive quoted identifiers in SQL) so that SQLDescribeCol reports the correct names.
- SQLTables and SQLColumns accept pattern value arguments (% and _ are wildcards) while SQLPrimaryKeys and SQLForeignKeys accept ordinary value arguments (exact match, no wildcards).
- The query buffer size: catalog queries can be long (especially SQLColumns). Use a reasonably large static buffer (4096 bytes) or dynamically allocate. The original uses PQExpBuffer (dynamic); we can use a large static buffer since our queries have bounded size.
- The temp tables in tests use the pg_temp schema which may not match patterns. Tests should either use a known schema or search specifically for the temp table names.
- Foreign key UPDATE_RULE/DELETE_RULE use PostgreSQL's confupdtype/confdeltype single-char codes that map to ODBC integer constants: 'a'=NO_ACTION(3), 'r'=RESTRICT(1), 'c'=CASCADE(0), 'n'=SET_NULL(2), 'd'=SET_DEFAULT(4).
- `array_position()` is available in PostgreSQL 9.5+. Since we target PG 12+, this is safe. It gives us the 1-based position of an attribute number within the index key array, which is the KEY_SEQ value.
- LATERAL unnest with ORDINALITY is available in PostgreSQL 9.4+. This expands multi-column foreign keys into one row per column pair with correct ordering.
