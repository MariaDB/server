/*****************************************************************************
Copyright (c) 2019, 2020, MariaDB Corporation.
*****************************************************************************/

#ifndef _xpand_connection_h
#define _xpand_connection_h

#ifdef USE_PRAGMA_INTERFACE
#pragma interface     /* gcc class implementation */
#endif

#define MYSQL_SERVER 1
#include "my_global.h"
#include "m_string.h"
#include "mysql.h"
#include "sql_common.h"
#include "my_base.h"
#include "mysqld_error.h"
#include "my_bitmap.h"
#include "handler.h"

#define XPAND_SERVER_REQUEST 30

enum xpand_lock_mode_t {
    XPAND_NO_LOCKS,
    XPAND_SHARED,
    XPAND_EXCLUSIVE,
};

enum xpand_balance_algorithm_enum {
  XPAND_BALANCE_FIRST,
  XPAND_BALANCE_ROUND_ROBIN
};

class xpand_connection_cursor;
class xpand_connection
{
private:
  THD *session;
  MYSQL xpand_net;
  uchar *command_buffer;
  size_t command_buffer_length;
  size_t command_length;

  int trans_state;
  int trans_flags;
  int allocate_cursor(MYSQL *xpand_net, ulong buffer_size,
                      xpand_connection_cursor **scan);
public:
  xpand_connection(THD *parent_thd);
  ~xpand_connection();

  inline bool is_connected()
  {
    return xpand_net.net.vio;
  }
  int connect();
  int connect_direct(char *host);
  void disconnect(bool is_destructor = FALSE);

  bool has_open_transaction();
  int commit_transaction();
  int rollback_transaction();
  int begin_transaction_next();
  int new_statement_next();
  int rollback_statement_next(); // also starts new statement
  void auto_commit_next();
  void auto_commit_closed();

  int run_query(String &stmt);
  int write_row(ulonglong xpand_table_oid, uchar *packed_row,
                size_t packed_size, ulonglong *last_insert_id);
  int key_update(ulonglong xpand_table_oid,
                 uchar *packed_key, size_t packed_key_length,
                 MY_BITMAP *update_set,
                 uchar *packed_new_data, size_t packed_new_length);
  int key_delete(ulonglong xpand_table_oid,
                 uchar *packed_key, size_t packed_key_length);
  int key_read(ulonglong xpand_table_oid, uint index,
               xpand_lock_mode_t lock_mode, MY_BITMAP *read_set,
               uchar *packed_key, ulong packed_key_length, uchar **rowdata,
               ulong *rowdata_length);
  enum sort_order {SORT_NONE = 0, SORT_ASC = 1, SORT_DESC = 2};
  enum scan_type {
    READ_KEY_OR_NEXT,  /* rows with key and greater */
    READ_KEY_OR_PREV,  /* rows with key and less. */
    READ_AFTER_KEY,    /* rows with keys greater than key */
    READ_BEFORE_KEY,   /* rows with keys less than key */
    READ_FROM_START,   /* rows with forwards from first key. */
    READ_FROM_LAST,    /* rows with backwards from last key. */
  };
  int scan_table(ulonglong xpand_table_oid,
                 xpand_lock_mode_t lock_mode,
                 MY_BITMAP *read_set, ushort row_req,
                 xpand_connection_cursor **scan);
  int scan_query(String &stmt, uchar *fieldtype, uint fields, uchar *null_bits,
                 uint null_bits_size, uchar *field_metadata,
                 uint field_metadata_size, ushort row_req, ulonglong *oids,
                 xpand_connection_cursor **scan);
  int update_query(String &stmt, LEX_CSTRING &dbname, ulonglong *oids,
                   ulonglong *affected_rows);
  int scan_from_key(ulonglong xpand_table_oid, uint index,
                    xpand_lock_mode_t lock_mode,
                    enum scan_type scan_dir, int no_key_cols, bool sorted_scan,
                    MY_BITMAP *read_set, uchar *packed_key,
                    ulong packed_key_length, ushort row_req,
                    xpand_connection_cursor **scan);
  int scan_next(xpand_connection_cursor *scan, uchar **rowdata,
                ulong *rowdata_length);
  int scan_end(xpand_connection_cursor *scan);

  int populate_table_list(LEX_CSTRING *db, handlerton::discovered_list *result);
  int get_table_oid(const char *db, size_t db_len, const char *name,
                    size_t name_len, ulonglong *oid, TABLE_SHARE *share);
  int discover_table_details(LEX_CSTRING *db, LEX_CSTRING *name, THD *thd,
                             TABLE_SHARE *share);

private:
  int expand_command_buffer(size_t add_length);
  int add_command_operand_uchar(uchar value);
  int add_command_operand_ushort(ushort value);
  int add_command_operand_uint(uint value);
  int add_command_operand_ulonglong(ulonglong value);
  int add_command_operand_lcb(ulonglong value);
  int add_command_operand_str(const uchar *str, size_t length);
  int add_command_operand_vlstr(const uchar *str, size_t length);
  int add_command_operand_lex_string(LEX_CSTRING str);
  int add_command_operand_bitmap(MY_BITMAP *bitmap);
  int add_status_vars();
  int begin_command(uchar command);
  int send_command();
  int read_query_response();
};

static const int max_host_count = 128;
class xpand_host_list {
private:
  char *strtok_buf;
public:
  int hosts_len;
  char *hosts[max_host_count];

  int fill(const char *hosts);
  void empty();
};

#endif  // _xpand_connection_h
