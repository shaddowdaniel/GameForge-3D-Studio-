# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Edited animations written back into their track set (``.stck``).

The importer bakes a track set into an action (``gameforge_file``, ``gameforge_anim_start``,
``gameforge_anim_end``, ``gameforge_anim_fps``, ``gameforge_frame_scale``): engine frame ``f`` is
Blender frame ``1 + (f - start) * frame_scale``, with the pose bone location and quaternion
relative to the rest pose. Bone matrices are made proper rotations for Blender (a mirrored bone,
``A3DBone`` flag 0x02, gets its X axis negated).

Saving reverses that: the action's curves are evaluated at the frames of every bone track, the
model space matrices rebuilt (mirrors restored), and each bone's position and rotation in its
parent's space compared with the track. Tracks that match are copied byte for byte; the others are
written dense (one key per frame of the track) in the file's own version, keeping the keys outside
the action's frame range.

Track set layout: ``'STCK'``, version, bone track count, anim start, end and fps, then per bone
track the joint id, the position track and the rotation track. A track starts with key count,
segment count and frame rate, then
- version 1: the keys (``x, y, z, w`` for rotations) and the segments ``(start ms, end ms, first
  key, last key)``; the engine's files use one segment from 0 to the time of the last frame;
- version 2: [rotations: key format, 0 = ``x, y, z, w``] the keys, the length in ms and the
  compression (0 = one key per frame).
