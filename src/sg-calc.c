/* sg-calc -- Calculator (calc.exe): Standard, Scientific and Programmer.
 *
 * A Windows 10-style calculator, drawn entirely here: a top bar (the
 * navigation button that switches modes, the mode's name, History), the
 * expression line above the big result, the memory row, and a keypad of
 * flat keys. A wide window shows History and Memory beside the keypad; a
 * narrow one shows them over it.
 *
 *   Standard    immediate execution, as Windows' Standard mode: 2 + 3 x 4
 *               is 20. Percent is a share of the left operand for + and -,
 *               a hundredth for x and /.
 *   Scientific  precedence and parentheses, trigonometry in degrees,
 *               radians or gradians, 2nd functions, F-E notation.
 *   Programmer  64-bit integers cut to QWORD/DWORD/WORD/BYTE, HEX/DEC/OCT/BIN
 *               readouts (click one to type in it), AND OR XOR NOT, Lsh Rsh
 *               (arithmetic), Mod.
 *
 * Repeating = repeats the last operation. Keyboard: digits, + - * / = Enter,
 * Esc (C), Delete (CE), Backspace, %, F9 (negate), ( ), ^, @ (square root),
 * r (1/x), q (x^2), & | ~ < > in Programmer, Ctrl+C / Ctrl+V, Ctrl+M/R/P/Q/L
 * for the memory keys, Ctrl+H History, Alt+1/2/3 the modes.
 *
 * calc.exe resolves here through App Paths (defaults/70-sg-calc.reg), as
 * does the calculator: URI. SG_CALC_DUMP=<file> writes what is shown after
 * every paint (for the gate): the mode, display, expression, readouts,
 * memory, history, and every key's screen rectangle.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#define WIN32_LEAN_AND_MEAN
#define _USE_MATH_DEFINES
#include <windows.h>
#include <windowsx.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#define APP_KEY L"Software\\Stained Glass\\Calculator"

/* ---- colours: the Stained Glass Light palette -------------------------------------------- */
#define C_BG        RGB(236, 234, 240)
#define C_KEY_NUM   RGB(251, 251, 252)
#define C_KEY_FN    RGB(244, 243, 246)
#define C_KEY_HOT   RGB(222, 219, 228)
#define C_KEY_DOWN  RGB(202, 198, 210)
#define C_ACCENT    RGB(112, 48, 192)
#define C_ACCENT_HOT RGB(134, 78, 208)
#define C_ACCENT_DN RGB(90, 34, 160)
#define C_TEXT      RGB(0, 0, 0)
#define C_TEXT2     RGB(96, 96, 104)
#define C_DISABLED  RGB(172, 170, 178)
#define C_PANEL     RGB(248, 247, 250)

enum mode { M_STD, M_SCI, M_PROG };
static const WCHAR *const MODE_NAME[] = { L"Standard", L"Scientific", L"Programmer" };

/* ---- keys --------------------------------------------------------------------------------- */
enum key {
    K_NONE, K_0, K_1, K_2, K_3, K_4, K_5, K_6, K_7, K_8, K_9, K_A, K_B, K_C_HEX, K_D, K_E, K_F,
    K_DOT, K_NEG, K_ADD, K_SUB, K_MUL, K_DIV, K_EQ, K_PCT, K_CE, K_CLR, K_BACK,
    K_RECIP, K_SQR, K_SQRT, K_CUBE, K_CBRT, K_ABS, K_EXP, K_MOD, K_LPAR, K_RPAR, K_FACT,
    K_POW, K_YROOT, K_POW10, K_POW2, K_LOG, K_LOGY, K_LN, K_EPOW, K_PI, K_EULER, K_2ND,
    K_SIN, K_COS, K_TAN, K_ASIN, K_ACOS, K_ATAN, K_ANGLE, K_FE,
    K_AND, K_OR, K_XOR, K_NOT, K_LSH, K_RSH, K_WORD,
    K_HEX, K_DEC, K_OCT, K_BIN,
    K_MC, K_MR, K_MADD, K_MSUB, K_MS,
    K_NAV, K_HIST, K_NAV_STD, K_NAV_SCI, K_NAV_PROG, K_TAB_HIST, K_TAB_MEM, K_CLEAR_LIST,
    K_LIST_ITEM,
    K_COUNT
};

/* label markup: {..} superscript, [..] subscript, \b the backspace glyph */
static const struct keyinfo { const WCHAR *name, *label; char kind; } KEYS[K_COUNT] = {
    [K_0] = { L"0", L"0", 'n' }, [K_1] = { L"1", L"1", 'n' }, [K_2] = { L"2", L"2", 'n' },
    [K_3] = { L"3", L"3", 'n' }, [K_4] = { L"4", L"4", 'n' }, [K_5] = { L"5", L"5", 'n' },
    [K_6] = { L"6", L"6", 'n' }, [K_7] = { L"7", L"7", 'n' }, [K_8] = { L"8", L"8", 'n' },
    [K_9] = { L"9", L"9", 'n' }, [K_A] = { L"a", L"A", 'f' }, [K_B] = { L"b", L"B", 'f' },
    [K_C_HEX] = { L"c", L"C", 'f' }, [K_D] = { L"d", L"D", 'f' }, [K_E] = { L"e", L"E", 'f' },
    [K_F] = { L"f", L"F", 'f' },
    [K_DOT] = { L"dot", L".", 'n' }, [K_NEG] = { L"negate", L"\x00b1", 'n' },
    [K_ADD] = { L"add", L"+", 'f' }, [K_SUB] = { L"sub", L"\x2212", 'f' },
    [K_MUL] = { L"mul", L"\x00d7", 'f' }, [K_DIV] = { L"div", L"\x00f7", 'f' },
    [K_EQ] = { L"eq", L"=", '=' }, [K_PCT] = { L"percent", L"%", 'f' },
    [K_CE] = { L"ce", L"CE", 'f' }, [K_CLR] = { L"clear", L"C", 'f' }, [K_BACK] = { L"back", L"\b", 'f' },
    [K_RECIP] = { L"recip", L"{1}/x", 'f' }, [K_SQR] = { L"sqr", L"x{2}", 'f' },
    [K_SQRT] = { L"sqrt", L"{2}\x221ax", 'f' }, [K_CUBE] = { L"cube", L"x{3}", 'f' },
    [K_CBRT] = { L"cbrt", L"{3}\x221ax", 'f' }, [K_ABS] = { L"abs", L"|x|", 'f' },
    [K_EXP] = { L"exp", L"exp", 'f' }, [K_MOD] = { L"mod", L"mod", 'f' },
    [K_LPAR] = { L"lparen", L"(", 'f' }, [K_RPAR] = { L"rparen", L")", 'f' },
    [K_FACT] = { L"fact", L"n!", 'f' }, [K_POW] = { L"pow", L"x{y}", 'f' },
    [K_YROOT] = { L"yroot", L"{y}\x221ax", 'f' }, [K_POW10] = { L"pow10", L"10{x}", 'f' },
    [K_POW2] = { L"pow2", L"2{x}", 'f' }, [K_LOG] = { L"log", L"log", 'f' },
    [K_LOGY] = { L"logy", L"log[y]x", 'f' }, [K_LN] = { L"ln", L"ln", 'f' },
    [K_EPOW] = { L"epow", L"e{x}", 'f' }, [K_PI] = { L"pi", L"\x03c0", 'f' },
    [K_EULER] = { L"euler", L"e", 'f' }, [K_2ND] = { L"2nd", L"2{nd}", 'f' },
    [K_SIN] = { L"sin", L"sin", 'f' }, [K_COS] = { L"cos", L"cos", 'f' }, [K_TAN] = { L"tan", L"tan", 'f' },
    [K_ASIN] = { L"asin", L"sin{-1}", 'f' }, [K_ACOS] = { L"acos", L"cos{-1}", 'f' },
    [K_ATAN] = { L"atan", L"tan{-1}", 'f' }, [K_ANGLE] = { L"angle", L"DEG", 't' },
    [K_FE] = { L"fe", L"F-E", 't' },
    [K_AND] = { L"and", L"AND", 'f' }, [K_OR] = { L"or", L"OR", 'f' }, [K_XOR] = { L"xor", L"XOR", 'f' },
    [K_NOT] = { L"not", L"NOT", 'f' }, [K_LSH] = { L"lsh", L"Lsh", 'f' }, [K_RSH] = { L"rsh", L"Rsh", 'f' },
    [K_WORD] = { L"word", L"QWORD", 't' },
    [K_HEX] = { L"hex", L"HEX", 'r' }, [K_DEC] = { L"decimal", L"DEC", 'r' },
    [K_OCT] = { L"oct", L"OCT", 'r' }, [K_BIN] = { L"bin", L"BIN", 'r' },
    [K_MC] = { L"mc", L"MC", 'm' }, [K_MR] = { L"mr", L"MR", 'm' }, [K_MADD] = { L"madd", L"M+", 'm' },
    [K_MSUB] = { L"msub", L"M\x2212", 'm' }, [K_MS] = { L"ms", L"MS", 'm' },
    [K_NAV] = { L"nav", L"", 'i' }, [K_HIST] = { L"history", L"", 'i' },
    [K_NAV_STD] = { L"nav-standard", L"Standard", 'v' }, [K_NAV_SCI] = { L"nav-scientific", L"Scientific", 'v' },
    [K_NAV_PROG] = { L"nav-programmer", L"Programmer", 'v' },
    [K_TAB_HIST] = { L"tab-history", L"History", 'b' }, [K_TAB_MEM] = { L"tab-memory", L"Memory", 'b' },
    [K_CLEAR_LIST] = { L"clear-list", L"", 'i' }, [K_LIST_ITEM] = { L"item", L"", 'l' },
};

/* keypads, row by row; K_NONE ends a row */
static const enum key PAD_STD[] = {
    K_PCT, K_CE, K_CLR, K_BACK, K_NONE,
    K_RECIP, K_SQR, K_SQRT, K_DIV, K_NONE,
    K_7, K_8, K_9, K_MUL, K_NONE,
    K_4, K_5, K_6, K_SUB, K_NONE,
    K_1, K_2, K_3, K_ADD, K_NONE,
    K_NEG, K_0, K_DOT, K_EQ, K_NONE, K_COUNT };
