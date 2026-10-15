# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""The game clients the user added to the Studio, kept in ``gameforge_clients.json``."""

__all__ = (
    "Client",
    "add",
    "client_root",
    "load",
    "remove",
    "set_server",
)

import json
import os
from dataclasses import dataclass

_FILE_NAME = "gameforge_clients.json"


@dataclass(frozen=True)
class Client:
    name: str
    # The client data folder (``element``), where the ``.pck`` packages are.
    root: str
    # The folder of the matching server's data files (``gamed\config``), when known.
    server: str = ""


def _config_path():
    import bpy
    return os.path.join(bpy.utils.user_resource('CONFIG'), _FILE_NAME)


def load():
    try:
        with open(_config_path(), "r", encoding="utf-8") as fh:
            items = json.load(fh).get("clients", [])
    except (OSError, ValueError, AttributeError):
        return []
    clients = []
    for item in items:
        if isinstance(item, dict) and item.get("root"):
            clients.append(Client(name=str(item.get("name") or item["root"]), root=str(item["root"]),
                                  server=str(item.get("server") or "")))
    return clients


def _save(clients):
    path = _config_path()
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as fh:
        json.dump({"clients": [{"name": c.name, "root": c.root, "server": c.server} for c in clients]},
                  fh, ensure_ascii=False, indent=1)


def client_root(folder):
    """
    The client data folder for a folder the user picked: the folder itself when it holds packages,
    otherwise an ``element`` folder inside it (the game's install folder). None when neither has
    any ``.pck`` package.
    """
    folder = os.path.normpath(folder)
    candidates = [folder]
    try:
        candidates += [os.path.join(folder, name) for name in os.listdir(folder) if name.lower() == "element"]
    except OSError:
        return None
    for candidate in candidates:
        try:
            if any(name.lower().endswith(".pck") for name in os.listdir(candidate)):
                return candidate
        except OSError:
            continue
    return None


def _default_name(root):
    # `...\PerfectWorld-Unlimited\element` -> "PerfectWorld-Unlimited".
    parent, name = os.path.split(os.path.normpath(root))
    if name.lower() == "element" and parent:
        return os.path.basename(parent) or name
    return name


def add(folder, name=""):
    """Add a client (or rename it when already added). Returns the ``Client`` or None."""
    root = client_root(folder)
    if root is None:
        return None
    existing = [c for c in load() if os.path.normcase(c.root) == os.path.normcase(root)]
    clients = [c for c in load() if os.path.normcase(c.root) != os.path.normcase(root)]
    client = Client(name=name or _default_name(root), root=root, server=existing[0].server if existing else "")
    clients.append(client)
    _save(clients)
    return client


def remove(root):
    _save([c for c in load() if os.path.normcase(c.root) != os.path.normcase(root)])


def set_server(root, folder):
    """Remember the server data folder (with its ``elements.data``) of a client."""
    clients = load()
    for i, c in enumerate(clients):
        if os.path.normcase(c.root) == os.path.normcase(root):
            clients[i] = Client(name=c.name, root=c.root, server=folder)
    _save(clients)
