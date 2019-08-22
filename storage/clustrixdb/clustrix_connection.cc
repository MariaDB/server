/*****************************************************************************
Copyright (c) 2019, MariaDB Corporation.
*****************************************************************************/

/** @file clustrix_connection.cc */

#include "clustrix_connection.h"
#include <string>
#include "errmsg.h"
#include "handler.h"
#include "table.h"

extern int clustrix_connect_timeout;
extern int clustrix_read_timeout;
extern int clustrix_write_timeout;
extern char *clustrix_host;
extern char *clustrix_username;
extern char *clustrix_password;
extern uint clustrix_port;
extern char *clustrix_socket;

static const char charset_name[] = "utf8";

enum clustrix_commands {
  CLUSTRIX_WRITE_ROW = 1,
  CLUSTRIX_SCAN_TABLE,
  CLUSTRIX_SCAN_NEXT,
  CLUSTRIX_SCAN_STOP,
  CLUSTRIX_KEY_READ,
  CLUSTRIX_KEY_DELETE,
  CLUSTRIX_SCAN_QUERY,
  CLUSTRIX_KEY_UPDATE
};

/****************************************************************************
** Class clustrix_connection
****************************************************************************/

void clustrix_connection::disconnect(bool is_destructor)
{
  if (is_destructor)
  {
    /*
      Connection object destruction occurs after the destruction of
      the thread used by the network has begun, so usage of that
      thread object now is not reliable
    */
    clustrix_net.net.thd = NULL;
  }
  mysql_close(&clustrix_net);
}

int clustrix_connection::connect()
{
  int error_code = 0;
  my_bool my_true = 1;
  DBUG_ENTER("connect");

  /* Validate the connection parameters */
  if (!strcmp(clustrix_socket, ""))
    if (!strcmp(clustrix_host, "127.0.0.1"))
      if (clustrix_port == MYSQL_PORT_DEFAULT)
        DBUG_RETURN(ER_CONNECT_TO_FOREIGN_DATA_SOURCE);

  //clustrix_net.methods = &connection_methods;

  if (!mysql_init(&clustrix_net))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);

  mysql_options(&clustrix_net, MYSQL_OPT_READ_TIMEOUT,
                &clustrix_read_timeout);
  mysql_options(&clustrix_net, MYSQL_OPT_WRITE_TIMEOUT,
                &clustrix_write_timeout);
  mysql_options(&clustrix_net, MYSQL_OPT_CONNECT_TIMEOUT,
                &clustrix_connect_timeout);
  mysql_options(&clustrix_net, MYSQL_OPT_USE_REMOTE_CONNECTION,
                NULL);
  mysql_options(&clustrix_net, MYSQL_SET_CHARSET_NAME, charset_name);
  mysql_options(&clustrix_net, MYSQL_OPT_USE_THREAD_SPECIFIC_MEMORY,
                (char *) &my_true);
  mysql_options(&clustrix_net, MYSQL_INIT_COMMAND,"SET autocommit=0");

#ifdef CLUSTRIX_CONNECTION_SSL
  if (opt_ssl_ca_length | conn->tgt_ssl_capath_length |
      conn->tgt_ssl_cert_length | conn->tgt_ssl_key_length)
  {
    mysql_ssl_set(&clustrix_net, conn->tgt_ssl_key, conn->tgt_ssl_cert,
                  conn->tgt_ssl_ca, conn->tgt_ssl_capath, conn->tgt_ssl_cipher);
    if (conn->tgt_ssl_vsc)
    {
      my_bool verify_flg = TRUE;
      mysql_options(&clustrix_net, MYSQL_OPT_SSL_VERIFY_SERVER_CERT,
                    &verify_flg);
    }
  }
