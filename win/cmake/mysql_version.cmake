
MACRO(MYSQL_GET_CONFIG_VALUE keyword var)
 IF(NOT ${var})
   IF (EXISTS ${CMAKE_SOURCE_DIR}/configure.in)
     FILE (STRINGS  ${CMAKE_SOURCE_DIR}/configure.in str  REGEX  "^[ ]*${keyword}=")
    IF(str)
      STRING(REPLACE "${keyword}=" "" str ${str})
      STRING(REGEX REPLACE  "[ ].*" ""  str ${str})
      SET(${var} ${str} CACHE INTERNAL "Config variable")
    ENDIF()
   ENDIF()
 ENDIF()
ENDMACRO()


# Read mysql version for configure script

MACRO(GET_MYSQL_VERSION)

  IF(NOT VERSION_STRING)
    IF(EXISTS ${CMAKE_SOURCE_DIR}/configure.in)
      FILE(STRINGS  ${CMAKE_SOURCE_DIR}/configure.in  str REGEX "AM_INIT_AUTOMAKE")
      STRING(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+[-][^ \\)]+" VERSION_STRING "${str}")
      IF(NOT VERSION_STRING)
        STRING(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" VERSION_STRING "${str}")
        IF(NOT VERSION_STRING)
          FILE(STRINGS  configure.in  str REGEX "AC_INIT\\(")
          STRING(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+[-][a-zA-Z0-9-]+" VERSION_STRING "${str}")
          IF(NOT VERSION_STRING)
            STRING(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" VERSION_STRING "${str}")
          ENDIF()
        ENDIF()
      ENDIF()
    ENDIF()
  ENDIF()
  
  SET(VERSION_EXTRA) #alpha beta gamma delta epsilon, etc
  
  FOREACH(suffix alpha beta gamma)
    IF(VERSION_STRING MATCHES "${suffix}")
      SET(VERSION_EXTRA "-${suffix}")
    ENDIF()
  ENDFOREACH()
  
  STRING(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+[-][a-zA-Z0-9]+" VERSION_STRING "${str}")
  
  IF(NOT VERSION_STRING)
    MESSAGE(FATAL_ERROR 
  "VERSION_STRING cannot be parsed, please specify -DVERSION_STRING=major.minor.patch-extra"
  "when calling cmake")
  ENDIF()
  
  SET(VERSION ${VERSION_STRING})
  STRING(REPLACE "-" "_" MYSQL_U_SCORE_VERSION "${VERSION_STRING}")
  
  # Remove trailing (non-numeric) part of the version string
  STRING(REGEX REPLACE "[^\\.0-9].*" "" VERSION_STRING ${VERSION_STRING})
 
  STRING(REGEX REPLACE "([0-9]+)\\.[0-9]+\\.[0-9]+" "\\1" MAJOR_VERSION "${VERSION_STRING}")
  STRING(REGEX REPLACE "[0-9]+\\.([0-9]+)\\.[0-9]+" "\\1" MINOR_VERSION "${VERSION_STRING}")
  STRING(REGEX REPLACE "[0-9]+\\.[0-9]+\\.([0-9]+)" "\\1" PATCH "${VERSION_STRING}")
  SET(MYSQL_BASE_VERSION "${MAJOR_VERSION}.${MINOR_VERSION}" CACHE INTERNAL "MySQL Base version")
  SET(MYSQL_NO_DASH_VERSION "${MAJOR_VERSION}.${MINOR_VERSION}.${PATCH}")
  MATH(EXPR MYSQL_VERSION_ID "10000*${MAJOR_VERSION} + 100*${MINOR_VERSION} + ${PATCH}")
  MARK_AS_ADVANCED(VERSION MYSQL_VERSION_ID MYSQL_BASE_VERSION)
  SET(CPACK_PACKAGE_VERSION_MAJOR ${MAJOR_VERSION})
  SET(CPACK_PACKAGE_VERSION_MINOR ${MINOR_VERSION})
  SET(CPACK_PACKAGE_VERSION_PATCH ${PATCH})
ENDMACRO()

# Get mysql version and other interesting variables
GET_MYSQL_VERSION()

MYSQL_GET_CONFIG_VALUE("PROTOCOL_VERSION" PROTOCOL_VERSION)
MYSQL_GET_CONFIG_VALUE("DOT_FRM_VERSION" DOT_FRM_VERSION)
MYSQL_GET_CONFIG_VALUE("MYSQL_TCP_PORT_DEFAULT" MYSQL_TCP_PORT_DEFAULT)
MYSQL_GET_CONFIG_VALUE("MYSQL_UNIX_ADDR_DEFAULT" MYSQL_UNIX_ADDR_DEFAULT)
MYSQL_GET_CONFIG_VALUE("SHARED_LIB_MAJOR_VERSION" SHARED_LIB_MAJOR_VERSION)
IF(NOT MYSQL_TCP_PORT_DEFAULT)
 SET(MYSQL_TCP_PORT_DEFAULT "3306")
ENDIF()
IF(NOT MYSQL_TCP_PORT)
  SET(MYSQL_TCP_PORT ${MYSQL_TCP_PORT_DEFAULT})
  SET(MYSQL_TCP_PORT_DEFAULT "0")
ELSEIF(MYSQL_TCP_PORT EQUAL MYSQL_TCP_PORT_DEFAULT)
  SET(MYSQL_TCP_PORT_DEFAULT "0")
ENDIF()


IF(NOT MYSQL_UNIX_ADDR)
  SET(MYSQL_UNIX_ADDR "/tmp/mysql.sock")
ENDIF()
IF(NOT COMPILATION_COMMENT)
  SET(COMPILATION_COMMENT "Source distribution")
ENDIF()


INCLUDE(package_name)
IF(NOT CPACK_PACKAGE_FILE_NAME)
  GET_PACKAGE_FILE_NAME(CPACK_PACKAGE_FILE_NAME)
ENDIF()

IF(NOT CPACK_SOURCE_PACKAGE_FILE_NAME)
  SET(CPACK_SOURCE_PACKAGE_FILE_NAME "mariadb-${VERSION_STRING}${VERSION_EXTRA}")
ENDIF()
SET(CPACK_PACKAGE_CONTACT "MariaDB team <info@montyprogram.com>")
SET(CPACK_PACKAGE_VENDOR "Monty Program AB")
SET(CPACK_SOURCE_GENERATOR "TGZ")
INCLUDE(cpack_source_ignore_files)

# Defintions for windows version resources
SET(PRODUCTNAME "MariaDB Server")
SET(COMPANYNAME ${CPACK_PACKAGE_VENDOR})

# Windows 'date' command has unpredictable output, so cannot rely on it to
# set MYSQL_COPYRIGHT_YEAR - if someone finds a portable way to do so then
# it might be useful
#IF (WIN32)
#  EXECUTE_PROCESS(COMMAND "date" "/T" OUTPUT_VARIABLE TMP_DATE)
#  STRING(REGEX REPLACE "(..)/(..)/..(..).*" "\\3\\2\\1" MYSQL_COPYRIGHT_YEAR ${TMP_DATE})
IF(UNIX)
  EXECUTE_PROCESS(COMMAND "date" "+%Y" OUTPUT_VARIABLE MYSQL_COPYRIGHT_YEAR OUTPUT_STRIP_TRAILING_WHITESPACE)
ENDIF()

# Add version information to the exe and dll files
# Refer to http://msdn.microsoft.com/en-us/library/aa381058(VS.85).aspx
# for more info.
IF(MSVC)
    # Tiny version is used to identify the build, it can be set with cmake -DTINY_VERSION=<number>
    # to bzr revno for example (in the CI builds)
    SET(TINY_VERSION "0" CACHE INTERNAL "")
  
    GET_FILENAME_COMPONENT(MYSQL_CMAKE_SCRIPT_DIR ${CMAKE_CURRENT_LIST_FILE} PATH)
	
    SET(FILETYPE VFT_APP)
    CONFIGURE_FILE(${MYSQL_CMAKE_SCRIPT_DIR}/versioninfo.rc.in 
    ${CMAKE_BINARY_DIR}/versioninfo_exe.rc)

    SET(FILETYPE VFT_DLL)
    CONFIGURE_FILE(${MYSQL_CMAKE_SCRIPT_DIR}/versioninfo.rc.in  
      ${CMAKE_BINARY_DIR}/versioninfo_dll.rc)
	  
  FUNCTION(ADD_VERSION_INFO target target_type sources_var)
    IF("${target_type}" MATCHES "SHARED" OR "${target_type}" MATCHES "MODULE")
      SET(rcfile ${CMAKE_BINARY_DIR}/versioninfo_dll.rc)
    ELSEIF("${target_type}" MATCHES "EXE")
      SET(rcfile ${CMAKE_BINARY_DIR}/versioninfo_exe.rc)
    ENDIF()
    SET(${sources_var} ${${sources_var}} ${rcfile} PARENT_SCOPE)
  ENDFUNCTION()
ELSE()
  FUNCTION(ADD_VERSION_INFO)
  ENDFUNCTION()
ENDIF()
