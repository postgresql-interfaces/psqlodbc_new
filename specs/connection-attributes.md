# Plan: Connection Attributes (SQLSetConnectAttr / SQLGetConnectAttr)

## Task Description
Implement SQLSetConnectAttr and SQLGetConnectAttr to allow applications to configure connection behavior — autocommit mode, transaction isolation level, connection timeout, login timeout, and read-only mode.

## Objective
When this plan is complete:
1. Applications can set SQL_ATTR_AUTOCOMMIT (ON/OFF) to control transaction behavior
2. Applications can set SQL_ATTR_TXN_ISOLATION to choose isolation levels
3. Applications can query SQL_ATTR_CONNECTION_DEAD to check connection health
4. Applications can set SQL_ATTR_LOGIN_TIMEOUT and SQL_ATTR_CONNECTION_TIMEOUT
5. The driver issues BEGIN/COMMIT/ROLLBACK as appropriate when autocommit is off

## Problem Statement
The connection handle has `autocommit = true` but there's no way for applications to change it. Transaction control, isolation levels, and timeouts are fundamental ODBC features required by virtually all database applications.

## Solution Approach
1. **Autocommit management** — when autocommit is OFF, issue implicit BEGIN before first statement execution and COMMIT/ROLLBACK on SQLEndTran
2. **Transaction isolation** — translate ODBC isolation constants to PostgreSQL SET TRANSACTION ISOLATION LEVEL commands
3. **Connection attributes struct** — extend OdbcConnection with configurable attributes
4. **SQLEndTran** — implement for explicit COMMIT/ROLLBACK when autocommit is OFF

## Relevant Files

### Original Reference (~/projects/psqlodbc)
- `options.c` — PGAPI_SetConnectOption, PGAPI_GetConnectOption (autocommit, isolation)
- `pgapi30.c` — PGAPI_SetConnectAttr, PGAPI_GetConnectAttr
- `connection.c` — CC_begin, CC_commit, CC_abort, CC_set_autocommit

### New Files (this project)
- (No new files — modifications to existing modules)

### Modified Files
- `src/connection.h` — Add transaction state tracking, login_timeout, read_only
- `src/connection.c` — Add begin/commit/rollback helpers
- `src/odbc_api.c` — Add SQLSetConnectAttr, SQLGetConnectAttr, SQLEndTran exports
- `psqlodbc2.def` — Add new exports

## Implementation Phases

### Phase 1: Foundation
- Add transaction state enum (IDLE, IN_TRANSACTION, IN_ERROR) to connection
- Add attributes: login_timeout, connection_timeout, access_mode (read-only), txn_isolation
- Implement internal helpers: connection_begin(), connection_commit(), connection_rollback()

### Phase 2: Core Implementation
- Implement SQLSetConnectAttr dispatch for supported attributes
- Implement SQLGetConnectAttr dispatch
- Implement SQLEndTran (commit or rollback)
- When autocommit is OFF: track whether we're in a transaction, issue BEGIN before first execute
- When autocommit changes from OFF→ON: commit any open transaction

### Phase 3: Integration & Polish
- Wire up exports in odbc_api.c with doc comments
- Add tests for autocommit toggle, transaction commit/rollback
- Handle edge cases: setting attributes while connected vs. before connect

## Team Orchestration

- You operate as the team lead and orchestrate the team to execute the plan.
- You NEVER operate directly on the codebase. You use `Task` and `Task*` tools to deploy team members.
- The flow is: Builder implements → Reviewer checks quality → Validator verifies correctness.

### Team Members

- Builder
  - Name: builder-connattr
  - Role: Implement connection attributes and transaction management
  - Agent Type: builder
  - Resume: true
- Reviewer
  - Name: reviewer-connattr
  - Role: Code quality and C11 best practices review
  - Agent Type: reviewer
  - Resume: false
- Validator
  - Name: validator-connattr
  - Role: Build verification and behavioral correctness
  - Agent Type: validator
  - Resume: false

## Step by Step Tasks

