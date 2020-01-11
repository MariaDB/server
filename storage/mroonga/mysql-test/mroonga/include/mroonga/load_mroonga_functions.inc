# Copyright(C) 2013-2017  Kouhei Sutou <kou@clear-code.com>
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2.1 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA

--source ../../include/mroonga/check_ha_mroonga_so.inc

--disable_query_log
eval CREATE FUNCTION last_insert_grn_id RETURNS INTEGER SONAME $ha_mroonga_so;
eval CREATE FUNCTION mroonga_snippet RETURNS STRING SONAME $ha_mroonga_so;
eval CREATE FUNCTION mroonga_command RETURNS STRING SONAME $ha_mroonga_so;
eval CREATE FUNCTION mroonga_escape RETURNS STRING SONAME $ha_mroonga_so;
eval CREATE FUNCTION mroonga_snippet_html RETURNS STRING SONAME $ha_mroonga_so;
eval CREATE FUNCTION mroonga_normalize RETURNS STRING SONAME $ha_mroonga_so;
eval CREATE FUNCTION mroonga_highlight_html RETURNS STRING SONAME $ha_mroonga_so;
eval CREATE FUNCTION mroonga_query_expand RETURNS STRING SONAME $ha_mroonga_so;
--enable_query_log
