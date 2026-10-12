/* SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup angelica
 *
 * Model import: skinned characters (`.ecm` -> `.smd` -> `.bon` + `.ski` + `.stck`) and static
 * models (`.bmd`, `.mox`).
 *
 * Skinning in the engine computes `v' = v * bone_init * absolute` for every bone, where
 * `bone_init` is the inverse of the bone's bind pose in model space. The armature rest pose is
 * therefore `inverse(bone_init)`, and the meshes stay in model space, parented to the armature.
 *
 * Animations are sampled once per engine frame. The engine rebuilds each bone from its track
 * (`relative = rotation(quaternion)` with the translation in the last row, the X row negated for
 * mirrored bones) and chains it to the parent, so the importer does the same in model space and
 * stores the difference to the rest pose as pose bone channels. Mirrored bones have a negative
 * determinant that Blender bones cannot represent: the mirror is moved into bone space on both
 * the rest and the animated matrices, where it cancels out of the skinning.
 */

#include <algorithm>
#include <cmath>
#include <cstring>

#include "ANIM_action.hh"
#include "ANIM_animdata.hh"
#include "ANIM_fcurve.hh"

#include "BKE_anim_data.hh"
#include "BKE_armature.hh"
#include "BKE_attribute.hh"
#include "BKE_collection.hh"
#include "BKE_deform.hh"
#include "BKE_fcurve.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_mesh.hh"
#include "BKE_modifier.hh"
#include "BKE_object.hh"
#include "BKE_object_deform.h"

#include "BLI_linear_allocator.hh"
#include "BLI_listbase.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_quaternion.hh"
#include "BLI_math_vector.hh"
#include "BLI_string.hh"
#include "BLI_string_utf8.hh"
#include "BLI_vector.hh"

#include "DNA_anim_types.h"
#include "DNA_armature_types.h"
#include "DNA_collection_types.h"
#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "ED_armature.hh"

#include "MEM_guardedalloc.h"

#include "angelica_import_model.hh"
#include "angelica_model.hh"
#include "angelica_skeleton.hh"
#include "angelica_skin.hh"
#include "angelica_static.hh"

namespace blender::io::angelica {

bool is_model_extension(const std::string &ext)
{
  return ext == ".ecm" || ext == ".smd" || ext == ".ski" || ext == ".bon" || ext == ".bmd" ||
         ext == ".mox";
}

/* -------------------------------------------------------------------- */
/** \name Helpers
 * \{ */

/** Bone flag of mirrored bones (`A3DBone::FLAG_FLIPPED`). */
static constexpr uint8_t BONE_FLAG_FLIPPED = 0x02;
/** Engine frame rate used when a file does not store one. */
static constexpr float default_anim_fps = 30.0f;

/** Read a file referenced by `path`, relative to `base_dir` first. */
static bool read_model_file(ImportContext &ctx,
                            const std::string &path,
                            const std::string &base_dir,
                            Bytes &r_data)
{
  if (path.empty()) {
    return false;
  }
  if (ctx.vfs.read(path, r_data, base_dir)) {
    return true;
  }
  /* Exporters sometimes store absolute paths of the artist's machine: try the file name. */
  const std::string name = path_basename(path);
  return name != path && ctx.vfs.read(name, r_data, base_dir);
}

static std::string engine_dirname(const std::string &engine_path)
{
  return path_dirname(normalize_engine_path(engine_path, false));
}

/** Remove scale and move a mirror into bone space, so the matrix is a proper rotation. */
static float4x4 orthonormal_bone_matrix(float4x4 m)
{
  float3 x = m.x_axis(), y = m.y_axis(), z = m.z_axis();
  const float len_y = math::length(y);
  y = (len_y > 1e-8f) ? y / len_y : float3(0.0f, 1.0f, 0.0f);
  z = z - y * math::dot(z, y);
  const float len_z = math::length(z);
  z = (len_z > 1e-8f) ? z / len_z : math::normalize(math::cross(x, y));
  /* A right-handed X: mirrored bones get their X axis negated, matching the engine flag. */
  x = math::cross(y, z);
  m.x_axis() = x;
  m.y_axis() = y;
  m.z_axis() = z;
  m[0][3] = m[1][3] = m[2][3] = 0.0f;
  m[3][3] = 1.0f;
  return m;
}

/** Engine `relative` matrix of a bone from a track sample, converted to Blender. */
static float4x4 bone_local_from_track(const float q[4],
                                      const float3 &pos,
                                      const bool flipped,
                                      const float scale)
{
  /* D3DXMatrixRotationQuaternion, row-vector convention. */
  const float x = q[0], y = q[1], z = q[2], w = q[3];
  float m[16] = {1.0f - 2.0f * (y * y + z * z),
                 2.0f * (x * y + z * w),
                 2.0f * (x * z - y * w),
                 0.0f,
                 2.0f * (x * y - z * w),
                 1.0f - 2.0f * (x * x + z * z),
                 2.0f * (y * z + x * w),
                 0.0f,
                 2.0f * (x * z + y * w),
                 2.0f * (y * z - x * w),
                 1.0f - 2.0f * (x * x + y * y),
                 0.0f,
                 pos.x,
                 pos.y,
                 pos.z,
                 1.0f};
  if (flipped) {
    m[0] = -m[0];
    m[1] = -m[1];
    m[2] = -m[2];
  }
  return engine_matrix_to_blender(m, scale);
}

/** Sample a key track at `frame` (in the track's own frames): linear, or normalized lerp. */
static void sample_track(const KeyTrack &track, const float frame, float *r_value)
{
  const int count = track.key_count();
  const int n = track.components;
  if (count == 0) {
    return;
  }
  const std::vector<float> &frames = track.key_frames;
  auto key = [&](const int i) { return &track.keys[size_t(i) * n]; };
  auto key_frame = [&](const int i) {
    return (size_t(i) < frames.size()) ? frames[size_t(i)] : float(i);
  };
  if (count == 1 || frame <= key_frame(0)) {
    std::copy_n(key(0), n, r_value);
    return;
  }
  if (frame >= key_frame(count - 1)) {
    std::copy_n(key(count - 1), n, r_value);
    return;
  }
  int hi = 1;
  while (hi < count - 1 && key_frame(hi) < frame) {
    hi++;
  }
  const int lo = hi - 1;
  const float span = key_frame(hi) - key_frame(lo);
  const float t = span > 0.0f ? (frame - key_frame(lo)) / span : 0.0f;
  const float *a = key(lo);
  const float *b = key(hi);
  if (n == 4) {
    const float sign = (a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]) < 0.0f ? -1.0f :
                                                                                        1.0f;
    float len = 0.0f;
    for (int c = 0; c < 4; c++) {
      r_value[c] = a[c] * (1.0f - t) + sign * b[c] * t;
      len += r_value[c] * r_value[c];
    }
    len = std::sqrt(len);
    if (len > 1e-8f) {
      for (int c = 0; c < 4; c++) {
        r_value[c] /= len;
      }
    }
    return;
  }
  for (int c = 0; c < n; c++) {
    r_value[c] = a[c] * (1.0f - t) + b[c] * t;
  }
}

