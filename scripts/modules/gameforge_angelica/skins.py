# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Edited skin meshes written back into their ``.ski`` file.

The importer makes one object per mesh of the file (``gameforge_skin_file``,
``gameforge_skin_mesh``), one Blender vertex per file vertex, positions ``(x, z, y) * scale``,
texture V flipped and the file normals as custom normals. Saving reverses that and replaces only
the records of the edited meshes: the header, textures, materials and every other mesh stay byte
for byte as they were.

A mesh record of an ``A3DSkinMesh``: name (``i32`` length + GBK), texture and material index,
vertex and index count, 48-byte vertices (position, 3 weights, 4 bone index bytes, normal, UV)
and 16-bit triangle indices. ``A3DRigidMesh`` has the bone index after the name and 32-byte
vertices (position, normal, UV). Bone indices point into the skin's own bone name list (version 9)
or straight at the skeleton's bones (version 8).
"""

__all__ = (
    "export_skin",
    "mesh_fingerprint",
    "skin_objects",
)

import hashlib
import struct

import numpy as np

PROP_SKIN_FILE = "gameforge_skin_file"
PROP_SKIN_MESH = "gameforge_skin_mesh"
PROP_SCALE = "gameforge_scale"
PROP_GEOMETRY = "gameforge_geometry"

SKIN_VERTEX = np.dtype([("pos", "<f4", (3,)), ("weights", "<f4", (3,)), ("bones", "u1", (4,)),
                        ("normal", "<f4", (3,)), ("uv", "<f4", (2,))])
RIGID_VERTEX = np.dtype([("pos", "<f4", (3,)), ("normal", "<f4", (3,)), ("uv", "<f4", (2,))])
WRITABLE_TYPES = {"SKIN", "RIGID"}
MAX_INFLUENCES = 4


def skin_objects(objects):
    """``{(client, skin file): [objects]}`` for the objects made from client skin meshes."""
    result = {}
    for ob in objects:
        if ob.type != 'MESH' or PROP_SKIN_FILE not in ob or PROP_SKIN_MESH not in ob:
            continue
        root = ob.get("gameforge_client") or ob.data.get("gameforge_client")
        result.setdefault((root, ob[PROP_SKIN_FILE]), []).append(ob)
    return result


def _weights(ob, mesh):
    """``(vertex, group, weight)`` arrays of the mesh's vertex group weights."""
    vertices, groups, weights = [], [], []
    for v in mesh.vertices:
        for g in v.groups:
            if g.weight > 0.0:
                vertices.append(v.index)
                groups.append(g.group)
                weights.append(g.weight)
    return (np.array(vertices, dtype=np.int32), np.array(groups, dtype=np.int32),
            np.array(weights, dtype=np.float32))


def _corner_data(mesh):
    """Per corner of the triangulated mesh: vertex index, UV and normal (Blender space)."""
    mesh.calc_loop_triangles()
    tri_loops = np.empty(len(mesh.loop_triangles) * 3, dtype=np.int32)
    mesh.loop_triangles.foreach_get("loops", tri_loops)
    corner_verts = np.empty(len(mesh.loops), dtype=np.int32)
    mesh.loops.foreach_get("vertex_index", corner_verts)
    uv_layer = mesh.uv_layers.get("UVMap") or mesh.uv_layers.active
    uvs = np.zeros((len(mesh.loops), 2), dtype=np.float32)
    if uv_layer is not None:
        uv_layer.uv.foreach_get("vector", uvs.ravel())
    normals = np.empty((len(mesh.loops), 3), dtype=np.float32)
    mesh.corner_normals.foreach_get("vector", normals.ravel())
    return tri_loops, corner_verts, uvs, normals


def mesh_fingerprint(ob):
    """A hash of everything saving writes (geometry, UVs, normals, weights), to detect edits."""
    mesh = ob.data
    positions = np.empty(len(mesh.vertices) * 3, dtype=np.float32)
    mesh.vertices.foreach_get("co", positions)
    tri_loops, corner_verts, uvs, normals = _corner_data(mesh)
    vertices, groups, weights = _weights(ob, mesh)
    h = hashlib.sha1()
    for array in (positions, tri_loops, corner_verts, uvs, normals, vertices, groups, weights):
        h.update(np.ascontiguousarray(array).tobytes())
    h.update("\0".join(g.name for g in ob.vertex_groups).encode("utf-8"))
    return h.hexdigest()


