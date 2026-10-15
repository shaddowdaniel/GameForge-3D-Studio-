/* SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup angelica
 */

#include <map>
#include <memory>
#include <set>

#include "BKE_collection.hh"

#include "DNA_collection_types.h"

#include "angelica_import_package.hh"
#include "angelica_pck.hh"

namespace blender::io::angelica {

/** Lower number = preferred description of a model folder. */
static int model_priority(const std::string &ext)
{
  if (ext == ".ecm") {
    return 0;
  }
  if (ext == ".smd") {
    return 1;
  }
  return 2;
}

void import_package(ImportContext &ctx,
                    const std::string &pck_path,
                    const std::string &filter,
                    const int max_assets,
                    const ModelOptions &options)
{
  std::string error;
  std::shared_ptr<PckArchive> package = open_package_shared(pck_path, &error);
  if (!package) {
    ctx.error("Angelica: cannot open package '" + pck_path + "': " + error);
    return;
  }
  const std::string mount = path_stem(pck_path);
  ctx.vfs.add_package(package, mount);

  /* Pick the entries to import, grouped by folder so a model is imported once. */
  std::map<std::string, std::map<int, std::vector<const PckEntry *>>> folders;
  for (const PckEntry &entry : package->entries()) {
    const std::string ext = path_extension(entry.path);
    if (!is_model_extension(ext) || ext == ".bon") {
      continue;
    }
    if (!filter.empty() && !wildcard_match(filter, entry.path) &&
        !wildcard_match(filter, mount + "\\" + entry.path))
    {
      continue;
    }
    folders[path_dirname(entry.path)][model_priority(ext)].push_back(&entry);
  }

  if (folders.empty()) {
    ctx.warn("Angelica: no importable models in '" + path_basename(pck_path) + "'" +
             (filter.empty() ? "" : " matching '" + filter + "'"));
    return;
  }

  Collection *package_collection = nullptr;
  if (ctx.link_to_scene) {
    package_collection = BKE_collection_add(ctx.bmain, ctx.target_collection(), mount.c_str());
    /* A whole package placed at the origin is unreadable in the viewport: keep it hidden, the
     * Asset Browser is where it is browsed. */
    package_collection->flag |= COLLECTION_HIDE_VIEWPORT | COLLECTION_HIDE_RENDER;
  }

  int count = 0;
  for (const auto &[folder, by_priority] : folders) {
    /* Only the most complete kind of description present in this folder. */
    const std::vector<const PckEntry *> &entries = by_priority.begin()->second;
    for (const PckEntry *entry : entries) {
      if (max_assets > 0 && count >= max_assets) {
        return;
      }
      /* Engine paths include the package folder so references resolve like in the client. */
      const std::string engine_path = mount + "\\" + entry->path;
      if (import_model(ctx, engine_path, options, package_collection) != nullptr) {
        count++;
      }
    }
  }
}

}  // namespace blender::io::angelica
