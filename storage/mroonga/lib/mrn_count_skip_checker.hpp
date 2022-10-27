/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2016 Kouhei Sutou <kou@clear-code.com>

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

#ifndef MRN_COUNT_SKIP_CHECKER_HPP_
#define MRN_COUNT_SKIP_CHECKER_HPP_

#include <mrn_mysql_compat.h>

#include <item_cmpfunc.h>

#include <groonga.h>

namespace mrn {
  class CountSkipChecker {
  public:
    CountSkipChecker(grn_ctx *ctx,
                     TABLE *table,
                     SELECT_LEX *select_lex,
                     KEY *key_info,
                     key_part_map target_key_part_map,
                     bool is_storage_mode);
    ~CountSkipChecker();

    bool check();

  private:
    grn_ctx *ctx_;
    TABLE *table_;
    SELECT_LEX *select_lex_;
    KEY *key_info_;
    key_part_map target_key_part_map_;
    bool is_storage_mode_;

    bool is_skippable(Item *where);
    bool is_skippable(Item_cond *cond_item);
    bool is_skippable(Item_func *func_item);
    bool is_skippable(Item_field *field_item);
  };
}

#endif /* MRN_COUNT_SKIP_CHECKER_HPP_ */
