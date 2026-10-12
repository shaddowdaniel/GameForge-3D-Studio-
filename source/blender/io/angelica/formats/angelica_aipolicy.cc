/* SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup angelica
 */

#include <algorithm>
#include <cstring>

#include "angelica_aipolicy.hh"
#include "angelica_elements.hh"

namespace blender::io::angelica {

namespace {

/** Deepest condition tree accepted (real trees are a handful of levels). */
constexpr int MAX_CONDITION_DEPTH = 256;
/** Sanity limit for one condition or talk parameter block. */
constexpr uint32_t MAX_PARAM_SIZE = 1u << 20;

void set_error(std::string *r_error, const std::string &message)
{
  if (r_error) {
    *r_error = message;
  }
}

template<typename T> void put(Bytes &out, const T &value)
{
  const size_t at = out.size();
  out.resize(at + sizeof(T));
  std::memcpy(out.data() + at, &value, sizeof(T));
}

void put_bytes(Bytes &out, ByteSpan bytes)
{
  out.insert(out.end(), bytes.begin(), bytes.end());
}

/* -------------------------------------------------------------------- */
/** \name Tables
 * \{ */

const char *const CONDITION_NAMES[] = {
    "c_time_come",
    "c_hp_less",
    "c_start_attack",
    "c_random",
    "c_kill_player",
    "c_not",
    "c_or",
    "c_and",
    "c_died",
    "c_plus",
    "c_minus",
    "c_multiply",
    "c_divide",
    "c_great",
    "c_less",
    "c_equ",
    "c_var",
    "c_constant",
    "c_be_hurt",
    "c_reach_end",
    "c_at_history_stage",
    "c_history_value",
    "c_stop_fight",
    "c_local_var",
    "c_reach_end_2",
    "c_has_filter",
    "c_room_index",
    "c_player_count_in_radius",
    "c_player_count_in_region",
};

struct OperationInfo {
  const char *name;
  /** Parameter size in current triggers (-2: talk). */
  int size;
};

const OperationInfo OPERATIONS[] = {
    {"o_attack", 4},
    {"o_use_skill", 8},
    {"o_talk", -2},
    {"o_reset_hate_list", 0},
    {"o_run_trigger", 4},
    {"o_stop_trigger", 4},
    {"o_active_trigger", 4},
    {"o_create_timer", 12},
    {"o_kill_timer", 4},
    {"o_flee", 0},
    {"o_set_hate_to_first", 0},
    {"o_set_hate_to_last", 0},
    {"o_set_hate_fifty_percent", 0},
    {"o_skip_operation", 0},
    {"o_active_controller", 8},
    {"o_set_global", 12},
    {"o_revise_global", 8},
    {"o_summon_monster", 24},
    {"o_walk_along", 16},
    {"o_play_action", 140},
    {"o_revise_history", 8},
    {"o_set_history", 12},
    {"o_deliver_faction_pvp_points", 4},
    {"o_calc_var", 28},
    {"o_summon_monster_2", 36},
    {"o_walk_along_2", 20},
    {"o_use_skill_2", 16},
    {"o_active_controller_2", 12},
    {"o_deliver_task", 8},
    {"o_summon_mine", 28},
    {"o_summon_npc", 36},
    {"o_deliver_random_task_in_region", 28},
    {"o_deliver_task_in_hate_list", 16},
    {"o_clear_tower_task_in_region", 24},
    {"o_save_player_count_in_radius_to_param", 16},
    {"o_save_player_count_in_region_to_param", 32},
};

const char *const TARGET_NAMES[] = {
    "t_hate_first",
    "t_hate_second",
    "t_hate_others",
    "t_most_hp",
    "t_most_mp",
    "t_least_hp",
    "t_occupation_list",
    "t_self",
    "t_monster_killer",
    "t_monster_birthplace_faction",
    "t_hate_random_one",
    "t_hate_nearest",
    "t_hate_farthest",
    "t_hate_first_redirected",
};

template<typename T, size_t N> constexpr int32_t table_size(const T (&)[N])
{
  return int32_t(N);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Reading
 * \{ */

bool read_condition(
    BinaryReader &r, AiCondition &node, int depth, uint32_t &r_nodes, std::string *r_error)
{
  if (depth > MAX_CONDITION_DEPTH) {
    set_error(r_error, "condition tree nested too deep");
    return false;
  }
  r_nodes++;
  node.type = r.i32();
  const uint32_t size = r.u32();
  if (r.failed() || size > MAX_PARAM_SIZE || size > r.remaining()) {
    set_error(r_error, "invalid condition parameter size");
    return false;
  }
  ByteSpan param = r.read_span(size);
  node.param.assign(param.begin(), param.end());
  while (true) {
    const int32_t flag = r.i32();
    if (r.failed()) {
      set_error(r_error, "condition tree is truncated");
      return false;
    }
    if (flag == AI_CONDITION_LEFT_SUB || flag == AI_CONDITION_RIGHT_SUB) {
      AiCondition &child = node.children.emplace_back();
      child.side = flag;
      if (!read_condition(r, child, depth + 1, r_nodes, r_error)) {
        return false;
      }
      continue;
    }
    if (flag == AI_CONDITION_LEAF || flag == AI_CONDITION_NODE_END) {
      node.end_flag = flag;
      return true;
    }
    set_error(r_error, "unknown condition tree flag " + std::to_string(flag));
    return false;
  }
}

bool read_operation(BinaryReader &r, uint32_t version, AiOperation &op, std::string *r_error)
{
  op.type = r.i32();
  const int size = ai_operation_param_size(op.type, version);
  if (size == -1) {
    set_error(r_error, "unknown AI operation " + std::to_string(op.type));
    return false;
  }
  const size_t start = r.tell();
  if (size == -2) {
    const uint32_t text_size = r.u32();
    if (r.failed() || text_size > MAX_PARAM_SIZE) {
      set_error(r_error, "invalid talk text size");
      return false;
    }
    r.skip(text_size);
    if (version >= 17) {
      r.skip(4);
    }
  }
  else {
    r.skip(size_t(size));
  }
  if (r.failed()) {
    set_error(r_error, "AI operation is truncated");
    return false;
  }
  const size_t end = r.tell();
  r.seek(start);
  ByteSpan param = r.read_span(end - start);
  op.param.assign(param.begin(), param.end());
  op.target_type = r.i32();
  if (op.target_type == AI_TARGET_OCCUPATION_LIST) {
    op.target_param = r.u32();
  }
  if (r.failed()) {
    set_error(r_error, "AI operation target is truncated");
    return false;
  }
  return true;
}

bool read_trigger(BinaryReader &r, AiTrigger &trigger, std::string *r_error)
{
  trigger.version = r.u32();
  if (trigger.version > 1000) {
    set_error(r_error, "invalid trigger version " + std::to_string(trigger.version));
    return false;
  }
  trigger.id = r.u32();
  trigger.active = r.u8();
  trigger.run = r.u8();
  trigger.attack_valid = r.u8();
  ByteSpan name = r.read_span(sizeof(trigger.name));
  if (r.failed()) {
    set_error(r_error, "trigger is truncated");
    return false;
  }
  std::memcpy(trigger.name, name.data(), sizeof(trigger.name));
  uint32_t nodes = 0;
  if (!read_condition(r, trigger.condition, 0, nodes, r_error)) {
    return false;
  }
  const int32_t op_count = r.i32();
  if (r.failed() || op_count < 0 || size_t(op_count) > r.remaining() / 8) {
    set_error(r_error, "invalid operation count");
    return false;
  }
  trigger.operations.resize(size_t(op_count));
  for (AiOperation &op : trigger.operations) {
    if (!read_operation(r, trigger.version, op, r_error)) {
      return false;
    }
  }
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Writing
 * \{ */

void write_condition(Bytes &out, const AiCondition &node)
{
  put<int32_t>(out, node.type);
  put<uint32_t>(out, uint32_t(node.param.size()));
  put_bytes(out, node.param);
  for (const AiCondition &child : node.children) {
    put<int32_t>(out, child.side);
    write_condition(out, child);
  }
  put<int32_t>(out, node.end_flag);
}

bool check_operation(const AiOperation &op, uint32_t version, std::string *r_error)
{
  const int size = ai_operation_param_size(op.type, version);
  if (size == -1) {
    set_error(r_error, "unknown AI operation " + std::to_string(op.type));
    return false;
  }
  if (size == -2) {
    const size_t tail = version >= 17 ? 4 : 0;
    uint32_t text_size = 0;
    if (op.param.size() >= 4) {
      std::memcpy(&text_size, op.param.data(), 4);
    }
    if (op.param.size() < 4 || op.param.size() != 4 + size_t(text_size) + tail) {
      set_error(r_error, "talk operation parameters do not match their size field");
      return false;
    }
    return true;
  }
  if (op.param.size() != size_t(size)) {
    set_error(r_error,
              std::string("parameters of ") + ai_operation_name(op.type) + " must be " +
                  std::to_string(size) + " bytes");
    return false;
  }
  return true;
}

/** \} */

}  // namespace

const AiCondition *AiCondition::left() const
{
  for (const AiCondition &child : children) {
    if (child.side == AI_CONDITION_LEFT_SUB) {
      return &child;
    }
  }
  return nullptr;
}

const AiCondition *AiCondition::right() const
{
  for (const AiCondition &child : children) {
    if (child.side == AI_CONDITION_RIGHT_SUB) {
      return &child;
    }
  }
  return nullptr;
}

int ai_operation_param_size(int32_t type, uint32_t trigger_version)
{
  if (type < 0 || type >= table_size(OPERATIONS)) {
    return -1;
  }
  const auto op = AiOperationType(type);
  /* Trigger versions 0 to 3 only stored parameters for the operations that existed then. */
  if (trigger_version < 4) {
    switch (op) {
      case AiOperationType::Attack:
      case AiOperationType::UseSkill:
      case AiOperationType::Talk:
      case AiOperationType::RunTrigger:
      case AiOperationType::StopTrigger:
      case AiOperationType::ActiveTrigger:
      case AiOperationType::CreateTimer:
      case AiOperationType::KillTimer:
      case AiOperationType::ActiveController:
        break;
      case AiOperationType::SetGlobal:
      case AiOperationType::ReviseGlobal:
        if (trigger_version < 2) {
          return 0;
        }
        break;
      default:
        return 0;
    }
  }
  switch (op) {
    case AiOperationType::ActiveController:
      return trigger_version < 1 ? 4 : 8;
    case AiOperationType::SetGlobal:
      return trigger_version < 4 ? 8 : 12;
    case AiOperationType::PlayAction:
      return trigger_version < 9 ? 136 : 140;
    default:
      return OPERATIONS[type].size;
  }
}

const char *ai_condition_name(int32_t type)
{
  return (type >= 0 && type < table_size(CONDITION_NAMES)) ? CONDITION_NAMES[type] : "";
}

const char *ai_operation_name(int32_t type)
{
  return (type >= 0 && type < table_size(OPERATIONS)) ? OPERATIONS[type].name : "";
}

const char *ai_target_name(int32_t type)
{
  return (type >= 0 && type < table_size(TARGET_NAMES)) ? TARGET_NAMES[type] : "";
}

bool parse_aipolicy(ByteSpan data, AiPolicyData &r_data, std::string *r_error)
{
  r_data = AiPolicyData();
  BinaryReader r(data);
  r_data.version = r.u32();
  const int32_t count = r.i32();
  if (r.failed() || count < 0 || size_t(count) > r.remaining() / 12) {
    set_error(r_error, "not an aipolicy.data file (invalid header)");
    return false;
  }
  r_data.policies.resize(size_t(count));
  for (size_t i = 0; i < r_data.policies.size(); i++) {
    AiPolicy &policy = r_data.policies[i];
    policy.version = r.u32();
    policy.id = r.u32();
    const int32_t triggers = r.i32();
    if (r.failed() || triggers < 0 || size_t(triggers) > r.remaining() / 140) {
      set_error(r_error, "policy " + std::to_string(i) + ": invalid trigger count");
      return false;
    }
    policy.triggers.resize(size_t(triggers));
    for (size_t t = 0; t < policy.triggers.size(); t++) {
      std::string err;
      if (!read_trigger(r, policy.triggers[t], &err)) {
        set_error(r_error,
                  "policy " + std::to_string(policy.id) + " trigger " + std::to_string(t) + ": " +
                      err);
        return false;
      }
    }
  }
  ByteSpan rest = r.read_span(r.remaining());
  r_data.trailing.assign(rest.begin(), rest.end());
  return true;
}

bool write_aipolicy(const AiPolicyData &data, Bytes &r_out, std::string *r_error)
{
  r_out.clear();
  put<uint32_t>(r_out, data.version);
  put<int32_t>(r_out, int32_t(data.policies.size()));
  for (const AiPolicy &policy : data.policies) {
    put<uint32_t>(r_out, policy.version);
    put<uint32_t>(r_out, policy.id);
    put<int32_t>(r_out, int32_t(policy.triggers.size()));
    for (const AiTrigger &trigger : policy.triggers) {
      put<uint32_t>(r_out, trigger.version);
      put<uint32_t>(r_out, trigger.id);
      put<uint8_t>(r_out, trigger.active);
      put<uint8_t>(r_out, trigger.run);
      put<uint8_t>(r_out, trigger.attack_valid);
      put_bytes(r_out, ByteSpan(trigger.name, sizeof(trigger.name)));
      write_condition(r_out, trigger.condition);
      put<int32_t>(r_out, int32_t(trigger.operations.size()));
      for (const AiOperation &op : trigger.operations) {
        if (!check_operation(op, trigger.version, r_error)) {
          return false;
        }
        put<int32_t>(r_out, op.type);
        put_bytes(r_out, op.param);
        put<int32_t>(r_out, op.target_type);
        if (op.target_type == AI_TARGET_OCCUPATION_LIST) {
          put<uint32_t>(r_out, op.target_param);
        }
      }
    }
  }
  put_bytes(r_out, data.trailing);
  return true;
}

std::string ai_trigger_name(const AiTrigger &trigger)
{
  size_t len = 0;
  while (len < sizeof(trigger.name) && trigger.name[len] != 0) {
    len++;
  }
  return gbk_to_utf8(std::string_view(reinterpret_cast<const char *>(trigger.name), len));
}

bool ai_set_trigger_name(AiTrigger &trigger, std::string_view utf8, std::string *r_error)
{
  if (ai_trigger_name(trigger) == utf8) {
    return true;
  }
  std::string gbk;
  if (!utf8_to_gbk(utf8, gbk)) {
    set_error(r_error, "trigger name cannot be encoded as GBK");
    return false;
  }
  if (gbk.size() >= sizeof(trigger.name)) {
    set_error(r_error, "trigger name is too long");
    return false;
  }
  std::memset(trigger.name, 0, sizeof(trigger.name));
  std::memcpy(trigger.name, gbk.data(), gbk.size());
  return true;
}

std::string ai_talk_text(const AiOperation &op)
{
  if (op.type != int32_t(AiOperationType::Talk) || op.param.size() < 4) {
    return {};
  }
  uint32_t size = 0;
  std::memcpy(&size, op.param.data(), 4);
  size = std::min<uint32_t>(size, uint32_t(op.param.size() - 4));
  ByteSpan text(op.param.data() + 4, size & ~1u);
  /* Stop at the terminator. */
  size_t len = 0;
  while (len + 1 < text.size() && (text[len] | text[len + 1]) != 0) {
    len += 2;
  }
  return utf16le_to_utf8(text.first(len));
}

uint32_t ai_talk_append_mask(const AiOperation &op)
{
  if (op.type != int32_t(AiOperationType::Talk) || op.param.size() < 8) {
    return 0;
  }
  uint32_t size = 0;
  std::memcpy(&size, op.param.data(), 4);
  if (size_t(size) + 8 != op.param.size()) {
    return 0;
  }
  uint32_t mask = 0;
  std::memcpy(&mask, op.param.data() + 4 + size, 4);
  return mask;
}

bool ai_set_talk(AiOperation &op,
                 uint32_t trigger_version,
                 std::string_view utf8,
                 uint32_t append_mask,
                 std::string *r_error)
{
  Bytes text;
  if (!utf8_to_utf16le(utf8, text)) {
    set_error(r_error, "invalid UTF-8 text");
    return false;
  }
  /* The engine stores the terminator too. */
  text.push_back(0);
  text.push_back(0);
  op.type = int32_t(AiOperationType::Talk);
  op.param.clear();
  put<uint32_t>(op.param, uint32_t(text.size()));
  put_bytes(op.param, text);
  if (trigger_version >= 17) {
    put<uint32_t>(op.param, append_mask);
  }
  return true;
}

bool ai_make_operation(int32_t type,
                       uint32_t trigger_version,
                       AiOperation &r_op,
                       std::string *r_error)
{
  const int size = ai_operation_param_size(type, trigger_version);
  if (size == -1) {
    set_error(r_error, "unknown AI operation " + std::to_string(type));
    return false;
  }
  r_op = AiOperation();
  r_op.type = type;
  if (size == -2) {
    return ai_set_talk(r_op, trigger_version, "", 0, r_error);
  }
  r_op.param.assign(size_t(size), 0);
  return true;
}

int32_t ai_param_i32(const Bytes &param, size_t offset)
{
  int32_t value = 0;
  if (offset + 4 <= param.size()) {
    std::memcpy(&value, param.data() + offset, 4);
  }
  return value;
}

float ai_param_f32(const Bytes &param, size_t offset)
{
  float value = 0.0f;
  if (offset + 4 <= param.size()) {
    std::memcpy(&value, param.data() + offset, 4);
  }
  return value;
}

bool ai_param_set_i32(Bytes &param, size_t offset, int32_t value)
{
  if (offset + 4 > param.size()) {
    return false;
  }
  std::memcpy(param.data() + offset, &value, 4);
  return true;
}

bool ai_param_set_f32(Bytes &param, size_t offset, float value)
{
  if (offset + 4 > param.size()) {
    return false;
  }
  std::memcpy(param.data() + offset, &value, 4);
  return true;
}

}  // namespace blender::io::angelica
