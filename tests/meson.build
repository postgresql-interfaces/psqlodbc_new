# Determine the path to the built shared library so the test can dlopen it.
# We use the full_path() method to get the absolute path at build time.
driver_library_path = psqlodbc2_lib.full_path()

test_driver_load = executable('test_driver_load',
    'test_driver_load.c',
    include_directories : inc,
    dependencies : odbc_dep,
    c_args : ['-DDRIVER_LIBRARY_PATH="' + driver_library_path + '"'],
)

test('driver_load', test_driver_load,
    depends : psqlodbc2_lib,
)

# Connection string parsing tests (pure unit tests, no database needed).
# Compiles the connection string source directly because these are internal
# functions with hidden visibility (not exported from the shared library).
# dsn_config.c is included because connection_string.c calls dsn_config_read.
test_connection_string_deps = [odbc_dep, libpq_dep]
if odbcinst_dep.found()
    test_connection_string_deps += odbcinst_dep
endif

test_connection_string = executable('test_connection_string',
    'test_connection_string.c',
    '../src/connection_string.c',
    '../src/dsn_config.c',
    '../src/diagnostics.c',
    include_directories : [inc, include_directories('../src')],
    dependencies : test_connection_string_deps,
)

test('connection_string', test_connection_string,
    depends : psqlodbc2_lib,
)

# Connection handle lifecycle tests (dlopen-based, no database needed).
test_connection_lifecycle = executable('test_connection_lifecycle',
    'test_connection_lifecycle.c',
    include_directories : inc,
    dependencies : odbc_dep,
    c_args : ['-DDRIVER_LIBRARY_PATH="' + driver_library_path + '"'],
)

test('connection_lifecycle', test_connection_lifecycle,
    depends : psqlodbc2_lib,
)

# Statement handle lifecycle tests (dlopen-based, no database needed).
test_statement_lifecycle = executable('test_statement_lifecycle',
    'test_statement_lifecycle.c',
    include_directories : inc,
    dependencies : odbc_dep,
    c_args : ['-DDRIVER_LIBRARY_PATH="' + driver_library_path + '"'],
)

test('statement_lifecycle', test_statement_lifecycle,
    depends : psqlodbc2_lib,
)

# Statement execution tests (requires live PostgreSQL; skips if no connection string).
test_statement_execution = executable('test_statement_execution',
    'test_statement_execution.c',
    include_directories : inc,
    dependencies : odbc_dep,
    c_args : ['-DDRIVER_LIBRARY_PATH="' + driver_library_path + '"'],
)

test('statement_execution', test_statement_execution,
    depends : psqlodbc2_lib,
)

# Results module tests (requires live PostgreSQL; skips if no connection string).
test_results = executable('test_results',
    'test_results.c',
    include_directories : inc,
    dependencies : odbc_dep,
    c_args : ['-DDRIVER_LIBRARY_PATH="' + driver_library_path + '"'],
)

test('results', test_results,
    depends : psqlodbc2_lib,
)

# Catalog function tests (requires live PostgreSQL; skips if no connection string).
test_catalog = executable('test_catalog',
    'test_catalog.c',
    include_directories : inc,
    dependencies : odbc_dep,
    c_args : ['-DDRIVER_LIBRARY_PATH="' + driver_library_path + '"'],
)

test('catalog', test_catalog,
    depends : psqlodbc2_lib,
)

# Parameter binding tests (pure unit tests, no database needed).
# Compiles the parameter source directly because these are internal
# functions with hidden visibility (not exported from the shared library).
test_parameter_binding = executable('test_parameter_binding',
    'test_parameter_binding.c',
    '../src/parameter.c',
    include_directories : [inc, include_directories('../src')],
    dependencies : [odbc_dep, libpq_dep],
)

test('parameter_binding', test_parameter_binding,
    depends : psqlodbc2_lib,
)

# Error mapping tests (requires live PostgreSQL; skips if no connection string).
# Verifies that PostgreSQL SQLSTATE codes are correctly extracted and propagated
# through the ODBC diagnostic system.
test_error_mapping = executable('test_error_mapping',
    'test_error_mapping.c',
    include_directories : inc,
    dependencies : odbc_dep,
    c_args : ['-DDRIVER_LIBRARY_PATH="' + driver_library_path + '"'],
)

test('error_mapping', test_error_mapping,
    depends : psqlodbc2_lib,
)

# Connection attributes tests (set/get attributes and transaction management).
# No-DB tests always run; DB-required tests run if PSQLODBC2_TEST_CONNSTR is set.
test_connection_attributes = executable('test_connection_attributes',
    'test_connection_attributes.c',
    include_directories : inc,
    dependencies : odbc_dep,
    c_args : ['-DDRIVER_LIBRARY_PATH="' + driver_library_path + '"'],
)

test('connection_attributes', test_connection_attributes,
    depends : psqlodbc2_lib,
)

# Column binding tests (requires live PostgreSQL; skips if no connection string).
# Verifies SQLBindCol, fetch-into-bindings, unbind, and mixed bound/unbound usage.
test_column_binding = executable('test_column_binding',
    'test_column_binding.c',
    include_directories : inc,
    dependencies : odbc_dep,
    c_args : ['-DDRIVER_LIBRARY_PATH="' + driver_library_path + '"'],
)

test('column_binding', test_column_binding,
    depends : psqlodbc2_lib,
)

# Statement attributes tests (no database needed — only handle allocation).
# Verifies SQLSetStmtAttr / SQLGetStmtAttr roundtrip and downgrade behavior.
test_statement_attributes = executable('test_statement_attributes',
    'test_statement_attributes.c',
    include_directories : inc,
    dependencies : odbc_dep,
    c_args : ['-DDRIVER_LIBRARY_PATH="' + driver_library_path + '"'],
)

test('statement_attributes', test_statement_attributes,
    depends : psqlodbc2_lib,
)

# DSN config tests (reads from a temp odbc.ini file, no database needed).
# Requires libodbcinst to be available; if not, the test exits with code 77 (skip).
test_dsn_config_deps = [odbc_dep, libpq_dep]
if odbcinst_dep.found()
    test_dsn_config_deps += odbcinst_dep
endif

test_dsn_config = executable('test_dsn_config',
    'test_dsn_config.c',
    '../src/dsn_config.c',
    include_directories : [inc, include_directories('../src')],
    dependencies : test_dsn_config_deps,
)

test('dsn_config', test_dsn_config,
    depends : psqlodbc2_lib,
)
