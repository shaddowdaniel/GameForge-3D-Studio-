# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Copying assets from one game client into another, with every file they use.

The files come from the engine's own dependency collector (``_angelica.dependencies``): model,
skin model, skins, skeleton, animations, textures, effects, sounds. Each goes where the target
client looks for it: the package of its folder (``models.pck``, ``gfx.pck``, ...), or a loose file
when the target has no package there. Files the target already has with the same bytes are left
alone; files it has with other bytes are kept unless the user chooses to replace them.

Older clients do not read the newest versions of some formats. Files newer than what the target
client uses are converted when the Studio knows how (track sets 2 -> 1, skins 100/101 -> 9) and
reported otherwise.

Writing goes through ``save``: into the target client itself (with undo) or into a folder.
"""

__all__ = (
    "CopyItem",
    "RecordCopy",
    "copy",
    "copy_record",
    "plan",
    "plan_record",
)

import os
import re
import struct

import numpy as np

from . import anims, data, icons, save, sources

# Formats whose version the copy checks: extension -> (package to sample, binary tag or None).
_VERSIONED = {".stck": b"STCK", ".ski": b"ASKI", ".smd": b"ASMD", ".bon": b"ASKE", ".ecm": None}
# Highest versions seen per target client (sampled once per session): {root: {ext: version}}.
_target_versions = {}


class CopyItem:
    """One file of the copy and what happens to it."""
    __slots__ = ("path", "kind", "status", "size", "note", "convert", "unsupported", "target")

    def __init__(self, path, kind, status, size=0, note="", convert=None, unsupported=False, target=None):
        self.path = path
        # Where the file goes in the target client (another folder for armor skin sets).
        self.target = target or path
        self.kind = kind
        # 'NEW', 'SAME', 'DIFFERENT' (the target has other bytes) or 'MISSING' (not in the source).
        self.status = status
        self.size = size
        self.note = note
        # (from_version, to_version) when the file is converted for the target.
        self.convert = convert
        # Newer than the target client reads, with no conversion.
        self.unsupported = unsupported


def _file_version(path, data):
    """Version of a model file, None when the format is not versioned here."""
    ext = os.path.splitext(path)[1].lower()
    if ext not in _VERSIONED or not data:
        return None
    body = data[4:] if data[:4] in (b"MOXB", b"MOXT") else data
    if ext == ".ecm":
        text = body[:200]
        index = text.find(b"Version:")
        if index < 0:
            return None
        digits = text[index + 8:].split(b"\n", 1)[0].strip()
        return int(digits) if digits.isdigit() else None
    return struct.unpack_from("<I", body, 4)[0] if len(body) >= 8 else None


def target_versions(root):
    """The highest version of each format the client uses (sampled from its own files)."""
    import _angelica
    if root in _target_versions:
        return _target_versions[root]
    result = {}
    models = os.path.join(root, "models.pck")
    if os.path.isfile(models):
        entries = _angelica.package_entries(models)
        for ext in _VERSIONED:
            files = [p for p, size, _s in entries if p.lower().endswith(ext) and size > 0]
            best = None
            for p in files[:: max(1, len(files) // 150)]:
                version = _file_version(p, _angelica.package_read(models, p))
                if version is not None and (best is None or version > best):
                    best = version
            if best is not None:
                result[ext] = best
        # Sound elements of the EC models (1.5.5 based clients read up to SoundVer 5).
        sound = None
        ecms = [p for p, size, _s in entries if p.lower().endswith(".ecm") and size > 0]
        for p in ecms[:: max(1, len(ecms) // 150)]:
            for match in re.finditer(rb"SoundVer:\s*(\d+)", _angelica.package_read(models, p) or b""):
                sound = max(sound or 0, int(match.group(1)))
        if sound is not None:
            result["soundver"] = sound
        elif result.get(".ecm", 99) <= 67:
            result["soundver"] = 5
    _target_versions[root] = result
    return result


# -----------------------------------------------------------------------------
# Conversions


def _stck_to_v1(data, _supported=None):
    """A version 2 track set as version 1 (one key per frame, one segment per track)."""
    import _angelica
    tracks = _angelica.stck_decode(data)
    offset = tracks["header_offset"]
    out = bytearray(data[:offset]) + data[offset:offset + 4] + struct.pack("<I", 1)
    out += struct.pack("<iiii", len(tracks["tracks"]), tracks["anim_start"], tracks["anim_end"], tracks["anim_fps"])
    anim_ms = int(round((tracks["anim_end"] - tracks["anim_start"]) * 1000.0 / max(tracks["anim_fps"], 1)))
    for track in tracks["tracks"]:
        out += struct.pack("<i", track["id"])
        for kind in ("position", "rotation"):
            data_track = track[kind]
            n = anims._dense_frame_count(data_track)
            values = np.array([anims._sample(data_track, f) for f in range(n)], dtype="<f4")
            rate = max(data_track["frame_rate"], 1)
            end_ms = int(round((n - 1) * 1000.0 / rate)) if n > 1 else anim_ms
            out += struct.pack("<iii", n, 1, data_track["frame_rate"]) + values.tobytes()
            out += struct.pack("<iiii", 0, end_ms, 0, max(n - 1, 0))
    return bytes(out)


def _ski_to_v9(data, _supported=None):
    """A version 100/101 skin as version 9: the per vertex tangents are dropped."""
    import _angelica
    layout = _angelica.ski_layout(data)
    out = bytearray()
    position = 0
    for mesh in layout["meshes"]:
        if mesh["type"] not in ("SKIN", "RIGID", "MORPH_SKIN", "MORPH_RIGID"):
            continue
        begin = mesh["range"][0]
        name_len = struct.unpack_from("<i", data, begin)[0]
        rigid = mesh["type"] in ("RIGID", "MORPH_RIGID")
        head = 4 + name_len + (20 if rigid else 16)
        tangents = begin + head + mesh["vertices"] * (32 if rigid else 48) + mesh["indices"] * 2
        out += data[position:tangents]
        position = tangents + mesh["vertices"] * 16
    out += data[position:]
    struct.pack_into("<I", out, layout["header_offset"] + 4, 9)
    return bytes(out)


def _smd_to_v8(data, _supported=None):
    """
    A version 9 skin model as version 8: actions lose their own frame rate (version 8 plays them
    at the rate of their track set). Layout: header (84 bytes), skeleton, skins, physique and track
    folder strings, then per action its name, start and end frame, [fps], track set file.
    """
    base = 4 if data[:4] == b"MOXB" else 0
    num_skins, num_actions = struct.unpack_from("<ii", data, base + 8)
    position = base + 84

    def string_end(at):
        return at + 4 + struct.unpack_from("<i", data, at)[0]

    for _ in range(num_skins + 3):  # Skeleton, skins, physique, track folder.
        position = string_end(position)
    out = bytearray(data[:position])
    for _ in range(num_actions):
        frames_end = string_end(position) + 8
        out += data[position:frames_end]
        position = string_end(frames_end + 4)
        out += data[frames_end + 4:position]
    out += data[position:]
    struct.pack_into("<I", out, base + 4, 8)
    return bytes(out)


_SOUND_VER = re.compile(r"^SoundVer:\s*(\d+)")


def _ecm_to_v67(data, supported=None):
    """
    A version 68-71 EC model (text) as version 67, from the 1.7.8 `EC_Model.cpp` /
    `A3DCombinedAction.cpp` gates: V68 audio event groups (11 `AudioEventGroupEnable` lines after
    `ComActCount`, `Group` after `Volume` in audio events), V69 `GfxUseFixedPoint` in effect events,
    V70 `ParticleBonesCount` + its lines after the groups, V71 `FadeOut` after the audio event's
    `Group`. Lines are removed by position (an audio event has another `FadeOut`, sound events have
    a `Group` too), so files that already have the old layout under a newer number pass unchanged.
    Clients on the 1.5.5 effect library also read sound elements only up to `SoundVer` 5: then
    the two lines after `SilentHeader` (`PercentStart`, `Group`) go too.
    """
    text, encoding, newline = sources.decode_text(data)
    lines = text.split("\n")
    max_sound = (supported or {}).get("soundver")
    lines[0] = re.sub(r"Version:\s*\d+", "Version: 67", lines[0], count=1)
    out = []
    event_type = None
    i = 0
    while i < len(lines):
        line = lines[i]
        out.append(line)
        i += 1
        if line.startswith("ComActCount:"):
            groups = 0
            while i < len(lines) and groups < 11 and lines[i].startswith("AudioEventGroupEnable:"):
                i += 1
                groups += 1
            if i < len(lines) and lines[i].startswith("ParticleBonesCount:"):
                count = int(lines[i].split(":", 1)[1].strip() or 0)
                i += 1 + count
        elif line.startswith("EventType:"):
            event_type = int(line.split(":", 1)[1].strip() or -1)
        elif event_type == 100 and line.startswith("GfxUseFixedPoint:"):
            out.pop()
        elif event_type == 109 and line.startswith("Volume:"):
            if i < len(lines) and lines[i].startswith("Group:"):
                i += 1
            if i < len(lines) and lines[i].startswith("FadeOut:"):
                i += 1
        elif event_type == 101 and max_sound is not None:
            match = _SOUND_VER.match(line)
            if match and int(match.group(1)) > max_sound:
                out[-1] = "SoundVer: {:d}".format(max_sound)
            elif line.startswith("SilentHeader:") and max_sound <= 5:
                if i < len(lines) and lines[i].startswith("PercentStart:"):
                    i += 1
                if i < len(lines) and lines[i].startswith("Group:"):
                    i += 1
    return sources.encode_text("\n".join(out), encoding, newline)


# Converters by extension: (source versions, highest target version, function, result version).
_CONVERTERS = {
    ".stck": ({2}, 1, _stck_to_v1, 1),
    ".ski": ({100, 101}, 9, _ski_to_v9, 9),
    ".smd": ({9}, 8, _smd_to_v8, 8),
    ".ecm": ({68, 69, 70, 71}, 67, _ecm_to_v67, 67),
}


def _conversion(path, version, supported):
    """``(converter, result version)`` when the target needs ``path`` converted, else None."""
    ext = os.path.splitext(path)[1].lower()
    if version is None or supported is None or version <= supported:
        return None
    entry = _CONVERTERS.get(ext)
    if entry and version in entry[0] and entry[1] <= supported:
        return entry[2], entry[3]
    return False


# -----------------------------------------------------------------------------
# Plan and copy


def plan(src_root, path, dst_root, remap=None):
    """
    What copying ``path`` (and everything it uses) from one client into another involves.
    ``remap(path)`` gives the target path of a file when it differs (armor skin sets).
    """
    import _angelica
    supported = target_versions(dst_root)
    items = []
    for dep in _angelica.dependencies(src_root, path):
        target = remap(dep["path"]) if remap else dep["path"]
        if dep["location"] is None:
            items.append(CopyItem(dep["path"], dep["kind"], 'MISSING'))
            continue
        data = _angelica.read(src_root, dep["path"])
        if data is None:
            items.append(CopyItem(dep["path"], dep["kind"], 'MISSING'))
            continue
        ext = os.path.splitext(dep["path"])[1].lower()
        version = _file_version(dep["path"], data)
        conversion = _conversion(dep["path"], version, supported.get(ext))
        note, convert, unsupported = "", None, False
        if conversion:
            data = conversion[0](data, supported)
            convert = (version, conversion[1])
            note = "convertido da versão {:d} para {:d}".format(*convert)
        elif conversion is False:
            note = "versão {:d}: o cliente de destino usa até {:d}".format(version, supported[ext])
            unsupported = True
        existing = _angelica.read(dst_root, target)
        if existing is None:
            status = 'NEW'
        elif existing == data:
            status = 'SAME'
        else:
            status = 'DIFFERENT'
        items.append(CopyItem(dep["path"], dep["kind"], status, len(data), note, convert, unsupported, target))
    return items


class _CopyChange(save.Change):
    """A file of a copy: its bytes come from the source client (``src_path``, by default the same path)."""
    __slots__ = ("src_root", "conversion", "src_path")

    def __init__(self, dst_root, path, src_root, conversion, src_path=None):
        super().__init__(dst_root, path, 'COPY', None)
        self.src_root = src_root
        self.conversion = conversion
        self.src_path = src_path or path

    @property
    def label(self):
        return self.path

    def data(self):
        import _angelica
        data = _angelica.read(self.src_root, self.src_path)
        if data is None:
            raise save.SaveError("o cliente de origem não tem mais o arquivo")
        return self.conversion(data, target_versions(self.root)) if self.conversion else data

    def mark_saved(self, _data):
        pass


def copy(src_root, dst_root, items, replace_different=False, include_unsupported=False,
         mode='IN_PLACE', out_dir=""):
    """
    Write the files of a :func:`plan` into the target client (or a folder, see ``save.save``):
    new files, and files the target has with other bytes when ``replace_different``. Files newer
    than the target reads are left out unless ``include_unsupported``. Returns the files written;
    raises ``save.SaveError``.
    """
    changes = []
    for item in items:
        if item.unsupported and not include_unsupported:
            continue
        if item.status == 'NEW' or (item.status == 'DIFFERENT' and replace_different):
            ext = os.path.splitext(item.path)[1].lower()
            conversion = _CONVERTERS[ext][2] if item.convert else None
            changes.append(_CopyChange(dst_root, item.target, src_root, conversion, item.path))
    if not changes:
        return []
    return save.save(changes, mode, out_dir)


# -----------------------------------------------------------------------------
# Game data records


class RecordCopy:
    """Copying a record of ``elements.data``: the data part and the files it names."""

    def __init__(self, src_root, dst_root, list_name, record_id, data_plan, elements, items, extra=None,
                 notes=None):
        self.src_root = src_root
        self.dst_root = dst_root
        self.list_name = list_name
        self.record_id = record_id
        # ``data.DataCopy``: records added / shared, new ids, unresolved references.
        self.data = data_plan
        # The target's new ``elements.data``.
        self.elements = elements
        # ``CopyItem`` per file the records (and those files) use.
        self.items = items
        # Other target files rewritten by the copy (descriptions, icon atlases): {path: bytes}.
        self.extra = extra or {}
        # What the copy could not do, for the user.
        self.notes = notes or []


_ITEM_TEXTS = ("configs\\item_ext_desc.txt", "configs\\item_color.txt")
_ID_LINE = re.compile(r"^(\d+)([ \t]+)(.*)$")


def _copied_id_lines(src_root, dst_root, path, id_map):
    """The target's ``path`` (``id<TAB>value`` lines) with the source lines of the copied ids."""
    import _angelica
    src_data, dst_data = _angelica.read(src_root, path), _angelica.read(dst_root, path)
    if src_data is None or dst_data is None:
        return None
    lines = {}
    for line in sources.decode_text(src_data)[0].split("\n"):
        match = _ID_LINE.match(line)
        if match:
            lines.setdefault(int(match.group(1)), (match.group(2), match.group(3)))
    new = ["{:d}{:s}{:s}".format(new_id, *lines[old_id]) for old_id, new_id in id_map if old_id in lines]
    if not new:
        return None
    text, encoding, newline = sources.decode_text(dst_data)
    body = text[:-1] if text.endswith("\n") else text
    return sources.encode_text(body + "\n" + "\n".join(new) + ("\n" if text.endswith("\n") else ""),
                               encoding, newline)


