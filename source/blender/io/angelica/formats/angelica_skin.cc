/* SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup angelica
 *
 * `.ski` layout (grounded on `A3DSkin.cpp`, `A3DSkinMesh.cpp`, `A3DRigidMesh.cpp`,
 * `A3DMorph*.cpp`, `A3DSuppleMesh.cpp`, `A3DMuscleMesh.cpp` and `A3DMaterial.cpp` of Angelica 2.2
 * and the 1.5.5 client):
 *
 * - `A3DSKINFILEHEADER` (packed, 104 bytes): `uint32 'ASKI'`, `uint32 version`, `int` counts of
 *   skin / rigid / morph skin / morph rigid meshes, textures, materials, skin bones,
 *   `float fMinWeight`, `int iNumSkeBone`, `int` supple / muscle mesh counts, 52 reserved bytes.
 * - Version >= 9: skin bone names (`AFile` strings).
 * - Texture names (`AFile` strings), materials (`A3DMaterial::Load`).
 * - Meshes in the order skin, rigid, morph skin, morph rigid, supple, muscle. Every mesh starts
 *   with an `AFile` string name.
 *   - Skin: `{iTexture, iMaterial, iNumVert, iNumIdx}`, `A3DBVERTEX3[n]` (48 bytes: pos, 3
 *     weights, `uint32` bone index bytes, normal, uv), `uint16[iNumIdx]`, versions 100/101:
 *     `A3DVECTOR4[n]` tangents.
 *   - Rigid: `{iBoneIdx, iTexture, iMaterial, iNumVert, iNumIdx}`, `A3DVERTEX[n]` (32 bytes:
 *     pos, normal, uv), indices, versions 100/101 tangents.
 *   - Morph skin / rigid: the matching base mesh, then `MORPHDATA {iNumChannel, iAnimStart,
 *     iAnimEnd, iAnimFPS}`, per channel: name, `A3DMCVERTEX[n]` (position + normal deltas), a
 *     float weight track; then `uint8 sort[(end - start + 1) * channels]`.
 *   - Supple: `SUPPLEMESHDATA {iTexture, iMaterial, iNumSVert, iNumRVert, iNumIdx, iNumSpring,
 *     fGravityFactor, fAirResist, fVertWeight}`, `SVERTEX[s]` (32 bytes), `RVERTEX[r]`
 *     (24 bytes: `int iSVertex`, normal, uv), indices, `SPRING[springs]` (16 bytes).
 *   - Muscle: `{iTexture, iMaterial, iNumVert, iNumIdx}`, `A3DBVERTEX3[n]`, indices.
 */

#include "angelica_skin.hh"
#include "angelica_anim.hh"

#include <algorithm>
#include <cstdio>

