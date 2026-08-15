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
# Strategy: an explicit libpq_prefix wins (used on Windows/vcpkg, see below);
# otherwise try pkg-config, then probe common install locations per platform.
libpq_prefix = get_option('libpq_prefix')
if libpq_prefix != ''
    # Explicit prefix: link via find_library rather than pkg-config. On MSVC,
    # Meson's pkg-config backend drops vcpkg's "-llibpq" link flag, so a
    # pkg-config-resolved libpq compiles but fails to link. find_library against
    # the prefix's lib/ produces a correct, arch-matched link.
    cc = meson.get_compiler('c')
    # The import/shared library is named "libpq.lib" on Windows (vcpkg/MSVC) but
    # "libpq.so"/"libpq.dylib" on Unix, whose canonical find_library name is
    # "pq". Try the Windows name first, then the Unix name, so a single option
    # works across platforms.
    libpq_lib = cc.find_library('libpq',
        dirs : [libpq_prefix / 'lib'],
        required : false,
    )
    if not libpq_lib.found()
        libpq_lib = cc.find_library('pq',
            dirs : [libpq_prefix / 'lib'],
            required : true,
        )
    endif
    libpq_dep = declare_dependency(
        dependencies : libpq_lib,
        include_directories : include_directories(libpq_prefix / 'include'),
    )
else
    libpq_dep = dependency('libpq', required : false)
endif
if not libpq_dep.found()
    cc = meson.get_compiler('c')

    # Common library search paths by platform
    libpq_search_dirs = []
    libpq_include_args = []

    if host_machine.system() == 'windows'
        # Standard EDB/Chocolatey install paths on Windows
        libpq_search_dirs = [
            'C:/Program Files/PostgreSQL/17/lib',
            'C:/Program Files/PostgreSQL/16/lib',
            'C:/Program Files/PostgreSQL/15/lib',
        ]
        libpq_include_candidates = [
            'C:/Program Files/PostgreSQL/17/include',
            'C:/Program Files/PostgreSQL/16/include',
            'C:/Program Files/PostgreSQL/15/include',
        ]
    else
        # macOS source builds and Homebrew, Linux standard paths
        libpq_search_dirs = [
            '/usr/local/pgsql/18/lib',
            '/usr/local/pgsql/17/lib',
            '/opt/homebrew/opt/libpq/lib',
        ]
        libpq_include_candidates = [
            '/usr/local/pgsql/18/include',
            '/usr/local/pgsql/17/include',
            '/opt/homebrew/opt/libpq/include',
        ]
    endif

    libpq_lib = cc.find_library('pq', dirs : libpq_search_dirs, required : false)

    if libpq_lib.found()
        # Find the matching include directory
        libpq_inc_dir = ''
        foreach inc_path : libpq_include_candidates
            if cc.has_header('libpq-fe.h', args : ['-I' + inc_path])
                libpq_inc_dir = inc_path
                break
            endif
        endforeach

        if libpq_inc_dir != ''
            libpq_dep = declare_dependency(
                dependencies : libpq_lib,
                include_directories : include_directories(libpq_inc_dir),
            )
        elif cc.has_header('libpq-fe.h')
            # Header is in default include path already
            libpq_dep = declare_dependency(dependencies : libpq_lib)
        else
            error('Found libpq library but not libpq-fe.h header. Install PostgreSQL development headers.')
        endif
    else
        error('libpq not found. Install PostgreSQL client library or set PKG_CONFIG_PATH.')
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
