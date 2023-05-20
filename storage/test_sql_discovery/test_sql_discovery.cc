/*
   Copyright (c) 2013 Monty Program Ab

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; version 2 of
   the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA
*/

/*
  a really minimal engine to test table discovery via sql statements.
  See the archive engine if you're interested in real-life usable engine that
  uses discovery via frm shipping.
*/

#include <my_global.h>
#include <mysql_version.h>
#include <handler.h>
#include <table.h>

static MYSQL_THDVAR_STR(statement, PLUGIN_VAR_MEMALLOC,
  "The table name and the SQL statement to discover the next table",
  NULL, NULL, 0);

static MYSQL_THDVAR_BOOL(write_frm, 0,
  "Whether to cache discovered table metadata in frm files",
  NULL, NULL, TRUE);

static struct st_mysql_sys_var *sysvars[] = {
  MYSQL_SYSVAR(statement),
  MYSQL_SYSVAR(write_frm),
  NULL
};

class TSD_share : public Handler_share {
public:
  THR_LOCK lock;
  TSD_share()
  {
    thr_lock_init(&lock);
  }
  ~TSD_share()
  {
    thr_lock_delete(&lock);
  }
};

class ha_tsd: public handler
{
private:
  THR_LOCK_DATA lock;
  TSD_share *share;
  TSD_share *get_share();

public:
  ha_tsd(handlerton *hton, TABLE_SHARE *table_arg)
    : handler(hton, table_arg) { }
  ulonglong table_flags() const
  { // NO_TRANSACTIONS and everything that affects CREATE TABLE
    return HA_NO_TRANSACTIONS | HA_CAN_GEOMETRY | HA_NULL_IN_KEY |
           HA_CAN_INDEX_BLOBS | HA_AUTO_PART_KEY | HA_CAN_RTREEKEYS |
           HA_CAN_FULLTEXT;
  }

  ulong index_flags(uint inx, uint part, bool all_parts) const { return 0; }

  THR_LOCK_DATA **store_lock(THD *thd, THR_LOCK_DATA **to,
                             enum thr_lock_type lock_type)
  {
    if (lock_type != TL_IGNORE && lock.type == TL_UNLOCK)
      lock.type = lock_type;
    *to ++= &lock;
    return to;
  }

  int rnd_init(bool scan) { return 0; }
  int rnd_next(unsigned char *buf) { return HA_ERR_END_OF_FILE; }
  void position(const uchar *record) { }
  int rnd_pos(uchar *buf, uchar *pos) { return HA_ERR_END_OF_FILE; }
  int info(uint flag) { return 0; }
  uint max_supported_keys() const { return 16; }
  int create(const char *name, TABLE *table_arg,
             HA_CREATE_INFO *create_info) { return HA_ERR_WRONG_COMMAND; }

  int open(const char *name, int mode, uint test_if_locked);
  int close(void);
};

TSD_share *ha_tsd::get_share()
{
  TSD_share *tmp_share;
  lock_shared_ha_data();
  if (!(tmp_share= static_cast<TSD_share*>(get_ha_share_ptr())))
  {
    tmp_share= new TSD_share;
    if (!tmp_share)
      goto err;

    set_ha_share_ptr(static_cast<Handler_share*>(tmp_share));
  }
err:
  unlock_shared_ha_data();
  return tmp_share;
}

int ha_tsd::open(const char *name, int mode, uint test_if_locked)
{
  if (!(share= get_share()))
    return HA_ERR_OUT_OF_MEM;

  thr_lock_data_init(&share->lock,&lock,NULL);
  return 0;
}

int ha_tsd::close(void)
{
  return 0;
}

static handler *create_handler(handlerton *hton, TABLE_SHARE *table,
                               MEM_ROOT *mem_root)
{
  return new (mem_root) ha_tsd(hton, table);
}

static int discover_table(handlerton *hton, THD* thd, TABLE_SHARE *share)
{
  const char *sql= THDVAR(thd, statement);

  // the table is discovered if sql starts from "table_name:"
  if (!sql ||
      strncmp(sql, share->table_name.str, share->table_name.length) ||
      sql[share->table_name.length] != ':')
    return HA_ERR_NO_SUCH_TABLE;

  sql+= share->table_name.length + 1;
  return share->init_from_sql_statement_string(thd, THDVAR(thd, write_frm),
                                               sql, strlen(sql));
}

static int drop_table(handlerton *hton, const char *path)
{
  const char *name= strrchr(path, FN_LIBCHAR)+1;
  const char *sql= THDVAR(current_thd, statement);
  return !sql || strncmp(sql, name, strlen(name)) || sql[strlen(name)] != ':'
    ? ENOENT : 0;
}

static int init(void *p)
{
  handlerton *hton = (handlerton *)p;
  hton->create = create_handler;
  hton->discover_table = discover_table;
  hton->drop_table= drop_table;
  return 0;
}

struct st_mysql_storage_engine descriptor =
{ MYSQL_HANDLERTON_INTERFACE_VERSION };

maria_declare_plugin(test_sql_discovery)
{
  MYSQL_STORAGE_ENGINE_PLUGIN,
  &descriptor,
  "TEST_SQL_DISCOVERY",
  "Sergei Golubchik",
  "Minimal engine to test table discovery via sql statements",
  PLUGIN_LICENSE_GPL,
  init,
  NULL,
  0x0001,
  NULL,
  sysvars,
  "0.1",
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL
}
maria_declare_plugin_end;

