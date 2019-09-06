# Copyright(C) 2010-2014 Kentoku SHIBA
# Copyright(C) 2011 Kouhei Sutou <kou@clear-code.com>
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

disable_query_log;

if ($have_default_storage_engine_variable) {
  eval set default_storage_engine=$original_default_storage_engine;
}
if (!$have_default_storage_engine_variable) {
  eval set storage_engine=$original_storage_engine;
}

if ($have_default_tmp_storage_engine_variable) {
  eval set default_tmp_storage_engine=$original_default_tmp_storage_engine;
}

if (!$have_mroonga_storage_engine) {
  UNINSTALL PLUGIN mroonga_stats;
  UNINSTALL PLUGIN mroonga;
}

# Some test re-creates the test database. Put it back in the
# original character set
--source include/default_charset.inc
disable_query_log;
drop database test;
create database test;
enable_query_log;
