/* SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup angelica
 *
 * `tasks.data` layout
 * ===================
 *
 * Pack: `uint32 magic, version, count`, an extra `uint32` from version 223, `count` record
 * offsets (`int32`, from the file start) and the records.
 *
 * Record (`ATaskTempl`): the fixed data, `pwstr` description / OK / No / tribute texts, five talk
 * procedures (deliver, unqualified, deliver item, execute, award), `int32` sub task count and the
 * sub task records. Texts are UTF-16 XOR'ed with the low 16 bits of the task id from version 52.
 *
 * The fixed data is the engine's `ATaskTemplFixedData` written member by member, followed by its
 * variable parts (timetable, change keys, expressions, regions, wanted items...) and the award
 * blocks. Members appear, disappear and change type between versions; #read_fixed_data follows
 * the order of the engine structure and gates every member on the version that introduced or
 * removed it. Up to version 175 the structure still holds 32-bit pointer members, which are kept
 * as opaque `int32` values. From version 135 most `bool` members are packed into the
 * `CombinedSwitches` bit field.
 *
 * Instead of mapping the hundreds of members to C++ fields, the reader records every member it
 * consumes as a #TaskField (name, type, byte range, nesting). Editing works on the raw record
 * bytes through that index, so anything the editor does not touch stays byte identical.
 */

#include <algorithm>
#include <climits>
#include <cstring>

#include "angelica_elements.hh"
#include "angelica_tasks.hh"

