# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""
Tests for writing Angelica packages (``_angelica.pck_*``), on copies of small client packages.

The smallest package of each client in ``GAMEFORGE_TEST_CLIENTS`` (folders separated by ``;``) is
copied to a temporary folder; only the copies are written. Without it the tests are skipped.
Run inside the Studio:
  GameForge3DStudio.exe -b --factory-startup --python tests/python/gameforge_pck_writer_test.py
"""

import hashlib
import os
import shutil
import sys
import tempfile
import unittest

import _angelica

CLIENTS = [p for p in os.environ.get("GAMEFORGE_TEST_CLIENTS", "").split(";") if p.strip()]
MAX_SIZE = 32 * 1024 * 1024
VERSION_2_3 = 0x00020003


def small_package(root):
    """The smallest package of a client with more than 1 MB, without ``.pkx`` parts."""
    candidates = []
    for name in os.listdir(root):
        path = os.path.join(root, name)
        if not name.lower().endswith(".pck") or not os.path.isfile(path):
            continue
        size = os.path.getsize(path)
        if 1024 * 1024 < size <= MAX_SIZE and not os.path.exists(path[:-4] + ".pkx"):
            candidates.append((size, path))
    return min(candidates)[1] if candidates else None


def sha1(data):
    return hashlib.sha1(data).hexdigest()


def file_sha1(path):
    with open(path, "rb") as fh:
        return sha1(fh.read())


def mount_of(pck):
    return os.path.splitext(os.path.basename(pck))[0]


def engine_path(pck, stored):
    """Engine path of a stored name (packages store names with or without their folder)."""
    mount = mount_of(pck)
    return stored if stored.lower().startswith(mount.lower() + "\\") else mount + "\\" + stored


def read_engine(pck, path):
    """Read an engine path from a package, the way the client file system looks it up."""
    data = _angelica.package_read(pck, path)
    prefix = mount_of(pck).lower() + "\\"
    if data is None and path.lower().startswith(prefix):
        data = _angelica.package_read(pck, path[len(prefix):])
    return data


def package_files(pck):
    """``{stored path: sha1 of the content}`` for every file of a package."""
    return {
        path.lower(): sha1(_angelica.package_read(pck, path))
        for path, _size, _stored in _angelica.package_entries(pck)
    }


def parts_of(pck):
    info = _angelica.package_info(pck)
    return list(zip(info["parts"], info["part_sizes"]))


@unittest.skipUnless(CLIENTS, "set GAMEFORGE_TEST_CLIENTS to the client folders to test")
class PackageWriterTest(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="gameforge-pck-test-")
        cls.packages = []
        for index, root in enumerate(CLIENTS):
            original = small_package(root)
            if original is None:
                continue
            folder = os.path.join(cls.tmp, "client{:d}".format(index))
            os.makedirs(folder)
            copy = os.path.join(folder, os.path.basename(original))
            shutil.copy2(original, copy)
            cls.packages.append(copy)
            info = _angelica.package_info(copy)
            print("{:#010x} {:>6d} files  {}".format(info["version"], info["entries"], original))

    @classmethod
    def tearDownClass(cls):
        _angelica._set_part_limit_for_tests(0)
        _angelica.release()
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def subfolder(self, pck, name):
        folder = os.path.join(os.path.dirname(pck), name)
        os.makedirs(folder, exist_ok=True)
        return os.path.join(folder, os.path.basename(pck))

    def test_update_in_place_and_undo(self):
        for pck in self.packages:
            with self.subTest(package=pck):
                whole_before = file_sha1(pck)
                before = package_files(pck)
                names = sorted(before)
                replaced, removed = names[0], names[-1]
                new_path = mount_of(pck) + "\\gameforge_teste\\novo_\u6d4b\u8bd5.txt"
                new_data = os.urandom(5000) + b"GameForge" * 1000
                result = _angelica.pck_update(pck, mount_of(pck), [
                    (engine_path(pck, replaced), b"conteudo novo"),
                    (engine_path(pck, removed), None),
                    (new_path, new_data),
                ])
                self.assertEqual((result["written"], result["added"], result["removed"]), (1, 1, 1))

                after = package_files(pck)
                self.assertEqual(read_engine(pck, engine_path(pck, replaced)), b"conteudo novo")
                self.assertIsNone(read_engine(pck, engine_path(pck, removed)))
                self.assertEqual(read_engine(pck, new_path), new_data)
                self.assertEqual(len(after), len(before))
                unchanged = set(before) - {replaced, removed}
                self.assertEqual({k: after[k] for k in unchanged}, {k: before[k] for k in unchanged})

                # Undo gives back the package byte for byte.
                self.assertTrue(_angelica.pck_can_undo(pck))
                _angelica.pck_undo(pck)
                self.assertFalse(_angelica.pck_can_undo(pck))
                self.assertEqual(file_sha1(pck), whole_before)
                self.assertFalse(os.path.exists(pck + ".gfjournal"))

    def test_several_updates_undo_in_order(self):
        for pck in self.packages:
            with self.subTest(package=pck):
                whole_before = file_sha1(pck)
                path = mount_of(pck) + "\\gameforge_teste\\versao.txt"
                states = []
                for version in range(3):
                    _angelica.pck_update(pck, mount_of(pck), [(path, b"versao %d" % version)])
                    states.append(file_sha1(pck))
                for version in reversed(range(3)):
                    self.assertEqual(file_sha1(pck), states[version])
                    self.assertEqual(read_engine(pck, path), b"versao %d" % version)
                    _angelica.pck_undo(pck)
                self.assertEqual(file_sha1(pck), whole_before)
                self.assertFalse(_angelica.pck_can_undo(pck))

    def test_refused_changes_leave_the_package(self):
        for pck in self.packages:
            with self.subTest(package=pck):
                whole_before = file_sha1(pck)
                mount = mount_of(pck)
                bad = [
                    [(mount + "\\a.txt", b"1"), (mount + "\\A.TXT", b"2")],  # Same file twice.
                    [(mount + "\\" + "x" * 300 + ".txt", b"1")],  # Name too long.
                    [(mount + "\\emoji_\U0001f600.txt", b"1")],  # Not representable in GBK.
                ]
                for changes in bad:
                    with self.assertRaises(OSError):
                        _angelica.pck_update(pck, mount, changes)
                # Another program has the package open (the game client).
                with open(pck, "rb"):
                    with self.assertRaises(OSError) as raised:
                        _angelica.pck_update(pck, mount, [(mount + "\\a.txt", b"1")])
                    self.assertIn("another program", str(raised.exception))
                self.assertEqual(file_sha1(pck), whole_before)
                self.assertFalse(_angelica.pck_can_undo(pck))

    def test_copy_and_patch(self):
        for pck in self.packages:
            with self.subTest(package=pck):
                whole_before = file_sha1(pck)
                before = package_files(pck)
                names = sorted(before)
                info = _angelica.package_info(pck)

                copy = self.subfolder(pck, "copia")
                _angelica.pck_write_copy(pck, mount_of(pck), copy, [
                    (engine_path(pck, names[0]), b"copia"),
                    (engine_path(pck, names[1]), None),
                ])
                expected = dict(before)
                expected[names[0]] = sha1(b"copia")
                del expected[names[1]]
                self.assertEqual(package_files(copy), expected)
                copy_info = _angelica.package_info(copy)
                for key in ("version", "description", "algorithm_id"):
                    self.assertEqual(copy_info[key], info[key], key)

                patch = self.subfolder(pck, "patch")
                _angelica.pck_write_patch(pck, mount_of(pck), patch, [
                    (engine_path(pck, names[0]), b"patch"),
                ])
                self.assertEqual(len(_angelica.package_entries(patch)), 1)
                self.assertEqual(read_engine(patch, engine_path(pck, names[0])), b"patch")
                self.assertEqual(_angelica.package_info(patch)["version"], info["version"])
                # The source is only read.
                self.assertEqual(file_sha1(pck), whole_before)

    def test_packages_split_in_parts(self):
        _angelica._set_part_limit_for_tests(1024 * 1024)
        try:
            for pck in self.packages:
                with self.subTest(package=pck):
                    before = package_files(pck)
                    split = self.subfolder(pck, "partes")
                    _angelica.pck_write_copy(pck, mount_of(pck), split, [])
                    parts_before = parts_of(split)
                    self.assertGreater(len(parts_before), 1)
                    self.assertEqual(package_files(split), before)

                    # Incompressible data larger than a part.
                    big = os.urandom(1536 * 1024)
                    path = mount_of(pck) + "\\gameforge_teste\\grande.bin"
                    _angelica.pck_update(split, mount_of(pck), [(path, big)])
                    self.assertEqual(read_engine(split, path), big)
                    parts_after = parts_of(split)
                    if _angelica.package_info(split)["version"] == VERSION_2_3:
                        self.assertGreater(len(parts_after), len(parts_before))
                    self.assertGreater(sum(s for _p, s in parts_after), sum(s for _p, s in parts_before))

                    _angelica.pck_undo(split)
                    self.assertEqual(parts_of(split), parts_before)
                    self.assertEqual(package_files(split), before)
        finally:
            _angelica._set_part_limit_for_tests(0)


if __name__ == "__main__":
    sys.argv = [__file__] + (sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])
    result = unittest.main(exit=False).result
    sys.exit(0 if result.wasSuccessful() else 1)
