FIND_PACKAGE(ZSTD)

SET(CPACK_RPM_provider-zstd_PACKAGE_SUMMARY "Zstandard compression support in the server and storage engines" PARENT_SCOPE)
SET(CPACK_RPM_provider-zstd_PACKAGE_DESCRIPTION "Zstandard compression support in the server and storage engines" PARENT_SCOPE)

IF (ZSTD_FOUND)
  GET_PROPERTY(dirs DIRECTORY PROPERTY INCLUDE_DIRECTORIES)
  LIST(REMOVE_ITEM dirs ${CMAKE_SOURCE_DIR}/include/providers)
  SET_PROPERTY(DIRECTORY PROPERTY INCLUDE_DIRECTORIES "${dirs}")

  INCLUDE_DIRECTORIES(${ZSTD_INCLUDE_DIRS})

  MYSQL_ADD_PLUGIN(provider_zstd plugin.c COMPONENT provider-zstd
    LINK_LIBRARIES ${ZSTD_LIBRARIES} CONFIG provider_zstd.cnf)
ENDIF()
