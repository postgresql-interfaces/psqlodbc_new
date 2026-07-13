# Plan: Error Handling — SQLSTATE Mapping from libpq

## Task Description
Improve error handling by extracting proper SQLSTATE codes from libpq's PGresult error fields, constructing rich diagnostic messages with detail/hint information, and correctly propagating PostgreSQL error context to ODBC applications.

## Objective
When this plan is complete:
1. SQLGetDiagRec returns the actual PostgreSQL SQLSTATE (e.g., "23505" for unique violation) instead of generic "HY000"
2. Error messages include detail and hint text from PostgreSQL when available
3. Connection errors use appropriate SQLSTATE codes from libpq (08001, 08006, etc.)
4. Statement execution errors propagate the server's SQLSTATE faithfully
5. Native error codes map to PostgreSQL's error position information

## Problem Statement
Currently all errors from libpq are reported with generic SQLSTATE "HY000" (General error) or hardcoded states. PostgreSQL returns rich error information via PQresultErrorField (SQLSTATE, detail, hint, position, context) that applications rely on for programmatic error handling (e.g., retry logic for serialization failures "40001", constraint violation detection "23xxx").

## Solution Approach
1. **Extract SQLSTATE** — use PQresultErrorField(PG_DIAG_SQLSTATE) to get the actual 5-char SQLSTATE from every PGresult error
2. **Rich messages** — combine primary message + detail + hint into the diagnostic message text
3. **Connection errors** — use PQresultErrorField on connection failures where possible, or map PQstatus codes to appropriate SQLSTATEs
4. **Fallback** — when no SQLSTATE is available (e.g., connection lost before response), use appropriate ODBC-defined defaults

## Relevant Files

### Original Reference (~/projects/psqlodbc)
- `connection.c` — handle_pgres_error() (lines 821-950): extracts PG_DIAG_SQLSTATE, constructs rich error messages with detail/hint/context
- `qresult.h` — QResultClass.sqlstate field
- `qresult.c` — Error propagation from PGresult to ODBC diagnostic records

### New Files (this project)
- `src/error_mapping.h` — Helper functions for extracting/formatting libpq errors
- `src/error_mapping.c` — Implementation of error extraction and message formatting

### Modified Files
- `src/statement.c` — Update handle_execution_result to use rich error extraction
- `src/connection.c` — Update connection_connect error handling
- `src/diagnostics.h` — Possibly extend DiagnosticRecord with additional fields
- `src/meson.build` — Add error_mapping.c

## Implementation Phases

### Phase 1: Foundation
- Create error_mapping module with function to extract full error info from PGresult
- Define helper: extract SQLSTATE, primary message, detail, hint, position
- Define helper: format combined message ("ERROR: primary\nDETAIL: detail\nHINT: hint")

### Phase 2: Core Implementation
- Update statement.c handle_execution_result to call error_mapping instead of hardcoded "HY000"
- Update connection.c connection_connect to extract SQLSTATE from connection failure
- Add statement position to diagnostic native_error_code field
- Handle cases where PGresult is NULL (connection lost → SQLSTATE "08S01")

### Phase 3: Integration & Polish
- Add tests: verify SQLSTATE propagation for known error scenarios
- Verify that applications can distinguish error types via SQLSTATE
- Handle the edge case where PG_DIAG_SQLSTATE returns NULL (use "HY000" fallback)

## Team Orchestration

- You operate as the team lead and orchestrate the team to execute the plan.
- You NEVER operate directly on the codebase. You use `Task` and `Task*` tools to deploy team members.
- The flow is: Builder implements → Reviewer checks quality → Validator verifies correctness.

### Team Members

- Builder
  - Name: builder-errors
  - Role: Implement SQLSTATE extraction and rich error formatting
  - Agent Type: builder
  - Resume: true
- Reviewer
  - Name: reviewer-errors
  - Role: Code quality and C11 best practices review
  - Agent Type: reviewer
  - Resume: false
- Validator
  - Name: validator-errors
  - Role: Build verification and behavioral correctness
  - Agent Type: validator
  - Resume: false

## Step by Step Tasks

- Execute every step in order, top to bottom.
- Before you start, run `TaskCreate` to create the initial task list.

### 1. Create Error Mapping Module
- **Task ID**: create-error-mapping
- **Depends On**: none
- **Assigned To**: builder-errors
- **Agent Type**: builder
- **Parallel**: false
- Create `src/error_mapping.h` declaring extraction functions
- Create `src/error_mapping.c` implementing:
  - `error_extract_from_result(PGresult *result, char *sqlstate, char *message_buf, size_t buf_size)` — pulls SQLSTATE + formatted message
  - `error_extract_from_connection(PGconn *conn, char *sqlstate, char *message_buf, size_t buf_size)` — for connection-level errors
  - Message format: "ERROR: <primary>\nDETAIL: <detail>\nHINT: <hint>" (only include non-NULL fields)

### 2. Integrate with Statement and Connection
- **Task ID**: integrate-errors
- **Depends On**: create-error-mapping
- **Assigned To**: builder-errors
- **Agent Type**: builder
- **Parallel**: false
- Update handle_execution_result in statement.c to use error_extract_from_result
- Update connection_connect in connection.c to use error_extract_from_connection
- Replace all hardcoded "HY000" with extracted SQLSTATE where a PGresult is available
- Keep "HY000" only for truly driver-internal errors (memory allocation, invalid state, etc.)

### 3. Test and Validate
- **Task ID**: validate-all
- **Depends On**: integrate-errors
- **Assigned To**: validator-errors
- **Agent Type**: validator
- **Parallel**: false
- Build the project
- Run all tests
- Verify that syntax errors produce SQLSTATE "42601"
- Verify that connection failures produce "08001" or "08006"

## Acceptance Criteria
- Syntax error in SQL produces SQLSTATE "42601" (not "HY000")
- Unique violation produces "23505"
- Connection refused produces "08001"
- Connection lost mid-query produces "08S01"
- Error messages include DETAIL and HINT when PostgreSQL provides them
- NULL PGresult (connection completely lost) falls back to "08S01"
- Driver-internal errors (bad state, memory) retain appropriate ODBC SQLSTATEs

## Validation Commands
- `meson setup builddir --reconfigure` - Configure build
- `meson compile -C builddir` - Compile
- `meson test -C builddir` - Run tests

## Notes
- PostgreSQL SQLSTATE codes follow the SQL standard (5 characters, first 2 = class). They map directly to ODBC SQLSTATEs in most cases.
- PG_DIAG_STATEMENT_POSITION returns a character offset into the query — we store this in native_error_code since ODBC has no dedicated field for it.
- The original psqlodbc has an `optional_errors` flag that controls how much detail is included. We always include all available detail for now.
- libpq-fe.h defines the PG_DIAG_* constants we need (PG_DIAG_SQLSTATE, PG_DIAG_MESSAGE_PRIMARY, PG_DIAG_MESSAGE_DETAIL, PG_DIAG_MESSAGE_HINT, PG_DIAG_STATEMENT_POSITION).
