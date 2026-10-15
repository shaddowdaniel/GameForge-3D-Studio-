# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Tests for the ``_angelica`` module against real game clients (read only, nothing is written).

The client folders (``element``) are given in ``GAMEFORGE_TEST_CLIENTS``, separated by ``;``.
Without it the tests are skipped. Run inside the Studio:
  set GAMEFORGE_TEST_CLIENTS=D:\\clients\\pw155\\element;D:\\clients\\pw178\\element
  GameForge3DStudio.exe -b --factory-startup --python tests/python/gameforge_angelica_client_test.py
"""

import os
import sys
import unittest

import _angelica

CLIENTS = [p for p in os.environ.get("GAMEFORGE_TEST_CLIENTS", "").split(";") if p.strip()]


def client_packages(root):
    """``element\\*.pck`` plus packages one folder deeper (``help\\script.pck``)."""
    found = []
    for dirpath, dirnames, filenames in os.walk(root):
        depth = os.path.relpath(dirpath, root).count(os.sep) + (0 if dirpath == root else 1)
        if depth > 1:
            dirnames[:] = []
            continue
        for name in filenames:
            if name.lower().endswith(".pck"):
                found.append(os.path.join(dirpath, name))
    return sorted(found)


@unittest.skipUnless(CLIENTS, "set GAMEFORGE_TEST_CLIENTS to the client folders to test")
class ClientPackagesTest(unittest.TestCase):

    def test_every_package_opens_and_reads(self):
        for root in CLIENTS:
            packages = client_packages(root)
            self.assertTrue(packages, "no packages in " + root)
            for pck in packages:
                with self.subTest(package=pck):
                    info = _angelica.package_info(pck)
                    entries = _angelica.package_entries(pck)
                    self.assertEqual(info["entries"], len(entries))
                    print("{:>8d} entries  {:#010x}  {}".format(len(entries), info["version"], pck))
                    if not entries:
                        continue
                    # The first and the last file decompress to their stored sizes.
                    mount = os.path.splitext(os.path.relpath(pck, root))[0]
                    for path, size, _stored in (entries[0], entries[-1]):
                        engine_path = path if path.lower().startswith(mount.lower() + "\\") else mount + "\\" + path
                        location = _angelica.locate(root, engine_path)
                        self.assertIsNotNone(location, engine_path)
                        if location["kind"] != "PACKED":
                            continue  # A loose copy would not be read from the package.
                        data = _angelica.read(root, engine_path)
                        self.assertIsNotNone(data, engine_path)
                        self.assertEqual(len(data), size, engine_path)


@unittest.skipUnless(CLIENTS, "set GAMEFORGE_TEST_CLIENTS to the client folders to test")
class DependenciesTest(unittest.TestCase):

    def test_model_dependencies(self):
        """An ``.ecm`` reaches its skin model, skeleton, skins and their textures."""
        for root in CLIENTS:
            models = os.path.join(root, "models.pck")
            if not os.path.exists(models):
                continue
            ecms = [path for path, _size, _stored in _angelica.package_entries(models)
                    if path.lower().endswith(".ecm")]
            self.assertTrue(ecms, "no .ecm in " + models)
            checked = 0
            for path in ecms[:: max(1, len(ecms) // 20)]:
                engine_path = path if path.lower().startswith("models\\") else "models\\" + path
                deps = _angelica.dependencies(root, engine_path)
                self.assertEqual(deps[0]["kind"], "root")
                kinds = {d["kind"] for d in deps if d["location"]}
                if "skin_model" not in kinds:
                    continue  # Some retail models reference files the client does not ship.
                self.assertIn("skeleton", kinds, engine_path)
                missing = [d["path"] for d in deps if not d["location"]]
                print("{:>4d} files, {:>3d} missing  {}".format(len(deps), len(missing), engine_path))
                checked += 1
            self.assertGreater(checked, 0, "no model with a complete skin model in " + root)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]], exit=False, verbosity=2)
