# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Keeps the Studio plugins from atomgameforge.com up to date (see ``gameforge_account.plugins``).

Once the account is authorized, a background thread installs the current version of each plugin
(today the ``.data`` schemas used by the Angelica importers and data editors) and checks again
every few hours. ``WM_OT_gameforge_plugins_update`` does the same on request.
"""

__all__ = (
    "WM_OT_gameforge_plugins_update",
)

import threading
import time

import bpy
from bpy.types import Operator

import gameforge_account
from gameforge_account.plugins import SCHEMAS_PLUGIN_SLUG, PluginCache

PLUGIN_SLUGS = (SCHEMAS_PLUGIN_SLUG,)

_TIMER_INTERVAL_SEC = 5.0
_RECHECK_INTERVAL_SEC = 6 * 3600.0
_RETRY_INTERVAL_SEC = 10 * 60.0


class _Sync:
    lock = threading.Lock()
    thread = None
    next_check = 0.0
    message = ""


def cache_dir():
    return bpy.utils.user_resource('DATAFILES', path="gameforge_plugins", create=True)


def _run(token, directory):
    session = gameforge_account.get()
    cache = PluginCache(session._portal, directory)
    ok = True
    messages = []
    for slug in PLUGIN_SLUGS:
        try:
            _path, version, updated = cache.sync(slug, token)
            messages.append("{:s} {:s}{:s}".format(slug, version, " (atualizado)" if updated else ""))
        except Exception as ex:
            ok = False
            messages.append("{:s}: {:s}".format(slug, str(ex)))
    _Sync.message = "; ".join(messages)
    print("GameForge plugins:", _Sync.message)
    _Sync.next_check = time.monotonic() + (_RECHECK_INTERVAL_SEC if ok else _RETRY_INTERVAL_SEC)


def start_sync(force=False):
    """Start a background sync when the account is authorized. Returns False when it cannot start."""
    token = gameforge_account.get().authorized_token()
    if not token:
        return False
    with _Sync.lock:
        if _Sync.thread is not None and _Sync.thread.is_alive():
            return True
        if not force and time.monotonic() < _Sync.next_check:
            return True
        # The cache folder is resolved here: `bpy` is not used from the worker thread.
        directory = cache_dir()
        _Sync.thread = threading.Thread(
            target=_run, args=(token, directory), name="gameforge-plugins", daemon=True)
        _Sync.thread.start()
    return True


def _tick():
    try:
        start_sync()
    except Exception as ex:
        print("GameForge plugins: falha ao sincronizar:", ex)
    return _TIMER_INTERVAL_SEC


class WM_OT_gameforge_plugins_update(Operator):
    """Download the latest GameForge Studio plugins (game data schemas) from atomgameforge.com"""
    bl_idname = "wm.gameforge_plugins_update"
    bl_label = "Update GameForge Plugins"

    def execute(self, _context):
        if not start_sync(force=True):
            self.report({'WARNING'}, "Entre com a conta atomgameforge.com para baixar os plugins")
            return {'CANCELLED'}
        self.report({'INFO'}, "Atualizando os plugins do GameForge em segundo plano")
        return {'FINISHED'}


classes = (
    WM_OT_gameforge_plugins_update,
)


def register():
    if bpy.app.background:
        return
    if not bpy.app.timers.is_registered(_tick):
        bpy.app.timers.register(_tick, first_interval=_TIMER_INTERVAL_SEC, persistent=True)


def unregister():
    if bpy.app.timers.is_registered(_tick):
        bpy.app.timers.unregister(_tick)
