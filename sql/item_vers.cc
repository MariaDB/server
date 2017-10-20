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

#include "sql_class.h"
#include "tztime.h"
#include "item.h"

Item_func_vtq_ts::Item_func_vtq_ts(
    THD *thd,
    handlerton* hton,
    Item* a,
    vtq_field_t _vtq_field) :
  VTQ_common<Item_datetimefunc>(thd, hton, a),
  vtq_field(_vtq_field)
{
  decimals= 6;
  null_value= true;
  DBUG_ASSERT(arg_count == 1 && args[0]);
  check_hton();
}

template <class Item_func_X>
void
VTQ_common<Item_func_X>::check_hton()
{
  DBUG_ASSERT(hton);
  if (!(hton->flags & HTON_NATIVE_SYS_VERSIONING) && hton->db_type != DB_TYPE_HEAP)
  {
    my_error(ER_VERS_ENGINE_UNSUPPORTED, MYF(0), Item::name.str ? Item::name.str : this->func_name());
    hton= NULL;
  }
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

  DBUG_ASSERT(hton && hton->vers_query_trx_id);
  null_value= !hton->vers_query_trx_id(thd, res, trx_id, vtq_field);
  if (null_value)
  {
    my_error(ER_VERS_NO_TRX_ID, MYF(0), trx_id);
  }

  return null_value;
}


Item_func_vtq_id::Item_func_vtq_id(
    THD *thd,
    handlerton *hton,
    Item* a,
    vtq_field_t _vtq_field,
    bool _backwards) :
  VTQ_common<Item_longlong_func>(thd, hton, a),
  vtq_field(_vtq_field),
  backwards(_backwards)
{
  memset(&cached_result, 0, sizeof(cached_result));
  decimals= 0;
  unsigned_flag= 1;
  null_value= true;
  DBUG_ASSERT(arg_count == 1 && args[0]);
  check_hton();
}

Item_func_vtq_id::Item_func_vtq_id(
    THD *thd,
    handlerton *hton,
    Item* a,
    Item* b,
    vtq_field_t _vtq_field) :
  VTQ_common<Item_longlong_func>(thd, hton, a, b),
  vtq_field(_vtq_field),
  backwards(false)
{
  memset(&cached_result, 0, sizeof(cached_result));
  decimals= 0;
  unsigned_flag= 1;
  null_value= true;
  DBUG_ASSERT(arg_count == 2 && args[0] && args[1]);
  check_hton();
}

longlong
Item_func_vtq_id::get_by_trx_id(ulonglong trx_id)
{
  ulonglong res;
  THD *thd= current_thd; // can it differ from constructor's?
  DBUG_ASSERT(thd);

  if (trx_id == ULONGLONG_MAX)
  {
    null_value= true;
    return 0;
  }

  DBUG_ASSERT(hton->vers_query_trx_id);
  null_value= !hton->vers_query_trx_id(thd, &res, trx_id, vtq_field);
  return res;
}

longlong
Item_func_vtq_id::get_by_commit_ts(MYSQL_TIME &commit_ts, bool backwards)
{
  THD *thd= current_thd; // can it differ from constructor's?
  DBUG_ASSERT(thd);

  DBUG_ASSERT(hton->vers_query_commit_ts);
  null_value= !hton->vers_query_commit_ts(thd, &cached_result, commit_ts, VTQ_ALL, backwards);
  if (null_value)
  {
    return 0;
  }

  switch (vtq_field)
  {
  case VTQ_COMMIT_ID:
    return cached_result.commit_id;
  case VTQ_ISO_LEVEL:
    return cached_result.iso_level;
  case VTQ_TRX_ID:
    return cached_result.trx_id;
  default:
    DBUG_ASSERT(0);
    null_value= true;
  }

  return 0;
}

longlong
Item_func_vtq_id::val_int()
{
  if (!hton)
  {
    null_value= true;
    return 0;
  }

  if (args[0]->is_null())
  {
    if (arg_count < 2 || vtq_field == VTQ_TRX_ID)
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

Item_func_vtq_trx_sees::Item_func_vtq_trx_sees(
    THD *thd,
    handlerton *hton,
    Item* a,
    Item* b) :
  VTQ_common<Item_bool_func>(thd, hton, a, b),
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

  if (!hton)
  {
    null_value= true;
    return 0;
  }

  ulonglong trx_id1, trx_id0;
  ulonglong commit_id1= 0;
  ulonglong commit_id0= 0;
  uchar iso_level1= 0;

  DBUG_ASSERT(arg_count > 1);
  trx_id1= args[0]->val_uint();
  trx_id0= args[1]->val_uint();

  vtq_record_t *cached= args[0]->vtq_cached_result();
  if (cached && cached->commit_id)
  {
    commit_id1= cached->commit_id;
    iso_level1= cached->iso_level;
  }

  cached= args[1]->vtq_cached_result();
  if (cached && cached->commit_id)
  {
    commit_id0= cached->commit_id;
  }

  if (accept_eq && trx_id1 && trx_id1 == trx_id0)
  {
    null_value= false;
    return true;
  }

  DBUG_ASSERT(hton->vers_trx_sees);
  bool result= false;
  null_value= !hton->vers_trx_sees(thd, result, trx_id1, trx_id0, commit_id1, iso_level1, commit_id0);
  return result;
}
