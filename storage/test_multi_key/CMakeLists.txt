# Plugins on Windows link against server.dll, which exports only the sql,
# mysys, dbug and strings objects, so ha_heap is out of their reach.
IF(WIN32)
  RETURN()
ENDIF()

MYSQL_ADD_PLUGIN(test_multi_key ha_test_multi_key.cc STORAGE_ENGINE
  COMPONENT Test)
