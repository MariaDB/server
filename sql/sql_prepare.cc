/* Copyright (c) 2002, 2015, Oracle and/or its affiliates.
   Copyright (c) 2008, 2022, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1335  USA */

/**
  @file

This file contains the implementation of prepared statements.

When one prepares a statement:

  - Server gets the query from client with command 'COM_STMT_PREPARE';
    in the following format:
    [COM_STMT_PREPARE:1] [query]
  - Parse the query and recognize any parameter markers '?' and
    store its information list in lex->param_list
  - Allocate a new statement for this prepare; and keep this in
    'thd->stmt_map'.
  - Without executing the query, return back to client the total
    number of parameters along with result-set metadata information
    (if any) in the following format:
    @verbatim
    [STMT_ID:4]
    [Column_count:2]
    [Param_count:2]
    [Params meta info (stubs only for now)]  (if Param_count > 0)
    [Columns meta info] (if Column_count > 0)
    @endverbatim

  During prepare the tables used in a statement are opened, but no
  locks are acquired.  Table opening will block any DDL during the
  operation, and we do not need any locks as we neither read nor
  modify any data during prepare.  Tables are closed after prepare
  finishes.

When one executes a statement:

  - Server gets the command 'COM_STMT_EXECUTE' to execute the
    previously prepared query. If there are any parameter markers, then the
    client will send the data in the following format:
    @verbatim
    [COM_STMT_EXECUTE:1]
    [STMT_ID:4]
    [NULL_BITS:(param_count+7)/8)]
    [TYPES_SUPPLIED_BY_CLIENT(0/1):1]
    [[length]data]
    [[length]data] .. [[length]data].
    @endverbatim
    (Note: Except for string/binary types; all other types will not be
    supplied with length field)
  - If it is a first execute or types of parameters were altered by client,
    then setup the conversion routines.
  - Assign parameter items from the supplied data.
  - Execute the query without re-parsing and send back the results
    to client

  During execution of prepared statement tables are opened and locked
  the same way they would for normal (non-prepared) statement
  execution.  Tables are unlocked and closed after the execution.

When one supplies long data for a placeholder:

  - Server gets the long data in pieces with command type
    'COM_STMT_SEND_LONG_DATA'.
  - The packet received will have the format as:
    [COM_STMT_SEND_LONG_DATA:1][STMT_ID:4][parameter_number:2][data]
  - data from the packet is appended to the long data value buffer for this
    placeholder.
  - It's up to the client to stop supplying data chunks at any point. The
    server doesn't care; also, the server doesn't notify the client whether
    it got the data or not; if there is any error, then it will be returned
    at statement execute.
*/

#include "mariadb.h"                          /* NO_EMBEDDED_ACCESS_CHECKS */
#include "sql_priv.h"
#include "unireg.h"
#include "sql_class.h"                          // set_var.h: THD
#include "set_var.h"
#include "sql_admin.h" // fill_check_table_metadata_fields
#include "sql_prepare.h"
#include "sql_parse.h" // insert_precheck, update_precheck, delete_precheck
#include "sql_base.h"  // open_normal_and_derived_tables
#include "sql_cache.h"                          // query_cache_*
#include "sql_view.h"                          // create_view_precheck
#include "sql_delete.h"                        // mysql_prepare_delete
#include "sql_select.h" // for JOIN
#include "sql_insert.h" // upgrade_lock_type_for_insert, mysql_prepare_insert
#include "sql_update.h" // mysql_prepare_update
#include "sql_db.h"     // mysql_opt_change_db, mysql_change_db
#include "sql_derived.h" // mysql_derived_prepare,
                         // mysql_handle_derived
#include "sql_cte.h"
#include "sql_cursor.h"
#include "sql_show.h"
#include "sql_repl.h"
#include "sql_help.h"    // mysqld_help_prepare
#include "sql_table.h"   // fill_checksum_table_metadata_fields
#include "slave.h"
#include "sp_head.h"
#include "sp.h"
#include "sp_cache.h"
#include "sql_handler.h"  // mysql_ha_rm_tables
#include "probes_mysql.h"
#include "opt_trace.h"
#ifdef EMBEDDED_LIBRARY
/* include MYSQL_BIND headers */
#include <mysql.h>
#else
#include <mysql_com.h>
/* Constants defining bits in parameter type flags. Flags are read from high byte of short value */
static const uint PARAMETER_FLAG_UNSIGNED= 128U << 8;
#endif
#include "lock.h"                               // MYSQL_OPEN_FORCE_SHARED_MDL
#include "log_event.h"                          // class Log_event
#include "sql_handler.h"
#include "transaction.h"                        // trans_rollback_implicit
#include "mysql/psi/mysql_ps.h"                 // MYSQL_EXECUTE_PS
#ifdef WITH_WSREP
#include "wsrep_mysqld.h"
#include "wsrep_trans_observer.h"
#endif /* WITH_WSREP */
#include "xa.h"           // xa_recover_get_fields
#include "sql_audit.h"    // mysql_audit_release

/**
  A result class used to send cursor rows using the binary protocol.
*/

class Select_fetch_protocol_binary: public select_send
{
  Protocol_binary protocol;
public:
  Select_fetch_protocol_binary(THD *thd);
  virtual bool send_result_set_metadata(List<Item> &list, uint flags);
  virtual int send_data(List<Item> &items);
  virtual bool send_eof();
#ifdef EMBEDDED_LIBRARY
  void begin_dataset()
  {
    protocol.begin_dataset();
  }
#endif
};

/****************************************************************************/

/**
  Prepared_statement: a statement that can contain placeholders.
*/

class Prepared_statement: public Statement
{
public:
  enum flag_values
  {
    IS_IN_USE= 1,
    IS_SQL_PREPARE= 2
  };

  THD *thd;
  PSI_prepared_stmt* m_prepared_stmt;
  Select_fetch_protocol_binary result;
  Item_param **param_array;
  Server_side_cursor *cursor;
  uchar *packet;
  uchar *packet_end;
  uint param_count;
  uint last_errno;
  uint flags;
  char last_error[MYSQL_ERRMSG_SIZE];
  my_bool iterations;
  my_bool start_param;
  my_bool read_types;

#ifndef EMBEDDED_LIBRARY
  bool (*set_params)(Prepared_statement *st, uchar *data, uchar *data_end,
                     uchar *read_pos, String *expanded_query);
  bool (*set_bulk_params)(Prepared_statement *st,
                          uchar **read_pos, uchar *data_end, bool reset);
#else
  bool (*set_params_data)(Prepared_statement *st, String *expanded_query);
  /*TODO: add bulk support for builtin server */
#endif
  bool (*set_params_from_actual_params)(Prepared_statement *stmt,
                                        List<Item> &list,
                                        String *expanded_query);
public:
  Prepared_statement(THD *thd_arg);
  virtual ~Prepared_statement();
  void setup_set_params();
  Query_arena::Type type() const override;
  bool cleanup_stmt(bool restore_set_statement_vars) override;
  bool set_name(const LEX_CSTRING *name);
  inline void close_cursor() { delete cursor; cursor= 0; }
  inline bool is_in_use() { return flags & (uint) IS_IN_USE; }
  inline bool is_sql_prepare() const { return flags & (uint) IS_SQL_PREPARE; }
  void set_sql_prepare() { flags|= (uint) IS_SQL_PREPARE; }
  bool prepare(const char *packet, uint packet_length);
  bool execute_loop(String *expanded_query,
                    bool open_cursor,
                    uchar *packet_arg, uchar *packet_end_arg);
  bool execute_bulk_loop(String *expanded_query,
                         bool open_cursor,
                         uchar *packet_arg, uchar *packet_end_arg);
  bool execute_server_runnable(Server_runnable *server_runnable);
  my_bool set_bulk_parameters(bool reset);
  bool bulk_iterations() { return iterations; };
  /* Destroy this statement */
  void deallocate();
  bool execute_immediate(const char *query, uint query_length);
private:
  /**
    The memory root to allocate parsed tree elements (instances of Item,
    SELECT_LEX and other classes).
  */
  MEM_ROOT main_mem_root;
  sql_mode_t m_sql_mode;
private:
  bool set_db(const LEX_CSTRING *db);
  bool set_parameters(String *expanded_query,
                      uchar *packet, uchar *packet_end);
  bool execute(String *expanded_query, bool open_cursor);
  void deallocate_immediate();
  bool reprepare();
  bool validate_metadata(Prepared_statement  *copy);
  void swap_prepared_statement(Prepared_statement *copy);
};

/**
  Execute one SQL statement in an isolated context.
*/

class Execute_sql_statement: public Server_runnable
{
public:
  Execute_sql_statement(LEX_STRING sql_text);
  virtual bool execute_server_code(THD *thd);
private:
  LEX_STRING m_sql_text;
};


class Ed_connection;


/******************************************************************************
  Implementation
******************************************************************************/


inline bool is_param_null(const uchar *pos, ulong param_no)
{
  return pos[param_no/8] & (1 << (param_no & 7));
}

/**
  Find a prepared statement in the statement map by id.

    Try to find a prepared statement and set THD error if it's not found.

  @param thd                thread handle
  @param id                 statement id
  @param where              the place from which this function is called (for
                            error reporting).

  @return
    0 if the statement was not found, a pointer otherwise.
*/

static Prepared_statement *
find_prepared_statement(THD *thd, ulong id)
{
  /*
    To strictly separate namespaces of SQL prepared statements and C API
    prepared statements find() will return 0 if there is a named prepared
    statement with such id.

    LAST_STMT_ID is special value which mean last prepared statement ID
    (it was made for COM_MULTI to allow prepare and execute a statement
    in the same command but usage is not limited by COM_MULTI only).
  */
  Statement *stmt= ((id == LAST_STMT_ID) ?
                    thd->last_stmt :
                    thd->stmt_map.find(id));

  if (stmt == 0 || stmt->type() != Query_arena::PREPARED_STATEMENT)
    return NULL;

  return (Prepared_statement *) stmt;
}


/**
  Send prepared statement id and metadata to the client after prepare.

  @todo
    Fix this nasty upcast from List<Item_param> to List<Item>

  @return
    0 in case of success, 1 otherwise
*/

#ifndef EMBEDDED_LIBRARY
static bool send_prep_stmt(Prepared_statement *stmt, uint columns)
{
  NET *net= &stmt->thd->net;
  uchar buff[12];
  uint tmp;
  int error;
  THD *thd= stmt->thd;
  DBUG_ENTER("send_prep_stmt");
  DBUG_PRINT("enter",("stmt->id: %lu  columns: %d  param_count: %d",
                      stmt->id, columns, stmt->param_count));

  buff[0]= 0;                                   /* OK packet indicator */
  int4store(buff+1, stmt->id);
  int2store(buff+5, columns);
  int2store(buff+7, stmt->param_count);
  buff[9]= 0;                                   // Guard against a 4.1 client
  tmp= MY_MIN(stmt->thd->get_stmt_da()->current_statement_warn_count(), 65535);
  int2store(buff+10, tmp);

  /*
    Send types and names of placeholders to the client
    XXX: fix this nasty upcast from List<Item_param> to List<Item>
  */
  error= my_net_write(net, buff, sizeof(buff));
  if (stmt->param_count && likely(!error))
  {
    /*
      Force the column info to be written
      (in this case PS parameter type info).
    */
    error= thd->protocol_text.send_result_set_metadata(
                (List<Item> *)&stmt->lex->param_list,
                Protocol::SEND_EOF | Protocol::SEND_FORCE_COLUMN_INFO);
  }

  if (likely(!error))
  {
    /* Flag that a response has already been sent */
    thd->get_stmt_da()->disable_status();
  }

  DBUG_RETURN(error);
}
#else
static bool send_prep_stmt(Prepared_statement *stmt,
                           uint columns __attribute__((unused)))
{
  THD *thd= stmt->thd;

  thd->client_stmt_id= stmt->id;
  thd->client_param_count= stmt->param_count;
  thd->clear_error();
  thd->get_stmt_da()->disable_status();

  return 0;
}
#endif /*!EMBEDDED_LIBRARY*/


#ifndef EMBEDDED_LIBRARY

/**
  Read the length of the parameter data and return it back to
  the caller.

    Read data length, position the packet to the first byte after it,
    and return the length to the caller.

  @param packet             a pointer to the data
  @param len                remaining packet length

  @return
    Length of data piece.
*/

static ulong get_param_length(uchar **packet, ulong len)
{
  uchar *pos= *packet;
  if (len < 1)
    return 0;
  if (*pos < 251)
  {
    (*packet)++;
    return (ulong) *pos;
  }
  if (len < 3)
    return 0;
  if (*pos == 252)
  {
    (*packet)+=3;
    return (ulong) uint2korr(pos+1);
  }
  if (len < 4)
    return 0;
  if (*pos == 253)
  {
    (*packet)+=4;
    return (ulong) uint3korr(pos+1);
  }
  if (len < 5)
    return 0;
  (*packet)+=9; // Must be 254 when here
  /*
    In our client-server protocol all numbers bigger than 2^24
    stored as 8 bytes with uint8korr. Here we always know that
    parameter length is less than 2^4 so don't look at the second
    4 bytes. But still we need to obey the protocol hence 9 in the
    assignment above.
  */
  return (ulong) uint4korr(pos+1);
}
#else
#define get_param_length(packet, len) len
#endif /*!EMBEDDED_LIBRARY*/

/**
  Data conversion routines.

    All these functions read the data from pos, convert it to requested
    type and assign to param; pos is advanced to predefined length.

    Make a note that the NULL handling is examined at first execution
    (i.e. when input types altered) and for all subsequent executions
    we don't read any values for this.

  @param  pos               input data buffer
  @param  len               length of data in the buffer
*/

void Item_param::set_param_tiny(uchar **pos, ulong len)
{
#ifndef EMBEDDED_LIBRARY
  if (len < 1)
    return;
#endif
  int8 value= (int8) **pos;
  set_int(unsigned_flag ? (longlong) ((uint8) value) :
                          (longlong) value, 4);
  *pos+= 1;
}

void Item_param::set_param_short(uchar **pos, ulong len)
{
  int16 value;
#ifndef EMBEDDED_LIBRARY
  if (len < 2)
    return;
  value= sint2korr(*pos);
#else
  shortget(value, *pos);
#endif
  set_int(unsigned_flag ? (longlong) ((uint16) value) :
                          (longlong) value, 6);
  *pos+= 2;
}

void Item_param::set_param_int32(uchar **pos, ulong len)
{
  int32 value;
#ifndef EMBEDDED_LIBRARY
  if (len < 4)
    return;
  value= sint4korr(*pos);
#else
  longget(value, *pos);
#endif
  set_int(unsigned_flag ? (longlong) ((uint32) value) :
                          (longlong) value, 11);
  *pos+= 4;
}

void Item_param::set_param_int64(uchar **pos, ulong len)
{
  longlong value;
#ifndef EMBEDDED_LIBRARY
  if (len < 8)
    return;
  value= (longlong) sint8korr(*pos);
#else
  longlongget(value, *pos);
#endif
  set_int(value, 21);
  *pos+= 8;
}

void Item_param::set_param_float(uchar **pos, ulong len)
{
  float data;
#ifndef EMBEDDED_LIBRARY
  if (len < 4)
    return;
  float4get(data,*pos);
#else
  floatget(data, *pos);
#endif
  set_double((double) data);
  *pos+= 4;
}

void Item_param::set_param_double(uchar **pos, ulong len)
{
  double data;
#ifndef EMBEDDED_LIBRARY
  if (len < 8)
    return;
  float8get(data,*pos);
#else
  doubleget(data, *pos);
#endif
  set_double((double) data);
  *pos+= 8;
}

void Item_param::set_param_decimal(uchar **pos, ulong len)
{
  ulong length= get_param_length(pos, len);
  set_decimal((char*)*pos, length);
  *pos+= length;
}

#ifndef EMBEDDED_LIBRARY

/*
  Read date/time/datetime parameter values from network (binary
  protocol). See writing counterparts of these functions in
  libmysql.c (store_param_{time,date,datetime}).
*/

/**
  @todo
    Add warning 'Data truncated' here
*/
void Item_param::set_param_time(uchar **pos, ulong len)
{
  MYSQL_TIME tm;
  ulong length= get_param_length(pos, len);

  if (length >= 8)
  {
    uchar *to= *pos;
    uint day;

    tm.neg= (bool) to[0];
    day= (uint) sint4korr(to+1);
    tm.hour=   (uint) to[5] + day * 24;
    tm.minute= (uint) to[6];
    tm.second= (uint) to[7];
    tm.second_part= (length > 8) ? (ulong) sint4korr(to+8) : 0;
    if (tm.hour > 838)
    {
      /* TODO: add warning 'Data truncated' here */
      tm.hour= 838;
      tm.minute= 59;
      tm.second= 59;
    }
    tm.day= tm.year= tm.month= 0;
  }
  else
    set_zero_time(&tm, MYSQL_TIMESTAMP_TIME);
  set_time(&tm, MYSQL_TIMESTAMP_TIME, MAX_TIME_FULL_WIDTH);
  *pos+= length;
}

void Item_param::set_param_datetime(uchar **pos, ulong len)
{
  MYSQL_TIME tm;
  ulong length= get_param_length(pos, len);

  if (length >= 4)
  {
    uchar *to= *pos;

    tm.neg=    0;
    tm.year=   (uint) sint2korr(to);
    tm.month=  (uint) to[2];
    tm.day=    (uint) to[3];
    if (length > 4)
    {
      tm.hour=   (uint) to[4];
      tm.minute= (uint) to[5];
      tm.second= (uint) to[6];
    }
    else
      tm.hour= tm.minute= tm.second= 0;

    tm.second_part= (length > 7) ? (ulong) sint4korr(to+7) : 0;
  }
  else
    set_zero_time(&tm, MYSQL_TIMESTAMP_DATETIME);
  set_time(&tm, MYSQL_TIMESTAMP_DATETIME, MAX_DATETIME_WIDTH);
  *pos+= length;
}


void Item_param::set_param_date(uchar **pos, ulong len)
{
  MYSQL_TIME tm;
  ulong length= get_param_length(pos, len);

  if (length >= 4)
  {
    uchar *to= *pos;

    tm.year=  (uint) sint2korr(to);
    tm.month=  (uint) to[2];
    tm.day= (uint) to[3];

    tm.hour= tm.minute= tm.second= 0;
    tm.second_part= 0;
    tm.neg= 0;
  }
  else
    set_zero_time(&tm, MYSQL_TIMESTAMP_DATE);
  set_time(&tm, MYSQL_TIMESTAMP_DATE, MAX_DATE_WIDTH);
  *pos+= length;
}

#else/*!EMBEDDED_LIBRARY*/
/**
  @todo
    Add warning 'Data truncated' here
*/
void Item_param::set_param_time(uchar **pos, ulong len)
{
  MYSQL_TIME tm= *((MYSQL_TIME*)*pos);
  tm.hour+= tm.day * 24;
  tm.day= tm.year= tm.month= 0;
  if (tm.hour > 838)
  {
    /* TODO: add warning 'Data truncated' here */
    tm.hour= 838;
    tm.minute= 59;
    tm.second= 59;
  }
  set_time(&tm, MYSQL_TIMESTAMP_TIME, MAX_TIME_WIDTH);
}

void Item_param::set_param_datetime(uchar **pos, ulong len)
{
  MYSQL_TIME tm= *((MYSQL_TIME*)*pos);
  tm.neg= 0;
  set_time(&tm, MYSQL_TIMESTAMP_DATETIME, MAX_DATETIME_WIDTH);
}

void Item_param::set_param_date(uchar **pos, ulong len)
{
  MYSQL_TIME *to= (MYSQL_TIME*)*pos;
  set_time(to, MYSQL_TIMESTAMP_DATE, MAX_DATE_WIDTH);
}
#endif /*!EMBEDDED_LIBRARY*/


void Item_param::set_param_str(uchar **pos, ulong len)
{
  ulong length= get_param_length(pos, len);
  if (length == 0 && m_empty_string_is_null)
    set_null();
  else
  {
    if (length > len)
      length= len;
    /*
      We use &my_charset_bin here. Conversion and setting real character
      sets will be done in Item_param::convert_str_value(), after the
      original value is appended to the query used for logging.
    */
    set_str((const char *) *pos, length, &my_charset_bin, &my_charset_bin);
    *pos+= length;
  }
}


#undef get_param_length


void Item_param::setup_conversion(THD *thd, uchar param_type)
{
  const Type_handler *h=
    Type_handler::get_handler_by_field_type((enum_field_types) param_type);
  /*
    The client library ensures that we won't get any unexpected typecodes
    in the bound parameter. Translating unknown typecodes to
    &type_handler_string lets us to handle malformed packets as well.
  */
  if (!h)
    h= &type_handler_string;
  else if (unsigned_flag)
    h= h->type_handler_unsigned();
  set_handler(h);
  h->Item_param_setup_conversion(thd, this);
}


void Item_param::setup_conversion_blob(THD *thd)
{
  value.cs_info.character_set_of_placeholder= &my_charset_bin;
  value.cs_info.character_set_client= thd->variables.character_set_client;
  DBUG_ASSERT(thd->variables.character_set_client);
  value.cs_info.final_character_set_of_str_value= &my_charset_bin;
  m_empty_string_is_null= thd->variables.sql_mode & MODE_EMPTY_STRING_IS_NULL;
}


void Item_param::setup_conversion_string(THD *thd, CHARSET_INFO *fromcs)
{
  value.cs_info.set(thd, fromcs);
  m_empty_string_is_null= thd->variables.sql_mode & MODE_EMPTY_STRING_IS_NULL;
  /*
    Exact value of max_length is not known unless data is converted to
    charset of connection, so we have to set it later.
  */
}

#ifndef EMBEDDED_LIBRARY

/**
  Routines to assign parameters from data supplied by the client.

    Update the parameter markers by reading data from the packet and
    and generate a valid query for logging.

  @note
    This function, along with other _with_log functions is called when one of
    binary, slow or general logs is open. Logging of prepared statements in
    all cases is performed by means of conventional queries: if parameter
    data was supplied from C API, each placeholder in the query is
    replaced with its actual value; if we're logging a [Dynamic] SQL
    prepared statement, parameter markers are replaced with variable names.
    Example:
    @verbatim
     mysqld_stmt_prepare("UPDATE t1 SET a=a*1.25 WHERE a=?")
       --> general logs gets [Prepare] UPDATE t1 SET a*1.25 WHERE a=?"
     mysqld_stmt_execute(stmt);
       --> general and binary logs get
                             [Execute] UPDATE t1 SET a*1.25 WHERE a=1"
    @endverbatim

    If a statement has been prepared using SQL syntax:
    @verbatim
     PREPARE stmt FROM "UPDATE t1 SET a=a*1.25 WHERE a=?"
       --> general log gets
                                 [Query]   PREPARE stmt FROM "UPDATE ..."
     EXECUTE stmt USING @a
       --> general log gets
                             [Query]   EXECUTE stmt USING @a;
    @endverbatim

  @retval
    0  if success
  @retval
    1  otherwise
*/

static bool insert_params_with_log(Prepared_statement *stmt, uchar *null_array,
                                   uchar *read_pos, uchar *data_end,
                                   String *query)
{
  THD  *thd= stmt->thd;
  Item_param **begin= stmt->param_array;
  Item_param **end= begin + stmt->param_count;
  Copy_query_with_rewrite acc(thd, stmt->query(), stmt->query_length(), query);
  DBUG_ENTER("insert_params_with_log");

  for (Item_param **it= begin; it < end; ++it)
  {
    Item_param *param= *it;
    if (!param->has_long_data_value())
    {
      if (is_param_null(null_array, (uint) (it - begin)))
        param->set_null();
      else
      {
        if (read_pos >= data_end)
          DBUG_RETURN(1);
        param->set_param_func(&read_pos, (uint) (data_end - read_pos));
        if (param->has_no_value())
          DBUG_RETURN(1);

        if (param->limit_clause_param && !param->has_int_value())
        {
          if (param->set_limit_clause_param(param->val_int()))
            DBUG_RETURN(1);
        }
      }
    }
    /*
      A long data stream was supplied for this parameter marker.
      This was done after prepare, prior to providing a placeholder
      type (the types are supplied at execute). Check that the
      supplied type of placeholder can accept a data stream.
    */
    else if (!param->type_handler()->is_param_long_data_type())
      DBUG_RETURN(1);

    if (acc.append(param))
      DBUG_RETURN(1);

    if (param->convert_str_value(thd))
      DBUG_RETURN(1);                           /* out of memory */

    param->sync_clones();
  }
  if (acc.finalize())
    DBUG_RETURN(1);

  DBUG_RETURN(0);
}


