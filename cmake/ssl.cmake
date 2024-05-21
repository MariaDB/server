# Copyright (c) 2009, 2012, Oracle and/or its affiliates.
# Copyright (c) 2011, 2017, MariaDB Corporation
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

# We support different versions of SSL:
# - "bundled" uses source code in <source dir>/extra/wolfssl
# - "system"  (typically) uses headers/libraries in /usr/lib and /usr/lib64
# - a custom installation of openssl can be used like this
#     - cmake -DOPENSSL_ROOT_DIR=</path/to/custom/openssl>
#
# For custom build/install of openssl, see the accompanying README and
# INSTALL* files. When building with gcc, you must build the shared libraries
# (in addition to the static ones):
#   ./config --prefix=</path/to/custom/openssl> --shared; make; make install
# On some platforms (mac) you need to choose 32/64 bit architecture.
# Build/Install of openssl on windows is slightly different: you need to run
# perl and nmake. You might also need to
#   'set path=</path/to/custom/openssl>\bin;%PATH%
# in order to find the .dll files at runtime.

SET(WITH_SSL_DOC "bundled (use wolfssl)")
SET(WITH_SSL_DOC
  "${WITH_SSL_DOC}, yes (prefer os library if present, otherwise use bundled)")
SET(WITH_SSL_DOC
  "${WITH_SSL_DOC}, system (use os library)")
SET(WITH_SSL yes CACHE STRING ${WITH_SSL_DOC})

MACRO (CHANGE_SSL_SETTINGS string)
  SET(WITH_SSL ${string} CACHE STRING ${WITH_SSL_DOC} FORCE)
ENDMACRO()

MACRO (MYSQL_USE_BUNDLED_SSL)
  SET(INC_DIRS
    ${CMAKE_BINARY_DIR}/extra/wolfssl
    ${CMAKE_SOURCE_DIR}/extra/wolfssl/wolfssl
    ${CMAKE_SOURCE_DIR}/extra/wolfssl/wolfssl/wolfssl
  )
  SET(SSL_LIBRARIES  wolfssl)
  SET(SSL_INCLUDE_DIRS ${INC_DIRS})
  SET(SSL_DEFINES "-DHAVE_OPENSSL -DHAVE_WOLFSSL  -DWOLFSSL_USER_SETTINGS")
  SET(HAVE_ERR_remove_thread_state ON CACHE INTERNAL "wolfssl doesn't have ERR_remove_thread_state")
  SET(HAVE_EncryptAes128Ctr ON CACHE INTERNAL "wolfssl does support AES-CTR")
  SET(HAVE_EncryptAes128Gcm OFF CACHE INTERNAL "wolfssl does not support AES-GCM")
  SET(HAVE_des ON CACHE INTERNAL "wolfssl does support DES API")
  SET(HAVE_evp_pkey ON CACHE INTERNAL "wolfssl does support EVP_PKEY API")
  SET(HAVE_hkdf ON CACHE INTERNAL "wolfssl does support EVP_PKEY_HKDF API")
  CHANGE_SSL_SETTINGS("bundled")
  ADD_SUBDIRECTORY(extra/wolfssl)
  MESSAGE_ONCE(SSL_LIBRARIES "SSL_LIBRARIES = ${SSL_LIBRARIES}")
ENDMACRO()

