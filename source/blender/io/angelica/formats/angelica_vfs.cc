/* SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup angelica
 */

#include "angelica_vfs.hh"
#include "angelica_pck.hh"

#include <algorithm>
#include <filesystem>
#include <system_error>

namespace blender::io::angelica {

namespace fs = std::filesystem;

static fs::path to_fs_path(std::string_view utf8)
{
  return fs::path(std::u8string(reinterpret_cast<const char8_t *>(utf8.data()), utf8.size()));
}

static std::string from_fs_path(const fs::path &path)
{
  const std::u8string s = path.u8string();
  return std::string(reinterpret_cast<const char *>(s.data()), s.size());
}

static char lower_ascii(const char c)
{
  return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c;
}

static bool equals_nocase(std::string_view a, std::string_view b)
{
  if (a.size() != b.size()) {
    return false;
  }
  for (size_t i = 0; i < a.size(); i++) {
    if (lower_ascii(a[i]) != lower_ascii(b[i])) {
      return false;
    }
  }
  return true;
}

static bool is_separator(const char c)
{
  return c == '/' || c == '\\';
}

std::string path_join(std::string_view a, std::string_view b)
{
  if (a.empty()) {
    return std::string(b);
  }
  std::string out(a);
  if (!is_separator(out.back())) {
    out.push_back('/');
  }
  size_t start = 0;
  while (start < b.size() && is_separator(b[start])) {
    start++;
  }
  out.append(b.substr(start));
  return out;
}

std::string path_dirname(std::string_view path)
{
  size_t end = path.size();
  while (end > 0 && !is_separator(path[end - 1])) {
    end--;
  }
  while (end > 1 && is_separator(path[end - 1])) {
    end--;
  }
  return std::string(path.substr(0, end));
}

std::string path_basename(std::string_view path)
{
  size_t start = path.size();
  while (start > 0 && !is_separator(path[start - 1])) {
    start--;
  }
  return std::string(path.substr(start));
}

std::string path_stem(std::string_view path)
{
  std::string base = path_basename(path);
  const size_t dot = base.rfind('.');
  if (dot != std::string::npos && dot > 0) {
    base.resize(dot);
  }
  return base;
}

std::string path_extension(std::string_view path)
{
  const std::string base = path_basename(path);
  const size_t dot = base.rfind('.');
  if (dot == std::string::npos) {
    return {};
  }
  std::string ext = base.substr(dot);
  for (char &c : ext) {
    c = lower_ascii(c);
  }
  return ext;
}

std::string find_file_nocase(const std::string &directory, std::string_view name)
{
  std::error_code ec;
  const fs::path dir = to_fs_path(directory);
  const fs::path exact = dir / to_fs_path(name);
  if (fs::exists(exact, ec)) {
    return from_fs_path(exact);
  }
  for (const fs::directory_entry &entry : fs::directory_iterator(dir, ec)) {
    const std::string entry_name = from_fs_path(entry.path().filename());
    if (equals_nocase(entry_name, name)) {
      return from_fs_path(entry.path());
    }
  }
  return {};
}

/** Resolve a relative path one component at a time, ignoring case. */
static std::string resolve_nocase(const std::string &root, std::string_view relative)
{
  std::string current = root;
  size_t pos = 0;
  while (pos < relative.size()) {
    size_t next = pos;
    while (next < relative.size() && !is_separator(relative[next])) {
      next++;
    }
    const std::string_view part = relative.substr(pos, next - pos);
    if (!part.empty() && part != ".") {
      current = find_file_nocase(current, part);
      if (current.empty()) {
        return {};
      }
    }
    pos = next + 1;
  }
  return current;
}

std::string detect_client_root(const std::string &path)
{
  std::error_code ec;
  fs::path current = to_fs_path(path);
  if (!fs::is_directory(current, ec)) {
    current = current.parent_path();
  }
  while (!current.empty()) {
    const std::string dir = from_fs_path(current);
    const std::string name = from_fs_path(current.filename());
    if (equals_nocase(name, "element") || !find_file_nocase(dir, "models.pck").empty() ||
        !find_file_nocase(dir, "configs.pck").empty())
    {
      return dir;
    }
    if (current == current.parent_path()) {
      break;
    }
    current = current.parent_path();
  }
  return {};
}

bool wildcard_match(std::string_view pattern, std::string_view text)
{
  /* Iterative glob matching with single back-tracking point for `*`. */
  size_t p = 0, t = 0, star = std::string_view::npos, match = 0;
  auto same = [](char a, char b) {
    if (is_separator(a) && is_separator(b)) {
      return true;
    }
    return lower_ascii(a) == lower_ascii(b);
  };
  while (t < text.size()) {
    if (p < pattern.size() && (pattern[p] == '?' || same(pattern[p], text[t]))) {
      p++;
      t++;
    }
    else if (p < pattern.size() && pattern[p] == '*') {
      star = p++;
      match = t;
    }
    else if (star != std::string_view::npos) {
      p = star + 1;
      t = ++match;
    }
    else {
      return false;
    }
  }
  while (p < pattern.size() && pattern[p] == '*') {
    p++;
  }
  return p == pattern.size();
}

ClientFileSystem::ClientFileSystem(std::string client_root) : root_(std::move(client_root)) {}

ClientFileSystem::~ClientFileSystem() = default;

void ClientFileSystem::add_package(std::shared_ptr<PckArchive> package, std::string mount)
{
  mount = normalize_engine_path(mount, false);
  for (auto &item : packages_) {
    if (item.first == mount) {
      item.second = std::move(package);
      return;
    }
  }
  packages_.emplace_back(std::move(mount), std::move(package));
}

std::shared_ptr<PckArchive> ClientFileSystem::package_for_mount(const std::string &mount)
{
  for (auto &item : packages_) {
    if (item.first == mount) {
      return item.second;
    }
  }
  if (root_.empty() || missing_mounts_.count(mount)) {
    return nullptr;
  }
  const std::string pck_path = find_file_nocase(root_, mount + ".pck");
  if (pck_path.empty()) {
    missing_mounts_[mount] = true;
    return nullptr;
  }
  auto package = std::make_shared<PckArchive>();
  if (!package->open(pck_path, nullptr)) {
    missing_mounts_[mount] = true;
    return nullptr;
  }
  packages_.emplace_back(mount, package);
  return package;
}

bool ClientFileSystem::read_loose(const std::string &normalized, Bytes &r_data)
{
  if (root_.empty()) {
    return false;
  }
  const std::string path = resolve_nocase(root_, normalized);
  return !path.empty() && read_file(path, r_data);
}

bool ClientFileSystem::read_packed(const std::string &normalized, Bytes &r_data)
{
  const size_t sep = normalized.find('\\');
  if (sep != std::string::npos) {
    const std::string mount = normalized.substr(0, sep);
    if (std::shared_ptr<PckArchive> package = package_for_mount(mount)) {
      /* Entries are stored either with or without the mount folder, depending on the tool that
       * built the package. */
      const PckEntry *entry = package->find(normalized);
      if (entry == nullptr) {
        entry = package->find(normalized.substr(sep + 1));
      }
      if (entry && package->read(*entry, r_data, nullptr)) {
        return true;
      }
    }
  }
  /* Fall back to every opened package, for paths that do not start with a mount folder. */
  for (auto &item : packages_) {
    const PckEntry *entry = item.second->find(normalized);
    if (entry == nullptr) {
      entry = item.second->find(item.first + "\\" + normalized);
    }
    if (entry && item.second->read(*entry, r_data, nullptr)) {
      return true;
    }
  }
  return false;
}

bool ClientFileSystem::read(std::string_view engine_path, Bytes &r_data, std::string_view base_dir)
{
  const std::string normalized = normalize_engine_path(engine_path, false);
  if (normalized.empty()) {
    return false;
  }
  if (!base_dir.empty()) {
    const std::string relative = normalize_engine_path(base_dir, false) + "\\" + normalized;
    if (read_loose(relative, r_data) || read_packed(relative, r_data)) {
      return true;
    }
  }
  if (read_loose(normalized, r_data) || read_packed(normalized, r_data)) {
    return true;
  }
  for (const std::string &dir : search_dirs_) {
    std::string path = resolve_nocase(dir, normalized);
    if (path.empty()) {
      path = resolve_nocase(dir, path_basename(normalized));
    }
    if (!path.empty() && read_file(path, r_data)) {
      return true;
    }
  }
  return false;
}

void ClientFileSystem::add_search_dir(std::string directory)
{
  if (!directory.empty() &&
      std::find(search_dirs_.begin(), search_dirs_.end(), directory) == search_dirs_.end())
  {
    search_dirs_.push_back(std::move(directory));
  }
}

bool ClientFileSystem::exists(std::string_view engine_path)
{
  Bytes data;
  return read(engine_path, data);
}

}  // namespace blender::io::angelica