static const enum key PAD_SCI[] = {
    K_ANGLE, K_FE, K_SIN, K_COS, K_TAN, K_NONE,
    K_2ND, K_PI, K_EULER, K_CLR, K_BACK, K_NONE,
    K_SQR, K_RECIP, K_ABS, K_EXP, K_MOD, K_NONE,
    K_SQRT, K_LPAR, K_RPAR, K_FACT, K_DIV, K_NONE,
    K_POW, K_7, K_8, K_9, K_MUL, K_NONE,
    K_POW10, K_4, K_5, K_6, K_SUB, K_NONE,
    K_LOG, K_1, K_2, K_3, K_ADD, K_NONE,
    K_LN, K_NEG, K_0, K_DOT, K_EQ, K_NONE, K_COUNT };
static const enum key PAD_PROG[] = {
    K_AND, K_OR, K_XOR, K_NOT, K_WORD, K_NONE,
    K_A, K_LSH, K_RSH, K_CLR, K_BACK, K_NONE,
    K_B, K_LPAR, K_RPAR, K_MOD, K_DIV, K_NONE,
    K_C_HEX, K_7, K_8, K_9, K_MUL, K_NONE,
    K_D, K_4, K_5, K_6, K_SUB, K_NONE,
    K_E, K_1, K_2, K_3, K_ADD, K_NONE,
    K_F, K_NEG, K_0, K_DOT, K_EQ, K_NONE, K_COUNT };

struct button { enum key key; RECT rc; BOOL enabled; int index; };
#define MAX_BUTTONS 160
static struct button g_btn[MAX_BUTTONS];
static int g_nbtn, g_hover = -1, g_pressed = -1;

/* ---- state -------------------------------------------------------------------------------- */
typedef struct { double d; long long i; } num;

static enum mode g_mode;
static WCHAR g_entry[80];           /* the number being typed, in the current radix */
static BOOL g_typing;
static num g_cur;                   /* the value shown, when not typing */
static WCHAR g_curtext[512];        /* the current operand as the expression shows it */
static WCHAR g_expr[1024];
static num g_vals[64]; static int g_nvals;
static enum key g_ops[64]; static int g_nops;
static int g_paren_start[64];       /* expression offset of each open parenthesis */
static BOOL g_after_op, g_eq_done, g_paren_operand;
static int g_paren_operand_at;
static enum key g_last_op; static num g_last_rhs; static BOOL g_have_last;
static const WCHAR *g_error;
static BOOL g_second, g_fe;
static int g_angle;                 /* 0 degrees, 1 radians, 2 gradians */
static int g_radix = 10, g_bits = 64;

#define MAX_LIST 64
static struct { WCHAR expr[1024], result[128]; num v; } g_hist[MAX_LIST];
static int g_nhist;
static num g_mem[MAX_LIST]; static int g_nmem;
static BOOL g_side, g_overlay, g_nav, g_show_mem;

static HWND g_hwnd;
static int g_dpi = 96;
static WCHAR g_dump[MAX_PATH];

static int S(int v) { return MulDiv(v, g_dpi, 96); }

/* ---- numbers ------------------------------------------------------------------------------ */
static long long to_width(long long v)
{
    unsigned long long u = (unsigned long long)v;
    if (g_bits == 64) return v;
    u &= (1ULL << g_bits) - 1;
    if (u & (1ULL << (g_bits - 1))) u |= ~((1ULL << g_bits) - 1);
    return (long long)u;
}

static unsigned long long width_mask(void) { return g_bits == 64 ? ~0ULL : (1ULL << g_bits) - 1; }

static void group(const WCHAR *in, WCHAR *out, int every, WCHAR sep)
{
    /* groups the leading run of digits (after an optional sign), from the right */
    int n = 0, i, o = 0, start = 0, len = lstrlenW(in);
    if (in[0] == '-') { out[o++] = '-'; start = 1; }
    while (start + n < len && iswxdigit(in[start + n])) n++;
    for (i = 0; i < n; i++)
    {
        if (i && (n - i) % every == 0) out[o++] = sep;
        out[o++] = in[start + i];
    }
    lstrcpyW(out + o, in + start + n);
}

/* 16 significant digits, as Windows shows, exponential beyond that range */
static void fmt_double(double v, WCHAR *out, int cch, BOOL grouped)
{
    WCHAR raw[128], *e;
    int e10;
    if (v == 0) { lstrcpynW(out, L"0", cch); return; }
    e10 = (int)floor(log10(fabs(v)));
    if (!g_fe && e10 < 16 && e10 > -17)
    {
        int decimals = 15 - e10;
        if (decimals < 0) decimals = 0;
        if (decimals > 30) decimals = 30;
        _snwprintf(raw, 128, L"%.*f", decimals, v);
        raw[127] = 0;
        if (wcschr(raw, '.'))
        {
            WCHAR *end = raw + lstrlenW(raw) - 1;
            while (*end == '0') *end-- = 0;
            if (*end == '.') *end = 0;
        }
        if (!lstrcmpW(raw, L"-0")) lstrcpyW(raw, L"0");
        if (grouped) group(raw, out, 3, ','); else lstrcpynW(out, raw, cch);
        return;
    }
    _snwprintf(raw, 128, L"%.15e", v);
    raw[127] = 0;
    if ((e = wcschr(raw, 'e')))
    {
        WCHAR exp[32], *m = e - 1;
        int ev = _wtoi(e + 1);
        while (*m == '0') m--;
        m[1] = 0;
        _snwprintf(exp, 32, L"e%c%d", ev < 0 ? '-' : '+', abs(ev));
        lstrcatW(raw, exp);
    }
    lstrcpynW(out, raw, cch);
}

static void fmt_int(long long v, int radix, WCHAR *out, BOOL grouped)
{
    WCHAR raw[80], tmp[80];
    unsigned long long u = (unsigned long long)v & width_mask();
    int n = 0, i;
    if (radix == 10)
    {
        _snwprintf(raw, 80, L"%lld", to_width(v));
        if (grouped) group(raw, out, 3, ','); else lstrcpyW(out, raw);
        return;
    }
    do { int d = (int)(u % radix); tmp[n++] = d < 10 ? '0' + d : 'A' + d - 10; u /= radix; } while (u);
    for (i = 0; i < n; i++) raw[i] = tmp[n - 1 - i];
    raw[n] = 0;
    if (grouped) group(raw, out, radix == 8 ? 3 : 4, ' '); else lstrcpyW(out, raw);
}

static void fmt_num(num v, WCHAR *out, int cch, BOOL grouped)
{
    if (g_mode == M_PROG) fmt_int(v.i, g_radix, out, grouped);
    else fmt_double(v.d, out, cch, grouped);
}

static num make_d(double d) { num n = { d, (long long)d }; return n; }
static num make_i(long long i) { num n = { (double)i, to_width(i) }; return n; }

/* the typed entry's value */
static num entry_value(void)
{
    if (g_mode == M_PROG)
    {
        const WCHAR *p = g_entry;
        BOOL neg = FALSE;
        unsigned long long u = 0;
        if (*p == '-') { neg = TRUE; p++; }
        for (; *p; p++)
        {
            int d = iswdigit(*p) ? *p - '0' : towupper(*p) - 'A' + 10;
            u = u * g_radix + d;
        }
        return make_i(neg ? -(long long)u : (long long)u);
    }
    return make_d(wcstod(g_entry, NULL));
}

static num cur_value(void) { return g_typing ? entry_value() : g_cur; }

static void show_error(const WCHAR *msg) { g_error = msg; g_typing = FALSE; }

/* ---- memory and history ------------------------------------------------------------------ */
static void save_settings(void)
{
    HKEY k;
    DWORD v;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, APP_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL)) return;
    v = g_mode; RegSetValueExW(k, L"Mode", 0, REG_DWORD, (BYTE *)&v, sizeof(v));
    v = g_angle; RegSetValueExW(k, L"Angle", 0, REG_DWORD, (BYTE *)&v, sizeof(v));
    v = g_bits; RegSetValueExW(k, L"WordSize", 0, REG_DWORD, (BYTE *)&v, sizeof(v));
    RegCloseKey(k);
}

static DWORD load_dword(const WCHAR *name, DWORD def)
{
    DWORD v = def, cb = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, APP_KEY, name, RRF_RT_REG_DWORD, NULL, &v, &cb)) return def;
    return v;
}

static void add_history(const WCHAR *expr, num v)
{
    if (g_nhist == MAX_LIST) { memmove(g_hist, g_hist + 1, sizeof(g_hist[0]) * (MAX_LIST - 1)); g_nhist--; }
    lstrcpynW(g_hist[g_nhist].expr, expr, 1024);
    fmt_num(v, g_hist[g_nhist].result, 128, TRUE);
    g_hist[g_nhist].v = v;
    g_nhist++;
}

/* ---- the engine --------------------------------------------------------------------------- */
static int precedence(enum key op)
{
    if (g_mode == M_STD) return 1;             /* immediate execution */
    switch (op)
    {
    case K_OR: return 1;
    case K_XOR: return 2;
    case K_AND: return 3;
    case K_LSH: case K_RSH: return 4;
    case K_ADD: case K_SUB: return 5;
    case K_MUL: case K_DIV: case K_MOD: return 6;
    case K_POW: case K_YROOT: case K_LOGY: case K_EXP: return 7;
    default: return 0;
    }
}

static const WCHAR *op_text(enum key op)
{
    switch (op)
    {
    case K_ADD: return L"+"; case K_SUB: return L"\x2212"; case K_MUL: return L"\x00d7";
    case K_DIV: return L"\x00f7"; case K_MOD: return L"Mod"; case K_POW: return L"^";
    case K_YROOT: return L"yroot"; case K_LOGY: return L"log base"; case K_EXP: return L"e+";
    case K_AND: return L"AND"; case K_OR: return L"OR"; case K_XOR: return L"XOR";
    case K_LSH: return L"Lsh"; case K_RSH: return L"Rsh";
    default: return L"?";
    }
}

