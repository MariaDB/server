IF (TARGET rocksdb)
  INCLUDE_DIRECTORIES(${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/zlib
                      ${CMAKE_SOURCE_DIR}/unittest/mytap
    ${CMAKE_SOURCE_DIR}/rocksdb/third-party/gtest-1.7.0/fused-src
  )
  LINK_LIBRARIES(mytap mysys dbug strings)

  ADD_DEFINITIONS(-DSTANDALONE_UNITTEST)

  MYSQL_ADD_EXECUTABLE(test_properties_collector
          test_properties_collector.cc
          )
  TARGET_LINK_LIBRARIES(test_properties_collector mysqlserver)

  # Necessary to make sure that we can use the jemalloc API calls.
  GET_TARGET_PROPERTY(mysql_embedded LINK_FLAGS PREV_LINK_FLAGS)
  IF(NOT PREV_LINK_FLAGS)
    SET(PREV_LINK_FLAGS)
  ENDIF()
  SET_TARGET_PROPERTIES(test_properties_collector PROPERTIES LINK_FLAGS
  "${PREV_LINK_FLAGS} ${WITH_MYSQLD_LDFLAGS}")
ENDIF()
