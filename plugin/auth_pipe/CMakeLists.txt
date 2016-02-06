IF(WIN32)
  MYSQL_ADD_PLUGIN(auth_named_pipe auth_pipe.c)
ENDIF()
