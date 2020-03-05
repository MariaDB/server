/*****************************************************************************
Copyright (c) 2019, 2020, MariaDB Corporation.
*****************************************************************************/

/** @file xpand_connection.cc */

#include "xpand_connection.h"
#include "ha_xpand.h"
#include <string>
#include "handler.h"
#include "table.h"
#include "sql_class.h"
#include "my_pthread.h"
#include "tztime.h"

//#include "errmsg.h"
//name conflicts on macro ER with sql_class.h
#define CR_CONNECTION_ERROR 2002
#define CR_CONN_HOST_ERROR 2003

extern int xpand_connect_timeout;
extern int xpand_read_timeout;
extern int xpand_write_timeout;
extern char *xpand_username;
extern char *xpand_password;
extern uint xpand_port;
extern char *xpand_socket;

/*
   This class implements the commands that can be sent to the cluster by the
   Xpand engine.  All of these commands return a status to the caller, but some
   commands also create open invocations on the cluster, which must be closed by
   sending additional commands.

   Transactions on the cluster are started using flags attached to commands, and
   transactions are committed or rolled back using separate commands.

   Methods ending with _next affect the transaction state after the next command
   is sent to the cluster.  Other transaction commands are sent to the cluster
   immediately, and the state is changed before they return.

       _____________________     _______________________
      |        |            |   |         |             |
      V        |            |   V         |             |
    NONE --> REQUESTED --> STARTED --> NEW_STMT         |
                                |                       |
                                `----> ROLLBACK_STMT ---`

   The commit and rollback commands will change any other state to NONE.  This
   includes the REQUESTED state, for which nothing will be sent to the cluster.
   The rollback statement command can likewise change the state from NEW_STMT to
   STARTED without sending anything to the cluster.

   In addition, the XPAND_TRANS_AUTOCOMMIT flag will cause the transactions
   for commands that complete without leaving open invocations on the cluster to
   be committed if successful or rolled back if there was an error.  If
   auto-commit is enabled, only one open invocation may be in progress at a
   time.
*/

enum xpand_trans_state {
  XPAND_TRANS_STARTED = 0,
  XPAND_TRANS_REQUESTED = 1,
  XPAND_TRANS_NEW_STMT = 2,
  XPAND_TRANS_ROLLBACK_STMT = 4,
  XPAND_TRANS_NONE = 32,
};

enum xpand_trans_post_flags {
  XPAND_TRANS_AUTOCOMMIT = 8,
  XPAND_TRANS_NO_POST_FLAGS = 0,
};

enum xpand_commands {
  XPAND_WRITE_ROW = 1,
  XPAND_SCAN_TABLE,
  XPAND_SCAN_NEXT,
  XPAND_SCAN_STOP,
  XPAND_KEY_READ,
  XPAND_KEY_DELETE,
  XPAND_SCAN_QUERY,
  XPAND_KEY_UPDATE,
  XPAND_SCAN_FROM_KEY,
  XPAND_UPDATE_QUERY,
  XPAND_COMMIT,
  XPAND_ROLLBACK,
};

/****************************************************************************
** Class xpand_connection
****************************************************************************/
xpand_connection::xpand_connection(THD *parent_thd)
  : session(parent_thd), command_buffer(NULL), command_buffer_length(0), command_length(0),
    trans_state(XPAND_TRANS_NONE), trans_flags(XPAND_TRANS_NO_POST_FLAGS)
{
  DBUG_ENTER("xpand_connection::xpand_connection");
  memset(&xpand_net, 0, sizeof(MYSQL));
  DBUG_VOID_RETURN;
}

xpand_connection::~xpand_connection()
{
  DBUG_ENTER("xpand_connection::~xpand_connection");
  if (is_connected())
    disconnect(TRUE);

  if (command_buffer)
    my_free(command_buffer);
  DBUG_VOID_RETURN;
}

void xpand_connection::disconnect(bool is_destructor)
{
  DBUG_ENTER("xpand_connection::disconnect");
  if (is_destructor)
  {
    /*
      Connection object destruction occurs after the destruction of
      the thread used by the network has begun, so usage of that
      thread object now is not reliable
    */
    xpand_net.net.thd = NULL;
  }
  mysql_close(&xpand_net);
  DBUG_VOID_RETURN;
}

extern int xpand_hosts_cur;
extern ulong xpand_balance_algorithm;

extern mysql_rwlock_t xpand_hosts_lock;
extern xpand_host_list *xpand_hosts;