#endif

  if (!mysql_real_connect(&clustrix_net, clustrix_host,
                          clustrix_username, clustrix_password,
                          NULL, clustrix_port, clustrix_socket,
                          CLIENT_MULTI_STATEMENTS))
  {
    error_code = mysql_errno(&clustrix_net);
    disconnect();

    if (error_code != CR_CONN_HOST_ERROR &&
        error_code != CR_CONNECTION_ERROR)
    {
      if (error_code == ER_CON_COUNT_ERROR)
      {
        my_error(ER_CON_COUNT_ERROR, MYF(0));
        DBUG_RETURN(ER_CON_COUNT_ERROR);
      }
      my_error(ER_CONNECT_TO_FOREIGN_DATA_SOURCE, MYF(0), clustrix_host);
      DBUG_RETURN(ER_CONNECT_TO_FOREIGN_DATA_SOURCE);
    }
  }

  clustrix_net.reconnect = 1;

  DBUG_RETURN(0);
}

int clustrix_connection::send_command()
{
  my_bool com_error;
  com_error = simple_command(&clustrix_net,
                             (enum_server_command)CLUSTRIX_SERVER_REQUEST,
                             command_buffer, command_length, TRUE);

  if (com_error)
  {
    my_printf_error(mysql_errno(&clustrix_net),
                    "Clustrix error: %s", MYF(0),
                    mysql_error(&clustrix_net));
    return ER_QUERY_ON_FOREIGN_DATA_SOURCE;
  }

  return 0;
}

int clustrix_connection::read_query_response()
{
  my_bool comerr = clustrix_net.methods->read_query_result(&clustrix_net);
  if (comerr)
  {
    my_printf_error(mysql_errno(&clustrix_net),
                    "Clustrix error: %s", MYF(0),
                    mysql_error(&clustrix_net));
    return ER_QUERY_ON_FOREIGN_DATA_SOURCE;
  }

  return 0;
}

int clustrix_connection::begin_trans()
{
  if (has_transaction)
      return 0;

  const char *stmt = "BEGIN TRANSACTION";
  int error_code = mysql_real_query(&clustrix_net, stmt, strlen(stmt));
  if (error_code)
    return mysql_errno(&clustrix_net);
  has_transaction = TRUE;
  return error_code;
}

int clustrix_connection::commit_trans()
{
  const char *stmt = "COMMIT TRANSACTION";
  int error_code = mysql_real_query(&clustrix_net, stmt, strlen(stmt));
  if (error_code)
    return mysql_errno(&clustrix_net);
  has_transaction = FALSE;
  has_statement_trans = FALSE;
  return error_code;
}

int clustrix_connection::rollback_trans()
{
  const char *stmt = "ROLLBACK TRANSACTION";
  int error_code = mysql_real_query(&clustrix_net, stmt, strlen(stmt));
  if (error_code)
    return mysql_errno(&clustrix_net);
  has_transaction = FALSE;
  has_statement_trans = FALSE;
  return error_code;
}

int clustrix_connection::begin_stmt_trans()
{
  assert(has_transaction);
  if (has_statement_trans)
    return 0;

  const char *stmt = "SAVEPOINT STMT_TRANS";
  int error_code = mysql_real_query(&clustrix_net, stmt, strlen(stmt));
  if (error_code)
    return mysql_errno(&clustrix_net);
  has_statement_trans = TRUE;
  return error_code;
}

int clustrix_connection::commit_stmt_trans()
{
  assert(has_transaction);
  const char *stmt = "RELEASE SAVEPOINT STMT_TRANS";
  int error_code = mysql_real_query(&clustrix_net, stmt, strlen(stmt));
  if (error_code)
    return mysql_errno(&clustrix_net);
  has_statement_trans = FALSE;
  return error_code;
}

int clustrix_connection::rollback_stmt_trans()
{
  assert(has_transaction);
  const char *stmt = "ROLLBACK TO STMT_TRANS";
  int error_code = mysql_real_query(&clustrix_net, stmt, strlen(stmt));
  if (error_code)
    return mysql_errno(&clustrix_net);
  has_statement_trans = FALSE;
  return error_code;
}

