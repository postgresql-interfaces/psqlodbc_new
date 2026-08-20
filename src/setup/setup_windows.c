/*-------------------------------------------------------------------------
 *
 * setup_windows.c
 *	  Windows-only ODBC setup entry points (ConfigDSN / ConfigDSNW /
 *	  ConfigDriver).
 *
 * This module is compiled into a separate DLL (psqlodbc2setup.dll), NOT into
 * the driver itself. The ODBC Data Source Administrator loads this DLL when
 * the user clicks Add.../Configure/Remove for the driver, and calls ConfigDSN
 * (or its Unicode twin ConfigDSNW). Keeping the GUI/installer dependencies
 * (odbccp32, comctl32, ...) out of the always-loaded driver DLL mirrors the
 * split used by the original psqlodbc.
 *
 * Behavior: ODBC_REMOVE_DSN deletes the DSN. ODBC_ADD_DSN / ODBC_CONFIG_DSN
 * either present a modal dialog (when the Administrator supplies a parent
 * window) so the user can edit the honored connection keywords, or — when
 * invoked silently with no parent window — register the DSN directly from the
 * attribute string with no UI.
 *
 * Copyright (c) 2026, Dave Cramer
 *
 *
 * IDENTIFICATION
 *	  src/setup/setup_windows.c
 *
 *-------------------------------------------------------------------------
 */

/*
 * This is the sole platform-specific module in the codebase. The entire body
 * is guarded so the file compiles to nothing meaningful off Windows. In
 * practice the Meson target that builds it is itself guarded to Windows, so
 * this file is never even handed to the compiler on Linux/macOS — the guard
 * is a belt-and-suspenders safeguard.
 */
#ifdef _WIN32

#include <windows.h>
#include <odbcinst.h>
#include <stdlib.h>
#include <string.h>

#include "resource.h"

/*
 * Handle to this DLL's own module, captured in DllMain. DialogBoxParamW needs
 * it to locate the dialog template resource bundled in this DLL, so it must be
 * the setup DLL's instance — never the caller's. It is written once during
 * DLL_PROCESS_ATTACH (before any ODBC entry point can run) and only read
 * thereafter, so no synchronization is required.
 */
static HINSTANCE setup_module_instance = NULL;

/*
 * INI "file name" passed to the ODBC installer API for data-source entries.
 * The installer routes this pseudo-file to the correct per-user or system
 * registry hive automatically, so the setup DLL never touches the registry
 * directly and never needs to know whether the DSN is a User or System DSN.
 */
#define ODBC_INI_FILE_NAME "ODBC.INI"

/*
 * The keyword that names the data source itself within an ODBC attribute
 * list (e.g. "DSN=MyDatabase").
 */
#define ATTRIBUTE_KEYWORD_DATA_SOURCE_NAME "DSN"

/*
 * Connection keywords this driver honors, written into the DSN section.
 *
 * These string literals MUST stay identical to the DSN_KEY_* macros in
 * src/dsn_config.h — that is where the driver reads them back at connect
 * time, so a mismatch would silently drop settings. They are duplicated here
 * (rather than including dsn_config.h) so the standalone setup DLL does not
 * pull in the driver's internal headers and dependencies.
 */
#define ATTRIBUTE_KEYWORD_DESCRIPTION      "Description"
#define ATTRIBUTE_KEYWORD_SERVERNAME       "Servername"
#define ATTRIBUTE_KEYWORD_PORT             "Port"
#define ATTRIBUTE_KEYWORD_DATABASE         "Database"
#define ATTRIBUTE_KEYWORD_USERNAME         "Username"
#define ATTRIBUTE_KEYWORD_UID              "UID"
#define ATTRIBUTE_KEYWORD_PASSWORD         "Password"
#define ATTRIBUTE_KEYWORD_SSLMODE          "SSLmode"
#define ATTRIBUTE_KEYWORD_APPLICATION_NAME "ApplicationName"
#define ATTRIBUTE_KEYWORD_TIMEOUT          "Timeout"

/*
 * Buffer sizes for values copied out of the attribute list. ODBC data source
 * names are limited to 32 characters by the installer, but connection values
 * (database name, application name, password) can be longer; this bound is
 * generous while still guarding against a runaway attribute value.
 */
