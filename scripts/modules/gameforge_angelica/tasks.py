# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Editing the quests of a client (``data\\tasks.data``) in the Studio.

Edits are kept in the scene (``Scene.gameforge_task_edits``: client, top level task id, field path,
value as text), like the record edits of ``records``. Saving ("Salvar no Cliente do Jogo") writes
the client's ``data\\tasks.data`` and, when the client has a server folder with a ``tasks.data``,
the server's in the same save: the same bytes when both were the same file, otherwise the same
fields set in the server's own tasks. Tasks that were not edited keep their bytes.

The reading and writing is ``angelica_tasks.cc`` (pack versions 43 to 241) through the
``_angelica.tasks_*`` functions.
"""

__all__ = (
    "TasksChange",
    "TasksFile",
    "changes",
    "duplicate",
    "open_tasks",
    "parse_value",
    "task_name",
)

import os

from . import save

TEXT = "text"
CONTAINERS = ("struct", "array", "item", "bytes")
# The task id keys the XOR of its texts and links the sub tasks: not edited as plain numbers.
READ_ONLY = ("m_ID", "m_ulParent", "m_ulPrevSibling", "m_ulNextSibling", "m_ulFirstChild")
_INTS = {"bool": (0, 1), "u8": (0, 255), "i16": (-2 ** 15, 2 ** 15 - 1), "i32": (-2 ** 31, 2 ** 31 - 1),
         "u32": (0, 2 ** 32 - 1)}
_TRUE = ("1", "true", "sim", "s", "yes")
_FALSE = ("0", "false", "não", "nao", "n", "no")


class TasksFile:
    """An open ``tasks.data``."""

    def __init__(self, data):
        import _angelica
        self.handle = _angelica.tasks_open(data)
        info = _angelica.tasks_info(self.handle)
        self.version = info["version"]
        self.count = info["count"]

    def close(self):
        import _angelica
        if self.handle:
            _angelica.tasks_close(self.handle)
            self.handle = 0

    def __del__(self):
        try:
            self.close()
        except Exception:
            pass

    def list(self):
        """``[(id, name)]`` of the top level tasks, in record order."""
        import _angelica
        return _angelica.tasks_list(self.handle)

    def nodes(self, index):
        import _angelica
        return _angelica.tasks_nodes(self.handle, index)

    def fields(self, index):
        import _angelica
        return _angelica.tasks_fields(self.handle, index)

    def set(self, index, field, value):
        import _angelica
        _angelica.tasks_set(self.handle, index, field, value)

    def duplicate(self, index, first_id=0):
        """Append a copy of quest ``index`` under new ids: ``(new index, [new ids])``."""
        import _angelica
        return _angelica.tasks_duplicate(self.handle, index, first_id)

    def find(self, task_id):
        """``(record index, node)`` of a task or sub task, or None."""
        import _angelica
        return _angelica.tasks_find(self.handle, task_id)

    def to_bytes(self):
        import _angelica
        return _angelica.tasks_bytes(self.handle)


# client root -> (stamp, TasksFile, {id: (record index, name)}).
_open = {}


def open_tasks(root):
    """The client's tasks.data and the names of its top level tasks, opened again when it changes."""
    import _angelica
    location = _angelica.locate(root, "data\\tasks.data")
    if location is None:
        raise FileNotFoundError("o cliente não tem data\\tasks.data")
    stamp = (location["container"], location["size"], os.path.getmtime(location["container"]))
    cached = _open.get(root)
    if cached is None or cached[0] != stamp:
        if cached is not None:
            cached[1].close()
        tasks = TasksFile(_angelica.read(root, "data\\tasks.data"))
        names = {task_id: (index, name) for index, (task_id, name) in enumerate(tasks.list())}
        _open[root] = (stamp, tasks, names)
    return _open[root][1], _open[root][2]


def task_name(root, task_id):
    """Name of a top level task of the client, '' when unknown."""
    try:
        _tasks, names = open_tasks(root)
    except (OSError, ValueError):
        return ""
    found = names.get(task_id)
    return found[1] if found else ""


def parse_value(kind, text):
    """The value of a task field typed as text. Raises ValueError with a message for the user."""
    if kind == TEXT:
        return text
    text = text.strip()
    if kind == "bool":
        if text.lower() in _TRUE:
            return 1
        if text.lower() in _FALSE:
            return 0
        raise ValueError("use 1 (sim) ou 0 (não)")
    if kind in _INTS:
        try:
            value = int(text)
        except ValueError:
            raise ValueError("'{:s}' não é um número inteiro".format(text)) from None
        low, high = _INTS[kind]
        if not low <= value <= high:
            raise ValueError("{:d} fora do intervalo do campo ({:d} a {:d})".format(value, low, high))
        return value
    if kind == "f32":
        try:
            return float(text.replace(",", "."))
        except ValueError:
            raise ValueError("'{:s}' não é um número".format(text)) from None
    raise ValueError("este campo não é editável")


def format_value(kind, value):
    if kind == "f32":
        return "{:.6g}".format(value)
    return "" if value is None else str(value)