static bool insert_params(Prepared_statement *stmt, uchar *null_array,
                          uchar *read_pos, uchar *data_end,
                          String *expanded_query)
{
  Item_param **begin= stmt->param_array;
  Item_param **end= begin + stmt->param_count;

  DBUG_ENTER("insert_params");

  for (Item_param **it= begin; it < end; ++it)
  {
    Item_param *param= *it;
    param->indicator= STMT_INDICATOR_NONE; // only for bulk parameters
    if (!param->has_long_data_value())
    {
      if (is_param_null(null_array, (uint) (it - begin)))
        param->set_null();
      else
      {
        if (read_pos >= data_end)
          DBUG_RETURN(1);
        param->set_param_func(&read_pos, (uint) (data_end - read_pos));
        if (param->has_no_value())
          DBUG_RETURN(1);
      }
    }
    /*
      A long data stream was supplied for this parameter marker.
      This was done after prepare, prior to providing a placeholder
      type (the types are supplied at execute). Check that the
      supplied type of placeholder can accept a data stream.
    */
    else if (!param->type_handler()->is_param_long_data_type())
      DBUG_RETURN(1);
    if (param->convert_str_value(stmt->thd))
      DBUG_RETURN(1);                           /* out of memory */
    param->sync_clones();
  }
  DBUG_RETURN(0);
}


static bool insert_bulk_params(Prepared_statement *stmt,
                               uchar **read_pos, uchar *data_end,
                               bool reset)
{
  Item_param **begin= stmt->param_array;
  Item_param **end= begin + stmt->param_count;

  DBUG_ENTER("insert_params");

  for (Item_param **it= begin; it < end; ++it)
  {
    Item_param *param= *it;
    if (reset)
      param->reset();
    if (!param->has_long_data_value())
    {
      param->indicator= (enum_indicator_type) *((*read_pos)++);
      if ((*read_pos) > data_end)
        DBUG_RETURN(1);
      switch (param->indicator)
      {
      case STMT_INDICATOR_NONE:
        if ((*read_pos) >= data_end)
          DBUG_RETURN(1);
        param->set_param_func(read_pos, (uint) (data_end - (*read_pos)));
        if (param->has_no_value())
          DBUG_RETURN(1);
        if (param->convert_str_value(stmt->thd))
          DBUG_RETURN(1);                           /* out of memory */
        break;
      case STMT_INDICATOR_NULL:
        param->set_null();
        break;
      case STMT_INDICATOR_DEFAULT:
        param->set_default();
        break;
      case STMT_INDICATOR_IGNORE:
        param->set_ignore();
        break;
      default:
        DBUG_ASSERT(0);
        DBUG_RETURN(1);
      }
    }
    else
      DBUG_RETURN(1); // long is not supported here
    param->sync_clones();
  }
  DBUG_RETURN(0);
}


/**
  Checking if parameter type and flags are valid

  @param typecode  ushort value with type in low byte, and flags in high byte

  @retval true  this parameter is wrong
  @retval false this parameter is OK
*/

static bool
parameter_type_sanity_check(ushort typecode)
{
  /* Checking if type in lower byte is valid */
  switch (typecode & 0xff) {
  case MYSQL_TYPE_DECIMAL:
  case MYSQL_TYPE_NEWDECIMAL:
  case MYSQL_TYPE_TINY:
  case MYSQL_TYPE_SHORT:
  case MYSQL_TYPE_LONG:
  case MYSQL_TYPE_LONGLONG:
  case MYSQL_TYPE_INT24:
  case MYSQL_TYPE_YEAR:
  case MYSQL_TYPE_BIT:
  case MYSQL_TYPE_FLOAT:
  case MYSQL_TYPE_DOUBLE:
  case MYSQL_TYPE_NULL:
  case MYSQL_TYPE_VARCHAR:
  case MYSQL_TYPE_TINY_BLOB:
  case MYSQL_TYPE_MEDIUM_BLOB:
  case MYSQL_TYPE_LONG_BLOB:
  case MYSQL_TYPE_BLOB:
  case MYSQL_TYPE_VAR_STRING:
  case MYSQL_TYPE_STRING:
  case MYSQL_TYPE_ENUM:
  case MYSQL_TYPE_SET:
  case MYSQL_TYPE_GEOMETRY:
  case MYSQL_TYPE_TIMESTAMP:
  case MYSQL_TYPE_DATE:
  case MYSQL_TYPE_TIME:
  case MYSQL_TYPE_DATETIME:
  case MYSQL_TYPE_NEWDATE:
  break;
  /*
    This types normally cannot be sent by client, so maybe it'd be
    better to treat them like an error here.
  */
  case MYSQL_TYPE_TIMESTAMP2:
  case MYSQL_TYPE_TIME2:
  case MYSQL_TYPE_DATETIME2:
  default:
    return true;
  };

  // In Flags in high byte only unsigned bit may be set
  if (typecode & ((~PARAMETER_FLAG_UNSIGNED) & 0x0000ff00))
  {
    return true;
  }
  return false;
}

static bool
set_conversion_functions(Prepared_statement *stmt, uchar **data)
{
  uchar *read_pos= *data;

  DBUG_ENTER("set_conversion_functions");
  /*
     First execute or types altered by the client, setup the
     conversion routines for all parameters (one time)
   */
  Item_param **it= stmt->param_array;
  Item_param **end= it + stmt->param_count;
  THD *thd= stmt->thd;
  for (; it < end; ++it)
  {
    ushort typecode;

    /*
      stmt_execute_packet_sanity_check has already verified, that there
      are enough data in the packet for data types
    */
    typecode= sint2korr(read_pos);
    read_pos+= 2;
    if (parameter_type_sanity_check(typecode))
    {
      DBUG_RETURN(1);
    }
    (**it).unsigned_flag= MY_TEST(typecode & PARAMETER_FLAG_UNSIGNED);
    (*it)->setup_conversion(thd, (uchar) (typecode & 0xff));
    (*it)->sync_clones();
  }
  *data= read_pos;
  DBUG_RETURN(0);
}


static bool setup_conversion_functions(Prepared_statement *stmt,
                                       uchar **data,
                                       bool bulk_protocol= 0)
{
  /* skip null bits */
  uchar *read_pos= *data;
  if (!bulk_protocol)
    read_pos+= (stmt->param_count+7) / 8;

  DBUG_ENTER("setup_conversion_functions");

  if (*read_pos++) //types supplied / first execute
  {
    *data= read_pos;
    bool res= set_conversion_functions(stmt, data);
    DBUG_RETURN(res);
  }
  *data= read_pos;
  DBUG_RETURN(0);
}

#else

//TODO: support bulk parameters

/**
  Embedded counterparts of parameter assignment routines.

    The main difference between the embedded library and the server is
    that in embedded case we don't serialize/deserialize parameters data.

    Additionally, for unknown reason, the client-side flag raised for
    changed types of placeholders is ignored and we simply setup conversion
    functions at each execute (TODO: fix).
*/

static bool emb_insert_params(Prepared_statement *stmt, String *expanded_query)
{
  THD *thd= stmt->thd;
  Item_param **it= stmt->param_array;
  Item_param **end= it + stmt->param_count;
  MYSQL_BIND *client_param= stmt->thd->client_params;

  DBUG_ENTER("emb_insert_params");

  for (; it < end; ++it, ++client_param)
  {
    Item_param *param= *it;
    param->setup_conversion(thd, client_param->buffer_type);
    if (!param->has_long_data_value())
    {
      if (*client_param->is_null)
        param->set_null();
      else
      {
        uchar *buff= (uchar*) client_param->buffer;
        param->unsigned_flag= client_param->is_unsigned;
        param->set_param_func(&buff,
                              client_param->length ?
                              *client_param->length :
                              client_param->buffer_length);
        if (param->has_no_value())
          DBUG_RETURN(1);
      }
      param->sync_clones();
    }
    if (param->convert_str_value(thd))
      DBUG_RETURN(1);                           /* out of memory */
  }
  DBUG_RETURN(0);
}


static bool emb_insert_params_with_log(Prepared_statement *stmt, String *query)
{
  THD *thd= stmt->thd;
  Item_param **it= stmt->param_array;
  Item_param **end= it + stmt->param_count;
  MYSQL_BIND *client_param= thd->client_params;
  Copy_query_with_rewrite acc(thd, stmt->query(), stmt->query_length(), query);
  DBUG_ENTER("emb_insert_params_with_log");

  for (; it < end; ++it, ++client_param)
  {
    Item_param *param= *it;
    param->setup_conversion(thd, client_param->buffer_type);
    if (!param->has_long_data_value())
    {
      if (*client_param->is_null)
        param->set_null();
      else
      {
        uchar *buff= (uchar*)client_param->buffer;
        param->unsigned_flag= client_param->is_unsigned;
        param->set_param_func(&buff,
                              client_param->length ?
                              *client_param->length :
                              client_param->buffer_length);
        if (param->has_no_value())
          DBUG_RETURN(1);
      }
    }
    if (acc.append(param))
      DBUG_RETURN(1);

    if (param->convert_str_value(thd))
      DBUG_RETURN(1);                           /* out of memory */
    param->sync_clones();
  }
  if (acc.finalize())
    DBUG_RETURN(1);

  DBUG_RETURN(0);
}

#endif /*!EMBEDDED_LIBRARY*/

/**
  Setup data conversion routines using an array of parameter
  markers from the original prepared statement.
  Swap the parameter data of the original prepared
  statement to the new one.

  Used only when we re-prepare a prepared statement.
  There are two reasons for this function to exist:

  1) In the binary client/server protocol, parameter metadata
  is sent only at first execute. Consequently, if we need to
  reprepare a prepared statement at a subsequent execution,
  we may not have metadata information in the packet.
  In that case we use the parameter array of the original
  prepared statement to setup parameter types of the new
  prepared statement.

  2) In the binary client/server protocol, we may supply
  long data in pieces. When the last piece is supplied,
  we assemble the pieces and convert them from client
  character set to the connection character set. After
  that the parameter value is only available inside
  the parameter, the original pieces are lost, and thus
  we can only assign the corresponding parameter of the
  reprepared statement from the original value.

  @param[out]  param_array_dst  parameter markers of the new statement
  @param[in]   param_array_src  parameter markers of the original
                                statement
  @param[in]   param_count      total number of parameters. Is the
                                same in src and dst arrays, since
                                the statement query is the same

  @return this function never fails
*/

static void
swap_parameter_array(Item_param **param_array_dst,
                     Item_param **param_array_src,
                     uint param_count)
{
  Item_param **dst= param_array_dst;
  Item_param **src= param_array_src;
  Item_param **end= param_array_dst + param_count;

  for (; dst < end; ++src, ++dst)
  {
    (*dst)->set_param_type_and_swap_value(*src);
    (*dst)->sync_clones();
    (*src)->sync_clones();
  }
}


/**
  Assign prepared statement parameters from user variables.

  @param stmt      Statement
  @param params    A list of parameters. Caller must ensure that number
                   of parameters in the list is equal to number of statement
                   parameters
  @param query     Ignored
*/

static bool
insert_params_from_actual_params(Prepared_statement *stmt,
                                 List<Item> &params,
                                 String *query __attribute__((unused)))
{
  Item_param **begin= stmt->param_array;
  Item_param **end= begin + stmt->param_count;
  List_iterator<Item> param_it(params);
  DBUG_ENTER("insert_params_from_actual_params");

  for (Item_param **it= begin; it < end; ++it)
  {
    Item_param *param= *it;
    Item *ps_param= param_it++;
    if (ps_param->save_in_param(stmt->thd, param) ||
        param->convert_str_value(stmt->thd))
      DBUG_RETURN(1);
    param->sync_clones();
  }
  DBUG_RETURN(0);
}


/**
  Do the same as insert_params_from_actual_params
  but also construct query text for binary log.

  @param stmt      Prepared statement
  @param params    A list of parameters. Caller must ensure that number of
                   parameters in the list is equal to number of statement
                   parameters
  @param query     The query with parameter markers replaced with corresponding
                   user variables that were used to execute the query.
*/

static bool
insert_params_from_actual_params_with_log(Prepared_statement *stmt,
                                          List<Item> &params,
                                          String *query)
{
  Item_param **begin= stmt->param_array;
  Item_param **end= begin + stmt->param_count;
  List_iterator<Item> param_it(params);
  THD *thd= stmt->thd;
  Copy_query_with_rewrite acc(thd, stmt->query(), stmt->query_length(), query);

  DBUG_ENTER("insert_params_from_actual_params_with_log");

  for (Item_param **it= begin; it < end; ++it)
  {
    Item_param *param= *it;
    Item *ps_param= param_it++;
    if (ps_param->save_in_param(thd, param))
      DBUG_RETURN(1);

    if (acc.append(param))
      DBUG_RETURN(1);

    if (param->convert_str_value(thd))
      DBUG_RETURN(1);

    param->sync_clones();
  }
  if (acc.finalize())
    DBUG_RETURN(1);

  DBUG_RETURN(0);
}


/*
  Validate INSERT statement.

  @param stmt               prepared statement
  @param table_list         global/local table list
  @param fields             list of the table's fields to insert values
  @param values_list        values to be inserted into the table
  @param update_fields      the update fields.
  @param update_values      the update values.
  @param duplic             a way to handle duplicates

  @retval
    FALSE             success
  @retval
    TRUE              error, error message is set in THD
*/

static bool mysql_test_insert_common(Prepared_statement *stmt,
                                     TABLE_LIST *table_list,
                                     List<Item> &fields,
                                     List<List_item> &values_list,
                                     List<Item> &update_fields,
                                     List<Item> &update_values,
                                     enum_duplicates duplic)
{
  THD *thd= stmt->thd;
  List_iterator_fast<List_item> its(values_list);
  List_item *values;
  DBUG_ENTER("mysql_test_insert_common");

  if (insert_precheck(thd, table_list))
    goto error;

  //upgrade_lock_type_for_insert(thd, &table_list->lock_type, duplic,
  //                             values_list.elements > 1);
  /*
    open temporary memory pool for temporary data allocated by derived
    tables & preparation procedure
    Note that this is done without locks (should not be needed as we will not
    access any data here)
    If we would use locks, then we have to ensure we are not using
    TL_WRITE_DELAYED as having two such locks can cause table corruption.
  */
  if (open_normal_and_derived_tables(thd, table_list,
                                     MYSQL_OPEN_FORCE_SHARED_MDL, DT_INIT))
    goto error;

  if ((values= its++))
  {
    uint value_count;
    Item *unused_conds= 0;

    if (table_list->table)
    {
      // don't allocate insert_values
      table_list->table->insert_values=(uchar *)1;
    }

    if (mysql_prepare_insert(thd, table_list, fields, values, update_fields,
                             update_values, duplic, &unused_conds, FALSE))
      goto error;

    value_count= values->elements;
    its.rewind();

    if (table_list->lock_type == TL_WRITE_DELAYED &&
        !(table_list->table->file->ha_table_flags() & HA_CAN_INSERT_DELAYED))
    {
      my_error(ER_DELAYED_NOT_SUPPORTED, MYF(0), (table_list->view ?
                                                  table_list->view_name.str :
                                                  table_list->table_name.str));
      goto error;
    }
    while ((values= its++))
    {
      if (values->elements != value_count)
      {
        my_error(ER_WRONG_VALUE_COUNT_ON_ROW, MYF(0),
                 thd->get_stmt_da()->current_row_for_warning());
        goto error;
      }
      if (setup_fields(thd, Ref_ptr_array(),
                       *values, COLUMNS_READ, 0, NULL, 0))
        goto error;
      thd->get_stmt_da()->inc_current_row_for_warning();
    }
    thd->get_stmt_da()->reset_current_row_for_warning(1);
  }
  DBUG_RETURN(FALSE);

error:
  /* insert_values is cleared in open_table */
  DBUG_RETURN(TRUE);
}


/**
  Open temporary tables if required and validate INSERT statement.

  @param stmt               prepared statement
  @param tables             global/local table list

  @retval
    FALSE             success
  @retval
    TRUE              error, error message is set in THD
*/

static bool mysql_test_insert(Prepared_statement *stmt,
                              TABLE_LIST *table_list,
                              List<Item> &fields,
                              List<List_item> &values_list,
                              List<Item> &update_fields,
                              List<Item> &update_values,
                              enum_duplicates duplic)
{
  THD *thd= stmt->thd;

  /*
    Since INSERT DELAYED doesn't support temporary tables, we could
    not pre-open temporary tables for SQLCOM_INSERT / SQLCOM_REPLACE.
    Open them here instead.
  */
  if (table_list->lock_type != TL_WRITE_DELAYED)
  {
    if (thd->open_temporary_tables(table_list))
      return true;
  }

  return mysql_test_insert_common(stmt, table_list, fields, values_list,
                                  update_fields, update_values, duplic);
}


/**
  Validate UPDATE statement.

  @param stmt               prepared statement
  @param tables             list of tables used in this query

  @todo
    - here we should send types of placeholders to the client.

  @retval
    0                 success
  @retval
    1                 error, error message is set in THD
  @retval
    2                 convert to multi_update
*/

static int mysql_test_update(Prepared_statement *stmt,
                              TABLE_LIST *table_list)
{
  int res;
  THD *thd= stmt->thd;
  uint table_count= 0;
  TABLE_LIST *update_source_table;
  SELECT_LEX *select= stmt->lex->first_select_lex();
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  privilege_t want_privilege(NO_ACL);
#endif
  DBUG_ENTER("mysql_test_update");

  if (update_precheck(thd, table_list) ||
      open_tables(thd, &table_list, &table_count, MYSQL_OPEN_FORCE_SHARED_MDL))
    goto error;

  if (mysql_handle_derived(thd->lex, DT_INIT))
    goto error;

  if (((update_source_table= unique_table(thd, table_list,
                                          table_list->next_global, 0)) ||
        table_list->is_multitable()))
  {
    DBUG_ASSERT(update_source_table || table_list->view != 0);
    DBUG_PRINT("info", ("Switch to multi-update"));
    /* pass counter value */
    thd->lex->table_count= table_count;
    /* convert to multiupdate */
    DBUG_RETURN(2);
  }

  /*
    thd->fill_derived_tables() is false here for sure (because it is
    preparation of PS, so we even do not check it).
  */
  if (table_list->handle_derived(thd->lex, DT_MERGE_FOR_INSERT))
    goto error;
  if (table_list->handle_derived(thd->lex, DT_PREPARE))
    goto error;

  if (!table_list->single_table_updatable())
  {
    my_error(ER_NON_UPDATABLE_TABLE, MYF(0), table_list->alias.str, "UPDATE");
    goto error;
  }

#ifndef NO_EMBEDDED_ACCESS_CHECKS
  /* Force privilege re-checking for views after they have been opened. */
  want_privilege= (table_list->view ? UPDATE_ACL :
                   table_list->grant.want_privilege);
#endif

  if (mysql_prepare_update(thd, table_list, &select->where,
                           select->order_list.elements,
                           select->order_list.first))
    goto error;

#ifndef NO_EMBEDDED_ACCESS_CHECKS
  table_list->grant.want_privilege= want_privilege;
  table_list->table->grant.want_privilege= want_privilege;
  table_list->register_want_access(want_privilege);
#endif
  thd->lex->first_select_lex()->no_wrap_view_item= TRUE;
  res= setup_fields(thd, Ref_ptr_array(),
                    select->item_list, MARK_COLUMNS_READ, 0, NULL, 0);
  thd->lex->first_select_lex()->no_wrap_view_item= FALSE;
  if (res)
    goto error;
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  /* Check values */
  table_list->grant.want_privilege=
  table_list->table->grant.want_privilege=
    (SELECT_ACL & ~table_list->table->grant.privilege);
  table_list->register_want_access(SELECT_ACL);
#endif
  if (setup_fields(thd, Ref_ptr_array(),
                   stmt->lex->value_list, COLUMNS_READ, 0, NULL, 0) ||
      check_unique_table(thd, table_list))
    goto error;
  /* TODO: here we should send types of placeholders to the client. */
  DBUG_RETURN(0);
error:
  DBUG_RETURN(1);
}


/**
  Validate DELETE statement.

  @param stmt               prepared statement
  @param tables             list of tables used in this query

  @retval
    FALSE             success
  @retval
    TRUE              error, error message is set in THD
*/

static bool mysql_test_delete(Prepared_statement *stmt,
                              TABLE_LIST *table_list)
{
  uint table_count= 0;
  THD *thd= stmt->thd;
  LEX *lex= stmt->lex;
  bool delete_while_scanning;
  DBUG_ENTER("mysql_test_delete");

  if (delete_precheck(thd, table_list) ||
      open_tables(thd, &table_list, &table_count, MYSQL_OPEN_FORCE_SHARED_MDL))
    goto error;

  if (mysql_handle_derived(thd->lex, DT_INIT))
    goto error;
  if (mysql_handle_derived(thd->lex, DT_MERGE_FOR_INSERT))
    goto error;
  if (mysql_handle_derived(thd->lex, DT_PREPARE))
    goto error;

  if (!table_list->single_table_updatable())
  {
    my_error(ER_NON_UPDATABLE_TABLE, MYF(0), table_list->alias.str, "DELETE");
    goto error;
  }
  if (!table_list->table || !table_list->table->is_created())
  {
    my_error(ER_VIEW_DELETE_MERGE_VIEW, MYF(0),
             table_list->view_db.str, table_list->view_name.str);
    goto error;
  }

  DBUG_RETURN(mysql_prepare_delete(thd, table_list,
                                   &lex->first_select_lex()->where,
                                   &delete_while_scanning));
error:
  DBUG_RETURN(TRUE);
}


/**
  Validate SELECT statement.

    In case of success, if this query is not EXPLAIN, send column list info
    back to the client.

  @param stmt               prepared statement
  @param tables             list of tables used in the query

  @retval
    0                 success
  @retval
    1                 error, error message is set in THD
  @retval
    2                 success, and statement metadata has been sent
*/

static int mysql_test_select(Prepared_statement *stmt,
                             TABLE_LIST *tables)
{
  THD *thd= stmt->thd;
  LEX *lex= stmt->lex;
  SELECT_LEX_UNIT *unit= &lex->unit;
  DBUG_ENTER("mysql_test_select");

  lex->first_select_lex()->context.resolve_in_select_list= TRUE;

  privilege_t privilege(lex->exchange ? SELECT_ACL | FILE_ACL : SELECT_ACL);
  if (tables)
  {
    if (check_table_access(thd, privilege, tables, FALSE, UINT_MAX, FALSE))
      goto error;
  }
  else if (check_access(thd, privilege, any_db.str, NULL, NULL, 0, 0))
    goto error;

  if (!lex->result && !(lex->result= new (stmt->mem_root) select_send(thd)))
  {
    my_error(ER_OUTOFMEMORY, MYF(ME_FATAL),
             static_cast<int>(sizeof(select_send)));
    goto error;
  }

  if (open_normal_and_derived_tables(thd, tables,  MYSQL_OPEN_FORCE_SHARED_MDL,
                                     DT_INIT | DT_PREPARE))
    goto error;

  thd->lex->used_tables= 0;                        // Updated by setup_fields

  /*
    JOIN::prepare calls
    It is not SELECT COMMAND for sure, so setup_tables will be called as
    usual, and we pass 0 as setup_tables_done_option
  */
  if (unit->prepare(unit->derived, 0, 0))
    goto error;
  if (!lex->describe && !thd->lex->analyze_stmt && !stmt->is_sql_prepare())
  {
    /* Make copy of item list, as change_columns may change it */
    SELECT_LEX_UNIT* master_unit= unit->first_select()->master_unit();
    bool is_union_op=
      master_unit->is_unit_op() || master_unit->fake_select_lex;

    List<Item> fields(is_union_op ? unit->item_list :
                                    lex->first_select_lex()->item_list);

    /* Change columns if a procedure like analyse() */
    if (unit->last_procedure && unit->last_procedure->change_columns(thd, fields))
      goto error;

    /*
      We can use lex->result as it should've been prepared in
      unit->prepare call above.
    */
    if (send_prep_stmt(stmt, lex->result->field_count(fields)) ||
        lex->result->send_result_set_metadata(fields, Protocol::SEND_EOF) ||
        thd->protocol->flush())
      goto error;
    DBUG_RETURN(2);
  }
  DBUG_RETURN(0);
error:
  DBUG_RETURN(1);
}


