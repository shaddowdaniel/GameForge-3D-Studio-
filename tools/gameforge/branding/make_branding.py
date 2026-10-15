#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Generate the GameForge 3D Studio logo, splash screen and application icons.

The logo is the PWOps logo with only its name replaced (see ``make_logo.py``):
cracked gold and steel letters, gear and rune circle, spear, orange fire core
and molten metal.

Outputs:

- ``release/datafiles/gameforge/gameforge_logo.png``     full logo (1254px).
- ``release/datafiles/gameforge/gameforge_crest.png``    crest without lettering.
- ``release/datafiles/splash.png``                        splash screen.
- ``release/freedesktop/icons/scalable/apps/blender.svg`` Linux application icon.
- ``release/freedesktop/icons/symbolic/apps/blender-symbolic.svg`` monochrome icon.
- ``release/datafiles/icons_svg/blender_logo_large.svg``  wordmark of the About dialog.
- ``release/windows/icons/winblender.ico``                Windows application icon.

Requirements: Python 3,
``pip install pillow numpy fonttools opencv-python-headless``.
The Cinzel fonts (SIL Open Font License) are downloaded from google/fonts on
first run into ``--font-dir`` and are only used to draw the lettering; they
are not shipped with the application.

Usage, from the repository root::

   python3 tools/gameforge/branding/make_branding.py
