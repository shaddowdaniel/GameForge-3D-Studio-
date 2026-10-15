# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
The quest editor: open a quest of the active client (``data\\tasks.data``) by name, walk its sub
tasks and edit their fields (see ``gameforge_angelica.tasks``). Saving goes through "Salvar no
Cliente do Jogo", like every other edit.
"""

__all__ = (
    "GameForgeTaskEdit",
    "GameForgeTaskNode",
    "GameForgeTaskField",
    "GameForgeTask",
    "GAMEFORGE_OT_task_open",
    "GAMEFORGE_OT_task_revert",
    "GAMEFORGE_OT_task_duplicate",
)

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

from gameforge_angelica import clients, save, tasks

_loading = [False]


class GameForgeTaskEdit(PropertyGroup):
    """An edited field of a quest, waiting for "Salvar no Cliente do Jogo"."""
    client: StringProperty()
    # The quest (top level task), the task or sub task of the field and its path inside that task.
    task_id: IntProperty()
    node_id: IntProperty()
    path: StringProperty()
    value: StringProperty()


def _field_changed(self, context):
    if _loading[0]:
        return
    scene = context.scene
    quest = scene.gameforge_task
    edits = scene.gameforge_task_edits
    found = next((i for i, e in enumerate(edits)
                  if e.client == quest.client and e.task_id == quest.task_id and e.node_id == self.node_id and
                  e.path == self.path), None)
    if self.value == self.original:
        if found is not None:
            edits.remove(found)
    else:
        edit = edits[found] if found is not None else edits.add()
        edit.client, edit.task_id, edit.node_id = quest.client, quest.task_id, self.node_id
        edit.path, edit.value = self.path, self.value
    try:
        tasks.parse_value(self.kind, self.value)
        self.error = ""
    except ValueError as ex:
        self.error = str(ex)


class GameForgeTaskNode(PropertyGroup):
    """A task or sub task of the open quest (``name`` is shown, indented by depth)."""
    node: IntProperty()
    task_id: IntProperty()


class GameForgeTaskField(PropertyGroup):
    """A field of the selected task (``name`` and ``path``: its path inside that task)."""
    node_id: IntProperty()
    path: StringProperty()
    value: StringProperty(name="Valor", update=_field_changed)
    original: StringProperty()
    kind: StringProperty()
    error: StringProperty()
    locked: BoolProperty()


def _node_changed(_self, context):
    if not _loading[0]:
        load_fields(context)


class GameForgeTask(PropertyGroup):
    """The quest open in the editor (``task_id`` is the top level task)."""
    client: StringProperty()
    task_id: IntProperty()
    title: StringProperty()
    nodes: CollectionProperty(type=GameForgeTaskNode)
    node_index: IntProperty(update=_node_changed)
    fields: CollectionProperty(type=GameForgeTaskField)
    field_index: IntProperty()


def load_fields(context):
    """Show the fields of the selected task (its own, not those of its sub tasks)."""
    scene = context.scene
    quest = scene.gameforge_task
    file, names = tasks.open_tasks(quest.client)
    nodes, fields = tasks.node_fields(file, names[quest.task_id][0])
    selected = quest.nodes[quest.node_index].node if 0 <= quest.node_index < len(quest.nodes) else 0
    node_id = nodes[selected][2]
    edits = {e.path: e.value for e in scene.gameforge_task_edits
             if e.client == quest.client and e.task_id == quest.task_id and e.node_id == node_id}
    _loading[0] = True
    try:
        quest.fields.clear()
        for _field, node, path, kind, value in fields:
            if node != selected:
                continue
            item = quest.fields.add()
            item.name = item.path = path
            item.node_id, item.kind = node_id, kind
            item.locked = path.rsplit(".", 1)[-1] in tasks.READ_ONLY
            item.original = tasks.format_value(kind, value)
            item.value = edits.get(path, item.original)
        quest.field_index = 0
    finally:
        _loading[0] = False


def load_task(context, client, task_id):
    """Open the quest holding task ``task_id`` (a sub task opens its quest at it). Raises ValueError."""
    file, names = tasks.open_tasks(client)
    if task_id in names:
        index, node = names[task_id][0], 0
    else:
        found = file.find(task_id)
        if found is None:
            raise ValueError("a missão {:d} não existe neste cliente".format(task_id))
        index, node = found
    top_id = next(i for i, (k, _name) in names.items() if k == index)
    nodes = file.nodes(index)
    quest = context.scene.gameforge_task
    _loading[0] = True
    try:
        quest.client, quest.task_id = client, top_id
        quest.title = nodes[0][3]
        quest.nodes.clear()
        depth = []
        for k, (_field, parent, node_id, name) in enumerate(nodes):
            depth.append(depth[parent] + 1 if parent >= 0 else 0)
            item = quest.nodes.add()
            item.name = "{:s}{:s} ({:d})".format("    " * depth[k], name or "?", node_id)
            item.node, item.task_id = k, node_id
        quest.node_index = node
    finally:
        _loading[0] = False
    load_fields(context)


# Quests to pick from, per client: [(identifier, label, description)].
_pick_cache = {}


def _task_items(_self, context):
    client = context.window_manager.gameforge_library.client if context else ""
    if not client:
        return [("0", "Nenhum cliente", "Escolha o cliente do jogo na biblioteca")]
    if client not in _pick_cache:
        try:
            _file, names = tasks.open_tasks(client)
        except (OSError, ValueError) as ex:
            return [("0", "Missões indisponíveis", str(ex))]
        _pick_cache.clear()
        _pick_cache[client] = [(str(task_id), "{:s} ({:d})".format(name or "?", task_id), "")
                               for task_id, (_index, name) in sorted(names.items())]
    return _pick_cache[client]


class GAMEFORGE_OT_task_open(Operator):
    """Abrir uma missão do cliente (tasks.data) para ver e editar, com as subtarefas"""
    bl_idname = "gameforge.task_open"
    bl_label = "Abrir Missão"
    bl_options = {'REGISTER', 'UNDO'}
    bl_property = "choice"

    client: StringProperty(name="Cliente", options={'SKIP_SAVE'})
    task_id: IntProperty(name="Id", min=0, options={'SKIP_SAVE'})
    choice: EnumProperty(name="Missão", items=_task_items)

    def invoke(self, context, _event):
        if self.task_id:
            return self.execute(context)
        context.window_manager.invoke_search_popup(self)
        return {'RUNNING_MODAL'}

    def execute(self, context):
        client = self.client or context.window_manager.gameforge_library.client
        if not client:
            self.report({'ERROR'}, "Escolha o cliente do jogo na biblioteca")
            return {'CANCELLED'}
        task_id = self.task_id or int(self.choice or 0)
        context.window.cursor_set('WAIT') if context.window else None
        try:
            load_task(context, client, task_id)
        except (OSError, KeyError) as ex:
            self.report({'ERROR'}, str(ex))
            return {'CANCELLED'}
        except ValueError as ex:
            try:
                version = tasks.open_tasks(client)[0].version
            except (OSError, ValueError):
                version = 0
            self.report({'ERROR'}, "Os campos desta missão não puderam ser lidos (tasks.data versão {:d}): {:s}".format(
                version, str(ex)))
            return {'CANCELLED'}
        finally:
            if context.window:
                context.window.cursor_set('DEFAULT')
        return {'FINISHED'}


class GAMEFORGE_OT_task_revert(Operator):
    """Desfazer as edições ainda não salvas desta missão"""
    bl_idname = "gameforge.task_revert"
    bl_label = "Descartar Edições da Missão"
    bl_options = {'REGISTER', 'UNDO'}

    def execute(self, context):
        scene = context.scene
        quest = scene.gameforge_task
        edits = scene.gameforge_task_edits
        for i in reversed(range(len(edits))):
            if edits[i].client == quest.client and edits[i].task_id == quest.task_id:
                edits.remove(i)
        if quest.task_id:
            load_fields(context)
        return {'FINISHED'}


class GAMEFORGE_OT_task_duplicate(Operator):
    """Criar uma cópia desta missão (com as subtarefas) com ids novos, no tasks.data do cliente e do \
