/* Copyright (c) 2024 Bytedance Ltd. and/or its affiliates

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
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "ha_videx.h"

/**
 * Write callback function for cURL.
 *
 * @param contents Pointer to the received data.
 * @param size Size of each data element.
 * @param nmemb Number of data elements.
 * @param outString Pointer to the string where the data will be appended.
 * @return The total size of the data processed.
 */
size_t write_callback(void *contents, size_t size, size_t nmemb, std::string *outString) {
    size_t totalSize = size * nmemb;
    outString->append((char *) contents, totalSize);
    return totalSize;
}

/**
 * Sends a request to the Videx HTTP server and validates the response.
 * If the response is successful (code=200), the passed-in &request will be filled.
 *
 * @param request The VidexJsonItem object containing the request data.
 * @param res_json The VidexStringMap object to store the response data.
 * @param thd Pointer to the current thread's THD object.
 * @return 0 if the request is successful, 1 otherwise.
 */
int ask_from_videx_http(VidexJsonItem &request, VidexStringMap &res_json, THD* thd) {
  // For videx performance testing. When DEBUG_SKIP_HTTP is enabled,
  // it tests the videx execution performance without network access, but directly return a mocked value.
  char debug_skip_http[100];  // Buffer to hold the value of the user variable
  int is_null;
  if (get_user_var_str("DEBUG_SKIP_HTTP", debug_skip_http, sizeof(debug_skip_http), 0, &is_null) == 0){
    // For performance testing. If set to the string "True", it skips the HTTP request.
    if (strcmp(debug_skip_http, "True") == 0) {
        return 1;
    }
  }
    Opt_trace_context *const trace = &thd->opt_trace;
    // For MySQL trace, if the key name is not specified, it will be automatically assigned as "unknown_key_xxx".
    Opt_trace_object trace_http(trace);
    trace_http.add_alnum("dict_name", "videx_http");

  // Read the server address and change the host IP.
  const char *host_ip = "127.0.0.1:5001";
  char value[1000];  // Buffer to hold the value of the user variable
  
  if (get_user_var_str("VIDEX_SERVER", value, sizeof(value), 0, &is_null) == 0)
    host_ip = value;
  const char *videx_options = "{}";
  char option_value[1000];
  if (get_user_var_str("VIDEX_OPTIONS", option_value, sizeof(option_value), 0, &is_null) == 0)
    videx_options = option_value;
  std::cout << "VIDEX OPTIONS: " << videx_options << " IP: " << host_ip << std::endl;
  request.add_property("videx_options", videx_options);

  std::string url = std::string("http://") + host_ip + "/ask_videx";
  CURL *curl;
  CURLcode res_code;
  std::string readBuffer;

  trace_http.add_utf8("url", url.c_str());
  curl = curl_easy_init(); // 初始化一个CURL easy handle。
  if(curl) {
      curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
      curl_easy_setopt(curl, CURLOPT_POST, 1);


      std::string request_str = request.to_json();
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_str.c_str());

      // Set the headers
      struct curl_slist *headers = NULL;
      headers = curl_slist_append(headers, "Content-Type: application/json");
      curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
      curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);

      // Set the connection timeout to 10 seconds.
      curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
      // Set the overall request timeout to 30 seconds.
      curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

      // Disallow connection reuse, so libcurl will close the connection immediately after completing a request.
      curl_easy_setopt(curl, CURLOPT_FORBID_REUSE, 1L);

      trace_http.add_utf8("request", request_str.c_str());
      res_code = curl_easy_perform(curl);
      if (res_code != CURLE_OK) {
        trace_http.add("success", false)
            .add_utf8("reason", "res_code != CURLE_OK")
            .add_utf8("detail", curl_easy_strerror(res_code));
        trace_http.end();
        std::cout << "access videx_server failed res_code != curle_ok: " << host_ip << std::endl;
          fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res_code));
          return 1;
      } else {
          int code;
          std::string message;
          int error = videx_parse_simple_json(readBuffer.c_str(), code, message, res_json);
          if (error) {
              std::cout << "!__!__!__!__!__! JSON parse error: " << message << '\n';
              trace_http.add("success", false)
                  .add_utf8("reason", "res_json.HasParseError")
                  .add_utf8("detail", readBuffer.c_str());
              trace_http.end();
              return 1;
          } else {
              if (message == "OK") {
                  trace_http.add("success", true).add_utf8("detail", readBuffer.c_str());
                  trace_http.end();
                  std::cout << "access videx_server success: " << host_ip << std::endl;
                  return 0;
              } else {
                  trace_http.add("success", false)
                      .add_utf8("reason", "msg != OK")
                      .add_utf8("detail", readBuffer.c_str());
                  trace_http.end();
                  std::cout << "access videx_server success but msg != OK: " << readBuffer.c_str() << std::endl;
                  return 1;
              }
          }
      }
  }
  trace_http.add("success", false)
      .add_utf8("reason", "curl = false");
  trace_http.end();
  std::cout << "access videx_server failed curl = false: " << host_ip << std::endl;
  return 1;
}

/**
 * Sends a request to the Videx HTTP server and aims to return an integer value.
 * If the response is successful (code=200), it extracts the integer value from `response_json["value"]`.
 *
 * @param request The VidexJsonItem object containing the request data.
 * @param result_str Reference to the string where the result value will be stored.
 * @param thd Pointer to the current thread's THD object.
 * @return 0 if the request is successful, 1 otherwise.
 */
int ask_from_videx_http(VidexJsonItem &request,
                                      std::string& result_str, THD * thd){
    VidexJsonItem result_item;
    VidexStringMap res_json_string_map;
    int error = ask_from_videx_http(request, res_json_string_map, thd);
    if (error){
      return error;
    }
    else if (videx_contains_key(res_json_string_map, "value")){
      // For std::string, using the assignment operator = automatically replaces the entire contents of the string.
      // There is no need to manually clear the string beforehand.
      result_str = res_json_string_map["value"];
      return error;
    } else{
      // HTTP request returned successfully, but the result does not contain a "value" field.
      // This indicates an invalid format. Set the error_code to 1 and return.
      return 1; // Unprocessable Entity
    }
}

std::string extraFuncToString(ha_extra_function extraFunc) {
    switch (extraFunc) {
        case HA_EXTRA_NORMAL: return "HA_EXTRA_NORMAL";
        case HA_EXTRA_QUICK: return "HA_EXTRA_QUICK";
        case HA_EXTRA_NOT_USED: return "HA_EXTRA_NOT_USED";
        case HA_EXTRA_NO_READCHECK: return "HA_EXTRA_NO_READCHECK";
        case HA_EXTRA_READCHECK: return "HA_EXTRA_READCHECK";
        case HA_EXTRA_KEYREAD: return "HA_EXTRA_KEYREAD";
        case HA_EXTRA_NO_KEYREAD: return "HA_EXTRA_NO_KEYREAD";
        case HA_EXTRA_NO_USER_CHANGE: return "HA_EXTRA_NO_USER_CHANGE";
        case HA_EXTRA_WAIT_LOCK: return "HA_EXTRA_WAIT_LOCK";
        case HA_EXTRA_NO_WAIT_LOCK: return "HA_EXTRA_NO_WAIT_LOCK";
        case HA_EXTRA_NO_KEYS: return "HA_EXTRA_NO_KEYS";
        case HA_EXTRA_KEYREAD_CHANGE_POS: return "HA_EXTRA_KEYREAD_CHANGE_POS";
        case HA_EXTRA_REMEMBER_POS: return "HA_EXTRA_REMEMBER_POS";
        case HA_EXTRA_RESTORE_POS: return "HA_EXTRA_RESTORE_POS";
        case HA_EXTRA_FORCE_REOPEN: return "HA_EXTRA_FORCE_REOPEN";
        case HA_EXTRA_FLUSH: return "HA_EXTRA_FLUSH";
        case HA_EXTRA_NO_ROWS: return "HA_EXTRA_NO_ROWS";
        case HA_EXTRA_RESET_STATE: return "HA_EXTRA_RESET_STATE";
        case HA_EXTRA_IGNORE_DUP_KEY: return "HA_EXTRA_IGNORE_DUP_KEY";
        case HA_EXTRA_NO_IGNORE_DUP_KEY: return "HA_EXTRA_NO_IGNORE_DUP_KEY";
        case HA_EXTRA_PREPARE_FOR_DROP: return "HA_EXTRA_PREPARE_FOR_DROP";
        case HA_EXTRA_PREPARE_FOR_UPDATE: return "HA_EXTRA_PREPARE_FOR_UPDATE";
        case HA_EXTRA_PRELOAD_BUFFER_SIZE: return "HA_EXTRA_PRELOAD_BUFFER_SIZE";
        case HA_EXTRA_CHANGE_KEY_TO_UNIQUE: return "HA_EXTRA_CHANGE_KEY_TO_UNIQUE";
        case HA_EXTRA_CHANGE_KEY_TO_DUP: return "HA_EXTRA_CHANGE_KEY_TO_DUP";
        case HA_EXTRA_KEYREAD_PRESERVE_FIELDS: return "HA_EXTRA_KEYREAD_PRESERVE_FIELDS";
        case HA_EXTRA_IGNORE_NO_KEY: return "HA_EXTRA_IGNORE_NO_KEY";
        case HA_EXTRA_NO_IGNORE_NO_KEY: return "HA_EXTRA_NO_IGNORE_NO_KEY";
        case HA_EXTRA_MARK_AS_LOG_TABLE: return "HA_EXTRA_MARK_AS_LOG_TABLE";
        case HA_EXTRA_WRITE_CAN_REPLACE: return "HA_EXTRA_WRITE_CAN_REPLACE";
        case HA_EXTRA_WRITE_CANNOT_REPLACE: return "HA_EXTRA_WRITE_CANNOT_REPLACE";
        case HA_EXTRA_DELETE_CANNOT_BATCH: return "HA_EXTRA_DELETE_CANNOT_BATCH";
        case HA_EXTRA_UPDATE_CANNOT_BATCH: return "HA_EXTRA_UPDATE_CANNOT_BATCH";
        case HA_EXTRA_INSERT_WITH_UPDATE: return "HA_EXTRA_INSERT_WITH_UPDATE";
        case HA_EXTRA_PREPARE_FOR_RENAME: return "HA_EXTRA_PREPARE_FOR_RENAME";
        case HA_EXTRA_ADD_CHILDREN_LIST: return "HA_EXTRA_ADD_CHILDREN_LIST";
        case HA_EXTRA_ATTACH_CHILDREN: return "HA_EXTRA_ATTACH_CHILDREN";
        case HA_EXTRA_IS_ATTACHED_CHILDREN: return "HA_EXTRA_IS_ATTACHED_CHILDREN";
        case HA_EXTRA_DETACH_CHILDREN: return "HA_EXTRA_DETACH_CHILDREN";
        case HA_EXTRA_EXPORT: return "HA_EXTRA_EXPORT";
        case HA_EXTRA_SECONDARY_SORT_ROWID: return "HA_EXTRA_SECONDARY_SORT_ROWID";
        case HA_EXTRA_NO_READ_LOCKING: return "HA_EXTRA_NO_READ_LOCKING";
        case HA_EXTRA_BEGIN_ALTER_COPY: return "HA_EXTRA_BEGIN_ALTER_COPY";
        case HA_EXTRA_END_ALTER_COPY: return "HA_EXTRA_END_ALTER_COPY";
        case HA_EXTRA_NO_AUTOINC_LOCKING: return "HA_EXTRA_NO_AUTOINC_LOCKING";
        case HA_EXTRA_ENABLE_UNIQUE_RECORD_FILTER: return "HA_EXTRA_ENABLE_UNIQUE_RECORD_FILTER";
        case HA_EXTRA_DISABLE_UNIQUE_RECORD_FILTER: return "HA_EXTRA_DISABLE_UNIQUE_RECORD_FILTER";
        default: return "Unknown";
    }
}

