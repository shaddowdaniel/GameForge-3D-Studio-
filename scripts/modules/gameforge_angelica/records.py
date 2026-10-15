# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Editing the records of a client's ``elements.data`` (items, NPCs and their services, monsters,
...) in the Studio.

Edits are kept in the scene (``Scene.gameforge_record_edits``: client, list, record id, field,
value as text), so they take part in undo and are saved with the ``.blend``. Saving ("Salvar no
Cliente do Jogo") writes the client's ``data\\elements.data`` and, when the client has a server
folder, the server's ``elements.data`` in the same save: the same bytes when both were the same
file, otherwise the same fields set in the server's own records. Fields that were not edited keep
their bytes.
"""

__all__ = (
    "ElementsChange",
    "changes",
    "duplicate",
    "format_value",
    "link_name",
    "parse_value",
    "read_record",
)

import os

from . import data, save

_INTS = {"int32": (-2 ** 31, 2 ** 31 - 1), "uint32": (0, 2 ** 32 - 1), "int64": (-2 ** 63, 2 ** 63 - 1)}


def format_value(kind, value):
    """A field value as the text shown in the editor."""
    if kind == "float":
        return "{:.6g}".format(value)
    return str(value)


def parse_value(kind, size, text):
    """The value of a field typed as text. Raises ValueError with a message for the user."""
    text = text.strip() if kind != "string" and kind != "wstring" else text
    if kind in _INTS:
        try:
            value = int(text)
        except ValueError:
            raise ValueError("'{:s}' não é um número inteiro".format(text)) from None
        low, high = _INTS[kind]
        if not low <= value <= high:
            raise ValueError("{:d} fora do intervalo do campo ({:d} a {:d})".format(value, low, high))
        return value
    if kind == "float":
        try:
            return float(text.replace(",", "."))
        except ValueError:
            raise ValueError("'{:s}' não é um número".format(text)) from None
    # Strings: GBK bytes or UTF-16 units in a fixed size field, room kept for the terminator.
    encoded = text.encode("gbk", errors="replace") if kind == "string" else text.encode("utf-16-le")
    room = size - (1 if kind == "string" else 2)
    if len(encoded) > room:
        raise ValueError("texto longo demais para o campo ({:d} bytes no máximo)".format(room))
    return text


def read_record(db, list_index, record_id):
    """``[(name, kind, size, value)]`` of a record, or None when the list has no such id."""
    record = db.find(list_index, record_id)
    if record < 0:
        return None
    values = db.record(list_index, record)
    return [(name, kind, size, value) for (name, kind, size), value in zip(db.lists[list_index]["fields"], values)]


def link_name(db, target, value):
    """Name of the record a linked field points to (``target`` from ``data.LinkRules``), or ''."""
    if not isinstance(value, int) or value <= 0 or target in (None, data.FILE):
        return ""
    if target == data.ITEM:
        found = db.find_in_space("ESSENCE", value)
    else:
        list_index = db.index.get(target)
        record = db.find(list_index, value) if list_index is not None else -1
        found = (list_index, record) if record >= 0 else None
    if found is None:
        return "(não existe)"
    list_index, record = found
    fields = db.field_index[list_index]
    return str(db.record(list_index, record)[fields["Name"]]) if "Name" in fields else ""


def _apply(db, edits, where):
    """Set the edited fields into an open elements.data. Raises save.SaveError."""
    import _angelica
    for edit in edits:
        list_index = db.index.get(edit.list_name)
        record = db.find(list_index, edit.record_id) if list_index is not None else -1
        if record < 0:
            raise save.SaveError("{:s} {:d} não existe {:s}".format(edit.list_name, edit.record_id, where))
        field = db.field_index[list_index].get(edit.field)
        if field is None:
            raise save.SaveError("{:s} não tem o campo {:s} {:s}".format(edit.list_name, edit.field, where))
        _name, kind, size = db.lists[list_index]["fields"][field]
        try:
            value = parse_value(kind, size, edit.value)
        except ValueError as ex:
            raise save.SaveError("{:s} {:d}, {:s}: {:s}".format(edit.list_name, edit.record_id, edit.field,
                                                                str(ex))) from None
        _angelica.elements_set(db.handle, list_index, record, field, value)


class ElementsChange(save.EditsChange):
    """The ``elements.data`` of a client (or of its server) with the record edits of the scenes."""
    __slots__ = ()
    EDITS = "gameforge_record_edits"
    CLIENT_PATH = "data\\elements.data"
    SERVER_NAME = "elements.data"
    KIND = 'ELEMENTS'

    def build(self, file_bytes, edits, where):
        db = data.ClientData(self.root, file_bytes)
        try:
            _apply(db, edits, where)
            return db.to_bytes()
        finally:
            db.close()


def changes(scenes):
    """The :class:`ElementsChange` of every client with record edits (and of its server)."""
    return ElementsChange.changes(scenes)


def _add_copy(db, list_name, record_id, new_id, where):
    """Append a copy of a record under ``new_id`` (named "... (cópia)"). Raises save.SaveError."""
    import _angelica
    list_index = db.index.get(list_name)
    record = db.find(list_index, record_id) if list_index is not None else -1
    if record < 0:
        raise save.SaveError("{:s} {:d} não existe {:s}".format(list_name, record_id, where))
    if db.find_in_space(db.lists[list_index]["id_space"], new_id) is not None:
        raise save.SaveError("o id {:d} já está em uso {:s}".format(new_id, where))
    added = _angelica.elements_add(db.handle, list_index, new_id, record)
    name_field = db.field_index[list_index].get("Name")
    if name_field is not None:
        _name, kind, size = db.lists[list_index]["fields"][name_field]
        name = str(db.record(list_index, added)[name_field])
        for cut in range(len(name), -1, -1):
            try:
                copy_name = parse_value(kind, size, name[:cut].rstrip() + " (cópia)")
            except ValueError:
                continue
            _angelica.elements_set(db.handle, list_index, added, name_field, copy_name)
            break


def duplicate(client, list_name, record_id, server=""):
    """
    Copy a record of the client's elements.data under a new id (and into the server's
    elements.data, with the same id, when ``server`` is given), saved at once with undo. Returns
    the new id. Raises save.SaveError.
    """
    import _angelica
    original = _angelica.read(client, "data\\elements.data")
    if original is None:
        raise save.SaveError("o cliente não tem data\\elements.data")
    db = data.ClientData(client, original)
    try:
        list_index = db.index.get(list_name)
        if list_index is None:
            raise save.SaveError("o cliente não tem a lista " + list_name)
        new_id = data.next_free_id(db.ids_in_space(db.lists[list_index]["id_space"]))
        _add_copy(db, list_name, record_id, new_id, "no cliente")
        client_bytes = db.to_bytes()
    finally:
        db.close()
    changes = [save.BytesChange(client, "data\\elements.data", client_bytes, 'ELEMENTS')]
    server_path = os.path.join(server, "elements.data") if server else ""
    if server_path and os.path.isfile(server_path):
        with open(server_path, "rb") as fh:
            server_original = fh.read()
        if server_original == original:
            server_bytes = client_bytes
        else:
            server_db = data.ClientData(server, server_original)
            try:
                _add_copy(server_db, list_name, record_id, new_id, "no elements.data do servidor")
                server_bytes = server_db.to_bytes()
            finally:
                server_db.close()
        change = save.BytesChange(server, "elements.data", server_bytes, 'ELEMENTS')
        change.folder = "servidor"
        changes.append(change)
    save.save(changes, 'IN_PLACE')
    return new_id
