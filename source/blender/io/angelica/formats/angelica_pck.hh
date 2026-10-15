/* SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup angelica
 *
 * Reader for Angelica Engine file packages (`.pck`), including the `.pkx`, `.pkx1`, ...
 * continuation files that extend a package past 2 GB. The package and its continuations are
 * treated as one logical byte stream; files are read on demand with 64-bit seeks, the package is
 * never loaded into memory as a whole.
 *
 * See `angelica_pck.cc` for the on-disk layout.
 */

#pragma once

#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "angelica_reader.hh"

namespace blender::io::angelica {

struct PckEntry {
  /** Lookup key: UTF-8, lower-case ASCII, `\` separators (see #normalize_engine_path). */
  std::string path;
  /** UTF-8 with the original case and `\` separators, for display. */
  std::string path_display;
  /** Offset in the logical `.pck` + `.pkx*` stream. */
  uint64_t offset = 0;
  /** Uncompressed size. */
  uint32_t size = 0;
  /** Stored size. Data is zlib compressed when smaller than #size. */
  uint32_t compressed_size = 0;

  bool is_compressed() const
  {
    return compressed_size < size;
  }
};

/** The four key words of a package (`AFilePackMan::SetAlgorithmID`). */
struct PckKeys {
  uint32_t guard0 = 0;
  uint32_t guard1 = 0;
  uint32_t mask = 0;
  uint32_t check = 0;
  /** Algorithm id, -1 for the fixed sets that have none. */
  int64_t id = 0;
};

/** How a package is laid out on disk, kept so it can be written back the same way. */
struct PckLayout {
  /** End of the package data: from the safe header, or the end of the stream. */
  uint64_t data_end = 0;
  /** Offset of the file table. */
  uint64_t entry_offset = 0;
  /**
   * Version 0x00020003: the high half of the stored table offset is masked with the sign extension
   * of the 32-bit mask (the 64-bit client's way) rather than with zero.
   */
  bool entry_offset_sign_extended = true;
  /** Size of the safe header at offset 0: 0 (none), 12 or 16 bytes. */
  int safe_header_size = 0;
  /** The safe header stores the end as 64 bits (`u32 tag, u64 end [, u32 tag2]`). */
  bool safe_header_u64 = false;
  /** The safe header as read. */
  uint8_t safe_header[16] = {};
  /** The `FILEHEADER` before the trailing count and version (272 or 280 bytes), as stored. */
  Bytes file_header;
};

class PckArchive {
 public:
  /** Package versions found in Perfect World clients. */
  static constexpr uint32_t VERSION_2_1 = 0x00020001;
  static constexpr uint32_t VERSION_2_2 = 0x00020002;
  static constexpr uint32_t VERSION_2_3 = 0x00020003;

  PckArchive();
  ~PckArchive();
  PckArchive(const PckArchive &) = delete;
  PckArchive &operator=(const PckArchive &) = delete;

  /**
   * Open a `.pck` file and read its file table. Continuation files named `<base>.pkx`,
   * `<base>.pkx1`, `<base>.pkx2`, ... next to it are picked up automatically (file names are
   * matched case-insensitively). Returns false with a message in `r_error` on failure.
   */
  bool open(const std::string &filepath, std::string *r_error);
  void close();
  bool is_open() const;

  const std::vector<PckEntry> &entries() const
  {
    return entries_;
  }

  /** Find an entry by path. Case and separator style do not matter. */
  const PckEntry *find(std::string_view path) const;

  /** Read and (when needed) decompress the data of an entry. Thread safe. */
  bool read(const PckEntry &entry, Bytes &r_data, std::string *r_error) const;
  /** Convenience: #find followed by #read. */
  bool read(std::string_view path, Bytes &r_data, std::string *r_error) const;

  /** The bytes of an entry as stored (compressed or not), for copying it unchanged. */
  bool read_stored(const PckEntry &entry, Bytes &r_data) const;

  /**
   * The `FILEENTRY` record of every entry as stored in the file table (276 or 288 bytes each, in
   * the order of #entries), keeping the fields this reader does not interpret.
   */
  bool raw_records(std::vector<Bytes> &r_records, std::string *r_error) const;

  /** Package version, for example `0x00020002`. */
  uint32_t version() const
  {
    return version_;
  }
  /** Value passed to the engine's `AFilePackMan::SetAlgorithmID` for this package, or -1 when the
   * package used one of the fixed custom key sets. */
  int64_t algorithm_id() const
  {
    return algorithm_id_;
  }
  bool is_encrypted() const
  {
    return encrypted_;
  }
  /** Description string stored in the package header (usually the copyright line). */
  const std::string &description() const
  {
    return description_;
  }
  /** Paths of the files making up the logical stream (`.pck` first). */
  std::vector<std::string> part_paths() const;
  /** Size of each part of the logical stream, in the order of #part_paths. */
  std::vector<uint64_t> part_sizes() const;
  /** Size of the logical stream (sum of all parts). */
  uint64_t stream_size() const
  {
    return stream_size_;
  }
  const PckKeys &keys() const
  {
    return keys_;
  }
  const PckLayout &layout() const
  {
    return layout_;
  }

 private:
  struct Part {
    std::FILE *file = nullptr;
    std::string path;
    uint64_t start = 0;
    uint64_t size = 0;
  };

  std::vector<Part> parts_;
  uint64_t stream_size_ = 0;
  uint32_t version_ = 0;
  int64_t algorithm_id_ = 0;
  bool encrypted_ = false;
  std::string description_;
  PckKeys keys_;
  PckLayout layout_;
  std::vector<PckEntry> entries_;
  std::unordered_map<std::string, size_t> lookup_;
  mutable std::mutex io_mutex_;

  bool read_at(uint64_t offset, void *dst, size_t size) const;
};

/**
 * Open a package once per process and share it: later calls for the same `.pck` return the same
 * archive while the file keeps its size and modification time (a package rewritten on disk is
 * opened again). Thread safe. Returns null with a message in `r_error` on failure.
 */
std::shared_ptr<PckArchive> open_package_shared(const std::string &pck_path,
                                                std::string *r_error);

/** Forget the shared archive of a package, closing its files once no one uses it any more. */
void release_package_shared(const std::string &pck_path);

}  // namespace blender::io::angelica
