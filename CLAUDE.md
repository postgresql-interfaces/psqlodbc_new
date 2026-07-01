# psqlodbc2 — Modernized PostgreSQL ODBC Driver

A ground-up rewrite of the PostgreSQL ODBC driver using modern C11 practices.

## Project Facts

- **Language**: C11 (no GNU extensions without platform guards)
- **Build system**: Meson
- **Platforms**: Linux, macOS, Windows
- **Goal**: Drop-in replacement for the original psqlodbc
- **Original source reference**: `~/projects/psqlodbc`

## Build Commands

```bash
meson setup builddir          # First-time configure
meson compile -C builddir     # Build
meson test -C builddir        # Run tests
```

## Team Workflow

1. `/plan_w_team <description>` — generates a plan in `specs/`
2. `/build specs/<plan-name>.md` — executes the plan with builder/reviewer/validator agents

## Conventions

- One module per logical ODBC subsystem (connection, statement, descriptor, results, etc.)
- Header files declare public API; implementation files are internal
- All ODBC entry points go in a single `odbc_api.c` dispatch file
- Error handling uses ODBC diagnostic records (SQLGetDiagRec pattern)
- Platform abstraction layer in `platform/` directory

## Code Readability (MANDATORY)

- **Descriptive names**: All functions, variables, structs, and typedefs must be self-documenting. No single-letter names (except `i`/`j` loop counters). No cryptic abbreviations — use full words (e.g., `connection_info` not `ci`, `statement_handle` not `hstmt`, `result_set` not `res`).
- **Comments explain WHY**: Every non-obvious decision, constraint, workaround, or business rule gets a comment explaining intent. Never comment WHAT the code does — the code shows that.
- **No magic numbers**: Named constants or enums for all numeric values.
- **Newcomer test**: A developer unfamiliar with this codebase should be able to read any file and understand it without external context.
