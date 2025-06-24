FIND_PACKAGE(LZO)

SET(CPACK_RPM_provider-lzo_PACKAGE_SUMMARY "LZO compression support in the server and storage engines" PARENT_SCOPE)
SET(CPACK_RPM_provider-lzo_PACKAGE_DESCRIPTION "LZO compression support in the server and storage engines" PARENT_SCOPE)

IF (LZO_FOUND)
  GET_PROPERTY(dirs DIRECTORY PROPERTY INCLUDE_DIRECTORIES)
  LIST(REMOVE_ITEM dirs ${CMAKE_SOURCE_DIR}/include/providers)
  SET_PROPERTY(DIRECTORY PROPERTY INCLUDE_DIRECTORIES "${dirs}")

  INCLUDE_DIRECTORIES(${LZO_INCLUDE_DIRS})

  MYSQL_ADD_PLUGIN(provider_lzo plugin.c COMPONENT provider-lzo
    LINK_LIBRARIES ${LZO_LIBRARIES} CONFIG provider_lzo.cnf)
ENDIF()
