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

#define MYSQL_SERVER 1
#define MYSQL_LEX 1
#include <my_global.h>
#include "mysql_version.h"
#include "spd_environ.h"
#if MYSQL_VERSION_ID >= 50500
#include "sql_priv.h"
#include "probes_mysql.h"
#include "sql_class.h"
#include "sql_partition.h"
#include "spd_err.h"
#include "spd_db_include.h"
#include "spd_include.h"
#include "ha_spider.h"
#include "spd_trx.h"
#include "spd_db_conn.h"
#include "spd_table.h"
#include "spd_conn.h"
#include "spd_malloc.h"
#include "spd_parse.h"

extern SPIDER_DBTON spider_dbton[SPIDER_DBTON_SIZE];
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
extern HASH spider_open_tables;
#endif

LEX_CSTRING spider_ident_back_quote = {STRING_WITH_LEN("`")};
LEX_CSTRING spider_ident_double_quote = {STRING_WITH_LEN("\"")};

spider_parse_sql::spider_parse_sql() :
  flags(0), found_semicolon(NULL), parser_state_backup(NULL), work_str(NULL),
  spider(NULL), spider_last(NULL), trx(NULL), field(NULL), zero_bitmap(NULL)
{
  DBUG_ENTER("spider_parse_sql::spider_parse_sql");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_VOID_RETURN;
}

spider_parse_sql::~spider_parse_sql()
{
  spider_db_sql *db_sql_tmp;
  DBUG_ENTER("spider_parse_sql::~spider_parse_sql");
  DBUG_PRINT("info",("spider this=%p", this));
  if (flags & SPIDER_PARSE_PARSER_STATE_BACKUPED)
  {
    thd->m_parser_state = parser_state_backup;
  }
  if (spider)
  {
    ha_spider *tmp = spider;
    do {
      ha_spider *next = tmp->next;
      SPIDER_SHARE *share = tmp->share;
      spider_free_spider_object_for_share_with_sql_string(&tmp);
      spider_free_share_resource_only(share);
      tmp = next;
    } while (tmp);
  }
  if (trx)
  {
    trx->thd = NULL;
    spider_free_trx(trx, TRUE);
  }
  if (work_str)
  {
    delete [] work_str;
  }
  while ((db_sql_tmp = db_sql))
  {
    db_sql = db_sql->next;
    delete db_sql_tmp;
  }
  if (zero_bitmap)
  {
    my_bitmap_free(zero_bitmap);
  }
  DBUG_VOID_RETURN;
}