int xpand_connection::connect()
{
  DBUG_ENTER("xpand_connection::connect");
  int start = 0;
  if (xpand_balance_algorithm == XPAND_BALANCE_ROUND_ROBIN)
    start = my_atomic_add32(&xpand_hosts_cur, 1);

  mysql_rwlock_rdlock(&xpand_hosts_lock);

  //search for available host
  int error_code = ER_BAD_HOST_ERROR;
  for (int i = 0; i < xpand_hosts->hosts_len; i++) {
    char *host = xpand_hosts->hosts[(start + i) % xpand_hosts->hosts_len];
    error_code = connect_direct(host);
    if (!error_code)
      break;
  }
  mysql_rwlock_unlock(&xpand_hosts_lock);
  if (error_code)
    my_error(error_code, MYF(0), "clustrix");

  DBUG_RETURN(error_code);
}


int xpand_connection::connect_direct(char *host)
{
  DBUG_ENTER("xpand_connection::connect_direct");
  my_bool my_true = true;
  DBUG_PRINT("host", ("%s", host));

  if (!mysql_init(&xpand_net))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);

  uint protocol_tcp = MYSQL_PROTOCOL_TCP;
  mysql_options(&xpand_net, MYSQL_OPT_PROTOCOL, &protocol_tcp);
  mysql_options(&xpand_net, MYSQL_OPT_READ_TIMEOUT,
                &xpand_read_timeout);
  mysql_options(&xpand_net, MYSQL_OPT_WRITE_TIMEOUT,
                &xpand_write_timeout);
  mysql_options(&xpand_net, MYSQL_OPT_CONNECT_TIMEOUT,
                &xpand_connect_timeout);
  mysql_options(&xpand_net, MYSQL_OPT_USE_REMOTE_CONNECTION,
                NULL);
  mysql_options(&xpand_net, MYSQL_SET_CHARSET_NAME, "utf8mb4");
  mysql_options(&xpand_net, MYSQL_OPT_USE_THREAD_SPECIFIC_MEMORY,
                (char *) &my_true);
  mysql_options(&xpand_net, MYSQL_INIT_COMMAND,"SET autocommit=0");

#ifdef XPAND_CONNECTION_SSL
  if (opt_ssl_ca_length | conn->tgt_ssl_capath_length |
      conn->tgt_ssl_cert_length | conn->tgt_ssl_key_length)
  {
    mysql_ssl_set(&xpand_net, conn->tgt_ssl_key, conn->tgt_ssl_cert,
                  conn->tgt_ssl_ca, conn->tgt_ssl_capath, conn->tgt_ssl_cipher);
    if (conn->tgt_ssl_vsc)
    {
      my_bool verify_flg = TRUE;
      mysql_options(&xpand_net, MYSQL_OPT_SSL_VERIFY_SERVER_CERT, &verify_flg);
    }
  }
#endif

  int error_code = 0;
  if (!mysql_real_connect(&xpand_net, host, xpand_username, xpand_password,
                          NULL, xpand_port, xpand_socket,
                          CLIENT_MULTI_STATEMENTS))
  {
    error_code = mysql_errno(&xpand_net);
    disconnect();
  }

  if (error_code && error_code != ER_CON_COUNT_ERROR) {
    error_code = ER_CONNECT_TO_FOREIGN_DATA_SOURCE;
  }

  DBUG_RETURN(error_code);
}

int xpand_connection::add_status_vars()
{
  DBUG_ENTER("xpand_connection::add_status_vars");
  assert(session);

  int error_code = 0;
  system_variables vars = session->variables;
  if ((error_code = add_command_operand_uchar(1)))
    DBUG_RETURN(error_code);
  //sql mode
  if ((error_code = add_command_operand_ulonglong(vars.sql_mode)))
    DBUG_RETURN(error_code);
  //auto increment state
  if ((error_code = add_command_operand_ushort(vars.auto_increment_increment)))
    DBUG_RETURN(error_code);
  if ((error_code = add_command_operand_ushort(vars.auto_increment_offset)))
    DBUG_RETURN(error_code);
  //character set and collation
  if ((error_code = add_command_operand_ushort(vars.character_set_client->number)))
    DBUG_RETURN(error_code);
  if ((error_code = add_command_operand_ushort(vars.collation_connection->number)))
    DBUG_RETURN(error_code);
  if ((error_code = add_command_operand_ushort(vars.collation_server->number)))
    DBUG_RETURN(error_code);
  //timezone and time names
  String tzone; //convert to utf8
  vars.time_zone->get_name()->print(&tzone, get_charset(33,0));
  if ((error_code = add_command_operand_str((const uchar*)tzone.ptr(),tzone.length())))
    DBUG_RETURN(error_code);
  if ((error_code = add_command_operand_ushort(vars.lc_time_names->number)))
    DBUG_RETURN(error_code);
  //transaction isolation
  if ((error_code = add_command_operand_uchar(vars.tx_isolation)))
    DBUG_RETURN(error_code);
  DBUG_RETURN(0);
}

