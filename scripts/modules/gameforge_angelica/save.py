# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Saving what was edited in the Studio into the game client.

Every data-block opened from a client file knows its client and engine path (see ``sources``).
A save finds the edited ones, turns each into the bytes of its file, groups the files by the
package that holds them (or the loose file) and writes them in one of three ways:

- ``IN_PLACE``: into the client's own packages and files. Each package stays readable by the game
  at every moment, and the save can be undone (:func:`undo_last_save`).
- ``PATCH``: new packages in a folder holding only the edited files (``models.pck``, ...), with
  the client's folder layout.
- ``COPY``: full copies of the touched packages in a folder, with the edited files replaced.

Nothing is written when any edited file cannot be converted, and a failed in-place save undoes the
packages it already wrote, so the client is never left half saved.
"""

__all__ = (
    "BytesChange",
    "Change",
    "EditsChange",
    "SaveError",
    "can_undo",
    "pending_changes",
    "save",
    "target",
    "undo_last_save",
)

import hashlib
import json
import os
import shutil
import time

from . import anims, scene, skins, sources, spawns, textures

MODES = ('IN_PLACE', 'PATCH', 'COPY')
_HISTORY_FILE = "gameforge_saves.json"
_BACKUP_DIR = "gameforge_backups"


class SaveError(Exception):
    pass


class Change:
    """An edited client file: where it goes and how to produce its bytes."""
    __slots__ = ("root", "path", "kind", "id_data", "objects", "warning", "folder")

    def __init__(self, root, path, kind, id_data, warning="", objects=None):
        self.root = root
        # Saving into a folder: subfolder for files that are not the client's (a server's data).
        self.folder = ""
        self.path = path
        self.kind = kind
        self.id_data = id_data
        # Skin files: the edited objects, one per mesh of the file. Track sets: the edited actions.
        self.objects = objects or []
        self.warning = warning

    @property
    def label(self):
        return self.id_data.name

    def _original(self):
        import _angelica
        original = _angelica.read(self.root, self.path)
        if original is None:
            raise SaveError("o cliente não tem mais o arquivo original")
        return original

    def data(self):
        if self.kind == 'TEXT':
            text = self.id_data
            return sources.encode_text(text.as_string(), text[sources.PROP_ENCODING], text[sources.PROP_NEWLINE])
        if self.kind == 'IMAGE':
            if not self.id_data.has_data and tuple(self.id_data.size) == (0, 0):
                raise SaveError("a imagem não tem pixels carregados")
            return textures.encode_like(self.path, self._original(), textures.image_rgba8(self.id_data))
        if self.kind == 'SKIN':
            return skins.export_skin(self._original(), self.objects, self._skeleton_bone_names)
        if self.kind == 'BON':
            return anims.export_skeleton(self._original(), self.id_data)
        if self.kind == 'NPCGEN':
            return spawns.build_npcgen(self.id_data)
        if self.kind == 'SCENE':
            return scene.build_world(self.id_data)
        if self.kind == 'ANIM':
            import _angelica
            data = self._original()
            for action in self.objects:
                armature = _armature_of(action, self.root)
                skeleton = _angelica.read(self.root, armature[anims.PROP_SKELETON])
                if skeleton is None:
                    raise SaveError("o cliente não tem o esqueleto " + armature[anims.PROP_SKELETON])
                data = anims.export_action(data, skeleton, armature, action)
            return data
        raise SaveError("tipo de dado ainda não salvo no cliente: " + self.kind)

    def _skeleton_bone_names(self):
        """Studio bone name per skeleton bone, for skins that index the skeleton directly."""
        import _angelica
        for ob in self.objects:
            armatures = [ob.parent] + [m.object for m in ob.modifiers if m.type == 'ARMATURE']
            armature = next((a for a in armatures if a is not None and a.get(anims.PROP_SKELETON)), None)
            if armature is not None:
                data = _angelica.read(self.root, armature[anims.PROP_SKELETON])
                if data is None:
                    raise SaveError("o cliente não tem o esqueleto " + armature[anims.PROP_SKELETON])
                return anims._armature_names(armature, _angelica.bon_layout(data)["bones"])
        raise SaveError("a malha não está ligada a um esqueleto do cliente")

    def mark_saved(self, data):
        """After an in-place save: the data-block matches the client again."""
        id_data = self.id_data
        if self.kind == 'TEXT':
            id_data[sources.PROP_FINGERPRINT] = sources.text_fingerprint(id_data.as_string())
        elif self.kind == 'IMAGE':
            # Show what the game shows (the saved file, compression included) and clear the edits.
            id_data.pack(data=data, data_len=len(data))
            # Reload from the packed bytes (`reload()` would first look for the file on disk).
            id_data.buffers_free()
            id_data[sources.PROP_FINGERPRINT] = hashlib.sha1(data).hexdigest()
        elif self.kind == 'SKIN':
            for ob in self.objects:
                ob[skins.PROP_GEOMETRY] = skins.mesh_fingerprint(ob)
        elif self.kind == 'ANIM':
            for action in self.objects:
                action[anims.PROP_KEYS] = anims.action_fingerprint(action)
        elif self.kind == 'BON':
            id_data[anims.PROP_REST] = anims.rest_fingerprint(id_data)
        elif self.kind == 'NPCGEN':
            id_data[spawns.PROP_HASH] = hashlib.sha1(data).hexdigest()
        elif self.kind == 'SCENE':
            scene.mark_placed(id_data)


class BytesChange(Change):
    """A file whose new bytes are already known (a copied record's elements.data, ...)."""
    __slots__ = ("payload",)

    def __init__(self, root, path, payload, kind='COPY'):
        super().__init__(root, path, kind, None)
        self.payload = payload

    @property
    def label(self):
        return self.path

    def data(self):
        return self.payload

    def mark_saved(self, _data):
        pass


class EditsChange(Change):
    """
    A client file rebuilt from edits kept in the scenes (records of elements.data, quests of
    tasks.data), or the same file of the client's server: the client's new bytes when both were
    the same file, otherwise the edits applied to the server's own file. Subclasses give ``EDITS``
    (the scene collection of the edits, each with a ``client``), ``CLIENT_PATH``, ``SERVER_NAME``,
    ``KIND`` and :meth:`build`.
    """
    __slots__ = ("client",)
    EDITS = CLIENT_PATH = SERVER_NAME = KIND = ""

    def __init__(self, root, path, client, scene):
        super().__init__(root, path, self.KIND, scene)
        self.client = client

    @property
    def label(self):
        return "{:s} ({:d} campo(s) editado(s))".format(self.path, len(self._edits()))

    def _edits(self):
        import bpy
        return [e for s in bpy.data.scenes for e in getattr(s, self.EDITS) if e.client == self.client]

    def build(self, data, edits, where):
        """``data`` with ``edits`` applied (``where`` names the file in messages). Raises SaveError."""
        raise NotImplementedError

    def data(self):
        import _angelica
        original = _angelica.read(self.client, self.CLIENT_PATH)
        if original is None:
            raise SaveError("o cliente não tem " + self.CLIENT_PATH)
        client_bytes = self.build(original, self._edits(), "no cliente")
        if self.root == self.client:
            return client_bytes
        with open(os.path.join(self.root, self.path), "rb") as fh:
            server_original = fh.read()
        if server_original == original:
            return client_bytes
        return self.build(server_original, self._edits(), "no arquivo do servidor")

    def mark_saved(self, _data):
        import bpy
        for s in bpy.data.scenes:
            edits = getattr(s, self.EDITS)
            for i in reversed(range(len(edits))):
                if edits[i].client == self.client:
                    edits.remove(i)

    @classmethod
    def changes(cls, scenes):
        """The changes of every client with edits, and of its server when it has the file."""
        from . import clients
        scenes = list(scenes)
        found = []
        for client in sorted({e.client for s in scenes for e in getattr(s, cls.EDITS)}):
            found.append(cls(client, cls.CLIENT_PATH, client, scenes[0]))
            server = next((c.server for c in clients.load() if c.root == client), "")
            if server and os.path.isfile(os.path.join(server, cls.SERVER_NAME)):
                change = cls(server, cls.SERVER_NAME, client, scenes[0])
                change.folder = "servidor"
                found.append(change)
        return found


def _armature_of(action, root):
    """The armature an action animates: the one playing it, or the one its slot was made for."""
    import bpy
    candidates = [ob for ob in bpy.data.objects
                  if ob.type == 'ARMATURE' and ob.get(anims.PROP_SKELETON) and ob.get(anims.PROP_CLIENT) == root]
    for ob in candidates:
        if ob.animation_data and ob.animation_data.action == action:
            return ob
    identifiers = {slot.identifier for slot in action.slots}
    for ob in candidates:
        if "OB" + ob.name in identifiers:
            return ob
    raise SaveError("a animação '{:s}' não está ligada a um esqueleto do cliente".format(action.name))


def _source(id_data):
    root = id_data.get(sources.PROP_CLIENT)
    path = id_data.get(sources.PROP_FILE)
    if isinstance(root, str) and isinstance(path, str) and root and path:
        return root, path
    return None


def _image_edited(image):
    if image.is_dirty:
        return True
    if image.packed_file is None or not image.filepath_raw.startswith("//angelica/"):
        return True  # Replaced by another image file.
    fingerprint = image.get(sources.PROP_FINGERPRINT)
    # Saved and then undone: the packed bytes are the saved ones, not the client's.
    return fingerprint is not None and hashlib.sha1(image.packed_file.data).hexdigest() != fingerprint


def pending_changes():
    """The client files edited in the Studio, as a list of :class:`Change`."""
    import bpy
    changes = []
    for text in bpy.data.texts:
        source = _source(text)
        if source is None or sources.PROP_FINGERPRINT not in text:
            continue
        if sources.text_fingerprint(text.as_string()) == text[sources.PROP_FINGERPRINT]:
            continue
        warning = "" if text.get(sources.PROP_EXACT, True) else \
            "o arquivo original tinha quebras de linha misturadas: linhas não editadas podem mudar"
        changes.append(Change(source[0], source[1], 'TEXT', text, warning))
    for image in bpy.data.images:
        source = _source(image)
        if source is not None and _image_edited(image):
            changes.append(Change(source[0], source[1], 'IMAGE', image))
    for (root, path), objects in skins.skin_objects(bpy.data.objects).items():
        if not root:
            continue
        edited = [ob for ob in objects
                  if ob.get(skins.PROP_GEOMETRY) is not None and
                  skins.mesh_fingerprint(ob) != ob[skins.PROP_GEOMETRY]]
        if edited:
            changes.append(Change(root, path, 'SKIN', edited[0], objects=edited))
    for ob in bpy.data.objects:
        if ob.type == 'ARMATURE' and ob.get(anims.PROP_CLIENT) and ob.get(anims.PROP_SKELETON) and \
                ob.get(anims.PROP_REST) is not None and anims.rest_fingerprint(ob) != ob[anims.PROP_REST]:
            changes.append(Change(ob[anims.PROP_CLIENT], ob[anims.PROP_SKELETON], 'BON', ob))
    for (root, path), actions in anims.skeleton_actions(bpy.data.actions).items():
        edited = [a for a in actions if anims.action_fingerprint(a) != a[anims.PROP_KEYS]]
        if edited:
            changes.append(Change(root, path, 'ANIM', edited[0], objects=edited))
    # Spawn files of the server: rebuilt from the objects of their collection.
    for collection in spawns.spawn_collections():
        try:
            built = spawns.build_npcgen(collection)
        except (OSError, ValueError):
            continue
        if hashlib.sha1(built).hexdigest() != collection.get(spawns.PROP_HASH):
            changes.append(Change(collection[spawns.PROP_SERVER], collection[spawns.PROP_FILE], 'NPCGEN', collection))
    # Map scenes of the client: the places of moved effects, models and sounds.
    for collection in scene.scene_collections():
        if scene.scene_changed(collection):
            changes.append(Change(collection[scene.PROP_CLIENT], collection[scene.PROP_FILE], 'SCENE', collection))
    # Edited records of elements.data (the client's, and its server's).
    from . import records, tasks
    changes += records.changes(bpy.data.scenes)
    changes += tasks.changes(bpy.data.scenes)
    return changes


def _refresh_fingerprints(root, paths):
    """After an undo: compare the data-blocks of these files with the client files again."""
    import bpy
    import _angelica
    wanted = {path.lower() for path in paths}
    for collection in (bpy.data.texts, bpy.data.images):
        for id_data in collection:
            source = _source(id_data)
            if source is None or source[0] != root or source[1].lower() not in wanted:
                continue
            data = _angelica.read(root, source[1])
            if data is None:
                id_data[sources.PROP_FINGERPRINT] = ""
            elif collection is bpy.data.texts:
                id_data[sources.PROP_FINGERPRINT] = sources.text_fingerprint(sources.decode_text(data)[0])
            else:
                id_data[sources.PROP_FINGERPRINT] = hashlib.sha1(data).hexdigest()
    for (ob_root, path), objects in skins.skin_objects(bpy.data.objects).items():
        if ob_root == root and path.lower() in wanted:
            for ob in objects:
                # The client has the file from before the save: these meshes differ from it again.
                ob[skins.PROP_GEOMETRY] = ""
    for (action_root, path), actions in anims.skeleton_actions(bpy.data.actions).items():
        if action_root == root and path.lower() in wanted:
            for action in actions:
                action[anims.PROP_KEYS] = ""
    for ob in bpy.data.objects:
        if ob.get(anims.PROP_CLIENT) == root and str(ob.get(anims.PROP_SKELETON, "")).lower() in wanted:
            ob[anims.PROP_REST] = ""
    for collection in spawns.spawn_collections():
        if collection[spawns.PROP_SERVER] == root and collection[spawns.PROP_FILE].lower() in wanted:
            path = os.path.join(root, collection[spawns.PROP_FILE])
            if os.path.isfile(path):
                with open(path, "rb") as fh:
                    collection[spawns.PROP_HASH] = hashlib.sha1(fh.read()).hexdigest()
    for collection in scene.scene_collections():
        if collection[scene.PROP_CLIENT] == root and collection[scene.PROP_FILE].lower() in wanted:
            # The file has the places from before the save: the objects differ from it again.
            for ob in collection.all_objects:
                if scene.PROP_PLACED in ob:
                    del ob[scene.PROP_PLACED]


# -----------------------------------------------------------------------------
# Where files go


def target(root, path):
    """``('PACKAGE', pck, mount)`` or ``('FILE', file path, '')`` for a client file."""
    import _angelica
    location = _angelica.locate(root, path)
    if location is not None and location["kind"] == "PACKED":
        return 'PACKAGE', location["container"], location["mount"]
    if location is not None and location["kind"] == "LOOSE":
        return 'FILE', location["container"], ""
    # A new file goes into the package of its folder, where the client looks first.
    parts = path.split("\\")
    for depth in (2, 1):
        if len(parts) > depth:
            pck = os.path.join(root, *parts[:depth - 1], parts[depth - 1] + ".pck")
            if os.path.isfile(pck):
                return 'PACKAGE', pck, "\\".join(parts[:depth])
    return 'FILE', os.path.join(root, *parts), ""


def _config_dir():
    import bpy
    return bpy.utils.user_resource('CONFIG')


def _load_history():
    try:
        with open(os.path.join(_config_dir(), _HISTORY_FILE), "r", encoding="utf-8") as fh:
            history = json.load(fh)
        return history if isinstance(history, list) else []
    except (OSError, ValueError):
        return []


def _save_history(history):
    path = os.path.join(_config_dir(), _HISTORY_FILE)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path + ".tmp", "w", encoding="utf-8") as fh:
        json.dump(history, fh, ensure_ascii=False, indent=1)
    os.replace(path + ".tmp", path)


