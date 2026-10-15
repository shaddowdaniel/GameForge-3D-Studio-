# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Tests for editing the records of ``elements.data`` (``gameforge_angelica.records`` and the record
editor of ``bl_operators.gameforge_data``).

The ``data\\elements.data`` of the first client in ``GAMEFORGE_TEST_CLIENTS`` is copied into a
temporary client and a temporary server folder; only the copies are written, and the client list
and save history go to a temporary config folder. Run inside the Studio:
  GameForge3DStudio.exe -b --factory-startup --python tests/python/gameforge_records_test.py
"""

import hashlib
import os
import shutil
import sys
import tempfile
import unittest

import bpy

import _angelica
from bl_operators import gameforge_data
from gameforge_angelica import clients, data, records, save

CLIENTS = [p for p in os.environ.get("GAMEFORGE_TEST_CLIENTS", "").split(";") if p.strip()]


def npc_with_shop(db):
    """``(npc id, sell service id)`` of the first NPC with a shop."""
    npc = db.index["NPC_ESSENCE"]
    fields = db.field_index[npc]
    for record, record_id in enumerate(_angelica.elements_ids(db.handle, npc)):
        values = db.record(npc, record)
        if values[fields["id_sell_service"]]:
            return record_id, values[fields["id_sell_service"]]
    return None


@unittest.skipUnless(CLIENTS, "set GAMEFORGE_TEST_CLIENTS to a client folder")
class RecordsTest(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="gameforge-records-test-")
        config = os.path.join(cls.tmp, "config")
        cls._config_dir, cls._clients_path = save._config_dir, clients._config_path
        save._config_dir = lambda: config
        clients._config_path = lambda: os.path.join(config, "gameforge_clients.json")
        cls.original = _angelica.read(CLIENTS[0], "data\\elements.data")
        if cls.original is None:
            raise unittest.SkipTest("the client has no data\\elements.data")
        cls.client = os.path.join(cls.tmp, "client")
        cls.server = os.path.join(cls.tmp, "server")
        for path in (os.path.join(cls.client, "data", "elements.data"), os.path.join(cls.server, "elements.data")):
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as fh:
                fh.write(cls.original)
        clients._save([clients.Client("Teste", cls.client, cls.server)])
        db = data.ClientData(cls.client)
        try:
            cls.npc, cls.shop = npc_with_shop(db)
        finally:
            db.close()

    @classmethod
    def tearDownClass(cls):
        save._config_dir, clients._config_path = cls._config_dir, cls._clients_path
        _angelica.release()
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def setUp(self):
        scene = bpy.context.scene
        scene.gameforge_record_edits.clear()
        scene.gameforge_record.history = ""
        gameforge_data.load_record(bpy.context, self.client, "NPC_ESSENCE", self.npc, remember=False)

    def field(self, name):
        record = bpy.context.scene.gameforge_record
        index = next(i for i, f in enumerate(record.fields) if f.name == name)
        return index, record.fields[index]

    def files(self):
        """SHA-1 of the client's and the server's elements.data (a diff of 25 MB would never end)."""
        result = []
        for path in (os.path.join(self.client, "data", "elements.data"), os.path.join(self.server, "elements.data")):
            with open(path, "rb") as fh:
                result.append(hashlib.sha1(fh.read()).hexdigest())
        return result

    def originals(self):
        return [hashlib.sha1(self.original).hexdigest()] * 2

    def test_links_follow_and_back(self):
        index, shop = self.field("id_sell_service")
        self.assertEqual(shop.link, "NPC_SELL_SERVICE")
        self.assertTrue(shop.link_label)
        self.assertIn('FINISHED', bpy.ops.gameforge.record_follow(index=index))
        record = bpy.context.scene.gameforge_record
        self.assertEqual((record.list_name, record.record_id), ("NPC_SELL_SERVICE", self.shop))
        _index, goods = self.field("pages_1_goods_1_id")
        self.assertEqual(goods.link, data.ITEM)
        if goods.value != "0":
            self.assertTrue(goods.link_label)
        self.assertIn('FINISHED', bpy.ops.gameforge.record_back())
        self.assertEqual((record.list_name, record.record_id), ("NPC_ESSENCE", self.npc))

    def test_edit_save_and_undo(self):
        _index, name = self.field("Name")
        name.value = "GameForge teste"
        self.assertEqual(len(bpy.context.scene.gameforge_record_edits), 1)
        changes = [c for c in save.pending_changes() if c.kind == 'ELEMENTS']
        # The client's elements.data and its server's.
        self.assertEqual(sorted(c.root for c in changes), sorted([self.client, self.server]))
        save.save(changes, 'IN_PLACE')
        client_hash, server_hash = self.files()
        self.assertEqual(client_hash, server_hash)
        db = data.ClientData(self.client)
        try:
            npc = db.index["NPC_ESSENCE"]
            values = db.record(npc, db.find(npc, self.npc))
            self.assertEqual(values[db.field_index[npc]["Name"]], "GameForge teste")
        finally:
            db.close()
        self.assertFalse([c for c in save.pending_changes() if c.kind == 'ELEMENTS'])
        self.assertEqual(save.undo_last_save(), [])
        self.assertEqual(self.files(), self.originals())

    def test_invalid_value_is_not_saved(self):
        _index, kind = self.field("id_type")
        kind.value = "abc"
        self.assertTrue(kind.error)
        changes = [c for c in save.pending_changes() if c.kind == 'ELEMENTS']
        with self.assertRaises(save.SaveError):
            save.save(changes, 'IN_PLACE')
        self.assertEqual(self.files(), self.originals())
        # Back to the file's value: no edit left.
        kind.value = kind.original
        self.assertFalse(bpy.context.scene.gameforge_record_edits)

    def test_duplicate_and_undo(self):
        new_id = records.duplicate(self.client, "NPC_SELL_SERVICE", self.shop, self.server)
        self.assertGreaterEqual(new_id, data.FIRST_NEW_ID)
        client_hash, server_hash = self.files()
        self.assertEqual(client_hash, server_hash)
        db = data.ClientData(self.client)
        try:
            sell = db.index["NPC_SELL_SERVICE"]
            copy = db.record(sell, db.find(sell, new_id))
            source = db.record(sell, db.find(sell, self.shop))
            self.assertEqual(copy[0], new_id)
            self.assertTrue(str(copy[1]).endswith("(cópia)"))
            # The same goods and pages (every field after ID and Name).
            self.assertEqual(copy[2:], source[2:])
        finally:
            db.close()
        self.assertEqual(save.undo_last_save(), [])
        self.assertEqual(self.files(), self.originals())

    def test_value_rules(self):
        self.assertEqual(records.parse_value("int32", 4, " 12 "), 12)
        self.assertEqual(records.parse_value("float", 4, "1,5"), 1.5)
        with self.assertRaises(ValueError):
            records.parse_value("int32", 4, str(2 ** 31))
        with self.assertRaises(ValueError):
            records.parse_value("wstring", 8, "abcd")  # 8 bytes, room for 3 UTF-16 units
        self.assertEqual(records.parse_value("wstring", 8, "abc"), "abc")


if __name__ == "__main__":
    sys.argv = [__file__] + (sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])
    result = unittest.main(exit=False).result
    sys.exit(0 if result.wasSuccessful() else 1)
