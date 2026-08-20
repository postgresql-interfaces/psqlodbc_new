driver_sources = files(
    'driver_main.c',
    'diagnostics.c',
    'environment.c',
    'connection.c',
    'connection_string.c',
    'dsn_config.c',
    'statement.c',
    'descriptor.c',
    'parameter.c',
    'column_binding.c',
    'type_mapping.c',
    'large_object.c',
    'results.c',
    'catalog.c',
    'error_mapping.c',
    'query_parser.c',
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

# ---- Windows-only DSN setup DLL ----
#
# The ODBC Data Source Administrator loads this separate DLL to add/configure/
# remove a DSN (it exports ConfigDSN/ConfigDSNW/ConfigDriver). It is kept out of
# the driver DLL so the GUI/installer dependencies below are not pulled into the
# always-loaded driver. This entire block is guarded to Windows and therefore
# never evaluates on Linux/macOS — the setup source is inherently platform code.
if host_machine.system() == 'windows'
    windows_compiler = meson.get_compiler('c')

    # ODBC installer + Win32 GUI libraries required by the setup routines.
    # odbccp32 provides SQLWriteDSNToIni / SQLWritePrivateProfileString /
    # SQLRemoveDSNFromIni / SQLGetPrivateProfileString; user32/comctl32 back
    # the dialog (DialogBoxParamW and its controls).
    setup_windows_deps = [
        windows_compiler.find_library('odbccp32'),
        windows_compiler.find_library('odbc32'),
        windows_compiler.find_library('comctl32'),
        windows_compiler.find_library('user32'),
        windows_compiler.find_library('comdlg32'),
    ]

    # Compile the dialog resource script (.rc) into a linkable object. The
    # include_directories lets the .rc find setup/resource.h. This uses
    # Meson's windows module, which drives rc.exe (MSVC) or windres (MinGW).
    windows_module = import('windows')
    setup_dialog_resources = windows_module.compile_resources(
        'setup/setup_dialog.rc',
        include_directories : include_directories('setup'),
    )

    psqlodbc2_setup_lib = shared_library('psqlodbc2setup',
        ['setup/setup_windows.c', setup_dialog_resources],
        include_directories : [inc, include_directories('.', 'setup')],
        dependencies : setup_windows_deps,
        vs_module_defs : 'setup/psqlodbc2setup.def',
        install : true,
    )
endif