def _write_file(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".gameforge-tmp"
    with open(tmp, "wb") as fh:
        fh.write(data)
    try:
        os.replace(tmp, path)
    except OSError:
        os.remove(tmp)
        raise


def _undo_entry(entry):
    """Undo one in-place save. Returns the list of problems (empty when all was restored)."""
    import _angelica
    problems = []
    for pck in reversed(entry.get("packages", [])):
        try:
            _angelica.pck_undo(pck)
        except OSError as ex:
            problems.append(str(ex))
    for item in reversed(entry.get("files", [])):
        try:
            if item.get("backup"):
                shutil.copy2(item["backup"], item["path"])
            elif os.path.exists(item["path"]):
                os.remove(item["path"])
        except OSError as ex:
            problems.append("{:s}: {:s}".format(item["path"], str(ex)))
    _angelica.release()
    return problems


def _save_in_place(groups, paths_by_root):
    import _angelica
    entry = {"time": time.time(), "roots": paths_by_root, "packages": [], "files": []}
    backup_dir = os.path.join(_config_dir(), _BACKUP_DIR, time.strftime("%Y%m%d-%H%M%S"))
    try:
        for (_root, kind, container, mount), files in groups.items():
            if kind == 'PACKAGE':
                _angelica.pck_update(container, mount, [(c.path, data) for c, data in files])
                entry["packages"].append(container)
                continue
            for _change, data in files:
                backup = None
                if os.path.exists(container):
                    backup = os.path.join(backup_dir, str(len(entry["files"])), os.path.basename(container))
                    os.makedirs(os.path.dirname(backup), exist_ok=True)
                    shutil.copy2(container, backup)
                _write_file(container, data)
                entry["files"].append({"path": container, "backup": backup})
    except OSError as ex:
        problems = _undo_entry(entry)
        message = "Nada foi salvo: " + str(ex)
        if problems:
            message += "\nAo desfazer o que já tinha sido gravado: " + "; ".join(problems)
        raise SaveError(message) from ex
    history = _load_history()
    history.append(entry)
    _save_history(history[-50:])
    return entry


def _save_to_folder(groups, mode, out_dir):
    import _angelica
    written = []
    for (root, kind, container, mount), files in groups.items():
        relative = os.path.relpath(container, root)
        if relative.startswith(".."):
            relative = os.path.basename(container)
        dst = os.path.join(out_dir, files[0][0].folder, relative)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        if kind == 'PACKAGE':
            changes = [(c.path, data) for c, data in files]
            if mode == 'PATCH':
                _angelica.pck_write_patch(container, mount, dst, changes)
            else:
                _angelica.pck_write_copy(container, mount, dst, changes)
        else:
            _write_file(dst, files[0][1])
        written.append(dst)
    return written


def save(changes, mode, out_dir=""):
    """
    Write ``changes`` into their client (``IN_PLACE``) or into ``out_dir`` (``PATCH``, ``COPY``).
    One save is undone as a whole, also when it touches several folders (a client and its server).
    Returns the list of files written; raises :class:`SaveError`.
    """
    import _angelica
    if mode not in MODES:
        raise ValueError(mode)
    if mode != 'IN_PLACE' and not out_dir:
        raise SaveError("Escolha a pasta onde salvar")

    produced = []
    problems = []
    for change in changes:
        try:
            produced.append((change, change.data()))
        except (SaveError, ValueError, UnicodeError, OSError) as ex:
            problems.append("{:s}: {:s}".format(change.path, str(ex)))
    if problems:
        raise SaveError("Nada foi salvo, estes arquivos não puderam ser convertidos:\n" + "\n".join(problems))

    groups = {}
    paths_by_root = {}
    for change, data in produced:
        groups.setdefault((change.root,) + target(change.root, change.path), []).append((change, data))
        paths_by_root.setdefault(change.root, []).append(change.path)
    try:
        if mode == 'IN_PLACE':
            entry = _save_in_place(groups, paths_by_root)
            written = entry["packages"] + [f["path"] for f in entry["files"]]
        else:
            written = _save_to_folder(groups, mode, out_dir)
    except OSError as ex:
        raise SaveError(str(ex)) from ex
    finally:
        _angelica.release()
    if mode == 'IN_PLACE':
        for change, data in produced:
            change.mark_saved(data)
    return written


def can_undo():
    return bool(_load_history())


def undo_last_save():
    """Undo the last in-place save. Returns a list of problems (empty when all was restored)."""
    history = _load_history()
    if not history:
        raise SaveError("Não há salvamento no cliente para desfazer")
    entry = history.pop()
    problems = _undo_entry(entry)
    _save_history(history)
    roots = entry.get("roots") or ({entry["root"]: entry.get("paths", [])} if entry.get("root") else {})
    for root, paths in roots.items():
        _refresh_fingerprints(root, paths)
    return problems