int clustrix_connection::run_query(String &stmt)
{
  int error_code = mysql_real_query(&clustrix_net, stmt.ptr(), stmt.length());
  if (error_code)
    return mysql_errno(&clustrix_net);
  return error_code;
}

int clustrix_connection::write_row(ulonglong clustrix_table_oid,
                                   uchar *packed_row, size_t packed_size)
{
  int error_code;
  command_length = 0;

  if ((error_code = add_command_operand_uchar(CLUSTRIX_WRITE_ROW)))
    return error_code;

  if ((error_code = add_command_operand_ulonglong(clustrix_table_oid)))
    return error_code;

  if ((error_code = add_command_operand_str(packed_row, packed_size)))
    return error_code;

  if ((error_code = send_command()))
    return error_code;

  if ((error_code = read_query_response()))
    return error_code;

  last_insert_id = clustrix_net.insert_id;
  return error_code;
}

int clustrix_connection::key_update(ulonglong clustrix_table_oid,
                                    uchar *packed_key, size_t packed_key_length,
                                    MY_BITMAP *update_set,
                                    uchar *packed_new_data,
                                    size_t packed_new_length)
{
  int error_code;
  command_length = 0;

  if ((error_code = add_command_operand_uchar(CLUSTRIX_KEY_UPDATE)))
    return error_code;

  if ((error_code = add_command_operand_ulonglong(clustrix_table_oid)))
    return error_code;

  if ((error_code = add_command_operand_str(packed_key, packed_key_length)))
    return error_code;

  if ((error_code = add_command_operand_bitmap(update_set)))
    return error_code;

  if ((error_code = add_command_operand_str(packed_new_data,
                                            packed_new_length)))
    return error_code;

  if ((error_code = send_command()))
    return error_code;

  if ((error_code = read_query_response()))
    return mysql_errno(&clustrix_net);

  return error_code;

}

int clustrix_connection::key_delete(ulonglong clustrix_table_oid,
                                    uchar *packed_key, size_t packed_key_length)
{
  int error_code;
  command_length = 0;

  if ((error_code = add_command_operand_uchar(CLUSTRIX_KEY_DELETE)))
    return error_code;

  if ((error_code = add_command_operand_ulonglong(clustrix_table_oid)))
    return error_code;

  if ((error_code = add_command_operand_str(packed_key, packed_key_length)))
    return error_code;

  if ((error_code = send_command()))
    return error_code;

  if ((error_code = read_query_response()))
    return mysql_errno(&clustrix_net);

  return error_code;
}

int clustrix_connection::key_read(ulonglong clustrix_table_oid, uint index,
                                  MY_BITMAP *read_set, uchar *packed_key,
                                  ulong packed_key_length, uchar **rowdata,
                                  ulong *rowdata_length)
{
  int error_code;
  command_length = 0;

  if ((error_code = add_command_operand_uchar(CLUSTRIX_KEY_READ)))
    return error_code;

  if ((error_code = add_command_operand_ulonglong(clustrix_table_oid)))
    return error_code;

  if ((error_code = add_command_operand_uint(index)))
    return error_code;

  if ((error_code = add_command_operand_bitmap(read_set)))
    return error_code;

  if ((error_code = add_command_operand_str(packed_key, packed_key_length)))
    return error_code;

  if ((error_code = send_command()))
    return error_code;

  ulong packet_length = cli_safe_read(&clustrix_net);
  if (packet_length == packet_error)
    return mysql_errno(&clustrix_net);

  *rowdata = clustrix_net.net.read_pos;
  *rowdata_length = safe_net_field_length_ll(rowdata, packet_length);

  return 0;
}

