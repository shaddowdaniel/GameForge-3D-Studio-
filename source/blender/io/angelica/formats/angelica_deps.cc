/* SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup angelica
 */

#include "angelica_deps.hh"

#include <cstdlib>
#include <deque>
#include <unordered_map>

#include "angelica_model.hh"
#include "angelica_reader.hh"
#include "angelica_skin.hh"
#include "angelica_static.hh"

namespace blender::io::angelica {

const char *dependency_kind_name(const DependencyKind kind)
{
  switch (kind) {
    case DependencyKind::Root:
      return "root";
    case DependencyKind::Model:
      return "model";
    case DependencyKind::SkinModel:
      return "skin_model";
    case DependencyKind::Skin:
      return "skin";
    case DependencyKind::Skeleton:
      return "skeleton";
    case DependencyKind::Tracks:
      return "tracks";
    case DependencyKind::Texture:
      return "texture";
    case DependencyKind::Effect:
      return "effect";
    case DependencyKind::Sound:
      return "sound";
    case DependencyKind::Attack:
      return "attack";
    case DependencyKind::Physics:
      return "physics";
    case DependencyKind::Building:
      return "building";
    case DependencyKind::Other:
      break;
  }
  return "other";
}

namespace {

/** Effect event types of `A3DCombinedAction` (`EVENT_TYPE_*`). */
constexpr int EVENT_TYPE_GFX = 100;
constexpr int EVENT_TYPE_SFX = 101;

std::string engine_join(std::string_view dir, std::string_view name)
{
  if (dir.empty()) {
    return std::string(name);
  }
  std::string out(dir);
  out.push_back('\\');
  out.append(name);
  return out;
}

bool has_separator(std::string_view path)
{
  return path.find_first_of("\\/") != std::string_view::npos;
}

std::string_view trim(std::string_view s)
{
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) {
    s.remove_prefix(1);
  }
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
    s.remove_suffix(1);
  }
  return s;
}

/** Split a `Key: value` line of an `AFile` text file. */
bool split_key(std::string_view line, std::string_view &r_key, std::string_view &r_value)
{
  const size_t colon = line.find(':');
  if (colon == std::string_view::npos) {
    return false;
  }
  r_key = trim(line.substr(0, colon));
  r_value = trim(line.substr(colon + 1));
  return true;
}

/** Walk the `Key: value` lines of an `AFile` text file. Returns false for binary files. */
template<typename Fn> bool for_each_text_line(const Bytes &data, Fn &&fn)
{
  const AFileContent content = afile_open(data);
  if (!content.is_text) {
    return false;
  }
  BinaryReader reader(content.body);
  std::string line;
  while (afile_read_line(reader, line)) {
    std::string_view key, value;
    if (split_key(line, key, value)) {
      fn(key, value);
    }
  }
  return true;
}

class Collector {
 public:
  explicit Collector(ClientFileSystem &vfs) : vfs_(vfs) {}

  std::vector<Dependency> run(std::string_view root)
  {
    add(std::string(root), DependencyKind::Root, -1);
    while (!queue_.empty()) {
      const int index = queue_.front();
      queue_.pop_front();
      process(index);
    }
    return std::move(result_);
  }

 private:
  ClientFileSystem &vfs_;
  std::vector<Dependency> result_;
  std::unordered_map<std::string, int> index_;
  std::deque<int> queue_;

  bool exists(const std::string &path)
  {
    return vfs_.locate(path).kind != FileLocation::Kind::None;
  }

  /** List a file (once) and queue it for its own references. Returns its index. */
  int add(const std::string &path_utf8, const DependencyKind kind, const int parent)
  {
    std::string path = normalize_engine_path(path_utf8, false);
    if (path.empty()) {
      return -1;
    }
    if (const auto it = index_.find(path); it != index_.end()) {
      return it->second;
    }
    Dependency dep;
    dep.location = vfs_.locate(path);
    dep.path = std::move(path);
    dep.kind = kind;
    dep.parent = parent;
    const int index = int(result_.size());
    index_.emplace(dep.path, index);
    result_.push_back(std::move(dep));
    queue_.push_back(index);
    return index;
  }

  /** Companion files the engine only uses when present. */
  void add_optional(const std::string &path, const DependencyKind kind, const int parent)
  {
    if (exists(path)) {
      add(path, kind, parent);
    }
  }

