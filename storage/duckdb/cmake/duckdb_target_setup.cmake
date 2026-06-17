#
# Common compile settings for all DuckDB plugin sub-libraries.
# Usage:  duckdb_setup_target(<target_name>)
#
MACRO(duckdb_setup_target _target)
  # DuckDB headers require C++17.
  SET_TARGET_PROPERTIES(${_target} PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
  )
  # libduckdb_bundle.a is built without debug STL wrappers.
  # -U unconditionally undefines the macro no matter how it was inherited
  # (CMAKE_CXX_FLAGS_DEBUG, directory properties, generator expressions, etc.).
  # Mismatched _GLIBCXX_DEBUG changes sizeof(std::vector) and friends -> SIGSEGV.
  TARGET_COMPILE_OPTIONS(${_target} PRIVATE
    -U_GLIBCXX_DEBUG -U_GLIBCXX_ASSERTIONS
  )
  IF(DUCKDB_WERROR)
    TARGET_COMPILE_OPTIONS(${_target} PRIVATE -Werror)
  ENDIF()
  ADD_DEPENDENCIES(${_target} GenError)
ENDMACRO()
