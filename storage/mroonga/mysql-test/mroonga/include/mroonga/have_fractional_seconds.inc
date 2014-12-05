# Copyright(C) 2012 Kouhei Sutou <kou@clear-code.com>
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
# Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

--source ../../include/mroonga/check_mariadb.inc
--source ../../include/mroonga/check_version.inc

if ($mariadb) {
  let $fractional_seconds = 1;
}

if (!$mariadb) {
  if ($version_56) {
    let $fractional_seconds = `SELECT @@global.version >= '5.6'`;
  }
}

if (!$fractional_seconds) {
  skip fractional seconds in time values are available in MySQL version 5.6 or later or MariaDB;
}
