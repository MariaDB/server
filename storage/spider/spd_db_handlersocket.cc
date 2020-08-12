/* Copyright (C) 2012-2018 Kentoku Shiba

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

#define MYSQL_SERVER 1
#include <my_global.h>
#include "mysql_version.h"
#include "spd_environ.h"
#if MYSQL_VERSION_ID < 50500
#include "mysql_priv.h"
#include <mysql/plugin.h>
#else
#include "sql_priv.h"
#include "probes_mysql.h"
#include "sql_analyse.h"
#endif

#if defined(HS_HAS_SQLCOM) && defined(HAVE_HANDLERSOCKET)
#include "spd_err.h"
#include "spd_param.h"
#include "spd_db_include.h"
#include "spd_include.h"
#include "spd_db_handlersocket.h"
#include "ha_spider.h"
#include "spd_db_conn.h"
#include "spd_trx.h"
#include "spd_conn.h"
#include "spd_malloc.h"

extern handlerton *spider_hton_ptr;
extern HASH spider_open_connections;
extern HASH spider_ipport_conns;
extern SPIDER_DBTON spider_dbton[SPIDER_DBTON_SIZE];
extern const char spider_dig_upper[];

#define SPIDER_SQL_INTERVAL_STR " + interval "
#define SPIDER_SQL_INTERVAL_LEN (sizeof(SPIDER_SQL_INTERVAL_STR) - 1)
#define SPIDER_SQL_NEGINTERVAL_STR " - interval "
#define SPIDER_SQL_NEGINTERVAL_LEN (sizeof(SPIDER_SQL_NEGINTERVAL_STR) - 1)

#define SPIDER_SQL_NAME_QUOTE_STR ""
#define SPIDER_SQL_NAME_QUOTE_LEN (sizeof(SPIDER_SQL_NAME_QUOTE_STR) - 1)
static const char *name_quote_str = SPIDER_SQL_NAME_QUOTE_STR;

#define SPIDER_SQL_TYPE_FULL_HS (SPIDER_SQL_TYPE_SELECT_HS | \
  SPIDER_SQL_TYPE_INSERT_HS | SPIDER_SQL_TYPE_UPDATE_HS | \
  SPIDER_SQL_TYPE_DELETE_HS | SPIDER_SQL_TYPE_OTHER_HS)

static uchar SPIDER_SQL_LINESTRING_HEAD_STR[] =
  {0x00,0x00,0x00,0x00,0x01,0x02,0x00,0x00,0x00,0x02,0x00,0x00,0x00};
#define SPIDER_SQL_LINESTRING_HEAD_LEN sizeof(SPIDER_SQL_LINESTRING_HEAD_STR)

static const char *spider_db_timefunc_interval_str[] =
{
  " year", " quarter", " month", " week", " day",
  " hour", " minute", " second", " microsecond",
  " year_month", " day_hour", " day_minute",
  " day_second", " hour_minute", " hour_second",
  " minute_second", " day_microsecond", " hour_microsecond",
  " minute_microsecond", " second_microsecond"
};

static SPIDER_HS_STRING_REF spider_null_string_ref = SPIDER_HS_STRING_REF();

int spider_handlersocket_init()
{
  DBUG_ENTER("spider_handlersocket_init");
  DBUG_RETURN(0);
}

int spider_handlersocket_deinit()
{
  DBUG_ENTER("spider_handlersocket_deinit");
  DBUG_RETURN(0);
}

spider_db_share *spider_handlersocket_create_share(
  SPIDER_SHARE *share
) {
  DBUG_ENTER("spider_handlersocket_create_share");
  DBUG_RETURN(new spider_handlersocket_share(share));
}

spider_db_handler *spider_handlersocket_create_handler(
  ha_spider *spider,
  spider_db_share *db_share
) {
  DBUG_ENTER("spider_handlersocket_create_handler");
  DBUG_RETURN(new spider_handlersocket_handler(spider,
    (spider_handlersocket_share *) db_share));
}

SPIDER_DB_CONN *spider_handlersocket_create_conn(
  SPIDER_CONN *conn
) {
  DBUG_ENTER("spider_handlersocket_create_conn");
  DBUG_RETURN(new spider_db_handlersocket(conn));
}

bool spider_handlersocket_support_direct_join(
) {
  DBUG_ENTER("spider_handlersocket_support_direct_join");
  DBUG_RETURN(FALSE);
}

spider_db_handlersocket_util spider_db_handlersocket_utility;

SPIDER_DBTON spider_dbton_handlersocket = {
  0,
  SPIDER_DB_WRAPPER_MYSQL,
  SPIDER_DB_ACCESS_TYPE_NOSQL,
  spider_handlersocket_init,
  spider_handlersocket_deinit,
  spider_handlersocket_create_share,
  spider_handlersocket_create_handler,
  NULL,
  spider_handlersocket_create_conn,
  spider_handlersocket_support_direct_join,
  &spider_db_handlersocket_utility,
  "For communicating using the handlersocket protocol",
  "0.1.0",
  SPIDER_MATURITY_BETA
};

#ifndef HANDLERSOCKET_MYSQL_UTIL
spider_db_hs_string_ref_buffer::spider_db_hs_string_ref_buffer()
{
  DBUG_ENTER("spider_db_hs_string_ref_buffer::spider_db_hs_string_ref_buffer");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_VOID_RETURN;
}

spider_db_hs_string_ref_buffer::~spider_db_hs_string_ref_buffer()
{
  DBUG_ENTER("spider_db_hs_string_ref_buffer::~spider_db_hs_string_ref_buffer");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_VOID_RETURN;
}

int spider_db_hs_string_ref_buffer::init()
{
  DBUG_ENTER("spider_db_hs_string_ref_buffer::init");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(0);
}

void spider_db_hs_string_ref_buffer::clear()
{
  DBUG_ENTER("spider_db_hs_string_ref_buffer::clear");
  DBUG_PRINT("info",("spider this=%p", this));
  hs_conds.clear();
  DBUG_VOID_RETURN;
}

int spider_db_hs_string_ref_buffer::push_back(
  SPIDER_HS_STRING_REF &cond
) {
  DBUG_ENTER("spider_db_hs_string_ref_buffer::push_back");
  DBUG_PRINT("info",("spider this=%p", this));
  hs_conds.push_back(cond);
  DBUG_RETURN(0);
}

SPIDER_HS_STRING_REF *spider_db_hs_string_ref_buffer::ptr()
{
  DBUG_ENTER("spider_db_hs_string_ref_buffer::ptr");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(&hs_conds[0]);
}

uint spider_db_hs_string_ref_buffer::size()
{
  DBUG_ENTER("spider_db_hs_string_ref_buffer::size");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN((uint) hs_conds.size());
}
#else
spider_db_hs_string_ref_buffer::spider_db_hs_string_ref_buffer() : hs_da_init(FALSE)
{
  DBUG_ENTER("spider_db_hs_string_ref_buffer::spider_db_hs_string_ref_buffer");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_VOID_RETURN;
}

spider_db_hs_string_ref_buffer::~spider_db_hs_string_ref_buffer()
{
  DBUG_ENTER("spider_db_hs_string_ref_buffer::~spider_db_hs_string_ref_buffer");
  DBUG_PRINT("info",("spider this=%p", this));
  if (hs_da_init)
  {
    spider_free_mem_calc(spider_current_trx,
      hs_conds_id, hs_conds.max_element * hs_conds.size_of_element);
    delete_dynamic(&hs_conds);
  }
  DBUG_VOID_RETURN;
}

int spider_db_hs_string_ref_buffer::init()
{
  DBUG_ENTER("spider_db_hs_string_ref_buffer::init");
  DBUG_PRINT("info",("spider this=%p", this));
  if (!hs_da_init)
  {
    SPD_INIT_DYNAMIC_ARRAY2(&hs_conds, sizeof(SPIDER_HS_STRING_REF),
      NULL, 16, 16, MYF(MY_WME));
    spider_alloc_calc_mem_init(hs_conds, 159);
    spider_alloc_calc_mem(spider_current_trx,
      hs_conds, hs_conds.max_element * hs_conds.size_of_element);
    hs_da_init = TRUE;
  }
  DBUG_RETURN(0);
}

void spider_db_hs_string_ref_buffer::clear()
{
  DBUG_ENTER("spider_db_hs_string_ref_buffer::clear");
  DBUG_PRINT("info",("spider this=%p", this));
  hs_conds.elements = 0;
  DBUG_VOID_RETURN;
}

int spider_db_hs_string_ref_buffer::push_back(
  SPIDER_HS_STRING_REF &cond
) {
  uint old_elements = hs_conds.max_element;
  DBUG_ENTER("spider_db_hs_string_ref_buffer::push_back");
  DBUG_PRINT("info",("spider this=%p", this));
  if (insert_dynamic(&hs_conds, (uchar *) &cond))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  if (hs_conds.max_element > old_elements)
  {
    spider_alloc_calc_mem(spider_current_trx,
      hs_conds,
      (hs_conds.max_element - old_elements) * hs_conds.size_of_element);
  }
  DBUG_RETURN(0);
}

SPIDER_HS_STRING_REF *spider_db_hs_string_ref_buffer::ptr()
{
  DBUG_ENTER("spider_db_hs_string_ref_buffer::ptr");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN((SPIDER_HS_STRING_REF *) hs_conds.buffer);
}

uint spider_db_hs_string_ref_buffer::size()
{
  DBUG_ENTER("spider_db_hs_string_ref_buffer::size");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(hs_conds.elements);
}
#endif

spider_db_hs_str_buffer::spider_db_hs_str_buffer() : hs_da_init(FALSE)
{
  DBUG_ENTER("spider_db_hs_str_buffer::spider_db_hs_str_buffer");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_VOID_RETURN;
}

spider_db_hs_str_buffer::~spider_db_hs_str_buffer()
{
  DBUG_ENTER("spider_db_hs_str_buffer::~spider_db_hs_str_buffer");
  DBUG_PRINT("info",("spider this=%p", this));
  if (hs_da_init)
  {
    spider_free_mem_calc(spider_current_trx,
      hs_conds_id, hs_conds.max_element * hs_conds.size_of_element);
    delete_dynamic(&hs_conds);
  }
  DBUG_VOID_RETURN;
}

int spider_db_hs_str_buffer::init()
{
  DBUG_ENTER("spider_db_hs_str_buffer::init");
  DBUG_PRINT("info",("spider this=%p", this));
  if (!hs_da_init)
  {
    SPD_INIT_DYNAMIC_ARRAY2(&hs_conds, sizeof(spider_string *),
      NULL, 16, 16, MYF(MY_WME));
    spider_alloc_calc_mem_init(hs_conds, 160);
    spider_alloc_calc_mem(spider_current_trx,
      hs_conds, hs_conds.max_element * hs_conds.size_of_element);
    hs_da_init = TRUE;
  }
  DBUG_RETURN(0);
}

void spider_db_hs_str_buffer::clear()
{
  uint i;
  spider_string *element;
  DBUG_ENTER("spider_db_hs_str_buffer::clear");
  DBUG_PRINT("info",("spider this=%p", this));
  for (i = 0; i < hs_conds.elements; i++)
  {
    get_dynamic(&hs_conds, (uchar *) &element, i);
    element->free();
    spider_free(spider_current_trx, element, MYF(0));
  }
  hs_conds.elements = 0;
  DBUG_VOID_RETURN;
}

spider_string *spider_db_hs_str_buffer::add(
  uint *strs_pos,
  const char *str,
  uint str_len
) {
  spider_string *element;
  DBUG_ENTER("spider_db_hs_str_buffer::add");
  DBUG_PRINT("info",("spider this=%p", this));
  if (hs_conds.elements <= *strs_pos + 1)
  {
    if (!(element = (spider_string *) spider_malloc(spider_current_trx, 8,
      sizeof(spider_string), MYF(MY_WME | MY_ZEROFILL))))
      DBUG_RETURN(NULL);
    element->init_calc_mem(98);
    element->set_charset(&my_charset_bin);
    if ((element->reserve(str_len + 1)))
    {
      spider_free(spider_current_trx, element, MYF(0));
      DBUG_RETURN(NULL);
    }
    element->q_append(str, str_len);
    uint old_elements = hs_conds.max_element;
    if (insert_dynamic(&hs_conds, (uchar *) &element))
    {
      element->free();
      spider_free(spider_current_trx, element, MYF(0));
      DBUG_RETURN(NULL);
    }
    if (hs_conds.max_element > old_elements)
    {
      spider_alloc_calc_mem(spider_current_trx,
        hs_conds,
        (hs_conds.max_element - old_elements) *
        hs_conds.size_of_element);
    }
  } else {
    element = ((spider_string **) hs_conds.buffer)[*strs_pos];
    element->length(0);
    if ((element->reserve(str_len + 1)))
      DBUG_RETURN(NULL);
    element->q_append(str, str_len);
  }
  (*strs_pos)++;
  DBUG_RETURN(element);
}

spider_db_handlersocket_row::spider_db_handlersocket_row() :
  spider_db_row(spider_dbton_handlersocket.dbton_id),
  hs_row(NULL), field_count(0), row_size(0), cloned(FALSE)
{
  DBUG_ENTER("spider_db_handlersocket_row::spider_db_handlersocket_row");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_VOID_RETURN;
}

spider_db_handlersocket_row::~spider_db_handlersocket_row()
{
  DBUG_ENTER("spider_db_handlersocket_row::~spider_db_handlersocket_row");
  DBUG_PRINT("info",("spider this=%p", this));
  if (cloned)
  {
    spider_free(spider_current_trx, hs_row_first, MYF(0));
  }
  DBUG_VOID_RETURN;
}

int spider_db_handlersocket_row::store_to_field(
  Field *field,
  CHARSET_INFO *access_charset
) {
  DBUG_ENTER("spider_db_handlersocket_row::store_to_field");
  DBUG_PRINT("info",("spider this=%p", this));
  if (!hs_row->begin())
  {
    DBUG_PRINT("info", ("spider field is null"));
    field->set_null();
    field->reset();
  } else {
#ifndef DBUG_OFF
    char buf[MAX_FIELD_WIDTH];
    spider_string tmp_str(buf, MAX_FIELD_WIDTH, field->charset());
    tmp_str.init_calc_mem(119);
    tmp_str.length(0);
    tmp_str.append(hs_row->begin(), hs_row->size(), &my_charset_bin);
    DBUG_PRINT("info", ("spider val=%s", tmp_str.c_ptr_safe()));
#endif
    field->set_notnull();
    if (field->flags & BLOB_FLAG)
    {
      DBUG_PRINT("info", ("spider blob field"));
      ((Field_blob *)field)->set_ptr(
        hs_row->size(), (uchar *) hs_row->begin());
    } else
      field->store(hs_row->begin(), hs_row->size(), &my_charset_bin);
  }
  DBUG_RETURN(0);
}

int spider_db_handlersocket_row::append_to_str(
  spider_string *str
) {
  DBUG_ENTER("spider_db_handlersocket_row::append_to_str");
  DBUG_PRINT("info",("spider this=%p", this));
  if (str->reserve(hs_row->size()))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  str->q_append(hs_row->begin(), hs_row->size());
  DBUG_RETURN(0);
}

int spider_db_handlersocket_row::append_escaped_to_str(
  spider_string *str,
  uint dbton_id
) {
  DBUG_ENTER("spider_db_handlersocket_row::append_escaped_to_str");
  DBUG_PRINT("info",("spider this=%p", this));
  spider_string tmp_str(hs_row->begin(), hs_row->size() + 1, &my_charset_bin);
  tmp_str.init_calc_mem(172);
  tmp_str.length(hs_row->size());
  if (str->reserve(hs_row->size() * 2 + 2))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  spider_dbton[dbton_id].db_util->append_escaped_util(str, tmp_str.get_str());
  str->mem_calc();
  DBUG_RETURN(0);
}

void spider_db_handlersocket_row::first()
{
  DBUG_ENTER("spider_db_handlersocket_row::first");
  DBUG_PRINT("info",("spider this=%p", this));
  hs_row = hs_row_first;
  DBUG_VOID_RETURN;
}

void spider_db_handlersocket_row::next()
{
  DBUG_ENTER("spider_db_handlersocket_row::next");
  DBUG_PRINT("info",("spider this=%p", this));
  hs_row++;
  DBUG_VOID_RETURN;
}

bool spider_db_handlersocket_row::is_null()
{
  DBUG_ENTER("spider_db_handlersocket_row::is_null");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(!hs_row->begin());
}

int spider_db_handlersocket_row::val_int()
{
  DBUG_ENTER("spider_db_handlersocket_row::val_int");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(atoi(hs_row->begin()));
}

double spider_db_handlersocket_row::val_real()
{
  DBUG_ENTER("spider_db_handlersocket_row::val_real");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(hs_row->begin() ? my_atof(hs_row->begin()) : 0.0);
}

my_decimal *spider_db_handlersocket_row::val_decimal(
  my_decimal *decimal_value,
  CHARSET_INFO *access_charset
) {
  DBUG_ENTER("spider_db_handlersocket_row::val_decimal");
  DBUG_PRINT("info",("spider this=%p", this));
  if (!hs_row->begin())
    DBUG_RETURN(NULL);

#ifdef SPIDER_HAS_DECIMAL_OPERATION_RESULTS_VALUE_TYPE
  decimal_operation_results(str2my_decimal(0, hs_row->begin(), hs_row->size(),
    access_charset, decimal_value), "", "");
#else
  decimal_operation_results(str2my_decimal(0, hs_row->begin(), hs_row->size(),
    access_charset, decimal_value));
#endif

  DBUG_RETURN(decimal_value);
}

SPIDER_DB_ROW *spider_db_handlersocket_row::clone()
{
  spider_db_handlersocket_row *clone_row;
  char *tmp_char;
  uint i;
  DBUG_ENTER("spider_db_handlersocket_row::clone");
  DBUG_PRINT("info",("spider this=%p", this));
  if (!(clone_row = new spider_db_handlersocket_row(dbton_id)))
  {
    DBUG_RETURN(NULL);
  }
  if (!spider_bulk_malloc(spider_current_trx, 169, MYF(MY_WME),
    &clone_row->hs_row, (uint) (sizeof(SPIDER_HS_STRING_REF) * field_count),
    &tmp_char, (uint) (row_size),
    NullS)
  ) {
    delete clone_row;
    DBUG_RETURN(NULL);
  }
  for (i = 0; i < field_count; i++)
  {
    memcpy(tmp_char, hs_row_first[i].begin(), hs_row_first[i].size());
    clone_row->hs_row[i].set(tmp_char, hs_row_first[i].size());
    tmp_char += hs_row_first[i].size();
  }
  clone_row->hs_row_first = clone_row->hs_row;
  clone_row->cloned = TRUE;;
  clone_row->row_size = row_size;;
  DBUG_RETURN(NULL);
}

int spider_db_handlersocket_row::store_to_tmp_table(
  TABLE *tmp_table,
  spider_string *str
) {
  uint i;
  SPIDER_HS_STRING_REF *tmp_hs_row = hs_row;
  DBUG_ENTER("spider_db_handlersocket_row::store_to_tmp_table");
  DBUG_PRINT("info",("spider this=%p", this));
  str->length(0);
  for (i = 0; i < field_count; i++)
  {
    if (tmp_hs_row->begin())
    {
      if (str->reserve(tmp_hs_row->size()))
      {
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      }
      str->q_append(tmp_hs_row->begin(), tmp_hs_row->size());
    }
    tmp_hs_row++;
  }
  tmp_table->field[0]->set_notnull();
  tmp_table->field[0]->store(
    (const char *) hs_row,
    sizeof(SPIDER_HS_STRING_REF) * field_count, &my_charset_bin);
  tmp_table->field[1]->set_notnull();
  tmp_table->field[1]->store(
    str->ptr(), str->length(), &my_charset_bin);
  tmp_table->field[2]->set_null();
  DBUG_RETURN(tmp_table->file->ha_write_row(tmp_table->record[0]));
}

uint spider_db_handlersocket_row::get_byte_size()
{
  DBUG_ENTER("spider_db_handlersocket_row::get_byte_size");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(row_size);
}


spider_db_handlersocket_result_buffer::spider_db_handlersocket_result_buffer(
) : spider_db_result_buffer()
{
  DBUG_ENTER("spider_db_handlersocket_result_buffer::spider_db_handlersocket_result_buffer");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_VOID_RETURN;
}

spider_db_handlersocket_result_buffer::~spider_db_handlersocket_result_buffer()
{
  DBUG_ENTER(
    "spider_db_handlersocket_result_buffer::~spider_db_handlersocket_result_buffer");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_VOID_RETURN;
}

void spider_db_handlersocket_result_buffer::clear()
{
  DBUG_ENTER("spider_db_handlersocket_result_buffer::clear");
  DBUG_PRINT("info",("spider this=%p", this));
  hs_result.readbuf.clear();
  DBUG_VOID_RETURN;
}

bool spider_db_handlersocket_result_buffer::check_size(
  longlong size
) {
  DBUG_ENTER("spider_db_handlersocket_result_buffer::check_size");
  DBUG_PRINT("info",("spider this=%p", this));
  if ((uint) hs_result.readbuf.real_size() > size)
  {
    hs_result.readbuf.real_free();
    DBUG_RETURN(TRUE);
  }
  DBUG_RETURN(FALSE);
}

spider_db_handlersocket_result::spider_db_handlersocket_result(
  SPIDER_DB_CONN *in_db_conn
) : spider_db_result(in_db_conn), row(in_db_conn->dbton_id)
{
  DBUG_ENTER("spider_db_handlersocket_result::spider_db_handlersocket_result");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_VOID_RETURN;
}

spider_db_handlersocket_result::~spider_db_handlersocket_result()
{
  DBUG_ENTER(
    "spider_db_handlersocket_result::~spider_db_handlersocket_result");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_VOID_RETURN;
}

bool spider_db_handlersocket_result::has_result()
{
  DBUG_ENTER("spider_db_handlersocket_result::has_result");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(((*hs_conn_p)->get_response_end_offset() > 0));
}

void spider_db_handlersocket_result::free_result()
{
  DBUG_ENTER("spider_db_handlersocket_result::free_result");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_PRINT("info",("spider hs hs_conn=%p", hs_conn_p));
  DBUG_PRINT("info",("spider hs num_req_bufd=%zu",
    (*hs_conn_p)->get_num_req_bufd()));
  DBUG_PRINT("info",("spider hs num_req_sent=%zu",
    (*hs_conn_p)->get_num_req_sent()));
  DBUG_PRINT("info",("spider hs num_req_rcvd=%zu",
    (*hs_conn_p)->get_num_req_rcvd()));
  DBUG_PRINT("info",("spider hs response_end_offset=%zu",
    (*hs_conn_p)->get_response_end_offset()));
  if ((*hs_conn_p)->get_response_end_offset() > 0)
  {
    (*hs_conn_p)->response_buf_remove();
    if ((*hs_conn_p)->get_error_code())
    {
      DBUG_PRINT("info",("spider hs %d %s",
        (*hs_conn_p)->get_error_code(),
        (*hs_conn_p)->get_error().ptr()));
      (*hs_conn_p)->write_error_to_log(__func__, __FILE__, __LINE__);
    }
    DBUG_PRINT("info",("spider hs num_req_bufd=%zu",
      (*hs_conn_p)->get_num_req_bufd()));
    DBUG_PRINT("info",("spider hs num_req_sent=%zu",
      (*hs_conn_p)->get_num_req_sent()));
    DBUG_PRINT("info",("spider hs num_req_rcvd=%zu",
      (*hs_conn_p)->get_num_req_rcvd()));
    DBUG_PRINT("info",("spider hs response_end_offset=%zu",
      (*hs_conn_p)->get_response_end_offset()));
  }
  DBUG_VOID_RETURN;
}

SPIDER_DB_ROW *spider_db_handlersocket_result::current_row()
{
  DBUG_ENTER("spider_db_handlersocket_result::current_row");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN((SPIDER_DB_ROW *) row.clone());
}

SPIDER_DB_ROW *spider_db_handlersocket_result::fetch_row()
{
  DBUG_ENTER("spider_db_handlersocket_result::fetch_row");
  DBUG_PRINT("info",("spider this=%p", this));
  if (!(row.hs_row = (SPIDER_HS_STRING_REF *)
    (*hs_conn_p)->get_next_row()))
  {
    store_error_num = HA_ERR_END_OF_FILE;
    DBUG_RETURN(NULL);
  }
  row.field_count = field_count;
  row.hs_row_first = row.hs_row;
  row.row_size = (*hs_conn_p)->get_row_size();
  DBUG_RETURN((SPIDER_DB_ROW *) &row);
}

SPIDER_DB_ROW *spider_db_handlersocket_result::fetch_row_from_result_buffer(
  spider_db_result_buffer *spider_res_buf
) {
  spider_db_handlersocket_result_buffer *hs_res_buf;
  DBUG_ENTER("spider_db_handlersocket_result::fetch_row_from_result_buffer");
  DBUG_PRINT("info",("spider this=%p", this));
  hs_res_buf = (spider_db_handlersocket_result_buffer *) spider_res_buf;
  if (!(row.hs_row = (SPIDER_HS_STRING_REF *)
    (*hs_conn_p)->get_next_row_from_result(hs_res_buf->hs_result)))
  {
    store_error_num = HA_ERR_END_OF_FILE;
    DBUG_RETURN(NULL);
  }
  row.field_count = field_count;
  row.hs_row_first = row.hs_row;
  row.row_size = (*hs_conn_p)->get_row_size_from_result(hs_res_buf->hs_result);
  DBUG_RETURN((SPIDER_DB_ROW *) &row);
}

SPIDER_DB_ROW *spider_db_handlersocket_result::fetch_row_from_tmp_table(
  TABLE *tmp_table
) {
  uint i;
  spider_string tmp_str1, tmp_str2;
  const char *row_ptr;
  SPIDER_HS_STRING_REF *tmp_hs_row;
  uint field_count;
  DBUG_ENTER("spider_db_handlersocket_result::fetch_row_from_tmp_table");
  DBUG_PRINT("info",("spider this=%p", this));
  tmp_str1.init_calc_mem(171);
  tmp_str2.init_calc_mem(173);
  tmp_table->field[0]->val_str(tmp_str1.get_str());
  tmp_table->field[1]->val_str(tmp_str2.get_str());
  tmp_str1.mem_calc();
  tmp_str2.mem_calc();
  row_ptr = tmp_str2.ptr();
  tmp_hs_row = (SPIDER_HS_STRING_REF *) tmp_str1.ptr();
  field_count = tmp_str1.length() / sizeof(SPIDER_HS_STRING_REF);
  row.hs_row = tmp_hs_row;
  row.field_count = field_count;
  row.hs_row_first = row.hs_row;
  for (i = 0; i < field_count; i++)
  {
    if (tmp_hs_row->begin())
    {
      uint length = tmp_hs_row->size();
      tmp_hs_row->set(row_ptr, length);
      row_ptr += length;
    }
    tmp_hs_row++;
  }
  row.row_size = row_ptr - tmp_str2.ptr();
  DBUG_RETURN((SPIDER_DB_ROW *) &row);
}

int spider_db_handlersocket_result::fetch_table_status(
  int mode,
  ha_statistics &stat
) {
  DBUG_ENTER("spider_db_handlersocket_result::fetch_table_status");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_db_handlersocket_result::fetch_table_records(
  int mode,
  ha_rows &records
) {
  DBUG_ENTER("spider_db_handlersocket_result::fetch_table_records");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_db_handlersocket_result::fetch_table_cardinality(
  int mode,
  TABLE *table,
  longlong *cardinality,
  uchar *cardinality_upd,
  int bitmap_size
) {
  DBUG_ENTER("spider_db_handlersocket_result::fetch_table_cardinality");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_db_handlersocket_result::fetch_table_mon_status(
  int &status
) {
  DBUG_ENTER("spider_db_handlersocket_result::fetch_table_mon_status");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

longlong spider_db_handlersocket_result::num_rows()
{
  DBUG_ENTER("spider_db_handlersocket_result::num_rows");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN((longlong) 0);
}

uint spider_db_handlersocket_result::num_fields()
{
  DBUG_ENTER("spider_db_handlersocket_result::num_fields");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(field_count);
}

void spider_db_handlersocket_result::move_to_pos(
  longlong pos
) {
  DBUG_ENTER("spider_db_handlersocket_result::move_to_pos");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_VOID_RETURN;
}

int spider_db_handlersocket_result::get_errno()
{
  DBUG_ENTER("spider_db_handlersocket_result::get_errno");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_PRINT("info",("spider store_error_num=%d", store_error_num));
  DBUG_RETURN(store_error_num);
}

#ifdef SPIDER_HAS_DISCOVER_TABLE_STRUCTURE
int spider_db_handlersocket_result::fetch_columns_for_discover_table_structure(
  spider_string *str,
  CHARSET_INFO *access_charset
) {
  DBUG_ENTER("spider_db_handlersocket_result::fetch_columns_for_discover_table_structure");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(HA_ERR_WRONG_COMMAND);
}

int spider_db_handlersocket_result::fetch_index_for_discover_table_structure(
  spider_string *str,
  CHARSET_INFO *access_charset
) {
  DBUG_ENTER("spider_db_handlersocket_result::fetch_index_for_discover_table_structure");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(HA_ERR_WRONG_COMMAND);
}

int spider_db_handlersocket_result::fetch_table_for_discover_table_structure(
  spider_string *str,
  SPIDER_SHARE *spider_share,
  CHARSET_INFO *access_charset
) {
  DBUG_ENTER("spider_db_handlersocket_result::fetch_table_for_discover_table_structure");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(HA_ERR_WRONG_COMMAND);
}
#endif

spider_db_handlersocket::spider_db_handlersocket(
  SPIDER_CONN *conn
) : spider_db_conn(conn),
  handler_open_array_inited(FALSE),
  request_key_req_first(NULL),
  request_key_req_last(NULL),
  request_key_snd_first(NULL),
  request_key_snd_last(NULL),
  request_key_reuse_first(NULL),
  request_key_reuse_last(NULL)
{
  DBUG_ENTER("spider_db_handlersocket::spider_db_handlersocket");
  DBUG_PRINT("info",("spider this=%p", this));
#ifndef HANDLERSOCKET_MYSQL_UTIL
#else
  hs_conn = NULL;
#endif
  DBUG_VOID_RETURN;
}

spider_db_handlersocket::~spider_db_handlersocket()
{
  st_spider_db_request_key *tmp_request_key;
  DBUG_ENTER("spider_db_handlersocket::~spider_db_handlersocket");
  DBUG_PRINT("info",("spider this=%p", this));
  if (handler_open_array_inited)
  {
    reset_opened_handler();
    spider_free_mem_calc(spider_current_trx,
      handler_open_array_id,
      handler_open_array.max_element *
      handler_open_array.size_of_element);
    delete_dynamic(&handler_open_array);
  }
  while (request_key_req_first)
  {
    tmp_request_key = request_key_req_first->next;
    spider_free(spider_current_trx, request_key_req_first, MYF(0));
    request_key_req_first = tmp_request_key;
  }
  while (request_key_snd_first)
  {
    tmp_request_key = request_key_snd_first->next;
    spider_free(spider_current_trx, request_key_snd_first, MYF(0));
    request_key_snd_first = tmp_request_key;
  }
  while (request_key_reuse_first)
  {
    tmp_request_key = request_key_reuse_first->next;
    spider_free(spider_current_trx, request_key_reuse_first, MYF(0));
    request_key_reuse_first = tmp_request_key;
  }
  DBUG_VOID_RETURN;
}

int spider_db_handlersocket::init()
{
  DBUG_ENTER("spider_db_handlersocket::init");
  DBUG_PRINT("info",("spider this=%p", this));
  if (
    SPD_INIT_DYNAMIC_ARRAY2(&handler_open_array,
      sizeof(SPIDER_LINK_FOR_HASH *), NULL, 16, 16, MYF(MY_WME))
  ) {
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  spider_alloc_calc_mem_init(handler_open_array, 79);
  spider_alloc_calc_mem(spider_current_trx,
    handler_open_array,
    handler_open_array.max_element *
    handler_open_array.size_of_element);
  handler_open_array_inited = TRUE;
  DBUG_RETURN(0);
}

bool spider_db_handlersocket::is_connected()
{
  DBUG_ENTER("spider_db_handlersocket::is_connected");
  DBUG_PRINT("info",("spider this=%p", this));
#ifndef HANDLERSOCKET_MYSQL_UTIL
  DBUG_RETURN(hs_conn.operator->());
#else
  DBUG_RETURN(hs_conn);
#endif
}

void spider_db_handlersocket::bg_connect()
{
  DBUG_ENTER("spider_db_handlersocket::bg_connect");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_VOID_RETURN;
}

int spider_db_handlersocket::connect(
  char *tgt_host,
  char *tgt_username,
  char *tgt_password,
  long tgt_port,
  char *tgt_socket,
  char *server_name,
  int connect_retry_count,
  longlong connect_retry_interval
) {
  DBUG_ENTER("spider_db_handlersocket::connect");
  DBUG_PRINT("info",("spider this=%p", this));
  SPIDER_HS_SOCKARGS sockargs;
  sockargs.timeout = conn->connect_timeout;
  sockargs.recv_timeout = conn->net_read_timeout;
  sockargs.send_timeout = conn->net_write_timeout;
  if (conn->hs_sock)
  {
    sockargs.family = AF_UNIX;
    sockargs.set_unix_domain(conn->hs_sock);
  } else {
    char port_str[6];
    my_sprintf(port_str, (port_str, "%05ld", conn->hs_port));
    if (sockargs.resolve(conn->tgt_host, port_str) != 0)
    {
      my_error(ER_CONNECT_TO_FOREIGN_DATA_SOURCE, MYF(0),
        conn->tgt_host);
      DBUG_RETURN(ER_CONNECT_TO_FOREIGN_DATA_SOURCE);
    }
  }
#ifndef HANDLERSOCKET_MYSQL_UTIL
  if (!(hs_conn.operator->()))
#else
  if (!(hs_conn))
#endif
  {
    hs_conn = SPIDER_HS_CONN_CREATE(sockargs);
  } else {
    hs_conn->reconnect();
    spider_db_hs_request_buf_reset(conn);
  }
#ifndef HANDLERSOCKET_MYSQL_UTIL
  if (!(hs_conn.operator->()))
#else
  if (!(hs_conn))
#endif
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  while (hs_conn->get_error_code())
  {
    THD *thd = current_thd;
    if (
      !connect_retry_count ||
      (thd && thd->killed)
    ) {
      my_error(ER_CONNECT_TO_FOREIGN_DATA_SOURCE, MYF(0),
        conn->tgt_host);
      DBUG_RETURN(ER_CONNECT_TO_FOREIGN_DATA_SOURCE);
    }
    connect_retry_count--;
    my_sleep((ulong) connect_retry_interval);
    hs_conn->reconnect();
  }
  reset_request_key_req();
  reset_request_key_snd();
  DBUG_RETURN(0);
}

int spider_db_handlersocket::ping()
{
  SPIDER_HS_CONN *hs_conn_p = &hs_conn;
  DBUG_ENTER("spider_db_handlersocket::ping");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_PRINT("info",("spider hs num_req_bufd=%zu",
    (*hs_conn_p)->get_num_req_bufd()));
  DBUG_PRINT("info",("spider hs num_req_sent=%zu",
    (*hs_conn_p)->get_num_req_sent()));
  DBUG_PRINT("info",("spider hs num_req_rcvd=%zu",
    (*hs_conn_p)->get_num_req_rcvd()));
  DBUG_PRINT("info",("spider hs response_end_offset=%zu",
    (*hs_conn_p)->get_response_end_offset()));
  if ((*hs_conn_p)->reconnect())
  {
    DBUG_PRINT("info",("spider hs num_req_bufd=%zu",
      (*hs_conn_p)->get_num_req_bufd()));
    DBUG_PRINT("info",("spider hs num_req_sent=%zu",
      (*hs_conn_p)->get_num_req_sent()));
    DBUG_PRINT("info",("spider hs num_req_rcvd=%zu",
      (*hs_conn_p)->get_num_req_rcvd()));
    DBUG_PRINT("info",("spider hs response_end_offset=%zu",
      (*hs_conn_p)->get_response_end_offset()));
    DBUG_RETURN(ER_SPIDER_HS_NUM);
  }
  DBUG_PRINT("info",("spider hs num_req_bufd=%zu",
    (*hs_conn_p)->get_num_req_bufd()));
  DBUG_PRINT("info",("spider hs num_req_sent=%zu",
    (*hs_conn_p)->get_num_req_sent()));
  DBUG_PRINT("info",("spider hs num_req_rcvd=%zu",
    (*hs_conn_p)->get_num_req_rcvd()));
  DBUG_PRINT("info",("spider hs response_end_offset=%zu",
    (*hs_conn_p)->get_response_end_offset()));

  reset_request_key_req();
  reset_request_key_snd();
  conn->opened_handlers = 0;
  conn->db_conn->reset_opened_handler();
  ++conn->connection_id;
  DBUG_RETURN(0);
}

void spider_db_handlersocket::bg_disconnect()
{
  DBUG_ENTER("spider_db_handlersocket::bg_disconnect");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_VOID_RETURN;
}

void spider_db_handlersocket::disconnect()
{
  DBUG_ENTER("spider_db_handlersocket::disconnect");
  DBUG_PRINT("info",("spider this=%p", this));
#ifndef HANDLERSOCKET_MYSQL_UTIL
  if (hs_conn.operator->())
#else
  DBUG_PRINT("info",("spider hs_conn=%p", hs_conn));
  if (hs_conn)
#endif
  {
    hs_conn->close();
#ifndef HANDLERSOCKET_MYSQL_UTIL
    SPIDER_HS_CONN tmp_hs_conn;
    tmp_hs_conn = hs_conn;
#else
    delete hs_conn;
    hs_conn = NULL;
#endif
  }
  DBUG_VOID_RETURN;
}

int spider_db_handlersocket::set_net_timeout()
{
  DBUG_ENTER("spider_db_handlersocket::set_net_timeout");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(hs_conn->set_timeout(
    conn->net_write_timeout,
    conn->net_read_timeout
  ));
}

int spider_db_handlersocket::exec_query(
  const char *query,
  uint length,
  int quick_mode
) {
  DBUG_ENTER("spider_db_handlersocket::query");
  DBUG_PRINT("info",("spider this=%p", this));
  SPIDER_HS_CONN *hs_conn_p = &hs_conn;
#ifndef HANDLERSOCKET_MYSQL_UTIL
  DBUG_PRINT("info", ("spider hs_conn %p", hs_conn.operator->()));
#else
  DBUG_PRINT("info", ("spider hs_conn %p", hs_conn));
#endif
  DBUG_PRINT("info",("spider hs num_req_bufd=%zu",
    (*hs_conn_p)->get_num_req_bufd()));
  DBUG_PRINT("info",("spider hs num_req_sent=%zu",
    (*hs_conn_p)->get_num_req_sent()));
  DBUG_PRINT("info",("spider hs num_req_rcvd=%zu",
    (*hs_conn_p)->get_num_req_rcvd()));
  DBUG_PRINT("info",("spider hs response_end_offset=%zu",
    (*hs_conn_p)->get_response_end_offset()));
  if (spider_param_general_log())
  {
    const char *tgt_str = conn->hs_sock ? conn->hs_sock : conn->tgt_host;
    uint32 tgt_len = strlen(tgt_str);
    spider_string tmp_query_str((*hs_conn_p)->get_writebuf_size() +
      conn->tgt_wrapper_length +
      tgt_len + (SPIDER_SQL_SPACE_LEN * 2));
    tmp_query_str.init_calc_mem(231);
    tmp_query_str.length(0);
    tmp_query_str.q_append(conn->tgt_wrapper, conn->tgt_wrapper_length);
    tmp_query_str.q_append(SPIDER_SQL_SPACE_STR, SPIDER_SQL_SPACE_LEN);
    tmp_query_str.q_append(tgt_str, tgt_len);
    tmp_query_str.q_append(SPIDER_SQL_SPACE_STR, SPIDER_SQL_SPACE_LEN);
    tmp_query_str.q_append((*hs_conn_p)->get_writebuf_begin(),
      (*hs_conn_p)->get_writebuf_size());
    general_log_write(current_thd, COM_QUERY, tmp_query_str.ptr(),
      tmp_query_str.length());
  }
  if ((*hs_conn_p)->request_send() < 0)
  {
    DBUG_PRINT("info",("spider hs num_req_bufd=%zu",
      (*hs_conn_p)->get_num_req_bufd()));
    DBUG_PRINT("info",("spider hs num_req_sent=%zu",
      (*hs_conn_p)->get_num_req_sent()));
    DBUG_PRINT("info",("spider hs num_req_rcvd=%zu",
      (*hs_conn_p)->get_num_req_rcvd()));
    DBUG_PRINT("info",("spider hs response_end_offset=%zu",
      (*hs_conn_p)->get_response_end_offset()));
    DBUG_RETURN(ER_SPIDER_HS_NUM);
  }
  DBUG_PRINT("info",("spider hs num_req_bufd=%zu",
    (*hs_conn_p)->get_num_req_bufd()));
  DBUG_PRINT("info",("spider hs num_req_sent=%zu",
    (*hs_conn_p)->get_num_req_sent()));
  DBUG_PRINT("info",("spider hs num_req_rcvd=%zu",
    (*hs_conn_p)->get_num_req_rcvd()));
  DBUG_PRINT("info",("spider hs response_end_offset=%zu",
    (*hs_conn_p)->get_response_end_offset()));
  move_request_key_to_snd();
  DBUG_RETURN(0);
}

int spider_db_handlersocket::get_errno()
{
  DBUG_ENTER("spider_db_handlersocket::get_errno");
  DBUG_PRINT("info",("spider this=%p", this));
  stored_error = hs_conn->get_error_code();
  DBUG_PRINT("info",("spider stored_error=%d", stored_error));
  DBUG_RETURN(stored_error);
}

const char *spider_db_handlersocket::get_error()
{
  const char *error_ptr;
  DBUG_ENTER("spider_db_handlersocket::get_error");
  DBUG_PRINT("info",("spider this=%p", this));
#ifndef HANDLERSOCKET_MYSQL_UTIL
  error_ptr = hs_conn->get_error().c_str();
#else
  error_ptr = hs_conn->get_error().c_ptr();
#endif
  DBUG_PRINT("info",("spider error=%s", error_ptr));
  DBUG_RETURN(error_ptr);
}

bool spider_db_handlersocket::is_server_gone_error(
  int error_num
) {
  bool server_gone;
  DBUG_ENTER("spider_db_handlersocket::is_server_gone_error");
  DBUG_PRINT("info",("spider this=%p", this));
  server_gone = (hs_conn->get_error_code() < 0);
  DBUG_PRINT("info",("spider server_gone=%s", server_gone ? "TRUE" : "FALSE"));
  DBUG_RETURN(server_gone);
}

bool spider_db_handlersocket::is_dup_entry_error(
  int error_num
) {
  bool dup_entry;
  DBUG_ENTER("spider_db_handlersocket::is_dup_entry_error");
  DBUG_PRINT("info",("spider this=%p", this));
#ifndef HANDLERSOCKET_MYSQL_UTIL
  const char *c_str = hs_conn->get_error().c_str();
#else
  const char *c_str = hs_conn->get_error().c_ptr_safe();
#endif
  dup_entry =
    (
      c_str[0] == '1' &&
      c_str[1] == '2' &&
      c_str[2] == '1' &&
      c_str[3] == '\0'
    );
  DBUG_PRINT("info",("spider dup_entry=%s", dup_entry ? "TRUE" : "FALSE"));
  DBUG_RETURN(dup_entry);
}

bool spider_db_handlersocket::is_xa_nota_error(
  int error_num
) {
  bool xa_nota;
  DBUG_ENTER("spider_db_handlersocket::is_xa_nota_error");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  xa_nota = (stored_error == ER_XAER_NOTA);
  DBUG_PRINT("info",("spider xa_nota=%s", xa_nota ? "TRUE" : "FALSE"));
  DBUG_RETURN(xa_nota);
}

spider_db_result *spider_db_handlersocket::store_result(
  spider_db_result_buffer **spider_res_buf,
  st_spider_db_request_key *request_key,
  int *error_num
) {
  int internal_error;
  spider_db_handlersocket_result *result;
  spider_db_handlersocket_result_buffer *hs_res_buf;
  DBUG_ENTER("spider_db_handlersocket::store_result");
  DBUG_PRINT("info",("spider this=%p", this));
  if (*spider_res_buf)
  {
    hs_res_buf = (spider_db_handlersocket_result_buffer *) *spider_res_buf;
  } else {
    if (!(hs_res_buf = new spider_db_handlersocket_result_buffer()))
    {
      *error_num = HA_ERR_OUT_OF_MEM;
      DBUG_RETURN(NULL);
    }
    *spider_res_buf = (spider_db_result_buffer *) hs_res_buf;
  }
  hs_res_buf->clear();
  if (!(result = new spider_db_handlersocket_result(this)))
  {
    *error_num = HA_ERR_OUT_OF_MEM;
    DBUG_RETURN(NULL);
  }
  *error_num = 0;
  result->hs_conn_p = &hs_conn;
  size_t num_fields;
  SPIDER_HS_CONN *hs_conn_p = &hs_conn;
  DBUG_PRINT("info",("spider hs hs_conn=%p", hs_conn_p));
  if (request_key)
  {
    int tmp_res, tmp_err = (*hs_conn_p)->get_error_code();
    while ((tmp_res = check_request_key(request_key)) == 1)
    {
      DBUG_PRINT("info",("spider hs num_req_bufd=%zu",
        (*hs_conn_p)->get_num_req_bufd()));
      DBUG_PRINT("info",("spider hs num_req_sent=%zu",
        (*hs_conn_p)->get_num_req_sent()));
      DBUG_PRINT("info",("spider hs num_req_rcvd=%zu",
        (*hs_conn_p)->get_num_req_rcvd()));
      DBUG_PRINT("info",("spider hs response_end_offset=%zu",
        (*hs_conn_p)->get_response_end_offset()));
      if ((internal_error = (*hs_conn_p)->response_recv(num_fields)))
      {
        if (!tmp_err && internal_error > 0)
        {
          (*hs_conn_p)->clear_error();
        } else {
          (*hs_conn_p)->write_error_to_log(__func__, __FILE__, __LINE__);
#ifndef DBUG_OFF
          if ((*hs_conn_p)->get_response_end_offset() > 0 &&
            (*hs_conn_p)->get_readbuf_begin())
          {
            char tmp_buf[MAX_FIELD_WIDTH];
            String tmp_str(tmp_buf, MAX_FIELD_WIDTH, &my_charset_bin);
            tmp_str.length(0);
            tmp_str.append((*hs_conn_p)->get_readbuf_begin(),
              (*hs_conn_p)->get_response_end_offset(), &my_charset_bin);
            DBUG_PRINT("info",("spider hs readbuf01 size=%zu str=%s",
              (*hs_conn_p)->get_response_end_offset(), tmp_str.c_ptr_safe()));
          }
#endif
          DBUG_PRINT("info",("spider hs num_req_bufd=%zu",
            (*hs_conn_p)->get_num_req_bufd()));
          DBUG_PRINT("info",("spider hs num_req_sent=%zu",
            (*hs_conn_p)->get_num_req_sent()));
          DBUG_PRINT("info",("spider hs num_req_rcvd=%zu",
            (*hs_conn_p)->get_num_req_rcvd()));
          DBUG_PRINT("info",("spider hs response_end_offset=%zu",
            (*hs_conn_p)->get_response_end_offset()));
          if (internal_error > 0)
          {
            (*hs_conn_p)->response_buf_remove();
            if ((*hs_conn_p)->get_error_code())
            {
              DBUG_PRINT("info",("spider hs %d %s",
                (*hs_conn_p)->get_error_code(),
                (*hs_conn_p)->get_error().ptr()));
              (*hs_conn_p)->write_error_to_log(__func__, __FILE__, __LINE__);
            }
            DBUG_PRINT("info",("spider hs num_req_bufd=%zu",
              (*hs_conn_p)->get_num_req_bufd()));
            DBUG_PRINT("info",("spider hs num_req_sent=%zu",
              (*hs_conn_p)->get_num_req_sent()));
            DBUG_PRINT("info",("spider hs num_req_rcvd=%zu",
              (*hs_conn_p)->get_num_req_rcvd()));
            DBUG_PRINT("info",("spider hs response_end_offset=%zu",
              (*hs_conn_p)->get_response_end_offset()));
            (*hs_conn_p)->clear_error();
          }
          delete result;
          DBUG_RETURN(NULL);
        }
      }
      DBUG_PRINT("info",("spider hs num_req_bufd=%zu",
        (*hs_conn_p)->get_num_req_bufd()));
      DBUG_PRINT("info",("spider hs num_req_sent=%zu",
        (*hs_conn_p)->get_num_req_sent()));
      DBUG_PRINT("info",("spider hs num_req_rcvd=%zu",
        (*hs_conn_p)->get_num_req_rcvd()));
      DBUG_PRINT("info",("spider hs response_end_offset=%zu",
        (*hs_conn_p)->get_response_end_offset()));
      (*hs_conn_p)->response_buf_remove();
      DBUG_PRINT("info",("spider hs num_req_bufd=%zu",
        (*hs_conn_p)->get_num_req_bufd()));
      DBUG_PRINT("info",("spider hs num_req_sent=%zu",
        (*hs_conn_p)->get_num_req_sent()));
      DBUG_PRINT("info",("spider hs num_req_rcvd=%zu",
        (*hs_conn_p)->get_num_req_rcvd()));
      DBUG_PRINT("info",("spider hs response_end_offset=%zu",
        (*hs_conn_p)->get_response_end_offset()));
    }
    if (tmp_res == -1)
    {
      DBUG_PRINT("info",("spider ER_SPIDER_REQUEST_KEY_NUM"));
      *error_num = ER_SPIDER_REQUEST_KEY_NUM;
      DBUG_RETURN(NULL);
    }
  }
  DBUG_PRINT("info",("spider hs num_req_bufd=%zu",
    (*hs_conn_p)->get_num_req_bufd()));
  DBUG_PRINT("info",("spider hs num_req_sent=%zu",
    (*hs_conn_p)->get_num_req_sent()));
  DBUG_PRINT("info",("spider hs num_req_rcvd=%zu",
    (*hs_conn_p)->get_num_req_rcvd()));
  DBUG_PRINT("info",("spider hs response_end_offset=%zu",
    (*hs_conn_p)->get_response_end_offset()));
  if (
    (internal_error = (*hs_conn_p)->response_recv(num_fields)) ||
    (*error_num = (*hs_conn_p)->get_result(hs_res_buf->hs_result))
  ) {
    if (*error_num)
    {
      *error_num = HA_ERR_OUT_OF_MEM;
    }
    (*hs_conn_p)->write_error_to_log(__func__, __FILE__, __LINE__);
#ifndef DBUG_OFF
    if ((*hs_conn_p)->get_response_end_offset() > 0 &&
      (*hs_conn_p)->get_readbuf_begin())
    {
      char tmp_buf[MAX_FIELD_WIDTH];
      String tmp_str(tmp_buf, MAX_FIELD_WIDTH, &my_charset_bin);
      tmp_str.length(0);
      tmp_str.append((*hs_conn_p)->get_readbuf_begin(),
        (*hs_conn_p)->get_response_end_offset(), &my_charset_bin);
      DBUG_PRINT("info",("spider hs readbuf01 size=%zu str=%s",
        (*hs_conn_p)->get_response_end_offset(), tmp_str.c_ptr_safe()));
    }
#endif
    DBUG_PRINT("info",("spider hs num_req_bufd=%zu",
      (*hs_conn_p)->get_num_req_bufd()));
    DBUG_PRINT("info",("spider hs num_req_sent=%zu",
      (*hs_conn_p)->get_num_req_sent()));
    DBUG_PRINT("info",("spider hs num_req_rcvd=%zu",
      (*hs_conn_p)->get_num_req_rcvd()));
    DBUG_PRINT("info",("spider hs response_end_offset=%zu",
      (*hs_conn_p)->get_response_end_offset()));
    if (internal_error > 0)
    {
      (*hs_conn_p)->response_buf_remove();
      if ((*hs_conn_p)->get_error_code())
      {
        DBUG_PRINT("info",("spider hs %d %s",
          (*hs_conn_p)->get_error_code(),
          (*hs_conn_p)->get_error().ptr()));
        (*hs_conn_p)->write_error_to_log(__func__, __FILE__, __LINE__);
      }
      DBUG_PRINT("info",("spider hs num_req_bufd=%zu",
        (*hs_conn_p)->get_num_req_bufd()));
      DBUG_PRINT("info",("spider hs num_req_sent=%zu",
        (*hs_conn_p)->get_num_req_sent()));
      DBUG_PRINT("info",("spider hs num_req_rcvd=%zu",
        (*hs_conn_p)->get_num_req_rcvd()));
      DBUG_PRINT("info",("spider hs response_end_offset=%zu",
        (*hs_conn_p)->get_response_end_offset()));
    }
    delete result;
    DBUG_RETURN(NULL);
  }
#ifndef DBUG_OFF
  if ((*hs_conn_p)->get_response_end_offset() > 0 &&
    (*hs_conn_p)->get_readbuf_begin())
  {
    char tmp_buf[MAX_FIELD_WIDTH];
    String tmp_str(tmp_buf, MAX_FIELD_WIDTH, &my_charset_bin);
    tmp_str.length(0);
    tmp_str.append((*hs_conn_p)->get_readbuf_begin(),
      (*hs_conn_p)->get_response_end_offset(), &my_charset_bin);
    DBUG_PRINT("info",("spider hs readbuf02 size=%zu str=%s",
      (*hs_conn_p)->get_response_end_offset(), tmp_str.c_ptr_safe()));
  }
#endif
  DBUG_PRINT("info",("spider hs num_req_bufd=%zu",
    (*hs_conn_p)->get_num_req_bufd()));
  DBUG_PRINT("info",("spider hs num_req_sent=%zu",
    (*hs_conn_p)->get_num_req_sent()));
  DBUG_PRINT("info",("spider hs num_req_rcvd=%zu",
    (*hs_conn_p)->get_num_req_rcvd()));
  DBUG_PRINT("info",("spider hs response_end_offset=%zu",
    (*hs_conn_p)->get_response_end_offset()));
  (*hs_conn_p)->response_buf_remove();
  if ((*hs_conn_p)->get_error_code())
  {
    DBUG_PRINT("info",("spider hs %d %s",
      (*hs_conn_p)->get_error_code(),
      (*hs_conn_p)->get_error().ptr()));
    (*hs_conn_p)->write_error_to_log(__func__, __FILE__, __LINE__);
  }
  DBUG_PRINT("info",("spider hs num_req_bufd=%zu",
    (*hs_conn_p)->get_num_req_bufd()));
  DBUG_PRINT("info",("spider hs num_req_sent=%zu",
    (*hs_conn_p)->get_num_req_sent()));
  DBUG_PRINT("info",("spider hs num_req_rcvd=%zu",
    (*hs_conn_p)->get_num_req_rcvd()));
  DBUG_PRINT("info",("spider hs response_end_offset=%zu",
    (*hs_conn_p)->get_response_end_offset()));
  field_count = (uint) num_fields;
  result->field_count = field_count;
  DBUG_RETURN(result);
}

spider_db_result *spider_db_handlersocket::use_result(
  ha_spider *spider,
  st_spider_db_request_key *request_key,
  int *error_num
) {
  int internal_error;
  spider_db_handlersocket_result *result;
  DBUG_ENTER("spider_db_handlersocket::use_result");
  DBUG_PRINT("info",("spider this=%p", this));
  if (!(result = new spider_db_handlersocket_result(this)))
  {
    *error_num = HA_ERR_OUT_OF_MEM;
    DBUG_RETURN(NULL);
  }
  *error_num = 0;
  result->hs_conn_p = &hs_conn;
  size_t num_fields;
  SPIDER_HS_CONN *hs_conn_p = &hs_conn;
  DBUG_PRINT("info",("spider hs hs_conn=%p", hs_conn_p));
  if (request_key)
  {
    int tmp_res, tmp_err = (*hs_conn_p)->get_error_code();
    while ((tmp_res = check_request_key(request_key)) == 1)
    {
      DBUG_PRINT("info",("spider hs num_req_bufd=%zu",
        (*hs_conn_p)->get_num_req_bufd()));
      DBUG_PRINT("info",("spider hs num_req_sent=%zu",
        (*hs_conn_p)->get_num_req_sent()));
      DBUG_PRINT("info",("spider hs num_req_rcvd=%zu",
        (*hs_conn_p)->get_num_req_rcvd()));
      DBUG_PRINT("info",("spider hs response_end_offset=%zu",
        (*hs_conn_p)->get_response_end_offset()));
      if ((internal_error = (*hs_conn_p)->response_recv(num_fields)))
      {
        if (!tmp_err && internal_error > 0)
        {
          (*hs_conn_p)->clear_error();
        } else {
          (*hs_conn_p)->write_error_to_log(__func__, __FILE__, __LINE__);
#ifndef DBUG_OFF
          if ((*hs_conn_p)->get_response_end_offset() > 0 &&
            (*hs_conn_p)->get_readbuf_begin())
          {
            char tmp_buf[MAX_FIELD_WIDTH];
            String tmp_str(tmp_buf, MAX_FIELD_WIDTH, &my_charset_bin);
            tmp_str.length(0);
            tmp_str.append((*hs_conn_p)->get_readbuf_begin(),
              (*hs_conn_p)->get_response_end_offset(), &my_charset_bin);
            DBUG_PRINT("info",("spider hs readbuf01 size=%zu str=%s",
              (*hs_conn_p)->get_response_end_offset(), tmp_str.c_ptr_safe()));
          }
#endif
          DBUG_PRINT("info",("spider hs num_req_bufd=%zu",
            (*hs_conn_p)->get_num_req_bufd()));
          DBUG_PRINT("info",("spider hs num_req_sent=%zu",
            (*hs_conn_p)->get_num_req_sent()));
          DBUG_PRINT("info",("spider hs num_req_rcvd=%zu",
            (*hs_conn_p)->get_num_req_rcvd()));
          DBUG_PRINT("info",("spider hs response_end_offset=%zu",
            (*hs_conn_p)->get_response_end_offset()));
          if (internal_error > 0)
          {
            (*hs_conn_p)->response_buf_remove();
            if ((*hs_conn_p)->get_error_code())
            {
              DBUG_PRINT("info",("spider hs %d %s",
                (*hs_conn_p)->get_error_code(),
                (*hs_conn_p)->get_error().ptr()));
              (*hs_conn_p)->write_error_to_log(__func__, __FILE__, __LINE__);
            }
            DBUG_PRINT("info",("spider hs num_req_bufd=%zu",
              (*hs_conn_p)->get_num_req_bufd()));
            DBUG_PRINT("info",("spider hs num_req_sent=%zu",
              (*hs_conn_p)->get_num_req_sent()));
            DBUG_PRINT("info",("spider hs num_req_rcvd=%zu",
              (*hs_conn_p)->get_num_req_rcvd()));
            DBUG_PRINT("info",("spider hs response_end_offset=%zu",
              (*hs_conn_p)->get_response_end_offset()));
            (*hs_conn_p)->clear_error();
          }
          delete result;
          DBUG_RETURN(NULL);
        }
      }
      DBUG_PRINT("info",("spider hs num_req_bufd=%zu",
        (*hs_conn_p)->get_num_req_bufd()));
      DBUG_PRINT("info",("spider hs num_req_sent=%zu",
        (*hs_conn_p)->get_num_req_sent()));
      DBUG_PRINT("info",("spider hs num_req_rcvd=%zu",
        (*hs_conn_p)->get_num_req_rcvd()));
      DBUG_PRINT("info",("spider hs response_end_offset=%zu",
        (*hs_conn_p)->get_response_end_offset()));
      (*hs_conn_p)->response_buf_remove();
      DBUG_PRINT("info",("spider hs num_req_bufd=%zu",
        (*hs_conn_p)->get_num_req_bufd()));
      DBUG_PRINT("info",("spider hs num_req_sent=%zu",
        (*hs_conn_p)->get_num_req_sent()));
      DBUG_PRINT("info",("spider hs num_req_rcvd=%zu",
        (*hs_conn_p)->get_num_req_rcvd()));
      DBUG_PRINT("info",("spider hs response_end_offset=%zu",
        (*hs_conn_p)->get_response_end_offset()));
    }
    if (tmp_res == -1)
    {
      DBUG_PRINT("info",("spider ER_SPIDER_REQUEST_KEY_NUM"));
      *error_num = ER_SPIDER_REQUEST_KEY_NUM;
      DBUG_RETURN(NULL);
    }
  }
  DBUG_PRINT("info",("spider hs num_req_bufd=%zu",
    (*hs_conn_p)->get_num_req_bufd()));
  DBUG_PRINT("info",("spider hs num_req_sent=%zu",
    (*hs_conn_p)->get_num_req_sent()));
  DBUG_PRINT("info",("spider hs num_req_rcvd=%zu",
    (*hs_conn_p)->get_num_req_rcvd()));
  DBUG_PRINT("info",("spider hs response_end_offset=%zu",
    (*hs_conn_p)->get_response_end_offset()));
  if (
    (internal_error = (*hs_conn_p)->response_recv(num_fields))
  ) {
    (*hs_conn_p)->write_error_to_log(__func__, __FILE__, __LINE__);
#ifndef DBUG_OFF
    if ((*hs_conn_p)->get_response_end_offset() > 0 &&
      (*hs_conn_p)->get_readbuf_begin())
    {
      char tmp_buf[MAX_FIELD_WIDTH];
      String tmp_str(tmp_buf, MAX_FIELD_WIDTH, &my_charset_bin);
      tmp_str.length(0);
      tmp_str.append((*hs_conn_p)->get_readbuf_begin(),
        (*hs_conn_p)->get_response_end_offset(), &my_charset_bin);
      DBUG_PRINT("info",("spider hs readbuf01 size=%zu str=%s",
        (*hs_conn_p)->get_response_end_offset(), tmp_str.c_ptr_safe()));
    }
#endif
    DBUG_PRINT("info",("spider hs num_req_bufd=%zu",
      (*hs_conn_p)->get_num_req_bufd()));
    DBUG_PRINT("info",("spider hs num_req_sent=%zu",
      (*hs_conn_p)->get_num_req_sent()));
    DBUG_PRINT("info",("spider hs num_req_rcvd=%zu",
      (*hs_conn_p)->get_num_req_rcvd()));
    DBUG_PRINT("info",("spider hs response_end_offset=%zu",
      (*hs_conn_p)->get_response_end_offset()));
    if (internal_error > 0)
    {
      (*hs_conn_p)->response_buf_remove();
      if ((*hs_conn_p)->get_error_code())
      {
        DBUG_PRINT("info",("spider hs %d %s",
          (*hs_conn_p)->get_error_code(),
          (*hs_conn_p)->get_error().ptr()));
        (*hs_conn_p)->write_error_to_log(__func__, __FILE__, __LINE__);
      }
      DBUG_PRINT("info",("spider hs num_req_bufd=%zu",
        (*hs_conn_p)->get_num_req_bufd()));
      DBUG_PRINT("info",("spider hs num_req_sent=%zu",
        (*hs_conn_p)->get_num_req_sent()));
      DBUG_PRINT("info",("spider hs num_req_rcvd=%zu",
        (*hs_conn_p)->get_num_req_rcvd()));
      DBUG_PRINT("info",("spider hs response_end_offset=%zu",
        (*hs_conn_p)->get_response_end_offset()));
    }
    delete result;
    DBUG_RETURN(NULL);
  }
#ifndef DBUG_OFF
  if ((*hs_conn_p)->get_response_end_offset() > 0 &&
    (*hs_conn_p)->get_readbuf_begin())
  {
    char tmp_buf[MAX_FIELD_WIDTH];
    String tmp_str(tmp_buf, MAX_FIELD_WIDTH, &my_charset_bin);
    tmp_str.length(0);
    tmp_str.append((*hs_conn_p)->get_readbuf_begin(),
      (*hs_conn_p)->get_response_end_offset(), &my_charset_bin);
    DBUG_PRINT("info",("spider hs readbuf02 size=%zu str=%s",
      (*hs_conn_p)->get_response_end_offset(), tmp_str.c_ptr_safe()));
  }
#endif
  DBUG_PRINT("info",("spider hs num_req_bufd=%zu",
    (*hs_conn_p)->get_num_req_bufd()));
  DBUG_PRINT("info",("spider hs num_req_sent=%zu",
    (*hs_conn_p)->get_num_req_sent()));
  DBUG_PRINT("info",("spider hs num_req_rcvd=%zu",
    (*hs_conn_p)->get_num_req_rcvd()));
  DBUG_PRINT("info",("spider hs response_end_offset=%zu",
    (*hs_conn_p)->get_response_end_offset()));
  field_count = (uint) num_fields;
  result->field_count = field_count;
  DBUG_RETURN(result);
}

int spider_db_handlersocket::next_result()
{
  SPIDER_HS_CONN *hs_conn_p = &hs_conn;
  DBUG_ENTER("spider_db_handlersocket::next_result");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_PRINT("info",("spider hs hs_conn=%p", hs_conn_p));
  if ((*hs_conn_p)->stable_point())
    DBUG_RETURN(-1);
  DBUG_RETURN(0);
}

uint spider_db_handlersocket::affected_rows()
{
  int error_num;
  const SPIDER_HS_STRING_REF *hs_row;
  SPIDER_HS_CONN *hs_conn_p = &hs_conn;
  DBUG_ENTER("spider_db_handlersocket::affected_rows");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_PRINT("info",("spider hs hs_conn=%p", hs_conn_p));
  if (
    field_count != 1 ||
    !(hs_row = (*hs_conn_p)->get_next_row()) ||
    !hs_row->begin()
  ) {
    DBUG_RETURN(0);
  }
  DBUG_RETURN((uint) my_strtoll10(hs_row->begin(), (char**) NULL, &error_num));
}

uint spider_db_handlersocket::matched_rows()
{
  DBUG_ENTER("spider_db_handlersocket::matched_rows");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(0);
}

bool spider_db_handlersocket::inserted_info(
  spider_db_handler *handler,
  ha_copy_info *copy_info
) {
  DBUG_ENTER("spider_db_handlersocket::inserted_info");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(FALSE);
}

ulonglong spider_db_handlersocket::last_insert_id()
{
  DBUG_ENTER("spider_db_handlersocket::last_insert_id");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(0);
}

int spider_db_handlersocket::set_character_set(
  const char *csname
) {
  DBUG_ENTER("spider_db_handlersocket::set_character_set");
  DBUG_PRINT("info",("spider this=%p", this));
  /* nothing to do */
  DBUG_RETURN(0);
}

