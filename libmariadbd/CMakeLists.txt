# Copyright (c) 2026, MariaDB plc
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

# MDEV-11111: embedded server as a private mariadbd process.
# Client side only, does not depend on the server sources.
INCLUDE_DIRECTORIES(${CMAKE_SOURCE_DIR}/libmariadb/include
                    ${CMAKE_BINARY_DIR}/libmariadb/include)
ADD_LIBRARY(mariadb_embedded_launcher STATIC
            embedded_launcher.c embedded_glue.c)
TARGET_LINK_LIBRARIES(mariadb_embedded_launcher ${CMAKE_DL_LIBS})

# libmariadbd: the client library (same objects as libmariadb, same exports)
# plus the launcher, registered when the library is loaded. An application
# links it instead of libmariadb and gets an embedded server from the usual
# mysql_server_init() / mysql_real_connect() / mysql_server_end().
IF(TARGET mariadb_obj AND TARGET mariadbclient)
  SET(MARIADB_CLIENT_EXPORTS
      ${CMAKE_BINARY_DIR}/libmariadb/libmariadb/mariadbclient.def)
  IF(WIN32)
    ADD_LIBRARY(libmariadbd SHARED $<TARGET_OBJECTS:mariadb_obj>
                embedded_launcher.c embedded_glue.c ${MARIADB_CLIENT_EXPORTS})
    SET_TARGET_PROPERTIES(libmariadbd PROPERTIES LINKER_LANGUAGE C)
  ELSE()
    ADD_LIBRARY(libmariadbd SHARED $<TARGET_OBJECTS:mariadb_obj>
                embedded_launcher.c embedded_glue.c)
    SET_TARGET_PROPERTIES(libmariadbd PROPERTIES
      COMPILE_FLAGS "${CMAKE_SHARED_LIBRARY_C_FLAGS}")
    # Same export list as libmariadb uses, where its linker takes one
    IF(CMAKE_SYSTEM_NAME MATCHES "Linux" OR
       CMAKE_SYSTEM_NAME MATCHES "kFreeBSD" OR
       CMAKE_SYSTEM_NAME MATCHES "GNU")
      SET_TARGET_PROPERTIES(libmariadbd PROPERTIES
        LINK_FLAGS "${MARIADB_CLIENT_EXPORTS}")
    ENDIF()
  ENDIF()
  SET_TARGET_PROPERTIES(libmariadbd PROPERTIES PREFIX ""
    COMPILE_DEFINITIONS MARIADB_EMBEDDED_AUTOREGISTER)
  TARGET_LINK_LIBRARIES(libmariadbd LINK_PRIVATE
    $<TARGET_GENEX_EVAL:mariadbclient,$<TARGET_PROPERTY:mariadbclient,LINK_LIBRARIES>>
    ${CMAKE_DL_LIBS})

  IF(UNIX)
    # Same API as the client library, hence the same version
    SET_TARGET_PROPERTIES(libmariadbd PROPERTIES
      SOVERSION "${SHARED_LIB_MAJOR_VERSION}")
    SET(COMPONENT_LIBMARIADBD "Server")
    # What libmysqld used to be called
    INSTALL_SYMLINK(libmysqld.so libmariadbd ${INSTALL_LIBDIR}
                    ${COMPONENT_LIBMARIADBD})
  ELSE()
    SET(COMPONENT_LIBMARIADBD "Embedded")
  ENDIF()
  INSTALL(TARGETS libmariadbd
    LIBRARY DESTINATION ${INSTALL_LIBDIR} COMPONENT ${COMPONENT_LIBMARIADBD}
    ARCHIVE DESTINATION ${INSTALL_LIBDIR} COMPONENT Development
    RUNTIME DESTINATION ${INSTALL_BINDIR} COMPONENT ${COMPONENT_LIBMARIADBD})

  # An unmodified client program: only the client API, linked with libmariadbd
  ADD_EXECUTABLE(embedded_app_test EXCLUDE_FROM_ALL embedded_app_test.c)
  TARGET_LINK_LIBRARIES(embedded_app_test libmariadbd)
  ADD_EXECUTABLE(embedded_auth_test EXCLUDE_FROM_ALL embedded_auth_test.c)
  TARGET_LINK_LIBRARIES(embedded_auth_test libmariadbd)
ENDIF()
