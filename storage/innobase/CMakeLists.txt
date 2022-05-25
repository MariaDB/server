
# Copyright (c) 2006, 2017, Oracle and/or its affiliates. All rights reserved.
# Copyright (c) 2014, 2022, MariaDB Corporation.
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

# This is the CMakeLists for InnoDB



INCLUDE(innodb.cmake)
INCLUDE_DIRECTORIES(${PROJECT_SOURCE_DIR}/tpool)

SET(INNOBASE_SOURCES
	btr/btr0btr.cc
	btr/btr0bulk.cc
	btr/btr0cur.cc
	btr/btr0pcur.cc
	btr/btr0sea.cc
	btr/btr0defragment.cc
	buf/buf0block_hint.cc
	buf/buf0buddy.cc
	buf/buf0buf.cc
	buf/buf0dblwr.cc
	buf/buf0checksum.cc
	buf/buf0dump.cc
	buf/buf0flu.cc
	buf/buf0lru.cc
	buf/buf0rea.cc
	data/data0data.cc
	data/data0type.cc
	dict/dict0boot.cc
	dict/dict0crea.cc
	dict/dict0dict.cc
	dict/dict0load.cc
	dict/dict0mem.cc
	dict/dict0stats.cc
	dict/dict0stats_bg.cc
	dict/dict0defrag_bg.cc
	dict/drop.cc
	eval/eval0eval.cc
	eval/eval0proc.cc
	fil/fil0fil.cc
	fil/fil0pagecompress.cc
	fil/fil0crypt.cc
	fsp/fsp0fsp.cc
	fsp/fsp0file.cc
	fsp/fsp0space.cc
	fsp/fsp0sysspace.cc
	fut/fut0lst.cc
	ha/ha0storage.cc
	fts/fts0fts.cc
	fts/fts0ast.cc
	fts/fts0blex.cc
	fts/fts0config.cc
	fts/fts0opt.cc
	fts/fts0pars.cc
	fts/fts0que.cc
	fts/fts0sql.cc
	fts/fts0tlex.cc
	gis/gis0geo.cc
	gis/gis0rtree.cc
	gis/gis0sea.cc
	fts/fts0plugin.cc
	handler/ha_innodb.cc
	handler/handler0alter.cc
	handler/i_s.cc
	ibuf/ibuf0ibuf.cc
	include/btr0btr.h
	include/btr0btr.inl
	include/btr0bulk.h
	include/btr0cur.h
	include/btr0cur.inl
	include/btr0defragment.h
	include/btr0pcur.h
	include/btr0pcur.inl
	include/btr0sea.h
	include/btr0sea.inl
	include/btr0types.h
	include/buf0buddy.h
	include/buf0buf.h
	include/buf0buf.inl
	include/buf0checksum.h
	include/buf0dblwr.h
	include/buf0dump.h
	include/buf0flu.h
	include/buf0lru.h
	include/buf0rea.h
	include/buf0types.h
	include/data0data.h
	include/data0data.inl
	include/data0type.h
	include/data0type.inl
	include/data0types.h
	include/db0err.h
	include/dict0boot.h
	include/dict0crea.h
	include/dict0crea.inl
	include/dict0defrag_bg.h
	include/dict0dict.h
	include/dict0dict.inl
	include/dict0load.h
	include/dict0mem.h
	include/dict0mem.inl
	include/dict0pagecompress.h
	include/dict0pagecompress.inl
	include/dict0stats.h
	include/dict0stats.inl
	include/dict0stats_bg.h
	include/dict0types.h
	include/dyn0buf.h
	include/dyn0types.h
	include/eval0eval.h
	include/eval0eval.inl
	include/eval0proc.h
	include/eval0proc.inl
	include/fil0crypt.h
	include/fil0crypt.inl
	include/fil0fil.h
	include/fil0fil.inl
	include/fil0pagecompress.h
	include/fsp0file.h
	include/fsp0fsp.h
	include/fsp0space.h
	include/fsp0sysspace.h
	include/fsp0types.h
	include/fts0ast.h
	include/fts0blex.h
	include/fts0fts.h
	include/fts0opt.h
	include/fts0pars.h
	include/fts0plugin.h
	include/fts0priv.h
	include/fts0priv.inl
	include/fts0tlex.h
	include/fts0tokenize.h
	include/fts0types.h
	include/fts0types.inl
	include/fts0vlc.h
	include/fut0fut.h
	include/fut0lst.h
	include/gis0geo.h
	include/gis0rtree.h
	include/gis0rtree.inl
	include/gis0type.h
	include/ha_prototypes.h
	include/ha0ha.h
	include/ha0ha.inl
	include/ha0storage.h
	include/ha0storage.inl
	include/handler0alter.h
	include/hash0hash.h
	include/ibuf0ibuf.h
	include/ibuf0ibuf.inl/
	include/ibuf0types.h
	include/lock0iter.h
	include/lock0lock.h
	include/lock0lock.inl
	include/lock0prdt.h
	include/lock0priv.h
	include/lock0priv.inl
	include/lock0types.h
	include/log0crypt.h
	include/log0log.h
	include/log0recv.h
	include/log0types.h
	include/mach0data.h
	include/mach0data.inl
	include/mem0mem.h
	include/mem0mem.inl
	include/mtr0log.h
	include/mtr0mtr.h
	include/mtr0mtr.inl
	include/mtr0types.h
	include/os0file.h
	include/os0file.inl
	include/page0cur.h
	include/page0cur.inl
	include/page0page.h
	include/page0page.inl
	include/page0types.h
	include/page0zip.h
	include/page0zip.inl
	include/pars0grm.h
	include/pars0opt.h
	include/pars0pars.h
	include/pars0sym.h
	include/pars0types.h
	include/que0que.h
	include/que0que.inl
	include/que0types.h
	include/read0types.h
	include/rem0cmp.h
	include/rem0rec.h
	include/rem0rec.inl
	include/rem0types.h
	include/row0ext.h
	include/row0ext.inl
	include/row0ftsort.h
	include/row0import.h
	include/row0ins.h
	include/row0log.h
	include/row0merge.h
	include/row0mysql.h
	include/row0purge.h
	include/row0quiesce.h
	include/row0row.h
	include/row0row.inl
	include/row0sel.h
	include/row0sel.inl
	include/row0types.h
	include/row0uins.h
	include/row0umod.h
	include/row0undo.h
	include/row0upd.h
	include/row0upd.inl
	include/row0vers.h
	include/rw_lock.h
	include/srv0mon.h
	include/srv0mon.inl
	include/srv0srv.h
	include/srv0start.h
	include/srw_lock.h
	include/sux_lock.h
	include/trx0i_s.h
	include/trx0purge.h
	include/trx0rec.h
	include/trx0rec.inl
	include/trx0roll.h
	include/trx0rseg.h
	include/trx0rseg.inl
	include/trx0sys.h
	include/trx0trx.h
	include/trx0trx.inl
	include/trx0types.h
	include/trx0undo.h
	include/trx0undo.inl
	include/trx0xa.h
	include/univ.i
	include/ut0byte.h
	include/ut0byte.inl
	include/ut0counter.h
	include/ut0dbg.h
	include/ut0list.h
	include/ut0list.inl
	include/ut0lst.h
	include/ut0mem.h
	include/ut0mem.inl
	include/ut0new.h
	include/ut0pool.h
	include/ut0rbt.h
	include/ut0rnd.h
	include/ut0rnd.inl
	include/ut0sort.h
	include/ut0stage.h
	include/ut0ut.h
	include/ut0ut.inl
	include/ut0vec.h
	include/ut0vec.inl
	include/ut0wqueue.h
	lock/lock0iter.cc
	lock/lock0prdt.cc
	lock/lock0lock.cc
	log/log0log.cc
	log/log0recv.cc
	log/log0crypt.cc
	log/log0sync.cc
	mem/mem0mem.cc
	mtr/mtr0mtr.cc
	os/os0file.cc
	page/page0cur.cc
	page/page0page.cc
	page/page0zip.cc
	pars/lexyy.cc
	pars/pars0grm.cc
	pars/pars0opt.cc
	pars/pars0pars.cc
	pars/pars0sym.cc
	que/que0que.cc
	read/read0read.cc
	rem/rem0cmp.cc
	rem/rem0rec.cc
	row/row0ext.cc
	row/row0ftsort.cc
	row/row0import.cc
	row/row0ins.cc
	row/row0merge.cc
	row/row0mysql.cc
	row/row0log.cc
	row/row0purge.cc
	row/row0row.cc
	row/row0sel.cc
	row/row0uins.cc
	row/row0umod.cc
	row/row0undo.cc
	row/row0upd.cc
	row/row0quiesce.cc
	row/row0vers.cc
	srv/srv0mon.cc
	srv/srv0srv.cc
	srv/srv0start.cc
	sync/srw_lock.cc
	trx/trx0i_s.cc
	trx/trx0purge.cc
	trx/trx0rec.cc
	trx/trx0roll.cc
	trx/trx0rseg.cc
	trx/trx0sys.cc
	trx/trx0trx.cc
	trx/trx0undo.cc
	ut/ut0dbg.cc
	ut/ut0list.cc
	ut/ut0mem.cc
	ut/ut0new.cc
	ut/ut0rbt.cc
	ut/ut0rnd.cc
	ut/ut0ut.cc
	ut/ut0vec.cc
	ut/ut0wqueue.cc)