int spider_parse_sql::init(
  THD *thd_arg,
  char *query_arg,
  uint query_length,
  const struct charset_info_st *query_charset,
  ulonglong query_id
) {
  int error_num, roop_count;
  cs = query_charset;
  spider_db_sql *tmp;
  DBUG_ENTER("spider_parse_sql::init");
  DBUG_PRINT("info",("spider this=%p", this));
  thd = thd_arg;
  query = query_arg;
  query_len = query_length;
  parser_state_backup = thd->m_parser_state;
  flags = SPIDER_PARSE_PARSER_STATE_BACKUPED;
  thd->m_parser_state = &parser_state;
  if (parser_state.init(thd, query, query_length))
  {
    error_num = HA_ERR_OUT_OF_MEM;
    goto error;
  }
  parser_state.m_digest_psi = NULL;
  parser_state.m_lip.m_digest = NULL;
  if (thd->variables.sql_mode & MODE_ORACLE)
  {
    DBlex = ORAlex;
    if (unlikely(!(db_sql = spider_oracle_create_sql())))
    {
      error_num = HA_ERR_OUT_OF_MEM;
      goto error;
    }
  } else {
    DBlex = MYSQLlex;
    if (unlikely(!(db_sql = spider_mariadb_create_sql())))
    {
      error_num = HA_ERR_OUT_OF_MEM;
      goto error;
    }
  }
  if (unlikely((error_num = db_sql->init(cs))))
  {
    goto error;
  }
  if (thd->variables.sql_mode & MODE_ANSI_QUOTES)
  {
    db_sql->set_quote_char_for_ident(spider_ident_double_quote);
  } else {
    db_sql->set_quote_char_for_ident(spider_ident_back_quote);
  }
  tmp = db_sql;
  for (roop_count = 0; roop_count < (int) SPIDER_DBTON_SIZE; ++roop_count)
  {
    if (!spider_dbton[roop_count].db_util)
    {
      break;
    }
    if (spider_dbton[roop_count].db_access_type ==
      SPIDER_DB_ACCESS_TYPE_NOSQL)
    {
      continue;
    }
    if (unlikely(!(tmp->next = (spider_db_sql *) spider_dbton[roop_count].
      create_db_sql())))
    {
      error_num = HA_ERR_OUT_OF_MEM;
      goto error;
    }
    tmp = tmp->next;
    if (unlikely((error_num = tmp->init(cs))))
    {
      goto error;
    }
    db_sql_by_id[roop_count] = tmp;
  }
  table_share.path.str = "";
  table_share.path.length = 0;
  table_share.normalized_path.str = "";
  table_share.normalized_path.length = 0;
  table_share.partition_info_str = (char *) "";
  table_share.table_charset = NULL;
  table_share.fields = 0;
  table_share.keys = 0;
  table_share.field = &field;
  table_share.key_info = NULL;
  table_share.tmp_table = INTERNAL_TMP_TABLE;
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
  hash_value = my_calc_hash(&spider_open_tables, (uchar*) "", 0);
#endif
  if (
    part_info.partitions.push_back(&part_p_elem) ||
    sub_part_info.partitions.push_back(&sub_part_p_elem) ||
    sub_part_p_elem.subpartitions.push_back(&sub_part_sub_p_elem)
  ) {
    error_num = HA_ERR_OUT_OF_MEM;
    goto error;
  }
  if (unlikely(!(work_str = new spider_string[2])))
  {
    error_num = HA_ERR_OUT_OF_MEM;
    goto error;
  }
  for (roop_count = 0; roop_count < 2; ++roop_count)
  {
    work_str[roop_count].init_calc_mem(263);
    work_str[roop_count].set_charset(cs);
  }
  zero_bitmap = &table_share.all_set;
  if (my_bitmap_init(zero_bitmap, NULL, 0, FALSE))
  {
    zero_bitmap = NULL;
    error_num = HA_ERR_OUT_OF_MEM;
    goto error;
  }
  bitmap_set_all(zero_bitmap);
  DBUG_RETURN(0);

error:
  if (work_str)
  {
    delete [] work_str;
    work_str = NULL;
  }
  while ((tmp = db_sql))
  {
    db_sql = db_sql->next;
    delete tmp;
  }
  thd->m_parser_state = parser_state_backup;
  flags &= ~SPIDER_PARSE_PARSER_STATE_BACKUPED;
  DBUG_RETURN(error_num);
}

void spider_parse_sql::reset(
  char *query_arg,
  uint query_length,
  const struct charset_info_st *query_charset,
  ulonglong query_id
) {
  cs = query_charset;
  spider_db_sql *tmp;
  DBUG_ENTER("spider_parse_sql::reset");
  DBUG_PRINT("info",("spider this=%p", this));
  query = query_arg;
  query_len = query_length;
  parser_state_backup = thd->m_parser_state;
  flags = SPIDER_PARSE_PARSER_STATE_BACKUPED;
  thd->m_parser_state = &parser_state;
  parser_state.reset(query, query_len);
  parser_state.m_digest_psi = NULL;
  parser_state.m_lip.m_digest = NULL;
  table_share.table_charset = NULL;
  if (thd->variables.sql_mode & MODE_ORACLE)
  {
    DBlex = ORAlex;
  } else {
    DBlex = MYSQLlex;
  }
  db_sql->reset(cs);
  if (thd->variables.sql_mode & MODE_ANSI_QUOTES)
  {
    db_sql->set_quote_char_for_ident(spider_ident_double_quote);
  } else {
    db_sql->set_quote_char_for_ident(spider_ident_back_quote);
  }
  tmp = db_sql->next;
  while (tmp)
  {
    tmp->reset(cs);
    tmp = tmp->next;
  }
  if (spider)
  {
    ha_spider *tmp_spider = spider;
    do {
      ha_spider *next = tmp_spider->next;
      SPIDER_SHARE *share = tmp_spider->share;
      spider_free_spider_object_for_share_with_sql_string(&tmp_spider);
      spider_free_share_resource_only(share);
      tmp_spider = next;
    } while (tmp_spider);
    spider = NULL;
    spider_last = NULL;
  }
  DBUG_VOID_RETURN;
}

