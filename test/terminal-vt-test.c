/* Unit gate for sg-terminal's screen (src/terminal/vt.c), built natively:
 * what the VT parser makes of text, controls and sequences -- cursor
 * movement, erasing, colours, wrapping, scrollback, the alternate screen,
 * titles, replies, sequences split across writes, wide characters, resize.
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#include "../src/terminal/vt.h"
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(what, cond) do { if (cond) printf("PASS  %s\n", what); else { printf("FAIL  %s (line %d)\n", what, __LINE__); fails++; } } while (0)

static const char *row(struct vt *t, int y)
{
    static char buf[1024];
    vt_line_text(vt_view_line(t, 0, y), t->cols, buf, sizeof(buf));
    return buf;
}

static void W(struct vt *t, const char *s) { vt_write(t, s, strlen(s)); }

int main(void)
{
    struct vt t;
    char buf[256];

    vt_init(&t, 20, 5, 100);
    W(&t, "hello\r\nworld");
    CHECK("text and CR LF", !strcmp(row(&t, 0), "hello") && !strcmp(row(&t, 1), "world") && t.cx == 5 && t.cy == 1);

    W(&t, "\x1b[2J\x1b[3;4HX");
    CHECK("ED 2 clears, CUP moves (1-based)", !strcmp(row(&t, 0), "") && !strcmp(row(&t, 1), "") && !strcmp(row(&t, 2), "   X"));

    W(&t, "\x1b[H\x1b[31mR\x1b[1;38;5;208mO\x1b[38;2;1;2;3mT\x1b[0mN");
    CHECK("SGR 31: red", t.screen[0].cells[0].fg == 1);
    CHECK("SGR 1;38;5;208: bold, 256-colour", t.screen[0].cells[1].fg == 208 && (t.screen[0].cells[1].flags & VT_BOLD));
    CHECK("SGR 38;2: RGB", t.screen[0].cells[2].fg == VT_RGB(1, 2, 3));
    CHECK("SGR 0: back to the default", t.screen[0].cells[3].fg == VT_DEFAULT && !(t.screen[0].cells[3].flags & VT_BOLD));
    W(&t, "\x1b[97;104mB");
    CHECK("SGR 97;104: bright colours", t.screen[0].cells[4].fg == 15 && t.screen[0].cells[4].bg == 12);
    W(&t, "\x1b[0m");

    vt_free(&t);
    vt_init(&t, 10, 3, 100);
    W(&t, "abcdefghijKL");
    CHECK("a long line wraps", !strcmp(row(&t, 0), "abcdefghij") && t.screen[0].wrapped && !strcmp(row(&t, 1), "KL"));
    W(&t, "\r\n1\r\n2\r\n3");
    CHECK("lines scroll into the scrollback", t.sb_count == 2 && !strcmp(row(&t, 2), "3"));
    vt_line_text(vt_view_line(&t, 2, 0), 10, buf, sizeof(buf));
    CHECK("the scrollback is readable from the view", !strcmp(buf, "abcdefghij"));

    vt_free(&t);
    vt_init(&t, 20, 4, 10);
    W(&t, "main");
    W(&t, "\x1b[?1049h");
    CHECK("?1049h: an empty alternate screen", t.alt && !strcmp(row(&t, 0), ""));
    W(&t, "\x1b[Halt screen");
    W(&t, "\x1b[?1049l");
    CHECK("?1049l: the main screen back, cursor restored", !t.alt && !strcmp(row(&t, 0), "main") && t.cx == 4);

    W(&t, "\r\x1b[K0123456789\x1b[5G\x1b[2P");
    CHECK("DCH deletes at the cursor", !strcmp(row(&t, 0), "01236789"));
    W(&t, "\x1b[2@");
    CHECK("ICH inserts blanks", !strcmp(row(&t, 0), "0123  6789"));
    W(&t, "\x1b[1K");
    CHECK("EL 1 erases to the cursor", !strcmp(row(&t, 0), "      6789"));

    W(&t, "\x1b]0;my title\x07");
    CHECK("OSC 0 with BEL sets the title", !strcmp(t.title, "my title"));
    W(&t, "\x1b]2;other\x1b\\");
    CHECK("OSC 2 with ST sets the title", !strcmp(t.title, "other"));

    W(&t, "\x1b[2;1H\x1b[3");
    W(&t, "2mG");
    CHECK("a sequence split across writes", t.screen[1].cells[0].ch == 'G' && t.screen[1].cells[0].fg == 2);
    W(&t, "\x1b[0m");

    W(&t, "\x1b[3;1H\xe6\x97\xa5\xe6\x9c\xac!");
    CHECK("wide characters take two cells", t.screen[2].cells[0].ch == 0x65e5 && (t.screen[2].cells[1].flags & VT_WIDE_TAIL)
          && t.screen[2].cells[2].ch == 0x672c && t.screen[2].cells[4].ch == '!');

    t.reply_len = 0;
    W(&t, "\x1b[2;7H\x1b[6n");
    CHECK("DSR 6: the cursor position is reported", t.reply_len && !strncmp(t.reply, "\x1b[2;7R", t.reply_len));

    vt_free(&t);
    vt_init(&t, 10, 5, 10);
    W(&t, "a\r\nb\r\nc\r\nd\r\ne");
    W(&t, "\x1b[2;4r\x1b[4;1H\n");
    CHECK("a scroll region scrolls only itself", !strcmp(row(&t, 0), "a") && !strcmp(row(&t, 1), "c") &&
          !strcmp(row(&t, 3), "") && !strcmp(row(&t, 4), "e"));

    W(&t, "\x1b[r\x1b[5;1H");
    vt_resize(&t, 8, 3);
    CHECK("resize: fewer rows keep the cursor's line", t.rows == 3 && t.cols == 8 && !strcmp(row(&t, 2), "e") && t.cy == 2);
    vt_resize(&t, 12, 6);
    CHECK("resize: more rows and columns", t.rows == 6 && t.cols == 12 && t.screen[0].width >= 12);

    W(&t, "\x1b" "c");
    CHECK("ESC c resets", !strcmp(row(&t, 0), "") && t.cx == 0 && t.cy == 0);

    W(&t, "x\x08y\tz");
    CHECK("BS and HT", !strcmp(row(&t, 0), "y       z"));

    vt_free(&t);
    printf("terminal-vt-test: %s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