int xpand_connection::begin_command(uchar command)
{
  if (trans_state == XPAND_TRANS_NONE)
    return HA_ERR_INTERNAL_ERROR;

  command_length = 0;
  int error_code = 0;
  if ((error_code = add_command_operand_uchar(command)))
    return error_code;

  if ((error_code = add_command_operand_uchar(trans_state | trans_flags)))
    return error_code;

  if (trans_state & XPAND_TRANS_NEW_STMT ||
      trans_state & XPAND_TRANS_REQUESTED)
    if ((error_code = add_status_vars()))
      return error_code;

  return error_code;
}

int xpand_connection::send_command()
{
  my_bool com_error;

  /*
     Please note:
     * The transaction state is set before the command is sent because rolling
       back a nonexistent transaction is better than leaving a tranaction open
       on the cluster.
     * The state may have alreadly been STARTED.
     * Commit and rollback commands update the transaction state after calling
       this function.
     * If auto-commit is enabled, the state may also updated after the
       response has been processed.  We do not clear the auto-commit flag here
       because it needs to be sent with each command until the transaction is
       committed or rolled back.
  */
  trans_state = XPAND_TRANS_STARTED;

  com_error = simple_command(&xpand_net,
                             (enum_server_command)XPAND_SERVER_REQUEST,
                             command_buffer, command_length, TRUE);

  if (com_error)
  {
    int error_code = mysql_errno(&xpand_net);
    my_printf_error(error_code, "Xpand error: %s", MYF(0),
                    mysql_error(&xpand_net));
    return error_code;
  }

  return 0;
}

int xpand_connection::read_query_response()
{
  my_bool comerr = xpand_net.methods->read_query_result(&xpand_net);
  int error_code = 0;
  if (comerr)
  {
    error_code = mysql_errno(&xpand_net);
    my_printf_error(error_code, "Xpand error: %s", MYF(0),
                    mysql_error(&xpand_net));
  }

  auto_commit_closed();
  return error_code;
}

bool xpand_connection::has_open_transaction()
{
  return trans_state != XPAND_TRANS_NONE;
}

int xpand_connection::commit_transaction()
{
  DBUG_ENTER("xpand_connection::commit_transaction");
  if (trans_state == XPAND_TRANS_NONE)
    DBUG_RETURN(HA_ERR_INTERNAL_ERROR);

  if (trans_state == XPAND_TRANS_REQUESTED) {
    trans_state = XPAND_TRANS_NONE;
    trans_flags = XPAND_TRANS_NO_POST_FLAGS;
    DBUG_RETURN(0);
  }

  int error_code;
  if ((error_code = begin_command(XPAND_COMMIT)))
    DBUG_RETURN(error_code);

  if ((error_code = send_command()))
    DBUG_RETURN(error_code);

  if ((error_code = read_query_response()))
    DBUG_RETURN(error_code);

  trans_state = XPAND_TRANS_NONE;
  trans_flags = XPAND_TRANS_NO_POST_FLAGS;
  DBUG_RETURN(error_code);
}

int xpand_connection::rollback_transaction()
{
  DBUG_ENTER("xpand_connection::rollback_transaction");
  if (trans_state == XPAND_TRANS_NONE ||
      trans_state == XPAND_TRANS_REQUESTED) {
    trans_state = XPAND_TRANS_NONE;
    DBUG_RETURN(0);
  }

  int error_code;
  if ((error_code = begin_command(XPAND_ROLLBACK)))
    DBUG_RETURN(error_code);

  if ((error_code = send_command()))
    DBUG_RETURN(error_code);

  if ((error_code = read_query_response()))
    DBUG_RETURN(error_code);

  trans_state = XPAND_TRANS_NONE;
  trans_flags = XPAND_TRANS_NO_POST_FLAGS;
  DBUG_RETURN(error_code);
}

int xpand_connection::begin_transaction_next()
{
  DBUG_ENTER("xpand_connection::begin_transaction_next");
  if (trans_state != XPAND_TRANS_NONE ||
      trans_flags != XPAND_TRANS_NO_POST_FLAGS)
    DBUG_RETURN(HA_ERR_INTERNAL_ERROR);

  trans_state = XPAND_TRANS_REQUESTED;
  DBUG_RETURN(0);
}

