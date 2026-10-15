# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Builds a GameForge 3D Studio update for the portal (atomgameforge.com /admin > Versões).

    python pack_update.py <build dir> --changelog "texto" --manifests <folder> --out <folder>
        The update from the last installer up to the build: <out>/gameforge-3d-studio-<v>-release.zip (the zip to
        upload) and <manifests>/<v>.json.

    python pack_update.py <build dir> --record-installer --manifests <folder>
        After publishing a full installer (MSI) of the build: its manifest becomes the base of the next updates.

The update is cumulative: it carries every file that differs between the build and the installer or any update
published since, so one package takes any of those installations to the new version. <build dir> is an
installation with gameforge-manifest.json (gf-build\\bin, or the folder CPack installed into).
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
from gameforge_update.versions import MANIFEST_FILE, VERSION_FILE, ver_cmp  # noqa: E402

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


def load_build(build_dir):
    with open(os.path.join(build_dir, VERSION_FILE), encoding="utf-8") as fh:
        version = json.load(fh)["version"]
    with open(os.path.join(build_dir, MANIFEST_FILE), encoding="utf-8") as fh:
        files = {path: digest for path, digest in json.load(fh).items() if not _skip(path)}
    return version, files


def load_manifests(folder):
    found = []
    if os.path.isdir(folder):
        for name in os.listdir(folder):
            if name.endswith(".json"):
                with open(os.path.join(folder, name), encoding="utf-8") as fh:
                    found.append(json.load(fh))
    found.sort(key=functools.cmp_to_key(lambda a, b: ver_cmp(a["version"], b["version"])))
    return found


def record_installer(build_dir, manifests):
    version, files = load_build(build_dir)
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
    parser.add_argument("build", help="a instalação do build, com gameforge-manifest.json")
    parser.add_argument("--manifests", required=True, help="a pasta dos manifestos publicados")
    parser.add_argument("--record-installer", action="store_true", help="registra o build como base (depois do MSI)")
    parser.add_argument("--changelog", default="", help="o texto do changelog (até 600 caracteres)")
    parser.add_argument("--out", default=".", help="onde gravar o zip para o /admin")
    args = parser.parse_args(argv)
    if args.record_installer:
        print("base registrada:", record_installer(args.build, args.manifests))
        return
    release, changed, removed = pack(args.build, args.changelog, args.manifests, args.out)
    print("{:s}: {:d} arquivo(s) no pacote, {:d} removido(s)".format(release, len(changed), len(removed)))


if __name__ == "__main__":
    main()