static handler *videx_create_handler(handlerton *hton, TABLE_SHARE *table,
                                       bool partitioned, MEM_ROOT *mem_root);

handlerton *videx_hton;

/* Interface to mysqld, to check system tables supported by SE */
static bool videx_is_supported_system_table(const char *db,
                                              const char *table_name,
                                              bool is_sql_layer_system_table);

videx_share::videx_share() { thr_lock_init(&lock); }

/**
 * Initialization function for the Videx storage engine.
 * This function is called when the MySQL server is started.
 *
 * @param p Pointer to the handlerton structure for the Videx storage engine.
 * @return Always returns 0 to indicate successful initialization.
 */
static int videx_init_func(void *p) {
  DBUG_TRACE;

  videx_hton = (handlerton *)p;
  videx_hton->state = SHOW_OPTION_YES;
  videx_hton->create = videx_create_handler;
  videx_hton->flags =
       HTON_SUPPORTS_EXTENDED_KEYS |     // Supports extended keys
       HTON_SUPPORTS_FOREIGN_KEYS |      // Supports foreign keys
       HTON_SUPPORTS_ATOMIC_DDL |        // Supports atomic DDL operations
       HTON_CAN_RECREATE |               // Can recreate tables
       HTON_SUPPORTS_SECONDARY_ENGINE |  // Supports secondary storage engine
       //      HTON_SUPPORTS_TABLE_ENCRYPTION |  // Supports table encryption (commented out)
       //      HTON_SUPPORTS_ONLINE_BACKUPS |    // Supports online backups (commented out)
       HTON_SUPPORTS_COMPRESSED_COLUMNS |  // Supports compressed columns
       HTON_SUPPORTS_GENERATED_INVISIBLE_PK;  // Supports generated invisible primary keys

  // Set the is_supported_system_table function pointer to videx_is_supported_system_table.
  // This function is used to determine if a given system table is supported by the Videx storage engine.
  videx_hton->is_supported_system_table = videx_is_supported_system_table;

  return 0;
}

/**
  @brief
  Example of simple lock controls. The "share" it creates is a
  structure we will pass to each videx handler. Do you have to have
  one of these? Well, you have pieces that are used for locking, and
  they are needed to function.
*/

videx_share *ha_videx::get_share() {
  videx_share *tmp_share;

  DBUG_TRACE;

  lock_shared_ha_data();
  if (!(tmp_share = static_cast<videx_share *>(get_ha_share_ptr()))) {
    tmp_share = new videx_share;
    if (!tmp_share) goto err;

    set_ha_share_ptr(static_cast<Handler_share *>(tmp_share));
  }
err:
  unlock_shared_ha_data();
  return tmp_share;
}

static handler *videx_create_handler(handlerton *hton, TABLE_SHARE *table,
                                       bool, MEM_ROOT *mem_root) {
  videx_log_ins.markHaFuncPassby(FUNC_FILE_LINE);
  return new (mem_root) ha_videx(hton, table);
}

/** Construct ha_innobase handler. */

/**
 *
 *  - HA_NULL_IN_KEY: Indicates that the storage engine supports NULL values in indexes.
 *  - HA_CAN_INDEX_BLOBS: Indicates that the storage engine can index BLOB columns.
 *  - HA_CAN_SQL_HANDLER: Indicates that the storage engine supports the HANDLER SQL statement.
 *  - HA_PRIMARY_KEY_REQUIRED_FOR_POSITION: Indicates that the storage engine requires a primary key to determine the position of a record.
 *  - HA_PRIMARY_KEY_IN_READ_INDEX: Indicates that the storage engine includes the primary key columns in the read index.
 *  - HA_BINLOG_ROW_CAPABLE: Indicates that the storage engine is capable of row-based binary logging.
 *  - HA_CAN_GEOMETRY: Indicates that the storage engine supports spatial data types.
 *  - HA_PARTIAL_COLUMN_READ: Indicates that the storage engine supports partial column reads.
 *  - HA_TABLE_SCAN_ON_INDEX: Indicates that the storage engine can perform table scans using an index.
 *  - (comment out) HA_CAN_FULLTEXT: Indicates that the storage engine supports fulltext indexes.
 *  - (comment out) HA_CAN_FULLTEXT_EXT: Indicates that the storage engine supports extended fulltext features.
 *  - (comment out)  HA_CAN_FULLTEXT_HINTS: Indicates that the storage engine supports fulltext hints.
 *  - HA_CAN_EXPORT: Indicates that the storage engine supports exporting tables.
 *  - (comment out) HA_CAN_RTREEKEYS: Indicates that the storage engine supports R-tree indexes.
 *  - HA_NO_READ_LOCAL_LOCK: Indicates that the storage engine does not require read locks on local tables.
 *  - HA_GENERATED_COLUMNS: Indicates that the storage engine supports generated columns.
 *  - HA_ATTACHABLE_TRX_COMPATIBLE: Indicates that the storage engine is compatible with attachable transactions.
 *  - HA_CAN_INDEX_VIRTUAL_GENERATED_COLUMN: Indicates that the storage engine can index virtual generated columns.
 *  - HA_DESCENDING_INDEX: Indicates that the storage engine supports descending indexes.
 *  - HA_MULTI_VALUED_KEY_SUPPORT: Indicates that the storage engine supports multi-valued keys.
 *  - HA_BLOB_PARTIAL_UPDATE: Indicates that the storage engine supports partial updates of BLOB columns.
 *  - (comment out) HA_SUPPORTS_GEOGRAPHIC_GEOMETRY_COLUMN: Indicates that the storage engine supports geographic geometry columns.
 *  - HA_SUPPORTS_DEFAULT_EXPRESSION: Indicates that the storage engine supports default expressions for columns.
 * @param hton
 * @param table_arg
 */
ha_videx::ha_videx(handlerton *hton, TABLE_SHARE *table_arg)
    : handler(hton, table_arg),
      m_ds_mrr(this),
//      m_prebuilt(),
      m_user_thd(),
      m_int_table_flags(
          HA_NULL_IN_KEY | HA_CAN_INDEX_BLOBS | HA_CAN_SQL_HANDLER |
          HA_PRIMARY_KEY_REQUIRED_FOR_POSITION | HA_PRIMARY_KEY_IN_READ_INDEX |
          HA_BINLOG_ROW_CAPABLE | HA_CAN_GEOMETRY | HA_PARTIAL_COLUMN_READ |
          HA_TABLE_SCAN_ON_INDEX |
//          HA_CAN_FULLTEXT | HA_CAN_FULLTEXT_EXT | HA_CAN_FULLTEXT_HINTS |
          HA_CAN_EXPORT |
//          HA_CAN_RTREEKEYS |
          HA_NO_READ_LOCAL_LOCK | HA_GENERATED_COLUMNS |
          HA_ATTACHABLE_TRX_COMPATIBLE | HA_CAN_INDEX_VIRTUAL_GENERATED_COLUMN |
          HA_DESCENDING_INDEX | HA_MULTI_VALUED_KEY_SUPPORT |
          HA_BLOB_PARTIAL_UPDATE |
//          HA_SUPPORTS_GEOGRAPHIC_GEOMETRY_COLUMN |
          HA_SUPPORTS_DEFAULT_EXPRESSION),
      m_start_of_scan(),
//      m_stored_select_lock_type(LOCK_NONE_UNSET),
      m_mysql_has_locked() {
}

/*
  List of all system tables specific to the SE.
  Array element would look like below,
     { "<database_name>", "<system table name>" },
  The last element MUST be,
     { (const char*)NULL, (const char*)NULL }

  This array is optional, so every SE need not implement it.
*/
static st_handler_tablename ha_videx_system_tables[] = {
    {(const char *)nullptr, (const char *)nullptr}};

/**
  @brief Check if the given db.tablename is a system table for this SE.

  @param db                         Database name to check.
  @param table_name                 table name to check.
  @param is_sql_layer_system_table  if the supplied db.table_name is a SQL
                                    layer system table.

  @retval true   Given db.table_name is supported system table.
  @retval false  Given db.table_name is not a supported system table.
*/
static bool videx_is_supported_system_table(const char *db,
                                              const char *table_name,
                                              bool is_sql_layer_system_table) {
  videx_log_ins.markHaFuncPassby(FUNC_FILE_LINE);
  st_handler_tablename *systab;

  // Does this SE support "ALL" SQL layer system tables ?
  if (is_sql_layer_system_table) return false;

  // Check if this is SE layer system tables
  systab = ha_videx_system_tables;
  while (systab && systab->db) {
    if (systab->db == db && strcmp(systab->tablename, table_name) == 0)
      return true;
    systab++;
  }

  return false;
}

