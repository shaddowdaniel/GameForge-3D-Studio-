# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Versions of the Studio: the order the launcher, the DLL and the portal use, and what is installed."""

__all__ = (
    "MANIFEST_FILE",
    "VERSION_FILE",
    "installed",
    "newer",
    "ver_cmp",
)

import functools
import json
import os
import re

VERSION_FILE = "gameforge-version.json"
MANIFEST_FILE = "gameforge-manifest.json"
_VERSION = re.compile(r"(?:(\d+)(?:\.(\d+))?(?:\.(\d+))?(?:\.(\d+))?)?(.*)", re.S)


def ver_cmp(a, b):
    """-1, 0 or 1. Up to four numbers, one by one (missing = 0); with equal numbers the suffix decides as in
    semver (1.0.0 > 1.0.0-beta). The rule of the launcher (`ver_cmp`), the DLL and the portal (`verCmp`)."""
    x, y = _VERSION.fullmatch(a).groups(), _VERSION.fullmatch(b).groups()
    for i in range(4):
        difference = int(x[i] or 0) - int(y[i] or 0)
        if difference:
            return 1 if difference > 0 else -1
    if not x[4] or not y[4]:
        return -1 if x[4] else (1 if y[4] else 0)
    return (x[4] > y[4]) - (x[4] < y[4])


def installed(install_dir):
    """``{"version", "base_version", "build"}`` of an installation; ``None`` without the file (a development build)."""
    try:
        with open(os.path.join(install_dir, VERSION_FILE), encoding="utf-8") as fh:
            data = json.load(fh)
    except (OSError, ValueError):
        return None
    return data if isinstance(data, dict) and data.get("version") else None


def newer(listing, version):
    """The versions of a ``plugin-download`` list answer above ``version``, oldest first."""
    found = [item for item in listing.get("versions") or []
             if isinstance(item, dict) and item.get("version") and ver_cmp(str(item["version"]), version) > 0]
    found.sort(key=functools.cmp_to_key(lambda a, b: ver_cmp(str(a["version"]), str(b["version"]))))
    return found
