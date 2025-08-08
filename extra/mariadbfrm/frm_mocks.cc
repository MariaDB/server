#include "sql_priv.h"
#include "handler.h"
#include "sql_plugin.h"
#include "sql_class.h"
#include "table.h"
#include "sql_error.h"

plugin_ref plugin_lock(THD *thd, plugin_ref ptr) {
    return ptr; // Just return what was passed
}

void plugin_unlock(THD *thd, plugin_ref ptr) {
}

plugin_ref ha_resolve_by_name(THD *thd, const LEX_CSTRING *name, bool is_temp_table) {
  printf("plugin: %p\n", global_system_variables.table_plugin);
  return global_system_variables.table_plugin;
}

plugin_ref ha_lock_engine(THD *thd, const handlerton *hton) {
    return global_system_variables.table_plugin;
}

enum legacy_db_type ha_checktype(THD *thd, enum legacy_db_type database_type, bool no_substitute, bool report_error) {
    return DB_TYPE_MYISAM;
}

static handlerton mock_handlerton = {0};

handlerton *ha_default_handlerton(THD *thd) {
    return plugin_hton(global_system_variables.table_plugin);
}




handler *get_new_handler(TABLE_SHARE *share, MEM_ROOT *alloc, handlerton *db_type) {
    return nullptr;
}


void sql_print_warning(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
}

void sql_print_error(const char *format, ...) {

}

void push_warning_printf(THD *thd, Sql_condition::enum_warning_level level, uint code, const char *format, ...) {

}

void open_table_error(TABLE_SHARE *share, enum open_frm_error error, int db_errno) {

}

bool engine_table_options_frm_read(const uchar *buf, size_t length, TABLE_SHARE *share) {
    return false;
}

bool parse_engine_table_options(THD *thd, handlerton *ht, TABLE_SHARE *share) {
    return false;
}

bool parse_option_list(THD *thd, void *struct_ptr, ha_create_table_option *option_list,
                      bool suppress_warning, MEM_ROOT *root) {
    return false;
}


bool change_to_partiton_engine(plugin_ref *se_plugin) {
    return false;
}
