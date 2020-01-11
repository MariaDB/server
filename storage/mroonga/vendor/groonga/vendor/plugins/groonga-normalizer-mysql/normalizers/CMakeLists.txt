# Copyright(C) 2013-2015  Kouhei Sutou <kou@clear-code.com>
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Library General Public
# License as published by the Free Software Foundation; version 2
# of the License.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Library General Public License for more details.
#
# You should have received a copy of the GNU Library General Public
# License along with this library; if not, write to the Free
# Software Foundation, Inc., 51 Franklin St, Fifth Floor, Boston,
# MA 02110-1335  USA

set(NORMALIZERS_DIR "${GROONGA_PLUGINS_DIR}/normalizers")
read_file_list(${CMAKE_CURRENT_SOURCE_DIR}/mysql_sources.am MYSQL_SOURCES)
if(GROONGA_NORMALIZER_MYSQL_EMBED)
  add_library(mysql_normalizer STATIC ${MYSQL_SOURCES})
  set_property(TARGET mysql_normalizer APPEND PROPERTY
    COMPILE_DEFINITIONS "GROONGA_NORMALIZER_MYSQL_EMBED")
  set_target_properties(
    mysql_normalizer
    PROPERTIES
    POSITION_INDEPENDENT_CODE ON)
  if(NOT DEFINED CMAKE_C_COMPILE_OPTIONS_PIC)
    # For old CMake
    if(CMAKE_COMPILER_IS_GNUCXX)
      set_source_files_properties(${MYSQL_SOURCES}
	PROPERTIES
	COMPILE_FLAGS "-fPIC")
    endif()
  endif()
else()
  add_library(mysql_normalizer MODULE ${MYSQL_SOURCES})
  set_target_properties(mysql_normalizer PROPERTIES
    PREFIX ""
    OUTPUT_NAME "mysql")
  install(TARGETS mysql_normalizer DESTINATION "${NORMALIZERS_DIR}")
endif()
target_link_libraries(mysql_normalizer ${GROONGA_LIBRARIES})
