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
  CLUSTRIX_KEY_UPDATE,
  CLUSTRIX_SCAN_FROM_KEY,
  CLUSTRIX_UPDATE_QUERY,
  CLUSTRIX_TRANSACTION_CMD
};

enum clustrix_transaction_flags {
    CLUSTRIX_TRANS_BEGIN = 1,
    CLUSTRIX_TRANS_COMMIT = 2,
    CLUSTRIX_TRANS_ROLLBACK = 4,
    CLUSTRIX_STMT_NEW = 8,
    CLUSTRIX_STMT_ROLLBACK = 16,
    CLUSTRIX_TRANS_COMMIT_ON_FINISH = 32
};

/****************************************************************************
** Class clustrix_connection
****************************************************************************/

void clustrix_connection::disconnect(bool is_destructor)
{
  DBUG_ENTER("clustrix_connection::disconnect");
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
  DBUG_VOID_RETURN;
}

int host_list_next;
extern int host_list_cnt;
extern char **host_list;

int clustrix_connection::connect()
{
  int error_code = 0;
  my_bool my_true = 1;
  DBUG_ENTER("clustrix_connection::connect");

  // cpu concurrency by damned!
  int host_num = host_list_next;
  host_num = host_num % host_list_cnt;
  char *host = host_list[host_num];
  host_list_next = host_num + 1;
  DBUG_PRINT("host", ("%s", host));

  /* Validate the connection parameters */
  if (!strcmp(clustrix_socket, ""))
    if (!strcmp(host, "127.0.0.1"))
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

  if (!mysql_real_connect(&clustrix_net, host,
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
      my_error(ER_CONNECT_TO_FOREIGN_DATA_SOURCE, MYF(0), host);
      DBUG_RETURN(ER_CONNECT_TO_FOREIGN_DATA_SOURCE);
    }
  }

  clustrix_net.reconnect = 1;

  DBUG_RETURN(0);
}

int clustrix_connection::begin_command(uchar command)
{
  assert(command == CLUSTRIX_TRANSACTION_CMD || has_transaction);
  command_length = 0;
  int error_code = 0;
  if ((error_code = add_command_operand_uchar(command)))
    return error_code;

  if ((error_code = add_command_operand_uchar(commit_flag_next)))
    return error_code;

  commit_flag_next &= CLUSTRIX_TRANS_COMMIT_ON_FINISH;
  return error_code;
}

int clustrix_connection::send_command()
{
  my_bool com_error;
  com_error = simple_command(&clustrix_net,
                             (enum_server_command)CLUSTRIX_SERVER_REQUEST,
                             command_buffer, command_length, TRUE);

  if (com_error)
  {
    int error_code = mysql_errno(&clustrix_net);
    my_printf_error(error_code,
                    "Clustrix error: %s", MYF(0),
                    mysql_error(&clustrix_net));
    return error_code;
  }

  return 0;
}

int clustrix_connection::read_query_response()
{
  my_bool comerr = clustrix_net.methods->read_query_result(&clustrix_net);
  if (comerr)
  {
    int error_code = mysql_errno(&clustrix_net);
    my_printf_error(error_code,
                    "Clustrix error: %s", MYF(0),
                    mysql_error(&clustrix_net));
    return error_code;
  }

  return 0;
}

int clustrix_connection::send_transaction_cmd()
{
  DBUG_ENTER("clustrix_connection::send_transaction_cmd");
  if (!commit_flag_next)
      DBUG_RETURN(0);

  int error_code;
  if ((error_code = begin_command(CLUSTRIX_TRANSACTION_CMD)))
    DBUG_RETURN(error_code);

  if ((error_code = send_command()))
    DBUG_RETURN(error_code);

  if ((error_code = read_query_response()))
    DBUG_RETURN(mysql_errno(&clustrix_net));
  
  DBUG_RETURN(error_code);
}

bool clustrix_connection::begin_transaction()
{
  DBUG_ENTER("clustrix_connection::begin_transaction");
  assert(!has_transaction);
  commit_flag_next |= CLUSTRIX_TRANS_BEGIN;
  has_transaction = TRUE;
  DBUG_RETURN(TRUE);
}

