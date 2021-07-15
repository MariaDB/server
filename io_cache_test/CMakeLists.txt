INCLUDE_DIRECTORIES(${CMAKE_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/mysys)
MYSQL_ADD_EXECUTABLE(io_cache_test io_cache_test.cpp)
TARGET_LINK_LIBRARIES(io_cache_test mysys)

SET(INPUT_FILE_NAME input.txt)

CONFIGURE_FILE(input.txt input.txt COPYONLY)