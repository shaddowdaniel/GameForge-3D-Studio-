/* SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup angelica
 *
 * `elements.data` layout (`elementdataman::save_data` / `load_data`), all little-endian, 32-bit
 * client so `size_t` and `time_t` are 4 bytes:
 *
 *   u16 version, u16 signature   the version (156 for PW 1.5.5) picks the schema; the signature is
 *                                0x3000 for PW. Versions above 7 follow with a u32 export
 * `time_t`, which the `.cfg` files count in the prefix of list 0 (`4`). then for each list, in
 * schema order: prefix           fixed byte count from the schema, or an `AUTO` block, see below.
 *     [i32 list_type]  versions 191..349 only
 *     u32 count
 *     [u32 item_size]  versions 191..349 only, must equal the schema record size
 *     count * record_size bytes (`array<T>::save`)
 *   except for the talk-proc list (its index is the second line of the `.cfg`: after NPC_ESSENCE
 * in 1.5.x schemas, last in 1.7.x schemas): u32 count, then per talk proc: u32 id_talk, u16
 * text[64], i32 num_window, then per window: u32 id, u32 id_parent, i32 text_len, u16
 * text[text_len], i32 num_option, num_option * { u32 id, u16 text[64], u32 param }   (136 bytes)
 *
 * AUTO blocks (what the exporter writes with `_MD5_CHECK`; editors may randomize the tags, so they
 * are located by list, not by tag value):
 *   list 0:                        u32 time_t, u32 len, len bytes
 *   before SKILLTOME_SUB_TYPE:     u32 tag (0xAB7689DD), u32 len, len bytes (computer name),
 *                                  u32 time_t
 *   before NPC_WAR_TOWERBUILD_SERVICE and any other list:
 *                                  u32 tag (0xEE35679F), u32 len, len bytes (hardware info)
 * The bytes are kept verbatim so they round-trip exactly.
 *
 * Versions 350 and later (PW 1.7.4+) store every list compressed with a different header and are
 * rejected. `.cfg` field sizes are bytes: `wstring:64` is 32 UTF-16 code units.
 *
 * Cross-checked against the engine (`elementdataman.cpp`), sELedit and RAE ("Universal data
 * Editor") readers.
 */

#include "angelica_elements.hh"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <system_error>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <iconv.h>
#endif