OPTION(WITH_PMEM "Support redo log in persistent memory" OFF)
FIND_PACKAGE(PMEM)
IF(PMEM_FOUND)
  INCLUDE_DIRECTORIES(${PMEM_INCLUDES})
  ADD_COMPILE_FLAGS(log/log0log.cc log/log0recv.cc
    buf/buf0flu.cc mtr/mtr0mtr.cc trx/trx0trx.cc srv/srv0start.cc
    COMPILE_FLAGS "-DHAVE_PMEM")
  SET(PMEM_LIBRARY ${PMEM_LIBRARIES})
ELSE()
  IF(WITH_PMEM)
    MESSAGE(FATAL_ERROR "WITH_PMEM=ON cannot be satisfied")
  ENDIF()
ENDIF()

MYSQL_ADD_PLUGIN(innobase ${INNOBASE_SOURCES} STORAGE_ENGINE
  MODULE_OUTPUT_NAME ha_innodb
  DEFAULT RECOMPILE_FOR_EMBEDDED
  LINK_LIBRARIES
	${ZLIB_LIBRARY}
	${PMEM_LIBRARY}
	${NUMA_LIBRARY}
	${LIBSYSTEMD}
	${LINKER_SCRIPT})

IF(NOT TARGET innobase)
  RETURN()
