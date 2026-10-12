/* SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup angelica
 */

#pragma once

#include <string>

#include "angelica_import_common.hh"
#include "angelica_import_model.hh"

namespace blender::io::angelica {

/**
 * Import every supported asset of a `.pck` package in batch.
 *
 * Model folders are imported once, through their most complete description: an `.ecm` when the
 * folder has one, otherwise its `.smd` files, otherwise loose `.ski`, `.bmd` and `.mox` files.
 * `filter` narrows the entries (`*`, `?`, case-insensitive) and `max_assets` limits the count.
 */
void import_package(ImportContext &ctx,
                    const std::string &pck_path,
                    const std::string &filter,
                    int max_assets,
                    const ModelOptions &options);

}  // namespace blender::io::angelica
