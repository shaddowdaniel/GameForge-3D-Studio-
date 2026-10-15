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
import urllib.request
import zipfile

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "scripts", "modules"))
sys.path.insert(0, os.path.join(ROOT, "release", "windows", "gameforge"))

from gameforge_update import ed25519, package, signature, versions  # noqa: E402
from gameforge_update import apply as applier  # noqa: E402
import gameforge_update  # noqa: E402
from gameforge_account.portal import NetworkError, Portal, PortalError  # noqa: E402
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


def read(root, path):
    with open(os.path.join(root, *path.split("/")), "rb") as fh:
        return fh.read()


def snapshot(root):
    """Every file of a folder: {relative path: bytes}."""
    found = {}
    for folder, _dirs, names in os.walk(root):
        for name in names:
            path = os.path.join(folder, name)
            with open(path, "rb") as fh:
                found[os.path.relpath(path, root)] = fh.read()
    return found


class ApplyTest(unittest.TestCase):

    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp, True)
        b = self.builds = Builds(self.tmp)
        pack_update.pack(b.b1, "Primeira", b.manifests, b.out)
        release, _changed, _removed = pack_update.pack(b.b2, "Segunda", b.manifests, b.out)
        self.p2 = os.path.join(self.tmp, "p2.zip")
        with open(self.p2, "wb") as fh:
            fh.write(open_release(release)[4])

    def install(self, build):
        target = os.path.join(self.tmp, "install-" + os.path.basename(build))
        shutil.copytree(build, target)
        return target

    def test_from_installer_and_from_update(self):
        for start in (self.builds.b0, self.builds.b1):
            install = self.install(start)
            pdb = read(install, "blender.pdb")
            self.assertEqual(package.apply_update(install, self.p2, log=lambda _text: None), "5.4.2")
            self.assertEqual(read(install, "a.txt"), b"a0")
            self.assertEqual(read(install, "sub/d.py"), b"d1")
            self.assertFalse(os.path.exists(os.path.join(install, "b.txt")))
            self.assertEqual(read(install, "blender.pdb"), pdb)  # Debug symbols are left alone.
            self.assertEqual(versions.installed(install)["version"], "5.4.2")
            self.assertEqual(versions.installed(install)["base_version"], "5.4.0")
            with open(os.path.join(install, versions.MANIFEST_FILE), encoding="utf-8") as fh:
                manifest = json.load(fh)
            self.assertEqual(manifest["sub/d.py"], hashlib.sha256(b"d1").hexdigest())
            self.assertNotIn("b.txt", manifest)
            self.assertIn("blender.pdb", manifest)
            leftovers = [name for _folder, dirs, names in os.walk(install) for name in names + dirs
                         if name.endswith(".gfold") or name == ".gameforge-update"]
            self.assertEqual(leftovers, [])

    @unittest.skipUnless(sys.platform == "win32", "Windows file locks")
    def test_file_in_use_puts_everything_back(self):
        install = self.install(self.builds.b1)
        before = snapshot(install)
        with open(os.path.join(install, "b.txt"), "rb"):  # Opened without sharing delete: it cannot be renamed.
            with self.assertRaises(PermissionError) as caught:
                package.apply_update(install, self.p2, log=lambda _text: None)
        self.assertIn("b.txt", str(caught.exception))
        self.assertEqual(snapshot(install), before)

    def test_refusals_change_nothing(self):
        install = self.install(self.builds.b0)
        before = snapshot(install)
        evil = os.path.join(self.tmp, "evil.zip")
        with zipfile.ZipFile(evil, "w") as zf:
            zf.writestr(package.UPDATE_FILE, json.dumps({
                "schema": 1, "version": "5.4.3", "base_version": "5.4.0", "removed": [],
                "files": {"../x.dll": "0" * 64}, "manifest": {"../x.dll": "0" * 64}}))
            zf.writestr("../x.dll", b"x")
        with self.assertRaises(package.UpdateError):
            package.apply_update(install, evil)
        later = os.path.join(self.tmp, "later.zip")  # Starts from a base newer than the installation.
        with zipfile.ZipFile(later, "w") as zf:
            zf.writestr(package.UPDATE_FILE, json.dumps({
                "schema": 1, "version": "5.5.1", "base_version": "5.5.0", "removed": [], "files": {}, "manifest": {}}))
        with self.assertRaises(package.UpdateError):
            package.apply_update(install, later)
        self.assertEqual(snapshot(install), before)
        self.assertFalse(os.path.exists(os.path.join(self.tmp, "x.dll")))
        package.apply_update(install, self.p2, log=lambda _text: None)
        updated = snapshot(install)
        with self.assertRaises(package.UpdateError):  # The same version again.
            package.apply_update(install, self.p2, log=lambda _text: None)
        self.assertEqual(snapshot(install), updated)

    def test_apply_role_checks_the_signature(self):
        install = self.install(self.builds.b0)
        log = os.path.join(self.tmp, "update.log")
        signature.PINS["key_test"] = TEST_PINS["key_test"]
        self.addCleanup(signature.PINS.pop, "key_test", None)
        good = signature.envelope_from_download(signed_answer("5.4.2", package.file_sha256(self.p2)))
        for envelope, expected in ((dict(good, version="5.4.9"), 1), (good, 0)):
            path = os.path.join(self.tmp, "envelope.json")
            with open(path, "w", encoding="utf-8") as fh:
                json.dump(envelope, fh)
            self.assertEqual(applier.apply(install, self.p2, path, log), expected)
        with open(log, encoding="utf-8") as fh:
            text = fh.read()
        self.assertIn("RESULTADO falhou: o pacote não confere com a assinatura do portal", text)
        self.assertIn("RESULTADO atualizado para 5.4.2", text)
        self.assertFalse(os.path.exists(os.path.join(install, ".gameforge-package.zip")))

    @unittest.skipUnless(sys.platform == "win32", "Windows command lines")
    def test_elevated_command_line_keeps_each_argument(self):
        import ctypes
        from ctypes import wintypes
        arguments = ["apply", r"C:\Program Files\ATOM GameForge\GameForge 3D Studio 5.4",
                     r"C:\Users\Zé Ação\AppData\Local\x y\pacote.zip", 'D:\\a"b\\envelope.json', "C:\\log dir\\"]
        parse = ctypes.windll.shell32.CommandLineToArgvW
        parse.argtypes = [wintypes.LPCWSTR, ctypes.POINTER(ctypes.c_int)]
        parse.restype = ctypes.POINTER(wintypes.LPWSTR)
        count = ctypes.c_int()
        argv = parse("python.exe " + applier.elevated_parameters(arguments), ctypes.byref(count))
        parsed = [argv[i] for i in range(count.value)]
        ctypes.windll.kernel32.LocalFree(argv)
        self.assertEqual(parsed[1:3], ["-I", os.path.abspath(applier.__file__)])
        self.assertEqual(parsed[3:], arguments)

    def test_studio_running(self):
        self.assertTrue(applier.studio_running('"GameForge3DStudio.exe","1234","Console","1","500.000 K"\r\n'))
        self.assertFalse(applier.studio_running("INFORMAÇÕES: nenhuma tarefa em execução.\r\n"))