def node_fields(quests, index):
    """
    The tasks of quest record ``index`` and its value fields: ``(nodes, [(field, node, path, kind,
    value)])`` where ``node`` indexes ``nodes`` (the task or sub task the field belongs to) and
    ``path`` is the field's path inside that task (sibling sub tasks share their full paths).
    """
    fields = quests.fields(index)
    nodes = quests.nodes(index)
    node_of_field = {node[0]: k for k, node in enumerate(nodes)}
    owner = []
    result = []
    for i, (_name, path, kind, parent, value) in enumerate(fields):
        owner.append(node_of_field.get(i, owner[parent] if parent >= 0 else 0))
        if kind in CONTAINERS:
            continue
        prefix = fields[nodes[owner[i]][0]][1] + "."
        result.append((i, owner[i], path[len(prefix):] if path.startswith(prefix) else path, kind, value))
    return nodes, result


def _apply(quests, edits, where):
    """Set the edited fields into an open tasks.data. Raises save.SaveError."""
    for edit in edits:
        found = quests.find(edit.task_id)
        if found is None:
            raise save.SaveError("a missão {:d} não existe {:s}".format(edit.task_id, where))
        # Looked up again for every edit: a text edit can shift the fields after it.
        nodes, fields = node_fields(quests, found[0])
        node = next((k for k, n in enumerate(nodes) if n[2] == edit.node_id), None)
        field = next((f for f in fields if f[1] == node and f[2] == edit.path), None)
        if field is None:
            raise save.SaveError("a missão {:d} não tem o campo {:s} {:s}".format(edit.node_id, edit.path, where))
        if edit.path.rsplit(".", 1)[-1] in READ_ONLY:
            raise save.SaveError("missão {:d}: {:s} não pode ser editado".format(edit.node_id, edit.path))
        try:
            quests.set(found[0], field[0], parse_value(field[3], edit.value))
        except ValueError as ex:
            raise save.SaveError("missão {:d}, {:s}: {:s}".format(edit.node_id, edit.path, str(ex))) from None


class TasksChange(save.EditsChange):
    """The ``tasks.data`` of a client (or of its server) with the task edits of the scenes."""
    __slots__ = ()
    EDITS = "gameforge_task_edits"
    CLIENT_PATH = "data\\tasks.data"
    SERVER_NAME = "tasks.data"
    KIND = 'TASKS'

    def build(self, file_bytes, edits, where):
        quests = TasksFile(file_bytes)
        try:
            _apply(quests, edits, where)
            return quests.to_bytes()
        finally:
            quests.close()


def changes(scenes):
    """The :class:`TasksChange` of every client with task edits (and of its server)."""
    return TasksChange.changes(scenes)


# The quest name field holds 30 UTF-16 units with its terminator.
_NAME_LEN = 29


def _copy_quest(quests, index, first_id=0):
    """Append a copy of quest ``index`` named "... (cópia)". Returns the new ids. Raises ValueError."""
    new_index, ids = quests.duplicate(index, first_id)
    _nodes, fields = node_fields(quests, new_index)
    field, _node, _path, _kind, name = next(f for f in fields if f[1] == 0 and f[2] == "m_szName")
    suffix = " (cópia)"
    quests.set(new_index, field, name[:_NAME_LEN - len(suffix)].rstrip() + suffix)
    return ids


def duplicate(client, task_id, server=""):
    """
    Copy a quest of the client (with its sub tasks) under new ids, and into the server's
    tasks.data with the same ids when ``server`` is given, saved at once with undo. Returns the new
    quest's id. Raises save.SaveError.
    """
    import _angelica
    original = _angelica.read(client, "data\\tasks.data")
    if original is None:
        raise save.SaveError("o cliente não tem data\\tasks.data")
    quests = TasksFile(original)
    try:
        found = quests.find(task_id)
        if found is None:
            raise save.SaveError("a missão {:d} não existe no cliente".format(task_id))
        try:
            ids = _copy_quest(quests, found[0])
        except ValueError as ex:
            raise save.SaveError(str(ex)) from None
        client_bytes = quests.to_bytes()
    finally:
        quests.close()
    changes = [save.BytesChange(client, "data\\tasks.data", client_bytes, 'TASKS')]
    server_path = os.path.join(server, "tasks.data") if server else ""
    if server_path and os.path.isfile(server_path):
        with open(server_path, "rb") as fh:
            server_original = fh.read()
        if server_original == original:
            server_bytes = client_bytes
        else:
            server_quests = TasksFile(server_original)
            try:
                found = server_quests.find(task_id)
                if found is None:
                    raise save.SaveError("a missão {:d} não existe no tasks.data do servidor".format(task_id))
                try:
                    _copy_quest(server_quests, found[0], ids[0])
                except ValueError as ex:
                    raise save.SaveError("no tasks.data do servidor: " + str(ex)) from None
                server_bytes = server_quests.to_bytes()
            finally:
                server_quests.close()
        change = save.BytesChange(server, "tasks.data", server_bytes, 'TASKS')
        change.folder = "servidor"
        changes.append(change)
    save.save(changes, 'IN_PLACE')
    return ids[0]