namespace blender::io::angelica {

namespace {

void set_error(std::string *r_error, const std::string &message)
{
  if (r_error) {
    *r_error = message;
  }
}

template<typename T> void put(Bytes &out, const T v)
{
  const size_t at = out.size();
  out.resize(at + sizeof(T));
  std::memcpy(out.data() + at, &v, sizeof(T));
}

template<typename T> T get_at(ByteSpan data, size_t offset)
{
  T v{};
  if (offset + sizeof(T) <= data.size()) {
    std::memcpy(&v, data.data() + offset, sizeof(T));
  }
  return v;
}

template<typename T> void set_at(Bytes &data, size_t offset, const T v)
{
  if (offset + sizeof(T) <= data.size()) {
    std::memcpy(data.data() + offset, &v, sizeof(T));
  }
}

/* -------------------------------------------------------------------- */
/** \name Version dependent sizes
 * \{ */

int max_timetable(uint32_t v)
{
  return v < 69 ? 12 : 24;
}
int max_prem_tasks(uint32_t v)
{
  return v < 90 ? 5 : 20;
}
int max_occupations(uint32_t v)
{
  if (v < 89) {
    return 8;
  }
  if (v < 120) {
    return 10;
  }
  if (v < 147) {
    return 12;
  }
  if (v < 187) {
    return 14;
  }
  if (v < 195) {
    return 15;
  }
  if (v < 216) {
    return 16;
  }
  if (v < 240) {
    return 17;
  }
  return 18;
}
int max_mutex_tasks(uint32_t v)
{
  return v < 215 ? 5 : 20;
}
int max_award_scales(uint32_t v)
{
  return v < 140 ? 5 : 6;
}

/**
 * Bits of `CombinedSwitches`, in the order the engine assigns them. A switch exists in versions
 * `[min, max]` (0 meaning unbounded); the bit index of a switch is the number of existing switches
 * before it.
 */
struct SwitchInfo {
  const char *name;
  uint16_t min;
  uint16_t max;
};

constexpr SwitchInfo SWITCHES[] = {
    {"ShowByTransformed", 0, 0},
    {"ShowByLev", 0, 0},
    {"PremCheckReincarnation", 0, 0},
    {"ShowByReincarnation", 0, 0},
    {"PremCheckRealmLevel", 0, 0},
    {"PremCheckRealmExpFull", 0, 0},
    {"CheckTradeTime", 232, 0},
    {"ShowByTradeTime", 232, 0},
    {"ShowByRealmLevel", 0, 0},
    {"ShowByItems", 0, 0},
    {"PremItemsAnyOne", 0, 0},
    {"MethodItemsRandOne", 215, 0},
    {"NotOpenWithItem", 0, 0},
    {"ShowByDeposit", 0, 0},
    {"ShowByRepu", 0, 0},
    {"ShowByPreTask", 0, 0},
    {"ShowByPeriod", 0, 0},
    {"ShowByFaction", 0, 0},
    {"ShowByGender", 0, 0},
    {"ShowByOccup", 0, 0},
    {"Premise_Spouse", 0, 0},
    {"ShowBySpouse", 0, 0},
    {"PremiseWeddingOwner", 0, 0},
    {"ShowByWeddingOwner", 0, 0},
    {"GM", 0, 0},
    {"ShieldUser", 0, 0},
    {"ShowByRMB", 0, 0},
    {"CharTime", 0, 0},
    {"ShowByCharTime", 0, 0},
    {"Teamwork", 0, 0},
    {"RcvByTeam", 0, 0},
    {"SharedTask", 0, 0},
    {"SharedAchieved", 0, 0},
    {"CheckTeammate", 0, 0},
    {"AllFail", 0, 0},
    {"CapFail", 0, 0},
    {"CapSucc", 0, 0},
    {"DismAsSelfFail", 0, 0},
    {"RcvChckMem", 0, 0},
    {"CntByMemPos", 0, 0},
    {"AllSucc", 0, 0},
    {"CoupleOnly", 0, 0},
    {"DistinguishedOcc", 0, 0},
    {"ShowByTeam", 0, 0},
    {"PremNeedComp", 0, 0},
    {"PremNeedCompPerson", 178, 0},
    {"PremNeedCompHistory", 185, 0},
    {"PremCheckForce", 0, 0},
    {"ShowByForce", 0, 0},
    {"ShowByForceReputation", 0, 0},
    {"ShowByForceContribution", 0, 0},
    {"ShowByForceExp", 0, 0},
    {"ShowByForceSP", 0, 0},
    {"ShowByForceActivityLevel", 0, 0},
    {"PremIsKing", 0, 0},
    {"ShowByKing", 0, 0},
    {"PremNotInTeam", 0, 0},
    {"ShowByNotInTeam", 0, 0},
    {"ShowByTitle", 0, 0},
    {"ShowByHistoryStage", 0, 0},
    {"ShowByGeneralCard", 0, 0},
    {"ShowByGeneralCardRank", 0, 0},
    {"ShowByIconStateID", 0, 0},
    {"ShowByVIPLevel", 0, 0},
    {"PremNoHome", 0, 0},
    {"ShowByHomeLevel", 0, 0},
    {"ShowByHomeResourceLevel", 0, 0},
    {"ShowByHomeFactoryLevel", 0, 0},
    {"ShowByHomeFlourish", 0, 0},
    {"PremHomeStorageTask", 0, 0},
    {"ShowByHomeStorageTask", 0, 0},
    {"MonsterTotalCount", 0, 0},
    {"NotShowGfx", 195, 0},
    {"FinNeedComp", 0, 0},
    {"FinNeedCompPerson", 178, 0},
    {"FinNeedCompHistory", 185, 0},
    {"IsLibraryTask", 0, 0},
    {"IsUniqueStorageTask", 0, 0},
    {"HasSign", 0, 0},
    {"OfflineFail", 0, 0},
    {"ItemNotTakeOff", 0, 0},
    {"AbsTime", 0, 0},
    {"MultiNpcTalk", 180, 0},
    {"ChooseOne", 0, 0},
    {"RandOne", 0, 0},
    {"ExeChildInOrder", 0, 0},
    {"ParentAlsoFail", 0, 0},
    {"ParentAlsoSucc", 0, 0},
    {"CanGiveUp", 0, 0},
    {"CanRedo", 0, 0},
    {"CanRedoAfterFailure", 0, 0},
    {"ClearAsGiveUp", 0, 0},
    {"NeedRecord", 0, 0},
    {"FailAsPlayerDie", 0, 0},
    {"DelvInZone", 0, 0},
    {"EnterRegionFail", 0, 0},
    {"LeaveRegionFail", 0, 0},
    {"LeaveForceFail", 0, 0},
    {"TransTo", 0, 0},
    {"TrigCtrl", 0, 0},
    {"AutoDeliver", 0, 0},
    {"DisplayInExclusiveUI", 0, 0},
    {"ReadyToNotifyServer", 0, 0},
    {"UsedInTokenShop", 0, 0},
    {"DeathTrig", 0, 0},
    {"ClearAcquired", 0, 0},
    {"ShowPrompt", 0, 0},
    {"KeyTask", 0, 0},
    {"SkillTask", 0, 0},
    {"CanSeekOut", 0, 0},
    {"ShowDirection", 0, 0},
    {"Marriage", 0, 0},
    {"SwitchSceneFail", 0, 0},
    {"Hidden", 0, 0},
    {"DeliverySkill", 0, 0},
    {"ShowGfxFinished", 0, 0},
    {"ChangePQRanking", 0, 0},
    {"CompareItemAndInventory", 0, 0},
    {"TowerTask", 0, 0},
    {"HomeTask", 0, 0},
    {"DeliverInHostHome", 0, 0},
    {"FinishInHostHome", 0, 0},
    {"PQTask", 0, 0},
    {"PQSubTask", 0, 0},
    {"ClearContrib", 0, 0},
    {"ShowByNeedRecordTasksNum", 0, 0},
    {"ShowByFactionContrib", 0, 0},
    {"AccountTaskLimit", 0, 0},
    {"RoleTaskLimit", 0, 0},
    {"LeaveFactionFail", 0, 0},
    {"NotIncCntWhenFailed", 0, 0},
    {"NotClearItemWhenFailed", 0, 0},
    {"DisplayInTitleTaskUI", 0, 0},
    {"CanAwardOnCarrier", 0, 0},
    {"PremReachArenaWinCount", 137, 0},
    {"ShowByReachArenaWinCount", 137, 0},
    {"WriteSpecialLog", 140, 0},
    {"ShowByFactionScore", 146, 0},
    {"ShowByFactionMoney", 146, 0},
    {"ShowByFactionPersonScore", 146, 0},
    {"CheckFactionMember", 146, 0},
    {"PremFaction", 146, 0},
    {"SpecialLogic", 146, 0},
    {"BecomeMentor", 153, 0},
    {"ShowMentorMorality", 153, 0},
    {"ShowMentorStudentValue", 153, 0},
    {"DiscardedTask", 153, 0},
    {"NoCheckTitleGot", 153, 0},
    {"ShowCoupleIntimacy", 156, 0},
    {"ShowDeposit2", 158, 0},
    {"CrossServer", 194, 0},
    {"RecordFnshCnt", 163, 0},
    {"RecordReset", 198, 0},
    {"ShowCamp", 167, 0},
    {"ShowCampPower", 167, 0},
    {"ReturnCampPower", 167, 0},
    {"TeamHaveRecaller", 167, 0},
    {"BindedRecaller", 167, 0},
    {"ShowNeedFriendPoint", 167, 0},
    {"HistoryVarTask", 175, 0},
    {"ShowMethod", 194, 0},
};

bool switch_exists(const SwitchInfo &s, uint32_t v)
{
  if (s.min != 0 && v < s.min) {
    return false;
  }
  if (s.max != 0 && v > s.max) {
    return false;
  }
  return true;
}

/** Bit index of `name`, -1 when the version has no such switch. */
int switch_bit(uint32_t v, std::string_view name)
{
  int bit = 0;
  for (const SwitchInfo &s : SWITCHES) {
    if (!switch_exists(s, v)) {
      continue;
    }
    if (name == s.name) {
      return bit;
    }
    bit++;
  }
  return -1;
}

int switch_bytes(uint32_t v)
{
  int count = 0;
  for (const SwitchInfo &s : SWITCHES) {
    count += switch_exists(s, v) ? 1 : 0;
  }
  return (count + 7) / 8;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Recording reader
 * \{ */

/** A count read from the file together with the field that holds it. */
struct Count {
  int64_t n = 0;
  int32_t field = -1;
};

class Ctx {
 public:
  BinaryReader r;
  uint32_t v;
  std::vector<TaskField> *fields;
  std::vector<TaskNodeInfo> *nodes;
  int32_t cur = -1;
  int32_t last = -1;
  int32_t cur_node = -1;
  bool bad = false;
  std::string error;

  Ctx(ByteSpan data, uint32_t version, std::vector<TaskField> *f, std::vector<TaskNodeInfo> *n)
      : r(data), v(version), fields(f), nodes(n)
  {
  }

  bool ok() const
  {
    return !bad && !r.failed();
  }

  void fail(const std::string &message)
  {
    if (!bad) {
      bad = true;
      error = message;
    }
  }

  int32_t add(const char *name, TaskFieldType type, size_t offset, size_t size)
  {
    if (!fields) {
      last = -1;
      return -1;
    }
    TaskField f;
    f.name = name;
    f.type = type;
    f.offset = uint32_t(offset);
    f.size = uint32_t(size);
    f.parent = cur;
    fields->push_back(f);
    last = int32_t(fields->size() - 1);
    return last;
  }

  template<typename T> T scalar(const char *name, TaskFieldType type)
  {
    const size_t at = r.tell();
    const T value = r.read<T>();
    add(name, type, at, sizeof(T));
    return value;
  }

  bool b(const char *name)
  {
    return scalar<uint8_t>(name, TaskFieldType::Bool) != 0;
  }
  uint8_t u8(const char *name)
  {
    return scalar<uint8_t>(name, TaskFieldType::U8);
  }
  int16_t i16(const char *name)
  {
    return scalar<int16_t>(name, TaskFieldType::I16);
  }
  int32_t i32(const char *name)
  {
    return scalar<int32_t>(name, TaskFieldType::I32);
  }
  uint32_t u32(const char *name)
  {
    return scalar<uint32_t>(name, TaskFieldType::U32);
  }
  float f32(const char *name)
  {
    return scalar<float>(name, TaskFieldType::F32);
  }
  /** Engine pointer member (meaningless on disk, kept verbatim). Named `<member>_ptr`. */
  void ptr(const char *name)
  {
    scalar<int32_t>(name, TaskFieldType::I32);
  }

  Count count_i32(const char *name)
  {
    Count c;
    c.n = i32(name);
    c.field = last;
    return c;
  }
  Count count_u32(const char *name)
  {
    Count c;
    c.n = u32(name);
    c.field = last;
    return c;
  }
  Count count_i16(const char *name)
  {
    Count c;
    c.n = i16(name);
    c.field = last;
    return c;
  }

  void bytes(const char *name, size_t n)
  {
    const size_t at = r.tell();
    r.skip(n);
    add(name, TaskFieldType::Bytes, at, n);
  }
  /** Fixed UTF-16 text. Texts are XOR'ed with the task id from version 52. */
  void wchars(const char *name, size_t n, bool encrypted)
  {
    const size_t at = r.tell();
    r.skip(n * 2);
    add(name,
        (encrypted && v >= 52) ? TaskFieldType::WCharsXor : TaskFieldType::WChars,
        at,
        n * 2);
  }
  void chars(const char *name, size_t n)
  {
    const size_t at = r.tell();
    r.skip(n);
    add(name, TaskFieldType::Chars, at, n);
  }
  /** `int32` character count and UTF-16 characters. A negative count stores no characters. */
  void pwstr(const char *name)
  {
    const size_t at = r.tell();
    const int32_t len = r.i32();
    if (len > 0) {
      if (size_t(len) * 2 > r.remaining()) {
        fail(std::string("text length out of range in ") + name);
        return;
      }
      r.skip(size_t(len) * 2);
    }
    add(name, TaskFieldType::PwStrXor, at, r.tell() - at);
  }

  int32_t open(const char *name, TaskFieldType type, TaskKind kind, uint32_t index = 0)
  {
    const int32_t idx = add(name, type, r.tell(), 0);
    if (idx >= 0) {
      (*fields)[idx].kind = kind;
      (*fields)[idx].index = index;
      cur = idx;
    }
    return idx;
  }

  void close(int32_t idx)
  {
    if (idx < 0) {
      return;
    }
    TaskField &f = (*fields)[idx];
    f.size = uint32_t(r.tell() - f.offset);
    cur = f.parent;
    last = idx;
  }

  /** Validate a count before looping over it. */
  bool check_count(const Count &count, const char *name, size_t min_item_size = 1)
  {
    if (count.n < 0 || uint64_t(count.n) * min_item_size > r.remaining()) {
      fail(std::string("invalid count for ") + name);
      return false;
    }
    return true;
  }

  template<typename F> void structure(const char *name, TaskKind kind, F &&body)
  {
    const int32_t s = open(name, TaskFieldType::Struct, kind);
    body();
    close(s);
  }

  /** Variable array with its count in a field read earlier. */
  template<typename F>
  void array(const char *name, TaskKind kind, const Count &count, size_t min_item_size, F &&item)
  {
    if (!check_count(count, name, min_item_size)) {
      return;
    }
    const int32_t a = open(name, TaskFieldType::Array, kind);
    if (a >= 0) {
      (*fields)[a].count_field = count.field;
    }
    for (int64_t i = 0; i < count.n && ok(); i++) {
      const int32_t it = open(name, TaskFieldType::Item, kind, uint32_t(i));
      item();
      close(it);
    }
    close(a);
  }

  /** Fixed size array of scalars or structures. */
  template<typename F> void fixed(const char *name, TaskKind kind, int n, F &&item)
  {
    const int32_t a = open(name, TaskFieldType::Array, kind);
    for (int i = 0; i < n && ok(); i++) {
      const int32_t it = open(name, TaskFieldType::Item, kind, uint32_t(i));
      item();
      close(it);
    }
    close(a);
  }

  void fixed_i32(const char *name, int n)
  {
    fixed(name, TaskKind::I32Value, n, [&]() { i32(name); });
  }
  void fixed_i16(const char *name, int n)
  {
    fixed(name, TaskKind::I16Value, n, [&]() { i16(name); });
  }
};

/* Small structures. */

void read_tm(Ctx &c, const char *name)
{
  c.structure(name, TaskKind::TaskTm, [&]() {
    c.i32("year");
    c.i32("month");
    c.i32("day");
    c.i32("hour");
    c.i32("min");
    c.i32("wday");
  });
}

void read_vert(Ctx &c, const char *name)
{
  c.structure(name, TaskKind::Other, [&]() {
    c.f32("x");
    c.f32("y");
    c.f32("z");
  });
}

void read_region_body(Ctx &c)
{
  if (c.v >= 157) {
    c.i32("MapID");
  }
  if (c.v >= 191) {
    c.i32("RegionID");
  }
  read_vert(c, "zvMin");
  read_vert(c, "zvMax");
}

void read_region(Ctx &c, const char *name)
{
  c.structure(name, TaskKind::TaskRegion, [&]() { read_region_body(c); });
}

void read_compare(Ctx &c, const char *name)
{
  c.structure(name, TaskKind::Other, [&]() {
    c.i32("nLeftType");
    c.i32("lLeftNum");
    c.i32("nCompOper");
    c.i32("nRightType");
    c.i32("lRightNum");
  });
}

void read_item_wanted(Ctx &c)
{
  c.i32("m_ulItemTemplId");
  c.b("m_bCommonItem");
  c.i32("m_ulItemNum");
  c.f32("m_fProb");
  if (c.v >= 60) {
    c.i32("m_lPeriod");
  }
  if (c.v >= 234) {
    c.u32("m_ulServerLimit");
  }
}

void read_monster_wanted(Ctx &c)
{
  c.i32("m_ulMonsterTemplId");
  c.i32("m_ulMonsterNum");
  c.i32("m_ulDropItemId");
  c.i32("m_ulDropItemCount");
  c.b("m_bDropCmnItem");
  c.f32("m_fDropProb");
  c.u8("m_bKillerLev");
  if (c.v >= 92) {
    c.i32("m_iDPH");
    c.i32("m_iDPS");
  }
  if (c.v >= 226) {
    c.i32("m_iWorldLevel");
  }
}

void read_player_wanted(Ctx &c)
{
  c.i32("m_ulPlayerNum");
  c.i32("m_ulDropItemId");
  c.i32("m_ulDropItemCount");
  c.b("m_bDropCmnItem");
  c.f32("m_fDropProb");
  c.structure("m_Requirements", TaskKind::Other, [&]() {
    c.i32("m_ulOccupations");
    c.i32("m_iMinLevel");
    c.i32("m_iMaxLevel");
    c.i32("m_iGender");
    c.i32("m_iForce");
    if (c.v >= 130) {
      c.b("ModelCheck");
      c.fixed("ModelType", TaskKind::Other, 3, [&]() { c.u8("ModelType"); });
    }
  });
}

void read_team_mem_wanted(Ctx &c)
{
  c.i32("m_ulLevelMin");
  c.i32("m_ulLevelMax");
  c.i32("m_ulRace");
  c.i32("m_ulOccupation");
  c.i32("m_ulGender");
  c.i32("m_ulMinCount");
  c.i32("m_ulMaxCount");
  c.i32("m_ulTask");
  if (c.v >= 97) {
    c.i32("m_iForce");
  }
  if (c.v >= 153) {
    c.i16("TeamMemTaskSpace");
  }
}

void read_monsters_contrib(Ctx &c)
{
  c.i32("m_ulMonsterTemplId");
  c.i32("m_iWholeContrib");
  c.i32("m_iShareContrib");
  if (c.v >= 82) {
    c.i32("m_iPersonalWholeContrib");
  }
}

void read_change_key(Ctx &c)
{
  c.i32("key");
  c.i32("value");
  if (c.v >= 215) {
    c.i32("value2");
  }
  c.u8("type");
}

void read_expression(Ctx &c)
{
  c.i32("type");
  c.i32("value");
}

/** An expression: its source text and the compiled form (64 tokens). */
void read_expression_group(Ctx &c)
{
  c.chars("m_pszExp", 64);
  c.fixed("m_pExpArr", TaskKind::Expression, 64, [&]() { read_expression(c); });
}

void read_task_event(Ctx &c)
{
  if (c.v >= 153) {
    c.i16("IdSpace");
  }
  c.i32("Id");
  c.f32("Value");
}

void read_monster_summoned(Ctx &c)
{
  c.i32("m_ulMonsterTemplId");
  c.i32("m_ulMonsterNum");
  c.f32("m_fSummonProb");
  c.i32("m_lPeriod");
}

void read_ranking_award(Ctx &c)
{
  c.i32("m_iRankingStart");
  c.i32("m_iRankingEnd");
  c.b("m_bCommonItem");
  c.i32("m_ulAwardItemId");
  c.i32("m_ulAwardItemNum");
  c.i32("m_lPeriod");
}

void read_title_award(Ctx &c)
{
  c.i32("m_ulTitleID");
  c.i32("m_lPeriod");
  if (c.v >= 234) {
    c.u32("m_ulServerLimit");
  }
}

void read_award_cand_items(Ctx &c)
{
  c.b("m_bRandChoose");
  const Count n = c.count_i32("m_ulAwardItems");
  c.array("m_AwardItems", TaskKind::ItemWanted, n, 13, [&]() { read_item_wanted(c); });
}

/** Changed key block of an award: key, value, [value2], type. */
void read_change_keys(Ctx &c, const char *name, const Count &n)
{
  c.array(name, TaskKind::ChangeKey, n, 9, [&]() { read_change_key(c); });
}

void read_award_data(Ctx &c);

void read_award_list(Ctx &c, const Count &n)
{
  c.array("m_Awards", TaskKind::AwardData, n, 16, [&]() { read_award_data(c); });
}

void read_award_data(Ctx &c)
{
  const uint32_t v = c.v;
  /* Basic. */
  c.i32("m_ulGoldNum");
  c.i32("m_ulExp");
  if (v >= 113) {
    c.i32("m_ulRealmExp");
    if (v >= 198) {
      c.u32("m_ulPanguExp");
    }
    if (v >= 225) {
      c.u32("m_ulPanguExpYear");
    }
    c.b("m_bExpandRealmLevelMax");
  }
  if (v >= 153) {
    c.i32("NewTaskSpace");
  }
  c.i32("m_ulNewTask");
  c.i32("m_ulSP");
  c.i32("m_lReputation");
  c.i32("m_ulNewPeriod");
  c.i32("m_ulNewRelayStation");
  /* Storage. */
  c.i32("m_ulStorehouseSize");
  if (v >= 57) {
    c.i32("m_ulStorehouseSize2");
    c.i32("m_ulStorehouseSize3");
  }
  if (v >= 75) {
    c.i32("m_ulStorehouseSize4");
  }
  if (v >= 52) {
    c.i32("m_lInventorySize");
    c.i32("m_ulPetInventorySize");
  }
  /* Control. */
  c.i32("m_ulFuryULimit");
  c.i32("m_ulTransWldId");
  if (v >= 161) {
    c.i32("m_ulCrossServerWldId");
  }
  read_vert(c, "m_TransPt");
  if (v >= 52) {
    c.i32("m_lMonsCtrl");
    c.b("m_bTrigCtrl");
  }
  if (v >= 54) {
    c.b("m_bUseLevCo");
  }
  if (v >= 55) {
    c.b("m_bDivorce");
  }
  if (v >= 56) {
    c.b("m_bSendMsg");
    c.i32("m_nMsgChannel");
  }
  /* Items and monsters. */
  const Count cand = c.count_i32("m_ulCandItems");
  if (v < 176) {
    c.ptr("m_CandItems_ptr");
  }
  Count summoned;
  if (v >= 64) {
    summoned = c.count_i32("m_ulSummonedMonsters");
    if (v < 176) {
      c.ptr("m_SummonedMonsters_ptr");
    }
  }
  if (v >= 69) {
    c.b("m_bAwardDeath");
    c.b("m_bAwardDeathWithLoss");
  }
  /* Skill and contribution. */
  if (v >= 75) {
    c.i32("m_ulDividend");
  }
  if (v >= 225) {
    c.u8("m_bRepairShentongSearchBug");
  }
  if (v >= 78) {
    c.b("m_bAwardSkill");
    c.i32("m_iAwardSkillID");
    c.i32("m_iAwardSkillLevel");
  }
  if (v >= 124) {
    c.i32("m_iSoloTowerChallengeScore");
  }
  if (v >= 153) {
    c.i32("m_iOpenSystem");
  }
  if (v >= 75) {
    c.i32("m_ulSpecifyContribTaskID");
  }
  if (v >= 78) {
    c.i32("m_ulSpecifyContribSubTaskID");
  }
  if (v >= 75) {
    c.i32("m_ulSpecifyContrib");
    c.i32("m_ulContrib");
    c.i32("m_ulRandContrib");
    c.i32("m_ulLowestcontrib");
  }
  if (v >= 87) {
    c.i32("m_iFactionContrib");
  }
  if (v >= 89) {
    c.i32("m_iFactionExpContrib");
  }
  /* Change keys. */
  Count ranking, change, person, carrier, history;
  if (v >= 75) {
    ranking = c.count_i32("m_ulPQRankingAwardCnt");
    if (v < 176) {
      c.ptr("m_PQRankingAward_ptr");
    }
  }
  if (v >= 59) {
    change = c.count_i32("m_ulChangeKeyCnt");
    if (v < 176) {
      c.ptr("m_plChangeKey_ptr");
      c.ptr("m_plChangeKeyValue_ptr");
      c.ptr("m_pbChangeType_ptr");
    }
  }
  if (v >= 178) {
    person = c.count_i32("m_ulPersonChangeKeyCnt");
  }
  if (v >= 186) {
    carrier = c.count_i32("m_ulCarrierChangeCnt");
  }
  if (v >= 112) {
    history = c.count_i32("m_ulHistoryChangeCnt");
    if (v < 176) {
      c.ptr("m_plHistoryChangeKey_ptr");
      c.ptr("m_plHistoryChangeKeyValue_ptr");
      c.ptr("m_pbHistoryChangeType_ptr");
    }
  }
  /* Display and expressions. */
  Count display, exp, task_char;
  if (v >= 59) {
    c.b("m_bMulti");
    c.i32("m_nNumType");
    c.i32("m_lNum");
    display = c.count_i32("m_ulDisplayKeyCnt");
    if (v < 176) {
      c.ptr("m_plDisplayKey_ptr");
    }
  }
  if (v >= 63) {
    exp = c.count_i32("m_ulExpCnt");
    if (v < 176) {
      c.ptr("m_pszExp_ptr");
      c.ptr("m_pExpArr_ptr");
    }
    task_char = c.count_i32("m_ulTaskCharCnt");
    if (v < 176) {
      c.ptr("m_pTaskChar_ptr");
    }
  }
  /* Force and titles. */
  Count titles;
  if (v >= 97) {
    c.i32("m_iForceContribution");
    c.i32("m_iForceReputation");
    c.i32("m_iForceActivity");
  }
  if (v >= 100) {
    c.i32("m_iForceSetRepu");
  }
  if (v >= 101) {
    c.i32("m_iTaskLimit");
  }
  if (v >= 109) {
    titles = c.count_i32("m_ulTitleNum");
    if (v < 176) {
      c.ptr("m_pTitleAward_ptr");
    }
  }
  if (v >= 115) {
    c.i32("m_iLeaderShip");
  }
  if (v >= 119) {
    c.i32("m_iWorldContribution");
  }
  /* Home and faction. */
  Count faction_targets;
  if (v >= 126) {
    c.fixed_i32("m_iHomeResource", 5);
    c.b("m_bCreateHome");
  }
  if (v >= 135) {
    c.b("m_bLeaveCarrier");
  }
  if (v >= 146) {
    c.i32("m_faction_money");
    c.i32("m_faction_person_score");
    faction_targets = c.count_i32("m_usFactionTargetCount");
    if (v < 176) {
      c.ptr("AwardFactionTargetData_ptr");
    }
    c.b("m_bSubmitRankScore");
    c.structure("submit_rank", TaskKind::Other, [&]() {
      c.i32("m_iSubmitType");
      c.i32("m_iRankID");
      c.i32("m_iTempRankID");
    });
  }
  if (v >= 156) {
    c.i32("m_iMentorStudentValue");
    c.i32("m_iCoupleIntimacy");
  }
  /* Later additions. */
  int64_t group_num = 0, num_per_group = 0;
  if (v >= 162) {
    c.i32("m_ulRedBookExp");
  }
  if (v >= 167) {
    c.i32("m_iCampScore");
    c.i32("m_iCrossServerMoney");
    c.i32("m_iInviterAwardItem");
    c.i32("m_iRewardFriendPoint");
  }
  if (v >= 175) {
    c.i32("m_iInviterAwardNum");
  }
  if (v >= 180) {
    c.i32("m_iPerceptionVal");
  }
  if (v >= 194) {
    group_num = c.i32("m_uiGroupNum");
    num_per_group = c.i32("m_uiNumPerGroup");
  }
  if (v >= 216) {
    c.i32("m_iNewPlayerEvent");
  }
  if (v >= 232) {
    c.i32("m_iFactionAwardConfig");
  }
  if (v >= 237) {
    c.b("m_iAwardRedBookKey");
    c.i32("m_iAwardRedBookKeyType");
    c.i32("m_iAwardRedBookKeyCount");
  }
  if (v >= 240) {
    c.i32("m_iAwardCameraMovementID");
  }

  /* Variable parts. */
  c.array("m_CandItems", TaskKind::AwardCandItems, cand, 5, [&]() { read_award_cand_items(c); });
  if (v >= 64 && summoned.n > 0) {
    c.structure("m_SummonedMonsters", TaskKind::Other, [&]() {
      c.b("m_bRandChoose");
      c.i32("m_ulSummonRadius");
      c.b("m_bDeathDisappear");
      c.array("m_Monsters", TaskKind::MonsterSummoned, summoned, 16, [&]() {
        read_monster_summoned(c);
      });
    });
  }
  if (v >= 75 && ranking.n > 0) {
    c.structure("m_PQRankingAward", TaskKind::Other, [&]() {
      c.b("m_bAwardByProf");
      c.array(
          "m_RankingAward", TaskKind::RankingAward, ranking, 21, [&]() { read_ranking_award(c); });
    });
  }
  if (v >= 109) {
    c.array("m_pTitleAward", TaskKind::TitleAward, titles, 8, [&]() { read_title_award(c); });
  }
  if (v >= 59) {
    read_change_keys(c, "m_ChangeKeys", change);
  }
  if (v >= 178) {
    read_change_keys(c, "PersonChangeKeys", person);
  }
  if (v >= 186) {
    read_change_keys(c, "NewChangeKeys", carrier);
  }
  if (v >= 112) {
    read_change_keys(c, "m_HistoryChangeKeys", history);
  }
  if (v >= 59) {
    c.array("m_plDisplayKey", TaskKind::I32Value, display, 4, [&]() { c.i32("m_plDisplayKey"); });
  }
  if (v >= 63) {
    c.array("m_Expressions", TaskKind::Expression, exp, 576, [&]() { read_expression_group(c); });
    c.array("m_pTaskChar", TaskKind::TaskChar, task_char, 128, [&]() {
      c.wchars("m_pTaskChar", 64, false);
    });
  }
  if (v >= 146) {
    c.array("AwardFactionTargetData", TaskKind::FactionTarget, faction_targets, 8, [&]() {
      c.i32("FactionTargetId");
      c.i32("FactionNum");
    });
  }
  if (v >= 194 && group_num > 0 && num_per_group > 0) {
    Count texts;
    texts.n = group_num * num_per_group;
    c.array("AwardRandomDialogGroupData", TaskKind::DialogText, texts, 200, [&]() {
      c.wchars("TextGroups", 100, false);
    });
  }
}

void read_award_ratio_scale(Ctx &c, const char *name)
{
  c.structure(name, TaskKind::Other, [&]() {
    const Count n = c.count_i32("m_ulScales");
    c.fixed("m_Ratios", TaskKind::Other, max_award_scales(c.v), [&]() { c.f32("m_Ratios"); });
    read_award_list(c, n);
  });
}

void read_award_items_scale(Ctx &c, const char *name)
{
  c.structure(name, TaskKind::Other, [&]() {
    const Count n = c.count_i32("m_ulScales");
    c.i32("m_ulItemId");
    c.fixed_i32("m_Counts", max_award_scales(c.v));
    read_award_list(c, n);
  });
}

void read_award_carrier_hp_scale(Ctx &c, const char *name)
{
  c.structure(name, TaskKind::Other, [&]() {
    const Count n = c.count_i32("m_ulScales");
    c.fixed_i32("AwardCarrierHp", 5);
    read_award_list(c, n);
  });
}

void read_award_var_scale(Ctx &c, const char *name)
{
  c.structure(name, TaskKind::Other, [&]() {
    const Count n = c.count_i32("m_ulScales");
    c.fixed("AwardVar", TaskKind::Expression, 16, [&]() { read_expression(c); });
    read_award_list(c, n);
  });
}

const char *special_param_name(int32_t type)
{
  static const char *names[] = {"FailWhenGetOffCarrier",
                                "TimeWhenRideCarrier",
                                "UseItemTimes",
                                "UseItemCount",
                                "UseItemIDsPointer",
                                "CompleteTaskTimes",
                                "CompleteTaskCount",
                                "CompleteTaskIDsPointer",
                                "ByCompleteTask",
                                "Parameter9",
                                "GetOffCarrierID",
                                "RideCarrierID",
                                "need_story_book_collection",
                                "story_book_collection_count",
                                "CompleteTaskIDsSpacePointer"};
  return (type >= 0 && type <= 14) ? names[type] : nullptr;
}

void read_special_params(Ctx &c, const Count &count)
{
  c.structure("SpecialParameters", TaskKind::Other, [&]() {
    Count use_items, complete_tasks;
    c.array("Parameters", TaskKind::SpecialParam, count, 4, [&]() {
      const int32_t type = c.i32("ParameterType");
      if (const char *name = special_param_name(type)) {
        const Count value = c.count_i32(name);
        if (type == 3) {
          use_items = value;
        }
        else if (type == 6) {
          complete_tasks = value;
        }
      }
      if (c.v >= 176) {
        c.i32("unk176");
      }
    });
    if (use_items.n > 0) {
      c.array("UseItemIDs", TaskKind::I32Value, use_items, 4, [&]() { c.i32("UseItemIDs"); });
    }
    if (complete_tasks.n > 0) {
      c.array("CompleteTaskIDs", TaskKind::I32Value, complete_tasks, 4, [&]() {
        c.i32("CompleteTaskIDs");
      });
      if (c.v >= 153) {
        c.array("CompleteTaskIDsSpace", TaskKind::I32Value, complete_tasks, 4, [&]() {
          c.i32("CompleteTaskIDsSpace");
        });
      }
    }
  });
}

/** Single region of versions before 80 (later versions store a list). */
void read_old_region(Ctx &c, const char *name)
{
  read_region(c, name);
}

/**
 * The fixed data of a task. Returns the task id.
 * Member names follow the engine's `ATaskTemplFixedData`.
 */
uint32_t read_fixed_data(Ctx &c, int32_t node_index)
{
  const uint32_t v = c.v;

  /* Identity. */
  if (v >= 153) {
    c.i16("m_usIDSpace");
  }
  const uint32_t id = uint32_t(c.i32("m_ID"));
  const int32_t id_field = c.last;
  c.wchars("m_szName", 30, true);
  const int32_t name_field = c.last;
  if (node_index >= 0 && c.nodes) {
    (*c.nodes)[node_index].id = id;
    (*c.nodes)[node_index].id_field = id_field;
    (*c.nodes)[node_index].name_field = name_field;
  }
  bool has_sign = false;
  if (v < 135) {
    has_sign = c.b("m_bHasSign");
  }
  if (v < 176) {
    c.ptr("m_pszSignature_ptr");
  }
  c.i32("m_ulType");
  c.i32("m_ulTimeLimit");

  /* Time. */
  if (v >= 68) {
    if (v < 135) {
      c.b("m_bOfflineFail");
    }
    c.b("m_bAbsFail");
    read_tm(c, "m_tmAbsFailTime");
    if (v >= 175) {
      c.b("m_bNextTimeFail");
      read_tm(c, "m_tmNextFailTime");
    }
    if (v < 135) {
      c.b("m_bItemNotTakeOff");
    }
  }
  if (v >= 52 && v < 135) {
    c.b("m_bAbsTime");
  }
  const Count timetable = c.count_i32("m_ulTimetable");
  if (v >= 52) {
    c.fixed("m_tmType", TaskKind::Other, max_timetable(v), [&]() { c.u8("m_tmType"); });
  }
  if (v < 176) {
    c.ptr("m_tmStart_ptr");
    c.ptr("m_tmEnd_ptr");
  }
  if (v >= 52) {
    c.i32("m_lAvailFrequency");
    c.i32("m_lFrequencyNum");
  }
  if (v >= 191) {
    c.i32("m_lPeriodLimit");
  }
  if (v >= 175) {
    c.b("m_bAvailFrequency");
    read_tm(c, "m_tmAvailFrequency");
  }

  /* Sub task behaviour and delivery. */
  if (v < 135) {
    c.b("m_bChooseOne");
    c.b("m_bRandOne");
    c.b("m_bExeChildInOrder");
    c.b("m_bParentAlsoFail");
    c.b("m_bParentAlsoSucc");
    c.b("m_bCanGiveUp");
    c.b("m_bCanRedo");
    c.b("m_bCanRedoAfterFailure");
    c.b("m_bClearAsGiveUp");
    c.b("m_bNeedRecord");
    c.b("m_bFailAsPlayerDie");
  }
  c.i32("m_ulMaxReceiver");
  if (v < 135) {
    c.b("m_bDelvInZone");
  }
  c.i32("m_ulDelvWorld");
  Count delv_regions, enter_regions, leave_regions;
  if (v >= 80) {
    delv_regions = c.count_i32("m_ulDelvRegionCnt");
    if (v < 176) {
      c.ptr("m_pDelvRegion_ptr");
    }
  }
  else {
    read_old_region(c, "m_DelvRegion");
  }
  if (v >= 68) {
    if (v < 135) {
      c.b("m_bEnterRegionFail");
    }
    c.i32("m_ulEnterRegionWorld");
    if (v >= 80) {
      enter_regions = c.count_i32("m_ulEnterRegionCnt");
      if (v < 176) {
        c.ptr("m_pEnterRegion_ptr");
      }
    }
    else {
      read_old_region(c, "m_EnterRegion");
    }
    if (v < 135) {
      c.b("m_bLeaveRegionFail");
    }
    c.i32("m_ulLeaveRegionWorld");
    if (v >= 80) {
      leave_regions = c.count_i32("m_ulLeaveRegionCnt");
      if (v < 176) {
        c.ptr("m_pLeaveRegion_ptr");
      }
    }
    else {
      read_old_region(c, "m_LeaveRegion");
    }
  }
  if (v >= 97 && v < 135) {
    c.b("m_bLeaveForceFail");
  }
  if (v < 135) {
    c.b("m_bTransTo");
  }
  c.i32("m_ulTransWldId");
  read_vert(c, "m_TransPt");
  if (v >= 52) {
    c.i32("m_lMonsCtrl");
    if (v < 135) {
      c.b("m_bTrigCtrl");
    }
  }
  if (v < 135) {
    c.b("m_bAutoDeliver");
    if (v >= 84) {
      c.b("m_bDisplayInExclusiveUI");
    }
    if (v >= 89) {
      c.b("m_bReadyToNotifyServer");
    }
    if (v >= 116) {
      c.b("m_bUsedInTokenShop");
    }
    c.b("m_bDeathTrig");
    c.b("m_bClearAcquired");
  }
  c.i32("m_ulSuitableLevel");
  if (v < 135) {
    c.b("m_bShowPrompt");
    c.b("m_bKeyTask");
  }
  c.i32("m_ulDelvNPC");
  c.i32("m_ulAwardNPC");
  /* Global variable changes. The count sits right after `m_bMarriage` (a separate bool before
   * version 135, a switch afterwards), followed by the three array pointers while records still
   * stored pointers. Verified on version 128; the positions of the person and history counts
   * before version 198 are extrapolated. */
  Count change_keys, person_keys, history_keys;
  auto read_change_counts = [&]() {
    if (v < 59) {
      return;
    }
    change_keys = c.count_i32("m_ulChangeKeyCnt");
    if (v >= 178) {
      person_keys = c.count_i32("m_ulChangePersonKeyCnt");
    }
    if (v >= 185) {
      history_keys = c.count_i32("m_ulChangeHistoryKeyCnt");
    }
    if (v < 176) {
      c.ptr("m_plChangeKey_ptr");
      c.ptr("m_plChangeKeyValue_ptr");
      c.ptr("m_pbChangeType_ptr");
    }
  };
  if (v >= 135) {
    read_change_counts();
  }
  if (v < 135) {
    c.b("m_bSkillTask");
    if (v >= 52) {
      c.b("m_bCanSeekOut");
      c.b("m_bShowDirection");
    }
    if (v >= 55) {
      c.b("m_bMarriage");
    }
    read_change_counts();
  }
  if (v >= 63 && v < 135) {
    c.b("m_bSwitchSceneFail");
    c.b("m_bHidden");
  }
  if (v >= 78) {
    if (v < 135) {
      c.b("m_bDeliverySkill");
    }
    c.i32("m_iDeliveredSkillID");
    c.i32("m_iDeliveredSkillLevel");
    if (v < 135) {
      c.b("m_bShowGfxFinished");
    }
  }
  if (v >= 79 && v < 135) {
    c.b("m_bChangePQRanking");
  }
  if (v >= 81) {
    if (v < 135) {
      c.b("m_bCompareItemAndInventory");
    }
    c.i32("m_ulInventorySlotNum");
  }
  if (v < 135) {
    if (v >= 123) {
      c.b("m_bTowerTask");
    }
    if (v >= 126) {
      c.b("m_bHomeTask");
    }
    if (v >= 128) {
      c.b("m_bDeliverInHostHome");
      c.b("m_bFinishInHostHome");
    }
  }

  /* Public quest. */
  Count pq_exps, contribs;
  if (v >= 75) {
    if (v < 135) {
      c.b("m_bPQTask");
    }
    pq_exps = c.count_i32("m_ulPQExpCnt");
    if (v < 176) {
      c.ptr("m_pszPQExp_ptr");
      c.ptr("m_pPQExpArr_ptr");
    }
    if (v < 135) {
      c.b("m_bPQSubTask");
      c.b("m_bClearContrib");
    }
    contribs = c.count_i32("m_ulMonsterContribCnt");
    if (v < 176) {
      c.ptr("m_MonstersContrib_ptr");
    }
  }
  if (v >= 91) {
    c.i32("m_iPremNeedRecordTasksNum");
    if (v < 135) {
      c.b("m_bShowByNeedRecordTasksNum");
    }
  }
  if (v >= 87) {
    c.i32("m_iPremiseFactionContrib");
    if (v < 135) {
      c.b("m_bShowByFactionContrib");
    }
  }
  if (v < 135) {
    if (v >= 75) {
      c.b("m_bAccountTaskLimit");
    }
    if (v >= 115) {
      c.b("m_bRoleTaskLimit");
    }
  }
  if (v >= 75) {
    c.i32("m_ulAccountTaskLimitCnt");
  }
  if (v < 135) {
    if (v >= 105) {
      c.b("m_bLeaveFactionFail");
    }
    if (v >= 111) {
      c.b("m_bNotIncCntWhenFailed");
    }
    if (v >= 108) {
      c.b("m_bNotClearItemWhenFailed");
    }
    if (v >= 110) {
      c.b("m_bDisplayInTitleTaskUI");
    }
  }
  if (v >= 146) {
    c.i32("m_iFactionMember");
  }
  if (v >= 161) {
    c.i32("m_ulCrossServerId");
    c.i32("m_ulIconStateIDForAwardVar");
    c.i32("m_lGlobalKeyForAwardVar");
  }
  if (v >= 178) {
    c.i32("m_lPersonKeyForAwardVar");
  }
  if (v >= 185) {
    c.i32("m_lHistoryKeyForAwardVar");
  }
  if (v >= 175) {
    c.b("m_bPropNeedComp");
    c.i32("m_nPropExp1AndOrExp2");
    read_compare(c, "m_Prop1KeyValue");
    read_compare(c, "m_Prop2KeyValue");
  }
  if (v >= 189) {
    c.b("m_bPropNeedComp_personal");
    c.i32("m_nPropExp1AndOrExp2_personal");
    read_compare(c, "m_Prop1KeyValue_personal");
    read_compare(c, "m_Prop2KeyValue_personal");
  }
  Count prop_summoned, vague_prompts;
  if (v >= 180) {
    c.i32("m_iHideOtherPlayerTime");
    prop_summoned = c.count_i32("m_ulPropSummonedMonsters");
  }
  if (v >= 180 && v < 182) {
    /* Inline block of 32 monsters, replaced by a variable list in version 182. */
    c.structure("PropMonsterSummoned", TaskKind::Other, [&]() {
      c.i32("m_ulMonsterNum");
      c.b("m_bRandChoose");
      c.i32("m_ulSummonRadius");
      c.b("m_bDeathDisappear");
      c.fixed("m_Monsters", TaskKind::MonsterSummoned, 32, [&]() { read_monster_summoned(c); });
    });
  }
  if (v >= 181) {
    c.i32("m_iGfxType");
  }
  if (v >= 198) {
    c.f32("m_fVaguePromptRadius");
    c.f32("m_fVaguePromptX");
    c.f32("m_fVaguePromptY");
    if (v >= 215) {
      vague_prompts = c.count_u32("m_ulVaguePromptNumbers");
    }
  }
  if (v >= 240) {
    c.i32("m_iCameraMovementID");
  }

  /* Premises. */
  if (v >= 96) {
    c.u8("m_ucPremiseTransformedForm");
    if (v < 135) {
      c.b("m_bShowByTransformed");
    }
  }
  c.i32("m_ulPremise_Lev_Min");
  c.i32("m_ulPremise_Lev_Max");
  if (v >= 113) {
    c.i32("m_bPremCheckMaxHistoryLevel");
  }
  if (v < 135) {
    c.b("m_bShowByLev");
  }
  if (v >= 113) {
    if (v < 135) {
      c.b("m_bPremCheckReincarnation");
    }
    c.i32("m_ulPremReincarnationMin");
    c.i32("m_ulPremReincarnationMax");
    if (v < 135) {
      c.b("m_bShowByReincarnation");
      c.b("m_bPremCheckRealmLevel");
    }
    c.i32("m_ulPremRealmLevelMin");
    c.i32("m_ulPremRealmLevelMax");
    if (v < 135) {
      c.b("m_bPremCheckRealmExpFull");
      c.b("m_bShowByRealmLevel");
    }
  }
  Count scene_items, scene_regions;
  if (v >= 227) {
    scene_items = c.count_u32("m_ulChangeSceneRemoveItemRegionNum");
    scene_regions = c.count_u32("m_ulChangeSceneRemoveNum");
  }
  const Count prem_items = c.count_i32("m_ulPremItems");
  if (v < 176) {
    c.ptr("m_PremItems_ptr");
  }
  if (v < 135) {
    c.b("m_bShowByItems");
    if (v >= 98) {
      c.b("m_bPremItemsAnyOne");
    }
  }
  const Count given_items = c.count_i32("m_ulGivenItems");
  c.i32("m_ulGivenCmnCount");
  c.i32("m_ulGivenTskCount");
  if (v >= 215) {
    c.u32("m_ulGivenRelicCount");
    c.u32("m_ulGivenDongluCmnCount");
  }
  if (v < 176) {
    c.ptr("m_GivenItems_ptr");
  }
  c.i32("m_ulPremise_Deposit");
  if (v < 135) {
    c.b("m_bShowByDeposit");
  }
  c.i32("m_lPremise_Reputation");
  if (v >= 53) {
    c.i32("m_lPremise_RepuMax");
  }
  if (v < 135) {
    c.b("m_bShowByRepu");
  }
  c.i32("m_ulPremise_Task_Count");
  c.fixed_i32("m_ulPremise_Tasks", max_prem_tasks(v));
  if (v >= 153) {
    c.fixed_i16("m_usPremise_Tasks_Space", max_prem_tasks(v));
  }
  if (v < 135) {
    c.b("m_bShowByPreTask");
  }
  if (v >= 90) {
    c.i32("m_ulPremise_Task_Least_Num");
  }
  c.i32("m_ulPremise_Period");
  if (v < 135) {
    c.b("m_bShowByPeriod");
  }
  c.i32("m_ulPremise_Faction");
  if (v >= 93) {
    c.i32("m_iPremise_FactionRole");
  }
  if (v < 135) {
    c.b("m_bShowByFaction");
  }
  c.i32("m_ulGender");
  if (v < 135) {
    c.b("m_bShowByGender");
  }
  c.i32("m_ulOccupations");
  c.fixed_i32("m_Occupations", max_occupations(v));
  if (v < 135) {
    c.b("m_bShowByOccup");
    c.b("m_bPremise_Spouse");
    c.b("m_bShowBySpouse");
    if (v >= 102) {
      c.b("m_bPremiseWeddingOwner");
      c.b("m_bShowByWeddingOwner");
    }
  }
  if (v < 135) {
    if (v >= 52) {
      c.b("m_bGM");
    }
    if (v >= 68) {
      c.b("m_bShieldUser");
    }
  }
  if (v >= 75) {
    if (v < 135) {
      c.b("m_bShowByRMB");
    }
    c.i32("m_ulPremRMBMin");
    c.i32("m_ulPremRMBMax");
    if (v < 135) {
      c.b("m_bCharTime");
      c.b("m_bShowByCharTime");
    }
    c.i32("m_iCharStartTime");
    c.i32("m_iCharEndTime");
    read_tm(c, "m_tmCharEndTime");
    c.i32("m_ulCharTimeGreaterThan");
  }
  if (v >= 153) {
    c.i16("m_usPremise_Cotask_Space");
  }
  c.i32("m_ulPremise_Cotask");
  c.i32("m_ulCoTaskCond");
  c.i32("m_ulMutexTaskCount");
  if (v >= 153) {
    c.fixed_i16("m_usMutexTasksSpace", max_mutex_tasks(v));
  }
  c.fixed_i32("m_ulMutexTasks", max_mutex_tasks(v));
  c.fixed_i32("m_lSkillLev", 4);
  c.u8("m_DynTaskType");
  c.i32("m_ulSpecialAward");

  /* Team. */
  if (v < 135) {
    c.b("m_bTeamwork");
    c.b("m_bRcvByTeam");
    c.b("m_bSharedTask");
    c.b("m_bSharedAchieved");
    c.b("m_bCheckTeammate");
  }
  c.f32("m_fTeammateDist");
  if (v < 135) {
    c.b("m_bAllFail");
    c.b("m_bCapFail");
    c.b("m_bCapSucc");
  }
  c.f32("m_fSuccDist");
  if (v < 135) {
    c.b("m_bDismAsSelfFail");
    c.b("m_bRcvChckMem");
  }
  c.f32("m_fRcvMemDist");
  if (v < 135) {
    c.b("m_bCntByMemPos");
  }
  c.f32("m_fCntMemDist");
  if (v < 135) {
    if (v >= 68) {
      c.b("m_bAllSucc");
    }
    if (v >= 75) {
      c.b("m_bCoupleOnly");
    }
    if (v >= 86) {
      c.b("m_bDistinguishedOcc");
    }
  }
  const Count team_members = c.count_i32("m_ulTeamMemsWanted");
  if (v < 176) {
    c.ptr("m_TeamMemsWanted_ptr");
  }
  if (v < 135) {
    c.b("m_bShowByTeam");
  }

  /* Global key premises. */
  if (v >= 59) {
    if (v < 135) {
      c.b("m_bPremNeedComp");
    }
    c.i32("m_nPremExp1AndOrExp2");
    read_compare(c, "m_Prem1KeyValue");
    read_compare(c, "m_Prem2KeyValue");
  }
  if (v >= 178) {
    c.i32("m_nPremPersonExp1AndOrExp2");
    read_compare(c, "m_Prem1PersonKeyValue");
    read_compare(c, "m_Prem2PersonKeyValue");
  }
  if (v >= 185) {
    c.i32("m_nPremHistoryExp1AndOrExp2");
    read_compare(c, "m_Prem1HistoryKeyValue");
    read_compare(c, "m_Prem2HistoryKeyValue");
  }

  /* Force, king, titles, cards, home... */
  if (v >= 97) {
    if (v < 135) {
      c.b("m_bPremCheckForce");
    }
    c.i32("m_iPremForce");
    if (v < 135) {
      c.b("m_bShowByForce");
    }
    c.i32("m_iPremForceReputation");
    if (v < 135) {
      c.b("m_bShowByForceReputation");
    }
    c.i32("m_iPremForceContribution");
    if (v < 135) {
      c.b("m_bShowByForceContribution");
    }
    c.i32("m_iPremForceExp");
    if (v < 135) {
      c.b("m_bShowByForceExp");
    }
    c.i32("m_iPremForceSP");
    if (v < 135) {
      c.b("m_bShowByForceSP");
    }
  }
  if (v >= 99) {
    c.i32("m_iPremForceActivityLevel");
    if (v < 135) {
      c.b("m_bShowByForceActivityLevel");
    }
  }
  if (v < 135) {
    if (v >= 106) {
      c.b("m_bPremIsKing");
      c.b("m_bShowByKing");
    }
    if (v >= 107) {
      c.b("m_bPremNotInTeam");
      c.b("m_bShowByNotInTeam");
    }
  }
  Count titles;
  if (v >= 109) {
    titles = c.count_i32("m_iPremTitleNumTotal");
    c.i32("m_iPremTitleNumRequired");
    if (v < 176) {
      c.ptr("m_PremTitles_ptr");
    }
    if (v < 135) {
      c.b("m_bShowByTitle");
    }
  }
  if (v >= 112) {
    c.fixed_i32("m_iPremHistoryStageIndex", 2);
  }
  if (v >= 118 && v < 135) {
    c.b("m_bShowByHistoryStage");
  }
  if (v >= 114) {
    c.i32("m_ulPremGeneralCardCount");
    if (v < 135) {
      c.b("m_bShowByGeneralCard");
    }
  }
  if (v >= 117) {
    c.i32("m_iPremGeneralCardRank");
    c.i32("m_ulPremGeneralCardRankCount");
    if (v < 135) {
      c.b("m_bShowByGeneralCardRank");
    }
  }
  if (v >= 122) {
    c.i32("m_ulPremIconStateID");
    if (v < 135) {
      c.b("m_bShowByIconStateID");
    }
  }
  if (v >= 237) {
    c.i32("m_ulFailIconStateID");
  }
  if (v >= 125) {
    c.i32("m_iVIPLevelMin");
    c.i32("m_iVIPLevelMax");
    if (v < 135) {
      c.b("m_bShowByVIPLevel");
    }
  }
  if (v >= 129 && v < 135) {
    c.b("m_bPremNoHome");
  }
  if (v >= 126) {
    c.i32("m_iPremHomeLevelMin");
    c.i32("m_iPremHomeLevelMax");
    if (v < 135) {
      c.b("m_bShowByHomeLevel");
    }
    c.i32("m_iPremHomeResourceLevelType");
    c.i32("m_iPremHomeResourceLevelMin");
    c.i32("m_iPremHomeResourceLevelMax");
    if (v < 135) {
      c.b("m_bShowByHomeResourceLevel");
    }
    c.i32("m_iPremHomeFactoryLevelType");
    c.i32("m_iPremHomeFactoryLevelMin");
    c.i32("m_iPremHomeFactoryLevelMax");
    if (v < 135) {
      c.b("m_bShowByHomeFactoryLevel");
    }
    c.i32("m_iPremHomeFlourishMin");
    c.i32("m_iPremHomeFlourishMax");
    if (v < 135) {
      c.b("m_bShowByHomeFlourish");
      c.b("m_bPremHomeStorageTask");
      c.b("m_bShowByHomeStorageTask");
    }
  }
  if (v >= 137) {
    c.i32("m_iPremArenaWinCount");
  }
  if (v >= 146) {
    c.i32("m_iPremFactionScore");
    c.i32("m_iPremFactionMoney");
    c.i32("m_iPremFactionPersonScore");
  }
  if (v >= 153) {
    c.i32("m_iPremMentorMorality");
    c.i32("m_iPremMentorStudentValue");
  }
  if (v >= 156) {
    c.i32("m_iPremCoupleIntimacy");
  }
  if (v >= 158) {
    c.i32("m_ulPremDeposit2");
  }
  if (v >= 161) {
    c.i32("m_ulNeededTaskCount");
    c.fixed_i16("m_usNeededTasksSpace", max_mutex_tasks(v));
    c.fixed_i32("m_ulNeededTasks", max_mutex_tasks(v));
    c.u8("m_iNeededTasksLeastNum");
  }
  if (v >= 167) {
    c.i32("m_iCamp");
    c.i32("m_iCampPower");
    c.i32("m_iNeedFriendPoint");
  }
  if (v >= 175) {
    c.i32("m_iRankType");
    c.i32("m_iRankGradeMin");
    c.i32("m_iRankGradeMax");
  }
  if (v >= 198) {
    c.u32("m_ulPremPanguLevelMin");
    c.u32("m_ulPremPanguLevelMax");
    if (v >= 215) {
      c.u32("m_ulPremPanguLevelYear");
    }
    c.i32("m_iAchievementSeriesId");
    if (v >= 215) {
      c.i32("m_iPanguEquipYear");
      c.i32("m_iPanguEquipSlot");
      c.i32("m_iPanguEquipQuality");
      c.i32("m_iPanguEquipLevel");
    }
    if (v >= 218) {
      c.u8("m_cPremInventorySizeMin");
      c.u8("m_cPremInventorySizeMax");
      c.u8("m_cPremStorageSizeMin");
      c.u8("m_cPremStorageSizeMax");
    }
    if (v >= 223) {
      c.i32("m_iPremLieMoLevelMin");
      c.i32("m_iPremLieMoLevelMax");
      c.i32("m_iPremYuMoLevelMin");
      c.i32("m_iPremYuMoLevelMax");
      c.u8("m_cEffectiveGameServer");
    }
    if (v >= 229) {
      c.i32("m_iPremDailyActivityValue");
    }
  }

  /* Completion method. */
  Count monsters_unwanted, players_wanted, monsters_wanted, items_wanted;
  if (v >= 195) {
    monsters_unwanted = c.count_u32("m_ulMonsterUnwanted");
  }
  c.i32("m_enumMethod");
  c.i32("m_enumFinishType");
  if (v >= 103) {
    players_wanted = c.count_i32("m_ulPlayerWanted");
    if (v < 176) {
      c.ptr("m_PlayerWanted_ptr");
    }
  }
  monsters_wanted = c.count_i32("m_ulMonsterWanted");
  if (v < 176) {
    c.ptr("m_MonsterWanted_ptr");
  }
  items_wanted = c.count_i32("m_ulItemsWanted");
  if (v < 176) {
    c.ptr("m_ItemsWanted_ptr");
  }
  c.i32("m_ulGoldWanted");
  if (v >= 88) {
    c.i32("m_iFactionContribWanted");
  }
  if (v >= 89) {
    c.i32("m_iFactionExpContribWanted");
  }
  c.i32("m_ulNPCToProtect");
  c.i32("m_ulProtectTimeLen");
  c.i32("m_ulNPCMoving");
  c.i32("m_ulNPCDestSite");
  Count reach_sites, leave_sites;
  if (v >= 80) {
    if (v < 176) {
      c.ptr("m_pReachSite_ptr");
    }
    reach_sites = c.count_i32("m_ulReachSiteCnt");
    c.i32("m_ulReachSiteId");
  }
  else {
    read_old_region(c, "m_ReachSite");
    c.i32("m_ulReachSiteId");
  }
  c.i32("m_ulWaitTime");
  if (v >= 94) {
    read_vert(c, "m_TreasureStartZone");
    c.u8("m_ucZonesNumX");
    c.u8("m_ucZonesNumZ");
    c.u8("m_ucZoneSide");
  }
  if (v >= 137) {
    c.i16("m_usZoneMapID");
  }
  if (v >= 69) {
    if (v >= 80) {
      if (v < 176) {
        c.ptr("m_pLeaveSite_ptr");
      }
      leave_sites = c.count_i32("m_ulLeaveSiteCnt");
      c.i32("m_ulLeaveSiteId");
    }
    else {
      read_old_region(c, "m_LeaveSite");
      c.i32("m_ulLeaveSiteId");
    }
  }
  if (v >= 59) {
    if (v < 135) {
      c.b("m_bFinNeedComp");
    }
    c.i32("m_nFinExp1AndOrExp2");
    read_compare(c, "m_Fin1KeyValue");
    read_compare(c, "m_Fin2KeyValue");
  }
  if (v >= 178) {
    c.i32("m_nFinPersonExp1AndOrExp2");
    read_compare(c, "m_Fin1PersonKeyValue");
    read_compare(c, "m_Fin2PersonKeyValue");
  }
  if (v >= 185) {
    c.i32("m_nFinHistoryExp1AndOrExp2");
    read_compare(c, "m_Fin1HistoryKeyValue");
    read_compare(c, "m_Fin2HistoryKeyValue");
  }
  Count exps, person_exps, history_exps;
  if (v >= 63) {
    exps = c.count_i32("m_ulExpCnt");
    if (v < 176) {
      c.ptr("m_pszExp_ptr");
      c.ptr("m_pExpArr_ptr");
    }
  }
  if (v >= 178) {
    person_exps = c.count_i32("m_ulPersonExpCnt");
  }
  if (v >= 185) {
    history_exps = c.count_i32("m_ulHistoryExpCnt");
  }
  Count task_chars, person_chars, history_chars;
  if (v >= 63) {
    task_chars = c.count_i32("m_ulTaskCharCnt");
    if (v < 176) {
      c.ptr("m_pTaskChar_ptr");
    }
  }
  if (v >= 178) {
    person_chars = c.count_i32("m_ulPersonTaskCharCnt");
  }
  if (v >= 185) {
    history_chars = c.count_i32("m_ulHistoryTaskCharCnt");
  }
  if (v >= 104) {
    c.u8("m_ucTransformedForm");
  }
  if (v >= 215) {
    c.u32("m_ulMonsterShapeID");
  }
  if (v >= 156 && v < 175) {
    c.i32("TransformedDescID");
  }
  if (v >= 113) {
    c.i32("m_ulReachLevel");
    c.i32("m_ulReachReincarnationCount");
    c.i32("m_ulReachRealmLevel");
    c.i32("m_uiEmotion");
  }
  if (v >= 153) {
    c.i32("m_ulGrowthTarget");
  }
  Count npc_services, target_tasks;
  if (v >= 223) {
    c.u32("m_uSpecialMethodType");
    c.u32("m_uSpecialMethodParm1");
    c.u32("m_uSpecialMethodParm2");
    c.u32("m_uSpecialMethodParm3");
    npc_services = c.count_u32("m_uSpecialMethodNpcServicesCount");
    c.u32("m_uSpecialMethodNpcServicesLeastCount");
    target_tasks = c.count_u32("m_uSpecialMethodTargetTaskCount");
    c.u32("m_uSpecialMethodTargetTaskLeastCount");
  }
  Count home_items;
  if (v >= 122) {
    c.i32("m_ulTMIconStateID");
  }
  if (v >= 156 && v < 175) {
    c.i32("TMIconStateDescID");
  }
  if (v >= 127) {
    c.i32("m_ulTMHomeLevelType");
  }
  if (v >= 126) {
    c.i32("m_ulTMReachHomeLevel");
    c.i32("m_ulTMReachHomeFlourish");
    home_items = c.count_i32("m_ulHomeItemsWanted");
    if (v < 176) {
      c.ptr("m_HomeItemsWanted_ptr");
    }
  }
  if (v >= 135) {
    c.i32("m_ulBindOnCarrierId");
    if (v >= 140) {
      c.i16("m_ulTMReachArenaCount");
    }
    else {
      c.i32("m_ulTMReachArenaCount");
    }
  }
  if (v >= 137) {
    if (v >= 140) {
      c.i16("m_ulTMReachArenaWinCount");
    }
    else {
      c.i32("m_ulTMReachArenaWinCount");
    }
  }
  if (v >= 215) {
    c.u32("m_ulTransmitPointID");
  }
  if (v >= 156) {
    c.wchars("m_szShowDialogName", 20, false);
  }
  if (v >= 167) {
    c.i32("m_iTriggerEvent");
  }
  if (v >= 175) {
    c.i32("m_iVerifyStateLayer");
    c.i32("m_ulVerifyStateLayerID");
    c.i32("m_iKeyWordIndex");
  }
  Count event_monsters, event_mines, event_tasks;
  if (v >= 140) {
    c.wchars("m_TaskEventName", 30, false);
    c.i32("m_TaskEventType");
    c.i16("m_usTaskEventTimeOrScore");
    event_monsters = c.count_i16("m_usTaskEventMonsterCount");
    if (v < 176) {
      c.ptr("TaskEventMonsterData_ptr");
    }
    event_mines = c.count_i16("m_usTaskEventMineCount");
    if (v < 176) {
      c.ptr("TaskEventMineData_ptr");
    }
    event_tasks = c.count_i16("m_usTaskEventTaskCount");
    if (v < 176) {
      c.ptr("TaskEventTaskData_ptr");
    }
  }
  if (v >= 146) {
    c.i16("m_FactionPersonScore");
  }
  if (v >= 153) {
    c.u8("m_iTinyGameType");
  }
  if (v >= 215) {
    c.u8("m_iSlotMachineType");
  }
  if (v >= 158) {
    c.i32("m_ulGoldWanted2");
  }
  if (v >= 185) {
    c.i32("m_chatGroupId");
  }
  if (v >= 219) {
    c.i32("m_HintConfigId");
  }
  if (v >= 188) {
    c.i32("m_iGuessStringID");
  }
  Count submit_items;
  if (v >= 194) {
    c.structure("m_Camera", TaskKind::Other, [&]() {
      c.f32("camera_pos_x");
      c.f32("camera_pos_y");
      c.f32("camera_pos_z");
      c.f32("camera_quat_x");
      c.f32("camera_quat_y");
      c.f32("camera_quat_z");
      c.f32("camera_quat_a");
      c.f32("camera_target_pos_x");
      c.f32("camera_target_pos_y");
      c.f32("camera_target_pos_z");
      c.f32("camera_target_quat_x");
      c.f32("camera_target_quat_y");
      c.f32("camera_target_quat_z");
      c.f32("camera_target_quat_a");
      c.i32("camera_rotate_axis");
      c.f32("camera_rotate_angle");
      c.f32("camera_take_seconds");
    });
  }
  if (v >= 215) {
    submit_items = c.count_u32("m_ulSubmitItemWantedNum");
  }
  if (v >= 228) {
    c.u32("m_ulDailyActivityValue");
  }

  /* Awards and tree links. */
  c.i32("m_ulAwardType_S");
  c.i32("m_ulAwardType_F");
  if (v < 176) {
    c.ptr("m_Award_S_ptr");
    c.ptr("m_Award_F_ptr");
    c.ptr("m_AwByRatio_S_ptr");
    c.ptr("m_AwByRatio_F_ptr");
    c.ptr("m_AwByItems_S_ptr");
    c.ptr("m_AwByItems_F_ptr");
    if (v >= 140) {
      c.ptr("AwByTaskEventRatio_S_ptr");
      c.ptr("AwByTaskEventRatio_F_ptr");
    }
    if (v >= 146) {
      c.ptr("AwByCarrierHp_S_ptr");
      c.ptr("AwByCarrierHp_F_ptr");
    }
    if (v >= 161) {
      c.ptr("AwByVar_S_ptr");
      c.ptr("AwByVar_F_ptr");
    }
  }
  c.i32("m_ulParent");
  c.i32("m_ulPrevSibling");
  c.i32("m_ulNextSibling");
  c.i32("m_ulFirstChild");
  if (v >= 83) {
    if (v < 135) {
      c.b("m_bIsLibraryTask");
    }
    c.f32("m_fLibraryTasksProbability");
  }
  if (v >= 95 && v < 135) {
    c.b("m_bIsUniqueStorageTask");
  }
  if (v >= 119) {
    c.i32("m_iWorldContribution");
  }
  if (v >= 215) {
    c.i32("m_iMaskDisableIcon");
  }
  if (v >= 135) {
    const size_t at = c.r.tell();
    const int n = switch_bytes(v);
    c.bytes("CombinedSwitches", size_t(n));
    const int bit = switch_bit(v, "HasSign");
    if (bit >= 0 && c.ok()) {
      has_sign = (c.r.data()[at + bit / 8] >> (bit % 8)) & 1;
    }
  }
  if (v == 139) {
    c.b("m_bWriteSpecialLog");
  }
  Count special_params;
  if (v >= 146) {
    special_params = c.count_i32("x_SpecialParametersCount");
    if (v < 176) {
      c.ptr("SpecialParameters_ptr");
    }
  }

  /* End of the engine structure, the variable parts follow. */
  if (has_sign) {
    c.wchars("m_pszSignature", 30, true);
  }
  c.array("m_Timetable", TaskKind::TaskTm, timetable, 48, [&]() {
    read_tm(c, "m_tmStart");
    read_tm(c, "m_tmEnd");
  });
  if (v >= 59) {
    read_change_keys(c, "m_ChangeKeys", change_keys);
  }
  if (v >= 178) {
    read_change_keys(c, "m_PersonChangeKeys", person_keys);
  }
  if (v >= 185) {
    read_change_keys(c, "m_HistoryChangeKeys", history_keys);
  }
  if (v >= 75) {
    c.array("m_PQExpressions", TaskKind::Expression, pq_exps, 576, [&]() {
      read_expression_group(c);
    });
    c.array("m_MonstersContrib", TaskKind::MonstersContrib, contribs, 12, [&]() {
      read_monsters_contrib(c);
    });
  }
  if (v >= 182 && prop_summoned.n > 0) {
    c.structure("PropMonsterSummoned", TaskKind::Other, [&]() {
      const Count n = c.count_i32("m_ulMonsterNum");
      c.b("m_bRandChoose");
      c.i32("m_ulSummonRadius");
      c.b("m_bDeathDisappear");
      Count capped = n;
      capped.n = std::min<int64_t>(std::max<int64_t>(n.n, 0), 32);
      c.array("m_Monsters", TaskKind::MonsterSummoned, capped, 16, [&]() {
        read_monster_summoned(c);
      });
    });
  }
  if (v >= 80) {
    c.array(
        "m_pDelvRegion", TaskKind::TaskRegion, delv_regions, 24, [&]() { read_region_body(c); });
    c.array(
        "m_pEnterRegion", TaskKind::TaskRegion, enter_regions, 24, [&]() { read_region_body(c); });
    c.array(
        "m_pLeaveRegion", TaskKind::TaskRegion, leave_regions, 24, [&]() { read_region_body(c); });
  }
  if (v >= 215) {
    c.array("m_pVaguePromptInfo", TaskKind::VaguePrompt, vague_prompts, 12, [&]() {
      c.f32("m_fVaguePromptRadius");
      c.f32("m_fVaguePromptX");
      c.f32("m_fVaguePromptY");
    });
  }
  c.array("m_PremItems", TaskKind::ItemWanted, prem_items, 13, [&]() { read_item_wanted(c); });
  c.array("m_GivenItems", TaskKind::ItemWanted, given_items, 13, [&]() { read_item_wanted(c); });
  c.array("m_TeamMemsWanted", TaskKind::TeamMemWanted, team_members, 32, [&]() {
    read_team_mem_wanted(c);
  });
  if (v >= 109) {
    c.array("m_PremTitles", TaskKind::I32Value, titles, 4, [&]() { c.i32("m_PremTitles"); });
  }
  if (v >= 195) {
    c.array("m_MonsterUnwanted", TaskKind::MonsterWanted, monsters_unwanted, 25, [&]() {
      read_monster_wanted(c);
    });
  }
  c.array("m_MonsterWanted", TaskKind::MonsterWanted, monsters_wanted, 25, [&]() {
    read_monster_wanted(c);
  });
  if (v >= 103) {
    c.array("m_PlayerWanted", TaskKind::PlayerWanted, players_wanted, 37, [&]() {
      read_player_wanted(c);
    });
  }
  c.array("m_ItemsWanted", TaskKind::ItemWanted, items_wanted, 13, [&]() { read_item_wanted(c); });
  if (v >= 215) {
    c.array("m_pSubmitItemWanted", TaskKind::ItemWanted, submit_items, 13, [&]() {
      read_item_wanted(c);
    });
  }
  if (v >= 223) {
    c.array("m_pTSMNPCServices", TaskKind::NpcService, npc_services, 16, [&]() {
      c.u32("service1");
      c.u32("service2");
      c.u32("service3");
      c.u32("service4");
    });
    c.array("m_pTSMTargetTask", TaskKind::TargetTask, target_tasks, 8, [&]() {
      c.u32("target1");
      c.u32("target2");
    });
  }
  if (v >= 63) {
    c.array("m_Expressions", TaskKind::Expression, exps, 576, [&]() { read_expression_group(c); });
    c.array("m_pTaskChar", TaskKind::TaskChar, task_chars, 128, [&]() {
      c.wchars("m_pTaskChar", 64, false);
    });
  }
  if (v >= 178) {
    c.array("PersonExpressions", TaskKind::Expression, person_exps, 576, [&]() {
      read_expression_group(c);
    });
    c.array("PersonTaskChar", TaskKind::TaskChar, person_chars, 128, [&]() {
      c.wchars("PersonTaskChar", 64, false);
    });
  }
  if (v >= 185) {
    c.array("HistoryExpressions", TaskKind::Expression, history_exps, 576, [&]() {
      read_expression_group(c);
    });
    c.array("HistoryTaskChar", TaskKind::TaskChar, history_chars, 128, [&]() {
      c.wchars("HistoryTaskChar", 64, false);
    });
  }
  if (v >= 80) {
    c.array("m_pReachSite", TaskKind::TaskRegion, reach_sites, 24, [&]() { read_region_body(c); });
    c.array("m_pLeaveSite", TaskKind::TaskRegion, leave_sites, 24, [&]() { read_region_body(c); });
  }
  if (v >= 227) {
    c.array("m_pChangeSceneRemoveItemRegionNum", TaskKind::ItemWanted, scene_items, 13, [&]() {
      read_item_wanted(c);
    });
    c.array("m_pChangeSceneRemoveNum", TaskKind::TaskRegion, scene_regions, 24, [&]() {
      read_region_body(c);
    });
  }
  if (v >= 126) {
    c.array("m_HomeItemsWanted", TaskKind::HomeItemWanted, home_items, 8, [&]() {
      c.i32("m_ulHomeItemTid");
      c.i32("m_ulHomeItemCount");
    });
  }
  if (v >= 140) {
    c.array("TaskEventMonsterData", TaskKind::TaskEvent, event_monsters, 8, [&]() {
      read_task_event(c);
    });
    c.array(
        "TaskEventMineData", TaskKind::TaskEvent, event_mines, 8, [&]() { read_task_event(c); });
    c.array(
        "TaskEventTaskData", TaskKind::TaskEvent, event_tasks, 8, [&]() { read_task_event(c); });
  }
  c.structure("m_Award_S", TaskKind::AwardData, [&]() { read_award_data(c); });
  c.structure("m_Award_F", TaskKind::AwardData, [&]() { read_award_data(c); });
  read_award_ratio_scale(c, "m_AwByRatio_S");
  read_award_ratio_scale(c, "m_AwByRatio_F");
  read_award_items_scale(c, "m_AwByItems_S");
  read_award_items_scale(c, "m_AwByItems_F");
  if (v >= 140) {
    read_award_ratio_scale(c, "AwByTaskEventRatio_S");
    read_award_ratio_scale(c, "AwByTaskEventRatio_F");
  }
  if (v >= 146) {
    read_award_carrier_hp_scale(c, "AwByCarrierHp_S");
    read_award_carrier_hp_scale(c, "AwByCarrierHp_F");
  }
  if (v >= 161) {
    read_award_var_scale(c, "AwByVar_S");
    read_award_var_scale(c, "AwByVar_F");
  }
  if (v >= 146) {
    read_special_params(c, special_params);
  }
  return id;
}

void read_talk_proc(Ctx &c, const char *name)
{
  c.structure(name, TaskKind::Other, [&]() {
    c.i32("id_talk");
    c.wchars("text", 64, true);
    const Count windows = c.count_i32("num_window");
    c.array("windows", TaskKind::TalkWindow, windows, 16, [&]() {
      c.i32("id");
      c.i32("id_parent");
      c.pwstr("talk_text");
      const Count options = c.count_i32("num_option");
      c.array("options", TaskKind::TalkOption, options, 136, [&]() {
        c.i32("param");
        c.wchars("text", 64, true);
        c.i32("id");
      });
    });
  });
}

/** Maximum sub task nesting, as a guard against corrupt data. */
constexpr int MAX_TASK_DEPTH = 32;

void read_task(Ctx &c, const char *name, int32_t parent_node, int depth, uint32_t index = 0)
{
  if (depth > MAX_TASK_DEPTH) {
    c.fail("sub tasks nested too deep");
    return;
  }
  int32_t node = -1;
  if (c.nodes) {
    TaskNodeInfo info;
    info.parent = parent_node;
    c.nodes->push_back(info);
    node = int32_t(c.nodes->size() - 1);
  }
  const int32_t s = c.open(name, TaskFieldType::Struct, TaskKind::Task, index);
  if (node >= 0) {
    (*c.nodes)[node].field = s;
  }
  const size_t start = c.r.tell();
  read_fixed_data(c, node);
  if (node >= 0 && !c.fields && c.ok()) {
    /* Without the field index the id and name are taken from their fixed position. */
    const ByteSpan record = c.r.data().subspan(start);
    (*c.nodes)[node].id = task_record_id(record, c.v);
    (*c.nodes)[node].name = task_record_name(record, c.v);
  }
  c.pwstr("m_pwstrDescript");
  c.pwstr("m_pwstrOkText");
  c.pwstr("m_pwstrNoText");
  c.pwstr("m_pwstrTribute");
  read_talk_proc(c, "m_DelvTaskTalk");
  read_talk_proc(c, "m_UnqualifiedTalk");
  read_talk_proc(c, "m_DelvItemTalk");
  read_talk_proc(c, "m_ExeTalk");
  read_talk_proc(c, "m_AwardTalk");
  const Count subs = c.count_i32("m_nSubCount");
  if (!c.check_count(subs, "m_nSubCount", 64)) {
    c.close(s);
    return;
  }
  const int32_t a = c.open("m_SubTasks", TaskFieldType::Array, TaskKind::Task);
  if (a >= 0) {
    (*c.fields)[a].count_field = subs.field;
  }
  for (int64_t i = 0; i < subs.n && c.ok(); i++) {
    read_task(c, "m_SubTasks", node, depth + 1, uint32_t(i));
  }
  c.close(a);
  c.close(s);
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name Text helpers
 * \{ */

std::string decode_utf16(ByteSpan bytes, uint16_t key)
{
  Bytes plain(bytes.begin(), bytes.end());
  if (key != 0) {
    for (size_t i = 0; i + 1 < plain.size(); i += 2) {
      uint16_t ch = uint16_t(plain[i] | (plain[i + 1] << 8)) ^ key;
      plain[i] = uint8_t(ch & 0xFF);
      plain[i + 1] = uint8_t(ch >> 8);
    }
  }
  return utf16le_to_utf8(plain);
}

uint16_t xor_key(const TaskField &f, uint32_t owner_id)
{
  return (f.type == TaskFieldType::WCharsXor || f.type == TaskFieldType::PwStrXor) ?
             uint16_t(owner_id & 0xFFFF) :
             0;
}

/** \} */

}  // namespace

/* -------------------------------------------------------------------- */
/** \name Public API
 * \{ */

bool task_version_supported(uint32_t version)
{
  return version >= TASK_VERSION_MIN && version <= TASK_VERSION_MAX;
}

bool parse_task_record(
    ByteSpan blob, uint32_t version, TaskParse &r_parse, std::string *r_error, bool fields)
{
  r_parse = TaskParse();
  r_parse.version = version;
  if (!task_version_supported(version)) {
    set_error(r_error, "unsupported tasks.data version " + std::to_string(version));
    return false;
  }
  Ctx c(blob, version, fields ? &r_parse.fields : nullptr, &r_parse.nodes);
  read_task(c, "task", -1, 0);
  if (!c.ok()) {
    set_error(r_error, c.bad ? c.error : "task record is truncated");
    return false;
  }
  if (c.r.tell() != blob.size()) {
    set_error(r_error,
              "task record has " + std::to_string(blob.size() - c.r.tell()) +
                  " unread bytes (layout mismatch for version " + std::to_string(version) + ")");
    return false;
  }
  /* Decode node names. */
  for (TaskNodeInfo &node : r_parse.nodes) {
    if (node.name_field >= 0) {
      const TaskField &f = r_parse.fields[node.name_field];
      node.name = decode_utf16(blob.subspan(f.offset, f.size), xor_key(f, node.id));
    }
  }
  return true;
}

uint32_t task_record_id(ByteSpan blob, uint32_t version)
{
  return get_at<uint32_t>(blob, version >= 153 ? 2 : 0);
}

std::string task_record_name(ByteSpan blob, uint32_t version)
{
  const size_t at = (version >= 153 ? 2 : 0) + 4;
  if (at + 60 > blob.size()) {
    return {};
  }
  const uint16_t key = version >= 52 ? uint16_t(task_record_id(blob, version)) : 0;
  return decode_utf16(blob.subspan(at, 60), key);
}

int32_t task_find_field(const TaskParse &parse, int32_t parent, std::string_view name)
{
  const int32_t begin = parent + 1;
  for (int32_t i = std::max(begin, 0); i < int32_t(parse.fields.size()); i++) {
    const TaskField &f = parse.fields[i];
    if (f.parent == parent && name == f.name) {
      return i;
    }
    /* Children of `parent` are contiguous after it; stop once outside its range. */
    if (parent >= 0) {
      const TaskField &p = parse.fields[parent];
      if (f.offset > p.offset + p.size) {
        break;
      }
    }
  }
  return -1;
}

std::string task_field_path(const TaskParse &parse, int32_t field)
{
  std::vector<int32_t> chain;
  for (int32_t i = field; i >= 0; i = parse.fields[i].parent) {
    chain.push_back(i);
  }
  std::string path;
  for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
    const TaskField &f = parse.fields[*it];
    if (f.type == TaskFieldType::Item) {
      path += "[" + std::to_string(f.index) + "]";
      continue;
    }
    if (f.parent >= 0 && parse.fields[f.parent].type == TaskFieldType::Item &&
        std::strcmp(parse.fields[f.parent].name, f.name) == 0)
    {
      /* Scalar item of a value array: the index says it all. */
      continue;
    }
    if (!path.empty()) {
      path += ".";
    }
    path += f.name;
  }
  return path;
}

namespace {

/** Node owning `field`: the innermost task struct containing it. */
int32_t owner_node(const TaskParse &parse, int32_t field)
{
  for (int32_t i = field; i >= 0; i = parse.fields[i].parent) {
    const TaskField &f = parse.fields[i];
    if (f.type == TaskFieldType::Struct && f.kind == TaskKind::Task) {
      for (size_t n = 0; n < parse.nodes.size(); n++) {
        if (parse.nodes[n].field == i) {
          return int32_t(n);
        }
      }
    }
  }
  return parse.nodes.empty() ? -1 : 0;
}

}  // namespace

uint32_t task_field_owner_id(ByteSpan blob, const TaskParse &parse, int32_t field)
{
  const int32_t node = owner_node(parse, field);
  if (node < 0) {
    return 0;
  }
  const int32_t id_field = parse.nodes[node].id_field;
  return id_field >= 0 ? get_at<uint32_t>(blob, parse.fields[id_field].offset) : 0;
}

int64_t task_get_int(ByteSpan blob, const TaskParse &parse, int32_t field)
{
  const TaskField &f = parse.fields[field];
  switch (f.type) {
    case TaskFieldType::Bool:
    case TaskFieldType::U8:
      return get_at<uint8_t>(blob, f.offset);
    case TaskFieldType::I16:
      return get_at<int16_t>(blob, f.offset);
    case TaskFieldType::I32:
      return get_at<int32_t>(blob, f.offset);
    case TaskFieldType::U32:
      return get_at<uint32_t>(blob, f.offset);
    case TaskFieldType::F32:
      return int64_t(get_at<float>(blob, f.offset));
    default:
      return 0;
  }
}

float task_get_float(ByteSpan blob, const TaskParse &parse, int32_t field)
{
  const TaskField &f = parse.fields[field];
  if (f.type == TaskFieldType::F32) {
    return get_at<float>(blob, f.offset);
  }
  return float(task_get_int(blob, parse, field));
}

std::string task_get_text(ByteSpan blob, const TaskParse &parse, int32_t field)
{
  const TaskField &f = parse.fields[field];
  const uint32_t id = task_field_owner_id(blob, parse, field);
  switch (f.type) {
    case TaskFieldType::WChars:
    case TaskFieldType::WCharsXor:
      return decode_utf16(blob.subspan(f.offset, f.size), xor_key(f, id));
    case TaskFieldType::PwStrXor:
      return decode_utf16(blob.subspan(f.offset + 4, f.size - 4),
                          parse.version >= 52 ? xor_key(f, id) : 0);
    case TaskFieldType::Chars: {
      size_t len = 0;
      while (len < f.size && blob[f.offset + len] != 0) {
        len++;
      }
      return gbk_to_utf8(
          std::string_view(reinterpret_cast<const char *>(blob.data() + f.offset), len));
    }
    default:
      return {};
  }
}

bool task_set_int(
    Bytes &blob, const TaskParse &parse, int32_t field, int64_t value, std::string *r_error)
{
  const TaskField &f = parse.fields[field];
  auto check = [&](int64_t lo, int64_t hi) {
    if (value < lo || value > hi) {
      set_error(r_error, std::string("value out of range for ") + f.name);
      return false;
    }
    return true;
  };
  switch (f.type) {
    case TaskFieldType::Bool:
      if (!check(0, 1)) {
        return false;
      }
      set_at<uint8_t>(blob, f.offset, uint8_t(value));
      return true;
    case TaskFieldType::U8:
      if (!check(0, 255)) {
        return false;
      }
      set_at<uint8_t>(blob, f.offset, uint8_t(value));
      return true;
    case TaskFieldType::I16:
      if (!check(INT16_MIN, UINT16_MAX)) {
        return false;
      }
      set_at<uint16_t>(blob, f.offset, uint16_t(value));
      return true;
    case TaskFieldType::I32:
    case TaskFieldType::U32:
      if (!check(INT32_MIN, UINT32_MAX)) {
        return false;
      }
      set_at<uint32_t>(blob, f.offset, uint32_t(value));
      return true;
    default:
      set_error(r_error, std::string(f.name) + " is not an integer field");
      return false;
  }
}

bool task_set_float(Bytes &blob, const TaskParse &parse, int32_t field, float value)
{
  const TaskField &f = parse.fields[field];
  if (f.type != TaskFieldType::F32) {
    return false;
  }
  set_at<float>(blob, f.offset, value);
  return true;
}

namespace {

bool reparse(const Bytes &blob, TaskParse &parse, std::string *r_error)
{
  const uint32_t version = parse.version;
  return parse_task_record(blob, version, parse, r_error);
}

void encode_utf16(Bytes &utf16, uint16_t key)
{
  for (size_t i = 0; i + 1 < utf16.size(); i += 2) {
    const uint16_t ch = uint16_t(utf16[i] | (utf16[i + 1] << 8)) ^ key;
    utf16[i] = uint8_t(ch & 0xFF);
    utf16[i + 1] = uint8_t(ch >> 8);
  }
}

void splice(Bytes &blob, size_t offset, size_t remove, ByteSpan insert)
{
  blob.erase(blob.begin() + offset, blob.begin() + offset + remove);
  blob.insert(blob.begin() + offset, insert.begin(), insert.end());
}

}  // namespace

bool task_set_text(
    Bytes &blob, TaskParse &parse, int32_t field, std::string_view utf8, std::string *r_error)
{
  const TaskField f = parse.fields[field];
  if (task_get_text(blob, parse, field) == utf8) {
    /* Unchanged: keep the bytes (fixed buffers may hold leftovers after the terminator). */
    return true;
  }
  const uint32_t id = task_field_owner_id(blob, parse, field);
  const uint16_t key = parse.version >= 52 ? xor_key(f, id) : 0;
  if (f.type == TaskFieldType::Chars) {
    std::string gbk;
    if (!utf8_to_gbk(utf8, gbk)) {
      set_error(r_error, "text cannot be encoded as GBK");
      return false;
    }
    if (gbk.size() >= f.size) {
      set_error(r_error, "text too long");
      return false;
    }
    std::fill(blob.begin() + f.offset, blob.begin() + f.offset + f.size, 0);
    std::memcpy(blob.data() + f.offset, gbk.data(), gbk.size());
    return true;
  }
  Bytes utf16;
  if (!utf8_to_utf16le(utf8, utf16)) {
    set_error(r_error, "invalid UTF-8 text");
    return false;
  }
  if (f.type == TaskFieldType::WChars || f.type == TaskFieldType::WCharsXor) {
    /* Keep one NUL character so the engine sees a terminated string. */
    if (utf16.size() + 2 > f.size) {
      set_error(r_error, "text too long (at most " + std::to_string(f.size / 2 - 1) + " chars)");
      return false;
    }
    utf16.resize(f.size, 0);
    encode_utf16(utf16, key);
    std::memcpy(blob.data() + f.offset, utf16.data(), f.size);
    for (TaskNodeInfo &node : parse.nodes) {
      if (node.name_field == field) {
        node.name = std::string(utf8);
      }
    }
    return true;
  }
  if (f.type == TaskFieldType::PwStrXor) {
    /* Window texts keep a terminating NUL character inside the counted length. */
    const int32_t old_len = get_at<int32_t>(blob, f.offset);
    const bool had_nul = old_len > 0 &&
                         get_at<uint16_t>(blob, f.offset + 4 + (old_len - 1) * 2) == key;
    if (had_nul) {
      utf16.push_back(0);
      utf16.push_back(0);
    }
    encode_utf16(utf16, key);
    Bytes encoded;
    put<int32_t>(encoded, int32_t(utf16.size() / 2));
    encoded.insert(encoded.end(), utf16.begin(), utf16.end());
    splice(blob, f.offset, f.size, encoded);
    return reparse(blob, parse, r_error);
  }
  set_error(r_error, std::string(f.name) + " is not a text field");
  return false;
}

bool task_set_id(
    Bytes &blob, TaskParse &parse, int32_t node, uint32_t new_id, std::string *r_error)
{
  if (node < 0 || node >= int32_t(parse.nodes.size())) {
    set_error(r_error, "invalid task node");
    return false;
  }
  const uint32_t old_id = parse.nodes[node].id;
  const uint16_t delta = parse.version >= 52 ? uint16_t((old_id ^ new_id) & 0xFFFF) : 0;
  for (int32_t i = 0; i < int32_t(parse.fields.size()); i++) {
    const TaskField &f = parse.fields[i];
    if (f.type != TaskFieldType::WCharsXor && f.type != TaskFieldType::PwStrXor) {
      continue;
    }
    if (owner_node(parse, i) != node) {
      continue;
    }
    const size_t begin = f.offset + (f.type == TaskFieldType::PwStrXor ? 4 : 0);
    for (size_t at = begin; at + 1 < f.offset + f.size; at += 2) {
      set_at<uint16_t>(blob, at, get_at<uint16_t>(blob, at) ^ delta);
    }
  }
  set_at<uint32_t>(blob, parse.fields[parse.nodes[node].id_field].offset, new_id);
  /* Links of the other tasks of the record. */
  static const char *links[] = {
      "m_ulParent", "m_ulPrevSibling", "m_ulNextSibling", "m_ulFirstChild"};
  for (const TaskNodeInfo &other : parse.nodes) {
    for (const char *link : links) {
      const int32_t lf = task_find_field(parse, other.field, link);
      if (lf >= 0 && get_at<uint32_t>(blob, parse.fields[lf].offset) == old_id) {
        set_at<uint32_t>(blob, parse.fields[lf].offset, new_id);
      }
    }
  }
  return reparse(blob, parse, r_error);
}

Bytes task_empty_item(TaskKind kind, uint32_t version)
{
  /* Read an item of `kind` from zeros: every count is zero, so the consumed size is minimal. */
  static const Bytes zeros(1 << 16, 0);
  Ctx c(zeros, version, nullptr, nullptr);
  switch (kind) {
    case TaskKind::Task: {
      std::vector<TaskNodeInfo> nodes;
      c.nodes = &nodes;
      read_task(c, "task", -1, 0);
      break;
    }
    case TaskKind::TaskTm:
      read_tm(c, "m_tmStart");
      read_tm(c, "m_tmEnd");
      break;
    case TaskKind::TaskRegion:
      read_region_body(c);
      break;
    case TaskKind::ItemWanted:
      read_item_wanted(c);
      break;
    case TaskKind::MonsterWanted:
      read_monster_wanted(c);
      break;
    case TaskKind::PlayerWanted:
      read_player_wanted(c);
      break;
    case TaskKind::TeamMemWanted:
      read_team_mem_wanted(c);
      break;
    case TaskKind::MonstersContrib:
      read_monsters_contrib(c);
      break;
    case TaskKind::ChangeKey:
    case TaskKind::ChangeKeyLegacy:
      read_change_key(c);
      break;
    case TaskKind::Expression:
      read_expression_group(c);
      break;
    case TaskKind::TaskChar:
      c.wchars("m_pTaskChar", 64, false);
      break;
    case TaskKind::TaskEvent:
      read_task_event(c);
      break;
    case TaskKind::VaguePrompt:
      c.r.skip(12);
      break;
    case TaskKind::NpcService:
      c.r.skip(16);
      break;
    case TaskKind::TargetTask:
    case TaskKind::HomeItemWanted:
    case TaskKind::FactionTarget:
      c.r.skip(8);
      break;
    case TaskKind::MonsterSummoned:
      read_monster_summoned(c);
      break;
    case TaskKind::AwardData:
      read_award_data(c);
      break;
    case TaskKind::AwardCandItems:
      read_award_cand_items(c);
      break;
    case TaskKind::RankingAward:
      read_ranking_award(c);
      break;
    case TaskKind::TitleAward:
      read_title_award(c);
      break;
    case TaskKind::DialogText:
      c.r.skip(200);
      break;
    case TaskKind::TalkWindow:
      c.r.skip(16);
      break;
    case TaskKind::TalkOption:
      c.r.skip(136);
      break;
    case TaskKind::SpecialParam:
      c.r.skip(version >= 176 ? 12 : 8);
      break;
    case TaskKind::I32Value:
      c.r.skip(4);
      break;
    case TaskKind::I16Value:
      c.r.skip(2);
      break;
    default:
      return {};
  }
  return Bytes(c.r.tell(), 0);
}

bool task_array_insert(Bytes &blob,
                       TaskParse &parse,
                       int32_t array_field,
                       uint32_t at,
                       ByteSpan item,
                       std::string *r_error)
{
  const TaskField array = parse.fields[array_field];
  if (array.type != TaskFieldType::Array || array.count_field < 0) {
    set_error(r_error, std::string(array.name) + " has a fixed size");
    return false;
  }
  /* Arrays sharing the count (parallel arrays) all get an item. Process from the end so offsets
   * of the earlier ones stay valid. */
  std::vector<int32_t> arrays;
  for (int32_t i = 0; i < int32_t(parse.fields.size()); i++) {
    if (parse.fields[i].type == TaskFieldType::Array &&
        parse.fields[i].count_field == array.count_field)
    {
      arrays.push_back(i);
    }
  }
  const TaskField count = parse.fields[array.count_field];
  const int64_t n = task_get_int(blob, parse, array.count_field);
  if (at > n) {
    at = uint32_t(n);
  }
  for (auto it = arrays.rbegin(); it != arrays.rend(); ++it) {
    const TaskField &a = parse.fields[*it];
    /* Insert position: start of item `at`, or end of the array. */
    size_t pos = a.offset + a.size;
    for (int32_t i = *it + 1; i < int32_t(parse.fields.size()); i++) {
      const TaskField &f = parse.fields[i];
      if (f.parent == *it && f.type == TaskFieldType::Item && f.index == at) {
        pos = f.offset;
        break;
      }
      if (f.offset >= a.offset + a.size) {
        break;
      }
    }
    Bytes bytes;
    if (*it == array_field && !item.empty()) {
      bytes.assign(item.begin(), item.end());
    }
    else {
      bytes = task_empty_item(a.kind, parse.version);
    }
    if (bytes.empty()) {
      set_error(r_error, std::string("cannot create items of ") + a.name);
      return false;
    }
    splice(blob, pos, 0, bytes);
  }
  /* The count precedes its arrays, so its offset did not move. */
  Bytes tmp = blob;
  TaskParse count_parse = parse;
  std::string err;
  if (!task_set_int(tmp, count_parse, array.count_field, n + 1, &err)) {
    set_error(r_error, err);
    return false;
  }
  (void)count;
  blob.swap(tmp);
  return reparse(blob, parse, r_error);
}

bool task_array_remove(
    Bytes &blob, TaskParse &parse, int32_t array_field, uint32_t at, std::string *r_error)
{
  const TaskField array = parse.fields[array_field];
  if (array.type != TaskFieldType::Array || array.count_field < 0) {
    set_error(r_error, std::string(array.name) + " has a fixed size");
    return false;
  }
  const int64_t n = task_get_int(blob, parse, array.count_field);
  if (at >= n) {
    set_error(r_error, "item index out of range");
    return false;
  }
  std::vector<int32_t> arrays;
  for (int32_t i = 0; i < int32_t(parse.fields.size()); i++) {
    if (parse.fields[i].type == TaskFieldType::Array &&
        parse.fields[i].count_field == array.count_field)
    {
      arrays.push_back(i);
    }
  }
  for (auto it = arrays.rbegin(); it != arrays.rend(); ++it) {
    for (int32_t i = *it + 1; i < int32_t(parse.fields.size()); i++) {
      const TaskField &f = parse.fields[i];
      if (f.parent == *it && f.type == TaskFieldType::Item && f.index == at) {
        splice(blob, f.offset, f.size, {});
        break;
      }
    }
  }
  if (!task_set_int(blob, parse, array.count_field, n - 1, r_error)) {
    return false;
  }
  return reparse(blob, parse, r_error);
}

bool task_relink(Bytes &blob, TaskParse &parse, std::string *r_error)
{
  auto set_link = [&](const TaskNodeInfo &node, const char *name, uint32_t value) {
    const int32_t f = task_find_field(parse, node.field, name);
    if (f >= 0) {
      set_at<uint32_t>(blob, parse.fields[f].offset, value);
    }
  };
  for (size_t n = 0; n < parse.nodes.size(); n++) {
    const TaskNodeInfo &node = parse.nodes[n];
    /* Children in order. */
    std::vector<size_t> children;
    for (size_t k = n + 1; k < parse.nodes.size(); k++) {
      if (parse.nodes[k].parent == int32_t(n)) {
        children.push_back(k);
      }
    }
    set_link(node, "m_ulFirstChild", children.empty() ? 0 : parse.nodes[children[0]].id);
    for (size_t k = 0; k < children.size(); k++) {
      const TaskNodeInfo &child = parse.nodes[children[k]];
      set_link(child, "m_ulParent", node.id);
      set_link(child, "m_ulPrevSibling", k > 0 ? parse.nodes[children[k - 1]].id : 0);
      set_link(
          child, "m_ulNextSibling", k + 1 < children.size() ? parse.nodes[children[k + 1]].id : 0);
    }
  }
  return reparse(blob, parse, r_error);
}

std::vector<const char *> task_switch_names(uint32_t version)
{
  std::vector<const char *> names;
  if (version < 135) {
    return names;
  }
  for (const SwitchInfo &s : SWITCHES) {
    if (switch_exists(s, version)) {
      names.push_back(s.name);
    }
  }
  return names;
}

namespace {

int32_t switches_field(const TaskParse &parse, int32_t node)
{
  if (node < 0 || node >= int32_t(parse.nodes.size())) {
    return -1;
  }
  return task_find_field(parse, parse.nodes[node].field, "CombinedSwitches");
}

}  // namespace

bool task_get_switch(ByteSpan blob, const TaskParse &parse, int32_t node, std::string_view name)
{
  const int32_t f = switches_field(parse, node);
  const int bit = switch_bit(parse.version, name);
  if (f < 0 || bit < 0) {
    return false;
  }
  return (blob[parse.fields[f].offset + bit / 8] >> (bit % 8)) & 1;
}

bool task_set_switch(
    Bytes &blob, const TaskParse &parse, int32_t node, std::string_view name, bool value)
{
  const int32_t f = switches_field(parse, node);
  const int bit = switch_bit(parse.version, name);
  if (f < 0 || bit < 0) {
    return false;
  }
  uint8_t &byte = blob[parse.fields[f].offset + bit / 8];
  byte = value ? uint8_t(byte | (1 << (bit % 8))) : uint8_t(byte & ~(1 << (bit % 8)));
  /* Switching HasSign changes the record layout (the signature text). */
  return name != "HasSign";
}

/* -------------------------------------------------------------------- */
/** \name Pack
 * \{ */

bool parse_tasks(ByteSpan data, TasksData &r_data, std::string *r_error)
{
  r_data = TasksData();
  BinaryReader r(data);
  r_data.magic = r.u32();
  r_data.version = r.u32();
  const uint32_t count = r.u32();
  if (r.failed()) {
    set_error(r_error, "tasks.data: file too small");
    return false;
  }
  if (r_data.magic != TASK_PACK_MAGIC && r_data.magic != 0) {
    set_error(r_error, "tasks.data: bad magic");
    return false;
  }
  if (!task_version_supported(r_data.version)) {
    set_error(r_error, "tasks.data: unsupported version " + std::to_string(r_data.version));
    return false;
  }
  if (r_data.version >= 223) {
    r_data.header_extra = r.u32();
  }
  if (uint64_t(count) * 4 > r.remaining()) {
    set_error(r_error, "tasks.data: bad task count");
    return false;
  }
  std::vector<uint32_t> offsets(count);
  for (uint32_t i = 0; i < count; i++) {
    offsets[i] = r.u32();
  }
  const size_t header_end = r.tell();
  std::vector<uint32_t> sorted = offsets;
  std::sort(sorted.begin(), sorted.end());
  sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
  r_data.sequential_layout = count == 0 || (offsets[0] == header_end);
  for (uint32_t i = 1; i < count; i++) {
    if (offsets[i] <= offsets[i - 1]) {
      r_data.sequential_layout = false;
    }
  }
  r_data.tasks.resize(count);
  for (uint32_t i = 0; i < count; i++) {
    const uint32_t begin = offsets[i];
    if (begin < header_end || begin > data.size()) {
      set_error(r_error, "tasks.data: task offset out of range");
      return false;
    }
    const auto next = std::upper_bound(sorted.begin(), sorted.end(), begin);
    const size_t end = next == sorted.end() ? data.size() : *next;
    r_data.tasks[i].assign(data.begin() + begin, data.begin() + end);
  }
  return true;
}

bool write_tasks(const TasksData &data, Bytes &r_out, std::string *r_error)
{
  r_out.clear();
  if (!task_version_supported(data.version)) {
    set_error(r_error, "tasks.data: unsupported version");
    return false;
  }
  size_t total = 16 + data.tasks.size() * 4;
  for (const Bytes &t : data.tasks) {
    total += t.size();
  }
  if (total > UINT32_MAX) {
    set_error(r_error, "tasks.data: file too large");
    return false;
  }
  r_out.reserve(total);
  put<uint32_t>(r_out, data.magic);
  put<uint32_t>(r_out, data.version);
  put<uint32_t>(r_out, uint32_t(data.tasks.size()));
  if (data.version >= 223) {
    put<uint32_t>(r_out, data.header_extra);
  }
  uint32_t offset = uint32_t(r_out.size() + data.tasks.size() * 4);
  for (const Bytes &t : data.tasks) {
    put<uint32_t>(r_out, offset);
    offset += uint32_t(t.size());
  }
  for (const Bytes &t : data.tasks) {
    r_out.insert(r_out.end(), t.begin(), t.end());
  }
  return true;
}

bool is_task_pack_index(ByteSpan data)
{
  for (const int skip : {0, 4}) {
    if (data.size() < size_t(16 + skip)) {
      continue;
    }
    if (skip == 4 && get_at<uint32_t>(data, 0) != TASK_PACK_MAGIC) {
      continue;
    }
    const uint32_t packs = get_at<uint32_t>(data, skip + 12);
    if (packs > 0 && packs < 1024 && data.size() == size_t(skip + 16) + packs * 16) {
      return true;
    }
  }
  return false;
}

bool parse_task_pack_index(ByteSpan data, TaskPackIndex &r_index, std::string *r_error)
{
  r_index = TaskPackIndex();
  if (!is_task_pack_index(data)) {
    set_error(r_error, "not a tasks.data pack index");
    return false;
  }
  BinaryReader r(data);
  const uint32_t packs_if_magic = data.size() >= 20 ? get_at<uint32_t>(data, 16) : 0;
  r_index.has_magic = get_at<uint32_t>(data, 0) == TASK_PACK_MAGIC &&
                      data.size() == 20 + size_t(packs_if_magic) * 16;
  if (r_index.has_magic) {
    r_index.magic = r.u32();
  }
  r_index.version = r.u32();
  r_index.export_version = r.u32();
  r_index.item_count = r.u32();
  const uint32_t packs = r.u32();
  r_index.pack_md5.resize(packs);
  for (auto &md5 : r_index.pack_md5) {
    r.read_raw(md5.data(), 16);
  }
  return !r.failed();
}

void write_task_pack_index(const TaskPackIndex &index, Bytes &r_out)
{
  r_out.clear();
  if (index.has_magic) {
    put<uint32_t>(r_out, index.magic);
  }
  put<uint32_t>(r_out, index.version);
  put<uint32_t>(r_out, index.export_version);
  put<uint32_t>(r_out, index.item_count);
  put<uint32_t>(r_out, uint32_t(index.pack_md5.size()));
  for (const auto &md5 : index.pack_md5) {
    r_out.insert(r_out.end(), md5.begin(), md5.end());
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name task_npc.data and dyn_tasks.data
 * \{ */

bool parse_task_npc(ByteSpan data, TaskNpcData &r_data, std::string *r_error)
{
  r_data = TaskNpcData();
  BinaryReader r(data);
  r_data.pack_size = r.u32();
  r_data.time_mark = r.i32();
  r_data.version = r.u16();
  const uint16_t count = r.u16();
  if (r.failed()) {
    set_error(r_error, "task_npc.data: file too small");
    return false;
  }
  if (size_t(count) * 12 > r.remaining()) {
    set_error(r_error, "task_npc.data: truncated");
    return false;
  }
  r_data.npcs.resize(count);
  for (TaskNpcInfo &npc : r_data.npcs) {
    npc.id = r.u32();
    npc.x = r.i16();
    npc.y = r.i16();
    npc.z = r.i16();
    npc.pad = r.u16();
  }
  ByteSpan rest = r.read_span(r.remaining());
  r_data.trailing.assign(rest.begin(), rest.end());
  return true;
}

void write_task_npc(const TaskNpcData &data, Bytes &r_out, bool update_size)
{
  r_out.clear();
  const uint32_t size = uint32_t(12 + data.npcs.size() * 12);
  put<uint32_t>(r_out, update_size ? size : data.pack_size);
  put<int32_t>(r_out, data.time_mark);
  put<uint16_t>(r_out, data.version);
  put<uint16_t>(r_out, uint16_t(data.npcs.size()));
  for (const TaskNpcInfo &npc : data.npcs) {
    put<uint32_t>(r_out, npc.id);
    put<int16_t>(r_out, npc.x);
    put<int16_t>(r_out, npc.y);
    put<int16_t>(r_out, npc.z);
    put<uint16_t>(r_out, npc.pad);
  }
  r_out.insert(r_out.end(), data.trailing.begin(), data.trailing.end());
}

bool parse_dyn_tasks(ByteSpan data, DynTasksData &r_data, std::string *r_error)
{
  r_data = DynTasksData();
  BinaryReader r(data);
  r_data.pack_size = r.u32();
  r_data.time_mark = r.i32();
  r_data.version = r.u16();
  r_data.task_count = r.u16();
  if (r.failed()) {
    set_error(r_error, "dyn_tasks.data: file too small");
    return false;
  }
  ByteSpan rest = r.read_span(r.remaining());
  r_data.body.assign(rest.begin(), rest.end());
  return true;
}

void write_dyn_tasks(const DynTasksData &data, Bytes &r_out)
{
  r_out.clear();
  put<uint32_t>(r_out, data.pack_size);
  put<int32_t>(r_out, data.time_mark);
  put<uint16_t>(r_out, data.version);
  put<uint16_t>(r_out, data.task_count);
  r_out.insert(r_out.end(), data.body.begin(), data.body.end());
}

/** \} */

}  // namespace blender::io::angelica
