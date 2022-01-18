# Copyright (c) 2006, 2014, Oracle and/or its affiliates
# Copyright (c) 2009, 2018, MariaDB Corporation
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

INCLUDE_DIRECTORIES(${ZLIB_INCLUDE_DIR} ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/mysys)

SET(MYSYS_SOURCES  array.c charset-def.c charset.c my_default.c
                get_password.c
				errors.c hash.c list.c
                                mf_cache.c mf_dirname.c mf_fn_ext.c
				mf_format.c mf_getdate.c mf_iocache.c mf_iocache2.c mf_keycache.c 
				mf_keycaches.c mf_loadpath.c mf_pack.c mf_path.c mf_qsort.c mf_qsort2.c
				mf_radix.c mf_same.c mf_sort.c mf_soundex.c mf_arr_appstr.c mf_tempdir.c
				mf_tempfile.c mf_unixpath.c mf_wcomp.c mulalloc.c my_access.c
				my_alloc.c my_bit.c my_bitmap.c my_chsize.c
				my_compress.c my_copy.c  my_create.c my_delete.c
				my_div.c my_error.c my_file.c my_fopen.c my_fstream.c 
				my_gethwaddr.c my_getopt.c my_getsystime.c my_getwd.c my_compare.c my_init.c
				my_lib.c my_lock.c my_malloc.c my_mess.c
				my_mkdir.c my_mmap.c my_once.c my_open.c my_pread.c my_pthread.c
				my_quick.c my_read.c my_redel.c my_rename.c my_seek.c my_sleep.c
				my_static.c my_symlink.c my_symlink2.c my_sync.c my_thr_init.c 
				my_basename.c
				my_write.c ptr_cmp.c queues.c stacktrace.c
				string.c thr_alarm.c thr_lock.c thr_mutex.c
				thr_rwlock.c thr_timer.c
                                tree.c typelib.c base64.c my_memmem.c
                                my_getpagesize.c
                                guess_malloc_library.c
				lf_alloc-pin.c lf_dynarray.c lf_hash.cc
                                safemalloc.c my_new.cc
				my_getncpus.c my_safehash.c my_chmod.c my_rnd.c
                                my_uuid.c wqueue.c waiting_threads.c ma_dyncol.c ../sql-common/my_time.c
				my_rdtsc.c psi_noop.c
                                my_atomic_writes.c my_cpu.c my_likely.c my_largepage.c
                                file_logger.c my_dlerror.c   crc32/crc32c.cc)

IF (WIN32)
  SET (MYSYS_SOURCES ${MYSYS_SOURCES}
    my_winthread.c
    my_wintoken.c
    my_wincond.c
    my_winerr.c
    my_winfile.c
    my_minidump.cc
    my_win_popen.cc)
ENDIF()

IF(MSVC_INTEL)
  SET(MYSYS_SOURCES ${MYSYS_SOURCES} crc32/crc32_x86.c)
  IF(CMAKE_SIZEOF_VOID_P EQUAL 8)
    SET (MYSYS_SOURCES ${MYSYS_SOURCES} crc32/crc32c_amd64.cc)
  ENDIF()
  ADD_DEFINITIONS(-DHAVE_SSE42 -DHAVE_PCLMUL)
  IF(CLANG_CL)
    SET_SOURCE_FILES_PROPERTIES(crc32/crc32_x86.c PROPERTIES COMPILE_FLAGS "-msse4.2 -mpclmul")
  ENDIF()