"""

import argparse
import base64
import io
import math
import sys
import os
import random
import urllib.request

import numpy as np
from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.pens.transformPen import TransformPen
from fontTools.ttLib import TTFont
from PIL import Image, ImageDraw, ImageFilter, ImageFont

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import make_logo  # noqa: E402

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
# Vector marks.

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


def mark_paths():
    """The crest reduced to one color: gear and rune ring, spear, fire core (1000 units)."""
    cx, cy = 500.0, 580.0

    def ring(r_out, r_in):
        return ("M%.1f,%.1f A%.1f,%.1f 0 1 1 %.1f,%.1f Z M%.1f,%.1f A%.1f,%.1f 0 1 0 %.1f,%.1f Z"
                % (cx, cy - r_out, r_out, r_out, cx - 0.1, cy - r_out,
                   cx, cy - r_in, r_in, r_in, cx + 0.1, cy - r_in))

    teeth = []
    for i in range(7):
        a = math.radians(115 + i * 21.7)
        pts = []
        for da, r in ((-7, 330), (-5, 400), (5, 400), (7, 330)):
            aa = a + math.radians(da)
            pts.append((cx + r * math.cos(aa), cy + r * math.sin(aa)))
        teeth.append("M" + " L".join("%.1f,%.1f" % p for p in pts) + " Z")
    # Rune marks cut into the right half of the ring.
    runes = []
    for i in range(6):
        a = math.radians(-62 + i * 24.8)
        pts = []
        for da, r in ((-3.2, 272), (-3.2, 328), (3.2, 328), (3.2, 272)):
            aa = a + math.radians(da)
            pts.append((cx + r * math.cos(aa), cy + r * math.sin(aa)))
        runes.append("M" + " L".join("%.1f,%.1f" % p for p in pts) + " Z")
    spear = "M500,10 L600,320 L566,480 L434,480 L400,320 Z"
    core = ("M%.1f,%.1f A105,105 0 1 1 %.1f,%.1f Z M%.1f,%.1f A70,70 0 1 0 %.1f,%.1f Z "
            "M%.1f,%.1f A42,42 0 1 1 %.1f,%.1f Z" % (
                cx, cy - 105, cx - 0.1, cy - 105, cx, cy - 70, cx + 0.1, cy - 70,
                cx, cy - 42, cx - 0.1, cy - 42))
    return [ring(345, 255) + " " + " ".join(runes), " ".join(teeth), spear, core]


def mark_svg(color, size=1000):
    paths = "".join('<path fill="%s" fill-rule="evenodd" d="%s"/>' % (color, d)
                    for d in mark_paths())
    return ('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1000 1000" width="%d" '
            'height="%d">\n%s\n</svg>\n' % (size, size, paths))


def symbolic_svg():
    """Monochrome mark for desktops that use symbolic icons."""
    return mark_svg("#000000", 16)


def x11_icon_header(crest):
    """Window icon pixel data for X11 (`_NET_WM_ICON`, ARGB per pixel)."""
    lines = []
    for size in (16, 32, 48, 64):
        img = crest.resize((size, size), Image.LANCZOS).convert("RGBA")
        values = ["%d" % size, "%d" % size]
        for r, g, b, a in np.asarray(img).reshape(-1, 4).tolist():
            values.append("0x%x" % ((a << 24) | (r << 16) | (g << 8) | b))
        for i in range(0, len(values), 8):
            lines.append("    " + " ".join("%s," % v for v in values[i:i + 8]))
    return (
        "/* SPDX-FileCopyrightText: 2001-2002 NaN Holding BV. All rights reserved.\n"
        " *\n"
        " * SPDX-License-Identifier: GPL-2.0-or-later */\n\n"
        "/** \\file\n"
        " * \\ingroup GHOST\n"
        " * Icon image data for X11: the GameForge 3D Studio crest at 16, 32, 48 and 64 pixels.\n"
        " *\n"
        " * Generated by `tools/gameforge/branding/make_branding.py`, do not edit by hand.\n"
        " */\n\n"
        "#pragma once\n\n"
        "/** \\note Using 'unsigned' to avoid `-Wnarrowing` warning. */\n"
        "static const unsigned long BLENDER_ICONS_WM_X11[] = {\n" + "\n".join(lines) + "\n};\n")


# -----------------------------------------------------------------------------
# Raster finishing: cracked metal texture, glow and sparks.

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


def make_splash(logo, fonts, out_path, w=2000, h=1000):
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

    # Logo on the left, its black background melting into a dark halo.
    size = int(h * 0.98)
    lx, ly = int(w * 0.27 - size / 2), int(h * 0.5 - size / 2)
    halo = Image.new("L", (w, h), 0)
    ImageDraw.Draw(halo).ellipse((lx + size * 0.08, ly + size * 0.06, lx + size * 0.92,
                                  ly + size * 0.94), fill=235)
    halo = halo.filter(ImageFilter.GaussianBlur(size * 0.07))
    img = Image.composite(Image.new("RGBA", (w, h), (2, 3, 6, 255)), img, halo)
    lg = logo.convert("RGBA").resize((size, size), Image.LANCZOS)
    edge = Image.new("L", (size, size), 0)
    ImageDraw.Draw(edge).ellipse((size * 0.06, size * 0.04, size * 0.94, size * 0.96), fill=255)
    lg.putalpha(edge.filter(ImageFilter.GaussianBlur(size * 0.04)))
    img.alpha_composite(lg, (lx, ly))

    f_tag = ImageFont.truetype(fonts["Cinzel[wght].ttf"], 52)
    f_tag.set_variation_by_axes([700])
    f_small = ImageFont.truetype(fonts["Cinzel[wght].ttf"], 30)
    f_small.set_variation_by_axes([500])
    tx = w * 0.74

    deco = ImageDraw.Draw(img)
    # Divider with a glowing diamond.
    deco.line((tx - 330, 535, tx - 30, 535), fill=GOLD, width=3)
    deco.line((tx + 30, 535, tx + 330, 535), fill=GOLD, width=3)
    deco.polygon([(tx, 521), (tx + 16, 535), (tx, 549), (tx - 16, 535)], fill=(110, 180, 255),
                 outline=GOLD_BRIGHT)
    deco.text((tx, 470), "FORGE.  CREATE.  DOMINATE.", font=f_tag, anchor="ms", fill=GOLD_BRIGHT)
    for i, line in enumerate(("Native Angelica Engine", "Perfect World editors", "AI agents")):
        deco.text((tx, 610 + i * 52), line, font=f_small, anchor="ms", fill=(170, 182, 205))

    frame = ImageDraw.Draw(img)
    ornate_frame(frame, w, h, 18, GOLD, (60, 140, 255))
    img = add_glow(img, 0.6)
    img.convert("RGB").save(out_path, optimize=True)


# -----------------------------------------------------------------------------
# Icons.

def png_svg(img):
    """Wrap a raster image in an SVG, for the freedesktop scalable icon slot."""
    buf = io.BytesIO()
    img.save(buf, format="PNG", optimize=True)
    data = base64.b64encode(buf.getvalue()).decode("ascii")
    w, h = img.size
    return ('<svg xmlns="http://www.w3.org/2000/svg" xmlns:xlink="http://www.w3.org/1999/xlink" '
            'viewBox="0 0 %d %d">\n<title>GameForge 3D Studio</title>\n'
            '<image width="%d" height="%d" xlink:href="data:image/png;base64,%s"/>\n</svg>\n'
            % (w, h, w, h, data))


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--root", default=".", help="Repository root.")
    parser.add_argument("--pwops-logo", default=os.path.join(
        "release", "datafiles", "gameforge", "source", "pwops_logo.png"),
        help="The PWOps logo the GameForge logo is made from.")
    parser.add_argument("--font-dir", default=os.path.join(os.path.expanduser("~"), ".cache",
                                                            "gameforge-fonts"))
    args = parser.parse_args()

    fonts = ensure_fonts(args.font_dir)
    root = args.root
    out_dir = os.path.join(root, "release", "datafiles", "gameforge")
    os.makedirs(out_dir, exist_ok=True)

    logo, crest = make_logo.build(args.pwops_logo, fonts)
    logo.save(os.path.join(out_dir, "gameforge_logo.png"), optimize=True)
    crest.save(os.path.join(out_dir, "gameforge_crest.png"), optimize=True)

    icon = crest.resize((256, 256), Image.LANCZOS)
    icon.save(os.path.join(out_dir, "gameforge_app_icon.png"), optimize=True)
    crest.resize((512, 512), Image.LANCZOS).save(os.path.join(
        root, "release", "darwin", "Blender.app", "Contents", "Resources",
                            "blender_icon_legacy.icns"), format="ICNS")
    with open(os.path.join(root, "intern", "ghost", "intern", "GHOST_IconX11.hh"), "w",
              encoding="utf-8") as fh:
        fh.write(x11_icon_header(crest))
    crest.save(os.path.join(root, "release", "windows", "icons", "winblender.ico"), format="ICO",
               sizes=[(s, s) for s in (16, 24, 32, 48, 64, 128, 256)])
    for path, svg in (
            (("release", "freedesktop", "icons", "scalable", "apps", "blender.svg"), png_svg(icon)),
            (("release", "freedesktop", "icons", "symbolic", "apps", "blender-symbolic.svg"),
             symbolic_svg()),
            (("release", "datafiles", "icons_svg", "blender_logo_large.svg"), wordmark_svg(fonts)),
            # Menu icon at the left of the top bar and of client-side window decorations.
            (("release", "datafiles", "icons_svg", "blender.svg"), mark_svg("#ffffff")),
            # macOS 26 icon layer (drawn on the icon's own dark gradient).
            (("release", "darwin", "blender_liquid_glass.icon", "Assets", "Blender Logo.svg"),
             mark_svg("#e9b85a")),
    ):
        os.makedirs(os.path.join(root, *path[:-1]), exist_ok=True)
        with open(os.path.join(root, *path), "w", encoding="utf-8") as fh:
            fh.write(svg)

    make_splash(logo, fonts, os.path.join(root, "release", "datafiles", "splash.png"))
    print("Branding written to", out_dir)


if __name__ == "__main__":
    main()