void spider_parse_sql::end_parse()
{
  DBUG_ENTER("spider_parse_sql::end_parse");
  DBUG_PRINT("info",("spider this=%p", this));
  if (flags & SPIDER_PARSE_PARSER_STATE_BACKUPED)
  {
    spider_db_sql *tmp = db_sql;
    do {
      tmp->set_sql_end_pos();
    } while ((tmp = tmp->next));
    found_semicolon = parser_state.m_lip.found_semicolon;
    thd->m_parser_state = parser_state_backup;
    flags &= ~SPIDER_PARSE_PARSER_STATE_BACKUPED;
  }
  DBUG_VOID_RETURN;
}

const char *spider_parse_sql::get_found_semicolon()
{
  DBUG_ENTER("spider_parse_sql::get_found_semicolon");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(found_semicolon);
}

int spider_parse_sql::get_next(
  union YYSTYPE *yylval
) {
  DBUG_ENTER("spider_parse_sql::get_next");
  DBUG_PRINT("info",("spider this=%p", this));
  get_next_val = DBlex(yylval, thd);
  DBUG_PRINT("info",("spider get_next_val=%d", get_next_val));
  if (unlikely(get_next_val <= 0))
  {
    get_next_val = END_OF_INPUT;
  }
  DBUG_RETURN(get_next_val);
}

void spider_parse_sql::push_syntax_error(
  const char *near_by
) {
  DBUG_ENTER("spider_parse_sql::push_syntax_error");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_PRINT("info",("spider get_next_val=%d", get_next_val));
  Lex_input_stream *lip= &parser_state.m_lip;
  if (!near_by && !(near_by = lip->get_tok_start()))
  {
    near_by = "";
  }
  ErrConvString ecs(near_by, strlen(near_by),
    thd->variables.character_set_client);
  my_printf_error(ER_SPIDER_SYNTAX_NUM,  ER_SPIDER_SYNTAX_STR, MYF(0),
    "Spider Rewrite Plugin", ecs.ptr(), lip->yylineno);
#ifndef DBUG_OFF
  union YYSTYPE yylval;
  get_next(&yylval);
#endif
  DBUG_VOID_RETURN;
}

void spider_parse_sql::push_error(
  int error_num
) {
  DBUG_ENTER("spider_parse_sql::push_error");
  DBUG_PRINT("info",("spider this=%p", this));
  if (thd->is_error())
  {
    /* nothing to do */
    DBUG_VOID_RETURN;
  }
  switch (error_num)
  {
    case HA_ERR_OUT_OF_MEM:
      my_error(HA_ERR_OUT_OF_MEM, MYF(0));
      break;
    case ER_SPIDER_TOO_LONG_NUM:
      my_printf_error(ER_SPIDER_TOO_LONG_NUM,  ER_SPIDER_TOO_LONG_STR, MYF(0),
        error_str_piece);
      break;
    default:
      my_printf_error(ER_SPIDER_UNKNOWN_NUM,  ER_SPIDER_UNKNOWN_STR2, MYF(0),
        error_num, "Spider Rewrite Plugin");
      break;
  }
  DBUG_VOID_RETURN;
}

int spider_parse_sql::append_parsed_symbol(
  int symbol_tok,
  union YYSTYPE *yylval_tok
) {
  int error_num;
  spider_db_sql *tmp;
  DBUG_ENTER("spider_parse_sql::append_parsed_symbol");
  DBUG_PRINT("info",("spider this=%p", this));
  tmp = db_sql;
  do {
    if ((error_num = tmp->append_parsed_symbol(symbol_tok, yylval_tok)))
    {
      DBUG_RETURN(error_num);
    }
  } while ((tmp = tmp->next));
  DBUG_RETURN(0);
}

