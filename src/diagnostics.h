/*-------------------------------------------------------------------------
 *
 * diagnostics.h
 *	  Diagnostic record struct and API
 *
 * Copyright (c) 2026, Dave Cramer
 *
 *
 * IDENTIFICATION
 *	  src/diagnostics.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PSQLODBC2_DIAGNOSTICS_H
#define PSQLODBC2_DIAGNOSTICS_H

#include <stdbool.h>

/* Maximum number of diagnostic records retained per handle. The ODBC spec does
 * not mandate a specific limit, but 16 is generous for typical usage patterns
 * where applications check errors immediately after each call. */
#define MAX_DIAGNOSTIC_RECORDS 16

/* SQLSTATE codes are always exactly 5 characters (e.g., "08001") plus a null
 * terminator. This constant avoids magic '6' appearing in struct definitions. */
#define SQLSTATE_LENGTH 5

/* Default chunk size for paging a long diagnostic message across successive
 * SQLGetDiagRec calls when the caller passes a zero-length buffer (so no chunk
 * size can be inferred). This mirrors the historical ODBC driver-manager
 * message-buffer cap used by the original psqlodbc driver (DRVMNGRDIV). */
#define DIAGNOSTIC_DEFAULT_PAGE_SIZE 511

/* A single diagnostic record as defined by the ODBC specification.
 * message_text is heap-allocated and owned by this record. */
typedef struct DiagnosticRecord {
    char sqlstate[SQLSTATE_LENGTH + 1];  /* 5-char SQLSTATE + null terminator */
    int  native_error_code;
    char *message_text;                  /* heap-allocated, NULL if no message */
} DiagnosticRecord;

/* Collection of diagnostic records embedded in each ODBC handle.
 * Records are added sequentially; record_count tracks how many are valid. */
typedef struct DiagnosticRecords {
    DiagnosticRecord records[MAX_DIAGNOSTIC_RECORDS];
    int record_count;

    /*
     * Chunk size (in characters) used when a single long diagnostic message is
     * returned to the application in pieces across successive SQLGetDiagRec
     * RecNumber calls. See the paging discussion in SQLGetDiagRec (odbc_api.c).
     * Captured from the caller's buffer length on the first retrieval; 0 means
     * "not yet established". Reset by diagnostics_clear.
     */
    int paging_chunk_size;
} DiagnosticRecords;

/*
 * Clear all diagnostic records, freeing any heap-allocated message strings.
 * This is called at the start of each ODBC API function to reset the
 * diagnostic state (per ODBC spec: diagnostics are cleared on each new call).
 */
void diagnostics_clear(DiagnosticRecords *records);

/*
 * Add a new diagnostic record. Returns true if the record was added, false
 * if the record array is full (in which case the record is silently dropped).
 *
 * sqlstate must point to a valid 5-character string (or NULL for "00000").
 * message is copied to the heap (may be NULL for no message).
 */
bool diagnostics_add_record(DiagnosticRecords *records,
                            const char *sqlstate,
                            int native_error_code,
                            const char *message);

/*
 * Retrieve a diagnostic record by 1-based index (matching ODBC's convention
 * where record_number=1 is the first record).
 *
 * Returns true if the record exists, false if record_number is out of range.
 * Any output pointer may be NULL if the caller doesn't need that field.
 *
 * out_sqlstate must point to a buffer of at least SQLSTATE_LENGTH+1 bytes.
 * out_message receives a pointer to the internal string (not a copy) — the
 * caller must not free it.
 */
bool diagnostics_get_record(const DiagnosticRecords *records,
                            int record_number,
                            char *out_sqlstate,
                            int *out_native_error_code,
                            const char **out_message);

#endif /* PSQLODBC2_DIAGNOSTICS_H */