"""

__all__ = (
    "action_fingerprint",
    "export_action",
    "export_skeleton",
    "rest_fingerprint",
    "skeleton_actions",
)

import hashlib
import struct

import numpy as np

PROP_FILE = "gameforge_file"
PROP_CLIENT = "gameforge_client"
PROP_SKELETON = "gameforge_skeleton_file"
PROP_SCALE = "gameforge_scale"
PROP_START = "gameforge_anim_start"
PROP_END = "gameforge_anim_end"
PROP_FPS = "gameforge_anim_fps"
PROP_FRAME_SCALE = "gameforge_frame_scale"
PROP_KEYS = "gameforge_keys"
PROP_REST = "gameforge_rest"

BONE_FLAG_FLIPPED = 0x02
# Swaps Y and Z: the engine's Y up is Blender's Z up.
_SWAP = np.array([[1, 0, 0, 0], [0, 0, 1, 0], [0, 1, 0, 0], [0, 0, 0, 1]], dtype=np.float64)


def _channelbag(action, ob=None):
    from bpy_extras import anim_utils
    slot = None
    if ob is not None and ob.animation_data and ob.animation_data.action == action:
        slot = ob.animation_data.action_slot
    if slot is None:
        slot = anim_utils.action_get_first_suitable_slot(action, 'OBJECT')
    return anim_utils.action_get_channelbag_for_slot(action, slot) if slot else None


def action_fingerprint(action):
    """A hash of the keys of an action, to detect edits."""
    channelbag = _channelbag(action)
    h = hashlib.sha1()
    if channelbag is None:
        return h.hexdigest()
    for fcurve in sorted(channelbag.fcurves, key=lambda fc: (fc.data_path, fc.array_index)):
        points = np.empty(len(fcurve.keyframe_points) * 2, dtype=np.float32)
        fcurve.keyframe_points.foreach_get("co", points)
        h.update(fcurve.data_path.encode("utf-8"))
        h.update(struct.pack("<i", fcurve.array_index))
        h.update(points.tobytes())
    return h.hexdigest()


def skeleton_actions(actions):
    """``{(client, track set file): [actions]}`` for the actions made from client track sets."""
    result = {}
    for action in actions:
        if PROP_FILE in action and PROP_CLIENT in action and PROP_KEYS in action:
            result.setdefault((action[PROP_CLIENT], action[PROP_FILE]), []).append(action)
    return result


# -----------------------------------------------------------------------------
# Matrices


def _engine_to_blender(m, scale):
    """Row-major engine matrix (16 floats, row vectors) to a Blender matrix (column vectors)."""
    b = _SWAP @ np.asarray(m, dtype=np.float64).reshape(4, 4).T @ _SWAP
    b[:3, 3] *= scale
    return b


def _blender_to_engine(b, scale):
    b = b.copy()
    b[:3, 3] /= scale
    return (_SWAP @ b @ _SWAP).T


def _quat_to_matrix(w, x, y, z):
    return np.array([
        [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w), 0],
        [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w), 0],
        [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y), 0],
        [0, 0, 0, 1]], dtype=np.float64)


def _engine_quaternion(e):
    """``(x, y, z, w)`` of the rotation of a row-vector engine matrix (``D3DXQuaternionRotationMatrix``)."""
    r = e[:3, :3].T  # Column-vector rotation.
    trace = r[0, 0] + r[1, 1] + r[2, 2]
    if trace > 0.0:
        s = 0.5 / np.sqrt(trace + 1.0)
        w, x, y, z = 0.25 / s, (r[2, 1] - r[1, 2]) * s, (r[0, 2] - r[2, 0]) * s, (r[1, 0] - r[0, 1]) * s
    elif r[0, 0] > r[1, 1] and r[0, 0] > r[2, 2]:
        s = 2.0 * np.sqrt(1.0 + r[0, 0] - r[1, 1] - r[2, 2])
        w, x, y, z = (r[2, 1] - r[1, 2]) / s, 0.25 * s, (r[0, 1] + r[1, 0]) / s, (r[0, 2] + r[2, 0]) / s
    elif r[1, 1] > r[2, 2]:
        s = 2.0 * np.sqrt(1.0 + r[1, 1] - r[0, 0] - r[2, 2])
        w, x, y, z = (r[0, 2] - r[2, 0]) / s, (r[0, 1] + r[1, 0]) / s, 0.25 * s, (r[1, 2] + r[2, 1]) / s
    else:
        s = 2.0 * np.sqrt(1.0 + r[2, 2] - r[0, 0] - r[1, 1])
        w, x, y, z = (r[1, 0] - r[0, 1]) / s, (r[0, 2] + r[2, 0]) / s, (r[1, 2] + r[2, 1]) / s, 0.25 * s
    q = np.array([x, y, z, w])
    return q / np.linalg.norm(q)


# -----------------------------------------------------------------------------
# Tracks


def _sample(track, frame):
    """Value of a decoded track at ``frame`` (its own frames): linear, normalized for rotations."""
    n = track["components"]
    keys = np.frombuffer(track["keys"], dtype=np.float32).reshape(-1, n)
    frames = np.frombuffer(track["key_frames"], dtype=np.float32)
    if len(keys) == 0:
        return None
    if len(keys) == 1 or frame <= frames[0]:
        return keys[0].astype(np.float64)
    if frame >= frames[-1]:
        return keys[-1].astype(np.float64)
    hi = int(np.searchsorted(frames, frame, side="left"))
    lo = hi - 1
    span = frames[hi] - frames[lo]
    t = (frame - frames[lo]) / span if span > 0 else 0.0
    a, b = keys[lo].astype(np.float64), keys[hi].astype(np.float64)
    if n == 4:
        if np.dot(a, b) < 0:
            b = -b
        value = a * (1 - t) + b * t
        return value / np.linalg.norm(value)
    return a * (1 - t) + b * t


def _dense_frame_count(track):
    """Frames of a track, one key each when written dense."""
    if track["segments"]:
        end_ms = max(segment[1] for segment in track["segments"])
        return int(round(end_ms * track["frame_rate"] / 1000.0)) + 1
    frames = np.frombuffer(track["key_frames"], dtype=np.float32)
    return int(round(float(frames.max()))) + 1 if len(frames) else 0


def _pack_track(version, track, values, rotation):
    """A dense track in the layout of ``version``."""
    values = np.asarray(values, dtype="<f4")
    if version < 2:
        segments = track["segments"]
        end_ms = max(s[1] for s in segments) if segments else \
            int(round((len(values) - 1) * 1000.0 / max(track["frame_rate"], 1)))
        return (struct.pack("<iii", len(values), 1, track["frame_rate"]) + values.tobytes() +
                struct.pack("<iiii", 0, end_ms, 0, len(values) - 1))
    out = struct.pack("<iii", len(values), 0, track["frame_rate"])
    if rotation:
        out += struct.pack("<i", 0)
    return out + values.tobytes() + struct.pack("<ii", track["track_length_ms"], 0)


def _matches(original, values, rotation):
    """Whether new key values are the original ones up to the round trip's float noise."""
    if rotation:
        # Exporters leave some keys slightly off unit length.
        original = original / np.linalg.norm(original, axis=1, keepdims=True)
        return bool(np.all(np.abs(np.sum(original * values, axis=1)) >= 1.0 - 1e-8))
    return bool(np.all(np.abs(original - values) <= 1e-4))


