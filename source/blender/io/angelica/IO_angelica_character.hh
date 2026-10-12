/* SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup angelica
 *
 * Import an assembled player character (body, equipment, weapons, wings, face) from the
 * character description the GameForge account services return for a saved character
 * (PWOps `GET /api/studio/appearance`), reading every model from the local game client.
 */

#pragma once

#include <string>
#include <vector>

#include "BLI_path_utils.hh"

#include "IO_angelica.hh"

namespace blender {

struct AngelicaCharacterImportParams {
  /** Character description (JSON), see #io::angelica::character_spec_from_json. */
  std::string spec_json;
  /** Client folder (usually `element`) holding the `.pck` packages. */
  char client_root[FILE_MAX] = "";
  /** Name of the character collection, empty to name it after the body model. */
  std::string name;
  float global_scale = 1.0f;
  bool import_textures = true;
  bool import_animations = true;
  bool link_to_scene = true;

  ReportList *reports = nullptr;
};

namespace io::angelica {

/** Import the character. Returns the character collection as the only asset, empty on failure. */
std::vector<ImportedAsset> import_character_json(Main *bmain,
                                                 Scene *scene,
                                                 ViewLayer *view_layer,
                                                 const AngelicaCharacterImportParams &params);

}  // namespace io::angelica
}  // namespace blender
