/* SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup angelica
 */

#pragma once

#include <string>

#include "angelica_import_common.hh"

#include "BLI_vector.hh"

namespace blender {
struct Collection;
struct Object;
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

/* -------------------------------------------------------------------- */
/** \name Building Blocks for Assembled Characters
 *
 * Used by the character preview (`angelica_import_character.cc`), which puts equipment skins on
 * a body skeleton and hangs weapons, wings and the face from its hooks.
 * \{ */

/**
 * Import a `.smd` (skeleton, skins, animations) plus `extra_skins` (client paths) bound to the same
 * skeleton, into `collection`. Returns the armature object, null when the model has no skeleton.
 */
Object *import_smd_with_skins(ImportContext &ctx,
                              const std::string &smd_path,
                              const Vector<std::string> &extra_skins,
                              const ModelOptions &options,
                              Collection *collection);

/**
 * Build an armature from a `.bon` and bind `skins` (client paths) to it, with the animation of
 * `stck_path` (or the skeleton's own tracks) when animations are enabled. Used for weapons, wings
 * and faces, which are a skeleton plus skins without a `.smd`. Returns the armature object.
 */
Object *import_bon_with_skins(ImportContext &ctx,
                              const std::string &bon_path,
                              const Vector<std::string> &skins,
                              const std::string &stck_path,
                              const ModelOptions &options,
                              Collection *collection);

/** \} */

}  // namespace blender::io::angelica
