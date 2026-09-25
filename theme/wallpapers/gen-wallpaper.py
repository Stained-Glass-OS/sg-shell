#!/usr/bin/python3
# Stained Glass OS wallpapers, generated: panes of coloured glass in lead.
#
#   gen-wallpaper.py OUT.jpg [--night] [--width W --height H] [--seed N]
#
# Deterministic for a seed. The panes are a Voronoi tiling of jittered seed
# points (each pixel only looks at the 3x3 neighbouring grid cells), the lead
# is where a pixel is nearly as close to its second-nearest seed as to its
# nearest (the distance to their bisector), anti-aliased. The colours are
# jewel tones in bands radiating from a focal point, like a rose window,
# lit by a soft glow and darkened toward each pane's edge like bevelled glass.
#
# Copyright (C) 2026 Stained Glass OS contributors
# SPDX-License-Identifier: AGPL-3.0-or-later
import argparse

import numpy as np
from PIL import Image

PALETTE = np.array([
    (0x7B, 0x2F, 0xBE),  # Stained Glass purple
    (0x4B, 0x1C, 0x7A),  # deep violet
    (0xC0, 0x2B, 0x8A),  # magenta
    (0x9E, 0x1F, 0x3F),  # ruby
    (0xE0, 0x9A, 0x1E),  # amber
    (0x1E, 0xC0, 0xB0),  # turquoise
    (0x0F, 0x6E, 0x78),  # deep teal
    (0x1E, 0x5A, 0xC0),  # cobalt
], dtype=np.float32) / 255.0
LEAD = np.array((0x12, 0x0E, 0x16), dtype=np.float32) / 255.0


def smoothstep(e0, e1, x):
    t = np.clip((x - e0) / (e1 - e0), 0.0, 1.0)
    return t * t * (3 - 2 * t)


def generate(width, height, seed, night):
    rng = np.random.default_rng(seed)
    cell = max(64, int(round(min(width, height) / 8.5)))
    gw, gh = width // cell + 3, height // cell + 3
    # one jittered seed per grid cell (the grid starts one cell off-screen)
    sx = (np.arange(gw)[None, :] - 1 + rng.uniform(0.1, 0.9, (gh, gw))) * cell
    sy = (np.arange(gh)[:, None] - 1 + rng.uniform(0.1, 0.9, (gh, gw))) * cell

    # each pane's colour: bands by angle and ring around a focal point
    fx, fy = width * 0.62, height * 0.42
    ang = np.arctan2(sy - fy, sx - fx)
    rad = np.hypot(sx - fx, sy - fy) / min(width, height)
    band = (np.floor((ang + np.pi) / (2 * np.pi) * 8 + rad * 2.2 + rng.uniform(-0.35, 0.35, (gh, gw))) % 8).astype(int)
    colour = PALETTE[band]
    colour *= rng.uniform(0.82, 1.1, (gh, gw, 1)).astype(np.float32)   # each pane its own sheet

    lead_w = max(2.0, cell * 0.055)
    out = np.empty((height, width, 3), dtype=np.float32)
    ys_all = np.arange(height, dtype=np.float32)
    xs = np.arange(width, dtype=np.float32)
    for y0 in range(0, height, 96):
        ys = ys_all[y0:y0 + 96]
        py, px = np.meshgrid(ys, xs, indexing='ij')
        gi = (py / cell).astype(int) + 1
        gj = (px / cell).astype(int) + 1
        best = np.full(py.shape, np.inf, np.float32)
        second = np.full(py.shape, np.inf, np.float32)
        bi = np.zeros(py.shape, int)
        bj = np.zeros(py.shape, int)
        cand = []
        for di in (-1, 0, 1):
            for dj in (-1, 0, 1):
                ci = np.clip(gi + di, 0, gh - 1)
                cj = np.clip(gj + dj, 0, gw - 1)
                cand.append((ci, cj, (px - sx[ci, cj]) ** 2 + (py - sy[ci, cj]) ** 2))
        for ci, cj, d in cand:
            closer = d < best
            second = np.where(closer, best, np.minimum(second, d))
            bi = np.where(closer, ci, bi)
            bj = np.where(closer, cj, bj)
            best = np.where(closer, d, best)
        # distance to the bisector with the second-nearest seed, roughly
        d1, d2 = np.sqrt(best), np.sqrt(second)
        edge = (d2 - d1) * 0.5
        glass = colour[bi, bj]
        # bevel: darker toward the lead
        glass = glass * (0.72 + 0.28 * smoothstep(0, cell * 0.35, edge))[..., None]
        # the light: a broad glow from the upper left, and a fall-off at the far corner
        gx = px / width
        gyn = py / height
        light = 0.55 + 0.6 * np.exp(-(((gx - 0.28) ** 2) / 0.18 + ((gyn - 0.18) ** 2) / 0.22))
        light *= 1.0 - 0.25 * smoothstep(0.55, 1.25, np.hypot(gx - 0.25, gyn - 0.2))
        if night:
            light *= 0.55
        glass = glass * light[..., None]
        lead = 1.0 - smoothstep(lead_w * 0.5 - 0.75, lead_w * 0.5 + 0.75, edge)
        out[y0:y0 + len(ys)] = glass * (1 - lead[..., None]) + LEAD * lead[..., None]
    return Image.fromarray((np.clip(out, 0, 1) * 255 + 0.5).astype(np.uint8), 'RGB')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('--width', type=int, default=3840)
    ap.add_argument('--height', type=int, default=2160)
    ap.add_argument('--seed', type=int, default=2026)
    ap.add_argument('--night', action='store_true')
    a = ap.parse_args()
    generate(a.width, a.height, a.seed, a.night).save(a.out, quality=92, optimize=True)


if __name__ == '__main__':
    main()