static BOOL apply_binary(enum key op, num a, num b, num *r)
{
    if (g_mode == M_PROG)
    {
        long long x = a.i, y = b.i, v = 0;
        switch (op)
        {
        case K_ADD: v = (long long)((unsigned long long)x + (unsigned long long)y); break;
        case K_SUB: v = (long long)((unsigned long long)x - (unsigned long long)y); break;
        case K_MUL: v = (long long)((unsigned long long)x * (unsigned long long)y); break;
        case K_DIV: case K_MOD:
            if (!y) { show_error(L"Cannot divide by zero"); return FALSE; }
            if (x == LLONG_MIN && y == -1) v = op == K_DIV ? x : 0;
            else v = op == K_DIV ? x / y : x % y;
            break;
        case K_AND: v = x & y; break;
        case K_OR: v = x | y; break;
        case K_XOR: v = x ^ y; break;
        case K_LSH: v = (y < 0 || y >= 64) ? 0 : (long long)((unsigned long long)x << y); break;
        case K_RSH: v = (y < 0 || y >= 64) ? (x < 0 ? -1 : 0) : x >> y; break;
        default: return FALSE;
        }
        *r = make_i(v);
        return TRUE;
    }
    else
    {
        double x = a.d, y = b.d, v;
        switch (op)
        {
        case K_ADD: v = x + y; break;
        case K_SUB: v = x - y; break;
        case K_MUL: v = x * y; break;
        case K_DIV:
            if (y == 0) { show_error(x == 0 ? L"Result is undefined" : L"Cannot divide by zero"); return FALSE; }
            v = x / y; break;
        case K_MOD:
            if (y == 0) { show_error(L"Result is undefined"); return FALSE; }
            v = fmod(x, y); break;
        case K_POW: v = pow(x, y); break;
        case K_YROOT:
            if (y == 0) { show_error(L"Invalid input"); return FALSE; }
            if (x < 0 && fmod(y, 2) == 1) v = -pow(-x, 1 / y);
            else v = pow(x, 1 / y);
            break;
        case K_LOGY:
            if (x <= 0 || y <= 0 || y == 1) { show_error(L"Invalid input"); return FALSE; }
            v = log(x) / log(y); break;
        case K_EXP: v = x * pow(10, y); break;
        default: return FALSE;
        }
        if (isnan(v)) { show_error(L"Invalid input"); return FALSE; }
        if (isinf(v)) { show_error(L"Overflow"); return FALSE; }
        *r = make_d(v);
        return TRUE;
    }
}

/* apply the operators on the stack down to (not past) an open parenthesis,
 * while their precedence is at least min_prec */
static BOOL reduce(int min_prec)
{
    while (g_nops && g_ops[g_nops - 1] != K_LPAR && precedence(g_ops[g_nops - 1]) >= min_prec && g_nvals >= 2)
    {
        num r;
        enum key op = g_ops[--g_nops];
        if (!apply_binary(op, g_vals[g_nvals - 2], g_vals[g_nvals - 1], &r)) return FALSE;
        g_nvals -= 2;
        g_vals[g_nvals++] = r;
    }
    return TRUE;
}

static void reset_all(void)
{
    lstrcpyW(g_entry, L"0");
    g_typing = FALSE;
    g_cur = make_d(0);
    g_curtext[0] = g_expr[0] = 0;
    g_nvals = g_nops = 0;
    g_after_op = g_eq_done = g_paren_operand = FALSE;
    g_have_last = FALSE;
    g_error = NULL;
}

/* the text of the current operand for the expression line */
static void operand_text(WCHAR *out, int cch)
{
    if (g_curtext[0]) lstrcpynW(out, g_curtext, cch);
    else { num v = cur_value(); fmt_num(v, out, cch, FALSE); }
}

static void expr_append(const WCHAR *s)
{
    int n = lstrlenW(g_expr);
    lstrcpynW(g_expr + n, s, 1024 - n);
}

/* leaving an error or a finished calculation when a new number starts */
static void start_operand(void)
{
    if (g_error) reset_all();
    if (g_eq_done) { reset_all(); }
    if (g_paren_operand) { g_expr[g_paren_operand_at] = 0; g_paren_operand = FALSE; }
    g_curtext[0] = 0;
}

static int digit_value(enum key k)
{
    if (k >= K_0 && k <= K_9) return k - K_0;
    if (k >= K_A && k <= K_F) return 10 + (k - K_A);
    return -1;
}

static BOOL digit_allowed(enum key k)
{
    int d = digit_value(k);
    if (g_mode != M_PROG) return d >= 0 && d < 10;
    return d >= 0 && d < g_radix;
}

static void press_digit(enum key k)
{
    int d = digit_value(k);
    WCHAR c = d < 10 ? '0' + d : 'A' + d - 10;
    int len, digits = 0;
    const WCHAR *p;
    if (!digit_allowed(k)) return;
    if (!g_typing || g_error || g_eq_done)
    {
        start_operand();
        lstrcpyW(g_entry, L"0");
        g_typing = TRUE;
    }
    g_after_op = FALSE;
    for (p = g_entry; *p; p++) if (iswxdigit(*p)) digits++;
    len = lstrlenW(g_entry);
    if (g_mode == M_PROG)
    {
        /* only while the value still fits the word */
        WCHAR save[80];
        unsigned long long u = 0, limit = g_radix == 10 ? width_mask() >> 1 : width_mask();
        BOOL over = FALSE;
        lstrcpyW(save, g_entry);
        if (!lstrcmpW(g_entry, L"0")) g_entry[0] = 0; else if (!lstrcmpW(g_entry, L"-0")) g_entry[1] = 0;
        len = lstrlenW(g_entry);
        g_entry[len] = c; g_entry[len + 1] = 0;
        for (p = g_entry; *p; p++)
        {
            int dv;
            if (*p == '-') continue;
            dv = iswdigit(*p) ? *p - '0' : towupper(*p) - 'A' + 10;
            if (u > (limit - dv) / g_radix) { over = TRUE; break; }
            u = u * g_radix + dv;
        }
        if (over || lstrlenW(g_entry) > 70) lstrcpyW(g_entry, save);
        return;
    }
    if (digits >= 16) return;
    if (!lstrcmpW(g_entry, L"0")) { g_entry[0] = c; g_entry[1] = 0; return; }
    if (!lstrcmpW(g_entry, L"-0")) { g_entry[1] = c; g_entry[2] = 0; return; }
    g_entry[len] = c; g_entry[len + 1] = 0;
}

static void press_dot(void)
{
    if (g_mode == M_PROG) return;
    if (!g_typing || g_error || g_eq_done)
    {
        start_operand();
        lstrcpyW(g_entry, L"0");
        g_typing = TRUE;
    }
    g_after_op = FALSE;
    if (!wcschr(g_entry, '.')) lstrcatW(g_entry, L".");
}

static void press_binary(enum key op)
{
    num v;
    WCHAR t[512];
    if (g_error) return;
    if (g_after_op && g_nops && g_ops[g_nops - 1] != K_LPAR)
    {
        /* another operator straight after one replaces it */
        int n = lstrlenW(g_expr);
        WCHAR *sp;
        g_ops[g_nops - 1] = op;
        if (n && g_expr[n - 1] == ' ') g_expr[n - 1] = 0;
        if ((sp = wcsrchr(g_expr, ' '))) sp[1] = 0;
        expr_append(op_text(op)); expr_append(L" ");
        return;
    }
    if (g_eq_done) { g_expr[0] = 0; g_nvals = g_nops = 0; g_eq_done = FALSE; }
    v = cur_value();
    if (!g_paren_operand) { operand_text(t, 512); expr_append(t); expr_append(L" "); }
    else expr_append(L" ");
    g_paren_operand = FALSE;
    g_vals[g_nvals++] = v;
    if (!reduce(precedence(op))) return;
    g_ops[g_nops++] = op;
    expr_append(op_text(op)); expr_append(L" ");
    g_cur = g_vals[g_nvals - 1];
    g_typing = FALSE;
    g_curtext[0] = 0;
    g_after_op = TRUE;
}

static void press_equals(void)
{
    num v, r;
    WCHAR t[512], a[128], b[128];
    int i;
    if (g_error) { reset_all(); return; }
    if (g_eq_done)
    {
        if (!g_have_last) return;
        if (!apply_binary(g_last_op, g_cur, g_last_rhs, &r)) return;
        fmt_num(g_cur, a, 128, FALSE); fmt_num(g_last_rhs, b, 128, FALSE);
        _snwprintf(g_expr, 1024, L"%ls %ls %ls =", a, op_text(g_last_op), b);
        g_expr[1023] = 0;
        g_cur = r;
        add_history(g_expr, r);
        return;
    }
    v = g_after_op ? g_cur : cur_value();
    if (!g_nops && !g_nvals)
    {
        operand_text(t, 512);
        if (!g_paren_operand) expr_append(t);
        expr_append(L" =");
        g_cur = v; g_typing = FALSE; g_eq_done = TRUE; g_curtext[0] = 0; g_paren_operand = FALSE;
        return;
    }
    if (g_paren_operand) expr_append(L" ");
    else { if (g_after_op) fmt_num(v, t, 512, FALSE); else operand_text(t, 512); expr_append(t); expr_append(L" "); }
    g_paren_operand = FALSE;
    /* close what is open */
    for (i = g_nops - 1; i >= 0; i--)
        if (g_ops[i] == K_LPAR) { int n = lstrlenW(g_expr); if (n && g_expr[n - 1] == ' ') g_expr[n - 1] = 0; expr_append(L") "); }
    g_vals[g_nvals++] = v;
    g_have_last = FALSE;
    for (i = g_nops - 1; i >= 0; i--) if (g_ops[i] != K_LPAR) { g_last_op = g_ops[i]; g_have_last = TRUE; break; }
    g_last_rhs = v;
    while (g_nops)
    {
        int before = g_nops;
        if (g_ops[g_nops - 1] == K_LPAR) { g_nops--; continue; }
        if (!reduce(0)) return;
        if (g_nops == before) g_nops--;        /* an operator with nothing to apply to */
    }
    expr_append(L"=");
    g_cur = g_nvals ? g_vals[g_nvals - 1] : v;
    g_nvals = g_nops = 0;
    g_typing = FALSE; g_after_op = FALSE; g_eq_done = TRUE; g_curtext[0] = 0;
    add_history(g_expr, g_cur);
}

