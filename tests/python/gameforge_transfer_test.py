# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Tests for copying assets between clients (``gameforge_angelica.transfer``).

The source is a newer client and the target an older one, both from ``GAMEFORGE_TEST_CLIENTS``
(folders separated by ``;``): the first client whose models use version 2 track sets is the source,
the first one using only version 1 track sets the target. The target is a temporary mini client
packed from one of its own models; the real clients are only read. Run inside the Studio:
  GameForge3DStudio.exe -b --factory-startup --python tests/python/gameforge_transfer_test.py
"""

import hashlib
import os
import shutil
import sys
import tempfile
import unittest

import bpy
import numpy as np

import _angelica
from gameforge_angelica import anims, save, transfer

sys.path.append(os.path.dirname(__file__))
from gameforge_skin_save_test import build_mini_client, find_model  # noqa: E402

CLIENTS = [p for p in os.environ.get("GAMEFORGE_TEST_CLIENTS", "").split(";") if p.strip()]
MAX_FILES = 120
MAX_BYTES = 24 * 1024 * 1024


def file_sha1(path):
    with open(path, "rb") as fh:
        return hashlib.sha1(fh.read()).hexdigest()


def model_with_v2_tracks(root):
    models = os.path.join(root, "models.pck")
    ecms = [p for p, _size, _s in _angelica.package_entries(models) if p.lower().endswith(".ecm")]
    for path in ecms[:: max(1, len(ecms) // 300)]:
        engine = path if path.lower().startswith("models\\") else "models\\" + path
        deps = [d for d in _angelica.dependencies(root, engine) if d["location"] is not None]
        if not deps or len(deps) > MAX_FILES or sum(d["location"]["size"] for d in deps) > MAX_BYTES:
            continue
        stcks = [d["path"] for d in deps if d["path"].lower().endswith(".stck")]
        if stcks and any(_angelica.stck_decode(_angelica.read(root, p))["version"] == 2 for p in stcks):
            return engine
    return None


@unittest.skipUnless(len(CLIENTS) >= 2, "set GAMEFORGE_TEST_CLIENTS to a newer and an older client")
class TransferTest(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="gameforge-transfer-test-")
        cls.config = os.path.join(cls.tmp, "config")
        cls._config_dir = save._config_dir
        save._config_dir = lambda: cls.config
        versions = {root: transfer.target_versions(root) for root in CLIENTS}
        cls.src = next((r for r in CLIENTS if versions[r].get(".stck", 0) >= 2), None)
        old = next((r for r in CLIENTS if versions[r].get(".stck", 99) == 1), None)
        if cls.src is None or old is None:
            raise unittest.SkipTest("need a client with v2 and one with only v1 track sets")
        cls.ecm = model_with_v2_tracks(cls.src)
        if cls.ecm is None:
            raise unittest.SkipTest("no small model with v2 track sets")
        # The target: a mini client of the older client, which reads what that client reads.
        target_ecm, target_deps = find_model(old, 9)
        cls.dst = os.path.join(cls.tmp, "element")
        build_mini_client(old, target_deps, cls.dst)
        transfer._target_versions[cls.dst] = versions[old]
        print("copy {} from {} to a mini client of {} ({})".format(cls.ecm, cls.src, old, versions[old]))

    @classmethod
    def tearDownClass(cls):
        save._config_dir = cls._config_dir
        _angelica.release()
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def packages(self):
        return {os.path.join(d, n): file_sha1(os.path.join(d, n))
                for d, _dirs, names in os.walk(self.dst) for n in names if n.lower().endswith((".pck", ".pkx"))}

    def test_copy_model_with_conversion_and_undo(self):
        items = transfer.plan(self.src, self.ecm, self.dst)
        new = [i for i in items if i.status == 'NEW']
        converted = [i for i in items if i.convert]
        self.assertTrue(new)
        self.assertTrue(any(i.path.lower().endswith(".stck") and i.convert == (2, 1) for i in converted))
        before = self.packages()

        # Newer versions too: the Studio itself reads them, so the copy can be opened below.
        written = transfer.copy(self.src, self.dst, items, include_unsupported=True, mode='IN_PLACE')
        self.assertTrue(written)
        print("copied {:d} files, converted {:d}, newer than the target reads: {}".format(
            len(new), len(converted), sorted({os.path.splitext(i.path)[1] for i in items if i.unsupported})))
        for item in new:
            data = _angelica.read(self.dst, item.path)
            self.assertIsNotNone(data, item.path)
            original = _angelica.read(self.src, item.path)
            if not item.convert:
                self.assertEqual(data, original, item.path)
            elif item.path.lower().endswith(".ecm"):
                # Version 67 layout: none of the 68-71 lines, everything else as it was.
                text = data.decode("gbk", "replace")
                self.assertIn("Version: 67", text.splitlines()[0])
                for key in ("AudioEventGroupEnable:", "ParticleBonesCount:", "GfxUseFixedPoint:"):
                    self.assertNotIn(key, text, item.path)
                old_lines = original.decode("gbk", "replace").splitlines()
                kept = [line for line in old_lines[1:] if not line.startswith(
                    ("AudioEventGroupEnable:", "ParticleBonesCount:", "GfxUseFixedPoint:"))]
                self.assertLessEqual(len(text.splitlines()) - 1, len(kept))
            elif item.path.lower().endswith(".stck"):
                a, b = _angelica.stck_decode(original), _angelica.stck_decode(data)
                self.assertEqual(b["version"], 1)
                for ta, tb in zip(a["tracks"], b["tracks"]):
                    for kind in ("position", "rotation"):
                        n = anims._dense_frame_count(ta[kind])
                        va = np.array([anims._sample(ta[kind], f) for f in range(n)])
                        vb = np.array([anims._sample(tb[kind], f) for f in range(n)])
                        np.testing.assert_allclose(vb, va, atol=1e-6, err_msg=item.path)
        # A second plan finds everything in place.
        again = transfer.plan(self.src, self.ecm, self.dst)
        self.assertFalse([i.path for i in again if i.status == 'NEW'])

        # The copied model opens from the target client.
        for ob in list(bpy.data.objects):
            bpy.data.objects.remove(ob)
        self.assertIn('FINISHED', bpy.ops.wm.angelica_import_asset(client_root=self.dst, engine_path=self.ecm))
        self.assertTrue([ob for ob in bpy.data.objects if ob.type == 'MESH'])
        self.assertTrue([a for a in bpy.data.actions if a.get(anims.PROP_FILE)])

        self.assertEqual(save.undo_last_save(), [])
        self.assertEqual(self.packages(), before)

    def test_skin_tangents_dropped(self):
        for root in CLIENTS:
            models = os.path.join(root, "models.pck")
            skins = [p for p, size, _s in _angelica.package_entries(models) if p.lower().endswith(".ski")]
            for path in skins[:: max(1, len(skins) // 400)]:
                data = _angelica.package_read(models, path)
                if transfer._file_version(path, data) not in (100, 101):
                    continue
                converted = transfer._ski_to_v9(data)
                a, b = _angelica.ski_layout(data), _angelica.ski_layout(converted)
                self.assertEqual(b["version"], 9)
                self.assertEqual([(m["type"], m["vertices"], m["indices"]) for m in a["meshes"]],
                                 [(m["type"], m["vertices"], m["indices"]) for m in b["meshes"]])
                self.assertEqual(a["bone_names"], b["bone_names"])
                return
        self.skipTest("no skin with tangents found")


if __name__ == "__main__":
    sys.argv = [__file__] + (sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])
    result = unittest.main(exit=False).result
    sys.exit(0 if result.wasSuccessful() else 1)
