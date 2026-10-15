/* SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup editor/io
 *
 * Operators for the native Angelica Engine (Perfect World) importers. Everything they import is
 * marked as an asset and placed in a catalog that mirrors its path in the client, so whole
 * packages and maps show up directly in the Asset Browser.
 */

#ifdef WITH_IO_ANGELICA

#  include <string>
#  include <vector>

#  include "BKE_asset.hh"
#  include "BKE_context.hh"
#  include "BKE_file_handler.hh"
#  include "BKE_main.hh"
#  include "BKE_report.hh"

#  include "BLI_fileops.hh"
#  include "BLI_path_utils.hh"
#  include "BLI_string.hh"
#  include "BLI_string_utf8.hh"

#  include "BLT_translation.hh"

#  include "DEG_depsgraph.hh"
#  include "DEG_depsgraph_build.hh"

#  include "DNA_ID.h"
#  include "DNA_asset_types.h"
#  include "DNA_scene_types.h"
#  include "DNA_space_types.h"

#  include "ED_outliner.hh"

#  include "MEM_guardedalloc.h"

#  include "RNA_access.hh"
#  include "RNA_define.hh"

#  include "UI_interface.hh"
#  include "UI_interface_layout.hh"
#  include "UI_resources.hh"

#  include "WM_api.hh"
#  include "WM_types.hh"

#  include "AS_asset_catalog.hh"
#  include "AS_asset_catalog_path.hh"
#  include "AS_asset_library.hh"

#  include "ED_asset_mark_clear.hh"

#  include "IO_angelica.hh"
#  include "io_angelica_ops.hh"
#  include "io_utils.hh"

namespace blender {

/* -------------------------------------------------------------------- */
/** \name Asset Marking
 * \{ */

static asset_system::AssetCatalog *ensure_catalog(asset_system::AssetCatalogService &service,
                                                  const asset_system::AssetCatalogPath &path)
{
  if (path.str().empty()) {
    return nullptr;
  }
  if (asset_system::AssetCatalog *catalog = service.find_catalog_by_path(path)) {
    return catalog;
  }
  /* Create the parents first so the tree is browsable at every level. */
  const asset_system::AssetCatalogPath parent = path.parent();
  if (!parent.str().empty() && parent != path) {
    ensure_catalog(service, parent);
  }
  asset_system::AssetCatalog *catalog = service.create_catalog(path);
  service.tag_has_unsaved_changes(catalog);
  return catalog;
}

static int mark_imported_assets(bContext *C,
                                Main *bmain,
                                const std::vector<io::angelica::ImportedAsset> &assets,
                                const bool use_catalogs,
                                const bool generate_previews)
{
  asset_system::AssetLibrary *library = nullptr;
  if (use_catalogs) {
    library = AS_asset_library_load(bmain, asset_system::current_file_library_reference());
  }

  int marked = 0;
  for (const io::angelica::ImportedAsset &asset : assets) {
    if (asset.id == nullptr) {
      continue;
    }
    ed::asset::mark_id(asset.id);
    AssetMetaData *meta = asset.id->asset_data;
    if (meta == nullptr) {
      continue;
    }
    marked++;
    if (!asset.description.empty()) {
      MEM_SAFE_DELETE(meta->description);
      meta->description = BLI_strdup(asset.description.c_str());
    }
    for (const std::string &tag : asset.tags) {
      BKE_asset_metadata_tag_ensure(meta, tag.c_str());
    }
    if (library != nullptr && !asset.catalog_path.empty()) {
      asset_system::AssetCatalogService &service = library->catalog_service();
      if (!service.is_read_only()) {
        const asset_system::AssetCatalogPath path(asset.catalog_path);
        if (const asset_system::AssetCatalog *catalog = ensure_catalog(service, path.cleanup())) {
          BKE_asset_metadata_catalog_id_set(
              meta, catalog->catalog_id, catalog->simple_name.c_str());
        }
      }
    }
    if (generate_previews) {
      ed::asset::generate_preview(C, asset.id);
    }
  }

  WM_main_add_notifier(NC_ASSET | ND_ASSET_LIST | NA_ADDED, nullptr);
  WM_main_add_notifier(NC_ASSET | ND_ASSET_CATALOGS, nullptr);
  WM_main_add_notifier(NC_ID | NA_EDITED, nullptr);
  return marked;
}

static void finish_import(bContext *C,
                          wmOperator *op,
                          const std::vector<io::angelica::ImportedAsset> &assets)
{
  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);

