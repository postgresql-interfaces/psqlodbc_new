#!/bin/bash
#
# Run the original psqlodbc regression tests against psqlodbc2.
#
# This script:
#   1. Creates a local odbcinst.ini registering our driver
#   2. Creates a local odbc.ini with a test DSN pointing to the local PG
#   3. Compiles the original test programs from ~/projects/psqlodbc/test/src
#   4. Loads the sample tables
#   5. Runs each test and compares output against expected results
#
# Prerequisites:
#   - psqlodbc2 built in ../builddir/
#   - PostgreSQL running on localhost:5432
#   - A database named contrib_regression accessible by current user
#   - The original psqlodbc source at ~/projects/psqlodbc
#
# Usage:
#   ./run_regression.sh [test_name ...]
#   If no test names given, runs all tests listed in the 'tests' file.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
# Location of the upstream psqlodbc source tree. Its test/ subdirectory supplies
# the regression test programs and expected outputs we run against our driver.
# Override PSQLODBC_ORIG_DIR in CI (where the source lives elsewhere).
ORIG_TEST_DIR="${PSQLODBC_ORIG_DIR:-${HOME}/projects/psqlodbc}/test"
BUILD_DIR="${PROJECT_DIR}/builddir"

# Configurable via environment
PG_HOST="${PG_HOST:-localhost}"
PG_PORT="${PG_PORT:-5432}"
PG_DATABASE="${PG_DATABASE:-contrib_regression}"
PG_USER="${PG_USER:-$(whoami)}"

# Verify prerequisites
if [ ! -d "$BUILD_DIR/src" ]; then
    echo "ERROR: Build directory not found. Run 'meson compile -C builddir' first."
    exit 1
fi

if [ ! -d "$ORIG_TEST_DIR/src" ]; then
    echo "ERROR: Original psqlodbc test sources not found at $ORIG_TEST_DIR"
    exit 1
fi

# Find the built driver shared library
DRIVER_LIB=""
for ext in so dylib dll; do
    if [ -f "$BUILD_DIR/src/libpsqlodbc2w.$ext" ]; then
        DRIVER_LIB="$BUILD_DIR/src/libpsqlodbc2w.$ext"
        break
    fi
done

if [ -z "$DRIVER_LIB" ]; then
    echo "ERROR: Cannot find built driver library in $BUILD_DIR/src/"
    exit 1
fi

echo "Driver: $DRIVER_LIB"
echo "Database: $PG_HOST:$PG_PORT/$PG_DATABASE"

# Set up working directory
WORK_DIR="$SCRIPT_DIR/work"
mkdir -p "$WORK_DIR/exe" "$WORK_DIR/results"

# Create odbcinst.ini
cat > "$WORK_DIR/odbcinst.ini" << EOF
[ODBC]
Trace = off
TraceFile =

[psqlodbc2]
Description = psqlodbc2 driver under test
Driver = $DRIVER_LIB
EOF

# Create odbc.ini
cat > "$WORK_DIR/odbc.ini" << EOF
[psqlodbc_test_dsn]
Description = psqlodbc2 regression test DSN
Driver = psqlodbc2
Trace = No
TraceFile =
Database = $PG_DATABASE
Servername = $PG_HOST
Username = $PG_USER
Password =
Port = $PG_PORT
ReadOnly = No
ConnSettings = set lc_messages='C'

[psqlodbc_test_dsn_ansi]
Description = psqlodbc2 regression test DSN (ansi)
Driver = psqlodbc2
Trace = No
TraceFile =
Database = $PG_DATABASE
Servername = $PG_HOST
Username = $PG_USER
Password =
Port = $PG_PORT
ReadOnly = No
ConnSettings = set lc_messages='C'
EOF

# Export ODBC environment to use our local configs.
# ODBCSYSINI is the directory containing odbcinst.ini (not the file path).
# ODBCINI is the full path to the odbc.ini file.
export ODBCSYSINI="$WORK_DIR"
export ODBCINI="$WORK_DIR/odbc.ini"

# Determine compiler flags
ODBC_CFLAGS=$(pkg-config --cflags odbc 2>/dev/null || echo "-I/usr/include")
ODBC_LIBS=$(pkg-config --libs odbc 2>/dev/null || echo "-lodbc")
PG_CFLAGS=$(pg_config --includedir 2>/dev/null | xargs -I{} echo "-I{}" || echo "")

CFLAGS="-g -O0 -Wall $ODBC_CFLAGS $PG_CFLAGS -I$ORIG_TEST_DIR/.."
LDFLAGS="$ODBC_LIBS"

# Compile common.o
echo "Compiling test harness..."
cc $CFLAGS -c "$ORIG_TEST_DIR/src/common.c" -o "$WORK_DIR/exe/common.o" 2>/dev/null || {
    # If config.h is needed, create a minimal one
    echo "#define HAVE_STDBOOL_H 1" > "$WORK_DIR/config.h"
    cc $CFLAGS -I"$WORK_DIR" -c "$ORIG_TEST_DIR/src/common.c" -o "$WORK_DIR/exe/common.o"
}

