/* SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup angelica
 *
 * `npcgen.data` layout (`npcgendata.h` / `CNPCGenMan::Load` in the server), `#pragma pack(1)`,
 * little-endian, `size_t` and `bool` are 4 and 1 bytes:
 *
 *   u32 version (1..11)
 *   header      i32 num_areas, i32 num_res_areas
 *               + i32 num_dyn_objects (v >= 6) + i32 num_controllers (v >= 7)
 *   num_areas * AREA
 *     i32 type, i32 num_spawns, f32 pos[3], f32 dir[3], f32 exts[3], i32 npc_type, i32 group_type,
 *     u8 init_gen, u8 auto_revive, u8 valid_once, u32 gen_id                       (59 bytes)
 *     + i32 controller_id, i32 life_time, i32 max_num (v >= 7)                      (71 bytes)
 *     followed by num_spawns * SPAWN
 *       u32 id, u32 count, i32 refresh, u32 died_times, u32 aggressive, f32 offset_water,
 *       f32 offset_terrain, u32 faction, u32 faction_helper, u32 faction_accept, u8 need_help,
 *       u8 def_faction, u8 def_fac_helper, u8 def_fac_accept, i32 path_id, i32 loop_type,
 *       i32 speed_flag, i32 dead_time                                               (60 bytes)
 *       + i32 refresh_lower (v >= 11)                                               (64 bytes)
 *   num_res_areas * RESAREA
 *     f32 pos[3], f32 ext_x, f32 ext_z, i32 num_res, u8 init_gen, u8 auto_revive, u8 valid_once,
 *     u32 gen_id                                                                    (31 bytes)
 *     + u8 dir[2], u8 rad (v >= 6)                                                  (34 bytes)
 *     + i32 controller_id, i32 max_num (v >= 7)                                     (42 bytes)
 *     followed by num_res * { i32 type, i32 template_id, u32 refresh, u32 count, f32 height_off }
 *   num_dyn_objects * DYNOBJ
 *     u32 id, f32 pos[3], u8 dir[2], u8 rad                                         (19 bytes)
 *     + u32 controller_id (v >= 9) + u8 scale (v >= 10)                          (23 / 24 bytes)
 *   num_controllers * CONTROLLER
 *     u32 id, i32 controller_id, char name[128], u8 activated, i32 wait_time, i32 stop_time,
 *     u8 active_time_invalid, u8 stop_time_invalid, 2 * {i32 year, month, week, day, hours,
 *     minutes}                                                                     (195 bytes)
 *     + i32 active_time_range (v >= 8)                                             (199 bytes)
 */

#include "angelica_npcgen.hh"

#include <cstring>

#include "angelica_elements.hh"

