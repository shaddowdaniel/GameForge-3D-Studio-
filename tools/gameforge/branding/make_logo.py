# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Turn the PWOps logo into the GameForge 3D Studio logo by replacing only the name.

The "PWOps" lettering is masked out and in-painted, then "GAMEFORGE" (gold "GAME",
steel "FORGE", like the gold "PW" and steel "Ops") and "3D STUDIO" are drawn in its
place. The metal of the new letters is synthesized from patches of the original
letters so the cracked gold and steel match, and a fire core sits in the "O" of
FORGE like the target in the original "O".

Used by ``make_branding.py``; needs ``opencv-python-headless`` besides its requirements.
"""

import math
import random

import cv2
import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

# Area covered by the original "PWOps" lettering in the 1254px source.
TEXT_AREA = [(135, 545), (700, 540), (960, 548), (1000, 590), (1175, 600), (1185, 800),
             (1000, 830), (700, 850), (135, 850)]
# Crop of the crest (gear, rune ring, spear, fire core) used for icons.
CREST_BOX = (297, 30, 957, 690)
# Rows of the crop where the lettering starts; faded out.
CREST_FADE = (480, 545)


def _remove_text(src):
    gray = cv2.cvtColor(src, cv2.COLOR_RGB2GRAY)
    area = np.zeros(gray.shape, np.uint8)
    cv2.fillPoly(area, [np.array(TEXT_AREA, np.int32)], 255)
    mask = ((gray > 55) & (area > 0)).astype(np.uint8) * 255
    mask = cv2.morphologyEx(mask, cv2.MORPH_CLOSE, np.ones((9, 9), np.uint8))
    mask = cv2.dilate(mask, np.ones((9, 9), np.uint8))
    return cv2.inpaint(src, mask, 9, cv2.INPAINT_TELEA)


def _find_patches(src, size=32):
    lum = src.mean(2)
    sat = src.max(2) - src.min(2)
    gold, steel = [], []
    for y in range(540, 830, 8):
        for x in range(140, 1180, 8):
            lum_win = lum[y:y + size, x:x + size]
            sat_win = sat[y:y + size, x:x + size]
            if (lum_win > 60).mean() <= 0.92:
                continue
            win = src[y:y + size, x:x + size]
            if (sat_win > 60).mean() > 0.9 and win[..., 0].mean() > win[..., 2].mean() + 60:
                gold.append((x, y))
            elif (sat_win < 50).mean() > 0.9:
                steel.append((x, y))
    return gold, steel


def _synthesize(src, patches, shape, seed, size=32):
    """Quilt random, rotated and feathered patches into a texture."""
    rnd = random.Random(seed)
    h, w = shape
    acc = np.zeros((h, w, 3), np.float32)
    weight = np.zeros((h, w, 1), np.float32)
    ramp = np.minimum(np.linspace(0, 1, size + 2)[1:-1], np.linspace(1, 0, size + 2)[1:-1])
    feather = np.outer(ramp, ramp)[..., None] + 1e-3
    for y in range(0, h, size // 2):
        for x in range(0, w, size // 2):
            px, py = patches[rnd.randrange(len(patches))]
            patch = np.rot90(src[py:py + size, px:px + size], rnd.randrange(4))
            if rnd.random() < 0.5:
                patch = patch[:, ::-1]
            ph, pw = min(size, h - y), min(size, w - x)
            acc[y:y + ph, x:x + pw] += patch[:ph, :pw] * feather[:ph, :pw]
            weight[y:y + ph, x:x + pw] += feather[:ph, :pw]
    return acc / weight


def _glyph_masks(text, font_path, size, cx, baseline, shape, tracking=0.0, max_width=None,
                 weight=None):
    font = ImageFont.truetype(font_path, size)
    if weight:
        font.set_variation_by_axes([weight])
    widths = [font.getlength(c) for c in text]
    total = sum(widths) + tracking * (len(text) - 1)
    if max_width and total > max_width:
        k = max_width / total
        return _glyph_masks(text, font_path, int(size * k), cx, baseline, shape, tracking * k,
                            None, weight)
    h, w = shape
    masks = []
    x = cx - total / 2
    for ch, adv in zip(text, widths):
        m = Image.new("L", (w, h), 0)
        ImageDraw.Draw(m).text((x, baseline), ch, font=font, anchor="ls", fill=255)
        masks.append(m)
        x += adv + tracking
    return masks


def _union(masks):
    out = np.zeros_like(np.asarray(masks[0]))
    for m in masks:
        out = np.maximum(out, np.asarray(m))
    return Image.fromarray(out)


def _draw_metal(base, texture, mask):
    """Chiselled metal letters: bevel, vertical shading, bronze rim, outline, drop shadow."""
    h = base.shape[0]
    m = np.asarray(mask, np.float32)[..., None] / 255
    blur = np.asarray(mask.filter(ImageFilter.GaussianBlur(4)), np.float32) / 255
    gy, gx = np.gradient(blur)
    shade = ((gx + 1.3 * gy) / math.hypot(1, 1.3) * 26)[..., None]
    rows = np.where(np.asarray(mask).any(1))[0]
    y0, y1 = rows.min(), rows.max()
    vert = (1.15 - 0.45 * np.clip((np.arange(h) - y0) / (y1 - y0 + 1), 0, 1))[:, None, None]
    color = np.clip(texture * vert * (1 + shade * 0.6) + shade * 60, 0, 255)

    def grown(px, blur_px=0):
        g = mask.filter(ImageFilter.MaxFilter(px))
        if blur_px:
            g = g.filter(ImageFilter.GaussianBlur(blur_px))
        return np.asarray(g, np.float32)[..., None] / 255

    shadow = np.roll(np.roll(grown(9, 10), 8, 0), 6, 1)
    outline, rim = grown(9), grown(5)
    out = base * (1 - 0.75 * shadow)
    out = out * (1 - outline) + np.array([14, 9, 4]) * outline
    out = out * (1 - rim) + np.array([120, 78, 30]) * rim
    return out * (1 - m) + color * m


def build(source_path, fonts):
    """Return (logo RGB, crest RGBA) images."""
    src = np.asarray(Image.open(source_path).convert("RGB"))
    shape = src.shape[:2]
    h, w = shape
    srcf = src.astype(np.float32)
    clean = _remove_text(src).astype(np.float32)

    gold_patches, steel_patches = _find_patches(srcf)
    gold = _synthesize(srcf, gold_patches, shape, 1)
    steel = _synthesize(srcf, steel_patches, shape, 2)
    mean = steel.mean()
    steel = np.clip((steel - mean) * 1.45 + mean - 12, 0, 255)

    # Darken what is left of the in-painted lettering.
    band = Image.new("L", (w, h), 0)
    ImageDraw.Draw(band).polygon([(135, 540), (1180, 545), (1185, 810), (700, 850), (135, 850)],
                                 fill=225)
    band = np.asarray(band.filter(ImageFilter.GaussianBlur(30)), np.float32)[..., None] / 255
    img = clean * (1 - 0.55 * band)

    title = _glyph_masks("GAMEFORGE", fonts["CinzelDecorative-Black.ttf"], 260, 655, 722, shape,
                         tracking=-6, max_width=1110)
    img = _draw_metal(img, gold, _union(title[:4]))
    img = _draw_metal(img, steel, _union(title[4:]))
    sub = _glyph_masks("3D  STUDIO", fonts["Cinzel[wght].ttf"], 92, 655, 832, shape, tracking=22,
                       weight=900)
    img = _draw_metal(img, gold, _union(sub))

    # Fire core in the "O" of FORGE.
    o_mask = np.asarray(title[5])
    ys, xs = np.where(o_mask > 0)
    cx, cy = xs.mean(), ys.mean()
    glow = Image.new("RGB", (w, h), 0)
    g = ImageDraw.Draw(glow)
    for r, c in ((20, (110, 35, 0)), (12, (230, 110, 10)), (6, (255, 200, 80)), (3, (255, 250, 220))):
        g.ellipse((cx - r, cy - r, cx + r, cy + r), fill=c)
    g.line((cx - 28, cy, cx + 28, cy), fill=(255, 170, 60), width=2)
    g.line((cx, cy - 38, cx, cy + 38), fill=(255, 170, 60), width=2)
    glow = np.asarray(glow.filter(ImageFilter.GaussianBlur(2.5)), np.float32)
    inside = 1 - np.asarray(Image.fromarray(o_mask).filter(ImageFilter.MaxFilter(5)),
                            np.float32)[..., None] / 255
    img = 255 - (255 - img) * (1 - glow * inside / 255)
    logo = Image.fromarray(np.clip(img, 0, 255).astype(np.uint8), "RGB")

    # Crest without lettering, on a transparent square, for icons and the splash.
    x0, y0, x1, y1 = CREST_BOX
    crop = Image.fromarray(src[y0:y1, x0:x1]).convert("RGBA")
    side = crop.size[0]
    alpha = Image.new("L", (side, side), 0)
    ImageDraw.Draw(alpha).ellipse((8, 8, side - 8, side - 8), fill=255)
    alpha = np.asarray(alpha.filter(ImageFilter.GaussianBlur(6)), np.float32)
    f0, f1 = CREST_FADE
    alpha *= np.clip(1 - (np.arange(side) - f0) / (f1 - f0), 0, 1)[:, None]
    crop.putalpha(Image.fromarray(alpha.astype(np.uint8)))
    crest = crop
    return logo, crest