bool clustrix_connection::commit_transaction()
{
  DBUG_ENTER("clustrix_connection::commit_transaction");
  assert(has_transaction);

  if (commit_flag_next & CLUSTRIX_TRANS_BEGIN) {
    commit_flag_next &= ~CLUSTRIX_TRANS_BEGIN;
    DBUG_RETURN(FALSE);
  }

  commit_flag_next |= CLUSTRIX_TRANS_COMMIT;
  has_transaction = FALSE;
  has_anonymous_savepoint = FALSE;
  DBUG_RETURN(TRUE);
}

bool clustrix_connection::rollback_transaction()
{
  DBUG_ENTER("clustrix_connection::rollback_transaction");
  assert(has_transaction);

  if (commit_flag_next & CLUSTRIX_TRANS_BEGIN) {
    commit_flag_next &= ~CLUSTRIX_TRANS_BEGIN;
    DBUG_RETURN(FALSE);
  }

  commit_flag_next |= CLUSTRIX_TRANS_ROLLBACK;
  has_transaction = FALSE;
  has_anonymous_savepoint = FALSE;
  DBUG_RETURN(TRUE);
}

void clustrix_connection::auto_commit_next()
{
  commit_flag_next |= CLUSTRIX_TRANS_COMMIT_ON_FINISH;
}

void clustrix_connection::auto_commit_closed()
{
  assert(has_transaction);
  if (commit_flag_next & CLUSTRIX_TRANS_COMMIT_ON_FINISH) {
    has_transaction = FALSE;
    has_anonymous_savepoint = FALSE;
    commit_flag_next &= ~CLUSTRIX_TRANS_COMMIT_ON_FINISH;
  }
}

bool clustrix_connection::set_anonymous_savepoint()
{
  DBUG_ENTER("clustrix_connection::set_anonymous_savepoint");
  assert(has_transaction);
  assert(!has_anonymous_savepoint);

  commit_flag_next |= CLUSTRIX_STMT_NEW;
  has_anonymous_savepoint = TRUE;
  DBUG_RETURN(TRUE);
}

bool clustrix_connection::release_anonymous_savepoint()
{
  DBUG_ENTER("clustrix_connection::release_anonymous_savepoint");
  assert(has_transaction);
  assert(has_anonymous_savepoint);

  if (commit_flag_next & CLUSTRIX_STMT_NEW) {
      commit_flag_next &= ~CLUSTRIX_STMT_NEW;
      DBUG_RETURN(FALSE);
  }

  has_anonymous_savepoint = FALSE;
  DBUG_RETURN(TRUE);
}

bool clustrix_connection::rollback_to_anonymous_savepoint()
{
  DBUG_ENTER("clustrix_connection::rollback_to_anonymous_savepoint");
  assert(has_transaction);
  assert(has_anonymous_savepoint);

  if (commit_flag_next & CLUSTRIX_STMT_NEW) {
      commit_flag_next &= ~CLUSTRIX_STMT_NEW;
      DBUG_RETURN(FALSE);
  }

  commit_flag_next |= CLUSTRIX_STMT_ROLLBACK;
  has_anonymous_savepoint = FALSE;
  DBUG_RETURN(TRUE);
}

int clustrix_connection::run_query(String &stmt)
{
  int error_code = mysql_real_query(&clustrix_net, stmt.ptr(), stmt.length());
  if (error_code)
    return mysql_errno(&clustrix_net);
  return error_code;
}

my_ulonglong clustrix_connection::rows_affected()
{
    return clustrix_net.affected_rows;
}

int clustrix_connection::write_row(ulonglong clustrix_table_oid,
                                   uchar *packed_row, size_t packed_size,
                                   ulonglong *last_insert_id)
{
  int error_code;
  command_length = 0;

  if ((error_code = begin_command(CLUSTRIX_WRITE_ROW)))
    return error_code;

  if ((error_code = add_command_operand_ulonglong(clustrix_table_oid)))
    return error_code;

  if ((error_code = add_command_operand_str(packed_row, packed_size)))
    return error_code;

  if ((error_code = send_command()))
    return error_code;

  if ((error_code = read_query_response()))
    return error_code;

  auto_commit_closed();
  *last_insert_id = clustrix_net.insert_id;
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

  if ((error_code = begin_command(CLUSTRIX_KEY_UPDATE)))
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
    return error_code;

  auto_commit_closed();
  return error_code;

}

