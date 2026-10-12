#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Apply the GameForge "Forge" palette to Blender's built-in default theme.

``release/datafiles/userdef/userdef_default_theme.c`` is generated upstream by
``tools/utils/blender_theme_as_c.py``. Rather than hand editing the generated
file (and fighting every upstream sync), this script re-colors it:

- Neutral greys used for UI surfaces become the dark navy ramp of the
  PWOps / ATOM GameForge style, and light greys used for text become a warm
  parchment white.
- Blender's blue accent becomes the GameForge royal blue.
- A small table of overrides adds the bronze/gold frames: widget outlines,
  panel titles and outlines, the active editor border, popup frames, tabs.

Colors outside the UI (bone color sets, collection and strip colors) are left
untouched, as are saturated colors used for data (axes, keyframes, handles).

Usage (from the repository root)::

   python3 tools/gameforge/apply_theme.py            # rewrite in place
   python3 tools/gameforge/apply_theme.py --check    # exit 1 if not applied

Run it again after merging an upstream change to the default theme.
The transform is idempotent: already re-colored values are not grey any more.
"""

import argparse
import os
import re
import sys

THEME_PATH = os.path.join("release", "datafiles", "userdef", "userdef_default_theme.c")

# -----------------------------------------------------------------------------
# Palette.

# Grey value (0..255) -> navy surface. Interpolated linearly between stops.
NAVY_RAMP = (
    (0x00, 0x000000),
    (0x12, 0x050810),
    (0x16, 0x060a13),
    (0x18, 0x070c16),
    (0x1d, 0x0a1120),
    (0x22, 0x0c1426),
    (0x28, 0x0f192d),
    (0x2d, 0x111c33),
    (0x30, 0x121e36),
    (0x3d, 0x172742),
    (0x54, 0x203453),
    (0x72, 0x34496b),
    (0x80, 0x4a5a78),
    (0x99, 0x8c8f9a),
    (0xb8, 0xb7b1a3),
    (0xd9, 0xdcd3c0),
    (0xe6, 0xe9e0cc),
    (0xff, 0xffffff),
)

# Exact color replacements (RGB only, alpha is kept).
ACCENTS = {
    0x4772b3: 0x2a6fd6,  # Blender blue -> GameForge royal blue.
    0x71a8ff: 0xf2c46a,  # Text cursor -> gold.
    0x6fa9e6: 0x6fb2ff,  # Links.
    0x3b5689: 0x24508f,
    0x223a5b: 0x1b3560,
}

GOLD = 0xd9ad55
GOLD_BRIGHT = 0xf2c869
BRONZE = 0x76582e
BRONZE_DARK = 0x4a3820
BLUE = 0x2a6fd6

# Overrides by dotted path, value is RGBA.
OVERRIDES = {
    # Widgets: bronze frames, gold when active.
    **{
        "tui.%s.%s" % (wcol, field): value
        for wcol in (
            "wcol_regular", "wcol_tool", "wcol_toolbar_item", "wcol_text", "wcol_radio",
            "wcol_option", "wcol_toggle", "wcol_num", "wcol_numslider", "wcol_box",
            "wcol_scroll", "wcol_progress",
        )
        for field, value in (
            ("outline", (BRONZE << 8) | 0xff),
            ("outline_sel", (GOLD << 8) | 0xff),
        )
    },
    # Popups, menus and tooltips get a full gold frame.
    "tui.wcol_menu.outline": (BRONZE << 8) | 0xff,
    "tui.wcol_menu.outline_sel": (GOLD << 8) | 0xff,
    "tui.wcol_menu_back.outline": (BRONZE << 8) | 0xff,
    "tui.wcol_menu_back.outline_sel": (GOLD << 8) | 0xff,
    "tui.wcol_menu_back.text": 0xc9bfa9ff,
    "tui.wcol_tooltip.outline": (BRONZE << 8) | 0xff,
    "tui.wcol_tooltip.outline_sel": (BRONZE << 8) | 0xff,
    "tui.wcol_pie_menu.outline": (BRONZE << 8) | 0xff,
    "tui.wcol_pie_menu.outline_sel": (GOLD << 8) | 0xff,
    "tui.wcol_pie_menu.item": (GOLD << 8) | 0xff,
    # Tabs: active tab reads as a gold label on a lit navy plate.
    "tui.wcol_tab.outline": (BRONZE_DARK << 8) | 0xff,
    "tui.wcol_tab.outline_sel": (GOLD << 8) | 0xff,
    "tui.wcol_tab.inner_sel": 0x1a2c4cff,
    "tui.wcol_tab.text": 0x9a9384ff,
    "tui.wcol_tab.text_sel": (GOLD_BRIGHT << 8) | 0xff,
    # Progress bars burn like the forge core.
    "tui.wcol_progress.item": 0xe08a2aff,
    "tui.wcol_progress.inner_sel": 0xe08a2aff,
    # Menu and list highlights.
    "tui.wcol_menu_item.inner_sel": (BLUE << 8) | 0xcc,
    "tui.wcol_list_item.outline_sel": (GOLD << 8) | 0xff,
    # Panels: navy plates with a faint gold rim and gold titles.
    "tui.panel_header": 0x16243eff,
    "tui.panel_back": 0x101a2eff,
    "tui.panel_outline": (GOLD << 8) | 0x38,
    "tui.panel_title": (GOLD_BRIGHT << 8) | 0xff,
    "tui.panel_active": (BLUE << 8) | 0xff,
    # Editor borders: near black gaps, the active editor framed in gold.
    "tui.editor_border": 0x04070dff,
    "tui.editor_outline": (BRONZE << 8) | 0x40,
    "tui.editor_outline_active": (GOLD << 8) | 0xb0,
    "tui.widget_emboss": 0x00000040,
    "tui.transparent_checker_primary": 0x1d2a42ff,
    "tui.transparent_checker_secondary": 0x131d31ff,
    # Fire accents in the top bar and the status bar.
    "space_topbar.text_hi": (GOLD_BRIGHT << 8) | 0xff,
    "space_statusbar.text": 0x8f8a7cff,
    "space_properties.match": (GOLD << 8) | 0xff,
}

# Floats and ints that change.
SCALAR_OVERRIDES = {
    "tui.panel_roundness": "0.25f",
    "tui.menu_shadow_fac": "0.45f",
    "tui.menu_shadow_width": "10",
    "tui.icon_saturation": "0.65f",
}

# Top level sections whose neutral greys are UI surfaces.
UI_SECTIONS = {"tui", "regions", "common"}

# -----------------------------------------------------------------------------
# Color math.


def _lerp(a, b, t):
    return a + (b - a) * t


def _rgb(c):
    return (c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff


def _pack(r, g, b):
    return (int(round(r)) << 16) | (int(round(g)) << 8) | int(round(b))


def navy_from_grey(v):
    for (v0, c0), (v1, c1) in zip(NAVY_RAMP, NAVY_RAMP[1:]):
        if v0 <= v <= v1:
            t = 0.0 if v1 == v0 else (v - v0) / (v1 - v0)
            a, b = _rgb(c0), _rgb(c1)
            return _pack(*(_lerp(x, y, t) for x, y in zip(a, b)))
    return _pack(v, v, v)


def recolor(rgb):
    if rgb in ACCENTS:
        return ACCENTS[rgb]
    r, g, b = _rgb(rgb)
    if r == g == b:
        return navy_from_grey((r + g + b) / 3.0)
    return rgb


# -----------------------------------------------------------------------------
# File rewriting.

RE_OPEN = re.compile(r"^\s*(?:\.(\w+) = )?\{\s*$")
RE_CLOSE = re.compile(r"^\s*\},?\s*$")
RE_COLOR = re.compile(r"^(\s*\.(\w+) = )(RGBA?)\(0x([0-9a-f]+)\)(,.*)$")
RE_SCALAR = re.compile(r"^(\s*\.(\w+) = )([0-9.]+f?)(,.*)$")


def transform(text):
    out = []
    stack = []
    changed = 0
    in_theme = False
    for line in text.splitlines(keepends=True):
        stripped = line.rstrip("\n")
        if not in_theme:
            if stripped.startswith("const bTheme U_theme_default = {"):
                in_theme = True
            out.append(line)
            continue
        m = RE_OPEN.match(stripped)
        if m:
            stack.append(m.group(1) or "[]")
            out.append(line)
            continue
        if RE_CLOSE.match(stripped):
            if stack:
                stack.pop()
            out.append(line)
            continue
        if stripped.startswith("};"):
            in_theme = False
            out.append(line)
            continue

        m = RE_COLOR.match(stripped)
        if m:
            prefix, name, kind, hexval, suffix = m.groups()
            path = ".".join(stack + [name])
            section = stack[0] if stack else ""
            value = int(hexval, 16)
            if kind == "RGBA":
                rgb, alpha = value >> 8, value & 0xff
            else:
                rgb, alpha = value, None

            new_rgb, new_alpha = rgb, alpha
            if path in OVERRIDES:
                ov = OVERRIDES[path]
                if kind == "RGBA":
                    new_rgb, new_alpha = ov >> 8, ov & 0xff
                else:
                    new_rgb = ov >> 8
            elif section in UI_SECTIONS or section.startswith("space_"):
                new_rgb = recolor(rgb)

            if kind == "RGBA":
                new_hex = "%06x%02x" % (new_rgb, new_alpha)
            else:
                new_hex = "%06x" % new_rgb
            if new_hex != hexval:
                changed += 1
            out.append("%s%s(0x%s)%s\n" % (prefix, kind, new_hex, suffix))
            continue

        m = RE_SCALAR.match(stripped)
        if m:
            prefix, name, value, suffix = m.groups()
            path = ".".join(stack + [name])
            if path in SCALAR_OVERRIDES and SCALAR_OVERRIDES[path] != value:
                value = SCALAR_OVERRIDES[path]
                changed += 1
            out.append("%s%s%s\n" % (prefix, value, suffix))
            continue

        out.append(line)

    result = "".join(out)
    # Header note, so nobody regenerates the file without re-applying.
    note = (
        " * Generated by 'tools/utils/blender_theme_as_c.py'\n"
        " * then re-colored by 'tools/gameforge/apply_theme.py' (GameForge Forge theme).\n"
    )
    if "apply_theme.py" not in result:
        result = result.replace(" * Generated by 'tools/utils/blender_theme_as_c.py'\n", note, 1)
    return result, changed


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--check", action="store_true", help="Only check, do not write.")
    parser.add_argument("--path", default=THEME_PATH)
    args = parser.parse_args()

    with open(args.path, encoding="utf-8") as fh:
        text = fh.read()
    result, changed = transform(text)
    if args.check:
        if result != text:
            print("Theme is not up to date, run: python3 tools/gameforge/apply_theme.py")
            return 1
        return 0
    if result != text:
        with open(args.path, "w", encoding="utf-8") as fh:
            fh.write(result)
    print("%s: %d values re-colored" % (args.path, changed))
    return 0


if __name__ == "__main__":
    sys.exit(main())
