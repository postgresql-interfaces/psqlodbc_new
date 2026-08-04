# Diagnosis: Cursor Cluster Regression Failures

## Purpose
This document records the **observed** root causes of the 9 failing cursor-family
regression tests, gathered by running the full upstream suite against the driver
on 2026-08-04 (result: 35 passed, 25 failed, 0 skipped). It grounds the two
existing implementation specs — [`scrollable-cursors.md`](scrollable-cursors.md)
and [`bulk-operations-bookmarks.md`](bulk-operations-bookmarks.md) — in concrete
failure evidence, and flags where those specs' phase assignments disagree with
what the tests actually exercise. Read this before executing either spec.

## How the failures were captured
- Full suite run via `regress/run_regression.sh` (now defaults to all 60 upstream
  tests) against a local PostgreSQL with database `contrib_regression`.
- Per-test stdout/stderr captured in `regress/work/results/<test>.out`.
- Failure classification below comes from those captures plus the driver's
  exported-symbol list (`psqlodbc2.def`) and `grep` over `src/`.

## The 9 cursor-cluster tests and their observed cause

| Test | Observed failure | Root cause | Covered by |
|---|---|---|---|
| cursor-movement | `IM001 Driver does not support this function` on `SQL_FETCH_NEXT` | `SQLFetchScroll` not exported/implemented | scrollable Phase 1 |
| cursor-scrollable | empty output / early exit | `SQLFetchScroll` missing | scrollable Phase 1 |
| declare-fetch-block | crash/error | `SQLFetchScroll` + block fetch missing | scrollable Phase 1/3 |
| cursor-name | `IM001` | `SQLSetCursorName` / `SQLGetCursorName` missing | scrollable Phase 2 |
| positioned-update | `IM001` on `SQLSetPos` UPDATE | `SQLSetPos` missing | bulk-ops Phase 2 |
| cursor-block-delete | `IM001` | `SQLSetPos` + `SQLBulkOperations` + `SQLFetchScroll` missing | bulk-ops Phase 2/3 |
| **cursors** | reports `SQL_CB_CLOSE`; `SQLFetch` after COMMIT fails `HY010` | **Wrong `SQLGetInfo` value + buffer discarded at commit** — NOT DECLARE/FETCH | see discrepancy below |
| cursor-commit | crash/error | `SQLFetchScroll` missing (uses DECLARE/FETCH path too) | scrollable Phase 1/3 |
| declare-fetch-commit | `25P02 current transaction is aborted` | Needs `UseDeclareFetch=1;Fetch=1` server-side cursor mode | scrollable Phase 3 |

## Confirmed root cause: missing entry points
None of the following exist anywhere in `src/`, and none appear in
`psqlodbc2.def`. unixODBC therefore rejects the calls with
`IM001 Driver does not support this function` before the driver ever runs:

- `SQLFetchScroll` — blocks cursor-movement, cursor-scrollable, cursor-commit,
  declare-fetch-block, cursor-block-delete, positioned-update
- `SQLSetPos` — blocks positioned-update, cursor-block-delete
- `SQLBulkOperations` — blocks cursor-block-delete
- `SQLSetCursorName` / `SQLGetCursorName` — blocks cursor-name
- `SQLExtendedFetch`, `SQLCloseCursor` — absent (not directly asserted by these
  tests but expected by the ODBC surface)

**Leverage note:** the driver already buffers each full result set client-side
(`PQexec` materializes the entire `PGresult`; `results.c` tracks a single
`current_row_position`). So `SQLFetchScroll`'s orientation engine (NEXT / PRIOR /
FIRST / LAST / ABSOLUTE / RELATIVE) is arithmetic over the existing buffer — no
server protocol work — and is the single highest-leverage change, unblocking
~5 tests. This matches `scrollable-cursors.md` Phase 1.

## Discrepancy with the existing spec — must fix before executing

`scrollable-cursors.md` places **`cursors`** in Phase 3 (server-side
DECLARE/FETCH). The captured evidence shows this is wrong:

- `cursors-test.c` connects with plain `test_connect()` — **no**
  `UseDeclareFetch`. It does not use the server-side cursor path at all.
- Its actual failure is two-part, both client-side:
  1. `odbc_api.c` (around lines 3277–3281) hardcodes `SQL_CB_CLOSE` for both
     `SQL_CURSOR_COMMIT_BEHAVIOR` and `SQL_CURSOR_ROLLBACK_BEHAVIOR`; the test
     expects `SQL_CB_PRESERVE`.
  2. After `SQLEndTran(COMMIT)` the driver's `SQLFetch` returns `HY010`, i.e. the
     cached result is being discarded/invalidated at commit.
- Because the whole result is buffered client-side, the cursor genuinely *does*
  survive a commit. Reporting `SQL_CB_PRESERVE` is therefore both what the test
  wants and factually correct for this implementation — provided the fetch buffer
  is not torn down on `SQLEndTran`.

**Action:** move `cursors` out of Phase 3 and into an early, cheap step alongside
the GetInfo fix (it does not depend on DECLARE/FETCH). Keep `cursor-commit` and
`declare-fetch-commit` in Phase 3 — those two *do* use the server-side cursor
path (`declare-fetch-commit-test.c` connects with
`UseDeclareFetch=1;Fetch=1;Protocol=7.4-2`).

## Suggested execution order (revised)
1. **`SQLGetInfo` cursor-behavior fix + verify fetch buffer survives commit** →
   fixes `cursors`. Smallest change, do first.
2. **`SQLFetchScroll` client-side orientation engine** (scrollable Phase 1) →
   fixes cursor-movement, cursor-scrollable, and unblocks declare-fetch-block.
3. **Cursor naming** (scrollable Phase 2) → fixes cursor-name.
4. **Server-side DECLARE/FETCH** (scrollable Phase 3) → fixes cursor-commit,
   declare-fetch-commit.
5. **`SQLSetPos` + keyset via ctid, then `SQLBulkOperations`**
   (bulk-ops Phases 2–3) → fixes positioned-update, cursor-block-delete.

Steps 1–2 alone recover ~4 tests and are self-contained; they are the
recommended first slice.

## Verification
Re-run just the cluster after each step:

```bash
./regress/run_regression.sh cursors cursor-movement cursor-scrollable \
  cursor-commit cursor-name cursor-block-delete positioned-update \
  declare-fetch-commit declare-fetch-block
```

Then run the full suite to confirm no regression in the 35 currently passing:

```bash
./regress/run_regression.sh
```

## Out of scope
Positioned update/delete and bookmark navigation detail live in
[`bulk-operations-bookmarks.md`](bulk-operations-bookmarks.md). Non-cursor
failures (result-conversions, arraybinding, large-object, etc.) are separate
clusters and not addressed here.