  /** The first candidate the client has; the first one is listed (as missing) otherwise. */
  void add_first_found(const std::vector<std::string> &candidates,
                       const DependencyKind kind,
                       const int parent)
  {
    for (const std::string &candidate : candidates) {
      if (exists(candidate)) {
        add(candidate, kind, parent);
        return;
      }
    }
    if (!candidates.empty()) {
      add(candidates.front(), kind, parent);
    }
  }

  void process(const int index)
  {
    const std::string path = result_[index].path;
    if (result_[index].location.kind == FileLocation::Kind::None) {
      return;
    }
    const std::string ext = path_extension(path);
    if (!(ext == ".ecm" || ext == ".smd" || ext == ".ski" || ext == ".gfx" || ext == ".att" ||
          ext == ".sgc" || ext == ".bmd" || ext == ".mox"))
    {
      return;
    }
    Bytes data;
    if (!vfs_.read(path, data)) {
      return;
    }
    if (ext == ".ecm") {
      collect_ecm(index, path, data);
    }
    else if (ext == ".smd") {
      collect_smd(index, path, data);
    }
    else if (ext == ".ski") {
      collect_ski(index, path, data);
    }
    else if (ext == ".gfx") {
      collect_gfx(index, data);
    }
    else if (ext == ".att" || ext == ".sgc") {
      collect_att(index, data);
    }
    else if (ext == ".bmd") {
      collect_bmd(index, data);
    }
    else {
      collect_mox(index, path, data);
    }
  }

  void collect_ecm(const int index, const std::string &path, const Bytes &data)
  {
    const std::string dir = path_dirname(path);
    ECModel model;
    if (parse_ec_model(data, model, nullptr)) {
      if (!model.skin_model_path.empty()) {
        /* Client root; a bare file name that is not found there lives next to the `.ecm`. */
        std::string smd = model.skin_model_path;
        if (!has_separator(smd) && !exists(smd)) {
          smd = engine_join(dir, smd);
        }
        add(smd, DependencyKind::SkinModel, index);
      }
      for (const std::string &skin : model.additional_skins) {
        add(engine_join("models", skin), DependencyKind::Skin, index);
      }
      for (const ECModelChild &child : model.children) {
        if (!child.path.empty()) {
          add(child.path, DependencyKind::Model, index);
        }
      }
      if (!model.physics_file.empty()) {
        add(engine_join(dir, model.physics_file), DependencyKind::Physics, index);
      }
    }

    /* Convex hulls of the model (`CECModelStaticData::LoadData`). */
    const size_t sep = path.find('\\');
    if (sep != std::string::npos) {
      std::string hull = "grasses\\ecmodelhull" + path.substr(sep);
      hull = hull.substr(0, hull.size() - path_extension(hull).size()) + ".ecp";
      add_optional(hull, DependencyKind::Physics, index);
    }

    /* Combined action events (the parser above does not decode them). */
    int event_type = -1;
    for_each_text_line(data, [&](std::string_view key, std::string_view value) {
      if (key == "EventType") {
        event_type = std::atoi(std::string(value).c_str());
      }
      else if (key == "FxFilePath" && !value.empty()) {
        const std::string file = gbk_to_utf8(value);
        if (event_type == EVENT_TYPE_GFX) {
          add(engine_join("gfx", file), DependencyKind::Effect, index);
        }
        else if (event_type == EVENT_TYPE_SFX) {
          add(engine_join("sfx", file), DependencyKind::Sound, index);
        }
      }
      else if (key == "AtkPath" && !value.empty()) {
        add(engine_join("gfx\\skillattack", gbk_to_utf8(value)), DependencyKind::Attack, index);
      }
    });
  }

