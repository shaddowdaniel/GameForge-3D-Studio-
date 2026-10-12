/* SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup angelica
 */

#include "BKE_report.hh"

#include "DEG_depsgraph_build.hh"

#include "IO_angelica_character.hh"

#include "angelica_import_character.hh"
#include "angelica_import_common.hh"

namespace blender::io::angelica {

std::vector<ImportedAsset> import_character_json(Main *bmain,
                                                 Scene *scene,
                                                 ViewLayer *view_layer,
                                                 const AngelicaCharacterImportParams &params)
{
  CharacterSpec spec;
  std::string error;
  if (!character_spec_from_json(params.spec_json, spec, &error)) {
    BKE_reportf(params.reports, RPT_ERROR, "Angelica: %s", error.c_str());
    return {};
  }
  if (!params.name.empty()) {
    spec.name = params.name;
  }
  if (params.client_root[0] == '\0') {
    BKE_report(params.reports,
               RPT_ERROR,
               "Angelica: choose the game client folder (element) to load the character models");
    return {};
  }

  ImportContext ctx(bmain,
                    scene,
                    view_layer,
                    params.reports,
                    params.client_root,
                    params.global_scale,
                    params.import_textures,
                    params.link_to_scene);
  ModelOptions options;
  options.import_armature = true;
  options.import_animations = params.import_animations;

  Collection *parent = params.link_to_scene ? ctx.target_collection() : nullptr;
  if (import_character(ctx, spec, options, parent) == nullptr) {
    BKE_reportf(params.reports,
                RPT_ERROR,
                "Angelica: cannot load the character body '%s' from the client folder",
                spec.smd_path.c_str());
  }
  DEG_relations_tag_update(bmain);
  return std::move(ctx.assets);
}

}  // namespace blender::io::angelica
