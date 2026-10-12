/* SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup angelica
 */

#include <algorithm>
#include <cstring>
#include <set>

#include "angelica_elements.hh"
#include "angelica_pathmap.hh"

namespace blender::io::angelica {

namespace {

void set_error(std::string *r_error, const std::string &message)
{
  if (r_error) {
    *r_error = message;
  }
}

template<typename T> void put(Bytes &out, const T &value)
{
  const size_t at = out.size();
  out.resize(at + sizeof(T));
  std::memcpy(out.data() + at, &value, sizeof(T));
}

/** Case and separator folding of a GBK path; the second byte of a double byte character is kept
 * as is (it can be in the ASCII letter range). */
std::string fold_path(std::string_view path)
{
  std::string out(path);
  for (size_t i = 0; i < out.size(); i++) {
    const uint8_t c = uint8_t(out[i]);
    if (c >= 0x81) {
      i++;
    }
    else if (c == '/') {
      out[i] = '\\';
    }
    else if (c >= 'A' && c <= 'Z') {
      out[i] = char(c - 'A' + 'a');
    }
  }
  return out;
}

}  // namespace

bool parse_pathmap(ByteSpan data, PathMapData &r_data, std::string *r_error)
{
  r_data = PathMapData();
  BinaryReader r(data);
  r_data.identify = r.u32();
  const uint32_t count = r.u32();
  if (r.failed() || count > r.remaining() / 8) {
    set_error(r_error, "not a path table (invalid header)");
    return false;
  }
  r_data.entries.resize(count);
  for (size_t i = 0; i < r_data.entries.size(); i++) {
    PathMapEntry &entry = r_data.entries[i];
    entry.id = r.u32();
    const uint32_t len = r.u32();
    if (r.failed() || len > r.remaining()) {
      set_error(r_error, "path table entry " + std::to_string(i) + " is truncated");
      return false;
    }
    ByteSpan bytes = r.read_span(len);
    entry.path_gbk.assign(reinterpret_cast<const char *>(bytes.data()), bytes.size());
  }
  ByteSpan rest = r.read_span(r.remaining());
  r_data.trailing.assign(rest.begin(), rest.end());
  return true;
}

bool write_pathmap(const PathMapData &data, Bytes &r_out, std::string *r_error)
{
  std::set<uint32_t> ids;
  std::set<std::string> paths;
  for (const PathMapEntry &entry : data.entries) {
    if (entry.path_gbk.size() > PATHMAP_MAX_PATH) {
      set_error(r_error, "path of id " + std::to_string(entry.id) + " is longer than 255 bytes");
      return false;
    }
    if (!ids.insert(entry.id).second) {
      set_error(r_error, "id " + std::to_string(entry.id) + " is used twice");
      return false;
    }
    /* The engine compares paths exactly. */
    if (!paths.insert(entry.path_gbk).second) {
      set_error(r_error, "path of id " + std::to_string(entry.id) + " is listed twice");
      return false;
    }
  }
  r_out.clear();
  put<uint32_t>(r_out, data.identify);
  put<uint32_t>(r_out, uint32_t(data.entries.size()));
  for (const PathMapEntry &entry : data.entries) {
    put<uint32_t>(r_out, entry.id);
    put<uint32_t>(r_out, uint32_t(entry.path_gbk.size()));
    r_out.insert(r_out.end(), entry.path_gbk.begin(), entry.path_gbk.end());
  }
  r_out.insert(r_out.end(), data.trailing.begin(), data.trailing.end());
  return true;
}

bool pathmap_is_engine_readable(const PathMapData &data)
{
  return data.identify == PATHMAP_IDENTIFY;
}

std::string pathmap_path_utf8(const PathMapEntry &entry)
{
  const size_t nul = entry.path_gbk.find('\0');
  return gbk_to_utf8(std::string_view(entry.path_gbk).substr(0, nul));
}

bool pathmap_set_path(PathMapEntry &entry, std::string_view utf8, std::string *r_error)
{
  if (pathmap_path_utf8(entry) == utf8) {
    return true;
  }
  std::string gbk;
  if (!utf8_to_gbk(utf8, gbk)) {
    set_error(r_error, "path cannot be encoded as GBK");
    return false;
  }
  if (gbk.size() > PATHMAP_MAX_PATH) {
    set_error(r_error, "path is longer than 255 bytes");
    return false;
  }
  entry.path_gbk = std::move(gbk);
  return true;
}

int64_t pathmap_find_id(const PathMapData &data, const uint32_t id)
{
  for (size_t i = 0; i < data.entries.size(); i++) {
    if (data.entries[i].id == id) {
      return int64_t(i);
    }
  }
  return -1;
}

int64_t pathmap_find_path(const PathMapData &data, std::string_view utf8)
{
  std::string gbk;
  if (!utf8_to_gbk(utf8, gbk)) {
    return -1;
  }
  const std::string key = fold_path(gbk);
  for (size_t i = 0; i < data.entries.size(); i++) {
    if (fold_path(data.entries[i].path_gbk) == key) {
      return int64_t(i);
    }
  }
  return -1;
}

uint32_t pathmap_next_id(const PathMapData &data)
{
  uint32_t next = 1;
  for (const PathMapEntry &entry : data.entries) {
    next = std::max(next, entry.id + 1);
  }
  return next;
}

}  // namespace blender::io::angelica
