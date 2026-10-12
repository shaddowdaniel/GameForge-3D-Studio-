#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Generate the GameForge 3D Studio emblem, splash screen and application icons.

The emblem adapts the PWOps / ATOM GameForge crest: a cracked gold and bronze
shield, a half gear and half rune circle, an anvil with an orange fire core,
and molten metal dripping from the name plate.

Everything is drawn from code so it can be regenerated and tweaked:

- ``release/datafiles/gameforge/gameforge_emblem.svg``   vector crest with the name.
- ``release/datafiles/gameforge/gameforge_icon.svg``     vector crest without text.
- ``release/datafiles/gameforge/gameforge_emblem.png``   textured raster crest (1024px).
- ``release/datafiles/splash.png``                        splash screen.
- ``release/freedesktop/icons/scalable/apps/blender.svg`` Linux application icon.
- ``release/freedesktop/icons/symbolic/apps/blender-symbolic.svg`` monochrome icon.
- ``release/datafiles/icons_svg/blender_logo_large.svg``  wordmark of the About dialog.
- ``release/windows/icons/winblender.ico``                Windows application icon.

Requirements: Python 3, ``pip install pillow numpy cairosvg fonttools``.
The Cinzel fonts (SIL Open Font License) are downloaded from google/fonts on
first run into ``--font-dir`` and are only used to draw the lettering; they
are not shipped with the application.

Usage, from the repository root::

   python3 tools/gameforge/branding/make_branding.py
