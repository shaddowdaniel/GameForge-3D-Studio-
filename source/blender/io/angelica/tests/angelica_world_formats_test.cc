/* SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup angelica
 *
 * Tests for the world formats: `.pck` packages, `A3DTerrain2` terrain and server `.hmap` files.
 * Files are synthesized in memory with small writers. When the environment variable
 * `ANGELICA_TEST_DATA` points at a client `element` folder, the parsers are additionally run on
 * the real files found there.
 */

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>

#include <zlib.h>

#include "testing/testing.h"

#include "../formats/angelica_hmap.hh"
#include "../formats/angelica_pck.hh"
#include "../formats/angelica_terrain.hh"

namespace blender::io::angelica::tests {

namespace fs = std::filesystem;

/* -------------------------------------------------------------------- */
/** \name Helpers
 * \{ */

class Writer {
 public:
  Bytes data;

  template<typename T> Writer &put(const T &value)
  {
    const uint8_t *p = reinterpret_cast<const uint8_t *>(&value);
    data.insert(data.end(), p, p + sizeof(T));
    return *this;
  }
  Writer &u8(const uint8_t v)
  {
    return put(v);
  }
  Writer &u16(const uint16_t v)
  {
    return put(v);
  }
  Writer &i16(const int16_t v)
  {
    return put(v);
  }
  Writer &u32(const uint32_t v)
  {
    return put(v);
  }
  Writer &i32(const int32_t v)
  {
    return put(v);
  }
  Writer &u64(const uint64_t v)
  {
    return put(v);
  }
  Writer &f32(const float v)
  {
    return put(v);
  }
  Writer &bytes(ByteSpan b)
  {
    data.insert(data.end(), b.begin(), b.end());
    return *this;
  }
  Writer &zeros(const size_t n)
  {
    data.insert(data.end(), n, 0);
    return *this;
  }
  Writer &string_i32(const std::string &s)
  {
    i32(int32_t(s.size()));
    data.insert(data.end(), s.begin(), s.end());
    return *this;
  }
  Writer &fixed_string(const std::string &s, const size_t n)
  {
    Bytes buf(n, 0);
    std::memcpy(buf.data(), s.data(), std::min(n, s.size()));
    return bytes(buf);
  }
  size_t size() const
  {
    return data.size();
  }
  void patch_u32(const size_t pos, const uint32_t v)
  {
    std::memcpy(&data[pos], &v, 4);
  }
};

static Bytes zlib_compress(ByteSpan src)
{
  uLongf len = compressBound(uLong(src.size()));
  Bytes out(len);
  EXPECT_EQ(compress2(out.data(), &len, src.data(), uLong(src.size()), 6), Z_OK);
  out.resize(len);
  return out;
}

static Bytes floats_to_bytes(const std::vector<float> &values)
{
  Bytes out(values.size() * 4);
  std::memcpy(out.data(), values.data(), out.size());
  return out;
}

/** A fresh, empty temporary directory, removed on destruction. */
class TempDir {
 public:
  fs::path path;

