/* sg-control -- desktop pictures (wallpaper.c).
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef SG_WALLPAPER_H
#define SG_WALLPAPER_H

/* the fits Windows offers, in its order */
enum { WP_FILL, WP_FIT, WP_STRETCH, WP_TILE, WP_CENTER, WP_SPAN, WP_COUNT };

DWORD *image_load(const WCHAR *path, int *w, int *h);
DWORD *image_fit(const DWORD *src, int sw, int sh, int style, COLORREF bg, int out_w, int out_h);
BOOL image_save_bmp(const WCHAR *path, const DWORD *px, int w, int h);
HBITMAP image_thumbnail(const DWORD *src, int sw, int sh, int style, COLORREF bg, int w, int h);

#endif
