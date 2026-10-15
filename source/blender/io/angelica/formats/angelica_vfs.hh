/* SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup angelica
 *
 * The client's virtual file system, in the order the client looks files up when it runs with
 * packages (`AFileImage::Init`, `AFilePackMan::GetFilePck`): first inside the package whose
 * folder holds the path (`models\npc\x.ski` lives in `models.pck`, `help\script\x.lua` in
 * `help\script.pck`), then as a loose file under the client folder (`element`) when the package
 * does not have it. Lookups are case insensitive, like on Windows.
 *
 * Packages are opened once per process and shared (#open_package_shared), so browsing a client
 * and importing from it do not read the file tables again.
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
struct PckEntry;

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

/** Where the client finds a file. */
struct FileLocation {
  enum class Kind : uint8_t {
    /** Not found. */
    None,
    /** An entry of a package (`.pck`). */
    Packed,
    /** A loose file under the client folder. */
    Loose,
    /** A file in a search folder outside the client (see #ClientFileSystem::add_search_dir). */
    External,
  };
  Kind kind = Kind::None;
  /** Engine path that was found (normalized), relative to the client root. */
  std::string engine_path;
  /** The package file for packed files, the file itself otherwise. */
  std::string container;
  /** Packed files: mount folder of the package (`models`, `help\script`). */
  std::string mount;
  /** Packed files: path of the entry inside the package, as stored (UTF-8, original case). */
  std::string entry;
  /** Uncompressed size in bytes. */
  uint64_t size = 0;

  /** Internal: the package and entry, for reading without a second lookup. */
  std::shared_ptr<PckArchive> package;
  const PckEntry *pck_entry = nullptr;
};

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
   * Find a file by its engine path (`models\npc\x.ski`, separators and case do not matter) in
   * the order the client uses: the package whose folder holds the path first, then the loose
   * file (`AFileImage::Init`). When `base_dir` (an engine path) is given, the path is first tried
   * relative to it.
   */
  FileLocation locate(std::string_view engine_path, std::string_view base_dir = {});

  /** #locate and read the file. */
  bool read(std::string_view engine_path, Bytes &r_data, std::string_view base_dir = {});

  /** Read a file found by #locate. */
  bool read(const FileLocation &location, Bytes &r_data);

  bool exists(std::string_view engine_path);

  /**
   * Also look in a folder outside the client (for files opened directly from disk): referenced
   * files are tried relative to it, then by file name alone.
   */
  void add_search_dir(std::string directory);

  /**
   * When set, every successful read appends where the file came from (used to know which files
   * of the client an import used). Pass null to stop.
   */
  void set_read_log(std::vector<FileLocation> *log)
  {
    read_log_ = log;
  }

  /** Every opened package with its mount folder. */
  const std::vector<std::pair<std::string, std::shared_ptr<PckArchive>>> &packages() const
  {
    return packages_;
  }

 private:
  /** The package mounted at `mount` (`models`, `help\script`), opened on first use. */
  std::shared_ptr<PckArchive> package_for_mount(const std::string &mount);
  FileLocation locate_packed(const std::string &normalized);
  FileLocation locate_loose(const std::string &normalized);

  std::string root_;
  std::vector<std::string> search_dirs_;
  std::vector<std::pair<std::string, std::shared_ptr<PckArchive>>> packages_;
  /** Mount folders that were looked for and have no package, to avoid probing the disk again. */
  std::map<std::string, bool> missing_mounts_;
  std::vector<FileLocation> *read_log_ = nullptr;
};

}  // namespace blender::io::angelica
