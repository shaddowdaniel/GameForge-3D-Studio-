/* SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup angelica
 *
 * Parser for Angelica Engine skin files (`.ski`, `A3DSkin`), plus the `AFile` helpers shared by
 * the other model parsers (file mode tag, NUL terminated strings, text lines, `A3DMaterial`).
 *
 * Conventions for all model formats (`angelica_skin`, `angelica_skeleton`, `angelica_anim`,
 * `angelica_model`, `angelica_static`):
 * - Data is kept exactly as stored on disk. The engine is Direct3D based: left-handed, +Y up,
 *   +Z forward, one unit is one meter.
 * - Matrices are 16 floats, row-major as in `A3DMATRIX4::m[4][4]`, for row vectors
 *   (`v' = v * M`): rows 0..2 are the X/Y/Z axes, row 3 (`m[12..14]`) is the translation.
 * - Quaternions are stored `x, y, z, w` and turned into matrices with
 *   `D3DXMatrixRotationQuaternion`, so for row vectors they rotate like the usual `q * v * q^-1`.
 * - Texture coordinates use the Direct3D convention (V points down), triangles are clockwise
 *   front facing. Mirroring into a right-handed system is the importer's job.
 * - Names and paths are converted from GBK to UTF-8. Engine paths are additionally kept as the
 *   raw GBK bytes in `*_raw` members so they can be looked up in `.pck` packages with
 *   #normalize_engine_path().
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "angelica_reader.hh"

namespace blender::io::angelica {

/* -------------------------------------------------------------------- */
/** \name AFile Helpers
 * \{ */

/** `AFile` FOURCC tags written in front of files saved through `AFile` ("MOXB" / "MOXT"). */
constexpr uint32_t AFILE_TAG_BINARY = 0x42584f4d;
constexpr uint32_t AFILE_TAG_TEXT = 0x54584f4d;

/**
 * The content of a file as `AFile` / `AFileImage` sees it. A leading "MOXB" tag selects binary
 * mode and is skipped, "MOXT" selects text mode and is skipped, anything else is text mode with
 * nothing skipped. Text mode only changes how strings and materials are read
 * (`ReadLine` instead of NUL terminated strings).
 */
struct AFileContent {
  ByteSpan body;
  bool is_text = true;
};

AFileContent afile_open(ByteSpan data);

/**
 * `AFile::ReadString(char *, len)`: bytes up to a NUL (consumed). Fails the reader when no NUL is
 * found within `max_len` bytes (the engine buffers are 2048 bytes).
 */
std::string afile_read_cstring(BinaryReader &reader, size_t max_len = 2048);

/**
 * `AFile::ReadLine`: bytes up to and including `\n`, with up to two trailing `\r` / `\n` removed.
 * Returns false at the end of the data.
 */
bool afile_read_line(BinaryReader &reader, std::string &r_line);

/** RGBA color as `A3DCOLORVALUE` (floats, usually 0..1). */
struct Color4f {
  float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f;
};

/** `A3DMaterial` (fixed function D3D material). */
struct MaterialInfo {
  std::string name;
  Color4f ambient;
  Color4f diffuse;
  Color4f emissive;
  Color4f specular;
  /** Specular power, clamped to [-128, 128] like the engine does. */
  float power = 0.0f;
  bool two_sided = false;
};

/**
 * `A3DMaterial::Load`. Binary: "MATERIAL: name" NUL string, 4 x `A3DCOLORVALUE`, `float` power,
 * `bool` two sided. Text: the same values as `MATERIAL: name`, `{`, `Ambient:(r, g, b, a)`, ...,
 * `Power:(p)`, `2Sided:(n)`, `}` lines.
 */
