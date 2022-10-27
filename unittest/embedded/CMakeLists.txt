
INCLUDE_DIRECTORIES(${CMAKE_SOURCE_DIR}/include
                    ${CMAKE_SOURCE_DIR}/libmysqld/include
                    ${PCRE_INCLUDES}
                    ${CMAKE_SOURCE_DIR}/sql
                    ${MY_READLINE_INCLUDE_DIR}
                    )


ADD_DEFINITIONS(-DEMBEDDED_LIBRARY -UMYSQL_CLIENT)


MYSQL_ADD_EXECUTABLE(test-connect-t test-connect.cc
               COMPONENT Test)
TARGET_LINK_LIBRARIES(test-connect-t mysqlserver )
MY_ADD_TEST(test-connect)

IF(UNIX)
SET_TARGET_PROPERTIES(test-connect-t PROPERTIES ENABLE_EXPORTS TRUE)
ENDIF()
