/* Copyright (c) 2025 Bytedance Ltd. and/or its affiliates

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License, version 2.0,
  as published by the Free Software Foundation.

  This program is also distributed with certain software (including
  but not limited to OpenSSL) that is licensed under separate terms,
  as designated in a particular file or component or in included license
  documentation.  The authors of MySQL hereby grant you an additional
  permission to link the program and your derivative works with the
  separately licensed software that they have included with MySQL.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License, version 2.0, for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA */

#include "my_global.h"                   /* ulonglong */
#include "thr_lock.h"                    /* THR_LOCK, THR_LOCK_DATA */
#include "handler.h"                     /* handler */
#include "my_base.h"                     /* ha_rows */
#include "table.h"
#include <sql_acl.h>
#include <sql_class.h>
#include <my_sys.h>
#include "scope.h"
#include "videx_utils.h"
#include <replication.h>
#include <curl/curl.h>

/** Shared state used by all open VIDEX handlers. */
class videx_share : public Handler_share
{
public:
  mysql_mutex_t mutex;
  THR_LOCK lock;
  videx_share();
  ~videx_share()
  {
    thr_lock_delete(&lock);
    mysql_mutex_destroy(&mutex);
  }
};

/** Storage engine class. */
class ha_videx : public handler
{
  THR_LOCK_DATA lock;
  videx_share *share;
  videx_share *get_share();

public:
  ha_videx(handlerton *hton, TABLE_SHARE *table_arg);
  ~ha_videx() override;

  const char *table_type() const override;

  Table_flags table_flags() const override;

  ulong index_flags(uint idx, uint part, bool all_parts) const override;

  uint max_supported_keys() const override;

  uint max_supported_key_length() const override;

  uint max_supported_key_part_length() const override;

  void column_bitmaps_signal() override;

  int open(const char *name, int mode, uint test_if_locked) override;

  int close(void) override;

  handler *clone(const char *name, MEM_ROOT *mem_root) override;

  int rnd_init(bool scan) override;

  int rnd_next(uchar *buf) override;

  int rnd_pos(uchar *buf, uchar *pos) override;

  void position(const uchar *record) override;

  int info(uint) override;

  THR_LOCK_DATA **store_lock(THD *thd, THR_LOCK_DATA **to,
                             thr_lock_type lock_type) override;

  ha_rows records_in_range(uint inx, const key_range *min_key,
                           const key_range *max_key,
                           page_range *pages) override;

  int create(const char *name, TABLE *table_arg,
             HA_CREATE_INFO *create_info) override;

  int delete_table(const char *name) override;

  int rename_table(const char *from, const char *to) override;

  int multi_range_read_init(RANGE_SEQ_IF *seq, void *seq_init_param,
                            uint n_ranges, uint mode,
                            HANDLER_BUFFER *buf) override;

  int multi_range_read_next(range_id_t *range_info) override;

  ha_rows multi_range_read_info_const(uint keyno, RANGE_SEQ_IF *seq,
                                      void *seq_init_param, uint n_ranges,
                                      uint *bufsz, uint *flags, ha_rows limit,
                                      Cost_estimate *cost) override;

  ha_rows multi_range_read_info(uint keyno, uint n_ranges, uint keys,
                                uint key_parts, uint *bufsz, uint *flags,
                                Cost_estimate *cost) override;

  int multi_range_read_explain_info(uint mrr_mode, char *str,
                                    size_t size) override;

  Item *idx_cond_push(uint keyno, Item *idx_cond) override;

  int info_low(uint flag, bool is_analyze);

  /** The multi range read session object */
  DsMrr_impl m_ds_mrr;

  /** Flags that specificy the handler instance (table) capability. */
  Table_flags m_int_table_flags;

  /** Index into the server's primary key meta-data table->key_info{} */
  uint m_primary_key;

  /** this is set to 1 when we are starting a table scan but have
  not yet fetched any row, else false */
  bool m_start_of_scan;
};

static MYSQL_THDVAR_STR(server_ip, PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_MEMALLOC,
                        "VIDEX server address (host:port)", nullptr, nullptr,
                        "127.0.0.1:5001");

static MYSQL_THDVAR_STR(options, PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_MEMALLOC,
                        "VIDEX connection options (JSON format)", nullptr,
                        nullptr, "{}");