namespace blender::io::angelica {

static bool skin_fail(std::string *r_error, const std::string &message)
{
  if (r_error) {
    *r_error = message;
  }
  return false;
}

/* -------------------------------------------------------------------- */
/** \name AFile Helpers
 * \{ */

AFileContent afile_open(ByteSpan data)
{
  AFileContent content;
  content.body = data;
  content.is_text = true;
  if (data.size() >= 4) {
    uint32_t tag;
    std::memcpy(&tag, data.data(), 4);
    if (tag == AFILE_TAG_BINARY) {
      content.body = data.subspan(4);
      content.is_text = false;
    }
    else if (tag == AFILE_TAG_TEXT) {
      content.body = data.subspan(4);
    }
  }
  return content;
}

std::string afile_read_cstring(BinaryReader &reader, const size_t max_len)
{
  const ByteSpan rest = reader.data().subspan(std::min(reader.tell(), reader.size()));
  const size_t limit = std::min(rest.size(), max_len);
  for (size_t i = 0; i < limit; i++) {
    if (rest[i] == 0) {
      std::string result(reinterpret_cast<const char *>(rest.data()), i);
      reader.skip(i + 1);
      return result;
    }
  }
  /* No terminator: consume everything so the caller sees a failure. */
  reader.skip(rest.size());
  reader.skip(1);
  return {};
}

bool afile_read_line(BinaryReader &reader, std::string &r_line)
{
  r_line.clear();
  if (reader.at_end()) {
    return false;
  }
  const ByteSpan rest = reader.data().subspan(reader.tell());
  size_t len = 0;
  while (len < rest.size() && rest[len] != '\n') {
    len++;
  }
  const size_t consumed = len < rest.size() ? len + 1 : len;
  r_line.assign(reinterpret_cast<const char *>(rest.data()), len);
  reader.skip(consumed);
  /* `AFileImage::ReadLine` chops at most two trailing line break characters. */
  for (int i = 0; i < 2; i++) {
    if (!r_line.empty() && (r_line.back() == '\r' || r_line.back() == '\n')) {
      r_line.pop_back();
    }
  }
  return true;
}

static std::string string_after(const std::string &s, const char *prefix)
{
  const size_t len = std::strlen(prefix);
  if (s.compare(0, len, prefix) == 0) {
    return s.substr(len);
  }
  return s;
}

static bool scan_color(const std::string &line, const char *format, Color4f &r_color)
{
  return std::sscanf(line.c_str(), format, &r_color.r, &r_color.g, &r_color.b, &r_color.a) == 4;
}

bool afile_read_material(BinaryReader &reader,
                         const bool is_text,
                         MaterialInfo &r_material,
                         std::string *r_error)
{
  r_material = MaterialInfo();
  if (!is_text) {
    const std::string name = afile_read_cstring(reader);
    r_material.name = gbk_to_utf8(string_after(name, "MATERIAL: "));
    for (Color4f *c :
         {&r_material.ambient, &r_material.diffuse, &r_material.emissive, &r_material.specular})
    {
      c->r = reader.f32();
      c->g = reader.f32();
      c->b = reader.f32();
      c->a = reader.f32();
    }
    r_material.power = reader.f32();
    r_material.two_sided = reader.u8() != 0;
    if (reader.failed()) {
      return skin_fail(r_error, "Truncated material");
    }
  }
  else {
    std::string line;
    if (!afile_read_line(reader, line)) {
      return skin_fail(r_error, "Truncated text material");
    }
    r_material.name = gbk_to_utf8(string_after(line, "MATERIAL: "));
    if (!afile_read_line(reader, line) || line != "{") {
      return skin_fail(r_error, "Invalid text material block");
    }
    const char *formats[4] = {"Ambient:(%f, %f, %f, %f)",
                              "Diffuse:(%f, %f, %f, %f)",
                              "Emissive:(%f, %f, %f, %f)",
                              "Specular:(%f, %f, %f, %f)"};
    Color4f *colors[4] = {
        &r_material.ambient, &r_material.diffuse, &r_material.emissive, &r_material.specular};
    for (int i = 0; i < 4; i++) {
      if (!afile_read_line(reader, line)) {
        return skin_fail(r_error, "Truncated text material");
      }
      scan_color(line, formats[i], *colors[i]);
    }
    if (!afile_read_line(reader, line)) {
      return skin_fail(r_error, "Truncated text material");
    }
    std::sscanf(line.c_str(), "Power:(%f)", &r_material.power);
    int two_sided = 0;
    if (!afile_read_line(reader, line)) {
      return skin_fail(r_error, "Truncated text material");
    }
    std::sscanf(line.c_str(), "2Sided:(%d)", &two_sided);
    r_material.two_sided = two_sided != 0;
    if (!afile_read_line(reader, line) || line != "}") {
      return skin_fail(r_error, "Invalid text material block end");
    }
  }
  r_material.power = std::clamp(r_material.power, -128.0f, 128.0f);
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Skin Meshes
 * \{ */

namespace {

struct SkinReadContext {
  BinaryReader &reader;
  uint32_t version;
  std::string *r_error;

  bool has_tangents() const
  {
    return version == SKIN_FILE_VERSION_TANGENT || version == SKIN_FILE_VERSION_TANGENT_OLD;
  }
};

}  // namespace

static bool check_count(SkinReadContext &ctx, int32_t count, size_t bytes_each, const char *what)
{
  if (count < 0 || size_t(count) * bytes_each > ctx.reader.remaining()) {
    return skin_fail(ctx.r_error, std::string("Invalid ") + what + " count");
  }
  return true;
}

/** `A3DBVERTEX3` (48 bytes) into the mesh arrays at vertex `i`. */
static void read_blend_vertex(BinaryReader &reader, SkinMesh &mesh, const size_t i)
{
  for (int k = 0; k < 3; k++) {
    mesh.positions[i * 3 + k] = reader.f32();
  }
  float total = 0.0f;
  for (int k = 0; k < 3; k++) {
    const float w = reader.f32();
    mesh.bone_weights[i * 4 + k] = w;
    total += w;
  }
  mesh.bone_weights[i * 4 + 3] = std::max(0.0f, 1.0f - total);
  for (int k = 0; k < 4; k++) {
    mesh.bone_indices[i * 4 + k] = reader.u8();
  }
  for (int k = 0; k < 3; k++) {
    mesh.normals[i * 3 + k] = reader.f32();
  }
  mesh.uvs[i * 2 + 0] = reader.f32();
  mesh.uvs[i * 2 + 1] = reader.f32();
}

static void resize_mesh(SkinMesh &mesh, size_t num_verts, bool weighted)
{
  mesh.positions.resize(num_verts * 3);
  mesh.normals.resize(num_verts * 3);
  mesh.uvs.resize(num_verts * 2);
  if (weighted) {
    mesh.bone_indices.resize(num_verts * 4);
    mesh.bone_weights.resize(num_verts * 4);
  }
}

static bool read_indices(SkinReadContext &ctx, SkinMesh &mesh, int32_t num_indices)
{
  if (!check_count(ctx, num_indices, 2, "index")) {
    return false;
  }
  mesh.indices.resize(size_t(num_indices));
  for (uint16_t &index : mesh.indices) {
    index = ctx.reader.u16();
  }
  return true;
}

static bool read_tangents(SkinReadContext &ctx, SkinMesh &mesh)
{
  if (!ctx.has_tangents()) {
    return true;
  }
  const size_t n = size_t(mesh.vertex_count());
  if (n * 16 > ctx.reader.remaining()) {
    return skin_fail(ctx.r_error, "Truncated tangent data in mesh \"" + mesh.name + "\"");
  }
  mesh.tangents.resize(n * 4);
  for (float &v : mesh.tangents) {
    v = ctx.reader.f32();
  }
  return true;
}

static bool read_skin_mesh(SkinReadContext &ctx, SkinMesh &mesh, bool with_tangents)
{
  BinaryReader &r = ctx.reader;
  mesh.name = gbk_to_utf8(r.string_i32());
  mesh.texture_index = r.i32();
  mesh.material_index = r.i32();
  const int32_t num_verts = r.i32();
  const int32_t num_indices = r.i32();
  if (r.failed()) {
    return skin_fail(ctx.r_error, "Truncated skin mesh header");
  }
  if (!check_count(ctx, num_verts, 48, "skin mesh vertex")) {
    return false;
  }
  resize_mesh(mesh, size_t(num_verts), true);
  for (size_t i = 0; i < size_t(num_verts); i++) {
    read_blend_vertex(r, mesh, i);
  }
  if (!read_indices(ctx, mesh, num_indices)) {
    return false;
  }
  if (with_tangents && !read_tangents(ctx, mesh)) {
    return false;
  }
  if (r.failed()) {
    return skin_fail(ctx.r_error, "Truncated skin mesh \"" + mesh.name + "\"");
  }
  return true;
}

static bool read_rigid_mesh(SkinReadContext &ctx, SkinMesh &mesh)
{
  BinaryReader &r = ctx.reader;
  mesh.name = gbk_to_utf8(r.string_i32());
  mesh.bone_index = r.i32();
  mesh.texture_index = r.i32();
  mesh.material_index = r.i32();
  const int32_t num_verts = r.i32();
  const int32_t num_indices = r.i32();
  if (r.failed()) {
    return skin_fail(ctx.r_error, "Truncated rigid mesh header");
  }
  if (!check_count(ctx, num_verts, 32, "rigid mesh vertex")) {
    return false;
  }
  resize_mesh(mesh, size_t(num_verts), false);
  for (size_t i = 0; i < size_t(num_verts); i++) {
    for (int k = 0; k < 3; k++) {
      mesh.positions[i * 3 + k] = r.f32();
    }
    for (int k = 0; k < 3; k++) {
      mesh.normals[i * 3 + k] = r.f32();
    }
    mesh.uvs[i * 2 + 0] = r.f32();
    mesh.uvs[i * 2 + 1] = r.f32();
  }
  if (!read_indices(ctx, mesh, num_indices) || !read_tangents(ctx, mesh)) {
    return false;
  }
  if (r.failed()) {
    return skin_fail(ctx.r_error, "Truncated rigid mesh \"" + mesh.name + "\"");
  }
  return true;
}

/**
 * Morph channel weight tracks are written by `A3DFloatTrack::Save`. The 1.5.5 engine reads them
 * as version 1 tracks (keys + segments), Angelica 2.2 always as version 2 tracks. Pick the layout
 * whose data is consistent.
 */
static bool read_morph_weight_track(SkinReadContext &ctx, KeyTrack &r_track)
{
  BinaryReader probe = ctx.reader;
  probe.skip(4);
  const int32_t num_segments = probe.i32();
  if (!probe.failed() && num_segments > 0) {
    BinaryReader v1 = ctx.reader;
    KeyTrack track;
    if (read_key_track(v1, 1, TrackValueType::Float, track, nullptr)) {
      bool plausible = true;
      for (const TrackSegment &seg : track.segments) {
        if (seg.start_time_ms < 0 || seg.end_time_ms < seg.start_time_ms || seg.start_key < 0 ||
            seg.end_key < seg.start_key || seg.end_key >= track.stored_key_count)
        {
          plausible = false;
          break;
        }
      }
      if (plausible) {
        ctx.reader = v1;
        r_track = std::move(track);
        return true;
      }
    }
  }
  std::string error;
  if (!read_key_track(ctx.reader, 2, TrackValueType::Float, r_track, &error)) {
    return skin_fail(ctx.r_error, "Morph weight track: " + error);
  }
  return true;
}

static bool read_morph_data(SkinReadContext &ctx, SkinMesh &mesh)
{
  BinaryReader &r = ctx.reader;
  const int32_t num_channels = r.i32();
  mesh.morph_anim_start = r.i32();
  mesh.morph_anim_end = r.i32();
  mesh.morph_anim_fps = r.i32();
  if (r.failed()) {
    return skin_fail(ctx.r_error, "Truncated morph data");
  }
  const size_t num_verts = size_t(mesh.vertex_count());
  /* A channel is at least a name length, the deltas and a track header. */
  if (!check_count(ctx, num_channels, 4 + num_verts * 24 + 12, "morph channel")) {
    return false;
  }
  mesh.morph_channels.resize(size_t(num_channels));
  for (MorphChannel &channel : mesh.morph_channels) {
    channel.name = gbk_to_utf8(r.string_i32());
    if (num_verts * 24 > r.remaining()) {
      return skin_fail(ctx.r_error, "Truncated morph channel \"" + channel.name + "\"");
    }
    channel.position_deltas.resize(num_verts * 3);
    channel.normal_deltas.resize(num_verts * 3);
    for (size_t i = 0; i < num_verts; i++) {
      for (int k = 0; k < 3; k++) {
        channel.position_deltas[i * 3 + k] = r.f32();
      }
      for (int k = 0; k < 3; k++) {
        channel.normal_deltas[i * 3 + k] = r.f32();
      }
    }
    KeyTrack track;
    if (!read_morph_weight_track(ctx, track)) {
      return false;
    }
    channel.weight_keys = std::move(track.keys);
    channel.weight_key_frames = std::move(track.key_frames);
    channel.frame_rate = track.frame_rate;
  }
  /* Per frame channel sort table, not needed for import. */
  const int64_t num_frames = int64_t(mesh.morph_anim_end) - int64_t(mesh.morph_anim_start) + 1;
  const int64_t sort_size = num_frames * int64_t(num_channels);
  if (num_frames < 0 || sort_size < 0 || uint64_t(sort_size) > r.remaining()) {
    return skin_fail(ctx.r_error, "Invalid morph frame range");
  }
  r.skip(size_t(sort_size));
  if (r.failed()) {
    return skin_fail(ctx.r_error, "Truncated morph data");
  }
  return true;
}

static bool read_supple_mesh(SkinReadContext &ctx, SkinMesh &mesh)
{
  BinaryReader &r = ctx.reader;
  mesh.name = gbk_to_utf8(r.string_i32());
  mesh.texture_index = r.i32();
  mesh.material_index = r.i32();
  const int32_t num_spring_verts = r.i32();
  const int32_t num_render_verts = r.i32();
  const int32_t num_indices = r.i32();
  const int32_t num_springs = r.i32();
  mesh.supple_gravity_factor = r.f32();
  mesh.supple_air_resist = r.f32();
  mesh.supple_vertex_weight = r.f32();
  if (r.failed()) {
    return skin_fail(ctx.r_error, "Truncated supple mesh header");
  }
  if (!check_count(ctx, num_spring_verts, 32, "supple spring vertex")) {
    return false;
  }
  /* Spring vertices: position, 3 weights, bone index bytes, bone factor. */
  struct SpringVertex {
    float pos[3];
    float weights[4];
    uint8_t bones[4];
    float bone_factor;
  };
  std::vector<SpringVertex> svert(static_cast<size_t>(num_spring_verts));
  for (SpringVertex &v : svert) {
    float total = 0.0f;
    for (int k = 0; k < 3; k++) {
      v.pos[k] = r.f32();
    }
    for (int k = 0; k < 3; k++) {
      v.weights[k] = r.f32();
      total += v.weights[k];
    }
    v.weights[3] = std::max(0.0f, 1.0f - total);
    for (int k = 0; k < 4; k++) {
      v.bones[k] = r.u8();
    }
    v.bone_factor = r.f32();
  }
  if (!check_count(ctx, num_render_verts, 24, "supple render vertex")) {
    return false;
  }
  resize_mesh(mesh, size_t(num_render_verts), true);
  mesh.supple_bone_factors.resize(size_t(num_render_verts));
  for (size_t i = 0; i < size_t(num_render_verts); i++) {
    const int32_t sv = r.i32();
    if (sv < 0 || sv >= num_spring_verts) {
      return skin_fail(ctx.r_error, "Supple mesh vertex references an invalid spring vertex");
    }
    const SpringVertex &s = svert[size_t(sv)];
    for (int k = 0; k < 3; k++) {
      mesh.positions[i * 3 + k] = s.pos[k];
      mesh.normals[i * 3 + k] = r.f32();
    }
    mesh.uvs[i * 2 + 0] = r.f32();
    mesh.uvs[i * 2 + 1] = r.f32();
    for (int k = 0; k < 4; k++) {
      mesh.bone_weights[i * 4 + k] = s.weights[k];
      mesh.bone_indices[i * 4 + k] = s.bones[k];
    }
    mesh.supple_bone_factors[i] = s.bone_factor;
  }
  if (!read_indices(ctx, mesh, num_indices)) {
    return false;
  }
  if (!check_count(ctx, num_springs, 16, "supple spring")) {
    return false;
  }
  r.skip(size_t(num_springs) * 16);
  if (r.failed()) {
    return skin_fail(ctx.r_error, "Truncated supple mesh \"" + mesh.name + "\"");
  }
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Skin File
 * \{ */

bool parse_skin(ByteSpan data, Skin &r_skin, std::string *r_error)
{
  r_skin = Skin();
  const AFileContent content = afile_open(data);
  r_skin.is_text = content.is_text;
  BinaryReader reader(content.body);

  if (reader.remaining() < 104) {
    return skin_fail(r_error, "File too small for a skin header");
  }
  const uint32_t flags = reader.u32();
  const uint32_t version = reader.u32();
  if (flags != SKIN_FILE_IDENTIFY) {
    return skin_fail(r_error, "Not a skin (.ski) file");
  }
  if (version == 0 || (version > SKIN_FILE_VERSION && version != SKIN_FILE_VERSION_TANGENT &&
                       version != SKIN_FILE_VERSION_TANGENT_OLD))
  {
    return skin_fail(r_error, "Unsupported skin version " + std::to_string(version));
  }
  r_skin.version = version;

  const int32_t num_skin_meshes = reader.i32();
  const int32_t num_rigid_meshes = reader.i32();
  const int32_t num_morph_skin_meshes = reader.i32();
  const int32_t num_morph_rigid_meshes = reader.i32();
  const int32_t num_textures = reader.i32();
  const int32_t num_materials = reader.i32();
  const int32_t num_skin_bones = reader.i32();
  r_skin.min_weight = reader.f32();
  r_skin.skeleton_bone_count = reader.i32();
  const int32_t num_supple_meshes = reader.i32();
  const int32_t num_muscle_meshes = reader.i32();
  reader.skip(52);

  SkinReadContext ctx{reader, version, r_error};
  /* Smallest mesh record: name length + 16 byte header. */
  if (!check_count(ctx, num_skin_meshes, 20, "skin mesh") ||
      !check_count(ctx, num_rigid_meshes, 24, "rigid mesh") ||
      !check_count(ctx, num_morph_skin_meshes, 36, "morph skin mesh") ||
      !check_count(ctx, num_morph_rigid_meshes, 40, "morph rigid mesh") ||
      !check_count(ctx, num_supple_meshes, 40, "supple mesh") ||
      !check_count(ctx, num_muscle_meshes, 20, "muscle mesh") ||
      !check_count(ctx, num_textures, 4, "texture") ||
      !check_count(ctx, num_materials, 70, "material") ||
      !check_count(ctx, std::max(num_skin_bones, 0), 4, "skin bone") || num_skin_bones < 0)
  {
    return false;
  }

  if (version >= 9) {
    r_skin.bone_names.reserve(size_t(num_skin_bones));
    for (int i = 0; i < num_skin_bones; i++) {
      r_skin.bone_names.push_back(gbk_to_utf8(reader.string_i32()));
    }
  }
  for (int i = 0; i < num_textures; i++) {
    std::string raw = reader.string_i32();
    r_skin.textures.push_back(gbk_to_utf8(raw));
    r_skin.textures_raw.push_back(std::move(raw));
  }
  if (reader.failed()) {
    return skin_fail(r_error, "Truncated skin bone or texture names");
  }
  for (int i = 0; i < num_materials; i++) {
    MaterialInfo material;
    if (!afile_read_material(reader, content.is_text, material, r_error)) {
      return false;
    }
    /* The engine zeroes the ambient and emissive alpha so diffuse alpha drives transparency. */
    material.ambient.a = 0.0f;
    material.emissive.a = 0.0f;
    r_skin.materials.push_back(std::move(material));
  }

  auto read_meshes = [&](int32_t count, SkinMeshType type) -> bool {
    for (int i = 0; i < count; i++) {
      SkinMesh mesh;
      mesh.type = type;
      bool ok = false;
      switch (type) {
        case SkinMeshType::Skin:
          ok = read_skin_mesh(ctx, mesh, true);
          break;
        case SkinMeshType::Rigid:
          ok = read_rigid_mesh(ctx, mesh);
          break;
        case SkinMeshType::MorphSkin:
          ok = read_skin_mesh(ctx, mesh, true) && read_morph_data(ctx, mesh);
          break;
        case SkinMeshType::MorphRigid:
          ok = read_rigid_mesh(ctx, mesh) && read_morph_data(ctx, mesh);
          break;
        case SkinMeshType::Supple:
          ok = read_supple_mesh(ctx, mesh);
          break;
        case SkinMeshType::Muscle:
          ok = read_skin_mesh(ctx, mesh, false);
          break;
      }
      if (!ok) {
        return false;
      }
      r_skin.meshes.push_back(std::move(mesh));
    }
    return true;
  };

  if (!read_meshes(num_skin_meshes, SkinMeshType::Skin) ||
      !read_meshes(num_rigid_meshes, SkinMeshType::Rigid) ||
      !read_meshes(num_morph_skin_meshes, SkinMeshType::MorphSkin) ||
      !read_meshes(num_morph_rigid_meshes, SkinMeshType::MorphRigid) ||
      !read_meshes(num_supple_meshes, SkinMeshType::Supple) ||
      !read_meshes(num_muscle_meshes, SkinMeshType::Muscle))
  {
    return false;
  }
  if (reader.failed()) {
    return skin_fail(r_error, "Truncated skin file");
  }
  return true;
}

/** \} */

}  // namespace blender::io::angelica
