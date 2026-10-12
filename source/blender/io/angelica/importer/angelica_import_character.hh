/* SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup angelica
 *
 * Assembled characters: a body `.smd` with equipment skins on its skeleton, weapons, wings and
 * flying swords hung from its hooks, and the face on the head bone. This is how the client shows
 * a player (`CECPlayer::ShowEquipments`, `CECFace`), and what the character preview builds from
 * a real character of the user's account (the PWOps `/api/studio/appearance` description).
 */

#pragma once

#include <optional>
#include <string>

#include "BLI_math_matrix_types.hh"
#include "BLI_string_ref.hh"
#include "BLI_vector.hh"

#include "angelica_import_model.hh"

namespace blender {
struct Collection;
}

namespace blender::io::angelica {

/** A model hung from a hook of the body: weapon, wing, flying sword. */
struct CharacterAttachment {
  /** Skeleton of the attachment; weapons and wings are a `.bon` plus skins. */
  std::string bon_path;
  /** Some flying mounts only exist as a `.smd` (then #bon_path is empty). */
  std::string smd_path;
  Vector<std::string> skins;
  /** Animation of the attachment itself (wings flapping), optional. */
  std::string stck_path;
  /** Hook of the body (`HH_*`) and of the attachment (`CC_*`). */
  std::string hanger_hook;
  std::string child_hook;
  /** `right` or `left` for weapons: the hooks are chosen from the weapon's skeleton. */
  std::string weapon_side;
  /** Wings and flying swords: only visible while flying in the game. */
  bool flymount = false;
};

struct CharacterFace {
  std::string bon_path;
  Vector<std::string> skins;
};

struct CharacterSpec {
  std::string name;
  /** Body model. */
  std::string smd_path;
  /** Armor and fashion skins, drawn on the body skeleton. */
  Vector<std::string> skins;
  Vector<CharacterAttachment> attachments;
  std::optional<CharacterFace> face;
  /** Full suite glow effect (`.gfx`), played on `HH_Spine` in the game. Not imported yet. */
  std::string suite_gfx;
  /** Action to make active on the body (for example `站立`, standing), optional. */
  std::string clip;
};

/** Body hook names, from the client (`EC_Face.cpp`, `rig.js` weapon catalog). */
constexpr const char *CHARACTER_HEAD_BONE = "Bip01 Head";
/** `脖子对齐`: the neck bone of face skeletons, placed on #CHARACTER_HEAD_BONE. */
constexpr const char *CHARACTER_FACE_NECK_BONE = "\xe8\x84\x96\xe5\xad\x90\xe5\xaf\xb9\xe9\xbd\x90";

/**
 * Read the character description produced by PWOps (`buildCharacter`, JSON): `smdPath`,
 * `parts[]` (`{ski}` or a path), `attachments[]` (`bonPath`/`smdPath`, `parts`, `stckPath`,
 * `hh`, `cc`, `weapon`, `flymount`), `face` (`bonPath`, `parts`), `suiteGfx`, `clip`.
 * Returns false with a message when the JSON is not a character description.
 */
bool character_spec_from_json(StringRef json, CharacterSpec &r_spec, std::string *r_error);

/**
 * Hook names for a weapon, as the client picks them from the weapon's skeleton: `CC_weapon` on
 * `HH_righthandweapon` / `HH_lefthandweapon`, or an orbiting magic item (only `CC_qiu`) on
 * `HH_qiu`.
 */
void weapon_hooks(StringRef side, bool has_cc_weapon, bool has_cc_qiu, std::string &r_hanger, std::string &r_child);

/**
 * Where to put a child model's root so that its point `child_point` (model space) lands on the
 * parent's point `parent_point` (model space). Both are Blender matrices: `bone_rest * hook`.
 */
float4x4 attachment_root_matrix(const float4x4 &parent_point, const float4x4 &child_point);

/**
 * Import an assembled character into a new collection under `parent`. The collection is
 * registered as an asset. Returns null when the body cannot be read.
 */
Collection *import_character(ImportContext &ctx,
                             const CharacterSpec &spec,
                             const ModelOptions &options,
                             Collection *parent);

}  // namespace blender::io::angelica
