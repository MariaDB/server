# Copyright (c) 2006, 2014, Oracle and/or its affiliates
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

INCLUDE_DIRECTORIES(
${CMAKE_SOURCE_DIR}/include
${CMAKE_BINARY_DIR}/strings
)

SET(STRINGS_SOURCES bchange.c bmove_upp.c ctype-big5.c ctype-bin.c ctype-cp932.c
                ctype-czech.c ctype-euc_kr.c ctype-eucjpms.c ctype-extra.c ctype-gb2312.c ctype-gbk.c
                ctype-latin1.c ctype-mb.c ctype-simple.c ctype-sjis.c ctype-tis620.c
                ctype-uca.c ctype-uca0900.c ctype-uca1400.c
                ctype-ucs2.c ctype-ujis.c ctype-utf8.c ctype-win1250ch.c ctype.c decimal.c dtoa.c int2str.c
                ctype-unidata.c
                is_prefix.c llstr.c longlong2str.c my_strtoll10.c my_vsnprintf.c
                str2int.c strcend.c strend.c strfill.c strmake.c strmov.c strnmov.c
                strxmov.c strxnmov.c xml.c
                strmov_overlapp.c
		my_strchr.c strcont.c strappend.c json_lib.c json_normalize.c)

IF(NOT HAVE_STRNLEN)
  # OSX below 10.7 did not have strnlen
  SET(STRINGS_SOURCES ${STRINGS_SOURCES} strnlen.c)
ENDIF()
# Avoid dependencies on perschema data defined in mysys
ADD_DEFINITIONS(-DDISABLE_MYSQL_THREAD_H)
ADD_CONVENIENCE_LIBRARY(strings ${STRINGS_SOURCES})
TARGET_LINK_LIBRARIES(strings dbug mysys)
MAYBE_DISABLE_IPO(strings)
ADD_EXECUTABLE(conf_to_src EXCLUDE_FROM_ALL conf_to_src.c)
SET_TARGET_PROPERTIES(conf_to_src PROPERTIES EXCLUDE_FROM_DEFAULT_BUILD TRUE)
TARGET_LINK_LIBRARIES(conf_to_src mysys strings)

IF(NOT CMAKE_CROSSCOMPILING OR DEFINED CMAKE_CROSSCOMPILING_EMULATOR)
  ADD_EXECUTABLE(uca-dump uca-dump.c)
ENDIF()

ADD_CUSTOM_COMMAND(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/ctype-uca1400data.h
  COMMAND uca-dump --name-prefix=uca1400 --levels=3 ${PROJECT_SOURCE_DIR}/mysql-test/std_data/unicode/allkeys1400.txt > ctype-uca1400data.h
  DEPENDS uca-dump
)

ADD_CUSTOM_TARGET(
        GenUnicodeDataSource
        DEPENDS
        ${CMAKE_CURRENT_BINARY_DIR}/ctype-uca1400data.h
)
ADD_DEPENDENCIES(strings GenUnicodeDataSource)
