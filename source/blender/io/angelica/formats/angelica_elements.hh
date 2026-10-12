/* SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup angelica
 *
 * Reader and writer for the Perfect World item / NPC / monster database
 * `element/data/elements.data`
 * (`elementdataman::load_data` in the client and server).
 *
 * The file is a versioned sequence of lists of fixed size records. Record layouts change with
 * every game build, so they are described by sELedit style `.cfg` schema files, one per file
 * version, loaded from a folder the user configures (see
 * `release/datafiles/angelica/elements/README.md` and `angelica_elements.cc` for the
 * layout).
 *
 * Everything that is not modelled field by field (list prefixes such as the exporter tag blocks,
 * the talk-proc list, trailing bytes) is kept as raw bytes, and records keep their raw bytes too,
 * so #parse_elements followed by #write_elements reproduces the input byte for byte. Versions 350+
 * (PW 1.7.4 and later) compress each record; the original compressed bytes of records that were
 * not changed are kept and written back as they were.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "angelica_reader.hh"

namespace blender::io::angelica {

/* -------------------------------------------------------------------- */
/** \name Schema
 * \{ */

enum class ElementFieldType : uint8_t {
  Int16,
  Int32,
  UInt32,
  Int64,
  Float,
  Double,
  /** `byte:N`: opaque bytes. */
  Raw,
  /** `wstring:N`: N bytes of NUL padded UTF-16LE text. */
  WString,
  /** `string:N`: N bytes of NUL padded GBK text (file paths). */
  String,
};

struct ElementFieldDef {
  std::string name;
  ElementFieldType type = ElementFieldType::Int32;
  /** Size in bytes. */
  uint32_t size = 0;
  /** Offset inside the record. */
  uint32_t offset = 0;
};

/** Engine `ID_SPACE` a list belongs to. Ids are only unique inside one space. */
enum class ElementIdSpace : uint8_t {
  Essence = 0,
  Addon = 1,
  Talk = 2,
  Face = 3,
  Recipe = 4,
  Config = 5,
};

struct ElementListDef {
  /** List name without the number, for example `NPC_ESSENCE`. */
  std::string name;
  /** Name line as written in the `.cfg`, for example `057 - NPC_ESSENCE`. */
  std::string label;
  /** Number in the label (1-based), -1 when the label has none. */
  int label_number = -1;
  /** The list is preceded by an `AUTO` block (exporter tag, see `angelica_elements.cc`). */
  bool prefix_auto = false;
  /** Bytes preceding the record count when not #prefix_auto (for list 0 this includes the u32
   * export time that follows the header). */
  uint32_t prefix_size = 0;
  /** The variable size talk-proc list (`RAW` / `byte:AUTO`). Has no fields. */
  bool is_talk_proc = false;
  ElementIdSpace id_space = ElementIdSpace::Essence;
  std::vector<ElementFieldDef> fields;
  uint32_t record_size = 0;

  /** Index of a field by exact name, -1 when missing. */
  int field_index(std::string_view field_name) const;
};

struct ElementsSchema {
  /** Version from the file name (`PW_1.5.5_v156.cfg` -> 156), 0 when unknown. */
  uint32_t version = 0;
  /** File name or description of the source, for messages. */
  std::string source_name;
  /** Lists in file order. */
  std::vector<ElementListDef> lists;
  /** Index of the talk-proc list in #lists, -1 when there is none. */
  int talk_proc_index = -1;

  /** Index of a list by name (`NPC_ESSENCE`) or label (`057 - NPC_ESSENCE`), -1 when missing. */
  int list_index(std::string_view list_name) const;
};

/**
 * Parse an sELedit `.cfg` schema:
 *
 *     <list count>
 *     <talk-proc list index>
 *     <blank line>
 *     NNN - LIST_NAME          (repeated per list, separated by blank lines)
 *     <prefix: byte count or AUTO>
 *     field;names;...
 *     int32;wstring:64;...     (int16 int32 int uint int64 float double byte:N wstring:N string:N)
 *
 * Lists are in file order. The talk-proc list is the one typed `byte:AUTO` (normally at the index
 * given on the second line: 58 in 1.5.x schemas, the last list in 1.7.x schemas).
 */
bool load_elements_schema(std::string_view cfg_text,
                          ElementsSchema &r_schema,
                          std::string *r_error);
bool load_elements_schema_file(const std::string &filepath,
                               ElementsSchema &r_schema,
                               std::string *r_error);

/** Version encoded in a schema file name: `PW_1.5.5_v156.cfg` -> 156, 0 when there is none. */
uint32_t elements_schema_version_from_filename(std::string_view filename);

struct ElementsSchemaFile {
  std::string path;
  uint32_t version = 0;
};

