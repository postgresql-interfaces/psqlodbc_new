# Plan: Windows DSN Setup Dialog

## Task Description
Give psqlodbc2 a Windows DSN configuration GUI, matching the convenience of the original
psqlodbc. Selecting `PostgreSQL ODBC Driver (psqlodbc2)` in the ODBC Data Source
Administrator and clicking **Add…** or **Configure** currently fails ("the setup routines
… could not be found") because the driver exports no `ConfigDSN` routine and no setup
component is registered. This plan adds a Windows-only **setup DLL** that implements
`ConfigDSN`, presents a Win32 dialog for the DSN keys the driver honors, and is
registered with the driver via the WiX installer.

## Objective
When complete:
1. A new Windows-only shared library `psqlodbc2setup.dll` is built by Meson and exports
   `ConfigDSN` / `ConfigDSNW` / `ConfigDriver`.
2. **Add…** opens a dialog to enter DSN parameters; on OK the DSN is written to the
   registry. **Configure** loads the existing DSN's values and saves edits. **Remove**
   deletes the DSN.
3. The WiX installer packages the setup DLL and registers it as the driver's `Setup`
   component so the Administrator invokes it.
4. **The Linux and macOS builds are completely unaffected** — the setup target is guarded
   to Windows, and `meson setup builddir && meson compile -C builddir && meson test -C
   builddir` on this host stays green.

## Design Decisions (resolved — do not re-litigate)
- **Separate setup DLL** (`psqlodbc2setup.dll`), *not* exports added to the driver DLL.
  Rationale: keeps GUI/`comctl32`/`odbccp32` dependencies out of `psqlodbc2w.dll`, which
  is loaded by every connection on every platform. This mirrors upstream psqlodbc's split
  between the driver and its setup routines.
- **Plain Win32 dialog** built from a resource script (`.rc`) driven by
  `DialogBoxParamW`, no MFC/ATL. Compiled with Meson's `windows` module
  (`compile_resources`).
- **DSN read/write** goes through `SQLGetPrivateProfileString` /
  `SQLWritePrivateProfileString` / `SQLWriteDSNToIni` / `SQLRemoveDSNFromIni` from
  `odbccp32` — the standard ODBC installer API, so it writes to the correct registry
  location for User vs. System DSNs automatically.
- **Dialog fields = the DSN keys the driver actually honors** (see
  `src/dsn_config.c` / `docs/connection-options.md`): `Description`, `Servername`,
  `Port`, `Database`, `Username`, `Password`, `SSLmode`, `ApplicationName`, `Timeout`.
  The behavior-toggle keywords (BoolsAsChar, LFConversion, …) are **out of scope** for
  this dialog because `dsn_config.c` does not currently read them from a DSN — adding
  them here would write keys the driver ignores. (Tracked as a follow-up; see Notes.)
- **`driver_manager=none` compatibility:** the setup DLL includes `<odbcinst.h>` from the
  Windows SDK and links `odbccp32.lib` directly, so it builds under the existing Windows
  CI configuration (`-Ddriver_manager=none`).

## Relevant Files

### New files
- `src/setup/setup_windows.c` — `ConfigDSN`, `ConfigDSNW`, `ConfigDriver`; dialog
  procedure; read/write helpers over the ODBC installer API.
- `src/setup/setup_dialog.rc` — the dialog template and control layout.
- `src/setup/resource.h` — control ID constants shared by the `.rc` and the `.c`.
- `src/setup/psqlodbc2setup.def` — export list (`ConfigDSN`, `ConfigDSNW`,
  `ConfigDriver`).

### Modified files
- `src/meson.build` — add a Windows-only `shared_library('psqlodbc2setup', …)` target
  (compiled resources + the setup source), linking `odbccp32`, `odbc32`, `comctl32`,
  `user32`, `comdlg32`. Guard the whole block with
  `if host_machine.system() == 'windows'`.
- `installer/psqlodbc2.wxs` — add a `<File>` for `psqlodbc2setup.dll` and set the
  `<ODBCDriver>` element's `SetupFile` attribute to reference it, so the driver's
  `Setup=` registry value is written.
- `.github/workflows/release.yml` — copy `builddir/src/psqlodbc2setup.dll` into the
  `stage/` payload alongside `psqlodbc2w.dll` before the MSI build (the existing `<Files>`
  wildcard packages it, but WiX's `SetupFile` needs the file present at harvest time).

### Reference (read, do not modify)
- `psqlodbc2.def`, `installer/psqlodbc2.wxs` — current driver registration.
- `src/dsn_config.{c,h}` — the exact DSN key names/semantics the dialog must match.
- `docs/connection-options.md` (DSN key table), `docs/installation-windows.md`.
- Upstream `~/projects/psqlodbc`: `setup.c`, `dlg_specific.c`, `*.rc`.

## Implementation Phases

### Phase 1 — Setup DLL skeleton + build wiring
1. Create `src/setup/psqlodbc2setup.def` exporting `ConfigDSN`, `ConfigDSNW`,
   `ConfigDriver`.
2. Create `src/setup/setup_windows.c` with `ConfigDSN`/`ConfigDSNW` that handle
   `ODBC_ADD_DSN`, `ODBC_CONFIG_DSN`, `ODBC_REMOVE_DSN` request codes. For this phase,
   `ODBC_REMOVE_DSN` calls `SQLRemoveDSNFromIni`; add/config write a hardcoded/default
   set via `SQLWriteDSNToIni` + `SQLWritePrivateProfileString` (dialog comes in Phase 2).
   Implement `ConfigDriver` as a success stub.
3. Add the Windows-only target to `src/meson.build`; confirm the guard leaves the Linux
   build unchanged (`meson setup builddir` reconfigures cleanly; `psqlodbc2setup` does
   **not** appear as a target on Linux).

### Phase 2 — Dialog
1. Create `src/setup/resource.h` with control IDs (one per field + OK/Cancel).
2. Create `src/setup/setup_dialog.rc` laying out labeled edit controls for the honored
   DSN keys (Password field uses `ES_PASSWORD`).
3. In `setup_windows.c`, implement the dialog procedure: populate fields from the
   existing DSN on entry (`SQLGetPrivateProfileString`), validate the DSN name, and on OK
   write every field with `SQLWritePrivateProfileString`. Wire `ConfigDSN` add/config to
   invoke `DialogBoxParamW`. Prompt-on-`ODBC_PROMPT`/silent semantics per the request
   flags.
4. Compile the `.rc` via Meson's `windows` module and link it into the setup DLL.

### Phase 3 — Installer registration
1. In `installer/psqlodbc2.wxs`, add a `<File>` element for `psqlodbc2setup.dll` (its own
   component or within the driver component) and set `SetupFile` on `<ODBCDriver>` to that
   file's Id.
2. Update `.github/workflows/release.yml`'s "Stage payload" step to copy the setup DLL
   into `stage/`.

## Step by Step Tasks
- Execute in order; keep the Linux build green after every task.

### 1. Setup DLL skeleton and Meson target
- **Task ID**: setup-dll-skeleton
- **Depends On**: none
- Create `src/setup/psqlodbc2setup.def`, `src/setup/setup_windows.c` (ConfigDSN family
  handling the three request codes; remove works, add/config write without a dialog yet),
  and the guarded `shared_library('psqlodbc2setup', …)` in `src/meson.build`.
- Verify on Linux: `meson setup builddir --reconfigure && meson compile -C builddir &&
  meson test -C builddir` all pass and no `psqlodbc2setup` target is produced.

### 2. Dialog resource and procedure
- **Task ID**: setup-dialog
- **Depends On**: setup-dll-skeleton
- Add `resource.h`, `setup_dialog.rc`, and the dialog procedure; wire add/config to show
  it, load existing values, and save on OK. Compile the resource via the Meson `windows`
  module.
- Re-verify the Linux build/test stays green (the new files are Windows-only).

### 3. Installer registration
- **Task ID**: installer-setupfile
- **Depends On**: setup-dialog
- Add the setup DLL `<File>` and `SetupFile` attribute in `installer/psqlodbc2.wxs`;
  stage the DLL in `release.yml`.

### 4. Docs
- **Task ID**: update-docs
- **Depends On**: installer-setupfile
- Update `docs/installation-windows.md` Step 3 to document the dialog as the primary path
  (keep the DSN-less / registry routes as alternatives). Remove the "no setup dialog"
  callout and the corresponding troubleshooting bullet, since it now exists.

## Team Orchestration
- Deploy builder → reviewer → validator per task, looping on failures.
- **Run workers synchronously, not in the background** (background delegation has stalled
  the orchestrator here before).
- Never break the currently-passing Linux build/test suite.

## Validation
- **On this host (Linux) — must pass and is the gating check:**
  - `meson setup builddir --reconfigure` succeeds.
  - `meson compile -C builddir` succeeds with no new warnings.
  - `meson test -C builddir` — same 8 pass / 5 skip result as before.
  - `meson introspect builddir --targets` (or the compile output) shows **no**
    `psqlodbc2setup` target — proving the Windows guard works.
- **Windows behavior — DEFERRED to CI / a Windows machine (cannot be validated here):**
  - The setup DLL compiles under MSVC with `-Ddriver_manager=none`.
  - In the ODBC Data Source Administrator, Add…/Configure opens the dialog, values persist
    to the DSN, and a `DSN=` connection using them succeeds; Remove deletes the DSN.
  - The MSI installs the setup DLL and writes the driver's `Setup=` registry value.
  - Reviewer/validator should code-review the Windows path against the ODBC installer API
    contract, since they cannot execute it.

## Acceptance Criteria
- New files exist as listed; `src/meson.build` builds the setup DLL only on Windows.
- The Linux build and test suite remain green with no new warnings, and no
  `psqlodbc2setup` target is created on Linux.
- `ConfigDSN`/`ConfigDSNW`/`ConfigDriver` are exported by the setup DLL's `.def`.
- The WiX file references the setup DLL via `SetupFile`; `release.yml` stages it.
- Code follows CLAUDE.md conventions; the sole platform `#ifdef`s live in the
  Windows-only setup module (which is inherently platform-specific).

## Report
Summarize: which files were created/changed; confirmation the Linux build/test stayed
green (with the pass/skip counts) and that no Windows-only target leaked into the Linux
build; the exports the setup DLL provides; and an explicit statement of what remains
**unvalidated because it requires Windows** (MSVC compile, dialog behavior, MSI
registration) so the user knows CI/Windows verification is the next gate.

## Notes
- Behavior-toggle DSN keys (BoolsAsChar, LFConversion, UnknownSizes, etc.) are out of
  scope: `dsn_config.c` doesn't read them from a DSN today. A sensible follow-up is to
  teach `dsn_config.c` to read them *and* add matching dialog controls in one change, so
  the dialog never offers a key the driver ignores.
- unixODBC also drives DSN setup through `ConfigDSN`; a future refactor could share the
  logic, but this plan is Windows-only.