int clustrix_connection::key_delete(ulonglong clustrix_table_oid,
                                    uchar *packed_key, size_t packed_key_length)
{
  int error_code;
  command_length = 0;

  if ((error_code = begin_command(CLUSTRIX_KEY_DELETE)))
    return error_code;

  if ((error_code = add_command_operand_ulonglong(clustrix_table_oid)))
    return error_code;

  if ((error_code = add_command_operand_str(packed_key, packed_key_length)))
    return error_code;

  if ((error_code = send_command()))
    return error_code;

  if ((error_code = read_query_response()))
    return error_code;

  auto_commit_closed();
  return error_code;
}

int clustrix_connection::key_read(ulonglong clustrix_table_oid, uint index,
                                  MY_BITMAP *read_set, uchar *packed_key,
                                  ulong packed_key_length, uchar **rowdata,
                                  ulong *rowdata_length)
{
  int error_code;
  command_length = 0;

  if ((error_code = begin_command(CLUSTRIX_KEY_READ)))
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

  auto_commit_closed();
  uchar *data = clustrix_net.net.read_pos;
  *rowdata_length = safe_net_field_length_ll(&data, packet_length);
  *rowdata = (uchar *)my_malloc(*rowdata_length, MYF(MY_WME));
  memcpy(*rowdata, data, *rowdata_length); 

  packet_length = cli_safe_read(&clustrix_net);
  if (packet_length == packet_error) {
    my_free(*rowdata);
    *rowdata = NULL;
    *rowdata_length = 0;
    return mysql_errno(&clustrix_net);
  }

  return 0;
}

class clustrix_connection_cursor {
  struct rowdata {
    ulong length;
    uchar *data;
  };

  ulong current_row;
  ulong last_row;
  struct rowdata *rows;
  uchar *outstanding_row; // to be freed on next request.
  MYSQL *clustrix_net;

public:
  ulong buffer_size;
  ulonglong scan_refid;
  bool eof_reached;

private:
  int cache_row(uchar *rowdata, ulong rowdata_length)
  {
    DBUG_ENTER("clustrix_connection_cursor::cache_row");
    rows[last_row].length = rowdata_length;
    rows[last_row].data = (uchar *)my_malloc(rowdata_length, MYF(MY_WME));
    if (!rows[last_row].data)
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    memcpy(rows[last_row].data, rowdata, rowdata_length);
    last_row++;
    DBUG_RETURN(0);
  }

  int load_rows_impl(bool *stmt_completed)
  {
    DBUG_ENTER("clustrix_connection_cursor::load_rows_impl");
    int error_code = 0;
    ulong packet_length = cli_safe_read(clustrix_net);
    if (packet_length == packet_error) {
      error_code = mysql_errno(clustrix_net);
      *stmt_completed = TRUE;
      if (error_code == HA_ERR_END_OF_FILE) {
        // We have read all rows for query.
        eof_reached = TRUE;
        DBUG_RETURN(0);
      }
      DBUG_RETURN(error_code);
    }

    uchar *rowdata = clustrix_net->net.read_pos;
    ulong rowdata_length = safe_net_field_length_ll(&rowdata, packet_length);
    if (!rowdata_length) {
      // We have read all rows in this batch.
      DBUG_RETURN(0);
    }

    if ((error_code = cache_row(rowdata, rowdata_length)))
      DBUG_RETURN(error_code);

    DBUG_RETURN(load_rows_impl(stmt_completed));
  }

public:
  clustrix_connection_cursor(MYSQL *clustrix_net_, ulong bufsize)
  {
    DBUG_ENTER("clustrix_connection_cursor::clustrix_connection_cursor");
    clustrix_net = clustrix_net_;
    eof_reached = FALSE;
    current_row = 0;
    last_row = 0;
    outstanding_row = NULL;
    buffer_size = bufsize;
    rows = NULL;
    DBUG_VOID_RETURN;
  }