static struct st_mysql_sys_var *videx_system_variables[]= {
    MYSQL_SYSVAR(server_ip),
    MYSQL_SYSVAR(options), NULL};

/**
 * Write callback function for cURL.
 *
 * @param contents Pointer to the received data.
 * @param size Size of each data element.
 * @param nmemb Number of data elements.
 * @param outString Pointer to the string where the data will be appended.
 * @return The total size of the data processed.
 */
size_t write_callback(void *contents, size_t size, size_t nmemb,
                      std::string *outString)
{
  size_t totalSize= size * nmemb;
  outString->append((char *) contents, totalSize);
  return totalSize;
}

/**
 * Sends a request to the Videx HTTP server and validates the response.
 * If the response is successful (code=200), the passed-in &request will be
 * filled.
 *
 * @param request The VidexJsonItem object containing the request data.
 * @param res_json The VidexStringMap object to store the response data.
 * @param thd Pointer to the current thread's THD object.
 * @return 0 if the request is successful, 1 otherwise.
 */
int ask_from_videx_http(VidexJsonItem &request, VidexStringMap &res_json,
                        THD *thd)
{
  const char *host_ip= THDVAR(thd, server_ip);
  if (!host_ip)
  {
    return 1;
  }

  const char *videx_options= THDVAR(thd, options);
  if (!videx_options)
  {
    return 1;
  }

  DBUG_PRINT("info", ("VIDEX OPTIONS: %s IP: %s", videx_options, host_ip));
  request.add_property("videx_options", videx_options);

  std::string url= std::string("http://") + host_ip + "/ask_videx";
  CURL *curl;
  CURLcode res_code;
  std::string readBuffer;

  curl= curl_easy_init();
  if (curl)
  {
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1);

    std::string request_str= request.to_json();
    DBUG_PRINT("info", ("request_str: %s", request_str.c_str()));
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_str.c_str());

    // Set the headers
    struct curl_slist *headers= NULL;
    headers= curl_slist_append(headers, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);

    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    // Disallow connection reuse, so libcurl will close the connection
    // immediately after completing a request.
    curl_easy_setopt(curl, CURLOPT_FORBID_REUSE, 1L);

    curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);

    res_code= curl_easy_perform(curl);
    if (res_code != CURLE_OK)
    {
      sql_print_warning(
          "VIDEX: access videx_server failed res_code != curle_ok: %s",
          host_ip);
      return 1;
    }
    else
    {
      int code;
      std::string message;
      int error=
          videx_parse_simple_json(readBuffer.c_str(), code, message, res_json);
      if (error)
      {
        sql_print_warning("VIDEX: JSON parse error: %s", message.c_str());
        return 1;
      }
      else
      {
        if (message == "OK")
        {
          DBUG_PRINT("info", ("access videx_server success: %s", host_ip));
          return 0;
        }
        else
        {
          sql_print_warning(
              "VIDEX: access videx_server success but msg != OK: %s",
              readBuffer.c_str());
          return 1;
        }
      }
    }
  }
  sql_print_warning("VIDEX: access videx_server failed curl = false: %s",
                    host_ip);
  return 1;
}

/**
 * Sends a request to the Videx HTTP server and aims to return an integer
 * value. If the response is successful (code=200), it extracts the integer
 * value from `response_json["value"]`.
 *
 * @param request The VidexJsonItem object containing the request data.
 * @param result_str Reference to the string where the result value will be
 * stored.
 * @param thd Pointer to the current thread's THD object.
 * @return 0 if the request is successful, 1 otherwise.
 */
int ask_from_videx_http(VidexJsonItem &request, std::string &result_str,
                        THD *thd)
{
  VidexJsonItem result_item;
  VidexStringMap res_json_string_map;
  int error= ask_from_videx_http(request, res_json_string_map, thd);
  if (error)
  {
    return error;
  }
  else if (videx_contains_key(res_json_string_map, "value"))
  {
    result_str= res_json_string_map["value"];
    return error;
  }
  else
  {
    // HTTP request returned successfully, but the result does not contain a
    // "value" field. This indicates an invalid format. Set the error_code to 1
    // and return.
    return 1;
  }
}

static handler *videx_create_handler(handlerton *hton, TABLE_SHARE *table,
                                     MEM_ROOT *mem_root);

