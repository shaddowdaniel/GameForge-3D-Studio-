/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup wm
 *
 * Window icon generation for Wayland.
 * Scales the full-color GameForge 3D Studio crest (a PNG, see `tools/gameforge/branding`)
 * to the requested window icon size.
 */

#include <algorithm>
#include <cstring>

#include "GHOST_Types.hh"

#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "wm_window_icon.hh"

extern "C" const int datatoc_gameforge_app_icon_png_size;
extern "C" const char datatoc_gameforge_app_icon_png[];

static void wm_ghost_icon_generate(const GHOST_IconGenerator * /*icon_generator*/,
                                   GHOST_IWindow * /*window*/,
                                   uint8_t *pixels,
                                   int icon_size)
{
  const size_t buffer_size = size_t(icon_size) * icon_size * 4;
  const size_t stride = size_t(icon_size) * 4;
  memset(pixels, 0, buffer_size);

  ImBuf *ibuf = IMB_load_image_from_memory(
      reinterpret_cast<const uchar *>(datatoc_gameforge_app_icon_png),
      size_t(datatoc_gameforge_app_icon_png_size),
      ImBufFlags::ByteData,
      "<window icon>");
  /* The bundled PNG is known to be valid. */
  if (ibuf == nullptr || ibuf->byte_buffer.data == nullptr) [[unlikely]] {
    if (ibuf) {
      IMB_freeImBuf(ibuf);
    }
    return;
  }

  if (ibuf->x != icon_size || ibuf->y != icon_size) {
    IMB_scale(ibuf, icon_size, icon_size, IMBScaleFilter::Box, false);
  }

  /* #ImBuf rows go bottom to top, the icon buffer rows go top to bottom. */
  const int height = std::min(ibuf->y, icon_size);
  const size_t row_size = size_t(std::min(ibuf->x, icon_size)) * 4;
  for (int y = 0; y < height; y++) {
    const uchar *src = ibuf->byte_buffer.data + size_t(ibuf->y - 1 - y) * ibuf->x * 4;
    memcpy(pixels + size_t(y) * stride, src, row_size);
  }

  IMB_freeImBuf(ibuf);
}

const GHOST_IconGenerator wm_ghost_icon_generator = {
    /*generate_fn*/ wm_ghost_icon_generate,
    /*user_data*/ nullptr,
};
