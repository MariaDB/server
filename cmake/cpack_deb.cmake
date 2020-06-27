#
# One day it'll be a complete solution for building deb packages with CPack
# But for now it's only to make INSTALL_DOCUMENTATION function happy
#
IF(DEB)
SET(CPACK_COMPONENT_SERVER_GROUP "server")
SET(CPACK_COMPONENT_README_GROUP "server")
SET(CPACK_COMPONENTS_ALL Server Test SharedLibraries)
SET(PYTHON_SHEBANG "/usr/bin/python3" CACHE STRING "python shebang")
ENDIF(DEB)