/**
  @brief
  Used for opening tables. The name will be the name of the file.

  @details
  A table is opened when it needs to be opened; e.g. when a request comes in
  for a SELECT on the table (tables are not open and closed for each request,
  they are cached).

  Called from handler.cc by handler::ha_open(). The server opens all tables by
  calling ha_open() which then calls the handler specific open().

  @see
  handler::ha_open() in handler.cc
*/

int ha_videx::open(const char *name, int, uint, const dd::Table *) {
  DBUG_TRACE;

  if (!(share = get_share())) return 1;
  thr_lock_data_init(&share->lock, &lock, nullptr);

  if (table_share->is_missing_primary_key()) {
    log_errlog(ERROR_LEVEL, ER_INNODB_PK_NOT_IN_MYSQL, name);
    // Note: When InnoDB encounters a missing primary key, it finds the InnoDB primary index
    // and assigns its key_length to ref_length.
    // Considering that most normal tables in production and benchmarks have a primary key,
    // VIDEX currently does not handle this situation (do nothing).
    ref_length = 6; // DATA_ROW_ID_LEN;
  } else {
    // Following comment is directly from InnoDB code, only for reference:
    /* MySQL allocates the buffer for ref.
    key_info->key_length includes space for all key
    columns + one byte for each column that may be
    NULL. ref_length must be as exact as possible to
    save space, because all row reference buffers are
    allocated based on ref_length. */

    ref_length = table->key_info[table_share->primary_key].key_length;
  }
  // InnoDB uses stats.block_size = UNIV_PAGE_SIZE;
  // We temporarily hard-code it to 16K. If needed, it can be dynamically requested from VIDEX server in the future.
  stats.block_size = 16384;

  info(HA_STATUS_NO_LOCK | HA_STATUS_VARIABLE | HA_STATUS_CONST);

  return 0;
}

/**
  @brief
  Closes a table.

  @details
  Called from sql_base.cc, sql_select.cc, and table.cc. In sql_select.cc it is
  only used to close up temporary tables or during the process where a
  temporary table is converted over to being a myisam table.

  For sql_base.cc look at close_data_tables().

  @see
  sql_base.cc, sql_select.cc and table.cc
*/

int ha_videx::close(void) {
  DBUG_TRACE;
  return 0;
}

/**
  VIDEX doesn't support INSERT/UPDATE/DELETE operators for now.

  @brief
  write_row() inserts a row. No extra() hint is given currently if a bulk load
  is happening. buf() is a byte array of data. You can use the field
  information to extract the data from the native byte array type.

  @details
  Example of this would be:
  @code
  for (Field **field=table->field ; *field ; field++)
  {
    ...
  }
  @endcode

  See ha_tina.cc for an example of extracting all of the data as strings.
  ha_berekly.cc has an example of how to store it intact by "packing" it
  for ha_berkeley's own native storage type.

  See the note for update_row() on auto_increments. This case also applies to
  write_row().

  Called from item_sum.cc, item_sum.cc, sql_acl.cc, sql_insert.cc,
  sql_insert.cc, sql_select.cc, sql_table.cc, sql_udf.cc, and sql_update.cc.

  @see
  item_sum.cc, item_sum.cc, sql_acl.cc, sql_insert.cc,
  sql_insert.cc, sql_select.cc, sql_table.cc, sql_udf.cc and sql_update.cc
*/

int ha_videx::write_row(uchar *) {
  DBUG_TRACE;
  /*
    Example of a successful write_row. We don't store the data
    anywhere; they are thrown away. A real implementation will
    probably need to do something with 'buf'. We report a success
    here, to pretend that the insert was successful.
  */
  videx_log_ins.markPassbyUnexpected(FUNC_FILE_LINE);
  return HA_ERR_WRONG_COMMAND;
}

/**
  VIDEX doesn't support INSERT/UPDATE/DELETE operators for now.

  @brief
  Yes, update_row() does what you expect, it updates a row. old_data will have
  the previous row record in it, while new_data will have the newest data in it.
  Keep in mind that the server can do updates based on ordering if an ORDER BY
  clause was used. Consecutive ordering is not guaranteed.

  @details
  Currently new_data will not have an updated auto_increament record. You can
  do this for example by doing:

  @code

  if (table->next_number_field && record == table->record[0])
    update_auto_increment();

  @endcode

  Called from sql_select.cc, sql_acl.cc, sql_update.cc, and sql_insert.cc.

  @see
  sql_select.cc, sql_acl.cc, sql_update.cc and sql_insert.cc
*/
int ha_videx::update_row(const uchar *, uchar *) {
  DBUG_TRACE;
  videx_log_ins.markPassbyUnexpected(FUNC_FILE_LINE);
  return HA_ERR_WRONG_COMMAND;
}

/**
  VIDEX doesn't support INSERT/UPDATE/DELETE operators for now.

  @brief
  This will delete a row. buf will contain a copy of the row to be deleted.
  The server will call this right after the current row has been called (from
  either a previous rnd_nexT() or index call).

  @details
  If you keep a pointer to the last row or can access a primary key it will
  make doing the deletion quite a bit easier. Keep in mind that the server does
  not guarantee consecutive deletions. ORDER BY clauses can be used.

  Called in sql_acl.cc and sql_udf.cc to manage internal table
  information.  Called in sql_delete.cc, sql_insert.cc, and
  sql_select.cc. In sql_select it is used for removing duplicates
  while in insert it is used for REPLACE calls.

  @see
  sql_acl.cc, sql_udf.cc, sql_delete.cc, sql_insert.cc and sql_select.cc
*/

int ha_videx::delete_row(const uchar *) {
  DBUG_TRACE;
  videx_log_ins.markPassbyUnexpected(FUNC_FILE_LINE);
  return HA_ERR_WRONG_COMMAND;
}

//**
//  @brief
//  Positions an index cursor to the index specified in the handle. Fetches the
//  row if available. If the key value is null, begin at the first key of the
//  index.
//*/

//int ha_videx::index_read_map(uchar *, const uchar *, key_part_map,
//                               enum ha_rkey_function) {
//  videx_log_ins.markHaFuncPassby(FUNC_FILE_LINE);
//  int rc;
//  DBUG_TRACE;
//  rc = HA_ERR_WRONG_COMMAND;
//  return rc;
//}

/**
  VIDEX doesn't support INDEX access (index_next/index_prev/index_read/index_first) for now,
  but we have already implemented some experimental support. We will provide a limited index mock in future versions.

  @brief
  Used to read forward through the index.
*/
int ha_videx::index_next(uchar *) {
  videx_log_ins.markPassbyUnexpected(FUNC_FILE_LINE);
  // Returning 0 (Enabling this function) will lead to unexpected results in tpch-q22, so it is directly disabled.
  // We skip the index_read by turning on the optimizer parameters `subquery_to_derived`
  int rc;
  DBUG_TRACE;
  rc = HA_ERR_WRONG_COMMAND;
  return rc;
}

/**
  @brief
  Used to read backwards through the index.
*/

int ha_videx::index_prev(uchar *) {
  videx_log_ins.markPassbyUnexpected(FUNC_FILE_LINE);
  int rc;
  DBUG_TRACE;
  rc = HA_ERR_WRONG_COMMAND;
  return rc;
}


int ha_videx::index_read(uchar *,
                       const uchar *,
                       uint,
                       enum ha_rkey_function) {
  videx_log_ins.markPassbyUnexpected(FUNC_FILE_LINE);
  int rc;
  DBUG_TRACE;
  rc = HA_ERR_WRONG_COMMAND;
  return rc;
}

/**
  @brief
  index_first() asks for the first key in the index.

  @details
  Called from opt_range.cc, opt_sum.cc, sql_handler.cc, and sql_select.cc.

  @see
  opt_range.cc, opt_sum.cc, sql_handler.cc and sql_select.cc
*/
int ha_videx::index_first(uchar *) {
  videx_log_ins.markPassbyUnexpected(FUNC_FILE_LINE);
  int rc;
  DBUG_TRACE;
  rc = HA_ERR_WRONG_COMMAND;
  return rc;
}

/**
  @brief
  index_last() asks for the last key in the index.

  @details
  Called from opt_range.cc, opt_sum.cc, sql_handler.cc, and sql_select.cc.

  @see
  opt_range.cc, opt_sum.cc, sql_handler.cc and sql_select.cc
*/
int ha_videx::index_last(uchar *) {
  videx_log_ins.markPassbyUnexpected(FUNC_FILE_LINE);
  int rc;
  DBUG_TRACE;
  rc = HA_ERR_WRONG_COMMAND;
  return rc;
}

/**
  @brief
  rnd_init() is called when the system wants the storage engine to do a table
  scan. See the example in the introduction at the top of this file to see when
  rnd_init() is called.

  @details
  Called from filesort.cc, records.cc, sql_handler.cc, sql_select.cc,
  sql_table.cc, and sql_update.cc.

  @see
  filesort.cc, records.cc, sql_handler.cc, sql_select.cc, sql_table.cc and
  sql_update.cc
*/
int ha_videx::rnd_init(bool) {
  DBUG_TRACE;
  videx_log_ins.markPassbyUnexpected(FUNC_FILE_LINE);
  return 0;
}

int ha_videx::rnd_end() {
  DBUG_TRACE;
  videx_log_ins.markPassbyUnexpected(FUNC_FILE_LINE);
  return 0;
}

/**
  @brief
  This is called for each row of the table scan. When you run out of records
  you should return HA_ERR_END_OF_FILE. Fill buff up with the row information.
  The Field structure for the table is the key to getting data into buf
  in a manner that will allow the server to understand it.

  @details
  Called from filesort.cc, records.cc, sql_handler.cc, sql_select.cc,
  sql_table.cc, and sql_update.cc.

  @see
  filesort.cc, records.cc, sql_handler.cc, sql_select.cc, sql_table.cc and
  sql_update.cc
*/
int ha_videx::rnd_next(uchar *) {
  videx_log_ins.markPassbyUnexpected(FUNC_FILE_LINE);
  int rc;
  DBUG_TRACE;
  rc = HA_ERR_END_OF_FILE;
  return rc;
}

