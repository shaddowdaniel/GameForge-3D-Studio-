/* SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup angelica
 *
 * Writing is the mirror of `angelica_pck.cc`:
 * - File data: zlib (RFC 1950) when that is smaller than the file, raw bytes otherwise (the client
 *   only decompresses when the stored size is smaller than the file size).
 * - File table: per file `u32 len ^ MASK`, `u32 len ^ MASK ^ CHECK` and the `FILEENTRY` record,
 *   zlib compressed unless that does not make it smaller.
 * - Trailer: the source package's `FILEHEADER` with the new table offset, the file count and the
 *   version.
 * - Safe header at offset 0 (`tag1`, end, `tag2` / 64-bit end), written last: it is the commit.
 * - Parts: the `.pck` holds at most 0x7FFFFF00 bytes, then `.pkx` (and for 64-bit clients
 *   `.pkx1`, `.pkx2`, ... of 0xFFFFFE00 bytes each); version 0x00020001/2 packages (32-bit
 *   clients) have a single `.pkx` and 32-bit offsets, so they end before 4 GB.
 */

#include "angelica_pck_writer.hh"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_set>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <io.h>
#  include <windows.h>
#else
#  include <unistd.h>
#endif

#include <zlib.h>

#include "angelica_elements.hh"
#include "angelica_pck.hh"