int clustrix_connection::scan_table(ulonglong clustrix_table_oid, uint index,
                                    enum sort_order sort, MY_BITMAP *read_set,
                                    ulonglong *scan_refid)
{
  int error_code;
  command_length = 0;

  if ((error_code = add_command_operand_uchar(CLUSTRIX_SCAN_TABLE)))
    return error_code;

  if ((error_code = add_command_operand_ulonglong(clustrix_table_oid)))
    return error_code;

  if ((error_code = add_command_operand_uint(index)))
    return error_code;

  if ((error_code = add_command_operand_uchar(sort)))
    return error_code;

  if ((error_code = add_command_operand_bitmap(read_set)))
    return error_code;

  if ((error_code = send_command()))
    return error_code;

  ulong packet_length = cli_safe_read(&clustrix_net);
  if (packet_length == packet_error)
    return mysql_errno(&clustrix_net);

  unsigned char *pos = clustrix_net.net.read_pos;
  *scan_refid = safe_net_field_length_ll(&pos, packet_length);
  return error_code;
}

/**
 * @brief
 *   Sends a command to initiate query scan.
 * @details
 *   Sends a command over mysql protocol connection to initiate an
 *   arbitrary query using a query text.
 *   Uses field types, field metadata and nullability to explicitly
 *   cast result to expected data type. Exploits RBR TABLE_MAP_EVENT
 *   format + sends SQL text.
 * @args
 *   stmt& Query text to send
 *   fieldtype* array of byte wide field types of result projection
 *   null_bits* fields nullability bitmap of result projection
 *   field_metadata* Field metadata of result projection
 *   scan_refid id used to reference this scan later
 *   Used in pushdowns to initiate query scan.
 **/
int clustrix_connection::scan_query(String &stmt, uchar *fieldtype, uint fields,
                                    uchar *null_bits, uint null_bits_size,
                                    uchar *field_metadata,
                                    uint field_metadata_size,
                                    ulonglong *scan_refid)
{
  int error_code;
  command_length = 0;

  if ((error_code = add_command_operand_uchar(CLUSTRIX_SCAN_QUERY)))
    return error_code;

  if ((error_code = add_command_operand_str((uchar*)stmt.ptr(), stmt.length())))
    return error_code;

  if ((error_code = add_command_operand_str(fieldtype, fields)))
    return error_code;

  if ((error_code = add_command_operand_str(field_metadata, field_metadata_size)))
    return error_code;

  // This variable length string calls for an additional store w/o lcb lenth prefix.
  if ((error_code = add_command_operand_vlstr(null_bits, null_bits_size)))
    return error_code;

  if ((error_code = send_command()))
    return error_code;

  ulong packet_length = cli_safe_read(&clustrix_net);
  if (packet_length == packet_error)
    return mysql_errno(&clustrix_net);

  unsigned char *pos = clustrix_net.net.read_pos;
  *scan_refid = safe_net_field_length_ll(&pos, packet_length);
  return error_code;
}

int clustrix_connection::scan_next(ulonglong scan_refid, uchar **rowdata,
                                   ulong *rowdata_length)
{
  int error_code;
  command_length = 0;

  if ((error_code = add_command_operand_uchar(CLUSTRIX_SCAN_NEXT)))
    return error_code;

  if ((error_code = add_command_operand_lcb(scan_refid)))
    return error_code;

  if ((error_code = send_command()))
    return error_code;

  ulong packet_length = cli_safe_read(&clustrix_net);
  if (packet_length == packet_error)
    return mysql_errno(&clustrix_net);

  *rowdata = clustrix_net.net.read_pos;
  *rowdata_length =  safe_net_field_length_ll(rowdata, packet_length);

  return 0;
}

int clustrix_connection::scan_end(ulonglong scan_refid)
{
  int error_code;
  command_length = 0;

  if ((error_code = add_command_operand_uchar(CLUSTRIX_SCAN_STOP)))
    return error_code;

  if ((error_code = add_command_operand_lcb(scan_refid)))
    return error_code;

  if ((error_code = send_command()))
    return error_code;

  ulong packet_length = cli_safe_read(&clustrix_net);
  if (packet_length == packet_error)
    return mysql_errno(&clustrix_net);

  return 0;
}