int spider_parse_sql::append_parsed_symbol_for_data_nodes(
  int symbol_tok,
  union YYSTYPE *yylval_tok
) {
  int error_num;
  spider_db_sql *tmp;
  DBUG_ENTER("spider_parse_sql::append_parsed_symbol_for_data_nodes");
  DBUG_PRINT("info",("spider this=%p", this));
  tmp = db_sql;
  while ((tmp = tmp->next))
  {
    if ((error_num = tmp->append_parsed_symbol(symbol_tok, yylval_tok)))
    {
      DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}

int spider_parse_sql::append_parsed_symbol_for_spider_nodes(
  int symbol_tok,
  union YYSTYPE *yylval_tok
) {
  DBUG_ENTER("spider_parse_sql::append_parsed_symbol_for_spider_nodes");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(db_sql->append_parsed_symbol(symbol_tok, yylval_tok));
}

int spider_parse_sql::append_parsed_symbol_for_spider_nodes_ex(
  int symbol_tok,
  union YYSTYPE *yylval_tok
) {
  DBUG_ENTER("spider_parse_sql::append_parsed_symbol_for_spider_nodes_ex");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(db_sql->append_parsed_symbol_ex(symbol_tok, yylval_tok));
}

char *spider_parse_sql::get_query_for_spider_node(
  uint *query_length
) {
  DBUG_ENTER("spider_parse_sql::get_query_for_spider_node");
  DBUG_PRINT("info",("spider this=%p", this));
  *query_length = db_sql->sql_str[0].length();
  DBUG_RETURN((char *) db_sql->sql_str[0].c_ptr_safe());
}

void spider_parse_sql::set_query_id(
  ulonglong query_id_arg
) {
  DBUG_ENTER("spider_parse_sql::set_query_id");
  DBUG_PRINT("info",("spider this=%p", this));
  query_id = query_id_arg;
  DBUG_VOID_RETURN;
}

ulonglong spider_parse_sql::get_query_id()
{
  DBUG_ENTER("spider_parse_sql::get_query_id");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(query_id);
}

void spider_parse_sql::set_schema_name(
  LEX_CSTRING &name
) {
  DBUG_ENTER("spider_parse_sql::set_schema_name");
  DBUG_PRINT("info",("spider this=%p", this));
  schema_name = name;
  table_share.db = name;
  DBUG_VOID_RETURN;
}

void spider_parse_sql::set_table_name(
  LEX_CSTRING &name
) {
  spider_db_sql *tmp;
  DBUG_ENTER("spider_parse_sql::set_table_name");
  DBUG_PRINT("info",("spider this=%p", this));
  table_name = name;
  table_share.table_name = name;
  tmp = db_sql;
  do {
    tmp->append_table_name_space();
  } while ((tmp = tmp->next));
  DBUG_VOID_RETURN;
}

int spider_parse_sql::set_create_or_replace()
{
  int error_num;
  spider_db_sql *tmp;
  DBUG_ENTER("spider_parse_sql::set_create_or_replace");
  DBUG_PRINT("info",("spider this=%p", this));
  tmp = db_sql;
  do {
    if ((error_num = tmp->append_create_or_replace()))
    {
      DBUG_RETURN(error_num);
    }
  } while ((tmp = tmp->next));
  flags |= SPIDER_PARSE_CREATE_OR_REPLACE;
  DBUG_RETURN(0);
}

int spider_parse_sql::append_create_or_replace_table()
{
  int error_num;
  spider_db_sql *tmp;
  DBUG_ENTER("spider_parse_sql::append_create_or_replace_table");
  DBUG_PRINT("info",("spider this=%p", this));
  if (flags & SPIDER_PARSE_CREATE_OR_REPLACE)
  {
    tmp = db_sql;
    while ((tmp = tmp->next))
    {
      if ((error_num = tmp->append_create_or_replace_table()))
      {
        DBUG_RETURN(error_num);
      }
    }
  }
  DBUG_RETURN(0);
}

int spider_parse_sql::append_if_not_exists()
{
  int error_num;
  spider_db_sql *tmp;
  DBUG_ENTER("spider_parse_sql::append_if_not_exists");
  DBUG_PRINT("info",("spider this=%p", this));
  tmp = db_sql;
  do {
    if ((error_num = tmp->append_if_not_exists()))
    {
      DBUG_RETURN(error_num);
    }
  } while ((tmp = tmp->next));
  DBUG_RETURN(0);
}

int spider_parse_sql::append_table_option_name_for_data_nodes(
  int symbol_tok,
  union YYSTYPE *yylval_tok
) {
  int error_num;
  spider_db_sql *tmp;
  DBUG_ENTER("spider_parse_sql::append_table_option_name_for_data_nodes");
  DBUG_PRINT("info",("spider this=%p", this));
  tmp = db_sql;
  while ((tmp = tmp->next))
  {
    if ((error_num = tmp->append_table_option_name(symbol_tok, yylval_tok)))
    {
      DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}

int spider_parse_sql::append_table_option_name(
  int symbol_tok,
  union YYSTYPE *yylval_tok
) {
  int error_num;
  spider_db_sql *tmp;
  DBUG_ENTER("spider_parse_sql::append_table_option_name");
  DBUG_PRINT("info",("spider this=%p", this));
  tmp = db_sql;
  do {
    if ((error_num = tmp->append_table_option_name(symbol_tok, yylval_tok)))
    {
      DBUG_RETURN(error_num);
    }
  } while ((tmp = tmp->next));
  DBUG_RETURN(0);
}

int spider_parse_sql::append_table_option_value_for_data_nodes(
  int symbol_tok,
  union YYSTYPE *yylval_tok
) {
  int error_num;
  spider_db_sql *tmp;
  DBUG_ENTER("spider_parse_sql::append_table_option_value_for_data_nodes");
  DBUG_PRINT("info",("spider this=%p", this));
  tmp = db_sql;
  while ((tmp = tmp->next))
  {
    if ((error_num = tmp->append_table_option_value(symbol_tok, yylval_tok)))
    {
      DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}

int spider_parse_sql::append_table_option_value(
  int symbol_tok,
  union YYSTYPE *yylval_tok
) {
  int error_num;
  spider_db_sql *tmp;
  DBUG_ENTER("spider_parse_sql::append_table_option_value");
  DBUG_PRINT("info",("spider this=%p", this));
  tmp = db_sql;
  do {
    if ((error_num = tmp->append_table_option_value(symbol_tok, yylval_tok)))
    {
      DBUG_RETURN(error_num);
    }
  } while ((tmp = tmp->next));
  if (flags & SPIDER_PARSE_CHARSET_NOTICE)
  {
    char csname[MY_CS_NAME_SIZE + 1];
    DBUG_ASSERT(MY_CS_NAME_SIZE >= yylval_tok->lex_str.length);
    memcpy(csname, yylval_tok->lex_str.str, yylval_tok->lex_str.length);
    csname[yylval_tok->lex_str.length] = '\0';
    table_share.table_charset = get_charset_by_csname(
      csname, MY_CS_PRIMARY, MYF(MY_WME));
    flags &= ~SPIDER_PARSE_CHARSET_NOTICE;
  }
  DBUG_RETURN(0);
}

int spider_parse_sql::append_table_option_character_set()
{
  int error_num;
  spider_db_sql *tmp;
  DBUG_ENTER("spider_parse_sql::append_table_option_character_set");
  DBUG_PRINT("info",("spider this=%p", this));
  tmp = db_sql;
  do {
    if ((error_num = tmp->append_table_option_character_set()))
    {
      DBUG_RETURN(error_num);
    }
  } while ((tmp = tmp->next));
  flags |= SPIDER_PARSE_CHARSET_NOTICE;
  DBUG_RETURN(0);
}

int spider_parse_sql::append_table_option_data_directory_for_data_nodes()
{
  int error_num;
  spider_db_sql *tmp;
  DBUG_ENTER("spider_parse_sql::append_table_option_data_directory_for_data_nodes");
  DBUG_PRINT("info",("spider this=%p", this));
  tmp = db_sql;
  while ((tmp = tmp->next))
  {
    if ((error_num = tmp->append_table_option_data_directory()))
    {
      DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}

int spider_parse_sql::append_table_option_index_directory_for_data_nodes()
{
  int error_num;
  spider_db_sql *tmp;
  DBUG_ENTER("spider_parse_sql::append_table_option_index_directory_for_data_nodes");
  DBUG_PRINT("info",("spider this=%p", this));
  tmp = db_sql;
  while ((tmp = tmp->next))
  {
    if ((error_num = tmp->append_table_option_index_directory()))
    {
      DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}

int spider_parse_sql::append_table_option_with_system_versioning_for_data_nodes()
{
  int error_num;
  spider_db_sql *tmp;
  DBUG_ENTER("spider_parse_sql::append_table_option_with_system_versioning_for_data_nodes");
  DBUG_PRINT("info",("spider this=%p", this));
  tmp = db_sql;
  while ((tmp = tmp->next))
  {
    if ((error_num = tmp->append_table_option_with_system_versioning()))
    {
      DBUG_RETURN(error_num);
    }
  }
  DBUG_RETURN(0);
}

int spider_parse_sql::append_spider_table_for_spider_nodes(
  SPIDER_RWTBLTBL *rwtbltbl
) {
  int error_num;
  DBUG_ENTER("spider_parse_sql::append_spider_table_for_spider_nodes");
  DBUG_PRINT("info",("spider this=%p", this));
  if ((error_num = db_sql->append_table_name(&schema_name, &table_name)))
  {
    DBUG_RETURN(error_num);
  }
  DBUG_RETURN(db_sql->append_spider_table(&table_name, rwtbltbl));
}

int spider_parse_sql::create_share_from_table(
  SPIDER_RWTBLTBL *rwtbltbl
) {
  int error_num;
  SPIDER_RWTBLPTT *tp;
  SPIDER_SHARE *share;
  DBUG_ENTER("spider_parse_sql::create_share_from_table");
  DBUG_PRINT("info",("spider this=%p", this));
  if (!trx && !(trx = spider_get_trx(NULL, FALSE, &error_num)))
  {
    DBUG_RETURN(error_num);
  }
  trx->thd = thd;
  table_share.comment = rwtbltbl->comment_str;
  if (!rwtbltbl->partition_method.length)
  {
    /* no partition definition */
    spider_string *str = &work_str[1];
    str->length(0);
    if (str->reserve(rwtbltbl->connection_str.length + table_name.length * 2 +
      SPIDER_SQL_TABLE_LEN + 4))
    {
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    }
    str->q_append(SPIDER_SQL_TABLE_STR, SPIDER_SQL_TABLE_LEN);
    str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
    str->append_for_single_quote(table_name.str, table_name.length);
    str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
    if (rwtbltbl->connection_str.length)
    {
      str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
      str->q_append(rwtbltbl->connection_str.str,
        rwtbltbl->connection_str.length);
    }
    table_share.connect_string.str = str->c_ptr_safe();
    table_share.connect_string.length = str->length();
    if (!(share = spider_create_share("", &table_share,
#ifdef WITH_PARTITION_STORAGE_ENGINE
      NULL,
#endif
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
      hash_value,
#endif
      &error_num)))
    {
      DBUG_RETURN(error_num);
    }
    ha_spider *tmp_spider = NULL;
    if ((error_num = spider_create_spider_object_for_share_with_sql_string(
      trx, share, &tmp_spider)))
    {
      spider_free_share_resource_only(share);
      DBUG_RETURN(error_num);
    }
    if (spider_last)
    {
      spider_last->next = tmp_spider;
    } else {
      spider = tmp_spider;
    }
    spider_last = tmp_spider;
    spider_last->next = NULL;
    DBUG_PRINT("info",("spider tmp_spider->share=%p", tmp_spider->share));
    DBUG_RETURN(0);
  }
  table_share.connect_string = rwtbltbl->connection_str;
  tp = rwtbltbl->tp;
  while (tp)
  {
    if ((error_num = create_share_from_partition(rwtbltbl, tp)))
    {
      DBUG_RETURN(error_num);
    }
    tp = tp->next;
  }
  DBUG_RETURN(0);
}

int spider_parse_sql::create_share_from_partition(
  SPIDER_RWTBLTBL *rwtbltbl,
  SPIDER_RWTBLPTT *rwtblptt
) {
  int error_num;
  SPIDER_SHARE *share;
  SPIDER_RWTBLSPTT *ts;
  DBUG_ENTER("spider_parse_sql::create_share_from_partition");
  DBUG_PRINT("info",("spider this=%p", this));
  if (!rwtblptt->ts)
  {
    /* no subpartition definition */
    char tmp_name[FN_REFLEN + 1];
    part_p_elem.part_comment = rwtblptt->comment_str.str;
#ifdef SPIDER_PARTITION_HAS_CONNECTION_STRING
    spider_string *str = &work_str[1];
    str->length(0);
    if (str->reserve(rwtblptt->connection_str.length + table_name.length * 2 +
      SPIDER_SQL_TABLE_LEN + 4))
    {
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    }
    str->q_append(SPIDER_SQL_TABLE_STR, SPIDER_SQL_TABLE_LEN);
    str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
    str->append_for_single_quote(table_name.str, table_name.length);
    str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
    if (rwtblptt->connection_str.length)
    {
      str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
      str->q_append(rwtblptt->connection_str.str,
        rwtblptt->connection_str.length);
    }
    part_p_elem.connect_string.str = str->c_ptr_safe();
    part_p_elem.connect_string.length = str->length();
#endif
    part_p_elem.partition_name = rwtblptt->partition_name.str;
    if ((error_num = SPIDER_create_partition_name(
      tmp_name, FN_REFLEN + 1, table_share.path.str,
      part_p_elem.partition_name, NORMAL_PART_NAME, TRUE)))
    {
      if (error_num == HA_WRONG_CREATE_OPTION)
      {
        error_num = ER_SPIDER_TOO_LONG_NUM;
        error_str_piece = "Table name + partition name";
      }
      DBUG_RETURN(error_num);
    }
    if (!(share = spider_create_share(tmp_name, &table_share,
#ifdef WITH_PARTITION_STORAGE_ENGINE
      &part_info,
#endif
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
      hash_value,
#endif
      &error_num)))
    {
      DBUG_RETURN(error_num);
    }
    ha_spider *tmp_spider = NULL;
    if ((error_num = spider_create_spider_object_for_share_with_sql_string(
      trx, share, &tmp_spider)))
    {
      spider_free_share_resource_only(share);
      DBUG_RETURN(error_num);
    }
    if (spider_last)
    {
      spider_last->next = tmp_spider;
    } else {
      spider = tmp_spider;
    }
    spider_last = tmp_spider;
    spider_last->next = NULL;
    DBUG_PRINT("info",("spider tmp_spider->share=%p", tmp_spider->share));
    DBUG_RETURN(0);
  }
  sub_part_p_elem.part_comment = rwtblptt->comment_str.str;
#ifdef SPIDER_PARTITION_HAS_CONNECTION_STRING
  sub_part_p_elem.connect_string = rwtblptt->connection_str;
#endif
  sub_part_p_elem.partition_name = rwtblptt->partition_name.str;
  ts = rwtblptt->ts;
  while (ts)
  {
    if ((error_num = create_share_from_subpartition(rwtbltbl, ts)))
    {
      DBUG_RETURN(error_num);
    }
    ts = ts->next;
  }
  DBUG_RETURN(0);
}

int spider_parse_sql::create_share_from_subpartition(
  SPIDER_RWTBLTBL *rwtbltbl,
  SPIDER_RWTBLSPTT *rwtblsptt
) {
  int error_num;
  char tmp_name[FN_REFLEN + 1];
  SPIDER_SHARE *share;
  DBUG_ENTER("spider_parse_sql::create_share_from_subpartition");
  DBUG_PRINT("info",("spider this=%p", this));
  sub_part_sub_p_elem.part_comment = rwtblsptt->comment_str.str;
#ifdef SPIDER_PARTITION_HAS_CONNECTION_STRING
  spider_string *str = &work_str[1];
  str->length(0);
  if (str->reserve(rwtblsptt->connection_str.length + table_name.length * 2 +
    SPIDER_SQL_TABLE_LEN + 4))
  {
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  str->q_append(SPIDER_SQL_TABLE_STR, SPIDER_SQL_TABLE_LEN);
  str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
  str->append_for_single_quote(table_name.str, table_name.length);
  str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
  if (rwtblsptt->connection_str.length)
  {
    str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
    str->q_append(rwtblsptt->connection_str.str,
      rwtblsptt->connection_str.length);
  }
  sub_part_sub_p_elem.connect_string.str = str->c_ptr_safe();
  sub_part_sub_p_elem.connect_string.length = str->length();
#endif
  sub_part_sub_p_elem.partition_name = rwtblsptt->subpartition_name.str;
  if ((error_num = SPIDER_create_subpartition_name(
    tmp_name, FN_REFLEN + 1, table_share.path.str,
    sub_part_p_elem.partition_name, sub_part_sub_p_elem.partition_name,
    NORMAL_PART_NAME)))
  {
    if (error_num == HA_WRONG_CREATE_OPTION)
    {
      error_num = ER_SPIDER_TOO_LONG_NUM;
      error_str_piece = "Table name + partition name + subpartition name";
    }
    DBUG_RETURN(error_num);
  }
  if (!(share = spider_create_share(tmp_name, &table_share,
#ifdef WITH_PARTITION_STORAGE_ENGINE
    &sub_part_info,
#endif
#ifdef SPIDER_HAS_HASH_VALUE_TYPE
    hash_value,
#endif
    &error_num)))
  {
    DBUG_RETURN(error_num);
  }
  ha_spider *tmp_spider = NULL;
  if ((error_num = spider_create_spider_object_for_share_with_sql_string(
    trx, share, &tmp_spider)))
  {
    spider_free_share_resource_only(share);
    DBUG_RETURN(error_num);
  }
  if (spider_last)
  {
    spider_last->next = tmp_spider;
  } else {
    spider = tmp_spider;
  }
  spider_last = tmp_spider;
  spider_last->next = NULL;
  DBUG_PRINT("info",("spider tmp_spider->share=%p", tmp_spider->share));
  DBUG_RETURN(0);
}

int spider_parse_sql::get_conn()
{
  int error_num;
  ha_spider *tmp;
  DBUG_ENTER("spider_parse_sql::get_conn");
  DBUG_PRINT("info",("spider this=%p", this));
  tmp = spider;
  do {
    uint roop_count;
    SPIDER_SHARE *share = tmp->share;
    for (roop_count = 0; roop_count < share->all_link_count; ++roop_count)
    {
      if (!spider_get_conn(share, roop_count, share->conn_keys[roop_count],
        trx, tmp, FALSE, FALSE, SPIDER_CONN_KIND_MYSQL, &error_num))
      {
        DBUG_RETURN(error_num);
      }
    }
  } while ((tmp = tmp->next));
  DBUG_RETURN(0);
}

int spider_parse_sql::send_sql_to_data_nodes()
{
  int error_num;
  ha_spider *tmp;
  DBUG_ENTER("spider_parse_sql::send_sql_to_data_nodes");
  DBUG_PRINT("info",("spider this=%p", this));
  tmp = spider;
  do {
    uint roop_count;
    SPIDER_SHARE *share = tmp->share;
#ifndef WITHOUT_SPIDER_BG_SEARCH
    if ((error_num = spider_set_conn_bg_param(tmp)))
      goto end;
#endif
    for (roop_count = 0; roop_count < share->all_link_count; ++roop_count)
    {
      SPIDER_CONN *conn = tmp->conns[roop_count];
      spider_db_handler *dbton_hdl = tmp->dbton_handler[conn->dbton_id];
      if ((error_num =
        dbton_hdl->set_sql_for_exec(db_sql_by_id[conn->dbton_id], roop_count)))
      {
        goto end;
      }
#ifndef WITHOUT_SPIDER_BG_SEARCH
      if (tmp->result_list.bgs_phase > 0)
      {
        if ((error_num = spider_check_and_init_casual_read(thd, tmp,
          roop_count)))
        {
          goto end;
        }
        conn = tmp->conns[roop_count];
        pthread_mutex_lock(&conn->bg_conn_mutex);
        conn->bg_target = tmp;
        conn->bg_error_num = &tmp->need_mons[roop_count];
        conn->bg_sql_type = SPIDER_SQL_TYPE_DDL_SQL;
        conn->link_idx = roop_count;
        conn->bg_exec_sql = TRUE;
        conn->bg_caller_sync_wait = TRUE;
        pthread_mutex_lock(&conn->bg_conn_sync_mutex);
        pthread_cond_signal(&conn->bg_conn_cond);
        pthread_mutex_unlock(&conn->bg_conn_mutex);
        pthread_cond_wait(&conn->bg_conn_sync_cond, &conn->bg_conn_sync_mutex);
        pthread_mutex_unlock(&conn->bg_conn_sync_mutex);
        conn->bg_caller_sync_wait = FALSE;
      } else {
#endif
        pthread_mutex_lock(&conn->mta_conn_mutex);
        SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
        conn->need_mon = &tmp->need_mons[roop_count];
        conn->mta_conn_mutex_lock_already = TRUE;
        conn->mta_conn_mutex_unlock_later = TRUE;
        conn->link_idx = roop_count;
        error_num = spider_db_query_with_set_names(
          SPIDER_SQL_TYPE_DDL_SQL, tmp, conn, roop_count);
        conn->mta_conn_mutex_lock_already = FALSE;
        conn->mta_conn_mutex_unlock_later = FALSE;
        SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
        pthread_mutex_unlock(&conn->mta_conn_mutex);
        if (error_num)
        {
          goto end;
        }
#ifndef WITHOUT_SPIDER_BG_SEARCH
      }
#endif
    }
  } while ((tmp = tmp->next));

end:
#ifndef WITHOUT_SPIDER_BG_SEARCH
  tmp = spider;
  do {
    if (tmp->result_list.bgs_phase > 0)
    {
      uint roop_count;
      SPIDER_SHARE *share = tmp->share;
      for (roop_count = 0; roop_count < share->all_link_count; ++roop_count)
      {
        SPIDER_CONN *conn = tmp->conns[roop_count];
        if (conn->bg_exec_sql)
        {
          /* wait */
          pthread_mutex_lock(&conn->bg_conn_mutex);
          pthread_mutex_unlock(&conn->bg_conn_mutex);
        }
        if (tmp->need_mons[roop_count])
        {
          error_num = tmp->need_mons[roop_count];
        }
      }
      tmp->result_list.bgs_phase = 0;
    }
  } while ((tmp = tmp->next));
#endif
  DBUG_RETURN(error_num);
}
#endif
