/* sg-wordpad -- a small non-validating XML reader into a tree, for the
 * documents' parts (Office Open XML and OpenDocument). Elements, attributes,
 * text with the predefined and numeric character references, CDATA;
 * comments, processing instructions and DOCTYPE are skipped. Names keep
 * their prefixes; xml_attr/xml_child match on the local part.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "wordpad.h"

typedef struct { const char *s; size_t n, i; } X;

static char *dupn(const char *s, size_t n)
{
    char *p = malloc(n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

static void put_utf8(Buf *b, unsigned cp)
{
    char u[4];
    int n;
    if (cp < 0x80) { u[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { u[0] = (char)(0xC0 | cp >> 6); u[1] = (char)(0x80 | (cp & 63)); n = 2; }
    else if (cp < 0x10000) { u[0] = (char)(0xE0 | cp >> 12); u[1] = (char)(0x80 | ((cp >> 6) & 63)); u[2] = (char)(0x80 | (cp & 63)); n = 3; }
    else { u[0] = (char)(0xF0 | cp >> 18); u[1] = (char)(0x80 | ((cp >> 12) & 63)); u[2] = (char)(0x80 | ((cp >> 6) & 63)); u[3] = (char)(0x80 | (cp & 63)); n = 4; }
    buf_add(b, u, n);
}

/* text with references resolved */
static char *unescape(const char *s, size_t n)
{
    Buf b = { 0 };
    size_t i = 0;
    while (i < n)
    {
        if (s[i] == '&')
        {
            size_t j = i + 1;
            while (j < n && j - i < 12 && s[j] != ';') j++;
            if (j < n && s[j] == ';')
            {
                const char *e = s + i + 1;
                size_t len = j - i - 1;
                if (len == 2 && !strncmp(e, "lt", 2)) buf_add(&b, "<", 1);
                else if (len == 2 && !strncmp(e, "gt", 2)) buf_add(&b, ">", 1);
                else if (len == 3 && !strncmp(e, "amp", 3)) buf_add(&b, "&", 1);
                else if (len == 4 && !strncmp(e, "quot", 4)) buf_add(&b, "\"", 1);
                else if (len == 4 && !strncmp(e, "apos", 4)) buf_add(&b, "'", 1);
                else if (len > 1 && e[0] == '#')
                {
                    unsigned cp = (e[1] == 'x' || e[1] == 'X') ? (unsigned)strtoul(e + 2, NULL, 16) : (unsigned)strtoul(e + 1, NULL, 10);
                    if (cp && cp < 0x110000) put_utf8(&b, cp);
                }
                else buf_add(&b, s + i, j - i + 1);
                i = j + 1;
                continue;
            }
        }
        buf_add(&b, s + i, 1);
        i++;
    }
    if (!b.p) return dupn("", 0);
    return b.p;
}

static void add_kid(XNode *p, XNode *k)
{
    p->kids = realloc(p->kids, (p->nkids + 1) * sizeof(XNode *));
    p->kids[p->nkids++] = k;
}

static int is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

static size_t find(X *x, const char *what)
{
    size_t wl = strlen(what);
    for (size_t j = x->i; j + wl <= x->n; j++) if (!memcmp(x->s + j, what, wl)) return j;
    return x->n;
}

static XNode *parse_element(X *x, int depth);

static void parse_content(X *x, XNode *parent, int depth)
{
    while (x->i < x->n)
    {
        if (x->s[x->i] == '<')
        {
            if (x->i + 1 < x->n && x->s[x->i + 1] == '/')
            {
                size_t e = find(x, ">");
                x->i = e < x->n ? e + 1 : x->n;
                return;
            }
            if (x->i + 3 < x->n && !memcmp(x->s + x->i, "<!--", 4)) { size_t e = find(x, "-->"); x->i = e < x->n ? e + 3 : x->n; continue; }
            if (x->i + 8 < x->n && !memcmp(x->s + x->i, "<![CDATA[", 9))
            {
                size_t st = x->i + 9, e;
                x->i = st;
                e = find(x, "]]>");
                {
                    XNode *t = calloc(1, sizeof(XNode));
                    t->text = dupn(x->s + st, e - st);
                    add_kid(parent, t);
                }
                x->i = e < x->n ? e + 3 : x->n;
                continue;
            }
            if (x->i + 1 < x->n && (x->s[x->i + 1] == '?' || x->s[x->i + 1] == '!')) { size_t e = find(x, ">"); x->i = e < x->n ? e + 1 : x->n; continue; }
            {
                XNode *k = parse_element(x, depth + 1);
                if (k) add_kid(parent, k);
                else return;
            }
        }
        else
        {
            size_t st = x->i;
            while (x->i < x->n && x->s[x->i] != '<') x->i++;
            {
                XNode *t = calloc(1, sizeof(XNode));
                t->text = unescape(x->s + st, x->i - st);
                add_kid(parent, t);
            }
        }
    }
}

