#include "handler.h"
#include "sql_plugin.h"
#include "sql_class.h"
#include "table.h"
#include "sql_error.h"
#include "sql_priv.h"

class FRM_Mock_Handler : public handler
{
public:
  FRM_Mock_Handler(handlerton *hton_arg, TABLE_SHARE *share_arg)
    : handler(hton_arg, share_arg) {}

  int open(const char *name, int mode, uint test_if_locked) override { return 0; }
  int close(void) override { return 0; }
  int write_row(const uchar *buf) override { return HA_ERR_WRONG_COMMAND; }
  int update_row(const uchar *old_data, const uchar *new_data) override { return HA_ERR_WRONG_COMMAND; }
  int delete_row(const uchar *buf) override { return HA_ERR_WRONG_COMMAND; }
  int index_read_map(uchar *buf, const uchar *key, key_part_map keypart_map,
                     enum ha_rkey_function find_flag) override { return HA_ERR_WRONG_COMMAND; }
  int index_next(uchar *buf) override { return HA_ERR_WRONG_COMMAND; }
  int index_prev(uchar *buf) override { return HA_ERR_WRONG_COMMAND; }
  int index_first(uchar *buf) override { return HA_ERR_WRONG_COMMAND; }
  int index_last(uchar *buf) override { return HA_ERR_WRONG_COMMAND; }
  int rnd_init(bool scan) override { return 0; }
  int rnd_end() override { return 0; }
  int rnd_next(uchar *buf) override { return HA_ERR_END_OF_FILE; }
  int rnd_pos(uchar *buf, uchar *pos) override { return HA_ERR_WRONG_COMMAND; }
  void position(const uchar *record) override {}
  int info(uint flag) override { return 0; }

  ulong index_flags(uint idx, uint part, bool all_parts) const override {
    return (HA_READ_NEXT | HA_READ_PREV | HA_READ_ORDER | HA_READ_RANGE);
  }
  THR_LOCK_DATA **store_lock(THD *thd, THR_LOCK_DATA **to, enum thr_lock_type lock_type) override {
    return to;
  }
  int create(const char *name, TABLE *form, HA_CREATE_INFO *info) override {
    return HA_ERR_WRONG_COMMAND;
  }
  const char *table_type() const override { return "FRM_MOCK"; }
  ulonglong table_flags() const override {
    return (HA_NO_TRANSACTIONS | HA_REC_NOT_IN_SEQ | HA_CAN_GEOMETRY);
  }
  uint max_supported_key_length() const override { return 1000; }
  uint max_supported_key_part_length() const override { return 255; }
  int delete_all_rows() override { return HA_ERR_WRONG_COMMAND; }
  ha_rows records_in_range(uint inx, const key_range *min_key,
                          const key_range *max_key, page_range *pages) override { return 10; }
};

static handler *frm_mock_create_handler(handlerton *hton, TABLE_SHARE *table, MEM_ROOT *mem_root);

static handlerton frm_mock_hton_struct = {0};

static handler *frm_mock_create_handler(handlerton *hton, TABLE_SHARE *table, MEM_ROOT *mem_root) {
    return new (mem_root) FRM_Mock_Handler(hton, table);
}

static void init_frm_mock_handlerton() {
    frm_mock_hton_struct.create = frm_mock_create_handler;
    frm_mock_hton_struct.db_type = DB_TYPE_UNKNOWN;

    frm_mock_hton_struct.flags = HTON_NO_FLAGS;

    static const char* engine_name = "FRM_MOCK";
    static const char* engine_comment = "FRM parsing mock engine";


    frm_mock_hton_struct.slot = 0;
    frm_mock_hton_struct.savepoint_offset = 0;
    frm_mock_hton_struct.close_connection = nullptr;
    frm_mock_hton_struct.savepoint_set = nullptr;
    frm_mock_hton_struct.savepoint_rollback = nullptr;
    frm_mock_hton_struct.savepoint_rollback_can_release_mdl = nullptr;
    frm_mock_hton_struct.savepoint_release = nullptr;
    frm_mock_hton_struct.commit = nullptr;
    frm_mock_hton_struct.rollback = nullptr;
    frm_mock_hton_struct.prepare = nullptr;
    frm_mock_hton_struct.recover = nullptr;
    frm_mock_hton_struct.commit_by_xid = nullptr;
    frm_mock_hton_struct.rollback_by_xid = nullptr;
    frm_mock_hton_struct.drop_database = nullptr;
    frm_mock_hton_struct.panic = nullptr;
    frm_mock_hton_struct.start_consistent_snapshot = nullptr;
    frm_mock_hton_struct.flush_logs = nullptr;
    frm_mock_hton_struct.show_status = nullptr;
}

handlerton *ha_default_handlerton(THD* thd) {
  static bool initialized = false;
  if (!initialized) {
    init_frm_mock_handlerton();
    initialized = true;
  }
  return &frm_mock_hton_struct;
}

handlerton* get_frm_mock_handlerton() {
  static bool initialized = false;
  if (!initialized) {
    init_frm_mock_handlerton();
    initialized = true;
  }
  return &frm_mock_hton_struct;
}

plugin_ref plugin_lock(THD *thd, plugin_ref ptr) {
    return ptr;
}

void plugin_unlock(THD *thd, plugin_ref ptr) {
}

plugin_ref ha_resolve_by_name(THD *thd, const LEX_CSTRING *name, bool is_temp_table) {
    return global_system_variables.table_plugin;
}

plugin_ref ha_lock_engine(THD *thd, const handlerton *hton) {
    return global_system_variables.table_plugin;
}

enum legacy_db_type ha_checktype(THD *thd, enum legacy_db_type database_type, bool no_substitute, bool report_error) {
    return DB_TYPE_MYISAM;
}


handler *get_new_handler(TABLE_SHARE *share, MEM_ROOT *alloc, handlerton *db_type) {
    return new (alloc) FRM_Mock_Handler(db_type, share);
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

void TABLE_SHARE::update_optimizer_costs(handlerton *hton) {
    return;
}

bool require_quotes(const char *name, size_t name_length) {
  return false;
}

char get_quote_char_for_identifier(THD *thd, const char *name, size_t length) {
  return '`';
}
