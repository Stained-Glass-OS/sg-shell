/* sg-pdf -- PDF Viewer: talking to the Linux half (sg-session's sg-pdf).
 *
 * Wine gives a Windows program no AF_UNIX and no pipe to a native program,
 * so the viewer re-launches itself as `sg-pdf --bridge wine <itself>
 * --bridged ...` and its standard handles are then pipes to poppler. One
 * request a line; each answer a line, "OK ... bytes=N" or "ERR kind msg",
 * then N bytes (see sg-pdf's header for the protocol).
 *
 * The pipe is shared by the UI thread (text, links, search, outline: small
 * answers) and the render thread (bitmaps); a critical section keeps each
 * request and its answer together. The render thread reads a page's pixels
 * straight into the DIB section it hands to the view.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "pdf.h"

static HANDLE g_in, g_out;
static CRITICAL_SECTION g_io;
static BYTE g_rbuf[65536];
static DWORD g_rlen, g_rpos;
static BOOL g_broken;

void to_utf8(const WCHAR *w, char *out, int cap)
{
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap, NULL, NULL)) out[0] = 0;
    out[cap - 1] = 0;
}

WCHAR *from_utf8(const char *s, int len)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s, len, NULL, 0);
    WCHAR *w = malloc((n + 1) * sizeof(WCHAR));
    if (!w) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, s, len, w, n);
    w[n] = 0;
    return w;
}

BOOL br_start(void)
{
    g_in = GetStdHandle(STD_INPUT_HANDLE);
    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    InitializeCriticalSection(&g_io);
    return g_in && g_in != INVALID_HANDLE_VALUE && g_out && g_out != INVALID_HANDLE_VALUE;
}

static BOOL fill(void)
{
    DWORD got = 0;
    if (g_broken) return FALSE;
    if (!ReadFile(g_in, g_rbuf, sizeof(g_rbuf), &got, NULL) || !got) {
        g_broken = TRUE;
        return FALSE;
    }
    g_rlen = got;
    g_rpos = 0;
    return TRUE;
}

static BOOL read_exact(BYTE *dst, DWORD n)
{
    while (n) {
        DWORD take;
        if (g_rpos == g_rlen) {
            /* a big read goes straight to its destination */
            if (n >= sizeof(g_rbuf)) {
                DWORD got = 0;
                if (!ReadFile(g_in, dst, n, &got, NULL) || !got) { g_broken = TRUE; return FALSE; }
                dst += got;
                n -= got;
                continue;
            }
            if (!fill()) return FALSE;
        }
        take = min(n, g_rlen - g_rpos);
        memcpy(dst, g_rbuf + g_rpos, take);
        g_rpos += take;
        dst += take;
        n -= take;
    }
    return TRUE;
}

static BOOL read_line(char *out, int cap)
{
    int n = 0;
    for (;;) {
        if (g_rpos == g_rlen && !fill()) return FALSE;
        char c = (char)g_rbuf[g_rpos++];
        if (c == '\n') break;
        if (n < cap - 1) out[n++] = c;
    }
    out[n] = 0;
    return TRUE;
}

static BOOL write_line(const char *line)
{
    DWORD len = (DWORD)strlen(line), done = 0;
    char *buf = malloc(len + 2);
    BOOL ok;
    if (!buf) return FALSE;
    memcpy(buf, line, len);
    buf[len] = '\n';
    ok = WriteFile(g_out, buf, len + 1, &done, NULL) && done == len + 1;
    free(buf);
    if (!ok) g_broken = TRUE;
    return ok;
}

const char *br_field(const char *head, const char *key, char *buf, int cap)
{
    size_t kl = strlen(key);
    const char *p = head;
    while ((p = strstr(p, key))) {
        if ((p == head || p[-1] == ' ') && p[kl] == '=') {
            int n = 0;
            p += kl + 1;
            while (*p && *p != ' ' && n < cap - 1) buf[n++] = *p++;
            buf[n] = 0;
            return buf;
        }
        p += kl;
    }
    return NULL;
}

static void gone(void)
{
    if (g_view) PostMessageW(g_view, WM_APP_GONE, 0, 0);
}

/* 1: OK (the payload, if any, in *payload: free it), 0: ERR (head says why), -1: the pipe is gone */
int br_request(const char *line, char *head, int headcap, BYTE **payload, DWORD *len)
{
    char num[32];
    DWORD n = 0;
    int rc = -1;
    if (payload) *payload = NULL;
    if (len) *len = 0;
    head[0] = 0;
    if (!g.bridged) { lstrcpynA(head, "ERR failed not connected", headcap); return -1; }
    EnterCriticalSection(&g_io);
    if (write_line(line) && read_line(head, headcap)) {
        if (br_field(head, "bytes", num, sizeof(num))) n = strtoul(num, NULL, 10);
        if (n) {
            BYTE *buf = malloc(n + 1);
            if (buf && read_exact(buf, n)) {
                buf[n] = 0;
                if (payload) *payload = buf; else free(buf);
                if (len) *len = n;
                rc = strncmp(head, "OK", 2) ? 0 : 1;
            } else {
                BYTE sink[4096];
                free(buf);
                /* keep the stream in step even if we had no memory */
                while (n && !g_broken) { DWORD t = min(n, sizeof(sink)); if (!read_exact(sink, t)) break; n -= t; }
            }
        } else rc = strncmp(head, "OK", 2) ? 0 : 1;
    }
    LeaveCriticalSection(&g_io);
    if (rc < 0) gone();
    return rc;
}

