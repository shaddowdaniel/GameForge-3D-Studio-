#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Generate the Windows icon of `.blend` documents (``release/windows/icons/winblenderfile.ico``):
a navy page with a gold frame and a folded corner, with the GameForge crest on it.

Requirements: Python 3, ``pip install pillow``.

Usage, from the repository root::

   python3 tools/gameforge/branding/make_file_icon.py
"""

import argparse
import os

from PIL import Image, ImageDraw

SIZE = 256
NAVY_TOP = (23, 39, 66)
NAVY_BOTTOM = (10, 17, 32)
FOLD = (32, 52, 83)
GOLD = (217, 173, 85)


def page_icon(crest):
    page = Image.new("RGBA", (SIZE, SIZE), (0, 0, 0, 0))
    left, top, right, bottom, fold = 36, 10, 220, 246, 52
    outline = [(left, top), (right - fold, top), (right, top + fold), (right, bottom), (left, bottom)]

    # Vertical navy gradient clipped to the page shape.
    gradient = Image.new("RGBA", (SIZE, SIZE))
    for y in range(SIZE):
        t = y / (SIZE - 1)
        color = tuple(round(a + (b - a) * t) for a, b in zip(NAVY_TOP, NAVY_BOTTOM))
        ImageDraw.Draw(gradient).line([(0, y), (SIZE, y)], fill=color + (255,))
    mask = Image.new("L", (SIZE, SIZE), 0)
    ImageDraw.Draw(mask).polygon(outline, fill=255)
    page.paste(gradient, (0, 0), mask)

    draw = ImageDraw.Draw(page)
    draw.polygon([(right - fold, top), (right - fold, top + fold), (right, top + fold)], fill=FOLD + (255,))
    draw.line(outline + [outline[0]], fill=GOLD + (255,), width=6, joint="curve")
    draw.line([(right - fold, top), (right - fold, top + fold), (right, top + fold)], fill=GOLD + (255,), width=4)

    emblem = crest.copy()
    emblem.thumbnail((150, 150), Image.LANCZOS)
    page.alpha_composite(emblem, ((SIZE - emblem.width) // 2, 92))
    return page


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--root", default=".", help="Repository root.")
    args = parser.parse_args()
    root = os.path.abspath(args.root)

    crest = Image.open(os.path.join(root, "release", "datafiles", "gameforge", "gameforge_crest.png")).convert("RGBA")
    icon = page_icon(crest)
    icon.save(
        os.path.join(root, "release", "windows", "icons", "winblenderfile.ico"),
        format="ICO",
        sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (96, 96), (128, 128), (256, 256)],
    )


if __name__ == "__main__":
    main()
