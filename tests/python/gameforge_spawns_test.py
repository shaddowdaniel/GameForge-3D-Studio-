# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Tests for editing map spawns (``gameforge_angelica.spawns``, the server's ``npcgen.data``).

``GAMEFORGE_TEST_SERVER`` is a server data folder (``gamed\\config``, one folder per map); the
smallest ``npcgen.data`` there is copied to a temporary server folder and only the copy is
written. ``GAMEFORGE_TEST_CLIENTS`` (first client) gives the template names. Run inside the Studio:
  GameForge3DStudio.exe -b --factory-startup --python tests/python/gameforge_spawns_test.py
"""

import hashlib
import os
import shutil
import sys
import tempfile
import unittest

import bpy

from gameforge_angelica import npcgen, save, spawns

SERVER = os.environ.get("GAMEFORGE_TEST_SERVER", "")
CLIENTS = [p for p in os.environ.get("GAMEFORGE_TEST_CLIENTS", "").split(";") if p.strip()]


def smallest_map(server):
    best = None
    for name in os.listdir(server):
        path = os.path.join(server, name, "npcgen.data")
        if os.path.isfile(path):
            size = os.path.getsize(path)
            with open(path, "rb") as fh:
                gen = npcgen.NpcGen(fh.read())
            if len(gen.areas) >= 3 and (best is None or size < best[0]):
                best = (size, name)
    return best[1] if best else None


@unittest.skipUnless(SERVER and os.path.isdir(SERVER), "set GAMEFORGE_TEST_SERVER to a server data folder")
class SpawnsTest(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="gameforge-spawns-test-")
        cls.config = os.path.join(cls.tmp, "config")
        cls._config_dir = save._config_dir
        save._config_dir = lambda: cls.config
        cls.map = smallest_map(SERVER)
        if cls.map is None:
            raise unittest.SkipTest("no map with spawns")
        cls.server = os.path.join(cls.tmp, "server")
        os.makedirs(os.path.join(cls.server, cls.map))
        shutil.copy2(os.path.join(SERVER, cls.map, "npcgen.data"), os.path.join(cls.server, cls.map, "npcgen.data"))
        cls.path = os.path.join(cls.server, cls.map, "npcgen.data")
        with open(cls.path, "rb") as fh:
            cls.original = fh.read()
        print("map", cls.map, "areas", len(npcgen.NpcGen(cls.original).areas))

    @classmethod
    def tearDownClass(cls):
        save._config_dir = cls._config_dir
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def setUp(self):
        for collection in list(bpy.data.collections):
            bpy.data.collections.remove(collection)
        for ob in list(bpy.data.objects):
            bpy.data.objects.remove(ob)
        self.collection = spawns.load_map_spawns(self.server, self.map, CLIENTS[0] if CLIENTS else "")
        self.collection[spawns.PROP_HASH] = hashlib.sha1(self.original).hexdigest()

    def areas(self):
        return sorted((ob for ob in self.collection.objects if ob.gameforge_spawn.kind == 'AREA'),
                      key=lambda o: o.gameforge_spawn.index)

    def test_unchanged_is_identical(self):
        self.assertEqual(spawns.build_npcgen(self.collection), self.original)
        self.assertFalse([c for c in save.pending_changes() if c.kind == 'NPCGEN'])

    def test_move_duplicate_delete_and_edit(self):
        areas = self.areas()
        original = npcgen.NpcGen(self.original)
        areas[0].location.x += 5.0
        areas[1].gameforge_spawn.spawns[0].count += 3
        # A duplicate (same area index) becomes a new area; a deleted object takes its area away.
        duplicate = areas[2].copy()
        self.collection.objects.link(duplicate)
        duplicate.location.y += 10.0
        bpy.data.objects.remove(areas[-1])
        result = npcgen.NpcGen(spawns.build_npcgen(self.collection))
        self.assertEqual(len(result.areas), len(original.areas))
        self.assertAlmostEqual(result.areas[0].pos[0], original.areas[0].pos[0] + 5.0, places=3)
        self.assertEqual(result.areas[0].pos[1:], original.areas[0].pos[1:])
        self.assertEqual(result.areas[1].spawns[0].count, original.areas[1].spawns[0].count + 3)
        for i in range(2, len(original.areas) - 1):
            self.assertEqual(result.areas[i], original.areas[i])
        # The duplicate is the last area, 10 further on the engine's Z.
        self.assertAlmostEqual(result.areas[-1].pos[2], original.areas[2].pos[2] + 10.0, places=3)
        self.assertEqual(result.areas[-1].spawns, original.areas[2].spawns)

    def test_size_is_full_extent(self):
        # vExts is the area's full size: the empty, drawn from -1 to 1, is scaled by half of it.
        area = self.areas()[0]
        original = npcgen.NpcGen(self.original).areas[0]
        area.scale.x = 4.0
        result = npcgen.NpcGen(spawns.build_npcgen(self.collection)).areas[0]
        self.assertAlmostEqual(result.extents[0], 8.0, places=3)
        # The axes that were not resized keep the file's values (even point areas drawn 1 m wide).
        self.assertEqual(result.extents[1:], original.extents[1:])

    @unittest.skipUnless(CLIENTS, "the models come from the client")
    def test_show_models(self):
        shown, failed = spawns.show_models(self.collection, CLIENTS[0], limit=8)
        print("models shown", shown, "failed", failed[:3])
        self.assertGreater(shown, 0)
        instances = [ob for ob in self.collection.all_objects if ob.instance_type == 'COLLECTION']
        self.assertTrue(instances)
        self.assertTrue(all(ob.constraints and ob.constraints[0].target for ob in instances))
        # The model objects are not spawn areas: the file stays the same.
        self.assertEqual(spawns.build_npcgen(self.collection), self.original)

    @unittest.skipUnless(CLIENTS, "the names come from the client")
    def test_template_names(self):
        entry = self.areas()[0].gameforge_spawn.spawns[0]
        known = spawns.templates(CLIENTS[0])
        self.assertEqual(entry.name, known[entry.template_id][0])
        other = next(i for i, (_name, kind) in known.items() if kind == "MONSTER_ESSENCE" and i != entry.template_id)
        entry.template_id = other
        self.assertEqual(entry.name, known[other][0])
        # The kinds with names write the file's integers.
        props = self.areas()[0].gameforge_spawn
        props.shape = 'BOX' if props.area_type == 0 else 'TERRAIN'
        self.assertEqual(props.area_type, 1 if props.shape == 'BOX' else 0)
        self.assertEqual(self.areas()[0].empty_display_type, 'CUBE' if props.area_type == 1 else 'CIRCLE')

    def test_save_and_undo(self):
        self.areas()[0].location.z += 1.0
        changes = [c for c in save.pending_changes() if c.kind == 'NPCGEN']
        self.assertEqual(len(changes), 1)
        save.save(changes, 'IN_PLACE')
        with open(self.path, "rb") as fh:
            saved = fh.read()
        self.assertNotEqual(saved, self.original)
        self.assertFalse([c for c in save.pending_changes() if c.kind == 'NPCGEN'])
        self.assertEqual(save.undo_last_save(), [])
        with open(self.path, "rb") as fh:
            self.assertEqual(fh.read(), self.original)
        self.assertEqual(len([c for c in save.pending_changes() if c.kind == 'NPCGEN']), 1)


if __name__ == "__main__":
    sys.argv = [__file__] + (sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])
    result = unittest.main(exit=False).result
    sys.exit(0 if result.wasSuccessful() else 1)
