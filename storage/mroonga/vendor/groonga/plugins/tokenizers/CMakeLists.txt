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
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA

include_directories(
  ${CMAKE_CURRENT_SOURCE_DIR}/../../lib
  )

set(TOKENIZERS_DIR "${GRN_RELATIVE_PLUGINS_DIR}/tokenizers")
if(GRN_WITH_MECAB)
  read_file_list(${CMAKE_CURRENT_SOURCE_DIR}/mecab_sources.am MECAB_SOURCES)
  include_directories(${MECAB_INCLUDE_DIRS})
  link_directories(${MECAB_LIBRARY_DIRS})
  add_library(mecab_tokenizer MODULE ${MECAB_SOURCES})
  set_source_files_properties(${MECAB_SOURCES}
    PROPERTIES
    COMPILE_FLAGS "${GRN_C_COMPILE_FLAGS}")
  set_target_properties(mecab_tokenizer PROPERTIES
    PREFIX ""
    OUTPUT_NAME "mecab")
  target_link_libraries(mecab_tokenizer libgroonga ${MECAB_LIBRARIES})
  install(TARGETS mecab_tokenizer DESTINATION "${TOKENIZERS_DIR}")
endif()

if(GRN_WITH_KYTEA)
  read_file_list(${CMAKE_CURRENT_SOURCE_DIR}/kytea_sources.am KYTEA_SOURCES)
  include_directories(${KYTEA_INCLUDE_DIRS})
  link_directories(${KYTEA_LIBRARY_DIRS})
  add_library(kytea_tokenizer MODULE ${KYTEA_SOURCES})
  set_source_files_properties(${KYTEA_SOURCES}
    PROPERTIES
    COMPILE_FLAGS "${GRN_CXX_COMPILE_FLAGS}")
  set_target_properties(kytea_tokenizer PROPERTIES
    PREFIX ""
    OUTPUT_NAME "kytea")
  target_link_libraries(kytea_tokenizer libgroonga ${KYTEA_LIBRARIES})
  install(TARGETS kytea_tokenizer DESTINATION "${TOKENIZERS_DIR}")
endif()
