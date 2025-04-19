INCLUDE(generate_submodule_info)
INCLUDE(ExternalProject)


# Extract user name and repository name from a github URL.
FUNCTION (EXTRACT_REPO_NAME_AND_USER repo_url repo_name_var repo_user_var)
  IF(repo_url MATCHES "^git@")
    # normalize to https-style URLs
    STRING(REGEX REPLACE "^git@([^:]+):(.*)$" "https://\\1/\\2" repo_url "${repo_url}")
  ENDIF()
  # Extract the repository user
  STRING(REGEX REPLACE "https://([^/]+)/([^/]+)/.*" "\\2" repo_user "${repo_url}")

  STRING(REGEX REPLACE ".*/([^/]*)$" "\\1" repo_name "${repo_url}")
  STRING(REGEX REPLACE "\\.git$" "" repo_name "${repo_name}")

  SET(${repo_name_var} ${repo_name} PARENT_SCOPE)
  SET(${repo_user_var} ${repo_user} PARENT_SCOPE)
ENDFUNCTION()

# Add a known 3rd party dependency for SBOM generation
# Currently used for "vendored" (part of our repository) source code we know about
# such as zlib, as well ExternalProject_Add() projects
MACRO(ADD_THIRD_PARTY_DEPENDENCY name url tag rev version description)
 LIST(FIND ALL_THIRD_PARTY ${name} idx)
 IF (idx GREATER -1)
   MESSAGE(FATAL_ERROR "${name} is already in ALL_THIRD_PARTY")
 ENDIF()
 SET(${name}_URL ${url})
 SET(${name}_TAG ${tag})
 SET(${name}_REVISION ${rev})
 SET(${name}_DESCRIPTION "${description}")
 SET(${name}_VERSION "${version}")
 LIST(APPEND ALL_THIRD_PARTY ${name})
ENDMACRO()

# Get CPE ID ( https://en.wikipedia.org/wiki/Common_Platform_Enumeration )
# for given project name and version
# CPE prefix are stored with other auxilliary info in the 3rdparty_info.cmake
# file
FUNCTION(SBOM_GET_CPE name version var)
  SET(${var} "" PARENT_SCOPE)
  STRING(FIND "${version}" "." dot_idx)
  IF(${dot_idx} EQUAL -1)
    # Version does not have dot inside.
    # mostly likely it is just a git hash
    RETURN()
  ENDIF()
  SET(cpe_name_and_vendor "${${repo_name_lower}.cpe-prefix}")
  IF(NOT cpe_name_and_vendor)
    RETURN()
  ENDIF()

  STRING(REGEX REPLACE "[^0-9\\.]" "" cleaned_version "${version}")
  SET(${var} "cpe:2.3:a:${cpe_name_and_vendor}:${cleaned_version}:*:*:*:*:*:*:*" PARENT_SCOPE)
ENDFUNCTION()

# Add dependency on CMake ExternalProject.
# Currently, only works for github hosted projects,
# URL property of the external project needs to point to release source download
MACRO(ADD_CMAKE_EXTERNAL_PROJECT_DEPENDENCY name)
  ExternalProject_GET_PROPERTY(${name} URL)
  STRING(REGEX REPLACE "https://github.com/([^/]+/[^/]+)/releases/download/([^/]+)/.*-([^-]+)\\..*" "\\1;\\2;\\3" parsed "${URL}")
  # Split the result into components
  LIST(LENGTH parsed parsed_length)
  IF(parsed_length EQUAL 3)
    LIST(GET parsed 0 project_path)
    LIST(GET parsed 1 tag)
    LIST(GET parsed 2 ver)
  ELSE()
    STRING(REGEX REPLACE "https://github.com/([^/]+/[^/]+)/archive/refs/tags/([^/]+)\\.(tar\\.gz|zip)$" "\\1;\\2;\\3" parsed "${URL}")
    LIST(LENGTH parsed parsed_length)
    IF(parsed_length GREATER 1)
      LIST(GET parsed 0 project_path)
      LIST(GET parsed 1 tag)
      STRING(REGEX REPLACE "[^0-9.]" "" ver "${tag}")
    ELSE()
      MESSAGE(FATAL_ERROR "Unexpected format for the download URL of project ${name} : (${URL})")
    ENDIF()
  ENDIF()
  ADD_THIRD_PARTY_DEPENDENCY(${name} "https://github.com/${project_path}" "${tag}" "${tag}" "${ver}" "")
