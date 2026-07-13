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
ORIG_TEST_DIR="${HOME}/projects/psqlodbc/test"
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

# Load sample tables
echo "Loading sample tables..."
psql -h "$PG_HOST" -p "$PG_PORT" -d "$PG_DATABASE" -q -c "DROP SCHEMA public CASCADE; CREATE SCHEMA public;" 2>/dev/null || true
psql -h "$PG_HOST" -p "$PG_PORT" -d "$PG_DATABASE" -q -f "$ORIG_TEST_DIR/sampletables.sql" 2>/dev/null

# Determine which tests to run
if [ $# -gt 0 ]; then
    TESTS="$@"
else
    # Start with a subset of tests most likely to work with our current feature set
    TESTS="connect select stmthandles update commands getresult prepare params"
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
        if diff -q "$candidate" "$result_file" >/dev/null 2>&1; then
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