#define MAX_DATA_SOURCE_NAME_LENGTH 256
#define MAX_ATTRIBUTE_VALUE_LENGTH  1024

/*
 * The ODBC installer limits data source names to 32 characters. The dialog's
 * DSN-name edit control is capped to this so the user cannot type a name the
 * installer would later reject.
 */
#define ODBC_MAX_DSN_NAME_LENGTH 32

/*
 * One honored connection keyword and the value parsed for it, if any.
 * was_supplied distinguishes "not present in the attribute list" from
 * "present but empty", so we only write keys the caller actually provided.
 */
typedef struct HonoredAttribute
{
	const char *keyword;
	char        value[MAX_ATTRIBUTE_VALUE_LENGTH];
	BOOL        was_supplied;
} HonoredAttribute;

/*
 * Copy src into dest, always NUL-terminating and never writing past
 * dest_size bytes. Used for the fixed-size value buffers above.
 */
static void
copy_string_bounded(char *dest, size_t dest_size, const char *src)
{
	if (dest_size == 0)
		return;

	size_t index = 0;
	while (index + 1 < dest_size && src[index] != '\0')
	{
		dest[index] = src[index];
		index++;
	}
	dest[index] = '\0';
}

/*
 * Parse one "KEYWORD=VALUE" entry from the attribute list. If the keyword
 * matches one of the honored connection keywords, its value is stored into
 * that slot. If it is the DSN keyword, the value is copied into the supplied
 * data-source-name buffer.
 */
static void
apply_attribute_entry(const char *entry,
					  HonoredAttribute *honored_attributes,
					  size_t honored_attribute_count,
					  char *data_source_name,
					  size_t data_source_name_size)
{
	const char *equals_sign = strchr(entry, '=');
	if (equals_sign == NULL)
		return;					/* Not a KEYWORD=VALUE pair; ignore. */

	size_t keyword_length = (size_t) (equals_sign - entry);
	const char *value = equals_sign + 1;

	/* Match the keyword up to '=' case-insensitively, as ODBC keywords are. */
	if (keyword_length == strlen(ATTRIBUTE_KEYWORD_DATA_SOURCE_NAME)
		&& _strnicmp(entry, ATTRIBUTE_KEYWORD_DATA_SOURCE_NAME, keyword_length) == 0)
	{
		copy_string_bounded(data_source_name, data_source_name_size, value);
		return;
	}

	for (size_t i = 0; i < honored_attribute_count; i++)
	{
		HonoredAttribute *attribute = &honored_attributes[i];
		if (keyword_length == strlen(attribute->keyword)
			&& _strnicmp(entry, attribute->keyword, keyword_length) == 0)
		{
			copy_string_bounded(attribute->value, sizeof(attribute->value), value);
			attribute->was_supplied = TRUE;
			return;
		}
	}
}

/*
 * Register a data source and persist its honored connection keywords.
 *
 * attributes is the ODBC attribute list: a run of NUL-terminated
 * "KEYWORD=VALUE" strings, itself terminated by an empty string (a double
 * NUL). driver_description is the driver's registered name (as it appears in
 * the [ODBC Drivers] section), needed by SQLWriteDSNToIni to associate the
 * DSN with this driver.
 *
 * Returns TRUE on success. Returns FALSE (and posts an installer error) if no
 * data source name was supplied or the DSN could not be written.
 */
