/* SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup angelica
 */

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <cstring>

#include "angelica_elements.hh"
#include "angelica_precinct.hh"

namespace blender::io::angelica {

namespace {

constexpr int32_t MAX_PRECINCTS = 100000;
constexpr int32_t MAX_POINTS = 1000000;

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

/* -------------------------------------------------------------------- */
/** \name Script tokenizer
 *
 * Same rules as the engine's `AWScriptFile`: tokens are separated by characters up to 32, `;`
 * and `,`; `"..."` and `(...)` quote a token; `//` and `/ * * /` are comments. A token request
 * either may cross line ends or must be found on the current line.
 * \{ */

class Script {
 public:
  explicit Script(std::string_view text) : s_(text) {}

  bool next(bool cross_line, std::string &r_token)
  {
    while (true) {
      while (pos_ < s_.size()) {
        const unsigned char c = uint8_t(s_[pos_]);
        if (c > 32 && c != ';' && c != ',') {
          break;
        }
        if (c == '\n' && !cross_line) {
          return false;
        }
        pos_++;
      }
      if (pos_ >= s_.size()) {
        return false;
      }
      if (s_.compare(pos_, 2, "//") == 0) {
        const size_t end = s_.find('\n', pos_);
        if (end == std::string_view::npos || !cross_line) {
          pos_ = end == std::string_view::npos ? s_.size() : end;
          return false;
        }
        pos_ = end + 1;
        continue;
      }
      if (s_.compare(pos_, 2, "/*") == 0) {
        const size_t end = s_.find("*/", pos_ + 2);
        if (end == std::string_view::npos) {
          pos_ = s_.size();
          return false;
        }
        const bool crossed = s_.substr(pos_, end - pos_).find('\n') != std::string_view::npos;
        pos_ = end + 2;
        if (crossed && !cross_line) {
          return false;
        }
        continue;
      }
      break;
    }
    const char c = s_[pos_];
    if (c == '"' || c == '(') {
      const char end_char = c == '"' ? '"' : ')';
      const size_t end = s_.find(end_char, pos_ + 1);
      const size_t stop = end == std::string_view::npos ? s_.size() : end;
      r_token.assign(s_.substr(pos_ + 1, stop - pos_ - 1));
      pos_ = std::min(s_.size(), stop + 1);
      return true;
    }
    const size_t start = pos_;
    while (pos_ < s_.size()) {
      const unsigned char ch = uint8_t(s_[pos_]);
      if (ch <= 32 || ch == ';' || ch == ',') {
        break;
      }
      pos_++;
    }
    r_token.assign(s_.substr(start, pos_ - start));
    return true;
  }

  bool peek(bool cross_line)
  {
    const size_t saved = pos_;
    std::string token;
    const bool found = next(cross_line, token);
    pos_ = saved;
    return found;
  }

  int32_t next_int(bool cross_line)
  {
    std::string token;
    if (!next(cross_line, token)) {
      missing_ = true;
      return 0;
    }
    return int32_t(std::strtol(token.c_str(), nullptr, 10));
  }

  float next_float(bool cross_line)
  {
    std::string token;
    if (!next(cross_line, token)) {
      missing_ = true;
      return 0.0f;
    }
    return std::strtof(token.c_str(), nullptr);
  }

  std::string next_string(bool cross_line)
  {
    std::string token;
    if (!next(cross_line, token)) {
      missing_ = true;
    }
    return token;
  }

  bool missing() const
  {
    return missing_;
  }

