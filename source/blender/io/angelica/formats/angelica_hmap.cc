/* SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup angelica
 */

#include "angelica_hmap.hh"

#include <cmath>
#include <filesystem>
#include <map>
#include <optional>

namespace blender::io::angelica {

namespace {

constexpr int MAX_HMAP_SIZE = 8193;
constexpr int MAX_AREAS = 65536;
constexpr int64_t MAX_MAP_VERTICES = int64_t(1) << 28;
constexpr uintmax_t MAX_CONFIG_FILE_SIZE = 4 * 1024 * 1024;

bool fail(std::string *r_error, std::string msg)
{
  if (r_error) {
    *r_error = std::move(msg);
  }
  return false;
}

std::filesystem::path u8path(const std::string &s)
{
  return std::filesystem::path(
      std::u8string(reinterpret_cast<const char8_t *>(s.data()), s.size()));
}

std::string path_to_utf8(const std::filesystem::path &p)
{
  const std::u8string s = p.u8string();
  return std::string(reinterpret_cast<const char *>(s.data()), s.size());
}

std::string ascii_lower(std::string s)
{
  for (char &c : s) {
    if (c >= 'A' && c <= 'Z') {
      c = char(c - 'A' + 'a');
    }
  }
  return s;
}

/** Lower-case key with all white space removed, so `Map Width` and `mapwidth` compare equal. */
std::string squash_key(const std::string_view key)
{
  std::string out;
  for (const char c : key) {
    if (c != ' ' && c != '\t') {
      out.push_back(c);
    }
  }
  return ascii_lower(out);
}

std::string_view trim(std::string_view s)
{
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) {
    s.remove_prefix(1);
  }
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
    s.remove_suffix(1);
  }
  return s;
}

std::optional<double> parse_number(std::string_view s)
{
  s = trim(s);
  /* Accept C float suffixes like `2.0f`. */
  while (!s.empty() && (s.back() == 'f' || s.back() == 'F')) {
    s.remove_suffix(1);
  }
  if (s.empty()) {
    return std::nullopt;
  }
  const std::string str(s);
  char *end = nullptr;
  const double v = std::strtod(str.c_str(), &end);
  if (end == str.c_str() || !std::isfinite(v)) {
    return std::nullopt;
  }
  return v;
}

/** `key = value` pairs grouped by `[section]`; pairs before any section go to "". */
using ConfigSections = std::vector<std::pair<std::string, std::map<std::string, std::string>>>;

bool read_config(const std::filesystem::path &path, ConfigSections &r_sections)
{
  std::error_code ec;
  const uintmax_t size = std::filesystem::file_size(path, ec);
  if (ec || size > MAX_CONFIG_FILE_SIZE) {
    return false;
  }
  Bytes data;
  if (!read_file(path_to_utf8(path), data)) {
    return false;
  }
  r_sections.clear();
  r_sections.emplace_back("", std::map<std::string, std::string>());
  const std::string_view text(reinterpret_cast<const char *>(data.data()), data.size());
  size_t pos = 0;
  while (pos < text.size()) {
    size_t eol = text.find('\n', pos);
    if (eol == std::string_view::npos) {
      eol = text.size();
    }
    std::string_view line = trim(text.substr(pos, eol - pos));
    pos = eol + 1;
    if (const size_t comment = line.find_first_of("#;"); comment != std::string_view::npos) {
      line = trim(line.substr(0, comment));
    }
    if (line.empty()) {
      continue;
    }
    if (line.front() == '[' && line.back() == ']') {
      r_sections.emplace_back(std::string(trim(line.substr(1, line.size() - 2))),
                              std::map<std::string, std::string>());
      continue;
    }
    const size_t eq = line.find('=');
    if (eq == std::string_view::npos) {
      continue;
    }
    r_sections.back().second[squash_key(line.substr(0, eq))] = std::string(
        trim(line.substr(eq + 1)));
  }
  return true;
}

std::optional<double> config_number(const std::map<std::string, std::string> &kv, const char *key)
{
  const auto it = kv.find(key);
  if (it == kv.end()) {
    return std::nullopt;
  }
  return parse_number(it->second);
}

/** Find a file by name in a directory ignoring ASCII case. */
std::optional<std::filesystem::path> find_ci(const std::filesystem::path &dir,
                                             const std::string &name)
{
  std::error_code ec;
  if (std::filesystem::is_regular_file(dir / u8path(name), ec)) {
    return dir / u8path(name);
  }
  const std::string wanted = ascii_lower(name);
  for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    if (ascii_lower(path_to_utf8(it->path().filename())) == wanted && it->is_regular_file(ec)) {
      return it->path();
    }
  }
  return std::nullopt;
}