/**
  @brief
  position() is called after each call to rnd_next() if the data needs
  to be ordered. You can do something like the following to store
  the position:
  @code
  my_store_ptr(ref, ref_length, current_position);
  @endcode

  @details
  The server uses ref to store data. ref_length in the above case is
  the size needed to store current_position. ref is just a byte array
  that the server will maintain. If you are using offsets to mark rows, then
  current_position should be the offset. If it is a primary key like in
  BDB, then it needs to be a primary key.

  Called from filesort.cc, sql_select.cc, sql_delete.cc, and sql_update.cc.

  @see
  filesort.cc, sql_select.cc, sql_delete.cc and sql_update.cc
*/
void ha_videx::position(const uchar *) { 
  videx_log_ins.markPassbyUnexpected(FUNC_FILE_LINE);
  DBUG_TRACE; 
}

/**
  @brief
  This is like rnd_next, but you are given a position to use
  to determine the row. The position will be of the type that you stored in
  ref. You can use ha_get_ptr(pos,ref_length) to retrieve whatever key
  or position you saved when position() was called.

  @details
  Called from filesort.cc, records.cc, sql_insert.cc, sql_select.cc, and
  sql_update.cc.

  @see
  filesort.cc, records.cc, sql_insert.cc, sql_select.cc and sql_update.cc
*/
int ha_videx::rnd_pos(uchar *, uchar *) {
  int rc;
  DBUG_TRACE;
  videx_log_ins.markPassbyUnexpected(FUNC_FILE_LINE);
  rc = HA_ERR_WRONG_COMMAND;
  return rc;
}

/**
 * Visit VIDEX_stats_server to get `scan_time` of the table.
 *
 * @return The estimated scan time of the table.
 */
double ha_videx::scan_time() {
  VidexJsonItem request_item = construct_request(table->s->db.str, table->s->table_name.str, __PRETTY_FUNCTION__);
  std::string val_str;

  //  THD* thd = m_user_thd;
  THD* thd = ha_thd();
  int error = ask_from_videx_http(request_item, val_str, thd);
  double res_v;
  if (error) {
    // The strategy below is from ha_example's strategy, kept as the default strategy when an error occurs.
    res_v = (double) (stats.records + stats.deleted) / 20.0 + 10;
  }else{
    res_v = std::stod(val_str);
  }
  videx_log_ins.markPassby_DBTB_otherType(
      FUNC_FILE_LINE,
      to_string(table->s->db),
      to_string(table->s->table_name),
      res_v);
  return res_v;
}

/**
 * Visit VIDEX_stats_server to get `memory_buffer_size` of the table.
 *
 * @return The estimated scan time of the table.
 */
longlong ha_videx::get_memory_buffer_size() const {
  VidexJsonItem request_item = construct_request(table->s->db.str, table->s->table_name.str, __PRETTY_FUNCTION__);
  std::string val_str;

//  THD* thd = m_user_thd;
  THD* thd = ha_thd();

  int error = ask_from_videx_http(request_item, val_str, thd);

  longlong res_v;
  if (error) {
    // The strategy below is from ha_example's strategy, kept as the default strategy when an error occurs.
    res_v = handler::get_memory_buffer_size();
  }else{
    res_v = std::stoll(val_str);
  }
  videx_log_ins.markPassby_otherType(FUNC_FILE_LINE, res_v);
  return res_v;
}

/**
 * A very important function. When a query arrives, MySQL calls this function to initialize the information of a table.
 * In a session, this function is only called once by the MySQL query optimizer.
 * VIDEX requests the `videx_stats_server` to return various statistics of a single table, including:
 *  stat_n_rows: The number of rows in the table.
 *  stat_clustered_index_size: The size of the clustered index.
 *  stat_sum_of_other_index_sizes: The sum of sizes of other indexes.
 *  data_file_length: The size of the data file.
 *  index_file_length: The length of the index file.
 *  data_free_length: The length of free space in the data file.
 *  pct_cached of index: the cache percentage of an index loaded into the buffer pool.
 *
 *  [Very Important]
 *  rec_per_key of several columns: require NDV algorithm
 *
 * Returns statistics information of the table to the MySQL interpreter, in various fields of the handle object.
@param[in]      flag            what information is requested
@param[in]      is_analyze      True if called from "::analyze()".
@return HA_ERR_* error code or 0 */
int ha_videx::info_low(uint flag, bool is_analyze) {
  (void)is_analyze;
//  dict_table_t *ib_table;
  uint64_t n_rows;

  DBUG_TRACE;

  DEBUG_SYNC_C("ha_innobase_info_low");

  // construct request
    VidexStringMap res_json;
  VidexJsonItem request_item = construct_request(table->s->db.str, table->s->table_name.str, __PRETTY_FUNCTION__);
  for (uint i = 0; i < table->s->keys; i++) {
    KEY *key = &table->key_info[i];
    VidexJsonItem * keyItem = request_item.create("key");
    keyItem->add_property("name", key->name);
    keyItem->add_property_nonan("key_length", key->key_length);
    ulong j;
    for (j = 0; j < key->actual_key_parts; j++) {
      if ((key->flags & HA_FULLTEXT) || (key->flags & HA_SPATIAL)) {
              continue;
      }
      VidexJsonItem* field = keyItem->create("field");
      field->add_property("name", key->key_part[j].field->field_name);
      field->add_property_nonan("store_length", key->key_part[j].store_length);
    }
  }

  //  THD* thd = m_user_thd;
  THD* thd = ha_thd();
  int error = ask_from_videx_http(request_item, res_json, thd);
  if (error) {
    std::cout << "ask_from_videx_http error. ha_videx::info_low" << std::endl;
    return 0;
  }
  else {
    // validate the returned json
    // stat_n_rows。stat_clustered_index_size。stat_sum_of_other_index_sizes。data_file_length。index_file_length。data_free_length
      if (!(
              videx_contains_key(res_json, "stat_n_rows") &&
              videx_contains_key(res_json, "stat_clustered_index_size") &&
              videx_contains_key(res_json, "stat_sum_of_other_index_sizes") &&
              videx_contains_key(res_json, "data_file_length") &&
              videx_contains_key(res_json, "index_file_length") &&
              videx_contains_key(res_json, "data_free_length")
      )) {
      std::cout << "res_json data error=0 but miss some key." << std::endl;
      return 0;
    }
  }

//  update_thd(ha_thd());

//  m_prebuilt->trx->op_info = (char *)"returning various info to MySQL";

//  ib_table = m_prebuilt->table;
//  assert(ib_table->n_ref_count > 0);

//  if (flag & HA_STATUS_TIME) {
//    // do nothing
//    stats.update_time = (ulong)std::chrono::system_clock::to_time_t(
//        ib_table->update_time.load());
//  }

  if (flag & HA_STATUS_VARIABLE) {
//    unsigned long stat_clustered_index_size;
//    unsigned long stat_sum_of_other_index_sizes;

    n_rows = std::stoul(res_json["stat_n_rows"]);

//    stat_clustered_index_size = std::stoul(res_json["data"]["stat_clustered_index_size"].GetString());
//    stat_sum_of_other_index_sizes = std::stoul(res_json["data"]["stat_sum_of_other_index_sizes"].GetString());

    if (n_rows == 0 && !(flag & HA_STATUS_TIME) &&
        table_share->table_category != TABLE_CATEGORY_TEMPORARY) {
      n_rows++;
    }

    stats.records = (ha_rows)n_rows;
    stats.deleted = 0;

    // #### calculate_index_size_stats START
    stats.records = static_cast<ha_rows>(n_rows);
    stats.data_file_length = std::stoull(res_json["data_file_length"]);
    stats.index_file_length = std::stoull(res_json["index_file_length"]);
//      stats->data_file_length =
//          static_cast<ulonglong>(stat_clustered_index_size) * page_size.physical();
//      stats->index_file_length =
//          static_cast<ulonglong>(stat_sum_of_other_index_sizes) *
//          page_size.physical();
    if (stats.records == 0) {
      stats.mean_rec_length = 0;
    } else {
      stats.mean_rec_length =
          static_cast<ulong>(stats.data_file_length / stats.records);
    }
    // #### calculate_index_size_stats END

    if (flag & HA_STATUS_NO_LOCK || !(flag & HA_STATUS_VARIABLE_EXTRA)) {

    } else {
      // calculate_delete_length_stat(ib_table, &stats, ha_thd());
      /*
       Derivation process of stats.data_length:
        if (avail_space == UINTMAX_MAX) stats->delete_length = 0
        else stats->delete_length = avail_space * 1024
        And since stats->m_data_free = avail_space * 1024;
        If information_schema.TABLES.DATA_FREE is known, it can be inferred that:
        if DATA_FREE = 0, delete_length = 0
        else delete_length = DATA_FREE / 1024 * 1024 = DATA_FREE
        That is, delete_length = DATA_FREE
       */
      stats.delete_length = std::stoull(res_json["data_free_length"]);
    }

    stats.check_time = 0;
    stats.mrr_length_per_rec = ref_length + sizeof(void *);
  }

  // Verify the number of indexes in InnoDB and MySQL matches up. Temporarily skipped.
  for (uint i = 0; i < table->s->keys; i++) {
    ulong j;
    /* We could get index quickly through internal
    index mapping with the index translation table.
    The identity of index (match up index name with
    that of table->key_info[i]) is already verified in
    innobase_get_index().  */
//    dict_index_t *index = innobase_get_index(i);

//    if (index == nullptr) {
//      log_errlog(ERROR_LEVEL, ER_INNODB_IDX_CNT_FEWER_THAN_DEFINED_IN_MYSQL,
//                 ib_table->name.m_name, TROUBLESHOOTING_MSG);
//      break;
//    }

    KEY *key = &table->key_info[i];

    double pct_cached;

    /* We do not maintain stats for fulltext or spatial indexes.
    Thus, we can't calculate pct_cached below because we need
    dict_index_t::stat_n_leaf_pages for that. See
    dict_stats_should_ignore_index(). */
    if ((key->flags & HA_FULLTEXT) || (key->flags & HA_SPATIAL)) {
      pct_cached = IN_MEMORY_ESTIMATE_UNKNOWN;
    } else {
      // Through experiments, it is found that for indexes just loaded into memory, pct_cached can be set to 0.
//      pct_cached = index_pct_cached(index);
      std::string pct_cached_key = "pct_cached #@# " + std::string(key->name);
      if (videx_contains_key(res_json, pct_cached_key.c_str())) {
              pct_cached = std::stod(res_json[pct_cached_key.c_str()]);
      } else {
              pct_cached = 0;
      }
    }

    key->set_in_memory_estimate(pct_cached);

    if (strcmp(key->name, "PRIMARY") == 0) {
      stats.table_in_mem_estimate = pct_cached;
    }

    if (flag & HA_STATUS_CONST) {
      if (strcmp(table->s->db.str, "mysql") != 0) {
              std::cout << ""; // vscode 断点设置的很奇怪，所以只能这样打断点
      }
      if (!key->supports_records_per_key()) {
              std::ostringstream msg;
              msg << "idx_name= " << key->name << ", pct_cached= " << pct_cached << ", records_per_key=UNSUPPORT";
              videx_log_ins.markPassby_DBTB_otherType(
                  FUNC_FILE_LINE,
                  to_string(table->s->db),
                  to_string(table->s->table_name),
                  msg.str());
              continue;
      }

      for (j = 0; j < key->actual_key_parts; j++) {
              if ((key->flags & HA_FULLTEXT) || (key->flags & HA_SPATIAL)) {
                  /* The record per key does not apply to
                  FTS or Spatial indexes. */
                  key->set_records_per_key(j, 1.0f);
                  continue;
              }

//              if (j + 1 > index->n_uniq) {
//                  log_errlog(ERROR_LEVEL, ER_INNODB_IDX_COLUMN_CNT_DIFF, index->name(),
//                             ib_table->name.m_name, (unsigned long)index->n_uniq, j + 1,
//                             TROUBLESHOOTING_MSG);
//                  break;
//              }
              // 简单起见，key field 是用 大字典存的：
              // key->name #@# key->key_part[j].field->field_name
              std::string concat_key = "rec_per_key #@# " + std::string(key->name) + " #@# " + key->key_part[j].field->field_name;
              // ib_table->rec_per_keys[key->name][j];
              rec_per_key_t rec_per_key_float = -1;
              if (videx_contains_key(res_json, concat_key.c_str())){
                  rec_per_key_float = std::stof(res_json[concat_key.c_str()]);
              }
              else {
                  rec_per_key_float = stats.records;
              }

//              rec_per_key_t rec_per_key_float = static_cast<ulong>(
//                  videx_rec_per_key(index, (unsigned long int)j, stats.records));

              key->set_records_per_key(j, rec_per_key_float);

              int rec_per_key_int = rec_per_key_float / 2;

              if (rec_per_key_int == 0) {
                  rec_per_key_int = 1;
              }

              key->rec_per_key[j] = rec_per_key_int;

              std::ostringstream msg;
              msg << "idx name= " << key->name <<
                  ", key_len= " << key->key_length <<
                  ", pct_cached= " << pct_cached <<
                  ", col["<<j<<"].name= " << key->key_part[j].field->field_name <<
                  ", store_length= " << key->key_part[j].store_length <<
                  ", rec_per_key_float= " << rec_per_key_float <<
                  ", rec_per_key_int= " << rec_per_key_int;

              videx_log_ins.markPassby_DBTB_otherType(
                  FUNC_FILE_LINE,
                  to_string(table->s->db),
                  to_string(table->s->table_name),
                  msg.str());
      }
    }
  }

  // skip flag & HA_STATUS_ERRKEY

  // skip flag & HA_STATUS_AUTO
  // if ((flag & HA_STATUS_AUTO) && table->found_next_number_field)
  return 0;
}

