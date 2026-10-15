/* SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup angelica
 *
 * The `_angelica` Python module: the game client's packages and files for the Studio's own
 * scripts (the client library, saving into packages, copying assets between clients).
 *
 * Clients are identified by their folder (`element`). Their file systems and packages are opened
 * once and shared, so browsing and importing do not read the package tables again.
 */

#include <Python.h>

#include <algorithm>
#include <map>
#include <memory>
#include <mutex>

#include "angelica_dds.hh"
#include "angelica_deps.hh"
#include "angelica_elements.hh"
#include "angelica_pck.hh"
#include "angelica_pck_writer.hh"
#include "angelica_skeleton.hh"
#include "angelica_skin.hh"
#include "angelica_tasks.hh"
#include "angelica_vfs.hh"
#include "IO_angelica.hh"

namespace blender {

namespace io::angelica {

/** Guards the client file systems: they are used without the GIL, and are not thread safe. */
static std::mutex clients_mutex;

static std::map<std::string, std::unique_ptr<ClientFileSystem>> &clients()
{
  static std::map<std::string, std::unique_ptr<ClientFileSystem>> map;
  return map;
}

/** Run `fn(vfs)` on the file system of a client, with the GIL released. */
template<typename Fn> static void with_client(const std::string &root, Fn &&fn)
{
  Py_BEGIN_ALLOW_THREADS;
  {
    std::lock_guard lock(clients_mutex);
    std::unique_ptr<ClientFileSystem> &vfs = clients()[root];
    if (!vfs) {
      vfs = std::make_unique<ClientFileSystem>(root);
    }
    fn(*vfs);
  }
  Py_END_ALLOW_THREADS;
}

static PyObject *py_str(const std::string &s)
{
  return PyUnicode_DecodeUTF8(s.data(), Py_ssize_t(s.size()), "surrogateescape");
}

static const char *location_kind_name(const FileLocation::Kind kind)
{
  switch (kind) {
    case FileLocation::Kind::Packed:
      return "PACKED";
    case FileLocation::Kind::Loose:
      return "LOOSE";
    case FileLocation::Kind::External:
      return "EXTERNAL";
    case FileLocation::Kind::None:
      break;
  }
  return "NONE";
}

static PyObject *location_to_dict(const FileLocation &location)
{
  PyObject *dict = PyDict_New();
  PyObject *items[][2] = {
      {PyUnicode_FromString("kind"), PyUnicode_FromString(location_kind_name(location.kind))},
      {PyUnicode_FromString("path"), py_str(location.engine_path)},
      {PyUnicode_FromString("container"), py_str(location.container)},
      {PyUnicode_FromString("mount"), py_str(location.mount)},
      {PyUnicode_FromString("entry"), py_str(location.entry)},
      {PyUnicode_FromString("size"), PyLong_FromUnsignedLongLong(location.size)},
  };
  for (auto &item : items) {
    PyDict_SetItem(dict, item[0], item[1]);
    Py_DECREF(item[0]);
    Py_DECREF(item[1]);
  }
  return dict;
}

static std::shared_ptr<PckArchive> open_package_or_raise(const std::string &pck_path)
{
  std::string error;
  std::shared_ptr<PckArchive> package;
  Py_BEGIN_ALLOW_THREADS;
  package = open_package_shared(pck_path, &error);
  Py_END_ALLOW_THREADS;
  if (!package) {
    PyErr_SetString(PyExc_OSError, error.c_str());
  }
  return package;
}

PyDoc_STRVAR(py_package_info_doc,
             ".. function:: package_info(pck_path)\n"
             "\n"
             "   Header data of a package: ``version``, ``entries`` (count), ``description``,\n"
             "   ``parts`` (file paths), ``part_sizes``, ``algorithm_id``, ``encrypted``.\n");
static PyObject *py_package_info(PyObject * /*self*/, PyObject *args)
{
  const char *pck_path;
  if (!PyArg_ParseTuple(args, "s:package_info", &pck_path)) {
    return nullptr;
  }
  std::shared_ptr<PckArchive> package = open_package_or_raise(pck_path);
  if (!package) {
    return nullptr;
  }
  PyObject *parts = PyList_New(0);
  PyObject *sizes = PyList_New(0);
  const std::vector<std::string> part_paths = package->part_paths();
  const std::vector<uint64_t> part_sizes = package->part_sizes();
  for (size_t i = 0; i < part_paths.size(); i++) {
    PyObject *path = py_str(part_paths[i]);
    PyObject *size = PyLong_FromUnsignedLongLong(part_sizes[i]);
    PyList_Append(parts, path);
    PyList_Append(sizes, size);
    Py_DECREF(path);
    Py_DECREF(size);
  }
  return Py_BuildValue("{s:I,s:n,s:N,s:N,s:N,s:L,s:O}",
                       "version",
                       unsigned(package->version()),
                       "entries",
                       Py_ssize_t(package->entries().size()),
                       "description",
                       py_str(package->description()),
                       "parts",
                       parts,
                       "part_sizes",
                       sizes,
                       "algorithm_id",
                       (long long)(package->algorithm_id()),
                       "encrypted",
                       package->is_encrypted() ? Py_True : Py_False);
}

PyDoc_STRVAR(py_package_entries_doc,
             ".. function:: package_entries(pck_path)\n"
             "\n"
             "   Every file of a package as ``(path, size, stored_size)`` tuples (path with the\n"
             "   original case and ``\\`` separators, as stored).\n");
static PyObject *py_package_entries(PyObject * /*self*/, PyObject *args)
{
  const char *pck_path;
  if (!PyArg_ParseTuple(args, "s:package_entries", &pck_path)) {
    return nullptr;
  }
  std::shared_ptr<PckArchive> package = open_package_or_raise(pck_path);
  if (!package) {
    return nullptr;
  }
  const std::vector<PckEntry> &entries = package->entries();
  PyObject *list = PyList_New(Py_ssize_t(entries.size()));
  for (size_t i = 0; i < entries.size(); i++) {
    const PckEntry &entry = entries[i];
    PyList_SET_ITEM(list,
                    Py_ssize_t(i),
                    Py_BuildValue("(NII)",
                                  py_str(entry.path_display),
                                  unsigned(entry.size),
                                  unsigned(entry.compressed_size)));
  }
  return list;
}

PyDoc_STRVAR(py_locate_doc,
             ".. function:: locate(client_root, path)\n"
             "\n"
             "   Where the client finds a file, in the client's own order (package first, then\n"
             "   the loose file): a dict with ``kind`` (``'PACKED'``, ``'LOOSE'``), ``path``,\n"
             "   ``container`` (the package or the file), ``mount``, ``entry`` and ``size``, or\n"
             "   None when the client does not have the file.\n");
static PyObject *py_locate(PyObject * /*self*/, PyObject *args)
{
  const char *root, *path;
  if (!PyArg_ParseTuple(args, "ss:locate", &root, &path)) {
    return nullptr;
  }
  FileLocation location;
  with_client(root, [&](ClientFileSystem &vfs) { location = vfs.locate(path); });
  if (location.kind == FileLocation::Kind::None) {
    Py_RETURN_NONE;
  }
  return location_to_dict(location);
}

PyDoc_STRVAR(py_read_doc,
             ".. function:: read(client_root, path)\n"
             "\n"
             "   The content of a client file as bytes (decompressed), or None when missing.\n");
static PyObject *py_read(PyObject * /*self*/, PyObject *args)
{
  const char *root, *path;
  if (!PyArg_ParseTuple(args, "ss:read", &root, &path)) {
    return nullptr;
  }
  Bytes data;
  bool ok = false;
  with_client(root, [&](ClientFileSystem &vfs) { ok = vfs.read(path, data); });
  if (!ok) {
    Py_RETURN_NONE;
  }
  return PyBytes_FromStringAndSize(reinterpret_cast<const char *>(data.data()),
                                   Py_ssize_t(data.size()));
}

PyDoc_STRVAR(py_dependencies_doc,
             ".. function:: dependencies(client_root, path)\n"
             "\n"
             "   ``path`` and every file it references, recursively, following the engine's own\n"
             "   export collector. A list of dicts with ``path``, ``kind`` (``'root'``, ``'skin'``,\n"
             "   ``'texture'``, ``'effect'``, ...), ``parent`` (index in the list, -1 for the root)\n"
             "   and ``location`` (see :func:`locate`, None for files the client does not have).\n");
static PyObject *py_dependencies(PyObject * /*self*/, PyObject *args)
{
  const char *root, *path;
  if (!PyArg_ParseTuple(args, "ss:dependencies", &root, &path)) {
    return nullptr;
  }
  std::vector<Dependency> deps;
  with_client(root, [&](ClientFileSystem &vfs) { deps = collect_dependencies(vfs, path); });
  PyObject *list = PyList_New(Py_ssize_t(deps.size()));
  for (size_t i = 0; i < deps.size(); i++) {
    const Dependency &dep = deps[i];
    PyObject *location = nullptr;
    if (dep.location.kind == FileLocation::Kind::None) {
      location = Py_None;
      Py_INCREF(location);
    }
    else {
      location = location_to_dict(dep.location);
    }
    PyList_SET_ITEM(list,
                    Py_ssize_t(i),
                    Py_BuildValue("{s:N,s:s,s:i,s:N}",
                                  "path",
                                  py_str(dep.path),
                                  "kind",
                                  dependency_kind_name(dep.kind),
                                  "parent",
                                  dep.parent,
                                  "location",
                                  location));
  }
  return list;
}

PyDoc_STRVAR(py_release_doc,
             ".. function:: release(path=None)\n"
             "\n"
             "   Forget the open client file systems and, when ``path`` names a package, that\n"
             "   package (call after a package was written so it is read again).\n");
static PyObject *py_release(PyObject * /*self*/, PyObject *args)
{
  const char *pck_path = nullptr;
  if (!PyArg_ParseTuple(args, "|z:release", &pck_path)) {
    return nullptr;
  }
  Py_BEGIN_ALLOW_THREADS;
  {
    std::lock_guard lock(clients_mutex);
    clients().clear();
  }
  if (pck_path) {
    release_package_shared(pck_path);
  }
  Py_END_ALLOW_THREADS;
  Py_RETURN_NONE;
}

PyDoc_STRVAR(py_package_read_doc,
             ".. function:: package_read(pck_path, path)\n"
             "\n"
             "   The content of a file of one package (decompressed), or None when the package\n"
             "   does not have it.\n");
static PyObject *py_package_read(PyObject * /*self*/, PyObject *args)
{
  const char *pck_path, *path;
  if (!PyArg_ParseTuple(args, "ss:package_read", &pck_path, &path)) {
    return nullptr;
  }
  std::shared_ptr<PckArchive> package = open_package_or_raise(pck_path);
  if (!package) {
    return nullptr;
  }
  Bytes data;
  bool ok = false;
  Py_BEGIN_ALLOW_THREADS;
  ok = package->read(std::string_view(path), data, nullptr);
  Py_END_ALLOW_THREADS;
  if (!ok) {
    Py_RETURN_NONE;
  }
  return PyBytes_FromStringAndSize(reinterpret_cast<const char *>(data.data()),
                                   Py_ssize_t(data.size()));
}

/** `[(path, bytes or None), ...]` as package changes (None removes the file). */
static bool changes_from_py(PyObject *changes, std::vector<PckFileChange> &r_changes)
{
  PyObject *seq = PySequence_Fast(changes, "changes must be a list of (path, bytes or None)");
  if (seq == nullptr) {
    return false;
  }
  const Py_ssize_t count = PySequence_Fast_GET_SIZE(seq);
  r_changes.resize(size_t(count));
  bool ok = true;
  for (Py_ssize_t i = 0; ok && i < count; i++) {
    const char *path;
    PyObject *data;
    ok = PyArg_ParseTuple(PySequence_Fast_GET_ITEM(seq, i), "sO", &path, &data);
    if (!ok) {
      break;
    }
    PckFileChange &change = r_changes[size_t(i)];
    change.path = path;
    if (data == Py_None) {
      change.remove = true;
      continue;
    }
    Py_buffer view;
    ok = PyObject_GetBuffer(data, &view, PyBUF_SIMPLE) == 0;
    if (ok) {
      const uint8_t *bytes = static_cast<const uint8_t *>(view.buf);
      change.data.assign(bytes, bytes + view.len);
      PyBuffer_Release(&view);
    }
  }
  Py_DECREF(seq);
  return ok;
}

/**
 * Run a package write with the GIL released. The client file systems keep their packages open,
 * so they are dropped first (they reopen on the next use).
 */
template<typename Fn> static bool run_package_write(Fn &&fn)
{
  std::string error;
  bool ok = false;
  Py_BEGIN_ALLOW_THREADS;
  {
    std::lock_guard lock(clients_mutex);
    clients().clear();
    ok = fn(&error);
  }
  Py_END_ALLOW_THREADS;
  if (!ok) {
    PyErr_SetString(PyExc_OSError, error.c_str());
  }
  return ok;
}

static PyObject *write_result_to_py(const PckWriteResult &result)
{
  return Py_BuildValue("{s:i,s:i,s:i,s:K}",
                       "written",
                       result.written,
                       "added",
                       result.added,
                       "removed",
                       result.removed,
                       "size",
                       (unsigned long long)result.size);
}

PyDoc_STRVAR(py_pck_update_doc,
             ".. function:: pck_update(pck_path, mount, changes)\n"
             "\n"
             "   Store files in the package itself: ``changes`` is a list of ``(path, data)``\n"
             "   with engine paths (``models\\npc\\x.ski``) and bytes, or None to remove the\n"
             "   file. The package stays readable by the client at every moment and can be\n"
             "   restored with :func:`pck_undo`. Returns a dict with ``written``, ``added``,\n"
             "   ``removed`` and ``size``; raises OSError (package in use, no safe header, ...).\n");
static PyObject *py_pck_update(PyObject * /*self*/, PyObject *args)
{
  const char *pck_path, *mount;
  PyObject *py_changes;
  if (!PyArg_ParseTuple(args, "ssO:pck_update", &pck_path, &mount, &py_changes)) {
    return nullptr;
  }
  std::vector<PckFileChange> changes;
  if (!changes_from_py(py_changes, changes)) {
    return nullptr;
  }
  PckWriteResult result;
  if (!run_package_write([&](std::string *r_error) {
        return pck_update_in_place(pck_path, mount, changes, &result, r_error);
      }))
  {
    return nullptr;
  }
  return write_result_to_py(result);
}

PyDoc_STRVAR(py_pck_can_undo_doc,
             ".. function:: pck_can_undo(pck_path)\n"
             "\n"
             "   Whether the last :func:`pck_update` of the package can be undone.\n");
static PyObject *py_pck_can_undo(PyObject * /*self*/, PyObject *args)
{
  const char *pck_path;
  if (!PyArg_ParseTuple(args, "s:pck_can_undo", &pck_path)) {
    return nullptr;
  }
  return PyBool_FromLong(pck_can_undo(pck_path));
}

PyDoc_STRVAR(py_pck_undo_doc,
             ".. function:: pck_undo(pck_path)\n"
             "\n"
             "   Restore the package as it was before its last :func:`pck_update`.\n");
static PyObject *py_pck_undo(PyObject * /*self*/, PyObject *args)
{
  const char *pck_path;
  if (!PyArg_ParseTuple(args, "s:pck_undo", &pck_path)) {
    return nullptr;
  }
  if (!run_package_write(
          [&](std::string *r_error) { return pck_undo_last_update(pck_path, r_error); }))
  {
    return nullptr;
  }
  Py_RETURN_NONE;
}

PyDoc_STRVAR(py_pck_write_patch_doc,
             ".. function:: pck_write_patch(like_pck, mount, dst_pck, changes)\n"
             "\n"
             "   Write a new package holding only ``changes`` (see :func:`pck_update`), in the\n"
             "   format of ``like_pck``.\n");
static PyObject *py_pck_write_patch(PyObject * /*self*/, PyObject *args)
{
  const char *like_pck, *mount, *dst_pck;
  PyObject *py_changes;
  if (!PyArg_ParseTuple(
          args, "sssO:pck_write_patch", &like_pck, &mount, &dst_pck, &py_changes))
  {
    return nullptr;
  }
  std::vector<PckFileChange> changes;
  if (!changes_from_py(py_changes, changes)) {
    return nullptr;
  }
  PckWriteResult result;
  if (!run_package_write([&](std::string *r_error) {
        return pck_write_patch(like_pck, mount, dst_pck, changes, &result, r_error);
      }))
  {
    return nullptr;
  }
  return write_result_to_py(result);
}

PyDoc_STRVAR(py_pck_write_copy_doc,
             ".. function:: pck_write_copy(src_pck, mount, dst_pck, changes)\n"
             "\n"
             "   Write a full copy of a package (all its parts) at ``dst_pck`` with ``changes``\n"
             "   applied (see :func:`pck_update`).\n");
static PyObject *py_pck_write_copy(PyObject * /*self*/, PyObject *args)
{
  const char *src_pck, *mount, *dst_pck;
  PyObject *py_changes;
  if (!PyArg_ParseTuple(args, "sssO:pck_write_copy", &src_pck, &mount, &dst_pck, &py_changes)) {
    return nullptr;
  }
  std::vector<PckFileChange> changes;
  if (!changes_from_py(py_changes, changes)) {
    return nullptr;
  }
  PckWriteResult result;
  if (!run_package_write([&](std::string *r_error) {
        return pck_write_copy(src_pck, mount, dst_pck, changes, &result, r_error);
      }))
  {
    return nullptr;
  }
  return write_result_to_py(result);
}

PyDoc_STRVAR(py_compress_blocks_doc,
             ".. function:: compress_blocks(format, width, height, rgba)\n"
             "\n"
             "   Compress RGBA pixels (8 bits per channel, rows from the top) into the blocks of\n"
             "   a DDS texture level: ``format`` is ``'DXT1'``, ``'DXT3'`` or ``'DXT5'``.\n");
static PyObject *py_compress_blocks(PyObject * /*self*/, PyObject *args)
{
  const char *format_name;
  int width, height;
  Py_buffer view;
  if (!PyArg_ParseTuple(args, "siiy*:compress_blocks", &format_name, &width, &height, &view)) {
    return nullptr;
  }
  const std::string name = format_name;
  const BlockFormat format = name == "DXT1" ? BlockFormat::DXT1 :
                             name == "DXT3" ? BlockFormat::DXT3 :
                                              BlockFormat::DXT5;
  if ((name != "DXT1" && name != "DXT3" && name != "DXT5") || width <= 0 || height <= 0 ||
      view.len != Py_ssize_t(width) * Py_ssize_t(height) * 4)
  {
    PyBuffer_Release(&view);
    PyErr_SetString(PyExc_ValueError,
                    "compress_blocks: format must be DXT1, DXT3 or DXT5 and rgba must hold "
                    "width * height * 4 bytes");
    return nullptr;
  }
  Bytes blocks;
  Py_BEGIN_ALLOW_THREADS;
  compress_blocks(format, width, height, static_cast<const uint8_t *>(view.buf), blocks);
  Py_END_ALLOW_THREADS;
  PyBuffer_Release(&view);
  return PyBytes_FromStringAndSize(reinterpret_cast<const char *>(blocks.data()),
                                   Py_ssize_t(blocks.size()));
}

static const char *skin_mesh_type_name(const SkinMeshType type)
{
  switch (type) {
    case SkinMeshType::Skin:
      return "SKIN";
    case SkinMeshType::Rigid:
      return "RIGID";
    case SkinMeshType::MorphSkin:
      return "MORPH_SKIN";
    case SkinMeshType::MorphRigid:
      return "MORPH_RIGID";
    case SkinMeshType::Supple:
      return "SUPPLE";
    case SkinMeshType::Muscle:
      return "MUSCLE";
  }
  return "";
}

PyDoc_STRVAR(py_ski_layout_doc,
             ".. function:: ski_layout(data)\n"
             "\n"
             "   The structure of a skin file (``.ski`` bytes), for writing changed meshes back:\n"
             "   ``version``, ``header_offset`` (after the MOXB tag), ``bone_names`` and their\n"
             "   byte range ``bone_names_range``, ``textures``, and ``meshes``: dicts with ``type``\n"
             "   (``'SKIN'``, ``'RIGID'``, ``'MORPH_SKIN'``, ...), ``name``, ``range`` (bytes of the\n"
             "   record), ``texture``, ``material``, ``bone``, ``vertices`` and ``indices``.\n"
             "   Raises ValueError for data that is not a skin file.\n");
static PyObject *py_ski_layout(PyObject * /*self*/, PyObject *args)
{
  Py_buffer view;
  if (!PyArg_ParseTuple(args, "y*:ski_layout", &view)) {
    return nullptr;
  }
  Skin skin;
  std::string error;
  bool ok = false;
  Py_BEGIN_ALLOW_THREADS;
  ok = parse_skin(ByteSpan(static_cast<const uint8_t *>(view.buf), size_t(view.len)), skin, &error);
  Py_END_ALLOW_THREADS;
  PyBuffer_Release(&view);
  if (!ok) {
    PyErr_SetString(PyExc_ValueError, error.c_str());
    return nullptr;
  }
  PyObject *bone_names = PyList_New(Py_ssize_t(skin.bone_names.size()));
  for (size_t i = 0; i < skin.bone_names.size(); i++) {
    PyList_SET_ITEM(bone_names, Py_ssize_t(i), py_str(skin.bone_names[i]));
  }
  PyObject *textures = PyList_New(Py_ssize_t(skin.textures.size()));
  for (size_t i = 0; i < skin.textures.size(); i++) {
    PyList_SET_ITEM(textures, Py_ssize_t(i), py_str(skin.textures[i]));
  }
  PyObject *meshes = PyList_New(Py_ssize_t(skin.meshes.size()));
  for (size_t i = 0; i < skin.meshes.size(); i++) {
    const SkinMesh &mesh = skin.meshes[i];
    PyList_SET_ITEM(meshes,
                    Py_ssize_t(i),
                    Py_BuildValue("{s:s,s:N,s:(nn),s:i,s:i,s:i,s:i,s:n}",
                                  "type",
                                  skin_mesh_type_name(mesh.type),
                                  "name",
                                  py_str(mesh.name),
                                  "range",
                                  Py_ssize_t(mesh.record_begin),
                                  Py_ssize_t(mesh.record_end),
                                  "texture",
                                  mesh.texture_index,
                                  "material",
                                  mesh.material_index,
                                  "bone",
                                  mesh.bone_index,
                                  "vertices",
                                  mesh.vertex_count(),
                                  "indices",
                                  Py_ssize_t(mesh.indices.size())));
  }
  return Py_BuildValue("{s:I,s:O,s:n,s:N,s:(nn),s:N,s:N,s:i}",
                       "version",
                       unsigned(skin.version),
                       "is_text",
                       skin.is_text ? Py_True : Py_False,
                       "header_offset",
                       Py_ssize_t(skin.header_offset),
                       "bone_names",
                       bone_names,
                       "bone_names_range",
                       Py_ssize_t(skin.bone_names_begin),
                       Py_ssize_t(skin.bone_names_end),
                       "textures",
                       textures,
                       "meshes",
                       meshes,
                       "skeleton_bone_count",
                       skin.skeleton_bone_count);
}

static PyObject *floats_to_py(const float *values, const size_t count)
{
  return PyBytes_FromStringAndSize(reinterpret_cast<const char *>(values),
                                   Py_ssize_t(count * sizeof(float)));
}

PyDoc_STRVAR(py_bon_layout_doc,
             ".. function:: bon_layout(data)\n"
             "\n"
             "   The bones of a skeleton file (``.bon`` bytes): ``version`` and ``bones``, dicts\n"
             "   with ``name``, ``parent``, ``first_joint``, ``flags``, ``relative`` and\n"
             "   ``bone_init`` (16 float32 as bytes, row-major engine matrices). Raises ValueError\n"
             "   for data that is not a skeleton.\n");
static PyObject *py_bon_layout(PyObject * /*self*/, PyObject *args)
{
  Py_buffer view;
  if (!PyArg_ParseTuple(args, "y*:bon_layout", &view)) {
    return nullptr;
  }
  Skeleton skeleton;
  std::string error;
  bool ok = false;
  Py_BEGIN_ALLOW_THREADS;
  ok = parse_skeleton(
      ByteSpan(static_cast<const uint8_t *>(view.buf), size_t(view.len)), skeleton, &error);
  Py_END_ALLOW_THREADS;
  PyBuffer_Release(&view);
  if (!ok) {
    PyErr_SetString(PyExc_ValueError, error.c_str());
    return nullptr;
  }
  PyObject *bones = PyList_New(Py_ssize_t(skeleton.bones.size()));
  for (size_t i = 0; i < skeleton.bones.size(); i++) {
    const SkeletonBone &bone = skeleton.bones[i];
    PyList_SET_ITEM(bones,
                    Py_ssize_t(i),
                    Py_BuildValue("{s:N,s:i,s:i,s:i,s:N,s:N,s:n}",
                                  "name",
                                  py_str(bone.name),
                                  "parent",
                                  bone.parent,
                                  "first_joint",
                                  bone.first_joint,
                                  "flags",
                                  int(bone.flags),
                                  "relative",
                                  floats_to_py(bone.relative, 16),
                                  "bone_init",
                                  floats_to_py(bone.bone_init, 16),
                                  "matrices_offset",
                                  Py_ssize_t(bone.matrices_offset)));
  }
  return Py_BuildValue("{s:I,s:N}", "version", unsigned(skeleton.version), "bones", bones);
}

static PyObject *key_track_to_py(const KeyTrack &track)
{
  PyObject *segments = PyList_New(Py_ssize_t(track.segments.size()));
  for (size_t i = 0; i < track.segments.size(); i++) {
    const TrackSegment &seg = track.segments[i];
    PyList_SET_ITEM(segments,
                    Py_ssize_t(i),
                    Py_BuildValue("(iiii)",
                                  seg.start_time_ms,
                                  seg.end_time_ms,
                                  seg.start_key,
                                  seg.end_key));
  }
  return Py_BuildValue("{s:i,s:i,s:N,s:N,s:i,s:N,s:i,s:i,s:O,s:(nn)}",
                       "components",
                       track.components,
                       "frame_rate",
                       track.frame_rate,
                       "keys",
                       floats_to_py(track.keys.data(), track.keys.size()),
                       "key_frames",
                       floats_to_py(track.key_frames.data(), track.key_frames.size()),
                       "stored_key_count",
                       track.stored_key_count,
                       "segments",
                       segments,
                       "track_length_ms",
                       track.track_length_ms,
                       "compression",
                       int(track.compression),
                       "quaternion_without_w",
                       track.quaternion_without_w ? Py_True : Py_False,
                       "range",
                       Py_ssize_t(track.record_begin),
                       Py_ssize_t(track.record_end));
}

PyDoc_STRVAR(py_stck_decode_doc,
             ".. function:: stck_decode(data)\n"
             "\n"
             "   The bone tracks of a track set (``.stck`` bytes): ``version``, ``anim_start``,\n"
             "   ``anim_end``, ``anim_fps``, ``header_offset`` (after the MOXB tag) and ``tracks``:\n"
             "   dicts with ``id``, ``position`` and ``rotation``. Each track has ``frame_rate``,\n"
             "   ``keys`` and ``key_frames`` (float32 bytes, keys decoded), the stored layout\n"
             "   (``stored_key_count``, ``segments``, ``track_length_ms``, ``compression``) and\n"
             "   ``quaternion_without_w``. Raises ValueError for data that is not a track set.\n");
static PyObject *py_stck_decode(PyObject * /*self*/, PyObject *args)
{
  Py_buffer view;
  if (!PyArg_ParseTuple(args, "y*:stck_decode", &view)) {
    return nullptr;
  }
  const ByteSpan data(static_cast<const uint8_t *>(view.buf), size_t(view.len));
  TrackSet tracks;
  std::string error;
  bool ok = false;
  size_t header_offset = 0;
  Py_BEGIN_ALLOW_THREADS;
  ok = parse_track_set(data, tracks, &error);
  header_offset = size_t(afile_open(data).body.data() - data.data());
  Py_END_ALLOW_THREADS;
  PyBuffer_Release(&view);
  if (!ok) {
    PyErr_SetString(PyExc_ValueError, error.c_str());
    return nullptr;
  }
  PyObject *list = PyList_New(Py_ssize_t(tracks.tracks.size()));
  for (size_t i = 0; i < tracks.tracks.size(); i++) {
    const BoneTrack &track = tracks.tracks[i];
    PyList_SET_ITEM(list,
                    Py_ssize_t(i),
                    Py_BuildValue("{s:i,s:N,s:N}",
                                  "id",
                                  track.id,
                                  "position",
                                  key_track_to_py(track.position),
                                  "rotation",
                                  key_track_to_py(track.rotation)));
  }
  return Py_BuildValue("{s:I,s:i,s:i,s:i,s:n,s:N}",
                       "version",
                       unsigned(tracks.version),
                       "anim_start",
                       tracks.anim_start,
                       "anim_end",
                       tracks.anim_end,
                       "anim_fps",
                       tracks.anim_fps,
                       "header_offset",
                       Py_ssize_t(header_offset),
                       "tracks",
                       list);
}

/* -------------------------------------------------------------------- */
/** \name elements.data
 *
 * Open files are kept by handle (an integer) until #py_elements_close.
 * \{ */

static std::map<long, std::unique_ptr<ElementsData>> &open_elements()
{
  static std::map<long, std::unique_ptr<ElementsData>> map;
  return map;
}

static ElementsData *elements_or_raise(const long handle)
{
  auto it = open_elements().find(handle);
  if (it == open_elements().end()) {
    PyErr_SetString(PyExc_ValueError, "elements: invalid handle");
    return nullptr;
  }
  return it->second.get();
}

static bool list_or_raise(const ElementsData &data, const int list)
{
  if (list < 0 || list >= int(data.lists.size())) {
    PyErr_SetString(PyExc_IndexError, "elements: list index out of range");
    return false;
  }
  return true;
}

static const char *field_type_name(const ElementFieldType type)
{
  switch (type) {
    case ElementFieldType::Int16:
      return "int16";
    case ElementFieldType::Int32:
      return "int32";
    case ElementFieldType::UInt32:
      return "uint32";
    case ElementFieldType::Int64:
      return "int64";
    case ElementFieldType::Float:
      return "float";
    case ElementFieldType::Double:
      return "double";
    case ElementFieldType::Raw:
      return "raw";
    case ElementFieldType::WString:
      return "wstring";
    case ElementFieldType::String:
      return "string";
  }
  return "";
}

static PyObject *element_value_to_py(const ElementValue &value)
{
  if (const int64_t *i = std::get_if<int64_t>(&value)) {
    return PyLong_FromLongLong(*i);
  }
  if (const double *d = std::get_if<double>(&value)) {
    return PyFloat_FromDouble(*d);
  }
  if (const std::string *text = std::get_if<std::string>(&value)) {
    return py_str(*text);
  }
  const Bytes &bytes = std::get<Bytes>(value);
  return PyBytes_FromStringAndSize(reinterpret_cast<const char *>(bytes.data()),
                                   Py_ssize_t(bytes.size()));
}

PyDoc_STRVAR(py_decompress_blocks_doc,
             ".. function:: decompress_blocks(format, width, height, blocks)\n"
             "\n"
             "   Decode DDS blocks (``'DXT1'``, ``'DXT3'`` or ``'DXT5'``) of a ``width`` x ``height``\n"
             "   image into RGBA bytes (rows from the top).\n");
static PyObject *py_decompress_blocks(PyObject * /*self*/, PyObject *args)
{
  const char *format_name;
  int width, height;
  Py_buffer view;
  if (!PyArg_ParseTuple(args, "siiy*:decompress_blocks", &format_name, &width, &height, &view)) {
    return nullptr;
  }
  const std::string name = format_name;
  const BlockFormat format = name == "DXT1" ? BlockFormat::DXT1 :
                             name == "DXT3" ? BlockFormat::DXT3 :
                                              BlockFormat::DXT5;
  Bytes rgba;
  bool ok = name == "DXT1" || name == "DXT3" || name == "DXT5";
  if (ok) {
    Py_BEGIN_ALLOW_THREADS;
    ok = decompress_blocks(
        format, width, height, ByteSpan(static_cast<const uint8_t *>(view.buf), size_t(view.len)), rgba);
    Py_END_ALLOW_THREADS;
  }
  PyBuffer_Release(&view);
  if (!ok) {
    PyErr_SetString(PyExc_ValueError,
                    "decompress_blocks: format must be DXT1, DXT3 or DXT5 with enough blocks");
    return nullptr;
  }
  return PyBytes_FromStringAndSize(reinterpret_cast<const char *>(rgba.data()),
                                   Py_ssize_t(rgba.size()));
}

PyDoc_STRVAR(py_schemas_directory_doc,
             ".. function:: schemas_directory(kind)\n"
             "\n"
             "   Folder of the downloaded data layouts of one kind (``'elements'``, ...), empty\n"
             "   when the schemas plugin is not installed.\n");
static PyObject *py_schemas_directory(PyObject * /*self*/, PyObject *args)
{
  const char *kind;
  if (!PyArg_ParseTuple(args, "s:schemas_directory", &kind)) {
    return nullptr;
  }
  return py_str(schemas_directory(kind));
}

PyDoc_STRVAR(py_elements_open_doc,
             ".. function:: elements_open(data, schema_folder='')\n"
             "\n"
             "   Parse ``elements.data`` bytes with the schema of their version (from\n"
             "   ``schema_folder``, by default the downloaded schemas). Returns a handle for the\n"
             "   other ``elements_*`` functions; raises ValueError.\n");
static PyObject *py_elements_open(PyObject * /*self*/, PyObject *args)
{
  Py_buffer view;
  const char *folder = "";
  if (!PyArg_ParseTuple(args, "y*|s:elements_open", &view, &folder)) {
    return nullptr;
  }
  const ByteSpan bytes(static_cast<const uint8_t *>(view.buf), size_t(view.len));
  std::string error;
  auto data = std::make_unique<ElementsData>();
  bool ok = false;
  Py_BEGIN_ALLOW_THREADS;
  uint32_t version = 0;
  const std::string dir = folder[0] ? std::string(folder) : schemas_directory("elements");
  auto schema = std::make_shared<ElementsSchema>();
  if (dir.empty()) {
    error = "The elements.data layouts are not installed (sign in to download them)";
  }
  else if (peek_elements_version(bytes, version, &error) &&
           load_elements_schema_for_version(dir, version, *schema, &error))
  {
    ok = parse_elements(bytes, *schema, *data, &error);
  }
  Py_END_ALLOW_THREADS;
  PyBuffer_Release(&view);
  if (!ok) {
    PyErr_SetString(PyExc_ValueError, error.c_str());
    return nullptr;
  }
  static long next_handle = 1;
  const long handle = next_handle++;
  open_elements()[handle] = std::move(data);
  return PyLong_FromLong(handle);
}

PyDoc_STRVAR(py_elements_info_doc,
             ".. function:: elements_info(handle)\n"
             "\n"
             "   ``version`` and ``lists``: dicts with ``name``, ``label``, ``id_space``, ``count``\n"
             "   and ``fields`` (``(name, type, size)`` tuples).\n");
static PyObject *py_elements_info(PyObject * /*self*/, PyObject *args)
{
  long handle;
  if (!PyArg_ParseTuple(args, "l:elements_info", &handle)) {
    return nullptr;
  }
  const ElementsData *data = elements_or_raise(handle);
  if (!data) {
    return nullptr;
  }
  static const char *spaces[] = {"ESSENCE", "ADDON", "TALK", "FACE", "RECIPE", "CONFIG"};
  PyObject *lists = PyList_New(Py_ssize_t(data->lists.size()));
  for (size_t i = 0; i < data->lists.size(); i++) {
    const ElementListDef *def = data->list_def(int(i));
    PyObject *fields = PyList_New(Py_ssize_t(def->fields.size()));
    for (size_t f = 0; f < def->fields.size(); f++) {
      const ElementFieldDef &field = def->fields[f];
      PyList_SET_ITEM(fields,
                      Py_ssize_t(f),
                      Py_BuildValue("(NsI)",
                                    py_str(field.name),
                                    field_type_name(field.type),
                                    unsigned(field.size)));
    }
    PyList_SET_ITEM(lists,
                    Py_ssize_t(i),
                    Py_BuildValue("{s:N,s:N,s:s,s:n,s:N}",
                                  "name",
                                  py_str(def->name),
                                  "label",
                                  py_str(def->label),
                                  "id_space",
                                  spaces[int(def->id_space)],
                                  "count",
                                  Py_ssize_t(data->lists[i].size()),
                                  "fields",
                                  fields));
  }
  return Py_BuildValue("{s:I,s:N}", "version", unsigned(data->version()), "lists", lists);
}

PyDoc_STRVAR(py_elements_record_doc,
             ".. function:: elements_record(handle, list, index)\n"
             "\n"
             "   The field values of a record (int, float, str or bytes, in field order).\n");
static PyObject *py_elements_record(PyObject * /*self*/, PyObject *args)
{
  long handle;
  int list;
  Py_ssize_t index;
  if (!PyArg_ParseTuple(args, "lin:elements_record", &handle, &list, &index)) {
    return nullptr;
  }
  const ElementsData *data = elements_or_raise(handle);
  if (!data || !list_or_raise(*data, list)) {
    return nullptr;
  }
  std::vector<ElementValue> values;
  if (index < 0 || !data->decode_record(list, size_t(index), values)) {
    PyErr_SetString(PyExc_IndexError, "elements: record index out of range");
    return nullptr;
  }
  PyObject *result = PyList_New(Py_ssize_t(values.size()));
  for (size_t i = 0; i < values.size(); i++) {
    PyList_SET_ITEM(result, Py_ssize_t(i), element_value_to_py(values[i]));
  }
  return result;
}

PyDoc_STRVAR(py_elements_find_doc,
             ".. function:: elements_find(handle, list, id)\n"
             "\n"
             "   Index of the first record of a list with this id, -1 when there is none.\n");
static PyObject *py_elements_find(PyObject * /*self*/, PyObject *args)
{
  long handle;
  int list;
  unsigned long id;
  if (!PyArg_ParseTuple(args, "lik:elements_find", &handle, &list, &id)) {
    return nullptr;
  }
  const ElementsData *data = elements_or_raise(handle);
  if (!data || !list_or_raise(*data, list)) {
    return nullptr;
  }
  const ElementRef ref = data->find(list, uint32_t(id));
  return PyLong_FromLongLong(ref.valid() ? (long long)ref.record : -1);
}

PyDoc_STRVAR(py_elements_set_doc,
             ".. function:: elements_set(handle, list, index, field, value)\n"
             "\n"
             "   Set a field (by index) of a record: int, float, str or bytes as the field takes.\n");
static PyObject *py_elements_set(PyObject * /*self*/, PyObject *args)
{
  long handle;
  int list, field;
  Py_ssize_t index;
  PyObject *py_value;
  if (!PyArg_ParseTuple(args, "liniO:elements_set", &handle, &list, &index, &field, &py_value)) {
    return nullptr;
  }
  ElementsData *data = elements_or_raise(handle);
  if (!data || !list_or_raise(*data, list)) {
    return nullptr;
  }
  ElementValue value;
  if (PyBool_Check(py_value) || PyLong_Check(py_value)) {
    value = int64_t(PyLong_AsLongLong(py_value));
  }
  else if (PyFloat_Check(py_value)) {
    value = PyFloat_AsDouble(py_value);
  }
  else if (PyUnicode_Check(py_value)) {
    Py_ssize_t len;
    const char *utf8 = PyUnicode_AsUTF8AndSize(py_value, &len);
    if (!utf8) {
      return nullptr;
    }
    value = std::string(utf8, size_t(len));
  }
  else if (PyBytes_Check(py_value)) {
    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(PyBytes_AS_STRING(py_value));
    value = Bytes(bytes, bytes + PyBytes_GET_SIZE(py_value));
  }
  else {
    PyErr_SetString(PyExc_TypeError, "elements_set: value must be int, float, str or bytes");
    return nullptr;
  }
  if (PyErr_Occurred()) {
    return nullptr;
  }
  std::string error;
  if (index < 0 || !data->set(list, size_t(index), field, value, &error)) {
    PyErr_SetString(PyExc_ValueError, error.empty() ? "elements_set: invalid record" : error.c_str());
    return nullptr;
  }
  Py_RETURN_NONE;
}

PyDoc_STRVAR(py_elements_add_doc,
             ".. function:: elements_add(handle, list, id, copy_from=-1)\n"
             "\n"
             "   Append a record (zero filled, or a copy of record ``copy_from`` of the same list)\n"
             "   with its id set to ``id``. Returns the new record index.\n");
static PyObject *py_elements_add(PyObject * /*self*/, PyObject *args)
{
  long handle;
  int list;
  unsigned long id;
  Py_ssize_t copy_from = -1;
  if (!PyArg_ParseTuple(args, "lik|n:elements_add", &handle, &list, &id, &copy_from)) {
    return nullptr;
  }
  ElementsData *data = elements_or_raise(handle);
  if (!data || !list_or_raise(*data, list)) {
    return nullptr;
  }
  const int64_t index = data->add_record(
      list,
      uint32_t(id),
      copy_from >= 0 ? std::optional<size_t>(size_t(copy_from)) : std::nullopt);
  if (index < 0) {
    PyErr_SetString(PyExc_ValueError, "elements_add: the record cannot be added to this list");
    return nullptr;
  }
  return PyLong_FromLongLong(index);
}

PyDoc_STRVAR(py_elements_max_id_doc,
             ".. function:: elements_max_id(handle, list)\n"
             "\n"
             "   Largest id used in a list (0 when empty).\n");
static PyObject *py_elements_max_id(PyObject * /*self*/, PyObject *args)
{
  long handle;
  int list;
  if (!PyArg_ParseTuple(args, "li:elements_max_id", &handle, &list)) {
    return nullptr;
  }
  const ElementsData *data = elements_or_raise(handle);
  if (!data || !list_or_raise(*data, list)) {
    return nullptr;
  }
  return PyLong_FromUnsignedLong(data->max_id(list));
}

PyDoc_STRVAR(py_elements_bytes_doc,
             ".. function:: elements_bytes(handle)\n"
             "\n"
             "   The file bytes of an open ``elements.data`` (unchanged files are byte identical).\n");
static PyObject *py_elements_bytes(PyObject * /*self*/, PyObject *args)
{
  long handle;
  if (!PyArg_ParseTuple(args, "l:elements_bytes", &handle)) {
    return nullptr;
  }
  const ElementsData *data = elements_or_raise(handle);
  if (!data) {
    return nullptr;
  }
  Bytes out;
  std::string error;
  bool ok = false;
  Py_BEGIN_ALLOW_THREADS;
  ok = write_elements(*data, *data->schema, out, &error);
  Py_END_ALLOW_THREADS;
  if (!ok) {
    PyErr_SetString(PyExc_ValueError, error.c_str());
    return nullptr;
  }
  return PyBytes_FromStringAndSize(reinterpret_cast<const char *>(out.data()),
                                   Py_ssize_t(out.size()));
}

PyDoc_STRVAR(py_elements_ids_doc,
             ".. function:: elements_ids(handle, list)\n"
             "\n"
             "   The id of every record of a list, in record order.\n");
static PyObject *py_elements_ids(PyObject * /*self*/, PyObject *args)
{
  long handle;
  int list;
  if (!PyArg_ParseTuple(args, "li:elements_ids", &handle, &list)) {
    return nullptr;
  }
  const ElementsData *data = elements_or_raise(handle);
  if (!data || !list_or_raise(*data, list)) {
    return nullptr;
  }
  const size_t count = data->lists[size_t(list)].size();
  PyObject *ids = PyList_New(Py_ssize_t(count));
  for (size_t i = 0; i < count; i++) {
    PyList_SET_ITEM(ids, Py_ssize_t(i), PyLong_FromUnsignedLong(data->record_id(list, i)));
  }
  return ids;
}

PyDoc_STRVAR(py_elements_find_in_space_doc,
             ".. function:: elements_find_in_space(handle, space, id)\n"
             "\n"
             "   ``(list, index)`` of the first record with this id in an id space (``'ESSENCE'``,\n"
             "   ``'ADDON'``, ...), or None.\n");
static PyObject *py_elements_find_in_space(PyObject * /*self*/, PyObject *args)
{
  long handle;
  const char *space_name;
  unsigned long id;
  if (!PyArg_ParseTuple(args, "lsk:elements_find_in_space", &handle, &space_name, &id)) {
    return nullptr;
  }
  const ElementsData *data = elements_or_raise(handle);
  if (!data) {
    return nullptr;
  }
  static const char *spaces[] = {"ESSENCE", "ADDON", "TALK", "FACE", "RECIPE", "CONFIG"};
  int space = -1;
  for (int i = 0; i < 6; i++) {
    if (std::string(space_name) == spaces[i]) {
      space = i;
    }
  }
  if (space < 0) {
    PyErr_SetString(PyExc_ValueError, "elements_find_in_space: unknown id space");
    return nullptr;
  }
  const ElementRef ref = data->find_in_space(ElementIdSpace(space), uint32_t(id));
  if (!ref.valid()) {
    Py_RETURN_NONE;
  }
  return Py_BuildValue("(in)", ref.list, Py_ssize_t(ref.record));
}

static PyObject *py_elements_close(PyObject * /*self*/, PyObject *args)
{
  long handle;
  if (!PyArg_ParseTuple(args, "l:elements_close", &handle)) {
    return nullptr;
  }
  open_elements().erase(handle);
  Py_RETURN_NONE;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name tasks.data
 *
 * Open packs are kept by handle until #py_tasks_close. The field index of a top level task is
 * built when first asked for and kept up to date by the edits.
 * \{ */

struct OpenTasks {
  TasksData data;
  std::map<size_t, TaskParse> parses;
};

static std::map<long, std::unique_ptr<OpenTasks>> &open_tasks()
{
  static std::map<long, std::unique_ptr<OpenTasks>> map;
  return map;
}

static OpenTasks *tasks_or_raise(const long handle)
{
  auto it = open_tasks().find(handle);
  if (it == open_tasks().end()) {
    PyErr_SetString(PyExc_ValueError, "tasks: invalid handle");
    return nullptr;
  }
  return it->second.get();
}

/** The field index of top level task `index`, or null with a Python error set. */
static TaskParse *task_parse_or_raise(OpenTasks &tasks, const Py_ssize_t index)
{
  if (index < 0 || size_t(index) >= tasks.data.tasks.size()) {
    PyErr_SetString(PyExc_IndexError, "tasks: task index out of range");
    return nullptr;
  }
  auto it = tasks.parses.find(size_t(index));
  if (it != tasks.parses.end()) {
    return &it->second;
  }
  TaskParse parse;
  std::string error;
  if (!parse_task_record(tasks.data.tasks[size_t(index)], tasks.data.version, parse, &error)) {
    PyErr_SetString(PyExc_ValueError, error.c_str());
    return nullptr;
  }
  return &(tasks.parses[size_t(index)] = std::move(parse));
}

static const char *task_field_type_name(const TaskFieldType type)
{
  switch (type) {
    case TaskFieldType::Struct:
      return "struct";
    case TaskFieldType::Array:
      return "array";
    case TaskFieldType::Item:
      return "item";
    case TaskFieldType::Bool:
      return "bool";
    case TaskFieldType::U8:
      return "u8";
    case TaskFieldType::I16:
      return "i16";
    case TaskFieldType::I32:
      return "i32";
    case TaskFieldType::U32:
      return "u32";
    case TaskFieldType::F32:
      return "f32";
    case TaskFieldType::Bytes:
      return "bytes";
    case TaskFieldType::WCharsXor:
    case TaskFieldType::WChars:
    case TaskFieldType::Chars:
    case TaskFieldType::PwStrXor:
      return "text";
  }
  return "";
}

PyDoc_STRVAR(py_tasks_open_doc,
             ".. function:: tasks_open(data)\n"
             "\n"
             "   Parse ``tasks.data`` bytes. Returns a handle for the other ``tasks_*`` functions;\n"
             "   raises ValueError (also for packs split into ``tasks.data1..N``).\n");
static PyObject *py_tasks_open(PyObject * /*self*/, PyObject *args)
{
  Py_buffer view;
  if (!PyArg_ParseTuple(args, "y*:tasks_open", &view)) {
    return nullptr;
  }
  const ByteSpan bytes(static_cast<const uint8_t *>(view.buf), size_t(view.len));
  auto tasks = std::make_unique<OpenTasks>();
  std::string error;
  bool ok = false;
  Py_BEGIN_ALLOW_THREADS;
  if (is_task_pack_index(bytes)) {
    error = "tasks.data is split into tasks.data1..N, which is not supported yet";
  }
  else {
    ok = parse_tasks(bytes, tasks->data, &error);
  }
  Py_END_ALLOW_THREADS;
  PyBuffer_Release(&view);
  if (!ok) {
    PyErr_SetString(PyExc_ValueError, error.c_str());
    return nullptr;
  }
  static long next_handle = 1;
  const long handle = next_handle++;
  open_tasks()[handle] = std::move(tasks);
  return PyLong_FromLong(handle);
}

PyDoc_STRVAR(py_tasks_info_doc,
             ".. function:: tasks_info(handle)\n"
             "\n"
             "   ``version``, ``count`` (top level tasks) and ``sequential`` (False when saving\n"
             "   lays the records out in another order than the source file).\n");
static PyObject *py_tasks_info(PyObject * /*self*/, PyObject *args)
{
  long handle;
  if (!PyArg_ParseTuple(args, "l:tasks_info", &handle)) {
    return nullptr;
  }
  const OpenTasks *tasks = tasks_or_raise(handle);
  if (!tasks) {
    return nullptr;
  }
  return Py_BuildValue("{s:k,s:n,s:O}",
                       "version",
                       (unsigned long)tasks->data.version,
                       "count",
                       Py_ssize_t(tasks->data.tasks.size()),
                       "sequential",
                       tasks->data.sequential_layout ? Py_True : Py_False);
}

PyDoc_STRVAR(py_tasks_list_doc,
             ".. function:: tasks_list(handle)\n"
             "\n"
             "   ``(id, name)`` of every top level task, in record order.\n");
static PyObject *py_tasks_list(PyObject * /*self*/, PyObject *args)
{
  long handle;
  if (!PyArg_ParseTuple(args, "l:tasks_list", &handle)) {
    return nullptr;
  }
  const OpenTasks *tasks = tasks_or_raise(handle);
  if (!tasks) {
    return nullptr;
  }
  const size_t count = tasks->data.tasks.size();
  std::vector<std::pair<uint32_t, std::string>> rows(count);
  Py_BEGIN_ALLOW_THREADS;
  for (size_t i = 0; i < count; i++) {
    const Bytes &blob = tasks->data.tasks[i];
    rows[i] = {task_record_id(blob, tasks->data.version), task_record_name(blob, tasks->data.version)};
  }
  Py_END_ALLOW_THREADS;
  PyObject *result = PyList_New(Py_ssize_t(count));
  for (size_t i = 0; i < count; i++) {
    PyList_SET_ITEM(result,
                    Py_ssize_t(i),
                    Py_BuildValue("(kN)", (unsigned long)rows[i].first, py_str(rows[i].second)));
  }
  return result;
}

PyDoc_STRVAR(py_tasks_nodes_doc,
             ".. function:: tasks_nodes(handle, index)\n"
             "\n"
             "   The task and sub tasks of top level task ``index``, depth first: ``(field, parent,\n"
             "   id, name)`` with ``field`` the index of the task's struct in :func:`tasks_fields` and\n"
             "   ``parent`` the index of the parent node (-1 for the top level task).\n");
static PyObject *py_tasks_nodes(PyObject * /*self*/, PyObject *args)
{
  long handle;
  Py_ssize_t index;
  if (!PyArg_ParseTuple(args, "ln:tasks_nodes", &handle, &index)) {
    return nullptr;
  }
  OpenTasks *tasks = tasks_or_raise(handle);
  const TaskParse *parse = tasks ? task_parse_or_raise(*tasks, index) : nullptr;
  if (!parse) {
    return nullptr;
  }
  PyObject *result = PyList_New(Py_ssize_t(parse->nodes.size()));
  for (size_t i = 0; i < parse->nodes.size(); i++) {
    const TaskNodeInfo &node = parse->nodes[i];
    PyList_SET_ITEM(result,
                    Py_ssize_t(i),
                    Py_BuildValue("(iikN)", node.field, node.parent, (unsigned long)node.id, py_str(node.name)));
  }
  return result;
}

PyDoc_STRVAR(py_tasks_fields_doc,
             ".. function:: tasks_fields(handle, index)\n"
             "\n"
             "   Every field of top level task ``index`` (sub tasks included): ``(name, path, type,\n"
             "   parent, value)``. ``type`` is ``struct``, ``array``, ``item``, ``bool``, ``u8``,\n"
             "   ``i16``, ``i32``, ``u32``, ``f32``, ``text`` or ``bytes``; ``value`` is an int, a\n"
             "   float, a str or None (containers, bytes).\n");
static PyObject *py_tasks_fields(PyObject * /*self*/, PyObject *args)
{
  long handle;
  Py_ssize_t index;
  if (!PyArg_ParseTuple(args, "ln:tasks_fields", &handle, &index)) {
    return nullptr;
  }
  OpenTasks *tasks = tasks_or_raise(handle);
  const TaskParse *parse = tasks ? task_parse_or_raise(*tasks, index) : nullptr;
  if (!parse) {
    return nullptr;
  }
  const ByteSpan blob = tasks->data.tasks[size_t(index)];
  PyObject *result = PyList_New(Py_ssize_t(parse->fields.size()));
  for (size_t i = 0; i < parse->fields.size(); i++) {
    const TaskField &field = parse->fields[i];
    PyObject *value;
    switch (field.type) {
      case TaskFieldType::Bool:
      case TaskFieldType::U8:
      case TaskFieldType::I16:
      case TaskFieldType::I32:
      case TaskFieldType::U32:
        value = PyLong_FromLongLong(task_get_int(blob, *parse, int32_t(i)));
        break;
      case TaskFieldType::F32:
        value = PyFloat_FromDouble(task_get_float(blob, *parse, int32_t(i)));
        break;
      case TaskFieldType::WCharsXor:
      case TaskFieldType::WChars:
      case TaskFieldType::Chars:
      case TaskFieldType::PwStrXor:
        value = py_str(task_get_text(blob, *parse, int32_t(i)));
        break;
      default:
        value = Py_NewRef(Py_None);
        break;
    }
    PyList_SET_ITEM(result,
                    Py_ssize_t(i),
                    Py_BuildValue("(sNsiN)",
                                  field.name,
                                  py_str(task_field_path(*parse, int32_t(i))),
                                  task_field_type_name(field.type),
                                  field.parent,
                                  value));
  }
  return result;
}

PyDoc_STRVAR(py_tasks_set_doc,
             ".. function:: tasks_set(handle, index, field, value)\n"
             "\n"
             "   Set field ``field`` of top level task ``index``: an int for integers and booleans,\n"
             "   a float, or a str for texts. Raises ValueError when the value does not fit.\n");
static PyObject *py_tasks_set(PyObject * /*self*/, PyObject *args)
{
  long handle;
  Py_ssize_t index;
  int field;
  PyObject *py_value;
  if (!PyArg_ParseTuple(args, "lniO:tasks_set", &handle, &index, &field, &py_value)) {
    return nullptr;
  }
  OpenTasks *tasks = tasks_or_raise(handle);
  TaskParse *parse = tasks ? task_parse_or_raise(*tasks, index) : nullptr;
  if (!parse) {
    return nullptr;
  }
  if (field < 0 || size_t(field) >= parse->fields.size()) {
    PyErr_SetString(PyExc_IndexError, "tasks_set: field index out of range");
    return nullptr;
  }
  Bytes &blob = tasks->data.tasks[size_t(index)];
  std::string error = "tasks_set: this field cannot be set";
  bool ok = false;
  switch (parse->fields[size_t(field)].type) {
    case TaskFieldType::Bool:
    case TaskFieldType::U8:
    case TaskFieldType::I16:
    case TaskFieldType::I32:
    case TaskFieldType::U32: {
      const long long value = PyLong_AsLongLong(py_value);
      if (PyErr_Occurred()) {
        return nullptr;
      }
      ok = task_set_int(blob, *parse, field, value, &error);
      break;
    }
    case TaskFieldType::F32: {
      const double value = PyFloat_AsDouble(py_value);
      if (PyErr_Occurred()) {
        return nullptr;
      }
      ok = task_set_float(blob, *parse, field, float(value));
      break;
    }
    case TaskFieldType::WCharsXor:
    case TaskFieldType::WChars:
    case TaskFieldType::Chars:
    case TaskFieldType::PwStrXor: {
      const char *text = PyUnicode_AsUTF8(py_value);
      if (!text) {
        return nullptr;
      }
      ok = task_set_text(blob, *parse, field, text, &error);
      break;
    }
    default:
      break;
  }
  if (!ok) {
    PyErr_SetString(PyExc_ValueError, error.c_str());
    return nullptr;
  }
  Py_RETURN_NONE;
}

PyDoc_STRVAR(py_tasks_find_doc,
             ".. function:: tasks_find(handle, id)\n"
             "\n"
             "   ``(index, node)`` of the task (or sub task) with this id, or None.\n");
static PyObject *py_tasks_find(PyObject * /*self*/, PyObject *args)
{
  long handle;
  unsigned long id;
  if (!PyArg_ParseTuple(args, "lk:tasks_find", &handle, &id)) {
    return nullptr;
  }
  const OpenTasks *tasks = tasks_or_raise(handle);
  if (!tasks) {
    return nullptr;
  }
  long long found_index = -1;
  int found_node = 0;
  Py_BEGIN_ALLOW_THREADS;
  const uint32_t version = tasks->data.version;
  for (size_t i = 0; i < tasks->data.tasks.size() && found_index < 0; i++) {
    if (task_record_id(tasks->data.tasks[i], version) == uint32_t(id)) {
      found_index = (long long)i;
    }
  }
  for (size_t i = 0; i < tasks->data.tasks.size() && found_index < 0; i++) {
    TaskParse parse;
    if (parse_task_record(tasks->data.tasks[i], version, parse, nullptr, false)) {
      for (size_t k = 1; k < parse.nodes.size(); k++) {
        if (parse.nodes[k].id == uint32_t(id)) {
          found_index = (long long)i;
          found_node = int(k);
          break;
        }
      }
    }
  }
  Py_END_ALLOW_THREADS;
  if (found_index < 0) {
    Py_RETURN_NONE;
  }
  return Py_BuildValue("(Li)", found_index, found_node);
}

PyDoc_STRVAR(py_tasks_duplicate_doc,
             ".. function:: tasks_duplicate(handle, index, first_id=0)\n"
             "\n"
             "   Append a copy of top level task ``index`` (with its sub tasks) whose tasks get the\n"
             "   ids ``first_id``, ``first_id + 1``, ... in depth first order (0: one past the\n"
             "   largest task id of the file). Texts are encrypted again for the new ids and the\n"
             "   links between the copied tasks follow. Returns ``(new index, [new ids])``; raises\n"
             "   ValueError when one of the ids is already used.\n");
static PyObject *py_tasks_duplicate(PyObject * /*self*/, PyObject *args)
{
  long handle;
  Py_ssize_t index;
  unsigned long first_id = 0;
  if (!PyArg_ParseTuple(args, "ln|k:tasks_duplicate", &handle, &index, &first_id)) {
    return nullptr;
  }
  OpenTasks *tasks = tasks_or_raise(handle);
  if (!tasks) {
    return nullptr;
  }
  if (index < 0 || size_t(index) >= tasks->data.tasks.size()) {
    PyErr_SetString(PyExc_IndexError, "tasks_duplicate: task index out of range");
    return nullptr;
  }
  const uint32_t version = tasks->data.version;
  Bytes blob = tasks->data.tasks[size_t(index)];
  TaskParse parse;
  std::string error;
  std::vector<uint32_t> new_ids;
  bool ok = false;
  Py_BEGIN_ALLOW_THREADS;
  /* Every id of the file (top level and sub tasks). */
  std::vector<uint32_t> used;
  uint32_t max_id = 0;
  for (const Bytes &record : tasks->data.tasks) {
    TaskParse nodes;
    if (parse_task_record(record, version, nodes, nullptr, false)) {
      for (const TaskNodeInfo &node : nodes.nodes) {
        used.push_back(node.id);
        max_id = std::max(max_id, node.id);
      }
    }
  }
  std::sort(used.begin(), used.end());
  ok = parse_task_record(blob, version, parse, &error);
  if (ok) {
    const uint32_t first = first_id ? uint32_t(first_id) : max_id + 1;
    for (size_t k = 0; ok && k < parse.nodes.size(); k++) {
      const uint32_t id = first + uint32_t(k);
      if (std::binary_search(used.begin(), used.end(), id)) {
        error = "tasks_duplicate: the id " + std::to_string(id) + " is already used";
        ok = false;
        break;
      }
      new_ids.push_back(id);
    }
    /* The new ids are all unused, so rewriting one never meets a link to another new id. */
    for (size_t k = 0; ok && k < parse.nodes.size(); k++) {
      ok = task_set_id(blob, parse, int32_t(k), new_ids[k], &error);
    }
  }
  Py_END_ALLOW_THREADS;
  if (!ok) {
    PyErr_SetString(PyExc_ValueError, error.c_str());
    return nullptr;
  }
  tasks->data.tasks.push_back(std::move(blob));
  const size_t new_index = tasks->data.tasks.size() - 1;
  tasks->parses.erase(new_index);
  PyObject *ids = PyList_New(Py_ssize_t(new_ids.size()));
  for (size_t k = 0; k < new_ids.size(); k++) {
    PyList_SET_ITEM(ids, Py_ssize_t(k), PyLong_FromUnsignedLong(new_ids[k]));
  }
  return Py_BuildValue("(nN)", Py_ssize_t(new_index), ids);
}

PyDoc_STRVAR(py_tasks_bytes_doc,
             ".. function:: tasks_bytes(handle)\n"
             "\n"
             "   The file bytes of an open ``tasks.data`` (unedited tasks keep their bytes).\n");
static PyObject *py_tasks_bytes(PyObject * /*self*/, PyObject *args)
{
  long handle;
  if (!PyArg_ParseTuple(args, "l:tasks_bytes", &handle)) {
    return nullptr;
  }
  const OpenTasks *tasks = tasks_or_raise(handle);
  if (!tasks) {
    return nullptr;
  }
  Bytes out;
  std::string error;
  bool ok = false;
  Py_BEGIN_ALLOW_THREADS;
  ok = write_tasks(tasks->data, out, &error);
  Py_END_ALLOW_THREADS;
  if (!ok) {
    PyErr_SetString(PyExc_ValueError, error.c_str());
    return nullptr;
  }
  return PyBytes_FromStringAndSize(reinterpret_cast<const char *>(out.data()), Py_ssize_t(out.size()));
}

static PyObject *py_tasks_close(PyObject * /*self*/, PyObject *args)
{
  long handle;
  if (!PyArg_ParseTuple(args, "l:tasks_close", &handle)) {
    return nullptr;
  }
  open_tasks().erase(handle);
  Py_RETURN_NONE;
}

/** \} */

static PyObject *py_set_part_limit_for_tests(PyObject * /*self*/, PyObject *args)
{
  unsigned long long limit;
  if (!PyArg_ParseTuple(args, "K:_set_part_limit_for_tests", &limit)) {
    return nullptr;
  }
  pck_writer_set_part_limit_for_tests(uint64_t(limit));
  Py_RETURN_NONE;
}

static PyMethodDef methods[] = {
    {"package_info", py_package_info, METH_VARARGS, py_package_info_doc},
    {"package_entries", py_package_entries, METH_VARARGS, py_package_entries_doc},
    {"locate", py_locate, METH_VARARGS, py_locate_doc},
    {"read", py_read, METH_VARARGS, py_read_doc},
    {"dependencies", py_dependencies, METH_VARARGS, py_dependencies_doc},
    {"release", py_release, METH_VARARGS, py_release_doc},
    {"package_read", py_package_read, METH_VARARGS, py_package_read_doc},
    {"pck_update", py_pck_update, METH_VARARGS, py_pck_update_doc},
    {"pck_can_undo", py_pck_can_undo, METH_VARARGS, py_pck_can_undo_doc},
    {"pck_undo", py_pck_undo, METH_VARARGS, py_pck_undo_doc},
    {"pck_write_patch", py_pck_write_patch, METH_VARARGS, py_pck_write_patch_doc},
    {"pck_write_copy", py_pck_write_copy, METH_VARARGS, py_pck_write_copy_doc},
    {"compress_blocks", py_compress_blocks, METH_VARARGS, py_compress_blocks_doc},
    {"ski_layout", py_ski_layout, METH_VARARGS, py_ski_layout_doc},
    {"bon_layout", py_bon_layout, METH_VARARGS, py_bon_layout_doc},
    {"stck_decode", py_stck_decode, METH_VARARGS, py_stck_decode_doc},
    {"decompress_blocks", py_decompress_blocks, METH_VARARGS, py_decompress_blocks_doc},
    {"schemas_directory", py_schemas_directory, METH_VARARGS, py_schemas_directory_doc},
    {"elements_open", py_elements_open, METH_VARARGS, py_elements_open_doc},
    {"elements_info", py_elements_info, METH_VARARGS, py_elements_info_doc},
    {"elements_record", py_elements_record, METH_VARARGS, py_elements_record_doc},
    {"elements_find", py_elements_find, METH_VARARGS, py_elements_find_doc},
    {"elements_set", py_elements_set, METH_VARARGS, py_elements_set_doc},
    {"elements_add", py_elements_add, METH_VARARGS, py_elements_add_doc},
    {"elements_max_id", py_elements_max_id, METH_VARARGS, py_elements_max_id_doc},
    {"elements_bytes", py_elements_bytes, METH_VARARGS, py_elements_bytes_doc},
    {"elements_ids", py_elements_ids, METH_VARARGS, py_elements_ids_doc},
    {"elements_find_in_space", py_elements_find_in_space, METH_VARARGS, py_elements_find_in_space_doc},
    {"elements_close", py_elements_close, METH_VARARGS, nullptr},
    {"tasks_open", py_tasks_open, METH_VARARGS, py_tasks_open_doc},
    {"tasks_info", py_tasks_info, METH_VARARGS, py_tasks_info_doc},
    {"tasks_list", py_tasks_list, METH_VARARGS, py_tasks_list_doc},
    {"tasks_nodes", py_tasks_nodes, METH_VARARGS, py_tasks_nodes_doc},
    {"tasks_fields", py_tasks_fields, METH_VARARGS, py_tasks_fields_doc},
    {"tasks_set", py_tasks_set, METH_VARARGS, py_tasks_set_doc},
    {"tasks_find", py_tasks_find, METH_VARARGS, py_tasks_find_doc},
    {"tasks_duplicate", py_tasks_duplicate, METH_VARARGS, py_tasks_duplicate_doc},
    {"tasks_bytes", py_tasks_bytes, METH_VARARGS, py_tasks_bytes_doc},
    {"tasks_close", py_tasks_close, METH_VARARGS, nullptr},
    {"_set_part_limit_for_tests", py_set_part_limit_for_tests, METH_VARARGS, nullptr},
    {nullptr, nullptr, 0, nullptr},
};

PyDoc_STRVAR(module_doc, "Angelica Engine (Perfect World) client packages and files.");

static PyModuleDef module = {
    PyModuleDef_HEAD_INIT,
    "_angelica",
    module_doc,
    -1,
    methods,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
};

}  // namespace io::angelica

PyObject *BPyInit__angelica();

PyObject *BPyInit__angelica()
{
  return PyModule_Create(&io::angelica::module);
}

}  // namespace blender