struct DetectedLayout {
  int cols = 0;
  int rows = 0;
  float grid_size = 0.0f;
  float submap_width = 0.0f;
  bool has_height_range = false;
  float height_min = 0.0f;
  float height_max = 0.0f;
  std::string layout_source;
  std::string height_source;
};

/** `movemap.conf`: `Map Width = cols`, `Map Length = rows`, `Submap Width = meters`. */
void detect_from_movemap(const std::filesystem::path &dir, DetectedLayout &r_layout)
{
  const std::filesystem::path candidates[] = {dir, dir / "..", dir / ".." / "movemap"};
  for (const std::filesystem::path &cand_dir : candidates) {
    const std::optional<std::filesystem::path> path = find_ci(cand_dir, "movemap.conf");
    ConfigSections sections;
    if (!path || !read_config(*path, sections)) {
      continue;
    }
    const auto &kv = sections.front().second;
    const std::optional<double> w = config_number(kv, "mapwidth");
    const std::optional<double> l = config_number(kv, "maplength");
    if (!w || !l || *w < 1 || *l < 1 || *w * *l > MAX_AREAS) {
      continue;
    }
    r_layout.cols = int(*w);
    r_layout.rows = int(*l);
    if (const std::optional<double> sw = config_number(kv, "submapwidth"); sw && *sw > 0) {
      r_layout.submap_width = float(*sw);
    }
    r_layout.layout_source = path_to_utf8(path->lexically_normal());
    return;
  }
}

/**
 * Server terrain config (`[Terrain_*]` section of `gs.conf` or a stand-alone file): used when
 * exactly one section describes a map with `num_files` areas.
 */
void detect_from_terrain_config(const std::filesystem::path &dir,
                                const int num_files,
                                DetectedLayout &r_layout)
{
  for (const std::filesystem::path &cand_dir : {dir, dir / ".."}) {
    std::error_code ec;
    for (std::filesystem::directory_iterator it(cand_dir, ec), end; !ec && it != end;
         it.increment(ec))
    {
      const std::string name = ascii_lower(path_to_utf8(it->path().filename()));
      if (name.size() < 5 || name.substr(name.size() - 5) != ".conf" || name == "movemap.conf") {
        continue;
      }
      ConfigSections sections;
      if (!it->is_regular_file(ec) || !read_config(it->path(), sections)) {
        continue;
      }
      const std::map<std::string, std::string> *match = nullptr;
      int matches = 0;
      for (const auto &[section, kv] : sections) {
        const std::optional<double> cols = config_number(kv, "nnumcols");
        const std::optional<double> rows = config_number(kv, "nnumrows");
        if (!cols || !rows || *cols < 1 || *rows < 1) {
          continue;
        }
        if (int64_t(*cols) * int64_t(*rows) > num_files) {
          continue;
        }
        match = &kv;
        matches++;
      }
      if (matches != 1) {
        continue;
      }
      const auto &kv = *match;
      if (r_layout.cols == 0) {
        r_layout.cols = int(*config_number(kv, "nnumcols"));
        r_layout.rows = int(*config_number(kv, "nnumrows"));
        r_layout.layout_source = path_to_utf8(it->path().lexically_normal());
      }
      if (const std::optional<double> g = config_number(kv, "vgridsize"); g && *g > 0) {
        r_layout.grid_size = float(*g);
      }
      const std::optional<double> hmin = config_number(kv, "vheightmin");
      const std::optional<double> hmax = config_number(kv, "vheightmax");
      if (hmin && hmax) {
        r_layout.has_height_range = true;
        r_layout.height_min = float(*hmin);
        r_layout.height_max = float(*hmax);
        r_layout.height_source = path_to_utf8(it->path().lexically_normal());
      }
      return;
    }
  }
}

}  // namespace

bool parse_hmap(ByteSpan data, HeightMap &r_map, std::string *r_error)
{
  r_map = HeightMap();
  if (data.empty() || data.size() % 4 != 0) {
    return fail(r_error, ".hmap size is not a multiple of 4 bytes");
  }
  const size_t count = data.size() / 4;
  const int n = int(std::llround(std::sqrt(double(count))));
  if (n < 2 || n > MAX_HMAP_SIZE || size_t(n) * size_t(n) != count) {
    return fail(r_error, ".hmap does not contain a square grid of floats");
  }
  r_map.size = n;
  r_map.values.resize(count);
  std::memcpy(r_map.values.data(), data.data(), data.size());
  for (float &v : r_map.values) {
    if (!std::isfinite(v)) {
      v = 0.0f;
    }
  }
  return true;
}