/** Returns statistics information of the table to the MySQL interpreter,
 in various fields of the handle object.
 @return HA_ERR_* error code or 0 */

int ha_videx::info(uint flag) /*!< in: what information is requested */
{
  videx_log_ins.markHaFuncPassby(FUNC_FILE_LINE);
  return (info_low(flag, false /* not ANALYZE */));
}

/****************************************************************************
 DS-MRR implementation
 ***************************************************************************/

/**
Multi Range Read interface, DS-MRR calls */
int ha_videx::multi_range_read_init(RANGE_SEQ_IF *seq, void *seq_init_param,
                                       uint n_ranges, uint mode,
                                       HANDLER_BUFFER *buf) {
  videx_log_ins.markPassbyUnexpected(FUNC_FILE_LINE);
  m_ds_mrr.init(table);

  return (m_ds_mrr.dsmrr_init(seq, seq_init_param, n_ranges, mode, buf));
}

int ha_videx::multi_range_read_next(char **range_info) {
  videx_log_ins.markPassbyUnexpected(FUNC_FILE_LINE);
  return (m_ds_mrr.dsmrr_next(range_info));
}

ha_rows ha_videx::multi_range_read_info_const(uint keyno, RANGE_SEQ_IF *seq,
                                                 void *seq_init_param,
                                                 uint n_ranges, uint *bufsz,
                                                 uint *flags,
                                                 Cost_estimate *cost) {
  videx_log_ins.markHaFuncPassby(FUNC_FILE_LINE);
  /* See comments in ha_myisam::multi_range_read_info_const */
  m_ds_mrr.init(table);

  return (m_ds_mrr.dsmrr_info_const(keyno, seq, seq_init_param, n_ranges, bufsz,
                                    flags, cost));
}

ha_rows ha_videx::multi_range_read_info(uint keyno, uint n_ranges, uint keys,
                                           uint *bufsz, uint *flags,
                                           Cost_estimate *cost) {
  m_ds_mrr.init(table);

  return (m_ds_mrr.dsmrr_info(keyno, n_ranges, keys, bufsz, flags, cost));
}


/** Attempt to push down an index condition.
@param[in] keyno MySQL key number
@param[in] idx_cond Index condition to be checked
@return idx_cond if pushed; NULL if not pushed */
class Item *ha_videx::idx_cond_push(uint keyno, class Item *idx_cond) {
  DBUG_TRACE;
  videx_log_ins.markPassbyUnexpected(FUNC_FILE_LINE);
  assert(keyno != MAX_KEY);
  assert(idx_cond != nullptr);

  pushed_idx_cond = idx_cond;
  pushed_idx_cond_keyno = keyno;
  in_range_check_pushed_down = true;
  /* We will evaluate the condition entirely */
  return nullptr;
}

/**
  @brief
  extra() is called whenever the server wishes to send a hint to
  the storage engine. The myisam engine implements the most hints.
  ha_innodb.cc has the most exhaustive list of these hints.

    @see
  ha_innodb.cc
*/
int ha_videx::extra(enum ha_extra_function func) {
  std::string extra_str = extraFuncToString(func);
  videx_log_ins.markPassby_otherType(FUNC_FILE_LINE, extra_str);
  DBUG_TRACE;
  return 0;
}

/**
  @brief
  Used to delete all rows in a table, including cases of truncate and cases
  where the optimizer realizes that all rows will be removed as a result of an
  SQL statement.

  @details
  Called from item_sum.cc by Item_func_group_concat::clear(),
  Item_sum_count_distinct::clear(), and Item_func_group_concat::clear().
  Called from sql_delete.cc by mysql_delete().
  Called from sql_select.cc by JOIN::reinit().
  Called from sql_union.cc by st_query_block_query_expression::exec().

  @see
  Item_func_group_concat::clear(), Item_sum_count_distinct::clear() and
  Item_func_group_concat::clear() in item_sum.cc;
  mysql_delete() in sql_delete.cc;
  JOIN::reinit() in sql_select.cc and
  st_query_block_query_expression::exec() in sql_union.cc.
*/
int ha_videx::delete_all_rows() {
  DBUG_TRACE;
  videx_log_ins.markPassbyUnexpected(FUNC_FILE_LINE);
  return HA_ERR_WRONG_COMMAND;
}

/**
  @brief
  This create a lock on the table. If you are implementing a storage engine
  that can handle transacations look at ha_berkely.cc to see how you will
  want to go about doing this. Otherwise you should consider calling flock()
  here. Hint: Read the section "locking functions for mysql" in lock.cc to
  understand this.

  @details
  Called from lock.cc by lock_external() and unlock_external(). Also called
  from sql_table.cc by copy_data_between_tables().

  @see
  lock.cc by lock_external() and unlock_external() in lock.cc;
  the section "locking functions for mysql" in lock.cc;
  copy_data_between_tables() in sql_table.cc.
*/
int ha_videx::external_lock(THD *, int) {
  videx_log_ins.NotMarkPassby(FUNC_FILE_LINE);
  DBUG_TRACE;
  return 0;
}

/**
  @brief
  The idea with handler::store_lock() is: The statement decides which locks
  should be needed for the table. For updates/deletes/inserts we get WRITE
  locks, for SELECT... we get read locks.

  @details
  Before adding the lock into the table lock handler (see thr_lock.c),
  mysqld calls store lock with the requested locks. Store lock can now
  modify a write lock to a read lock (or some other lock), ignore the
  lock (if we don't want to use MySQL table locks at all), or add locks
  for many tables (like we do when we are using a MERGE handler).

  Berkeley DB, for example, changes all WRITE locks to TL_WRITE_ALLOW_WRITE
  (which signals that we are doing WRITES, but are still allowing other
  readers and writers).

  When releasing locks, store_lock() is also called. In this case one
  usually doesn't have to do anything.

  In some exceptional cases MySQL may send a request for a TL_IGNORE;
  This means that we are requesting the same lock as last time and this
  should also be ignored. (This may happen when someone does a flush
  table when we have opened a part of the tables, in which case mysqld
  closes and reopens the tables and tries to get the same locks at last
  time). In the future we will probably try to remove this.

  Called from lock.cc by get_lock_data().

  @note
  In this method one should NEVER rely on table->in_use, it may, in fact,
  refer to a different thread! (this happens if get_lock_data() is called
  from mysql_lock_abort_for_thread() function)

  @see
  get_lock_data() in lock.cc
*/
THR_LOCK_DATA **ha_videx::store_lock(THD *, THR_LOCK_DATA **to,
                                       enum thr_lock_type lock_type) {
  videx_log_ins.NotMarkPassby(FUNC_FILE_LINE);
  if (lock_type != TL_IGNORE && lock.type == TL_UNLOCK) lock.type = lock_type;
  *to++ = &lock;
  return to;
}

