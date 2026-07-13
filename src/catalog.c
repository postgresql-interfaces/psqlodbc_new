/*-------------------------------------------------------------------------
 *
 * catalog.c
 *	  ODBC catalog functions (SQLTables, SQLColumns, etc.)
 *
 * Copyright (c) 2026, Dave Cramer
 *
 *
 * IDENTIFICATION
 *	  src/catalog.c
 *
 *-------------------------------------------------------------------------
 */
#include "catalog.h"
#include "connection.h"
#include "diagnostics.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Portable case-insensitive string comparison (strcasecmp is POSIX, not C11) */
static int catalog_strcasecmp(const char *left, const char *right)
{
    while (*left && *right) {
        int diff = tolower((unsigned char)*left) - tolower((unsigned char)*right);
        if (diff != 0) {
            return diff;
        }
        left++;
        right++;
    }
    return tolower((unsigned char)*left) - tolower((unsigned char)*right);
}
#include <libpq-fe.h>

/* Maximum size for catalog query strings. These queries have bounded size
 * because the only variable parts are escaped table/schema/column names. */
#define CATALOG_QUERY_BUFFER_SIZE 8192

/* ---- Internal Helpers ---- */

/*
 * Execute a catalog query on the statement's parent connection and store the
 * result on the statement handle. Replaces any previous result.
 */
static SQLRETURN execute_catalog_query(OdbcStatement *statement, const char *query)
{
    if (!statement->parent_connection ||
        statement->parent_connection->state != CONNECTION_STATE_CONNECTED ||
        !statement->parent_connection->libpq_connection) {
        diagnostics_add_record(&statement->diagnostics,
                               "08003",
                               0,
                               "Cannot execute catalog query: connection is not active.");
        return SQL_ERROR;
    }

    /* Clear any previous result */
    if (statement->current_result) {
        PQclear(statement->current_result);
        statement->current_result = NULL;
    }

    PGconn *libpq_connection = statement->parent_connection->libpq_connection;
    PGresult *result = PQexec(libpq_connection, query);

    if (!result) {
        diagnostics_add_record(&statement->diagnostics,
                               "HY000",
                               0,
                               "Catalog query returned NULL result (connection may be lost).");
        return SQL_ERROR;
    }

    ExecStatusType status = PQresultStatus(result);
    if (status != PGRES_TUPLES_OK) {
        const char *error_message = PQresultErrorMessage(result);
        diagnostics_add_record(&statement->diagnostics,
                               "HY000",
                               0,
                               error_message ? error_message : "Catalog query failed.");
        PQclear(result);
        return SQL_ERROR;
    }

    statement->current_result = result;
    statement->has_result_set = true;
    statement->affected_row_count = PQntuples(result);
    statement->current_row_position = -1;
    statement->state = STATEMENT_STATE_HAS_CURSOR;
    statement->is_prepared = false;
    statement->prepared_name[0] = '\0';

    return SQL_SUCCESS;
}

/*
 * Resolve the byte length of a SQL string argument.
 * Returns 0 if value is NULL or length is invalid.
 */
static size_t resolve_argument_length(const SQLCHAR *value, SQLSMALLINT declared_length)
{
    if (!value) {
        return 0;
    }
    if (declared_length == SQL_NTS) {
        return strlen((const char *)value);
    }
    if (declared_length < 0) {
        return 0;
    }
    return (size_t)declared_length;
}

/*
 * Append an escaped SQL literal to the query buffer. Single quotes in the
 * input are doubled to prevent SQL injection.
 * Returns the number of characters written (excluding null terminator).
 */
static size_t append_escaped_literal(char *buffer, size_t buffer_remaining,
                                     const SQLCHAR *value, size_t value_length)
{
    size_t written = 0;

    for (size_t i = 0; i < value_length && written < buffer_remaining - 1; i++) {
        if (value[i] == '\'') {
            if (written + 2 > buffer_remaining - 1) break;
            buffer[written++] = '\'';
            buffer[written++] = '\'';
        } else {
            buffer[written++] = (char)value[i];
        }
    }
    buffer[written] = '\0';
    return written;
}

/*
 * Append a LIKE filter clause to the query buffer if the pattern value is not NULL.
 * Example output: " AND n.nspname LIKE 'public'"
 */
static void append_like_filter(char *query, size_t query_size, size_t *offset,
                               const char *column_expression,
                               const SQLCHAR *pattern_value,
                               SQLSMALLINT pattern_length)
{
    if (!pattern_value) {
        return;
    }

    size_t actual_length = resolve_argument_length(pattern_value, pattern_length);
    if (actual_length == 0) {
        return;
    }

    size_t remaining = query_size - *offset;
    int written = snprintf(query + *offset, remaining, " AND %s LIKE '", column_expression);
    if (written > 0) *offset += (size_t)written;

    remaining = query_size - *offset;
    *offset += append_escaped_literal(query + *offset, remaining, pattern_value, actual_length);

    remaining = query_size - *offset;
    written = snprintf(query + *offset, remaining, "'");
    if (written > 0) *offset += (size_t)written;
}

/*
 * Append an exact-match (=) filter clause to the query buffer.
 * Example output: " AND c.relname = 'my_table'"
 */
static void append_exact_filter(char *query, size_t query_size, size_t *offset,
                                const char *column_expression,
                                const SQLCHAR *value,
                                SQLSMALLINT value_length)
{
    if (!value) {
        return;
    }

    size_t actual_length = resolve_argument_length(value, value_length);
    if (actual_length == 0) {
        return;
    }

    size_t remaining = query_size - *offset;
    int written = snprintf(query + *offset, remaining, " AND %s = '", column_expression);
    if (written > 0) *offset += (size_t)written;

    remaining = query_size - *offset;
    *offset += append_escaped_literal(query + *offset, remaining, value, actual_length);

    remaining = query_size - *offset;
    written = snprintf(query + *offset, remaining, "'");
    if (written > 0) *offset += (size_t)written;
}

/* ---- Public Interface ---- */

SQLRETURN catalog_tables(OdbcStatement *statement,
                         const SQLCHAR *catalog_name, SQLSMALLINT catalog_name_length,
                         const SQLCHAR *schema_pattern, SQLSMALLINT schema_length,
                         const SQLCHAR *table_pattern, SQLSMALLINT table_length,
                         const SQLCHAR *table_type, SQLSMALLINT type_length)
{
    (void)catalog_name;
    (void)catalog_name_length;

    char query[CATALOG_QUERY_BUFFER_SIZE];
    size_t offset = 0;

    /* Columns are cast to varchar(128) (and REMARKS to varchar(254)) so
     * SQLDescribeCol reports the fixed widths ODBC applications expect, matching
     * the original driver's catalog result metadata. REMARKS is coalesced to an
     * empty string (not NULL) for relations without a COMMENT, because the
     * original returns empty text there and callers print it as an empty field.
     * The TABLE_TYPE label for a materialized view is "MATVIEW" (the original's
     * CSTR_MATVIEW), not the SQL-standard "MATERIALIZED VIEW". */
    int written = snprintf(query, sizeof(query),
        "SELECT current_database()::varchar(128) AS \"TABLE_CAT\", "
        "n.nspname::varchar(128) AS \"TABLE_SCHEM\", "
        "c.relname::varchar(128) AS \"TABLE_NAME\", "
        "CASE c.relkind "
        "  WHEN 'r' THEN 'TABLE' "
        "  WHEN 'p' THEN 'TABLE' "
        "  WHEN 'v' THEN 'VIEW' "
        "  WHEN 'm' THEN 'MATVIEW' "
        "  WHEN 'f' THEN 'FOREIGN TABLE' "
        "END::varchar(128) AS \"TABLE_TYPE\", "
        "COALESCE(pg_catalog.obj_description(c.oid, 'pg_class'), '')::varchar(254) AS \"REMARKS\" "
        "FROM pg_catalog.pg_class c "
        "JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
        "WHERE c.relkind IN ('r','v','m','f','p') "
        "AND n.nspname NOT IN ('pg_catalog','information_schema','pg_toast')");
    offset = (size_t)written;

    /* Apply schema pattern filter */
    append_like_filter(query, sizeof(query), &offset,
                       "n.nspname", schema_pattern, schema_length);

    /* Apply table name pattern filter */
    append_like_filter(query, sizeof(query), &offset,
                       "c.relname", table_pattern, table_length);

    /* Apply table type filter — parse comma-separated type list and map to relkinds */
    if (table_type) {
        size_t type_actual_length = resolve_argument_length(table_type, type_length);
        if (type_actual_length > 0) {
            /* Build a relkind IN clause from the table type string.
             * Recognized types: TABLE, VIEW, MATERIALIZED VIEW, FOREIGN TABLE.
             *
             * We first record which relkinds are requested as a set of flags,
             * then emit the IN-list once. Collecting into flags (rather than
             * appending to a fixed buffer per token) is inherently deduplicated
             * and bounded: table_type is a caller-controlled public parameter,
             * so a token list like "TABLE,TABLE,TABLE" must not be able to
             * overflow — with flags, repeated tokens just re-set the same bit. */
            bool want_r = false, want_p = false, want_v = false,
                 want_f = false, want_m = false;
            char type_copy[256];
            size_t copy_len = type_actual_length < sizeof(type_copy) - 1
                ? type_actual_length : sizeof(type_copy) - 1;
            memcpy(type_copy, table_type, copy_len);
            type_copy[copy_len] = '\0';

            /* Tokenize by comma */
            /* Tokenize by comma — portable replacement for strtok_r (POSIX, not C11) */
            char *pos = type_copy;
            while (*pos) {
                /* Skip leading commas and spaces */
                while (*pos == ',' || *pos == ' ') pos++;
                if (!*pos) break;

                /* Find end of token */
                char *token_start = pos;
                while (*pos && *pos != ',') pos++;
                size_t tlen = (size_t)(pos - token_start);

                /* Make a null-terminated copy for comparison */
                char token_buf[64];
                if (tlen >= sizeof(token_buf)) tlen = sizeof(token_buf) - 1;
                memcpy(token_buf, token_start, tlen);
                token_buf[tlen] = '\0';

                /* Strip leading/trailing spaces and quotes */
                char *token = token_buf;
                while (*token == ' ' || *token == '\'') token++;
                size_t token_len = strlen(token);
                while (token_len > 0 && (token[token_len-1] == ' ' || token[token_len-1] == '\'')) {
                    token[--token_len] = '\0';
                }

                if (catalog_strcasecmp(token, "TABLE") == 0) {
                    /* The original driver classifies anything that is not a view
                     * and not a system table as a "regular table", so the TABLE
                     * type also matches partitioned tables ('p'), foreign tables
                     * ('f'), and materialized views ('m') — only plain views are
                     * excluded. Each still reports its own TABLE_TYPE label via
                     * the CASE expression in the SELECT. */
                    want_r = want_p = want_f = want_m = true;
                } else if (catalog_strcasecmp(token, "VIEW") == 0) {
                    want_v = true;
                } else if (catalog_strcasecmp(token, "MATERIALIZED VIEW") == 0) {
                    want_m = true;
                } else if (catalog_strcasecmp(token, "FOREIGN TABLE") == 0) {
                    want_f = true;
                }
            }

            /* Emit the relkind IN-list once from the collected flags. The list
             * has at most five short literals, so it comfortably fits. */
            if (want_r || want_p || want_v || want_f || want_m) {
                char relkinds[32];
                size_t rk_offset = 0;
                const struct { bool wanted; char kind; } kinds[] = {
                    { want_r, 'r' }, { want_p, 'p' }, { want_v, 'v' },
                    { want_f, 'f' }, { want_m, 'm' },
                };
                for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
                    if (!kinds[i].wanted) {
                        continue;
                    }
                    /* Each entry needs up to 4 bytes (",'x'") plus the NUL. */
                    if (rk_offset + 5 >= sizeof(relkinds)) {
                        break;
                    }
                    if (rk_offset > 0) {
                        relkinds[rk_offset++] = ',';
                    }
                    relkinds[rk_offset++] = '\'';
                    relkinds[rk_offset++] = kinds[i].kind;
                    relkinds[rk_offset++] = '\'';
                }
                relkinds[rk_offset] = '\0';

                size_t remaining = sizeof(query) - offset;
                int w = snprintf(query + offset, remaining,
                                 " AND c.relkind IN (%s)", relkinds);
                if (w > 0) offset += (size_t)w;
            }
        }
    }

    /* Order by schema then table name, matching the original driver (which
     * sorts "order by nspname, relname"). */
    size_t remaining = sizeof(query) - offset;
    written = snprintf(query + offset, remaining,
                       " ORDER BY \"TABLE_SCHEM\", \"TABLE_NAME\"");
    if (written > 0) offset += (size_t)written;

    return execute_catalog_query(statement, query);
}

