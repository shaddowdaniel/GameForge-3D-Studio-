# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Tests for the client library (``gameforge_angelica``) against a real game client (read only).

The client folder (``element``) is the first entry of ``GAMEFORGE_TEST_CLIENTS``. Run inside the Studio:
  GameForge3DStudio.exe -b --factory-startup --python tests/python/gameforge_client_library_test.py
"""

import json
import os
import sys
import tempfile
import unittest

import bpy
import _angelica

from gameforge_angelica import clients, library, sources

CLIENTS = [p for p in os.environ.get("GAMEFORGE_TEST_CLIENTS", "").split(";") if p.strip()]
ROOT = CLIENTS[0] if CLIENTS else ""


def first_file(lib, extension, mount="models"):
    container = next(c for c in lib.containers if c.mount.lower() == mount)
    return next(path for path, _size in container.files() if path.lower().endswith(extension))


@unittest.skipUnless(ROOT, "set GAMEFORGE_TEST_CLIENTS to a client folder")
class LibraryTest(unittest.TestCase):

    def test_clients_registry(self):
        with tempfile.TemporaryDirectory() as tmp:
            clients._config_path = lambda: os.path.join(tmp, "clients.json")
            game_folder = os.path.dirname(ROOT)
            client = clients.add(game_folder)
            self.assertIsNotNone(client, "the game folder holds an element folder with packages")
            self.assertEqual(os.path.normcase(client.root), os.path.normcase(os.path.normpath(ROOT)))
            self.assertEqual([c.root for c in clients.load()], [client.root])
            clients.remove(client.root)
            self.assertEqual(clients.load(), [])
            self.assertIsNone(clients.add(tmp), "a folder without packages is not a client")

    def test_tree_and_search(self):
        lib = library.get(ROOT, refresh=True)
        names = [c.name.lower() for c in lib.containers]
        self.assertIn("models.pck", names)
        self.assertIn("maps", names, "the loose maps folder is a container too")
        models = lib.containers[names.index("models.pck")]
        folders, files = models.children(models.root_prefix)
        self.assertTrue(folders, "models.pck has folders")
        total = sum(count for _name, _prefix, count in folders) + len(files)
        self.assertEqual(total, len(models.files()), "every file is under exactly one top folder")
        name, prefix, count = folders[0]
        sub_folders, sub_files = models.children(prefix)
        self.assertEqual(sum(c for _n, _p, c in sub_folders) + len(sub_files), count)
        results, _truncated = lib.search(".ecm", kind="MODEL", limit=50)
        self.assertTrue(results)
        self.assertTrue(all(path.lower().endswith(".ecm") for _c, path, _s in results))


@unittest.skipUnless(ROOT, "set GAMEFORGE_TEST_CLIENTS to a client folder")
class OpenTest(unittest.TestCase):

    def test_text_round_trip(self):
        lib = library.get(ROOT)
        path = first_file(lib, ".gfx", mount="gfx")
        data = _angelica.read(ROOT, path)
        text, encoding, newline = sources.decode_text(data)
        self.assertEqual(sources.encode_text(text, encoding, newline), data, "unedited text is written back as read")
        block = sources.open_text(ROOT, path)
        self.assertEqual(block[sources.PROP_FILE], path)
        self.assertTrue(block[sources.PROP_EXACT])
        self.assertEqual(sources.text_fingerprint(block.as_string()), block[sources.PROP_FINGERPRINT])

    def test_open_image(self):
        lib = library.get(ROOT)
        path = first_file(lib, ".dds")
        image = sources.open_image(ROOT, path)
        self.assertGreater(image.size[0], 1, path)
        self.assertEqual(image[sources.PROP_FILE], path)

    def test_open_model_records_its_files(self):
        lib = library.get(ROOT)
        container = next(c for c in lib.containers if c.mount.lower() == "models")
        ecms = [p for p, _s in container.files() if p.lower().endswith(".ecm")]
        for path in ecms[:: max(1, len(ecms) // 10)]:
            before = set(bpy.data.collections)
            result = bpy.ops.wm.angelica_import_asset(client_root=ROOT, engine_path=path)
            if 'FINISHED' not in result:
                continue
            collection = next(c for c in bpy.data.collections if c not in before and "gameforge_source" in c)
            source = json.loads(collection["gameforge_source"])
            self.assertEqual(source["path"], path.lower())
            files = {f["path"] for f in source["files"]}
            self.assertIn(path.lower(), files)
            self.assertTrue(any(f.endswith(".ski") for f in files), files)
            meshes = [ob for ob in collection.all_objects if ob.type == 'MESH']
            self.assertTrue(meshes)
            self.assertTrue(all("gameforge_skin_file" in ob for ob in meshes))
            self.assertTrue(all(ob["gameforge_skin_file"] in files for ob in meshes))
            print("{:>3d} files  {:>3d} meshes  {}".format(len(files), len(meshes), path))
            return
        self.fail("no model of the client could be opened")


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]], exit=False, verbosity=2)