# MYSQL_CHECK_SSL
#
# Provides the following configure options:
# WITH_SSL=[yes|bundled|system|<path/to/custom/installation>]
MACRO (MYSQL_CHECK_SSL)
  IF(NOT WITH_SSL)
   IF(WIN32)
     CHANGE_SSL_SETTINGS("bundled")
   ENDIF()
  ENDIF()

  IF (NOT WITH_SSL MATCHES "^(yes|system|bundled)$" AND EXISTS "${WITH_SSL}")
    IF (CMAKE_VERSION VERSION_LESS 3.24)
      # workaround for https://gitlab.kitware.com/cmake/cmake/-/issues/22945
      SET(OPENSSL_ROOT_DIR "${WITH_SSL}" "${WITH_SSL}/lib64")
    ELSE()
      SET(OPENSSL_ROOT_DIR "${WITH_SSL}")
    ENDIF()
  ENDIF()

  IF(WITH_SSL STREQUAL "bundled")
    MYSQL_USE_BUNDLED_SSL()
    # Reset some variables, in case we switch from /path/to/ssl to "bundled".
    IF (OPENSSL_ROOT_DIR)
      UNSET(OPENSSL_ROOT_DIR)
      UNSET(OPENSSL_ROOT_DIR CACHE)
    ENDIF()
    IF (OPENSSL_INCLUDE_DIR)
      UNSET(OPENSSL_INCLUDE_DIR)
      UNSET(OPENSSL_INCLUDE_DIR CACHE)
    ENDIF()
    IF (WIN32 AND OPENSSL_APPLINK_C)
      UNSET(OPENSSL_APPLINK_C)
      UNSET(OPENSSL_APPLINK_C CACHE)
    ENDIF()
    IF (OPENSSL_SSL_LIBRARY)
      UNSET(OPENSSL_SSL_LIBRARY)
      UNSET(OPENSSL_SSL_LIBRARY CACHE)
    ENDIF()
  ELSEIF(WITH_SSL STREQUAL "system" OR
         WITH_SSL STREQUAL "yes"    OR
         OPENSSL_ROOT_DIR)
    FIND_PACKAGE(OpenSSL)
    SET_PACKAGE_PROPERTIES(OpenSSL PROPERTIES TYPE RECOMMENDED)
    IF(OPENSSL_FOUND)
      SET(OPENSSL_LIBRARY ${OPENSSL_SSL_LIBRARY})
      SET(SSL_SOURCES "")
      SET(SSL_LIBRARIES ${OPENSSL_LIBRARIES})

      MESSAGE_ONCE(OPENSSL_INCLUDE_DIR "OPENSSL_INCLUDE_DIR = ${OPENSSL_INCLUDE_DIR}")
      MESSAGE_ONCE(OPENSSL_SSL_LIBRARY "OPENSSL_SSL_LIBRARY = ${OPENSSL_SSL_LIBRARY}")
      MESSAGE_ONCE(OPENSSL_CRYPTO_LIBRARY "OPENSSL_CRYPTO_LIBRARY = ${OPENSSL_CRYPTO_LIBRARY}")
      MESSAGE_ONCE(OPENSSL_VERSION "OPENSSL_VERSION = ${OPENSSL_VERSION}")
      MESSAGE_ONCE(SSL_LIBRARIES "SSL_LIBRARIES = ${SSL_LIBRARIES}")
      SET(SSL_INCLUDE_DIRS ${OPENSSL_INCLUDE_DIR})
      SET(SSL_INTERNAL_INCLUDE_DIRS "")
      SET(SSL_DEFINES "-DHAVE_OPENSSL")

      # Silence "deprecated in OpenSSL 3.0"
      IF((NOT OPENSSL_VERSION) # 3.0 not determined by older cmake
         OR NOT(OPENSSL_VERSION VERSION_LESS "3.0.0"))
        SET(SSL_DEFINES "${SSL_DEFINES} -DOPENSSL_API_COMPAT=0x10100000L")
        SET(CMAKE_REQUIRED_DEFINITIONS -DOPENSSL_API_COMPAT=0x10100000L)
      ENDIF()

      SET(CMAKE_REQUIRED_INCLUDES ${OPENSSL_INCLUDE_DIR})
      SET(CMAKE_REQUIRED_LIBRARIES ${SSL_LIBRARIES})
      SET(CMAKE_REQUIRED_INCLUDES ${OPENSSL_INCLUDE_DIR})
      INCLUDE(CheckSymbolExists)
      CHECK_SYMBOL_EXISTS(ERR_remove_thread_state "openssl/err.h"
                          HAVE_ERR_remove_thread_state)
      CHECK_SYMBOL_EXISTS(EVP_aes_128_ctr "openssl/evp.h"
                          HAVE_EncryptAes128Ctr)
      CHECK_SYMBOL_EXISTS(EVP_aes_128_gcm "openssl/evp.h"
                          HAVE_EncryptAes128Gcm)
      CHECK_SYMBOL_EXISTS(DES_set_key_unchecked "openssl/des.h"
                          HAVE_des)
      CHECK_SYMBOL_EXISTS(EVP_PKEY_get_raw_public_key "openssl/evp.h"
                          HAVE_evp_pkey)
      CHECK_SYMBOL_EXISTS(EVP_PKEY_CTX_set_hkdf_md "string.h;stdarg.h;openssl/kdf.h"
                          HAVE_hkdf)
      SET(CMAKE_REQUIRED_INCLUDES)
      SET(CMAKE_REQUIRED_LIBRARIES)
      SET(CMAKE_REQUIRED_DEFINITIONS)
    ELSE()
      IF(WITH_SSL STREQUAL "system")
        MESSAGE(FATAL_ERROR "Cannot find appropriate system libraries for SSL. Use WITH_SSL=bundled to enable SSL support")
      ENDIF()
      MYSQL_USE_BUNDLED_SSL()
    ENDIF()
  ELSE()
    MESSAGE(FATAL_ERROR
      "Wrong option for WITH_SSL. Valid values are: ${WITH_SSL_DOC}."
      "For custom location of OpenSSL library, use OPENSSL_ROOT_DIR pointing to the library."
    )
  ENDIF()
ENDMACRO()


# Many executables will depend on libeay32.dll and ssleay32.dll at runtime.
# In order to ensure we find the right version(s), we copy them into
# the same directory as the executables.
# NOTE: Using dlls will likely crash in malloc/free,
#       see INSTALL.W32 which comes with the openssl sources.
# So we should be linking static versions of the libraries.
MACRO (COPY_OPENSSL_DLLS target_name)
  IF (WIN32 AND WITH_SSL_PATH)
    GET_FILENAME_COMPONENT(CRYPTO_NAME "${OPENSSL_CRYPTO_LIBRARY}" NAME_WE)
    GET_FILENAME_COMPONENT(OPENSSL_NAME "${OPENSSL_SSL_LIBRARY}" NAME_WE)
    FILE(GLOB HAVE_CRYPTO_DLL "${WITH_SSL_PATH}/bin/${CRYPTO_NAME}.dll")
    FILE(GLOB HAVE_OPENSSL_DLL "${WITH_SSL_PATH}/bin/${OPENSSL_NAME}.dll")
    IF (HAVE_CRYPTO_DLL AND HAVE_OPENSSL_DLL)
      ADD_CUSTOM_COMMAND(OUTPUT ${target_name}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${WITH_SSL_PATH}/bin/${CRYPTO_NAME}.dll"
          "${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_CFG_INTDIR}/${CRYPTO_NAME}.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "${WITH_SSL_PATH}/bin/${OPENSSL_NAME}.dll"
          "${CMAKE_CURRENT_BINARY_DIR}/${CMAKE_CFG_INTDIR}/${OPENSSL_NAME}.dll"
        )
      ADD_CUSTOM_TARGET(${target_name} ALL)
    ENDIF()
  ENDIF()
ENDMACRO()
