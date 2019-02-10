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

#include "derived_handler.h"
#include "select_handler.h"

/*
  Implementation class of the derived_handler interface for FEDERATEDX:
  class declaration
*/

class ha_federatedx_derived_handler: public derived_handler
{
private:
  FEDERATEDX_SHARE *share;
  federatedx_txn *txn;
  federatedx_io **iop;
  FEDERATEDX_IO_RESULT *stored_result;

public:
  ha_federatedx_derived_handler(THD* thd_arg, TABLE_LIST *tbl);
  ~ha_federatedx_derived_handler();
  int init_scan();
  int next_row();
  int end_scan();
  void print_error(int, unsigned long);
};


/*
  Implementation class of the select_handler interface for FEDERATEDX:
  class declaration
*/

class ha_federatedx_select_handler: public select_handler
{
private:
  FEDERATEDX_SHARE *share;
  federatedx_txn *txn;
  federatedx_io **iop;
  FEDERATEDX_IO_RESULT *stored_result;

public:
  ha_federatedx_select_handler(THD* thd_arg, SELECT_LEX *sel);
  ~ha_federatedx_select_handler();
  int init_scan();
  int next_row();
  int end_scan();
  void print_error(int, unsigned long);
};