def _split_vertices(mesh):
    """
    Engine vertices for a Blender mesh: one per distinct (vertex, UV, normal) of the corners, in
    vertex order, so an unchanged mesh keeps the file's vertex order. Returns the source Blender
    vertex, UV and normal of each engine vertex and the triangle indices.
    """
    tri_loops, corner_verts, uvs, normals = _corner_data(mesh)
    corners = np.unique(tri_loops)
    # Quantized keys: corners that only differ by float noise share a vertex.
    keys = np.empty(len(corners), dtype=[("v", "<i4"), ("u", "<i4"), ("w", "<i4"),
                                         ("nx", "<i4"), ("ny", "<i4"), ("nz", "<i4")])
    keys["v"] = corner_verts[corners]
    q_uv = np.rint(uvs[corners] * 1e5).astype(np.int32)
    q_n = np.rint(normals[corners] * 1e4).astype(np.int32)
    keys["u"], keys["w"] = q_uv[:, 0], q_uv[:, 1]
    keys["nx"], keys["ny"], keys["nz"] = q_n[:, 0], q_n[:, 1], q_n[:, 2]
    unique_keys, first, inverse = np.unique(keys, return_index=True, return_inverse=True)
    corner_to_vertex = np.full(len(mesh.loops), -1, dtype=np.int64)
    corner_to_vertex[corners] = inverse.ravel()
    source = corners[first]
    # Blender's front faces are counter-clockwise, the engine's clockwise once mirrored back by the
    # Y/Z swap: the last two corners of every triangle swap back (the importer swapped them).
    indices = corner_to_vertex[tri_loops].reshape(-1, 3)[:, [0, 2, 1]].ravel()
    return corner_verts[source], uvs[source], normals[source], indices


def _weighted_groups(ob):
    """Names of the vertex groups of ``ob`` that weigh any vertex."""
    vertices, groups, weights = _weights(ob, ob.data)
    names = [g.name for g in ob.vertex_groups]
    return [names[g] for g in sorted(set(groups.tolist())) if g < len(names)]


def _bone_slots(ob, mesh, vertex_source, bone_names):
    """
    4 bone indices (into ``bone_names``) and 3 weights per engine vertex. A vertex without weights
    follows the model itself: the index one past the skin's bones.
    """
    vertices, groups, weights = _weights(ob, mesh)
    group_names = [g.name for g in ob.vertex_groups]
    count = len(mesh.vertices)
    per_vertex = [[] for _ in range(count)]
    for v, g, w in zip(vertices.tolist(), groups.tolist(), weights.tolist()):
        if g < len(group_names):
            per_vertex[v].append((w, group_names[g]))
    slots = np.zeros((count, MAX_INFLUENCES), dtype=np.uint8)
    stored = np.zeros((count, 3), dtype=np.float32)
    unweighted = 0
    for v, influences in enumerate(per_vertex):
        influences.sort(reverse=True)
        influences = influences[:MAX_INFLUENCES]
        total = sum(w for w, _name in influences)
        if total <= 0.0:
            unweighted += 1
            slots[v, 0] = len(bone_names)
            stored[v, 0] = 1.0
            continue
        for k, (w, name) in enumerate(influences):
            slots[v, k] = bone_names.index(name)
            if k < 3:
                stored[v, k] = w / total
    return slots[vertex_source], stored[vertex_source], unweighted


