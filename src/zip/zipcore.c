/* sg-zip -- the ZIP format, written from the published specifications:
 * RFC 1951 (DEFLATE) and PKWARE's APPNOTE (the container, ZIP64 reading).
 * No zlib, no other library: inflate, deflate (LZ77 over hash chains, dynamic
 * Huffman blocks with fixed and stored as fall-backs), CRC-32, the archive
 * reader and writer are all here.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <stdlib.h>
#include <string.h>
#include "zipcore.h"

/* ---- CRC-32 (the reflected 0xEDB88320 polynomial) ---------------------------------------- */
static DWORD crc_table[256];

DWORD crc32_update(DWORD crc, const BYTE *p, size_t n)
{
    if (!crc_table[1])
    {
        DWORD c;
        int i, k;
        for (i = 0; i < 256; i++)
        {
            for (c = i, k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >> 1) : c >> 1;
            crc_table[i] = c;
        }
    }
    crc = ~crc;
    while (n--) crc = crc_table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

/* ---- the tables RFC 1951 defines ---------------------------------------------------------- */
static const WORD len_base[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                   35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const BYTE len_extra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const WORD dist_base[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
                                    8193, 12289, 16385, 24577 };
static const BYTE dist_extra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                     7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };
static const BYTE clen_order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };

/* ---- inflate ------------------------------------------------------------------------------ */
typedef struct {
    const BYTE *in;
    size_t inn, pos;
    DWORD bits;
    int nbits;
    BYTE *out;
    size_t outn, outpos;
} istate;

typedef struct { short count[16]; short symbol[288]; } huff;

static int need_bits(istate *s, int n, unsigned *v)
{
    DWORD b = s->bits;
    while (s->nbits < n)
    {
        if (s->pos >= s->inn) return -1;
        b |= (DWORD)s->in[s->pos++] << s->nbits;
        s->nbits += 8;
    }
    *v = b & ((1u << n) - 1);
    s->bits = b >> n;
    s->nbits -= n;
    return 0;
}

/* canonical code from lengths; <0 when over-subscribed */
static int huff_build(huff *h, const BYTE *len, int n)
{
    short offs[16];
    int i, left = 1;
    memset(h->count, 0, sizeof(h->count));
    for (i = 0; i < n; i++) h->count[len[i]]++;
    if (h->count[0] == n) return 0;
    for (i = 1; i < 16; i++)
    {
        left <<= 1;
        left -= h->count[i];
        if (left < 0) return -1;
    }
    offs[1] = 0;
    for (i = 1; i < 15; i++) offs[i + 1] = offs[i] + h->count[i];
    for (i = 0; i < n; i++) if (len[i]) h->symbol[offs[len[i]]++] = (short)i;
    return left;
}

