# Building from Source

psqlodbc2 uses [Meson](https://mesonbuild.com/) with the Ninja backend. The standard
cycle on every platform is:

```bash
meson setup builddir          # configure (first time only)
meson compile -C builddir     # build
meson test -C builddir        # run tests
```

The build produces the driver shared library under `builddir/src/`:

| Platform | File |
|---|---|
| Linux | `libpsqlodbc2w.so` |
| macOS | `libpsqlodbc2w.dylib` |
| Windows | `psqlodbc2w.dll` |

Build options (the driver-manager choice and the libpq prefix) are documented in
[build-options.md](build-options.md).

---

## Linux

Install the toolchain and dependencies (Debian/Ubuntu):

```bash
sudo apt-get update
sudo apt-get install -y \
  meson ninja-build \
  libpq-dev unixodbc-dev libodbcinst2
```

- `libpq-dev` — PostgreSQL client library and headers.
- `unixodbc-dev` — ODBC driver-manager headers/library.
- `libodbcinst2` — enables DSN lookup from `odbc.ini` (see
  [build-options.md](build-options.md#auto-detected-dsn-support-have_odbcinst)).

Then build:

```bash
meson setup builddir
meson compile -C builddir
meson test -C builddir
```

## macOS

Install the toolchain and dependencies with Homebrew:

```bash
brew install meson ninja libpq unixodbc
```

libpq is keg-only on Homebrew (not linked into the default prefix), so point pkg-config
at it when configuring:

```bash
PKG_CONFIG_PATH=/opt/homebrew/opt/libpq/lib/pkgconfig meson setup builddir
meson compile -C builddir
meson test -C builddir
```

> On Intel Macs the Homebrew prefix is `/usr/local` instead of `/opt/homebrew`; adjust
> the `PKG_CONFIG_PATH` accordingly.

## Windows

Requirements:

- **MSVC** (the Visual Studio C toolchain). Run the build from a "Developer Command
  Prompt" / after loading the MSVC environment.
- **Meson + Ninja + pkgconf**:

  ```powershell
  pip install meson ninja pkgconf
  ```

- **libpq** — obtained one of two ways (below).

The Windows build links no ODBC driver manager (`-Ddriver_manager=none`) because the
ODBC headers come from the platform SDK rather than pkg-config.

### Option A — PostgreSQL install (Chocolatey / EDB)

Install PostgreSQL (which bundles libpq), then use a Meson native file to point pkgconf
at its `pkgconfig` directory:

```powershell
choco install postgresql17 --params "/Password:postgres /Port:5432" -y
```

Create `native.ini`:

```ini
[binaries]
pkg-config = 'pkgconf-pypi'

[built-in options]
pkg_config_path = 'C:/Program Files/PostgreSQL/17/lib/pkgconfig'
```

Then configure and build:

```powershell
meson setup builddir -Ddriver_manager=none --native-file=native.ini
meson compile -C builddir
```

> The Chocolatey PostgreSQL package ships a `libpq.pc` with the build machine's prefix
> baked in; if configuration can't find libpq, rewrite `libpq.pc`'s `prefix` to the real
> install path (`C:/Program Files/PostgreSQL/17`). Also make sure Strawberry Perl is not
> ahead of pkgconf on `PATH`, as it injects a broken `pkg-config`.

### Option B — vcpkg (used by the release build)

Install libpq for your target triplet, then point Meson at the vcpkg install prefix with
`-Dlibpq_prefix` (this avoids a pkg-config linking problem on MSVC — see
[build-options.md](build-options.md#libpq_prefix)):

```powershell
vcpkg install libpq:x64-windows
meson setup builddir --buildtype=release -Ddriver_manager=none `
  "-Dlibpq_prefix=$env:VCPKG_INSTALLATION_ROOT/installed/x64-windows"
meson compile -C builddir
```

### ARM64

To cross-compile for ARM64, set up the `amd64_arm64` MSVC environment, use the
`arm64-windows` vcpkg triplet, and pass the bundled cross file:

```powershell
meson setup builddir --buildtype=release -Ddriver_manager=none `
  "-Dlibpq_prefix=$env:VCPKG_INSTALLATION_ROOT/installed/arm64-windows" `
  --cross-file build-aux/windows-arm64.cross
```

---

## Running the tests

`meson test -C builddir` runs the unit tests unconditionally. Several test suites also
have a live-database mode that is **skipped** (reported as `SKIP`, not a failure) unless
you provide a connection string via the `PSQLODBC2_TEST_CONNSTR` environment variable:

```bash
PSQLODBC2_TEST_CONNSTR='Server=localhost;Port=5432;Database=contrib_regression;UID=postgres;PWD=postgres' \
  meson test -C builddir --print-errorlogs
```

The connection string uses the driver's own connection-string syntax — see
[connection-options.md](connection-options.md).

---

## Release build

The build the project ships (stripped, optimized):

```bash
meson setup builddir --buildtype=release --strip
meson compile -C builddir
```
