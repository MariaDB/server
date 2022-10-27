/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2013-2017 Kouhei Sutou <kou@clear-code.com>

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

#include "mrn_condition_converter.hpp"
#include "mrn_time_converter.hpp"
#include "mrn_smart_grn_obj.hpp"

// for debug
#define MRN_CLASS_NAME "mrn::ConditionConverter"

#ifdef MRN_ITEM_HAVE_ITEM_NAME
#  define MRN_ITEM_FIELD_GET_NAME(item)        ((item)->item_name.ptr())
#  define MRN_ITEM_FIELD_GET_NAME_LENGTH(item) ((item)->item_name.length())
#else
#  define MRN_ITEM_FIELD_GET_NAME(item)        ((item)->name.str)
#  define MRN_ITEM_FIELD_GET_NAME_LENGTH(item) ((item)->name.length)
#endif

namespace mrn {
  ConditionConverter::ConditionConverter(grn_ctx *ctx, grn_obj *table,
                                         bool is_storage_mode)
    : ctx_(ctx),
      table_(table),
      is_storage_mode_(is_storage_mode) {
    GRN_TEXT_INIT(&column_name_, 0);
    GRN_VOID_INIT(&value_);
  }

  ConditionConverter::~ConditionConverter() {
    grn_obj_unlink(ctx_, &column_name_);
    grn_obj_unlink(ctx_, &value_);
  }

  bool ConditionConverter::is_convertable(const Item *item) {
    MRN_DBUG_ENTER_METHOD();

    if (!item) {
      DBUG_RETURN(false);
    }

    switch (item->type()) {
    case Item::COND_ITEM:
      {
        const Item_cond *cond_item = reinterpret_cast<const Item_cond *>(item);
        bool convertable = is_convertable(cond_item);
        DBUG_RETURN(convertable);
      }
      break;
    case Item::FUNC_ITEM:
      {
        const Item_func *func_item = reinterpret_cast<const Item_func *>(item);
        bool convertable = is_convertable(func_item);
        DBUG_RETURN(convertable);
      }
      break;
    default:
      DBUG_RETURN(false);
      break;
    }

    DBUG_RETURN(false);
  }

  bool ConditionConverter::is_convertable(const Item_cond *cond_item) {
    MRN_DBUG_ENTER_METHOD();

    if (!is_storage_mode_) {
      DBUG_RETURN(false);
    }

    if (cond_item->functype() != Item_func::COND_AND_FUNC) {
      DBUG_RETURN(false);
    }

    List<Item> *argument_list =
      const_cast<Item_cond *>(cond_item)->argument_list();
    List_iterator<Item> iterator(*argument_list);
    const Item *sub_item;
    while ((sub_item = iterator++)) {
      if (!is_convertable(sub_item)) {
        DBUG_RETURN(false);
      }
    }

    DBUG_RETURN(true);
  }

  bool ConditionConverter::is_convertable(const Item_func *func_item) {
    MRN_DBUG_ENTER_METHOD();

    switch (func_item->functype()) {
    case Item_func::EQ_FUNC:
    case Item_func::LT_FUNC:
    case Item_func::LE_FUNC:
    case Item_func::GE_FUNC:
    case Item_func::GT_FUNC:
      if (!is_storage_mode_) {
        DBUG_RETURN(false);
      }
      {
        Item **arguments = func_item->arguments();
        Item *left_item = arguments[0];
        Item *right_item = arguments[1];
        if (left_item->type() != Item::FIELD_ITEM) {
          DBUG_RETURN(false);
        }
        if (!right_item->basic_const_item()) {
          DBUG_RETURN(false);
        }

        bool convertable =
          is_convertable_binary_operation(static_cast<Item_field *>(left_item),
                                          right_item,
                                          func_item->functype());
        DBUG_RETURN(convertable);
      }
      break;
    case Item_func::FT_FUNC:
      DBUG_RETURN(true);
      break;
    case Item_func::BETWEEN:
      if (!is_storage_mode_) {
        DBUG_RETURN(false);
      }
      {
        Item **arguments = func_item->arguments();
        Item *target_item = arguments[0];
        Item *min_item = arguments[1];
        Item *max_item = arguments[2];
        if (target_item->type() != Item::FIELD_ITEM) {
          DBUG_RETURN(false);
        }
        if (!min_item->basic_const_item()) {
          DBUG_RETURN(false);
        }
        if (!max_item->basic_const_item()) {
          DBUG_RETURN(false);
        }

        bool convertable =
          is_convertable_between(static_cast<Item_field *>(target_item),
                                 min_item,
                                 max_item);
        DBUG_RETURN(convertable);
      }
    default:
      DBUG_RETURN(false);
      break;
    }

    DBUG_RETURN(true);
  }

