# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Writes, at the root of an installation of GameForge 3D Studio:

- ``gameforge-manifest.json``: ``{relative path: sha256}`` of every installed file;
- ``gameforge-version.json``: ``{"version", "base_version", "build"}``. An installation from the installer is
  its own base; the auto-update rewrites both files.

Run by the last install rule (``source/creator/CMakeLists.txt``) with the installed Python, so the installer
(CPack installs into its own folder first), ``gf-build\\bin`` and a development deploy all carry them::

    python -I write_manifest.py <install dir> <version> [<source dir, for the git hash>]
"""

import hashlib
import json
import os
import subprocess
import sys

MANIFEST_FILE = "gameforge-manifest.json"
VERSION_FILE = "gameforge-version.json"


def file_sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def manifest(root):
    files = {}
    for folder, dirs, names in os.walk(root):
        dirs[:] = [d for d in dirs if d != "__pycache__"]
        for name in names:
            path = os.path.join(folder, name)
            relative = os.path.relpath(path, root).replace(os.sep, "/")
            if relative not in {MANIFEST_FILE, VERSION_FILE}:
                files[relative] = file_sha256(path)
    return files


def _write_json(path, data):
    with open(path, "w", encoding="utf-8") as fh:
        json.dump(data, fh, ensure_ascii=False, indent=0, sort_keys=True)


def git_hash(source_dir):
    try:
        done = subprocess.run(["git", "-C", source_dir, "rev-parse", "--short=12", "HEAD"],
                              capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.TimeoutExpired):
        return ""
    return done.stdout.strip() if done.returncode == 0 else ""


def write(root, version, build=""):
    files = manifest(root)
    _write_json(os.path.join(root, MANIFEST_FILE), files)
    _write_json(os.path.join(root, VERSION_FILE), {"version": version, "base_version": version, "build": build})
    return files


if __name__ == "__main__":
    write(sys.argv[1], sys.argv[2], git_hash(sys.argv[3]) if len(sys.argv) > 3 else "")