"""

import argparse
import io
import math
import os
import random
import urllib.request

import cairosvg
import numpy as np
from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.pens.transformPen import TransformPen
from fontTools.ttLib import TTFont
from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

FONT_URLS = {
    "CinzelDecorative-Black.ttf":
        "https://raw.githubusercontent.com/google/fonts/main/ofl/cinzeldecorative/CinzelDecorative-Black.ttf",
    "CinzelDecorative-Bold.ttf":
        "https://raw.githubusercontent.com/google/fonts/main/ofl/cinzeldecorative/CinzelDecorative-Bold.ttf",
    "Cinzel[wght].ttf":
        "https://raw.githubusercontent.com/google/fonts/main/ofl/cinzel/Cinzel%5Bwght%5D.ttf",
}

# Palette shared with tools/gameforge/apply_theme.py.
NAVY_DEEP = (5, 8, 16)
NAVY = (12, 20, 38)
GOLD = (217, 173, 85)
GOLD_BRIGHT = (242, 200, 105)
FIRE = (255, 138, 26)


# -----------------------------------------------------------------------------
# Fonts.

def ensure_fonts(font_dir):
    os.makedirs(font_dir, exist_ok=True)
    paths = {}
    for name, url in FONT_URLS.items():
        path = os.path.join(font_dir, name)
        if not os.path.exists(path):
            print("Downloading", url)
            with urllib.request.urlopen(url) as response, open(path, "wb") as fh:
                fh.write(response.read())
        paths[name] = path
    return paths


def text_path(font_path, text, x_center, baseline, height, tracking=0.0, max_width=None):
    """Return an SVG path ``d`` for ``text`` with its cap height set to ``height``."""
    font = TTFont(font_path)
    glyph_set = font.getGlyphSet()
    cmap = font.getBestCmap()
    cap_height = getattr(font["OS/2"], "sCapHeight", 0) or font["head"].unitsPerEm * 0.7
    scale = height / cap_height
    hmtx = font["hmtx"]

    advances = []
    for ch in text:
        glyph = cmap.get(ord(ch), ".notdef")
        advances.append((glyph, hmtx[glyph][0]))
    track = tracking * font["head"].unitsPerEm
    width = sum(a for _, a in advances) + track * (len(text) - 1)
    if max_width and width * scale > max_width:
        scale = max_width / width
    x = x_center - width * scale / 2.0

    pen = SVGPathPen(glyph_set)
    cursor = 0.0
    for glyph, advance in advances:
        tpen = TransformPen(pen, (scale, 0, 0, -scale, x + cursor * scale, baseline))
        glyph_set[glyph].draw(tpen)
        cursor += advance + track
    return pen.getCommands()


# -----------------------------------------------------------------------------
# Vector crest.

def _pts(points):
    return " ".join("%.1f,%.1f" % p for p in points)


def gear_path(cx, cy, r_in, r_out, a0, a1, teeth_step):
    """Half gear: tooth blocks between angles a0..a1 (degrees, 0 = right, CW)."""
    d = []
    a = a0
    while a <= a1:
        half = teeth_step * 0.2
        pts = []
        for ang, r in ((a - half * 1.45, r_in), (a - half, r_out), (a + half, r_out),
                       (a + half * 1.45, r_in)):
            rad = math.radians(ang)
            pts.append((cx + r * math.cos(rad), cy + r * math.sin(rad)))
        d.append("M" + " L".join("%.1f,%.1f" % p for p in pts) + " Z")
        a += teeth_step
    return " ".join(d)


def arc_band(cx, cy, r_in, r_out, a0, a1):
    def p(r, a):
        rad = math.radians(a)
        return cx + r * math.cos(rad), cy + r * math.sin(rad)
    large = 1 if (a1 - a0) > 180 else 0
    x0, y0 = p(r_out, a0)
    x1, y1 = p(r_out, a1)
    x2, y2 = p(r_in, a1)
    x3, y3 = p(r_in, a0)
    return ("M%.1f,%.1f A%.1f,%.1f 0 %d 1 %.1f,%.1f L%.1f,%.1f A%.1f,%.1f 0 %d 0 %.1f,%.1f Z" %
            (x0, y0, r_out, r_out, large, x1, y1, x2, y2, r_in, r_in, large, x3, y3))


# Simple runic strokes in a unit box (x right, y down).
RUNES = [
    [((0.5, 0), (0.5, 1)), ((0.5, 0.15), (0.9, 0.45)), ((0.5, 0.45), (0.9, 0.75))],
    [((0.3, 0), (0.3, 1)), ((0.3, 0), (0.8, 0.35)), ((0.8, 0.35), (0.3, 0.6))],
    [((0.5, 0), (0.5, 1)), ((0.15, 0.25), (0.85, 0.75)), ((0.85, 0.25), (0.15, 0.75))],
    [((0.2, 0), (0.2, 1)), ((0.8, 0), (0.8, 1)), ((0.2, 0.3), (0.8, 0.6))],
    [((0.5, 0), (0.5, 1)), ((0.5, 0.2), (0.15, 0.5)), ((0.5, 0.2), (0.85, 0.5))],
    [((0.25, 0), (0.75, 0.5)), ((0.75, 0.5), (0.25, 1)), ((0.25, 0), (0.25, 1))],
    [((0.15, 0.5), (0.5, 0)), ((0.5, 0), (0.85, 0.5)), ((0.85, 0.5), (0.5, 1)), ((0.5, 1), (0.15, 0.5))],
    [((0.5, 0), (0.5, 1)), ((0.2, 0.2), (0.8, 0.2))],
]


def rune_ring(cx, cy, radius, a0, a1, size):
    d = []
    count = 9
    for i in range(count):
        a = a0 + (a1 - a0) * (i + 0.5) / count
        rad = math.radians(a)
        px, py = cx + radius * math.cos(rad), cy + radius * math.sin(rad)
        rot = rad + math.pi / 2
        c, s = math.cos(rot), math.sin(rot)
        for (x0, y0), (x1, y1) in RUNES[i % len(RUNES)]:
            pts = []
            for ux, uy in ((x0, y0), (x1, y1)):
                lx, ly = (ux - 0.5) * size * 0.7, (uy - 0.5) * size
                pts.append((px + lx * c - ly * s, py + lx * s + ly * c))
            d.append("M%.1f,%.1f L%.1f,%.1f" % (pts[0] + pts[1]))
    return " ".join(d)


DEFS = """
<defs>
  <linearGradient id="gold" x1="0" y1="0" x2="0" y2="1">
    <stop offset="0" stop-color="#fff4c8"/>
    <stop offset="0.32" stop-color="#f1c35b"/>
    <stop offset="0.62" stop-color="#b8791f"/>
    <stop offset="1" stop-color="#5a320b"/>
  </linearGradient>
  <linearGradient id="steel" x1="0" y1="0" x2="0" y2="1">
    <stop offset="0" stop-color="#e2e3e4"/>
    <stop offset="0.4" stop-color="#8e939b"/>
    <stop offset="0.75" stop-color="#454952"/>
    <stop offset="1" stop-color="#1d1f24"/>
  </linearGradient>
  <linearGradient id="bronze" x1="0" y1="0" x2="1" y2="1">
    <stop offset="0" stop-color="#f0cd85"/>
    <stop offset="0.5" stop-color="#9a6427"/>
    <stop offset="1" stop-color="#3d240c"/>
  </linearGradient>
  <linearGradient id="iron" x1="0" y1="0" x2="0" y2="1">
    <stop offset="0" stop-color="#2d323c"/>
    <stop offset="1" stop-color="#0b0d12"/>
  </linearGradient>
  <linearGradient id="flame" x1="0" y1="1" x2="0" y2="0">
    <stop offset="0" stop-color="#ffe680"/>
    <stop offset="0.3" stop-color="#ff9a1f"/>
    <stop offset="0.7" stop-color="#e5520a"/>
    <stop offset="1" stop-color="#8a1800"/>
  </linearGradient>
  <linearGradient id="molten" x1="0" y1="0" x2="0" y2="1">
    <stop offset="0" stop-color="#ffe08a"/>
    <stop offset="0.4" stop-color="#ff8a1a"/>
    <stop offset="1" stop-color="#9c2200"/>
  </linearGradient>
  <radialGradient id="core" cx="0.5" cy="0.5" r="0.5">
    <stop offset="0" stop-color="#ffffff"/>
    <stop offset="0.18" stop-color="#ffe9a0"/>
    <stop offset="0.45" stop-color="#ff8a1a" stop-opacity="0.85"/>
    <stop offset="1" stop-color="#ff5a00" stop-opacity="0"/>
  </radialGradient>
  <radialGradient id="aura" cx="0.5" cy="0.5" r="0.5">
    <stop offset="0" stop-color="#ff7a1a" stop-opacity="0.55"/>
    <stop offset="0.55" stop-color="#a03200" stop-opacity="0.18"/>
    <stop offset="1" stop-color="#000000" stop-opacity="0"/>
  </radialGradient>
  <radialGradient id="hearth" cx="0.5" cy="0.62" r="0.55">
    <stop offset="0" stop-color="#5a1c02"/>
    <stop offset="0.6" stop-color="#1a0d08"/>
    <stop offset="1" stop-color="#07080c"/>
  </radialGradient>