  bool ConditionConverter::is_convertable_binary_operation(
    const Item_field *field_item,
    Item *value_item,
    Item_func::Functype func_type) {
    MRN_DBUG_ENTER_METHOD();

    bool convertable = false;

    enum_field_types field_type = field_item->field->real_type();
    NormalizedType normalized_type = normalize_field_type(field_type);
    switch (normalized_type) {
    case STRING_TYPE:
      if (value_item->is_of_type(Item::CONST_ITEM, STRING_RESULT) &&
          func_type == Item_func::EQ_FUNC) {
        convertable = have_index(field_item, GRN_OP_EQUAL);
      }
      break;
    case INT_TYPE:
      if (field_type == MYSQL_TYPE_ENUM) {
        convertable = value_item->is_of_type(Item::CONST_ITEM, STRING_RESULT) ||
                      value_item->is_of_type(Item::CONST_ITEM, INT_RESULT);
      } else {
        convertable = value_item->is_of_type(Item::CONST_ITEM, INT_RESULT);
      }
      break;
    case TIME_TYPE:
      if (is_valid_time_value(field_item, value_item)) {
        convertable = have_index(field_item, func_type);
      }
      break;
    case UNSUPPORTED_TYPE:
      break;
    }

    DBUG_RETURN(convertable);
  }

  bool ConditionConverter::is_convertable_between(const Item_field *field_item,
                                                  Item *min_item,
                                                  Item *max_item) {
    MRN_DBUG_ENTER_METHOD();

    bool convertable = false;

    enum_field_types field_type = field_item->field->type();
    NormalizedType normalized_type = normalize_field_type(field_type);
    switch (normalized_type) {
    case STRING_TYPE:
      if (min_item->is_of_type(Item::CONST_ITEM, STRING_RESULT) &&
          max_item->is_of_type(Item::CONST_ITEM, STRING_RESULT)) {
        convertable = have_index(field_item, GRN_OP_LESS);
      }
      break;
    case INT_TYPE:
      if (min_item->is_of_type(Item::CONST_ITEM, INT_RESULT) &&
          max_item->is_of_type(Item::CONST_ITEM, INT_RESULT)) {
        convertable = have_index(field_item, GRN_OP_LESS);
      }
      break;
    case TIME_TYPE:
      if (is_valid_time_value(field_item, min_item) &&
          is_valid_time_value(field_item, max_item)) {
        convertable = have_index(field_item, GRN_OP_LESS);
      }
      break;
    case UNSUPPORTED_TYPE:
      break;
    }

    DBUG_RETURN(convertable);
  }

  bool ConditionConverter::is_valid_time_value(const Item_field *field_item,
                                               Item *value_item) {
    MRN_DBUG_ENTER_METHOD();

    MYSQL_TIME mysql_time;
    bool error = get_time_value(field_item, value_item, &mysql_time);

    DBUG_RETURN(!error);
  }

  bool ConditionConverter::get_time_value(const Item_field *field_item,
                                          Item *value_item,
                                          MYSQL_TIME *mysql_time) {
    MRN_DBUG_ENTER_METHOD();

    bool error;
    Item *real_value_item = value_item->real_item();
    switch (field_item->field->type()) {
    case MYSQL_TYPE_TIME:
    {
      THD *thd= current_thd;
      error= real_value_item->get_date(thd, mysql_time, Time::Options(thd));
      break;
    }
    case MYSQL_TYPE_YEAR:
      mysql_time->year        = static_cast<int>(value_item->val_int());
      mysql_time->month       = 1;
      mysql_time->day         = 1;
      mysql_time->hour        = 0;
      mysql_time->hour        = 0;
      mysql_time->minute      = 0;
      mysql_time->second_part = 0;
      mysql_time->neg         = false;
      mysql_time->time_type   = MYSQL_TIMESTAMP_DATE;
      error = false;
      break;
    default:
    {
      THD *thd= current_thd;
      Datetime::Options opt(TIME_FUZZY_DATES, thd);
      error = real_value_item->get_date(thd, mysql_time, opt);
      break;
    }
    }

    DBUG_RETURN(error);
  }