handlerton *videx_hton;

static const char *ha_videx_exts[]= {NullS};

#ifdef HAVE_PSI_INTERFACE
static PSI_mutex_key ex_key_mutex_videx_share_mutex;

static PSI_mutex_info all_videx_mutexes[]= {
    {&ex_key_mutex_videx_share_mutex, "videx_share::mutex", 0}};

static void init_videx_psi_keys()
{
  const char *category= "videx";
  int count;

  count= array_elements(all_videx_mutexes);
  mysql_mutex_register(category, all_videx_mutexes, count);
}
#else
static void init_videx_psi_keys() {}
#endif

videx_share::videx_share()
{
  thr_lock_init(&lock);
  mysql_mutex_init(ex_key_mutex_videx_share_mutex, &mutex, MY_MUTEX_INIT_FAST);
}

static void videx_update_optimizer_costs(OPTIMIZER_COSTS *costs)
{
  /*
   * The following values were taken from MariaDB Server 11.8 in the function
   * innobase_update_optimizer_costs(OPTIMIZER_COSTS *costs). See more details
   * in
   * https://github.com/MariaDB/server/blob/11.8/storage/innobase/handler/ha_innodb.cc
   */
  costs->row_next_find_cost= 0.00007013;
  costs->row_lookup_cost= 0.00076597;
  costs->key_next_find_cost= 0.00009900;
  costs->key_lookup_cost= 0.00079112;
  costs->row_copy_cost= 0.00006087;
}

static int videx_init(void *p)
{
  DBUG_ENTER("videx_init");

  init_videx_psi_keys();

  videx_hton= static_cast<handlerton *>(p);

  videx_hton->create= videx_create_handler;
  videx_hton->flags= HTON_SUPPORTS_EXTENDED_KEYS | HTON_SUPPORTS_FOREIGN_KEYS |
                     HTON_NATIVE_SYS_VERSIONING | HTON_WSREP_REPLICATION |
                     HTON_REQUIRES_CLOSE_AFTER_TRUNCATE |
                     HTON_TRUNCATE_REQUIRES_EXCLUSIVE_USE |
                     HTON_REQUIRES_NOTIFY_TABLEDEF_CHANGED_AFTER_COMMIT;

  videx_hton->update_optimizer_costs= videx_update_optimizer_costs;
  videx_hton->tablefile_extensions= ha_videx_exts;

  DBUG_RETURN(0);
}

videx_share *ha_videx::get_share()
{
  videx_share *tmp_share;

  DBUG_ENTER("ha_videx::get_share()");

  lock_shared_ha_data();
  if (!(tmp_share= static_cast<videx_share *>(get_ha_share_ptr())))
  {
    tmp_share= new videx_share;
    if (!tmp_share)
      goto err;

    set_ha_share_ptr(static_cast<Handler_share *>(tmp_share));
  }
err:
  unlock_shared_ha_data();
  DBUG_RETURN(tmp_share);
}

static handler *videx_create_handler(handlerton *hton, TABLE_SHARE *table,
                                     MEM_ROOT *mem_root)
{
  return new (mem_root) ha_videx(hton, table);
}

ha_videx::ha_videx(handlerton *hton, TABLE_SHARE *table_arg)
    : handler(hton, table_arg),
      m_int_table_flags(
          HA_REC_NOT_IN_SEQ | HA_NULL_IN_KEY | HA_CAN_VIRTUAL_COLUMNS |
          HA_CAN_INDEX_BLOBS | HA_CAN_SQL_HANDLER |
          HA_REQUIRES_KEY_COLUMNS_FOR_DELETE |
          HA_PRIMARY_KEY_REQUIRED_FOR_POSITION | HA_PRIMARY_KEY_IN_READ_INDEX |
          HA_BINLOG_ROW_CAPABLE | HA_CAN_GEOMETRY | HA_PARTIAL_COLUMN_READ |
          HA_TABLE_SCAN_ON_INDEX | HA_CAN_EXPORT | HA_ONLINE_ANALYZE |
          HA_CAN_RTREEKEYS |
          HA_CAN_ONLINE_BACKUPS | HA_CONCURRENT_OPTIMIZE | HA_CAN_SKIP_LOCKED),
      m_start_of_scan()
{
}

ha_videx::~ha_videx()= default;