ENDMACRO()


# Match third party component with supplier
# CyclonDX documentation says it is
#  "The organization that supplied the component.
#  The supplier may often be the manufacturer, but may also be a distributor or repackager."
#
# Perhaps it can always be "MariaDB", but security team recommendation is different
# more towards "author"
FUNCTION (sbom_get_supplier repo_name repo_user varname)
  IF("${${repo_name}_SUPPLIER}")
    SET(${varname} "${${repo_name}_SUPPLIER}" PARENT_SCOPE)
  ELSEIF (repo_name MATCHES "zlib|minizip")
    # stuff that is checked into out repos
    SET(${varname} "MariaDB" PARENT_SCOPE)
  ELSEIF (repo_name MATCHES "boost")
    SET(${varname} "Boost.org" PARENT_SCOPE)
  ELSEIF(repo_user MATCHES "mariadb-corporation|mariadb")
    SET(${varname} "MariaDB")
  ELSE()
    # Capitalize just first letter in repo_user
    STRING(SUBSTRING "${repo_user}" 0 1 first_letter)
    STRING(SUBSTRING "${repo_user}" 1 -1 rest)
    STRING(TOUPPER "${first_letter}" first_letter_upper)
    SET(${varname} "${first_letter_upper}${rest}" PARENT_SCOPE)
  ENDIF()
ENDFUNCTION()