static BOOL
register_data_source(const char *driver_description, const char *attributes)
{
	HonoredAttribute honored_attributes[] = {
		{ ATTRIBUTE_KEYWORD_DESCRIPTION,      "", FALSE },
		{ ATTRIBUTE_KEYWORD_SERVERNAME,       "", FALSE },
		{ ATTRIBUTE_KEYWORD_PORT,             "", FALSE },
		{ ATTRIBUTE_KEYWORD_DATABASE,         "", FALSE },
		{ ATTRIBUTE_KEYWORD_USERNAME,         "", FALSE },
		{ ATTRIBUTE_KEYWORD_UID,              "", FALSE },
		{ ATTRIBUTE_KEYWORD_PASSWORD,         "", FALSE },
		{ ATTRIBUTE_KEYWORD_SSLMODE,          "", FALSE },
		{ ATTRIBUTE_KEYWORD_APPLICATION_NAME, "", FALSE },
		{ ATTRIBUTE_KEYWORD_TIMEOUT,          "", FALSE },
	};
	const size_t honored_attribute_count =
		sizeof(honored_attributes) / sizeof(honored_attributes[0]);

	char data_source_name[MAX_DATA_SOURCE_NAME_LENGTH] = "";

	/* Walk the double-NUL-terminated attribute list one entry at a time. */
	if (attributes != NULL)
	{
		for (const char *entry = attributes;
			 *entry != '\0';
			 entry += strlen(entry) + 1)
		{
			apply_attribute_entry(entry, honored_attributes,
								   honored_attribute_count,
								   data_source_name, sizeof(data_source_name));
		}
	}

	/* A data source name is mandatory; without it there is nothing to write. */
	if (data_source_name[0] == '\0')
	{
		SQLPostInstallerError(ODBC_ERROR_INVALID_KEYWORD_VALUE,
							  "A data source name (DSN) is required.");
		return FALSE;
	}

	/*
	 * Create/overwrite the DSN entry and bind it to this driver. This must
	 * happen before writing individual keywords so the section exists.
	 */
	if (!SQLWriteDSNToIni(data_source_name, driver_description))
	{
		SQLPostInstallerError(ODBC_ERROR_REQUEST_FAILED,
							  "Failed to register the data source.");
		return FALSE;
	}

	/* Persist each keyword the caller actually supplied. */
	for (size_t i = 0; i < honored_attribute_count; i++)
	{
		const HonoredAttribute *attribute = &honored_attributes[i];
		if (!attribute->was_supplied)
			continue;

		SQLWritePrivateProfileString(data_source_name, attribute->keyword,
									 attribute->value, ODBC_INI_FILE_NAME);
	}

	return TRUE;
}

/*
 * Convert a wide (UTF-16) string to a newly allocated ANSI string using the
 * active code page. wide_length is the number of wchar_t elements to convert,
 * or -1 for a NUL-terminated string. For an attribute list (which contains
 * embedded NULs) pass the exact element count including the terminating empty
 * string, so the embedded NULs are converted too.
 *
 * Returns a malloc'd buffer the caller must free, or NULL on failure.
 */
static char *
convert_wide_to_ansi(const wchar_t *source, int wide_length)
{
	int required_bytes = WideCharToMultiByte(CP_ACP, 0, source, wide_length,
											 NULL, 0, NULL, NULL);
	if (required_bytes <= 0)
		return NULL;

	char *ansi = (char *) malloc((size_t) required_bytes);
	if (ansi == NULL)
		return NULL;

	if (WideCharToMultiByte(CP_ACP, 0, source, wide_length,
							ansi, required_bytes, NULL, NULL) <= 0)
	{
		free(ansi);
		return NULL;
	}

	return ansi;
}

/*
 * Number of wchar_t elements in a double-NUL-terminated ODBC attribute list,
 * counting every embedded NUL and the final terminating empty string. This
 * length lets convert_wide_to_ansi translate the whole block, embedded NULs
 * included, in a single call.
 */
static int
wide_attribute_list_length(const wchar_t *attributes)
{
	const wchar_t *cursor = attributes;
	while (*cursor != L'\0')
		cursor += wcslen(cursor) + 1;

	/* +1 for the final empty string that terminates the list. */
	return (int) (cursor - attributes) + 1;
}

/*
 * Maps each editable dialog control to the DSN keyword it reads/writes. The
 * dialog procedure walks this table to load values on open and to write them
 * back on OK, so adding a field is a one-line change here plus a control in
 * setup_dialog.rc.
 *
 * The data-source-name control (IDC_EDIT_DATA_SOURCE_NAME) is intentionally
 * NOT in this table: it names the DSN section itself rather than a value
 * within it, so it is handled separately — validated, then used as the section
 * name for every write below.
 */
typedef struct DsnDialogField
{
	int         control_id;
	const char *keyword;
} DsnDialogField;

