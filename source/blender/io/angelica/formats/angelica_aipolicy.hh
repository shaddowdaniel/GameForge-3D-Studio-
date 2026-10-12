/* SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup angelica
 *
 * Reader and writer for the monster AI policy database `data\aipolicy.data`
 * (`CPolicyDataManager`), shared by the client and the server.
 *
 * The file is a list of policies (one per monster controller); a policy is a list of triggers; a
 * trigger is a condition tree plus a list of operations, each with a target. Every trigger stores
 * its own version (0 to 23 in known files), which decides which operations carry parameters and
 * the size of a few parameter blocks.
 *
 * Condition and operation parameters are kept as raw bytes: the condition size is stored in the
 * file, the operation sizes come from a table (see #ai_operation_param_size). Talk texts (UTF-16)
 * have helpers. Saving an unchanged file reproduces it byte for byte.
 */

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "angelica_reader.hh"

namespace blender::io::angelica {

/** Version written by the 1.5 server tools (`F_TRIGGER_VERSION`). */
constexpr uint32_t AI_TRIGGER_VERSION_CURRENT = 23;

/** Condition node types (`c_*`). */
enum class AiConditionType : int32_t {
  TimeCome = 0,
  HpLess,
  StartAttack,
  Random,
  KillPlayer,
  Not,
  Or,
  And,
  Died,
  Plus,
  Minus,
  Multiply,
  Divide,
  Great,
  Less,
  Equ,
  Var,
  Constant,
  BeHurt,
  ReachEnd,
  AtHistoryStage,
  HistoryValue,
  StopFight,
  LocalVar,
  ReachEnd2,
  HasFilter,
  RoomIndex,
  PlayerCountInRadius,
  PlayerCountInRegion,
};

/** Operation types (`o_*`). */
enum class AiOperationType : int32_t {
  Attack = 0,
  UseSkill,
  Talk,
  ResetHateList,
  RunTrigger,
  StopTrigger,
  ActiveTrigger,
  CreateTimer,
  KillTimer,
  Flee,
  SetHateToFirst,
  SetHateToLast,
  SetHateFiftyPercent,
  SkipOperation,
  ActiveController,
  SetGlobal,
  ReviseGlobal,
  SummonMonster,
  WalkAlong,
  PlayAction,
  ReviseHistory,
  SetHistory,
  DeliverFactionPvpPoints,
  CalcVar,
  SummonMonster2,
  WalkAlong2,
  UseSkill2,
  ActiveController2,
  DeliverTask,
  SummonMine,
  SummonNpc,
  DeliverRandomTaskInRegion,
  DeliverTaskInHateList,
  ClearTowerTaskInRegion,
  SavePlayerCountInRadius,
  SavePlayerCountInRegion,
};

/** Target types (`t_*`). Only #AI_TARGET_OCCUPATION_LIST carries a parameter. */
constexpr int32_t AI_TARGET_OCCUPATION_LIST = 6;

/** Flags of the serialized condition tree (`CONDITION_*`). */
constexpr int32_t AI_CONDITION_LEFT_SUB = 1;
constexpr int32_t AI_CONDITION_RIGHT_SUB = 2;
constexpr int32_t AI_CONDITION_LEAF = 3;
constexpr int32_t AI_CONDITION_NODE_END = 4;

struct AiCondition {
  int32_t type = 0;
  Bytes param;
  /** For a child: #AI_CONDITION_LEFT_SUB or #AI_CONDITION_RIGHT_SUB. */
  int32_t side = AI_CONDITION_LEFT_SUB;
  /** Sub conditions in file order. */
  std::vector<AiCondition> children;
  /** #AI_CONDITION_LEAF or #AI_CONDITION_NODE_END, as stored. */
  int32_t end_flag = AI_CONDITION_LEAF;

  const AiCondition *left() const;
  const AiCondition *right() const;
};

struct AiOperation {
  int32_t type = 0;
  /**
   * Parameter bytes exactly as stored. For talk operations this is `uint32 size`, the UTF-16
   * text and (trigger version 17 and later) `uint32 append mask`; see #ai_talk_text.
   */
  Bytes param;
  int32_t target_type = 0;
  /** Only stored when `target_type == AI_TARGET_OCCUPATION_LIST` (occupation bit mask). */
  uint32_t target_param = 0;
};

struct AiTrigger {
  uint32_t version = AI_TRIGGER_VERSION_CURRENT;
  uint32_t id = 0;
  /** Booleans, stored as bytes (normally 0 or 1; other values are kept). */
  uint8_t active = 0;
  uint8_t run = 0;
  uint8_t attack_valid = 0;
  /** 128 bytes of GBK name, kept raw (editors leave bytes after the terminator). */
  uint8_t name[128] = {};
  AiCondition condition;
  std::vector<AiOperation> operations;
};

struct AiPolicy {
  uint32_t version = 0;
  uint32_t id = 0;
  std::vector<AiTrigger> triggers;
};

struct AiPolicyData {
  /** `F_POLICY_EXP_VERSION`, 1 in known files. */
  uint32_t version = 1;
  std::vector<AiPolicy> policies;
  /** Bytes after the last policy (none in known files). */
  Bytes trailing;
};

bool parse_aipolicy(ByteSpan data, AiPolicyData &r_data, std::string *r_error);
bool write_aipolicy(const AiPolicyData &data, Bytes &r_out, std::string *r_error);

/**
 * Size of the parameters of operation `type` in a trigger of `trigger_version`: 0 when the
 * operation has none, -1 for an unknown operation, -2 for talk (variable size).
 */
int ai_operation_param_size(int32_t type, uint32_t trigger_version);

/** Engine names of conditions, operations and targets ("c_hp_less"...), "" when unknown. */
const char *ai_condition_name(int32_t type);
const char *ai_operation_name(int32_t type);
const char *ai_target_name(int32_t type);

/** Trigger name decoded from GBK. */
std::string ai_trigger_name(const AiTrigger &trigger);
/** Fails when the GBK encoding does not fit the 128 byte field. */
bool ai_set_trigger_name(AiTrigger &trigger, std::string_view utf8, std::string *r_error);

/** Text of a talk operation, UTF-8. */
std::string ai_talk_text(const AiOperation &op);
uint32_t ai_talk_append_mask(const AiOperation &op);
/** Rebuild a talk operation's parameters (`trigger_version` decides whether the mask is stored).
 */
bool ai_set_talk(AiOperation &op,
                 uint32_t trigger_version,
                 std::string_view utf8,
                 uint32_t append_mask,
                 std::string *r_error);

/** A new operation of `type` with zeroed parameters of the right size. */
bool ai_make_operation(int32_t type,
                       uint32_t trigger_version,
                       AiOperation &r_op,
                       std::string *r_error);

/** \name Typed access to the raw parameter bytes (little endian, bounds checked).
 * \{ */
int32_t ai_param_i32(const Bytes &param, size_t offset);
float ai_param_f32(const Bytes &param, size_t offset);
bool ai_param_set_i32(Bytes &param, size_t offset, int32_t value);
bool ai_param_set_f32(Bytes &param, size_t offset, float value);
/** \} */

}  // namespace blender::io::angelica
