INCLUDE_DIRECTORIES(${CMAKE_SOURCE_DIR}/sql ${CMAKE_SOURCE_DIR}/pcre
                    ${CMAKE_SOURCE_DIR}/extra/yassl/include)

MYSQL_ADD_PLUGIN(QUERY_CACHE_INFO qc_info.cc)