  ~clustrix_connection_cursor()
  {
    DBUG_ENTER("clustrix_connection_cursor::~clustrix_connection_cursor");
    if (outstanding_row)
      my_free(outstanding_row);
    while (current_row < last_row)
      my_free(rows[current_row++].data);
    if (rows)
      my_free(rows);
    DBUG_VOID_RETURN;
  }

  int load_rows(bool *stmt_completed)
  {
    DBUG_ENTER("clustrix_connection_cursor::load_rows");
    current_row = 0;
    last_row = 0;
    DBUG_RETURN(load_rows_impl(stmt_completed));
  }

  int initialize(bool *stmt_completed)
  {
    DBUG_ENTER("clustrix_connection_cursor::initialize");
    ulong packet_length = cli_safe_read(clustrix_net);
    if (packet_length == packet_error) {
      *stmt_completed = TRUE;
      DBUG_RETURN(mysql_errno(clustrix_net));
    }

    unsigned char *pos = clustrix_net->net.read_pos;
    scan_refid = safe_net_field_length_ll(&pos, packet_length);

    rows = (struct rowdata *)my_malloc(buffer_size * sizeof(struct rowdata),
                                       MYF(MY_WME));
    if (!rows)
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);

    DBUG_RETURN(load_rows(stmt_completed));
  }

  uchar *retrieve_row(ulong *rowdata_length)
  {
    DBUG_ENTER("clustrix_connection_cursor::retrieve_row");
    if (outstanding_row) {
      my_free(outstanding_row);
      outstanding_row = NULL;
    }
    if (current_row == last_row)
      DBUG_RETURN(NULL);
    *rowdata_length = rows[current_row].length;
    outstanding_row = rows[current_row].data;
    current_row++;
    DBUG_RETURN(outstanding_row);
  }
};

int allocate_clustrix_connection_cursor(MYSQL *clustrix_net, ulong buffer_size,
                                        bool *stmt_completed,
                                        clustrix_connection_cursor **scan)
{
  DBUG_ENTER("allocate_clustrix_connection_cursor");
  *scan = new clustrix_connection_cursor(clustrix_net, buffer_size);
  if (!*scan)
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);

  DBUG_RETURN((*scan)->initialize(stmt_completed));
}

int clustrix_connection::scan_table(ulonglong clustrix_table_oid, uint index,
                                    enum sort_order sort, MY_BITMAP *read_set,
                                    ushort row_req,
                                    clustrix_connection_cursor **scan)
{
  int error_code;
  command_length = 0;

  if ((error_code = begin_command(CLUSTRIX_SCAN_TABLE)))
    return error_code;

  if ((error_code = add_command_operand_ushort(row_req)))
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

  bool stmt_completed = FALSE;
  error_code = allocate_clustrix_connection_cursor(&clustrix_net, row_req,
                                                   &stmt_completed, scan);
  if (stmt_completed)
    auto_commit_closed();
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
                                    ushort row_req,
                                    clustrix_connection_cursor **scan)
{
  int error_code;
  command_length = 0;

  if ((error_code = begin_command(CLUSTRIX_SCAN_QUERY)))
    return error_code;

  if ((error_code = add_command_operand_ushort(row_req)))
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

  bool stmt_completed = FALSE;
  error_code = allocate_clustrix_connection_cursor(&clustrix_net, row_req,
                                                   &stmt_completed, scan);
  if (stmt_completed)
    auto_commit_closed();
  return error_code;
}

/**
 * @brief
 *   Sends a command to initiate UPDATE.
 * @details
 *   Sends a command over mysql protocol connection to initiate an
 *   UPDATE query using a query text.
 * @args
 *   stmt& Query text to send
 *   dbname current working database
 *   dbname &current database name
 **/
