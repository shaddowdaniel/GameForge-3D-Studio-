/* SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup angelica
 *
 * `.bmd` layout (grounded on `CELBrushBuilding::Load` of the element client and
 * `A3DLitModel::Load`, `A3DLitMesh::Load`, `A3DLitModelSharedData::LoadFromFile` of Angelica 2.2
 * and the 1.5.5 client):
 *
 * - Optional `uint32 0x80000001` + `bool bCollideOnly` building header.
 * - `uint32 version`, `A3DVECTOR3 scale, dir, up, pos`.
 * - 0x10000001 / 0x10000002 / 0x10000100 / 0x10000101: `int nNumMeshes` + `A3DLitMesh` records,
 *   then light map names (`char[256]` for 0x10000100, `char[260]` day + night for 0x10000101).
 *   `A3DLitMesh`: `uint32 version`, `char name[64]`, `char texture[256]`, `int verts, faces`,
 *   [0x10000006: `bool extra colors`], vertices (`A3DLVERTEX` 28 bytes for 0x10000002/3,
 *   `A3DLMVERTEX_WITHOUTNORMAL` 24 bytes otherwise), `uint16 indices[faces * 3]`,
 *   `A3DVECTOR3 normals[verts]`, [>= 0x10000003: day + night `A3DCOLOR[verts]`],
 *   [extra colors: day + night extra], `A3DAABB` (48 bytes), [>= 0x10000005: material],
 *   [0x100001xx: `A3DLIGHTMAPCOORD[verts]`], [0x10000101/2: `bool use light map`].
 * - 0x10000200 (shared): `bool light map`, `AFile` string shared file, `int nNumMeshes`, per mesh
 *   `bool support LM, use LM`, [day + night colors unless LM is used], `A3DAABB`; light map
 *   strings.
 * - 0x10000300 (extended): `int nNumMeshes`, `bool light map, shared`, [shared file], per mesh
 *   `bool support LM, use LM`, [inline: name, texture strings, `int verts, faces`, vertices,
 *   indices, normals, material, [LM coords]], [colors], `A3DAABB`, a sized extra block (detail
 *   tile, hardness, detail texture), light map strings, a sized model extra block.
 * - Building collision data follows (`int nNumHull`, ...), not decoded.
 *
 * `.mox` layout (`A3DFrame::Load`, `A3DMesh::Load`, binary mode): see `parse_frame` and
 * `parse_mox_mesh` below.
 */

#include "angelica_static.hh"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace blender::io::angelica {

static bool static_fail(std::string *r_error, const std::string &message)
{
  if (r_error) {
    *r_error = message;
  }
  return false;
}

static void read_vec3(BinaryReader &reader, float r_v[3])
{
  r_v[0] = reader.f32();
  r_v[1] = reader.f32();
  r_v[2] = reader.f32();
}

/* -------------------------------------------------------------------- */
/** \name Lit Model
 * \{ */

static void normalize3(float v[3])
{
  const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  if (len > 1e-12f) {
    v[0] /= len;
    v[1] /= len;
    v[2] /= len;
  }
}

static void compute_world_matrix(LitModel &model)
{
  float z[3] = {model.dir[0], model.dir[1], model.dir[2]};
  float y[3] = {model.up[0], model.up[1], model.up[2]};
  normalize3(z);
  normalize3(y);
  float x[3] = {y[1] * z[2] - y[2] * z[1], y[2] * z[0] - y[0] * z[2], y[0] * z[1] - y[1] * z[0]};
  normalize3(x);
  float *m = model.world_matrix;
  for (int i = 0; i < 3; i++) {
    m[0 + i] = x[i] * model.scale[0];
    m[4 + i] = y[i] * model.scale[1];
    m[8 + i] = z[i] * model.scale[2];
    m[12 + i] = model.pos[i];
  }
  m[3] = m[7] = m[11] = 0.0f;
  m[15] = 1.0f;
}

namespace {
struct LitReadContext {
  BinaryReader &reader;
  bool is_text;
  std::string *r_error;
};
}  // namespace

static bool check_lit_counts(LitReadContext &ctx, int32_t verts, int32_t faces, size_t vert_size)
{
  if (verts < 0 || faces < 0) {
    return static_fail(ctx.r_error, "Invalid lit mesh vertex or face count");
  }
  if (size_t(verts) * vert_size + size_t(faces) * 6 > ctx.reader.remaining()) {
    return static_fail(ctx.r_error, "Lit mesh vertex or face count exceeds the file size");
  }
  return true;
}

