# `elements.data` schemas

Perfect World stores its item, NPC, monster, mine and configuration database in
`element/data/elements.data`. The file is a sequence of lists of fixed size records. The record
layout changes with every game build, so GameForge 3D Studio reads the file with a schema for its
version.

**No schemas ship with GameForge 3D Studio.** The schemas are sELedit style `.cfg` files,
distributed as the `estudio-angelica-schemas` plugin on atomgameforge.com. After you sign in, the
Studio downloads the current version of the plugin (gated by your plan, checked against the
SHA-256 the portal signed) and keeps it in the user data files folder:
`gameforge_plugins/estudio-angelica-schemas/<version>/elements/`. Without a network connection the
last downloaded version is used. **Update GameForge Plugins** (`wm.gameforge_plugins_update`)
checks for a new version right away.

To publish a new schema set, build the release zip with
`tools/utils/gameforge/pack_angelica_schemas.py` and upload it in the portal's /admin › Versões.

The version is taken from the file name. For example, `PW_1.5.5_v156.cfg` is used for files whose
header version is 156, and `PW_1.7.8-seledit_v402.cfg` for version 402. The header version is the
low 16 bits of the file's first `uint32`. When several files carry the same version, the first one
in name order is used. The parser accepts any folder path, including paths with spaces or
non-ASCII characters.

Supported file versions:

- **Up to 190 (PW 1.5.x):** plain lists.
- **191 to 349 (PW 1.5.6 to 1.7.3):** every record count also has a `list_type` / `item_size`
  pair, which the reader handles.
- **350 and later (PW 1.7.4 to 1.7.8+), except 1488 and 30000+:** every record is compressed with
  MPPC (the `GNET::mppc` codec). These files also carry extra exporter blocks after the header.
  - Records that were not edited keep their original compressed bytes when written.
  - Edited and new records are compressed again.

## Format

```
<number of lists>
<index of the talk-proc list>

001 - EQUIPMENT_ADDON          list number and name
4                              bytes before the record count, or AUTO
ID;Name;num_params;...         field names
int32;wstring:64;int32;...     field types
```

Field types: `int16`, `int32` (`int`), `uint`, `int64`, `float`, `double`, `byte:N`,
`wstring:N` (N bytes of UTF-16LE text) and `string:N` (N bytes of GBK text, used for file paths).
All sizes are in bytes. Files may use CRLF line endings.

`AUTO` marks the exporter blocks the engine writes before some lists:

- Before 1.7.4: before list 0, `SKILLTOME_SUB_TYPE` and `NPC_WAR_TOWERBUILD_SERVICE`.
- From 1.7.4 on: all of them sit after the header.

These blocks are kept verbatim.

The talk-proc list holds the NPC dialogues. It has the single type `byte:AUTO` and a variable
size. Its index is on the second line of the file: after `NPC_ESSENCE` in 1.5.x schemas, last in
1.7.x schemas. Lists are in file order.

The parser is `source/blender/io/angelica/formats/angelica_elements.cc`. Data that was not edited
is written back byte for byte.
