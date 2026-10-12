#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Build the release zip of the ``estudio-angelica-schemas`` plugin for atomgameforge.com.

The plugin is a zip with the ``.data`` schemas per kind (``elements/*.cfg`` today). The portal's
/admin › Versões publishes a STORED zip that carries ``pwops-release.json`` at its root and names
that inner zip as its ``payload``; the Studio then downloads the inner zip.

  python3 tools/utils/gameforge/pack_angelica_schemas.py \\
      --elements "D:/.../configs/PW/configs" --version 1.0.0 \\
      --changelog "Schemas do elements.data, PW 1.1.6 a 1.8.8" --output estudio-angelica-schemas-1.0.0.zip
"""

import argparse
import hashlib
import io
import json
import os
import re
import sys
import zipfile

PLUGIN_SLUG = "estudio-angelica-schemas"
PAYLOAD_NAME = "angelica-schemas.zip"
CHANGELOG_MAX = 600
VERSION_RE = re.compile(r"^[0-9A-Za-z.+-]{1,31}$")


def schema_files(folder, suffix):
    names = sorted(n for n in os.listdir(folder) if n.lower().endswith(suffix))
    if not names:
        raise SystemExit("nenhum {:s} em {:s}".format(suffix, folder))
    return [(n, os.path.join(folder, n)) for n in names]


def build_payload(kinds):
    buffer = io.BytesIO()
    index = {"schema": 1, "kinds": {}}
    with zipfile.ZipFile(buffer, "w", compression=zipfile.ZIP_DEFLATED) as zf:
        for kind, files in kinds.items():
            index["kinds"][kind] = [name for name, _path in files]
            for name, path in files:
                with open(path, "rb") as fh:
                    zf.writestr("{:s}/{:s}".format(kind, name), fh.read())
        zf.writestr("schemas.json", json.dumps(index, indent=2, ensure_ascii=False))
    return buffer.getvalue()


def build_release(payload, version, changelog):
    manifest = {
        "schema": 1,
        "plugin": PLUGIN_SLUG,
        "version": version,
        "changelog": changelog,
        "files": [{"path": PAYLOAD_NAME, "sha256": hashlib.sha256(payload).hexdigest(), "bytes": len(payload)}],
        "payload": PAYLOAD_NAME,
    }
    buffer = io.BytesIO()
    # The portal's zip reader only accepts STORED entries.
    with zipfile.ZipFile(buffer, "w", compression=zipfile.ZIP_STORED) as zf:
        zf.writestr("pwops-release.json", json.dumps(manifest, indent=2, ensure_ascii=False))
        zf.writestr(PAYLOAD_NAME, payload)
    return buffer.getvalue()


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--elements", required=True, help="folder with the elements.data .cfg schemas")
    parser.add_argument("--version", required=True)
    parser.add_argument("--changelog", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args(argv)
    if not VERSION_RE.match(args.version):
        raise SystemExit("versão inválida: {!r}".format(args.version))
    if not args.changelog.strip() or len(args.changelog) > CHANGELOG_MAX:
        raise SystemExit("changelog vazio ou com mais de {:d} caracteres".format(CHANGELOG_MAX))
    payload = build_payload({"elements": schema_files(args.elements, ".cfg")})
    with open(args.output, "wb") as fh:
        fh.write(build_release(payload, args.version, args.changelog.strip()))
    print("{:s}: {:s} {:s}, {:d} bytes".format(args.output, PLUGIN_SLUG, args.version, len(payload)))


if __name__ == "__main__":
    main(sys.argv[1:])
