/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2010-2013 Kentoku SHIBA
  Copyright(C) 2011-2017 Kouhei Sutou <kou@clear-code.com>

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

#include "mrn_count_skip_checker.hpp"

#include <item_sum.h>

// for debug
#define MRN_CLASS_NAME "mrn::CountSkipChecker"

namespace mrn {
  CountSkipChecker::CountSkipChecker(grn_ctx *ctx,
                                     TABLE *table,
                                     SELECT_LEX *select_lex,
                                     KEY *key_info,
                                     key_part_map target_key_part_map,
                                     bool is_storage_mode)
    : ctx_(ctx),
      table_(table),
      select_lex_(select_lex),
      key_info_(key_info),
      target_key_part_map_(target_key_part_map),
      is_storage_mode_(is_storage_mode) {
  }

  CountSkipChecker::~CountSkipChecker() {
  }

  bool CountSkipChecker::check() {
    MRN_DBUG_ENTER_METHOD();

    if (select_lex_->item_list.elements != 1) {
      GRN_LOG(ctx_, GRN_LOG_DEBUG,
              "[mroonga][count-skip][false] not only one item: %u",
              select_lex_->item_list.elements);
      DBUG_RETURN(false);
    }
    if (select_lex_->group_list.elements > 0) {
      GRN_LOG(ctx_, GRN_LOG_DEBUG,
              "[mroonga][count-skip][false] have groups: %u",
              select_lex_->group_list.elements);
      DBUG_RETURN(false);
    }
    if (MRN_SELECT_LEX_GET_HAVING_COND(select_lex_)) {
      GRN_LOG(ctx_, GRN_LOG_DEBUG,
              "[mroonga][count-skip][false] have HAVING");
      DBUG_RETURN(false);
    }
    if (select_lex_->table_list.elements != 1) {
      GRN_LOG(ctx_, GRN_LOG_DEBUG,
              "[mroonga][count-skip][false] not only one table: %u",
              select_lex_->table_list.elements);
      DBUG_RETURN(false);
    }

    Item *info = static_cast<Item *>(select_lex_->item_list.first_node()->info);
    if (info->type() != Item::SUM_FUNC_ITEM) {
      GRN_LOG(ctx_, GRN_LOG_DEBUG,
              "[mroonga][count-skip][false] item isn't sum function: %u",
              info->type());
      DBUG_RETURN(false);
    }
    Item_sum *sum_item = static_cast<Item_sum *>(info);
    if (sum_item->sum_func() != Item_sum::COUNT_FUNC) {
      GRN_LOG(ctx_, GRN_LOG_DEBUG,
              "[mroonga][count-skip][false] not COUNT: %u",
              sum_item->sum_func());
      DBUG_RETURN(false);
    }
    if (ITEM_SUM_GET_NEST_LEVEL(sum_item) != 0 ||
        ITEM_SUM_GET_AGGR_LEVEL(sum_item) != 0 ||
        ITEM_SUM_GET_MAX_AGGR_LEVEL(sum_item) != -1 ||
        sum_item->max_sum_func_level != -1) {
      GRN_LOG(ctx_, GRN_LOG_DEBUG,
              "[mroonga][count-skip][false] not simple COUNT(*): %d:%d:%d:%d",
              ITEM_SUM_GET_NEST_LEVEL(sum_item),
              ITEM_SUM_GET_AGGR_LEVEL(sum_item),
              ITEM_SUM_GET_MAX_AGGR_LEVEL(sum_item),
              sum_item->max_sum_func_level);
      DBUG_RETURN(false);
    }

    Item *where = MRN_SELECT_LEX_GET_WHERE_COND(select_lex_);
    if (!where) {
      if (is_storage_mode_) {
        GRN_LOG(ctx_, GRN_LOG_DEBUG,
                "[mroonga][count-skip][true] no condition");
        DBUG_RETURN(true);
      } else {
        GRN_LOG(ctx_, GRN_LOG_DEBUG,
                "[mroonga][count-skip][false] no condition with wrapper mode");
        DBUG_RETURN(false);
      }
    }

    bool skippable = is_skippable(where);
    DBUG_RETURN(skippable);
  }

  bool CountSkipChecker::is_skippable(Item *where) {
    MRN_DBUG_ENTER_METHOD();

    bool skippable = false;
    switch (where->type()) {
    case Item::COND_ITEM:
      {
        Item_cond *cond_item = static_cast<Item_cond *>(where);
        skippable = is_skippable(cond_item);
        if (skippable) {
          GRN_LOG(ctx_, GRN_LOG_DEBUG,
                  "[mroonga][count-skip][true] skippable multiple conditions");
        }
      }
      break;
    case Item::FUNC_ITEM:
      {
        Item_func *func_item = static_cast<Item_func *>(where);
        if (func_item->functype() == Item_func::FT_FUNC) {
          if (select_lex_->select_n_where_fields == 1) {
            GRN_LOG(ctx_, GRN_LOG_DEBUG,
                    "[mroonga][count-skip][true] "
                    "only one full text search condition");
            DBUG_RETURN(true);
          } else {
            GRN_LOG(ctx_, GRN_LOG_DEBUG,
                    "[mroonga][count-skip][false] "
                    "full text search condition and more conditions: %u",
                    select_lex_->select_n_where_fields);
            DBUG_RETURN(false);
          }
        } else {
          skippable = is_skippable(func_item);
          if (skippable) {
            GRN_LOG(ctx_, GRN_LOG_DEBUG,
                    "[mroonga][count-skip][true] skippable condition");
          }
        }
      }
      break;
    default:
      GRN_LOG(ctx_, GRN_LOG_DEBUG,
              "[mroonga][count-skip][false] unsupported top level item: %u",
              where->type());
      break;
    }

    DBUG_RETURN(skippable);
  }

