SET(METADATA_LOCK_INFO_SOURCES metadata_lock_info.cc)
MYSQL_ADD_PLUGIN(metadata_lock_info ${METADATA_LOCK_INFO_SOURCES}
                 RECOMPILE_FOR_EMBEDDED)

# mdl elements need extern defination
SET_TARGET_PROPERTIES(metadata_lock_info PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
