#
# Wrapper for CPackRPM.cmake
#

# load the original CPackRPM.cmake
# http://public.kitware.com/Bug/view.php?id=14782
if (CMAKE_VERSION VERSION_LESS "3.3.1" AND CMAKE_VERSION VERSION_GREATER "2.8.12.2")
  message("CPackRPM: using dodgy workaround since CMake < 3.3.1")
  include(CPackRPM-3.2.2)
else()
  set(orig_CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH})
  unset(CMAKE_MODULE_PATH)
  include(CPackRPM)
  set(CMAKE_MODULE_PATH ${orig_CMAKE_MODULE_PATH})
endif()

# per-component cleanup
foreach(_RPM_SPEC_HEADER URL REQUIRES SUGGESTS PROVIDES OBSOLETES PREFIX CONFLICTS AUTOPROV AUTOREQ AUTOREQPROV)
  unset(TMP_RPM_${_RPM_SPEC_HEADER})
  unset(CPACK_RPM_PACKAGE_${_RPM_SPEC_HEADER}_TMP)
endforeach()

