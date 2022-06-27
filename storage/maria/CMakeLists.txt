# Copyright (C) 2007 MySQL AB
# Copyright (C) 2009,2020 MariaDB Corporation Ab
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
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1335  USA

INCLUDE_DIRECTORIES(${SSL_INCLUDE_DIRS})

IF(SSL_DEFINES)
  SET_SOURCE_FILES_PROPERTIES(ma_crypt.c PROPERTIES COMPILE_FLAGS ${SSL_DEFINES})
ENDIF()

SET(ARIA_SOURCES ma_init.c ma_open.c ma_extra.c ma_info.c ma_rkey.c 
            ma_rnext.c ma_rnext_same.c 
            ma_search.c ma_page.c ma_key_recover.c ma_key.c 
            ma_locking.c ma_state.c
            ma_rrnd.c ma_scan.c ma_cache.c 
            ma_statrec.c ma_packrec.c ma_dynrec.c 
            ma_blockrec.c ma_bitmap.c 
            ma_update.c ma_write.c ma_unique.c
            ma_delete.c 
            ma_rprev.c ma_rfirst.c ma_rlast.c ma_rsame.c 
            ma_rsamepos.c ma_panic.c ma_close.c ma_create.c
            ma_range.c ma_dbug.c ma_checksum.c 
            ma_changed.c ma_static.c ma_delete_all.c 
            ma_delete_table.c ma_rename.c  ma_check.c 
            ma_keycache.c ma_preload.c ma_ft_parser.c
            ma_ft_update.c ma_ft_boolean_search.c 
            ma_ft_nlq_search.c ft_maria.c ma_sort.c 
            ha_maria.cc trnman.c lockman.c
            ma_rt_index.c ma_rt_key.c ma_rt_mbr.c ma_rt_split.c 
            ma_sp_key.c ma_control_file.c ma_loghandler.c 
            ma_pagecache.c ma_pagecaches.c
            ma_checkpoint.c ma_recovery.c ma_commit.c ma_pagecrc.c
            ha_maria.h maria_def.h ma_recovery_util.c ma_servicethread.c
            ma_norec.c
            ma_crypt.c ma_backup.c
)

IF(APPLE)
  # Workaround linker bug on OSX 10.7
  ADD_DEFINITIONS(-fno-common)
ENDIF()

IF(CMAKE_SYSTEM_NAME MATCHES AIX)
  # Workaround linker bug on AIX
  SET(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,-berok")
ENDIF()

MYSQL_ADD_PLUGIN(aria ${ARIA_SOURCES} STORAGE_ENGINE MANDATORY
                 LINK_LIBRARIES myisam mysys mysys_ssl
                 RECOMPILE_FOR_EMBEDDED)

MYSQL_ADD_EXECUTABLE(aria_ftdump aria_ftdump.c COMPONENT Server)
TARGET_LINK_LIBRARIES(aria_ftdump aria)

MYSQL_ADD_EXECUTABLE(aria_chk aria_chk.c COMPONENT Server)
TARGET_LINK_LIBRARIES(aria_chk aria)

MYSQL_ADD_EXECUTABLE(aria_read_log aria_read_log.c COMPONENT Server)
TARGET_LINK_LIBRARIES(aria_read_log aria)

MYSQL_ADD_EXECUTABLE(aria_dump_log aria_dump_log.c unittest/ma_loghandler_examples.c COMPONENT Server)
TARGET_LINK_LIBRARIES(aria_dump_log aria)
SET_TARGET_PROPERTIES(aria_dump_log PROPERTIES COMPILE_FLAGS "-DMARIA_DUMP_LOG")

MYSQL_ADD_EXECUTABLE(aria_pack aria_pack.c COMPONENT Server)
TARGET_LINK_LIBRARIES(aria_pack aria)

IF(WITH_UNIT_TESTS)
  ADD_EXECUTABLE(ma_test1 ma_test1.c)
  TARGET_LINK_LIBRARIES(ma_test1 aria)

  ADD_EXECUTABLE(ma_test2 ma_test2.c)
  TARGET_LINK_LIBRARIES(ma_test2 aria)

  ADD_EXECUTABLE(ma_test3 ma_test3.c)
  TARGET_LINK_LIBRARIES(ma_test3 aria)

  ADD_EXECUTABLE(ma_rt_test ma_rt_test.c)
  TARGET_LINK_LIBRARIES(ma_rt_test aria)

  ADD_EXECUTABLE(ma_sp_test ma_sp_test.c)
  TARGET_LINK_LIBRARIES(ma_sp_test aria)

  ADD_EXECUTABLE(test_ma_backup test_ma_backup.c)
  TARGET_LINK_LIBRARIES(test_ma_backup aria)

  ADD_SUBDIRECTORY(unittest)

ENDIF()
  
IF (MSVC)
  SET_TARGET_PROPERTIES(aria_chk aria_pack PROPERTIES LINK_FLAGS "setargv.obj")
ENDIF()

OPTION(USE_ARIA_FOR_TMP_TABLES "Use Aria for temporary tables" ON)

#
# S3
#
INCLUDE (CheckIncludeFiles)

SET(S3_SOURCES s3_func.c
    libmarias3/src/debug.c libmarias3/src/error.c libmarias3/src/marias3.c
    libmarias3/src/request.c libmarias3/src/response.c libmarias3/src/sha256.c
    libmarias3/src/sha256-internal.c libmarias3/src/xml.c
    libmarias3/src/assume_role.c)

IF(NOT PLUGIN_S3 STREQUAL NO AND NOT WIN32)
  FIND_PACKAGE(CURL)
ENDIF()

IF (CURL_FOUND)
  INCLUDE_DIRECTORIES(${CURL_INCLUDE_DIRS})
  MYSQL_ADD_PLUGIN(s3 ha_s3.cc ${S3_SOURCES} COMPONENT s3-engine
    LINK_LIBRARIES ${CURL_LIBRARIES} z STORAGE_ENGINE NOT_EMBEDDED CONFIG s3.cnf)
ENDIF()

SET(CPACK_RPM_s3-engine_PACKAGE_SUMMARY "Amazon S3 archival storage engine for MariaDB" PARENT_SCOPE)
SET(CPACK_RPM_s3-engine_PACKAGE_DESCRIPTION "The S3 storage engine allows one to archive MariaDB tables in Amazon S3 (or any third-party public or private cloud that implements S3 API), but still have them accessible in MariaDB in read-only mode." PARENT_SCOPE)

IF(TARGET s3)
  MYSQL_ADD_EXECUTABLE(aria_s3_copy aria_s3_copy.cc ${S3_SOURCES} COMPONENT s3-engine)
  TARGET_LINK_LIBRARIES(aria_s3_copy aria myisam mysys mysys_ssl curl z)
  INCLUDE_DIRECTORIES(${CMAKE_CURRENT_SOURCE_DIR}/libmarias3)
  ADD_DEFINITIONS(-DWITH_S3_STORAGE_ENGINE)
ENDIF()
