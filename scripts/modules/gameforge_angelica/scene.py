# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
The client scene of a map (``maps\\<code>\\<code>.ecwld`` and ``.ecbsd``, see ``ecwld``) in the
Studio: buildings where the client draws them, scene models, effects, sounds, trees, environment
areas, critters and routes, each kind in its own collection under the map's.

Effects, scene models and sounds can be moved, turned, duplicated (a copy becomes a new object
with a new id) and deleted: they live only in the world file, and saving ("Salvar no Cliente do
Jogo") writes them into it, the rest of the file staying byte for byte the same. Only objects that
were opened can be deleted (a map opened around a place keeps everything else). The other objects are locked. A building is placed by its own
``.bmd`` and its collision is baked there and in the server files, so moving one is not a scene
edit.

Engine space is Y up; the Studio is Z up: Blender ``(x, y, z)`` is engine ``(x, z, y)`` times the
import scale, as for the terrain, models and spawns.
"""

__all__ = (
    "build_world",
    "import_model_once",
    "load_map_scene",
    "mark_placed",
    "model_library",
    "scene_changed",
    "scene_collections",
)

import os
import struct

from . import ecwld

PROP_FILE = "gameforge_scene_file"
PROP_CLIENT = "gameforge_client"
PROP_SCALE = "gameforge_scale"
# On objects: what they are in the scene files, and their place when loaded or last saved.
PROP_KIND = "gameforge_scene_kind"
PROP_ID = "gameforge_scene_id"
PROP_PATH = "gameforge_scene_path"
PROP_PLACED = "gameforge_scene_placed"
# On the collection: the editable objects read from the file ("KIND:id"), and the ids the last
# save gave to copies made in the Studio (object name -> id).
PROP_LOADED = "gameforge_scene_loaded"
PROP_NEW_IDS = "gameforge_scene_new_ids"
# On the collection: editable objects deleted in the Studio (deleted again after an undone save).
PROP_GONE = "gameforge_scene_gone"
# On editable objects: the id of the object they were read as (or copied from), to copy again.
PROP_SOURCE = "gameforge_scene_source"
EDITABLE = ('EFFECT', 'MODEL', 'SOUND')
LABELS = {
    'BUILDING': "Prédios",
    'MODEL': "Modelos",
    'EFFECT': "Efeitos",
    'SOUND': "Sons",
    'TREE': "Árvores",
    'BOX_AREA': "Áreas de ambiente",
    'CRITTER': "Criaturas",
    'BEZIER': "Rotas",
    'WATER': "Água",
}
LIBRARY = "Modelos de cena"
_BRUSH_BUILDING = 0x80000001


def _swap(v):
    return (v[0], v[2], v[1])


def _matrix(pos, dir, up, scale, sizes=(1.0, 1.0, 1.0)):
    """Blender matrix of the engine's ``TransformMatrix(dir, up, pos)`` (with ``Scaling(sizes)``)."""
    from mathutils import Matrix, Vector
    z = Vector(dir).normalized()
    y = Vector(up).normalized()
    x = y.cross(z).normalized()
    if x.length == 0.0:
        x, y, z = Vector((1.0, 0.0, 0.0)), Vector((0.0, 1.0, 0.0)), Vector((0.0, 0.0, 1.0))
    # Engine axes x, y, z become the object's X, Z and Y axes, with y and z swapped in each.
    columns = (Vector(_swap(x)) * sizes[0], Vector(_swap(z)) * sizes[2], Vector(_swap(y)) * sizes[1])
    m = Matrix((columns[0], columns[1], columns[2])).transposed().to_4x4()
    m.translation = Vector(_swap(pos)) * scale
    return m


def _placement(ob, scale):
    """Engine ``(pos, dir, up)`` of an object."""
    m = ob.matrix_basis
    pos = tuple(v / scale for v in _swap(m.translation))
    return pos, _swap(m.col[1].xyz.normalized()), _swap(m.col[2].xyz.normalized())


def _flat(m):
    return [v for row in m for v in row]


def _moved(ob):
    placed = ob.get(PROP_PLACED)
    if placed is None or len(placed) != 16:
        return True
    return any(abs(a - b) > 1e-5 for a, b in zip(_flat(ob.matrix_basis), placed))


def _editable(collection):
    return [ob for ob in collection.all_objects if ob.get(PROP_KIND) in EDITABLE]


def _key(ob):
    return "{:s}:{:d}".format(ob[PROP_KIND], ob.get(PROP_ID, 0))


def mark_placed(collection):
    """The objects match the file (after loading or saving): their places, ids and the loaded set."""
    new_ids = collection.get(PROP_NEW_IDS)
    if new_ids:
        for ob in _editable(collection):
            if ob.name in new_ids:
                ob[PROP_ID] = int(new_ids[ob.name])
        del collection[PROP_NEW_IDS]
    for ob in _editable(collection):
        ob[PROP_PLACED] = _flat(ob.matrix_basis)
        if PROP_SOURCE not in ob:
            ob[PROP_SOURCE] = ob.get(PROP_ID, 0)
    present = {_key(ob) for ob in _editable(collection)}
    gone = set(collection.get(PROP_GONE, ())) | (set(collection.get(PROP_LOADED, ())) - present)
    collection[PROP_GONE] = sorted(gone)
    collection[PROP_LOADED] = sorted(present)


def scene_collections():
    """Collections made from map scenes."""
    import bpy
    return [c for c in bpy.data.collections if PROP_FILE in c and PROP_CLIENT in c]


def scene_changed(collection):
    """Objects moved, copied or deleted since the file was read or saved."""
    obs = _editable(collection)
    if any(_moved(ob) for ob in obs):
        return True
    keys = [_key(ob) for ob in obs]
    return len(set(keys)) != len(keys) or set(keys) != set(collection.get(PROP_LOADED, ()))


def _read_scene(client_root, path):
    import _angelica
    world = _angelica.read(client_root, path)
    bsd = _angelica.read(client_root, path[:-len(".ecwld")] + ".ecbsd")
    if world is None or bsd is None:
        raise FileNotFoundError("o cliente não tem " + path + " e o .ecbsd do mesmo mapa")
    return ecwld.SceneFile(world, bsd)


def build_world(collection):
    """
    The bytes of the collection's world file with the moved, copied and deleted objects. A copy
    made in the Studio carries the id of the object it was copied from: the first of them by name
    keeps the id, the others become new objects (their new ids are applied after the save).
    """
    scene = _read_scene(collection[PROP_CLIENT], collection[PROP_FILE])
    scale = float(collection.get(PROP_SCALE, 1.0)) or 1.0
    by_key = {"{:s}:{:d}".format(o.kind, o.export_id): o for o in scene.objects if o.kind in EDITABLE}
    obs = sorted(_editable(collection), key=lambda ob: ob.name)
    seen = set()
    new_ids = {}
    for ob in obs:
        key = _key(ob)
        source = by_key.get(key)
        pos, dir, up = _placement(ob, scale)
        if ob[PROP_KIND] == 'SOUND':
            dir = up = None
        if source is None:
            # A copy whose id is not in the file (a save that was undone): copied again.
            template = by_key.get("{:s}:{:d}".format(ob[PROP_KIND], ob.get(PROP_SOURCE, 0)))
            if template is not None:
                new_ids[ob.name] = scene.add_copy(template, pos, dir, up).export_id
            continue
        if key not in seen:
            seen.add(key)
            if _moved(ob):
                scene.move(source, pos=pos, dir=dir, up=up)
        else:
            new_ids[ob.name] = scene.add_copy(source, pos, dir, up).export_id
    # Objects read from the file and deleted in the Studio (also those an undone save brought back).
    held = set(collection.get(PROP_LOADED, ())) | set(collection.get(PROP_GONE, ()))
    for key in held - {_key(ob) for ob in obs}:
        if key in by_key:
            scene.remove(by_key[key])
    collection[PROP_NEW_IDS] = new_ids
    return scene.world_bytes()


def import_model_once(library, client_root, path):
    """
    The collection of a client model in ``library`` (imported the first time it is asked for), or
    None when the model cannot be opened.
    """
    import bpy
    existing = next((c for c in library.children if c.get("gameforge_model") == path), None)
    if existing is not None:
        return existing
    before = set(bpy.data.collections)
    try:
        result = bpy.ops.wm.angelica_import_asset(client_root=client_root, engine_path=path, import_animations=False)
    except RuntimeError:
        return None
    new = [c for c in bpy.data.collections if c not in before and "gameforge_source" in c]
    if 'FINISHED' not in result or not new:
        return None
    model = new[0]
    for parent in [c for c in bpy.data.collections if model.name in c.children] + [bpy.context.scene.collection]:
        if model.name in parent.children:
            parent.children.unlink(model)
    library.children.link(model)
    model["gameforge_model"] = path
    return model


def model_library(name):
    """A collection holding imported models, excluded from the view layer (seen through instances)."""
    import bpy
    library = bpy.data.collections.get(name) or bpy.data.collections.new(name)
    if library.name not in bpy.context.scene.collection.children:
        bpy.context.scene.collection.children.link(library)
    layer = bpy.context.view_layer.layer_collection.children.get(library.name)
    if layer is not None:
        layer.exclude = True
    return library


def _bmd_placement(data):
    """``(sizes, dir, up, pos)`` stored at the start of a ``.bmd`` building (its lit model header)."""
    offset = 4 if bytes(data[:4]) in (b"MOXB", b"MOXT") else 0
    (version,) = struct.unpack_from("<I", data, offset)
    offset += 4
    if version == _BRUSH_BUILDING:
        offset += 1
        offset += 4
    values = struct.unpack_from("<12f", data, offset)
    return values[0:3], values[3:6], values[6:9], values[9:12]


class _MapFiles(dict):
    """Files of a map folder of the client, read when first asked for (None when missing)."""

    def __init__(self, client_root, map_code):
        super().__init__()
        self.client_root = client_root
        self.map_code = map_code

    def __missing__(self, name):
        import _angelica
        data = _angelica.read(self.client_root, "maps\\{:s}\\{:s}".format(self.map_code, name))
        self[name] = data
        return data

    def get(self, name, default=None):
        data = self[name]
        return default if data is None else data


def _srgb_to_linear(c):
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def _water_material(color):
    """A see-through material in the water's color (one per color)."""
    import bpy
    name = "Água {:08X}".format(color)
    material = bpy.data.materials.get(name)
    if material is not None:
        return material
    material = bpy.data.materials.new(name)
    rgb = [_srgb_to_linear(((color >> shift) & 255) / 255.0) for shift in (16, 8, 0)]
    material.diffuse_color = rgb + [0.7]
    if material.node_tree is None:
        material.use_nodes = True
    bsdf = next((n for n in material.node_tree.nodes if n.type == 'BSDF_PRINCIPLED'), None)
    if bsdf is not None:
        bsdf.inputs["Base Color"].default_value = rgb + [1.0]
        bsdf.inputs["Alpha"].default_value = 0.7
        bsdf.inputs["Roughness"].default_value = 0.05
    if hasattr(material, "surface_render_method"):
        material.surface_render_method = 'BLENDED'
    return material


def _water_mesh(name, area, scale):
    """The cells with water of an area as quads at the water height (Blender ``(x, y)`` = engine
    ``(x, z)``, row 0 north)."""
    import bpy
    cell, columns, rows = area["cell"], area["columns"], area["rows"]
    west = area["center"][0] - cell * columns / 2.0
    north = area["center"][2] + cell * rows / 2.0
    height = area["height"] * scale
    verts, faces = [], []
    for row in range(rows):
        for column in range(columns):
            if not area["cells"][row * columns + column] & 1:
                continue
            x, y = (west + column * cell) * scale, (north - row * cell) * scale
            k = len(verts)
            size = cell * scale
            # North-west, south-west, south-east, north-east: counter-clockwise seen from above.
            verts += [(x, y, height), (x, y - size, height), (x + size, y - size, height), (x + size, y, height)]
            faces.append((k, k + 1, k + 2, k + 3))
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata(verts, [], faces)
    mesh.materials.append(_water_material(area["color"]))
    return mesh


def _name(path):
    return os.path.splitext(path.replace("/", "\\").rsplit("\\", 1)[-1])[0] or "?"


def _lock(ob):
    ob.lock_location = (True, True, True)
    ob.lock_rotation = (True, True, True)
    ob.lock_scale = (True, True, True)


def load_map_scene(client_root, map_code, scale=1.0, center=None, radius=0.0, building_limit=400):
    """
    Read the scene of ``map_code`` into a new collection. ``center`` (engine ``x``, ``z``) and
    ``radius`` keep only the objects around a place (0: the whole map). At most
    ``building_limit`` buildings are opened. Returns ``(collection, problems)``.
    """
    import bpy
    import _angelica
    path = "maps\\{0:s}\\{0:s}.ecwld".format(map_code)
    scene = _read_scene(client_root, path)

    def inside(o):
        if not radius or center is None or o.kind == 'WATER':
            return True
        return abs(o.pos[0] - center[0]) <= radius and abs(o.pos[2] - center[1]) <= radius

    objects = [o for o in scene.objects if o.kind in LABELS and inside(o)]
    map_files = _MapFiles(client_root, map_code)
    collection = bpy.data.collections.new("Cena " + map_code)
    collection[PROP_FILE] = path
    collection[PROP_CLIENT] = client_root
    collection[PROP_SCALE] = float(scale)
    bpy.context.scene.collection.children.link(collection)
    parts = {}

    def part(kind):
        if kind not in parts:
            parts[kind] = bpy.data.collections.new("{:s} ({:s})".format(LABELS[kind], map_code))
            collection.children.link(parts[kind])
        return parts[kind]

    def new_object(o, data=None, name=""):
        ob = bpy.data.objects.new(name or _name(o.path) or LABELS[o.kind], data)
        part(o.kind).objects.link(ob)
        ob[PROP_KIND] = o.kind
        ob[PROP_ID] = o.export_id
        ob[PROP_PATH] = o.path
        return ob

    problems = []
    library = model_library(LIBRARY)
    for o in objects:
        if o.kind == 'EFFECT':
            ob = new_object(o)
            ob.empty_display_type = 'SPHERE'
            ob.empty_display_size = 0.5
            ob.matrix_basis = _matrix(o.pos, o.dir, o.up, scale, (o.extra["scale"],) * 3)
            for key in ("speed", "alpha", "valid_time"):
                ob["gameforge_effect_" + key] = o.extra[key]
        elif o.kind == 'SOUND':
            speaker = bpy.data.speakers.new(_name(o.path))
            speaker.distance_reference = o.extra["min_dist"] * scale
            speaker.distance_max = o.extra["max_dist"] * scale
            ob = new_object(o, speaker)
            ob.matrix_basis = _matrix(o.pos, (0.0, 0.0, 1.0), (0.0, 1.0, 0.0), scale)
        elif o.kind == 'MODEL':
            ob = new_object(o)
            ob.matrix_basis = _matrix(o.pos, o.dir, o.up, scale)
            ob["gameforge_model_action"] = o.extra["action"]
            model = import_model_once(library, client_root, o.path)
            if model is None:
                problems.append(o.path)
                ob.empty_display_type = 'ARROWS'
            else:
                ob.instance_type = 'COLLECTION'
                ob.instance_collection = model
        elif o.kind == 'TREE':
            ob = new_object(o)
            ob.empty_display_type = 'CONE'
            ob.empty_display_size = 2.0 * scale
            ob.matrix_basis = _matrix(o.pos, (0.0, 1.0, 0.0), (0.0, 0.0, -1.0), scale)
            _lock(ob)
        elif o.kind == 'BOX_AREA':
            ob = new_object(o, name="Ambiente {:d}".format(o.export_id))
            ob.empty_display_type = 'CUBE'
            # The box's extents are half sizes along its own axes.
            ob.matrix_basis = _matrix(o.pos, o.dir, o.up, scale, o.extra["extents"])
            ob.hide_viewport = o.extra["default"]
            ob["gameforge_area_strings"] = o.extra["strings"]
            _lock(ob)
        elif o.kind == 'CRITTER':
            ob = new_object(o)
            ob.empty_display_type = 'SPHERE'
            ob.empty_display_size = max(o.extra["radius"], 0.5) * scale
            ob.matrix_basis = _matrix(o.pos, (0.0, 0.0, 1.0), (0.0, 1.0, 0.0), scale)
            ob["gameforge_critter_count"] = o.extra["count"]
            _lock(ob)
        elif o.kind == 'WATER':
            area = scene.water_area(o, map_files)
            if area is None or not any(c & 1 for c in area["cells"]):
                continue
            if radius and center is not None:
                half = area["cell"] * max(area["columns"], area["rows"]) / 2.0
                if abs(area["center"][0] - center[0]) > radius + half or \
                        abs(area["center"][2] - center[1]) > radius + half:
                    continue
            ob = new_object(o, _water_mesh("Água {:d}".format(o.export_id), area, scale),
                            name="Água {:d}".format(o.export_id))
            _lock(ob)
        elif o.kind == 'BEZIER':
            points = o.extra["points"]
            if len(points) < 2:
                continue
            curve = bpy.data.curves.new("Rota {:d}".format(o.export_id), 'CURVE')
            curve.dimensions = '3D'
            spline = curve.splines.new('POLY')
            spline.points.add(len(points) - 1)
            for point, p in zip(spline.points, points):
                point.co = tuple(v * scale for v in _swap(p)) + (1.0,)
            ob = new_object(o, curve, name=curve.name)
            _lock(ob)

    buildings = [o for o in objects if o.kind == 'BUILDING']
    if len(buildings) > building_limit:
        problems.append("{:d} prédios fora do limite de {:d}".format(len(buildings) - building_limit, building_limit))
    for o in buildings[:building_limit]:
        full = "litmodels\\{:s}\\{:s}".format(map_code, o.path)
        data = _angelica.read(client_root, full)
        model = import_model_once(library, client_root, full) if data is not None else None
        if model is None:
            problems.append(full)
            continue
        ob = new_object(o, name=_name(o.path))
        ob.instance_type = 'COLLECTION'
        ob.instance_collection = model
        sizes, dir, up, pos = _bmd_placement(data)
        ob.matrix_basis = _matrix(pos, dir, up, scale, sizes)
        _lock(ob)
    mark_placed(collection)
    return collection, problems