namespace blender::io::angelica {

namespace {

namespace fs = std::filesystem;

constexpr uint64_t PCK_PART_LIMIT = 0x7FFFFF00u;
constexpr uint64_t PKX_PART_LIMIT_64 = 0xFFFFFE00u;
constexpr uint64_t NO_PART_LIMIT = UINT64_MAX / 2;
constexpr size_t NAME_LEN = 260;
constexpr size_t ENTRY_SIZE_V2 = 276;
constexpr size_t ENTRY_SIZE_V3 = 288;
constexpr const char *JOURNAL_EXT = ".gfjournal";

uint64_t g_part_limit_for_tests = 0;

void set_error(std::string *r_error, std::string msg)
{
  if (r_error) {
    *r_error = std::move(msg);
  }
}

fs::path u8path(const std::string &s)
{
  return fs::path(std::u8string(reinterpret_cast<const char8_t *>(s.data()), s.size()));
}

std::string from_fs(const fs::path &p)
{
  const std::u8string s = p.u8string();
  return std::string(reinterpret_cast<const char *>(s.data()), s.size());
}

void store_u32(uint8_t *p, const uint32_t v)
{
  p[0] = uint8_t(v);
  p[1] = uint8_t(v >> 8);
  p[2] = uint8_t(v >> 16);
  p[3] = uint8_t(v >> 24);
}

void store_u64(uint8_t *p, const uint64_t v)
{
  store_u32(p, uint32_t(v));
  store_u32(p + 4, uint32_t(v >> 32));
}

void append_u32(Bytes &out, const uint32_t v)
{
  uint8_t b[4];
  store_u32(b, v);
  out.insert(out.end(), b, b + 4);
}

bool starts_with(const std::string &s, const std::string &prefix)
{
  return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

std::FILE *open_file(const std::string &path, const char *mode)
{
#ifdef _WIN32
  const std::wstring wmode(mode, mode + std::strlen(mode));
  return _wfopen(u8path(path).c_str(), wmode.c_str());
#else
  return std::fopen(path.c_str(), mode);
#endif
}

bool seek64(std::FILE *f, const uint64_t offset)
{
#ifdef _WIN32
  return _fseeki64(f, int64_t(offset), SEEK_SET) == 0;
#else
  return fseeko(f, off_t(offset), SEEK_SET) == 0;
#endif
}

bool sync_file(std::FILE *f)
{
  if (std::fflush(f) != 0) {
    return false;
  }
#ifdef _WIN32
  return _commit(_fileno(f)) == 0;
#else
  return fsync(fileno(f)) == 0;
#endif
}

/** Whether another program has the file open: the game client, a packing tool, ... */
bool file_in_use(const std::string &path)
{
#ifdef _WIN32
  /* Opening without sharing fails while any other handle is open. */
  const HANDLE handle = CreateFileW(
      u8path(path).c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return GetLastError() == ERROR_SHARING_VIOLATION;
  }
  CloseHandle(handle);
#else
  (void)path;
#endif
  return false;
}

bool check_not_in_use(const std::vector<std::string> &paths, std::string *r_error)
{
  for (const std::string &path : paths) {
    if (file_in_use(path)) {
      set_error(r_error,
                "\"" + path +
                    "\" is open in another program: close the game client and the tools using it");
      return false;
    }
  }
  return true;
}

/** File name of part `index` of the package `pck_path`: `.pck`, `.pkx`, `.pkx1`, ... */
std::string part_path(const std::string &pck_path, const int index)
{
  if (index == 0) {
    return pck_path;
  }
  fs::path p = u8path(pck_path);
  p.replace_extension(index == 1 ? ".pkx" : ".pkx" + std::to_string(index - 1));
  return from_fs(p);
}

/** The part files of a package that exist on disk, `.pck` first. */
std::vector<std::string> existing_parts(const std::string &pck_path)
{
  std::vector<std::string> paths;
  for (int index = 0; fs::exists(u8path(part_path(pck_path, index))); index++) {
    paths.push_back(part_path(pck_path, index));
  }
  return paths;
}

/** Largest size of part `index`, 0 when packages of this version cannot have that part. */
uint64_t part_limit(const int index, const bool v3)
{
  if (index == 0) {
    return g_part_limit_for_tests ? g_part_limit_for_tests : PCK_PART_LIMIT;
  }
  if (v3) {
    return g_part_limit_for_tests ? g_part_limit_for_tests : PKX_PART_LIMIT_64;
  }
  /* A single `.pkx`, bounded by the 32-bit offsets (checked when writing the table). */
  return index == 1 ? NO_PART_LIMIT : 0;
}

/** The logical stream of a package's part files, for writing. */
class PartWriter {
 public:
  PartWriter(std::string pck_path, const bool v3) : pck_path_(std::move(pck_path)), v3_(v3) {}
  ~PartWriter()
  {
    close();
  }

  /** Write into the parts of an existing package, `sizes` being their current sizes. */
  bool open_existing(const std::vector<std::string> &paths,
                     const std::vector<uint64_t> &sizes,
                     std::string *r_error)
  {
    uint64_t start = 0;
    for (size_t i = 0; i < paths.size(); i++) {
      std::FILE *f = open_file(paths[i], "r+b");
      if (f == nullptr) {
        set_error(r_error, "Cannot open \"" + paths[i] + "\" for writing");
        return false;
      }
      parts_.push_back({paths[i], f, start, sizes[i], sizes[i], false});
      start += sizes[i];
    }
    return true;
  }

  /** Start a new package (the `.pck`; more parts are created as needed). */
  bool create(std::string *r_error)
  {
    std::FILE *f = open_file(pck_path_, "w+b");
    if (f == nullptr) {
      set_error(r_error, "Cannot create \"" + pck_path_ + "\"");
      return false;
    }
    parts_.push_back({pck_path_, f, 0, 0, 0, true});
    return true;
  }

  bool write(uint64_t offset, const uint8_t *data, size_t size, std::string *r_error)
  {
    while (size > 0) {
      uint64_t capacity = 0;
      Part *part = part_at(offset, capacity, r_error);
      if (part == nullptr) {
        return false;
      }
      const uint64_t in_part = offset - part->start;
      const size_t chunk = size_t(std::min<uint64_t>(capacity - in_part, size));
      if (!seek64(part->file, in_part) || std::fwrite(data, 1, chunk, part->file) != chunk) {
        set_error(r_error, "Cannot write to \"" + part->path + "\" (is the disk full?)");
        return false;
      }
      part->size = std::max(part->size, in_part + chunk);
      offset += chunk;
      data += chunk;
      size -= chunk;
    }
    return true;
  }

  bool write(const uint64_t offset, const Bytes &data, std::string *r_error)
  {
    return write(offset, data.data(), data.size(), r_error);
  }

  bool sync(std::string *r_error)
  {
    for (Part &part : parts_) {
      if (part.file && !sync_file(part.file)) {
        set_error(r_error, "Cannot flush \"" + part.path + "\" to disk");
        return false;
      }
    }
    return true;
  }

  void close()
  {
    for (Part &part : parts_) {
      if (part.file) {
        std::fclose(part.file);
        part.file = nullptr;
      }
    }
  }

  /** Put the part files back as they were before writing (sizes, no new parts). */
  void restore()
  {
    close();
    std::error_code ec;
    for (const Part &part : parts_) {
      if (part.created) {
        fs::remove(u8path(part.path), ec);
      }
      else {
        fs::resize_file(u8path(part.path), part.original_size, ec);
      }
    }
  }

  std::vector<std::string> paths() const
  {
    std::vector<std::string> result;
    for (const Part &part : parts_) {
      result.push_back(part.path);
    }
    return result;
  }

 private:
  struct Part {
    std::string path;
    std::FILE *file = nullptr;
    uint64_t start = 0;
    uint64_t size = 0;
    uint64_t original_size = 0;
    bool created = false;
  };

  /** The part holding `offset`, created when the offset is past the last part. */
  Part *part_at(const uint64_t offset, uint64_t &r_capacity, std::string *r_error)
  {
    for (size_t i = 0; i < parts_.size(); i++) {
      Part &part = parts_[i];
      /* Parts before the last keep their size (the reader splits the stream at the physical part
       * sizes), the last one grows up to its limit. */
      r_capacity = i + 1 < parts_.size() ? part.size :
                                           std::max(part.size, part_limit(int(i), v3_));
      if (offset < part.start + r_capacity) {
        return &part;
      }
    }
    const Part &last = parts_.back();
    const uint64_t last_capacity = std::max(last.size, part_limit(int(parts_.size()) - 1, v3_));
    const uint64_t limit = part_limit(int(parts_.size()), v3_);
    if (limit == 0) {
      set_error(r_error,
                "The package is full: clients of this package version read a single .pkx part");
      return nullptr;
    }
    if (last.size != last_capacity || offset != last.start + last.size) {
      set_error(r_error, "Internal error: package parts are not written in order");
      return nullptr;
    }
    const std::string path = part_path(pck_path_, int(parts_.size()));
    std::FILE *f = open_file(path, "w+b");
    if (f == nullptr) {
      set_error(r_error, "Cannot create \"" + path + "\"");
      return nullptr;
    }
    parts_.push_back({path, f, offset, 0, 0, true});
    r_capacity = limit;
    return &parts_.back();
  }

  std::string pck_path_;
  bool v3_;
  std::vector<Part> parts_;
};

/** One file of the package being written. */
struct OutEntry {
  /** The `FILEENTRY` record (276 or 288 bytes); offset and sizes are set when data is written. */
  Bytes record;
  /** Whether data is written for this entry: copied from #copy_from, or #stored. Kept entries of
   * an in-place update keep pointing at their current data. */
  bool write_data = false;
  const PckEntry *copy_from = nullptr;
  Bytes stored;
  uint32_t size = 0;
};

Bytes zlib_compress(const uint8_t *data, const size_t size, const int level)
{
  uLongf out_size = compressBound(uLong(size));
  Bytes out(out_size);
  if (compress2(out.data(), &out_size, data, uLong(size), level) != Z_OK) {
    return {};
  }
  out.resize(out_size);
  return out;
}

/** The bytes a package stores for a file: zlib when smaller, raw otherwise (engine rule). */
Bytes stored_form(const Bytes &data)
{
  if (data.empty()) {
    return {};
  }
  Bytes compressed = zlib_compress(data.data(), data.size(), Z_BEST_COMPRESSION);
  if (!compressed.empty() && compressed.size() < data.size()) {
    return compressed;
  }
  return data;
}

void set_record_location(
    Bytes &record, const bool v3, const uint64_t offset, const uint32_t size, const uint32_t stored)
{
  if (v3) {
    store_u64(&record[264], offset);
    store_u32(&record[272], size);
    store_u32(&record[276], stored);
  }
  else {
    store_u32(&record[260], uint32_t(offset));
    store_u32(&record[264], size);
    store_u32(&record[268], stored);
  }
}

/**
 * The safe header of a package ending at `end`, in the style of `layout` (which has one). False
 * when the end does not fit the header.
 */
bool make_safe_header(const PckLayout &layout,
                      const uint64_t end,
                      Bytes &r_header,
                      std::string *r_error)
{
  r_header.assign(layout.safe_header, layout.safe_header + layout.safe_header_size);
  if (layout.safe_header_u64) {
    store_u64(&r_header[4], end);
    return true;
  }
  if (end > 0xFFFFFFFFu) {
    set_error(r_error,
              "The package would pass 4 GB, the limit of its header (save a copy instead)");
    return false;
  }
  store_u32(&r_header[4], uint32_t(end));
  return true;
}

/**
 * Write the data of `entries` that need it from `offset` on, then the file table and the trailer.
 * Returns the end of the package in `r_end`.
 */
bool write_body(PartWriter &out,
                uint64_t offset,
                std::vector<OutEntry> &entries,
                const PckArchive &like,
                const PckArchive *copy_source,
                uint64_t &r_end,
                std::string *r_error)
{
  const bool v3 = like.version() == PckArchive::VERSION_2_3;
  const PckKeys &keys = like.keys();
  const PckLayout &layout = like.layout();
  const char *too_large =
      "The package would pass 4 GB, the limit of its version (save a copy for a newer client)";

  Bytes copied;
  for (OutEntry &entry : entries) {
    if (!entry.write_data) {
      continue;
    }
    const Bytes *stored = &entry.stored;
    uint32_t size = entry.size;
    if (entry.copy_from) {
      if (!copy_source->read_stored(*entry.copy_from, copied)) {
        set_error(r_error,
                  "Cannot read \"" + entry.copy_from->path_display + "\" from the package");
        return false;
      }
      stored = &copied;
      size = entry.copy_from->size;
    }
    if (!v3 && offset + stored->size() > 0xFFFFFFFFu) {
      set_error(r_error, too_large);
      return false;
    }
    set_record_location(entry.record, v3, offset, size, uint32_t(stored->size()));
    if (!out.write(offset, *stored, r_error)) {
      return false;
    }
    offset += stored->size();
  }

  const uint64_t table_offset = offset;
  Bytes table;
  const size_t entry_size = v3 ? ENTRY_SIZE_V3 : ENTRY_SIZE_V2;
  for (const OutEntry &entry : entries) {
    Bytes record = zlib_compress(entry.record.data(), entry.record.size(), Z_DEFAULT_COMPRESSION);
    if (record.empty() || record.size() >= entry_size) {
      record = entry.record;
    }
    const uint32_t len = uint32_t(record.size());
    append_u32(table, len ^ keys.mask);
    append_u32(table, len ^ keys.mask ^ keys.check);
    table.insert(table.end(), record.begin(), record.end());
  }

  Bytes header = layout.file_header;
  store_u32(&header[8], uint32_t(table_offset) ^ keys.mask);
  if (v3) {
    /* The high half is masked the way the source package did it (zero or sign extension). */
    const uint32_t high_mask = layout.entry_offset_sign_extended ? 0xFFFFFFFFu : 0u;
    store_u32(&header[12], uint32_t(table_offset >> 32) ^ high_mask);
  }
  table.insert(table.end(), header.begin(), header.end());
  append_u32(table, uint32_t(entries.size()));
  append_u32(table, like.version());

  r_end = offset + table.size();
  if (!v3 && r_end > 0xFFFFFFFFu) {
    set_error(r_error, too_large);
    return false;
  }
  return out.write(offset, table, r_error);
}

/** The name the engine path `path` gets in a package mounted at `mount`. */
std::string stored_name(const PckArchive &archive, const std::string &mount, const std::string &path)
{
  std::string name = normalize_engine_path(path, false);
  if (mount.empty()) {
    return name;
  }
  /* Packages store their files with or without the mount folder, depending on the tool that
   * built them: follow the package's first file. */
  const std::string prefix = normalize_engine_path(mount, false) + "\\";
  const bool with_mount = archive.entries().empty() ||
                          starts_with(archive.entries().front().path, prefix);
  if (!with_mount && starts_with(name, prefix)) {
    name.erase(0, prefix.size());
  }
  else if (with_mount && !starts_with(name, prefix)) {
    name.insert(0, prefix);
  }
  return name;
}

/**
 * Index of the package entry for `path`, -1 when the package does not have it. Looked up like the
 * client file system does: the full engine path, then the path without the mount folder.
 */
int64_t find_entry(const PckArchive &archive, const std::string &mount, const std::string &path)
{
  const std::string normalized = normalize_engine_path(path, false);
  const PckEntry *entry = archive.find(normalized);
  if (entry == nullptr && !mount.empty()) {
    const std::string prefix = normalize_engine_path(mount, false) + "\\";
    entry = starts_with(normalized, prefix) ? archive.find(normalized.substr(prefix.size())) :
                                              archive.find(prefix + normalized);
  }
  return entry ? int64_t(entry - archive.entries().data()) : -1;
}

/** A zeroed record for a new file, named the way the package names its files. */
bool new_record(const PckArchive &archive,
                const std::string &mount,
                const std::string &path,
                Bytes &r_record,
                std::string *r_error)
{
  std::string gbk;
  if (!utf8_to_gbk(stored_name(archive, mount, path), gbk)) {
    set_error(r_error, "The path \"" + path + "\" has characters the game cannot use");
    return false;
  }
  if (gbk.size() >= NAME_LEN) {
    set_error(r_error, "The path \"" + path + "\" is longer than the game accepts");
    return false;
  }
  r_record.assign(archive.version() == PckArchive::VERSION_2_3 ? ENTRY_SIZE_V3 : ENTRY_SIZE_V2,
                  0);
  std::memcpy(r_record.data(), gbk.data(), gbk.size());
  return true;
}

bool check_unique_paths(const std::vector<PckFileChange> &changes, std::string *r_error)
{
  std::unordered_set<std::string> seen;
  for (const PckFileChange &change : changes) {
    if (!seen.insert(normalize_engine_path(change.path, false)).second) {
      set_error(r_error, "\"" + change.path + "\" is listed twice");
      return false;
    }
  }
  return true;
}

/** Check that `path` reads back as `expected` (or is missing when `expected` is null). */
bool verify_entry(const PckArchive &archive,
                  const std::string &mount,
                  const std::string &path,
                  const Bytes *expected,
                  std::string *r_error)
{
  const int64_t index = find_entry(archive, mount, path);
  if (expected == nullptr) {
    if (index >= 0) {
      set_error(r_error, "Verification failed: \"" + path + "\" is still in the package");
      return false;
    }
    return true;
  }
  Bytes data;
  if (index < 0 || !archive.read(archive.entries()[size_t(index)], data, nullptr) ||
      data != *expected)
  {
    set_error(r_error, "Verification failed: \"" + path + "\" does not read back as written");
    return false;
  }
  return true;
}

/* -------------------------------------------------------------------- */
/** \name Journal
 *
 * One record per in-place update, in a text file next to the package:
 * \code
 * GFJ1
 * before <safe header before the update, hex>
 * after <safe header the update committed, hex>
 * part <size before the update> <path>
 * end
 * \endcode
 * \{ */

struct JournalRecord {
  Bytes before;
  Bytes after;
  std::vector<std::pair<std::string, uint64_t>> parts;
};

std::string journal_path(const std::string &pck_path)
{
  return pck_path + JOURNAL_EXT;
}

std::string to_hex(const Bytes &data)
{
  static const char *digits = "0123456789abcdef";
  std::string out;
  for (const uint8_t b : data) {
    out.push_back(digits[b >> 4]);
    out.push_back(digits[b & 15]);
  }
  return out;
}

Bytes from_hex(const std::string &hex)
{
  Bytes out;
  for (size_t i = 0; i + 1 < hex.size(); i += 2) {
    out.push_back(uint8_t(std::strtoul(hex.substr(i, 2).c_str(), nullptr, 16)));
  }
  return out;
}

std::vector<JournalRecord> read_journal(const std::string &pck_path)
{
  std::vector<JournalRecord> records;
  std::ifstream in(u8path(journal_path(pck_path)), std::ios::binary);
  std::string line;
  JournalRecord current;
  bool open = false;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line == "GFJ1") {
      current = {};
      open = true;
    }
    else if (open && starts_with(line, "before ")) {
      current.before = from_hex(line.substr(7));
    }
    else if (open && starts_with(line, "after ")) {
      current.after = from_hex(line.substr(6));
    }
    else if (open && starts_with(line, "part ")) {
      const size_t space = line.find(' ', 5);
      if (space != std::string::npos) {
        current.parts.emplace_back(line.substr(space + 1),
                                   std::strtoull(line.substr(5, space - 5).c_str(), nullptr, 10));
      }
    }
    else if (open && line == "end") {
      if (!current.before.empty() && current.before.size() == current.after.size() &&
          !current.parts.empty())
      {
        records.push_back(current);
      }
      open = false;
    }
  }
  return records;
}

bool write_journal(const std::string &pck_path,
                   const std::vector<JournalRecord> &records,
                   std::string *r_error)
{
  const std::string path = journal_path(pck_path);
  std::error_code ec;
  if (records.empty()) {
    fs::remove(u8path(path), ec);
    return true;
  }
  std::ostringstream text;
  for (const JournalRecord &record : records) {
    text << "GFJ1\nbefore " << to_hex(record.before) << "\nafter " << to_hex(record.after) << "\n";
    for (const auto &[part, size] : record.parts) {
      text << "part " << size << " " << part << "\n";
    }
    text << "end\n";
  }
  /* Written aside and renamed, so a crash leaves the previous journal. */
  const std::string tmp = path + ".tmp";
  std::FILE *f = open_file(tmp, "wb");
  const std::string data = text.str();
  bool ok = f && std::fwrite(data.data(), 1, data.size(), f) == data.size() && sync_file(f);
  if (f) {
    std::fclose(f);
  }
  if (ok) {
    fs::rename(u8path(tmp), u8path(path), ec);
    ok = !ec;
  }
  if (!ok) {
    fs::remove(u8path(tmp), ec);
    set_error(r_error, "Cannot write the save journal \"" + path + "\"");
  }
  return ok;
}

/** The first `size` bytes of a file (the safe header of a package). */
Bytes read_head(const std::string &path, const size_t size)
{
  Bytes data(size);
  std::FILE *f = open_file(path, "rb");
  const bool ok = f && std::fread(data.data(), 1, size, f) == size;
  if (f) {
    std::fclose(f);
  }
  return ok ? data : Bytes();
}

bool write_head(const std::string &path, const Bytes &data, std::string *r_error)
{
  std::FILE *f = open_file(path, "r+b");
  const bool ok = f && seek64(f, 0) && std::fwrite(data.data(), 1, data.size(), f) == data.size() &&
                  sync_file(f);
  if (f) {
    std::fclose(f);
  }
  if (!ok) {
    set_error(r_error, "Cannot write the header of \"" + path + "\"");
  }
  return ok;
}

/** Journal records whose update is in the package as it is now, oldest first. */
std::vector<JournalRecord> committed_records(const std::string &pck_path)
{
  std::vector<JournalRecord> records = read_journal(pck_path);
  /* Updates that never committed (interrupted), or that another program wrote over since, cannot
   * be undone. */
  while (!records.empty() &&
         read_head(pck_path, records.back().after.size()) != records.back().after)
  {
    records.pop_back();
  }
  return records;
}

/** \} */

}  // namespace

