/* SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup angelica
 *
 * Reader and writer for the territory war maps: `data\domain.data` (guild territory war, 1.x
 * map) and `data\domain2.data` / `data\domain2_cross.data` (country war), loaded by
 * `globaldata_load` and `loadimpl_domain2_data`.
 *
 * Every domain is a polygon on the war map (2D integer vertices, a triangulation and the ids of
 * the neighbouring domains). The files end with the battle time table.
 *
 * Variants: some clients prefix `domain.data` with the four characters `MOXB`; country war maps
 * store a time stamp before the move speed since 1.5. Both are detected and written back as read.
 */

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "angelica_reader.hh"

namespace blender::io::angelica {

/** `MOXB` prefix of some `domain.data` files. */
constexpr uint32_t DOMAIN_MAGIC = 0x42584F4Du;

enum class DomainFormat : uint8_t {
  Auto = 0,
  /** `domain.data`. */
  Domain,
  /** `domain2.data`, `domain2_cross.data`. */
  Domain2,
};

struct DomainInfo {
  /** UTF-16 name, 16 characters. */
  std::array<uint8_t, 32> name{};
  int32_t id = 0;
  /* Domain (territory war). */
  int32_t type = 0;
  int32_t reward = 0;
  /* Domain2 (country war). */
  int32_t point = 0;
  int32_t war_type = 0;
  int32_t country_id = 0;
  int32_t is_capital = 0;
  /** Four 3D positions on the world map (`mappos[4][3]`). */
  std::array<float, 12> map_pos{};
  /* Both. */
  std::array<int32_t, 2> center{};
  std::vector<std::array<int32_t, 2>> verts;
  /** Triangle vertex indices, three per triangle. */
  std::vector<int32_t> faces;
  std::vector<int32_t> neighbours;
};

struct DomainBattleTime {
  int32_t day = 0;
  int32_t hour = 0;
  int32_t minute = 0;
};

/** A battle time table (`count`, times, `nTimeMax`). */
struct DomainTimeTable {
  std::vector<DomainBattleTime> times;
  int32_t time_max = 0;
};

struct DomainData {
  DomainFormat format = DomainFormat::Domain;
  bool has_magic = false;
  /** Domain2: whether the file has the time stamp (1.5 and later). */
  bool has_timestamp = true;
  uint32_t timestamp = 0;
  /** Domain2: move speed used to compute travel times between domains. */
  float speed = 1.0f;
  std::vector<DomainInfo> domains;
  std::vector<DomainBattleTime> times;
  int32_t time_max = 0;
  /**
   * Further time tables after the first one. Some `MOXB` territory maps store a second schedule
   * with the same layout; the engine of the 1.5 sources reads only the first.
   */
  std::vector<DomainTimeTable> extra_time_tables;
  /** Bytes after the tables, kept verbatim. */
  Bytes trailing;
};

/**
 * Parse a domain file. With `format == DomainFormat::Auto` the variant that consumes the whole
 * file is chosen (pass the format when the file name is known).
 */
bool parse_domain(ByteSpan data,
                  DomainData &r_data,
                  std::string *r_error,
                  DomainFormat format = DomainFormat::Auto);
bool write_domain(const DomainData &data, Bytes &r_out, std::string *r_error);

/** Format from a file name (`domain.data` or `domain2*.data`), Auto when unknown. */
DomainFormat domain_format_from_filename(std::string_view filename);

std::string domain_name(const DomainInfo &domain);
bool domain_set_name(DomainInfo &domain, std::string_view utf8, std::string *r_error);

}  // namespace blender::io::angelica
