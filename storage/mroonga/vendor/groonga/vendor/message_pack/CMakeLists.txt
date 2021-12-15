# Copyright(C) 2016 Brazil
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License version 2.1 as published by the Free Software Foundation.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA

set(MESSAGE_PACK_VERSION ${GRN_BUNDLED_MESSAGE_PACK_VERSION})

set(MESSAGE_PACK_SOURCE_DIR
  "${CMAKE_CURRENT_SOURCE_DIR}/../msgpack-${MESSAGE_PACK_VERSION}")

if(GRN_WITH_BUNDLED_MESSAGE_PACK)
  include_directories(
    BEFORE
    ${MESSAGE_PACK_SOURCE_DIR}/include
    )

  set(MESSAGE_PACK_SOURCES
    "${MESSAGE_PACK_SOURCE_DIR}/src/objectc.c"
    "${MESSAGE_PACK_SOURCE_DIR}/src/unpack.c"
    "${MESSAGE_PACK_SOURCE_DIR}/src/version.c"
    "${MESSAGE_PACK_SOURCE_DIR}/src/vrefbuffer.c"
    "${MESSAGE_PACK_SOURCE_DIR}/src/zone.c"
    )

  set_source_files_properties(${MESSAGE_PACK_SOURCES}
    PROPERTIES
    COMPILE_FLAGS "${GRN_C_COMPILE_FLAGS}")

  add_library(msgpackc SHARED ${MESSAGE_PACK_SOURCES})
  install(TARGETS msgpackc
    ARCHIVE DESTINATION "${LIB_DIR}"
    LIBRARY DESTINATION "${LIB_DIR}"
    RUNTIME DESTINATION "${BIN_DIR}")
  install(DIRECTORY
    "${MESSAGE_PACK_SOURCE_DIR}/include/"
    DESTINATION "${INCLUDE_DIR}"
    FILES_MATCHING PATTERN "*.h")
  install(FILES
    "${MESSAGE_PACK_SOURCE_DIR}/COPYING"
    "${MESSAGE_PACK_SOURCE_DIR}/LICENSE_1_0.txt"
    "${MESSAGE_PACK_SOURCE_DIR}/README.md"
    DESTINATION "${GRN_DATA_DIR}/msgpack")
endif()
