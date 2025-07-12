# FRM Parser utility - selective source compilation
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
)

# Start with minimal set of SQL sources needed for FRM parsing
SET(FRM_SQL_SOURCES
    # Core FRM parsing files
    ${CMAKE_SOURCE_DIR}/sql/table.cc
    ${CMAKE_SOURCE_DIR}/sql/field.cc
    ${CMAKE_SOURCE_DIR}/sql/field_conv.cc
    ${CMAKE_SOURCE_DIR}/sql/unireg.cc
    ${CMAKE_SOURCE_DIR}/sql/key.cc
    ${CMAKE_SOURCE_DIR}/sql/sql_string.cc
    ${CMAKE_SOURCE_DIR}/sql/strfunc.cc
    
    # Basic infrastructure (minimal)
    ${CMAKE_SOURCE_DIR}/sql/sql_class.cc
    ${CMAKE_SOURCE_DIR}/sql/sql_table.cc
    ${CMAKE_SOURCE_DIR}/sql/create_options.cc
    ${CMAKE_SOURCE_DIR}/sql/charset_collations.cc
    ${CMAKE_SOURCE_DIR}/sql/sql_type.cc
    ${CMAKE_SOURCE_DIR}/sql/sql_type_string.cc
    
    # Handler infrastructure (partial)
    ${CMAKE_SOURCE_DIR}/sql/handler.cc
    
    # String and utility functions
    ${CMAKE_SOURCE_DIR}/sql/lex_string.h
    
    # Our custom mock file
    frm_mocks.cc
)

MYSQL_ADD_EXECUTABLE(mariadb-frm mariadb_frm.cc ${FRM_SQL_SOURCES})
TARGET_COMPILE_DEFINITIONS(mariadb-frm PRIVATE MYSQL_SERVER)

# Link with essential libraries only
TARGET_LINK_LIBRARIES(mariadb-frm 
    mysys 
    mysys_ssl 
    dbug 
    strings 
    vio 
    pcre2-8
    tpool
    ${LIBWRAP} 
    ${LIBCRYPT} 
    ${CMAKE_DL_LIBS} 
    ${CMAKE_THREAD_LIBS_INIT}
    ${SSL_LIBRARIES}
)

ADD_DEPENDENCIES(mariadb-frm GenError)

# Add dependency on pcre2 if it's a target
IF(TARGET pcre2)
  ADD_DEPENDENCIES(mariadb-frm pcre2)
ENDIF()
