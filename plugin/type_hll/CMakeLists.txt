# Copyright (c) 2026, MariaDB corporation.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; version 2 of the License.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA

SET(HLL_CORE_SOURCES hll.cc hll_codec.cc)

MYSQL_ADD_PLUGIN(type_hll
                 plugin.cc sql_type_hll.cc ${HLL_CORE_SOURCES}
                 MODULE_ONLY RECOMPILE_FOR_EMBEDDED)

# The plugin compiles its own copy of the sources (MYSQL_DYNAMIC_PLUGIN);
# the unit test links them from a separate library.
IF(WITH_UNIT_TESTS)
  INCLUDE_DIRECTORIES(${CMAKE_CURRENT_SOURCE_DIR})
  ADD_CONVENIENCE_LIBRARY(hll_core ${HLL_CORE_SOURCES})
  MY_ADD_TESTS(hll EXT cc LINK_LIBRARIES hll_core mysys)
ENDIF()
