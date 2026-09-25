/* test/fontinfo-dump.c -- runs sg-fontview's font file reader (fontinfo.c)
 * natively and prints what it read, one face per line, tab-separated:
 *   FILE kind nfaces
 *   FACE i family full subfamily version weight italic flags typo_family
 *   REG registry-name
 * test/fontview-check.sh compares these with fontTools' reading.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include "../src/fontview/fontinfo.h"

int main(int argc, char **argv)
{
    int a, rc = 0;
    for (a = 1; a < argc; a++) {
        FILE *fp = fopen(argv[a], "rb");
        unsigned char *buf;
        long n;
        static struct fi_file f;
        char reg[1024];
        int i;
        if (!fp) { printf("FILE\t%s\tmissing\n", argv[a]); rc = 1; continue; }
        fseek(fp, 0, SEEK_END); n = ftell(fp); fseek(fp, 0, SEEK_SET);
        buf = malloc((size_t)n + 1);
        if (!buf || fread(buf, 1, (size_t)n, fp) != (size_t)n) { fclose(fp); return 2; }
        fclose(fp);
        if (fi_parse(buf, (size_t)n, &f)) { printf("FILE\t%s\tunreadable\n", argv[a]); free(buf); continue; }
        printf("FILE\t%s\t%d\t%d\n", argv[a], f.kind, f.nfaces);
        for (i = 0; i < f.nfaces; i++) {
            const struct fi_face *x = &f.faces[i];
            printf("FACE\t%d\t%s\t%s\t%s\t%s\t%d\t%d\t%u\t%s\n", i, x->family, x->full, x->subfamily, x->version,
                   x->weight, x->italic, x->flags, x->typo_family);
        }
        fi_registry_name(&f, reg, sizeof(reg));
        printf("REG\t%s\n", reg);
        free(buf);
    }
    return rc;
}
