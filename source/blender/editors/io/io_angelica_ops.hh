/* SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup editor/io
 */

#pragma once

namespace blender {

struct wmOperatorType;

void WM_OT_angelica_import(wmOperatorType *ot);
void WM_OT_angelica_import_map(wmOperatorType *ot);

namespace ed::io {
void angelica_file_handler_add();
}

}  // namespace blender