- Execute every step in order, top to bottom.
- Before you start, run `TaskCreate` to create the initial task list.

### 1. Add Transaction Management to Connection
- **Task ID**: add-transaction-mgmt
- **Depends On**: none
- **Assigned To**: builder-connattr
- **Agent Type**: builder
- **Parallel**: false
- Add TransactionState enum (IDLE, ACTIVE, FAILED) to connection.h
- Add fields: transaction_state, txn_isolation, login_timeout, connection_timeout, access_mode
- Implement connection_begin_transaction(), connection_commit(), connection_rollback()
- These issue PQexec("BEGIN"), PQexec("COMMIT"), PQexec("ROLLBACK")

### 2. Implement Set/Get ConnectAttr
- **Task ID**: implement-attrs
- **Depends On**: add-transaction-mgmt
- **Assigned To**: builder-connattr
- **Agent Type**: builder
- **Parallel**: false
- Implement handler for SQL_ATTR_AUTOCOMMIT (set autocommit, commit if switching OFF→ON)
- Implement handler for SQL_ATTR_TXN_ISOLATION (SQL_TXN_READ_UNCOMMITTED through SERIALIZABLE)
- Implement handler for SQL_ATTR_CONNECTION_DEAD (read-only, check PQstatus)
- Implement handler for SQL_ATTR_LOGIN_TIMEOUT, SQL_ATTR_CONNECTION_TIMEOUT
- Implement handler for SQL_ATTR_ACCESS_MODE (SQL_MODE_READ_ONLY / READ_WRITE)
- Return SQL_ERROR with appropriate SQLSTATE for unsupported attributes

### 3. Implement SQLEndTran and Auto-BEGIN
- **Task ID**: implement-endtran
- **Depends On**: implement-attrs
- **Assigned To**: builder-connattr
- **Agent Type**: builder
- **Parallel**: false
- Add SQLEndTran export to odbc_api.c
- When autocommit is OFF and a statement executes: issue implicit BEGIN if not already in transaction
- SQLEndTran(SQL_COMMIT) → connection_commit()
- SQLEndTran(SQL_ROLLBACK) → connection_rollback()
- Handle SQL_HANDLE_ENV scope (commit/rollback all connections)

### 4. Wire Up and Test
- **Task ID**: wire-and-test
- **Depends On**: implement-endtran
- **Assigned To**: builder-connattr
- **Agent Type**: builder
- **Parallel**: false
- Add SQLSetConnectAttr, SQLGetConnectAttr, SQLEndTran to odbc_api.c and psqlodbc2.def
- Write tests: attribute set/get round-trip, autocommit state tracking

### 5. Validate
- **Task ID**: validate-all
- **Depends On**: wire-and-test
- **Assigned To**: validator-connattr
- **Agent Type**: validator
- **Parallel**: false
- Build the project
- Run all tests
- Verify behavioral compatibility with original

## Acceptance Criteria
- SQLSetConnectAttr(SQL_ATTR_AUTOCOMMIT, SQL_AUTOCOMMIT_OFF) disables autocommit
- When autocommit is OFF, first statement execution issues implicit BEGIN
- SQLEndTran(SQL_COMMIT) commits the transaction
- SQLEndTran(SQL_ROLLBACK) rolls back the transaction
- SQLGetConnectAttr(SQL_ATTR_CONNECTION_DEAD) returns accurate status
- Unsupported attributes return SQL_ERROR with SQLSTATE HY092

## Validation Commands
- `meson setup builddir --reconfigure` - Configure build
- `meson compile -C builddir` - Compile
- `meson test -C builddir` - Run tests

## Notes
- PostgreSQL does not support READ UNCOMMITTED — it maps to READ COMMITTED. We accept the ODBC constant but the actual isolation is READ COMMITTED.
- The original psqlodbc has complex savepoint management for per-statement rollback. We skip this initially — errors in a transaction require explicit ROLLBACK.
- SQL_ATTR_CURRENT_CATALOG could be supported later (returns current database name from PQdb()).
