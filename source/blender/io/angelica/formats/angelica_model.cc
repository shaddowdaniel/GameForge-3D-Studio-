/* SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup angelica
 *
 * `.smd` layout (grounded on `A3DSkinModelMan::LoadModelFromFile`, `A3DSkinModelActionCore::Load`,
 * `A3DSkinModel::LoadHanger` and `A3DSkinModelAux.cpp` of Angelica 2.2 and the 1.5.5 client):
 *
 * - `A3DSKINMODELFILEHEADER` (packed, 84 bytes): `uint32 'ASMD'`, `uint32 version`,
 *   `int iNumSkin, iNumAction, iNumHanger, iNumProp`, 60 reserved bytes.
 * - `AFile` strings: skeleton file, `iNumSkin` skin files, physique file, version >= 8 the track
 *   directory.
 * - Actions: name, then version < 6 `{int group, int iNumJoint, uint32 flags}` followed by
 *   `iNumJoint` x `{int joint, float start, float end, int loops, float speed}` (the first one
 *   gives the frame range), versions 6..8 `{float start, float end}`, version 9
 *   `{float start, float end, int fps}`; version >= 7 a track set file name.
 * - Hangers: `int type`, name, bind target, `{int type, bool on_bone}` (8 bytes), then
 *   type 0 (gfx) the effect file, type 1 (skin model) the model file and the child hook name.
 * - Properties: name / value string pairs.
 *
 * `.ecm` text layout (`CECModelStaticData::LoadData`, `LoadAdditionalSkin`, `CECModelHook::Load`
 * in GfxCommon of both clients): one `Key: value` per line. `Version`, `SkinModelPath`, then
 * version gated header values (`AutoUpdata` >= 33, `OrgColor` >= 16, `EmissiveCol` >= 52,
 * `SrcBlend`/`DestBlend` >= 21, `OuterNum` + `Float` lines, `NewScale` >= 28, `BoneNum` + bone
 * scales, ...), co-effects, `ComActCount` + combined actions, scripts, then `AddiSkinCount` +
 * `AddiSkinPath` lines (>= 8), `ChildCount` + `ChildName`/`ChildPath`/`HHName`/`CCName` (>= 5),
 * `PhysFileName` (>= 34) and `ECMHookCount` + hooks (>= 41). Combined actions are not decoded:
 * the additional skin / child section is located by its unique `AddiSkinCount:` /
 * `ChildCount:` keys after `ComActCount:`.
 */

#include "angelica_model.hh"
#include "angelica_skin.hh"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace blender::io::angelica {

static bool model_fail(std::string *r_error, const std::string &message)
{
  if (r_error) {
    *r_error = message;
  }
  return false;
}

/* -------------------------------------------------------------------- */
/** \name Skin Model (.smd)
 * \{ */

static void read_path(BinaryReader &reader, std::string &r_utf8, std::string &r_raw)
{
  r_raw = reader.string_i32();
  r_utf8 = gbk_to_utf8(r_raw);
}

/** `af_GetFileTitle` + `af_ChangeFileExt(.., "")`: file name without directory and extension. */
static std::string file_title_no_ext(std::string_view path)
{
  const size_t slash = path.find_last_of("\\/");
  if (slash != std::string_view::npos) {
    path = path.substr(slash + 1);
  }
  const size_t dot = path.find_last_of('.');
  if (dot != std::string_view::npos) {
    path = path.substr(0, dot);
  }
  return std::string(path);
}

