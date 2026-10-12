/* SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup angelica
 *
 * Reader and writer for the server spawn file `npcgen.data` (`config/<map>/npcgen.data`, loaded by
 * `CNPCGenMan::Load` in the game server): NPC / monster spawn areas, resource (mine / herb) areas,
 * dynamic objects and the controllers (triggers) that switch spawns on and off.
 *
 * File versions 1 to 11 (`AIGENFILE_VERSION`) are supported. Every on-disk field is kept,
 * including flag bytes verbatim, so #parse_npcgen followed by #write_npcgen is byte identical.
 * See `angelica_npcgen.cc` for the layout.
 */

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "angelica_reader.hh"

namespace blender::io::angelica {

/** Newest version written by the engine tools (`AIGENFILE_VERSION`). */
constexpr uint32_t NPCGEN_VERSION_MAX = 11;

/** One generator of an area: an NPC or monster template spawned `count` times. */
struct NpcGenSpawn {
  /** Template id in `elements.data` (MONSTER_ESSENCE or NPC_ESSENCE). */
  uint32_t id = 0;
  uint32_t count = 0;
  /** Respawn time in seconds (upper bound since version 11). */
  int32_t refresh = 0;
  uint32_t died_times = 0;
  uint32_t aggressive = 0;
  float offset_water = 0.0f;
  float offset_terrain = 0.0f;
  uint32_t faction = 0;
  uint32_t faction_helper = 0;
  uint32_t faction_accept = 0;
  /* Flag bytes are kept as bytes so unusual values round-trip. */
  uint8_t need_help = 0;
  uint8_t default_faction = 0;
  uint8_t default_faction_helper = 0;
  uint8_t default_faction_accept = 0;
  int32_t path_id = 0;
  /** 0 no loop, 1 back and forth, 2 restart from the first point. */
  int32_t loop_type = 0;
  /** 1 run, 0 walk. */
  int32_t speed_flag = 0;
  /** Seconds the corpse stays. */
  int32_t dead_time = 0;
  /** Respawn time lower bound, version 11 and later. */
  int32_t refresh_lower = 0;
};

enum class NpcGenAreaType : int32_t { OnTerrain = 0, Box = 1 };
enum class NpcGenNpcType : int32_t { Monster = 0, Server = 1 };

struct NpcGenArea {
  /** #NpcGenAreaType: 0 on terrain, 1 box. */
  int32_t type = 0;
  float pos[3] = {0.0f, 0.0f, 0.0f};
  float dir[3] = {0.0f, 0.0f, 1.0f};
  /** Half extents of the area. */
  float extents[3] = {0.0f, 0.0f, 0.0f};
  /** #NpcGenNpcType: 0 monsters, 1 service NPCs. */
  int32_t npc_type = 0;
  int32_t group_type = 0;
  uint8_t init_gen = 1;
  uint8_t auto_revive = 1;
  uint8_t valid_once = 0;
  uint32_t gen_id = 0;
  /* Version 7 and later. */
  int32_t controller_id = 0;
  int32_t life_time = 0;
  int32_t max_num = 0;
  std::vector<NpcGenSpawn> spawns;
};

struct NpcGenResource {
  int32_t type = 0;
  /** Template id in `elements.data` (MINE_ESSENCE). */
  int32_t template_id = 0;
  uint32_t refresh_time = 0;
  uint32_t count = 0;
  float height_offset = 0.0f;
};

struct NpcGenResArea {
  float pos[3] = {0.0f, 0.0f, 0.0f};
  float extent_x = 0.0f;
  float extent_z = 0.0f;
  uint8_t init_gen = 1;
  uint8_t auto_revive = 1;
  uint8_t valid_once = 0;
  uint32_t gen_id = 0;
  /* Version 6 and later: packed direction and rotation. */
  uint8_t dir[2] = {0, 0};
  uint8_t rad = 0;
  /* Version 7 and later. */
  int32_t controller_id = 0;
  int32_t max_num = 0;
  std::vector<NpcGenResource> resources;
};

/** Dynamic object (doors, gates, decorations switched by controllers). Version 6 and later. */
struct NpcGenDynObject {
  uint32_t id = 0;
  float pos[3] = {0.0f, 0.0f, 0.0f};
  uint8_t dir[2] = {0, 0};
  uint8_t rad = 0;
  /** Version 9 and later. */
  uint32_t controller_id = 0;
  /** Version 10 and later, 16 means 1.0. */
  uint8_t scale = 16;
};

struct NpcGenCtrlTime {
  /** [1900, 2100] or -1 for any. */
  int32_t year = -1;
  /** [0, 11] or -1. */
  int32_t month = -1;
  /** [0, 6] (0 is Sunday) or -1. */
  int32_t week = -1;
  /** [1, 31] or -1. */
  int32_t day = -1;
  /** [0, 23] or -1. */
  int32_t hours = -1;
  /** [0, 59]. */
  int32_t minutes = 0;
};

/** Spawn controller (trigger). Version 7 and later. */
struct NpcGenController {
  uint32_t id = 0;
  /** Designer facing id, used by tasks and scripts to switch the controller. */
  int32_t controller_id = 0;
  /** `char szName[128]`, GBK, kept raw. See #name / #set_name. */
  std::array<uint8_t, 128> name_raw = {};
  uint8_t activated = 0;
  int32_t wait_time = 0;
  int32_t stop_time = 0;
  uint8_t active_time_invalid = 1;
  uint8_t stop_time_invalid = 1;
  NpcGenCtrlTime active_time;
  NpcGenCtrlTime stop_time_point;
  /** Version 8 and later, minutes. */
  int32_t active_time_range = 0;

  /** Name decoded to UTF-8. */
  std::string name() const;
  /** Set the name from UTF-8 (GBK encoded, at most 127 bytes). */
  bool set_name(std::string_view utf8, std::string *r_error);
};

struct NpcGenData {
  uint32_t version = NPCGEN_VERSION_MAX;
  std::vector<NpcGenArea> areas;
  std::vector<NpcGenResArea> resource_areas;
  std::vector<NpcGenDynObject> dynamic_objects;
  std::vector<NpcGenController> controllers;
  /** Bytes after the last record (normally none). */
  Bytes trailing;

  size_t spawn_count() const;
  size_t resource_count() const;
  /** Controller by its #NpcGenController::id, nullptr when missing. */
  const NpcGenController *find_controller(uint32_t id) const;
};

bool parse_npcgen(ByteSpan data, NpcGenData &r_data, std::string *r_error);

/**
 * Serialize in #NpcGenData::version. Fails when the data uses something the version cannot store
 * (dynamic objects before version 6, controllers before 7).
 */
bool write_npcgen(const NpcGenData &data, Bytes &r_out, std::string *r_error);

}  // namespace blender::io::angelica