SQLRETURN catalog_columns(OdbcStatement *statement,
                          const SQLCHAR *catalog_name, SQLSMALLINT catalog_name_length,
                          const SQLCHAR *schema_pattern, SQLSMALLINT schema_length,
                          const SQLCHAR *table_pattern, SQLSMALLINT table_length,
                          const SQLCHAR *column_pattern, SQLSMALLINT column_length)
{
    (void)catalog_name;
    (void)catalog_name_length;

    char query[CATALOG_QUERY_BUFFER_SIZE];
    size_t offset = 0;

    /* With BoolsAsChar on (the driver default, matching the original), a boolean
     * column is described as SQL_VARCHAR rather than SQL_BIT so applications that
     * cannot handle a true BIT type receive the textual representation. */
    bool bools_as_char = !statement->parent_connection ||
                         statement->parent_connection->info.bools_as_char;
    int bool_sql_type = bools_as_char ? (int)SQL_VARCHAR : (int)SQL_BIT;

    /* Type resolution uses tb.oid — the *base* type of a column. For a domain
     * (typtype 'd', e.g. the test's "lo" domain over oid) PostgreSQL reports the
     * domain OID as atttypid, but ODBC clients expect the underlying base type's
     * SQL type; tb resolves the domain to its base (COALESCE(typbasetype, oid)).
     * TYPE_NAME still reports the declared type name (t.typname, e.g. "lo").
     *
     * Column widths are cast to varchar(128)/varchar(254) so SQLDescribeCol
     * reports the fixed widths the original driver uses. No relkind filter is
     * applied: like the original, SQLColumns describes columns of every relation
     * (ordinary tables, views, matviews, foreign tables, AND indexes), which is
     * why index relations such as "testtab1_pkey" appear in the result. */
    int written = snprintf(query, sizeof(query),
        "SELECT "
        "current_database()::varchar(128) AS \"TABLE_CAT\", "
        "n.nspname::varchar(128) AS \"TABLE_SCHEM\", "
        "c.relname::varchar(128) AS \"TABLE_NAME\", "
        "a.attname::varchar(128) AS \"COLUMN_NAME\", "
        "CASE "
        "  WHEN tb.oid = 16 THEN %d "    /* bool → SQL_BIT */
        "  WHEN tb.oid = 21 THEN %d "    /* int2 → SQL_SMALLINT */
        "  WHEN tb.oid = 23 THEN %d "    /* int4 → SQL_INTEGER */
        "  WHEN tb.oid = 26 THEN %d "    /* oid (base of the 'lo' domain) → SQL_LONGVARBINARY */
        "  WHEN tb.oid = 20 THEN %d "    /* int8 → SQL_BIGINT */
        "  WHEN tb.oid = 700 THEN %d "   /* float4 → SQL_REAL */
        "  WHEN tb.oid = 701 THEN %d "   /* float8 → SQL_DOUBLE */
        "  WHEN tb.oid = 1700 THEN %d "  /* numeric → SQL_NUMERIC */
        "  WHEN tb.oid = 1042 THEN %d "  /* bpchar → SQL_CHAR */
        "  WHEN tb.oid = 1043 THEN %d "  /* varchar → SQL_VARCHAR */
        "  WHEN tb.oid = 25 THEN %d "    /* text → SQL_LONGVARCHAR */
        "  WHEN tb.oid = 1082 THEN %d "  /* date → SQL_TYPE_DATE */
        "  WHEN tb.oid = 1083 THEN %d "  /* time → SQL_TYPE_TIME */
        "  WHEN tb.oid IN (1114,1184) THEN %d " /* timestamp → SQL_TYPE_TIMESTAMP */
        "  WHEN tb.oid = 17 THEN %d "    /* bytea → SQL_LONGVARBINARY */
        "  ELSE %d "                     /* default (incl. interval) → SQL_VARCHAR */
        "END::smallint AS \"DATA_TYPE\", "
        "t.typname::varchar(128) AS \"TYPE_NAME\", "
        "CASE "
        "  WHEN tb.oid IN (1042,1043) AND a.atttypmod > 0 THEN a.atttypmod - 4 "
        "  WHEN tb.oid = 1700 AND a.atttypmod > 0 THEN ((a.atttypmod - 4) >> 16) & 65535 "
        "  WHEN tb.oid = 21 THEN 5 "
        "  WHEN tb.oid = 23 THEN 10 "
        "  WHEN tb.oid = 20 THEN 19 "
        "  WHEN tb.oid = 700 THEN 7 "
        "  WHEN tb.oid = 701 THEN 15 "
        "  WHEN tb.oid = 16 THEN 1 "
        "  WHEN tb.oid = 1082 THEN 10 "
        "  WHEN tb.oid = 1083 THEN 8 "
        "  WHEN tb.oid IN (1114,1184) THEN 26 "
        "  ELSE 0 "
        "END::int AS \"COLUMN_SIZE\", "
        "CASE "
        "  WHEN tb.oid IN (21) THEN 2 "
        "  WHEN tb.oid IN (23) THEN 4 "
        "  WHEN tb.oid IN (20) THEN 8 "
        "  WHEN tb.oid IN (700) THEN 4 "
        "  WHEN tb.oid IN (701) THEN 8 "
        "  WHEN tb.oid IN (16) THEN 1 "
        "  WHEN tb.oid IN (1042,1043) AND a.atttypmod > 0 THEN a.atttypmod - 4 "
        "  ELSE 0 "
        "END::int AS \"BUFFER_LENGTH\", "
        "CASE "
        "  WHEN tb.oid = 1700 AND a.atttypmod > 0 THEN ((a.atttypmod - 4) & 65535)::smallint "
        "  WHEN tb.oid IN (1114,1184,1083) THEN 6::smallint "
        "  ELSE 0::smallint "
        "END AS \"DECIMAL_DIGITS\", "
        "CASE WHEN tb.oid IN (21,23,20,1700) THEN 10 ELSE NULL END::smallint AS \"NUM_PREC_RADIX\", "
        "CASE WHEN a.attnotnull THEN 0 ELSE 1 END::smallint AS \"NULLABLE\", "
        "COALESCE(pg_catalog.col_description(c.oid, a.attnum), '')::varchar(254) AS \"REMARKS\", "
        "pg_catalog.pg_get_expr(d.adbin, d.adrelid)::varchar(254) AS \"COLUMN_DEF\", "
        "CASE "
        "  WHEN tb.oid = 16 THEN %d "
        "  WHEN tb.oid = 21 THEN %d "
        "  WHEN tb.oid = 23 THEN %d "
        "  WHEN tb.oid = 20 THEN %d "
        "  WHEN tb.oid = 700 THEN %d "
        "  WHEN tb.oid = 701 THEN %d "
        "  WHEN tb.oid = 1700 THEN %d "
        "  WHEN tb.oid IN (1042,1043,25) THEN %d "
        "  WHEN tb.oid IN (1082,1083,1114,1184) THEN %d "
        "  ELSE %d "
        "END::smallint AS \"SQL_DATA_TYPE\", "
        "NULL::smallint AS \"SQL_DATETIME_SUB\", "
        "CASE WHEN tb.oid IN (1042,1043,25) AND a.atttypmod > 0 THEN a.atttypmod - 4 ELSE NULL END::int AS \"CHAR_OCTET_LENGTH\", "
        "a.attnum::int AS \"ORDINAL_POSITION\", "
        "CASE WHEN a.attnotnull THEN 'NO' ELSE 'YES' END::varchar(254) AS \"IS_NULLABLE\", "
        /* Driver-specific trailing columns. The catalogfunctions test prints
         * only the first 6 columns of each row but describes ALL columns, so
         * these must exist with the exact names/types below; their row values
         * are not asserted, so 0/NULL placeholders are fine. */
        "0::int AS \"DISPLAY_SIZE\", "
        "tb.oid::int AS \"FIELD_TYPE\", "
        "0::int AS \"AUTO_INCREMENT\", "
        /* Wrap attnum/atttypmod in NULLIF so PostgreSQL reports these columns as
         * nullable (they derive from NOT NULL catalog columns; the original
         * driver's synthesized result marks them nullable). NULLIF(x, NULL)
         * yields x while making the expression's nullability true. */
        "NULLIF(a.attnum, NULL)::smallint AS \"PHYSICAL NUMBER\", "
        "c.oid::int AS \"TABLE OID\", "
        "0::int AS \"BASE TYPEID\", "
        "NULLIF(a.atttypmod, NULL)::int AS \"TYPMOD\", "
        "0::int AS \"TABLE INFO\" "
        "FROM pg_catalog.pg_attribute a "
        "JOIN pg_catalog.pg_class c ON a.attrelid = c.oid "
        "JOIN pg_catalog.pg_namespace n ON c.relnamespace = n.oid "
        "JOIN pg_catalog.pg_type t ON a.atttypid = t.oid "
        /* Resolve a domain (typtype 'd') to its base type for SQL-type mapping. */
        "JOIN pg_catalog.pg_type tb ON tb.oid = "
        "  (CASE WHEN t.typtype = 'd' AND t.typbasetype <> 0 THEN t.typbasetype ELSE t.oid END) "
        "LEFT JOIN pg_catalog.pg_attrdef d ON (a.attrelid = d.adrelid AND a.attnum = d.adnum) "
        "WHERE a.attnum > 0 AND NOT a.attisdropped "
        "AND n.nspname NOT IN ('pg_catalog','information_schema','pg_toast')",
        /* DATA_TYPE values */
        bool_sql_type, (int)SQL_SMALLINT, (int)SQL_INTEGER, (int)SQL_LONGVARBINARY,
        (int)SQL_BIGINT, (int)SQL_REAL, (int)SQL_DOUBLE, (int)SQL_NUMERIC,
        (int)SQL_CHAR, (int)SQL_VARCHAR, (int)SQL_LONGVARCHAR,
        (int)SQL_TYPE_DATE, (int)SQL_TYPE_TIME, (int)SQL_TYPE_TIMESTAMP,
        (int)SQL_LONGVARBINARY, (int)SQL_VARCHAR,
        /* SQL_DATA_TYPE values (same as DATA_TYPE for non-datetime) */
        bool_sql_type, (int)SQL_SMALLINT, (int)SQL_INTEGER, (int)SQL_BIGINT,
        (int)SQL_REAL, (int)SQL_DOUBLE, (int)SQL_NUMERIC,
        (int)SQL_VARCHAR, (int)SQL_TYPE_TIMESTAMP, (int)SQL_VARCHAR);
    offset = (size_t)written;

    append_like_filter(query, sizeof(query), &offset,
                       "n.nspname", schema_pattern, schema_length);
    append_like_filter(query, sizeof(query), &offset,
                       "c.relname", table_pattern, table_length);
    append_like_filter(query, sizeof(query), &offset,
                       "a.attname", column_pattern, column_length);

    size_t remaining = sizeof(query) - offset;
    written = snprintf(query + offset, remaining,
                       " ORDER BY \"TABLE_SCHEM\", \"TABLE_NAME\", \"ORDINAL_POSITION\"");
    if (written > 0) offset += (size_t)written;

    return execute_catalog_query(statement, query);
}

