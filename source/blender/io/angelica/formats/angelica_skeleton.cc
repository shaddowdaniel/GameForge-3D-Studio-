/* SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup angelica
 *
 * `.bon` layout (grounded on `A3DSkeleton.cpp`, `A3DBone.cpp`, `A3DJoint.cpp` of Angelica 2.2
 * and the 1.5.5 client, both identical):
 *
 * - `A3DSKELETONFILEHEADER` (packed, 96 bytes): `uint32 'ASKE'`, `uint32 version`,
 *   `int iNumBone, iNumJoint, iNumHook, iAnimStart, iAnimEnd, iAnimFPS`, 64 reserved bytes.
 * - Bones: `AFile` string name, `BONEDATA` (not packed, 144 bytes: `uint8 flags`, 3 padding
 *   bytes, `int iParent, iFirstJoint, iNumChild`, `A3DMATRIX4 matRelative, matBoneInit`), then
 *   `int children[iNumChild]`.
 * - Joints: `uint32 class_id[iNumJoint]`, then per joint an `AFile` string name and
 *   `JOINTDATA {int iParentBone, iChildBone, iSiblingJoint}`; version < 6 follows every joint
 *   with its bone track (version 1 track format).
 * - Hooks: `AFile` string name, `HOOKDATA {uint32 dwType, int iBone, A3DMATRIX4 matHookTM}`.
 */

#include "angelica_skeleton.hh"
#include "angelica_skin.hh"

namespace blender::io::angelica {

static bool skel_fail(std::string *r_error, const std::string &message)
{
  if (r_error) {
    *r_error = message;
  }
  return false;
}

static bool is_known_joint_class(const uint32_t class_id)
{
  return class_id == JOINT_CLASS_CONTROLLED || class_id == JOINT_CLASS_ANIM ||
         class_id == JOINT_CLASS_SPRING || class_id == JOINT_CLASS_AXIS_SPRING;
}

static void read_matrix(BinaryReader &reader, float r_matrix[16])
{
  for (int i = 0; i < 16; i++) {
    r_matrix[i] = reader.f32();
  }
}

int Skeleton::find_bone(const std::string &name) const
{
  for (size_t i = 0; i < bones.size(); i++) {
    if (bones[i].name == name) {
      return int(i);
    }
  }
  return -1;
}

bool parse_skeleton(ByteSpan data, Skeleton &r_skeleton, std::string *r_error)
{
  r_skeleton = Skeleton();
  const AFileContent content = afile_open(data);
  BinaryReader reader(content.body);

  if (reader.remaining() < 96) {
    return skel_fail(r_error, "File too small for a skeleton header");
  }
  const uint32_t flags = reader.u32();
  const uint32_t version = reader.u32();
  if (flags != SKELETON_FILE_IDENTIFY) {
    return skel_fail(r_error, "Not a skeleton (.bon) file");
  }
  if (version == 0 || version > SKELETON_FILE_VERSION) {
    return skel_fail(r_error, "Unsupported skeleton version " + std::to_string(version));
  }
  r_skeleton.version = version;
  const int32_t num_bones = reader.i32();
  const int32_t num_joints = reader.i32();
  const int32_t num_hooks = reader.i32();
  r_skeleton.anim_start = reader.i32();
  r_skeleton.anim_end = reader.i32();
  r_skeleton.anim_fps = reader.i32();
  reader.skip(64);

  /* Smallest records: bone 4 + 144, joint 4 + 4 + 12, hook 4 + 72. */
  if (num_bones < 0 || size_t(num_bones) * 148 > reader.remaining()) {
    return skel_fail(r_error, "Invalid bone count");
  }
  if (num_joints < 0 || size_t(num_joints) * 20 > reader.remaining()) {
    return skel_fail(r_error, "Invalid joint count");
  }
  if (num_hooks < 0 || size_t(num_hooks) * 76 > reader.remaining()) {
    return skel_fail(r_error, "Invalid hook count");
  }

  r_skeleton.bones.resize(size_t(num_bones));
  for (int i = 0; i < num_bones; i++) {
    SkeletonBone &bone = r_skeleton.bones[size_t(i)];
    bone.name = gbk_to_utf8(reader.string_i32());
    bone.flags = reader.u8();
    reader.skip(3);
    bone.parent = reader.i32();
    bone.first_joint = reader.i32();
    const int32_t num_children = reader.i32();
    read_matrix(reader, bone.relative);
    read_matrix(reader, bone.bone_init);
    if (reader.failed()) {
      return skel_fail(r_error, "Truncated bone " + std::to_string(i));
    }
    if (num_children < 0 || size_t(num_children) * 4 > reader.remaining()) {
      return skel_fail(r_error, "Invalid child count of bone \"" + bone.name + "\"");
    }
    bone.children.resize(size_t(num_children));
    for (int &child : bone.children) {
      child = reader.i32();
    }
  }
  /* Validate the hierarchy so the importer can trust the indices. */
  for (int i = 0; i < num_bones; i++) {
    SkeletonBone &bone = r_skeleton.bones[size_t(i)];
    if (bone.parent < -1 || bone.parent >= num_bones || bone.parent == i) {
      return skel_fail(r_error, "Bone \"" + bone.name + "\" has an invalid parent");
    }
    for (const int child : bone.children) {
      if (child < 0 || child >= num_bones) {
        return skel_fail(r_error, "Bone \"" + bone.name + "\" has an invalid child");
      }
    }
  }
  for (int i = 0; i < num_bones; i++) {
    /* Reject parent cycles (bounded walk). */
    int p = r_skeleton.bones[size_t(i)].parent;
    for (int steps = 0; p >= 0; steps++) {
      if (steps > num_bones) {
        return skel_fail(r_error, "Bone hierarchy contains a cycle");
      }
      p = r_skeleton.bones[size_t(p)].parent;
    }
  }

  if (num_joints > 0) {
    std::vector<uint32_t> class_ids(static_cast<size_t>(num_joints));
    for (uint32_t &id : class_ids) {
      id = reader.u32();
    }
    r_skeleton.joints.resize(size_t(num_joints));
    for (int i = 0; i < num_joints; i++) {
      SkeletonJoint &joint = r_skeleton.joints[size_t(i)];
      joint.class_id = class_ids[size_t(i)];
      if (!is_known_joint_class(joint.class_id)) {
        return skel_fail(r_error, "Unknown joint class " + std::to_string(joint.class_id));
      }
      joint.name = gbk_to_utf8(reader.string_i32());
      joint.parent_bone = reader.i32();
      joint.child_bone = reader.i32();
      joint.sibling_joint = reader.i32();
      if (reader.failed()) {
        return skel_fail(r_error, "Truncated joint " + std::to_string(i));
      }
      if (version < 6) {
        BoneTrack track;
        if (!read_bone_track(reader, 1, i, track, r_error)) {
          return false;
        }
        r_skeleton.embedded_tracks.push_back(std::move(track));
      }
    }
  }

  r_skeleton.hooks.resize(size_t(num_hooks));
  for (int i = 0; i < num_hooks; i++) {
    SkeletonHook &hook = r_skeleton.hooks[size_t(i)];
    hook.name = gbk_to_utf8(reader.string_i32());
    hook.type = reader.u32();
    hook.bone = reader.i32();
    read_matrix(reader, hook.matrix);
    if (reader.failed()) {
      return skel_fail(r_error, "Truncated hook " + std::to_string(i));
    }
  }
  return true;
}

}  // namespace blender::io::angelica
