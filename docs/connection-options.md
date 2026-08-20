# Connection Options Reference

This is the reference for everything you can put in a psqlodbc2 connection string or
DSN, plus the driver-specific connection attributes an application can set through
`SQLSetConnectAttr`.

- **Connection-string keywords** are parsed by the driver from the string passed to
  `SQLDriverConnect` / `SQLConnect`.
- **DSN keys** are read from `odbc.ini` (Unix) or the registry (Windows) when a
  connection string contains `DSN=<name>`.
- Values from a DSN are applied first; any keyword that appears *after* `DSN=` in the
  connection string overrides the DSN value.

Keywords and keys are matched case-insensitively.

---

## Connection-string keywords

### Connection parameters (passed through to libpq)

| Keyword | Aliases | Maps to (libpq) | Notes |
|---|---|---|---|
| `Server` | `Servername` | `host` | Hostname or IP of the PostgreSQL server. |
| `Port` | | `port` | TCP port (default PostgreSQL port is 5432). |
| `Database` | `DB` | `dbname` | Database name. |
| `UID` | `Username`, `User` | `user` | Login role. |
| `PWD` | `Password` | `password` | Stored on the heap and securely wiped before free. |
| `SSLmode` | | `sslmode` | Standard libpq values: `disable`, `allow`, `prefer`, `require`, `verify-ca`, `verify-full`. |
| `ApplicationName` | `Application_Name` | `application_name` | Shown in `pg_stat_activity`. |
| `Timeout` | `Connect_Timeout` | `connect_timeout` | Connection timeout in seconds. |

Any keyword not listed here is silently ignored, so unknown parameters from other tools
don't cause a connection failure.

### Behavior toggles (driver-specific)

| Keyword | Values | Default | Effect |
|---|---|---|---|
| `BoolsAsChar` | `0` / non-zero | **on** | When on, PostgreSQL `bool` columns are described to the application as `SQL_VARCHAR(5)` (textual `true`/`false`) instead of `SQL_BIT`. Some applications — notably MS Access — cannot handle the `BIT` type. Set `BoolsAsChar=0` to expose `bool` as `SQL_BIT`. |
| `UnknownSizes` | `0`, `1`, `2` | `0` | How the reported size of an unbounded variable-length column is computed: `0` = MAX (the configured maximum for the type), `1` = DONTKNOW (report `0`, let the app decide), `2` = LONGEST (scan fetched rows and report the longest actual value). |
| `MaxVarcharSize` | integer | `255` | Size (in characters) reported for `varchar`/`char` columns that have no declared length limit. |
| `Parse` | `0` / non-zero | off | When on, the driver parses `SELECT` statements client-side to refine result-column metadata — e.g. reporting a string literal in the select list as `VARCHAR(length)` rather than PostgreSQL's generic `text`. Matches the original driver's `Parse` option used by MS Access. |
| `FetchRefcursors` | `0` / non-zero | off | When on, a function/procedure returning `refcursor` OUT parameters has each cursor automatically `FETCH ALL`'d and exposed as successive result sets (walk them with `SQLMoreResults`). Because a refcursor is only valid inside the transaction that opened it, this requires autocommit **off**. When off, the OUT value is returned verbatim as the portal name. |
| `LFConversion` | `0` / non-zero | off | When on, every bare line feed (`\n`) in char/wchar output is expanded to a CR+LF pair (`\r\n`); the reported length counts the expanded bytes. Needed by some Windows applications that require CR+LF line endings. |
| `Protocol` | `7.4-N` | unset | Legacy keyword. Only the trailing `-N` is meaningful and selects the rollback-on-error mode (see below); the `7.4` prefix is ignored. |

### Rollback-on-error mode (`Protocol=7.4-N`)

Controls how the driver reacts when a statement errors inside an explicit
(autocommit-off) transaction:

