/* SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup angelica
 *
 * Every file the client loads for an asset: the closure of the references of a model, effect,
 * attack file or building, following the engine's own export collector (`GfxEditorUtil.cpp`,
 * `a_ExpCollect*Files` in the client sources) and the loaders' path rules:
 *
 * - `.ecm` (text): `SkinModelPath` (client root, or next to the `.ecm` for a bare file name),
 *   `AddiSkinPath` (under `Models\`), `ChildPath` (client root), `PhysFileName` (next to the
 *   `.ecm`), combined action events: effects `FxFilePath` under `Gfx\` (event type 100), sounds
 *   `FxFilePath` under `Sfx\` (101), attack points `AtkPath` under `Gfx\SkillAttack\` (104).
 * - `.smd`: skeleton, skins and tracks next to the `.smd` (`<dir>\<tracks dir>\<file>.stck`),
 *   optional `<smd>.phy` / `<smd>.stck`, effect hangers under `Gfx\`, child model hangers from the
 *   client root.
 * - `.ski`: textures by file name in `<dir>\Textures\`, then `<dir>\Tex_<ski title>\`; optional
 *   `<ski>.sphy`.
 * - `.gfx` (text): `TexFile` / `ShaderTex` under `Gfx\Textures\`, containers `GfxPath` under `Gfx\`,
 *   model elements `ModelPath` under `Gfx\Models\`, EC model elements `ECMPath` (client root),
 *   sound elements `Path` under `Sfx\`.
 * - `.att` / `.sgc` (text): the fly, hit and hit-ground effects (`Path`) under `Gfx\`.
 * - `.bmd`: the texture of every lit mesh (client root).
 */

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "angelica_vfs.hh"

namespace blender::io::angelica {

enum class DependencyKind : uint8_t {
  /** The file the collection started from. */
  Root,
  Model,
  SkinModel,
  Skin,
  Skeleton,
  Tracks,
  Texture,
  Effect,
  Sound,
  Attack,
  Physics,
  Building,
  Other,
};

/** Short English name of a kind ("texture", "skin"...), for scripts and messages. */
const char *dependency_kind_name(DependencyKind kind);

struct Dependency {
  /** Engine path (from the client root), normalized like #normalize_engine_path. */
  std::string path;
  DependencyKind kind = DependencyKind::Other;
  /** Index of the file that references this one in the result, -1 for the root. */
  int parent = -1;
  /** Where the client finds it; `location.kind == None` when the file is missing. */
  FileLocation location;
};

/**
 * Collect `engine_path` and everything it references, recursively. The root comes first and
 * every file appears once (the first reference wins). Missing files are listed too, with an
 * empty location, so callers can report broken references.
 */
std::vector<Dependency> collect_dependencies(ClientFileSystem &vfs, std::string_view engine_path);

}  // namespace blender::io::angelica
