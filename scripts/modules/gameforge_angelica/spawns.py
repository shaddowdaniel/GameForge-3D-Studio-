# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
NPC and monster spawns of a map (the server's ``npcgen.data``) as objects in the Studio.

Every spawn area becomes an empty in a collection of the map: its place and turn are the area's
position and direction, its scale the area's half extents (box areas), and its ``gameforge_spawn``
properties hold the rest (monster or service NPC area, the templates spawned there, ...). Resource
areas (mines, herbs) and dynamic objects (gates, decorations) become empties too. Moving, adding,
duplicating or deleting these objects edits the spawns; saving rebuilds the file and writes it
into the server folder. Areas whose object did not change keep their values bit for bit, and the
parts of the file that are not objects (controllers) are kept as they are.

Engine space is Y up; the Studio is Z up: Blender ``(x, y, z)`` is engine ``(x, z, y)`` times the
import scale, as for the terrain and models.
"""

__all__ = (
    "build_npcgen",
    "load_map_spawns",
    "show_models",
    "spawn_collections",
    "template_name",
    "templates",
)

import hashlib
import math
import os

from . import npcgen

PROP_FILE = "gameforge_npcgen_file"
PROP_SERVER = "gameforge_npcgen_server"
PROP_CLIENT = "gameforge_client"
PROP_SCALE = "gameforge_scale"
PROP_HASH = "gameforge_npcgen_hash"
KINDS = ('AREA', 'RESOURCE', 'DYNAMIC')
TEMPLATE_LISTS = ("MONSTER_ESSENCE", "NPC_ESSENCE", "MINE_ESSENCE")
_SPAWN_FIELDS = ("count", "refresh", "died_times", "aggressive", "offset_water", "offset_terrain", "faction",
                 "faction_helper", "faction_accept", "need_help", "default_faction", "default_faction_helper",
                 "default_faction_accept", "path_id", "loop_type", "speed_flag", "dead_time", "refresh_lower")
_AREA_FIELDS = ("npc_type", "group_type", "init_gen", "auto_revive", "valid_once", "gen_id", "controller_id",
                "life_time", "max_num")


def to_blender(p, scale):
    return (p[0] * scale, p[2] * scale, p[1] * scale)


def to_engine(p, scale):
    return (p[0] / scale, p[2] / scale, p[1] / scale)


def _yaw(direction):
    """Turn about the up axis of an engine direction (``x``, ``z`` on the ground plane)."""
    return math.atan2(-direction[0], direction[2]) if (direction[0] or direction[2]) else 0.0


def _direction(yaw):
    return (-math.sin(yaw), 0.0, math.cos(yaw))


def _packed_yaw(d):
    """Resource areas and dynamic objects pack their turn into ``rad`` (0..255 is a full turn)."""
    return d * 2.0 * math.pi / 256.0


# client root -> (elements.data time, {template id: (name, list name)}).
_templates_cache = {}


def templates(client_root):
    """
    ``{template id: (name, list name)}`` of the client's monsters, NPCs and mines (empty when
    unknown), read once per version of its ``elements.data``.
    """
    try:
        stamp = os.path.getmtime(os.path.join(client_root, "data", "elements.data"))
    except OSError:
        stamp = 0.0
    cached = _templates_cache.get(client_root)
    if cached is not None and cached[0] == stamp:
        return cached[1]
    from . import data
    import _angelica
    found = {}
    try:
        db = data.ClientData(client_root)
    except (OSError, ValueError):
        db = None
    if db is not None:
        try:
            for list_name in TEMPLATE_LISTS:
                index = db.index.get(list_name)
                name_field = db.field_index[index].get("Name") if index is not None else None
                if name_field is None:
                    continue
                for record, record_id in enumerate(_angelica.elements_ids(db.handle, index)):
                    found.setdefault(record_id, (str(db.record(index, record)[name_field]), list_name))
        finally:
            db.close()
    _templates_cache[client_root] = (stamp, found)
    return found


def template_name(client_root, template_id):
    """Name of an NPC, monster or mine of the client, empty when unknown."""
    found = templates(client_root).get(template_id) if client_root else None
    return found[0] if found else ""


def _empty(collection, name, kind, display):
    import bpy
    ob = bpy.data.objects.new(name, None)
    ob.empty_display_type = display
    collection.objects.link(ob)
    ob.gameforge_spawn.kind = kind
    return ob


def load_map_spawns(server_folder, map_code, client_root="", scale=1.0, collection_name=""):
    """
    Read ``<server_folder>\\<map_code>\\npcgen.data`` into a new collection of empties. Returns the
    collection.
    """
    import bpy
    path = os.path.join(server_folder, map_code, "npcgen.data")
    with open(path, "rb") as fh:
        raw = fh.read()
    gen = npcgen.NpcGen(raw)
    known = templates(client_root) if client_root else {}

    def name_of(template_id):
        return known[template_id][0] if template_id in known else str(template_id)

    collection = bpy.data.collections.new(collection_name or "Spawns {:s}".format(map_code))
    collection[PROP_FILE] = map_code + "\\npcgen.data"
    collection[PROP_SERVER] = server_folder
    collection[PROP_CLIENT] = client_root
    collection[PROP_SCALE] = float(scale)
    collection[PROP_HASH] = hashlib.sha1(raw).hexdigest()

    for index, area in enumerate(gen.areas):
        first = area.spawns[0].id if area.spawns else 0
        label = name_of(first)
        if len(area.spawns) > 1:
            label += " +{:d}".format(len(area.spawns) - 1)
        ob = _empty(collection, label, 'AREA', 'CUBE' if area.type == 1 else 'CIRCLE')
        ob.location = to_blender(area.pos, scale)
        ob.rotation_euler = (0.0, 0.0, _yaw(area.dir))
        ob.scale = _area_scale(area.extents, scale)
        props = ob.gameforge_spawn
        props.index = index
        props.area_type = area.type
        for name in _AREA_FIELDS:
            setattr(props, name, getattr(area, name))
        for spawn in area.spawns:
            entry = props.spawns.add()
            # Setting the template also sets the entry's name (from the collection's client).
            entry.template_id = spawn.id
            for name in _SPAWN_FIELDS:
                setattr(entry, name, getattr(spawn, name))
    for index, area in enumerate(gen.resource_areas):
        first = area.resources[0].template_id if area.resources else 0
        ob = _empty(collection, "Recurso " + name_of(first), 'RESOURCE', 'PLAIN_AXES')
        ob.location = to_blender(area.pos, scale)
        ob.rotation_euler = (0.0, 0.0, _packed_yaw(area.rad))
        ob.scale = (max(area.extent_x, 1.0) * 0.5 * scale, max(area.extent_z, 1.0) * 0.5 * scale, 1.0)
        props = ob.gameforge_spawn
        props.index = index
    for index, obj in enumerate(gen.dynamic_objects):
        ob = _empty(collection, "Objeto dinâmico {:d}".format(obj.id), 'DYNAMIC', 'SINGLE_ARROW')
        ob.location = to_blender(obj.pos, scale)
        ob.rotation_euler = (0.0, 0.0, _packed_yaw(obj.rad))
        props = ob.gameforge_spawn
        props.index = index
    bpy.context.scene.collection.children.link(collection)
    return collection


def _template_models(client_root, template_ids):
    """``{template id: model path}`` for NPC and monster templates (an NPC without a model of its
    own shows its source monster's)."""
    from . import data
    import _angelica
    models = {}
    try:
        db = data.ClientData(client_root)
    except (OSError, ValueError):
        return models
    try:
        def model_of(template_id, depth=0):
            found = db.find_in_space("ESSENCE", template_id)
            if found is None:
                return ""
            list_index, record = found
            fields = db.field_index[list_index]
            values = db.record(list_index, record)
            path = values[fields["file_model"]] if "file_model" in fields else ""
            if (not path or path == "0") and "id_src_monster" in fields and depth < 2:
                return model_of(values[fields["id_src_monster"]], depth + 1)
            return path if isinstance(path, str) and path not in ("", "0") else ""

        for template_id in template_ids:
            path = model_of(template_id)
            if path:
                models[template_id] = path.replace("/", "\\")
    finally:
        db.close()
    return models


def show_models(collection, client_root, limit=200):
    """
    Put the model of the first NPC or monster of every area on it: each model is imported once into
    a hidden collection and shown by an instance that follows the area (place and turn, not size).
    Returns ``(models shown, models that could not be opened)``.
    """
    import bpy
    from . import scene
    areas = [ob for ob in collection.objects if ob.gameforge_spawn.kind == 'AREA' and ob.gameforge_spawn.spawns]
    models = _template_models(client_root, {ob.gameforge_spawn.spawns[0].template_id for ob in areas})
    library = scene.model_library("Modelos de spawn")
    instances = bpy.data.collections.get(collection.name + " (modelos)")
    if instances is None:
        instances = bpy.data.collections.new(collection.name + " (modelos)")
        collection.children.link(instances)
    loaded, failed = {}, []
    for path in sorted(set(models.values()))[:limit]:
        model = scene.import_model_once(library, client_root, path)
        if model is None:
            failed.append(path)
        else:
            loaded[path] = model
    for ob in areas:
        model = loaded.get(models.get(ob.gameforge_spawn.spawns[0].template_id))
        if model is None:
            continue
        inst = bpy.data.objects.new(ob.name + " (modelo)", None)
        inst.instance_type = 'COLLECTION'
        inst.instance_collection = model
        instances.objects.link(inst)
        for kind in ('COPY_LOCATION', 'COPY_ROTATION'):
            constraint = inst.constraints.new(kind)
            constraint.target = ob
    return len(loaded), failed


def spawn_collections():
    """Collections made from spawn files."""
    import bpy
    return [c for c in bpy.data.collections if PROP_FILE in c and PROP_SERVER in c]


def _angle_difference(a, b):
    return abs((a - b + math.pi) % (2.0 * math.pi) - math.pi)


def _area_scale(extents, scale):
    """
    Object scale showing an area: ``vExts`` is the area's full size (the server spawns within
    ``vPos -+ vExts / 2``) and the empty is drawn from -1 to 1. Point areas (no size) are drawn one
    meter wide so they can be seen and picked.
    """
    return tuple(max(e, 1.0) * 0.5 * scale for e in (extents[0], extents[2], extents[1]))


def _changed(a, b, tolerance=1e-4):
    return any(abs(x - y) > tolerance for x, y in zip(a, b))


def build_npcgen(collection):
    """
    The bytes of the collection's ``npcgen.data`` as edited. Values the user did not change keep
    the file's own numbers; objects that share an area (duplicates) become new areas.
    """
    path = os.path.join(collection[PROP_SERVER], collection[PROP_FILE])
    with open(path, "rb") as fh:
        gen = npcgen.NpcGen(fh.read())
    scale = float(collection.get(PROP_SCALE, 1.0)) or 1.0
    areas, resources, dynamics = [], [], []
    # Records of the file first, in file order; then new ones (added, or duplicates of a record).
    used = {kind: set() for kind in KINDS}
    existing, new = [], []
    for ob in sorted(collection.all_objects, key=lambda o: (o.gameforge_spawn.index < 0, o.gameforge_spawn.index, o.name)):
        props = ob.gameforge_spawn
        if props.index >= 0 and props.index not in used[props.kind]:
            used[props.kind].add(props.index)
            existing.append((ob, props.index))
        else:
            new.append((ob, -1))
    for ob, index in existing + new:
        props = ob.gameforge_spawn
        location = tuple(ob.location)
        yaw = ob.rotation_euler.z
        if props.kind == 'AREA':
            old = gen.areas[index] if 0 <= index < len(gen.areas) else None
            area = npcgen.Area()
            area.pos = old.pos if old and not _changed(location, to_blender(old.pos, scale)) else \
                to_engine(location, scale)
            area.dir = old.dir if old and _angle_difference(yaw, _yaw(old.dir)) <= 1e-4 else _direction(yaw)
            # Per axis (engine x, y, z are the object's x, z, y): the file's size unless resized.
            current = tuple(ob.scale)
            shown = _area_scale(old.extents, scale) if old else None
            area.extents = tuple(
                old.extents[e] if old and abs(current[b] - shown[b]) <= 1e-4 else current[b] * 2.0 / scale
                for e, b in ((0, 0), (1, 2), (2, 1)))
            area.type = props.area_type
            for name in _AREA_FIELDS:
                setattr(area, name, getattr(props, name))
            for entry in props.spawns:
                spawn = npcgen.Spawn(id=entry.template_id)
                for name in _SPAWN_FIELDS:
                    setattr(spawn, name, getattr(entry, name))
                area.spawns.append(spawn)
            areas.append(area)
        elif props.kind == 'RESOURCE':
            old = gen.resource_areas[index] if 0 <= index < len(gen.resource_areas) else None
            if old is None:
                continue  # Resource areas are only moved in this version.
            area = npcgen.ResourceArea(**{k: getattr(old, k) for k in old.__dataclass_fields__})
            if _changed(location, to_blender(old.pos, scale)):
                area.pos = to_engine(location, scale)
            resources.append(area)
        elif props.kind == 'DYNAMIC':
            old = gen.dynamic_objects[index] if 0 <= index < len(gen.dynamic_objects) else None
            if old is None:
                continue
            obj = npcgen.DynamicObject(**{k: getattr(old, k) for k in old.__dataclass_fields__})
            if _changed(location, to_blender(old.pos, scale)):
                obj.pos = to_engine(location, scale)
            dynamics.append(obj)
    gen.areas, gen.resource_areas, gen.dynamic_objects = areas, resources, dynamics
    return gen.to_bytes()
