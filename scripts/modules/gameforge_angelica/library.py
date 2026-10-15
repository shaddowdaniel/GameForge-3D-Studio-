# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
The files of a game client grouped by container: every ``.pck`` package (``models.pck``,
``help\\script.pck``...) and every loose folder of the client (``maps``, ``data``...).

Package file tables come from the native ``_angelica`` module the first time a container is
expanded or searched; folder trees are built on demand, one level at a time, so even packages with
hundreds of thousands of files stay responsive.
"""

__all__ = (
    "Container",
    "Library",
    "KINDS",
    "file_kind",
    "get",
    "human_size",
)

import bisect
import os

# Asset kinds shown by the library: (label, icon, extensions).
KINDS = {
    "MODEL": ("Modelos", 'OUTLINER_OB_ARMATURE', {".ecm", ".smd"}),
    "SKIN": ("Malhas", 'MESH_DATA', {".ski"}),
    "SKELETON": ("Esqueletos", 'BONE_DATA', {".bon"}),
    "ANIMATION": ("Animações", 'ACTION', {".stck"}),
    "TEXTURE": ("Texturas", 'IMAGE_DATA', {".dds", ".tga", ".bmp", ".jpg", ".jpeg", ".png"}),
    "EFFECT": ("Efeitos", 'PARTICLES', {".gfx", ".att", ".sgc"}),
    "SOUND": ("Sons", 'SOUND', {".wav", ".ogg", ".mp3"}),
    "BUILDING": ("Construções", 'HOME', {".bmd", ".mox", ".chf"}),
    "MAP": ("Mapas", 'WORLD', {".trn2", ".ecwld", ".ecbsd", ".hmap", ".t2bk", ".t2mk", ".t2lm", ".t2lm1"}),
    "DATA": ("Dados", 'PRESET', {".data", ".clt", ".sev"}),
    "TEXT": ("Textos", 'TEXT', {".txt", ".xml", ".ini", ".cfg", ".lua", ".stf", ".dtf", ".htf"}),
}
_EXTENSION_KIND = {ext: kind for kind, (_label, _icon, exts) in KINDS.items() for ext in exts}


def file_kind(path):
    return _EXTENSION_KIND.get(os.path.splitext(path)[1].lower(), "OTHER")


def human_size(size):
    for unit in ("B", "KB", "MB", "GB"):
        if size < 1024 or unit == "GB":
            return "{:.0f} {:s}".format(size, unit) if unit == "B" else "{:.1f} {:s}".format(size, unit)
        size /= 1024.0
    return ""


class Container:
    """A package or a loose folder of the client, with its files (engine paths)."""

    def __init__(self, kind, name, path, mount):
        # 'PACKAGE' or 'FOLDER'.
        self.kind = kind
        self.name = name
        # The package file, or the folder on disk.
        self.path = path
        # Engine folder it holds, for example "models" or "help\\script".
        self.mount = mount
        self._keys = None
        self._files = None
        self._children = {}

    @property
    def loaded(self):
        return self._files is not None

    @property
    def root_prefix(self):
        return self.mount.lower() + "\\"

    def files(self):
        """Every file as ``(engine_path, size)``, sorted case-insensitively."""
        if self._files is None:
            items = self._load()
            items.sort(key=lambda item: item[0].lower())
            self._files = items
            self._keys = [path.lower() for path, _size in items]
        return self._files

    def _load(self):
        if self.kind == 'PACKAGE':
            import _angelica
            prefix = self.mount + "\\"
            items = []
            for path, size, _stored in _angelica.package_entries(self.path):
                # Entries are stored with or without the package folder, depending on the tool.
                if not path.lower().startswith(prefix.lower()):
                    path = prefix + path
                items.append((path, size))
            return items
        items = []
        base = os.path.dirname(self.path)
        for dirpath, _dirnames, filenames in os.walk(self.path):
            for name in filenames:
                lower = name.lower()
                if lower.endswith(".pck") or ".pkx" in lower:
                    continue
                full = os.path.join(dirpath, name)
                try:
                    size = os.path.getsize(full)
                except OSError:
                    continue
                items.append((os.path.relpath(full, base).replace("/", "\\"), size))
        return items

    def children(self, prefix):
        """
        The content of one folder (``prefix`` is a lower-case engine path ending with ``\\``):
        ``(folders, files)`` with folders as ``(name, prefix, file_count)`` and files as
        ``(name, engine_path, size)``.
        """
        cached = self._children.get(prefix)
        if cached is not None:
            return cached
        files = self.files()
        keys = self._keys
        start = bisect.bisect_left(keys, prefix)
        end = bisect.bisect_left(keys, prefix + "￿")
        folder_counts = {}
        folder_names = {}
        direct = []
        for i in range(start, end):
            rest = keys[i][len(prefix):]
            sep = rest.find("\\")
            if sep >= 0:
                key = rest[:sep]
                folder_counts[key] = folder_counts.get(key, 0) + 1
                if key not in folder_names:
                    path = files[i][0]
                    folder_names[key] = path[len(prefix):len(prefix) + sep]
            else:
                path, size = files[i]
                direct.append((path[len(prefix):], path, size))
        folders = [(folder_names[key], prefix + key + "\\", folder_counts[key]) for key in sorted(folder_counts)]
        result = (folders, direct)
        self._children[prefix] = result
        return result


class Library:
    """The containers of one client."""

    def __init__(self, root):
        self.root = root
        self.containers = _scan(root)

    def search(self, text, kind=None, limit=500):
        """Files whose path contains ``text`` (case insensitive): ``([(container, path, size)], truncated)``."""
        text = text.lower().replace("/", "\\")
        results = []
        for container in self.containers:
            for path, size in container.files():
                if text in path.lower() and (kind is None or file_kind(path) == kind):
                    results.append((container, path, size))
                    if len(results) >= limit:
                        return results, True
        return results, False


def _scan(root):
    containers = []
    mounts = set()
    for dirpath, dirnames, filenames in os.walk(root):
        rel = os.path.relpath(dirpath, root)
        depth = 0 if rel == "." else rel.count(os.sep) + 1
        if depth > 1:
            dirnames[:] = []
            continue
        for name in sorted(filenames, key=str.lower):
            if name.lower().endswith(".pck"):
                full = os.path.join(dirpath, name)
                mount = os.path.splitext(os.path.relpath(full, root))[0].replace("/", "\\")
                containers.append(Container('PACKAGE', mount + ".pck", full, mount))
                mounts.add(mount.lower())
    containers.sort(key=lambda c: c.name.lower())
    # Loose folders of the client that no package covers (maps, data, ...).
    try:
        names = sorted(os.listdir(root), key=str.lower)
    except OSError:
        names = []
    for name in names:
        full = os.path.join(root, name)
        if os.path.isdir(full) and name.lower() not in mounts:
            containers.append(Container('FOLDER', name, full, name))
    return containers


_libraries = {}


def get(root, refresh=False):
    """The ``Library`` of a client folder, scanned once and kept until ``refresh``."""
    key = os.path.normcase(os.path.normpath(root))
    library = _libraries.get(key)
    if library is None or refresh:
        library = Library(root)
        _libraries[key] = library
    return library
