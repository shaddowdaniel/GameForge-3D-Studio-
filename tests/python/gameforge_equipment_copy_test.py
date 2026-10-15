# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Tests for copying armors between clients: the armor record and its skin sets (built from
``realname`` like the client does) go into the folder the target client keeps equipment in.

The first client of ``GAMEFORGE_TEST_CLIENTS`` (folders separated by ``;``) is the source, the
second the target; the target is a temporary mini client with its own elements.data, a model and
one of its default armor skins (so its equipment folder is known). Run inside the Studio:
  GameForge3DStudio.exe -b --factory-startup --python tests/python/gameforge_equipment_copy_test.py
"""

import hashlib
import os
import shutil
import sys
import tempfile
import unittest

import _angelica
from gameforge_angelica import data, save, transfer

sys.path.append(os.path.dirname(__file__))
from gameforge_skin_save_test import build_mini_client, find_model  # noqa: E402

CLIENTS = [p for p in os.environ.get("GAMEFORGE_TEST_CLIENTS", "").split(";") if p.strip()]


def tree_hashes(folder):
    result = {}
    for d, _dirs, names in os.walk(folder):
        for n in names:
            with open(os.path.join(d, n), "rb") as fh:
                result[os.path.join(d, n)] = hashlib.sha1(fh.read()).hexdigest()
    return result


@unittest.skipUnless(len(CLIENTS) >= 2, "set GAMEFORGE_TEST_CLIENTS to a source and a target client")
class EquipmentCopyTest(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.src_root, target = CLIENTS[0], CLIENTS[1]
        cls.tmp = tempfile.mkdtemp(prefix="gameforge-equipment-test-")
        cls.config = os.path.join(cls.tmp, "config")
        cls._config_dir = save._config_dir
        save._config_dir = lambda: cls.config
        cls.dst_root = os.path.join(cls.tmp, "element")
        target_dir = transfer.equipment_root(target)
        if target_dir is None:
            raise unittest.SkipTest("the target client has no equipment folder")
        default = target_dir + "\\男\\默认上衣\\武侠男默认上衣一级.ski"
        _ecm, deps = find_model(target, 9)
        deps = deps + [{"path": default, "location": _angelica.locate(target, default)}]
        build_mini_client(target, deps, cls.dst_root)
        os.makedirs(os.path.join(cls.dst_root, "data"))
        with open(os.path.join(cls.dst_root, "data", "elements.data"), "wb") as fh:
            fh.write(_angelica.read(target, "data\\elements.data"))
        transfer._target_versions[cls.dst_root] = transfer.target_versions(target)
        print("armor from", cls.src_root, "({}) into a mini client of {} ({})".format(
            transfer.equipment_root(cls.src_root), target, target_dir))

    @classmethod
    def tearDownClass(cls):
        save._config_dir = cls._config_dir
        _angelica.release()
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def armor_with_skins(self):
        src = data.ClientData(self.src_root)
        armors = src.index["ARMOR_ESSENCE"]
        fields = src.field_index[armors]
        try:
            for record in range(src.lists[armors]["count"] - 1, -1, -1):
                values = src.record(armors, record)
                realname = values[fields["realname"]]
                if realname and realname != "0" and transfer.equipment_skins(
                        self.src_root, self.dst_root, "ARMOR_ESSENCE", realname,
                        values[fields["character_combo_id"]], -1):
                    return values[0], realname
        finally:
            src.close()
        self.skipTest("no armor with skins in the source")

    def test_copy_armor_with_skin_sets(self):
        armor_id, realname = self.armor_with_skins()
        before = tree_hashes(self.dst_root)
        record_copy = transfer.plan_record(self.src_root, "ARMOR_ESSENCE", armor_id, self.dst_root)
        skins = [i for i in record_copy.items if i.path.lower().endswith(".ski") and realname.lower() in i.path.lower()]
        self.assertTrue(skins)
        dst_dir = transfer.equipment_root(self.dst_root)
        for item in skins:
            self.assertTrue(item.target.lower().startswith(dst_dir.lower()), item.target)
        print("armor", armor_id, realname, "skins", len(skins), "converted",
              sum(1 for i in skins if i.convert), "files", len(record_copy.items))
        transfer.copy_record(record_copy, include_unsupported=True)
        for item in skins:
            data_bytes = _angelica.read(self.dst_root, item.target)
            self.assertIsNotNone(data_bytes, item.target)
            layout = _angelica.ski_layout(data_bytes)
            self.assertLessEqual(layout["version"], 9)
        # The armor is in the target's database under its new id.
        result = data.ClientData(self.dst_root)
        new_id = record_copy.data.ids[("ARMOR_ESSENCE", armor_id)]
        self.assertGreaterEqual(result.find(result.index["ARMOR_ESSENCE"], new_id), 0)
        result.close()
        self.assertEqual(save.undo_last_save(), [])
        self.assertEqual(tree_hashes(self.dst_root), before)


if __name__ == "__main__":
    sys.argv = [__file__] + (sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])
    result = unittest.main(exit=False).result
    sys.exit(0 if result.wasSuccessful() else 1)
