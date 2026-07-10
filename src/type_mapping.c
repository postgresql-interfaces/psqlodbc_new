/*-------------------------------------------------------------------------
 *
 * type_mapping.c
 *	  PostgreSQL OID to ODBC SQL type mapping
 *
 * Copyright (c) 2026, Dave Cramer
 *
 *
 * IDENTIFICATION
 *	  src/type_mapping.c
 *
 *-------------------------------------------------------------------------
 */
#include "type_mapping.h"

#include <stddef.h>

/* ---- Type Mapping Table ---- */

typedef struct PostgresTypeMapping {
    unsigned int postgres_oid;
    SQLSMALLINT  sql_type;
    SQLSMALLINT  default_c_type;
    const char  *type_name;
} PostgresTypeMapping;

static const PostgresTypeMapping type_table[] = {
    { PG_TYPE_BOOL,        SQL_BIT,              SQL_C_BIT,      "bool"        },
    { PG_TYPE_BYTEA,       SQL_LONGVARBINARY,    SQL_C_BINARY,   "bytea"       },
    { PG_TYPE_CHAR,        SQL_CHAR,             SQL_C_CHAR,     "char"        },
    { PG_TYPE_NAME,        SQL_VARCHAR,          SQL_C_CHAR,     "name"        },
    { PG_TYPE_INT8,        SQL_BIGINT,           SQL_C_SBIGINT,  "int8"        },
    { PG_TYPE_INT2,        SQL_SMALLINT,         SQL_C_SSHORT,   "int2"        },
    { PG_TYPE_INT4,        SQL_INTEGER,          SQL_C_SLONG,    "int4"        },
    { PG_TYPE_TEXT,        SQL_LONGVARCHAR,      SQL_C_CHAR,     "text"        },
    { PG_TYPE_OID,         SQL_INTEGER,          SQL_C_SLONG,    "oid"         },
    { PG_TYPE_FLOAT4,      SQL_REAL,             SQL_C_FLOAT,    "float4"      },
    { PG_TYPE_FLOAT8,      SQL_DOUBLE,           SQL_C_DOUBLE,   "float8"      },
    { PG_TYPE_BPCHAR,      SQL_CHAR,             SQL_C_CHAR,     "bpchar"      },
    { PG_TYPE_VARCHAR,     SQL_VARCHAR,          SQL_C_CHAR,     "varchar"     },
    { PG_TYPE_DATE,        SQL_TYPE_DATE,        SQL_C_CHAR,     "date"        },
    { PG_TYPE_TIME,        SQL_TYPE_TIME,        SQL_C_CHAR,     "time"        },
    { PG_TYPE_TIMESTAMP,   SQL_TYPE_TIMESTAMP,   SQL_C_CHAR,     "timestamp"   },
    { PG_TYPE_TIMESTAMPTZ, SQL_TYPE_TIMESTAMP,   SQL_C_CHAR,     "timestamptz" },
    { PG_TYPE_NUMERIC,     SQL_NUMERIC,          SQL_C_CHAR,     "numeric"     },
    { PG_TYPE_UUID,        SQL_GUID,             SQL_C_CHAR,     "uuid"        },
};

static const int TYPE_TABLE_SIZE = (int)(sizeof(type_table) / sizeof(type_table[0]));

/* ---- Internal Helpers ---- */

static const PostgresTypeMapping *find_mapping(unsigned int postgres_oid)
{
    for (int index = 0; index < TYPE_TABLE_SIZE; index++) {
        if (type_table[index].postgres_oid == postgres_oid) {
            return &type_table[index];
        }
    }
    return NULL;
}

/* ---- Public Interface ---- */

SQLSMALLINT type_mapping_get_sql_type(unsigned int postgres_oid)
{
    const PostgresTypeMapping *mapping = find_mapping(postgres_oid);
    if (mapping) {
        return mapping->sql_type;
    }
    return SQL_VARCHAR;
}

SQLSMALLINT type_mapping_get_default_c_type(SQLSMALLINT sql_type)
{
    switch (sql_type) {
    case SQL_BIT:
        return SQL_C_BIT;
    case SQL_SMALLINT:
        return SQL_C_SSHORT;
    case SQL_INTEGER:
        return SQL_C_SLONG;
    case SQL_BIGINT:
        return SQL_C_SBIGINT;
    case SQL_REAL:
        return SQL_C_FLOAT;
    case SQL_DOUBLE:
    case SQL_FLOAT:
        return SQL_C_DOUBLE;
    case SQL_NUMERIC:
    case SQL_DECIMAL:
        return SQL_C_CHAR;
    case SQL_CHAR:
    case SQL_VARCHAR:
    case SQL_LONGVARCHAR:
        return SQL_C_CHAR;
    case SQL_TYPE_DATE:
        return SQL_C_CHAR;
    case SQL_TYPE_TIME:
        return SQL_C_CHAR;
    case SQL_TYPE_TIMESTAMP:
        return SQL_C_CHAR;
    case SQL_BINARY:
    case SQL_VARBINARY:
    case SQL_LONGVARBINARY:
        return SQL_C_BINARY;
    case SQL_GUID:
        return SQL_C_CHAR;
    default:
        return SQL_C_CHAR;
    }
}

