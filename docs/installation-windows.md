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

## Step 3 — Create a DSN

The driver ships a setup dialog, so you can create a DSN directly in the ODBC Data
Source Administrator:

1. Choose the **User DSN** or **System DSN** tab.
2. Click **Add…**, select **PostgreSQL ODBC Driver (psqlodbc2)**, and click **Finish**.
3. In the **PostgreSQL ODBC Driver (psqlodbc2) DSN Setup** dialog, enter a data source
   name plus the server, port, database, user, and other fields, then click **OK**.
4. To change a DSN later, select it and click **Configure**; to delete it, click
   **Remove**.

The dialog exposes the DSN keys the driver honors (server, port, database, username,
password, SSL mode, application name, timeout); the full list is in
[connection-options.md](connection-options.md#dsn-keys-odbcini--registry). On Windows,
DSN definitions are stored in the registry — the Administrator manages that for you.
Applications then connect with `DSN=MyPostgres`.

### Alternatives to a DSN

**DSN-less connection string** — no DSN or registry setup at all; the application passes
every option inline:

```
Driver={PostgreSQL ODBC Driver (psqlodbc2)};Server=localhost;Port=5432;Database=mydb;UID=postgres;PWD=secret
```

See [connection-options.md](connection-options.md) for every supported keyword.

**Registry import** — to script DSN creation without opening the dialog, save the
following as a `.reg` file and import it (adjust the values and, for a User DSN, use
`HKEY_CURRENT_USER` instead of `HKEY_LOCAL_MACHINE`):

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
  Configure** — the setup DLL (`psqlodbc2setup.dll`) is missing or not registered.
  It is installed and registered by the MSI; reinstall if it was removed or if the DLL
  was moved out of its install folder. As a workaround, use a DSN-less connection string
  or the registry import in Step 3.
- **DLL load failure when connecting** — a runtime dependency is missing. The MSI
  bundles libpq's DLLs next to the driver; if you moved the driver DLL out of its
  install folder, the loader can no longer find them.
