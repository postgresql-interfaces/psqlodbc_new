project('psqlodbc2', 'c',
    version : '0.1.0',
    meson_version : '>= 1.1',
    default_options : ['c_std=c11', 'warning_level=2'],
    license : 'PostgreSQL',
)

# Locate ODBC headers and libraries.
# The 'driver_manager' option lets the user force a specific driver manager;
# 'auto' tries odbc -> unixODBC -> iODBC -> header-only fallback.
dm_choice = get_option('driver_manager')

odbc_dep = dependency('', required : false)  # start with nothing found

if dm_choice == 'none'
    cc = meson.get_compiler('c')
    assert(cc.has_header('sql.h'), 'ODBC headers not found. Install unixODBC or iODBC development package.')
    odbc_dep = declare_dependency()
elif dm_choice == 'unixodbc'
    odbc_dep = dependency('odbc', required : false)
    if not odbc_dep.found()
        odbc_dep = dependency('unixODBC', required : true)
    endif
elif dm_choice == 'iodbc'
    odbc_dep = dependency('iODBC', required : true)
else  # auto
    odbc_dep = dependency('odbc', required : false)
    if not odbc_dep.found()
        odbc_dep = dependency('unixODBC', required : false)
    endif
    if not odbc_dep.found()
        odbc_dep = dependency('iODBC', required : false)
    endif
    if not odbc_dep.found()
        cc = meson.get_compiler('c')
        assert(cc.has_header('sql.h'), 'ODBC headers not found. Install unixODBC or iODBC development package.')
        odbc_dep = declare_dependency()
    endif
endif

# Locate PostgreSQL client library (libpq) for connecting to the database.
# If pkg-config cannot find libpq automatically, check common install locations.
libpq_dep = dependency('libpq', required : false)
if not libpq_dep.found()
    cc = meson.get_compiler('c')
    # Check /usr/local/pgsql/18 (common for source builds on macOS)
    libpq_lib = cc.find_library('pq',
        dirs : ['/usr/local/pgsql/18/lib'],
        required : false,
    )
    if libpq_lib.found() and cc.has_header('libpq-fe.h',
        args : ['-I/usr/local/pgsql/18/include'])
        libpq_dep = declare_dependency(
            dependencies : libpq_lib,
            include_directories : include_directories('/usr/local/pgsql/18/include'),
        )
    else
        error('libpq not found. Set PKG_CONFIG_PATH to include libpq.pc location.')
    endif
endif

# libodbcinst provides SQLGetPrivateProfileString for reading DSN parameters
# from odbc.ini. It's part of unixODBC and typically installed alongside libodbc.
# If not found, DSN lookup is disabled but the driver still works with connection strings.
odbcinst_dep = dependency('odbcinst', required : false)
if not odbcinst_dep.found()
    cc = meson.get_compiler('c')
    odbcinst_dep = cc.find_library('odbcinst', required : false)
endif

if odbcinst_dep.found()
    add_project_arguments('-DHAVE_ODBCINST=1', language : 'c')
endif

inc = include_directories('include')

subdir('src')
subdir('tests')
