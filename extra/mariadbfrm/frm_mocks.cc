#include "handler.h"
#include "sql_plugin.h"
#include "sql_class.h"
#include "table.h"
#include "sql_error.h"

class FRM_Mock_Handler : public handler
{
public:
  FRM_Mock_Handler(handlerton *hton_arg, TABLE_SHARE *share_arg)
      : handler(hton_arg, share_arg)
  {
    cached_table_flags= FRM_Mock_Handler::table_flags();
  }

  int open(const char *name, int mode, uint test_if_locked) override
  {
    return 0;
  }
  int close() override { return 0; }
  int write_row(const uchar *buf) override { return HA_ERR_WRONG_COMMAND; }
  int update_row(const uchar *old_data, const uchar *new_data) override
  {
    return HA_ERR_WRONG_COMMAND;
  }
  int delete_row(const uchar *buf) override { return HA_ERR_WRONG_COMMAND; }
  int index_read_map(uchar *buf, const uchar *key, key_part_map keypart_map,
                     ha_rkey_function find_flag) override
  {
    return HA_ERR_WRONG_COMMAND;
  }
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

  [[nodiscard]] ulong index_flags(uint idx, uint part,
                                  bool all_parts) const override
  {
    return (HA_READ_NEXT | HA_READ_PREV | HA_READ_ORDER | HA_READ_RANGE);
  }
  THR_LOCK_DATA **store_lock(THD *thd, THR_LOCK_DATA **to,
                             thr_lock_type lock_type) override
  {
    return to;
  }
  int create(const char *name, TABLE *form, HA_CREATE_INFO *info) override
  {
    return HA_ERR_WRONG_COMMAND;
  }
  [[nodiscard]] const char *table_type() const override { return "FRM_MOCK"; }
  [[nodiscard]] ulonglong table_flags() const override
  {
    return (HA_NO_TRANSACTIONS | HA_REC_NOT_IN_SEQ | HA_CAN_GEOMETRY);
  }
  [[nodiscard]] uint max_supported_key_length() const override { return 1000; }
  [[nodiscard]] uint max_supported_key_part_length() const override
  {
    return 255;
  }
  int delete_all_rows() override { return HA_ERR_WRONG_COMMAND; }
  ha_rows records_in_range(uint inx, const key_range *min_key,
                           const key_range *max_key,
                           page_range *pages) override
  {
    return 10;
  }
};

static handler *frm_mock_create_handler(handlerton *hton, TABLE_SHARE *table,
                                        MEM_ROOT *mem_root);

static handlerton frm_mock_hton_struct= {};
bool plugins_are_initialized= false;

static handler *frm_mock_create_handler(handlerton *hton, TABLE_SHARE *table,
                                        MEM_ROOT *mem_root)
{
  return new (mem_root) FRM_Mock_Handler(hton, table);
}

static void init_frm_mock_handlerton()
{
  frm_mock_hton_struct.create= frm_mock_create_handler;
  frm_mock_hton_struct.db_type= DB_TYPE_UNKNOWN;
  frm_mock_hton_struct.flags= HTON_NO_FLAGS;
  frm_mock_hton_struct.slot= 0;
  frm_mock_hton_struct.savepoint_offset= 0;
}

handlerton *ha_default_handlerton(THD *thd)
{
  static bool initialized= false;
  if (!initialized)
  {
    init_frm_mock_handlerton();
    initialized= true;
  }
  return &frm_mock_hton_struct;
}

handlerton *get_frm_mock_handlerton()
{
  static bool initialized= false;
  if (!initialized)
  {
    init_frm_mock_handlerton();
    initialized= true;
  }
  return &frm_mock_hton_struct;
}

plugin_ref plugin_lock(THD *thd, plugin_ref ptr) { return ptr; }

void plugin_unlock(THD *thd, plugin_ref ptr) {}

plugin_ref ha_resolve_by_name(THD *thd, const LEX_CSTRING *name,
                              bool is_temp_table)
{
  return global_system_variables.table_plugin;
}