/**
  Validate and prepare for execution DO statement expressions.

  @param stmt               prepared statement
  @param tables             list of tables used in this query
  @param values             list of expressions

  @retval
    FALSE             success
  @retval
    TRUE              error, error message is set in THD
*/

static bool mysql_test_do_fields(Prepared_statement *stmt,
                                TABLE_LIST *tables,
                                List<Item> *values)
{
  THD *thd= stmt->thd;

  DBUG_ENTER("mysql_test_do_fields");
  if (tables && check_table_access(thd, SELECT_ACL, tables, FALSE,
                                   UINT_MAX, FALSE))
    DBUG_RETURN(TRUE);

  if (open_normal_and_derived_tables(thd, tables, MYSQL_OPEN_FORCE_SHARED_MDL,
                                     DT_INIT | DT_PREPARE))
    DBUG_RETURN(TRUE);
  DBUG_RETURN(setup_fields(thd, Ref_ptr_array(),
                           *values, COLUMNS_READ, 0, NULL, 0));
}


/**
  Validate and prepare for execution SET statement expressions.

  @param stmt               prepared statement
  @param tables             list of tables used in this query
  @param values             list of expressions

  @retval
    FALSE             success
  @retval
    TRUE              error, error message is set in THD
*/

static bool mysql_test_set_fields(Prepared_statement *stmt,
                                  TABLE_LIST *tables,
                                  List<set_var_base> *var_list)
{
  DBUG_ENTER("mysql_test_set_fields");
  List_iterator_fast<set_var_base> it(*var_list);
  THD *thd= stmt->thd;
  set_var_base *var;

  if ((tables &&
       check_table_access(thd, SELECT_ACL, tables, FALSE, UINT_MAX, FALSE)) ||
      open_normal_and_derived_tables(thd, tables, MYSQL_OPEN_FORCE_SHARED_MDL,
                                     DT_INIT | DT_PREPARE))
    goto error;

  while ((var= it++))
  {
    if (var->light_check(thd))
      goto error;
  }
  DBUG_RETURN(FALSE);
error:
  DBUG_RETURN(TRUE);
}


/**
  Validate and prepare for execution CALL statement expressions.

  @param stmt               prepared statement
  @param tables             list of tables used in this query
  @param value_list         list of expressions

  @retval FALSE             success
  @retval TRUE              error, error message is set in THD
*/

static bool mysql_test_call_fields(Prepared_statement *stmt,
                                   TABLE_LIST *tables,
                                   List<Item> *value_list)
{
  DBUG_ENTER("mysql_test_call_fields");

  List_iterator<Item> it(*value_list);
  THD *thd= stmt->thd;
  Item *item;

  if ((tables &&
       check_table_access(thd, SELECT_ACL, tables, FALSE, UINT_MAX, FALSE)) ||
      open_normal_and_derived_tables(thd, tables, MYSQL_OPEN_FORCE_SHARED_MDL,
                                     DT_INIT | DT_PREPARE))
    goto err;

  while ((item= it++))
  {
    if (item->fix_fields_if_needed(thd, it.ref()))
      goto err;
  }
  DBUG_RETURN(FALSE);
err:
  DBUG_RETURN(TRUE);
}


/**
  Check internal SELECT of the prepared command.

  @param stmt                      prepared statement
  @param specific_prepare          function of command specific prepare
  @param setup_tables_done_option  options to be passed to LEX::unit.prepare()

  @note
    This function won't directly open tables used in select. They should
    be opened either by calling function (and in this case you probably
    should use select_like_stmt_test_with_open()) or by
    "specific_prepare" call (like this happens in case of multi-update).

  @retval
    FALSE                success
  @retval
    TRUE                 error, error message is set in THD
*/

static bool select_like_stmt_test(Prepared_statement *stmt,
                                  int (*specific_prepare)(THD *thd),
                                  ulong setup_tables_done_option)
{
  DBUG_ENTER("select_like_stmt_test");
  THD *thd= stmt->thd;
  LEX *lex= stmt->lex;

  lex->first_select_lex()->context.resolve_in_select_list= TRUE;

  if (specific_prepare && (*specific_prepare)(thd))
    DBUG_RETURN(TRUE);

  thd->lex->used_tables= 0;                        // Updated by setup_fields

  /* Calls JOIN::prepare */
  DBUG_RETURN(lex->unit.prepare(lex->unit.derived, 0, setup_tables_done_option));
}

/**
  Check internal SELECT of the prepared command (with opening of used
  tables).

  @param stmt                      prepared statement
  @param tables                    list of tables to be opened
                                   before calling specific_prepare function
  @param specific_prepare          function of command specific prepare
  @param setup_tables_done_option  options to be passed to LEX::unit.prepare()

  @retval
    FALSE                success
  @retval
    TRUE                 error
*/

static bool
select_like_stmt_test_with_open(Prepared_statement *stmt,
                                TABLE_LIST *tables,
                                int (*specific_prepare)(THD *thd),
                                ulong setup_tables_done_option)
{
  uint table_count= 0;
  DBUG_ENTER("select_like_stmt_test_with_open");

  /*
    We should not call LEX::unit.cleanup() after this
    open_normal_and_derived_tables() call because we don't allow
    prepared EXPLAIN yet so derived tables will clean up after
    themself.
  */
  THD *thd= stmt->thd;
  if (open_tables(thd, &tables, &table_count, MYSQL_OPEN_FORCE_SHARED_MDL))
    DBUG_RETURN(TRUE);

  DBUG_RETURN(select_like_stmt_test(stmt, specific_prepare,
                                    setup_tables_done_option));
}


/**
  Validate and prepare for execution CREATE TABLE statement.

  @param stmt               prepared statement
  @param tables             list of tables used in this query

  @retval
    FALSE             success
  @retval
    TRUE              error, error message is set in THD
*/

static bool mysql_test_create_table(Prepared_statement *stmt)
{
  DBUG_ENTER("mysql_test_create_table");
  THD *thd= stmt->thd;
  LEX *lex= stmt->lex;
  SELECT_LEX *select_lex= lex->first_select_lex();
  bool res= FALSE;
  bool link_to_local;
  TABLE_LIST *create_table= lex->query_tables;
  TABLE_LIST *tables= lex->create_last_non_select_table->next_global;

  if (create_table_precheck(thd, tables, create_table))
    DBUG_RETURN(TRUE);

  if (select_lex->item_list.elements)
  {
    /* Base table and temporary table are not in the same name space. */
    if (!lex->create_info.tmp_table())
      create_table->open_type= OT_BASE_ONLY;

    if (open_normal_and_derived_tables(stmt->thd, lex->query_tables,
                                       MYSQL_OPEN_FORCE_SHARED_MDL,
                                       DT_INIT | DT_PREPARE))
      DBUG_RETURN(TRUE);

    select_lex->context.resolve_in_select_list= TRUE;

    lex->unlink_first_table(&link_to_local);

    res= select_like_stmt_test(stmt, 0, 0);

    lex->link_first_table_back(create_table, link_to_local);
  }
  else
  {
    /*
      Check that the source table exist, and also record
      its metadata version. Even though not strictly necessary,
      we validate metadata of all CREATE TABLE statements,
      which keeps metadata validation code simple.
    */
    if (open_normal_and_derived_tables(stmt->thd, lex->query_tables,
                                       MYSQL_OPEN_FORCE_SHARED_MDL,
                                       DT_INIT | DT_PREPARE))
      DBUG_RETURN(TRUE);
  }

  DBUG_RETURN(res);
}


static int send_stmt_metadata(THD *thd, Prepared_statement *stmt, List<Item> *fields)
{
  if (stmt->is_sql_prepare())
    return 0;

  if (send_prep_stmt(stmt, fields->elements) ||
      thd->protocol->send_result_set_metadata(fields, Protocol::SEND_EOF) ||
      thd->protocol->flush())
    return 1;

  return 2;
}


/**
  Validate and prepare for execution SHOW CREATE TABLE statement.

  @param stmt               prepared statement
  @param tables             list of tables used in this query

  @retval
    FALSE             success
  @retval
    TRUE              error, error message is set in THD
*/

static int mysql_test_show_create_table(Prepared_statement *stmt,
                                        TABLE_LIST *tables)
{
  DBUG_ENTER("mysql_test_show_create_table");
  THD *thd= stmt->thd;
  List<Item> fields;
  char buff[2048];
  String buffer(buff, sizeof(buff), system_charset_info);

  if (mysqld_show_create_get_fields(thd, tables, &fields, &buffer))
    DBUG_RETURN(1);

  DBUG_RETURN(send_stmt_metadata(thd, stmt, &fields));
}


/**
  Validate and prepare for execution SHOW CREATE DATABASE statement.

  @param stmt               prepared statement

  @retval
    FALSE             success
  @retval
    TRUE              error, error message is set in THD
*/

static int mysql_test_show_create_db(Prepared_statement *stmt)
{
  DBUG_ENTER("mysql_test_show_create_db");
  THD *thd= stmt->thd;
  List<Item> fields;

  mysqld_show_create_db_get_fields(thd, &fields);
    
  DBUG_RETURN(send_stmt_metadata(thd, stmt, &fields));
}


#ifndef NO_EMBEDDED_ACCESS_CHECKS
/**
  Validate and prepare for execution SHOW GRANTS statement.

  @param stmt               prepared statement

  @retval
    FALSE             success
  @retval
    TRUE              error, error message is set in THD
*/

static int mysql_test_show_grants(Prepared_statement *stmt)
{
  DBUG_ENTER("mysql_test_show_grants");
  THD *thd= stmt->thd;
  List<Item> fields;
  char buff[1024];
  const char *username= NULL, *hostname= NULL, *rolename= NULL, *end;

  if (get_show_user(thd, thd->lex->grant_user, &username, &hostname, &rolename))
    DBUG_RETURN(1);

  if (username)
    end= strxmov(buff,"Grants for ",username,"@",hostname, NullS);
  else if (rolename)
    end= strxmov(buff,"Grants for ",rolename, NullS);
  else
    DBUG_RETURN(1);

  mysql_show_grants_get_fields(thd, &fields, buff, (uint)(end - buff));
  DBUG_RETURN(send_stmt_metadata(thd, stmt, &fields));
}
#endif /*NO_EMBEDDED_ACCESS_CHECKS*/


#ifndef EMBEDDED_LIBRARY
/**
  Validate and prepare for execution SHOW SLAVE STATUS statement.

  @param stmt               prepared statement

  @retval
    FALSE             success
  @retval
    TRUE              error, error message is set in THD
*/

static int mysql_test_show_slave_status(Prepared_statement *stmt,
                                        bool show_all_slaves_stat)
{
  DBUG_ENTER("mysql_test_show_slave_status");
  THD *thd= stmt->thd;
  List<Item> fields;

  show_master_info_get_fields(thd, &fields, show_all_slaves_stat, 0);

  DBUG_RETURN(send_stmt_metadata(thd, stmt, &fields));
}


/**
  Validate and prepare for execution SHOW BINLOG STATUS statement.

  @param stmt               prepared statement

  @retval
    FALSE             success
  @retval
    TRUE              error, error message is set in THD
*/

static int mysql_test_show_binlog_status(Prepared_statement *stmt)
{
  DBUG_ENTER("mysql_test_show_binlog_status");
  THD *thd= stmt->thd;
  List<Item> fields;

  show_binlog_info_get_fields(thd, &fields);
    
  DBUG_RETURN(send_stmt_metadata(thd, stmt, &fields));
}


/**
  Validate and prepare for execution SHOW BINLOGS statement.

  @param stmt               prepared statement

  @retval
    FALSE             success
  @retval
    TRUE              error, error message is set in THD
*/

static int mysql_test_show_binlogs(Prepared_statement *stmt)
{
  DBUG_ENTER("mysql_test_show_binlogs");
  THD *thd= stmt->thd;
  List<Item> fields;

  show_binlogs_get_fields(thd, &fields);
    
  DBUG_RETURN(send_stmt_metadata(thd, stmt, &fields));
}

#endif /* EMBEDDED_LIBRARY */


/**
  Validate and prepare for execution SHOW CREATE PROC/FUNC statement.

  @param stmt               prepared statement

  @retval
    FALSE             success
  @retval
    TRUE              error, error message is set in THD
*/

static int mysql_test_show_create_routine(Prepared_statement *stmt,
                                          const Sp_handler *sph)
{
  DBUG_ENTER("mysql_test_show_binlogs");
  THD *thd= stmt->thd;
  List<Item> fields;

  sp_head::show_create_routine_get_fields(thd, sph, &fields);
    
  DBUG_RETURN(send_stmt_metadata(thd, stmt, &fields));
}


/**
  @brief Validate and prepare for execution CREATE VIEW statement

  @param stmt prepared statement

  @note This function handles create view commands.

  @retval FALSE Operation was a success.
  @retval TRUE An error occurred.
*/

static bool mysql_test_create_view(Prepared_statement *stmt)
{
  DBUG_ENTER("mysql_test_create_view");
  THD *thd= stmt->thd;
  LEX *lex= stmt->lex;
  bool res= TRUE;
  /* Skip first table, which is the view we are creating */
  bool link_to_local;
  TABLE_LIST *view= lex->unlink_first_table(&link_to_local);
  TABLE_LIST *tables= lex->query_tables;

  if (create_view_precheck(thd, tables, view, lex->create_view->mode))
    goto err;

  /*
    Since we can't pre-open temporary tables for SQLCOM_CREATE_VIEW,
    (see mysql_create_view) we have to do it here instead.
  */
  if (thd->open_temporary_tables(tables))
    goto err;

  lex->context_analysis_only|= CONTEXT_ANALYSIS_ONLY_VIEW;
  if (open_normal_and_derived_tables(thd, tables, MYSQL_OPEN_FORCE_SHARED_MDL,
                                     DT_INIT | DT_PREPARE))
    goto err;

  res= select_like_stmt_test(stmt, 0, 0);

err:
  /* put view back for PS rexecuting */
  lex->link_first_table_back(view, link_to_local);
  DBUG_RETURN(res);
}


/*
  Validate and prepare for execution a multi update statement.

  @param stmt               prepared statement
  @param tables             list of tables used in this query
  @param converted          converted to multi-update from usual update

  @retval
    FALSE             success
  @retval
    TRUE              error, error message is set in THD
*/

static bool mysql_test_multiupdate(Prepared_statement *stmt,
                                  TABLE_LIST *tables,
                                  bool converted)
{
  /* if we switched from normal update, rights are checked */
  if (!converted && multi_update_precheck(stmt->thd, tables))
    return TRUE;

  return select_like_stmt_test(stmt, &mysql_multi_update_prepare,
                               OPTION_SETUP_TABLES_DONE);
}


/**
  Validate and prepare for execution a multi delete statement.

  @param stmt               prepared statement
  @param tables             list of tables used in this query

  @retval
    FALSE             success
  @retval
    TRUE              error, error message in THD is set.
*/

static bool mysql_test_multidelete(Prepared_statement *stmt,
                                  TABLE_LIST *tables)
{
  THD *thd= stmt->thd;

  thd->lex->current_select= thd->lex->first_select_lex();
  if (add_item_to_list(thd, new (thd->mem_root)
                       Item_null(thd)))
  {
    my_error(ER_OUTOFMEMORY, MYF(ME_FATAL), 0);
    goto error;
  }

  if (multi_delete_precheck(thd, tables) ||
      select_like_stmt_test_with_open(stmt, tables,
                                      &mysql_multi_delete_prepare,
                                      OPTION_SETUP_TABLES_DONE))
    goto error;
  if (!tables->table)
  {
    my_error(ER_VIEW_DELETE_MERGE_VIEW, MYF(0),
             tables->view_db.str, tables->view_name.str);
    goto error;
  }
  return FALSE;
error:
  return TRUE;
}


/**
  Wrapper for mysql_insert_select_prepare, to make change of local tables
  after open_normal_and_derived_tables() call.

  @param thd                thread handle

  @note
    We need to remove the first local table after
    open_normal_and_derived_tables(), because mysql_handle_derived
    uses local tables lists.
*/

static int mysql_insert_select_prepare_tester(THD *thd)
{
  SELECT_LEX *first_select= thd->lex->first_select_lex();
  TABLE_LIST *second_table= first_select->table_list.first->next_local;

  /* Skip first table, which is the table we are inserting in */
  first_select->table_list.first= second_table;
  thd->lex->first_select_lex()->context.table_list=
    thd->lex->first_select_lex()->context.first_name_resolution_table=
    second_table;

  return mysql_insert_select_prepare(thd, NULL);
}


/**
  Validate and prepare for execution INSERT ... SELECT statement.

  @param stmt               prepared statement
  @param tables             list of tables used in this query

  @retval
    FALSE             success
  @retval
    TRUE              error, error message is set in THD
*/

static bool mysql_test_insert_select(Prepared_statement *stmt,
                                     TABLE_LIST *tables)
{
  int res;
  LEX *lex= stmt->lex;
  TABLE_LIST *first_local_table;

  if (tables->table)
  {
    // don't allocate insert_values
    tables->table->insert_values=(uchar *)1;
  }

  if (insert_precheck(stmt->thd, tables))
    return 1;

  /* store it, because mysql_insert_select_prepare_tester change it */
  first_local_table= lex->first_select_lex()->table_list.first;
  DBUG_ASSERT(first_local_table != 0);

  res=
    select_like_stmt_test_with_open(stmt, tables,
                                    &mysql_insert_select_prepare_tester,
                                    OPTION_SETUP_TABLES_DONE);
  /* revert changes  made by mysql_insert_select_prepare_tester */
  lex->first_select_lex()->table_list.first= first_local_table;
  return res;
}

/**
  Validate SELECT statement.

    In case of success, if this query is not EXPLAIN, send column list info
    back to the client.

  @param stmt               prepared statement
  @param tables             list of tables used in the query

  @retval 0 success
  @retval 1 error, error message is set in THD
  @retval 2 success, and statement metadata has been sent
*/

static int mysql_test_handler_read(Prepared_statement *stmt,
                                   TABLE_LIST *tables)
{
  THD *thd= stmt->thd;
  LEX *lex= stmt->lex;
  SQL_HANDLER *ha_table;
  DBUG_ENTER("mysql_test_handler_read");

  lex->first_select_lex()->context.resolve_in_select_list= TRUE;

  /*
    We don't have to test for permissions as this is already done during
    HANDLER OPEN
  */
  if (!(ha_table= mysql_ha_read_prepare(thd, tables, lex->ha_read_mode,
                                        lex->ident.str,
                                        lex->insert_list,
                                        lex->ha_rkey_mode,
                                        lex->first_select_lex()->where)))
    DBUG_RETURN(1);

  if (!stmt->is_sql_prepare())
  {
    if (!lex->result && !(lex->result= new (stmt->mem_root) select_send(thd)))
      DBUG_RETURN(1);

    if (send_prep_stmt(stmt, ha_table->fields.elements) ||
        lex->result->send_result_set_metadata(ha_table->fields, Protocol::SEND_EOF) ||
        thd->protocol->flush())
      DBUG_RETURN(1);
    DBUG_RETURN(2);
  }
  DBUG_RETURN(0);
}


/**
  Send metadata to a client on PREPARE phase of XA RECOVER statement
  processing

  @param stmt  prepared statement

  @return 0 on success, 1 on failure, 2 in case metadata was already sent
*/

static int mysql_test_xa_recover(Prepared_statement *stmt)
{
  THD *thd= stmt->thd;
  List<Item> field_list;

  xa_recover_get_fields(thd, &field_list, nullptr);
  return send_stmt_metadata(thd, stmt, &field_list);
}


/**
  Send metadata to a client on PREPARE phase of HELP statement processing

  @param stmt  prepared statement

  @return 0 on success, 1 on failure, 2 in case metadata was already sent
*/

static int mysql_test_help(Prepared_statement *stmt)
{
  THD *thd= stmt->thd;
  List<Item> fields;

  if (mysqld_help_prepare(thd, stmt->lex->help_arg, &fields))
    return 1;

  return send_stmt_metadata(thd, stmt, &fields);
}


/**
  Send metadata to a client on PREPARE phase of admin related statements
  processing

  @param stmt  prepared statement

  @return 0 on success, 1 on failure, 2 in case metadata was already sent
*/

static int mysql_test_admin_table(Prepared_statement *stmt)
{
  THD *thd= stmt->thd;
  List<Item> fields;

  fill_check_table_metadata_fields(thd, &fields);
  return send_stmt_metadata(thd, stmt, &fields);
}


/**
  Send metadata to a client on PREPARE phase of CHECKSUM TABLE statement
  processing

  @param stmt  prepared statement

  @return 0 on success, 1 on failure, 2 in case metadata was already sent
*/

static int mysql_test_checksum_table(Prepared_statement *stmt)
{
  THD *thd= stmt->thd;
  List<Item> fields;

  fill_checksum_table_metadata_fields(thd, &fields);
  return send_stmt_metadata(thd, stmt, &fields);
}


/**
  Perform semantic analysis of the parsed tree and send a response packet
  to the client.

    This function
    - opens all tables and checks access rights
    - validates semantics of statement columns and SQL functions
      by calling fix_fields.

  @param stmt               prepared statement

  @retval
    FALSE             success, statement metadata is sent to client
  @retval
    TRUE              error, error message is set in THD (but not sent)
*/