static void resize_lit_mesh(LitMesh &mesh, size_t verts, size_t faces)
{
  mesh.positions.resize(verts * 3);
  mesh.normals.resize(verts * 3);
  mesh.uvs.resize(verts * 2);
  mesh.indices.resize(faces * 3);
}

/** `A3DLMVERTEX_WITHOUTNORMAL` (24 bytes) or `A3DLVERTEX` (28 bytes). Returns vertex diffuse. */
static void read_lit_vertices(BinaryReader &r,
                              LitMesh &mesh,
                              bool legacy,
                              std::vector<uint32_t> &r_diffuse)
{
  const size_t n = size_t(mesh.vertex_count());
  r_diffuse.resize(n);
  for (size_t i = 0; i < n; i++) {
    read_vec3(r, &mesh.positions[i * 3]);
    r_diffuse[i] = r.u32();
    if (legacy) {
      r.skip(4); /* Specular. */
    }
    mesh.uvs[i * 2 + 0] = r.f32();
    mesh.uvs[i * 2 + 1] = r.f32();
  }
}

static void read_u16_array(BinaryReader &r, std::vector<uint16_t> &arr)
{
  for (uint16_t &v : arr) {
    v = r.u16();
  }
}

static void read_vec3_array(BinaryReader &r, std::vector<float> &arr)
{
  for (float &v : arr) {
    v = r.f32();
  }
}

static bool read_colors(LitReadContext &ctx, std::vector<uint32_t> &arr, size_t n)
{
  if (n * 4 > ctx.reader.remaining()) {
    return static_fail(ctx.r_error, "Truncated lit mesh colors");
  }
  arr.resize(n);
  for (uint32_t &c : arr) {
    c = ctx.reader.u32();
  }
  return true;
}

static bool read_lightmap_uvs(LitReadContext &ctx, LitMesh &mesh)
{
  const size_t n = size_t(mesh.vertex_count());
  if (n * 8 > ctx.reader.remaining()) {
    return static_fail(ctx.r_error, "Truncated light map coordinates");
  }
  mesh.lightmap_uvs.resize(n * 2);
  for (float &v : mesh.lightmap_uvs) {
    v = ctx.reader.f32();
  }
  return true;
}

static void read_aabb(BinaryReader &r, LitMesh &mesh)
{
  float center[3], extents[3];
  read_vec3(r, center);
  read_vec3(r, extents);
  read_vec3(r, mesh.aabb_min);
  read_vec3(r, mesh.aabb_max);
}

static bool read_lit_mesh(LitReadContext &ctx, LitMesh &mesh)
{
  BinaryReader &r = ctx.reader;
  mesh.version = r.u32();
  if (r.failed()) {
    return static_fail(ctx.r_error, "Truncated lit mesh");
  }
  const uint32_t v = mesh.version;
  if (!(v >= 0x10000002 && v <= 0x10000006) && !(v >= 0x10000100 && v <= 0x10000102)) {
    return static_fail(ctx.r_error, "Unsupported lit mesh version " + std::to_string(v));
  }
  mesh.name = gbk_to_utf8(r.string_fixed(64));
  mesh.texture_raw = r.string_fixed(256);
  mesh.texture = gbk_to_utf8(mesh.texture_raw);
  const int32_t verts = r.i32();
  const int32_t faces = r.i32();
  bool extra_colors = false;
  if (v == 0x10000006) {
    extra_colors = r.u8() != 0;
  }
  if (r.failed()) {
    return static_fail(ctx.r_error, "Truncated lit mesh header");
  }
  const bool legacy = v <= 0x10000003;
  if (!check_lit_counts(ctx, verts, faces, (legacy ? 28 : 24) + 12)) {
    return false;
  }
  resize_lit_mesh(mesh, size_t(verts), size_t(faces));
  std::vector<uint32_t> diffuse;
  read_lit_vertices(r, mesh, legacy, diffuse);
  read_u16_array(r, mesh.indices);
  read_vec3_array(r, mesh.normals);
  if (v == 0x10000002) {
    mesh.day_colors = diffuse;
    mesh.night_colors = std::move(diffuse);
  }
  else {
    if (!read_colors(ctx, mesh.day_colors, size_t(verts)) ||
        !read_colors(ctx, mesh.night_colors, size_t(verts)))
    {
      return false;
    }
    if (extra_colors && (!read_colors(ctx, mesh.day_colors_extra, size_t(verts)) ||
                         !read_colors(ctx, mesh.night_colors_extra, size_t(verts))))
    {
      return false;
    }
  }
  read_aabb(r, mesh);
  if (v >= 0x10000005) {
    mesh.has_material = true;
    if (!afile_read_material(r, ctx.is_text, mesh.material, ctx.r_error)) {
      return false;
    }
  }
  if (v >= 0x10000100) {
    if (!read_lightmap_uvs(ctx, mesh)) {
      return false;
    }
    mesh.use_lightmap = true;
    if (v >= 0x10000101) {
      mesh.use_lightmap = r.u8() != 0;
    }
  }
  if (r.failed()) {
    return static_fail(ctx.r_error, "Truncated lit mesh \"" + mesh.name + "\"");
  }
  return true;
}