static void press_lparen(void)
{
    if (g_mode == M_STD || g_error) return;
    if (g_eq_done) reset_all();
    if (g_typing || g_curtext[0] || g_paren_operand) return;   /* only where an operand starts */
    if (g_nops >= 60) return;
    g_paren_start[g_nops] = lstrlenW(g_expr);
    g_ops[g_nops++] = K_LPAR;
    expr_append(L"(");
    g_after_op = FALSE;
    g_cur = make_d(0); if (g_mode == M_PROG) g_cur = make_i(0);
}

static void press_rparen(void)
{
    num v;
    WCHAR t[512];
    int i, open = -1, start;
    if (g_error) return;
    for (i = g_nops - 1; i >= 0; i--) if (g_ops[i] == K_LPAR) { open = i; break; }
    if (open < 0) return;
    start = g_paren_start[open];
    v = g_after_op ? g_cur : cur_value();
    if (!g_paren_operand) { if (g_after_op) fmt_num(v, t, 512, FALSE); else operand_text(t, 512); expr_append(t); }
    g_vals[g_nvals++] = v;
    if (!reduce(0)) return;
    g_nops--;                                  /* the parenthesis */
    expr_append(L")");
    g_cur = g_vals[--g_nvals];
    g_typing = FALSE; g_after_op = FALSE; g_curtext[0] = 0;
    g_paren_operand = TRUE; g_paren_operand_at = start;
}

static double to_rad(double x) { return g_angle == 0 ? x * M_PI / 180 : g_angle == 2 ? x * M_PI / 200 : x; }
static double from_rad(double x) { return g_angle == 0 ? x * 180 / M_PI : g_angle == 2 ? x * 200 / M_PI : x; }
static double clean15(double v) { WCHAR b[64]; _snwprintf(b, 64, L"%.15g", v); return wcstod(b, NULL); }

static void press_unary(enum key k)
{
    num v, r;
    WCHAR base[512], text[600];
    const WCHAR *fn = NULL;
    double x, y = 0;
    if (g_error) return;
    v = cur_value();
    x = v.d;
    if (g_paren_operand)
    {
        lstrcpynW(base, g_expr + g_paren_operand_at, 512);
        g_expr[g_paren_operand_at] = 0;
        g_paren_operand = FALSE;
    }
    else operand_text(base, 512);
    if (g_eq_done) { g_expr[0] = 0; g_nvals = g_nops = 0; g_eq_done = FALSE; }
    if (g_mode == M_PROG)
    {
        if (k == K_NOT) { r = make_i(~v.i); fn = L"NOT"; }
        else if (k == K_NEG) { r = make_i(-v.i); fn = L"negate"; }
        else return;
    }
    else
    {
        switch (k)
        {
        case K_RECIP: if (x == 0) { show_error(L"Cannot divide by zero"); return; } y = 1 / x; fn = L"1/"; break;
        case K_SQR: y = x * x; fn = L"sqr"; break;
        case K_CUBE: y = x * x * x; fn = L"cube"; break;
        case K_SQRT: if (x < 0) { show_error(L"Invalid input"); return; } y = sqrt(x); fn = L"\x221a"; break;
        case K_CBRT: y = cbrt(x); fn = L"cuberoot"; break;
        case K_ABS: y = fabs(x); fn = L"abs"; break;
        case K_NEG: y = -x; fn = L"negate"; break;
        case K_FACT:
            if (x < 0 && x == floor(x)) { show_error(L"Invalid input"); return; }
            if (x == floor(x) && x <= 170) { double f = 1; int i; for (i = 2; i <= (int)x; i++) f *= i; y = f; }
            else y = tgamma(x + 1);
            fn = L"fact"; break;
        case K_POW10: y = pow(10, x); fn = L"10^"; break;
        case K_POW2: y = pow(2, x); fn = L"2^"; break;
        case K_EPOW: y = exp(x); fn = L"e^"; break;
        case K_LOG: if (x <= 0) { show_error(L"Invalid input"); return; } y = log10(x); fn = L"log"; break;
        case K_LN: if (x <= 0) { show_error(L"Invalid input"); return; } y = log(x); fn = L"ln"; break;
        case K_SIN: case K_COS: case K_TAN:
        {
            double period = g_angle == 0 ? 180 : g_angle == 2 ? 200 : 0;
            if (period && k == K_TAN && fmod(fabs(x) - period / 2, period) == 0) { show_error(L"Invalid input"); return; }
            if (period && k == K_SIN && fmod(x, period) == 0) y = 0;
            else if (period && k == K_COS && fmod(fabs(x) - period / 2, period) == 0) y = 0;
            else y = clean15(k == K_SIN ? sin(to_rad(x)) : k == K_COS ? cos(to_rad(x)) : tan(to_rad(x)));
            fn = k == K_SIN ? L"sin" : k == K_COS ? L"cos" : L"tan";
            break;
        }
        case K_ASIN: case K_ACOS:
            if (x < -1 || x > 1) { show_error(L"Invalid input"); return; }
            y = clean15(from_rad(k == K_ASIN ? asin(x) : acos(x))); fn = k == K_ASIN ? L"sin\x207b\x00b9" : L"cos\x207b\x00b9"; break;
        case K_ATAN: y = clean15(from_rad(atan(x))); fn = L"tan\x207b\x00b9"; break;
        default: return;
        }
        if (isnan(y)) { show_error(L"Invalid input"); return; }
        if (isinf(y)) { show_error(L"Overflow"); return; }
        r = make_d(y);
    }
    if (k == K_RECIP) _snwprintf(text, 600, L"1/(%ls)", base);
    else _snwprintf(text, 600, L"%ls(%ls)", fn, base);
    text[599] = 0;
    lstrcpynW(g_curtext, text, 512);
    g_cur = r;
    g_typing = FALSE;
    g_after_op = FALSE;
}

static void press_negate(void)
{
    if (g_typing && !g_error)
    {
        /* while typing, the sign of the entry flips */
        if (g_entry[0] == '-') memmove(g_entry, g_entry + 1, lstrlenW(g_entry) * sizeof(WCHAR));
        else if (lstrcmpW(g_entry, L"0")) { memmove(g_entry + 1, g_entry, (lstrlenW(g_entry) + 1) * sizeof(WCHAR)); g_entry[0] = '-'; }
        return;
    }
    press_unary(K_NEG);
}

static void press_percent(void)
{
    num v = cur_value(), r;
    WCHAR t[128];
    if (g_error) return;
    if (g_mode == M_PROG) { press_binary(K_MOD); return; }
    if (g_eq_done) { g_expr[0] = 0; g_nvals = g_nops = 0; g_eq_done = FALSE; }
    if (g_nops && g_nvals && (g_ops[g_nops - 1] == K_ADD || g_ops[g_nops - 1] == K_SUB))
        r = make_d(g_vals[g_nvals - 1].d * v.d / 100);
    else if (g_nops && (g_ops[g_nops - 1] == K_MUL || g_ops[g_nops - 1] == K_DIV))
        r = make_d(v.d / 100);
    else r = make_d(0);
    fmt_num(r, t, 128, FALSE);
    lstrcpyW(g_curtext, t);
    g_cur = r;
    g_typing = FALSE;
    g_after_op = FALSE;
}

static void press_constant(double c)
{
    start_operand();
    g_cur = make_d(c);
    g_typing = FALSE;
    g_after_op = FALSE;
}

static void press_back(void)
{
    int n;
    if (g_error) { reset_all(); return; }
    if (g_eq_done) { g_expr[0] = 0; return; }
    if (!g_typing) return;
    n = lstrlenW(g_entry);
    if (n) g_entry[--n] = 0;
    if (!n || !lstrcmpW(g_entry, L"-")) lstrcpyW(g_entry, L"0");
}

static void press_ce(void)
{
    if (g_error || g_eq_done) { reset_all(); return; }
    if (g_paren_operand) { g_expr[g_paren_operand_at] = 0; g_paren_operand = FALSE; }
    lstrcpyW(g_entry, L"0");
    g_typing = TRUE;
    g_curtext[0] = 0;
    g_after_op = FALSE;
}

static void memory(enum key k)
{
    num v;
    if (g_error) return;
    v = cur_value();
    switch (k)
    {
    case K_MC: g_nmem = 0; break;
    case K_MR:
        if (!g_nmem) return;
        start_operand();
        g_cur = g_mem[g_nmem - 1]; g_typing = FALSE; g_after_op = FALSE;
        break;
    case K_MS:
        if (g_nmem == MAX_LIST) { memmove(g_mem, g_mem + 1, sizeof(num) * (MAX_LIST - 1)); g_nmem--; }
        g_mem[g_nmem++] = v;
        g_typing = FALSE; g_cur = v;
        break;
    case K_MADD: case K_MSUB:
        if (!g_nmem) g_mem[g_nmem++] = g_mode == M_PROG ? make_i(0) : make_d(0);
        if (g_mode == M_PROG) g_mem[g_nmem - 1] = make_i(k == K_MADD ? g_mem[g_nmem - 1].i + v.i : g_mem[g_nmem - 1].i - v.i);
        else g_mem[g_nmem - 1] = make_d(k == K_MADD ? g_mem[g_nmem - 1].d + v.d : g_mem[g_nmem - 1].d - v.d);
        g_typing = FALSE; g_cur = v;
        break;
    default: break;
    }
}

static void set_mode(enum mode m)
{
    if (m == g_mode) return;
    g_mode = m;
    g_radix = 10;
    g_second = FALSE;
    g_nmem = 0;                    /* each mode keeps its own kind of number */
    g_nhist = 0;
    reset_all();
    g_cur = m == M_PROG ? make_i(0) : make_d(0);
    save_settings();
}

static void set_radix(int radix)
{
    num v = cur_value();
    if (g_mode != M_PROG) return;
    g_radix = radix;
    g_typing = FALSE;
    g_cur = v;
    g_curtext[0] = 0;
}

static void cycle_word(void)
{
    num v = cur_value();
    g_bits = g_bits == 64 ? 32 : g_bits == 32 ? 16 : g_bits == 16 ? 8 : 64;
    g_cur = make_i(v.i);
    g_typing = FALSE;
    save_settings();
}