def _copy_icons(src_root, dst_root, dst, added, notes):
    """
    Icons of the copied records into the target's atlases (male and female), under the same name,
    or a name of their own when the target uses that name for another picture. Returns the
    rewritten atlas files.
    """
    import _angelica
    pairs = []
    for base in icons.ATLASES:
        try:
            pairs.append((icons.IconAtlas(src_root, base), icons.IconAtlas(dst_root, base)))
        except (FileNotFoundError, ValueError) as ex:
            notes.append(str(ex))
    for name, _old_id, new_id in added:
        dst_list = dst.index[name]
        field = dst.field_index[dst_list].get("file_icon")
        if field is None or not pairs:
            continue
        record = dst.find(dst_list, new_id)
        file_icon = dst.record(dst_list, record)[field]
        if not isinstance(file_icon, str) or not file_icon.strip():
            continue
        base_name = file_icon.replace("/", "\\").rsplit("\\", 1)[-1].strip()
        pictures = []
        for src_atlas, dst_atlas in pairs:
            index = src_atlas.find(file_icon)
            if index is None:
                index = pairs[0][0].find(file_icon)
                source = pairs[0][0]
            else:
                source = src_atlas
            pictures.append(source.pixels(index) if index is not None else None)
        if all(p is None for p in pictures):
            notes.append("o ícone {:s} não está no cliente de origem".format(base_name))
            continue
        pictures = [p if p is not None else next(q for q in pictures if q is not None) for p in pictures]
        # Same name, other picture in the target: the copy gets a name of its own.
        conflict = False
        for (_src_atlas, dst_atlas), picture in zip(pairs, pictures):
            index = dst_atlas.find(base_name)
            if index is not None and np.abs(dst_atlas.pixels(index).astype(np.int16) - picture).max() > 8:
                conflict = True
        new_name = "gf{:d}_{:s}".format(new_id, base_name) if conflict else base_name
        for (_src_atlas, dst_atlas), picture in zip(pairs, pictures):
            if dst_atlas.find(new_name) is None:
                dst_atlas.add(new_name, picture)
        if conflict:
            prefix = file_icon[:len(file_icon) - len(file_icon.replace("/", "\\").rsplit("\\", 1)[-1])]
            _angelica.elements_set(dst.handle, dst_list, record, field, prefix + new_name)
    files = {}
    for _src_atlas, dst_atlas in pairs:
        if dst_atlas.changed:
            files[dst_atlas.base + ".txt"] = dst_atlas.txt_bytes()
            files[dst_atlas.base + ".dds"] = dst_atlas.dds_bytes()
    return files


