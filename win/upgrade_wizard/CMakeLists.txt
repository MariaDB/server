IF((NOT MSVC) OR CLANG_CL OR WITH_ASAN)
  RETURN()
ENDIF()

# We need MFC
# /permissive- flag does not play well with MFC, disable it.
REMOVE_DEFINITIONS(-DNOSERVICE) # fixes "already defined" warning in an AFX header

FIND_PACKAGE(MFC)
IF(NOT MFC_FOUND)
  IF(BUILD_RELEASE)
    MESSAGE(FATAL_ERROR
    "Can't find MFC. It is necessary for producing official package"
  )
  ENDIF()
  RETURN()
ENDIF()

# MFC should be statically linked
SET(CMAKE_MFC_FLAG 1)

# Enable exception handling (avoids warnings)
ADD_DEFINITIONS(-DNO_WARN_MBCS_MFC_DEPRECATION)
INCLUDE_DIRECTORIES(${CMAKE_SOURCE_DIR}/sql)
MYSQL_ADD_EXECUTABLE(mariadb-upgrade-wizard
  upgrade.cpp upgradeDlg.cpp
  ${CMAKE_SOURCE_DIR}/sql/winservice.c
  upgrade.rc
  COMPONENT Server)

# upgrade_wizard is Windows executable, set WIN32_EXECUTABLE so it does not
# create a console.
SET_TARGET_PROPERTIES(mariadb-upgrade-wizard PROPERTIES
  WIN32_EXECUTABLE 1
  LINK_FLAGS "/MANIFESTUAC:level='requireAdministrator'"
  MSVC_RUNTIME_LIBRARY MultiThreaded
)
