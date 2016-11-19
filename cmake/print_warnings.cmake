IF (NOT DEFINED WITHOUT_REPRINT AND CMAKE_BUILD_TYPE MATCHES "Debug")
  SET_DIRECTORY_PROPERTIES(PROPERTIES RULE_LAUNCH_COMPILE
    "bash ${CMAKE_SOURCE_DIR}/BUILD/capture_warnings.sh ${CMAKE_BINARY_DIR}")
  SET_DIRECTORY_PROPERTIES(PROPERTY ADDITIONAL_MAKE_CLEAN_FILES
    "${CMAKE_BINARY_DIR}/compile.warnings")
  ADD_CUSTOM_TARGET(rm_compile.warnings ALL
    COMMAND rm -f compile.warnings
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
  ADD_CUSTOM_TARGET(print_warnings ALL
    COMMAND bash -c '[ -f compile.warnings ] && { echo "Warnings found:" \; cat compile.warnings \; echo "" \; } \; true'
    DEPENDS mysql udf_example explain_filename-t rm_compile.warnings
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}")
ENDIF()
