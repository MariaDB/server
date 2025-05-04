ADD_DEFINITIONS(${SSL_DEFINES})
IF(WITH_SSL STREQUAL "bundled")
  # WolfSSL is static, we don't want it linked both into plugin and server
  SET(static STATIC_ONLY DEFAULT)
ENDIF()
MYSQL_ADD_PLUGIN(auth_mysql_sha2
  mysql_sha2.c sha256crypt.c ssl_stuff.c openssl1-compat.c
  LINK_LIBRARIES ${SSL_LIBRARIES} ${static})