/* ---- clipboard ---------------------------------------------------------------------------- */
static void display_text(WCHAR *out, int cch, BOOL grouped)
{
    if (g_error) { lstrcpynW(out, g_error, cch); return; }
    if (g_typing)
    {
        if (!grouped) { lstrcpynW(out, g_entry, cch); return; }
        if (g_mode == M_PROG) group(g_entry, out, g_radix == 10 ? 3 : g_radix == 8 ? 3 : 4, g_radix == 10 ? ',' : ' ');
        else group(g_entry, out, 3, ',');
        return;
    }
    fmt_num(g_cur, out, cch, grouped);
}

static void copy_result(void)
{
    WCHAR t[128];
    HGLOBAL h;
    display_text(t, 128, FALSE);
    if (!OpenClipboard(g_hwnd)) return;
    EmptyClipboard();
    if ((h = GlobalAlloc(GMEM_MOVEABLE, (lstrlenW(t) + 1) * sizeof(WCHAR))))
    {
        lstrcpyW(GlobalLock(h), t);
        GlobalUnlock(h);
        SetClipboardData(CF_UNICODETEXT, h);
    }
    CloseClipboard();
}

static void paste(void)
{
    WCHAR t[128], clean[128];
    HANDLE h;
    int o = 0, i, radix = g_radix;
    BOOL ok = FALSE;
    if (!OpenClipboard(g_hwnd)) return;
    if ((h = GetClipboardData(CF_UNICODETEXT)))
    {
        const WCHAR *p = GlobalLock(h);
        if (p) { lstrcpynW(t, p, 128); ok = TRUE; GlobalUnlock(h); }
    }
    CloseClipboard();
    if (!ok) return;
    for (i = 0; t[i] && o < 120; i++)
        if (t[i] != ' ' && t[i] != ',' && t[i] != '\t' && t[i] != '\r' && t[i] != '\n' && t[i] != 0x2212) clean[o++] = t[i];
        else if (t[i] == 0x2212) clean[o++] = '-';
    clean[o] = 0;
    if (!o) return;
    if (g_mode == M_PROG)
    {
        WCHAR *end, *s = clean;
        unsigned long long u;
        BOOL neg = FALSE;
        if (*s == '-') { neg = TRUE; s++; }
        if ((s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))) { radix = 16; s += 2; }
        if (!*s) return;
        u = wcstoull(s, &end, radix);
        if (*end) return;
        start_operand();
        g_cur = make_i(neg ? -(long long)u : (long long)u);
    }
    else
    {
        WCHAR *end;
        double d = wcstod(clean, &end);
        if (*end || !isfinite(d)) return;
        start_operand();
        g_cur = make_d(d);
    }
    g_typing = FALSE;
    g_after_op = FALSE;
}

/* ---- one key ------------------------------------------------------------------------------ */
static void press(enum key k, int index)
{
    switch (k)
    {
    case K_0: case K_1: case K_2: case K_3: case K_4: case K_5: case K_6: case K_7: case K_8: case K_9:
    case K_A: case K_B: case K_C_HEX: case K_D: case K_E: case K_F: press_digit(k); break;
    case K_DOT: press_dot(); break;
    case K_NEG: press_negate(); break;
    case K_ADD: case K_SUB: case K_MUL: case K_DIV: case K_MOD: case K_POW: case K_YROOT: case K_LOGY:
    case K_EXP: case K_AND: case K_OR: case K_XOR: case K_LSH: case K_RSH:
        press_binary(k); break;
    case K_EQ: press_equals(); break;
    case K_PCT: press_percent(); break;
    case K_CE: press_ce(); break;
    case K_CLR: reset_all(); if (g_mode == M_PROG) g_cur = make_i(0); break;
    case K_BACK: press_back(); break;
    case K_RECIP: case K_SQR: case K_SQRT: case K_CUBE: case K_CBRT: case K_ABS: case K_FACT:
    case K_POW10: case K_POW2: case K_LOG: case K_LN: case K_EPOW: case K_SIN: case K_COS: case K_TAN:
    case K_ASIN: case K_ACOS: case K_ATAN: case K_NOT:
        press_unary(k); break;
    case K_LPAR: press_lparen(); break;
    case K_RPAR: press_rparen(); break;
    case K_PI: press_constant(M_PI); break;
    case K_EULER: press_constant(M_E); break;
    case K_2ND: g_second = !g_second; break;
    case K_ANGLE: g_angle = (g_angle + 1) % 3; save_settings(); break;
    case K_FE: g_fe = !g_fe; break;
    case K_WORD: cycle_word(); break;
    case K_HEX: set_radix(16); break;
    case K_DEC: set_radix(10); break;
    case K_OCT: set_radix(8); break;
    case K_BIN: set_radix(2); break;
    case K_MC: case K_MR: case K_MADD: case K_MSUB: case K_MS: memory(k); break;
    case K_NAV: g_nav = !g_nav; break;
    case K_NAV_STD: set_mode(M_STD); g_nav = FALSE; break;
    case K_NAV_SCI: set_mode(M_SCI); g_nav = FALSE; break;
    case K_NAV_PROG: set_mode(M_PROG); g_nav = FALSE; break;
    case K_HIST: g_overlay = !g_overlay; break;
    case K_TAB_HIST: g_show_mem = FALSE; break;
    case K_TAB_MEM: g_show_mem = TRUE; break;
    case K_CLEAR_LIST: if (g_show_mem) g_nmem = 0; else g_nhist = 0; break;
    case K_LIST_ITEM:
        if (g_show_mem) { if (index >= 0 && index < g_nmem) { start_operand(); g_cur = g_mem[index]; } }
        else if (index >= 0 && index < g_nhist)
        {
            reset_all();
            g_cur = g_hist[index].v;
            lstrcpyW(g_expr, g_hist[index].expr);
            g_eq_done = TRUE;
        }
        g_typing = FALSE;
        g_overlay = FALSE;
        break;
    default: break;
    }
}

/* ---- layout ------------------------------------------------------------------------------- */
static struct { RECT top, expr, result, readout[4], memrow, pad, side, nav; } L;

static void add_button(enum key k, int l, int t, int r, int b, BOOL enabled, int index)
{
    if (g_nbtn >= MAX_BUTTONS) return;
    g_btn[g_nbtn].key = k;
    SetRect(&g_btn[g_nbtn].rc, l, t, r, b);
    g_btn[g_nbtn].enabled = enabled;
    g_btn[g_nbtn].index = index;
    g_nbtn++;
}

static enum key shown_key(enum key k)
{
    if (!g_second) return k;
    switch (k)
    {
    case K_SQR: return K_CUBE; case K_SQRT: return K_CBRT; case K_POW: return K_YROOT;
    case K_POW10: return K_POW2; case K_LOG: return K_LOGY; case K_LN: return K_EPOW;
    case K_SIN: return K_ASIN; case K_COS: return K_ACOS; case K_TAN: return K_ATAN;
    default: return k;
    }
}

static BOOL key_enabled(enum key k)
{
    if (g_error)
        return (k >= K_0 && k <= K_F) || k == K_DOT || k == K_CE || k == K_CLR || k == K_BACK || k == K_EQ
               || k == K_WORD || (k >= K_HEX && k <= K_BIN);
    if (g_mode == M_PROG)
    {
        if (digit_value(k) >= 0) return digit_allowed(k);
        if (k == K_DOT) return FALSE;
    }
    if (k == K_MC || k == K_MR) return g_nmem > 0;
    if (k == K_RPAR) { int i; for (i = 0; i < g_nops; i++) if (g_ops[i] == K_LPAR) return TRUE; return FALSE; }
    return TRUE;
}

static void layout(void)
{
    RECT c;
    int w, h, y, pad_w, rows = 0, cols = 0, r, col, i, n;
    const enum key *pad = g_mode == M_STD ? PAD_STD : g_mode == M_SCI ? PAD_SCI : PAD_PROG;
    GetClientRect(g_hwnd, &c);
    w = c.right; h = c.bottom;
    g_nbtn = 0;
    g_side = w >= S(620);
    pad_w = g_side ? w - S(300) : w;
    SetRect(&L.top, 0, 0, pad_w, S(44));
    add_button(K_NAV, S(4), S(4), S(44), S(40), TRUE, 0);
    if (!g_side) add_button(K_HIST, pad_w - S(44), S(4), pad_w - S(4), S(40), TRUE, 0);
    y = S(44);
    SetRect(&L.expr, S(12), y, pad_w - S(12), y + S(24)); y += S(24);
    SetRect(&L.result, S(8), y, pad_w - S(12), y + S(g_mode == M_PROG ? 58 : 76)); y += S(g_mode == M_PROG ? 58 : 76);
    if (g_mode == M_PROG)
    {
        for (i = 0; i < 4; i++)
        {
            SetRect(&L.readout[i], 0, y, pad_w, y + S(26));
            add_button((enum key)(K_HEX + i), 0, y, pad_w, y + S(26), TRUE, 0);
            y += S(26);
        }
        y += S(4);
    }
    SetRect(&L.memrow, 0, y, pad_w, y + S(34));
    for (i = 0; i < 5; i++)
    {
        int l = S(4) + (pad_w - S(8)) * i / 5, rr = S(4) + (pad_w - S(8)) * (i + 1) / 5;
        enum key mk = (enum key)(K_MC + i);
        add_button(mk, l, y, rr, y + S(34), key_enabled(mk), 0);
    }
    y += S(36);
    for (i = 0, n = 0; pad[i] != K_COUNT; i++)
    {
        if (pad[i] == K_NONE) { rows++; if (n > cols) cols = n; n = 0; }
        else n++;
    }
    SetRect(&L.pad, S(3), y, pad_w - S(3), h - S(3));
    for (i = 0, r = 0, col = 0; pad[i] != K_COUNT; i++)
    {
        int l, t, rr, b;
        enum key k;
        if (pad[i] == K_NONE) { r++; col = 0; continue; }
        l = L.pad.left + (L.pad.right - L.pad.left) * col / cols;
        rr = L.pad.left + (L.pad.right - L.pad.left) * (col + 1) / cols;
        t = L.pad.top + (L.pad.bottom - L.pad.top) * r / rows;
        b = L.pad.top + (L.pad.bottom - L.pad.top) * (r + 1) / rows;
        k = shown_key(pad[i]);
        add_button(k, l + 1, t + 1, rr - 1, b - 1, key_enabled(k), 0);
        col++;
    }
    /* History and Memory: beside the keypad, or over it */
    if (g_side || g_overlay)
    {
        int n2 = g_show_mem ? g_nmem : g_nhist, top;
        if (g_side) SetRect(&L.side, pad_w, 0, w, h);
        else SetRect(&L.side, 0, L.memrow.top, w, h);
        add_button(K_TAB_HIST, L.side.left + S(12), L.side.top + S(8), L.side.left + S(92), L.side.top + S(40), TRUE, 0);
        add_button(K_TAB_MEM, L.side.left + S(96), L.side.top + S(8), L.side.left + S(180), L.side.top + S(40), TRUE, 0);
        top = L.side.top + S(48);
        for (i = n2 - 1; i >= 0 && top + S(64) < L.side.bottom - S(44); i--)
        {
            add_button(K_LIST_ITEM, L.side.left + S(4), top, L.side.right - S(4), top + S(g_show_mem ? 48 : 64), TRUE, i);
            top += S(g_show_mem ? 50 : 66);
        }
        if (n2) add_button(K_CLEAR_LIST, L.side.right - S(48), L.side.bottom - S(44), L.side.right - S(8), L.side.bottom - S(8), TRUE, 0);
    }
    else SetRectEmpty(&L.side);
    if (g_nav)
    {
        SetRect(&L.nav, 0, S(44), min(w, S(260)), h);
        add_button(K_NAV_STD, 0, S(84), L.nav.right, S(124), TRUE, 0);
        add_button(K_NAV_SCI, 0, S(124), L.nav.right, S(164), TRUE, 0);
        add_button(K_NAV_PROG, 0, S(164), L.nav.right, S(204), TRUE, 0);
    }
    else SetRectEmpty(&L.nav);
}