static const DsnDialogField dsn_dialog_fields[] = {
	{ IDC_EDIT_DESCRIPTION,      ATTRIBUTE_KEYWORD_DESCRIPTION },
	{ IDC_EDIT_SERVERNAME,       ATTRIBUTE_KEYWORD_SERVERNAME },
	{ IDC_EDIT_PORT,             ATTRIBUTE_KEYWORD_PORT },
	{ IDC_EDIT_DATABASE,         ATTRIBUTE_KEYWORD_DATABASE },
	{ IDC_EDIT_USERNAME,         ATTRIBUTE_KEYWORD_USERNAME },
	{ IDC_EDIT_PASSWORD,         ATTRIBUTE_KEYWORD_PASSWORD },
	{ IDC_EDIT_SSLMODE,          ATTRIBUTE_KEYWORD_SSLMODE },
	{ IDC_EDIT_APPLICATION_NAME, ATTRIBUTE_KEYWORD_APPLICATION_NAME },
	{ IDC_EDIT_TIMEOUT,          ATTRIBUTE_KEYWORD_TIMEOUT },
};

static const size_t dsn_dialog_field_count =
	sizeof(dsn_dialog_fields) / sizeof(dsn_dialog_fields[0]);

/*
 * Context threaded through DialogBoxParamW into the dialog procedure via
 * lParam. Carries what the procedure needs that is not held in a control: the
 * name of the DSN being edited (used to load existing values when the dialog
 * opens) and the driver name (needed to bind the DSN to this driver on save).
 */
typedef struct DsnDialogContext
{
	const char *driver_description;
	char        data_source_name[MAX_DATA_SOURCE_NAME_LENGTH];
} DsnDialogContext;

/*
 * Populate the dialog's edit controls from the DSN currently named in the
 * context. A keyword absent from the DSN reads back as the "" default, which
 * correctly leaves the control blank — e.g. for a brand new DSN where nothing
 * has been saved yet.
 */
static void
load_dialog_from_dsn(HWND dialog, const DsnDialogContext *context)
{
	SetDlgItemTextA(dialog, IDC_EDIT_DATA_SOURCE_NAME,
					context->data_source_name);

	for (size_t i = 0; i < dsn_dialog_field_count; i++)
	{
		const DsnDialogField *field = &dsn_dialog_fields[i];
		char value[MAX_ATTRIBUTE_VALUE_LENGTH] = "";

		SQLGetPrivateProfileString(context->data_source_name,
								   field->keyword, "",
								   value, sizeof(value),
								   ODBC_INI_FILE_NAME);
		SetDlgItemTextA(dialog, field->control_id, value);
	}
}

/*
 * Read every control back out of the dialog and persist it to the named DSN.
 *
 * The DSN section is (re)created and bound to this driver first via
 * SQLWriteDSNToIni, so the section is guaranteed to exist before the per-key
 * writes; this also covers the "add a new DSN" case. Every honored key is
 * written — including ones left blank — so clearing a field in the dialog
 * clears the stale value in the DSN rather than leaving it behind.
 *
 * data_source_name is the (possibly edited) name typed in the dialog.
 * context->data_source_name is the name the dialog opened with. When the user
 * renames a DSN in a Configure request the two differ; in that case the old
 * section is removed after the new one is written, so a rename does not leave
 * an orphaned duplicate behind (mirrors upstream setup.c SetDSNAttributes).
 *
 * Returns TRUE on success, FALSE (with an installer error posted) if the DSN
 * section could not be created.
 */
static BOOL
save_dialog_to_dsn(HWND dialog, const DsnDialogContext *context,
				   const char *data_source_name)
{
	if (!SQLWriteDSNToIni(data_source_name, context->driver_description))
	{
		SQLPostInstallerError(ODBC_ERROR_REQUEST_FAILED,
							  "Failed to register the data source.");
		return FALSE;
	}

	for (size_t i = 0; i < dsn_dialog_field_count; i++)
	{
		const DsnDialogField *field = &dsn_dialog_fields[i];
		char value[MAX_ATTRIBUTE_VALUE_LENGTH] = "";

		GetDlgItemTextA(dialog, field->control_id, value, sizeof(value));
		SQLWritePrivateProfileString(data_source_name, field->keyword,
									 value, ODBC_INI_FILE_NAME);
	}

	/*
	 * If the user renamed the DSN, delete the original section now that the
	 * renamed one is fully written. DSN names are compared case-insensitively
	 * because the ODBC installer treats them that way.
	 */
	if (context->data_source_name[0] != '\0'
		&& _stricmp(context->data_source_name, data_source_name) != 0)
	{
		SQLRemoveDSNFromIni(context->data_source_name);
	}

	return TRUE;
}

