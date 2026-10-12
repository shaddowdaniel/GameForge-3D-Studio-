/* SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup angelica
 *
 * Parser for Angelica Engine skeleton files (`.bon`, `A3DSkeleton`).
 * See `angelica_skin.hh` for the coordinate and matrix conventions.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "angelica_anim.hh"
#include "angelica_reader.hh"

namespace blender::io::angelica {

constexpr uint32_t SKELETON_FILE_IDENTIFY = fourcc('A', 'S', 'K', 'E');
/** Versions 1..6. Before 6 the bone tracks are stored inside the skeleton. */
constexpr uint32_t SKELETON_FILE_VERSION = 6;

/** `A3DJoint` class ids (`A3DClassID.h`) stored in front of the joints. */
enum : uint32_t {
  JOINT_CLASS_CONTROLLED = 12,
  JOINT_CLASS_ANIM = 15,
  JOINT_CLASS_SPRING = 16,
  JOINT_CLASS_AXIS_SPRING = 17,
};

struct SkeletonBone {
  std::string name;
  /** Parent bone index, -1 for roots. */
  int parent = -1;
  /** Joint (and bone track id) that animates this bone, -1 if none. */
  int first_joint = -1;
  /** `A3DBone` flags byte. */
  uint8_t flags = 0;
  /** Bind pose transform from this bone's space into its parent's space (`matRelative`). */
  float relative[16];
  /**
   * `matBoneInit`: model space to bone space at bind time (the inverse bind matrix). The engine
   * skins with `bone_init * bone_absolute`.
   */
  float bone_init[16];
  std::vector<int> children;
};

struct SkeletonJoint {
  std::string name;
  /** One of the `JOINT_CLASS_*` values. */
  uint32_t class_id = 0;
  int parent_bone = -1;
  int child_bone = -1;
  int sibling_joint = -1;
};

/** `A3DSkeletonHook`: attachment point (weapons, effects, child models). */
struct SkeletonHook {
  std::string name;
  uint32_t type = 0;
  /** Bone the hook follows, -1 for the model itself. */
  int bone = -1;
  /** Hook transform in the space of #bone. */
  float matrix[16];
};

struct Skeleton {
  uint32_t version = 0;
  int anim_start = 0;
  int anim_end = 0;
  int anim_fps = 0;
  std::vector<SkeletonBone> bones;
  std::vector<SkeletonJoint> joints;
  std::vector<SkeletonHook> hooks;
  /**
   * Versions < 6: the bone tracks stored after every joint (track id = joint index, version 1
   * track format). Newer skeletons keep animations in `.stck` files.
   */
  std::vector<BoneTrack> embedded_tracks;

  /** Index of the bone with this name, -1 if not found. */
  int find_bone(const std::string &name) const;
};

/** Parse a `.bon` file (versions 1..6). */
bool parse_skeleton(ByteSpan data, Skeleton &r_skeleton, std::string *r_error);

}  // namespace blender::io::angelica
