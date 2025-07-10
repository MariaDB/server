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
  /*
    Bitmap marking constant items among the select items. They are
    SELECTed in the query executed at the data node, but not stored in
    SPIDER_DB_ROW, because the temp table do not contain the
    corresponding fields.
  */
  MY_BITMAP skips;

public:
  spider_group_by_handler(
    THD *thd_arg,
    Query *query_arg,
    spider_fields *fields_arg,
    const MY_BITMAP &skips1
  );
  ~spider_group_by_handler();
  int init_scan() override;
  int next_row() override;
  int end_scan() override;
};

group_by_handler *spider_create_group_by_handler(
  THD *thd,
  Query *query
);

/* TODO: move this somewhere else so that spider_select_handler does
  not depend on gbh */
SPIDER_TABLE_HOLDER *spider_create_table_holder(
  uint table_count_arg
);

SPIDER_TABLE_HOLDER *spider_add_table_holder(
  ha_spider *spider_arg,
  SPIDER_TABLE_HOLDER *table_holder
);

int spider_make_query(const Query& query, spider_fields* fields,
                      ha_spider *spider, TABLE *table);

int spider_prepare_init_scan(
  const Query& query, MY_BITMAP *skips, spider_fields *fields, ha_spider *spider,
  SPIDER_TRX *trx, longlong& offset_limit, THD *thd);