/**
  @brief
  Used to delete a table. By the time delete_table() has been called all
  opened references to this table will have been closed (and your globally
  shared references released). The variable name will just be the name of
  the table. You will need to remove any files you have created at this point.

  @details
  If you do not implement this, the default delete_table() is called from
  handler.cc and it will delete all files with the file extensions from
  handlerton::file_extensions.

  Called from handler.cc by delete_table and ha_create_table(). Only used
  during create if the table_flag HA_DROP_BEFORE_CREATE was specified for
  the storage engine.

  @see
  delete_table and ha_create_table() in handler.cc
*/
int ha_videx::delete_table(const char *, const dd::Table *) {
  DBUG_TRACE;
  videx_log_ins.markPassbyUnexpected(FUNC_FILE_LINE);
  /* This is not implemented but we want someone to be able that it works. */
  return 0;
}

/**
  @brief
  Renames a table from one name to another via an alter table call.

  @details
  If you do not implement this, the default rename_table() is called from
  handler.cc and it will delete all files with the file extensions from
  handlerton::file_extensions.

  Called from sql_table.cc by mysql_rename_table().

  @see
  mysql_rename_table() in sql_table.cc
*/
int ha_videx::rename_table(const char *, const char *, const dd::Table *,
                             dd::Table *) {
  videx_log_ins.markPassbyUnexpected(FUNC_FILE_LINE);
  DBUG_TRACE;
  // return HA_ERR_WRONG_COMMAND;
  return 0;
}
/** Determines if the primary key is clustered index.
 @return true */

bool ha_videx::primary_key_is_clustered() const { 
  videx_log_ins.markHaFuncPassby(FUNC_FILE_LINE);
  return (true); }
/**
  @brief
  Given a starting key and an ending key, estimate the number of rows that
  will exist between the two keys.

  @details
  end_key may be empty, in which case determine if start_key matches any rows.

  Called from opt_range.cc by check_quick_keys().

  @see
  check_quick_keys() in opt_range.cc
*/

/** from innodb: 
 * Estimates the number of index records in a range.
 @return estimated number of rows */

ha_rows ha_videx::records_in_range(
    uint keynr,         /*!< in: index number */
    key_range *min_key, /*!< in: start key value of the
                        range, may also be 0 */
    key_range *max_key) /*!< in: range end key val, may
                        also be 0 */
{
  // videx_log_ins.markPassby_otherType(FUNC_FILE_LINE, "IMPORTANT_FUNC");
  KEY *key;

  DBUG_TRACE;

  active_index = keynr;

  key = table->key_info + active_index;

  // videx_log_ins.markRecordInRange(FUNC_FILE_LINE, min_key, max_key, key);
  VidexJsonItem request_item = construct_request(table->s->db.str, table->s->table_name.str, __PRETTY_FUNCTION__);
  videx_log_ins.markRecordInRange(FUNC_FILE_LINE, min_key, max_key, key, &request_item);
  
  std::string val_str;

  //  THD* thd = m_user_thd;
  THD* thd = ha_thd();
  int error = ask_from_videx_http(request_item, val_str, thd);
  ha_rows res_v; // unsigned long long int
  if (error) {
      // keep the same as ha_example when ocrurred error
      res_v = 10;  // low number to force index usage
  }else{
      res_v = std::stoull(val_str);
  }
  videx_log_ins.markPassby_DBTB_otherType(
    FUNC_FILE_LINE,
    to_string(table->s->db), 
    to_string(table->s->table_name), 
    res_v);
    
  return res_v;
}


/* Abort execution if EXPR does not evaluate to nonzero.
   @param EXPR assertion expression that should hold */
#define ut_a(EXPR)                                        \
  do {                                                    \
    if (!(EXPR)) {                                        \
      fprintf(stderr, "Assertion failed: %s, file %s, line %d\n",\
              #EXPR, __FILE__, __LINE__);                \
      abort();                                            \
    }                                                     \
  } while (0)

/* Abort execution. */
#define ut_error do {                                     \
  fprintf(stderr, "Unrecoverable error at file %s, line %d\n",\
          __FILE__, __LINE__);                            \
  abort();                                                \
} while (0)

#define ut_ad(EXPR)
#define ut_d(EXPR)
#define ut_o(EXPR) do { EXPR; } while (0)


/** Check if a column is the only column in an index.
@param[in]      index   data dictionary index
@param[in]      column  the column to look for
@return whether the column is the only column in the index */
static bool dd_is_only_column(const dd::Index *index,
                              const dd::Column *column) {
  return (index->elements().size() == 1 &&
          &(*index->elements().begin())->column() == column);
}


/** Look up a column in a table using the system_charset_info collation.
@param[in]      dd_table        data dictionary table
@param[in]      name            column name
@return the column
@retval nullptr if not found */
inline const dd::Column *dd_find_column(const dd::Table *dd_table,
                                        const char *name) {
  for (const dd::Column *c : dd_table->columns()) {
      if (!my_strcasecmp(system_charset_info, c->name().c_str(), name)) {
      return (c);
      }
  }
  return (nullptr);
}

/** Add a hidden column when creating a table.
@param[in,out]  dd_table        table containing user columns and indexes
@param[in]      name            hidden column name
@param[in]      length          length of the column, in bytes
@param[in]      type            column type
@return the added column, or NULL if there already was a column by that name */
inline dd::Column *dd_add_hidden_column(dd::Table *dd_table, const char *name,
                                        uint length,
                                        dd::enum_column_types type) {
  if (const dd::Column *c = dd_find_column(dd_table, name)) {
      my_error(ER_WRONG_COLUMN_NAME, MYF(0), c->name().c_str());
      return (nullptr);
  }

  dd::Column *col = dd_table->add_column();
  col->set_hidden(dd::Column::enum_hidden_type::HT_HIDDEN_SE);
  col->set_name(name);
  col->set_type(type);
  col->set_nullable(false);
  col->set_char_length(length);
  col->set_collation_id(my_charset_bin.number);

  return (col);
}

inline bool dd_drop_hidden_column(dd::Table *dd_table, const char *name) {
#ifdef UNIV_DEBUG
  const dd::Column *col = dd_find_column(dd_table, name);
  ut_ad(col != nullptr);
  ut_ad(dd_column_is_dropped(col));
#endif
  return dd_table->drop_column(name);
}

/** Add a hidden index element at the end.
@param[in,out]  index   created index metadata
@param[in]      column  column of the index */
inline void dd_add_hidden_element(dd::Index *index, const dd::Column *column) {
  dd::Index_element *e = index->add_element(const_cast<dd::Column *>(column));
  e->set_hidden(true);
  e->set_order(dd::Index_element::ORDER_ASC);
}

/** Initialize a hidden unique B-tree index.
@param[in,out]  index   created index metadata
@param[in]      name    name of the index
@param[in]      column  column of the index
@return the initialized index */
inline dd::Index *dd_set_hidden_unique_index(dd::Index *index, const char *name,
                                             const dd::Column *column) {
  index->set_name(name);
  index->set_hidden(true);
  index->set_algorithm(dd::Index::IA_BTREE);
  index->set_type(dd::Index::IT_UNIQUE);
  // static const char innobase_hton_name[] = "InnoDB";
  index->set_engine("VIDEX");
  dd_add_hidden_element(index, column);
  return (index);
}


/** The name of the index created by FTS */
#define FTS_DOC_ID_INDEX_NAME "FTS_DOC_ID_INDEX"

/**
 * Add hidden columns and indexes to an InnoDB table definition.
 * We follow InnoDB's approach and support extra columns and keys here.
 * This means that an index will include the primary key columns as extra columns,
 * appending them to the end of the existing column list.
 * Example:
 *  if the primary key consists of columns c1 and c2, and an index is created on columns c3 and c4,
 *  the actual index will be created on columns c3, c4, c1, and c2. The columns c1 and c2 are hidden but require NDV estimation.
 *
 * @param[in,out]  dd_table        Data dictionary cache object.
 * @return Error number.
 * @retval 0 on success.
 */
