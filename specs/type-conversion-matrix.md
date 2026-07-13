# Plan: Type Conversion Matrix and Numeric/Interval/WChar Support

## Task Description
Complete the PostgreSQL-type × ODBC-C-type conversion matrix so that every column type can be retrieved as any compatible C type, with correct truncation reporting, special-value handling, and precision control. This fixes the type-conversion family of regression tests.

## Objective
When complete, these regression tests pass: `result-conversions`, `numeric`, `interval-overflow`, `wchar-char`.

## Problem Statement
The current `convert_value_to_c_type` in `results.c` handles common types but is missing: SQL_C_NUMERIC (128-bit packed decimal), full SQL_C_WCHAR (UTF-16LE) conversion, complete interval subtype handling with precision clamping, GUID, and various edge cases (NaN/Inf floats, bytea hex vs escape, empty-string→current-date). The `result-conversions` test exercises every PG type against every C type.

## Solution Approach
Expand `convert_value_to_c_type` (and the parameter-side conversion in `parameter.c`) to cover the full matrix. Key additions:
1. **SQL_C_NUMERIC** — build/parse `SQL_NUMERIC_STRUCT` (sign, precision, scale, 128-bit little-endian `val[16]`). Convert PostgreSQL numeric text ↔ the packed struct.
2. **SQL_C_WCHAR** — convert UTF-8 (or client encoding) to UTF-16LE. Report length in bytes. Handle truncation on character boundaries.
3. **Interval subtypes** — all SQL_C_INTERVAL_* with fractional-second precision clamped to 9 (guard against the 10-byte stack overrun from GitHub #173). Read ARD SQL_DESC_PRECISION if set.
4. **Special float values** — NaN, Infinity, -Infinity for SQL_C_FLOAT/DOUBLE.
5. **bytea** — handle both hex (`\x...`) and escape formats for SQL_C_BINARY/CHAR.
6. **Truncation** — every string/binary conversion reports SQLSTATE 01004 SUCCESS_WITH_INFO with the full untruncated length in the indicator.

## Relevant Files

### Original Reference (~/projects/psqlodbc)
- `convert.c` — `copy_and_convert_field` (the master conversion function), numeric parse/format helpers, `getPrecisionPart` (interval clamp), `convert_linefeeds`
- `pgtypes.c` — type size/precision helpers
- `test/src/result-conversions-test.c`, `numeric-test.c`, `interval-overflow-test.c`, `wchar-char-test.c` and their `expected/*.out`

### Modified Files
- `src/results.c` — expand `convert_value_to_c_type` with the new C types and edge cases
- `src/parameter.c` — matching SQL_C_NUMERIC and SQL_C_WCHAR parameter conversion
- `src/type_mapping.c/.h` — helpers for numeric precision/scale, GUID
- `src/statement.h` — if ARD precision override is needed, a field to carry it

## Implementation Phases

### Phase 1: Numeric
- Implement SQL_C_NUMERIC output: parse PG numeric text ("123.45") into SQL_NUMERIC_STRUCT (compute sign, precision, scale, 128-bit mantissa little-endian). Overflow at 2^128.
- Implement SQL_C_NUMERIC input (parameter): format the packed struct back to decimal text.

### Phase 2: WChar
- UTF-8 → UTF-16LE conversion for SQL_C_WCHAR output (SQLGetData, bound columns).
- UTF-16LE → UTF-8 for SQL_C_WCHAR parameters.
- Truncation on character boundaries, byte-length reporting.

### Phase 3: Intervals + Special Values + bytea
- Complete interval subtype matrix with precision clamp to 9.
- NaN/Inf float formatting.
- bytea hex/escape both directions.
- Empty-string → current-date substitution (unless CvtNullDate — see separate spec).

## Team Orchestration

- Builder implements → Reviewer checks quality → Validator verifies correctness.

### Team Members
- Builder
  - Name: builder-conversions
  - Role: Implement the type conversion matrix additions
  - Agent Type: builder
  - Resume: true
- Reviewer
  - Name: reviewer-conversions
  - Role: Code quality and C11 best practices review
  - Agent Type: reviewer
  - Resume: false
- Validator
  - Name: validator-conversions
  - Role: Build and regression verification
  - Agent Type: validator
  - Resume: false

## Step by Step Tasks

### 1. Implement Numeric Conversion
- **Task ID**: implement-numeric
- **Depends On**: none
- **Assigned To**: builder-conversions
- **Agent Type**: builder
- **Parallel**: false
- Add SQL_C_NUMERIC to convert_value_to_c_type and parameter.c
- Verify `numeric` regression test passes

### 2. Implement WChar Conversion
- **Task ID**: implement-wchar
- **Depends On**: none
- **Assigned To**: builder-conversions
- **Agent Type**: builder
- **Parallel**: true
- UTF-8 ↔ UTF-16LE both directions
- Verify `wchar-char` regression test passes

### 3. Implement Intervals, Special Values, bytea
- **Task ID**: implement-intervals-etc
- **Depends On**: none
- **Assigned To**: builder-conversions
- **Agent Type**: builder
- **Parallel**: true
- Interval precision clamp, NaN/Inf, bytea formats
- Verify `interval-overflow` and `result-conversions` pass

### 4. Validate
- **Task ID**: validate-all
- **Depends On**: implement-numeric, implement-wchar, implement-intervals-etc
- **Assigned To**: validator-conversions
- **Agent Type**: validator
- **Parallel**: false
- Build, run unit tests, run the 4 target regression tests plus the currently-passing suite

## Acceptance Criteria
- `result-conversions`, `numeric`, `interval-overflow`, `wchar-char` regression tests pass
- No regression in currently-passing tests
- All 13 unit tests still pass
- Truncation reports SQLSTATE 01004 with full length in indicator

## Validation Commands
- `meson compile -C builddir`
- `meson test -C builddir`
- `export PATH="/usr/local/pgsql/18/bin:$PATH" && ./regress/run_regression.sh result-conversions numeric interval-overflow wchar-char`

## Notes
- SQL_NUMERIC_STRUCT val[] is little-endian 128-bit unsigned. Precision is total digits, scale is digits after decimal.
- The interval-overflow test specifically sets ARD SQL_DESC_PRECISION=20; the conversion must clamp fractional precision to 9 to avoid a stack buffer overrun.
- wchar-char round-trips through multiple client encodings (SJIS, UTF-8, EUC-JP). Focus on UTF-8 first (the CI/test default) and add others if the expected output requires them.