# Folders of armor and fashion skins, by build (`_equipment_skin` in EC_Player.cpp).
_EQUIPMENT_ROOTS = ("models\\players\\装备", "models2\\players\\装备", "styles\\装备", "evolved\\players\\装备")
# Variants: (gender folder, name prefix); index by `_equipment_variants`.
_MALE, _BARBARIAN, _FEMALE, _VENOMANCER = ("男", "男通用"), ("男", "妖兽"), ("女", "女通用"), ("女", "妖精")
_LODS = ("一级", "二级", "三级")


def _equipment_variants(list_name, combo, gender):
    """Skin variants an armor (by class mask) or a fashion (by gender) needs."""
    if list_name == "FASHION_ESSENCE" and gender in (0, 1):
        return [_MALE, _BARBARIAN] if gender == 0 else [_FEMALE, _VENOMANCER]
    if combo is None or combo < 0:
        return [_MALE, _BARBARIAN, _FEMALE, _VENOMANCER]
    variants = []
    if combo & ~(0x08 | 0x10):
        variants += [_MALE, _FEMALE]
    if combo & 0x08:
        variants.append(_VENOMANCER)
    if combo & 0x10:
        variants.append(_BARBARIAN)
    return variants


def _skin_base(root, realname, variant):
    if "/" in realname:  # EvolvedPW: "<pck>/<name>" puts the set under <pck>\players\装备.
        pck, realname = realname.split("/", 1)
        root = pck + "\\players\\装备"
    return "{:s}\\{:s}\\{:s}\\{:s}{:s}".format(root, variant[0], realname, variant[1], realname)


