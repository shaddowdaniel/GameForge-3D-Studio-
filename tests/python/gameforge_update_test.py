# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Tests for the Studio's auto-update (``gameforge_update``): the portal's signature, versions, the update package
(``pack_update.py``), applying it to an installation and the download. No portal, no network, no elevation.

Runs with plain Python, no Studio build needed:
  python3 tests/python/gameforge_update_test.py [TestClass]
"""

import base64
import hashlib
import http.server
import io
import json
import os
import shutil
import sys
import tempfile
import threading
import unittest
import urllib.parse
import zipfile

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "scripts", "modules"))
sys.path.insert(0, os.path.join(ROOT, "release", "windows", "gameforge"))

from gameforge_update import ed25519, package, signature, versions  # noqa: E402
import pack_update  # noqa: E402
import write_manifest  # noqa: E402

TEST_SEED = bytes(range(32))
TEST_PINS = {"key_test": ed25519.public_key(TEST_SEED)}
PLUGIN_ID = "11111111-2222-3333-4444-555555555555"


def signed_answer(version, file_hash, *, name=None, seed=TEST_SEED, kid="key_test", base=None):
    """A plugin-download answer as the portal gives it, signed the way admin-sign signs."""
    file_path = "{:s}/{:s}/{:s}".format(PLUGIN_ID, version, name or "gameforge-3d-studio-{:s}.zip".format(version))
    message = json.dumps({"plugin_id": PLUGIN_ID, "version": version, "file_path": file_path,
                          "file_hash": file_hash, "kid": kid}, separators=(",", ":")).encode("utf-8")
    url = ("https://x.supabase.co/storage/v1/object/sign/plugin-files/" + urllib.parse.quote(file_path) +
           "?token=abc")
    return {"version": version, "download_url": url, "file_hash": file_hash, "min_helper_version": base,
            "signature": base64.b64encode(ed25519.sign(seed, message)).decode("ascii")}


class Ed25519Test(unittest.TestCase):

    def test_rfc8032_vectors(self):
        for seed, public, message, sig in (
            ("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
             "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
             "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25b"
             "f5f0595bbe24655141438e7a100b"),
            ("4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
             "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
             "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2e"
             "aeb4302aeeb00d291612bb0c00"),
        ):
            seed, public, message, sig = (bytes.fromhex(v) for v in (seed, public, message, sig))
            self.assertEqual(ed25519.public_key(seed), public)
            self.assertEqual(ed25519.sign(seed, message), sig)
            self.assertTrue(ed25519.verify(public, message, sig))
            self.assertFalse(ed25519.verify(public, message + b"x", sig))

    def test_real_portal_signature(self):
        """The launcher 1.5.0 as the portal signed it (plugin_versions, 2026-10-10): the envelope format is exact."""
        envelope = {"plugin_id": "4ce45c4a-6ad9-495d-8c0e-649c386a973b", "version": "1.5.0",
                    "file_path": "4ce45c4a-6ad9-495d-8c0e-649c386a973b/1.5.0/AtomLauncher.exe",
                    "file_hash": "861638ff411e8f0b311b1230fea2a60f41ebba9eb256c753585718d4fc8bb749",
                    "signature": "r53TpITdEQjy+J6oV04hv/ffys/OVsBxQ5q2r4wDNB1mCwHVe36mzQpSHTeqF9gqhgW9xBHukWIUIpokFqx2CQ=="}
        self.assertTrue(signature.envelope_ok(envelope))
        self.assertFalse(signature.envelope_ok(dict(envelope, version="1.5.1")))


class VersionTest(unittest.TestCase):

    def test_ver_cmp(self):
        self.assertEqual(versions.ver_cmp("1.2.10", "1.2.9"), 1)
        self.assertEqual(versions.ver_cmp("5.4.0", "5.4"), 0)
        self.assertEqual(versions.ver_cmp("1.0.0-beta", "1.0.0"), -1)
        self.assertEqual(versions.ver_cmp("5.4.1", "5.4.2"), -1)

    def test_newer(self):
        listing = {"versions": [{"version": "5.4.2", "changelog": "b"}, {"version": "5.4.0", "changelog": "x"},
                                {"version": "5.4.1", "changelog": "a"}, {"version": "5.3.9"}, {"bad": 1}]}
        self.assertEqual([v["version"] for v in versions.newer(listing, "5.4.0")], ["5.4.1", "5.4.2"])
        self.assertEqual(versions.newer(listing, "5.4.2"), [])
        self.assertEqual(versions.newer({}, "5.4.0"), [])

    def test_installed(self):
        with tempfile.TemporaryDirectory() as folder:
            self.assertIsNone(versions.installed(folder))
            with open(os.path.join(folder, versions.VERSION_FILE), "w", encoding="utf-8") as fh:
                json.dump({"version": "5.4.1", "base_version": "5.4.0"}, fh)
            self.assertEqual(versions.installed(folder)["version"], "5.4.1")


class SignatureTest(unittest.TestCase):

    def test_envelope_from_download(self):
        answer = signed_answer("5.4.2", "ab" * 32, name="pacote ação.zip")
        envelope = signature.envelope_from_download(answer)
        self.assertEqual(envelope["file_path"], "{:s}/5.4.2/pacote ação.zip".format(PLUGIN_ID))
        self.assertEqual(envelope["plugin_id"], PLUGIN_ID)
        self.assertIsNone(signature.envelope_from_download(dict(answer, download_url="https://x/y.zip")))
        self.assertIsNone(signature.envelope_from_download(dict(answer, file_hash="xyz")))
        self.assertIsNone(signature.envelope_from_download(dict(answer, version='5.4"2')))

    def test_envelope_ok(self):
        envelope = signature.envelope_from_download(signed_answer("5.4.2", "ab" * 32))
        self.assertTrue(signature.envelope_ok(envelope, TEST_PINS))
        self.assertFalse(signature.envelope_ok(dict(envelope, version="5.4.3"), TEST_PINS))
        self.assertFalse(signature.envelope_ok(dict(envelope, signature="não é base64"), TEST_PINS))
        self.assertFalse(signature.envelope_ok({"signature": envelope["signature"]}, TEST_PINS))
        # The production keys never accept a test signature.
        self.assertFalse(signature.envelope_ok(envelope))


def make_build(root, version, files):
    """A fake installation: ``files`` = {relative path: bytes}, plus its manifest and version files."""
    for path, data in files.items():
        target = os.path.join(root, *path.split("/"))
        os.makedirs(os.path.dirname(target), exist_ok=True)
        with open(target, "wb") as fh:
            fh.write(data)
    write_manifest.write(root, version)
    return root


class ManifestTest(unittest.TestCase):

    def test_manifest_and_version(self):
        with tempfile.TemporaryDirectory() as root:
            make_build(root, "5.4.0", {"x.txt": b"x", "sub/y.bin": b"y", "sub/__pycache__/z.pyc": b"z"})
            with open(os.path.join(root, versions.MANIFEST_FILE), encoding="utf-8") as fh:
                files = json.load(fh)
            self.assertEqual(files, {"sub/y.bin": hashlib.sha256(b"y").hexdigest(),
                                     "x.txt": hashlib.sha256(b"x").hexdigest()})
            self.assertEqual(versions.installed(root), {"version": "5.4.0", "base_version": "5.4.0", "build": ""})
            # Running again does not list its own files.
            self.assertEqual(write_manifest.write(root, "5.4.0"), files)


class Builds:
    """Three builds of a fake Studio and the installer manifest of the first (5.4.0)."""

    def __init__(self, tmp):
        self.manifests = os.path.join(tmp, "manifests")
        self.out = os.path.join(tmp, "out")
        self.b0 = make_build(os.path.join(tmp, "b0"), "5.4.0", {
            "a.txt": b"a0", "b.txt": b"b0", "sub/c.dll": b"c0", "blender.pdb": b"p0"})
        self.b1 = make_build(os.path.join(tmp, "b1"), "5.4.1", {
            "a.txt": b"a1", "b.txt": b"b0", "sub/c.dll": b"c0", "sub/d.py": b"d1", "blender.pdb": b"p1"})
        # a.txt goes back to the installer's bytes; b.txt is gone.
        self.b2 = make_build(os.path.join(tmp, "b2"), "5.4.2", {
            "a.txt": b"a0", "sub/c.dll": b"c0", "sub/d.py": b"d1", "blender.pdb": b"p2"})
        pack_update.record_installer(self.b0, self.manifests)


def open_release(release):
    """(pwops-release.json, its zip entries, gameforge-update.json, names in the package, package bytes)."""
    with zipfile.ZipFile(release) as rz:
        manifest = json.loads(rz.read("pwops-release.json"))
        payload = rz.read(manifest["payload"])
        infos = rz.infolist()
    with zipfile.ZipFile(io.BytesIO(payload)) as zf:
        return manifest, infos, json.loads(zf.read(package.UPDATE_FILE)), sorted(zf.namelist()), payload


class PackTest(unittest.TestCase):

    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.builds = Builds(self.tmp)

    def test_cumulative_package(self):
        b = self.builds
        pack_update.pack(b.b1, "Primeira", b.manifests, b.out)
        release, _changed, _removed = pack_update.pack(b.b2, "Segunda", b.manifests, b.out)
        manifest, infos, update, names, payload = open_release(release)
        # a.txt is back to the installer's bytes but changed in 5.4.1: an installation on 5.4.1 still needs it.
        self.assertEqual(sorted(update["files"]), ["a.txt", "sub/d.py"])
        self.assertEqual(update["removed"], ["b.txt"])
        self.assertEqual((update["version"], update["base_version"]), ("5.4.2", "5.4.0"))
        self.assertNotIn("blender.pdb", update["manifest"])
        self.assertEqual(names, sorted(["a.txt", "sub/d.py", package.UPDATE_FILE]))
        # The zip for the /admin: STORED, the payload listed with its SHA-256, the base as min_helper_version.
        self.assertTrue(all(info.compress_type == zipfile.ZIP_STORED for info in infos))
        self.assertEqual((manifest["schema"], manifest["plugin"], manifest["version"]), (1, "gameforge-3d-studio", "5.4.2"))
        self.assertEqual(manifest["min_helper_version"], "5.4.0")
        self.assertEqual(manifest["files"], [{"path": manifest["payload"], "sha256": hashlib.sha256(payload).hexdigest(),
                                              "bytes": len(payload)}])
        self.assertTrue(os.path.isfile(os.path.join(b.manifests, "5.4.2.json")))

    def test_refusals(self):
        b = self.builds
        with self.assertRaises(SystemExit):
            pack_update.pack(b.b0, "Mesma versão", b.manifests, b.out)
        with self.assertRaises(SystemExit):
            pack_update.pack(b.b1, "", b.manifests, b.out)
        with self.assertRaises(SystemExit):
            pack_update.pack(b.b1, "x" * 601, b.manifests, b.out)
        with self.assertRaises(SystemExit):
            pack_update.pack(b.b1, "Grande demais", b.manifests, b.out, max_bytes=100)
        with self.assertRaises(SystemExit):
            pack_update.pack(b.b1, "Sem base", os.path.join(self.tmp, "vazio"), b.out)


if __name__ == "__main__":
    unittest.main()