void pck_writer_set_part_limit_for_tests(const uint64_t limit)
{
  g_part_limit_for_tests = limit;
}

bool pck_can_undo(const std::string &pck_path)
{
  return !committed_records(pck_path).empty();
}

bool pck_undo_last_update(const std::string &pck_path, std::string *r_error)
{
  std::vector<JournalRecord> records = committed_records(pck_path);
  if (records.empty()) {
    set_error(r_error,
              "\"" + pck_path +
                  "\" has no save to undo (or another program changed it after the save)");
    return false;
  }
  const JournalRecord record = records.back();
  release_package_shared(pck_path);
  const std::vector<std::string> paths = existing_parts(pck_path);
  if (!check_not_in_use(paths, r_error)) {
    return false;
  }
  /* The old header first: from then on the client reads the package as it was. */
  if (!write_head(record.parts.front().first, record.before, r_error)) {
    return false;
  }
  /* Then drop the bytes the update appended, and the parts it created. */
  std::error_code ec;
  for (const auto &[part, size] : record.parts) {
    fs::resize_file(u8path(part), size, ec);
  }
  for (size_t index = record.parts.size(); index < paths.size(); index++) {
    fs::remove(u8path(paths[index]), ec);
  }
  records.pop_back();
  if (!write_journal(pck_path, records, r_error)) {
    return false;
  }
  PckArchive check;
  return check.open(pck_path, r_error);
}

