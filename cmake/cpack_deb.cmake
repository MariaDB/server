#
# One day it'll be a complete solution for building deb packages with CPack
# But for now it's only to make INSTALL_DOCUMENTATION function happy
#
IF(DEB)
SET(CPACK_COMPONENT_SERVER_GROUP "server")
SET(CPACK_COMPONENT_README_GROUP "server")
SET(CPACK_COMPONENTS_ALL Server Test SharedLibraries)
SET(PYTHON_SHEBANG "/usr/bin/python3" CACHE STRING "python shebang")

FUNCTION(SET_PLUGIN_DEB_VERSION plugin ver)
  STRING(REPLACE "_" "-" plugin ${plugin})
  STRING(REPLACE "-" "." serverver ${SERVER_VERSION})
  STRING(REPLACE ${SERVER_VERSION} ${serverver} ver ${ver})
  FILE(READ ${CMAKE_SOURCE_DIR}/debian/changelog changelog)
  STRING(REPLACE ${serverver} ${ver} changelog "${changelog}")
  FILE(WRITE ${CMAKE_SOURCE_DIR}/debian/mariadb-plugin-${plugin}.changelog "${changelog}")
ENDFUNCTION()

ELSE(DEB)
FUNCTION(SET_PLUGIN_DEB_VERSION plugin ver)
ENDFUNCTION()
ENDIF(DEB)

