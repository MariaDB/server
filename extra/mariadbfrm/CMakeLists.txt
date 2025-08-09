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

SET(FRM_PARSER_SOURCES mariadb_frm.cc frm_mocks.cc 
    ${CMAKE_SOURCE_DIR}/unittest/sql/dummy_builtins.cc
)


MYSQL_ADD_EXECUTABLE(mariadb-frm ${FRM_PARSER_SOURCES})

TARGET_COMPILE_DEFINITIONS(mariadb-frm PRIVATE MYSQL_SERVER)

TARGET_LINK_LIBRARIES(mariadb-frm
    sql
    sql_builtins 
    mysys
    strings
    dbug
    mysys_ssl
    ${ZLIB_LIBRARIES}
    ${SSL_LIBRARIES}
    ${CMAKE_THREAD_LIBS_INIT}
)
if(NOT APPLE)
    target_link_options(mariadb-frm PRIVATE -flto
                        "LINKER:--allow-multiple-definition"
                        "-ffunction-sections"
                        "LINKER:--gc-sections")
endif()

ADD_DEPENDENCIES(mariadb-frm GenError)