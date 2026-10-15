# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
The Studio's own updates from atomgameforge.com, through the same ``plugin-download`` endpoint and /admin
publishing as the PWOps launcher. The plugin ``gameforge-3d-studio`` carries cumulative update packages
(``release/windows/gameforge/pack_update.py``); the full installer is on GitHub Releases.

The Studio asks before anything (``bl_operators.gameforge_update``), downloads and checks the package here, and
``apply.py`` applies it once the Studio closes, with the permission of Windows (the installation is per machine).
"""

__all__ = (
    "INSTALLER_PAGE",
    "NeedsInstaller",
    "SLUG",
    "UpdateError",
    "download",
    "last_result",
    "staging_dir",
    "start_install",
)

import json
import os
import subprocess
import sys

from . import signature
from .package import UpdateError
from .versions import ver_cmp

SLUG = "gameforge-3d-studio"
INSTALLER_PAGE = "https://atomgameforge.com/conta"
MAX_PACKAGE_BYTES = 50 * 1024 * 1024  # The portal's Storage limit (Supabase Free).
APPLY_SCRIPT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "apply.py")
LOG_NAME = "update.log"


class NeedsInstaller(Exception):
    """The published update starts from a version newer than the installed one: the full installer is needed."""


def staging_dir():
    base = os.environ.get("LOCALAPPDATA") or os.path.expanduser("~")
    path = os.path.join(base, "ATOM GameForge", "GameForge 3D Studio", "updates")
    os.makedirs(path, exist_ok=True)
    return path


def download(portal, session_token, installed_version, progress=None, pins=None):
    """Downloads the current update and checks it: ``(package path, envelope path, version)``. The signature is
    checked before downloading and the SHA-256 after, like the launcher does."""
    answer = portal.plugin_download(SLUG, session_token)
    base = str(answer.get("min_helper_version") or "")
    if base and ver_cmp(base, installed_version) > 0:
        raise NeedsInstaller(base)
    envelope = signature.envelope_from_download(answer)
    if envelope is None or not signature.envelope_ok(envelope, pins or signature.PINS):
        raise UpdateError("assinatura inválida: a atualização foi recusada")
    if not envelope["file_path"].lower().endswith(".zip"):
        raise UpdateError("o portal ofereceu um arquivo que não é um pacote de atualização")
    if ver_cmp(envelope["version"], installed_version) <= 0:
        raise UpdateError("a versão {:s} já está instalada".format(installed_version))
    folder = staging_dir()
    name = "{:s}-{:s}".format(SLUG, envelope["version"])
    package_path = os.path.join(folder, name + ".zip")
    part = package_path + ".part"
    try:
        digest = portal.fetch_to_file(answer["download_url"], part, max_bytes=MAX_PACKAGE_BYTES, progress=progress)
        if digest != envelope["file_hash"]:
            raise UpdateError("o arquivo baixado não confere com a assinatura")
        os.replace(part, package_path)
    finally:
        if os.path.exists(part):
            os.remove(part)
    envelope_path = os.path.join(folder, name + ".json")
    with open(envelope_path, "w", encoding="utf-8") as fh:
        json.dump(envelope, fh, ensure_ascii=False)
    return package_path, envelope_path, envelope["version"]


def start_install(install_dir, package_path, envelope_path, python=sys.executable):
    """Starts the applier's "watch" role, detached: it waits for the Studio to close. Returns the log path."""
    log = os.path.join(staging_dir(), LOG_NAME)
    subprocess.Popen([python, "-I", APPLY_SCRIPT, "watch", install_dir, package_path, envelope_path, log],
                     creationflags=0x00000008 | 0x00000200,  # DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP
                     close_fds=True)
    return log


def last_result():
    """The last applier outcome (a "RESULTADO ..." line without the prefix), once: the log is set aside after reading."""
    path = os.path.join(staging_dir(), LOG_NAME)
    try:
        with open(path, encoding="utf-8") as fh:
            lines = [line.split("RESULTADO ", 1)[1].strip() for line in fh if "RESULTADO " in line]
        os.replace(path, path + ".lido")
    except OSError:
        return ""
    return lines[-1] if lines else ""