plugin_ref ha_lock_engine(THD *thd, const handlerton *hton)
{
  return global_system_variables.table_plugin;
}

legacy_db_type ha_checktype(THD *thd, legacy_db_type database_type,
                            bool no_substitute, bool report_error)
{
  return DB_TYPE_MYISAM;
}

handler *get_new_handler(TABLE_SHARE *share, MEM_ROOT *alloc,
                         handlerton *db_type)
{
  return new (alloc) FRM_Mock_Handler(db_type, share);
}

void sql_print_warning(const char *format, ...)
{
  va_list args;
  va_start(args, format);
  vfprintf(stderr, format, args);
  va_end(args);
}

void sql_print_error(const char *format, ...) {}

void push_warning_printf(THD *thd, Sql_condition::enum_warning_level level,
                         uint code, const char *format, ...)
{
}

bool engine_table_options_frm_read(const uchar *buf, size_t length,
                                   TABLE_SHARE *share)
{
  return false;
}

bool parse_engine_table_options(THD *thd, handlerton *ht, TABLE_SHARE *share)
{
  return false;
}

bool parse_option_list(THD *thd, void *struct_ptr,
                       ha_create_table_option *option_list,
                       bool suppress_warning, MEM_ROOT *root)
{
  return false;
}

bool change_to_partiton_engine(plugin_ref *se_plugin) { return false; }

void TABLE_SHARE::update_optimizer_costs(handlerton *hton) {}

bool require_quotes(const char *name, size_t name_length) { return false; }

char get_quote_char_for_identifier(THD *thd, const char *name, size_t length)
{
  return '`';
}

extern "C"
{
  void pfs_destroy_mutex_v1(PSI_mutex *mutex) {}

  void destroy_mutex(void *pfs_mutex) {}

  bool pfs_enabled= false;

#ifdef SAFE_MUTEX
  int safe_mutex_init(safe_mutex_t *mp, const pthread_mutexattr_t *attr,
                      const char *name, const char *file, uint line)
  {
    bzero(mp, sizeof(*mp));
    const int result= pthread_mutex_init(&mp->mutex, attr);
    if (result == 0)
    {
      mp->file= file;
      mp->line= line;
      mp->name= name[0] == '&' ? name + 1 : name;
      mp->count= 0;
      mp->thread= 0;
      mp->create_flags= MYF_NO_DEADLOCK_DETECTION;
    }
    return result;
  }

  int safe_mutex_lock(safe_mutex_t *mp, myf my_flags, const char *file,
                      uint line)
  {
    int error;
    if (my_flags & MYF_TRY_LOCK)
      error= pthread_mutex_trylock(&mp->mutex);
    else
      error= pthread_mutex_lock(&mp->mutex);

    if (error == 0)
    {
      mp->thread= pthread_self();
      mp->count++;
      mp->file= file;
      mp->line= line;
    }
    return error;
  }

  int safe_mutex_unlock(safe_mutex_t *mp, const char *file, uint line)
  {
    mp->thread= 0;
    mp->count--;
    return pthread_mutex_unlock(&mp->mutex);
  }

  int safe_mutex_destroy(safe_mutex_t *mp, const char *file, uint line)
  {
    mp->file= nullptr;
    return pthread_mutex_destroy(&mp->mutex);
  }

  void safe_mutex_free_deadlock_data(safe_mutex_t *mp) {}

  int safe_cond_wait(pthread_cond_t *cond, safe_mutex_t *mp, const char *file,
                     uint line)
  {
    return pthread_cond_wait(cond, &mp->mutex);
  }

  int safe_cond_timedwait(pthread_cond_t *cond, safe_mutex_t *mp,
                          const struct timespec *abstime, const char *file,
                          uint line)
  {
    return pthread_cond_timedwait(cond, &mp->mutex, abstime);
  }
#endif // SAFE_MUTEX
}