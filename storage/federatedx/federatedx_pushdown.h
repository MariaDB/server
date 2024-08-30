/*
   Copyright (c) 2019 MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "sql_string.h"
#include "derived_handler.h"
#include "select_handler.h"

class federatedx_handler_base
{
protected:
  FEDERATEDX_SHARE *share;
  federatedx_txn *txn;
  federatedx_io **iop;
  FEDERATEDX_IO_RESULT *stored_result;
  StringBuffer<512> query;
  
  TABLE *query_table;

  int next_row_(TABLE *tmp_tbl);
  int init_scan_();
  int end_scan_();
public:
  federatedx_handler_base(THD *thd_arg, TABLE *tbl_arg);
};

/*
  Implementation class of the derived_handler interface for FEDERATEDX:
  class declaration
*/

class ha_federatedx_derived_handler: public derived_handler, public federatedx_handler_base
{
private:

public:
  ha_federatedx_derived_handler(THD* thd_arg, TABLE_LIST *tbl, TABLE *tbl_arg);
  ~ha_federatedx_derived_handler();
  int init_scan() override { return federatedx_handler_base::init_scan_(); }
  int next_row() override { return federatedx_handler_base::next_row_(table); }
  int end_scan() override { return federatedx_handler_base::end_scan_(); }
};


/*
  Implementation class of the select_handler interface for FEDERATEDX:
  class declaration
*/

class ha_federatedx_select_handler: public select_handler, public federatedx_handler_base
{
public:
  ha_federatedx_select_handler(THD *thd_arg, SELECT_LEX_UNIT *sel_unit,
                               TABLE *tbl);
  ha_federatedx_select_handler(THD *thd_arg, SELECT_LEX *sel_lex,
                               SELECT_LEX_UNIT *sel_unit, TABLE *tbl);
  ~ha_federatedx_select_handler();
  int init_scan() override { return federatedx_handler_base::init_scan_(); }
  int next_row() override { return federatedx_handler_base::next_row_(table); }
  int end_scan() override;

private:
  static constexpr auto PRINT_QUERY_TYPE=
      enum_query_type(QT_VIEW_INTERNAL | QT_SELECT_ONLY |
                      QT_ITEM_ORIGINAL_FUNC_NULLIF | QT_PARSABLE);
};
