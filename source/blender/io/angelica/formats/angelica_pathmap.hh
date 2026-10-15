/* SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup angelica
 *
 * Reader and writer for the id to path tables of the client (`DataPathMan`):
 * `configs\DynamicObjects.data` (models of the dynamic objects of `npcgen.data`) and
 * `data\path.data` of other Angelica games (Perfect World's `elements.data` stores its file paths
 * inline, as 128 byte GBK strings, and does not use `path.data`).
 *
 * Layout: `uint32 identify` (`PMID`, or a time stamp written by some editors), `uint32 count`,
 * then per entry `uint32 id`, `uint32 length` and `length` bytes of GBK path (no terminator).
 */

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "angelica_reader.hh"

namespace blender::io::angelica {

/** `PATHMAP_IDENTIFY`: the characters `PMID` as a big endian number. */
constexpr uint32_t PATHMAP_IDENTIFY = 0x504D4944u;
/** The engine reads paths into a 256 byte buffer. */
constexpr size_t PATHMAP_MAX_PATH = 255;

struct PathMapEntry {
  uint32_t id = 0;
  /** Path bytes as stored (GBK). */
  std::string path_gbk;
};

struct PathMapData {
  uint32_t identify = PATHMAP_IDENTIFY;
  std::vector<PathMapEntry> entries;
  /** Bytes after the table, kept verbatim. */
  Bytes trailing;
};

bool parse_pathmap(ByteSpan data, PathMapData &r_data, std::string *r_error);
/** Fails when an entry is longer than the engine accepts or ids/paths repeat. */
bool write_pathmap(const PathMapData &data, Bytes &r_out, std::string *r_error);

/** Whether the client would accept the table (identify is `PMID`). */
bool pathmap_is_engine_readable(const PathMapData &data);

std::string pathmap_path_utf8(const PathMapEntry &entry);
bool pathmap_set_path(PathMapEntry &entry, std::string_view utf8, std::string *r_error);
/** Index of the entry with `id`, -1 when missing. */
int64_t pathmap_find_id(const PathMapData &data, uint32_t id);
/** Index of the entry with the given path (case insensitive, `/` and `\` equal), -1 if missing. */
int64_t pathmap_find_path(const PathMapData &data, std::string_view utf8);
/** One more than the largest id in use. */
uint32_t pathmap_next_id(const PathMapData &data);

}  // namespace blender::io::angelica