int spider_db_handlersocket::select_db(
  const char *dbname
) {
  DBUG_ENTER("spider_db_handlersocket::select_db");
  DBUG_PRINT("info",("spider this=%p", this));
  /* nothing to do */
  DBUG_RETURN(0);
}

int spider_db_handlersocket::consistent_snapshot(
  int *need_mon
) {
  DBUG_ENTER("spider_db_handlersocket::consistent_snapshot");
  DBUG_PRINT("info",("spider this=%p", this));
  /* nothing to do */
  DBUG_RETURN(0);
}

bool spider_db_handlersocket::trx_start_in_bulk_sql()
{
  DBUG_ENTER("spider_db_handlersocket::trx_start_in_bulk_sql");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(FALSE);
}

int spider_db_handlersocket::start_transaction(
  int *need_mon
) {
  DBUG_ENTER("spider_db_handlersocket::start_transaction");
  DBUG_PRINT("info",("spider this=%p", this));
  /* nothing to do */
  DBUG_RETURN(0);
}

int spider_db_handlersocket::commit(
  int *need_mon
) {
  DBUG_ENTER("spider_db_handlersocket::commit");
  DBUG_PRINT("info",("spider this=%p", this));
  /* nothing to do */
  DBUG_RETURN(0);
}

int spider_db_handlersocket::rollback(
  int *need_mon
) {
  DBUG_ENTER("spider_db_handlersocket::rollback");
  DBUG_PRINT("info",("spider this=%p", this));
  /* nothing to do */
  DBUG_RETURN(0);
}