static bool check_prepared_statement(Prepared_statement *stmt)
{
  THD *thd= stmt->thd;
  LEX *lex= stmt->lex;
  SELECT_LEX *select_lex= lex->first_select_lex();
  TABLE_LIST *tables;
  enum enum_sql_command sql_command= lex->sql_command;
  int res= 0;
  DBUG_ENTER("check_prepared_statement");
  DBUG_PRINT("enter",("command: %d  param_count: %u",
                      sql_command, stmt->param_count));

  lex->first_lists_tables_same();
  lex->fix_first_select_number();
  tables= lex->query_tables;

  /* set context for commands which do not use setup_tables */
  lex->first_select_lex()->context.resolve_in_table_list_only(select_lex->
                                                     get_table_list());

  /*
    For the optimizer trace, this is the symmetric, for statement preparation,
    of what is done at statement execution (in mysql_execute_command()).
  */
  Opt_trace_start ots(thd);
  ots.init(thd, tables, lex->sql_command, &lex->var_list, thd->query(),
           thd->query_length(), thd->variables.character_set_client);

  Json_writer_object trace_command(thd);
  Json_writer_array trace_command_steps(thd, "steps");

  /* Reset warning count for each query that uses tables */
  if (tables)
    thd->get_stmt_da()->opt_clear_warning_info(thd->query_id);

  if (sql_command_flags[sql_command] & CF_HA_CLOSE)
    mysql_ha_rm_tables(thd, tables);

  /*
    Open temporary tables that are known now. Temporary tables added by
    prelocking will be opened afterwards (during open_tables()).
  */
  if (sql_command_flags[sql_command] & CF_PREOPEN_TMP_TABLES)
  {
    if (thd->open_temporary_tables(tables))
      goto error;
  }

#ifdef WITH_WSREP
    if (wsrep_sync_wait(thd, sql_command))
      goto error;
#endif
  switch (sql_command) {
  case SQLCOM_REPLACE:
  case SQLCOM_INSERT:
    res= mysql_test_insert(stmt, tables, lex->field_list,
                           lex->many_values,
                           lex->update_list, lex->value_list,
                           lex->duplicates);
    break;

  case SQLCOM_LOAD:
    res= mysql_test_insert_common(stmt, tables, lex->field_list,
                                  lex->many_values,
                                  lex->update_list, lex->value_list,
                                  lex->duplicates);
    break;

  case SQLCOM_UPDATE:
    res= mysql_test_update(stmt, tables);
    /* mysql_test_update returns 2 if we need to switch to multi-update */
    if (res != 2)
      break;
    /* fall through */
  case SQLCOM_UPDATE_MULTI:
    res= mysql_test_multiupdate(stmt, tables, res == 2);
    break;

  case SQLCOM_DELETE:
    res= mysql_test_delete(stmt, tables);
    break;
  /* The following allow WHERE clause, so they must be tested like SELECT */
  case SQLCOM_SHOW_DATABASES:
  case SQLCOM_SHOW_TABLES:
  case SQLCOM_SHOW_TRIGGERS:
  case SQLCOM_SHOW_EVENTS:
  case SQLCOM_SHOW_OPEN_TABLES:
  case SQLCOM_SHOW_FIELDS:
  case SQLCOM_SHOW_KEYS:
  case SQLCOM_SHOW_COLLATIONS:
  case SQLCOM_SHOW_CHARSETS:
  case SQLCOM_SHOW_VARIABLES:
  case SQLCOM_SHOW_STATUS:
  case SQLCOM_SHOW_TABLE_STATUS:
  case SQLCOM_SHOW_STATUS_PROC:
  case SQLCOM_SHOW_STATUS_FUNC:
  case SQLCOM_SHOW_STATUS_PACKAGE:
  case SQLCOM_SHOW_STATUS_PACKAGE_BODY:
  case SQLCOM_SELECT:
    res= mysql_test_select(stmt, tables);
    if (res == 2)
    {
      /* Statement and field info has already been sent */
      DBUG_RETURN(FALSE);
    }
    break;
  case SQLCOM_CREATE_TABLE:
  case SQLCOM_CREATE_SEQUENCE:
    res= mysql_test_create_table(stmt);
    break;
  case SQLCOM_SHOW_CREATE:
    if ((res= mysql_test_show_create_table(stmt, tables)) == 2)
    {
      /* Statement and field info has already been sent */
      DBUG_RETURN(FALSE);
    }
    break;
  case SQLCOM_SHOW_CREATE_DB:
    if ((res= mysql_test_show_create_db(stmt)) == 2)
    {
      /* Statement and field info has already been sent */
      DBUG_RETURN(FALSE);
    }
    break;
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  case SQLCOM_SHOW_GRANTS:
    if ((res= mysql_test_show_grants(stmt)) == 2)
    {
      /* Statement and field info has already been sent */
      DBUG_RETURN(FALSE);
    }
    break;
#endif /* NO_EMBEDDED_ACCESS_CHECKS */
#ifndef EMBEDDED_LIBRARY
  case SQLCOM_SHOW_SLAVE_STAT:
    {
      DBUG_ASSERT(thd->lex->m_sql_cmd);
      Sql_cmd_show_slave_status *cmd;
      cmd= dynamic_cast<Sql_cmd_show_slave_status*>(thd->lex->m_sql_cmd);
      DBUG_ASSERT(cmd);
      if ((res= mysql_test_show_slave_status(stmt,
                                             cmd->is_show_all_slaves_stat()))
                                             == 2)
      {
        /* Statement and field info has already been sent */
        DBUG_RETURN(FALSE);
      }
      break;
    }
  case SQLCOM_SHOW_BINLOG_STAT:
    if ((res= mysql_test_show_binlog_status(stmt)) == 2)
    {
      /* Statement and field info has already been sent */
      DBUG_RETURN(FALSE);
    }
    break;
  case SQLCOM_SHOW_BINLOGS:
    if ((res= mysql_test_show_binlogs(stmt)) == 2)
    {
      /* Statement and field info has already been sent */
      DBUG_RETURN(FALSE);
    }
    break;
  case SQLCOM_SHOW_BINLOG_EVENTS:
  case SQLCOM_SHOW_RELAYLOG_EVENTS:
    {
      List<Item> field_list;
      Log_event::init_show_field_list(thd, &field_list);

      if ((res= send_stmt_metadata(thd, stmt, &field_list)) == 2)
        DBUG_RETURN(FALSE);
    }
  break;
#endif /* EMBEDDED_LIBRARY */
  case SQLCOM_SHOW_CREATE_PROC:
    if ((res= mysql_test_show_create_routine(stmt, &sp_handler_procedure)) == 2)
    {
      /* Statement and field info has already been sent */
      DBUG_RETURN(FALSE);
    }
    break;
  case SQLCOM_SHOW_CREATE_FUNC:
    if ((res= mysql_test_show_create_routine(stmt, &sp_handler_function)) == 2)
    {
      /* Statement and field info has already been sent */
      DBUG_RETURN(FALSE);
    }
    break;
  case SQLCOM_SHOW_CREATE_PACKAGE:
    if ((res= mysql_test_show_create_routine(stmt, &sp_handler_package_spec)) == 2)
    {
      /* Statement and field info has already been sent */
      DBUG_RETURN(FALSE);
    }
    break;
  case SQLCOM_SHOW_CREATE_PACKAGE_BODY:
    if ((res= mysql_test_show_create_routine(stmt,
                                             &sp_handler_package_body)) == 2)
    {
      /* Statement and field info has already been sent */
      DBUG_RETURN(FALSE);
    }
    break;
  case SQLCOM_CREATE_VIEW:
    res= mysql_test_create_view(stmt);
    break;
  case SQLCOM_DO:
    res= mysql_test_do_fields(stmt, tables, lex->insert_list);
    break;

  case SQLCOM_CALL:
    res= mysql_test_call_fields(stmt, tables, &lex->value_list);
    break;
  case SQLCOM_SET_OPTION:
    res= mysql_test_set_fields(stmt, tables, &lex->var_list);
    break;

  case SQLCOM_DELETE_MULTI:
    res= mysql_test_multidelete(stmt, tables);
    break;

  case SQLCOM_INSERT_SELECT:
  case SQLCOM_REPLACE_SELECT:
    res= mysql_test_insert_select(stmt, tables);
    break;

  case SQLCOM_HA_READ:
    res= mysql_test_handler_read(stmt, tables);
    /* Statement and field info has already been sent */
    DBUG_RETURN(res == 1 ? TRUE : FALSE);

  case SQLCOM_XA_RECOVER:
    res= mysql_test_xa_recover(stmt);
    if (res == 2)
      /* Statement and field info has already been sent */
      DBUG_RETURN(false);
    break;

  case SQLCOM_HELP:
    res= mysql_test_help(stmt);
    if (res == 2)
      /* Statement and field info has already been sent */
      DBUG_RETURN(false);
    break;

  case SQLCOM_ANALYZE:
  case SQLCOM_ASSIGN_TO_KEYCACHE:
  case SQLCOM_CHECK:
  case SQLCOM_OPTIMIZE:
  case SQLCOM_PRELOAD_KEYS:
  case SQLCOM_REPAIR:
    res= mysql_test_admin_table(stmt);
    if (res == 2)
      /* Statement and field info has already been sent */
      DBUG_RETURN(false);
    break;

  case SQLCOM_CHECKSUM:
    res= mysql_test_checksum_table(stmt);
    if (res == 2)
      /* Statement and field info has already been sent */
      DBUG_RETURN(false);
    break;

  case SQLCOM_PREPARE:
  case SQLCOM_EXECUTE:
  case SQLCOM_EXECUTE_IMMEDIATE:
  case SQLCOM_DEALLOCATE_PREPARE:
    my_message(ER_UNSUPPORTED_PS, ER_THD(thd, ER_UNSUPPORTED_PS), MYF(0));
    goto error;

  default:
    break;
  }
  if (res == 0)
  {
    if (!stmt->is_sql_prepare())
    {
       if (lex->describe || lex->analyze_stmt)
       {
         select_send result(thd);
         List<Item> field_list;
         res= thd->prepare_explain_fields(&result, &field_list,
                                          lex->describe, lex->analyze_stmt) ||
              send_prep_stmt(stmt, result.field_count(field_list)) ||
              result.send_result_set_metadata(field_list,
                                                    Protocol::SEND_EOF);
       }
       else
         res= send_prep_stmt(stmt, 0);
       if (!res)
         thd->protocol->flush();
    }
    DBUG_RETURN(FALSE);
  }
error:
  DBUG_RETURN(TRUE);
}

/**
  Initialize array of parameters in statement from LEX.
  (We need to have quick access to items by number in mysql_stmt_get_longdata).
  This is to avoid using malloc/realloc in the parser.
*/

static bool init_param_array(Prepared_statement *stmt)
{
  LEX *lex= stmt->lex;
  if ((stmt->param_count= lex->param_list.elements))
  {
    if (stmt->param_count > (uint) UINT_MAX16)
    {
      /* Error code to be defined in 5.0 */
      my_message(ER_PS_MANY_PARAM, ER_THD(stmt->thd, ER_PS_MANY_PARAM),
                 MYF(0));
      return TRUE;
    }
    Item_param **to;
    List_iterator<Item_param> param_iterator(lex->param_list);
    /* Use thd->mem_root as it points at statement mem_root */
    stmt->param_array= (Item_param **)
                       alloc_root(stmt->thd->mem_root,
                                  sizeof(Item_param*) * stmt->param_count);
    if (!stmt->param_array)
      return TRUE;
    for (to= stmt->param_array;
         to < stmt->param_array + stmt->param_count;
         ++to)
    {
      *to= param_iterator++;
    }
  }
  return FALSE;
}


/**
  COM_STMT_PREPARE handler.

    Given a query string with parameter markers, create a prepared
    statement from it and send PS info back to the client.

    If parameter markers are found in the query, then store the information
    using Item_param along with maintaining a list in lex->param_array, so
    that a fast and direct retrieval can be made without going through all
    field items.

  @param packet             query to be prepared
  @param packet_length      query string length, including ignored
                            trailing NULL or quote char.

  @note
    This function parses the query and sends the total number of parameters
    and resultset metadata information back to client (if any), without
    executing the query i.e. without any log/disk writes. This allows the
    queries to be re-executed without re-parsing during execute.

  @return
    none: in case of success a new statement id and metadata is sent
    to the client, otherwise an error message is set in THD.
*/

void mysqld_stmt_prepare(THD *thd, const char *packet, uint packet_length)
{
  Protocol *save_protocol= thd->protocol;
  Prepared_statement *stmt;
  DBUG_ENTER("mysqld_stmt_prepare");
  DBUG_PRINT("prep_query", ("%s", packet));

  /* First of all clear possible warnings from the previous command */
  thd->reset_for_next_command();

  if (! (stmt= new Prepared_statement(thd)))
    goto end;           /* out of memory: error is set in Sql_alloc */

  if (thd->stmt_map.insert(thd, stmt))
  {
    /*
      The error is set in the insert. The statement itself
      will be also deleted there (this is how the hash works).
    */
    goto end;
  }

  thd->protocol= &thd->protocol_binary;

  /* Create PS table entry, set query text after rewrite. */
  stmt->m_prepared_stmt= MYSQL_CREATE_PS(stmt, stmt->id,
                                         thd->m_statement_psi,
                                         stmt->name.str, stmt->name.length);

  if (stmt->prepare(packet, packet_length))
  {
    /*
       Prepare failed and stmt will be freed.
       Now we have to save the query_string in the so the
       audit plugin later gets the meaningful notification.
    */
    if (alloc_query(thd, stmt->query_string.str(), stmt->query_string.length()))
    {
      thd->set_query(0, 0);
    }
    /* Statement map deletes statement on erase */
    thd->stmt_map.erase(stmt);
    thd->clear_last_stmt();
  }
  else
    thd->set_last_stmt(stmt);

  thd->protocol= save_protocol;

  sp_cache_enforce_limit(thd->sp_proc_cache, stored_program_cache_size);
  sp_cache_enforce_limit(thd->sp_func_cache, stored_program_cache_size);
  sp_cache_enforce_limit(thd->sp_package_spec_cache, stored_program_cache_size);
  sp_cache_enforce_limit(thd->sp_package_body_cache, stored_program_cache_size);

  /* check_prepared_statemnt sends the metadata packet in case of success */
end:
  DBUG_VOID_RETURN;
}

/**
  Get an SQL statement from an item in m_code.

  This function can return pointers to very different memory classes:
  - a static string "NULL", if the item returned NULL
  - the result of prepare_stmt_code->val_str(), if no conversion was needed
  - a thd->mem_root allocated string with the result of
    prepare_stmt_code->val_str() converted to @@collation_connection,
    if conversion was needed

  The caller must dispose the result before the life cycle of "buffer" ends.
  As soon as buffer's destructor is called, the value is not valid any more!

  mysql_sql_stmt_prepare() and mysql_sql_stmt_execute_immediate()
  call get_dynamic_sql_string() and then call respectively
  Prepare_statement::prepare() and Prepare_statment::execute_immediate(),
  who store the returned result into its permanent location using
  alloc_query(). "buffer" is still not destructed at that time.

  @param[out]   dst        the result is stored here
  @param[inout] buffer

  @retval       false on success
  @retval       true on error (out of memory)
*/

bool Lex_prepared_stmt::get_dynamic_sql_string(THD *thd,
                                               LEX_CSTRING *dst,
                                               String *buffer)
{
  if (m_code->fix_fields_if_needed_for_scalar(thd, NULL))
    return true;

  const String *str= m_code->val_str(buffer);
  if (m_code->null_value)
  {
    /*
      Prepare source was NULL, so we need to set "str" to
      something reasonable to get a readable error message during parsing
    */
    dst->str= "NULL";
    dst->length= 4;
    return false;
  }

  /*
    Character set conversion notes:

    1) When PREPARE or EXECUTE IMMEDIATE are used with string literals:
          PREPARE stmt FROM 'SELECT ''str''';
          EXECUTE IMMEDIATE 'SELECT ''str''';
       it's very unlikely that any conversion will happen below, because
       @@character_set_client and @@collation_connection are normally
       set to the same CHARSET_INFO pointer.

       In tricky environments when @@collation_connection is set to something
       different from @@character_set_client, double conversion may happen:
       - When the parser scans the string literal
         (sql_yacc.yy rules "prepare_src" -> "expr" -> ... -> "text_literal")
         it will convert 'str' from @@character_set_client to
         @@collation_connection.
       - Then in the code below will convert 'str' from @@collation_connection
         back to @@character_set_client.

    2) When PREPARE or EXECUTE IMMEDIATE is used with a user variable,
        it should work about the same way, because user variables are usually
        assigned like this:
          SET @str='str';
        and thus have the same character set with string literals.

    3) When PREPARE or EXECUTE IMMEDIATE is used with some
       more complex expression, conversion will depend on this expression.
       For example, a concatenation of string literals:
         EXECUTE IMMEDIATE 'SELECT * FROM'||'t1';
       should work the same way with just a single literal,
       so no conversion normally.
  */
  CHARSET_INFO *to_cs= thd->variables.character_set_client;

  uint32 unused;
  if (String::needs_conversion(str->length(), str->charset(), to_cs, &unused))
  {
    if (!(dst->str= sql_strmake_with_convert(thd, str->ptr(), str->length(),
                                             str->charset(), UINT_MAX32,
                                             to_cs, &dst->length)))
    {
      dst->length= 0;
      return true;
    }
    DBUG_ASSERT(dst->length <= UINT_MAX32);
    return false;
  }
  dst->str= str->ptr();
  dst->length= str->length();
  return false;
}


/**
  SQLCOM_PREPARE implementation.

    Prepare an SQL prepared statement. This is called from
    mysql_execute_command and should therefore behave like an
    ordinary query (e.g. should not reset any global THD data).

  @param thd     thread handle

  @return
    none: in case of success, OK packet is sent to the client,
    otherwise an error message is set in THD
*/

void mysql_sql_stmt_prepare(THD *thd)
{
  LEX *lex= thd->lex;
  CSET_STRING orig_query= thd->query_string;
  const LEX_CSTRING *name= &lex->prepared_stmt.name();
  Prepared_statement *stmt;
  LEX_CSTRING query;
  DBUG_ENTER("mysql_sql_stmt_prepare");

  if ((stmt= (Prepared_statement*) thd->stmt_map.find_by_name(name)))
  {
    /*
      If there is a statement with the same name, remove it. It is ok to
      remove old and fail to insert a new one at the same time.
    */
    if (stmt->is_in_use())
    {
      my_error(ER_PS_NO_RECURSION, MYF(0));
      DBUG_VOID_RETURN;
    }

    stmt->deallocate();
  }

  /*
    It's important for "buffer" not to be destructed before stmt->prepare()!
    See comments in get_dynamic_sql_string().
  */
  StringBuffer<256> buffer;
  if (lex->prepared_stmt.get_dynamic_sql_string(thd, &query, &buffer) ||
      ! (stmt= new Prepared_statement(thd)))
  {
    DBUG_VOID_RETURN;                           /* out of memory */
  }

  stmt->set_sql_prepare();

  /* Set the name first, insert should know that this statement has a name */
  if (stmt->set_name(name))
  {
    delete stmt;
    DBUG_VOID_RETURN;
  }

  if (thd->stmt_map.insert(thd, stmt))
  {
    /* The statement is deleted and an error is set if insert fails */
    DBUG_VOID_RETURN;
  }

  /*
    Make sure we call Prepared_statement::prepare() with an empty
    THD::change_list. It can be non-empty as LEX::get_dynamic_sql_string()
    calls fix_fields() for the Item containing the PS source,
    e.g. on character set conversion:

    SET NAMES utf8;
    DELIMITER $$
    CREATE PROCEDURE p1()
    BEGIN
      PREPARE stmt FROM CONCAT('SELECT ',CONVERT(RAND() USING latin1));
      EXECUTE stmt;
    END;
    $$
    DELIMITER ;
    CALL p1();
  */
  Item_change_list_savepoint change_list_savepoint(thd);

  /* Create PS table entry, set query text after rewrite. */
  stmt->m_prepared_stmt= MYSQL_CREATE_PS(stmt, stmt->id,
                                         thd->m_statement_psi,
                                         stmt->name.str, stmt->name.length);

  bool res= stmt->prepare(query.str, (uint) query.length);
  /*
    stmt->prepare() sets thd->query_string with the prepared
    query, so the audit plugin gets adequate notification with the
    mysqld_stmt_* set of functions.
    But here we should restore the original query so it's mentioned in
    logs properly.
  */
  thd->set_query(orig_query);
  if (res)
  {
    /* Statement map deletes the statement on erase */
    thd->stmt_map.erase(stmt);
  }
  else
  {
    thd->session_tracker.state_change.mark_as_changed(thd);
    my_ok(thd, 0L, 0L, "Statement prepared");
  }
  change_list_savepoint.rollback(thd);

  DBUG_VOID_RETURN;
}


void mysql_sql_stmt_execute_immediate(THD *thd)
{
  LEX *lex= thd->lex;
  CSET_STRING orig_query= thd->query_string;
  Prepared_statement *stmt;
  LEX_CSTRING query;
  DBUG_ENTER("mysql_sql_stmt_execute_immediate");

  if (lex->prepared_stmt.params_fix_fields(thd))
    DBUG_VOID_RETURN;

  /*
    Prepared_statement is quite large,
    let's allocate it on the heap rather than on the stack.

    It's important for "buffer" not to be destructed
    before stmt->execute_immediate().
    See comments in get_dynamic_sql_string().
  */
  StringBuffer<256> buffer;
  if (lex->prepared_stmt.get_dynamic_sql_string(thd, &query, &buffer) ||
      !(stmt= new Prepared_statement(thd)))
    DBUG_VOID_RETURN;                           // out of memory

  // See comments on thd->free_list in mysql_sql_stmt_execute()
  Item *free_list_backup= thd->free_list;
  thd->free_list= NULL;
  /*
    Make sure we call Prepared_statement::execute_immediate()
    with an empty THD::change_list. It can be non empty as the above
    LEX::prepared_stmt_params_fix_fields() and LEX::get_dynamic_str_string()
    call fix_fields() for the PS source and PS parameter Items and
    can do Item tree changes, e.g. on character set conversion:

    - Example #1: Item tree changes in get_dynamic_str_string()
    SET NAMES utf8;
    CREATE PROCEDURE p1()
      EXECUTE IMMEDIATE CONCAT('SELECT ',CONVERT(RAND() USING latin1));
    CALL p1();

    - Example #2: Item tree changes in prepared_stmt_param_fix_fields():
    SET NAMES utf8;
    CREATE PROCEDURE p1(a VARCHAR(10) CHARACTER SET utf8)
      EXECUTE IMMEDIATE 'SELECT ?' USING CONCAT(a, CONVERT(RAND() USING latin1));
    CALL p1('x');
  */
  Item_change_list_savepoint change_list_savepoint(thd);
  (void) stmt->execute_immediate(query.str, (uint) query.length);
  change_list_savepoint.rollback(thd);
  thd->free_items();
  thd->free_list= free_list_backup;

  /*
    stmt->execute_immediately() sets thd->query_string with the executed
    query, so the audit plugin gets adequate notification with the
    mysqld_stmt_* set of functions.
    But here we should restore the original query so it's mentioned in
    logs properly.
  */
  thd->set_query_inner(orig_query);
  stmt->lex->restore_set_statement_var();
  delete stmt;
  DBUG_VOID_RETURN;
}


/**
  Reinit prepared statement/stored procedure before execution.

  @todo
    When the new table structure is ready, then have a status bit
    to indicate the table is altered, and re-do the setup_*
    and open the tables back.
*/

void reinit_stmt_before_use(THD *thd, LEX *lex)
{
  SELECT_LEX *sl= lex->all_selects_list;
  DBUG_ENTER("reinit_stmt_before_use");
  Window_spec *win_spec;

  /*
    We have to update "thd" pointer in LEX, all its units and in LEX::result,
    since statements which belong to trigger body are associated with TABLE
    object and because of this can be used in different threads.
  */
  lex->thd= thd;
  DBUG_ASSERT(!lex->explain);

  if (lex->empty_field_list_on_rset)
  {
    lex->empty_field_list_on_rset= 0;
    lex->field_list.empty();
  }
  for (; sl; sl= sl->next_select_in_list())
  {
    if (sl->changed_elements & TOUCHED_SEL_COND)
    {
      /* remove option which was put by mysql_explain_union() */
      sl->options&= ~SELECT_DESCRIBE;

      /* see unique_table() */
      sl->exclude_from_table_unique_test= FALSE;

      /*
        Copy WHERE, HAVING clause pointers to avoid damaging them
        by optimisation
      */
      if (sl->prep_where)
      {
        /*
          We need this rollback because memory allocated in
          copy_andor_structure() will be freed
        */
        thd->change_item_tree((Item**)&sl->where,
                              sl->prep_where->copy_andor_structure(thd));
        sl->where->cleanup();
      }
      else
        sl->where= NULL;
      if (sl->prep_having)
      {
        /*
          We need this rollback because memory allocated in
          copy_andor_structure() will be freed
        */
        thd->change_item_tree((Item**)&sl->having,
                              sl->prep_having->copy_andor_structure(thd));
        sl->having->cleanup();
      }
      else
        sl->having= NULL;
      DBUG_ASSERT(sl->join == 0);
      ORDER *order;
      /* Fix GROUP list */
      if (sl->group_list_ptrs && sl->group_list_ptrs->size() > 0)
      {
        for (uint ix= 0; ix < sl->group_list_ptrs->size() - 1; ++ix)
        {
          order= sl->group_list_ptrs->at(ix);
          order->next= sl->group_list_ptrs->at(ix+1);
        }
      }
    }
    { // no harm to do it (item_ptr set on parsing)
      ORDER *order;
      for (order= sl->group_list.first; order; order= order->next)
      {
        order->item= &order->item_ptr;
      }
      /* Fix ORDER list */
      for (order= sl->order_list.first; order; order= order->next)
        order->item= &order->item_ptr;
      /* Fix window functions too */
      List_iterator<Window_spec> it(sl->window_specs);

      while ((win_spec= it++))
      {
        for (order= win_spec->partition_list->first; order; order= order->next)
          order->item= &order->item_ptr;
        for (order= win_spec->order_list->first; order; order= order->next)
          order->item= &order->item_ptr;
      }

      // Reinit Pushdown
      sl->cond_pushed_into_where= NULL;
      sl->cond_pushed_into_having= NULL;
    }
    if (sl->changed_elements & TOUCHED_SEL_DERIVED)
    {
#ifdef DBUG_ASSERT_EXISTS
      bool res=
#endif
        sl->handle_derived(lex, DT_REINIT);
      DBUG_ASSERT(res == 0);
    }

    {
      SELECT_LEX_UNIT *unit= sl->master_unit();
      unit->unclean();
      unit->types.empty();
      /* for derived tables & PS (which can't be reset by Item_subselect) */
      unit->reinit_exec_mechanism();
      unit->set_thd(thd);
    }
  }

  /*
    TODO: When the new table structure is ready, then have a status bit
    to indicate the table is altered, and re-do the setup_*
    and open the tables back.
  */
  /*
    NOTE: We should reset whole table list here including all tables added
    by prelocking algorithm (it is not a problem for substatements since
    they have their own table list).
  */
  for (TABLE_LIST *tables= lex->query_tables;
       tables;
       tables= tables->next_global)
  {
    tables->reinit_before_use(thd);
  }

  /* Reset MDL tickets for procedures/functions */
  for (Sroutine_hash_entry *rt=
         (Sroutine_hash_entry*)thd->lex->sroutines_list.first;
       rt; rt= rt->next)
    rt->mdl_request.ticket= NULL;

  /*
    Cleanup of the special case of DELETE t1, t2 FROM t1, t2, t3 ...
    (multi-delete).  We do a full clean up, although at the moment all we
    need to clean in the tables of MULTI-DELETE list is 'table' member.
  */
  for (TABLE_LIST *tables= lex->auxiliary_table_list.first;
       tables;
       tables= tables->next_global)
  {
    tables->reinit_before_use(thd);
  }
  lex->current_select= lex->first_select_lex();


  if (lex->result)
  {
    lex->result->cleanup();
    lex->result->set_thd(thd);
  }
  lex->allow_sum_func.clear_all();
  lex->in_sum_func= NULL;
  DBUG_VOID_RETURN;
}