int xpand_connection::new_statement_next()
{
  DBUG_ENTER("xpand_connection::new_statement_next");
  if (trans_state != XPAND_TRANS_STARTED ||
      trans_flags != XPAND_TRANS_NO_POST_FLAGS)
    DBUG_RETURN(HA_ERR_INTERNAL_ERROR);

  trans_state = XPAND_TRANS_NEW_STMT;
  DBUG_RETURN(0);
}

int xpand_connection::rollback_statement_next()
{
  DBUG_ENTER("xpand_connection::rollback_statement_next");
  if (trans_state != XPAND_TRANS_STARTED ||
      trans_flags != XPAND_TRANS_NO_POST_FLAGS)
    DBUG_RETURN(HA_ERR_INTERNAL_ERROR);

  trans_state = XPAND_TRANS_ROLLBACK_STMT;
  DBUG_RETURN(0);
}

void xpand_connection::auto_commit_next()
{
  trans_flags |= XPAND_TRANS_AUTOCOMMIT;
}

void xpand_connection::auto_commit_closed()
{
  if (trans_flags & XPAND_TRANS_AUTOCOMMIT) {
    trans_flags &= ~XPAND_TRANS_AUTOCOMMIT;
    trans_state = XPAND_TRANS_NONE;
  }
}

int xpand_connection::run_query(String &stmt)
{
  int error_code = mysql_real_query(&xpand_net, stmt.ptr(), stmt.length());
  if (error_code)
    return mysql_errno(&xpand_net);
  return error_code;
}

int xpand_connection::write_row(ulonglong xpand_table_oid, uchar *packed_row,
                                size_t packed_size, ulonglong *last_insert_id)
{
  int error_code;
  command_length = 0;

  // row based commands should not be called with auto commit.
  if (trans_flags & XPAND_TRANS_AUTOCOMMIT)
    return HA_ERR_INTERNAL_ERROR;

  if ((error_code = begin_command(XPAND_WRITE_ROW)))
    return error_code;

  if ((error_code = add_command_operand_ulonglong(xpand_table_oid)))
    return error_code;

  if ((error_code = add_command_operand_str(packed_row, packed_size)))
    return error_code;

  if ((error_code = send_command()))
    return error_code;

  if ((error_code = read_query_response()))
    return error_code;

  *last_insert_id = xpand_net.insert_id;
  return error_code;
}

int xpand_connection::key_update(ulonglong xpand_table_oid, uchar *packed_key,
                                 size_t packed_key_length,
                                 MY_BITMAP *update_set, uchar *packed_new_data,
                                 size_t packed_new_length)
{
  int error_code;
  command_length = 0;

  // row based commands should not be called with auto commit.
  if (trans_flags & XPAND_TRANS_AUTOCOMMIT)
    return HA_ERR_INTERNAL_ERROR;

  if ((error_code = begin_command(XPAND_KEY_UPDATE)))
    return error_code;

  if ((error_code = add_command_operand_ulonglong(xpand_table_oid)))
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

  return error_code;
}

int xpand_connection::key_delete(ulonglong xpand_table_oid,
                                 uchar *packed_key, size_t packed_key_length)
{
  int error_code;
  command_length = 0;

  // row based commands should not be called with auto commit.
  if (trans_flags & XPAND_TRANS_AUTOCOMMIT)
    return HA_ERR_INTERNAL_ERROR;

  if ((error_code = begin_command(XPAND_KEY_DELETE)))
    return error_code;

  if ((error_code = add_command_operand_ulonglong(xpand_table_oid)))
    return error_code;

  if ((error_code = add_command_operand_str(packed_key, packed_key_length)))
    return error_code;

  if ((error_code = send_command()))
    return error_code;

  if ((error_code = read_query_response()))
    return error_code;

  return error_code;
}

