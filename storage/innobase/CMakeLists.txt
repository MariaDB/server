
# Copyright (c) 2006, 2017, Oracle and/or its affiliates. All rights reserved.
# Copyright (c) 2014, 2021, MariaDB Corporation.
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
	os/os0thread.cc
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

set (INNOBASE_HEADERS
		#	 fts0blex.h
		#	 fts0pars.h
		#	fts0tlex.h
		#	fts0tokenize.h
		# pars0grm.h
		#	 fts0vlc.h # missing pragma once
		#	 handler0alter.h # missing pragma once

	btr0btr.h
	btr0bulk.h
	btr0cur.h
	buf0block_hint.h
#	btr0defragment.h
#	btr0pcur.h
#	btr0sea.h
#	btr0types.h
#	buf0buddy.h
	buf0buf.h
#	buf0checksum.h
#	buf0dblwr.h
#	buf0dump.h
#	buf0flu.h
#	buf0lru.h
#	buf0rea.h
#	buf0types.h
#	data0data.h
#	data0type.h
#	data0types.h
#	db0err.h
#	dict0boot.h
#	dict0crea.h
#	dict0defrag_bg.h
	dict0dict.h
#	dict0load.h
	dict0mem.h
#	dict0pagecompress.h
#	dict0stats.h
#	dict0stats_bg.h
#	dict0types.h
	dyn0buf.h
#	dyn0types.h
#	eval0eval.h
#	eval0proc.h
#	fil0crypt.h
	fil0fil.h
#	fil0pagecompress.h
#	fsp0file.h
#	fsp0fsp.h
#	fsp0space.h
#	fsp0sysspace.h
#	fsp0types.h
#	 fts0ast.h
#	 fts0fts.h
#	 fts0opt.h
#	fts0plugin.h
#	fts0priv.h
#	fts0types.h
#	fut0fut.h
#	fut0lst.h
#	gis0geo.h
#	gis0rtree.h
#	gis0type.h
#	ha_prototypes.h
#	ha0ha.h
#	ha0storage.h
#	hash0hash.h
#	ibuf0ibuf.h
#	ibuf0types.h
#	lock0iter.h
	lock0lock.h
#	lock0prdt.h
#	lock0types.h
#	log0crypt.h
#	log0log.h
#	log0recv.h
#	log0types.h
#	mach0data.h
#	 mem0mem.h
	mtr0log.h
	mtr0mtr.h
#	mtr0types.h
#	os0file.h
#	os0thread.h
#	page0cur.h
	page0page.h
#	page0types.h
#	page0zip.h
#	pars0opt.h
#	pars0pars.h
#	pars0sym.h
#	pars0types.h
#	que0que.h
#	que0types.h
#	read0types.h
#	rem0cmp.h
	rem0rec.h
#	rem0types.h
#	row0ext.h
#	row0ftsort.h
#	row0import.h
#	row0ins.h
#	row0log.h
#	row0merge.h
	row0mysql.h
#	row0purge.h
#	row0quiesce.h
#	row0row.h
#	row0sel.h
#	row0types.h
#	row0uins.h
#	row0umod.h
#	row0undo.h
#	row0upd.h
#	row0vers.h
	rw_lock.h
#	srv0mon.h
	srv0srv.h
#	srv0start.h
	srw_lock.h
	sux_lock.h
	transactional_lock_guard.h
#	trx0i_s.h
#	trx0purge.h
#	trx0rec.h
#	trx0roll.h
	trx0rseg.h
	trx0sys.h
#	trx0trx.h
#	trx0types.h
#	trx0undo.h
#	trx0xa.h
#	univ.i
#	ut0byte.h
	ut0counter.h
#	ut0dbg.h
#	ut0list.h
	ut0lst.h
#	ut0mem.h
#	ut0new.h
#	ut0pool.h
#	ut0rbt.h
#	ut0rnd.h
#	ut0sort.h
#	ut0stage.h
#	ut0ut.h
#	ut0vec.h
#	ut0wqueue.h
		)
	# data0data.ic
	# btr0btr.ic
	# btr0cur.ic
	# btr0pcur.ic
	# btr0sea.ic
	# buf0buf.ic
	# data0type.ic
	# dict0crea.ic
	# dict0dict.ic
	# dict0mem.ic
	# dict0pagecompress.ic
	# dict0stats.ic
	# eval0eval.ic
	# eval0proc.ic
	# fil0crypt.ic
	# fil0fil.ic
	# fts0priv.ic
	# fts0types.ic
	# gis0rtree.ic
	# ha0ha.ic
	# ha0storage.ic
	# ibuf0ibuf.ic
	# dict0stats.ic
	# lock0lock.ic
	# log0log.ic
	# mach0data.ic
	# mem0mem.ic
	# mtr0mtr.ic
	# os0file.ic
	# page0cur.ic
	# page0page.ic
	# page0zip.ic
	# que0que.ic
	# rem0cmp.ic
	# rem0rec.ic
	# row0ext.ic
	# row0log.ic
	# row0row.ic
	# row0sel.ic
	# row0upd.ic
	# srv0mon.ic
	# trx0rseg.ic
	# trx0trx.ic
	# trx0undo.ic
	# ut0byte.ic
	# ut0list.ic
	# ut0mem.ic
	# ut0rnd.ic
	# ut0ut.ic
	# ut0vec.ic
	# trx0rec.ic

OPTION(WITH_PMEM "Support redo log in persistent memory" OFF)
FIND_PACKAGE(PMEM)
IF(PMEM_FOUND)
  INCLUDE_DIRECTORIES(${PMEM_INCLUDES})
  ADD_COMPILE_FLAGS(log/log0log.cc COMPILE_FLAGS "-DHAVE_PMEM")
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

target_precompile_headers(innobase PRIVATE
  "$<$<COMPILE_LANGUAGE:CXX>:${INNOBASE_HEADERS}>")
set_property(SOURCE
  fts/fts0pars.cc fts/fts0blex.cc fts/fts0tlex.cc
  pars/pars0grm.cc pars/lexyy.cc sync/srw_lock.cc
  PROPERTY SKIP_PRECOMPILE_HEADERS yes)
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
