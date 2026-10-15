# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""The client library in the Asset Browser's side region (see ``bl_operators.gameforge_client``)."""

from bpy.types import (
    Panel,
    UIList,
)


class GAMEFORGE_UL_client_library(UIList):

    def draw_item(self, _context, layout, _data, item, _icon, _active_data, _active_propname, index):
        row = layout.row(align=True)
        for _ in range(item.depth):
            row.separator(factor=1.2)
        if item.row_type in {'CONTAINER', 'FOLDER'}:
            props = row.operator(
                "gameforge.library_toggle",
                text="",
                icon='DISCLOSURE_TRI_DOWN' if item.expanded else 'DISCLOSURE_TRI_RIGHT',
                emboss=False,
            )
            props.index = index
        row.label(text=item.name, icon=item.icon, translate=False)
        if item.info:
            sub = row.row()
            sub.alignment = 'RIGHT'
            sub.enabled = False
            sub.label(text=item.info, translate=False)

    def draw_filter(self, _context, _layout):
        # The panel has its own search and type filter (searching every package of the client).
        pass


class FILEBROWSER_PT_gameforge_client_library(Panel):
    bl_space_type = 'FILE_BROWSER'
    bl_region_type = 'TOOLS'
    bl_label = "Cliente do jogo"

    @classmethod
    def poll(cls, context):
        space = context.space_data
        return space and space.type == 'FILE_BROWSER' and space.browse_mode == 'ASSETS'

    def draw(self, context):
        layout = self.layout
        settings = context.window_manager.gameforge_library

        row = layout.row(align=True)
        row.prop(settings, "client", text="", icon='PACKAGE')
        row.operator("gameforge.client_add", text="", icon='ADD')
        row.operator("gameforge.client_remove", text="", icon='REMOVE')
        row.operator("gameforge.library_refresh", text="", icon='FILE_REFRESH')

        if not settings.client:
            col = layout.column(align=True)
            col.label(text="Adicione a pasta do cliente do jogo", icon='INFO', translate=False)
            col.label(text="(Arquivo › Importar › Cliente do jogo).", translate=False)
            return

        row = layout.row(align=True)
        row.prop(settings, "search", text="", icon='VIEWZOOM')
        row.prop(settings, "kind", text="", icon_only=True)

        if not settings.rows and not settings.search:
            layout.operator("gameforge.library_refresh", text="Mostrar arquivos do cliente", icon='FILE_REFRESH')

        layout.template_list(
            "GAMEFORGE_UL_client_library", "",
            settings, "rows",
            settings, "row_index",
            rows=16,
        )
        if settings.message:
            layout.label(text=settings.message, icon='INFO', translate=False)

        rows = settings.rows
        if 0 <= settings.row_index < len(rows) and rows[settings.row_index].row_type == 'FILE':
            item = rows[settings.row_index]
            col = layout.column(align=True)
            col.label(text=item.path, translate=False)
            row = col.row(align=True)
            row.scale_y = 1.3
            row.operator("gameforge.library_open", text="Abrir para editar", icon='FILE_FOLDER')
            row.operator("gameforge.library_links", text="Vínculos", icon='LINKED')
            # Models described in text: the text itself can be edited too.
            if item.path.lower().endswith(".ecm"):
                col.operator("gameforge.library_open", text="Abrir como texto", icon='TEXT').as_text = True
            col.operator("gameforge.library_copy", text="Copiar para outro cliente...", icon='DUPLICATE')

        layout.separator()
        row = layout.row(align=True)
        row.operator("gameforge.client_save", text="Salvar edições no cliente", icon='PACKAGE')
        row.operator("gameforge.client_save_undo", text="", icon='LOOP_BACK')


class GAMEFORGE_UL_game_data(UIList):

    def draw_item(self, _context, layout, _data, item, _icon, _active_data, _active_propname, _index):
        row = layout.row(align=True)
        sub = row.row()
        sub.ui_units_x = 3.5
        sub.label(text=str(item.record_id), translate=False)
        row.label(text=item.name, translate=False)

    def draw_filter(self, _context, _layout):
        pass


class FILEBROWSER_PT_gameforge_game_data(Panel):
    bl_space_type = 'FILE_BROWSER'
    bl_region_type = 'TOOLS'
    bl_label = "Dados do jogo"
    bl_options = {'DEFAULT_CLOSED'}

    @classmethod
    def poll(cls, context):
        space = context.space_data
        return (space and space.type == 'FILE_BROWSER' and space.browse_mode == 'ASSETS' and
                bool(context.window_manager.gameforge_library.client))

    def draw(self, context):
        layout = self.layout
        settings = context.window_manager.gameforge_data
        row = layout.row(align=True)
        row.prop(settings, "list_name", text="")
        row.operator("gameforge.data_search", text="", icon='FILE_REFRESH')
        layout.prop(settings, "search", text="", icon='VIEWZOOM')
        layout.template_list("GAMEFORGE_UL_game_data", "", settings, "rows", settings, "row_index", rows=10)
        if settings.message:
            layout.label(text=settings.message, icon='INFO', translate=False)
        if 0 <= settings.row_index < len(settings.rows):
            row = layout.row()
            row.scale_y = 1.3
            op = row.operator("gameforge.record_open", text="Editar", icon='GREASEPENCIL')
            op.list_name = settings.list_name
            op.record_id = settings.rows[settings.row_index].record_id
            row.operator("gameforge.data_copy", text="Copiar para outro cliente...", icon='DUPLICATE')


class FILEBROWSER_PT_gameforge_record(Panel):
    bl_space_type = 'FILE_BROWSER'
    bl_region_type = 'TOOLS'
    bl_label = "Registro do jogo"

    @classmethod
    def poll(cls, context):
        space = context.space_data
        return (space and space.type == 'FILE_BROWSER' and space.browse_mode == 'ASSETS' and
                bool(context.scene.gameforge_record.list_name))

    def draw(self, context):
        from bl_ui.space_view3d_gameforge import draw_record_editor
        draw_record_editor(self.layout, context)


classes = (
    GAMEFORGE_UL_client_library,
    FILEBROWSER_PT_gameforge_client_library,
    GAMEFORGE_UL_game_data,
    FILEBROWSER_PT_gameforge_game_data,
    FILEBROWSER_PT_gameforge_record,
)


if __name__ == "__main__":  # only for live edit.
    from bpy.utils import register_class
    for cls in classes:
        register_class(cls)
