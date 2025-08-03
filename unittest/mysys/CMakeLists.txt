# Copyright (c) 2006, 2013, Oracle and/or its affiliates. All rights reserved.
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
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1335 USA

MY_ADD_TESTS(
  base64
  bitmap
  byte_order
  crc32
  dynamic_array
  dynstring
  lf
  my_atomic
  my_getopt
  my_malloc
  my_rdtsc
  my_tzinfo
  queues
  stack_allocation
  stacktrace
  waiting_threads
  LINK_LIBRARIES mysys)
MY_ADD_TESTS(my_vsnprintf LINK_LIBRARIES strings mysys)
ADD_DEFINITIONS(${SSL_DEFINES})
MY_ADD_TESTS(aes LINK_LIBRARIES  mysys mysys_ssl)
INCLUDE_DIRECTORIES(${ZLIB_INCLUDE_DIRS})
MY_ADD_TESTS(ma_dyncol LINK_LIBRARIES  mysys)

IF(WIN32)
  MY_ADD_TESTS(my_delete LINK_LIBRARIES mysys)
ENDIF()