static XNode *parse_element(X *x, int depth)
{
    XNode *e;
    size_t st;
    if (depth > 256 || x->i >= x->n || x->s[x->i] != '<') return NULL;
    x->i++;
    st = x->i;
    while (x->i < x->n && !is_space(x->s[x->i]) && x->s[x->i] != '>' && x->s[x->i] != '/') x->i++;
    e = calloc(1, sizeof(XNode));
    e->name = dupn(x->s + st, x->i - st);
    for (;;)
    {
        while (x->i < x->n && is_space(x->s[x->i])) x->i++;
        if (x->i >= x->n) return e;
        if (x->s[x->i] == '/') { size_t g = find(x, ">"); x->i = g < x->n ? g + 1 : x->n; return e; }
        if (x->s[x->i] == '>') { x->i++; break; }
        {
            size_t ns = x->i, ne, vs, ve;
            char q;
            while (x->i < x->n && x->s[x->i] != '=' && !is_space(x->s[x->i]) && x->s[x->i] != '>' && x->s[x->i] != '/') x->i++;
            ne = x->i;
            while (x->i < x->n && is_space(x->s[x->i])) x->i++;
            if (x->i >= x->n || x->s[x->i] != '=') { if (x->i == ns) x->i++; continue; }
            x->i++;
            while (x->i < x->n && is_space(x->s[x->i])) x->i++;
            if (x->i >= x->n) return e;
            q = x->s[x->i];
            if (q != '"' && q != '\'') continue;
            vs = ++x->i;
            while (x->i < x->n && x->s[x->i] != q) x->i++;
            ve = x->i;
            if (x->i < x->n) x->i++;
            e->attr = realloc(e->attr, (e->nattr + 1) * 2 * sizeof(char *));
            e->attr[e->nattr * 2] = dupn(x->s + ns, ne - ns);
            e->attr[e->nattr * 2 + 1] = unescape(x->s + vs, ve - vs);
            e->nattr++;
        }
    }
    parse_content(x, e, depth);
    return e;
}

XNode *xml_parse(const char *s, size_t n)
{
    X x = { s, n, 0 };
    XNode *root = calloc(1, sizeof(XNode));
    if (n >= 3 && (BYTE)s[0] == 0xEF && (BYTE)s[1] == 0xBB && (BYTE)s[2] == 0xBF) x.i = 3;
    root->name = dupn("#document", 9);
    parse_content(&x, root, 0);
    return root;
}

void xml_free(XNode *x)
{
    if (!x) return;
    for (int i = 0; i < x->nkids; i++) xml_free(x->kids[i]);
    for (int i = 0; i < x->nattr * 2; i++) free(x->attr[i]);
    free(x->attr); free(x->kids); free(x->name); free(x->text);
    free(x);
}

const char *xml_local(const char *name)
{
    const char *c = name ? strchr(name, ':') : NULL;
    return c ? c + 1 : name;
}

const char *xml_attr(const XNode *x, const char *name)
{
    if (!x) return NULL;
    for (int i = 0; i < x->nattr; i++)
        if (!strcmp(x->attr[i * 2], name) || !strcmp(xml_local(x->attr[i * 2]), name)) return x->attr[i * 2 + 1];
    return NULL;
}

XNode *xml_child(const XNode *x, const char *local)
{
    if (!x) return NULL;
    for (int i = 0; i < x->nkids; i++)
        if (x->kids[i]->name && !strcmp(xml_local(x->kids[i]->name), local)) return x->kids[i];
    return NULL;
}
