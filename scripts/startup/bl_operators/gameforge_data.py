# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
The game database of the active client (``elements.data``) in the Asset Browser: pick a list,
search records by name or id, and copy a record (with the records and files it uses) into another
client. A record can also be opened and edited field by field (see ``gameforge_angelica.records``),
following the ids it holds to the records they name (an NPC's shop, the items it sells, ...).
"""

__all__ = (
    "GameForgeDataRow",
    "GameForgeDataSettings",
    "GAMEFORGE_OT_data_search",
    "GAMEFORGE_OT_data_copy",
    "GameForgeRecordEdit",
    "GameForgeRecordField",
    "GameForgeRecord",
    "GAMEFORGE_OT_record_open",
    "GAMEFORGE_OT_record_follow",
    "GAMEFORGE_OT_record_back",
    "GAMEFORGE_OT_record_pick",
    "GAMEFORGE_OT_record_revert",
    "GAMEFORGE_OT_record_duplicate",
)

import os

import bpy
from bpy.props import (
    BoolProperty,
    CollectionProperty,
    EnumProperty,
    IntProperty,
    PointerProperty,
    StringProperty,
)
from bpy.types import (
    Operator,
    PropertyGroup,
)

from gameforge_angelica import clients, data, library, records, save

MAX_ROWS = 2000
# Open databases per client: {root: (stamp, ClientData)}.
_open = {}
_list_items_cache = []


def _root(context):
    root = context.window_manager.gameforge_library.client
    return root if root and os.path.isdir(root) else ""


def client_data(root):
    """The client's database, opened once and again when the file changes."""
    import _angelica
    location = _angelica.locate(root, "data\\elements.data")
    if location is None:
        raise FileNotFoundError("o cliente não tem data\\elements.data")
    stamp = (location["container"], location["size"], os.path.getmtime(location["container"]))
    cached = _open.get(root)
    if cached is None or cached[0] != stamp:
        if cached is not None:
            cached[1].close()
        _open[root] = (stamp, data.ClientData(root))
    return _open[root][1]


def _list_items(_self, context):
    items = []
    root = _root(context)
    if root:
        try:
            db = client_data(root)
            for i, entry in enumerate(db.lists):
                if entry["count"] and entry["fields"] and entry["fields"][0][0] == "ID":
                    items.append((entry["name"], "{:s} ({:d})".format(entry["name"], entry["count"]), entry["label"]))
        except (OSError, ValueError) as ex:
            items = [("", "Dados indisponíveis", str(ex))]
    _list_items_cache[:] = items or [("", "Nenhuma lista", "")]
    return _list_items_cache


def _rebuild(context):
    settings = context.window_manager.gameforge_data
    settings.rows.clear()
    settings.message = ""
    root = _root(context)
    if not root or not settings.list_name:
        return
    try:
        db = client_data(root)
    except (OSError, ValueError) as ex:
        settings.message = str(ex)
        return
    list_index = db.index.get(settings.list_name)
    if list_index is None:
        return
    name_field = db.field_index[list_index].get("Name")
    search = settings.search.strip().lower()
    for record in range(db.lists[list_index]["count"]):
        values = db.record(list_index, record)
        name = values[name_field] if name_field is not None else ""
        if search and search not in str(values[0]) and search not in str(name).lower():
            continue
        row = settings.rows.add()
        row.record_id = values[0]
        row.name = str(name)
        row.index = record
        if len(settings.rows) >= MAX_ROWS:
            settings.message = "Mostrando os primeiros {:d}: use a busca".format(MAX_ROWS)
            break
    if not settings.rows:
        settings.message = "Nada encontrado"


class GameForgeDataRow(PropertyGroup):
    record_id: IntProperty()
    index: IntProperty()


class GameForgeDataSettings(PropertyGroup):
    list_name: EnumProperty(
        name="Lista",
        description="Lista de dados do jogo (itens, NPCs, monstros, ...)",
        items=_list_items,
        update=lambda _self, context: _rebuild(context),
    )
    search: StringProperty(
        name="Buscar",
        description="Buscar registros pelo nome ou pelo id",
        update=lambda _self, context: _rebuild(context),
    )
    rows: CollectionProperty(type=GameForgeDataRow)
    row_index: IntProperty(name="Registro ativo")
    message: StringProperty()


def _active_record(context):
    settings = context.window_manager.gameforge_data
    if 0 <= settings.row_index < len(settings.rows):
        return settings.list_name, settings.rows[settings.row_index].record_id
    return None


class GAMEFORGE_OT_data_search(Operator):
    """Ler de novo os registros da lista"""
    bl_idname = "gameforge.data_search"
    bl_label = "Atualizar registros"

    @classmethod
    def poll(cls, context):
        return bool(_root(context))

    def execute(self, context):
        context.window.cursor_set('WAIT')
        try:
            _rebuild(context)
        finally:
            context.window.cursor_set('DEFAULT')
        return {'FINISHED'}


# Plan shown by the copy dialog.
_record_plan = [None]
_target_cache = []


def _target_items(_self, context):
    active = _root(context)
    items = [(c.root, c.name, c.root) for c in clients.load()
             if os.path.normcase(c.root) != os.path.normcase(active)]
    _target_cache[:] = items or [("", "Nenhum outro cliente", "Adicione outro cliente do jogo")]
    return _target_cache


def _plan(op, context):
    from gameforge_angelica import transfer
    record = _active_record(context)
    _record_plan[0] = None
    op.error = ""
    if record is None or not op.target:
        return
    context.window.cursor_set('WAIT')
    try:
        _record_plan[0] = transfer.plan_record(_root(context), record[0], record[1], op.target, op.follow_items)
    except (OSError, ValueError) as ex:
        op.error = str(ex)
    finally:
        context.window.cursor_set('DEFAULT')


class GAMEFORGE_OT_data_copy(Operator):
    """Copiar este registro para outro cliente do jogo, com os registros que ele usa (tipos, \\
addons, serviços, ...) e os arquivos de modelo, efeito e som"""
    bl_idname = "gameforge.data_copy"
    bl_label = "Copiar Registro para Outro Cliente"
    bl_options = {'REGISTER'}

    target: EnumProperty(name="Cliente de destino", items=_target_items,
                         update=lambda self, context: _plan(self, context))
    follow_items: BoolProperty(
        name="Levar também os itens citados",
        description="Copiar também os itens que o registro cita (drops, materiais, ...); "
        "sem isso as citações a itens que o destino não tem ficam zeradas",
        update=lambda self, context: _plan(self, context),
    )
    replace_different: BoolProperty(name="Substituir os arquivos diferentes")
    include_unsupported: BoolProperty(name="Copiar também versões mais novas")
    mode: EnumProperty(
        name="Como salvar",
        items=(
            ('IN_PLACE', "No cliente de destino", "Grava no elements.data e nos pacotes do destino; pode ser desfeito"),
            ('PATCH', "Pacote só com a cópia", "Cria numa pasta o elements.data novo e pacotes só com os arquivos copiados"),
            ('COPY', "Cópia completa dos pacotes", "Cria numa pasta o elements.data novo e cópias completas dos pacotes"),
        ),
    )
    directory: StringProperty(name="Pasta", subtype='DIR_PATH')
    server_folder: StringProperty(
        name="Servidor",
        description="Pasta de dados do servidor do cliente de destino (com o elements.data, por exemplo "
        "gamed\\config): o item só funciona no jogo quando o servidor tem os mesmos registros",
        subtype='DIR_PATH',
    )
    error: StringProperty(options={'HIDDEN', 'SKIP_SAVE'})

    @classmethod
    def poll(cls, context):
        return _active_record(context) is not None and len(clients.load()) > 1

    def invoke(self, context, _event):
        _plan(self, context)
        known = next((c for c in clients.load() if c.root == self.target), None)
        self.server_folder = known.server if known else ""
        return context.window_manager.invoke_props_dialog(self, width=640, confirm_text="Copiar")

    def draw(self, _context):
        layout = self.layout
        layout.prop(self, "target")
        if self.error:
            layout.label(text=self.error, icon='ERROR', translate=False)
            return
        plan = _record_plan[0]
        if plan is None:
            layout.label(text="Escolha o cliente de destino", icon='INFO')
            return
        box = layout.box()
        col = box.column(align=True)
        col.label(text="{:d} registro(s) novos no destino (ids a partir de {:d})".format(
            len(plan.data.added), data.FIRST_NEW_ID), icon='ADD')
        for name, old_id, new_id in plan.data.added[:5]:
            col.label(text="    {:s} {:d} → {:d}".format(name, old_id, new_id), translate=False)
        col.label(text="{:d} já existem iguais no destino (reaproveitados)".format(len(plan.data.shared)),
                  icon='CHECKMARK')
        if plan.data.cleared:
            col.label(text="{:d} citação(ões) a itens que o destino não tem ficam zeradas".format(
                len(plan.data.cleared)), icon='ERROR')
        if plan.data.unresolved:
            col.label(text="{:d} referência(s) que a cópia não segue".format(len(plan.data.unresolved)),
                      icon='QUESTION')
        new_files = [i for i in plan.items if i.status == 'NEW']
        col.label(text="{:d} arquivo(s) novos ({:s}), {:d} já iguais".format(
            len(new_files), library.human_size(sum(i.size for i in new_files)),
            sum(1 for i in plan.items if i.status == 'SAME')), icon='FILE')
        if any(i.convert for i in plan.items):
            col.label(text="{:d} arquivo(s) convertidos para versões que o destino lê".format(
                sum(1 for i in plan.items if i.convert)), icon='FILE_REFRESH')
        if plan.extra:
            col.label(text="Descrição, cor e ícone vão junto ({:d} arquivo(s) do destino atualizados)".format(
                len(plan.extra)), icon='IMAGE_DATA')
        for note in plan.notes[:3]:
            col.label(text=note, icon='INFO', translate=False)
        layout.prop(self, "follow_items")
        if any(i.status == 'DIFFERENT' for i in plan.items):
            layout.prop(self, "replace_different")
        if any(i.unsupported for i in plan.items):
            layout.prop(self, "include_unsupported")
        layout.prop(self, "mode", expand=True)
        if self.mode != 'IN_PLACE':
            layout.prop(self, "directory")
        layout.prop(self, "server_folder")
        if not self.server_folder:
            layout.label(text="Sem a pasta do servidor, o item só aparece no cliente.", icon='INFO')

    def execute(self, context):
        from gameforge_angelica import transfer
        plan = _record_plan[0]
        if plan is None:
            self.report({'ERROR'}, self.error or "Escolha o cliente de destino")
            return {'CANCELLED'}
        context.window.cursor_set('WAIT')
        server = bpy.path.abspath(self.server_folder) if self.server_folder else ""
        if server and not os.path.isfile(os.path.join(server, "elements.data")):
            self.report({'ERROR'}, "A pasta do servidor não tem elements.data")
            return {'CANCELLED'}
        clients.set_server(plan.dst_root, server)
        try:
            written = transfer.copy_record(plan, self.replace_different, self.include_unsupported,
                                           self.mode, bpy.path.abspath(self.directory), server)
        except save.SaveError as ex:
            self.report({'ERROR'}, str(ex))
            return {'CANCELLED'}
        finally:
            context.window.cursor_set('DEFAULT')
        new_id = plan.data.ids.get((plan.list_name, plan.record_id), plan.record_id)
        self.report({'INFO'}, "{:s} {:d} copiado como {:d} ({:d} arquivo(s)/pacote(s) gravados)".format(
            plan.list_name, plan.record_id, new_id, len(written)))
        return {'FINISHED'}


# -----------------------------------------------------------------------------
# Record editor

_rules = []
_loading = [False]


def link_rules():
    if not _rules:
        _rules.append(data.LinkRules())
    return _rules[0]


def _as_int(text):
    try:
        return int(text.strip())
    except ValueError:
        return 0


class GameForgeRecordEdit(PropertyGroup):
    """An edited field of a record, waiting for "Salvar no Cliente do Jogo"."""
    client: StringProperty()
    list_name: StringProperty()
    record_id: IntProperty()
    field: StringProperty()
    value: StringProperty()


def _edit_index(scene, record, field):
    return next((i for i, e in enumerate(scene.gameforge_record_edits)
                 if e.client == record.client and e.list_name == record.list_name and
                 e.record_id == record.record_id and e.field == field), None)


def _field_changed(self, context):
    if _loading[0]:
        return
    scene = context.scene
    record = scene.gameforge_record
    edits = scene.gameforge_record_edits
    found = _edit_index(scene, record, self.name)
    if self.value == self.original:
        if found is not None:
            edits.remove(found)
    else:
        edit = edits[found] if found is not None else edits.add()
        edit.client, edit.list_name, edit.record_id = record.client, record.list_name, record.record_id
        edit.field, edit.value = self.name, self.value
    try:
        records.parse_value(self.kind, self.size, self.value)
        self.error = ""
    except ValueError as ex:
        self.error = str(ex)
    if self.link:
        try:
            self.link_label = _link_label(record.client, self.link, _as_int(self.value))
        except (OSError, ValueError):
            self.link_label = ""


def _link_label(client, link, value):
    if link == data.TASK:
        from gameforge_angelica import tasks
        return tasks.task_name(client, value) if value > 0 else ""
    return records.link_name(client_data(client), link, value)


class GameForgeRecordField(PropertyGroup):
    """A field of the open record (``name`` is the field's name)."""
    value: StringProperty(name="Valor", update=_field_changed)
    original: StringProperty()
    kind: StringProperty()
    size: IntProperty()
    # The list (or data.ITEM) the field's id points to, and that record's name.
    link: StringProperty()
    link_label: StringProperty()
    error: StringProperty()


class GameForgeRecord(PropertyGroup):
    """The record open in the editor."""
    client: StringProperty()
    list_name: StringProperty()
    record_id: IntProperty()
    title: StringProperty()
    fields: CollectionProperty(type=GameForgeRecordField)
    field_index: IntProperty()
    # Records opened before ("LIST:id" separated by "|"), for going back.
    history: StringProperty()


def load_record(context, client, list_name, record_id, remember=True):
    """Show a record in the editor, with the scene's edits of it. Raises ValueError, OSError."""
    db = client_data(client)
    list_index = db.index.get(list_name)
    fields = records.read_record(db, list_index, record_id) if list_index is not None else None
    if fields is None:
        raise ValueError("{:s} {:d} não existe neste cliente".format(list_name, record_id))
    scene = context.scene
    record = scene.gameforge_record
    if remember and record.list_name and record.client == client and \
            (record.list_name, record.record_id) != (list_name, record_id):
        record.history = "|".join(([record.history] if record.history else []) +
                                  ["{:s}:{:d}".format(record.list_name, record.record_id)])
    edits = {e.field: e.value for e in scene.gameforge_record_edits
             if e.client == client and e.list_name == list_name and e.record_id == record_id}
    links = dict(link_rules().links(db, list_index))
    _loading[0] = True
    try:
        record.client, record.list_name, record.record_id = client, list_name, record_id
        record.fields.clear()
        record.title = ""
        for k, (name, kind, size, value) in enumerate(fields):
            item = record.fields.add()
            item.name, item.kind, item.size = name, kind, size
            item.original = records.format_value(kind, value)
            item.value = edits.get(name, item.original)
            item.link = links.get(k) or ""
            if item.link:
                item.link_label = _link_label(client, item.link, _as_int(item.value))
            if name == "Name":
                record.title = item.value
        record.field_index = 0
    finally:
        _loading[0] = False


def _library_client(context):
    return context.window_manager.gameforge_library.client


class GAMEFORGE_OT_record_open(Operator):
    """Abrir um registro do elements.data (item, NPC, monstro, serviço, ...) para ver e editar os campos"""
    bl_idname = "gameforge.record_open"
    bl_label = "Abrir Registro"
    bl_options = {'REGISTER', 'UNDO'}

    client: StringProperty(name="Cliente", options={'SKIP_SAVE'})
    list_name: StringProperty(name="Lista", options={'SKIP_SAVE'})
    record_id: IntProperty(name="Id", min=0, options={'SKIP_SAVE'})

    def invoke(self, context, _event):
        if self.record_id:
            return self.execute(context)
        return context.window_manager.invoke_props_dialog(self, width=360, confirm_text="Abrir")

    def draw(self, context):
        layout = self.layout
        layout.prop(context.window_manager.gameforge_data, "list_name", text="Lista")
        layout.prop(self, "record_id")

    def execute(self, context):
        client = self.client or _library_client(context)
        if not client:
            self.report({'ERROR'}, "Escolha o cliente do jogo na biblioteca")
            return {'CANCELLED'}
        list_name = self.list_name or context.window_manager.gameforge_data.list_name
        try:
            db = client_data(client)
            if not list_name:
                # An id alone: the record of that id among items, NPCs and monsters.
                found = db.find_in_space("ESSENCE", self.record_id)
                if found is None:
                    raise ValueError("nenhum registro com o id {:d}".format(self.record_id))
                list_name = db.lists[found[0]]["name"]
            load_record(context, client, list_name, self.record_id)
        except (OSError, ValueError) as ex:
            self.report({'ERROR'}, str(ex))
            return {'CANCELLED'}
        return {'FINISHED'}


def _open_field(context, index):
    record = context.scene.gameforge_record
    return record.fields[index] if 0 <= index < len(record.fields) else None


class GAMEFORGE_OT_record_follow(Operator):
    """Abrir o registro que este campo indica"""
    bl_idname = "gameforge.record_follow"
    bl_label = "Abrir Registro Indicado"
    bl_options = {'REGISTER', 'UNDO'}

    index: IntProperty(options={'HIDDEN'})

    def execute(self, context):
        record = context.scene.gameforge_record
        field = _open_field(context, self.index)
        record_id = _as_int(field.value) if field else 0
        if field is None or not field.link or field.link == data.FILE or record_id <= 0:
            self.report({'WARNING'}, "Este campo não indica um registro")
            return {'CANCELLED'}
        if field.link == data.TASK:
            return bpy.ops.gameforge.task_open(client=record.client, task_id=record_id)
        try:
            db = client_data(record.client)
            list_name = field.link
            if list_name == data.ITEM:
                found = db.find_in_space("ESSENCE", record_id)
                if found is None:
                    raise ValueError("o item {:d} não existe".format(record_id))
                list_name = db.lists[found[0]]["name"]
            load_record(context, record.client, list_name, record_id)
        except (OSError, ValueError) as ex:
            self.report({'ERROR'}, str(ex))
            return {'CANCELLED'}
        return {'FINISHED'}


class GAMEFORGE_OT_record_back(Operator):
    """Voltar ao registro aberto antes"""
    bl_idname = "gameforge.record_back"
    bl_label = "Voltar"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        return bool(context.scene.gameforge_record.history)

    def execute(self, context):
        record = context.scene.gameforge_record
        entries = record.history.split("|")
        list_name, record_id = entries[-1].rsplit(":", 1)
        record.history = "|".join(entries[:-1])
        try:
            load_record(context, record.client, list_name, int(record_id), remember=False)
        except (OSError, ValueError) as ex:
            self.report({'ERROR'}, str(ex))
            return {'CANCELLED'}
        return {'FINISHED'}


# Names to pick from, per (client, target): [(identifier, label, description)].
_pick_cache = {}


def _pick_items(self, context):
    record = context.scene.gameforge_record
    field = _open_field(context, self.index)
    if field is None or not field.link or field.link == data.FILE:
        return [("0", "Nenhum", "")]
    key = (record.client, field.link)
    if key not in _pick_cache and field.link == data.TASK:
        from gameforge_angelica import tasks
        _file, names = tasks.open_tasks(record.client)
        _pick_cache.clear()
        _pick_cache[key] = [("0", "Nenhuma (0)", "")] + [
            (str(task_id), "{:s} ({:d})".format(name or "?", task_id), "")
            for task_id, (_index, name) in sorted(names.items())]
    if key not in _pick_cache:
        import _angelica
        db = client_data(record.client)
        if field.link == data.ITEM:
            lists = [i for i, entry in enumerate(db.lists) if entry["id_space"] == "ESSENCE" and
                     entry["name"].endswith("_ESSENCE") and
                     entry["name"] not in ("NPC_ESSENCE", "MONSTER_ESSENCE", "MINE_ESSENCE")]
        else:
            lists = [db.index[field.link]] if field.link in db.index else []
        items = [("0", "Nenhum (0)", "")]
        for list_index in lists:
            name_field = db.field_index[list_index].get("Name")
            for record_index, record_id in enumerate(_angelica.elements_ids(db.handle, list_index)):
                name = db.record(list_index, record_index)[name_field] if name_field is not None else ""
                items.append((str(record_id), "{:s} ({:d})".format(str(name), record_id), ""))
        _pick_cache.clear()
        _pick_cache[key] = items
    return _pick_cache[key]


class GAMEFORGE_OT_record_pick(Operator):
    """Escolher pelo nome o registro (ou item) deste campo"""
    bl_idname = "gameforge.record_pick"
    bl_label = "Escolher pelo Nome"
    bl_options = {'REGISTER', 'UNDO'}
    bl_property = "choice"

    index: IntProperty(options={'HIDDEN'})
    choice: EnumProperty(name="Registro", items=_pick_items)

    def invoke(self, context, _event):
        context.window_manager.invoke_search_popup(self)
        return {'RUNNING_MODAL'}

    def execute(self, context):
        field = _open_field(context, self.index)
        if field is None:
            return {'CANCELLED'}
        field.value = self.choice
        return {'FINISHED'}


class GAMEFORGE_OT_record_revert(Operator):
    """Desfazer as edições ainda não salvas deste registro"""
    bl_idname = "gameforge.record_revert"
    bl_label = "Descartar Edições do Registro"
    bl_options = {'REGISTER', 'UNDO'}

    def execute(self, context):
        scene = context.scene
        record = scene.gameforge_record
        edits = scene.gameforge_record_edits
        for i in reversed(range(len(edits))):
            e = edits[i]
            if e.client == record.client and e.list_name == record.list_name and e.record_id == record.record_id:
                edits.remove(i)
        if record.list_name:
            load_record(context, record.client, record.list_name, record.record_id, remember=False)
        return {'FINISHED'}


class GAMEFORGE_OT_record_duplicate(Operator):
    """Criar uma cópia deste registro com um id novo (no elements.data do cliente e do servidor). \
A cópia é salva na hora e pode ser desfeita com Desfazer Último Salvamento no Cliente"""
    bl_idname = "gameforge.record_duplicate"
    bl_label = "Duplicar Registro"
    bl_options = {'REGISTER'}

    @classmethod
    def poll(cls, context):
        return bool(context.scene.gameforge_record.list_name)

    def invoke(self, context, _event):
        record = context.scene.gameforge_record
        return context.window_manager.invoke_confirm(
            self, None, title="Duplicar {:s} {:d}?".format(record.list_name, record.record_id),
            message="A cópia ganha um id novo e é salva agora no cliente (e no servidor).",
            confirm_text="Duplicar")

    def execute(self, context):
        record = context.scene.gameforge_record
        server = next((c.server for c in clients.load() if c.root == record.client), "")
        if context.window:
            context.window.cursor_set('WAIT')
        try:
            new_id = records.duplicate(record.client, record.list_name, record.record_id, server)
            load_record(context, record.client, record.list_name, new_id)
        except (save.SaveError, OSError, ValueError) as ex:
            self.report({'ERROR'}, str(ex))
            return {'CANCELLED'}
        finally:
            if context.window:
                context.window.cursor_set('DEFAULT')
        self.report({'INFO'}, "Cópia criada: {:s} {:d}".format(record.list_name, new_id))
        return {'FINISHED'}


classes = (
    GameForgeDataRow,
    GameForgeDataSettings,
    GAMEFORGE_OT_data_search,
    GAMEFORGE_OT_data_copy,
    GameForgeRecordEdit,
    GameForgeRecordField,
    GameForgeRecord,
    GAMEFORGE_OT_record_open,
    GAMEFORGE_OT_record_follow,
    GAMEFORGE_OT_record_back,
    GAMEFORGE_OT_record_pick,
    GAMEFORGE_OT_record_revert,
    GAMEFORGE_OT_record_duplicate,
)


def register():
    # The classes are registered by `bl_operators` (from `classes`), before this runs.
    bpy.types.WindowManager.gameforge_data = PointerProperty(type=GameForgeDataSettings)
    bpy.types.Scene.gameforge_record = PointerProperty(type=GameForgeRecord)
    bpy.types.Scene.gameforge_record_edits = CollectionProperty(type=GameForgeRecordEdit)


def unregister():
    del bpy.types.WindowManager.gameforge_data
    del bpy.types.Scene.gameforge_record
    del bpy.types.Scene.gameforge_record_edits
