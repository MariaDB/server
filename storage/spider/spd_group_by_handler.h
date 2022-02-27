/* Copyright (C) 2016 Kentoku Shiba

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

class spider_group_by_handler: public group_by_handler
{
  Query query;
  spider_fields *fields;
  ha_spider *spider;
  SPIDER_TRX *trx;
  spider_db_result *result;
  bool first;
  longlong offset_limit;
  int store_error;

public:
  spider_group_by_handler(
    THD *thd_arg,
    Query *query_arg,
    spider_fields *fields_arg
  );
  ~spider_group_by_handler();
  int init_scan();
  int next_row();
  int end_scan();
};

group_by_handler *spider_create_group_by_handler(
  THD *thd,
  Query *query
);
