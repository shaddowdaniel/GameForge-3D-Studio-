/* SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup angelica
 *
 * Tests for the client data files other than `elements.data` and `npcgen.data`: quests
 * (`tasks.data`, `task_npc.data`, `dyn_tasks.data`), `aipolicy.data`, the shops (`gshop*.data`),
 * the war maps (`domain*.data`), the path tables (`path.data`, `DynamicObjects.data`) and the
 * precinct files. Files are synthesized in memory.
 *
 * When the environment variable `ANGELICA_TEST_DATA` points at a client `element` folder, the
 * files found in its `data` folder are additionally parsed and written back byte for byte.
 */

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#include "testing/testing.h"

#include "../formats/angelica_aipolicy.hh"
#include "../formats/angelica_domain.hh"
#include "../formats/angelica_gshop.hh"
#include "../formats/angelica_pathmap.hh"
#include "../formats/angelica_precinct.hh"
#include "../formats/angelica_tasks.hh"

namespace blender::io::angelica::tests {

namespace {

namespace fs = std::filesystem;

template<typename T> void put(Bytes &out, const T &value)
{
  const uint8_t *p = reinterpret_cast<const uint8_t *>(&value);
  out.insert(out.end(), p, p + sizeof(T));
}

/** Folder named by `ANGELICA_TEST_DATA` joined with `relpath`, empty when missing. */
std::string real_file(const char *relpath)
{
  const char *env = std::getenv("ANGELICA_TEST_DATA");
  if (env == nullptr || env[0] == '\0') {
    return {};
  }
  const fs::path path = fs::path(env) / relpath;
  std::error_code ec;
  return fs::is_regular_file(path, ec) ? path.string() : std::string();
}

/* -------------------------------------------------------------------- */
/** \name Tasks
 * \{ */

const uint32_t TASK_TEST_VERSIONS[] = {43, 52, 80, 100, 125, 128, 135, 153, 176, 198, 223, 241};

/** A pack with two empty top level tasks (ids 100 and 101) of `version`. */
TasksData make_tasks(uint32_t version)
{
  TasksData data;
  data.version = version;
  for (uint32_t id : {100u, 101u}) {
    Bytes task = task_empty_item(TaskKind::Task, version);
    TaskParse parse;
    EXPECT_TRUE(parse_task_record(task, version, parse, nullptr));
    EXPECT_TRUE(task_set_id(task, parse, 0, id, nullptr));
    data.tasks.push_back(task);
  }
  return data;
}

TEST(angelica_tasks, empty_tasks_roundtrip_all_versions)
{
  for (uint32_t version : TASK_TEST_VERSIONS) {
    SCOPED_TRACE(version);
    TasksData data = make_tasks(version);
    Bytes file;
    std::string err;
    ASSERT_TRUE(write_tasks(data, file, &err)) << err;
    TasksData back;
    ASSERT_TRUE(parse_tasks(file, back, &err)) << err;
    EXPECT_EQ(back.version, version);
    ASSERT_EQ(back.tasks.size(), 2u);
    EXPECT_EQ(back.tasks, data.tasks);
    EXPECT_EQ(task_record_id(back.tasks[1], version), 101u);
    Bytes again;
    ASSERT_TRUE(write_tasks(back, again, &err));
    EXPECT_EQ(again, file);
  }
}

TEST(angelica_tasks, edit_text_scalars_and_arrays)
{
  for (uint32_t version : TASK_TEST_VERSIONS) {
    SCOPED_TRACE(version);
    Bytes task = task_empty_item(TaskKind::Task, version);
    TaskParse parse;
    std::string err;
    ASSERT_TRUE(parse_task_record(task, version, parse, &err)) << err;
    ASSERT_TRUE(task_set_id(task, parse, 0, 4242, &err)) << err;
    const Bytes original = task;

    /* Fixed size name (XOR'ed with the id from version 52). */
    const int32_t name = parse.nodes[0].name_field;
    ASSERT_GE(name, 0);
    ASSERT_TRUE(task_set_text(task, parse, name, "Quest çãé", &err)) << err;
    EXPECT_EQ(task_get_text(task, parse, name), "Quest çãé");
    EXPECT_EQ(task_record_name(task, version), "Quest çãé");
    EXPECT_EQ(parse.nodes[0].name, "Quest çãé");

    /* Length prefixed description: the record grows and is parsed again. */
    const int32_t root = parse.nodes[0].field;
    int32_t desc = task_find_field(parse, root, "m_pwstrDescript");
    ASSERT_GE(desc, 0);
    ASSERT_TRUE(task_set_text(task, parse, desc, "Go and talk to the elder.", &err)) << err;
    desc = task_find_field(parse, root, "m_pwstrDescript");
    EXPECT_EQ(task_get_text(task, parse, desc), "Go and talk to the elder.");

    /* Scalar patch. */
    const int32_t limit = task_find_field(parse, root, "m_ulTimeLimit");
    ASSERT_GE(limit, 0);
    ASSERT_TRUE(task_set_int(task, parse, limit, 3600, &err)) << err;
    EXPECT_EQ(task_get_int(task, parse, limit), 3600);

    /* Array insert and remove. */
    const int32_t wanted = task_find_field(parse, root, "m_ItemsWanted");
    ASSERT_GE(wanted, 0);
    ASSERT_TRUE(task_array_insert(task, parse, wanted, 0, {}, &err)) << err;
    const int32_t wanted2 = task_find_field(parse, root, "m_ItemsWanted");
    EXPECT_EQ(task_get_int(task, parse, parse.fields[wanted2].count_field), 1);
    ASSERT_TRUE(task_array_remove(task, parse, wanted2, 0, &err)) << err;

    /* Sub task insertion and relinking. */
    const int32_t subs = task_find_field(parse, root, "m_SubTasks");
    ASSERT_GE(subs, 0);
    ASSERT_TRUE(task_array_insert(task, parse, subs, 0, {}, &err)) << err;
    ASSERT_EQ(parse.nodes.size(), 2u);
    ASSERT_TRUE(task_set_id(task, parse, 1, 4243, &err)) << err;
    ASSERT_TRUE(task_relink(task, parse, &err)) << err;
    const int32_t parent_link = task_find_field(parse, parse.nodes[1].field, "m_ulParent");
    ASSERT_GE(parent_link, 0);
    EXPECT_EQ(task_get_int(task, parse, parent_link), 4242);

    /* The record still parses exactly. */
    TaskParse check;
    EXPECT_TRUE(parse_task_record(task, version, check, &err)) << err;
    EXPECT_NE(task, original);
  }
}

TEST(angelica_tasks, switches)
{
  EXPECT_TRUE(task_switch_names(128).empty());
  for (uint32_t version : {135u, 200u, 241u}) {
    SCOPED_TRACE(version);
    const std::vector<const char *> names = task_switch_names(version);
    ASSERT_FALSE(names.empty());
    Bytes task = task_empty_item(TaskKind::Task, version);
    TaskParse parse;
    ASSERT_TRUE(parse_task_record(task, version, parse, nullptr));
    for (const char *name : {names.front(), names.back()}) {
      EXPECT_FALSE(task_get_switch(task, parse, 0, name));
      EXPECT_TRUE(task_set_switch(task, parse, 0, name, true));
      EXPECT_TRUE(task_get_switch(task, parse, 0, name));
    }
    EXPECT_FALSE(task_set_switch(task, parse, 0, "no_such_switch", true));
  }
}

TEST(angelica_tasks, rejects_malformed)
{
  std::string err;
  TasksData data;
  EXPECT_FALSE(parse_tasks(Bytes{1, 2, 3}, data, &err));
  Bytes header;
  put<uint32_t>(header, TASK_PACK_MAGIC);
  put<uint32_t>(header, 128);
  put<uint32_t>(header, 5); /* Five tasks, no offsets. */
  EXPECT_FALSE(parse_tasks(header, data, &err));

  Bytes task = task_empty_item(TaskKind::Task, 128);
  TaskParse parse;
  task.push_back(0);
  EXPECT_FALSE(parse_task_record(task, 128, parse, &err));
  task.resize(task.size() - 9);
  EXPECT_FALSE(parse_task_record(task, 128, parse, &err));
  EXPECT_FALSE(parse_task_record(task, 7, parse, &err));
}

TEST(angelica_tasks, pack_index)
{
  TaskPackIndex index;
  index.version = 241;
  index.export_version = 3;
  index.item_count = 1234;
  index.pack_md5.resize(3);
  index.pack_md5[1][5] = 0xAB;
  Bytes file;
  write_task_pack_index(index, file);
  EXPECT_TRUE(is_task_pack_index(file));
  TaskPackIndex back;
  std::string err;
  ASSERT_TRUE(parse_task_pack_index(file, back, &err)) << err;
  EXPECT_EQ(back.item_count, 1234u);
  EXPECT_EQ(back.pack_md5, index.pack_md5);
  Bytes again;
  write_task_pack_index(back, again);
  EXPECT_EQ(again, file);
}

TEST(angelica_tasks, task_npc_and_dyn_tasks)
{
  TaskNpcData npcs;
  npcs.time_mark = 77;
  npcs.npcs.push_back({1001, -10, 20, 300, 0});
  npcs.npcs.push_back({1002, 1, 2, 3, 0xCCCC});
  Bytes file;
  write_task_npc(npcs, file);
  TaskNpcData back;
  std::string err;
  ASSERT_TRUE(parse_task_npc(file, back, &err)) << err;
  ASSERT_EQ(back.npcs.size(), 2u);
  EXPECT_EQ(back.npcs[0].x, -10);
  EXPECT_EQ(back.npcs[1].pad, 0xCCCC);
  EXPECT_EQ(back.pack_size, file.size());
  Bytes again;
  write_task_npc(back, again, false);
  EXPECT_EQ(again, file);

  DynTasksData dyn;
  dyn.time_mark = 5;
  dyn.version = 10;
  dyn.task_count = 2;
  dyn.body = {1, 2, 3, 4, 5};
  write_dyn_tasks(dyn, file);
  DynTasksData dyn_back;
  ASSERT_TRUE(parse_dyn_tasks(file, dyn_back, &err)) << err;
  EXPECT_EQ(dyn_back.body, dyn.body);
  write_dyn_tasks(dyn_back, again);
  EXPECT_EQ(again, file);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name AI policy
 * \{ */

AiPolicyData make_policies()
{
  AiPolicyData data;
  AiPolicy &policy = data.policies.emplace_back();
  policy.version = 1;
  policy.id = 77;

  AiTrigger &t = policy.triggers.emplace_back();
  t.version = AI_TRIGGER_VERSION_CURRENT;
  t.id = 1;
  t.active = 1;
  EXPECT_TRUE(ai_set_trigger_name(t, "start", nullptr));
  /* (hp_less 50%) and not(random) */
  t.condition.type = int32_t(AiConditionType::And);
  t.condition.end_flag = AI_CONDITION_NODE_END;
  AiCondition &left = t.condition.children.emplace_back();
  left.side = AI_CONDITION_LEFT_SUB;
  left.type = int32_t(AiConditionType::HpLess);
  left.param = {0, 0, 0, 0x3F};
  AiCondition &right = t.condition.children.emplace_back();
  right.side = AI_CONDITION_RIGHT_SUB;
  right.type = int32_t(AiConditionType::Not);
  right.end_flag = AI_CONDITION_NODE_END;
  AiCondition &inner = right.children.emplace_back();
  inner.type = int32_t(AiConditionType::Random);
  inner.param = {1, 2, 3, 4};

  AiOperation talk;
  EXPECT_TRUE(ai_set_talk(talk, t.version, "Olá!", 3, nullptr));
  t.operations.push_back(talk);
  AiOperation skill;
  EXPECT_TRUE(ai_make_operation(int32_t(AiOperationType::UseSkill2), t.version, skill, nullptr));
  ai_param_set_i32(skill.param, 0, 1234);
  skill.target_type = AI_TARGET_OCCUPATION_LIST;
  skill.target_param = 0x15;
  t.operations.push_back(skill);

  /* Old trigger: set_global has no parameters before version 2. */
  AiTrigger &old = policy.triggers.emplace_back();
  old.version = 1;
  old.id = 2;
  AiOperation global;
  global.type = int32_t(AiOperationType::SetGlobal);
  old.operations.push_back(global);
  AiOperation controller;
  EXPECT_TRUE(ai_make_operation(
      int32_t(AiOperationType::ActiveController), old.version, controller, nullptr));
  EXPECT_EQ(controller.param.size(), 8u);
  old.operations.push_back(controller);
  return data;
}

TEST(angelica_aipolicy, roundtrip_and_helpers)
{
  const AiPolicyData data = make_policies();
  Bytes file;
  std::string err;
  ASSERT_TRUE(write_aipolicy(data, file, &err)) << err;
  AiPolicyData back;
  ASSERT_TRUE(parse_aipolicy(file, back, &err)) << err;
  ASSERT_EQ(back.policies.size(), 1u);
  const AiTrigger &t = back.policies[0].triggers[0];
  EXPECT_EQ(ai_trigger_name(t), "start");
  ASSERT_NE(t.condition.right(), nullptr);
  EXPECT_EQ(t.condition.right()->children.size(), 1u);
  EXPECT_EQ(t.condition.left()->param.size(), 4u);
  EXPECT_EQ(ai_talk_text(t.operations[0]), "Olá!");
  EXPECT_EQ(ai_talk_append_mask(t.operations[0]), 3u);
  EXPECT_EQ(ai_param_i32(t.operations[1].param, 0), 1234);
  EXPECT_EQ(t.operations[1].target_param, 0x15u);
  EXPECT_TRUE(back.policies[0].triggers[1].operations[0].param.empty());
  Bytes again;
  ASSERT_TRUE(write_aipolicy(back, again, &err));
  EXPECT_EQ(again, file);

  EXPECT_EQ(ai_operation_param_size(int32_t(AiOperationType::PlayAction), 8), 136);
  EXPECT_EQ(ai_operation_param_size(int32_t(AiOperationType::PlayAction), 23), 140);
  EXPECT_EQ(ai_operation_param_size(int32_t(AiOperationType::SummonMonster), 3), 0);
  EXPECT_EQ(ai_operation_param_size(int32_t(AiOperationType::SetGlobal), 3), 8);
  EXPECT_EQ(ai_operation_param_size(999, 23), -1);
  EXPECT_STREQ(ai_condition_name(int32_t(AiConditionType::HpLess)), "c_hp_less");
}

TEST(angelica_aipolicy, rejects_malformed)
{
  AiPolicyData data = make_policies();
  Bytes file;
  std::string err;
  ASSERT_TRUE(write_aipolicy(data, file, &err));
  AiPolicyData back;
  for (size_t cut : {size_t(3), size_t(20), file.size() / 2, file.size() - 1}) {
    Bytes truncated(file.begin(), file.begin() + cut);
    EXPECT_FALSE(parse_aipolicy(truncated, back, &err)) << cut;
  }
  /* Wrong parameter size is refused on save. */
  data.policies[0].triggers[0].operations[1].param.pop_back();
  EXPECT_FALSE(write_aipolicy(data, file, &err));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Shop
 * \{ */

GShopData make_shop(GShopVersion version, int items)
{
  GShopData data;
  data.version = version;
  data.timestamp = 0x5000;
  for (int i = 0; i < items; i++) {
    GShopItem &item = data.items.emplace_back();
    item.id = i;
    item.category = i % 3;
    item.sub_category = 1;
    item.item_id = 1000 + uint32_t(i);
    item.options[0].price = 500;
    EXPECT_TRUE(gshop_set_utf16_text(item.name.data(), item.name.size(), "Poção", nullptr));
    EXPECT_TRUE(
        gshop_set_gbk_text(item.icon.data(), item.icon.size(), "Surfaces\\icon.dds", nullptr));
  }
  data.categories.resize(GSHOP_CATEGORIES);
  gshop_set_utf16_text(
      data.categories[0].name.data(), data.categories[0].name.size(), "Especial", nullptr);
  data.categories[0].sub_categories.resize(2);
  return data;
}

TEST(angelica_gshop, roundtrip_and_detection_all_layouts)
{
  for (int v = int(GShopVersion::PW_1_2_6); v <= int(GShopVersion::V10); v++) {
    const GShopVersion version = GShopVersion(v);
    SCOPED_TRACE(gshop_version_name(version));
    const GShopData data = make_shop(version, 3);
    Bytes file;
    std::string err;
    ASSERT_TRUE(write_gshop(data, file, &err)) << err;
    EXPECT_EQ(file.size(), 8 + 3 * gshop_item_size(version) + 8 * 132 + 2 * 128);
    GShopData back;
    ASSERT_TRUE(parse_gshop(file, back, &err)) << err;
    EXPECT_EQ(back.version, version);
    EXPECT_EQ(gshop_utf16_text(back.items[2].name), "Poção");
    EXPECT_EQ(gshop_gbk_text(back.items[2].icon), "Surfaces\\icon.dds");
    EXPECT_EQ(gshop_utf16_text(back.categories[0].name), "Especial");
    Bytes again;
    ASSERT_TRUE(write_gshop(back, again, &err));
    EXPECT_EQ(again, file);
  }
}

TEST(angelica_gshop, trailing_and_errors)
{
  GShopData data = make_shop(GShopVersion::PW_1_5_5_v156, 2);
  data.trailing = {9, 9, 9};
  Bytes file;
  std::string err;
  ASSERT_TRUE(write_gshop(data, file, &err));
  GShopData back;
  ASSERT_TRUE(parse_gshop(file, back, &err, GShopVersion::PW_1_5_5_v156)) << err;
  EXPECT_EQ(back.trailing, data.trailing);
  EXPECT_FALSE(parse_gshop(Bytes{1, 0, 0}, back, &err));
  GShopItem item;
  EXPECT_FALSE(
      gshop_set_utf16_text(item.name.data(), item.name.size(), std::string(40, 'x'), &err));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Domains
 * \{ */

DomainData make_domains(DomainFormat format)
{
  DomainData data;
  data.format = format;
  data.timestamp = 1234;
  data.speed = 3.0f;
  for (int i = 0; i < 2; i++) {
    DomainInfo &d = data.domains.emplace_back();
    EXPECT_TRUE(domain_set_name(d, i == 0 ? "Terra" : "Céu", nullptr));
    d.id = i + 1;
    d.type = 1;
    d.reward = 10;
    d.point = 2;
    d.map_pos[4] = 1.5f;
    d.center = {10, 20};
    d.verts = {{0, 0}, {10, 0}, {0, 10}};
    d.faces = {0, 1, 2};
    d.neighbours = {i == 0 ? 2 : 1};
  }
  data.times = {{0, 20, 0}, {6, 21, 30}};
  data.time_max = 11;
  return data;
}

TEST(angelica_domain, roundtrip_variants)
{
  struct Case {
    DomainFormat format;
    bool magic;
    bool timestamp;
    bool extra_table;
  };
  const Case cases[] = {
      {DomainFormat::Domain, false, false, false},
      {DomainFormat::Domain, true, false, true},
      {DomainFormat::Domain2, false, true, false},
      {DomainFormat::Domain2, false, false, false},
  };
  for (const Case &c : cases) {
    DomainData data = make_domains(c.format);
    data.has_magic = c.magic;
    data.has_timestamp = c.timestamp;
    if (c.extra_table) {
      data.extra_time_tables.push_back({{{5, 20, 0}}, 11});
    }
    Bytes file;
    std::string err;
    ASSERT_TRUE(write_domain(data, file, &err)) << err;
    DomainData back;
    ASSERT_TRUE(parse_domain(file, back, &err, c.format)) << err;
    EXPECT_EQ(back.has_magic, c.magic);
    EXPECT_EQ(back.domains.size(), 2u);
    EXPECT_EQ(domain_name(back.domains[1]), "Céu");
    EXPECT_EQ(back.extra_time_tables.size(), c.extra_table ? 1u : 0u);
    EXPECT_TRUE(back.trailing.empty());
    if (c.format == DomainFormat::Domain2) {
      EXPECT_EQ(back.has_timestamp, c.timestamp);
      EXPECT_EQ(back.speed, 3.0f);
    }
    Bytes again;
    ASSERT_TRUE(write_domain(back, again, &err));
    EXPECT_EQ(again, file);
  }
  EXPECT_EQ(domain_format_from_filename("data\\domain2_cross.data"), DomainFormat::Domain2);
  EXPECT_EQ(domain_format_from_filename("DOMAIN.data"), DomainFormat::Domain);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Path tables
 * \{ */

TEST(angelica_pathmap, roundtrip_and_lookup)
{
  PathMapData data;
  data.entries.push_back({1, "models\\npcs\\a.ecm"});
  data.entries.push_back({7, "Models\\Matter\\B.ecm"});
  Bytes file;
  std::string err;
  ASSERT_TRUE(write_pathmap(data, file, &err)) << err;
  PathMapData back;
  ASSERT_TRUE(parse_pathmap(file, back, &err)) << err;
  EXPECT_TRUE(pathmap_is_engine_readable(back));
  EXPECT_EQ(pathmap_find_id(back, 7), 1);
  EXPECT_EQ(pathmap_find_path(back, "models/matter/b.ecm"), 1);
  EXPECT_EQ(pathmap_next_id(back), 8u);
  ASSERT_TRUE(pathmap_set_path(back.entries[0], "models\\npcs\\c.ecm", &err));
  EXPECT_EQ(pathmap_path_utf8(back.entries[0]), "models\\npcs\\c.ecm");
  back.entries.push_back({7, "dup"});
  EXPECT_FALSE(write_pathmap(back, file, &err));
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Precincts
 * \{ */

Bytes utf16_file(const std::string &text)
{
  Bytes out = {0xFF, 0xFE};
  for (unsigned char c : text) {
    out.push_back(c);
    out.push_back(0);
  }
  return out;
}

TEST(angelica_precinct, text_roundtrip_and_edit)
{
  const std::string text =
      "// exported\r\n"
      "version  7\r\n"
      "1600000000\r\n"
      "\r\n"
      "\"Archosaur\"\r\n"
      "12  3  1  5  1  1  30  0  1  0  1\r\n"
      "100.5, 220, -30\r\n"
      "0, 0, 0\r\n"
      "10, 0, 0\r\n"
      "10, 0, 10\r\n"
      "\"Gate\"  1, 2, 3\r\n"
      "\"amb.mp3\"\r\n"
      "\"music\\a.mp3\"\r\n"
      "\"amb_n.mp3\"\r\n";
  const Bytes file = utf16_file(text);
  PrecinctData data;
  std::string err;
  ASSERT_TRUE(parse_precinct(file, data, &err)) << err;
  EXPECT_EQ(data.format, PrecinctFormat::Text);
  EXPECT_EQ(data.version, 7u);
  EXPECT_EQ(data.timestamp, 1600000000u);
  ASSERT_EQ(data.precincts.size(), 1u);
  const Precinct &p = data.precincts[0];
  EXPECT_EQ(p.name, "Archosaur");
  EXPECT_EQ(p.id, 12u);
  EXPECT_EQ(p.points.size(), 3u);
  EXPECT_EQ(p.marks[0].name, "Gate");
  EXPECT_EQ(p.music_files[0], "music\\a.mp3");
  EXPECT_EQ(p.sound_night, "amb_n.mp3");
  EXPECT_EQ(p.pk_protect, 1);
  EXPECT_FLOAT_EQ(p.city_pos[0], 100.5f);

  /* Unchanged: identical bytes, comments included. */
  Bytes out;
  ASSERT_TRUE(write_precinct(data, out, &err)) << err;
  EXPECT_EQ(out, file);

  /* Edited: a clean file that parses to the same model. */
  data.precincts[0].name = "Arquossauro";
  data.precincts[0].points.push_back({1.25f, 0.0f, -7.5f});
  ASSERT_TRUE(write_precinct(data, out, &err)) << err;
  PrecinctData back;
  ASSERT_TRUE(parse_precinct(out, back, &err)) << err;
  EXPECT_EQ(back.precincts, data.precincts);
}

TEST(angelica_precinct, binary_roundtrip)
{
  PrecinctData data;
  data.format = PrecinctFormat::Binary;
  data.version = 7;
  data.timestamp = 99;
  Precinct &p = data.precincts.emplace_back();
  p.priority = 2;
  p.dst_instance = 1;
  p.domain_id = 4;
  p.pk_protect = 1;
  p.points = {{0, 0, 0}, {5, 0, 0}, {5, 0, 5}};
  Bytes file;
  std::string err;
  ASSERT_TRUE(write_precinct(data, file, &err)) << err;
  PrecinctData back;
  ASSERT_TRUE(parse_precinct(file, back, &err)) << err;
  EXPECT_EQ(back.format, PrecinctFormat::Binary);
  EXPECT_EQ(back.precincts[0].points.size(), 3u);
  Bytes again;
  ASSERT_TRUE(write_precinct(back, again, &err));
  EXPECT_EQ(again, file);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Real files (optional)
 * \{ */

TEST(angelica_game_data, real_files_roundtrip)
{
  bool any = false;
  std::string err;
  Bytes data, out;
  if (const std::string path = real_file("data/tasks.data"); !path.empty()) {
    any = true;
    ASSERT_TRUE(read_file(path, data));
    TasksData tasks;
    ASSERT_TRUE(parse_tasks(data, tasks, &err)) << err;
    if (task_version_supported(tasks.version)) {
      for (size_t i = 0; i < tasks.tasks.size(); i++) {
        TaskParse parse;
        ASSERT_TRUE(parse_task_record(tasks.tasks[i], tasks.version, parse, &err, false))
            << "task " << i << ": " << err;
      }
    }
    ASSERT_TRUE(write_tasks(tasks, out, &err));
    if (tasks.sequential_layout) {
      EXPECT_TRUE(out == data);
    }
  }
  if (const std::string path = real_file("data/task_npc.data"); !path.empty()) {
    any = true;
    ASSERT_TRUE(read_file(path, data));
    TaskNpcData npcs;
    ASSERT_TRUE(parse_task_npc(data, npcs, &err)) << err;
    write_task_npc(npcs, out, false);
    EXPECT_TRUE(out == data);
  }
  if (const std::string path = real_file("data/aipolicy.data"); !path.empty()) {
    any = true;
    ASSERT_TRUE(read_file(path, data));
    AiPolicyData policies;
    ASSERT_TRUE(parse_aipolicy(data, policies, &err)) << err;
    ASSERT_TRUE(write_aipolicy(policies, out, &err)) << err;
    EXPECT_TRUE(out == data);
  }
  for (const char *name : {"data/gshop.data", "data/gshop1.data", "data/gshop2.data"}) {
    if (const std::string path = real_file(name); !path.empty()) {
      any = true;
      ASSERT_TRUE(read_file(path, data));
      GShopData shop;
      ASSERT_TRUE(parse_gshop(data, shop, &err)) << name << ": " << err;
      ASSERT_TRUE(write_gshop(shop, out, &err));
      EXPECT_TRUE(out == data) << name;
    }
  }
  for (const char *name : {"data/domain.data", "data/domain2.data", "data/domain2_cross.data"}) {
    if (const std::string path = real_file(name); !path.empty()) {
      any = true;
      ASSERT_TRUE(read_file(path, data));
      DomainData domains;
      ASSERT_TRUE(parse_domain(data, domains, &err, domain_format_from_filename(name)))
          << name << ": " << err;
      ASSERT_TRUE(write_domain(domains, out, &err));
      EXPECT_TRUE(out == data) << name;
    }
  }
  if (!any) {
    GTEST_SKIP() << "set ANGELICA_TEST_DATA to a client element folder";
  }
}

/** \} */

}  // namespace

}  // namespace blender::io::angelica::tests
