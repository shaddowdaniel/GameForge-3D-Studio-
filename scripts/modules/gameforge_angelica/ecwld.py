# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
The client scene of a map: ``maps\\<code>\\<code>.ecwld`` (the world file: per block of the map,
its trees, effects, models and sounds, and references to the rest) and ``<code>.ecbsd`` (block
shared data: buildings, environment areas, critters, routes, ...).

Layouts from the element client: ``EC_WorldFile.h`` (default 8-byte packing, so records ending in
``bool`` carry padding) and the loaders in ``EC_SceneBlock.cpp``, ``EC_Scene.cpp``,
``EC_ManOrnament.cpp``, ``EC_Ornament.cpp``, ``EC_ScnBoxArea.cpp``, ``EC_CritterGroup.cpp`` and
``EC_Bezier.cpp``. World file versions 4 to 12 and shared data versions 4 to 12 come from that
source. The newer maps of the 1.7.8 and later clients (world files 16 and 17, shared data 17 and
18) have no source; their differences were measured on the files (PWOps, "Mapa 1.7.8: cena", every
map of the 1.7.8 client reads and writes back the same bytes): world file 16 is laid out as 12,
17 adds a float after ``up`` to scene models, and shared data 17 / 18 only grow the environment
area record (364 / 368 bytes, the first 224 as in 11, and 20 strings).

