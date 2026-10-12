/* SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup angelica
 *
 * Terrain import. The client terrain is split into mask areas (256 m squares in most maps), each
 * with its own texture layers, 8-bit blend masks and lightmap, so every mask area becomes one
 * object with a material that reproduces the client's splatting:
 *
 *   color = layer0; color = mix(color, layerN, maskN) for each further layer; color *= lightmap
 *
 * Layer textures are projected like the client does (`byProjAxis`): 1 = top (x, z), which is
 * almost every layer, 2 = front (x, y - min height), 0 = side (z, y - min height), then scaled by
 * the layer's UV scale.
 */

#include <algorithm>
#include <cmath>
#include <functional>

#include "BKE_attribute.hh"
#include "BKE_collection.hh"
#include "BKE_layer.hh"
#include "BKE_lib_id.hh"
#include "BKE_main.hh"
#include "BKE_material.hh"
#include "BKE_mesh.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_tree_update.hh"
#include "BKE_object.hh"

#include "BLI_math_vector.hh"
#include "BLI_math_vector_c.hh"
#include "BLI_string.hh"

#include "DNA_collection_types.h"
#include "DNA_layer_types.h"
#include "DNA_material_types.h"
#include "DNA_mesh_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"

#include "angelica_hmap.hh"
#include "angelica_import_terrain.hh"
#include "angelica_terrain.hh"

