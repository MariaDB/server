/* Copyright (C) 2018-2019 Kentoku Shiba
   Copyright (C) 2018-2019 MariaDB corp

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

void spider_free_rewrite_table_subpartitions(
  SPIDER_RWTBLSPTT *info
);

void spider_free_rewrite_table_partitions(
  SPIDER_RWTBLPTT *info
);

void spider_free_rewrite_table_tables(
  SPIDER_RWTBLTBL *info
);

void spider_free_rewrite_tables(
  SPIDER_RWTBL *info
);

void spider_free_rewrite_cache(
  DYNAMIC_ARRAY *rw_table_cache
);

bool spider_load_rewrite_table_subpartitions(
  THD *thd,
  MEM_ROOT *mem_root,
  TABLE_LIST *tables,
  SPIDER_RWTBLPTT *rwtblptt
);

bool spider_load_rewrite_table_partitions(
  THD *thd,
  MEM_ROOT *mem_root,
  TABLE_LIST *tables,
  SPIDER_RWTBLTBL *rwtbltbl
);

bool spider_load_rewrite_table_tables(
  THD *thd,
  MEM_ROOT *mem_root,
  TABLE_LIST *tables,
  SPIDER_RWTBL *rwtbl
);

bool spider_init_rewrite_cache(
  THD *thd
);

SPIDER_RWTBL *spider_rewrite_table_cache_compare(
  const LEX_CSTRING *db_name,
  const LEX_CSTRING *table_name,
  const struct charset_info_st *cs
);

int spider_rewrite_insert_rewritten_tables(
  THD *thd,
  LEX_CSTRING *schema_name,
  LEX_CSTRING *table_name,
  const struct charset_info_st *cs,
  SPIDER_RWTBL *rwtbl
);

int spider_rewrite_parse(
  THD *thd,
  mysql_event_query_rewrite *ev,
  spider_parse_sql **parse_sql_p
);

int spider_rewrite_parse_create(
  spider_parse_sql *parse_sql
);

int spider_rewrite_parse_create_table(
  spider_parse_sql *parse_sql
);

int spider_rewrite_parse_nest_of_paren(
  spider_parse_sql *parse_sql,
  int &retval,
  union YYSTYPE &yylval
);

int spider_rewrite_parse_nest_of_paren_for_data_nodes(
  spider_parse_sql *parse_sql,
  int &retval,
  union YYSTYPE &yylval
);

int spider_rewrite_parse_column_definition(
  spider_parse_sql *parse_sql,
  int &retval,
  union YYSTYPE &yylval
);

int spider_rewrite_parse_index_definition(
  spider_parse_sql *parse_sql,
  int &retval,
  union YYSTYPE &yylval
);

int spider_rewrite_parse_period_definition(
  spider_parse_sql *parse_sql,
  int &retval,
  union YYSTYPE &yylval
);

int spider_rewrite_parse_check_definition(
  spider_parse_sql *parse_sql,
  int &retval,
  union YYSTYPE &yylval
);

int spider_rewrite_parse_create_table_select_statement(
  spider_parse_sql *parse_sql,
  int &retval,
  union YYSTYPE &yylval
);

int spider_rewrite_parse_interval(
  spider_parse_sql *parse_sql,
  int &retval,
  union YYSTYPE &yylval
);

int spider_rewrite_parse_create_table_table_option(
  spider_parse_sql *parse_sql,
  int &retval,
  union YYSTYPE &yylval
);

int spider_rewrite_parse_create_table_partition(
  spider_parse_sql *parse_sql,
  int &retval,
  union YYSTYPE &yylval
);