servidor. A cópia é salva na hora e pode ser desfeita com Desfazer Último Salvamento no Cliente"""
    bl_idname = "gameforge.task_duplicate"
    bl_label = "Duplicar Missão"
    bl_options = {'REGISTER'}

    @classmethod
    def poll(cls, context):
        return bool(context.scene.gameforge_task.task_id)

    def invoke(self, context, _event):
        quest = context.scene.gameforge_task
        return context.window_manager.invoke_confirm(
            self, None, title="Duplicar a missão {:d}?".format(quest.task_id),
            message="A cópia e as subtarefas ganham ids novos e são salvas agora no cliente (e no servidor).",
            confirm_text="Duplicar")

    def execute(self, context):
        quest = context.scene.gameforge_task
        server = next((c.server for c in clients.load() if c.root == quest.client), "")
        if context.window:
            context.window.cursor_set('WAIT')
        try:
            new_id = tasks.duplicate(quest.client, quest.task_id, server)
            load_task(context, quest.client, new_id)
        except (save.SaveError, OSError, ValueError) as ex:
            self.report({'ERROR'}, str(ex))
            return {'CANCELLED'}
        finally:
            if context.window:
                context.window.cursor_set('DEFAULT')
        self.report({'INFO'}, "Cópia criada: missão {:d}".format(new_id))
        return {'FINISHED'}


classes = (
    GameForgeTaskEdit,
    GameForgeTaskNode,
    GameForgeTaskField,
    GameForgeTask,
    GAMEFORGE_OT_task_open,
    GAMEFORGE_OT_task_revert,
    GAMEFORGE_OT_task_duplicate,
)


def register():
    # The classes are registered by `bl_operators` (from `classes`), before this runs.
    bpy.types.Scene.gameforge_task = PointerProperty(type=GameForgeTask)
    bpy.types.Scene.gameforge_task_edits = CollectionProperty(type=GameForgeTaskEdit)


def unregister():
    del bpy.types.Scene.gameforge_task
    del bpy.types.Scene.gameforge_task_edits
