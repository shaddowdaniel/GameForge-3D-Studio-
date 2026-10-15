# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
The spawn file of a map on the server (``config\\<map>\\npcgen.data``): NPC and monster spawn
areas, resource (mine and herb) areas, dynamic objects and the controllers that switch them on
and off. The layout is the one of ``angelica_npcgen.cc`` (``npcgendata.h``, ``#pragma pack(1)``,
versions 1 to 11); every field is kept, so reading and writing gives the same bytes.
"""

__all__ = (
    "Area",
    "Controller",
    "DynamicObject",
    "NpcGen",
    "ResourceArea",
    "Spawn",
)

import struct
from dataclasses import dataclass, field

_AREA = struct.Struct("<ii3f3f3fiiBBBI")
_AREA_V7 = struct.Struct("<iii")
_SPAWN = struct.Struct("<IIiIIffIIIBBBBiiii")
_SPAWN_V11 = struct.Struct("<i")
_RES_AREA = struct.Struct("<3fffiBBBI")
_RES_AREA_V6 = struct.Struct("<BBB")
_RES_AREA_V7 = struct.Struct("<ii")
_RESOURCE = struct.Struct("<iiIIf")
_DYN = struct.Struct("<I3fBBB")
_DYN_V9 = struct.Struct("<I")
_DYN_V10 = struct.Struct("<B")
_CONTROLLER = struct.Struct("<Ii128sBiiBB6i6i")
_CONTROLLER_V8 = struct.Struct("<i")
VERSION_MAX = 11


@dataclass
class Spawn:
    """One NPC or monster template of an area, spawned ``count`` times."""
    id: int = 0
    count: int = 1
    refresh: int = 0
    died_times: int = 0
    aggressive: int = 0
    offset_water: float = 0.0
    offset_terrain: float = 0.0
    faction: int = 0
    faction_helper: int = 0
    faction_accept: int = 0
    need_help: int = 0
    default_faction: int = 0
    default_faction_helper: int = 0
    default_faction_accept: int = 0
    path_id: int = 0
    loop_type: int = 0
    speed_flag: int = 0
    dead_time: int = 0
    refresh_lower: int = 0


@dataclass
class Area:
    """A spawn area: ``type`` 0 on the terrain, 1 a box; ``npc_type`` 0 monsters, 1 service NPCs."""
    type: int = 0
    pos: tuple = (0.0, 0.0, 0.0)
    dir: tuple = (0.0, 0.0, 1.0)
    extents: tuple = (0.0, 0.0, 0.0)
    npc_type: int = 0
    group_type: int = 0
    init_gen: int = 1
    auto_revive: int = 1
    valid_once: int = 0
    gen_id: int = 0
    controller_id: int = 0
    life_time: int = 0
    max_num: int = 0
    spawns: list = field(default_factory=list)


@dataclass
class Resource:
    type: int = 0
    template_id: int = 0
    refresh_time: int = 0
    count: int = 1
    height_offset: float = 0.0


@dataclass
class ResourceArea:
    pos: tuple = (0.0, 0.0, 0.0)
    extent_x: float = 0.0
    extent_z: float = 0.0
    init_gen: int = 1
    auto_revive: int = 1
    valid_once: int = 0
    gen_id: int = 0
    dir: tuple = (0, 0)
    rad: int = 0
    controller_id: int = 0
    max_num: int = 0
    resources: list = field(default_factory=list)


@dataclass
class DynamicObject:
    id: int = 0
    pos: tuple = (0.0, 0.0, 0.0)
    dir: tuple = (0, 0)
    rad: int = 0
    controller_id: int = 0
    scale: int = 16


@dataclass
class Controller:
    id: int = 0
    controller_id: int = 0
    name_raw: bytes = bytes(128)
    activated: int = 0
    wait_time: int = 0
    stop_time: int = 0
    active_time_invalid: int = 1
    stop_time_invalid: int = 1
    active_time: tuple = (-1, -1, -1, -1, -1, 0)
    stop_time_point: tuple = (-1, -1, -1, -1, -1, 0)
    active_time_range: int = 0

    @property
    def name(self):
        return self.name_raw.split(b"\0", 1)[0].decode("gbk", "replace")


class NpcGen:
    """A whole ``npcgen.data``."""

    def __init__(self, data=None):
        self.version = VERSION_MAX
        self.areas = []
        self.resource_areas = []
        self.dynamic_objects = []
        self.controllers = []
        self.trailing = b""
        if data is not None:
            self._read(bytes(data))

    def _read(self, data):
        pos = 0

        def take(fmt):
            nonlocal pos
            values = fmt.unpack_from(data, pos)
            pos += fmt.size
            return values

        (self.version,) = struct.unpack_from("<I", data, 0)
        pos = 4
        if not 1 <= self.version <= VERSION_MAX:
            raise ValueError("npcgen.data: versão {:d} não suportada".format(self.version))
        num_areas, num_res = struct.unpack_from("<ii", data, pos)
        pos += 8
        num_dyn = num_ctrl = 0
        if self.version >= 6:
            (num_dyn,) = struct.unpack_from("<i", data, pos)
            pos += 4
        if self.version >= 7:
            (num_ctrl,) = struct.unpack_from("<i", data, pos)
            pos += 4
        v = self.version
        for _ in range(num_areas):
            a = take(_AREA)
            area = Area(type=a[0], pos=a[2:5], dir=a[5:8], extents=a[8:11], npc_type=a[11], group_type=a[12],
                        init_gen=a[13], auto_revive=a[14], valid_once=a[15], gen_id=a[16])
            if v >= 7:
                area.controller_id, area.life_time, area.max_num = take(_AREA_V7)
            for _ in range(a[1]):
                spawn = Spawn(*take(_SPAWN))
                if v >= 11:
                    (spawn.refresh_lower,) = take(_SPAWN_V11)
                area.spawns.append(spawn)
            self.areas.append(area)
        for _ in range(num_res):
            r = take(_RES_AREA)
            area = ResourceArea(pos=r[0:3], extent_x=r[3], extent_z=r[4], init_gen=r[6], auto_revive=r[7],
                                valid_once=r[8], gen_id=r[9])
            if v >= 6:
                d0, d1, area.rad = take(_RES_AREA_V6)
                area.dir = (d0, d1)
            if v >= 7:
                area.controller_id, area.max_num = take(_RES_AREA_V7)
            area.resources = [Resource(*take(_RESOURCE)) for _ in range(r[5])]
            self.resource_areas.append(area)
        for _ in range(num_dyn):
            d = take(_DYN)
            obj = DynamicObject(id=d[0], pos=d[1:4], dir=(d[4], d[5]), rad=d[6])
            if v >= 9:
                (obj.controller_id,) = take(_DYN_V9)
            if v >= 10:
                (obj.scale,) = take(_DYN_V10)
            self.dynamic_objects.append(obj)
        for _ in range(num_ctrl):
            c = take(_CONTROLLER)
            ctrl = Controller(id=c[0], controller_id=c[1], name_raw=c[2], activated=c[3], wait_time=c[4],
                              stop_time=c[5], active_time_invalid=c[6], stop_time_invalid=c[7],
                              active_time=c[8:14], stop_time_point=c[14:20])
            if v >= 8:
                (ctrl.active_time_range,) = take(_CONTROLLER_V8)
            self.controllers.append(ctrl)
        self.trailing = data[pos:]

    def to_bytes(self):
        v = self.version
        if self.dynamic_objects and v < 6:
            raise ValueError("npcgen.data versão {:d} não guarda objetos dinâmicos".format(v))
        if self.controllers and v < 7:
            raise ValueError("npcgen.data versão {:d} não guarda controladores".format(v))
        out = bytearray(struct.pack("<Iii", v, len(self.areas), len(self.resource_areas)))
        if v >= 6:
            out += struct.pack("<i", len(self.dynamic_objects))
        if v >= 7:
            out += struct.pack("<i", len(self.controllers))
        for a in self.areas:
            out += _AREA.pack(a.type, len(a.spawns), *a.pos, *a.dir, *a.extents, a.npc_type, a.group_type,
                              a.init_gen, a.auto_revive, a.valid_once, a.gen_id)
            if v >= 7:
                out += _AREA_V7.pack(a.controller_id, a.life_time, a.max_num)
            for s in a.spawns:
                out += _SPAWN.pack(s.id, s.count, s.refresh, s.died_times, s.aggressive, s.offset_water,
                                   s.offset_terrain, s.faction, s.faction_helper, s.faction_accept, s.need_help,
                                   s.default_faction, s.default_faction_helper, s.default_faction_accept,
                                   s.path_id, s.loop_type, s.speed_flag, s.dead_time)
                if v >= 11:
                    out += _SPAWN_V11.pack(s.refresh_lower)
        for r in self.resource_areas:
            out += _RES_AREA.pack(*r.pos, r.extent_x, r.extent_z, len(r.resources), r.init_gen, r.auto_revive,
                                  r.valid_once, r.gen_id)
            if v >= 6:
                out += _RES_AREA_V6.pack(r.dir[0], r.dir[1], r.rad)
            if v >= 7:
                out += _RES_AREA_V7.pack(r.controller_id, r.max_num)
            for res in r.resources:
                out += _RESOURCE.pack(res.type, res.template_id, res.refresh_time, res.count, res.height_offset)
        for d in self.dynamic_objects:
            out += _DYN.pack(d.id, *d.pos, d.dir[0], d.dir[1], d.rad)
            if v >= 9:
                out += _DYN_V9.pack(d.controller_id)
            if v >= 10:
                out += _DYN_V10.pack(d.scale)
        for c in self.controllers:
            out += _CONTROLLER.pack(c.id, c.controller_id, c.name_raw, c.activated, c.wait_time, c.stop_time,
                                    c.active_time_invalid, c.stop_time_invalid, *c.active_time, *c.stop_time_point)
            if v >= 8:
                out += _CONTROLLER_V8.pack(c.active_time_range)
        return bytes(out + self.trailing)
