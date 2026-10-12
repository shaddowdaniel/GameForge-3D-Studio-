/* SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup angelica
 *
 * Readers for the Angelica `A3DTerrain2` client terrain:
 *
 * - `<code>.trn2`: terrain descriptor (dimensions, sub-terrain height ranges, texture list).
 * - `<name>.t2bk`: terrain blocks (heights, normals, vertex colors).
 * - `<name>.t2mk`: texture layers and their 8-bit splat masks per mask area.
 * - `<name>.t2lm` / `.t2lm1`: day / night lightmaps per mask area (DDS or raw RGB24).
 *
 * Two loaders exist in the engine. LOADER_A keeps the blocks inside the `.trn2` itself next to a
 * single `<code>.t2mk` / `<code>.t2lm`. LOADER_B (used by all retail maps) splits the terrain
 * into sub-terrains with their own `<sub>.t2bk/.t2mk/.t2lm` files, the sub-terrain names being
 * listed in the `.trn2`.
 *
 * Coordinates are engine space: X east, Y up, Z north. The terrain is centered on the origin; the
 * vertex at row `i` / column `j` of the whole terrain grid lies at
 * `x = -width / 2 + j * grid_size`, `z = +height / 2 - i * grid_size` (row 0 is the north edge).
 *
 * See `angelica_terrain.cc` for byte layouts.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "angelica_reader.hh"

namespace blender::io::angelica {

/* -------------------------------------------------------------------- */
/** \name `.trn2` Descriptor
 * \{ */

enum class Trn2LoaderId : int32_t {
  A = 0,
  B = 1,
};

struct Trn2SubTerrain {
  float min_height = 0.0f;
  float max_height = 0.0f;
};

struct Trn2Info {
  uint32_t version = 0;
  /** Terrain extent in meters along X (`width`) and Z (`height`). */
  float terrain_width = 0.0f;
  float terrain_height = 0.0f;
  float block_size = 0.0f;
  /** Grids per block side; a block has `(block_grid + 1)^2` vertices. */
  int block_grid = 0;
  int num_blocks = 0;
  int block_rows = 0;
  int block_cols = 0;
  int num_textures = 0;
  int num_mask_areas = 0;
  /** Blocks per mask area side. */
  int mask_blocks = 0;
  /** Grids per mask area side. */
  int mask_grid = 0;
  int num_sub_terrains = 0;
  int sub_terrain_rows = 0;
  int sub_terrain_cols = 0;
  /** Grids per sub-terrain side. */
  int sub_terrain_grid = 0;
  float grid_size = 0.0f;
  uint32_t texture_offset = 0;
  int32_t loader_id = 0;
  uint32_t loader_offset = 0;

  /** `sub_terrain_rows * sub_terrain_cols` height ranges. */
  std::vector<Trn2SubTerrain> sub_terrains;
  /** Texture file paths (UTF-8, as stored in the file, usually relative to the client root). */
  std::vector<std::string> texture_paths;

  /** LOADER_B: size in pixels of every non-base mask map (32 when the file stores 0). */
  int mask_map_size = 0;
  /** LOADER_B: base file names of the sub-terrains (UTF-8, no extension), row-major. */
  std::vector<std::string> sub_terrain_names;

  /** LOADER_A: absolute offsets of the block records inside the `.trn2`. */
  std::vector<uint32_t> block_offsets;

  int vertices_per_block_side() const
  {
    return block_grid + 1;
  }
  int blocks_per_sub_terrain_side() const
  {
    return block_grid > 0 ? sub_terrain_grid / block_grid : 0;
  }
  int mask_areas_per_row() const
  {
    return mask_grid > 0 ? block_cols * block_grid / mask_grid : 0;
  }
};

bool parse_trn2(ByteSpan data, Trn2Info &r_info, std::string *r_error);

/** \} */

/* -------------------------------------------------------------------- */
/** \name `.t2bk` Blocks
 * \{ */

/** Compression flags of block files (`T2BKCOMP_*`). */
enum : uint8_t {
  T2BK_COL_ZIP = 0x01,
  T2BK_HEI_ZIP = 0x04,
  T2BK_HEI_INC = 0x08,
  T2BK_NOR_ZIP = 0x10,
  T2BK_NOR_DEG = 0x20,
};

