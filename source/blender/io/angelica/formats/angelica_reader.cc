/* SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup angelica
 */

#include "angelica_reader.hh"

#include <algorithm>
#include <cerrno>
#include <cstdio>

#include <zlib.h>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <iconv.h>
#endif

namespace blender::io::angelica {

static bool is_ascii(std::string_view s)
{
  for (const char c : s) {
    if (uint8_t(c) >= 0x80) {
      return false;
    }
  }
  return true;
}

#ifdef _WIN32

static std::string wide_to_utf8(const std::wstring &wide)
{
  if (wide.empty()) {
    return {};
  }
  const int len = WideCharToMultiByte(
      CP_UTF8, 0, wide.data(), int(wide.size()), nullptr, 0, nullptr, nullptr);
  std::string out(size_t(len), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), int(wide.size()), out.data(), len, nullptr, nullptr);
  return out;
}

std::string gbk_to_utf8(std::string_view gbk)
{
  if (is_ascii(gbk)) {
    return std::string(gbk);
  }
  const int len = MultiByteToWideChar(936, 0, gbk.data(), int(gbk.size()), nullptr, 0);
  if (len <= 0) {
    std::string out(gbk);
    for (char &c : out) {
      if (uint8_t(c) >= 0x80) {
        c = '?';
      }
    }
    return out;
  }
  std::wstring wide(size_t(len), L'\0');
  MultiByteToWideChar(936, 0, gbk.data(), int(gbk.size()), wide.data(), len);
  return wide_to_utf8(wide);
}

#else

std::string gbk_to_utf8(std::string_view gbk)
{
  if (is_ascii(gbk)) {
    return std::string(gbk);
  }
  /* GB18030 is a superset of GBK and is what the client effectively accepts. */
  iconv_t cd = iconv_open("UTF-8", "GB18030");
  if (cd == iconv_t(-1)) {
    cd = iconv_open("UTF-8", "GBK");
  }
  std::string out;
  if (cd == iconv_t(-1)) {
    out.assign(gbk);
    for (char &c : out) {
      if (uint8_t(c) >= 0x80) {
        c = '?';
      }
    }
    return out;
  }

  std::string in(gbk);
  char *in_ptr = in.data();
  size_t in_left = in.size();
  out.reserve(in.size() * 3 / 2 + 4);
  char buf[256];
  while (in_left > 0) {
    char *out_ptr = buf;
    size_t out_left = sizeof(buf);
    const size_t res = iconv(cd, &in_ptr, &in_left, &out_ptr, &out_left);
    out.append(buf, sizeof(buf) - out_left);
    if (res == size_t(-1)) {
      if (errno == E2BIG) {
        continue;
      }
      /* Invalid or truncated multi-byte sequence: emit a replacement and skip one byte. */
      out.push_back('?');
      in_ptr++;
      in_left--;
    }
  }
  iconv_close(cd);
  return out;
}

#endif

std::string normalize_engine_path(std::string_view path, const bool is_gbk)
{
  std::string out = is_gbk ? gbk_to_utf8(path) : std::string(path);
  for (char &c : out) {
    if (c == '/') {
      c = '\\';
    }
    else if (c >= 'A' && c <= 'Z') {
      c = char(c - 'A' + 'a');
    }
  }
  size_t start = 0;
  while (start < out.size() && (out[start] == '\\' || out[start] == '.')) {
    /* Strip leading `\` and `.\` but keep names that start with a dot followed by text. */
    if (out[start] == '.' && !(start + 1 < out.size() && out[start + 1] == '\\')) {
      break;
    }
    start++;
  }
  return out.substr(start);
}

static bool inflate_impl(ByteSpan src, const size_t expected_size, Bytes &r_out, const int bits)
{
  z_stream zs = {};
  if (inflateInit2(&zs, bits) != Z_OK) {
    return false;
  }
  r_out.clear();
  r_out.resize(expected_size > 0 ? expected_size : std::max<size_t>(src.size() * 4, 1024));
  zs.next_in = const_cast<Bytef *>(src.data());
  zs.avail_in = uInt(src.size());
  int ret = Z_OK;
  while (true) {
    if (zs.total_out >= r_out.size()) {
      r_out.resize(r_out.size() * 2);
    }
    zs.next_out = r_out.data() + zs.total_out;
    zs.avail_out = uInt(r_out.size() - zs.total_out);
    ret = inflate(&zs, Z_NO_FLUSH);
    if (ret == Z_STREAM_END) {
      break;
    }
    if (ret != Z_OK && ret != Z_BUF_ERROR) {
      break;
    }
    if (ret == Z_BUF_ERROR && zs.avail_in == 0) {
      /* Truncated input. */
      break;
    }
  }
  const size_t produced = zs.total_out;
  inflateEnd(&zs);
  if (ret != Z_STREAM_END) {
    r_out.clear();
    return false;
  }
  r_out.resize(produced);
  return true;
}

bool zlib_inflate(ByteSpan src, const size_t expected_size, Bytes &r_out)
{
  if (inflate_impl(src, expected_size, r_out, MAX_WBITS)) {
    return true;
  }
  return inflate_impl(src, expected_size, r_out, -MAX_WBITS);
}

bool read_file(const std::string &filepath, Bytes &r_data)
{
#ifdef _WIN32
  const int wlen = MultiByteToWideChar(CP_UTF8, 0, filepath.c_str(), -1, nullptr, 0);
  std::wstring wpath(size_t(std::max(wlen, 1)), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, filepath.c_str(), -1, wpath.data(), wlen);
  FILE *f = _wfopen(wpath.c_str(), L"rb");
#else
  FILE *f = std::fopen(filepath.c_str(), "rb");
#endif
  if (f == nullptr) {
    return false;
  }
  r_data.clear();
  uint8_t buf[1 << 16];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
    r_data.insert(r_data.end(), buf, buf + n);
  }
  const bool ok = !std::ferror(f);
  std::fclose(f);
  return ok;
}

}  // namespace blender::io::angelica
