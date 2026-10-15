# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Tests for editing quests (``gameforge_angelica.tasks`` and ``bl_operators.gameforge_tasks``).

The first client in ``GAMEFORGE_TEST_CLIENTS`` whose ``data\\tasks.data`` the field reader knows
is used: its file is copied into a temporary client and a temporary server folder, and only the
copies are written (the client list and save history go to a temporary config folder). Run inside
the Studio:
  GameForge3DStudio.exe -b --factory-startup --python tests/python/gameforge_tasks_test.py
"""

import hashlib
import os
import shutil
import sys
import tempfile
import unittest

import bpy

import _angelica
from bl_operators import gameforge_tasks
from gameforge_angelica import clients, save, tasks

CLIENTS = [p for p in os.environ.get("GAMEFORGE_TEST_CLIENTS", "").split(";") if p.strip()]


def readable_client():
    """``(root, tasks.data bytes, quest index with sub tasks)`` of the first client the reader knows."""
    for root in CLIENTS:
        raw = _angelica.read(root, "data\\tasks.data")
        if raw is None:
            continue
        handle = _angelica.tasks_open(raw)
        try:
            for index in range(min(_angelica.tasks_info(handle)["count"], 2000)):
                try:
                    if len(_angelica.tasks_nodes(handle, index)) > 2:
                        return root, raw, index
                except ValueError:
                    break
        finally:
            _angelica.tasks_close(handle)
    return None


def sha1(path):
    with open(path, "rb") as fh:
        return hashlib.sha1(fh.read()).hexdigest()


@unittest.skipUnless(CLIENTS, "set GAMEFORGE_TEST_CLIENTS to client folders")
class TasksTest(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        found = readable_client()
        if found is None:
            raise unittest.SkipTest("no client whose tasks.data the field reader knows")
        cls.source, raw, cls.index = found
        cls.tmp = tempfile.mkdtemp(prefix="gameforge-tasks-test-")
        config = os.path.join(cls.tmp, "config")
        cls._config_dir, cls._clients_path = save._config_dir, clients._config_path
        save._config_dir = lambda: config
        clients._config_path = lambda: os.path.join(config, "gameforge_clients.json")
        cls.client = os.path.join(cls.tmp, "client")
        cls.server = os.path.join(cls.tmp, "server")
        cls.paths = (os.path.join(cls.client, "data", "tasks.data"), os.path.join(cls.server, "tasks.data"))
        for path in cls.paths:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "wb") as fh:
                fh.write(raw)
        cls.original = hashlib.sha1(raw).hexdigest()
        clients._save([clients.Client("Teste", cls.client, cls.server)])
        quests, names = tasks.open_tasks(cls.client)
        cls.quest = next(task_id for task_id, (index, _name) in names.items() if index == cls.index)
        cls.sub = quests.nodes(cls.index)[1][2]
        print("client", cls.source, "version", quests.version, "quest", cls.quest, "sub task", cls.sub)

    @classmethod
    def tearDownClass(cls):
        save._config_dir, clients._config_path = cls._config_dir, cls._clients_path
        tasks._open.clear()
        _angelica.release()
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def setUp(self):
        bpy.context.scene.gameforge_task_edits.clear()
        gameforge_tasks.load_task(bpy.context, self.client, self.quest)

    def field(self, predicate):
        quest = bpy.context.scene.gameforge_task
        return next(f for f in quest.fields if predicate(f))

    def test_open_and_sub_tasks(self):
        quest = bpy.context.scene.gameforge_task
        self.assertEqual(quest.task_id, self.quest)
        self.assertGreater(len(quest.nodes), 2)
        self.assertTrue(quest.fields)
        self.assertTrue(self.field(lambda f: f.name == "m_ID").locked)
        # Opening a sub task opens its quest at it, with the sub task's own fields.
        self.assertIn('FINISHED', bpy.ops.gameforge.task_open(client=self.client, task_id=self.sub))
        self.assertEqual(quest.task_id, self.quest)
        self.assertEqual(quest.nodes[quest.node_index].task_id, self.sub)
        self.assertEqual(self.field(lambda f: f.name == "m_ID").value, str(self.sub))

    def test_edit_save_and_undo(self):
        name = self.field(lambda f: f.name == "m_szName")
        name.value = "GameForge teste"
        number = self.field(lambda f: f.kind in ("i32", "u32") and not f.locked and f.value == "0")
        number.value = "7"
        # The field items are made again when the quest is reopened: keep the path, not the item.
        number_path = number.path
        changes = [c for c in save.pending_changes() if c.kind == 'TASKS']
        self.assertEqual(sorted(c.root for c in changes), sorted([self.client, self.server]))
        save.save(changes, 'IN_PLACE')
        self.assertEqual(sha1(self.paths[0]), sha1(self.paths[1]))
        self.assertNotEqual(sha1(self.paths[0]), self.original)
        self.assertFalse([c for c in save.pending_changes() if c.kind == 'TASKS'])
        # The saved file has the new values (the editor reopens the changed file).
        gameforge_tasks.load_task(bpy.context, self.client, self.quest)
        self.assertEqual(self.field(lambda f: f.name == "m_szName").value, "GameForge teste")
        self.assertEqual(self.field(lambda f: f.path == number_path).value, "7")
        self.assertEqual(save.undo_last_save(), [])
        self.assertEqual([sha1(p) for p in self.paths], [self.original] * 2)

    def test_duplicate_and_undo(self):
        quests, names = tasks.open_tasks(self.client)
        original_nodes = quests.nodes(names[self.quest][0])
        new_id = tasks.duplicate(self.client, self.quest, self.server)
        self.assertEqual(sha1(self.paths[0]), sha1(self.paths[1]))
        quests, names = tasks.open_tasks(self.client)
        self.assertIn(new_id, names)
        self.assertTrue(names[new_id][1].endswith("(cópia)"))
        nodes = quests.nodes(names[new_id][0])
        # Every task of the copy has a new id, and the texts decode with them.
        self.assertEqual([n[2] for n in nodes], list(range(new_id, new_id + len(original_nodes))))
        self.assertEqual([n[3] for n in nodes[1:]], [n[3] for n in original_nodes[1:]])
        self.assertEqual([n[1] for n in nodes], [n[1] for n in original_nodes])
        self.assertEqual(save.undo_last_save(), [])
        self.assertEqual([sha1(p) for p in self.paths], [self.original] * 2)

    def test_invalid_value_is_not_saved(self):
        number = self.field(lambda f: f.kind == "i32" and not f.locked)
        number.value = "abc"
        self.assertTrue(number.error)
        with self.assertRaises(save.SaveError):
            save.save([c for c in save.pending_changes() if c.kind == 'TASKS'], 'IN_PLACE')
        self.assertEqual([sha1(p) for p in self.paths], [self.original] * 2)


if __name__ == "__main__":
    sys.argv = [__file__] + (sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])
    result = unittest.main(exit=False).result
    sys.exit(0 if result.wasSuccessful() else 1)