bool spider_db_handlersocket::xa_start_in_bulk_sql()
{
  DBUG_ENTER("spider_db_handlersocket::xa_start_in_bulk_sql");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(FALSE);
}

int spider_db_handlersocket::xa_start(
  XID *xid,
  int *need_mon
) {
  DBUG_ENTER("spider_db_handlersocket::xa_start");
  DBUG_PRINT("info",("spider this=%p", this));
  /* nothing to do */
  DBUG_RETURN(0);
}

int spider_db_handlersocket::xa_end(
  XID *xid,
  int *need_mon
) {
  DBUG_ENTER("spider_db_handlersocket::xa_end");
  DBUG_PRINT("info",("spider this=%p", this));
  /* nothing to do */
  DBUG_RETURN(0);
}

int spider_db_handlersocket::xa_prepare(
  XID *xid,
  int *need_mon
) {
  DBUG_ENTER("spider_db_handlersocket::xa_prepare");
  DBUG_PRINT("info",("spider this=%p", this));
  /* nothing to do */
  DBUG_RETURN(0);
}

int spider_db_handlersocket::xa_commit(
  XID *xid,
  int *need_mon
) {
  DBUG_ENTER("spider_db_handlersocket::xa_commit");
  DBUG_PRINT("info",("spider this=%p", this));
  /* nothing to do */
  DBUG_RETURN(0);
}

int spider_db_handlersocket::xa_rollback(
  XID *xid,
  int *need_mon
) {
  DBUG_ENTER("spider_db_handlersocket::xa_rollback");
  DBUG_PRINT("info",("spider this=%p", this));
  /* nothing to do */
  DBUG_RETURN(0);
}

bool spider_db_handlersocket::set_trx_isolation_in_bulk_sql()
{
  DBUG_ENTER("spider_db_handlersocket::set_trx_isolation_in_bulk_sql");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(FALSE);
}

int spider_db_handlersocket::set_trx_isolation(
  int trx_isolation,
  int *need_mon
) {
  DBUG_ENTER("spider_db_handlersocket::set_trx_isolation");
  DBUG_PRINT("info",("spider this=%p", this));
  /* nothing to do */
  DBUG_RETURN(0);
}

bool spider_db_handlersocket::set_autocommit_in_bulk_sql()
{
  DBUG_ENTER("spider_db_handlersocket::set_autocommit_in_bulk_sql");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(FALSE);
}

int spider_db_handlersocket::set_autocommit(
  bool autocommit,
  int *need_mon
) {
  DBUG_ENTER("spider_db_handlersocket::set_autocommit");
  DBUG_PRINT("info",("spider this=%p", this));
  /* nothing to do */
  DBUG_RETURN(0);
}

bool spider_db_handlersocket::set_sql_log_off_in_bulk_sql()
{
  DBUG_ENTER("spider_db_handlersocket::set_sql_log_off_in_bulk_sql");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(FALSE);
}

int spider_db_handlersocket::set_sql_log_off(
  bool sql_log_off,
  int *need_mon
) {
  DBUG_ENTER("spider_db_handlersocket::set_sql_log_off");
  DBUG_PRINT("info",("spider this=%p", this));
  /* nothing to do */
  DBUG_RETURN(0);
}

bool spider_db_handlersocket::set_wait_timeout_in_bulk_sql()
{
  DBUG_ENTER("spider_db_handlersocket::set_wait_timeout_in_bulk_sql");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(FALSE);
}

int spider_db_handlersocket::set_wait_timeout(
  int wait_timeout,
  int *need_mon
) {
  DBUG_ENTER("spider_db_handlersocket::set_wait_timeout");
  DBUG_PRINT("info",("spider this=%p", this));
  /* nothing to do */
  DBUG_RETURN(0);
}

bool spider_db_handlersocket::set_sql_mode_in_bulk_sql()
{
  DBUG_ENTER("spider_db_handlersocket::set_sql_mode_in_bulk_sql");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(FALSE);
}

int spider_db_handlersocket::set_sql_mode(
  sql_mode_t sql_mode,
  int *need_mon
) {
  DBUG_ENTER("spider_db_handlersocket::set_sql_mode");
  DBUG_PRINT("info",("spider this=%p", this));
  /* nothing to do */
  DBUG_RETURN(0);
}

bool spider_db_handlersocket::set_time_zone_in_bulk_sql()
{
  DBUG_ENTER("spider_db_handlersocket::set_time_zone_in_bulk_sql");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(FALSE);
}

int spider_db_handlersocket::set_time_zone(
  Time_zone *time_zone,
  int *need_mon
) {
  DBUG_ENTER("spider_db_handlersocket::set_time_zone");
  DBUG_PRINT("info",("spider this=%p", this));
  /* nothing to do */
  DBUG_RETURN(0);
}

int spider_db_handlersocket::show_master_status(
  SPIDER_TRX *trx,
  SPIDER_SHARE *share,
  int all_link_idx,
  int *need_mon,
  TABLE *table,
  spider_string *str,
  int mode,
  SPIDER_DB_RESULT **res1,
  SPIDER_DB_RESULT **res2
) {
  DBUG_ENTER("spider_db_handlersocket::show_master_status");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(0);
}

int spider_db_handlersocket::append_sql(
  char *sql,
  ulong sql_length,
  st_spider_db_request_key *request_key
) {
  int error_num;
  size_t req_num;
  SPIDER_HS_CONN *hs_conn_p = &hs_conn;
  DBUG_ENTER("spider_db_handlersocket::append_sql");
  DBUG_PRINT("info",("spider this=%p", this));
  if ((error_num = append_request_key(request_key)))
    DBUG_RETURN(error_num);
  DBUG_PRINT("info",("spider hs hs_conn=%p", hs_conn_p));
  DBUG_PRINT("info",("spider hs num_req_bufd=%zu",
    (*hs_conn_p)->get_num_req_bufd()));
  DBUG_PRINT("info",("spider hs num_req_sent=%zu",
    (*hs_conn_p)->get_num_req_sent()));
  DBUG_PRINT("info",("spider hs num_req_rcvd=%zu",
    (*hs_conn_p)->get_num_req_rcvd()));
  DBUG_PRINT("info",("spider hs response_end_offset=%zu",
    (*hs_conn_p)->get_response_end_offset()));
  if (!(req_num = (*hs_conn_p)->request_buf_append(sql, sql + sql_length)))
  {
    DBUG_PRINT("info",("spider hs %d %s",
      (*hs_conn_p)->get_error_code(),
      (*hs_conn_p)->get_error().ptr()));
    (*hs_conn_p)->write_error_to_log(__func__, __FILE__, __LINE__);
    DBUG_RETURN((*hs_conn_p)->get_error_code());
  }
  DBUG_PRINT("info",("spider hs num_req_bufd=%zu",
    (*hs_conn_p)->get_num_req_bufd()));
  DBUG_PRINT("info",("spider hs num_req_sent=%zu",
    (*hs_conn_p)->get_num_req_sent()));
  DBUG_PRINT("info",("spider hs num_req_rcvd=%zu",
    (*hs_conn_p)->get_num_req_rcvd()));
  DBUG_PRINT("info",("spider hs response_end_offset=%zu",
    (*hs_conn_p)->get_response_end_offset()));
  while (req_num > 1)
  {
    if ((error_num = append_request_key(request_key)))
      DBUG_RETURN(error_num);
    --req_num;
  }
  DBUG_RETURN(0);
}

int spider_db_handlersocket::append_open_handler(
  uint handler_id,
  const char *db_name,
  const char *table_name,
  const char *index_name,
  const char *sql,
  st_spider_db_request_key *request_key
) {
  int error_num;
  SPIDER_HS_CONN *hs_conn_p = &hs_conn;
  DBUG_ENTER("spider_db_handlersocket::append_open_handler");
  DBUG_PRINT("info",("spider this=%p", this));
  if ((error_num = append_request_key(request_key)))
    DBUG_RETURN(error_num);
  DBUG_PRINT("info",("spider hs hs_conn=%p", hs_conn_p));
  DBUG_PRINT("info",("spider hs num_req_bufd=%zu",
    (*hs_conn_p)->get_num_req_bufd()));
  DBUG_PRINT("info",("spider hs num_req_sent=%zu",
    (*hs_conn_p)->get_num_req_sent()));
  DBUG_PRINT("info",("spider hs num_req_rcvd=%zu",
    (*hs_conn_p)->get_num_req_rcvd()));
  DBUG_PRINT("info",("spider hs response_end_offset=%zu",
    (*hs_conn_p)->get_response_end_offset()));
  (*hs_conn_p)->request_buf_open_index(
    handler_id,
    db_name,
    table_name,
    index_name,
    sql
  );
  DBUG_PRINT("info",("spider hs num_req_bufd=%zu",
    (*hs_conn_p)->get_num_req_bufd()));
  DBUG_PRINT("info",("spider hs num_req_sent=%zu",
    (*hs_conn_p)->get_num_req_sent()));
  DBUG_PRINT("info",("spider hs num_req_rcvd=%zu",
    (*hs_conn_p)->get_num_req_rcvd()));
  DBUG_PRINT("info",("spider hs response_end_offset=%zu",
    (*hs_conn_p)->get_response_end_offset()));
  DBUG_RETURN(0);
}

int spider_db_handlersocket::append_select(
  uint handler_id,
  spider_string *sql,
  SPIDER_DB_HS_STRING_REF_BUFFER *keys,
  int limit,
  int skip,
  st_spider_db_request_key *request_key
) {
  int error_num;
  SPIDER_HS_CONN *hs_conn_p = &hs_conn;
  DBUG_ENTER("spider_db_handlersocket::append_select");
  DBUG_PRINT("info",("spider this=%p", this));
  if ((error_num = append_request_key(request_key)))
    DBUG_RETURN(error_num);
  DBUG_PRINT("info",("spider hs hs_conn=%p", hs_conn_p));
  DBUG_PRINT("info",("spider hs num_req_bufd=%zu",
    (*hs_conn_p)->get_num_req_bufd()));
  DBUG_PRINT("info",("spider hs num_req_sent=%zu",
    (*hs_conn_p)->get_num_req_sent()));
  DBUG_PRINT("info",("spider hs num_req_rcvd=%zu",
    (*hs_conn_p)->get_num_req_rcvd()));
  DBUG_PRINT("info",("spider hs response_end_offset=%zu",
    (*hs_conn_p)->get_response_end_offset()));
  (*hs_conn_p)->request_buf_exec_generic(
    handler_id,
    SPIDER_HS_STRING_REF(sql->ptr(), sql->length()),
    keys->ptr(), (size_t) keys->size(),
    limit, skip,
    SPIDER_HS_STRING_REF(),
    NULL, 0);
  DBUG_PRINT("info",("spider hs num_req_bufd=%zu",
    (*hs_conn_p)->get_num_req_bufd()));
  DBUG_PRINT("info",("spider hs num_req_sent=%zu",
    (*hs_conn_p)->get_num_req_sent()));
  DBUG_PRINT("info",("spider hs num_req_rcvd=%zu",
    (*hs_conn_p)->get_num_req_rcvd()));
  DBUG_PRINT("info",("spider hs response_end_offset=%zu",
    (*hs_conn_p)->get_response_end_offset()));
  DBUG_RETURN(0);
}

int spider_db_handlersocket::append_insert(
  uint handler_id,
  SPIDER_DB_HS_STRING_REF_BUFFER *upds,
  st_spider_db_request_key *request_key
) {
  int error_num;
  SPIDER_HS_CONN *hs_conn_p = &hs_conn;
  DBUG_ENTER("spider_db_handlersocket::append_insert");
  DBUG_PRINT("info",("spider this=%p", this));
  if ((error_num = append_request_key(request_key)))
    DBUG_RETURN(error_num);
  DBUG_PRINT("info",("spider hs hs_conn=%p", hs_conn_p));
  DBUG_PRINT("info",("spider hs num_req_bufd=%zu",
    (*hs_conn_p)->get_num_req_bufd()));
  DBUG_PRINT("info",("spider hs num_req_sent=%zu",
    (*hs_conn_p)->get_num_req_sent()));
  DBUG_PRINT("info",("spider hs num_req_rcvd=%zu",
    (*hs_conn_p)->get_num_req_rcvd()));
  DBUG_PRINT("info",("spider hs response_end_offset=%zu",
    (*hs_conn_p)->get_response_end_offset()));
  (*hs_conn_p)->request_buf_exec_generic(
    handler_id,
    SPIDER_HS_STRING_REF(SPIDER_SQL_HS_INSERT_STR, SPIDER_SQL_HS_INSERT_LEN),
    upds->ptr(), (size_t) upds->size(),
    0, 0,
    SPIDER_HS_STRING_REF(), NULL, 0);
  DBUG_PRINT("info",("spider hs num_req_bufd=%zu",
    (*hs_conn_p)->get_num_req_bufd()));
  DBUG_PRINT("info",("spider hs num_req_sent=%zu",
    (*hs_conn_p)->get_num_req_sent()));
  DBUG_PRINT("info",("spider hs num_req_rcvd=%zu",
    (*hs_conn_p)->get_num_req_rcvd()));
  DBUG_PRINT("info",("spider hs response_end_offset=%zu",
    (*hs_conn_p)->get_response_end_offset()));
  DBUG_RETURN(0);
}

int spider_db_handlersocket::append_update(
  uint handler_id,
  spider_string *sql,
  SPIDER_DB_HS_STRING_REF_BUFFER *keys,
  SPIDER_DB_HS_STRING_REF_BUFFER *upds,
  int limit,
  int skip,
  bool increment,
  bool decrement,
  st_spider_db_request_key *request_key
) {
  int error_num;
  SPIDER_HS_CONN *hs_conn_p = &hs_conn;
  DBUG_ENTER("spider_db_handlersocket::append_update");
  DBUG_PRINT("info",("spider this=%p", this));
  if ((error_num = append_request_key(request_key)))
    DBUG_RETURN(error_num);
  DBUG_PRINT("info",("spider hs hs_conn=%p", hs_conn_p));
  DBUG_PRINT("info",("spider hs num_req_bufd=%zu",
    (*hs_conn_p)->get_num_req_bufd()));
  DBUG_PRINT("info",("spider hs num_req_sent=%zu",
    (*hs_conn_p)->get_num_req_sent()));
  DBUG_PRINT("info",("spider hs num_req_rcvd=%zu",
    (*hs_conn_p)->get_num_req_rcvd()));
  DBUG_PRINT("info",("spider hs response_end_offset=%zu",
    (*hs_conn_p)->get_response_end_offset()));
  (*hs_conn_p)->request_buf_exec_generic(
    handler_id,
    SPIDER_HS_STRING_REF(sql->ptr(), sql->length()),
    keys->ptr(), (size_t) keys->size(),
    limit, skip,
    increment ?
      SPIDER_HS_STRING_REF(SPIDER_SQL_HS_INCREMENT_STR,
        SPIDER_SQL_HS_INCREMENT_LEN) :
      decrement ?
        SPIDER_HS_STRING_REF(SPIDER_SQL_HS_DECREMENT_STR,
          SPIDER_SQL_HS_DECREMENT_LEN) :
        SPIDER_HS_STRING_REF(SPIDER_SQL_HS_UPDATE_STR,
          SPIDER_SQL_HS_UPDATE_LEN),
    upds->ptr(), (size_t) upds->size()
  );
  DBUG_PRINT("info",("spider hs num_req_bufd=%zu",
    (*hs_conn_p)->get_num_req_bufd()));
  DBUG_PRINT("info",("spider hs num_req_sent=%zu",
    (*hs_conn_p)->get_num_req_sent()));
  DBUG_PRINT("info",("spider hs num_req_rcvd=%zu",
    (*hs_conn_p)->get_num_req_rcvd()));
  DBUG_PRINT("info",("spider hs response_end_offset=%zu",
    (*hs_conn_p)->get_response_end_offset()));
  DBUG_RETURN(0);
}