/**
 * DialogProc — window procedure for the DSN configuration dialog.
 *
 * Not an ODBC entry point, but it drives the ODBC installer read/write APIs on
 * the user's behalf. On WM_INITDIALOG it loads the named DSN's current values
 * into the controls; on IDOK it validates the DSN name and writes every field
 * back; on IDCANCEL it closes without touching the DSN.
 *
 * @param dialog   Handle to the dialog window.
 * @param message  Window message (WM_INITDIALOG, WM_COMMAND, ...).
 * @param wParam   Message-specific parameter (control/notification on WM_COMMAND).
 * @param lParam   On WM_INITDIALOG, the DsnDialogContext pointer passed to
 *                 DialogBoxParamW.
 * @return TRUE if the message was handled, FALSE to let the default dialog
 *         procedure handle it.
 *
 * See: https://learn.microsoft.com/en-us/windows/win32/api/winuser/nc-winuser-dlgproc
 */
static INT_PTR CALLBACK
DialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
		case WM_INITDIALOG:
		{
			DsnDialogContext *context = (DsnDialogContext *) lParam;

			/* Stash the context so later messages (IDOK) can reach it. */
			SetWindowLongPtrW(dialog, DWLP_USER, (LONG_PTR) context);

			/* Enforce the installer's DSN-name length limit in the UI too. */
			SendDlgItemMessageW(dialog, IDC_EDIT_DATA_SOURCE_NAME,
								EM_LIMITTEXT,
								(WPARAM) (ODBC_MAX_DSN_NAME_LENGTH - 1), 0);

			load_dialog_from_dsn(dialog, context);
			return TRUE;		/* Let the dialog manager set initial focus. */
		}

		case WM_COMMAND:
			switch (LOWORD(wParam))
			{
				case IDOK:
				{
					DsnDialogContext *context = (DsnDialogContext *)
						GetWindowLongPtrW(dialog, DWLP_USER);
					char data_source_name[MAX_DATA_SOURCE_NAME_LENGTH] = "";

					GetDlgItemTextA(dialog, IDC_EDIT_DATA_SOURCE_NAME,
									data_source_name, sizeof(data_source_name));

					/*
					 * A DSN with no name cannot be written. Warn and keep the
					 * dialog open so the user can correct it rather than
					 * silently discarding their input.
					 */
					if (data_source_name[0] == '\0')
					{
						MessageBoxW(dialog,
									L"A data source name is required.",
									L"psqlodbc2 DSN Setup",
									MB_OK | MB_ICONEXCLAMATION);
						return TRUE;
					}

					if (!save_dialog_to_dsn(dialog, context, data_source_name))
						return TRUE;	/* Error already posted; stay open. */

					EndDialog(dialog, IDOK);
					return TRUE;
				}

				case IDCANCEL:
					/* Discard edits: close without writing anything. */
					EndDialog(dialog, IDCANCEL);
					return TRUE;
			}
			return FALSE;

		default:
			return FALSE;
	}
}

/*
 * Present the modal DSN configuration dialog and, if the user accepts it,
 * persist the DSN (the write happens inside the dialog procedure on IDOK).
 *
 * initial_data_source_name is the DSN being configured (from the "DSN="
 * attribute), or empty for a fresh Add. Returns TRUE if the user clicked OK
 * and the DSN was written, FALSE if they cancelled or the dialog failed to
 * open.
 */