const char *ha_videx::table_type() const { return "VIDEX"; }

handler::Table_flags ha_videx::table_flags() const
{
  THD *thd= ha_thd();
  handler::Table_flags flags= m_int_table_flags;

  if (thd_sql_command(thd) == SQLCOM_CREATE_TABLE)
  {
    flags|= HA_REQUIRE_PRIMARY_KEY;
  }

  if (thd_tx_isolation(thd) <= ISO_READ_COMMITTED)
  {
    return (flags);
  }

  return (flags | HA_BINLOG_STMT_CAPABLE);
}

ulong ha_videx::index_flags(uint key, uint, bool) const
{
  if (table_share->key_info[key].algorithm == HA_KEY_ALG_FULLTEXT)
  {
    return (0);
  }

  /* For spatial index, we don't support descending scan
  and ICP so far. */
  if (table_share->key_info[key].algorithm == HA_KEY_ALG_RTREE)
  {
    return HA_READ_NEXT | HA_READ_ORDER | HA_READ_RANGE | HA_KEYREAD_ONLY |
           HA_KEY_SCAN_NOT_ROR;
  }

  ulong flags= key == table_share->primary_key
                   ? HA_CLUSTERED_INDEX
                   : HA_KEYREAD_ONLY | HA_DO_RANGE_FILTER_PUSHDOWN;

  flags|= HA_READ_NEXT | HA_READ_PREV | HA_READ_ORDER | HA_READ_RANGE |
          HA_DO_INDEX_COND_PUSHDOWN;
  return (flags);
}

uint ha_videx::max_supported_keys() const { return (MAX_KEY); }

uint ha_videx::max_supported_key_length() const
{
  /*
   * This value was taken from MariaDB Server 11.8 in the function
   * innobase_max_supported_key_length() See more details in
   * https://github.com/MariaDB/server/blob/11.8/storage/innobase/handler/ha_innodb.cc
   */
  return (3500);
}

uint ha_videx::max_supported_key_part_length() const
{
  /*
   * This value was taken from MariaDB Server 11.8 in the function
   * innobase_max_supported_key_part_length() See more details in
   * https://github.com/MariaDB/server/blob/11.8/storage/innobase/handler/ha_innodb.cc
   */
  return (3072);
}

void ha_videx::column_bitmaps_signal()
{
  DBUG_ENTER("ha_videx::column_bitmaps_signal");
  // TODO: handle indexed virtual columns for VIDEX engine
  DBUG_VOID_RETURN;
}

/**
        @brief
        Used for opening tables. The name will be the name of the file.

        @details
        A table is opened when it needs to be opened; e.g. when a request comes
   in for a SELECT on the table (tables are not open and closed for each
   request, they are cached).

        Called from handler.cc by handler::ha_open(). The server opens all
   tables by calling ha_open() which then calls the handler specific open().

        @see
        handler::ha_open() in handler.cc
*/

int ha_videx::open(const char *name, int mode, uint test_if_locked)
{
  DBUG_ENTER("ha_videx::open");

  if (!(share= get_share()))
    DBUG_RETURN(1);
  thr_lock_data_init(&share->lock, &lock, NULL);

  m_primary_key= table->s->primary_key;

  if (m_primary_key >= MAX_KEY)
  {
    ref_length= 6; // DATA_ROW_ID_LEN;
  }
  else
  {
    ref_length= table->key_info[m_primary_key].key_length;
  }

  /*
   * This value was taken from MariaDB Server 11.8 in the function open(),
   * where stats.block_size is set to srv_page_size. See more details in
   * https://github.com/MariaDB/server/blob/11.8/storage/innobase/handler/ha_innodb.cc
   */
  stats.block_size= 16384;

  info(HA_STATUS_NO_LOCK | HA_STATUS_VARIABLE | HA_STATUS_CONST |
       HA_STATUS_OPEN);

  DBUG_RETURN(0);
}

/**
        @brief
        Closes a table.

        @details
        Called from sql_base.cc, sql_select.cc, and table.cc. In sql_select.cc
   it is only used to close up temporary tables or during the process where a
        temporary table is converted over to being a myisam table.

        For sql_base.cc look at close_data_tables().

        @see
        sql_base.cc, sql_select.cc and table.cc
*/

