# Copyright (C) 2014, SkySQL Ab. All Rights Reserved.
#
# This program is free software; you can redistribute it and/or modify it under
# the terms of the GNU General Public License as published by the Free Software
# Foundation; version 2 of the License.
#
# This program is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
# FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License along with
# this program; if not, write to the Free Software Foundation, Inc.,
# 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA

MACRO (MYSQL_CHECK_LZMA)

CHECK_INCLUDE_FILES(lzma.h HAVE_LZMA_H)
CHECK_LIBRARY_EXISTS(lzma lzma_stream_buffer_decode "" HAVE_LZMA_DECODE)
CHECK_LIBRARY_EXISTS(lzma lzma_easy_buffer_encode "" HAVE_LZMA_ENCODE)

IF (HAVE_LZMA_DECODE AND HAVE_LZMA_ENCODE AND HAVE_LZMA_H)
  ADD_DEFINITIONS(-DHAVE_LZMA=1)
  LINK_LIBRARIES(lzma) 
ENDIF()
ENDMACRO()