bool pck_update_in_place(const std::string &pck_path,
                         const std::string &mount,
                         const std::vector<PckFileChange> &changes,
                         PckWriteResult *r_result,
                         std::string *r_error)
{
  if (!check_unique_paths(changes, r_error)) {
    return false;
  }
  /* Checked before this function opens the package itself. */
  release_package_shared(pck_path);
  if (!check_not_in_use(existing_parts(pck_path), r_error)) {
    return false;
  }
  auto archive = std::make_unique<PckArchive>();
  if (!archive->open(pck_path, r_error)) {
    return false;
  }
  const PckLayout layout = archive->layout();
  if (layout.safe_header_size == 0) {
    set_error(r_error,
              "This package has no safe header, so it cannot be changed in place without risk "
              "(save a full copy instead)");
    return false;
  }
  if (archive->is_encrypted()) {
    set_error(r_error, "Encrypted packages cannot be written");
    return false;
  }
  const std::vector<std::string> paths = archive->part_paths();
  const std::vector<uint64_t> sizes = archive->part_sizes();

  std::vector<Bytes> records;
  if (!archive->raw_records(records, r_error)) {
    return false;
  }
  const std::vector<PckEntry> &old_entries = archive->entries();
  std::vector<OutEntry> entries(records.size());
  for (size_t i = 0; i < records.size(); i++) {
    entries[i].record = std::move(records[i]);
  }
  /* Entries replaced or removed; the others must keep their place, which verification checks. */
  std::vector<bool> touched(entries.size(), false);
  std::vector<bool> removed(entries.size(), false);
  std::vector<OutEntry> added;
  PckWriteResult result;
  for (const PckFileChange &change : changes) {
    const int64_t index = find_entry(*archive, mount, change.path);
    if (index >= 0) {
      touched[size_t(index)] = true;
    }
    if (change.remove) {
      if (index >= 0) {
        removed[size_t(index)] = true;
        result.removed++;
      }
      continue;
    }
    OutEntry *entry = nullptr;
    if (index >= 0) {
      entry = &entries[size_t(index)];
      result.written++;
    }
    else {
      entry = &added.emplace_back();
      if (!new_record(*archive, mount, change.path, entry->record, r_error)) {
        return false;
      }
      result.added++;
    }
    entry->write_data = true;
    entry->size = uint32_t(change.data.size());
    entry->stored = stored_form(change.data);
  }
  std::vector<OutEntry> final_entries;
  final_entries.reserve(entries.size() + added.size());
  for (size_t i = 0; i < entries.size(); i++) {
    if (!removed[i]) {
      final_entries.push_back(std::move(entries[i]));
    }
  }
  for (OutEntry &entry : added) {
    final_entries.push_back(std::move(entry));
  }

  JournalRecord journal_record;
  journal_record.before.assign(layout.safe_header, layout.safe_header + layout.safe_header_size);
  for (size_t i = 0; i < paths.size(); i++) {
    journal_record.parts.emplace_back(paths[i], sizes[i]);
  }

  /* Everything goes after the package's end: the bytes the client reads now are not touched.
   * ponytail: every update appends a new file table and leaves the old one as dead bytes (kept
   * for undo); a full copy (#pck_write_copy) compacts the package when that space matters. */
  PartWriter writer(pck_path, archive->version() == PckArchive::VERSION_2_3);
  uint64_t end = 0;
  if (!writer.open_existing(paths, sizes, r_error) ||
      !write_body(writer, layout.data_end, final_entries, *archive, nullptr, end, r_error) ||
      !writer.sync(r_error) || !make_safe_header(layout, end, journal_record.after, r_error))
  {
    writer.restore();
    return false;
  }
  std::vector<JournalRecord> journal = committed_records(pck_path);
  journal.push_back(journal_record);
  if (!write_journal(pck_path, journal, r_error)) {
    writer.restore();
    return false;
  }

  /* Commit. */
  if (!writer.write(0, journal_record.after, r_error) || !writer.sync(r_error)) {
    std::string ignored;
    writer.write(0, journal_record.before, &ignored);
    writer.restore();
    journal.pop_back();
    write_journal(pck_path, journal, &ignored);
    return false;
  }
  writer.close();

  /* Read everything back; undo on any difference. */
  std::string error;
  PckArchive check;
  bool ok = check.open(pck_path, &error);
  if (ok && check.entries().size() != final_entries.size()) {
    error = "Verification failed: the package does not have the expected number of files";
    ok = false;
  }
  for (size_t i = 0; ok && i < changes.size(); i++) {
    ok = verify_entry(
        check, mount, changes[i].path, changes[i].remove ? nullptr : &changes[i].data, &error);
  }
  /* The table keeps the order of the old one (by position: packages can name a file twice). */
  for (size_t i = 0, j = 0; ok && i < old_entries.size(); i++) {
    if (removed[i]) {
      continue;
    }
    const PckEntry &now = check.entries()[j++];
    const PckEntry &old = old_entries[i];
    if (now.path != old.path ||
        (!touched[i] && (now.offset != old.offset || now.size != old.size ||
                         now.compressed_size != old.compressed_size)))
    {
      error = "Verification failed: \"" + old.path_display + "\" changed";
      ok = false;
    }
  }
  check.close();
  archive.reset();
  if (!ok) {
    std::string undo_error;
    const bool undone = pck_undo_last_update(pck_path, &undo_error);
    set_error(r_error,
              error + (undone ? " (the package was restored)" :
                                " (restoring the package failed: " + undo_error + ")"));
    return false;
  }
  release_package_shared(pck_path);
  result.size = end;
  if (r_result) {
    *r_result = result;
  }
  return true;
}