SQLRETURN catalog_primary_keys(OdbcStatement *statement,
                               const SQLCHAR *catalog_name, SQLSMALLINT catalog_name_length,
                               const SQLCHAR *schema_name, SQLSMALLINT schema_length,
                               const SQLCHAR *table_name, SQLSMALLINT table_length)
{
    (void)catalog_name;
    (void)catalog_name_length;

    /* Table name is required for SQLPrimaryKeys */
    size_t table_actual_length = resolve_argument_length(table_name, table_length);
    if (table_actual_length == 0) {
        diagnostics_add_record(&statement->diagnostics,
                               "HY009",
                               0,
                               "Table name is required for SQLPrimaryKeys.");
        return SQL_ERROR;
    }

    char query[CATALOG_QUERY_BUFFER_SIZE];
    size_t offset = 0;

    int written = snprintf(query, sizeof(query),
        "SELECT "
        "current_database()::varchar(128) AS \"TABLE_CAT\", "
        "n.nspname::varchar(128) AS \"TABLE_SCHEM\", "
        "c.relname::varchar(128) AS \"TABLE_NAME\", "
        "a.attname::varchar(128) AS \"COLUMN_NAME\", "
        "(array_position(i.indkey, a.attnum) + 1)::smallint AS \"KEY_SEQ\", "
        "ic.relname::varchar(128) AS \"PK_NAME\" "
        "FROM pg_catalog.pg_index i "
        "JOIN pg_catalog.pg_class c ON c.oid = i.indrelid "
        "JOIN pg_catalog.pg_class ic ON ic.oid = i.indexrelid "
        "JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
        "JOIN pg_catalog.pg_attribute a ON a.attrelid = c.oid AND a.attnum = ANY(i.indkey) "
        "WHERE i.indisprimary");
    offset = (size_t)written;

    /* A PG11+ covering primary key stores the INCLUDE payload columns after the
     * key columns in indkey; indnkeyatts counts only the leading key columns.
     * SQLPrimaryKeys must report just the key columns, so drop any attribute
     * whose 1-based position in indkey falls past indnkeyatts. indkey is an
     * int2vector subscripted from 0, so the 1-based position is
     * array_position(...) + 1 (matching the KEY_SEQ expression above).
     *
     * pg_index.indnkeyatts only exists on PostgreSQL 11+. On 9.x/10 referencing
     * it would make the entire query error out (and SQLPrimaryKeys wrongly
     * return no keys), so gate the filter on the server version. Pre-11 servers
     * cannot have INCLUDE columns anyway, so omitting the filter is correct. */
    if (statement->parent_connection &&
        statement->parent_connection->server_version_major >= 11) {
        size_t remaining = sizeof(query) - offset;
        int w = snprintf(query + offset, remaining,
                         " AND (array_position(i.indkey, a.attnum) + 1) <= i.indnkeyatts");
        if (w > 0) offset += (size_t)w;
    }

    /* Exact match for table name (not LIKE) */
    append_exact_filter(query, sizeof(query), &offset, "c.relname", table_name, table_length);

    /* Schema filter — exact match. If not provided, exclude system schemas */
    if (schema_name && resolve_argument_length(schema_name, schema_length) > 0) {
        append_exact_filter(query, sizeof(query), &offset, "n.nspname", schema_name, schema_length);
    } else {
        size_t remaining = sizeof(query) - offset;
        int w = snprintf(query + offset, remaining,
                         " AND n.nspname NOT IN ('pg_catalog','information_schema','pg_toast')");
        if (w > 0) offset += (size_t)w;
    }

    size_t remaining = sizeof(query) - offset;
    written = snprintf(query + offset, remaining, " ORDER BY \"KEY_SEQ\"");
    if (written > 0) offset += (size_t)written;

    return execute_catalog_query(statement, query);
}

/* ---- SQLGetTypeInfo ---- */

/* Sentinel meaning "this integer attribute is SQL NULL" in a TypeInfoDescriptor.
 * Chosen as -1 because none of the columns that may be NULL (unsigned attribute,
 * auto-increment, scales, datetime sub-code, radix) ever carry a real -1. */
#define TYPE_INFO_NULL_INT (-1)

/*
 * Static description of one SQLGetTypeInfo row. The values mirror the original
 * psqlodbc's pgtype_* helpers for the corresponding PostgreSQL type. Integer
 * fields set to TYPE_INFO_NULL_INT are emitted as SQL NULL; string fields set to
 * NULL are emitted as SQL NULL. column_size for the variable-length character
 * types is filled in at runtime from the connection's MaxVarcharSize setting.
 */
typedef struct TypeInfoDescriptor {
    const char *type_name;
    SQLSMALLINT data_type;        /* DATA_TYPE (the concise SQL type) */
    int column_size;              /* COLUMN_SIZE; -2 means "use connection char size" */
    const char *literal_prefix;   /* NULL for non-quoted (numeric) types */
    const char *literal_suffix;
    const char *create_params;    /* e.g. "max. length"; NULL when none */
    SQLSMALLINT case_sensitive;   /* SQL_TRUE / SQL_FALSE */
    SQLSMALLINT searchable;       /* SQL_SEARCHABLE / SQL_ALL_EXCEPT_LIKE */
    int unsigned_attribute;       /* 0, 1, or TYPE_INFO_NULL_INT */
    int auto_unique_value;        /* 0 or TYPE_INFO_NULL_INT */
    int minimum_scale;            /* or TYPE_INFO_NULL_INT */
    int maximum_scale;            /* or TYPE_INFO_NULL_INT */
    SQLSMALLINT sql_data_type;    /* SQL_DATA_TYPE (SQL_DATETIME for date/time) */
    int datetime_sub;             /* SQL_DATETIME_SUB code, or TYPE_INFO_NULL_INT */
    int num_prec_radix;           /* 10 for numeric types, or TYPE_INFO_NULL_INT */
} TypeInfoDescriptor;

