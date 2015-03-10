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

add_definitions(
  -DGRN_DAT_EXPORT
  )

include_directories(
  ${CMAKE_CURRENT_SOURCE_DIR}/dat
  ${MRUBY_INCLUDE_DIRS}
  ${LIBLZ4_INCLUDE_DIRS})
link_directories(
  ${LIBLZ4_LIBRARY_DIRS})

read_file_list(${CMAKE_CURRENT_SOURCE_DIR}/sources.am LIBGROONGA_SOURCES)
read_file_list(${CMAKE_CURRENT_SOURCE_DIR}/dat/sources.am LIBGRNDAT_SOURCES)
string(REGEX REPLACE "([^;]+)" "dat/\\1"
  LIBGRNDAT_SOURCES "${LIBGRNDAT_SOURCES}")
read_file_list(${CMAKE_CURRENT_SOURCE_DIR}/mrb/sources.am LIBGRNMRB_SOURCES)
string(REGEX REPLACE "([^;]+)" "mrb/\\1"
  LIBGRNMRB_SOURCES "${LIBGRNMRB_SOURCES}")

set_source_files_properties(${LIBGROONGA_SOURCES} ${LIBGRNMRB_SOURCES}
  PROPERTIES
  COMPILE_FLAGS "${GRN_C_COMPILE_FLAGS}")
set_source_files_properties(dat.cpp ${LIBGRNDAT_SOURCES}
  PROPERTIES
  COMPILE_FLAGS "${GRN_CXX_COMPILE_FLAGS}")

set(GRN_ALL_SOURCES
  ${LIBGROONGA_SOURCES}
  ${LIBGRNDAT_SOURCES}
  ${LIBGRNMRB_SOURCES}
  ${MRUBY_LIBS})
if(GRN_EMBED)
  add_library(libgroonga STATIC ${GRN_ALL_SOURCES})
else()
  add_library(libgroonga SHARED ${GRN_ALL_SOURCES})
endif()
set_target_properties(libgroonga PROPERTIES OUTPUT_NAME "groonga")

set(GRN_ALL_LIBRARIES
    ${EXECINFO_LIBS}
    ${RT_LIBS}
    ${PTHREAD_LIBS}
    ${Z_LIBS}
    ${LIBLZ4_LIBRARIES}
    ${DL_LIBS}
    ${M_LIBS}
    ${WS2_32_LIBS})
if(GRN_EMBED)
  target_link_libraries(libgroonga
    ${GRN_ALL_LIBRARIES}
    ${STDCPP_LIBS})
else()
  target_link_libraries(libgroonga
    ${GRN_ALL_LIBRARIES})
  install(TARGETS libgroonga
    ARCHIVE DESTINATION "${LIB_DIR}"
    LIBRARY DESTINATION "${LIB_DIR}"
    RUNTIME DESTINATION "${BIN_DIR}")
endif()

if(GRN_WITH_MRUBY AND NOT GRN_EMBED)
  read_file_list(${CMAKE_CURRENT_SOURCE_DIR}/mrb/scripts/sources.am
    RUBY_SCRIPTS)
  string(REGEX REPLACE "([^;]+)" "mrb/scripts/\\1"
    RUBY_SCRIPTS "${RUBY_SCRIPTS}")
  install(
    FILES ${RUBY_SCRIPTS}
    DESTINATION "${GRN_RELATIVE_RUBY_SCRIPTS_DIR}")
endif()
