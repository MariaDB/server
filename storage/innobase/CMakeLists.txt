# Copyright (c) 2006, 2017, Oracle and/or its affiliates. All rights reserved.
# Copyright (c) 2014, 2019, MariaDB Corporation.
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

SET(INNOBASE_SOURCES
	btr/btr0btr.cc
	btr/btr0bulk.cc
	btr/btr0cur.cc
	btr/btr0pcur.cc
	btr/btr0scrub.cc
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
	buf/buf0mtflu.cc
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
	ha/ha0ha.cc
	ha/ha0storage.cc
	ha/hash0hash.cc
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
	lock/lock0wait.cc
	log/log0log.cc
	log/log0recv.cc
	log/log0crypt.cc
	mach/mach0data.cc
	mem/mem0mem.cc
	mtr/mtr0log.cc
	mtr/mtr0mtr.cc
	os/os0file.cc
	os/os0proc.cc
	os/os0event.cc
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
	row/row0trunc.cc
	row/row0uins.cc
	row/row0umod.cc
	row/row0undo.cc
	row/row0upd.cc
	row/row0quiesce.cc
	row/row0vers.cc
	srv/srv0conc.cc
	srv/srv0mon.cc
	srv/srv0srv.cc
	srv/srv0start.cc
	sync/sync0arr.cc
	sync/sync0rw.cc
	sync/sync0debug.cc
	sync/sync0sync.cc
	trx/trx0i_s.cc
	trx/trx0purge.cc
	trx/trx0rec.cc
	trx/trx0roll.cc
	trx/trx0rseg.cc
	trx/trx0sys.cc
	trx/trx0trx.cc
	trx/trx0undo.cc
	ut/ut0crc32.cc
	ut/ut0dbg.cc
	ut/ut0list.cc
	ut/ut0mem.cc
	ut/ut0new.cc
	ut/ut0rbt.cc
	ut/ut0rnd.cc
	ut/ut0ut.cc
	ut/ut0vec.cc
	ut/ut0wqueue.cc)

MYSQL_ADD_PLUGIN(innobase ${INNOBASE_SOURCES} STORAGE_ENGINE
  MODULE_OUTPUT_NAME ha_innodb
  DEFAULT RECOMPILE_FOR_EMBEDDED
  LINK_LIBRARIES
	${ZLIB_LIBRARY}
	${CRC32_VPMSUM_LIBRARY}
	${NUMA_LIBRARY}
	${LIBSYSTEMD}
	${LINKER_SCRIPT})

IF(NOT TARGET innobase)
  RETURN()
ENDIF()

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
  # silence "switch statement contains 'default' but no 'case' label
  # on generated file.
  TARGET_COMPILE_OPTIONS(innobase PRIVATE "/wd4065")
ENDIF()

ADD_SUBDIRECTORY(${CMAKE_SOURCE_DIR}/extra/mariabackup ${CMAKE_BINARY_DIR}/extra/mariabackup)

IF(WITH_UNIT_TESTS)
  ADD_SUBDIRECTORY(unittest)
ENDIF()