static Mesh *mesh_to_main(Main *bmain, Mesh *mesh_nomain, const std::string &name)
{
  Mesh *mesh = BKE_mesh_add(bmain, name.c_str());
  BKE_mesh_nomain_to_mesh(mesh_nomain, mesh, nullptr);
  return mesh;
}

/** Geometry of one engine mesh, in engine space. */
struct MeshSource {
  Span<float> positions;
  Span<float> normals;
  Span<float> uvs;
  Span<float> uvs2;
  Span<uint16_t> indices;
};

/**
 * Build a triangle mesh from engine vertex arrays. Invalid and degenerate triangles are dropped.
 * Returns a mesh outside of #Main, or null when nothing is left.
 */
static Mesh *build_mesh(const MeshSource &src, const float scale, const char *uv2_name)
{
  const int vert_count = int(src.positions.size() / 3);
  if (vert_count == 0) {
    return nullptr;
  }
  Vector<int3> tris;
  tris.reserve(int64_t(src.indices.size() / 3));
  for (int64_t i = 0; i + 2 < src.indices.size(); i += 3) {
    const int a = src.indices[i], b = src.indices[i + 1], c = src.indices[i + 2];
    if (a >= vert_count || b >= vert_count || c >= vert_count || a == b || b == c || a == c) {
      continue;
    }
    tris.append(int3(a, b, c));
  }
  if (tris.is_empty()) {
    return nullptr;
  }

  Mesh *mesh = BKE_mesh_new_nomain(vert_count, 0, int(tris.size()), int(tris.size()) * 3);
  MutableSpan<float3> positions = mesh->vert_positions_for_write();
  for (int v = 0; v < vert_count; v++) {
    positions[v] = to_blender(&src.positions[size_t(v) * 3], scale);
  }
  MutableSpan<int> face_offsets = mesh->face_offsets_for_write();
  MutableSpan<int> corner_verts = mesh->corner_verts_for_write();
  for (const int64_t f : tris.index_range()) {
    face_offsets[f] = int(f) * 3;
    /* The Y/Z swap is a reflection: the engine's clockwise order becomes counter-clockwise. */
    corner_verts[f * 3 + 0] = tris[f][0];
    corner_verts[f * 3 + 1] = tris[f][1];
    corner_verts[f * 3 + 2] = tris[f][2];
  }
  bke::mesh_calc_edges(*mesh, false, false);
  bke::mesh_smooth_set(*mesh, true);

  auto add_uvs = [&](const Span<float> uvs, const char *name) {
    if (uvs.size() < int64_t(vert_count) * 2) {
      return;
    }
    bke::MutableAttributeAccessor attributes = mesh->attributes_for_write();
    bke::SpanAttributeWriter<float2> uv = attributes.lookup_or_add_for_write_only_span<float2>(
        name, bke::AttrDomain::Corner);
    for (const int corner : corner_verts.index_range()) {
      const int v = corner_verts[corner];
      /* Engine V runs down the image. */
      uv.span[corner] = float2(uvs[size_t(v) * 2], 1.0f - uvs[size_t(v) * 2 + 1]);
    }
    uv.finish();
  };
  add_uvs(src.uvs, "UVMap");
  if (uv2_name) {
    add_uvs(src.uvs2, uv2_name);
  }
  if (src.uvs.size() >= int64_t(vert_count) * 2) {
    mesh->uv_maps_active_set("UVMap");
    mesh->uv_maps_default_set("UVMap");
  }

  if (src.normals.size() >= int64_t(vert_count) * 3) {
    Array<float3> normals(vert_count);
    for (int v = 0; v < vert_count; v++) {
      const float3 n = to_blender(&src.normals[size_t(v) * 3]);
      const float len = math::length(n);
      normals[v] = len > 1e-8f ? n / len : float3(0.0f, 0.0f, 1.0f);
    }
    bke::mesh_set_custom_normals_from_verts_normalized(*mesh, normals);
  }
  return mesh;
}

static Object *add_model_object(ImportContext &ctx,
                                Collection *collection,
                                const std::string &name,
                                const short type,
                                ID *data)
{
  Object *ob = BKE_object_add_only_object(ctx.bmain, ObjectType(type), name.c_str());
  ob->data = data;
  BKE_collection_object_add(ctx.bmain, collection, ob);
  return ob;
}

static void assign_material(ImportContext &ctx, Object *ob, Material *material)
{
  if (material != nullptr) {
    BKE_object_material_assign(ctx.bmain, ob, material, 1, BKE_MAT_ASSIGN_OBDATA);
  }
}