int xpand_connection::key_read(ulonglong xpand_table_oid, uint index,
                               xpand_lock_mode_t lock_mode, MY_BITMAP *read_set,
                               uchar *packed_key, ulong packed_key_length,
                               uchar **rowdata, ulong *rowdata_length)
{
  int error_code;
  command_length = 0;

  // row based commands should not be called with auto commit.
  if (trans_flags & XPAND_TRANS_AUTOCOMMIT)
    return HA_ERR_INTERNAL_ERROR;

  if ((error_code = begin_command(XPAND_KEY_READ)))
    return error_code;

  if ((error_code = add_command_operand_ulonglong(xpand_table_oid)))
    return error_code;

  if ((error_code = add_command_operand_uint(index)))
    return error_code;

  if ((error_code = add_command_operand_uchar((uchar)lock_mode)))
    return error_code;

  if ((error_code = add_command_operand_bitmap(read_set)))
    return error_code;

  if ((error_code = add_command_operand_str(packed_key, packed_key_length)))
    return error_code;

  if ((error_code = send_command()))
    return error_code;

  ulong packet_length = cli_safe_read(&xpand_net);
  if (packet_length == packet_error)
    return mysql_errno(&xpand_net);

  uchar *data = xpand_net.net.read_pos;
  *rowdata_length = safe_net_field_length_ll(&data, packet_length);
  *rowdata = (uchar *)my_malloc(*rowdata_length, MYF(MY_WME));
  memcpy(*rowdata, data, *rowdata_length);

  packet_length = cli_safe_read(&xpand_net);
  if (packet_length == packet_error) {
    my_free(*rowdata);
    *rowdata = NULL;
    *rowdata_length = 0;
    return mysql_errno(&xpand_net);
  }

  return 0;
}

class xpand_connection_cursor {
  struct rowdata {
    ulong length;
    uchar *data;
  };

  ulong current_row;
  ulong last_row;
  struct rowdata *rows;
  uchar *outstanding_row; // to be freed on next request.
  MYSQL *xpand_net;

public:
  ulong buffer_size;
  ulonglong scan_refid;
  bool eof_reached;

private:
  int cache_row(uchar *rowdata, ulong rowdata_length)
  {
    DBUG_ENTER("xpand_connection_cursor::cache_row");
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
    DBUG_ENTER("xpand_connection_cursor::load_rows_impl");
    int error_code = 0;
    ulong packet_length = cli_safe_read(xpand_net);
    if (packet_length == packet_error) {
      error_code = mysql_errno(xpand_net);
      *stmt_completed = TRUE;
      if (error_code == HA_ERR_END_OF_FILE) {
        // We have read all rows for query.
        eof_reached = TRUE;
        DBUG_RETURN(0);
      }
      DBUG_RETURN(error_code);
    }

    uchar *rowdata = xpand_net->net.read_pos;
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
  xpand_connection_cursor(MYSQL *xpand_net_, ulong bufsize)
  {
    DBUG_ENTER("xpand_connection_cursor::xpand_connection_cursor");
    xpand_net = xpand_net_;
    eof_reached = FALSE;
    current_row = 0;
    last_row = 0;
    outstanding_row = NULL;
    buffer_size = bufsize;
    rows = NULL;
    DBUG_VOID_RETURN;
  }

  ~xpand_connection_cursor()
  {
    DBUG_ENTER("xpand_connection_cursor::~xpand_connection_cursor");
    if (outstanding_row)
      my_free(outstanding_row);
    if (rows) {
      while (current_row < last_row)
        my_free(rows[current_row++].data);
      my_free(rows);
    }
    DBUG_VOID_RETURN;
  }

  int load_rows(bool *stmt_completed)
  {
    DBUG_ENTER("xpand_connection_cursor::load_rows");
    current_row = 0;
    last_row = 0;
    DBUG_RETURN(load_rows_impl(stmt_completed));
  }

  int initialize(bool *stmt_completed)
  {
    DBUG_ENTER("xpand_connection_cursor::initialize");
    ulong packet_length = cli_safe_read(xpand_net);
    if (packet_length == packet_error) {
      *stmt_completed = TRUE;
      int error_code = mysql_errno(xpand_net);
      my_printf_error(error_code, "Xpand error: %s", MYF(0),
                    mysql_error(xpand_net));
      DBUG_RETURN(error_code);
    }

    unsigned char *pos = xpand_net->net.read_pos;
    scan_refid = safe_net_field_length_ll(&pos, packet_length);

    rows = (struct rowdata *)my_malloc(buffer_size * sizeof(struct rowdata),
                                       MYF(MY_WME));
    if (!rows)
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);

    DBUG_RETURN(load_rows(stmt_completed));
  }