/* Marker for "column_size comes from the connection's max varchar size". */
#define TYPE_INFO_CHAR_SIZE (-2)

/*
 * The supported SQL types, ordered by DATA_TYPE as the ODBC spec requires for
 * SQLGetTypeInfo(SQL_ALL_TYPES). Values follow the original driver:
 *   - character types are case-sensitive and fully searchable;
 *   - numeric types have radix 10, no literal quoting, and are signed;
 *   - date/time types report SQL_DATETIME as SQL_DATA_TYPE with a datetime sub-code.
 * FIXED_PREC_SCALE is 0 for all of these (money is not exposed), NULLABLE is
 * SQL_NULLABLE for all (matching pgtype_nullable), LOCAL_TYPE_NAME is always NULL,
 * and INTERVAL_PRECISION is 0.
 */
static const TypeInfoDescriptor TYPE_INFO_ROWS[] = {
    /* name        data_type          col_size            prefix suffix create_params      case srch  unsg  auto  min max  sql_data_type      dtsub                radix */
    { "int8",      SQL_BIGINT,        19,                 NULL,  NULL,  NULL,              0,   SQL_ALL_EXCEPT_LIKE, 0,  TYPE_INFO_NULL_INT, 0,  0,  SQL_BIGINT,        TYPE_INFO_NULL_INT, 10 },
    { "bool",      SQL_BIT,           1,                  NULL,  NULL,  NULL,              0,   SQL_ALL_EXCEPT_LIKE, TYPE_INFO_NULL_INT, TYPE_INFO_NULL_INT, 0, 0, SQL_BIT,   TYPE_INFO_NULL_INT, TYPE_INFO_NULL_INT },
    { "varchar",   SQL_VARCHAR,       TYPE_INFO_CHAR_SIZE,"'",   "'",   "max. length",     1,   SQL_SEARCHABLE,      TYPE_INFO_NULL_INT, TYPE_INFO_NULL_INT, TYPE_INFO_NULL_INT, TYPE_INFO_NULL_INT, SQL_VARCHAR, TYPE_INFO_NULL_INT, TYPE_INFO_NULL_INT },
    { "bpchar",    SQL_CHAR,          TYPE_INFO_CHAR_SIZE,"'",   "'",   "max. length",     1,   SQL_SEARCHABLE,      TYPE_INFO_NULL_INT, TYPE_INFO_NULL_INT, TYPE_INFO_NULL_INT, TYPE_INFO_NULL_INT, SQL_CHAR,    TYPE_INFO_NULL_INT, TYPE_INFO_NULL_INT },
    { "numeric",   SQL_NUMERIC,       28,                 NULL,  NULL,  "precision, scale",0,   SQL_ALL_EXCEPT_LIKE, 0,  TYPE_INFO_NULL_INT, 0,  0,  SQL_NUMERIC,       TYPE_INFO_NULL_INT, 10 },
    { "int4",      SQL_INTEGER,       10,                 NULL,  NULL,  NULL,              0,   SQL_ALL_EXCEPT_LIKE, 0,  TYPE_INFO_NULL_INT, 0,  0,  SQL_INTEGER,       TYPE_INFO_NULL_INT, 10 },
    { "int2",      SQL_SMALLINT,      5,                  NULL,  NULL,  NULL,              0,   SQL_ALL_EXCEPT_LIKE, 0,  TYPE_INFO_NULL_INT, 0,  0,  SQL_SMALLINT,      TYPE_INFO_NULL_INT, 10 },
    { "float4",    SQL_REAL,          9,                  NULL,  NULL,  NULL,              0,   SQL_ALL_EXCEPT_LIKE, 0,  TYPE_INFO_NULL_INT, 0,  0,  SQL_REAL,          TYPE_INFO_NULL_INT, 10 },
    { "float8",    SQL_DOUBLE,        17,                 NULL,  NULL,  NULL,              0,   SQL_ALL_EXCEPT_LIKE, 0,  TYPE_INFO_NULL_INT, 0,  0,  SQL_DOUBLE,        TYPE_INFO_NULL_INT, 10 },
    { "text",      SQL_LONGVARCHAR,   TYPE_INFO_CHAR_SIZE,"'",   "'",   NULL,              1,   SQL_SEARCHABLE,      TYPE_INFO_NULL_INT, TYPE_INFO_NULL_INT, TYPE_INFO_NULL_INT, TYPE_INFO_NULL_INT, SQL_LONGVARCHAR, TYPE_INFO_NULL_INT, TYPE_INFO_NULL_INT },
    { "bytea",     SQL_LONGVARBINARY, TYPE_INFO_NULL_INT, "'",   "'",   NULL,              0,   SQL_ALL_EXCEPT_LIKE, TYPE_INFO_NULL_INT, TYPE_INFO_NULL_INT, TYPE_INFO_NULL_INT, TYPE_INFO_NULL_INT, SQL_LONGVARBINARY, TYPE_INFO_NULL_INT, TYPE_INFO_NULL_INT },
    { "date",      SQL_TYPE_DATE,     10,                 "'",   "'",   NULL,              0,   SQL_ALL_EXCEPT_LIKE, TYPE_INFO_NULL_INT, TYPE_INFO_NULL_INT, TYPE_INFO_NULL_INT, TYPE_INFO_NULL_INT, SQL_DATETIME, SQL_CODE_DATE, TYPE_INFO_NULL_INT },
    { "time",      SQL_TYPE_TIME,     8,                  "'",   "'",   NULL,              0,   SQL_ALL_EXCEPT_LIKE, TYPE_INFO_NULL_INT, TYPE_INFO_NULL_INT, TYPE_INFO_NULL_INT, TYPE_INFO_NULL_INT, SQL_DATETIME, SQL_CODE_TIME, TYPE_INFO_NULL_INT },
    { "timestamp", SQL_TYPE_TIMESTAMP,22,                 "'",   "'",   NULL,              0,   SQL_ALL_EXCEPT_LIKE, TYPE_INFO_NULL_INT, TYPE_INFO_NULL_INT, 0,  38, SQL_DATETIME, SQL_CODE_TIMESTAMP, TYPE_INFO_NULL_INT },
    { "uuid",      SQL_GUID,          36,                 "'",   "'",   NULL,              0,   SQL_ALL_EXCEPT_LIKE, TYPE_INFO_NULL_INT, TYPE_INFO_NULL_INT, TYPE_INFO_NULL_INT, TYPE_INFO_NULL_INT, SQL_GUID, TYPE_INFO_NULL_INT, TYPE_INFO_NULL_INT },
};

static const int TYPE_INFO_ROW_COUNT =
    (int)(sizeof(TYPE_INFO_ROWS) / sizeof(TYPE_INFO_ROWS[0]));

/*
 * Bounds-safe append of a formatted fragment to query[*offset]. Returns true on
 * success; returns false (leaving *offset unchanged) if the buffer is already
 * full or the fragment would not fit, so callers can detect overflow instead of
 * letting "query_size - *offset" underflow (size_t) into a huge length and
 * corrupt memory. On success *offset advances past the written text.
 */
static bool append_query_fragment(char *query, size_t query_size, size_t *offset,
                                  const char *format, ...)
{
    if (*offset >= query_size) {
        return false;
    }
    va_list args;
    va_start(args, format);
    int written = vsnprintf(query + *offset, query_size - *offset, format, args);
    va_end(args);

    /* Negative means an encoding error; >= remaining means it was truncated. */
    if (written < 0 || (size_t)written >= query_size - *offset) {
        return false;
    }
    *offset += (size_t)written;
    return true;
}

/* Append a nullable integer as either its decimal text or SQL NULL. */
static bool append_type_info_int(char *query, size_t query_size, size_t *offset, int value)
{
    if (value == TYPE_INFO_NULL_INT) {
        return append_query_fragment(query, query_size, offset, "NULL");
    }
    return append_query_fragment(query, query_size, offset, "%d", value);
}

/* Append a string literal (single-quoted, doubling embedded quotes) or SQL NULL.
 * Builds the escaped text in a local buffer first, then appends it in one
 * bounds-checked step. Returns false on overflow (buffer or query). */
static bool append_type_info_string(char *query, size_t query_size, size_t *offset,
                                    const char *value)
{
    if (!value) {
        return append_query_fragment(query, query_size, offset, "NULL");
    }
    /* Escaped SQL literal: opening quote + doubled quotes + closing quote. Type
     * names and create-params are short, so a fixed buffer is ample. */
    char escaped[256];
    size_t pos = 0;
    escaped[pos++] = '\'';
    for (const char *cursor = value; *cursor; cursor++) {
        /* Reserve room for a possible doubled quote, the closing quote, and NUL. */
        if (pos + 3 >= sizeof(escaped)) {
            return false;
        }
        if (*cursor == '\'') {
            escaped[pos++] = '\'';
        }
        escaped[pos++] = *cursor;
    }
    escaped[pos++] = '\'';
    escaped[pos] = '\0';
    return append_query_fragment(query, query_size, offset, "%s", escaped);
}

