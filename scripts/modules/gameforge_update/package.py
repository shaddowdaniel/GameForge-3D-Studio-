# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
An update package of the Studio and how it is applied.

The package is a zip (LZMA) with ``gameforge-update.json`` and the files that changed since the installed base,
built by ``release/windows/gameforge/pack_update.py``::

    {"schema": 1, "version": "5.4.2", "base_version": "5.4.0",
     "files": {path: sha256},     # the files in the zip
     "removed": [path],           # files of earlier versions that the new one no longer has
     "manifest": {path: sha256}}  # every file of the new version

Paths are relative to the installation, with ``/``.
"""

__all__ = (
    "UPDATE_FILE",
    "UpdateError",
)

UPDATE_FILE = "gameforge-update.json"


class UpdateError(Exception):
    pass
