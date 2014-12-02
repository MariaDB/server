# Copyright(C) 2013 Kentoku SHIBA
# Copyright(C) 2014 Toshihisa Tashiro
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

--source ../../include/mroonga/skip_osx.inc

disable_query_log;
disable_warnings;
let $VERSION_COMPILE_64BIT=
  `SELECT IF(@@version_compile_machine like '%64%', 1, 0)`;
enable_warnings;
enable_query_log;
if ($VERSION_COMPILE_64BIT) {
  skip Need a 32 bit machine/binary;
}