int ha_videx::close(void)
{
  DBUG_ENTER("ha_videx::close");
  DBUG_RETURN(0);
}

handler *ha_videx::clone(const char *name,   /*!< in: table name */
                         MEM_ROOT *mem_root) /*!< in: memory context */
{
  DBUG_ENTER("ha_videx::clone");
  DBUG_RETURN(NULL);
}

/**
rnd_init() is called when the system wants the storage engine to do a table
scan. Not required for VIDEX.
*/
int ha_videx::rnd_init(bool scan)
{
  DBUG_ENTER("ha_videx::rnd_init");
  DBUG_RETURN(0);
}

/**
This is called for each row of the table scan. Not required for VIDEX.
*/
int ha_videx::rnd_next(uchar *buf)
{
  int rc;
  DBUG_ENTER("ha_videx::rnd_next");
  rc= HA_ERR_END_OF_FILE;
  DBUG_RETURN(rc);
}

/**
This is like rnd_next, but you are given a position to use
to determine the row. Not required for VIDEX.
*/
int ha_videx::rnd_pos(uchar *buf, uchar *pos)
{
  int rc;
  DBUG_ENTER("ha_videx::rnd_pos");
  rc= HA_ERR_WRONG_COMMAND;
  DBUG_RETURN(rc);
}

/**
position() is called after each call to rnd_next() if the data needs
to be ordered. Not required for VIDEX.
*/
void ha_videx::position(const uchar *record)
{
  DBUG_ENTER("ha_videx::position");
  DBUG_VOID_RETURN;
}

/**
Returns table statistics to the server; fills fields in the handler object.
@return 0 on success or HA_ERR_*.
*/

int ha_videx::info(uint flag) { return (info_low(flag, false)); }

/**
Converts a MySQL table lock in 'lock' to the engine representation.
Not required for VIDEX.
@return pointer to the current element in 'to'.
*/

THR_LOCK_DATA **ha_videx::store_lock(THD *thd, THR_LOCK_DATA **to,
                                     enum thr_lock_type lock_type)
{
  if (lock_type != TL_IGNORE && lock.type == TL_UNLOCK)
    lock.type= lock_type;
  *to++= &lock;
  return to;
}

/**
 * Estimates the number of index records in a specific range via VIDEX HTTP.
 * Working principle: VIDEX will forward requests via HTTP to the external
 * VIDEX-Statistic-Server (launched in a RESTful manner). If the request fails,
 * a default value of 10 will be returned. Reference link: For an
 * implementation of VIDEX-Statistic-Server, see
 * https://github.com/bytedance/videx. We plan to introduce it into the
 * official MariaDB repository in a subsequent PR.
 * @param keynr: The index number of the key
 * @param min_key: The minimum key value of the range
 * @param max_key: The maximum key value of the range
 * @param pages: Page range information (present as a parameter but not used in
 * the function)
 * @return estimated number of rows.
 */

ha_rows ha_videx::records_in_range(uint keynr, const key_range *min_key,
                                   const key_range *max_key, page_range *pages)
{
  DBUG_ENTER("ha_videx::records_in_range");
  KEY *key;
  active_index= keynr;
  key= table->key_info + active_index;

  VidexJsonItem request_item= construct_request(
      table->s->db.str, table->s->table_name.str, __PRETTY_FUNCTION__);
  serializeKeyRangeToJson(min_key, max_key, key, &request_item);

  std::string n_rows_str;
  ha_rows n_rows;
  THD *thd= ha_thd();
  int error= ask_from_videx_http(request_item, n_rows_str, thd);
  if (error)
  {
    n_rows= 10; // default number to force index
  }
  else
  {
    n_rows= std::stoull(n_rows_str);
  }

  // Avoid returning 0 to the optimizer, similar to InnoDB's behavior
  if (n_rows == 0)
  {
    n_rows= 1;
  }

  DBUG_RETURN(n_rows);
}

/**
Create a new table to an VIDEX database. Not required for VIDEX. */

int ha_videx::create(const char *name, TABLE *table_arg,
                     HA_CREATE_INFO *create_info)
{
  DBUG_ENTER("ha_videx::create");
  DBUG_PRINT("info", ("name: %s, table_arg: %p, create_info: %p", name,
                      table_arg, create_info));
  DBUG_RETURN(0);
}