/**
  Clears parameters from data left from previous execution or long data.

  @param stmt               prepared statement for which parameters should
                            be reset
*/

static void reset_stmt_params(Prepared_statement *stmt)
{
  Item_param **item= stmt->param_array;
  Item_param **end= item + stmt->param_count;
  for (;item < end ; ++item)
  {
    (**item).reset();
    (**item).sync_clones();
  }
}


static void mysql_stmt_execute_common(THD *thd,
                                      ulong stmt_id,
                                      uchar *packet,
                                      uchar *packet_end,
                                      ulong cursor_flags,
                                      bool iteration,
                                      bool types);

/**
  COM_STMT_EXECUTE handler: execute a previously prepared statement.

    If there are any parameters, then replace parameter markers with the
    data supplied from the client, and then execute the statement.
    This function uses binary protocol to send a possible result set
    to the client.

  @param thd                current thread
  @param packet_arg         parameter types and data, if any
  @param packet_length      packet length, including the terminator character.

  @return
    none: in case of success OK packet or a result set is sent to the
    client, otherwise an error message is set in THD.
*/

void mysqld_stmt_execute(THD *thd, char *packet_arg, uint packet_length)
{
  const uint packet_min_lenght= 9;
  uchar *packet= (uchar*)packet_arg; // GCC 4.0.1 workaround

  DBUG_ENTER("mysqld_stmt_execute");

  if (packet_length < packet_min_lenght)
  {
    my_error(ER_MALFORMED_PACKET, MYF(0));
    DBUG_VOID_RETURN;
  }
  ulong stmt_id= uint4korr(packet);
  ulong flags= (ulong) packet[4];
  uchar *packet_end= packet + packet_length;

  packet+= 9;                               /* stmt_id + 5 bytes of flags */

  mysql_stmt_execute_common(thd, stmt_id, packet, packet_end, flags, FALSE,
  FALSE);
  DBUG_VOID_RETURN;
}


/**
  COM_STMT_BULK_EXECUTE handler: execute a previously prepared statement.

    If there are any parameters, then replace parameter markers with the
    data supplied from the client, and then execute the statement.
    This function uses binary protocol to send a possible result set
    to the client.

  @param thd                current thread
  @param packet_arg         parameter types and data, if any
  @param packet_length      packet length, including the terminator character.

  @return
    none: in case of success OK packet or a result set is sent to the
    client, otherwise an error message is set in THD.
*/

void mysqld_stmt_bulk_execute(THD *thd, char *packet_arg, uint packet_length)
{
  uchar *packet= (uchar*)packet_arg; // GCC 4.0.1 workaround
  DBUG_ENTER("mysqld_stmt_execute_bulk");

  const uint packet_header_lenght= 4 + 2; //ID & 2 bytes of flags

  if (packet_length < packet_header_lenght)
  {
    my_error(ER_MALFORMED_PACKET, MYF(0));
    DBUG_VOID_RETURN;
  }

  ulong stmt_id= uint4korr(packet);
  uint flags= (uint) uint2korr(packet + 4);
  uchar *packet_end= packet + packet_length;

  if (!(thd->client_capabilities &
        MARIADB_CLIENT_STMT_BULK_OPERATIONS))
  {
    DBUG_PRINT("error",
               ("An attempt to execute bulk operation without support"));
    my_error(ER_UNSUPPORTED_PS, MYF(0));
    DBUG_VOID_RETURN;
  }
  /* Check for implemented parameters */
  if (flags & (~STMT_BULK_FLAG_CLIENT_SEND_TYPES))
  {
    DBUG_PRINT("error", ("unsupported bulk execute flags %x", flags));
    my_error(ER_UNSUPPORTED_PS, MYF(0));
    DBUG_VOID_RETURN;
  }

  /* stmt id and two bytes of flags */
  packet+= packet_header_lenght;
  mysql_stmt_execute_common(thd, stmt_id, packet, packet_end, 0, TRUE,
                            (flags & STMT_BULK_FLAG_CLIENT_SEND_TYPES));
  DBUG_VOID_RETURN;
}

/**
  Additional packet checks for direct execution

  @param thd             THD handle
  @param stmt            prepared statement being directly executed
  @param paket           packet with parameters to bind
  @param packet_end      pointer to the byte after parameters end
  @param bulk_op         is it bulk operation
  @param direct_exec     is it direct execution
  @param read_bytes      need to read types (only with bulk_op)

  @retval true  this parameter is wrong
  @retval false this parameter is OK
*/

static bool
stmt_execute_packet_sanity_check(Prepared_statement *stmt,
                                 uchar *packet, uchar *packet_end,
                                 bool bulk_op, bool direct_exec,
                                 bool read_types)
{

  DBUG_ASSERT((!read_types) || (read_types && bulk_op));
  if (stmt->param_count > 0)
  {
    uint packet_length= static_cast<uint>(packet_end - packet);
    uint null_bitmap_bytes= (bulk_op ? 0 : (stmt->param_count + 7)/8);
    uint min_len_for_param_count = null_bitmap_bytes
                                 + (bulk_op ? 0 : 1); /* sent types byte */

    if (!bulk_op && packet_length >= min_len_for_param_count)
    {
      if ((read_types= packet[null_bitmap_bytes]))
      {
        /*
          Should be 0 or 1. If the byte is not 1, that could mean,
          e.g. that we read incorrect byte due to incorrect number
          of sent parameters for direct execution (i.e. null bitmap
          is shorter or longer, than it should be)
        */
        if (packet[null_bitmap_bytes] != '\1')
        {
          return true;
        }
      }
    }

    if (read_types)
    {
      /* 2 bytes per parameter of the type and flags */
      min_len_for_param_count+= 2*stmt->param_count;
    }
    else
    {
      /*
        If types are not sent, there is nothing to do here.
        But for direct execution types should always be sent
      */
      return direct_exec;
    }

    /*
      If true, the packet is guaranteed too short for the number of
      parameters in the PS
    */
    return (packet_length < min_len_for_param_count);
  }
  else
  {
    /*
      If there is no parameters, this should be normally already end
      of the packet, but it is not a problem if something left (popular
      mistake in protocol implementation) because we will not read anymore
      from the buffer.
    */
    return false;
  }
  return false;
}


/**
  Common part of prepared statement execution

  @param thd             THD handle
  @param stmt_id         id of the prepared statement
  @param paket           packet with parameters to bind
  @param packet_end      pointer to the byte after parameters end
  @param cursor_flags    cursor flags
  @param bulk_op         id it bulk operation
  @param read_types      flag say that types muast been read
*/

static void mysql_stmt_execute_common(THD *thd,
                                      ulong stmt_id,
                                      uchar *packet,
                                      uchar *packet_end,
                                      ulong cursor_flags,
                                      bool bulk_op,
                                      bool read_types)
{
  /* Query text for binary, general or slow log, if any of them is open */
  String expanded_query;
  Prepared_statement *stmt;
  Protocol *save_protocol= thd->protocol;
  bool open_cursor;
  DBUG_ENTER("mysqld_stmt_execute_common");
  DBUG_ASSERT((!read_types) || (read_types && bulk_op));

  /* First of all clear possible warnings from the previous command */
  thd->reset_for_next_command();

  if (!(stmt= find_prepared_statement(thd, stmt_id)))
  {
    char llbuf[22];
    size_t length;
    /*
      Did not find the statement with the provided stmt_id.
      Set thd->query_string with the stmt_id so the
      audit plugin gets the meaningful notification.
    */
    length= (size_t) (longlong10_to_str(stmt_id, llbuf, 10) - llbuf);
    if (alloc_query(thd, llbuf, length + 1))
      thd->set_query(0, 0);
    my_error(ER_UNKNOWN_STMT_HANDLER, MYF(0), (int) length, llbuf,
             "mysqld_stmt_execute");
    DBUG_VOID_RETURN;
  }

  /*
    In case of direct execution application decides how many parameters
    to send.

    Thus extra checks are required to prevent crashes caused by incorrect
    interpretation of the packet data. Plus there can be always a broken
    evil client.
  */
  if (stmt_execute_packet_sanity_check(stmt, packet, packet_end, bulk_op,
                                       stmt_id == LAST_STMT_ID, read_types))
  {
    my_error(ER_MALFORMED_PACKET, MYF(0));
    /*
      Let's set the thd->query_string so the audit plugin
      can report the executed query that failed.
    */
    thd->set_query_inner(stmt->query_string);
    DBUG_VOID_RETURN;
  }

  stmt->read_types= read_types;

#if defined(ENABLED_PROFILING)
  thd->profiling.set_query_source(stmt->query(), stmt->query_length());
#endif
  DBUG_PRINT("exec_query", ("%s", stmt->query()));
  DBUG_PRINT("info",("stmt: %p bulk_op %d", stmt, bulk_op));

  open_cursor= MY_TEST(cursor_flags & (ulong) CURSOR_TYPE_READ_ONLY);

  thd->protocol= &thd->protocol_binary;
  MYSQL_EXECUTE_PS(thd->m_statement_psi, stmt->m_prepared_stmt);

  auto save_cur_stmt= thd->cur_stmt;
  thd->cur_stmt= stmt;

  if (!bulk_op)
    stmt->execute_loop(&expanded_query, open_cursor, packet, packet_end);
  else
    stmt->execute_bulk_loop(&expanded_query, open_cursor, packet, packet_end);

  thd->cur_stmt= save_cur_stmt;
  thd->protocol= save_protocol;

  sp_cache_enforce_limit(thd->sp_proc_cache, stored_program_cache_size);
  sp_cache_enforce_limit(thd->sp_func_cache, stored_program_cache_size);
  sp_cache_enforce_limit(thd->sp_package_spec_cache, stored_program_cache_size);
  sp_cache_enforce_limit(thd->sp_package_body_cache, stored_program_cache_size);

  /* Close connection socket; for use with client testing (Bug#43560). */
  DBUG_EXECUTE_IF("close_conn_after_stmt_execute", vio_shutdown(thd->net.vio,SHUT_RD););

  DBUG_VOID_RETURN;
}


/**
  SQLCOM_EXECUTE implementation.

    Execute prepared statement using parameter values from
    lex->prepared_stmt.params() and send result to the client using
    text protocol. This is called from mysql_execute_command and
    therefore should behave like an ordinary query (e.g. not change
    global THD data, such as warning count, server status, etc).
    This function uses text protocol to send a possible result set.

  @param thd                thread handle

  @return
    none: in case of success, OK (or result set) packet is sent to the
    client, otherwise an error is set in THD
*/

void mysql_sql_stmt_execute(THD *thd)
{
  LEX *lex= thd->lex;
  Prepared_statement *stmt;
  const LEX_CSTRING *name= &lex->prepared_stmt.name();
  /* Query text for binary, general or slow log, if any of them is open */
  String expanded_query;
  DBUG_ENTER("mysql_sql_stmt_execute");
  DBUG_PRINT("info", ("EXECUTE: %.*s", (int) name->length, name->str));

  if (!(stmt= (Prepared_statement*) thd->stmt_map.find_by_name(name)))
  {
    my_error(ER_UNKNOWN_STMT_HANDLER, MYF(0),
             static_cast<int>(name->length), name->str, "EXECUTE");
    DBUG_VOID_RETURN;
  }

  if (stmt->param_count != lex->prepared_stmt.param_count())
  {
    my_error(ER_WRONG_ARGUMENTS, MYF(0), "EXECUTE");
    DBUG_VOID_RETURN;
  }

  DBUG_PRINT("info",("stmt: %p", stmt));

  if (lex->prepared_stmt.params_fix_fields(thd))
    DBUG_VOID_RETURN;

  /*
    thd->free_list can already have some Items.

    Example queries:
      - SET STATEMENT var=expr FOR EXECUTE stmt;
      - EXECUTE stmt USING expr;

    E.g. for a query like this:
      PREPARE stmt FROM 'INSERT INTO t1 VALUES (@@max_sort_length)';
      SET STATEMENT max_sort_length=2048 FOR EXECUTE stmt;
    thd->free_list contains a pointer to Item_int corresponding to 2048.

    If Prepared_statement::execute() notices that the table metadata for "t1"
    has changed since PREPARE, it returns an error asking the calling
    Prepared_statement::execute_loop() to re-prepare the statement.
    Before returning the error, Prepared_statement::execute()
    calls Prepared_statement::cleanup_stmt(),
    which calls thd->cleanup_after_query(),
    which calls Query_arena::free_items().

    We hide "external" Items, e.g. those created while parsing the
    "SET STATEMENT" or "USING" parts of the query,
    so they don't get freed in case of re-prepare.
    See MDEV-10702 Crash in SET STATEMENT FOR EXECUTE
  */
  Item *free_list_backup= thd->free_list;
  thd->free_list= NULL; // Hide the external (e.g. "SET STATEMENT") Items
  /*
    Make sure we call Prepared_statement::execute_loop() with an empty
    THD::change_list. It can be non-empty because the above
    LEX::prepared_stmt_params_fix_fields() calls fix_fields() for
    the PS parameter Items and can do some Item tree changes,
    e.g. on character set conversion:

    SET NAMES utf8;
    DELIMITER $$
    CREATE PROCEDURE p1(a VARCHAR(10) CHARACTER SET utf8)
    BEGIN
      PREPARE stmt FROM 'SELECT ?';
      EXECUTE stmt USING CONCAT(a, CONVERT(RAND() USING latin1));
    END;
    $$
    DELIMITER ;
    CALL p1('x');
  */
  Item_change_list_savepoint change_list_savepoint(thd);
  MYSQL_EXECUTE_PS(thd->m_statement_psi, stmt->m_prepared_stmt);

  (void) stmt->execute_loop(&expanded_query, FALSE, NULL, NULL);
  change_list_savepoint.rollback(thd);
  thd->free_items();    // Free items created by execute_loop()
  /*
    Now restore the "external" (e.g. "SET STATEMENT") Item list.
    It will be freed normaly in THD::cleanup_after_query().
  */
  thd->free_list= free_list_backup;

  stmt->lex->restore_set_statement_var();
  DBUG_VOID_RETURN;
}


/**
  COM_STMT_FETCH handler: fetches requested amount of rows from cursor.

  @param thd                Thread handle
  @param packet             Packet from client (with stmt_id & num_rows)
  @param packet_length      Length of packet
*/

void mysqld_stmt_fetch(THD *thd, char *packet, uint packet_length)
{
  /* assume there is always place for 8-16 bytes */
  ulong stmt_id= uint4korr(packet);
  ulong num_rows= uint4korr(packet+4);
  Prepared_statement *stmt;
  Statement stmt_backup;
  Server_side_cursor *cursor;
  DBUG_ENTER("mysqld_stmt_fetch");

  /* First of all clear possible warnings from the previous command */
  thd->reset_for_next_command();

  status_var_increment(thd->status_var.com_stmt_fetch);
  if (!(stmt= find_prepared_statement(thd, stmt_id)))
  {
    char llbuf[22];
    my_error(ER_UNKNOWN_STMT_HANDLER, MYF(0), static_cast<int>(sizeof(llbuf)),
             llstr(stmt_id, llbuf), "mysqld_stmt_fetch");
    DBUG_VOID_RETURN;
  }

  cursor= stmt->cursor;
  if (!cursor)
  {
    my_error(ER_STMT_HAS_NO_OPEN_CURSOR, MYF(0), stmt_id);
    DBUG_VOID_RETURN;
  }

  thd->stmt_arena= stmt;
  thd->set_n_backup_statement(stmt, &stmt_backup);

  cursor->fetch(num_rows);

  if (!cursor->is_open())
  {
    stmt->close_cursor();
    reset_stmt_params(stmt);
  }

  thd->restore_backup_statement(stmt, &stmt_backup);
  thd->stmt_arena= thd;

  DBUG_VOID_RETURN;
}


/**
  Reset a prepared statement in case there was a recoverable error.

    This function resets statement to the state it was right after prepare.
    It can be used to:
    - clear an error happened during mysqld_stmt_send_long_data
    - cancel long data stream for all placeholders without
      having to call mysqld_stmt_execute.
    - close an open cursor
    Sends 'OK' packet in case of success (statement was reset)
    or 'ERROR' packet (unrecoverable error/statement not found/etc).

  @param thd                Thread handle
  @param packet             Packet with stmt id
*/

void mysqld_stmt_reset(THD *thd, char *packet)
{
  /* There is always space for 4 bytes in buffer */
  ulong stmt_id= uint4korr(packet);
  Prepared_statement *stmt;
  DBUG_ENTER("mysqld_stmt_reset");

  /* First of all clear possible warnings from the previous command */
  thd->reset_for_next_command();

  status_var_increment(thd->status_var.com_stmt_reset);
  if (!(stmt= find_prepared_statement(thd, stmt_id)))
  {
    char llbuf[22];
    my_error(ER_UNKNOWN_STMT_HANDLER, MYF(0), static_cast<int>(sizeof(llbuf)),
             llstr(stmt_id, llbuf), "mysqld_stmt_reset");
    DBUG_VOID_RETURN;
  }

  stmt->close_cursor();

  /*
    Clear parameters from data which could be set by
    mysqld_stmt_send_long_data() call.
  */
  reset_stmt_params(stmt);

  stmt->state= Query_arena::STMT_PREPARED;

  general_log_print(thd, thd->get_command(), NullS);

  my_ok(thd);

  DBUG_VOID_RETURN;
}


/**
  Delete a prepared statement from memory.

  @note
    we don't send any reply to this command.
*/

void mysqld_stmt_close(THD *thd, char *packet)
{
  /* There is always space for 4 bytes in packet buffer */
  ulong stmt_id= uint4korr(packet);
  Prepared_statement *stmt;
  DBUG_ENTER("mysqld_stmt_close");

  thd->get_stmt_da()->disable_status();

  if (!(stmt= find_prepared_statement(thd, stmt_id)))
    DBUG_VOID_RETURN;

  /*
    The only way currently a statement can be deallocated when it's
    in use is from within Dynamic SQL.
  */
  DBUG_ASSERT(! stmt->is_in_use());
  stmt->deallocate();
  general_log_print(thd, thd->get_command(), NullS);

  if (thd->last_stmt == stmt)
    thd->clear_last_stmt();

  DBUG_VOID_RETURN;
}


/**
  SQLCOM_DEALLOCATE implementation.

    Close an SQL prepared statement. As this can be called from Dynamic
    SQL, we should be careful to not close a statement that is currently
    being executed.

  @return
    none: OK packet is sent in case of success, otherwise an error
    message is set in THD
*/

void mysql_sql_stmt_close(THD *thd)
{
  Prepared_statement* stmt;
  const LEX_CSTRING *name= &thd->lex->prepared_stmt.name();
  DBUG_PRINT("info", ("DEALLOCATE PREPARE: %.*s", (int) name->length,
                      name->str));

  if (! (stmt= (Prepared_statement*) thd->stmt_map.find_by_name(name)))
    my_error(ER_UNKNOWN_STMT_HANDLER, MYF(0),
             static_cast<int>(name->length), name->str, "DEALLOCATE PREPARE");
  else if (stmt->is_in_use())
    my_error(ER_PS_NO_RECURSION, MYF(0));
  else
  {
    stmt->deallocate();
    thd->session_tracker.state_change.mark_as_changed(thd);
    my_ok(thd);
  }
}


/**
  Handle long data in pieces from client.

    Get a part of a long data. To make the protocol efficient, we are
    not sending any return packets here. If something goes wrong, then
    we will send the error on 'execute' We assume that the client takes
    care of checking that all parts are sent to the server. (No checking
    that we get a 'end of column' in the server is performed).

  @param thd                Thread handle
  @param packet             String to append
  @param packet_length      Length of string (including end \\0)
*/

void mysql_stmt_get_longdata(THD *thd, char *packet, ulong packet_length)
{
  ulong stmt_id;
  uint param_number;
  Prepared_statement *stmt;
  Item_param *param;
#ifndef EMBEDDED_LIBRARY
  char *packet_end= packet + packet_length;
#endif
  DBUG_ENTER("mysql_stmt_get_longdata");

  status_var_increment(thd->status_var.com_stmt_send_long_data);

  thd->get_stmt_da()->disable_status();
#ifndef EMBEDDED_LIBRARY
  /* Minimal size of long data packet is 6 bytes */
  if (packet_length < MYSQL_LONG_DATA_HEADER)
    DBUG_VOID_RETURN;
#endif

  stmt_id= uint4korr(packet);
  packet+= 4;

  if (!(stmt=find_prepared_statement(thd, stmt_id)))
    DBUG_VOID_RETURN;

  param_number= uint2korr(packet);
  packet+= 2;
#ifndef EMBEDDED_LIBRARY
  if (param_number >= stmt->param_count)
  {
    /* Error will be sent in execute call */
    stmt->state= Query_arena::STMT_ERROR;
    stmt->last_errno= ER_WRONG_ARGUMENTS;
    sprintf(stmt->last_error, ER_THD(thd, ER_WRONG_ARGUMENTS),
            "mysqld_stmt_send_long_data");
    DBUG_VOID_RETURN;
  }
#endif

  param= stmt->param_array[param_number];

  Diagnostics_area new_stmt_da(thd->query_id, false, true);
  Diagnostics_area *save_stmt_da= thd->get_stmt_da();

  thd->set_stmt_da(&new_stmt_da);

#ifndef EMBEDDED_LIBRARY
  param->set_longdata(packet, (ulong) (packet_end - packet));
#else
  param->set_longdata(thd->extra_data, thd->extra_length);
#endif
  if (unlikely(thd->get_stmt_da()->is_error()))
  {
    stmt->state= Query_arena::STMT_ERROR;
    stmt->last_errno= thd->get_stmt_da()->sql_errno();
    strmake_buf(stmt->last_error, thd->get_stmt_da()->message());
  }
  thd->set_stmt_da(save_stmt_da);

  general_log_print(thd, thd->get_command(), NullS);

  DBUG_VOID_RETURN;
}


/***************************************************************************
 Select_fetch_protocol_binary
****************************************************************************/

Select_fetch_protocol_binary::Select_fetch_protocol_binary(THD *thd_arg):
  select_send(thd_arg), protocol(thd_arg)
{}

bool Select_fetch_protocol_binary::send_result_set_metadata(List<Item> &list, uint flags)
{
  bool rc;
  Protocol *save_protocol= thd->protocol;

  /*
    Protocol::send_result_set_metadata caches the information about column types:
    this information is later used to send data. Therefore, the same
    dedicated Protocol object must be used for all operations with
    a cursor.
  */
  thd->protocol= &protocol;
  rc= select_send::send_result_set_metadata(list, flags);
  thd->protocol= save_protocol;

  return rc;
}

bool Select_fetch_protocol_binary::send_eof()
{
  /*
    Don't send EOF if we're in error condition (which implies we've already
    sent or are sending an error)
  */
  if (unlikely(thd->is_error()))
    return true;

  ::my_eof(thd);
  return false;
}


int
Select_fetch_protocol_binary::send_data(List<Item> &fields)
{
  Protocol *save_protocol= thd->protocol;
  int rc;

  thd->protocol= &protocol;
  rc= select_send::send_data(fields);
  thd->protocol= save_protocol;
  return rc;
}

/*******************************************************************
* Reprepare_observer
*******************************************************************/
/** Push an error to the error stack and return TRUE for now. */

