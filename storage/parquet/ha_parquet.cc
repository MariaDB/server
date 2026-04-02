#define MYSQL_SERVER 1

#include "ha_parquet.h"
#include "sql_class.h"
#include "handler.h"
#include <curl/curl.h>

handlerton *parquet_hton= 0;
static THR_LOCK parquet_lock;

ha_parquet::ha_parquet(handlerton *hton, TABLE_SHARE *table_arg)
  : handler(hton, table_arg)
{
  thr_lock_data_init(&parquet_lock, &lock, NULL);
}

ulonglong ha_parquet::table_flags() const
{
  return HA_NO_TRANSACTIONS | HA_FILE_BASED;
}

ulong ha_parquet::index_flags(uint, uint, bool) const
{
  return 0;
}

int ha_parquet::open(const char *, int, uint)
{
  return 0;
}

int ha_parquet::close(void)
{
  return 0;
}

int ha_parquet::create(const char *, TABLE *, HA_CREATE_INFO *)
{
  return 0;
}

int ha_parquet::write_row(const uchar *buf)
{
  return HA_ERR_WRONG_COMMAND;
}

int ha_parquet::update_row(const uchar *old_data, const uchar *new_data)
{
  return HA_ERR_WRONG_COMMAND;
}

int ha_parquet::delete_row(const uchar *buf)
{
  return HA_ERR_WRONG_COMMAND;
}

int ha_parquet::rnd_init(bool)
{
  return 0;
}

int ha_parquet::rnd_next(uchar *)
{
  return HA_ERR_END_OF_FILE;
}

int ha_parquet::rnd_pos(uchar *, uchar *)
{
  return HA_ERR_WRONG_COMMAND;
}

void ha_parquet::position(const uchar *)
{
}

int ha_parquet::info(uint)
{
  return 0;
}

enum_alter_inplace_result
ha_parquet::check_if_supported_inplace_alter(TABLE *,
                                             Alter_inplace_info *)
{
  return HA_ALTER_INPLACE_NOT_SUPPORTED;
}

int ha_parquet::ha_parquet_external_lock(THD *thd, int lock_type)
{
  int error;
  DBUG_ENTER("handler::ha_external_lock");
  /*
    Whether this is lock or unlock, this should be true, and is to verify that
    if get_auto_increment() was called (thus may have reserved intervals or
    taken a table lock), ha_release_auto_increment() was too.
  */
  DBUG_ASSERT(next_insert_id == 0);
  /* Consecutive calls for lock without unlocking in between is not allowed */
  DBUG_ASSERT(table_share->tmp_table != NO_TMP_TABLE ||
              ((lock_type != F_UNLCK && m_lock_type == F_UNLCK) ||
               lock_type == F_UNLCK));
  /* SQL HANDLER call locks/unlock while scanning (RND/INDEX). */
  DBUG_ASSERT(inited == NONE || table->open_by_handler);

  if (MYSQL_HANDLER_RDLOCK_START_ENABLED() ||
      MYSQL_HANDLER_WRLOCK_START_ENABLED() ||
      MYSQL_HANDLER_UNLOCK_START_ENABLED())
  {
    if (lock_type == F_RDLCK)
    {
      MYSQL_HANDLER_RDLOCK_START(table_share->db.str,
                                 table_share->table_name.str);
    }
    else if (lock_type == F_WRLCK)
    {
      MYSQL_HANDLER_WRLOCK_START(table_share->db.str,
                                 table_share->table_name.str);
    }
    else if (lock_type == F_UNLCK)
    {
      MYSQL_HANDLER_UNLOCK_START(table_share->db.str,
                                 table_share->table_name.str);
    }
  }

  if (lock_type == F_UNLCK)
    (void) table->unlock_hlindexes();

  /*
    We cache the table flags if the locking succeeded. Otherwise, we
    keep them as they were when they were fetched in ha_open().
  */
  MYSQL_TABLE_LOCK_WAIT(PSI_TABLE_EXTERNAL_LOCK, lock_type,
    { error= external_lock(thd, lock_type); })

  DBUG_EXECUTE_IF("external_lock_failure", error= HA_ERR_GENERIC;);

  if (likely(error == 0 || lock_type == F_UNLCK))
  {
    m_lock_type= lock_type;
    cached_table_flags= table_flags();
    if (table_share->tmp_table == NO_TMP_TABLE)
      mysql_audit_external_lock(thd, table_share, lock_type);
  }

  if (MYSQL_HANDLER_RDLOCK_DONE_ENABLED() ||
      MYSQL_HANDLER_WRLOCK_DONE_ENABLED() ||
      MYSQL_HANDLER_UNLOCK_DONE_ENABLED())
  {
    if (lock_type == F_RDLCK)
    {
      MYSQL_HANDLER_RDLOCK_DONE(error);
    }
    else if (lock_type == F_WRLCK)
    {
      MYSQL_HANDLER_WRLOCK_DONE(error);
    }
    else if (lock_type == F_UNLCK)
    {
      MYSQL_HANDLER_UNLOCK_DONE(error);
    }
  }
  DBUG_RETURN(error);
}

