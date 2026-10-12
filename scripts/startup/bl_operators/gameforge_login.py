# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Login screen contents and the account gate of the GameForge 3D Studio.

The popup itself is native (``WM_OT_gameforge_login`` in ``wm_gameforge_login.cc``); this module
draws its contents (``WM_MT_gameforge_login``), offers the actions, and runs the gate: a timer that
reopens the login whenever the session is not authorized and closes it once it is.
"""

__all__ = (
    "WM_MT_gameforge_login",
    "WM_OT_gameforge_account",
)

import bpy
from bpy.props import EnumProperty
from bpy.types import (
    Menu,
    Operator,
)

import gameforge_account
from gameforge_account.session import State

_TIMER_INTERVAL_SEC = 0.25
_TIMER_INTERVAL_AUTHORIZED_SEC = 1.0


class _Gate:
    """What the timer remembers between ticks."""
    revision = -1
    opened_browser_for = ""
    was_authorized = False
    splash_pending = True  # Show the splash after the first sign in, as on a normal startup.
    menu_added = False


def _session():
    return gameforge_account.get()


def _draw_centered_label(layout, text, *, scale=1.0, icon='NONE'):
    row = layout.row()
    row.alignment = 'CENTER'
    row.scale_y = scale
    row.label(text=text, icon=icon, translate=False)


class WM_MT_gameforge_login(Menu):
    bl_label = "GameForge Login"

    def draw(self, context):
        layout = self.layout
        layout.operator_context = 'EXEC_DEFAULT'
        snap = _session().state
        state = snap.state

        layout.separator()
        _draw_centered_label(layout, "Entre com a sua conta GameForge", scale=1.4)
        layout.separator()

        col = layout.column(align=True)

        if state is State.CHECKING:
            _draw_centered_label(col, snap.message or "Verificando sua conta...", icon='TIME')

        elif state is State.SIGNED_OUT:
            if snap.message:
                _draw_centered_label(col, snap.message, icon='INFO')
                col.separator()
            _draw_centered_label(col, "Use a mesma conta de atomgameforge.com (login com Discord).")
            col.separator()
            row = col.row()
            row.scale_y = 1.6
            row.operator(
                "wm.gameforge_account", text="Entrar com a conta GameForge", icon='URL', translate=False,
            ).action = 'BEGIN'

        elif state is State.WAITING_APPROVAL:
            if not snap.user_code:
                _draw_centered_label(col, snap.message or "Gerando o código...", icon='TIME')
            else:
                _draw_centered_label(col, "No navegador, entre e confirme este código:")
                col.separator()
                _draw_centered_label(col, snap.user_code, scale=2.5)
                _draw_centered_label(col, snap.verification_uri)
                col.separator()
                row = col.row(align=True)
                row.operator(
                    "wm.gameforge_account", text="Abrir no navegador", icon='URL', translate=False,
                ).action = 'OPEN_LINK'
                row.operator(
                    "wm.gameforge_account", text="Copiar código", icon='COPYDOWN', translate=False,
                ).action = 'COPY_CODE'
                col.separator()
                _draw_centered_label(col, "Aguardando a confirmação...", icon='TIME')
            col.separator()
            col.operator("wm.gameforge_account", text="Cancelar", translate=False).action = 'CANCEL'

        elif state is State.AUTHORIZED:
            _draw_centered_label(col, "Tudo certo. Abrindo o Studio...", icon='CHECKMARK')

        elif state is State.NOT_INCLUDED:
            _draw_centered_label(col, snap.message, icon='ERROR')
            if snap.plan_name:
                _draw_centered_label(col, "Seu plano atual: " + snap.plan_name)
            col.separator()
            row = col.row()
            row.scale_y = 1.4
            row.operator(
                "wm.gameforge_account", text="Ver planos", icon='URL', translate=False,
            ).action = 'OPEN_PLANS'
            row = col.row(align=True)
            row.operator("wm.gameforge_account", text="Verificar de novo", translate=False).action = 'RETRY'
            row.operator("wm.gameforge_account", text="Entrar com outra conta", translate=False).action = 'SIGN_OUT'

        elif state is State.OFFLINE:
            _draw_centered_label(col, "Sem conexão com atomgameforge.com.", icon='ERROR')
            if snap.message:
                _draw_centered_label(col, snap.message)
            col.separator()
            row = col.row()
            row.scale_y = 1.4
            row.operator("wm.gameforge_account", text="Tentar de novo", translate=False).action = 'RETRY'

        layout.separator()
        row = layout.row(align=True)
        if _Gate.was_authorized and bpy.data.is_dirty:
            # Access ended while working: never lose the open file.
            row.operator_context = 'INVOKE_DEFAULT'
            row.operator("wm.save_mainfile", text="Salvar o arquivo", icon='FILE_TICK', translate=False)
        row.operator_context = 'INVOKE_DEFAULT'
        row.operator("wm.quit_blender", text="Fechar o Studio", icon='QUIT', translate=False)
        layout.separator()


class WM_OT_gameforge_account(Operator):
    """Sign in, sign out, or manage the GameForge account"""
    bl_idname = "wm.gameforge_account"
    bl_label = "GameForge Account"
    bl_options = {'INTERNAL'}

    action: EnumProperty(
        items=(
            ('BEGIN', "Sign In", ""),
            ('CANCEL', "Cancel", ""),
            ('OPEN_LINK', "Open Link", ""),
            ('COPY_CODE', "Copy Code", ""),
            ('OPEN_PLANS', "Plans", ""),
            ('RETRY', "Retry", ""),
            ('SIGN_OUT', "Sign Out", ""),
        ),
    )

    def execute(self, context):
        session = _session()
        snap = session.state
        action = self.action
        if action == 'BEGIN':
            session.begin_login()
        elif action == 'CANCEL':
            session.cancel_login()
        elif action == 'OPEN_LINK':
            if snap.verification_uri_complete:
                bpy.ops.wm.url_open(url=snap.verification_uri_complete)
        elif action == 'COPY_CODE':
            context.window_manager.clipboard = snap.user_code
        elif action == 'OPEN_PLANS':
            bpy.ops.wm.url_open(url=session._portal.config.plans_url)
        elif action == 'RETRY':
            session.retry()
        elif action == 'SIGN_OUT':
            session.sign_out()
        _refresh_login()
        return {'FINISHED'}


def _first_window():
    wm = getattr(bpy.context, "window_manager", None)
    if wm is None or not wm.windows:
        return None
    return wm.windows[0]


def _with_window(fn):
    win = _first_window()
    if win is None:
        return None
    with bpy.context.temp_override(window=win, screen=win.screen):
        return fn()


def _refresh_login():
    def refresh():
        if bpy.ops.wm.gameforge_login_refresh.poll():
            bpy.ops.wm.gameforge_login_refresh()
    _with_window(refresh)


def _gate_tick():
    if bpy.app.background:
        return None
    # The top bar menus register after the operators, so the account entry is added on the first tick.
    if not _Gate.menu_added and hasattr(bpy.types, "TOPBAR_MT_blender"):
        bpy.types.TOPBAR_MT_blender.append(_draw_account_menu)
        _Gate.menu_added = True
    snap = _session().state

    def tick():
        login_open = bpy.ops.wm.gameforge_login_close.poll()
        if snap.authorized:
            if login_open:
                bpy.ops.wm.gameforge_login_close()
            if not _Gate.was_authorized:
                _Gate.was_authorized = True
                if _Gate.splash_pending and bpy.context.preferences.view.show_splash and not bpy.data.filepath:
                    bpy.ops.wm.splash('INVOKE_DEFAULT')
            _Gate.splash_pending = False
            return

        # Open the browser by itself the first time a code shows up.
        if snap.state is State.WAITING_APPROVAL and snap.user_code and _Gate.opened_browser_for != snap.user_code:
            _Gate.opened_browser_for = snap.user_code
            bpy.ops.wm.url_open(url=snap.verification_uri_complete)

        if not login_open:
            if bpy.ops.wm.gameforge_login.poll():
                bpy.ops.wm.gameforge_login('INVOKE_DEFAULT')
                _Gate.revision = snap.revision
        elif snap.revision != _Gate.revision:
            _Gate.revision = snap.revision
            bpy.ops.wm.gameforge_login_refresh()

    try:
        _with_window(tick)
    except Exception as ex:
        print("GameForge: falha no login:", ex)
    return _TIMER_INTERVAL_AUTHORIZED_SEC if snap.authorized else _TIMER_INTERVAL_SEC


def _draw_account_menu(self, _context):
    snap = _session().state
    if not snap.authorized:
        return
    layout = self.layout
    layout.separator()
    layout.label(text="Conta GameForge: " + (snap.plan_name or snap.plan_code), icon='USER', translate=False)
    layout.operator("wm.gameforge_account", text="Sair da conta", translate=False).action = 'SIGN_OUT'


classes = (
    WM_MT_gameforge_login,
    WM_OT_gameforge_account,
)


def register():
    if not gameforge_account.is_required() or bpy.app.background:
        return
    if not bpy.app.timers.is_registered(_gate_tick):
        bpy.app.timers.register(_gate_tick, first_interval=_TIMER_INTERVAL_SEC, persistent=True)


def unregister():
    if bpy.app.timers.is_registered(_gate_tick):
        bpy.app.timers.unregister(_gate_tick)
    if _Gate.menu_added:
        bpy.types.TOPBAR_MT_blender.remove(_draw_account_menu)
        _Gate.menu_added = False
