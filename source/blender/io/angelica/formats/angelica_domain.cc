/* SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup angelica
 */

#include <algorithm>
#include <cctype>
#include <cstring>

#include "angelica_domain.hh"
#include "angelica_elements.hh"

namespace blender::io::angelica {

namespace {

constexpr int32_t MAX_DOMAINS = 100000;

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

/** Reads `count` (int32) and checks that `count * item_size` bytes remain. */
bool read_count(BinaryReader &r, size_t item_size, int32_t &r_count)
{
  r_count = r.i32();
  return !r.failed() && r_count >= 0 && size_t(r_count) <= r.remaining() / item_size;
}

bool read_domain(BinaryReader &r, DomainFormat format, DomainInfo &d)
{
  r.read_raw(d.name.data(), d.name.size());
  d.id = r.i32();
  if (format == DomainFormat::Domain) {
    d.type = r.i32();
    d.reward = r.i32();
  }
  else {
    d.point = r.i32();
    d.war_type = r.i32();
    d.country_id = r.i32();
    d.is_capital = r.i32();
    r.read_raw(d.map_pos.data(), sizeof(d.map_pos));
  }
  d.center[0] = r.i32();
  d.center[1] = r.i32();
  int32_t n = 0;
  if (!read_count(r, 8, n)) {
    return false;
  }
  d.verts.resize(size_t(n));
  for (auto &v : d.verts) {
    v[0] = r.i32();
    v[1] = r.i32();
  }
  if (!read_count(r, 12, n)) {
    return false;
  }
  d.faces.resize(size_t(n) * 3);
  for (int32_t &index : d.faces) {
    index = r.i32();
  }
  if (!read_count(r, 4, n)) {
    return false;
  }
  d.neighbours.resize(size_t(n));
  for (int32_t &id : d.neighbours) {
    id = r.i32();
  }
  return !r.failed();
}

/** Returns false on a structural error; `r_exact` tells whether the file was consumed exactly. */
bool try_parse(ByteSpan data,
               DomainFormat format,
               bool has_timestamp,
               DomainData &r_data,
               bool &r_exact,
               std::string *r_error)
{
  r_data = DomainData();
  r_data.format = format;
  r_exact = false;
  BinaryReader r(data);
  if (data.size() >= 4) {
    uint32_t first = 0;
    std::memcpy(&first, data.data(), 4);
    if (first == DOMAIN_MAGIC) {
      r_data.has_magic = true;
      r.skip(4);
    }
  }
  if (format == DomainFormat::Domain2) {
    r_data.has_timestamp = has_timestamp;
    if (has_timestamp) {
      r_data.timestamp = r.u32();
    }
    r_data.speed = r.f32();
  }
  else {
    r_data.has_timestamp = false;
  }
  int32_t count = 0;
  if (!read_count(r, 48, count) || count > MAX_DOMAINS) {
    set_error(r_error, "invalid domain count");
    return false;
  }
  r_data.domains.resize(size_t(count));
  for (size_t i = 0; i < r_data.domains.size(); i++) {
    if (!read_domain(r, format, r_data.domains[i])) {
      set_error(r_error, "domain " + std::to_string(i) + " is invalid or truncated");
      return false;
    }
  }
  int32_t times = 0;
  if (!read_count(r, 12, times)) {
    set_error(r_error, "invalid battle time count");
    return false;
  }
  r_data.times.resize(size_t(times));
  for (DomainBattleTime &t : r_data.times) {
    t.day = r.i32();
    t.hour = r.i32();
    t.minute = r.i32();
  }
  r_data.time_max = r.i32();
  if (r.failed()) {
    set_error(r_error, "domain file is truncated");
    return false;
  }
  /* Extra time tables, only when they fill the rest of the file exactly. */
  {
    BinaryReader probe(data.subspan(r.tell()));
    std::vector<DomainTimeTable> extra;
    while (probe.remaining() >= 8) {
      int32_t n = 0;
      if (!read_count(probe, 12, n) || probe.remaining() < size_t(n) * 12 + 4) {
        break;
      }
      DomainTimeTable &table = extra.emplace_back();
      table.times.resize(size_t(n));
      for (DomainBattleTime &t : table.times) {
        t.day = probe.i32();
        t.hour = probe.i32();
        t.minute = probe.i32();
      }
      table.time_max = probe.i32();
    }
    if (!extra.empty() && probe.remaining() == 0 && !probe.failed()) {
      r_data.extra_time_tables = std::move(extra);
      r.skip(r.remaining());
    }
  }
  ByteSpan rest = r.read_span(r.remaining());
  r_data.trailing.assign(rest.begin(), rest.end());
  r_exact = r_data.trailing.empty();
  return true;
}

}  // namespace

DomainFormat domain_format_from_filename(std::string_view filename)
{
  const size_t slash = filename.find_last_of("/\\");
  std::string name(slash == std::string_view::npos ? filename : filename.substr(slash + 1));
  std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
    return char(std::tolower(c));
  });
  if (name.rfind("domain2", 0) == 0) {
    return DomainFormat::Domain2;
  }
  if (name.rfind("domain", 0) == 0) {
    return DomainFormat::Domain;
  }
  return DomainFormat::Auto;
}

