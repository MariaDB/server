IF (HAVE_evp_pkey)
  ADD_DEFINITIONS(${SSL_DEFINES})
  IF(WITH_SSL STREQUAL "bundled")
    MYSQL_ADD_PLUGIN(auth_parsec server_parsec.cc STATIC_ONLY DEFAULT)
  ELSE()
    MYSQL_ADD_PLUGIN(auth_parsec server_parsec.cc
      LINK_LIBRARIES ${SSL_LIBRARIES})
  ENDIF()
ENDIF()
