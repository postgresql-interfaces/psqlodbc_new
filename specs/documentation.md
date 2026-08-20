# Plan: User & Build Documentation

## Task Description
The project has a complete, tested driver but no end-user documentation. Produce a
coherent set of Markdown documents under `docs/` (plus a top-level `README.md`) that
tell a new user how to install the driver on Unix-like systems and Windows, how to
build it from source on each platform, and what every tunable — build option, ODBC
connection-string keyword, DSN key, and driver-specific connect attribute — does.

All content must be derived from the actual code and CI, not invented. This spec
records the ground-truth facts (with source locations) so the docs stay accurate.

## Objective
When this plan is complete, a developer who has never seen the repo can:
1. Install a released binary and register the driver with their ODBC subsystem.
2. Build from source on Linux, macOS, or Windows following copy-pasteable steps.
3. Look up any connection-string keyword / DSN key / build option and understand its
   effect and default.

## Ground-Truth Facts (verify against source before writing; cite in docs)

### Identity
- Shared library base name: `psqlodbc2w` → `libpsqlodbc2w.so` (Linux),
  `libpsqlodbc2w.dylib` (macOS), `psqlodbc2w.dll` (Windows). Source: `src/meson.build:30`.
- Windows ODBC driver display name (the `Driver={...}` value): **`PostgreSQL ODBC Driver (psqlodbc2)`**. Source: `installer/psqlodbc2.wxs:39`.
- Version: `0.1.0`. Source: `meson.build:2`. Language: C11. Build system: Meson (`>= 1.1`).
- Symbol visibility is `hidden`; only `PSQLODBC2_EXPORT` symbols are exported;
  `psqlodbc2.def` drives Windows exports. Source: `src/meson.build:34-35`.

### Build options (the "feature flags") — `meson.options`
- `driver_manager` (combo: `auto` | `unixodbc` | `iodbc` | `none`, default `auto`).
  `auto` probes odbc → unixODBC → iODBC → header-only fallback. `none` links no DM and
  only requires `sql.h` on the include path (used for the Windows build). Source:
  `meson.options`, resolved in `meson.build:11-39`.
- `libpq_prefix` (string, default empty). When set, libpq is located via
  `find_library` against `<prefix>/lib` + `<prefix>/include` instead of pkg-config.
  Required on Windows/MSVC+vcpkg, where Meson's pkg-config backend drops vcpkg's
  `-llibpq` flag so a pkg-config build links with unresolved `PQ*` symbols. Source:
  `meson.options`, `meson.build:44-71`.
- Implicit, auto-detected (not a user option, but document it): if `odbcinst` is found,
  the build defines `-DHAVE_ODBCINST=1`, which enables reading DSNs from `odbc.ini`.
  Without it the driver still works, but only via explicit connection-string params —
  DSN lookup silently returns "not found". Source: `meson.build:136-144`, `src/dsn_config.c`.
- libpq fallback search paths when neither `libpq_prefix` nor pkg-config resolve it:
  Windows `C:/Program Files/PostgreSQL/{17,16,15}`; Unix `/usr/local/pgsql/{18,17}`,
  `/opt/homebrew/opt/libpq`. Source: `meson.build:79-103`.

### Build / test commands
- Configure/build/test: `meson setup builddir` → `meson compile -C builddir` →
  `meson test -C builddir`. Source: `CLAUDE.md`.
- Release build (what CI ships): `meson setup builddir --buildtype=release --strip`.
  Source: `.github/workflows/release.yml:57`.
- Live-database tests are gated on the `PSQLODBC2_TEST_CONNSTR` env var; when unset,
  those test binaries **skip** (Meson exit code 77) rather than fail. Unit tests always
  run. Source: `.github/workflows/ci.yml:74-80`, observed `meson test` output.

### Platform dependencies (from CI, the authoritative source)
- Ubuntu: `meson ninja-build libpq-dev unixodbc-dev libodbcinst2` (regression suite
  also needs `autoconf automake libtool`). Source: `.github/workflows/ci.yml:44-52`.
- macOS (Homebrew): `brew install meson ninja libpq unixodbc`; libpq is keg-only, so
  set `PKG_CONFIG_PATH=/opt/homebrew/opt/libpq/lib/pkgconfig` before `meson setup`.
  Source: `.github/workflows/ci.yml:53-67`.
- Windows: `pip install meson ninja pkgconf`; MSVC toolchain; libpq from either the
  EDB/Chocolatey PostgreSQL install or vcpkg (`vcpkg install libpq:<triplet>`). Configure
  with `-Ddriver_manager=none`; supply libpq via a native file (`pkg_config_path`) or
  `-Dlibpq_prefix=<vcpkg install prefix>`. Source: `.github/workflows/ci.yml:129-206`,
  `.github/workflows/release.yml:83-165`.