int ha_videx::get_extra_columns_and_keys(const HA_CREATE_INFO *,
                                            const List<Create_field> *,
                                            const KEY *, uint,
                                            dd::Table *dd_table) {
  DBUG_TRACE;
  videx_log_ins.markPassby_otherType(FUNC_FILE_LINE, "@@@@@ IMPORTANT_FUNC");
  THD *thd = ha_thd();
  dd::Index *primary = nullptr;
  bool has_fulltext = false;
  const dd::Index *fts_doc_id_index = nullptr;

  for (dd::Index *i : *dd_table->indexes()) {
      /* The name "PRIMARY" is reserved for the PRIMARY KEY */
      ut_ad((i->type() == dd::Index::IT_PRIMARY) ==
            !my_strcasecmp(system_charset_info, i->name().c_str(),
                           primary_key_name));

      if (!my_strcasecmp(system_charset_info, i->name().c_str(),
                         FTS_DOC_ID_INDEX_NAME)) {
//      ut_ad(!fts_doc_id_index);
      ut_ad(i->type() != dd::Index::IT_PRIMARY);
      fts_doc_id_index = i;
      }

      switch (i->algorithm()) {
      case dd::Index::IA_SE_SPECIFIC:
              ut_d(ut_error);
              ut_o(break);
      case dd::Index::IA_HASH:
              /* This is currently blocked
              by ha_innobase::is_index_algorithm_supported(). */
              ut_d(ut_error);
              ut_o(break);
      case dd::Index::IA_RTREE:
              if (i->type() == dd::Index::IT_SPATIAL) {
                  continue;
              }
              ut_d(ut_error);
              ut_o(break);
      case dd::Index::IA_BTREE:
              switch (i->type()) {
                  case dd::Index::IT_PRIMARY:
                    ut_ad(!primary);
                    ut_ad(i == *dd_table->indexes()->begin());
                    primary = i;
                    continue;
                  case dd::Index::IT_UNIQUE:
                    if (primary == nullptr && i->is_candidate_key()) {
                      primary = i;
                      ut_ad(*dd_table->indexes()->begin() == i);
                    }
                    continue;
                  case dd::Index::IT_MULTIPLE:
                    continue;
                  case dd::Index::IT_FULLTEXT:
                  case dd::Index::IT_SPATIAL:
                    ut_d(ut_error);
              }
              break;
      case dd::Index::IA_FULLTEXT:
              if (i->type() == dd::Index::IT_FULLTEXT) {
                  has_fulltext = true;
                  continue;
              }
              ut_d(ut_error);
              ut_o(break);
      }

      my_error(ER_UNSUPPORTED_INDEX_ALGORITHM, MYF(0), i->name().c_str());
      return ER_UNSUPPORTED_INDEX_ALGORITHM;
  }

  if (has_fulltext) {
      /* Add FTS_DOC_ID_INDEX(FTS_DOC_ID) if needed */
      const dd::Column *fts_doc_id =
          dd_find_column(dd_table, FTS_DOC_ID_COL_NAME);

      if (fts_doc_id_index) {
      switch (fts_doc_id_index->type()) {
              case dd::Index::IT_PRIMARY:
                  /* PRIMARY!=FTS_DOC_ID_INDEX */
                  ut_ad(!"wrong fts_doc_id_index");
                  [[fallthrough]];
              case dd::Index::IT_UNIQUE:
                  /* We already checked for this. */
                  ut_ad(fts_doc_id_index->algorithm() == dd::Index::IA_BTREE);
                  if (dd_is_only_column(fts_doc_id_index, fts_doc_id)) {
                    break;
                  }
                  [[fallthrough]];
              case dd::Index::IT_MULTIPLE:
              case dd::Index::IT_FULLTEXT:
              case dd::Index::IT_SPATIAL:
                  my_error(ER_INNODB_FT_WRONG_DOCID_INDEX, MYF(0),
                           fts_doc_id_index->name().c_str());
                  push_warning(thd, Sql_condition::SL_WARNING, ER_WRONG_NAME_FOR_INDEX,
                               " InnoDB: Index name " FTS_DOC_ID_INDEX_NAME
                               " is reserved"
                               " for UNIQUE INDEX(" FTS_DOC_ID_COL_NAME
                               ") for "
                               " FULLTEXT Document ID indexing.");
                  return ER_INNODB_FT_WRONG_DOCID_INDEX;
      }
      ut_ad(fts_doc_id);
      }

      if (fts_doc_id) {
      if (fts_doc_id->type() != dd::enum_column_types::LONGLONG ||
          fts_doc_id->is_nullable() ||
          fts_doc_id->name() != FTS_DOC_ID_COL_NAME) {
              my_error(ER_INNODB_FT_WRONG_DOCID_COLUMN, MYF(0),
                       fts_doc_id->name().c_str());
              push_warning(thd, Sql_condition::SL_WARNING, ER_WRONG_COLUMN_NAME,
                           " InnoDB: Column name " FTS_DOC_ID_COL_NAME
                           " is reserved for"
                           " FULLTEXT Document ID indexing.");
              return ER_INNODB_FT_WRONG_DOCID_COLUMN;
      }
      } else {
      /* Add hidden FTS_DOC_ID column */
      dd::Column *col = dd_table->add_column();
      col->set_hidden(dd::Column::enum_hidden_type::HT_HIDDEN_SE);
      col->set_name(FTS_DOC_ID_COL_NAME);
      col->set_type(dd::enum_column_types::LONGLONG);
      col->set_nullable(false);
      col->set_unsigned(true);
      col->set_collation_id(1);
      fts_doc_id = col;
      }

      ut_ad(fts_doc_id);

      if (fts_doc_id_index == nullptr) {
      dd_set_hidden_unique_index(dd_table->add_index(), FTS_DOC_ID_INDEX_NAME,
                                 fts_doc_id);
      }
  }

  /** stored length for row id */
  constexpr uint32_t DATA_ROW_ID_LEN = 6;

  /** Transaction ID type size in bytes. */
  constexpr size_t DATA_TRX_ID_LEN = 6;

//  /** Rollback data pointer: 7 bytes */
//  constexpr size_t DATA_ROLL_PTR = 2;

  /** Rollback data pointer type size in bytes. */
  constexpr size_t DATA_ROLL_PTR_LEN = 7;

  if (primary == nullptr) {
      dd::Column *db_row_id = dd_add_hidden_column(
          dd_table, "DB_ROW_ID", DATA_ROW_ID_LEN, dd::enum_column_types::INT24);

      if (db_row_id == nullptr) {
      return ER_WRONG_COLUMN_NAME;
      }

      primary = dd_set_hidden_unique_index(dd_table->add_first_index(),
                                           primary_key_name, db_row_id);
  }

  /* Add PRIMARY KEY columns to each secondary index, including:
  1. all PRIMARY KEY column prefixes
  2. full PRIMARY KEY columns which don't exist in the secondary index */

//  std::vector<const dd::Index_element *,
//              ut::allocator<const dd::Index_element *>>
//      pk_elements;
  std::vector<const dd::Index_element *> pk_elements;


  for (dd::Index *index : *dd_table->indexes()) {
      if (index == primary) {
      continue;
      }

      pk_elements.clear();
      for (const dd::Index_element *e : primary->elements()) {
      if (e->is_prefix() ||
          std::search_n(
              index->elements().begin(), index->elements().end(), 1, e,
              [](const dd::Index_element *ie, const dd::Index_element *e) {
                return (&ie->column() == &e->column());
              }) == index->elements().end()) {
              pk_elements.push_back(e);
      }
      }

      for (const dd::Index_element *e : pk_elements) {
      auto ie = index->add_element(const_cast<dd::Column *>(&e->column()));
      ie->set_hidden(true);
      ie->set_order(e->order());
      }
  }

  /* Add the InnoDB system columns DB_TRX_ID, DB_ROLL_PTR. */
  dd::Column *db_trx_id = dd_add_hidden_column(
      dd_table, "DB_TRX_ID", DATA_TRX_ID_LEN, dd::enum_column_types::INT24);
  if (db_trx_id == nullptr) {
      return ER_WRONG_COLUMN_NAME;
  }

  dd::Column *db_roll_ptr =
      dd_add_hidden_column(dd_table, "DB_ROLL_PTR", DATA_ROLL_PTR_LEN,
                           dd::enum_column_types::LONGLONG);
  if (db_roll_ptr == nullptr) {
      return ER_WRONG_COLUMN_NAME;
  }

  dd_add_hidden_element(primary, db_trx_id);
  dd_add_hidden_element(primary, db_roll_ptr);

  /* Add all non-virtual columns to the clustered index,
  unless they already part of the PRIMARY KEY. */

  for (const dd::Column *c :
       const_cast<const dd::Table *>(dd_table)->columns()) {
      if (c->is_se_hidden() || c->is_virtual()) {
      continue;
      }

      if (std::search_n(primary->elements().begin(), primary->elements().end(), 1,
                        c, [](const dd::Index_element *e, const dd::Column *c) {
                          return (!e->is_prefix() && &e->column() == c);
                        }) == primary->elements().end()) {
      dd_add_hidden_element(primary, c);
      }
  }

  return 0;
}

static MYSQL_THDVAR_STR(last_create_thdvar, PLUGIN_VAR_MEMALLOC, nullptr,
                        nullptr, nullptr, nullptr);

static MYSQL_THDVAR_UINT(create_count_thdvar, 0, nullptr, nullptr, nullptr, 0,
                         0, 1000, 0);

/**
  @brief
  create() is called to create a database. The variable name will have the name
  of the table.

  @details
  When create() is called you do not need to worry about
  opening the table. Also, the .frm file will have already been
  created so adjusting create_info is not necessary. You can overwrite
  the .frm file at this point if you wish to change the table
  definition, but there are no methods currently provided for doing
  so.

  Called from handle.cc by ha_create_table().

  @see
  ha_create_table() in handle.cc
*/

int ha_videx::create(const char *name, TABLE *, HA_CREATE_INFO *,
                       dd::Table *) {
  videx_log_ins.markPassbyUnexpected(FUNC_FILE_LINE);
  DBUG_TRACE;
  /*
    This is not implemented but we want someone to be able to see that it
    works.
  */

  /*
    It's just an example of THDVAR_SET() usage below.
  */
  THD *thd = ha_thd();
  char *buf = (char *)my_malloc(PSI_NOT_INSTRUMENTED, SHOW_VAR_FUNC_BUFF_SIZE,
                                MYF(MY_FAE));
  snprintf(buf, SHOW_VAR_FUNC_BUFF_SIZE, "Last creation '%s'", name);
  THDVAR_SET(thd, last_create_thdvar, buf);
  my_free(buf);

  uint count = THDVAR(thd, create_count_thdvar) + 1;
  THDVAR_SET(thd, create_count_thdvar, &count);

  return 0;
}

struct st_mysql_storage_engine videx_storage_engine = {
    MYSQL_HANDLERTON_INTERFACE_VERSION};

static ulong srv_enum_var = 0;
static ulong srv_ulong_var = 0;
static double srv_double_var = 0;
static int srv_signed_int_var = 0;
static long srv_signed_long_var = 0;
static longlong srv_signed_longlong_var = 0;

const char *enum_var_names[] = {"e1", "e2", NullS};

TYPELIB enum_var_typelib = {array_elements(enum_var_names) - 1,
                            "enum_var_typelib", enum_var_names, nullptr};

static MYSQL_SYSVAR_ENUM(enum_var,                        // name
                         srv_enum_var,                    // varname
                         PLUGIN_VAR_RQCMDARG,             // opt
                         "Sample ENUM system variable.",  // comment
                         nullptr,                         // check
                         nullptr,                         // update
                         0,                               // def
                         &enum_var_typelib);              // typelib

static MYSQL_SYSVAR_ULONG(ulong_var, srv_ulong_var, PLUGIN_VAR_RQCMDARG,
                          "0..1000", nullptr, nullptr, 8, 0, 1000, 0);

static MYSQL_SYSVAR_DOUBLE(double_var, srv_double_var, PLUGIN_VAR_RQCMDARG,
                           "0.500000..1000.500000", nullptr, nullptr, 8.5, 0.5,
                           1000.5,
                           0);  // reserved always 0

static MYSQL_THDVAR_DOUBLE(double_thdvar, PLUGIN_VAR_RQCMDARG,
                           "0.500000..1000.500000", nullptr, nullptr, 8.5, 0.5,
                           1000.5, 0);