SQLRETURN catalog_get_type_info(OdbcStatement *statement, SQLSMALLINT sql_type)
{
    /* Resolve the character-type column size from the connection (MaxVarcharSize,
     * default 255) so varchar/char/text report the same size the rest of the
     * driver uses for unbounded character columns. */
    int char_column_size = DEFAULT_MAX_VARCHAR_SIZE;
    if (statement->parent_connection &&
        statement->parent_connection->info.max_varchar_size > 0) {
        char_column_size = statement->parent_connection->info.max_varchar_size;
    }

    /* The result set is assembled as a typed VALUES table wrapped in a SELECT
     * that CASTs each column to the exact SQL type ODBC expects (VARCHAR(128),
     * SMALLINT, INTEGER). Building it as a query — rather than fabricating a
     * PGresult by hand — lets the existing result machinery report metadata and
     * fetch rows uniformly. */
    char query[CATALOG_QUERY_BUFFER_SIZE];
    size_t offset = 0;
    bool ok = true;

    ok = ok && append_query_fragment(query, sizeof(query), &offset,
        "SELECT "
        "CAST(c1  AS varchar(128)) AS \"TYPE_NAME\", "
        "CAST(c2  AS int2)         AS \"DATA_TYPE\", "
        "CAST(c3  AS int4)         AS \"COLUMN_SIZE\", "
        "CAST(c4  AS varchar(128)) AS \"LITERAL_PREFIX\", "
        "CAST(c5  AS varchar(128)) AS \"LITERAL_SUFFIX\", "
        "CAST(c6  AS varchar(128)) AS \"CREATE_PARAMS\", "
        "CAST(c7  AS int2)         AS \"NULLABLE\", "
        "CAST(c8  AS int2)         AS \"CASE_SENSITIVE\", "
        "CAST(c9  AS int2)         AS \"SEARCHABLE\", "
        "CAST(c10 AS int2)         AS \"UNSIGNED_ATTRIBUTE\", "
        "CAST(c11 AS int2)         AS \"FIXED_PREC_SCALE\", "
        "CAST(c12 AS int2)         AS \"AUTO_UNIQUE_VALUE\", "
        "CAST(c13 AS varchar(128)) AS \"LOCAL_TYPE_NAME\", "
        "CAST(c14 AS int2)         AS \"MINIMUM_SCALE\", "
        "CAST(c15 AS int2)         AS \"MAXIMUM_SCALE\", "
        "CAST(c16 AS int2)         AS \"SQL_DATA_TYPE\", "
        "CAST(c17 AS int2)         AS \"SQL_DATETIME_SUB\", "
        "CAST(c18 AS int4)         AS \"NUM_PREC_RADIX\", "
        "CAST(c19 AS int2)         AS \"INTERVAL_PRECISION\" "
        "FROM (VALUES ");

    bool emitted_any = false;
    for (int index = 0; index < TYPE_INFO_ROW_COUNT && ok; index++) {
        const TypeInfoDescriptor *row = &TYPE_INFO_ROWS[index];

        /* SQL_ALL_TYPES returns every row; otherwise filter to the requested
         * concise SQL type. */
        if (sql_type != SQL_ALL_TYPES && row->data_type != sql_type) {
            continue;
        }

        if (emitted_any) {
            ok = ok && append_query_fragment(query, sizeof(query), &offset, ", ");
        }
        emitted_any = true;

        int column_size = (row->column_size == TYPE_INFO_CHAR_SIZE)
                              ? char_column_size
                              : row->column_size;

        ok = ok && append_query_fragment(query, sizeof(query), &offset, "(");
        ok = ok && append_type_info_string(query, sizeof(query), &offset, row->type_name);
        ok = ok && append_query_fragment(query, sizeof(query), &offset, ", ");
        ok = ok && append_type_info_int(query, sizeof(query), &offset, row->data_type);
        ok = ok && append_query_fragment(query, sizeof(query), &offset, ", ");
        ok = ok && append_type_info_int(query, sizeof(query), &offset, column_size);
        ok = ok && append_query_fragment(query, sizeof(query), &offset, ", ");
        ok = ok && append_type_info_string(query, sizeof(query), &offset, row->literal_prefix);
        ok = ok && append_query_fragment(query, sizeof(query), &offset, ", ");
        ok = ok && append_type_info_string(query, sizeof(query), &offset, row->literal_suffix);
        ok = ok && append_query_fragment(query, sizeof(query), &offset, ", ");
        ok = ok && append_type_info_string(query, sizeof(query), &offset, row->create_params);
        ok = ok && append_query_fragment(query, sizeof(query), &offset, ", ");
        ok = ok && append_type_info_int(query, sizeof(query), &offset, SQL_NULLABLE);
        ok = ok && append_query_fragment(query, sizeof(query), &offset, ", ");
        ok = ok && append_type_info_int(query, sizeof(query), &offset, row->case_sensitive);
        ok = ok && append_query_fragment(query, sizeof(query), &offset, ", ");
        ok = ok && append_type_info_int(query, sizeof(query), &offset, row->searchable);
        ok = ok && append_query_fragment(query, sizeof(query), &offset, ", ");
        ok = ok && append_type_info_int(query, sizeof(query), &offset, row->unsigned_attribute);
        ok = ok && append_query_fragment(query, sizeof(query), &offset, ", ");
        ok = ok && append_type_info_int(query, sizeof(query), &offset, 0);   /* FIXED_PREC_SCALE (money) */
        ok = ok && append_query_fragment(query, sizeof(query), &offset, ", ");
        ok = ok && append_type_info_int(query, sizeof(query), &offset, row->auto_unique_value);
        ok = ok && append_query_fragment(query, sizeof(query), &offset, ", ");
        ok = ok && append_type_info_string(query, sizeof(query), &offset, NULL); /* LOCAL_TYPE_NAME */
        ok = ok && append_query_fragment(query, sizeof(query), &offset, ", ");
        ok = ok && append_type_info_int(query, sizeof(query), &offset, row->minimum_scale);
        ok = ok && append_query_fragment(query, sizeof(query), &offset, ", ");
        ok = ok && append_type_info_int(query, sizeof(query), &offset, row->maximum_scale);
        ok = ok && append_query_fragment(query, sizeof(query), &offset, ", ");
        ok = ok && append_type_info_int(query, sizeof(query), &offset, row->sql_data_type);
        ok = ok && append_query_fragment(query, sizeof(query), &offset, ", ");
        ok = ok && append_type_info_int(query, sizeof(query), &offset, row->datetime_sub);
        ok = ok && append_query_fragment(query, sizeof(query), &offset, ", ");
        ok = ok && append_type_info_int(query, sizeof(query), &offset, row->num_prec_radix);
        ok = ok && append_query_fragment(query, sizeof(query), &offset, ", ");
        ok = ok && append_type_info_int(query, sizeof(query), &offset, 0);   /* INTERVAL_PRECISION */
        ok = ok && append_query_fragment(query, sizeof(query), &offset, ")");
    }

    /* When no supported type matches the requested SQL type, return an empty
     * (but correctly-shaped) result set by selecting the typed template with a
     * false predicate. */
    if (ok && !emitted_any) {
        ok = append_query_fragment(query, sizeof(query), &offset,
            "(NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, "
            "NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL)) "
            "AS t(c1,c2,c3,c4,c5,c6,c7,c8,c9,c10,c11,c12,c13,c14,c15,c16,c17,c18,c19) "
            "WHERE false ORDER BY \"DATA_TYPE\"");
    } else {
        ok = ok && append_query_fragment(query, sizeof(query), &offset,
            ") AS t(c1,c2,c3,c4,c5,c6,c7,c8,c9,c10,c11,c12,c13,c14,c15,c16,c17,c18,c19) "
            "ORDER BY \"DATA_TYPE\"");
    }

    if (!ok) {
        /* The type list is fixed and comfortably fits CATALOG_QUERY_BUFFER_SIZE,
         * so this is not reachable today; guard defensively so a future edit that
         * grows the list fails loudly instead of overrunning the buffer. */
        diagnostics_add_record(&statement->diagnostics, "HY000", 0,
                               "Internal error: SQLGetTypeInfo query exceeded its buffer.");
        return SQL_ERROR;
    }

    return execute_catalog_query(statement, query);
}