bool afile_read_material(BinaryReader &reader,
                         bool is_text,
                         MaterialInfo &r_material,
                         std::string *r_error);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Skin (.ski)
 * \{ */

constexpr uint32_t SKIN_FILE_IDENTIFY = fourcc('A', 'S', 'K', 'I');
/** Highest regular version (Angelica 2.x). Versions 1..9 are accepted. */
constexpr uint32_t SKIN_FILE_VERSION = 9;
/** Angelica 2.2 versions with a per vertex `A3DVECTOR4` tangent after the indices. */
constexpr uint32_t SKIN_FILE_VERSION_TANGENT_OLD = 100;
constexpr uint32_t SKIN_FILE_VERSION_TANGENT = 101;

enum class SkinMeshType : uint8_t {
  /** `A3DSkinMesh`: up to four bones per vertex. */
  Skin,
  /** `A3DRigidMesh`: every vertex follows #SkinMesh::bone_index with weight 1. */
  Rigid,
  /** `A3DMorphSkinMesh`: skin mesh plus morph channels. */
  MorphSkin,
  /** `A3DMorphRigidMesh`: rigid mesh plus morph channels. */
  MorphRigid,
  /** `A3DSuppleMesh`: spring simulated mesh, imported with its bind pose skinning. */
  Supple,
  /** `A3DMuscleMesh`: skin mesh deformed by a muscle organ at runtime. */
  Muscle,
};

/** One morph target (`A3DMorphMesh` channel). Deltas are relative to the base mesh. */
struct MorphChannel {
  std::string name;
  /** 3 floats per base vertex. */
  std::vector<float> position_deltas;
  /** 3 floats per base vertex. */
  std::vector<float> normal_deltas;
  /** Channel weight keys (`A3DFloatTrack`), one per key, sampled at #frame_rate. */
  std::vector<float> weight_keys;
  /** Frame number of each key in #weight_keys (same length). */
  std::vector<float> weight_key_frames;
  int frame_rate = 0;
};

struct SkinMesh {
  SkinMeshType type = SkinMeshType::Skin;
  std::string name;
  /** Index into #Skin::textures, may be out of range (or -1) for "no texture". */
  int texture_index = -1;
  /** Index into #Skin::materials, may be out of range for "no material". */
  int material_index = -1;
  /** Rigid meshes: bone the mesh is attached to (same index space as #bone_indices). */
  int bone_index = -1;

  /** 3 floats per vertex. */
  std::vector<float> positions;
  /** 3 floats per vertex. */
  std::vector<float> normals;
  /** 2 floats per vertex (Direct3D convention, V down). */
  std::vector<float> uvs;
  /**
   * 4 floats per vertex, only for skin versions 100 / 101, as stored on disk:
   * version 101 is (tangent.xyz, handedness), version 100 the engine turns into a binormal.
   */
  std::vector<float> tangents;

  /**
   * Skin / morph skin / supple / muscle meshes: 4 bone indices per vertex (`dwMatIndices` bytes).
   * For skin version >= 9 they index #Skin::bone_names, for older versions they index the
   * skeleton's bones directly. An index equal to the bone count means "the model itself".
   */
  std::vector<uint8_t> bone_indices;
  /** 4 weights per vertex. The file stores 3, the 4th is `1 - (w0 + w1 + w2)` clamped to >= 0. */
  std::vector<float> bone_weights;

  /** Triangle list. */
  std::vector<uint16_t> indices;

  /* Morph meshes. */
  int morph_anim_start = 0;
  int morph_anim_end = 0;
  int morph_anim_fps = 0;
  std::vector<MorphChannel> morph_channels;

  /* Supple meshes (cloth like spring simulation parameters). */
  float supple_gravity_factor = 0.0f;
  float supple_air_resist = 0.0f;
  float supple_vertex_weight = 0.0f;
  /** Per vertex "bone factor" of the spring vertex (1 = fully driven by the bones). */
  std::vector<float> supple_bone_factors;

  int vertex_count() const
  {
    return int(positions.size() / 3);
  }
};

struct Skin {
  /** File version: 1..9, 100 or 101. */
  uint32_t version = 0;
  /** True when the file was read in `AFile` text mode (no "MOXB" tag). */
  bool is_text = false;
  float min_weight = 0.0f;
  /** Number of bones of the skeleton the skin was exported against. */
  int skeleton_bone_count = 0;
  /** Skin bone names (versions >= 9), matched by name against the skeleton. */
  std::vector<std::string> bone_names;
  /**
   * Texture file names (UTF-8) and their raw GBK bytes. The engine looks for them in
   * `<skin dir>\Textures\` first, then in `<skin dir>\Tex_<skin title>\`.
   */
  std::vector<std::string> textures;
  std::vector<std::string> textures_raw;
  std::vector<MaterialInfo> materials;
  /** All meshes in file order: skin, rigid, morph skin, morph rigid, supple, muscle. */
  std::vector<SkinMesh> meshes;
};

/** Parse a `.ski` file. On failure returns false and fills `r_error` when not null. */
bool parse_skin(ByteSpan data, Skin &r_skin, std::string *r_error);

/** \} */

}  // namespace blender::io::angelica
