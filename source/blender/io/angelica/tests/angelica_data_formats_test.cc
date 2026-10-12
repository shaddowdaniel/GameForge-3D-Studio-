/* SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup angelica
 *
 * Tests for the game data formats: `elements.data` (with sELedit `.cfg` schemas) and the server
 * spawn file `npcgen.data`. Files and schemas are synthesized in memory. When the environment
 * variable `ANGELICA_TEST_DATA` points at a client `element` folder, `data/elements.data` is
 * additionally parsed and written back byte for byte, with the `.cfg` schemas of the folder named
 * by `ANGELICA_ELEMENTS_SCHEMAS` (or a `schemas` / `configs` folder inside `ANGELICA_TEST_DATA`).
 * No schemas ship with the repository.
 */

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>

#include "testing/testing.h"

#include "../formats/angelica_elements.hh"
#include "../formats/angelica_npcgen.hh"

namespace blender::io::angelica::tests {

namespace fs = std::filesystem;

/* -------------------------------------------------------------------- */
/** \name Helpers
 * \{ */

class Writer {
 public:
  Bytes data;

  template<typename T> Writer &put(const T &value)
  {
    const uint8_t *p = reinterpret_cast<const uint8_t *>(&value);
    data.insert(data.end(), p, p + sizeof(T));
    return *this;
  }
  Writer &u8(const uint8_t v)
  {
    return put(v);
  }
  Writer &u16(const uint16_t v)
  {
    return put(v);
  }
  Writer &i16(const int16_t v)
  {
    return put(v);
  }
  Writer &i32(const int32_t v)
  {
    return put(v);
  }
  Writer &u32(const uint32_t v)
  {
    return put(v);
  }
  Writer &i64(const int64_t v)
  {
    return put(v);
  }
  Writer &f32(const float v)
  {
    return put(v);
  }
  Writer &raw(const void *p, const size_t n)
  {
    const uint8_t *b = static_cast<const uint8_t *>(p);
    data.insert(data.end(), b, b + n);
    return *this;
  }
  Writer &zeros(const size_t n)
  {
    data.insert(data.end(), n, 0);
    return *this;
  }
  /** Fixed size UTF-16LE field from ASCII text. */
  Writer &wstr(const std::string &ascii, const size_t bytes)
  {
    for (size_t i = 0; i < bytes / 2; i++) {
      const uint16_t c = i < ascii.size() ? uint16_t(uint8_t(ascii[i])) : 0;
      put(c);
    }
    return *this;
  }
  /** Fixed size 8-bit field. */
  Writer &str(const std::string &text, const size_t bytes)
  {
    for (size_t i = 0; i < bytes; i++) {
      u8(i < text.size() ? uint8_t(text[i]) : 0);
    }
    return *this;
  }
};

static std::string test_data_dir()
{
  const char *env = std::getenv("ANGELICA_TEST_DATA");
  return env ? std::string(env) : std::string();
}

static fs::path make_temp_dir(const char *name)
{
  const fs::path dir = fs::temp_directory_path() / (std::string("angelica_data_test_") + name +
                                                    "_" + std::to_string(std::random_device()()));
  fs::create_directories(dir);
  return dir;
}

static Bytes read_all(const fs::path &path)
{
  std::ifstream in(path, std::ios::binary);
  return Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name elements.data (synthetic)
 * \{ */

/* Talk procs last, as in 1.7.x schemas (1.5.x schemas put them after NPC_ESSENCE). */
static const char *TEST_CFG =
    "4\r\n"
    "3\r\n"
    "\r\n"
    "001 - EQUIPMENT_ADDON\r\n"
    "4\r\n"
    "ID;Name;num_params\r\n"
    "int32;wstring:8;int32\r\n"
    "\r\n"
    "002 - MONSTER_ESSENCE\r\n"
    "AUTO\r\n"
    "ID;Name;file_model;size;flags;big\r\n"
    "int32;wstring:16;string:16;float;byte:3;int64\r\n"
    "\r\n"
    "004 - NPC_ESSENCE\r\n"
    "0\r\n"
    "ID;Name;file_model;id_src_monster;level\r\n"
    "int32;wstring:16;string:16;int32;int16;\r\n"
    "\r\n"
    "003 - TALK_PROC\r\n"
    "0\r\n"
    "RAW\r\n"
    "byte:AUTO\r\n";

static void write_monster(Writer &w,
                          const int32_t id,
                          const std::string &name,
                          const std::string &model,
                          const float size)
{
  w.i32(id).wstr(name, 16).str(model, 16).f32(size).u8(1).u8(2).u8(3).i64(int64_t(1) << 40);
}

static Bytes build_test_elements()
{
  Writer w;
  w.u32(0x30000001);
  /* EQUIPMENT_ADDON: 4 byte prefix (the export time), 2 records of 16 bytes. */
  w.u32(0x12345678);
  w.i32(2);
  w.i32(1).wstr("Atk", 8).i32(1);
  w.i32(2).wstr("Def", 8).i32(2);
  /* MONSTER_ESSENCE: exporter tag block, 2 records. */
  w.u32(0xAB7689DD).u32(5).raw("host!", 5).u32(0x5555AAAA);
  w.i32(2);
  write_monster(w, 100, "Wolf", "models\\wolf.ecm", 1.5f);
  write_monster(w, 101, "Bear", "models\\bear.ecm", 2.0f);
  /* NPC_ESSENCE: 1 record taking its size from monster 101. */
  w.i32(1);
  w.i32(200).wstr("Smith", 16).str("models\\smith.ecm", 16).i32(101).i16(-3);
  /* TALK_PROC: one dialogue with one window and one option. */
  w.i32(1);
  w.u32(7).wstr("root", 128).i32(1);
  w.u32(1).u32(0xFFFFFFFF).i32(3).wstr("hi", 6).i32(1);
  w.u32(0x80000001).wstr("Bye", 128).u32(5);
  return w.data;
}

constexpr int TALK = 3;

static void load_test_schema(ElementsSchema &schema)
{
  std::string err;
  ASSERT_TRUE(load_elements_schema(TEST_CFG, schema, &err)) << err;
}

TEST(angelica_elements, schema)
{
  ElementsSchema schema;
  load_test_schema(schema);
  ASSERT_EQ(schema.lists.size(), 4u);
  EXPECT_EQ(schema.lists[0].name, "EQUIPMENT_ADDON");
  EXPECT_EQ(schema.lists[0].prefix_size, 4u);
  EXPECT_EQ(schema.lists[0].record_size, 16u);
  EXPECT_EQ(schema.lists[0].id_space, ElementIdSpace::Addon);
  EXPECT_EQ(schema.lists[1].name, "MONSTER_ESSENCE");
  EXPECT_TRUE(schema.lists[1].prefix_auto);
  EXPECT_EQ(schema.lists[1].record_size, 4u + 16u + 16u + 4u + 3u + 8u);
  EXPECT_EQ(schema.lists[1].fields[5].offset, 43u);
  EXPECT_EQ(schema.lists[1].id_space, ElementIdSpace::Essence);
  EXPECT_EQ(schema.talk_proc_index, 3);
  EXPECT_TRUE(schema.lists[3].is_talk_proc);
  EXPECT_EQ(schema.lists[3].label_number, 3);
  EXPECT_EQ(schema.lists[2].name, "NPC_ESSENCE");
  EXPECT_EQ(schema.lists[2].label, "004 - NPC_ESSENCE");
  EXPECT_EQ(schema.lists[2].record_size, 4u + 16u + 16u + 4u + 2u);
  EXPECT_EQ(schema.lists[2].field_index("level"), 4);
  EXPECT_EQ(schema.list_index("NPC_ESSENCE"), 2);
  EXPECT_EQ(schema.list_index("002 - MONSTER_ESSENCE"), 1);
  EXPECT_EQ(schema.list_index("NOPE"), -1);

  EXPECT_EQ(elements_schema_version_from_filename("PW_1.5.5_v156.cfg"), 156u);
  EXPECT_EQ(elements_schema_version_from_filename("PW_1.7.8-seledit_v402.cfg"), 402u);
  EXPECT_EQ(elements_schema_version_from_filename("PW_1.5.7_v208_0.cfg"), 208u);
  EXPECT_EQ(elements_schema_version_from_filename("readme.cfg"), 0u);

  /* `PW_AUTO_*` schemas type the talk-proc list with one file's byte count. */
  std::string err;
  ElementsSchema auto_schema;
  ASSERT_TRUE(
      load_elements_schema("2\n1\n\n001 - A\n0\nID\nint32\n\n"
                           "002 - TALK_PROC\n0\nRAW\nbyte:2204548\n",
                           auto_schema,
                           &err))
      << err;
  EXPECT_EQ(auto_schema.talk_proc_index, 1);
  EXPECT_TRUE(auto_schema.lists[1].is_talk_proc);
  /* Elsewhere `byte:N` stays a fixed-size field. */
  ASSERT_TRUE(
      load_elements_schema("2\n1\n\n001 - A\n0\nID;Blob\nint32;byte:12\n\n"
                           "002 - TALK_PROC\n0\nRAW\nbyte:AUTO\n",
                           auto_schema,
                           &err))
      << err;
  EXPECT_EQ(auto_schema.lists[0].record_size, 16u);
}

TEST(angelica_elements, schema_rejects_malformed)
{
  ElementsSchema schema;
  std::string err;
  EXPECT_FALSE(load_elements_schema("", schema, &err));
  EXPECT_FALSE(load_elements_schema("abc\n58\n", schema, &err));
  EXPECT_FALSE(load_elements_schema("2\n0\n\n001 - A\n0\nID\nint32\n", schema, &err));
  EXPECT_FALSE(load_elements_schema("1\n0\n\n001 - A\n0\nID;Name\nint32\n", schema, &err));
  EXPECT_FALSE(load_elements_schema("1\n0\n\n001 - A\n0\nID\nquux\n", schema, &err));
  EXPECT_FALSE(load_elements_schema("1\n0\n\n001 - A\nX\nID\nint32\n", schema, &err));
  EXPECT_FALSE(load_elements_schema("1\n0\n\n001 - A\n0\nID\nwstring:0\n", schema, &err));
  EXPECT_FALSE(err.empty());
  /* Every truncation of a valid schema either fails or parses fewer complete lists. */
  const std::string cfg = TEST_CFG;
  for (size_t n = 0; n < cfg.size(); n++) {
    ElementsSchema partial;
    if (load_elements_schema(std::string_view(cfg).substr(0, n), partial, nullptr)) {
      EXPECT_EQ(partial.lists.size(), 4u);
    }
  }
}

TEST(angelica_elements, parse_and_roundtrip)
{
  ElementsSchema schema;
  load_test_schema(schema);
  const Bytes file = build_test_elements();
  ElementsData data;
  std::string err;
  ASSERT_TRUE(parse_elements(file, schema, data, &err)) << err;
  EXPECT_EQ(data.version(), 1u);
  ASSERT_EQ(data.lists.size(), 4u);
  EXPECT_EQ(data.lists[0].size(), 2u);
  EXPECT_EQ(data.lists[1].size(), 2u);
  EXPECT_EQ(data.lists[1].prefix.size(), 17u);
  EXPECT_EQ(data.lists[2].size(), 1u);
  EXPECT_EQ(data.lists[TALK].size(), 1u);
  EXPECT_TRUE(data.trailing.empty());

  const ElementRef bear = data.find("MONSTER_ESSENCE", 101);
  ASSERT_TRUE(bear.valid());
  EXPECT_EQ(bear.record, 1u);
  EXPECT_EQ(data.get_string(bear, "Name"), "Bear");
  EXPECT_EQ(data.get_string(bear, "file_model"), "models\\bear.ecm");
  EXPECT_FLOAT_EQ(float(data.get_float(bear, "size")), 2.0f);
  EXPECT_EQ(data.get_int(bear, "big"), int64_t(1) << 40);
  ElementValue flags;
  ASSERT_TRUE(data.get(bear, "flags", flags));
  EXPECT_EQ(std::get<Bytes>(flags), (Bytes{1, 2, 3}));
  EXPECT_FALSE(data.find("MONSTER_ESSENCE", 999).valid());
  EXPECT_FALSE(data.find("NOPE", 101).valid());
  EXPECT_EQ(data.find_in_space(ElementIdSpace::Addon, 2).list, 0);
  EXPECT_FALSE(data.find_in_space(ElementIdSpace::Essence, 2).valid());

  const ElementRef smith = data.find("NPC_ESSENCE", 200);
  EXPECT_EQ(data.get_int(smith, "level"), -3);
  std::vector<ElementValue> values;
  ASSERT_TRUE(data.decode_record(smith.list, smith.record, values));
  ASSERT_EQ(values.size(), 5u);
  EXPECT_EQ(std::get<int64_t>(values[0]), 200);

  ElementModelInfo info;
  ASSERT_TRUE(find_element_model(data, 200, info));
  EXPECT_EQ(info.list_name, "NPC_ESSENCE");
  EXPECT_EQ(info.name, "Smith");
  EXPECT_EQ(info.file_model, "models\\smith.ecm");
  EXPECT_TRUE(info.has_size);
  EXPECT_FLOAT_EQ(info.size, 2.0f);
  ASSERT_TRUE(find_element_model(data, 100, info));
  EXPECT_FLOAT_EQ(info.size, 1.5f);
  EXPECT_FALSE(find_element_model(data, 1, info)); /* Addon space, no model. */
  EXPECT_EQ(collect_element_models(data).size(), 3u);

  std::vector<TalkProc> talks;
  ASSERT_TRUE(parse_talk_procs(data.lists[TALK], talks, &err)) << err;
  ASSERT_EQ(talks.size(), 1u);
  EXPECT_EQ(talks[0].id_talk, 7u);
  EXPECT_EQ(talks[0].text, "root");
  ASSERT_EQ(talks[0].windows.size(), 1u);
  EXPECT_EQ(talks[0].windows[0].id_parent, 0xFFFFFFFFu);
  EXPECT_EQ(talks[0].windows[0].text, "hi");
  ASSERT_EQ(talks[0].windows[0].options.size(), 1u);
  EXPECT_EQ(talks[0].windows[0].options[0].text, "Bye");
  EXPECT_EQ(talks[0].windows[0].options[0].param, 5u);

  Bytes out;
  ASSERT_TRUE(write_elements(data, schema, out, &err)) << err;
  EXPECT_EQ(out, file);
}

TEST(angelica_elements, list_headers_v191_to_349)
{
  /* Versions 191..349 wrap each count in `i32 list_type` and `u32 item_size`, and the AUTO block
   * of list 0 is the export time plus a length prefixed block. */
  ElementsSchema schema;
  std::string err;
  ASSERT_TRUE(
      load_elements_schema("3\n2\n\n"
                           "001 - EQUIPMENT_ADDON\nAUTO\nID;Name\nint;wstring:4\n\n"
                           "002 - NPC_ESSENCE\n0\nID;Name;file_model\nint;wstring:4;string:8\n\n"
                           "003 - TALK_PROC\n0\nRAW\nbyte:AUTO\n",
                           schema,
                           &err))
      << err;
  Writer w;
  w.u16(340).u16(0x3000);
  w.u32(0x11223344).u32(3).raw("abc", 3);
  w.i32(7).i32(1).u32(8).i32(1).wstr("Ax", 4);
  w.i32(9).i32(2).u32(16);
  w.i32(5).wstr("N1", 4).str("m1.ecm", 8);
  w.i32(6).wstr("N2", 4).str("m2.ecm", 8);
  w.i32(0);
  const Bytes file = w.data;

  ElementsData data;
  ASSERT_TRUE(parse_elements(file, schema, data, &err)) << err;
  EXPECT_EQ(data.version(), 340u);
  EXPECT_EQ(data.lists[0].prefix.size(), 11u);
  EXPECT_EQ(data.lists[0].list_type, 7);
  EXPECT_EQ(data.lists[1].list_type, 9);
  EXPECT_EQ(data.get_string(data.find("NPC_ESSENCE", 6), "file_model"), "m2.ecm");
  Bytes out;
  ASSERT_TRUE(write_elements(data, schema, out, &err)) << err;
  EXPECT_EQ(out, file);

  /* The stored item size must match the schema. */
  Bytes bad = file;
  bad[4 + 11 + 8] = 9;
  EXPECT_FALSE(parse_elements(bad, schema, data, &err));
  EXPECT_NE(err.find("record size"), std::string::npos) << err;
}

TEST(angelica_elements, edit_and_reparse)
{
  ElementsSchema schema;
  load_test_schema(schema);
  ElementsData data;
  std::string err;
  ASSERT_TRUE(parse_elements(build_test_elements(), schema, data, &err)) << err;

  const ElementRef wolf = data.find("MONSTER_ESSENCE", 100);
  EXPECT_TRUE(data.set(wolf, "Name", std::string("Lobo \xC3\xA9"), &err)) << err;
  EXPECT_TRUE(data.set(wolf, "file_model", std::string("models\\lobo.ecm"), &err)) << err;
  EXPECT_TRUE(data.set(wolf, "size", 3.25, &err)) << err;
  EXPECT_TRUE(data.set(wolf, "big", int64_t(-7), &err)) << err;
  EXPECT_TRUE(data.set(wolf, "flags", Bytes{9, 8, 7}, &err)) << err;

  /* Type and range checking. */
  EXPECT_FALSE(data.set(wolf, "Name", int64_t(1), &err));
  EXPECT_FALSE(data.set(wolf, "Name", std::string("this name is far too long"), &err));
  EXPECT_FALSE(data.set(wolf, "file_model", std::string("models\\far_too_long.ecm"), &err));
  EXPECT_FALSE(data.set(wolf, "ID", int64_t(1) << 33, &err));
  EXPECT_FALSE(data.set(wolf, "ID", 1.5, &err));
  EXPECT_FALSE(data.set(wolf, "flags", Bytes{1}, &err));
  EXPECT_FALSE(data.set(wolf, "missing", int64_t(1), &err));
  EXPECT_FALSE(data.set(wolf, "Name", std::string("\xFF"), &err));
  const ElementRef smith = data.find("NPC_ESSENCE", 200);
  EXPECT_FALSE(data.set(smith, "level", int64_t(70000), &err));
  EXPECT_TRUE(data.set(smith, "level", int64_t(-32768), &err)) << err;

  /* Add a copy of the bear with a new id, remove the addon 1. */
  const int monsters = data.list_index("MONSTER_ESSENCE");
  EXPECT_EQ(data.max_id(monsters), 101u);
  const int64_t added = data.add_record(monsters, data.max_id(monsters) + 1, 1);
  ASSERT_EQ(added, 2);
  EXPECT_EQ(data.add_record(monsters, 0, 99), -1);
  EXPECT_EQ(data.add_record(TALK, 1), -1); /* Talk procs have no records. */
  EXPECT_TRUE(data.remove_record(0, 0));
  EXPECT_FALSE(data.remove_record(0, 5));

  Bytes out;
  ASSERT_TRUE(write_elements(data, schema, out, &err)) << err;
  ElementsData again;
  ASSERT_TRUE(parse_elements(out, schema, again, &err)) << err;
  const ElementRef wolf2 = again.find("MONSTER_ESSENCE", 100);
  EXPECT_EQ(again.get_string(wolf2, "Name"), "Lobo \xC3\xA9");
  EXPECT_EQ(again.get_string(wolf2, "file_model"), "models\\lobo.ecm");
  EXPECT_FLOAT_EQ(float(again.get_float(wolf2, "size")), 3.25f);
  EXPECT_EQ(again.get_int(wolf2, "big"), -7);
  const ElementRef copy = again.find("MONSTER_ESSENCE", 102);
  ASSERT_TRUE(copy.valid());
  EXPECT_EQ(again.get_string(copy, "Name"), "Bear");
  EXPECT_EQ(again.lists[0].size(), 1u);
  EXPECT_FALSE(again.find("EQUIPMENT_ADDON", 1).valid());
  EXPECT_EQ(again.get_int(again.find("NPC_ESSENCE", 200), "level"), -32768);
  EXPECT_EQ(again.lists[TALK].raw, data.lists[TALK].raw);

  Bytes out2;
  ASSERT_TRUE(write_elements(again, schema, out2, &err)) << err;
  EXPECT_EQ(out2, out);
}

TEST(angelica_elements, rejects_malformed)
{
  ElementsSchema schema;
  load_test_schema(schema);
  const Bytes file = build_test_elements();
  ElementsData data;
  std::string err;

  /* Every truncation fails. */
  for (size_t n = 0; n < file.size(); n++) {
    EXPECT_FALSE(parse_elements(ByteSpan(file).first(n), schema, data, &err)) << n;
  }
  /* Version 0. */
  Bytes bad = file;
  bad[0] = 0;
  EXPECT_FALSE(parse_elements(bad, schema, data, &err));
  /* The same bytes read as a packed (1.7.4+) version do not parse. */
  bad = file;
  bad[0] = uint8_t(352 & 0xFF);
  bad[1] = uint8_t(352 >> 8);
  EXPECT_FALSE(parse_elements(bad, schema, data, &err));
  /* Huge record count. */
  bad = file;
  std::memset(bad.data() + 8, 0x7F, 4);
  EXPECT_FALSE(parse_elements(bad, schema, data, &err));
  /* Negative record count. */
  bad = file;
  std::memset(bad.data() + 8, 0xFF, 4);
  EXPECT_FALSE(parse_elements(bad, schema, data, &err));
  /* Version mismatch with a versioned schema. */
  ElementsSchema versioned = schema;
  versioned.version = 156;
  EXPECT_FALSE(parse_elements(file, versioned, data, &err));
  EXPECT_NE(err.find("156"), std::string::npos);
  /* Too much data left over means the schema does not describe the file. */
  bad = file;
  bad.resize(bad.size() + ELEMENTS_MAX_TRAILING + 1, 0);
  EXPECT_FALSE(parse_elements(bad, schema, data, &err));
  /* A little is kept and written back. */
  bad = file;
  bad.push_back(0x42);
  ASSERT_TRUE(parse_elements(bad, schema, data, &err)) << err;
  EXPECT_EQ(data.trailing, Bytes{0x42});
  Bytes out;
  ASSERT_TRUE(write_elements(data, schema, out, &err));
  EXPECT_EQ(out, bad);

  /* Random corruption never crashes. */
  std::mt19937 rng(1234);
  for (int i = 0; i < 2000; i++) {
    Bytes fuzz = file;
    const int flips = 1 + int(rng() % 8);
    for (int k = 0; k < flips; k++) {
      fuzz[4 + rng() % (fuzz.size() - 4)] = uint8_t(rng());
    }
    if (parse_elements(fuzz, schema, data, nullptr)) {
      ASSERT_TRUE(write_elements(data, schema, out, nullptr));
      EXPECT_EQ(out, fuzz);
      std::vector<TalkProc> talks;
      parse_talk_procs(data.lists[TALK], talks, nullptr);
    }
  }
}

TEST(angelica_elements, schema_folder_and_safe_save)
{
  const fs::path dir = make_temp_dir("schemas");
  {
    std::ofstream(dir / "PW_test_v1.cfg", std::ios::binary) << TEST_CFG;
    std::ofstream(dir / "PW_test_v7.cfg", std::ios::binary) << "1\n0\n\n001 - A\n0\nID\nint32\n";
    std::ofstream(dir / "notes.txt") << "x";
  }
  const std::vector<ElementsSchemaFile> files = list_elements_schemas(dir.string());
  ASSERT_EQ(files.size(), 2u);
  EXPECT_EQ(files[0].version, 1u);
  EXPECT_EQ(files[1].version, 7u);

  const Bytes file = build_test_elements();
  uint32_t version = 0;
  std::string err;
  ASSERT_TRUE(peek_elements_version(file, version, &err));
  ElementsSchema schema;
  ASSERT_TRUE(load_elements_schema_for_version(dir.string(), version, schema, &err)) << err;
  EXPECT_EQ(schema.version, 1u);
  EXPECT_EQ(schema.source_name, "PW_test_v1.cfg");
  ElementsData data;
  ASSERT_TRUE(parse_elements(file, schema, data, &err)) << err;
  EXPECT_FALSE(load_elements_schema_for_version(dir.string(), 99, schema, &err));
  EXPECT_NE(err.find("available: 1, 7"), std::string::npos) << err;

  /* Safe save: the first save keeps the original as .bak, later saves keep that .bak. */
  const fs::path target = dir / "elements.data";
  {
    std::ofstream(target, std::ios::binary) << "original";
  }
  const Bytes v1 = {'o', 'n', 'e'};
  const Bytes v2 = {'t', 'w', 'o', '!'};
  ASSERT_TRUE(save_file_with_backup(target.string(), v1, &err)) << err;
  EXPECT_EQ(read_all(target), v1);
  const Bytes original = {'o', 'r', 'i', 'g', 'i', 'n', 'a', 'l'};
  EXPECT_EQ(read_all(fs::path(target.string() + ".bak")), original);
  ASSERT_TRUE(save_file_with_backup(target.string(), v2, &err)) << err;
  EXPECT_EQ(read_all(target), v2);
  EXPECT_EQ(read_all(fs::path(target.string() + ".bak")), original);
  EXPECT_FALSE(fs::exists(target.string() + ".tmp"));
  /* A new file gets no backup. */
  const fs::path fresh = dir / "new.data";
  ASSERT_TRUE(save_file_with_backup(fresh.string(), v1, &err)) << err;
  EXPECT_FALSE(fs::exists(fresh.string() + ".bak"));
  /* Unwritable location fails cleanly. */
  EXPECT_FALSE(save_file_with_backup((dir / "missing" / "x.data").string(), v1, &err));

  std::error_code ec;
  fs::remove_all(dir, ec);
}

TEST(angelica_elements, text_helpers)
{
  Bytes utf16;
  ASSERT_TRUE(utf8_to_utf16le("A\xC3\xA9\xF0\x9F\x98\x80", utf16));
  EXPECT_EQ(utf16.size(), 8u);
  EXPECT_EQ(utf16le_to_utf8(utf16), "A\xC3\xA9\xF0\x9F\x98\x80");
  EXPECT_FALSE(utf8_to_utf16le("\xC3", utf16));
  EXPECT_FALSE(utf8_to_utf16le("\xED\xA0\x80", utf16));
  std::string gbk;
  ASSERT_TRUE(utf8_to_gbk("models\\npc.ecm", gbk));
  EXPECT_EQ(gbk, "models\\npc.ecm");
  /* "怪物" (monster) round-trips through GBK when the platform converter is available. */
  if (utf8_to_gbk("\xE6\x80\xAA\xE7\x89\xA9", gbk)) {
    EXPECT_EQ(gbk, "\xB9\xD6\xCE\xEF");
    EXPECT_EQ(gbk_to_utf8(gbk), "\xE6\x80\xAA\xE7\x89\xA9");
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name elements.data packed versions (350+, PW 1.7.4 and later)
 * \{ */

TEST(angelica_elements, mppc)
{
  /* Streams produced by the reference `GNET::mppc::compress`. */
  const std::vector<std::pair<std::string, Bytes>> known = {
      {"aaaaaaaaaa", {0x61, 0xF0, 0x71}},
      {std::string("abcabcabcabc\x80\xFF", 14), {0x61, 0x62, 0x63, 0xF0, 0xF1, 0x80, 0x5F, 0xC0}},
      {"Perfect World Perfect World Perfect World!",
       {0x50,
        0x65,
        0x72,
        0x66,
        0x65,
        0x63,
        0x74,
        0x20,
        0x57,
        0x6F,
        0x72,
        0x6C,
        0x64,
        0x20,
        0xF3,
        0xBA,
        0xC8,
        0x40}},
  };
  for (const auto &[text, stream] : known) {
    const ByteSpan in(reinterpret_cast<const uint8_t *>(text.data()), text.size());
    Bytes packed, back;
    ASSERT_TRUE(mppc_compress_record(in, packed));
    EXPECT_EQ(packed, stream) << text;
    ASSERT_TRUE(mppc_decompress_record(stream, text.size(), back)) << text;
    EXPECT_EQ(std::string(back.begin(), back.end()), text);
    /* The size must match exactly. */
    EXPECT_FALSE(mppc_decompress_record(stream, text.size() + 1, back));
    EXPECT_FALSE(mppc_decompress_record(stream, text.size() - 1, back));
  }

  /* Round trips, single blocks and framed blocks (records above 8192 bytes). */
  std::mt19937 rng(7);
  for (int i = 0; i < 300; i++) {
    const size_t size = 1 + rng() % (i % 4 == 0 ? 40000 : 8192);
    Bytes in(size);
    for (size_t k = 0; k < size; k++) {
      switch (i % 3) {
        case 0:
          in[k] = uint8_t(rng());
          break;
        case 1:
          in[k] = (k % 23 < 4) ? uint8_t(rng()) : 0;
          break;
        default:
          in[k] = uint8_t(0x80 + rng() % 3);
          break;
      }
    }
    Bytes packed, back;
    ASSERT_TRUE(mppc_compress_record(in, packed));
    ASSERT_TRUE(mppc_decompress_record(packed, size, back)) << size;
    EXPECT_EQ(back, in);
  }
  Bytes packed;
  EXPECT_FALSE(mppc_compress_record({}, packed));

  /* Garbage never crashes the decoder. */
  for (int i = 0; i < 3000; i++) {
    Bytes garbage(rng() % 200);
    for (uint8_t &b : garbage) {
      b = uint8_t(rng());
    }
    Bytes back;
    mppc_decompress_record(garbage, 1 + rng() % 20000, back);
  }
}

static const char *PACKED_CFG =
    "4\n"
    "3\n"
    "\n"
    "001 - EQUIPMENT_ADDON\n"
    "AUTO\n"
    "ID;Name;num_params\n"
    "int32;wstring:8;int32\n"
    "\n"
    "002 - MONSTER_ESSENCE\n"
    "0\n"
    "ID;Name;file_model;size\n"
    "int32;wstring:16;string:16;float\n"
    "\n"
    "003 - ILLUSTRATED_REWARD_CONFIG\n"
    "0\n"
    "ID;table\n"
    "int32;byte:9000\n"
    "\n"
    "004 - TALK_PROC\n"
    "0\n"
    "RAW\n"
    "byte:AUTO\n";

struct PackedRecord {
  uint32_t id;
  Bytes blob;
};

static void put_packed_list(Writer &w, const std::vector<PackedRecord> &records)
{
  w.i32(int32_t(records.size()));
  if (records.empty()) {
    return;
  }
  uint32_t total = 0;
  for (const PackedRecord &r : records) {
    w.u32(r.id).u16(uint16_t(r.blob.size()));
    total += uint32_t(r.blob.size());
  }
  w.u32(total);
  for (const PackedRecord &r : records) {
    w.raw(r.blob.data(), r.blob.size());
  }
}

static Bytes pack(const Bytes &record)
{
  Bytes out;
  EXPECT_TRUE(mppc_compress_record(record, out));
  return out;
}

/**
 * A packed file. Two records are stored the way another encoder could have written them, not as
 * #mppc_compress_record would: an all-zero record as plain literals, and a large record as stored
 * (uncompressed) 8192 byte chunks. They must still be written back unchanged.
 */
static Bytes build_packed_elements()
{
  Writer w;
  w.u16(402).u16(0x3000);
  /* Export time, org data, computer name block, hardware info block. */
  w.u32(0x66778899);
  w.u32(3).raw("org", 3);
  w.u32(0x12345678).u32(4).raw("host", 4).u32(0x66778899);
  w.u32(0x9ABCDEF0).u32(2).raw("hw", 2);

  /* EQUIPMENT_ADDON. */
  Writer a1, a2;
  a1.i32(1).wstr("Atk", 8).i32(1);
  a2.zeros(16);
  put_packed_list(w, {{1, pack(a1.data)}, {0, Bytes(16, 0)}});

  /* MONSTER_ESSENCE. */
  Writer m1;
  m1.i32(100).wstr("Wolf", 16).str("models\\wolf.ecm", 16).f32(1.5f);
  put_packed_list(w, {{100, pack(m1.data)}});

  /* ILLUSTRATED_REWARD_CONFIG: 9004 byte records use framed chunks. */
  Writer big;
  big.i32(7);
  for (int i = 0; i < 9000; i++) {
    big.u8(uint8_t(i % 7 == 0 ? i : 0));
  }
  Bytes stored;
  for (size_t pos = 0; pos < big.data.size(); pos += 8192) {
    const size_t n = std::min<size_t>(8192, big.data.size() - pos);
    stored.push_back(uint8_t(n & 0xFF));
    stored.push_back(uint8_t(n >> 8));
    stored.insert(stored.end(), big.data.begin() + pos, big.data.begin() + pos + n);
  }
  put_packed_list(w, {{7, stored}});

  /* TALK_PROC, stored as is. */
  w.i32(1);
  w.u32(9).wstr("t", 128).i32(0);
  return w.data;
}

TEST(angelica_elements, packed_roundtrip)
{
  ElementsSchema schema;
  std::string err;
  ASSERT_TRUE(load_elements_schema(PACKED_CFG, schema, &err)) << err;
  ASSERT_TRUE(elements_version_is_packed(402));
  ASSERT_TRUE(elements_version_is_packed(350));
  EXPECT_FALSE(elements_version_is_packed(349));
  EXPECT_FALSE(elements_version_is_packed(1488));
  EXPECT_FALSE(elements_version_is_packed(30000));

  const Bytes file = build_packed_elements();
  ElementsData data;
  ASSERT_TRUE(parse_elements(file, schema, data, &err)) << err;
  EXPECT_EQ(data.version(), 402u);
  EXPECT_EQ(data.lists[0].prefix.size(), 4u + 7u + 16u + 10u);
  EXPECT_EQ(data.lists[0].size(), 2u);
  EXPECT_EQ(data.get_string(data.find("EQUIPMENT_ADDON", 1), "Name"), "Atk");
  const ElementRef wolf = data.find("MONSTER_ESSENCE", 100);
  ASSERT_TRUE(wolf.valid());
  EXPECT_EQ(data.get_string(wolf, "file_model"), "models\\wolf.ecm");
  EXPECT_FLOAT_EQ(float(data.get_float(wolf, "size")), 1.5f);
  ElementValue table;
  ASSERT_TRUE(data.get(data.find("ILLUSTRATED_REWARD_CONFIG", 7), "table", table));
  EXPECT_EQ(std::get<Bytes>(table)[7], 7);
  EXPECT_EQ(data.lists[3].size(), 1u);
  EXPECT_TRUE(data.trailing.empty());

  Bytes out;
  ASSERT_TRUE(write_elements(data, schema, out, &err)) << err;
  EXPECT_EQ(out, file);

  /* Edit one record, add and remove others; untouched records keep their stored bytes. */
  ASSERT_TRUE(data.set(wolf, "Name", std::string("Lobo"), &err)) << err;
  const int monsters = data.list_index("MONSTER_ESSENCE");
  ASSERT_EQ(data.add_record(monsters, 101, 0), 1);
  ASSERT_TRUE(data.set({monsters, 1}, "size", 2.5, &err)) << err;
  ASSERT_TRUE(data.remove_record(0, 0));
  /* Setting a field to its current value changes nothing. */
  ASSERT_TRUE(data.set(data.find("ILLUSTRATED_REWARD_CONFIG", 7), "ID", int64_t(7), &err));
  ASSERT_TRUE(write_elements(data, schema, out, &err)) << err;

  ElementsData again;
  ASSERT_TRUE(parse_elements(out, schema, again, &err)) << err;
  EXPECT_EQ(again.get_string(again.find("MONSTER_ESSENCE", 100), "Name"), "Lobo");
  const ElementRef added = again.find("MONSTER_ESSENCE", 101);
  ASSERT_TRUE(added.valid());
  EXPECT_EQ(again.get_string(added, "Name"), "Lobo");
  EXPECT_FLOAT_EQ(float(again.get_float(added, "size")), 2.5f);
  ASSERT_EQ(again.lists[0].size(), 1u);
  EXPECT_EQ(again.lists[0].packed[0].id, 0u);
  EXPECT_EQ(again.lists[0].packed[0].size, 16u);        /* The literal-only zero record. */
  EXPECT_EQ(again.lists[2].packed[0].size, 9004u + 4u); /* The stored chunks. */
  EXPECT_EQ(again.lists[1].packed[1].id, 101u);
  EXPECT_EQ(again.lists[0].prefix, data.lists[0].prefix);
  EXPECT_EQ(again.lists[3].raw, data.lists[3].raw);

  Bytes out2;
  ASSERT_TRUE(write_elements(again, schema, out2, &err)) << err;
  EXPECT_EQ(out2, out);

  /* An emptied list stores only its count. */
  ASSERT_TRUE(again.remove_record(0, 0));
  ASSERT_TRUE(write_elements(again, schema, out2, &err)) << err;
  ElementsData emptied;
  ASSERT_TRUE(parse_elements(out2, schema, emptied, &err)) << err;
  EXPECT_EQ(emptied.lists[0].size(), 0u);
}

TEST(angelica_elements, packed_rejects_malformed)
{
  ElementsSchema schema;
  std::string err;
  ASSERT_TRUE(load_elements_schema(PACKED_CFG, schema, &err)) << err;
  const Bytes file = build_packed_elements();
  ElementsData data;
  for (size_t n = 0; n < file.size(); n++) {
    EXPECT_FALSE(parse_elements(ByteSpan(file).first(n), schema, data, &err)) << n;
  }
  /* Header blocks start after the 8 byte header + time; list 0 starts after them. */
  const size_t list0 = 4 + 4 + 7 + 16 + 10;
  /* Total compressed size that disagrees with the index. */
  Bytes bad = file;
  bad[list0 + 4 + 12] ^= 1;
  EXPECT_FALSE(parse_elements(bad, schema, data, &err));
  /* A record that decompresses to the wrong size. */
  bad = file;
  bad[list0 + 4 + 4] = uint8_t(bad[list0 + 4 + 4] - 1);
  bad[list0 + 4 + 12] = uint8_t(bad[list0 + 4 + 12] - 1);
  EXPECT_FALSE(parse_elements(bad, schema, data, &err));
  /* Huge exporter block. */
  bad = file;
  bad[8 + 3] = 0x7F;
  EXPECT_FALSE(parse_elements(bad, schema, data, &err));

  std::mt19937 rng(99);
  Bytes out;
  for (int i = 0; i < 2000; i++) {
    Bytes fuzz = file;
    const int flips = 1 + int(rng() % 6);
    for (int k = 0; k < flips; k++) {
      fuzz[4 + rng() % (fuzz.size() - 4)] = uint8_t(rng());
    }
    if (parse_elements(fuzz, schema, data, nullptr)) {
      ASSERT_TRUE(write_elements(data, schema, out, nullptr));
      EXPECT_EQ(out, fuzz);
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name npcgen.data (synthetic)
 * \{ */

static NpcGenData build_test_npcgen(const uint32_t version)
{
  NpcGenData d;
  d.version = version;
  NpcGenArea area;
  area.type = 1;
  area.pos[0] = 100.0f;
  area.pos[1] = 20.0f;
  area.pos[2] = -300.0f;
  area.dir[0] = 0.0f;
  area.dir[1] = 0.0f;
  area.dir[2] = 1.0f;
  area.extents[0] = 5.0f;
  area.extents[1] = 2.0f;
  area.extents[2] = 6.0f;
  area.npc_type = 0;
  area.group_type = 3;
  area.init_gen = 1;
  area.auto_revive = 2; /* Unusual flag value must survive. */
  area.valid_once = 0;
  area.gen_id = 77;
  if (version >= 7) {
    area.controller_id = 5;
    area.life_time = 600;
    area.max_num = 9;
  }
  for (uint32_t i = 0; i < 2; i++) {
    NpcGenSpawn s;
    s.id = 1000 + i;
    s.count = 3 + i;
    s.refresh = 30;
    s.died_times = 1;
    s.aggressive = 1;
    s.offset_water = 0.5f;
    s.offset_terrain = 0.25f;
    s.faction = 8;
    s.faction_helper = 16;
    s.faction_accept = 32;
    s.need_help = 1;
    s.default_faction = 1;
    s.default_faction_helper = 0;
    s.default_faction_accept = 1;
    s.path_id = int32_t(i) - 1;
    s.loop_type = 2;
    s.speed_flag = 1;
    s.dead_time = 10;
    s.refresh_lower = version >= 11 ? 20 : 0;
    area.spawns.push_back(s);
  }
  d.areas.push_back(area);
  NpcGenArea empty_area;
  d.areas.push_back(empty_area);

  NpcGenResArea res;
  res.pos[0] = 1.0f;
  res.pos[1] = 2.0f;
  res.pos[2] = 3.0f;
  res.extent_x = 4.0f;
  res.extent_z = 5.0f;
  res.gen_id = 12;
  if (version >= 6) {
    res.dir[0] = 10;
    res.dir[1] = 20;
    res.rad = 30;
  }
  if (version >= 7) {
    res.controller_id = 6;
    res.max_num = 4;
  }
  res.resources.push_back({0, 3074, 60, 2, 0.75f});
  res.resources.push_back({1, 3075, 90, 1, 0.0f});
  d.resource_areas.push_back(res);

  if (version >= 6) {
    NpcGenDynObject obj;
    obj.id = 44;
    obj.pos[0] = 7.0f;
    obj.pos[1] = 8.0f;
    obj.pos[2] = 9.0f;
    obj.dir[0] = 1;
    obj.dir[1] = 2;
    obj.rad = 3;
    obj.controller_id = version >= 9 ? 5 : 0;
    obj.scale = version >= 10 ? 32 : 16;
    d.dynamic_objects.push_back(obj);
  }
  if (version >= 7) {
    NpcGenController c;
    c.id = 5;
    c.controller_id = 50001;
    EXPECT_TRUE(c.set_name("Boss wave", nullptr));
    c.activated = 0;
    c.wait_time = 15;
    c.stop_time = 3600;
    c.active_time_invalid = 0;
    c.stop_time_invalid = 1;
    c.active_time = {-1, -1, 6, -1, 20, 30};
    c.stop_time_point = {2026, 11, -1, 31, 23, 59};
    c.active_time_range = version >= 8 ? 45 : 0;
    d.controllers.push_back(c);
  }
  return d;
}

static void expect_npcgen_equal(const NpcGenData &a, const NpcGenData &b)
{
  ASSERT_EQ(a.version, b.version);
  ASSERT_EQ(a.areas.size(), b.areas.size());
  for (size_t i = 0; i < a.areas.size(); i++) {
    const NpcGenArea &x = a.areas[i], &y = b.areas[i];
    EXPECT_EQ(x.type, y.type);
    EXPECT_EQ(std::memcmp(x.pos, y.pos, sizeof(x.pos)), 0);
    EXPECT_EQ(std::memcmp(x.dir, y.dir, sizeof(x.dir)), 0);
    EXPECT_EQ(std::memcmp(x.extents, y.extents, sizeof(x.extents)), 0);
    EXPECT_EQ(x.npc_type, y.npc_type);
    EXPECT_EQ(x.group_type, y.group_type);
    EXPECT_EQ(x.init_gen, y.init_gen);
    EXPECT_EQ(x.auto_revive, y.auto_revive);
    EXPECT_EQ(x.valid_once, y.valid_once);
    EXPECT_EQ(x.gen_id, y.gen_id);
    EXPECT_EQ(x.controller_id, y.controller_id);
    EXPECT_EQ(x.life_time, y.life_time);
    EXPECT_EQ(x.max_num, y.max_num);
    ASSERT_EQ(x.spawns.size(), y.spawns.size());
    for (size_t k = 0; k < x.spawns.size(); k++) {
      EXPECT_EQ(std::memcmp(&x.spawns[k], &y.spawns[k], sizeof(NpcGenSpawn)), 0);
    }
  }
  ASSERT_EQ(a.resource_areas.size(), b.resource_areas.size());
  for (size_t i = 0; i < a.resource_areas.size(); i++) {
    const NpcGenResArea &x = a.resource_areas[i], &y = b.resource_areas[i];
    EXPECT_EQ(std::memcmp(x.pos, y.pos, sizeof(x.pos)), 0);
    EXPECT_EQ(x.extent_x, y.extent_x);
    EXPECT_EQ(x.extent_z, y.extent_z);
    EXPECT_EQ(x.gen_id, y.gen_id);
    EXPECT_EQ(x.dir[0], y.dir[0]);
    EXPECT_EQ(x.dir[1], y.dir[1]);
    EXPECT_EQ(x.rad, y.rad);
    EXPECT_EQ(x.controller_id, y.controller_id);
    EXPECT_EQ(x.max_num, y.max_num);
    ASSERT_EQ(x.resources.size(), y.resources.size());
    for (size_t k = 0; k < x.resources.size(); k++) {
      EXPECT_EQ(x.resources[k].template_id, y.resources[k].template_id);
      EXPECT_EQ(x.resources[k].type, y.resources[k].type);
      EXPECT_EQ(x.resources[k].refresh_time, y.resources[k].refresh_time);
      EXPECT_EQ(x.resources[k].count, y.resources[k].count);
      EXPECT_EQ(x.resources[k].height_offset, y.resources[k].height_offset);
    }
  }
  ASSERT_EQ(a.dynamic_objects.size(), b.dynamic_objects.size());
  for (size_t i = 0; i < a.dynamic_objects.size(); i++) {
    const NpcGenDynObject &x = a.dynamic_objects[i], &y = b.dynamic_objects[i];
    EXPECT_EQ(x.id, y.id);
    EXPECT_EQ(std::memcmp(x.pos, y.pos, sizeof(x.pos)), 0);
    EXPECT_EQ(x.rad, y.rad);
    EXPECT_EQ(x.controller_id, y.controller_id);
    EXPECT_EQ(x.scale, y.scale);
  }
  ASSERT_EQ(a.controllers.size(), b.controllers.size());
  for (size_t i = 0; i < a.controllers.size(); i++) {
    const NpcGenController &x = a.controllers[i], &y = b.controllers[i];
    EXPECT_EQ(x.id, y.id);
    EXPECT_EQ(x.controller_id, y.controller_id);
    EXPECT_EQ(x.name_raw, y.name_raw);
    EXPECT_EQ(x.activated, y.activated);
    EXPECT_EQ(x.wait_time, y.wait_time);
    EXPECT_EQ(x.stop_time, y.stop_time);
    EXPECT_EQ(x.active_time_invalid, y.active_time_invalid);
    EXPECT_EQ(x.stop_time_invalid, y.stop_time_invalid);
    EXPECT_EQ(std::memcmp(&x.active_time, &y.active_time, sizeof(NpcGenCtrlTime)), 0);
    EXPECT_EQ(std::memcmp(&x.stop_time_point, &y.stop_time_point, sizeof(NpcGenCtrlTime)), 0);
    EXPECT_EQ(x.active_time_range, y.active_time_range);
  }
  EXPECT_EQ(a.trailing, b.trailing);
}

/** Expected file size from the documented record sizes. */
static size_t expected_npcgen_size(const uint32_t v)
{
  const size_t header = 4 + (v >= 7 ? 16 : (v >= 6 ? 12 : 8));
  const size_t area = v >= 7 ? 71 : 59;
  const size_t spawn = v >= 11 ? 64 : 60;
  const size_t res_area = v >= 7 ? 42 : (v >= 6 ? 34 : 31);
  const size_t dyn = v >= 6 ? (v >= 10 ? 24 : (v >= 9 ? 23 : 19)) : 0;
  const size_t ctrl = v >= 7 ? (v >= 8 ? 199 : 195) : 0;
  return header + 2 * area + 2 * spawn + res_area + 2 * 20 + dyn + ctrl;
}

TEST(angelica_npcgen, roundtrip_all_versions)
{
  for (uint32_t v = 1; v <= NPCGEN_VERSION_MAX; v++) {
    SCOPED_TRACE("version " + std::to_string(v));
    const NpcGenData src = build_test_npcgen(v);
    Bytes file;
    std::string err;
    ASSERT_TRUE(write_npcgen(src, file, &err)) << err;
    EXPECT_EQ(file.size(), expected_npcgen_size(v));
    NpcGenData parsed;
    ASSERT_TRUE(parse_npcgen(file, parsed, &err)) << err;
    expect_npcgen_equal(src, parsed);
    EXPECT_EQ(parsed.spawn_count(), 2u);
    EXPECT_EQ(parsed.resource_count(), 2u);
    Bytes again;
    ASSERT_TRUE(write_npcgen(parsed, again, &err)) << err;
    EXPECT_EQ(again, file);
  }
}

TEST(angelica_npcgen, documented_layout_v11)
{
  /* Hand written version 11 file, independent of the writer. */
  Writer w;
  w.u32(11).i32(1).i32(1).i32(1).i32(1);
  /* Area. */
  w.i32(0).i32(1).f32(10).f32(11).f32(12).f32(0).f32(0).f32(1).f32(3).f32(4).f32(5);
  w.i32(1).i32(0).u8(1).u8(0).u8(1).u32(99).i32(5).i32(120).i32(8);
  /* Spawn. */
  w.u32(2109).u32(1).i32(300).u32(0).u32(0).f32(0).f32(0).u32(0).u32(0).u32(0);
  w.u8(0).u8(1).u8(1).u8(1).i32(0).i32(0).i32(0).i32(5).i32(200);
  /* Resource area + resource. */
  w.f32(1).f32(2).f32(3).f32(6).f32(7).i32(1).u8(1).u8(1).u8(0).u32(3);
  w.u8(4).u8(5).u8(6).i32(5).i32(2);
  w.i32(0).i32(3074).u32(60).u32(2).f32(1.5f);
  /* Dynamic object. */
  w.u32(77).f32(1).f32(2).f32(3).u8(7).u8(8).u8(9).u32(5).u8(24);
  /* Controller. */
  w.u32(5).i32(1234).str("gate", 128).u8(1).i32(2).i32(3).u8(0).u8(1);
  w.i32(-1).i32(-1).i32(-1).i32(-1).i32(12).i32(0);
  w.i32(2030).i32(0).i32(-1).i32(1).i32(0).i32(0);
  w.i32(90);
  const Bytes file = w.data;

  NpcGenData d;
  std::string err;
  ASSERT_TRUE(parse_npcgen(file, d, &err)) << err;
  ASSERT_EQ(d.areas.size(), 1u);
  EXPECT_EQ(d.areas[0].npc_type, 1);
  EXPECT_EQ(d.areas[0].gen_id, 99u);
  EXPECT_EQ(d.areas[0].controller_id, 5);
  EXPECT_EQ(d.areas[0].max_num, 8);
  EXPECT_FLOAT_EQ(d.areas[0].pos[2], 12.0f);
  ASSERT_EQ(d.areas[0].spawns.size(), 1u);
  EXPECT_EQ(d.areas[0].spawns[0].id, 2109u);
  EXPECT_EQ(d.areas[0].spawns[0].refresh, 300);
  EXPECT_EQ(d.areas[0].spawns[0].dead_time, 5);
  EXPECT_EQ(d.areas[0].spawns[0].refresh_lower, 200);
  ASSERT_EQ(d.resource_areas.size(), 1u);
  EXPECT_EQ(d.resource_areas[0].rad, 6);
  EXPECT_EQ(d.resource_areas[0].max_num, 2);
  EXPECT_EQ(d.resource_areas[0].resources[0].template_id, 3074);
  EXPECT_FLOAT_EQ(d.resource_areas[0].resources[0].height_offset, 1.5f);
  ASSERT_EQ(d.dynamic_objects.size(), 1u);
  EXPECT_EQ(d.dynamic_objects[0].id, 77u);
  EXPECT_EQ(d.dynamic_objects[0].controller_id, 5u);
  EXPECT_EQ(d.dynamic_objects[0].scale, 24);
  ASSERT_EQ(d.controllers.size(), 1u);
  EXPECT_EQ(d.controllers[0].name(), "gate");
  EXPECT_EQ(d.controllers[0].controller_id, 1234);
  EXPECT_EQ(d.controllers[0].active_time.hours, 12);
  EXPECT_EQ(d.controllers[0].stop_time_point.year, 2030);
  EXPECT_EQ(d.controllers[0].active_time_range, 90);
  EXPECT_EQ(d.find_controller(5), &d.controllers[0]);
  EXPECT_EQ(d.find_controller(6), nullptr);
  EXPECT_TRUE(d.trailing.empty());

  Bytes out;
  ASSERT_TRUE(write_npcgen(d, out, &err)) << err;
  EXPECT_EQ(out, file);
}

TEST(angelica_npcgen, edit_and_reparse)
{
  NpcGenData d = build_test_npcgen(11);
  Bytes file;
  std::string err;
  ASSERT_TRUE(write_npcgen(d, file, &err)) << err;
  NpcGenData parsed;
  ASSERT_TRUE(parse_npcgen(file, parsed, &err)) << err;

  parsed.areas[0].pos[0] = -42.5f;
  parsed.areas[0].spawns[1].count = 12;
  parsed.areas[0].spawns.erase(parsed.areas[0].spawns.begin());
  parsed.areas[1].spawns.push_back(NpcGenSpawn{});
  parsed.areas[1].spawns.back().id = 555;
  parsed.resource_areas[0].resources.pop_back();
  ASSERT_TRUE(parsed.controllers[0].set_name("Onda \xC3\xA9", &err)) << err;
  EXPECT_FALSE(parsed.controllers[0].set_name(std::string(200, 'x'), &err));
  parsed.trailing = {1, 2, 3};

  Bytes out;
  ASSERT_TRUE(write_npcgen(parsed, out, &err)) << err;
  NpcGenData again;
  ASSERT_TRUE(parse_npcgen(out, again, &err)) << err;
  expect_npcgen_equal(parsed, again);
  EXPECT_FLOAT_EQ(again.areas[0].pos[0], -42.5f);
  ASSERT_EQ(again.areas[0].spawns.size(), 1u);
  EXPECT_EQ(again.areas[0].spawns[0].count, 12u);
  EXPECT_EQ(again.areas[1].spawns[0].id, 555u);
  EXPECT_EQ(again.resource_count(), 1u);
  EXPECT_EQ(again.controllers[0].name(), "Onda \xC3\xA9");

  /* Features an old version cannot store are refused instead of silently dropped. */
  NpcGenData old = again;
  old.version = 6;
  EXPECT_FALSE(write_npcgen(old, out, &err));
  old.version = 5;
  old.controllers.clear();
  EXPECT_FALSE(write_npcgen(old, out, &err));
  old.version = 0;
  EXPECT_FALSE(write_npcgen(old, out, &err));
}

TEST(angelica_npcgen, rejects_malformed)
{
  const NpcGenData d = build_test_npcgen(11);
  Bytes file;
  std::string err;
  ASSERT_TRUE(write_npcgen(d, file, &err)) << err;
  NpcGenData parsed;
  for (size_t n = 0; n < file.size(); n++) {
    EXPECT_FALSE(parse_npcgen(ByteSpan(file).first(n), parsed, &err)) << n;
  }
  Bytes bad = file;
  bad[0] = 12;
  EXPECT_FALSE(parse_npcgen(bad, parsed, &err));
  bad[0] = 0;
  EXPECT_FALSE(parse_npcgen(bad, parsed, &err));
  /* Huge and negative area counts. */
  bad = file;
  std::memset(bad.data() + 4, 0x7F, 4);
  EXPECT_FALSE(parse_npcgen(bad, parsed, &err));
  std::memset(bad.data() + 4, 0xFF, 4);
  EXPECT_FALSE(parse_npcgen(bad, parsed, &err));
  /* Huge spawn count inside the first area. */
  bad = file;
  std::memset(bad.data() + 20 + 4, 0x7F, 4);
  EXPECT_FALSE(parse_npcgen(bad, parsed, &err));

  std::mt19937 rng(99);
  for (int i = 0; i < 2000; i++) {
    Bytes fuzz = file;
    const int flips = 1 + int(rng() % 6);
    for (int k = 0; k < flips; k++) {
      fuzz[rng() % fuzz.size()] = uint8_t(rng());
    }
    if (parse_npcgen(fuzz, parsed, nullptr)) {
      Bytes out;
      ASSERT_TRUE(write_npcgen(parsed, out, nullptr));
      EXPECT_EQ(out, fuzz);
    }
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Real client data (optional)
 * \{ */

/** Folder of `.cfg` schemas for the real file test, empty when there is none. */
static std::string elements_schema_dir(const std::string &test_data)
{
  if (const char *env = std::getenv("ANGELICA_ELEMENTS_SCHEMAS")) {
    return env;
  }
  for (const char *sub : {"schemas", "configs", "."}) {
    const fs::path dir = fs::path(test_data) / sub;
    if (!list_elements_schemas(dir.string()).empty()) {
      return dir.string();
    }
  }
  return {};
}

static std::string find_child_ci(const fs::path &dir, const std::string &name)
{
  std::error_code ec;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    std::string a = it->path().filename().string(), b = name;
    std::transform(a.begin(), a.end(), a.begin(), ::tolower);
    std::transform(b.begin(), b.end(), b.begin(), ::tolower);
    if (a == b) {
      return it->path().string();
    }
  }
  return {};
}

TEST(angelica_elements, real_elements_data)
{
  const std::string root = test_data_dir();
  if (root.empty()) {
    GTEST_SKIP() << "ANGELICA_TEST_DATA not set";
  }
  const std::string data_dir = find_child_ci(root, "data");
  const std::string path = data_dir.empty() ? std::string() :
                                              find_child_ci(data_dir, "elements.data");
  if (path.empty()) {
    GTEST_SKIP() << "data/elements.data not found in " << root;
  }
  Bytes file;
  ASSERT_TRUE(read_file(path, file));
  uint32_t version = 0;
  std::string err;
  ASSERT_TRUE(peek_elements_version(file, version, &err)) << err;
  const std::string schema_dir = elements_schema_dir(root);
  if (schema_dir.empty()) {
    GTEST_SKIP() << "no .cfg schemas: set ANGELICA_ELEMENTS_SCHEMAS";
  }
  ElementsSchema schema;
  if (!load_elements_schema_for_version(schema_dir, version, schema, &err)) {
    GTEST_SKIP() << err;
  }
  ElementsData data;
  ASSERT_TRUE(parse_elements(file, schema, data, &err)) << err;
  /* The whole file is described by the schema. */
  EXPECT_TRUE(data.trailing.empty());
  size_t records = 0;
  for (const ElementsList &list : data.lists) {
    records += list.size();
  }
  std::cout << "elements.data v" << version << ": " << data.lists.size() << " lists, " << records
            << " records\n";

  Bytes out;
  ASSERT_TRUE(write_elements(data, schema, out, &err)) << err;
  EXPECT_TRUE(out == file) << "round-trip is not byte identical";

  for (const char *list_name : {"NPC_ESSENCE", "MONSTER_ESSENCE", "MINE_ESSENCE"}) {
    const int list = data.list_index(list_name);
    ASSERT_GE(list, 0) << list_name;
    EXPECT_GT(data.lists[list].size(), 0u) << list_name;
    ElementModelInfo info;
    ASSERT_TRUE(element_model_info(data, {list, 0}, info));
    EXPECT_FALSE(info.file_model.empty()) << list_name;
    EXPECT_FALSE(info.name.empty()) << list_name;
    ElementModelInfo found;
    ASSERT_TRUE(find_element_model(data, info.id, found));
  }
  const int talk = schema.talk_proc_index;
  ASSERT_GE(talk, 0);
  std::vector<TalkProc> talks;
  ASSERT_TRUE(parse_talk_procs(data.lists[talk], talks, &err)) << err;
  EXPECT_EQ(talks.size(), data.lists[talk].size());

  /* Edit one monster, write, re-parse: the edit sticks and nothing else changes size. */
  const int monsters = data.list_index("MONSTER_ESSENCE");
  const ElementRef first{monsters, 0};
  const uint32_t id = data.record_id(monsters, 0);
  ASSERT_TRUE(data.set(first, "Name", std::string("GameForge test"), &err)) << err;
  ASSERT_TRUE(data.set(first, "size", 4.5, &err)) << err;
  ASSERT_TRUE(write_elements(data, schema, out, &err)) << err;
  if (!elements_version_is_packed(version)) {
    EXPECT_EQ(out.size(), file.size());
  }
  ElementsData again;
  ASSERT_TRUE(parse_elements(out, schema, again, &err)) << err;
  const ElementRef ref = again.find("MONSTER_ESSENCE", id);
  ASSERT_TRUE(ref.valid());
  EXPECT_EQ(again.get_string(ref, "Name"), "GameForge test");
  EXPECT_FLOAT_EQ(float(again.get_float(ref, "size")), 4.5f);
}

/** \} */

}  // namespace blender::io::angelica::tests