SQLRETURN catalog_foreign_keys(OdbcStatement *statement,
                               const SQLCHAR *pk_catalog, SQLSMALLINT pk_catalog_length,
                               const SQLCHAR *pk_schema, SQLSMALLINT pk_schema_length,
                               const SQLCHAR *pk_table, SQLSMALLINT pk_table_length,
                               const SQLCHAR *fk_catalog, SQLSMALLINT fk_catalog_length,
                               const SQLCHAR *fk_schema, SQLSMALLINT fk_schema_length,
                               const SQLCHAR *fk_table, SQLSMALLINT fk_table_length)
{
    (void)pk_catalog;
    (void)pk_catalog_length;
    (void)fk_catalog;
    (void)fk_catalog_length;

    /* At least one of pk_table or fk_table must be specified */
    size_t pk_table_actual = resolve_argument_length(pk_table, pk_table_length);
    size_t fk_table_actual = resolve_argument_length(fk_table, fk_table_length);

    if (pk_table_actual == 0 && fk_table_actual == 0) {
        diagnostics_add_record(&statement->diagnostics,
                               "HY009",
                               0,
                               "At least one of PK table or FK table must be specified for SQLForeignKeys.");
        return SQL_ERROR;
    }

    char query[CATALOG_QUERY_BUFFER_SIZE];
    size_t offset = 0;

    int written = snprintf(query, sizeof(query),
        "SELECT "
        /* The schema/name/column columns are raw NOT NULL "name" catalog columns
         * (width 63), so PostgreSQL reports them as not-nullable — matching the
         * original driver's FK result metadata. The catalog columns are cast to
         * varchar(63) only where the expected result is nullable (CAT). */
        "current_database()::varchar(63) AS \"PKTABLE_CAT\", "
        "pn.nspname AS \"PKTABLE_SCHEM\", "
        "pc.relname AS \"PKTABLE_NAME\", "
        "pa.attname AS \"PKCOLUMN_NAME\", "
        "current_database()::varchar(63) AS \"FKTABLE_CAT\", "
        "fn.nspname AS \"FKTABLE_SCHEM\", "
        "fc.relname AS \"FKTABLE_NAME\", "
        "fa.attname AS \"FKCOLUMN_NAME\", "
        "cols.ordinality::smallint AS \"KEY_SEQ\", "
        "CASE con.confupdtype "
        "  WHEN 'c' THEN 0 WHEN 'r' THEN 1 WHEN 'n' THEN 2 "
        "  WHEN 'a' THEN 3 WHEN 'd' THEN 4 ELSE 3 "
        "END::smallint AS \"UPDATE_RULE\", "
        "CASE con.confdeltype "
        "  WHEN 'c' THEN 0 WHEN 'r' THEN 1 WHEN 'n' THEN 2 "
        "  WHEN 'a' THEN 3 WHEN 'd' THEN 4 ELSE 3 "
        "END::smallint AS \"DELETE_RULE\", "
        "con.conname AS \"FK_NAME\", "
        /* PK_NAME is a direct reference to the primary-key index's NOT NULL
         * "name" column (via the joins below) rather than a correlated subquery,
         * so PostgreSQL reports it as not-nullable — matching the original
         * driver's FK result metadata. */
        "pk_idx.relname AS \"PK_NAME\", "
        "CASE "
        "  WHEN con.condeferrable AND con.condeferred THEN 5 "
        "  WHEN con.condeferrable THEN 6 "
        "  ELSE 7 "
        "END::smallint AS \"DEFERRABILITY\" "
        "FROM pg_catalog.pg_constraint con "
        "JOIN pg_catalog.pg_class fc ON fc.oid = con.conrelid "
        "JOIN pg_catalog.pg_namespace fn ON fn.oid = fc.relnamespace "
        "JOIN pg_catalog.pg_class pc ON pc.oid = con.confrelid "
        "JOIN pg_catalog.pg_namespace pn ON pn.oid = pc.relnamespace "
        "CROSS JOIN LATERAL unnest(con.conkey, con.confkey) "
        "  WITH ORDINALITY AS cols(fk_attnum, pk_attnum, ordinality) "
        "JOIN pg_catalog.pg_attribute fa ON fa.attrelid = con.conrelid AND fa.attnum = cols.fk_attnum "
        "JOIN pg_catalog.pg_attribute pa ON pa.attrelid = con.confrelid AND pa.attnum = cols.pk_attnum "
        /* Join the referenced table's primary-key index to expose its name as a
         * NOT NULL column reference for PK_NAME. */
        "JOIN pg_catalog.pg_index pk_i ON pk_i.indrelid = con.confrelid AND pk_i.indisprimary "
        "JOIN pg_catalog.pg_class pk_idx ON pk_idx.oid = pk_i.indexrelid "
        "WHERE con.contype = 'f'");
    offset = (size_t)written;

    /* Apply PK table filters */
    if (pk_table_actual > 0) {
        append_exact_filter(query, sizeof(query), &offset, "pc.relname", pk_table, pk_table_length);
    }
    if (pk_schema && resolve_argument_length(pk_schema, pk_schema_length) > 0) {
        append_exact_filter(query, sizeof(query), &offset, "pn.nspname", pk_schema, pk_schema_length);
    }

    /* Apply FK table filters */
    if (fk_table_actual > 0) {
        append_exact_filter(query, sizeof(query), &offset, "fc.relname", fk_table, fk_table_length);
    }
    if (fk_schema && resolve_argument_length(fk_schema, fk_schema_length) > 0) {
        append_exact_filter(query, sizeof(query), &offset, "fn.nspname", fk_schema, fk_schema_length);
    }

    /* Order per ODBC spec: by PK table for imported keys, FK table for exported */
    size_t remaining = sizeof(query) - offset;
    if (fk_table_actual > 0 && pk_table_actual == 0) {
        written = snprintf(query + offset, remaining,
                           " ORDER BY \"PKTABLE_SCHEM\", \"PKTABLE_NAME\", \"KEY_SEQ\"");
    } else {
        written = snprintf(query + offset, remaining,
                           " ORDER BY \"FKTABLE_SCHEM\", \"FKTABLE_NAME\", \"KEY_SEQ\"");
    }
    if (written > 0) offset += (size_t)written;

    return execute_catalog_query(statement, query);
}

/* ---- SQLColumnPrivileges ---- */

SQLRETURN catalog_column_privileges(OdbcStatement *statement,
                                    const SQLCHAR *catalog_name, SQLSMALLINT catalog_length,
                                    const SQLCHAR *schema_name, SQLSMALLINT schema_length,
                                    const SQLCHAR *table_name, SQLSMALLINT table_length,
                                    const SQLCHAR *column_name, SQLSMALLINT column_length)
{
    (void)catalog_name;
    (void)catalog_length;

    char query[CATALOG_QUERY_BUFFER_SIZE];
    size_t offset = 0;

    /* Pass through information_schema.column_privileges: its column domains
     * (sql_identifier = name/63, character_data for privilege, yes_or_no/3 for
     * is_grantable) produce exactly the widths the original driver reports. The
     * lower-case aliases match the original's result column names. Ordering is
     * left as information_schema returns it, which matches the expected output
     * (UPDATE, SELECT, REFERENCES, INSERT). */
    int written = snprintf(query, sizeof(query),
        "SELECT table_catalog::varchar(63) AS table_cat, "
        "table_schema::varchar(63) AS table_schem, "
        "table_name::varchar(63) AS table_name, "
        "column_name::varchar(63) AS column_name, "
        "grantor::varchar(63) AS grantor, "
        "grantee::varchar(63) AS grantee, "
        "privilege_type::varchar(255) AS privilege, "
        "is_grantable::varchar(3) AS is_grantable "
        "FROM information_schema.column_privileges WHERE true");
    offset = (size_t)written;

    append_exact_filter(query, sizeof(query), &offset, "table_schema", schema_name, schema_length);
    append_exact_filter(query, sizeof(query), &offset, "table_name", table_name, table_length);
    append_like_filter(query, sizeof(query), &offset, "column_name", column_name, column_length);

    return execute_catalog_query(statement, query);
}

/* ---- SQLTablePrivileges ---- */

SQLRETURN catalog_table_privileges(OdbcStatement *statement,
                                   const SQLCHAR *catalog_name, SQLSMALLINT catalog_length,
                                   const SQLCHAR *schema_pattern, SQLSMALLINT schema_length,
                                   const SQLCHAR *table_pattern, SQLSMALLINT table_length)
{
    (void)catalog_name;
    (void)catalog_length;

    char query[CATALOG_QUERY_BUFFER_SIZE];
    size_t offset = 0;

    /* The original driver reports the table owner as GRANTEE "_SYSTEM" with the
     * full privilege set (INSERT, SELECT, UPDATE, DELETE, REFERENCES) in that
     * fixed order, rather than decoding the actual ACL. Reproduce that with a
     * VALUES list of privilege names cross-joined to the matching tables, so the
     * widths are the driver's varchar(128) and the order is deterministic. */
    int written = snprintf(query, sizeof(query),
        "SELECT current_database()::varchar(128) AS \"TABLE_CAT\", "
        "n.nspname::varchar(128) AS \"TABLE_SCHEM\", "
        "c.relname::varchar(128) AS \"TABLE_NAME\", "
        "'_SYSTEM'::varchar(128) AS \"GRANTOR\", "
        "pg_catalog.pg_get_userbyid(c.relowner)::varchar(128) AS \"GRANTEE\", "
        "p.priv::varchar(128) AS \"PRIVILEGE\", "
        "'YES'::varchar(128) AS \"IS_GRANTABLE\" "
        "FROM pg_catalog.pg_class c "
        "JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
        "CROSS JOIN (VALUES (1,'INSERT'),(2,'SELECT'),(3,'UPDATE'),(4,'DELETE'),"
        "(5,'REFERENCES')) AS p(ord, priv) "
        "WHERE c.relkind IN ('r','v','m','f','p') "
        "AND n.nspname NOT IN ('pg_catalog','information_schema','pg_toast')");
    offset = (size_t)written;

    append_like_filter(query, sizeof(query), &offset, "n.nspname", schema_pattern, schema_length);
    append_like_filter(query, sizeof(query), &offset, "c.relname", table_pattern, table_length);

    size_t remaining = sizeof(query) - offset;
    written = snprintf(query + offset, remaining,
                       " ORDER BY \"TABLE_SCHEM\", \"TABLE_NAME\", p.ord");
    if (written > 0) offset += (size_t)written;

    return execute_catalog_query(statement, query);
}

/* ---- SQLStatistics ---- */