static BOOL
show_config_dialog(HWND parent_window, const char *driver_description,
				   const char *initial_data_source_name)
{
	DsnDialogContext context;

	context.driver_description = driver_description;
	copy_string_bounded(context.data_source_name,
						sizeof(context.data_source_name),
						initial_data_source_name != NULL
							? initial_data_source_name : "");

	INT_PTR result = DialogBoxParamW(setup_module_instance,
									 MAKEINTRESOURCEW(IDD_CONFIG_DIALOG),
									 parent_window, DialogProc,
									 (LPARAM) &context);

	/*
	 * DialogBoxParamW returns -1 when the dialog could not be created (e.g. a
	 * missing/corrupt resource). Surface that to the Administrator instead of
	 * failing silently; IDOK/IDCANCEL are the normal user outcomes.
	 */
	if (result == -1)
	{
		SQLPostInstallerError(ODBC_ERROR_GENERAL_ERR,
							  "Failed to open the DSN configuration dialog.");
		return FALSE;
	}

	return result == IDOK;
}

/**
 * ConfigDSN — add, configure, or remove a data source (ANSI entry point).
 *
 * Called by the ODBC Data Source Administrator when the user manages a DSN
 * for this driver.
 *
 * @param parent_window       Parent window for the dialog. When NULL the
 *                            request is silent: the DSN is written directly
 *                            from the attributes with no UI.
 * @param request             One of ODBC_ADD_DSN, ODBC_CONFIG_DSN,
 *                            ODBC_REMOVE_DSN.
 * @param driver_description  Registered driver name, associated with the DSN.
 * @param attributes          Double-NUL-terminated list of "KEYWORD=VALUE"
 *                            pairs; the "DSN" keyword names the data source.
 * @return TRUE on success, FALSE on failure (with an installer error posted).
 *
 * See: https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/configdsn-function
 */
BOOL INSTAPI
ConfigDSN(HWND parent_window,
		  WORD request,
		  LPCSTR driver_description,
		  LPCSTR attributes)
{
	switch (request)
	{
		case ODBC_REMOVE_DSN:
		{
			/*
			 * Removal only needs the DSN name from the attribute list. Reuse
			 * the parser to extract it without honoring any other keyword.
			 */
			char data_source_name[MAX_DATA_SOURCE_NAME_LENGTH] = "";
			if (attributes != NULL)
			{
				for (const char *entry = attributes;
					 *entry != '\0';
					 entry += strlen(entry) + 1)
				{
					apply_attribute_entry(entry, NULL, 0,
										   data_source_name,
										   sizeof(data_source_name));
				}
			}

			if (data_source_name[0] == '\0')
			{
				SQLPostInstallerError(ODBC_ERROR_INVALID_KEYWORD_VALUE,
									  "A data source name (DSN) is required.");
				return FALSE;
			}

			return SQLRemoveDSNFromIni(data_source_name);
		}

		case ODBC_ADD_DSN:
		case ODBC_CONFIG_DSN:
		{
			/*
			 * With a parent window, show the interactive dialog so the user
			 * can review and edit every field. Without one (a silent
			 * request), fall back to writing the DSN directly from the
			 * attribute string — the no-UI path used by tools that configure
			 * DSNs programmatically.
			 */
			if (parent_window == NULL)
				return register_data_source(driver_description, attributes);

			/* Pre-fill the dialog with the DSN named in the attributes. */
			char data_source_name[MAX_DATA_SOURCE_NAME_LENGTH] = "";
			if (attributes != NULL)
			{
				for (const char *entry = attributes;
					 *entry != '\0';
					 entry += strlen(entry) + 1)
				{
					apply_attribute_entry(entry, NULL, 0,
										   data_source_name,
										   sizeof(data_source_name));
				}
			}

			return show_config_dialog(parent_window, driver_description,
									  data_source_name);
		}

		default:
			SQLPostInstallerError(ODBC_ERROR_INVALID_REQUEST_TYPE,
								  "Unsupported ConfigDSN request type.");
			return FALSE;
	}
}

