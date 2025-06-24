FIND_PACKAGE(BZip2)

SET(CPACK_RPM_provider-bzip2_PACKAGE_SUMMARY "BZip2 compression support in the server and storage engines" PARENT_SCOPE)
SET(CPACK_RPM_provider-bzip2_PACKAGE_DESCRIPTION "BZip2 compression support in the server and storage engines" PARENT_SCOPE)

IF (BZIP2_FOUND)
  GET_PROPERTY(dirs DIRECTORY PROPERTY INCLUDE_DIRECTORIES)
  LIST(REMOVE_ITEM dirs ${CMAKE_SOURCE_DIR}/include/providers)
  SET_PROPERTY(DIRECTORY PROPERTY INCLUDE_DIRECTORIES "${dirs}")

  INCLUDE_DIRECTORIES(${BZIP2_INCLUDE_DIRS})

  MYSQL_ADD_PLUGIN(provider_bzip2 plugin.c COMPONENT provider-bzip2
    LINK_LIBRARIES ${BZIP2_LIBRARIES} CONFIG provider_bzip2.cnf)
ENDIF()