static int huff_decode(istate *s, const huff *h)
{
    int code = 0, first = 0, index = 0, len;
    unsigned bit;
    for (len = 1; len < 16; len++)
    {
        int count;
        if (need_bits(s, 1, &bit)) return -1;
        code |= bit;
        count = h->count[len];
        if (code - first < count) return h->symbol[index + code - first];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static int inflate_codes(istate *s, const huff *lit, const huff *dist)
{
    for (;;)
    {
        int sym = huff_decode(s, lit);
        unsigned extra;
        if (sym < 0) return ZE_DATA;
        if (sym < 256)
        {
            if (s->outpos >= s->outn) return ZE_DATA;
            s->out[s->outpos++] = (BYTE)sym;
        }
        else if (sym == 256) return ZE_OK;
        else
        {
            size_t len, d;
            sym -= 257;
            if (sym >= 29) return ZE_DATA;
            if (need_bits(s, len_extra[sym], &extra)) return ZE_DATA;
            len = len_base[sym] + extra;
            sym = huff_decode(s, dist);
            if (sym < 0 || sym >= 30) return ZE_DATA;
            if (need_bits(s, dist_extra[sym], &extra)) return ZE_DATA;
            d = dist_base[sym] + extra;
            if (d > s->outpos || len > s->outn - s->outpos) return ZE_DATA;
            while (len--) { s->out[s->outpos] = s->out[s->outpos - d]; s->outpos++; }
        }
    }
}

static int inflate_stored(istate *s)
{
    unsigned len, nlen;
    s->bits = 0; s->nbits = 0;              /* to the byte boundary */
    if (s->pos + 4 > s->inn) return ZE_DATA;
    len = s->in[s->pos] | s->in[s->pos + 1] << 8;
    nlen = s->in[s->pos + 2] | s->in[s->pos + 3] << 8;
    s->pos += 4;
    if (len != (~nlen & 0xFFFF)) return ZE_DATA;
    if (len > s->inn - s->pos || len > s->outn - s->outpos) return ZE_DATA;
    memcpy(s->out + s->outpos, s->in + s->pos, len);
    s->pos += len; s->outpos += len;
    return ZE_OK;
}

static void fixed_lengths(BYTE *lens)
{
    int i;
    for (i = 0; i < 144; i++) lens[i] = 8;
    for (; i < 256; i++) lens[i] = 9;
    for (; i < 280; i++) lens[i] = 7;
    for (; i < 288; i++) lens[i] = 8;
    for (i = 0; i < 30; i++) lens[288 + i] = 5;
}

static int inflate_dynamic(istate *s)
{
    BYTE lens[320], cl[19];
    unsigned nlen, ndist, ncode, v;
    unsigned i;
    huff lit, dist, clh;
    if (need_bits(s, 5, &nlen) || need_bits(s, 5, &ndist) || need_bits(s, 4, &ncode)) return ZE_DATA;
    nlen += 257; ndist += 1; ncode += 4;
    if (nlen > 286 || ndist > 30) return ZE_DATA;
    memset(cl, 0, sizeof(cl));
    for (i = 0; i < ncode; i++) { if (need_bits(s, 3, &v)) return ZE_DATA; cl[clen_order[i]] = (BYTE)v; }
    if (huff_build(&clh, cl, 19) != 0) return ZE_DATA;       /* must be complete */
    for (i = 0; i < nlen + ndist;)
    {
        int sym = huff_decode(s, &clh);
        unsigned rep;
        BYTE val = 0;
        if (sym < 0) return ZE_DATA;
        if (sym < 16) { lens[i++] = (BYTE)sym; continue; }
        if (sym == 16)
        {
            if (!i) return ZE_DATA;
            val = lens[i - 1];
            if (need_bits(s, 2, &rep)) return ZE_DATA;
            rep += 3;
        }
        else if (sym == 17) { if (need_bits(s, 3, &rep)) return ZE_DATA; rep += 3; }
        else { if (need_bits(s, 7, &rep)) return ZE_DATA; rep += 11; }
        if (i + rep > nlen + ndist) return ZE_DATA;
        while (rep--) lens[i++] = val;
    }
    if (!lens[256]) return ZE_DATA;
    /* incomplete codes are allowed only for a single-code tree */
    {
        int left = huff_build(&lit, lens, nlen);
        if (left < 0 || (left > 0 && (int)nlen - lit.count[0] != 1)) return ZE_DATA;
        left = huff_build(&dist, lens + nlen, ndist);
        if (left < 0 || (left > 0 && (int)ndist - dist.count[0] != 1)) return ZE_DATA;
    }
    return inflate_codes(s, &lit, &dist);
}

int inflate_buf(const BYTE *in, size_t inn, BYTE *out, size_t outn)
{
    istate s = { in, inn, 0, 0, 0, out, outn, 0 };
    unsigned last, type;
    int r;
    do
    {
        if (need_bits(&s, 1, &last) || need_bits(&s, 2, &type)) return ZE_DATA;
        if (type == 0) r = inflate_stored(&s);
        else if (type == 1)
        {
            static huff flit, fdist;
            static BOOL built;
            if (!built)
            {
                BYTE lens[320];
                fixed_lengths(lens);
                huff_build(&flit, lens, 288);
                huff_build(&fdist, lens + 288, 30);
                built = TRUE;
            }
            r = inflate_codes(&s, &flit, &fdist);
        }
        else if (type == 2) r = inflate_dynamic(&s);
        else r = ZE_DATA;
        if (r) return r;
    } while (!last);
    return s.outpos == outn ? ZE_OK : ZE_DATA;
}

/* ---- deflate ------------------------------------------------------------------------------ */
typedef struct {
    BYTE *buf;
    size_t len, cap;
    DWORD bits;
    int nbits;
    BOOL oom;
} bitw;

static void bw_byte(bitw *w, BYTE b)
{
    if (w->len == w->cap)
    {
        size_t cap = w->cap ? w->cap * 2 : 65536;
        BYTE *p = realloc(w->buf, cap);
        if (!p) { w->oom = TRUE; return; }
        w->buf = p; w->cap = cap;
    }
    w->buf[w->len++] = b;
}

static void bw_put(bitw *w, unsigned v, int n)
{
    w->bits |= (DWORD)v << w->nbits;
    w->nbits += n;
    while (w->nbits >= 8) { bw_byte(w, (BYTE)w->bits); w->bits >>= 8; w->nbits -= 8; }
}

static void bw_align(bitw *w)
{
    if (w->nbits) bw_byte(w, (BYTE)w->bits);
    w->bits = 0; w->nbits = 0;
}

static unsigned reverse(unsigned code, int len)
{
    unsigned r = 0;
    while (len--) { r = (r << 1) | (code & 1); code >>= 1; }
    return r;
}

/* canonical codes (bit-reversed, ready to write) from lengths */
static void make_codes(const BYTE *len, int n, WORD *code)
{
    WORD count[16] = { 0 }, next[16];
    int i;
    unsigned c = 0;
    for (i = 0; i < n; i++) count[len[i]]++;
    count[0] = 0;
    for (i = 1; i < 16; i++) { c = (c + count[i - 1]) << 1; next[i] = (WORD)c; }
    for (i = 0; i < n; i++) if (len[i]) code[i] = (WORD)reverse(next[len[i]]++, len[i]);
}

/* Huffman code lengths for freq[], none longer than limit. Frequencies are
 * halved until the tree fits, which keeps the codes near optimal. */
static void huff_lengths(const DWORD *freq_in, int n, int limit, BYTE *len)
{
    DWORD freq[320], w[640];
    int parent[640], alive[640];
    int i, nodes, used, maxlen;

    memcpy(freq, freq_in, n * sizeof(DWORD));
    for (;;)
    {
        memset(len, 0, n);
        nodes = 0; used = 0;
        for (i = 0; i < n; i++) if (freq[i]) used++;
        if (used == 0) { len[0] = 1; len[1] = 1; return; }
        if (used == 1)
        {
            for (i = 0; i < n; i++) if (freq[i]) len[i] = 1;
            len[len[0] ? 1 : 0] = 1;          /* a second code keeps the tree complete */
            return;
        }
        for (i = 0; i < n; i++) { w[i] = freq[i]; alive[i] = freq[i] != 0; parent[i] = -1; }
        nodes = n;
        for (;;)
        {
            int a = -1, b = -1, k;
            for (k = 0; k < nodes; k++)
            {
                if (!alive[k]) continue;
                if (a < 0 || w[k] < w[a]) { b = a; a = k; }
                else if (b < 0 || w[k] < w[b]) b = k;
            }
            if (b < 0) break;
            w[nodes] = w[a] + w[b]; alive[nodes] = 1; parent[nodes] = -1;
            alive[a] = alive[b] = 0; parent[a] = parent[b] = nodes;
            nodes++;
        }
        maxlen = 0;
        for (i = 0; i < n; i++)
        {
            int d = 0, p;
            if (!freq[i]) continue;
            for (p = parent[i]; p >= 0; p = parent[p]) d++;
            len[i] = (BYTE)(d > 255 ? 255 : d);
            if (d > maxlen) maxlen = d;
        }
        if (maxlen <= limit) return;
        for (i = 0; i < n; i++) if (freq[i]) freq[i] = (freq[i] >> 1) | 1;
    }
}

static int len_code(int len)
{
    int c = 0;
    while (c < 28 && len_base[c + 1] <= len) c++;
    return c;
}

static int dist_code(int d)
{
    int c = 0;
    while (c < 29 && dist_base[c + 1] <= d) c++;
    return c;
}

#define WSIZE    32768
#define WMASK    (WSIZE - 1)
#define HBITS    15
#define HSIZE    (1 << HBITS)
#define MAXCHAIN 96
#define BLOCKSYM 16384

typedef struct { WORD lit; WORD dist; } lzsym;     /* lit < 256, or 256+len with dist */

/* the run-length coded lengths of a dynamic header */
typedef struct { BYTE sym, extra; } clsym;

static int rle_lengths(const BYTE *lens, int n, clsym *out)
{
    int i = 0, k = 0;
    while (i < n)
    {
        int run = 1;
        while (i + run < n && lens[i + run] == lens[i]) run++;
        if (!lens[i] && run >= 3)
        {
            int r = run > 138 ? 138 : run;
            if (r >= 11) { out[k].sym = 18; out[k++].extra = (BYTE)(r - 11); }
            else { out[k].sym = 17; out[k++].extra = (BYTE)(r - 3); }
            i += r;
        }
        else if (lens[i] && run >= 4)
        {
            int r = run - 1 > 6 ? 6 : run - 1;
            out[k].sym = lens[i]; out[k++].extra = 0;
            out[k].sym = 16; out[k++].extra = (BYTE)(r - 3);
            i += r + 1;
        }
        else { out[k].sym = lens[i]; out[k++].extra = 0; i++; }
    }
    return k;
}

static void emit_symbols(bitw *w, const lzsym *syms, int nsym,
                         const BYTE *llen, const WORD *lcode, const BYTE *dlen, const WORD *dcode)
{
    int i;
    for (i = 0; i < nsym; i++)
    {
        if (syms[i].lit < 256) bw_put(w, lcode[syms[i].lit], llen[syms[i].lit]);
        else
        {
            int len = syms[i].lit - 256, c = len_code(len), d = syms[i].dist, dc = dist_code(d);
            bw_put(w, lcode[257 + c], llen[257 + c]);
            if (len_extra[c]) bw_put(w, len - len_base[c], len_extra[c]);
            bw_put(w, dcode[dc], dlen[dc]);
            if (dist_extra[dc]) bw_put(w, d - dist_base[dc], dist_extra[dc]);
        }
    }
    bw_put(w, lcode[256], llen[256]);
}

static void flush_block(bitw *w, const lzsym *syms, int nsym, const BYTE *raw, size_t rawlen, BOOL last)
{
    DWORD lf[286] = { 0 }, df[30] = { 0 }, cf[19] = { 0 };
    BYTE llen[286], dlen[30], clen[19], all[316], flen[320];
    WORD lcode[286], dcode[30], ccode[19], flcode[288], fdcode[30];
    clsym rle[320];
    ULONGLONG extra_bits = 0, dyn, fix, sto;
    int i, nl, nd, nc, nrle;

    for (i = 0; i < nsym; i++)
    {
        if (syms[i].lit < 256) lf[syms[i].lit]++;
        else
        {
            int c = len_code(syms[i].lit - 256), dc = dist_code(syms[i].dist);
            lf[257 + c]++; df[dc]++;
            extra_bits += len_extra[c] + dist_extra[dc];
        }
    }
    lf[256] = 1;
    huff_lengths(lf, 286, 15, llen);
    huff_lengths(df, 30, 15, dlen);
    for (nl = 286; nl > 257 && !llen[nl - 1]; nl--) ;
    for (nd = 30; nd > 1 && !dlen[nd - 1]; nd--) ;
    memcpy(all, llen, nl);
    memcpy(all + nl, dlen, nd);
    nrle = rle_lengths(all, nl + nd, rle);
    for (i = 0; i < nrle; i++) cf[rle[i].sym]++;
    huff_lengths(cf, 19, 7, clen);
    for (nc = 19; nc > 4 && !clen[clen_order[nc - 1]]; nc--) ;

    dyn = 3 + 14 + 3 * nc + extra_bits;
    for (i = 0; i < nrle; i++)
        dyn += clen[rle[i].sym] + (rle[i].sym == 16 ? 2 : rle[i].sym == 17 ? 3 : rle[i].sym == 18 ? 7 : 0);
    for (i = 0; i < 286; i++) dyn += (ULONGLONG)lf[i] * llen[i];
    for (i = 0; i < 30; i++) dyn += (ULONGLONG)df[i] * dlen[i];

    fixed_lengths(flen);
    fix = 3 + extra_bits;
    for (i = 0; i < 286; i++) fix += (ULONGLONG)lf[i] * flen[i];
    for (i = 0; i < 30; i++) fix += (ULONGLONG)df[i] * 5;

    sto = ((rawlen + 65534) / 65535 + !rawlen) * 40 + rawlen * 8 + 8;

    if (sto <= dyn && sto <= fix)
    {
        size_t off = 0;
        do
        {
            size_t n = rawlen - off > 65535 ? 65535 : rawlen - off;
            bw_put(w, last && off + n == rawlen, 1);
            bw_put(w, 0, 2);
            bw_align(w);
            bw_byte(w, (BYTE)n); bw_byte(w, (BYTE)(n >> 8));
            bw_byte(w, (BYTE)~n); bw_byte(w, (BYTE)(~n >> 8));
            for (i = 0; (size_t)i < n; i++) bw_byte(w, raw[off + i]);
            off += n;
        } while (off < rawlen);
        return;
    }
    bw_put(w, last, 1);
    if (fix <= dyn)
    {
        bw_put(w, 1, 2);
        make_codes(flen, 288, flcode);
        make_codes(flen + 288, 30, fdcode);
        emit_symbols(w, syms, nsym, flen, flcode, flen + 288, fdcode);
        return;
    }
    bw_put(w, 2, 2);
    bw_put(w, nl - 257, 5);
    bw_put(w, nd - 1, 5);
    bw_put(w, nc - 4, 4);
    for (i = 0; i < nc; i++) bw_put(w, clen[clen_order[i]], 3);
    make_codes(clen, 19, ccode);
    for (i = 0; i < nrle; i++)
    {
        bw_put(w, ccode[rle[i].sym], clen[rle[i].sym]);
        if (rle[i].sym == 16) bw_put(w, rle[i].extra, 2);
        else if (rle[i].sym == 17) bw_put(w, rle[i].extra, 3);
        else if (rle[i].sym == 18) bw_put(w, rle[i].extra, 7);
    }
    make_codes(llen, 286, lcode);
    make_codes(dlen, 30, dcode);
    emit_symbols(w, syms, nsym, llen, lcode, dlen, dcode);
}

static inline unsigned hash3(const BYTE *p) { return ((p[0] << 10) ^ (p[1] << 5) ^ p[2]) & (HSIZE - 1); }

int deflate_buf(const BYTE *in, size_t inn, BYTE **out, size_t *outn)
{
    bitw w = { 0 };
    int *head = malloc(HSIZE * sizeof(int)), *prev = malloc(WSIZE * sizeof(int));
    lzsym *syms = malloc(BLOCKSYM * sizeof(lzsym));
    size_t pos = 0, block_start = 0;
    int nsym = 0, i;

    if (!head || !prev || !syms) { free(head); free(prev); free(syms); return ZE_NOMEM; }
    for (i = 0; i < HSIZE; i++) head[i] = -1;

    while (pos < inn)
    {
        int best = 0, bestd = 0;
        if (pos + 3 <= inn)
        {
            unsigned h = hash3(in + pos);
            long long cand = head[h];
            int chain = MAXCHAIN, maxlen = inn - pos > 258 ? 258 : (int)(inn - pos);
            while (cand >= 0 && chain-- > 0)
            {
                size_t d = pos - (size_t)cand;
                if (d == 0 || d > WSIZE) break;
                if (in[cand + best] == in[pos + best] && in[cand] == in[pos])
                {
                    int l = 0;
                    while (l < maxlen && in[cand + l] == in[pos + l]) l++;
                    if (l > best) { best = l; bestd = (int)d; if (l == maxlen) break; }
                }
                {
                    long long nx = prev[cand & WMASK];
                    if (nx >= cand) break;
                    cand = nx;
                }
            }
        }
        if (best >= 3 && !(best == 3 && bestd > 4096))
        {
            size_t end = pos + best;
            syms[nsym].lit = (WORD)(256 + best); syms[nsym++].dist = (WORD)bestd;
            for (; pos < end; pos++)
                if (pos + 3 <= inn) { unsigned h = hash3(in + pos); prev[pos & WMASK] = head[h]; head[h] = (int)pos; }
        }
        else
        {
            if (pos + 3 <= inn) { unsigned h = hash3(in + pos); prev[pos & WMASK] = head[h]; head[h] = (int)pos; }
            syms[nsym].lit = in[pos]; syms[nsym++].dist = 0;
            pos++;
        }
        if (nsym == BLOCKSYM)
        {
            flush_block(&w, syms, nsym, in + block_start, pos - block_start, pos == inn);
            block_start = pos; nsym = 0;
        }
    }
    if (nsym || !inn) flush_block(&w, syms, nsym, in + block_start, pos - block_start, TRUE);
    bw_align(&w);
    free(head); free(prev); free(syms);
    if (w.oom) { free(w.buf); return ZE_NOMEM; }
    *out = w.buf; *outn = w.len;
    return ZE_OK;
}

/* ---- reading an archive ------------------------------------------------------------------- */
static WORD  rd16(const BYTE *p) { return (WORD)(p[0] | p[1] << 8); }
static DWORD rd32(const BYTE *p) { return p[0] | p[1] << 8 | p[2] << 16 | (DWORD)p[3] << 24; }
static ULONGLONG rd64(const BYTE *p) { return rd32(p) | (ULONGLONG)rd32(p + 4) << 32; }

void zip_close(zarchive *z)
{
    int i;
    for (i = 0; i < z->n; i++) free(z->e[i].name);
    free(z->e);
    if (z->base) UnmapViewOfFile(z->base);
    if (z->map) CloseHandle(z->map);
    if (z->file && z->file != INVALID_HANDLE_VALUE) CloseHandle(z->file);
    memset(z, 0, sizeof(*z));
}

int zip_open(zarchive *z, const WCHAR *path)
{
    LARGE_INTEGER size;
    ULONGLONG eocd = 0, cdoff, cdsize, count, p, lo;
    BOOL found = FALSE;
    int i;

    memset(z, 0, sizeof(*z));
    z->file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (z->file == INVALID_HANDLE_VALUE || !GetFileSizeEx(z->file, &size)) { zip_close(z); return ZE_OPEN; }
    z->size = size.QuadPart;
    if (z->size < 22) { zip_close(z); return ZE_FORMAT; }
    if (!(z->map = CreateFileMappingW(z->file, NULL, PAGE_READONLY, 0, 0, NULL)) ||
        !(z->base = MapViewOfFile(z->map, FILE_MAP_READ, 0, 0, 0))) { zip_close(z); return ZE_OPEN; }

    lo = z->size > 22 + 65535 ? z->size - 22 - 65535 : 0;
    for (p = z->size - 22 + 1; p-- > lo;)
        if (rd32(z->base + p) == 0x06054b50 && p + 22 + rd16(z->base + p + 20) <= z->size) { eocd = p; found = TRUE; break; }
    if (!found) { zip_close(z); return ZE_FORMAT; }
    count  = rd16(z->base + eocd + 10);
    cdsize = rd32(z->base + eocd + 12);
    cdoff  = rd32(z->base + eocd + 16);
    if ((count == 0xFFFF || cdsize == 0xFFFFFFFF || cdoff == 0xFFFFFFFF) && eocd >= 20 &&
        rd32(z->base + eocd - 20) == 0x07064b50)
    {
        ULONGLONG z64 = rd64(z->base + eocd - 12);
        if (z64 + 56 > z->size || rd32(z->base + z64) != 0x06064b50) { zip_close(z); return ZE_FORMAT; }
        count  = rd64(z->base + z64 + 32);
        cdsize = rd64(z->base + z64 + 40);
        cdoff  = rd64(z->base + z64 + 48);
    }
    if (cdoff > z->size || cdsize > z->size - cdoff || count > cdsize / 46 + 1 || count > 1000000) { zip_close(z); return ZE_FORMAT; }
    if (count && !(z->e = calloc((size_t)count, sizeof(zentry)))) { zip_close(z); return ZE_NOMEM; }

    p = cdoff;
    for (i = 0; (ULONGLONG)i < count; i++)
    {
        const BYTE *h = z->base + p;
        zentry *e = &z->e[i];
        unsigned nlen, xlen, clen, cp, k;
        const BYTE *x;
        if (p + 46 > cdoff + cdsize || rd32(h) != 0x02014b50) { zip_close(z); return ZE_FORMAT; }
        nlen = rd16(h + 28); xlen = rd16(h + 30); clen = rd16(h + 32);
        if (p + 46 + nlen + xlen + clen > cdoff + cdsize) { zip_close(z); return ZE_FORMAT; }
        e->flags = rd16(h + 8); e->method = rd16(h + 10);
        e->dostime = rd16(h + 12); e->dosdate = rd16(h + 14);
        e->crc = rd32(h + 16); e->csize = rd32(h + 20); e->usize = rd32(h + 24);
        e->lho = rd32(h + 42);
        /* ZIP64 extended information: only the fields that overflowed, in order */
        for (x = h + 46 + nlen; x + 4 <= h + 46 + nlen + xlen; x += 4 + rd16(x + 2))
        {
            const BYTE *f = x + 4, *fend = x + 4 + rd16(x + 2);
            if (fend > h + 46 + nlen + xlen) break;
            if (rd16(x) != 0x0001) continue;
            if (e->usize == 0xFFFFFFFF && f + 8 <= fend) { e->usize = rd64(f); f += 8; }
            if (e->csize == 0xFFFFFFFF && f + 8 <= fend) { e->csize = rd64(f); f += 8; }
            if (e->lho == 0xFFFFFFFF && f + 8 <= fend) { e->lho = rd64(f); f += 8; }
        }
        cp = e->flags & 0x800 ? CP_UTF8 : 437;
        k = MultiByteToWideChar(cp, 0, (const char *)h + 46, nlen, NULL, 0);
        if (!(e->name = calloc(k + 1, sizeof(WCHAR)))) { zip_close(z); return ZE_NOMEM; }
        MultiByteToWideChar(cp, 0, (const char *)h + 46, nlen, e->name, k);
        for (k = 0; e->name[k]; k++) if (e->name[k] == '\\') e->name[k] = '/';
        e->dir = k && e->name[k - 1] == '/';
        z->n = i + 1;
        p += 46 + nlen + xlen + clen;
    }
    z->n = (int)count;
    return ZE_OK;
}

int zip_read(zarchive *z, int i, BYTE **out, size_t *len)
{
    zentry *e = &z->e[i];
    const BYTE *h, *data;
    BYTE *buf;
    int r;

    *out = NULL; *len = 0;
    if (e->flags & 1) return ZE_ENCRYPTED;
    if (e->method != 0 && e->method != 8) return ZE_METHOD;
    if (e->lho + 30 > z->size) return ZE_FORMAT;
    h = z->base + e->lho;
    if (rd32(h) != 0x04034b50) return ZE_FORMAT;
    if (e->lho + 30 + rd16(h + 26) + rd16(h + 28) > z->size) return ZE_FORMAT;
    data = h + 30 + rd16(h + 26) + rd16(h + 28);
    if (e->csize > z->size - (ULONGLONG)(data - z->base)) return ZE_FORMAT;
    if (e->usize > (ULONGLONG)((size_t)-1 >> 1)) return ZE_NOMEM;
    if (!(buf = malloc(e->usize ? (size_t)e->usize : 1))) return ZE_NOMEM;
    if (e->method == 0)
    {
        if (e->csize != e->usize) { free(buf); return ZE_DATA; }
        memcpy(buf, data, (size_t)e->usize);
    }
    else if ((r = inflate_buf(data, (size_t)e->csize, buf, (size_t)e->usize))) { free(buf); return r; }
#ifndef SG_MUTANT_NOCRC
    if (crc32_update(0, buf, (size_t)e->usize) != e->crc) { free(buf); return ZE_CRC; }
#endif
    *out = buf; *len = (size_t)e->usize;
    return ZE_OK;
}

BOOL zip_safe_path(const WCHAR *name, WCHAR *out, int cch)
{
    const WCHAR *p = name;
    int o = 0;

    out[0] = 0;
#ifndef SG_MUTANT_TRAVERSAL
    if (name[0] == '/' || name[0] == '\\') return FALSE;      /* absolute */
    if (wcschr(name, ':')) return FALSE;                       /* a drive, or a stream */
#endif
    while (*p)
    {
        const WCHAR *s = p;
        int n, k;
        BOOL dots = TRUE;
        while (*p && *p != '/' && *p != '\\') p++;
        n = (int)(p - s);
        if (*p) p++;
        if (!n || (n == 1 && s[0] == '.')) continue;
        for (k = 0; k < n; k++) if (s[k] != '.' && s[k] != ' ') dots = FALSE;
#ifndef SG_MUTANT_TRAVERSAL
        if (dots) return FALSE;                                /* "..", and what Windows trims to it */
#else
        (void)dots;
#endif
        if (o + n + 2 > cch) return FALSE;
        if (o) out[o++] = '\\';
        for (k = 0; k < n; k++)
        {
            WCHAR c = s[k];
            out[o++] = (c < 32 || wcschr(L"<>\"|?*", c)) ? '_' : c;
        }
    }
    out[o] = 0;
    return o > 0;
}

/* ---- writing an archive ------------------------------------------------------------------- */
typedef struct { char *name; int nlen; WORD method, flags, t, d; DWORD crc, csize, usize, lho, attr; } wentry;

struct zwriter {
    HANDLE f;
    WCHAR path[MAX_PATH], tmp[MAX_PATH + 8];
    ULONGLONG off;
    wentry *e;
    int n, cap;
    BOOL failed;
};

static void wr(zwriter *w, const void *p, DWORD n)
{
    DWORD done;
    if (w->failed) return;
    if (!WriteFile(w->f, p, n, &done, NULL) || done != n) w->failed = TRUE;
    w->off += n;
}

static void put16(BYTE *p, unsigned v) { p[0] = (BYTE)v; p[1] = (BYTE)(v >> 8); }
static void put32(BYTE *p, DWORD v) { put16(p, v & 0xFFFF); put16(p + 2, v >> 16); }

zwriter *zw_open(const WCHAR *path)
{
    zwriter *w = calloc(1, sizeof(*w));
    if (!w) return NULL;
    lstrcpynW(w->path, path, MAX_PATH);
    wsprintfW(w->tmp, L"%s.part", w->path);
    w->f = CreateFileW(w->tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN, NULL);
    if (w->f == INVALID_HANDLE_VALUE) { free(w); return NULL; }
    return w;
}

static int add_entry(zwriter *w, const WCHAR *arcname, const FILETIME *ft, WORD method, DWORD crc,
                     const BYTE *data, DWORD csize, DWORD usize, DWORD attr)
{
    BYTE h[30];
    wentry *e;
    FILETIME lft;
    int nlen, i;
    BOOL ascii = TRUE;

    if (w->n == 65535 || w->off + 30 + csize + 1024 > 0xFFFFFFFFull) return ZE_TOOBIG;
    if (w->n == w->cap)
    {
        wentry *p = realloc(w->e, (w->cap ? w->cap * 2 : 64) * sizeof(wentry));
        if (!p) return ZE_NOMEM;
        w->e = p; w->cap = w->cap ? w->cap * 2 : 64;
    }
    e = &w->e[w->n];
    memset(e, 0, sizeof(*e));
    nlen = WideCharToMultiByte(CP_UTF8, 0, arcname, -1, NULL, 0, NULL, NULL) - 1;
    if (nlen <= 0 || nlen > 65535 || !(e->name = malloc(nlen + 1))) return ZE_NOMEM;
    WideCharToMultiByte(CP_UTF8, 0, arcname, -1, e->name, nlen + 1, NULL, NULL);
    for (i = 0; i < nlen; i++) { if (e->name[i] == '\\') e->name[i] = '/'; if ((BYTE)e->name[i] >= 0x80) ascii = FALSE; }
    e->nlen = nlen;
    e->flags = ascii ? 0 : 0x800;
    e->method = method;
    e->crc = crc; e->csize = csize; e->usize = usize;
    e->lho = (DWORD)w->off;
    e->attr = attr;
    if (ft && FileTimeToLocalFileTime(ft, &lft) && FileTimeToDosDateTime(&lft, &e->d, &e->t)) ;
    else { e->d = (1 << 5) | 1; e->t = 0; }       /* 1980-01-01 */

    put32(h, 0x04034b50); put16(h + 4, 20); put16(h + 6, e->flags); put16(h + 8, method);
    put16(h + 10, e->t); put16(h + 12, e->d); put32(h + 14, crc); put32(h + 18, csize);
    put32(h + 22, usize); put16(h + 26, nlen); put16(h + 28, 0);
    wr(w, h, 30);
    wr(w, e->name, nlen);
    if (csize) wr(w, data, csize);
    w->n++;
    return w->failed ? ZE_WRITE : ZE_OK;
}

int zw_add_dir(zwriter *w, const WCHAR *arcname, const FILETIME *ft)
{
    WCHAR name[MAX_PATH + 2];
    int n = lstrlenW(arcname);
    if (n + 2 > MAX_PATH + 2) return ZE_TOOBIG;
    lstrcpyW(name, arcname);
    if (!n || (name[n - 1] != '/' && name[n - 1] != '\\')) { name[n] = '/'; name[n + 1] = 0; }
    return add_entry(w, name, ft, 0, 0, NULL, 0, 0, FILE_ATTRIBUTE_DIRECTORY);
}

int zw_add_file(zwriter *w, const WCHAR *arcname, const WCHAR *src)
{
    HANDLE f = CreateFileW(src, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    LARGE_INTEGER size;
    FILETIME ft;
    BYTE *data = NULL, *packed = NULL;
    size_t plen = 0;
    DWORD got = 0, crc;
    int r;

    if (f == INVALID_HANDLE_VALUE) return ZE_OPEN;
    if (!GetFileSizeEx(f, &size) || !GetFileTime(f, NULL, NULL, &ft)) { CloseHandle(f); return ZE_OPEN; }
    if (size.QuadPart > 0xFFFFFFF0ll) { CloseHandle(f); return ZE_TOOBIG; }
    if (!(data = malloc(size.QuadPart ? (size_t)size.QuadPart : 1))) { CloseHandle(f); return ZE_NOMEM; }
    if (size.QuadPart && (!ReadFile(f, data, (DWORD)size.QuadPart, &got, NULL) || got != (DWORD)size.QuadPart))
    { CloseHandle(f); free(data); return ZE_OPEN; }
    CloseHandle(f);
    crc = crc32_update(0, data, got);
    if (got && (r = deflate_buf(data, got, &packed, &plen))) { free(data); return r; }
    if (got && plen < got)
        r = add_entry(w, arcname, &ft, 8, crc, packed, (DWORD)plen, got, FILE_ATTRIBUTE_ARCHIVE);
    else
        r = add_entry(w, arcname, &ft, 0, crc, data, got, got, FILE_ATTRIBUTE_ARCHIVE);
    free(packed); free(data);
    return r;
}

/* an entry from memory; store=TRUE keeps it uncompressed (as OpenDocument's
 * "mimetype" must be) */
int zw_add_mem(zwriter *w, const WCHAR *arcname, const void *data, DWORD n, BOOL store)
{
    FILETIME ft;
    BYTE *packed = NULL;
    size_t plen = 0;
    DWORD crc = crc32_update(0, data, n);
    int r;
    GetSystemTimeAsFileTime(&ft);
    if (!store && n && !deflate_buf(data, n, &packed, &plen) && plen < n)
        r = add_entry(w, arcname, &ft, 8, crc, packed, (DWORD)plen, n, FILE_ATTRIBUTE_ARCHIVE);
    else
        r = add_entry(w, arcname, &ft, 0, crc, data, n, n, FILE_ATTRIBUTE_ARCHIVE);
    free(packed);
    return r;
}

int zw_close(zwriter *w, BOOL keep)
{
    BYTE h[46];
    DWORD cdoff = (DWORD)w->off;
    int i, r = ZE_OK;

    if (keep)
    {
        for (i = 0; i < w->n; i++)
        {
            wentry *e = &w->e[i];
            memset(h, 0, sizeof(h));
            put32(h, 0x02014b50); put16(h + 4, 20); put16(h + 6, 20); put16(h + 8, e->flags);
            put16(h + 10, e->method); put16(h + 12, e->t); put16(h + 14, e->d); put32(h + 16, e->crc);
            put32(h + 20, e->csize); put32(h + 24, e->usize); put16(h + 28, e->nlen);
            put32(h + 38, e->attr); put32(h + 42, e->lho);
            wr(w, h, 46);
            wr(w, e->name, e->nlen);
        }
        memset(h, 0, 22);
        put32(h, 0x06054b50); put16(h + 8, w->n); put16(h + 10, w->n);
        put32(h + 12, (DWORD)w->off - cdoff); put32(h + 16, cdoff);
        wr(w, h, 22);
        if (w->failed || w->off > 0xFFFFFFFFull) r = w->failed ? ZE_WRITE : ZE_TOOBIG;
    }
    CloseHandle(w->f);
    if (keep && !r)
    {
        if (!MoveFileExW(w->tmp, w->path, MOVEFILE_REPLACE_EXISTING)) r = ZE_WRITE;
        else SetFileAttributesW(w->path, FILE_ATTRIBUTE_ARCHIVE);
    }
    if (!keep || r) DeleteFileW(w->tmp);
    for (i = 0; i < w->n; i++) free(w->e[i].name);
    free(w->e);
    free(w);
    return keep ? r : ZE_CANCELLED;
}

const WCHAR *zip_strerror(int e)
{
    switch (e)
    {
    case ZE_OK:        return L"The operation completed successfully.";
    case ZE_OPEN:      return L"The file cannot be opened.";
    case ZE_FORMAT:    return L"The Compressed (zipped) Folder is invalid.";
    case ZE_METHOD:    return L"The file was compressed with a method that is not supported.";
    case ZE_ENCRYPTED: return L"The file is password protected.";
    case ZE_DATA:      return L"The compressed data is damaged.";
    case ZE_CRC:       return L"The file is damaged: its contents do not match its checksum.";
    case ZE_NOMEM:     return L"There is not enough memory.";
    case ZE_UNSAFE:    return L"The name would place the file outside the destination folder.";
    case ZE_WRITE:     return L"The destination cannot be written.";
    case ZE_TOOBIG:    return L"The file is too large for a Compressed (zipped) Folder.";
    case ZE_CANCELLED: return L"The operation was cancelled.";
    }
    return L"Unknown error.";
}
