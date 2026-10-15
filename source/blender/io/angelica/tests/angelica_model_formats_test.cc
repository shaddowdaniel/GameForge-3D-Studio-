/* SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup angelica
 *
 * Tests for the model format parsers (`.ski`, `.bon`, `.stck`, `.smd`, `.ecm`, `.bmd`, `.mox`)
 * on small synthetic files built in memory. When the environment variable `ANGELICA_TEST_DATA`
 * points at a folder, every model file found below it must parse as well.
 */

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#if __has_include("testing/testing.h")
#  include "testing/testing.h"
#else
#  include <gtest/gtest.h>
#endif

#include "../formats/angelica_anim.hh"
#include "../formats/angelica_model.hh"
#include "../formats/angelica_skeleton.hh"
#include "../formats/angelica_skin.hh"
#include "../formats/angelica_static.hh"

namespace blender::io::angelica::tests {

/* -------------------------------------------------------------------- */
/** \name Writer Helpers
 * \{ */

class Writer {
 public:
  Bytes data;

  Writer &u8(uint8_t v)
  {
    data.push_back(v);
    return *this;
  }
  Writer &u16(uint16_t v)
  {
    return raw(&v, 2);
  }
  Writer &u32(uint32_t v)
  {
    return raw(&v, 4);
  }
  Writer &i32(int32_t v)
  {
    return raw(&v, 4);
  }
  Writer &f32(float v)
  {
    return raw(&v, 4);
  }
  Writer &vec3(float x, float y, float z)
  {
    return f32(x).f32(y).f32(z);
  }
  Writer &raw(const void *p, size_t n)
  {
    const uint8_t *b = static_cast<const uint8_t *>(p);
    data.insert(data.end(), b, b + n);
    return *this;
  }
  Writer &zeros(size_t n)
  {
    data.insert(data.end(), n, 0);
    return *this;
  }
  /** `AFile::WriteString`. */
  Writer &str(const std::string &s)
  {
    i32(int32_t(s.size()));
    return raw(s.data(), s.size());
  }
  /** NUL terminated string. */
  Writer &cstr(const std::string &s)
  {
    raw(s.data(), s.size());
    return u8(0);
  }
  /** Fixed size NUL padded field. */
  Writer &fixed(const std::string &s, size_t n)
  {
    std::string padded = s;
    padded.resize(n, '\0');
    return raw(padded.data(), n);
  }
  Writer &text(const std::string &s)
  {
    return raw(s.data(), s.size());
  }
  Writer &matrix_identity(float tx = 0.0f, float ty = 0.0f, float tz = 0.0f)
  {
    for (int i = 0; i < 16; i++) {
      f32(i == 12 ? tx : i == 13 ? ty : i == 14 ? tz : (i % 5 == 0 ? 1.0f : 0.0f));
    }
    return *this;
  }
  /** Binary `A3DMaterial`. */
  Writer &material(const std::string &name, float power = 20.0f, bool two_sided = true)
  {
    cstr("MATERIAL: " + name);
    for (int c = 0; c < 4; c++) {
      f32(0.1f * float(c + 1)).f32(0.2f).f32(0.3f).f32(0.5f);
    }
    f32(power);
    return u8(two_sided ? 1 : 0);
  }
};

static const char *MOXB = "MOXB";

/** Every proper prefix of a valid file must be rejected (and must not crash). */
template<typename ParseFn>
static void expect_truncations_fail(const Bytes &data,
                                    ParseFn parse,
                                    const std::vector<size_t> &valid_lengths = {})
{
  for (size_t len = 0; len < data.size(); len++) {
    if (std::find(valid_lengths.begin(), valid_lengths.end(), len) != valid_lengths.end()) {
      continue;
    }
    ByteSpan prefix(data.data(), len);
    EXPECT_FALSE(parse(prefix)) << "prefix of length " << len << " of " << data.size();
  }
}

/** Random corruption must never crash; the result does not matter. */
template<typename ParseFn> static void fuzz(const Bytes &data, ParseFn parse, int rounds = 300)
{
  std::mt19937 rng(1234);
  for (int round = 0; round < rounds; round++) {
    Bytes copy = data;
    const int flips = 1 + int(rng() % 8);
    for (int i = 0; i < flips && !copy.empty(); i++) {
      copy[rng() % copy.size()] = uint8_t(rng());
    }
    parse(ByteSpan(copy));
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Skin (.ski)
 * \{ */

static void write_skin_header(Writer &w,
                              uint32_t version,
                              int skin,
                              int rigid,
                              int morph_skin,
                              int morph_rigid,
                              int textures,
                              int materials,
                              int skin_bones,
                              int supple,
                              int muscle)
{
  w.u32(SKIN_FILE_IDENTIFY).u32(version);
  w.i32(skin).i32(rigid).i32(morph_skin).i32(morph_rigid).i32(textures).i32(materials);
  w.i32(skin_bones).f32(0.01f).i32(5).i32(supple).i32(muscle).zeros(52);
}

static void write_blend_vertices(Writer &w, int count)
{
  for (int i = 0; i < count; i++) {
    w.vec3(float(i), 1.0f, 2.0f);      /* Position. */
    w.f32(0.5f).f32(0.25f).f32(0.0f);  /* Weights. */
    w.u8(0).u8(1).u8(2).u8(0);         /* Bone indices. */
    w.vec3(0.0f, 1.0f, 0.0f);          /* Normal. */
    w.f32(0.1f * float(i)).f32(0.75f); /* UV. */
  }
}

static void write_tri_indices(Writer &w)
{
  w.u16(0).u16(1).u16(2);
}

static Bytes make_skin_v9()
{
  Writer w;
  w.raw(MOXB, 4);
  write_skin_header(w, 9, 1, 1, 1, 0, 1, 1, 2, 1, 1);
  w.str("Bip01").str("Bip01 Spine");
  w.str("\xce\xc6\xc0\xed.dds"); /* GBK "纹理.dds". */
  w.material("skin_mat");

  /* Skin mesh. */
  w.str("body").i32(0).i32(0).i32(3).i32(3);
  write_blend_vertices(w, 3);
  write_tri_indices(w);

  /* Rigid mesh on bone 1. */
  w.str("helmet").i32(1).i32(0).i32(0).i32(3).i32(3);
  for (int i = 0; i < 3; i++) {
    w.vec3(float(i), 0.0f, 0.0f).vec3(0.0f, 0.0f, -1.0f).f32(0.0f).f32(1.0f);
  }
  write_tri_indices(w);

  /* Morph skin mesh: 1 channel, frames 0..1, version 2 weight track (no segments). */
  w.str("face").i32(0).i32(0).i32(3).i32(3);
  write_blend_vertices(w, 3);
  write_tri_indices(w);
  w.i32(1).i32(0).i32(1).i32(30);
  w.str("smile");
  for (int i = 0; i < 3; i++) {
    w.vec3(0.0f, 0.1f * float(i), 0.0f).vec3(0.0f, 0.0f, 0.0f);
  }
  w.i32(2).i32(0).i32(30); /* Track info. */
  w.f32(0.0f).f32(1.0f);   /* Keys. */
  w.i32(33).i32(0);        /* Length, raw compression. */
  w.u8(0).u8(0);           /* Channel sort: 2 frames x 1 channel. */

  /* Supple mesh: 2 spring vertices, 3 render vertices, 1 spring. */
  w.str("cape").i32(0).i32(0).i32(2).i32(3).i32(3).i32(1).f32(1.0f).f32(0.5f).f32(2.0f);
  for (int i = 0; i < 2; i++) {
    w.vec3(float(i), 5.0f, 0.0f).f32(1.0f).f32(0.0f).f32(0.0f).u8(1).u8(0).u8(0).u8(0);
    w.f32(0.25f);
  }
  w.i32(0).vec3(0, 0, 1).f32(0).f32(0);
  w.i32(1).vec3(0, 0, 1).f32(1).f32(0);
  w.i32(1).vec3(0, 0, 1).f32(1).f32(1);
  write_tri_indices(w);
  w.i32(0).i32(1).f32(1.0f).f32(1.0f);

  /* Muscle mesh. */
  w.str("arm").i32(0).i32(0).i32(3).i32(3);
  write_blend_vertices(w, 3);
  write_tri_indices(w);
  return w.data;
}

TEST(angelica_skin, parse_version9_all_mesh_types)
{
  Skin skin;
  std::string error;
  ASSERT_TRUE(parse_skin(make_skin_v9(), skin, &error)) << error;
  EXPECT_EQ(skin.version, 9u);
  EXPECT_FALSE(skin.is_text);
  EXPECT_EQ(skin.skeleton_bone_count, 5);
  ASSERT_EQ(skin.bone_names.size(), 2u);
  EXPECT_EQ(skin.bone_names[1], "Bip01 Spine");
  ASSERT_EQ(skin.textures.size(), 1u);
  EXPECT_EQ(skin.textures[0], "\xe7\xba\xb9\xe7\x90\x86.dds");
  EXPECT_EQ(skin.textures_raw[0], "\xce\xc6\xc0\xed.dds");
  ASSERT_EQ(skin.materials.size(), 1u);
  EXPECT_EQ(skin.materials[0].name, "skin_mat");
  EXPECT_FLOAT_EQ(skin.materials[0].power, 20.0f);
  EXPECT_TRUE(skin.materials[0].two_sided);
  EXPECT_FLOAT_EQ(skin.materials[0].diffuse.r, 0.2f);
  EXPECT_FLOAT_EQ(skin.materials[0].ambient.a, 0.0f);

  ASSERT_EQ(skin.meshes.size(), 5u);
  const SkinMesh &body = skin.meshes[0];
  EXPECT_EQ(body.type, SkinMeshType::Skin);
  EXPECT_EQ(body.name, "body");
  EXPECT_EQ(body.vertex_count(), 3);
  EXPECT_FLOAT_EQ(body.positions[3], 1.0f);
  EXPECT_FLOAT_EQ(body.uvs[3], 0.75f);
  EXPECT_FLOAT_EQ(body.bone_weights[0], 0.5f);
  EXPECT_FLOAT_EQ(body.bone_weights[3], 0.25f);
  EXPECT_EQ(body.bone_indices[1], 1);
  EXPECT_EQ(body.indices.size(), 3u);
  EXPECT_TRUE(body.tangents.empty());

  const SkinMesh &helmet = skin.meshes[1];
  EXPECT_EQ(helmet.type, SkinMeshType::Rigid);
  EXPECT_EQ(helmet.bone_index, 1);
  EXPECT_FLOAT_EQ(helmet.normals[2], -1.0f);
  EXPECT_TRUE(helmet.bone_weights.empty());

  const SkinMesh &face = skin.meshes[2];
  EXPECT_EQ(face.type, SkinMeshType::MorphSkin);
  ASSERT_EQ(face.morph_channels.size(), 1u);
  EXPECT_EQ(face.morph_channels[0].name, "smile");
  EXPECT_FLOAT_EQ(face.morph_channels[0].position_deltas[4], 0.1f);
  ASSERT_EQ(face.morph_channels[0].weight_keys.size(), 2u);
  EXPECT_FLOAT_EQ(face.morph_channels[0].weight_keys[1], 1.0f);
  EXPECT_FLOAT_EQ(face.morph_channels[0].weight_key_frames[1], 1.0f);

  const SkinMesh &cape = skin.meshes[3];
  EXPECT_EQ(cape.type, SkinMeshType::Supple);
  EXPECT_EQ(cape.vertex_count(), 3);
  EXPECT_FLOAT_EQ(cape.positions[3], 1.0f); /* Render vertex 1 uses spring vertex 1. */
  EXPECT_EQ(cape.bone_indices[0], 1);
  EXPECT_FLOAT_EQ(cape.bone_weights[0], 1.0f);
  EXPECT_FLOAT_EQ(cape.supple_bone_factors[2], 0.25f);
  EXPECT_FLOAT_EQ(cape.supple_air_resist, 0.5f);

  EXPECT_EQ(skin.meshes[4].type, SkinMeshType::Muscle);
  EXPECT_EQ(skin.meshes[4].vertex_count(), 3);
}

TEST(angelica_skin, parse_tangent_version)
{
  Writer w;
  w.raw(MOXB, 4);
  write_skin_header(w, 101, 1, 0, 0, 0, 0, 0, 0, 0, 0);
  w.str("m").i32(-1).i32(-1).i32(3).i32(3);
  write_blend_vertices(w, 3);
  write_tri_indices(w);
  for (int i = 0; i < 3; i++) {
    w.f32(1.0f).f32(0.0f).f32(0.0f).f32(-1.0f);
  }
  Skin skin;
  std::string error;
  ASSERT_TRUE(parse_skin(w.data, skin, &error)) << error;
  ASSERT_EQ(skin.meshes.size(), 1u);
  ASSERT_EQ(skin.meshes[0].tangents.size(), 12u);
  EXPECT_FLOAT_EQ(skin.meshes[0].tangents[3], -1.0f);

  /* The same data as version 9 leaves 48 trailing bytes but parses. */
  w.data[8] = 9;
  EXPECT_TRUE(parse_skin(w.data, skin, &error)) << error;
  EXPECT_TRUE(skin.meshes[0].tangents.empty());
}

TEST(angelica_skin, parse_old_text_mode)
{
  /* Version 8 without "MOXB" tag: text mode materials, no bone names. */
  Writer w;
  write_skin_header(w, 8, 0, 1, 0, 0, 0, 1, 3, 0, 0);
  w.text("MATERIAL: stone\r\n{\r\nAmbient:(1.0, 1.0, 1.0, 1.0)\r\n");
  w.text("Diffuse:(0.5, 0.25, 0.125, 1.0)\r\nEmissive:(0, 0, 0, 0)\r\n");
  w.text("Specular:(0, 0, 0, 1)\r\nPower:(300.0)\r\n2Sided:(1)\r\n}\r\n");
  w.str("rock").i32(0).i32(-1).i32(0).i32(0).i32(0);
  Skin skin;
  std::string error;
  ASSERT_TRUE(parse_skin(w.data, skin, &error)) << error;
  EXPECT_TRUE(skin.is_text);
  EXPECT_TRUE(skin.bone_names.empty());
  ASSERT_EQ(skin.materials.size(), 1u);
  EXPECT_EQ(skin.materials[0].name, "stone");
  EXPECT_FLOAT_EQ(skin.materials[0].diffuse.g, 0.25f);
  EXPECT_FLOAT_EQ(skin.materials[0].power, 128.0f); /* Clamped. */
  EXPECT_TRUE(skin.materials[0].two_sided);
  ASSERT_EQ(skin.meshes.size(), 1u);
  EXPECT_EQ(skin.meshes[0].vertex_count(), 0);
}

TEST(angelica_skin, reject_malformed)
{
  std::string error;
  Skin skin;
  EXPECT_FALSE(parse_skin({}, skin, &error));

  Bytes data = make_skin_v9();
  Bytes bad_magic = data;
  bad_magic[4] = 'X';
  EXPECT_FALSE(parse_skin(bad_magic, skin, &error));

  Bytes bad_version = data;
  bad_version[8] = 10;
  EXPECT_FALSE(parse_skin(bad_version, skin, &error));
  EXPECT_NE(error.find("version"), std::string::npos);

  Bytes huge_count = data;
  const int32_t huge = 0x7fffffff;
  std::memcpy(&huge_count[12], &huge, 4);
  EXPECT_FALSE(parse_skin(huge_count, skin, &error));

  Bytes negative = data;
  const int32_t neg = -1;
  std::memcpy(&negative[28], &neg, 4); /* Material count. */
  EXPECT_FALSE(parse_skin(negative, skin, &error));

  auto parse = [](ByteSpan d) {
    Skin s;
    return parse_skin(d, s, nullptr);
  };
  expect_truncations_fail(data, parse);
  fuzz(data, parse);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Track Set (.stck)
 * \{ */

TEST(angelica_anim, parse_version1_segments)
{
  Writer w;
  w.raw(MOXB, 4);
  w.u32(TRACK_SET_IDENTIFY).u32(1).i32(1).i32(0).i32(20).i32(10);
  w.i32(7); /* Track id. */
  /* Position: 4 keys, segment 0 keys 0..2 over 0..200 ms, then key 3 held 200..1000 ms. */
  w.i32(4).i32(2).i32(10);
  for (int k = 0; k < 4; k++) {
    w.vec3(float(k), 0.0f, 0.0f);
  }
  w.i32(0).i32(200).i32(0).i32(2);
  w.i32(200).i32(1000).i32(3).i32(3);
  /* Rotation: 1 key, 1 static segment. */
  w.i32(1).i32(1).i32(10);
  w.f32(0.0f).f32(0.0f).f32(0.0f).f32(1.0f);
  w.i32(0).i32(1000).i32(0).i32(0);

  TrackSet set;
  std::string error;
  ASSERT_TRUE(parse_track_set(w.data, set, &error)) << error;
  EXPECT_EQ(set.version, 1u);
  EXPECT_EQ(set.anim_end, 20);
  EXPECT_EQ(set.anim_fps, 10);
  ASSERT_EQ(set.tracks.size(), 1u);
  const BoneTrack &t = set.tracks[0];
  EXPECT_EQ(t.id, 7);
  EXPECT_EQ(t.position.stored_key_count, 4);
  ASSERT_EQ(t.position.segments.size(), 2u);
  /* Keys at frames 0, 1, 2, then key 3 at frame 2 is skipped (not increasing)... */
  const std::vector<float> expected_frames = {0.0f, 1.0f, 2.0f, 9.0f};
  ASSERT_EQ(t.position.key_frames.size(), expected_frames.size());
  for (size_t i = 0; i < expected_frames.size(); i++) {
    EXPECT_FLOAT_EQ(t.position.key_frames[i], expected_frames[i]);
  }
  EXPECT_FLOAT_EQ(t.position.keys[3 * 3], 3.0f);
  ASSERT_EQ(t.rotation.key_count(), 2);
  EXPECT_FLOAT_EQ(t.rotation.key_frames[1], 9.0f);
  EXPECT_FLOAT_EQ(t.rotation.keys[3], 1.0f);
}

static Bytes make_stck_v2()
{
  Writer w;
  w.u32(TRACK_SET_IDENTIFY).u32(2).i32(2).i32(0).i32(30).i32(30);
  for (int id = 0; id < 2; id++) {
    w.i32(id);
    /* Raw position track. */
    w.i32(2).i32(0).i32(30).vec3(0, 0, 0).vec3(1, 2, 3).i32(33).i32(0);
    /* Quaternion track without w, compressed with key ids. */
    w.i32(2).i32(0).i32(30).i32(1);
    w.vec3(0.0f, 0.0f, 0.0f).vec3(0.6f, 0.0f, 0.0f);
    w.i32(1000).i32(1).u16(0).u16(30);
  }
  return w.data;
}

TEST(angelica_anim, parse_version2_compressed)
{
  TrackSet set;
  std::string error;
  ASSERT_TRUE(parse_track_set(make_stck_v2(), set, &error)) << error;
  ASSERT_EQ(set.tracks.size(), 2u);
  const BoneTrack &t = set.tracks[1];
  EXPECT_EQ(t.id, 1);
  EXPECT_FLOAT_EQ(t.position.key_frames[1], 1.0f);
  EXPECT_FLOAT_EQ(t.position.keys[5], 3.0f);
  EXPECT_TRUE(t.rotation.quaternion_without_w);
  EXPECT_EQ(t.rotation.compression, 1);
  EXPECT_FLOAT_EQ(t.rotation.key_frames[1], 30.0f);
  EXPECT_FLOAT_EQ(t.rotation.keys[3], 1.0f);
  EXPECT_NEAR(t.rotation.keys[7], 0.8f, 1e-6f);
}

TEST(angelica_anim, reject_malformed)
{
  std::string error;
  TrackSet set;
  Bytes data = make_stck_v2();
  Bytes bad_version = data;
  bad_version[4] = 3;
  EXPECT_FALSE(parse_track_set(bad_version, set, &error));

  Bytes duplicate = data;
  /* Second track id (offset 24 + 1 track) to 0. */
  const size_t track_size = 4 + (12 + 24 + 8) + (12 + 4 + 24 + 8 + 4);
  std::memset(&duplicate[24 + track_size], 0, 4);
  EXPECT_FALSE(parse_track_set(duplicate, set, &error));

  Bytes bad_compression = data;
  bad_compression[24 + 4 + 12 + 24 + 4] = 7;
  EXPECT_FALSE(parse_track_set(bad_compression, set, &error));

  auto parse = [](ByteSpan d) {
    TrackSet s;
    return parse_track_set(d, s, nullptr);
  };
  expect_truncations_fail(data, parse);
  fuzz(data, parse);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Skeleton (.bon)
 * \{ */

static void write_bone(Writer &w,
                       const std::string &name,
                       int parent,
                       int joint,
                       const std::vector<int> &children,
                       float tx)
{
  w.str(name).u8(1).zeros(3).i32(parent).i32(joint).i32(int(children.size()));
  w.matrix_identity(tx, 0, 0).matrix_identity(-tx, 0, 0);
  for (const int c : children) {
    w.i32(c);
  }
}

static Bytes make_skeleton(uint32_t version)
{
  Writer w;
  w.raw(MOXB, 4);
  w.u32(SKELETON_FILE_IDENTIFY).u32(version).i32(2).i32(2).i32(1).i32(0).i32(10).i32(30);
  w.zeros(64);
  write_bone(w, "Bip01", -1, 0, {1}, 0.0f);
  write_bone(w, "Bip01 Head", 0, 1, {}, 1.5f);
  w.u32(JOINT_CLASS_ANIM).u32(JOINT_CLASS_ANIM);
  for (int j = 0; j < 2; j++) {
    w.str("joint" + std::to_string(j)).i32(j - 1).i32(j).i32(-1);
    if (version < 6) {
      w.i32(1).i32(1).i32(30).vec3(0, float(j), 0).i32(0).i32(0).i32(0).i32(0);
      w.i32(1).i32(1).i32(30).f32(0).f32(0).f32(0).f32(1).i32(0).i32(0).i32(0).i32(0);
    }
  }
  w.str("HH_weapon").u32(0).i32(1).matrix_identity(0.0f, 0.25f, 0.0f);
  return w.data;
}

TEST(angelica_skeleton, parse_version6)
{
  Skeleton skel;
  std::string error;
  ASSERT_TRUE(parse_skeleton(make_skeleton(6), skel, &error)) << error;
  EXPECT_EQ(skel.version, 6u);
  EXPECT_EQ(skel.anim_fps, 30);
  ASSERT_EQ(skel.bones.size(), 2u);
  EXPECT_EQ(skel.bones[1].name, "Bip01 Head");
  EXPECT_EQ(skel.bones[1].parent, 0);
  EXPECT_EQ(skel.bones[1].first_joint, 1);
  EXPECT_EQ(skel.bones[1].flags, 1);
  EXPECT_FLOAT_EQ(skel.bones[1].relative[12], 1.5f);
  EXPECT_FLOAT_EQ(skel.bones[1].bone_init[12], -1.5f);
  ASSERT_EQ(skel.bones[0].children.size(), 1u);
  EXPECT_EQ(skel.find_bone("Bip01 Head"), 1);
  ASSERT_EQ(skel.joints.size(), 2u);
  EXPECT_EQ(skel.joints[1].class_id, uint32_t(JOINT_CLASS_ANIM));
  EXPECT_EQ(skel.joints[1].child_bone, 1);
  ASSERT_EQ(skel.hooks.size(), 1u);
  EXPECT_EQ(skel.hooks[0].name, "HH_weapon");
  EXPECT_EQ(skel.hooks[0].bone, 1);
  EXPECT_FLOAT_EQ(skel.hooks[0].matrix[13], 0.25f);
  EXPECT_TRUE(skel.embedded_tracks.empty());
}

TEST(angelica_skeleton, parse_version5_embedded_tracks)
{
  Skeleton skel;
  std::string error;
  ASSERT_TRUE(parse_skeleton(make_skeleton(5), skel, &error)) << error;
  ASSERT_EQ(skel.embedded_tracks.size(), 2u);
  EXPECT_EQ(skel.embedded_tracks[1].id, 1);
  EXPECT_FLOAT_EQ(skel.embedded_tracks[1].position.keys[1], 1.0f);
  ASSERT_EQ(skel.hooks.size(), 1u);
}

TEST(angelica_skeleton, reject_malformed)
{
  std::string error;
  Skeleton skel;
  Bytes data = make_skeleton(6);

  Bytes bad_version = data;
  bad_version[8] = 7;
  EXPECT_FALSE(parse_skeleton(bad_version, skel, &error));

  Bytes bad_parent = data;
  /* Parent of bone 1: header 100 + bone 0 (4 + 5 + 4 + 144 + 4) + name (4 + 10) + flags 4. */
  const size_t bone1_parent = 100 + (4 + 5 + 144 + 4) + (4 + 10) + 4;
  const int32_t parent = 5;
  std::memcpy(&bad_parent[bone1_parent], &parent, 4);
  EXPECT_FALSE(parse_skeleton(bad_parent, skel, &error));

  Bytes cycle = data;
  const size_t bone0_parent = 100 + 4 + 5 + 4;
  const int32_t one = 1;
  std::memcpy(&cycle[bone0_parent], &one, 4);
  EXPECT_FALSE(parse_skeleton(cycle, skel, &error));

  Bytes bad_joint = data;
  const size_t joint_ids = 100 + (4 + 5 + 144 + 4) + (4 + 10 + 144);
  bad_joint[joint_ids] = 99;
  EXPECT_FALSE(parse_skeleton(bad_joint, skel, &error));

  auto parse = [](ByteSpan d) {
    Skeleton s;
    return parse_skeleton(d, s, nullptr);
  };
  expect_truncations_fail(data, parse);
  expect_truncations_fail(make_skeleton(5), parse);
  fuzz(data, parse);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Skin Model (.smd)
 * \{ */

static Bytes make_smd_v9()
{
  Writer w;
  w.raw(MOXB, 4);
  w.u32(SKIN_MODEL_FILE_IDENTIFY).u32(9).i32(2).i32(2).i32(2).i32(1).zeros(60);
  w.str("hero.bon").str("hero_body.ski").str("").str("hero.phy").str("tcks_hero");
  w.str("stand").f32(0.0f).f32(30.0f).i32(0).str("stand.stck");
  w.str("run").f32(31.0f).f32(50.0f).i32(60).str("run.stck");
  w.i32(0).str("glow").str("HH_head").i32(0).u8(0).zeros(3).str("gfx\\glow.gfx");
  w.i32(1).str("sword").str("Bip01 R Hand").i32(1).u8(1).zeros(3).str("weapon\\sword.smd");
  w.str("CC_hand");
  w.str("height").str("1.8");
  return w.data;
}

TEST(angelica_model, parse_skin_model_v9)
{
  SkinModel model;
  std::string error;
  ASSERT_TRUE(parse_skin_model(make_smd_v9(), model, &error)) << error;
  EXPECT_EQ(model.version, 9u);
  EXPECT_EQ(model.skeleton_file, "hero.bon");
  ASSERT_EQ(model.skin_files.size(), 2u);
  EXPECT_EQ(model.skin_files[0], "hero_body.ski");
  EXPECT_TRUE(model.skin_files[1].empty());
  EXPECT_EQ(model.physique_file, "hero.phy");
  EXPECT_EQ(model.tracks_dir, "tcks_hero");
  ASSERT_EQ(model.actions.size(), 2u);
  EXPECT_EQ(model.actions[1].name, "run");
  EXPECT_FLOAT_EQ(model.actions[1].start_frame, 31.0f);
  EXPECT_EQ(model.actions[1].frame_rate, 60);
  EXPECT_EQ(model.actions[1].track_file, "run.stck");
  ASSERT_EQ(model.hangers.size(), 2u);
  EXPECT_EQ(model.hangers[0].type, SkinModelHangerType::Gfx);
  EXPECT_EQ(model.hangers[0].file, "gfx\\glow.gfx");
  EXPECT_EQ(model.hangers[1].type, SkinModelHangerType::SkinModel);
  EXPECT_TRUE(model.hangers[1].on_bone);
  EXPECT_EQ(model.hangers[1].child_hook, "CC_hand");
  ASSERT_EQ(model.properties.size(), 1u);
  EXPECT_EQ(model.properties[0].second, "1.8");
}

TEST(angelica_model, parse_skin_model_old_versions)
{
  /* Version 5: joint actions, tracks in the skeleton, derived track directory. */
  Writer w;
  w.u32(SKIN_MODEL_FILE_IDENTIFY).u32(5).i32(1).i32(1).i32(0).i32(0).zeros(60);
  w.str("old.bon").str("old.ski").str("");
  w.str("walk").i32(0).i32(2).u32(0);
  w.i32(0).f32(5.0f).f32(25.0f).i32(-1).f32(1.0f);
  w.i32(1).f32(6.0f).f32(26.0f).i32(-1).f32(1.0f);
  SkinModel model;
  std::string error;
  ASSERT_TRUE(parse_skin_model(w.data, model, &error, "Old.SMD")) << error;
  EXPECT_EQ(model.tracks_dir, "tcks_Old");
  ASSERT_EQ(model.actions.size(), 1u);
  EXPECT_FLOAT_EQ(model.actions[0].start_frame, 5.0f);
  EXPECT_FLOAT_EQ(model.actions[0].end_frame, 25.0f);
  EXPECT_TRUE(model.actions[0].track_file.empty());

  /* Version 7: two float frame range plus track file, derived directory. */
  Writer w7;
  w7.u32(SKIN_MODEL_FILE_IDENTIFY).u32(7).i32(0).i32(1).i32(0).i32(0).zeros(60);
  w7.str("").str("").str("jump").f32(1.0f).f32(9.0f).str("jump.stck");
  ASSERT_TRUE(parse_skin_model(w7.data, model, &error, "models\\a\\b.smd")) << error;
  EXPECT_EQ(model.tracks_dir, "tcks_b");
  EXPECT_EQ(model.actions[0].track_file, "jump.stck");
}

TEST(angelica_model, reject_malformed_skin_model)
{
  std::string error;
  SkinModel model;
  Bytes data = make_smd_v9();
  Bytes bad = data;
  bad[8] = 10;
  EXPECT_FALSE(parse_skin_model(bad, model, &error));

  Bytes bad_hanger = data;
  /* Find the first hanger type (int 0 right before "glow"). */
  const std::string glow = "glow";
  auto it = std::search(bad_hanger.begin(), bad_hanger.end(), glow.begin(), glow.end());
  ASSERT_NE(it, bad_hanger.end());
  *(it - 8) = 5;
  EXPECT_FALSE(parse_skin_model(bad_hanger, model, &error));

  auto parse = [](ByteSpan d) {
    SkinModel m;
    return parse_skin_model(d, m, nullptr);
  };
  expect_truncations_fail(data, parse);
  fuzz(data, parse);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name EC Model (.ecm)
 * \{ */

static std::string make_ecm_text_v45()
{
  std::string s = "MOXTVersion: 45\r\n";
  s += "SkinModelPath: models\\npcs\\\xb9\xd6\xce\xef\\monster.smd\r\n"; /* GBK "怪物". */
  s += "AutoUpdata: 0\r\n";
  s += "OrgColor: ff80ff80\r\n";
  s += "SrcBlend: 5\r\nDestBlend: 2\r\n";
  s += "OuterNum: 2\r\nFloat: 1.0\r\nFloat: 2.0\r\n";
  s += "NewScale: 1\r\nBoneNum: 1\r\nBoneIndex: 3\r\nBoneScale: 1.5, 1.0, 2.0\r\n";
  s += "DefSpeed: 1.25\r\nCanCastShadow: 0\r\nRenderModel: 1\r\nRenderEdge: 0\r\n";
  s += "ChannelCount: 1\r\nChannel: 1\r\nBoneNum: 1\r\nBip01 Head\r\n";
  s += "ChannelCount: 0\r\n";
  s += "CoGfxNum: 0\r\nComActCount: 1\r\n";
  /* A combined action the parser has to skip over. */
  s += "CombineActName: attack\r\nLoopCount: 1\r\nEventCount: 1\r\nEventType: 100\r\n";
  s += "ScriptCount: 0\r\n";
  s += "AddiSkinCount: 2\r\nAddiSkinPath: models\\a\\hair.ski\r\nAddiSkinPath: \r\n";
  s += "ChildCount: 1\r\nChildName: wing\r\nChildPath: models\\wings\\wing.ecm\r\n";
  s += "HHName: HH_back\r\nCCName: CC_wing\r\n";
  s += "PhysFileName: monster.mphy\r\n";
  s += "ECMHookCount: 1\r\nECMHook: EH_top\r\nID: 4\r\nScale: 2.0\r\n";
  s += "1 0 0\r\n0 1 0\r\n0 0 1\r\n0.5 3 0\r\n";
  return s;
}

static Bytes to_bytes(const std::string &s)
{
  return Bytes(s.begin(), s.end());
}

TEST(angelica_model, parse_ec_model_text)
{
  ECModel model;
  std::string error;
  ASSERT_TRUE(parse_ec_model(to_bytes(make_ecm_text_v45()), model, &error)) << error;
  EXPECT_TRUE(model.is_text);
  EXPECT_EQ(model.version, 45u);
  EXPECT_EQ(model.skin_model_path, "models\\npcs\\\xe6\x80\xaa\xe7\x89\xa9\\monster.smd");
  EXPECT_EQ(model.skin_model_path_raw, "models\\npcs\\\xb9\xd6\xce\xef\\monster.smd");
  EXPECT_FALSE(model.auto_update);
  EXPECT_EQ(model.org_color, 0xff80ff80u);
  EXPECT_EQ(model.dest_blend, 2);
  ASSERT_EQ(model.bone_scales.size(), 1u);
  EXPECT_EQ(model.bone_scales[0].bone_index, 3);
  EXPECT_TRUE(model.bone_scales[0].new_method);
  EXPECT_FLOAT_EQ(model.bone_scales[0].scale[2], 2.0f);
  EXPECT_FLOAT_EQ(model.default_play_speed, 1.25f);
  EXPECT_FALSE(model.can_cast_shadow);
  EXPECT_FALSE(model.render_edge);
  EXPECT_TRUE(model.children_parsed);
  ASSERT_EQ(model.additional_skins.size(), 1u);
  EXPECT_EQ(model.additional_skins[0], "models\\a\\hair.ski");
  ASSERT_EQ(model.children.size(), 1u);
  EXPECT_EQ(model.children[0].name, "wing");
  EXPECT_EQ(model.children[0].path, "models\\wings\\wing.ecm");
  EXPECT_EQ(model.children[0].parent_hook, "HH_back");
  EXPECT_EQ(model.children[0].child_hook, "CC_wing");
  EXPECT_EQ(model.physics_file, "monster.mphy");
  ASSERT_EQ(model.hooks.size(), 1u);
  EXPECT_EQ(model.hooks[0].name, "EH_top");
  EXPECT_EQ(model.hooks[0].id, 4);
  EXPECT_FLOAT_EQ(model.hooks[0].scale, 2.0f);
  EXPECT_FLOAT_EQ(model.hooks[0].matrix[13], 3.0f);
  EXPECT_FLOAT_EQ(model.hooks[0].matrix[15], 1.0f);
}

TEST(angelica_model, parse_ec_model_minimal_and_binary)
{
  ECModel model;
  std::string error;
  /* The shape of a minimal client file (header only). */
  ASSERT_TRUE(parse_ec_model(
      to_bytes("MOXTVersion: 1\r\nSkinModelPath: models\\m\\base\\base.smd\r\n"), model, &error))
      << error;
  EXPECT_EQ(model.skin_model_path, "models\\m\\base\\base.smd");

  /* Version 6: children without additional skins, no tag (text mode). */
  ASSERT_TRUE(parse_ec_model(to_bytes("Version: 6\nSkinModelPath: a.smd\nComActCount: 0\n"
                                      "ChildCount: 1\nChildName: c\nChildPath: b.ecm\n"
                                      "HHName: h\nCCName: cc\n"),
                             model,
                             &error))
      << error;
  ASSERT_EQ(model.children.size(), 1u);
  EXPECT_EQ(model.children[0].path, "b.ecm");

  Writer w;
  w.raw(MOXB, 4).u32(40).str("x\\y.smd").u32(0xffffffff).i32(5).i32(6);
  w.i32(1).f32(1.0f);
  w.i32(1).i32(2).i32(0).vec3(1, 2, 3);
  w.i32(0).i32(0);
  w.i32(1).str("child").str("c.ecm").str("HH").str("CC");
  ASSERT_TRUE(parse_ec_model(w.data, model, &error)) << error;
  EXPECT_FALSE(model.is_text);
  EXPECT_EQ(model.skin_model_path, "x\\y.smd");
  ASSERT_EQ(model.bone_scales.size(), 1u);
  EXPECT_FLOAT_EQ(model.bone_scales[0].scale[1], 2.0f);
  ASSERT_EQ(model.children.size(), 1u);
  EXPECT_EQ(model.children[0].child_hook, "CC");
}

TEST(angelica_model, reject_malformed_ec_model)
{
  ECModel model;
  std::string error;
  EXPECT_FALSE(parse_ec_model({}, model, &error));
  EXPECT_FALSE(parse_ec_model(to_bytes("hello world\n"), model, &error));
  EXPECT_FALSE(parse_ec_model(to_bytes("Version: 99\nSkinModelPath: a.smd\n"), model, &error));
  EXPECT_FALSE(parse_ec_model(to_bytes("Version: 10\n"), model, &error));
  EXPECT_FALSE(parse_ec_model(to_bytes("Version: 10\nSkinModelPath: a\nComActCount: 0\n"
                                       "AddiSkinCount: 1000000\n"),
                              model,
                              &error));
  auto parse = [](ByteSpan d) {
    ECModel m;
    return parse_ec_model(d, m, nullptr);
  };
  fuzz(to_bytes(make_ecm_text_v45()), parse, 1000);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Lit Model (.bmd)
 * \{ */

static void write_lit_mesh(Writer &w, uint32_t version)
{
  w.u32(version).fixed("wall", 64).fixed("textures\\building\\wall.dds", 256);
  w.i32(3).i32(1);
  if (version == 0x10000006) {
    w.u8(1);
  }
  const bool legacy = version <= 0x10000003;
  for (int i = 0; i < 3; i++) {
    w.vec3(float(i), 0.0f, 1.0f).u32(0xff112233);
    if (legacy) {
      w.u32(0);
    }
    w.f32(0.5f).f32(float(i));
  }
  w.u16(0).u16(2).u16(1);
  for (int i = 0; i < 3; i++) {
    w.vec3(0.0f, 1.0f, 0.0f);
  }
  if (version >= 0x10000003) {
    for (int i = 0; i < 3; i++) {
      w.u32(0xffaaaaaa);
    }
    for (int i = 0; i < 3; i++) {
      w.u32(0xff555555);
    }
  }
  if (version == 0x10000006) {
    w.zeros(3 * 4 * 2);
  }
  w.vec3(1, 0, 0).vec3(1, 0, 0).vec3(0, 0, 0).vec3(2, 0, 1);
  if (version >= 0x10000005) {
    w.material("wall_mat");
  }
  if (version >= 0x10000100) {
    for (int i = 0; i < 3; i++) {
      w.f32(0.25f).f32(0.5f);
    }
    if (version >= 0x10000101) {
      w.u8(1);
    }
  }
}

static Bytes make_bmd_v2()
{
  Writer w;
  w.raw(MOXB, 4);
  w.u32(BRUSH_BUILDING_VERSION).u8(1);
  w.u32(LIT_MODEL_VERSION_2);
  w.vec3(2, 2, 2).vec3(1, 0, 0).vec3(0, 1, 0).vec3(10, 20, 30);
  w.i32(2);
  write_lit_mesh(w, 0x10000006);
  write_lit_mesh(w, 0x10000002);
  return w.data;
}

TEST(angelica_static, parse_bmd_building)
{
  Bytes data = make_bmd_v2();
  Writer tail;
  tail.i32(0); /* Collision data. */
  data.insert(data.end(), tail.data.begin(), tail.data.end());

  LitModel model;
  std::string error;
  ASSERT_TRUE(parse_bmd(data, model, &error)) << error;
  EXPECT_EQ(model.building_version, BRUSH_BUILDING_VERSION);
  EXPECT_TRUE(model.collide_only);
  EXPECT_EQ(model.version, uint32_t(LIT_MODEL_VERSION_2));
  EXPECT_TRUE(model.has_collision_data);
  /* dir = +X, up = +Y: x axis = up x dir = -Z, scaled by 2. */
  EXPECT_FLOAT_EQ(model.world_matrix[2], -2.0f);
  EXPECT_FLOAT_EQ(model.world_matrix[8], 2.0f);
  EXPECT_FLOAT_EQ(model.world_matrix[14], 30.0f);
  ASSERT_EQ(model.meshes.size(), 2u);
  const LitMesh &m = model.meshes[0];
  EXPECT_EQ(m.name, "wall");
  EXPECT_EQ(m.texture, "textures\\building\\wall.dds");
  EXPECT_EQ(m.vertex_count(), 3);
  EXPECT_EQ(m.indices[1], 2);
  EXPECT_FLOAT_EQ(m.uvs[5], 2.0f);
  EXPECT_EQ(m.day_colors[0], 0xffaaaaaau);
  EXPECT_EQ(m.night_colors[2], 0xff555555u);
  EXPECT_EQ(m.day_colors_extra.size(), 3u);
  EXPECT_TRUE(m.has_material);
  EXPECT_EQ(m.material.name, "wall_mat");
  EXPECT_FLOAT_EQ(m.aabb_max[0], 2.0f);
  const LitMesh &legacy = model.meshes[1];
  EXPECT_EQ(legacy.day_colors[0], 0xff112233u);
  EXPECT_FLOAT_EQ(legacy.uvs[3], 1.0f);
  EXPECT_FALSE(legacy.has_material);
}

TEST(angelica_static, parse_bmd_lightmap_and_extended)
{
  Writer w;
  w.raw(MOXB, 4);
  w.u32(LIT_MODEL_VERSION_LIGHTMAP).vec3(1, 1, 1).vec3(0, 0, 1).vec3(0, 1, 0).vec3(0, 0, 0);
  w.i32(1);
  write_lit_mesh(w, 0x10000102);
  w.fixed("lm_day.dds", 260).fixed("lm_night.dds", 260);
  LitModel model;
  std::string error;
  ASSERT_TRUE(parse_bmd(w.data, model, &error)) << error;
  EXPECT_EQ(model.lightmap, "lm_day.dds");
  EXPECT_EQ(model.night_lightmap, "lm_night.dds");
  ASSERT_EQ(model.meshes.size(), 1u);
  EXPECT_TRUE(model.meshes[0].use_lightmap);
  EXPECT_FLOAT_EQ(model.meshes[0].lightmap_uvs[1], 0.5f);
  EXPECT_FALSE(model.has_collision_data);
  EXPECT_FLOAT_EQ(model.world_matrix[0], 1.0f);

  /* Extended version, inline mesh with light map coordinates, colors skipped (LM used). */
  Writer x;
  x.raw(MOXB, 4);
  x.u32(LIT_MODEL_VERSION_EX).vec3(1, 1, 1).vec3(0, 0, 1).vec3(0, 1, 0).vec3(0, 0, 0);
  x.i32(1).u8(1).u8(0);
  x.u8(1).u8(1).str("inline").str("tex\\a.dds").i32(3).i32(1);
  for (int i = 0; i < 3; i++) {
    x.vec3(float(i), 0, 0).u32(0xffffffff).f32(0).f32(0);
  }
  x.u16(0).u16(1).u16(2);
  for (int i = 0; i < 3; i++) {
    x.vec3(0, 1, 0);
  }
  x.material("m");
  for (int i = 0; i < 3; i++) {
    x.f32(0.1f).f32(0.2f);
  }
  x.zeros(48);
  /* Mesh extra block: size, tile, hardness, detail texture, 4 unknown bytes. */
  x.u32(4 + 4 + 4 + 4 + 10 + 4).f32(4.0f).f32(0.5f).str("detail.dds").u32(0);
  x.str("day.dds").str("night.dds");
  x.u32(8).u32(0); /* Model extra block. */
  ASSERT_TRUE(parse_bmd(x.data, model, &error)) << error;
  ASSERT_EQ(model.meshes.size(), 1u);
  EXPECT_EQ(model.meshes[0].name, "inline");
  EXPECT_EQ(model.meshes[0].detail_texture, "detail.dds");
  EXPECT_FLOAT_EQ(model.meshes[0].detail_tile, 4.0f);
  EXPECT_TRUE(model.meshes[0].day_colors.empty());
  EXPECT_EQ(model.night_lightmap, "night.dds");

  /* The newer extended versions: 0x10000302 has 32-bit indices; 0x10000305 also an int32 after the
   * transform and four floats before each mesh's material. Larger extra blocks, then a building's
   * (empty) collision data. */
  for (const uint32_t version : {0x10000302u, 0x10000305u}) {
    Writer y;
    y.raw(MOXB, 4).u32(BRUSH_BUILDING_VERSION).u8(0);
    y.u32(version).vec3(1, 1, 1).vec3(0, 0, 1).vec3(0, 1, 0).vec3(10, 20, 30);
    if (version >= 0x10000303) {
      y.i32(-1);
    }
    y.i32(1).u8(0).u8(0);
    y.u8(0).u8(0).str("Plane01").str("tex\\b.dds").i32(3).i32(1);
    for (int i = 0; i < 3; i++) {
      y.vec3(float(i), 0, 0).u32(0xff767678).f32(0).f32(0);
    }
    y.u32(2).u32(1).u32(0);
    for (int i = 0; i < 3; i++) {
      y.vec3(0, 1, 0);
    }
    if (version >= 0x10000305) {
      y.f32(1).f32(1).f32(1).f32(1);
    }
    y.material("m");
    for (int i = 0; i < 6; i++) {
      y.u32(0xff767678);
    }
    y.zeros(48);
    y.u32(4 + 4 + 4 + 4 + 8).f32(5.0f).f32(1.0f).str("").zeros(8);
    y.u32(36).zeros(32);
    y.i32(0);
    ASSERT_TRUE(parse_bmd(y.data, model, &error)) << error;
    ASSERT_EQ(model.meshes.size(), 1u);
    EXPECT_TRUE(model.meshes[0].indices.empty());
    ASSERT_EQ(model.meshes[0].indices32.size(), 3u);
    EXPECT_EQ(model.meshes[0].indices32[0], 2u);
    EXPECT_EQ(model.meshes[0].material.name, "m");
    EXPECT_EQ(model.meshes[0].day_colors.size(), 3u);
    EXPECT_FLOAT_EQ(model.meshes[0].detail_tile, 5.0f);
    EXPECT_FLOAT_EQ(model.world_matrix[13], 20.0f);
    EXPECT_TRUE(model.has_collision_data);
  }
}

TEST(angelica_static, parse_bmd_shared)
{
  Writer shared;
  shared.raw(MOXB, 4).u32(LIT_SHARED_DATA_MAGIC).u32(1).u32(1);
  shared.i32(3).i32(3).u8(0);
  for (int i = 0; i < 3; i++) {
    shared.vec3(float(i), 1, 1);
  }
  for (int i = 0; i < 3; i++) {
    shared.vec3(0, 0, 1);
  }
  for (int i = 0; i < 3; i++) {
    shared.f32(0).f32(1);
  }
  shared.u16(2).u16(1).u16(0).str("shared.dds").material("sm");

  Writer w;
  w.u32(LIT_MODEL_VERSION_SHARED).vec3(1, 1, 1).vec3(0, 0, 1).vec3(0, 1, 0).vec3(0, 0, 0);
  w.u8(0).str("litmodels\\x.lmsd").i32(1);
  w.u8(0).u8(0);
  for (int i = 0; i < 6; i++) {
    w.u32(0xff000000u + uint32_t(i));
  }
  w.zeros(48);

  LitModel model;
  std::string error;
  EXPECT_FALSE(parse_bmd(w.data, model, &error));
  EXPECT_NE(error.find("x.lmsd"), std::string::npos);

  auto loader = [&](const std::string &path, Bytes &r_data) {
    if (path != "litmodels\\x.lmsd") {
      return false;
    }
    r_data = shared.data;
    return true;
  };
  ASSERT_TRUE(parse_bmd(w.data, model, &error, loader)) << error;
  ASSERT_EQ(model.meshes.size(), 1u);
  EXPECT_EQ(model.meshes[0].texture, "shared.dds");
  EXPECT_EQ(model.meshes[0].indices[0], 2);
  EXPECT_EQ(model.meshes[0].night_colors[2], 0xff000005u);
  EXPECT_EQ(model.meshes[0].material.name, "sm");
}

TEST(angelica_static, reject_malformed_bmd)
{
  LitModel model;
  std::string error;
  EXPECT_FALSE(parse_bmd({}, model, &error));
  Bytes data = make_bmd_v2();
  Bytes bad = data;
  bad[9] = 0x77; /* Lit model version. */
  EXPECT_FALSE(parse_bmd(bad, model, &error));
  Bytes bad_mesh = data;
  bad_mesh[9 + 4 + 48 + 4] = 0x01; /* First mesh version -> 0x10000001. */
  EXPECT_FALSE(parse_bmd(bad_mesh, model, &error));

  auto parse = [](ByteSpan d) {
    LitModel m;
    return parse_bmd(d, m, nullptr);
  };
  expect_truncations_fail(data, parse);
  fuzz(data, parse);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Frame Model (.mox)
 * \{ */

static void write_mox_mesh(Writer &w, int version, int frames)
{
  if (version != 1) {
    w.cstr("VERSION: " + std::to_string(version));
  }
  w.cstr("MESH: leaf").i32(3).i32(3).i32(frames).u8(1).u8(0);
  if (version == 1) {
    w.i32(40).u8(1).u8(0).u8(0).u8(0).i32(7).u8(1).u8(0).zeros(30);
  }
  else {
    w.u32(0x41).i32(3);
  }
  w.u16(0).u16(1).u16(2);
  w.f32(10.0f).f32(100.0f).i32(2).u16(0).u16(1).u16(2); /* LOD. */
  for (int f = 0; f < frames; f++) {
    for (int i = 0; i < 3; i++) {
      w.vec3(float(i + f), 0, 0).vec3(0, 1, 0).f32(0.5f).f32(float(i));
    }
  }
  w.cstr("DETAILTEXTURE: detail.dds").matrix_identity(0.0f, 0.0f, 0.0f);
  w.cstr("TEXTURE: \xca\xf7\xd2\xb6.tga"); /* GBK "树叶.tga". */
  w.material("leaf_mat");
  if (version == 100) {
    for (int f = 0; f < frames; f++) {
      for (int i = 0; i < 3; i++) {
        w.f32(0.25f).f32(0.75f);
      }
    }
  }
}

static void write_mox_frame(Writer &w, int version, int meshes, int children, int depth)
{
  if (version != 1) {
    w.cstr("VERSION: " + std::to_string(version));
  }
  w.cstr("FRAME: f" + std::to_string(depth)).i32(1).matrix_identity(0, float(depth), 0);
  w.i32(1).fixed("box", 32);
  for (int i = 0; i < 8; i++) {
    w.vec3(float(i), 0, 0);
  }
  if (version == 1) {
    w.u8(1).u8(0).u8(1).u8(0).i32(3).zeros(32);
  }
  else {
    w.u32(6).i32(2);
  }
  w.u32(uint32_t(meshes));
  for (int i = 0; i < meshes; i++) {
    write_mox_mesh(w, i == 0 ? 2 : 100, 2);
  }
  w.u32(uint32_t(children));
  for (int i = 0; i < children; i++) {
    write_mox_frame(w, 1, 1, 0, depth + 1);
  }
}

static Bytes make_mox()
{
  Writer w;
  w.raw(MOXB, 4);
  write_mox_frame(w, 2, 2, 1, 0);
  return w.data;
}

TEST(angelica_static, parse_mox)
{
  MoxFrame root;
  std::string error;
  ASSERT_TRUE(parse_mox(make_mox(), root, &error)) << error;
  EXPECT_EQ(root.version, 2);
  EXPECT_EQ(root.name, "f0");
  ASSERT_EQ(root.relative_tms.size(), 1u);
  ASSERT_EQ(root.obbs.size(), 1u);
  EXPECT_EQ(root.obbs[0].name, "box");
  EXPECT_FLOAT_EQ(root.obbs[0].extents[0], 7.0f);
  EXPECT_EQ(root.obbs[0].props, 6u);
  ASSERT_EQ(root.meshes.size(), 2u);
  const MoxMesh &m = root.meshes[0];
  EXPECT_EQ(m.version, 2);
  EXPECT_EQ(m.name, "leaf");
  EXPECT_EQ(m.props, 0x41u);
  EXPECT_EQ(m.material_type, 3);
  EXPECT_EQ(m.frame_count, 2);
  ASSERT_EQ(m.animated_positions.size(), 1u);
  EXPECT_FLOAT_EQ(m.animated_positions[0][3], 2.0f);
  EXPECT_FLOAT_EQ(m.uvs[5], 2.0f);
  EXPECT_EQ(m.texture, "\xe6\xa0\x91\xe5\x8f\xb6.tga");
  EXPECT_EQ(m.detail_texture, "detail.dds");
  EXPECT_EQ(m.material.name, "leaf_mat");
  EXPECT_EQ(root.meshes[1].version, 100);
  EXPECT_FLOAT_EQ(root.meshes[1].lightmap_uvs[1], 0.75f);
  ASSERT_EQ(root.children.size(), 1u);
  const MoxFrame &child = root.children[0];
  EXPECT_EQ(child.version, 1);
  EXPECT_FLOAT_EQ(child.relative_tms[0][13], 1.0f);
  EXPECT_EQ(child.obbs[0].props, 5u);
  ASSERT_EQ(child.meshes.size(), 1u);

  /* Version 1 mesh properties. */
  Writer w;
  w.raw(MOXB, 4).cstr("FRAME: one").i32(1).matrix_identity().i32(0).u32(1);
  write_mox_mesh(w, 1, 1);
  ASSERT_TRUE(parse_mox(w.data, root, &error)) << error;
  ASSERT_EQ(root.meshes.size(), 1u);
  EXPECT_EQ(root.meshes[0].props, 0x11u); /* 2 sided + on ground. */
  EXPECT_EQ(root.meshes[0].material_type, 7);
  EXPECT_TRUE(root.children.empty()); /* Missing child count at the end is allowed. */
}

TEST(angelica_static, reject_malformed_mox)
{
  MoxFrame root;
  std::string error;
  EXPECT_FALSE(parse_mox({}, root, &error));
  EXPECT_FALSE(parse_mox(to_bytes("MOXTFRAME: x\n{\n"), root, &error));

  /* Recursion bomb: 200 nested frames. */
  Writer w;
  w.raw(MOXB, 4);
  for (int i = 0; i < 200; i++) {
    w.cstr("FRAME: x").i32(0).i32(0).u32(0).u32(1);
  }
  EXPECT_FALSE(parse_mox(w.data, root, &error));

  Bytes data = make_mox();
  auto parse = [](ByteSpan d) {
    MoxFrame f;
    return parse_mox(d, f, nullptr);
  };
  /* A file may end right before a frame's child count (read as zero). That makes the prefix
   * ending at the root's child count valid as well as the one without the trailing child count of
   * the last frame. Every other prefix must fail. */
  const std::string child_tag = "FRAME: f1";
  const auto child_it = std::search(data.begin(), data.end(), child_tag.begin(), child_tag.end());
  ASSERT_NE(child_it, data.end());
  const size_t root_child_count = size_t(child_it - data.begin()) - 4;
  ASSERT_TRUE(parse(ByteSpan(data.data(), root_child_count)));
  Bytes trimmed(data.begin(), data.end() - 4);
  ASSERT_TRUE(parse(trimmed));
  expect_truncations_fail(trimmed, parse, {root_child_count});
  fuzz(data, parse);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Real Files
 * \{ */

TEST(angelica_model_formats, real_files)
{
  const char *root = std::getenv("ANGELICA_TEST_DATA");
  if (root == nullptr || root[0] == '\0') {
    GTEST_SKIP() << "ANGELICA_TEST_DATA is not set";
  }
  namespace fs = std::filesystem;
  std::error_code ec;
  if (!fs::is_directory(fs::path(root), ec)) {
    GTEST_SKIP() << "ANGELICA_TEST_DATA is not a directory";
  }
  int parsed = 0;
  for (const fs::directory_entry &entry : fs::recursive_directory_iterator(fs::path(root), ec)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    std::string ext = entry.path().extension().string();
    for (char &c : ext) {
      c = char(std::tolower(uint8_t(c)));
    }
    const std::string path = entry.path().string();
    Bytes data;
    std::string error;
    bool ok = true;
    if (ext == ".ski") {
      Skin skin;
      ASSERT_TRUE(read_file(path, data));
      ok = parse_skin(data, skin, &error);
    }
    else if (ext == ".bon") {
      Skeleton skel;
      ASSERT_TRUE(read_file(path, data));
      ok = parse_skeleton(data, skel, &error);
    }
    else if (ext == ".stck") {
      TrackSet set;
      ASSERT_TRUE(read_file(path, data));
      ok = parse_track_set(data, set, &error);
    }
    else if (ext == ".smd") {
      SkinModel model;
      ASSERT_TRUE(read_file(path, data));
      ok = parse_skin_model(data, model, &error, entry.path().filename().string());
    }
    else if (ext == ".ecm") {
      ECModel model;
      ASSERT_TRUE(read_file(path, data));
      ok = parse_ec_model(data, model, &error);
    }
    else if (ext == ".bmd") {
      LitModel model;
      ASSERT_TRUE(read_file(path, data));
      ok = parse_bmd(data, model, &error);
    }
    else if (ext == ".mox") {
      MoxFrame frame;
      ASSERT_TRUE(read_file(path, data));
      ok = parse_mox(data, frame, &error);
    }
    else {
      continue;
    }
    EXPECT_TRUE(ok) << path << ": " << error;
    parsed++;
  }
  if (parsed == 0) {
    /* ANGELICA_TEST_DATA is shared with the other real-file tests; a client folder keeps its
     * models inside packages, which `angelica_pck.real_packages` covers. */
    GTEST_SKIP() << "no loose model files below " << root;
  }
}

/** \} */

}  // namespace blender::io::angelica::tests
