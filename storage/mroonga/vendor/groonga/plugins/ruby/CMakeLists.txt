# Copyright(C) 2013 Brazil
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
  ${MRUBY_INCLUDE_DIRS})

if(GRN_WITH_MRUBY)
  set(GRN_RELATIVE_RUBY_PLUGINS_DIR "${GRN_RELATIVE_PLUGINS_DIR}/ruby")

  read_file_list(${CMAKE_CURRENT_SOURCE_DIR}/eval_sources.am RUBY_EVAL_SOURCES)
  set_source_files_properties(${RUBY_EVAL_SOURCES}
    PROPERTIES
    COMPILE_FLAGS "${GRN_C_COMPILE_FLAGS}")
  if(GRN_EMBED)
    add_library(ruby_eval STATIC ${RUBY_EVAL_SOURCES})
    set_target_properties(
      ruby_eval
      PROPERTIES
      POSITION_INDEPENDENT_CODE ON)
  else()
    add_library(ruby_eval MODULE ${RUBY_EVAL_SOURCES})
    set_target_properties(ruby_eval PROPERTIES
      PREFIX ""
      OUTPUT_NAME "eval")
    install(TARGETS ruby_eval DESTINATION "${GRN_RELATIVE_RUBY_PLUGINS_DIR}")
  endif()
  target_link_libraries(ruby_eval libgroonga)

  read_file_list(${CMAKE_CURRENT_SOURCE_DIR}/load_sources.am RUBY_LOAD_SOURCES)
  set_source_files_properties(${RUBY_LOAD_SOURCES}
    PROPERTIES
    COMPILE_FLAGS "${GRN_C_COMPILE_FLAGS}")
  if(GRN_EMBED)
    add_library(ruby_load STATIC ${RUBY_LOAD_SOURCES})
    set_target_properties(
      ruby_load
      PROPERTIES
      POSITION_INDEPENDENT_CODE ON)
  else()
    add_library(ruby_load MODULE ${RUBY_LOAD_SOURCES})
    set_target_properties(ruby_load PROPERTIES
      PREFIX ""
      OUTPUT_NAME "load")
    install(TARGETS ruby_load DESTINATION "${GRN_RELATIVE_RUBY_PLUGINS_DIR}")
  endif()
  target_link_libraries(ruby_load libgroonga)
endif()
