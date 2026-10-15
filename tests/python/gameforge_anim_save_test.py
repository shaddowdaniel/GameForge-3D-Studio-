# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Tests for saving edited animations (``gameforge_angelica.anims``) into a client.

A small animated model of the first client in ``GAMEFORGE_TEST_CLIENTS`` (folders separated by
``;``) and every file it uses are packed into a temporary mini client; the real client is only
read. The model is opened, its action exported unchanged and compared with the track set, then a
bone is posed differently, saved into the mini client's package and undone. Run inside the Studio:
  GameForge3DStudio.exe -b --factory-startup --python tests/python/gameforge_anim_save_test.py
"""

import hashlib
import math
import os
import shutil
import sys
import tempfile
import unittest

import bpy
import numpy as np

import _angelica
from gameforge_angelica import anims, save

sys.path.append(os.path.dirname(__file__))
from gameforge_skin_save_test import build_mini_client  # noqa: E402

CLIENTS = [p for p in os.environ.get("GAMEFORGE_TEST_CLIENTS", "").split(";") if p.strip()]
MAX_FILES = 200
MAX_BYTES = 32 * 1024 * 1024


def file_sha1(path):
    with open(path, "rb") as fh:
        return hashlib.sha1(fh.read()).hexdigest()


def find_animated_model(root, version=2):
    """An ``.ecm`` with few dependencies whose skeleton is animated by track sets of ``version``."""
    models = os.path.join(root, "models.pck")
    if not os.path.exists(models):
        return None, None
    ecms = [p for p, _size, _s in _angelica.package_entries(models) if p.lower().endswith(".ecm")]
    first = (None, None)
    for path in ecms[:: max(1, len(ecms) // 300)]:
        engine = path if path.lower().startswith("models\\") else "models\\" + path
        deps = [d for d in _angelica.dependencies(root, engine) if d["location"] is not None]
        if not deps or len(deps) > MAX_FILES or sum(d["location"]["size"] for d in deps) > MAX_BYTES:
            continue
        stcks = [d for d in deps if d["path"].lower().endswith(".stck")]
        if not stcks or any(_angelica.stck_decode(_angelica.read(root, d["path"]))["version"] != version
                            for d in stcks):
            continue
        bons = [_angelica.bon_layout(_angelica.read(root, d["path"]))
                for d in deps if d["path"].lower().endswith(".bon")]
        # Skeletons older than 6 carry their own animation: the engine does not play the .stck then.
        if any(b["version"] < 6 for b in bons):
            continue
        # Prefer skeletons with mirrored bones (the hardest case of the conversion).
        if any(bone["flags"] & anims.BONE_FLAG_FLIPPED for b in bons for bone in b["bones"]):
            return engine, deps
        if first[0] is None:
            first = (engine, deps)
    return first


def track_values(data, track_id, kind, frames):
    """Values of a decoded track (``position``/``rotation``) at its own frames."""
    tracks = _angelica.stck_decode(data)
    track = next(t for t in tracks["tracks"] if t["id"] == track_id)[kind]
    return np.array([anims._sample(track, f) for f in frames])


@unittest.skipUnless(CLIENTS, "set GAMEFORGE_TEST_CLIENTS to the client folders to test")
class AnimSaveTest(unittest.TestCase):
    VERSION = 2

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="gameforge-anim-test-")
        cls.config = os.path.join(cls.tmp, "config")
        cls._config_dir = save._config_dir
        save._config_dir = lambda: cls.config
        for root in CLIENTS:
            cls.ecm, deps = find_animated_model(root, cls.VERSION)
            if cls.ecm:
                break
        if not cls.ecm:
            raise unittest.SkipTest("no small animated model found")
        cls.root = os.path.join(cls.tmp, "element")
        build_mini_client(root, deps, cls.root)
        print("model: {} ({:d} files, .stck v{:d})".format(cls.ecm, len(deps), cls.VERSION))

    @classmethod
    def tearDownClass(cls):
        save._config_dir = cls._config_dir
        _angelica.release()
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def open_model(self):
        for ob in list(bpy.data.objects):
            bpy.data.objects.remove(ob)
        for action in list(bpy.data.actions):
            bpy.data.actions.remove(action)
        result = bpy.ops.wm.angelica_import_asset(client_root=self.root, engine_path=self.ecm)
        self.assertIn('FINISHED', result)
        for action in bpy.data.actions:
            if anims.PROP_FILE in action:
                action[anims.PROP_KEYS] = anims.action_fingerprint(action)
        armature = next(ob for ob in bpy.data.objects if ob.type == 'ARMATURE')
        armature[anims.PROP_REST] = anims.rest_fingerprint(armature)
        actions = [a for a in bpy.data.actions if anims.PROP_FILE in a]
        self.assertTrue(actions)
        return armature, actions

    def test_unchanged_export_matches_the_tracks(self):
        armature, actions = self.open_model()
        self.assertEqual(save.pending_changes(), [])
        skeleton = _angelica.read(self.root, armature[anims.PROP_SKELETON])
        checked = 0
        for action in actions[:3]:
            original = _angelica.read(self.root, action[anims.PROP_FILE])
            exported = anims.export_action(original, skeleton, armature, action)
            # Unchanged tracks are copied as they are.
            self.assertEqual(exported, original)
            a, b = _angelica.stck_decode(original), _angelica.stck_decode(exported)
            self.assertEqual([t["id"] for t in a["tracks"]], [t["id"] for t in b["tracks"]])
            fps = float(action[anims.PROP_FPS])
            start, end = float(action[anims.PROP_START]), float(action[anims.PROP_END])
            for track in a["tracks"]:
                rate = track["rotation"]["frame_rate"]
                frames = [f for f in range(anims._dense_frame_count(track["rotation"]))
                          if start <= f / rate * fps <= end]
                with self.subTest(action=action.name, track=track["id"]):
                    qa = track_values(original, track["id"], "rotation", frames)
                    qb = track_values(exported, track["id"], "rotation", frames)
                    dots = np.abs(np.sum(qa * qb, axis=1))
                    self.assertGreater(dots.min(), 0.9999)
                    rate = track["position"]["frame_rate"]
                    frames = [f for f in range(anims._dense_frame_count(track["position"]))
                              if start <= f / rate * fps <= end]
                    pa = track_values(original, track["id"], "position", frames)
                    pb = track_values(exported, track["id"], "position", frames)
                    np.testing.assert_allclose(pb, pa, atol=1e-3)
                    checked += 1
        self.assertGreater(checked, 0)

    def test_edit_save_in_place_and_undo(self):
        armature, actions = self.open_model()
        action = actions[0]
        path = action[anims.PROP_FILE]
        models_before = {p: file_sha1(p) for p in self._packages()}
        original = _angelica.read(self.root, path)

        # Turn the first bone that has a track by 30 degrees on every key.
        bones = _angelica.bon_layout(_angelica.read(self.root, armature[anims.PROP_SKELETON]))["bones"]
        ids = {t["id"] for t in _angelica.stck_decode(original)["tracks"]}
        names = anims._armature_names(armature, bones)
        tracked = {names[i] for i, bone in enumerate(bones) if bone["first_joint"] in ids and names[i]}
        channelbag = anims._channelbag(action, armature)
        curves = [fc for fc in channelbag.fcurves if fc.data_path.endswith("rotation_quaternion")]
        bone_path = next(fc.data_path for fc in curves
                         if fc.data_path.split('"')[1] in tracked)
        quat = {fc.array_index: fc for fc in curves if fc.data_path == bone_path}
        half = math.radians(30) / 2
        for k in range(len(quat[0].keyframe_points)):
            w, x, y, z = (quat[i].keyframe_points[k].co[1] for i in range(4))
            # Rotate about the bone's own Y axis: q * (cos h, 0, sin h, 0).
            c, s = math.cos(half), math.sin(half)
            rotated = (w * c - y * s, x * c - z * s, w * s + y * c, x * s + z * c)
            for i in range(4):
                quat[i].keyframe_points[k].co[1] = rotated[i]
        for fc in quat.values():
            fc.update()
        changes = save.pending_changes()
        self.assertEqual([(c.kind, c.path) for c in changes], [('ANIM', path)])

        save.save(changes, 'IN_PLACE')
        self.assertEqual(save.pending_changes(), [])
        saved = _angelica.read(self.root, path)
        self.assertNotEqual(saved, original)
        changed = 0
        a, b = _angelica.stck_decode(original), _angelica.stck_decode(saved)
        for ta, tb in zip(a["tracks"], b["tracks"]):
            frames = range(anims._dense_frame_count(ta["rotation"]))
            qa = np.array([anims._sample(ta["rotation"], f) for f in frames])
            qb = np.array([anims._sample(tb["rotation"], f) for f in frames])
            if np.abs(np.sum(qa * qb, axis=1)).min() < 0.99:
                changed += 1
        # The turned bone (and nothing else) has new rotations.
        self.assertEqual(changed, 1)

        self.assertEqual(save.undo_last_save(), [])
        self.assertEqual({p: file_sha1(p) for p in self._packages()}, models_before)

    def test_skeleton_rest_pose(self):
        armature, _actions = self.open_model()
        path = armature[anims.PROP_SKELETON]
        original = _angelica.read(self.root, path)
        # Unchanged: the very same bytes.
        self.assertEqual(anims.export_skeleton(original, armature), original)

        # Move the last bone up by 0.1 (Blender Z, the engine's Y) in edit mode.
        bpy.context.view_layer.objects.active = armature
        armature.select_set(True)
        bpy.ops.object.mode_set(mode='EDIT')
        bone = armature.data.edit_bones[-1]
        name = bone.name
        scale = float(armature.get(anims.PROP_SCALE, 1.0))
        bone.head.z += 0.1 * scale
        bone.tail.z += 0.1 * scale
        bpy.ops.object.mode_set(mode='OBJECT')
        changes = [c for c in save.pending_changes() if c.kind == 'BON']
        self.assertEqual([c.path for c in changes], [path])

        packages_before = {p: file_sha1(p) for p in self._packages()}
        save.save(changes, 'IN_PLACE')
        saved = _angelica.read(self.root, path)
        bones = _angelica.bon_layout(saved)["bones"]
        old_bones = _angelica.bon_layout(original)["bones"]
        index = anims._armature_names(armature, bones).index(name)
        # Its bind translation moved by 0.1 on the engine's Y.
        def model_matrix(b):
            return np.linalg.inv(np.frombuffer(b["bone_init"], dtype=np.float32).reshape(4, 4).astype(np.float64))
        delta = model_matrix(bones[index])[3, :3] - model_matrix(old_bones[index])[3, :3]
        np.testing.assert_allclose(delta, [0.0, 0.1, 0.0], atol=1e-4)
        # The rewritten bones chain: relative * parent model matrix == own model matrix.
        for i, b in enumerate(bones):
            if i != index and b["parent"] != index:
                continue
            relative = np.frombuffer(b["relative"], dtype=np.float32).reshape(4, 4).astype(np.float64)
            chained = relative @ model_matrix(bones[b["parent"]]) if b["parent"] >= 0 else relative
            np.testing.assert_allclose(chained, model_matrix(b), atol=2e-3)
        # Other bones keep their bytes.
        for i, (a, b) in enumerate(zip(old_bones, bones)):
            if i != index and b["parent"] != index:
                self.assertEqual(a["relative"], b["relative"])
                self.assertEqual(a["bone_init"], b["bone_init"])

        self.assertEqual(save.undo_last_save(), [])
        self.assertEqual({p: file_sha1(p) for p in self._packages()}, packages_before)

    def _packages(self):
        return sorted(os.path.join(dirpath, name)
                      for dirpath, _dirs, names in os.walk(self.root)
                      for name in names if name.lower().endswith((".pck", ".pkx")))



class AnimSaveV1Test(AnimSaveTest):
    """Version 1 track sets (keys and segments), the only kind older clients read."""
    VERSION = 1


if __name__ == "__main__":
    sys.argv = [__file__] + (sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])
    result = unittest.main(exit=False).result
    sys.exit(0 if result.wasSuccessful() else 1)
