# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Tests for saving edited skin meshes (``gameforge_angelica.skins``) into a client.

A small model of the first client in ``GAMEFORGE_TEST_CLIENTS`` (folders separated by ``;``) and
every file it uses are packed into a temporary mini client (new packages written by the Studio
itself, in the format of the client's own packages); the real client is only read. The model is
opened, saved unchanged, edited, saved into the mini client's package and undone. Run inside the
Studio:
  GameForge3DStudio.exe -b --factory-startup --python tests/python/gameforge_skin_save_test.py
"""

import hashlib
import os
import shutil
import struct
import sys
import tempfile
import unittest

import bpy
import numpy as np

import _angelica
from gameforge_angelica import save, skins

CLIENTS = [p for p in os.environ.get("GAMEFORGE_TEST_CLIENTS", "").split(";") if p.strip()]
MAX_FILES = 150
MAX_BYTES = 24 * 1024 * 1024


def file_sha1(path):
    with open(path, "rb") as fh:
        return hashlib.sha1(fh.read()).hexdigest()


def find_model(root, version=9):
    """An ``.ecm`` with few dependencies, one of them a skin of ``version`` with a SKIN mesh."""
    models = os.path.join(root, "models.pck")
    if not os.path.exists(models):
        return None, None
    ecms = [p for p, _size, _s in _angelica.package_entries(models) if p.lower().endswith(".ecm")]
    for path in ecms[:: max(1, len(ecms) // 60)]:
        engine = path if path.lower().startswith("models\\") else "models\\" + path
        deps = _angelica.dependencies(root, engine)
        found = [d for d in deps if d["location"] is not None]
        if not found or len(found) > MAX_FILES or sum(d["location"]["size"] for d in found) > MAX_BYTES:
            continue
        for dep in found:
            if dep["path"].lower().endswith(".ski"):
                layout = _angelica.ski_layout(_angelica.read(root, dep["path"]))
                if layout["version"] == version and any(m["type"] == "SKIN" for m in layout["meshes"]):
                    return engine, found
    return None, None


def build_mini_client(root, deps, dst_root):
    """Pack the files of ``deps`` into packages of the same names under ``dst_root``."""
    packages = {}
    for dep in deps:
        location = dep["location"]
        data = _angelica.read(root, dep["path"])
        if location["kind"] == "PACKED":
            packages.setdefault((location["container"], location["mount"]), []).append((dep["path"], data))
        else:
            path = os.path.join(dst_root, *dep["path"].split("\\"))
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as fh:
                fh.write(data)
    for (pck, mount), files in packages.items():
        dst = os.path.join(dst_root, os.path.relpath(pck, root))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        _angelica.pck_write_patch(pck, mount, dst, files)


def decode_record(data, record):
    """Per triangle corner: position, normal, uv (and bone weights for skin meshes)."""
    begin, _end = record["range"]
    name_len = struct.unpack_from("<i", data, begin)[0]
    offset = begin + 4 + name_len + (12 if record["type"] == "RIGID" else 8)
    vertex_count, index_count = struct.unpack_from("<ii", data, offset)
    offset += 8
    dtype = skins.RIGID_VERTEX if record["type"] == "RIGID" else skins.SKIN_VERTEX
    vertices = np.frombuffer(data, dtype=dtype, count=vertex_count, offset=offset)
    offset += vertices.nbytes
    indices = np.frombuffer(data, dtype="<u2", count=index_count, offset=offset).reshape(-1, 3)
    # Triangles the importer drops: degenerate, out of range or without area.
    keep = (indices[:, 0] != indices[:, 1]) & (indices[:, 1] != indices[:, 2]) & \
        (indices[:, 0] != indices[:, 2]) & (indices.max(axis=1) < vertex_count)
    tris = indices[keep]
    pos = vertices["pos"].astype(np.float64)
    ab, ac = pos[tris[:, 1]] - pos[tris[:, 0]], pos[tris[:, 2]] - pos[tris[:, 0]]
    longest = np.max([np.einsum("ij,ij->i", e, e) for e in (ab, ac, ac - ab)], axis=0)
    tris = tris[np.linalg.norm(np.cross(ab, ac), axis=1) > 1e-6 * longest]
    corners = vertices[tris.ravel()]
    result = {"pos": corners["pos"], "normal": corners["normal"], "uv": corners["uv"]}
    if record["type"] != "RIGID":
        weights = np.zeros((len(corners), 256), dtype=np.float32)
        full = np.column_stack((corners["weights"], 1.0 - corners["weights"].sum(axis=1)))
        for k in range(4):
            np.add.at(weights, (np.arange(len(corners)), corners["bones"][:, k]), np.maximum(full[:, k], 0))
        result["weights"] = weights
    return result


@unittest.skipUnless(CLIENTS, "set GAMEFORGE_TEST_CLIENTS to the client folders to test")
class SkinSaveTest(unittest.TestCase):
    VERSION = 9

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="gameforge-skin-test-")
        cls.config = os.path.join(cls.tmp, "config")
        cls._config_dir = save._config_dir
        save._config_dir = lambda: cls.config
        for root in CLIENTS:
            cls.ecm, deps = find_model(root, cls.VERSION)
            if cls.ecm:
                break
        if not cls.ecm:
            raise unittest.SkipTest("no small skinned model found")
        cls.root = os.path.join(cls.tmp, "element")
        build_mini_client(root, deps, cls.root)
        print("model: {} ({:d} files, .ski v{:d})".format(cls.ecm, len(deps), cls.VERSION))

    @classmethod
    def tearDownClass(cls):
        save._config_dir = cls._config_dir
        _angelica.release()
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def open_model(self):
        for ob in list(bpy.data.objects):
            bpy.data.objects.remove(ob)
        result = bpy.ops.wm.angelica_import_asset(client_root=self.root, engine_path=self.ecm)
        self.assertIn('FINISHED', result)
        for ob in bpy.data.objects:
            if skins.PROP_SKIN_FILE in ob:
                ob[skins.PROP_GEOMETRY] = skins.mesh_fingerprint(ob)
        groups = skins.skin_objects(bpy.data.objects)
        self.assertTrue(groups)
        # Only the skins of the version under test.
        return {key: obs for key, obs in groups.items()
                if _angelica.ski_layout(_angelica.read(*key))["version"] == self.VERSION}

    def test_unchanged_export_matches_the_file(self):
        groups = self.open_model()
        self.assertEqual(save.pending_changes(), [])
        for (root, path), objects in groups.items():
            original = _angelica.read(root, path)
            layout = _angelica.ski_layout(original)
            writable = [ob for ob in objects if layout["meshes"][ob[skins.PROP_SKIN_MESH]]["type"] in skins.WRITABLE_TYPES]
            change = save.Change(root, path, 'SKIN', writable[0], objects=writable)
            exported = skins.export_skin(original, writable, change._skeleton_bone_names)
            new_layout = _angelica.ski_layout(exported)
            self.assertEqual([m["type"] for m in new_layout["meshes"]], [m["type"] for m in layout["meshes"]])
            for ob in writable:
                index = ob[skins.PROP_SKIN_MESH]
                with self.subTest(skin=path, mesh=index):
                    a = decode_record(original, layout["meshes"][index])
                    b = decode_record(exported, new_layout["meshes"][index])
                    self.assertEqual(a["pos"].shape, b["pos"].shape)
                    np.testing.assert_allclose(b["pos"], a["pos"], atol=1e-4)
                    np.testing.assert_allclose(b["uv"], a["uv"], atol=1e-5)
                    # Files carry some zero or non unit normals; the Studio keeps unit normals, with
                    # the precision of its custom normals (well under a degree).
                    length = np.linalg.norm(a["normal"], axis=1)
                    valid = length > 1e-3
                    np.testing.assert_allclose(b["normal"][valid], a["normal"][valid] / length[valid, None],
                                               atol=1e-2)
                    if "weights" in a:
                        np.testing.assert_allclose(b["weights"], a["weights"], atol=1e-4)

    def test_faces_agree_with_the_file_normals(self):
        # The Y/Z swap mirrors the model: faces must still point outward like the file's normals,
        # or the lighting comes from behind and every model looks dark.
        groups = self.open_model()
        for objects in groups.values():
            for ob in objects:
                mesh = ob.data
                agree = sum(1 for face in mesh.polygons for corner in face.loop_indices
                            if mesh.corner_normals[corner].vector.dot(face.normal) > 0.0)
                with self.subTest(mesh=ob.name):
                    self.assertGreater(agree, 0.9 * len(mesh.loops))

    def test_edit_save_in_place_and_undo(self):
        groups = self.open_model()
        (root, path), objects = next(iter(groups.items()))
        original = _angelica.read(root, path)
        layout = _angelica.ski_layout(original)
        ob = next(o for o in objects if layout["meshes"][o[skins.PROP_SKIN_MESH]]["type"] in skins.WRITABLE_TYPES)
        index = ob[skins.PROP_SKIN_MESH]
        models = os.path.join(self.root, "models.pck")
        whole_before = file_sha1(models)

        # Raise the whole mesh by 0.25 (Blender Z is the engine's Y).
        scale = float(ob.get(skins.PROP_SCALE, 1.0))
        for v in ob.data.vertices:
            v.co.z += 0.25 * scale
        changes = save.pending_changes()
        self.assertEqual([(c.kind, c.path) for c in changes], [('SKIN', path)])

        save.save(changes, 'IN_PLACE')
        self.assertEqual(save.pending_changes(), [])
        saved = _angelica.read(root, path)
        saved_layout = _angelica.ski_layout(saved)
        a = decode_record(original, layout["meshes"][index])
        b = decode_record(saved, saved_layout["meshes"][index])
        np.testing.assert_allclose(b["pos"][:, 1] - a["pos"][:, 1], 0.25, atol=1e-4)
        np.testing.assert_allclose(b["pos"][:, [0, 2]], a["pos"][:, [0, 2]], atol=1e-4)
        # The other meshes are the same bytes.
        for i, (m_old, m_new) in enumerate(zip(layout["meshes"], saved_layout["meshes"])):
            if i != index:
                self.assertEqual(original[slice(*m_old["range"])], saved[slice(*m_new["range"])])
        # The saved model opens again.
        self.open_model()

        self.assertEqual(save.undo_last_save(), [])
        self.assertEqual(file_sha1(models), whole_before)


class SkinSaveV8Test(SkinSaveTest):
    """Skins of version 8 index the skeleton's bones directly."""
    VERSION = 8


if __name__ == "__main__":
    sys.argv = [__file__] + (sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])
    result = unittest.main(exit=False).result
    sys.exit(0 if result.wasSuccessful() else 1)
