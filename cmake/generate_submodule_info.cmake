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

# Helper function to execute a git command in a specified directory.
# If the command succeeds, the output is stored in the result variable.
# Otherwise, the result variable is set to an empty string.
FUNCTION(INVOKE_GIT command working_dir result_var)
  EXECUTE_PROCESS(
    COMMAND ${GIT_EXECUTABLE} ${command}
    RESULT_VARIABLE res OUTPUT_VARIABLE out
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_VARIABLE err
    WORKING_DIRECTORY ${working_dir}
  )
  IF (res EQUAL 0)
    SET(${result_var} ${out} PARENT_SCOPE)
    MESSAGE("'${GIT_EXECUTABLE} ${command}'  in ${working_dir} returned '${out}'")
  ELSE()
    MESSAGE("'${GIT_EXECUTABLE} ${command}' failed in ${working_dir} with ${res} : ${err}")
    SET(${result_var} "" PARENT_SCOPE)
  ENDIF()
ENDFUNCTION()

# Get short hash of the current git commit in the specified
# directory.
MACRO(GET_GIT_REVISION working_dir result_var)
  INVOKE_GIT("rev-parse;--short;HEAD" ${working_dir} ${result_var})
ENDMACRO()

# Get URL of the remote origin for the git repository in the
# specified directory.
MACRO(GET_GIT_URL working_dir result_var)
  INVOKE_GIT("remote;get-url;origin" ${working_dir} ${result_var})
ENDMACRO()

# Get git tag corresponding to the latest commit in specified directory
# If "git describe --tags --exact" fails, the fallback is to use
# "git ls-remote --tags origin", because tags are not always checked out
FUNCTION(GET_GIT_TAG working_dir result_var)
  SET(${result_var} "no-tag" PARENT_SCOPE)

  # Try to find exact tag, this can fail due if checkout depth is 1
  INVOKE_GIT("describe;--tags;--exact" ${working_dir} out)
  IF(out)
    SET(${result_var} "${out}" PARENT_SCOPE)
    RETURN()
  ENDIF()

  # Get current commit hash
  INVOKE_GIT("rev-parse;HEAD" ${working_dir} head_hash)
  IF(NOT head_hash)
    RETURN()
  ENDIF()

  # Try to find tag in remote
  INVOKE_GIT("ls-remote;--tags;origin" ${working_dir} remote_tags)
  IF (NOT remote_tags)
    RETURN()
  ENDIF()

  STRING(REPLACE "\n" ";" tag_list "${remote_tags}")
  FOREACH(t ${tag_list})
    # Look for a line starting with the current commit hash
    IF(t MATCHES "^${head_hash}[ \t]+refs/tags/")
      STRING(REGEX REPLACE "${head_hash}[ \t]+refs/tags/" "" tag "${t}")
      # The commit referenced by a tag is marked with trailing ^{}
      STRING(REPLACE "^{}" "" tag "${tag}")
      SET(${result_var} "${tag}" PARENT_SCOPE)
      RETURN()
    ENDIF()
  ENDFOREACH()
ENDFUNCTION()

FUNCTION(generate_submodule_info outfile)
  FIND_PACKAGE(Git REQUIRED)
  SET(ENV_LC_ALL "$ENV{LC_ALL}")
  SET($ENV{LC_ALL} C)

  INVOKE_GIT("submodule;foreach;--recursive" ${CMAKE_SOURCE_DIR} outvar)
  IF(NOT outvar)
    MESSAGE(FATAL_ERROR "'git submodule foreach' failed")
  ENDIF()
  IF(NOT "${ENV_LC_ALL}" STREQUAL "")
   SET($ENV{LC_ALL} ${ENV_LC_ALL})
  ENDIF()

  STRING(REPLACE "\n" ";" out_list "${outvar}")
  SET(out_string)
  SET(all_submodules)
  FOREACH(s ${out_list})
    IF (NOT("${s}" MATCHES "Entering '"))
      MESSAGE(FATAL "Unexpected output ${outvar}")
    ENDIF()
    STRING(LENGTH "${s}" slen)
    MATH(EXPR substr_len "${slen} - 11")
    STRING(SUBSTRING "${s}" 10  ${substr_len} submodule)
    LIST(APPEND all_submodules ${submodule})
    SET(submodule_dir "${CMAKE_SOURCE_DIR}/${submodule}")
    GET_GIT_TAG(${submodule_dir} tag)
    STRING(APPEND out_string "SET(${submodule}_TAG ${tag})\n")
    GET_GIT_REVISION(${submodule_dir} revision)
    STRING(APPEND out_string "SET(${submodule}_REVISION ${revision})\n")
    GET_GIT_URL(${submodule_dir} url)
    STRING(APPEND out_string "SET(${submodule}_URL ${url})\n")
  ENDFOREACH()

  STRING(APPEND out_string "SET(ALL_SUBMODULES \"${all_submodules}\")\n")
  # Also while not strictly "submodule" info, get the origin url
  IF(NOT GIT_REMOTE_ORIGIN_URL)
    GET_GIT_URL(${CMAKE_SOURCE_DIR} GIT_REMOTE_ORIGIN_URL)
    IF(NOT GIT_REMOTE_ORIGIN_URL)
      # Meh, origin is not called "origin", and there is no GIT_REMOTE_ORIGIN_URL
      # set. Fallback to hardcoded default
      SET(GIT_REMOTE_ORIGIN_URL https://github.com/mariadb/server.git)
    ENDIF()
  ENDIF()

  STRING(APPEND out_string "SET(GIT_REMOTE_ORIGIN_URL \"${GIT_REMOTE_ORIGIN_URL}\")\n")
  GET_GIT_REVISION(${CMAKE_SOURCE_DIR} outvar)
  STRING(APPEND out_string "SET(GIT_REV_SHORT \"${outvar}\")\n")
  SET(CMAKE_CONFIGURABLE_FILE_CONTENT ${out_string})
  CONFIGURE_FILE(${CMAKE_SOURCE_DIR}/cmake/configurable_file_content.in ${outfile} @ONLY)
  FILE(READ "${outfile}" submodule_info)
  message("---submodule info --")
  message("${submodule_info}")
  message("---submodule info end--")
ENDFUNCTION()
