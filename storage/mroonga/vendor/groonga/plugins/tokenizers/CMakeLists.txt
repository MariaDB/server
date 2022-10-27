# Copyright(C) 2012-2013 Brazil
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

include_directories(
  ${CMAKE_CURRENT_SOURCE_DIR}/../../lib
  )

set(TOKENIZERS_DIR "${GRN_RELATIVE_PLUGINS_DIR}/tokenizers")
if(GRN_WITH_MECAB)
  read_file_list(${CMAKE_CURRENT_SOURCE_DIR}/mecab_sources.am MECAB_SOURCES)
  include_directories(${MECAB_INCLUDE_DIRS})
  link_directories(${MECAB_LIBRARY_DIRS})
  if(GRN_WITH_BUNDLED_MECAB)
    set(GRN_BUNDLED_MECAB_RELATIVE_RC_PATH "${CONFIG_DIR}/mecabrc")
    set(MECAB_COMPILE_DEFINITIONS
      "GRN_WITH_BUNDLED_MECAB"
      "GRN_BUNDLED_MECAB_RELATIVE_RC_PATH=\"${GRN_BUNDLED_MECAB_RELATIVE_RC_PATH}\""
      "GRN_BUNDLED_MECAB_RC_PATH=\"${CMAKE_INSTALL_PREFIX}/${GRN_BUNDLED_MECAB_RELATIVE_RC_PATH}\"")
    set_source_files_properties(${MECAB_SOURCES}
      PROPERTIES
      COMPILE_DEFINITIONS
      "${MECAB_COMPILE_DEFINITIONS}")
  endif()
  set_source_files_properties(${MECAB_SOURCES}
    PROPERTIES
    COMPILE_FLAGS "${GRN_C_COMPILE_FLAGS}")
  if(GRN_EMBED)
    add_library(mecab_tokenizer STATIC ${MECAB_SOURCES})
    set_target_properties(
      mecab_tokenizer
      PROPERTIES
      POSITION_INDEPENDENT_CODE ON)
  else()
    add_library(mecab_tokenizer MODULE ${MECAB_SOURCES})
    set_target_properties(mecab_tokenizer PROPERTIES
      PREFIX ""
      OUTPUT_NAME "mecab")
    install(TARGETS mecab_tokenizer DESTINATION "${TOKENIZERS_DIR}")
  endif()
  target_link_libraries(mecab_tokenizer libgroonga ${MECAB_LIBRARIES})
endif()

if(GRN_WITH_KYTEA)
  read_file_list(${CMAKE_CURRENT_SOURCE_DIR}/kytea_sources.am KYTEA_SOURCES)
  include_directories(${KYTEA_INCLUDE_DIRS})
  link_directories(${KYTEA_LIBRARY_DIRS})
  set_source_files_properties(${KYTEA_SOURCES}
    PROPERTIES
    COMPILE_FLAGS "${GRN_CXX_COMPILE_FLAGS}")
  if(GRN_EMBED)
    add_library(kytea_tokenizer STATIC ${KYTEA_SOURCES})
    set_target_properties(
      kytea_tokenizer
      PROPERTIES
      POSITION_INDEPENDENT_CODE ON)
  else()
    add_library(kytea_tokenizer MODULE ${KYTEA_SOURCES})
    set_target_properties(kytea_tokenizer PROPERTIES
      PREFIX ""
      OUTPUT_NAME "kytea")
    install(TARGETS kytea_tokenizer DESTINATION "${TOKENIZERS_DIR}")
  endif()
  target_link_libraries(kytea_tokenizer libgroonga ${KYTEA_LIBRARIES})
endif()