# Blender keeps a bone's rest matrix as head, tail and roll, which loses ~1e-5: closer than this to
# the skeleton file counts as unchanged.
REST_TOLERANCE = 1e-4


def _imported_rest(info, scale):
    """The rest matrix the importer gave a skeleton bone (armature space)."""
    stored = np.linalg.inv(_engine_to_blender(np.frombuffer(info["bone_init"], dtype=np.float32), scale))
    return _orthonormal(stored), stored


def _rest_matrices(armature_ob, bones, names, scale):
    """
    Rest matrix of every skeleton bone: the exact one of the file for bones the user did not move
    (so unedited poses go back unchanged), the armature's for moved ones, None for missing bones.
    """
    result = []
    for info, name in zip(bones, names):
        bone = armature_ob.data.bones.get(name) if name else None
        if bone is None:
            result.append(None)
            continue
        current = np.array(bone.matrix_local, dtype=np.float64)
        imported = _imported_rest(info, scale)[0]
        result.append(imported if np.allclose(current, imported, atol=REST_TOLERANCE) else current)
    return result


class _Pose:
    """Evaluates an action on the armature's bones in skeleton order."""

    def __init__(self, armature_ob, action, bones, scale):
        channelbag = _channelbag(action, armature_ob)
        curves = {}
        if channelbag is not None:
            for fcurve in channelbag.fcurves:
                curves[(fcurve.data_path, fcurve.array_index)] = fcurve
        self.curves = curves
        self.bones = bones
        self.names = _armature_names(armature_ob, bones)
        rest = _rest_matrices(armature_ob, bones, self.names, scale)
        self.rest_local = []
        for i, info in enumerate(bones):
            parent = info["parent"]
            if rest[i] is None:
                self.rest_local.append(None)
            elif 0 <= parent < len(bones) and parent != i and rest[parent] is not None:
                self.rest_local.append(np.linalg.inv(rest[parent]) @ rest[i])
            else:
                self.rest_local.append(rest[i])
        self.order = _parent_first(bones)
        # Mirror sign of each bone in model space: the product of the flips along its chain.
        self.sign = [1.0] * len(bones)
        for i in self.order:
            parent = bones[i]["parent"]
            own = -1.0 if bones[i]["flags"] & BONE_FLAG_FLIPPED else 1.0
            self.sign[i] = own * (self.sign[parent] if 0 <= parent < len(bones) and parent != i else 1.0)

    def _value(self, path, index, frame, default):
        fcurve = self.curves.get((path, index))
        return fcurve.evaluate(frame) if fcurve is not None else default

    def basis(self, i, frame):
        name = self.names[i]
        prefix = 'pose.bones["{:s}"].'.format(name.replace("\\", "\\\\").replace('"', '\\"'))
        loc = [self._value(prefix + "location", k, frame, 0.0) for k in range(3)]
        q = np.array([self._value(prefix + "rotation_quaternion", k, frame, (1.0, 0.0, 0.0, 0.0)[k])
                      for k in range(4)], dtype=np.float64)
        length = np.linalg.norm(q)
        q = q / length if length > 1e-12 else np.array([1.0, 0.0, 0.0, 0.0])
        m = _quat_to_matrix(*q)
        m[:3, 3] = loc
        return m

    def engine_locals(self, frame, scale):
        """Per skeleton bone: the engine relative matrix at ``frame`` (None for bones the
        armature does not have)."""
        count = len(self.bones)
        world = [None] * count
        result = [None] * count
        for i in self.order:
            if self.rest_local[i] is None:
                continue
            parent = self.bones[i]["parent"]
            has_parent = 0 <= parent < count and parent != i and world[parent] is not None
            fixed = (world[parent][1] if has_parent else np.identity(4)) @ self.rest_local[i] @ self.basis(i, frame)
            true_world = fixed.copy()
            true_world[:3, 0] *= self.sign[i]
            world[i] = (true_world, fixed)
            local = np.linalg.inv(world[parent][0]) @ true_world if has_parent else true_world
            e = _blender_to_engine(local, scale)
            if self.bones[i]["flags"] & BONE_FLAG_FLIPPED:
                e[0, :3] = -e[0, :3]
            result[i] = e
        return result