int ha_videx::delete_table(const char *name)
{
  DBUG_ENTER("ha_videx::delete_table");
  DBUG_RETURN(0);
}

int ha_videx::rename_table(const char *from, const char *to)
{
  DBUG_ENTER("ha_videx::rename_table");
  DBUG_RETURN(0);
}

/**
DS-MRR implementation.
*/

int ha_videx::multi_range_read_init(RANGE_SEQ_IF *seq, void *seq_init_param,
                                    uint n_ranges, uint mode,
                                    HANDLER_BUFFER *buf)
{
  return (m_ds_mrr.dsmrr_init(this, seq, seq_init_param, n_ranges, mode, buf));
}

int ha_videx::multi_range_read_next(range_id_t *range_info)
{
  return (m_ds_mrr.dsmrr_next(range_info));
}

ha_rows ha_videx::multi_range_read_info_const(uint keyno, RANGE_SEQ_IF *seq,
                                              void *seq_init_param,
                                              uint n_ranges, uint *bufsz,
                                              uint *flags, ha_rows limit,
                                              Cost_estimate *cost)
{
  m_ds_mrr.init(this, table);

  return (m_ds_mrr.dsmrr_info_const(keyno, seq, seq_init_param, n_ranges,
                                    bufsz, flags, limit, cost));
}

ha_rows ha_videx::multi_range_read_info(uint keyno, uint n_ranges, uint keys,
                                        uint key_parts, uint *bufsz,
                                        uint *flags, Cost_estimate *cost)
{
  m_ds_mrr.init(this, table);
  return (m_ds_mrr.dsmrr_info(keyno, n_ranges, keys, key_parts, bufsz, flags,
                              cost));
}

int ha_videx::multi_range_read_explain_info(uint mrr_mode, char *str,
                                            size_t size)
{
  return (m_ds_mrr.dsmrr_explain_info(mrr_mode, str, size));
}

/**
 * Attempt to push down an index condition.
 * @param keyno MySQL key number
 * @param idx_cond Index condition to be checked
 * @return Part of idx_cond which the handler will not evaluate
 */

class Item *ha_videx::idx_cond_push(uint keyno, class Item *idx_cond)
{
  DBUG_ENTER("ha_videx::idx_cond_push");
  DBUG_ASSERT(keyno != MAX_KEY);
  DBUG_ASSERT(idx_cond != NULL);

  pushed_idx_cond= idx_cond;
  pushed_idx_cond_keyno= keyno;
  in_range_check_pushed_down= TRUE;
  DBUG_RETURN(NULL);
}

/**
 * A very important function. When a query arrives, MariaDB calls this function
 * to initialize the information of a table. In a session, this function is
 * only called once by the MariaDB query optimizer. VIDEX requests the
 * `videx_stats_server` to return various statistics of a single table,
 * including: stat_n_rows: The number of rows in the table.
 *  stat_clustered_index_size: The size of the clustered index.
 *  stat_sum_of_other_index_sizes: The sum of sizes of other indexes.
 *  data_file_length: The size of the data file.
 *  index_file_length: The length of the index file.
 *  data_free_length: The length of free space in the data file.
 *
 *  [Very Important]
 *  rec_per_key of several columns: require NDV algorithm
 *
 * Returns statistics information of the table to the MariaDB interpreter, in
 * various fields of the handle object.
 * @param[in]      flag            what information is requested
 * @param[in]      is_analyze      True if called from "::analyze()".
 * @return HA_ERR_* error code or 0
 */

