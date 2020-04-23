# Copyright (C) 2007 MySQL AB
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

MYSQL_ADD_PLUGIN(aria ${ARIA_SOURCES} STORAGE_ENGINE MANDATORY
                 LINK_LIBRARIES myisam mysys mysys_ssl
                 RECOMPILE_FOR_EMBEDDED)

MYSQL_ADD_EXECUTABLE(aria_ftdump maria_ftdump.c COMPONENT Server)
TARGET_LINK_LIBRARIES(aria_ftdump aria)

MYSQL_ADD_EXECUTABLE(aria_chk maria_chk.c COMPONENT Server)
TARGET_LINK_LIBRARIES(aria_chk aria)

MYSQL_ADD_EXECUTABLE(aria_read_log maria_read_log.c COMPONENT Server)
TARGET_LINK_LIBRARIES(aria_read_log aria)

MYSQL_ADD_EXECUTABLE(aria_dump_log  maria_dump_log.c unittest/ma_loghandler_examples.c COMPONENT Server)
TARGET_LINK_LIBRARIES(aria_dump_log aria)
SET_TARGET_PROPERTIES(aria_dump_log PROPERTIES COMPILE_FLAGS "-DMARIA_DUMP_LOG")

MYSQL_ADD_EXECUTABLE(aria_pack maria_pack.c COMPONENT Server)
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
INCLUDE (CheckCSourceRuns)

SET(CURL_SSL_TEST_SOURCE "\n
   #include <curl/curl.h>\n
   #include <stdlib.h>\n
   #include <string.h>\n
   int main(void)\n
   {\n
     curl_version_info_data *data = curl_version_info(CURLVERSION_NOW);\n
\n
     if (data->ssl_version)\n
     {\n
       if (strncmp(data->ssl_version, \"OpenSSL\", 7) != 0)\n
       {\n
         return EXIT_SUCCESS;\n
       }\n
       if (data->ssl_version[8] == '0')\n
       {\n
         return EXIT_FAILURE;\n
       }\n
       if ((data->ssl_version[8] == '1') && (data->ssl_version[10] == '0'))\n
       {\n
         return EXIT_FAILURE;\n
       }\n
     }\n
     return EXIT_SUCCESS;\n
   }")

SET(S3_SOURCES ha_s3.cc s3_func.c
    libmarias3/src/debug.c libmarias3/src/error.c libmarias3/src/marias3.c
    libmarias3/src/request.c libmarias3/src/response.c libmarias3/src/sha256.c
    libmarias3/src/sha256-internal.c libmarias3/src/xml.c)

IF(NOT PLUGIN_S3 STREQUAL NO)
  FIND_PACKAGE(CURL)
ENDIF()

IF (CURL_FOUND)
  MYSQL_ADD_PLUGIN(s3 ${S3_SOURCES} STORAGE_ENGINE STATIC_ONLY
                   LINK_LIBRARIES aria myisam mysys mysys_ssl curl
                   NOT_EMBEDDED)
ENDIF()

IF(TARGET s3)
  SET(CMAKE_REQUIRED_LIBRARIES curl)
  CHECK_C_SOURCE_RUNS("${CURL_SSL_TEST_SOURCE}" FOUND_SAFE_CURL_SSL)
  UNSET(CMAKE_REQUIRED_LIBRARIES)
  MYSQL_ADD_EXECUTABLE(aria_s3_copy aria_s3_copy.cc COMPONENT Server)
  TARGET_LINK_LIBRARIES(aria_s3_copy s3)

  INCLUDE_DIRECTORIES(${CMAKE_CURRENT_SOURCE_DIR}/libmarias3)

  ADD_DEFINITIONS(-DWITH_S3_STORAGE_ENGINE)
  IF (FOUND_SAFE_CURL_SSL)
      MESSAGE(STATUS "Found thread safe curl ssl")
  ELSE ()
      MESSAGE(STATUS "Found thread unsafe curl ssl, locks added")
      ADD_DEFINITIONS(-DHAVE_CURL_OPENSSL_UNSAFE)
  ENDIF ()

  TARGET_LINK_LIBRARIES(aria s3)
ENDIF()
