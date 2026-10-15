/* SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup angelica
 *
 * Writing Angelica file packages (`.pck` + `.pkx` parts), in the layout of an existing package
 * (same version, keys, description and safe header style), so the client reads the result like
 * the original. See `angelica_pck.cc` for the on-disk layout.
 *
 * Three ways to save edited files:
 * - #pck_update_in_place: the official engine's way (`AFilePackage::ReplaceFile` + `Close`). New
 *   data is appended after the package's logical end without touching live bytes, then the full
 *   file table and trailer, and only then the safe header at offset 0 is rewritten: the client
 *   only trusts what that header points to, so an interrupted save leaves the old package intact.
 *   The previous header and part sizes go to a journal next to the package
 *   (`<package>.gfjournal`), which #pck_undo_last_update uses to restore it.
 * - #pck_write_patch: a new package with only the given files (a patch to apply later; the client
 *   does not load extra packages by itself).
 * - #pck_write_copy: a full copy of a package with the files replaced, written to a new location.
 *
 * Every function verifies the result by reading it back. Packages are never written while
 * #PckArchive objects of the shared cache use them (#release_package_shared is called first).
 */

#pragma once

#include <string>
#include <vector>

#include "angelica_reader.hh"

namespace blender::io::angelica {

/** A file to store in a package, or to remove from it. */
struct PckFileChange {
  /**
   * Engine path of the file (`models\npc\x.ski`), case and separators do not matter. Paths are
   * stored the way the package already stores them (with or without its mount folder).
   */
  std::string path;
  /** New content, ignored when #remove is set. */
  Bytes data;
  bool remove = false;
};

struct PckWriteResult {
  /** Files written (replaced or added) and removed. */
  int written = 0;
  int added = 0;
  int removed = 0;
  /** Logical size of the package after the write. */
  uint64_t size = 0;
};

/**
 * Store `changes` into the package `pck_path` itself (see the file description). `mount` is the
 * package's folder in the client (`models`, `help\script`), used to store new paths like the
 * existing ones. Fails without changing the package when it cannot be done safely (no safe header,
 * encrypted package, size limit of the package version).
 */
bool pck_update_in_place(const std::string &pck_path,
                         const std::string &mount,
                         const std::vector<PckFileChange> &changes,
                         PckWriteResult *r_result,
                         std::string *r_error);

/** Whether the journal of a package can restore the state before its last in-place update. */
bool pck_can_undo(const std::string &pck_path);

/** Restore the package as it was before the last #pck_update_in_place. */
bool pck_undo_last_update(const std::string &pck_path, std::string *r_error);

/**
 * Write a new package at `dst_pck` holding only `changes` (removals are ignored), laid out like
 * `like_pck`. Existing files at the destination are replaced.
 */
bool pck_write_patch(const std::string &like_pck,
                     const std::string &mount,
                     const std::string &dst_pck,
                     const std::vector<PckFileChange> &changes,
                     PckWriteResult *r_result,
                     std::string *r_error);

/**
 * Write a full copy of `src_pck` (all its parts) at `dst_pck` with `changes` applied. Unchanged
 * files are copied as stored. `dst_pck` must not be the source package.
 */
bool pck_write_copy(const std::string &src_pck,
                    const std::string &mount,
                    const std::string &dst_pck,
                    const std::vector<PckFileChange> &changes,
                    PckWriteResult *r_result,
                    std::string *r_error);

/**
 * Testing: the largest size of each part file (index 0 is the `.pck`). Zero restores the engine's
 * limits (0x7FFFFF00 for the `.pck`, 0xFFFFFE00 for the parts of 64-bit clients).
 */
void pck_writer_set_part_limit_for_tests(uint64_t limit);

}  // namespace blender::io::angelica