  uchar *retrieve_row(ulong *rowdata_length)
  {
    DBUG_ENTER("xpand_connection_cursor::retrieve_row");
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

int xpand_connection::allocate_cursor(MYSQL *xpand_net, ulong buffer_size,
                                      xpand_connection_cursor **scan)
{
  DBUG_ENTER("xpand_connection::allocate_cursor");
  *scan = new xpand_connection_cursor(xpand_net, buffer_size);
  if (!*scan)
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);

  bool stmt_completed = FALSE;
  int error_code = (*scan)->initialize(&stmt_completed);
  if (error_code) {
    delete *scan;
    *scan = NULL;
  }

  if (stmt_completed)
    auto_commit_closed();

  DBUG_RETURN(error_code);
}

int xpand_connection::scan_table(ulonglong xpand_table_oid,
                                 xpand_lock_mode_t lock_mode,
                                 MY_BITMAP *read_set, ushort row_req,
                                 xpand_connection_cursor **scan)
{
  int error_code;
  command_length = 0;

  // row based commands should not be called with auto commit.
  if (trans_flags & XPAND_TRANS_AUTOCOMMIT)
    return HA_ERR_INTERNAL_ERROR;

  if ((error_code = begin_command(XPAND_SCAN_TABLE)))
    return error_code;

  if ((error_code = add_command_operand_ushort(row_req)))
    return error_code;

  if ((error_code = add_command_operand_ulonglong(xpand_table_oid)))
    return error_code;

  if ((error_code = add_command_operand_uchar((uchar)lock_mode)))
    return error_code;

  if ((error_code = add_command_operand_bitmap(read_set)))
    return error_code;

  if ((error_code = send_command()))
    return error_code;

  return allocate_cursor(&xpand_net, row_req, scan);
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
int xpand_connection::scan_query(String &stmt, uchar *fieldtype, uint fields,
                                 uchar *null_bits, uint null_bits_size,
                                 uchar *field_metadata,
                                 uint field_metadata_size, ushort row_req,
                                 ulonglong *oids,
                                 xpand_connection_cursor **scan)
{
  int error_code;
  command_length = 0;

  if ((error_code = begin_command(XPAND_SCAN_QUERY)))
    return error_code;

  do {
    if ((error_code = add_command_operand_ulonglong(*oids)))
      return error_code;
  }
  while (*oids++);

  if ((error_code = add_command_operand_ushort(row_req)))
    return error_code;

  if ((error_code = add_command_operand_str((uchar*)stmt.ptr(), stmt.length())))
    return error_code;

  if ((error_code = add_command_operand_str(fieldtype, fields)))
    return error_code;

  if ((error_code = add_command_operand_str(field_metadata,
                                            field_metadata_size)))
    return error_code;

  // This variable length string calls for an additional store w/o lcb lenth prefix.
  if ((error_code = add_command_operand_vlstr(null_bits, null_bits_size)))
    return error_code;

  if ((error_code = send_command()))
    return error_code;

  return allocate_cursor(&xpand_net, row_req, scan);
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
int xpand_connection::update_query(String &stmt, LEX_CSTRING &dbname,
                                   ulonglong *oids, ulonglong *affected_rows)
{
  int error_code;
  command_length = 0;

  if ((error_code = begin_command(XPAND_UPDATE_QUERY)))
    return error_code;

  do {
    if ((error_code = add_command_operand_ulonglong(*oids)))
      return error_code;
  }
  while (*oids++);

  if ((error_code = add_command_operand_str((uchar*)dbname.str, dbname.length)))
    return error_code;

  if ((error_code = add_command_operand_str((uchar*)stmt.ptr(), stmt.length())))
    return error_code;

  if ((error_code = send_command()))
    return error_code;

  error_code = read_query_response();
  if (!error_code)
    *affected_rows = xpand_net.affected_rows;

  return error_code;
}

int xpand_connection::scan_from_key(ulonglong xpand_table_oid, uint index,
                                    xpand_lock_mode_t lock_mode,
                                    enum scan_type scan_dir,
                                    int no_key_cols, bool sorted_scan,
                                    MY_BITMAP *read_set, uchar *packed_key,
                                    ulong packed_key_length, ushort row_req,
                                    xpand_connection_cursor **scan)
{
  int error_code;
  command_length = 0;

  // row based commands should not be called with auto commit.
  if (trans_flags & XPAND_TRANS_AUTOCOMMIT)
    return HA_ERR_INTERNAL_ERROR;

  if ((error_code = begin_command(XPAND_SCAN_FROM_KEY)))
    return error_code;

  if ((error_code = add_command_operand_ushort(row_req)))
    return error_code;

  if ((error_code = add_command_operand_ulonglong(xpand_table_oid)))
    return error_code;

  if ((error_code = add_command_operand_uint(index)))
    return error_code;

  if ((error_code = add_command_operand_uchar((uchar)lock_mode)))
    return error_code;

  if ((error_code = add_command_operand_uchar(scan_dir)))
    return error_code;

  if ((error_code = add_command_operand_uint(no_key_cols)))
    return error_code;

  if ((error_code = add_command_operand_uchar(sorted_scan)))
    return error_code;

  if ((error_code = add_command_operand_str(packed_key, packed_key_length)))
    return error_code;

  if ((error_code = add_command_operand_bitmap(read_set)))
    return error_code;

  if ((error_code = send_command()))
    return error_code;

  return allocate_cursor(&xpand_net, row_req, scan);
}

int xpand_connection::scan_next(xpand_connection_cursor *scan,
                                uchar **rowdata, ulong *rowdata_length)
{
  *rowdata = scan->retrieve_row(rowdata_length);
  if (*rowdata)
    return 0;

  if (scan->eof_reached)
    return HA_ERR_END_OF_FILE;

  int error_code;
  command_length = 0;

  if ((error_code = begin_command(XPAND_SCAN_NEXT)))
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

int xpand_connection::scan_end(xpand_connection_cursor *scan)
{
  int error_code;
  command_length = 0;
  ulonglong scan_refid = scan->scan_refid;
  bool eof_reached = scan->eof_reached;
  delete scan;

  if (eof_reached)
      return 0;

  if ((error_code = begin_command(XPAND_SCAN_STOP)))
    return error_code;

  if ((error_code = add_command_operand_lcb(scan_refid)))
    return error_code;

  if ((error_code = send_command()))
    return error_code;

  return read_query_response();
}

int xpand_connection::populate_table_list(LEX_CSTRING *db,
                                          handlerton::discovered_list *result)
{
  int error_code = 0;
  String stmt;
  stmt.append("SHOW FULL TABLES FROM ");
  stmt.append(db);
  stmt.append(" WHERE table_type = 'BASE TABLE'");

  if (mysql_real_query(&xpand_net, stmt.c_ptr(), stmt.length())) {
    int error_code = mysql_errno(&xpand_net);
    if (error_code == ER_BAD_DB_ERROR)
      return 0;
    else
      return error_code;
  }

  MYSQL_RES *results = mysql_store_result(&xpand_net);
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


/*
  Given a table name, find its OID in the Clustrix, and save it in TABLE_SHARE

  @param db     Database name
  @param name   Table name
  @param oid    OUT   Return the OID here
  @param share  INOUT If not NULL and the share has ha_share pointer, also
                      update Xpand_share::xpand_table_oid.

  @return
     0 - OK
     error code if an error occurred
*/

int xpand_connection::get_table_oid(const char *db, size_t db_len,
                                    const char *name, size_t name_len,
                                    ulonglong *oid, TABLE_SHARE *share)
{
  MYSQL_ROW row;
  int error_code = 0;
  MYSQL_RES *results_oid = NULL;
  String get_oid;
  DBUG_ENTER("xpand_connection::get_table_oid");

  /* get oid */
  get_oid.append("select r.table "
                 "from system.databases d "
                 "     inner join ""system.relations r on d.db = r.db "
                 "where d.name = '");
  get_oid.append(db, db_len);
  get_oid.append("' and r.name = '");
  get_oid.append(name, name_len);
  get_oid.append("'");

  if (mysql_real_query(&xpand_net, get_oid.c_ptr(), get_oid.length())) {
    if ((error_code = mysql_errno(&xpand_net))) {
      DBUG_PRINT("mysql_real_query returns ", ("%d", error_code));
      error_code = HA_ERR_NO_SUCH_TABLE;
      goto error;
    }
  }

  results_oid = mysql_store_result(&xpand_net);
  DBUG_PRINT("oid results",
             ("rows: %llu, fields: %u", mysql_num_rows(results_oid),
              mysql_num_fields(results_oid)));

  if (mysql_num_rows(results_oid) != 1) {
    error_code = HA_ERR_NO_SUCH_TABLE;
    goto error;
  }

  if ((row = mysql_fetch_row(results_oid))) {
    DBUG_PRINT("row", ("%s", row[0]));
    *oid = strtoull((const char *)row[0], NULL, 10);
  } else {
    error_code = HA_ERR_NO_SUCH_TABLE;
    goto error;
  }

error:
  if (results_oid)
    mysql_free_result(results_oid);

  DBUG_RETURN(error_code);
}


/*
  Given a table name, fetch table definition from Clustrix and fill the TABLE_SHARE
  object with details about field, indexes, etc.
*/
int xpand_connection::discover_table_details(LEX_CSTRING *db, LEX_CSTRING *name,
                                             THD *thd, TABLE_SHARE *share)
{
  DBUG_ENTER("xpand_connection::discover_table_details");
  int error_code = 0;
  MYSQL_RES *results_create = NULL;
  MYSQL_ROW row;
  String show;
  ulonglong oid = 0;
  Xpand_share *cs;

  if ((error_code = xpand_connection::get_table_oid(db->str, db->length,
                                                    name->str, name->length,
                                                    &oid, share)))
      goto error;

  if (!share->ha_share)
    share->ha_share= new Xpand_share;
  cs= static_cast<Xpand_share*>(share->ha_share);
  cs->xpand_table_oid = oid;

  /* get show create statement */
  show.append("show simple create table ");
  show.append(db);
  show.append(".");
  show.append("`");
  show.append(name);
  show.append("`");
  if (mysql_real_query(&xpand_net, show.c_ptr(), show.length())) {
    if ((error_code = mysql_errno(&xpand_net))) {
      DBUG_PRINT("mysql_real_query returns ", ("%d", error_code));
      error_code = HA_ERR_NO_SUCH_TABLE;
      goto error;
    }
  }

  results_create = mysql_store_result(&xpand_net);
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

  cs->rediscover_table = false;
error:
  if (results_create)
    mysql_free_result(results_create);
  DBUG_RETURN(error_code);
}

#define COMMAND_BUFFER_SIZE_INCREMENT 1024
#define COMMAND_BUFFER_SIZE_INCREMENT_BITS 10
int xpand_connection::expand_command_buffer(size_t add_length)
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

int xpand_connection::add_command_operand_uchar(uchar value)
{
  int error_code = expand_command_buffer(sizeof(value));
  if (error_code)
    return error_code;

  memcpy(command_buffer + command_length, &value, sizeof(value));
  command_length += sizeof(value);

  return 0;
}

int xpand_connection::add_command_operand_ushort(ushort value)
{
  ushort be_value = htobe16(value);
  int error_code = expand_command_buffer(sizeof(be_value));
  if (error_code)
    return error_code;

  memcpy(command_buffer + command_length, &be_value, sizeof(be_value));
  command_length += sizeof(be_value);
  return 0;
}

int xpand_connection::add_command_operand_uint(uint value)
{
  uint be_value = htobe32(value);
  int error_code = expand_command_buffer(sizeof(be_value));
  if (error_code)
    return error_code;

  memcpy(command_buffer + command_length, &be_value, sizeof(be_value));
  command_length += sizeof(be_value);
  return 0;
}

int xpand_connection::add_command_operand_ulonglong(ulonglong value)
{
  ulonglong be_value = htobe64(value);
  int error_code = expand_command_buffer(sizeof(be_value));
  if (error_code)
    return error_code;

  memcpy(command_buffer + command_length, &be_value, sizeof(be_value));
  command_length += sizeof(be_value);
  return 0;
}

int xpand_connection::add_command_operand_lcb(ulonglong value)
{
  int len = net_length_size(value);
  int error_code = expand_command_buffer(len);
  if (error_code)
    return error_code;

  net_store_length(command_buffer + command_length, value);
  command_length += len;
  return 0;
}

int xpand_connection::add_command_operand_str(const uchar *str,
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
int xpand_connection::add_command_operand_vlstr(const uchar *str,
                                                size_t str_length)
{
  int error_code = expand_command_buffer(str_length);
  if (error_code)
    return error_code;

  memcpy(command_buffer + command_length, str, str_length);
  command_length += str_length;
  return 0;
}

int xpand_connection::add_command_operand_lex_string(LEX_CSTRING str)
{
  return add_command_operand_str((const uchar *)str.str, str.length);
}

int xpand_connection::add_command_operand_bitmap(MY_BITMAP *bitmap)
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

/****************************************************************************
** Class xpand_host_list
****************************************************************************/

int xpand_host_list::fill(const char *hosts)
{
  strtok_buf = my_strdup(hosts, MYF(MY_WME));
  if (!strtok_buf) {
    return HA_ERR_OUT_OF_MEM;
  }

  const char *sep = ",; ";
  //parse into array
  int i = 0;
  char *cursor = NULL;
  char *token = NULL;
  for (token = strtok_r(strtok_buf, sep, &cursor);
       token && i < max_host_count;
       token = strtok_r(NULL, sep, &cursor)) {
    this->hosts[i] = token;
    i++;
  }

  //host count out of range
  if (i == 0 || token) {
    my_free(strtok_buf);
    return ER_BAD_HOST_ERROR;
  }
  hosts_len = i;

  return 0;
}

void xpand_host_list::empty()
{
  my_free(strtok_buf);
  strtok_buf = NULL;
  hosts_len = 0;
}
