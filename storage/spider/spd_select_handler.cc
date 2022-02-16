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

#include "spd_select_handler.h"

extern handlerton *spider_hton_ptr;

select_handler *spider_create_select_handler(THD *thd, SELECT_LEX *sel)
{
  return NULL;
}

ha_spider_select_handler::ha_spider_select_handler(THD *thd, SELECT_LEX *sel)
    : select_handler(thd, spider_hton_ptr)
{
  select= sel;
}

ha_spider_select_handler::~ha_spider_select_handler() {}

int ha_spider_select_handler::init_scan() { return 0; }

int ha_spider_select_handler::next_row() { return 0; }

int ha_spider_select_handler::end_scan() { return 0; }

void ha_spider_select_handler::print_error(int, unsigned long){};