# Generate sbom.json in the top-level build directory
FUNCTION(GENERATE_SBOM)
  IF(EXISTS ${PROJECT_SOURCE_DIR}/cmake/submodule_info.cmake)
    INCLUDE(${PROJECT_SOURCE_DIR}/cmake/submodule_info.cmake)
  ELSE()
    GENERATE_SUBMODULE_INFO(${PROJECT_BINARY_DIR}/cmake/submodule_info.cmake)
    INCLUDE(${PROJECT_BINARY_DIR}/cmake/submodule_info.cmake)
  ENDIF()
  # Remove irrelevant for the current build submodule information
  # That is, if we do not build say columnstore, do  not include
  # dependency info into SBOM
  IF(NOT TARGET wolfssl)
    # using openssl, rather than wolfssl
    LIST(FILTER ALL_SUBMODULES EXCLUDE REGEX wolfssl)
  ENDIF()
  IF(NOT WITH_WSREP)
    # wsrep is not compiled
    LIST(FILTER ALL_SUBMODULES EXCLUDE REGEX wsrep)
  ENDIF()
  IF(NOT TARGET columnstore)
    LIST(FILTER ALL_SUBMODULES EXCLUDE REGEX columnstore)
  ENDIF()
  IF(NOT TARGET rocksdb)
    # Rocksdb is not compiled
    LIST(FILTER ALL_SUBMODULES EXCLUDE REGEX rocksdb)
  ENDIF()
  IF(NOT TARGET s3)
    # S3 aria is not compiled
    LIST(FILTER ALL_SUBMODULES EXCLUDE REGEX storage/maria/libmarias3)
  ENDIF()
  # libmariadb/docs is not a library, so remove it
  LIST(FILTER ALL_SUBMODULES EXCLUDE REGEX libmariadb/docs)

  # It is possible to provide  EXTRA_SBOM_DEPENDENCIES
  # and accompanying per-dependency data, to extend generared sbom
  # document.
  # Example below injects an extra "ncurses" dependency  using several
  # command line parameters for CMake.
  # -DEXTRA_SBOM_DEPENDENCIES=ncurses
  # -Dncurses_URL=https://github.com/mirror/ncurses
  # -Dncurses_TAG=v6.4
  # -Dncurses_VERSION=6.4
  # -Dncurses_DESCRIPTION="A fake extra dependency"
  SET(ALL_THIRD_PARTY ${ALL_SUBMODULES} ${EXTRA_SBOM_DEPENDENCIES})

  # Add dependencies on cmake ExternalProjects
  FOREACH(ext_proj libfmt pcre2)
    IF(TARGET ${ext_proj})
      ADD_CMAKE_EXTERNAL_PROJECT_DEPENDENCY(${ext_proj})
    ENDIF()
  ENDFOREACH()

  # ZLIB
  IF(TARGET zlib OR TARGET connect)
    # Path to the zlib.h file
    SET(ZLIB_HEADER_PATH "${PROJECT_SOURCE_DIR}/zlib/zlib.h")
    # Variable to store the extracted version
    SET(ZLIB_VERSION "")
    # Read the version string from the file
    file(STRINGS "${ZLIB_HEADER_PATH}" ZLIB_VERSION_LINE REGEX "#define ZLIB_VERSION.*")
    # Extract the version number using a regex
    IF (ZLIB_VERSION_LINE)
      STRING(REGEX MATCH "\"([^\"]+)\"" ZLIB_VERSION_MATCH "${ZLIB_VERSION_LINE}")
      IF (ZLIB_VERSION_MATCH)
        STRING(REPLACE "\"" "" ZLIB_VERSION "${ZLIB_VERSION_MATCH}")
        IF(NOT ("${ZLIB_VERSION}" MATCHES "[0-9]+\\.[0-9]+\\.[0-9]+"))
          MESSAGE(FATAL_ERROR "Unexpected zlib version '${ZLIB_VERSION}' parsed from ${ZLIB_HEADER_PATH}")
        ENDIF()
      ELSE()
        MESSAGE(FATAL_ERROR "Could not extract ZLIB version from the line: ${ZLIB_VERSION_LINE}")
      ENDIF()
    ELSE()
      MESSAGE(FATAL_ERROR "ZLIB_VERSION definition not found in ${ZLIB_HEADER_PATH}")
    ENDIF()
    IF(TARGET zlib)
      ADD_THIRD_PARTY_DEPENDENCY(zlib "https://github.com/madler/zlib"
      "v${ZLIB_VERSION}" "v${ZLIB_VERSION}" "${ZLIB_VERSION}" "Vendored zlib included into server source")
    ENDIF()
    IF(TARGET ha_connect OR TARGET connect)
      SET(minizip_PURL "pkg:github/madler/zlib@${ZLIB_VERSION}?path=contrib/minizip")
      ADD_THIRD_PARTY_DEPENDENCY(minizip "https://github.com/madler/zlib?path=contrib/minizip"
      "v${ZLIB_VERSION}-minizip" "v${ZLIB_VERSION}-minizip" "${ZLIB_VERSION}"
      "Vendored minizip (zip.c, unzip.c, ioapi.c) in connect engine, copied from zlib/contributions")
    ENDIF()
  ENDIF()

  IF(TARGET columnstore)
    # Determining if Columnstore builds Boost is tricky.
    # The presence of the external_boost target isn't reliable, it is always
    # present. Instead, we check indirectly by verifying if one of the libraries
    # built by the external project exists in the build directory.
    IF(TARGET external_boost AND TARGET boost_filesystem)
      GET_TARGET_PROPERTY(boost_filesystem_loc boost_filesystem IMPORTED_LOCATION)
      STRING(FIND "${boost_filesystem_loc}" "${CMAKE_BINARY_DIR}" idx)
      IF(idx EQUAL 0)
        # Now we can be reasonably sure, external_boost is indeed an external project
        ExternalProject_GET_PROPERTY(external_boost URL)
        # Extract the version from the URL using string manipulation.
        STRING(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" BOOST_VERSION ${URL})
        SET(tag boost-${BOOST_VERSION})
        ADD_THIRD_PARTY_DEPENDENCY(boost
          "https://github.com/boostorg/boost" "${tag}" "${tag}" "${BOOST_VERSION}"
          "Boost library, linked with columnstore engine")
      ENDIF()
    ENDIF()
    IF(TARGET external_thrift)
      ADD_CMAKE_EXTERNAL_PROJECT_DEPENDENCY(external_thrift)
    ENDIF()
  ENDIF()

  SET(sbom_components "")
  SET(sbom_dependencies "\n    {
      \"ref\": \"${CPACK_PACKAGE_NAME}\",
      \"dependsOn\": [" )

  INCLUDE(3rdparty_info)
  SET(first ON)
  FOREACH(dep ${ALL_THIRD_PARTY})
    # Extract the part after the last "/" from URL
    SET(revision ${${dep}_REVISION})
    SET(tag ${${dep}_TAG})
    SET(desc ${${dep}_DESCRIPTION})
    IF((tag STREQUAL "no-tag") OR (NOT tag))
     SET(tag ${revision})
    ENDIF()
    IF (NOT "${revision}" AND "${tag}")
      SET(revision ${tag})
    ENDIF()
    SET(version ${${dep}_VERSION})

    IF (version)
    ELSEIF(tag)
      SET(version ${tag})
    ELSEIF(revision)
      SET(version ${revision})
    ENDIF()

    EXTRACT_REPO_NAME_AND_USER("${${dep}_URL}"  repo_name repo_user)

    IF(first)
      SET(first OFF)
    ELSE()
      STRING(APPEND sbom_components ",")
      STRING(APPEND sbom_dependencies ",")
    ENDIF()
    SET(bom_ref "${repo_name}-${version}")
    IF(desc)
      SET(desc_line "\n      \"description\": \"${desc}\",")
    ELSE()
      SET(desc_line "")
    ENDIF()
    STRING(TOLOWER "${repo_user}" repo_user_lower)
    STRING(TOLOWER "${repo_name}" repo_name_lower)
    IF (${repo_name_lower}_PURL)
      SET(purl "${${repo_name_lower}_PURL}")
    ELSE()
      SET(purl "pkg:github/${repo_user_lower}/${repo_name_lower}@${revision}")
    ENDIF()
    SBOM_GET_SUPPLIER(${repo_name_lower} ${repo_user_lower} supplier)
    SBOM_GET_CPE(${repo_name_lower} "${version}" cpe)
    IF(cpe)
      SET(cpe "\n      \"cpe\": \"${cpe}\",")
    ENDIF()
    SET(license "${${repo_name_lower}.license}")
    IF(NOT license)
      MESSAGE(FATAL_ERROR "no license for 3rd party dependency ${repo_name_lower}.")
    ENDIF()
    SET(copyright "${${repo_name_lower}.copyright}")
    IF(NOT copyright)
      SET(copyright NOASSERTION)
    ENDIF()
    STRING(APPEND sbom_components "
    {
      \"bom-ref\": \"${bom_ref}\",
      \"type\": \"library\",
      \"name\": \"${repo_name}\",
      \"version\": \"${version}\",${desc_line}
      \"purl\": \"${purl}\",${cpe}
      \"supplier\": {
          \"name\": \"${supplier}\"
       },
      \"licenses\": [
          {
            \"license\": {
              \"id\": \"${license}\"
            }
          }
        ],
      \"copyright\": \"${copyright}\"
    }")
    STRING(APPEND sbom_dependencies "
        \"${bom_ref}\"")
    STRING(APPEND reflist ",\n    {\"ref\": \"${bom_ref}\"}")
  ENDFOREACH()
  STRING(APPEND sbom_dependencies "\n      ]\n    }${reflist}\n")
  STRING(UUID UUID NAMESPACE ee390ca3-e70f-4b35-808e-a512489156f5 NAME SBOM TYPE SHA1)
  STRING(TIMESTAMP TIMESTAMP "%Y-%m-%dT%H:%M:%SZ" UTC)
  EXTRACT_REPO_NAME_AND_USER("${GIT_REMOTE_ORIGIN_URL}" GITHUB_REPO_NAME GITHUB_REPO_USER)
  #github-purl needs lowercased user and project names
  STRING(TOLOWER "${GITHUB_REPO_NAME}" GITHUB_REPO_NAME)
  STRING(TOLOWER "${GITHUB_REPO_USER}" GITHUB_REPO_USER)
  IF(NOT DEFINED CPACK_PACKAGE_VERSION)
    SET(CPACK_PACKAGE_VERSION "${CPACK_PACKAGE_VERSION_MAJOR}.${CPACK_PACKAGE_VERSION_MINOR}.${CPACK_PACKAGE_VERSION_PATCH}")
  ENDIF()
  STRING(TIMESTAMP CURRENT_YEAR "%Y")
  configure_file(${CMAKE_CURRENT_LIST_DIR}/cmake/sbom.json.in ${CMAKE_BINARY_DIR}/sbom.json)
ENDFUNCTION()
