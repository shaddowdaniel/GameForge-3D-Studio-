# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Builds a GameForge 3D Studio update for the portal (atomgameforge.com /admin > Versões).

    python pack_update.py <build dir> --changelog "texto" --manifests <folder> --out <folder>
        The update from the last installer up to the build: <out>/gameforge-3d-studio-<v>-release.zip (the zip to
        upload) and <manifests>/<v>.json.

    python pack_update.py <component folder>... --record-installer --manifests <folder>
        After publishing a full installer (MSI): the files in the folders CPack installed its components into
        (C:\\gfpk\\_CPack_Packages\\Windows\\WIX\\<package>\\Blender, Libraries and Unspecified) become the base of
        the next updates.

The update is cumulative: it carries every file that differs between the build and the installer or any update
published since, so one package takes any of those installations to the new version. <build dir> is a clean
installation of the build, never gf-build\\bin (it keeps build tools and files of older versions):
    cmake --install gf-build --config Release --prefix <empty folder>
The files themselves are hashed; the gameforge-manifest.json in a folder is not trusted.
"""

import argparse
import functools
import hashlib
import io
import json
import os
import re
import sys
import zipfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "scripts", "modules"))

from gameforge_update.package import UPDATE_FILE  # noqa: E402
from gameforge_update.versions import VERSION_FILE, ver_cmp  # noqa: E402

import write_manifest  # noqa: E402  (next to this script)

PLUGIN = "gameforge-3d-studio"
MAX_BYTES = 45 * 1024 * 1024  # The portal stores files up to 50 MB (Supabase Free).
CHANGELOG_MAX = 600           # release-zip.ts (CHANGELOG_MAX).
_VERSION_RE = re.compile(r"[0-9A-Za-z.+-]{1,31}")


def _skip(path):
    # Debug symbols: big, rebuilt every time and only read by crash reports. Updates leave them as they are.
    return path.lower().endswith(".pdb")


def _write_json(path, data):
    with open(path, "w", encoding="utf-8") as fh:
        json.dump(data, fh, ensure_ascii=False, indent=0, sort_keys=True)


def _files(folder):
    """The installed files of a folder, {path: sha256}, without debug symbols."""
    return {path: digest for path, digest in write_manifest.manifest(folder).items() if not _skip(path)}


def _version(folders):
    for folder in folders:
        path = os.path.join(folder, VERSION_FILE)
        if os.path.isfile(path):
            with open(path, encoding="utf-8") as fh:
                return json.load(fh)["version"]
    raise SystemExit("sem {:s} em {:s}".format(VERSION_FILE, ", ".join(folders)))


def load_build(build_dir):
    return _version([build_dir]), _files(build_dir)


def load_manifests(folder):
    found = []
    if os.path.isdir(folder):
        for name in os.listdir(folder):
            if name.endswith(".json"):
                with open(os.path.join(folder, name), encoding="utf-8") as fh:
                    found.append(json.load(fh))
    found.sort(key=functools.cmp_to_key(lambda a, b: ver_cmp(a["version"], b["version"])))
    return found


def record_installer(folders, manifests):
    """The base of the next updates: the files of every folder (the MSI's components) together."""
    folders = [folders] if isinstance(folders, str) else list(folders)
    version = _version(folders)
    files = {}
    for folder in folders:
        files.update(_files(folder))
    os.makedirs(manifests, exist_ok=True)
    _write_json(os.path.join(manifests, version + ".json"), {"version": version, "base_version": version, "files": files})
    return version


def pack(build_dir, changelog, manifests, out, max_bytes=MAX_BYTES):
    version, files = load_build(build_dir)
    published = load_manifests(manifests)
    if not published:
        raise SystemExit("sem o manifesto do instalador: rode --record-installer depois de publicar o MSI")
    latest = published[-1]
    if not _VERSION_RE.fullmatch(version) or ver_cmp(version, latest["version"]) <= 0:
        raise SystemExit("a versão {:s} do build não é maior que a {:s} já publicada".format(version, latest["version"]))
    changelog = changelog.strip()
    if not changelog or len(changelog) > CHANGELOG_MAX:
        raise SystemExit("changelog vazio ou com mais de {:d} caracteres".format(CHANGELOG_MAX))
    base = latest["base_version"]
    since_base = [m["files"] for m in published if m["base_version"] == base]
    changed = sorted(path for path, digest in files.items() if any(old.get(path) != digest for old in since_base))
    removed = sorted({path for old in since_base for path in old} - set(files))
    update = {"schema": 1, "version": version, "base_version": base,
              "files": {path: files[path] for path in changed}, "removed": removed, "manifest": files}

    name = "{:s}-{:s}.zip".format(PLUGIN, version)
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w", compression=zipfile.ZIP_LZMA) as zf:
        zf.writestr(UPDATE_FILE, json.dumps(update, ensure_ascii=False, indent=0, sort_keys=True))
        for path in changed:
            zf.write(os.path.join(build_dir, *path.split("/")), path)
    data = buffer.getvalue()
    if len(data) > max_bytes:
        raise SystemExit("o pacote tem {:.1f} MB (máximo {:.0f}): publique um instalador completo (MSI)".format(
            len(data) / 2 ** 20, max_bytes / 2 ** 20))

    # The zip for the /admin (release-zip.ts): STORED, the manifest at the root, the package as the payload.
    release = {"schema": 1, "plugin": PLUGIN, "version": version, "changelog": changelog,
               "files": [{"path": name, "sha256": hashlib.sha256(data).hexdigest(), "bytes": len(data)}],
               "payload": name, "min_helper_version": base}
    os.makedirs(out, exist_ok=True)
    release_path = os.path.join(out, "{:s}-{:s}-release.zip".format(PLUGIN, version))
    with zipfile.ZipFile(release_path, "w", compression=zipfile.ZIP_STORED) as zf:
        zf.writestr("pwops-release.json", json.dumps(release, ensure_ascii=False, indent=1))
        zf.writestr(name, data)
    _write_json(os.path.join(manifests, version + ".json"), {"version": version, "base_version": base, "files": files})
    return release_path, changed, removed


def main(argv=None):
    parser = argparse.ArgumentParser(description="Atualização do GameForge 3D Studio para o portal.")
    parser.add_argument("build", nargs="+",
                        help="a instalação limpa do build (ou, com --record-installer, as pastas dos componentes do MSI)")
    parser.add_argument("--manifests", required=True, help="a pasta dos manifestos publicados")
    parser.add_argument("--record-installer", action="store_true", help="registra o build como base (depois do MSI)")
    parser.add_argument("--changelog", default="", help="o texto do changelog (até 600 caracteres)")
    parser.add_argument("--out", default=".", help="onde gravar o zip para o /admin")
    args = parser.parse_args(argv)
    if args.record_installer:
        print("base registrada:", record_installer(args.build, args.manifests))
        return
    if len(args.build) != 1:
        raise SystemExit("o pacote parte de uma pasta só: a instalação limpa do build")
    release, changed, removed = pack(args.build[0], args.changelog, args.manifests, args.out)
    print("{:s}: {:d} arquivo(s) no pacote, {:d} removido(s)".format(release, len(changed), len(removed)))


if __name__ == "__main__":
    main()