static MYSQL_SYSVAR_INT(signed_int_var, srv_signed_int_var, PLUGIN_VAR_RQCMDARG,
                        "INT_MIN..INT_MAX", nullptr, nullptr, -10, INT_MIN,
                        INT_MAX, 0);

static MYSQL_THDVAR_INT(signed_int_thdvar, PLUGIN_VAR_RQCMDARG,
                        "INT_MIN..INT_MAX", nullptr, nullptr, -10, INT_MIN,
                        INT_MAX, 0);

static MYSQL_SYSVAR_LONG(signed_long_var, srv_signed_long_var,
                         PLUGIN_VAR_RQCMDARG, "LONG_MIN..LONG_MAX", nullptr,
                         nullptr, -10, LONG_MIN, LONG_MAX, 0);

static MYSQL_THDVAR_LONG(signed_long_thdvar, PLUGIN_VAR_RQCMDARG,
                         "LONG_MIN..LONG_MAX", nullptr, nullptr, -10, LONG_MIN,
                         LONG_MAX, 0);

static MYSQL_SYSVAR_LONGLONG(signed_longlong_var, srv_signed_longlong_var,
                             PLUGIN_VAR_RQCMDARG, "LLONG_MIN..LLONG_MAX",
                             nullptr, nullptr, -10, LLONG_MIN, LLONG_MAX, 0);

static MYSQL_THDVAR_LONGLONG(signed_longlong_thdvar, PLUGIN_VAR_RQCMDARG,
                             "LLONG_MIN..LLONG_MAX", nullptr, nullptr, -10,
                             LLONG_MIN, LLONG_MAX, 0);

static SYS_VAR *videx_system_variables[] = {
    MYSQL_SYSVAR(enum_var),
    MYSQL_SYSVAR(ulong_var),
    MYSQL_SYSVAR(double_var),
    MYSQL_SYSVAR(double_thdvar),
    MYSQL_SYSVAR(last_create_thdvar),
    MYSQL_SYSVAR(create_count_thdvar),
    MYSQL_SYSVAR(signed_int_var),
    MYSQL_SYSVAR(signed_int_thdvar),
    MYSQL_SYSVAR(signed_long_var),
    MYSQL_SYSVAR(signed_long_thdvar),
    MYSQL_SYSVAR(signed_longlong_var),
    MYSQL_SYSVAR(signed_longlong_thdvar),
    nullptr};

// this is an videx of SHOW_FUNC
static int show_func_videx(MYSQL_THD, SHOW_VAR *var, char *buf) {
  videx_log_ins.markHaFuncPassby(FUNC_FILE_LINE);
  var->type = SHOW_CHAR;
  var->value = buf;  // it's of SHOW_VAR_FUNC_BUFF_SIZE bytes
  snprintf(buf, SHOW_VAR_FUNC_BUFF_SIZE,
           "enum_var is %lu, ulong_var is %lu, "
           "double_var is %f, signed_int_var is %d, "
           "signed_long_var is %ld, signed_longlong_var is %lld",
           srv_enum_var, srv_ulong_var, srv_double_var, srv_signed_int_var,
           srv_signed_long_var, srv_signed_longlong_var);
  return 0;
}

struct videx_vars_t {
  ulong var1;
  double var2;
  char var3[64];
  bool var4;
  bool var5;
  ulong var6;
};

videx_vars_t videx_vars = {100, 20.01, "three hundred", true, false, 8250};

static SHOW_VAR show_status_videx[] = {
    {"var1", (char *)&videx_vars.var1, SHOW_LONG, SHOW_SCOPE_GLOBAL},
    {"var2", (char *)&videx_vars.var2, SHOW_DOUBLE, SHOW_SCOPE_GLOBAL},
    {nullptr, nullptr, SHOW_UNDEF,
     SHOW_SCOPE_UNDEF}  // null terminator required
};

static SHOW_VAR show_array_videx[] = {
    {"array", (char *)show_status_videx, SHOW_ARRAY, SHOW_SCOPE_GLOBAL},
    {"var3", (char *)&videx_vars.var3, SHOW_CHAR, SHOW_SCOPE_GLOBAL},
    {"var4", (char *)&videx_vars.var4, SHOW_BOOL, SHOW_SCOPE_GLOBAL},
    {nullptr, nullptr, SHOW_UNDEF, SHOW_SCOPE_UNDEF}};

static SHOW_VAR func_status[] = {
    {"videx_func_videx", (char *)show_func_videx, SHOW_FUNC,
     SHOW_SCOPE_GLOBAL},
    {"videx_status_var5", (char *)&videx_vars.var5, SHOW_BOOL,
     SHOW_SCOPE_GLOBAL},
    {"videx_status_var6", (char *)&videx_vars.var6, SHOW_LONG,
     SHOW_SCOPE_GLOBAL},
    {"videx_status", (char *)show_array_videx, SHOW_ARRAY,
     SHOW_SCOPE_GLOBAL},
    {nullptr, nullptr, SHOW_UNDEF, SHOW_SCOPE_UNDEF}};

#ifdef STATIC_VIDEX
mysql_declare_plugin(videx_static)
#else
mysql_declare_plugin(videx)
#endif
{
    MYSQL_STORAGE_ENGINE_PLUGIN,
    &videx_storage_engine,
    "VIDEX",
    "Kang Rong",
    "Disaggregated, Extensible Virtual Index Engine for What-If Analysis",
    PLUGIN_LICENSE_GPL,
    videx_init_func, /* Plugin Init */
    nullptr,           /* Plugin check uninstall */
    nullptr,           /* Plugin Deinit */
    0x0001 /* 0.1 */,
    func_status,              /* status variables */
    videx_system_variables, /* system variables */
    nullptr,                  /* config options */
    0,                        /* flags */
} mysql_declare_plugin_end;

/** Returns the maximum number of keys.
 @return MAX_KEY */

uint ha_videx::max_supported_keys() const { 
  return (MAX_KEY); 
}

uint ha_videx::max_supported_key_length() const {
  /* An InnoDB page must store >= 2 keys; a secondary key record
  must also contain the primary key value.  Therefore, if both
  the primary key and the secondary key are at this maximum length,
  it must be less than 1/4th of the free space on a page including
  record overhead.

  MySQL imposes its own limit to this number; MAX_KEY_LENGTH = 3072.

      For page sizes = 16k, InnoDB historically reported 3500 bytes here,
  But the MySQL limit of 3072 was always used through the handler
          interface. */
  // a simplified version

  videx_log_ins.markPassby_otherType(FUNC_FILE_LINE, 3500);
  return (3500);
}


/** Returns the operations supported for indexes.
 @return flags of supported operations */

ulong ha_videx::index_flags(uint key, uint, bool) const {
  videx_log_ins.markHaFuncPassby(FUNC_FILE_LINE);
  if (table_share->key_info[key].algorithm == HA_KEY_ALG_FULLTEXT) {
    return (0);
  }

  ulong flags = HA_READ_NEXT | HA_READ_PREV | HA_READ_ORDER | HA_READ_RANGE |
                HA_KEYREAD_ONLY | HA_DO_INDEX_COND_PUSHDOWN;

  /* For spatial index, we don't support descending scan
  and ICP so far. */
  if (table_share->key_info[key].flags & HA_SPATIAL) {
    flags = HA_READ_NEXT | HA_READ_ORDER | HA_READ_RANGE | HA_KEYREAD_ONLY |
            HA_KEY_SCAN_NOT_ROR;
    return (flags);
  }

 /* For dd tables mysql.*, we disable ICP for them,
 it's for avoiding recursively access same page. */
 /*
 caused by ICP fixed. */
 const char *dbname = table_share->db.str;
 // NOTE THAT const char *dict_sys_t::s_dd_space_name = "mysql";
 // 这里把 dict_sys_t::s_dd_space_name 直接替换了
 if (dbname && strstr(dbname, "mysql") != nullptr &&
     strlen(dbname) == 5) {
   flags = HA_READ_NEXT | HA_READ_PREV | HA_READ_ORDER | HA_READ_RANGE |
           HA_KEYREAD_ONLY;
 }

  /* Multi-valued keys don't support ordered retrieval, neither they're
  suitable for keyread only retrieval. */
  if (table_share->key_info[key].flags & HA_MULTI_VALUED_KEY) {
    flags &= ~(HA_READ_ORDER | HA_KEYREAD_ONLY);
  }

  return (flags);
}


/** HA_DUPLICATE_POS and HA_READ_BEFORE_WRITE_REMOVAL is not
set from ha_innobase, but cannot yet be supported in ha_innopart.
Full text and geometry is not yet supported. */
const handler::Table_flags HA_INNOPART_DISABLED_TABLE_FLAGS =
    (HA_CAN_FULLTEXT | HA_CAN_FULLTEXT_EXT | HA_CAN_GEOMETRY |
     HA_DUPLICATE_POS | HA_READ_BEFORE_WRITE_REMOVAL);

/** Get the table flags to use for the statement.
 @return table flags */

handler::Table_flags ha_videx::table_flags() const {
  videx_log_ins.markHaFuncPassby(FUNC_FILE_LINE);
  THD *thd = ha_thd();
  handler::Table_flags flags = m_int_table_flags;

  /* If querying the table flags when no table_share is given,
  then we must check if the table to be created/checked is partitioned.
  */
  if (table_share == nullptr && thd_get_work_part_info(thd) != nullptr) {
    /* Currently ha_innopart does not support
    all InnoDB features such as GEOMETRY, FULLTEXT etc. */
    flags &= ~(HA_INNOPART_DISABLED_TABLE_FLAGS);
  }

  /* Temporary table provides accurate record count */
  if (table_share != nullptr &&
      table_share->table_category == TABLE_CATEGORY_TEMPORARY) {
    flags |= HA_STATS_RECORDS_IS_EXACT;
  }

  /* Need to use tx_isolation here since table flags is (also)
  called before prebuilt is inited. */

  ulong const tx_isolation = thd_tx_isolation(thd);

  if (tx_isolation <= ISO_READ_COMMITTED) {
    return (flags);
  }

  return (flags | HA_BINLOG_STMT_CAPABLE);
}