  ConditionConverter::NormalizedType
  ConditionConverter::normalize_field_type(enum_field_types field_type) {
    MRN_DBUG_ENTER_METHOD();

    NormalizedType type = UNSUPPORTED_TYPE;

    switch (field_type) {
    case MYSQL_TYPE_DECIMAL:
      type = STRING_TYPE;
      break;
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_LONG:
      type = INT_TYPE;
      break;
    case MYSQL_TYPE_FLOAT:
    case MYSQL_TYPE_DOUBLE:
      type = UNSUPPORTED_TYPE;
      break;
    case MYSQL_TYPE_NULL:
      type = UNSUPPORTED_TYPE;
      break;
    case MYSQL_TYPE_TIMESTAMP:
      type = TIME_TYPE;
      break;
    case MYSQL_TYPE_LONGLONG:
    case MYSQL_TYPE_INT24:
      type = INT_TYPE;
      break;
    case MYSQL_TYPE_DATE:
    case MYSQL_TYPE_TIME:
    case MYSQL_TYPE_DATETIME:
    case MYSQL_TYPE_YEAR:
    case MYSQL_TYPE_NEWDATE:
      type = TIME_TYPE;
      break;
    case MYSQL_TYPE_VARCHAR:
      type = STRING_TYPE;
      break;
    case MYSQL_TYPE_BIT:
      type = INT_TYPE;
      break;
#ifdef MRN_HAVE_MYSQL_TYPE_TIMESTAMP2
    case MYSQL_TYPE_TIMESTAMP2:
      type = TIME_TYPE;
      break;
#endif
#ifdef MRN_HAVE_MYSQL_TYPE_DATETIME2
    case MYSQL_TYPE_DATETIME2:
      type = TIME_TYPE;
      break;
#endif
#ifdef MRN_HAVE_MYSQL_TYPE_TIME2
    case MYSQL_TYPE_TIME2:
      type = TIME_TYPE;
      break;
#endif
    case MYSQL_TYPE_NEWDECIMAL:
      type = STRING_TYPE;
      break;
    case MYSQL_TYPE_ENUM:
      type = INT_TYPE;
      break;
    case MYSQL_TYPE_SET:
      type = INT_TYPE;
      break;
    case MYSQL_TYPE_TINY_BLOB:
    case MYSQL_TYPE_MEDIUM_BLOB:
    case MYSQL_TYPE_LONG_BLOB:
    case MYSQL_TYPE_BLOB:
    case MYSQL_TYPE_VAR_STRING:
    case MYSQL_TYPE_STRING:
      type = STRING_TYPE;
      break;
    case MYSQL_TYPE_GEOMETRY:
      type = UNSUPPORTED_TYPE;
      break;
    case MYSQL_TYPE_VARCHAR_COMPRESSED:
    case MYSQL_TYPE_BLOB_COMPRESSED:
      DBUG_ASSERT(0);
#ifdef MRN_HAVE_MYSQL_TYPE_JSON
    case MYSQL_TYPE_JSON:
      type = STRING_TYPE;
      break;
#endif
    }

    DBUG_RETURN(type);
  }

  bool ConditionConverter::have_index(const Item_field *field_item,
                                      grn_operator _operator) {
    MRN_DBUG_ENTER_METHOD();

    grn_obj *column;
    column = grn_obj_column(ctx_, table_,
                            MRN_ITEM_FIELD_GET_NAME(field_item),
                            MRN_ITEM_FIELD_GET_NAME_LENGTH(field_item));
    if (!column) {
      DBUG_RETURN(false);
    }
    mrn::SmartGrnObj smart_column(ctx_, column);

    int n_indexes = grn_column_index(ctx_, column, _operator, NULL, 0, NULL);
    bool convertable = (n_indexes > 0);

    DBUG_RETURN(convertable);
  }

