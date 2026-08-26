/* Dummy storage engine, built out-of-tree against an installed
   MariaDB-devel/SDK via mariadb-plugin-config.cmake + MARIADB_ADD_PLUGIN(
   ... STORAGE_ENGINE ...), to exercise the handlerton/server-headers path
   end to end (MDEV-40608). It does not implement a working engine - every
   handler method just returns HA_ERR_UNSUPPORTED. */

#include <my_global.h>
#include <handler.h>

static handlerton *dummy_se_hton;
static struct st_mysql_storage_engine dummy_se_storage_engine=
{ MYSQL_HANDLERTON_INTERFACE_VERSION };

class ha_dummy_se : public handler
{
public:
  ha_dummy_se(handlerton *hton, TABLE_SHARE *table_arg)
    : handler(hton, table_arg) {}
  ~ha_dummy_se() {}

  const char *table_type() const { return "DUMMY_SE"; }
  ulonglong table_flags() const { return 0; }
  ulong index_flags(uint, uint, bool) const { return 0; }
  uint max_supported_keys() const { return 0; }
  int open(const char *, int, uint) { return HA_ERR_UNSUPPORTED; }
  int close(void) { return 0; }
  int rnd_init(bool) { return HA_ERR_UNSUPPORTED; }
  int rnd_next(uchar *) { return HA_ERR_END_OF_FILE; }
  int rnd_pos(uchar *, uchar *) { return HA_ERR_UNSUPPORTED; }
  void position(const uchar *) {}
  int info(uint) { return 0; }
  int create(const char *, TABLE *, HA_CREATE_INFO *) { return HA_ERR_UNSUPPORTED; }
  int external_lock(THD *, int) { return 0; }
  THR_LOCK_DATA **store_lock(THD *, THR_LOCK_DATA **to, thr_lock_type) { return to; }
};

static handler *dummy_se_create_handler(handlerton *hton, TABLE_SHARE *table,
                                         MEM_ROOT *mem_root)
{
  return new (mem_root) ha_dummy_se(hton, table);
}

static int dummy_se_init(void *p)
{
  handlerton *hton= (handlerton *)p;
  dummy_se_hton= hton;
  hton->create= dummy_se_create_handler;
  return 0;
}

maria_declare_plugin(dummy_storage_engine)
{
  MYSQL_STORAGE_ENGINE_PLUGIN,
  &dummy_se_storage_engine,
  "DUMMY_SE",
  "MariaDB plc",
  "Dummy storage engine exercising MARIADB_ADD_PLUGIN STORAGE_ENGINE (MDEV-40608)",
  PLUGIN_LICENSE_GPL,
  dummy_se_init,
  NULL,
  0x0100,
  NULL,
  NULL,
  "1.0",
  MariaDB_PLUGIN_MATURITY_STABLE
}
maria_declare_plugin_end;
