#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Generate the art of the Windows installer (WiX) in the GameForge style.

Built from the splash screen (``release/datafiles/splash.png``) made by
``make_branding.py``: starry navy sky, bronze/gold frames with blue diamonds,
and the logo with its fire core.

Outputs (24 bit BMP, the sizes WiX expects):

- ``release/windows/installer_wix/gameforge_dialog.bmp`` 493x312, behind the
  welcome and finish pages. The logo sits in the left panel; the text of the
  page is drawn over the parchment area on the right.
- ``release/windows/installer_wix/gameforge_banner.bmp`` 493x58, at the top of
  the other pages, with the crest (``gameforge_crest.png``) on the right. The page title is drawn over its parchment part.

The installer draws its text in fixed colors (see ``WixUI_Blender.wxs``), so the
areas under the text stay light.

Requirements: Python 3, ``pip install pillow``.

Usage, from the repository root::

   python3 tools/gameforge/branding/make_installer_art.py
"""

import argparse
import math
import os
import random

from PIL import Image, ImageDraw, ImageFilter

NAVY_DARK = (6, 11, 24)
NAVY = (14, 30, 66)
GOLD = (217, 173, 85)
GOLD_LIGHT = (242, 200, 105)
BRONZE = (118, 88, 46)
DIAMOND = (90, 160, 255)
PARCHMENT = (246, 241, 230)
PARCHMENT_EDGE = (233, 224, 204)

DIALOG_SIZE = (493, 312)
DIALOG_PANEL = 164
BANNER_SIZE = (493, 58)


def lerp(a, b, t):
    return tuple(int(round(a[i] + (b[i] - a[i]) * t)) for i in range(3))


def starfield(size, seed, glow_center=None):
    """Navy sky with a soft glow and small stars, like the splash background."""
    w, h = size
    img = Image.new("RGB", size, NAVY_DARK)
    px = img.load()
    cx, cy = glow_center if glow_center else (w * 0.5, h * 0.5)
    radius = math.hypot(w, h) * 0.6
    for y in range(h):
        for x in range(w):
            t = min(1.0, math.hypot(x - cx, y - cy) / radius)
            px[x, y] = lerp(NAVY, NAVY_DARK, t ** 0.8)
    rng = random.Random(seed)
    draw = ImageDraw.Draw(img)
    for _ in range(int(w * h / 180)):
        x, y = rng.randrange(w), rng.randrange(h)
        level = rng.randint(90, 220)
        draw.point((x, y), fill=(level, level, min(255, level + 30)))
    return img


def parchment(size, fade_right=None):
    """Light parchment area for the installer text, with a faint rune circle."""
    w, h = size
    img = Image.new("RGB", size, PARCHMENT)
    px = img.load()
    for y in range(h):
        for x in range(w):
            t = (x / max(1, w - 1)) * 0.35 + (y / max(1, h - 1)) * 0.65
            px[x, y] = lerp(PARCHMENT, PARCHMENT_EDGE, t * 0.8)
    overlay = Image.new("RGBA", size, (0, 0, 0, 0))
    draw = ImageDraw.Draw(overlay)
    cx, cy, r = w * 0.78, h * 0.62, min(w, h) * 0.55
    for k, rr in enumerate((r, r * 0.86, r * 0.62)):
        draw.ellipse((cx - rr, cy - rr, cx + rr, cy + rr), outline=GOLD + (40 - k * 8,), width=1)
    for i in range(12):
        a = i * math.pi / 6
        draw.line((cx + math.cos(a) * r * 0.62, cy + math.sin(a) * r * 0.62,
                   cx + math.cos(a + 2.2) * r, cy + math.sin(a + 2.2) * r),
                  fill=GOLD + (22,), width=1)
    img = Image.alpha_composite(img.convert("RGBA"), overlay).convert("RGB")
    if fade_right:
        # Blend into the navy panel on the right.
        start, end = fade_right
        mask = Image.new("L", size, 0)
        mpx = mask.load()
        for x in range(w):
            t = 0.0 if x < start else min(1.0, (x - start) / max(1, end - start))
            for y in range(h):
                mpx[x, y] = int(255 * t)
        return img, mask
    return img


def diamond(draw, cx, cy, r, fill=DIAMOND):
    draw.polygon([(cx, cy - r), (cx + r, cy), (cx, cy + r), (cx - r, cy)],
                 fill=fill, outline=(220, 235, 255))


def gold_frame(draw, box, width=2):
    x0, y0, x1, y1 = box
    draw.rectangle(box, outline=BRONZE, width=width + 1)
    draw.rectangle((x0 + 1, y0 + 1, x1 - 1, y1 - 1), outline=GOLD, width=max(1, width - 1))


def feather(size, radius):
    """Mask that fades the edges of a pasted image into the background."""
    w, h = size
    mask = Image.new("L", size, 0)
    ImageDraw.Draw(mask).rectangle((radius, radius, w - 1 - radius, h - 1 - radius), fill=255)
    return mask.filter(ImageFilter.GaussianBlur(radius * 0.6))


def crest_crop(crest):
    """The emblem of the crest image, without the cut lettering at its bottom."""
    w, h = crest.size
    side = int(h * 0.72)
    left = (w - side) // 2
    return crest.crop((left, 0, left + side, side))


def logo_crop(splash):
    """The logo of the splash (left half), as a square image."""
    w, h = splash.size
    side = int(h * 0.94)
    left = int(w * 0.03)
    top = int(h * 0.03)
    return splash.crop((left, top, left + side, top + side))


def make_dialog(logo):
    w, h = DIALOG_SIZE
    img = Image.new("RGB", DIALOG_SIZE)
    img.paste(parchment((w - DIALOG_PANEL, h)), (DIALOG_PANEL, 0))

    panel = starfield((DIALOG_PANEL, h), seed=7, glow_center=(DIALOG_PANEL * 0.5, h * 0.45))
    size = DIALOG_PANEL - 8
    mark = logo.resize((size, size), Image.LANCZOS)
    panel.paste(mark, ((DIALOG_PANEL - size) // 2, int(h * 0.46 - size / 2)), feather((size, size), 5))
    img.paste(panel, (0, 0))

    draw = ImageDraw.Draw(img)
    # Gold divider between the panel and the text, with a blue diamond.
    draw.rectangle((DIALOG_PANEL - 3, 0, DIALOG_PANEL - 1, h), fill=BRONZE)
    draw.line((DIALOG_PANEL - 2, 0, DIALOG_PANEL - 2, h), fill=GOLD_LIGHT)
    diamond(draw, DIALOG_PANEL - 2, int(h * 0.44), 6)
    gold_frame(draw, (0, 0, w - 1, h - 1))
    for cx, cy in ((4, 4), (w - 5, 4), (4, h - 5), (w - 5, h - 5)):
        diamond(draw, cx, cy, 4, fill=GOLD_LIGHT)
    return img


def make_banner(crest):
    w, h = BANNER_SIZE
    sky = starfield(BANNER_SIZE, seed=11, glow_center=(w * 0.85, h * 0.5))
    light, mask = parchment(BANNER_SIZE, fade_right=(int(w * 0.62), int(w * 0.80)))
    img = Image.composite(sky, light, mask)

    size = h + 14
    mark = crest.resize((size, size), Image.LANCZOS)
    img.paste(mark, (w - size - 10, (h - size) // 2 - 2), feather((size, size), 7))

    draw = ImageDraw.Draw(img)
    draw.rectangle((0, h - 3, w, h - 1), fill=BRONZE)
    draw.line((0, h - 2, w, h - 2), fill=GOLD_LIGHT)
    diamond(draw, int(w * 0.71), h - 2, 4)
    return img


def main():
    root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--splash", default=os.path.join(root, "release", "datafiles", "splash.png"))
    parser.add_argument("--crest", default=os.path.join(
        root, "release", "datafiles", "gameforge", "gameforge_crest.png"))
    parser.add_argument("--output-dir", default=os.path.join(root, "release", "windows", "installer_wix"))
    args = parser.parse_args()

    splash = Image.open(args.splash).convert("RGB")
    logo = logo_crop(splash)
    crest = crest_crop(Image.open(args.crest).convert("RGB"))
    os.makedirs(args.output_dir, exist_ok=True)
    for name, image in (("gameforge_dialog.bmp", make_dialog(logo)),
                        ("gameforge_banner.bmp", make_banner(crest))):
        path = os.path.join(args.output_dir, name)
        image.save(path, format="BMP")
        print("wrote", path, image.size)


if __name__ == "__main__":
    main()
