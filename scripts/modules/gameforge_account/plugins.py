# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Studio plugins published on atomgameforge.com, kept in a local cache.

A plugin is a zip published in the portal's /admin (``plugin-download``, gated by the user's plan).
The Studio downloads the current version once, checks it against the SHA-256 the portal signed,
extracts it to ``<cache>/<slug>/<version>/`` and points ``<cache>/<slug>/current.txt`` at it. Older
versions stay until a newer one is installed, so the Studio keeps working offline with the last
version it got.

The first plugin is ``SCHEMAS_PLUGIN_SLUG``: the field layouts (``.cfg``) of the game's ``.data``
files per client version, read by the native Angelica importers and data editors from
``<cache>/estudio-angelica-schemas/<version>/elements/``.

Pure Python standard library, no ``bpy``.
"""

__all__ = (
    "SCHEMAS_PLUGIN_SLUG",
    "PluginCache",
    "installed_dir",
)

import hashlib
import io
import os
import re
import shutil
import zipfile

from .portal import PortalError

SCHEMAS_PLUGIN_SLUG = "estudio-angelica-schemas"

# A plugin zip is data, not code: only these kinds of files are extracted.
_ALLOWED_SUFFIXES = (".cfg", ".txt", ".json", ".md", ".csv", ".ini", ".xml")
_MAX_DOWNLOAD_BYTES = 256 * 1024 * 1024
_MAX_EXTRACTED_BYTES = 512 * 1024 * 1024
_VERSION_RE = re.compile(r"^[0-9A-Za-z.+-]{1,47}$")
_CURRENT_FILE = "current.txt"


def _safe_member(name):
    """Relative path inside the zip, or None when it is unsafe (absolute, ``..``, drive, ...)."""
    name = name.replace("\\", "/")
    if not name or name.startswith("/") or name.endswith("/"):
        return None
    parts = name.split("/")
    if any(part in {"", ".", ".."} or ":" in part for part in parts):
        return None
    return "/".join(parts)


def installed_dir(cache_dir, slug):
    """Folder of the installed version of ``slug``, or None when nothing is installed yet."""
    try:
        with open(os.path.join(cache_dir, slug, _CURRENT_FILE), "r", encoding="utf-8") as fh:
            version = fh.read().strip()
    except OSError:
        return None
    if not _VERSION_RE.match(version):
        return None
    path = os.path.join(cache_dir, slug, version)
    return path if os.path.isdir(path) else None


class PluginCache:

    def __init__(self, portal, cache_dir):
        self._portal = portal
        self.cache_dir = cache_dir

    def installed_version(self, slug):
        path = installed_dir(self.cache_dir, slug)
        return os.path.basename(path) if path else None

    def sync(self, slug, session_token):
        """
        Make sure the portal's current version of ``slug`` is installed.
        Returns ``(path, version, updated)``; raises ``PortalError`` when it cannot.
        """
        info = self._portal.plugin_download(slug, session_token)
        version = str(info["version"])
        if not _VERSION_RE.match(version):
            raise PortalError("versão inválida do plugin: {!r}".format(version))
        if self.installed_version(slug) == version:
            return installed_dir(self.cache_dir, slug), version, False

        data = self._portal.fetch(info["download_url"], max_bytes=_MAX_DOWNLOAD_BYTES)
        digest = hashlib.sha256(data).hexdigest()
        if digest != str(info["file_hash"]).lower():
            raise PortalError("o arquivo baixado de {:s} não confere com o publicado".format(slug))

        path = self._install(slug, version, data)
        return path, version, True

    def _install(self, slug, version, data):
        plugin_dir = os.path.join(self.cache_dir, slug)
        final_dir = os.path.join(plugin_dir, version)
        temp_dir = final_dir + ".partial"
        shutil.rmtree(temp_dir, ignore_errors=True)
        os.makedirs(temp_dir)
        try:
            self._extract(data, temp_dir)
            shutil.rmtree(final_dir, ignore_errors=True)
            os.replace(temp_dir, final_dir)
        except BaseException:
            shutil.rmtree(temp_dir, ignore_errors=True)
            raise

        current = os.path.join(plugin_dir, _CURRENT_FILE)
        with open(current + ".tmp", "w", encoding="utf-8") as fh:
            fh.write(version)
        os.replace(current + ".tmp", current)

        # Only the installed version is kept.
        for entry in os.listdir(plugin_dir):
            entry_path = os.path.join(plugin_dir, entry)
            if entry != version and os.path.isdir(entry_path):
                shutil.rmtree(entry_path, ignore_errors=True)
        return final_dir

    @staticmethod
    def _extract(data, target_dir):
        try:
            archive = zipfile.ZipFile(io.BytesIO(data))
        except zipfile.BadZipFile as ex:
            raise PortalError("o plugin baixado não é um zip válido") from ex
        total = 0
        with archive:
            for member in archive.infolist():
                if member.is_dir():
                    continue
                name = _safe_member(member.filename)
                if name is None:
                    raise PortalError("caminho inseguro no plugin: {:s}".format(member.filename))
                if not name.lower().endswith(_ALLOWED_SUFFIXES):
                    continue
                total += member.file_size
                if total > _MAX_EXTRACTED_BYTES:
                    raise PortalError("o plugin é maior que o permitido")
                out_path = os.path.join(target_dir, *name.split("/"))
                os.makedirs(os.path.dirname(out_path), exist_ok=True)
                with archive.open(member) as src, open(out_path, "wb") as dst:
                    shutil.copyfileobj(src, dst)
