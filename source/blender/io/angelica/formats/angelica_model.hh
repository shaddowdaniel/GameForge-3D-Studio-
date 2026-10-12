/* SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup angelica
 *
 * Parsers for the Angelica Engine model description files:
 * - `.smd` skin models (`A3DSkinModel`): skeleton, skins, physique and actions.
 * - `.ecm` element client models (`CECModelStaticData` in GfxCommon): the `.smd` plus additional
 *   skins, child models and hooks. Only the parts needed to assemble the model are read; combined
 *   actions, effects and scripts are skipped.
 *
 * File references are stored as written (UTF-8 plus raw GBK). How the engine resolves them is
 * documented on each member.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "angelica_reader.hh"

namespace blender::io::angelica {

/* -------------------------------------------------------------------- */
/** \name Skin Model (.smd)
 * \{ */

constexpr uint32_t SKIN_MODEL_FILE_IDENTIFY = fourcc('A', 'S', 'M', 'D');
/** Versions 1..9 (1.5.5 writes 8, Angelica 2.2 writes 9). */
constexpr uint32_t SKIN_MODEL_FILE_VERSION = 9;

struct SkinModelAction {
  std::string name;
  float start_frame = 0.0f;
  float end_frame = 0.0f;
  /** Version >= 9: action frame rate, 0 = use the skeleton's frame rate. */
  int frame_rate = 0;
  /**
   * Version >= 7: `.stck` file name as written (a file name with extension). The engine loads
   * `<smd dir>\<tracks dir>\<name>`. Empty for older versions whose tracks live in the `.bon`
   * file.
   */
  std::string track_file;
  std::string track_file_raw;
};

enum class SkinModelHangerType : int32_t {
  Gfx = 0,
  SkinModel = 1,
};

/** `A3DHanger`: an effect or a child skin model attached to a hook or bone. */
struct SkinModelHanger {
  SkinModelHangerType type = SkinModelHangerType::Gfx;
  std::string name;
  /** Hook name, or bone name when #on_bone is set. */
  std::string bind_to;
  bool on_bone = false;
  /** Effect file (`.gfx`) or child model file (`.smd`), engine relative. */
  std::string file;
  std::string file_raw;
  /** Child models: hook (`CC`) of the child that is attached to #bind_to. */
  std::string child_hook;
};

struct SkinModel {
  uint32_t version = 0;
  /**
   * Skeleton file (`.bon`), skin files (`.ski`, empty entries are kept) and physique file
   * (`.phy`). These are names relative to the `.smd` file's directory (the engine prepends it).
   */
  std::string skeleton_file;
  std::string skeleton_file_raw;
  std::vector<std::string> skin_files;
  std::vector<std::string> skin_files_raw;
  std::string physique_file;
  std::string physique_file_raw;
  /**
   * Directory of the action tracks relative to the `.smd` directory. Version < 8 derives it as
   * `tcks_<smd title>`, newer versions store it (it may be empty: tracks next to the `.smd`).
   */
  std::string tracks_dir;
  std::string tracks_dir_raw;
  std::vector<SkinModelAction> actions;
  std::vector<SkinModelHanger> hangers;
  /** User properties (name, value). */
  std::vector<std::pair<std::string, std::string>> properties;
};

/**
 * Parse a `.smd` file. `file_name` is the model's file name (used to derive the track directory
 * of version < 8 files, may be empty).
 */
bool parse_skin_model(ByteSpan data,
                      SkinModel &r_model,
                      std::string *r_error,
                      std::string_view file_name = {});

/** \} */

/* -------------------------------------------------------------------- */
/** \name EC Model (.ecm)
 * \{ */

/** Highest version accepted by both the 1.5.5 and the Angelica 2.2 clients. */
constexpr uint32_t EC_MODEL_FILE_VERSION = 71;

struct ECModelChild {
  /** Name the parent model uses for the child. */
  std::string name;
  /** Child `.ecm` file, engine relative (from the client root). */
  std::string path;
  std::string path_raw;
  /** Hook of the parent ("hanger hook") the child is attached to. */
  std::string parent_hook;
  /** Hook of the child ("CC") that is placed on #parent_hook. */
  std::string child_hook;
};

/** `CECModelHook`: an extra hook defined by the `.ecm`. */
struct ECModelHook {
  std::string name;
  int id = 0;
  float scale = 1.0f;
  /** Row-major transform (rows 0..2 axes, row 3 translation). */
  float matrix[16];
};

struct ECModelBoneScale {
  int bone_index = -1;
  /** Old method (`NewScale: 0`): scale type and scale vector. */
  int type = 0;
  float scale[3] = {1.0f, 1.0f, 1.0f};
  /** New method: length, thickness and whole factors (stored in #scale). */
  bool new_method = false;
};

struct ECModel {
  uint32_t version = 0;
  bool is_text = true;
  /**
   * The `.smd` file. Normally engine relative from the client root; when it contains no path
   * separator the engine looks for it next to the `.ecm` file.
   */
  std::string skin_model_path;
  std::string skin_model_path_raw;
  bool auto_update = true;
  /** `A3DCOLOR` ARGB. */
  uint32_t org_color = 0xffffffff;
  uint32_t emissive_color = 0;
  int src_blend = 5;
  int dest_blend = 6;
  float default_play_speed = 1.0f;
  bool can_cast_shadow = true;
  bool render_model = true;
  bool render_edge = true;
  std::vector<ECModelBoneScale> bone_scales;
  /** Additional skins (`.ski`), engine relative. */
  std::vector<std::string> additional_skins;
  std::vector<std::string> additional_skins_raw;
  std::vector<ECModelChild> children;
  /** PhysX description file, relative to the `.ecm` directory. */
  std::string physics_file;
  std::vector<ECModelHook> hooks;
  /**
   * False when the child / skin list could not be located (binary `.ecm` with combined actions
   * or effects, which this parser does not decode).
   */
  bool children_parsed = false;
};

/** Parse a `.ecm` file (text, or binary as far as described above). */
bool parse_ec_model(ByteSpan data, ECModel &r_model, std::string *r_error);

/** \} */

}  // namespace blender::io::angelica