/** Per block flags (`T2BKFLAG_*`, block file version 6). */
enum : uint32_t {
  T2BK_FLAG_NORENDER = 0x01,
  T2BK_FLAG_NOTRACE = 0x02,
  T2BK_FLAG_NOPOSHEIGHT = 0x04,
  T2BK_FLAG_NORENDERWITHWATER = 0x08,
};

struct TerrainBlock {
  /** Global index of the mask area the block belongs to. */
  int mask_area = 0;
  int row_in_terrain = 0;
  int col_in_terrain = 0;
  float lod_scale = 1.0f;
  /** Bit `n` set when texture layer `n` of the mask area affects this block. */
  uint32_t layer_flags = 0;
  uint32_t block_flags = 0;
  /** `(block_grid + 1)^2` absolute heights in meters, row-major, row 0 north. */
  std::vector<float> heights;
  /** 3 floats (x, y, z) per vertex, or empty when the file has none. */
  std::vector<float> normals;
  /** Day / night vertex colors (`0xAARRGGBB`), empty when not stored or not decodable. */
  std::vector<uint32_t> diffuse;
  std::vector<uint32_t> diffuse_night;
};

class T2bkFile {
 public:
  /** Parse a LOADER_B `.t2bk` file. `block_grid` comes from the `.trn2`. */
  bool parse(Bytes data, int block_grid, std::string *r_error);
  /** Use the blocks stored inside a LOADER_A `.trn2` (`info` must come from the same data). */
  bool parse_loader_a(Bytes trn2_data, const Trn2Info &info, std::string *r_error);
  bool load(const std::string &filepath, int block_grid, std::string *r_error);

  /** Decode block `index` (index inside this file). */
  bool read_block(int index, TerrainBlock &r_block, std::string *r_error) const;

  uint32_t version() const
  {
    return version_;
  }
  uint8_t compression() const
  {
    return compression_;
  }
  int num_blocks() const
  {
    return int(offsets_.size());
  }
  const std::vector<uint32_t> &block_offsets() const
  {
    return offsets_;
  }