bool
Reprepare_observer::report_error(THD *thd)
{
  /*
    This 'error' is purely internal to the server:
    - No exception handler is invoked,
    - No condition is added in the condition area (warn_list).
    The diagnostics area is set to an error status to enforce
    that this thread execution stops and returns to the caller,
    backtracking all the way to Prepared_statement::execute_loop().
  */
  thd->get_stmt_da()->set_error_status(ER_NEED_REPREPARE);
  m_invalidated= TRUE;

  return TRUE;
}


/*******************************************************************
* Server_runnable
*******************************************************************/

Server_runnable::~Server_runnable()
{
}

///////////////////////////////////////////////////////////////////////////

Execute_sql_statement::
Execute_sql_statement(LEX_STRING sql_text)
  :m_sql_text(sql_text)
{}


/**
  Parse and execute a statement. Does not prepare the query.

  Allows to execute a statement from within another statement.
  The main property of the implementation is that it does not
  affect the environment -- i.e. you  can run many
  executions without having to cleanup/reset THD in between.
*/

static bool execute_server_code(THD *thd,
                                const char *sql_text, size_t sql_len)
{
  PSI_statement_locker *parent_locker;
  bool error;
  query_id_t save_query_id= thd->query_id;
  query_id_t next_id= next_query_id();

  if (alloc_query(thd, sql_text, sql_len))
    return TRUE;

  Parser_state parser_state;
  if (parser_state.init(thd, thd->query(), thd->query_length()))
    return TRUE;

  thd->query_id= next_id;
  parser_state.m_lip.multi_statements= FALSE;
  lex_start(thd);

  error= parse_sql(thd, &parser_state, NULL) || thd->is_error();

  if (unlikely(error))
    goto end;

  thd->lex->set_trg_event_type_for_tables();

  parent_locker= thd->m_statement_psi;
  thd->m_statement_psi= NULL;
  error= mysql_execute_command(thd);
  thd->m_statement_psi= parent_locker;

  /* report error issued during command execution */
  if (likely(error == 0) && thd->spcont == NULL)
    general_log_write(thd, COM_QUERY,
                      thd->query(), thd->query_length());

end:
  thd->lex->restore_set_statement_var();
  thd->query_id= save_query_id;
  delete_explain_query(thd->lex);
  lex_end(thd->lex);

  return error;
}

bool Execute_sql_statement::execute_server_code(THD *thd)
{
  return ::execute_server_code(thd, m_sql_text.str, m_sql_text.length);
}

/***************************************************************************
 Prepared_statement
****************************************************************************/

Prepared_statement::Prepared_statement(THD *thd_arg)
  :Statement(NULL, &main_mem_root,
             STMT_INITIALIZED,
             ((++thd_arg->statement_id_counter) & STMT_ID_MASK)),
  thd(thd_arg),
  m_prepared_stmt(NULL),
  result(thd_arg),
  param_array(0),
  cursor(0),
  packet(0),
  packet_end(0),
  param_count(0),
  last_errno(0),
  flags((uint) IS_IN_USE),
  iterations(0),
  start_param(0),
  read_types(0),
  m_sql_mode(thd->variables.sql_mode)
{
  init_sql_alloc(key_memory_prepared_statement_main_mem_root,
                 &main_mem_root, thd_arg->variables.query_alloc_block_size,
                 thd_arg->variables.query_prealloc_size, MYF(MY_THREAD_SPECIFIC));
  *last_error= '\0';
}


void Prepared_statement::setup_set_params()
{
  /*
    Note: BUG#25843 applies here too (query cache lookup uses thd->db, not
    db from "prepare" time).
  */
  if (query_cache_maybe_disabled(thd)) // we won't expand the query
    lex->safe_to_cache_query= FALSE;   // so don't cache it at Execution

  /*
    Decide if we have to expand the query (because we must write it to logs or
    because we want to look it up in the query cache) or not.
  */
  bool replace_params_with_values= false;
  // binlog
  replace_params_with_values|= mysql_bin_log.is_open() && is_update_query(lex->sql_command);
  // general or slow log
  replace_params_with_values|= opt_log || thd->variables.sql_log_slow;
  // query cache
  replace_params_with_values|= query_cache_is_cacheable_query(lex);
  // but never for compound statements
  replace_params_with_values&= lex->sql_command != SQLCOM_COMPOUND;

  if (replace_params_with_values)
  {
    set_params_from_actual_params= insert_params_from_actual_params_with_log;
#ifndef EMBEDDED_LIBRARY
    set_params= insert_params_with_log;
    set_bulk_params= insert_bulk_params; // RBR is on for bulk operation
#else
    //TODO: add bulk support for bulk parameters
    set_params_data= emb_insert_params_with_log;
#endif
  }
  else
  {
    set_params_from_actual_params= insert_params_from_actual_params;
#ifndef EMBEDDED_LIBRARY
    set_params= insert_params;
    set_bulk_params= insert_bulk_params;
#else
    //TODO: add bulk support for bulk parameters
    set_params_data= emb_insert_params;
#endif
  }
}


/**
  Destroy this prepared statement, cleaning up all used memory
  and resources.

  This is called from ::deallocate() to handle COM_STMT_CLOSE and
  DEALLOCATE PREPARE or when THD ends and all prepared statements are freed.
*/

Prepared_statement::~Prepared_statement()
{
  DBUG_ENTER("Prepared_statement::~Prepared_statement");
  DBUG_PRINT("enter",("stmt: %p  cursor: %p",
                      this, cursor));

  MYSQL_DESTROY_PS(m_prepared_stmt);

  delete cursor;
  /*
    We have to call free on the items even if cleanup is called as some items,
    like Item_param, don't free everything until free_items()
  */
  free_items();
  if (lex)
  {
    sp_head::destroy(lex->sphead);
    delete lex->result;
    delete (st_lex_local *) lex;
  }
  free_root(&main_mem_root, MYF(0));
  DBUG_VOID_RETURN;
}


Query_arena::Type Prepared_statement::type() const
{
  return PREPARED_STATEMENT;
}


bool Prepared_statement::cleanup_stmt(bool restore_set_statement_vars)
{
  bool error= false;
  DBUG_ENTER("Prepared_statement::cleanup_stmt");
  DBUG_PRINT("enter",("stmt: %p", this));

  if (restore_set_statement_vars)
    error= lex->restore_set_statement_var();

  thd->rollback_item_tree_changes();
  cleanup_items(free_list);
  thd->cleanup_after_query();

  DBUG_RETURN(error);
}


bool Prepared_statement::set_name(const LEX_CSTRING *name_arg)
{
  name.length= name_arg->length;
  name.str= (char*) memdup_root(mem_root, name_arg->str, name_arg->length);
  return name.str == 0;
}


/**
  Remember the current database.

  We must reset/restore the current database during execution of
  a prepared statement since it affects execution environment:
  privileges, @@character_set_database, and other.

  @return 1 if out of memory.
*/

bool
Prepared_statement::set_db(const LEX_CSTRING *db_arg)
{
  /* Remember the current database. */
  if (db_arg->length)
  {
    if (!(db.str= this->strmake(db_arg->str, db_arg->length)))
      return 1;
    db.length= db_arg->length;
  }
  else
    db= null_clex_str;
  return 0;
}

/**************************************************************************
  Common parts of mysql_[sql]_stmt_prepare, mysql_[sql]_stmt_execute.
  Essentially, these functions do all the magic of preparing/executing
  a statement, leaving network communication, input data handling and
  global THD state management to the caller.
***************************************************************************/

/**
  Parse statement text, validate the statement, and prepare it for execution.

    You should not change global THD state in this function, if at all
    possible: it may be called from any context, e.g. when executing
    a COM_* command, and SQLCOM_* command, or a stored procedure.

  @param packet             statement text
  @param packet_len

  @note
    Precondition:
    The caller must ensure that thd->change_list and thd->free_list
    is empty: this function will not back them up but will free
    in the end of its execution.

  @note
    Postcondition:
    thd->mem_root contains unused memory allocated during validation.
*/

bool Prepared_statement::prepare(const char *packet, uint packet_len)
{
  bool error;
  Statement stmt_backup;
  Query_arena *old_stmt_arena;
  DBUG_ENTER("Prepared_statement::prepare");
  DBUG_ASSERT(m_sql_mode == thd->variables.sql_mode);
  /*
    If this is an SQLCOM_PREPARE, we also increase Com_prepare_sql.
    However, it seems handy if com_stmt_prepare is increased always,
    no matter what kind of prepare is processed.
  */
  status_var_increment(thd->status_var.com_stmt_prepare);

  if (! (lex= new (mem_root) st_lex_local))
    DBUG_RETURN(TRUE);
  lex->stmt_lex= lex;

  if (set_db(&thd->db))
    DBUG_RETURN(TRUE);

  /*
    alloc_query() uses thd->mem_root && thd->query, so we should call
    both of backup_statement() and backup_query_arena() here.
  */
  thd->set_n_backup_statement(this, &stmt_backup);
  thd->set_n_backup_active_arena(this, &stmt_backup);

  if (alloc_query(thd, packet, packet_len))
  {
    thd->restore_backup_statement(this, &stmt_backup);
    thd->restore_active_arena(this, &stmt_backup);
    DBUG_RETURN(TRUE);
  }

  /*
    We'd like to have thd->query to be set to the actual query
    after the function ends.
    This value will be sent to audit plugins later.
    As the statement is created, the query will be stored
    in statement's arena. Normally the statement lives longer than
    the end of this query, so we can just set thd->query_string to
    be the stmt->query_string.
    Though errors can result in statement to be freed. These cases
    should be handled appropriately.
  */
  stmt_backup.query_string= thd->query_string;

  old_stmt_arena= thd->stmt_arena;
  thd->stmt_arena= this;
  auto save_cur_stmt= thd->cur_stmt;
  thd->cur_stmt= this;

  Parser_state parser_state;
  if (parser_state.init(thd, thd->query(), thd->query_length()))
  {
    thd->restore_backup_statement(this, &stmt_backup);
    thd->restore_active_arena(this, &stmt_backup);
    thd->stmt_arena= old_stmt_arena;
    thd->cur_stmt = save_cur_stmt;
    DBUG_RETURN(TRUE);
  }

  parser_state.m_lip.stmt_prepare_mode= TRUE;
  parser_state.m_lip.multi_statements= FALSE;

  lex_start(thd);
  lex->context_analysis_only|= CONTEXT_ANALYSIS_ONLY_PREPARE;


  error= (parse_sql(thd, & parser_state, NULL) ||
          thd->is_error() ||
          init_param_array(this));

  if (thd->security_ctx->password_expired &&
      lex->sql_command != SQLCOM_SET_OPTION &&
      lex->sql_command != SQLCOM_PREPARE &&
      lex->sql_command != SQLCOM_EXECUTE &&
      lex->sql_command != SQLCOM_DEALLOCATE_PREPARE)
  {
    thd->restore_backup_statement(this, &stmt_backup);
    thd->restore_active_arena(this, &stmt_backup);
    thd->stmt_arena= old_stmt_arena;
    thd->cur_stmt = save_cur_stmt;
    my_error(ER_MUST_CHANGE_PASSWORD, MYF(0));
    DBUG_RETURN(true);
  }
  lex->set_trg_event_type_for_tables();

  /*
    While doing context analysis of the query (in check_prepared_statement)
    we allocate a lot of additional memory: for open tables, JOINs, derived
    tables, etc.  Let's save a snapshot of current parse tree to the
    statement and restore original THD. In cases when some tree
    transformation can be reused on execute, we set again thd->mem_root from
    stmt->mem_root (see setup_wild for one place where we do that).
  */
  thd->restore_active_arena(this, &stmt_backup);

  /*
    If called from a stored procedure, ensure that we won't rollback
    external changes when cleaning up after validation.
  */
  DBUG_ASSERT(thd->Item_change_list::is_empty());

  /*
    Marker used to release metadata locks acquired while the prepared
    statement is being checked.
  */
  MDL_savepoint mdl_savepoint= thd->mdl_context.mdl_savepoint();

  /*
    Set variables specified by
      SET STATEMENT var1=value1 [, var2=value2, ...] FOR <statement>
    clause for duration of prepare phase. Original values of variable
    listed in the SET STATEMENT clause is restored right after return
    from the function check_prepared_statement()
  */
  if (likely(error == 0))
    error= run_set_statement_if_requested(thd, lex);

  /* 
   The only case where we should have items in the thd->free_list is
   after stmt->set_params_from_vars(), which may in some cases create
   Item_null objects.
  */

  if (likely(error == 0))
    error= check_prepared_statement(this);

  if (unlikely(error))
  {
    /*
      let the following code know we're not in PS anymore,
      the won't be any EXECUTE, so we need a full cleanup
    */
    lex->context_analysis_only&= ~CONTEXT_ANALYSIS_ONLY_PREPARE;
  }

  /* The order is important */
  lex->unit.cleanup();

  /* No need to commit statement transaction, it's not started. */
  DBUG_ASSERT(thd->transaction->stmt.is_empty());

  close_thread_tables_for_query(thd);
  thd->mdl_context.rollback_to_savepoint(mdl_savepoint);

  /*
    Transaction rollback was requested since MDL deadlock was discovered
    while trying to open tables. Rollback transaction in all storage
    engines including binary log and release all locks.

    Once dynamic SQL is allowed as substatements the below if-statement
    has to be adjusted to not do rollback in substatement.
  */
  DBUG_ASSERT(! thd->in_sub_stmt);
  if (thd->transaction_rollback_request)
  {
    trans_rollback_implicit(thd);
    thd->release_transactional_locks();
  }

  /* Preserve locked plugins for SET */
  if (lex->sql_command != SQLCOM_SET_OPTION)
    lex_unlock_plugins(lex);

  /*
    Pass the value true to restore original values of variables modified
    on handling SET STATEMENT clause.
  */
  error|= cleanup_stmt(true);

  thd->restore_backup_statement(this, &stmt_backup);
  thd->stmt_arena= old_stmt_arena;
  thd->cur_stmt= save_cur_stmt;

  if (likely(error == 0))
  {
    setup_set_params();
    lex->context_analysis_only&= ~CONTEXT_ANALYSIS_ONLY_PREPARE;
    state= Query_arena::STMT_PREPARED;
    flags&= ~ (uint) IS_IN_USE;

    MYSQL_SET_PS_TEXT(m_prepared_stmt, query(), query_length());

    /* 
      Log COM_EXECUTE to the general log. Note, that in case of SQL
      prepared statements this causes two records to be output:

      Query       PREPARE stmt from @user_variable
      Prepare     <statement SQL text>

      This is considered user-friendly, since in the
      second log entry we output the actual statement text.

      Do not print anything if this is an SQL prepared statement and
      we're inside a stored procedure (also called Dynamic SQL) --
      sub-statements inside stored procedures are not logged into
      the general log.
    */
    if (thd->spcont == NULL)
      general_log_write(thd, COM_STMT_PREPARE, query(), query_length());
  }
  DBUG_RETURN(error);
}


/**
  Assign parameter values either from variables, in case of SQL PS
  or from the execute packet.

  @param expanded_query  a container with the original SQL statement.
                         '?' placeholders will be replaced with
                         their values in case of success.
                         The result is used for logging and replication
  @param packet          pointer to execute packet.
                         NULL in case of SQL PS
  @param packet_end      end of the packet. NULL in case of SQL PS

  @todo Use a paremeter source class family instead of 'if's, and
  support stored procedure variables.

  @retval TRUE an error occurred when assigning a parameter (likely
          a conversion error or out of memory, or malformed packet)
  @retval FALSE success
*/

bool
Prepared_statement::set_parameters(String *expanded_query,
                                   uchar *packet, uchar *packet_end)
{
  bool is_sql_ps= packet == NULL;
  bool res= FALSE;

  if (is_sql_ps)
  {
    /* SQL prepared statement */
    res= set_params_from_actual_params(this, thd->lex->prepared_stmt.params(),
                                       expanded_query);
  }
  else if (param_count)
  {
#ifndef EMBEDDED_LIBRARY
    uchar *null_array= packet;
    res= (setup_conversion_functions(this, &packet) ||
          set_params(this, null_array, packet, packet_end, expanded_query));
#else
    /*
      In embedded library we re-install conversion routines each time
      we set parameters, and also we don't need to parse packet.
      So we do it in one function.
    */
    res= set_params_data(this, expanded_query);
#endif
  }
  if (res)
  {
    my_error(ER_WRONG_ARGUMENTS, MYF(0),
             is_sql_ps ? "EXECUTE" : "mysqld_stmt_execute");
    reset_stmt_params(this);
  }
  return res;
}


/**
  Execute a prepared statement. Re-prepare it a limited number
  of times if necessary.

  Try to execute a prepared statement. If there is a metadata
  validation error, prepare a new copy of the prepared statement,
  swap the old and the new statements, and try again.
  If there is a validation error again, repeat the above, but
  perform no more than MAX_REPREPARE_ATTEMPTS.

  @note We have to try several times in a loop since we
  release metadata locks on tables after prepared statement
  prepare. Therefore, a DDL statement may sneak in between prepare
  and execute of a new statement. If this happens repeatedly
  more than MAX_REPREPARE_ATTEMPTS times, we give up.

  @return TRUE if an error, FALSE if success
  @retval  TRUE    either MAX_REPREPARE_ATTEMPTS has been reached,
                   or some general error
  @retval  FALSE   successfully executed the statement, perhaps
                   after having reprepared it a few times.
*/
const static int MAX_REPREPARE_ATTEMPTS= 3;

bool
Prepared_statement::execute_loop(String *expanded_query,
                                 bool open_cursor,
                                 uchar *packet,
                                 uchar *packet_end)
{
  Reprepare_observer reprepare_observer;
  bool error;
  int reprepare_attempt= 0;
  iterations= FALSE;

  /*
    - In mysql_sql_stmt_execute() we hide all "external" Items
      e.g. those created in the "SET STATEMENT" part of the "EXECUTE" query.
    - In case of mysqld_stmt_execute() there should not be "external" Items.
  */
  DBUG_ASSERT(thd->free_list == NULL);

  /* Check if we got an error when sending long data */
  if (unlikely(state == Query_arena::STMT_ERROR))
  {
    my_message(last_errno, last_error, MYF(0));
    return TRUE;
  }

  if (set_parameters(expanded_query, packet, packet_end))
    return TRUE;
#ifdef WITH_WSREP
  if (thd->wsrep_delayed_BF_abort)
  {
    WSREP_DEBUG("delayed BF abort, quitting execute_loop, stmt: %d", id);
    return TRUE;
  }
#endif /* WITH_WSREP */
reexecute:
  // Make sure that reprepare() did not create any new Items.
  DBUG_ASSERT(thd->free_list == NULL);

  /*
    Install the metadata observer. If some metadata version is
    different from prepare time and an observer is installed,
    the observer method will be invoked to push an error into
    the error stack.
  */

  if (sql_command_flags[lex->sql_command] & CF_REEXECUTION_FRAGILE)
  {
    reprepare_observer.reset_reprepare_observer();
    DBUG_ASSERT(thd->m_reprepare_observer == NULL);
    thd->m_reprepare_observer= &reprepare_observer;
  }

  error= execute(expanded_query, open_cursor) || thd->is_error();

  thd->m_reprepare_observer= NULL;

  if (unlikely(error) &&
      (sql_command_flags[lex->sql_command] & CF_REEXECUTION_FRAGILE) &&
      !thd->is_fatal_error && !thd->killed &&
      reprepare_observer.is_invalidated() &&
      reprepare_attempt++ < MAX_REPREPARE_ATTEMPTS)
  {
    DBUG_ASSERT(thd->get_stmt_da()->sql_errno() == ER_NEED_REPREPARE);
    thd->clear_error();

    error= reprepare();

    if (likely(!error))                         /* Success */
      goto reexecute;
  }
  reset_stmt_params(this);

  return error;
}

my_bool bulk_parameters_set(THD *thd)
{
  DBUG_ENTER("bulk_parameters_set");
  Prepared_statement *stmt= (Prepared_statement *) thd->bulk_param;

  if (stmt && unlikely(stmt->set_bulk_parameters(FALSE)))
    DBUG_RETURN(TRUE);
  DBUG_RETURN(FALSE);
}

my_bool bulk_parameters_iterations(THD *thd)
{
  Prepared_statement *stmt= (Prepared_statement *) thd->bulk_param;
  if (!stmt)
    return FALSE;
  return stmt->bulk_iterations();
}


my_bool Prepared_statement::set_bulk_parameters(bool reset)
{
  DBUG_ENTER("Prepared_statement::set_bulk_parameters");
  DBUG_PRINT("info", ("iteration: %d", iterations));

  if (iterations)
  {
#ifndef EMBEDDED_LIBRARY
    if ((*set_bulk_params)(this, &packet, packet_end, reset))
#else
    // bulk parameters are not supported for embedded, so it will an error
#endif
    {
      my_error(ER_WRONG_ARGUMENTS, MYF(0),
               "mysqld_stmt_bulk_execute");
      reset_stmt_params(this);
      DBUG_RETURN(true);
    }
    if (packet >= packet_end)
      iterations= FALSE;
  }
  start_param= 0;
  DBUG_RETURN(false);
}

bool
Prepared_statement::execute_bulk_loop(String *expanded_query,
                                      bool open_cursor,
                                      uchar *packet_arg,
                                      uchar *packet_end_arg)
{
  Reprepare_observer reprepare_observer;
  unsigned char *readbuff= NULL;
  bool error= 0;
  packet= packet_arg;
  packet_end= packet_end_arg;
  iterations= TRUE;
  start_param= true;
#ifdef DBUG_ASSERT_EXISTS
  Item *free_list_state= thd->free_list;
#endif
  thd->set_bulk_execution((void *)this);
  /* Check if we got an error when sending long data */
  if (state == Query_arena::STMT_ERROR)
  {
    my_message(last_errno, last_error, MYF(0));
    goto err;
  }
  /* Check for non zero parameter count*/
  if (param_count == 0)
  {
    DBUG_PRINT("error", ("Statement with no parameters for bulk execution."));
    my_error(ER_UNSUPPORTED_PS, MYF(0));
    goto err;
  }

  if (!(sql_command_flags[lex->sql_command] & CF_PS_ARRAY_BINDING_SAFE))
  {
    DBUG_PRINT("error", ("Command is not supported in bulk execution."));
    my_error(ER_UNSUPPORTED_PS, MYF(0));
    goto err;
  }
  /*
     Here second buffer for not optimized commands,
     optimized commands do it inside thier internal loop.
  */
  if (!(sql_command_flags[lex->sql_command] & CF_PS_ARRAY_BINDING_OPTIMIZED) &&
      this->lex->has_returning())
  {
    // Above check can be true for SELECT in future
    DBUG_ASSERT(lex->sql_command != SQLCOM_SELECT);
    readbuff= thd->net.buff; // old buffer
    if (net_allocate_new_packet(&thd->net, thd, MYF(MY_THREAD_SPECIFIC)))
    {
      readbuff= NULL; // failure, net_allocate_new_packet keeps old buffer
      goto err;
    }
  }

#ifndef EMBEDDED_LIBRARY
  if (read_types &&
      set_conversion_functions(this, &packet))
#else
  // bulk parameters are not supported for embedded, so it will an error
#endif
  {
    my_error(ER_WRONG_ARGUMENTS, MYF(0),
            "mysqld_stmt_bulk_execute");
    goto err;
  }
  read_types= FALSE;

  // iterations changed by set_bulk_parameters
  while ((iterations || start_param) && !error && !thd->is_error())
  {
    int reprepare_attempt= 0;

    /*
      Here we set parameters for not optimized commands,
      optimized commands do it inside thier internal loop.
    */
    if (!(sql_command_flags[lex->sql_command] & CF_PS_ARRAY_BINDING_OPTIMIZED))
    {
      if (set_bulk_parameters(TRUE))
      {
        goto err;
      }
    }

reexecute:
    /*
      If the free_list is not empty, we'll wrongly free some externally
      allocated items when cleaning up after validation of the prepared
      statement.
    */
    DBUG_ASSERT(thd->free_list == free_list_state);

    /*
      Install the metadata observer. If some metadata version is
      different from prepare time and an observer is installed,
      the observer method will be invoked to push an error into
      the error stack.
    */

    if (sql_command_flags[lex->sql_command] & CF_REEXECUTION_FRAGILE)
    {
      reprepare_observer.reset_reprepare_observer();
      DBUG_ASSERT(thd->m_reprepare_observer == NULL);
      thd->m_reprepare_observer= &reprepare_observer;
    }

    error= execute(expanded_query, open_cursor) || thd->is_error();

    thd->m_reprepare_observer= NULL;

#ifdef WITH_WSREP
    if (!(sql_command_flags[lex->sql_command] & CF_PS_ARRAY_BINDING_OPTIMIZED) &&
	WSREP(thd))
    {
      if (wsrep_after_statement(thd))
      {
        /*
          Re-execution success is unlikely after an error from
          wsrep_after_statement(), so retrun error immediately.
        */
        thd->get_stmt_da()->reset_diagnostics_area();
        wsrep_override_error(thd, thd->wsrep_cs().current_error(),
                             thd->wsrep_cs().current_error_status());
      }
    }
    else
#endif /* WITH_WSREP */
    if (unlikely(error) &&
        (sql_command_flags[lex->sql_command] & CF_REEXECUTION_FRAGILE) &&
        !thd->is_fatal_error && !thd->killed &&
        reprepare_observer.is_invalidated() &&
        reprepare_attempt++ < MAX_REPREPARE_ATTEMPTS)
    {
      DBUG_ASSERT(thd->get_stmt_da()->sql_errno() == ER_NEED_REPREPARE);
      thd->clear_error();

      error= reprepare();

      if (likely(!error))                                /* Success */
        goto reexecute;
    }
  }
  reset_stmt_params(this);
  thd->set_bulk_execution(0);
  if (readbuff)
    my_free(readbuff);
  return error;

err:
  reset_stmt_params(this);
  thd->set_bulk_execution(0);
  if (readbuff)
    my_free(readbuff);
  return true;
}