  bool ConditionConverter::have_index(const Item_field *field_item,
                                      Item_func::Functype func_type) {
    MRN_DBUG_ENTER_METHOD();

    bool have = false;
    switch (func_type) {
    case Item_func::EQ_FUNC:
      have = have_index(field_item, GRN_OP_EQUAL);
      break;
    case Item_func::LT_FUNC:
      have = have_index(field_item, GRN_OP_LESS);
      break;
    case Item_func::LE_FUNC:
      have = have_index(field_item, GRN_OP_LESS_EQUAL);
      break;
    case Item_func::GE_FUNC:
      have = have_index(field_item, GRN_OP_GREATER_EQUAL);
      break;
    case Item_func::GT_FUNC:
      have = have_index(field_item, GRN_OP_GREATER);
      break;
    default:
      break;
    }

    DBUG_RETURN(have);
  }

  unsigned int ConditionConverter::count_match_against(const Item *item) {
    MRN_DBUG_ENTER_METHOD();

    if (!item) {
      DBUG_RETURN(0);
    }

    switch (item->type()) {
    case Item::COND_ITEM:
      if (is_storage_mode_) {
        Item_cond *cond_item = (Item_cond *)item;
        if (cond_item->functype() == Item_func::COND_AND_FUNC) {
          unsigned int n_match_againsts = 0;
          List_iterator<Item> iterator(*((cond_item)->argument_list()));
          const Item *sub_item;
          while ((sub_item = iterator++)) {
            n_match_againsts += count_match_against(sub_item);
          }
          DBUG_RETURN(n_match_againsts);
        }
      }
      break;
    case Item::FUNC_ITEM:
      {
        const Item_func *func_item = (const Item_func *)item;
        switch (func_item->functype()) {
        case Item_func::FT_FUNC:
          DBUG_RETURN(1);
          break;
        default:
          break;
        }
      }
      break;
    default:
      break;
    }

    DBUG_RETURN(0);
  }

  void ConditionConverter::convert(const Item *where, grn_obj *expression) {
    MRN_DBUG_ENTER_METHOD();

    if (!where || where->type() != Item::COND_ITEM) {
      DBUG_VOID_RETURN;
    }

    Item_cond *cond_item = (Item_cond *)where;
    List_iterator<Item> iterator(*((cond_item)->argument_list()));
    const Item *sub_item;
    while ((sub_item = iterator++)) {
      switch (sub_item->type()) {
      case Item::FUNC_ITEM:
        {
          const Item_func *func_item = (const Item_func *)sub_item;
          switch (func_item->functype()) {
          case Item_func::EQ_FUNC:
            convert_binary_operation(func_item, expression, GRN_OP_EQUAL);
            break;
          case Item_func::LT_FUNC:
            convert_binary_operation(func_item, expression, GRN_OP_LESS);
            break;
          case Item_func::LE_FUNC:
            convert_binary_operation(func_item, expression, GRN_OP_LESS_EQUAL);
            break;
          case Item_func::GE_FUNC:
            convert_binary_operation(func_item, expression,
                                     GRN_OP_GREATER_EQUAL);
            break;
          case Item_func::GT_FUNC:
            convert_binary_operation(func_item, expression, GRN_OP_GREATER);
            break;
          case Item_func::BETWEEN:
            convert_between(func_item, expression);
            break;
          default:
            break;
          }
        }
        break;
      default:
        break;
      }
    }

    DBUG_VOID_RETURN;
  }

  void ConditionConverter::convert_binary_operation(const Item_func *func_item,
                                                    grn_obj *expression,
                                                    grn_operator _operator) {
    Item **arguments = func_item->arguments();
    Item *left_item = arguments[0];
    Item *right_item = arguments[1];
    if (left_item->type() == Item::FIELD_ITEM) {
      const Item_field *field_item = static_cast<const Item_field *>(left_item);
      append_field_value(field_item, expression);
      append_const_item(field_item, right_item, expression);
      grn_expr_append_op(ctx_, expression, _operator, 2);
      grn_expr_append_op(ctx_, expression, GRN_OP_AND, 2);
    }
  }