int spider_db_handlersocket::append_delete(
  uint handler_id,
  spider_string *sql,
  SPIDER_DB_HS_STRING_REF_BUFFER *keys,
  int limit,
  int skip,
  st_spider_db_request_key *request_key
) {
  int error_num;
  SPIDER_HS_CONN *hs_conn_p = &hs_conn;
  DBUG_ENTER("spider_db_handlersocket::append_delete");
  DBUG_PRINT("info",("spider this=%p", this));
  if ((error_num = append_request_key(request_key)))
    DBUG_RETURN(error_num);
  DBUG_PRINT("info",("spider hs hs_conn=%p", hs_conn_p));
  DBUG_PRINT("info",("spider hs num_req_bufd=%zu",
    (*hs_conn_p)->get_num_req_bufd()));
  DBUG_PRINT("info",("spider hs num_req_sent=%zu",
    (*hs_conn_p)->get_num_req_sent()));
  DBUG_PRINT("info",("spider hs num_req_rcvd=%zu",
    (*hs_conn_p)->get_num_req_rcvd()));
  DBUG_PRINT("info",("spider hs response_end_offset=%zu",
    (*hs_conn_p)->get_response_end_offset()));
  (*hs_conn_p)->request_buf_exec_generic(
    handler_id,
    SPIDER_HS_STRING_REF(sql->ptr(), sql->length()),
    keys->ptr(), (size_t) keys->size(),
    limit, skip,
    SPIDER_HS_STRING_REF(SPIDER_SQL_HS_DELETE_STR, SPIDER_SQL_HS_DELETE_LEN),
    NULL, 0);
  DBUG_PRINT("info",("spider hs num_req_bufd=%zu",
    (*hs_conn_p)->get_num_req_bufd()));
  DBUG_PRINT("info",("spider hs num_req_sent=%zu",
    (*hs_conn_p)->get_num_req_sent()));
  DBUG_PRINT("info",("spider hs num_req_rcvd=%zu",
    (*hs_conn_p)->get_num_req_rcvd()));
  DBUG_PRINT("info",("spider hs response_end_offset=%zu",
    (*hs_conn_p)->get_response_end_offset()));
  DBUG_RETURN(0);
}

void spider_db_handlersocket::reset_request_queue()
{
  SPIDER_HS_CONN *hs_conn_p = &hs_conn;
  DBUG_ENTER("spider_db_handlersocket::reset_request_queue");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_PRINT("info",("spider hs hs_conn=%p", hs_conn_p));
  DBUG_PRINT("info",("spider hs num_req_bufd=%zu",
    (*hs_conn_p)->get_num_req_bufd()));
  DBUG_PRINT("info",("spider hs num_req_sent=%zu",
    (*hs_conn_p)->get_num_req_sent()));
  DBUG_PRINT("info",("spider hs num_req_rcvd=%zu",
    (*hs_conn_p)->get_num_req_rcvd()));
  DBUG_PRINT("info",("spider hs response_end_offset=%zu",
    (*hs_conn_p)->get_response_end_offset()));
  (*hs_conn_p)->request_reset();
  DBUG_PRINT("info",("spider hs num_req_bufd=%zu",
    (*hs_conn_p)->get_num_req_bufd()));
  DBUG_PRINT("info",("spider hs num_req_sent=%zu",
    (*hs_conn_p)->get_num_req_sent()));
  DBUG_PRINT("info",("spider hs num_req_rcvd=%zu",
    (*hs_conn_p)->get_num_req_rcvd()));
  DBUG_PRINT("info",("spider hs response_end_offset=%zu",
    (*hs_conn_p)->get_response_end_offset()));
  reset_request_key_req();
  DBUG_VOID_RETURN;
}

size_t spider_db_handlersocket::escape_string(
  char *to,
  const char *from,
  size_t from_length
) {
  DBUG_ENTER("spider_db_handlersocket::escape_string");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  memcpy(to, from, from_length);
  DBUG_RETURN(from_length);
}

bool spider_db_handlersocket::have_lock_table_list()
{
  DBUG_ENTER("spider_db_handlersocket::have_lock_table_list");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(FALSE);
}

int spider_db_handlersocket::append_lock_tables(
  spider_string *str
) {
  DBUG_ENTER("spider_db_handlersocket::lock_tables");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(0);
}

int spider_db_handlersocket::append_unlock_tables(
  spider_string *str
) {
  DBUG_ENTER("spider_db_handlersocket::append_unlock_tables");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(0);
}