| `N` | Mode | Behavior |
|---|---|---|
| `0` | NOTHING | Leave the transaction aborted; the application must issue its own `ROLLBACK` (via `SQLEndTran`) before continuing. |
| `1` | TRANSACTION | Automatically roll back the entire transaction on any error. |
| `2` | STATEMENT | Roll back only the failed statement (via an internal per-statement `SAVEPOINT`), preserving earlier successful work. |
| *(unset)* | default | Resolved at runtime from the server version — STATEMENT on servers new enough to support `SAVEPOINT`. |

### Packed bitfield keywords

The original driver packs several options into short hex-bitfield attributes. psqlodbc2
accepts them and reads the bits it implements:

| Keyword | Format | Bit read | Effect |
|---|---|---|---|
| `CX` | hex bitfield (optionally prefixed with a 2-hex-digit count when ≥ 2 chars) | `0x01` | Enables `LFConversion`. |
| `AB` | hex bitfield ("extra options") | `0x08` | Enables **CvtNullDate**: an empty bound character string (`""`) targeting a date/time column is sent as SQL `NULL` instead of the empty literal (which PostgreSQL rejects as *invalid input syntax*). Historically used by FoxPro. |

### Accepted but ignored (compatibility only)

This driver buffers each result set on the client, so several server-side-cursor
tunables from the original driver have no effect here. They are accepted so existing
connection strings and test suites don't break:

| Keyword | Why it has no effect |
|---|---|
| `Fetch` | Cursor fetch-cache size. The whole result set is materialized client-side, so there is no cache to size. |
| `UseDeclareFetch` | Selects a server-side `DECLARE`/`FETCH` cursor upstream. This driver always materializes results client-side (which inherently survives a mid-fetch `COMMIT`), so no distinct code path is needed. |
| `DisallowPremature` | Results are always described after execution, so there is no "premature" describe to disallow. |
| `Driver` | Consumed by the ODBC Driver Manager to select the driver library; irrelevant by the time the driver sees it. |

---

## DSN keys (`odbc.ini` / registry)

When the connection string contains `DSN=<name>`, the driver reads these keys from the
named DSN section. (DSN lookup requires `libodbcinst`; see
[build-options.md](build-options.md). If lookup is unavailable, `DSN=<name>` falls back
to treating `<name>` as the database name.)

| Key | Aliases | Meaning |
|---|---|---|
| `Servername` | `Server` | Server host. |
| `Port` | | TCP port. |
| `Database` | | Database name. |
| `Username` | `UID` | Login role. |
| `Password` | | Password. |
| `SSLmode` | | SSL mode (libpq values). |
| `ApplicationName` | | `application_name`. |
| `Timeout` | | Connection timeout in seconds. |
| `Description` | | Free-text description shown in ODBC administration tools. |

See [installation-unix.md](installation-unix.md) for a complete example `odbc.ini`.

---

## Driver-specific connection attributes

These are numeric attributes an application sets via `SQLSetConnectAttr`. They carry the
same numeric values as the original psqlodbc so existing applications work unchanged.

| Attribute | Value | Effect |
|---|---|---|
| `SQL_ATTR_PGOPT_MSJET` | `65549` | Enables MS Access / Jet compatibility: rewrites `("col" = 1)` boolean comparisons to `("col" = '1')` in the query parser. |
| `SQL_ATTR_PGOPT_BATCHSIZE` | `65550` | Array-execution batch size — how many bound parameter sets (rows) are grouped per server round-trip. A batch that errors marks all its rows as errored and stops. Default `100`; a value ≤ 0 means the default. |
| `SQL_ATTR_PGOPT_FETCH` | `65541` | Accepted but has no effect (see `Fetch` above). Present so applications that tune fetch size don't fail. |

---

## Examples

DSN-less connection string (Unix, driver library resolved via `odbcinst.ini`):

```
Driver={PostgreSQL ODBC Driver (psqlodbc2)};Server=localhost;Port=5432;Database=mydb;UID=postgres;PWD=secret
```

Using a DSN, overriding the database for one connection:

```
DSN=MyPostgres;Database=reporting
```

MS Access-friendly options:

```
DSN=MyPostgres;BoolsAsChar=1;Parse=1
```