/**
 * ConfigDSNW — add, configure, or remove a data source (Unicode entry point).
 *
 * The Unicode variant the ODBC Administrator prefers on modern Windows. The
 * driver's DSN keywords and values are handled as narrow (ANSI) strings, so
 * this converts its arguments and delegates to ConfigDSN, keeping a single
 * implementation of the DSN read/write logic.
 *
 * @param parent_window       Parent window for the dialog; passed straight
 *                            through to ConfigDSN, where a non-NULL handle
 *                            drives the interactive prompt and NULL means a
 *                            silent write.
 * @param request             One of ODBC_ADD_DSN, ODBC_CONFIG_DSN,
 *                            ODBC_REMOVE_DSN.
 * @param driver_description  Registered driver name (wide), associated with
 *                            the DSN.
 * @param attributes          Double-NUL-terminated wide list of
 *                            "KEYWORD=VALUE" pairs.
 * @return TRUE on success, FALSE on failure.
 *
 * See: https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/configdsn-function
 */
BOOL INSTAPI
ConfigDSNW(HWND parent_window,
		   WORD request,
		   LPCWSTR driver_description,
		   LPCWSTR attributes)
{
	char *ansi_driver_description = NULL;
	char *ansi_attributes = NULL;
	BOOL  result = FALSE;

	if (driver_description != NULL)
	{
		ansi_driver_description = convert_wide_to_ansi(driver_description, -1);
		if (ansi_driver_description == NULL)
		{
			SQLPostInstallerError(ODBC_ERROR_GENERAL_ERR,
								  "Failed to convert the driver name.");
			return FALSE;
		}
	}

	if (attributes != NULL)
	{
		int attribute_element_count = wide_attribute_list_length(attributes);
		ansi_attributes = convert_wide_to_ansi(attributes, attribute_element_count);
		if (ansi_attributes == NULL)
		{
			free(ansi_driver_description);
			SQLPostInstallerError(ODBC_ERROR_GENERAL_ERR,
								  "Failed to convert the DSN attributes.");
			return FALSE;
		}
	}

	result = ConfigDSN(parent_window, request,
					   ansi_driver_description, ansi_attributes);

	free(ansi_attributes);
	free(ansi_driver_description);
	return result;
}

/**
 * ConfigDriver — perform driver-specific install/configure/remove actions.
 *
 * The ODBC installer calls this for driver-level requests. This driver has no
 * driver-specific configuration to perform, so it succeeds without action; it
 * still clears the output message buffer as the contract requires.
 *
 * @param parent_window       Parent window for any UI (unused).
 * @param request             Driver request type (e.g. ODBC_INSTALL_DRIVER).
 * @param driver_description  Registered driver name.
 * @param arguments           Request-specific arguments (unused).
 * @param message_buffer      Buffer for an output message; cleared here.
 * @param message_buffer_size Capacity of message_buffer in characters.
 * @param message_length_out  Receives the output message length (0 here).
 * @return TRUE (success).
 *
 * See: https://learn.microsoft.com/en-us/sql/odbc/reference/syntax/configdriver-function
 */
BOOL INSTAPI
ConfigDriver(HWND parent_window,
			 WORD request,
			 LPCSTR driver_description,
			 LPCSTR arguments,
			 LPSTR message_buffer,
			 WORD message_buffer_size,
			 WORD *message_length_out)
{
	(void) parent_window;
	(void) request;
	(void) driver_description;
	(void) arguments;

	if (message_buffer != NULL && message_buffer_size > 0)
		message_buffer[0] = '\0';
	if (message_length_out != NULL)
		*message_length_out = 0;

	return TRUE;
}

/**
 * DllMain — DLL entry point.
 *
 * Captures this DLL's own module handle on load so the dialog code can pass it
 * to DialogBoxParamW (which needs the instance that owns the dialog template
 * resource). No other per-process or per-thread setup is required, so the
 * remaining notifications are ignored.
 *
 * @param module_instance  Handle to this DLL's module.
 * @param reason           Reason for the call (attach/detach, process/thread).
 * @param reserved         Reserved by Windows; unused.
 * @return TRUE to allow the DLL to load.
 *
 * See: https://learn.microsoft.com/en-us/windows/win32/dlls/dllmain
 */
BOOL WINAPI
DllMain(HINSTANCE module_instance, DWORD reason, LPVOID reserved)
{
	(void) reserved;

	if (reason == DLL_PROCESS_ATTACH)
	{
		setup_module_instance = module_instance;

		/* No per-thread callbacks are needed; suppress them to save work. */
		DisableThreadLibraryCalls(module_instance);
	}

	return TRUE;
}

#endif /* _WIN32 */