ENDIF()

ADD_DEFINITIONS(${SSL_DEFINES} ${TPOOL_DEFINES})

# A GCC bug causes crash when compiling these files on ARM64 with -O1+
# Compile them with -O0 as a workaround.
IF(CMAKE_COMPILER_IS_GNUCXX AND CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64"
   AND CMAKE_C_COMPILER_VERSION VERSION_LESS "5.2.0")
  ADD_COMPILE_FLAGS(
      btr/btr0btr.cc
      btr/btr0cur.cc
      buf/buf0buf.cc
      fts/fts0fts.cc
      gis/gis0sea.cc
      handler/handler0alter.cc
      mtr/mtr0mtr.cc
      row/row0merge.cc
      row/row0mysql.cc
      srv/srv0srv.cc
      COMPILE_FLAGS "-O0"
      )
ENDIF()

# Older gcc version insist on -mhtm flag for including the
# htmxlintrin.h header. This is also true for new gcc versions
# like 11.2.0 in Debian Sid
# s390x because of the way it defines the high level intrinsics
# as not-inline in the header file can only be included by one
# source file that has -mhtm enabled.
IF(CMAKE_SYSTEM_PROCESSOR MATCHES "ppc64|powerpc64|s390x"
   OR CMAKE_SYSTEM_NAME MATCHES "AIX")
  ADD_COMPILE_FLAGS(
      sync/srw_lock.cc
      COMPILE_FLAGS "-mhtm"
      )
ENDIF()
IF(MSVC)
  IF(CMAKE_SIZEOF_VOID_P EQUAL 8)
   ADD_COMPILE_FLAGS(
      pars/lexyy.cc
      COMPILE_FLAGS "/wd4267")
  ENDIF()
  # silence "switch statement contains 'default' but no 'case' label
  # on generated file.
  TARGET_COMPILE_OPTIONS(innobase PRIVATE "/wd4065")
ENDIF()

IF(NOT (PLUGIN_INNOBASE STREQUAL DYNAMIC))
  TARGET_LINK_LIBRARIES(innobase tpool mysys)
  ADD_SUBDIRECTORY(${CMAKE_SOURCE_DIR}/extra/mariabackup ${CMAKE_BINARY_DIR}/extra/mariabackup)
ENDIF()

IF(WITH_UNIT_TESTS)
  ADD_SUBDIRECTORY(unittest)
ENDIF()