class FakePortal:
    def __init__(self, answer, data):
        self.answer, self.data = answer, data

    def plugin_download(self, slug, token):
        assert (slug, token) == (gameforge_update.SLUG, "tok")
        return self.answer

    def fetch_to_file(self, url, path, *, max_bytes, progress=None):
        with open(path, "wb") as fh:
            fh.write(self.data)
        if progress is not None:
            progress(len(self.data), len(self.data))
        return hashlib.sha256(self.data).hexdigest()


class DownloadTest(unittest.TestCase):

    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp, True)
        old = os.environ.get("LOCALAPPDATA")
        os.environ["LOCALAPPDATA"] = self.tmp
        self.addCleanup(lambda: os.environ.__setitem__("LOCALAPPDATA", old) if old else os.environ.pop("LOCALAPPDATA"))

    def test_downloads_and_checks(self):
        data = b"pacote"
        answer = signed_answer("5.4.2", hashlib.sha256(data).hexdigest(), base="5.4.0")
        package_path, envelope_path, version = gameforge_update.download(
            FakePortal(answer, data), "tok", "5.4.1", pins=TEST_PINS)
        self.assertEqual(version, "5.4.2")
        with open(package_path, "rb") as fh:
            self.assertEqual(fh.read(), data)
        with open(envelope_path, encoding="utf-8") as fh:
            self.assertTrue(signature.envelope_ok(json.load(fh), TEST_PINS))

    def test_refusals(self):
        data = b"pacote"
        digest = hashlib.sha256(data).hexdigest()
        with self.assertRaises(gameforge_update.NeedsInstaller):
            gameforge_update.download(FakePortal(signed_answer("5.5.1", digest, base="5.5.0"), data), "tok", "5.4.2",
                                      pins=TEST_PINS)
        cases = (
            signed_answer("5.4.2", digest, seed=bytes(32)),                # Another key.
            signed_answer("5.4.2", hashlib.sha256(b"outro").hexdigest()),  # Not what was downloaded.
            signed_answer("5.4.2", digest, name="Setup.msi"),              # Not an update package.
            signed_answer("5.4.1", digest),                                # Already installed.
        )
        for answer in cases:
            with self.assertRaises(gameforge_update.UpdateError):
                gameforge_update.download(FakePortal(answer, data), "tok", "5.4.1", pins=TEST_PINS)
        self.assertEqual([n for n in os.listdir(gameforge_update.staging_dir()) if n.endswith((".zip", ".part"))], [])


class _Files(http.server.BaseHTTPRequestHandler):
    body = b"x" * (3 * 1024 * 1024 + 5)

    def do_GET(self):
        self.send_response(200)
        self.send_header("content-length", str(len(self.body)))
        self.end_headers()
        self.wfile.write(self.body if self.path == "/full" else self.body[:1000])  # "/short": the link drops.

    def log_message(self, *_args):
        pass


class FetchTest(unittest.TestCase):

    def setUp(self):
        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), _Files)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.addCleanup(self.server.shutdown)
        self.base = "http://127.0.0.1:{:d}".format(self.server.server_address[1])
        self.portal = Portal(opener=urllib.request.build_opener())
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp, True)

    def test_streams_with_progress(self):
        seen = []
        path = os.path.join(self.tmp, "full.part")
        digest = self.portal.fetch_to_file(self.base + "/full", path, max_bytes=10 * 2 ** 20,
                                           progress=lambda done, total: seen.append((done, total)))
        self.assertEqual(digest, hashlib.sha256(_Files.body).hexdigest())
        self.assertEqual(seen[-1], (len(_Files.body), len(_Files.body)))
        self.assertGreater(len(seen), 1)

    def test_truncated_download_fails(self):
        with self.assertRaises(NetworkError):
            self.portal.fetch_to_file(self.base + "/short", os.path.join(self.tmp, "short.part"), max_bytes=10 * 2 ** 20)

    def test_too_big(self):
        with self.assertRaises(PortalError):
            self.portal.fetch_to_file(self.base + "/full", os.path.join(self.tmp, "big.part"), max_bytes=1000)


if __name__ == "__main__":
    unittest.main()
