#
# Wrapper for CPackRPM.cmake
#

#
# Support for per-component LICENSE and VENDOR
#
# per component values, if present, are copied into global CPACK_RPM_PACKAGE_xxx
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

foreach (WHAT SUMMARY DESCRIPTION)
  if(CPACK_RPM_PACKAGE_COMPONENT AND
     NOT CPACK_RPM_${CPACK_RPM_PACKAGE_COMPONENT}_PACKAGE_${WHAT})
    message(FATAL_ERROR "CPACK_RPM_${CPACK_RPM_PACKAGE_COMPONENT}_PACKAGE_${WHAT} is not defined")
  endif()
endforeach()

set_from_component(LICENSE)
set_from_component(VENDOR)

#
# Support for the %posttrans scriptlet
#
# the scriptlet, if present, is appended (together with the %posttrans tag)
# to the pre-uninstall scriptlet
#
if(CMAKE_VERSION VERSION_LESS 3.18)
  set(base_time "PRE")
  set(base_type "UNINSTALL")
  set(base_var CPACK_RPM_${CPACK_RPM_PACKAGE_COMPONENT}_${base_time}_${base_type}_SCRIPT_FILE)
  set(acc)

  macro(read_one_file time_ type_ tag_)
    set(var CPACK_RPM_${CPACK_RPM_PACKAGE_COMPONENT}_${time_}_${type_}_SCRIPT_FILE)
    if (${var})
      file(READ ${${var}} content)
      set(acc "${tag_}\n${content}\n\n${acc}")
    endif()
  endmacro()

  read_one_file("POST" "TRANS" "%posttrans")
  if (acc)
    set(orig_${base_var} ${${base_var}})
    read_one_file(${base_time} ${base_type} "")
    set(${base_var} ${CPACK_TOPLEVEL_DIRECTORY}/SPECS/${CPACK_RPM_PACKAGE_COMPONENT}_${base_time}_${base_type}.scriptlet)
    file(WRITE ${${base_var}} "${acc}")
  endif()
endif(CMAKE_VERSION VERSION_LESS 3.18)

# load the original CPackRPM.cmake
set(orig_CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH})
unset(CMAKE_MODULE_PATH)
if (CMAKE_VERSION VERSION_GREATER "3.12.99")
  include(Internal/CPack/CPackRPM)
else()
  include(CPackRPM)
endif()
set(CMAKE_MODULE_PATH ${orig_CMAKE_MODULE_PATH})

restore(LICENSE)
restore(VENDOR)
if(${orig_${base_var}})
  set(${base_var} ${orig_${base_var}})
endif()

# per-component cleanup
foreach(_RPM_SPEC_HEADER URL REQUIRES SUGGESTS PROVIDES OBSOLETES PREFIX CONFLICTS AUTOPROV AUTOREQ AUTOREQPROV)
  unset(TMP_RPM_${_RPM_SPEC_HEADER})
  unset(CPACK_RPM_PACKAGE_${_RPM_SPEC_HEADER}_TMP)
endforeach()
