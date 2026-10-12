/* SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup angelica
 *
 * Reader for the Perfect World server height maps (`.hmap`).
 *
 * A `.hmap` file is a square `n * n` grid of little-endian float32 values normalized to 0..1
 * (`n` is the area width in cells + 1, usually 513, 65 for some small instance maps), row-major,
 * row 0 at the north edge. A map is a `cols * rows` grid of such areas stored as
 * `1.hmap` .. `(rows * cols).hmap`, file `row * cols + col + 1` holding area (row, col).
 * Neighboring areas share their edge row / column. The map is centered on the origin and the
 * real height is `value * (height_max - height_min) + height_min`
 * (see the server's `CTerrain::Init` and `[Terrain_*]` sections of `gs.conf`).
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "angelica_reader.hh"

namespace blender::io::angelica {

/** One `.hmap` file. */
struct HeightMap {
  /** Vertices per side. */
  int size = 0;
  /** `size * size` normalized values, row-major, row 0 north. */
  std::vector<float> values;
};

bool parse_hmap(ByteSpan data, HeightMap &r_map, std::string *r_error);

/** A whole map stitched from its area files. */
struct HmapMap {
  int cols = 0;
  int rows = 0;
  /** Cells per area side (`HeightMap::size - 1`). */
  int area_size = 0;
  float grid_size = 2.0f;
  /** Height range the normalized values map to (`vHeightMin` / `vHeightMax`). */
  float height_min = 0.0f;
  float height_max = 800.0f;
  /** `cols * area_size + 1` by `rows * area_size + 1`. */
  int verts_x = 0;
  int verts_z = 0;
  /** `verts_x * verts_z` normalized values, row-major, row 0 north. */
  std::vector<float> values;
  /** Files the layout and height range were read from, empty when they were inferred. */
  std::string layout_source;
  std::string height_range_source;

  /** World X of column 0 (west edge). */
  float origin_x() const
  {
    return -float(cols * area_size) * grid_size * 0.5f;
  }
  /** World Z of row 0 (north edge). */
  float origin_z() const
  {
    return float(rows * area_size) * grid_size * 0.5f;
  }
  /** Height in meters of vertex (row, col). */
  float height(const int row, const int col) const
  {
    return values[size_t(row) * size_t(verts_x) + size_t(col)] * (height_max - height_min) +
           height_min;
  }
};

/**
 * Optional overrides for #load_hmap_folder. Zero / negative values mean "detect".
 */
struct HmapLoadOptions {
  int cols = 0;
  int rows = 0;
  float grid_size = 0.0f;
  bool has_height_range = false;
  float height_min = 0.0f;
  float height_max = 800.0f;
};

/**
 * Load all `<n>.hmap` files of a folder and stitch them into one grid.
 *
 * The area layout comes from a `movemap.conf` (`Map Width` / `Map Length` / `Submap Width`)
 * found in the folder, its parent or a sibling `movemap/` folder, or from a server terrain
 * config (`nNumCols` / `nNumRows` / `vGridSize` / `vHeightMin` / `vHeightMax`, as in a
 * `[Terrain_*]` section of `gs.conf`) in the folder or its parent. Without either, the number
 * of files must be a perfect square and a square layout is assumed.
 */
bool load_hmap_folder(const std::string &dir,
                      HmapMap &r_map,
                      std::string *r_error,
                      const HmapLoadOptions &options = {});

}  // namespace blender::io::angelica