namespace blender::io::angelica {

namespace fs = std::filesystem;

namespace {

constexpr uint32_t TAG_EXPORTER = 0xAB7689DD;
/** Sanity limits against garbage counts. */
constexpr uint32_t MAX_TAG_BLOCK = 1u << 20;
constexpr int32_t MAX_TALK_WINDOWS = 1 << 16;
constexpr int32_t MAX_TALK_OPTIONS = 1 << 16;
constexpr size_t TALK_TEXT_BYTES = 128;
constexpr size_t TALK_OPTION_BYTES = 4 + TALK_TEXT_BYTES + 4;

void set_error(std::string *r_error, std::string message)
{
  if (r_error) {
    *r_error = std::move(message);
  }
}

std::string_view trim(std::string_view s)
{
  while (!s.empty() &&
         (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n' ||
          uint8_t(s.front()) == 0xEF || uint8_t(s.front()) == 0xBB || uint8_t(s.front()) == 0xBF))
  {
    s.remove_prefix(1);
  }
  while (!s.empty() &&
         (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n'))
  {
    s.remove_suffix(1);
  }
  return s;
}

bool parse_uint(std::string_view s, uint64_t &r_value)
{
  s = trim(s);
  if (s.empty()) {
    return false;
  }
  const auto res = std::from_chars(s.data(), s.data() + s.size(), r_value);
  return res.ec == std::errc() && res.ptr == s.data() + s.size();
}

std::vector<std::string_view> split(std::string_view s, const char sep)
{
  std::vector<std::string_view> out;
  size_t start = 0;
  while (true) {
    const size_t p = s.find(sep, start);
    if (p == std::string_view::npos) {
      out.push_back(s.substr(start));
      break;
    }
    out.push_back(s.substr(start, p - start));
    start = p + 1;
  }
  return out;
}

bool ends_with(std::string_view s, std::string_view suffix)
{
  return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

bool iequals(std::string_view a, std::string_view b)
{
  if (a.size() != b.size()) {
    return false;
  }
  for (size_t i = 0; i < a.size(); i++) {
    char x = a[i], y = b[i];
    if (x >= 'A' && x <= 'Z') {
      x = char(x - 'A' + 'a');
    }
    if (y >= 'A' && y <= 'Z') {
      y = char(y - 'A' + 'a');
    }
    if (x != y) {
      return false;
    }
  }
  return true;
}

/** Engine `ID_SPACE` of a list (`elementdataman::add_id_index` calls). */
ElementIdSpace id_space_of(std::string_view name)
{
  static constexpr std::string_view face[] = {"FACE_TEXTURE_ESSENCE",
                                              "FACE_SHAPE_ESSENCE",
                                              "FACE_EMOTION_TYPE",
                                              "FACE_EXPRESSION_ESSENCE",
                                              "FACE_HAIR_ESSENCE",
                                              "FACE_MOUSTACHE_ESSENCE",
                                              "COLORPICKER_ESSENCE",
                                              "CUSTOMIZEDATA_ESSENCE",
                                              "FACE_FALING_ESSENCE",
                                              "FACE_THIRDEYE_ESSENCE",
                                              "FACE_HAIR_TEXTURE_MAP"};
  static constexpr std::string_view recipe[] = {"RECIPE_MAJOR_TYPE",
                                                "RECIPE_SUB_TYPE",
                                                "RECIPE_ESSENCE",
                                                "ENGRAVE_MAJOR_TYPE",
                                                "ENGRAVE_SUB_TYPE",
                                                "ENGRAVE_ESSENCE",
                                                "RANDPROP_TYPE",
                                                "RANDPROP_ESSENCE",
                                                "STONE_CHANGE_RECIPE_TYPE",
                                                "STONE_CHANGE_RECIPE"};
  if (name == "TALK_PROC") {
    return ElementIdSpace::Talk;
  }
  if (ends_with(name, "_ADDON")) {
    return ElementIdSpace::Addon;
  }
  for (const std::string_view f : face) {
    if (name == f) {
      return ElementIdSpace::Face;
    }
  }
  for (const std::string_view r : recipe) {
    if (name == r) {
      return ElementIdSpace::Recipe;
    }
  }
  if (ends_with(name, "_CONFIG")) {
    return ElementIdSpace::Config;
  }
  return ElementIdSpace::Essence;
}

bool parse_field_type(std::string_view token, ElementFieldDef &r_field, bool &r_talk_proc)
{
  r_talk_proc = false;
  token = trim(token);
  const auto sized = [&](std::string_view prefix, ElementFieldType type) {
    if (token.substr(0, prefix.size()) != prefix) {
      return false;
    }
    uint64_t n = 0;
    if (!parse_uint(token.substr(prefix.size()), n) || n == 0 || n > (1u << 20)) {
      return false;
    }
    r_field.type = type;
    r_field.size = uint32_t(n);
    return true;
  };
  if (token == "int16" || token == "short") {
    r_field.type = ElementFieldType::Int16;
    r_field.size = 2;
  }
  else if (token == "int32" || token == "int") {
    r_field.type = ElementFieldType::Int32;
    r_field.size = 4;
  }
  else if (token == "uint" || token == "uint32") {
    r_field.type = ElementFieldType::UInt32;
    r_field.size = 4;
  }
  else if (token == "int64") {
    r_field.type = ElementFieldType::Int64;
    r_field.size = 8;
  }
  else if (token == "float") {
    r_field.type = ElementFieldType::Float;
    r_field.size = 4;
  }
  else if (token == "double") {
    r_field.type = ElementFieldType::Double;
    r_field.size = 8;
  }
  else if (token == "byte:AUTO") {
    r_talk_proc = true;
  }
  else if (sized("byte:", ElementFieldType::Raw) || sized("wstring:", ElementFieldType::WString) ||
           sized("string:", ElementFieldType::String))
  {
  }
  else {
    return false;
  }
  return true;
}

/** Split `NNN - NAME` into its number and name. */
void parse_label(std::string_view label, int &r_number, std::string &r_name)
{
  label = trim(label);
  r_number = -1;
  const size_t dash = label.find(" - ");
  if (dash != std::string_view::npos) {
    uint64_t n = 0;
    if (parse_uint(label.substr(0, dash), n) && n < 100000) {
      r_number = int(n);
      r_name = std::string(trim(label.substr(dash + 3)));
      return;
    }
  }
  r_name = std::string(label);
}

/* Text conversion. */

void append_utf8(std::string &out, uint32_t cp)
{
  if (cp < 0x80) {
    out.push_back(char(cp));
  }
  else if (cp < 0x800) {
    out.push_back(char(0xC0 | (cp >> 6)));
    out.push_back(char(0x80 | (cp & 0x3F)));
  }
  else if (cp < 0x10000) {
    out.push_back(char(0xE0 | (cp >> 12)));
    out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(char(0x80 | (cp & 0x3F)));
  }
  else {
    out.push_back(char(0xF0 | (cp >> 18)));
    out.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(char(0x80 | (cp & 0x3F)));
  }
}

bool is_ascii(std::string_view s)
{
  for (const char c : s) {
    if (uint8_t(c) >= 0x80) {
      return false;
    }
  }
  return true;
}

std::string fixed_gbk_to_utf8(ByteSpan bytes)
{
  size_t len = 0;
  while (len < bytes.size() && bytes[len] != 0) {
    len++;
  }
  return gbk_to_utf8(std::string_view(reinterpret_cast<const char *>(bytes.data()), len));
}

template<typename T> T load_le(const uint8_t *p)
{
  T v;
  std::memcpy(&v, p, sizeof(T));
  return v;
}

template<typename T> void store_le(uint8_t *p, const T v)
{
  std::memcpy(p, &v, sizeof(T));
}

void put_u32(Bytes &out, const uint32_t v)
{
  const size_t at = out.size();
  out.resize(at + 4);
  store_le(out.data() + at, v);
}

ElementValue decode_field(const ElementFieldDef &f, const uint8_t *p)
{
  switch (f.type) {
    case ElementFieldType::Int16:
      return int64_t(load_le<int16_t>(p));
    case ElementFieldType::Int32:
      return int64_t(load_le<int32_t>(p));
    case ElementFieldType::UInt32:
      return int64_t(load_le<uint32_t>(p));
    case ElementFieldType::Int64:
      return load_le<int64_t>(p);
    case ElementFieldType::Float:
      return double(load_le<float>(p));
    case ElementFieldType::Double:
      return load_le<double>(p);
    case ElementFieldType::Raw:
      return Bytes(p, p + f.size);
    case ElementFieldType::WString:
      return utf16le_to_utf8(ByteSpan(p, f.size));
    case ElementFieldType::String:
      return fixed_gbk_to_utf8(ByteSpan(p, f.size));
  }
  return int64_t(0);
}

/** Read an AUTO prefix block and return its raw bytes. */
bool read_auto_prefix(BinaryReader &r,
                      const size_t list_index,
                      std::string_view list_name,
                      Bytes &r_prefix,
                      std::string *r_error)
{
  const size_t start = r.tell();
  const ByteSpan all = r.data();
  bool trailing_time = false;
  if (list_index == 0) {
    /* Export time, then a length prefixed block. */
    r.skip(4);
  }
  else {
    const uint32_t tag = r.u32();
    trailing_time = list_name == "SKILLTOME_SUB_TYPE" || tag == TAG_EXPORTER;
  }
  const uint32_t len = r.u32();
  if (r.failed() || len > MAX_TAG_BLOCK || len > r.remaining()) {
    set_error(r_error,
              "elements.data: invalid AUTO block before list " + std::to_string(list_index) + " " +
                  std::string(list_name));
    return false;
  }
  r.skip(len);
  if (trailing_time) {
    r.skip(4);
  }
  if (r.failed()) {
    set_error(r_error,
              "elements.data: truncated AUTO block before list " + std::to_string(list_index));
    return false;
  }
  r_prefix.assign(all.begin() + start, all.begin() + r.tell());
  return true;
}

/** Walk the talk-proc list, validating it. Returns false on malformed data. */
bool scan_talk_procs(BinaryReader &r, uint32_t &r_count, std::vector<TalkProc> *r_procs)
{
  const int32_t count = r.i32();
  if (r.failed() || count < 0) {
    return false;
  }
  for (int32_t i = 0; i < count; i++) {
    TalkProc proc;
    proc.id_talk = r.u32();
    ByteSpan text = r.read_span(TALK_TEXT_BYTES);
    const int32_t num_window = r.i32();
    if (r.failed() || num_window < 0 || num_window > MAX_TALK_WINDOWS) {
      return false;
    }
    if (r_procs) {
      proc.text = utf16le_to_utf8(text);
    }
    for (int32_t w = 0; w < num_window; w++) {
      TalkWindow window;
      window.id = r.u32();
      window.id_parent = r.u32();
      const int32_t text_len = r.i32();
      if (r.failed() || text_len < 0 || size_t(text_len) * 2 > r.remaining()) {
        return false;
      }
      ByteSpan wtext = r.read_span(size_t(text_len) * 2);
      const int32_t num_option = r.i32();
      if (r.failed() || num_option < 0 || num_option > MAX_TALK_OPTIONS ||
          size_t(num_option) * TALK_OPTION_BYTES > r.remaining())
      {
        return false;
      }
      if (!r_procs) {
        r.skip(size_t(num_option) * TALK_OPTION_BYTES);
        continue;
      }
      window.text = utf16le_to_utf8(wtext);
      for (int32_t o = 0; o < num_option; o++) {
        TalkOption option;
        option.id = r.u32();
        option.text = utf16le_to_utf8(r.read_span(TALK_TEXT_BYTES));
        option.param = r.u32();
        window.options.push_back(std::move(option));
      }
      proc.windows.push_back(std::move(window));
    }
    if (r.failed()) {
      return false;
    }
    if (r_procs) {
      r_procs->push_back(std::move(proc));
    }
  }
  r_count = uint32_t(count);
  return !r.failed();
}

bool read_text_file(const std::string &filepath, std::string &r_text)
{
  Bytes bytes;
  if (!read_file(filepath, bytes)) {
    return false;
  }
  r_text.assign(reinterpret_cast<const char *>(bytes.data()), bytes.size());
  return true;
}

fs::path to_fs_path(const std::string &utf8)
{
  return fs::path(std::u8string(reinterpret_cast<const char8_t *>(utf8.data()), utf8.size()));
}

std::string from_fs_path(const fs::path &path)
{
  const std::u8string s = path.u8string();
  return std::string(reinterpret_cast<const char *>(s.data()), s.size());
}

}  // namespace

/* -------------------------------------------------------------------- */
/** \name Text helpers
 * \{ */

std::string utf16le_to_utf8(ByteSpan bytes)
{
  std::string out;
  const size_t n = bytes.size() / 2;
  for (size_t i = 0; i < n; i++) {
    uint32_t cu = uint32_t(bytes[i * 2]) | (uint32_t(bytes[i * 2 + 1]) << 8);
    if (cu == 0) {
      break;
    }
    if (cu >= 0xD800 && cu <= 0xDBFF && i + 1 < n) {
      const uint32_t lo = uint32_t(bytes[i * 2 + 2]) | (uint32_t(bytes[i * 2 + 3]) << 8);
      if (lo >= 0xDC00 && lo <= 0xDFFF) {
        append_utf8(out, 0x10000 + ((cu - 0xD800) << 10) + (lo - 0xDC00));
        i++;
        continue;
      }
    }
    if (cu >= 0xD800 && cu <= 0xDFFF) {
      cu = 0xFFFD;
    }
    append_utf8(out, cu);
  }
  return out;
}

bool utf8_to_utf16le(std::string_view utf8, Bytes &r_out)
{
  r_out.clear();
  const auto put = [&](uint32_t cu) {
    r_out.push_back(uint8_t(cu & 0xFF));
    r_out.push_back(uint8_t(cu >> 8));
  };
  size_t i = 0;
  while (i < utf8.size()) {
    const uint8_t c = uint8_t(utf8[i]);
    uint32_t cp = 0;
    int extra = 0;
    if (c < 0x80) {
      cp = c;
    }
    else if ((c & 0xE0) == 0xC0) {
      cp = c & 0x1F;
      extra = 1;
    }
    else if ((c & 0xF0) == 0xE0) {
      cp = c & 0x0F;
      extra = 2;
    }
    else if ((c & 0xF8) == 0xF0) {
      cp = c & 0x07;
      extra = 3;
    }
    else {
      return false;
    }
    if (size_t(extra) > utf8.size() - i - 1) {
      return false;
    }
    for (int k = 1; k <= extra; k++) {
      const uint8_t cc = uint8_t(utf8[i + k]);
      if ((cc & 0xC0) != 0x80) {
        return false;
      }
      cp = (cp << 6) | (cc & 0x3F);
    }
    i += size_t(extra) + 1;
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
      return false;
    }
    if (cp >= 0x10000) {
      cp -= 0x10000;
      put(0xD800 + (cp >> 10));
      put(0xDC00 + (cp & 0x3FF));
    }
    else {
      put(cp);
    }
  }
  return true;
}

#ifdef _WIN32
bool utf8_to_gbk(std::string_view utf8, std::string &r_gbk)
{
  if (is_ascii(utf8)) {
    r_gbk.assign(utf8);
    return true;
  }
  const int wlen = MultiByteToWideChar(
      CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), int(utf8.size()), nullptr, 0);
  if (wlen <= 0) {
    return false;
  }
  std::wstring wide(size_t(wlen), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), wide.data(), wlen);
  BOOL used_default = FALSE;
  const int len = WideCharToMultiByte(
      936, 0, wide.data(), wlen, nullptr, 0, nullptr, &used_default);
  if (len <= 0 || used_default) {
    return false;
  }
  r_gbk.assign(size_t(len), '\0');
  WideCharToMultiByte(936, 0, wide.data(), wlen, r_gbk.data(), len, nullptr, nullptr);
  return true;
}
#else
bool utf8_to_gbk(std::string_view utf8, std::string &r_gbk)
{
  if (is_ascii(utf8)) {
    r_gbk.assign(utf8);
    return true;
  }
  iconv_t cd = iconv_open("GBK", "UTF-8");
  if (cd == iconv_t(-1)) {
    cd = iconv_open("GB18030", "UTF-8");
  }
  if (cd == iconv_t(-1)) {
    return false;
  }
  std::string in(utf8);
  char *in_ptr = in.data();
  size_t in_left = in.size();
  std::string out;
  char buf[256];
  bool ok = true;
  while (in_left > 0) {
    char *out_ptr = buf;
    size_t out_left = sizeof(buf);
    const size_t res = iconv(cd, &in_ptr, &in_left, &out_ptr, &out_left);
    out.append(buf, sizeof(buf) - out_left);
    if (res == size_t(-1)) {
      if (errno == E2BIG) {
        continue;
      }
      ok = false;
      break;
    }
  }
  iconv_close(cd);
  if (ok) {
    r_gbk = std::move(out);
  }
  return ok;
}
#endif

/** \} */

/* -------------------------------------------------------------------- */
/** \name Schema
 * \{ */

int ElementListDef::field_index(std::string_view field_name) const
{
  for (size_t i = 0; i < fields.size(); i++) {
    if (fields[i].name == field_name) {
      return int(i);
    }
  }
  return -1;
}

int ElementsSchema::list_index(std::string_view list_name) const
{
  for (size_t i = 0; i < lists.size(); i++) {
    if (lists[i].name == list_name || lists[i].label == list_name) {
      return int(i);
    }
  }
  return -1;
}

bool load_elements_schema(std::string_view cfg_text,
                          ElementsSchema &r_schema,
                          std::string *r_error)
{
  r_schema = ElementsSchema();
  std::vector<std::string_view> lines = split(cfg_text, '\n');
  size_t li = 0;
  const auto next_nonblank = [&]() -> std::string_view {
    while (li < lines.size() && trim(lines[li]).empty()) {
      li++;
    }
    return li < lines.size() ? trim(lines[li++]) : std::string_view();
  };
  /* Within a list block lines are read as they come: the names line of a list may be empty. */
  const auto next_line = [&]() -> std::string_view {
    return li < lines.size() ? trim(lines[li++]) : std::string_view();
  };

  uint64_t list_count = 0, talk_index = 0;
  if (!parse_uint(next_nonblank(), list_count) || list_count == 0 || list_count > 10000) {
    set_error(r_error, "elements schema: first line must be the list count");
    return false;
  }
  if (!parse_uint(next_nonblank(), talk_index)) {
    set_error(r_error, "elements schema: second line must be the talk-proc list index");
    return false;
  }

  std::vector<ElementListDef> lists;
  lists.reserve(list_count);
  for (uint64_t i = 0; i < list_count; i++) {
    const std::string_view label = next_nonblank();
    const std::string_view prefix = next_line();
    const std::string_view names_line = next_line();
    const std::string_view types_line = next_line();
    const std::string where = "elements schema: list " + std::to_string(i + 1) + " (" +
                              std::string(label) + ")";
    if (label.empty() || prefix.empty() || types_line.empty()) {
      set_error(r_error, where + " is incomplete");
      return false;
    }
    ElementListDef def;
    def.label = std::string(label);
    parse_label(label, def.label_number, def.name);
    def.id_space = id_space_of(def.name);
    uint64_t prefix_size = 0;
    if (prefix == "AUTO") {
      def.prefix_auto = true;
    }
    else if (parse_uint(prefix, prefix_size) && prefix_size <= (1u << 20)) {
      def.prefix_size = uint32_t(prefix_size);
    }
    else {
      set_error(r_error, where + ": invalid prefix '" + std::string(prefix) + "'");
      return false;
    }

    std::vector<std::string_view> names = split(names_line, ';');
    std::vector<std::string_view> types = split(types_line, ';');
    while (!types.empty() && trim(types.back()).empty()) {
      types.pop_back();
    }
    while (names.size() > types.size() && trim(names.back()).empty()) {
      names.pop_back();
    }
    if (names.size() != types.size()) {
      set_error(r_error,
                where + ": " + std::to_string(names.size()) + " field names but " +
                    std::to_string(types.size()) + " types");
      return false;
    }
    uint64_t offset = 0;
    for (size_t f = 0; f < types.size(); f++) {
      ElementFieldDef field;
      bool talk = false;
      if (!parse_field_type(types[f], field, talk)) {
        set_error(r_error, where + ": unknown field type '" + std::string(types[f]) + "'");
        return false;
      }
      if (talk) {
        if (types.size() != 1) {
          set_error(r_error, where + ": byte:AUTO must be the only field");
          return false;
        }
        def.is_talk_proc = true;
        def.id_space = ElementIdSpace::Talk;
        break;
      }
      field.name = std::string(trim(names[f]));
      field.offset = uint32_t(offset);
      offset += field.size;
      def.fields.push_back(std::move(field));
    }
    if (!def.is_talk_proc && (def.fields.empty() || offset > (1u << 24))) {
      set_error(r_error, where + ": invalid record layout");
      return false;
    }
    def.record_size = uint32_t(offset);
    lists.push_back(std::move(def));
  }

  /* The talk-proc list is the one given on the second line, which must be typed `byte:AUTO`;
   * fall back to the only `byte:AUTO` list when the index is off. */
  int talk = -1;
  if (talk_index < lists.size() && lists[talk_index].is_talk_proc) {
    talk = int(talk_index);
  }
  for (size_t i = 0; i < lists.size(); i++) {
    if (lists[i].is_talk_proc && int(i) != talk) {
      if (talk >= 0) {
        set_error(r_error, "elements schema: more than one talk-proc (byte:AUTO) list");
        return false;
      }
      talk = int(i);
    }
  }
  r_schema.lists = std::move(lists);
  r_schema.talk_proc_index = talk;
  return true;
}

bool load_elements_schema_file(const std::string &filepath,
                               ElementsSchema &r_schema,
                               std::string *r_error)
{
  std::string text;
  if (!read_text_file(filepath, text)) {
    set_error(r_error, "cannot read elements schema " + filepath);
    return false;
  }
  if (!load_elements_schema(text, r_schema, r_error)) {
    if (r_error) {
      *r_error = filepath + ": " + *r_error;
    }
    return false;
  }
  const std::string name = from_fs_path(to_fs_path(filepath).filename());
  r_schema.version = elements_schema_version_from_filename(name);
  r_schema.source_name = name;
  return true;
}

uint32_t elements_schema_version_from_filename(std::string_view filename)
{
  /* Digits after the last `_v`, ignoring variant suffixes such as `PW_1.5.7_v208_0.cfg`. */
  const size_t dot = filename.rfind('.');
  const std::string_view stem = filename.substr(0, dot);
  const size_t v = stem.rfind("_v");
  if (v == std::string_view::npos) {
    return 0;
  }
  size_t end = v + 2;
  while (end < stem.size() && stem[end] >= '0' && stem[end] <= '9') {
    end++;
  }
  uint64_t n = 0;
  if (!parse_uint(stem.substr(v + 2, end - v - 2), n) || n > 0xFFFF) {
    return 0;
  }
  return uint32_t(n);
}

std::vector<ElementsSchemaFile> list_elements_schemas(const std::string &folder)
{
  std::vector<ElementsSchemaFile> out;
  std::error_code ec;
  for (fs::directory_iterator it(to_fs_path(folder), ec), end; !ec && it != end; it.increment(ec))
  {
    if (!it->is_regular_file(ec)) {
      continue;
    }
    const std::string name = from_fs_path(it->path().filename());
    if (name.size() < 4 || !iequals(std::string_view(name).substr(name.size() - 4), ".cfg")) {
      continue;
    }
    const uint32_t version = elements_schema_version_from_filename(name);
    if (version != 0) {
      out.push_back({from_fs_path(it->path()), version});
    }
  }
  std::sort(out.begin(), out.end(), [](const auto &a, const auto &b) {
    return a.version != b.version ? a.version < b.version : a.path < b.path;
  });
  return out;
}

bool load_elements_schema_for_version(const std::string &folder,
                                      const uint32_t version,
                                      ElementsSchema &r_schema,
                                      std::string *r_error)
{
  const std::vector<ElementsSchemaFile> files = list_elements_schemas(folder);
  for (const ElementsSchemaFile &file : files) {
    if (file.version == version) {
      return load_elements_schema_file(file.path, r_schema, r_error);
    }
  }
  std::string available;
  for (const ElementsSchemaFile &file : files) {
    available += (available.empty() ? "" : ", ") + std::to_string(file.version);
  }
  set_error(r_error,
            "no elements schema for version " + std::to_string(version) + " in " + folder +
                " (available: " + (available.empty() ? "none" : available) + ")");
  return false;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Parse / write
 * \{ */

bool elements_version_is_packed(const uint32_t version)
{
  return version >= 350 && version != 1488 && version < 30000;
}

bool elements_version_has_list_headers(const uint32_t version)
{
  return version >= 191 && version <= 349;
}

bool peek_elements_version(ByteSpan data, uint32_t &r_version, std::string *r_error)
{
  if (data.size() < 8) {
    set_error(r_error, "elements.data: file too small");
    return false;
  }
  r_version = load_le<uint16_t>(data.data());
  if (r_version == 0) {
    set_error(r_error, "elements.data: invalid version 0");
    return false;
  }
  return true;
}

bool parse_elements(ByteSpan data,
                    const ElementsSchema &schema,
                    ElementsData &r_data,
                    std::string *r_error)
{
  r_data = ElementsData();
  uint32_t version = 0;
  if (!peek_elements_version(data, version, r_error)) {
    return false;
  }
  if (schema.version != 0 && schema.version != version) {
    set_error(r_error,
              "elements.data: file version " + std::to_string(version) +
                  " does not match schema version " + std::to_string(schema.version));
    return false;
  }
  if (schema.lists.empty()) {
    set_error(r_error, "elements.data: empty schema");
    return false;
  }
  if (elements_version_is_packed(version)) {
    set_error(r_error,
              "elements.data: version " + std::to_string(version) +
                  " stores compressed lists, which are not supported");
    return false;
  }
  const bool list_headers = elements_version_has_list_headers(version);

  BinaryReader r(data);
  r_data.header = r.u32();
  r_data.lists.resize(schema.lists.size());
  for (size_t i = 0; i < schema.lists.size(); i++) {
    const ElementListDef &def = schema.lists[i];
    ElementsList &list = r_data.lists[i];
    list.def_index = int(i);
    list.record_size = def.record_size;
    const std::string where = "elements.data: list " + std::to_string(i) + " " + def.name;

    if (def.prefix_auto) {
      if (!read_auto_prefix(r, i, def.name, list.prefix, r_error)) {
        return false;
      }
    }
    else if (def.prefix_size > 0) {
      const ByteSpan prefix = r.read_span(def.prefix_size);
      if (r.failed()) {
        set_error(r_error, where + ": truncated prefix");
        return false;
      }
      list.prefix.assign(prefix.begin(), prefix.end());
    }

    if (def.is_talk_proc) {
      const size_t start = r.tell();
      if (!scan_talk_procs(r, list.raw_count, nullptr)) {
        set_error(r_error, where + ": malformed talk procs");
        return false;
      }
      list.raw.assign(data.begin() + start, data.begin() + r.tell());
      continue;
    }

    if (list_headers) {
      list.list_type = r.i32();
    }
    const int32_t count = r.i32();
    if (r.failed() || count < 0) {
      set_error(r_error, where + ": invalid record count");
      return false;
    }
    if (list_headers) {
      const uint32_t item_size = r.u32();
      if (r.failed() || item_size != def.record_size) {
        set_error(r_error,
                  where + ": record size " + std::to_string(item_size) + " in the file, " +
                      std::to_string(def.record_size) + " in the schema");
        return false;
      }
    }
    const uint64_t bytes = uint64_t(count) * def.record_size;
    if (bytes > r.remaining()) {
      set_error(r_error,
                where + ": " + std::to_string(count) + " records of " +
                    std::to_string(def.record_size) +
                    " bytes exceed the file (schema does not match the file)");
      return false;
    }
    const ByteSpan records = r.read_span(size_t(bytes));
    list.records.assign(records.begin(), records.end());
  }

  const size_t left = r.remaining();
  if (left > ELEMENTS_MAX_TRAILING) {
    set_error(r_error,
              "elements.data: " + std::to_string(left) +
                  " bytes left after the last list (schema does not match the file)");
    return false;
  }
  const ByteSpan rest = r.read_span(left);
  r_data.trailing.assign(rest.begin(), rest.end());
  r_data.schema = std::make_shared<const ElementsSchema>(schema);
  return true;
}

bool write_elements(const ElementsData &data,
                    const ElementsSchema &schema,
                    Bytes &r_out,
                    std::string *r_error)
{
  r_out.clear();
  if (data.lists.size() != schema.lists.size()) {
    set_error(r_error, "write elements: data has a different list count than the schema");
    return false;
  }
  const uint32_t version = data.header & 0xFFFF;
  if (elements_version_is_packed(version)) {
    set_error(r_error, "write elements: compressed versions are not supported");
    return false;
  }
  const bool list_headers = elements_version_has_list_headers(version);
  size_t total = 4 + data.trailing.size();
  for (const ElementsList &list : data.lists) {
    total += list.prefix.size() + 12 + list.records.size() + list.raw.size();
  }
  r_out.reserve(total);
  put_u32(r_out, data.header);
  for (size_t i = 0; i < schema.lists.size(); i++) {
    const ElementListDef &def = schema.lists[i];
    const ElementsList &list = data.lists[i];
    if (!def.prefix_auto && list.prefix.size() != def.prefix_size) {
      set_error(r_error, "write elements: list " + def.name + " has a prefix of the wrong size");
      return false;
    }
    r_out.insert(r_out.end(), list.prefix.begin(), list.prefix.end());
    if (def.is_talk_proc) {
      if (list.raw.size() < 4) {
        set_error(r_error, "write elements: talk-proc list without data");
        return false;
      }
      r_out.insert(r_out.end(), list.raw.begin(), list.raw.end());
      continue;
    }
    if (list.record_size != def.record_size || list.records.size() % def.record_size != 0 ||
        list.records.size() / def.record_size > size_t(std::numeric_limits<int32_t>::max()))
    {
      set_error(r_error, "write elements: list " + def.name + " does not match the schema layout");
      return false;
    }
    if (list_headers) {
      put_u32(r_out, uint32_t(list.list_type));
    }
    put_u32(r_out, uint32_t(list.records.size() / def.record_size));
    if (list_headers) {
      put_u32(r_out, def.record_size);
    }
    r_out.insert(r_out.end(), list.records.begin(), list.records.end());
  }
  r_out.insert(r_out.end(), data.trailing.begin(), data.trailing.end());
  return true;
}

bool parse_talk_procs(const ElementsList &list,
                      std::vector<TalkProc> &r_procs,
                      std::string *r_error)
{
  r_procs.clear();
  BinaryReader r(list.raw);
  uint32_t count = 0;
  if (!scan_talk_procs(r, count, &r_procs) || !r.at_end()) {
    r_procs.clear();
    set_error(r_error, "malformed talk-proc list");
    return false;
  }
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Record access and editing
 * \{ */

const ElementListDef *ElementsData::list_def(const int list) const
{
  if (!schema || list < 0 || size_t(list) >= lists.size() || size_t(list) >= schema->lists.size())
  {
    return nullptr;
  }
  return &schema->lists[size_t(list)];
}

int ElementsData::list_index(std::string_view list_name) const
{
  return schema ? schema->list_index(list_name) : -1;
}

uint32_t ElementsData::record_id(const int list, const size_t record) const
{
  const ElementListDef *def = list_def(list);
  if (!def || def->is_talk_proc || record >= lists[list].size()) {
    return 0;
  }
  const ElementFieldDef &f = def->fields[0];
  const uint8_t *p = lists[list].records.data() + record * def->record_size;
  switch (f.type) {
    case ElementFieldType::Int32:
    case ElementFieldType::UInt32:
      return load_le<uint32_t>(p);
    case ElementFieldType::Int16:
      return load_le<uint16_t>(p);
    default:
      return 0;
  }
}

ElementRef ElementsData::find(const int list, const uint32_t id) const
{
  const ElementListDef *def = list_def(list);
  if (!def || def->is_talk_proc) {
    return {};
  }
  const size_t n = lists[list].size();
  for (size_t i = 0; i < n; i++) {
    if (record_id(list, i) == id) {
      return {list, i};
    }
  }
  return {};
}

ElementRef ElementsData::find(std::string_view list_name, const uint32_t id) const
{
  return find(list_index(list_name), id);
}

ElementRef ElementsData::find_in_space(const ElementIdSpace space, const uint32_t id) const
{
  for (size_t i = 0; i < lists.size(); i++) {
    const ElementListDef *def = list_def(int(i));
    if (def && def->id_space == space) {
      const ElementRef ref = find(int(i), id);
      if (ref.valid()) {
        return ref;
      }
    }
  }
  return {};
}

bool ElementsData::get(const int list,
                       const size_t record,
                       const int field,
                       ElementValue &r_value) const
{
  const ElementListDef *def = list_def(list);
  if (!def || def->is_talk_proc || field < 0 || size_t(field) >= def->fields.size() ||
      record >= lists[list].size())
  {
    return false;
  }
  const ElementFieldDef &f = def->fields[size_t(field)];
  r_value = decode_field(f, lists[list].records.data() + record * def->record_size + f.offset);
  return true;
}

bool ElementsData::get(const ElementRef ref,
                       std::string_view field_name,
                       ElementValue &r_value) const
{
  const ElementListDef *def = list_def(ref.list);
  return def && get(ref.list, ref.record, def->field_index(field_name), r_value);
}

int64_t ElementsData::get_int(const ElementRef ref,
                              std::string_view field_name,
                              const int64_t fallback) const
{
  ElementValue v;
  if (get(ref, field_name, v)) {
    if (const int64_t *i = std::get_if<int64_t>(&v)) {
      return *i;
    }
  }
  return fallback;
}

double ElementsData::get_float(const ElementRef ref,
                               std::string_view field_name,
                               const double fallback) const
{
  ElementValue v;
  if (get(ref, field_name, v)) {
    if (const double *d = std::get_if<double>(&v)) {
      return *d;
    }
    if (const int64_t *i = std::get_if<int64_t>(&v)) {
      return double(*i);
    }
  }
  return fallback;
}

std::string ElementsData::get_string(const ElementRef ref, std::string_view field_name) const
{
  ElementValue v;
  if (get(ref, field_name, v)) {
    if (std::string *s = std::get_if<std::string>(&v)) {
      return std::move(*s);
    }
  }
  return {};
}

bool ElementsData::decode_record(const int list,
                                 const size_t record,
                                 std::vector<ElementValue> &r_values) const
{
  r_values.clear();
  const ElementListDef *def = list_def(list);
  if (!def || def->is_talk_proc || record >= lists[list].size()) {
    return false;
  }
  r_values.reserve(def->fields.size());
  const uint8_t *base = lists[list].records.data() + record * def->record_size;
  for (const ElementFieldDef &f : def->fields) {
    r_values.push_back(decode_field(f, base + f.offset));
  }
  return true;
}

bool ElementsData::set(const int list,
                       const size_t record,
                       const int field,
                       const ElementValue &value,
                       std::string *r_error)
{
  const ElementListDef *def = list_def(list);
  if (!def || def->is_talk_proc || field < 0 || size_t(field) >= def->fields.size() ||
      record >= lists[list].size())
  {
    set_error(r_error, "set field: list, record or field out of range");
    return false;
  }
  const ElementFieldDef &f = def->fields[size_t(field)];
  uint8_t *p = lists[list].records.data() + record * def->record_size + f.offset;
  const std::string where = "set " + def->name + "." + f.name + ": ";
  const int64_t *as_int = std::get_if<int64_t>(&value);
  const double *as_double = std::get_if<double>(&value);
  const std::string *as_string = std::get_if<std::string>(&value);
  const Bytes *as_bytes = std::get_if<Bytes>(&value);

  const auto check_range = [&](int64_t lo, int64_t hi) {
    if (!as_int) {
      set_error(r_error, where + "expects an integer");
      return false;
    }
    if (*as_int < lo || *as_int > hi) {
      set_error(r_error, where + "value " + std::to_string(*as_int) + " out of range");
      return false;
    }
    return true;
  };

  switch (f.type) {
    case ElementFieldType::Int16:
      if (!check_range(std::numeric_limits<int16_t>::min(), std::numeric_limits<uint16_t>::max()))
      {
        return false;
      }
      store_le(p, uint16_t(*as_int));
      return true;
    case ElementFieldType::Int32:
    case ElementFieldType::UInt32:
      if (!check_range(std::numeric_limits<int32_t>::min(), std::numeric_limits<uint32_t>::max()))
      {
        return false;
      }
      store_le(p, uint32_t(*as_int));
      return true;
    case ElementFieldType::Int64:
      if (!as_int) {
        set_error(r_error, where + "expects an integer");
        return false;
      }
      store_le(p, *as_int);
      return true;
    case ElementFieldType::Float:
    case ElementFieldType::Double: {
      double d = 0.0;
      if (as_double) {
        d = *as_double;
      }
      else if (as_int) {
        d = double(*as_int);
      }
      else {
        set_error(r_error, where + "expects a number");
        return false;
      }
      if (f.type == ElementFieldType::Float) {
        store_le(p, float(d));
      }
      else {
        store_le(p, d);
      }
      return true;
    }
    case ElementFieldType::Raw:
      if (!as_bytes || as_bytes->size() != f.size) {
        set_error(r_error, where + "expects exactly " + std::to_string(f.size) + " bytes");
        return false;
      }
      std::memcpy(p, as_bytes->data(), f.size);
      return true;
    case ElementFieldType::WString:
    case ElementFieldType::String: {
      if (!as_string) {
        set_error(r_error, where + "expects text");
        return false;
      }
      Bytes encoded;
      if (f.type == ElementFieldType::WString) {
        if (!utf8_to_utf16le(*as_string, encoded)) {
          set_error(r_error, where + "invalid UTF-8");
          return false;
        }
      }
      else {
        std::string gbk;
        if (!utf8_to_gbk(*as_string, gbk)) {
          set_error(r_error, where + "text cannot be encoded as GBK");
          return false;
        }
        encoded.assign(gbk.begin(), gbk.end());
      }
      if (encoded.size() > f.size) {
        set_error(r_error,
                  where + "text needs " + std::to_string(encoded.size()) + " bytes, field has " +
                      std::to_string(f.size));
        return false;
      }
      std::memset(p, 0, f.size);
      if (!encoded.empty()) {
        std::memcpy(p, encoded.data(), encoded.size());
      }
      return true;
    }
  }
  return false;
}

bool ElementsData::set(const ElementRef ref,
                       std::string_view field_name,
                       const ElementValue &value,
                       std::string *r_error)
{
  const ElementListDef *def = list_def(ref.list);
  if (!def) {
    set_error(r_error, "set field: invalid list");
    return false;
  }
  const int field = def->field_index(field_name);
  if (field < 0) {
    set_error(r_error, "set field: " + def->name + " has no field " + std::string(field_name));
    return false;
  }
  return set(ref.list, ref.record, field, value, r_error);
}

int64_t ElementsData::add_record(const int list,
                                 const uint32_t id,
                                 const std::optional<size_t> copy_from)
{
  const ElementListDef *def = list_def(list);
  if (!def || def->is_talk_proc) {
    return -1;
  }
  ElementsList &l = lists[list];
  const size_t count = l.size();
  if (count >= size_t(std::numeric_limits<int32_t>::max())) {
    return -1;
  }
  if (copy_from && *copy_from >= count) {
    return -1;
  }
  const size_t at = l.records.size();
  l.records.resize(at + def->record_size, 0);
  if (copy_from) {
    std::memcpy(
        l.records.data() + at, l.records.data() + *copy_from * def->record_size, def->record_size);
  }
  if (id != 0) {
    const ElementFieldDef &f = def->fields[0];
    if (f.type == ElementFieldType::Int32 || f.type == ElementFieldType::UInt32) {
      store_le(l.records.data() + at, id);
    }
    else if (f.type == ElementFieldType::Int16) {
      store_le(l.records.data() + at, uint16_t(id));
    }
  }
  return int64_t(count);
}

bool ElementsData::remove_record(const int list, const size_t record)
{
  const ElementListDef *def = list_def(list);
  if (!def || def->is_talk_proc || record >= lists[list].size()) {
    return false;
  }
  Bytes &records = lists[list].records;
  const auto first = records.begin() + ptrdiff_t(record * def->record_size);
  records.erase(first, first + def->record_size);
  return true;
}

uint32_t ElementsData::max_id(const int list) const
{
  uint32_t best = 0;
  const ElementListDef *def = list_def(list);
  if (!def || def->is_talk_proc) {
    return 0;
  }
  for (size_t i = 0; i < lists[list].size(); i++) {
    best = std::max(best, record_id(list, i));
  }
  return best;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Model info
 * \{ */

bool element_model_info(const ElementsData &data, const ElementRef ref, ElementModelInfo &r_info)
{
  r_info = ElementModelInfo();
  const ElementListDef *def = data.list_def(ref.list);
  if (!def || def->is_talk_proc || ref.record >= data.lists[ref.list].size()) {
    return false;
  }
  const int model_field = def->field_index("file_model");
  if (model_field < 0) {
    return false;
  }
  r_info.ref = ref;
  r_info.list_name = def->name;
  r_info.id = data.record_id(ref.list, ref.record);
  r_info.file_model = data.get_string(ref, "file_model");
  for (const ElementFieldDef &f : def->fields) {
    if (iequals(f.name, "name") &&
        (f.type == ElementFieldType::WString || f.type == ElementFieldType::String))
    {
      r_info.name = data.get_string(ref, f.name);
      break;
    }
  }
  const int size_field = def->field_index("size");
  if (size_field >= 0) {
    r_info.size = float(data.get_float(ref, "size"));
    r_info.has_size = true;
  }
  else if (def->field_index("id_src_monster") >= 0) {
    /* Service NPCs take their body size from their source monster (EC_NPCServer.cpp). */
    const int64_t src = data.get_int(ref, "id_src_monster");
    if (src > 0) {
      const ElementRef monster = data.find("MONSTER_ESSENCE", uint32_t(src));
      if (monster.valid() && data.list_def(monster.list)->field_index("size") >= 0) {
        r_info.size = float(data.get_float(monster, "size"));
        r_info.has_size = true;
      }
    }
  }
  return true;
}

bool find_element_model(const ElementsData &data, const uint32_t id, ElementModelInfo &r_info)
{
  for (size_t i = 0; i < data.lists.size(); i++) {
    const ElementListDef *def = data.list_def(int(i));
    if (!def || def->id_space != ElementIdSpace::Essence || def->field_index("file_model") < 0) {
      continue;
    }
    const ElementRef ref = data.find(int(i), id);
    if (ref.valid()) {
      return element_model_info(data, ref, r_info);
    }
  }
  r_info = ElementModelInfo();
  return false;
}

std::vector<ElementModelInfo> collect_element_models(const ElementsData &data)
{
  std::vector<ElementModelInfo> out;
  for (size_t i = 0; i < data.lists.size(); i++) {
    const ElementListDef *def = data.list_def(int(i));
    if (!def || def->is_talk_proc || def->field_index("file_model") < 0) {
      continue;
    }
    for (size_t k = 0; k < data.lists[i].size(); k++) {
      ElementModelInfo info;
      if (element_model_info(data, {int(i), k}, info)) {
        out.push_back(std::move(info));
      }
    }
  }
  return out;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Safe saving
 * \{ */

bool save_file_with_backup(const std::string &filepath, ByteSpan data, std::string *r_error)
{
  const fs::path path = to_fs_path(filepath);
  fs::path tmp = path;
  tmp += ".tmp";
  fs::path bak = path;
  bak += ".bak";
  std::error_code ec;
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
      set_error(r_error, "cannot create " + from_fs_path(tmp));
      return false;
    }
    out.write(reinterpret_cast<const char *>(data.data()), std::streamsize(data.size()));
    out.flush();
    if (!out) {
      out.close();
      fs::remove(tmp, ec);
      set_error(r_error, "cannot write " + from_fs_path(tmp));
      return false;
    }
  }
  if (fs::exists(path, ec) && !fs::exists(bak, ec)) {
    if (!fs::copy_file(path, bak, ec) || ec) {
      fs::remove(tmp, ec);
      set_error(r_error, "cannot create backup " + from_fs_path(bak));
      return false;
    }
  }
  fs::rename(tmp, path, ec);
  if (ec) {
    std::error_code ec2;
    fs::remove(tmp, ec2);
    set_error(r_error, "cannot replace " + filepath + ": " + ec.message());
    return false;
  }
  return true;
}

/** \} */

}  // namespace blender::io::angelica
