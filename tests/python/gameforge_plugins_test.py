# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Tests for ``gameforge_account.plugins`` (Studio plugins downloaded from atomgameforge.com).

Runs with plain Python, no Studio build needed:
  python3 tests/python/gameforge_plugins_test.py
"""

import hashlib
import io
import os
import sys
import tempfile
import unittest
import zipfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "scripts", "modules"))

from gameforge_account.plugins import SCHEMAS_PLUGIN_SLUG, PluginCache, installed_dir  # noqa: E402
from gameforge_account.portal import PortalError  # noqa: E402


def make_zip(files):
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w", compression=zipfile.ZIP_DEFLATED) as zf:
        for name, data in files.items():
            zf.writestr(name, data)
    return buffer.getvalue()


class FakePortal:
    def __init__(self):
        self.releases = {}
        self.current = None
        self.fetches = 0
        self.tamper = False

    def publish(self, version, files):
        self.releases[version] = make_zip(files)
        self.current = version

    def plugin_download(self, slug, token, version=None):
        assert slug == SCHEMAS_PLUGIN_SLUG and token == "tok"
        version = version or self.current
        data = self.releases[version]
        return {
            "version": version,
            "download_url": "https://storage/" + version,
            "file_hash": hashlib.sha256(data).hexdigest(),
        }

    def fetch(self, url, *, max_bytes):
        self.fetches += 1
        data = self.releases[url.rsplit("/", 1)[1]]
        return data + b"x" if self.tamper else data


class PluginCacheTest(unittest.TestCase):

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.portal = FakePortal()
        self.cache = PluginCache(self.portal, self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_install_and_reuse(self):
        self.portal.publish("1.0.0", {"elements/PW_1.5.5_v156.cfg": "a", "schemas.json": "{}"})
        path, version, updated = self.cache.sync(SCHEMAS_PLUGIN_SLUG, "tok")
        self.assertEqual((version, updated), ("1.0.0", True))
        with open(os.path.join(path, "elements", "PW_1.5.5_v156.cfg")) as fh:
            self.assertEqual(fh.read(), "a")
        self.assertEqual(installed_dir(self.tmp.name, SCHEMAS_PLUGIN_SLUG), path)

        _path, _version, updated = self.cache.sync(SCHEMAS_PLUGIN_SLUG, "tok")
        self.assertFalse(updated)
        self.assertEqual(self.portal.fetches, 1)

    def test_update_replaces_old_version(self):
        self.portal.publish("1.0.0", {"elements/a.cfg": "1"})
        old, _v, _u = self.cache.sync(SCHEMAS_PLUGIN_SLUG, "tok")
        self.portal.publish("1.1.0", {"elements/a.cfg": "2"})
        new, version, updated = self.cache.sync(SCHEMAS_PLUGIN_SLUG, "tok")
        self.assertEqual((version, updated), ("1.1.0", True))
        self.assertFalse(os.path.exists(old))
        with open(os.path.join(new, "elements", "a.cfg")) as fh:
            self.assertEqual(fh.read(), "2")

    def test_hash_mismatch_keeps_installed_version(self):
        self.portal.publish("1.0.0", {"elements/a.cfg": "1"})
        path, _v, _u = self.cache.sync(SCHEMAS_PLUGIN_SLUG, "tok")
        self.portal.publish("1.1.0", {"elements/a.cfg": "2"})
        self.portal.tamper = True
        with self.assertRaises(PortalError):
            self.cache.sync(SCHEMAS_PLUGIN_SLUG, "tok")
        self.assertEqual(installed_dir(self.tmp.name, SCHEMAS_PLUGIN_SLUG), path)

    def test_unsafe_paths_rejected(self):
        self.portal.publish("1.0.0", {"../evil.cfg": "x"})
        with self.assertRaises(PortalError):
            self.cache.sync(SCHEMAS_PLUGIN_SLUG, "tok")
        self.assertIsNone(installed_dir(self.tmp.name, SCHEMAS_PLUGIN_SLUG))
        self.assertFalse(os.path.exists(os.path.join(self.tmp.name, "evil.cfg")))

    def test_only_data_files_extracted(self):
        self.portal.publish("1.0.0", {"elements/a.cfg": "1", "run.py": "print()", "tool.exe": "MZ"})
        path, _v, _u = self.cache.sync(SCHEMAS_PLUGIN_SLUG, "tok")
        self.assertTrue(os.path.exists(os.path.join(path, "elements", "a.cfg")))
        self.assertFalse(os.path.exists(os.path.join(path, "run.py")))
        self.assertFalse(os.path.exists(os.path.join(path, "tool.exe")))

    def test_not_a_zip(self):
        self.portal.releases["1.0.0"] = b"not a zip"
        self.portal.current = "1.0.0"
        with self.assertRaises(PortalError):
            self.cache.sync(SCHEMAS_PLUGIN_SLUG, "tok")


if __name__ == "__main__":
    unittest.main()