bool
Prepared_statement::execute_server_runnable(Server_runnable *server_runnable)
{
  Statement stmt_backup;
  bool error;
  Query_arena *save_stmt_arena= thd->stmt_arena;
  Reprepare_observer *save_reprepare_observer= thd->m_reprepare_observer;
  Item_change_list save_change_list;

  thd->Item_change_list::move_elements_to(&save_change_list);

  state= STMT_CONVENTIONAL_EXECUTION;

  if (!(lex= new (mem_root) st_lex_local))
    return TRUE;

  thd->set_n_backup_statement(this, &stmt_backup);
  thd->set_n_backup_active_arena(this, &stmt_backup);

  thd->stmt_arena= this;
  thd->m_reprepare_observer= 0;

  error= server_runnable->execute_server_code(thd);

  thd->cleanup_after_query();

  thd->m_reprepare_observer= save_reprepare_observer;
  thd->restore_active_arena(this, &stmt_backup);
  thd->restore_backup_statement(this, &stmt_backup);
  thd->stmt_arena= save_stmt_arena;

  save_change_list.move_elements_to(thd);

  /* Items and memory will freed in destructor */

  return error;
}


/**
  Reprepare this prepared statement.

  Currently this is implemented by creating a new prepared
  statement, preparing it with the original query and then
  swapping the new statement and the original one.

  @retval  TRUE   an error occurred. Possible errors include
                  incompatibility of new and old result set
                  metadata
  @retval  FALSE  success, the statement has been reprepared
*/

bool
Prepared_statement::reprepare()
{
  char saved_cur_db_name_buf[SAFE_NAME_LEN+1];
  LEX_STRING saved_cur_db_name=
    { saved_cur_db_name_buf, sizeof(saved_cur_db_name_buf) };
  LEX_CSTRING stmt_db_name= db;
  bool cur_db_changed;
  bool error;

  Prepared_statement copy(thd);
  copy.m_sql_mode= m_sql_mode;

  copy.set_sql_prepare(); /* To suppress sending metadata to the client. */

  status_var_increment(thd->status_var.com_stmt_reprepare);

  if (unlikely(mysql_opt_change_db(thd, &stmt_db_name, &saved_cur_db_name,
                                   TRUE, &cur_db_changed)))
    return TRUE;

  Sql_mode_instant_set sms(thd, m_sql_mode);

  error= ((name.str && copy.set_name(&name)) ||
          copy.prepare(query(), query_length()) ||
          validate_metadata(&copy));

  if (cur_db_changed)
    mysql_change_db(thd, (LEX_CSTRING*) &saved_cur_db_name, TRUE);

  if (likely(!error))
  {
    MYSQL_REPREPARE_PS(m_prepared_stmt);
    swap_prepared_statement(&copy);
    swap_parameter_array(param_array, copy.param_array, param_count);
#ifdef DBUG_ASSERT_EXISTS
    is_reprepared= TRUE;
#endif
    /*
      Clear possible warnings during reprepare, it has to be completely
      transparent to the user. We use clear_warning_info() since
      there were no separate query id issued for re-prepare.
      Sic: we can't simply silence warnings during reprepare, because if
      it's failed, we need to return all the warnings to the user.
    */
    thd->get_stmt_da()->clear_warning_info(thd->query_id);
    column_info_state.reset();
  }
  else
  {
    /*
       Prepare failed and the 'copy' will be freed.
       Now we have to restore the query_string in the so the
       audit plugin later gets the meaningful notification.
    */
    thd->set_query(query(), query_length());
  }
  return error;
}


/**
  Validate statement result set metadata (if the statement returns
  a result set).

  Currently we only check that the number of columns of the result
  set did not change.
  This is a helper method used during re-prepare.

  @param[in]  copy  the re-prepared prepared statement to verify
                    the metadata of

  @retval TRUE  error, ER_PS_REBIND is reported
  @retval FALSE statement return no or compatible metadata
*/


bool Prepared_statement::validate_metadata(Prepared_statement *copy)
{
  /**
    If this is an SQL prepared statement or EXPLAIN,
    return FALSE -- the metadata of the original SELECT,
    if any, has not been sent to the client.
  */
  if (is_sql_prepare() || lex->describe)
    return FALSE;

  if (lex->first_select_lex()->item_list.elements !=
      copy->lex->first_select_lex()->item_list.elements)
  {
    /** Column counts mismatch, update the client */
    thd->server_status|= SERVER_STATUS_METADATA_CHANGED;
  }

  return FALSE;
}


/**
  Replace the original prepared statement with a prepared copy.

  This is a private helper that is used as part of statement
  reprepare

  @return This function does not return any errors.
*/

void
Prepared_statement::swap_prepared_statement(Prepared_statement *copy)
{
  Statement tmp_stmt;

  /* Swap memory roots. */
  swap_variables(MEM_ROOT, main_mem_root, copy->main_mem_root);

  /* Swap the arenas */
  tmp_stmt.set_query_arena(this);
  set_query_arena(copy);
  copy->set_query_arena(&tmp_stmt);

  /* Swap the statement parent classes */
  tmp_stmt.set_statement(this);
  set_statement(copy);
  copy->set_statement(&tmp_stmt);

  /* Swap ids back, we need the original id */
  swap_variables(ulong, id, copy->id);
  /* Swap mem_roots back, they must continue pointing at the main_mem_roots */
  swap_variables(MEM_ROOT *, mem_root, copy->mem_root);
  /*
    Swap the old and the new parameters array. The old array
    is allocated in the old arena.
  */
  swap_variables(Item_param **, param_array, copy->param_array);
  /* Don't swap flags: the copy has IS_SQL_PREPARE always set. */
  /* swap_variables(uint, flags, copy->flags); */
  /* Swap names, the old name is allocated in the wrong memory root */
  swap_variables(LEX_CSTRING, name, copy->name);
  /* Ditto */
  swap_variables(LEX_CSTRING, db, copy->db);

  DBUG_ASSERT(param_count == copy->param_count);
  DBUG_ASSERT(thd == copy->thd);
  last_error[0]= '\0';
  last_errno= 0;
}


/**
  Execute a prepared statement.

    You should not change global THD state in this function, if at all
    possible: it may be called from any context, e.g. when executing
    a COM_* command, and SQLCOM_* command, or a stored procedure.

  @param expanded_query     A query for binlogging which has all parameter
                            markers ('?') replaced with their actual values.
  @param open_cursor        True if an attempt to open a cursor should be made.
                            Currenlty used only in the binary protocol.

  @note
    Preconditions, postconditions.
    - See the comment for Prepared_statement::prepare().

  @retval
    FALSE	    ok
  @retval
    TRUE		Error
*/

bool Prepared_statement::execute(String *expanded_query, bool open_cursor)
{
  Statement stmt_backup;
  Query_arena *old_stmt_arena;
  bool error= TRUE;
  bool qc_executed= FALSE;

  char saved_cur_db_name_buf[SAFE_NAME_LEN+1];
  LEX_STRING saved_cur_db_name=
    { saved_cur_db_name_buf, sizeof(saved_cur_db_name_buf) };
  bool cur_db_changed;

  LEX_CSTRING stmt_db_name= db;

  status_var_increment(thd->status_var.com_stmt_execute);

  if (flags & (uint) IS_IN_USE)
  {
    my_error(ER_PS_NO_RECURSION, MYF(0));
    return TRUE;
  }

  /*
    For SHOW VARIABLES lex->result is NULL, as it's a non-SELECT
    command. For such queries we don't return an error and don't
    open a cursor -- the client library will recognize this case and
    materialize the result set.
    For SELECT statements lex->result is created in
    check_prepared_statement. lex->result->simple_select() is FALSE
    in INSERT ... SELECT and similar commands.
  */

  if (open_cursor && lex->result && lex->result->check_simple_select())
  {
    DBUG_PRINT("info",("Cursor asked for not SELECT stmt"));
    return TRUE;
  }

  /* In case the command has a call to SP which re-uses this statement name */
  flags|= IS_IN_USE;

  close_cursor();

  /*
    If the free_list is not empty, we'll wrongly free some externally
    allocated items when cleaning up after execution of this statement.
  */
  DBUG_ASSERT(thd->Item_change_list::is_empty());

  /* 
   The only case where we should have items in the thd->free_list is
   after stmt->set_params_from_vars(), which may in some cases create
   Item_null objects.
  */

  thd->set_n_backup_statement(this, &stmt_backup);

  /*
    Change the current database (if needed).

    Force switching, because the database of the prepared statement may be
    NULL (prepared statements can be created while no current database
    selected).
  */

  if (mysql_opt_change_db(thd, &stmt_db_name, &saved_cur_db_name, TRUE,
                          &cur_db_changed))
    goto error;

  /* Allocate query. */

  if (expanded_query->length() &&
      alloc_query(thd, expanded_query->ptr(), expanded_query->length()))
  {
    my_error(ER_OUTOFMEMORY, MYF(ME_FATAL), expanded_query->length());
    goto error;
  }
  /*
    Expanded query is needed for slow logging, so we want thd->query
    to point at it even after we restore from backup. This is ok, as
    expanded query was allocated in thd->mem_root.
  */
  stmt_backup.set_query_inner(thd->query_string);

  /*
    At first execution of prepared statement we may perform logical
    transformations of the query tree. Such changes should be performed
    on the parse tree of current prepared statement and new items should
    be allocated in its memory root. Set the appropriate pointer in THD
    to the arena of the statement.
  */
  old_stmt_arena= thd->stmt_arena;
  thd->stmt_arena= this;
  reinit_stmt_before_use(thd, lex);

  /* Go! */

  /*
    Log COM_EXECUTE to the general log. Note, that in case of SQL
    prepared statements this causes two records to be output:

    Query       EXECUTE <statement name>
    Execute     <statement SQL text>

    This is considered user-friendly, since in the
    second log entry we output values of parameter markers.

    Do not print anything if this is an SQL prepared statement and
    we're inside a stored procedure (also called Dynamic SQL) --
    sub-statements inside stored procedures are not logged into
    the general log.
  */

  if (thd->spcont == nullptr)
    general_log_write(thd, COM_STMT_EXECUTE, thd->query(), thd->query_length());

  if (open_cursor)
    error= mysql_open_cursor(thd, &result, &cursor);
  else
  {
    /*
      Try to find it in the query cache, if not, execute it.
      Note that multi-statements cannot exist here (they are not supported in
      prepared statements).
    */
    if (query_cache_send_result_to_client(thd, thd->query(),
                                          thd->query_length()) <= 0)
    {
      MYSQL_QUERY_EXEC_START(thd->query(), thd->thread_id, thd->get_db(),
                             &thd->security_ctx->priv_user[0],
                             (char *) thd->security_ctx->host_or_ip, 1);
      error= mysql_execute_command(thd, true);
      MYSQL_QUERY_EXEC_DONE(error);
      thd->update_server_status();
    }
    else
    {
      thd->lex->sql_command= SQLCOM_SELECT;
      status_var_increment(thd->status_var.com_stat[SQLCOM_SELECT]);
      thd->update_stats();
      qc_executed= TRUE;
    }
  }

  /*
    Restore the current database (if changed).

    Force switching back to the saved current database (if changed),
    because it may be NULL. In this case, mysql_change_db() would generate
    an error.
  */

  if (cur_db_changed)
    mysql_change_db(thd, (LEX_CSTRING*) &saved_cur_db_name, TRUE);

  /* Assert that if an error, no cursor is open */
  DBUG_ASSERT(! (error && cursor));

  if (! cursor)
    /*
      Pass the value false to don't restore set statement variables.
      See the next comment block for more details.
    */
    cleanup_stmt(false);

  /*
    Log the statement to slow query log if it passes filtering.
    We do it here for prepared statements despite of the fact that the function
    log_slow_statement() is also called upper the stack from the function
    dispatch_command(). The reason for logging slow queries here is that
    the function log_slow_statement() must be called before restoring system
    variables that could be set on execution of SET STATEMENT clause. Since
    for prepared statement restoring of system variables set on execution of
    SET STATEMENT clause is performed on return from the method
    Prepared_statement::execute(), by the time the function log_slow_statement()
    be invoked from the function dispatch_command() all variables set by
    the SET STATEMEN clause would be already reset to their original values
    that break semantic of the SET STATEMENT clause.

    E.g., lets consider the following statements
      SET slow_query_log= 1;
      SET @@long_query_time=0.01;
      PREPARE stmt FROM 'set statement slow_query_log=0 for select sleep(0.1)';
      EXECUTE stmt;

    It's expected that the above statements don't write any record
    to slow query log since the system variable slow_query_log is set to 0
    during execution of the whole statement
      'set statement slow_query_log=0 for select sleep(0.1)'

    However, if the function log_slow_statement wasn't called here the record
    for the statement would be written to slow query log since the variable
    slow_query_log is restored to its original value by the time the function
    log_slow_statement is called from disptach_command() to write a record
    into slow query log.
  */
  log_slow_statement(thd);

  error|= lex->restore_set_statement_var();


  /*
    EXECUTE command has its own dummy "explain data". We don't need it,
    instead, we want to keep the query plan of the statement that was 
    executed.
  */
  if (!stmt_backup.lex->explain || 
      !stmt_backup.lex->explain->have_query_plan())
  {
    delete_explain_query(stmt_backup.lex);
    stmt_backup.lex->explain = thd->lex->explain;
    thd->lex->explain= NULL;
  }
  else
    delete_explain_query(thd->lex);

  thd->set_statement(&stmt_backup);
  thd->stmt_arena= old_stmt_arena;

  if (state == Query_arena::STMT_PREPARED && !qc_executed)
    state= Query_arena::STMT_EXECUTED;

  if (likely(error == 0) && this->lex->sql_command == SQLCOM_CALL)
  {
    if (is_sql_prepare())
    {
      /*
        Here we have the diagnostics area status already set to DA_OK.
        sent_out_parameters() can raise errors when assigning OUT parameters:
          DECLARE a DATETIME;
          EXECUTE IMMEDIATE 'CALL p1(?)' USING a;
        when the procedure p1 assigns a DATETIME-incompatible value (e.g. 10)
        to the out parameter. Allow to overwrite status (to DA_ERROR).
      */
      thd->get_stmt_da()->set_overwrite_status(true);
      thd->protocol_text.send_out_parameters(&this->lex->param_list);
      thd->get_stmt_da()->set_overwrite_status(false);
    }
    else
      thd->protocol->send_out_parameters(&this->lex->param_list);
  }

error:
  error|= thd->lex->restore_set_statement_var();
  flags&= ~ (uint) IS_IN_USE;
  return error;
}


/**
  Prepare, execute and clean-up a statement.
  @param query  - query text
  @param length - query text length
  @retval true  - the query was not executed (parse error, wrong parameters)
  @retval false - the query was prepared and executed

  Note, if some error happened during execution, it still returns "false".
*/
bool Prepared_statement::execute_immediate(const char *query, uint query_len)
{
  DBUG_ENTER("Prepared_statement::execute_immediate");
  String expanded_query;
  static LEX_CSTRING execute_immediate_stmt_name=
    {STRING_WITH_LEN("(immediate)") };

  set_sql_prepare();
  name= execute_immediate_stmt_name;      // for DBUG_PRINT etc

  m_prepared_stmt= MYSQL_CREATE_PS(this, id, thd->m_statement_psi,
                                   name.str, name.length);

  if (prepare(query, query_len))
    DBUG_RETURN(true);

  if (param_count != thd->lex->prepared_stmt.param_count())
  {
    my_error(ER_WRONG_ARGUMENTS, MYF(0), "EXECUTE");
    deallocate_immediate();
    DBUG_RETURN(true);
  }

  MYSQL_EXECUTE_PS(thd->m_statement_psi, m_prepared_stmt);
  (void) execute_loop(&expanded_query, FALSE, NULL, NULL);
  deallocate_immediate();
  DBUG_RETURN(false);
}


/**
  Common part of DEALLOCATE PREPARE, EXECUTE IMMEDIATE, mysqld_stmt_close.
*/
void Prepared_statement::deallocate_immediate()
{
  /* We account deallocate in the same manner as mysqld_stmt_close */
  status_var_increment(thd->status_var.com_stmt_close);

  /* It should now be safe to reset CHANGE MASTER parameters */
  lex_end(lex);
}


/** Common part of DEALLOCATE PREPARE and mysqld_stmt_close. */

void Prepared_statement::deallocate()
{
  deallocate_immediate();
  /* Statement map calls delete stmt on erase */
  thd->stmt_map.erase(this);
}


/***************************************************************************
* Ed_result_set
***************************************************************************/
/**
  Use operator delete to free memory of Ed_result_set.
  Accessing members of a class after the class has been destroyed
  is a violation of the C++ standard but is commonly used in the
  server code.
*/

void Ed_result_set::operator delete(void *ptr, size_t size) throw ()
{
  if (ptr)
  {
    /*
      Make a stack copy, otherwise free_root() will attempt to
      write to freed memory.
    */
    MEM_ROOT own_root= ((Ed_result_set*) ptr)->m_mem_root;
    free_root(&own_root, MYF(0));
  }
}


/**
  Initialize an instance of Ed_result_set.

  Instances of the class, as well as all result set rows, are
  always allocated in the memory root passed over as the second
  argument. In the constructor, we take over ownership of the
  memory root. It will be freed when the class is destroyed.

  sic: Ed_result_est is not designed to be allocated on stack.
*/

Ed_result_set::Ed_result_set(List<Ed_row> *rows_arg,
                             size_t column_count_arg,
                             MEM_ROOT *mem_root_arg)
  :m_mem_root(*mem_root_arg),
  m_column_count(column_count_arg),
  m_rows(rows_arg),
  m_next_rset(NULL)
{
  /* Take over responsibility for the memory */
  clear_alloc_root(mem_root_arg);
}

/***************************************************************************
* Ed_result_set
***************************************************************************/

/**
  Create a new "execute direct" connection.
*/

Ed_connection::Ed_connection(THD *thd)
  :m_diagnostics_area(thd->query_id, false, true),
  m_thd(thd),
  m_rsets(0),
  m_current_rset(0)
{
}


/**
  Free all result sets of the previous statement, if any,
  and reset warnings and errors.

  Called before execution of the next query.
*/

void
Ed_connection::free_old_result()
{
  while (m_rsets)
  {
    Ed_result_set *rset= m_rsets->m_next_rset;
    delete m_rsets;
    m_rsets= rset;
  }
  m_current_rset= m_rsets;
  m_diagnostics_area.reset_diagnostics_area();
  m_diagnostics_area.clear_warning_info(m_thd->query_id);
}


/**
  A simple wrapper that uses a helper class to execute SQL statements.
*/

bool
Ed_connection::execute_direct(Protocol *p, LEX_STRING sql_text)
{
  Execute_sql_statement execute_sql_statement(sql_text);
  DBUG_PRINT("ed_query", ("%s", sql_text.str));

  return execute_direct(p, &execute_sql_statement);
}


/**
  Execute a fragment of server functionality without an effect on
  thd, and store results in memory.

  Conventions:
  - the code fragment must finish with OK, EOF or ERROR.
  - the code fragment doesn't have to close thread tables,
  free memory, commit statement transaction or do any other
  cleanup that is normally done in the end of dispatch_command().

  @param server_runnable A code fragment to execute.
*/

bool Ed_connection::execute_direct(Protocol *p, Server_runnable *server_runnable)
{
  bool rc= FALSE;
  Prepared_statement stmt(m_thd);
  Protocol *save_protocol= m_thd->protocol;
  Diagnostics_area *save_diagnostics_area= m_thd->get_stmt_da();

  DBUG_ENTER("Ed_connection::execute_direct");

  free_old_result(); /* Delete all data from previous execution, if any */

  m_thd->protocol= p;
  m_thd->set_stmt_da(&m_diagnostics_area);

  rc= stmt.execute_server_runnable(server_runnable);
  m_thd->protocol->end_statement();

  m_thd->protocol= save_protocol;
  m_thd->set_stmt_da(save_diagnostics_area);
  /*
    Protocol_local makes use of m_current_rset to keep
    track of the last result set, while adding result sets to the end.
    Reset it to point to the first result set instead.
  */
  m_current_rset= m_rsets;

  DBUG_RETURN(rc);
}


/**
  A helper method that is called only during execution.

  Although Ed_connection doesn't support multi-statements,
  a statement may generate many result sets. All subsequent
  result sets are appended to the end.

  @pre This is called only by Protocol_local.
*/

void
Ed_connection::add_result_set(Ed_result_set *ed_result_set)
{
  if (m_rsets)
  {
    m_current_rset->m_next_rset= ed_result_set;
    /* While appending, use m_current_rset as a pointer to the tail. */
    m_current_rset= ed_result_set;
  }
  else
    m_current_rset= m_rsets= ed_result_set;
}


/**
  Release ownership of the current result set to the client.

  Since we use a simple linked list for result sets,
  this method uses a linear search of the previous result
  set to exclude the released instance from the list.

  @todo Use double-linked list, when this is really used.

  XXX: This has never been tested with more than one result set!

  @pre There must be a result set.
*/

Ed_result_set *
Ed_connection::store_result_set()
{
  Ed_result_set *ed_result_set;

  DBUG_ASSERT(m_current_rset);

  if (m_current_rset == m_rsets)
  {
    /* Assign the return value */
    ed_result_set= m_current_rset;
    /* Exclude the return value from the list. */
    m_current_rset= m_rsets= m_rsets->m_next_rset;
  }
  else
  {
    Ed_result_set *prev_rset= m_rsets;
    /* Assign the return value. */
    ed_result_set= m_current_rset;

    /* Exclude the return value from the list */
    while (prev_rset->m_next_rset != m_current_rset)
      prev_rset= ed_result_set->m_next_rset;
    m_current_rset= prev_rset->m_next_rset= m_current_rset->m_next_rset;
  }
  ed_result_set->m_next_rset= NULL; /* safety */

  return ed_result_set;
}


#include <mysql.h>
#include "../libmysqld/embedded_priv.h"

class Protocol_local : public Protocol_text
{
public:
  struct st_mysql_data *cur_data;
  struct st_mysql_data *first_data;
  struct st_mysql_data **data_tail;
  void clear_data_list();
  struct st_mysql_data *alloc_new_dataset();
  char **next_field;
  MYSQL_FIELD *next_mysql_field;
  MEM_ROOT *alloc;
  THD *new_thd;
  Security_context empty_ctx;

