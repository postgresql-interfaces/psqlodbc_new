# Plan: Windows DSN Setup Dialog (Future Work)

> **Status: not implemented.** This is a design note for a feature the driver does not
> yet have. It exists so the gap is recorded and can be picked up later.

## Problem
On Windows, the original psqlodbc provides a configuration GUI: selecting the driver in
the ODBC Data Source Administrator and clicking **Add…** / **Configure** opens a tabbed
dialog with fields and checkboxes for the connection options (server, port, database,
SSL mode, BoolsAsChar, LFConversion, etc.). psqlodbc2 has **no such dialog**:

- The driver exports no `ConfigDSN` / `ConfigDriver` routine (`psqlodbc2.def` lists only
  the ODBC API functions).
- The WiX installer registers only the driver via `<ODBCDriver>` — it writes no `Setup=`
  key into `ODBCINST.INI` and installs no setup component (`installer/psqlodbc2.wxs`).

As a result, users must configure connections via DSN-less connection strings or by
creating DSN registry entries by hand (see `docs/installation-windows.md`). Clicking
**Configure** in the Administrator yields "the setup routines … could not be found".

## Objective
Provide a Windows setup dialog so a DSN can be created and edited through the ODBC Data
Source Administrator, matching the convenience of the original driver — without
regressing the Unix build or the driver's exported ODBC surface.

## Approach (sketch — to be refined when picked up)
- Implement the ODBC installer entry points `ConfigDSN` (and optionally `ConfigDriver`,
  `ConfigDSNW`). These live in a *setup* module, conventionally a separate DLL
  (`psqlodbc2S.dll` upstream) so the driver DLL itself stays free of GUI/`comctl32`
  dependencies. Decide: separate setup DLL vs. exporting from the driver DLL.
- Build the dialog with the Win32 API (a dialog resource + `DialogBox`), reading/writing
  DSN keys through `SQLWritePrivateProfileString` / `SQLGetPrivateProfileString`. Keep it
  Windows-only behind the platform abstraction; the Unix build must be unaffected.
- Cover the DSN keys the driver actually honors — keep this in sync with
  `docs/connection-options.md` (the DSN key table) and `src/dsn_config.c`.
- Register the setup component: add the `Setup=` value to the ODBC driver registration
  in `installer/psqlodbc2.wxs` (the `<ODBCDriver>` / driver registry rows) and package
  the setup DLL in the MSI payload.

## Relevant Files (when implementing)
- `psqlodbc2.def` — would gain the `ConfigDSN` family exports (or a new setup DLL gets
  its own `.def`).
- `installer/psqlodbc2.wxs` — register the `Setup=` entry and ship the setup DLL.
- `src/dsn_config.{c,h}` — the DSN key names/semantics the dialog must read/write.
- `docs/connection-options.md`, `docs/installation-windows.md` — update once the dialog
  exists (Step 3 currently documents the manual registry route).
- Reference: `~/projects/psqlodbc` (`setup.c`, `dlg_specific.c`, the `.rc` resources).

## Acceptance Criteria (when done)
- In the ODBC Data Source Administrator, **Add…** for
  `PostgreSQL ODBC Driver (psqlodbc2)` opens a dialog; entered values persist to the DSN
  and a connection using that DSN succeeds.
- **Configure** on an existing DSN loads its current values and saves edits.
- The Linux/macOS build is unchanged (no GUI code compiled there).
- `docs/installation-windows.md` Step 3 is updated to describe the dialog, and this note
  is removed or marked done.

## Notes
- This is Windows-only. unixODBC has its own GUI tools (e.g. `ODBCManageDataSourcesQ4`)
  driven by the same `ConfigDSN` mechanism, so a well-factored setup module could
  potentially serve both, but that is out of scope for the initial dialog.
