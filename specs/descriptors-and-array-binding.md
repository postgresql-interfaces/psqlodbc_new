# Plan: Descriptor Handles and Array/Batch Parameter Binding

## Task Description
Implement ODBC descriptor handles (SQLGetDescRec/SetDescRec/SetDescField/GetDescField, SQL_HANDLE_DESC alloc/free) and array/batch parameter binding (SQL_ATTR_PARAMSET_SIZE with status arrays).

## Objective
When complete, these regression tests pass: `descrec`, `descriptors-free`, `arraybinding`, `params-batch-exec`.

## Problem Statement
Descriptors are the ODBC 3.x mechanism underlying column and parameter binding — each statement has four automatic descriptors (ARD, APD, IRD, IPD). Applications can read column metadata via the IRD and set bindings via the ARD/APD. Array binding executes one statement against multiple parameter sets in a single call.

## Solution Approach
1. **Descriptor handle** — a struct holding an array of descriptor records (one per column/parameter), each with fields (TYPE, OCTET_LENGTH, PRECISION, SCALE, DATA_PTR, INDICATOR_PTR, NAME, etc.). Each statement has 4 implicit descriptors.
2. **SQLGetStmtAttr(SQL_ATTR_APP_ROW_DESC etc.)** — return the descriptor handles.
3. **SQLGetDescRec/SetDescRec/GetDescField/SetDescField** — read/write descriptor records and fields. Reading the IRD gives column metadata; setting the ARD establishes bindings.
4. **SQLAllocHandle(SQL_HANDLE_DESC)** — explicit application descriptors; SQLFreeHandle reverts the statement to its implicit descriptor.
5. **Array binding** — SQL_ATTR_PARAMSET_SIZE + bound parameter arrays. Execute the statement once per parameter set, reporting per-row status via SQL_ATTR_PARAM_STATUS_PTR and count via SQL_ATTR_PARAMS_PROCESSED_PTR.
6. **Batch execution** — SQL_ATTR_PGOPT_BATCHSIZE controls how many parameter rows are sent per server round-trip.

## Relevant Files

### Original Reference (~/projects/psqlodbc)
- `descriptor.c` — `DC_Constructor`/`DC_Destructor`, `PGAPI_GetDescRec`, `PGAPI_SetDescRec`, `PGAPI_SetDescField`, `PGAPI_GetDescField`, ARD/APD/IRD/IPD field access
- `execute.c` — paramset loop (`paramset_size`, `next_param_row`, `param_processed_ptr`), batch execution
- `test/src/descrec-test.c`, `descriptors-free-test.c`, `arraybinding-test.c`, `params-batch-exec-test.c`

### New Files (this project)
- `src/descriptor.h/.c` — descriptor handle and record management

### Modified Files
- `src/statement.h/.c` — 4 implicit descriptors per statement, paramset execution loop
- `src/parameter.c` — array parameter binding with stride
- `src/odbc_api.c` — descriptor API exports, SQL_HANDLE_DESC alloc/free, paramset stmt attrs
- `src/connection.c/.h` — SQL_ATTR_PGOPT_BATCHSIZE
- `psqlodbc2.def` — new exports

## Implementation Phases

### Phase 1: Descriptor handles (fixes descrec, descriptors-free)
- Descriptor struct with records and fields
- 4 implicit descriptors per statement (ARD/APD/IRD/IPD)
- SQLGetStmtAttr returns descriptor handles
- SQLGetDescRec/SetDescRec/GetDescField/SetDescField
- SQLAllocHandle/FreeHandle(SQL_HANDLE_DESC); explicit desc reverts to implicit on free

### Phase 2: Array parameter binding (fixes arraybinding, params-batch-exec)
- SQL_ATTR_PARAMSET_SIZE, SQL_ATTR_PARAM_STATUS_PTR, SQL_ATTR_PARAMS_PROCESSED_PTR
- SQL_ATTR_PARAM_BIND_TYPE (column-wise)
- Execute-per-row loop with per-row status
- SQL_ATTR_PGOPT_BATCHSIZE batching
- DELETE ... RETURNING produces one result set per param set (SQLMoreResults interaction)

## Team Orchestration
- Builder implements → Reviewer checks quality → Validator verifies correctness.

### Team Members
- Builder
  - Name: builder-desc
  - Role: Implement descriptor handles and array binding
  - Agent Type: builder
  - Resume: true
- Reviewer
  - Name: reviewer-desc
  - Role: Code quality and C11 best practices review
  - Agent Type: reviewer
  - Resume: false
- Validator
  - Name: validator-desc
  - Role: Build and regression verification
  - Agent Type: validator
  - Resume: false

## Step by Step Tasks

### 1. Descriptor Handles
- **Task ID**: implement-descriptors
- **Depends On**: none
- **Assigned To**: builder-desc
- **Agent Type**: builder
- **Parallel**: false
- Descriptor module, 4 implicit descriptors, Get/Set DescRec/DescField, alloc/free
- Verify descrec, descriptors-free pass

### 2. Array Parameter Binding
- **Task ID**: implement-array-binding
- **Depends On**: implement-descriptors
- **Assigned To**: builder-desc
- **Agent Type**: builder
- **Parallel**: false
- PARAMSET_SIZE loop, status arrays, batch size
- Verify arraybinding, params-batch-exec pass

### 3. Validate
- **Task ID**: validate-all
- **Depends On**: implement-array-binding
- **Assigned To**: validator-desc
- **Agent Type**: validator
- **Parallel**: false
- Build, unit tests, 4 target regression tests plus currently-passing suite

## Acceptance Criteria
- `descrec`, `descriptors-free`, `arraybinding`, `params-batch-exec` pass
- No regression in currently-passing tests
- Freeing an explicit descriptor reverts the statement to its implicit one
- Array binding executes once per parameter set with correct status reporting

## Validation Commands
- `meson compile -C builddir`
- `meson test -C builddir`
- `export PATH="/usr/local/pgsql/18/bin:$PATH" && ./regress/run_regression.sh descrec descriptors-free arraybinding params-batch-exec`

## Notes
- The four descriptor types: ARD (app row - output bindings), APD (app param - input bindings), IRD (impl row - result metadata, read-only), IPD (impl param - parameter metadata).
- Our existing column_binding and parameter modules can be refactored to store their data IN the ARD/APD descriptors, or the descriptors can be a thin view over them. Keeping them as the backing store avoids duplication.
- Array binding builds on parameter binding — do descriptors first since array status pointers are descriptor-adjacent stmt attributes.
