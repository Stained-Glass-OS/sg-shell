/* SG Office -- Get SG Office: shared declarations.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef SG_OFFICE_SETUP_H
#define SG_OFFICE_SETUP_H
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commctrl.h>
#include <wininet.h>
#include <bcrypt.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

BOOL payload_dir(WCHAR *out, int cch);
void payload_value(const WCHAR *key, WCHAR *out, int cch);
BOOL office_program_dir(WCHAR *out, int cch);
BOOL payload_current(void);
BOOL payload_apply(WCHAR *err, int cch);
BOOL verify(const WCHAR *file, WCHAR *err, int cch);
#endif
