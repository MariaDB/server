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

class Item_func_history: public Item_bool_func
{
public:
  /*
     @param    a  Item_field for row_end system field
  */
  Item_func_history(THD *thd, Item *a): Item_bool_func(thd, a)
  {
    DBUG_ASSERT(a->type() == Item::FIELD_ITEM);
  }

  bool val_bool() override;
  longlong val_int() override
  {
    return (val_bool() ? 1 : 0);
  }
  bool fix_length_and_dec() override
  {
    maybe_null= 0;
    null_value= 0;
    decimals= 0;
    max_length= 1;
    return FALSE;
  }
  const char* func_name() const override { return "is_history"; }
  void print(String *str, enum_query_type query_type) override;
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_func_history>(thd, this); }
};

class Item_func_trt_ts: public Item_datetimefunc
{
  TR_table::field_id_t trt_field;
public:
  Item_func_trt_ts(THD *thd, Item* a, TR_table::field_id_t _trt_field);
  const char *func_name() const override
  {
    if (trt_field == TR_table::FLD_BEGIN_TS)
    {
      return "trt_begin_ts";
    }
    return "trt_commit_ts";
  }
  bool get_date(THD *thd, MYSQL_TIME *res, date_mode_t fuzzydate) override;
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_func_trt_ts>(thd, this); }
  bool fix_length_and_dec() override
  { fix_attributes_datetime(decimals); return FALSE; }
};

class Item_func_trt_id : public Item_longlong_func
{
  TR_table::field_id_t trt_field;
  bool backwards;

  longlong get_by_trx_id(ulonglong trx_id);
  longlong get_by_commit_ts(MYSQL_TIME &commit_ts, bool backwards);

public:
  Item_func_trt_id(THD *thd, Item* a, TR_table::field_id_t _trt_field, bool _backwards= false);
  Item_func_trt_id(THD *thd, Item* a, Item* b, TR_table::field_id_t _trt_field);

  const char *func_name() const override
  {
    switch (trt_field)
    {
    case TR_table::FLD_TRX_ID:
      return "trt_trx_id";
    case TR_table::FLD_COMMIT_ID:
        return "trt_commit_id";
    case TR_table::FLD_ISO_LEVEL:
      return "trt_iso_level";
    default:
      DBUG_ASSERT(0);
    }
    return NULL;
  }

  bool fix_length_and_dec() override
  {
    bool res= Item_int_func::fix_length_and_dec();
    max_length= 20;
    return res;
  }

  longlong val_int() override;
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_func_trt_id>(thd, this); }
};

class Item_func_trt_trx_sees : public Item_bool_func
{
protected:
  bool accept_eq;

public:
  Item_func_trt_trx_sees(THD *thd, Item* a, Item* b);
  const char *func_name() const override
  {
    return "trt_trx_sees";
  }
  longlong val_int() override;
  Item *do_get_copy(THD *thd) const override
  { return get_item_copy<Item_func_trt_trx_sees>(thd, this); }
};

class Item_func_trt_trx_sees_eq :
  public Item_func_trt_trx_sees
{
public:
  Item_func_trt_trx_sees_eq(THD *thd, Item* a, Item* b) :
    Item_func_trt_trx_sees(thd, a, b)
  {
    accept_eq= true;
  }
  const char *func_name() const override
  {
    return "trt_trx_sees_eq";
  }
};

#endif /* ITEM_VERS_INCLUDED */
