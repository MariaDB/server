IF((NOT MSVC) OR CLANG_CL OR WITH_ASAN)
  RETURN()
ENDIF()

# We need MFC
# /permissive- flag does not play well with MFC, disable it.
STRING(REPLACE "/permissive-"  "" CMAKE_CXX_FLAGS  "${CMAKE_CXX_FLAGS}")
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

IF(MSVC_CRT_TYPE MATCHES "/MD")
  # FORCE static CRT and MFC for upgrade wizard,
  # so we do not have to redistribute MFC.
  FORCE_STATIC_CRT()
  SET(UPGRADE_WIZARD_SOURCES  ${CMAKE_SOURCE_DIR}/sql/winservice.c)
ELSE()
  SET(UPGRADE_WIZARD_LINK_LIBRARIES winservice)
ENDIF()

# MFC should be statically linked
SET(CMAKE_MFC_FLAG 1)

# Enable exception handling (avoids warnings)
SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /EHsc -DNO_WARN_MBCS_MFC_DEPRECATION")

INCLUDE_DIRECTORIES(${CMAKE_SOURCE_DIR}/sql)
MYSQL_ADD_EXECUTABLE(mariadb-upgrade-wizard
  upgrade.cpp upgradeDlg.cpp upgrade.rc ${UPGRADE_WIZARD_SOURCES}
  COMPONENT Server)


TARGET_LINK_LIBRARIES(mariadb-upgrade-wizard ${UPGRADE_WIZARD_LINK_LIBRARIES})
# upgrade_wizard is Windows executable, set WIN32_EXECUTABLE so it does not
# create a console.
SET_TARGET_PROPERTIES(mariadb-upgrade-wizard PROPERTIES
  WIN32_EXECUTABLE 1
  LINK_FLAGS "/MANIFESTUAC:level='requireAdministrator'"
)