</defs>
"""

CX, CY = 512.0, 400.0

SHIELD = [
    (512, 92), (640, 150), (742, 112), (724, 222), (820, 300), (760, 372), (800, 520),
    (700, 560), (650, 800), (512, 968), (374, 800), (324, 560), (224, 520), (264, 372),
    (204, 300), (300, 222), (282, 112), (384, 150),
]

ANVIL = [
    (348, 382), (402, 370), (444, 366), (640, 366), (668, 372), (668, 410), (618, 418),
    (594, 446), (590, 468), (636, 492), (642, 508), (382, 508), (388, 492), (434, 468),
    (430, 446), (406, 420), (370, 404),
]

FLAME = ("M512,170 C540,214 590,244 576,298 C600,284 606,262 604,246 "
         "C636,290 628,342 596,366 L428,366 C396,342 390,296 420,262 "
         "C420,284 430,298 446,304 C430,250 482,228 512,170 Z")

FLAME_INNER = ("M512,236 C528,266 560,286 552,322 C566,316 570,304 570,296 "
               "C586,326 578,352 560,366 L464,366 C446,352 442,330 456,312 "
               "C458,326 466,332 474,334 C470,300 496,280 512,236 Z")


def crest_body(with_text, fonts):
    parts = []
    # Without the name plate the lower point of the shield is shortened.
    shield = SHIELD if with_text else [(x, y if y <= 520 else 520 + (y - 520) * 0.55)
                                       for x, y in SHIELD]
    seam = [(512, 820), (512, 930)] if with_text else [(512, 640), (512, 735)]
    parts.append('<circle cx="512" cy="470" r="470" fill="url(#aura)"/>')

    # Spear blade behind the ring.
    parts.append('<polygon points="%s" fill="url(#steel)" stroke="#16171b" stroke-width="5"/>' %
                 _pts([(512, 18), (548, 150), (536, 250), (488, 250), (476, 150)]))
    parts.append('<polyline points="%s" fill="none" stroke="#ffffff" stroke-opacity="0.55" '
                 'stroke-width="3"/>' % _pts([(512, 30), (512, 240)]))

    # Shield frame: iron body, bronze rim, gold hairline.
    parts.append('<polygon points="%s" fill="url(#iron)" stroke="url(#bronze)" '
                 'stroke-width="18" stroke-linejoin="miter"/>' % _pts(shield))
    parts.append('<polygon points="%s" fill="none" stroke="#f2c869" stroke-opacity="0.75" '
                 'stroke-width="3" transform="translate(512 520) scale(0.93) translate(-512 -520)"/>'
                 % _pts(shield))
    # Fire seam down the lower point (ATOM GameForge).
    parts.append('<polyline points="%s" fill="none" stroke="#ff9a2a" stroke-width="5"/>' %
                 _pts(seam))

    # Ring: steel half gear on the left, rune band on the right.
    parts.append('<path d="%s" fill="url(#steel)" stroke="#16171b" stroke-width="4"/>' %
                 gear_path(CX, CY, 214, 252, 104, 256, 12))
    parts.append('<path d="%s" fill="url(#steel)" stroke="#16171b" stroke-width="5"/>' %
                 arc_band(CX, CY, 172, 222, 95, 265))
    parts.append('<path d="%s" fill="url(#iron)" stroke="url(#bronze)" stroke-width="7"/>' %
                 arc_band(CX, CY, 172, 248, -85, 85))
    parts.append('<path d="%s" fill="none" stroke="#ff9a2a" stroke-width="7" '
                 'stroke-linecap="round"/>' % rune_ring(CX, CY, 210, -80, 80, 34))
    parts.append('<path d="%s" fill="none" stroke="#ffe3a0" stroke-width="2.5" '
                 'stroke-linecap="round"/>' % rune_ring(CX, CY, 210, -80, 80, 34))
    # Hearth inside the ring.
    parts.append('<circle cx="%.0f" cy="%.0f" r="172" fill="url(#hearth)" stroke="url(#bronze)" '
                 'stroke-width="8"/>' % (CX, CY))

    # Flame and the fire core.
    parts.append('<circle cx="512" cy="340" r="130" fill="url(#core)" opacity="0.45"/>')
    parts.append('<path d="%s" fill="url(#flame)"/>' % FLAME)
    parts.append('<path d="%s" fill="#ffd36a" opacity="0.75"/>' % FLAME_INNER)

    # Anvil.
    parts.append('<polygon points="%s" fill="url(#steel)" stroke="#121317" stroke-width="7" '
                 'stroke-linejoin="round"/>' % _pts(ANVIL))
    parts.append('<polyline points="%s" fill="none" stroke="#ffffff" stroke-opacity="0.8" '
                 'stroke-width="4"/>' % _pts([(404, 372), (640, 372)]))
    parts.append('<polyline points="%s" fill="none" stroke="#ff9a2a" stroke-width="4"/>' %
                 _pts([(512, 380), (512, 500)]))
    parts.append('<circle cx="512" cy="370" r="46" fill="url(#core)"/>')

    if with_text:
        # Name plate with molten drips.
        plate = [(70, 620), (150, 572), (874, 572), (954, 620), (874, 742), (150, 742)]
        drips = []
        rnd = random.Random(7)
        for x in (196, 236, 282, 300, 700, 724, 770, 812, 846):
            length = rnd.uniform(50, 170)
            w = rnd.uniform(9, 16)
            drips.append("M%.1f,736 C%.1f,%.1f %.1f,%.1f %.1f,%.1f "
                         "A%.1f,%.1f 0 1 1 %.1f,%.1f C%.1f,%.1f %.1f,%.1f %.1f,736 Z" % (
                             x - w, x - w * 0.6, 736 + length * 0.5, x - w * 0.5, 736 + length * 0.8,
                             x - w * 0.55, 736 + length,
                             w * 0.55, w * 0.55, x + w * 0.55, 736 + length,
                             x + w * 0.5, 736 + length * 0.8, x + w * 0.6, 736 + length * 0.5, x + w))
        parts.append('<path d="%s" fill="url(#molten)"/>' % " ".join(drips))
        parts.append('<polygon points="%s" fill="url(#iron)" stroke="url(#bronze)" '
                     'stroke-width="12" stroke-linejoin="miter"/>' % _pts(plate))
        parts.append('<polyline points="%s" fill="none" stroke="#ff8a1a" stroke-width="4"/>' %
                     _pts([(170, 738), (854, 738)]))

        title = text_path(fonts["CinzelDecorative-Black.ttf"], "GAMEFORGE", 512, 696, 104,
                          tracking=0.0, max_width=780)
        parts.append('<path d="%s" fill="#1a0c03" transform="translate(5 7)"/>' % title)
        parts.append('<path d="%s" fill="url(#gold)" stroke="#2a1606" stroke-width="5" '
                     'paint-order="stroke"/>' % title)

        sub_plate = [(330, 770), (360, 752), (664, 752), (694, 770), (664, 822), (360, 822)]
        parts.append('<polygon points="%s" fill="url(#iron)" stroke="url(#bronze)" '
                     'stroke-width="7"/>' % _pts(sub_plate))
        sub = text_path(fonts["Cinzel[wght].ttf"], "3D STUDIO", 512, 804, 38, tracking=0.22,
                        max_width=300)
        parts.append('<path d="%s" fill="#e9e6df" stroke="#0b0b0d" stroke-width="3" '
                     'paint-order="stroke"/>' % sub)
    return "\n".join(parts)


def crest_svg(with_text, fonts, view_box=None):
    vb = view_box or ("0 0 1024 1024")
    return ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="%s">\n'
            '<title>GameForge 3D Studio</title>\n%s\n%s\n</svg>\n' %
            (vb, DEFS, crest_body(with_text, fonts)))


def wordmark_svg(fonts):
    """Single color wordmark for the About dialog (tinted by the theme)."""
    w, h = 5236, 1600
    title = text_path(fonts["CinzelDecorative-Black.ttf"], "GAMEFORGE", w / 2, 980, 760,
                      max_width=w - 120)
    sub = text_path(fonts["Cinzel[wght].ttf"], "3D STUDIO", w / 2, 1520, 330, tracking=0.35,
                    max_width=w * 0.44)
    rule = "M%d,1340 H%d V1370 H%d Z M%d,1340 H%d V1370 H%d Z" % (
        300, w * 0.24, 300, w * 0.76, w - 300, w * 0.76)
    return ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d" width="%d" height="%d">\n'
            '<path fill="#ffffff" d="%s %s %s"/>\n</svg>\n' % (w, h, w, h, title, sub, rule))


def symbolic_svg():
    """Monochrome 16px crest for desktop environments that use symbolic icons."""
    shield = [(x, y if y <= 520 else 520 + (y - 520) * 0.55) for x, y in SHIELD]
    k = 16 / 772.0

    def tr(points):
        return " ".join("%.2f,%.2f" % ((x - 126) * k, (y - 10) * k) for x, y in points)
    inner = [(512 + (x - 512) * 0.8, 430 + (y - 430) * 0.8) for x, y in shield]
    return ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 16 16">\n'
            '<path fill="#000000" fill-rule="evenodd" d="M%s Z M%s Z M%s Z"/>\n</svg>\n' % (
                tr(shield), tr(inner[::-1]), tr(ANVIL)))


# -----------------------------------------------------------------------------
# Raster finishing: cracked metal texture, glow and sparks.

def render_svg(svg, size):
    png = cairosvg.svg2png(bytestring=svg.encode("utf-8"), output_width=size, output_height=size)
    return Image.open(io.BytesIO(png)).convert("RGBA")


def fractal_noise(size, seed, octaves=5):
    rng = np.random.default_rng(seed)
    acc = np.zeros((size, size), np.float32)
    amp, total = 1.0, 0.0
    for o in range(octaves):
        cells = 4 * 2 ** o
        small = rng.random((cells, cells)).astype(np.float32)
        layer = np.asarray(Image.fromarray((small * 255).astype(np.uint8)).resize(
            (size, size), Image.BICUBIC), np.float32) / 255.0
        acc += layer * amp
        total += amp
        amp *= 0.55
    return acc / total


def crack_layer(size, seed, count):
    rnd = random.Random(seed)
    img = Image.new("L", (size, size), 0)
    draw = ImageDraw.Draw(img)
    for _ in range(count):
        x, y = rnd.uniform(0, size), rnd.uniform(0, size)
        ang = rnd.uniform(0, math.tau)
        for _ in range(rnd.randint(4, 12)):
            step = rnd.uniform(size * 0.006, size * 0.02)
            ang += rnd.uniform(-0.9, 0.9)
            nx, ny = x + math.cos(ang) * step, y + math.sin(ang) * step
            draw.line((x, y, nx, ny), fill=255, width=max(1, size // 700))
            x, y = nx, ny
    return img


def finish_metal(img, seed=1):
    w, h = img.size
    size = max(w, h)
    arr = np.asarray(img, np.float32) / 255.0
    rgb, alpha = arr[..., :3], arr[..., 3:4]
    lum = rgb.mean(axis=2, keepdims=True)
    sat = rgb.max(axis=2, keepdims=True) - rgb.min(axis=2, keepdims=True)
    # Metal: bright-ish and not the saturated fire.
    metal = np.clip((lum - 0.18) * 3.0, 0, 1) * np.clip(1.0 - (sat - 0.35) * 3.0, 0, 1)

    noise = fractal_noise(size, seed)[:h, :w, None]
    rgb = rgb * (0.78 + 0.32 * noise * metal + 0.22 * (1 - metal))

    cracks = np.asarray(crack_layer(size, seed, 260).filter(ImageFilter.GaussianBlur(0.6)),
                        np.float32)[:h, :w, None] / 255.0
    crack_light = np.roll(np.roll(cracks, -1, 0), -1, 1)
    rgb = rgb * (1 - 0.65 * cracks * metal) + 0.25 * crack_light * metal * (1 - cracks)
    out = np.concatenate([np.clip(rgb, 0, 1), alpha], axis=2)
    return Image.fromarray((out * 255).astype(np.uint8), "RGBA")


def add_glow(img, radius_scale=1.0):
    size = img.size[0]
    arr = np.asarray(img, np.float32) / 255.0
    r, g, b, a = arr[..., 0], arr[..., 1], arr[..., 2], arr[..., 3]
    fire = np.clip((r - 0.7) * 3, 0, 1) * np.clip((r - b - 0.35) * 3, 0, 1) * a
    fire_img = Image.fromarray((fire * 255).astype(np.uint8), "L")
    glow = np.zeros_like(arr[..., :3])
    for rad, strength in ((size * 0.006, 0.9), (size * 0.02, 0.7), (size * 0.05, 0.45)):
        blurred = np.asarray(fire_img.filter(ImageFilter.GaussianBlur(rad * radius_scale)),
                             np.float32)[..., None] / 255.0
        glow += blurred * np.array([1.0, 0.55, 0.12]) * strength
    rgb = arr[..., :3] * a[..., None]
    rgb = 1 - (1 - rgb) * (1 - np.clip(glow, 0, 1))
    alpha = np.clip(np.maximum(a, glow.max(axis=2)), 0, 1)
    with np.errstate(divide="ignore", invalid="ignore"):
        rgb = np.where(alpha[..., None] > 0, rgb / alpha[..., None], 0)
    out = np.concatenate([np.clip(rgb, 0, 1), alpha[..., None]], axis=2)
    return Image.fromarray((out * 255).astype(np.uint8), "RGBA")


def sparks(size, seed, count, box=None):
    rnd = random.Random(seed)
    layer = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(layer)
    x0, y0, x1, y1 = box or (0, 0, size, size)
    for _ in range(count):
        x, y = rnd.uniform(x0, x1), rnd.uniform(y0, y1)
        r = rnd.choice((1, 1, 1, 2, 2, 3)) * size / 1024
        heat = rnd.random()
        col = (255, int(140 + 100 * heat), int(40 + 80 * heat), int(150 + 105 * rnd.random()))
        draw.ellipse((x - r, y - r, x + r, y + r), fill=col)
    halo = layer.filter(ImageFilter.GaussianBlur(size / 300))
    return Image.alpha_composite(halo, layer)


def raster_crest(svg, size, seed=3, with_sparks=True):
    img = render_svg(svg, size)
    img = finish_metal(img, seed)
    img = add_glow(img)
    if with_sparks:
        sp = sparks(size, seed, int(size * 0.12))
        # Keep sparks around the crest, not in the corners.
        mask = Image.new("L", (size, size), 0)
        ImageDraw.Draw(mask).ellipse((size * 0.04, size * 0.04, size * 0.96, size * 0.96), fill=255)
        mask = mask.filter(ImageFilter.GaussianBlur(size * 0.06))
        sp.putalpha(ImageChops.multiply(sp.getchannel("A"), mask))
        img = Image.alpha_composite(sp, img)
    return img


# -----------------------------------------------------------------------------
# Splash screen.

def ornate_frame(draw, w, h, inset, color, glow_color):
    x0, y0, x1, y1 = inset, inset, w - inset, h - inset
    draw.rectangle((x0, y0, x1, y1), outline=color, width=3)
    draw.rectangle((x0 + 9, y0 + 9, x1 - 9, y1 - 9), outline=color + (110,), width=1)
    # Corner brackets.
    arm = h * 0.09
    for cx, cy, sx, sy in ((x0, y0, 1, 1), (x1, y0, -1, 1), (x0, y1, 1, -1), (x1, y1, -1, -1)):
        draw.line((cx, cy + sy * arm, cx, cy, cx + sx * arm, cy), fill=GOLD_BRIGHT, width=7)
        d = 16
        draw.polygon([(cx + sx * 18, cy + sy * (18 - d)), (cx + sx * (18 + d), cy + sy * 18),
                      (cx + sx * 18, cy + sy * (18 + d)), (cx + sx * (18 - d), cy + sy * 18)],
                     fill=GOLD_BRIGHT)
    # Diamonds at the middle of the top and bottom edges.
    for cy in (y0, y1):
        mx = w / 2
        draw.polygon([(mx, cy - 16), (mx + 26, cy), (mx, cy + 16), (mx - 26, cy)],
                     fill=glow_color, outline=GOLD_BRIGHT)
        draw.line((mx - 140, cy, mx - 34, cy), fill=GOLD_BRIGHT, width=5)
        draw.line((mx + 34, cy, mx + 140, cy), fill=GOLD_BRIGHT, width=5)


def make_splash(crest, fonts, out_path, w=2000, h=1000):
    rng = np.random.default_rng(11)
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    # Night-sky navy with a blue arcane glow on the right and forge glow on the left.
    base = np.zeros((h, w, 3), np.float32)
    t = yy / h
    base += np.array(NAVY_DEEP, np.float32) * (1 - t)[..., None] + np.array(NAVY, np.float32) * t[..., None]
    d_blue = np.hypot(xx - w * 0.72, yy - h * 0.38) / (w * 0.5)
    base += np.array([20, 60, 140], np.float32) * np.clip(1 - d_blue, 0, 1)[..., None] ** 2 * 0.9
    d_fire = np.hypot(xx - w * 0.25, yy - h * 0.55) / (w * 0.32)
    base += np.array([150, 55, 8], np.float32) * np.clip(1 - d_fire, 0, 1)[..., None] ** 2 * 0.8
    noise = fractal_noise(max(w, h), 5)[:h, :w, None]
    base *= 0.85 + 0.3 * noise
    img = Image.fromarray(np.clip(base, 0, 255).astype(np.uint8), "RGB").convert("RGBA")

    # Stars.
    stars = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    sd = ImageDraw.Draw(stars)
    for _ in range(900):
        x, y = rng.uniform(0, w), rng.uniform(0, h)
        r = rng.choice([0.6, 0.8, 1.0, 1.4, 2.0])
        a = int(rng.uniform(40, 220))
        sd.ellipse((x - r, y - r, x + r, y + r), fill=(190, 210, 255, a))
    img = Image.alpha_composite(img, stars)

    # Arcane rune circle behind the title.
    arcane = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    ad = ImageDraw.Draw(arcane)
    acx, acy = w * 0.72, h * 0.40
    for r, wd, a in ((430, 2, 70), (400, 1, 60), (300, 2, 50), (290, 1, 40)):
        ad.ellipse((acx - r, acy - r, acx + r, acy + r), outline=(90, 160, 255, a), width=wd)
    for i in range(12):
        ang = math.tau * i / 12
        p0 = (acx + 300 * math.cos(ang), acy + 300 * math.sin(ang))
        p1 = (acx + 400 * math.cos(ang + math.tau * 5 / 12), acy + 400 * math.sin(ang + math.tau * 5 / 12))
        ad.line((p0, p1), fill=(90, 160, 255, 35), width=1)
    arcane = Image.alpha_composite(arcane.filter(ImageFilter.GaussianBlur(3)), arcane)
    img = Image.alpha_composite(img, arcane)

    # Crest on the left.
    crest_size = int(h * 0.8)
    c = crest.resize((crest_size, crest_size), Image.LANCZOS)
    img.alpha_composite(c, (int(w * 0.25 - crest_size / 2), int(h * 0.5 - crest_size / 2)))
    img = Image.alpha_composite(img, sparks(w, 21, 160, (40, 60, w * 0.5, h - 60)).crop((0, 0, w, h)))

    # Title on the right.
    title_layer = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    td = ImageDraw.Draw(title_layer)
    f_title = ImageFont.truetype(fonts["CinzelDecorative-Black.ttf"], 128)
    f_sub = ImageFont.truetype(fonts["Cinzel[wght].ttf"], 64)
    f_sub.set_variation_by_axes([700])
    f_tag = ImageFont.truetype(fonts["Cinzel[wght].ttf"], 36)
    f_tag.set_variation_by_axes([600])
    f_small = ImageFont.truetype(fonts["Cinzel[wght].ttf"], 26)
    f_small.set_variation_by_axes([500])

    tx = w * 0.70
    td.text((tx, 380), "GAMEFORGE", font=f_title, anchor="ms", fill=(255, 255, 255, 255))
    title_mask = title_layer.getchannel("A")
    # Gold gradient fill for the title.
    grad = np.zeros((h, w, 3), np.float32)
    stops = np.array([[255, 244, 200], [241, 195, 91], [184, 121, 31], [110, 64, 16]], np.float32)
    ty = np.clip((yy - 250) / 140, 0, 1)
    idx = np.clip((ty * 3).astype(int), 0, 2)
    frac = (ty * 3 - idx)[..., None]
    grad = stops[idx] * (1 - frac) + stops[np.clip(idx + 1, 0, 3)] * frac
    gold = Image.fromarray(grad.astype(np.uint8), "RGB").convert("RGBA")
    gold.putalpha(title_mask)
    gold = finish_metal(gold, 9)
    shadow = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    shadow.putalpha(title_mask.filter(ImageFilter.GaussianBlur(6)))
    shadow = ImageChops.offset(shadow, 6, 8)
    outline = Image.new("RGBA", (w, h), (40, 22, 6, 0))
    outline.putalpha(title_mask.filter(ImageFilter.MaxFilter(7)))
    img = Image.alpha_composite(img, shadow)
    img = Image.alpha_composite(img, outline)
    img = Image.alpha_composite(img, gold)

    deco = ImageDraw.Draw(img)
    deco.text((tx, 470), "3 D   S T U D I O", font=f_sub, anchor="ms", fill=(214, 220, 228, 255),
              stroke_width=2, stroke_fill=(10, 12, 18, 255))
    # Divider with a glowing diamond.
    deco.line((tx - 330, 520, tx - 30, 520), fill=GOLD, width=3)
    deco.line((tx + 30, 520, tx + 330, 520), fill=GOLD, width=3)
    deco.polygon([(tx, 506), (tx + 16, 520), (tx, 534), (tx - 16, 520)], fill=(110, 180, 255),
                 outline=GOLD_BRIGHT)
    deco.text((tx, 590), "FORGE.  CREATE.  DOMINATE.", font=f_tag, anchor="ms", fill=GOLD_BRIGHT)
    deco.text((tx, 650), "Native Angelica Engine  •  Perfect World editors  •  AI agents",
              font=f_small, anchor="ms", fill=(170, 182, 205))
    deco.text((tx, h - 70), "Based on Blender", font=f_small, anchor="ms", fill=(120, 130, 150))

    frame = ImageDraw.Draw(img)
    ornate_frame(frame, w, h, 18, GOLD, (60, 140, 255))
    img = add_glow(img, 0.6)
    img.convert("RGB").save(out_path, optimize=True)


# -----------------------------------------------------------------------------
# Icons.

ICON_VIEWBOX = "126 10 772 772"


def make_icons(icon_svg, ico_path):
    big = raster_crest(icon_svg, 1024, seed=4, with_sparks=False)
    sizes = [16, 24, 32, 48, 64, 128, 256]
    big.save(ico_path, format="ICO", sizes=[(s, s) for s in sizes])
    return big


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--root", default=".", help="Repository root.")
    parser.add_argument("--font-dir", default=os.path.join(os.path.expanduser("~"), ".cache",
                                                            "gameforge-fonts"))
    args = parser.parse_args()

    fonts = ensure_fonts(args.font_dir)
    root = args.root
    out_dir = os.path.join(root, "release", "datafiles", "gameforge")
    os.makedirs(out_dir, exist_ok=True)

    emblem_svg = crest_svg(True, fonts)
    icon_svg = crest_svg(False, fonts, ICON_VIEWBOX)
    for name, svg in (("gameforge_emblem.svg", emblem_svg), ("gameforge_icon.svg", icon_svg)):
        with open(os.path.join(out_dir, name), "w", encoding="utf-8") as fh:
            fh.write(svg)
    for path, svg in (
            (("release", "freedesktop", "icons", "scalable", "apps", "blender.svg"), icon_svg),
            (("release", "freedesktop", "icons", "symbolic", "apps", "blender-symbolic.svg"),
             symbolic_svg()),
            (("release", "datafiles", "icons_svg", "blender_logo_large.svg"), wordmark_svg(fonts)),
    ):
        os.makedirs(os.path.join(root, *path[:-1]), exist_ok=True)
        with open(os.path.join(root, *path), "w", encoding="utf-8") as fh:
            fh.write(svg)

    crest = raster_crest(emblem_svg, 1024)
    crest.save(os.path.join(out_dir, "gameforge_emblem.png"), optimize=True)
    icon = make_icons(icon_svg, os.path.join(root, "release", "windows", "icons", "winblender.ico"))
    icon.resize((512, 512), Image.LANCZOS).save(os.path.join(out_dir, "gameforge_icon.png"),
                                                optimize=True)
    splash_crest = raster_crest(crest_svg(False, fonts, ICON_VIEWBOX), 1024, seed=5)
    make_splash(splash_crest, fonts, os.path.join(root, "release", "datafiles", "splash.png"))
    print("Branding written to", out_dir)


if __name__ == "__main__":
    main()
