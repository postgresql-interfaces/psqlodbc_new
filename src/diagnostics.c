/*-------------------------------------------------------------------------
 *
 * diagnostics.c
 *	  ODBC diagnostic record system
 *
 * Copyright (c) 2026, Dave Cramer
 *
 *
 * IDENTIFICATION
 *	  src/diagnostics.c
 *
 *-------------------------------------------------------------------------
 */
#include "diagnostics.h"

#include <stdlib.h>
#include <string.h>

void diagnostics_clear(DiagnosticRecords *records)
{
    if (!records) {
        return;
    }

    for (int index = 0; index < records->record_count; index++) {
        free(records->records[index].message_text);
        records->records[index].message_text = NULL;
    }

    records->record_count = 0;
}

bool diagnostics_add_record(DiagnosticRecords *records,
                            const char *sqlstate,
                            int native_error_code,
                            const char *message)
{
    if (!records) {
        return false;
    }

    if (records->record_count >= MAX_DIAGNOSTIC_RECORDS) {
        /* Array is full — drop the record. This is acceptable because
         * applications should retrieve diagnostics promptly. */
        return false;
    }

    DiagnosticRecord *record = &records->records[records->record_count];

    /* Copy SQLSTATE (default to "00000" if not provided) */
    if (sqlstate) {
        strncpy(record->sqlstate, sqlstate, SQLSTATE_LENGTH);
        record->sqlstate[SQLSTATE_LENGTH] = '\0';
    } else {
        memcpy(record->sqlstate, "00000", SQLSTATE_LENGTH + 1);
    }

    record->native_error_code = native_error_code;

    /* Heap-allocate a copy of the message text.
     * Using malloc+memcpy instead of strdup for strict C11 portability
     * (strdup was only standardized in C23). */
    if (message) {
        size_t message_length = strlen(message);
        record->message_text = malloc(message_length + 1);
        if (record->message_text) {
            memcpy(record->message_text, message, message_length + 1);
        }
    } else {
        record->message_text = NULL;
    }

    records->record_count++;
    return true;
}

bool diagnostics_get_record(const DiagnosticRecords *records,
                            int record_number,
                            char *out_sqlstate,
                            int *out_native_error_code,
                            const char **out_message)
{
    if (!records) {
        return false;
    }

    /* ODBC uses 1-based record numbering */
    int index = record_number - 1;

    if (index < 0 || index >= records->record_count) {
        return false;
    }

    const DiagnosticRecord *record = &records->records[index];

    if (out_sqlstate) {
        memcpy(out_sqlstate, record->sqlstate, SQLSTATE_LENGTH + 1);
    }

    if (out_native_error_code) {
        *out_native_error_code = record->native_error_code;
    }

    if (out_message) {
        *out_message = record->message_text;
    }

    return true;
}
