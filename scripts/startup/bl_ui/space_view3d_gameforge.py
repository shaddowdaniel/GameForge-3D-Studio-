# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
The GameForge tab of the 3D View sidebar: the character preview (see
``bl_operators.gameforge_character``) and map editing (see ``bl_operators.gameforge_world``).
The "Agentes de IA" tab: Claude through the user's own Claude Code (see ``bl_operators.gameforge_agent``).
"""

import re
import textwrap

from bpy.types import (
    Panel,
    UIList,
)

import gameforge_account

_COLOR_CODE = re.compile(r"\^[0-9A-Fa-f]{6}")


def _plain(text):
    """Game text without its ``^RRGGBB`` color codes, for headers (the fields keep the codes)."""
    return _COLOR_CODE.sub("", text)


class VIEW3D_PT_gameforge_character(Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "GameForge"
    bl_label = "Personagem da conta"

    def draw(self, context):
        from bl_operators import gameforge_character as preview

        layout = self.layout
        layout.use_property_split = True
        layout.use_property_decorate = False
        settings = context.window_manager.gameforge_character

        if not gameforge_account.get().session_token():
            col = layout.column(align=True)
            col.label(text="Entre com a conta atomgameforge.com", icon='LOCKED', translate=False)
            col.label(text="para usar os seus personagens.", translate=False)
            return

        layout.prop(settings, "client_root", text="Cliente", translate=False)

        row = layout.row(align=True)
        row.prop(settings, "character", text="", translate=False)
        row.operator("gameforge.character_refresh", text="", icon='FILE_REFRESH')

        col = layout.column()
        col.active = preview.has_characters()
        col.prop(settings, "fashion", translate=False)
        col.prop(settings, "try_on", text="Provar", translate=False)
        col.prop(settings, "import_animations", translate=False)

        row = layout.row()
        row.scale_y = 1.4
        row.operator("gameforge.character_preview", text="Montar personagem", icon='ARMATURE_DATA', translate=False)

        text, is_error = preview.panel_status()
        if preview.is_busy():
            layout.label(text=text or "Carregando...", icon='SORTTIME', translate=False)
        elif text:
            layout.label(text=text, icon='ERROR' if is_error else 'INFO', translate=False)


class GAMEFORGE_UL_spawn_entries(UIList):

    def draw_item(self, _context, layout, _data, item, _icon, _active_data, _active_propname, _index):
        split = layout.split(factor=0.28, align=True)
        split.label(text=str(item.template_id), translate=False)
        row = split.row()
        row.label(text=item.name or "?", translate=False)
        sub = row.row()
        sub.alignment = 'RIGHT'
        sub.label(text="x{:d}".format(item.count), translate=False)

    def draw_filter(self, _context, _layout):
        pass


class VIEW3D_PT_gameforge_map(Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "GameForge"
    bl_label = "Mapa do jogo"

    def draw(self, _context):
        layout = self.layout
        col = layout.column(align=True)
        col.scale_y = 1.2
        col.operator("gameforge.spawns_open", text="Abrir spawns do mapa...", icon='OUTLINER_OB_EMPTY')
        col.operator("gameforge.spawn_area_add", text="Nova área de spawn", icon='ADD')
        col.operator("gameforge.spawns_show_models", text="Mostrar modelos nos spawns", icon='OUTLINER_OB_ARMATURE')
        layout.operator("gameforge.scene_open", text="Abrir objetos do mapa (.ecwld)...", icon='HOME')
        layout.operator("gameforge.client_save", text="Salvar no cliente do jogo...", icon='PACKAGE')


class VIEW3D_PT_gameforge_spawn(Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "GameForge"
    bl_label = "Spawn"

    @classmethod
    def poll(cls, context):
        ob = context.active_object
        return ob is not None and ob.gameforge_spawn.kind != 'NONE'

    def draw(self, context):
        layout = self.layout
        props = context.active_object.gameforge_spawn
        if props.kind != 'AREA':
            layout.label(text={'RESOURCE': "Área de recurso: só a posição é editada",
                               'DYNAMIC': "Objeto dinâmico: só a posição é editada"}[props.kind],
                         icon='INFO', translate=False)
            return
        layout.label(text="Área nova" if props.index < 0 else "Área {:d} do arquivo".format(props.index),
                     icon='OUTLINER_OB_EMPTY', translate=False)
        layout.row().prop(props, "content", expand=True)
        layout.row().prop(props, "shape", expand=True)
        row = layout.row()
        row.template_list("GAMEFORGE_UL_spawn_entries", "", props, "spawns", props, "spawn_index", rows=4)
        col = row.column(align=True)
        col.operator("gameforge.spawn_entry_add", text="", icon='ADD')
        col.operator("gameforge.spawn_entry_remove", text="", icon='REMOVE')
        if 0 <= props.spawn_index < len(props.spawns):
            entry = props.spawns[props.spawn_index]
            col = layout.column(align=True)
            col.use_property_split = True
            row = col.row(align=True)
            row.prop(entry, "template_id")
            row.operator("gameforge.spawn_template_pick", text="", icon='VIEWZOOM')
            op = row.operator("gameforge.record_open", text="", icon='FILE_TEXT')
            op.record_id = entry.template_id
            collection = next((c for c in context.active_object.users_collection if "gameforge_npcgen_file" in c), None)
            op.client = collection.get("gameforge_client", "") if collection else ""
            for name in ("count", "refresh", "refresh_lower", "aggressive", "path_id", "loop_type", "speed_flag",
                         "dead_time"):
                col.prop(entry, name)
        box = layout.box()
        col = box.column(align=True)
        col.use_property_split = True
        for name in ("init_gen", "auto_revive", "valid_once", "group_type", "gen_id", "controller_id",
                     "life_time", "max_num"):
            col.prop(props, name)


# Names of the NPC services in the record editor.
SERVICE_LABELS = {
    "id_talk_service": "Conversa",
    "id_sell_service": "Loja (vende)",
    "id_buy_service": "Compra itens",
    "id_repair_service": "Conserto",
    "id_install_service": "Engastar",
    "id_uninstall_service": "Desengastar",
    "id_task_out_service": "Dá missões",
    "id_task_in_service": "Recebe missões",
    "id_task_matter_service": "Itens de missão",
    "id_skill_service": "Ensina habilidades",
    "id_heal_service": "Cura",
    "id_transmit_service": "Teleporte",
    "id_transport_service": "Transporte",
    "id_proxy_service": "Procurador",
    "id_storage_service": "Armazém",
    "id_make_service": "Fabricação",
    "id_decompose_service": "Decomposição",
    "id_identify_service": "Identificação",
    "id_war_towerbuild_service": "Torres de guerra",
    "id_resetprop_service": "Redistribuir atributos",
    "id_petname_service": "Nome do mascote",
    "id_petlearnskill_service": "Mascote aprende",
    "id_petforgetskill_service": "Mascote esquece",
    "id_equipbind_service": "Vincular equipamento",
    "id_equipdestroy_service": "Destruir equipamento",
    "id_equipundestroy_service": "Recuperar equipamento",
}


class GAMEFORGE_UL_record_fields(UIList):

    def draw_item(self, _context, layout, _data, item, _icon, _active_data, _active_propname, index):
        from gameforge_angelica import data
        split = layout.split(factor=0.42, align=True)
        icon = 'ERROR' if item.error else 'GREASEPENCIL' if item.value != item.original else 'NONE'
        split.label(text=SERVICE_LABELS.get(item.name, item.name), icon=icon, translate=False)
        row = split.row(align=True)
        row.prop(item, "value", text="")
        if item.link and item.link != data.FILE:
            if item.link_label:
                sub = row.row(align=True)
                sub.ui_units_x = 5
                sub.label(text=item.link_label, translate=False)
            row.operator("gameforge.record_pick", text="", icon='VIEWZOOM').index = index
            row.operator("gameforge.record_follow", text="", icon='FORWARD').index = index


def draw_record_editor(layout, context):
    """The record editor (shared by the 3D View sidebar and the Asset Browser)."""
    scene = context.scene
    record = scene.gameforge_record
    row = layout.row(align=True)
    row.operator("gameforge.record_open", text="Abrir registro...", icon='FILE_TEXT')
    row.operator("gameforge.record_back", text="", icon='BACK')
    if not record.list_name:
        return
    box = layout.box()
    row = box.row()
    row.label(text="{:s} {:d}".format(record.list_name, record.record_id), icon='PROPERTIES', translate=False)
    row.operator("gameforge.record_duplicate", text="", icon='DUPLICATE')
    if record.title:
        box.label(text=_plain(record.title), translate=False)
    layout.template_list("GAMEFORGE_UL_record_fields", "", record, "fields", record, "field_index", rows=12)
    if 0 <= record.field_index < len(record.fields):
        field = record.fields[record.field_index]
        if field.error:
            layout.label(text=field.error, icon='ERROR', translate=False)
    edited = sum(1 for e in scene.gameforge_record_edits if e.client == record.client)
    if edited:
        row = layout.row(align=True)
        row.label(text="{:d} campo(s) editado(s) neste cliente".format(edited), icon='GREASEPENCIL', translate=False)
        row.operator("gameforge.record_revert", text="", icon='X')
        layout.operator("gameforge.client_save", text="Salvar no cliente do jogo...", icon='PACKAGE')


class VIEW3D_PT_gameforge_record(Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "GameForge"
    bl_label = "Registro do jogo"
    bl_options = {'DEFAULT_CLOSED'}

    def draw(self, context):
        draw_record_editor(self.layout, context)


class GAMEFORGE_UL_task_nodes(UIList):

    def draw_item(self, _context, layout, _data, item, _icon, _active_data, _active_propname, _index):
        layout.label(text=item.name, translate=False)

    def draw_filter(self, _context, _layout):
        pass


class GAMEFORGE_UL_task_fields(UIList):

    def draw_item(self, _context, layout, _data, item, _icon, _active_data, _active_propname, _index):
        split = layout.split(factor=0.55, align=True)
        icon = 'ERROR' if item.error else 'GREASEPENCIL' if item.value != item.original else 'NONE'
        split.label(text=item.name, icon=icon, translate=False)
        if item.locked:
            split.label(text=item.value, icon='LOCKED', translate=False)
        else:
            split.prop(item, "value", text="")


def draw_task_editor(layout, context):
    scene = context.scene
    quest = scene.gameforge_task
    layout.operator("gameforge.task_open", text="Abrir missão...", icon='VIEWZOOM')
    if not quest.task_id:
        return
    box = layout.box()
    row = box.row()
    row.label(text="{:s} ({:d})".format(_plain(quest.title) or "?", quest.task_id), icon='BOOKMARKS', translate=False)
    row.operator("gameforge.task_duplicate", text="", icon='DUPLICATE')
    if len(quest.nodes) > 1:
        layout.template_list("GAMEFORGE_UL_task_nodes", "", quest, "nodes", quest, "node_index", rows=4)
    layout.template_list("GAMEFORGE_UL_task_fields", "", quest, "fields", quest, "field_index", rows=12)
    if 0 <= quest.field_index < len(quest.fields) and quest.fields[quest.field_index].error:
        layout.label(text=quest.fields[quest.field_index].error, icon='ERROR', translate=False)
    edited = sum(1 for e in scene.gameforge_task_edits if e.client == quest.client)
    if edited:
        row = layout.row(align=True)
        row.label(text="{:d} campo(s) editado(s) em missões".format(edited), icon='GREASEPENCIL', translate=False)
        row.operator("gameforge.task_revert", text="", icon='X')
        layout.operator("gameforge.client_save", text="Salvar no cliente do jogo...", icon='PACKAGE')


class VIEW3D_PT_gameforge_task(Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "GameForge"
    bl_label = "Missão do jogo"
    bl_options = {'DEFAULT_CLOSED'}

    def draw(self, context):
        draw_task_editor(self.layout, context)


class VIEW3D_PT_gameforge_scene_object(Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "GameForge"
    bl_label = "Objeto do mapa"

    @classmethod
    def poll(cls, context):
        ob = context.active_object
        return ob is not None and ob.get("gameforge_scene_kind") is not None

    def draw(self, context):
        from gameforge_angelica import scene
        layout = self.layout
        ob = context.active_object
        kind = ob["gameforge_scene_kind"]
        layout.label(text="{:s} {:d}".format(scene.LABELS.get(kind, kind), ob.get("gameforge_scene_id", 0)),
                     translate=False)
        if ob.get("gameforge_scene_path"):
            layout.label(text=ob["gameforge_scene_path"], icon='FILE', translate=False)
        if kind in scene.EDITABLE:
            layout.label(text="Mova, gire, duplique (Shift+D) ou apague e salve no cliente", icon='INFO', translate=False)
        elif kind == 'BUILDING':
            layout.label(text="Prédio: a colisão está no .bmd e no servidor, não é movido aqui", icon='LOCKED',
                         translate=False)
        else:
            layout.label(text="Só para ver (ainda não editado)", icon='LOCKED', translate=False)


class VIEW3D_PT_gameforge_agent(Panel):
    bl_space_type = 'VIEW_3D'
    bl_region_type = 'UI'
    bl_category = "Agentes de IA"
    bl_label = "Claude"

    def draw(self, context):
        from bl_operators import gameforge_agent as agent

        layout = self.layout
        settings = context.window_manager.gameforge_agent
        scale = context.preferences.system.ui_scale
        # Characters per line for the wrapped conversation (the UI font is about 7 px wide at scale 1).
        width = max(24, int((context.region.width - 40) / (7.0 * scale)))
        request = agent.pending_request()
        # As many conversation lines as fit, so the request field stays in view (the whole conversation is in
        # "Transcrição").
        room = max(8, int((context.region.height - (560 if request is not None else 300) * scale) / (17.0 * scale)))

        box = layout.box()
        status = agent.auth()
        if not agent.claude_exe():
            box.label(text="O painel usa o Claude Code, que não está instalado", icon='ERROR', translate=False)
            row = box.row(align=True)
            row.operator("gameforge.agent_install_claude", icon='IMPORT', translate=False)
            row.operator("gameforge.agent_refresh", text="", icon='FILE_REFRESH')
        elif status is None:
            box.label(text="Conferindo a conta Claude...", icon='SORTTIME', translate=False)
        elif not status.get("loggedIn"):
            box.label(text="Entre na sua conta para usar o Claude", icon='LOCKED', translate=False)
            col = box.column(align=True)
            col.scale_y = 1.2
            col.operator("gameforge.agent_login", text="Entrar com a conta Claude", icon='USER', translate=False)
            col.operator("gameforge.agent_login", text="Entrar com o Console (chave de API)", icon='KEYINGSET',
                         translate=False).console = True
        else:
            box.label(text=agent.account_label(status), icon='CHECKMARK', translate=False)
        row = box.row(align=True)
        row.prop(settings, "model", text="Modelo", translate=False)
        if status is not None and status.get("loggedIn"):
            row.operator("gameforge.agent_refresh", text="", icon='FILE_REFRESH')
            row.operator("gameforge.agent_logout", text="", icon='X')

        box = layout.box()
        col = box.column(align=True)
        col.scale_y = 0.85
        conversation = agent.lines()
        if not conversation:
            col.label(text="Escreva abaixo o que você quer no Studio.", icon='INFO', translate=False)
            col.label(text="Ex.: crie uma espada de cristal azul ao lado do modelo.", translate=False)
        wrapped = [piece for line in conversation[-200:] for piece in (textwrap.wrap(line, width) or [""])]
        if len(wrapped) > room:
            col.label(text="... (a conversa inteira está em Transcrição)", translate=False)
        for piece in wrapped[-room:]:
            col.label(text=piece, translate=False)

        if request is not None:
            box = layout.box()
            box.alert = True
            box.label(text="O Claude quer: " + agent.engine.tool_label(request.get("tool_name", "")),
                      icon='QUESTION', translate=False)
            col = box.column(align=True)
            col.scale_y = 0.8
            summary = agent.request_summary(request)
            for line in summary[:12]:
                col.label(text=line[:width] or " ", translate=False)
            if len(summary) > 12:
                col.label(text="... (+{:d} linhas)".format(len(summary) - 12), translate=False)
            row = box.row(align=True)
            row.scale_y = 1.3
            row.operator("gameforge.agent_decide", text="Executar", icon='CHECKMARK', translate=False).allow = True
            row.operator("gameforge.agent_decide", text="Recusar", icon='CANCEL', translate=False).allow = False
            row = box.row(align=True)
            row.operator("gameforge.agent_show_code", icon='TEXT', translate=False)
            op = row.operator("gameforge.agent_decide", text="Executar sempre nesta conversa", translate=False)
            op.allow = True
            op.always = True

        layout.prop(settings, "prompt", text="", icon='GREASEPENCIL')
        row = layout.row(align=True)
        row.scale_y = 1.3
        sub = row.row(align=True)
        sub.enabled = not agent.is_running()
        sub.operator("gameforge.agent_send", icon='PLAY', translate=False)
        row.operator("gameforge.agent_stop", text="", icon='PAUSE')
        row.operator("gameforge.agent_new", text="", icon='FILE_NEW')
        row.operator("gameforge.agent_transcript", text="", icon='TEXT')
        text = agent.status()
        if text:
            layout.label(text=text, icon='SORTTIME', translate=False)


classes = (
    VIEW3D_PT_gameforge_agent,
    VIEW3D_PT_gameforge_character,
    GAMEFORGE_UL_spawn_entries,
    VIEW3D_PT_gameforge_map,
    VIEW3D_PT_gameforge_spawn,
    VIEW3D_PT_gameforge_scene_object,
    GAMEFORGE_UL_record_fields,
    VIEW3D_PT_gameforge_record,
    GAMEFORGE_UL_task_nodes,
    GAMEFORGE_UL_task_fields,
    VIEW3D_PT_gameforge_task,
)


if __name__ == "__main__":  # only for live edit.
    from bpy.utils import register_class
    for cls in classes:
        register_class(cls)