 private:
  std::string_view s_;
  size_t pos_ = 0;
  bool missing_ = false;
};

/** \} */

bool read_text_precinct(Script &s, uint32_t version, Precinct &p)
{
  p.name = s.next_string(true);
  int32_t points = 0;
  if (version < 2) {
    p.id = 0;
    points = s.next_int(true);
  }
  else {
    p.id = uint32_t(s.next_int(true));
    points = s.next_int(false);
  }
  const int32_t marks = s.next_int(false);
  p.priority = s.next_int(false);
  p.dst_instance = s.next_int(false);
  const int32_t music = s.next_int(false);
  p.music_interval = s.next_int(false);
  p.music_loop = s.next_int(false);
  p.src_instance = version >= 4 ? s.next_int(false) : 1;
  p.domain_id = version >= 6 ? s.next_int(false) : 0;
  p.pk_protect = version >= 7 ? s.next_int(false) : 0;
  if (points < 0 || points > MAX_POINTS || marks < 0 || marks > MAX_POINTS || music < 0 ||
      music > MAX_POINTS)
  {
    return false;
  }
  p.city_pos[0] = s.next_float(true);
  p.city_pos[1] = s.next_float(false);
  p.city_pos[2] = s.next_float(false);
  p.points.resize(size_t(points));
  for (auto &v : p.points) {
    v[0] = s.next_float(true);
    v[1] = s.next_float(false);
    v[2] = s.next_float(false);
  }
  p.marks.resize(size_t(marks));
  for (PrecinctMark &mark : p.marks) {
    mark.name = s.next_string(true);
    mark.pos[0] = s.next_float(false);
    mark.pos[1] = s.next_float(false);
    mark.pos[2] = s.next_float(false);
  }
  p.sound = s.next_string(true);
  p.music_files.resize(size_t(music));
  for (std::string &file : p.music_files) {
    file = s.next_string(true);
  }
  if (version >= 3) {
    p.sound_night = s.next_string(true);
  }
  return !s.missing();
}

bool parse_text(ByteSpan data, PrecinctData &r_data, std::string *r_error)
{
  std::string text;
  if (data.size() >= 2 && data[0] == 0xFF && data[1] == 0xFE) {
    r_data.format = PrecinctFormat::Text;
    text = utf16le_to_utf8(data.subspan(2, (data.size() - 2) & ~size_t(1)));
  }
  else {
    r_data.format = PrecinctFormat::TextUtf8;
    size_t skip = 0;
    if (data.size() >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
      skip = 3;
    }
    text.assign(reinterpret_cast<const char *>(data.data()) + skip, data.size() - skip);
  }
  Script s(text);
  std::string token;
  /* `MatchToken("version")`: skip tokens until the keyword. */
  bool found = false;
  while (s.next(true, token)) {
    if (token == "version") {
      found = true;
      break;
    }
  }
  if (!found) {
    set_error(r_error, "not a precinct file (no version)");
    return false;
  }
  const int32_t version = s.next_int(false);
  if (version < 1 || uint32_t(version) > PRECINCT_VERSION_MAX) {
    set_error(r_error, "unsupported precinct file version " + std::to_string(version));
    return false;
  }
  r_data.version = uint32_t(version);
  r_data.timestamp = version >= 5 ? uint32_t(s.next_int(true)) : 0;
  while (s.peek(true)) {
    if (r_data.precincts.size() >= size_t(MAX_PRECINCTS)) {
      set_error(r_error, "too many precincts");
      return false;
    }
    Precinct &p = r_data.precincts.emplace_back();
    if (!read_text_precinct(s, r_data.version, p)) {
      set_error(r_error,
                "precinct " + std::to_string(r_data.precincts.size() - 1) + " is incomplete");
      return false;
    }
  }
  r_data.source.assign(data.begin(), data.end());
  return true;
}

bool parse_binary(ByteSpan data, PrecinctData &r_data, std::string *r_error)
{
  r_data.format = PrecinctFormat::Binary;
  BinaryReader r(data);
  r_data.version = r.u32();
  const int32_t count = r.i32();
  if (r.failed() || r_data.version > PRECINCT_VERSION_MAX || count < 0 || count > MAX_PRECINCTS) {
    set_error(r_error, "not a precinct.sev file");
    return false;
  }
  r_data.timestamp = r_data.version >= 5 ? r.u32() : 0;
  r_data.precincts.resize(size_t(count));
  for (Precinct &p : r_data.precincts) {
    const int32_t points = r.i32();
    p.priority = r.i32();
    p.dst_instance = r.i32();
    p.src_instance = r_data.version >= 4 ? r.i32() : 1;
    p.domain_id = r_data.version >= 6 ? r.i32() : 0;
    p.pk_protect = r_data.version >= 7 ? r.u8() : 0;
    for (float &f : p.city_pos) {
      f = r.f32();
    }
    if (r.failed() || points < 0 || size_t(points) > r.remaining() / 12) {
      set_error(r_error, "precinct.sev is truncated");
      return false;
    }
    p.points.resize(size_t(points));
    for (auto &v : p.points) {
      v[0] = r.f32();
      v[1] = r.f32();
      v[2] = r.f32();
    }
  }
  if (r.failed()) {
    set_error(r_error, "precinct.sev is truncated");
    return false;
  }
  ByteSpan rest = r.read_span(r.remaining());
  r_data.trailing.assign(rest.begin(), rest.end());
  return true;
}

std::string format_float(float value)
{
  char buf[64];
  const auto result = std::to_chars(buf, buf + sizeof(buf), value);
  return std::string(buf, result.ptr);
}

std::string quote(const std::string &text)
{
  return "\"" + text + "\"";
}

bool write_text(const PrecinctData &data, Bytes &r_out, std::string *r_error)
{
  std::string t;
  const auto line = [&](const std::string &s) { t += s + "\r\n"; };
  line("version  " + std::to_string(data.version));
  if (data.version >= 5) {
    line(std::to_string(data.timestamp));
  }
  for (const Precinct &p : data.precincts) {
    for (const std::string *s : {&p.name, &p.sound, &p.sound_night}) {
      if (s->find('"') != std::string::npos) {
        set_error(r_error, "precinct texts cannot contain '\"'");
        return false;
      }
    }
    line("");
    line(quote(p.name));
    std::string ints;
    if (data.version >= 2) {
      ints += std::to_string(p.id) + "  ";
    }
    ints += std::to_string(p.points.size()) + "  " + std::to_string(p.marks.size()) + "  " +
            std::to_string(p.priority) + "  " + std::to_string(p.dst_instance) + "  " +
            std::to_string(p.music_files.size()) + "  " + std::to_string(p.music_interval) + "  " +
            std::to_string(p.music_loop);
    if (data.version >= 4) {
      ints += "  " + std::to_string(p.src_instance);
    }
    if (data.version >= 6) {
      ints += "  " + std::to_string(p.domain_id);
    }
    if (data.version >= 7) {
      ints += "  " + std::to_string(p.pk_protect);
    }
    line(ints);
    const auto vec = [&](const std::array<float, 3> &v) {
      return format_float(v[0]) + ", " + format_float(v[1]) + ", " + format_float(v[2]);
    };
    line(vec(p.city_pos));
    for (const auto &v : p.points) {
      line(vec(v));
    }
    for (const PrecinctMark &mark : p.marks) {
      line(quote(mark.name) + "  " + vec(mark.pos));
    }
    line(quote(p.sound));
    for (const std::string &file : p.music_files) {
      line(quote(file));
    }
    if (data.version >= 3) {
      line(quote(p.sound_night));
    }
  }
  r_out.clear();
  if (data.format == PrecinctFormat::TextUtf8) {
    r_out.assign(t.begin(), t.end());
    return true;
  }
  Bytes utf16;
  if (!utf8_to_utf16le(t, utf16)) {
    set_error(r_error, "invalid UTF-8 text");
    return false;
  }
  r_out.push_back(0xFF);
  r_out.push_back(0xFE);
  r_out.insert(r_out.end(), utf16.begin(), utf16.end());
  return true;
}

void write_binary(const PrecinctData &data, Bytes &r_out)
{
  r_out.clear();
  put<uint32_t>(r_out, data.version);
  put<int32_t>(r_out, int32_t(data.precincts.size()));
  if (data.version >= 5) {
    put<uint32_t>(r_out, data.timestamp);
  }
  for (const Precinct &p : data.precincts) {
    put<int32_t>(r_out, int32_t(p.points.size()));
    put<int32_t>(r_out, p.priority);
    put<int32_t>(r_out, p.dst_instance);
    if (data.version >= 4) {
      put<int32_t>(r_out, p.src_instance);
    }
    if (data.version >= 6) {
      put<int32_t>(r_out, p.domain_id);
    }
    if (data.version >= 7) {
      put<uint8_t>(r_out, uint8_t(p.pk_protect));
    }
    for (float f : p.city_pos) {
      put<float>(r_out, f);
    }
    for (const auto &v : p.points) {
      put<float>(r_out, v[0]);
      put<float>(r_out, v[1]);
      put<float>(r_out, v[2]);
    }
  }
  r_out.insert(r_out.end(), data.trailing.begin(), data.trailing.end());
}

}  // namespace

bool parse_precinct(ByteSpan data, PrecinctData &r_data, std::string *r_error)
{
  r_data = PrecinctData();
  /* The binary export starts with a small version number; text starts with a byte order mark
   * or printable characters. */
  if (data.size() >= 8 && data[0] <= PRECINCT_VERSION_MAX && data[1] == 0 && data[2] == 0 &&
      data[3] == 0)
  {
    return parse_binary(data, r_data, r_error);
  }
  return parse_text(data, r_data, r_error);
}

bool write_precinct(const PrecinctData &data, Bytes &r_out, std::string *r_error)
{
  if (data.format == PrecinctFormat::Binary) {
    write_binary(data, r_out);
    return true;
  }
  if (!data.source.empty()) {
    /* Keep the original text (comments, spacing, number spelling) while nothing changed. */
    PrecinctData original;
    if (parse_text(data.source, original, nullptr) && original.format == data.format &&
        original.version == data.version && original.timestamp == data.timestamp &&
        original.precincts == data.precincts)
    {
      r_out = data.source;
      return true;
    }
  }
  return write_text(data, r_out, r_error);
}

}  // namespace blender::io::angelica
