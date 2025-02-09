MYSQL_ADD_PLUGIN(test_sql_discovery test_sql_discovery.cc STORAGE_ENGINE
  COMPONENT Test)

# missing symbols needs to be defined as extern?
SET_TARGET_PROPERTIES(test_sql_discovery PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)