# Reset the database to the pristine sample-table state.
#
# This is called before EVERY test, not just once at the start, to give each
# test complete isolation. Tests share table names (e.g. testtab1) and some
# commit changes or leave a transaction mid-flight when they crash; without a
# per-test reset, one test's residue can perturb a later test's assertions,
# producing order-dependent flakiness (observed with fetch-refcursors, which
# reads and asserts exact rows of testtab1). Dropping and recreating the schema
# guarantees a deterministic starting point regardless of run order.
load_sample_tables() {
    psql -h "$PG_HOST" -p "$PG_PORT" -d "$PG_DATABASE" -q \
        -c "DROP SCHEMA public CASCADE; CREATE SCHEMA public;" 2>/dev/null || true
    psql -h "$PG_HOST" -p "$PG_PORT" -d "$PG_DATABASE" -q \
        -f "$ORIG_TEST_DIR/sampletables.sql" 2>/dev/null
}

echo "Loading sample tables..."
load_sample_tables

# Determine which tests to run.
#
# When no test names are given, run the FULL upstream regression suite so our
# coverage matches psqlodbc exactly. This list is the canonical set of 60 tests
# from upstream's test/tests manifest (the TESTBINS list), kept in the same
# order. Five *-test.c sources in upstream (deprecated, describe, prepare-rows,
# specialcolumns, timestamp) are intentionally excluded: they are not in the
# upstream manifest either (orphaned helpers / missing expected output).
#
# Tests that don't yet build or pass against our driver surface as SKIP/FAIL
# rather than being silently omitted -- that is what "same coverage" requires.
# If upstream adds or removes a test, update this list to match.
if [ $# -gt 0 ]; then
    TESTS="$@"
else
    TESTS="connect stmthandles select update commands multistmt getresult \
colattribute result-conversions prepare premature params \
leading-literal-numparams param-conversions parse identity notice \
arraybinding insertreturning dataatexecution boolsaschar cvtnulldate \
alter async-enable quotes cursors cursor-movement cursor-scrollable \
cursor-commit cursor-name cursor-block-delete bookmark ard-bookmark-oom \
declare-fetch-commit declare-fetch-block positioned-update \
bulkoperations catalogfunctions bindcol lfconversion cte errors \
error-rollback diagnostic numeric large-object \
large-object-data-at-exec odbc-escapes odbc-conformance wchar-char \
params-batch-exec fetch-refcursors descrec descriptors-free \
primarykeys-include interval-overflow conn-settings percent-decode \
dbms-version surrogate-pair"
fi

echo ""
echo "Running regression tests..."
echo "=============================="

PASS=0
FAIL=0
SKIP=0

for test_name in $TESTS; do
    src_file="$ORIG_TEST_DIR/src/${test_name}-test.c"
    if [ ! -f "$src_file" ]; then
        echo "SKIP: $test_name (source not found)"
        SKIP=$((SKIP + 1))
        continue
    fi

    # Compile the test
    exe_file="$WORK_DIR/exe/${test_name}-test"
    if ! cc $CFLAGS -I"$WORK_DIR" "$src_file" "$WORK_DIR/exe/common.o" -o "$exe_file" $LDFLAGS 2>/dev/null; then
        echo "SKIP: $test_name (compilation failed)"
        SKIP=$((SKIP + 1))
        continue
    fi

    # Reset to pristine sample-table state so this test is isolated from any
    # residue left by earlier tests (see load_sample_tables above).
    load_sample_tables

    # Run the test and capture output
    result_file="$WORK_DIR/results/${test_name}.out"
    if ! "$exe_file" > "$result_file" 2>&1; then
        # Test crashed or returned non-zero
        echo "FAIL: $test_name (crashed or returned error)"
        if [ -s "$result_file" ]; then
            echo "      Output: $(head -5 "$result_file" | tr '\n' ' ')"
        fi
        FAIL=$((FAIL + 1))
        continue
    fi

    # Compare against expected output. Upstream psqlodbc allows a test to have
    # several acceptable outputs: the base "<test>.out" plus numbered variants
    # "<test>_1.out", "<test>_2.out", ... (e.g. wchar-char differs per client
    # locale/encoding). A run passes if it matches ANY of these, mirroring the
    # original runsuite's rundiff() behavior.
    expected_file="$ORIG_TEST_DIR/expected/${test_name}.out"
    if [ ! -f "$expected_file" ]; then
        echo "PASS: $test_name (no expected file to compare)"
        PASS=$((PASS + 1))
        continue
    fi

    matched=""
    for candidate in "$expected_file" "$ORIG_TEST_DIR/expected/${test_name}"_*.out; do
        [ -f "$candidate" ] || continue
        # --strip-trailing-cr matches the original psqlodbc runsuite's diff
        # invocation: some checked-in expected files carry stray CRLF line
        # endings that are not significant to the test.
        if diff -q --strip-trailing-cr "$candidate" "$result_file" >/dev/null 2>&1; then
            matched="$candidate"
            break
        fi
    done

    if [ -n "$matched" ]; then
        echo "PASS: $test_name"
        PASS=$((PASS + 1))
    else
        echo "FAIL: $test_name (output differs)"
        diff -u "$expected_file" "$result_file" | head -20
        echo "      ..."
        FAIL=$((FAIL + 1))
    fi
done

echo ""
echo "=============================="
echo "Results: $PASS passed, $FAIL failed, $SKIP skipped"
echo ""

if [ $FAIL -gt 0 ]; then
    exit 1
fi
exit 0
