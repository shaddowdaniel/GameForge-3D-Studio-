/* SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup angelica
 *
 * Shared state and helpers for turning parsed Angelica data into Blender data.
 *
 * Coordinate system: the engine is Direct3D style, left-handed with +Y up, +X east and +Z north,
 * matrices are row-major and multiply row vectors (`v' = v * M`, translation in the last row).
 * Blender is right-handed with +Z up. Positions are converted with `(x, y, z) -> (x, z, y)`,
 * which keeps east on +X and north on +Y. The swap is a reflection, so it also turns the engine's
 * clockwise front faces into Blender's counter-clockwise ones: triangle order is kept as is.
 */

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "BLI_map.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector_types.hh"

#include "IO_angelica.hh"

#include "angelica_vfs.hh"

namespace blender {

struct Collection;
struct Image;
struct Material;
struct Object;

namespace io::angelica {

inline float3 to_blender(const float x, const float y, const float z, const float scale = 1.0f)
{
  return float3(x, z, y) * scale;
}

inline float3 to_blender(const float v[3], const float scale = 1.0f)
{
  return to_blender(v[0], v[1], v[2], scale);
}

/** Convert an engine row-major, row-vector 4x4 transform into a Blender matrix. */
float4x4 engine_matrix_to_blender(const float m[16], float scale = 1.0f);

/** Make a valid UTF-8 data-block name from an engine file name (no directory, no extension). */
std::string name_from_engine_path(std::string_view engine_path);

/** Catalog path for an engine path: `Angelica/<folders>` using `/`, without the file name. */
std::string catalog_from_engine_path(std::string_view engine_path, std::string_view root = "");

struct TextureRequest {
  /** Engine path of the texture (absolute in the client, or relative to `base_dir`). */
  std::string path;
  /** Engine directory of the file that references the texture. */
  std::string base_dir;
};

class ImportContext {
 public:
  ImportContext(Main *bmain,
                Scene *scene,
                ViewLayer *view_layer,
                ReportList *reports,
                std::string client_root,
                float global_scale,
                bool import_textures,
                bool link_to_scene);
  ~ImportContext();

  Main *bmain;
  Scene *scene;
  ViewLayer *view_layer;
  ReportList *reports;
  float scale;
  bool import_textures;
  bool link_to_scene;

  ClientFileSystem vfs;
  std::vector<ImportedAsset> assets;

  /** Report a warning once per unique message. */
  void warn(const std::string &message);
  void error(const std::string &message);

  /**
   * Create an object for `data`, linked to the target collection when objects are placed in the
   * scene (otherwise it only lives in the file, for example as an asset).
   */
  Object *add_object(const std::string &name, short type, ID *data);

  /** Collection that receives objects: the active one, or a child collection per import. */
  Collection *target_collection();
  void set_target_collection(Collection *collection);

  /**
   * Load a texture through the virtual file system, packed into the blend file so assets keep
   * working outside the client folder. Cached per resolved path. Returns null when not found.
   */
  Image *load_image(const TextureRequest &request);

  /** Create an image from an encoded file in memory (DDS, TGA, ...), packed into the file. */
  Image *image_from_memory(const std::string &name,
                           const std::string &virtual_path,
                           const uint8_t *data,
                           size_t size);

  /**
   * Create a packed image from 8-bit RGBA pixels, row 0 at the top (engine order).
   * `non_color` marks data images such as splat masks.
   */
  Image *image_from_pixels(
      const std::string &name, int width, int height, const uint8_t *rgba, bool non_color);

  /**
   * Material with an image texture on the base color, cached per texture and settings.
   * `alpha_mode`: 0 = opaque, 1 = alpha test (clip), 2 = alpha blend.
   */
  Material *material_for_texture(const std::string &name,
                                 Image *image,
                                 const float diffuse[4],
                                 int alpha_mode,
                                 bool two_sided);

  void add_asset(ID *id,
                 std::string catalog_path,
                 std::string description,
                 std::vector<std::string> tags = {});

 private:
  Collection *collection_ = nullptr;
  Map<std::string, Image *> image_cache_;
  Map<std::string, Material *> material_cache_;
  Map<std::string, bool> reported_;
};

}  // namespace io::angelica
}  // namespace blender
