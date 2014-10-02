# Copyright(C) 2012-2013 Kouhei Sutou <kou@clear-code.com>
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

--disable_query_log
let $version_major_minor =
  `SELECT CAST(SUBSTRING_INDEX(@@global.version, '.', 2) AS DECIMAL(4, 2))`;

let $version_55            = `SELECT $version_major_minor = 5.5`;
let $version_56            = `SELECT $version_major_minor = 5.6`;
let $version_100           = `SELECT $version_major_minor = 10.0`;

let $version_55_or_later   = `SELECT $version_major_minor >= 5.5`;
let $version_56_or_later   = `SELECT $version_major_minor >= 5.6`;
let $version_100_or_later  = `SELECT $version_major_minor >= 10.0`;
--enable_query_log
