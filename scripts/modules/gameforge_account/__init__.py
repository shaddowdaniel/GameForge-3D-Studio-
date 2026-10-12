# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
GameForge account: signing in to the Studio with the atomgameforge.com account.

``get()`` returns the process wide ``Session``; the login screen and the rest of the Studio read
its ``state``. See ``portal.py`` for the protocol and ``session.py`` for the life cycle.
"""

__all__ = (
    "get",
    "is_required",
)

import os

_session = None


def is_required():
    """
    The login gate is on by default. ``GAMEFORGE_ACCOUNT_REQUIRED=0`` turns it off for development
    builds and automated tests, which have no account.
    """
    return os.environ.get("GAMEFORGE_ACCOUNT_REQUIRED", "1") != "0"


def get():
    global _session
    if _session is None:
        import platform
        from .portal import Portal
        from .session import Session
        from .vault import Vault

        try:
            import bpy
            config_dir = bpy.utils.user_resource('CONFIG')
            app_version = "GameForge 3D Studio " + bpy.app.version_string
        except ImportError:
            config_dir = os.path.join(os.path.expanduser("~"), ".config", "gameforge")
            app_version = "GameForge 3D Studio"

        portal = Portal(app_version=app_version, os_name="{:s} {:s}".format(platform.system(), platform.release()))
        _session = Session(portal, Vault(config_dir))
    return _session
