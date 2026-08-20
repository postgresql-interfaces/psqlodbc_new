# Installation on Unix-like Systems (Linux & macOS)

This guide covers installing the psqlodbc2 driver and registering it with a
[unixODBC](https://www.unixodbc.org/) driver manager. To build from source instead, see
[building.md](building.md).

## Prerequisites

- A **driver manager** — unixODBC is recommended:
  - Debian/Ubuntu: `sudo apt-get install unixodbc`
  - macOS (Homebrew): `brew install unixodbc`
- The **PostgreSQL client library (libpq)** must be available at runtime:
  - Debian/Ubuntu: `sudo apt-get install libpq5`
  - macOS (Homebrew): `brew install libpq`

DSN lookup from `odbc.ini` requires the driver to have been built against `libodbcinst`
(shipped with unixODBC). Prebuilt release binaries are built this way. Without it, the
driver still connects via explicit connection-string parameters, but a `DSN=<name>` is
treated as a database name rather than a lookup key.

## Step 1 — Install the driver library

Obtain `libpsqlodbc2w.so` (Linux) or `libpsqlodbc2w.dylib` (macOS) — either from a
release tarball or from a local build (`builddir/src/`) — and copy it to a stable
location, for example:

```bash
sudo cp libpsqlodbc2w.so /usr/local/lib/
```

Note the full path; you'll reference it when registering the driver.

## Step 2 — Register the driver with unixODBC

Driver definitions live in `odbcinst.ini`. Find its location with:

```bash
odbcinst -j
```

Create a template file — `psqlodbc2.ini`:

```ini
[PostgreSQL ODBC Driver (psqlodbc2)]
Description = Modern PostgreSQL ODBC driver (psqlodbc2)
Driver      = /usr/local/lib/libpsqlodbc2w.so
```

Register it:

```bash
sudo odbcinst -i -d -f psqlodbc2.ini
```

(You can also edit `odbcinst.ini` directly instead of using `odbcinst -i`.) Verify:

```bash
odbcinst -q -d          # lists registered driver names
```

The name in square brackets — `PostgreSQL ODBC Driver (psqlodbc2)` — is what you use as
the `Driver=` value in DSN-less connection strings. Matching the Windows driver name
keeps connection strings portable across platforms.

## Step 3 — Create a DSN (optional)

Data sources live in `odbc.ini` (system-wide) or `~/.odbc.ini` (per-user). Add a
section named for your DSN:

```ini
[MyPostgres]
Description = My PostgreSQL database
Driver      = PostgreSQL ODBC Driver (psqlodbc2)
Servername  = localhost
Port        = 5432
Database    = mydb
Username    = postgres
Password    = secret
SSLmode     = prefer
```

The full list of DSN keys is in
[connection-options.md](connection-options.md#dsn-keys-odbcini--registry).

Verify the DSN with `isql` (part of unixODBC):

```bash
isql -v MyPostgres
```

## Connecting

Applications can connect two ways:

- **By DSN:** `DSN=MyPostgres`
- **DSN-less (no `odbc.ini` entry needed):**

  ```
  Driver={PostgreSQL ODBC Driver (psqlodbc2)};Server=localhost;Port=5432;Database=mydb;UID=postgres;PWD=secret
  ```

See [connection-options.md](connection-options.md) for every supported keyword.

## macOS notes

- Homebrew installs unixODBC and libpq under `/opt/homebrew` on Apple Silicon and
  `/usr/local` on Intel. Adjust the `Driver=` path in Step 2 accordingly (e.g. the
  driver you built will reference libpq from the Homebrew prefix).
- libpq is keg-only on Homebrew. If an application can't load the driver because libpq
  isn't found at runtime, ensure the Homebrew libpq `lib` directory is on the loader
  path (or that the driver was linked with an rpath to it).
- Find the active ODBC config paths with `odbcinst -j`.

## Troubleshooting

- **`Can't open lib ... file not found`** — the `Driver=` path in `odbcinst.ini` is
  wrong, or a dependent library (libpq) isn't on the loader path.
- **`Data source name not found`** — the DSN section name doesn't match, or the driver
  was built without `libodbcinst` so DSN lookup is unavailable. Use a DSN-less
  connection string, or install unixODBC and rebuild.
- Use `isql -v <DSN>` for verbose connection diagnostics.
