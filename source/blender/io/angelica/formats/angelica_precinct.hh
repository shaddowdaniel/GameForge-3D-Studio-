/* SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup angelica
 *
 * Reader and writer for the precinct (named area) files of a map: the client text file
 * `precinct.clt` (UTF-16 script read by `CELPrecinctSet::Load` through `AWScriptFile`) and the
 * server binary export `precinct.sev`.
 *
 * A precinct is a polygon (3D points, only x and z are used) with a name, a priority, the city
 * (revive) position, the instance it leads to, map marks and background music.
 *
 * The text writer reproduces the source file exactly while the model is unchanged; after edits
 * it writes a clean file in the engine's layout.
 */

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "angelica_reader.hh"

namespace blender::io::angelica {

/** Newest precinct file version known (`ELPCTFILE_VERSION`). */
constexpr uint32_t PRECINCT_VERSION_MAX = 7;

struct PrecinctMark {
  std::string name;
  std::array<float, 3> pos{};

  bool operator==(const PrecinctMark &other) const = default;
};

struct Precinct {
  /** UTF-8. */
  std::string name;
  /** Version 2 and later. */
  uint32_t id = 0;
  int32_t priority = 0;
  int32_t dst_instance = 0;
  int32_t music_interval = 0;
  int32_t music_loop = 0;
  /** Version 4 and later (1 before). */
  int32_t src_instance = 1;
  /** Version 6 and later. */
  int32_t domain_id = 0;
  /** Version 7 and later. Text files store an integer, the server file one byte. */
  int32_t pk_protect = 0;
  std::array<float, 3> city_pos{};
  std::vector<std::array<float, 3>> points;
  std::vector<PrecinctMark> marks;
  /** Ambient sound file, relative to `music\` (as written in the file). */
  std::string sound;
  std::vector<std::string> music_files;
  /** Night ambient sound, version 3 and later. */
  std::string sound_night;

  bool operator==(const Precinct &other) const = default;
};

enum class PrecinctFormat : uint8_t {
  /** `precinct.clt`, UTF-16 text with byte order mark. */
  Text,
  /** `precinct.clt` saved as UTF-8 by external tools (the client does not read it). */
  TextUtf8,
  /** `precinct.sev`. */
  Binary,
};

struct PrecinctData {
  PrecinctFormat format = PrecinctFormat::Text;
  uint32_t version = PRECINCT_VERSION_MAX;
  /** Version 5 and later. */
  uint32_t timestamp = 0;
  std::vector<Precinct> precincts;
  /** Text files: the bytes read, written back unchanged while the model matches them. */
  Bytes source;
  /** Binary files: bytes after the last precinct. */
  Bytes trailing;
};

/** Detects text or binary from the content. */
bool parse_precinct(ByteSpan data, PrecinctData &r_data, std::string *r_error);
bool write_precinct(const PrecinctData &data, Bytes &r_out, std::string *r_error);

}  // namespace blender::io::angelica
