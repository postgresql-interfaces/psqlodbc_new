driver_sources = files(
    'driver_main.c',
    'diagnostics.c',
    'environment.c',
    'connection.c',
    'connection_string.c',
    'dsn_config.c',
    'statement.c',
    'parameter.c',
    'column_binding.c',
    'type_mapping.c',
    'results.c',
    'catalog.c',
    'error_mapping.c',
    'odbc_api.c',
)

# Collect driver dependencies — odbcinst is optional (enables DSN lookup from odbc.ini)
driver_deps = [odbc_dep, libpq_dep]
if odbcinst_dep.found()
    driver_deps += odbcinst_dep
endif

# The shared library is named 'psqlodbc2w' (w = Unicode variant).
# Symbol visibility is set to 'hidden' by default — only functions explicitly
# marked with PSQLODBC2_EXPORT are visible to callers.
psqlodbc2_lib = shared_library('psqlodbc2w',
    driver_sources,
    include_directories : [inc, include_directories('.')],
    dependencies : driver_deps,
    vs_module_defs : '../psqlodbc2.def',
    gnu_symbol_visibility : 'hidden',
    install : true,
)