/** `A3DLitModelSharedData::LoadFromFile`. */
static bool parse_lit_shared_data(ByteSpan data,
                                  std::vector<LitMesh> &r_meshes,
                                  std::string *r_error)
{
  const AFileContent content = afile_open(data);
  BinaryReader r(content.body);
  LitReadContext ctx{r, content.is_text, r_error};
  if (r.u32() != LIT_SHARED_DATA_MAGIC) {
    return static_fail(r_error, "Not a lit model shared data (LMSD) file");
  }
  const uint32_t version = r.u32();
  if (version > 1) {
    return static_fail(r_error, "Unsupported lit model shared data version");
  }
  const uint32_t num_meshes = r.u32();
  if (r.failed() || size_t(num_meshes) * 9 > r.remaining()) {
    return static_fail(r_error, "Invalid lit model shared data mesh count");
  }
  r_meshes.resize(num_meshes);
  for (LitMesh &mesh : r_meshes) {
    const int32_t verts = r.i32();
    const int32_t num_indices = r.i32();
    const bool lightmap = r.u8() != 0;
    if (r.failed() || verts < 0 || num_indices < 0 ||
        size_t(verts) * (lightmap ? 40 : 32) + size_t(num_indices) * 2 > r.remaining())
    {
      return static_fail(r_error, "Invalid lit model shared data mesh");
    }
    mesh.positions.resize(size_t(verts) * 3);
    mesh.normals.resize(size_t(verts) * 3);
    mesh.uvs.resize(size_t(verts) * 2);
    mesh.indices.resize(size_t(num_indices));
    read_vec3_array(r, mesh.positions);
    read_vec3_array(r, mesh.normals);
    read_vec3_array(r, mesh.uvs);
    if (lightmap) {
      mesh.lightmap_uvs.resize(size_t(verts) * 2);
      read_vec3_array(r, mesh.lightmap_uvs);
    }
    read_u16_array(r, mesh.indices);
    mesh.texture_raw = r.string_i32();
    mesh.texture = gbk_to_utf8(mesh.texture_raw);
    mesh.has_material = true;
    if (!afile_read_material(r, ctx.is_text, mesh.material, r_error)) {
      return false;
    }
  }
  if (r.failed()) {
    return static_fail(r_error, "Truncated lit model shared data");
  }
  return true;
}

static bool load_shared_meshes(LitModel &model,
                               const LitSharedDataLoader &loader,
                               int32_t num_meshes,
                               std::vector<LitMesh> &r_meshes,
                               std::string *r_error)
{
  if (!loader) {
    return static_fail(r_error,
                       "Lit model needs shared data file \"" + model.shared_data_file + "\"");
  }
  Bytes shared;
  if (!loader(model.shared_data_file_raw, shared)) {
    return static_fail(r_error,
                       "Missing lit model shared data \"" + model.shared_data_file + "\"");
  }
  std::string error;
  if (!parse_lit_shared_data(shared, r_meshes, &error)) {
    return static_fail(r_error, model.shared_data_file + ": " + error);
  }
  if (int64_t(r_meshes.size()) != int64_t(num_meshes)) {
    return static_fail(r_error, "Lit model shared data does not match the model");
  }
  return true;
}