namespace {

/**
 * Write a new package at `dst_pck`, laid out like `like`: the entries of `source` (copied as
 * stored) when given, with `changes` applied. The parts are written in a folder next to the
 * destination and moved in place once verified.
 */
bool write_new_package(const PckArchive &like,
                       const PckArchive *source,
                       const std::string &mount,
                       const std::string &dst_pck,
                       const std::vector<PckFileChange> &changes,
                       PckWriteResult *r_result,
                       std::string *r_error)
{
  if (!check_unique_paths(changes, r_error)) {
    return false;
  }
  if (like.is_encrypted()) {
    set_error(r_error, "Encrypted packages cannot be written");
    return false;
  }
  const bool v3 = like.version() == PckArchive::VERSION_2_3;
  std::vector<OutEntry> entries;
  PckWriteResult result;
  if (source) {
    std::vector<Bytes> records;
    if (!source->raw_records(records, r_error)) {
      return false;
    }
    entries.resize(records.size());
    for (size_t i = 0; i < records.size(); i++) {
      entries[i].record = std::move(records[i]);
      entries[i].write_data = true;
      entries[i].copy_from = &source->entries()[i];
    }
  }
  std::vector<bool> removed(entries.size(), false);
  for (const PckFileChange &change : changes) {
    const int64_t index = source ? find_entry(*source, mount, change.path) : -1;
    if (change.remove) {
      if (index >= 0) {
        removed[size_t(index)] = true;
        result.removed++;
      }
      continue;
    }
    OutEntry *entry = nullptr;
    if (index >= 0) {
      entry = &entries[size_t(index)];
      entry->copy_from = nullptr;
      result.written++;
    }
    else {
      entry = &entries.emplace_back();
      removed.push_back(false);
      if (!new_record(like, mount, change.path, entry->record, r_error)) {
        return false;
      }
      entry->write_data = true;
      result.added++;
    }
    entry->size = uint32_t(change.data.size());
    entry->stored = stored_form(change.data);
  }
  std::vector<OutEntry> final_entries;
  final_entries.reserve(entries.size());
  for (size_t i = 0; i < entries.size(); i++) {
    if (!removed[i]) {
      final_entries.push_back(std::move(entries[i]));
    }
  }

  const std::vector<std::string> dst_parts = existing_parts(dst_pck);
  release_package_shared(dst_pck);
  if (!check_not_in_use(dst_parts, r_error)) {
    return false;
  }

  std::error_code ec;
  const fs::path dst = u8path(dst_pck);
  const fs::path tmp_dir = dst.parent_path() / u8path(".gameforge-writing");
  fs::remove_all(tmp_dir, ec);
  fs::create_directories(tmp_dir, ec);
  const std::string tmp_pck = from_fs(tmp_dir / dst.filename());

  /* Same safe header style as the model package (none when it has none). */
  const PckLayout &layout = like.layout();
  PartWriter writer(tmp_pck, v3);
  uint64_t end = 0;
  Bytes header;
  bool ok = writer.create(r_error) && write_body(writer,
                                                 uint64_t(layout.safe_header_size),
                                                 final_entries,
                                                 like,
                                                 source,
                                                 end,
                                                 r_error);
  if (ok && layout.safe_header_size) {
    ok = make_safe_header(layout, end, header, r_error) && writer.write(0, header, r_error);
  }
  ok = ok && writer.sync(r_error);
  const std::vector<std::string> tmp_parts = writer.paths();
  writer.close();

  if (ok) {
    PckArchive check;
    ok = check.open(tmp_pck, r_error);
    if (ok && check.entries().size() != final_entries.size()) {
      set_error(r_error,
                "Verification failed: the package does not have the expected number of files");
      ok = false;
    }
    for (size_t i = 0; ok && i < changes.size(); i++) {
      if (!changes[i].remove) {
        ok = verify_entry(check, mount, changes[i].path, &changes[i].data, r_error);
      }
    }
  }
  if (!ok) {
    fs::remove_all(tmp_dir, ec);
    return false;
  }

  for (size_t i = 0; i < tmp_parts.size(); i++) {
    const fs::path target = u8path(part_path(dst_pck, int(i)));
    fs::rename(u8path(tmp_parts[i]), target, ec);
    if (ec) {
      set_error(r_error,
                "Cannot move the new package to \"" + from_fs(target) + "\": " + ec.message());
      return false;
    }
  }
  /* Parts of a previous package at the destination that the new one does not have. */
  for (size_t i = tmp_parts.size(); i < dst_parts.size(); i++) {
    fs::remove(u8path(dst_parts[i]), ec);
  }
  fs::remove_all(tmp_dir, ec);
  /* The journal of a previous package there does not apply any more. */
  fs::remove(u8path(journal_path(dst_pck)), ec);
  result.size = end;
  if (r_result) {
    *r_result = result;
  }
  return true;
}

}  // namespace

