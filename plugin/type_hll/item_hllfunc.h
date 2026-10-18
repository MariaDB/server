#ifndef ITEM_HLLFUNC_INCLUDED
#define ITEM_HLLFUNC_INCLUDED
/* Copyright (c) 2026, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02111-1301 USA */

#include "item.h"
#include "item_func.h"
#include "item_strfunc.h"
#include "item_sum.h"
#include "hll.h"

/*
  Hash contract v1: typed canonical bytes of the SQL value, hashed with
  XXH3-64, seed 0. The domain is resolved once from the argument data type.
*/
enum class Hll_domain
{
  UNSUPPORTED, INTEGER, FLOAT, DOUBLE, BINARY,
  DATE, TIME, DATETIME, TIMESTAMP, YEAR, UUID, INET4, INET6
};

Hll_domain hll_input_domain(const Item *item);

/*
  Hash one value of the given domain.
  @param buf   scratch buffer for string values
  @param null  set to true when the value is SQL NULL
  @retval true error (already reported)
*/
bool hll_hash_item(THD *thd, Item *item, Hll_domain domain, String *buf,
                   uint64 *hash, bool *null);


/* Base for scalar functions returning HLL */
class Item_hll_func: public Item_str_func
{
public:
  using Item_str_func::Item_str_func;
  const Type_handler *type_handler() const override;
  bool fix_length_and_dec(THD *thd) override;
  bool val_native(THD *thd, Native *to) override;
};


class Item_func_hll_empty: public Item_hll_func
{
public:
  using Item_hll_func::Item_hll_func;
  String *val_str(String *to) override;
  LEX_CSTRING func_name_cstring() const override
  { return "hll_empty"_LEX_CSTRING; }
  Item *shallow_copy(THD *thd) const override
  { return get_item_copy<Item_func_hll_empty>(thd, this); }
};


class Item_func_hll_union: public Item_hll_func
{
  Hll_state *m_state[2]= {NULL, NULL};
  String m_buf[2];
public:
  using Item_hll_func::Item_hll_func;
  bool fix_length_and_dec(THD *thd) override;
  String *val_str(String *to) override;
  void cleanup() override;
  LEX_CSTRING func_name_cstring() const override
  { return "hll_union"_LEX_CSTRING; }
  Item *shallow_copy(THD *thd) const override
  { return get_item_copy<Item_func_hll_union>(thd, this); }
};


class Item_func_hll_cardinality: public Item_real_func
{
  Hll_state *m_state= NULL;
  String m_buf;
public:
  using Item_real_func::Item_real_func;
  bool fix_length_and_dec(THD *thd) override;
  double val_real() override;
  void cleanup() override;
  LEX_CSTRING func_name_cstring() const override
  { return "hll_cardinality"_LEX_CSTRING; }
  Item *shallow_copy(THD *thd) const override
  { return get_item_copy<Item_func_hll_cardinality>(thd, this); }
};


class Item_func_hll_cardinality_round: public Item_longlong_func
{
  Hll_state *m_state= NULL;
  String m_buf;
public:
  using Item_longlong_func::Item_longlong_func;
  bool fix_length_and_dec(THD *thd) override;
  longlong val_int() override;
  void cleanup() override;
  LEX_CSTRING func_name_cstring() const override
  { return "hll_cardinality_round"_LEX_CSTRING; }
  Item *shallow_copy(THD *thd) const override
  { return get_item_copy<Item_func_hll_cardinality_round>(thd, this); }
};


class Item_func_hll_format_version: public Item_long_func
{
  String m_buf;
public:
  using Item_long_func::Item_long_func;
  bool fix_length_and_dec(THD *thd) override;
  longlong val_int() override;
  LEX_CSTRING func_name_cstring() const override
  { return "hll_format_version"_LEX_CSTRING; }
  Item *shallow_copy(THD *thd) const override
  { return get_item_copy<Item_func_hll_format_version>(thd, this); }
};


class Item_func_hll_info: public Item_str_func
{
  Hll_state *m_state= NULL;
  String m_buf;
public:
  using Item_str_func::Item_str_func;
  bool fix_length_and_dec(THD *thd) override;
  String *val_str(String *to) override;
  void cleanup() override;
  LEX_CSTRING func_name_cstring() const override
  { return "hll_info"_LEX_CSTRING; }
  Item *shallow_copy(THD *thd) const override
  { return get_item_copy<Item_func_hll_info>(thd, this); }
};


/* Base for aggregates returning HLL */
class Item_sum_hll: public Item_sum_plugin
{
protected:
  Hll_state *m_state= NULL;
  String m_buf;
  bool init_state();
  void endup() { if (aggr) aggr->endup(); }
public:
  Item_sum_hll(THD *thd, Item *item): Item_sum_plugin(thd, item) {}
  Item_sum_hll(THD *thd, Item_sum_hll *item): Item_sum_plugin(thd, item) {}
  const Type_handler *type_handler() const override;
  bool fix_length_and_dec(THD *thd) override;
  void clear() override;
  void cleanup() override;
  String *val_str(String *to) override;
  bool val_native(THD *thd, Native *to) override;
  /* Unreachable: Type_handler_hll::can_return_*() reject these contexts */
  double val_real() override { null_value= true; return 0; }
  longlong val_int() override { null_value= true; return 0; }
  my_decimal *val_decimal(my_decimal *) override
  { null_value= true; return NULL; }
  bool get_date(THD *, MYSQL_TIME *, date_mode_t) override
  { null_value= true; return true; }
};


class Item_sum_hll_build: public Item_sum_hll
{
  using Self= Item_sum_hll_build;
  Hll_domain m_domain= Hll_domain::UNSUPPORTED;
  String m_arg_buf;
public:
  Item_sum_hll_build(THD *thd, Item *item): Item_sum_hll(thd, item) {}
  Item_sum_hll_build(THD *thd, Self *item)
   :Item_sum_hll(thd, item), m_domain(item->m_domain) {}
  bool fix_length_and_dec(THD *thd) override;
  bool add_row() override;
  LEX_CSTRING func_name_cstring() const override
  { return "hll_build_agg("_LEX_CSTRING; }
  Item *copy_or_same(THD *thd) override
  { return new (thd->mem_root) Self(thd, this); }
protected:
  Item *shallow_copy(THD *thd) const override
  { return get_item_copy<Self>(thd, this); }
};


class Item_sum_hll_union: public Item_sum_hll
{
  using Self= Item_sum_hll_union;
  Hll_state *m_arg_state= NULL;
public:
  Item_sum_hll_union(THD *thd, Item *item): Item_sum_hll(thd, item) {}
  Item_sum_hll_union(THD *thd, Self *item): Item_sum_hll(thd, item) {}
  bool fix_length_and_dec(THD *thd) override;
  bool add_row() override;
  void cleanup() override;
  /* HLL has no equality contract in v1; union is idempotent anyway */
  bool supports_distinct() const override { return false; }
  LEX_CSTRING func_name_cstring() const override
  { return "hll_union_agg("_LEX_CSTRING; }
  Item *copy_or_same(THD *thd) override
  { return new (thd->mem_root) Self(thd, this); }
protected:
  Item *shallow_copy(THD *thd) const override
  { return get_item_copy<Self>(thd, this); }
};

#endif // ITEM_HLLFUNC_INCLUDED
