#ifndef ITEM_VERS_INCLUDED
#define ITEM_VERS_INCLUDED
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


/* System Versioning items */

#ifdef USE_PRAGMA_INTERFACE
#pragma interface			/* gcc class implementation */
#endif

class Item_func_vtq_ts: public Item_datetimefunc
{
  TR_table::field_id_t vtq_field;
public:
  Item_func_vtq_ts(THD *thd, Item* a, TR_table::field_id_t _vtq_field);
  const char *func_name() const
  {
    if (vtq_field == TR_table::FLD_BEGIN_TS)
    {
      return "vtq_begin_ts";
    }
    return "vtq_commit_ts";
  }
  bool get_date(MYSQL_TIME *res, ulonglong fuzzy_date);
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_vtq_ts>(thd, mem_root, this); }
  void fix_length_and_dec() { fix_attributes_datetime(decimals); }
};

class Item_func_vtq_id : public Item_longlong_func
{
  TR_table::field_id_t vtq_field;
  bool backwards;

  longlong get_by_trx_id(ulonglong trx_id);
  longlong get_by_commit_ts(MYSQL_TIME &commit_ts, bool backwards);

public:
  Item_func_vtq_id(THD *thd, Item* a, TR_table::field_id_t _vtq_field, bool _backwards= false);
  Item_func_vtq_id(THD *thd, Item* a, Item* b, TR_table::field_id_t _vtq_field);

  const char *func_name() const
  {
    switch (vtq_field)
    {
    case TR_table::FLD_TRX_ID:
      return "vtq_trx_id";
    case TR_table::FLD_COMMIT_ID:
        return "vtq_commit_id";
    case TR_table::FLD_ISO_LEVEL:
      return "vtq_iso_level";
    default:
      DBUG_ASSERT(0);
    }
    return NULL;
  }

  void fix_length_and_dec()
  {
    Item_int_func::fix_length_and_dec();
    max_length= 20;
  }

  longlong val_int();
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_vtq_id>(thd, mem_root, this); }
};

class Item_func_vtq_trx_sees : public Item_bool_func
{
protected:
  bool accept_eq;

public:
  Item_func_vtq_trx_sees(THD *thd, Item* a, Item* b);
  const char *func_name() const
  {
    return "vtq_trx_sees";
  }
  longlong val_int();
  Item *get_copy(THD *thd, MEM_ROOT *mem_root)
  { return get_item_copy<Item_func_vtq_trx_sees>(thd, mem_root, this); }
};

class Item_func_vtq_trx_sees_eq :
  public Item_func_vtq_trx_sees
{
public:
  Item_func_vtq_trx_sees_eq(THD *thd, Item* a, Item* b) :
    Item_func_vtq_trx_sees(thd, a, b)
  {
    accept_eq= true;
  }
  const char *func_name() const
  {
    return "vtq_trx_sees_eq";
  }
};

#endif /* ITEM_VERS_INCLUDED */