### ODBC connection-string keywords — `src/connection_string.c:36-183`
Document each with: aliases, meaning, default, and whether it maps to libpq or is a
driver behavior toggle. Mark the three compatibility keywords that are **accepted and
ignored** so users aren't surprised.
- `Server` / `Servername` → libpq `host`.
- `Port` → libpq `port`.
- `Database` / `DB` → libpq `dbname`.
- `UID` / `Username` / `User` → libpq `user`.
- `PWD` / `Password` → libpq `password` (heap-stored, wiped on free).
- `SSLmode` → libpq `sslmode`.
- `ApplicationName` / `Application_Name` → libpq `application_name`.
- `Timeout` / `Connect_Timeout` → libpq `connect_timeout` (seconds).
- `BoolsAsChar` (default ON) — describe PG `bool` as `VARCHAR(5)` vs `SQL_BIT`.
  Rationale: MS Access. Source also `src/connection.h:72-77`.
- `UnknownSizes` — `0`=MAX, `1`=DONTKNOW, `2`=LONGEST. Constants `UNKNOWN_SIZES_*`
  in `src/connection.h:159-166`.
- `MaxVarcharSize` — reported size cap for unbounded varchar/char (default 255,
  `DEFAULT_MAX_VARCHAR_SIZE`, `src/connection.h:170`).
- `Parse` — client-side SELECT parsing for refined column metadata (MS Access).
- `FetchRefcursors` — auto-`FETCH ALL` refcursor OUT params as result sets; requires
  autocommit OFF. Source: `src/connection.h:96-102`.
- `Protocol` — form `7.4-N`; only the trailing `N` matters, sets rollback-on-error mode
  (`ROLLBACK_ON_ERROR_*`, `src/connection.h:140-151`): `0`=NOTHING, `1`=TRANSACTION,
  `2`=STATEMENT, default (unset) resolves per server version.
- `LFConversion` — expand `\n` → `\r\n` on fetched char/wchar output.
- `CX` — packed hex bitfield; bit `0x01` = LFConversion (`CONNECTION_LF_CONVERSION_BIT`).
- `AB` — extra-options hex bitfield; bit `0x08` = CvtNullDate (empty string → SQL NULL
  for date/time params; `CONNECTION_CVT_NULL_DATE_BIT`).
- **Accepted & ignored** (compatibility only): `Fetch`, `UseDeclareFetch`,
  `DisallowPremature` — this driver materializes whole result sets client-side, so these
  server-side-cursor tunables have no effect. `Driver` is consumed by the DM. Unknown
  keys are silently ignored.

### DSN keys in `odbc.ini` — `src/dsn_config.h:24-34`, read in `src/dsn_config.c`
`Servername` (or `Server`), `Port`, `Database`, `Username` (or `UID`), `Password`,
`SSLmode`, `ApplicationName`, `Timeout`, `Description`. DSN values are overlaid by any
connection-string values parsed after `DSN=`. Working examples live in
`regress/work/odbc.ini` and `regress/work/odbcinst.ini` — reuse these in the docs.

### Driver-specific connect attributes — `src/connection.h:262-275`
`SQL_ATTR_PGOPT_MSJET` (65549, MS Access/Jet boolean rewrite),
`SQL_ATTR_PGOPT_FETCH` (65541, accepted/no-op), `SQL_ATTR_PGOPT_BATCHSIZE` (65550,
array-execution batch size, default 100 = `DEFAULT_BATCH_SIZE`).

## Deliverables

### `README.md` (repo root)
One-page front door: what the driver is (drop-in modern rewrite of psqlodbc), status
(v0.1.0, upstream regression parity), supported platforms, a 5-line quickstart, and a
table of links into `docs/`. Keep install/build details in the dedicated docs; the
README only points at them.

### `docs/installation-unix.md`
- Installing a release tarball (`libpsqlodbc2w.so` / `.dylib`): where to put it.
- Registering the driver with unixODBC: an `odbcinst.ini` stanza (model on
  `regress/work/odbcinst.ini`) and `odbcinst -i -d -f`, plus a manual-edit alternative.
- Creating a DSN in `odbc.ini` (model on `regress/work/odbc.ini`) with the full DSN key
  table; and the DSN-less `Driver=...;Server=...;...` connection-string form.
- Verifying with `isql` / `odbcinst -q`.
- Note the `libodbcinst` dependency for DSN lookup and what degrades without it.
- macOS specifics: Homebrew unixODBC paths, keg-only libpq, Apple Silicon prefixes.