  Protocol_local(THD *thd_arg, THD *new_thd_arg, ulong prealloc) :
    Protocol_text(thd_arg, prealloc),
    cur_data(0), first_data(0), data_tail(&first_data), alloc(0),
    new_thd(new_thd_arg)
  {}
 
protected:
  bool net_store_data(const uchar *from, size_t length);
  bool net_store_data_cs(const uchar *from, size_t length,
                         CHARSET_INFO *fromcs, CHARSET_INFO *tocs);
  bool net_send_eof(THD *thd, uint server_status, uint statement_warn_count);
  bool net_send_ok(THD *, uint, uint, ulonglong, ulonglong, const char *,
                   bool);
  bool net_send_error_packet(THD *, uint, const char *, const char *);
  bool begin_dataset();
  bool begin_dataset(THD *thd, uint numfields);

  bool write();
  bool flush();

  bool store_field_metadata(const THD *thd, const Send_field &field,
                            CHARSET_INFO *charset_for_protocol,
                            uint pos);
  bool send_result_set_metadata(List<Item> *list, uint flags);
  void remove_last_row();
  bool store_null();
  void prepare_for_resend();
  bool send_list_fields(List<Field> *list, const TABLE_LIST *table_list);
 
  enum enum_protocol_type type() { return PROTOCOL_LOCAL; };
};

static
bool
write_eof_packet_local(THD *thd,
    Protocol_local *p, uint server_status, uint statement_warn_count)
{
//  if (!thd->mysql)            // bootstrap file handling
//    return FALSE;
  /*
    The following test should never be true, but it's better to do it
    because if 'is_fatal_error' is set the server is not going to execute
    other queries (see the if test in dispatch_command / COM_QUERY)
  */
  if (thd->is_fatal_error)
    thd->server_status&= ~SERVER_MORE_RESULTS_EXISTS;
  p->cur_data->embedded_info->server_status= server_status;
  /*
    Don't send warn count during SP execution, as the warn_list
    is cleared between substatements, and mysqltest gets confused
  */
  p->cur_data->embedded_info->warning_count=
    (thd->spcont ? 0 : MY_MIN(statement_warn_count, 65535));
  return FALSE;
}


MYSQL_DATA *Protocol_local::alloc_new_dataset()
{
  MYSQL_DATA *data;
  struct embedded_query_result *emb_data;
  if (!my_multi_malloc(PSI_INSTRUMENT_ME, MYF(MY_WME | MY_ZEROFILL),
                       &data, sizeof(*data),
                       &emb_data, sizeof(*emb_data),
                       NULL))
    return NULL;

  emb_data->prev_ptr= &data->data;
  cur_data= data;
  *data_tail= data;
  data_tail= &emb_data->next;
  data->embedded_info= emb_data;
  return data;
}


void Protocol_local::clear_data_list()
{
  while (first_data)
  {
    MYSQL_DATA *data= first_data;
    first_data= data->embedded_info->next;
    free_rows(data);
  }
  data_tail= &first_data;
  free_rows(cur_data);
  cur_data= 0;
}


static char *dup_str_aux(MEM_ROOT *root, const char *from, uint length,
                         CHARSET_INFO *fromcs, CHARSET_INFO *tocs)
{
  uint32 dummy32;
  uint dummy_err;
  char *result;

  /* 'tocs' is set 0 when client issues SET character_set_results=NULL */
  if (tocs && String::needs_conversion(0, fromcs, tocs, &dummy32))
  {
    uint new_len= (tocs->mbmaxlen * length) / fromcs->mbminlen + 1;
    result= (char *)alloc_root(root, new_len);
    length= copy_and_convert(result, new_len,
                             tocs, from, length, fromcs, &dummy_err);
  }
  else
  {
    result= (char *)alloc_root(root, length + 1);
    memcpy(result, from, length);
  }

  result[length]= 0;
  return result;
}


static char *dup_str_aux(MEM_ROOT *root, const LEX_CSTRING &from,
                         CHARSET_INFO *fromcs, CHARSET_INFO *tocs)
{
  return dup_str_aux(root, from.str, (uint) from.length, fromcs, tocs);
}


bool Protocol_local::net_store_data(const uchar *from, size_t length)
{
  char *field_buf;
//  if (!thd->mysql)            // bootstrap file handling
//    return FALSE;

  if (!(field_buf= (char*) alloc_root(alloc, length + sizeof(uint) + 1)))
    return TRUE;
  *(uint *)field_buf= (uint) length;
  *next_field= field_buf + sizeof(uint);
  memcpy((uchar*) *next_field, from, length);
  (*next_field)[length]= 0;
  if (next_mysql_field->max_length < length)
    next_mysql_field->max_length= (unsigned long) length;
  ++next_field;
  ++next_mysql_field;
  return FALSE;
}


bool Protocol_local::net_store_data_cs(const uchar *from, size_t length,
                       CHARSET_INFO *from_cs, CHARSET_INFO *to_cs)
{
  uint conv_length= (uint) (to_cs->mbmaxlen * length / from_cs->mbminlen);
  uint dummy_error;
  char *field_buf;
//  if (!thd->mysql)            // bootstrap file handling
//    return false;

  if (!(field_buf= (char*) alloc_root(alloc, conv_length + sizeof(uint) + 1)))
    return true;
  *next_field= field_buf + sizeof(uint);
  length= copy_and_convert(*next_field, conv_length, to_cs,
                           (const char*) from, length, from_cs, &dummy_error);
  *(uint *) field_buf= (uint) length;
  (*next_field)[length]= 0;
  if (next_mysql_field->max_length < length)
    next_mysql_field->max_length= (unsigned long) length;
  ++next_field;
  ++next_mysql_field;
  return false;
}


/**
  Embedded library implementation of OK response.

  This function is used by the server to write 'OK' packet to
  the "network" when the server is compiled as an embedded library.
  Since there is no network in the embedded configuration,
  a different implementation is necessary.
  Instead of marshalling response parameters to a network representation
  and then writing it to the socket, here we simply copy the data to the
  corresponding client-side connection structures. 

  @sa Server implementation of net_send_ok in protocol.cc for
  description of the arguments.

  @return
    @retval TRUE An error occurred
    @retval FALSE Success
*/

bool
Protocol_local::net_send_ok(THD *thd,
  uint server_status, uint statement_warn_count,
  ulonglong affected_rows, ulonglong id, const char *message, bool)
{
  DBUG_ENTER("emb_net_send_ok");
  MYSQL_DATA *data;
//  MYSQL *mysql= thd->mysql;

//  if (!mysql)            // bootstrap file handling
//    DBUG_RETURN(FALSE);
  if (!(data= alloc_new_dataset()))
    DBUG_RETURN(TRUE);
  data->embedded_info->affected_rows= affected_rows;
  data->embedded_info->insert_id= id;
  if (message)
    strmake_buf(data->embedded_info->info, message);

  bool error= write_eof_packet_local(thd, this,
                                     server_status, statement_warn_count);
  cur_data= 0;
  DBUG_RETURN(error);
}


/**
  Embedded library implementation of EOF response.

  @sa net_send_ok

  @return
    @retval TRUE  An error occurred
    @retval FALSE Success
*/

bool
Protocol_local::net_send_eof(THD *thd, uint server_status,
                             uint statement_warn_count)
{
  bool error= write_eof_packet_local(thd, this, server_status,
                                     statement_warn_count);
  cur_data= 0;
  return error;
}


bool Protocol_local::net_send_error_packet(THD *thd, uint sql_errno,
       const char *err, const char *sqlstate)
{
  uint error;
  char converted_err[MYSQL_ERRMSG_SIZE];
  MYSQL_DATA *data= cur_data;
  struct embedded_query_result *ei;
 
//  if (!thd->mysql)            // bootstrap file handling
//  {
//    fprintf(stderr, "ERROR: %d  %s\n", sql_errno, err);
//    return TRUE;
//  }
  if (!data)
    data= alloc_new_dataset();

  ei= data->embedded_info;
  ei->last_errno= sql_errno;
  convert_error_message(converted_err, sizeof(converted_err),
                        thd->variables.character_set_results,
                        err, strlen(err),
                        system_charset_info, &error);
  /* Converted error message is always null-terminated. */
  strmake_buf(ei->info, converted_err);
  strmov(ei->sqlstate, sqlstate);
  ei->server_status= thd->server_status;
  cur_data= 0;
  return FALSE;
}


bool Protocol_local::begin_dataset()
{
  MYSQL_DATA *data= alloc_new_dataset();
  if (!data)
    return 1;
  alloc= &data->alloc;
  /* Assume rowlength < 8192 */
  init_alloc_root(PSI_INSTRUMENT_ME, alloc, 8192, 0, MYF(0));
  alloc->min_malloc= sizeof(MYSQL_ROWS);
  return 0;
}


bool Protocol_local::begin_dataset(THD *thd, uint numfields)
{
  if (begin_dataset())
    return true;
  MYSQL_DATA *data= cur_data;
  data->fields= field_count= numfields;
  if (!(data->embedded_info->fields_list=
      (MYSQL_FIELD*)alloc_root(&data->alloc, sizeof(MYSQL_FIELD)*field_count)))
    return true;
  return false;
}


bool Protocol_local::write()
{
//  if (!thd->mysql)            // bootstrap file handling
//    return false;

  *next_field= 0;
  return false;
}


bool Protocol_local::flush()
{
  return 0;
}


bool Protocol_local::store_field_metadata(const THD * thd,
                                          const Send_field &server_field,
                                          CHARSET_INFO *charset_for_protocol,
                                          uint pos)
{
  CHARSET_INFO *cs= system_charset_info;
  CHARSET_INFO *thd_cs= thd->variables.character_set_results;
  MYSQL_DATA *data= cur_data;
  MEM_ROOT *field_alloc= &data->alloc;
  MYSQL_FIELD *client_field= &cur_data->embedded_info->fields_list[pos];
  DBUG_ASSERT(server_field.is_sane());

  client_field->db= dup_str_aux(field_alloc, server_field.db_name,
                                cs, thd_cs);
  client_field->table= dup_str_aux(field_alloc, server_field.table_name,
                                   cs, thd_cs);
  client_field->name= dup_str_aux(field_alloc, server_field.col_name,
                                  cs, thd_cs);
  client_field->org_table= dup_str_aux(field_alloc, server_field.org_table_name,
                                       cs, thd_cs);
  client_field->org_name= dup_str_aux(field_alloc, server_field.org_col_name,
                                      cs, thd_cs);
  if (charset_for_protocol == &my_charset_bin || thd_cs == NULL)
  {
    /* No conversion */
    client_field->charsetnr= charset_for_protocol->number;
    client_field->length= server_field.length;
  }
  else
  {
    /* With conversion */
    client_field->charsetnr= thd_cs->number;
    client_field->length= server_field.max_octet_length(charset_for_protocol,
                                                        thd_cs);
  }
  client_field->type= server_field.type_handler()->type_code_for_protocol();
  client_field->flags= (uint16) server_field.flags;
  client_field->decimals= server_field.decimals;

  client_field->db_length=        (unsigned int) strlen(client_field->db);
  client_field->table_length=     (unsigned int) strlen(client_field->table);
  client_field->name_length=      (unsigned int) strlen(client_field->name);
  client_field->org_name_length=  (unsigned int) strlen(client_field->org_name);
  client_field->org_table_length= (unsigned int) strlen(client_field->org_table);

  client_field->catalog= dup_str_aux(field_alloc, "def", 3, cs, thd_cs);
  client_field->catalog_length= 3;

  if (IS_NUM(client_field->type))
    client_field->flags|= NUM_FLAG;

  client_field->max_length= 0;
  client_field->def= 0;
  return false;
}


void Protocol_local::remove_last_row()
{
  MYSQL_DATA *data= cur_data;
  MYSQL_ROWS **last_row_hook= &data->data;
  my_ulonglong count= data->rows;
  DBUG_ENTER("Protocol_text::remove_last_row");
  while (--count)
    last_row_hook= &(*last_row_hook)->next;

  *last_row_hook= 0;
  data->embedded_info->prev_ptr= last_row_hook;
  data->rows--;

  DBUG_VOID_RETURN;
}


bool Protocol_local::send_result_set_metadata(List<Item> *list, uint flags)
{
  List_iterator_fast<Item> it(*list);
  Item *item;
  DBUG_ENTER("send_result_set_metadata");

//  if (!thd->mysql)            // bootstrap file handling
//    DBUG_RETURN(0);

  if (begin_dataset(thd, list->elements))
    goto err;

  for (uint pos= 0 ; (item= it++); pos++)
  {
    if (store_item_metadata(thd, item, pos))
      goto err;
  }

  if (flags & SEND_EOF)
    write_eof_packet_local(thd, this, thd->server_status,
                     thd->get_stmt_da()->current_statement_warn_count());

  DBUG_RETURN(prepare_for_send(list->elements));
 err:
  my_error(ER_OUT_OF_RESOURCES, MYF(0));        /* purecov: inspected */
  DBUG_RETURN(1);				/* purecov: inspected */
}


static void
list_fields_send_default(THD *thd, Protocol_local *p, Field *fld, uint pos)
{
  char buff[80];
  String tmp(buff, sizeof(buff), default_charset_info), *res;
  MYSQL_FIELD *client_field= &p->cur_data->embedded_info->fields_list[pos];

  if (fld->is_null() || !(res= fld->val_str(&tmp)))
  {
    client_field->def_length= 0;
    client_field->def= strmake_root(&p->cur_data->alloc, "", 0);
  }
  else
  {
    client_field->def_length= res->length();
    client_field->def= strmake_root(&p->cur_data->alloc, res->ptr(),
                                    client_field->def_length);
  }
}


bool Protocol_local::send_list_fields(List<Field> *list, const TABLE_LIST *table_list)
{
  DBUG_ENTER("send_result_set_metadata");
  Protocol_text prot(thd);
  List_iterator_fast<Field> it(*list);
  Field *fld;

//  if (!thd->mysql)            // bootstrap file handling
//    DBUG_RETURN(0);

  if (begin_dataset(thd, list->elements))
    goto err;

  for (uint pos= 0 ; (fld= it++); pos++)
  {
    if (prot.store_field_metadata_for_list_fields(thd, fld, table_list, pos))
      goto err;
    list_fields_send_default(thd, this, fld, pos);
  }

  DBUG_RETURN(prepare_for_send(list->elements));
err:
  my_error(ER_OUT_OF_RESOURCES, MYF(0));
  DBUG_RETURN(1);
}


void Protocol_local::prepare_for_resend()
{
  MYSQL_ROWS *cur;
  MYSQL_DATA *data= cur_data;
  DBUG_ENTER("send_data");

//  if (!thd->mysql)            // bootstrap file handling
//    DBUG_VOID_RETURN;

  data->rows++;
  if (!(cur= (MYSQL_ROWS *)alloc_root(alloc, sizeof(MYSQL_ROWS)+(field_count + 1) * sizeof(char *))))
  {
    my_error(ER_OUT_OF_RESOURCES,MYF(0));
    DBUG_VOID_RETURN;
  }
  cur->data= (MYSQL_ROW)(((char *)cur) + sizeof(MYSQL_ROWS));

  *data->embedded_info->prev_ptr= cur;
  data->embedded_info->prev_ptr= &cur->next;
  next_field=cur->data;
  next_mysql_field= data->embedded_info->fields_list;
#ifndef DBUG_OFF
  field_pos= 0;
#endif

  DBUG_VOID_RETURN;
}

bool Protocol_local::store_null()
{
  *(next_field++)= NULL;
  ++next_mysql_field;
  return false;
}


#include <sql_common.h>
#include <errmsg.h>

static void embedded_get_error(MYSQL *mysql, MYSQL_DATA *data)
{
  NET *net= &mysql->net;
  struct embedded_query_result *ei= data->embedded_info;
  net->last_errno= ei->last_errno;
  strmake_buf(net->last_error, ei->info);
  memcpy(net->sqlstate, ei->sqlstate, sizeof(net->sqlstate));
  mysql->server_status= ei->server_status;
  my_free(data);
}


static my_bool loc_read_query_result(MYSQL *mysql)
{
  Protocol_local *p= (Protocol_local *) mysql->thd;

  MYSQL_DATA *res= p->first_data;
  DBUG_ASSERT(!p->cur_data);
  p->first_data= res->embedded_info->next;
  if (res->embedded_info->last_errno &&
      !res->embedded_info->fields_list)
  {
    embedded_get_error(mysql, res);
    return 1;
  }

  mysql->warning_count= res->embedded_info->warning_count;
  mysql->server_status= res->embedded_info->server_status;
  mysql->field_count= res->fields;
  if (!(mysql->fields= res->embedded_info->fields_list))
  {
    mysql->affected_rows= res->embedded_info->affected_rows;
    mysql->insert_id= res->embedded_info->insert_id;
  }
  net_clear_error(&mysql->net);
  mysql->info= 0;

  if (res->embedded_info->info[0])
  {
    strmake(mysql->info_buffer, res->embedded_info->info, MYSQL_ERRMSG_SIZE-1);
    mysql->info= mysql->info_buffer;
  }

  if (res->embedded_info->fields_list)
  {
    mysql->status=MYSQL_STATUS_GET_RESULT;
    p->cur_data= res;
  }
  else
    my_free(res);

  return 0;
}


static my_bool
loc_advanced_command(MYSQL *mysql, enum enum_server_command command,
                     const uchar *header, ulong header_length,
                     const uchar *arg, ulong arg_length, my_bool skip_check,
                     MYSQL_STMT *stmt)
{
  my_bool result= 1;
  Protocol_local *p= (Protocol_local *) mysql->thd;
  NET *net= &mysql->net;

  if (p->thd && p->thd->killed != NOT_KILLED)
  {
    if (p->thd->killed < KILL_CONNECTION)
      p->thd->killed= NOT_KILLED;
    else
      return 1;
  }

  p->clear_data_list();
  /* Check that we are calling the client functions in right order */
  if (mysql->status != MYSQL_STATUS_READY)
  {
    set_mysql_error(mysql, CR_COMMANDS_OUT_OF_SYNC, unknown_sqlstate);
    goto end;
  }

  /* Clear result variables */
  p->thd->clear_error(1);
  mysql->affected_rows= ~(my_ulonglong) 0;
  mysql->field_count= 0;
  net_clear_error(net);

  /* 
     We have to call free_old_query before we start to fill mysql->fields 
     for new query. In the case of embedded server we collect field data
     during query execution (not during data retrieval as it is in remote
     client). So we have to call free_old_query here
  */
  free_old_query(mysql);

  if (header)
  {
    arg= header;
    arg_length= header_length;
  }

  if (p->new_thd)
  {
    THD *thd_orig= current_thd;
    set_current_thd(p->thd);
    p->thd->thread_stack= (char*) &result;
    p->thd->set_time();
    result= execute_server_code(p->thd, (const char *)arg, arg_length);
    p->thd->cleanup_after_query();
    mysql_audit_release(p->thd);
    p->end_statement();
    set_current_thd(thd_orig);
  }
  else
  {
    Ed_connection con(p->thd);
    Security_context *ctx_orig= p->thd->security_ctx;
    MYSQL_LEX_STRING sql_text;
    DBUG_ASSERT(current_thd == p->thd);
    sql_text.str= (char *) arg;
    sql_text.length= arg_length;
    p->thd->security_ctx= &p->empty_ctx;
    result= con.execute_direct(p, sql_text);
    p->thd->security_ctx= ctx_orig;
  }
  if (skip_check)
    result= 0;
  p->cur_data= 0;

end:
  return result;
}


/*
  reads dataset from the next query result

  SYNOPSIS
  loc_read_rows()
  mysql		connection handle
  other parameters are not used

  NOTES
    It just gets next MYSQL_DATA from the result's queue

  RETURN
    pointer to MYSQL_DATA with the coming recordset
*/

static MYSQL_DATA *
loc_read_rows(MYSQL *mysql, MYSQL_FIELD *mysql_fields __attribute__((unused)),
              unsigned int fields __attribute__((unused)))
{
  MYSQL_DATA *result= ((Protocol_local *)mysql->thd)->cur_data;
  ((Protocol_local *)mysql->thd)->cur_data= 0;
  if (result->embedded_info->last_errno)
  {
    embedded_get_error(mysql, result);
    return NULL;
  }
  *result->embedded_info->prev_ptr= NULL;
  return result;
}


/**************************************************************************
  Get column lengths of the current row
  If one uses mysql_use_result, res->lengths contains the length information,
  else the lengths are calculated from the offset between pointers.
**************************************************************************/

static void loc_fetch_lengths(ulong *to, MYSQL_ROW column,
                              unsigned int field_count)
{
  MYSQL_ROW end;

  for (end=column + field_count; column != end ; column++,to++)
    *to= *column ? *(uint *)((*column) - sizeof(uint)) : 0;
}


static void loc_flush_use_result(MYSQL *mysql, my_bool)
{
  Protocol_local *p= (Protocol_local *) mysql->thd;
  if (p->cur_data)
  {
    free_rows(p->cur_data);
    p->cur_data= 0;
  }
  else if (p->first_data)
  {
    MYSQL_DATA *data= p->first_data;
    p->first_data= data->embedded_info->next;
    free_rows(data);
  }
}


static void loc_on_close_free(MYSQL *mysql)
{
  Protocol_local *p= (Protocol_local *) mysql->thd;
  THD *thd= p->new_thd;
  delete p;
  if (thd)
  {
    delete thd;
    local_connection_thread_count--;
  }
  my_free(mysql->info_buffer);
  mysql->info_buffer= 0;
}

static MYSQL_RES *loc_use_result(MYSQL *mysql)
{
  return mysql_store_result(mysql);
}

static MYSQL_METHODS local_methods=
{
  loc_read_query_result,                       /* read_query_result */
  loc_advanced_command,                        /* advanced_command */
  loc_read_rows,                               /* read_rows */
  loc_use_result,                              /* use_result */
  loc_fetch_lengths,                           /* fetch_lengths */
  loc_flush_use_result,                        /* flush_use_result */
  NULL,                                        /* read_change_user_result */
  loc_on_close_free                            /* on_close_free */
#ifdef EMBEDDED_LIBRARY
  ,NULL,                                       /* list_fields */
  NULL,                                        /* read_prepare_result */
  NULL,                                        /* stmt_execute */
  NULL,                                        /* read_binary_rows */
  NULL,                                        /* unbuffered_fetch */
  NULL,                                        /* read_statistics */
  NULL,                                        /* next_result */
  NULL                                         /* read_rows_from_cursor */
#endif
};


Atomic_counter<uint32_t> local_connection_thread_count;

extern "C" MYSQL *mysql_real_connect_local(MYSQL *mysql)
{
  THD *thd_orig= current_thd;
  THD *new_thd;
  Protocol_local *p;
  DBUG_ENTER("mysql_real_connect_local");

  /* Test whether we're already connected */
  if (mysql->server_version)
  {
    set_mysql_error(mysql, CR_ALREADY_CONNECTED, unknown_sqlstate);
    DBUG_RETURN(0);
  }

  mysql->methods= &local_methods;
  mysql->user= NULL;

  mysql->info_buffer= (char *) my_malloc(PSI_INSTRUMENT_ME,
                                         MYSQL_ERRMSG_SIZE, MYF(0));
  if (!thd_orig || thd_orig->lock)
  {
    /*
      When we start with the empty current_thd (that happens when plugins
      are loaded during the server start) or when some tables are locked
      with the current_thd already (that happens when INSTALL PLUGIN
      calls the plugin_init or with queries), we create the new THD for
      the local connection. So queries with this MYSQL will be run with
      it rather than the current THD.
    */

    new_thd= new THD(0);
    local_connection_thread_count++;
    new_thd->thread_stack= (char*) &thd_orig;
    new_thd->store_globals();
    new_thd->security_ctx->skip_grants();
    new_thd->query_cache_is_applicable= 0;
    new_thd->variables.wsrep_on= 0;
    /*
      TOSO: decide if we should turn the auditing off
      for such threads.
      We can do it like this:
        new_thd->audit_class_mask[0]= ~0;
    */
    bzero((char*) &new_thd->net, sizeof(new_thd->net));
    set_current_thd(thd_orig);
    thd_orig= new_thd;
  }
  else
    new_thd= NULL;

  p= new Protocol_local(thd_orig, new_thd, 0);
  if (new_thd)
    new_thd->protocol= p;
  else
  {
    p->empty_ctx.init();
    p->empty_ctx.skip_grants();
  }

  mysql->thd= p;
  mysql->server_status= SERVER_STATUS_AUTOCOMMIT;


  DBUG_PRINT("exit",("Mysql handler: %p", mysql));
  DBUG_RETURN(mysql);
}