int clustrix_connection::update_query(String &stmt, LEX_CSTRING &dbname,
                                      ulonglong *affected_rows)
{
  int error_code;
  command_length = 0;

  if ((error_code = add_command_operand_uchar(CLUSTRIX_UPDATE_QUERY)))
    return error_code;

  if ((error_code = add_command_operand_str((uchar*)dbname.str, dbname.length)))
    return error_code;

  if ((error_code = add_command_operand_str((uchar*)stmt.ptr(), stmt.length())))
    return error_code;

  if ((error_code = send_command()))
    return error_code;

  if ((error_code = read_query_response()))
    return error_code;

  auto_commit_closed();
  *affected_rows = clustrix_net.affected_rows;

  return 0;
}



int clustrix_connection::scan_from_key(ulonglong clustrix_table_oid, uint index,
                                       enum scan_type scan_dir,
                                       bool sorted_scan, MY_BITMAP *read_set,
                                       uchar *packed_key,
                                       ulong packed_key_length,
                                       ushort row_req,
                                       clustrix_connection_cursor **scan)
{
  int error_code;
  command_length = 0;

  if ((error_code = begin_command(CLUSTRIX_SCAN_FROM_KEY)))
    return error_code;

  if ((error_code = add_command_operand_ushort(row_req)))
    return error_code;

  if ((error_code = add_command_operand_ulonglong(clustrix_table_oid)))
    return error_code;

  if ((error_code = add_command_operand_uint(index)))
    return error_code;

  if ((error_code = add_command_operand_uchar(scan_dir)))
    return error_code;

  if ((error_code = add_command_operand_uchar(sorted_scan)))
    return error_code;

  if ((error_code = add_command_operand_str(packed_key, packed_key_length)))
    return error_code;

  if ((error_code = add_command_operand_bitmap(read_set)))
    return error_code;

  if ((error_code = send_command()))
    return error_code;

  bool stmt_completed = FALSE;
  error_code = allocate_clustrix_connection_cursor(&clustrix_net, row_req,
                                                   &stmt_completed, scan);
  if (stmt_completed)
    auto_commit_closed();
  return error_code;
}

int clustrix_connection::scan_next(clustrix_connection_cursor *scan,
                                   uchar **rowdata, ulong *rowdata_length)
{
  *rowdata = scan->retrieve_row(rowdata_length);
  if (*rowdata)
    return 0;

  if (scan->eof_reached)
    return HA_ERR_END_OF_FILE;

  int error_code;
  command_length = 0;

  if ((error_code = begin_command(CLUSTRIX_SCAN_NEXT)))
    return error_code;

  if ((error_code = add_command_operand_ushort(scan->buffer_size)))
    return error_code;

  if ((error_code = add_command_operand_lcb(scan->scan_refid)))
    return error_code;

  if ((error_code = send_command()))
    return error_code;

  bool stmt_completed = FALSE;
  error_code = scan->load_rows(&stmt_completed);
  if (stmt_completed)
    auto_commit_closed();
  if (error_code)
    return error_code;

  *rowdata = scan->retrieve_row(rowdata_length);
  if (!*rowdata)
    return HA_ERR_END_OF_FILE;

  return 0;
}

int clustrix_connection::scan_end(clustrix_connection_cursor *scan)
{
  int error_code;
  command_length = 0;
  ulonglong scan_refid = scan->scan_refid;
  bool eof_reached = scan->eof_reached;
  delete scan;

  if (eof_reached)
      return 0;

  if ((error_code = begin_command(CLUSTRIX_SCAN_STOP)))
    return error_code;

  if ((error_code = add_command_operand_lcb(scan_refid)))
    return error_code;

  if ((error_code = send_command()))
    return error_code;

  if ((error_code = read_query_response()))
    return error_code;

  auto_commit_closed();
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
  show.append("show simple create table ");
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

int clustrix_connection::add_command_operand_ushort(ushort value)
{
  ushort be_value = htobe16(value);
  int error_code = expand_command_buffer(sizeof(be_value));
  if (error_code)
    return error_code;

  memcpy(command_buffer + command_length, &be_value, sizeof(be_value));
  command_length += sizeof(be_value);
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

  if (!str_length)
      return 0;

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