bool parse_domain(ByteSpan data, DomainData &r_data, std::string *r_error, DomainFormat format)
{
  struct Variant {
    DomainFormat format;
    bool timestamp;
  };
  const Variant variants[] = {
      {DomainFormat::Domain2, true},
      {DomainFormat::Domain2, false},
      {DomainFormat::Domain, false},
  };
  DomainData fallback;
  bool have_fallback = false;
  std::string first_error;
  for (const Variant &variant : variants) {
    if (format != DomainFormat::Auto && variant.format != format) {
      continue;
    }
    DomainData candidate;
    bool exact = false;
    std::string err;
    if (!try_parse(data, variant.format, variant.timestamp, candidate, exact, &err)) {
      if (first_error.empty()) {
        first_error = err;
      }
      continue;
    }
    if (exact) {
      r_data = std::move(candidate);
      return true;
    }
    if (!have_fallback) {
      fallback = std::move(candidate);
      have_fallback = true;
    }
  }
  if (have_fallback) {
    r_data = std::move(fallback);
    return true;
  }
  set_error(r_error, "not a domain file: " + first_error);
  return false;
}

bool write_domain(const DomainData &data, Bytes &r_out, std::string *r_error)
{
  if (data.format == DomainFormat::Auto) {
    set_error(r_error, "domain format not set");
    return false;
  }
  r_out.clear();
  if (data.has_magic) {
    put<uint32_t>(r_out, DOMAIN_MAGIC);
  }
  if (data.format == DomainFormat::Domain2) {
    if (data.has_timestamp) {
      put<uint32_t>(r_out, data.timestamp);
    }
    put<float>(r_out, data.speed);
  }
  put<int32_t>(r_out, int32_t(data.domains.size()));
  for (const DomainInfo &d : data.domains) {
    if (d.faces.size() % 3 != 0) {
      set_error(r_error, "domain " + std::to_string(d.id) + ": face list is not triangles");
      return false;
    }
    r_out.insert(r_out.end(), d.name.begin(), d.name.end());
    put<int32_t>(r_out, d.id);
    if (data.format == DomainFormat::Domain) {
      put<int32_t>(r_out, d.type);
      put<int32_t>(r_out, d.reward);
    }
    else {
      put<int32_t>(r_out, d.point);
      put<int32_t>(r_out, d.war_type);
      put<int32_t>(r_out, d.country_id);
      put<int32_t>(r_out, d.is_capital);
      for (float f : d.map_pos) {
        put<float>(r_out, f);
      }
    }
    put<int32_t>(r_out, d.center[0]);
    put<int32_t>(r_out, d.center[1]);
    put<int32_t>(r_out, int32_t(d.verts.size()));
    for (const auto &v : d.verts) {
      put<int32_t>(r_out, v[0]);
      put<int32_t>(r_out, v[1]);
    }
    put<int32_t>(r_out, int32_t(d.faces.size() / 3));
    for (int32_t f : d.faces) {
      put<int32_t>(r_out, f);
    }
    put<int32_t>(r_out, int32_t(d.neighbours.size()));
    for (int32_t n : d.neighbours) {
      put<int32_t>(r_out, n);
    }
  }
  put<int32_t>(r_out, int32_t(data.times.size()));
  for (const DomainBattleTime &t : data.times) {
    put<int32_t>(r_out, t.day);
    put<int32_t>(r_out, t.hour);
    put<int32_t>(r_out, t.minute);
  }
  put<int32_t>(r_out, data.time_max);
  for (const DomainTimeTable &table : data.extra_time_tables) {
    put<int32_t>(r_out, int32_t(table.times.size()));
    for (const DomainBattleTime &t : table.times) {
      put<int32_t>(r_out, t.day);
      put<int32_t>(r_out, t.hour);
      put<int32_t>(r_out, t.minute);
    }
    put<int32_t>(r_out, table.time_max);
  }
  r_out.insert(r_out.end(), data.trailing.begin(), data.trailing.end());
  return true;
}

std::string domain_name(const DomainInfo &domain)
{
  size_t len = 0;
  while (len + 1 < domain.name.size() && (domain.name[len] | domain.name[len + 1]) != 0) {
    len += 2;
  }
  return utf16le_to_utf8(ByteSpan(domain.name.data(), len));
}

bool domain_set_name(DomainInfo &domain, std::string_view utf8, std::string *r_error)
{
  if (domain_name(domain) == utf8) {
    return true;
  }
  Bytes text;
  if (!utf8_to_utf16le(utf8, text)) {
    set_error(r_error, "invalid UTF-8 text");
    return false;
  }
  /* The engine reads all 16 characters; a name may fill the buffer without a terminator. */
  if (text.size() > domain.name.size()) {
    set_error(r_error, "domain name is longer than 16 characters");
    return false;
  }
  domain.name.fill(0);
  std::memcpy(domain.name.data(), text.data(), text.size());
  return true;
}

}  // namespace blender::io::angelica