int clustrix_connection::populate_table_list(LEX_CSTRING *db,
                                             handlerton::discovered_list *result)
{
  int error_code = 0;
  String stmt;
  stmt.append("SHOW FULL TABLES FROM ");
  stmt.append(db);
  stmt.append(" WHERE table_type = 'BASE TABLE'");

  if (mysql_real_query(&clustrix_net, stmt.c_ptr(), stmt.length())) {
    int error_code = mysql_errno(&clustrix_net);
    if (error_code == ER_BAD_DB_ERROR)
      return 0;
    else
      return error_code;
  }

  MYSQL_RES *results = mysql_store_result(&clustrix_net);
  if (mysql_num_fields(results) != 2) {
    error_code = HA_ERR_CORRUPT_EVENT;
    goto error;
  }

  MYSQL_ROW row;
  while((row = mysql_fetch_row(results)))
    result->add_table(row[0], strlen(row[0]));

error:
  mysql_free_result(results);
  return error_code;
}

int clustrix_connection::discover_table_details(LEX_CSTRING *db,
                                                LEX_CSTRING *name, THD *thd,
                                                TABLE_SHARE *share)
{
  DBUG_ENTER("clustrix_connection::discover_table_details");
  int error_code = 0;
  MYSQL_RES *results_oid = NULL;
  MYSQL_RES *results_create = NULL;
  MYSQL_ROW row;
  String get_oid, show;

  /* get oid */
  get_oid.append("select r.table "
                 "from system.databases d "
                 "     inner join ""system.relations r on d.db = r.db "
                 "where d.name = '");
  get_oid.append(db);
  get_oid.append("' and r.name = '");
  get_oid.append(name);
  get_oid.append("'");

  if (mysql_real_query(&clustrix_net, get_oid.c_ptr(), get_oid.length())) {
    if ((error_code = mysql_errno(&clustrix_net))) {
      DBUG_PRINT("mysql_real_query returns ", ("%d", error_code));
      error_code = HA_ERR_NO_SUCH_TABLE;
      goto error;
    }
  }

  results_oid = mysql_store_result(&clustrix_net);
  DBUG_PRINT("oid results",
             ("rows: %llu, fields: %u", mysql_num_rows(results_oid),
              mysql_num_fields(results_oid)));

  if (mysql_num_rows(results_oid) != 1) {
    error_code = HA_ERR_NO_SUCH_TABLE;
    goto error;
  }

  while((row = mysql_fetch_row(results_oid))) {
    DBUG_PRINT("row", ("%s", row[0]));
    uchar *to = (uchar*)alloc_root(&share->mem_root, strlen(row[0]) + 1);
    if (!to) {
      error_code = HA_ERR_OUT_OF_MEM;
      goto error;
    }

    strcpy((char *)to, (char *)row[0]);
    share->tabledef_version.str = to;
    share->tabledef_version.length = strlen(row[0]);
  }

  /* get show create statement */
  show.append("show create table ");
  show.append(db);
  show.append(".");
  show.append(name);
  if (mysql_real_query(&clustrix_net, show.c_ptr(), show.length())) {
    if ((error_code = mysql_errno(&clustrix_net))) {
      DBUG_PRINT("mysql_real_query returns ", ("%d", error_code));
      error_code = HA_ERR_NO_SUCH_TABLE;
      goto error;
    }
  }

  results_create = mysql_store_result(&clustrix_net);
  DBUG_PRINT("show table results",
             ("rows: %llu, fields: %u", mysql_num_rows(results_create),
              mysql_num_fields(results_create)));

  if (mysql_num_rows(results_create) != 1) {
    error_code = HA_ERR_NO_SUCH_TABLE;
    goto error;
  }

  if (mysql_num_fields(results_create) != 2) {
    error_code = HA_ERR_CORRUPT_EVENT;
    goto error;
  }

  while((row = mysql_fetch_row(results_create))) {
    DBUG_PRINT("row", ("%s - %s", row[0], row[1]));
    error_code = share->init_from_sql_statement_string(thd, false, row[1],
                                                       strlen(row[1]));
  }

error:
  if (results_oid)
    mysql_free_result(results_oid);

  if (results_create)
    mysql_free_result(results_create);
  DBUG_RETURN(error_code);
}

