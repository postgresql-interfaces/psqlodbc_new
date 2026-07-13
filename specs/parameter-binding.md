# Plan: Parameter Binding (SQLBindParameter)

## Task Description
Implement parameter binding for prepared statements and parameterized queries. This allows applications to bind C variables to parameter markers (`$1`, `$2`, etc. or `?`) in SQL statements, then have the driver convert and pass those values to PostgreSQL when executing.

## Objective
When this plan is complete:
1. Applications can call SQLBindParameter to bind C variables to statement parameters
2. SQLExecute passes bound parameter values to PQexecPrepared via the params arrays
3. SQLExecDirect with parameters uses PQexecParams instead of PQexec
4. Type conversion from ODBC C types to PostgreSQL text format works for common types
5. NULL parameters are handled correctly via indicator variables
6. Re-execution with different parameter values works without re-preparing

## Problem Statement
The statement module currently passes `0, NULL, NULL, NULL, NULL` to PQexecPrepared and uses PQexec (no params) for direct execution. Applications that use parameterized queries — which is the standard and secure way to pass user data — cannot function.

## Solution Approach
1. **Parameter descriptor struct** — stores per-parameter binding info: C type, SQL type, buffer pointer, buffer length, indicator/length pointer
2. **Parameter array on the statement** — fixed-size array of parameter bindings (matching MAX_PARAMS)
3. **Build params for libpq** — before execution, iterate bound parameters, convert C values to text strings, build the parallel arrays (values, lengths, formats) required by PQexecPrepared/PQexecParams
4. **Type conversion** — convert C types (SQL_C_SLONG, SQL_C_CHAR, SQL_C_DOUBLE, etc.) to PostgreSQL text representation
5. **SQLFreeStmt(SQL_RESET_PARAMS)** — clear all parameter bindings

## Relevant Files

### Original Reference (~/projects/psqlodbc)
- `bind.c` — PGAPI_BindParameter implementation
- `bind.h` — ParameterInfoClass, ParameterImplClass structs
- `convert.c` — build_libpq_bind_params(), ResolveOneParam()
- `execute.c` — Exec_with_parameters_resolved(), parameter passing to libpq
- `statement.h` — APDFields, parameter storage on statement

### New Files (this project)
- `src/parameter.h` — ParameterBinding struct, parameter management declarations
- `src/parameter.c` — Bind, unbind, convert, and build libpq param arrays

### Modified Files
- `src/statement.h` — Add parameter bindings array and count to OdbcStatement
- `src/statement.c` — Pass bound params to PQexecPrepared/PQexecParams
- `src/odbc_api.c` — Add SQLBindParameter export, update SQLFreeStmt SQL_RESET_PARAMS
- `src/meson.build` — Add parameter.c
- `psqlodbc2.def` — Add SQLBindParameter export

## Implementation Phases

### Phase 1: Foundation
- Define ParameterBinding struct (C type, SQL type, buffer pointer, buffer length, indicator pointer, column size, decimal digits)
- Add parameter storage to OdbcStatement (array + count + max)
- Implement bind/unbind logic

### Phase 2: Core Implementation
- Implement type conversion: C values → PostgreSQL text format for each supported C type
- Build libpq parameter arrays (const char **values, int *lengths, int *formats) from bindings
- Update statement_execute to pass parameters to PQexecPrepared
- Update statement_exec_direct to use PQexecParams when parameters are bound
- Handle SQL_NULL_DATA indicator
- Handle SQL_NTS for string parameters

### Phase 3: Integration & Polish
- Wire up SQLBindParameter in odbc_api.c
- Implement SQL_RESET_PARAMS in statement_free_stmt
- Add tests for parameter binding (connection string parsing style — no live DB needed for struct tests; live DB for execution tests)
- Handle edge cases: re-binding, binding subset of params, parameter count mismatch

## Team Orchestration

- You operate as the team lead and orchestrate the team to execute the plan.
- You NEVER operate directly on the codebase. You use `Task` and `Task*` tools to deploy team members.
- The flow is: Builder implements → Reviewer checks quality → Validator verifies correctness.

