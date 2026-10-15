# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
The Studio's own updates (``gameforge_update``). Once the account is authorized, a background check asks the
portal for versions newer than the installed one; the top bar then offers "Atualização <versão>", whose dialog shows
what changed and, when confirmed, downloads and checks the package. "Instalar e reiniciar" starts the applier and
closes the Studio. Development builds (no ``gameforge-version.json``) never check.
"""

__all__ = (
    "GAMEFORGE_OT_update_check",
    "GAMEFORGE_OT_update_show",
    "GAMEFORGE_OT_update_install",
    "GAMEFORGE_OT_update_installer_page",
)

import os
import textwrap
import threading
import time

import bpy
from bpy.types import Operator

import gameforge_account
import gameforge_update
from gameforge_update import versions

_TIMER_SEC = 10.0
_RECHECK_SEC = 6 * 3600.0


class _Update:
    lock = threading.Lock()
    thread = None
    next_check = 0.0
    current = None          # The installed {"version", "base_version"}; None in development builds.
    available = []          # Newer versions [{"version", "changelog", "published_at"}], oldest first.
    busy = ""               # What runs in the background ("Baixando a atualização 40%").
    staged = None           # (package, envelope, version): downloaded, checked, waiting for "Instalar".
    needs_installer = ""    # The base version of an update this installation cannot take.
    message = ""            # The last error, for the dialog.
    menus_added = False
    result_shown = False


def install_dir():
    return os.path.dirname(bpy.app.binary_path)


def _start_thread(target, *args):
    with _Update.lock:
        if _Update.thread is not None and _Update.thread.is_alive():
            return False
        _Update.thread = threading.Thread(target=target, args=args, name="gameforge-update", daemon=True)
        _Update.thread.start()
    return True


def _check(token):
    try:
        listing = gameforge_account.get()._portal.plugin_versions(gameforge_update.SLUG, token)
        _Update.available = versions.newer(listing, _Update.current["version"])
        _Update.message = ""
    except Exception as ex:  # noqa: BLE001 - offline, or nothing published yet: the next check tries again
        _Update.message = str(ex)
    _Update.next_check = time.monotonic() + _RECHECK_SEC


def start_check(force=False):
    """A background check when the account is authorized. False when it cannot run."""
    token = gameforge_account.get().session_token()
    if _Update.current is None or not token:
        return False
    if force or time.monotonic() >= _Update.next_check:
        _start_thread(_check, token)
    return True


def _download(token):
    def progress(done, total):
        _Update.busy = ("Baixando a atualização {:d}%".format(min(99, done * 100 // total)) if total else
                        "Baixando a atualização...")

    _Update.busy = "Baixando a atualização..."
    try:
        _Update.staged = gameforge_update.download(
            gameforge_account.get()._portal, token, _Update.current["version"], progress)
        _Update.message = ""
    except gameforge_update.NeedsInstaller as ex:
        _Update.needs_installer = str(ex)
    except Exception as ex:  # noqa: BLE001 - shown in the dialog; the next try starts over
        _Update.message = str(ex)
    _Update.busy = ""


def _draw_topbar(self, context):
    if context.region.alignment != 'RIGHT':  # The upper bar draws a left and a right region.
        return
    layout = self.layout
    if _Update.staged is not None:
        layout.operator("gameforge.update_install", text="Instalar a {:s} e reiniciar".format(_Update.staged[2]),
                        icon='FILE_REFRESH', translate=False)
    elif _Update.busy:
        layout.label(text=_Update.busy, icon='SORTTIME', translate=False)
    elif _Update.needs_installer:
        layout.operator("gameforge.update_installer_page", text="Baixe o instalador novo", icon='ERROR',
                        translate=False)
    elif _Update.available:
        layout.operator("gameforge.update_show", text="Atualização " + str(_Update.available[-1]["version"]),
                        icon='IMPORT', translate=False)


def _draw_menu(self, _context):
    if _Update.current is None:
        return
    self.layout.separator()
    self.layout.operator("gameforge.update_check", icon='FILE_REFRESH', translate=False)


def _redraw_topbars():
    for window in bpy.context.window_manager.windows:
        for area in window.screen.areas:
            if area.type == 'TOPBAR':
                area.tag_redraw()


def _show_result(text):
    windows = bpy.context.window_manager.windows
    if not windows:
        return

    def draw(self, _context):
        self.layout.label(text=text, translate=False)

    with bpy.context.temp_override(window=windows[0]):
        bpy.context.window_manager.popup_menu(draw, title="Atualização do GameForge 3D Studio",
                                              icon='ERROR' if text.startswith("falhou") else 'INFO')


def _tick():
    if not _Update.menus_added and hasattr(bpy.types, "TOPBAR_HT_upper_bar") and hasattr(bpy.types, "TOPBAR_MT_blender"):
        # The top bar registers after the operators: the entries are added on the first tick.
        bpy.types.TOPBAR_HT_upper_bar.append(_draw_topbar)
        bpy.types.TOPBAR_MT_blender.append(_draw_menu)
        _Update.menus_added = True
    if not _Update.result_shown:
        _Update.result_shown = True
        result = gameforge_update.last_result()
        if result:
            _show_result(result)
    try:
        start_check()
    except Exception as ex:  # noqa: BLE001
        print("GameForge: falha ao procurar atualizações:", ex)
    _redraw_topbars()
    return 1.0 if _Update.busy else _TIMER_SEC


class GAMEFORGE_OT_update_check(Operator):
    """Look for a new version of the GameForge 3D Studio at atomgameforge.com"""
    bl_idname = "gameforge.update_check"
    bl_label = "Procurar atualizações"

    def execute(self, _context):
        if not start_check(force=True):
            self.report({'WARNING'}, "Entre com a conta atomgameforge.com para procurar atualizações")
            return {'CANCELLED'}
        self.report({'INFO'}, "Procurando atualizações do GameForge 3D Studio...")
        return {'FINISHED'}


class GAMEFORGE_OT_update_show(Operator):
    """See what is new in the GameForge 3D Studio and download the update"""
    bl_idname = "gameforge.update_show"
    bl_label = "Atualização do GameForge 3D Studio"

    @classmethod
    def poll(cls, _context):
        return bool(_Update.available) and _Update.current is not None

    def invoke(self, context, _event):
        return context.window_manager.invoke_props_dialog(self, width=560, confirm_text="Baixar agora")

    def draw(self, _context):
        layout = self.layout
        layout.label(text="Você tem a versão {:s}. O que há de novo:".format(_Update.current["version"]),
                     translate=False)
        for item in reversed(_Update.available):
            box = layout.box()
            box.label(text="Versão " + str(item["version"]), icon='INFO', translate=False)
            col = box.column(align=True)
            col.scale_y = 0.85
            for line in str(item.get("changelog") or "").splitlines():
                for piece in textwrap.wrap(line, 90) or [""]:
                    col.label(text=piece, translate=False)
        if _Update.message:
            layout.label(text=_Update.message, icon='ERROR', translate=False)
        layout.label(text="Depois de baixar, você escolhe quando instalar.", icon='INFO', translate=False)

    def execute(self, _context):
        token = gameforge_account.get().session_token()
        if not token:
            self.report({'WARNING'}, "Entre com a conta atomgameforge.com para baixar a atualização")
            return {'CANCELLED'}
        _Update.message = ""
        _start_thread(_download, token)
        return {'FINISHED'}


class GAMEFORGE_OT_update_install(Operator):
    """Close the Studio and install the update: Windows asks for permission once and the Studio opens again"""
    bl_idname = "gameforge.update_install"
    bl_label = "Instalar a atualização"

    @classmethod
    def poll(cls, _context):
        return _Update.staged is not None

    def invoke(self, context, event):
        return context.window_manager.invoke_confirm(
            self, event, title="Instalar a versão {:s}?".format(_Update.staged[2]),
            message="O Studio fecha (perguntando se quer salvar), o Windows pede permissão e o Studio abre de novo "
                    "atualizado. Se você cancelar o fechamento, a atualização é instalada quando o Studio fechar.",
            confirm_text="Instalar e reiniciar")

    def execute(self, _context):
        package_path, envelope_path, _version = _Update.staged
        gameforge_update.start_install(install_dir(), package_path, envelope_path)
        _Update.staged = None
        bpy.ops.wm.quit_blender('INVOKE_DEFAULT')
        return {'FINISHED'}


class GAMEFORGE_OT_update_installer_page(Operator):
    """This update needs the full installer: open the page to download it"""
    bl_idname = "gameforge.update_installer_page"
    bl_label = "Baixar o instalador"

    def execute(self, _context):
        bpy.ops.wm.url_open(url=gameforge_update.INSTALLER_PAGE)
        return {'FINISHED'}


classes = (
    GAMEFORGE_OT_update_check,
    GAMEFORGE_OT_update_show,
    GAMEFORGE_OT_update_install,
    GAMEFORGE_OT_update_installer_page,
)


def register():
    # The classes are registered by `bl_operators` (from `classes`), before this runs.
    if bpy.app.background:
        return
    _Update.current = versions.installed(install_dir())
    if not bpy.app.timers.is_registered(_tick):
        bpy.app.timers.register(_tick, first_interval=_TIMER_SEC, persistent=True)


def unregister():
    if bpy.app.timers.is_registered(_tick):
        bpy.app.timers.unregister(_tick)
    if _Update.menus_added:  # At exit the top bar may already be unregistered.
        for name, function in (("TOPBAR_HT_upper_bar", _draw_topbar), ("TOPBAR_MT_blender", _draw_menu)):
            owner = getattr(bpy.types, name, None)
            if owner is not None:
                owner.remove(function)
        _Update.menus_added = False