int ha_videx::info_low(uint flag, bool is_analyze)
{
  uint64_t n_rows;

  DBUG_ENTER("ha_videx::info_low");
  DEBUG_SYNC_C("ha_videx_info_low");

  // construct request
  VidexStringMap res_json;
  VidexJsonItem request_item= construct_request(
      table->s->db.str, table->s->table_name.str, __PRETTY_FUNCTION__);
  for (uint i= 0; i < table->s->keys; i++)
  {
    KEY *key= &table->key_info[i];
    VidexJsonItem *keyItem= request_item.create("key");
    keyItem->add_property("name", key->name.str);
    keyItem->add_property_nonan("key_length", key->key_length);
    for (ulong j= 0; j < key->usable_key_parts; j++)
    {
      if ((key->flags & HA_KEY_ALG_FULLTEXT) ||
          (key->flags & HA_SPATIAL_legacy))
      {
        continue;
      }
      VidexJsonItem *field= keyItem->create("field");
      field->add_property("name", key->key_part[j].field->field_name.str);
      field->add_property_nonan("store_length", key->key_part[j].store_length);
    }
  }
  DBUG_PRINT("info", ("Request JSON: %s", request_item.to_json().c_str()));

  THD *thd= ha_thd();
  int error= ask_from_videx_http(request_item, res_json, thd);
  if (error)
  {
    DBUG_RETURN(0);
  }
  else
  {
    // validate the returned json
    // stat_n_rows, stat_clustered_index_size, stat_sum_of_other_index_sizes,
    // data_file_length, index_file_length, data_free_length
    if (!(videx_contains_key(res_json, "stat_n_rows") &&
          videx_contains_key(res_json, "stat_clustered_index_size") &&
          videx_contains_key(res_json, "stat_sum_of_other_index_sizes") &&
          videx_contains_key(res_json, "data_file_length") &&
          videx_contains_key(res_json, "index_file_length") &&
          videx_contains_key(res_json, "data_free_length")))
    {
      sql_print_warning("VIDEX: res_json data error=0 but miss some key.");
      DBUG_RETURN(0);
    }
  }

  if (flag & HA_STATUS_VARIABLE)
  {
    n_rows= std::stoull(res_json["stat_n_rows"]);
    if (n_rows == 0 && !(flag & (HA_STATUS_TIME | HA_STATUS_OPEN)))
    {
      n_rows++;
    }

    stats.records= (ha_rows) n_rows;
    stats.deleted= 0;

    stats.data_file_length= std::stoull(res_json["data_file_length"]);
    stats.index_file_length= std::stoull(res_json["index_file_length"]);
    if (flag & HA_STATUS_VARIABLE_EXTRA)
    {
      stats.delete_length= std::stoull(res_json["data_free_length"]);
    }
    stats.check_time= 0;
    stats.mrr_length_per_rec=
        (uint) ref_length + 8; // 8 = max(sizeof(void *));

    if (stats.records == 0)
    {
      stats.mean_rec_length= 0;
    }
    else
    {
      stats.mean_rec_length= (ulong) (stats.data_file_length / stats.records);
    }
  }

  if (flag & HA_STATUS_CONST)
  {
    for (uint i= 0; i < table->s->keys; i++)
    {
      ulong j;
      KEY *key= &table->key_info[i];

      for (j= 0; j < key->ext_key_parts; j++)
      {

        if ((key->algorithm == HA_KEY_ALG_FULLTEXT) ||
            (key->algorithm == HA_KEY_ALG_RTREE))
        {
          continue;
        }

        // The #@# separator combines the index name and field name, making it
        // easier to extract the corresponding statistical values from the JSON
        // response.
        std::string concat_key=
            "rec_per_key #@# " + std::string(key->name.str) + " #@# " +
            std::string(key->key_part[j].field->field_name.str);
        ulong rec_per_key_int= 0;
        if (videx_contains_key(res_json, concat_key.c_str()))
        {
          rec_per_key_int= std::stoul(res_json[concat_key.c_str()]);
        }
        else
        {
          rec_per_key_int= stats.records;
        }
        if (rec_per_key_int == 0)
        {
          rec_per_key_int= 1;
        }
        key->rec_per_key[j]= rec_per_key_int;
      }
    }
  }
  DBUG_RETURN(0);
}

struct st_mysql_storage_engine videx_storage_engine= {
    MYSQL_HANDLERTON_INTERFACE_VERSION};

maria_declare_plugin(videx){
    MYSQL_STORAGE_ENGINE_PLUGIN,
    &videx_storage_engine,
    "VIDEX",
    "Rong Kang, Haibo Yang",
    "Disaggregated, Extensible Virtual Index Engine for What-If Analysis",
    PLUGIN_LICENSE_GPL,
    videx_init,                          /* Plugin Init */
    NULL,                                /* Plugin Deinit */
    0x0001,                              /* version number (0.1) */
    NULL,                   /* status variables */
    videx_system_variables,              /* system variables */
    "0.1",                               /* string version */
    MariaDB_PLUGIN_MATURITY_EXPERIMENTAL /* maturity */
} maria_declare_plugin_end;
