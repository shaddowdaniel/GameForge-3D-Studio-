/* SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup angelica
 *
 * The client's virtual file system: files are looked up first as loose files under the client
 * folder (`element`), then inside the `.pck` package named after the first path component
 * (`models\npc\x.ski` lives in `models.pck`), the same order the client uses when it runs with
 * packages enabled. Lookups are case insensitive, like on Windows.
 */

#pragma once

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "angelica_reader.hh"

namespace blender::io::angelica {

class PckArchive;

/** Convert a UTF-8 path to a `std::filesystem` friendly string on every platform. */
std::string path_join(std::string_view a, std::string_view b);
/** Directory part of a path (without trailing separator). */
std::string path_dirname(std::string_view path);
/** File name without directory. */
std::string path_basename(std::string_view path);
/** File name without directory and extension. */
std::string path_stem(std::string_view path);
/** Lower-case extension including the dot, empty when there is none. */
std::string path_extension(std::string_view path);

/** Find a file in `directory` ignoring case. Returns the full path or empty. */
std::string find_file_nocase(const std::string &directory, std::string_view name);

/**
 * Walk up from `path` to find the client data folder: the first folder containing `models.pck`,
 * `configs.pck` or `maps`, or named `element`. Empty when nothing looks like a client.
 */
std::string detect_client_root(const std::string &path);

/** Case-insensitive match of `text` against a pattern with `*` and `?`. */
bool wildcard_match(std::string_view pattern, std::string_view text);

class ClientFileSystem {
 public:
  explicit ClientFileSystem(std::string client_root);
  ~ClientFileSystem();

  ClientFileSystem(const ClientFileSystem &) = delete;
  ClientFileSystem &operator=(const ClientFileSystem &) = delete;

  const std::string &root() const
  {
    return root_;
  }

  /**
   * Make an already opened package searchable. `mount` is the folder it represents relative to
   * the client root (the package file name without extension, for example `models`).
   */
  void add_package(std::shared_ptr<PckArchive> package, std::string mount);

  /**
   * Read a file by its engine path (`models\npc\x.ski`, separators and case do not matter).
   * When `base_dir` (an engine path) is given, the path is first tried relative to it.
   */
  bool read(std::string_view engine_path, Bytes &r_data, std::string_view base_dir = {});

  bool exists(std::string_view engine_path);

  /**
   * Also look in a folder outside the client (for files opened directly from disk): referenced
   * files are tried relative to it, then by file name alone.
   */
  void add_search_dir(std::string directory);

  /** Every opened package with its mount folder. */
  const std::vector<std::pair<std::string, std::shared_ptr<PckArchive>>> &packages() const
  {
    return packages_;
  }

 private:
  std::shared_ptr<PckArchive> package_for_mount(const std::string &mount);
  bool read_loose(const std::string &normalized, Bytes &r_data);
  bool read_packed(const std::string &normalized, Bytes &r_data);

  std::string root_;
  std::vector<std::string> search_dirs_;
  std::vector<std::pair<std::string, std::shared_ptr<PckArchive>>> packages_;
  /** Mount folders that were looked for and have no package, to avoid probing the disk again. */
  std::map<std::string, bool> missing_mounts_;
};

}  // namespace blender::io::angelica
