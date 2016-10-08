#
# Wrapper for CPackRPM.cmake
#

macro(set_from_component WHAT)
  set(orig_CPACK_RPM_PACKAGE_${WHAT} ${CPACK_RPM_PACKAGE_${WHAT}})
  if(CPACK_RPM_${CPACK_RPM_PACKAGE_COMPONENT}_PACKAGE_${WHAT})
    set(CPACK_RPM_PACKAGE_${WHAT} ${CPACK_RPM_${CPACK_RPM_PACKAGE_COMPONENT}_PACKAGE_${WHAT}})
  endif()
endmacro()
macro(restore WHAT)
  set(CPACK_RPM_PACKAGE_${WHAT} ${orig_CPACK_RPM_PACKAGE_${WHAT}})
endmacro()

set_from_component(LICENSE)
set_from_component(VENDOR)

# load the original CPackRPM.cmake
set(orig_CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH})
unset(CMAKE_MODULE_PATH)
include(CPackRPM)
set(CMAKE_MODULE_PATH ${orig_CMAKE_MODULE_PATH})

restore(LICENSE)
restore(VENDOR)

# per-component cleanup
foreach(_RPM_SPEC_HEADER URL REQUIRES SUGGESTS PROVIDES OBSOLETES PREFIX CONFLICTS AUTOPROV AUTOREQ AUTOREQPROV)
  unset(TMP_RPM_${_RPM_SPEC_HEADER})
  unset(CPACK_RPM_PACKAGE_${_RPM_SPEC_HEADER}_TMP)
endforeach()