def equipment_root(root_dir, realname=None):
    """The folder a client keeps armor skins in: the one that has ``realname`` (or the defaults)."""
    import _angelica
    for candidate in _EQUIPMENT_ROOTS:
        probes = ([_skin_base(candidate, realname, v) + _LODS[0] + ".ski"
                   for v in (_MALE, _FEMALE, _BARBARIAN, _VENOMANCER)] if realname else
                  [candidate + "\\男\\默认上衣\\武侠男默认上衣一级.ski",
                   candidate + "\\女\\默认上衣\\武侠女默认上衣一级.ski"])
        if any(_angelica.locate(root_dir, p) for p in probes):
            return candidate
    return None


def equipment_skins(src_root, dst_root, list_name, realname, combo, gender):
    """``(source path, target path)`` of every skin file of an armor or fashion that the source has."""
    import _angelica
    src_dir = equipment_root(src_root, realname)
    if src_dir is None:
        return []
    dst_dir = equipment_root(dst_root) or src_dir
    pairs = []
    for variant in _equipment_variants(list_name, combo, gender):
        base = _skin_base(src_dir, realname, variant)
        for lod in _LODS:
            path = base + lod + ".ski"
            if _angelica.locate(src_root, path):
                pairs.append((path, dst_dir + path[len(src_dir):] if "/" not in realname else path))
    return pairs