namespace blender::io::angelica {

/** Above this many vertices the detail step is raised automatically to keep Blender responsive. */
static constexpr int64_t max_terrain_verts = 6'000'000;

static int auto_step(const int64_t verts_x, const int64_t verts_z, int step)
{
  step = std::max(step, 1);
  while ((verts_x / step + 1) * (verts_z / step + 1) > max_terrain_verts) {
    step++;
  }
  return step;
}

/* -------------------------------------------------------------------- */
/** \name Grid Meshes
 * \{ */

struct GridSpec {
  /** Number of quads along each axis. */
  int cells_x = 0;
  int cells_z = 0;
  /** Returns the engine position of grid vertex (row, col), row 0 north. */
  std::function<float3(int row, int col)> position;
  /** Optional engine normal of grid vertex (row, col). */
  std::function<float3(int row, int col)> normal;
};

static Mesh *build_grid_mesh(const GridSpec &spec, const float3 &origin_engine, const float scale)
{
  const int nx = spec.cells_x + 1;
  const int nz = spec.cells_z + 1;
  Mesh *mesh = BKE_mesh_new_nomain(
      nx * nz, 0, spec.cells_x * spec.cells_z, spec.cells_x * spec.cells_z * 4);

  MutableSpan<float3> positions = mesh->vert_positions_for_write();
  for (int row = 0; row < nz; row++) {
    for (int col = 0; col < nx; col++) {
      const float3 p = spec.position(row, col) - origin_engine;
      positions[row * nx + col] = to_blender(p.x, p.y, p.z, scale);
    }
  }

  MutableSpan<int> face_offsets = mesh->face_offsets_for_write();
  MutableSpan<int> corner_verts = mesh->corner_verts_for_write();
  int face = 0;
  for (int row = 0; row < spec.cells_z; row++) {
    for (int col = 0; col < spec.cells_x; col++) {
      const int nw = row * nx + col;
      const int ne = nw + 1;
      const int sw = nw + nx;
      const int se = sw + 1;
      face_offsets[face] = face * 4;
      /* Counter-clockwise seen from above in Blender space (+X east, +Y north). */
      corner_verts[face * 4 + 0] = sw;
      corner_verts[face * 4 + 1] = se;
      corner_verts[face * 4 + 2] = ne;
      corner_verts[face * 4 + 3] = nw;
      face++;
    }
  }
  bke::mesh_calc_edges(*mesh, false, false);
  bke::mesh_smooth_set(*mesh, true);

  if (spec.normal) {
    Array<float3> normals(nx * nz);
    for (int row = 0; row < nz; row++) {
      for (int col = 0; col < nx; col++) {
        const float3 n = spec.normal(row, col);
        normals[row * nx + col] = math::normalize(to_blender(n.x, n.y, n.z));
      }
    }
    bke::mesh_set_custom_normals_from_verts_normalized(*mesh, normals);
  }
  return mesh;
}

/** Add a corner-domain UV map computed from each corner's vertex. */
static void add_uv_map(Mesh *mesh,
                       const char *name,
                       const std::function<float2(int vert)> &uv_of_vert)
{
  bke::MutableAttributeAccessor attributes = mesh->attributes_for_write();
  bke::SpanAttributeWriter<float2> uv = attributes.lookup_or_add_for_write_only_span<float2>(
      name, bke::AttrDomain::Corner);
  const Span<int> corner_verts = mesh->corner_verts();
  for (const int corner : corner_verts.index_range()) {
    uv.span[corner] = uv_of_vert(corner_verts[corner]);
  }
  uv.finish();
}

static Mesh *mesh_to_main(Main *bmain, Mesh *mesh_nomain, const std::string &name)
{
  Mesh *mesh = BKE_mesh_add(bmain, name.c_str());
  BKE_mesh_nomain_to_mesh(mesh_nomain, mesh, nullptr);
  return mesh;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Splat Materials
 * \{ */

static bNode *add_node(bNodeTree *ntree, const int type, const float x, const float y)
{
  bNode *node = bke::node_add_static_node(nullptr, *ntree, type);
  node->location[0] = x;
  node->location[1] = y;
  return node;
}

static void link(bNodeTree *ntree, bNode *from, const char *from_id, bNode *to, const char *to_id)
{
  bNodeSocket *a = bke::node_find_socket(*from, SOCK_OUT, UString(from_id));
  bNodeSocket *b = bke::node_find_socket(*to, SOCK_IN, UString(to_id));
  if (a && b) {
    bke::node_add_link(*ntree, *from, *a, *to, *b);
  }
}

static void set_input_vector(bNode *node, const char *id, const float3 &value)
{
  if (bNodeSocket *socket = bke::node_find_socket(*node, SOCK_IN, UString(id))) {
    copy_v3_v3(static_cast<bNodeSocketValueVector *>(socket->default_value)->value, value);
  }
}

static void set_input_float(bNode *node, const char *id, const float value)
{
  if (bNodeSocket *socket = bke::node_find_socket(*node, SOCK_IN, UString(id))) {
    static_cast<bNodeSocketValueFloat *>(socket->default_value)->value = value;
  }
}

static bNode *add_uv_node(bNodeTree *ntree, const char *uv_name, const float x, const float y)
{
  bNode *node = add_node(ntree, SH_NODE_UVMAP, x, y);
  STRNCPY(static_cast<NodeShaderUVMap *>(node->storage)->uv_map, uv_name);
  return node;
}

static bNode *add_image_node(bNodeTree *ntree, Image *image, const float x, const float y)
{
  bNode *node = add_node(ntree, SH_NODE_TEX_IMAGE, x, y);
  node->id = &image->id;
  id_us_plus(&image->id);
  return node;
}

static bNode *add_mix_color(bNodeTree *ntree, const int blend_type, const float x, const float y)
{
  bNode *node = add_node(ntree, SH_NODE_MIX, x, y);
  NodeShaderMix *storage = static_cast<NodeShaderMix *>(node->storage);
  storage->data_type = SOCK_RGBA;
  storage->blend_type = int8_t(blend_type);
  storage->clamp_result = 1;
  return node;
}

static const char *uv_for_axis(const uint8_t proj_axis)
{
  switch (proj_axis) {
    case 0:
      return "Side";
    case 2:
      return "Front";
    default:
      return "Top";
  }
}

static Material *make_splat_material(ImportContext &ctx,
                                     const std::string &name,
                                     const TerrainHeightfield &field,
                                     const TerrainMaskArea &area,
                                     Image *lightmap)
{
  Material *mat = BKE_material_add(ctx.bmain, name.c_str());
  id_us_min(&mat->id);
  bNodeTree *ntree = mat->nodetree;

  bNode *bsdf = add_node(ntree, SH_NODE_BSDF_PRINCIPLED, 600.0f, 300.0f);
  bNode *output = add_node(ntree, SH_NODE_OUTPUT_MATERIAL, 900.0f, 300.0f);
  set_input_float(bsdf, "Roughness", 0.9f);

  bNode *mask_uv = add_uv_node(ntree, "Mask", -1200.0f, -200.0f);

  bNode *color_node = nullptr;
  const char *color_socket = "Color";
  float y = 600.0f;
  for (size_t i = 0; i < area.layers.size(); i++) {
    const TerrainMaskLayer &layer = area.layers[i];
    if (layer.texture_index < 0 || size_t(layer.texture_index) >= field.texture_paths.size()) {
      continue;
    }
    Image *image = ctx.load_image({field.texture_paths[layer.texture_index], ""});
    if (image == nullptr) {
      continue;
    }

    bNode *uv = add_uv_node(ntree, uv_for_axis(layer.proj_axis), -1200.0f, y);
    bNode *mapping = add_node(ntree, SH_NODE_MAPPING, -1000.0f, y);
    set_input_vector(mapping, "Scale", float3(layer.u_scale, layer.v_scale, 1.0f));
    link(ntree, uv, "UV", mapping, "Vector");
    bNode *tex = add_image_node(ntree, image, -800.0f, y);
    link(ntree, mapping, "Vector", tex, "Vector");

    if (color_node == nullptr || layer.mask.empty()) {
      /* The first layer (and any layer without a mask) covers everything below it. */
      color_node = tex;
      color_socket = "Color";
    }
    else {
      /* Masks are square 8-bit images; store them as grey RGBA. */
      const int size = layer.mask_size;
      std::vector<uint8_t> rgba(size_t(size) * size_t(size) * 4);
      for (size_t p = 0; p < size_t(size) * size_t(size) && p < layer.mask.size(); p++) {
        rgba[p * 4 + 0] = rgba[p * 4 + 1] = rgba[p * 4 + 2] = layer.mask[p];
        rgba[p * 4 + 3] = 255;
      }
      char mask_name[64];
      SNPRINTF(mask_name, "%s_mask%d", name.c_str(), int(i));
      Image *mask_image = ctx.image_from_pixels(mask_name, size, size, rgba.data(), true);
      bNode *mask_tex = mask_image ? add_image_node(ntree, mask_image, -800.0f, y - 280.0f) :
                                     nullptr;
      if (mask_tex) {
        link(ntree, mask_uv, "UV", mask_tex, "Vector");
      }
      bNode *mix = add_mix_color(ntree, MA_RAMP_BLEND, -300.0f, y);
      link(ntree, color_node, color_socket, mix, "A_Color");
      link(ntree, tex, "Color", mix, "B_Color");
      if (mask_tex) {
        link(ntree, mask_tex, "Color", mix, "Factor_Float");
      }
      color_node = mix;
      color_socket = "Result_Color";
    }
    y -= 600.0f;
  }

  if (color_node != nullptr && lightmap != nullptr) {
    bNode *lm_tex = add_image_node(ntree, lightmap, -300.0f, -600.0f);
    link(ntree, mask_uv, "UV", lm_tex, "Vector");
    bNode *mul = add_mix_color(ntree, MA_RAMP_MULT, 200.0f, 300.0f);
    set_input_float(mul, "Factor_Float", 1.0f);
    link(ntree, color_node, color_socket, mul, "A_Color");
    link(ntree, lm_tex, "Color", mul, "B_Color");
    color_node = mul;
    color_socket = "Result_Color";
  }
  if (color_node != nullptr) {
    link(ntree, color_node, color_socket, bsdf, "Base Color");
  }
  link(ntree, bsdf, "BSDF", output, "Surface");
  bke::node_set_active(*ntree, *output);
  BKE_ntree_update_after_single_tree_change(*ctx.bmain, *ntree);
  return mat;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Client Terrain
 * \{ */

static Collection *add_collection(ImportContext &ctx, const std::string &name)
{
  Collection *parent = ctx.link_to_scene ? ctx.target_collection() : nullptr;
  /* Without a parent the collection only lives in the file, kept by its asset fake user. */
  return BKE_collection_add(ctx.bmain, parent, name.c_str());
}

static void add_to_collection(ImportContext &ctx, Collection *collection, Object *ob)
{
  BKE_collection_object_add(ctx.bmain, collection, ob);
}

bool import_terrain(ImportContext &ctx,
                    const std::string &map_dir,
                    const std::string &map_code,
                    const int step_requested)
{
  TerrainHeightfield field;
  std::string error;
  if (!load_terrain_heightfield(map_dir, map_code, field, &error)) {
    if (!error.empty()) {
      ctx.warn("Angelica terrain '" + map_code + "': " + error);
    }
    return false;
  }
  for (const std::string &warning : field.warnings) {
    ctx.warn("Angelica terrain '" + map_code + "': " + warning);
  }

  const int step = auto_step(field.verts_x, field.verts_z, step_requested);
  if (step != std::max(step_requested, 1)) {
    ctx.warn("Angelica terrain '" + map_code + "' is large; using detail step " +
             std::to_string(step));
  }

  Collection *collection = add_collection(ctx, map_code + " Terrain");

  const int mask_grid = field.mask_grid > 0 ? field.mask_grid : std::max(field.verts_x - 1, 1);
  const int areas_x = field.mask_areas_x > 0 ? field.mask_areas_x : 1;
  const int areas_z = field.mask_areas_z > 0 ? field.mask_areas_z : 1;
  const int area_step = std::min(step, mask_grid);
  const int area_cells = std::max(mask_grid / area_step, 1);

  float min_height = 0.0f;
  if (!field.sub_terrains.empty()) {
    min_height = field.sub_terrains[0].min_height;
    for (const Trn2SubTerrain &sub : field.sub_terrains) {
      min_height = std::min(min_height, sub.min_height);
    }
  }

  for (int area_row = 0; area_row < areas_z; area_row++) {
    for (int area_col = 0; area_col < areas_x; area_col++) {
      const int row0 = area_row * mask_grid;
      const int col0 = area_col * mask_grid;
      if (row0 >= field.verts_z - 1 || col0 >= field.verts_x - 1) {
        continue;
      }

      auto vert_row = [&](const int r) {
        return std::min(row0 + r * area_step, field.verts_z - 1);
      };
      auto vert_col = [&](const int c) {
        return std::min(col0 + c * area_step, field.verts_x - 1);
      };
      auto engine_pos = [&](const int r, const int c) {
        const int vr = vert_row(r);
        const int vc = vert_col(c);
        return float3(field.origin_x + float(vc) * field.grid_size,
                      field.height_at(vr, vc),
                      field.origin_z - float(vr) * field.grid_size);
      };

      /* Object origin at the north-west corner of the area, at height zero. */
      const float3 origin(field.origin_x + float(col0) * field.grid_size,
                          0.0f,
                          field.origin_z - float(row0) * field.grid_size);

      GridSpec spec;
      spec.cells_x = area_cells;
      spec.cells_z = area_cells;
      spec.position = engine_pos;
      if (!field.normals.empty()) {
        spec.normal = [&](const int r, const int c) {
          const size_t v = size_t(vert_row(r)) * size_t(field.verts_x) + size_t(vert_col(c));
          return float3(
              field.normals[v * 3 + 0], field.normals[v * 3 + 1], field.normals[v * 3 + 2]);
        };
      }
      Mesh *mesh_nomain = build_grid_mesh(spec, origin, ctx.scale);

      const int nx = area_cells + 1;
      add_uv_map(mesh_nomain, "Top", [&](const int v) {
        const float3 p = engine_pos(v / nx, v % nx);
        return float2(p.x, -p.z);
      });
      add_uv_map(mesh_nomain, "Front", [&](const int v) {
        const float3 p = engine_pos(v / nx, v % nx);
        return float2(p.x, -(p.y - min_height));
      });
      add_uv_map(mesh_nomain, "Side", [&](const int v) {
        const float3 p = engine_pos(v / nx, v % nx);
        return float2(p.z, -(p.y - min_height));
      });
      add_uv_map(mesh_nomain, "Mask", [&](const int v) {
        const float u = float((v % nx) * area_step) / float(mask_grid);
        const float w = float((v / nx) * area_step) / float(mask_grid);
        /* Engine images have row 0 at the top. */
        return float2(u, 1.0f - w);
      });
      mesh_nomain->uv_maps_active_set("Top");
      mesh_nomain->uv_maps_default_set("Top");

      char name[64];
      SNPRINTF(name, "%s_%02d_%02d", map_code.c_str(), area_row, area_col);
      Mesh *mesh = mesh_to_main(ctx.bmain, mesh_nomain, name);
      Object *ob = BKE_object_add_only_object(ctx.bmain, OB_MESH, name);
      ob->data = &mesh->id;
      const float3 loc = to_blender(origin.x, origin.y, origin.z, ctx.scale);
      copy_v3_v3(ob->loc, loc);
      add_to_collection(ctx, collection, ob);

      const size_t area_index = size_t(area_row) * size_t(areas_x) + size_t(area_col);
      if (ctx.import_textures && area_index < field.mask_areas.size()) {
        const TerrainMaskArea &area = field.mask_areas[area_index];
        Image *lightmap = nullptr;
        if (!area.lightmap.empty()) {
          if (area.lightmap_is_dds) {
            lightmap = ctx.image_from_memory(std::string(name) + "_lightmap.dds",
                                             "maps/" + map_code + "/" + name + "_lightmap.dds",
                                             area.lightmap.data(),
                                             area.lightmap.size());
          }
          else if (area.lightmap_size > 0 &&
                   area.lightmap.size() >= size_t(area.lightmap_size) * area.lightmap_size * 3)
          {
            const int s = area.lightmap_size;
            std::vector<uint8_t> rgba(size_t(s) * s * 4);
            for (size_t p = 0; p < size_t(s) * s; p++) {
              /* Raw lightmaps are stored as BGR. */
              rgba[p * 4 + 0] = area.lightmap[p * 3 + 2];
              rgba[p * 4 + 1] = area.lightmap[p * 3 + 1];
              rgba[p * 4 + 2] = area.lightmap[p * 3 + 0];
              rgba[p * 4 + 3] = 255;
            }
            lightmap = ctx.image_from_pixels(
                std::string(name) + "_lightmap", s, s, rgba.data(), false);
          }
        }
        Material *mat = make_splat_material(ctx, name, field, area, lightmap);
        BKE_object_material_assign(ctx.bmain, ob, mat, 1, BKE_MAT_ASSIGN_OBDATA);
      }
    }
  }

  std::vector<std::string> tags = {"Angelica", "Terrain", map_code};
  ctx.add_asset(&collection->id,
                "Angelica/maps/" + map_code,
                "Perfect World terrain of map " + map_code + " (" +
                    std::to_string(int(field.terrain_width)) + " x " +
                    std::to_string(int(field.terrain_height)) + " m)",
                tags);
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Server Heightmaps
 * \{ */

bool import_hmap_folder(ImportContext &ctx,
                        const std::string &hmap_dir,
                        const std::string &map_code,
                        const std::string &trn2_dir,
                        const int step_requested)
{
  HmapLoadOptions options;
  /* Server heightmaps are normalized against the height range of the client terrain. */
  if (!trn2_dir.empty()) {
    const std::string trn2_path = find_file_case_insensitive(trn2_dir, map_code + ".trn2");
    Bytes data;
    Trn2Info info;
    if (!trn2_path.empty() && read_file(trn2_path, data) && parse_trn2(data, info, nullptr) &&
        !info.sub_terrains.empty())
    {
      float hmin = info.sub_terrains[0].min_height;
      float hmax = info.sub_terrains[0].max_height;
      for (const Trn2SubTerrain &sub : info.sub_terrains) {
        hmin = std::min(hmin, sub.min_height);
        hmax = std::max(hmax, sub.max_height);
      }
      if (hmax > hmin) {
        options.has_height_range = true;
        options.height_min = hmin;
        options.height_max = hmax;
      }
    }
  }

  HmapMap map;
  std::string error;
  if (!load_hmap_folder(hmap_dir, map, &error, options)) {
    return false;
  }

  const int step = auto_step(map.verts_x, map.verts_z, step_requested);
  const int cells_x = std::max((map.verts_x - 1) / step, 1);
  const int cells_z = std::max((map.verts_z - 1) / step, 1);

  GridSpec spec;
  spec.cells_x = cells_x;
  spec.cells_z = cells_z;
  spec.position = [&](const int r, const int c) {
    const int vr = std::min(r * step, map.verts_z - 1);
    const int vc = std::min(c * step, map.verts_x - 1);
    return float3(map.origin_x() + float(vc) * map.grid_size,
                  map.height(vr, vc),
                  map.origin_z() - float(vr) * map.grid_size);
  };
  Mesh *mesh_nomain = build_grid_mesh(spec, float3(0.0f), ctx.scale);
  add_uv_map(mesh_nomain, "UVMap", [&](const int v) {
    const int nx = cells_x + 1;
    return float2(float(v % nx) / float(cells_x), 1.0f - float(v / nx) / float(cells_z));
  });

  const std::string name = map_code + "_heightmap";
  Mesh *mesh = mesh_to_main(ctx.bmain, mesh_nomain, name);
  Object *ob = ctx.add_object(name, OB_MESH, &mesh->id);
  ctx.add_asset(&ob->id,
                "Angelica/maps/" + map_code,
                "Perfect World server heightmap of map " + map_code,
                {"Angelica", "Heightmap", map_code});
  return true;
}

/** \} */

}  // namespace blender::io::angelica
