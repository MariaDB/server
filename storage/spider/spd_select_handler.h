/* Copyright (C) 2022  MariaDB Corporation.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE.  See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA  02111-1307  USA */

#include "select_handler.h"

select_handler *spider_create_select_handler(THD *thd, SELECT_LEX *sel);

class ha_spider_select_handler: public select_handler
{
public:
  ha_spider_select_handler(THD *thd, SELECT_LEX *sel);
  ~ha_spider_select_handler();
  int init_scan();
  int next_row();
  int end_scan();
  void print_error(int, unsigned long);
};
