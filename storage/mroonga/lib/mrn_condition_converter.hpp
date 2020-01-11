/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2013 Kouhei Sutou <kou@clear-code.com>

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA
*/

#ifndef MRN_CONDITION_CONVERTER_HPP_
#define MRN_CONDITION_CONVERTER_HPP_

#include <mrn_mysql_compat.h>

#include <item_cmpfunc.h>

#include <groonga.h>

namespace mrn {
  class ConditionConverter {
  public:
    ConditionConverter(grn_ctx *ctx, grn_obj *table, bool is_storage_mode);
    ~ConditionConverter();

    bool is_convertable(const Item *item);
    unsigned int count_match_against(const Item *item);
    // caller must check "where" can be convertable by
    // is_convertable(). This method doesn't validate "where".
    void convert(const Item *where, grn_obj *expression);

  private:
    enum NormalizedType {
      STRING_TYPE,
      INT_TYPE,
      TIME_TYPE,
      UNSUPPORTED_TYPE,
    };

    grn_ctx *ctx_;
    grn_obj *table_;
    bool is_storage_mode_;
    grn_obj column_name_;
    grn_obj value_;

    bool is_convertable(const Item_cond *cond_item);
    bool is_convertable(const Item_func *func_item);
    bool is_convertable_binary_operation(const Item_field *field_item,
                                         Item *value_item,
                                         Item_func::Functype func_type);
    bool is_convertable_between(const Item_field *field_item,
                                Item *min_item,
                                Item *max_item);
    bool is_valid_time_value(const Item_field *field_item,
                             Item *value_item);
    bool get_time_value(const Item_field *field_item,
                        Item *value_item,
                        MYSQL_TIME *mysql_time);
    bool have_index(const Item_field *field_item, grn_operator _operator);
    bool have_index(const Item_field *field_item, Item_func::Functype func_type);

    NormalizedType normalize_field_type(enum_field_types field_type);

    void convert_binary_operation(const Item_func *func_item,
                                  grn_obj *expression,
                                  grn_operator _operator);
    void convert_between(const Item_func *func_item, grn_obj *expression);
    void append_field_value(const Item_field *field_item,
                            grn_obj *expression);
    void append_const_item(const Item_field *field_item,
                           Item *const_item,
                           grn_obj *expression);
  };
}

#endif /* MRN_CONDITION_CONVERTER_HPP_ */