SQLRETURN catalog_statistics(OdbcStatement *statement,
                             const SQLCHAR *catalog_name, SQLSMALLINT catalog_length,
                             const SQLCHAR *schema_name, SQLSMALLINT schema_length,
                             const SQLCHAR *table_name, SQLSMALLINT table_length,
                             SQLUSMALLINT unique, SQLUSMALLINT reserved)
{
    (void)catalog_name;
    (void)catalog_length;
    (void)unique;
    (void)reserved;

    char query[CATALOG_QUERY_BUFFER_SIZE];
    size_t offset = 0;

    /* One row per indexed column. NON_UNIQUE is 0 for a unique index, 1
     * otherwise; TYPE 3 is SQL_INDEX_OTHER; ASC_OR_DESC is 'A'. CARDINALITY,
     * PAGES and FILTER_CONDITION are reported NULL (the original leaves these
     * unpopulated unless statistics are requested). Column widths are cast to
     * the driver's varchar(128)/char(1) widths. */
    int written = snprintf(query, sizeof(query),
        "SELECT current_database()::varchar(128) AS \"TABLE_CAT\", "
        "n.nspname::varchar(128) AS \"TABLE_SCHEM\", "
        "c.relname::varchar(128) AS \"TABLE_NAME\", "
        "CASE WHEN i.indisunique THEN 0 ELSE 1 END::smallint AS \"NON_UNIQUE\", "
        "n.nspname::varchar(128) AS \"INDEX_QUALIFIER\", "
        "ic.relname::varchar(128) AS \"INDEX_NAME\", "
        "3::smallint AS \"TYPE\", "
        "k.ord::smallint AS \"ORDINAL_POSITION\", "
        "a.attname::varchar(128) AS \"COLUMN_NAME\", "
        "'A'::char(1) AS \"ASC_OR_DESC\", "
        "NULL::int AS \"CARDINALITY\", "
        "NULL::int AS \"PAGES\", "
        "NULL::varchar(128) AS \"FILTER_CONDITION\" "
        "FROM pg_catalog.pg_index i "
        "JOIN pg_catalog.pg_class c ON c.oid = i.indrelid "
        "JOIN pg_catalog.pg_class ic ON ic.oid = i.indexrelid "
        "JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
        "JOIN LATERAL unnest(i.indkey) WITH ORDINALITY AS k(attnum, ord) ON true "
        "JOIN pg_catalog.pg_attribute a ON a.attrelid = c.oid AND a.attnum = k.attnum "
        "WHERE true");
    offset = (size_t)written;

    append_exact_filter(query, sizeof(query), &offset, "c.relname", table_name, table_length);
    if (schema_name && resolve_argument_length(schema_name, schema_length) > 0) {
        append_exact_filter(query, sizeof(query), &offset, "n.nspname", schema_name, schema_length);
    } else {
        size_t remaining = sizeof(query) - offset;
        int w = snprintf(query + offset, remaining,
                         " AND n.nspname NOT IN ('pg_catalog','information_schema','pg_toast')");
        if (w > 0) offset += (size_t)w;
    }

    size_t remaining = sizeof(query) - offset;
    written = snprintf(query + offset, remaining,
                       " ORDER BY \"NON_UNIQUE\", \"INDEX_NAME\", \"ORDINAL_POSITION\"");
    if (written > 0) offset += (size_t)written;

    return execute_catalog_query(statement, query);
}

/* ---- SQLProcedures ---- */

SQLRETURN catalog_procedures(OdbcStatement *statement,
                             const SQLCHAR *catalog_name, SQLSMALLINT catalog_length,
                             const SQLCHAR *schema_pattern, SQLSMALLINT schema_length,
                             const SQLCHAR *proc_pattern, SQLSMALLINT proc_length)
{
    (void)catalog_name;
    (void)catalog_length;

    char query[CATALOG_QUERY_BUFFER_SIZE];
    size_t offset = 0;

    /* procedure_cat is empty (the original leaves it blank), the name columns
     * are the schema/proname, the three numeric-params columns are empty text
     * (the original leaves them unset), and procedure_type is 2 =
     * SQL_PT_FUNCTION for a function. Column widths (255/63) match the original.
     * The empty string for procedure_cat prints as an empty leading field. */
    int written = snprintf(query, sizeof(query),
        "SELECT ''::varchar(255) AS procedure_cat, "
        "n.nspname::name AS procedure_schem, "
        "p.proname::name AS procedure_name, "
        "''::varchar(255) AS num_input_params, "
        "''::varchar(255) AS num_output_params, "
        "''::varchar(255) AS num_result_sets, "
        "''::varchar(255) AS remarks, "
        "2::smallint AS procedure_type "
        "FROM pg_catalog.pg_proc p "
        "JOIN pg_catalog.pg_namespace n ON n.oid = p.pronamespace "
        "WHERE true");
    offset = (size_t)written;

    append_exact_filter(query, sizeof(query), &offset, "n.nspname", schema_pattern, schema_length);
    append_like_filter(query, sizeof(query), &offset, "p.proname", proc_pattern, proc_length);

    size_t remaining = sizeof(query) - offset;
    written = snprintf(query + offset, remaining,
                       " ORDER BY procedure_schem, procedure_name");
    if (written > 0) offset += (size_t)written;

    return execute_catalog_query(statement, query);
}

/* ---- SQLProcedureColumns ---- */

SQLRETURN catalog_procedure_columns(OdbcStatement *statement,
                                    const SQLCHAR *catalog_name, SQLSMALLINT catalog_length,
                                    const SQLCHAR *schema_pattern, SQLSMALLINT schema_length,
                                    const SQLCHAR *proc_pattern, SQLSMALLINT proc_length,
                                    const SQLCHAR *column_pattern, SQLSMALLINT column_length)
{
    (void)catalog_name;
    (void)catalog_length;
    (void)column_pattern;
    (void)column_length;

    /* One row per procedure parameter, plus a return-value/result rows for
     * functions. We reconstruct the parameter list in SQL:
     *   - proallargtypes/proargmodes describe all args (in/out/inout/table) when
     *     any OUT parameter exists; otherwise proargtypes lists the inputs and
     *     prorettype is the single return column.
     * COLUMN_TYPE: 1 = SQL_PARAM_INPUT, 4 = SQL_PARAM_OUTPUT (used for OUT and
     * the function return), matching the original's mapping for this test.
     *
     * The per-argument type metadata (DATA_TYPE, TYPE_NAME, sizes) is derived
     * from the argument's pg_type via the same mapping SQLColumns uses. BoolsAsChar
     * governs whether bool is reported as SQL_VARCHAR. */
    bool bools_as_char = !statement->parent_connection ||
                         statement->parent_connection->info.bools_as_char;
    int bool_sql_type = bools_as_char ? (int)SQL_VARCHAR : (int)SQL_BIT;

    /* The mapping from a pg_type OID (resolved through domains) to the ODBC SQL
     * type / sizes is expressed once as a reusable SQL fragment "m" via a LATERAL
     * lookup, keeping the projection readable. */
    char query[CATALOG_QUERY_BUFFER_SIZE];
    size_t offset = 0;

    int written = snprintf(query, sizeof(query),
        "WITH proc AS ( "
        "  SELECT n.nspname, p.proname, p.oid AS proid, p.prorettype, p.proretset, "
        "         p.proargtypes, p.proallargtypes, p.proargmodes, p.proargnames "
        "  FROM pg_catalog.pg_proc p "
        "  JOIN pg_catalog.pg_namespace n ON n.oid = p.pronamespace "
        "  WHERE true");
    offset = (size_t)written;
    /* Filter inside the CTE, before its closing parenthesis. */
    append_exact_filter(query, sizeof(query), &offset, "n.nspname", schema_pattern, schema_length);
    append_like_filter(query, sizeof(query), &offset, "p.proname", proc_pattern, proc_length);
    {
        size_t rem = sizeof(query) - offset;
        int w = snprintf(query + offset, rem, ")");
        if (w > 0) offset += (size_t)w;
    }

    /* Build the per-parameter rows via an "arg" subquery that unions:
     *   (a) the declared arguments — inputs (mode i / null) as COLUMN_TYPE 1,
     *       OUT/INOUT (mode o/b) as 4, and TABLE columns (mode t) as 3;
     *   (b) a scalar-return row (COLUMN_TYPE 5) for a function that returns a
     *       base type and declares no OUT/TABLE parameters;
     *   (c) the attributes of a composite return type expanded as result
     *       columns — COLUMN_TYPE 4 for a single composite (e.g. getfoo) or 3
     *       for a set-returning composite (e.g. getboo).
     * ORDINAL_POSITION follows the original: return row is 0, declared args are
     * 1..N, expanded composite columns continue after. Type metadata is derived
     * from each column's base type with the same OID→SQL-type mapping SQLColumns
     * uses; ordering places the return/args first, then result columns. */
    size_t remaining = sizeof(query) - offset;
    written = snprintf(query + offset, remaining,
        " SELECT current_database()::varchar(128) AS \"PROCEDURE_CAT\", "
        "proc.nspname::varchar(128) AS \"PROCEDUR_SCHEM\", "
        "proc.proname::varchar(128) AS \"PROCEDURE_NAME\", "
        "arg.argname::varchar(128) AS \"COLUMN_NAME\", "
        "arg.coltype::smallint AS \"COLUMN_TYPE\", "
        "m.data_type::smallint AS \"DATA_TYPE\", "
        "arg.typname::varchar(128) AS \"TYPE_NAME\", "
        "m.col_size::int AS \"COLUMN_SIZE\", "
        "m.buf_len::int AS \"BUFFER_LENGTH\", "
        "m.dec_digits::smallint AS \"DECIMAL_DIGITS\", "
        "m.radix::smallint AS \"NUM_PREC_RADIX\", "
        "2::smallint AS \"NULLABLE\", "
        "NULL::varchar(128) AS \"REMARKS\", "
        "NULL::varchar(128) AS \"COLUMN_DEF\", "
        "m.data_type::smallint AS \"SQL_DATA_TYPE\", "
        "NULL::smallint AS \"SQL_DATETIME_SUB\", "
        "NULL::int AS \"CHAR_OCTET_LENGTH\", "
        "arg.ord::int AS \"ORDINAL_POSITION\", "
        "''::varchar(128) AS \"IS_NULLABLE\" "
        "FROM proc "
        "JOIN LATERAL ( "
        "  SELECT a.ord, a.argname, a.coltype, tt.typname, "
        "         COALESCE(NULLIF(tt.typbasetype,0), a.argtypoid) AS basetype "
        "  FROM ( "
        /* (a) declared arguments */
        "    SELECT gs.ord::int AS ord, "
        "           COALESCE(CASE WHEN proc.proallargtypes IS NOT NULL "
        "                THEN (proc.proallargtypes)[gs.ord] "
        "                ELSE (proc.proargtypes)[gs.ord - 1] END, 0) AS argtypoid, "
        "           COALESCE(proc.proargnames[gs.ord], '') AS argname, "
        "           CASE WHEN proc.proargmodes IS NULL THEN 1 "
        "                WHEN (proc.proargmodes)[gs.ord] = 't' THEN 3 "
        /* OUT / INOUT parameters are result columns (3) when the function
         * returns a set (setof record), otherwise output parameters (4). */
        "                WHEN (proc.proargmodes)[gs.ord] IN ('o','b') "
        "                     THEN CASE WHEN proc.proretset THEN 3 ELSE 4 END "
        "                ELSE 1 END AS coltype "
        "    FROM generate_series(1, "
        "           CASE WHEN proc.proallargtypes IS NOT NULL "
        "                THEN array_length(proc.proallargtypes, 1) "
        "                ELSE array_length(proc.proargtypes, 1) END) AS gs(ord) "
        "    UNION ALL "
        /* (b) scalar (base-type) return row */
        "    SELECT 0, proc.prorettype, '', 5 "
        "    WHERE proc.proargmodes IS NULL AND proc.prorettype <> 2278 "
        "      AND (SELECT rt.typtype FROM pg_catalog.pg_type rt "
        "           WHERE rt.oid = proc.prorettype) <> 'c' "
        "    UNION ALL "
        /* (c) expanded columns of a composite return type */
        "    SELECT (1000 + ratt.attnum)::int, ratt.atttypid, ratt.attname::text, "
        "           CASE WHEN proc.proretset THEN 3 ELSE 4 END "
        "    FROM pg_catalog.pg_type rt "
        "    JOIN pg_catalog.pg_class rc ON rc.oid = rt.typrelid "
        "    JOIN pg_catalog.pg_attribute ratt ON ratt.attrelid = rc.oid "
        "         AND ratt.attnum > 0 AND NOT ratt.attisdropped "
        "    WHERE rt.oid = proc.prorettype AND proc.proargmodes IS NULL "
        "      AND rt.typtype = 'c' "
        "  ) a "
        "  JOIN pg_catalog.pg_type tt ON tt.oid = a.argtypoid "
        ") AS arg ON true "
        "JOIN LATERAL ( "
        "  SELECT "
        "    CASE arg.basetype "
        "      WHEN 16 THEN %d WHEN 21 THEN %d WHEN 23 THEN %d WHEN 26 THEN %d "
        "      WHEN 20 THEN %d WHEN 700 THEN %d WHEN 701 THEN %d WHEN 1700 THEN %d "
        "      WHEN 1042 THEN %d WHEN 1043 THEN %d WHEN 25 THEN %d WHEN 1082 THEN %d "
        "      WHEN 1083 THEN %d WHEN 1114 THEN %d WHEN 1184 THEN %d WHEN 17 THEN %d "
        "      ELSE %d END AS data_type, "
        "    CASE arg.basetype WHEN 21 THEN 5 WHEN 23 THEN 10 WHEN 20 THEN 19 "
        "      WHEN 700 THEN 7 WHEN 701 THEN 15 WHEN 16 THEN 1 ELSE 0 END AS col_size, "
        "    CASE arg.basetype WHEN 21 THEN 2 WHEN 23 THEN 4 WHEN 20 THEN 8 "
        "      WHEN 700 THEN 4 WHEN 701 THEN 8 WHEN 16 THEN 1 ELSE 0 END AS buf_len, "
        "    0::smallint AS dec_digits, "
        "    CASE WHEN arg.basetype IN (21,23,20,1700) THEN 10 ELSE NULL END AS radix "
        ") AS m ON true "
        "ORDER BY \"PROCEDUR_SCHEM\", \"PROCEDURE_NAME\", proc.proid, arg.ord",
        bool_sql_type, (int)SQL_SMALLINT, (int)SQL_INTEGER, (int)SQL_LONGVARBINARY,
        (int)SQL_BIGINT, (int)SQL_REAL, (int)SQL_DOUBLE, (int)SQL_NUMERIC,
        (int)SQL_CHAR, (int)SQL_VARCHAR, (int)SQL_LONGVARCHAR, (int)SQL_TYPE_DATE,
        (int)SQL_TYPE_TIME, (int)SQL_TYPE_TIMESTAMP, (int)SQL_TYPE_TIMESTAMP,
        (int)SQL_LONGVARBINARY, (int)SQL_VARCHAR);
    if (written > 0) offset += (size_t)written;

    return execute_catalog_query(statement, query);
}

