# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""The GameForge tab of the 3D View sidebar: the character preview (see ``bl_operators.gameforge_character``)."""

from bpy.types import Panel

import gameforge_account


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


classes = (
    VIEW3D_PT_gameforge_character,
)


if __name__ == "__main__":  # only for live edit.
    from bpy.utils import register_class
    for cls in classes:
        register_class(cls)
