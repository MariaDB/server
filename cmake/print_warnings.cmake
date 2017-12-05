IF(DEFINED WARN_MODE)
  IF(NOT WARN_MODE STREQUAL "early" AND
      NOT WARN_MODE STREQUAL "late" AND
      NOT WARN_MODE STREQUAL "both")
    MESSAGE(FATAL_ERROR "Unknown WARN_MODE: expected 'early', 'late' or 'both'")
  ENDIF()

  SET_DIRECTORY_PROPERTIES(PROPERTIES RULE_LAUNCH_COMPILE
    "bash ${CMAKE_SOURCE_DIR}/BUILD/capture_warnings.sh ${CMAKE_BINARY_DIR} ${WARN_MODE}")
  SET_DIRECTORY_PROPERTIES(PROPERTY ADDITIONAL_MAKE_CLEAN_FILES
    "${CMAKE_BINARY_DIR}/compile.warnings")
  ADD_CUSTOM_TARGET(rm_compile.warnings ALL
    COMMAND rm -f compile.warnings
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
  ADD_CUSTOM_TARGET(print_warnings ALL
    COMMAND bash -c '[ -f compile.warnings ] && { echo "Warnings found:" \; cat compile.warnings \; echo "" \; } \; true'
    DEPENDS mysqld rm_compile.warnings
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")

  IF(TARGET explain_filename-t)
    ADD_DEPENDENCIES(print_warnings explain_filename-t)
  ENDIF()

  IF(TARGET mysql_client_test)
    ADD_DEPENDENCIES(print_warnings mysql_client_test)
  ENDIF()

  IF(TARGET udf_example)
    ADD_DEPENDENCIES(print_warnings udf_example)
  ENDIF()
ENDIF()
