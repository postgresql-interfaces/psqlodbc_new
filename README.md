# psqlodbc2 — PostgreSQL ODBC Driver

A ground-up, modern C11 rewrite of the PostgreSQL ODBC driver, designed as a drop-in
replacement for the original [psqlodbc](https://github.com/postgresql-interfaces/psqlodbc).
It builds with [Meson](https://mesonbuild.com/) and runs on Linux, macOS, and Windows.

- **Version:** 0.1.0
- **Language:** C11
- **Driver library:** `libpsqlodbc2w.so` / `libpsqlodbc2w.dylib` / `psqlodbc2w.dll`
- **ODBC driver name:** `PostgreSQL ODBC Driver (psqlodbc2)`
- **Compatibility:** passes the upstream psqlodbc regression suite (parity target).

## Quickstart (build from source)

```bash
meson setup builddir          # configure
meson compile -C builddir     # build
meson test -C builddir         # run tests
```

The driver is produced under `builddir/src/`. On macOS, prefix `meson setup` with
`PKG_CONFIG_PATH=/opt/homebrew/opt/libpq/lib/pkgconfig`. See the build guide for
platform details.

Once installed and registered, connect with a DSN-less connection string:

```
Driver={PostgreSQL ODBC Driver (psqlodbc2)};Server=localhost;Port=5432;Database=mydb;UID=postgres;PWD=secret
```

## Documentation

| Topic | Document |
|---|---|
| Install & register on Linux / macOS | [docs/installation-unix.md](docs/installation-unix.md) |
| Install & register on Windows (MSI) | [docs/installation-windows.md](docs/installation-windows.md) |
| Build from source (all platforms) | [docs/building.md](docs/building.md) |
| Meson build options | [docs/build-options.md](docs/build-options.md) |
| Connection-string keywords, DSN keys, connect attributes | [docs/connection-options.md](docs/connection-options.md) |
| Cutting a release (maintainers) | [docs/releasing.md](docs/releasing.md) |

## License

PostgreSQL License. See [LICENSE](LICENSE).
