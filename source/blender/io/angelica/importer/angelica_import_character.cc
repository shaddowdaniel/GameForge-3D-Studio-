/* SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup angelica
 *
 * Assembling a player character the way the client does:
 * - body `.smd`, with armor and fashion skins bound to the same skeleton
 *   (`CECPlayer::ShowEquipments` / `UpdateCurSkins`);
 * - weapons, wings and flying swords: their own skeleton and skins, with the child hook (`CC_*`)
 *   placed on the body hook (`HH_*`), following that bone (`CECModel::AddChildModel`);
 * - the face: its own skeleton, with the neck bone placed on the body's head bone
 *   (`CECFace::Tick`, `EC_Face.cpp`).
 *
 * Math (Blender space): a hook's model space transform is `bone_rest * hook`. The child root goes
 * to `parent_point * inverse(child_point)`, then is parented to the body bone with a parent
 * inverse that keeps it there in the rest pose, so it follows the animation from then on.
 */

#include <algorithm>
#include <cctype>
#include <cstring>
#include <sstream>

#include "ANIM_action.hh"

#include "BKE_anim_data.hh"
#include "BKE_armature.hh"
#include "BKE_collection.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_object.hh"

#include "BLI_math_matrix.hh"
#include "BLI_serialize.hh"
#include "BLI_string.hh"

#include "DNA_action_types.h"
#include "DNA_armature_types.h"
#include "DNA_collection_types.h"
#include "DNA_object_types.h"

#include "angelica_import_character.hh"
#include "angelica_import_common.hh"
#include "angelica_model.hh"
#include "angelica_reader.hh"
#include "angelica_skeleton.hh"
#include "angelica_vfs.hh"

