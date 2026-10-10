
MYSQL_ADD_PLUGIN(plugin_name_ascii plugin_name_ascii.cc
        MODULE_ONLY
        NOT_EMBEDDED
        COMPONENT Test)

MYSQL_ADD_PLUGIN(plugin_name_non_ascii plugin_name_non_ascii.cc
        MODULE_ONLY
        NOT_EMBEDDED
        COMPONENT Test)
