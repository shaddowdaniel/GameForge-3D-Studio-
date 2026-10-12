/* SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup angelica
 */

#pragma once

#include <string>

#include "angelica_import_common.hh"

namespace blender {
struct Collection;
}

namespace blender::io::angelica {

struct ModelOptions {
  bool import_armature = true;
  bool import_animations = true;
};

/** Whether the extension (with dot, lower-case) is a model that #import_model handles. */
bool is_model_extension(const std::string &ext);

/**
 * Import a model read through the client file system: `.ecm` (with its `.smd`, skins and child
 * models), `.smd` (skeleton, skins, animations), `.ski`, `.bon`, `.bmd` or `.mox`.
 *
 * The objects are created in a new collection named after the model, placed under `parent` (or
 * left unlinked when `parent` is null), and the collection is registered as the asset.
 * Returns null when the model could not be read.
 */
Collection *import_model(ImportContext &ctx,
                         const std::string &engine_path,
                         const ModelOptions &options,
                         Collection *parent,
                         const std::string &catalog_root = "");

}  // namespace blender::io::angelica
