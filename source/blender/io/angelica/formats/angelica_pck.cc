/* SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup angelica
 *
 * Angelica file package (`.pck`) layout, all little-endian:
 *
 * Logical stream
 *   The `.pck` file followed by `<base>.pkx`, `<base>.pkx1`, `<base>.pkx2`, ... in that order.
 *   The engine starts writing the `.pkx` once the `.pck` reaches 0x7fffff00 bytes, so the split
 *   point is the physical size of each part. All offsets below are in this logical stream.
 *
 * Safe header (offset 0, optional)
 *   u32 tag1    0x4DCA23EF (0x3F071506 for the "custom" update packages)
 *   u32 offset  end of the package data; a `.pck` can carry garbage after it.
 *   u32 tag2    0x56A089B7 (other known values: 0x56949FF7, 0x2A1E5540)
 *   Newer 64-bit clients write `{u32 tag1; u64 offset}` instead (no tag2), recognized by tag2
 *   not being one of the known values. Without a safe header the data end is the stream end.
 *
 * Trailer (ends at the data end `E`)
 *   ... FILEHEADER, i32 file_count, u32 version
 *
 *   version 0x00020001 / 0x00020002, FILEHEADER is 272 bytes at E - 280:
 *     u32 guard0, u32 version, u32 entry_offset ^ MASK, u32 flags, char description[252],
 *     u32 guard1
 *     (some packing tools let the description overwrite `flags`.)
 *   version 0x00020003, FILEHEADER is 280 bytes at E - 288:
 *     u32 guard0, u32 version, u64 entry_offset ^ MASK, u32 flags, char description[252],
 *     u32 guard1, u32 unknown
 *     The high half of entry_offset was written as zero by some tools and as the sign extension of
 *     the 32-bit mask (0xFFFFFFFF) by others; both are accepted.
 *
 * File table (at entry_offset, file_count records)
 *   u32 stored_size ^ MASK
 *   u32 stored_size ^ MASK ^ CHECK
 *   stored_size bytes: a FILEENTRY, zlib compressed unless stored_size == sizeof(FILEENTRY).
 *
 *   FILEENTRY for 0x00020001 / 0x00020002 (276 bytes):
 *     char name[260] (GBK), u32 offset, u32 size, u32 compressed_size, i32 access_count
 *   FILEENTRY for 0x00020003 (288 bytes):
 *     char name[260] (GBK), u32 unknown, u64 offset, u32 size, u32 compressed_size,
 *     i32 access_count, u32 unknown
 *
 * File data
 *   compressed_size bytes at offset; zlib compressed when compressed_size < size. When the
 *   FILEHEADER flags have bit 31 set (PACKFLAG_ENCRYPT) the stored bytes are additionally
 *   scrambled in 4-byte big-endian groups with `rotl16(x) ^ (stored_len + 0x739802AB)`.
 *
 * Keys (`AFilePackMan::SetAlgorithmID(id)`, 32-bit wrap-around arithmetic):
 *   GUARD0 = 0xFDFDFEEE + id * 0x072341F2     GUARD1 = 0xF00DBEEF + id * 0x01237A73
 *   MASK   = 0xA8937462 + id * 0x0AB2321F     CHECK  = 0x59374231 + id * 0x0987A223
 *   id 111 uses a fixed set, and "update" packages of some clients use another fixed set. The key
 *   set is detected from the guard words, so packages of any algorithm id open without
 *   configuration. Perfect World retail clients use id 0; the 1.7.8 client keeps the id 0 guard
 *   words with MASK 0xA7F88E3B and CHECK 0x4B813BDD, so every key set whose guards match is
 *   checked against the first record of the file table.
 *
 * Package versions 0x0001xxxx (Angelica 1, 2002) predate the trailer layout and are rejected.
 */

#include "angelica_pck.hh"

#include <algorithm>
#include <cstring>
#include <filesystem>

#ifdef _WIN32
#  include <io.h>
#  include <stdio.h>
#endif