bool pck_write_patch(const std::string &like_pck,
                     const std::string &mount,
                     const std::string &dst_pck,
                     const std::vector<PckFileChange> &changes,
                     PckWriteResult *r_result,
                     std::string *r_error)
{
  std::error_code ec;
  if (fs::equivalent(u8path(like_pck), u8path(dst_pck), ec)) {
    set_error(r_error, "The patch must be written somewhere else than the original package");
    return false;
  }
  PckArchive like;
  if (!like.open(like_pck, r_error)) {
    return false;
  }
  return write_new_package(like, nullptr, mount, dst_pck, changes, r_result, r_error);
}

bool pck_write_copy(const std::string &src_pck,
                    const std::string &mount,
                    const std::string &dst_pck,
                    const std::vector<PckFileChange> &changes,
                    PckWriteResult *r_result,
                    std::string *r_error)
{
  std::error_code ec;
  if (fs::equivalent(u8path(src_pck), u8path(dst_pck), ec)) {
    set_error(r_error, "The copy must be written somewhere else than the original package");
    return false;
  }
  PckArchive source;
  if (!source.open(src_pck, r_error)) {
    return false;
  }
  return write_new_package(source, &source, mount, dst_pck, changes, r_result, r_error);
}

}  // namespace blender::io::angelica
