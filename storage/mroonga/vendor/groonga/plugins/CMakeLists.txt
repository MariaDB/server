# Copyright(C) 2012-2015 Brazil
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

add_subdirectory(suggest)
add_subdirectory(tokenizers)
add_subdirectory(table)
add_subdirectory(query_expanders)
add_subdirectory(ruby)
add_subdirectory(token_filters)
add_subdirectory(sharding)

if(GRN_WITH_MRUBY)
  read_file_list(${CMAKE_CURRENT_SOURCE_DIR}/ruby_scripts.am RUBY_SCRIPTS)
  install(FILES ${RUBY_SCRIPTS}
    DESTINATION "${GRN_RELATIVE_PLUGINS_DIR}")
endif()
