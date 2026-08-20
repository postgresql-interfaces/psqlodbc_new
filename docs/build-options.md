# Build Options Reference

psqlodbc2 is built with [Meson](https://mesonbuild.com/). Build options are passed to
`meson setup` with `-D<option>=<value>`, e.g.:

```bash
meson setup builddir -Ddriver_manager=unixodbc
```

They can be changed on an existing build directory with `meson configure`:

```bash
meson configure builddir -Dlibpq_prefix=/opt/pgsql
```

For the step-by-step build instructions per platform, see [building.md](building.md).

---

## Options

### `driver_manager`

| | |
|---|---|
| Type | combo |
| Choices | `auto`, `unixodbc`, `iodbc`, `none` |
| Default | `auto` |

Selects which ODBC Driver Manager the driver links against (for the DM-provided helper
symbols such as `SQLGetPrivateProfileString`).

- **`auto`** — probe in order: the pkg-config `odbc` module → `unixODBC` → `iODBC` →
  a header-only fallback that just requires `sql.h` to be on the include path.
- **`unixodbc`** — require unixODBC (tries the `odbc` pkg-config module first, then
  `unixODBC`).
- **`iodbc`** — require iODBC.
- **`none`** — link no driver manager; only requires that `sql.h` is findable. This is
  what the Windows build uses, because the Windows ODBC headers ship with the platform
  SDK rather than through pkg-config.

### `libpq_prefix`

| | |
|---|---|
| Type | string |
| Default | *(empty)* |

An explicit PostgreSQL client-library install prefix. The build expects
`<prefix>/include/libpq-fe.h` and `<prefix>/lib/libpq.{lib,so,dylib}`.

When set, libpq is located with `find_library` against that prefix instead of
pkg-config. This is required on Windows with MSVC + vcpkg: Meson's MSVC pkg-config
backend drops vcpkg's `-llibpq` link flag, so a pkg-config-resolved build *compiles but
fails to link* with unresolved `PQ*` symbols. Pointing `libpq_prefix` at the vcpkg
install prefix produces a correct, architecture-matched link.

Leave it empty on Linux/macOS, where pkg-config (or the fallback search below) resolves
libpq correctly.

---

## libpq resolution order

When configuring, the build locates libpq in this order:

1. **`-Dlibpq_prefix=<prefix>`** if set — via `find_library` against `<prefix>/lib` and
   `<prefix>/include` (tries the Windows `libpq` name, then the Unix `pq` name).
2. **pkg-config** (`dependency('libpq')`) — the normal path on Linux/macOS. On macOS,
   libpq is keg-only under Homebrew, so set
   `PKG_CONFIG_PATH=/opt/homebrew/opt/libpq/lib/pkgconfig` first.
3. **Platform fallback search paths** if pkg-config finds nothing:
   - Windows: `C:/Program Files/PostgreSQL/{17,16,15}`
   - Unix: `/usr/local/pgsql/{18,17}`, `/opt/homebrew/opt/libpq`

If none of these resolve libpq, configuration fails with
`libpq not found. Install PostgreSQL client library or set PKG_CONFIG_PATH.`

---

## Auto-detected: DSN support (`HAVE_ODBCINST`)

There is no user option for this — the build probes for `libodbcinst` (the library that
provides `SQLGetPrivateProfileString`, shipped with unixODBC). If found, the build
defines `-DHAVE_ODBCINST=1` and DSN lookup from `odbc.ini` is enabled.

If `libodbcinst` is **not** found, the driver still builds and works, but it cannot
resolve DSN names: a `DSN=<name>` in a connection string falls back to using `<name>` as
the database name. Explicit connection-string parameters always work either way.

Install `libodbcinst2` (Debian/Ubuntu) or `unixodbc` (Homebrew) to get DSN support.

---

## Build type

The standard `meson setup builddir` produces a debug build. The release build that CI
ships is:

```bash
meson setup builddir --buildtype=release --strip
```

`--strip` removes symbols from the installed shared library.
