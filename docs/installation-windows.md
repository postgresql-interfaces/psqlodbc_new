# Installation on Windows

On Windows the driver ships as an **MSI installer** that copies the driver and its
runtime dependencies into place and registers it with the Windows ODBC subsystem
automatically. To build from source instead, see [building.md](building.md).

## Step 1 — Install the MSI

Download the installer for your architecture from the project releases:

- `psqlodbc2-<version>-windows-x64.msi` — 64-bit Intel/AMD
- `psqlodbc2-<version>-windows-arm64.msi` — ARM64

Run it and accept the license. The installer:

- Places the driver DLL (`psqlodbc2w.dll`) and its bundled libpq runtime DLLs (libpq
  plus OpenSSL/zlib/etc.) together in
  `C:\Program Files\psqlODBC2\<arch>\bin`. Keeping them in one folder lets the ODBC
  loader resolve libpq next to the driver at load time.
- Registers the driver with the Windows ODBC subsystem so it appears in the **ODBC Data
  Source Administrator**.

Administrator privileges are required (the driver is installed per-machine).

## Step 2 — Confirm the driver is registered

Open **ODBC Data Source Administrator** (search for "ODBC" in the Start menu; use the
64-bit version, `odbcad32.exe`). On the **Drivers** tab you should see:

> **PostgreSQL ODBC Driver (psqlodbc2)**

That exact string is also what you use as the `Driver=` value in DSN-less connection
strings.

## Step 3 — Configuring connections

> **No setup dialog.** Unlike the original psqlodbc, this driver does not (yet) provide
> a configuration GUI. It exports no `ConfigDSN` routine and registers no setup DLL, so
> selecting it in the ODBC Data Source Administrator and clicking **Add…** or
> **Configure** will *not* open a driver-specific dialog to fill in server/port/database
> (Windows may report that the driver's setup routines could not be found). Configure
> connections one of the two ways below instead. A native setup dialog is tracked as
> future work in [specs/windows-setup-dialog.md](../specs/windows-setup-dialog.md).

### Option A — DSN-less connection string (recommended)

No registry setup is needed. Applications pass all options in the connection string:

```
Driver={PostgreSQL ODBC Driver (psqlodbc2)};Server=localhost;Port=5432;Database=mydb;UID=postgres;PWD=secret
```

See [connection-options.md](connection-options.md) for every supported keyword.

### Option B — Create a DSN via the registry

On Windows, DSN definitions live in the registry (there is no `odbc.ini` file). Because
there is no setup dialog, create the entries directly. Save the following as a `.reg`
file and import it (adjust the values and, for a User DSN, use `HKEY_CURRENT_USER`
instead of `HKEY_LOCAL_MACHINE`):

```reg
Windows Registry Editor Version 5.00

; Register the DSN name against the driver
[HKEY_LOCAL_MACHINE\SOFTWARE\ODBC\ODBC.INI\ODBC Data Sources]
"MyPostgres"="PostgreSQL ODBC Driver (psqlodbc2)"

; The DSN's parameters
[HKEY_LOCAL_MACHINE\SOFTWARE\ODBC\ODBC.INI\MyPostgres]
"Driver"="PostgreSQL ODBC Driver (psqlodbc2)"
"Servername"="localhost"
"Port"="5432"
"Database"="mydb"
"Username"="postgres"
"Password"="secret"
"SSLmode"="prefer"
```

The supported DSN keys are listed in
[connection-options.md](connection-options.md#dsn-keys-odbcini--registry). Once imported,
the DSN appears in the ODBC Data Source Administrator and applications can connect with
`DSN=MyPostgres`.

## Connecting

Applications can connect two ways:

- **By DSN:** `DSN=MyPostgres`
- **DSN-less:**

  ```
  Driver={PostgreSQL ODBC Driver (psqlodbc2)};Server=localhost;Port=5432;Database=mydb;UID=postgres;PWD=secret
  ```

See [connection-options.md](connection-options.md) for every supported keyword.

## Upgrading and uninstalling

- **Upgrade:** run a newer MSI; it removes the prior version and installs the new one in
  place. Installing the same or an older version over a newer one is blocked with a
  "newer version is already installed" message.
- **Uninstall:** use **Settings → Apps** (or Programs and Features) and remove
  **psqlODBC2**.
- The x64 and ARM64 builds are distinct products and each has its own upgrade identity,
  so they can be managed independently.

## Troubleshooting

- **Driver not listed in the Administrator** — make sure you opened the 64-bit
  Administrator for a 64-bit driver (a 32-bit application uses the 32-bit
  `odbcad32.exe`, which has a separate driver list).
- **"The setup routines for the … driver could not be found" when clicking Add… /
  Configure** — expected: this driver has no configuration GUI (see Step 3). Configure
  connections with a DSN-less connection string or a registry-created DSN instead.
- **DLL load failure when connecting** — a runtime dependency is missing. The MSI
  bundles libpq's DLLs next to the driver; if you moved the driver DLL out of its
  install folder, the loader can no longer find them.