uint spider_db_handlersocket::get_lock_table_hash_count()
{
  DBUG_ENTER("spider_db_handlersocket::get_lock_table_hash_count");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

void spider_db_handlersocket::reset_lock_table_hash()
{
  DBUG_ENTER("spider_db_handlersocket::reset_lock_table_hash");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_VOID_RETURN;
}

uint spider_db_handlersocket::get_opened_handler_count()
{
  DBUG_ENTER("spider_db_handlersocket::get_opened_handler_count");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(handler_open_array.elements);
}

void spider_db_handlersocket::reset_opened_handler()
{
  ha_spider *tmp_spider;
  int tmp_link_idx;
  SPIDER_LINK_FOR_HASH **tmp_link_for_hash;
  DBUG_ENTER("spider_db_handlersocket::reset_opened_handler");
  DBUG_PRINT("info",("spider this=%p", this));
  while ((tmp_link_for_hash =
    (SPIDER_LINK_FOR_HASH **) pop_dynamic(&handler_open_array)))
  {
    tmp_spider = (*tmp_link_for_hash)->spider;
    tmp_link_idx = (*tmp_link_for_hash)->link_idx;
    tmp_spider->clear_handler_opened(tmp_link_idx, conn->conn_kind);
  }
  DBUG_VOID_RETURN;
}

void spider_db_handlersocket::set_dup_key_idx(
  ha_spider *spider,
  int link_idx
) {
  DBUG_ENTER("spider_db_handlersocket::set_dup_key_idx");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_VOID_RETURN;
}

int spider_db_handlersocket::append_request_key(
  st_spider_db_request_key *request_key
) {
  st_spider_db_request_key *tmp_request_key;
  DBUG_ENTER("spider_db_handlersocket::append_request_key");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_PRINT("info",("spider request_key=%p", request_key));
  if (request_key)
  {
    DBUG_PRINT("info",("spider request_key->spider_thread_id=%llu",
      request_key->spider_thread_id));
    DBUG_PRINT("info",("spider request_key->query_id=%llu",
      request_key->query_id));
    DBUG_PRINT("info",("spider request_key->handler=%p",
      request_key->handler));
    DBUG_PRINT("info",("spider request_key->request_id=%llu",
      request_key->request_id));
    if (request_key_reuse_first)
    {
      tmp_request_key = request_key_reuse_first;
      request_key_reuse_first = request_key_reuse_first->next;
      if (!request_key_reuse_first)
        request_key_reuse_last = NULL;
    } else {
      if (!(tmp_request_key = (st_spider_db_request_key *)
        spider_malloc(spider_current_trx, 1, sizeof(st_spider_db_request_key),
          MYF(MY_WME)))
      )
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    }
    *tmp_request_key = *request_key;
    tmp_request_key->next = NULL;
    if (request_key_req_last)
      request_key_req_last->next = tmp_request_key;
    else
      request_key_req_first = tmp_request_key;
    request_key_req_last = tmp_request_key;
  }
  DBUG_RETURN(0);
}

void spider_db_handlersocket::reset_request_key_req()
{
  DBUG_ENTER("spider_db_handlersocket::reset_request_key_req");
  DBUG_PRINT("info",("spider this=%p", this));
  if (request_key_req_first)
  {
    if (request_key_reuse_last)
      request_key_reuse_last->next = request_key_req_first;
    else
      request_key_reuse_first = request_key_req_first;
    request_key_reuse_last = request_key_req_last;
    request_key_req_first = NULL;
    request_key_req_last = NULL;
  }
  DBUG_VOID_RETURN;
}

void spider_db_handlersocket::reset_request_key_snd()
{
  DBUG_ENTER("spider_db_handlersocket::reset_request_key_snd");
  DBUG_PRINT("info",("spider this=%p", this));
  if (request_key_snd_first)
  {
    if (request_key_reuse_last)
      request_key_reuse_last->next = request_key_snd_first;
    else
      request_key_reuse_first = request_key_snd_first;
    request_key_reuse_last = request_key_snd_last;
    request_key_snd_first = NULL;
    request_key_snd_last = NULL;
  }
  DBUG_VOID_RETURN;
}

void spider_db_handlersocket::move_request_key_to_snd()
{
  DBUG_ENTER("spider_db_handlersocket::move_request_key_to_snd");
  DBUG_PRINT("info",("spider this=%p", this));
  if (request_key_req_first)
  {
    if (request_key_snd_last)
      request_key_snd_last->next = request_key_req_first;
    else
      request_key_snd_first = request_key_req_first;
    request_key_snd_last = request_key_req_last;
    request_key_req_first = NULL;
    request_key_req_last = NULL;
  }
  DBUG_VOID_RETURN;
}

int spider_db_handlersocket::check_request_key(
  st_spider_db_request_key *request_key
) {
  st_spider_db_request_key *tmp_request_key;
  DBUG_ENTER("spider_db_handlersocket::check_request_key");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_PRINT("info",("spider request_key=%p", request_key));
  DBUG_PRINT("info",("spider request_key_snd_first=%p",
    request_key_snd_first));
  if (!request_key_snd_first)
  {
    DBUG_PRINT("info",("spider -1"));
    DBUG_RETURN(-1);
  }
  tmp_request_key = request_key_snd_first;
  request_key_snd_first = request_key_snd_first->next;
  if (!request_key_snd_first)
    request_key_snd_last = NULL;
  tmp_request_key->next = NULL;
  if (request_key_reuse_last)
    request_key_reuse_last->next = tmp_request_key;
  else
    request_key_reuse_first = tmp_request_key;
  request_key_reuse_last = tmp_request_key;

  DBUG_PRINT("info",("spider tmp_request_key->spider_thread_id=%llu",
    tmp_request_key->spider_thread_id));
  DBUG_PRINT("info",("spider request_key->spider_thread_id=%llu",
    request_key->spider_thread_id));
  DBUG_PRINT("info",("spider tmp_request_key->query_id=%llu",
    tmp_request_key->query_id));
  DBUG_PRINT("info",("spider request_key->query_id=%llu",
    request_key->query_id));
  DBUG_PRINT("info",("spider tmp_request_key->handler=%p",
    tmp_request_key->handler));
  DBUG_PRINT("info",("spider request_key->handler=%p",
    request_key->handler));
  DBUG_PRINT("info",("spider tmp_request_key->request_id=%llu",
    tmp_request_key->request_id));
  DBUG_PRINT("info",("spider request_key->request_id=%llu",
    request_key->request_id));
  if (
    tmp_request_key->spider_thread_id != request_key->spider_thread_id ||
    tmp_request_key->query_id != request_key->query_id ||
    tmp_request_key->handler != request_key->handler ||
    tmp_request_key->request_id != request_key->request_id
  ) {
    DBUG_PRINT("info",("spider 1"));
    DBUG_RETURN(1);
  }
  DBUG_PRINT("info",("spider 0"));
  DBUG_RETURN(0);
}

bool spider_db_handlersocket::cmp_request_key_to_snd(
  st_spider_db_request_key *request_key
) {
  DBUG_ENTER("spider_db_handlersocket::cmp_request_key_to_snd");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_PRINT("info",("spider request_key=%p", request_key));
  if (
    !request_key
  ) {
    DBUG_PRINT("info",("spider TRUE"));
    DBUG_RETURN(TRUE);
  }
  DBUG_PRINT("info",("spider request_key_snd_first=%p",
    request_key_snd_first));
  if (
    !request_key_snd_first
  ) {
    DBUG_PRINT("info",("spider FALSE"));
    DBUG_RETURN(FALSE);
  }
  DBUG_PRINT("info",("spider request_key_snd_first->spider_thread_id=%llu",
    request_key_snd_first->spider_thread_id));
  DBUG_PRINT("info",("spider request_key->spider_thread_id=%llu",
    request_key->spider_thread_id));
  DBUG_PRINT("info",("spider request_key_snd_first->query_id=%llu",
    request_key_snd_first->query_id));
  DBUG_PRINT("info",("spider request_key->query_id=%llu",
    request_key->query_id));
  DBUG_PRINT("info",("spider request_key_snd_first->handler=%p",
    request_key_snd_first->handler));
  DBUG_PRINT("info",("spider request_key->handler=%p",
    request_key->handler));
  DBUG_PRINT("info",("spider request_key_snd_first->request_id=%llu",
    request_key_snd_first->request_id));
  DBUG_PRINT("info",("spider request_key->request_id=%llu",
    request_key->request_id));
  if (
    request_key_snd_first->spider_thread_id != request_key->spider_thread_id ||
    request_key_snd_first->query_id != request_key->query_id ||
    request_key_snd_first->handler != request_key->handler ||
    request_key_snd_first->request_id != request_key->request_id
  ) {
    DBUG_PRINT("info",("spider FALSE"));
    DBUG_RETURN(FALSE);
  }
  DBUG_PRINT("info",("spider TRUE"));
  DBUG_RETURN(TRUE);
}

spider_db_handlersocket_util::spider_db_handlersocket_util() : spider_db_util()
{
  DBUG_ENTER("spider_db_handlersocket_util::spider_db_handlersocket_util");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_VOID_RETURN;
}

spider_db_handlersocket_util::~spider_db_handlersocket_util()
{
  DBUG_ENTER("spider_db_handlersocket_util::~spider_db_handlersocket_util");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_VOID_RETURN;
}

int spider_db_handlersocket_util::append_name(
  spider_string *str,
  const char *name,
  uint name_length
) {
  DBUG_ENTER("spider_db_handlersocket_util::append_name");
  str->q_append(name, name_length);
  DBUG_RETURN(0);
}

int spider_db_handlersocket_util::append_name_with_charset(
  spider_string *str,
  const char *name,
  uint name_length,
  CHARSET_INFO *name_charset
) {
  DBUG_ENTER("spider_db_handlersocket_util::append_name_with_charset");
  if (str->reserve(SPIDER_SQL_NAME_QUOTE_LEN * 2 + name_length * 2))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  str->q_append(SPIDER_SQL_NAME_QUOTE_STR, SPIDER_SQL_NAME_QUOTE_LEN);
  str->append(name, name_length, name_charset);
  if (str->reserve(SPIDER_SQL_NAME_QUOTE_LEN))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  str->q_append(SPIDER_SQL_NAME_QUOTE_STR, SPIDER_SQL_NAME_QUOTE_LEN);
  DBUG_RETURN(0);
}

int spider_db_handlersocket_util::append_escaped_name(
  spider_string *str,
  const char *name,
  uint name_length
) {
  int error_num;
  DBUG_ENTER("spider_db_handlersocket_util::append_name");
  if (str->reserve(SPIDER_SQL_NAME_QUOTE_LEN * 2 + name_length * 2))
  {
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  str->q_append(SPIDER_SQL_NAME_QUOTE_STR, SPIDER_SQL_NAME_QUOTE_LEN);
  if ((error_num = spider_db_append_name_with_quote_str_internal(
    str, name, name_length, dbton_id)))
  {
    DBUG_RETURN(error_num);
  }
  if (str->reserve(SPIDER_SQL_NAME_QUOTE_LEN))
  {
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  str->q_append(SPIDER_SQL_NAME_QUOTE_STR, SPIDER_SQL_NAME_QUOTE_LEN);
  DBUG_RETURN(0);
}

int spider_db_handlersocket_util::append_escaped_name_with_charset(
  spider_string *str,
  const char *name,
  uint name_length,
  CHARSET_INFO *name_charset
) {
  int error_num;
  DBUG_ENTER("spider_db_handlersocket_util::append_name_with_charset");
  if (str->reserve(SPIDER_SQL_NAME_QUOTE_LEN * 2 + name_length * 2))
  {
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  str->q_append(SPIDER_SQL_NAME_QUOTE_STR, SPIDER_SQL_NAME_QUOTE_LEN);
  if ((error_num = spider_db_append_name_with_quote_str_internal(
    str, name, name_length, name_charset, dbton_id)))
  {
    DBUG_RETURN(error_num);
  }
  if (str->reserve(SPIDER_SQL_NAME_QUOTE_LEN))
  {
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  str->q_append(SPIDER_SQL_NAME_QUOTE_STR, SPIDER_SQL_NAME_QUOTE_LEN);
  DBUG_RETURN(0);
}

bool spider_db_handlersocket_util::is_name_quote(
  const char head_code
) {
  DBUG_ENTER("spider_db_handlersocket_util::is_name_quote");
  DBUG_RETURN(head_code == *name_quote_str);
}

int spider_db_handlersocket_util::append_escaped_name_quote(
  spider_string *str
) {
  DBUG_ENTER("spider_db_handlersocket_util::append_escaped_name_quote");
  if (str->reserve(SPIDER_SQL_NAME_QUOTE_LEN * 2))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  str->q_append(SPIDER_SQL_NAME_QUOTE_STR, SPIDER_SQL_NAME_QUOTE_LEN);
  str->q_append(SPIDER_SQL_NAME_QUOTE_STR, SPIDER_SQL_NAME_QUOTE_LEN);
  DBUG_RETURN(0);
}

int spider_db_handlersocket_util::append_column_value(
  ha_spider *spider,
  spider_string *str,
  Field *field,
  const uchar *new_ptr,
  CHARSET_INFO *access_charset
) {
  char buf[MAX_FIELD_WIDTH];
  spider_string tmp_str(buf, MAX_FIELD_WIDTH, &my_charset_bin);
  String *ptr;
  uint length;
  DBUG_ENTER("spider_db_handlersocket_util::append_column_value");
  tmp_str.init_calc_mem(180);

  if (new_ptr)
  {
    if (
      field->type() == MYSQL_TYPE_BLOB ||
      field->real_type() == MYSQL_TYPE_VARCHAR
    ) {
      length = uint2korr(new_ptr);
      tmp_str.set_quick((char *) new_ptr + HA_KEY_BLOB_LENGTH, length,
        &my_charset_bin);
      ptr = tmp_str.get_str();
    } else if (field->type() == MYSQL_TYPE_GEOMETRY)
    {
/*
      uint mlength = SIZEOF_STORED_DOUBLE, lcnt;
      uchar *dest = (uchar *) buf;
      const uchar *source;
      for (lcnt = 0; lcnt < 4; lcnt++)
      {
        mlength = SIZEOF_STORED_DOUBLE;
        source = new_ptr + mlength + SIZEOF_STORED_DOUBLE * lcnt;
        while (mlength--)
          *dest++ = *--source;
      }
      tmp_str.length(SIZEOF_STORED_DOUBLE * lcnt);
*/
      double xmin, xmax, ymin, ymax;
/*
      float8store(buf,xmin);
      float8store(buf+8,xmax);
      float8store(buf+16,ymin);
      float8store(buf+24,ymax);
      memcpy(&xmin,new_ptr,sizeof(xmin));
      memcpy(&xmax,new_ptr + 8,sizeof(xmax));
      memcpy(&ymin,new_ptr + 16,sizeof(ymin));
      memcpy(&ymax,new_ptr + 24,sizeof(ymax));
      float8get(xmin, buf);
      float8get(xmax, buf + 8);
      float8get(ymin, buf + 16);
      float8get(ymax, buf + 24);
      DBUG_PRINT("info", ("spider geo is %f %f %f %f",
        xmin, xmax, ymin, ymax));
      DBUG_PRINT("info", ("spider geo is %.14g %.14g %.14g %.14g",
        xmin, xmax, ymin, ymax));
*/
      float8get(xmin, new_ptr);
      float8get(xmax, new_ptr + 8);
      float8get(ymin, new_ptr + 16);
      float8get(ymax, new_ptr + 24);
      DBUG_PRINT("info", ("spider geo is %f %f %f %f",
        xmin, xmax, ymin, ymax));
/*
      float8get(xmin, new_ptr + SIZEOF_STORED_DOUBLE * 4);
      float8get(xmax, new_ptr + SIZEOF_STORED_DOUBLE * 5);
      float8get(ymin, new_ptr + SIZEOF_STORED_DOUBLE * 6);
      float8get(ymax, new_ptr + SIZEOF_STORED_DOUBLE * 7);
      DBUG_PRINT("info", ("spider geo is %f %f %f %f",
        xmin, xmax, ymin, ymax));
      float8get(xmin, new_ptr + SIZEOF_STORED_DOUBLE * 8);
      float8get(xmax, new_ptr + SIZEOF_STORED_DOUBLE * 9);
      float8get(ymin, new_ptr + SIZEOF_STORED_DOUBLE * 10);
      float8get(ymax, new_ptr + SIZEOF_STORED_DOUBLE * 11);
      DBUG_PRINT("info", ("spider geo is %f %f %f %f",
        xmin, xmax, ymin, ymax));
      float8get(xmin, new_ptr + SIZEOF_STORED_DOUBLE * 12);
      float8get(xmax, new_ptr + SIZEOF_STORED_DOUBLE * 13);
      float8get(ymin, new_ptr + SIZEOF_STORED_DOUBLE * 14);
      float8get(ymax, new_ptr + SIZEOF_STORED_DOUBLE * 15);
      DBUG_PRINT("info", ("spider geo is %f %f %f %f",
        xmin, xmax, ymin, ymax));
*/
/*
      tmp_str.set_quick((char *) new_ptr, SIZEOF_STORED_DOUBLE * 4,
        &my_charset_bin);
*/
      tmp_str.length(0);
      tmp_str.q_append((char *) SPIDER_SQL_LINESTRING_HEAD_STR,
        SPIDER_SQL_LINESTRING_HEAD_LEN);
      tmp_str.q_append((char *) new_ptr, SIZEOF_STORED_DOUBLE);
      tmp_str.q_append((char *) new_ptr + SIZEOF_STORED_DOUBLE * 2,
        SIZEOF_STORED_DOUBLE);
      tmp_str.q_append((char *) new_ptr + SIZEOF_STORED_DOUBLE,
        SIZEOF_STORED_DOUBLE);
      tmp_str.q_append((char *) new_ptr + SIZEOF_STORED_DOUBLE * 3,
        SIZEOF_STORED_DOUBLE);
      ptr = tmp_str.get_str();
    } else {
      ptr = field->val_str(tmp_str.get_str(), new_ptr);
      tmp_str.mem_calc();
    }
  } else {
    ptr = field->val_str(tmp_str.get_str());
    tmp_str.mem_calc();
  }
  DBUG_PRINT("info", ("spider field->type() is %d", field->type()));
  DBUG_PRINT("info", ("spider ptr->length() is %d", ptr->length()));
/*
  if (
    field->type() == MYSQL_TYPE_BIT ||
    (field->type() >= MYSQL_TYPE_TINY_BLOB &&
      field->type() <= MYSQL_TYPE_BLOB)
  ) {
    uchar *hex_ptr = (uchar *) ptr->ptr(), *end_ptr;
    char *str_ptr;
    DBUG_PRINT("info", ("spider HEX"));
    if (str->reserve(SPIDER_SQL_HEX_LEN + ptr->length() * 2))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(SPIDER_SQL_HEX_STR, SPIDER_SQL_HEX_LEN);
    str_ptr = (char *) str->ptr() + str->length();
    for (end_ptr = hex_ptr + ptr->length(); hex_ptr < end_ptr; hex_ptr++)
    {
      *str_ptr++ = spider_dig_upper[(*hex_ptr) >> 4];
      *str_ptr++ = spider_dig_upper[(*hex_ptr) & 0x0F];
    }
    str->length(str->length() + ptr->length() * 2);
  } else 
*/
  spider_handlersocket_handler *hs_handler = (spider_handlersocket_handler *)
    spider->dbton_handler[spider_dbton_handlersocket.dbton_id];
  spider_string *hs_str;
  if (!(hs_str = hs_handler->hs_strs.add(
    &hs_handler->hs_strs_pos, ptr->ptr(), ptr->length())))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  SPIDER_HS_STRING_REF ref =
    SPIDER_HS_STRING_REF(hs_str->ptr(), hs_str->length());
  if (hs_handler->hs_adding_keys)
  {
    DBUG_PRINT("info", ("spider add to key:%s", hs_str->c_ptr_safe()));
    hs_handler->hs_keys.push_back(ref);
  } else {
    DBUG_PRINT("info", ("spider add to upd:%s", hs_str->c_ptr_safe()));
    hs_handler->hs_upds.push_back(ref);
  }
  DBUG_RETURN(0);
}

int spider_db_handlersocket_util::append_trx_isolation(
  spider_string *str,
  int trx_isolation
) {
  DBUG_ENTER("spider_db_handlersocket_util::append_trx_isolation");
  DBUG_PRINT("info",("spider this=%p", this));
  /* nothing to do */
  DBUG_RETURN(0);
}

int spider_db_handlersocket_util::append_autocommit(
  spider_string *str,
  bool autocommit
) {
  DBUG_ENTER("spider_db_handlersocket_util::append_autocommit");
  DBUG_PRINT("info",("spider this=%p", this));
  /* nothing to do */
  DBUG_RETURN(0);
}

int spider_db_handlersocket_util::append_sql_log_off(
  spider_string *str,
  bool sql_log_off
) {
  DBUG_ENTER("spider_db_handlersocket_util::append_sql_log_off");
  DBUG_PRINT("info",("spider this=%p", this));
  /* nothing to do */
  DBUG_RETURN(0);
}

int spider_db_handlersocket_util::append_wait_timeout(
  spider_string *str,
  int wait_timeout
) {
  DBUG_ENTER("spider_db_handlersocket_util::append_wait_timeout");
  DBUG_PRINT("info",("spider this=%p", this));
  /* nothing to do */
  DBUG_RETURN(0);
}

int spider_db_handlersocket_util::append_sql_mode(
  spider_string *str,
  sql_mode_t sql_mode
) {
  DBUG_ENTER("spider_db_handlersocket_util::append_sql_mode");
  DBUG_PRINT("info",("spider this=%p", this));
  /* nothing to do */
  DBUG_RETURN(0);
}

int spider_db_handlersocket_util::append_time_zone(
  spider_string *str,
  Time_zone *time_zone
) {
  DBUG_ENTER("spider_db_handlersocket_util::append_time_zone");
  DBUG_PRINT("info",("spider this=%p", this));
  /* nothing to do */
  DBUG_RETURN(0);
}

int spider_db_handlersocket_util::append_start_transaction(
  spider_string *str
) {
  DBUG_ENTER("spider_db_handlersocket_util::append_start_transaction");
  DBUG_PRINT("info",("spider this=%p", this));
  /* nothing to do */
  DBUG_RETURN(0);
}

int spider_db_handlersocket_util::append_xa_start(
  spider_string *str,
  XID *xid
) {
  DBUG_ENTER("spider_db_handlersocket_util::append_xa_start");
  DBUG_PRINT("info",("spider this=%p", this));
  /* nothing to do */
  DBUG_RETURN(0);
}

int spider_db_handlersocket_util::append_lock_table_head(
  spider_string *str
) {
  DBUG_ENTER("spider_db_handlersocket_util::append_lock_table_head");
  DBUG_PRINT("info",("spider this=%p", this));
  /* nothing to do */
  DBUG_RETURN(0);
}

int spider_db_handlersocket_util::append_lock_table_body(
  spider_string *str,
  const char *db_name,
  uint db_name_length,
  CHARSET_INFO *db_name_charset,
  const char *table_name,
  uint table_name_length,
  CHARSET_INFO *table_name_charset,
  int lock_type
) {
  DBUG_ENTER("spider_db_handlersocket_util::append_lock_table_body");
  DBUG_PRINT("info",("spider this=%p", this));
  /* nothing to do */
  DBUG_RETURN(0);
}

int spider_db_handlersocket_util::append_lock_table_tail(
  spider_string *str
) {
  DBUG_ENTER("spider_db_handlersocket_util::append_lock_table_tail");
  DBUG_PRINT("info",("spider this=%p", this));
  /* nothing to do */
  DBUG_RETURN(0);
}

int spider_db_handlersocket_util::append_unlock_table(
  spider_string *str
) {
  DBUG_ENTER("spider_db_handlersocket_util::append_unlock_table");
  DBUG_PRINT("info",("spider this=%p", this));
  /* nothing to do */
  DBUG_RETURN(0);
}

int spider_db_handlersocket_util::open_item_func(
  Item_func *item_func,
  ha_spider *spider,
  spider_string *str,
  const char *alias,
  uint alias_length,
  bool use_fields,
  spider_fields *fields
) {
  uint dbton_id = spider_dbton_handlersocket.dbton_id;
  int error_num;
  Item *item, **item_list = item_func->arguments();
  uint roop_count, item_count = item_func->argument_count(), start_item = 0;
  LEX_CSTRING func_name_c;
  const char *func_name = SPIDER_SQL_NULL_CHAR_STR,
    *separator_str = SPIDER_SQL_NULL_CHAR_STR,
    *last_str = SPIDER_SQL_NULL_CHAR_STR;
  int func_name_length = SPIDER_SQL_NULL_CHAR_LEN,
    separator_str_length = SPIDER_SQL_NULL_CHAR_LEN,
    last_str_length = SPIDER_SQL_NULL_CHAR_LEN;
  int use_pushdown_udf;
  bool merge_func = FALSE;
  DBUG_ENTER("spider_db_handlersocket_util::open_item_func");
  if (str)
  {
    if (str->reserve(SPIDER_SQL_OPEN_PAREN_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(SPIDER_SQL_OPEN_PAREN_STR, SPIDER_SQL_OPEN_PAREN_LEN);
  }
  DBUG_PRINT("info",("spider functype = %d", item_func->functype()));
  switch (item_func->functype())
  {
    case Item_func::ISNULL_FUNC:
      last_str = SPIDER_SQL_IS_NULL_STR;
      last_str_length = SPIDER_SQL_IS_NULL_LEN;
      break;
    case Item_func::ISNOTNULL_FUNC:
      last_str = SPIDER_SQL_IS_NOT_NULL_STR;
      last_str_length = SPIDER_SQL_IS_NOT_NULL_LEN;
      break;
    case Item_func::UNKNOWN_FUNC:
      func_name_c =      item_func->func_name_cstring();
      func_name =        func_name_c.str;
      func_name_length = func_name_c.length;
      DBUG_PRINT("info",("spider func_name = %s", func_name));
      DBUG_PRINT("info",("spider func_name_length = %d", func_name_length));
      if (func_name_length == 1 &&
        (
          !strncasecmp("+", func_name, func_name_length) ||
          !strncasecmp("-", func_name, func_name_length) ||
          !strncasecmp("*", func_name, func_name_length) ||
          !strncasecmp("/", func_name, func_name_length) ||
          !strncasecmp("%", func_name, func_name_length) ||
          !strncasecmp("&", func_name, func_name_length) ||
          !strncasecmp("|", func_name, func_name_length) ||
          !strncasecmp("^", func_name, func_name_length)
        )
      ) {
        /* no action */
        break;
      } else if (func_name_length == 2 &&
        (
          !strncasecmp("<<", func_name, func_name_length) ||
          !strncasecmp(">>", func_name, func_name_length)
        )
      ) {
        /* no action */
        break;
      } else if (func_name_length == 3 &&
        !strncasecmp("div", func_name, func_name_length)
      ) {
        /* no action */
        break;
      } else if (func_name_length == 4)
      {
        if (
          !strncasecmp("rand", func_name, func_name_length) &&
#ifdef SPIDER_Item_args_arg_count_IS_PROTECTED
          !item_func->argument_count()
#else
          !item_func->arg_count
#endif
        ) {
          if (str)
            str->length(str->length() - SPIDER_SQL_OPEN_PAREN_LEN);
          DBUG_RETURN(spider_db_open_item_int(item_func, NULL, spider, str,
            alias, alias_length, dbton_id, use_fields, fields));
        } else if (
          !strncasecmp("case", func_name, func_name_length)
        ) {
#ifdef ITEM_FUNC_CASE_PARAMS_ARE_PUBLIC
          Item_func_case *item_func_case = (Item_func_case *) item_func;
          if (str)
          {
            if (str->reserve(SPIDER_SQL_CASE_LEN))
              DBUG_RETURN(HA_ERR_OUT_OF_MEM);
            str->q_append(SPIDER_SQL_CASE_STR, SPIDER_SQL_CASE_LEN);
          }
          if (item_func_case->first_expr_num != -1)
          {
            if ((error_num = spider_db_print_item_type(
              item_list[item_func_case->first_expr_num], NULL, spider, str,
              alias, alias_length, dbton_id, use_fields, fields)))
              DBUG_RETURN(error_num);
          }
          for (roop_count = 0; roop_count < item_func_case->ncases;
            roop_count += 2)
          {
            if (str)
            {
              if (str->reserve(SPIDER_SQL_WHEN_LEN))
                DBUG_RETURN(HA_ERR_OUT_OF_MEM);
              str->q_append(SPIDER_SQL_WHEN_STR, SPIDER_SQL_WHEN_LEN);
            }
            if ((error_num = spider_db_print_item_type(
              item_list[roop_count], NULL, spider, str,
              alias, alias_length, dbton_id, use_fields, fields)))
              DBUG_RETURN(error_num);
            if (str)
            {
              if (str->reserve(SPIDER_SQL_THEN_LEN))
                DBUG_RETURN(HA_ERR_OUT_OF_MEM);
              str->q_append(SPIDER_SQL_THEN_STR, SPIDER_SQL_THEN_LEN);
            }
            if ((error_num = spider_db_print_item_type(
              item_list[roop_count + 1], NULL, spider, str,
              alias, alias_length, dbton_id, use_fields, fields)))
              DBUG_RETURN(error_num);
          }
          if (item_func_case->else_expr_num != -1)
          {
            if (str)
            {
              if (str->reserve(SPIDER_SQL_ELSE_LEN))
                DBUG_RETURN(HA_ERR_OUT_OF_MEM);
              str->q_append(SPIDER_SQL_ELSE_STR, SPIDER_SQL_ELSE_LEN);
            }
            if ((error_num = spider_db_print_item_type(
              item_list[item_func_case->else_expr_num], NULL, spider, str,
              alias, alias_length, dbton_id, use_fields, fields)))
              DBUG_RETURN(error_num);
          }
          if (str)
          {
            if (str->reserve(SPIDER_SQL_END_LEN + SPIDER_SQL_CLOSE_PAREN_LEN))
              DBUG_RETURN(HA_ERR_OUT_OF_MEM);
            str->q_append(SPIDER_SQL_END_STR, SPIDER_SQL_END_LEN);
            str->q_append(SPIDER_SQL_CLOSE_PAREN_STR,
              SPIDER_SQL_CLOSE_PAREN_LEN);
          }
          DBUG_RETURN(0);
#else
          DBUG_RETURN(ER_SPIDER_COND_SKIP_NUM);
#endif
        }
      } else if (func_name_length == 6 &&
        !strncasecmp("istrue", func_name, func_name_length)
      ) {
        last_str = SPIDER_SQL_IS_TRUE_STR;
        last_str_length = SPIDER_SQL_IS_TRUE_LEN;
        break;
      } else if (func_name_length == 7)
      {
        if (!strncasecmp("isfalse", func_name, func_name_length))
        {
          last_str = SPIDER_SQL_IS_FALSE_STR;
          last_str_length = SPIDER_SQL_IS_FALSE_LEN;
          break;
        } else if (
          !strncasecmp("sysdate", func_name, func_name_length) ||
          !strncasecmp("curdate", func_name, func_name_length) ||
          !strncasecmp("curtime", func_name, func_name_length)
        ) {
          if (str)
            str->length(str->length() - SPIDER_SQL_OPEN_PAREN_LEN);
          DBUG_RETURN(spider_db_open_item_string(item_func, NULL, spider, str,
            alias, alias_length, dbton_id, use_fields, fields));
        } else if (
          !strncasecmp("convert", func_name, func_name_length)
        ) {
          if (str)
          {
            if (str->reserve(func_name_length * 2 + SPIDER_SQL_OPEN_PAREN_LEN))
              DBUG_RETURN(HA_ERR_OUT_OF_MEM);
            str->q_append(func_name, func_name_length);
            str->q_append(SPIDER_SQL_OPEN_PAREN_STR,
              SPIDER_SQL_OPEN_PAREN_LEN);
            last_str = SPIDER_SQL_CLOSE_PAREN_STR;
            last_str_length = SPIDER_SQL_CLOSE_PAREN_LEN;
          }
          break;
        }
      } else if (func_name_length == 8 &&
        (
          !strncasecmp("utc_date", func_name, func_name_length) ||
          !strncasecmp("utc_time", func_name, func_name_length)
        )
      ) {
        if (str)
          str->length(str->length() - SPIDER_SQL_OPEN_PAREN_LEN);
        DBUG_RETURN(spider_db_open_item_string(item_func, NULL, spider, str,
          alias, alias_length, dbton_id, use_fields, fields));
      } else if (func_name_length == 9 &&
        !strncasecmp("isnottrue", func_name, func_name_length)
      ) {
        last_str = SPIDER_SQL_IS_NOT_TRUE_STR;
        last_str_length = SPIDER_SQL_IS_NOT_TRUE_LEN;
        break;
      } else if (func_name_length == 10)
      {
        if (!strncasecmp("isnotfalse", func_name, func_name_length))
        {
          last_str = SPIDER_SQL_IS_NOT_FALSE_STR;
          last_str_length = SPIDER_SQL_IS_NOT_FALSE_LEN;
          break;
        } else if (!strncasecmp("column_get", func_name, func_name_length))
        {
          if (str)
          {
            str->length(str->length() - SPIDER_SQL_OPEN_PAREN_LEN);
            if (str->reserve(func_name_length + SPIDER_SQL_OPEN_PAREN_LEN))
              DBUG_RETURN(HA_ERR_OUT_OF_MEM);
            str->q_append(func_name, func_name_length);
            str->q_append(SPIDER_SQL_OPEN_PAREN_STR, SPIDER_SQL_OPEN_PAREN_LEN);
          }
          func_name = SPIDER_SQL_COMMA_STR;
          func_name_length = SPIDER_SQL_COMMA_LEN;
          separator_str = SPIDER_SQL_COMMA_STR;
          separator_str_length = SPIDER_SQL_COMMA_LEN;
          break;
        }
      } else if (func_name_length == 12)
      {
        if (!strncasecmp("cast_as_date", func_name, func_name_length))
        {
          item = item_list[0];
          if (item->type() == Item::FUNC_ITEM)
          {
            DBUG_PRINT("info",("spider child is FUNC_ITEM"));
            Item_func *ifunc = (Item_func *) item;
            if (ifunc->functype() == Item_func::UNKNOWN_FUNC)
            {
              LEX_CSTRING child_func_name_c;
              const char *child_func_name;
              int child_func_name_length;
              DBUG_PRINT("info",("spider child is UNKNOWN_FUNC"));
              child_func_name_c =      ifunc->func_name_cstring();
              child_func_name =        child_func_name_c.str;
              child_func_name_length = child_func_name_c.length;
              DBUG_PRINT("info",("spider child func_name is %s", child_func_name));
              if (
                child_func_name_length == 10 &&
                !strncasecmp("column_get", child_func_name, child_func_name_length)
              ) {
                DBUG_PRINT("info",("spider this is merge func"));
                merge_func = TRUE;
              }
            }
          }

          if (str)
          {
            str->length(str->length() - SPIDER_SQL_OPEN_PAREN_LEN);
            if (!merge_func)
            {
              if (str->reserve(SPIDER_SQL_CAST_LEN))
                DBUG_RETURN(HA_ERR_OUT_OF_MEM);
              str->q_append(SPIDER_SQL_CAST_STR, SPIDER_SQL_CAST_LEN);
            }
          }
          last_str = SPIDER_SQL_AS_DATE_STR;
          last_str_length = SPIDER_SQL_AS_DATE_LEN;
          break;
        } else if (!strncasecmp("cast_as_time", func_name, func_name_length))
        {
          item = item_list[0];
          if (item->type() == Item::FUNC_ITEM)
          {
            DBUG_PRINT("info",("spider child is FUNC_ITEM"));
            Item_func *ifunc = (Item_func *) item;
            if (ifunc->functype() == Item_func::UNKNOWN_FUNC)
            {
              LEX_CSTRING child_func_name_c;
              const char *child_func_name;
              int child_func_name_length;
              DBUG_PRINT("info",("spider child is UNKNOWN_FUNC"));
              child_func_name_c =      ifunc->func_name_cstring();
              child_func_name =        child_func_name_c.str;
              child_func_name_length = child_func_name_c.length;
              DBUG_PRINT("info",("spider child func_name is %s", child_func_name));
              if (
                child_func_name_length == 10 &&
                !strncasecmp("column_get", child_func_name, child_func_name_length)
              ) {
                DBUG_PRINT("info",("spider this is merge func"));
                merge_func = TRUE;
              }
            }
          }

          if (str)
          {
            str->length(str->length() - SPIDER_SQL_OPEN_PAREN_LEN);
            if (!merge_func)
            {
              if (str->reserve(SPIDER_SQL_CAST_LEN))
                DBUG_RETURN(HA_ERR_OUT_OF_MEM);
              str->q_append(SPIDER_SQL_CAST_STR, SPIDER_SQL_CAST_LEN);
            }
          }
          last_str = SPIDER_SQL_AS_TIME_STR;
          last_str_length = SPIDER_SQL_AS_TIME_LEN;
          break;
        }
      } else if (func_name_length == 13)
      {
        if (!strncasecmp("utc_timestamp", func_name, func_name_length))
        {
          if (str)
            str->length(str->length() - SPIDER_SQL_OPEN_PAREN_LEN);
          DBUG_RETURN(spider_db_open_item_string(item_func, NULL, spider, str,
            alias, alias_length, dbton_id, use_fields, fields));
        } else if (!strncasecmp("timestampdiff", func_name, func_name_length))
        {
#ifdef ITEM_FUNC_TIMESTAMPDIFF_ARE_PUBLIC
          Item_func_timestamp_diff *item_func_timestamp_diff =
            (Item_func_timestamp_diff *) item_func;
          if (str)
          {
            const char *interval_str;
            uint interval_len;
            switch (item_func_timestamp_diff->int_type)
            {
              case INTERVAL_YEAR:
                interval_str = SPIDER_SQL_YEAR_STR;
                interval_len = SPIDER_SQL_YEAR_LEN;
                break;
              case INTERVAL_QUARTER:
                interval_str = SPIDER_SQL_QUARTER_STR;
                interval_len = SPIDER_SQL_QUARTER_LEN;
                break;
              case INTERVAL_MONTH:
                interval_str = SPIDER_SQL_MONTH_STR;
                interval_len = SPIDER_SQL_MONTH_LEN;
                break;
              case INTERVAL_WEEK:
                interval_str = SPIDER_SQL_WEEK_STR;
                interval_len = SPIDER_SQL_WEEK_LEN;
                break;
              case INTERVAL_DAY:
                interval_str = SPIDER_SQL_DAY_STR;
                interval_len = SPIDER_SQL_DAY_LEN;
                break;
              case INTERVAL_HOUR:
                interval_str = SPIDER_SQL_HOUR_STR;
                interval_len = SPIDER_SQL_HOUR_LEN;
                break;
              case INTERVAL_MINUTE:
                interval_str = SPIDER_SQL_MINUTE_STR;
                interval_len = SPIDER_SQL_MINUTE_LEN;
                break;
              case INTERVAL_SECOND:
                interval_str = SPIDER_SQL_SECOND_STR;
                interval_len = SPIDER_SQL_SECOND_LEN;
                break;
              case INTERVAL_MICROSECOND:
                interval_str = SPIDER_SQL_MICROSECOND_STR;
                interval_len = SPIDER_SQL_MICROSECOND_LEN;
                break;
              default:
                interval_str = "";
                interval_len = 0;
                break;
            }
            str->length(str->length() - SPIDER_SQL_OPEN_PAREN_LEN);
            if (str->reserve(func_name_length + SPIDER_SQL_OPEN_PAREN_LEN +
              interval_len + SPIDER_SQL_COMMA_LEN))
              DBUG_RETURN(HA_ERR_OUT_OF_MEM);
            str->q_append(func_name, func_name_length);
            str->q_append(SPIDER_SQL_OPEN_PAREN_STR, SPIDER_SQL_OPEN_PAREN_LEN);
            str->q_append(interval_str, interval_len);
            str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
          }
          if ((error_num = spider_db_print_item_type(item_list[0], NULL, spider,
            str, alias, alias_length, dbton_id, use_fields, fields)))
            DBUG_RETURN(error_num);
          if (str)
          {
            if (str->reserve(SPIDER_SQL_COMMA_LEN))
              DBUG_RETURN(HA_ERR_OUT_OF_MEM);
            str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
          }
          if ((error_num = spider_db_print_item_type(item_list[1], NULL, spider,
            str, alias, alias_length, dbton_id, use_fields, fields)))
            DBUG_RETURN(error_num);
          if (str)
          {
            if (str->reserve(SPIDER_SQL_CLOSE_PAREN_LEN))
              DBUG_RETURN(HA_ERR_OUT_OF_MEM);
            str->q_append(SPIDER_SQL_CLOSE_PAREN_STR,
              SPIDER_SQL_CLOSE_PAREN_LEN);
          }
          DBUG_RETURN(0);
#else
          DBUG_RETURN(ER_SPIDER_COND_SKIP_NUM);
#endif
        }
      } else if (func_name_length == 14)
      {
        if (!strncasecmp("cast_as_binary", func_name, func_name_length))
        {
          item = item_list[0];
          if (item->type() == Item::FUNC_ITEM)
          {
            DBUG_PRINT("info",("spider child is FUNC_ITEM"));
            Item_func *ifunc = (Item_func *) item;
            if (ifunc->functype() == Item_func::UNKNOWN_FUNC)
            {
              LEX_CSTRING child_func_name_c;
              const char *child_func_name;
              int child_func_name_length;
              DBUG_PRINT("info",("spider child is UNKNOWN_FUNC"));
              child_func_name_c =      ifunc->func_name_cstring();
              child_func_name =        child_func_name_c.str;
              child_func_name_length = child_func_name_c.length;
              DBUG_PRINT("info",("spider child func_name is %s", child_func_name));
              if (
                child_func_name_length == 10 &&
                !strncasecmp("column_get", child_func_name, child_func_name_length)
              ) {
                DBUG_PRINT("info",("spider this is merge func"));
                merge_func = TRUE;
              }
            }
          }

          if (str)
          {
            char tmp_buf[MAX_FIELD_WIDTH], *tmp_ptr, *tmp_ptr2;
            spider_string tmp_str(tmp_buf, MAX_FIELD_WIDTH, str->charset());
            tmp_str.init_calc_mem(123);
            tmp_str.length(0);
            str->length(str->length() - SPIDER_SQL_OPEN_PAREN_LEN);
            if (!merge_func)
            {
              if (str->reserve(SPIDER_SQL_CAST_LEN))
                DBUG_RETURN(HA_ERR_OUT_OF_MEM);
              str->q_append(SPIDER_SQL_CAST_STR, SPIDER_SQL_CAST_LEN);
            }
#if MYSQL_VERSION_ID < 50500
            item_func->print(tmp_str.get_str(), QT_IS);
#else
            item_func->print(tmp_str.get_str(), QT_TO_SYSTEM_CHARSET);
#endif
            tmp_str.mem_calc();
            if (tmp_str.reserve(1))
              DBUG_RETURN(HA_ERR_OUT_OF_MEM);
            tmp_ptr = tmp_str.c_ptr_quick();
            DBUG_PRINT("info",("spider tmp_ptr = %s", tmp_ptr));
            while ((tmp_ptr2 = strstr(tmp_ptr, SPIDER_SQL_AS_BINARY_STR)))
              tmp_ptr = tmp_ptr2 + 1;
            last_str = tmp_ptr - 1;
            last_str_length = strlen(last_str) - SPIDER_SQL_CLOSE_PAREN_LEN;
          }
          break;
        } else if (!strncasecmp("cast_as_signed", func_name, func_name_length))
        {
          item = item_list[0];
          if (item->type() == Item::FUNC_ITEM)
          {
            DBUG_PRINT("info",("spider child is FUNC_ITEM"));
            Item_func *ifunc = (Item_func *) item;
            if (ifunc->functype() == Item_func::UNKNOWN_FUNC)
            {
              LEX_CSTRING child_func_name_c;
              const char *child_func_name;
              int child_func_name_length;
              DBUG_PRINT("info",("spider child is UNKNOWN_FUNC"));
              child_func_name_c =      ifunc->func_name_cstring();
              child_func_name =        child_func_name_c.str;
              child_func_name_length = child_func_name_c.length;
              DBUG_PRINT("info",("spider child func_name is %s", child_func_name));
              if (
                child_func_name_length == 10 &&
                !strncasecmp("column_get", child_func_name, child_func_name_length)
              ) {
                DBUG_PRINT("info",("spider this is merge func"));
                merge_func = TRUE;
              }
            }
          }

          if (str)
          {
            str->length(str->length() - SPIDER_SQL_OPEN_PAREN_LEN);
            if (!merge_func)
            {
              if (str->reserve(SPIDER_SQL_CAST_LEN))
                DBUG_RETURN(HA_ERR_OUT_OF_MEM);
              str->q_append(SPIDER_SQL_CAST_STR, SPIDER_SQL_CAST_LEN);
            }
          }
          last_str = SPIDER_SQL_AS_SIGNED_STR;
          last_str_length = SPIDER_SQL_AS_SIGNED_LEN;
          break;
        }
      } else if (func_name_length == 16)
      {
        if (!strncasecmp("cast_as_unsigned", func_name, func_name_length))
        {
          item = item_list[0];
          if (item->type() == Item::FUNC_ITEM)
          {
            DBUG_PRINT("info",("spider child is FUNC_ITEM"));
            Item_func *ifunc = (Item_func *) item;
            if (ifunc->functype() == Item_func::UNKNOWN_FUNC)
            {
              LEX_CSTRING child_func_name_c;
              const char *child_func_name;
              int child_func_name_length;
              DBUG_PRINT("info",("spider child is UNKNOWN_FUNC"));
              child_func_name_c =      ifunc->func_name_cstring();
              child_func_name =        child_func_name_c.str;
              child_func_name_length = child_func_name_c.length;
              DBUG_PRINT("info",("spider child func_name is %s", child_func_name));
              if (
                child_func_name_length == 10 &&
                !strncasecmp("column_get", child_func_name, child_func_name_length)
              ) {
                DBUG_PRINT("info",("spider this is merge func"));
                merge_func = TRUE;
              }
            }
          }

          if (str)
          {
            str->length(str->length() - SPIDER_SQL_OPEN_PAREN_LEN);
            if (!merge_func)
            {
              if (str->reserve(SPIDER_SQL_CAST_LEN))
                DBUG_RETURN(HA_ERR_OUT_OF_MEM);
              str->q_append(SPIDER_SQL_CAST_STR, SPIDER_SQL_CAST_LEN);
            }
          }
          last_str = SPIDER_SQL_AS_UNSIGNED_STR;
          last_str_length = SPIDER_SQL_AS_UNSIGNED_LEN;
          break;
        } else if (!strncasecmp("decimal_typecast", func_name,
          func_name_length))
        {
          item = item_list[0];
          if (item->type() == Item::FUNC_ITEM)
          {
            DBUG_PRINT("info",("spider child is FUNC_ITEM"));
            Item_func *ifunc = (Item_func *) item;
            if (ifunc->functype() == Item_func::UNKNOWN_FUNC)
            {
              LEX_CSTRING child_func_name_c;
              const char *child_func_name;
              int child_func_name_length;
              DBUG_PRINT("info",("spider child is UNKNOWN_FUNC"));
              child_func_name_c =      ifunc->func_name_cstring();
              child_func_name =        child_func_name_c.str;
              child_func_name_length = child_func_name_c.length;
              DBUG_PRINT("info",("spider child func_name is %s", child_func_name));
              if (
                child_func_name_length == 10 &&
                !strncasecmp("column_get", child_func_name, child_func_name_length)
              ) {
                DBUG_PRINT("info",("spider this is merge func"));
                merge_func = TRUE;
              }
            }
          }

          if (str)
          {
            char tmp_buf[MAX_FIELD_WIDTH], *tmp_ptr, *tmp_ptr2;
            spider_string tmp_str(tmp_buf, MAX_FIELD_WIDTH, str->charset());
            tmp_str.init_calc_mem(124);
            tmp_str.length(0);
            str->length(str->length() - SPIDER_SQL_OPEN_PAREN_LEN);
            if (!merge_func)
            {
              if (str->reserve(SPIDER_SQL_CAST_LEN))
                DBUG_RETURN(HA_ERR_OUT_OF_MEM);
              str->q_append(SPIDER_SQL_CAST_STR, SPIDER_SQL_CAST_LEN);
            }
#if MYSQL_VERSION_ID < 50500
            item_func->print(tmp_str.get_str(), QT_IS);
#else
            item_func->print(tmp_str.get_str(), QT_TO_SYSTEM_CHARSET);
#endif
            tmp_str.mem_calc();
            if (tmp_str.reserve(1))
              DBUG_RETURN(HA_ERR_OUT_OF_MEM);
            tmp_ptr = tmp_str.c_ptr_quick();
            DBUG_PRINT("info",("spider tmp_ptr = %s", tmp_ptr));
            while ((tmp_ptr2 = strstr(tmp_ptr, SPIDER_SQL_AS_DECIMAL_STR)))
              tmp_ptr = tmp_ptr2 + 1;
            last_str = tmp_ptr - 1;
            last_str_length = strlen(last_str) - SPIDER_SQL_CLOSE_PAREN_LEN;
          }
          break;
        } else if (!strncasecmp("cast_as_datetime", func_name,
          func_name_length))
        {
          item = item_list[0];
          if (item->type() == Item::FUNC_ITEM)
          {
            DBUG_PRINT("info",("spider child is FUNC_ITEM"));
            Item_func *ifunc = (Item_func *) item;
            if (ifunc->functype() == Item_func::UNKNOWN_FUNC)
            {
              LEX_CSTRING child_func_name_c;
              const char *child_func_name;
              int child_func_name_length;
              DBUG_PRINT("info",("spider child is UNKNOWN_FUNC"));
              child_func_name_c =      ifunc->func_name_cstring();
              child_func_name =        child_func_name_c.str;
              child_func_name_length = child_func_name_c.length;
              DBUG_PRINT("info",("spider child func_name is %s", child_func_name));
              if (
                child_func_name_length == 10 &&
                !strncasecmp("column_get", child_func_name, child_func_name_length)
              ) {
                DBUG_PRINT("info",("spider this is merge func"));
                merge_func = TRUE;
              }
            }
          }

          if (str)
          {
            str->length(str->length() - SPIDER_SQL_OPEN_PAREN_LEN);
            if (!merge_func)
            {
              if (str->reserve(SPIDER_SQL_CAST_LEN))
                DBUG_RETURN(HA_ERR_OUT_OF_MEM);
              str->q_append(SPIDER_SQL_CAST_STR, SPIDER_SQL_CAST_LEN);
            }
          }
          last_str = SPIDER_SQL_AS_DATETIME_STR;
          last_str_length = SPIDER_SQL_AS_DATETIME_LEN;
          break;
        }
      } else if (func_name_length == 17)
      {
        if (!strncasecmp("date_add_interval", func_name, func_name_length))
        {
          Item_date_add_interval *item_date_add_interval =
            (Item_date_add_interval *) item_func;
          func_name = spider_db_timefunc_interval_str[
            item_date_add_interval->int_type];
          func_name_length = strlen(func_name);
          if ((error_num = spider_db_print_item_type(item_list[0], NULL, spider,
            str, alias, alias_length, dbton_id, use_fields, fields)))
            DBUG_RETURN(error_num);
          if (str)
          {
            if (item_date_add_interval->date_sub_interval)
            {
              if (str->reserve(SPIDER_SQL_NEGINTERVAL_LEN))
                DBUG_RETURN(HA_ERR_OUT_OF_MEM);
              str->q_append(SPIDER_SQL_NEGINTERVAL_STR,
                SPIDER_SQL_NEGINTERVAL_LEN);
            } else {
              if (str->reserve(SPIDER_SQL_INTERVAL_LEN))
                DBUG_RETURN(HA_ERR_OUT_OF_MEM);
              str->q_append(SPIDER_SQL_INTERVAL_STR, SPIDER_SQL_INTERVAL_LEN);
            }
          }
          if ((error_num = spider_db_print_item_type(item_list[1], NULL, spider,
            str, alias, alias_length, dbton_id, use_fields, fields)))
            DBUG_RETURN(error_num);
          if (str)
          {
            if (str->reserve(func_name_length + SPIDER_SQL_CLOSE_PAREN_LEN))
              DBUG_RETURN(HA_ERR_OUT_OF_MEM);
            str->q_append(func_name, func_name_length);
            str->q_append(SPIDER_SQL_CLOSE_PAREN_STR,
              SPIDER_SQL_CLOSE_PAREN_LEN);
          }
          DBUG_RETURN(0);
        }
      }
      if (str)
      {
        if (str->reserve(func_name_length + SPIDER_SQL_OPEN_PAREN_LEN))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        str->q_append(func_name, func_name_length);
        str->q_append(SPIDER_SQL_OPEN_PAREN_STR, SPIDER_SQL_OPEN_PAREN_LEN);
      }
      func_name = SPIDER_SQL_COMMA_STR;
      func_name_length = SPIDER_SQL_COMMA_LEN;
      separator_str = SPIDER_SQL_COMMA_STR;
      separator_str_length = SPIDER_SQL_COMMA_LEN;
      last_str = SPIDER_SQL_CLOSE_PAREN_STR;
      last_str_length = SPIDER_SQL_CLOSE_PAREN_LEN;
      break;
    case Item_func::NOW_FUNC:
      if (str)
        str->length(str->length() - SPIDER_SQL_OPEN_PAREN_LEN);
      DBUG_RETURN(spider_db_open_item_string(item_func, NULL, spider, str,
        alias, alias_length, dbton_id, use_fields, fields));
    case Item_func::CHAR_TYPECAST_FUNC:
      DBUG_PRINT("info",("spider CHAR_TYPECAST_FUNC"));
      {
        item = item_list[0];
        if (item->type() == Item::FUNC_ITEM)
        {
          DBUG_PRINT("info",("spider child is FUNC_ITEM"));
          Item_func *ifunc = (Item_func *) item;
          if (ifunc->functype() == Item_func::UNKNOWN_FUNC)
          {
            LEX_CSTRING child_func_name_c;
            const char *child_func_name;
            int child_func_name_length;
            DBUG_PRINT("info",("spider child is UNKNOWN_FUNC"));
            child_func_name_c =      ifunc->func_name_cstring();
            child_func_name =        child_func_name_c.str;
            child_func_name_length = child_func_name_c.length;
            DBUG_PRINT("info",("spider child func_name is %s", child_func_name));
            if (
              child_func_name_length == 10 &&
              !strncasecmp("column_get", child_func_name, child_func_name_length)
            ) {
              DBUG_PRINT("info",("spider this is merge func"));
              merge_func = TRUE;
            }
          }
        }

        if (str)
        {
          char tmp_buf[MAX_FIELD_WIDTH], *tmp_ptr, *tmp_ptr2;
          spider_string tmp_str(tmp_buf, MAX_FIELD_WIDTH, str->charset());
          tmp_str.init_calc_mem(125);
          tmp_str.length(0);
          str->length(str->length() - SPIDER_SQL_OPEN_PAREN_LEN);
          if (!merge_func)
          {
            if (str->reserve(SPIDER_SQL_CAST_LEN))
              DBUG_RETURN(HA_ERR_OUT_OF_MEM);
            str->q_append(SPIDER_SQL_CAST_STR, SPIDER_SQL_CAST_LEN);
          }
#if MYSQL_VERSION_ID < 50500
          item_func->print(tmp_str.get_str(), QT_IS);
#else
          item_func->print(tmp_str.get_str(), QT_TO_SYSTEM_CHARSET);
#endif
          tmp_str.mem_calc();
          if (tmp_str.reserve(1))
            DBUG_RETURN(HA_ERR_OUT_OF_MEM);
          tmp_ptr = tmp_str.c_ptr_quick();
          DBUG_PRINT("info",("spider tmp_ptr = %s", tmp_ptr));
          while ((tmp_ptr2 = strstr(tmp_ptr, SPIDER_SQL_AS_CHAR_STR)))
            tmp_ptr = tmp_ptr2 + 1;
          last_str = tmp_ptr - 1;
          last_str_length = strlen(last_str) - SPIDER_SQL_CLOSE_PAREN_LEN;
        }
      }
      break;
    case Item_func::NOT_FUNC:
      DBUG_PRINT("info",("spider NOT_FUNC"));
      if (item_list[0]->type() == Item::COND_ITEM)
      {
        DBUG_PRINT("info",("spider item_list[0] is COND_ITEM"));
        Item_cond *item_cond = (Item_cond *) item_list[0];
        if (item_cond->functype() == Item_func::COND_AND_FUNC)
        {
          DBUG_PRINT("info",("spider item_cond is COND_AND_FUNC"));
          List_iterator_fast<Item> lif(*(item_cond->argument_list()));
          bool has_expr_cache_item = FALSE;
          bool has_isnotnull_func = FALSE;
          bool has_other_item = FALSE;
          while((item = lif++))
          {
#ifdef SPIDER_HAS_EXPR_CACHE_ITEM
            if (
              item->type() == Item::EXPR_CACHE_ITEM
            ) {
              DBUG_PRINT("info",("spider EXPR_CACHE_ITEM"));
              has_expr_cache_item = TRUE;
            } else
#endif
            if (
              item->type() == Item::FUNC_ITEM &&
              ((Item_func *) item)->functype() == Item_func::ISNOTNULL_FUNC
            ) {
              DBUG_PRINT("info",("spider ISNOTNULL_FUNC"));
              has_isnotnull_func = TRUE;
            } else {
              DBUG_PRINT("info",("spider has other item"));
              DBUG_PRINT("info",("spider COND type=%d", item->type()));
              has_other_item = TRUE;
            }
          }
          if (has_expr_cache_item && has_isnotnull_func && !has_other_item)
          {
            DBUG_PRINT("info",("spider NOT EXISTS skip"));
            DBUG_RETURN(ER_SPIDER_COND_SKIP_NUM);
          }
        }
      }
      if (str)
      {
        func_name_c =      item_func->func_name_cstring();
        func_name =        func_name_c.str;
        func_name_length = func_name_c.length;
        if (str->reserve(func_name_length + SPIDER_SQL_SPACE_LEN))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        str->q_append(func_name, func_name_length);
        str->q_append(SPIDER_SQL_SPACE_STR, SPIDER_SQL_SPACE_LEN);
      }
      break;
    case Item_func::NEG_FUNC:
      if (str)
      {
        func_name_c =      item_func->func_name_cstring();
        func_name =        func_name_c.str;
        func_name_length = func_name_c.length;
        if (str->reserve(func_name_length + SPIDER_SQL_SPACE_LEN))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        str->q_append(func_name, func_name_length);
        str->q_append(SPIDER_SQL_SPACE_STR, SPIDER_SQL_SPACE_LEN);
      }
      break;
    case Item_func::IN_FUNC:
      if (((Item_func_opt_neg *) item_func)->negated)
      {
        func_name = SPIDER_SQL_NOT_IN_STR;
        func_name_length = SPIDER_SQL_NOT_IN_LEN;
        separator_str = SPIDER_SQL_COMMA_STR;
        separator_str_length = SPIDER_SQL_COMMA_LEN;
        last_str = SPIDER_SQL_CLOSE_PAREN_STR;
        last_str_length = SPIDER_SQL_CLOSE_PAREN_LEN;
      } else {
        func_name = SPIDER_SQL_IN_STR;
        func_name_length = SPIDER_SQL_IN_LEN;
        separator_str = SPIDER_SQL_COMMA_STR;
        separator_str_length = SPIDER_SQL_COMMA_LEN;
        last_str = SPIDER_SQL_CLOSE_PAREN_STR;
        last_str_length = SPIDER_SQL_CLOSE_PAREN_LEN;
      }
      break;
    case Item_func::BETWEEN:
      if (((Item_func_opt_neg *) item_func)->negated)
      {
        func_name = SPIDER_SQL_NOT_BETWEEN_STR;
        func_name_length = SPIDER_SQL_NOT_BETWEEN_LEN;
        separator_str = SPIDER_SQL_AND_STR;
        separator_str_length = SPIDER_SQL_AND_LEN;
      } else {
        func_name_c =      item_func->func_name_cstring();
        func_name =        func_name_c.str;
        func_name_length = func_name_c.length;
        separator_str = SPIDER_SQL_AND_STR;
        separator_str_length = SPIDER_SQL_AND_LEN;
      }
      break;
    case Item_func::UDF_FUNC:
      use_pushdown_udf = spider_param_use_pushdown_udf(spider->trx->thd,
        spider->share->use_pushdown_udf);
      if (!use_pushdown_udf)
        DBUG_RETURN(ER_SPIDER_COND_SKIP_NUM);
      if (str)
      {
        func_name_c =      item_func->func_name_cstring();
        func_name =        func_name_c.str;
        func_name_length = func_name_c.length;
        DBUG_PRINT("info",("spider func_name = %s", func_name));
        DBUG_PRINT("info",("spider func_name_length = %d", func_name_length));
        if (str->reserve(func_name_length + SPIDER_SQL_OPEN_PAREN_LEN))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        str->q_append(func_name, func_name_length);
        str->q_append(SPIDER_SQL_OPEN_PAREN_STR, SPIDER_SQL_OPEN_PAREN_LEN);
      }
      func_name = SPIDER_SQL_COMMA_STR;
      func_name_length = SPIDER_SQL_COMMA_LEN;
      separator_str = SPIDER_SQL_COMMA_STR;
      separator_str_length = SPIDER_SQL_COMMA_LEN;
      last_str = SPIDER_SQL_CLOSE_PAREN_STR;
      last_str_length = SPIDER_SQL_CLOSE_PAREN_LEN;
      break;
#ifdef MARIADB_BASE_VERSION
    case Item_func::XOR_FUNC:
#else
    case Item_func::COND_XOR_FUNC:
#endif
      if (str)
        str->length(str->length() - SPIDER_SQL_OPEN_PAREN_LEN);
      DBUG_RETURN(
        spider_db_open_item_cond((Item_cond *) item_func, spider, str,
          alias, alias_length, dbton_id, use_fields, fields));
    case Item_func::TRIG_COND_FUNC:
      DBUG_RETURN(ER_SPIDER_COND_SKIP_NUM);
    case Item_func::GUSERVAR_FUNC:
      if (str)
        str->length(str->length() - SPIDER_SQL_OPEN_PAREN_LEN);
      if (item_func->result_type() == STRING_RESULT)
        DBUG_RETURN(spider_db_open_item_string(item_func, NULL, spider, str,
          alias, alias_length, dbton_id, use_fields, fields));
      else
        DBUG_RETURN(spider_db_open_item_int(item_func, NULL, spider, str,
          alias, alias_length, dbton_id, use_fields, fields));
    case Item_func::FT_FUNC:
      if (spider_db_check_ft_idx(item_func, spider) == MAX_KEY)
        DBUG_RETURN(ER_SPIDER_COND_SKIP_NUM);
      start_item = 1;
      if (str)
      {
        if (str->reserve(SPIDER_SQL_MATCH_LEN))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        str->q_append(SPIDER_SQL_MATCH_STR, SPIDER_SQL_MATCH_LEN);
      }
      separator_str = SPIDER_SQL_COMMA_STR;
      separator_str_length = SPIDER_SQL_COMMA_LEN;
      last_str = SPIDER_SQL_CLOSE_PAREN_STR;
      last_str_length = SPIDER_SQL_CLOSE_PAREN_LEN;
      break;
    case Item_func::SP_EQUALS_FUNC:
      if (str)
      {
        func_name = SPIDER_SQL_MBR_EQUAL_STR;
        func_name_length = SPIDER_SQL_MBR_EQUAL_LEN;
        DBUG_PRINT("info",("spider func_name = %s", func_name));
        DBUG_PRINT("info",("spider func_name_length = %d", func_name_length));
        if (str->reserve(func_name_length))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        str->q_append(func_name, func_name_length);
      }
      func_name = SPIDER_SQL_COMMA_STR;
      func_name_length = SPIDER_SQL_COMMA_LEN;
      separator_str = SPIDER_SQL_COMMA_STR;
      separator_str_length = SPIDER_SQL_COMMA_LEN;
      last_str = SPIDER_SQL_CLOSE_PAREN_STR;
      last_str_length = SPIDER_SQL_CLOSE_PAREN_LEN;
      break;
    case Item_func::SP_DISJOINT_FUNC:
    case Item_func::SP_INTERSECTS_FUNC:
    case Item_func::SP_TOUCHES_FUNC:
    case Item_func::SP_CROSSES_FUNC:
    case Item_func::SP_WITHIN_FUNC:
    case Item_func::SP_CONTAINS_FUNC:
    case Item_func::SP_OVERLAPS_FUNC:
      if (str)
      {
        func_name_c =      item_func->func_name_cstring();
        func_name =        func_name_c.str;
        func_name_length = func_name_c.length;
        DBUG_PRINT("info",("spider func_name = %s", func_name));
        DBUG_PRINT("info",("spider func_name_length = %d", func_name_length));
        if (str->reserve(
#ifndef SPIDER_ITEM_GEOFUNC_NAME_HAS_MBR
          SPIDER_SQL_MBR_LEN +
#endif
          func_name_length + SPIDER_SQL_OPEN_PAREN_LEN))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
#ifndef SPIDER_ITEM_GEOFUNC_NAME_HAS_MBR
        str->q_append(SPIDER_SQL_MBR_STR, SPIDER_SQL_MBR_LEN);
#endif
        str->q_append(func_name, func_name_length);
        str->q_append(SPIDER_SQL_OPEN_PAREN_STR, SPIDER_SQL_OPEN_PAREN_LEN);
      }
      func_name = SPIDER_SQL_COMMA_STR;
      func_name_length = SPIDER_SQL_COMMA_LEN;
      separator_str = SPIDER_SQL_COMMA_STR;
      separator_str_length = SPIDER_SQL_COMMA_LEN;
      last_str = SPIDER_SQL_CLOSE_PAREN_STR;
      last_str_length = SPIDER_SQL_CLOSE_PAREN_LEN;
      break;
    case Item_func::EQ_FUNC:
    case Item_func::EQUAL_FUNC:
    case Item_func::NE_FUNC:
    case Item_func::LT_FUNC:
    case Item_func::LE_FUNC:
    case Item_func::GE_FUNC:
    case Item_func::GT_FUNC:
    case Item_func::LIKE_FUNC:
      if (str)
      {
        func_name_c =      item_func->func_name_cstring();
        func_name =        func_name_c.str;
        func_name_length = func_name_c.length;
      }
      break;
    default:
      THD *thd = spider->trx->thd;
      SPIDER_SHARE *share = spider->share;
      if (spider_param_skip_default_condition(thd,
        share->skip_default_condition))
        DBUG_RETURN(ER_SPIDER_COND_SKIP_NUM);
      if (str)
      {
        func_name_c =      item_func->func_name_cstring();
        func_name =        func_name_c.str;
        func_name_length = func_name_c.length;
      }
      break;
  }
  DBUG_PRINT("info",("spider func_name = %s", func_name));
  DBUG_PRINT("info",("spider func_name_length = %d", func_name_length));
  DBUG_PRINT("info",("spider separator_str = %s", separator_str));
  DBUG_PRINT("info",("spider separator_str_length = %d", separator_str_length));
  DBUG_PRINT("info",("spider last_str = %s", last_str));
  DBUG_PRINT("info",("spider last_str_length = %d", last_str_length));
  if (item_count)
  {
    item_count--;
    for (roop_count = start_item; roop_count < item_count; roop_count++)
    {
      item = item_list[roop_count];
      if ((error_num = spider_db_print_item_type(item, NULL, spider, str,
        alias, alias_length, dbton_id, use_fields, fields)))
        DBUG_RETURN(error_num);
      if (roop_count == 1)
      {
        func_name = separator_str;
        func_name_length = separator_str_length;
      }
      if (str)
      {
        if (str->reserve(func_name_length + SPIDER_SQL_SPACE_LEN * 2))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        str->q_append(SPIDER_SQL_SPACE_STR, SPIDER_SQL_SPACE_LEN);
        str->q_append(func_name, func_name_length);
        str->q_append(SPIDER_SQL_SPACE_STR, SPIDER_SQL_SPACE_LEN);
      }
    }
    item = item_list[roop_count];
    if ((error_num = spider_db_print_item_type(item, NULL, spider, str,
      alias, alias_length, dbton_id, use_fields, fields)))
      DBUG_RETURN(error_num);
  }
  if (item_func->functype() == Item_func::FT_FUNC)
  {
    Item_func_match *item_func_match = (Item_func_match *)item_func;
    if (str)
    {
      if (str->reserve(SPIDER_SQL_AGAINST_LEN))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      str->q_append(SPIDER_SQL_AGAINST_STR, SPIDER_SQL_AGAINST_LEN);
    }
    item = item_list[0];
    if ((error_num = spider_db_print_item_type(item, NULL, spider, str,
      alias, alias_length, dbton_id, use_fields, fields)))
      DBUG_RETURN(error_num);
    if (str)
    {
      if (str->reserve(
        ((item_func_match->match_flags & FT_BOOL) ?
          SPIDER_SQL_IN_BOOLEAN_MODE_LEN : 0) +
        ((item_func_match->match_flags & FT_EXPAND) ?
          SPIDER_SQL_WITH_QUERY_EXPANSION_LEN : 0)
      ))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      if (item_func_match->match_flags & FT_BOOL)
        str->q_append(SPIDER_SQL_IN_BOOLEAN_MODE_STR,
          SPIDER_SQL_IN_BOOLEAN_MODE_LEN);
      if (item_func_match->match_flags & FT_EXPAND)
        str->q_append(SPIDER_SQL_WITH_QUERY_EXPANSION_STR,
          SPIDER_SQL_WITH_QUERY_EXPANSION_LEN);
    }
  } else if (item_func->functype() == Item_func::UNKNOWN_FUNC)
  {
    if (
      func_name_length == 7 &&
      !strncasecmp("convert", func_name, func_name_length)
    ) {
      if (str)
      {
        Item_func_conv_charset *item_func_conv_charset =
          (Item_func_conv_charset *)item_func;
        CHARSET_INFO *conv_charset =
          item_func_conv_charset->SPIDER_Item_func_conv_charset_conv_charset;
        uint cset_length = strlen(conv_charset->csname);
        if (str->reserve(SPIDER_SQL_USING_LEN + cset_length))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        str->q_append(SPIDER_SQL_USING_STR, SPIDER_SQL_USING_LEN);
        str->q_append(conv_charset->csname, cset_length);
      }
    }
  }
  if (str)
  {
    if (merge_func)
      str->length(str->length() - SPIDER_SQL_CLOSE_PAREN_LEN);
    if (str->reserve(last_str_length + SPIDER_SQL_CLOSE_PAREN_LEN))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    str->q_append(last_str, last_str_length);
    str->q_append(SPIDER_SQL_CLOSE_PAREN_STR, SPIDER_SQL_CLOSE_PAREN_LEN);
  }
  DBUG_RETURN(0);
}

#ifdef HANDLER_HAS_DIRECT_AGGREGATE
int spider_db_handlersocket_util::open_item_sum_func(
  Item_sum *item_sum,
  ha_spider *spider,
  spider_string *str,
  const char *alias,
  uint alias_length,
  bool use_fields,
  spider_fields *fields
) {
  uint dbton_id = spider_dbton_handlersocket.dbton_id;
  uint roop_count, item_count = item_sum->get_arg_count();
  int error_num;
  DBUG_ENTER("spider_db_handlersocket_util::open_item_sum_func");
  DBUG_PRINT("info",("spider Sumfunctype = %d", item_sum->sum_func()));
  switch (item_sum->sum_func())
  {
    case Item_sum::COUNT_FUNC:
    case Item_sum::SUM_FUNC:
    case Item_sum::MIN_FUNC:
    case Item_sum::MAX_FUNC:
      {
        LEX_CSTRING func_name_c = item_sum->func_name_cstring();
        const char *func_name =   func_name_c.str;
        uint func_name_length =   func_name_c.length;
        Item *item, **args = item_sum->get_args();
        if (str)
        {
          if (str->reserve(func_name_length))
            DBUG_RETURN(HA_ERR_OUT_OF_MEM);
          str->q_append(func_name, func_name_length);
        }
        if (item_count)
        {
          item_count--;
          for (roop_count = 0; roop_count < item_count; roop_count++)
          {
            item = args[roop_count];
            if ((error_num = spider_db_print_item_type(item, NULL, spider, str,
              alias, alias_length, dbton_id, use_fields, fields)))
              DBUG_RETURN(error_num);
            if (str)
            {
              if (str->reserve(SPIDER_SQL_COMMA_LEN))
                DBUG_RETURN(HA_ERR_OUT_OF_MEM);
              str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
            }
          }
          item = args[roop_count];
          if ((error_num = spider_db_print_item_type(item, NULL, spider, str,
            alias, alias_length, dbton_id, use_fields, fields)))
            DBUG_RETURN(error_num);
        }
        if (str)
        {
          if (str->reserve(SPIDER_SQL_CLOSE_PAREN_LEN))
            DBUG_RETURN(HA_ERR_OUT_OF_MEM);
          str->q_append(SPIDER_SQL_CLOSE_PAREN_STR,
            SPIDER_SQL_CLOSE_PAREN_LEN);
        }
      }
      break;
    case Item_sum::COUNT_DISTINCT_FUNC:
    case Item_sum::SUM_DISTINCT_FUNC:
    case Item_sum::AVG_FUNC:
    case Item_sum::AVG_DISTINCT_FUNC:
    case Item_sum::STD_FUNC:
    case Item_sum::VARIANCE_FUNC:
    case Item_sum::SUM_BIT_FUNC:
    case Item_sum::UDF_SUM_FUNC:
    case Item_sum::GROUP_CONCAT_FUNC:
    default:
      DBUG_RETURN(ER_SPIDER_COND_SKIP_NUM);
  }
  DBUG_RETURN(0);
}
#endif

int spider_db_handlersocket_util::append_escaped_util(
  spider_string *to,
  String *from
) {
  DBUG_ENTER("spider_db_handlersocket_util::append_escaped_util");
  DBUG_PRINT("info",("spider this=%p", this));
  to->append_escape_string(from->ptr(), from->length());
  DBUG_RETURN(0);
}

#ifdef SPIDER_HAS_GROUP_BY_HANDLER
int spider_db_handlersocket_util::append_from_and_tables(
  ha_spider *spider,
  spider_fields *fields,
  spider_string *str,
  TABLE_LIST *table_list,
  uint table_count
) {
  DBUG_ENTER("spider_db_handlersocket_util::append_from_and_tables");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_db_handlersocket_util::reappend_tables(
  spider_fields *fields,
  SPIDER_LINK_IDX_CHAIN *link_idx_chain,
  spider_string *str
) {
  DBUG_ENTER("spider_db_handlersocket_util::reappend_tables");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_db_handlersocket_util::append_where(
  spider_string *str
) {
  DBUG_ENTER("spider_db_handlersocket_util::append_where");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_db_handlersocket_util::append_having(
  spider_string *str
) {
  DBUG_ENTER("spider_db_handlersocket_util::append_having");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}
#endif

spider_handlersocket_share::spider_handlersocket_share(
  st_spider_share *share
) : spider_db_share(
  share,
  spider_dbton_handlersocket.dbton_id
),
  table_names_str(NULL),
  db_names_str(NULL),
  db_table_str(NULL),
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
  db_table_str_hash_value(NULL),
#endif
  table_nm_max_length(0),
  db_nm_max_length(0),
  column_name_str(NULL),
  same_db_table_name(TRUE),
  first_all_link_idx(-1)
{
  DBUG_ENTER("spider_handlersocket_share::spider_handlersocket_share");
  DBUG_PRINT("info",("spider this=%p", this));
  spider_alloc_calc_mem_init(mem_calc, 186);
  spider_alloc_calc_mem(spider_current_trx, mem_calc, sizeof(*this));
  DBUG_VOID_RETURN;
}

spider_handlersocket_share::~spider_handlersocket_share()
{
  DBUG_ENTER("spider_handlersocket_share::~spider_handlersocket_share");
  DBUG_PRINT("info",("spider this=%p", this));
  free_column_name_str();
  free_table_names_str();
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
  if (db_table_str_hash_value)
  {
    spider_free(spider_current_trx, db_table_str_hash_value, MYF(0));
  }
#endif
  spider_free_mem_calc(spider_current_trx, mem_calc_id, sizeof(*this));
  DBUG_VOID_RETURN;
}

int spider_handlersocket_share::init()
{
  int error_num;
  DBUG_ENTER("spider_handlersocket_share::init");
  DBUG_PRINT("info",("spider this=%p", this));
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
  if (!(db_table_str_hash_value = (my_hash_value_type *)
    spider_bulk_alloc_mem(spider_current_trx, 203,
      __func__, __FILE__, __LINE__, MYF(MY_WME | MY_ZEROFILL),
      &db_table_str_hash_value,
        sizeof(my_hash_value_type) * spider_share->all_link_count,
      NullS))
  ) {
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
#endif

  if (
    (error_num = create_table_names_str()) ||
    (
      spider_share->table_share &&
      (error_num = create_column_name_str())
    )
  ) {
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  DBUG_RETURN(0);
}

int spider_handlersocket_share::append_table_name(
  spider_string *str,
  int all_link_idx
) {
  const char *db_nm = db_names_str[all_link_idx].ptr();
  uint db_nm_len = db_names_str[all_link_idx].length();
  const char *table_nm = table_names_str[all_link_idx].ptr();
  uint table_nm_len = table_names_str[all_link_idx].length();
  DBUG_ENTER("spider_handlersocket_share::append_table_name");
  DBUG_PRINT("info",("spider this=%p", this));
  if (str->reserve(db_nm_len + SPIDER_SQL_DOT_LEN + table_nm_len +
    /* SPIDER_SQL_NAME_QUOTE_LEN */ 4))
  {
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  spider_db_handlersocket_utility.append_name(str, db_nm, db_nm_len);
  str->q_append(SPIDER_SQL_DOT_STR, SPIDER_SQL_DOT_LEN);
  spider_db_handlersocket_utility.append_name(str, table_nm, table_nm_len);
  DBUG_RETURN(0);
}

int spider_handlersocket_share::create_table_names_str()
{
  int error_num, roop_count;
  uint table_nm_len, db_nm_len;
  spider_string *str, *first_tbl_nm_str, *first_db_nm_str, *first_db_tbl_str;
  char *first_tbl_nm, *first_db_nm;
  uint dbton_id = spider_dbton_handlersocket.dbton_id;
  DBUG_ENTER("spider_handlersocket_share::create_table_names_str");
  table_names_str = NULL;
  db_names_str = NULL;
  db_table_str = NULL;
  if (
    !(table_names_str = new spider_string[spider_share->all_link_count]) ||
    !(db_names_str = new spider_string[spider_share->all_link_count]) ||
    !(db_table_str = new spider_string[spider_share->all_link_count])
  ) {
    error_num = HA_ERR_OUT_OF_MEM;
    goto error;
  }

  same_db_table_name = TRUE;
  first_tbl_nm = spider_share->tgt_table_names[0];
  first_db_nm = spider_share->tgt_dbs[0];
  table_nm_len = spider_share->tgt_table_names_lengths[0];
  db_nm_len = spider_share->tgt_dbs_lengths[0];
  first_tbl_nm_str = &table_names_str[0];
  first_db_nm_str = &db_names_str[0];
  first_db_tbl_str = &db_table_str[0];
  for (roop_count = 0; roop_count < (int) spider_share->all_link_count;
    roop_count++)
  {
    table_names_str[roop_count].init_calc_mem(86);
    db_names_str[roop_count].init_calc_mem(87);
    db_table_str[roop_count].init_calc_mem(88);
    if (spider_share->sql_dbton_ids[roop_count] != dbton_id)
      continue;
    if (first_all_link_idx == -1)
      first_all_link_idx = roop_count;

    str = &table_names_str[roop_count];
    if (
      roop_count != 0 &&
      same_db_table_name &&
      spider_share->tgt_table_names_lengths[roop_count] == table_nm_len &&
      !memcmp(first_tbl_nm, spider_share->tgt_table_names[roop_count],
        table_nm_len)
    ) {
      if (str->copy(*first_tbl_nm_str))
      {
        error_num = HA_ERR_OUT_OF_MEM;
        goto error;
      }
    } else {
      str->set_charset(spider_share->access_charset);
      if ((error_num = spider_db_append_name_with_quote_str(str,
        spider_share->tgt_table_names[roop_count], dbton_id)))
        goto error;
      if (roop_count)
      {
        same_db_table_name = FALSE;
        DBUG_PRINT("info", ("spider found different table name %s",
          spider_share->tgt_table_names[roop_count]));
        if (str->length() > table_nm_max_length)
          table_nm_max_length = str->length();
      } else
        table_nm_max_length = str->length();
    }

    str = &db_names_str[roop_count];
    if (
      roop_count != 0 &&
      same_db_table_name &&
      spider_share->tgt_dbs_lengths[roop_count] == db_nm_len &&
      !memcmp(first_db_nm, spider_share->tgt_dbs[roop_count],
        db_nm_len)
    ) {
      if (str->copy(*first_db_nm_str))
      {
        error_num = HA_ERR_OUT_OF_MEM;
        goto error;
      }
    } else {
      str->set_charset(spider_share->access_charset);
      if ((error_num = spider_db_append_name_with_quote_str(str,
        spider_share->tgt_dbs[roop_count], dbton_id)))
        goto error;
      if (roop_count)
      {
        same_db_table_name = FALSE;
        DBUG_PRINT("info", ("spider found different db name %s",
          spider_share->tgt_dbs[roop_count]));
        if (str->length() > db_nm_max_length)
          db_nm_max_length = str->length();
      } else
        db_nm_max_length = str->length();
    }

    str = &db_table_str[roop_count];
    if (
      roop_count != 0 &&
      same_db_table_name
    ) {
      if (str->copy(*first_db_tbl_str))
      {
        error_num = HA_ERR_OUT_OF_MEM;
        goto error;
      }
    } else {
      str->set_charset(spider_share->access_charset);
      if ((error_num = append_table_name(str, roop_count)))
        goto error;
    }
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
    db_table_str_hash_value[roop_count] = my_calc_hash(
      &spider_open_connections, (uchar*) str->ptr(), str->length());
#endif
  }
  DBUG_RETURN(0);

error:
  if (db_table_str)
  {
    delete [] db_table_str;
    db_table_str = NULL;
  }
  if (db_names_str)
  {
    delete [] db_names_str;
    db_names_str = NULL;
  }
  if (table_names_str)
  {
    delete [] table_names_str;
    table_names_str = NULL;
  }
  DBUG_RETURN(error_num);
}

void spider_handlersocket_share::free_table_names_str()
{
  DBUG_ENTER("spider_handlersocket_share::free_table_names_str");
  if (db_table_str)
  {
    delete [] db_table_str;
    db_table_str = NULL;
  }
  if (db_names_str)
  {
    delete [] db_names_str;
    db_names_str = NULL;
  }
  if (table_names_str)
  {
    delete [] table_names_str;
    table_names_str = NULL;
  }
  DBUG_VOID_RETURN;
}

int spider_handlersocket_share::create_column_name_str()
{
  spider_string *str;
  int error_num;
  Field **field;
  TABLE_SHARE *table_share = spider_share->table_share;
  uint dbton_id = spider_dbton_handlersocket.dbton_id;
  DBUG_ENTER("spider_handlersocket_share::create_column_name_str");
  if (
    table_share->fields &&
    !(column_name_str = new spider_string[table_share->fields])
  )
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  for (field = table_share->field, str = column_name_str;
   *field; field++, str++)
  {
    str->init_calc_mem(202);
    str->set_charset(spider_share->access_charset);
    if ((error_num = spider_db_append_name_with_quote_str(str,
      (*field)->field_name, dbton_id)))
      goto error;
  }
  DBUG_RETURN(0);

error:
  if (column_name_str)
  {
    delete [] column_name_str;
    column_name_str = NULL;
  }
  DBUG_RETURN(error_num);
}

void spider_handlersocket_share::free_column_name_str()
{
  DBUG_ENTER("spider_handlersocket_share::free_column_name_str");
  if (column_name_str)
  {
    delete [] column_name_str;
    column_name_str = NULL;
  }
  DBUG_VOID_RETURN;
}

uint spider_handlersocket_share::get_column_name_length(
  uint field_index
) {
  DBUG_ENTER("spider_handlersocket_share::get_column_name_length");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(column_name_str[field_index].length());
}

int spider_handlersocket_share::append_column_name(
  spider_string *str,
  uint field_index
) {
  int error_num;
  DBUG_ENTER("spider_handlersocket_share::append_column_name");
  DBUG_PRINT("info",("spider this=%p", this));
  error_num = spider_db_handlersocket_utility.append_name(str,
    column_name_str[field_index].ptr(), column_name_str[field_index].length());
  DBUG_RETURN(error_num);
}

int spider_handlersocket_share::append_column_name_with_alias(
  spider_string *str,
  uint field_index,
  const char *alias,
  uint alias_length
) {
  DBUG_ENTER("spider_handlersocket_share::append_column_name_with_alias");
  DBUG_PRINT("info",("spider this=%p", this));
  if (str->reserve(
    alias_length +
    column_name_str[field_index].length() +
    /* SPIDER_SQL_NAME_QUOTE_LEN */ 2))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  str->q_append(alias, alias_length);
  append_column_name(str, field_index);
  DBUG_RETURN(0);
}

bool spider_handlersocket_share::need_change_db_table_name()
{
  DBUG_ENTER("spider_handlersocket_share::need_change_db_table_name");
  DBUG_RETURN(!same_db_table_name);
}

#ifdef SPIDER_HAS_DISCOVER_TABLE_STRUCTURE
int spider_handlersocket_share::discover_table_structure(
  SPIDER_TRX *trx,
  SPIDER_SHARE *spider_share,
  spider_string *str
) {
  DBUG_ENTER("spider_handlersocket_share::discover_table_structure");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(HA_ERR_WRONG_COMMAND);
}
#endif

spider_handlersocket_handler::spider_handlersocket_handler(
  ha_spider *spider,
  spider_handlersocket_share *db_share
) : spider_db_handler(
  spider,
  db_share
),
  handlersocket_share(db_share),
  link_for_hash(NULL)
{
  DBUG_ENTER("spider_handlersocket_handler::spider_handlersocket_handler");
  DBUG_PRINT("info",("spider this=%p", this));
  spider_alloc_calc_mem_init(mem_calc, 187);
  spider_alloc_calc_mem(spider_current_trx, mem_calc, sizeof(*this));
  DBUG_VOID_RETURN;
}

spider_handlersocket_handler::~spider_handlersocket_handler()
{
  DBUG_ENTER("spider_handlersocket_handler::~spider_handlersocket_handler");
  DBUG_PRINT("info",("spider this=%p", this));
  if (link_for_hash)
  {
    spider_free(spider_current_trx, link_for_hash, MYF(0));
  }
  spider_free_mem_calc(spider_current_trx, mem_calc_id, sizeof(*this));
  DBUG_VOID_RETURN;
}

int spider_handlersocket_handler::init()
{
  st_spider_share *share = spider->share;
  TABLE *table = spider->get_table();
  DBUG_ENTER("spider_handlersocket_handler::init");
  DBUG_PRINT("info",("spider this=%p", this));
  if (!(link_for_hash = (SPIDER_LINK_FOR_HASH *)
    spider_bulk_alloc_mem(spider_current_trx, 204,
      __func__, __FILE__, __LINE__, MYF(MY_WME | MY_ZEROFILL),
      &link_for_hash,
        sizeof(SPIDER_LINK_FOR_HASH) * share->link_count,
      &minimum_select_bitmap,
        table ? sizeof(uchar) * no_bytes_in_map(table->read_set) : 0,
      NullS))
  ) {
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  uint roop_count;
  for (roop_count = 0; roop_count < share->link_count; roop_count++)
  {
    link_for_hash[roop_count].spider = spider;
    link_for_hash[roop_count].link_idx = roop_count;
    link_for_hash[roop_count].db_table_str =
      &handlersocket_share->db_table_str[roop_count];
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
    link_for_hash[roop_count].db_table_str_hash_value =
      handlersocket_share->db_table_str_hash_value[roop_count];
#endif
  }
  hs_sql.init_calc_mem(63);
  hs_sql.set_charset(share->access_charset);
  hs_keys.init();
  hs_upds.init();
  hs_strs.init();
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::append_index_hint(
  spider_string *str,
  int link_idx,
  ulong sql_type
  )
{
  DBUG_ENTER("spider_handlersocket_handler::append_index_hint");
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::append_table_name_with_adjusting(
  spider_string *str,
  int link_idx,
  ulong sql_type
) {
  DBUG_ENTER("spider_handlersocket_handler::append_table_name_with_adjusting");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::append_tmp_table_and_sql_for_bka(
  const key_range *start_key
) {
  DBUG_ENTER("spider_handlersocket_handler::append_tmp_table_and_sql_for_bka");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::reuse_tmp_table_and_sql_for_bka()
{
  DBUG_ENTER("spider_handlersocket_handler::reuse_tmp_table_and_sql_for_bka");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::append_union_table_and_sql_for_bka(
  const key_range *start_key
) {
  DBUG_ENTER("spider_handlersocket_handler::append_union_table_and_sql_for_bka");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::reuse_union_table_and_sql_for_bka()
{
  DBUG_ENTER("spider_handlersocket_handler::reuse_union_table_and_sql_for_bka");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::append_insert_for_recovery(
  ulong sql_type,
  int link_idx
) {
  DBUG_ENTER("spider_handlersocket_handler::append_insert_for_recovery");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::append_update(
  const TABLE *table,
  my_ptrdiff_t ptr_diff
) {
  DBUG_ENTER("spider_handlersocket_handler::append_update");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::append_update(
  const TABLE *table,
  my_ptrdiff_t ptr_diff,
  int link_idx
) {
  DBUG_ENTER("spider_handlersocket_handler::append_update");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::append_delete(
  const TABLE *table,
  my_ptrdiff_t ptr_diff
) {
  DBUG_ENTER("spider_handlersocket_handler::append_delete");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::append_delete(
  const TABLE *table,
  my_ptrdiff_t ptr_diff,
  int link_idx
) {
  DBUG_ENTER("spider_handlersocket_handler::append_delete");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::append_insert_part()
{
  DBUG_ENTER("spider_handlersocket_handler::append_insert_part");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::append_update_part()
{
  DBUG_ENTER("spider_handlersocket_handler::append_update_part");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::append_delete_part()
{
  DBUG_ENTER("spider_handlersocket_handler::append_delete_part");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS
int spider_handlersocket_handler::append_increment_update_set_part()
{
  DBUG_ENTER("spider_handlersocket_handler::append_increment_update_set_part");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}
#endif

int spider_handlersocket_handler::append_update_set_part()
{
  DBUG_ENTER("spider_handlersocket_handler::append_update_set_part");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS
int spider_handlersocket_handler::append_direct_update_set_part()
{
  SPIDER_SHARE *share = spider->share;
  DBUG_ENTER("spider_handlersocket_handler::append_direct_update_set_part");
  if (
    spider->do_direct_update &&
    (spider->direct_update_kinds & SPIDER_SQL_KIND_HS)
  ) {
    DBUG_PRINT("info",("spider add set for DU SPIDER_SQL_KIND_HS"));
    size_t roop_count;
    Field *field;
    hs_adding_keys = FALSE;
    for (roop_count = 0; roop_count < spider->hs_pushed_ret_fields_num;
      roop_count++)
    {
      Field *top_table_field =
        spider->get_top_table_field(spider->hs_pushed_ret_fields[roop_count]);
      if (!(field = spider->field_exchange(top_table_field)))
        continue;
      if (top_table_field->is_null())
      {
        hs_upds.push_back(spider_null_string_ref);
      } else {
        if (spider_db_handlersocket_utility.
          append_column_value(spider, NULL, top_table_field, NULL,
            share->access_charset))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      }
    }
  }
  DBUG_RETURN(0);
}
#endif

int spider_handlersocket_handler::append_minimum_select_without_quote(
  spider_string *str
) {
  TABLE *table = spider->get_table();
  Field **field;
  int field_length;
  bool appended = FALSE;
  DBUG_ENTER("spider_handlersocket_handler::append_minimum_select_without_quote");
  minimum_select_bitmap_create();
  for (field = table->field; *field; field++)
  {
    if (minimum_select_bit_is_set((*field)->field_index))
    {
/*
      spider_set_bit(minimum_select_bitmap, (*field)->field_index);
*/
      field_length =
        handlersocket_share->column_name_str[(*field)->field_index].length();
      if (str->reserve(field_length + SPIDER_SQL_COMMA_LEN))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      str->q_append(
        handlersocket_share->column_name_str[(*field)->field_index].ptr(),
        handlersocket_share->column_name_str[(*field)->field_index].length());
      str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
      appended = TRUE;
    }
  }
  if (appended)
    str->length(str->length() - SPIDER_SQL_COMMA_LEN);
  DBUG_RETURN(0);
}

#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS
int spider_handlersocket_handler::append_minimum_select_by_field_idx_list(
  spider_string *str,
  uint32 *field_idxs,
  size_t field_idxs_num
) {
  Field *field;
  int roop_count, field_length;
  bool appended = FALSE;
  DBUG_ENTER("spider_handlersocket_handler::append_minimum_select_by_field_idx_list");
  for (roop_count = 0; roop_count < (int) field_idxs_num; roop_count++)
  {
    field = spider->get_top_table_field(field_idxs[roop_count]);
    if ((field = spider->field_exchange(field)))
    {
      field_length =
        handlersocket_share->column_name_str[field->field_index].length();
      if (str->reserve(field_length + SPIDER_SQL_COMMA_LEN))
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      str->q_append(
        handlersocket_share->column_name_str[field->field_index].ptr(),
        handlersocket_share->column_name_str[field->field_index].length());
      str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
      appended = TRUE;
    }
  }
  if (appended)
    str->length(str->length() - SPIDER_SQL_COMMA_LEN);
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::append_dup_update_pushdown_part(
  const char *alias,
  uint alias_length
) {
  DBUG_ENTER("spider_handlersocket_handler::append_dup_update_pushdown_part");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::append_update_columns_part(
  const char *alias,
  uint alias_length
) {
  DBUG_ENTER("spider_handlersocket_handler::append_update_columns_part");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::check_update_columns_part()
{
  DBUG_ENTER("spider_handlersocket_handler::check_update_columns_part");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}
#endif

int spider_handlersocket_handler::append_select_part(
  ulong sql_type
) {
  DBUG_ENTER("spider_handlersocket_handler::append_select_part");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::append_table_select_part(
  ulong sql_type
) {
  DBUG_ENTER("spider_handlersocket_handler::append_table_select_part");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::append_key_select_part(
  ulong sql_type,
  uint idx
) {
  DBUG_ENTER("spider_handlersocket_handler::append_key_select_part");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::append_minimum_select_part(
  ulong sql_type
) {
  DBUG_ENTER("spider_handlersocket_handler::append_minimum_select_part");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::append_hint_after_table_part(
  ulong sql_type
) {
  DBUG_ENTER("spider_handlersocket_handler::append_hint_after_table_part");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

void spider_handlersocket_handler::set_where_pos(
  ulong sql_type
) {
  DBUG_ENTER("spider_handlersocket_handler::set_where_pos");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_VOID_RETURN;
}

void spider_handlersocket_handler::set_where_to_pos(
  ulong sql_type
) {
  DBUG_ENTER("spider_handlersocket_handler::set_where_to_pos");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_VOID_RETURN;
}

int spider_handlersocket_handler::check_item_type(
  Item *item
) {
  DBUG_ENTER("spider_handlersocket_handler::check_item_type");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::append_values_connector_part(
  ulong sql_type
) {
  DBUG_ENTER("spider_handlersocket_handler::append_values_connector_part");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::append_values_terminator_part(
  ulong sql_type
) {
  DBUG_ENTER("spider_handlersocket_handler::append_values_terminator_part");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::append_union_table_connector_part(
  ulong sql_type
) {
  DBUG_ENTER("spider_handlersocket_handler::append_union_table_connector_part");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::append_union_table_terminator_part(
  ulong sql_type
) {
  DBUG_ENTER("spider_handlersocket_handler::append_union_table_terminator_part");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::append_key_column_values_part(
  const key_range *start_key,
  ulong sql_type
) {
  DBUG_ENTER("spider_handlersocket_handler::append_key_column_values_part");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::append_key_column_values_with_name_part(
  const key_range *start_key,
  ulong sql_type
) {
  DBUG_ENTER("spider_handlersocket_handler::append_key_column_values_with_name_part");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::append_key_where_part(
  const key_range *start_key,
  const key_range *end_key,
  ulong sql_type
) {
  int error_num;
  spider_string *str;
  bool set_order;
  DBUG_ENTER("spider_handlersocket_handler::append_key_where_part");
  switch (sql_type)
  {
    case SPIDER_SQL_TYPE_SELECT_HS:
    case SPIDER_SQL_TYPE_INSERT_HS:
    case SPIDER_SQL_TYPE_UPDATE_HS:
    case SPIDER_SQL_TYPE_DELETE_HS:
      str = &hs_sql;
      str->length(0);
      hs_adding_keys = TRUE;
      set_order = FALSE;
      break;
    default:
      DBUG_RETURN(0);
  }
  error_num = append_key_where(str, NULL, NULL, start_key, end_key,
    sql_type, set_order);
  DBUG_RETURN(error_num);
}

int spider_handlersocket_handler::append_key_where(
  spider_string *str,
  spider_string *str_part,
  spider_string *str_part2,
  const key_range *start_key,
  const key_range *end_key,
  ulong sql_type,
  bool set_order
) {
  int error_num;
  DBUG_ENTER("spider_handlersocket_handler::append_key_where");
  error_num = spider_db_append_key_where_internal(str, str_part, str_part2,
    start_key, end_key, spider, set_order, sql_type,
    spider_dbton_handlersocket.dbton_id);
  DBUG_RETURN(error_num);
}

int spider_handlersocket_handler::append_is_null_part(
  ulong sql_type,
  KEY_PART_INFO *key_part,
  const key_range *key,
  const uchar **ptr,
  bool key_eq,
  bool tgt_final
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_handlersocket_handler::append_is_null_part");
  DBUG_PRINT("info",("spider this=%p", this));
  switch (sql_type)
  {
    case SPIDER_SQL_TYPE_SELECT_HS:
    case SPIDER_SQL_TYPE_INSERT_HS:
    case SPIDER_SQL_TYPE_UPDATE_HS:
    case SPIDER_SQL_TYPE_DELETE_HS:
      str = &hs_sql;
      break;
    default:
      DBUG_RETURN(0);
  }
  error_num = append_is_null(sql_type, str, NULL, NULL, key_part, key, ptr,
    key_eq, tgt_final);
  DBUG_RETURN(error_num);
}

int spider_handlersocket_handler::append_is_null(
  ulong sql_type,
  spider_string *str,
  spider_string *str_part,
  spider_string *str_part2,
  KEY_PART_INFO *key_part,
  const key_range *key,
  const uchar **ptr,
  bool key_eq,
  bool tgt_final
) {
  DBUG_ENTER("spider_handlersocket_handler::append_is_null");
  DBUG_PRINT("info",("spider this=%p", this));
  if (key_part->null_bit)
  {
    if (*(*ptr)++)
    {
      hs_keys.push_back(spider_null_string_ref);
      DBUG_RETURN(-1);
    }
  }
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::append_where_terminator_part(
  ulong sql_type,
  bool set_order,
  int key_count
) {
  DBUG_ENTER("spider_handlersocket_handler::append_where_terminator_part");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::append_match_where_part(
  ulong sql_type
) {
  DBUG_ENTER("spider_handlersocket_handler::append_match_where_part");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::append_condition_part(
  const char *alias,
  uint alias_length,
  ulong sql_type,
  bool test_flg
) {
  DBUG_ENTER("spider_handlersocket_handler::append_condition_part");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::append_match_select_part(
  ulong sql_type,
  const char *alias,
  uint alias_length
) {
  DBUG_ENTER("spider_handlersocket_handler::append_match_select_part");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

#ifdef HANDLER_HAS_DIRECT_AGGREGATE
int spider_handlersocket_handler::append_sum_select_part(
  ulong sql_type,
  const char *alias,
  uint alias_length
) {
  DBUG_ENTER("spider_handlersocket_handler::append_sum_select_part");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}
#endif

void spider_handlersocket_handler::set_order_pos(
  ulong sql_type
) {
  DBUG_ENTER("spider_handlersocket_handler::set_order_pos");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_VOID_RETURN;
}

void spider_handlersocket_handler::set_order_to_pos(
  ulong sql_type
) {
  DBUG_ENTER("spider_handlersocket_handler::set_order_to_pos");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_VOID_RETURN;
}

#ifdef HANDLER_HAS_DIRECT_AGGREGATE
int spider_handlersocket_handler::append_group_by_part(
  const char *alias,
  uint alias_length,
  ulong sql_type
) {
  DBUG_ENTER("spider_handlersocket_handler::append_group_by_part");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}
#endif

int spider_handlersocket_handler::append_key_order_for_merge_with_alias_part(
  const char *alias,
  uint alias_length,
  ulong sql_type
) {
  DBUG_ENTER("spider_handlersocket_handler::append_key_order_for_merge_with_alias_part");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::append_key_order_for_direct_order_limit_with_alias_part(
  const char *alias,
  uint alias_length,
  ulong sql_type
) {
  DBUG_ENTER("spider_handlersocket_handler::append_key_order_for_direct_order_limit_with_alias_part");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::append_key_order_with_alias_part(
  const char *alias,
  uint alias_length,
  ulong sql_type
) {
  DBUG_ENTER("spider_handlersocket_handler::append_key_order_with_alias_part");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::append_limit_part(
  longlong offset,
  longlong limit,
  ulong sql_type
) {
  DBUG_ENTER("spider_handlersocket_handler::append_limit_part");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_PRINT("info", ("spider offset=%lld", offset));
  DBUG_PRINT("info", ("spider limit=%lld", limit));
  hs_skip = (int) offset;
  hs_limit = (int) limit;
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::reappend_limit_part(
  longlong offset,
  longlong limit,
  ulong sql_type
) {
  DBUG_ENTER("spider_handlersocket_handler::reappend_limit_part");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::append_select_lock_part(
  ulong sql_type
) {
  DBUG_ENTER("spider_handlersocket_handler::append_select_lock_part");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::append_union_all_start_part(
  ulong sql_type
) {
  DBUG_ENTER("spider_handlersocket_handler::append_union_all_start_part");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::append_union_all_part(
  ulong sql_type
) {
  DBUG_ENTER("spider_handlersocket_handler::append_union_all_part");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::append_union_all_end_part(
  ulong sql_type
) {
  DBUG_ENTER("spider_handlersocket_handler::append_union_all_end_part");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::append_multi_range_cnt_part(
  ulong sql_type,
  uint multi_range_cnt,
  bool with_comma
) {
  DBUG_ENTER("spider_handlersocket_handler::append_multi_range_cnt_part");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::append_multi_range_cnt_with_name_part(
  ulong sql_type,
  uint multi_range_cnt
) {
  DBUG_ENTER("spider_handlersocket_handler::append_multi_range_cnt_with_name_part");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::append_open_handler_part(
  ulong sql_type,
  uint handler_id,
  SPIDER_CONN *conn,
  int link_idx
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_handlersocket_handler::append_open_handler_part");
  DBUG_PRINT("info",("spider this=%p", this));
  switch (sql_type)
  {
    case SPIDER_SQL_TYPE_OTHER_HS:
      str = &hs_sql;
      break;
    default:
      DBUG_RETURN(0);
  }
  error_num = append_open_handler(str, handler_id, conn, link_idx);
  DBUG_RETURN(error_num);
}

int spider_handlersocket_handler::append_open_handler(
  spider_string *str,
  uint handler_id,
  SPIDER_CONN *conn,
  int link_idx
) {
  int error_num;
  DBUG_ENTER("spider_handlersocket_handler::append_open_handler");
  DBUG_PRINT("info",("spider this=%p", this));
  if (
    str->length() == 0 &&
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS
    (
      (
        (
          spider->sql_command == SQLCOM_HS_INSERT ||
          spider->hs_pushed_ret_fields_num == MAX_FIELDS
        ) &&
#endif
        (error_num = append_minimum_select_without_quote(str))
#ifdef HANDLER_HAS_DIRECT_UPDATE_ROWS
      ) ||
      (
        (
          spider->sql_command != SQLCOM_HS_INSERT &&
          spider->hs_pushed_ret_fields_num < MAX_FIELDS
        ) &&
        (error_num = append_minimum_select_by_field_idx_list(str,
          spider->hs_pushed_ret_fields, spider->hs_pushed_ret_fields_num))
      )
    )
#endif
  ) {
    DBUG_RETURN(error_num);
  }

  TABLE *table = spider->get_table();
  SPIDER_SHARE *share = spider->share;
  DBUG_PRINT("info",("spider field list=%s", str->c_ptr_safe()));
  if (!spider_bit_is_set(spider->db_request_phase, link_idx))
  {
    spider_set_bit(spider->db_request_phase, link_idx);
    ++spider->db_request_id[link_idx];
  }
  st_spider_db_request_key request_key;
  request_key.spider_thread_id = spider->trx->spider_thread_id;
  request_key.query_id = spider->trx->thd->query_id;
  request_key.handler = spider;
  request_key.request_id = spider->db_request_id[link_idx];
  request_key.next = NULL;
  conn->db_conn->append_open_handler(
    handler_id,
    share->tgt_dbs[spider->conn_link_idx[link_idx]],
    share->tgt_table_names[spider->conn_link_idx[link_idx]],
    spider->active_index < MAX_KEY ?
      table->key_info[spider->active_index].name :
      "0",
    str->c_ptr_safe(),
    &request_key
  );
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::append_close_handler_part(
  ulong sql_type,
  int link_idx
) {
  DBUG_ENTER("spider_handlersocket_handler::append_close_handler_part");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::append_insert_terminator_part(
  ulong sql_type
) {
  DBUG_ENTER("spider_handlersocket_handler::append_insert_terminator_part");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::append_insert_values_part(
  ulong sql_type
) {
  int error_num;
  spider_string *str;
  DBUG_ENTER("spider_mysql_handler::append_insert_values_part");
  DBUG_PRINT("info",("spider this=%p", this));
  switch (sql_type)
  {
    case SPIDER_SQL_TYPE_INSERT_HS:
      str = &hs_sql;
      break;
    default:
      DBUG_RETURN(0);
  }
  error_num = append_insert_values(str);
  DBUG_RETURN(error_num);
}

int spider_handlersocket_handler::append_insert_values(
  spider_string *str
) {
  SPIDER_SHARE *share = spider->share;
  TABLE *table = spider->get_table();
  Field **field;
  DBUG_ENTER("spider_mysql_handler::append_insert_values");
  DBUG_PRINT("info",("spider this=%p", this));
  hs_adding_keys = FALSE;
  for (field = table->field; *field; field++)
  {
    DBUG_PRINT("info",("spider field_index=%u", (*field)->field_index));
    if (
      bitmap_is_set(table->write_set, (*field)->field_index) ||
      bitmap_is_set(table->read_set, (*field)->field_index)
    ) {
#ifndef DBUG_OFF
      my_bitmap_map *tmp_map =
        dbug_tmp_use_all_columns(table, table->read_set);
#endif
      DBUG_PRINT("info",("spider is_null()=%s",
        (*field)->is_null() ? "TRUE" : "FALSE"));
      DBUG_PRINT("info",("spider table->next_number_field=%p",
        table->next_number_field));
      DBUG_PRINT("info",("spider *field=%p", *field));
      DBUG_PRINT("info",("spider force_auto_increment=%s",
        (table->next_number_field && spider->force_auto_increment) ?
        "TRUE" : "FALSE"));
      if (
        (*field)->is_null() ||
        (
          table->next_number_field == *field &&
          !table->auto_increment_field_not_null &&
          !spider->force_auto_increment
        )
      ) {
        hs_upds.push_back(spider_null_string_ref);
      } else {
        spider_db_handlersocket_utility.
          append_column_value(spider, NULL, *field, NULL,
            share->access_charset);
      }
#ifndef DBUG_OFF
      dbug_tmp_restore_column_map(table->read_set, tmp_map);
#endif
    }
  }
  int error_num;
  int roop_count2;
  for (
    roop_count2 = spider_conn_link_idx_next(share->link_statuses,
      spider->conn_link_idx, -1, share->link_count,
      SPIDER_LINK_STATUS_RECOVERY);
    roop_count2 < (int) share->link_count;
    roop_count2 = spider_conn_link_idx_next(share->link_statuses,
      spider->conn_link_idx, roop_count2, share->link_count,
      SPIDER_LINK_STATUS_RECOVERY)
  ) {
    if (spider->sql_kind[roop_count2] == SPIDER_SQL_KIND_HS)
    {
      SPIDER_CONN *conn = spider->hs_w_conns[roop_count2];
      if (conn->dbton_id == spider_dbton_handlersocket.dbton_id)
      {
        if ((error_num = request_buf_insert(roop_count2)))
          DBUG_RETURN(error_num);
#ifdef HA_CAN_BULK_ACCESS
        if (spider->is_bulk_access_clone)
        {
          spider->connection_ids[roop_count2] = conn->connection_id;
          spider_trx_add_bulk_access_conn(spider->trx, conn);
        }
#endif
      }
    }
  }
  hs_upds.clear();
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::append_into_part(
  ulong sql_type
) {
  DBUG_ENTER("spider_handlersocket_handler::append_into_part");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

void spider_handlersocket_handler::set_insert_to_pos(
  ulong sql_type
) {
  DBUG_ENTER("spider_handlersocket_handler::set_insert_to_pos");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_VOID_RETURN;
}

int spider_handlersocket_handler::append_from_part(
  ulong sql_type,
  int link_idx
) {
  DBUG_ENTER("spider_handlersocket_handler::append_from_part");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::append_delete_all_rows_part(
  ulong sql_type
) {
  DBUG_ENTER("spider_handlersocket_handler::append_delete_all_rows_part");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::append_explain_select_part(
  const key_range *start_key,
  const key_range *end_key,
  ulong sql_type,
  int link_idx
) {
  DBUG_ENTER("spider_handlersocket_handler::append_explain_select_part");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::is_sole_projection_field(
  uint16 field_index
) {
  DBUG_ENTER("spider_handlersocket_handler::is_sole_projection_field");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

bool spider_handlersocket_handler::is_bulk_insert_exec_period(
  bool bulk_end
) {
  DBUG_ENTER("spider_handlersocket_handler::is_bulk_insert_exec_period");
  DBUG_PRINT("info",("spider this=%p", this));
  if (!spider->bulk_insert || bulk_end)
    DBUG_RETURN(TRUE);
  DBUG_RETURN(FALSE);
}

bool spider_handlersocket_handler::sql_is_filled_up(
  ulong sql_type
) {
  DBUG_ENTER("spider_handlersocket_handler::sql_is_filled_up");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(FALSE);
}

bool spider_handlersocket_handler::sql_is_empty(
  ulong sql_type
) {
  DBUG_ENTER("spider_handlersocket_handler::sql_is_empty");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(FALSE);
}

bool spider_handlersocket_handler::support_multi_split_read()
{
  DBUG_ENTER("spider_handlersocket_handler::support_multi_split_read");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(FALSE);
}

bool spider_handlersocket_handler::support_bulk_update()
{
  DBUG_ENTER("spider_handlersocket_handler::support_bulk_update");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(FALSE);
}

int spider_handlersocket_handler::bulk_tmp_table_insert()
{
  DBUG_ENTER("spider_handlersocket_handler::bulk_tmp_table_insert");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::bulk_tmp_table_insert(
  int link_idx
) {
  DBUG_ENTER("spider_handlersocket_handler::bulk_tmp_table_insert");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::bulk_tmp_table_end_bulk_insert()
{
  DBUG_ENTER("spider_handlersocket_handler::bulk_tmp_table_end_bulk_insert");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::bulk_tmp_table_rnd_init()
{
  DBUG_ENTER("spider_handlersocket_handler::bulk_tmp_table_rnd_init");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::bulk_tmp_table_rnd_next()
{
  DBUG_ENTER("spider_handlersocket_handler::bulk_tmp_table_rnd_next");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::bulk_tmp_table_rnd_end()
{
  DBUG_ENTER("spider_handlersocket_handler::bulk_tmp_table_rnd_end");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

bool spider_handlersocket_handler::need_copy_for_update(
    int link_idx
) {
  DBUG_ENTER("spider_handlersocket_handler::need_copy_for_update");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(TRUE);
}

bool spider_handlersocket_handler::bulk_tmp_table_created()
{
  DBUG_ENTER("spider_handlersocket_handler::bulk_tmp_table_created");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(FALSE);
}

int spider_handlersocket_handler::mk_bulk_tmp_table_and_bulk_start()
{
  DBUG_ENTER("spider_handlersocket_handler::mk_bulk_tmp_table_and_bulk_start");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

void spider_handlersocket_handler::rm_bulk_tmp_table()
{
  DBUG_ENTER("spider_handlersocket_handler::rm_bulk_tmp_table");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_VOID_RETURN;
}

int spider_handlersocket_handler::insert_lock_tables_list(
  SPIDER_CONN *conn,
  int link_idx
) {
  DBUG_ENTER("spider_handlersocket_handler::insert_lock_tables_list");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::append_lock_tables_list(
  SPIDER_CONN *conn,
  int link_idx,
  int *appended
) {
  DBUG_ENTER("spider_handlersocket_handler::append_lock_tables_list");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::realloc_sql(
  ulong *realloced
) {
  THD *thd = spider->trx->thd;
  st_spider_share *share = spider->share;
  int init_sql_alloc_size =
    spider_param_init_sql_alloc_size(thd, share->init_sql_alloc_size);
  DBUG_ENTER("spider_handlersocket_handler::realloc_sql");
  DBUG_PRINT("info",("spider this=%p", this));
  if ((int) hs_sql.alloced_length() > init_sql_alloc_size * 2)
  {
    hs_sql.free();
    if (hs_sql.real_alloc(init_sql_alloc_size))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    *realloced |= SPIDER_SQL_TYPE_FULL_HS;
  }
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::reset_sql(
  ulong sql_type
) {
  DBUG_ENTER("spider_handlersocket_handler::reset_sql");
  DBUG_PRINT("info",("spider this=%p", this));
  if (sql_type & SPIDER_SQL_TYPE_FULL_HS)
  {
    hs_sql.length(0);
  }
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::reset_keys(
  ulong sql_type
) {
  DBUG_ENTER("spider_handlersocket_handler::reset_keys");
  DBUG_PRINT("info",("spider this=%p", this));
  if (sql_type & SPIDER_SQL_TYPE_FULL_HS)
  {
    hs_keys.clear();
  }
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::reset_upds(
  ulong sql_type
) {
  DBUG_ENTER("spider_handlersocket_handler::reset_upds");
  DBUG_PRINT("info",("spider this=%p", this));
  if (sql_type & SPIDER_SQL_TYPE_FULL_HS)
  {
    hs_upds.clear();
  }
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::reset_strs(
  ulong sql_type
) {
  DBUG_ENTER("spider_handlersocket_handler::reset_strs");
  DBUG_PRINT("info",("spider this=%p", this));
  if (sql_type & SPIDER_SQL_TYPE_FULL_HS)
  {
    hs_strs.clear();
  }
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::reset_strs_pos(
  ulong sql_type
) {
  DBUG_ENTER("spider_handlersocket_handler::reset_strs_pos");
  DBUG_PRINT("info",("spider this=%p", this));
  if (sql_type & SPIDER_SQL_TYPE_FULL_HS)
  {
    hs_strs_pos = 0;
  }
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::push_back_upds(
  SPIDER_HS_STRING_REF &info
) {
  int error_num;
  DBUG_ENTER("spider_handlersocket_handler::push_back_upds");
  DBUG_PRINT("info",("spider this=%p", this));
  error_num = hs_upds.push_back(info);
  DBUG_RETURN(error_num);
}

int spider_handlersocket_handler::request_buf_find(
  int link_idx
) {
  int error_num;
  spider_string *hs_str;
  SPIDER_CONN *conn;
  uint handler_id;
  DBUG_ENTER("spider_handlersocket_handler::request_buf_find");
  DBUG_PRINT("info",("spider this=%p", this));
  if (!(hs_str = hs_strs.add(&hs_strs_pos, hs_sql.ptr(), hs_sql.length())))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  if (spider->conn_kind[link_idx] == SPIDER_CONN_KIND_HS_READ)
  {
    conn = spider->hs_r_conns[link_idx];
    handler_id = spider->r_handler_id[link_idx];
  } else {
    conn = spider->hs_w_conns[link_idx];
    handler_id = spider->w_handler_id[link_idx];
  }
  if ((error_num = spider_db_conn_queue_action(conn)))
    DBUG_RETURN(error_num);
  if (!spider_bit_is_set(spider->db_request_phase, link_idx))
  {
    spider_set_bit(spider->db_request_phase, link_idx);
    ++spider->db_request_id[link_idx];
  }
  st_spider_db_request_key request_key;
  request_key.spider_thread_id = spider->trx->spider_thread_id;
  request_key.query_id = spider->trx->thd->query_id;
  request_key.handler = spider;
  request_key.request_id = spider->db_request_id[link_idx];
  request_key.next = NULL;
  conn->db_conn->append_select(
    handler_id, hs_str, &hs_keys,
    hs_limit, hs_skip, &request_key);
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::request_buf_insert(
  int link_idx
) {
  int error_num;
  DBUG_ENTER("spider_handlersocket_handler::request_buf_insert");
  DBUG_PRINT("info",("spider this=%p", this));
  if ((error_num = spider_db_conn_queue_action(spider->hs_w_conns[link_idx])))
    DBUG_RETURN(error_num);
  if (!spider_bit_is_set(spider->db_request_phase, link_idx))
  {
    spider_set_bit(spider->db_request_phase, link_idx);
    ++spider->db_request_id[link_idx];
  }
  st_spider_db_request_key request_key;
  request_key.spider_thread_id = spider->trx->spider_thread_id;
  request_key.query_id = spider->trx->thd->query_id;
  request_key.handler = spider;
  request_key.request_id = spider->db_request_id[link_idx];
  request_key.next = NULL;
  spider->hs_w_conns[link_idx]->db_conn->append_insert(
    spider->w_handler_id[link_idx], &hs_upds, &request_key);
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::request_buf_update(
  int link_idx
) {
  int error_num;
  spider_string *hs_str;
  DBUG_ENTER("spider_handlersocket_handler::request_buf_update");
  DBUG_PRINT("info",("spider this=%p", this));
  if (!(hs_str = hs_strs.add(&hs_strs_pos, hs_sql.ptr(), hs_sql.length())))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  if ((error_num = spider_db_conn_queue_action(spider->hs_w_conns[link_idx])))
    DBUG_RETURN(error_num);
  if (!spider_bit_is_set(spider->db_request_phase, link_idx))
  {
    spider_set_bit(spider->db_request_phase, link_idx);
    ++spider->db_request_id[link_idx];
  }
  st_spider_db_request_key request_key;
  request_key.spider_thread_id = spider->trx->spider_thread_id;
  request_key.query_id = spider->trx->thd->query_id;
  request_key.handler = spider;
  request_key.request_id = spider->db_request_id[link_idx];
  request_key.next = NULL;
  spider->hs_w_conns[link_idx]->db_conn->append_update(
    spider->w_handler_id[link_idx], hs_str, &hs_keys, &hs_upds,
    hs_limit, hs_skip,
    spider->hs_increment, spider->hs_decrement, &request_key
  );
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::request_buf_delete(
  int link_idx
) {
  int error_num;
  spider_string *hs_str;
  DBUG_ENTER("spider_handlersocket_handler::request_buf_delete");
  DBUG_PRINT("info",("spider this=%p", this));
  if (!(hs_str = hs_strs.add(&hs_strs_pos, hs_sql.ptr(), hs_sql.length())))
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  if ((error_num = spider_db_conn_queue_action(spider->hs_w_conns[link_idx])))
    DBUG_RETURN(error_num);
  if (!spider_bit_is_set(spider->db_request_phase, link_idx))
  {
    spider_set_bit(spider->db_request_phase, link_idx);
    ++spider->db_request_id[link_idx];
  }
  st_spider_db_request_key request_key;
  request_key.spider_thread_id = spider->trx->spider_thread_id;
  request_key.query_id = spider->trx->thd->query_id;
  request_key.handler = spider;
  request_key.request_id = spider->db_request_id[link_idx];
  request_key.next = NULL;
  spider->hs_w_conns[link_idx]->db_conn->append_delete(
    spider->w_handler_id[link_idx], hs_str, &hs_keys,
    hs_limit, hs_skip, &request_key);
  DBUG_RETURN(0);
}

bool spider_handlersocket_handler::need_lock_before_set_sql_for_exec(
  ulong sql_type
) {
  DBUG_ENTER("spider_handlersocket_handler::need_lock_before_set_sql_for_exec");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(TRUE);
}

#ifdef SPIDER_HAS_GROUP_BY_HANDLER
int spider_handlersocket_handler::set_sql_for_exec(
  ulong sql_type,
  int link_idx,
  SPIDER_LINK_IDX_CHAIN *link_idx_chain
) {
  DBUG_ENTER("spider_handlersocket_handler::set_sql_for_exec");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}
#endif

int spider_handlersocket_handler::set_sql_for_exec(
  ulong sql_type,
  int link_idx
) {
  DBUG_ENTER("spider_handlersocket_handler::set_sql_for_exec");
  DBUG_PRINT("info",("spider this=%p", this));
  if (sql_type & SPIDER_SQL_TYPE_SELECT_HS)
  {
    DBUG_RETURN(request_buf_find(link_idx));
  }
  if (sql_type & SPIDER_SQL_TYPE_INSERT_HS)
  {
    DBUG_RETURN(request_buf_insert(link_idx));
  }
  if (sql_type & SPIDER_SQL_TYPE_UPDATE_HS)
  {
    DBUG_RETURN(request_buf_update(link_idx));
  }
  if (sql_type & SPIDER_SQL_TYPE_DELETE_HS)
  {
    DBUG_RETURN(request_buf_delete(link_idx));
  }
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::set_sql_for_exec(
  spider_db_copy_table *tgt_ct,
  ulong sql_type
) {
  DBUG_ENTER("spider_handlersocket_handler::set_sql_for_exec");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::execute_sql(
  ulong sql_type,
  SPIDER_CONN *conn,
  int quick_mode,
  int *need_mon
) {
  DBUG_ENTER("spider_handlersocket_handler::execute_sql");
  DBUG_PRINT("info",("spider this=%p", this));
  if (!(sql_type & SPIDER_SQL_TYPE_FULL_HS))
  {
    /* nothing to do */
    DBUG_RETURN(0);
  }
  DBUG_RETURN(spider_db_query(
    conn,
    NULL,
    0,
    quick_mode,
    need_mon
  ));
}

int spider_handlersocket_handler::reset()
{
  DBUG_ENTER("spider_handlersocket_handler::reset");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::sts_mode_exchange(
  int sts_mode
) {
  DBUG_ENTER("spider_handlersocket_handler::sts_mode_exchange");
  DBUG_PRINT("info",("spider sts_mode=%d", sts_mode));
  DBUG_RETURN(sts_mode);
}

int spider_handlersocket_handler::show_table_status(
  int link_idx,
  int sts_mode,
  uint flag
) {
  spider_db_handlersocket_result res(NULL);
  SPIDER_SHARE *share = spider->share;
  ulonglong auto_increment_value = 0;
  DBUG_ENTER("spider_handlersocket_show_table_status");
  res.fetch_table_status(
    sts_mode,
    share->stat
  );
  if (auto_increment_value > share->lgtm_tblhnd_share->auto_increment_value)
  {
    share->lgtm_tblhnd_share->auto_increment_value = auto_increment_value;
    DBUG_PRINT("info",("spider auto_increment_value=%llu",
      share->lgtm_tblhnd_share->auto_increment_value));
  }
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::crd_mode_exchange(
  int crd_mode
) {
  DBUG_ENTER("spider_handlersocket_handler::crd_mode_exchange");
  DBUG_PRINT("info",("spider crd_mode=%d", crd_mode));
  DBUG_RETURN(crd_mode);
}

int spider_handlersocket_handler::show_index(
  int link_idx,
  int crd_mode
) {
  DBUG_ENTER("spider_handlersocket_handler::show_index");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::show_records(
  int link_idx
) {
  DBUG_ENTER("spider_handlersocket_handler::show_records");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::show_last_insert_id(
  int link_idx,
  ulonglong &last_insert_id
) {
  DBUG_ENTER("spider_handlersocket_handler::show_last_insert_id");
  last_insert_id = 0;
  DBUG_RETURN(0);
}

ha_rows spider_handlersocket_handler::explain_select(
  key_range *start_key,
  key_range *end_key,
  int link_idx
) {
  DBUG_ENTER("spider_handlersocket_handler::explain_select");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::lock_tables(
  int link_idx
) {
  DBUG_ENTER("spider_handlersocket_handler::lock_tables");
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::unlock_tables(
  int link_idx
) {
  DBUG_ENTER("spider_handlersocket_handler::unlock_tables");
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::disable_keys(
  SPIDER_CONN *conn,
  int link_idx
) {
  DBUG_ENTER("spider_handlersocket_handler::disable_keys");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::enable_keys(
  SPIDER_CONN *conn,
  int link_idx
) {
  DBUG_ENTER("spider_handlersocket_handler::enable_keys");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::check_table(
  SPIDER_CONN *conn,
  int link_idx,
  HA_CHECK_OPT* check_opt
) {
  DBUG_ENTER("spider_handlersocket_handler::check_table");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::repair_table(
  SPIDER_CONN *conn,
  int link_idx,
  HA_CHECK_OPT* check_opt
) {
  DBUG_ENTER("spider_handlersocket_handler::repair_table");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::analyze_table(
  SPIDER_CONN *conn,
  int link_idx
) {
  DBUG_ENTER("spider_handlersocket_handler::analyze_table");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::optimize_table(
  SPIDER_CONN *conn,
  int link_idx
) {
  DBUG_ENTER("spider_handlersocket_handler::optimize_table");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::flush_tables(
  SPIDER_CONN *conn,
  int link_idx,
  bool lock
) {
  DBUG_ENTER("spider_handlersocket_handler::flush_tables");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::flush_logs(
  SPIDER_CONN *conn,
  int link_idx
) {
  DBUG_ENTER("spider_handlersocket_handler::flush_logs");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::insert_opened_handler(
  SPIDER_CONN *conn,
  int link_idx
) {
  spider_db_handlersocket *db_conn = (spider_db_handlersocket *) conn->db_conn;
  SPIDER_LINK_FOR_HASH *tmp_link_for_hash = &link_for_hash[link_idx];
  DBUG_ASSERT(tmp_link_for_hash->spider == spider);
  DBUG_ASSERT(tmp_link_for_hash->link_idx == link_idx);
  uint old_elements = db_conn->handler_open_array.max_element;
  DBUG_ENTER("spider_handlersocket_handler::insert_opened_handler");
  DBUG_PRINT("info",("spider this=%p", this));
  if (insert_dynamic(&db_conn->handler_open_array,
    (uchar*) &tmp_link_for_hash))
  {
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  if (db_conn->handler_open_array.max_element > old_elements)
  {
    spider_alloc_calc_mem(spider_current_trx,
      db_conn->handler_open_array,
      (db_conn->handler_open_array.max_element - old_elements) *
      db_conn->handler_open_array.size_of_element);
  }
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::delete_opened_handler(
  SPIDER_CONN *conn,
  int link_idx
) {
  spider_db_handlersocket *db_conn = (spider_db_handlersocket *) conn->db_conn;
  uint roop_count, elements = db_conn->handler_open_array.elements;
  SPIDER_LINK_FOR_HASH *tmp_link_for_hash;
  DBUG_ENTER("spider_handlersocket_handler::delete_opened_handler");
  DBUG_PRINT("info",("spider this=%p", this));
  for (roop_count = 0; roop_count < elements; roop_count++)
  {
    get_dynamic(&db_conn->handler_open_array, (uchar *) &tmp_link_for_hash,
      roop_count);
    if (tmp_link_for_hash == &link_for_hash[link_idx])
    {
      delete_dynamic_element(&db_conn->handler_open_array, roop_count);
      break;
    }
  }
  DBUG_ASSERT(roop_count < elements);
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::sync_from_clone_source(
  spider_db_handler *dbton_hdl
) {
  spider_handlersocket_handler *hs_hdl =
    (spider_handlersocket_handler *) dbton_hdl;
  DBUG_ENTER("spider_handlersocket_handler::sync_from_clone_source");
  DBUG_PRINT("info",("spider this=%p", this));
  hs_strs_pos = hs_hdl->hs_strs_pos;
  DBUG_RETURN(0);
}

bool spider_handlersocket_handler::support_use_handler(
  int use_handler
) {
  DBUG_ENTER("spider_handlersocket_handler::support_use_handler");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(TRUE);
}

void spider_handlersocket_handler::minimum_select_bitmap_create()
{
  TABLE *table = spider->get_table();
  Field **field_p;
  DBUG_ENTER("spider_handlersocket_handler::minimum_select_bitmap_create");
  memset(minimum_select_bitmap, 0, no_bytes_in_map(table->read_set));
  if (
    spider->use_index_merge ||
#ifdef HA_CAN_BULK_ACCESS
    (spider->is_clone && !spider->is_bulk_access_clone)
#else
    spider->is_clone
#endif
  ) {
    /* need preparing for cmp_ref */
    TABLE_SHARE *table_share = table->s;
    if (
      table_share->primary_key == MAX_KEY
    ) {
      /* need all columns */
      memset(minimum_select_bitmap, 0xFF, no_bytes_in_map(table->read_set));
      DBUG_VOID_RETURN;
    } else {
      /* need primary key columns */
      uint roop_count;
      KEY *key_info;
      KEY_PART_INFO *key_part;
      Field *field;
      key_info = &table_share->key_info[table_share->primary_key];
      key_part = key_info->key_part;
      for (roop_count = 0;
        roop_count < spider_user_defined_key_parts(key_info);
        roop_count++)
      {
        field = key_part[roop_count].field;
        spider_set_bit(minimum_select_bitmap, field->field_index);
      }
    }
  }
  for (field_p = table->field; *field_p; field_p++)
  {
    uint field_index = (*field_p)->field_index;
    if (
      spider_bit_is_set(spider->searched_bitmap, field_index) ||
      bitmap_is_set(table->read_set, field_index) ||
      bitmap_is_set(table->write_set, field_index)
    ) {
      spider_set_bit(minimum_select_bitmap, field_index);
    }
  }
  DBUG_VOID_RETURN;
}

bool spider_handlersocket_handler::minimum_select_bit_is_set(
  uint field_index
) {
  DBUG_ENTER("spider_handlersocket_handler::minimum_select_bit_is_set");
  DBUG_PRINT("info",("spider field_index=%u", field_index));
  DBUG_PRINT("info",("spider minimum_select_bitmap=%s",
    spider_bit_is_set(minimum_select_bitmap, field_index) ?
      "TRUE" : "FALSE"));
  DBUG_RETURN(spider_bit_is_set(minimum_select_bitmap, field_index));
}

void spider_handlersocket_handler::copy_minimum_select_bitmap(
  uchar *bitmap
) {
  int roop_count;
  TABLE *table = spider->get_table();
  DBUG_ENTER("spider_handlersocket_handler::copy_minimum_select_bitmap");
  for (roop_count = 0;
    roop_count < (int) ((table->s->fields + 7) / 8);
    roop_count++)
  {
    bitmap[roop_count] =
      minimum_select_bitmap[roop_count];
    DBUG_PRINT("info",("spider roop_count=%d", roop_count));
    DBUG_PRINT("info",("spider bitmap=%d",
      bitmap[roop_count]));
  }
  DBUG_VOID_RETURN;
}

int spider_handlersocket_handler::init_union_table_name_pos()
{
  DBUG_ENTER("spider_handlersocket_handler::init_union_table_name_pos");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::set_union_table_name_pos()
{
  DBUG_ENTER("spider_handlersocket_handler::set_union_table_name_pos");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::reset_union_table_name(
  spider_string *str,
  int link_idx,
  ulong sql_type
) {
  DBUG_ENTER("spider_handlersocket_handler::reset_union_table_name");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

#ifdef SPIDER_HAS_GROUP_BY_HANDLER
int spider_handlersocket_handler::append_list_item_select_part(
  List<Item> *select,
  const char *alias,
  uint alias_length,
  bool use_fields,
  spider_fields *fields,
  ulong sql_type
) {
  DBUG_ENTER("spider_handlersocket_handler::append_list_item_select_part");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::append_from_and_tables_part(
  spider_fields *fields,
  ulong sql_type
) {
  DBUG_ENTER("spider_handlersocket_handler::append_from_and_tables_part");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::reappend_tables_part(
  spider_fields *fields,
  ulong sql_type
) {
  DBUG_ENTER("spider_handlersocket_handler::reappend_tables_part");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::append_where_part(
  ulong sql_type
) {
  DBUG_ENTER("spider_handlersocket_handler::append_where_part");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::append_having_part(
  ulong sql_type
) {
  DBUG_ENTER("spider_handlersocket_handler::append_having_part");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::append_item_type_part(
  Item *item,
  const char *alias,
  uint alias_length,
  bool use_fields,
  spider_fields *fields,
  ulong sql_type
) {
  DBUG_ENTER("spider_handlersocket_handler::append_item_type_part");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::append_group_by_part(
  ORDER *order,
  const char *alias,
  uint alias_length,
  bool use_fields,
  spider_fields *fields,
  ulong sql_type
) {
  DBUG_ENTER("spider_handlersocket_handler::append_group_by_part");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}

int spider_handlersocket_handler::append_order_by_part(
  ORDER *order,
  const char *alias,
  uint alias_length,
  bool use_fields,
  spider_fields *fields,
  ulong sql_type
) {
  DBUG_ENTER("spider_handlersocket_handler::append_order_by_part");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(0);
  DBUG_RETURN(0);
}
#endif
#endif