/** All `*.cfg` schema files of a folder with a version in their name, sorted by version. */
std::vector<ElementsSchemaFile> list_elements_schemas(const std::string &folder);

/**
 * Load the schema matching a file version from a folder of `.cfg` files. When several files carry
 * the same version the first in name order is used. Returns false when there is none.
 */
bool load_elements_schema_for_version(const std::string &folder,
                                      uint32_t version,
                                      ElementsSchema &r_schema,
                                      std::string *r_error);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Data
 * \{ */

/** Decoded field value: integers, floating point numbers, UTF-8 text or opaque bytes. */
using ElementValue = std::variant<int64_t, double, std::string, Bytes>;

/** Index entry of a compressed record (versions 350+): `u32 id, u16 size` plus its data. */
struct ElementsPackedSlot {
  uint32_t id = 0;
  /** Offset into #ElementsList::packed_data. */
  uint32_t offset = 0;
  uint16_t size = 0;
};

struct ElementsList {
  /** Index into #ElementsSchema::lists. */
  int def_index = -1;
  /** Raw bytes preceding the record count (fixed prefix or AUTO block). */
  Bytes prefix;
  uint32_t record_size = 0;
  /** `list_type` word stored before the count by versions 191..349, 0 otherwise. */
  int32_t list_type = 0;
  /** Records, `record_size` bytes each. Empty for the talk-proc list. */
  Bytes records;
  /** Talk-proc list only: the raw list after the prefix (count + all talk procs). */
  Bytes raw;
  /** Talk-proc list only: number of talk procs in #raw. */
  uint32_t raw_count = 0;
  /**
   * Packed versions only: the compressed records as read, one slot per record of #records (slots
   * follow #ElementsData::remove_record; records past the end have none). When writing, a
   * record whose bytes still decompress from its slot keeps the original compressed bytes and
   * index id, so untouched records are written byte for byte; others are compressed again.
   */
  std::vector<ElementsPackedSlot> packed;
  /** Packed versions only: the compressed bytes the slots point into. */
  Bytes packed_data;

  size_t size() const
  {
    return record_size ? records.size() / record_size : size_t(raw_count);
  }
  ByteSpan record(size_t index) const
  {
    return ByteSpan(records).subspan(index * record_size, record_size);
  }
};

struct ElementRef {
  int list = -1;
  size_t record = 0;

  bool valid() const
  {
    return list >= 0;
  }
};

struct ElementsData {
  /** Header word: u16 version, u16 signature (0x3000 for PW), for example `0x3000009C`. */
  uint32_t header = 0;
  std::shared_ptr<const ElementsSchema> schema;
  /** One entry per schema list, in file order. */
  std::vector<ElementsList> lists;
  /** Bytes after the last list (normally none). */
  Bytes trailing;

  /** Database version (low 16 bits of the header, 156 for PW 1.5.5). */
  uint32_t version() const
  {
    return header & 0xFFFF;
  }

  const ElementListDef *list_def(int list) const;
  int list_index(std::string_view list_name) const;

  /** Id of a record: its first field when that is an integer, 0 otherwise. */
  uint32_t record_id(int list, size_t record) const;
  /** First record of a list with this id. */
  ElementRef find(std::string_view list_name, uint32_t id) const;
  ElementRef find(int list, uint32_t id) const;
  /** First record with this id in any list of an id space, in file order. */
  ElementRef find_in_space(ElementIdSpace space, uint32_t id) const;

  /** Decode one field. Returns false when the indices are out of range. */
  bool get(int list, size_t record, int field, ElementValue &r_value) const;
  bool get(ElementRef ref, std::string_view field_name, ElementValue &r_value) const;
  /** Convenience accessors, `fallback` when the field is missing or of another kind. */
  int64_t get_int(ElementRef ref, std::string_view field_name, int64_t fallback = 0) const;
  double get_float(ElementRef ref, std::string_view field_name, double fallback = 0.0) const;
  std::string get_string(ElementRef ref, std::string_view field_name) const;
  /** Decode a whole record, one value per schema field. */
  bool decode_record(int list, size_t record, std::vector<ElementValue> &r_values) const;

  /**
   * Set a field with type checking: integer fields take `int64_t` (range checked), float fields
   * take `double` or `int64_t`, text fields take UTF-8 `std::string` that must fit the field once
   * encoded, `byte:N` fields take exactly N bytes. Bytes after a text terminator are zeroed.
   */
  bool set(int list, size_t record, int field, const ElementValue &value, std::string *r_error);
  bool set(ElementRef ref,
           std::string_view field_name,
           const ElementValue &value,
           std::string *r_error);

  /**
   * Append a record to a list, zero filled or copied from `copy_from` (a record of the same list),
   * with its id field set to `id` when non-zero. Returns the new record index, or -1.
   */
  int64_t add_record(int list, uint32_t id = 0, std::optional<size_t> copy_from = {});
  /** Remove a record. Returns false when out of range. */
  bool remove_record(int list, size_t record);
  /** Largest id used in a list (0 when empty), to pick a new one. */
  uint32_t max_id(int list) const;
};

/** Bytes tolerated after the last list before a schema is considered not to match the file. */
constexpr size_t ELEMENTS_MAX_TRAILING = 4096;

/**
 * Versions 350 and later (PW 1.7.4+, except 1488 and >= 30000) compress every record with MPPC
 * and store extra exporter blocks after the header (kept in the prefix of list 0).
 */
bool elements_version_is_packed(uint32_t version);
/** Versions 191..349 store `i32 list_type` before and `u32 item_size` after each record count. */
bool elements_version_has_list_headers(uint32_t version);

/** Read the version number (first u16) of an `elements.data`. */
bool peek_elements_version(ByteSpan data, uint32_t &r_version, std::string *r_error);

/**
 * Parse `elements.data` with a schema. The whole file must be consumed by the schema lists; up to
 * #ELEMENTS_MAX_TRAILING bytes left after the last list are kept in #ElementsData::trailing, more
 * is reported as a schema mismatch. Returns false with a message on malformed input.
 */
bool parse_elements(ByteSpan data,
                    const ElementsSchema &schema,
                    ElementsData &r_data,
                    std::string *r_error);

/** Serialize. Unedited data written with the schema it was parsed with is byte identical. */
bool write_elements(const ElementsData &data,
                    const ElementsSchema &schema,
                    Bytes &r_out,
                    std::string *r_error);

/**
 * MPPC compression of packed records, the bit stream of `GNET::mppc`: records up to 8192 bytes
 * are one block (`compress` / `uncompress`), larger ones a sequence of blocks of at most 8192
 * input bytes each preceded by a u16 (`compress2` / `uncompress2`: bit 15 set for a compressed
 * block, else the block is stored). Decompression fails unless exactly `size` bytes result.
 */
bool mppc_compress_record(ByteSpan data, Bytes &r_packed);
bool mppc_decompress_record(ByteSpan packed, size_t size, Bytes &r_data);

/* Talk procs (NPC dialogue trees, `talk_proc` in ExpTypes.h), decoded read-only from the raw list.
 */

struct TalkOption {
  /** Child window id, or a predefined function when the top bit is set. */
  uint32_t id = 0;
  std::string text;
  uint32_t param = 0;
};

struct TalkWindow {
  uint32_t id = 0;
  uint32_t id_parent = 0;
  std::string text;
  std::vector<TalkOption> options;
};

struct TalkProc {
  uint32_t id_talk = 0;
  std::string text;
  std::vector<TalkWindow> windows;
};

bool parse_talk_procs(const ElementsList &list,
                      std::vector<TalkProc> &r_procs,
                      std::string *r_error);

/* Data the 3D studio needs to place NPCs, monsters and mines. */

struct ElementModelInfo {
  ElementRef ref;
  std::string list_name;
  uint32_t id = 0;
  std::string name;
  /** Model path as stored (UTF-8, engine separators), for example `models\npcs\...\x.ecm`. */
  std::string file_model;
  /** `size` field (monster / pet body size), or the source monster size of a service NPC. */
  float size = 0.0f;
  bool has_size = false;
};

/** Model information of one record of a list with a `file_model` field. */
bool element_model_info(const ElementsData &data, ElementRef ref, ElementModelInfo &r_info);
/** Look an id up in the essence space and return its model info when the record has a model. */
bool find_element_model(const ElementsData &data, uint32_t id, ElementModelInfo &r_info);
/** Model info of every record of every list with a `file_model` field. */
std::vector<ElementModelInfo> collect_element_models(const ElementsData &data);

/** \} */

/* -------------------------------------------------------------------- */
/** \name Helpers shared by the data writers
 * \{ */

/** UTF-16LE (NUL terminated or padded) to UTF-8. */
std::string utf16le_to_utf8(ByteSpan bytes);
/** UTF-8 to UTF-16LE, without terminator. Returns false on invalid UTF-8. */
bool utf8_to_utf16le(std::string_view utf8, Bytes &r_out);
/** UTF-8 to GBK (GB18030 on POSIX). Returns false when a character has no GBK encoding. */
bool utf8_to_gbk(std::string_view utf8, std::string &r_gbk);

/**
 * Save a data file safely: the data is written to `<file>.tmp` and renamed over `<file>`. The
 * first time an existing file is replaced it is copied to `<file>.bak`; an existing `.bak` is
 * never overwritten, so it always holds the original.
 */
bool save_file_with_backup(const std::string &filepath, ByteSpan data, std::string *r_error);

/** \} */

}  // namespace blender::io::angelica