/* the topmost button under a point: overlays (nav, then the list) first */
static int hit(POINT pt)
{
    int i;
    if (g_nav)
    {
        for (i = g_nbtn - 1; i >= 0; i--)
            if ((g_btn[i].key == K_NAV_STD || g_btn[i].key == K_NAV_SCI || g_btn[i].key == K_NAV_PROG || g_btn[i].key == K_NAV)
                && PtInRect(&g_btn[i].rc, pt)) return i;
        return PtInRect(&L.nav, pt) ? -2 : -1;
    }
    if (g_overlay && !g_side && PtInRect(&L.side, pt))
    {
        for (i = g_nbtn - 1; i >= 0; i--)
            if ((g_btn[i].key >= K_TAB_HIST) && PtInRect(&g_btn[i].rc, pt)) return i;
        return -2;
    }
    for (i = 0; i < g_nbtn; i++)
        if (PtInRect(&g_btn[i].rc, pt)) return i;
    return -1;
}

/* ---- drawing ------------------------------------------------------------------------------ */
static HFONT font(int px, int weight)
{
    return CreateFontW(-S(px), 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
}

static void fill(HDC dc, const RECT *r, COLORREF c)
{
    SetDCBrushColor(dc, c);
    FillRect(dc, r, GetStockObject(DC_BRUSH));
}

/* a label with {superscript} and [subscript] runs, centred in r */
static void draw_label(HDC dc, const WCHAR *label, const RECT *r, int px, int weight, COLORREF color)
{
    HFONT big = font(px, weight), small = font(px * 2 / 3, weight), old;
    int total = 0, pass, x = 0, len = lstrlenW(label);
    TEXTMETRICW tm;
    SetTextColor(dc, color);
    SetBkMode(dc, TRANSPARENT);
    old = SelectObject(dc, big);
    GetTextMetricsW(dc, &tm);
    for (pass = 0; pass < 2; pass++)
    {
        int i = 0, base = (r->top + r->bottom) / 2 + tm.tmAscent / 2 - S(1);
        if (pass) x = (r->left + r->right - total) / 2;
        while (i < len)
        {
            int kind = 0, j;
            SIZE sz;
            if (label[i] == '{' || label[i] == '[') { kind = label[i] == '{' ? 1 : 2; i++; }
            for (j = i; j < len && label[j] != '{' && label[j] != '[' && label[j] != '}' && label[j] != ']'; j++) ;
            SelectObject(dc, kind ? small : big);
            GetTextExtentPoint32W(dc, label + i, j - i, &sz);
            if (pass)
            {
                TEXTMETRICW tm2;
                GetTextMetricsW(dc, &tm2);
                SetTextAlign(dc, TA_BASELINE | TA_LEFT);
                TextOutW(dc, x, kind == 1 ? base - tm.tmAscent * 4 / 10 : kind == 2 ? base + tm.tmDescent : base, label + i, j - i);
            }
            if (pass) x += sz.cx; else total += sz.cx;
            i = j;
            if (i < len && (label[i] == '}' || label[i] == ']')) i++;
        }
    }
    SetTextAlign(dc, TA_TOP | TA_LEFT);
    SelectObject(dc, old);
    DeleteObject(big);
    DeleteObject(small);
}

static void line(HDC dc, int x1, int y1, int x2, int y2)
{
    MoveToEx(dc, x1, y1, NULL);
    LineTo(dc, x2, y2);
}

static void draw_glyph(HDC dc, enum key k, const RECT *r, COLORREF color)
{
    HPEN pen = CreatePen(PS_SOLID, max(1, S(1)), color), op = SelectObject(dc, pen);
    HBRUSH ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    int cx = (r->left + r->right) / 2, cy = (r->top + r->bottom) / 2;
    if (k == K_BACK)
    {
        POINT p[5] = { { cx - S(11), cy }, { cx - S(5), cy - S(7) }, { cx + S(11), cy - S(7) },
                       { cx + S(11), cy + S(7) }, { cx - S(5), cy + S(7) } };
        Polygon(dc, p, 5);
        line(dc, cx - S(1), cy - S(3), cx + S(6), cy + S(4));
        line(dc, cx + S(6), cy - S(3), cx - S(1), cy + S(4));
    }
    else if (k == K_NAV)
    {
        int i;
        for (i = -1; i <= 1; i++) line(dc, cx - S(8), cy + i * S(5), cx + S(9), cy + i * S(5));
    }
    else if (k == K_HIST)
    {
        Ellipse(dc, cx - S(8), cy - S(8), cx + S(9), cy + S(9));
        line(dc, cx, cy - S(5), cx, cy + 1);
        line(dc, cx, cy, cx + S(4), cy + S(3));
    }
    else if (k == K_CLEAR_LIST)
    {
        /* a waste bin */
        Rectangle(dc, cx - S(5), cy - S(5), cx + S(6), cy + S(8));
        line(dc, cx - S(8), cy - S(6), cx + S(9), cy - S(6));
        line(dc, cx - S(2), cy - S(9), cx + S(3), cy - S(9));
        line(dc, cx - S(2), cy - S(2), cx - S(2), cy + S(5));
        line(dc, cx + S(2), cy - S(2), cx + S(2), cy + S(5));
    }
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(pen);
}

static void draw_button(HDC dc, int i)
{
    const struct button *b = &g_btn[i];
    const struct keyinfo *ki = &KEYS[b->key];
    COLORREF bg, fg = b->enabled ? C_TEXT : C_DISABLED;
    BOOL hot = i == g_hover && b->enabled, down = i == g_pressed && hot;
    RECT r = b->rc;
    int px = 16, weight = FW_NORMAL;
    const WCHAR *label = ki->label;
    WCHAR buf[64];

    switch (ki->kind)
    {
    case 'n': bg = C_KEY_NUM; px = 20; weight = FW_SEMIBOLD; break;
    case '=': bg = down ? C_ACCENT_DN : hot ? C_ACCENT_HOT : C_ACCENT; fg = RGB(255, 255, 255); px = 22; break;
    case 'm': case 'i': case 'b': case 'v': case 'l': case 'r': case 't': bg = (COLORREF)-1; px = 13; break;
    default: bg = C_KEY_FN; break;
    }
    if (ki->kind == 'f' && digit_value(b->key) >= 0) { bg = C_KEY_NUM; px = 20; weight = FW_SEMIBOLD; }
    if (b->key == K_ADD || b->key == K_SUB || b->key == K_MUL || b->key == K_DIV) px = 24;
    if (ki->kind != '=')
    {
        if (down) bg = C_KEY_DOWN;
        else if (hot) bg = C_KEY_HOT;
    }
    if (b->key == K_2ND && g_second) { bg = hot ? C_ACCENT_HOT : C_ACCENT; fg = RGB(255, 255, 255); }
    if (bg != (COLORREF)-1) fill(dc, &r, bg);

    switch (b->key)
    {
    case K_BACK: case K_NAV: case K_HIST: case K_CLEAR_LIST: draw_glyph(dc, b->key, &r, fg); return;
    case K_ANGLE: label = g_angle == 0 ? L"DEG" : g_angle == 1 ? L"RAD" : L"GRAD"; break;
    case K_WORD: label = g_bits == 64 ? L"QWORD" : g_bits == 32 ? L"DWORD" : g_bits == 16 ? L"WORD" : L"BYTE"; break;
    case K_LPAR:
    {
        int i2, open = 0;
        for (i2 = 0; i2 < g_nops; i2++) if (g_ops[i2] == K_LPAR) open++;
        if (open) { _snwprintf(buf, 64, L"([%d]", open); label = buf; }
        break;
    }
    case K_FE:
        if (g_fe) { RECT u = { r.left + (r.right - r.left) / 2 - S(12), r.bottom - S(5), r.left + (r.right - r.left) / 2 + S(12), r.bottom - S(3) }; fill(dc, &u, C_ACCENT); }
        break;
    case K_HEX: case K_DEC: case K_OCT: case K_BIN:
    {
        int radix = b->key == K_HEX ? 16 : b->key == K_DEC ? 10 : b->key == K_OCT ? 8 : 2;
        RECT tr = r;
        WCHAR v[100];
        HFONT f = font(13, radix == g_radix ? FW_SEMIBOLD : FW_NORMAL), of;
        num cv = cur_value();
        if (radix == g_radix) { RECT bar = { r.left + S(4), r.top + S(5), r.left + S(7), r.bottom - S(5) }; fill(dc, &bar, C_ACCENT); }
        of = SelectObject(dc, f);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, C_TEXT);
        tr.left += S(14);
        DrawTextW(dc, KEYS[b->key].label, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        tr.left += S(48);
        if (g_error) v[0] = 0; else fmt_int(cv.i, radix, v, TRUE);
        DrawTextW(dc, v, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(dc, of);
        DeleteObject(f);
        return;
    }
    case K_TAB_HIST: case K_TAB_MEM:
    {
        BOOL sel = (b->key == K_TAB_MEM) == g_show_mem;
        draw_label(dc, label, &r, 15, sel ? FW_SEMIBOLD : FW_NORMAL, C_TEXT);
        if (sel) { RECT u = { r.left + S(16), r.bottom - S(3), r.right - S(16), r.bottom }; fill(dc, &u, C_ACCENT); }
        return;
    }
    case K_NAV_STD: case K_NAV_SCI: case K_NAV_PROG:
    {
        RECT tr = r;
        HFONT f = font(15, FW_NORMAL), of;
        if ((int)(b->key - K_NAV_STD) == (int)g_mode) { RECT bar = { r.left, r.top + S(8), r.left + S(4), r.bottom - S(8) }; fill(dc, &bar, C_ACCENT); }
        of = SelectObject(dc, f);
        SetTextColor(dc, C_TEXT);
        tr.left += S(20);
        DrawTextW(dc, label, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, of); DeleteObject(f);
        return;
    }
    case K_LIST_ITEM:
    {
        RECT tr = r;
        HFONT f1 = font(13, FW_NORMAL), f2 = font(22, FW_SEMIBOLD), of;
        WCHAR v[128];
        tr.right -= S(12);
        of = SelectObject(dc, f1);
        SetTextColor(dc, C_TEXT2);
        if (g_show_mem) { fmt_num(g_mem[b->index], v, 128, TRUE); }
        else
        {
            RECT e = tr; e.bottom = e.top + S(26);
            DrawTextW(dc, g_hist[b->index].expr, -1, &e, DT_RIGHT | DT_BOTTOM | DT_SINGLELINE | DT_END_ELLIPSIS);
            lstrcpyW(v, g_hist[b->index].result);
            tr.top += S(24);
        }
        SelectObject(dc, f2);
        SetTextColor(dc, C_TEXT);
        DrawTextW(dc, v, -1, &tr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(dc, of); DeleteObject(f1); DeleteObject(f2);
        return;
    }
    default: break;
    }
    draw_label(dc, label, &r, px, weight, fg);
}

static void paint(HDC out)
{
    RECT c, r;
    HDC dc;
    HBITMAP bmp, ob;
    HFONT f, of;
    WCHAR text[256];
    int i, px;
    GetClientRect(g_hwnd, &c);
    dc = CreateCompatibleDC(out);
    bmp = CreateCompatibleBitmap(out, max(1, c.right), max(1, c.bottom));
    ob = SelectObject(dc, bmp);
    fill(dc, &c, C_BG);
    SetBkMode(dc, TRANSPARENT);

    /* the mode's name */
    f = font(20, FW_SEMIBOLD); of = SelectObject(dc, f);
    SetTextColor(dc, C_TEXT);
    r = L.top; r.left = S(56);
    DrawTextW(dc, MODE_NAME[g_mode], -1, &r, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, of); DeleteObject(f);

    /* the expression */
    f = font(14, FW_NORMAL); of = SelectObject(dc, f);
    SetTextColor(dc, C_TEXT2);
    DrawTextW(dc, g_expr, -1, &L.expr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS);
    SelectObject(dc, of); DeleteObject(f);

    /* the result, shrunk to fit */
    display_text(text, 256, TRUE);
    for (px = g_error ? 30 : (g_mode == M_PROG ? 40 : 48); px > 12; px -= 2)
    {
        SIZE sz;
        f = font(px, FW_SEMIBOLD); of = SelectObject(dc, f);
        GetTextExtentPoint32W(dc, text, lstrlenW(text), &sz);
        if (sz.cx <= L.result.right - L.result.left) break;
        SelectObject(dc, of); DeleteObject(f);
    }
    if (px <= 12) { f = font(12, FW_SEMIBOLD); of = SelectObject(dc, f); }
    SetTextColor(dc, C_TEXT);
    DrawTextW(dc, text, -1, &L.result, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, of); DeleteObject(f);

    if (!IsRectEmpty(&L.side))
    {
        fill(dc, &L.side, g_side ? C_BG : C_PANEL);
        if (!g_side) { RECT edge = L.side; edge.bottom = edge.top + 1; fill(dc, &edge, C_KEY_HOT); }
        if (!(g_show_mem ? g_nmem : g_nhist))
        {
            RECT t = L.side;
            t.left += S(16); t.top += S(56);
            f = font(14, FW_NORMAL); of = SelectObject(dc, f);
            SetTextColor(dc, C_TEXT);
            DrawTextW(dc, g_show_mem ? L"There's nothing saved in memory" : L"There's no history yet", -1, &t, DT_LEFT | DT_TOP | DT_WORDBREAK);
            SelectObject(dc, of); DeleteObject(f);
        }
    }
    for (i = 0; i < g_nbtn; i++)
    {
        BOOL in_overlay = g_btn[i].key >= K_TAB_HIST;
        BOOL in_nav = g_btn[i].key >= K_NAV_STD && g_btn[i].key <= K_NAV_PROG;
        if (in_nav) continue;
        if (!in_overlay && g_overlay && !g_side && g_btn[i].rc.top >= L.side.top) continue;
        draw_button(dc, i);
    }
    if (g_nav)
    {
        RECT sh = L.nav, h = L.nav;
        fill(dc, &L.nav, RGB(255, 255, 255));
        sh.left = sh.right; sh.right += S(1); fill(dc, &sh, C_KEY_DOWN);
        h.left += S(20); h.top += S(8); h.bottom = h.top + S(32);
        f = font(13, FW_SEMIBOLD); of = SelectObject(dc, f);
        SetTextColor(dc, C_TEXT2);
        DrawTextW(dc, L"Calculator", -1, &h, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, of); DeleteObject(f);
        for (i = 0; i < g_nbtn; i++)
            if (g_btn[i].key >= K_NAV_STD && g_btn[i].key <= K_NAV_PROG)
            {
                if (i == g_hover) fill(dc, &g_btn[i].rc, C_KEY_FN);
                draw_button(dc, i);
            }
    }
    BitBlt(out, 0, 0, c.right, c.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, ob);
    DeleteObject(bmp);
    DeleteDC(dc);
}

/* ---- the gate's view ---------------------------------------------------------------------- */
static void dump_line(HANDLE f, const WCHAR *fmt, ...)
{
    WCHAR w[2048];
    char u[6144];
    int n;
    DWORD wr;
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(w, 2047, fmt, ap);
    w[2047] = 0;
    va_end(ap);
    n = WideCharToMultiByte(CP_UTF8, 0, w, -1, u, sizeof(u) - 1, NULL, NULL);
    if (n > 0) { u[n - 1] = '\n'; WriteFile(f, u, n, &wr, NULL); }
}

static void dump(void)
{
    HANDLE f;
    WCHAR tmp[MAX_PATH + 8], text[256];
    POINT o = { 0, 0 };
    RECT c;
    int i;
    if (!g_dump[0]) return;
    _snwprintf(tmp, MAX_PATH + 8, L"%ls.tmp", g_dump);
    tmp[MAX_PATH + 7] = 0;
    f = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    ClientToScreen(g_hwnd, &o);
    GetClientRect(g_hwnd, &c);
    dump_line(f, L"CLIENT %d %d %d %d", o.x, o.y, c.right, c.bottom);
    dump_line(f, L"MODE %ls", MODE_NAME[g_mode]);
    display_text(text, 256, FALSE);
    dump_line(f, L"DISPLAY %ls", text);
    display_text(text, 256, TRUE);
    dump_line(f, L"SHOWN %ls", text);
    dump_line(f, L"EXPR %ls", g_expr);
    dump_line(f, L"ERROR %ls", g_error ? g_error : L"");
    if (g_mode == M_PROG)
    {
        num v = cur_value();
        WCHAR hex[100], dec[100], oct[100], bin[100];
        fmt_int(v.i, 16, hex, FALSE); fmt_int(v.i, 10, dec, FALSE); fmt_int(v.i, 8, oct, FALSE); fmt_int(v.i, 2, bin, FALSE);
        dump_line(f, L"RADIX %d BITS %d", g_radix, g_bits);
        dump_line(f, L"HEX %ls", hex); dump_line(f, L"DEC %ls", dec); dump_line(f, L"OCT %ls", oct); dump_line(f, L"BIN %ls", bin);
    }
    if (g_mode == M_SCI) dump_line(f, L"ANGLE %ls SECOND %d FE %d", g_angle == 0 ? L"DEG" : g_angle == 1 ? L"RAD" : L"GRAD", g_second, g_fe);
    if (g_nmem) { fmt_num(g_mem[g_nmem - 1], text, 256, FALSE); dump_line(f, L"MEMORY %d %ls", g_nmem, text); }
    else dump_line(f, L"MEMORY 0");
    dump_line(f, L"HISTORY %d", g_nhist);
    for (i = 0; i < g_nhist; i++) dump_line(f, L"HIST %ls %ls", g_hist[i].expr, g_hist[i].result);
    for (i = 0; i < g_nbtn; i++)
        dump_line(f, L"BTN %ls %d %d %d %d %d", KEYS[g_btn[i].key].name, o.x + g_btn[i].rc.left, o.y + g_btn[i].rc.top,
                  g_btn[i].rc.right - g_btn[i].rc.left, g_btn[i].rc.bottom - g_btn[i].rc.top, g_btn[i].enabled);
    CloseHandle(f);
    MoveFileExW(tmp, g_dump, MOVEFILE_REPLACE_EXISTING);
}

/* ---- the window --------------------------------------------------------------------------- */
static void refresh(void)
{
    layout();
    InvalidateRect(g_hwnd, NULL, FALSE);
}

static enum key char_key(WCHAR ch)
{
    if (ch >= '0' && ch <= '9') return (enum key)(K_0 + ch - '0');
    if (g_mode == M_PROG && ((ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F'))) return (enum key)(K_A + towlower(ch) - 'a');
    switch (ch)
    {
    case '+': return K_ADD; case '-': return K_SUB; case '*': return K_MUL; case '/': return K_DIV;
    case '=': return K_EQ; case '.': case ',': return K_DOT;
    case '%': return g_mode == M_PROG ? K_MOD : K_PCT;
    case '(': return K_LPAR; case ')': return K_RPAR;
    case '^': return g_mode == M_PROG ? K_XOR : K_POW;
    case '@': return g_mode == M_PROG ? K_NONE : K_SQRT;
    case 'r': return g_mode == M_PROG ? K_NONE : K_RECIP;
    case 'q': return g_mode == M_PROG ? K_NONE : K_SQR;
    case '!': return g_mode == M_SCI ? K_FACT : K_NONE;
    case 'p': return g_mode == M_SCI ? K_PI : K_NONE;
    case 's': return g_mode == M_SCI ? K_SIN : K_NONE;
    case 'o': return g_mode == M_SCI ? K_COS : K_NONE;
    case 't': return g_mode == M_SCI ? K_TAN : K_NONE;
    case 'l': return g_mode == M_SCI ? K_LOG : K_NONE;
    case 'n': return g_mode == M_SCI ? K_LN : K_NONE;
    case '&': return g_mode == M_PROG ? K_AND : K_NONE;
    case '|': return g_mode == M_PROG ? K_OR : K_NONE;
    case '~': return g_mode == M_PROG ? K_NOT : K_NONE;
    case '<': return g_mode == M_PROG ? K_LSH : K_NONE;
    case '>': return g_mode == M_PROG ? K_RSH : K_NONE;
    default: return K_NONE;
    }
}

static void do_key(enum key k, int index)
{
    int i;
    if (k == K_NONE) return;
    /* a key the layout shows disabled does nothing from the keyboard either */
    for (i = 0; i < g_nbtn; i++) if (g_btn[i].key == k && !g_btn[i].enabled && k != K_LIST_ITEM) return;
    if (g_mode == M_PROG && digit_value(k) >= 0 && !digit_allowed(k)) return;
    if (g_mode != M_PROG && digit_value(k) >= 10) return;
    press(k, index);
    refresh();
}

static LRESULT CALLBACK wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_SIZE:
        layout();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_DPICHANGED:
    {
        RECT *r = (RECT *)lp;
        g_dpi = HIWORD(wp);
        SetWindowPos(hwnd, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_GETMINMAXINFO:
    {
        MINMAXINFO *mm = (MINMAXINFO *)lp;
        mm->ptMinTrackSize.x = S(320);
        mm->ptMinTrackSize.y = S(g_mode == M_STD ? 480 : 540);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        paint(dc);
        EndPaint(hwnd, &ps);
        dump();
        return 0;
    }
    case WM_MOUSEMOVE:
    {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int h = hit(pt);
        if (h < 0) h = -1;
        if (h != g_hover)
        {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
            g_hover = h;
            TrackMouseEvent(&tme);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        g_hover = -1;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_LBUTTONDOWN:
    {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int h = hit(pt);
        SetFocus(hwnd);
        if (h == -1 && (g_nav || (g_overlay && !g_side)))
        {
            /* a click outside an overlay closes it */
            if (g_nav) g_nav = FALSE; else g_overlay = FALSE;
            refresh();
            return 0;
        }
        g_pressed = h >= 0 && g_btn[h].enabled ? h : -1;
        g_hover = h >= 0 ? h : -1;
        if (g_pressed >= 0) SetCapture(hwnd);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_LBUTTONUP:
    {
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        int h = hit(pt), p = g_pressed;
        g_pressed = -1;
        if (GetCapture() == hwnd) ReleaseCapture();
        if (p >= 0 && h == p) press(g_btn[p].key, g_btn[p].index);
        refresh();
        return 0;
    }
    case WM_CHAR:
        if (wp < ' ' || (GetKeyState(VK_CONTROL) < 0 && GetKeyState(VK_MENU) >= 0)) return 0;
        do_key(char_key((WCHAR)wp), 0);
        return 0;
    case WM_SYSKEYDOWN:
        if (wp == '1') { set_mode(M_STD); refresh(); return 0; }
        if (wp == '2') { set_mode(M_SCI); refresh(); return 0; }
        if (wp == '3' || wp == '4') { set_mode(M_PROG); refresh(); return 0; }
        break;
    case WM_SYSCHAR:
        if (wp >= '1' && wp <= '4') return 0;
        break;
    case WM_SYSCOMMAND:
        /* Alt alone (or Alt+1 released) would enter the menu loop of a window
         * with no menu and swallow the next keys; Alt+Space still opens the
         * system menu */
        if ((wp & 0xFFF0) == SC_KEYMENU && lp == 0) return 0;
        break;
    case WM_KEYDOWN:
    {
        BOOL ctrl = GetKeyState(VK_CONTROL) < 0;
        if (ctrl)
        {
            switch (wp)
            {
            case 'C': copy_result(); return 0;
            case 'V': paste(); refresh(); return 0;
            case 'M': do_key(K_MS, 0); return 0;
            case 'R': do_key(K_MR, 0); return 0;
            case 'P': do_key(K_MADD, 0); return 0;
            case 'Q': do_key(K_MSUB, 0); return 0;
            case 'L': do_key(K_MC, 0); return 0;
            case 'H': g_overlay = !g_overlay; refresh(); return 0;
            }
            return 0;
        }
        switch (wp)
        {
        case VK_RETURN: do_key(K_EQ, 0); return 0;
        case VK_ESCAPE:
            if (g_nav || g_overlay) { g_nav = g_overlay = FALSE; refresh(); }
            else do_key(K_CLR, 0);
            return 0;
        case VK_DELETE: do_key(K_CE, 0); return 0;
        case VK_BACK: do_key(K_BACK, 0); return 0;
        case VK_F9: do_key(K_NEG, 0); return 0;
        case VK_F2: if (g_mode == M_SCI) do_key(K_ANGLE, 0); return 0;
        case VK_F5: if (g_mode == M_PROG) do_key(K_HEX, 0); return 0;
        case VK_F6: if (g_mode == M_PROG) do_key(K_DEC, 0); return 0;
        case VK_F7: if (g_mode == M_PROG) do_key(K_OCT, 0); return 0;
        case VK_F8: if (g_mode == M_PROG) do_key(K_BIN, 0); return 0;
        }
        break;
    }
    case WM_DESTROY:
    {
        WINDOWPLACEMENT wpl = { sizeof(wpl) };
        HKEY k;
        if (GetWindowPlacement(hwnd, &wpl) &&
            !RegCreateKeyExW(HKEY_CURRENT_USER, APP_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL))
        {
            DWORD w = wpl.rcNormalPosition.right - wpl.rcNormalPosition.left, h = wpl.rcNormalPosition.bottom - wpl.rcNormalPosition.top;
            w = MulDiv(w, 96, g_dpi); h = MulDiv(h, 96, g_dpi);
            RegSetValueExW(k, L"Width", 0, REG_DWORD, (BYTE *)&w, sizeof(w));
            RegSetValueExW(k, L"Height", 0, REG_DWORD, (BYTE *)&h, sizeof(h));
            RegCloseKey(k);
        }
        PostQuitMessage(0);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* the window's icon: the same drawing as the exe's, at the size asked */
static HICON make_icon(int size)
{
    return LoadImageW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(1), IMAGE_ICON, size, size, 0);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmd, int show)
{
    WNDCLASSW wc = { 0 };
    MSG msg;
    HDC sdc;
    DWORD w, h;
    WCHAR lower[512];
    int i;
    (void)prev;

    {
        typedef BOOL (WINAPI *ctx_fn)(HANDLE);
        ctx_fn f = (ctx_fn)(void *)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext");
        if (!f || !f((HANDLE)-4)) SetProcessDPIAware();
    }
    sdc = GetDC(NULL);
    g_dpi = GetDeviceCaps(sdc, LOGPIXELSY);
    ReleaseDC(NULL, sdc);
    if (!GetEnvironmentVariableW(L"SG_CALC_DUMP", g_dump, MAX_PATH)) g_dump[0] = 0;

    g_mode = (enum mode)load_dword(L"Mode", M_STD);
    if (g_mode > M_PROG) g_mode = M_STD;
    g_angle = (int)load_dword(L"Angle", 0) % 3;
    g_bits = (int)load_dword(L"WordSize", 64);
    if (g_bits != 8 && g_bits != 16 && g_bits != 32) g_bits = 64;
    /* calc.exe [calculator:[//]scientific|programmer|standard] */
    lstrcpynW(lower, cmd ? cmd : L"", 512);
    for (i = 0; lower[i]; i++) lower[i] = towlower(lower[i]);
    if (wcsstr(lower, L"scientific")) g_mode = M_SCI;
    else if (wcsstr(lower, L"programmer")) g_mode = M_PROG;
    else if (wcsstr(lower, L"standard")) g_mode = M_STD;
    reset_all();
    if (g_mode == M_PROG) g_cur = make_i(0);

    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hIcon = make_icon(32);
    wc.lpszClassName = L"SgCalculator";
    RegisterClassW(&wc);
    w = load_dword(L"Width", 340);
    h = load_dword(L"Height", 560);
    if (w < 320 || w > 4000) w = 340;
    if (h < 480 || h > 4000) h = 560;
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"Calculator", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             S(w), S(h), NULL, NULL, inst, NULL);
    if (!g_hwnd) return 1;
    SendMessageW(g_hwnd, WM_SETICON, ICON_SMALL, (LPARAM)make_icon(16));
    SendMessageW(g_hwnd, WM_SETICON, ICON_BIG, (LPARAM)make_icon(32));
    layout();
    ShowWindow(g_hwnd, show);
    UpdateWindow(g_hwnd);
    while (GetMessageW(&msg, NULL, 0, 0) > 0)
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
