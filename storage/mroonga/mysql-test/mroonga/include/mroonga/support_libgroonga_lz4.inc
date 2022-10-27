# Copyright(C) 2014 Naoya Murakami <naoya@createfield.com>
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

--source ../../include/mroonga/check_libgroonga_support_lz4.inc

if (!$libgroonga_support_lz4) {
  --source ../../include/mroonga/have_mroonga_deinit.inc
  --skip This test is for libgroonga supports lz4
}
