# Plan: Query Parsing, ODBC Escapes, Connection Options, and Diagnostics

## Task Description
Complete the client-side query parser (quoting/identifiers/MS-Access rewrites), ODBC escape processing (function/call/date-time escapes with OUT params), remaining connection options (CvtNullDate, Protocol rollback modes, LFConversion), diagnostic idempotency, and async-enable rejection.

## Objective
When complete, these regression tests pass: `parse`, `quotes`, `odbc-escapes`, `cvtnulldate`, `error-rollback`, `diagnostic`, `async-enable`, `lfconversion`.

## Problem Statement
This is the "long tail" of smaller compatibility features: SQL parsing edge cases, ODBC escape clauses beyond the basics, connection-string options that alter conversion/transaction behavior, and correct diagnostic-record semantics.

## Solution Approach
1. **Parser (parse, quotes)** — quoted identifiers with doubled quotes, dollar-quoting (`$$`, `$tag$`), E-strings, `standard_conforming_strings` awareness, MS-Access boolean `= 1` rewrite, `INSERT INTO t () VALUES ()` → `DEFAULT VALUES`, FOR UPDATE / SELECT INTO detection.
2. **ODBC escapes (odbc-escapes)** — `{fn CONCAT/LOCATE/SUBSTRING/SPACE}`, `{call proc}`, `{ ? = call ... }` return value, `{d}`/`{t}`/`{ts}` literals, OUT/INOUT params, named parameters via IPD SQL_DESC_NAME.
3. **CvtNullDate (cvtnulldate)** — `AB=0x08` bit + UseServerSidePrepare; empty-string bound to a date param → NULL instead of current date.
4. **Protocol rollback (error-rollback)** — `Protocol=7.4-0/1/2` selecting rollback-on-error behavior: none / whole-transaction / statement-level via SAVEPOINT.
5. **Diagnostics (diagnostic)** — SQLGetDiagRec/GetDiagField idempotency (repeated calls don't clear state), long messages, connection-level diagnostics after backend death.
6. **async-enable** — SQL_ATTR_ASYNC_ENABLE: return SQL_ERROR for ON (unsupported), succeed for OFF.
7. **LFConversion (lfconversion)** — `CX=1` enables `\n`→`\r\n` expansion on string output, with correct length/truncation reporting.

## Relevant Files

### Original Reference (~/projects/psqlodbc)
- `parse.c` — client-side SQL parser
- `convert.c` — ODBC escape processing, `convert_linefeeds`, cvt_null_date
- `statement.c`/`connection.c` — savepoint/rollback logic
- `options.c` — async-enable, statement options
- `dlg_specific.c` — option bit definitions (BIT_CVT_NULL_DATE, Protocol, lf_conversion)
- `test/src/parse-test.c`, `quotes-test.c`, `odbc-escapes-test.c`, `cvtnulldate-test.c`, `error-rollback-test.c`, `diagnostic-test.c`, `async-enable-test.c`, `lfconversion-test.c`

### Modified Files
- `src/query_parser.c/.h` — parser edge cases, escape expansion, statement features
- `src/results.c`/`src/parameter.c` — LF conversion, CvtNullDate
- `src/connection.c/.h` — Protocol rollback modes, LFConversion/CvtNullDate options, savepoint logic
- `src/odbc_api.c` — async-enable stmt attr, diagnostic idempotency
- `src/connection_string.c` — parse new options (Protocol, CX, AB)
- `psqlodbc2.def` — any new exports

## Implementation Phases

### Phase 1: Parser + escapes (fixes parse, quotes, odbc-escapes)
- Dollar-quoting, E-strings, quoted identifiers in query_parser
- MS-Access rewrites, DEFAULT VALUES, FOR UPDATE detection
- Full ODBC escape expansion including {call} and OUT params, named params

### Phase 2: Connection options (fixes cvtnulldate, error-rollback, lfconversion)
- CvtNullDate: empty string → NULL for date params
- Protocol rollback modes with SAVEPOINT for statement-level
- LFConversion: \n → \r\n output expansion

### Phase 3: Diagnostics + async (fixes diagnostic, async-enable)
- Diagnostic record idempotency (don't clear on repeated GetDiagRec)
- Long message handling, connection-death diagnostics
- SQL_ATTR_ASYNC_ENABLE: ERROR on ON, OK on OFF

## Team Orchestration
- Builder implements → Reviewer checks quality → Validator verifies correctness.

### Team Members
- Builder
  - Name: builder-parse
  - Role: Implement parser, escapes, options, diagnostics
  - Agent Type: builder
  - Resume: true
- Reviewer
  - Name: reviewer-parse
  - Role: Code quality and C11 best practices review
  - Agent Type: reviewer
  - Resume: false
- Validator
  - Name: validator-parse
  - Role: Build and regression verification
  - Agent Type: validator
  - Resume: false

## Step by Step Tasks

### 1. Parser + ODBC Escapes
- **Task ID**: implement-parser-escapes
- **Depends On**: none
- **Assigned To**: builder-parse
- **Agent Type**: builder
- **Parallel**: false
- Parser edge cases and full escape expansion
- Verify parse, quotes, odbc-escapes pass

### 2. Connection Options
- **Task ID**: implement-options
- **Depends On**: none
- **Assigned To**: builder-parse
- **Agent Type**: builder
- **Parallel**: true
- CvtNullDate, Protocol rollback, LFConversion
- Verify cvtnulldate, error-rollback, lfconversion pass

### 3. Diagnostics + async-enable
- **Task ID**: implement-diagnostics
- **Depends On**: none
- **Assigned To**: builder-parse
- **Agent Type**: builder
- **Parallel**: true
- Diagnostic idempotency, async-enable rejection
- Verify diagnostic, async-enable pass

### 4. Validate
- **Task ID**: validate-all
- **Depends On**: implement-parser-escapes, implement-options, implement-diagnostics
- **Assigned To**: validator-parse
- **Agent Type**: validator
- **Parallel**: false
- Build, unit tests, 8 target regression tests plus currently-passing suite

## Acceptance Criteria
- `parse`, `quotes`, `odbc-escapes`, `cvtnulldate`, `error-rollback`, `diagnostic`, `async-enable`, `lfconversion` pass
- No regression in currently-passing tests
- SQLGetDiagRec is idempotent across repeated calls
- SQL_ATTR_ASYNC_ENABLE_ON returns SQL_ERROR

## Validation Commands
- `meson compile -C builddir`
- `meson test -C builddir`
- `export PATH="/usr/local/pgsql/18/bin:$PATH" && ./regress/run_regression.sh parse quotes odbc-escapes cvtnulldate error-rollback diagnostic async-enable lfconversion`

## Notes
- These are largely independent small features — the three phases can proceed in parallel by the same builder.
- The parser work extends the existing query_parser.c (which already handles ? → $N and quote/comment tracking); add dollar-quoting and the MS-Access rewrites there.
- error-rollback's statement-level mode uses SAVEPOINT before each statement and ROLLBACK TO on error — this ties into the transaction management already in connection.c.