### Team Members

- Builder
  - Name: builder-parameters
  - Role: Implement parameter binding, type conversion, and libpq param array building
  - Agent Type: builder
  - Resume: true
- Reviewer
  - Name: reviewer-parameters
  - Role: Code quality and C11 best practices review
  - Agent Type: reviewer
  - Resume: false
- Validator
  - Name: validator-parameters
  - Role: Build verification and behavioral correctness
  - Agent Type: validator
  - Resume: false

## Step by Step Tasks

- Execute every step in order, top to bottom.
- Before you start, run `TaskCreate` to create the initial task list.

### 1. Create Parameter Module
- **Task ID**: create-parameter-module
- **Depends On**: none
- **Assigned To**: builder-parameters
- **Agent Type**: builder
- **Parallel**: false
- Create `src/parameter.h` with ParameterBinding struct (parameter_number, c_type, sql_type, buffer_ptr, buffer_length, indicator_ptr, column_size, decimal_digits)
- Define MAX_PARAMETERS (256) constant
- Declare functions: parameter_bind(), parameter_unbind_all(), parameter_build_libpq_arrays(), parameter_free_libpq_arrays()
- Create `src/parameter.c` implementing bind logic and type conversion

### 2. Integrate with Statement Module
- **Task ID**: integrate-statement
- **Depends On**: create-parameter-module
- **Assigned To**: builder-parameters
- **Agent Type**: builder
- **Parallel**: false
- Add ParameterBinding array and bound_parameter_count to OdbcStatement
- Update statement_execute to call parameter_build_libpq_arrays and pass to PQexecPrepared
- Update statement_exec_direct to use PQexecParams when parameters are bound
- Implement SQL_RESET_PARAMS in statement_free_stmt (clear all bindings)
- Free parameter memory in statement_free

### 3. Wire Up ODBC API
- **Task ID**: wire-odbc-api
- **Depends On**: integrate-statement
- **Assigned To**: builder-parameters
- **Agent Type**: builder
- **Parallel**: false
- Add SQLBindParameter export to odbc_api.c with full ODBC doc comment
- Update psqlodbc2.def with SQLBindParameter
- Update src/meson.build with parameter.c

### 4. Write Tests
- **Task ID**: write-tests
- **Depends On**: wire-odbc-api
- **Assigned To**: builder-parameters
- **Agent Type**: builder
- **Parallel**: false
- Test parameter binding struct management (bind, rebind, unbind)
- Test type conversion (int→text, double→text, string pass-through, NULL handling)
- Test libpq array building from bound parameters

### 5. Review and Validate
- **Task ID**: validate-all
- **Depends On**: write-tests
- **Assigned To**: validator-parameters
- **Agent Type**: validator
- **Parallel**: false
- Build the project
- Run all tests
- Verify behavioral compatibility with original

## Acceptance Criteria
- SQLBindParameter stores binding info on the statement handle
- SQLExecute with bound parameters passes them to PQexecPrepared correctly
- SQLExecDirect with bound parameters uses PQexecParams
- NULL parameters (SQL_NULL_DATA indicator) produce NULL in PostgreSQL
- String, integer, float, double, and bigint C types convert correctly
- SQL_RESET_PARAMS clears all bindings
- Re-execution with changed parameter values works
- No memory leaks in bind/unbind/free cycle

## Validation Commands
- `meson setup builddir --reconfigure` - Configure build
- `meson compile -C builddir` - Compile
- `meson test -C builddir` - Run tests

## Notes
- The original psqlodbc supports data-at-exec (SQL_DATA_AT_EXEC) for streaming large parameters. We skip this for the initial implementation.
- The original supports array binding (multiple parameter sets for batch operations). We skip this initially — single-row execution only.
- Parameter markers: PostgreSQL uses $1, $2 notation natively; ODBC uses ? markers. For now we require the application to use $N or rely on the driver manager for ? → $N translation. A future enhancement can add ODBC escape processing.
- We use text format (0) for all parameters — binary format optimization can be added later.
