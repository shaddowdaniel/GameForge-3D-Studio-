/* SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup angelica
 *
 * A3DTerrain2 file layouts. All little-endian, structures use MSVC default packing (bool / BYTE
 * members are padded to 4 bytes when followed by larger members or at the end of a struct).
 * Strings are `AFile::WriteString` (i32 length + GBK bytes).
 *
 * `.trn2` (magic 'TRN2' = 0x54524E32, version <= 1)
 *   0x00 u32 magic, u32 version
 *   0x08 TRN2FILEHEADER (132 bytes):
 *        f32 terrain_width, f32 terrain_height, f32 block_size, i32 block_grid, i32 num_blocks,
 *        i32 block_rows, i32 block_cols, i32 num_textures, i32 num_mask_areas, i32 mask_blocks,
 *        i32 mask_grid, i32 num_sub_terrains, i32 sub_terrain_rows, i32 sub_terrain_cols,
 *        i32 sub_terrain_grid, f32 grid_size, u32 texture_offset, i32 loader_id,
 *        u32 loader_offset, u8 reserved[56]
 *   0x8C sub_terrain_rows * sub_terrain_cols * {f32 min_height, f32 max_height}
 *   LOADER_A only, directly after: num_blocks * u32 block offset, then the block records
 *        (same layout as `.t2bk` version 1).
 *   @texture_offset: num_textures strings (texture paths)
 *   LOADER_B only, @loader_offset: i32 mask_map_size (0 means 32), num_sub_terrains strings
 *        (sub-terrain base names; files are `<trn2 dir>/<name>.t2bk` etc).
 *
 * `.t2bk` (magic 'TBKB' = 0x54424B42, version 1..6)
 *   u32 magic, u32 version
 *   v1:    i32 num_blocks                              -> offsets at 12, no compression
 *   v2-4:  i32 num_blocks, bool compressed (+3 pad)    -> offsets at 16,
 *          compressed means COL_ZIP | HEI_ZIP | NOR_ZIP
 *   v5-6:  i32 num_blocks, u8 compression (+3 pad)     -> offsets at 16
 *   num_blocks * u32 absolute block offset (not necessarily sorted)
 *   Block record:
 *     i32 mask_area, u16 row_in_terrain, u16 col_in_terrain, f32 lod_scale, u32 layer_flags
 *     v2+: u32 height_size, u32 normal_size
 *     v3+: u32 diffuse_size
 *     v4+: u32 diffuse_night_size
 *     v6:  u32 block_flags
 *   followed by height data, normal data, day colors (v3+), night colors (v4+).
 *   v1 has no sizes: heights are N floats and normals 3N floats, N = (block_grid + 1)^2.
 *   Heights: N floats; HEI_ZIP = zlib; HEI_INC = incremental, see #decode_incremental_heights
 *   (when both are set the incremental data is zlib compressed). Normals: 3N floats; NOR_ZIP =
 *   zlib; NOR_DEG = two angle bytes per vertex, see #decode_degree_normals. Colors: N u32 ARGB,
 *   COL_ZIP = zlib.
 *
 * `.t2mk` LOADER_B (magic 'TMKB' = 0x544D4B42, version 1..3)
 *   u32 magic, u32 version
 *   v1: i32 num_mask_areas, i32 num_layers                       -> offsets at 16
 *   v2+: i32 num_mask_areas, i32 num_layers, bool compressed     -> offsets at 20
 *   num_mask_areas * u32 absolute mask area offset
 *   num_layers * T2MKFILELAYER (28 bytes): i32 texture_index, i32 specular_index,
 *     i32 mask_map_size, u8 proj_axis (+3), f32 u_scale, f32 v_scale, u8 weight (+3)
 *   Mask area: i32 num_layers, then per layer
 *     v1: i32 layer, u32 data_offset           v2+: i32 layer, u32 data_offset, u32 data_size
 *     v3 additionally: (num_layers + 2) / 3 * {u32 size, size bytes of texture file}
 *   Mask data (v1, v2) of every layer but the first: mask_map_size^2 bytes, zlib compressed
 *   (data_size bytes) when the header says so. The first layer has no mask (fully opaque).
 *
 * `.t2mk` LOADER_A (magic 'T2MK' = 0x54324D4B, version <= 3)
 *   u32 magic, u32 version, i32 num_mask_areas, 7 * {u32 offset, u32 size} lumps:
 *   texture index (i32), specular index (i32), mask size (i32), mask data offset (u32),
 *   projection axis (u8), uv scale ({f32, f32}), weight (u8).
 *   num_mask_areas * u32 mask area offset; mask area: i32 num_layers, i32 first_index into the
 *   lumps. Mask data is raw mask_size^2 bytes.
 *
 * `.t2lm` / `.t2lm1` (magic 'T2LM' = 0x54324C4D, version 1..2)
 *   u32 magic, u32 version
 *   v1: i32 num_mask_areas, i32 lightmap_size              -> offsets at 16, RGB24 data
 *   v2: i32 num_mask_areas, i32 lightmap_size, u32 flags, i32 data_size -> offsets at 24
 *   num_mask_areas * u32 offset; each area is data_size bytes: a DDS file when flags & 1,
 *   otherwise lightmap_size^2 * 3 bytes.
 */

#include "angelica_terrain.hh"

#include <algorithm>
#include <bit>
#include <cmath>
#include <filesystem>
#include <unordered_map>

