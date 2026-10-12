/* SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup angelica
 */

#pragma once

#include <string>

#include "angelica_import_common.hh"

namespace blender::io::angelica {

/**
 * Import the client terrain of a map (`<map_dir>/<code>.trn2` and its sub-terrain files) as one
 * mesh object per mask area, with splat materials (layer textures blended by their masks) and
 * the day lightmap. The objects are grouped in a collection that becomes the asset.
 * Returns false when there is no terrain.
 */
bool import_terrain(ImportContext &ctx,
                    const std::string &map_dir,
                    const std::string &map_code,
                    int step);

/**
 * Import server heightmaps (`1.hmap` .. `N.hmap`) of a map folder as one mesh.
 * `trn2_dir` is checked for a `.trn2` to take the height range from when the folder has no
 * configuration. Returns false when the folder has no heightmaps.
 */
bool import_hmap_folder(ImportContext &ctx,
                        const std::string &hmap_dir,
                        const std::string &map_code,
                        const std::string &trn2_dir,
                        int step);

}  // namespace blender::io::angelica