namespace blender::io::angelica {

namespace {

void set_error(std::string *r_error, std::string message)
{
  if (r_error) {
    *r_error = std::move(message);
  }
}

size_t area_size(const uint32_t v)
{
  return v >= 7 ? 71 : 59;
}
size_t spawn_size(const uint32_t v)
{
  return v >= 11 ? 64 : 60;
}
size_t res_area_size(const uint32_t v)
{
  return v >= 7 ? 42 : (v >= 6 ? 34 : 31);
}
constexpr size_t RESOURCE_SIZE = 20;
size_t dyn_object_size(const uint32_t v)
{
  return v >= 10 ? 24 : (v >= 9 ? 23 : 19);
}
size_t controller_size(const uint32_t v)
{
  return v >= 8 ? 199 : 195;
}

/** Validate a record count against the bytes left, so garbage counts fail before allocating. */
bool check_count(const BinaryReader &r, const int32_t count, const size_t min_size)
{
  return count >= 0 && uint64_t(count) * min_size <= r.remaining();
}

void read_vec3(BinaryReader &r, float v[3])
{
  for (int i = 0; i < 3; i++) {
    v[i] = r.f32();
  }
}

void read_time(BinaryReader &r, NpcGenCtrlTime &t)
{
  t.year = r.i32();
  t.month = r.i32();
  t.week = r.i32();
  t.day = r.i32();
  t.hours = r.i32();
  t.minutes = r.i32();
}

class Writer {
 public:
  Bytes &out;
  template<typename T> void put(const T v)
  {
    const size_t at = out.size();
    out.resize(at + sizeof(T));
    std::memcpy(out.data() + at, &v, sizeof(T));
  }
  void vec3(const float v[3])
  {
    put(v[0]);
    put(v[1]);
    put(v[2]);
  }
  void time(const NpcGenCtrlTime &t)
  {
    put(t.year);
    put(t.month);
    put(t.week);
    put(t.day);
    put(t.hours);
    put(t.minutes);
  }
};

}  // namespace

std::string NpcGenController::name() const
{
  size_t len = 0;
  while (len < name_raw.size() && name_raw[len] != 0) {
    len++;
  }
  return gbk_to_utf8(std::string_view(reinterpret_cast<const char *>(name_raw.data()), len));
}

bool NpcGenController::set_name(std::string_view utf8, std::string *r_error)
{
  std::string gbk;
  if (!utf8_to_gbk(utf8, gbk)) {
    set_error(r_error, "controller name cannot be encoded as GBK");
    return false;
  }
  if (gbk.size() >= name_raw.size()) {
    set_error(r_error, "controller name is longer than 127 bytes");
    return false;
  }
  name_raw.fill(0);
  std::memcpy(name_raw.data(), gbk.data(), gbk.size());
  return true;
}

size_t NpcGenData::spawn_count() const
{
  size_t n = 0;
  for (const NpcGenArea &area : areas) {
    n += area.spawns.size();
  }
  return n;
}

size_t NpcGenData::resource_count() const
{
  size_t n = 0;
  for (const NpcGenResArea &area : resource_areas) {
    n += area.resources.size();
  }
  return n;
}

const NpcGenController *NpcGenData::find_controller(const uint32_t id) const
{
  for (const NpcGenController &c : controllers) {
    if (c.id == id) {
      return &c;
    }
  }
  return nullptr;
}

bool parse_npcgen(ByteSpan data, NpcGenData &r_data, std::string *r_error)
{
  r_data = NpcGenData();
  BinaryReader r(data);
  const uint32_t v = r.u32();
  if (r.failed() || v == 0 || v > NPCGEN_VERSION_MAX) {
    set_error(r_error,
              r.failed() ? "npcgen: file too small" :
                           "npcgen: unsupported version " + std::to_string(v));
    return false;
  }
  r_data.version = v;
  const int32_t num_areas = r.i32();
  const int32_t num_res_areas = r.i32();
  const int32_t num_dyn = v >= 6 ? r.i32() : 0;
  const int32_t num_ctrl = v >= 7 ? r.i32() : 0;
  if (r.failed()) {
    set_error(r_error, "npcgen: truncated header");
    return false;
  }

  if (!check_count(r, num_areas, area_size(v))) {
    set_error(r_error, "npcgen: invalid area count " + std::to_string(num_areas));
    return false;
  }
  r_data.areas.resize(size_t(num_areas));
  for (int32_t i = 0; i < num_areas; i++) {
    NpcGenArea &a = r_data.areas[size_t(i)];
    a.type = r.i32();
    const int32_t num_spawns = r.i32();
    read_vec3(r, a.pos);
    read_vec3(r, a.dir);
    read_vec3(r, a.extents);
    a.npc_type = r.i32();
    a.group_type = r.i32();
    a.init_gen = r.u8();
    a.auto_revive = r.u8();
    a.valid_once = r.u8();
    a.gen_id = r.u32();
    if (v >= 7) {
      a.controller_id = r.i32();
      a.life_time = r.i32();
      a.max_num = r.i32();
    }
    else {
      a.controller_id = a.life_time = a.max_num = 0;
    }
    if (r.failed() || !check_count(r, num_spawns, spawn_size(v))) {
      set_error(r_error, "npcgen: truncated or invalid area " + std::to_string(i));
      return false;
    }
    a.spawns.resize(size_t(num_spawns));
    for (NpcGenSpawn &s : a.spawns) {
      s.id = r.u32();
      s.count = r.u32();
      s.refresh = r.i32();
      s.died_times = r.u32();
      s.aggressive = r.u32();
      s.offset_water = r.f32();
      s.offset_terrain = r.f32();
      s.faction = r.u32();
      s.faction_helper = r.u32();
      s.faction_accept = r.u32();
      s.need_help = r.u8();
      s.default_faction = r.u8();
      s.default_faction_helper = r.u8();
      s.default_faction_accept = r.u8();
      s.path_id = r.i32();
      s.loop_type = r.i32();
      s.speed_flag = r.i32();
      s.dead_time = r.i32();
      s.refresh_lower = v >= 11 ? r.i32() : 0;
    }
    if (r.failed()) {
      set_error(r_error, "npcgen: truncated spawns of area " + std::to_string(i));
      return false;
    }
  }

  if (!check_count(r, num_res_areas, res_area_size(v))) {
    set_error(r_error, "npcgen: invalid resource area count " + std::to_string(num_res_areas));
    return false;
  }
  r_data.resource_areas.resize(size_t(num_res_areas));
  for (int32_t i = 0; i < num_res_areas; i++) {
    NpcGenResArea &a = r_data.resource_areas[size_t(i)];
    read_vec3(r, a.pos);
    a.extent_x = r.f32();
    a.extent_z = r.f32();
    const int32_t num_res = r.i32();
    a.init_gen = r.u8();
    a.auto_revive = r.u8();
    a.valid_once = r.u8();
    a.gen_id = r.u32();
    if (v >= 6) {
      a.dir[0] = r.u8();
      a.dir[1] = r.u8();
      a.rad = r.u8();
    }
    if (v >= 7) {
      a.controller_id = r.i32();
      a.max_num = r.i32();
    }
    if (r.failed() || !check_count(r, num_res, RESOURCE_SIZE)) {
      set_error(r_error, "npcgen: truncated or invalid resource area " + std::to_string(i));
      return false;
    }
    a.resources.resize(size_t(num_res));
    for (NpcGenResource &res : a.resources) {
      res.type = r.i32();
      res.template_id = r.i32();
      res.refresh_time = r.u32();
      res.count = r.u32();
      res.height_offset = r.f32();
    }
  }

  if (!check_count(r, num_dyn, dyn_object_size(v))) {
    set_error(r_error, "npcgen: invalid dynamic object count " + std::to_string(num_dyn));
    return false;
  }
  r_data.dynamic_objects.resize(size_t(num_dyn));
  for (NpcGenDynObject &o : r_data.dynamic_objects) {
    o.id = r.u32();
    read_vec3(r, o.pos);
    o.dir[0] = r.u8();
    o.dir[1] = r.u8();
    o.rad = r.u8();
    o.controller_id = v >= 9 ? r.u32() : 0;
    o.scale = v >= 10 ? r.u8() : 16;
  }

  if (!check_count(r, num_ctrl, controller_size(v))) {
    set_error(r_error, "npcgen: invalid controller count " + std::to_string(num_ctrl));
    return false;
  }
  r_data.controllers.resize(size_t(num_ctrl));
  for (NpcGenController &c : r_data.controllers) {
    c.id = r.u32();
    c.controller_id = r.i32();
    r.read_raw(c.name_raw.data(), c.name_raw.size());
    c.activated = r.u8();
    c.wait_time = r.i32();
    c.stop_time = r.i32();
    c.active_time_invalid = r.u8();
    c.stop_time_invalid = r.u8();
    read_time(r, c.active_time);
    read_time(r, c.stop_time_point);
    c.active_time_range = v >= 8 ? r.i32() : 0;
  }
  if (r.failed()) {
    set_error(r_error, "npcgen: truncated file");
    return false;
  }
  const ByteSpan rest = r.read_span(r.remaining());
  r_data.trailing.assign(rest.begin(), rest.end());
  return true;
}

bool write_npcgen(const NpcGenData &data, Bytes &r_out, std::string *r_error)
{
  r_out.clear();
  const uint32_t v = data.version;
  if (v == 0 || v > NPCGEN_VERSION_MAX) {
    set_error(r_error, "npcgen: cannot write version " + std::to_string(v));
    return false;
  }
  if (v < 6 && !data.dynamic_objects.empty()) {
    set_error(r_error, "npcgen: dynamic objects need version 6 or later");
    return false;
  }
  if (v < 7 && !data.controllers.empty()) {
    set_error(r_error, "npcgen: controllers need version 7 or later");
    return false;
  }
  constexpr size_t max_count = size_t(INT32_MAX);
  if (data.areas.size() > max_count || data.resource_areas.size() > max_count ||
      data.dynamic_objects.size() > max_count || data.controllers.size() > max_count)
  {
    set_error(r_error, "npcgen: too many records");
    return false;
  }

  Writer w{r_out};
  w.put(v);
  w.put(int32_t(data.areas.size()));
  w.put(int32_t(data.resource_areas.size()));
  if (v >= 6) {
    w.put(int32_t(data.dynamic_objects.size()));
  }
  if (v >= 7) {
    w.put(int32_t(data.controllers.size()));
  }

  for (const NpcGenArea &a : data.areas) {
    if (a.spawns.size() > max_count) {
      set_error(r_error, "npcgen: too many spawns in an area");
      return false;
    }
    w.put(a.type);
    w.put(int32_t(a.spawns.size()));
    w.vec3(a.pos);
    w.vec3(a.dir);
    w.vec3(a.extents);
    w.put(a.npc_type);
    w.put(a.group_type);
    w.put(a.init_gen);
    w.put(a.auto_revive);
    w.put(a.valid_once);
    w.put(a.gen_id);
    if (v >= 7) {
      w.put(a.controller_id);
      w.put(a.life_time);
      w.put(a.max_num);
    }
    for (const NpcGenSpawn &s : a.spawns) {
      w.put(s.id);
      w.put(s.count);
      w.put(s.refresh);
      w.put(s.died_times);
      w.put(s.aggressive);
      w.put(s.offset_water);
      w.put(s.offset_terrain);
      w.put(s.faction);
      w.put(s.faction_helper);
      w.put(s.faction_accept);
      w.put(s.need_help);
      w.put(s.default_faction);
      w.put(s.default_faction_helper);
      w.put(s.default_faction_accept);
      w.put(s.path_id);
      w.put(s.loop_type);
      w.put(s.speed_flag);
      w.put(s.dead_time);
      if (v >= 11) {
        w.put(s.refresh_lower);
      }
    }
  }

  for (const NpcGenResArea &a : data.resource_areas) {
    if (a.resources.size() > max_count) {
      set_error(r_error, "npcgen: too many resources in an area");
      return false;
    }
    w.vec3(a.pos);
    w.put(a.extent_x);
    w.put(a.extent_z);
    w.put(int32_t(a.resources.size()));
    w.put(a.init_gen);
    w.put(a.auto_revive);
    w.put(a.valid_once);
    w.put(a.gen_id);
    if (v >= 6) {
      w.put(a.dir[0]);
      w.put(a.dir[1]);
      w.put(a.rad);
    }
    if (v >= 7) {
      w.put(a.controller_id);
      w.put(a.max_num);
    }
    for (const NpcGenResource &res : a.resources) {
      w.put(res.type);
      w.put(res.template_id);
      w.put(res.refresh_time);
      w.put(res.count);
      w.put(res.height_offset);
    }
  }

  for (const NpcGenDynObject &o : data.dynamic_objects) {
    w.put(o.id);
    w.vec3(o.pos);
    w.put(o.dir[0]);
    w.put(o.dir[1]);
    w.put(o.rad);
    if (v >= 9) {
      w.put(o.controller_id);
    }
    if (v >= 10) {
      w.put(o.scale);
    }
  }

  for (const NpcGenController &c : data.controllers) {
    w.put(c.id);
    w.put(c.controller_id);
    r_out.insert(r_out.end(), c.name_raw.begin(), c.name_raw.end());
    w.put(c.activated);
    w.put(c.wait_time);
    w.put(c.stop_time);
    w.put(c.active_time_invalid);
    w.put(c.stop_time_invalid);
    w.time(c.active_time);
    w.time(c.stop_time_point);
    if (v >= 8) {
      w.put(c.active_time_range);
    }
  }

  r_out.insert(r_out.end(), data.trailing.begin(), data.trailing.end());
  return true;
}

}  // namespace blender::io::angelica