namespace blender::io::angelica {

namespace {

constexpr uint32_t TRN2_MAGIC = fourcc('T', 'R', 'N', '2');
constexpr uint32_t T2BK_MAGIC = fourcc('T', 'B', 'K', 'B');
constexpr uint32_t T2MK_MAGIC_B = fourcc('T', 'M', 'K', 'B');
constexpr uint32_t T2MK_MAGIC_A = fourcc('T', '2', 'M', 'K');
constexpr uint32_t T2LM_MAGIC = fourcc('T', '2', 'L', 'M');

constexpr uint32_t TRN2_VERSION = 1;
constexpr uint32_t T2BK_VERSION = 6;
constexpr uint32_t T2MK_VERSION = 3;
constexpr uint32_t T2LM_VERSION = 2;

/* Sanity limits, far above anything the engine produces. */
constexpr int MAX_BLOCK_GRID = 1024;
constexpr int MAX_BLOCKS_PER_SIDE = 4096;
constexpr int MAX_TEXTURES = 65536;
constexpr int MAX_SUB_TERRAINS = 65536;
constexpr int MAX_LAYERS = 4096;
constexpr int MAX_LAYERS_PER_AREA = 256;
constexpr int MAX_MASK_SIZE = 8192;
constexpr int MAX_LIGHTMAP_SIZE = 8192;
constexpr int64_t MAX_TERRAIN_VERTICES = int64_t(1) << 28;

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

bool read_u32_array(BinaryReader &reader, const size_t count, std::vector<uint32_t> &r_values)
{
  if (count > reader.remaining() / 4) {
    return false;
  }
  r_values.resize(count);
  for (size_t i = 0; i < count; i++) {
    r_values[i] = reader.u32();
  }
  return !reader.failed();
}

/**
 * `A3DTerrain2::DecompressHeightMap`: a header `{u8 version; f32 min; f32 max; i32 count}`
 * (16 bytes with MSVC padding) followed by `count` i16 deltas of a running sum `d`;
 * `height = min + d * (max - min) / 32767`.
 */
bool decode_incremental_heights(ByteSpan src, const size_t num_vert, std::vector<float> &r_heights)
{
  /* Padded layout first, then a packed 13-byte header in case a tool wrote one. */
  struct Layout {
    size_t min_pos, max_pos, count_pos, data_pos;
  };
  const Layout layouts[] = {{4, 8, 12, 16}, {1, 5, 9, 13}};
  for (const Layout &l : layouts) {
    BinaryReader reader(src);
    reader.seek(l.min_pos);
    const float min_h = reader.f32();
    reader.seek(l.max_pos);
    const float max_h = reader.f32();
    reader.seek(l.count_pos);
    const int32_t count = reader.i32();
    if (reader.failed() || count < 0 || size_t(count) != num_vert) {
      continue;
    }
    if (src.size() < l.data_pos + num_vert * 2) {
      continue;
    }
    reader.seek(l.data_pos);
    const float delta = (max_h - min_h) / 32767.0f;
    r_heights.resize(num_vert);
    int32_t d = 0;
    for (size_t i = 0; i < num_vert; i++) {
      d += reader.i16();
      r_heights[i] = min_h + float(d) * delta;
    }
    return true;
  }
  return false;
}

/** Table of `sin` / `cos` for the 256 direction steps of `a3d_DecompressDir`. */
struct DirTables {
  float sin[256];
  float cos[256];
  DirTables()
  {
    for (int i = 0; i < 256; i++) {
      const double a = double(i) * 2.0 * 3.14159265358979323846 / 256.0;
      sin[i] = float(std::sin(a));
      cos[i] = float(std::cos(a));
    }
  }
};

/**
 * `A3DTerrain2::DecompressNormalMap`: header `{u8 version; i32 count}` (8 bytes padded), then two
 * bytes per vertex: `b1` the heading and `b2` the angle from +Y, both in 1/256 turns.
 */
bool decode_degree_normals(ByteSpan src, const size_t num_vert, std::vector<float> &r_normals)
{
  static const DirTables tables;
  const size_t count_positions[] = {4, 1};
  for (const size_t count_pos : count_positions) {
    BinaryReader reader(src);
    reader.seek(count_pos);
    const int32_t count = reader.i32();
    if (reader.failed() || count < 0 || size_t(count) != num_vert) {
      continue;
    }
    const size_t data_pos = count_pos + 4;
    if (src.size() < data_pos + num_vert * 2) {
      continue;
    }
    r_normals.resize(num_vert * 3);
    const uint8_t *p = src.data() + data_pos;
    for (size_t i = 0; i < num_vert; i++) {
      const uint8_t b1 = p[i * 2];
      const uint8_t b2 = p[i * 2 + 1];
      const float s = tables.sin[b2];
      r_normals[i * 3 + 0] = s * tables.cos[b1];
      r_normals[i * 3 + 1] = tables.cos[b2];
      r_normals[i * 3 + 2] = s * tables.sin[b1];
    }
    return true;
  }
  return false;
}

bool bytes_to_floats(ByteSpan src, const size_t count, std::vector<float> &r_values)
{
  if (src.size() < count * sizeof(float)) {
    return false;
  }
  r_values.resize(count);
  std::memcpy(r_values.data(), src.data(), count * sizeof(float));
  return true;
}

bool load_file(const std::string &filepath, Bytes &r_data, std::string *r_error)
{
  if (!read_file(filepath, r_data)) {
    return fail(r_error, "Cannot read \"" + filepath + "\"");
  }
  return true;
}

/**
 * Unpack the 8-bit masks of three layers from an uncompressed 32-bit DDS mask texture (mask
 * file version 3). Returns false for other texture formats.
 */
bool unpack_dds_masks(ByteSpan dds, std::vector<Bytes> &r_channels, int &r_size)
{
  BinaryReader reader(dds);
  if (reader.u32() != fourcc(' ', 'S', 'D', 'D')) {
    return false;
  }
  reader.seek(12);
  const uint32_t height = reader.u32();
  const uint32_t width = reader.u32();
  reader.seek(80);
  const uint32_t pf_flags = reader.u32();
  reader.seek(88);
  const uint32_t bit_count = reader.u32();
  const uint32_t r_mask = reader.u32();
  const uint32_t g_mask = reader.u32();
  const uint32_t b_mask = reader.u32();
  if (reader.failed() || !(pf_flags & 0x40) || bit_count != 32 || width != height || width == 0 ||
      width > uint32_t(MAX_MASK_SIZE))
  {
    return false;
  }
  const size_t num_pixels = size_t(width) * width;
  if (dds.size() < 128 + num_pixels * 4) {
    return false;
  }
  const uint32_t masks[3] = {b_mask, g_mask, r_mask};
  r_channels.assign(3, Bytes());
  for (int k = 0; k < 3; k++) {
    if (masks[k] == 0) {
      continue;
    }
    const int shift = std::countr_zero(masks[k]);
    Bytes &channel = r_channels[k];
    channel.resize(num_pixels);
    const uint8_t *src = dds.data() + 128;
    for (size_t i = 0; i < num_pixels; i++) {
      uint32_t v;
      std::memcpy(&v, src + i * 4, 4);
      channel[i] = uint8_t((v & masks[k]) >> shift);
    }
  }
  r_size = int(width);
  return true;
}

}  // namespace

/* -------------------------------------------------------------------- */
/** \name Files
 * \{ */

std::string find_file_case_insensitive(const std::string &dir, const std::string &name)
{
  std::error_code ec;
  const std::filesystem::path base = dir.empty() ? std::filesystem::path(".") : u8path(dir);
  const std::filesystem::path exact = base / u8path(name);
  if (std::filesystem::is_regular_file(exact, ec)) {
    return path_to_utf8(exact);
  }
  const std::string wanted = ascii_lower(name);
  for (std::filesystem::directory_iterator it(base, ec), end; !ec && it != end; it.increment(ec)) {
    if (ascii_lower(path_to_utf8(it->path().filename())) == wanted && it->is_regular_file(ec)) {
      return path_to_utf8(it->path());
    }
  }
  return {};
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name `.trn2`
 * \{ */

bool parse_trn2(ByteSpan data, Trn2Info &r_info, std::string *r_error)
{
  r_info = Trn2Info();
  BinaryReader reader(data);
  if (reader.u32() != TRN2_MAGIC) {
    return fail(r_error, "Not a .trn2 file (bad magic)");
  }
  Trn2Info &info = r_info;
  info.version = reader.u32();
  if (info.version > TRN2_VERSION) {
    return fail(r_error, ".trn2 version " + std::to_string(info.version) + " is not supported");
  }
  info.terrain_width = reader.f32();
  info.terrain_height = reader.f32();
  info.block_size = reader.f32();
  info.block_grid = reader.i32();
  info.num_blocks = reader.i32();
  info.block_rows = reader.i32();
  info.block_cols = reader.i32();
  info.num_textures = reader.i32();
  info.num_mask_areas = reader.i32();
  info.mask_blocks = reader.i32();
  info.mask_grid = reader.i32();
  info.num_sub_terrains = reader.i32();
  info.sub_terrain_rows = reader.i32();
  info.sub_terrain_cols = reader.i32();
  info.sub_terrain_grid = reader.i32();
  info.grid_size = reader.f32();
  info.texture_offset = reader.u32();
  info.loader_id = reader.i32();
  info.loader_offset = reader.u32();
  reader.skip(56);
  if (reader.failed()) {
    return fail(r_error, ".trn2 header is truncated");
  }

  if (info.block_grid <= 0 || info.block_grid > MAX_BLOCK_GRID || info.block_rows <= 0 ||
      info.block_cols <= 0 || info.block_rows > MAX_BLOCKS_PER_SIDE ||
      info.block_cols > MAX_BLOCKS_PER_SIDE)
  {
    return fail(r_error, ".trn2 has invalid block dimensions");
  }
  if (info.num_blocks != info.block_rows * info.block_cols) {
    return fail(r_error, ".trn2 block count does not match its rows and columns");
  }
  if (!(std::isfinite(info.grid_size) && info.grid_size > 0.0f) ||
      !std::isfinite(info.terrain_width) || !std::isfinite(info.terrain_height))
  {
    return fail(r_error, ".trn2 has an invalid grid size");
  }
  if (info.num_textures < 0 || info.num_textures > MAX_TEXTURES) {
    return fail(r_error, ".trn2 has an invalid texture count");
  }
  if (info.sub_terrain_rows < 0 || info.sub_terrain_cols < 0 ||
      int64_t(info.sub_terrain_rows) * info.sub_terrain_cols > MAX_SUB_TERRAINS ||
      info.num_sub_terrains < 0 || info.num_sub_terrains > MAX_SUB_TERRAINS)
  {
    return fail(r_error, ".trn2 has an invalid sub-terrain count");
  }
  const int64_t verts = (int64_t(info.block_cols) * info.block_grid + 1) *
                        (int64_t(info.block_rows) * info.block_grid + 1);
  if (verts > MAX_TERRAIN_VERTICES) {
    return fail(r_error, ".trn2 terrain is too large");
  }

  const int num_sub_info = info.sub_terrain_rows * info.sub_terrain_cols;
  info.sub_terrains.resize(size_t(num_sub_info));
  for (Trn2SubTerrain &sub : info.sub_terrains) {
    sub.min_height = reader.f32();
    sub.max_height = reader.f32();
  }
  if (reader.failed()) {
    return fail(r_error, ".trn2 sub-terrain table is truncated");
  }

  if (info.loader_id == int32_t(Trn2LoaderId::A)) {
    if (!read_u32_array(reader, size_t(info.num_blocks), info.block_offsets)) {
      return fail(r_error, ".trn2 block offset table is truncated");
    }
  }

  if (!reader.seek(info.texture_offset)) {
    return fail(r_error, ".trn2 texture table offset is out of range");
  }
  info.texture_paths.reserve(size_t(info.num_textures));
  for (int i = 0; i < info.num_textures; i++) {
    const std::string path = reader.string_i32();
    if (reader.failed()) {
      return fail(r_error, ".trn2 texture table is truncated");
    }
    info.texture_paths.push_back(gbk_to_utf8(path));
  }

  if (info.loader_id == int32_t(Trn2LoaderId::B)) {
    if (!reader.seek(info.loader_offset)) {
      return fail(r_error, ".trn2 loader section offset is out of range");
    }
    info.mask_map_size = reader.i32();
    if (info.mask_map_size == 0) {
      /* The engine assumes 32 when the whole terrain has a single (mask-less) layer. */
      info.mask_map_size = 32;
    }
    if (info.mask_map_size < 0 || info.mask_map_size > MAX_MASK_SIZE) {
      return fail(r_error, ".trn2 has an invalid mask map size");
    }
    info.sub_terrain_names.reserve(size_t(info.num_sub_terrains));
    for (int i = 0; i < info.num_sub_terrains; i++) {
      const std::string name = reader.string_i32();
      if (reader.failed()) {
        return fail(r_error, ".trn2 sub-terrain name list is truncated");
      }
      info.sub_terrain_names.push_back(gbk_to_utf8(name));
    }
  }
  else if (info.loader_id != int32_t(Trn2LoaderId::A)) {
    return fail(r_error, ".trn2 uses unknown loader " + std::to_string(info.loader_id));
  }
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name `.t2bk`
 * \{ */

bool T2bkFile::load(const std::string &filepath, const int block_grid, std::string *r_error)
{
  Bytes data;
  if (!load_file(filepath, data, r_error)) {
    return false;
  }
  if (!parse(std::move(data), block_grid, r_error)) {
    if (r_error) {
      *r_error = "\"" + filepath + "\": " + *r_error;
    }
    return false;
  }
  return true;
}

bool T2bkFile::parse(Bytes data, const int block_grid, std::string *r_error)
{
  data_ = std::move(data);
  offsets_.clear();
  if (block_grid <= 0 || block_grid > MAX_BLOCK_GRID) {
    return fail(r_error, "Invalid block grid");
  }
  block_grid_ = block_grid;

  BinaryReader reader(data_);
  if (reader.u32() != T2BK_MAGIC) {
    return fail(r_error, "Not a .t2bk file (bad magic)");
  }
  version_ = reader.u32();
  if (version_ > T2BK_VERSION) {
    return fail(r_error, ".t2bk version " + std::to_string(version_) + " is not supported");
  }
  const int32_t num_blocks = reader.i32();
  if (version_ < 2) {
    compression_ = 0;
  }
  else if (version_ < 5) {
    compression_ = reader.u8() ? (T2BK_COL_ZIP | T2BK_HEI_ZIP | T2BK_NOR_ZIP) : 0;
    reader.skip(3);
  }
  else {
    compression_ = reader.u8();
    reader.skip(3);
  }
  if (reader.failed() || num_blocks < 0) {
    return fail(r_error, ".t2bk header is truncated or invalid");
  }
  if (!read_u32_array(reader, size_t(num_blocks), offsets_)) {
    offsets_.clear();
    return fail(r_error, ".t2bk block offset table is truncated");
  }
  return true;
}

bool T2bkFile::parse_loader_a(Bytes trn2_data, const Trn2Info &info, std::string *r_error)
{
  data_ = std::move(trn2_data);
  offsets_.clear();
  if (info.loader_id != int32_t(Trn2LoaderId::A)) {
    return fail(r_error, "Terrain does not store its blocks in the .trn2 (not LOADER_A)");
  }
  if (info.block_grid <= 0 || info.block_grid > MAX_BLOCK_GRID) {
    return fail(r_error, "Invalid block grid");
  }
  block_grid_ = info.block_grid;
  version_ = 1;
  compression_ = 0;
  offsets_ = info.block_offsets;
  return true;
}

bool T2bkFile::read_block(const int index, TerrainBlock &r_block, std::string *r_error) const
{
  r_block = TerrainBlock();
  if (index < 0 || size_t(index) >= offsets_.size()) {
    return fail(r_error, "Block index " + std::to_string(index) + " out of range");
  }
  const std::string where = "block " + std::to_string(index);
  BinaryReader reader(data_);
  if (!reader.seek(offsets_[size_t(index)])) {
    return fail(r_error, where + ": offset out of range");
  }

  const size_t num_vert = size_t(block_grid_ + 1) * size_t(block_grid_ + 1);
  r_block.mask_area = reader.i32();
  r_block.row_in_terrain = reader.u16();
  r_block.col_in_terrain = reader.u16();
  r_block.lod_scale = reader.f32();
  r_block.layer_flags = reader.u32();
  uint32_t hei_size = uint32_t(num_vert * 4);
  uint32_t nor_size = uint32_t(num_vert * 12);
  uint32_t diff_size = 0;
  uint32_t diff_size_night = 0;
  if (version_ >= 2) {
    hei_size = reader.u32();
    nor_size = reader.u32();
  }
  if (version_ >= 3) {
    diff_size = reader.u32();
  }
  if (version_ >= 4) {
    diff_size_night = reader.u32();
  }
  if (version_ >= 6) {
    r_block.block_flags = reader.u32();
  }
  if (reader.failed()) {
    return fail(r_error, where + ": record is truncated");
  }

  const ByteSpan hei_data = reader.read_span(hei_size);
  const ByteSpan nor_data = reader.read_span(nor_size);
  if (reader.failed()) {
    return fail(r_error, where + ": height or normal data is truncated");
  }

  /* Heights. */
  {
    const bool zip = (compression_ & T2BK_HEI_ZIP) != 0;
    const bool inc = (compression_ & T2BK_HEI_INC) != 0;
    Bytes inflated;
    ByteSpan src = hei_data;
    if (zip) {
      if (!zlib_inflate(hei_data, inc ? 16 + num_vert * 2 : num_vert * 4, inflated)) {
        return fail(r_error, where + ": cannot decompress heights");
      }
      src = inflated;
    }
    const bool ok = inc ? decode_incremental_heights(src, num_vert, r_block.heights) :
                          bytes_to_floats(src, num_vert, r_block.heights);
    if (!ok) {
      return fail(r_error, where + ": height data has the wrong size");
    }
  }

  /* Normals. */
  if (nor_size > 0) {
    const bool zip = (compression_ & T2BK_NOR_ZIP) != 0;
    const bool deg = (compression_ & T2BK_NOR_DEG) != 0;
    Bytes inflated;
    ByteSpan src = nor_data;
    if (zip) {
      if (!zlib_inflate(nor_data, deg ? 8 + num_vert * 2 : num_vert * 12, inflated)) {
        return fail(r_error, where + ": cannot decompress normals");
      }
      src = inflated;
    }
    const bool ok = deg ? decode_degree_normals(src, num_vert, r_block.normals) :
                          bytes_to_floats(src, num_vert * 3, r_block.normals);
    if (!ok) {
      return fail(r_error, where + ": normal data has the wrong size");
    }
  }

  /* Vertex colors are optional (only used without lightmaps): decode what is there. */
  auto decode_colors = [&](const uint32_t size, std::vector<uint32_t> &r_colors) {
    const ByteSpan raw = reader.read_span(size);
    if (reader.failed() || size == 0) {
      return;
    }
    Bytes inflated;
    ByteSpan src = raw;
    if (compression_ & T2BK_COL_ZIP) {
      if (!zlib_inflate(raw, num_vert * 4, inflated)) {
        return;
      }
      src = inflated;
    }
    if (src.size() >= num_vert * 4) {
      r_colors.resize(num_vert);
      std::memcpy(r_colors.data(), src.data(), num_vert * 4);
    }
  };
  if (version_ >= 3) {
    decode_colors(diff_size, r_block.diffuse);
  }
  if (version_ >= 4) {
    decode_colors(diff_size_night, r_block.diffuse_night);
  }
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name `.t2mk`
 * \{ */

bool T2mkFile::load(const std::string &filepath, std::string *r_error)
{
  Bytes data;
  if (!load_file(filepath, data, r_error)) {
    return false;
  }
  if (!parse(data, r_error)) {
    if (r_error) {
      *r_error = "\"" + filepath + "\": " + *r_error;
    }
    return false;
  }
  return true;
}

static bool parse_t2mk_loader_a(ByteSpan data, T2mkFile &r_file, std::string *r_error)
{
  BinaryReader reader(data);
  reader.skip(8);
  const int32_t num_areas = reader.i32();
  struct Lump {
    uint32_t offset, size;
  };
  Lump lumps[7];
  for (Lump &lump : lumps) {
    lump.offset = reader.u32();
    lump.size = reader.u32();
  }
  if (reader.failed() || num_areas < 0) {
    return fail(r_error, ".t2mk header is truncated or invalid");
  }
  std::vector<uint32_t> area_offsets;
  if (!read_u32_array(reader, size_t(num_areas), area_offsets)) {
    return fail(r_error, ".t2mk mask area table is truncated");
  }
  ByteSpan lump_data[7];
  for (int i = 0; i < 7; i++) {
    if (lumps[i].offset > data.size() || lumps[i].size > data.size() - lumps[i].offset) {
      return fail(r_error, ".t2mk lump " + std::to_string(i) + " is out of range");
    }
    lump_data[i] = data.subspan(lumps[i].offset, lumps[i].size);
  }
  /* Element counts: four 4-byte lumps, two byte lumps and one 8-byte lump. */
  const size_t elem_size[7] = {4, 4, 4, 4, 1, 8, 1};
  auto lump_count = [&](const int lump) { return lump_data[lump].size() / elem_size[lump]; };
  auto lump_u32 = [&](const int lump, const size_t i) {
    uint32_t v;
    std::memcpy(&v, lump_data[lump].data() + i * 4, 4);
    return v;
  };
  auto lump_f32 = [&](const int lump, const size_t i) {
    float v;
    std::memcpy(&v, lump_data[lump].data() + i * 4, 4);
    return v;
  };

  r_file.mask_areas.resize(size_t(num_areas));
  for (int32_t a = 0; a < num_areas; a++) {
    BinaryReader area_reader(data);
    area_reader.seek(area_offsets[size_t(a)]);
    const int32_t num_layers = area_reader.i32();
    const int32_t first = area_reader.i32();
    if (area_reader.failed() || num_layers < 0 || num_layers > MAX_LAYERS_PER_AREA || first < 0) {
      return fail(r_error, ".t2mk mask area " + std::to_string(a) + " is invalid");
    }
    T2mkMaskArea &area = r_file.mask_areas[size_t(a)];
    area.layers.resize(size_t(num_layers));
    for (int32_t l = 0; l < num_layers; l++) {
      const size_t idx = size_t(first) + size_t(l);
      for (int lump = 0; lump < 7; lump++) {
        if (idx >= lump_count(lump)) {
          return fail(r_error,
                      ".t2mk mask area " + std::to_string(a) + " indexes past lump " +
                          std::to_string(lump));
        }
      }
      T2mkAreaLayer &dst = area.layers[size_t(l)];
      dst.layer_index = int(idx);
      dst.layer.texture_index = int32_t(lump_u32(0, idx));
      dst.layer.specular_index = int32_t(lump_u32(1, idx));
      dst.layer.mask_map_size = int32_t(lump_u32(2, idx));
      dst.layer.proj_axis = lump_data[4][idx];
      dst.layer.u_scale = lump_f32(5, idx * 2);
      dst.layer.v_scale = lump_f32(5, idx * 2 + 1);
      dst.layer.weight = lump_data[6][idx];
      if (l == 0) {
        continue;
      }
      const int size = dst.layer.mask_map_size;
      if (size <= 0 || size > MAX_MASK_SIZE) {
        return fail(r_error, ".t2mk mask area " + std::to_string(a) + " has an invalid mask size");
      }
      const uint32_t off = lump_u32(3, idx);
      const size_t bytes = size_t(size) * size_t(size);
      if (off > data.size() || bytes > data.size() - off) {
        return fail(r_error, ".t2mk mask data of area " + std::to_string(a) + " is out of range");
      }
      dst.mask_size = size;
      dst.mask.assign(data.begin() + off, data.begin() + off + bytes);
    }
  }
  return true;
}

bool T2mkFile::parse(ByteSpan data, std::string *r_error)
{
  version = 0;
  compressed = false;
  layers.clear();
  mask_areas.clear();

  BinaryReader reader(data);
  const uint32_t magic = reader.u32();
  version = reader.u32();
  if (reader.failed() || (magic != T2MK_MAGIC_B && magic != T2MK_MAGIC_A)) {
    return fail(r_error, "Not a .t2mk file (bad magic)");
  }
  if (version > T2MK_VERSION) {
    return fail(r_error, ".t2mk version " + std::to_string(version) + " is not supported");
  }
  if (magic == T2MK_MAGIC_A) {
    return parse_t2mk_loader_a(data, *this, r_error);
  }

  const int32_t num_areas = reader.i32();
  const int32_t num_layers = reader.i32();
  if (version >= 2) {
    compressed = reader.u8() != 0;
    reader.skip(3);
  }
  if (reader.failed() || num_areas < 0 || num_layers < 0 || num_layers > MAX_LAYERS) {
    return fail(r_error, ".t2mk header is truncated or invalid");
  }
  std::vector<uint32_t> area_offsets;
  if (!read_u32_array(reader, size_t(num_areas), area_offsets)) {
    return fail(r_error, ".t2mk mask area table is truncated");
  }
  layers.resize(size_t(num_layers));
  for (T2mkLayer &layer : layers) {
    layer.texture_index = reader.i32();
    layer.specular_index = reader.i32();
    layer.mask_map_size = reader.i32();
    layer.proj_axis = reader.u8();
    reader.skip(3);
    layer.u_scale = reader.f32();
    layer.v_scale = reader.f32();
    layer.weight = reader.u8();
    reader.skip(3);
  }
  if (reader.failed()) {
    return fail(r_error, ".t2mk layer table is truncated");
  }

  mask_areas.resize(size_t(num_areas));
  for (int32_t a = 0; a < num_areas; a++) {
    const std::string where = ".t2mk mask area " + std::to_string(a);
    BinaryReader area_reader(data);
    area_reader.seek(area_offsets[size_t(a)]);
    const int32_t area_num_layers = area_reader.i32();
    if (area_reader.failed() || area_num_layers < 0 || area_num_layers > MAX_LAYERS_PER_AREA) {
      return fail(r_error, where + " is invalid");
    }
    struct Ref {
      int32_t layer;
      uint32_t offset, size;
    };
    std::vector<Ref> refs(static_cast<size_t>(area_num_layers));
    for (Ref &ref : refs) {
      ref.layer = area_reader.i32();
      ref.offset = area_reader.u32();
      ref.size = version >= 2 ? area_reader.u32() : 0;
      if (ref.layer < 0 || ref.layer >= num_layers) {
        return fail(r_error, where + " references a missing layer");
      }
    }
    if (area_reader.failed()) {
      return fail(r_error, where + " is truncated");
    }

    T2mkMaskArea &area = mask_areas[size_t(a)];
    area.layers.resize(refs.size());
    for (size_t l = 0; l < refs.size(); l++) {
      T2mkAreaLayer &dst = area.layers[l];
      dst.layer_index = refs[l].layer;
      dst.layer = layers[size_t(refs[l].layer)];
    }

    if (version >= 3) {
      /* Masks stored as ready-made textures after the layer list. */
      const size_t num_tex = (refs.size() + 2) / 3;
      for (size_t t = 0; t < num_tex; t++) {
        const uint32_t size = area_reader.u32();
        const ByteSpan tex = area_reader.read_span(size);
        if (area_reader.failed()) {
          return fail(r_error, where + ": mask texture is truncated");
        }
        area.mask_textures.emplace_back(tex.begin(), tex.end());
        std::vector<Bytes> channels;
        int size_px = 0;
        if (unpack_dds_masks(tex, channels, size_px)) {
          for (size_t k = 0; k < 3; k++) {
            const size_t l = t * 3 + k;
            if (l == 0 || l >= area.layers.size() || channels[k].empty()) {
              continue;
            }
            area.layers[l].mask_size = size_px;
            area.layers[l].mask = std::move(channels[k]);
          }
        }
      }
      continue;
    }

    for (size_t l = 1; l < refs.size(); l++) {
      T2mkAreaLayer &dst = area.layers[l];
      const int size = dst.layer.mask_map_size;
      if (size <= 0 || size > MAX_MASK_SIZE) {
        return fail(r_error, where + " has an invalid mask size");
      }
      const size_t bytes = size_t(size) * size_t(size);
      const uint32_t off = refs[l].offset;
      const size_t stored = compressed ? refs[l].size : bytes;
      if (off > data.size() || stored > data.size() - off) {
        return fail(r_error, where + ": mask data is out of range");
      }
      const ByteSpan src = data.subspan(off, stored);
      if (compressed) {
        if (!zlib_inflate(src, bytes, dst.mask) || dst.mask.size() != bytes) {
          dst.mask.clear();
          return fail(r_error, where + ": cannot decompress mask");
        }
      }
      else {
        dst.mask.assign(src.begin(), src.end());
      }
      dst.mask_size = size;
    }
  }
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name `.t2lm`
 * \{ */

bool T2lmFile::load(const std::string &filepath, std::string *r_error)
{
  Bytes data;
  if (!load_file(filepath, data, r_error)) {
    return false;
  }
  if (!parse(std::move(data), r_error)) {
    if (r_error) {
      *r_error = "\"" + filepath + "\": " + *r_error;
    }
    return false;
  }
  return true;
}

bool T2lmFile::parse(Bytes data, std::string *r_error)
{
  data_ = std::move(data);
  offsets_.clear();
  BinaryReader reader(data_);
  if (reader.u32() != T2LM_MAGIC) {
    return fail(r_error, "Not a .t2lm file (bad magic)");
  }
  version_ = reader.u32();
  if (version_ > T2LM_VERSION) {
    return fail(r_error, ".t2lm version " + std::to_string(version_) + " is not supported");
  }
  const int32_t num_areas = reader.i32();
  lightmap_size_ = reader.i32();
  if (lightmap_size_ < 0 || lightmap_size_ > MAX_LIGHTMAP_SIZE) {
    return fail(r_error, ".t2lm has an invalid lightmap size");
  }
  if (version_ < 2) {
    flags_ = 0;
    data_size_ = lightmap_size_ * lightmap_size_ * 3;
  }
  else {
    flags_ = reader.u32();
    data_size_ = reader.i32();
  }
  if (reader.failed() || num_areas < 0 || data_size_ < 0) {
    return fail(r_error, ".t2lm header is truncated or invalid");
  }
  if (!read_u32_array(reader, size_t(num_areas), offsets_)) {
    offsets_.clear();
    return fail(r_error, ".t2lm offset table is truncated");
  }
  return true;
}

ByteSpan T2lmFile::area_data(const int index) const
{
  if (index < 0 || size_t(index) >= offsets_.size()) {
    return {};
  }
  const size_t off = offsets_[size_t(index)];
  const size_t size = size_t(data_size_);
  if (off > data_.size() || size > data_.size() - off) {
    return {};
  }
  return ByteSpan(data_).subspan(off, size);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Whole Terrain
 * \{ */

namespace {

struct FieldBuilder {
  const Trn2Info &info;
  TerrainHeightfield &field;
  bool have_normals = true;

  void put_block(const int row, const int col, const TerrainBlock &block)
  {
    const int n = info.block_grid + 1;
    for (int i = 0; i < n; i++) {
      const size_t dst_row = size_t(row) * size_t(info.block_grid) + size_t(i);
      for (int j = 0; j < n; j++) {
        const size_t dst = dst_row * size_t(field.verts_x) + size_t(col) * info.block_grid + j;
        const size_t src = size_t(i) * size_t(n) + size_t(j);
        field.heights[dst] = block.heights[src];
        if (have_normals && !block.normals.empty()) {
          std::copy_n(&block.normals[src * 3], 3, &field.normals[dst * 3]);
        }
      }
    }
    if (block.normals.empty()) {
      have_normals = false;
    }
  }

  void put_masks(const T2mkMaskArea &src, const int area_index, const int sub_terrain)
  {
    TerrainMaskArea &dst = field.mask_areas[size_t(area_index)];
    dst.sub_terrain = sub_terrain;
    dst.layers.clear();
    for (size_t l = 0; l < src.layers.size(); l++) {
      const T2mkAreaLayer &in = src.layers[l];
      TerrainMaskLayer out;
      out.texture_index = in.layer.texture_index;
      if (out.texture_index < 0 || out.texture_index >= int(field.texture_paths.size())) {
        field.warnings.push_back("Mask area " + std::to_string(area_index) + " layer " +
                                 std::to_string(l) + " uses missing texture " +
                                 std::to_string(in.layer.texture_index));
        out.texture_index = -1;
      }
      out.proj_axis = in.layer.proj_axis;
      out.u_scale = in.layer.u_scale;
      out.v_scale = in.layer.v_scale;
      out.weight = in.layer.weight;
      out.mask_size = in.mask_size;
      out.mask = in.mask;
      if (l > 0 && out.mask.empty()) {
        field.warnings.push_back("Mask area " + std::to_string(area_index) + " layer " +
                                 std::to_string(l) + " has no decodable mask");
      }
      dst.layers.push_back(std::move(out));
    }
  }

  void put_lightmap(const T2lmFile &lm, const int local_index, const int area_index)
  {
    const ByteSpan blob = lm.area_data(local_index);
    TerrainMaskArea &dst = field.mask_areas[size_t(area_index)];
    if (blob.empty()) {
      field.warnings.push_back("Mask area " + std::to_string(area_index) + " has no lightmap");
      return;
    }
    dst.lightmap.assign(blob.begin(), blob.end());
    dst.lightmap_is_dds = lm.is_dds();
    dst.lightmap_size = lm.lightmap_size();
  }
};

}  // namespace

bool load_terrain_heightfield(const std::string &map_dir,
                              const std::string &map_code,
                              TerrainHeightfield &r_field,
                              std::string *r_error)
{
  r_field = TerrainHeightfield();
  const std::string trn2_path = find_file_case_insensitive(map_dir, map_code + ".trn2");
  if (trn2_path.empty()) {
    return fail(r_error,
                "Terrain file \"" + map_code + ".trn2\" not found in \"" + map_dir + "\"");
  }
  Bytes trn2_data;
  if (!load_file(trn2_path, trn2_data, r_error)) {
    return false;
  }
  Trn2Info info;
  if (!parse_trn2(trn2_data, info, r_error)) {
    if (r_error) {
      *r_error = "\"" + trn2_path + "\": " + *r_error;
    }
    return false;
  }

  TerrainHeightfield &field = r_field;
  field.grid_size = info.grid_size;
  field.terrain_width = info.terrain_width;
  field.terrain_height = info.terrain_height;
  field.origin_x = -info.terrain_width * 0.5f;
  field.origin_z = info.terrain_height * 0.5f;
  field.verts_x = info.block_cols * info.block_grid + 1;
  field.verts_z = info.block_rows * info.block_grid + 1;
  field.heights.assign(size_t(field.verts_x) * size_t(field.verts_z), 0.0f);
  field.normals.assign(field.heights.size() * 3, 0.0f);
  field.texture_paths = info.texture_paths;
  field.sub_terrain_names = info.sub_terrain_names;
  field.sub_terrains = info.sub_terrains;

  const bool has_masks = info.mask_grid > 0 && info.mask_grid % info.block_grid == 0;
  if (has_masks) {
    field.mask_grid = info.mask_grid;
    field.mask_areas_x = info.block_cols * info.block_grid / info.mask_grid;
    field.mask_areas_z = info.block_rows * info.block_grid / info.mask_grid;
    field.mask_areas.resize(size_t(field.mask_areas_x) * size_t(field.mask_areas_z));
    for (int r = 0; r < field.mask_areas_z; r++) {
      for (int c = 0; c < field.mask_areas_x; c++) {
        TerrainMaskArea &area = field.mask_areas[size_t(r) * field.mask_areas_x + c];
        area.row = r;
        area.col = c;
      }
    }
  }
  else {
    field.warnings.push_back("Terrain has an invalid mask grid, texture layers are skipped");
  }

  FieldBuilder builder{info, field};
  TerrainBlock block;
  std::string err;

  if (info.loader_id == int32_t(Trn2LoaderId::A)) {
    T2bkFile blocks;
    if (!blocks.parse_loader_a(trn2_data, info, r_error)) {
      return false;
    }
    for (int b = 0; b < blocks.num_blocks(); b++) {
      if (!blocks.read_block(b, block, &err)) {
        return fail(r_error, "\"" + trn2_path + "\": " + err);
      }
      builder.put_block(b / info.block_cols, b % info.block_cols, block);
    }
    if (has_masks) {
      const std::string mk_path = find_file_case_insensitive(map_dir, map_code + ".t2mk");
      T2mkFile masks;
      if (mk_path.empty()) {
        field.warnings.push_back("\"" + map_code + ".t2mk\" not found");
      }
      else if (!masks.load(mk_path, &err)) {
        field.warnings.push_back(err);
      }
      else {
        const size_t n = std::min(masks.mask_areas.size(), field.mask_areas.size());
        for (size_t a = 0; a < n; a++) {
          builder.put_masks(masks.mask_areas[a], int(a), 0);
        }
      }
      const std::string lm_path = find_file_case_insensitive(map_dir, map_code + ".t2lm");
      T2lmFile lm;
      if (!lm_path.empty()) {
        if (!lm.load(lm_path, &err)) {
          field.warnings.push_back(err);
        }
        else {
          const int n = std::min(lm.num_mask_areas(), int(field.mask_areas.size()));
          for (int a = 0; a < n; a++) {
            builder.put_lightmap(lm, a, a);
          }
        }
      }
    }
  }
  else {
    /* LOADER_B: one set of files per sub-terrain. */
    const int sub_blocks = info.blocks_per_sub_terrain_side();
    if (sub_blocks <= 0 || info.sub_terrain_grid % info.block_grid != 0 ||
        info.block_rows % sub_blocks != 0 || info.block_cols % sub_blocks != 0)
    {
      return fail(r_error, "\"" + trn2_path + "\": invalid sub-terrain grid");
    }
    const int subs_x = info.block_cols / sub_blocks;
    const int subs_z = info.block_rows / sub_blocks;
    if (int64_t(subs_x) * subs_z > int64_t(info.sub_terrain_names.size())) {
      return fail(r_error, "\"" + trn2_path + "\": fewer sub-terrain names than sub-terrains");
    }
    const bool sub_masks = has_masks && info.sub_terrain_grid % info.mask_grid == 0;
    const int sub_mask_areas = sub_masks ? info.sub_terrain_grid / info.mask_grid : 0;

    for (int s = 0; s < subs_x * subs_z; s++) {
      const int sub_row = s / subs_x;
      const int sub_col = s % subs_x;
      const std::string &name = info.sub_terrain_names[size_t(s)];

      const std::string bk_path = find_file_case_insensitive(map_dir, name + ".t2bk");
      if (bk_path.empty()) {
        return fail(r_error, "Block file \"" + name + ".t2bk\" not found in \"" + map_dir + "\"");
      }
      T2bkFile blocks;
      if (!blocks.load(bk_path, info.block_grid, r_error)) {
        return false;
      }
      if (blocks.num_blocks() < sub_blocks * sub_blocks) {
        return fail(r_error, "\"" + bk_path + "\" has fewer blocks than its sub-terrain");
      }
      for (int b = 0; b < sub_blocks * sub_blocks; b++) {
        if (!blocks.read_block(b, block, &err)) {
          return fail(r_error, "\"" + bk_path + "\": " + err);
        }
        builder.put_block(
            sub_row * sub_blocks + b / sub_blocks, sub_col * sub_blocks + b % sub_blocks, block);
      }

      if (!sub_masks) {
        continue;
      }
      auto global_area = [&](const int local) {
        const int r = sub_row * sub_mask_areas + local / sub_mask_areas;
        const int c = sub_col * sub_mask_areas + local % sub_mask_areas;
        return r * field.mask_areas_x + c;
      };
      const int num_local = sub_mask_areas * sub_mask_areas;

      const std::string mk_path = find_file_case_insensitive(map_dir, name + ".t2mk");
      T2mkFile masks;
      if (mk_path.empty()) {
        field.warnings.push_back("\"" + name + ".t2mk\" not found");
      }
      else if (!masks.load(mk_path, &err)) {
        field.warnings.push_back(err);
      }
      else {
        const int n = std::min(num_local, int(masks.mask_areas.size()));
        for (int a = 0; a < n; a++) {
          builder.put_masks(masks.mask_areas[size_t(a)], global_area(a), s);
        }
      }

      const std::string lm_path = find_file_case_insensitive(map_dir, name + ".t2lm");
      if (!lm_path.empty()) {
        T2lmFile lm;
        if (!lm.load(lm_path, &err)) {
          field.warnings.push_back(err);
        }
        else {
          const int n = std::min(num_local, lm.num_mask_areas());
          for (int a = 0; a < n; a++) {
            builder.put_lightmap(lm, a, global_area(a));
          }
        }
      }
    }
  }

  if (!builder.have_normals) {
    field.normals.clear();
  }
  return true;
}

/** \} */

}  // namespace blender::io::angelica
