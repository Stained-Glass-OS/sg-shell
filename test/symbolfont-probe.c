/* symbolfont-probe: the symbol characters programs draw by the name "Segoe UI Symbol" come out as the
 * characters, for GDI+ (WinDirStat 2.9 paints its toolbar icons with GraphicsPath::AddString of U+25B6,
 * U+21BB, U+2315 in that family: without the name they were black squares) and for GDI (a font of that
 * face has the glyphs). Prints PASS/FAIL lines and RESULT. */
#include <windows.h>
#include <stdio.h>

typedef int GpStatus;
typedef struct { float X, Y, Width, Height; } GpRectF;
typedef struct { UINT32 GdiplusVersion; void *cb; BOOL a, b; } GdiplusStartupInput;
GpStatus WINAPI GdiplusStartup(ULONG_PTR *, const GdiplusStartupInput *, void *);
GpStatus WINAPI GdipCreateFontFamilyFromName(const WCHAR *, void *, void **);
GpStatus WINAPI GdipCreatePath(int, void **);
GpStatus WINAPI GdipAddPathString(void *, const WCHAR *, INT, void *, INT, float, const GpRectF *, void *);
GpStatus WINAPI GdipGetPointCount(void *, INT *);
GpStatus WINAPI GdipDeletePath(void *);

static int failures;
static void ok(BOOL c, const char *what, int v)
{
    printf("%s  %s (%d)\n", c ? "PASS" : "FAIL", what, v);
    if (!c) failures++;
}

static INT path_points(void *fam, WCHAR c)
{
    void *p; GpRectF r = { 0, 0, 64, 64 }; INT n = 0;
    GdipCreatePath(0, &p);
    if (GdipAddPathString(p, &c, 1, fam, 0, 56, &r, NULL) == 0) GdipGetPointCount(p, &n);
    GdipDeletePath(p);
    return n;
}

int main(void)
{
    static const WCHAR face[] = L"Segoe UI Symbol";
    /* WinDirStat's: resume, refresh, search, recycle */
    static const WCHAR chars[] = { 0x25B6, 0x21BB, 0x2315, 0x267B };
    ULONG_PTR tok; GdiplusStartupInput in = { 1 }; void *fam = NULL;
    WORD idx[ARRAYSIZE(chars)];
    GpStatus st; HDC dc; HFONT f; LOGFONTW lf = { 0 }; char what[96];
    unsigned i; INT n;

    GdiplusStartup(&tok, &in, NULL);
    st = GdipCreateFontFamilyFromName(face, NULL, &fam);
    ok(st == 0 && fam, "GDI+ finds the family \"Segoe UI Symbol\"", st);
    for (i = 0; fam && i < ARRAYSIZE(chars); i++)
    {
        sprintf(what, "GDI+ path of U+%04X has the character's outline", chars[i]);
        n = path_points(fam, chars[i]);
        /* a missing glyph is the font's box, the outline an unassigned code point (U+0378) gets */
        ok(n > 0 && n != path_points(fam, 0x0378), what, n);
    }

    dc = CreateCompatibleDC(NULL);
    lf.lfHeight = -32; lf.lfCharSet = DEFAULT_CHARSET;
    lstrcpyW(lf.lfFaceName, face);
    f = CreateFontIndirectW(&lf);
    SelectObject(dc, f);
    memset(idx, 0, sizeof(idx));
    GetGlyphIndicesW(dc, chars, ARRAYSIZE(chars), idx, GGI_MARK_NONEXISTING_GLYPHS);
    for (i = 0; i < ARRAYSIZE(chars); i++)
    {
        sprintf(what, "GDI font \"Segoe UI Symbol\" has a glyph for U+%04X", chars[i]);
        ok(idx[i] != 0xffff && idx[i] != 0, what, idx[i]);
    }
    printf("RESULT: %s\n", failures ? "FAIL" : "PASS");
    return failures != 0;
}
