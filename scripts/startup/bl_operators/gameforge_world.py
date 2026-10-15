# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Map editing: the NPC and monster spawns of a map (the server's ``npcgen.data``) and the client's
scene of the map (``.ecwld``: buildings, models, effects, sounds, ...) as objects in the 3D View,
with their settings in the sidebar (see ``gameforge_angelica.spawns`` and ``.scene``). Saving goes
through "Salvar no Cliente do Jogo", like every other edit.
"""

__all__ = (
    "GameForgeSpawnEntry",
    "GameForgeSpawnArea",
    "GAMEFORGE_OT_spawns_open",
    "GAMEFORGE_OT_spawn_entry_add",
    "GAMEFORGE_OT_spawn_entry_remove",
    "GAMEFORGE_OT_spawn_template_pick",
    "GAMEFORGE_OT_spawn_area_add",
    "GAMEFORGE_OT_spawns_show_models",
    "GAMEFORGE_OT_scene_open",
)

import os
from struct import error as struct_error

import bpy
from bpy.props import (
    CollectionProperty,
    EnumProperty,
    FloatProperty,
    IntProperty,
    PointerProperty,
    StringProperty,
)
from bpy.types import (
    Operator,
    PropertyGroup,
)

from gameforge_angelica import clients, scene, spawns

_map_items_cache = []


def _client(context):
    root = context.window_manager.gameforge_library.client
    return next((c for c in clients.load() if c.root == root), None)


def _spawn_collection(ob):
    return next((c for c in ob.users_collection if spawns.PROP_FILE in c), None)


def _template_changed(self, _context):
    collection = _spawn_collection(self.id_data)
    self.name = spawns.template_name(collection.get(spawns.PROP_CLIENT, ""), self.template_id) if collection else ""


class GameForgeSpawnEntry(PropertyGroup):
    """One NPC or monster template of a spawn area (``name`` is the template's name, not saved)."""
    template_id: IntProperty(
        name="Template",
        description="Id do NPC ou monstro (elements.data)",
        min=0,
        update=_template_changed,
    )
    count: IntProperty(name="Quantidade", min=0, default=1)
    refresh: IntProperty(name="Renascer (s)", description="Tempo para renascer, em segundos")
    refresh_lower: IntProperty(name="Renascer mínimo (s)")
    died_times: IntProperty(name="Mortes")
    aggressive: IntProperty(name="Agressivo")
    offset_water: FloatProperty(name="Altura na água")
    offset_terrain: FloatProperty(name="Altura no terreno")
    faction: IntProperty(name="Facção")
    faction_helper: IntProperty(name="Facção ajudante")
    faction_accept: IntProperty(name="Facção aceita")
    need_help: IntProperty(name="Pede ajuda")
    default_faction: IntProperty()
    default_faction_helper: IntProperty()
    default_faction_accept: IntProperty()
    path_id: IntProperty(name="Caminho")
    loop_type: IntProperty(name="Tipo de volta")
    speed_flag: IntProperty(name="Correndo")
    dead_time: IntProperty(name="Corpo (s)")


def _set_area_type(props, value):
    props.area_type = value
    props.id_data.empty_display_type = 'CUBE' if value == 1 else 'CIRCLE'


class GameForgeSpawnArea(PropertyGroup):
    """Spawn data of an object made from ``npcgen.data``."""
    kind: EnumProperty(
        items=(
            ('NONE', "Nenhum", ""),
            ('AREA', "Área de spawn", "NPCs ou monstros"),
            ('RESOURCE', "Área de recurso", "Minérios e ervas"),
            ('DYNAMIC', "Objeto dinâmico", "Portões, decorações"),
        ),
        default='NONE',
    )
    # Index of the record in the file, -1 for new areas.
    index: IntProperty(default=-1)
    area_type: IntProperty(name="Tipo", description="0 sobre o terreno, 1 caixa")
    npc_type: IntProperty(name="Conteúdo", description="0 monstros, 1 NPCs de serviço")
    # The two kinds above with names, for the sidebar (the file values stay in the integers).
    content: EnumProperty(
        name="Conteúdo",
        items=(('MONSTERS', "Monstros", "", 0), ('NPCS', "NPCs de serviço", "", 1)),
        get=lambda self: self.npc_type,
        set=lambda self, value: setattr(self, "npc_type", value),
    )
    shape: EnumProperty(
        name="Forma",
        items=(('TERRAIN', "Sobre o terreno", "Nascem no chão, dentro da área", 0),
               ('BOX', "Caixa", "Nascem dentro da caixa", 1)),
        get=lambda self: self.area_type,
        set=lambda self, value: _set_area_type(self, value),
    )
    group_type: IntProperty(name="Grupo")
    init_gen: IntProperty(name="Nasce no início", default=1)
    auto_revive: IntProperty(name="Renasce", default=1)
    valid_once: IntProperty(name="Uma vez só")
    gen_id: IntProperty(name="Id do gerador")
    controller_id: IntProperty(name="Controlador")
    life_time: IntProperty(name="Duração")
    max_num: IntProperty(name="Máximo")
    spawns: CollectionProperty(type=GameForgeSpawnEntry)
    spawn_index: IntProperty()


def _map_items(_self, context):
    items = []
    client = _client(context)
    if client and client.server and os.path.isdir(client.server):
        for name in sorted(os.listdir(client.server)):
            if os.path.isfile(os.path.join(client.server, name, "npcgen.data")):
                items.append((name, name, ""))
    _map_items_cache[:] = items or [("", "Nenhum mapa", "Defina a pasta do servidor do cliente")]
    return _map_items_cache


class GAMEFORGE_OT_spawns_open(Operator):
    """Abrir os spawns de NPCs e monstros de um mapa (npcgen.data do servidor) para editar na Vista 3D"""
    bl_idname = "gameforge.spawns_open"
    bl_label = "Abrir Spawns do Mapa"
    bl_options = {'REGISTER', 'UNDO'}

    server: StringProperty(
        name="Servidor",
        description="Pasta de dados do servidor do cliente (gamed\\config), com uma pasta por mapa",
        subtype='DIR_PATH',
    )
    map_code: EnumProperty(name="Mapa", items=_map_items)

    @classmethod
    def poll(cls, context):
        return _client(context) is not None

    def invoke(self, context, _event):
        client = _client(context)
        self.server = client.server
        return context.window_manager.invoke_props_dialog(self, width=480, confirm_text="Abrir")

    def draw(self, context):
        layout = self.layout
        layout.prop(self, "server")
        layout.prop(self, "map_code")

    def check(self, context):
        client = _client(context)
        server = bpy.path.abspath(self.server)
        if client and server != client.server and os.path.isdir(server):
            clients.set_server(client.root, server)
        return True

    def execute(self, context):
        client = _client(context)
        server = bpy.path.abspath(self.server)
        if not server or not os.path.isdir(server):
            self.report({'ERROR'}, "Escolha a pasta de dados do servidor")
            return {'CANCELLED'}
        if server != client.server:
            clients.set_server(client.root, server)
        if not self.map_code:
            self.report({'ERROR'}, "Nenhum mapa com npcgen.data nessa pasta")
            return {'CANCELLED'}
        context.window.cursor_set('WAIT')
        try:
            collection = spawns.load_map_spawns(server, self.map_code, client.root)
        except (OSError, ValueError) as ex:
            self.report({'ERROR'}, str(ex))
            return {'CANCELLED'}
        finally:
            context.window.cursor_set('DEFAULT')
        self.report({'INFO'}, "{:d} spawns do mapa {:s}".format(len(collection.objects), self.map_code))
        return {'FINISHED'}


def _active_area(context):
    ob = context.active_object
    if ob is not None and ob.gameforge_spawn.kind == 'AREA':
        return ob.gameforge_spawn
    return None


class GAMEFORGE_OT_spawn_entry_add(Operator):
    """Acrescentar um NPC ou monstro a esta área"""
    bl_idname = "gameforge.spawn_entry_add"
    bl_label = "Acrescentar template"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        return _active_area(context) is not None

    def execute(self, context):
        area = _active_area(context)
        entry = area.spawns.add()
        entry.count = 1
        area.spawn_index = len(area.spawns) - 1
        return {'FINISHED'}


class GAMEFORGE_OT_spawn_entry_remove(Operator):
    """Tirar o template selecionado desta área"""
    bl_idname = "gameforge.spawn_entry_remove"
    bl_label = "Remover template"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        area = _active_area(context)
        return area is not None and 0 <= area.spawn_index < len(area.spawns)

    def execute(self, context):
        area = _active_area(context)
        area.spawns.remove(area.spawn_index)
        area.spawn_index = min(area.spawn_index, len(area.spawns) - 1)
        return {'FINISHED'}


_template_items_cache = []


def _template_items(_self, context):
    ob = context.active_object if context else None
    collection = _spawn_collection(ob) if ob is not None else None
    known = spawns.templates(collection.get(spawns.PROP_CLIENT, "")) if collection else {}
    wanted = "NPC_ESSENCE" if collection and ob.gameforge_spawn.npc_type == 1 else "MONSTER_ESSENCE"
    _template_items_cache[:] = [
        (str(template_id), "{:s} ({:d})".format(name, template_id), "")
        for template_id, (name, list_name) in sorted(known.items()) if list_name == wanted
    ] or [("0", "Nenhum", "Os nomes vêm do elements.data do cliente da coleção")]
    return _template_items_cache


class GAMEFORGE_OT_spawn_template_pick(Operator):
    """Escolher pelo nome o NPC ou monstro do template selecionado"""
    bl_idname = "gameforge.spawn_template_pick"
    bl_label = "Escolher NPC ou Monstro"
    bl_options = {'REGISTER', 'UNDO'}
    bl_property = "template"

    template: EnumProperty(name="Template", items=_template_items)

    @classmethod
    def poll(cls, context):
        area = _active_area(context)
        return area is not None and 0 <= area.spawn_index < len(area.spawns)

    def invoke(self, context, _event):
        context.window_manager.invoke_search_popup(self)
        return {'RUNNING_MODAL'}

    def execute(self, context):
        area = _active_area(context)
        area.spawns[area.spawn_index].template_id = int(self.template)
        return {'FINISHED'}


class GAMEFORGE_OT_spawn_area_add(Operator):
    """Criar uma área de spawn nova no cursor 3D, no mapa do objeto ativo"""
    bl_idname = "gameforge.spawn_area_add"
    bl_label = "Nova Área de Spawn"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        ob = context.active_object
        return ob is not None and any(c in spawns.spawn_collections() for c in ob.users_collection)

    def execute(self, context):
        source = context.active_object
        collection = next(c for c in source.users_collection if c in spawns.spawn_collections())
        ob = bpy.data.objects.new("Área nova", None)
        ob.empty_display_type = 'CUBE'
        collection.objects.link(ob)
        ob.location = context.scene.cursor.location
        ob.scale = source.scale if source.gameforge_spawn.kind == 'AREA' else (5.0, 5.0, 2.0)
        props = ob.gameforge_spawn
        props.kind = 'AREA'
        props.index = -1
        props.area_type = 0
        if source.gameforge_spawn.kind == 'AREA':
            props.npc_type = source.gameforge_spawn.npc_type
        entry = props.spawns.add()
        entry.count = 1
        for other in context.selected_objects:
            other.select_set(False)
        ob.select_set(True)
        context.view_layer.objects.active = ob
        return {'FINISHED'}


class GAMEFORGE_OT_spawns_show_models(Operator):
    """Mostrar nas áreas de spawn os modelos dos NPCs e monstros (cada modelo é aberto uma vez)"""
    bl_idname = "gameforge.spawns_show_models"
    bl_label = "Mostrar Modelos nos Spawns"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        ob = context.active_object
        return ob is not None and any(c in spawns.spawn_collections() for c in ob.users_collection)

    def execute(self, context):
        collection = next(c for c in context.active_object.users_collection if c in spawns.spawn_collections())
        client = collection.get(spawns.PROP_CLIENT) or context.window_manager.gameforge_library.client
        if not client:
            self.report({'ERROR'}, "Escolha o cliente do jogo na biblioteca")
            return {'CANCELLED'}
        context.window.cursor_set('WAIT')
        try:
            shown, failed = spawns.show_models(collection, client)
        finally:
            context.window.cursor_set('DEFAULT')
        message = "{:d} modelo(s) nas áreas".format(shown)
        if failed:
            message += "; {:d} não abriram".format(len(failed))
        self.report({'WARNING'} if failed else {'INFO'}, message)
        return {'FINISHED'}


_scene_items_cache = []


def _scene_items(_self, context):
    client = _client(context)
    items = []
    maps = os.path.join(client.root, "maps") if client else ""
    if maps and os.path.isdir(maps):
        for name in sorted(os.listdir(maps)):
            if os.path.isfile(os.path.join(maps, name, name + ".ecwld")):
                items.append((name, name, ""))
    _scene_items_cache[:] = items or [("", "Nenhum mapa", "O cliente não tem maps\\<mapa>\\<mapa>.ecwld")]
    return _scene_items_cache


class GAMEFORGE_OT_scene_open(Operator):
    """Abrir os objetos de um mapa do cliente (.ecwld): prédios, modelos, efeitos, sons, árvores e áreas de ambiente"""
    bl_idname = "gameforge.scene_open"
    bl_label = "Abrir Objetos do Mapa"
    bl_options = {'REGISTER', 'UNDO'}

    map_code: EnumProperty(name="Mapa", items=_scene_items)
    around_cursor: bpy.props.BoolProperty(
        name="Só em volta do cursor 3D",
        description="Abrir só os objetos perto do cursor 3D (mapas grandes têm milhares de prédios)",
        default=False,
    )
    radius: FloatProperty(name="Distância", description="Metros em volta do cursor 3D", default=300.0, min=10.0,
                          subtype='DISTANCE')
    building_limit: IntProperty(name="Máximo de prédios", default=400, min=0)

    @classmethod
    def poll(cls, context):
        return _client(context) is not None

    def invoke(self, context, _event):
        return context.window_manager.invoke_props_dialog(self, width=420, confirm_text="Abrir")

    def draw(self, _context):
        layout = self.layout
        layout.use_property_split = True
        layout.prop(self, "map_code")
        layout.prop(self, "around_cursor")
        row = layout.row()
        row.enabled = self.around_cursor
        row.prop(self, "radius")
        layout.prop(self, "building_limit")

    def execute(self, context):
        client = _client(context)
        if not self.map_code:
            self.report({'ERROR'}, "Nenhum mapa com .ecwld neste cliente")
            return {'CANCELLED'}
        center = None
        if self.around_cursor:
            cursor = context.scene.cursor.location
            center = (cursor.x, cursor.y)
        context.window.cursor_set('WAIT')
        try:
            collection, problems = scene.load_map_scene(client.root, self.map_code, center=center,
                                                        radius=self.radius if self.around_cursor else 0.0,
                                                        building_limit=self.building_limit)
        except (OSError, ValueError, struct_error) as ex:
            self.report({'ERROR'}, str(ex))
            return {'CANCELLED'}
        finally:
            context.window.cursor_set('DEFAULT')
        message = "{:d} objetos do mapa {:s}".format(len(collection.all_objects), self.map_code)
        if problems:
            message += "; não abriram: " + ", ".join(problems[:3]) + (" ..." if len(problems) > 3 else "")
        self.report({'WARNING'} if problems else {'INFO'}, message)
        return {'FINISHED'}


classes = (
    GameForgeSpawnEntry,
    GameForgeSpawnArea,
    GAMEFORGE_OT_spawns_open,
    GAMEFORGE_OT_spawn_entry_add,
    GAMEFORGE_OT_spawn_entry_remove,
    GAMEFORGE_OT_spawn_template_pick,
    GAMEFORGE_OT_spawn_area_add,
    GAMEFORGE_OT_spawns_show_models,
    GAMEFORGE_OT_scene_open,
)


def register():
    # The classes are registered by `bl_operators` (from `classes`), before this runs.
    bpy.types.Object.gameforge_spawn = PointerProperty(type=GameForgeSpawnArea)


def unregister():
    del bpy.types.Object.gameforge_spawn
