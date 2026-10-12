/* SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup angelica
 */

#include <cstring>

#include "BKE_collection.hh"
#include "BKE_image.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_tree_update.hh"
#include "BKE_object.hh"
#include "BKE_report.hh"

#include "BLI_math_matrix.hh"
#include "BLI_string.hh"
#include "BLI_string_utf8.hh"

#include "DNA_collection_types.h"
#include "DNA_image_types.h"
#include "DNA_layer_types.h"
#include "DNA_material_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"

#include "IMB_colormanagement.hh"
#include "IMB_imbuf.hh"
#include "IMB_imbuf_types.hh"

#include "MEM_guardedalloc.h"

#include "angelica_import_common.hh"

namespace blender::io::angelica {

float4x4 engine_matrix_to_blender(const float m[16], const float scale)
{
  /* The engine stores `M[row][col]` and transforms row vectors, so its basis vectors are the
   * rows. Blender's matrix is indexed `[col][row]` and transforms column vectors, so the engine
   * memory layout maps directly onto Blender's columns. Then swap Y and Z on both sides:
   * `B = C * E * C` with `C` the Y/Z permutation. */
  float4x4 e;
  std::memcpy(e.ptr(), m, sizeof(float) * 16);
  float4x4 b;
  const auto perm = [](const int i) { return i == 1 ? 2 : (i == 2 ? 1 : i); };
  for (int col = 0; col < 4; col++) {
    for (int row = 0; row < 4; row++) {
      b[col][row] = e[perm(col)][perm(row)];
    }
  }
  b[3][0] *= scale;
  b[3][1] *= scale;
  b[3][2] *= scale;
  return b;
}

std::string name_from_engine_path(std::string_view engine_path)
{
  std::string name = path_stem(engine_path);
  if (name.empty()) {
    name = "Angelica";
  }
  /* Data-block names are limited; keep them valid UTF-8 after truncation. */
  char buf[MAX_ID_NAME - 2];
  BLI_strncpy_utf8(buf, name.c_str(), sizeof(buf));
  BLI_str_utf8_invalid_substitute(buf, strlen(buf), '?');
  return buf;
}

std::string catalog_from_engine_path(std::string_view engine_path, std::string_view root)
{
  std::string dir = path_dirname(engine_path);
  std::string out = "Angelica";
  if (!root.empty()) {
    out += "/";
    out += root;
  }
  size_t pos = 0;
  while (pos < dir.size()) {
    size_t next = dir.find_first_of("\\/", pos);
    if (next == std::string::npos) {
      next = dir.size();
    }
    std::string part = dir.substr(pos, next - pos);
    if (!part.empty() && part != "." && part.find(':') == std::string::npos) {
      out += "/";
      out += part;
    }
    pos = next + 1;
  }
  return out;
}

ImportContext::ImportContext(Main *bmain,
                             Scene *scene,
                             ViewLayer *view_layer,
                             ReportList *reports,
                             std::string client_root,
                             const float global_scale,
                             const bool import_textures,
                             const bool link_to_scene)
    : bmain(bmain),
      scene(scene),
      view_layer(view_layer),
      reports(reports),
      scale(global_scale),
      import_textures(import_textures),
      link_to_scene(link_to_scene),
      vfs(std::move(client_root))
{
}

ImportContext::~ImportContext() = default;

void ImportContext::warn(const std::string &message)
{
  if (reported_.add(message, true)) {
    BKE_report(reports, RPT_WARNING, message.c_str());
  }
}

void ImportContext::error(const std::string &message)
{
  BKE_report(reports, RPT_ERROR, message.c_str());
}

Collection *ImportContext::target_collection()
{
  if (collection_ != nullptr) {
    return collection_;
  }
  if (view_layer == nullptr) {
    return nullptr;
  }
  LayerCollection *lc = BKE_layer_collection_get_active_editable(view_layer);
  collection_ = lc ? lc->collection : nullptr;
  return collection_;
}

void ImportContext::set_target_collection(Collection *collection)
{
  collection_ = collection;
}

Object *ImportContext::add_object(const std::string &name, const short type, ID *data)
{
  Object *ob = BKE_object_add_only_object(bmain, ObjectType(type), name.c_str());
  ob->data = data;
  if (link_to_scene) {
    if (Collection *collection = target_collection()) {
      BKE_collection_object_add(bmain, collection, ob);
    }
  }
  return ob;
}

Image *ImportContext::load_image(const TextureRequest &request)
{
  if (!import_textures || request.path.empty()) {
    return nullptr;
  }
  const std::string key = normalize_engine_path(request.base_dir, false) + "|" +
                          normalize_engine_path(request.path, false);
  if (Image *const *cached = image_cache_.lookup_ptr(key)) {
    return *cached;
  }

  Bytes data;
  bool found = vfs.read(request.path, data, request.base_dir);
  if (!found) {
    /* Models keep their textures in a `textures` folder next to them, referenced by name. */
    const std::string name = path_basename(request.path);
    found = vfs.read(path_join("textures", name), data, request.base_dir) ||
            vfs.read(path_join("tex", name), data, request.base_dir);
  }

  Image *image = nullptr;
  if (found && !data.empty()) {
    image = image_from_memory(name_from_engine_path(request.path) + path_extension(request.path),
                              normalize_engine_path(request.path, false),
                              data.data(),
                              data.size());
  }
  else {
    warn("Angelica: texture not found: " + request.path);
  }
  image_cache_.add(key, image);
  return image;
}

Image *ImportContext::image_from_memory(const std::string &name,
                                        const std::string &virtual_path,
                                        const uint8_t *data,
                                        const size_t size)
{
  /* Use a stable, readable path so re-imports and the image editor show where it came from. */
  std::string filepath = "//angelica/" + virtual_path;
  for (char &c : filepath) {
    if (c == '\\') {
      c = '/';
    }
  }
  Image *image = BKE_image_load_exists(bmain, filepath.c_str(), false);
  if (image != nullptr && !BKE_image_has_packedfile(image)) {
    BKE_image_free_buffers(image);
    char *data_dup = MEM_new_array_uninitialized<char>(size, __func__);
    std::memcpy(data_dup, data, size);
    BKE_image_packfiles_from_mem(reports, image, data_dup, size);
    image->source = IMA_SRC_FILE;
    image->type = IMA_TYPE_IMAGE;
    BKE_libblock_rename(*bmain, image->id, name.c_str());
  }
  return image;
}

Image *ImportContext::image_from_pixels(const std::string &name,
                                        const int width,
                                        const int height,
                                        const uint8_t *rgba,
                                        const bool non_color)
{
  ImBuf *ibuf = IMB_allocImBuf(width, height, ImBufFlags::ByteData);
  if (ibuf == nullptr) {
    return nullptr;
  }
  uint8_t *dst = ibuf->byte_data_for_write();
  /* Blender stores the bottom row first. */
  for (int y = 0; y < height; y++) {
    std::memcpy(dst + size_t(height - 1 - y) * size_t(width) * 4,
                rgba + size_t(y) * size_t(width) * 4,
                size_t(width) * 4);
  }
  Image *image = BKE_image_add_from_imbuf(bmain, ibuf, name.c_str());
  IMB_freeImBuf(ibuf);
  if (image == nullptr) {
    return nullptr;
  }
  if (non_color) {
    STRNCPY(image->colorspace_settings.name,
            IMB_colormanagement_role_colorspace_name_get(COLOR_ROLE_DATA));
  }
  BKE_image_memorypack(image);
  return image;
}

static bNode *add_node(bNodeTree *ntree, const int type, const float x, const float y)
{
  bNode *node = bke::node_add_static_node(nullptr, *ntree, type);
  node->location[0] = x;
  node->location[1] = y;
  return node;
}

static void link_sockets(
    bNodeTree *ntree, bNode *from_node, const char *from_id, bNode *to_node, const char *to_id)
{
  bNodeSocket *from_sock = bke::node_find_socket(*from_node, SOCK_OUT, UString(from_id));
  bNodeSocket *to_sock = bke::node_find_socket(*to_node, SOCK_IN, UString(to_id));
  if (from_sock && to_sock) {
    bke::node_add_link(*ntree, *from_node, *from_sock, *to_node, *to_sock);
  }
}

Material *ImportContext::material_for_texture(const std::string &name,
                                              Image *image,
                                              const float diffuse[4],
                                              const int alpha_mode,
                                              const bool two_sided)
{
  const float default_color[4] = {0.8f, 0.8f, 0.8f, 1.0f};
  const float *color = diffuse ? diffuse : default_color;
  char key_buf[256];
  SNPRINTF(key_buf,
           "%p|%.3f|%.3f|%.3f|%.3f|%d|%d",
           static_cast<void *>(image),
           color[0],
           color[1],
           color[2],
           color[3],
           alpha_mode,
           int(two_sided));
  const std::string key = key_buf;
  if (Material *const *cached = material_cache_.lookup_ptr(key)) {
    return *cached;
  }

  Material *mat = BKE_material_add(bmain, name.c_str());
  id_us_min(&mat->id);
  copy_v3_v3(&mat->r, color);
  mat->a = color[3];

  bNodeTree *ntree = mat->nodetree;
  bNode *bsdf = add_node(ntree, SH_NODE_BSDF_PRINCIPLED, 0.0f, 300.0f);
  bNode *output = add_node(ntree, SH_NODE_OUTPUT_MATERIAL, 280.0f, 300.0f);
  if (bNodeSocket *socket = bke::node_find_socket(*bsdf, SOCK_IN, UString("Base Color"))) {
    float *value = static_cast<bNodeSocketValueRGBA *>(socket->default_value)->value;
    copy_v4_v4(value, color);
  }
  if (bNodeSocket *socket = bke::node_find_socket(*bsdf, SOCK_IN, UString("Roughness"))) {
    static_cast<bNodeSocketValueFloat *>(socket->default_value)->value = 0.8f;
  }
  if (image != nullptr) {
    bNode *tex = add_node(ntree, SH_NODE_TEX_IMAGE, -400.0f, 300.0f);
    tex->id = &image->id;
    id_us_plus(&image->id);
    link_sockets(ntree, tex, "Color", bsdf, "Base Color");
    if (alpha_mode != 0) {
      link_sockets(ntree, tex, "Alpha", bsdf, "Alpha");
    }
  }
  if (alpha_mode == 2) {
    mat->surface_render_method = MA_SURFACE_METHOD_FORWARD;
  }
  if (!two_sided) {
    mat->blend_flag |= MA_BL_CULL_BACKFACE;
  }
  link_sockets(ntree, bsdf, "BSDF", output, "Surface");
  bke::node_set_active(*ntree, *output);
  BKE_ntree_update_after_single_tree_change(*bmain, *ntree);

  material_cache_.add(key, mat);
  return mat;
}

void ImportContext::add_asset(ID *id,
                              std::string catalog_path,
                              std::string description,
                              std::vector<std::string> tags)
{
  ImportedAsset asset;
  asset.id = id;
  asset.catalog_path = std::move(catalog_path);
  asset.description = std::move(description);
  asset.tags = std::move(tags);
  assets.push_back(std::move(asset));
}

}  // namespace blender::io::angelica
