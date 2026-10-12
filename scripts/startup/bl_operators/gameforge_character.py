# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Character preview: load a real character saved in the user's GameForge account and assemble it in
the scene (body, equipment, weapons, wings, face) to try models, clothes and effects on it.

PWOps (``gameforge_account.pwops``) gives the list of characters and the look of one of them as
model paths; the native importer (``WM_OT_angelica_character_import``) reads the models from the
game client installed on this computer. The panel is ``VIEW3D_PT_gameforge_character``.
"""

__all__ = (
    "GameForgeCharacterSettings",
    "GAMEFORGE_OT_character_refresh",
    "GAMEFORGE_OT_character_preview",
)

import json
import os
import threading

import bpy
from bpy.props import (
    BoolProperty,
    EnumProperty,
    PointerProperty,
    StringProperty,
)
from bpy.types import (
    Operator,
    PropertyGroup,
)

import gameforge_account
from gameforge_account.portal import PortalError, SessionInvalid
from gameforge_account.pwops import PWOps, parse_try_on

_SETTINGS_FILE = "gameforge_preview.json"


class _Cache:
    """Characters of the account, kept for the enum (Python must own the item strings)."""
    characters = []
    items = []
    error = ""
    status = ""
    busy = False


def _pwops():
    return PWOps()


def _settings_path():
    return os.path.join(bpy.utils.user_resource('CONFIG'), _SETTINGS_FILE)


def _load_saved_client_root():
    try:
        with open(_settings_path(), "r", encoding="utf-8") as fh:
            return str(json.load(fh).get("client_root") or "")
    except (OSError, ValueError, AttributeError):
        return ""


def _save_client_root(self, _context):
    path = _settings_path()
    try:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w", encoding="utf-8") as fh:
            json.dump({"client_root": self.client_root}, fh)
    except OSError:
        pass


def _character_items(_self, _context):
    return _Cache.items or [("NONE", "Nenhum personagem", "Atualize a lista da sua conta")]


def _tag_redraw():
    for window in bpy.context.window_manager.windows:
        for area in window.screen.areas:
            if area.type == 'VIEW_3D':
                area.tag_redraw()


class GameForgeCharacterSettings(PropertyGroup):
    character: EnumProperty(
        name="Personagem",
        description="Personagem salvo na sua conta GameForge",
        items=_character_items,
    )
    client_root: StringProperty(
        name="Cliente do jogo",
        description="Pasta do cliente do jogo (a pasta element, com os arquivos .pck)",
        subtype='DIR_PATH',
        update=_save_client_root,
    )
    fashion: EnumProperty(
        name="Moda",
        description="Mostrar as roupas de moda ou a armadura",
        items=(
            ('AUTO', "Como no jogo", "Usar o modo salvo no personagem"),
            ('ON', "Moda", "Vestir as roupas de moda"),
            ('OFF', "Armadura", "Mostrar a armadura equipada"),
        ),
        default='AUTO',
    )
    try_on: StringProperty(
        name="Provar itens",
        description="Ids de itens do jogo para vestir só nesta prévia, separados por vírgula (até 8)",
    )
    import_animations: BoolProperty(
        name="Animações",
        description="Carregar as animações do personagem",
        default=True,
    )


class _AsyncOperator:
    """Run a network call on a thread while the Studio stays responsive."""

    _timer = None
    _thread = None
    _result = None
    _error = None

    def _start(self, context, work):
        self._result = None
        self._error = None

        def run():
            try:
                self._result = work()
            except Exception as ex:
                self._error = ex

        self._thread = threading.Thread(target=run, name="gameforge-preview", daemon=True)
        self._thread.start()
        self._timer = context.window_manager.event_timer_add(0.2, window=context.window)
        context.window_manager.modal_handler_add(self)
        _Cache.busy = True
        _tag_redraw()
        return {'RUNNING_MODAL'}

    def _finish(self, context):
        context.window_manager.event_timer_remove(self._timer)
        _Cache.busy = False
        _tag_redraw()

    def modal(self, context, event):
        if event.type != 'TIMER' or self._thread.is_alive():
            return {'PASS_THROUGH'}
        self._finish(context)
        if self._error is not None:
            if isinstance(self._error, SessionInvalid):
                # Let the account session re-check (and show the login if it really ended).
                gameforge_account.get().retry()
            message = str(self._error) if isinstance(self._error, PortalError) else repr(self._error)
            _Cache.error = message
            self.report({'ERROR'}, message)
            return {'CANCELLED'}
        _Cache.error = ""
        return self._done(context, self._result)


def _token_or_report(op):
    token = gameforge_account.get().session_token()
    if not token:
        op.report({'ERROR'}, "Entre com a conta atomgameforge.com para usar os seus personagens")
    return token


class GAMEFORGE_OT_character_refresh(_AsyncOperator, Operator):
    """Atualizar a lista de personagens salvos na sua conta GameForge"""
    bl_idname = "gameforge.character_refresh"
    bl_label = "Atualizar personagens"

    @classmethod
    def poll(cls, _context):
        return not _Cache.busy

    def invoke(self, context, _event):
        token = _token_or_report(self)
        if not token:
            return {'CANCELLED'}
        _Cache.status = "Buscando os personagens da conta..."
        return self._start(context, lambda: _pwops().characters(token))

    def execute(self, context):
        return self.invoke(context, None)

    def _done(self, context, characters):
        settings = context.window_manager.gameforge_character
        previous = settings.character
        _Cache.characters = characters
        _Cache.items = [
            (str(c.role_id), c.label, c.class_name or "", "USER", i)
            for i, c in enumerate(characters)
        ]
        if _Cache.items:
            ids = [item[0] for item in _Cache.items]
            settings.character = previous if previous in ids else ids[0]
            _Cache.status = "{:d} personagem(ns) na conta".format(len(characters))
        else:
            _Cache.status = "Nenhum personagem salvo na conta. Vincule uma conta do jogo no PWOps."
        return {'FINISHED'}


class GAMEFORGE_OT_character_preview(_AsyncOperator, Operator):
    """Montar o personagem escolhido na cena, com os modelos do cliente do jogo"""
    bl_idname = "gameforge.character_preview"
    bl_label = "Montar personagem"
    bl_options = {'REGISTER', 'UNDO'}

    @classmethod
    def poll(cls, context):
        if _Cache.busy or not _Cache.items:
            return False
        return context.window_manager.gameforge_character.character != "NONE"

    def invoke(self, context, _event):
        settings = context.window_manager.gameforge_character
        if not settings.client_root or not os.path.isdir(bpy.path.abspath(settings.client_root)):
            self.report({'ERROR'}, "Escolha a pasta do cliente do jogo (element)")
            return {'CANCELLED'}
        token = _token_or_report(self)
        if not token:
            return {'CANCELLED'}
        role_id = int(settings.character)
        try_on = parse_try_on(settings.try_on)
        fashion = {'ON': True, 'OFF': False}.get(settings.fashion)
        _Cache.status = "Buscando a aparência do personagem..."
        return self._start(
            context, lambda: _pwops().appearance(token, role_id, try_on=try_on, fashion=fashion))

    def execute(self, context):
        return self.invoke(context, None)

    def _done(self, context, spec_json):
        settings = context.window_manager.gameforge_character
        character = next((c for c in _Cache.characters if str(c.role_id) == settings.character), None)
        name = character.name if character and character.name else "Personagem"
        result = bpy.ops.wm.angelica_character_import(
            spec_json=spec_json,
            client_root=bpy.path.abspath(settings.client_root),
            name=name,
            import_animations=settings.import_animations,
        )
        if 'FINISHED' not in result:
            _Cache.status = ""
            return {'CANCELLED'}
        _Cache.status = "{:s} montado na cena".format(name)
        return {'FINISHED'}


def panel_status():
    """What the panel shows under its buttons: ``(text, is_error)``."""
    if _Cache.error:
        return _Cache.error, True
    return _Cache.status, False


def has_characters():
    return bool(_Cache.items)


def is_busy():
    return _Cache.busy


classes = (
    GameForgeCharacterSettings,
    GAMEFORGE_OT_character_refresh,
    GAMEFORGE_OT_character_preview,
)


def register():
    # The classes are registered by `bl_operators` (from `classes`), before this runs.
    bpy.types.WindowManager.gameforge_character = PointerProperty(type=GameForgeCharacterSettings)

    def restore_client_root():
        wm = bpy.context.window_manager
        if wm is not None and not wm.gameforge_character.client_root:
            saved = _load_saved_client_root()
            if saved:
                wm.gameforge_character.client_root = saved
        return None

    bpy.app.timers.register(restore_client_root, first_interval=0.5, persistent=True)


def unregister():
    del bpy.types.WindowManager.gameforge_character
