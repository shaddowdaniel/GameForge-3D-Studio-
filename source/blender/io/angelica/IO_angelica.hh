/* SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup angelica
 *
 * Native import of Angelica Engine (Perfect World) assets.
 *
 * Supported inputs:
 * - Packages: `.pck` (+ `.pkx`, `.pkx1`, ...). Every supported asset inside is imported in batch,
 *   optionally narrowed with a filter pattern.
 * - Models: `.ecm`, `.smd`, `.ski`, `.bon` (with `.stck` animations), `.bmd`, `.mox`.
 * - Terrain: `.trn2` (with `.t2bk` / `.t2mk` / `.t2lm` sub-terrain files) and server `.hmap`.
 * - Maps: a map folder named after its code (`a19`, `world`, ...), importing terrain and
 *   heightmaps in one step.
 *
 * Every imported top-level data-block is reported back as an #ImportedAsset so the caller can
 * mark it as an asset and place it in a catalog that mirrors the engine path.
 */

#pragma once

#include <string>
#include <vector>

#include "BLI_path_utils.hh"

namespace blender {

struct bContext;
struct ID;
struct Main;
struct ReportList;
struct Scene;
struct ViewLayer;

struct AngelicaImportParams {
  /** File to import (`.pck`, model, terrain or heightmap file). */
  char filepath[FILE_MAX] = "";
  /**
   * Client folder (usually `element`) used to resolve textures, skeletons and other referenced
   * files. Detected from #filepath when empty.
   */
  char client_root[FILE_MAX] = "";
  /** Only import package entries whose path matches this pattern (`*` and `?`), empty = all. */
  char filter[1024] = "";
  /** Stop after this many assets from one package, zero for no limit. */
  int max_assets = 0;

  float global_scale = 1.0f;
  bool import_textures = true;
  bool import_armature = true;
  bool import_animations = true;
  /** Place the imported objects in the current scene (they are always kept in the file). */
  bool link_to_scene = true;

  /** Terrain: use every n-th vertex (1 = full resolution, as in the client). */
  int terrain_step = 1;

  ReportList *reports = nullptr;
};

struct AngelicaMapImportParams {
  /** Map folder, for example `element/maps/a19`. Its name is the map code. */
  char directory[FILE_MAX] = "";
  char client_root[FILE_MAX] = "";
  float global_scale = 1.0f;
  bool import_terrain = true;
  bool import_heightmap = true;
  bool import_textures = true;
  bool link_to_scene = true;
  int terrain_step = 1;

  ReportList *reports = nullptr;
};

namespace io::angelica {

/** A top level data-block created by the importer, with where it belongs in the asset library. */
struct ImportedAsset {
  ID *id = nullptr;
  /** Catalog path using `/` separators, for example `Angelica/models/npc/monster`. */
  std::string catalog_path;
  /** Source of the asset (package and path inside it), stored as the asset description. */
  std::string description;
  std::vector<std::string> tags;
};

/** File extensions handled by #import_file, including the dot. */
const std::vector<std::string> &supported_extensions();

/**
 * Import one file. Imported data is added to `bmain`; objects are linked to the active
 * collection of `view_layer` when `params.link_to_scene` is set.
 */
std::vector<ImportedAsset> import_file(Main *bmain,
                                       Scene *scene,
                                       ViewLayer *view_layer,
                                       const AngelicaImportParams &params);

/** Import a map folder (terrain, heightmaps) by its map code. */
std::vector<ImportedAsset> import_map(Main *bmain,
                                      Scene *scene,
                                      ViewLayer *view_layer,
                                      const AngelicaMapImportParams &params);

}  // namespace io::angelica
}  // namespace blender
