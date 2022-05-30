/* Copyright (c) 2017, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA */


/**
  @brief
    System Versioning items
*/

#include "mariadb.h"
#include "sql_priv.h"

#include "sql_class.h"
#include "tztime.h"
#include "item.h"

bool Item_func_history::val_bool()
{
  Item_field *f= static_cast<Item_field *>(args[0]);
  DBUG_ASSERT(f->fixed());
  DBUG_ASSERT(f->field->flags & VERS_ROW_END);
  return !f->field->is_max();
}

void Item_func_history::print(String *str, enum_query_type query_type)
{
  str->append(func_name_cstring());
  str->append('(');
  args[0]->print(str, query_type);
  str->append(')');
}

Item_func_trt_ts::Item_func_trt_ts(THD *thd, Item* a, TR_table::field_id_t _trt_field) :
  Item_datetimefunc(thd, a),
  trt_field(_trt_field)
{
  decimals= 6;
  null_value= true;
  DBUG_ASSERT(arg_count == 1 && args[0]);
}


bool
Item_func_trt_ts::get_date(THD *thd, MYSQL_TIME *res, date_mode_t fuzzydate)
{
  DBUG_ASSERT(thd);
  DBUG_ASSERT(args[0]);
  if (args[0]->result_type() != INT_RESULT)
  {
    my_error(ER_ILLEGAL_PARAMETER_DATA_TYPE_FOR_OPERATION, MYF(0),
      args[0]->type_handler()->name().ptr(),
      func_name());
    return true;
  }
  ulonglong trx_id= args[0]->val_uint();
  if (trx_id == ULONGLONG_MAX)
  {
    null_value= false;
    thd->variables.time_zone->gmt_sec_to_TIME(res, TIMESTAMP_MAX_VALUE);
    res->second_part= TIME_MAX_SECOND_PART;
    return false;
  }

  TR_table trt(thd);

  null_value= !trt.query(trx_id);
  if (null_value)
    return true;

  return trt[trt_field]->get_date(res, fuzzydate);
}


Item_func_trt_id::Item_func_trt_id(THD *thd, Item* a, TR_table::field_id_t _trt_field,
                                   bool _backwards) :
  Item_longlong_func(thd, a),
  trt_field(_trt_field),
  backwards(_backwards)
{
  decimals= 0;
  unsigned_flag= 1;
  null_value= true;
  DBUG_ASSERT(arg_count == 1 && args[0]);
}

Item_func_trt_id::Item_func_trt_id(THD *thd, Item* a, Item* b, TR_table::field_id_t _trt_field) :
  Item_longlong_func(thd, a, b),
  trt_field(_trt_field),
  backwards(false)
{
  decimals= 0;
  unsigned_flag= 1;
  null_value= true;
  DBUG_ASSERT(arg_count == 2 && args[0] && args[1]);
}

longlong
Item_func_trt_id::get_by_trx_id(ulonglong trx_id)
{
  THD *thd= current_thd;
  DBUG_ASSERT(thd);

  if (trx_id == ULONGLONG_MAX)
  {
    null_value= true;
    return 0;
  }

  TR_table trt(thd);
  null_value= !trt.query(trx_id);
  if (null_value)
    return 0;

  return trt[trt_field]->val_int();
}

longlong
Item_func_trt_id::get_by_commit_ts(MYSQL_TIME &commit_ts, bool backwards)
{
  THD *thd= current_thd;
  DBUG_ASSERT(thd);

  TR_table trt(thd);
  null_value= !trt.query(commit_ts, backwards);
  if (null_value)
    return backwards ? ULONGLONG_MAX : 0;

  return trt[trt_field]->val_int();
}

longlong
Item_func_trt_id::val_int()
{
  if (args[0]->is_null())
  {
    if (arg_count < 2 || trt_field == TR_table::FLD_TRX_ID)
    {
      null_value= true;
      return 0;
    }
    return get_by_trx_id(args[1]->val_uint());
  }
  else
  {
    MYSQL_TIME commit_ts;
    THD *thd= current_thd;
    Datetime::Options opt(TIME_CONV_NONE, thd);
    if (args[0]->get_date(thd, &commit_ts, opt))
    {
      null_value= true;
      return 0;
    }
    if (arg_count > 1)
    {
      backwards= args[1]->val_bool();
      DBUG_ASSERT(arg_count == 2);
    }
    return get_by_commit_ts(commit_ts, backwards);
  }
}

Item_func_trt_trx_sees::Item_func_trt_trx_sees(THD *thd, Item* a, Item* b) :
  Item_bool_func(thd, a, b),
  accept_eq(false)
{
  null_value= true;
  DBUG_ASSERT(arg_count == 2 && args[0] && args[1]);
}

longlong
Item_func_trt_trx_sees::val_int()
{
  THD *thd= current_thd;
  DBUG_ASSERT(thd);

  DBUG_ASSERT(arg_count > 1);
  ulonglong trx_id1= args[0]->val_uint();
  ulonglong trx_id0= args[1]->val_uint();
  bool result= accept_eq;

  TR_table trt(thd);
  null_value= trt.query_sees(result, trx_id1, trx_id0);
  return result;
}