BOOL br_request_into_dib(const char *line, HBITMAP *out, int *w, int *h)
{
    char head[256], num[32];
    BITMAPINFO bi = { 0 };
    void *bits = NULL;
    HBITMAP bmp = NULL;
    DWORD n = 0;
    BOOL ok = FALSE, io = FALSE;
    *out = NULL;
    if (!g.bridged) return FALSE;
    EnterCriticalSection(&g_io);
    if (write_line(line) && read_line(head, sizeof(head))) {
        io = TRUE;
        if (br_field(head, "bytes", num, sizeof(num))) n = strtoul(num, NULL, 10);
        if (!strncmp(head, "OK", 2) && br_field(head, "w", num, sizeof(num))) {
            *w = atoi(num);
            *h = br_field(head, "h", num, sizeof(num)) ? atoi(num) : 0;
            if (*w > 0 && *h > 0 && (DWORD)(*w) * (DWORD)(*h) * 4 == n) {
                bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
                bi.bmiHeader.biWidth = *w;
                bi.bmiHeader.biHeight = -*h;
                bi.bmiHeader.biPlanes = 1;
                bi.bmiHeader.biBitCount = 32;
                bi.bmiHeader.biCompression = BI_RGB;
                bmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
            }
        }
        if (bmp && bits) {
            ok = read_exact(bits, n);
            if (!ok) io = FALSE;
        } else {
            BYTE sink[8192];
            while (n && !g_broken) { DWORD t = min(n, sizeof(sink)); if (!read_exact(sink, t)) { io = FALSE; break; } n -= t; }
        }
    }
    LeaveCriticalSection(&g_io);
    if (!io) gone();
    if (!ok && bmp) { DeleteObject(bmp); bmp = NULL; }
    *out = bmp;
    return ok;
}

/* ---- the render thread ------------------------------------------------------------------------- */

typedef struct { int page, rot, gen; double scale; BOOL thumb; } job_t;
typedef struct { int page, rot, gen, w, h; double scale; BOOL thumb; HBITMAP bmp; } result_t;

#define MAX_JOBS 256
static job_t g_jobs[MAX_JOBS];
static int g_njobs;
static job_t g_inflight;
static BOOL g_busy;
static CRITICAL_SECTION g_jlock;
static HANDLE g_jevent;

static BOOL same_job(const job_t *a, const job_t *b)
{
    return a->page == b->page && a->thumb == b->thumb && a->rot == b->rot && a->gen == b->gen &&
           fabs(a->scale - b->scale) < 1e-6;
}

void render_want(int page, double scale, int rot, BOOL thumb)
{
    job_t j = { page, rot, g.generation, scale, thumb };
    int i;
    EnterCriticalSection(&g_jlock);
    if (g_busy && same_job(&g_inflight, &j)) goto done;
    for (i = 0; i < g_njobs; i++) if (same_job(&g_jobs[i], &j)) goto done;
    if (g_njobs < MAX_JOBS) g_jobs[g_njobs++] = j;
    SetEvent(g_jevent);
done:
    LeaveCriticalSection(&g_jlock);
}

/* The view says again what it wants at each paint; what scrolled away is dropped. */
void render_clear_wants(BOOL thumbs)
{
    int i, n = 0;
    EnterCriticalSection(&g_jlock);
    for (i = 0; i < g_njobs; i++) if (g_jobs[i].thumb != thumbs) g_jobs[n++] = g_jobs[i];
    g_njobs = n;
    LeaveCriticalSection(&g_jlock);
}

static DWORD WINAPI render_thread(void *arg)
{
    (void)arg;
    for (;;) {
        job_t j;
        int i, pick = -1;
        char line[128];
        result_t *r;
        WaitForSingleObject(g_jevent, INFINITE);
        for (;;) {
            EnterCriticalSection(&g_jlock);
            /* pages before thumbnails, each in the order asked */
            for (i = 0; i < g_njobs && pick < 0; i++) if (!g_jobs[i].thumb) pick = i;
            if (pick < 0 && g_njobs) pick = 0;
            if (pick < 0) { g_busy = FALSE; LeaveCriticalSection(&g_jlock); break; }
            j = g_jobs[pick];
            memmove(&g_jobs[pick], &g_jobs[pick + 1], (g_njobs - pick - 1) * sizeof(job_t));
            g_njobs--;
            g_inflight = j;
            g_busy = TRUE;
            LeaveCriticalSection(&g_jlock);
            pick = -1;
            if (j.gen != g.generation) continue;
#ifdef SG_MUTANT_FIRSTPAGE
            snprintf(line, sizeof(line), "render\t%d\t%.5f\t%d", 0, j.scale, j.rot);
#else
            snprintf(line, sizeof(line), "render\t%d\t%.5f\t%d", j.page, j.scale, j.rot);
#endif
            r = calloc(1, sizeof(*r));
            if (!r) continue;
            r->page = j.page; r->rot = j.rot; r->gen = j.gen; r->scale = j.scale; r->thumb = j.thumb;
            br_request_into_dib(line, &r->bmp, &r->w, &r->h);
            if (!PostMessageW(g_view, WM_APP_RENDERED, 0, (LPARAM)r)) {
                if (r->bmp) DeleteObject(r->bmp);
                free(r);
            }
        }
    }
    return 0;
}

void render_init(void)
{
    InitializeCriticalSection(&g_jlock);
    g_jevent = CreateEventW(NULL, FALSE, FALSE, NULL);
}

void render_start_thread(void)
{
    CloseHandle(CreateThread(NULL, 0, render_thread, NULL, 0, NULL));
}

/* the view installs what the thread rendered (the UI thread) */
LRESULT bridge_on_rendered(LPARAM lp)
{
    result_t *r = (result_t *)lp;
    view_rendered(r->page, r->thumb, r->scale, r->rot, r->gen, r->bmp, r->w, r->h);
    free(r);
    return 0;
}
