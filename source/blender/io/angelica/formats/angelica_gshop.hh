/* SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup angelica
 *
 * Reader and writer for the item shop files of Perfect World: `data\gshop.data` (cash shop),
 * `data\gshop1.data` and `data\gshop2.data` (secondary shops), loaded by `globaldata_loadgshop`.
 *
 * Layout: `uint32 timestamp`, `int32 item count`, the items, then 8 main categories (UTF-16 name
 * and sub category names). The item record grew over the client releases; the layout is not
 * versioned in the file, so #parse_gshop tries every known layout and keeps the one that ends
 * exactly at the end of the file (or that the caller names).
 *
 * Text buffers are kept raw so bytes after the terminator survive a round trip.
 */

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "angelica_reader.hh"

namespace blender::io::angelica {

/** Item record layouts, oldest first. */
enum class GShopVersion : uint8_t {
  Auto = 0,
  PW_1_2_6,
  PW_1_4_2_v27,
  PW_1_4_2_v29,
  PW_1_4_4,
  PW_1_5_3,
  PW_1_5_5_v156,
  PW_1_5_5_v181,
  PW_1_6_0,
  PW_1_7_6,
  V9,
  V10,
};

const char *gshop_version_name(GShopVersion version);
/** Size in bytes of one item record of `version`. */
size_t gshop_item_size(GShopVersion version);

struct GShopSaleOption {
  uint32_t price = 0;
  uint32_t expire_date = 0;
  uint32_t duration = 0;
  /* Newer than 1.2.6. */
  uint32_t start_date = 0;
  int32_t control_type = -1;
  uint32_t day = 0;
  uint32_t status = 0;
  uint32_t flags = 0;
  /* 1.5.5 (v156) and later. */
  uint32_t min_vip_level = 0;
};

constexpr int GSHOP_SALE_OPTIONS = 4;
constexpr int GSHOP_OWNER_NPCS = 8;
constexpr int GSHOP_CATEGORIES = 8;

struct GShopItem {
  int32_t id = 0;
  int32_t category = 0;
  int32_t sub_category = 0;
  /** GBK icon path (`surfaces\...`). */
  std::array<uint8_t, 128> icon{};
  uint32_t item_id = 0;
  uint32_t count = 1;
  std::array<GShopSaleOption, GSHOP_SALE_OPTIONS> options{};
  /** 1.2.6 only: one status for the whole item. */
  uint32_t status_v126 = 0;
  /** UTF-16 description (512 characters). */
  std::array<uint8_t, 1024> description{};
  /** UTF-16 name (32 characters). */
  std::array<uint8_t, 64> name{};
  /* Newer than 1.4.2 (v27). */
  uint32_t gift_id = 0;
  uint32_t gift_count = 0;
  uint32_t gift_duration = 0;
  /* 1.4.4 and later. */
  uint32_t log_price = 0;
  /* 1.5.3 and later. */
  std::array<uint32_t, GSHOP_OWNER_NPCS> owner_npcs{};
  /* 1.5.5 (v156) and later. */
  int32_t buy_times_limit = 0;
  int32_t buy_times_limit_mode = 0;
  /* 1.6.0 and later. */
  int32_t tasks_id = 0;
  int32_t tasks_space = 0;
  /* 1.5.5 (v181) and later. */
  int32_t occupation = 0;
  /* 1.7.6 and later. */
  std::array<int32_t, 4> reserved{};
  /* Version 9 and later. */
  int32_t special_server_forbid = 0;
  uint8_t in_green_shop = 0;
  /** Version 9 only. */
  uint8_t unknown_v9 = 0;
  /* Version 10 and later. */
  int32_t add_trade_time = 0;
  int32_t reserved6 = 0;
};

struct GShopCategory {
  /** UTF-16 name (64 characters). */
  std::array<uint8_t, 128> name{};
  std::vector<std::array<uint8_t, 128>> sub_categories;
};

struct GShopData {
  GShopVersion version = GShopVersion::Auto;
  uint32_t timestamp = 0;
  std::vector<GShopItem> items;
  /** Always #GSHOP_CATEGORIES entries after parsing. */
  std::vector<GShopCategory> categories;
  /** Bytes after the categories, kept verbatim. */
  Bytes trailing;
};

/**
 * Parse a shop file. With `version == GShopVersion::Auto` every layout is tried (newest first)
 * and the first one that consumes the file exactly wins; failing that, the first plausible one.
 */
bool parse_gshop(ByteSpan data,
                 GShopData &r_data,
                 std::string *r_error,
                 GShopVersion version = GShopVersion::Auto);
/** Writes with `data.version` (must not be Auto). */
bool write_gshop(const GShopData &data, Bytes &r_out, std::string *r_error);

/** Fixed UTF-16 buffer helpers (stop at the terminator; setting fails when too long). */
std::string gshop_utf16_text(ByteSpan buffer);
bool gshop_set_utf16_text(uint8_t *buffer,
                          size_t size,
                          std::string_view utf8,
                          std::string *r_error);
/** Fixed GBK buffer helpers. */
std::string gshop_gbk_text(ByteSpan buffer);
bool gshop_set_gbk_text(uint8_t *buffer, size_t size, std::string_view utf8, std::string *r_error);

}  // namespace blender::io::angelica
