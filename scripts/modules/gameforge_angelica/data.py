# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
The game database of a client (``element\\data\\elements.data``) and copying its records into
another client with everything they use.

Records reference each other by id: an item its type and addons, an NPC its services, a monster
its drops. Which field points where comes from built-in rules on the field names, extended by the
link table of Rody's Angelica Editor (``PW_types.csv``) when it is available. Ids are unique per
id space (the essence space holds items, NPCs, monsters, ...; addons have their own).

Copying a record copies the records it reaches through those links (type and sub type records are
shared with the target when it has them), gives every copied record a new id from 200001 up
(unless the target already has the very same record), rewrites the references among them, and
lists the files they name (models, effects, icons) for the file copy.
"""

__all__ = (
    "ClientData",
    "DataCopy",
    "LinkRules",
    "apply_record_copy",
    "plan_record_copy",
)

import os
import re

FIRST_NEW_ID = 200001


def next_free_id(used):
    """The id a new record gets: past every id in use (``used``), from :data:`FIRST_NEW_ID` on."""
    return max([FIRST_NEW_ID - 1] + [i for i in used if i < 0x7FFFFFFF]) + 1

# Lists whose ``realname`` names a set of skins rather than a file (see ``transfer.equipment_skins``).
EQUIPMENT_LISTS = ("ARMOR_ESSENCE", "FASHION_ESSENCE")
ITEM = "<item>"
FILE = "<file>"
# A quest of tasks.data (not a list of elements.data).
TASK = "<task>"
# Pseudo types of the link table: any item, addons, file paths.
_ITEM_TYPES = {"matters", "matters_and_equip_suite"}
_ADDON_TYPES = {"equip_addon", "addon_param1", "addon_param2", "addon_param3"}
_FILE_TYPES = {"file_model", "file_icon", "file_icon_tga", "file_gfx", "file_sfx"}
_LINK_TABLE_ENV = "GAMEFORGE_LINK_TABLE"
_LINK_TABLE_CANDIDATES = (
    r"D:\Atom Sec Labs\ATOM GameForge\Rody's Angelica Editor\bin\Release\net481\configs\PW\PW_types.csv",
)


class ClientData:
    """An open ``elements.data`` of a client."""

    def __init__(self, root, data=None):
        import _angelica
        self.root = root
        if data is None:
            data = _angelica.read(root, "data\\elements.data")
            if data is None:
                raise FileNotFoundError("o cliente não tem data\\elements.data")
        self.handle = _angelica.elements_open(data)
        info = _angelica.elements_info(self.handle)
        self.version = info["version"]
        self.lists = info["lists"]
        self.index = {entry["name"]: i for i, entry in enumerate(self.lists)}
        self.field_index = [{f[0]: k for k, f in enumerate(entry["fields"])} for entry in self.lists]

    def close(self):
        import _angelica
        if self.handle:
            _angelica.elements_close(self.handle)
            self.handle = 0

    def __del__(self):
        try:
            self.close()
        except Exception:
            pass

    def record(self, list_index, record):
        import _angelica
        return _angelica.elements_record(self.handle, list_index, record)

    def find(self, list_index, record_id):
        import _angelica
        return _angelica.elements_find(self.handle, list_index, record_id)

    def find_in_space(self, space, record_id):
        """``(list index, record index)`` of an id in an id space, or None."""
        import _angelica
        return _angelica.elements_find_in_space(self.handle, space, record_id)

    def ids_in_space(self, space):
        """Every id used in an id space (for picking new ones)."""
        import _angelica
        used = set()
        for i, entry in enumerate(self.lists):
            if entry["id_space"] == space and entry["fields"] and entry["fields"][0][0] == "ID":
                used.update(_angelica.elements_ids(self.handle, i))
        return used

    def to_bytes(self):
        import _angelica
        return _angelica.elements_bytes(self.handle)


class LinkRules:
    """Which fields of which lists reference which list (or any item, or a file)."""

    def __init__(self, table_path=None):
        self.table = {}
        path = table_path or os.environ.get(_LINK_TABLE_ENV) or next(
            (p for p in _LINK_TABLE_CANDIDATES if os.path.isfile(p)), None)
        if path and os.path.isfile(path):
            self._load_table(path)

    def _load_table(self, path):
        current = None
        with open(path, encoding="utf-8", errors="replace") as fh:
            for line in fh:
                line = line.strip()
                if line.startswith("#"):
                    current = self.table.setdefault(line[1:], [])
                elif current is not None and ";" in line:
                    field, kind = line.split(";", 1)
                    if field.startswith("~"):
                        pattern = re.compile("^" + re.escape(field[1:]).replace(r"\$", r"\d+") + "$")
                    else:
                        pattern = re.compile("^" + re.escape(field) + "$")
                    current.append((pattern, kind.strip()))

    def target(self, data, list_name, field_name, field_type):
        """A list name, ``ITEM``, ``FILE``, ``TASK`` or None for a field."""
        # Quests the NPC services give and take (before the table, which does not name tasks.data).
        if re.match(r"^(tasks_data_\d+_id_tasks|tasks_\d+_tasks_data_\d+_id_task)$", field_name):
            return TASK
        for pattern, kind in self.table.get(list_name, ()):
            if pattern.match(field_name):
                if kind in data.index:
                    return kind
                if kind in _ITEM_TYPES:
                    return ITEM
                if kind in _ADDON_TYPES and "EQUIPMENT_ADDON" in data.index and kind == "equip_addon":
                    return "EQUIPMENT_ADDON"
                if kind in _FILE_TYPES and field_type == "string":
                    return FILE
                return None
        # Built-in rules on the field names (the engine's own struct member names).
        if field_type == "string" and field_name.startswith("file_"):
            return FILE
        if field_type in ("string", "wstring", "raw", "float", "double"):
            return None
        base = list_name[:-len("_ESSENCE")] if list_name.endswith("_ESSENCE") else None
        if base and field_name == "id_major_type" and base + "_MAJOR_TYPE" in data.index:
            return base + "_MAJOR_TYPE"
        if base and field_name == "id_sub_type" and base + "_SUB_TYPE" in data.index:
            return base + "_SUB_TYPE"
        if base and field_name == "id_type" and base + "_TYPE" in data.index:
            return base + "_TYPE"
        # Items sold, made, asked for or given by the NPC services.
        if re.match(r"^(pages_\d+_goods_\d+_id|pages_\d+_id_goods_\d+|id_goods_\d+|goods_\d+_item_id|"
                    r"id_object_need(_\d+)?|need_item_tid|cost_item|itemList_\d+_itemID)$", field_name):
            return ITEM
        if re.match(r"^(addons_\d+_id_addon|rands_\d+_id_rand|id_addon\d*)$", field_name):
            return "EQUIPMENT_ADDON" if "EQUIPMENT_ADDON" in data.index else None
        match = re.match(r"^id_(\w+)_service$", field_name)
        if match and "NPC_" + match.group(1).upper() + "_SERVICE" in data.index:
            return "NPC_" + match.group(1).upper() + "_SERVICE"
        if field_name == "id_src_monster" and "MONSTER_ESSENCE" in data.index:
            return "MONSTER_ESSENCE"
        return None

    def links(self, data, list_index):
        """``[(field index, target)]`` of a list (cached per list)."""
        key = (id(data), list_index)
        cache = self.__dict__.setdefault("_cache", {})
        if key not in cache:
            entry = data.lists[list_index]
            cache[key] = [(k, t) for k, (name, kind, _size) in enumerate(entry["fields"])
                          if k > 0 and (t := self.target(data, entry["name"], name, kind))]
        return cache[key]


def _is_type_list(name):
    return name.endswith(("_TYPE", "_MAJOR_TYPE", "_SUB_TYPE"))


class DataCopy:
    """The records a copy brings into the target, with their new ids, and the files they name."""

    def __init__(self):
        # (list name, old id) -> new id in the target.
        self.ids = {}
        # Records written into the target: (list name, old id, new id).
        self.added = []
        # Records the target already has (shared): (list name, id).
        self.shared = []
        # File paths named by the copied records.
        self.files = []
        # References the copy cannot follow (skills, quests, unknown lists): (list, field, value).
        self.unresolved = []
        # Item references set to 0 because the target has no such item: (list, field, value).
        self.cleared = []
        # Armor and fashion skin sets: (list name, realname, character_combo_id or -1, gender or -1).
        self.equipment = []


def _records_equal(src, src_list, src_values, dst, dst_list, dst_values):
    src_fields = src.lists[src_list]["fields"]
    dst_names = dst.field_index[dst_list]
    for k, (name, _kind, _size) in enumerate(src_fields):
        if k == 0:
            continue
        j = dst_names.get(name)
        if j is None or dst_values[j] != src_values[k]:
            return False
    return True


def plan_record_copy(src, dst, list_name, record_id, rules=None, follow_items=False):
    """
    Work out copying record ``record_id`` of ``list_name`` from ``src`` into ``dst`` (both
    :class:`ClientData`): which records go (with which new ids), which are shared, which files
    they name. Nothing is written; :func:`apply_record_copy` writes it.
    """
    rules = rules or LinkRules()
    plan = DataCopy()
    queue = [(list_name, record_id)]
    seen = set()
    used = {}

    def new_id(space):
        if space not in used:
            used[space] = dst.ids_in_space(space)
        candidate = next_free_id(used[space])
        used[space].add(candidate)
        return candidate

    while queue:
        name, old_id = queue.pop(0)
        if (name, old_id) in seen or old_id == 0:
            continue
        seen.add((name, old_id))
        src_list = src.index.get(name)
        dst_list = dst.index.get(name)
        if src_list is None:
            continue
        src_record = src.find(src_list, old_id)
        if src_record < 0:
            plan.unresolved.append((name, "ID", old_id))
            continue
        if dst_list is None:
            plan.unresolved.append((name, "lista inexistente no destino", old_id))
            continue
        values = src.record(src_list, src_record)
        dst_record = dst.find(dst_list, old_id)
        space = src.lists[src_list]["id_space"]
        if dst_record >= 0 and (_is_type_list(name) or _records_equal(
                src, src_list, values, dst, dst_list, dst.record(dst_list, dst_record))):
            plan.ids[(name, old_id)] = old_id
            plan.shared.append((name, old_id))
        else:
            plan.ids[(name, old_id)] = new_id(space)
            plan.added.append((name, old_id, plan.ids[(name, old_id)]))
        fields = src.field_index[src_list]
        if name in EQUIPMENT_LISTS and "realname" in fields:
            realname = values[fields["realname"]]
            if isinstance(realname, str) and realname.strip() not in ("", "0"):
                combo = values[fields["character_combo_id"]] if "character_combo_id" in fields else -1
                gender = values[fields["gender"]] if "gender" in fields else -1
                plan.equipment.append((name, realname.strip(), combo, gender))
        for k, target in rules.links(src, src_list):
            value = values[k]
            if target == FILE and name in EQUIPMENT_LISTS and src.lists[src_list]["fields"][k][0] == "realname":
                continue
            if target == FILE:
                if isinstance(value, str) and value.strip() not in ("", "0") and value.strip() not in plan.files:
                    plan.files.append(value.strip())
                continue
            if not isinstance(value, int) or value <= 0:
                continue
            if target == TASK:
                # A quest of tasks.data: not copied, the id stays as it is.
                plan.unresolved.append((name, src.lists[src_list]["fields"][k][0], value))
                continue
            if target == ITEM:
                found = src.find_in_space("ESSENCE", value)
                if found is None:
                    plan.unresolved.append((name, src.lists[src_list]["fields"][k][0], value))
                elif follow_items:
                    queue.append((src.lists[found[0]]["name"], value))
                continue
            queue.append((target, value))
    return plan


def _same_record_in_target(src, dst, found, record_id):
    """Whether the target has the same record under the same id (so a reference can stay)."""
    if found is None:
        return False
    name = src.lists[found[0]]["name"]
    dst_list = dst.index.get(name)
    if dst_list is None:
        return False
    dst_record = dst.find(dst_list, record_id)
    return dst_record >= 0 and _records_equal(src, found[0], src.record(*found),
                                              dst, dst_list, dst.record(dst_list, dst_record))


def apply_record_copy(src, dst, plan, rules=None):
    """Write the records of a :func:`plan_record_copy` into ``dst`` (in memory)."""
    import _angelica
    rules = rules or LinkRules()
    for name, old_id, new_id in plan.added:
        src_list, dst_list = src.index[name], dst.index[name]
        values = src.record(src_list, src.find(src_list, old_id))
        record = _angelica.elements_add(dst.handle, dst_list, new_id)
        src_names = src.field_index[src_list]
        links = dict(rules.links(dst, dst_list))
        for j, (field, _kind, _size) in enumerate(dst.lists[dst_list]["fields"]):
            if j == 0 or field not in src_names:
                continue
            value = values[src_names[field]]
            target = links.get(j)
            if target not in (None, FILE, ITEM) and isinstance(value, int) and (target, value) in plan.ids:
                value = plan.ids[(target, value)]
            elif target == ITEM and isinstance(value, int) and value > 0:
                found = src.find_in_space("ESSENCE", value)
                item_list = src.lists[found[0]]["name"] if found else None
                if (item_list, value) in plan.ids:
                    value = plan.ids[(item_list, value)]
                elif not _same_record_in_target(src, dst, found, value):
                    # An item the copy does not bring: the id would name another item there.
                    plan.cleared.append((name, field, value))
                    value = 0
            try:
                _angelica.elements_set(dst.handle, dst_list, record, j, value)
            except ValueError:
                pass  # A value the target field cannot hold keeps the default.
    return dst.to_bytes()
