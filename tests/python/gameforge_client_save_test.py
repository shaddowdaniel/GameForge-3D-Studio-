# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Tests for saving edited client files (``gameforge_angelica.save`` and ``textures``), on a copy of
a small package of a real client: texts and textures are opened from the package, edited, saved in
the three modes and undone.

The first client of ``GAMEFORGE_TEST_CLIENTS`` (folders separated by ``;``) whose smallest
package has texts and DDS textures is used; only a copy is written. Run inside the Studio:
  GameForge3DStudio.exe -b --factory-startup --python tests/python/gameforge_client_save_test.py
"""

import hashlib
import os
import shutil
import struct
import sys
import tempfile
import unittest

import bpy
import numpy as np

import _angelica
from gameforge_angelica import save, sources, textures

CLIENTS = [p for p in os.environ.get("GAMEFORGE_TEST_CLIENTS", "").split(";") if p.strip()]
TEXT_EXTS = (".txt", ".ini", ".xml", ".stf", ".dtf", ".htf", ".gfx", ".lua", ".cfg")
MAX_SIZE = 40 * 1024 * 1024


def file_sha1(path):
    with open(path, "rb") as fh:
        return hashlib.sha1(fh.read()).hexdigest()


def dds_format(data):
    flags = struct.unpack_from("<I", data, 80)[0]
    fourcc = data[84:88] if flags & 0x4 else b"RGB%d" % struct.unpack_from("<I", data, 88)[0]
    height, width = struct.unpack_from("<II", data, 12)
    mips = struct.unpack_from("<I", data, 28)[0]
    return fourcc, width, height, mips


def load_pixels(data, name):
    """Decode image bytes with the Studio's own image loading (what the user sees)."""
    image = bpy.data.images.new(name, 1, 1)
    image.pack(data=data, data_len=len(data))
    image.source = 'FILE'
    image.filepath_raw = "//angelica/" + name
    image.buffers_free()
    rgba = textures.image_rgba8(image)
    bpy.data.images.remove(image)
    return rgba


def find_test_package():
    """A small package with text files and DDS textures, and the client it belongs to."""
    for root in CLIENTS:
        candidates = []
        for name in os.listdir(root):
            path = os.path.join(root, name)
            if name.lower().endswith(".pck") and os.path.getsize(path) <= MAX_SIZE and \
                    not os.path.exists(path[:-4] + ".pkx"):
                candidates.append((os.path.getsize(path), path))
        for _size, pck in sorted(candidates):
            names = [p for p, size, _s in _angelica.package_entries(pck) if 0 < size < 4 * 1024 * 1024]
            if any(n.lower().endswith(TEXT_EXTS) for n in names) and \
                    any(n.lower().endswith(".dds") for n in names):
                return root, pck
    return None, None


