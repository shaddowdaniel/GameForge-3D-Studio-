/* SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup angelica
 *
 * Parsers for static (not skinned) Angelica Engine models:
 * - `.bmd` buildings: `CELBrushBuilding` (element client) wrapping an `A3DLitModel` with baked
 *   vertex lighting, followed by collision data (not decoded).
 * - `.mox` frames: `A3DFrame` hierarchies of `A3DMesh` (used for grass, trees and old props).
 *
 * See `angelica_skin.hh` for coordinate, matrix, UV and string conventions.
 */

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "angelica_reader.hh"
#include "angelica_skin.hh"

namespace blender::io::angelica {

/* -------------------------------------------------------------------- */
/** \name Lit Model (.bmd)
 * \{ */

/** `A3DLitModel` file versions. */
enum : uint32_t {
  LIT_MODEL_VERSION_1 = 0x10000001,
  LIT_MODEL_VERSION_2 = 0x10000002,
  LIT_MODEL_VERSION_LIGHTMAP_OLD = 0x10000100,
  LIT_MODEL_VERSION_LIGHTMAP = 0x10000101,
  LIT_MODEL_VERSION_SHARED = 0x10000200,
  LIT_MODEL_VERSION_EX = 0x10000300,
};

/** `CELBrushBuilding` version tag in front of the lit model (followed by `bool collide only`). */
constexpr uint32_t BRUSH_BUILDING_VERSION = 0x80000001;

/** `A3DLitModelSharedData` ("LMSD") files referenced by the shared lit model versions. */
constexpr uint32_t LIT_SHARED_DATA_MAGIC = fourcc('D', 'S', 'M', 'L');

struct LitMesh {
  /** `A3DLitMesh` version (0x10000002..0x10000006, 0x10000100..0x10000102), or the model version
   * for meshes stored inline in shared / extended models. */
  uint32_t version = 0;
  std::string name;
  /** Texture path as stored (engine relative, from the client root). */
  std::string texture;
  std::string texture_raw;

  /** 3 floats per vertex, in model space (see #LitModel::world_matrix). */
  std::vector<float> positions;
  std::vector<float> normals;
  /** 2 floats per vertex. */
  std::vector<float> uvs;
  /** 2 floats per vertex when the mesh has light map coordinates. */
  std::vector<float> lightmap_uvs;
  bool use_lightmap = false;
  /** Vertex colors as `A3DCOLOR` (0xAARRGGBB): baked day / night lighting. */
  std::vector<uint32_t> day_colors;
  std::vector<uint32_t> night_colors;
  /** Second lighting layer of version 0x10000006 meshes. */
  std::vector<uint32_t> day_colors_extra;
  std::vector<uint32_t> night_colors_extra;
  /** Triangle list. */
  std::vector<uint16_t> indices;

  float aabb_min[3] = {0.0f, 0.0f, 0.0f};
  float aabb_max[3] = {0.0f, 0.0f, 0.0f};

  bool has_material = false;
  MaterialInfo material;

  /* Extended version only. */
  float detail_tile = 0.0f;
  float detail_hardness = 0.0f;
  std::string detail_texture;
  std::string detail_texture_raw;

  int vertex_count() const
  {
    return int(positions.size() / 3);
  }
};

struct LitModel {
  /** #BRUSH_BUILDING_VERSION when the building wrapper header is present, otherwise 0. */
  uint32_t building_version = 0;
  bool collide_only = false;
  uint32_t version = 0;
  /** Placement stored in the file. */
  float scale[3] = {1.0f, 1.0f, 1.0f};
  float dir[3] = {0.0f, 0.0f, 1.0f};
  float up[3] = {0.0f, 1.0f, 0.0f};
  float pos[3] = {0.0f, 0.0f, 0.0f};
  /**
   * `Scaling(scale) * TransformMatrix(dir, up, pos)`, the transform the engine bakes into the
   * vertices on load (`A3DLitModel::UpdateAbsoluteTM` + `NormalizeTransforms`). The vertices in
   * #LitModel::meshes are *not* transformed. Note the engine does not re-orthogonalize `up`.
   */
  float world_matrix[16];
  std::vector<LitMesh> meshes;
  std::string lightmap;
  std::string lightmap_raw;
  std::string night_lightmap;
  std::string night_lightmap_raw;
  /** Shared versions: the "LMSD" file holding the geometry (engine relative). */
  std::string shared_data_file;
  std::string shared_data_file_raw;
  /** True when building collision data (convex hulls, brushes) follows the lit model. */
  bool has_collision_data = false;
};

/**
 * Loads an engine relative file (raw GBK path) for the shared lit model versions. Returns false
 * when the file cannot be found.
 */
using LitSharedDataLoader = std::function<bool(const std::string &path_raw, Bytes &r_data)>;

/**
 * Parse a `.bmd` building (or a bare `A3DLitModel`). Shared data versions need
 * `shared_data_loader`, without it they fail with an error naming the missing file.
 */
bool parse_bmd(ByteSpan data,
               LitModel &r_model,
               std::string *r_error,
               const LitSharedDataLoader &shared_data_loader = {});

/** \} */

/* -------------------------------------------------------------------- */
/** \name Frame Model (.mox)
 * \{ */

/** `A3DFRAMEOBB`: collision box of a frame. */
struct MoxOBB {
  std::string name;
  float center[3];
  float axis_x[3];
  float axis_y[3];
  float axis_z[3];
  float extents[3];
  /** `A3DFRAMEOBB` flags (shoot through 1, no mark 2, pick through 4, cannot destroy 8). */
  uint32_t props = 0;
  int material_type = 0;
};

struct MoxMesh {
  /** Mesh format version (1, 2 or 100 = light mapped). */
  int version = 1;
  std::string name;
  /**
   * Texture file name as stored; the engine loads `<mox dir>\Textures\<texture>`. Same for the
   * detail texture.
   */
  std::string texture;
  std::string texture_raw;
  std::string detail_texture;
  std::string detail_texture_raw;
  float detail_matrix[16];

  /** Number of vertex animation frames. */
  int frame_count = 0;
  /** Frame 0: 3 / 3 / 2 floats per vertex. */
  std::vector<float> positions;
  std::vector<float> normals;
  std::vector<float> uvs;
  /** Frames 1..n-1 (vertex animation), positions only, 3 floats per vertex each. */
  std::vector<std::vector<float>> animated_positions;
  /** Version 100: light map coordinates of frame 0. */
  std::vector<float> lightmap_uvs;
  std::vector<uint16_t> indices;
  /** True when #indices is a line list (`m_bWire`). */
  bool wire = false;
  /** `A3DMESHPROP` flags (2 sided 1, shoot through 2, no mark 4, pick through 8, on ground 16,
   * BSP PVS 32, not render 64, ...) and material type. */
  uint32_t props = 0;
  int material_type = 0;
  MaterialInfo material;

  int vertex_count() const
  {
    return int(positions.size() / 3);
  }
};

struct MoxFrame {
  /** Frame format version (1 or 2, changes the bounding box layout). */
  int version = 1;
  std::string name;
  /** Transform to the parent frame for each animation frame (at least one). */
  std::vector<std::array<float, 16>> relative_tms;
  std::vector<MoxOBB> obbs;
  std::vector<MoxMesh> meshes;
  std::vector<MoxFrame> children;
};

/** Parse a binary (`MOXB`) `.mox` file. Text `.mox` files are rejected. */
bool parse_mox(ByteSpan data, MoxFrame &r_root, std::string *r_error);

/** \} */

}  // namespace blender::io::angelica
