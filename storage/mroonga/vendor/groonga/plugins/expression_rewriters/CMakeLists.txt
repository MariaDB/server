# Copyright(C) 2015 Brazil
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

if(NOT GRN_EMBED)
  if(GRN_WITH_MRUBY)
    set(GRN_RELATIVE_EXPRESSION_REWRITER_PLUGINS_DIR
      "${GRN_RELATIVE_PLUGINS_DIR}/expression_rewriters")

    read_file_list(${CMAKE_CURRENT_SOURCE_DIR}/sources.am
      EXPRESSION_REWRITERS)
    install(FILES ${EXPRESSION_REWRITERS}
      DESTINATION "${GRN_RELATIVE_SEXPRESSION_REWRITER_PLUGINS_DIR}")
  endif()
endif()
