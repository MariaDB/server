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

Item_func_vtq_ts::Item_func_vtq_ts(THD *thd, Item* a, TR_table::field_id_t _vtq_field) :
  Item_datetimefunc(thd, a),
  vtq_field(_vtq_field)
{
  decimals= 6;
  null_value= true;
  DBUG_ASSERT(arg_count == 1 && args[0]);
}


bool
Item_func_vtq_ts::get_date(MYSQL_TIME *res, ulonglong fuzzy_date)
{
  THD *thd= current_thd; // can it differ from constructor's?
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
  {
    my_error(ER_VERS_NO_TRX_ID, MYF(0), (longlong) trx_id);
    return true;
  }

  return trt[vtq_field]->get_date(res, fuzzy_date);
}


Item_func_vtq_id::Item_func_vtq_id(THD *thd, Item* a, TR_table::field_id_t _vtq_field,
                                   bool _backwards) :
  Item_longlong_func(thd, a),
  vtq_field(_vtq_field),
  backwards(_backwards)
{
  decimals= 0;
  unsigned_flag= 1;
  null_value= true;
  DBUG_ASSERT(arg_count == 1 && args[0]);
}

Item_func_vtq_id::Item_func_vtq_id(THD *thd, Item* a, Item* b, TR_table::field_id_t _vtq_field) :
  Item_longlong_func(thd, a, b),
  vtq_field(_vtq_field),
  backwards(false)
{
  decimals= 0;
  unsigned_flag= 1;
  null_value= true;
  DBUG_ASSERT(arg_count == 2 && args[0] && args[1]);
}

longlong
Item_func_vtq_id::get_by_trx_id(ulonglong trx_id)
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

  return trt[vtq_field]->val_int();
}

longlong
Item_func_vtq_id::get_by_commit_ts(MYSQL_TIME &commit_ts, bool backwards)
{
  THD *thd= current_thd;
  DBUG_ASSERT(thd);

  TR_table trt(thd);
  null_value= !trt.query(commit_ts, backwards);
  if (null_value)
    return 0;

  return trt[vtq_field]->val_int();
}

longlong
Item_func_vtq_id::val_int()
{
  if (args[0]->is_null())
  {
    if (arg_count < 2 || vtq_field == TR_table::FLD_TRX_ID)
    {
      null_value= true;
      return 0;
    }
    return get_by_trx_id(args[1]->val_uint());
  }
  else
  {
    MYSQL_TIME commit_ts;
    if (args[0]->get_date(&commit_ts, 0))
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

Item_func_vtq_trx_sees::Item_func_vtq_trx_sees(THD *thd, Item* a, Item* b) :
  Item_bool_func(thd, a, b),
  accept_eq(false)
{
  null_value= true;
  DBUG_ASSERT(arg_count == 2 && args[0] && args[1]);
}

longlong
Item_func_vtq_trx_sees::val_int()
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
