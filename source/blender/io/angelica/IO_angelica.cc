/* SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup angelica
 */

#include <cctype>

#include "BKE_appdir.hh"
#include "BKE_collection.hh"
#include "BKE_report.hh"

#include "BLI_fileops.hh"
#include "BLI_path_utils.hh"

#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_build.hh"

#include "DNA_collection_types.h"

#include "IO_angelica.hh"

#include "angelica_import_common.hh"
#include "angelica_import_model.hh"
#include "angelica_import_package.hh"
#include "angelica_import_terrain.hh"

namespace blender::io::angelica {

const std::vector<std::string> &supported_extensions()
{
  static const std::vector<std::string> extensions = {
      ".pck", ".ecm", ".smd", ".ski", ".bon", ".stck", ".bmd", ".mox", ".trn2", ".hmap"};
  return extensions;
}

std::string schemas_directory(const std::string &kind)
{
  const std::optional<std::string> cache = BKE_appdir_folder_id_user_notest(
      BLENDER_USER_DATAFILES, "gameforge_plugins" SEP_STR "estudio-angelica-schemas");
  if (!cache) {
    return {};
  }
  Bytes current;
  if (!read_file(path_join(*cache, "current.txt"), current)) {
    return {};
  }
  std::string version(current.begin(), current.end());
  while (!version.empty() && std::isspace(uint8_t(version.back()))) {
    version.pop_back();
  }
  if (version.empty() || version.find_first_of("/\\:") != std::string::npos ||
      version.find("..") != std::string::npos)
  {
    return {};
  }
  const std::string dir = path_join(path_join(*cache, version), kind);
  return BLI_is_dir(dir.c_str()) ? dir : std::string();
}

/** Engine path of a file that lies inside the client folder, empty otherwise. */
static std::string engine_path_in_client(const std::string &client_root, const std::string &file)
{
  if (client_root.empty()) {
    return {};
  }
  std::string root = client_root;
  std::string path = file;
  for (char &c : root) {
    c = (c == '\\') ? '/' : c;
  }
  for (char &c : path) {
    c = (c == '\\') ? '/' : c;
  }
  while (!root.empty() && root.back() == '/') {
    root.pop_back();
  }
  if (path.size() <= root.size() + 1 || path[root.size()] != '/') {
    return {};
  }
  for (size_t i = 0; i < root.size(); i++) {
    if (std::tolower(uint8_t(root[i])) != std::tolower(uint8_t(path[i]))) {
      return {};
    }
  }
  return path.substr(root.size() + 1);
}

std::vector<ImportedAsset> import_file(Main *bmain,
                                       Scene *scene,
                                       ViewLayer *view_layer,
                                       const AngelicaImportParams &params)
{
  const std::string filepath = params.filepath;
  const std::string ext = path_extension(filepath);
  std::string client_root = params.client_root;
  if (client_root.empty()) {
    client_root = detect_client_root(filepath);
  }

  ImportContext ctx(bmain,
                    scene,
                    view_layer,
                    params.reports,
                    client_root,
                    params.global_scale,
                    params.import_textures,
                    params.link_to_scene);

  ModelOptions options;
  options.import_armature = params.import_armature;
  options.import_animations = params.import_animations;

  if (ext == ".pck") {
    import_package(ctx, filepath, params.filter, params.max_assets, options);
  }
  else if (ext == ".trn2") {
    import_terrain(ctx, path_dirname(filepath), path_stem(filepath), params.terrain_step);
  }
  else if (ext == ".hmap") {
    const std::string dir = path_dirname(filepath);
    const std::string code = path_basename(dir);
    import_hmap_folder(ctx, dir, code, "", params.terrain_step);
  }
  else if (is_model_extension(ext)) {
    std::string engine_path = engine_path_in_client(client_root, filepath);
    if (engine_path.empty()) {
      /* A file outside the client: read it and its neighbors from its own folder. */
      ctx.vfs.add_search_dir(path_dirname(filepath));
      engine_path = path_basename(filepath);
    }
    else {
      ctx.vfs.add_search_dir(path_dirname(filepath));
    }
    Collection *parent = params.link_to_scene ? ctx.target_collection() : nullptr;
    if (import_model(ctx, engine_path, options, parent) == nullptr) {
      BKE_reportf(params.reports, RPT_ERROR, "Angelica: cannot import '%s'", filepath.c_str());
    }
  }
  else {
    BKE_reportf(params.reports, RPT_ERROR, "Angelica: unsupported file '%s'", filepath.c_str());
  }

  DEG_relations_tag_update(bmain);
  return std::move(ctx.assets);
}

std::vector<ImportedAsset> import_map(Main *bmain,
                                      Scene *scene,
                                      ViewLayer *view_layer,
                                      const AngelicaMapImportParams &params)
{
  std::string dir = params.directory;
  while (dir.size() > 1 && (dir.back() == '/' || dir.back() == '\\')) {
    dir.pop_back();
  }
  const std::string code = path_basename(dir);

  std::string client_root = params.client_root;
  if (client_root.empty()) {
    client_root = detect_client_root(dir);
  }

  ImportContext ctx(bmain,
                    scene,
                    view_layer,
                    params.reports,
                    client_root,
                    params.global_scale,
                    params.import_textures,
                    params.link_to_scene);

  bool found = false;
  if (params.import_terrain && !find_file_nocase(dir, code + ".trn2").empty()) {
    found |= import_terrain(ctx, dir, code, params.terrain_step);
  }
  if (params.import_heightmap && !find_file_nocase(dir, "1.hmap").empty()) {
    found |= import_hmap_folder(ctx, dir, code, dir, params.terrain_step);
  }
  if (!found) {
    BKE_reportf(params.reports,
                RPT_ERROR,
                "Angelica: no terrain ('%s.trn2') or heightmaps ('1.hmap') in '%s'",
                code.c_str(),
                dir.c_str());
  }

  DEG_relations_tag_update(bmain);
  return std::move(ctx.assets);
}

}  // namespace blender::io::angelica
