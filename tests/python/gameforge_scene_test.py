# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Tests for the client scene of a map (``gameforge_angelica.ecwld`` and ``.scene``).

``GAMEFORGE_TEST_CLIENTS`` (first client) gives a map with effects, scene models and buildings.
Writes go to a temporary "mini client" holding copies of that map's ``.ecwld`` and ``.ecbsd``;
the real client is only read. Run inside the Studio:
  GameForge3DStudio.exe -b --factory-startup --python tests/python/gameforge_scene_test.py
"""

import os
import shutil
import sys
import tempfile
import unittest

import bpy

from gameforge_angelica import ecwld, save, scene

CLIENTS = [p for p in os.environ.get("GAMEFORGE_TEST_CLIENTS", "").split(";") if p.strip()]
MAP = os.environ.get("GAMEFORGE_TEST_MAP", "a08")


def scene_files(root, code):
    folder = os.path.join(root, "maps", code)
    return os.path.join(folder, code + ".ecwld"), os.path.join(folder, code + ".ecbsd")


@unittest.skipUnless(CLIENTS and os.path.isfile(scene_files(CLIENTS[0], MAP)[0]) if CLIENTS else False,
                     "set GAMEFORGE_TEST_CLIENTS to a client with maps\\" + MAP)
class SceneTest(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="gameforge-scene-test-")
        cls._config_dir = save._config_dir
        save._config_dir = lambda: os.path.join(cls.tmp, "config")
        cls.mini = os.path.join(cls.tmp, "client")
        os.makedirs(os.path.join(cls.mini, "maps", MAP))
        for src, dst in zip(scene_files(CLIENTS[0], MAP), scene_files(cls.mini, MAP)):
            shutil.copy2(src, dst)
        cls.world_path, cls.bsd_path = scene_files(cls.mini, MAP)
        with open(cls.world_path, "rb") as fh:
            cls.world = fh.read()
        with open(cls.bsd_path, "rb") as fh:
            cls.bsd = fh.read()

    @classmethod
    def tearDownClass(cls):
        save._config_dir = cls._config_dir
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def setUp(self):
        for collection in list(bpy.data.collections):
            bpy.data.collections.remove(collection)
        for ob in list(bpy.data.objects):
            bpy.data.objects.remove(ob)

    def load_mini(self):
        collection, _problems = scene.load_map_scene(self.mini, MAP, building_limit=0)
        return collection

    def objects(self, collection, kind):
        return sorted((ob for ob in collection.all_objects if ob.get(scene.PROP_KIND) == kind),
                      key=lambda ob: ob[scene.PROP_ID])

    def test_read_all_kinds(self):
        parsed = ecwld.SceneFile(self.world, self.bsd)
        self.assertEqual(parsed.world_bytes(), self.world)
        kinds = {ob.kind for ob in parsed.objects}
        self.assertIn('BUILDING', kinds)
        self.assertIn('EFFECT', kinds)
        # Buildings come from litmodels\<map>\..., effects are .gfx files.
        self.assertTrue(all(ob.path.lower().endswith(".bmd") for ob in parsed.objects if ob.kind == 'BUILDING'))
        self.assertTrue(all(ob.path.lower().endswith(".gfx") for ob in parsed.objects if ob.kind == 'EFFECT'))

    def test_every_map_reads(self):
        # Every map of the client, the newer versions (world 16 / 17, shared data 17 / 18) included.
        maps = os.path.join(CLIENTS[0], "maps")
        versions = set()
        for code in sorted(os.listdir(maps)):
            world_path, bsd_path = scene_files(CLIENTS[0], code)
            if not (os.path.isfile(world_path) and os.path.isfile(bsd_path)):
                continue
            with open(world_path, "rb") as fh:
                world = fh.read()
            with open(bsd_path, "rb") as fh:
                bsd = fh.read()
            parsed = ecwld.SceneFile(world, bsd)
            versions.add((parsed.version, parsed.bsd_version))
            self.assertEqual(parsed.world_bytes(), world, code)
            for ob in parsed.objects:
                if ob.kind == 'MODEL':
                    self.assertTrue(ob.path.lower().endswith(".ecm"), (code, ob.path))
        print("scene versions", sorted(versions))

    def test_unchanged_is_identical(self):
        collection = self.load_mini()
        self.assertTrue(self.objects(collection, 'EFFECT'))
        self.assertFalse(scene.scene_changed(collection))
        self.assertEqual(scene.build_world(collection), self.world)
        self.assertFalse([c for c in save.pending_changes() if c.kind == 'SCENE'])

    def test_move_and_turn(self):
        collection = self.load_mini()
        effect = self.objects(collection, 'EFFECT')[0]
        effect_id = effect[scene.PROP_ID]
        effect.location.x += 5.0
        effect.location.z += 1.0
        movable = self.objects(collection, 'MODEL') or self.objects(collection, 'EFFECT')[1:]
        turned = movable[0]
        turned.rotation_euler.z += 1.5707963
        built = scene.build_world(collection)
        before = {(o.kind, o.export_id): o for o in ecwld.SceneFile(self.world, self.bsd).objects}
        after = {(o.kind, o.export_id): o for o in ecwld.SceneFile(built, self.bsd).objects}
        moved = after[('EFFECT', effect_id)]
        old = before[('EFFECT', effect_id)]
        # Blender x is engine x, Blender z is engine y (up).
        self.assertAlmostEqual(moved.pos[0], old.pos[0] + 5.0, places=3)
        self.assertAlmostEqual(moved.pos[1], old.pos[1] + 1.0, places=3)
        self.assertAlmostEqual(moved.pos[2], old.pos[2], places=3)
        key = (turned[scene.PROP_KIND], turned[scene.PROP_ID])
        self.assertNotAlmostEqual(after[key].dir[0], before[key].dir[0], places=2)
        self.assertAlmostEqual(after[key].pos[0], before[key].pos[0], places=3)
        # Nothing else changed: same objects, and only the two records' floats differ.
        for k, o in before.items():
            if k not in (('EFFECT', effect_id), key):
                self.assertEqual((after[k].pos, after[k].dir, after[k].up, after[k].path), (o.pos, o.dir, o.up, o.path))
        self.assertEqual(len(built), len(self.world))
        changed = [i for i, (a, b) in enumerate(zip(built, self.world)) if a != b]
        self.assertLessEqual(len(changed), 2 * 36)

    def test_save_and_undo(self):
        collection = self.load_mini()
        self.objects(collection, 'EFFECT')[0].location.y -= 2.0
        changes = [c for c in save.pending_changes() if c.kind == 'SCENE']
        self.assertEqual(len(changes), 1)
        self.assertEqual(save.target(self.mini, changes[0].path)[0], 'FILE')
        save.save(changes, 'IN_PLACE')
        with open(self.world_path, "rb") as fh:
            saved = fh.read()
        self.assertNotEqual(saved, self.world)
        with open(self.bsd_path, "rb") as fh:
            self.assertEqual(fh.read(), self.bsd)
        self.assertFalse([c for c in save.pending_changes() if c.kind == 'SCENE'])
        self.assertEqual(save.undo_last_save(), [])
        with open(self.world_path, "rb") as fh:
            self.assertEqual(fh.read(), self.world)
        self.assertEqual(len([c for c in save.pending_changes() if c.kind == 'SCENE']), 1)

    def test_blocks_add_remove_and_move_across(self):
        parsed = ecwld.SceneFile(self.world, self.bsd)
        effects = [o for o in parsed.objects if o.kind == 'EFFECT']
        self.assertGreaterEqual(len(effects), 3)
        first, second, doomed = effects[:3]
        # The middle of a block that holds none of these effects.
        home = {parsed.block_at(o.pos).index for o in effects[:3]}
        target = next(b for b in parsed.blocks if b.index not in home)
        x = -parsed.world_width / 2 + (target.col + 0.5) * parsed.block_size
        z = parsed.world_length / 2 - (target.row + 0.5) * parsed.block_size
        copy = parsed.add_copy(first, (x, first.pos[1], z))
        parsed.move(second, pos=(x + 1.0, second.pos[1], z + 1.0))
        parsed.remove(doomed)
        again = ecwld.SceneFile(parsed.world_bytes(), self.bsd)
        found = {o.export_id: o for o in again.objects if o.kind == 'EFFECT'}
        self.assertEqual(len(found), len(effects))
        self.assertNotIn(doomed.export_id, found)
        self.assertEqual(found[copy.export_id].path, first.path)
        self.assertEqual(found[copy.export_id].extra, first.extra)
        self.assertEqual(found[copy.export_id].blocks, [target.index])
        self.assertEqual(found[second.export_id].blocks, [target.index])
        self.assertAlmostEqual(found[second.export_id].pos[0], x + 1.0, places=3)
        # Everything else is as it was.
        before = {(o.kind, o.export_id, o.pos) for o in ecwld.SceneFile(self.world, self.bsd).objects
                  if o.kind != 'EFFECT'}
        self.assertEqual(before, {(o.kind, o.export_id, o.pos) for o in again.objects if o.kind != 'EFFECT'})

    def test_copy_delete_save_and_undo(self):
        collection = self.load_mini()
        effects = self.objects(collection, 'EFFECT')
        source, doomed = effects[0], effects[1]
        source_id, doomed_id = source[scene.PROP_ID], doomed[scene.PROP_ID]
        copy = source.copy()
        for parent in source.users_collection:
            parent.objects.link(copy)
        copy.location.x += 3.0
        bpy.data.objects.remove(doomed)
        self.assertTrue(scene.scene_changed(collection))
        before = {o.export_id: o for o in ecwld.SceneFile(self.world, self.bsd).objects if o.kind == 'EFFECT'}

        def saved_effects():
            with open(self.world_path, "rb") as fh:
                return {o.export_id: o for o in ecwld.SceneFile(fh.read(), self.bsd).objects if o.kind == 'EFFECT'}

        save.save([c for c in save.pending_changes() if c.kind == 'SCENE'], 'IN_PLACE')
        found = saved_effects()
        new_id = copy[scene.PROP_ID]
        self.assertNotEqual(new_id, source_id)
        self.assertEqual(set(found), (set(before) - {doomed_id}) | {new_id})
        self.assertAlmostEqual(found[new_id].pos[0], before[source_id].pos[0] + 3.0, places=3)
        self.assertFalse(scene.scene_changed(collection))
        self.assertEqual(save.undo_last_save(), [])
        with open(self.world_path, "rb") as fh:
            self.assertEqual(fh.read(), self.world)
        # The Studio still has the copy and not the deleted one: saving again does both again.
        self.assertTrue(scene.scene_changed(collection))
        save.save([c for c in save.pending_changes() if c.kind == 'SCENE'], 'IN_PLACE')
        found = saved_effects()
        self.assertNotIn(doomed_id, found)
        self.assertEqual(len(found), len(before))
        self.assertEqual(save.undo_last_save(), [])

    def test_buildings_where_the_client_draws_them(self):
        # Read only: the real client's buildings, placed by their own .bmd.
        import _angelica
        collection, problems = scene.load_map_scene(CLIENTS[0], MAP, building_limit=5)
        buildings = self.objects(collection, 'BUILDING')
        print("buildings", len(buildings), "problems", problems[:3])
        self.assertTrue(buildings)
        for ob in buildings:
            self.assertEqual(ob.instance_type, 'COLLECTION')
            self.assertTrue(all(ob.lock_location))
            data = _angelica.read(CLIENTS[0], "litmodels\\{:s}\\{:s}".format(MAP, ob[scene.PROP_PATH]))
            _sizes, _dir, _up, pos = scene._bmd_placement(data)
            self.assertAlmostEqual(ob.matrix_basis.translation.x, pos[0], places=3)
            self.assertAlmostEqual(ob.matrix_basis.translation.y, pos[2], places=3)
            self.assertAlmostEqual(ob.matrix_basis.translation.z, pos[1], places=3)
        # Buildings are not scene edits: moving one gives no change to save.
        buildings[0].location.x += 10.0
        self.assertFalse(scene.scene_changed(collection))

    def test_water(self):
        # Read only: the water areas of the 1.7.8 world map near a lake (from its BSData files).
        if not os.path.isfile(scene_files(CLIENTS[0], "world")[0]):
            self.skipTest("the client has no world map")
        collection, _problems = scene.load_map_scene(CLIENTS[0], "world", center=(3584.0, 5120.0), radius=50.0,
                                                     building_limit=0)
        water = self.objects(collection, 'WATER')
        self.assertTrue(water)
        mesh = water[0].data
        self.assertTrue(mesh.polygons)
        heights = {round(v.co.z, 3) for v in mesh.vertices}
        self.assertEqual(len(heights), 1)
        self.assertTrue(all(p.normal.z > 0.99 for p in mesh.polygons))

    def test_newer_building_versions(self):
        # Read only. Lit models 0x10000301 (a101) and 0x10000302 (world) of the 1.7.8 client.
        import _angelica
        for code, center in (("a101", None), ("world", (1408.0, -2176.0))):
            if not os.path.isfile(scene_files(CLIENTS[0], code)[0]):
                continue
            collection, problems = scene.load_map_scene(CLIENTS[0], code, center=center,
                                                        radius=200.0 if center else 0.0, building_limit=3)
            buildings = self.objects(collection, 'BUILDING')
            self.assertEqual(len(buildings), 3, (code, problems))
            self.assertFalse([p for p in problems if p.startswith("litmodels")], code)
            data = _angelica.read(CLIENTS[0], "litmodels\\{:s}\\{:s}".format(code, buildings[0][scene.PROP_PATH]))
            version = int.from_bytes(data[9:13] if data[4:8] == b"\x01\x00\x00\x80" else data[4:8], "little")
            print(code, "building version", hex(version))
            self.assertTrue(all(ob.instance_collection.all_objects for ob in buildings))

    def test_terrain_colors_and_lightmap(self):
        # Read only. Terrain textures use their alpha for other things: their color must not vanish
        # where it is 0 (black terrain in the viewport); the lightmap modulates twice, like the
        # client's terrain shader (t2_3layer_batch_14: mul_x2).
        result = bpy.ops.wm.angelica_import_map(directory=CLIENTS[0], map_code=MAP, client_root=CLIENTS[0],
                                                import_heightmap=False, link_to_scene=True, mark_as_assets=False)
        self.assertIn('FINISHED', result)
        materials = [m for m in bpy.data.materials if m.name.lower().startswith(MAP.lower() + "_") and m.node_tree]
        self.assertTrue(materials)
        for material in materials:
            nodes = material.node_tree.nodes
            images = [n.image for n in nodes if n.type == 'TEX_IMAGE' and n.image and "_mask" not in n.image.name]
            self.assertTrue(images, material.name)
            self.assertEqual({i.alpha_mode for i in images}, {'CHANNEL_PACKED'}, material.name)
            if any("lightmap" in i.name for i in images):
                doubled = [n for n in nodes if n.type == 'MIX' and n.blend_type == 'MULTIPLY'
                           and tuple(next(s for s in n.inputs if s.identifier == "B_Color").default_value)[:3]
                           == (2.0, 2.0, 2.0)]
                self.assertTrue(doubled, material.name)


if __name__ == "__main__":
    sys.argv = [__file__] + (sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])
    result = unittest.main(exit=False).result
    sys.exit(0 if result.wasSuccessful() else 1)
