ADD_DEFINITIONS(${SSL_DEFINES})
SET(common_args auth_mysql_sha2 mysql_sha2.c sha256crypt.c ssl_stuff.c)
IF(WITH_SSL STREQUAL "bundled")
  # WolfSSL is static, we don't want it linked both into plugin and server
  MYSQL_ADD_PLUGIN(${common_args} STATIC_ONLY DEFAULT)
ELSE()
  MYSQL_ADD_PLUGIN(${common_args} openssl1-compat.c
                   LINK_LIBRARIES ${SSL_LIBRARIES})
ENDIF()