SQLULEN type_mapping_get_column_size(unsigned int postgres_oid, int type_modifier)
{
    switch (postgres_oid) {
    case PG_TYPE_BOOL:
        return 1;
    case PG_TYPE_INT2:
        return 5;      /* -32768 to 32767 */
    case PG_TYPE_INT4:
    case PG_TYPE_OID:
        return 10;     /* -2147483648 to 2147483647 */
    case PG_TYPE_INT8:
        return 19;     /* 19 digits for int8 */
    case PG_TYPE_FLOAT4:
        return 7;      /* ~7 significant digits */
    case PG_TYPE_FLOAT8:
        return 15;     /* ~15 significant digits */
    case PG_TYPE_DATE:
        return 10;     /* YYYY-MM-DD */
    case PG_TYPE_TIME:
        return 8;      /* HH:MM:SS (without fractional seconds) */
    case PG_TYPE_TIMESTAMP:
    case PG_TYPE_TIMESTAMPTZ:
        return 26;     /* YYYY-MM-DD HH:MM:SS.ffffff */
    case PG_TYPE_UUID:
        return 36;     /* xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx */
    case PG_TYPE_CHAR:
        return 1;
    case PG_TYPE_NAME:
        return 63;     /* NAMEDATALEN - 1 in PostgreSQL */

    case PG_TYPE_VARCHAR:
    case PG_TYPE_BPCHAR:
        /* typmod for varchar/char(n) is declared_length + 4 (header overhead).
         * If typmod is -1, the type has no declared limit. */
        if (type_modifier > 4) {
            return (SQLULEN)(type_modifier - 4);
        }
        return 0;  /* Variable length, no declared limit */

    case PG_TYPE_NUMERIC:
        /* typmod for numeric(p,s): precision = ((typmod-4) >> 16) & 0xFFFF.
         * If typmod is -1, it's an unconstrained numeric. */
        if (type_modifier > 4) {
            return (SQLULEN)(((type_modifier - 4) >> 16) & 0xFFFF);
        }
        return 38;  /* Default precision for unconstrained numeric */

    case PG_TYPE_TEXT:
    case PG_TYPE_BYTEA:
        return 0;  /* Variable length, no inherent limit */

    default:
        return 0;
    }
}

SQLSMALLINT type_mapping_get_decimal_digits(unsigned int postgres_oid, int type_modifier)
{
    switch (postgres_oid) {
    case PG_TYPE_NUMERIC:
        /* typmod for numeric(p,s): scale = (typmod-4) & 0xFFFF */
        if (type_modifier > 4) {
            return (SQLSMALLINT)((type_modifier - 4) & 0xFFFF);
        }
        return 0;  /* Unconstrained numeric — scale unknown */

    case PG_TYPE_TIMESTAMP:
    case PG_TYPE_TIMESTAMPTZ:
        /* PostgreSQL timestamps have microsecond precision (6 fractional digits) */
        return 6;

    case PG_TYPE_TIME:
        return 6;

    case PG_TYPE_INT2:
    case PG_TYPE_INT4:
    case PG_TYPE_INT8:
    case PG_TYPE_OID:
        return 0;  /* Integer types have no fractional digits */

    default:
        return 0;
    }
}

const char *type_mapping_get_type_name(unsigned int postgres_oid)
{
    const PostgresTypeMapping *mapping = find_mapping(postgres_oid);
    if (mapping) {
        return mapping->type_name;
    }
    return "unknown";
}

const char *type_mapping_get_param_cast(SQLSMALLINT sql_type)
{
    switch (sql_type) {
    case SQL_BINARY:
    case SQL_VARBINARY:
        return "::bytea";
    case SQL_TYPE_DATE:
    case SQL_DATE:
        return "::date";
    case SQL_DECIMAL:
    case SQL_NUMERIC:
        return "::numeric";
    case SQL_BIGINT:
        return "::int8";
    case SQL_INTEGER:
        return "::int4";
    case SQL_REAL:
        return "::float4";
    /* SQL_FLOAT and SQL_DOUBLE are intentionally left uncast, matching the
     * original driver. PostgreSQL infers the parameter type from context
     * (e.g. the numeric literal on the other side of a comparison), which
     * yields correct results and a "numeric" error for malformed input. */
    case SQL_SMALLINT:
    case SQL_TINYINT:
        return "::int2";
    case SQL_TIME:
    case SQL_TYPE_TIME:
        return "::time";
    case SQL_TIMESTAMP:
    case SQL_TYPE_TIMESTAMP:
        return "::timestamp";
    case SQL_GUID:
        return "::uuid";
    default:
        /* SQL_CHAR, SQL_VARCHAR, SQL_LONGVARCHAR and anything else: no cast,
         * so the value is treated as text by PostgreSQL. */
        return NULL;
    }
}