ELSEIF(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|i386|i686")
  MY_CHECK_CXX_COMPILER_FLAG(-msse4.2)
  MY_CHECK_CXX_COMPILER_FLAG(-mpclmul)
  CHECK_INCLUDE_FILE(cpuid.h HAVE_CPUID_H)
  CHECK_INCLUDE_FILE(x86intrin.h HAVE_X86INTRIN_H)
  IF(have_CXX__msse4.2 AND HAVE_CPUID_H)
    ADD_DEFINITIONS(-DHAVE_SSE42)
    IF (have_CXX__mpclmul AND HAVE_X86INTRIN_H)
      ADD_DEFINITIONS(-DHAVE_PCLMUL)
      SET(MYSYS_SOURCES ${MYSYS_SOURCES} crc32/crc32_x86.c)
      SET_SOURCE_FILES_PROPERTIES(crc32/crc32_x86.c PROPERTIES COMPILE_FLAGS "-msse4.2 -mpclmul")
      IF(CMAKE_SIZEOF_VOID_P EQUAL 8)
	SET(MYSYS_SOURCES ${MYSYS_SOURCES} crc32/crc32c_amd64.cc)
	SET_SOURCE_FILES_PROPERTIES(crc32/crc32c_amd64.cc PROPERTIES COMPILE_FLAGS "-msse4.2 -mpclmul")
      ENDIF()
    ENDIF()
  ENDIF()
ELSEIF(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|AARCH64")
  IF(CMAKE_COMPILER_IS_GNUCC)
    include(CheckCXXSourceCompiles)

    CHECK_CXX_SOURCE_COMPILES("
    #define CRC32CX(crc, value) __asm__(\"crc32cx %w[c], %w[c], %x[v]\":[c]\"+r\"(crc):[v]\"r\"(value))
    asm(\".arch_extension crc\");
    unsigned int foo(unsigned int ret) {
      CRC32CX(ret, 0);
      return ret;
    }
    #include <sys/auxv.h>
    int main() { foo(0);
    #ifdef __linux__
    getauxval(AT_HWCAP);
    #else
    unsigned long v;
    elf_aux_info(AT_HWCAP, &v, sizeof(v));
    #endif
    }" HAVE_ARMV8_CRC)

    CHECK_CXX_SOURCE_COMPILES("
    asm(\".arch_extension crypto\");
    unsigned int foo(unsigned int ret) {
      __asm__(\"pmull  v2.1q,          v2.1d,  v1.1d\");
      return ret;
    }
    #include <sys/auxv.h>
    int main() { foo(0);
    #ifdef __linux__
    getauxval(AT_HWCAP);
    #else
    unsigned long v;
    elf_aux_info(AT_HWCAP, &v, sizeof(v));
    #endif
    }" HAVE_ARMV8_CRYPTO)

    CHECK_C_COMPILER_FLAG(-march=armv8-a+crc+crypto HAVE_ARMV8_CRC_CRYPTO_MARCH)

    IF(HAVE_ARMV8_CRC_CRYPTO_MARCH)
      CHECK_INCLUDE_FILE(arm_acle.h HAVE_ARM_ACLE_H -march=armv8-a+crc+crypto)
      IF(HAVE_ARM_ACLE_H)
       ADD_DEFINITIONS(-DHAVE_ARMV8_CRC_CRYPTO_INTRINSICS)
      ENDIF()
      IF(HAVE_ARMV8_CRC)
        ADD_DEFINITIONS(-DHAVE_ARMV8_CRC)
      ENDIF()
      IF(HAVE_ARMV8_CRYPTO)
        ADD_DEFINITIONS(-DHAVE_ARMV8_CRYPTO)
      ENDIF()
      SET(MYSYS_SOURCES ${MYSYS_SOURCES} crc32/crc32_arm64.c)
      SET_SOURCE_FILES_PROPERTIES(crc32/crc32_arm64.c PROPERTIES
        COMPILE_FLAGS "-march=armv8-a+crc+crypto")
    ENDIF()
  ENDIF()
ENDIF()

IF(CMAKE_SYSTEM_PROCESSOR MATCHES "ppc64|powerpc64" OR CMAKE_SYSTEM_NAME MATCHES AIX)
  SET(MYSYS_SOURCES ${MYSYS_SOURCES} crc32/crc32_ppc64.c crc32/crc32c_ppc.c)
  SET_SOURCE_FILES_PROPERTIES(crc32/crc32_ppc64.c crc32/crc32c_ppc.c PROPERTIES
        COMPILE_FLAGS "${COMPILE_FLAGS} -maltivec -mvsx -mpower8-vector -mcrypto -mpower8-vector")
  ADD_DEFINITIONS(-DHAVE_POWER8 -DHAS_ALTIVEC)
ELSE()
  SET (MYSYS_SOURCES ${MYSYS_SOURCES} crc32ieee.cc)
ENDIF()

IF(UNIX)
 SET (MYSYS_SOURCES ${MYSYS_SOURCES} my_addr_resolve.c my_setuser.c)
ENDIF()

IF(HAVE_ALARM)
 SET(MYSYS_SOURCES ${MYSYS_SOURCES} my_alarm.c)
ENDIF()

IF(HAVE_MLOCK)
 SET(MYSYS_SOURCES ${MYSYS_SOURCES} my_lockmem.c)
ENDIF()

ADD_CONVENIENCE_LIBRARY(mysys ${MYSYS_SOURCES})
MAYBE_DISABLE_IPO(mysys)
TARGET_LINK_LIBRARIES(mysys dbug strings ${ZLIB_LIBRARY}
 ${LIBNSL} ${LIBM} ${LIBRT} ${CMAKE_DL_LIBS} ${LIBSOCKET} ${LIBEXECINFO})
DTRACE_INSTRUMENT(mysys)

IF (HAVE_GCC_C11_ATOMICS_WITH_LIBATOMIC)
  TARGET_LINK_LIBRARIES(mysys atomic)
ENDIF()

IF(HAVE_BFD_H)
  TARGET_LINK_LIBRARIES(mysys bfd)  
ENDIF(HAVE_BFD_H)

IF (WIN32)
  TARGET_LINK_LIBRARIES(mysys iphlpapi dbghelp)
ENDIF(WIN32)

# Need explicit pthread for gcc -fsanitize=address
IF(CMAKE_USE_PTHREADS_INIT AND CMAKE_C_FLAGS MATCHES "-fsanitize=")
  TARGET_LINK_LIBRARIES(mysys pthread)
ENDIF()

ADD_EXECUTABLE(thr_lock thr_lock.c)
TARGET_LINK_LIBRARIES(thr_lock mysys)
SET_TARGET_PROPERTIES(thr_lock PROPERTIES COMPILE_FLAGS "-DMAIN")

ADD_EXECUTABLE(thr_timer thr_timer.c)
TARGET_LINK_LIBRARIES(thr_timer mysys)
SET_TARGET_PROPERTIES(thr_timer PROPERTIES COMPILE_FLAGS "-DMAIN")

ADD_EXECUTABLE(test_hash hash.c)
TARGET_LINK_LIBRARIES(test_hash mysys)
SET_TARGET_PROPERTIES(test_hash PROPERTIES COMPILE_FLAGS "-DMAIN")
