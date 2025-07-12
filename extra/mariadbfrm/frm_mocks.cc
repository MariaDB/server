#include "mariadb.h"

#include "sql_plugin.h"
#include "handler.h"
#include "sql_class.h"
#include "mysqld.h"
#include "sql_string.h"
#include "my_global.h"

#include "frm_mocks.h"

// Global mutexes
mysql_mutex_t LOCK_global_system_variables;
mysql_mutex_t LOCK_status;
mysql_mutex_t LOCK_plugin;
mysql_mutex_t LOCK_gdl;
mysql_mutex_t LOCK_global_index_stats;
mysql_mutex_t LOCK_global_table_stats;
mysql_mutex_t LOCK_prepared_stmt_count;
mysql_mutex_t LOCK_optimizer_costs;
mysql_mutex_t LOCK_after_binlog_sync;
mysql_mutex_t LOCK_commit_ordered;
mysql_mutex_t LOCK_prepare_ordered;

// String constants
const LEX_CSTRING NULL_clex_str = {0, 0};
const LEX_CSTRING empty_clex_str = {"", 0};
const LEX_CSTRING null_clex_str = {0, 0};

// System variables
system_variables global_system_variables;
system_variables max_system_variables;
system_status_var global_status_var;

// Global variables with correct types
ulong opt_readonly = 0;
my_bool opt_bootstrap = 0;
uint lower_case_table_names = 0;
char reg_ext[FN_EXTLEN] = ".frm";
uint reg_ext_length = 4;

// Thread management
THD* _current_thd() {
    static THD* fake_thd = nullptr;
    return fake_thd;
}

void set_fake_current_thd(THD* thd) {
    // Stub for now
}

// Mock objects
handlerton mock_handlerton;
st_plugin_int mock_plugin_int;
static st_plugin_int *mock_plugin_ptr = &mock_plugin_int;
plugin_ref mock_plugin_ref = &mock_plugin_ptr;

// Plugin system
plugin_ref plugin_lock(THD*, plugin_ref*) { 
    return mock_plugin_ref; 
}

void plugin_unlock(THD*, plugin_ref) {
}

plugin_ref plugin_lock_by_name(THD*, const LEX_CSTRING*, int) {
    return mock_plugin_ref;
}

bool plugin_is_ready(const LEX_CSTRING*, int) {
    return true;
}

// Parser stubs
int MYSQLparse(THD*) { 
    return 0;
}

int ORAparse(THD*) {
    return 0;
}

// Warning/Error reporting
void push_warning(THD*, Sql_state_errno_level::enum_warning_level, 
                 uint, const char*) {
}

void push_warning_printf(THD*, Sql_state_errno_level::enum_warning_level, 
                       uint, const char*, ...) {
}

void sql_print_error(const char*, ...) {
}

void sql_print_warning(const char*, ...) {
}

void sql_print_information(const char*, ...) {
}

// Memory management
void init_sql_alloc(uint, MEM_ROOT*, uint, uint, myf) {
}

MEM_ROOT* get_thd_memroot(THD*) {
    return nullptr;
}

// Thread management
my_thread_id next_thread_id(void) {
    static my_thread_id thread_counter = 1;
    return thread_counter++;
}

// Infrastructure initialization
void plugin_mutex_init() { 
    mysql_mutex_init(0, &LOCK_global_system_variables, MY_MUTEX_INIT_FAST);
    mysql_mutex_init(0, &LOCK_status, MY_MUTEX_INIT_FAST);
    mysql_mutex_init(0, &LOCK_plugin, MY_MUTEX_INIT_FAST);
    mysql_mutex_init(0, &LOCK_gdl, MY_MUTEX_INIT_FAST);
    mysql_mutex_init(0, &LOCK_global_index_stats, MY_MUTEX_INIT_FAST);
    mysql_mutex_init(0, &LOCK_global_table_stats, MY_MUTEX_INIT_FAST);
    mysql_mutex_init(0, &LOCK_prepared_stmt_count, MY_MUTEX_INIT_FAST);
    mysql_mutex_init(0, &LOCK_optimizer_costs, MY_MUTEX_INIT_FAST);
    mysql_mutex_init(0, &LOCK_after_binlog_sync, MY_MUTEX_INIT_FAST);
    mysql_mutex_init(0, &LOCK_commit_ordered, MY_MUTEX_INIT_FAST);
    mysql_mutex_init(0, &LOCK_prepare_ordered, MY_MUTEX_INIT_FAST);
    
    printf("DEBUG: All critical mutexes initialized\n");
    fflush(stdout);
}