static void read_lightmap_strings(BinaryReader &r, LitModel &model)
{
  model.lightmap_raw = r.string_i32();
  model.night_lightmap_raw = r.string_i32();
  model.lightmap = gbk_to_utf8(model.lightmap_raw);
  model.night_lightmap = gbk_to_utf8(model.night_lightmap_raw);
}

/** Sized block: `uint32 size` counted from its own start. */
static bool skip_to_block_end(LitReadContext &ctx, size_t block_start, uint32_t size)
{
  if (size < 4 || size_t(size) > ctx.reader.size() - block_start ||
      ctx.reader.tell() > block_start + size)
  {
    return static_fail(ctx.r_error, "Invalid extra data block size");
  }
  ctx.reader.seek(block_start + size);
  return true;
}

static bool read_lit_model_shared_or_ex(LitReadContext &ctx,
                                        LitModel &model,
                                        const LitSharedDataLoader &loader)
{
  BinaryReader &r = ctx.reader;
  const bool ex = model.version == LIT_MODEL_VERSION_EX;
  bool lightmap = false;
  bool shared = true;
  int32_t num_meshes = 0;
  if (ex) {
    num_meshes = r.i32();
    lightmap = r.u8() != 0;
    shared = r.u8() != 0;
  }
  else {
    lightmap = r.u8() != 0;
  }
  if (shared) {
    model.shared_data_file_raw = r.string_i32();
    model.shared_data_file = gbk_to_utf8(model.shared_data_file_raw);
  }
  if (!ex) {
    num_meshes = r.i32();
  }
  if (r.failed() || num_meshes < 0 || size_t(num_meshes) * 50 > r.remaining()) {
    return static_fail(ctx.r_error, "Invalid lit model mesh count");
  }

  std::vector<LitMesh> shared_meshes;
  if (shared && !load_shared_meshes(model, loader, num_meshes, shared_meshes, ctx.r_error)) {
    return false;
  }

  for (int i = 0; i < num_meshes; i++) {
    LitMesh mesh;
    mesh.version = model.version;
    const bool support_lightmap = r.u8() != 0;
    const bool use_lightmap = r.u8() != 0;
    if (shared) {
      LitMesh &src = shared_meshes[size_t(i)];
      mesh.texture = std::move(src.texture);
      mesh.texture_raw = std::move(src.texture_raw);
      mesh.positions = std::move(src.positions);
      mesh.normals = std::move(src.normals);
      mesh.uvs = std::move(src.uvs);
      if (support_lightmap) {
        mesh.lightmap_uvs = std::move(src.lightmap_uvs);
      }
      mesh.indices = std::move(src.indices);
      mesh.material = std::move(src.material);
      mesh.has_material = true;
    }
    else {
      mesh.name = gbk_to_utf8(r.string_i32());
      mesh.texture_raw = r.string_i32();
      mesh.texture = gbk_to_utf8(mesh.texture_raw);
      const int32_t verts = r.i32();
      const int32_t faces = r.i32();
      if (r.failed() || !check_lit_counts(ctx, verts, faces, 36)) {
        return r.failed() ? static_fail(ctx.r_error, "Truncated lit mesh") : false;
      }
      resize_lit_mesh(mesh, size_t(verts), size_t(faces));
      std::vector<uint32_t> diffuse;
      read_lit_vertices(r, mesh, false, diffuse);
      read_u16_array(r, mesh.indices);
      read_vec3_array(r, mesh.normals);
      mesh.has_material = true;
      if (!afile_read_material(r, ctx.is_text, mesh.material, ctx.r_error)) {
        return false;
      }
      if (support_lightmap && !read_lightmap_uvs(ctx, mesh)) {
        return false;
      }
    }
    mesh.use_lightmap = use_lightmap;
    const size_t n = size_t(mesh.vertex_count());
    if (!support_lightmap || !use_lightmap) {
      if (!read_colors(ctx, mesh.day_colors, n) || !read_colors(ctx, mesh.night_colors, n)) {
        return false;
      }
    }
    read_aabb(r, mesh);
    if (ex) {
      const size_t block_start = r.tell();
      const uint32_t size = r.u32();
      mesh.detail_tile = r.f32();
      mesh.detail_hardness = r.f32();
      mesh.detail_texture_raw = r.string_i32();
      mesh.detail_texture = gbk_to_utf8(mesh.detail_texture_raw);
      if (r.failed() || !skip_to_block_end(ctx, block_start, size)) {
        return static_fail(ctx.r_error, "Invalid lit mesh extra data");
      }
    }
    if (r.failed()) {
      return static_fail(ctx.r_error, "Truncated lit mesh " + std::to_string(i));
    }
    model.meshes.push_back(std::move(mesh));
  }

  if (lightmap) {
    read_lightmap_strings(r, model);
  }
  if (ex) {
    const size_t block_start = r.tell();
    const uint32_t size = r.u32();
    if (r.failed() || !skip_to_block_end(ctx, block_start, size)) {
      return static_fail(ctx.r_error, "Invalid lit model extra data");
    }
  }
  if (r.failed()) {
    return static_fail(ctx.r_error, "Truncated lit model");
  }
  return true;
}