def _encode_record(ob, original, record, bone_names):
    """The new bytes of one mesh record for the object ``ob``."""
    mesh = ob.data
    scale = float(ob.get(PROP_SCALE, 1.0)) or 1.0
    vertex_source, uvs, normals, indices = _split_vertices(mesh)
    if len(vertex_source) > 65535:
        raise ValueError("a malha '{:s}' tem {:d} vértices; o jogo aceita até 65535 por malha".format(
            ob.name, len(vertex_source)))
    positions = np.empty((len(mesh.vertices), 3), dtype=np.float32)
    mesh.vertices.foreach_get("co", positions.ravel())
    positions = positions[vertex_source] / scale
    # Blender (x, y, z) -> engine (x, z, y); the swap is its own inverse.
    positions = positions[:, [0, 2, 1]]
    normals = normals[:, [0, 2, 1]]
    lengths = np.linalg.norm(normals, axis=1, keepdims=True)
    normals = np.where(lengths > 1e-8, normals / np.maximum(lengths, 1e-8), np.float32([0, 1, 0]))
    uvs = np.column_stack((uvs[:, 0], 1.0 - uvs[:, 1]))

    begin, end = record["range"]
    data = original[begin:end]
    name_len = struct.unpack_from("<i", data, 0)[0]
    name = data[:4 + name_len]
    if record["type"] == "RIGID":
        head = name + data[4 + name_len:4 + name_len + 12]  # Bone, texture, material.
        vertices = np.zeros(len(vertex_source), dtype=RIGID_VERTEX)
    else:
        head = name + data[4 + name_len:4 + name_len + 8]  # Texture, material.
        vertices = np.zeros(len(vertex_source), dtype=SKIN_VERTEX)
        bones, weights, unweighted = _bone_slots(ob, mesh, vertex_source, bone_names)
        if unweighted:
            print("GameForge: {:d} vertices of '{:s}' had no bone weight".format(unweighted, ob.name))
        vertices["bones"] = bones
        vertices["weights"] = weights
    vertices["pos"] = positions
    vertices["normal"] = normals
    vertices["uv"] = uvs
    counts = struct.pack("<ii", len(vertices), len(indices))
    return head + counts + vertices.tobytes() + indices.astype("<u2").tobytes()


def export_skin(original, objects, skeleton_bone_names=None):
    """
    The bytes of the ``.ski`` file ``original`` with the meshes of ``objects`` replaced.
    ``skeleton_bone_names()`` gives the Studio bone name of each skeleton bone, for version 8 skins.
    """
    import _angelica
    layout = _angelica.ski_layout(original)
    version = layout["version"]
    if version not in (8, 9):
        raise ValueError("arquivos .ski da versão {:d} ainda não são gravados".format(version))
    if layout["is_text"]:
        raise ValueError("arquivos .ski em modo texto ainda não são gravados")
    meshes = layout["meshes"]
    skin_meshes = [ob for ob in objects
                   if 0 <= int(ob[PROP_SKIN_MESH]) < len(meshes) and meshes[int(ob[PROP_SKIN_MESH])]["type"] == "SKIN"]
    if version == 9:
        bone_names = list(layout["bone_names"])
        # Bones newly weighted in the Studio join the skin's bone list (matched by name to the skeleton).
        for ob in skin_meshes:
            for name in _weighted_groups(ob):
                if name not in bone_names:
                    bone_names.append(name)
    else:
        bone_names = list(skeleton_bone_names()) if skin_meshes else []
        for ob in skin_meshes:
            missing = [name for name in _weighted_groups(ob) if name not in bone_names]
            if missing:
                raise ValueError("'{:s}' tem pesos para ossos que o esqueleto não tem: {:s}".format(
                    ob.name, ", ".join(missing[:5])))
    if len(bone_names) > 255:
        raise ValueError("a skin usaria {:d} ossos; o jogo aceita até 255".format(len(bone_names)))
    # A model can use the same skin twice (a weapon in each hand): its copies must agree.
    by_index = {}
    for ob in objects:
        index = int(ob[PROP_SKIN_MESH])
        if not 0 <= index < len(meshes):
            raise ValueError("'{:s}' aponta para uma malha que o arquivo não tem".format(ob.name))
        if meshes[index]["type"] not in WRITABLE_TYPES:
            raise ValueError("malhas do tipo {:s} ('{:s}') ainda não são gravadas".format(meshes[index]["type"], ob.name))
        by_index.setdefault(index, []).append(ob)
    replaced = {}
    for index, copies in by_index.items():
        if len({mesh_fingerprint(ob) for ob in copies}) > 1:
            raise ValueError("'{:s}' e '{:s}' são a mesma malha do arquivo com edições diferentes".format(
                copies[0].name, copies[1].name))
        replaced[index] = _encode_record(copies[0], original, meshes[index], bone_names)

    out = bytearray()
    position = 0
    if version == 9 and bone_names != layout["bone_names"]:
        begin, end = layout["bone_names_range"]
        table = b"".join(struct.pack("<i", len(n.encode("gbk"))) + n.encode("gbk") for n in bone_names)
        out += original[position:begin] + table
        position = end
        # Header: the skin bone count (after the tag, version and six other counts).
        struct.pack_into("<i", out, layout["header_offset"] + 32, len(bone_names))
    for index in sorted(replaced):
        begin, end = meshes[index]["range"]
        out += original[position:begin] + replaced[index]
        position = end
    out += original[position:]
    return bytes(out)