static Material *material_from_info(ImportContext &ctx,
                                    const std::string &name,
                                    const std::string &texture,
                                    const std::string &base_dir,
                                    const MaterialInfo *info)
{
  float diffuse[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  bool two_sided = false;
  if (info) {
    diffuse[0] = info->diffuse.r;
    diffuse[1] = info->diffuse.g;
    diffuse[2] = info->diffuse.b;
    diffuse[3] = info->diffuse.a > 0.0f ? info->diffuse.a : 1.0f;
    two_sided = info->two_sided;
  }
  Image *image = ctx.load_image({texture, base_dir});
  const std::string mat_name = texture.empty() ? name : name_from_engine_path(texture);
  /* Most character and plant textures use their alpha channel as a cut-out. */
  return ctx.material_for_texture(mat_name, image, diffuse, image ? 1 : 0, two_sided);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Skinned Models
 * \{ */

struct ArmatureBuild {
  Object *object = nullptr;
  /** Blender bone name per skeleton bone. */
  Vector<std::string> bone_names;
  /** Rest matrix (armature space, Blender) per skeleton bone. */
  Vector<float4x4> rest;
};

static float4x4 bind_matrix(const SkeletonBone &bone, const float scale)
{
  const float4x4 bone_init = engine_matrix_to_blender(bone.bone_init, scale);
  return orthonormal_bone_matrix(math::invert(bone_init));
}

static ArmatureBuild build_armature(ImportContext &ctx,
                                    Collection *collection,
                                    const Skeleton &skeleton,
                                    const std::string &name)
{
  ArmatureBuild build;
  const int bone_count = int(skeleton.bones.size());
  build.rest.resize(bone_count);
  for (int i = 0; i < bone_count; i++) {
    build.rest[i] = bind_matrix(skeleton.bones[i], ctx.scale);
  }

  bArmature *arm = BKE_armature_add(ctx.bmain, name.c_str());
  Object *ob = add_model_object(ctx, collection, name, OB_ARMATURE, &arm->id);
  ob->dtx |= OB_DRAW_IN_FRONT;
  build.object = ob;

  ED_armature_to_edit(arm);
  Vector<EditBone *> ebones(bone_count, nullptr);
  for (int i = 0; i < bone_count; i++) {
    const SkeletonBone &bone = skeleton.bones[i];
    const float3 head = build.rest[i].location();

    /* Length: average distance to the children, so chains connect visually. */
    float length = 0.0f;
    int children = 0;
    for (const int child : bone.children) {
      if (child >= 0 && child < bone_count) {
        length += math::distance(head, build.rest[child].location());
        children++;
      }
    }
    if (children > 0) {
      length /= float(children);
    }
    else if (bone.parent >= 0 && bone.parent < bone_count) {
      length = math::distance(head, build.rest[bone.parent].location()) * 0.5f;
    }
    length = std::max(length, 0.01f * ctx.scale);

    EditBone *ebone = ED_armature_ebone_add(arm, bone.name.empty() ? "Bone" : bone.name.c_str());
    ebone->tail[0] = 0.0f;
    ebone->tail[1] = length;
    ebone->tail[2] = 0.0f;
    float mat[4][4];
    std::memcpy(mat, build.rest[i].ptr(), sizeof(mat));
    ED_armature_ebone_from_mat4(ebone, mat);
    ebones[i] = ebone;
    build.bone_names.append(ebone->name);
  }
  for (int i = 0; i < bone_count; i++) {
    const int parent = skeleton.bones[i].parent;
    if (parent >= 0 && parent < bone_count && parent != i) {
      ebones[i]->parent = ebones[parent];
    }
  }
  ED_armature_from_edit(ctx.bmain, arm);
  ED_armature_edit_free(arm);
  return build;
}

/** Map a skin's bone indices onto skeleton bones (`A3DSkin::m_aBoneIdxMap`). */
static Vector<int> skin_bone_map(const Skin &skin, const Skeleton *skeleton)
{
  Vector<int> map;
  if (skeleton == nullptr) {
    return map;
  }
  if (skin.version >= 9 && !skin.bone_names.empty()) {
    for (const std::string &name : skin.bone_names) {
      map.append(skeleton->find_bone(name));
    }
  }
  else {
    for (int i = 0; i < int(skeleton->bones.size()); i++) {
      map.append(i);
    }
  }
  return map;
}

static void add_vertex_groups(Mesh *mesh,
                              const SkinMesh &src,
                              const Vector<int> &bone_map,
                              const ArmatureBuild &armature)
{
  const int vert_count = src.vertex_count();
  if (bone_map.is_empty() || mesh->verts_num != vert_count) {
    return;
  }
  auto skeleton_bone = [&](const int skin_index) {
    return (skin_index >= 0 && skin_index < bone_map.size()) ? bone_map[skin_index] : -1;
  };

  /* Vertex groups are created on demand, one per skeleton bone that has weights. */
  Map<int, int> group_of_bone;
  MutableSpan<MDeformVert> dverts = mesh->deform_verts_for_write();
  auto add_weight = [&](const int vert, const int bone, const float weight) {
    if (bone < 0 || bone >= armature.bone_names.size() || weight <= 0.0f) {
      return;
    }
    const int group = group_of_bone.lookup_or_add_cb(bone, [&]() {
      bDeformGroup *defgroup = MEM_new<bDeformGroup>("bDeformGroup");
      StringRef(armature.bone_names[bone]).copy_utf8_truncated(defgroup->name);
      BLI_addtail(&mesh->vertex_group_names, defgroup);
      return int(group_of_bone.size());
    });
    MDeformWeight *dw = BKE_defvert_ensure_index(&dverts[vert], group);
    dw->weight += weight;
  };

  const bool rigid = src.type == SkinMeshType::Rigid || src.type == SkinMeshType::MorphRigid;
  const bool has_weights = src.bone_indices.size() >= size_t(vert_count) * 4 &&
                           src.bone_weights.size() >= size_t(vert_count) * 4;
  for (int v = 0; v < vert_count; v++) {
    if (rigid || !has_weights) {
      add_weight(v, skeleton_bone(src.bone_index), 1.0f);
      continue;
    }
    for (int k = 0; k < 4; k++) {
      add_weight(v,
                 skeleton_bone(src.bone_indices[size_t(v) * 4 + k]),
                 src.bone_weights[size_t(v) * 4 + k]);
    }
  }
}

static void add_armature_modifier(Object *ob, Object *armature)
{
  ModifierData *md = BKE_modifier_new(eModifierType_Armature);
  STRNCPY_UTF8(md->name, BKE_id_name(armature->id));
  BLI_addtail(&ob->modifiers, md);
  BKE_modifiers_persistent_uid_init(*ob, *md);
  reinterpret_cast<ArmatureModifierData *>(md)->object = armature;
  ob->parent = armature;
}

static int import_skin(ImportContext &ctx,
                       Collection *collection,
                       const Skin &skin,
                       const std::string &skin_path,
                       const Skeleton *skeleton,
                       const ArmatureBuild *armature)
{
  const std::string base_dir = engine_dirname(skin_path);
  const Vector<int> bone_map = skin_bone_map(skin, skeleton);
  int created = 0;
  for (const SkinMesh &src : skin.meshes) {
    if (!src.morph_channels.empty()) {
      ctx.warn("Angelica: morph targets of '" + path_basename(skin_path) +
               "' are imported in their base shape");
    }
    MeshSource geometry;
    geometry.positions = src.positions;
    geometry.normals = src.normals;
    geometry.uvs = src.uvs;
    geometry.indices = src.indices;
    Mesh *mesh_nomain = build_mesh(geometry, ctx.scale, nullptr);
    if (mesh_nomain == nullptr) {
      continue;
    }
    const std::string name = src.name.empty() ? name_from_engine_path(skin_path) : src.name;
    if (armature && armature->object) {
      add_vertex_groups(mesh_nomain, src, bone_map, *armature);
    }
    Mesh *mesh = mesh_to_main(ctx.bmain, mesh_nomain, name);
    Object *ob = add_model_object(ctx, collection, name, OB_MESH, &mesh->id);

    const std::string texture = (src.texture_index >= 0 &&
                                 src.texture_index < int(skin.textures.size())) ?
                                    skin.textures[src.texture_index] :
                                    std::string();
    const MaterialInfo *info = (src.material_index >= 0 &&
                                src.material_index < int(skin.materials.size())) ?
                                   &skin.materials[src.material_index] :
                                   nullptr;
    assign_material(ctx, ob, material_from_info(ctx, name, texture, base_dir, info));

    if (armature && armature->object) {
      add_armature_modifier(ob, armature->object);
    }
    created++;
  }
  return created;
}

/** Bake one track set into an action on the armature. */
static bAction *import_action(ImportContext &ctx,
                              const Skeleton &skeleton,
                              const ArmatureBuild &armature,
                              const TrackSet &tracks,
                              const std::string &action_name,
                              float start_frame,
                              float end_frame,
                              float anim_fps)
{
  const int bone_count = int(skeleton.bones.size());
  if (anim_fps <= 0.0f) {
    anim_fps = tracks.anim_fps > 0 ? float(tracks.anim_fps) : default_anim_fps;
  }
  if (end_frame < start_frame) {
    start_frame = float(tracks.anim_start);
    end_frame = float(tracks.anim_end);
  }
  const int frame_count = std::max(int(std::floor(end_frame - start_frame)) + 1, 1);

  Map<int, const BoneTrack *> track_of_joint;
  for (const BoneTrack &track : tracks.tracks) {
    track_of_joint.add(track.id, &track);
  }

  /* Rest pose relative to the parent, Blender space. */
  Vector<float4x4> rest_local(bone_count);
  for (int i = 0; i < bone_count; i++) {
    const int parent = skeleton.bones[i].parent;
    rest_local[i] = (parent >= 0 && parent < bone_count) ?
                        math::invert(armature.rest[parent]) * armature.rest[i] :
                        armature.rest[i];
  }

  /* Bones are listed parent first in skeleton files, but do not rely on it. */
  Vector<int> order;
  {
    Vector<bool> done(bone_count, false);
    while (order.size() < bone_count) {
      const int64_t before = order.size();
      for (int i = 0; i < bone_count; i++) {
        const int parent = skeleton.bones[i].parent;
        if (!done[i] && (parent < 0 || parent >= bone_count || parent == i || done[parent])) {
          done[i] = true;
          order.append(i);
        }
      }
      if (order.size() == before) {
        break; /* Cycle: leave the remaining bones in their rest pose. */
      }
    }
  }

  const float scene_fps = ctx.scene ? float(ctx.scene->r.frs_sec) / ctx.scene->r.frs_sec_base :
                                      24.0f;
  const float frame_scale = scene_fps / anim_fps;

  /* Sample: basis transforms per frame per bone, as location + quaternion. */
  Array<float3> locations(int64_t(frame_count) * bone_count, float3(0.0f));
  Array<math::Quaternion> rotations(int64_t(frame_count) * bone_count,
                                    math::Quaternion::identity());
  Vector<float4x4> world(bone_count);
  Vector<float4x4> world_fixed(bone_count);
  for (int f = 0; f < frame_count; f++) {
    const float seconds = (start_frame + float(f)) / anim_fps;
    for (const int i : order) {
      const SkeletonBone &bone = skeleton.bones[i];
      const bool flipped = (bone.flags & BONE_FLAG_FLIPPED) != 0;
      float4x4 local;
      const BoneTrack *const *track = track_of_joint.lookup_ptr(bone.first_joint);
      if (track) {
        float q[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        float p[3] = {0.0f, 0.0f, 0.0f};
        const KeyTrack &rot = (*track)->rotation;
        const KeyTrack &pos = (*track)->position;
        sample_track(rot, seconds * float(rot.frame_rate > 0 ? rot.frame_rate : anim_fps), q);
        sample_track(pos, seconds * float(pos.frame_rate > 0 ? pos.frame_rate : anim_fps), p);
        local = bone_local_from_track(q, float3(p[0], p[1], p[2]), flipped, ctx.scale);
      }
      else {
        local = engine_matrix_to_blender(bone.relative, ctx.scale);
      }
      const int parent = bone.parent;
      world[i] = (parent >= 0 && parent < bone_count && parent != i) ? world[parent] * local :
                                                                       local;
      world_fixed[i] = orthonormal_bone_matrix(world[i]);

      const float4x4 parent_fixed = (parent >= 0 && parent < bone_count && parent != i) ?
                                        world_fixed[parent] :
                                        float4x4::identity();
      const float4x4 anim_local = math::invert(parent_fixed) * world_fixed[i];
      const float4x4 basis = math::invert(rest_local[i]) * anim_local;

      float3 loc, size;
      math::Quaternion quat;
      math::to_loc_rot_scale<true>(basis, loc, quat, size);
      /* Keep successive quaternions in the same hemisphere so curves interpolate cleanly. */
      if (f > 0) {
        const math::Quaternion &prev = rotations[int64_t(f - 1) * bone_count + i];
        if (prev.w * quat.w + prev.x * quat.x + prev.y * quat.y + prev.z * quat.z < 0.0f) {
          quat = math::Quaternion(-quat.w, -quat.x, -quat.y, -quat.z);
        }
      }
      locations[int64_t(f) * bone_count + i] = loc;
      rotations[int64_t(f) * bone_count + i] = quat;
    }
  }

  animrig::Action &action = animrig::action_add(*ctx.bmain, action_name);
  id_fake_user_set(&action.id);
  action.layer_keystrip_ensure();
  animrig::StripKeyframeData &strip_data =
      action.layer(0)->strip(0)->data<animrig::StripKeyframeData>(action);
  ID *id = &armature.object->id;
  animrig::Slot &slot = action.slot_add_for_id_type(GS(id->name));
  action.slot_identifier_define(slot, id->name);
  animrig::Channelbag &channelbag = strip_data.channelbag_for_slot_ensure(slot);

  LinearAllocator<> name_alloc;
  Vector<animrig::FCurveDescriptor> descriptors;
  for (int i = 0; i < bone_count; i++) {
    const std::string prefix = "pose.bones[\"" + armature.bone_names[i] + "\"].";
    StringRefNull group = name_alloc.copy_string(armature.bone_names[i]);
    StringRefNull loc_path = name_alloc.copy_string(prefix + "location");
    StringRefNull rot_path = name_alloc.copy_string(prefix + "rotation_quaternion");
    for (int c = 0; c < 3; c++) {
      descriptors.append({loc_path, c, {}, {}, group});
    }
    for (int c = 0; c < 4; c++) {
      descriptors.append({rot_path, c, {}, {}, group});
    }
  }
  Vector<FCurve *> curves = channelbag.fcurve_create_many(nullptr, descriptors.as_span());
  if (curves.size() != descriptors.size()) {
    return &action;
  }

  for (int i = 0; i < bone_count; i++) {
    FCurve **bone_curves = &curves[int64_t(i) * 7];
    for (int c = 0; c < 7; c++) {
      BKE_fcurve_bezt_resize(*bone_curves[c], frame_count);
    }
    for (int f = 0; f < frame_count; f++) {
      const float3 &loc = locations[int64_t(f) * bone_count + i];
      const math::Quaternion &q = rotations[int64_t(f) * bone_count + i];
      const float values[7] = {loc.x, loc.y, loc.z, q.w, q.x, q.y, q.z};
      const float time = 1.0f + float(f) * frame_scale;
      for (int c = 0; c < 7; c++) {
        BezTriple &bez = bone_curves[c]->bezt[f];
        bez.vec[1][0] = time;
        bez.vec[1][1] = values[c];
        bez.ipo = BEZT_IPO_LIN;
        bez.f1 = bez.f2 = bez.f3 = BEZT_FLAG_SELECT;
        bez.h1 = bez.h2 = HD_AUTO_ANIM;
      }
    }
    for (int c = 0; c < 7; c++) {
      BKE_fcurve_handles_recalc(*bone_curves[c]);
    }
  }

  action.frame_start = 1.0f;
  action.frame_end = 1.0f + float(frame_count - 1) * frame_scale;
  action.flag |= ACT_FRAME_RANGE;

  /* The first action plays on the imported armature. */
  const AnimData *adt = BKE_animdata_ensure_id(id);
  if (adt && adt->action == nullptr) {
    if (animrig::assign_action(&action, *id)) {
      animrig::assign_action_slot(&slot, *id);
    }
  }
  return &action;
}

struct LoadedSkeleton {
  bool valid = false;
  Skeleton skeleton;
  std::string path;
};

static LoadedSkeleton load_skeleton(ImportContext &ctx,
                                    const std::string &path,
                                    const std::string &base_dir)
{
  LoadedSkeleton result;
  Bytes data;
  if (!read_model_file(ctx, path, base_dir, data)) {
    ctx.warn("Angelica: skeleton '" + path + "' not found");
    return result;
  }
  std::string error;
  if (!parse_skeleton(data, result.skeleton, &error)) {
    ctx.warn("Angelica: cannot read skeleton '" + path + "': " + error);
    return result;
  }
  result.valid = true;
  result.path = base_dir.empty() ? path : base_dir + "\\" + path;
  return result;
}

static bool load_skin(ImportContext &ctx,
                      const std::string &path,
                      const std::string &base_dir,
                      Skin &r_skin)
{
  Bytes data;
  if (!read_model_file(ctx, path, base_dir, data)) {
    ctx.warn("Angelica: skin '" + path + "' not found");
    return false;
  }
  std::string error;
  if (!parse_skin(data, r_skin, &error)) {
    ctx.warn("Angelica: cannot read skin '" + path + "': " + error);
    return false;
  }
  return true;
}

static bool load_tracks(ImportContext &ctx,
                        const std::string &path,
                        const std::string &base_dir,
                        TrackSet &r_tracks)
{
  Bytes data;
  if (!read_model_file(ctx, path, base_dir, data)) {
    return false;
  }
  std::string error;
  if (!parse_track_set(data, r_tracks, &error)) {
    ctx.warn("Angelica: cannot read animation '" + path + "': " + error);
    return false;
  }
  return true;
}

/** Engine path of a file referenced from a file in `base_dir`, for display and recursion. */
static std::string referenced_path(const std::string &base_dir, const std::string &path)
{
  const std::string normalized = normalize_engine_path(path, false);
  return base_dir.empty() ? normalized : base_dir + "\\" + normalized;
}

/** Import a `.smd` into `collection`. Returns the armature object, if any. */
static Object *import_skin_model(ImportContext &ctx,
                                 const std::string &engine_path,
                                 const ModelOptions &options,
                                 Collection *collection,
                                 const Vector<std::string> &extra_skins,
                                 const std::string &extra_skins_dir,
                                 int *r_mesh_count)
{
  Bytes data;
  if (!ctx.vfs.read(engine_path, data)) {
    ctx.warn("Angelica: model '" + engine_path + "' not found");
    return nullptr;
  }
  SkinModel model;
  std::string error;
  if (!parse_skin_model(data, model, &error, path_basename(engine_path))) {
    ctx.warn("Angelica: cannot read model '" + engine_path + "': " + error);
    return nullptr;
  }
  const std::string base_dir = engine_dirname(engine_path);
  const std::string name = name_from_engine_path(engine_path);

  LoadedSkeleton skeleton;
  if (!model.skeleton_file.empty()) {
    skeleton = load_skeleton(ctx, model.skeleton_file, base_dir);
  }

  ArmatureBuild armature;
  const bool use_armature = options.import_armature && skeleton.valid &&
                            !skeleton.skeleton.bones.empty();
  if (use_armature) {
    armature = build_armature(ctx, collection, skeleton.skeleton, name);
  }

  int meshes = 0;
  for (const std::string &skin_file : model.skin_files) {
    Skin skin;
    if (load_skin(ctx, skin_file, base_dir, skin)) {
      meshes += import_skin(ctx,
                            collection,
                            skin,
                            referenced_path(base_dir, skin_file),
                            skeleton.valid ? &skeleton.skeleton : nullptr,
                            use_armature ? &armature : nullptr);
    }
  }
  for (const std::string &skin_file : extra_skins) {
    Skin skin;
    if (load_skin(ctx, skin_file, extra_skins_dir, skin)) {
      meshes += import_skin(ctx,
                            collection,
                            skin,
                            referenced_path(extra_skins_dir, skin_file),
                            skeleton.valid ? &skeleton.skeleton : nullptr,
                            use_armature ? &armature : nullptr);
    }
  }
  if (r_mesh_count) {
    *r_mesh_count += meshes;
  }

  if (use_armature && options.import_animations) {
    const std::string tracks_dir = model.tracks_dir.empty() ?
                                       base_dir :
                                       referenced_path(base_dir, model.tracks_dir);
    Map<std::string, std::shared_ptr<TrackSet>> cache;
    auto tracks_for = [&](const std::string &file) -> std::shared_ptr<TrackSet> {
      if (std::shared_ptr<TrackSet> *cached = cache.lookup_ptr(file)) {
        return *cached;
      }
      auto tracks = std::make_shared<TrackSet>();
      if (!load_tracks(ctx, file, tracks_dir, *tracks) &&
          !load_tracks(ctx, file, base_dir, *tracks))
      {
        ctx.warn("Angelica: animation '" + file + "' not found");
        tracks.reset();
      }
      cache.add(file, tracks);
      return tracks;
    };

    /* Older skeletons carry their animation; otherwise a track set named like the model. */
    std::shared_ptr<TrackSet> shared_tracks;
    if (!skeleton.skeleton.embedded_tracks.empty()) {
      shared_tracks = std::make_shared<TrackSet>();
      shared_tracks->anim_start = skeleton.skeleton.anim_start;
      shared_tracks->anim_end = skeleton.skeleton.anim_end;
      shared_tracks->anim_fps = skeleton.skeleton.anim_fps;
      shared_tracks->tracks = skeleton.skeleton.embedded_tracks;
    }
    auto default_tracks = [&]() -> std::shared_ptr<TrackSet> {
      if (!shared_tracks) {
        shared_tracks = std::make_shared<TrackSet>();
        if (!load_tracks(ctx, path_stem(engine_path) + ".stck", base_dir, *shared_tracks)) {
          shared_tracks = std::make_shared<TrackSet>();
        }
      }
      return shared_tracks->tracks.empty() ? nullptr : shared_tracks;
    };

    for (const SkinModelAction &act : model.actions) {
      std::shared_ptr<TrackSet> tracks = act.track_file.empty() ? default_tracks() :
                                                                  tracks_for(act.track_file);
      if (!tracks) {
        continue;
      }
      import_action(ctx,
                    skeleton.skeleton,
                    armature,
                    *tracks,
                    name + "|" + act.name,
                    act.start_frame,
                    act.end_frame,
                    float(act.frame_rate));
    }
    if (model.actions.empty()) {
      if (std::shared_ptr<TrackSet> tracks = default_tracks()) {
        import_action(ctx, skeleton.skeleton, armature, *tracks, name, 0.0f, -1.0f, 0.0f);
      }
    }
  }
  return use_armature ? armature.object : nullptr;
}

static Collection *import_ecm(ImportContext &ctx,
                              const std::string &engine_path,
                              const ModelOptions &options,
                              Collection *collection,
                              int depth,
                              int *r_mesh_count);

/** Import an `.ecm` (a `.smd` plus extra skins and child models) into `collection`. */
static Collection *import_ecm(ImportContext &ctx,
                              const std::string &engine_path,
                              const ModelOptions &options,
                              Collection *collection,
                              const int depth,
                              int *r_mesh_count)
{
  Bytes data;
  if (!ctx.vfs.read(engine_path, data)) {
    ctx.warn("Angelica: model '" + engine_path + "' not found");
    return nullptr;
  }
  ECModel model;
  std::string error;
  if (!parse_ec_model(data, model, &error)) {
    ctx.warn("Angelica: cannot read model '" + engine_path + "': " + error);
    return nullptr;
  }
  const std::string base_dir = engine_dirname(engine_path);

  /* `.ecm` references are relative to the client root, with older files relative to the ecm. */
  auto resolve = [&](const std::string &path) {
    const std::string normalized = normalize_engine_path(path, false);
    if (ctx.vfs.exists(normalized)) {
      return normalized;
    }
    return referenced_path(base_dir, path);
  };

  Vector<std::string> extra_skins;
  for (const std::string &skin : model.additional_skins) {
    extra_skins.append(resolve(skin));
  }
  Object *armature = nullptr;
  if (!model.skin_model_path.empty()) {
    armature = import_skin_model(
        ctx, resolve(model.skin_model_path), options, collection, extra_skins, "", r_mesh_count);
  }

  /* Child models (weapons, mounts, wings) hang from hooks of the parent: import them next to it,
   * placed in their own collection inside this one. */
  if (depth < 4) {
    for (const ECModelChild &child : model.children) {
      if (child.path.empty()) {
        continue;
      }
      const std::string child_path = resolve(child.path);
      Collection *child_collection = BKE_collection_add(
          ctx.bmain,
          collection,
          (child.name.empty() ? name_from_engine_path(child_path) : child.name).c_str());
      const std::string ext = path_extension(child_path);
      if (ext == ".ecm") {
        import_ecm(ctx, child_path, options, child_collection, depth + 1, r_mesh_count);
      }
      else {
        import_skin_model(ctx, child_path, options, child_collection, {}, "", r_mesh_count);
      }
    }
  }
  UNUSED_VARS(armature);
  return collection;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Static Models
 * \{ */

static int import_bmd(ImportContext &ctx, const std::string &engine_path, Collection *collection)
{
  Bytes data;
  if (!ctx.vfs.read(engine_path, data)) {
    ctx.warn("Angelica: model '" + engine_path + "' not found");
    return 0;
  }
  const std::string base_dir = engine_dirname(engine_path);
  LitModel model;
  std::string error;
  const LitSharedDataLoader loader = [&](const std::string &path_raw, Bytes &r_data) {
    return read_model_file(ctx, path_raw, base_dir, r_data);
  };
  if (!parse_bmd(data, model, &error, loader)) {
    ctx.warn("Angelica: cannot read model '" + engine_path + "': " + error);
    return 0;
  }

  int created = 0;
  for (const LitMesh &src : model.meshes) {
    MeshSource geometry;
    geometry.positions = src.positions;
    geometry.normals = src.normals;
    geometry.uvs = src.uvs;
    geometry.uvs2 = src.lightmap_uvs;
    geometry.indices = src.indices;
    Mesh *mesh_nomain = build_mesh(geometry, ctx.scale, "Lightmap");
    if (mesh_nomain == nullptr) {
      continue;
    }
    const std::string name = src.name.empty() ? name_from_engine_path(engine_path) : src.name;
    Mesh *mesh = mesh_to_main(ctx.bmain, mesh_nomain, name);
    Object *ob = add_model_object(ctx, collection, name, OB_MESH, &mesh->id);
    assign_material(
        ctx,
        ob,
        material_from_info(
            ctx, name, src.texture, base_dir, src.has_material ? &src.material : nullptr));
    created++;
  }
  return created;
}

static int import_mox_frame(ImportContext &ctx,
                            const MoxFrame &frame,
                            const float4x4 &parent_world,
                            const std::string &base_dir,
                            Collection *collection,
                            const std::string &fallback_name)
{
  float4x4 world = parent_world;
  if (!frame.relative_tms.empty()) {
    world = parent_world * engine_matrix_to_blender(frame.relative_tms[0].data(), ctx.scale);
  }
  int created = 0;
  for (const MoxMesh &src : frame.meshes) {
    MeshSource geometry;
    geometry.positions = src.positions;
    geometry.normals = src.normals;
    geometry.uvs = src.uvs;
    geometry.uvs2 = src.lightmap_uvs;
    geometry.indices = src.indices;
    Mesh *mesh_nomain = build_mesh(geometry, ctx.scale, "Lightmap");
    if (mesh_nomain == nullptr) {
      continue;
    }
    std::string name = !src.name.empty()   ? src.name :
                       !frame.name.empty() ? frame.name :
                                             fallback_name;
    Mesh *mesh = mesh_to_main(ctx.bmain, mesh_nomain, name);
    Object *ob = add_model_object(ctx, collection, name, OB_MESH, &mesh->id);
    float mat[4][4];
    std::memcpy(mat, world.ptr(), sizeof(mat));
    BKE_object_apply_mat4(ob, mat, false, false);
    assign_material(ctx, ob, material_from_info(ctx, name, src.texture, base_dir, &src.material));
    created++;
  }
  for (const MoxFrame &child : frame.children) {
    created += import_mox_frame(ctx, child, world, base_dir, collection, fallback_name);
  }
  return created;
}

static int import_mox(ImportContext &ctx, const std::string &engine_path, Collection *collection)
{
  Bytes data;
  if (!ctx.vfs.read(engine_path, data)) {
    ctx.warn("Angelica: model '" + engine_path + "' not found");
    return 0;
  }
  MoxFrame root;
  std::string error;
  if (!parse_mox(data, root, &error)) {
    ctx.warn("Angelica: cannot read model '" + engine_path + "': " + error);
    return 0;
  }
  return import_mox_frame(ctx,
                          root,
                          float4x4::identity(),
                          engine_dirname(engine_path),
                          collection,
                          name_from_engine_path(engine_path));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Entry Point
 * \{ */

Collection *import_model(ImportContext &ctx,
                         const std::string &engine_path,
                         const ModelOptions &options,
                         Collection *parent,
                         const std::string &catalog_root)
{
  const std::string ext = path_extension(engine_path);
  if (!is_model_extension(ext)) {
    return nullptr;
  }
  const std::string name = name_from_engine_path(engine_path);
  Collection *collection = BKE_collection_add(ctx.bmain, parent, name.c_str());

  int meshes = 0;
  bool has_armature = false;
  std::string kind;
  if (ext == ".ecm") {
    kind = "Character";
    import_ecm(ctx, engine_path, options, collection, 0, &meshes);
  }
  else if (ext == ".smd") {
    kind = "Character";
    has_armature = import_skin_model(ctx, engine_path, options, collection, {}, "", &meshes) !=
                   nullptr;
  }
  else if (ext == ".ski") {
    kind = "Skin";
    Skin skin;
    if (load_skin(ctx, engine_path, "", skin)) {
      meshes += import_skin(ctx, collection, skin, engine_path, nullptr, nullptr);
    }
  }
  else if (ext == ".bon") {
    kind = "Skeleton";
    const std::string base_dir = engine_dirname(engine_path);
    LoadedSkeleton skeleton = load_skeleton(ctx, path_basename(engine_path), base_dir);
    if (skeleton.valid && !skeleton.skeleton.bones.empty()) {
      ArmatureBuild armature = build_armature(ctx, collection, skeleton.skeleton, name);
      has_armature = true;
      if (options.import_animations) {
        TrackSet tracks;
        if (!skeleton.skeleton.embedded_tracks.empty()) {
          tracks.anim_start = skeleton.skeleton.anim_start;
          tracks.anim_end = skeleton.skeleton.anim_end;
          tracks.anim_fps = skeleton.skeleton.anim_fps;
          tracks.tracks = skeleton.skeleton.embedded_tracks;
        }
        else {
          load_tracks(ctx, path_stem(engine_path) + ".stck", base_dir, tracks);
        }
        if (!tracks.tracks.empty()) {
          import_action(ctx, skeleton.skeleton, armature, tracks, name, 0.0f, -1.0f, 0.0f);
        }
      }
    }
  }
  else if (ext == ".bmd") {
    kind = "Building";
    meshes += import_bmd(ctx, engine_path, collection);
  }
  else if (ext == ".mox") {
    kind = "Static";
    meshes += import_mox(ctx, engine_path, collection);
  }

  if (meshes == 0 && !has_armature) {
    BKE_id_delete(ctx.bmain, &collection->id);
    return nullptr;
  }

  ctx.add_asset(&collection->id,
                catalog_from_engine_path(engine_path, catalog_root),
                "Angelica " + kind + ": " + normalize_engine_path(engine_path, false),
                {"Angelica", kind});
  return collection;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Building Blocks for Assembled Characters
 * \{ */

Object *import_smd_with_skins(ImportContext &ctx,
                              const std::string &smd_path,
                              const Vector<std::string> &extra_skins,
                              const ModelOptions &options,
                              Collection *collection)
{
  return import_skin_model(ctx, smd_path, options, collection, extra_skins, "", nullptr);
}

Object *import_bon_with_skins(ImportContext &ctx,
                              const std::string &bon_path,
                              const Vector<std::string> &skins,
                              const std::string &stck_path,
                              const ModelOptions &options,
                              Collection *collection)
{
  LoadedSkeleton skeleton = load_skeleton(ctx, path_basename(bon_path), engine_dirname(bon_path));
  if (!skeleton.valid || skeleton.skeleton.bones.empty()) {
    return nullptr;
  }
  ArmatureBuild armature = build_armature(
      ctx, collection, skeleton.skeleton, name_from_engine_path(bon_path));
  for (const std::string &skin_path : skins) {
    Skin skin;
    if (load_skin(ctx, skin_path, "", skin)) {
      import_skin(ctx, collection, skin, skin_path, &skeleton.skeleton, &armature);
    }
  }
  if (options.import_animations) {
    TrackSet tracks;
    if (!skeleton.skeleton.embedded_tracks.empty()) {
      tracks.anim_start = skeleton.skeleton.anim_start;
      tracks.anim_end = skeleton.skeleton.anim_end;
      tracks.anim_fps = skeleton.skeleton.anim_fps;
      tracks.tracks = skeleton.skeleton.embedded_tracks;
    }
    else if (!stck_path.empty()) {
      load_tracks(ctx, stck_path, "", tracks);
    }
    if (!tracks.tracks.empty()) {
      import_action(ctx,
                    skeleton.skeleton,
                    armature,
                    tracks,
                    name_from_engine_path(bon_path),
                    0.0f,
                    -1.0f,
                    0.0f);
    }
  }
  return armature.object;
}

/** \} */

}  // namespace blender::io::angelica
