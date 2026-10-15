# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
The portal's release signature (``admin-sign`` at atomgameforge.com): Ed25519 over the UTF-8 bytes of
``JSON.stringify({plugin_id, version, file_path, file_hash, kid})``, base64. The same check as the launcher
(``PWOps/155dll/launcher/updater.hpp``, ``upd__sig_ok``): ``plugin-download`` answers neither ``file_path`` nor
``kid``, so the path comes from the signed Storage URL, the plugin id is its first segment and each pinned key is
tried with its own kid.
"""

__all__ = (
    "PINS",
    "envelope_from_download",
    "envelope_ok",
)

import base64
import binascii
import json
import re
import urllib.parse

from . import ed25519

# The portal's signing keys (`signing_keys`; the launcher's and the DLL's `g_joiaPins`). A new key is pinned by an
# update signed with a key that is already here, before the portal starts signing with it.
PINS = {
    "key_ec46b5ca": bytes.fromhex("dddcf28b09b79297058bbfffa5de5584c0a72c25f28d59aa1dde919e25dcafe4"),
    "key_7717f668": bytes.fromhex("1aeba158eaba9092aa3505ef30979ec86d03d5e944108e3d76d19d93ae8f7a5a"),
}
_MARK = "/object/sign/plugin-files/"
_HEX64 = re.compile(r"[0-9a-f]{64}")


def envelope_from_download(answer):
    """The signed fields of a ``plugin-download`` answer (plus the signature), or ``None`` when they cannot be
    the /admin's."""
    url = str(answer.get("download_url") or "")
    at = url.find(_MARK)
    if at < 0:
        return None
    try:
        path = urllib.parse.unquote(url[at + len(_MARK):].split("?", 1)[0].split("#", 1)[0], errors="strict")
    except UnicodeDecodeError:
        return None
    version = str(answer.get("version") or "")
    file_hash = str(answer.get("file_hash") or "")
    plugin_id = path.split("/", 1)[0]
    if ("/" not in path or not 0 < len(plugin_id) <= 64 or not version or not _HEX64.fullmatch(file_hash)
            or any(ord(c) < 0x20 or c in '"\\' for c in path + version)):
        return None
    return {"plugin_id": plugin_id, "version": version, "file_path": path, "file_hash": file_hash,
            "signature": str(answer.get("signature") or "")}


def envelope_ok(envelope, pins=PINS):
    """True when one of the pinned keys signed the envelope."""
    try:
        sig = base64.b64decode(envelope["signature"], validate=True)
        fields = {key: envelope[key] for key in ("plugin_id", "version", "file_path", "file_hash")}
    except (KeyError, TypeError, ValueError, binascii.Error):
        return False
    for kid, key in pins.items():
        message = json.dumps(dict(fields, kid=kid), separators=(",", ":"), ensure_ascii=False).encode("utf-8")
        if ed25519.verify(key, message, sig):
            return True
    return False