  TempDir()
  {
    static std::atomic<int> counter = 0;
    std::random_device rd;
    path = fs::temp_directory_path() /
           ("angelica_world_test_" + std::to_string(rd()) + "_" + std::to_string(counter++));
    fs::create_directories(path);
  }
  ~TempDir()
  {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
  std::string str() const
  {
    return path.string();
  }
  std::string write(const std::string &name, ByteSpan data) const
  {
    const fs::path p = path / name;
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char *>(data.data()), std::streamsize(data.size()));
    return p.string();
  }
  std::string write_text(const std::string &name, const std::string &text) const
  {
    return write(name, ByteSpan(reinterpret_cast<const uint8_t *>(text.data()), text.size()));
  }
};

/** Deterministic pseudo random payload. */
static Bytes make_payload(const size_t size, const uint32_t seed, const bool compressible)
{
  Bytes out(size);
  uint32_t x = seed * 2654435761u + 1;
  for (size_t i = 0; i < size; i++) {
    x = x * 1664525u + 1013904223u;
    out[i] = compressible ? uint8_t('a' + (i / 7 + seed) % 5) : uint8_t(x >> 24);
  }
  return out;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name PCK
 * \{ */

struct PckKeys {
  uint32_t guard0, guard1, mask, check;
};

static PckKeys pck_keys(const uint32_t id)
{
  if (id == 111) {
    return {0xab12908f, 0xb3231902, 0x2a63810e, 0x18734563};
  }
  return {0xfdfdfeeeu + id * 0x72341f2u,
          0xf00dbeefu + id * 0x1237a73u,
          0xa8937462u + id * 0xab2321fu,
          0x59374231u + id * 0x987a223u};
}

struct PckFileSpec {
  std::string name;
  Bytes data;
  bool compress = true;
};

enum class SafeHeader { None, U32, U64 };

struct PckSpec {
  uint32_t version = 0x00020002;
  uint32_t algorithm_id = 0;
  bool compress_records = true;
  bool encrypt = false;
  bool sign_extended_offset = false;
  bool description_over_flags = false;
  SafeHeader safe = SafeHeader::U32;
  std::vector<PckFileSpec> files;
  /** Garbage appended after the package data (only meaningful with a safe header). */
  size_t trailing_garbage = 0;
};

static void encrypt_pck_data(Bytes &data)
{
  const uint32_t mask = uint32_t(data.size()) + 0x739802abu;
  for (size_t i = 0; i + 3 < data.size(); i += 4) {
    uint32_t v = (uint32_t(data[i]) << 24) | (uint32_t(data[i + 1]) << 16) |
                 (uint32_t(data[i + 2]) << 8) | uint32_t(data[i + 3]);
    v ^= mask;
    v = (v << 16) | (v >> 16);
    data[i] = uint8_t(v >> 24);
    data[i + 1] = uint8_t(v >> 16);
    data[i + 2] = uint8_t(v >> 8);
    data[i + 3] = uint8_t(v);
  }
}

static Bytes build_pck(const PckSpec &spec)
{
  const PckKeys keys = pck_keys(spec.algorithm_id);
  const bool v3 = spec.version == 0x00020003;
  Writer w;
  if (spec.safe == SafeHeader::U32) {
    w.u32(0x4DCA23EF).u32(0).u32(0x56A089B7);
  }
  else if (spec.safe == SafeHeader::U64) {
    w.u32(0x4DCA23EF).u64(0);
  }

  struct Placed {
    uint64_t offset;
    uint32_t size, stored;
  };
  std::vector<Placed> placed;
  for (const PckFileSpec &file : spec.files) {
    Bytes stored = file.data;
    if (file.compress) {
      Bytes c = zlib_compress(file.data);
      if (c.size() < file.data.size()) {
        stored = std::move(c);
      }
    }
    if (spec.encrypt) {
      encrypt_pck_data(stored);
    }
    placed.push_back({w.size(), uint32_t(file.data.size()), uint32_t(stored.size())});
    w.bytes(stored);
  }

  const uint64_t table_offset = w.size();
  for (size_t i = 0; i < spec.files.size(); i++) {
    Writer rec;
    rec.fixed_string(spec.files[i].name, 260);
    if (v3) {
      rec.u32(0).u64(placed[i].offset).u32(placed[i].size).u32(placed[i].stored).i32(0).u32(0);
    }
    else {
      rec.u32(uint32_t(placed[i].offset)).u32(placed[i].size).u32(placed[i].stored).i32(0);
    }
    Bytes stored = rec.data;
    if (spec.compress_records) {
      Bytes c = zlib_compress(rec.data);
      if (c.size() < rec.data.size()) {
        stored = std::move(c);
      }
    }
    const uint32_t size = uint32_t(stored.size());
    w.u32(size ^ keys.mask).u32(size ^ keys.mask ^ keys.check);
    w.bytes(stored);
  }

  /* FILEHEADER. */
  const std::string description = "Angelica File Package, GameForge test.";
  w.u32(keys.guard0).u32(spec.version);
  if (v3) {
    const uint32_t high = spec.sign_extended_offset ? 0xFFFFFFFFu : 0u;
    w.u32(uint32_t(table_offset) ^ keys.mask).u32(high);
  }
  else {
    w.u32(uint32_t(table_offset) ^ keys.mask);
  }
  if (spec.description_over_flags) {
    w.fixed_string(description, 256);
  }
  else {
    w.u32(spec.encrypt ? 0x80000000u : 0u).fixed_string(description, 252);
  }
  w.u32(keys.guard1);
  if (v3) {
    w.u32(0);
  }
  w.i32(int32_t(spec.files.size())).u32(spec.version);

  const uint64_t end = w.size();
  if (spec.safe == SafeHeader::U32) {
    w.patch_u32(4, uint32_t(end));
  }
  else if (spec.safe == SafeHeader::U64) {
    w.patch_u32(4, uint32_t(end));
    w.patch_u32(8, uint32_t(end >> 32));
  }
  w.zeros(spec.trailing_garbage);
  return w.data;
}

static std::vector<PckFileSpec> sample_files()
{
  return {
      {"Models\\Npc\\Test.ski", make_payload(5000, 1, true), true},
      {"textures/RAW.dds", make_payload(3000, 2, false), true},
      {"configs\\empty.txt", Bytes(), true},
      {"Gfx\\Stored.gfx", make_payload(800, 3, true), false},
  };
}

static void check_pck_contents(const PckArchive &pck, const std::vector<PckFileSpec> &files)
{
  ASSERT_EQ(pck.entries().size(), files.size());
  for (const PckFileSpec &file : files) {
    const PckEntry *entry = pck.find(file.name);
    ASSERT_NE(entry, nullptr) << file.name;
    std::string expected_display = file.name;
    std::replace(expected_display.begin(), expected_display.end(), '/', '\\');
    EXPECT_EQ(entry->path_display, expected_display);
    EXPECT_EQ(entry->path, normalize_engine_path(file.name, false));
    Bytes data;
    std::string err;
    EXPECT_TRUE(pck.read(*entry, data, &err)) << err;
    EXPECT_EQ(data, file.data) << file.name;
  }
}

TEST(angelica_pck, round_trip_versions)
{
  for (const uint32_t version : {0x00020001u, 0x00020002u, 0x00020003u}) {
    for (const bool compress_records : {false, true}) {
      for (const SafeHeader safe : {SafeHeader::None, SafeHeader::U32, SafeHeader::U64}) {
        TempDir dir;
        PckSpec spec;
        spec.version = version;
        spec.compress_records = compress_records;
        spec.safe = safe;
        spec.files = sample_files();
        const std::string path = dir.write("test.pck", build_pck(spec));

        PckArchive pck;
        std::string err;
        ASSERT_TRUE(pck.open(path, &err)) << err;
        EXPECT_EQ(pck.version(), version);
        EXPECT_EQ(pck.algorithm_id(), 0);
        EXPECT_FALSE(pck.is_encrypted());
        EXPECT_EQ(pck.description(), "Angelica File Package, GameForge test.");
        check_pck_contents(pck, spec.files);
      }
    }
  }
}

TEST(angelica_pck, lookup_is_case_and_separator_insensitive)
{
  TempDir dir;
  PckSpec spec;
  spec.files = sample_files();
  PckArchive pck;
  std::string err;
  ASSERT_TRUE(pck.open(dir.write("a.pck", build_pck(spec)), &err)) << err;
  EXPECT_NE(pck.find("MODELS/NPC/TEST.SKI"), nullptr);
  EXPECT_NE(pck.find("\\models\\npc\\test.ski"), nullptr);
  EXPECT_NE(pck.find("Textures\\raw.DDS"), nullptr);
  EXPECT_EQ(pck.find("models\\npc\\missing.ski"), nullptr);
  Bytes data;
  EXPECT_FALSE(pck.read("models\\npc\\missing.ski", data, &err));
  EXPECT_TRUE(pck.read("gfx/stored.GFX", data, &err)) << err;
  EXPECT_EQ(data, spec.files[3].data);
}

TEST(angelica_pck, algorithm_ids_and_variants)
{
  for (const uint32_t id : {1u, 7u, 111u, 171u}) {
    TempDir dir;
    PckSpec spec;
    spec.algorithm_id = id;
    spec.files = sample_files();
    PckArchive pck;
    std::string err;
    ASSERT_TRUE(pck.open(dir.write("k.pck", build_pck(spec)), &err)) << err;
    EXPECT_EQ(pck.algorithm_id(), int64_t(id));
    check_pck_contents(pck, spec.files);
  }

  /* 64-bit header with a sign-extended offset mask, description over the flags field, garbage
   * after the data end. */
  TempDir dir;
  PckSpec spec;
  spec.version = 0x00020003;
  spec.sign_extended_offset = true;
  spec.description_over_flags = true;
  spec.trailing_garbage = 100;
  spec.files = sample_files();
  PckArchive pck;
  std::string err;
  ASSERT_TRUE(pck.open(dir.write("v3.pck", build_pck(spec)), &err)) << err;
  EXPECT_FALSE(pck.is_encrypted());
  check_pck_contents(pck, spec.files);
}

TEST(angelica_pck, encrypted_package)
{
  TempDir dir;
  PckSpec spec;
  spec.encrypt = true;
  spec.files = sample_files();
  PckArchive pck;
  std::string err;
  ASSERT_TRUE(pck.open(dir.write("enc.pck", build_pck(spec)), &err)) << err;
  EXPECT_TRUE(pck.is_encrypted());
  check_pck_contents(pck, spec.files);
}

TEST(angelica_pck, pkx_continuation)
{
  TempDir dir;
  PckSpec spec;
  spec.version = 0x00020003;
  spec.files = sample_files();
  /* Several files so that data and the file table straddle the part boundaries. */
  for (int i = 0; i < 20; i++) {
    spec.files.push_back({"data\\file" + std::to_string(i) + ".bin",
                          make_payload(size_t(1000 + i * 37), uint32_t(10 + i), i % 2 == 0),
                          true});
  }
  const Bytes whole = build_pck(spec);
  const size_t cut1 = 4001, cut2 = 9003;
  ASSERT_LT(cut2, whole.size());
  const std::string pck_path = dir.write("split.pck", ByteSpan(whole).subspan(0, cut1));
  /* Upper-case continuation names must be found on case-sensitive file systems too. */
  dir.write("SPLIT.PKX", ByteSpan(whole).subspan(cut1, cut2 - cut1));
  dir.write("split.pkx1", ByteSpan(whole).subspan(cut2));
  /* A gap in the numbering ends the sequence. */
  dir.write("split.pkx3", make_payload(10, 1, false));

  PckArchive pck;
  std::string err;
  ASSERT_TRUE(pck.open(pck_path, &err)) << err;
  EXPECT_EQ(pck.part_paths().size(), 3u);
  EXPECT_EQ(pck.stream_size(), whole.size());
  check_pck_contents(pck, spec.files);

  /* Without the continuation files the package cannot be opened. */
  TempDir dir2;
  PckArchive pck2;
  EXPECT_FALSE(pck2.open(dir2.write("split.pck", ByteSpan(whole).subspan(0, cut1)), &err));
  EXPECT_FALSE(err.empty());
}

TEST(angelica_pck, malformed_input)
{
  PckSpec spec;
  spec.files = sample_files();
  const Bytes good = build_pck(spec);
  std::string err;

  auto open_bytes = [&](const Bytes &bytes) {
    TempDir dir;
    PckArchive pck;
    const bool ok = pck.open(dir.write("m.pck", bytes), &err);
    if (ok) {
      /* Reading every entry must never crash, whatever it returns. */
      Bytes data;
      for (const PckEntry &e : pck.entries()) {
        pck.read(e, data, nullptr);
      }
    }
    return ok;
  };

  EXPECT_FALSE(open_bytes(Bytes()));
  EXPECT_FALSE(open_bytes(Bytes(100, 0)));
  {
    PckArchive pck;
    EXPECT_FALSE(pck.open("/nonexistent/dir/x.pck", &err));
  }
  {
    /* Unsupported version. */
    Bytes bad = good;
    const uint32_t v = 0x00010003;
    std::memcpy(&bad[bad.size() - 4], &v, 4);
    EXPECT_FALSE(open_bytes(bad));
    EXPECT_NE(err.find("version"), std::string::npos);
  }
  {
    /* Wrong guard words. */
    Bytes bad = good;
    bad[bad.size() - 8 - 272] ^= 0x5a;
    EXPECT_FALSE(open_bytes(bad));
  }
  {
    /* Absurd file count. */
    Bytes bad = good;
    const int32_t n = 0x7fffffff;
    std::memcpy(&bad[bad.size() - 8], &n, 4);
    EXPECT_FALSE(open_bytes(bad));
  }
  {
    /* Broken table check word. */
    PckSpec s2 = spec;
    s2.safe = SafeHeader::None;
    s2.compress_records = false;
    Bytes bad = build_pck(s2);
    size_t data_size = 0;
    for (const PckFileSpec &f : s2.files) {
      Bytes c = zlib_compress(f.data);
      data_size += (f.compress && c.size() < f.data.size()) ? c.size() : f.data.size();
    }
    bad[data_size + 4] ^= 1;
    EXPECT_FALSE(open_bytes(bad));
    EXPECT_NE(err.find("check"), std::string::npos);
  }
  {
    /* Corrupt compressed file data: opens, but reading that entry fails. */
    Bytes bad = good;
    for (size_t i = 12 + 2; i < 12 + 40; i++) {
      bad[i] ^= 0xff;
    }
    TempDir dir;
    PckArchive pck;
    ASSERT_TRUE(pck.open(dir.write("c.pck", bad), &err)) << err;
    Bytes data;
    EXPECT_FALSE(pck.read(*pck.find("models\\npc\\test.ski"), data, &err));
    EXPECT_TRUE(data.empty());
  }
  /* Every truncation and random byte corruption is handled without crashing. */
  for (size_t len = 0; len < good.size(); len += 97) {
    open_bytes(Bytes(good.begin(), good.begin() + len));
  }
  std::mt19937 rng(1234);
  for (int iter = 0; iter < 300; iter++) {
    Bytes bad = good;
    for (int k = 0; k < 4; k++) {
      bad[rng() % bad.size()] = uint8_t(rng());
    }
    open_bytes(bad);
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Terrain
 * \{ */

/* Small synthetic terrain: blocks of 4x4 grids, 4 x 2 blocks, sub-terrains of 2x2 blocks (two
 * of them side by side), mask areas of one block. */
constexpr int BG = 4;
constexpr int NV = (BG + 1) * (BG + 1);
constexpr int BLOCK_COLS = 4;
constexpr int BLOCK_ROWS = 2;
constexpr int SUB_GRID = 8;
constexpr int MASK_GRID = 4;
constexpr float GRID = 2.0f;

/** Height of global vertex (row, col), continuous across blocks. */
static float test_height(const int row, const int col)
{
  return 100.0f + float(row) * 3.0f + float(col) * 0.25f + float((row * 7 + col * 3) % 5);
}

struct TrnSpec {
  int32_t loader = 1;
  std::vector<std::string> textures = {"Textures\\Maps\\grass.dds", "textures\\maps\\rock.dds"};
  std::vector<std::string> sub_names = {"tst_1", "tst_2"};
  int mask_map_size = 8;
};

static void write_trn2_header(Writer &w,
                              const TrnSpec &spec,
                              const int sub_rows,
                              const int sub_cols)
{
  w.u32(fourcc('T', 'R', 'N', '2')).u32(1);
  w.f32(BLOCK_COLS * BG * GRID).f32(BLOCK_ROWS * BG * GRID).f32(BG * GRID);
  w.i32(BG).i32(BLOCK_COLS * BLOCK_ROWS).i32(BLOCK_ROWS).i32(BLOCK_COLS);
  w.i32(int32_t(spec.textures.size()));
  w.i32(BLOCK_COLS * BLOCK_ROWS * BG * BG / (MASK_GRID * MASK_GRID))
      .i32(MASK_GRID / BG)
      .i32(MASK_GRID);
  w.i32(sub_rows * sub_cols).i32(sub_rows).i32(sub_cols).i32(SUB_GRID).f32(GRID);
  w.u32(0).i32(spec.loader).u32(0); /* Offsets patched later. */
  w.zeros(56);
  for (int i = 0; i < sub_rows * sub_cols; i++) {
    w.f32(0.0f).f32(400.0f + float(i));
  }
}

struct BlockSpec {
  uint32_t version = 4;
  uint8_t comp = 0x15;
  bool normals = true;
};

static std::vector<float> block_heights(const int brow, const int bcol)
{
  std::vector<float> h(NV);
  for (int i = 0; i <= BG; i++) {
    for (int j = 0; j <= BG; j++) {
      h[i * (BG + 1) + j] = test_height(brow * BG + i, bcol * BG + j);
    }
  }
  return h;
}

/** Normal angle bytes used for vertex `v` of a block (for NOR_DEG). */
static void normal_bytes(const int v, uint8_t &b1, uint8_t &b2)
{
  b1 = uint8_t(v * 11);
  b2 = uint8_t(v % 64);
}

static std::vector<float> block_normals(const int brow, const int bcol)
{
  std::vector<float> n(NV * 3);
  for (int v = 0; v < NV; v++) {
    n[v * 3 + 0] = 0.1f * float(brow);
    n[v * 3 + 1] = 1.0f;
    n[v * 3 + 2] = 0.01f * float(v + bcol);
  }
  return n;
}

/** Block record + payload as written by the engine tools. */
static void write_block(
    Writer &w, const BlockSpec &spec, const int brow, const int bcol, const int mask_area)
{
  const std::vector<float> heights = block_heights(brow, bcol);
  Bytes hei;
  if (spec.comp & T2BK_HEI_INC) {
    Writer inc;
    const float mn = *std::min_element(heights.begin(), heights.end());
    const float mx = *std::max_element(heights.begin(), heights.end());
    inc.u8(1).zeros(3).f32(mn).f32(mx).i32(NV);
    int last = 0;
    for (const float h : heights) {
      const int d = int((h - mn) * 32767.0f / (mx - mn) + 0.5f);
      inc.i16(int16_t(d - last));
      last = d;
    }
    hei = inc.data;
  }
  else {
    hei = floats_to_bytes(heights);
  }
  if (spec.comp & T2BK_HEI_ZIP) {
    hei = zlib_compress(hei);
  }

  Bytes nor;
  if (spec.normals) {
    if (spec.comp & T2BK_NOR_DEG) {
      Writer deg;
      deg.u8(1).zeros(3).i32(NV);
      for (int v = 0; v < NV; v++) {
        uint8_t b1, b2;
        normal_bytes(v, b1, b2);
        deg.u8(b1).u8(b2);
      }
      nor = deg.data;
    }
    else {
      nor = floats_to_bytes(block_normals(brow, bcol));
    }
    if (spec.comp & T2BK_NOR_ZIP) {
      nor = zlib_compress(nor);
    }
  }

  Bytes col;
  {
    Writer c;
    for (int v = 0; v < NV; v++) {
      c.u32(0xff000000u | uint32_t(v));
    }
    col = c.data;
    if (spec.comp & T2BK_COL_ZIP) {
      col = zlib_compress(col);
    }
  }

  w.i32(mask_area).u16(uint16_t(brow)).u16(uint16_t(bcol)).f32(1.0f).u32(0x3);
  if (spec.version >= 2) {
    w.u32(uint32_t(hei.size())).u32(uint32_t(nor.size()));
  }
  if (spec.version >= 3) {
    w.u32(uint32_t(col.size()));
  }
  if (spec.version >= 4) {
    w.u32(uint32_t(col.size()));
  }
  if (spec.version >= 6) {
    w.u32(T2BK_FLAG_NOTRACE);
  }
  w.bytes(hei).bytes(nor);
  if (spec.version >= 3) {
    w.bytes(col);
  }
  if (spec.version >= 4) {
    w.bytes(col);
  }
}

/** `.t2bk` of sub-terrain `sub_col` (sub-terrains are 2x2 blocks, side by side). */
static Bytes build_t2bk(const BlockSpec &spec, const int sub_col)
{
  Writer w;
  w.u32(fourcc('T', 'B', 'K', 'B')).u32(spec.version).i32(4);
  if (spec.version >= 2) {
    w.u8(spec.version >= 5 ? spec.comp : (spec.comp ? 1 : 0)).zeros(3);
  }
  const size_t table = w.size();
  w.zeros(4 * 4);
  /* Store the blocks in reverse order: offsets need not be sorted. */
  for (int b = 3; b >= 0; b--) {
    w.patch_u32(table + size_t(b) * 4, uint32_t(w.size()));
    const int brow = b / 2;
    const int bcol = sub_col * 2 + b % 2;
    write_block(w, spec, brow, bcol, brow * BLOCK_COLS + bcol);
  }
  return w.data;
}

static Bytes test_mask(const int size, const int seed)
{
  Bytes m(size_t(size) * size);
  for (size_t i = 0; i < m.size(); i++) {
    m[i] = uint8_t(i * 13 + size_t(seed) * 31);
  }
  return m;
}

/** `.t2mk` (LOADER_B) with 4 mask areas: area `a` has `1 + a % 3` layers. */
static Bytes build_t2mk(const uint32_t version, const bool compressed, const int mask_size)
{
  Writer w;
  const int num_layers = 3;
  w.u32(fourcc('T', 'M', 'K', 'B')).u32(version).i32(4).i32(num_layers);
  if (version >= 2) {
    w.u8(compressed ? 1 : 0).zeros(3);
  }
  const size_t table = w.size();
  w.zeros(16);
  for (int l = 0; l < num_layers; l++) {
    w.i32(l % 2).i32(-1).i32(l == 0 ? 0 : mask_size).u8(uint8_t(l % 3)).zeros(3);
    w.f32(0.2f + float(l)).f32(0.3f + float(l)).u8(uint8_t(l)).zeros(3);
  }
  for (int a = 0; a < 4; a++) {
    w.patch_u32(table + size_t(a) * 4, uint32_t(w.size()));
    const int n = 1 + a % 3;
    w.i32(n);
    std::vector<size_t> patch_pos;
    for (int l = 0; l < n; l++) {
      w.i32(l);
      patch_pos.push_back(w.size());
      w.u32(0);
      if (version >= 2) {
        w.u32(0);
      }
    }
    if (version >= 3) {
      /* Masks as uncompressed 32-bit DDS textures, three layers per texture. */
      for (int t = 0; t < (n + 2) / 3; t++) {
        Writer dds;
        dds.u32(fourcc(' ', 'S', 'D', 'D')).u32(124).u32(0x100f).u32(uint32_t(mask_size));
        dds.u32(uint32_t(mask_size)).u32(uint32_t(mask_size * 4)).u32(0).u32(1).zeros(44);
        dds.u32(32).u32(0x41).u32(0).u32(32);
        dds.u32(0x00ff0000).u32(0x0000ff00).u32(0x000000ff).u32(0xff000000);
        dds.zeros(20);
        const Bytes m1 = test_mask(mask_size, a * 10 + 1);
        const Bytes m2 = test_mask(mask_size, a * 10 + 2);
        for (int p = 0; p < mask_size * mask_size; p++) {
          dds.u8(0xff).u8(n > 1 ? m1[size_t(p)] : 0).u8(n > 2 ? m2[size_t(p)] : 0).u8(0xff);
        }
        w.u32(uint32_t(dds.size())).bytes(dds.data);
      }
      continue;
    }
    for (int l = 1; l < n; l++) {
      Bytes m = test_mask(mask_size, a * 10 + l);
      if (compressed) {
        m = zlib_compress(m);
      }
      w.patch_u32(patch_pos[size_t(l)], uint32_t(w.size()));
      if (version >= 2) {
        w.patch_u32(patch_pos[size_t(l)] + 4, uint32_t(m.size()));
      }
      w.bytes(m);
    }
  }
  return w.data;
}

static Bytes build_t2lm(const uint32_t version, const int num_areas, const int size)
{
  Writer w;
  w.u32(fourcc('T', '2', 'L', 'M')).u32(version).i32(num_areas).i32(size);
  const int data_size = version >= 2 ? 128 + size * size / 2 : size * size * 3;
  if (version >= 2) {
    w.u32(T2lmFile::FLAG_DDS).i32(data_size);
  }
  const size_t table = w.size();
  w.zeros(size_t(num_areas) * 4);
  for (int a = num_areas - 1; a >= 0; a--) {
    w.patch_u32(table + size_t(a) * 4, uint32_t(w.size()));
    Bytes blob(size_t(data_size), uint8_t(a + 1));
    if (version >= 2) {
      std::memcpy(blob.data(), "DDS ", 4);
    }
    w.bytes(blob);
  }
  return w.data;
}

static Bytes build_trn2_loader_b(const TrnSpec &spec)
{
  Writer w;
  write_trn2_header(w, spec, 1, 2);
  w.patch_u32(0x48, uint32_t(w.size()));
  for (const std::string &t : spec.textures) {
    w.string_i32(t);
  }
  w.patch_u32(0x50, uint32_t(w.size()));
  w.i32(spec.mask_map_size);
  for (const std::string &n : spec.sub_names) {
    w.string_i32(n);
  }
  return w.data;
}

TEST(angelica_terrain, trn2_descriptor)
{
  TrnSpec spec;
  spec.textures.push_back("Textures\\\xb5\xd8\xc3\xe6.dds"); /* GBK "地面". */
  const Bytes data = build_trn2_loader_b(spec);
  Trn2Info info;
  std::string err;
  ASSERT_TRUE(parse_trn2(data, info, &err)) << err;
  EXPECT_EQ(info.loader_id, 1);
  EXPECT_FLOAT_EQ(info.terrain_width, 32.0f);
  EXPECT_FLOAT_EQ(info.terrain_height, 16.0f);
  EXPECT_EQ(info.block_grid, BG);
  EXPECT_EQ(info.num_blocks, 8);
  EXPECT_EQ(info.sub_terrain_grid, SUB_GRID);
  EXPECT_EQ(info.blocks_per_sub_terrain_side(), 2);
  EXPECT_EQ(info.mask_areas_per_row(), 4);
  ASSERT_EQ(info.sub_terrains.size(), 2u);
  EXPECT_FLOAT_EQ(info.sub_terrains[1].max_height, 401.0f);
  ASSERT_EQ(info.texture_paths.size(), 3u);
  EXPECT_EQ(info.texture_paths[0], "Textures\\Maps\\grass.dds");
  EXPECT_EQ(info.texture_paths[2], "Textures\\\xe5\x9c\xb0\xe9\x9d\xa2.dds");
  EXPECT_EQ(info.mask_map_size, 8);
  EXPECT_EQ(info.sub_terrain_names, spec.sub_names);

  /* A zero mask map size means 32. */
  spec.mask_map_size = 0;
  ASSERT_TRUE(parse_trn2(build_trn2_loader_b(spec), info, &err)) << err;
  EXPECT_EQ(info.mask_map_size, 32);
}

TEST(angelica_terrain, t2bk_all_versions)
{
  const BlockSpec specs[] = {
      {1, 0, true},
      {2, 0x15, true},
      {2, 0, true},
      {3, 0, true},
      {3, 0x15, false},
      {4, 0x15, true},
      {5, T2BK_HEI_INC | T2BK_NOR_DEG, true},
      {5, T2BK_HEI_ZIP | T2BK_HEI_INC | T2BK_NOR_ZIP | T2BK_NOR_DEG | T2BK_COL_ZIP, true},
      {6, 0x15, true},
      {6, 0, true},
  };
  for (const BlockSpec &spec : specs) {
    SCOPED_TRACE("version " + std::to_string(spec.version) + " comp " + std::to_string(spec.comp));
    T2bkFile file;
    std::string err;
    ASSERT_TRUE(file.parse(build_t2bk(spec, 1), BG, &err)) << err;
    EXPECT_EQ(file.version(), spec.version);
    ASSERT_EQ(file.num_blocks(), 4);
    for (int b = 0; b < 4; b++) {
      TerrainBlock block;
      ASSERT_TRUE(file.read_block(b, block, &err)) << err;
      const int brow = b / 2, bcol = 2 + b % 2;
      EXPECT_EQ(block.row_in_terrain, brow);
      EXPECT_EQ(block.col_in_terrain, bcol);
      EXPECT_EQ(block.mask_area, brow * BLOCK_COLS + bcol);
      EXPECT_EQ(block.layer_flags, 3u);
      EXPECT_EQ(block.block_flags, spec.version >= 6 ? T2BK_FLAG_NOTRACE : 0u);
      const std::vector<float> expected = block_heights(brow, bcol);
      ASSERT_EQ(block.heights.size(), size_t(NV));
      const float tol = (spec.comp & T2BK_HEI_INC) ? 0.01f : 0.0f;
      for (int v = 0; v < NV; v++) {
        EXPECT_NEAR(block.heights[v], expected[v], tol);
      }
      if (!spec.normals && spec.version >= 2) {
        EXPECT_TRUE(block.normals.empty());
      }
      else if (spec.comp & T2BK_NOR_DEG) {
        ASSERT_EQ(block.normals.size(), size_t(NV * 3));
        for (int v = 0; v < NV; v++) {
          uint8_t b1, b2;
          normal_bytes(v, b1, b2);
          const double a1 = b1 * 2.0 * M_PI / 256.0, a2 = b2 * 2.0 * M_PI / 256.0;
          EXPECT_NEAR(block.normals[v * 3 + 0], std::sin(a2) * std::cos(a1), 1e-5);
          EXPECT_NEAR(block.normals[v * 3 + 1], std::cos(a2), 1e-5);
          EXPECT_NEAR(block.normals[v * 3 + 2], std::sin(a2) * std::sin(a1), 1e-5);
        }
      }
      else {
        EXPECT_EQ(block.normals, block_normals(brow, bcol));
      }
      if (spec.version >= 3) {
        ASSERT_EQ(block.diffuse.size(), size_t(NV));
        EXPECT_EQ(block.diffuse[5], 0xff000005u);
      }
      else {
        EXPECT_TRUE(block.diffuse.empty());
      }
      EXPECT_EQ(block.diffuse_night.size(), spec.version >= 4 ? size_t(NV) : 0u);
    }
    TerrainBlock block;
    EXPECT_FALSE(file.read_block(4, block, &err));
    EXPECT_FALSE(file.read_block(-1, block, &err));
  }
}

TEST(angelica_terrain, t2mk_versions)
{
  struct Case {
    uint32_t version;
    bool compressed;
  };
  for (const Case c : {Case{1, false}, Case{2, false}, Case{2, true}, Case{3, false}}) {
    SCOPED_TRACE("version " + std::to_string(c.version));
    T2mkFile file;
    std::string err;
    ASSERT_TRUE(file.parse(build_t2mk(c.version, c.compressed, 8), &err)) << err;
    EXPECT_EQ(file.version, c.version);
    EXPECT_EQ(file.compressed, c.compressed);
    ASSERT_EQ(file.layers.size(), 3u);
    EXPECT_EQ(file.layers[1].texture_index, 1);
    EXPECT_EQ(file.layers[2].proj_axis, 2);
    EXPECT_FLOAT_EQ(file.layers[1].u_scale, 1.2f);
    EXPECT_FLOAT_EQ(file.layers[1].v_scale, 1.3f);
    EXPECT_EQ(file.layers[2].weight, 2);
    ASSERT_EQ(file.mask_areas.size(), 4u);
    for (int a = 0; a < 4; a++) {
      const T2mkMaskArea &area = file.mask_areas[size_t(a)];
      ASSERT_EQ(area.layers.size(), size_t(1 + a % 3));
      EXPECT_TRUE(area.layers[0].mask.empty());
      for (size_t l = 1; l < area.layers.size(); l++) {
        EXPECT_EQ(area.layers[l].layer_index, int(l));
        EXPECT_EQ(area.layers[l].mask_size, 8);
        EXPECT_EQ(area.layers[l].mask, test_mask(8, a * 10 + int(l)));
      }
      EXPECT_EQ(area.mask_textures.size(), c.version >= 3 ? size_t(1) : 0u);
    }
  }
}

TEST(angelica_terrain, t2lm_versions)
{
  for (const uint32_t version : {1u, 2u}) {
    T2lmFile file;
    std::string err;
    ASSERT_TRUE(file.parse(build_t2lm(version, 3, 8), &err)) << err;
    EXPECT_EQ(file.num_mask_areas(), 3);
    EXPECT_EQ(file.lightmap_size(), 8);
    EXPECT_EQ(file.is_dds(), version >= 2);
    for (int a = 0; a < 3; a++) {
      const ByteSpan blob = file.area_data(a);
      ASSERT_EQ(blob.size(), size_t(version >= 2 ? 128 + 32 : 8 * 8 * 3));
      EXPECT_EQ(blob.back(), uint8_t(a + 1));
    }
    EXPECT_TRUE(file.area_data(3).empty());
  }
}

static void check_field_heights(const TerrainHeightfield &field, const float tol)
{
  ASSERT_EQ(field.verts_x, BLOCK_COLS * BG + 1);
  ASSERT_EQ(field.verts_z, BLOCK_ROWS * BG + 1);
  EXPECT_FLOAT_EQ(field.origin_x, -16.0f);
  EXPECT_FLOAT_EQ(field.origin_z, 8.0f);
  for (int r = 0; r < field.verts_z; r++) {
    for (int c = 0; c < field.verts_x; c++) {
      ASSERT_NEAR(field.height_at(r, c), test_height(r, c), tol) << r << ", " << c;
    }
  }
}

TEST(angelica_terrain, heightfield_loader_b)
{
  TempDir dir;
  TrnSpec spec;
  dir.write("tst.trn2", build_trn2_loader_b(spec));
  /* Mixed case file names, and one sub-terrain without lightmaps. */
  dir.write("TST_1.T2BK", build_t2bk({4, 0x15, true}, 0));
  dir.write("tst_2.t2bk", build_t2bk({5, T2BK_HEI_ZIP | T2BK_HEI_INC | T2BK_NOR_ZIP, true}, 1));
  dir.write("Tst_1.t2mk", build_t2mk(2, true, 8));
  dir.write("tst_2.t2mk", build_t2mk(1, false, 8));
  dir.write("tst_1.T2LM", build_t2lm(2, 4, 8));

  TerrainHeightfield field;
  std::string err;
  ASSERT_TRUE(load_terrain_heightfield(dir.str(), "tst", field, &err)) << err;
  EXPECT_FLOAT_EQ(field.grid_size, GRID);
  check_field_heights(field, 0.01f);
  ASSERT_EQ(field.normals.size(), size_t(field.verts_x * field.verts_z * 3));
  /* Vertex (0, 0) comes from block (0, 0). */
  EXPECT_FLOAT_EQ(field.normals[2], 0.0f);
  EXPECT_EQ(field.texture_paths.size(), 2u);
  EXPECT_EQ(field.sub_terrain_names, spec.sub_names);

  ASSERT_EQ(field.mask_areas_x, 4);
  ASSERT_EQ(field.mask_areas_z, 2);
  ASSERT_EQ(field.mask_areas.size(), 8u);
  /* Sub-terrain 1 local area 3 is global area (row 1, col 3). */
  const TerrainMaskArea &area = field.mask_areas[1 * 4 + 3];
  EXPECT_EQ(area.row, 1);
  EXPECT_EQ(area.col, 3);
  EXPECT_EQ(area.sub_terrain, 1);
  ASSERT_EQ(area.layers.size(), 1u + 3 % 3);
  const TerrainMaskArea &area2 = field.mask_areas[0 * 4 + 1];
  ASSERT_EQ(area2.layers.size(), 2u);
  EXPECT_EQ(area2.layers[1].texture_index, 1);
  EXPECT_EQ(area2.layers[1].mask, test_mask(8, 11));
  EXPECT_TRUE(area2.lightmap_is_dds);
  EXPECT_EQ(area2.lightmap.back(), 2);
  EXPECT_TRUE(field.mask_areas[2].lightmap.empty());
  EXPECT_TRUE(field.warnings.empty());

  /* Missing block file is an error, missing mask files only warn. */
  fs::remove(dir.path / "tst_2.t2mk");
  ASSERT_TRUE(load_terrain_heightfield(dir.str(), "TST", field, &err)) << err;
  EXPECT_FALSE(field.warnings.empty());
  fs::remove(dir.path / "tst_2.t2bk");
  EXPECT_FALSE(load_terrain_heightfield(dir.str(), "tst", field, &err));
  EXPECT_NE(err.find("tst_2.t2bk"), std::string::npos);
  EXPECT_FALSE(load_terrain_heightfield(dir.str(), "nope", field, &err));
}

TEST(angelica_terrain, heightfield_loader_a)
{
  TrnSpec spec;
  spec.loader = 0;
  Writer w;
  write_trn2_header(w, spec, 1, 2);
  const size_t offsets = w.size();
  w.zeros(8 * 4);
  for (int b = 0; b < 8; b++) {
    w.patch_u32(offsets + size_t(b) * 4, uint32_t(w.size()));
    write_block(w, {1, 0, true}, b / BLOCK_COLS, b % BLOCK_COLS, b);
  }
  w.patch_u32(0x48, uint32_t(w.size()));
  for (const std::string &t : spec.textures) {
    w.string_i32(t);
  }

  /* LOADER_A mask file with lumps: 8 mask areas, area `a` uses layers [a % 2, a % 2 + 1]. */
  Writer mk;
  const int num_items = 3;
  mk.u32(fourcc('T', '2', 'M', 'K')).u32(1).i32(8);
  const size_t lump_table = mk.size();
  mk.zeros(7 * 8);
  const size_t area_table = mk.size();
  mk.zeros(8 * 4);
  std::vector<uint32_t> mask_offsets;
  for (int i = 0; i < num_items; i++) {
    mask_offsets.push_back(uint32_t(mk.size()));
    mk.bytes(test_mask(8, 100 + i));
  }
  auto lump = [&](const int index, const Bytes &bytes) {
    mk.patch_u32(lump_table + size_t(index) * 8, uint32_t(mk.size()));
    mk.patch_u32(lump_table + size_t(index) * 8 + 4, uint32_t(bytes.size()));
    mk.bytes(bytes);
  };
  {
    Writer tex, spec_map, size, off, axis, uv, weight;
    for (int i = 0; i < num_items; i++) {
      tex.i32(i % 2);
      spec_map.i32(-1);
      size.i32(8);
      off.u32(mask_offsets[size_t(i)]);
      axis.u8(1);
      uv.f32(0.5f).f32(0.25f);
      weight.u8(uint8_t(i));
    }
    lump(0, tex.data);
    lump(1, spec_map.data);
    lump(2, size.data);
    lump(3, off.data);
    lump(4, axis.data);
    lump(5, uv.data);
    lump(6, weight.data);
  }
  for (int a = 0; a < 8; a++) {
    mk.patch_u32(area_table + size_t(a) * 4, uint32_t(mk.size()));
    mk.i32(2).i32(a % 2);
  }

  TempDir dir;
  dir.write("mapa.trn2", w.data);
  dir.write("mapa.t2mk", mk.data);
  dir.write("mapa.t2lm", build_t2lm(1, 8, 4));

  Trn2Info info;
  std::string err;
  ASSERT_TRUE(parse_trn2(w.data, info, &err)) << err;
  EXPECT_EQ(info.block_offsets.size(), 8u);

  TerrainHeightfield field;
  ASSERT_TRUE(load_terrain_heightfield(dir.str(), "mapa", field, &err)) << err;
  check_field_heights(field, 0.0f);
  ASSERT_EQ(field.mask_areas.size(), 8u);
  const TerrainMaskArea &area = field.mask_areas[5];
  ASSERT_EQ(area.layers.size(), 2u);
  EXPECT_EQ(area.layers[1].texture_index, 0);
  EXPECT_FLOAT_EQ(area.layers[1].u_scale, 0.5f);
  EXPECT_FLOAT_EQ(area.layers[1].v_scale, 0.25f);
  EXPECT_EQ(area.layers[1].mask, test_mask(8, 102));
  EXPECT_FALSE(area.lightmap_is_dds);
  EXPECT_EQ(area.lightmap.size(), 4u * 4 * 3);
  EXPECT_TRUE(field.warnings.empty()) << field.warnings.front();
}

TEST(angelica_terrain, malformed_input)
{
  std::string err;
  const Bytes trn2 = build_trn2_loader_b(TrnSpec());
  const Bytes t2bk = build_t2bk({5, 0x3d, true}, 0);
  const Bytes t2mk = build_t2mk(2, true, 8);
  const Bytes t2mk3 = build_t2mk(3, false, 8);
  const Bytes t2lm = build_t2lm(2, 4, 8);

  Trn2Info info;
  EXPECT_FALSE(parse_trn2(Bytes(), info, &err));
  {
    Bytes bad = trn2;
    bad[0] = 'X';
    EXPECT_FALSE(parse_trn2(bad, info, &err));
  }
  {
    Bytes bad = trn2;
    bad[4] = 2; /* Version. */
    EXPECT_FALSE(parse_trn2(bad, info, &err));
  }
  {
    Bytes bad = trn2;
    const int32_t grid = 0;
    std::memcpy(&bad[8 + 12], &grid, 4);
    EXPECT_FALSE(parse_trn2(bad, info, &err));
  }
  {
    Bytes bad = trn2;
    const int32_t huge = 1 << 20;
    std::memcpy(&bad[8 + 24], &huge, 4); /* Block rows. */
    EXPECT_FALSE(parse_trn2(bad, info, &err));
  }
  {
    T2bkFile f;
    Bytes bad = t2bk;
    bad[4] = 7; /* Version. */
    EXPECT_FALSE(f.parse(bad, BG, &err));
    EXPECT_FALSE(f.parse(t2bk, 0, &err));
  }
  {
    /* Corrupt compressed heights: header parses, block decoding fails. */
    T2bkFile f;
    Bytes bad = build_t2bk({4, 0x15, true}, 0);
    ASSERT_TRUE(f.parse(bad, BG, &err));
    const uint32_t off = f.block_offsets()[0];
    for (size_t i = off + 32 + 2; i < off + 32 + 12; i++) {
      bad[i] ^= 0xff;
    }
    ASSERT_TRUE(f.parse(bad, BG, &err));
    TerrainBlock block;
    EXPECT_FALSE(f.read_block(0, block, &err));
    /* Wrong block grid: heights have the wrong size. */
    ASSERT_TRUE(f.parse(build_t2bk({4, 0x15, true}, 0), BG + 1, &err));
    EXPECT_FALSE(f.read_block(0, block, &err));
  }

  /* No truncation or random corruption may crash. */
  auto exercise = [&](const Bytes &data) {
    Trn2Info i;
    parse_trn2(data, i, nullptr);
    T2bkFile bk;
    if (bk.parse(data, BG, nullptr)) {
      TerrainBlock block;
      for (int b = 0; b < bk.num_blocks(); b++) {
        bk.read_block(b, block, nullptr);
      }
    }
    T2mkFile mk;
    mk.parse(data, nullptr);
    T2lmFile lm;
    if (lm.parse(data, nullptr)) {
      for (int a = 0; a < lm.num_mask_areas(); a++) {
        lm.area_data(a);
      }
    }
  };
  std::mt19937 rng(99);
  for (const Bytes *src : {&trn2, &t2bk, &t2mk, &t2mk3, &t2lm}) {
    for (size_t len = 0; len < src->size(); len += 7) {
      exercise(Bytes(src->begin(), src->begin() + len));
    }
    for (int iter = 0; iter < 400; iter++) {
      Bytes bad = *src;
      for (int k = 0; k < 3; k++) {
        bad[rng() % bad.size()] = uint8_t(rng());
      }
      exercise(bad);
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name HMAP
 * \{ */

static float hmap_value(const int global_row, const int global_col)
{
  return float((global_row * 31 + global_col * 7) % 100) / 100.0f;
}

/** Write `cols * rows` areas of `n * n` values into `dir`. */
static void write_hmaps(
    const TempDir &dir, const std::string &sub, const int cols, const int rows, const int n)
{
  for (int r = 0; r < rows; r++) {
    for (int c = 0; c < cols; c++) {
      std::vector<float> v(size_t(n) * n);
      for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
          v[size_t(i) * n + j] = hmap_value(r * (n - 1) + i, c * (n - 1) + j);
        }
      }
      dir.write(sub + std::to_string(r * cols + c + 1) + ".hmap", floats_to_bytes(v));
    }
  }
}

TEST(angelica_hmap, parse)
{
  std::vector<float> v(65 * 65, 0.5f);
  v[3] = NAN;
  HeightMap map;
  std::string err;
  ASSERT_TRUE(parse_hmap(floats_to_bytes(v), map, &err)) << err;
  EXPECT_EQ(map.size, 65);
  EXPECT_FLOAT_EQ(map.values[0], 0.5f);
  EXPECT_FLOAT_EQ(map.values[3], 0.0f);

  EXPECT_FALSE(parse_hmap(Bytes(), map, &err));
  EXPECT_FALSE(parse_hmap(Bytes(7), map, &err));
  EXPECT_FALSE(parse_hmap(Bytes(4 * 12), map, &err)); /* 12 floats: not square. */
  EXPECT_FALSE(parse_hmap(Bytes(4), map, &err));      /* 1x1 is not a grid. */
}

TEST(angelica_hmap, folder_square_layout)
{
  TempDir dir;
  write_hmaps(dir, "map/", 2, 2, 5);
  HmapMap map;
  std::string err;
  ASSERT_TRUE(load_hmap_folder((dir.path / "map").string(), map, &err)) << err;
  EXPECT_EQ(map.cols, 2);
  EXPECT_EQ(map.rows, 2);
  EXPECT_EQ(map.area_size, 4);
  ASSERT_EQ(map.verts_x, 9);
  ASSERT_EQ(map.verts_z, 9);
  EXPECT_TRUE(map.layout_source.empty());
  for (int r = 0; r < 9; r++) {
    for (int c = 0; c < 9; c++) {
      EXPECT_FLOAT_EQ(map.values[size_t(r) * 9 + c], hmap_value(r, c));
    }
  }
  EXPECT_FLOAT_EQ(map.height(1, 2), hmap_value(1, 2) * 800.0f);
  EXPECT_FLOAT_EQ(map.origin_x(), -8.0f);
  EXPECT_FLOAT_EQ(map.origin_z(), 8.0f);
}

TEST(angelica_hmap, folder_with_configs)
{
  TempDir dir;
  /* Server layout: `<map>/map/<n>.hmap` and `<map>/movemap/movemap.conf`. */
  write_hmaps(dir, "a01/map/", 3, 2, 5);
  dir.write_text("a01/movemap/movemap.conf",
                 "Map Width =3\r\nMap Length =2\r\nSubmap Width =16.000000\r\n"
                 "Submap Length =16.000000\r\n");
  dir.write_text("a01/gs.conf",
                 "[Terrain_gs01]\nszMapPath = map\nnAreaWidth = 4\nnNumCols = 3\n"
                 "nNumRows = 2\nvGridSize = 4.0f\nvHeightMin = -10.0f\nvHeightMax = 90.0f\n"
                 "[Other]\nfoo = bar\n");
  HmapMap map;
  std::string err;
  ASSERT_TRUE(load_hmap_folder((dir.path / "a01" / "map").string(), map, &err)) << err;
  EXPECT_EQ(map.cols, 3);
  EXPECT_EQ(map.rows, 2);
  EXPECT_NE(map.layout_source.find("movemap.conf"), std::string::npos);
  EXPECT_FLOAT_EQ(map.grid_size, 4.0f);
  EXPECT_FLOAT_EQ(map.height_min, -10.0f);
  EXPECT_FLOAT_EQ(map.height_max, 90.0f);
  ASSERT_EQ(map.verts_x, 13);
  ASSERT_EQ(map.verts_z, 9);
  for (int r = 0; r < 9; r++) {
    for (int c = 0; c < 13; c++) {
      EXPECT_FLOAT_EQ(map.values[size_t(r) * 13 + c], hmap_value(r, c));
    }
  }
  EXPECT_FLOAT_EQ(map.height(8, 12), hmap_value(8, 12) * 100.0f - 10.0f);

  /* Grid size from Submap Width when no terrain config exists. */
  fs::remove(dir.path / "a01" / "gs.conf");
  ASSERT_TRUE(load_hmap_folder((dir.path / "a01" / "map").string(), map, &err)) << err;
  EXPECT_FLOAT_EQ(map.grid_size, 4.0f);
  EXPECT_FLOAT_EQ(map.height_max, 800.0f);

  /* Explicit options win. */
  HmapLoadOptions options;
  options.cols = 2;
  options.rows = 3;
  options.has_height_range = true;
  options.height_min = 1.0f;
  options.height_max = 2.0f;
  ASSERT_TRUE(load_hmap_folder((dir.path / "a01" / "map").string(), map, &err, options)) << err;
  EXPECT_EQ(map.cols, 2);
  EXPECT_EQ(map.rows, 3);
  EXPECT_FLOAT_EQ(map.height_max, 2.0f);
}

TEST(angelica_hmap, folder_errors)
{
  std::string err;
  HmapMap map;
  {
    TempDir dir;
    EXPECT_FALSE(load_hmap_folder(dir.str(), map, &err));
  }
  {
    /* 3 files without config: not square. */
    TempDir dir;
    write_hmaps(dir, "", 3, 1, 5);
    EXPECT_FALSE(load_hmap_folder(dir.str(), map, &err));
  }
  {
    /* Config needs more files than exist. */
    TempDir dir;
    write_hmaps(dir, "map/", 2, 1, 5);
    dir.write_text("movemap.conf", "Map Width = 2\nMap Length = 2\n");
    EXPECT_FALSE(load_hmap_folder((dir.path / "map").string(), map, &err));
  }
  {
    /* Mixed area sizes. */
    TempDir dir;
    write_hmaps(dir, "", 2, 2, 5);
    dir.write("4.hmap", floats_to_bytes(std::vector<float>(9 * 9, 0.0f)));
    EXPECT_FALSE(load_hmap_folder(dir.str(), map, &err));
  }
  {
    /* Corrupt file size. */
    TempDir dir;
    write_hmaps(dir, "", 1, 1, 5);
    dir.write("1.hmap", Bytes(10));
    EXPECT_FALSE(load_hmap_folder(dir.str(), map, &err));
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Real Client Data (optional)
 * \{ */

static std::string test_data_dir()
{
  const char *env = std::getenv("ANGELICA_TEST_DATA");
  return env ? std::string(env) : std::string();
}

TEST(angelica_pck, real_packages)
{
  const std::string root = test_data_dir();
  if (root.empty()) {
    GTEST_SKIP() << "ANGELICA_TEST_DATA not set";
  }
  int opened = 0;
  for (const char *name : {"interfaces.pck", "fonts.pck", "pwops.pck"}) {
    const std::string path = find_file_case_insensitive(root, name);
    if (path.empty()) {
      continue;
    }
    SCOPED_TRACE(path);
    PckArchive pck;
    std::string err;
    ASSERT_TRUE(pck.open(path, &err)) << err;
    EXPECT_GT(pck.entries().size(), 0u);
    Bytes data;
    for (const PckEntry &entry : pck.entries()) {
      ASSERT_TRUE(pck.read(entry, data, &err)) << err;
      EXPECT_EQ(data.size(), entry.size);
      EXPECT_EQ(pck.find(entry.path_display), &entry);
    }
    opened++;
  }
  if (opened == 0) {
    GTEST_SKIP() << "No packages found in " << root;
  }
}

TEST(angelica_terrain, real_a19)
{
  const std::string root = test_data_dir();
  if (root.empty()) {
    GTEST_SKIP() << "ANGELICA_TEST_DATA not set";
  }
  const std::string dir = (fs::path(root) / "maps" / "a19").string();
  const std::string trn2_path = find_file_case_insensitive(dir, "a19.trn2");
  if (trn2_path.empty()) {
    GTEST_SKIP() << "maps/a19 not found in " << root;
  }
  std::string err;
  Bytes data;
  ASSERT_TRUE(read_file(trn2_path, data));
  Trn2Info info;
  ASSERT_TRUE(parse_trn2(data, info, &err)) << err;
  EXPECT_FLOAT_EQ(info.terrain_width, 3072.0f);
  EXPECT_FLOAT_EQ(info.terrain_height, 3072.0f);
  EXPECT_FLOAT_EQ(info.block_size, 64.0f);
  EXPECT_EQ(info.block_grid, 32);
  EXPECT_EQ(info.block_rows, 48);
  EXPECT_EQ(info.block_cols, 48);
  EXPECT_EQ(info.num_textures, 7);
  EXPECT_EQ(info.num_mask_areas, 144);
  EXPECT_EQ(info.mask_grid, 128);
  EXPECT_EQ(info.num_sub_terrains, 9);
  EXPECT_EQ(info.sub_terrain_grid, 512);
  EXPECT_FLOAT_EQ(info.grid_size, 2.0f);
  EXPECT_EQ(info.texture_offset, 0xD4u);
  EXPECT_EQ(info.loader_id, 1);
  EXPECT_EQ(info.loader_offset, 0x231u);
  EXPECT_EQ(info.mask_map_size, 256);
  ASSERT_EQ(info.sub_terrain_names.size(), 9u);
  EXPECT_EQ(info.sub_terrain_names[0], "a19_1");
  EXPECT_FLOAT_EQ(info.sub_terrains[0].max_height, 800.0f);

  T2bkFile bk;
  ASSERT_TRUE(bk.load(find_file_case_insensitive(dir, "a19_1.t2bk"), 32, &err)) << err;
  EXPECT_EQ(bk.version(), 4u);
  EXPECT_EQ(bk.num_blocks(), 256);
  EXPECT_EQ(bk.block_offsets()[0], 0x410u);
  TerrainBlock block;
  ASSERT_TRUE(bk.read_block(0, block, &err)) << err;
  EXPECT_EQ(block.heights.size(), 33u * 33u);

  T2mkFile mk;
  ASSERT_TRUE(mk.load(find_file_case_insensitive(dir, "a19_1.t2mk"), &err)) << err;
  EXPECT_EQ(mk.version, 2u);
  EXPECT_EQ(mk.mask_areas.size(), 16u);
  ASSERT_EQ(mk.layers.size(), 1u);
  EXPECT_EQ(mk.layers[0].proj_axis, 1);
  EXPECT_FLOAT_EQ(mk.layers[0].u_scale, 0.2f);

  T2lmFile lm;
  ASSERT_TRUE(lm.load(find_file_case_insensitive(dir, "a19_1.t2lm"), &err)) << err;
  EXPECT_TRUE(lm.is_dds());
  EXPECT_EQ(lm.lightmap_size(), 128);
  EXPECT_EQ(lm.data_size(), 8320);
  EXPECT_EQ(std::memcmp(lm.area_data(0).data(), "DDS ", 4), 0);

  TerrainHeightfield field;
  ASSERT_TRUE(load_terrain_heightfield(dir, "a19", field, &err)) << err;
  EXPECT_EQ(field.verts_x, 1537);
  EXPECT_EQ(field.verts_z, 1537);
  EXPECT_EQ(field.mask_areas.size(), 144u);
  EXPECT_EQ(field.normals.size(), field.heights.size() * 3);
  for (const float h : field.heights) {
    ASSERT_TRUE(std::isfinite(h));
    ASSERT_GE(h, -1.0f);
    ASSERT_LE(h, 801.0f);
  }
  for (const TerrainMaskArea &area : field.mask_areas) {
    ASSERT_FALSE(area.layers.empty());
    EXPECT_FALSE(area.lightmap.empty());
    for (size_t l = 1; l < area.layers.size(); l++) {
      EXPECT_EQ(area.layers[l].mask.size(), 256u * 256u);
    }
  }
  EXPECT_TRUE(field.warnings.empty());

  /* Edges shared by neighboring blocks agree, so the stitched grid has no seams. */
  T2bkFile bk5;
  ASSERT_TRUE(bk5.load(find_file_case_insensitive(dir, "a19_5.t2bk"), 32, &err)) << err;
  TerrainBlock a, b;
  ASSERT_TRUE(bk5.read_block(0, a, &err));
  ASSERT_TRUE(bk5.read_block(1, b, &err));
  for (int i = 0; i <= 32; i++) {
    EXPECT_EQ(a.heights[size_t(i) * 33 + 32], b.heights[size_t(i) * 33]);
  }
  EXPECT_EQ(a.row_in_terrain, 16);
  EXPECT_EQ(a.col_in_terrain, 16);
}

/** \} */

}  // namespace blender::io::angelica::tests