@unittest.skipUnless(CLIENTS, "set GAMEFORGE_TEST_CLIENTS to the client folders to test")
class ClientSaveTest(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="gameforge-save-test-")
        original_root, original_pck = find_test_package()
        if original_pck is None:
            raise unittest.SkipTest("no small package with texts and DDS textures")
        cls.root = os.path.join(cls.tmp, "element")
        os.makedirs(cls.root)
        cls.pck = os.path.join(cls.root, os.path.basename(original_pck))
        shutil.copy2(original_pck, cls.pck)
        cls.mount = os.path.splitext(os.path.basename(cls.pck))[0]
        print("package:", original_pck)
        # Keep the save history out of the user's configuration.
        cls.config = os.path.join(cls.tmp, "config")
        cls._config_dir = save._config_dir
        save._config_dir = lambda: cls.config

        def engine(path):
            return path if path.lower().startswith(cls.mount.lower() + "\\") else cls.mount + "\\" + path
        entries = [(engine(p), size) for p, size, _s in _angelica.package_entries(cls.pck)]
        cls.texts = [p for p, size in entries if p.lower().endswith(TEXT_EXTS) and 0 < size < 256 * 1024]
        cls.ddss = [p for p, size in entries if p.lower().endswith(".dds") and 0 < size < 4 * 1024 * 1024]

    @classmethod
    def tearDownClass(cls):
        save._config_dir = cls._config_dir
        _angelica.release()
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def setUp(self):
        for text in list(bpy.data.texts):
            bpy.data.texts.remove(text)
        for image in list(bpy.data.images):
            bpy.data.images.remove(image)

    def test_dds_reencode_keeps_format_and_pixels(self):
        """Every DDS format of the package survives decode, encode, decode."""
        seen = {}
        for path in self.ddss:
            data = _angelica.read(self.root, path)
            fourcc, _w, _h, _mips = dds_format(data)
            if fourcc not in seen:
                seen[fourcc] = (path, data)
        self.assertTrue(seen)
        for fourcc, (path, data) in seen.items():
            with self.subTest(format=fourcc, path=path):
                rgba = load_pixels(data, "a.dds")
                encoded = textures.encode_like(path, data, rgba)
                self.assertEqual(dds_format(encoded), dds_format(data))
                self.assertEqual(encoded[:4 + 76], data[:4 + 76])  # Header up to the pixel format.
                decoded = load_pixels(encoded, "b.dds")
                error = np.abs(decoded.astype(np.int16) - rgba.astype(np.int16)).mean()
                print("  {:<8s} {:>5d}x{:<5d} mean error {:.2f}  {}".format(
                    fourcc.decode(), rgba.shape[1], rgba.shape[0], error, path))
                self.assertLess(error, 4.0)

    def test_text_in_place_and_undo(self):
        path = self.texts[0]
        whole_before = file_sha1(self.pck)
        text = sources.open_text(self.root, path)
        self.assertEqual(save.pending_changes(), [])
        text.write("\n-- editado no GameForge 3D Studio\n")
        changes = save.pending_changes()
        self.assertEqual([c.path for c in changes], [path])

        save.save(changes, 'IN_PLACE')
        saved = _angelica.read(self.root, path)
        self.assertEqual(saved, sources.encode_text(text.as_string(), text["gameforge_encoding"],
                                                    text["gameforge_newline"]))
        self.assertEqual(save.pending_changes(), [])
        self.assertNotEqual(file_sha1(self.pck), whole_before)

        self.assertTrue(save.can_undo())
        self.assertEqual(save.undo_last_save(), [])
        self.assertEqual(file_sha1(self.pck), whole_before)
        # The edit is back to pending: the client has the original again.
        self.assertEqual([c.path for c in save.pending_changes()], [path])

    def test_image_in_place_patch_and_copy(self):
        path = next(p for p in self.ddss
                    if dds_format(_angelica.read(self.root, p))[0] in (b"DXT1", b"DXT3", b"DXT5"))
        whole_before = file_sha1(self.pck)
        original = _angelica.read(self.root, path)
        image = sources.open_image(self.root, path)
        width, height = image.size
        pixels = np.empty(width * height * 4, dtype=np.float32)
        image.pixels.foreach_get(pixels)
        pixels[0::4] = 1.0 - pixels[0::4]  # Invert red.
        image.pixels.foreach_set(pixels)
        image.update()
        changes = save.pending_changes()
        self.assertEqual([c.path for c in changes], [path])

        # A patch package with only the texture, and a full copy, in other folders.
        out_patch = os.path.join(self.tmp, "patch")
        out_copy = os.path.join(self.tmp, "copia")
        save.save(changes, 'PATCH', out_patch)
        save.save(changes, 'COPY', out_copy)
        patch_pck = os.path.join(out_patch, os.path.basename(self.pck))
        copy_pck = os.path.join(out_copy, os.path.basename(self.pck))
        self.assertEqual(len(_angelica.package_entries(patch_pck)), 1)
        self.assertEqual(len(_angelica.package_entries(copy_pck)), len(_angelica.package_entries(self.pck)))
        self.assertEqual(file_sha1(self.pck), whole_before)
        self.assertEqual(len(save.pending_changes()), 1)  # Still pending for the client itself.

        save.save(save.pending_changes(), 'IN_PLACE')
        saved = _angelica.read(self.root, path)
        self.assertEqual(dds_format(saved), dds_format(original))
        saved_rgba = load_pixels(saved, "c.dds")
        original_rgba = load_pixels(original, "d.dds")
        # Red inverted, the rest close to the original.
        self.assertGreater(np.abs(saved_rgba[..., 0].astype(np.int16) - original_rgba[..., 0]).mean(), 20)
        self.assertLess(np.abs(saved_rgba[..., 1].astype(np.int16) - original_rgba[..., 1]).mean(), 4)
        self.assertEqual(save.pending_changes(), [])
        self.assertEqual(_angelica.package_read(copy_pck, path) or
                         _angelica.package_read(copy_pck, path.split("\\", 1)[1]), saved)

        self.assertEqual(save.undo_last_save(), [])
        self.assertEqual(file_sha1(self.pck), whole_before)


if __name__ == "__main__":
    sys.argv = [__file__] + (sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])
    result = unittest.main(exit=False).result
    sys.exit(0 if result.wasSuccessful() else 1)