THR_LOCK_DATA **ha_parquet::store_lock(THD *thd,
                                       THR_LOCK_DATA **to,
                                       enum thr_lock_type lock_type)
{
  if (lock_type != TL_IGNORE && lock.type == TL_UNLOCK){
    lock.type= lock_type;
  }
  
  *to++ = &lock;
  return to;
}

static handler *parquet_create_handler(handlerton *p_hton,
                                  TABLE_SHARE * table,
                                  MEM_ROOT *mem_root)
{
  return new (mem_root) ha_parquet(p_hton, table);
}

static int ha_parquet_commit(handlerton *hton, THD *thd, bool all)
{
  if (!all) return 0; // We only care about full transaction commits

  // PLACEHOLDER: these will come from THD state once handler team implements close() -> writes to s3
  const char *lakekeeper_url = "http://placeholder-lakekeeper:8080/v1/namespaces/default/tables/placeholder_table/snapshots";
  const char *s3_file_path   = "s3://placeholder-bucket/placeholder.parquet";
  int         row_count      = 0;  // PLACEHOLDER: real row count from DuckDB
  int         file_size      = 0;  // PLACEHOLDER: real file size from S3 upload

  // Building the JSON with the s3 file path, row count, and file size data
  char json_body[1024];
  snprintf(json_body, sizeof(json_body),
    "{"
      "\"data-files\": [{"
        "\"file-path\": \"%s\","
        "\"file-format\": \"PARQUET\","
        "\"record-count\": %d,"
        "\"file-size-in-bytes\": %d"
      "}]"
    "}",
    s3_file_path, row_count, file_size
  );

  // For making the HTTP request
  CURL *curl = curl_easy_init();
  if (!curl) return 1;

  // Let lakekeeper know we're sending a JSON (headers)
  struct curl_slist *headers = NULL;
  headers = curl_slist_append(headers, "Content-Type: application/json");

  // Sets where to send the request (Lakekeeper endpoint), the JSON to send, and the headers
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_URL, lakekeeper_url);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

  // Sends the HTTP POST request to LakeKeeper. The response code is stored in res
  CURLcode res = curl_easy_perform(curl);

  // Free memory
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  // If it fails, return 1
  if (res != CURLE_OK) return 1;

  return 0;
}

static int ha_parquet_rollback(handlerton *hton, THD *thd, bool all)
{
  if (!all) return 0;

  // PLACEHOLDER: real S3 file path will come from THD state once handler team implements close()
  // const char *s3_file_path = "s3://placeholder-bucket/placeholder.parquet";

  // TODO: once DuckDB is integrated, use DuckDB to delete the orphaned S3 file
  // DuckDB handles S3 authentication so we don't need to manage credentials here
  // Something like: duckdb_connection.Query("DELETE FROM '" + s3_file_path + "'");

  return 0;

}

static int ha_parquet_init(void *p)
{
    parquet_hton = (handlerton *) p;
    parquet_hton->create = parquet_create_handler;
    parquet_hton->commit   = ha_parquet_commit;
    parquet_hton->rollback = ha_parquet_rollback;
    thr_lock_init(&parquet_lock);
    return 0;
}

static int ha_parquet_deinit(void *p)
{
  parquet_hton = 0;
  thr_lock_delete(&parquet_lock);
  return 0;
}

struct st_mysql_storage_engine parquet_storage_engine=
{ MYSQL_HANDLERTON_INTERFACE_VERSION };

maria_declare_plugin(parquet)
{
  MYSQL_STORAGE_ENGINE_PLUGIN,
  &parquet_storage_engine,
  "PARQUET",
  "UIUC Disruption Lab",
  "Parquet Storage Engine ",
  PLUGIN_LICENSE_GPL,
  ha_parquet_init,                   /* Plugin Init      */
  ha_parquet_deinit,                 /* Plugin Deinit    */
  0x0100,                            /* 1.0              */
  NULL,                              /* status variables */
  NULL,                               /* system variables */
  "1.0",                        /* string version   */
  MariaDB_PLUGIN_MATURITY_STABLE/* maturity         */
}
maria_declare_plugin_end;
