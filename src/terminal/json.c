/* sg-terminal -- the JSON document model (json.h).
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static jv *make(int type)
{
    jv *v = calloc(1, sizeof(*v));
    if (v) v->type = type;
    return v;
}

static char *dupn(const char *s, size_t n)
{
    char *p = malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

jv *jnull(void) { return make(J_NULL); }
jv *jbool(int b) { jv *v = make(J_BOOL); if (v) v->b = !!b; return v; }
jv *jarr(void) { return make(J_ARR); }
jv *jobj(void) { return make(J_OBJ); }

jv *jnum(double d)
{
    jv *v = make(J_NUM);
    char buf[40];
    if (!v) return NULL;
    if (d == (double)(long long)d) snprintf(buf, sizeof(buf), "%lld", (long long)d);
    else snprintf(buf, sizeof(buf), "%.6g", d);
    v->s = dupn(buf, strlen(buf));
    return v;
}

jv *jstr(const char *s)
{
    jv *v = make(J_STR);
    if (v) v->s = dupn(s, strlen(s));
    return v;
}

void json_free(jv *v)
{
    int i;
    if (!v) return;
    for (i = 0; i < v->n; i++) {
        json_free(v->items[i]);
        if (v->keys) free(v->keys[i]);
    }
    free(v->items);
    free(v->keys);
    free(v->s);
    free(v);
}

static int grow(jv *v)
{
    if (v->n < v->cap) return 1;
    {
        int cap = v->cap ? v->cap * 2 : 8;
        jv **it = realloc(v->items, cap * sizeof(*it));
        if (!it) return 0;
        v->items = it;
        if (v->type == J_OBJ) {
            char **k = realloc(v->keys, cap * sizeof(*k));
            if (!k) return 0;
            v->keys = k;
        }
        v->cap = cap;
    }
    return 1;
}

void jpush(jv *arr, jv *val)
{
    if (!arr || arr->type != J_ARR || !val || !grow(arr)) { json_free(val); return; }
    arr->items[arr->n++] = val;
}

jv *jget(const jv *obj, const char *key)
{
    int i;
    if (!obj || obj->type != J_OBJ) return NULL;
    for (i = 0; i < obj->n; i++) if (!strcmp(obj->keys[i], key)) return obj->items[i];
    return NULL;
}

void jset(jv *obj, const char *key, jv *val)
{
    int i;
    if (!obj || obj->type != J_OBJ || !val) { json_free(val); return; }
    for (i = 0; i < obj->n; i++)
        if (!strcmp(obj->keys[i], key)) { json_free(obj->items[i]); obj->items[i] = val; return; }
    if (!grow(obj)) { json_free(val); return; }
    obj->keys[obj->n] = dupn(key, strlen(key));
    obj->items[obj->n++] = val;
}

const char *jgets(const jv *obj, const char *key)
{
    jv *v = jget(obj, key);
    return v && v->type == J_STR ? v->s : NULL;
}

int jgetb(const jv *obj, const char *key, int def)
{
    jv *v = jget(obj, key);
    return v && v->type == J_BOOL ? v->b : def;
}

double jgetn(const jv *obj, const char *key, double def)
{
    jv *v = jget(obj, key);
    return v && v->type == J_NUM ? atof(v->s) : def;
}

/* ---- parsing -------------------------------------------------------------------------------------- */
struct src { const char *p, *end; int depth; };

static void ws(struct src *s)
{
    for (;;) {
        while (s->p < s->end && (*s->p == ' ' || *s->p == '\t' || *s->p == '\r' || *s->p == '\n')) s->p++;
        if (s->p + 1 < s->end && s->p[0] == '/' && s->p[1] == '/') {
            while (s->p < s->end && *s->p != '\n') s->p++;
            continue;
        }
        if (s->p + 1 < s->end && s->p[0] == '/' && s->p[1] == '*') {
            s->p += 2;
            while (s->p + 1 < s->end && !(s->p[0] == '*' && s->p[1] == '/')) s->p++;
            s->p = s->p + 2 <= s->end ? s->p + 2 : s->end;
            continue;
        }
        break;
    }
}

static void put_utf8(char **o, unsigned cp)
{
    char *q = *o;
    if (cp < 0x80) *q++ = (char)cp;
    else if (cp < 0x800) { *q++ = (char)(0xC0 | cp >> 6); *q++ = (char)(0x80 | (cp & 63)); }
    else if (cp < 0x10000) { *q++ = (char)(0xE0 | cp >> 12); *q++ = (char)(0x80 | ((cp >> 6) & 63)); *q++ = (char)(0x80 | (cp & 63)); }
    else { *q++ = (char)(0xF0 | cp >> 18); *q++ = (char)(0x80 | ((cp >> 12) & 63)); *q++ = (char)(0x80 | ((cp >> 6) & 63)); *q++ = (char)(0x80 | (cp & 63)); }
    *o = q;
}

static int hex4(const char *p, const char *end, unsigned *v)
{
    int i;
    *v = 0;
    if (end - p < 4) return 0;
    for (i = 0; i < 4; i++) {
        char c = p[i];
        *v <<= 4;
        if (c >= '0' && c <= '9') *v |= c - '0';
        else if (c >= 'a' && c <= 'f') *v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') *v |= c - 'A' + 10;
        else return 0;
    }
    return 1;
}

