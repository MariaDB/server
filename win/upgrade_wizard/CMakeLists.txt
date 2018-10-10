IF(NOT MSVC)
  RETURN()
ENDIF()
IF(CMAKE_C_COMPILER_ID MATCHES Clang)
  # MFC stuff does not compile with clang
  RETURN()
ENDIF()
IF(CMAKE_USING_VC_FREE_TOOLS)
  # No MFC, so it cannot be built
  RETURN()
ENDIF()

# We need MFC
# /permissive- flag does not play well with MFC, disable it.
STRING(REPLACE "/permissive-"  "" CMAKE_CXX_FLAGS  "${CMAKE_CXX_FLAGS}")

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
MYSQL_ADD_EXECUTABLE(mysql_upgrade_wizard
  upgrade.cpp upgradeDlg.cpp upgrade.rc ${UPGRADE_WIZARD_SOURCES}
  COMPONENT Server)
TARGET_LINK_LIBRARIES(mysql_upgrade_wizard ${UPGRADE_WIZARD_LINK_LIBRARIES})
# upgrade_wizard is Windows executable, set WIN32_EXECUTABLE so it does not
# create a console.
SET_TARGET_PROPERTIES(mysql_upgrade_wizard PROPERTIES WIN32_EXECUTABLE 1)

# Embed Vista "admin" manifest, since upgrade_wizard needs admin privileges
# to change service configuration. Due to a CMake bug http://www.vtk.org/Bug/view.php?id=11171
# it is not possible currenly to do it with linker flags. Work around is to use
# manifest tool mt.exe and embed the manifest post-build.
ADD_CUSTOM_COMMAND(
  TARGET mysql_upgrade_wizard POST_BUILD
  COMMAND mt.exe -manifest ${CMAKE_CURRENT_SOURCE_DIR}/upgrade_wizard.exe.manifest
  "-outputresource:$<TARGET_FILE:mysql_upgrade_wizard>;#1"
)