int clustrix_connection::expand_command_buffer(size_t add_length)
{
  size_t expanded_length;

  if (command_buffer_length >= command_length + add_length)
    return 0;

  expanded_length = command_buffer_length +
                    ((add_length >> COMMAND_BUFFER_SIZE_INCREMENT_BITS)
                                 << COMMAND_BUFFER_SIZE_INCREMENT_BITS) +
                     COMMAND_BUFFER_SIZE_INCREMENT;

  if (!command_buffer_length)
    command_buffer = (uchar *) my_malloc(expanded_length, MYF(MY_WME));
  else
    command_buffer = (uchar *) my_realloc(command_buffer, expanded_length,
                                          MYF(MY_WME));
  if (!command_buffer)
    return HA_ERR_OUT_OF_MEM;

  command_buffer_length = expanded_length;

  return 0;
}

int clustrix_connection::add_command_operand_uchar(uchar value)
{
  int error_code = expand_command_buffer(sizeof(value));
  if (error_code)
    return error_code;

  memcpy(command_buffer + command_length, &value, sizeof(value));
  command_length += sizeof(value);

  return 0;
}

int clustrix_connection::add_command_operand_uint(uint value)
{
  uint be_value = htobe32(value);
  int error_code = expand_command_buffer(sizeof(be_value));
  if (error_code)
    return error_code;

  memcpy(command_buffer + command_length, &be_value, sizeof(be_value));
  command_length += sizeof(be_value);
  return 0;
}

int clustrix_connection::add_command_operand_ulonglong(ulonglong value)
{
  ulonglong be_value = htobe64(value);
  int error_code = expand_command_buffer(sizeof(be_value));
  if (error_code)
    return error_code;

  memcpy(command_buffer + command_length, &be_value, sizeof(be_value));
  command_length += sizeof(be_value);
  return 0;
}

int clustrix_connection::add_command_operand_lcb(ulonglong value)
{
  int len = net_length_size(value);
  int error_code = expand_command_buffer(len);
  if (error_code)
    return error_code;

  net_store_length(command_buffer + command_length, value);
  command_length += len;
  return 0;
}

int clustrix_connection::add_command_operand_str(const uchar *str,
                                                 size_t str_length)
{
  int error_code = add_command_operand_lcb(str_length);
  if (error_code)
    return error_code;

  error_code = expand_command_buffer(str_length);
  if (error_code)
    return error_code;

  memcpy(command_buffer + command_length, str, str_length);
  command_length += str_length;
  return 0;
}

/**
 * @brief
 *   Puts variable length string into the buffer.
 * @details
 *   Puts into the buffer variable length string the size
 *   of which is send by other means. For details see
 *   MDB Client/Server Protocol.
 * @args
 *   str - string to send
 *   str_length - size
 **/
int clustrix_connection::add_command_operand_vlstr(const uchar *str,
                                                 size_t str_length)
{
  int error_code = expand_command_buffer(str_length);
  if (error_code)
    return error_code;

  memcpy(command_buffer + command_length, str, str_length);
  command_length += str_length;
  return 0;
}

int clustrix_connection::add_command_operand_lex_string(LEX_CSTRING str)
{
  return add_command_operand_str((const uchar *)str.str, str.length);
}

int clustrix_connection::add_command_operand_bitmap(MY_BITMAP *bitmap)
{
  int error_code = add_command_operand_lcb(bitmap->n_bits);
  if (error_code)
    return error_code;

  int no_bytes = no_bytes_in_map(bitmap);
  error_code = expand_command_buffer(no_bytes);
  if (error_code)
    return error_code;

  memcpy(command_buffer + command_length, bitmap->bitmap, no_bytes);
  command_length += no_bytes;
  return 0;
}
