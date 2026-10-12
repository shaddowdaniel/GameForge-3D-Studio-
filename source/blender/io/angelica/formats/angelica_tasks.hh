/* SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup angelica
 *
 * Reader and writer for the quest database `data\tasks.data` (`ATaskTemplMan::LoadTasksFromPack`)
 * and the two small quest side files `data\task_npc.data` and `userdata\dyn_tasks.data`.
 *
 * `tasks.data` is a pack: a header (`TASK_PACK_HEADER`), one offset per top level task and the
 * task records. A record (`ATaskTempl`) is the fixed data block, the four description texts, five
 * NPC talks and, recursively, the sub tasks. The fixed data layout changes with nearly every
 * client release; pack versions 43 to 241 are described (see `angelica_tasks.cc`).
 *
 * The model keeps every top level task as its raw bytes. #parse_task_record builds an index of
 * every field of one record (name, type, byte range, nesting), which is what the editor shows and
 * edits. Scalar edits patch bytes in place; edits that change sizes (texts, array items, sub
 * tasks) splice the record and parse it again. Unedited tasks are written back byte for byte.
 */

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "angelica_reader.hh"

namespace blender::io::angelica {

/* -------------------------------------------------------------------- */
/** \name tasks.data
 * \{ */

constexpr uint32_t TASK_PACK_MAGIC = 0x93858361u;
/** Lowest and highest pack version with a known record layout. */
constexpr uint32_t TASK_VERSION_MIN = 43;
constexpr uint32_t TASK_VERSION_MAX = 241;

/** Whether the fixed data layout of `version` is known. */
bool task_version_supported(uint32_t version);

struct TasksData {
  /** #TASK_PACK_MAGIC, or 0 in packs written by some editors. */
  uint32_t magic = TASK_PACK_MAGIC;
  uint32_t version = 0;
  /** Extra header word of version 223 and later (meaning unknown, kept verbatim). */
  uint32_t header_extra = 0;
  /** Top level task records, in offset table order. */
  std::vector<Bytes> tasks;
  /**
   * False when the offset table of the source file was not in increasing order: the records are
   * still read correctly, but saving lays them out sequentially, so the file is not byte equal.
   */
  bool sequential_layout = true;
};

bool parse_tasks(ByteSpan data, TasksData &r_data, std::string *r_error);
bool write_tasks(const TasksData &data, Bytes &r_out, std::string *r_error);

/**
 * Index file of client packs split into `tasks.data1..N` (the pack is then stored across the
 * numbered files and `tasks.data` holds this index). Used by some localized clients.
 */
struct TaskPackIndex {
  /** Absent in some protected clients (#parse_task_pack_index detects it). */
  bool has_magic = true;
  uint32_t magic = TASK_PACK_MAGIC;
  uint32_t version = 0;
  uint32_t export_version = 0;
  uint32_t item_count = 0;
  /** MD5 of every `tasks.dataN`, kept verbatim. */
  std::vector<std::array<uint8_t, 16>> pack_md5;
};

/** True when `data` looks like a split pack index rather than a task pack. */
bool is_task_pack_index(ByteSpan data);
bool parse_task_pack_index(ByteSpan data, TaskPackIndex &r_index, std::string *r_error);
void write_task_pack_index(const TaskPackIndex &index, Bytes &r_out);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Task record field index
 * \{ */

enum class TaskFieldType : uint8_t {
  /* Containers: their children are the following fields with #TaskField::parent pointing back. */
  Struct,
  Array,
  Item,
  /* Scalars. */
  Bool,
  U8,
  I16,
  I32,
  U32,
  F32,
  /** Opaque fixed size bytes (combined switches, pointers of old versions...). */
  Bytes,
  /** Fixed size UTF-16 text, XOR'ed with the task id (pack version 52 and later). */
  WCharsXor,
  /** Fixed size UTF-16 text, plain. */
  WChars,
  /** Fixed size 8-bit text (GBK). */
  Chars,
  /** `int32` character count followed by UTF-16 text XOR'ed with the task id. */
  PwStrXor,
};

/** What a container holds. Used to create empty array items. */
enum class TaskKind : uint8_t {
  None,
  Task,
  TaskTm,
  TaskRegion,
  ItemWanted,
  MonsterWanted,
  PlayerWanted,
  TeamMemWanted,
  MonstersContrib,
  ChangeKey,
  ChangeKeyLegacy,
  Expression,
  TaskChar,
  TaskEvent,
  VaguePrompt,
  NpcService,
  TargetTask,
  HomeItemWanted,
  MonsterSummoned,
  AwardData,
  AwardCandItems,
  RankingAward,
  TitleAward,
  FactionTarget,
  DialogText,
  TalkWindow,
  TalkOption,
  SpecialParam,
  I32Value,
  I16Value,
  Other,
};

struct TaskField {
  /** Engine member name (static storage). */
  const char *name = "";
  /** Byte range in the record. For containers, the whole content. */
  uint32_t offset = 0;
  uint32_t size = 0;
  /** Index of the enclosing container, -1 for the root task. */
  int32_t parent = -1;
  /** For #TaskFieldType::Array, the field holding the item count (-1 when the count is fixed). */
  int32_t count_field = -1;
  /** For items, the index in the array. */
  uint32_t index = 0;
  TaskFieldType type = TaskFieldType::Struct;
  TaskKind kind = TaskKind::None;

  bool is_container() const
  {
    return type == TaskFieldType::Struct || type == TaskFieldType::Array ||
           type == TaskFieldType::Item;
  }
};

/** One task or sub task of a record. */
struct TaskNodeInfo {
  /** Index of the task's #TaskField (a Struct of kind Task). */
  int32_t field = -1;
  /** Index of the parent task node, -1 for the top level task. */
  int32_t parent = -1;
  uint32_t id = 0;
  /** Decoded name, UTF-8. */
  std::string name;
  int32_t id_field = -1;
  int32_t name_field = -1;
};

struct TaskParse {
  uint32_t version = 0;
  std::vector<TaskField> fields;
  /** Tasks in depth first order; node 0 is the top level task. */
  std::vector<TaskNodeInfo> nodes;
};

/**
 * Parse one top level task record (with its sub tasks) into a field index. Fails when the record
 * does not end exactly at the end of `blob`, which catches layout mistakes.
 * `fields == false` only fills #TaskParse::nodes (fast, for listing).
 */
bool parse_task_record(
    ByteSpan blob, uint32_t version, TaskParse &r_parse, std::string *r_error, bool fields = true);

/** Id of a task record without parsing it. */
uint32_t task_record_id(ByteSpan blob, uint32_t version);
/** Decoded name of a task record without parsing it. */
std::string task_record_name(ByteSpan blob, uint32_t version);

/** Index of the field `name` that is a direct child of `parent`, -1 when missing. */
int32_t task_find_field(const TaskParse &parse, int32_t parent, std::string_view name);
/** Dotted path of a field, for example `m_Award_S.m_CandItems[0].m_AwardItems[1].m_ulItemNum`. */
std::string task_field_path(const TaskParse &parse, int32_t field);
/** Id of the task that owns `field` (its XOR key). */
uint32_t task_field_owner_id(ByteSpan blob, const TaskParse &parse, int32_t field);

int64_t task_get_int(ByteSpan blob, const TaskParse &parse, int32_t field);
float task_get_float(ByteSpan blob, const TaskParse &parse, int32_t field);
/** Text fields decoded to UTF-8 (XOR removed). */
std::string task_get_text(ByteSpan blob, const TaskParse &parse, int32_t field);

/** Patch an integer or boolean field in place. Fails when the value does not fit the type. */
bool task_set_int(
    Bytes &blob, const TaskParse &parse, int32_t field, int64_t value, std::string *r_error);
bool task_set_float(Bytes &blob, const TaskParse &parse, int32_t field, float value);
/**
 * Replace a text field. Fixed size texts are truncated to their size, length prefixed texts are
 * resized. The record is parsed again (`parse` is updated).
 */
bool task_set_text(
    Bytes &blob, TaskParse &parse, int32_t field, std::string_view utf8, std::string *r_error);
/**
 * Change the id of the task node `node`, re-encrypting all of its texts with the new key.
 * Sub task links (`m_ulParent`, ...) that point at the old id are updated.
 */
bool task_set_id(
    Bytes &blob, TaskParse &parse, int32_t node, uint32_t new_id, std::string *r_error);

/**
 * Insert an item into an array field at `at` (`item` bytes, or an empty item when `item` is
 * empty), incrementing its count field. Arrays sharing the count get an empty item too.
 */
bool task_array_insert(Bytes &blob,
                       TaskParse &parse,
                       int32_t array_field,
                       uint32_t at,
                       ByteSpan item,
                       std::string *r_error);
bool task_array_remove(
    Bytes &blob, TaskParse &parse, int32_t array_field, uint32_t at, std::string *r_error);
/**
 * Rewrite `m_ulParent`, `m_ulPrevSibling`, `m_ulNextSibling` and `m_ulFirstChild` of every task
 * of the record from the sub task nesting. Call after adding or removing sub tasks.
 */
bool task_relink(Bytes &blob, TaskParse &parse, std::string *r_error);

/** An empty (all zero counts) record of `kind` for `version`. */
Bytes task_empty_item(TaskKind kind, uint32_t version);

/**
 * Names of the bits of `CombinedSwitches` (pack version 135 and later), in bit order: bit `i` is
 * `(switches[i / 8] >> (i % 8)) & 1`. Empty before version 135, where the flags are separate
 * `bool` fields.
 */
std::vector<const char *> task_switch_names(uint32_t version);
/** Value of the named switch of a task node, false when the version has no such switch. */
bool task_get_switch(ByteSpan blob, const TaskParse &parse, int32_t node, std::string_view name);
bool task_set_switch(
    Bytes &blob, const TaskParse &parse, int32_t node, std::string_view name, bool value);

/** \} */

/* -------------------------------------------------------------------- */
/** \name task_npc.data
 * \{ */

struct TaskNpcInfo {
  uint32_t id = 0;
  int16_t x = 0;
  int16_t y = 0;
  int16_t z = 0;
  /** Structure padding, kept verbatim. */
  uint16_t pad = 0;
};

struct TaskNpcData {
  /** Written by the tools: header size plus the records. Kept as read. */
  uint32_t pack_size = 0;
  int32_t time_mark = 0;
  uint16_t version = 1;
  std::vector<TaskNpcInfo> npcs;
  Bytes trailing;
};

bool parse_task_npc(ByteSpan data, TaskNpcData &r_data, std::string *r_error);
/** `pack_size` is recomputed when `update_size`, else written as stored. */
void write_task_npc(const TaskNpcData &data, Bytes &r_out, bool update_size = true);

/** \} */

/* -------------------------------------------------------------------- */
/** \name dyn_tasks.data
 * \{ */

/**
 * Dynamic task cache downloaded from the server. Only the header is decoded; the task bodies use
 * the compact network marshalling of the server build and are kept verbatim.
 */
struct DynTasksData {
  uint32_t pack_size = 0;
  int32_t time_mark = 0;
  uint16_t version = 0;
  uint16_t task_count = 0;
  Bytes body;
};

bool parse_dyn_tasks(ByteSpan data, DynTasksData &r_data, std::string *r_error);
void write_dyn_tasks(const DynTasksData &data, Bytes &r_out);

/** \} */

}  // namespace blender::io::angelica