def _armature_names(armature_ob, bones):
    """Blender bone name of every skeleton bone (Blender makes repeated names unique)."""
    available = {b.name for b in armature_ob.data.bones}
    used = set()
    names = []
    for info in bones:
        base = info["name"] or "Bone"
        candidates = [base] + ["{:s}.{:03d}".format(base, n) for n in range(1, 1000)]
        name = next((c for c in candidates if c in available and c not in used), "")
        used.add(name)
        names.append(name)
    return names


def _parent_first(bones):
    order, done = [], [False] * len(bones)
    while len(order) < len(bones):
        before = len(order)
        for i, info in enumerate(bones):
            parent = info["parent"]
            if not done[i] and (parent < 0 or parent >= len(bones) or parent == i or done[parent]):
                done[i] = True
                order.append(i)
        if len(order) == before:
            break
    return order


def export_action(original, skeleton_data, armature_ob, action):
    """The bytes of the track set ``original`` with the keys of ``action`` in its frame range."""
    import _angelica
    tracks = _angelica.stck_decode(original)
    version = tracks["version"]
    bones = _angelica.bon_layout(skeleton_data)["bones"]
    scale = float(armature_ob.get(PROP_SCALE, 1.0)) or 1.0
    pose = _Pose(armature_ob, action, bones, scale)
    start, end = float(action[PROP_START]), float(action[PROP_END])
    anim_fps = float(action[PROP_FPS]) or float(tracks["anim_fps"]) or 30.0
    frame_scale = float(action[PROP_FRAME_SCALE])
    bone_of_joint = {}
    for i, info in enumerate(bones):
        if info["first_joint"] >= 0:
            bone_of_joint.setdefault(info["first_joint"], i)

    # Engine matrices per action frame, computed once per frame for all bones.
    cache = {}

    def locals_at(engine_frame):
        key = round(engine_frame, 4)
        if key not in cache:
            cache[key] = pose.engine_locals(1.0 + (engine_frame - start) * frame_scale, scale)
        return cache[key]

    out = bytearray(original[:tracks["header_offset"]])
    out += original[tracks["header_offset"]:tracks["header_offset"] + 8]  # 'STCK', version.
    out += struct.pack("<iiii", len(tracks["tracks"]), tracks["anim_start"], tracks["anim_end"], tracks["anim_fps"])
    for track in tracks["tracks"]:
        bone = bone_of_joint.get(track["id"])
        out += struct.pack("<i", track["id"])
        for kind in ("position", "rotation"):
            data = track[kind]
            rotation = kind == "rotation"
            values, originals = [], []
            previous = None
            for frame in range(_dense_frame_count(data)):
                engine_frame = frame / data["frame_rate"] * anim_fps if data["frame_rate"] > 0 else frame
                original_value = _sample(data, frame)
                value = None
                if bone is not None and start - 1e-4 <= engine_frame <= end + 1e-4:
                    e = locals_at(engine_frame)[bone]
                    if e is not None:
                        value = _engine_quaternion(e) if rotation else e[3, :3].copy()
                if value is None:
                    value = original_value
                if rotation and previous is not None and np.dot(value, previous) < 0:
                    value = -value
                previous = value
                values.append(value)
                originals.append(original_value)
            width = 4 if rotation else 3
            values = np.array(values).reshape(-1, width)
            if len(values) == 0 or _matches(np.array(originals).reshape(-1, width), values, rotation):
                out += original[data["range"][0]:data["range"][1]]
            else:
                out += _pack_track(version, data, values, rotation)
    return bytes(out)