  int marked = 0;
  if (RNA_boolean_get(op->ptr, "mark_as_assets")) {
    marked = mark_imported_assets(C,
                                  bmain,
                                  assets,
                                  RNA_boolean_get(op->ptr, "use_catalogs"),
                                  RNA_boolean_get(op->ptr, "generate_previews"));
  }

  DEG_id_tag_update(&scene->id, ID_RECALC_BASE_FLAGS);
  DEG_relations_tag_update(bmain);
  WM_event_add_notifier(C, NC_SCENE | ND_OB_SELECT, scene);
  WM_event_add_notifier(C, NC_SCENE | ND_OB_ACTIVE, scene);
  WM_event_add_notifier(C, NC_SCENE | ND_LAYER_CONTENT, scene);
  ED_outliner_select_sync_from_object_tag(C);

  if (marked > 0) {
    BKE_reportf(op->reports, RPT_INFO, "Imported %d Angelica asset(s)", marked);
  }
  else {
    BKE_reportf(op->reports, RPT_INFO, "Imported %d Angelica item(s)", int(assets.size()));
  }
}

static void def_asset_properties(wmOperatorType *ot)
{
  RNA_def_boolean(ot->srna,
                  "mark_as_assets",
                  true,
                  "Mark as Assets",
                  "Add everything that is imported to the asset library of this file");
  RNA_def_boolean(ot->srna,
                  "use_catalogs",
                  true,
                  "Catalogs from Client Folders",
                  "Place the assets in catalogs that mirror their folders in the client");
  RNA_def_boolean(ot->srna,
                  "generate_previews",
                  true,
                  "Generate Previews",
                  "Render preview images for the imported assets");
}

