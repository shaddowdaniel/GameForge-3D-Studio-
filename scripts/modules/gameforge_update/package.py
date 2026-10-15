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
    "apply_update",
    "file_sha256",
    "read_update",
)

import hashlib
import json
import os
import shutil
import zipfile

from .versions import MANIFEST_FILE, VERSION_FILE, installed, ver_cmp

UPDATE_FILE = "gameforge-update.json"
_OLD_SUFFIX = ".gfold"
_WORK_DIR = ".gameforge-update"


class UpdateError(Exception):
    pass


def file_sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _safe(path):
    """A relative path that stays inside the installation (the rule of the portal's `relOk`)."""
    return (isinstance(path, str) and bool(path) and all(part not in {"", ".", ".."} for part in path.split("/"))
            and not any(c in '\\:"*?<>|' or ord(c) < 0x20 for c in path))


def read_update(zip_path):
    """The package's ``gameforge-update.json``, checked against the zip's contents."""
    try:
        with zipfile.ZipFile(zip_path) as zf:
            info = json.loads(zf.read(UPDATE_FILE).decode("utf-8"))
            names = set(zf.namelist()) - {UPDATE_FILE}
    except (OSError, KeyError, ValueError, zipfile.BadZipFile) as ex:
        raise UpdateError("pacote de atualização ilegível: {:s}".format(str(ex))) from ex
    if not isinstance(info, dict) or info.get("schema") != 1 or not info.get("version") or not info.get("base_version"):
        raise UpdateError("pacote de atualização de formato desconhecido")
    files, removed, manifest = info.get("files") or {}, info.get("removed") or [], info.get("manifest") or {}
    for path in [*files, *removed, *manifest]:
        if not _safe(path):
            raise UpdateError("caminho inseguro no pacote: {!r}".format(path))
    if set(files) != names or any(manifest.get(path) != digest for path, digest in files.items()):
        raise UpdateError("os arquivos do pacote não batem com a lista dele")
    info["files"], info["removed"], info["manifest"] = files, removed, manifest
    return info


def _write_json(path, data):
    temp = path + ".tmp"
    with open(temp, "w", encoding="utf-8") as fh:
        json.dump(data, fh, ensure_ascii=False, indent=0, sort_keys=True)
    os.replace(temp, path)


def _delete_soon(path):
    """Deletes now or, when the file is in use (the applier's own python.exe), when Windows restarts."""
    try:
        os.remove(path)
    except OSError:
        import ctypes
        ctypes.windll.kernel32.MoveFileExW(path, None, 0x4)  # MOVEFILE_DELAY_UNTIL_REBOOT


def _forget_old_files(install_dir):
    """The `.gfold` files an earlier update could not delete (they were in use)."""
    for folder, _dirs, names in os.walk(install_dir):
        for name in names:
            if name.endswith(_OLD_SUFFIX):
                try:
                    os.remove(os.path.join(folder, name))
                except OSError:
                    pass


def apply_update(install_dir, zip_path, log=print):
    """Applies a package to an installation and returns the new version. On any error every file goes back as it
    was and the error is raised again. Checking the package's signature is the caller's job (``apply.py``)."""
    current = installed(install_dir)
    if current is None:
        raise UpdateError("instalação sem {:s}".format(VERSION_FILE))
    info = read_update(zip_path)
    if ver_cmp(info["version"], current["version"]) <= 0:
        raise UpdateError("a versão {:s} já está instalada".format(current["version"]))
    if ver_cmp(info["base_version"], current["version"]) > 0:
        raise UpdateError("a atualização {:s} parte da {:s}, mais nova que a instalada ({:s}): use o instalador "
                          "completo".format(info["version"], info["base_version"], current["version"]))
    try:
        with open(os.path.join(install_dir, MANIFEST_FILE), encoding="utf-8") as fh:
            old = json.load(fh)
    except (OSError, ValueError) as ex:
        raise UpdateError("instalação sem {:s}".format(MANIFEST_FILE)) from ex
    files, manifest = info["files"], info["manifest"]
    changed = sorted(path for path, digest in manifest.items() if old.get(path) != digest)
    missing = [path for path in changed if path not in files]
    if missing:
        raise UpdateError("o pacote não traz {:d} arquivo(s) que mudaram, ex.: {:s}".format(len(missing), missing[0]))
    removed = sorted(path for path in info["removed"] if path not in manifest)
    _forget_old_files(install_dir)

    work = os.path.join(install_dir, _WORK_DIR)
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    done = []  # (target, kind) in order; kind: "replaced", "added" or "removed".
    try:
        # 1. The new files next to the installation (same disk, so placing them is a rename), each one checked.
        with zipfile.ZipFile(zip_path) as zf:
            for i, path in enumerate(changed):
                staged = os.path.join(work, str(i))
                with zf.open(path) as src, open(staged, "wb") as dst:
                    shutil.copyfileobj(src, dst, 1 << 20)
                if file_sha256(staged) != manifest[path]:
                    raise UpdateError("arquivo corrompido no pacote: {:s}".format(path))
        # 2. Each old file becomes `<name>.gfold` (Windows lets a program in use be renamed); the new one takes
        #    its place.
        for i, path in enumerate(changed):
            target = os.path.join(install_dir, *path.split("/"))
            if os.path.exists(target):
                os.replace(target, target + _OLD_SUFFIX)
                done.append((target, "replaced"))
            else:
                os.makedirs(os.path.dirname(target), exist_ok=True)
                done.append((target, "added"))
            os.replace(os.path.join(work, str(i)), target)
        for path in removed:
            target = os.path.join(install_dir, *path.split("/"))
            if os.path.exists(target):
                os.replace(target, target + _OLD_SUFFIX)
                done.append((target, "removed"))
        # 3. The bookkeeping. Files the update does not know (debug symbols) stay listed as they are.
        new_manifest = dict(manifest)
        for path, digest in old.items():
            if path not in new_manifest and path not in removed:
                new_manifest[path] = digest
        _write_json(os.path.join(install_dir, MANIFEST_FILE), new_manifest)
        _write_json(os.path.join(install_dir, VERSION_FILE), {
            "version": info["version"], "base_version": info["base_version"], "build": info.get("build", "")})
    except BaseException:
        for target, kind in reversed(done):
            try:
                if kind in {"replaced", "added"} and os.path.exists(target):
                    os.remove(target)
                if kind in {"replaced", "removed"}:
                    os.replace(target + _OLD_SUFFIX, target)
            except OSError as ex:
                log("não deu para devolver {:s}: {:s}".format(target, str(ex)))
        _write_json(os.path.join(install_dir, MANIFEST_FILE), old)
        _write_json(os.path.join(install_dir, VERSION_FILE), current)
        raise
    finally:
        shutil.rmtree(work, ignore_errors=True)
    for target, kind in done:
        if kind != "added":
            _delete_soon(target + _OLD_SUFFIX)
    log("{:d} arquivo(s) trocado(s), {:d} removido(s)".format(len(changed), len(removed)))
    return info["version"]
