# generate a cmake script containing git submodule version information.
# This is typically done during "make dist", and is needed for SBOM generation
# During the build from source package, git info is no more available.
#
# What is in this script
# - Variable ALL_SUBMODULES, a list of submodule subdirectories
# - for every entry in this list, there are 2 related variables defined
#     ${name}_REVISION, set to the git tag of submodule, or git hash, if tags are missing
#     ${name}_URL , set to the git URL of the submodule
#   For example, following will be generated for wolfssl
#   SET(extra/wolfssl/wolfssl_REVISION v5.7.2-stable)
#   SET(extra/wolfssl/wolfssl_URL https://github.com/wolfSSL/wolfssl.git)

FUNCTION(generate_submodule_info outfile)
  FIND_PACKAGE(Git REQUIRED)
  SET(git_cmd "(git describe --tags --exact 2>/dev/null || echo no-tag) && git rev-parse --short HEAD && git remote get-url origin")
  SET(ENV_LC_ALL "$ENV{LC_ALL}")
  SET($ENV{LC_ALL} C)
  EXECUTE_PROCESS(
    COMMAND
    ${GIT_EXECUTABLE} submodule foreach --recursive ${git_cmd}
    OUTPUT_VARIABLE outvar
    RESULT_VARIABLE  res
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  )
  IF(NOT(${res} EQUAL 0))
    MESSAGE(FATAL_ERROR "'git submodule foreach' failed")
  ENDIF()
  IF(NOT "${ENV_LC_ALL}" STREQUAL "")
   SET($ENV{LC_ALL} ${ENV_LC_ALL})
  ENDIF()

  STRING(REPLACE "\n" ";" out_list "${outvar}")
  SET(out_string)
  SET(all_submodules)
  SET(counter 0)
  FOREACH(s ${out_list})
    IF(${counter} EQUAL 0)
      IF (NOT("${s}" MATCHES "Entering '"))
        MESSAGE(FATAL "Unexpected output ${outvar}")
      ENDIF()
      STRING(LENGTH "${s}" slen)
      MATH(EXPR substr_len "${slen} - 11")
      STRING(SUBSTRING "${s}" 10  ${substr_len} submodule)
      LIST(APPEND all_submodules ${submodule})
    ELSEIF(${counter} EQUAL 1)
      # tag
      STRING(APPEND out_string "SET(${submodule}_TAG ${s})\n")
    ELSEIF(${counter} EQUAL 2)
      # get revision
      STRING(APPEND out_string "SET(${submodule}_REVISION ${s})\n")
    ELSEIF(${counter} EQUAL 3)
      # origin url
      STRING(APPEND out_string "SET(${submodule}_URL ${s})\n")
    ELSE()
      MESSAGE(FATAL_ERROR "should never happen")
    ENDIF()
    MATH(EXPR counter "(${counter}+1)%4")
  ENDFOREACH()
  STRING(APPEND out_string "SET(ALL_SUBMODULES \"${all_submodules}\")\n")
  # Also while not strictly "submodule" info, get the origin url
  IF(NOT GIT_REMOTE_ORIGIN_URL)
    EXECUTE_PROCESS(
      COMMAND
      ${GIT_EXECUTABLE} remote get-url origin
      OUTPUT_VARIABLE GIT_REMOTE_ORIGIN_URL
      OUTPUT_STRIP_TRAILING_WHITESPACE
      RESULT_VARIABLE  res
      WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    )
    IF(("${GIT_REMOTE_ORIGIN_URL}" STREQUAL "") OR NOT(${res} EQUAL 0))
      # Meh, origin is not called "origin", and there is no GIT_REMOTE_ORIGIN_URL
      # set. Fallback to hardcoded default
      SET(GIT_REMOTE_ORIGIN_URL https://github.com/mariadb/server.git)
    ENDIF()
  ENDIF()

  STRING(APPEND out_string "SET(GIT_REMOTE_ORIGIN_URL \"${GIT_REMOTE_ORIGIN_URL}\")\n")
  EXECUTE_PROCESS(
    COMMAND
    ${GIT_EXECUTABLE} rev-parse --short HEAD
    OUTPUT_VARIABLE outvar
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE  res
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  )
  STRING(APPEND out_string "SET(GIT_REV_SHORT \"${outvar}\")\n")
  SET(CMAKE_CONFIGURABLE_FILE_CONTENT ${out_string})
  CONFIGURE_FILE(${CMAKE_SOURCE_DIR}/cmake/configurable_file_content.in ${outfile} @ONLY)
ENDFUNCTION()