  bool CountSkipChecker::is_skippable(Item_cond *cond_item) {
    MRN_DBUG_ENTER_METHOD();

    List_iterator<Item> iterator(*(cond_item->argument_list()));
    Item *sub_item;
    while ((sub_item = iterator++)) {
      if (sub_item->type() != Item::FUNC_ITEM) {
        GRN_LOG(ctx_, GRN_LOG_DEBUG,
                "[mroonga][count-skip][false] "
                "sub condition isn't function item: %u",
                sub_item->type());
        DBUG_RETURN(false);
      }
      if (!is_skippable(static_cast<Item_func *>(sub_item))) {
        DBUG_RETURN(false);
      }
    }
    DBUG_RETURN(true);
  }

  bool CountSkipChecker::is_skippable(Item_func *func_item) {
    MRN_DBUG_ENTER_METHOD();

    switch (func_item->functype()) {
    case Item_func::EQ_FUNC:
    case Item_func::EQUAL_FUNC:
    case Item_func::NE_FUNC:
    case Item_func::LT_FUNC:
    case Item_func::LE_FUNC:
    case Item_func::GE_FUNC:
    case Item_func::GT_FUNC:
      {
        Item **arguments = func_item->arguments();
        Item *left_item = arguments[0];
        if (left_item->type() != Item::FIELD_ITEM) {
          GRN_LOG(ctx_, GRN_LOG_DEBUG,
                  "[mroonga][count-skip][false] not field: %u:%u",
                  func_item->functype(),
                  left_item->type());
          DBUG_RETURN(false);
        }

        bool skippable = is_skippable(static_cast<Item_field *>(left_item));
        DBUG_RETURN(skippable);
      }
      break;
    case Item_func::BETWEEN:
      {
        Item **arguments = func_item->arguments();
        Item *target_item = arguments[0];
        if (target_item->type() != Item::FIELD_ITEM) {
          GRN_LOG(ctx_, GRN_LOG_DEBUG,
                  "[mroonga][count-skip][false] BETWEEN target isn't field: %u",
                  target_item->type());
          DBUG_RETURN(false);
        }

        bool skippable = is_skippable(static_cast<Item_field *>(target_item));
        DBUG_RETURN(skippable);
      }
      break;
    case Item_func::MULT_EQUAL_FUNC:
#ifdef MRN_HAVE_ITEM_EQUAL_FIELDS_ITERATOR
      {
        Item_equal *equal_item = static_cast<Item_equal *>(func_item);
        Item_equal_fields_iterator iterator(*equal_item);
        Item *field_item;
        while ((field_item = iterator++)) {
          bool skippable = is_skippable(static_cast<Item_field *>(field_item));
          if (!skippable) {
            DBUG_RETURN(skippable);
          }
        }
        DBUG_RETURN(true);
      }
#endif
      break;
    default:
      break;
    }

    GRN_LOG(ctx_, GRN_LOG_DEBUG,
            "[mroonga][count-skip][false] unsupported function item: %u",
            func_item->functype());
    DBUG_RETURN(false);
  }

  bool CountSkipChecker::is_skippable(Item_field *field_item) {
    MRN_DBUG_ENTER_METHOD();

    Field *field = field_item->field;
    if (!field) {
      GRN_LOG(ctx_, GRN_LOG_DEBUG,
              "[mroonga][count-skip][false] field is missing");
      DBUG_RETURN(false);
    }

    if (field->table != table_) {
      GRN_LOG(ctx_, GRN_LOG_DEBUG,
              "[mroonga][count-skip][false] external table's field");
      DBUG_RETURN(false);
    }

    if (!key_info_) {
      GRN_LOG(ctx_, GRN_LOG_DEBUG,
              "[mroonga][count-skip][false] no active index: <%s>:<%s>",
              *(field->table_name),
              field->field_name.str);
      DBUG_RETURN(false);
    }

    uint i;
    KEY_PART_INFO *key_part = key_info_->key_part;
    for (i = 0; i < KEY_N_KEY_PARTS(key_info_); i++) {
      if (key_part[i].field == field) {
        if ((target_key_part_map_ >> i) & 1) {
          DBUG_RETURN(true);
        } else {
          GRN_LOG(ctx_, GRN_LOG_DEBUG,
                  "[mroonga][count-skip][false] "
                  "field's index are out of key part map: %u:%lu: <%s>:<%s>",
                  i,
                  target_key_part_map_,
                  *(field->table_name),
                  field->field_name.str);
          DBUG_RETURN(false);
        }
      }
    }

    GRN_LOG(ctx_, GRN_LOG_DEBUG,
            "[mroonga][count-skip][false] field isn't indexed: <%s>:<%s>",
            *(field->table_name),
            field->field_name.str);
    DBUG_RETURN(false);
  }
}