static void def_common_properties(wmOperatorType *ot)
{
  PropertyRNA *prop = RNA_def_string_dir_path(
      ot->srna,
      "client_root",
      nullptr,
      FILE_MAX,
      "Client Folder",
      "Client data folder (usually 'element') used to find textures, skeletons and packages "
      "referenced by the imported files. Detected automatically when empty");
  RNA_def_property_flag(prop, PROP_SKIP_SAVE);

  RNA_def_float(ot->srna, "global_scale", 1.0f, 1e-6f, 1e6f, "Scale", "", 0.001f, 1000.0f);
  RNA_def_boolean(ot->srna,
                  "import_textures",
                  true,
                  "Textures",
                  "Load and pack the textures used by the imported assets");
  RNA_def_boolean(ot->srna,
                  "link_to_scene",
                  true,
                  "Add to Scene",
                  "Place the imported objects in the current scene. When disabled they only live "
                  "in the file, as assets");
  RNA_def_int(ot->srna,
              "terrain_step",
              1,
              1,
              16,
              "Terrain Detail Step",
              "Use every n-th terrain vertex (1 keeps the full resolution of the client)",
              1,
              8);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Import Files and Packages
 * \{ */

static wmOperatorStatus wm_angelica_import_exec(bContext *C, wmOperator *op)
{
  const Vector<std::string> paths = ed::io::paths_from_operator_properties(op->ptr);
  if (paths.is_empty()) {
    BKE_report(op->reports, RPT_ERROR, "No filepath given");
    return OPERATOR_CANCELLED;
  }

  AngelicaImportParams params;
  RNA_string_get(op->ptr, "client_root", params.client_root);
  RNA_string_get(op->ptr, "filter", params.filter);
  params.max_assets = RNA_int_get(op->ptr, "max_assets");
  params.global_scale = RNA_float_get(op->ptr, "global_scale");
  params.import_textures = RNA_boolean_get(op->ptr, "import_textures");
  params.import_armature = RNA_boolean_get(op->ptr, "import_armature");
  params.import_animations = RNA_boolean_get(op->ptr, "import_animations");
  params.link_to_scene = RNA_boolean_get(op->ptr, "link_to_scene");
  params.terrain_step = RNA_int_get(op->ptr, "terrain_step");
  params.reports = op->reports;

  Main *bmain = CTX_data_main(C);
  Scene *scene = CTX_data_scene(C);
  ViewLayer *view_layer = CTX_data_view_layer(C);

  std::vector<io::angelica::ImportedAsset> assets;
  for (const std::string &path : paths) {
    STRNCPY(params.filepath, path.c_str());
    std::vector<io::angelica::ImportedAsset> imported = io::angelica::import_file(
        bmain, scene, view_layer, params);
    assets.insert(assets.end(), imported.begin(), imported.end());
  }

  finish_import(C, op, assets);
  return OPERATOR_FINISHED;
}

static void wm_angelica_import_draw(bContext *C, wmOperator *op)
{
  ui::Layout &layout = *op->layout;
  PointerRNA *ptr = op->ptr;
  layout.use_property_split_set(true);
  layout.use_property_decorate_set(false);

  if (ui::Layout *panel = layout.panel(C, "ANGELICA_import_general", false, IFACE_("General"))) {
    ui::Layout &col = panel->column(false);
    col.prop(ptr, "global_scale", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    col.prop(ptr, "client_root", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    col.prop(ptr, "link_to_scene", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    col.prop(ptr, "import_textures", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
  if (ui::Layout *panel = layout.panel(C, "ANGELICA_import_package", false, IFACE_("Packages"))) {
    ui::Layout &col = panel->column(false);
    col.prop(ptr, "filter", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    col.prop(ptr, "max_assets", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
  if (ui::Layout *panel = layout.panel(C, "ANGELICA_import_models", false, IFACE_("Models"))) {
    ui::Layout &col = panel->column(false);
    col.prop(ptr, "import_armature", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    col.prop(ptr, "import_animations", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    col.prop(ptr, "terrain_step", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
  if (ui::Layout *panel = layout.panel(C, "ANGELICA_import_assets", false, IFACE_("Assets"))) {
    ui::Layout &col = panel->column(false);
    col.prop(ptr, "mark_as_assets", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    ui::Layout &sub = col.column(false);
    sub.active_set(RNA_boolean_get(ptr, "mark_as_assets"));
    sub.prop(ptr, "use_catalogs", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    sub.prop(ptr, "generate_previews", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
}

void WM_OT_angelica_import(wmOperatorType *ot)
{
  ot->name = "Import Angelica";
  ot->description =
      "Import Angelica Engine (Perfect World) files: packages (.pck) in batch, models, "
      "terrain and heightmaps, as assets";
  ot->idname = "WM_OT_angelica_import";

  ot->invoke = ed::io::filesel_drop_import_invoke;
  ot->exec = wm_angelica_import_exec;
  ot->poll = WM_operator_winactive;
  ot->ui = wm_angelica_import_draw;
  ot->flag = OPTYPE_UNDO | OPTYPE_PRESET;

  WM_operator_properties_filesel(ot,
                                 FILE_TYPE_FOLDER,
                                 FILE_BLENDER,
                                 FILE_OPENFILE,
                                 WM_FILESEL_FILEPATH | WM_FILESEL_FILES | WM_FILESEL_DIRECTORY |
                                     WM_FILESEL_SHOW_PROPS,
                                 FILE_DEFAULTDISPLAY,
                                 FILE_SORT_DEFAULT);

  def_common_properties(ot);

  RNA_def_string(ot->srna,
                 "filter",
                 nullptr,
                 1024,
                 "Package Filter",
                 "Only import package entries whose path matches this pattern, for example "
                 "'models\\npc\\*' or '*.ski' (empty imports every supported asset)");
  RNA_def_int(ot->srna,
              "max_assets",
              0,
              0,
              INT_MAX,
              "Limit",
              "Import at most this many assets from each package (0 for no limit)",
              0,
              10000);
  RNA_def_boolean(ot->srna,
                  "import_armature",
                  true,
                  "Armature",
                  "Create an armature from the skeleton (.bon) and skin the meshes to it");
  RNA_def_boolean(ot->srna,
                  "import_animations",
                  true,
                  "Animations",
                  "Import the skeleton animations (.stck) of the model as actions");

  def_asset_properties(ot);

  PropertyRNA *prop = RNA_def_string(
      ot->srna,
      "filter_glob",
      "*.pck;*.ecm;*.smd;*.ski;*.bon;*.stck;*.bmd;*.mox;*.trn2;*.hmap",
      0,
      "Extension Filter",
      "");
  RNA_def_property_flag(prop, PROP_HIDDEN);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Open an Asset of a Client
 * \{ */

static wmOperatorStatus wm_angelica_import_asset_exec(bContext *C, wmOperator *op)
{
  AngelicaAssetImportParams params;
  RNA_string_get(op->ptr, "client_root", params.client_root);
  RNA_string_get(op->ptr, "engine_path", params.engine_path);
  if (!params.client_root[0] || !params.engine_path[0]) {
    BKE_report(op->reports, RPT_ERROR, "No client folder or asset path given");
    return OPERATOR_CANCELLED;
  }
  params.global_scale = RNA_float_get(op->ptr, "global_scale");
  params.import_textures = RNA_boolean_get(op->ptr, "import_textures");
  params.import_armature = RNA_boolean_get(op->ptr, "import_armature");
  params.import_animations = RNA_boolean_get(op->ptr, "import_animations");
  params.reports = op->reports;

  const std::vector<io::angelica::ImportedAsset> assets = io::angelica::import_client_asset(
      CTX_data_main(C), CTX_data_scene(C), CTX_data_view_layer(C), params);
  if (assets.empty()) {
    return OPERATOR_CANCELLED;
  }
  finish_import(C, op, assets);
  return OPERATOR_FINISHED;
}

void WM_OT_angelica_import_asset(wmOperatorType *ot)
{
  ot->name = "Open Client Asset";
  ot->description =
      "Open an asset of the game client with every file it uses (skins, skeleton, animations, "
      "textures), read from inside its packages without extracting anything";
  ot->idname = "WM_OT_angelica_import_asset";

  ot->exec = wm_angelica_import_asset_exec;
  ot->poll = WM_operator_winactive;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;

  RNA_def_string(ot->srna,
                 "client_root",
                 nullptr,
                 FILE_MAX,
                 "Client Folder",
                 "Folder of the game client (usually 'element')");
  RNA_def_string(ot->srna,
                 "engine_path",
                 nullptr,
                 FILE_MAX,
                 "Asset Path",
                 "Path of the asset inside the client, for example 'models\\npcs\\x\\x.ecm'");
  RNA_def_float(ot->srna, "global_scale", 1.0f, 1e-6f, 1e6f, "Scale", "", 0.001f, 1000.0f);
  RNA_def_boolean(ot->srna, "import_textures", true, "Textures", "Load the textures of the asset");
  RNA_def_boolean(ot->srna,
                  "import_armature",
                  true,
                  "Armature",
                  "Create an armature from the skeleton (.bon) and skin the meshes to it");
  RNA_def_boolean(ot->srna,
                  "import_animations",
                  true,
                  "Animations",
                  "Import the skeleton animations (.stck) of the model as actions");
  def_asset_properties(ot);
  /* Opened to be edited: not added to the file's asset library unless asked. */
  RNA_def_property_boolean_default(RNA_struct_type_find_property(ot->srna, "mark_as_assets"), false);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Import Map Folder
 * \{ */

static wmOperatorStatus wm_angelica_import_map_invoke(bContext *C,
                                                      wmOperator *op,
                                                      const wmEvent * /*event*/)
{
  WM_event_add_fileselect(C, op);
  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus wm_angelica_import_map_exec(bContext *C, wmOperator *op)
{
  AngelicaMapImportParams params;
  RNA_string_get(op->ptr, "directory", params.directory);
  RNA_string_get(op->ptr, "client_root", params.client_root);

  /* A map code turns a client (or `maps`) folder into the map folder. */
  char map_code[64];
  RNA_string_get(op->ptr, "map_code", map_code);
  if (map_code[0] != '\0') {
    char candidate[FILE_MAX];
    BLI_path_join(candidate, sizeof(candidate), params.directory, "maps", map_code);
    if (!BLI_is_dir(candidate)) {
      BLI_path_join(candidate, sizeof(candidate), params.directory, map_code);
    }
    if (!BLI_is_dir(candidate)) {
      BKE_reportf(
          op->reports, RPT_ERROR, "Map '%s' not found in '%s'", map_code, params.directory);
      return OPERATOR_CANCELLED;
    }
    STRNCPY(params.directory, candidate);
  }

  if (params.directory[0] == '\0' || !BLI_is_dir(params.directory)) {
    BKE_report(op->reports, RPT_ERROR, "Select the folder of the map (named after its code)");
    return OPERATOR_CANCELLED;
  }

  params.global_scale = RNA_float_get(op->ptr, "global_scale");
  params.import_terrain = RNA_boolean_get(op->ptr, "import_terrain");
  params.import_heightmap = RNA_boolean_get(op->ptr, "import_heightmap");
  params.import_textures = RNA_boolean_get(op->ptr, "import_textures");
  params.link_to_scene = RNA_boolean_get(op->ptr, "link_to_scene");
  params.terrain_step = RNA_int_get(op->ptr, "terrain_step");
  params.reports = op->reports;

  std::vector<io::angelica::ImportedAsset> assets = io::angelica::import_map(
      CTX_data_main(C), CTX_data_scene(C), CTX_data_view_layer(C), params);
  if (assets.empty()) {
    if (!BKE_reports_contain(op->reports, RPT_ERROR)) {
      BKE_report(op->reports, RPT_ERROR, "No terrain or heightmap found in the map folder");
    }
    return OPERATOR_CANCELLED;
  }

  finish_import(C, op, assets);
  return OPERATOR_FINISHED;
}

static void wm_angelica_import_map_draw(bContext *C, wmOperator *op)
{
  ui::Layout &layout = *op->layout;
  PointerRNA *ptr = op->ptr;
  layout.use_property_split_set(true);
  layout.use_property_decorate_set(false);

  if (ui::Layout *panel = layout.panel(C, "ANGELICA_map_general", false, IFACE_("Map"))) {
    ui::Layout &col = panel->column(false);
    col.prop(ptr, "map_code", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    col.prop(ptr, "client_root", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    col.prop(ptr, "global_scale", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    col.prop(ptr, "link_to_scene", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
  if (ui::Layout *panel = layout.panel(C, "ANGELICA_map_content", false, IFACE_("Content"))) {
    ui::Layout &col = panel->column(false);
    col.prop(ptr, "import_terrain", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    col.prop(ptr, "import_heightmap", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    col.prop(ptr, "import_textures", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    col.prop(ptr, "terrain_step", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
  if (ui::Layout *panel = layout.panel(C, "ANGELICA_map_assets", false, IFACE_("Assets"))) {
    ui::Layout &col = panel->column(false);
    col.prop(ptr, "mark_as_assets", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    ui::Layout &sub = col.column(false);
    sub.active_set(RNA_boolean_get(ptr, "mark_as_assets"));
    sub.prop(ptr, "use_catalogs", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    sub.prop(ptr, "generate_previews", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
}

void WM_OT_angelica_import_map(wmOperatorType *ot)
{
  ot->name = "Import Angelica Map";
  ot->description =
      "Import a Perfect World map by selecting its folder (named after the map code, such as "
      "'a19'): terrain, textures and heightmaps become assets";
  ot->idname = "WM_OT_angelica_import_map";

  ot->invoke = wm_angelica_import_map_invoke;
  ot->exec = wm_angelica_import_map_exec;
  ot->poll = WM_operator_winactive;
  ot->ui = wm_angelica_import_map_draw;
  ot->flag = OPTYPE_UNDO | OPTYPE_PRESET;

  WM_operator_properties_filesel(ot,
                                 FILE_TYPE_FOLDER,
                                 FILE_BLENDER,
                                 FILE_OPENFILE,
                                 WM_FILESEL_DIRECTORY | WM_FILESEL_SHOW_PROPS,
                                 FILE_DEFAULTDISPLAY,
                                 FILE_SORT_DEFAULT);

  RNA_def_string(ot->srna,
                 "map_code",
                 nullptr,
                 64,
                 "Map Code",
                 "Code of the map to import (such as 'a19' or 'world') when the selected folder "
                 "is the client or its 'maps' folder; leave empty when selecting the map folder");
  def_common_properties(ot);
  RNA_def_boolean(ot->srna,
                  "import_terrain",
                  true,
                  "Terrain",
                  "Import the client terrain (.trn2 with its sub-terrain files)");
  RNA_def_boolean(ot->srna,
                  "import_heightmap",
                  true,
                  "Server Heightmap",
                  "Import the server heightmaps (.hmap) of the map as a separate mesh");
  def_asset_properties(ot);
}

/** \} */

namespace ed::io {

void angelica_file_handler_add()
{
  auto fh = std::make_unique<bke::FileHandlerType>();
  STRNCPY_UTF8(fh->idname, "IO_FH_angelica");
  STRNCPY_UTF8(fh->import_operator, "WM_OT_angelica_import");
  STRNCPY_UTF8(fh->label, "Angelica / Perfect World");
  STRNCPY_UTF8(fh->file_extensions_str, ".pck;.ecm;.smd;.ski;.bon;.stck;.bmd;.mox;.trn2;.hmap");
  fh->poll_drop = poll_file_object_drop;
  bke::file_handler_add(std::move(fh));
}

}  // namespace ed::io
}  // namespace blender

#endif /* WITH_IO_ANGELICA */