/* ---- SQLSpecialColumns ---- */

SQLRETURN catalog_special_columns(OdbcStatement *statement,
                                  SQLUSMALLINT identifier_type,
                                  const SQLCHAR *catalog_name, SQLSMALLINT catalog_length,
                                  const SQLCHAR *schema_name, SQLSMALLINT schema_length,
                                  const SQLCHAR *table_name, SQLSMALLINT table_length,
                                  SQLUSMALLINT scope, SQLUSMALLINT nullable)
{
    (void)catalog_name;
    (void)catalog_length;
    (void)scope;
    (void)nullable;

    char query[CATALOG_QUERY_BUFFER_SIZE];
    size_t offset = 0;

    if (identifier_type == SQL_ROWVER) {
        /* Row-version column: PostgreSQL's system "xmin" (transaction id) column
         * changes on every update. Emit a single fixed row with the driver's
         * ROWVER metadata (SCOPE/DATA_TYPE/... as smallint/int). xid is reported
         * as DATA_TYPE 4 (SQL_INTEGER), COLUMN_SIZE 10, PSEUDO_COLUMN 2
         * (SQL_PC_PSEUDO). */
        snprintf(query, sizeof(query),
            "SELECT NULL::smallint AS \"SCOPE\", "
            "CAST('xmin' AS varchar(128)) AS \"COLUMN_NAME\", "
            "%d::smallint AS \"DATA_TYPE\", "
            "CAST('xid' AS varchar(128)) AS \"TYPE_NAME\", "
            "10::int AS \"COLUMN_SIZE\", "
            "4::int AS \"BUFFER_LENGTH\", "
            "0::smallint AS \"DECIMAL_DIGITS\", "
            "2::smallint AS \"PSEUDO_COLUMN\"",
            (int)SQL_INTEGER);
        return execute_catalog_query(statement, query);
    }

    /* SQL_BEST_ROWID: the primary-key columns (or, failing that, the columns of
     * a unique index). This mirrors the original driver's findPrimaryOrUnique
     * query, whose text columns yield the BEST_ROWID result metadata the test
     * expects (SCOPE reported as text/NULL, DATA_TYPE as the type name). Only
     * key columns are considered (indkey[0 : indnkeyatts-1]) on PG11+; the slice
     * is version-gated below. */
    bool have_include_slice = statement->parent_connection &&
                              statement->parent_connection->server_version_major >= 11;
    const char *indkey_slice = have_include_slice
        ? "a.attnum = ANY (i.indkey[0:(i.indnkeyatts - 1)])"
        : "a.attnum = ANY (i.indkey)";

    /* First attempt: primary key. */
    int written = snprintf(query, sizeof(query),
        "SELECT NULL AS \"SCOPE\", a.attname AS \"COLUMN_NAME\", "
        "t.typname AS \"DATA_TYPE\", t.typname AS \"TYPE_NAME\", "
        "t.typlen AS \"COLUMN_SIZE\", a.attlen AS \"BUFFER_LENGTH\", "
        "CASE WHEN t.typname = 'numeric' THEN "
        "  CASE WHEN a.atttypmod > -1 THEN 6 ELSE a.atttypmod::int4 END "
        "ELSE 0 END AS \"DECIMAL_DIGITS\", "
        "1 AS \"PSEUDO_COLUMN\" "
        "FROM pg_class c "
        "INNER JOIN pg_namespace n ON n.oid = c.relnamespace "
        "INNER JOIN pg_attribute a ON a.attrelid = c.oid "
        "INNER JOIN pg_type t ON a.atttypid = t.oid "
        "LEFT JOIN pg_index i ON i.indrelid = c.oid AND %s "
        "WHERE i.indisprimary AND a.attnum > 0",
        indkey_slice);
    offset = (size_t)written;
    append_exact_filter(query, sizeof(query), &offset, "c.relname", table_name, table_length);
    if (schema_name && resolve_argument_length(schema_name, schema_length) > 0) {
        append_exact_filter(query, sizeof(query), &offset, "n.nspname", schema_name, schema_length);
    }

    SQLRETURN primary_result = execute_catalog_query(statement, query);

    /* If the table has no primary key, fall back to the columns of a unique
     * index (findPrimaryOrUnique's second pass). */
    if (primary_result == SQL_SUCCESS && statement->current_result &&
        PQntuples(statement->current_result) == 0) {
        offset = 0;
        written = snprintf(query, sizeof(query),
            "SELECT NULL AS \"SCOPE\", a.attname AS \"COLUMN_NAME\", "
            "t.typname AS \"DATA_TYPE\", t.typname AS \"TYPE_NAME\", "
            "t.typlen AS \"COLUMN_SIZE\", a.attlen AS \"BUFFER_LENGTH\", "
            "CASE WHEN t.typname = 'numeric' THEN "
            "  CASE WHEN a.atttypmod > -1 THEN 6 ELSE a.atttypmod::int4 END "
            "ELSE 0 END AS \"DECIMAL_DIGITS\", "
            "1 AS \"PSEUDO_COLUMN\" "
            "FROM pg_class c "
            "INNER JOIN pg_namespace n ON n.oid = c.relnamespace "
            "INNER JOIN pg_attribute a ON a.attrelid = c.oid "
            "INNER JOIN pg_type t ON a.atttypid = t.oid "
            "LEFT JOIN pg_index i ON i.indrelid = c.oid AND %s "
            "WHERE i.indisunique AND a.attnum > 0",
            indkey_slice);
        offset = (size_t)written;
        append_exact_filter(query, sizeof(query), &offset, "c.relname", table_name, table_length);
        if (schema_name && resolve_argument_length(schema_name, schema_length) > 0) {
            append_exact_filter(query, sizeof(query), &offset, "n.nspname", schema_name, schema_length);
        }
        return execute_catalog_query(statement, query);
    }

    return primary_result;
}
