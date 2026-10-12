# SPDX-FileCopyrightText: 2026 GameForge 3D Studio Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

set(INC
  .
  formats
  importer
  ../common
  ../../editors/include
  ../../makesrna
)

set(INC_SYS
)

set(SRC
  IO_angelica.cc
  IO_angelica_character.cc
  formats/angelica_anim.cc
  formats/angelica_hmap.cc
  formats/angelica_model.cc
  formats/angelica_pck.cc
  formats/angelica_reader.cc
  formats/angelica_skeleton.cc
  formats/angelica_skin.cc
  formats/angelica_static.cc
  formats/angelica_terrain.cc
  formats/angelica_vfs.cc
  importer/angelica_import_character.cc
  importer/angelica_import_common.cc
  importer/angelica_import_model.cc
  importer/angelica_import_package.cc
  importer/angelica_import_terrain.cc

  IO_angelica.hh
  IO_angelica_character.hh
  formats/angelica_anim.hh
  formats/angelica_hmap.hh
  formats/angelica_model.hh
  formats/angelica_pck.hh
  formats/angelica_reader.hh
  formats/angelica_skeleton.hh
  formats/angelica_skin.hh
  formats/angelica_static.hh
  formats/angelica_terrain.hh
  formats/angelica_vfs.hh
  importer/angelica_import_character.hh
  importer/angelica_import_common.hh
  importer/angelica_import_model.hh
  importer/angelica_import_package.hh
  importer/angelica_import_terrain.hh
)

set(LIB
  PRIVATE bf::animrig
  PRIVATE bf::blenkernel
  PRIVATE bf::blenlib
  PRIVATE bf::blentranslation
  PRIVATE bf::depsgraph
  PRIVATE bf::dna
  PRIVATE bf::imbuf
  PRIVATE bf::intern::clog
  PRIVATE bf::intern::guardedalloc
  PRIVATE bf::nodes
  PRIVATE bf::windowmanager
  PRIVATE bf::dependencies::zlib
  bf_io_common
)

if(WIN32)
  # GBK (code page 936) decoding uses the Win32 API, POSIX systems use iconv.
else()
  find_package(Iconv)
  if(Iconv_FOUND AND NOT Iconv_IS_BUILT_IN)
    list(APPEND INC_SYS ${Iconv_INCLUDE_DIRS})
    list(APPEND LIB ${Iconv_LIBRARIES})
  endif()
endif()

blender_add_lib(bf_io_angelica "${SRC}" "${INC}" "${INC_SYS}" "${LIB}")

if(WITH_GTESTS)
  set(TEST_SRC
    tests/angelica_character_test.cc
    tests/angelica_model_formats_test.cc
    tests/angelica_world_formats_test.cc
  )

  set(TEST_INC
    ${INC}
    ../../../../tests/gtests
  )

  set(TEST_LIB
    ${LIB}
    bf_io_angelica
  )

  blender_add_test_suite_lib(io_angelica "${TEST_SRC}" "${TEST_INC}" "${INC_SYS}" "${TEST_LIB}")
endif()
