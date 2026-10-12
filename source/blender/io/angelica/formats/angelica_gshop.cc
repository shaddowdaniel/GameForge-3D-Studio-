/* SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup angelica
 */

#include <cstring>

#include "angelica_elements.hh"
#include "angelica_gshop.hh"

namespace blender::io::angelica {

namespace {

constexpr uint32_t MAX_ITEMS = 200000;
constexpr uint32_t MAX_SUB_CATEGORIES = 999;

void set_error(std::string *r_error, const std::string &message)
{
  if (r_error) {
    *r_error = message;
  }
}

/** Reads or writes the same field sequence, so the layout is described once. */
struct ItemReader {
  BinaryReader &r;
  template<typename T> void v(T &value)
  {
    value = T();
    r.read_raw(&value, sizeof(T));
  }
  template<size_t N> void raw(std::array<uint8_t, N> &buffer)
  {
    r.read_raw(buffer.data(), N);
  }
};

struct ItemWriter {
  Bytes &out;
  template<typename T> void v(const T &value)
  {
    const size_t at = out.size();
    out.resize(at + sizeof(T));
    std::memcpy(out.data() + at, &value, sizeof(T));
  }
  template<size_t N> void raw(const std::array<uint8_t, N> &buffer)
  {
    out.insert(out.end(), buffer.begin(), buffer.end());
  }
};

/** Counts the bytes of a layout. */
struct ItemSizer {
  size_t size = 0;
  template<typename T> void v(const T & /*value*/)
  {
    size += sizeof(T);
  }
  template<size_t N> void raw(const std::array<uint8_t, N> & /*buffer*/)
  {
    size += N;
  }
};

template<typename IO, typename Item> void io_item(IO &io, Item &item, const GShopVersion ver)
{
  using V = GShopVersion;
  io.v(item.id);
  io.v(item.category);
  io.v(item.sub_category);
  io.raw(item.icon);
  io.v(item.item_id);
  io.v(item.count);
  for (auto &option : item.options) {
    io.v(option.price);
    io.v(option.expire_date);
    io.v(option.duration);
    if (ver > V::PW_1_2_6) {
      io.v(option.start_date);
      io.v(option.control_type);
      io.v(option.day);
      io.v(option.status);
      io.v(option.flags);
      if (ver >= V::PW_1_5_5_v156) {
        io.v(option.min_vip_level);
      }
    }
  }
  if (ver == V::PW_1_2_6) {
    io.v(item.status_v126);
  }
  io.raw(item.description);
  io.raw(item.name);
  if (ver <= V::PW_1_4_2_v27) {
    return;
  }
  io.v(item.gift_id);
  io.v(item.gift_count);
  io.v(item.gift_duration);
  if (ver >= V::PW_1_4_4) {
    io.v(item.log_price);
  }
  if (ver >= V::PW_1_5_3) {
    for (auto &npc : item.owner_npcs) {
      io.v(npc);
    }
  }
  if (ver >= V::PW_1_5_5_v156) {
    io.v(item.buy_times_limit);
    io.v(item.buy_times_limit_mode);
  }
  if (ver >= V::PW_1_6_0) {
    io.v(item.tasks_id);
    io.v(item.tasks_space);
  }
  if (ver >= V::PW_1_5_5_v181) {
    io.v(item.occupation);
  }
  if (ver >= V::PW_1_7_6) {
    for (auto &value : item.reserved) {
      io.v(value);
    }
  }
  if (ver >= V::V9) {
    io.v(item.special_server_forbid);
    io.v(item.in_green_shop);
    if (ver == V::V9) {
      io.v(item.unknown_v9);
    }
  }
  if (ver >= V::V10) {
    io.v(item.add_trade_time);
    io.v(item.reserved6);
  }
}

/** Plausibility of a decoded item, used to reject wrong layouts early. */
bool item_plausible(const GShopItem &item)
{
  return item.category >= 0 && item.category <= 9999 && item.sub_category >= 0 &&
         item.sub_category <= 9999 && item.buy_times_limit_mode >= -10 &&
         item.buy_times_limit_mode <= 10 && item.item_id <= 10000000u;
}

enum class TryResult { Fail, Plausible, Exact };

TryResult try_parse(ByteSpan data, GShopVersion ver, GShopData &r_data, std::string *r_error)
{
  r_data = GShopData();
  r_data.version = ver;
  BinaryReader r(data);
  r_data.timestamp = r.u32();
  const uint32_t count = r.u32();
  const size_t item_size = gshop_item_size(ver);
  if (r.failed() || count > MAX_ITEMS || size_t(count) * item_size > r.remaining()) {
    set_error(r_error, "item table does not fit the file");
    return TryResult::Fail;
  }
  r_data.items.resize(count);
  ItemReader io{r};
  for (GShopItem &item : r_data.items) {
    io_item(io, item, ver);
    if (!item_plausible(item)) {
      set_error(r_error, "implausible item " + std::to_string(item.id));
      return TryResult::Fail;
    }
  }
  r_data.categories.resize(GSHOP_CATEGORIES);
  for (GShopCategory &category : r_data.categories) {
    r.read_raw(category.name.data(), category.name.size());
    const uint32_t subs = r.u32();
    if (r.failed() || subs > MAX_SUB_CATEGORIES || size_t(subs) * 128 > r.remaining()) {
      set_error(r_error, "invalid category table");
      return TryResult::Fail;
    }
    category.sub_categories.resize(subs);
    for (auto &sub : category.sub_categories) {
      r.read_raw(sub.data(), sub.size());
    }
  }
  if (r.failed()) {
    set_error(r_error, "category table is truncated");
    return TryResult::Fail;
  }
  ByteSpan rest = r.read_span(r.remaining());
  r_data.trailing.assign(rest.begin(), rest.end());
  return r_data.trailing.empty() ? TryResult::Exact : TryResult::Plausible;
}

}  // namespace

const char *gshop_version_name(GShopVersion version)
{
  switch (version) {
    case GShopVersion::Auto:
      return "auto";
    case GShopVersion::PW_1_2_6:
      return "1.2.6";
    case GShopVersion::PW_1_4_2_v27:
      return "1.4.2 (v27)";
    case GShopVersion::PW_1_4_2_v29:
      return "1.4.2 (v29)";
    case GShopVersion::PW_1_4_4:
      return "1.4.4";
    case GShopVersion::PW_1_5_3:
      return "1.5.3";
    case GShopVersion::PW_1_5_5_v156:
      return "1.5.5 (v156)";
    case GShopVersion::PW_1_5_5_v181:
      return "1.5.5 (v181)";
    case GShopVersion::PW_1_6_0:
      return "1.6.0";
    case GShopVersion::PW_1_7_6:
      return "1.7.6";
    case GShopVersion::V9:
      return "version 9";
    case GShopVersion::V10:
      return "version 10";
  }
  return "";
}

size_t gshop_item_size(GShopVersion version)
{
  ItemSizer sizer;
  GShopItem item;
  io_item(sizer, item, version);
  return sizer.size;
}

bool parse_gshop(ByteSpan data, GShopData &r_data, std::string *r_error, GShopVersion version)
{
  if (version != GShopVersion::Auto) {
    std::string err;
    if (try_parse(data, version, r_data, &err) == TryResult::Fail) {
      set_error(r_error,
                std::string("not a gshop file of layout ") + gshop_version_name(version) + ": " +
                    err);
      return false;
    }
    return true;
  }
  /* Newest layouts first: an exact fit wins, else the first plausible layout. */
  GShopData plausible;
  bool have_plausible = false;
  for (int v = int(GShopVersion::V10); v >= int(GShopVersion::PW_1_2_6); v--) {
    GShopData candidate;
    const TryResult result = try_parse(data, GShopVersion(v), candidate, nullptr);
    if (result == TryResult::Exact) {
      r_data = std::move(candidate);
      return true;
    }
    if (result == TryResult::Plausible && !have_plausible) {
      plausible = std::move(candidate);
      have_plausible = true;
    }
  }
  if (have_plausible) {
    r_data = std::move(plausible);
    return true;
  }
  set_error(r_error, "not a gshop file (no known item layout fits)");
  return false;
}

bool write_gshop(const GShopData &data, Bytes &r_out, std::string *r_error)
{
  if (data.version == GShopVersion::Auto) {
    set_error(r_error, "gshop layout not set");
    return false;
  }
  r_out.clear();
  r_out.reserve(8 + data.items.size() * gshop_item_size(data.version) + 4096 +
                data.trailing.size());
  ItemWriter io{r_out};
  io.v(data.timestamp);
  io.v(uint32_t(data.items.size()));
  for (const GShopItem &item : data.items) {
    io_item(io, item, data.version);
  }
  for (int i = 0; i < GSHOP_CATEGORIES; i++) {
    static const GShopCategory empty;
    const GShopCategory &category = i < int(data.categories.size()) ? data.categories[i] : empty;
    io.raw(category.name);
    io.v(uint32_t(category.sub_categories.size()));
    for (const auto &sub : category.sub_categories) {
      io.raw(sub);
    }
  }
  r_out.insert(r_out.end(), data.trailing.begin(), data.trailing.end());
  return true;
}

std::string gshop_utf16_text(ByteSpan buffer)
{
  size_t len = 0;
  while (len + 1 < buffer.size() && (buffer[len] | buffer[len + 1]) != 0) {
    len += 2;
  }
  return utf16le_to_utf8(buffer.first(len));
}

bool gshop_set_utf16_text(uint8_t *buffer,
                          size_t size,
                          std::string_view utf8,
                          std::string *r_error)
{
  if (gshop_utf16_text(ByteSpan(buffer, size)) == utf8) {
    return true;
  }
  Bytes text;
  if (!utf8_to_utf16le(utf8, text)) {
    set_error(r_error, "invalid UTF-8 text");
    return false;
  }
  if (text.size() + 2 > size) {
    set_error(r_error, "text is too long");
    return false;
  }
  std::memset(buffer, 0, size);
  std::memcpy(buffer, text.data(), text.size());
  return true;
}

std::string gshop_gbk_text(ByteSpan buffer)
{
  size_t len = 0;
  while (len < buffer.size() && buffer[len] != 0) {
    len++;
  }
  return gbk_to_utf8(std::string_view(reinterpret_cast<const char *>(buffer.data()), len));
}

bool gshop_set_gbk_text(uint8_t *buffer, size_t size, std::string_view utf8, std::string *r_error)
{
  if (gshop_gbk_text(ByteSpan(buffer, size)) == utf8) {
    return true;
  }
  std::string gbk;
  if (!utf8_to_gbk(utf8, gbk)) {
    set_error(r_error, "text cannot be encoded as GBK");
    return false;
  }
  if (gbk.size() + 1 > size) {
    set_error(r_error, "text is too long");
    return false;
  }
  std::memset(buffer, 0, size);
  std::memcpy(buffer, gbk.data(), gbk.size());
  return true;
}

}  // namespace blender::io::angelica
