INCLUDE_DIRECTORIES(
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/sql
    ${CMAKE_BINARY_DIR}/sql
    ${CMAKE_BINARY_DIR}/include
    ${LIBFMT_INCLUDE_DIR}
    ${PCRE_INCLUDE_DIRS}
    ${ZLIB_INCLUDE_DIRS}
    ${SSL_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/tpool
    ${CMAKE_SOURCE_DIR}/mysys
    ${CMAKE_SOURCE_DIR}/strings
)

MYSQL_ADD_EXECUTABLE(mariadb-frm mariadb_frm.cc)

TARGET_COMPILE_DEFINITIONS(mariadb-frm PRIVATE MYSQL_SERVER)

TARGET_LINK_LIBRARIES(mariadb-frm
    sql
    mysys
    strings
    dbug
    mysys_ssl
    ${ZLIB_LIBRARIES}
    ${SSL_LIBRARIES}
    ${CMAKE_THREAD_LIBS_INIT}
)

ADD_DEPENDENCIES(mariadb-frm GenError)

IF(TARGET pcre2)
  ADD_DEPENDENCIES(mariadb-frm pcre2)
ENDIF()

ADD_DEPENDENCIES(mariadb-frm sql_builtins)