namespace blender::io::angelica {

namespace {

constexpr uint32_t SAFE_TAG1 = 0x4DCA23EF;
constexpr uint32_t SAFE_TAG1_CUSTOM = 0x3F071506;
constexpr uint32_t SAFE_TAG2_KNOWN[] = {0x56A089B7, 0x56949FF7, 0x2A1E5540};

constexpr uint32_t PACKFLAG_ENCRYPT = 0x80000000;
constexpr size_t NAME_LEN = 260;
constexpr size_t ENTRY_SIZE_V2 = 276;
constexpr size_t ENTRY_SIZE_V3 = 288;
constexpr size_t HEADER_SIZE_V2 = 272;
constexpr size_t HEADER_SIZE_V3 = 280;
constexpr size_t DESCRIPTION_LEN = 252;

/** Engine limit for one entry, see `AFilePackage::CheckFileEntryValid`. */
constexpr uint32_t MAX_ENTRY_DATA = 0x7fffff00u;
/** Sanity limit for the number of entries (the largest retail package has ~200k). */
constexpr int32_t MAX_ENTRY_COUNT = 16 * 1024 * 1024;

struct KeySet {
  uint32_t guard0, guard1, mask, check;
  int64_t id;
};

KeySet keys_for_id(const uint32_t id)
{
  if (id == 111) {
    return {0xab12908f, 0xb3231902, 0x2a63810e, 0x18734563, 111};
  }
  return {0xfdfdfeeeu + id * 0x72341f2u,
          0xf00dbeefu + id * 0x1237a73u,
          0xa8937462u + id * 0xab2321fu,
          0x59374231u + id * 0x987a223u,
          int64_t(int32_t(id))};
}

/** Multiplicative inverse of an odd number modulo 2^32 (Newton iteration). */
uint32_t inverse_mod_2_32(const uint32_t a)
{
  uint32_t x = a;
  for (int i = 0; i < 5; i++) {
    x *= 2u - a * x;
  }
  return x;
}

/** Find the key set whose guard words match, or return false. */
bool detect_keys(const uint32_t guard0, const uint32_t guard1, KeySet &r_keys)
{
  /* Fixed sets first. */
  const KeySet fixed[] = {
      keys_for_id(0),
      keys_for_id(111),
      {0x01207940, 0x549D22B4, 0x350DAA8C, 0x3E367B13, -1},
  };
  for (const KeySet &k : fixed) {
    if (k.guard0 == guard0 && k.guard1 == guard1) {
      r_keys = k;
      return true;
    }
  }
  /* GUARD1 is linear in the id with an odd factor, so the id can be solved for. */
  const uint32_t id = (guard1 - 0xf00dbeefu) * inverse_mod_2_32(0x1237a73u);
  const KeySet k = keys_for_id(id);
  if (k.guard0 == guard0 && k.guard1 == guard1) {
    r_keys = k;
    return true;
  }
  return false;
}

/**
 * Key sets to try for a package with these guard words. Some clients keep the guard words of a
 * key set but change its mask and check words (the 1.7.8 client uses the id 0 guards with
 * `MASK 0xA7F88E3B`, `CHECK 0x4B813BDD`), so the guards alone do not decide: the caller checks
 * each candidate against the first record of the file table.
 */
std::vector<KeySet> candidate_keys(const uint32_t guard0, const uint32_t guard1)
{
  std::vector<KeySet> result;
  KeySet keys;
  if (!detect_keys(guard0, guard1, keys)) {
    return result;
  }
  result.push_back(keys);
  if (keys.id == 0) {
    result.push_back({keys.guard0, keys.guard1, 0xA7F88E3Bu, 0x4B813BDDu, 0});
  }
  return result;
}

void set_error(std::string *r_error, std::string msg)
{
  if (r_error) {
    *r_error = std::move(msg);
  }
}

std::string hex32(const uint32_t v)
{
  char buf[16];
  std::snprintf(buf, sizeof(buf), "0x%08X", unsigned(v));
  return buf;
}

uint32_t load_u32(const uint8_t *p)
{
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

uint64_t load_u64(const uint8_t *p)
{
  return uint64_t(load_u32(p)) | (uint64_t(load_u32(p + 4)) << 32);
}

std::FILE *open_utf8(const std::string &filepath)
{
#ifdef _WIN32
  const std::filesystem::path p(
      std::u8string(reinterpret_cast<const char8_t *>(filepath.data()), filepath.size()));
  return _wfopen(p.c_str(), L"rb");
#else
  return std::fopen(filepath.c_str(), "rb");
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

bool file_size64(std::FILE *f, uint64_t &r_size)
{
#ifdef _WIN32
  if (_fseeki64(f, 0, SEEK_END) != 0) {
    return false;
  }
  const int64_t size = _ftelli64(f);
#else
  if (fseeko(f, 0, SEEK_END) != 0) {
    return false;
  }
  const int64_t size = int64_t(ftello(f));
#endif
  if (size < 0) {
    return false;
  }
  r_size = uint64_t(size);
  return true;
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

/**
 * Collect the continuation parts next to `pck_path`. Matching is case-insensitive so that
 * `Models.PKX` is found on case-sensitive file systems.
 */
std::vector<std::string> find_continuations(const std::string &pck_path)
{
  std::vector<std::string> result;
  std::error_code ec;
  const std::filesystem::path pck = u8path(pck_path);
  std::filesystem::path dir = pck.parent_path();
  if (dir.empty()) {
    dir = ".";
  }
  const std::string stem_lower = ascii_lower(path_to_utf8(pck.stem()));

  std::unordered_map<std::string, std::string> by_lower_name;
  for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    const std::string name = path_to_utf8(it->path().filename());
    by_lower_name.emplace(ascii_lower(name), path_to_utf8(it->path()));
  }
  for (int i = 0;; i++) {
    const std::string name = stem_lower + ".pkx" + (i == 0 ? std::string() : std::to_string(i));
    const auto found = by_lower_name.find(name);
    if (found == by_lower_name.end()) {
      break;
    }
    result.push_back(found->second);
  }
  return result;
}

void decrypt_data(uint8_t *data, const size_t len)
{
  const uint32_t mask = uint32_t(len) + 0x739802abu;
  for (size_t i = 0; i + 3 < len; i += 4) {
    uint32_t v = (uint32_t(data[i]) << 24) | (uint32_t(data[i + 1]) << 16) |
                 (uint32_t(data[i + 2]) << 8) | uint32_t(data[i + 3]);
    v = (v << 16) | (v >> 16);
    v ^= mask;
    data[i] = uint8_t(v >> 24);
    data[i + 1] = uint8_t(v >> 16);
    data[i + 2] = uint8_t(v >> 8);
    data[i + 3] = uint8_t(v);
  }
}

/** True when 4 bytes look like text, meaning the description overwrote the flags field. */
bool looks_like_text(const uint8_t *p)
{
  for (int i = 0; i < 4; i++) {
    if (p[i] < 0x20 || p[i] >= 0x7f) {
      return false;
    }
  }
  return true;
}

std::string fixed_string(const uint8_t *p, const size_t max_len)
{
  size_t len = 0;
  while (len < max_len && p[len] != 0) {
    len++;
  }
  return std::string(reinterpret_cast<const char *>(p), len);
}

/**
 * Decode the `count` records of a file table, calling `fn(record)` with the `entry_size` bytes of
 * each `FILEENTRY` (inflated when stored compressed). False with `r_error` on the first bad record.
 */
template<typename Fn>
bool decode_table(const ByteSpan table,
                  const int32_t count,
                  const size_t entry_size,
                  const KeySet &keys,
                  std::string &r_error,
                  Fn &&fn)
{
  BinaryReader reader(table);
  Bytes inflated;
  for (int32_t i = 0; i < count; i++) {
    const uint32_t stored = reader.u32() ^ keys.mask;
    const uint32_t check = reader.u32() ^ keys.mask ^ keys.check;
    if (reader.failed()) {
      r_error = "file table truncated at entry " + std::to_string(i);
      return false;
    }
    if (stored != check) {
      r_error = "file table check failed at entry " + std::to_string(i);
      return false;
    }
    if (stored == 0 || stored > entry_size + 1024) {
      r_error = "invalid record size at entry " + std::to_string(i);
      return false;
    }
    const ByteSpan record_data = reader.read_span(stored);
    if (reader.failed()) {
      r_error = "file table truncated at entry " + std::to_string(i);
      return false;
    }
    const uint8_t *rec = record_data.data();
    if (stored != entry_size) {
      if (!zlib_inflate(record_data, entry_size, inflated)) {
        r_error = "cannot decompress file table entry " + std::to_string(i);
        return false;
      }
      if (inflated.size() < entry_size - 8) {
        r_error = "file table entry " + std::to_string(i) + " too short";
        return false;
      }
      inflated.resize(std::max(inflated.size(), entry_size), 0);
      rec = inflated.data();
    }
    fn(rec);
  }
  return true;
}

}  // namespace

PckArchive::PckArchive() = default;

PckArchive::~PckArchive()
{
  close();
}

void PckArchive::close()
{
  std::lock_guard lock(io_mutex_);
  for (Part &part : parts_) {
    if (part.file) {
      std::fclose(part.file);
    }
  }
  parts_.clear();
  stream_size_ = 0;
  version_ = 0;
  algorithm_id_ = 0;
  encrypted_ = false;
  description_.clear();
  keys_ = {};
  layout_ = {};
  entries_.clear();
  lookup_.clear();
}

bool PckArchive::is_open() const
{
  return !parts_.empty();
}

std::vector<std::string> PckArchive::part_paths() const
{
  std::vector<std::string> result;
  for (const Part &part : parts_) {
    result.push_back(part.path);
  }
  return result;
}

std::vector<uint64_t> PckArchive::part_sizes() const
{
  std::vector<uint64_t> result;
  for (const Part &part : parts_) {
    result.push_back(part.size);
  }
  return result;
}

bool PckArchive::read_at(const uint64_t offset, void *dst, const size_t size) const
{
  if (size == 0) {
    return true;
  }
  if (offset > stream_size_ || size > stream_size_ - offset) {
    return false;
  }
  std::lock_guard lock(io_mutex_);
  uint8_t *out = static_cast<uint8_t *>(dst);
  uint64_t pos = offset;
  size_t done = 0;
  for (const Part &part : parts_) {
    if (done == size) {
      break;
    }
    if (pos >= part.start + part.size) {
      continue;
    }
    const uint64_t in_part = pos - part.start;
    const size_t chunk = size_t(std::min<uint64_t>(size - done, part.size - in_part));
    if (!seek64(part.file, in_part) || std::fread(out + done, 1, chunk, part.file) != chunk) {
      return false;
    }
    done += chunk;
    pos += chunk;
  }
  return done == size;
}

bool PckArchive::open(const std::string &filepath, std::string *r_error)
{
  close();

  /* Open all parts of the logical stream. */
  std::vector<std::string> paths = {filepath};
  for (std::string &p : find_continuations(filepath)) {
    paths.push_back(std::move(p));
  }
  for (const std::string &p : paths) {
    Part part;
    part.path = p;
    part.file = open_utf8(p);
    if (part.file == nullptr) {
      close();
      set_error(r_error, "Cannot open package file \"" + p + "\"");
      return false;
    }
    if (!file_size64(part.file, part.size)) {
      std::fclose(part.file);
      close();
      set_error(r_error, "Cannot determine the size of \"" + p + "\"");
      return false;
    }
    part.start = stream_size_;
    stream_size_ += part.size;
    parts_.push_back(part);
  }

  auto fail = [&](std::string msg) {
    close();
    set_error(r_error, "Package \"" + filepath + "\": " + msg);
    return false;
  };

  if (stream_size_ < 8 + HEADER_SIZE_V2) {
    return fail("file too small to be a package");
  }

  /* Determine where the package data ends. */
  uint64_t data_end = stream_size_;
  {
    uint8_t safe[16] = {};
    if (!read_at(0, safe, sizeof(safe))) {
      return fail("cannot read file header");
    }
    const uint32_t tag1 = load_u32(safe);
    if (tag1 == SAFE_TAG1 || tag1 == SAFE_TAG1_CUSTOM) {
      auto is_known_tag2 = [](const uint32_t tag) {
        return std::find(std::begin(SAFE_TAG2_KNOWN), std::end(SAFE_TAG2_KNOWN), tag) !=
               std::end(SAFE_TAG2_KNOWN);
      };
      const bool known_tag2 = is_known_tag2(load_u32(safe + 8));
      const uint64_t end = known_tag2 ? uint64_t(load_u32(safe + 4)) : load_u64(safe + 4);
      /* Only trust it when it points inside the stream. */
      if (end >= 8 + HEADER_SIZE_V2 && end <= stream_size_) {
        data_end = end;
        layout_.safe_header_u64 = !known_tag2;
        /* 64-bit ends are followed by the second tag in some clients (1.7.8). */
        layout_.safe_header_size = (!known_tag2 && is_known_tag2(load_u32(safe + 12))) ? 16 : 12;
        std::memcpy(layout_.safe_header, safe, sizeof(safe));
      }
    }
  }
  layout_.data_end = data_end;

  uint8_t trailer[8];
  if (!read_at(data_end - 8, trailer, 8)) {
    return fail("cannot read package trailer");
  }
  const int32_t file_count = int32_t(load_u32(trailer));
  version_ = load_u32(trailer + 4);

  size_t header_size, entry_size;
  if (version_ == VERSION_2_1 || version_ == VERSION_2_2) {
    header_size = HEADER_SIZE_V2;
    entry_size = ENTRY_SIZE_V2;
  }
  else if (version_ == VERSION_2_3) {
    header_size = HEADER_SIZE_V3;
    entry_size = ENTRY_SIZE_V3;
  }
  else {
    return fail("unsupported package version " + hex32(version_));
  }
  if (file_count < 0 || file_count > MAX_ENTRY_COUNT) {
    return fail("invalid file count " + std::to_string(file_count));
  }
  if (data_end < 8 + header_size) {
    return fail("file too small for its header");
  }

  const uint64_t header_pos = data_end - 8 - header_size;
  std::vector<uint8_t> header(header_size);
  if (!read_at(header_pos, header.data(), header_size)) {
    return fail("cannot read package header");
  }
  const uint8_t *h = header.data();
  const uint32_t guard0 = load_u32(h);
  const bool is_v3 = version_ == VERSION_2_3;
  const size_t flags_pos = is_v3 ? 16 : 12;
  const size_t guard1_pos = flags_pos + 4 + DESCRIPTION_LEN;
  const uint32_t guard1 = load_u32(h + guard1_pos);

  const std::vector<KeySet> candidates = candidate_keys(guard0, guard1);
  if (candidates.empty()) {
    return fail("unknown package key (guard words " + hex32(guard0) + ", " + hex32(guard1) + ")");
  }
  layout_.file_header.assign(header.begin(), header.end());

  if (looks_like_text(h + flags_pos)) {
    encrypted_ = false;
    description_ = gbk_to_utf8(fixed_string(h + flags_pos, DESCRIPTION_LEN + 4));
  }
  else {
    encrypted_ = (load_u32(h + flags_pos) & PACKFLAG_ENCRYPT) != 0;
    description_ = gbk_to_utf8(fixed_string(h + flags_pos + 4, DESCRIPTION_LEN));
  }

  /* Locate the file table with a key set: false when the offset falls outside the package. */
  auto table_offset = [&](const KeySet &k, uint64_t &r_offset) {
    const uint32_t low = load_u32(h + 8) ^ k.mask;
    if (is_v3) {
      const uint32_t high_raw = load_u32(h + 12);
      /* Zero-extended mask, sign-extended mask, then the low half alone. */
      const uint64_t offsets[] = {
          (uint64_t(high_raw) << 32) | low,
          (uint64_t(high_raw ^ 0xFFFFFFFFu) << 32) | low,
          uint64_t(low),
      };
      for (const uint64_t offset : offsets) {
        if (offset <= header_pos) {
          r_offset = offset;
          return true;
        }
      }
      return false;
    }
    r_offset = low;
    return low <= header_pos;
  };

  /* The right key set decodes a consistent first record (both size words agree). */
  KeySet keys;
  uint64_t entry_offset = 0;
  bool found_keys = false;
  for (const KeySet &k : candidates) {
    uint64_t offset;
    if (!table_offset(k, offset)) {
      continue;
    }
    if (file_count > 0) {
      uint8_t first[8];
      if (!read_at(offset, first, sizeof(first))) {
        continue;
      }
      const uint32_t stored = load_u32(first) ^ k.mask;
      const uint32_t check = load_u32(first + 4) ^ k.mask ^ k.check;
      if (stored != check || stored == 0 || stored > entry_size + 1024) {
        continue;
      }
    }
    keys = k;
    entry_offset = offset;
    found_keys = true;
    break;
  }
  if (!found_keys) {
    return fail("cannot decode the file table with the package key (guard words " +
                hex32(guard0) + ", " + hex32(guard1) + ")");
  }
  algorithm_id_ = keys.id;
  keys_ = {keys.guard0, keys.guard1, keys.mask, keys.check, keys.id};
  layout_.entry_offset = entry_offset;
  if (is_v3) {
    layout_.entry_offset_sign_extended = (load_u32(h + 12) ^ uint32_t(entry_offset >> 32)) ==
                                         0xFFFFFFFFu;
  }

  /* Read the whole file table in one go; it sits between the data and the header. */
  const uint64_t table_size = header_pos - entry_offset;
  if (table_size > (uint64_t(1) << 31)) {
    return fail("file table too large");
  }
  if (uint64_t(file_count) * 8 > table_size) {
    return fail("file table too small for " + std::to_string(file_count) + " entries");
  }
  Bytes table(static_cast<size_t>(table_size));
  if (!read_at(entry_offset, table.data(), table.size())) {
    return fail("cannot read file table");
  }

  entries_.reserve(size_t(file_count));
  std::string table_error;
  const bool table_ok = decode_table(table, file_count, entry_size, keys, table_error, [&](const uint8_t *rec) {
    PckEntry entry;
    const std::string name_gbk = fixed_string(rec, NAME_LEN);
    if (is_v3) {
      entry.offset = load_u64(rec + 264);
      entry.size = load_u32(rec + 272);
      entry.compressed_size = load_u32(rec + 276);
    }
    else {
      entry.offset = load_u32(rec + 260);
      entry.size = load_u32(rec + 264);
      entry.compressed_size = load_u32(rec + 268);
    }
    entry.path = normalize_engine_path(name_gbk, true);
    entry.path_display = gbk_to_utf8(name_gbk);
    std::replace(entry.path_display.begin(), entry.path_display.end(), '/', '\\');
    lookup_.emplace(entry.path, entries_.size());
    entries_.push_back(std::move(entry));
  });
  if (!table_ok) {
    return fail(table_error);
  }
  return true;
}

bool PckArchive::raw_records(std::vector<Bytes> &r_records, std::string *r_error) const
{
  r_records.clear();
  if (!is_open()) {
    set_error(r_error, "Package is not open");
    return false;
  }
  const bool is_v3 = version_ == VERSION_2_3;
  const size_t header_size = is_v3 ? HEADER_SIZE_V3 : HEADER_SIZE_V2;
  const size_t entry_size = is_v3 ? ENTRY_SIZE_V3 : ENTRY_SIZE_V2;
  const uint64_t header_pos = layout_.data_end - 8 - header_size;
  Bytes table(size_t(header_pos - layout_.entry_offset));
  if (!read_at(layout_.entry_offset, table.data(), table.size())) {
    set_error(r_error, "Cannot read the file table");
    return false;
  }
  const KeySet keys = {keys_.guard0, keys_.guard1, keys_.mask, keys_.check, keys_.id};
  r_records.reserve(entries_.size());
  std::string error;
  if (!decode_table(table, int32_t(entries_.size()), entry_size, keys, error, [&](const uint8_t *rec) {
        r_records.emplace_back(rec, rec + entry_size);
      }))
  {
    set_error(r_error, error);
    return false;
  }
  return true;
}

bool PckArchive::read_stored(const PckEntry &entry, Bytes &r_data) const
{
  r_data.resize(entry.compressed_size);
  return read_at(entry.offset, r_data.data(), r_data.size());
}

const PckEntry *PckArchive::find(const std::string_view path) const
{
  const auto it = lookup_.find(normalize_engine_path(path, false));
  return it == lookup_.end() ? nullptr : &entries_[it->second];
}

bool PckArchive::read(const std::string_view path, Bytes &r_data, std::string *r_error) const
{
  const PckEntry *entry = find(path);
  if (entry == nullptr) {
    set_error(r_error, "File \"" + std::string(path) + "\" not found in package");
    return false;
  }
  return read(*entry, r_data, r_error);
}

bool PckArchive::read(const PckEntry &entry, Bytes &r_data, std::string *r_error) const
{
  r_data.clear();
  if (!is_open()) {
    set_error(r_error, "Package is not open");
    return false;
  }
  if (entry.size > MAX_ENTRY_DATA || entry.compressed_size > MAX_ENTRY_DATA) {
    set_error(r_error, "Invalid sizes for \"" + entry.path_display + "\"");
    return false;
  }
  if (entry.offset > stream_size_ || entry.compressed_size > stream_size_ - entry.offset) {
    set_error(r_error, "Data of \"" + entry.path_display + "\" lies outside the package");
    return false;
  }

  Bytes stored(entry.compressed_size);
  if (!read_at(entry.offset, stored.data(), stored.size())) {
    set_error(r_error, "Cannot read data of \"" + entry.path_display + "\"");
    return false;
  }
  if (encrypted_) {
    decrypt_data(stored.data(), stored.size());
  }
  if (entry.compressed_size > entry.size) {
    /* Written by a tool that compresses every file, also the ones that grow (`AFilePackGame`
     * only decompresses when the stored size is smaller, so the game reads these as raw bytes and
     * gets garbage). Recover the real content when it inflates to the expected size, otherwise
     * return what the game reads. */
    Bytes inflated;
    if (zlib_inflate(stored, entry.size, inflated) && inflated.size() == entry.size) {
      r_data = std::move(inflated);
    }
    else {
      stored.resize(entry.size);
      r_data = std::move(stored);
    }
    return true;
  }
  if (!entry.is_compressed()) {
    r_data = std::move(stored);
    return true;
  }
  if (!zlib_inflate(stored, entry.size, r_data)) {
    set_error(r_error, "Cannot decompress \"" + entry.path_display + "\"");
    return false;
  }
  if (r_data.size() != entry.size) {
    set_error(r_error,
              "Decompressed size of \"" + entry.path_display + "\" is " +
                  std::to_string(r_data.size()) + ", expected " + std::to_string(entry.size));
    r_data.clear();
    return false;
  }
  return true;
}

/* -------------------------------------------------------------------- */
/** \name Shared Packages
 * \{ */

namespace {

struct SharedPackage {
  std::shared_ptr<PckArchive> archive;
  uint64_t size = 0;
  int64_t mtime = 0;
};

std::mutex &shared_packages_mutex()
{
  static std::mutex mutex;
  return mutex;
}

std::unordered_map<std::string, SharedPackage> &shared_packages()
{
  static std::unordered_map<std::string, SharedPackage> packages;
  return packages;
}

std::string shared_package_key(const std::string &pck_path)
{
  std::error_code ec;
  std::filesystem::path path = std::filesystem::absolute(u8path(pck_path), ec);
  if (ec) {
    path = u8path(pck_path);
  }
  return ascii_lower(path_to_utf8(path.lexically_normal()));
}

bool package_file_stamp(const std::string &pck_path, uint64_t &r_size, int64_t &r_mtime)
{
  std::error_code ec;
  const std::filesystem::path path = u8path(pck_path);
  const uint64_t size = std::filesystem::file_size(path, ec);
  if (ec) {
    return false;
  }
  const auto time = std::filesystem::last_write_time(path, ec);
  if (ec) {
    return false;
  }
  r_size = size;
  r_mtime = int64_t(time.time_since_epoch().count());
  return true;
}

}  // namespace

std::shared_ptr<PckArchive> open_package_shared(const std::string &pck_path, std::string *r_error)
{
  uint64_t size = 0;
  int64_t mtime = 0;
  if (!package_file_stamp(pck_path, size, mtime)) {
    set_error(r_error, "Cannot open package file \"" + pck_path + "\"");
    return nullptr;
  }
  const std::string key = shared_package_key(pck_path);
  {
    std::lock_guard lock(shared_packages_mutex());
    const auto it = shared_packages().find(key);
    if (it != shared_packages().end() && it->second.size == size && it->second.mtime == mtime) {
      return it->second.archive;
    }
  }
  auto archive = std::make_shared<PckArchive>();
  if (!archive->open(pck_path, r_error)) {
    return nullptr;
  }
  std::lock_guard lock(shared_packages_mutex());
  shared_packages()[key] = {archive, size, mtime};
  return archive;
}

void release_package_shared(const std::string &pck_path)
{
  std::lock_guard lock(shared_packages_mutex());
  shared_packages().erase(shared_package_key(pck_path));
}

/** \} */

}  // namespace blender::io::angelica
