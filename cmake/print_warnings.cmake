IF(NOT DEFINED WARN_MODE)
  IF(CMAKE_BUILD_TYPE MATCHES "Debug")
    SET(WARN_MODE "late")
  ELSE()
    SET(WARN_MODE "early")
  ENDIF()
ENDIF()

IF(NOT WARN_MODE STREQUAL "early" AND
    NOT WARN_MODE STREQUAL "late" AND
    NOT WARN_MODE STREQUAL "both")
  MESSAGE(FATAL_ERROR "Unknown WARN_MODE: expected 'early', 'late' or 'both'")
ENDIF()

IF(NOT WARN_MODE MATCHES "early")
  SET_DIRECTORY_PROPERTIES(PROPERTIES RULE_LAUNCH_COMPILE
    "bash ${CMAKE_SOURCE_DIR}/BUILD/capture_warnings.sh ${CMAKE_BINARY_DIR} ${WARN_MODE}")
  SET_DIRECTORY_PROPERTIES(PROPERTY ADDITIONAL_MAKE_CLEAN_FILES
    "${CMAKE_BINARY_DIR}/compile.warnings")
  ADD_CUSTOM_TARGET(rm_compile.warnings ALL
    COMMAND rm -f compile.warnings
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
  ADD_CUSTOM_TARGET(print_warnings ALL
    COMMAND bash -c '[ -f compile.warnings ] && { echo "Warnings found:" \; cat compile.warnings \; echo "" \; } \; true'
    DEPENDS mysql udf_example rm_compile.warnings
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")
ENDIF()
