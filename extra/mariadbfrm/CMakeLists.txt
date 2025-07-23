# MariaDB FRM Parser - Minimal Real Environment
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

# Create the mariadb-frm executable with minimal dependencies
MYSQL_ADD_EXECUTABLE(mariadb-frm mariadb_frm.cc)

# Define as server component to get proper definitions
TARGET_COMPILE_DEFINITIONS(mariadb-frm PRIVATE MYSQL_SERVER)

# Link with essential MariaDB libraries in correct order
TARGET_LINK_LIBRARIES(mariadb-frm 
    sql                 # Core SQL functionality including TABLE_SHARE
    mysys              # MySQL system library
    strings            # String handling
    dbug               # Debug library
    mysys_ssl          # SSL support
    ${ZLIB_LIBRARIES}  # Compression
    ${SSL_LIBRARIES}   # SSL
    ${CMAKE_THREAD_LIBS_INIT}  # Threading
)

# Add dependency on code generation
ADD_DEPENDENCIES(mariadb-frm GenError)

# Add dependency on pcre2 if available
IF(TARGET pcre2)
  ADD_DEPENDENCIES(mariadb-frm pcre2)
ENDIF()

# Ensure we can find all needed headers at build time
ADD_DEPENDENCIES(mariadb-frm sql_builtins)