bool parse_skin_model(ByteSpan data,
                      SkinModel &r_model,
                      std::string *r_error,
                      std::string_view file_name)
{
  r_model = SkinModel();
  const AFileContent content = afile_open(data);
  BinaryReader reader(content.body);

  if (reader.remaining() < 84) {
    return model_fail(r_error, "File too small for a skin model header");
  }
  const uint32_t flags = reader.u32();
  const uint32_t version = reader.u32();
  if (flags != SKIN_MODEL_FILE_IDENTIFY) {
    return model_fail(r_error, "Not a skin model (.smd) file");
  }
  if (version == 0 || version > SKIN_MODEL_FILE_VERSION) {
    return model_fail(r_error, "Unsupported skin model version " + std::to_string(version));
  }
  r_model.version = version;
  const int32_t num_skins = reader.i32();
  const int32_t num_actions = reader.i32();
  const int32_t num_hangers = reader.i32();
  const int32_t num_props = reader.i32();
  reader.skip(60);

  /* Every record holds at least one string length. */
  for (const int32_t count : {num_skins, num_actions, num_hangers, num_props}) {
    if (count < 0 || size_t(count) * 4 > reader.remaining()) {
      return model_fail(r_error, "Invalid skin model record count");
    }
  }

  read_path(reader, r_model.skeleton_file, r_model.skeleton_file_raw);
  for (int i = 0; i < num_skins; i++) {
    std::string utf8, raw;
    read_path(reader, utf8, raw);
    r_model.skin_files.push_back(std::move(utf8));
    r_model.skin_files_raw.push_back(std::move(raw));
  }
  read_path(reader, r_model.physique_file, r_model.physique_file_raw);
  if (version < 8) {
    r_model.tracks_dir_raw = "tcks_" + file_title_no_ext(file_name);
    r_model.tracks_dir = gbk_to_utf8(r_model.tracks_dir_raw);
    if (file_name.empty()) {
      r_model.tracks_dir.clear();
      r_model.tracks_dir_raw.clear();
    }
  }
  else {
    read_path(reader, r_model.tracks_dir, r_model.tracks_dir_raw);
  }
  if (reader.failed()) {
    return model_fail(r_error, "Truncated skin model file references");
  }

  r_model.actions.resize(size_t(num_actions));
  for (int i = 0; i < num_actions; i++) {
    SkinModelAction &action = r_model.actions[size_t(i)];
    action.name = gbk_to_utf8(reader.string_i32());
    if (version < 6) {
      reader.skip(4); /* Action group. */
      const int32_t num_joints = reader.i32();
      reader.skip(4); /* Flags. */
      if (reader.failed() || num_joints < 0 || size_t(num_joints) * 20 > reader.remaining()) {
        return model_fail(r_error, "Invalid joint action count in action \"" + action.name + "\"");
      }
      if (num_joints >= 1) {
        reader.skip(4); /* Joint index. */
        action.start_frame = reader.f32();
        action.end_frame = reader.f32();
        reader.skip(8); /* Loop count, speed factor. */
        reader.skip(size_t(num_joints - 1) * 20);
      }
    }
    else {
      action.start_frame = reader.f32();
      action.end_frame = reader.f32();
      if (version >= 9) {
        action.frame_rate = std::max(reader.i32(), 0);
      }
    }
    if (version >= 7) {
      read_path(reader, action.track_file, action.track_file_raw);
    }
    if (reader.failed()) {
      return model_fail(r_error, "Truncated action " + std::to_string(i));
    }
  }

  r_model.hangers.resize(size_t(num_hangers));
  for (int i = 0; i < num_hangers; i++) {
    SkinModelHanger &hanger = r_model.hangers[size_t(i)];
    const int32_t type = reader.i32();
    if (type != int32_t(SkinModelHangerType::Gfx) &&
        type != int32_t(SkinModelHangerType::SkinModel))
    {
      return model_fail(r_error, "Unknown hanger type " + std::to_string(type));
    }
    hanger.type = SkinModelHangerType(type);
    hanger.name = gbk_to_utf8(reader.string_i32());
    hanger.bind_to = gbk_to_utf8(reader.string_i32());
    reader.skip(4); /* `HANGERDATA::iType`, same as the leading type. */
    hanger.on_bone = reader.u8() != 0;
    reader.skip(3);
    read_path(reader, hanger.file, hanger.file_raw);
    if (hanger.type == SkinModelHangerType::SkinModel) {
      hanger.child_hook = gbk_to_utf8(reader.string_i32());
    }
    if (reader.failed()) {
      return model_fail(r_error, "Truncated hanger " + std::to_string(i));
    }
  }

  for (int i = 0; i < num_props; i++) {
    std::string name = gbk_to_utf8(reader.string_i32());
    std::string value = gbk_to_utf8(reader.string_i32());
    r_model.properties.emplace_back(std::move(name), std::move(value));
  }
  if (reader.failed()) {
    return model_fail(r_error, "Truncated skin model properties");
  }
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name EC Model (.ecm)
 * \{ */

/** Value after `key` when `line` starts with it (the key includes ": "). */
static bool key_value(const std::string &line, const char *key, std::string &r_value)
{
  const size_t len = std::strlen(key);
  if (line.compare(0, len, key) != 0) {
    return false;
  }
  r_value = line.substr(len);
  while (!r_value.empty() && (r_value.back() == ' ' || r_value.back() == '\t')) {
    r_value.pop_back();
  }
  return true;
}

static bool key_int(const std::string &line, const char *key, int &r_value)
{
  std::string value;
  return key_value(line, key, value) && std::sscanf(value.c_str(), "%d", &r_value) == 1;
}

static bool key_float(const std::string &line, const char *key, float &r_value)
{
  std::string value;
  return key_value(line, key, value) && std::sscanf(value.c_str(), "%f", &r_value) == 1;
}

namespace {

struct LineCursor {
  const std::vector<std::string> &lines;
  size_t pos = 0;

  const std::string &next()
  {
    static const std::string empty;
    return pos < lines.size() ? lines[pos++] : empty;
  }
  bool at_end() const
  {
    return pos >= lines.size();
  }
};

}  // namespace

/** A sane upper bound for counts in text files: there cannot be more records than lines. */
static bool valid_line_count(int count, const LineCursor &cursor, int lines_each)
{
  return count >= 0 && size_t(count) * size_t(lines_each) <= cursor.lines.size() - cursor.pos;
}

static bool parse_ec_model_text(ByteSpan body, ECModel &r_model, std::string *r_error)
{
  std::vector<std::string> lines;
  {
    BinaryReader reader(body);
    std::string line;
    while (afile_read_line(reader, line)) {
      lines.push_back(line);
    }
  }
  LineCursor cur{lines};

  int version = -1;
  if (!key_int(cur.next(), "Version: ", version) || version < 0) {
    return model_fail(r_error, "Not an EC model (.ecm) file");
  }
  if (uint32_t(version) > EC_MODEL_FILE_VERSION) {
    return model_fail(r_error, "Unsupported EC model version " + std::to_string(version));
  }
  r_model.version = uint32_t(version);

  std::string value;
  if (!key_value(cur.next(), "SkinModelPath: ", value)) {
    return model_fail(r_error, "Missing SkinModelPath in EC model");
  }
  r_model.skin_model_path_raw = value;
  r_model.skin_model_path = gbk_to_utf8(value);

  if (version >= 33) {
    int flag = 1;
    if (key_int(cur.next(), "AutoUpdata: ", flag)) {
      r_model.auto_update = flag != 0;
    }
  }
  if (version >= 16) {
    std::sscanf(cur.next().c_str(), "OrgColor: %x", &r_model.org_color);
    if (version >= 52) {
      std::sscanf(cur.next().c_str(), "EmissiveCol: %x", &r_model.emissive_color);
    }
    if (version >= 21) {
      key_int(cur.next(), "SrcBlend: ", r_model.src_blend);
      key_int(cur.next(), "DestBlend: ", r_model.dest_blend);
    }
    int num_outer = 0;
    key_int(cur.next(), "OuterNum: ", num_outer);
    if (!valid_line_count(num_outer, cur, 1)) {
      return model_fail(r_error, "Invalid OuterNum in EC model");
    }
    cur.pos += size_t(num_outer);

    bool new_scale = false;
    if (version >= 28) {
      int flag = 0;
      key_int(cur.next(), "NewScale: ", flag);
      new_scale = flag != 0;
    }
    int num_bone_scales = 0;
    key_int(cur.next(), "BoneNum: ", num_bone_scales);
    if (!valid_line_count(num_bone_scales, cur, new_scale ? 2 : 3)) {
      return model_fail(r_error, "Invalid bone scale count in EC model");
    }
    for (int i = 0; i < num_bone_scales; i++) {
      ECModelBoneScale scale;
      scale.new_method = new_scale;
      key_int(cur.next(), "BoneIndex: ", scale.bone_index);
      if (!new_scale) {
        key_int(cur.next(), "BoneSclType: ", scale.type);
      }
      std::sscanf(cur.next().c_str(),
                  "BoneScale: %f, %f, %f",
                  &scale.scale[0],
                  &scale.scale[1],
                  &scale.scale[2]);
      r_model.bone_scales.push_back(scale);
    }
  }

  /* Remaining header values up to the combined actions, by key. */
  size_t comact_line = lines.size();
  for (size_t i = cur.pos; i < lines.size(); i++) {
    const std::string &line = lines[i];
    int flag;
    if (line.compare(0, 13, "ComActCount: ") == 0) {
      comact_line = i;
      break;
    }
    key_float(line, "DefSpeed: ", r_model.default_play_speed);
    if (key_int(line, "CanCastShadow: ", flag)) {
      r_model.can_cast_shadow = flag != 0;
    }
    if (key_int(line, "RenderModel: ", flag)) {
      r_model.render_model = flag != 0;
    }
    if (key_int(line, "RenderEdge: ", flag)) {
      r_model.render_edge = flag != 0;
    }
  }
  if (comact_line == lines.size()) {
    /* Very old or truncated file: no combined actions, nothing after them can be located. */
    r_model.children_parsed = version < 5;
    return true;
  }

  /* Locate the additional skins / children after the combined actions. */
  const char *section_key = version >= 8 ? "AddiSkinCount: " : "ChildCount: ";
  size_t section = lines.size();
  if (version >= 5) {
    for (size_t i = comact_line + 1; i < lines.size(); i++) {
      if (lines[i].compare(0, std::strlen(section_key), section_key) == 0) {
        section = i;
        break;
      }
    }
    if (section == lines.size()) {
      r_model.children_parsed = false;
      return true;
    }
  }
  else {
    r_model.children_parsed = true;
    return true;
  }
  cur.pos = section;

  if (version >= 8) {
    int num_skins = 0;
    key_int(cur.next(), "AddiSkinCount: ", num_skins);
    if (!valid_line_count(num_skins, cur, 1)) {
      return model_fail(r_error, "Invalid AddiSkinCount in EC model");
    }
    for (int i = 0; i < num_skins; i++) {
      if (key_value(cur.next(), "AddiSkinPath: ", value) && !value.empty()) {
        r_model.additional_skins.push_back(gbk_to_utf8(value));
        r_model.additional_skins_raw.push_back(value);
      }
    }
  }

  int num_children = 0;
  if (!key_int(cur.next(), "ChildCount: ", num_children)) {
    return model_fail(r_error, "Missing ChildCount in EC model");
  }
  if (!valid_line_count(num_children, cur, 4)) {
    return model_fail(r_error, "Invalid ChildCount in EC model");
  }
  for (int i = 0; i < num_children; i++) {
    ECModelChild child;
    key_value(cur.next(), "ChildName: ", value);
    child.name = gbk_to_utf8(value);
    value.clear();
    key_value(cur.next(), "ChildPath: ", value);
    child.path_raw = value;
    child.path = gbk_to_utf8(value);
    value.clear();
    key_value(cur.next(), "HHName: ", value);
    child.parent_hook = gbk_to_utf8(value);
    value.clear();
    key_value(cur.next(), "CCName: ", value);
    child.child_hook = gbk_to_utf8(value);
    value.clear();
    r_model.children.push_back(std::move(child));
  }
  r_model.children_parsed = true;

  if (version >= 34 && !cur.at_end()) {
    if (key_value(cur.next(), "PhysFileName: ", value)) {
      r_model.physics_file = gbk_to_utf8(value);
    }
  }
  if (version >= 41 && !cur.at_end()) {
    int num_hooks = 0;
    key_int(cur.next(), "ECMHookCount: ", num_hooks);
    if (!valid_line_count(num_hooks, cur, 7)) {
      return model_fail(r_error, "Invalid ECMHookCount in EC model");
    }
    for (int i = 0; i < num_hooks; i++) {
      ECModelHook hook;
      key_value(cur.next(), "ECMHook: ", value);
      hook.name = gbk_to_utf8(value);
      key_int(cur.next(), "ID: ", hook.id);
      key_float(cur.next(), "Scale: ", hook.scale);
      for (int row = 0; row < 4; row++) {
        float *m = &hook.matrix[row * 4];
        m[0] = m[1] = m[2] = 0.0f;
        std::sscanf(cur.next().c_str(), "%f %f %f", &m[0], &m[1], &m[2]);
        m[3] = row == 3 ? 1.0f : 0.0f;
      }
      r_model.hooks.push_back(hook);
    }
  }
  return true;
}

static bool parse_ec_model_binary(ByteSpan body, ECModel &r_model, std::string *r_error)
{
  BinaryReader reader(body);
  r_model.version = reader.u32();
  if (reader.failed()) {
    return model_fail(r_error, "File too small for an EC model");
  }
  if (r_model.version > EC_MODEL_FILE_VERSION) {
    return model_fail(r_error, "Unsupported EC model version " + std::to_string(r_model.version));
  }
  read_path(reader, r_model.skin_model_path, r_model.skin_model_path_raw);
  r_model.org_color = reader.u32();
  r_model.src_blend = reader.i32();
  r_model.dest_blend = reader.i32();
  const int32_t num_outer = reader.i32();
  if (reader.failed() || num_outer < 0 || size_t(num_outer) * 4 > reader.remaining()) {
    return model_fail(r_error, "Invalid binary EC model header");
  }
  reader.skip(size_t(num_outer) * 4);
  const int32_t num_bone_scales = reader.i32();
  if (reader.failed() || num_bone_scales < 0 || size_t(num_bone_scales) * 20 > reader.remaining())
  {
    return model_fail(r_error, "Invalid binary EC model bone scales");
  }
  for (int i = 0; i < num_bone_scales; i++) {
    ECModelBoneScale scale;
    scale.bone_index = reader.i32();
    scale.type = reader.i32();
    for (float &s : scale.scale) {
      s = reader.f32();
    }
    r_model.bone_scales.push_back(scale);
  }
  const int32_t num_co_gfx = reader.i32();
  const int32_t num_com_acts = reader.i32();
  if (reader.failed()) {
    return model_fail(r_error, "Truncated binary EC model");
  }
  if (num_co_gfx != 0 || num_com_acts != 0 || r_model.version < 5) {
    /* Effects and combined actions are not decoded, the children cannot be reached. */
    r_model.children_parsed = r_model.version < 5;
    return true;
  }
  const int32_t num_children = reader.i32();
  if (reader.failed() || num_children < 0 || size_t(num_children) * 16 > reader.remaining()) {
    return model_fail(r_error, "Invalid binary EC model child count");
  }
  for (int i = 0; i < num_children; i++) {
    ECModelChild child;
    child.name = gbk_to_utf8(reader.string_i32());
    read_path(reader, child.path, child.path_raw);
    child.parent_hook = gbk_to_utf8(reader.string_i32());
    child.child_hook = gbk_to_utf8(reader.string_i32());
    r_model.children.push_back(std::move(child));
  }
  if (reader.failed()) {
    return model_fail(r_error, "Truncated binary EC model children");
  }
  r_model.children_parsed = true;
  return true;
}

bool parse_ec_model(ByteSpan data, ECModel &r_model, std::string *r_error)
{
  r_model = ECModel();
  const AFileContent content = afile_open(data);
  r_model.is_text = content.is_text;
  if (content.is_text) {
    return parse_ec_model_text(content.body, r_model, r_error);
  }
  return parse_ec_model_binary(content.body, r_model, r_error);
}

/** \} */

}  // namespace blender::io::angelica
