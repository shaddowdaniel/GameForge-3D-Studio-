# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Edited textures written back in the format the client loaded: the DDS keeps its header, pixel
format (DXT1/3/5 or uncompressed masks) and mip chain; the TGA keeps its depth and orientation.
"""

__all__ = (
    "encode_like",
    "image_rgba8",
)

import struct

import numpy as np

DDSD_PITCH = 0x8
DDSD_MIPMAPCOUNT = 0x20000
DDSD_LINEARSIZE = 0x80000
DDPF_ALPHAPIXELS = 0x1
DDPF_ALPHA = 0x2
DDPF_FOURCC = 0x4
DDSCAPS2_CUBEMAP = 0x200
DDSCAPS2_VOLUME = 0x200000

BLOCK_FORMATS = {b"DXT1": 8, b"DXT2": 16, b"DXT3": 16, b"DXT4": 16, b"DXT5": 16}


def image_rgba8(image):
    """The pixels of a Blender image as an ``(height, width, 4)`` uint8 array, top row first."""
    width, height = image.size
    pixels = np.empty(width * height * 4, dtype=np.float32)
    image.pixels.foreach_get(pixels)
    pixels = pixels.reshape(height, width, 4)[::-1]
    return np.clip(np.rint(pixels * 255.0), 0, 255).astype(np.uint8)


def _downsample(rgba):
    """Half size (box filter), at least one pixel per side."""
    level = rgba.astype(np.float32)
    if level.shape[0] > 1:
        rows = level.shape[0] // 2 * 2
        level = (level[0:rows:2] + level[1:rows:2]) * 0.5
    if level.shape[1] > 1:
        cols = level.shape[1] // 2 * 2
        level = (level[:, 0:cols:2] + level[:, 1:cols:2]) * 0.5
    return np.clip(np.rint(level), 0, 255).astype(np.uint8)


def _full_chain(width, height):
    return max(width, height).bit_length()


def _pack_masks(rgba, bit_count, masks, luminance):
    """Uncompressed pixels with the channel bit masks of the DDS pixel format."""
    channels = rgba.astype(np.uint64)
    value = np.zeros(rgba.shape[:2], dtype=np.uint64)
    for channel, mask in enumerate(masks):
        if not mask:
            continue
        shift = (mask & -mask).bit_length() - 1
        bits = bin(mask).count("1")
        source = channels[..., 0] if luminance and channel < 3 else channels[..., channel]
        value |= ((source * ((1 << bits) - 1) + 127) // 255) << np.uint64(shift)
    byte_count = bit_count // 8
    as_bytes = value.astype("<u8").view(np.uint8).reshape(rgba.shape[0], rgba.shape[1], 8)
    return as_bytes[..., :byte_count].tobytes()


def encode_dds(original, rgba):
    """A DDS file like ``original`` (its header and format) holding the pixels ``rgba``."""
    import _angelica

    if len(original) < 128 or original[:4] != b"DDS ":
        raise ValueError("o arquivo original não é um DDS válido")
    header = bytearray(original[4:128])
    (_size, flags, _height, _width, _pitch, _depth, mip_count) = struct.unpack_from("<7I", header, 0)
    pf_flags, fourcc, bit_count, r_mask, g_mask, b_mask, a_mask = struct.unpack_from("<I4s5I", header, 76)
    caps2 = struct.unpack_from("<I", header, 108)[0]
    if caps2 & (DDSCAPS2_CUBEMAP | DDSCAPS2_VOLUME):
        raise ValueError("texturas cúbicas e de volume ainda não são gravadas")

    height, width = rgba.shape[:2]
    same_size = (width, height) == (_width, _height)
    original_levels = mip_count if flags & DDSD_MIPMAPCOUNT and mip_count > 1 else 1
    if original_levels == 1:
        levels = 1
    elif original_levels >= _full_chain(_width, _height):
        levels = _full_chain(width, height)
    else:
        levels = min(original_levels, _full_chain(width, height))

    compressed = pf_flags & DDPF_FOURCC
    if compressed and fourcc not in BLOCK_FORMATS:
        raise ValueError("formato DDS não suportado: " + fourcc.decode("latin-1"))
    if compressed and fourcc in (b"DXT2", b"DXT4"):
        raise ValueError("DDS com alfa pré-multiplicado (DXT2/DXT4) ainda não é gravado")
    if not compressed and bit_count not in (8, 16, 24, 32):
        raise ValueError("DDS de {:d} bits por pixel não suportado".format(bit_count))

    data = bytearray()
    level = rgba
    for index in range(levels):
        if index:
            level = _downsample(level)
        if compressed:
            data += _angelica.compress_blocks(fourcc.decode("ascii"), level.shape[1], level.shape[0],
                                              np.ascontiguousarray(level).tobytes())
        else:
            luminance = bool(pf_flags & 0x20000) and not pf_flags & 0x40
            data += _pack_masks(level, bit_count, (r_mask, g_mask, b_mask, a_mask), luminance)

    # The header stays as it was unless the size changed (tools disagree on the pitch field).
    if not same_size:
        struct.pack_into("<II", header, 8, height, width)
        if compressed:
            pitch = max(1, (width + 3) // 4) * max(1, (height + 3) // 4) * BLOCK_FORMATS[fourcc]
        else:
            pitch = (width * bit_count + 7) // 8
        struct.pack_into("<I", header, 16, pitch)
    if levels != original_levels:
        struct.pack_into("<I", header, 24, levels)
    return b"DDS " + bytes(header) + bytes(data)


def encode_tga(original, rgba):
    """An uncompressed TGA with the depth, orientation and image id of ``original``."""
    if len(original) < 18:
        raise ValueError("o arquivo original não é um TGA válido")
    id_length, colormap_type, image_type = original[0], original[1], original[2]
    bit_count, descriptor = original[16], original[17]
    if colormap_type or image_type not in (2, 3, 10, 11):
        raise ValueError("TGA com paleta de cores ainda não é gravado")
    gray = image_type in (3, 11)
    height, width = rgba.shape[:2]
    pixels = rgba if descriptor & 0x20 else rgba[::-1]
    if descriptor & 0x10:
        pixels = pixels[:, ::-1]
    if gray:
        body = pixels[..., 0:1]
        bit_count = 8
    elif bit_count == 32:
        body = pixels[..., [2, 1, 0, 3]]
    else:
        body = pixels[..., [2, 1, 0]]
        bit_count = 24
        descriptor &= ~0x0F
    header = bytearray(original[:18])
    header[2] = 3 if gray else 2
    struct.pack_into("<HH", header, 12, width, height)
    header[16] = bit_count
    header[17] = descriptor
    return bytes(header) + original[18:18 + id_length] + np.ascontiguousarray(body).tobytes()


def encode_like(path, original, rgba):
    """The bytes of the texture ``path`` (for its extension) like ``original`` with ``rgba``."""
    ext = path.lower().rsplit(".", 1)[-1]
    if ext == "dds":
        return encode_dds(original, rgba)
    if ext == "tga":
        return encode_tga(original, rgba)
    raise ValueError("texturas ." + ext + " ainda não são gravadas no cliente")
