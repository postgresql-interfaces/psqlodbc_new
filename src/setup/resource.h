/*-------------------------------------------------------------------------
 *
 * resource.h
 *	  Control identifiers shared between the DSN setup dialog resource script
 *	  (setup_dialog.rc) and the dialog procedure (setup_windows.c).
 *
 * Every honored DSN field has one edit-control ID here, so neither the .rc nor
 * the C code ever refers to a control by a bare number. Keep this list and the
 * layout in setup_dialog.rc in sync: a control referenced in one file but not
 * declared here would be a silent magic number.
 *
 * Copyright (c) 2026, Dave Cramer
 *
 *
 * IDENTIFICATION
 *	  src/setup/resource.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PSQLODBC2_SETUP_RESOURCE_H
#define PSQLODBC2_SETUP_RESOURCE_H

/*
 * The DSN configuration dialog template. Referenced from ConfigDSN via
 * MAKEINTRESOURCE(IDD_CONFIG_DIALOG) when DialogBoxParamW is invoked.
 */
#define IDD_CONFIG_DIALOG 100

/*
 * OK and Cancel use the Windows-standard IDOK (1) and IDCANCEL (2) values,
 * which the SDK defines in <winuser.h>. The .rc pulls those in via
 * <windows.h>, so they are intentionally NOT redefined here.
 */

/*
 * Edit-control IDs, one per honored DSN keyword. The numeric values are
 * arbitrary but must be unique within the dialog and must match the .rc.
 * They are grouped in a contiguous range starting at 1000 to keep them
 * clearly distinct from the standard button IDs (1/2).
 */
#define IDC_EDIT_DATA_SOURCE_NAME  1000  /* The DSN name (mandatory). */
#define IDC_EDIT_DESCRIPTION       1001
#define IDC_EDIT_SERVERNAME        1002
#define IDC_EDIT_PORT              1003
#define IDC_EDIT_DATABASE          1004
#define IDC_EDIT_USERNAME          1005
#define IDC_EDIT_PASSWORD          1006
#define IDC_EDIT_SSLMODE           1007
#define IDC_EDIT_APPLICATION_NAME  1008
#define IDC_EDIT_TIMEOUT           1009

#endif /* PSQLODBC2_SETUP_RESOURCE_H */