  void collect_smd(const int index, const std::string &path, const Bytes &data)
  {
    const std::string dir = path_dirname(path);
    SkinModel model;
    if (!parse_skin_model(data, model, nullptr, path_basename(path))) {
      return;
    }
    if (!model.skeleton_file.empty()) {
      add(engine_join(dir, model.skeleton_file), DependencyKind::Skeleton, index);
    }
    for (const std::string &skin : model.skin_files) {
      if (!skin.empty()) {
        add(engine_join(dir, skin), DependencyKind::Skin, index);
      }
    }
    const std::string stem = path.substr(0, path.size() - path_extension(path).size());
    add_optional(stem + ".phy", DependencyKind::Physics, index);
    add_optional(stem + ".stck", DependencyKind::Tracks, index);
    const std::string tracks_dir = model.tracks_dir.empty() ? dir :
                                                              engine_join(dir, model.tracks_dir);
    for (const SkinModelAction &action : model.actions) {
      if (!action.track_file.empty()) {
        add(engine_join(tracks_dir, action.track_file), DependencyKind::Tracks, index);
      }
    }
    for (const SkinModelHanger &hanger : model.hangers) {
      if (hanger.file.empty()) {
        continue;
      }
      if (hanger.type == SkinModelHangerType::Gfx) {
        add(engine_join("gfx", hanger.file), DependencyKind::Effect, index);
      }
      else {
        add(hanger.file, DependencyKind::SkinModel, index);
      }
    }
  }

  void collect_ski(const int index, const std::string &path, const Bytes &data)
  {
    const std::string dir = path_dirname(path);
    Skin skin;
    if (!parse_skin(data, skin, nullptr)) {
      return;
    }
    const std::string title = path_stem(path);
    for (const std::string &texture : skin.textures) {
      if (texture.empty()) {
        continue;
      }
      const std::string name = path_basename(texture);
      add_first_found({engine_join(engine_join(dir, "textures"), name),
                       engine_join(engine_join(dir, "tex_" + title), name)},
                      DependencyKind::Texture,
                      index);
    }
    const std::string stem = path.substr(0, path.size() - path_extension(path).size());
    add_optional(stem + ".sphy", DependencyKind::Physics, index);
  }

  void collect_gfx(const int index, const Bytes &data)
  {
    for_each_text_line(data, [&](std::string_view key, std::string_view value) {
      if (value.empty()) {
        return;
      }
      const std::string file = gbk_to_utf8(value);
      if (key == "TexFile" || key == "ShaderTex") {
        add(engine_join("gfx\\textures", file), DependencyKind::Texture, index);
      }
      else if (key == "GfxPath") {
        add(engine_join("gfx", file), DependencyKind::Effect, index);
      }
      else if (key == "ModelPath") {
        add(engine_join("gfx\\models", file), DependencyKind::SkinModel, index);
      }
      else if (key == "ECMPath") {
        add(file, DependencyKind::Model, index);
      }
      else if (key == "Path") {
        add(engine_join("sfx", file), DependencyKind::Sound, index);
      }
    });
  }

  void collect_att(const int index, const Bytes &data)
  {
    for_each_text_line(data, [&](std::string_view key, std::string_view value) {
      if (key == "Path" && !value.empty()) {
        add(engine_join("gfx", gbk_to_utf8(value)), DependencyKind::Effect, index);
      }
    });
  }

  void collect_bmd(const int index, const Bytes &data)
  {
    LitModel model;
    const LitSharedDataLoader loader = [&](const std::string &path_raw, Bytes &r_data) {
      return vfs_.read(normalize_engine_path(path_raw, true), r_data);
    };
    if (!parse_bmd(data, model, nullptr, loader)) {
      return;
    }
    for (const LitMesh &mesh : model.meshes) {
      if (!mesh.texture.empty()) {
        add(mesh.texture, DependencyKind::Texture, index);
      }
    }
    if (!model.shared_data_file.empty()) {
      add(model.shared_data_file, DependencyKind::Building, index);
    }
  }

  void collect_mox(const int index, const std::string &path, const Bytes &data)
  {
    MoxFrame root;
    if (!parse_mox(data, root, nullptr)) {
      return;
    }
    const std::string textures = engine_join(path_dirname(path), "textures");
    std::vector<const MoxFrame *> frames = {&root};
    while (!frames.empty()) {
      const MoxFrame *frame = frames.back();
      frames.pop_back();
      for (const MoxMesh &mesh : frame->meshes) {
        if (!mesh.texture.empty()) {
          add(engine_join(textures, path_basename(mesh.texture)), DependencyKind::Texture, index);
        }
      }
      for (const MoxFrame &child : frame->children) {
        frames.push_back(&child);
      }
    }
  }
};

}  // namespace

std::vector<Dependency> collect_dependencies(ClientFileSystem &vfs, const std::string_view engine_path)
{
  return Collector(vfs).run(engine_path);
}

}  // namespace blender::io::angelica
