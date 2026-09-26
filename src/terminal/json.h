/* sg-terminal -- a small JSON document model for settings.json: parsed with
 * Windows Terminal's leniency (// and block comments, trailing commas), kept
 * in order with every key -- known or not -- so a rewrite loses nothing but
 * comments. Strings are UTF-8. Plain C, no Windows headers.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef SG_JSON_H
#define SG_JSON_H
#include <stddef.h>

enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ };

typedef struct jv {
    int type;
    char *s;                    /* J_STR: the string; J_NUM: the number as written */
    int b;                      /* J_BOOL */
    struct jv **items;          /* J_ARR, J_OBJ: values */
    char **keys;                /* J_OBJ: their keys */
    int n, cap;
} jv;

jv *json_parse(const char *text, size_t len);   /* NULL when it is not JSON */
char *json_write(const jv *v);                   /* malloc'd, 4-space indented, ends in a newline */
void json_free(jv *v);

jv *jnull(void);
jv *jbool(int b);
jv *jnum(double d);
jv *jstr(const char *s);
jv *jarr(void);
jv *jobj(void);

jv *jget(const jv *obj, const char *key);        /* NULL when absent or not an object */
void jset(jv *obj, const char *key, jv *val);    /* replaces the key's value in place, or appends */
void jpush(jv *arr, jv *val);
const char *jgets(const jv *obj, const char *key);   /* a string member, or NULL */
int jgetb(const jv *obj, const char *key, int def);
double jgetn(const jv *obj, const char *key, double def);

#endif
