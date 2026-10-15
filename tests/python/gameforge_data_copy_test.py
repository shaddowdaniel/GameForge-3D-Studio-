# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Tests for copying game data records between clients (``gameforge_angelica.data``).

The first two clients of ``GAMEFORGE_TEST_CLIENTS`` (folders separated by ``;``) whose
``elements.data`` the Studio reads are the source and the target; the target's file is copied to a
temporary client folder and only the copy changes. Run inside the Studio:
  GameForge3DStudio.exe -b --factory-startup --python tests/python/gameforge_data_copy_test.py
"""

import os
import shutil
import sys
import tempfile
import time
import unittest

import numpy as np

import _angelica
from gameforge_angelica import data, icons, save, transfer

sys.path.append(os.path.dirname(__file__))
from gameforge_skin_save_test import build_mini_client, find_model  # noqa: E402

CLIENTS = [p for p in os.environ.get("GAMEFORGE_TEST_CLIENTS", "").split(";") if p.strip()]


def readable(root):
    try:
        data.ClientData(root).close()
        return True
    except (ValueError, FileNotFoundError):
        return False


@unittest.skipUnless(len(CLIENTS) >= 2, "set GAMEFORGE_TEST_CLIENTS to two clients")
class DataCopyTest(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        roots = [r for r in CLIENTS if readable(r)]
        if len(roots) < 2:
            raise unittest.SkipTest("need two clients whose elements.data the Studio reads")
        cls.src_root = roots[0]
        cls.tmp = tempfile.mkdtemp(prefix="gameforge-data-test-")
        cls.config = os.path.join(cls.tmp, "config")
        cls._config_dir = save._config_dir
        save._config_dir = lambda: cls.config
        cls.dst_root = os.path.join(cls.tmp, "element")
        # A mini target client: packages of a few of its files (a model, the item descriptions and
        # the icon atlases), and its elements.data.
        _ecm, deps = find_model(roots[1], 9)
        extra = ["configs\\item_ext_desc.txt", "configs\\item_color.txt"] + \
            [base + ext for base in icons.ATLASES for ext in (".txt", ".dds")]
        deps = deps + [{"path": p, "location": _angelica.locate(roots[1], p)}
                       for p in extra if _angelica.locate(roots[1], p)]
        build_mini_client(roots[1], deps, cls.dst_root)
        os.makedirs(os.path.join(cls.dst_root, "data"), exist_ok=True)
        # The target's server: a data folder with the same elements.data.
        cls.server = os.path.join(cls.tmp, "gamed", "config")
        os.makedirs(cls.server)
        cls.dst_original = _angelica.read(roots[1], "data\\elements.data")
        with open(os.path.join(cls.dst_root, "data", "elements.data"), "wb") as fh:
            fh.write(cls.dst_original)
        with open(os.path.join(cls.server, "elements.data"), "wb") as fh:
            fh.write(cls.dst_original)
        print("copy records from", cls.src_root, "into a copy of", roots[1])

    @classmethod
    def tearDownClass(cls):
        save._config_dir = cls._config_dir
        _angelica.release()
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def weapon_with_addons(self, src):
        weapons = src.index["WEAPON_ESSENCE"]
        fields = src.field_index[weapons]
        for record in range(src.lists[weapons]["count"] - 1, -1, -1):
            values = src.record(weapons, record)
            if values[fields["addons_1_id_addon"]] > 0 and values[fields["addons_2_id_addon"]] > 0:
                return values
        self.fail("no weapon with addons")

    def test_copy_weapon_with_links(self):
        src = data.ClientData(self.src_root)
        dst = data.ClientData(self.dst_root)
        weapon = self.weapon_with_addons(src)
        t0 = time.time()
        plan = data.plan_record_copy(src, dst, "WEAPON_ESSENCE", weapon[0])
        out = data.apply_record_copy(src, dst, plan)
        print("plan + apply {:.1f}s: added {}, shared {}, files {}, cleared {}".format(
            time.time() - t0, plan.added[:6], plan.shared[:6], plan.files[:3], plan.cleared[:3]))

        new_id = plan.ids[("WEAPON_ESSENCE", weapon[0])]
        self.assertGreaterEqual(new_id, data.FIRST_NEW_ID)
        result = data.ClientData(self.dst_root, out)
        self.assertEqual(result.to_bytes(), out)
        weapons = result.index["WEAPON_ESSENCE"]
        record = result.find(weapons, new_id)
        self.assertGreaterEqual(record, 0)
        values = result.record(weapons, record)
        fields = result.field_index[weapons]
        src_fields = src.field_index[src.index["WEAPON_ESSENCE"]]
        self.assertEqual(values[fields["Name"]], weapon[src_fields["Name"]])
        self.assertEqual(values[fields["file_model_right"]], weapon[src_fields["file_model_right"]])
        # Addons point at the copied (or shared) addon records of the target.
        addons = result.index["EQUIPMENT_ADDON"]
        for slot in (1, 2):
            old = weapon[src_fields["addons_{:d}_id_addon".format(slot)]]
            new = values[fields["addons_{:d}_id_addon".format(slot)]]
            self.assertEqual(new, plan.ids[("EQUIPMENT_ADDON", old)])
            addon = result.record(addons, result.find(addons, new))
            original = src.record(src.index["EQUIPMENT_ADDON"], src.find(src.index["EQUIPMENT_ADDON"], old))
            self.assertEqual(addon[1:], original[1:])
        # The sub type is the target's own record, or copied when the target has no such type.
        old_sub = weapon[src_fields["id_sub_type"]]
        new_sub = values[fields["id_sub_type"]]
        self.assertEqual(new_sub, plan.ids[("WEAPON_SUB_TYPE", old_sub)])
        self.assertGreaterEqual(result.find(result.index["WEAPON_SUB_TYPE"], new_sub), 0)
        if ("WEAPON_SUB_TYPE", old_sub) in plan.shared:
            self.assertEqual(new_sub, old_sub)
        # Everything that was in the target is still there, unchanged.
        before = data.ClientData(self.dst_root, self.dst_original)
        for i, entry in enumerate(before.lists):
            if entry["count"] and entry["fields"] and entry["fields"][0][1] != "raw":
                self.assertEqual(result.lists[i]["count"],
                                 entry["count"] + sum(1 for n, _o, _new in plan.added if n == entry["name"]))
                self.assertEqual(result.record(i, 0), before.record(i, 0))
        src.close()
        dst.close()
        result.close()
        before.close()

    def test_copy_record_with_files_and_undo(self):
        src = data.ClientData(self.src_root)
        weapon = self.weapon_with_addons(src)
        src.close()
        before = {}
        for d, _dirs, names in list(os.walk(self.dst_root)) + list(os.walk(self.server)):
            for n in names:
                with open(os.path.join(d, n), "rb") as fh:
                    before[os.path.join(d, n)] = fh.read()
        record_copy = transfer.plan_record(self.src_root, "WEAPON_ESSENCE", weapon[0], self.dst_root)
        new_files = [i for i in record_copy.items if i.status == 'NEW']
        self.assertTrue(new_files)
        print("extra files:", sorted(record_copy.extra), "notes:", record_copy.notes)
        transfer.copy_record(record_copy, include_unsupported=True, server_folder=self.server)
        # The server got the same database as the client.
        with open(os.path.join(self.server, "elements.data"), "rb") as fh:
            self.assertEqual(fh.read(), _angelica.read(self.dst_root, "data\\elements.data"))
        new_id = record_copy.data.ids[("WEAPON_ESSENCE", weapon[0])]
        # The description of the item, under its new id.
        if "configs\\item_ext_desc.txt" in record_copy.extra:
            text = _angelica.read(self.dst_root, "configs\\item_ext_desc.txt").decode("utf-16")
            self.assertIn("\n{:d}\t".format(new_id), text.replace("\r\n", "\n"))
        # The icon, in both atlases, with the source picture.
        result_data = data.ClientData(self.dst_root)
        weapons = result_data.index["WEAPON_ESSENCE"]
        file_icon = result_data.record(weapons, result_data.find(weapons, new_id))[
            result_data.field_index[weapons]["file_icon"]]
        result_data.close()
        src = data.ClientData(self.src_root)
        source_icon = weapon[src.field_index[src.index["WEAPON_ESSENCE"]]["file_icon"]]
        src.close()
        source_atlas = icons.IconAtlas(self.src_root, icons.ATLASES[0])
        target_atlas = icons.IconAtlas(self.dst_root, icons.ATLASES[0])
        src_index = source_atlas.find(source_icon)
        dst_index = target_atlas.find(file_icon)
        self.assertIsNotNone(dst_index, file_icon)
        if src_index is not None:
            difference = np.abs(target_atlas.pixels(dst_index).astype(np.int16) - source_atlas.pixels(src_index))
            self.assertLessEqual(difference.mean(), 4.0)
        # The weapon is in the target's database, its model files in the target's packages.
        result = data.ClientData(self.dst_root)
        self.assertGreaterEqual(result.find(result.index["WEAPON_ESSENCE"], new_id), 0)
        result.close()
        for item in new_files:
            self.assertIsNotNone(_angelica.read(self.dst_root, item.path), item.path)
        # Undo gives the client back as it was.
        self.assertEqual(save.undo_last_save(), [])
        after = {}
        for d, _dirs, names in list(os.walk(self.dst_root)) + list(os.walk(self.server)):
            for n in names:
                with open(os.path.join(d, n), "rb") as fh:
                    after[os.path.join(d, n)] = fh.read()
        self.assertEqual(after, before)


if __name__ == "__main__":
    sys.argv = [__file__] + (sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])
    result = unittest.main(exit=False).result
    sys.exit(0 if result.wasSuccessful() else 1)