 private:
  Bytes data_;
  uint32_t version_ = 0;
  uint8_t compression_ = 0;
  int block_grid_ = 0;
  std::vector<uint32_t> offsets_;
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name `.t2mk` Texture Layers and Masks
 * \{ */

struct T2mkLayer {
  /** Index into #Trn2Info::texture_paths. */
  int texture_index = -1;
  int specular_index = -1;
  int mask_map_size = 0;
  /** 0: project on YZ, 1: project on XZ (top-down, the usual case), 2: project on XY. */
  uint8_t proj_axis = 1;
  float u_scale = 1.0f;
  float v_scale = 1.0f;
  uint8_t weight = 0;
};

struct T2mkAreaLayer {
  T2mkLayer layer;
  /** Index into #T2mkFile::layers (LOADER_B) or into the file's lumps (LOADER_A). */
  int layer_index = 0;
  /** Width and height of #mask. */
  int mask_size = 0;
  /**
   * 8-bit mask, `mask_size^2` bytes, row-major starting at the north-west corner of the mask
   * area. Empty for the first (base) layer, which is implicitly fully opaque, and when the mask
   * could not be decoded (see #T2mkMaskArea::mask_textures).
   */
  Bytes mask;
};

struct T2mkMaskArea {
  std::vector<T2mkAreaLayer> layers;
  /**
   * Mask file version 3 only: the masks are stored as ready-made textures, one per three layers
   * (layer `3t + k` in channel B, G, R for k = 0, 1, 2). The raw texture files (usually DDS) are
   * kept here; uncompressed 32-bit DDS textures are also unpacked into #T2mkAreaLayer::mask.
   */
  std::vector<Bytes> mask_textures;
};

class T2mkFile {
 public:
  uint32_t version = 0;
  bool compressed = false;
  /** LOADER_B: the sub-terrain's layer table. Empty for LOADER_A files. */
  std::vector<T2mkLayer> layers;
  std::vector<T2mkMaskArea> mask_areas;

  /** Parse a LOADER_B (`TMKB`) or LOADER_A (`T2MK`) mask file and decode all masks. */
  bool parse(ByteSpan data, std::string *r_error);
  bool load(const std::string &filepath, std::string *r_error);
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name `.t2lm` Lightmaps
 * \{ */

class T2lmFile {
 public:
  static constexpr uint32_t FLAG_DDS = 0x0001;

  bool parse(Bytes data, std::string *r_error);
  bool load(const std::string &filepath, std::string *r_error);

  uint32_t version() const
  {
    return version_;
  }
  int num_mask_areas() const
  {
    return int(offsets_.size());
  }
  /** Lightmap width and height in pixels. */
  int lightmap_size() const
  {
    return lightmap_size_;
  }
  uint32_t flags() const
  {
    return flags_;
  }
  /** True when each area blob is a complete DDS file, otherwise it is raw RGB24 (B, G, R). */
  bool is_dds() const
  {
    return (flags_ & FLAG_DDS) != 0;
  }
  int data_size() const
  {
    return data_size_;
  }
  /** Lightmap data of a mask area (local index in this file). Empty when out of range. */
  ByteSpan area_data(int index) const;

 private:
  Bytes data_;
  uint32_t version_ = 0;
  int lightmap_size_ = 0;
  uint32_t flags_ = 0;
  int data_size_ = 0;
  std::vector<uint32_t> offsets_;
};

/** \} */

/* -------------------------------------------------------------------- */
/** \name Whole Terrain
 * \{ */

struct TerrainMaskLayer {
  /** Index into #TerrainHeightfield::texture_paths, -1 when invalid. */
  int texture_index = -1;
  uint8_t proj_axis = 1;
  float u_scale = 1.0f;
  float v_scale = 1.0f;
  uint8_t weight = 0;
  int mask_size = 0;
  /** `mask_size^2` bytes; empty means fully opaque (the base layer). */
  Bytes mask;
};

struct TerrainMaskArea {
  /** Position in the mask area grid (row 0 north). */
  int row = 0;
  int col = 0;
  int sub_terrain = 0;
  std::vector<TerrainMaskLayer> layers;
  /** Day lightmap: a DDS file when #lightmap_is_dds, else raw 24-bit pixels; may be empty. */
  Bytes lightmap;
  bool lightmap_is_dds = false;
  int lightmap_size = 0;
};

struct TerrainHeightfield {
  float grid_size = 0.0f;
  float terrain_width = 0.0f;
  float terrain_height = 0.0f;
  /** World position of vertex (0, 0): the north-west corner. */
  float origin_x = 0.0f;
  float origin_z = 0.0f;
  /** Vertex counts: `block_cols * block_grid + 1` by `block_rows * block_grid + 1`. */
  int verts_x = 0;
  int verts_z = 0;
  /** `verts_x * verts_z` heights, row-major, row 0 north. Vertex (i, j) lies at
   * `x = origin_x + j * grid_size`, `z = origin_z - i * grid_size`. */
  std::vector<float> heights;
  /** 3 floats per vertex (engine space), empty when any block had no normals. */
  std::vector<float> normals;

  std::vector<std::string> texture_paths;
  std::vector<std::string> sub_terrain_names;
  std::vector<Trn2SubTerrain> sub_terrains;

  /** Grids per mask area side and the mask area grid dimensions. */
  int mask_grid = 0;
  int mask_areas_x = 0;
  int mask_areas_z = 0;
  /** Global mask area index order (`row * mask_areas_x + col`). */
  std::vector<TerrainMaskArea> mask_areas;

  /** Non fatal problems (missing mask or lightmap files, undecodable masks). */
  std::vector<std::string> warnings;

  float height_at(const int row, const int col) const
  {
    return heights[size_t(row) * size_t(verts_x) + size_t(col)];
  }
};

/**
 * Load a whole client terrain: `<map_dir>/<map_code>.trn2` and its sub-terrain files. File names
 * are matched case-insensitively. Missing `.t2mk` / `.t2lm` files only produce warnings; missing
 * or invalid block data is an error.
 */
bool load_terrain_heightfield(const std::string &map_dir,
                              const std::string &map_code,
                              TerrainHeightfield &r_field,
                              std::string *r_error);

/**
 * Find `name` inside `dir`, first exactly, then ignoring ASCII case. Returns the full path or an
 * empty string.
 */
std::string find_file_case_insensitive(const std::string &dir, const std::string &name);

/** \} */

}  // namespace blender::io::angelica