bool load_hmap_folder(const std::string &dir,
                      HmapMap &r_map,
                      std::string *r_error,
                      const HmapLoadOptions &options)
{
  r_map = HmapMap();
  const std::filesystem::path base = dir.empty() ? std::filesystem::path(".") : u8path(dir);

  /* Index the `<n>.hmap` files. */
  std::map<int, std::filesystem::path> files;
  {
    std::error_code ec;
    std::filesystem::directory_iterator it(base, ec);
    if (ec) {
      return fail(r_error, "Cannot list \"" + dir + "\"");
    }
    for (const std::filesystem::directory_iterator end; !ec && it != end; it.increment(ec)) {
      const std::string name = ascii_lower(path_to_utf8(it->path().filename()));
      if (name.size() <= 5 || name.substr(name.size() - 5) != ".hmap") {
        continue;
      }
      const std::string stem = name.substr(0, name.size() - 5);
      if (stem.size() > 6 || stem.find_first_not_of("0123456789") != std::string::npos) {
        continue;
      }
      const int index = std::stoi(stem);
      if (index >= 1 && index <= MAX_AREAS) {
        files[index] = it->path();
      }
    }
  }
  if (files.empty()) {
    return fail(r_error, "No .hmap files found in \"" + dir + "\"");
  }
  /* Areas are numbered 1..N without gaps. */
  int num_files = 0;
  while (files.count(num_files + 1)) {
    num_files++;
  }

  DetectedLayout layout;
  if (options.cols > 0 && options.rows > 0) {
    layout.cols = options.cols;
    layout.rows = options.rows;
  }
  else {
    detect_from_movemap(base, layout);
  }
  detect_from_terrain_config(base, num_files, layout);
  if (layout.cols == 0) {
    const int side = int(std::llround(std::sqrt(double(num_files))));
    if (side * side != num_files) {
      return fail(r_error,
                  "Cannot infer the area layout of " + std::to_string(num_files) +
                      " .hmap files (no movemap.conf and not a square count)");
    }
    layout.cols = side;
    layout.rows = side;
  }
  if (int64_t(layout.cols) * layout.rows > num_files) {
    return fail(r_error,
                "Area layout " + std::to_string(layout.cols) + "x" + std::to_string(layout.rows) +
                    " needs " + std::to_string(layout.cols * layout.rows) +
                    " .hmap files, found " + std::to_string(num_files) + " (1.hmap .. " +
                    std::to_string(num_files) + ".hmap)");
  }

  HmapMap &map = r_map;
  map.cols = layout.cols;
  map.rows = layout.rows;
  map.layout_source = layout.layout_source;

  HeightMap area;
  std::string err;
  for (int row = 0; row < map.rows; row++) {
    for (int col = 0; col < map.cols; col++) {
      const int file_index = row * map.cols + col + 1;
      const std::string path = path_to_utf8(files[file_index]);
      Bytes data;
      if (!read_file(path, data)) {
        return fail(r_error, "Cannot read \"" + path + "\"");
      }
      if (!parse_hmap(data, area, &err)) {
        return fail(r_error, "\"" + path + "\": " + err);
      }
      if (map.area_size == 0) {
        map.area_size = area.size - 1;
        const int64_t vx = int64_t(map.cols) * map.area_size + 1;
        const int64_t vz = int64_t(map.rows) * map.area_size + 1;
        if (vx * vz > MAX_MAP_VERTICES) {
          return fail(r_error, "Height map is too large");
        }
        map.verts_x = int(vx);
        map.verts_z = int(vz);
        map.values.assign(size_t(vx) * size_t(vz), 0.0f);
      }
      else if (area.size != map.area_size + 1) {
        return fail(r_error,
                    "\"" + path + "\" is " + std::to_string(area.size) + "x" +
                        std::to_string(area.size) + ", other areas are " +
                        std::to_string(map.area_size + 1));
      }
      /* Shared edges: later areas overwrite, like the server does. */
      const size_t n = size_t(area.size);
      for (size_t i = 0; i < n; i++) {
        const size_t dst = (size_t(row) * map.area_size + i) * size_t(map.verts_x) +
                           size_t(col) * map.area_size;
        std::memcpy(&map.values[dst], &area.values[i * n], n * sizeof(float));
      }
    }
  }

  if (options.grid_size > 0.0f) {
    map.grid_size = options.grid_size;
  }
  else if (layout.grid_size > 0.0f) {
    map.grid_size = layout.grid_size;
  }
  else if (layout.submap_width > 0.0f && map.area_size > 0) {
    map.grid_size = layout.submap_width / float(map.area_size);
  }

  if (options.has_height_range) {
    map.height_min = options.height_min;
    map.height_max = options.height_max;
  }
  else if (layout.has_height_range) {
    map.height_min = layout.height_min;
    map.height_max = layout.height_max;
    map.height_range_source = layout.height_source;
  }
  return true;
}

}  // namespace blender::io::angelica
