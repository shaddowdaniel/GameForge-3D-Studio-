# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Inventory icons of a client: the atlases ``surfaces\\iconset\\iconlist_ivtrm.dds`` (male) and
``iconlist_ivtrf.dds`` (female) with their name lists (``.txt``, GBK): icon width, icon height,
rows and columns of the grid, then one file name per cell in row order. Items name their icon in
``file_icon``; the client looks the file name up in the list.

A new icon goes into the next free cell. DXT blocks are independent 4x4 tiles, so only the blocks
of that cell are written, in every mip level where the cell covers whole blocks (the smallest
levels keep their old pixels, which inventory windows never show).
"""

__all__ = (
    "ATLASES",
    "IconAtlas",
)

import os
import struct

import numpy as np

from . import sources

ATLASES = ("surfaces\\iconset\\iconlist_ivtrm", "surfaces\\iconset\\iconlist_ivtrf")
_BLOCK_BYTES = {b"DXT1": 8, b"DXT3": 16, b"DXT5": 16}


def icon_name(file_icon):
    """The name an item's ``file_icon`` is listed under (file name, lower case)."""
    return file_icon.replace("/", "\\").rsplit("\\", 1)[-1].strip().lower()


class IconAtlas:
    """One atlas of a client: its name list and its DDS, editable in memory."""

    def __init__(self, root, base):
        import _angelica
        self.base = base
        txt = _angelica.read(root, base + ".txt")
        dds = _angelica.read(root, base + ".dds")
        if txt is None or dds is None:
            raise FileNotFoundError("o cliente não tem o atlas de ícones " + base)
        text, self.encoding, self.newline = sources.decode_text(txt)
        lines = text.split("\n")
        self.icon_w, self.icon_h, self.rows, self.cols = (int(v) for v in lines[:4])
        self.names = [line for line in lines[4:] if line.strip()]
        self.trailing_newline = text.endswith("\n")
        self.lookup = {}
        for i, name in enumerate(self.names):
            self.lookup.setdefault(name.strip().lower(), i)
        self.dds = bytearray(dds)
        self.height, self.width = struct.unpack_from("<II", self.dds, 12)
        mips = struct.unpack_from("<I", self.dds, 28)[0]
        flags = struct.unpack_from("<I", self.dds, 8)[0]
        self.levels = mips if flags & 0x20000 and mips > 1 else 1
        pf_flags, self.fourcc, self.bits = struct.unpack_from("<I4sI", self.dds, 80)
        if not pf_flags & 0x4:
            self.fourcc = None
            if self.bits != 32:
                raise ValueError("atlas de ícones de {:d} bits ainda não é gravado".format(self.bits))
        elif self.fourcc not in _BLOCK_BYTES:
            raise ValueError("formato do atlas de ícones não suportado: " + self.fourcc.decode("latin-1"))
        self.changed = False

    @property
    def capacity(self):
        return (self.height // self.icon_h) * self.cols

    def find(self, file_icon):
        return self.lookup.get(icon_name(file_icon))

    def _level(self, level):
        """``(offset, width, height)`` of a mip level of the DDS."""
        offset, w, h = 128, self.width, self.height
        for _ in range(level):
            offset += self._level_size(w, h)
            w, h = max(1, w // 2), max(1, h // 2)
        return offset, w, h

    def _level_size(self, w, h):
        if self.fourcc:
            return max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * _BLOCK_BYTES[self.fourcc]
        return w * h * 4

    def _cell(self, index):
        return (index % self.cols) * self.icon_w, (index // self.cols) * self.icon_h

    def pixels(self, index):
        """RGBA pixels (``icon_h`` x ``icon_w`` x 4, top row first) of a cell."""
        import _angelica
        x, y = self._cell(index)
        offset, w, _h = self._level(0)
        if not self.fourcc:
            rows = [self.dds[offset + ((y + r) * w + x) * 4: offset + ((y + r) * w + x + self.icon_w) * 4]
                    for r in range(self.icon_h)]
            bgra = np.frombuffer(b"".join(rows), dtype=np.uint8).reshape(self.icon_h, self.icon_w, 4)
            return bgra[..., [2, 1, 0, 3]].copy()
        size = _BLOCK_BYTES[self.fourcc]
        blocks_x = (w + 3) // 4
        data = bytearray()
        for by in range(y // 4, (y + self.icon_h) // 4):
            start = offset + (by * blocks_x + x // 4) * size
            data += self.dds[start:start + (self.icon_w // 4) * size]
        rgba = _angelica.decompress_blocks(self.fourcc.decode(), self.icon_w, self.icon_h, bytes(data))
        return np.frombuffer(rgba, dtype=np.uint8).reshape(self.icon_h, self.icon_w, 4).copy()

    def add(self, name, rgba):
        """Put an icon in the next free cell under ``name``. Returns its index."""
        import _angelica
        from . import textures
        index = len(self.names)
        if index >= self.capacity:
            raise ValueError("o atlas de ícones {:s} está cheio ({:d} ícones)".format(self.base, self.capacity))
        x, y = self._cell(index)
        level_pixels = np.ascontiguousarray(rgba, dtype=np.uint8)
        for level in range(self.levels):
            cw, ch = self.icon_w >> level, self.icon_h >> level
            lx, ly = x >> level, y >> level
            if level:
                level_pixels = textures._downsample(level_pixels)
            offset, w, _h = self._level(level)
            if self.fourcc is None:
                if cw < 1:
                    break
                bgra = level_pixels[..., [2, 1, 0, 3]]
                for r in range(ch):
                    start = offset + ((ly + r) * w + lx) * 4
                    self.dds[start:start + cw * 4] = bgra[r].tobytes()
                continue
            if cw < 4 or ch < 4 or lx % 4 or ly % 4:
                break
            size = _BLOCK_BYTES[self.fourcc]
            blocks = _angelica.compress_blocks(self.fourcc.decode(), cw, ch, level_pixels.tobytes())
            blocks_x = (w + 3) // 4
            row_bytes = (cw // 4) * size
            for row in range(ch // 4):
                start = offset + ((ly // 4 + row) * blocks_x + lx // 4) * size
                self.dds[start:start + row_bytes] = blocks[row * row_bytes:(row + 1) * row_bytes]
        self.names.append(name)
        self.lookup.setdefault(name.strip().lower(), index)
        self.rows = max(self.rows, index // self.cols + 1)
        self.changed = True
        return index

    def txt_bytes(self):
        lines = [str(self.icon_w), str(self.icon_h), str(self.rows), str(self.cols)] + self.names
        text = "\n".join(lines) + ("\n" if self.trailing_newline else "")
        return sources.encode_text(text, self.encoding, self.newline)

    def dds_bytes(self):
        return bytes(self.dds)
