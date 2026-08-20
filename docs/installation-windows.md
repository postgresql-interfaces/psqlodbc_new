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

## Step 3 — Create a DSN (optional)

In the ODBC Data Source Administrator:

1. Choose the **User DSN** or **System DSN** tab.
2. Click **Add…**, select **PostgreSQL ODBC Driver (psqlodbc2)**, and click **Finish**.
3. Fill in the server, port, database, user, and other keys. The supported keys are
   listed in [connection-options.md](connection-options.md#dsn-keys-odbcini--registry).

On Windows, DSN definitions are stored in the registry (not an `odbc.ini` file); the
Administrator manages them for you.

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
- **`The setup routines ... could not be loaded` / DLL load failure** — a runtime
  dependency is missing. The MSI bundles libpq's DLLs next to the driver; if you moved
  the driver DLL out of its install folder, the loader can no longer find them.