static char *string(struct src *s)
{
    const char *start;
    char *out, *o;
    if (s->p >= s->end || *s->p != '"') return NULL;
    start = ++s->p;
    while (s->p < s->end && *s->p != '"') { if (*s->p == '\\') s->p++; s->p++; }
    if (s->p >= s->end) return NULL;
    out = o = malloc((s->p - start) * 2 + 4);
    if (!out) return NULL;
    {
        const char *p = start;
        while (p < s->p) {
            if (*p != '\\') { *o++ = *p++; continue; }
            p++;
            switch (*p) {
            case 'n': *o++ = '\n'; p++; break;
            case 't': *o++ = '\t'; p++; break;
            case 'r': *o++ = '\r'; p++; break;
            case 'b': *o++ = '\b'; p++; break;
            case 'f': *o++ = '\f'; p++; break;
            case 'u': {
                unsigned cp, lo;
                if (!hex4(p + 1, s->p, &cp)) { free(out); return NULL; }
                p += 5;
                if (cp >= 0xD800 && cp < 0xDC00 && p + 1 < s->p && p[0] == '\\' && p[1] == 'u' && hex4(p + 2, s->p, &lo) &&
                    lo >= 0xDC00 && lo < 0xE000) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    p += 6;
                }
                put_utf8(&o, cp);
                break;
            }
            default: *o++ = *p++; break;       /* \" \\ \/ */
            }
        }
    }
    *o = 0;
    s->p++;
    return out;
}

static jv *value(struct src *s);

static jv *container(struct src *s, int obj)
{
    jv *v = make(obj ? J_OBJ : J_ARR);
    char close = obj ? '}' : ']';
    if (!v) return NULL;
    s->p++;
    for (;;) {
        char *key = NULL;
        jv *item;
        ws(s);
        if (s->p < s->end && *s->p == close) { s->p++; return v; }     /* also a trailing comma */
        if (obj) {
            if (!(key = string(s))) break;
            ws(s);
            if (s->p >= s->end || *s->p != ':') { free(key); break; }
            s->p++;
        }
        if (!(item = value(s))) { free(key); break; }
        if (obj) { jset(v, key, item); free(key); }
        else jpush(v, item);
        ws(s);
        if (s->p < s->end && *s->p == ',') { s->p++; continue; }
        if (s->p < s->end && *s->p == close) { s->p++; return v; }
        break;
    }
    json_free(v);
    return NULL;
}

static jv *value(struct src *s)
{
    jv *v = NULL;
    ws(s);
    if (s->p >= s->end || ++s->depth > 200) return NULL;
    switch (*s->p) {
    case '{': v = container(s, 1); break;
    case '[': v = container(s, 0); break;
    case '"': { char *str = string(s); if (str && (v = make(J_STR))) v->s = str; else free(str); break; }
    case 't': if (s->end - s->p >= 4 && !memcmp(s->p, "true", 4)) { s->p += 4; v = jbool(1); } break;
    case 'f': if (s->end - s->p >= 5 && !memcmp(s->p, "false", 5)) { s->p += 5; v = jbool(0); } break;
    case 'n': if (s->end - s->p >= 4 && !memcmp(s->p, "null", 4)) { s->p += 4; v = jnull(); } break;
    default: {
        const char *st = s->p;
        while (s->p < s->end && strchr("+-0123456789.eE", *s->p)) s->p++;
        if (s->p > st && (v = make(J_NUM))) v->s = dupn(st, s->p - st);
        break;
    }
    }
    s->depth--;
    return v;
}

jv *json_parse(const char *text, size_t len)
{
    struct src s = { text, text + len, 0 };
    jv *v;
    if (len >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) s.p += 3;
    v = value(&s);
    if (!v) return NULL;
    ws(&s);
    if (s.p != s.end) { json_free(v); return NULL; }
    return v;
}

/* ---- writing -------------------------------------------------------------------------------------- */
struct out { char *p; size_t n, cap; };

static void emit(struct out *o, const char *s, size_t n)
{
    if (o->n + n + 1 > o->cap) {
        size_t cap = o->cap ? o->cap * 2 : 1024;
        char *p;
        while (cap < o->n + n + 1) cap *= 2;
        if (!(p = realloc(o->p, cap))) return;
        o->p = p; o->cap = cap;
    }
    memcpy(o->p + o->n, s, n);
    o->n += n;
    o->p[o->n] = 0;
}

static void emits(struct out *o, const char *s) { emit(o, s, strlen(s)); }

static void indent(struct out *o, int d) { while (d-- > 0) emits(o, "    "); }

static void wstring(struct out *o, const char *s)
{
    emits(o, "\"");
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        char buf[8];
        if (c == '"') emits(o, "\\\"");
        else if (c == '\\') emits(o, "\\\\");
        else if (c == '\n') emits(o, "\\n");
        else if (c == '\r') emits(o, "\\r");
        else if (c == '\t') emits(o, "\\t");
        else if (c < 0x20) { snprintf(buf, sizeof(buf), "\\u%04x", c); emits(o, buf); }
        else emit(o, s, 1);
    }
    emits(o, "\"");
}

static void wvalue(struct out *o, const jv *v, int d)
{
    int i;
    switch (v->type) {
    case J_NULL: emits(o, "null"); break;
    case J_BOOL: emits(o, v->b ? "true" : "false"); break;
    case J_NUM: emits(o, v->s); break;
    case J_STR: wstring(o, v->s); break;
    case J_ARR:
    case J_OBJ:
        if (!v->n) { emits(o, v->type == J_ARR ? "[]" : "{}"); break; }
        emits(o, v->type == J_ARR ? "[\n" : "{\n");
        for (i = 0; i < v->n; i++) {
            indent(o, d + 1);
            if (v->type == J_OBJ) { wstring(o, v->keys[i]); emits(o, ": "); }
            wvalue(o, v->items[i], d + 1);
            emits(o, i + 1 < v->n ? ",\n" : "\n");
        }
        indent(o, d);
        emits(o, v->type == J_ARR ? "]" : "}");
        break;
    }
}

char *json_write(const jv *v)
{
    struct out o = { 0 };
    wvalue(&o, v, 0);
    emits(&o, "\n");
    return o.p;
}
