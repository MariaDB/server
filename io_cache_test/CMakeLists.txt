INCLUDE_DIRECTORIES(${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/mysys)
MYSQL_ADD_EXECUTABLE(io_cache_test io_cache_test.cpp)
TARGET_LINK_LIBRARIES(io_cache_test mysys)