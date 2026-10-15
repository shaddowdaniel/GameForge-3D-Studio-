/* SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup angelica
 *
 * Block compression for the DDS textures of the client (`D3DFMT_DXT1`, `DXT3`, `DXT5`), so an
 * edited texture is saved in the format the game loaded. Reading DDS files is done by the image
 * library; the DDS header and the mip chain are assembled by the caller.
 */

#pragma once

#include <cstdint>

#include "angelica_reader.hh"

namespace blender::io::angelica {

enum class BlockFormat : uint8_t {
  /** 4 colors per block, or 3 colors and transparent black where alpha < 128. */
  DXT1,
  /** DXT1 colors plus 4-bit explicit alpha. */
  DXT3,
  /** DXT1 colors plus interpolated 8-bit alpha. */
  DXT5,
};

/**
 * Compress `width` x `height` RGBA pixels (8 bits per channel, rows from the top) into blocks of
 * 4x4 pixels, appended to `r_blocks` (8 bytes per block for DXT1, 16 otherwise). Sizes that are
 * not a multiple of 4 are padded by repeating the last row and column, like the D3DX tools.
 */
void compress_blocks(
    BlockFormat format, int width, int height, const uint8_t *rgba, Bytes &r_blocks);

/**
 * Decode blocks of `format` covering `width` x `height` pixels (rows from the top) into RGBA,
 * 8 bits per channel, the way Direct3D does (DXT1 with `c0 <= c1` has 3 colors and transparent
 * black). `blocks` holds the blocks row by row. Returns false when `blocks` is too short.
 */
bool decompress_blocks(
    BlockFormat format, int width, int height, ByteSpan blocks, Bytes &r_rgba);

}  // namespace blender::io::angelica