namespace blender::io::angelica {

/* -------------------------------------------------------------------- */
/** \name Character Description (JSON)
 * \{ */

namespace serialize = io::serialize;

static std::string json_string(const serialize::DictionaryValue &dict, StringRef key)
{
  const std::optional<StringRefNull> value = dict.lookup_str(key);
  return value ? std::string(*value) : std::string();
}

/** A skin entry is either a path or an object with `ski`. */
static void json_skins(const serialize::ArrayValue *array, Vector<std::string> &r_skins)
{
  if (array == nullptr) {
    return;
  }
  for (const std::shared_ptr<serialize::Value> &item : array->elements()) {
    if (const serialize::StringValue *str = item->as_string_value()) {
      if (!str->value().empty()) {
        r_skins.append(normalize_engine_path(str->value(), false));
      }
    }
    else if (const serialize::DictionaryValue *dict = item->as_dictionary_value()) {
      const std::string ski = json_string(*dict, "ski");
      if (!ski.empty()) {
        r_skins.append(normalize_engine_path(ski, false));
      }
    }
  }
}

static std::string json_path(const serialize::DictionaryValue &dict, StringRef key)
{
  const std::string value = json_string(dict, key);
  return value.empty() ? value : normalize_engine_path(value, false);
}

bool character_spec_from_json(StringRef json, CharacterSpec &r_spec, std::string *r_error)
{
  auto fail = [&](const char *message) {
    if (r_error) {
      *r_error = message;
    }
    return false;
  };
  serialize::JsonFormatter formatter;
  std::istringstream stream{std::string(json)};
  const std::unique_ptr<serialize::Value> root = formatter.deserialize(stream);
  if (!root) {
    return fail("invalid JSON");
  }
  const serialize::DictionaryValue *dict = root->as_dictionary_value();
  if (dict == nullptr) {
    return fail("the character description is not a JSON object");
  }
  /* The PWOps answer wraps it: `{"ok": true, "character": {...}}`. */
  if (const serialize::DictionaryValue *inner = dict->lookup_dict("character")) {
    dict = inner;
  }

  r_spec = {};
  r_spec.name = json_string(*dict, "name");
  r_spec.smd_path = json_path(*dict, "smdPath");
  if (r_spec.smd_path.empty()) {
    return fail("the character has no body model (smdPath)");
  }
  r_spec.clip = json_string(*dict, "clip");
  json_skins(dict->lookup_array("parts"), r_spec.skins);

  if (const serialize::ArrayValue *attachments = dict->lookup_array("attachments")) {
    for (const std::shared_ptr<serialize::Value> &item : attachments->elements()) {
      const serialize::DictionaryValue *att = item->as_dictionary_value();
      if (att == nullptr) {
        continue;
      }
      CharacterAttachment a;
      a.bon_path = json_path(*att, "bonPath");
      a.smd_path = json_path(*att, "smdPath");
      a.stck_path = json_path(*att, "stckPath");
      a.hanger_hook = json_string(*att, "hh");
      a.child_hook = json_string(*att, "cc");
      a.weapon_side = json_string(*att, "weapon");
      a.flymount = att->lookup_bool("flymount").value_or(false);
      json_skins(att->lookup_array("parts"), a.skins);
      if (a.bon_path.empty() && a.smd_path.empty()) {
        continue;
      }
      r_spec.attachments.append(std::move(a));
    }
  }

  if (const serialize::DictionaryValue *face = dict->lookup_dict("face")) {
    CharacterFace f;
    f.bon_path = json_path(*face, "bonPath");
    json_skins(face->lookup_array("parts"), f.skins);
    if (!f.bon_path.empty()) {
      r_spec.face = std::move(f);
    }
  }

  if (const serialize::DictionaryValue *suite = dict->lookup_dict("suiteGfx")) {
    r_spec.suite_gfx = json_string(*suite, "gfx");
    if (r_spec.suite_gfx.empty()) {
      r_spec.suite_gfx = json_string(*suite, "file_gfx");
    }
  }
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Hooks
 * \{ */

void weapon_hooks(const StringRef side,
                  const bool has_cc_weapon,
                  const bool has_cc_qiu,
                  std::string &r_hanger,
                  std::string &r_child)
{
  if (side != "left" && !has_cc_weapon && has_cc_qiu) {
    r_hanger = "HH_qiu";
    r_child = "CC_qiu";
    return;
  }
  r_hanger = side == "left" ? "HH_lefthandweapon" : "HH_righthandweapon";
  r_child = "CC_weapon";
}

float4x4 attachment_root_matrix(const float4x4 &parent_point, const float4x4 &child_point)
{
  return parent_point * math::invert(child_point);
}

static bool iequals(const StringRef a, const StringRef b)
{
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
           return std::tolower((unsigned char)x) == std::tolower((unsigned char)y);
         });
}

/** Hooks are matched without case: the client asks for `HH_RighthandWeapon`, files store lower. */
static const SkeletonHook *find_hook(const Skeleton &skeleton, const StringRef name)
{
  for (const SkeletonHook &hook : skeleton.hooks) {
    if (iequals(hook.name, name)) {
      return &hook;
    }
  }
  return nullptr;
}

/** Model space rest transform of a bone (Blender space). */
static float4x4 bone_rest(const Skeleton &skeleton, const int bone, const float scale)
{
  if (bone < 0 || bone >= int(skeleton.bones.size())) {
    return float4x4::identity();
  }
  return math::invert(engine_matrix_to_blender(skeleton.bones[bone].bone_init, scale));
}

/** Model space transform of a hook (Blender space). */
static float4x4 hook_point(const Skeleton &skeleton, const SkeletonHook &hook, const float scale)
{
  return bone_rest(skeleton, hook.bone, scale) * engine_matrix_to_blender(hook.matrix, scale);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Assembly
 * \{ */

static std::string dirname(const std::string &path)
{
  const size_t slash = path.find_last_of("\\/");
  return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

static bool read_skeleton(ImportContext &ctx,
                          const std::string &path,
                          const std::string &base_dir,
                          Skeleton &r_skeleton)
{
  Bytes data;
  if (!ctx.vfs.read(path, data, base_dir)) {
    return false;
  }
  std::string error;
  return parse_skeleton(data, r_skeleton, &error);
}

/** The skeleton a `.smd` uses. */
static bool read_smd_skeleton(ImportContext &ctx, const std::string &smd_path, Skeleton &r_skeleton)
{
  Bytes data;
  if (!ctx.vfs.read(smd_path, data)) {
    return false;
  }
  SkinModel model;
  std::string error;
  if (!parse_skin_model(data, model, &error, path_basename(smd_path)) ||
      model.skeleton_file.empty())
  {
    return false;
  }
  return read_skeleton(ctx, model.skeleton_file, dirname(smd_path), r_skeleton) ||
         read_skeleton(ctx, path_basename(model.skeleton_file), dirname(smd_path), r_skeleton);
}

/**
 * Place `child` (an armature object) at `root` and make it follow `bone_index` of `body`.
 * The parent inverse keeps the child where it is in the rest pose.
 */
static void parent_to_bone(Object *child,
                           Object *body,
                           const Skeleton &body_skeleton,
                           const int bone_index,
                           const float4x4 &root)
{
  float root_m[4][4];
  std::memcpy(root_m, root.ptr(), sizeof(root_m));
  BKE_object_apply_mat4(child, root_m, false, false);

  child->parent = body;
  float4x4 parent_mat = body->object_to_world();
  Bone *bone = nullptr;
  if (bone_index >= 0 && bone_index < int(body_skeleton.bones.size())) {
    bArmature *arm = reinterpret_cast<bArmature *>(body->data);
    bone = BKE_armature_find_bone_name(arm, body_skeleton.bones[bone_index].name.c_str());
  }
  if (bone) {
    child->partype = PARBONE;
    STRNCPY(child->parsubstr, bone->name);
    /* A bone parent is the bone's tail in pose space (`BKE_object_get_parent_matrix`). */
    parent_mat = parent_mat * float4x4(bone->arm_mat) *
                 math::from_location<float4x4>(float3(0.0f, bone->length, 0.0f));
  }
  else {
    child->partype = PAROBJECT;
  }
  const float4x4 parent_inv = math::invert(parent_mat);
  std::memcpy(child->parentinv, parent_inv.ptr(), sizeof(child->parentinv));
}

static void hang_attachment(ImportContext &ctx,
                            const CharacterAttachment &att,
                            const ModelOptions &options,
                            Collection *collection,
                            Object *body,
                            const Skeleton &body_skeleton)
{
  const std::string label = !att.bon_path.empty() ? att.bon_path : att.smd_path;
  /* Wings and flying swords only show while flying in the game: their collection says so. */
  const std::string sub_name = (att.flymount ? "Voo " : "") + name_from_engine_path(label);
  Collection *sub = BKE_collection_add(ctx.bmain, collection, sub_name.c_str());

  Object *armature = nullptr;
  Skeleton skeleton;
  bool has_skeleton = false;
  if (!att.smd_path.empty() && att.bon_path.empty()) {
    armature = import_smd_with_skins(ctx, att.smd_path, att.skins, options, sub);
    has_skeleton = read_smd_skeleton(ctx, att.smd_path, skeleton);
  }
  else {
    armature = import_bon_with_skins(ctx, att.bon_path, att.skins, att.stck_path, options, sub);
    has_skeleton = read_skeleton(ctx, att.bon_path, "", skeleton);
  }
  if (armature == nullptr || !has_skeleton) {
    ctx.warn("Angelica: attachment '" + label + "' could not be read");
    return;
  }

  std::string hanger = att.hanger_hook;
  std::string child = att.child_hook;
  if (!att.weapon_side.empty()) {
    weapon_hooks(att.weapon_side,
                 find_hook(skeleton, "CC_weapon") != nullptr,
                 find_hook(skeleton, "CC_qiu") != nullptr,
                 hanger,
                 child);
  }
  const SkeletonHook *hh = find_hook(body_skeleton, hanger);
  const SkeletonHook *cc = find_hook(skeleton, child);
  if (hh == nullptr || cc == nullptr) {
    /* The client does not show it either; keep it in the file, at the origin, and say why. */
    ctx.warn("Angelica: hook " + hanger + "/" + child + " not found for '" + label + "'");
    return;
  }
  const float4x4 root = attachment_root_matrix(hook_point(body_skeleton, *hh, ctx.scale),
                                               hook_point(skeleton, *cc, ctx.scale));
  parent_to_bone(armature, body, body_skeleton, hh->bone, root);
}

static void attach_face(ImportContext &ctx,
                        const CharacterFace &face,
                        const ModelOptions &options,
                        Collection *collection,
                        Object *body,
                        const Skeleton &body_skeleton)
{
  Collection *sub = BKE_collection_add(ctx.bmain, collection, "Rosto");
  ModelOptions face_options = options;
  face_options.import_animations = false;
  Object *armature = import_bon_with_skins(ctx, face.bon_path, face.skins, "", face_options, sub);
  Skeleton skeleton;
  if (armature == nullptr || !read_skeleton(ctx, face.bon_path, "", skeleton)) {
    ctx.warn("Angelica: face '" + face.bon_path + "' could not be read");
    return;
  }
  const int head = body_skeleton.find_bone(CHARACTER_HEAD_BONE);
  const int neck = skeleton.find_bone(CHARACTER_FACE_NECK_BONE);
  if (head < 0 || neck < 0) {
    ctx.warn("Angelica: head or neck bone not found, the face stays at the origin");
    return;
  }
  const float4x4 root = attachment_root_matrix(bone_rest(body_skeleton, head, ctx.scale),
                                               bone_rest(skeleton, neck, ctx.scale));
  parent_to_bone(armature, body, body_skeleton, head, root);
}

/** Make the action `<model>|<clip>` the one playing on the body. */
static void play_clip(ImportContext &ctx, Object *body, const std::string &smd_path, const std::string &clip)
{
  const std::string name = name_from_engine_path(smd_path) + "|" + clip;
  ID *id = BKE_libblock_find_name(ctx.bmain, ID_AC, name.c_str());
  if (id == nullptr) {
    return;
  }
  animrig::Action &action = reinterpret_cast<bAction *>(id)->wrap();
  if (animrig::assign_action(&action, body->id) && !action.slots().is_empty()) {
    (void)animrig::assign_action_slot(action.slot(0), body->id);
  }
}

Collection *import_character(ImportContext &ctx,
                             const CharacterSpec &spec,
                             const ModelOptions &options,
                             Collection *parent)
{
  const std::string name = spec.name.empty() ? name_from_engine_path(spec.smd_path) : spec.name;
  Collection *collection = BKE_collection_add(ctx.bmain, parent, name.c_str());

  ModelOptions body_options = options;
  body_options.import_armature = true;
  Object *body = import_smd_with_skins(ctx, spec.smd_path, spec.skins, body_options, collection);
  Skeleton body_skeleton;
  if (body == nullptr || !read_smd_skeleton(ctx, spec.smd_path, body_skeleton)) {
    ctx.error("Angelica: the body '" + spec.smd_path + "' could not be read");
    BKE_id_delete(ctx.bmain, &collection->id);
    return nullptr;
  }

  for (const CharacterAttachment &att : spec.attachments) {
    hang_attachment(ctx, att, body_options, collection, body, body_skeleton);
  }
  if (spec.face) {
    attach_face(ctx, *spec.face, body_options, collection, body, body_skeleton);
  }
  if (!spec.clip.empty() && options.import_animations) {
    play_clip(ctx, body, spec.smd_path, spec.clip);
  }
  if (!spec.suite_gfx.empty()) {
    ctx.warn("Angelica: the full suite effect '" + spec.suite_gfx +
             "' is not imported yet (GFX effects come in a later step)");
  }

  ctx.add_asset(&collection->id,
                "Angelica/Personagens",
                "Angelica Character: " + spec.smd_path,
                {"Angelica", "Character"});
  return collection;
}

/** \} */

}  // namespace blender::io::angelica