Every block of the world file is kept as its raw records. Moving or turning an object rewrites
only its numbers (an effect, scene model or sound that leaves its block moves to the block it is in
now); effects, scene models and sounds can also be added (copies of another) and removed. The world
file is then laid out again in the same block order, so the blocks and records that did not change
keep their bytes, and an unchanged file comes out identical. The shared data file is only patched.
"""

__all__ = (
    "SceneFile",
    "SceneObject",
)

import struct
from dataclasses import dataclass, field

WORLD_MAGIC = b"DWCE"
BSD_MAGIC = b"FDSB"
WORLD_VERSION_MAX = 17
BSD_VERSION_MAX = 18

_HEADER = struct.Struct("<4sIfffiiiiIIiiI60s")
_BSD_HEADER = struct.Struct("<4sIi60s")
_BLOCK = struct.Struct("<HH10i")
_TREE = struct.Struct("<I3f")
_REF = struct.Struct("<II")
_GRASS_TYPE = struct.Struct("<B3xiffff")
# Effects by world file version: (record, number of fields read).
_EFFECTS = ((6, struct.Struct("<If3f3f3f")), (8, struct.Struct("<If3f3f3ff")),
            (12, struct.Struct("<If3f3f3ffi")), (None, struct.Struct("<If3f3f3ffifB3x")))
_MODEL = struct.Struct("<I3f3f3f")
_MODEL17 = struct.Struct("<I3f3f3ff")
_SOUND = struct.Struct("<I3fffi")
_SOUND10 = struct.Struct("<I3fffii")
_ORNAMENT = struct.Struct("<3f3f3fBB2x")
_GRASS = struct.Struct("<I3ffii")
_GRASS7 = struct.Struct("<I3ffiiii")
_WATER = struct.Struct("<ii")
_CRITTER = struct.Struct("<i3ffii")
_CRITTER5 = struct.Struct("<i3ffiiIf")
# Environment (box) areas: the oriented box starts the record; the size grows with the version.
# (versions below, fixed size, number of strings after it).
_BOX_AREAS = ((6, 120, 4), (8, 184, 7), (9, 192, 7), (10, 200, 7), (11, 208, 7), (17, 224, 7), (18, 364, 20),
              (None, 368, 20))
_BOX_OBB = struct.Struct("<3f3f3f3f3f")
_VEC3 = struct.Struct("<3f")

KINDS = ('TREE', 'WATER', 'BUILDING', 'BOX_AREA', 'GRASS', 'EFFECT', 'MODEL', 'CRITTER', 'BEZIER', 'SOUND')
# Counts in the block header order, and the order of the records in the block.
_HEADER_ORDER = ('TREE', 'WATER', 'GRASS', 'BUILDING', 'BOX_AREA', 'EFFECT', 'MODEL', 'CRITTER', 'BEZIER', 'SOUND')
_DATA_ORDER = ('TREE', 'WATER', 'BUILDING', 'BOX_AREA', 'GRASS', 'EFFECT', 'MODEL', 'CRITTER', 'BEZIER', 'SOUND')
# Kinds stored whole in the world file (the others are references into the shared data file), and
# where their position starts in a record.
INLINE = {'TREE': 4, 'EFFECT': 8, 'MODEL': 4, 'SOUND': 4}
# Inline kinds that can be added and removed (they have an export id).
EDITABLE = ('EFFECT', 'MODEL', 'SOUND')


@dataclass
class _Block:
    index: int
    # Offset in the file it was read from (the blocks are written again in this order).
    offset: int
    row: int
    col: int
    # Kind -> raw records (bytearray).
    sections: dict = field(default_factory=dict)


@dataclass
class SceneObject:
    """
    One object of the scene. ``pos`` is in engine space (Y up); ``dir`` and ``up`` give its turn.
    ``path`` is the file it shows (model, effect, sound, tree) as stored in the scene files.
    """
    kind: str
    export_id: int = 0
    pos: tuple = (0.0, 0.0, 0.0)
    dir: tuple = (0.0, 0.0, 1.0)
    up: tuple = (0.0, 1.0, 0.0)
    path: str = ""
    extra: dict = field(default_factory=dict)
    # Blocks listing the object (an object crossing blocks is listed in each of them).
    blocks: list = field(default_factory=list)
    # Where the placement is stored: ("record", raw record of the world file, offset of pos in it)
    # or ("bsd", offset of pos). dir and up follow pos when the object has them.
    places: list = field(default_factory=list)


def _string(data, offset):
    (length,) = struct.unpack_from("<i", data, offset)
    if length < 0 or offset + 4 + length > len(data):
        raise ValueError("texto inválido no deslocamento {:d}".format(offset))
    raw = bytes(data[offset + 4:offset + 4 + length])
    return raw.decode("gbk", errors="replace"), offset + 4 + length


def _struct_for(version, table):
    return next(s for limit, s in table if limit is None or version < limit)


class SceneFile:
    """A map scene read from the bytes of its ``.ecwld`` and ``.ecbsd`` files."""

    def __init__(self, world, bsd):
        self.world = bytearray(world)
        self.bsd = bytearray(bsd)
        (magic, self.version, self.world_width, self.world_length, self.block_size, self.block_grid,
         num_blocks, self.block_rows, self.block_cols, tree_offset, grass_offset, num_tree_types,
         num_grass_types, self.flags, _reserved) = _HEADER.unpack_from(world, 0)
        if magic != WORLD_MAGIC:
            raise ValueError("não é um arquivo de cena (.ecwld)")
        if not 4 <= self.version <= WORLD_VERSION_MAX:
            raise ValueError("versão {:d} do .ecwld não suportada".format(self.version))
        # The shared data file was written by AFile in binary mode: a "MOXB" tag comes first, and the
        # offsets the world file keeps into it count it.
        start = 4 if bytes(bsd[:4]) == b"MOXB" else 0
        (magic, self.bsd_version, num_sub_terrains, _reserved) = _BSD_HEADER.unpack_from(bsd, start)
        if magic != BSD_MAGIC:
            raise ValueError("não é um arquivo de dados de cena (.ecbsd)")
        if not 4 <= self.bsd_version <= BSD_VERSION_MAX:
            raise ValueError("versão {:d} do .ecbsd não suportada".format(self.bsd_version))
        # Files holding the grass and water data of each sub-terrain (relative to the map folder).
        self.sub_terrain_files = []
        offset = start + _BSD_HEADER.size
        if self.bsd_version >= 7:
            for _ in range(num_sub_terrains):
                name, offset = _string(bsd, offset)
                self.sub_terrain_files.append(name)

        self.tree_types = []
        offset = tree_offset
        for _ in range(num_tree_types):
            tree_file, offset = _string(world, offset)
            composite_map, offset = _string(world, offset)
            self.tree_types.append((tree_file, composite_map))
        self.grass_types = []
        offset = grass_offset
        for _ in range(num_grass_types):
            alpha_blend, alpha_ref, size, size_var, softness, sight_range = _GRASS_TYPE.unpack_from(world, offset)
            grass_file, offset = _string(world, offset + _GRASS_TYPE.size)
            self.grass_types.append(dict(file=grass_file, alpha_blend=alpha_blend, alpha_ref=alpha_ref, size=size,
                                         size_var=size_var, softness=softness, sight_range=sight_range))

        self.objects = []
        self._by_key = {}
        self.blocks = []
        self._tail = min(tree_offset, grass_offset)
        block_offsets = struct.unpack_from("<{:d}I".format(num_blocks), world, _HEADER.size)
        for block, block_offset in enumerate(block_offsets):
            if block_offset:
                self._read_block(block, block_offset)
        self._by_cell = {(b.row, b.col): b for b in self.blocks}

    # Reading.

    def _object(self, kind, export_id, block, place, **values):
        """The object of ``kind`` and ``export_id`` (objects listed by several blocks are one)."""
        key = (kind, export_id)
        ob = self._by_key.get(key) if kind != 'TREE' else None
        if ob is None:
            ob = SceneObject(kind=kind, export_id=export_id, **values)
            self.objects.append(ob)
            if kind != 'TREE':
                self._by_key[key] = ob
        if block not in ob.blocks:
            ob.blocks.append(block)
        # Records by identity: two records can hold the same bytes.
        if place is not None and not any(p == place if p[0] == "bsd" else (p[1] is place[1] and p[2:] == place[2:])
                                         for p in ob.places if p[0] == place[0]):
            ob.places.append(place)
        return ob

    def _record(self, block, kind, start, end):
        record = bytearray(self.world[start:end])
        block.sections[kind].append(record)
        return record

    def _read_block(self, block, offset):
        data = self.world
        (row, col, num_tree, num_water, num_grass, num_ornament, num_box, num_effect, num_model, num_critter,
         num_bezier, num_sound) = _BLOCK.unpack_from(data, offset)
        this = _Block(index=block, offset=offset, row=row, col=col, sections={kind: [] for kind in KINDS})
        self.blocks.append(this)
        if self.version < 7:
            num_bezier = num_sound = 0
            offset += 4 + 8 * 4
        elif self.version < 9:
            num_sound = 0
            offset += 4 + 9 * 4
        else:
            offset += _BLOCK.size

        if self.version < 5:
            raise ValueError("árvores do .ecwld versão < 5 não suportadas")
        for _ in range(num_tree):
            tree_type, x, y, z = _TREE.unpack_from(data, offset)
            record = self._record(this, 'TREE', offset, offset + _TREE.size)
            ob = self._object('TREE', 0, block, ("record", record, 4), pos=(x, y, z))
            ob.extra["type"] = tree_type
            ob.path = self.tree_types[tree_type][0] if tree_type < len(self.tree_types) else ""
            offset += _TREE.size

        refs = {}
        for kind, count in (('WATER', num_water), ('BUILDING', num_ornament), ('BOX_AREA', num_box),
                            ('GRASS', num_grass)):
            refs[kind] = [_REF.unpack_from(data, offset + i * _REF.size) for i in range(count)]
            for i in range(count):
                self._record(this, kind, offset + i * _REF.size, offset + (i + 1) * _REF.size)
            offset += count * _REF.size

        effect = _struct_for(self.version, _EFFECTS)
        for _ in range(num_effect):
            values = effect.unpack_from(data, offset)
            path, end = _string(data, offset + effect.size)
            record = self._record(this, 'EFFECT', offset, end)
            ob = self._object('EFFECT', values[0], block, ("record", record, 8), pos=values[2:5], dir=values[5:8],
                              up=values[8:11], path=path)
            ob.extra.update(scale=values[1], speed=values[11] if len(values) > 11 else 1.0,
                            valid_time=values[12] if len(values) > 12 else 2,
                            alpha=values[13] if len(values) > 13 else 1.0,
                            attenuation=bool(values[14]) if len(values) > 14 else True)
            offset = end

        model = _MODEL17 if self.version >= 17 else _MODEL
        for _ in range(num_model):
            values = model.unpack_from(data, offset)
            path, end = _string(data, offset + model.size)
            action, end = _string(data, end)
            record = self._record(this, 'MODEL', offset, end)
            ob = self._object('MODEL', values[0], block, ("record", record, 4), pos=values[1:4], dir=values[4:7],
                              up=values[7:10], path=path)
            ob.extra["action"] = action
            offset = end

        for kind, count in (('CRITTER', num_critter), ('BEZIER', num_bezier)):
            refs[kind] = [_REF.unpack_from(data, offset + i * _REF.size) for i in range(count)]
            for i in range(count):
                self._record(this, kind, offset + i * _REF.size, offset + (i + 1) * _REF.size)
            offset += count * _REF.size

        sound = _SOUND10 if self.version >= 10 else _SOUND
        for _ in range(num_sound):
            values = sound.unpack_from(data, offset)
            path, end = _string(data, offset + sound.size)
            record = self._record(this, 'SOUND', offset, end)
            ob = self._object('SOUND', values[0], block, ("record", record, 4), pos=values[1:4], path=path)
            ob.extra.update(min_dist=values[4], max_dist=values[5], group=values[6],
                            valid_time=values[7] if len(values) > 7 else 2)
            offset = end

        for kind, kind_refs in refs.items():
            for export_id, bsd_offset in kind_refs:
                if (kind, export_id) in self._by_key:
                    self._object(kind, export_id, block, None)
                    continue
                getattr(self, "_read_" + kind.lower())(export_id, block, bsd_offset)

    def _read_water(self, export_id, block, offset):
        ob = self._object('WATER', export_id, block, None)
        if self.bsd_version >= 7:
            ob.extra["sub_terrain"], ob.extra["data_index"] = _WATER.unpack_from(self.bsd, offset)

    def _read_building(self, export_id, block, offset):
        values = _ORNAMENT.unpack_from(self.bsd, offset)
        path, _end = _string(self.bsd, offset + _ORNAMENT.size)
        ob = self._object('BUILDING', export_id, block, ("bsd", offset), pos=values[0:3], dir=values[3:6],
                          up=values[6:9], path=path)
        ob.extra.update(reflect=bool(values[9]), refract=bool(values[10]))

    def _read_box_area(self, export_id, block, offset):
        size, num_strings = next((s, n) for limit, s, n in _BOX_AREAS if limit is None or self.bsd_version < limit)
        head = 4 if self.bsd_version < 6 else 8
        pos_ext_axes = _BOX_OBB.unpack_from(self.bsd, offset + head)
        end = offset + size
        names = []
        for _ in range(num_strings):
            name, end = _string(self.bsd, end)
            names.append(name)
        ob = self._object('BOX_AREA', export_id, block, ("bsd", offset + head), pos=pos_ext_axes[0:3],
                          dir=pos_ext_axes[12:15], up=pos_ext_axes[9:12])
        (default,) = struct.unpack_from("<i", self.bsd, offset)
        # Sky textures and the music: up to 16, the strings are three day skies, three night skies and
        # the music; the 20 strings of 17 and 18 have no source.
        ob.extra.update(default=bool(default), extents=pos_ext_axes[3:6], axis_x=pos_ext_axes[6:9], strings=names)

    def _read_grass(self, export_id, block, offset):
        grass = _GRASS7 if self.bsd_version >= 7 else _GRASS
        values = grass.unpack_from(self.bsd, offset)
        ob = self._object('GRASS', export_id, block, ("bsd", offset + 4), pos=values[1:4])
        ob.extra.update(type=values[0], grid_size=values[4], grid_cols=values[5], grid_rows=values[6])
        if values[0] < len(self.grass_types):
            ob.path = self.grass_types[values[0]]["file"]

    def _read_critter(self, export_id, block, offset):
        critter = _CRITTER5 if self.bsd_version >= 5 else _CRITTER
        values = critter.unpack_from(self.bsd, offset)
        path, _end = _string(self.bsd, offset + critter.size)
        ob = self._object('CRITTER', export_id, block, ("bsd", offset + 4), pos=values[1:4], path=path)
        ob.extra.update(type=values[0], radius=values[4], count=values[5], behaviour=values[6])

    def _read_bezier(self, export_id, block, offset):
        data = self.bsd
        version, object_id = struct.unpack_from("<Ii", data, offset)
        offset += 8
        if version >= 2:
            offset += 8
        action = ""
        if version >= 3:
            action, offset = _string(data, offset)
        (num_points,) = struct.unpack_from("<i", data, offset)
        offset += 4
        points = [struct.unpack_from("<3f", data, offset + i * 24) for i in range(num_points)]
        ob = self._object('BEZIER', export_id, block, None, pos=points[0] if points else (0.0, 0.0, 0.0))
        ob.extra.update(object_id=object_id, action=action, points=points)

    # Writing.

    def block_at(self, pos):
        """The block whose square holds an engine position, or None."""
        col = int((pos[0] + self.world_width / 2.0) // self.block_size)
        row = int((self.world_length / 2.0 - pos[2]) // self.block_size)
        return self._by_cell.get((row, col))

    def _block_of(self, record, kind):
        return next((b for b in self.blocks if any(r is record for r in b.sections[kind])), None)

    def move(self, ob, pos=None, dir=None, up=None):
        """
        Store a new place / turn of ``ob`` (in every place the files keep it). An inline object
        listed by one block moves to the block it now lies in.
        """
        for place in ob.places:
            data, offset = (place[1], place[2]) if place[0] == "record" else (self.bsd, place[1])
            if pos is not None:
                _VEC3.pack_into(data, offset, *pos)
            if ob.kind in ('BUILDING', 'EFFECT', 'MODEL'):
                if dir is not None:
                    _VEC3.pack_into(data, offset + 12, *dir)
                if up is not None:
                    _VEC3.pack_into(data, offset + 24, *up)
        if pos is not None:
            ob.pos = tuple(pos)
            if ob.kind in INLINE and len(ob.places) == 1:
                record = ob.places[0][1]
                old, new = self._block_of(record, ob.kind), self.block_at(pos)
                if old is not None and new is not None and new is not old:
                    old.sections[ob.kind] = [r for r in old.sections[ob.kind] if r is not record]
                    new.sections[ob.kind].append(record)
                    ob.blocks = [new.index]
        if dir is not None:
            ob.dir = tuple(dir)
        if up is not None:
            ob.up = tuple(up)

    def remove(self, ob):
        """Take an effect, scene model or sound out of the scene."""
        if ob.kind not in EDITABLE:
            raise ValueError("só efeitos, modelos e sons podem ser apagados")
        for place in ob.places:
            for block in self.blocks:
                block.sections[ob.kind] = [r for r in block.sections[ob.kind] if r is not place[1]]
        self.objects = [o for o in self.objects if o is not ob]
        self._by_key.pop((ob.kind, ob.export_id), None)

    def next_export_id(self):
        return max([0] + [o.export_id for o in self.objects]) + 1

    def add_copy(self, source, pos, dir=None, up=None, export_id=0):
        """
        A new effect, scene model or sound made from ``source`` (same file and settings) at ``pos``,
        with a new export id (or ``export_id``). Returns the new object.
        """
        if source.kind not in EDITABLE or not source.places:
            raise ValueError("só efeitos, modelos e sons podem ser copiados")
        export_id = export_id or self.next_export_id()
        if (source.kind, export_id) in self._by_key:
            raise ValueError("o id {:d} já está em uso na cena".format(export_id))
        record = bytearray(source.places[0][1])
        struct.pack_into("<I", record, 0, export_id)
        block = self.block_at(pos) or self._block_of(source.places[0][1], source.kind)
        block.sections[source.kind].append(record)
        ob = SceneObject(kind=source.kind, export_id=export_id, pos=source.pos, dir=source.dir, up=source.up,
                         path=source.path, extra=dict(source.extra), blocks=[block.index],
                         places=[("record", record, INLINE[source.kind])])
        self.objects.append(ob)
        self._by_key[(ob.kind, export_id)] = ob
        self.move(ob, pos=pos, dir=dir, up=up)
        return ob

    def world_bytes(self):
        """The world file laid out again: same header, same block order, then the type tables."""
        num_blocks = struct.unpack_from("<i", self.world, 24)[0]
        out = bytearray(self.world[:_HEADER.size + 4 * num_blocks])
        offsets = [0] * num_blocks
        for block in sorted(self.blocks, key=lambda b: b.offset):
            offsets[block.index] = len(out)
            counts = [len(block.sections[kind]) for kind in _HEADER_ORDER]
            if self.version < 7:
                out += struct.pack("<HH8i", block.row, block.col, *counts[:8])
            elif self.version < 9:
                out += struct.pack("<HH9i", block.row, block.col, *counts[:9])
            else:
                out += struct.pack("<HH10i", block.row, block.col, *counts)
            for kind in _DATA_ORDER:
                for record in block.sections[kind]:
                    out += record
        shift = len(out) - self._tail
        out += self.world[self._tail:]
        tree_offset, grass_offset = struct.unpack_from("<II", self.world, 36)
        struct.pack_into("<II", out, 36, tree_offset + shift, grass_offset + shift)
        struct.pack_into("<{:d}I".format(num_blocks), out, _HEADER.size, *offsets)
        return bytes(out)

    def bsd_bytes(self):
        return bytes(self.bsd)

    def water_area(self, ob, data_files):
        """
        The water of a ``WATER`` object (``A3DWaterArea::Load``), read from its sub-terrain data file
        (``data_files``: file name of :attr:`sub_terrain_files` -> bytes): a dict with ``height``,
        ``color`` (0xAARRGGBB), ``center`` (engine), ``cell``, ``columns``, ``rows`` and ``cells``
        (one byte per cell, row 0 north, bit 1 set where there is water). None when unknown.
        """
        sub = ob.extra.get("sub_terrain", -1)
        if not 0 <= sub < len(self.sub_terrain_files):
            return None
        data = data_files.get(self.sub_terrain_files[sub])
        if not data or len(data) < 12:
            return None
        # Offsets of the data blocks, at the end of the file (the count last).
        (count,) = struct.unpack_from("<I", data, len(data) - 4)
        index = ob.extra.get("data_index", -1)
        if not 0 <= index < count or 4 + 4 * count > len(data):
            return None
        (offset,) = struct.unpack_from("<I", data, len(data) - 4 - 4 * count + 4 * index)
        (version,) = struct.unpack_from("<I", data, offset)
        offset += 4
        if version >= 0x10002000:
            _id, color, height, _edge, columns, rows, cell = struct.unpack_from("<IIffiif", data, offset)
            offset += 28
        elif version == 0x10001000:
            _id, color, height, columns, rows, cell = struct.unpack_from("<IIfiif", data, offset)
            offset += 24
        elif version == 0x10000000:
            _id, height, columns, rows, cell = struct.unpack_from("<Ifiif", data, offset)
            color = 0xFFCCCCCC
            offset += 20
        else:
            return None
        center = struct.unpack_from("<3f", data, offset)
        offset += 12
        if version >= 0x10003000:
            offset += 1  # Minor water flag.
        if columns <= 0 or rows <= 0 or offset + columns * rows > len(data):
            return None
        return dict(height=height, color=color, center=center, cell=cell, columns=columns, rows=rows,
                    cells=bytes(data[offset:offset + columns * rows]))
