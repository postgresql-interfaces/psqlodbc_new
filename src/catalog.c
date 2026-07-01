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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

    int written = snprintf(query, sizeof(query),
        "SELECT current_database()::varchar AS \"TABLE_CAT\", "
        "n.nspname::varchar AS \"TABLE_SCHEM\", "
        "c.relname::varchar AS \"TABLE_NAME\", "
        "CASE c.relkind "
        "  WHEN 'r' THEN 'TABLE' "
        "  WHEN 'p' THEN 'TABLE' "
        "  WHEN 'v' THEN 'VIEW' "
        "  WHEN 'm' THEN 'MATERIALIZED VIEW' "
        "  WHEN 'f' THEN 'FOREIGN TABLE' "
        "END::varchar AS \"TABLE_TYPE\", "
        "pg_catalog.obj_description(c.oid, 'pg_class')::varchar AS \"REMARKS\" "
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
             * Recognized types: TABLE, VIEW, MATERIALIZED VIEW, FOREIGN TABLE */
            char relkinds[32];
            size_t rk_offset = 0;
            char type_copy[256];
            size_t copy_len = type_actual_length < sizeof(type_copy) - 1
                ? type_actual_length : sizeof(type_copy) - 1;
            memcpy(type_copy, table_type, copy_len);
            type_copy[copy_len] = '\0';

            /* Tokenize by comma */
            char *saveptr = NULL;
            char *token = strtok_r(type_copy, ",", &saveptr);
            while (token) {
                /* Strip leading/trailing spaces and quotes */
                while (*token == ' ' || *token == '\'') token++;
                size_t tlen = strlen(token);
                while (tlen > 0 && (token[tlen-1] == ' ' || token[tlen-1] == '\'')) {
                    token[--tlen] = '\0';
                }

                if (strcasecmp(token, "TABLE") == 0) {
                    if (rk_offset > 0) relkinds[rk_offset++] = ',';
                    relkinds[rk_offset++] = '\''; relkinds[rk_offset++] = 'r'; relkinds[rk_offset++] = '\'';
                    relkinds[rk_offset++] = ',';
                    relkinds[rk_offset++] = '\''; relkinds[rk_offset++] = 'p'; relkinds[rk_offset++] = '\'';
                } else if (strcasecmp(token, "VIEW") == 0) {
                    if (rk_offset > 0) relkinds[rk_offset++] = ',';
                    relkinds[rk_offset++] = '\''; relkinds[rk_offset++] = 'v'; relkinds[rk_offset++] = '\'';
                } else if (strcasecmp(token, "MATERIALIZED VIEW") == 0) {
                    if (rk_offset > 0) relkinds[rk_offset++] = ',';
                    relkinds[rk_offset++] = '\''; relkinds[rk_offset++] = 'm'; relkinds[rk_offset++] = '\'';
                } else if (strcasecmp(token, "FOREIGN TABLE") == 0) {
                    if (rk_offset > 0) relkinds[rk_offset++] = ',';
                    relkinds[rk_offset++] = '\''; relkinds[rk_offset++] = 'f'; relkinds[rk_offset++] = '\'';
                }

                token = strtok_r(NULL, ",", &saveptr);
            }
            relkinds[rk_offset] = '\0';

            if (rk_offset > 0) {
                size_t remaining = sizeof(query) - offset;
                int w = snprintf(query + offset, remaining,
                                 " AND c.relkind IN (%s)", relkinds);
                if (w > 0) offset += (size_t)w;
            }
        }
    }

    /* Order by per ODBC spec */
    size_t remaining = sizeof(query) - offset;
    written = snprintf(query + offset, remaining,
                       " ORDER BY \"TABLE_TYPE\", \"TABLE_SCHEM\", \"TABLE_NAME\"");
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

    int written = snprintf(query, sizeof(query),
        "SELECT "
        "current_database()::varchar AS \"TABLE_CAT\", "
        "n.nspname::varchar AS \"TABLE_SCHEM\", "
        "c.relname::varchar AS \"TABLE_NAME\", "
        "a.attname::varchar AS \"COLUMN_NAME\", "
        "CASE "
        "  WHEN t.oid = 16 THEN %d "    /* bool → SQL_BIT */
        "  WHEN t.oid = 21 THEN %d "    /* int2 → SQL_SMALLINT */
        "  WHEN t.oid = 23 THEN %d "    /* int4 → SQL_INTEGER */
        "  WHEN t.oid = 20 THEN %d "    /* int8 → SQL_BIGINT */
        "  WHEN t.oid = 700 THEN %d "   /* float4 → SQL_REAL */
        "  WHEN t.oid = 701 THEN %d "   /* float8 → SQL_DOUBLE */
        "  WHEN t.oid = 1700 THEN %d "  /* numeric → SQL_NUMERIC */
        "  WHEN t.oid = 1042 THEN %d "  /* bpchar → SQL_CHAR */
        "  WHEN t.oid = 1043 THEN %d "  /* varchar → SQL_VARCHAR */
        "  WHEN t.oid = 25 THEN %d "    /* text → SQL_LONGVARCHAR */
        "  WHEN t.oid = 1082 THEN %d "  /* date → SQL_TYPE_DATE */
        "  WHEN t.oid = 1083 THEN %d "  /* time → SQL_TYPE_TIME */
        "  WHEN t.oid IN (1114,1184) THEN %d " /* timestamp → SQL_TYPE_TIMESTAMP */
        "  WHEN t.oid = 17 THEN %d "    /* bytea → SQL_LONGVARBINARY */
        "  ELSE %d "                     /* default → SQL_VARCHAR */
        "END::smallint AS \"DATA_TYPE\", "
        "t.typname::varchar AS \"TYPE_NAME\", "
        "CASE "
        "  WHEN t.oid IN (1042,1043) AND a.atttypmod > 0 THEN a.atttypmod - 4 "
        "  WHEN t.oid = 1700 AND a.atttypmod > 0 THEN ((a.atttypmod - 4) >> 16) & 65535 "
        "  WHEN t.oid = 21 THEN 5 "
        "  WHEN t.oid = 23 THEN 10 "
        "  WHEN t.oid = 20 THEN 19 "
        "  WHEN t.oid = 700 THEN 7 "
        "  WHEN t.oid = 701 THEN 15 "
        "  WHEN t.oid = 16 THEN 1 "
        "  WHEN t.oid = 1082 THEN 10 "
        "  WHEN t.oid = 1083 THEN 8 "
        "  WHEN t.oid IN (1114,1184) THEN 26 "
        "  ELSE 0 "
        "END::int AS \"COLUMN_SIZE\", "
        "CASE "
        "  WHEN t.oid IN (21) THEN 2 "
        "  WHEN t.oid IN (23) THEN 4 "
        "  WHEN t.oid IN (20) THEN 8 "
        "  WHEN t.oid IN (700) THEN 4 "
        "  WHEN t.oid IN (701) THEN 8 "
        "  WHEN t.oid IN (16) THEN 1 "
        "  WHEN t.oid IN (1042,1043) AND a.atttypmod > 0 THEN a.atttypmod - 4 "
        "  ELSE 0 "
        "END::int AS \"BUFFER_LENGTH\", "
        "CASE "
        "  WHEN t.oid = 1700 AND a.atttypmod > 0 THEN ((a.atttypmod - 4) & 65535)::smallint "
        "  WHEN t.oid IN (1114,1184,1083) THEN 6::smallint "
        "  ELSE 0::smallint "
        "END AS \"DECIMAL_DIGITS\", "
        "CASE WHEN t.oid IN (21,23,20,1700) THEN 10 ELSE NULL END::smallint AS \"NUM_PREC_RADIX\", "
        "CASE WHEN a.attnotnull THEN 0 ELSE 1 END::smallint AS \"NULLABLE\", "
        "pg_catalog.col_description(c.oid, a.attnum)::varchar AS \"REMARKS\", "
        "pg_catalog.pg_get_expr(d.adbin, d.adrelid)::varchar AS \"COLUMN_DEF\", "
        "CASE "
        "  WHEN t.oid = 16 THEN %d "
        "  WHEN t.oid = 21 THEN %d "
        "  WHEN t.oid = 23 THEN %d "
        "  WHEN t.oid = 20 THEN %d "
        "  WHEN t.oid = 700 THEN %d "
        "  WHEN t.oid = 701 THEN %d "
        "  WHEN t.oid = 1700 THEN %d "
        "  WHEN t.oid IN (1042,1043,25) THEN %d "
        "  WHEN t.oid IN (1082,1083,1114,1184) THEN %d "
        "  ELSE %d "
        "END::smallint AS \"SQL_DATA_TYPE\", "
        "NULL::smallint AS \"SQL_DATETIME_SUB\", "
        "CASE WHEN t.oid IN (1042,1043,25) AND a.atttypmod > 0 THEN a.atttypmod - 4 ELSE NULL END::int AS \"CHAR_OCTET_LENGTH\", "
        "a.attnum::int AS \"ORDINAL_POSITION\", "
        "CASE WHEN a.attnotnull THEN 'NO' ELSE 'YES' END::varchar AS \"IS_NULLABLE\" "
        "FROM pg_catalog.pg_attribute a "
        "JOIN pg_catalog.pg_class c ON a.attrelid = c.oid "
        "JOIN pg_catalog.pg_namespace n ON c.relnamespace = n.oid "
        "JOIN pg_catalog.pg_type t ON a.atttypid = t.oid "
        "LEFT JOIN pg_catalog.pg_attrdef d ON (a.attrelid = d.adrelid AND a.attnum = d.adnum) "
        "WHERE a.attnum > 0 AND NOT a.attisdropped "
        "AND c.relkind IN ('r','v','m','f','p') "
        "AND n.nspname NOT IN ('pg_catalog','information_schema','pg_toast')",
        /* DATA_TYPE values */
        (int)SQL_BIT, (int)SQL_SMALLINT, (int)SQL_INTEGER, (int)SQL_BIGINT,
        (int)SQL_REAL, (int)SQL_DOUBLE, (int)SQL_NUMERIC,
        (int)SQL_CHAR, (int)SQL_VARCHAR, (int)SQL_LONGVARCHAR,
        (int)SQL_TYPE_DATE, (int)SQL_TYPE_TIME, (int)SQL_TYPE_TIMESTAMP,
        (int)SQL_LONGVARBINARY, (int)SQL_VARCHAR,
        /* SQL_DATA_TYPE values (same as DATA_TYPE for non-datetime) */
        (int)SQL_BIT, (int)SQL_SMALLINT, (int)SQL_INTEGER, (int)SQL_BIGINT,
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
        "current_database()::varchar AS \"TABLE_CAT\", "
        "n.nspname::varchar AS \"TABLE_SCHEM\", "
        "c.relname::varchar AS \"TABLE_NAME\", "
        "a.attname::varchar AS \"COLUMN_NAME\", "
        "(array_position(i.indkey, a.attnum) + 1)::smallint AS \"KEY_SEQ\", "
        "ic.relname::varchar AS \"PK_NAME\" "
        "FROM pg_catalog.pg_index i "
        "JOIN pg_catalog.pg_class c ON c.oid = i.indrelid "
        "JOIN pg_catalog.pg_class ic ON ic.oid = i.indexrelid "
        "JOIN pg_catalog.pg_namespace n ON n.oid = c.relnamespace "
        "JOIN pg_catalog.pg_attribute a ON a.attrelid = c.oid AND a.attnum = ANY(i.indkey) "
        "WHERE i.indisprimary");
    offset = (size_t)written;

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
        "current_database()::varchar AS \"PKTABLE_CAT\", "
        "pn.nspname::varchar AS \"PKTABLE_SCHEM\", "
        "pc.relname::varchar AS \"PKTABLE_NAME\", "
        "pa.attname::varchar AS \"PKCOLUMN_NAME\", "
        "current_database()::varchar AS \"FKTABLE_CAT\", "
        "fn.nspname::varchar AS \"FKTABLE_SCHEM\", "
        "fc.relname::varchar AS \"FKTABLE_NAME\", "
        "fa.attname::varchar AS \"FKCOLUMN_NAME\", "
        "cols.ordinality::smallint AS \"KEY_SEQ\", "
        "CASE con.confupdtype "
        "  WHEN 'c' THEN 0 WHEN 'r' THEN 1 WHEN 'n' THEN 2 "
        "  WHEN 'a' THEN 3 WHEN 'd' THEN 4 ELSE 3 "
        "END::smallint AS \"UPDATE_RULE\", "
        "CASE con.confdeltype "
        "  WHEN 'c' THEN 0 WHEN 'r' THEN 1 WHEN 'n' THEN 2 "
        "  WHEN 'a' THEN 3 WHEN 'd' THEN 4 ELSE 3 "
        "END::smallint AS \"DELETE_RULE\", "
        "con.conname::varchar AS \"FK_NAME\", "
        "(SELECT ic.relname FROM pg_catalog.pg_index pi "
        "  JOIN pg_catalog.pg_class ic ON ic.oid = pi.indexrelid "
        "  WHERE pi.indrelid = con.confrelid AND pi.indisprimary LIMIT 1)::varchar AS \"PK_NAME\", "
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