# -----------------------------------------------------------------------------
# Skeleton rest pose


def rest_fingerprint(armature_ob):
    """A hash of the rest pose of an armature, to detect edits."""
    h = hashlib.sha1()
    for bone in sorted(armature_ob.data.bones, key=lambda b: b.name):
        h.update(bone.name.encode("utf-8"))
        h.update(np.array(bone.matrix_local, dtype=np.float32).tobytes())
    return h.hexdigest()


def _orthonormal(m):
    """The importer's ``orthonormal_bone_matrix``: no scale, mirror moved out of the X axis."""
    x, y, z = m[:3, 0], m[:3, 1], m[:3, 2]
    length = np.linalg.norm(y)
    y = y / length if length > 1e-8 else np.array([0.0, 1.0, 0.0])
    z = z - y * np.dot(z, y)
    length = np.linalg.norm(z)
    z = z / length if length > 1e-8 else np.cross(x, y) / np.linalg.norm(np.cross(x, y))
    out = np.identity(4)
    out[:3, 0], out[:3, 1], out[:3, 2] = np.cross(y, z), y, z
    out[:3, 3] = m[:3, 3]
    return out


def export_skeleton(original, armature_ob):
    """
    The bytes of the skeleton ``original`` with the rest pose of the armature: the bind matrices
    of the bones that moved, and the parent-relative matrices that depend on them, are replaced in
    place; every other byte stays as it was.
    """
    import _angelica
    data = bytearray(original)
    bones = _angelica.bon_layout(original)["bones"]
    scale = float(armature_ob.get(PROP_SCALE, 1.0)) or 1.0
    names = _armature_names(armature_ob, bones)
    count = len(bones)

    # Model space bone matrices (Blender axes), mirrors included, as stored in the file.
    stored = [_imported_rest(b, scale)[1] for b in bones]
    sign = [1.0] * count
    for i in _parent_first(bones):
        parent = bones[i]["parent"]
        own = -1.0 if bones[i]["flags"] & BONE_FLAG_FLIPPED else 1.0
        sign[i] = own * (sign[parent] if 0 <= parent < count and parent != i else 1.0)
    world = list(stored)
    moved = [False] * count
    for i, name in enumerate(names):
        bone = armature_ob.data.bones.get(name) if name else None
        if bone is None:
            continue
        current = np.array(bone.matrix_local, dtype=np.float64)
        if np.allclose(current, _orthonormal(stored[i]), atol=REST_TOLERANCE):
            continue
        moved[i] = True
        current[:3, 0] *= sign[i]
        world[i] = current

    for i, info in enumerate(bones):
        parent = info["parent"]
        has_parent = 0 <= parent < count and parent != i
        if not (moved[i] or (has_parent and moved[parent])):
            continue
        local = np.linalg.inv(world[parent]) @ world[i] if has_parent else world[i]
        offset = info["matrices_offset"]
        struct.pack_into("<16f", data, offset, *_blender_to_engine(local, scale).ravel())
        if moved[i]:
            struct.pack_into("<16f", data, offset + 64, *_blender_to_engine(np.linalg.inv(world[i]), scale).ravel())
    return bytes(data)
