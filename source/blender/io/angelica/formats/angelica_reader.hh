/* SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup angelica
 *
 * Shared low level helpers for the Angelica Engine (Perfect World) file formats.
 *
 * Everything in `formats/` only depends on the C++ standard library and zlib, so the parsers can
 * be unit tested without a Blender build. Conversion into Blender data lives in `importer/`.
 *
 * All Angelica binary formats are little-endian. Strings written by `AFile::WriteString` are an
 * `int32` byte length followed by that many bytes, without a terminating NUL, encoded in GBK.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace blender::io::angelica {

using Bytes = std::vector<uint8_t>;
using ByteSpan = std::span<const uint8_t>;

/** Build a four-character code the way the engine does: `('A' << 24) | ('B' << 16) | ...`. */
constexpr uint32_t fourcc(char a, char b, char c, char d)
{
  return (uint32_t(uint8_t(a)) << 24) | (uint32_t(uint8_t(b)) << 16) |
         (uint32_t(uint8_t(c)) << 8) | uint32_t(uint8_t(d));
}

/**
 * Bounds checked little-endian reader over a byte buffer.
 *
 * Reading past the end never throws or crashes: it sets #failed() and returns zeroes, so parsers
 * can read a whole record and check once.
 */
class BinaryReader {
  ByteSpan data_;
  size_t pos_ = 0;
  bool failed_ = false;

 public:
  BinaryReader() = default;
  explicit BinaryReader(ByteSpan data) : data_(data) {}

  size_t tell() const
  {
    return pos_;
  }
  size_t size() const
  {
    return data_.size();
  }
  size_t remaining() const
  {
    return pos_ <= data_.size() ? data_.size() - pos_ : 0;
  }
  bool failed() const
  {
    return failed_;
  }
  bool at_end() const
  {
    return pos_ >= data_.size();
  }
  ByteSpan data() const
  {
    return data_;
  }

  bool seek(size_t pos)
  {
    if (pos > data_.size()) {
      failed_ = true;
      return false;
    }
    pos_ = pos;
    return true;
  }

  bool skip(size_t n)
  {
    return seek(pos_ + n);
  }

  /** Copy `n` bytes into `dst`. Fills `dst` with zeroes and fails when out of range. */
  bool read_raw(void *dst, size_t n)
  {
    if (n > remaining()) {
      std::memset(dst, 0, n);
      pos_ = data_.size();
      failed_ = true;
      return false;
    }
    std::memcpy(dst, data_.data() + pos_, n);
    pos_ += n;
    return true;
  }

  /** View of the next `n` bytes, advancing the cursor. Empty when out of range. */
  ByteSpan read_span(size_t n)
  {
    if (n > remaining()) {
      pos_ = data_.size();
      failed_ = true;
      return {};
    }
    ByteSpan result = data_.subspan(pos_, n);
    pos_ += n;
    return result;
  }

  /** Read a trivially copyable value. The engine only targets little-endian x86. */
  template<typename T> T read()
  {
    T value;
    read_raw(&value, sizeof(T));
    return value;
  }

  uint8_t u8()
  {
    return read<uint8_t>();
  }
  uint16_t u16()
  {
    return read<uint16_t>();
  }
  int16_t i16()
  {
    return read<int16_t>();
  }
  uint32_t u32()
  {
    return read<uint32_t>();
  }
  int32_t i32()
  {
    return read<int32_t>();
  }
  uint64_t u64()
  {
    return read<uint64_t>();
  }
  int64_t i64()
  {
    return read<int64_t>();
  }
  float f32()
  {
    return read<float>();
  }

  /** `AFile::ReadString`: `int32` length followed by raw (GBK) bytes. */
  std::string string_i32()
  {
    const int32_t len = i32();
    if (len < 0 || size_t(len) > remaining()) {
      failed_ = true;
      return {};
    }
    ByteSpan bytes = read_span(size_t(len));
    return std::string(reinterpret_cast<const char *>(bytes.data()), bytes.size());
  }

  /** Fixed size, NUL padded string field (for example `char szName[64]`). */
  std::string string_fixed(size_t n)
  {
    ByteSpan bytes = read_span(n);
    size_t len = 0;
    while (len < bytes.size() && bytes[len] != 0) {
      len++;
    }
    return std::string(reinterpret_cast<const char *>(bytes.data()), len);
  }
};

/**
 * Convert a GBK (code page 936) string, as used by every path and name in the Perfect World
 * client, into UTF-8. Pure ASCII input is returned unchanged. Bytes that cannot be decoded are
 * replaced with `?` so the result is always valid UTF-8.
 */
std::string gbk_to_utf8(std::string_view gbk);

/**
 * Normalize a path stored inside the engine: GBK decoded to UTF-8, `/` turned into `\`,
 * ASCII lower-cased and leading separators removed. This matches how the client looks files up
 * inside `.pck` packages (the lookup is case insensitive).
 */
std::string normalize_engine_path(std::string_view path_gbk_or_utf8, bool is_gbk = true);

/**
 * Inflate a zlib (RFC 1950) stream, falling back to raw deflate like the client tools do.
 * `expected_size` is a hint for the output buffer, zero when unknown.
 * Returns false when the data is not valid compressed data.
 */
bool zlib_inflate(ByteSpan src, size_t expected_size, Bytes &r_out);

/** Read a whole file from disk. Returns false when it cannot be opened or read. */
bool read_file(const std::string &filepath, Bytes &r_data);

}  // namespace blender::io::angelica