  void ConditionConverter::convert_between(const Item_func *func_item,
                                           grn_obj *expression) {
    MRN_DBUG_ENTER_METHOD();

    Item **arguments = func_item->arguments();
    Item *target_item = arguments[0];
    Item *min_item = arguments[1];
    Item *max_item = arguments[2];

    grn_obj *between_func = grn_ctx_get(ctx_, "between", strlen("between"));
    grn_expr_append_obj(ctx_, expression, between_func, GRN_OP_PUSH, 1);

    const Item_field *field_item = static_cast<const Item_field *>(target_item);
    append_field_value(field_item, expression);

    grn_obj include;
    mrn::SmartGrnObj smart_include(ctx_, &include);
    GRN_TEXT_INIT(&include, 0);
    GRN_TEXT_PUTS(ctx_, &include, "include");
    append_const_item(field_item, min_item, expression);
    grn_expr_append_const(ctx_, expression, &include, GRN_OP_PUSH, 1);
    append_const_item(field_item, max_item, expression);
    grn_expr_append_const(ctx_, expression, &include, GRN_OP_PUSH, 1);

    grn_expr_append_op(ctx_, expression, GRN_OP_CALL, 5);

    grn_expr_append_op(ctx_, expression, GRN_OP_AND, 2);

    DBUG_VOID_RETURN;
  }

  void ConditionConverter::append_field_value(const Item_field *field_item,
                                              grn_obj *expression) {
    MRN_DBUG_ENTER_METHOD();

    GRN_BULK_REWIND(&column_name_);
    GRN_TEXT_PUT(ctx_, &column_name_,
                 MRN_ITEM_FIELD_GET_NAME(field_item),
                 MRN_ITEM_FIELD_GET_NAME_LENGTH(field_item));
    grn_expr_append_const(ctx_, expression, &column_name_,
                          GRN_OP_PUSH, 1);
    grn_expr_append_op(ctx_, expression, GRN_OP_GET_VALUE, 1);

    DBUG_VOID_RETURN;
  }

  void ConditionConverter::append_const_item(const Item_field *field_item,
                                             Item *const_item,
                                             grn_obj *expression) {
    MRN_DBUG_ENTER_METHOD();

    enum_field_types field_type = field_item->field->real_type();
    NormalizedType normalized_type = normalize_field_type(field_type);

    switch (normalized_type) {
    case STRING_TYPE:
      grn_obj_reinit(ctx_, &value_, GRN_DB_TEXT, 0);
      {
        String *string;
        string = const_item->val_str(NULL);
        GRN_TEXT_SET(ctx_, &value_, string->ptr(), string->length());
      }
      break;
    case INT_TYPE:
      grn_obj_reinit(ctx_, &value_, GRN_DB_INT64, 0);
      if (field_type == MYSQL_TYPE_ENUM) {
        if (const_item->is_of_type(Item::CONST_ITEM, STRING_RESULT)) {
          String *string;
          string = const_item->val_str(NULL);
          Field_enum *enum_field = static_cast<Field_enum *>(field_item->field);
          int enum_value = find_type(string->c_ptr(),
                                     enum_field->typelib,
                                     FIND_TYPE_BASIC);
          GRN_INT64_SET(ctx_, &value_, enum_value);
        } else {
          GRN_INT64_SET(ctx_, &value_, const_item->val_int());
        }
      } else {
        GRN_INT64_SET(ctx_, &value_, const_item->val_int());
      }
      break;
    case TIME_TYPE:
      grn_obj_reinit(ctx_, &value_, GRN_DB_TIME, 0);
      {
        MYSQL_TIME mysql_time;
        get_time_value(field_item, const_item, &mysql_time);
        bool truncated = false;
        TimeConverter time_converter;
        long long int time =
          time_converter.mysql_time_to_grn_time(&mysql_time, &truncated);
        GRN_TIME_SET(ctx_, &value_, time);
      }
      break;
    case UNSUPPORTED_TYPE:
      // Should not be occurred.
      DBUG_PRINT("error",
                 ("mroonga: append_const_item: unsupported type: <%d> "
                  "This case should not be occurred.",
                  field_type));
      grn_obj_reinit(ctx_, &value_, GRN_DB_VOID, 0);
      break;
    }
    grn_expr_append_const(ctx_, expression, &value_, GRN_OP_PUSH, 1);

    DBUG_VOID_RETURN;
  }
}
