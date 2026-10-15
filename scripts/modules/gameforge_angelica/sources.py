# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Client files opened as Studio data, and where they came from.

Every data-block made from a client file carries ``gameforge_*`` custom properties: the client
folder, the engine path of the file and a fingerprint of what was opened. Saving into the client
compares the current data with that fingerprint to know what was edited.

Text files keep their encoding (GBK, or UTF-16 / UTF-8 with a byte order mark) and line endings
(the engine writes CRLF), so unedited lines are written back byte for byte.
"""

__all__ = (
    "decode_text",
    "encode_text",
    "open_image",
    "open_text",
    "text_fingerprint",
)

import hashlib
import os

PROP_CLIENT = "gameforge_client"
PROP_FILE = "gameforge_file"
PROP_ENCODING = "gameforge_encoding"
PROP_NEWLINE = "gameforge_newline"
PROP_FINGERPRINT = "gameforge_fingerprint"
PROP_EXACT = "gameforge_exact"


def decode_text(data):
    """``(text, encoding, newline)`` for the bytes of a client text file, text with ``\\n`` lines."""
    newline = "\r\n" if b"\r\n" in data else "\n"
    if data.startswith(b"\xff\xfe"):
        encoding = "utf-16-le-bom"
        text = data[2:].decode("utf-16-le")
    elif data.startswith(b"\xef\xbb\xbf"):
        encoding = "utf-8-bom"
        text = data[3:].decode("utf-8")
    else:
        for encoding in ("gbk", "gb18030", "latin-1"):
            try:
                text = data.decode(encoding)
                break
            except UnicodeDecodeError:
                continue
    return text.replace("\r\n", "\n"), encoding, newline


def encode_text(text, encoding, newline):
    """The bytes of a text file as the engine expects them (inverse of :func:`decode_text`)."""
    text = text.replace("\r\n", "\n")
    if newline != "\n":
        text = text.replace("\n", newline)
    if encoding == "utf-16-le-bom":
        return b"\xff\xfe" + text.encode("utf-16-le")
    if encoding == "utf-8-bom":
        return b"\xef\xbb\xbf" + text.encode("utf-8")
    return text.encode(encoding)


def text_fingerprint(text):
    return hashlib.sha1(text.replace("\r\n", "\n").encode("utf-8", "surrogatepass")).hexdigest()


def _set_source(id_data, client_root, path):
    id_data[PROP_CLIENT] = client_root
    id_data[PROP_FILE] = path


def _read(client_root, path):
    import _angelica
    data = _angelica.read(client_root, path)
    if data is None:
        raise FileNotFoundError("O cliente não tem o arquivo " + path)
    return data


def open_text(client_root, path):
    """A ``Text`` data-block with the content of a client text file (``.gfx``, ``.ecm``, ...)."""
    import bpy
    data = _read(client_root, path)
    text, encoding, newline = decode_text(data)
    block = bpy.data.texts.new(os.path.basename(path))
    block.from_string(text)
    _set_source(block, client_root, path)
    block[PROP_ENCODING] = encoding
    block[PROP_NEWLINE] = newline
    block[PROP_FINGERPRINT] = text_fingerprint(text)
    # Mixed line endings or unusual bytes do not survive decoding: saving would change lines the
    # user did not touch, so the save step warns about these files.
    block[PROP_EXACT] = encode_text(text, encoding, newline) == data
    return block


def open_image(client_root, path):
    """An ``Image`` packed with the bytes of a client texture (``.dds``, ``.tga``, ...)."""
    import bpy
    data = _read(client_root, path)
    image = bpy.data.images.new(os.path.basename(path), 1, 1)
    # Pack first: with a file path set, packing would try to read that (virtual) path from disk.
    image.pack(data=data, data_len=len(data))
    image.source = 'FILE'
    image.filepath_raw = "//angelica/" + path.replace("\\", "/")
    # Load from the packed bytes (`reload()` would first look for the file on disk).
    image.buffers_free()
    _set_source(image, client_root, path)
    image[PROP_FINGERPRINT] = hashlib.sha1(data).hexdigest()
    return image
