/* sg-zip -- the ZIP format: our own inflate, deflate, CRC-32, reader and writer.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef SG_ZIPCORE_H
#define SG_ZIPCORE_H
#include <windows.h>

enum {
    ZE_OK = 0,
    ZE_OPEN,        /* the file cannot be opened or mapped */
    ZE_FORMAT,      /* not a ZIP file, or its directory is damaged */
    ZE_METHOD,      /* a compression method we do not have */
    ZE_ENCRYPTED,   /* password protected */
    ZE_DATA,        /* the compressed data is damaged */
    ZE_CRC,         /* the data does not match its checksum */
    ZE_NOMEM,
    ZE_UNSAFE,      /* a name that would land outside the destination */
    ZE_WRITE,       /* the destination cannot be written */
    ZE_TOOBIG,      /* past what this writer supports (4 GB, 65535 entries) */
    ZE_CANCELLED,
};

typedef struct {
    WCHAR   *name;          /* as stored, '/' separated; a directory ends in '/' */
    BOOL     dir;
    WORD     method, flags, dostime, dosdate;
    DWORD    crc;
    ULONGLONG csize, usize, lho;
} zentry;

typedef struct {
    HANDLE     file, map;
    const BYTE *base;
    ULONGLONG  size;
    zentry    *e;
    int        n;
} zarchive;

DWORD crc32_update(DWORD crc, const BYTE *p, size_t n);

/* raw DEFLATE (RFC 1951). inflate_buf fills exactly outn bytes or fails. */
int inflate_buf(const BYTE *in, size_t inn, BYTE *out, size_t outn);
int deflate_buf(const BYTE *in, size_t inn, BYTE **out, size_t *outn);

int  zip_open(zarchive *z, const WCHAR *path);
void zip_close(zarchive *z);
/* the entry's contents, checked against its CRC; free() the result */
int  zip_read(zarchive *z, int i, BYTE **out, size_t *len);
/* a stored name as a relative Windows path under the destination, or FALSE
 * when it is absolute, has a drive, climbs with "..", or names a stream */
BOOL zip_safe_path(const WCHAR *name, WCHAR *out, int cch);

typedef struct zwriter zwriter;
zwriter *zw_open(const WCHAR *path);
int  zw_add_file(zwriter *w, const WCHAR *arcname, const WCHAR *src);
int  zw_add_dir(zwriter *w, const WCHAR *arcname, const FILETIME *ft);
int  zw_add_mem(zwriter *w, const WCHAR *arcname, const void *data, DWORD n, BOOL store);
int  zw_close(zwriter *w, BOOL keep);   /* keep=FALSE discards the partial file */

const WCHAR *zip_strerror(int e);
#endif