bool parse_bmd(ByteSpan data,
               LitModel &r_model,
               std::string *r_error,
               const LitSharedDataLoader &shared_data_loader)
{
  r_model = LitModel();
  const AFileContent content = afile_open(data);
  BinaryReader r(content.body);
  LitReadContext ctx{r, content.is_text, r_error};

  uint32_t version = r.u32();
  if (version == BRUSH_BUILDING_VERSION) {
    r_model.building_version = version;
    r_model.collide_only = r.u8() != 0;
    version = r.u32();
  }
  if (r.failed()) {
    return static_fail(r_error, "File too small for a lit model");
  }
  switch (version) {
    case LIT_MODEL_VERSION_1:
    case LIT_MODEL_VERSION_2:
    case LIT_MODEL_VERSION_LIGHTMAP_OLD:
    case LIT_MODEL_VERSION_LIGHTMAP:
    case LIT_MODEL_VERSION_SHARED:
    case LIT_MODEL_VERSION_EX:
      break;
    default:
      return static_fail(r_error, "Not a lit model (.bmd) file or unsupported version");
  }
  r_model.version = version;
  read_vec3(r, r_model.scale);
  read_vec3(r, r_model.dir);
  read_vec3(r, r_model.up);
  read_vec3(r, r_model.pos);
  if (r.failed()) {
    return static_fail(r_error, "Truncated lit model header");
  }
  compute_world_matrix(r_model);

  if (version == LIT_MODEL_VERSION_SHARED || version == LIT_MODEL_VERSION_EX) {
    if (!read_lit_model_shared_or_ex(ctx, r_model, shared_data_loader)) {
      return false;
    }
  }
  else {
    const int32_t num_meshes = r.i32();
    /* Smallest lit mesh: version, name, texture, counts and the AABB. */
    if (r.failed() || num_meshes < 0 || size_t(num_meshes) * 380 > r.remaining()) {
      return static_fail(r_error, "Invalid lit model mesh count");
    }
    r_model.meshes.resize(size_t(num_meshes));
    for (LitMesh &mesh : r_model.meshes) {
      if (!read_lit_mesh(ctx, mesh)) {
        return false;
      }
    }
    if (version == LIT_MODEL_VERSION_LIGHTMAP_OLD) {
      r_model.lightmap_raw = r.string_fixed(256);
      r_model.lightmap = gbk_to_utf8(r_model.lightmap_raw);
    }
    else if (version == LIT_MODEL_VERSION_LIGHTMAP) {
      r_model.lightmap_raw = r.string_fixed(260);
      r_model.night_lightmap_raw = r.string_fixed(260);
      r_model.lightmap = gbk_to_utf8(r_model.lightmap_raw);
      r_model.night_lightmap = gbk_to_utf8(r_model.night_lightmap_raw);
    }
    if (r.failed()) {
      return static_fail(r_error, "Truncated lit model light map names");
    }
  }
  r_model.has_collision_data = r.remaining() >= 4;
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Frame Model (.mox)
 * \{ */

constexpr int MOX_MAX_DEPTH = 64;

/** `"VERSION: n"` (optional) then `"<prefix>name"`, both NUL terminated strings. */
static bool read_versioned_name(BinaryReader &r,
                                const char *prefix,
                                int &r_version,
                                std::string &r_name_raw)
{
  r_version = 1;
  std::string line = afile_read_cstring(r);
  if (line.compare(0, 9, "VERSION: ") == 0) {
    r_version = std::atoi(line.c_str() + 9);
    line = afile_read_cstring(r);
  }
  const size_t len = std::strlen(prefix);
  r_name_raw = line.compare(0, len, prefix) == 0 ? line.substr(len) : std::string();
  return !r.failed();
}

static bool parse_mox_mesh(BinaryReader &r, MoxMesh &mesh, std::string *r_error)
{
  std::string name_raw;
  if (!read_versioned_name(r, "MESH: ", mesh.version, name_raw)) {
    return static_fail(r_error, "Truncated mesh name");
  }
  mesh.name = gbk_to_utf8(name_raw);
  const int32_t verts = r.i32();
  const int32_t num_indices = r.i32();
  const int32_t frames = r.i32();
  const bool has_lod = r.u8() != 0;
  mesh.wire = r.u8() != 0;
  if (r.failed()) {
    return static_fail(r_error, "Truncated mesh header");
  }
  if (verts < 0 || num_indices < 0 || frames < 0 ||
      size_t(verts) * size_t(frames) * 32 + size_t(num_indices) * 2 > r.remaining())
  {
    return static_fail(r_error, "Invalid counts in mesh \"" + mesh.name + "\"");
  }
  mesh.frame_count = frames;

  /* User properties. */
  if (mesh.version == 1) {
    /* `A3DMESH_PROP`: 4 bools, int material type, bool on ground, bool PVS, padding. */
    const int32_t size = r.i32();
    if (size < 0 || size_t(size) > r.remaining()) {
      return static_fail(r_error, "Invalid mesh property size");
    }
    const ByteSpan prop = r.read_span(size_t(size));
    auto flag = [&](size_t offset) { return offset < prop.size() && prop[offset] != 0; };
    if (flag(0)) {
      mesh.props |= 0x1;
    }
    if (flag(1)) {
      mesh.props |= 0x2;
    }
    if (flag(2)) {
      mesh.props |= 0x4;
    }
    if (flag(3)) {
      mesh.props |= 0x8;
    }
    if (prop.size() >= 8) {
      std::memcpy(&mesh.material_type, prop.data() + 4, 4);
    }
    if (flag(8)) {
      mesh.props |= 0x10;
    }
    if (flag(9)) {
      mesh.props |= 0x20;
    }
  }
  else {
    mesh.props = r.u32();
    mesh.material_type = r.i32();
  }

  mesh.indices.resize(size_t(num_indices));
  read_u16_array(r, mesh.indices);
  if (has_lod) {
    r.skip(12); /* LOD min / max distance, limit. */
    if (size_t(verts) * 2 > r.remaining()) {
      return static_fail(r_error, "Truncated mesh LOD data");
    }
    r.skip(size_t(verts) * 2);
  }
  for (int f = 0; f < frames; f++) {
    if (f == 0) {
      mesh.positions.resize(size_t(verts) * 3);
      mesh.normals.resize(size_t(verts) * 3);
      mesh.uvs.resize(size_t(verts) * 2);
      for (size_t i = 0; i < size_t(verts); i++) {
        read_vec3(r, &mesh.positions[i * 3]);
        read_vec3(r, &mesh.normals[i * 3]);
        mesh.uvs[i * 2 + 0] = r.f32();
        mesh.uvs[i * 2 + 1] = r.f32();
      }
    }
    else {
      std::vector<float> positions(size_t(verts) * 3);
      for (size_t i = 0; i < size_t(verts); i++) {
        read_vec3(r, &positions[i * 3]);
        r.skip(20);
      }
      mesh.animated_positions.push_back(std::move(positions));
    }
  }

  std::string line = afile_read_cstring(r);
  if (line.compare(0, 15, "DETAILTEXTURE: ") == 0) {
    mesh.detail_texture_raw = line.substr(15);
    mesh.detail_texture = gbk_to_utf8(mesh.detail_texture_raw);
    for (float &v : mesh.detail_matrix) {
      v = r.f32();
    }
    line = afile_read_cstring(r);
  }
  else {
    for (int i = 0; i < 16; i++) {
      mesh.detail_matrix[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    }
  }
  mesh.texture_raw = line.compare(0, 9, "TEXTURE: ") == 0 ? line.substr(9) : std::string();
  mesh.texture = gbk_to_utf8(mesh.texture_raw);
  if (r.failed()) {
    return static_fail(r_error, "Truncated mesh \"" + mesh.name + "\"");
  }
  if (!afile_read_material(r, false, mesh.material, r_error)) {
    return false;
  }
  if (mesh.version == 100) {
    if (size_t(verts) * size_t(frames) * 8 > r.remaining()) {
      return static_fail(r_error, "Truncated mesh light map coordinates");
    }
    for (int f = 0; f < frames; f++) {
      if (f == 0) {
        mesh.lightmap_uvs.resize(size_t(verts) * 2);
        read_vec3_array(r, mesh.lightmap_uvs);
      }
      else {
        r.skip(size_t(verts) * 8);
      }
    }
  }
  if (r.failed()) {
    return static_fail(r_error, "Truncated mesh \"" + mesh.name + "\"");
  }
  return true;
}

static bool parse_frame(BinaryReader &r, MoxFrame &frame, int depth, std::string *r_error)
{
  if (depth > MOX_MAX_DEPTH) {
    return static_fail(r_error, "Frame hierarchy too deep");
  }
  std::string name_raw;
  if (!read_versioned_name(r, "FRAME: ", frame.version, name_raw)) {
    return static_fail(r_error, "Truncated frame name");
  }
  frame.name = gbk_to_utf8(name_raw);

  const int32_t frame_count = r.i32();
  if (r.failed() || frame_count < 0 || size_t(frame_count) * 64 > r.remaining()) {
    return static_fail(r_error, "Invalid frame count in frame \"" + frame.name + "\"");
  }
  frame.relative_tms.resize(size_t(frame_count));
  for (std::array<float, 16> &m : frame.relative_tms) {
    for (float &v : m) {
      v = r.f32();
    }
  }

  const int32_t num_boxes = r.i32();
  const size_t box_size = frame.version == 1 ? 168 : 136;
  if (r.failed() || num_boxes < 0 || size_t(num_boxes) * box_size > r.remaining()) {
    return static_fail(r_error, "Invalid bounding box count in frame \"" + frame.name + "\"");
  }
  frame.obbs.resize(size_t(num_boxes));
  for (MoxOBB &obb : frame.obbs) {
    obb.name = gbk_to_utf8(r.string_fixed(32));
    float ext_unused[3];
    read_vec3(r, obb.center);
    read_vec3(r, obb.axis_x);
    read_vec3(r, obb.axis_y);
    read_vec3(r, obb.axis_z);
    for (int k = 0; k < 3; k++) {
      read_vec3(r, ext_unused); /* ExtX, ExtY, ExtZ are recomputed by the engine. */
    }
    read_vec3(r, obb.extents);
    if (frame.version == 1) {
      /* `A3DFRAMEOBB_PROP`: 4 bools, material type, 32 reserved bytes. */
      const uint8_t shoot = r.u8(), no_mark = r.u8(), pick = r.u8(), no_destroy = r.u8();
      obb.props = (shoot ? 1u : 0u) | (no_mark ? 2u : 0u) | (pick ? 4u : 0u) |
                  (no_destroy ? 8u : 0u);
      obb.material_type = r.i32();
      r.skip(32);
    }
    else {
      obb.props = r.u32();
      obb.material_type = r.i32();
    }
  }

  const uint32_t num_meshes = r.u32();
  if (r.failed() || size_t(num_meshes) * 16 > r.remaining()) {
    return static_fail(r_error, "Invalid mesh count in frame \"" + frame.name + "\"");
  }
  frame.meshes.resize(num_meshes);
  for (MoxMesh &mesh : frame.meshes) {
    if (!parse_mox_mesh(r, mesh, r_error)) {
      return false;
    }
  }

  /* The engine treats a missing child count at the end of the file as zero. */
  if (r.at_end()) {
    return true;
  }
  const uint32_t num_children = r.u32();
  if (r.failed() || size_t(num_children) * 16 > r.remaining()) {
    return static_fail(r_error, "Invalid child count in frame \"" + frame.name + "\"");
  }
  frame.children.resize(num_children);
  for (MoxFrame &child : frame.children) {
    if (!parse_frame(r, child, depth + 1, r_error)) {
      return false;
    }
  }
  return true;
}

bool parse_mox(ByteSpan data, MoxFrame &r_root, std::string *r_error)
{
  r_root = MoxFrame();
  const AFileContent content = afile_open(data);
  if (content.is_text) {
    return static_fail(r_error, "Text .mox files are not supported (missing MOXB tag)");
  }
  BinaryReader r(content.body);
  return parse_frame(r, r_root, 0, r_error);
}

/** \} */

}  // namespace blender::io::angelica