def plan_record(src_root, list_name, record_id, dst_root, follow_items=False):
    """What copying a record (with the records and files it uses) into another client involves."""
    src = data.ClientData(src_root)
    dst = data.ClientData(dst_root)
    notes = []
    try:
        rules = data.LinkRules()
        data_plan = data.plan_record_copy(src, dst, list_name, record_id, rules, follow_items)
        data.apply_record_copy(src, dst, data_plan, rules)
        extra = _copy_icons(src_root, dst_root, dst, data_plan.added, notes)
        elements = dst.to_bytes()
        spaces = {entry["name"]: entry["id_space"] for entry in dst.lists}
    finally:
        src.close()
        dst.close()
    # Descriptions and colors are per item id (essence space).
    essence = [(old_id, new_id) for name, old_id, new_id in data_plan.added if spaces.get(name) == "ESSENCE"]
    for path in _ITEM_TEXTS:
        text = _copied_id_lines(src_root, dst_root, path, essence)
        if text is not None:
            extra[path] = text
    items, seen = [], set()
    for path in data_plan.files:
        engine_path = path.replace("/", "\\")
        for item in plan(src_root, engine_path, dst_root):
            if item.path.lower() not in seen:
                seen.add(item.path.lower())
                items.append(item)
    # Armor and fashion skin sets, moved into the folder the target client keeps them in.
    for name, realname, combo, gender in data_plan.equipment:
        pairs = equipment_skins(src_root, dst_root, name, realname, combo, gender)
        if not pairs:
            notes.append("as skins de '{:s}' não estão no cliente de origem".format(realname))
        for src_path, dst_path in pairs:
            src_dir = src_path.rsplit("\\", 1)[0].lower()
            dst_dir = dst_path.rsplit("\\", 1)[0]

            def remap(p, src_dir=src_dir, dst_dir=dst_dir):
                return dst_dir + p[len(src_dir):] if p.lower().startswith(src_dir) else p
            for item in plan(src_root, src_path, dst_root, remap):
                if item.path.lower() not in seen:
                    seen.add(item.path.lower())
                    items.append(item)
    return RecordCopy(src_root, dst_root, list_name, record_id, data_plan, elements, items, extra, notes)


