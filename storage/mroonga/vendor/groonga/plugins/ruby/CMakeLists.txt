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
  add_library(eval MODULE ${RUBY_EVAL_SOURCES})
  set_source_files_properties(${RUBY_EVAL_SOURCES}
    PROPERTIES
    COMPILE_FLAGS "${GRN_C_COMPILE_FLAGS}")
  set_target_properties(eval PROPERTIES PREFIX "")
  target_link_libraries(eval libgroonga)
  install(TARGETS eval DESTINATION "${GRN_RELATIVE_RUBY_PLUGINS_DIR}")

  read_file_list(${CMAKE_CURRENT_SOURCE_DIR}/load_sources.am RUBY_LOAD_SOURCES)
  add_library(load MODULE ${RUBY_LOAD_SOURCES})
  set_source_files_properties(${RUBY_LOAD_SOURCES}
    PROPERTIES
    COMPILE_FLAGS "${GRN_C_COMPILE_FLAGS}")
  set_target_properties(load PROPERTIES PREFIX "")
  target_link_libraries(load libgroonga)
  install(TARGETS load DESTINATION "${GRN_RELATIVE_RUBY_PLUGINS_DIR}")
endif()
