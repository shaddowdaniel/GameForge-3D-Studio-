/* SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup editor/io
 *
 * Character preview: assemble a saved character of the user's account (the description the
 * GameForge account services return, see `IO_angelica_character.hh`) from the local client.
 * The GameForge sidebar panel fetches the description and calls this operator.
 */

#ifdef WITH_IO_ANGELICA

#  include <string>
#  include <vector>

#  include "BKE_context.hh"
#  include "BKE_report.hh"

#  include "BLI_fileops.hh"

#  include "DEG_depsgraph.hh"
#  include "DEG_depsgraph_build.hh"

#  include "DNA_ID.h"
#  include "DNA_scene_types.h"

#  include "ED_asset_mark_clear.hh"
#  include "ED_outliner.hh"

#  include "RNA_access.hh"
#  include "RNA_define.hh"

#  include "WM_api.hh"
#  include "WM_types.hh"

#  include "IO_angelica_character.hh"
#  include "io_angelica_character_ops.hh"

namespace blender {

static wmOperatorStatus wm_angelica_character_import_exec(bContext *C, wmOperator *op)
{
  AngelicaCharacterImportParams params;
  params.spec_json = RNA_string_get(op->ptr, "spec_json");
  RNA_string_get(op->ptr, "client_root", params.client_root);
  params.name = RNA_string_get(op->ptr, "name");
  params.global_scale = RNA_float_get(op->ptr, "global_scale");
  params.import_textures = RNA_boolean_get(op->ptr, "import_textures");
  params.import_animations = RNA_boolean_get(op->ptr, "import_animations");
  params.link_to_scene = true;
  params.reports = op->reports;

  if (params.spec_json.empty()) {
    BKE_report(op->reports, RPT_ERROR, "No character to import");
    return OPERATOR_CANCELLED;
  }
  if (params.client_root[0] == '\0' || !BLI_is_dir(params.client_root)) {
    BKE_report(op->reports, RPT_ERROR, "Choose the game client folder (element)");
    return OPERATOR_CANCELLED;
  }

  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  const std::vector<io::angelica::ImportedAsset> assets = io::angelica::import_character_json(
      bmain, scene, CTX_data_view_layer(C), params);
  if (assets.empty()) {
    return OPERATOR_CANCELLED;
  }

  /* A preview stays out of the Asset Browser unless asked for. */
  if (RNA_boolean_get(op->ptr, "mark_as_asset")) {
    for (const io::angelica::ImportedAsset &asset : assets) {
      if (asset.id != nullptr) {
        ed::asset::mark_id(asset.id);
      }
    }
    WM_main_add_notifier(NC_ASSET | ND_ASSET_LIST | NA_ADDED, nullptr);
  }

  DEG_id_tag_update(&scene->id, ID_RECALC_BASE_FLAGS);
  DEG_relations_tag_update(bmain);
  WM_event_add_notifier(C, NC_SCENE | ND_OB_SELECT, scene);
  WM_event_add_notifier(C, NC_SCENE | ND_OB_ACTIVE, scene);
  WM_event_add_notifier(C, NC_SCENE | ND_LAYER_CONTENT, scene);
  ED_outliner_select_sync_from_object_tag(C);
  return OPERATOR_FINISHED;
}

void WM_OT_angelica_character_import(wmOperatorType *ot)
{
  ot->name = "Import Angelica Character";
  ot->description =
      "Assemble a game character (body, equipment, weapons, wings and face) from its "
      "description, loading the models from the game client";
  ot->idname = "WM_OT_angelica_character_import";

  ot->exec = wm_angelica_character_import_exec;
  ot->poll = WM_operator_winactive;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  PropertyRNA *prop;
  prop = RNA_def_string(ot->srna,
                        "spec_json",
                        nullptr,
                        0,
                        "Character",
                        "Character description (JSON) from the GameForge account services");
  RNA_def_property_flag(prop, PROP_HIDDEN | PROP_SKIP_SAVE);
  RNA_def_string_dir_path(ot->srna,
                          "client_root",
                          nullptr,
                          FILE_MAX,
                          "Client Folder",
                          "Game client folder (usually 'element') holding the .pck packages");
  RNA_def_string(ot->srna, "name", nullptr, 64, "Name", "Name of the character collection");
  RNA_def_float(ot->srna, "global_scale", 1.0f, 1e-6f, 1e6f, "Scale", "", 0.001f, 1000.0f);
  RNA_def_boolean(
      ot->srna, "import_textures", true, "Textures", "Load the textures of every model");
  RNA_def_boolean(ot->srna,
                  "import_animations",
                  true,
                  "Animations",
                  "Load the character animations (.stck)");
  RNA_def_boolean(ot->srna,
                  "mark_as_asset",
                  false,
                  "Mark as Asset",
                  "Add the assembled character to the Asset Browser");
}

}  // namespace blender

#endif /* WITH_IO_ANGELICA */