def _server_elements(record_copy, server_folder):
    """
    The server's new ``elements.data``: the client's when both were the same file, otherwise the
    same records written with the same ids into the server's own file (refused when the ids do not
    fit there).
    """
    import _angelica
    path = os.path.join(server_folder, "elements.data")
    with open(path, "rb") as fh:
        server = fh.read()
    if server == _angelica.read(record_copy.dst_root, "data\\elements.data"):
        return record_copy.elements
    src = data.ClientData(record_copy.src_root)
    target = data.ClientData(server_folder, server)
    try:
        for name, _old_id, new_id in record_copy.data.added:
            space = target.lists[target.index[name]]["id_space"] if name in target.index else None
            if space is None or target.find_in_space(space, new_id) is not None:
                raise save.SaveError("o elements.data do servidor é outro arquivo e não tem lugar para "
                                     "{:s} {:d}: copie o do cliente para o servidor antes".format(name, new_id))
        for name, record_id in record_copy.data.shared:
            if name not in target.index or target.find(target.index[name], record_id) < 0:
                raise save.SaveError("o elements.data do servidor é outro arquivo e não tem {:s} {:d}".format(
                    name, record_id))
        data.apply_record_copy(src, target, record_copy.data)
        return target.to_bytes()
    finally:
        src.close()
        target.close()


def copy_record(record_copy, replace_different=False, include_unsupported=False, mode='IN_PLACE', out_dir="",
                server_folder=""):
    """
    Write a :func:`plan_record` (target elements.data, descriptions, icons, files and, when
    ``server_folder`` is given, the server's elements.data) in one save.
    """
    changes = [save.BytesChange(record_copy.dst_root, "data\\elements.data", record_copy.elements)]
    if server_folder:
        server = save.BytesChange(server_folder, "elements.data", _server_elements(record_copy, server_folder))
        server.folder = "servidor"
        changes.append(server)
    changes += [save.BytesChange(record_copy.dst_root, path, payload)
                for path, payload in record_copy.extra.items()]
    for item in record_copy.items:
        if item.unsupported and not include_unsupported:
            continue
        if item.status == 'NEW' or (item.status == 'DIFFERENT' and replace_different):
            ext = os.path.splitext(item.path)[1].lower()
            conversion = _CONVERTERS[ext][2] if item.convert else None
            changes.append(_CopyChange(record_copy.dst_root, item.target, record_copy.src_root, conversion, item.path))
    return save.save(changes, mode, out_dir)