### `docs/installation-windows.md`
- Installing via the MSI (`psqlodbc2-<ver>-windows-{x64,arm64}.msi`): the MSI copies the
  DLL + bundled libpq runtime DLLs into `Program Files\psqlODBC2\<arch>\bin` and registers
  the driver automatically. Source: `installer/psqlodbc2.wxs`.
- Where the driver then appears (ODBC Data Source Administrator) and the exact
  `Driver={PostgreSQL ODBC Driver (psqlodbc2)}` string for DSN-less use.
- Creating User/System DSNs via the Administrator; note DSN storage is the registry.
- Uninstall / in-place upgrade behavior (MajorUpgrade; per-arch UpgradeCodes).

### `docs/building.md`
Prerequisites and copy-pasteable build steps for each platform, then the standard
`meson setup / compile / test` cycle and how to run the live-DB tests via
`PSQLODBC2_TEST_CONNSTR`. Include the release-build invocation. One section each:
- **Linux**: apt dependency line; plain `meson setup builddir`.
- **macOS**: brew dependency line; `PKG_CONFIG_PATH` for keg-only libpq.
- **Windows**: MSVC + `pip install meson ninja pkgconf`; the two libpq routes
  (Chocolatey PostgreSQL + native-file `pkg_config_path`, or vcpkg + `-Dlibpq_prefix`);
  `-Ddriver_manager=none`; and the arm64 cross-file (`build-aux/windows-arm64.cross`).
Link to `docs/build-options.md` rather than duplicating the option reference.

### `docs/build-options.md`
Reference table for the Meson options (`driver_manager`, `libpq_prefix`) with choices,
defaults, and *why you'd change each* — plus the auto-detected `HAVE_ODBCINST` behavior
and the libpq resolution order (libpq_prefix → pkg-config → platform fallback paths).

### `docs/connection-options.md`
The user-facing reference for runtime tunables: one table for connection-string
keywords (keyword, aliases, values/default, effect, "ignored?" flag) and one for
`odbc.ini` DSN keys, followed by a short section on the driver-specific connect
attributes. This is the single source users consult for "what can I put in my
connection string". Populate strictly from the Ground-Truth Facts above.

## Relevant Files (read these; do not guess)
- `meson.build`, `meson.options`, `src/meson.build` — build config & options.
- `.github/workflows/ci.yml`, `.github/workflows/release.yml` — authoritative
  per-platform dependency and build/package steps.
- `src/connection_string.c`, `src/connection.h` — connection-string keywords & semantics.
- `src/dsn_config.c`, `src/dsn_config.h` — DSN keys.
- `installer/psqlodbc2.wxs`, `installer/license.rtf` — Windows install & driver name.
- `regress/work/odbc.ini`, `regress/work/odbcinst.ini` — working registration examples.
- `CLAUDE.md` — canonical build commands and conventions.

## Step by Step Tasks
1. Re-read every file in *Relevant Files* and confirm each fact/line reference in
   *Ground-Truth Facts* still holds; correct any drift before writing prose.
2. Write `docs/connection-options.md` and `docs/build-options.md` first (pure reference,
   fully determined by the facts).
3. Write `docs/building.md`, then `docs/installation-unix.md` and
   `docs/installation-windows.md`, cross-linking to the two reference docs.
4. Write the root `README.md` last, linking into `docs/`.
5. Validate: shell out to check every documented command actually runs on Linux
   (`meson setup/compile/test`), and confirm each documented keyword/option string
   appears verbatim in the cited source file (`grep`). Fix mismatches.

## Acceptance Criteria
- `docs/` contains `installation-unix.md`, `installation-windows.md`, `building.md`,
  `build-options.md`, `connection-options.md`; repo root has `README.md`.
- Every Meson option, connection-string keyword, DSN key, and connect attribute listed
  in *Ground-Truth Facts* is documented with its correct default/values; the three
  accepted-but-ignored keywords are explicitly marked as such.
- Every command block for Linux is verified to run; Windows/macOS steps match CI exactly.
- No invented flags, paths, or driver names — the Windows `Driver=` string and library
  file names match the source exactly.
- Markdown only; no changes to code or build files.

## Notes
- These docs are descriptive, not a code change — the `/build` reviewer/validator should
  check *accuracy against source*, not C11 style.
- The upstream reference driver's docs live at `~/projects/psqlodbc`; useful for tone and
  for wording of psqlodbc-compatible keywords, but this driver's behavior (esp. the
  accepted-but-ignored options) differs — always defer to this repo's source.
</content>
</invoke>
