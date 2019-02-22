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

#define SPIDER_PARSE_PARSER_STATE_BACKUPED (1 << 0)
#define SPIDER_PARSE_CREATE_OR_REPLACE     (1 << 1)
#define SPIDER_PARSE_CHARSET_NOTICE        (1 << 2)

class spider_parse_sql
{
public:
  uint flags, query_len;
  int get_next_val;
  char *query;
  const char *found_semicolon;
  const char *error_str_piece;
  Parser_state parser_state;
  Parser_state *parser_state_backup;
  THD *thd;
  spider_db_sql *db_sql;
  spider_db_sql *db_sql_by_id[SPIDER_DBTON_SIZE];
  ulonglong query_id;
  LEX_CSTRING schema_name;
  LEX_CSTRING table_name;
  const struct charset_info_st *cs;
  spider_string *work_str;
  int (*DBlex)(union YYSTYPE *yylval, THD *thd);

  ha_spider *spider, *spider_last;
  SPIDER_TRX *trx;
  TABLE_SHARE table_share;
  Field *field;
  partition_info part_info, sub_part_info;
  partition_element part_p_elem, sub_part_p_elem, sub_part_sub_p_elem;
  MY_BITMAP *zero_bitmap;
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
  my_hash_value_type hash_value;
#endif
  spider_parse_sql();
  ~spider_parse_sql();
  int init(
    THD *thd_arg,
    char *query_arg,
    uint query_length,
    const struct charset_info_st *query_charset,
    ulonglong query_id
  );
  void reset(
    char *query_arg,
    uint query_length,
    const struct charset_info_st *query_charset,
    ulonglong query_id
  );
  void end_parse();
  const char *get_found_semicolon();
  int get_next(
    union YYSTYPE *yylval
  );
  void push_syntax_error(
    const char *near_by
  );
  void push_error(
    int error_num
  );
  int append_parsed_symbol(
    int symbol_tok,
    union YYSTYPE *yylval_tok
  );
  int append_parsed_symbol_for_data_nodes(
    int symbol_tok,
    union YYSTYPE *yylval_tok
  );
  int append_parsed_symbol_for_spider_nodes(
    int symbol_tok,
    union YYSTYPE *yylval_tok
  );
  int append_parsed_symbol_for_spider_nodes_ex(
    int symbol_tok,
    union YYSTYPE *yylval_tok
  );
  char *get_query_for_spider_node(
    uint *query_length
  );
  void set_query_id(
    ulonglong query_id_arg
  );
  ulonglong get_query_id();
  void set_schema_name(
    LEX_CSTRING &name
  );
  void set_table_name(
    LEX_CSTRING &name
  );
  int set_create_or_replace();
  int append_create_or_replace_table();
  int append_if_not_exists();
  int append_table_option_name_for_data_nodes(
    int symbol_tok,
    union YYSTYPE *yylval_tok
  );
  int append_table_option_name(
    int symbol_tok,
    union YYSTYPE *yylval_tok
  );
  int append_table_option_value_for_data_nodes(
    int symbol_tok,
    union YYSTYPE *yylval_tok
  );
  int append_table_option_value(
    int symbol_tok,
    union YYSTYPE *yylval_tok
  );
  int append_table_option_character_set();
  int append_table_option_data_directory_for_data_nodes();
  int append_table_option_index_directory_for_data_nodes();
  int append_table_option_with_system_versioning_for_data_nodes();
  int append_spider_table_for_spider_nodes(
    SPIDER_RWTBLTBL *rwtbltbl
  );
  int create_share_from_table(
    SPIDER_RWTBLTBL *rwtbltbl
  );
  int create_share_from_partition(
    SPIDER_RWTBLTBL *rwtbltbl,
    SPIDER_RWTBLPTT *rwtblptt
  );
  int create_share_from_subpartition(
    SPIDER_RWTBLTBL *rwtbltbl,
    SPIDER_RWTBLSPTT *rwtblsptt
  );
  int get_conn();
  int send_sql_to_data_nodes();
};
