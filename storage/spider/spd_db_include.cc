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
#if MYSQL_VERSION_ID < 50500
#include "mysql_priv.h"
#include <mysql/plugin.h>
#else
#include "sql_priv.h"
#include "probes_mysql.h"
#include "sql_class.h"
#include "sql_lex.h"
#endif
#include "spd_err.h"
#include "spd_db_include.h"
#include "spd_include.h"
#include "spd_db_conn.h"
#include "spd_malloc.h"

spider_db_sql::~spider_db_sql()
{
  DBUG_ENTER("spider_db_sql::~spider_db_sql");
  DBUG_PRINT("info",("spider this=%p", this));
  if (likely(sql_init))
  {
    delete [] sql_str;
  }
  DBUG_VOID_RETURN;
}

int spider_db_sql::init(
  CHARSET_INFO *charset
) {
  uint roop_count;
  DBUG_ENTER("spider_db_sql::init");
  if (unlikely(!(sql_str = new spider_string[SPIDER_DB_SQL_STRINGS])))
  {
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  for (roop_count = 0; roop_count < SPIDER_DB_SQL_STRINGS; ++roop_count)
  {
    sql_str[roop_count].init_calc_mem(257);
    sql_str[roop_count].set_charset(charset);
  }
  sql_init = TRUE;
  DBUG_RETURN(0);
}

void spider_db_sql::reset(
  CHARSET_INFO *charset
) {
  uint roop_count;
  DBUG_ENTER("spider_db_sql::init");
  for (roop_count = 0; roop_count < SPIDER_DB_SQL_STRINGS; ++roop_count)
  {
    sql_str[roop_count].length(0);
    sql_str[roop_count].set_charset(charset);
  }
  DBUG_VOID_RETURN;
}

void spider_db_sql::set_quote_char_for_ident(
  LEX_CSTRING quote_char
) {
  DBUG_ENTER("spider_db_sql::set_quote_char_for_ident");
  quote_char_for_ident = quote_char;
  DBUG_VOID_RETURN;
}

int spider_db_sql::append_parsed_symbol(
  int symbol_tok,
  union YYSTYPE *yylval_tok
) {
  DBUG_ENTER("spider_db_sql::append_parsed_symbol");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(append_parsed_symbol(symbol_tok, yylval_tok, &sql_str[0]));
}

int spider_db_sql::append_parsed_symbol_ex(
  int symbol_tok,
  union YYSTYPE *yylval_tok
) {
  DBUG_ENTER("spider_db_sql::append_parsed_symbol_ex");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(append_parsed_symbol(symbol_tok, yylval_tok, &sql_str[1]));
}

int spider_db_sql::append_table_name_space(
  uint str_id
) {
  spider_string *str = &sql_str[str_id];
  DBUG_ENTER("spider_mysql_sql::append_table_name_space");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_PRINT("info",("spider str_id=%u", str_id));
  DBUG_PRINT("info",("spider table_name_pos=%u", str->length()));
  table_name_pos[str_id] = str->length();
  if (str->reserve(SPIDER_TABLE_NAME_SPACE))
  {
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  memset((char *) str->ptr() + str->length(), ' ', SPIDER_TABLE_NAME_SPACE);
  str->length(str->length() + SPIDER_TABLE_NAME_SPACE);
  DBUG_RETURN(0);
}

int spider_db_sql::append_table_name_space()
{
  DBUG_ENTER("spider_mysql_sql::append_table_name_space");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(append_table_name_space(0));
}

void spider_db_sql::set_sql_end_pos()
{
  DBUG_ENTER("spider_mysql_sql::set_sql_end_pos");
  DBUG_PRINT("info",("spider this=%p", this));
  sql_end_pos[0] = sql_str[0].length();
  sql_end_pos[1] = sql_str[1].length();
  DBUG_VOID_RETURN;
}

int spider_db_sql::append_parsed_symbol(
  int symbol_tok,
  union YYSTYPE *yylval_tok,
  spider_string *str
) {
  DBUG_ENTER("spider_db_sql::append_parsed_symbol");
  if (symbol_tok < 256)
  {
    uchar single_char = (uchar) symbol_tok;
    if (str->append((char *) &single_char, 1))
    {
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    }
    DBUG_RETURN(0);
  }
  switch (symbol_tok)
  {
    case AND_AND_SYM:
      DBUG_PRINT("info",("spider AND_AND_SYM"));
      if (str->append(STRING_WITH_LEN("&& "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case LE:
      DBUG_PRINT("info",("spider LE"));
      if (str->append(STRING_WITH_LEN("<= "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case NE:
      DBUG_PRINT("info",("spider NE"));
      if (str->append(STRING_WITH_LEN("!= "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#ifdef SPIDER_TOKEN_10_3
    case NEG:
      DBUG_PRINT("info",("spider NEG"));
      if (str->append(STRING_WITH_LEN("!"))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#endif
    case GE:
      DBUG_PRINT("info",("spider GE"));
      if (str->append(STRING_WITH_LEN(">= "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case SHIFT_LEFT:
      DBUG_PRINT("info",("spider SHIFT_LEFT"));
      if (str->append(STRING_WITH_LEN("<< "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case SHIFT_RIGHT:
      DBUG_PRINT("info",("spider SHIFT_RIGHT"));
      if (str->append(STRING_WITH_LEN(">> "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case EQUAL_SYM:
      DBUG_PRINT("info",("spider EQUAL_SYM"));
      if (str->append(STRING_WITH_LEN("<=> "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case ACCESSIBLE_SYM:
      DBUG_PRINT("info",("spider ACCESSIBLE_SYM"));
      if (str->append(STRING_WITH_LEN("ACCESSIBLE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#ifdef SPIDER_TOKEN_10_4
    case ACCOUNT_SYM:
      DBUG_PRINT("info",("spider ACCOUNT_SYM"));
      if (str->append(STRING_WITH_LEN("ACCOUNT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#endif
    case ACTION:
      DBUG_PRINT("info",("spider ACTION"));
      if (str->append(STRING_WITH_LEN("ACTION "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case ADD:
      DBUG_PRINT("info",("spider ADD"));
      if (str->append(STRING_WITH_LEN("ADD "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case ADMIN_SYM:
      DBUG_PRINT("info",("spider ADMIN_SYM"));
      if (str->append(STRING_WITH_LEN("ADMIN "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case AFTER_SYM:
      DBUG_PRINT("info",("spider AFTER_SYM"));
      if (str->append(STRING_WITH_LEN("AFTER "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case AGAINST:
      DBUG_PRINT("info",("spider AGAINST"));
      if (str->append(STRING_WITH_LEN("AGAINST "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case AGGREGATE_SYM:
      DBUG_PRINT("info",("spider AGGREGATE_SYM"));
      if (str->append(STRING_WITH_LEN("AGGREGATE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case ALL: /* ALL */
      DBUG_PRINT("info",("spider ALL"));
      if (str->append(STRING_WITH_LEN("ALL "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case ALGORITHM_SYM:
      DBUG_PRINT("info",("spider ALGORITHM_SYM"));
      if (str->append(STRING_WITH_LEN("ALGORITHM "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case ALTER:
      DBUG_PRINT("info",("spider ALTER"));
      if (str->append(STRING_WITH_LEN("ALTER "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case ALWAYS_SYM:
      DBUG_PRINT("info",("spider ALWAYS_SYM"));
      if (str->append(STRING_WITH_LEN("ALWAYS "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case ANALYZE_SYM:
      DBUG_PRINT("info",("spider ANALYZE_SYM"));
      if (str->append(STRING_WITH_LEN("ANALYZE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case AND_SYM:
      DBUG_PRINT("info",("spider AND_SYM"));
      if (str->append(STRING_WITH_LEN("AND "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case ANY_SYM:
      DBUG_PRINT("info",("spider ANY_SYM"));
      if (str->append(STRING_WITH_LEN("ANY "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case AS:
      DBUG_PRINT("info",("spider AS"));
      if (str->append(STRING_WITH_LEN("AS "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case ASC:
      DBUG_PRINT("info",("spider ASC"));
      if (str->append(STRING_WITH_LEN("ASC "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case ASCII_SYM:
      DBUG_PRINT("info",("spider ASCII_SYM"));
      if (str->append(STRING_WITH_LEN("ASCII "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case ASENSITIVE_SYM:
      DBUG_PRINT("info",("spider ASENSITIVE_SYM"));
      if (str->append(STRING_WITH_LEN("ASENSITIVE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case AT_SYM:
      DBUG_PRINT("info",("spider AT_SYM"));
      if (str->append(STRING_WITH_LEN("AT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case ATOMIC_SYM:
      DBUG_PRINT("info",("spider ATOMIC_SYM"));
      if (str->append(STRING_WITH_LEN("ATOMIC "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case AUTHORS_SYM:
      DBUG_PRINT("info",("spider AUTHORS_SYM"));
      if (str->append(STRING_WITH_LEN("AUTHORS "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case AUTO_INC:
      DBUG_PRINT("info",("spider AUTO_INC"));
      if (str->append(STRING_WITH_LEN("AUTO_INCREMENT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case AUTOEXTEND_SIZE_SYM:
      DBUG_PRINT("info",("spider AUTOEXTEND_SIZE_SYM"));
      if (str->append(STRING_WITH_LEN("AUTOEXTEND_SIZE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case AUTO_SYM:
      DBUG_PRINT("info",("spider AUTO_SYM"));
      if (str->append(STRING_WITH_LEN("AUTO "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case AVG_SYM:
      DBUG_PRINT("info",("spider AVG_SYM"));
      if (str->append(STRING_WITH_LEN("AVG "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case AVG_ROW_LENGTH:
      DBUG_PRINT("info",("spider AVG_ROW_LENGTH"));
      if (str->append(STRING_WITH_LEN("AVG_ROW_LENGTH "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case BACKUP_SYM:
      DBUG_PRINT("info",("spider BACKUP_SYM"));
      if (str->append(STRING_WITH_LEN("BACKUP "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case BEFORE_SYM:
      DBUG_PRINT("info",("spider BEFORE_SYM"));
      if (str->append(STRING_WITH_LEN("BEFORE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#ifdef SPIDER_TOKEN_10_3
    case BEGIN_MARIADB_SYM:
      DBUG_PRINT("info",("spider BEGIN_MARIADB_SYM"));
      if (str->append(STRING_WITH_LEN("BEGIN "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case BEGIN_ORACLE_SYM:
      DBUG_PRINT("info",("spider BEGIN_ORACLE_SYM"));
      if (str->append(STRING_WITH_LEN("BEGIN "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#else
    case BEGIN_SYM:
      DBUG_PRINT("info",("spider BEGIN_SYM"));
      if (str->append(STRING_WITH_LEN("BEGIN "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#endif
    case BETWEEN_SYM:
      DBUG_PRINT("info",("spider BETWEEN_SYM"));
      if (str->append(STRING_WITH_LEN("BETWEEN "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case BIGINT:
      DBUG_PRINT("info",("spider BIGINT"));
      if (str->append(STRING_WITH_LEN("BIGINT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case BINARY:
      DBUG_PRINT("info",("spider BINARY"));
      if (str->append(STRING_WITH_LEN("BINARY "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case BINLOG_SYM:
      DBUG_PRINT("info",("spider BINLOG_SYM"));
      if (str->append(STRING_WITH_LEN("BINLOG "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case BIT_SYM:
      DBUG_PRINT("info",("spider BIT_SYM"));
      if (str->append(STRING_WITH_LEN("BIT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#ifdef SPIDER_TOKEN_10_3
    case BLOB_MARIADB_SYM:
      DBUG_PRINT("info",("spider BLOB_MARIADB_SYM"));
      if (str->append(STRING_WITH_LEN("BLOB "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case BLOB_ORACLE_SYM:
      DBUG_PRINT("info",("spider BLOB_ORACLE_SYM"));
      if (str->append(STRING_WITH_LEN("BLOB "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#else
    case BLOB_SYM:
      DBUG_PRINT("info",("spider BLOB_SYM"));
      if (str->append(STRING_WITH_LEN("BLOB "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#endif
    case BLOCK_SYM:
      DBUG_PRINT("info",("spider BLOCK_SYM"));
      if (str->append(STRING_WITH_LEN("BLOCK "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#ifdef SPIDER_TOKEN_10_3
    case BODY_MARIADB_SYM:
      DBUG_PRINT("info",("spider BODY_MARIADB_SYM"));
      if (str->append(STRING_WITH_LEN("BODY "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case BODY_ORACLE_SYM:
      DBUG_PRINT("info",("spider BODY_ORACLE_SYM"));
      if (str->append(STRING_WITH_LEN("BODY "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#else
    case BODY_SYM:
      DBUG_PRINT("info",("spider BODY_SYM"));
      if (str->append(STRING_WITH_LEN("BODY "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#endif
    case BOOL_SYM:
      DBUG_PRINT("info",("spider BOOL_SYM"));
      if (str->append(STRING_WITH_LEN("BOOL "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case BOOLEAN_SYM:
      DBUG_PRINT("info",("spider BOOLEAN_SYM"));
      if (str->append(STRING_WITH_LEN("BOOLEAN "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case BOTH:
      DBUG_PRINT("info",("spider BOTH"));
      if (str->append(STRING_WITH_LEN("BOTH "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case BTREE_SYM:
      DBUG_PRINT("info",("spider BTREE_SYM"));
      if (str->append(STRING_WITH_LEN("BTREE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case BY:
      DBUG_PRINT("info",("spider BY"));
      if (str->append(STRING_WITH_LEN("BY "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case BYTE_SYM:
      DBUG_PRINT("info",("spider BYTE_SYM"));
      if (str->append(STRING_WITH_LEN("BYTE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case CACHE_SYM:
      DBUG_PRINT("info",("spider CACHE_SYM"));
      if (str->append(STRING_WITH_LEN("CACHE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case CALL_SYM:
      DBUG_PRINT("info",("spider CALL_SYM"));
      if (str->append(STRING_WITH_LEN("CALL "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case CASCADE:
      DBUG_PRINT("info",("spider CASCADE"));
      if (str->append(STRING_WITH_LEN("CASCADE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case CASCADED:
      DBUG_PRINT("info",("spider CASCADED"));
      if (str->append(STRING_WITH_LEN("CASCADED "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case CASE_SYM:
      DBUG_PRINT("info",("spider CASE_SYM"));
      if (str->append(STRING_WITH_LEN("CASE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case CATALOG_NAME_SYM:
      DBUG_PRINT("info",("spider CATALOG_NAME_SYM"));
      if (str->append(STRING_WITH_LEN("CATALOG_NAME "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case CHAIN_SYM:
      DBUG_PRINT("info",("spider CHAIN_SYM"));
      if (str->append(STRING_WITH_LEN("CHAIN "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case CHANGE:
      DBUG_PRINT("info",("spider CHANGE"));
      if (str->append(STRING_WITH_LEN("CHANGE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case CHANGED:
      DBUG_PRINT("info",("spider CHANGED"));
      if (str->append(STRING_WITH_LEN("CHANGED "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case CHAR_SYM:
      DBUG_PRINT("info",("spider CHAR_SYM"));
      if (str->append(STRING_WITH_LEN("CHARACTER "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case CHARSET:
      DBUG_PRINT("info",("spider CHARSET"));
      if (str->append(STRING_WITH_LEN("CHARSET "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case CHECK_SYM:
      DBUG_PRINT("info",("spider CHECK_SYM"));
      if (str->append(STRING_WITH_LEN("CHECK "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case CHECKPOINT_SYM:
      DBUG_PRINT("info",("spider CHECKPOINT_SYM"));
      if (str->append(STRING_WITH_LEN("CHECKPOINT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case CHECKSUM_SYM:
      DBUG_PRINT("info",("spider CHECKSUM_SYM"));
      if (str->append(STRING_WITH_LEN("CHECKSUM "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case CIPHER_SYM:
      DBUG_PRINT("info",("spider CIPHER_SYM"));
      if (str->append(STRING_WITH_LEN("CIPHER "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case CLASS_ORIGIN_SYM:
      DBUG_PRINT("info",("spider CLASS_ORIGIN_SYM"));
      if (str->append(STRING_WITH_LEN("CLASS_ORIGIN "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case CLIENT_SYM:
      DBUG_PRINT("info",("spider CLIENT_SYM"));
      if (str->append(STRING_WITH_LEN("CLIENT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#ifdef SPIDER_TOKEN_10_3
    case CLOB_MARIADB_SYM:
      DBUG_PRINT("info",("spider CLOB_MARIADB_SYM"));
      if (str->append(STRING_WITH_LEN("CLOB "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case CLOB_ORACLE_SYM:
      DBUG_PRINT("info",("spider CLOB_ORACLE_SYM"));
      if (str->append(STRING_WITH_LEN("CLOB "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#else
    case CLOB:
      DBUG_PRINT("info",("spider CLOB"));
      if (str->append(STRING_WITH_LEN("CLOB "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#endif
    case CLOSE_SYM:
      DBUG_PRINT("info",("spider CLOSE_SYM"));
      if (str->append(STRING_WITH_LEN("CLOSE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case COALESCE:
      DBUG_PRINT("info",("spider COALESCE"));
      if (str->append(STRING_WITH_LEN("COALESCE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case CODE_SYM:
      DBUG_PRINT("info",("spider CODE_SYM"));
      if (str->append(STRING_WITH_LEN("CODE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case COLLATE_SYM:
      DBUG_PRINT("info",("spider COLLATE_SYM"));
      if (str->append(STRING_WITH_LEN("COLLATE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case COLLATION_SYM:
      DBUG_PRINT("info",("spider COLLATION_SYM"));
      if (str->append(STRING_WITH_LEN("COLLATION "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#ifdef SPIDER_TOKEN_10_3
    case COLON_ORACLE_SYM:
      DBUG_PRINT("info",("spider COLON_ORACLE_SYM"));
      if (str->append(STRING_WITH_LEN(":"))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#endif
    case COLUMN_SYM:
      DBUG_PRINT("info",("spider COLUMN_SYM"));
      if (str->append(STRING_WITH_LEN("COLUMN "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case COLUMN_NAME_SYM:
      DBUG_PRINT("info",("spider COLUMN_NAME_SYM"));
      if (str->append(STRING_WITH_LEN("COLUMN_NAME "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case COLUMNS:
      DBUG_PRINT("info",("spider COLUMNS"));
      if (str->append(STRING_WITH_LEN("COLUMNS "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case COLUMN_ADD_SYM:
      DBUG_PRINT("info",("spider COLUMN_ADD_SYM"));
      if (str->append(STRING_WITH_LEN("COLUMN_ADD "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case COLUMN_CHECK_SYM:
      DBUG_PRINT("info",("spider COLUMN_CHECK_SYM"));
      if (str->append(STRING_WITH_LEN("COLUMN_CHECK "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case COLUMN_CREATE_SYM:
      DBUG_PRINT("info",("spider COLUMN_CREATE_SYM"));
      if (str->append(STRING_WITH_LEN("COLUMN_CREATE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case COLUMN_DELETE_SYM:
      DBUG_PRINT("info",("spider COLUMN_DELETE_SYM"));
      if (str->append(STRING_WITH_LEN("COLUMN_DELETE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case COLUMN_GET_SYM:
      DBUG_PRINT("info",("spider COLUMN_GET_SYM"));
      if (str->append(STRING_WITH_LEN("COLUMN_GET "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case COMMENT_SYM:
      DBUG_PRINT("info",("spider COMMENT_SYM"));
      if (str->append(STRING_WITH_LEN("COMMENT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case COMMIT_SYM:
      DBUG_PRINT("info",("spider COMMIT_SYM"));
      if (str->append(STRING_WITH_LEN("COMMIT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case COMMITTED_SYM:
      DBUG_PRINT("info",("spider COMMITTED_SYM"));
      if (str->append(STRING_WITH_LEN("COMMITTED "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case COMPACT_SYM:
      DBUG_PRINT("info",("spider COMPACT_SYM"));
      if (str->append(STRING_WITH_LEN("COMPACT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case COMPLETION_SYM:
      DBUG_PRINT("info",("spider COMPLETION_SYM"));
      if (str->append(STRING_WITH_LEN("COMPLETION "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case COMPRESSED_SYM:
      DBUG_PRINT("info",("spider COMPRESSED_SYM"));
      if (str->append(STRING_WITH_LEN("COMPRESSED "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case CONCURRENT:
      DBUG_PRINT("info",("spider CONCURRENT"));
      if (str->append(STRING_WITH_LEN("CONCURRENT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case CONDITION_SYM:
      DBUG_PRINT("info",("spider CONDITION_SYM"));
      if (str->append(STRING_WITH_LEN("CONDITION "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case CONNECTION_SYM:
      DBUG_PRINT("info",("spider CONNECTION_SYM"));
      if (str->append(STRING_WITH_LEN("CONNECTION "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case CONSISTENT_SYM:
      DBUG_PRINT("info",("spider CONSISTENT_SYM"));
      if (str->append(STRING_WITH_LEN("CONSISTENT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case CONSTRAINT:
      DBUG_PRINT("info",("spider CONSTRAINT"));
      if (str->append(STRING_WITH_LEN("CONSTRAINT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case CONSTRAINT_CATALOG_SYM:
      DBUG_PRINT("info",("spider CONSTRAINT_CATALOG_SYM"));
      if (str->append(STRING_WITH_LEN("CONSTRAINT_CATALOG "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case CONSTRAINT_NAME_SYM:
      DBUG_PRINT("info",("spider CONSTRAINT_NAME_SYM"));
      if (str->append(STRING_WITH_LEN("CONSTRAINT_NAME "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case CONSTRAINT_SCHEMA_SYM:
      DBUG_PRINT("info",("spider CONSTRAINT_SCHEMA_SYM"));
      if (str->append(STRING_WITH_LEN("CONSTRAINT_SCHEMA "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case CONTAINS_SYM:
      DBUG_PRINT("info",("spider CONTAINS_SYM"));
      if (str->append(STRING_WITH_LEN("CONTAINS "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case CONTEXT_SYM:
      DBUG_PRINT("info",("spider CONTEXT_SYM"));
      if (str->append(STRING_WITH_LEN("CONTEXT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#ifdef SPIDER_TOKEN_10_3
    case CONTINUE_MARIADB_SYM:
      DBUG_PRINT("info",("spider CONTINUE_MARIADB_SYM"));
      if (str->append(STRING_WITH_LEN("CONTINUE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case CONTINUE_ORACLE_SYM:
      DBUG_PRINT("info",("spider CONTINUE_ORACLE_SYM"));
      if (str->append(STRING_WITH_LEN("CONTINUE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#else
    case CONTINUE_SYM:
      DBUG_PRINT("info",("spider CONTINUE_SYM"));
      if (str->append(STRING_WITH_LEN("CONTINUE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#endif
    case CONTRIBUTORS_SYM:
      DBUG_PRINT("info",("spider CONTRIBUTORS_SYM"));
      if (str->append(STRING_WITH_LEN("CONTRIBUTORS "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case CONVERT_SYM:
      DBUG_PRINT("info",("spider CONVERT_SYM"));
      if (str->append(STRING_WITH_LEN("CONVERT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case CPU_SYM:
      DBUG_PRINT("info",("spider CPU_SYM"));
      if (str->append(STRING_WITH_LEN("CPU "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case CREATE: /* CREATE */
      DBUG_PRINT("info",("spider CREATE"));
      if (str->append(STRING_WITH_LEN("CREATE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case CROSS:
      DBUG_PRINT("info",("spider CROSS"));
      if (str->append(STRING_WITH_LEN("CROSS "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case CUBE_SYM:
      DBUG_PRINT("info",("spider CUBE_SYM"));
      if (str->append(STRING_WITH_LEN("CUBE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case CURRENT_SYM:
      DBUG_PRINT("info",("spider CURRENT_SYM"));
      if (str->append(STRING_WITH_LEN("CURRENT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case CURDATE:
      DBUG_PRINT("info",("spider CURDATE"));
      if (str->append(STRING_WITH_LEN("CURRENT_DATE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case CURRENT_POS_SYM:
      DBUG_PRINT("info",("spider CURRENT_POS_SYM"));
      if (str->append(STRING_WITH_LEN("CURRENT_POS "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case CURRENT_ROLE:
      DBUG_PRINT("info",("spider CURRENT_ROLE"));
      if (str->append(STRING_WITH_LEN("CURRENT_ROLE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case CURTIME:
      DBUG_PRINT("info",("spider CURTIME"));
      if (str->append(STRING_WITH_LEN("CURRENT_TIME "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case NOW_SYM:
      DBUG_PRINT("info",("spider NOW_SYM"));
      if (str->append(STRING_WITH_LEN("CURRENT_TIMESTAMP "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case CURRENT_USER:
      DBUG_PRINT("info",("spider CURRENT_USER"));
      if (str->append(STRING_WITH_LEN("CURRENT_USER "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case CURSOR_SYM:
      DBUG_PRINT("info",("spider CURSOR_SYM"));
      if (str->append(STRING_WITH_LEN("CURSOR "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case CURSOR_NAME_SYM:
      DBUG_PRINT("info",("spider CURSOR_NAME_SYM"));
      if (str->append(STRING_WITH_LEN("CURSOR_NAME "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case CYCLE_SYM:
      DBUG_PRINT("info",("spider CYCLE_SYM"));
      if (str->append(STRING_WITH_LEN("CYCLE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case DATA_SYM:
      DBUG_PRINT("info",("spider DATA_SYM"));
      if (str->append(STRING_WITH_LEN("DATA "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case DATABASE:
      DBUG_PRINT("info",("spider DATABASE"));
      if (str->append(STRING_WITH_LEN("DATABASE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case DATABASES:
      DBUG_PRINT("info",("spider DATABASES"));
      if (str->append(STRING_WITH_LEN("DATABASES "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case DATAFILE_SYM:
      DBUG_PRINT("info",("spider DATAFILE_SYM"));
      if (str->append(STRING_WITH_LEN("DATAFILE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case DATE_SYM:
      DBUG_PRINT("info",("spider DATE_SYM"));
      if (str->append(STRING_WITH_LEN("DATE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case DATETIME:
      DBUG_PRINT("info",("spider DATETIME"));
      if (str->append(STRING_WITH_LEN("DATETIME "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case DAY_SYM:
      DBUG_PRINT("info",("spider DAY_SYM"));
      if (str->append(STRING_WITH_LEN("DAY "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case DAY_HOUR_SYM:
      DBUG_PRINT("info",("spider DAY_HOUR_SYM"));
      if (str->append(STRING_WITH_LEN("DAY_HOUR "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case DAY_MICROSECOND_SYM:
      DBUG_PRINT("info",("spider DAY_MICROSECOND_SYM"));
      if (str->append(STRING_WITH_LEN("DAY_MICROSECOND "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case DAY_MINUTE_SYM:
      DBUG_PRINT("info",("spider DAY_MINUTE_SYM"));
      if (str->append(STRING_WITH_LEN("DAY_MINUTE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case DAY_SECOND_SYM:
      DBUG_PRINT("info",("spider DAY_SECOND_SYM"));
      if (str->append(STRING_WITH_LEN("DAY_SECOND "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case DEALLOCATE_SYM :
      DBUG_PRINT("info",("spider DEALLOCATE_SYM "));
      if (str->append(STRING_WITH_LEN("DEALLOCATE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case DECIMAL_SYM:
      DBUG_PRINT("info",("spider DECIMAL_SYM"));
      if (str->append(STRING_WITH_LEN("DECIMAL "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#ifdef SPIDER_TOKEN_10_3
    case DECLARE_MARIADB_SYM:
      DBUG_PRINT("info",("spider DECLARE_MARIADB_SYM"));
      if (str->append(STRING_WITH_LEN("DECLARE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case DECLARE_ORACLE_SYM:
      DBUG_PRINT("info",("spider DECLARE_ORACLE_SYM"));
      if (str->append(STRING_WITH_LEN("DECLARE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#else
    case DECLARE_SYM:
      DBUG_PRINT("info",("spider DECLARE_SYM"));
      if (str->append(STRING_WITH_LEN("DECLARE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#endif
    case DEFAULT:
      DBUG_PRINT("info",("spider DEFAULT"));
      if (str->append(STRING_WITH_LEN("DEFAULT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case DEFINER_SYM:
      DBUG_PRINT("info",("spider DEFINER_SYM"));
      if (str->append(STRING_WITH_LEN("DEFINER "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case DELAYED_SYM:
      DBUG_PRINT("info",("spider DELAYED_SYM"));
      if (str->append(STRING_WITH_LEN("DELAYED "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case DELAY_KEY_WRITE_SYM:
      DBUG_PRINT("info",("spider DELAY_KEY_WRITE_SYM"));
      if (str->append(STRING_WITH_LEN("DELAY_KEY_WRITE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case DELETE_SYM:
      DBUG_PRINT("info",("spider DELETE_SYM"));
      if (str->append(STRING_WITH_LEN("DELETE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case DELETE_DOMAIN_ID_SYM:
      DBUG_PRINT("info",("spider DELETE_DOMAIN_ID_SYM"));
      if (str->append(STRING_WITH_LEN("DELETE_DOMAIN_ID "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case DESC:
      DBUG_PRINT("info",("spider DESC"));
      if (str->append(STRING_WITH_LEN("DESC "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case DESCRIBE:
      DBUG_PRINT("info",("spider DESCRIBE"));
      if (str->append(STRING_WITH_LEN("DESCRIBE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case DES_KEY_FILE:
      DBUG_PRINT("info",("spider DES_KEY_FILE"));
      if (str->append(STRING_WITH_LEN("DES_KEY_FILE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case DETERMINISTIC_SYM:
      DBUG_PRINT("info",("spider DETERMINISTIC_SYM"));
      if (str->append(STRING_WITH_LEN("DETERMINISTIC "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case DIAGNOSTICS_SYM:
      DBUG_PRINT("info",("spider DIAGNOSTICS_SYM"));
      if (str->append(STRING_WITH_LEN("DIAGNOSTICS "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case DIRECTORY_SYM:
      DBUG_PRINT("info",("spider DIRECTORY_SYM"));
      if (str->append(STRING_WITH_LEN("DIRECTORY "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case DISABLE_SYM:
      DBUG_PRINT("info",("spider DISABLE_SYM"));
      if (str->append(STRING_WITH_LEN("DISABLE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case DISCARD:
      DBUG_PRINT("info",("spider DISCARD"));
      if (str->append(STRING_WITH_LEN("DISCARD "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case DISK_SYM:
      DBUG_PRINT("info",("spider DISK_SYM"));
      if (str->append(STRING_WITH_LEN("DISK "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case DISTINCT:
      DBUG_PRINT("info",("spider DISTINCT"));
      if (str->append(STRING_WITH_LEN("DISTINCT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case DIV_SYM:
      DBUG_PRINT("info",("spider DIV_SYM"));
      if (str->append(STRING_WITH_LEN("DIV "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case DO_SYM:
      DBUG_PRINT("info",("spider DO_SYM"));
      if (str->append(STRING_WITH_LEN("DO "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#ifdef SPIDER_TOKEN_10_3
    case DOT_DOT_SYM:
      DBUG_PRINT("info",("spider DOT_DOT_SYM"));
      if (str->append(STRING_WITH_LEN(".."))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#endif
    case DOUBLE_SYM:
      DBUG_PRINT("info",("spider DOUBLE_SYM"));
      if (str->append(STRING_WITH_LEN("DOUBLE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case DO_DOMAIN_IDS_SYM:
      DBUG_PRINT("info",("spider DO_DOMAIN_IDS_SYM"));
      if (str->append(STRING_WITH_LEN("DO_DOMAIN_IDS "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case DROP:
      DBUG_PRINT("info",("spider DROP"));
      if (str->append(STRING_WITH_LEN("DROP "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case DUAL_SYM:
      DBUG_PRINT("info",("spider DUAL_SYM"));
      if (str->append(STRING_WITH_LEN("DUAL "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case DUMPFILE:
      DBUG_PRINT("info",("spider DUMPFILE"));
      if (str->append(STRING_WITH_LEN("DUMPFILE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case DUPLICATE_SYM:
      DBUG_PRINT("info",("spider DUPLICATE_SYM"));
      if (str->append(STRING_WITH_LEN("DUPLICATE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case DYNAMIC_SYM:
      DBUG_PRINT("info",("spider DYNAMIC_SYM"));
      if (str->append(STRING_WITH_LEN("DYNAMIC "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case EACH_SYM:
      DBUG_PRINT("info",("spider EACH_SYM"));
      if (str->append(STRING_WITH_LEN("EACH "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case ELSE:
      DBUG_PRINT("info",("spider ELSE"));
      if (str->append(STRING_WITH_LEN("ELSE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#ifdef SPIDER_TOKEN_10_3
    case ELSEIF_MARIADB_SYM:
      DBUG_PRINT("info",("spider ELSEIF_MARIADB_SYM"));
      if (str->append(STRING_WITH_LEN("ELSEIF "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case ELSEIF_ORACLE_SYM:
      DBUG_PRINT("info",("spider ELSEIF_ORACLE_SYM"));
      if (str->append(STRING_WITH_LEN("ELSEIF "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#else
    case ELSEIF_SYM:
      DBUG_PRINT("info",("spider ELSEIF_SYM"));
      if (str->append(STRING_WITH_LEN("ELSEIF "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#endif
#ifdef SPIDER_TOKEN_10_3
    case ELSIF_MARIADB_SYM:
      DBUG_PRINT("info",("spider ELSIF_MARIADB_SYM"));
      if (str->append(STRING_WITH_LEN("ELSIF "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case ELSIF_ORACLE_SYM:
      DBUG_PRINT("info",("spider ELSIF_ORACLE_SYM"));
      if (str->append(STRING_WITH_LEN("ELSIF "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#else
    case ELSIF_SYM:
      DBUG_PRINT("info",("spider ELSIF_SYM"));
      if (str->append(STRING_WITH_LEN("ELSIF "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#endif
    case ENABLE_SYM:
      DBUG_PRINT("info",("spider ENABLE_SYM"));
      if (str->append(STRING_WITH_LEN("ENABLE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case ENCLOSED:
      DBUG_PRINT("info",("spider ENCLOSED"));
      if (str->append(STRING_WITH_LEN("ENCLOSED "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case END:
      DBUG_PRINT("info",("spider END"));
      if (str->append(STRING_WITH_LEN("END "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case ENDS_SYM:
      DBUG_PRINT("info",("spider ENDS_SYM"));
      if (str->append(STRING_WITH_LEN("ENDS "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case ENGINE_SYM:
      DBUG_PRINT("info",("spider ENGINE_SYM"));
      if (str->append(STRING_WITH_LEN("ENGINE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case ENGINES_SYM:
      DBUG_PRINT("info",("spider ENGINES_SYM"));
      if (str->append(STRING_WITH_LEN("ENGINES "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case ENUM:
      DBUG_PRINT("info",("spider ENUM"));
      if (str->append(STRING_WITH_LEN("ENUM "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case ERROR_SYM:
      DBUG_PRINT("info",("spider ERROR_SYM"));
      if (str->append(STRING_WITH_LEN("ERROR "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case ERRORS:
      DBUG_PRINT("info",("spider ERRORS"));
      if (str->append(STRING_WITH_LEN("ERRORS "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case ESCAPE_SYM:
      DBUG_PRINT("info",("spider ESCAPE_SYM"));
      if (str->append(STRING_WITH_LEN("ESCAPE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case ESCAPED:
      DBUG_PRINT("info",("spider ESCAPED"));
      if (str->append(STRING_WITH_LEN("ESCAPED "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case EVENT_SYM:
      DBUG_PRINT("info",("spider EVENT_SYM"));
      if (str->append(STRING_WITH_LEN("EVENT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case EVENTS_SYM:
      DBUG_PRINT("info",("spider EVENTS_SYM"));
      if (str->append(STRING_WITH_LEN("EVENTS "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case EVERY_SYM:
      DBUG_PRINT("info",("spider EVERY_SYM"));
      if (str->append(STRING_WITH_LEN("EVERY "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case EXAMINED_SYM:
      DBUG_PRINT("info",("spider EXAMINED_SYM"));
      if (str->append(STRING_WITH_LEN("EXAMINED "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case EXCEPT_SYM:
      DBUG_PRINT("info",("spider EXCEPT_SYM"));
      if (str->append(STRING_WITH_LEN("EXCEPT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case EXCHANGE_SYM:
      DBUG_PRINT("info",("spider EXCHANGE_SYM"));
      if (str->append(STRING_WITH_LEN("EXCHANGE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case EXCLUDE_SYM:
      DBUG_PRINT("info",("spider EXCLUDE_SYM"));
      if (str->append(STRING_WITH_LEN("EXCLUDE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case EXECUTE_SYM: /* EXECUTE */
      DBUG_PRINT("info",("spider EXECUTE_SYM"));
      if (str->append(STRING_WITH_LEN("EXECUTE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#ifdef SPIDER_TOKEN_10_3
    case EXCEPTION_MARIADB_SYM:
      DBUG_PRINT("info",("spider EXCEPTION_MARIADB_SYM"));
      if (str->append(STRING_WITH_LEN("EXCEPTION "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case EXCEPTION_ORACLE_SYM:
      DBUG_PRINT("info",("spider EXCEPTION_ORACLE_SYM"));
      if (str->append(STRING_WITH_LEN("EXCEPTION "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#else
    case EXCEPTION_SYM:
      DBUG_PRINT("info",("spider EXCEPTION_SYM"));
      if (str->append(STRING_WITH_LEN("EXCEPTION "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#endif
    case EXISTS:
      DBUG_PRINT("info",("spider EXISTS"));
      if (str->append(STRING_WITH_LEN("EXISTS "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#ifdef SPIDER_TOKEN_10_3
    case EXIT_MARIADB_SYM:
      DBUG_PRINT("info",("spider EXIT_MARIADB_SYM"));
      if (str->append(STRING_WITH_LEN("EXIT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case EXIT_ORACLE_SYM:
      DBUG_PRINT("info",("spider EXIT_ORACLE_SYM"));
      if (str->append(STRING_WITH_LEN("EXIT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#else
    case EXIT_SYM:
      DBUG_PRINT("info",("spider EXIT_SYM"));
      if (str->append(STRING_WITH_LEN("EXIT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#endif
    case EXPANSION_SYM:
      DBUG_PRINT("info",("spider EXPANSION_SYM"));
      if (str->append(STRING_WITH_LEN("EXPANSION "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#ifdef SPIDER_TOKEN_10_4
    case EXPIRE_SYM:
      DBUG_PRINT("info",("spider EXPIRE_SYM"));
      if (str->append(STRING_WITH_LEN("EXPIRE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#endif
    case EXPORT_SYM:
      DBUG_PRINT("info",("spider EXPORT_SYM"));
      if (str->append(STRING_WITH_LEN("EXPORT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case EXTENDED_SYM:
      DBUG_PRINT("info",("spider EXTENDED_SYM"));
      if (str->append(STRING_WITH_LEN("EXTENDED "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case EXTENT_SIZE_SYM:
      DBUG_PRINT("info",("spider EXTENT_SIZE_SYM"));
      if (str->append(STRING_WITH_LEN("EXTENT_SIZE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case FALSE_SYM:
      DBUG_PRINT("info",("spider FALSE_SYM"));
      if (str->append(STRING_WITH_LEN("FALSE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case FAST_SYM:
      DBUG_PRINT("info",("spider FAST_SYM"));
      if (str->append(STRING_WITH_LEN("FAST "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case FAULTS_SYM:
      DBUG_PRINT("info",("spider FAULTS_SYM"));
      if (str->append(STRING_WITH_LEN("FAULTS "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case FETCH_SYM:
      DBUG_PRINT("info",("spider FETCH_SYM"));
      if (str->append(STRING_WITH_LEN("FETCH "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case FILE_SYM:
      DBUG_PRINT("info",("spider FILE_SYM"));
      if (str->append(STRING_WITH_LEN("FILE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case FIRST_SYM:
      DBUG_PRINT("info",("spider FIRST_SYM"));
      if (str->append(STRING_WITH_LEN("FIRST "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case FIXED_SYM:
      DBUG_PRINT("info",("spider FIXED_SYM"));
      if (str->append(STRING_WITH_LEN("FIXED "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case FLOAT_SYM:
      DBUG_PRINT("info",("spider FLOAT_SYM"));
      if (str->append(STRING_WITH_LEN("FLOAT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case FLUSH_SYM:
      DBUG_PRINT("info",("spider FLUSH_SYM"));
      if (str->append(STRING_WITH_LEN("FLUSH "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case FOLLOWING_SYM:
      DBUG_PRINT("info",("spider FOLLOWING_SYM"));
      if (str->append(STRING_WITH_LEN("FOLLOWING "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case FOLLOWS_SYM:
      DBUG_PRINT("info",("spider FOLLOWS_SYM"));
      if (str->append(STRING_WITH_LEN("FOLLOWS "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case FOR_SYM:
      DBUG_PRINT("info",("spider FOR_SYM"));
      if (str->append(STRING_WITH_LEN("FOR "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#ifdef SPIDER_TOKEN_10_3
    case FOR_SYSTEM_TIME_SYM:
      DBUG_PRINT("info",("spider FOR_SYSTEM_TIME_SYM"));
      if (str->append(STRING_WITH_LEN("FOR SYSTEM TIME "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#endif
    case FORCE_SYM:
      DBUG_PRINT("info",("spider FORCE_SYM"));
      if (str->append(STRING_WITH_LEN("FORCE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case FOREIGN:
      DBUG_PRINT("info",("spider FOREIGN"));
      if (str->append(STRING_WITH_LEN("FOREIGN "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case FORMAT_SYM:
      DBUG_PRINT("info",("spider FORMAT_SYM"));
      if (str->append(STRING_WITH_LEN("FORMAT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case FOUND_SYM:
      DBUG_PRINT("info",("spider FOUND_SYM"));
      if (str->append(STRING_WITH_LEN("FOUND "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case FROM:
      DBUG_PRINT("info",("spider FROM"));
      if (str->append(STRING_WITH_LEN("FROM "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case FULL:
      DBUG_PRINT("info",("spider FULL"));
      if (str->append(STRING_WITH_LEN("FULL "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case FULLTEXT_SYM:
      DBUG_PRINT("info",("spider FULLTEXT_SYM"));
      if (str->append(STRING_WITH_LEN("FULLTEXT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case FUNCTION_SYM:
      DBUG_PRINT("info",("spider FUNCTION_SYM"));
      if (str->append(STRING_WITH_LEN("FUNCTION "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case GENERAL:
      DBUG_PRINT("info",("spider GENERAL"));
      if (str->append(STRING_WITH_LEN("GENERAL "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case GENERATED_SYM:
      DBUG_PRINT("info",("spider GENERATED_SYM"));
      if (str->append(STRING_WITH_LEN("GENERATED "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case GEOMETRY_SYM:
      DBUG_PRINT("info",("spider GEOMETRY_SYM"));
      if (str->append(STRING_WITH_LEN("GEOMETRY "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case GEOMETRYCOLLECTION:
      DBUG_PRINT("info",("spider GEOMETRYCOLLECTION"));
      if (str->append(STRING_WITH_LEN("GEOMETRYCOLLECTION "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case GET_FORMAT:
      DBUG_PRINT("info",("spider GET_FORMAT"));
      if (str->append(STRING_WITH_LEN("GET_FORMAT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case GET_SYM:
      DBUG_PRINT("info",("spider GET_SYM"));
      if (str->append(STRING_WITH_LEN("GET "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case GLOBAL_SYM:
      DBUG_PRINT("info",("spider GLOBAL_SYM"));
      if (str->append(STRING_WITH_LEN("GLOBAL "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#ifdef SPIDER_TOKEN_10_3
    case GOTO_MARIADB_SYM:
      DBUG_PRINT("info",("spider GOTO_MARIADB_SYM"));
      if (str->append(STRING_WITH_LEN("GOTO "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case GOTO_ORACLE_SYM:
      DBUG_PRINT("info",("spider GOTO_ORACLE_SYM"));
      if (str->append(STRING_WITH_LEN("GOTO "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#else
    case GOTO_SYM:
      DBUG_PRINT("info",("spider GOTO_SYM"));
      if (str->append(STRING_WITH_LEN("GOTO "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#endif
    case GRANT: /* GRANT */
      DBUG_PRINT("info",("spider GRANT"));
      if (str->append(STRING_WITH_LEN("GRANT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case GRANTS:
      DBUG_PRINT("info",("spider GRANTS"));
      if (str->append(STRING_WITH_LEN("GRANTS "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case GROUP_SYM:
      DBUG_PRINT("info",("spider GROUP_SYM"));
      if (str->append(STRING_WITH_LEN("GROUP "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case HANDLER_SYM:
      DBUG_PRINT("info",("spider HANDLER_SYM"));
      if (str->append(STRING_WITH_LEN("HANDLER "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case HARD_SYM:
      DBUG_PRINT("info",("spider HARD_SYM"));
      if (str->append(STRING_WITH_LEN("HARD "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case HASH_SYM:
      DBUG_PRINT("info",("spider HASH_SYM"));
      if (str->append(STRING_WITH_LEN("HASH "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case HAVING:
      DBUG_PRINT("info",("spider HAVING"));
      if (str->append(STRING_WITH_LEN("HAVING "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case HELP_SYM:
      DBUG_PRINT("info",("spider HELP_SYM"));
      if (str->append(STRING_WITH_LEN("HELP "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case HIGH_PRIORITY:
      DBUG_PRINT("info",("spider HIGH_PRIORITY"));
      if (str->append(STRING_WITH_LEN("HIGH_PRIORITY "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case HISTORY_SYM:
      DBUG_PRINT("info",("spider HISTORY_SYM"));
      if (str->append(STRING_WITH_LEN("HISTORY "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case HOST_SYM:
      DBUG_PRINT("info",("spider HOST_SYM"));
      if (str->append(STRING_WITH_LEN("HOST "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case HOSTS_SYM:
      DBUG_PRINT("info",("spider HOSTS_SYM"));
      if (str->append(STRING_WITH_LEN("HOSTS "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case HOUR_SYM:
      DBUG_PRINT("info",("spider HOUR_SYM"));
      if (str->append(STRING_WITH_LEN("HOUR "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case HOUR_MICROSECOND_SYM:
      DBUG_PRINT("info",("spider HOUR_MICROSECOND_SYM"));
      if (str->append(STRING_WITH_LEN("HOUR_MICROSECOND "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case HOUR_MINUTE_SYM:
      DBUG_PRINT("info",("spider HOUR_MINUTE_SYM"));
      if (str->append(STRING_WITH_LEN("HOUR_MINUTE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case HOUR_SECOND_SYM:
      DBUG_PRINT("info",("spider HOUR_SECOND_SYM"));
      if (str->append(STRING_WITH_LEN("HOUR_SECOND "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case ID_SYM: /* id */
      DBUG_PRINT("info",("spider ID_SYM"));
      if (str->append(STRING_WITH_LEN("ID "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case IDENTIFIED_SYM:
      DBUG_PRINT("info",("spider IDENTIFIED_SYM"));
      if (str->append(STRING_WITH_LEN("IDENTIFIED "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case IF_SYM:
      DBUG_PRINT("info",("spider IF_SYM"));
      if (str->append(STRING_WITH_LEN("IF "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case IGNORE_SYM:
      DBUG_PRINT("info",("spider IGNORE_SYM"));
      if (str->append(STRING_WITH_LEN("IGNORE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case IGNORE_DOMAIN_IDS_SYM:
      DBUG_PRINT("info",("spider IGNORE_DOMAIN_IDS_SYM"));
      if (str->append(STRING_WITH_LEN("IGNORE_DOMAIN_IDS "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case IGNORE_SERVER_IDS_SYM:
      DBUG_PRINT("info",("spider IGNORE_SERVER_IDS_SYM"));
      if (str->append(STRING_WITH_LEN("IGNORE_SERVER_IDS "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case IMMEDIATE_SYM:
      DBUG_PRINT("info",("spider IMMEDIATE_SYM"));
      if (str->append(STRING_WITH_LEN("IMMEDIATE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case IMPORT:
      DBUG_PRINT("info",("spider IMPORT"));
      if (str->append(STRING_WITH_LEN("IMPORT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#ifdef SPIDER_TOKEN_10_3
    case IMPOSSIBLE_ACTION:
      DBUG_PRINT("info",("spider IMPOSSIBLE_ACTION"));
      if (str->append(STRING_WITH_LEN("IMPOSSIBLE ACTION "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#endif
    case INTERSECT_SYM:
      DBUG_PRINT("info",("spider INTERSECT_SYM"));
      if (str->append(STRING_WITH_LEN("INTERSECT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case IN_SYM:
      DBUG_PRINT("info",("spider IN_SYM"));
      if (str->append(STRING_WITH_LEN("IN "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case INCREMENT_SYM:
      DBUG_PRINT("info",("spider INCREMENT_SYM"));
      if (str->append(STRING_WITH_LEN("INCREMENT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case INDEX_SYM:
      DBUG_PRINT("info",("spider INDEX_SYM"));
      if (str->append(STRING_WITH_LEN("INDEX "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case INDEXES:
      DBUG_PRINT("info",("spider INDEXES"));
      if (str->append(STRING_WITH_LEN("INDEXES "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case INFILE:
      DBUG_PRINT("info",("spider INFILE"));
      if (str->append(STRING_WITH_LEN("INFILE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case INITIAL_SIZE_SYM:
      DBUG_PRINT("info",("spider INITIAL_SIZE_SYM"));
      if (str->append(STRING_WITH_LEN("INITIAL_SIZE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case INNER_SYM:
      DBUG_PRINT("info",("spider INNER_SYM"));
      if (str->append(STRING_WITH_LEN("INNER "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case INOUT_SYM:
      DBUG_PRINT("info",("spider INOUT_SYM"));
      if (str->append(STRING_WITH_LEN("INOUT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case INSENSITIVE_SYM:
      DBUG_PRINT("info",("spider INSENSITIVE_SYM"));
      if (str->append(STRING_WITH_LEN("INSENSITIVE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case INSERT:
      DBUG_PRINT("info",("spider INSERT"));
      if (str->append(STRING_WITH_LEN("INSERT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case INSERT_METHOD:
      DBUG_PRINT("info",("spider INSERT_METHOD"));
      if (str->append(STRING_WITH_LEN("INSERT_METHOD "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case INSTALL_SYM:
      DBUG_PRINT("info",("spider INSTALL_SYM"));
      if (str->append(STRING_WITH_LEN("INSTALL "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case INT_SYM:
      DBUG_PRINT("info",("spider INT_SYM"));
      if (str->append(STRING_WITH_LEN("INT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case INTERVAL_SYM:
      DBUG_PRINT("info",("spider INTERVAL_SYM"));
      if (str->append(STRING_WITH_LEN("INTERVAL "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case INVISIBLE_SYM:
      DBUG_PRINT("info",("spider INVISIBLE_SYM"));
      if (str->append(STRING_WITH_LEN("INVISIBLE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case INTO:
      DBUG_PRINT("info",("spider INTO"));
      if (str->append(STRING_WITH_LEN("INTO "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case IO_SYM:
      DBUG_PRINT("info",("spider IO_SYM"));
      if (str->append(STRING_WITH_LEN("IO "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case RELAY_THREAD:
      DBUG_PRINT("info",("spider RELAY_THREAD"));
      if (str->append(STRING_WITH_LEN("IO_THREAD "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case IPC_SYM:
      DBUG_PRINT("info",("spider IPC_SYM"));
      if (str->append(STRING_WITH_LEN("IPC "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case IS:
      DBUG_PRINT("info",("spider IS"));
      if (str->append(STRING_WITH_LEN("IS "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case ISOLATION:
      DBUG_PRINT("info",("spider ISOLATION"));
      if (str->append(STRING_WITH_LEN("ISOLATION "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case ISOPEN_SYM:
      DBUG_PRINT("info",("spider ISOPEN_SYM"));
      if (str->append(STRING_WITH_LEN("ISOPEN "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case ISSUER_SYM:
      DBUG_PRINT("info",("spider ISSUER_SYM"));
      if (str->append(STRING_WITH_LEN("ISSUER "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case ITERATE_SYM:
      DBUG_PRINT("info",("spider ITERATE_SYM"));
      if (str->append(STRING_WITH_LEN("ITERATE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case INVOKER_SYM:
      DBUG_PRINT("info",("spider INVOKER_SYM"));
      if (str->append(STRING_WITH_LEN("INVOKER "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case JOIN_SYM:
      DBUG_PRINT("info",("spider JOIN_SYM"));
      if (str->append(STRING_WITH_LEN("JOIN "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case JSON_SYM:
      DBUG_PRINT("info",("spider JSON_SYM"));
      if (str->append(STRING_WITH_LEN("JSON "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case KEY_SYM:
      DBUG_PRINT("info",("spider KEY_SYM"));
      if (str->append(STRING_WITH_LEN("KEY "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case KEYS:
      DBUG_PRINT("info",("spider KEYS"));
      if (str->append(STRING_WITH_LEN("KEYS "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case KEY_BLOCK_SIZE:
      DBUG_PRINT("info",("spider KEY_BLOCK_SIZE"));
      if (str->append(STRING_WITH_LEN("KEY_BLOCK_SIZE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case KILL_SYM:
      DBUG_PRINT("info",("spider KILL_SYM"));
      if (str->append(STRING_WITH_LEN("KILL "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case LANGUAGE_SYM:
      DBUG_PRINT("info",("spider LANGUAGE_SYM"));
      if (str->append(STRING_WITH_LEN("LANGUAGE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case LAST_SYM:
      DBUG_PRINT("info",("spider LAST_SYM"));
      if (str->append(STRING_WITH_LEN("LAST "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case LAST_VALUE:
      DBUG_PRINT("info",("spider LAST_VALUE"));
      if (str->append(STRING_WITH_LEN("LAST_VALUE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case LASTVAL_SYM:
      DBUG_PRINT("info",("spider LASTVAL_SYM"));
      if (str->append(STRING_WITH_LEN("LASTVAL "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case LEADING:
      DBUG_PRINT("info",("spider LEADING"));
      if (str->append(STRING_WITH_LEN("LEADING "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case LEAVE_SYM:
      DBUG_PRINT("info",("spider LEAVE_SYM"));
      if (str->append(STRING_WITH_LEN("LEAVE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case LEAVES:
      DBUG_PRINT("info",("spider LEAVES"));
      if (str->append(STRING_WITH_LEN("LEAVES "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case LEFT:
      DBUG_PRINT("info",("spider LEFT"));
      if (str->append(STRING_WITH_LEN("LEFT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#ifdef SPIDER_TOKEN_10_4
    case LEFT_PAREN_ALT:
      DBUG_PRINT("info",("spider LEFT_PAREN_ALT"));
      if (str->append(STRING_WITH_LEN(") ALT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case LEFT_PAREN_LIKE:
      DBUG_PRINT("info",("spider LEFT_PAREN_LIKE"));
      if (str->append(STRING_WITH_LEN(") LIKE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case LEFT_PAREN_WITH:
      DBUG_PRINT("info",("spider LEFT_PAREN_WITH"));
      if (str->append(STRING_WITH_LEN(") WITH "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#endif
    case LESS_SYM:
      DBUG_PRINT("info",("spider LESS_SYM"));
      if (str->append(STRING_WITH_LEN("LESS "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case LEVEL_SYM:
      DBUG_PRINT("info",("spider LEVEL_SYM"));
      if (str->append(STRING_WITH_LEN("LEVEL "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case LIKE:
      DBUG_PRINT("info",("spider LIKE"));
      if (str->append(STRING_WITH_LEN("LIKE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case LIMIT:
      DBUG_PRINT("info",("spider LIMIT"));
      if (str->append(STRING_WITH_LEN("LIMIT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case LINEAR_SYM:
      DBUG_PRINT("info",("spider LINEAR_SYM"));
      if (str->append(STRING_WITH_LEN("LINEAR "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case LINES:
      DBUG_PRINT("info",("spider LINES"));
      if (str->append(STRING_WITH_LEN("LINES "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case LINESTRING:
      DBUG_PRINT("info",("spider LINESTRING"));
      if (str->append(STRING_WITH_LEN("LINESTRING "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case LIST_SYM:
      DBUG_PRINT("info",("spider LIST_SYM"));
      if (str->append(STRING_WITH_LEN("LIST "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case LOAD:
      DBUG_PRINT("info",("spider LOAD"));
      if (str->append(STRING_WITH_LEN("LOAD "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case LOCAL_SYM:
      DBUG_PRINT("info",("spider LOCAL_SYM"));
      if (str->append(STRING_WITH_LEN("LOCAL "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#ifdef SPIDER_TOKEN_10_3
    case LOCATOR_SYM:
      DBUG_PRINT("info",("spider LOCATOR_SYM"));
      if (str->append(STRING_WITH_LEN("LOCATOR "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#endif
    case LOCK_SYM:
      DBUG_PRINT("info",("spider LOCK_SYM"));
      if (str->append(STRING_WITH_LEN("LOCK "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case LOCKS_SYM:
      DBUG_PRINT("info",("spider LOCKS_SYM"));
      if (str->append(STRING_WITH_LEN("LOCKS "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case LOGFILE_SYM:
      DBUG_PRINT("info",("spider LOGFILE_SYM"));
      if (str->append(STRING_WITH_LEN("LOGFILE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case LOGS_SYM:
      DBUG_PRINT("info",("spider LOGS_SYM"));
      if (str->append(STRING_WITH_LEN("LOGS "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case LONG_SYM:
      DBUG_PRINT("info",("spider LONG_SYM"));
      if (str->append(STRING_WITH_LEN("LONG "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case LONGBLOB:
      DBUG_PRINT("info",("spider LONGBLOB"));
      if (str->append(STRING_WITH_LEN("LONGBLOB "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case LONGTEXT:
      DBUG_PRINT("info",("spider LONGTEXT"));
      if (str->append(STRING_WITH_LEN("LONGTEXT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case LOOP_SYM:
      DBUG_PRINT("info",("spider LOOP_SYM"));
      if (str->append(STRING_WITH_LEN("LOOP "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case LOW_PRIORITY:
      DBUG_PRINT("info",("spider LOW_PRIORITY"));
      if (str->append(STRING_WITH_LEN("LOW_PRIORITY "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case MASTER_SYM:
      DBUG_PRINT("info",("spider MASTER_SYM"));
      if (str->append(STRING_WITH_LEN("MASTER "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case MASTER_CONNECT_RETRY_SYM:
      DBUG_PRINT("info",("spider MASTER_CONNECT_RETRY_SYM"));
      if (str->append(STRING_WITH_LEN("MASTER_CONNECT_RETRY "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case MASTER_DELAY_SYM:
      DBUG_PRINT("info",("spider MASTER_DELAY_SYM"));
      if (str->append(STRING_WITH_LEN("MASTER_DELAY "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case MASTER_GTID_POS_SYM:
      DBUG_PRINT("info",("spider MASTER_GTID_POS_SYM"));
      if (str->append(STRING_WITH_LEN("MASTER_GTID_POS "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case MASTER_HOST_SYM:
      DBUG_PRINT("info",("spider MASTER_HOST_SYM"));
      if (str->append(STRING_WITH_LEN("MASTER_HOST "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case MASTER_LOG_FILE_SYM:
      DBUG_PRINT("info",("spider MASTER_LOG_FILE_SYM"));
      if (str->append(STRING_WITH_LEN("MASTER_LOG_FILE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case MASTER_LOG_POS_SYM:
      DBUG_PRINT("info",("spider MASTER_LOG_POS_SYM"));
      if (str->append(STRING_WITH_LEN("MASTER_LOG_POS "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case MASTER_PASSWORD_SYM:
      DBUG_PRINT("info",("spider MASTER_PASSWORD_SYM"));
      if (str->append(STRING_WITH_LEN("MASTER_PASSWORD "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case MASTER_PORT_SYM:
      DBUG_PRINT("info",("spider MASTER_PORT_SYM"));
      if (str->append(STRING_WITH_LEN("MASTER_PORT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case MASTER_SERVER_ID_SYM:
      DBUG_PRINT("info",("spider MASTER_SERVER_ID_SYM"));
      if (str->append(STRING_WITH_LEN("MASTER_SERVER_ID "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case MASTER_SSL_SYM:
      DBUG_PRINT("info",("spider MASTER_SSL_SYM"));
      if (str->append(STRING_WITH_LEN("MASTER_SSL "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case MASTER_SSL_CA_SYM:
      DBUG_PRINT("info",("spider MASTER_SSL_CA_SYM"));
      if (str->append(STRING_WITH_LEN("MASTER_SSL_CA "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case MASTER_SSL_CAPATH_SYM:
      DBUG_PRINT("info",("spider MASTER_SSL_CAPATH_SYM"));
      if (str->append(STRING_WITH_LEN("MASTER_SSL_CAPATH "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case MASTER_SSL_CERT_SYM:
      DBUG_PRINT("info",("spider MASTER_SSL_CERT_SYM"));
      if (str->append(STRING_WITH_LEN("MASTER_SSL_CERT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case MASTER_SSL_CIPHER_SYM:
      DBUG_PRINT("info",("spider MASTER_SSL_CIPHER_SYM"));
      if (str->append(STRING_WITH_LEN("MASTER_SSL_CIPHER "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case MASTER_SSL_CRL_SYM:
      DBUG_PRINT("info",("spider MASTER_SSL_CRL_SYM"));
      if (str->append(STRING_WITH_LEN("MASTER_SSL_CRL "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case MASTER_SSL_CRLPATH_SYM:
      DBUG_PRINT("info",("spider MASTER_SSL_CRLPATH_SYM"));
      if (str->append(STRING_WITH_LEN("MASTER_SSL_CRLPATH "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case MASTER_SSL_KEY_SYM:
      DBUG_PRINT("info",("spider MASTER_SSL_KEY_SYM"));
      if (str->append(STRING_WITH_LEN("MASTER_SSL_KEY "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case MASTER_SSL_VERIFY_SERVER_CERT_SYM:
      DBUG_PRINT("info",("spider MASTER_SSL_VERIFY_SERVER_CERT_SYM"));
      if (str->append(STRING_WITH_LEN("MASTER_SSL_VERIFY_SERVER_CERT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case MASTER_USER_SYM:
      DBUG_PRINT("info",("spider MASTER_USER_SYM"));
      if (str->append(STRING_WITH_LEN("MASTER_USER "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case MASTER_USE_GTID_SYM:
      DBUG_PRINT("info",("spider MASTER_USE_GTID_SYM"));
      if (str->append(STRING_WITH_LEN("MASTER_USE_GTID "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case MASTER_HEARTBEAT_PERIOD_SYM:
      DBUG_PRINT("info",("spider MASTER_HEARTBEAT_PERIOD_SYM"));
      if (str->append(STRING_WITH_LEN("MASTER_HEARTBEAT_PERIOD "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case MATCH:
      DBUG_PRINT("info",("spider MATCH"));
      if (str->append(STRING_WITH_LEN("MATCH "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case MAX_CONNECTIONS_PER_HOUR:
      DBUG_PRINT("info",("spider MAX_CONNECTIONS_PER_HOUR"));
      if (str->append(STRING_WITH_LEN("MAX_CONNECTIONS_PER_HOUR "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case MAX_QUERIES_PER_HOUR:
      DBUG_PRINT("info",("spider MAX_QUERIES_PER_HOUR"));
      if (str->append(STRING_WITH_LEN("MAX_QUERIES_PER_HOUR "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case MAX_ROWS:
      DBUG_PRINT("info",("spider MAX_ROWS"));
      if (str->append(STRING_WITH_LEN("MAX_ROWS "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case MAX_SIZE_SYM:
      DBUG_PRINT("info",("spider MAX_SIZE_SYM"));
      if (str->append(STRING_WITH_LEN("MAX_SIZE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case MAX_STATEMENT_TIME_SYM:
      DBUG_PRINT("info",("spider MAX_STATEMENT_TIME_SYM"));
      if (str->append(STRING_WITH_LEN("MAX_STATEMENT_TIME "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case MAX_UPDATES_PER_HOUR:
      DBUG_PRINT("info",("spider MAX_UPDATES_PER_HOUR"));
      if (str->append(STRING_WITH_LEN("MAX_UPDATES_PER_HOUR "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case MAX_USER_CONNECTIONS_SYM:
      DBUG_PRINT("info",("spider MAX_USER_CONNECTIONS_SYM"));
      if (str->append(STRING_WITH_LEN("MAX_USER_CONNECTIONS "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case MAXVALUE_SYM:
      DBUG_PRINT("info",("spider MAXVALUE_SYM"));
      if (str->append(STRING_WITH_LEN("MAXVALUE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case MEDIUM_SYM:
      DBUG_PRINT("info",("spider MEDIUM_SYM"));
      if (str->append(STRING_WITH_LEN("MEDIUM "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case MEDIUMBLOB:
      DBUG_PRINT("info",("spider MEDIUMBLOB"));
      if (str->append(STRING_WITH_LEN("MEDIUMBLOB "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case MEDIUMINT:
      DBUG_PRINT("info",("spider MEDIUMINT"));
      if (str->append(STRING_WITH_LEN("MEDIUMINT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case MEDIUMTEXT:
      DBUG_PRINT("info",("spider MEDIUMTEXT"));
      if (str->append(STRING_WITH_LEN("MEDIUMTEXT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case MEMORY_SYM:
      DBUG_PRINT("info",("spider MEMORY_SYM"));
      if (str->append(STRING_WITH_LEN("MEMORY "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case MERGE_SYM:
      DBUG_PRINT("info",("spider MERGE_SYM"));
      if (str->append(STRING_WITH_LEN("MERGE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case MESSAGE_TEXT_SYM:
      DBUG_PRINT("info",("spider MESSAGE_TEXT_SYM"));
      if (str->append(STRING_WITH_LEN("MESSAGE_TEXT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case MICROSECOND_SYM:
      DBUG_PRINT("info",("spider MICROSECOND_SYM"));
      if (str->append(STRING_WITH_LEN("MICROSECOND "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case MIGRATE_SYM:
      DBUG_PRINT("info",("spider MIGRATE_SYM"));
      if (str->append(STRING_WITH_LEN("MIGRATE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case MINUTE_SYM:
      DBUG_PRINT("info",("spider MINUTE_SYM"));
      if (str->append(STRING_WITH_LEN("MINUTE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case MINUTE_MICROSECOND_SYM:
      DBUG_PRINT("info",("spider MINUTE_MICROSECOND_SYM"));
      if (str->append(STRING_WITH_LEN("MINUTE_MICROSECOND "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case MINUTE_SECOND_SYM:
      DBUG_PRINT("info",("spider MINUTE_SECOND_SYM"));
      if (str->append(STRING_WITH_LEN("MINUTE_SECOND "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case MINVALUE_SYM:
      DBUG_PRINT("info",("spider MINVALUE_SYM"));
      if (str->append(STRING_WITH_LEN("MINVALUE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case MIN_ROWS:
      DBUG_PRINT("info",("spider MIN_ROWS"));
      if (str->append(STRING_WITH_LEN("MIN_ROWS "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case MOD_SYM:
      DBUG_PRINT("info",("spider MOD_SYM"));
      if (str->append(STRING_WITH_LEN("MOD "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case MODE_SYM:
      DBUG_PRINT("info",("spider MODE_SYM"));
      if (str->append(STRING_WITH_LEN("MODE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case MODIFIES_SYM:
      DBUG_PRINT("info",("spider MODIFIES_SYM"));
      if (str->append(STRING_WITH_LEN("MODIFIES "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case MODIFY_SYM:
      DBUG_PRINT("info",("spider MODIFY_SYM"));
      if (str->append(STRING_WITH_LEN("MODIFY "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case MONTH_SYM:
      DBUG_PRINT("info",("spider MONTH_SYM"));
      if (str->append(STRING_WITH_LEN("MONTH "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case MULTILINESTRING:
      DBUG_PRINT("info",("spider MULTILINESTRING"));
      if (str->append(STRING_WITH_LEN("MULTILINESTRING "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case MULTIPOINT:
      DBUG_PRINT("info",("spider MULTIPOINT"));
      if (str->append(STRING_WITH_LEN("MULTIPOINT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case MULTIPOLYGON:
      DBUG_PRINT("info",("spider MULTIPOLYGON"));
      if (str->append(STRING_WITH_LEN("MULTIPOLYGON "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case MUTEX_SYM:
      DBUG_PRINT("info",("spider MUTEX_SYM"));
      if (str->append(STRING_WITH_LEN("MUTEX "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#ifdef SPIDER_TOKEN_10_3
    case MYSQL_CONCAT_SYM:
      DBUG_PRINT("info",("spider MYSQL_CONCAT_SYM"));
      if (str->append(STRING_WITH_LEN("CONCAT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#endif
    case MYSQL_SYM:
      DBUG_PRINT("info",("spider MYSQL_SYM"));
      if (str->append(STRING_WITH_LEN("MYSQL "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case MYSQL_ERRNO_SYM:
      DBUG_PRINT("info",("spider MYSQL_ERRNO_SYM"));
      if (str->append(STRING_WITH_LEN("MYSQL_ERRNO "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case NAME_SYM:
      DBUG_PRINT("info",("spider NAME_SYM"));
      if (str->append(STRING_WITH_LEN("NAME "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case NAMES_SYM:
      DBUG_PRINT("info",("spider NAMES_SYM"));
      if (str->append(STRING_WITH_LEN("NAMES "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case NATIONAL_SYM:
      DBUG_PRINT("info",("spider NATIONAL_SYM"));
      if (str->append(STRING_WITH_LEN("NATIONAL "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case NATURAL:
      DBUG_PRINT("info",("spider NATURAL"));
      if (str->append(STRING_WITH_LEN("NATURAL "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case NCHAR_SYM:
      DBUG_PRINT("info",("spider NCHAR_SYM"));
      if (str->append(STRING_WITH_LEN("NCHAR "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#ifdef SPIDER_TOKEN_10_4
    case NEVER_SYM:
      DBUG_PRINT("info",("spider NEVER_SYM"));
      if (str->append(STRING_WITH_LEN("NEVER "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#endif
    case NEW_SYM:
      DBUG_PRINT("info",("spider NEW_SYM"));
      if (str->append(STRING_WITH_LEN("NEW "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case NEXT_SYM:
      DBUG_PRINT("info",("spider NEXT_SYM"));
      if (str->append(STRING_WITH_LEN("NEXT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case NEXTVAL_SYM:
      DBUG_PRINT("info",("spider NEXTVAL_SYM"));
      if (str->append(STRING_WITH_LEN("NEXTVAL "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case NO_SYM:
      DBUG_PRINT("info",("spider NO_SYM"));
      if (str->append(STRING_WITH_LEN("NO "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case NOMAXVALUE_SYM:
      DBUG_PRINT("info",("spider NOMAXVALUE_SYM"));
      if (str->append(STRING_WITH_LEN("NOMAXVALUE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case NOMINVALUE_SYM:
      DBUG_PRINT("info",("spider NOMINVALUE_SYM"));
      if (str->append(STRING_WITH_LEN("NOMINVALUE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case NOCACHE_SYM:
      DBUG_PRINT("info",("spider NOCACHE_SYM"));
      if (str->append(STRING_WITH_LEN("NOCACHE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case NOCYCLE_SYM:
      DBUG_PRINT("info",("spider NOCYCLE_SYM"));
      if (str->append(STRING_WITH_LEN("NOCYCLE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case NO_WAIT_SYM:
      DBUG_PRINT("info",("spider NO_WAIT_SYM"));
      if (str->append(STRING_WITH_LEN("NO_WAIT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case NOWAIT_SYM:
      DBUG_PRINT("info",("spider NOWAIT_SYM"));
      if (str->append(STRING_WITH_LEN("NOWAIT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case NODEGROUP_SYM:
      DBUG_PRINT("info",("spider NODEGROUP_SYM"));
      if (str->append(STRING_WITH_LEN("NODEGROUP "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case NONE_SYM:
      DBUG_PRINT("info",("spider NONE_SYM"));
      if (str->append(STRING_WITH_LEN("NONE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case NOT_SYM:
      DBUG_PRINT("info",("spider NOT_SYM"));
      if (str->append(STRING_WITH_LEN("NOT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case NOTFOUND_SYM:
      DBUG_PRINT("info",("spider NOTFOUND_SYM"));
      if (str->append(STRING_WITH_LEN("NOTFOUND "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case NO_WRITE_TO_BINLOG:
      DBUG_PRINT("info",("spider NO_WRITE_TO_BINLOG"));
      if (str->append(STRING_WITH_LEN("NO_WRITE_TO_BINLOG "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case NULL_SYM:
      DBUG_PRINT("info",("spider NULL_SYM"));
      if (str->append(STRING_WITH_LEN("NULL "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#ifdef SPIDER_TOKEN_10_3
    case NUMBER_MARIADB_SYM:
      DBUG_PRINT("info",("spider NUMBER_MARIADB_SYM"));
      if (str->append(STRING_WITH_LEN("NUMBER "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case NUMBER_ORACLE_SYM:
      DBUG_PRINT("info",("spider NUMBER_ORACLE_SYM"));
      if (str->append(STRING_WITH_LEN("NUMBER "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#else
    case NUMBER_SYM:
      DBUG_PRINT("info",("spider NUMBER_SYM"));
      if (str->append(STRING_WITH_LEN("NUMBER "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#endif
    case NUMERIC_SYM:
      DBUG_PRINT("info",("spider NUMERIC_SYM"));
      if (str->append(STRING_WITH_LEN("NUMERIC "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case NVARCHAR_SYM:
      DBUG_PRINT("info",("spider NVARCHAR_SYM"));
      if (str->append(STRING_WITH_LEN("NVARCHAR "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case OF_SYM:
      DBUG_PRINT("info",("spider OF_SYM"));
      if (str->append(STRING_WITH_LEN("OF "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case OFFSET_SYM:
      DBUG_PRINT("info",("spider OFFSET_SYM"));
      if (str->append(STRING_WITH_LEN("OFFSET "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case OLD_PASSWORD_SYM:
      DBUG_PRINT("info",("spider OLD_PASSWORD_SYM"));
      if (str->append(STRING_WITH_LEN("OLD_PASSWORD "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case ON: /* ON */
      DBUG_PRINT("info",("spider ON"));
      if (str->append(STRING_WITH_LEN("ON "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case ONE_SYM:
      DBUG_PRINT("info",("spider ONE_SYM"));
      if (str->append(STRING_WITH_LEN("ONE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case ONLINE_SYM:
      DBUG_PRINT("info",("spider ONLINE_SYM"));
      if (str->append(STRING_WITH_LEN("ONLINE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case ONLY_SYM:
      DBUG_PRINT("info",("spider ONLY_SYM"));
      if (str->append(STRING_WITH_LEN("ONLY "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case OPEN_SYM:
      DBUG_PRINT("info",("spider OPEN_SYM"));
      if (str->append(STRING_WITH_LEN("OPEN "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case OPTIMIZE:
      DBUG_PRINT("info",("spider OPTIMIZE"));
      if (str->append(STRING_WITH_LEN("OPTIMIZE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case OPTIONS_SYM:
      DBUG_PRINT("info",("spider OPTIONS_SYM"));
      if (str->append(STRING_WITH_LEN("OPTIONS "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case OPTION:
      DBUG_PRINT("info",("spider OPTION"));
      if (str->append(STRING_WITH_LEN("OPTION "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case OPTIONALLY:
      DBUG_PRINT("info",("spider OPTIONALLY"));
      if (str->append(STRING_WITH_LEN("OPTIONALLY "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case OR_SYM:
      DBUG_PRINT("info",("spider OR_SYM"));
      if (str->append(STRING_WITH_LEN("OR "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#ifdef SPIDER_TOKEN_10_3
    case ORACLE_CONCAT_SYM:
      DBUG_PRINT("info",("spider ORACLE_CONCAT_SYM"));
      if (str->append(STRING_WITH_LEN("|| "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#endif
    case ORDER_SYM:
      DBUG_PRINT("info",("spider ORDER_SYM"));
      if (str->append(STRING_WITH_LEN("ORDER "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#ifdef SPIDER_TOKEN_10_3
    case OTHERS_MARIADB_SYM:
      DBUG_PRINT("info",("spider OTHERS_MARIADB_SYM"));
      if (str->append(STRING_WITH_LEN("OTHERS "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case OTHERS_ORACLE_SYM:
      DBUG_PRINT("info",("spider OTHERS_ORACLE_SYM"));
      if (str->append(STRING_WITH_LEN("OTHERS "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#else
    case OTHERS_SYM:
      DBUG_PRINT("info",("spider OTHERS_SYM"));
      if (str->append(STRING_WITH_LEN("OTHERS "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#endif
    case OUT_SYM:
      DBUG_PRINT("info",("spider OUT_SYM"));
      if (str->append(STRING_WITH_LEN("OUT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case OUTER:
      DBUG_PRINT("info",("spider OUTER"));
      if (str->append(STRING_WITH_LEN("OUTER "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case OUTFILE:
      DBUG_PRINT("info",("spider OUTFILE"));
      if (str->append(STRING_WITH_LEN("OUTFILE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case OVER_SYM:
      DBUG_PRINT("info",("spider OVER_SYM"));
      if (str->append(STRING_WITH_LEN("OVER "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case OWNER_SYM:
      DBUG_PRINT("info",("spider OWNER_SYM"));
      if (str->append(STRING_WITH_LEN("OWNER "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#ifdef SPIDER_TOKEN_10_3
    case PACKAGE_MARIADB_SYM:
      DBUG_PRINT("info",("spider PACKAGE_MARIADB_SYM"));
      if (str->append(STRING_WITH_LEN("PACKAGE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case PACKAGE_ORACLE_SYM:
      DBUG_PRINT("info",("spider PACKAGE_ORACLE_SYM"));
      if (str->append(STRING_WITH_LEN("PACKAGE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#else
    case PACKAGE_SYM:
      DBUG_PRINT("info",("spider PACKAGE_SYM"));
      if (str->append(STRING_WITH_LEN("PACKAGE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#endif
    case PACK_KEYS_SYM:
      DBUG_PRINT("info",("spider PACK_KEYS_SYM"));
      if (str->append(STRING_WITH_LEN("PACK_KEYS "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case PAGE_SYM:
      DBUG_PRINT("info",("spider PAGE_SYM"));
      if (str->append(STRING_WITH_LEN("PAGE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case PAGE_CHECKSUM_SYM:
      DBUG_PRINT("info",("spider PAGE_CHECKSUM_SYM"));
      if (str->append(STRING_WITH_LEN("PAGE_CHECKSUM "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case PARSER_SYM:
      DBUG_PRINT("info",("spider PARSER_SYM"));
      if (str->append(STRING_WITH_LEN("PARSER "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case PARSE_VCOL_EXPR_SYM:
      DBUG_PRINT("info",("spider PARSE_VCOL_EXPR_SYM"));
      if (str->append(STRING_WITH_LEN("PARSE_VCOL_EXPR "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case PERIOD_SYM:
      DBUG_PRINT("info",("spider PERIOD_SYM"));
      if (str->append(STRING_WITH_LEN("PERIOD "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case PARTIAL:
      DBUG_PRINT("info",("spider PARTIAL"));
      if (str->append(STRING_WITH_LEN("PARTIAL "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case PARTITION_SYM:
      DBUG_PRINT("info",("spider PARTITION_SYM"));
      if (str->append(STRING_WITH_LEN("PARTITION "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case PARTITIONING_SYM:
      DBUG_PRINT("info",("spider PARTITIONING_SYM"));
      if (str->append(STRING_WITH_LEN("PARTITIONING "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case PARTITIONS_SYM:
      DBUG_PRINT("info",("spider PARTITIONS_SYM"));
      if (str->append(STRING_WITH_LEN("PARTITIONS "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case PASSWORD_SYM:
      DBUG_PRINT("info",("spider PASSWORD_SYM"));
      if (str->append(STRING_WITH_LEN("PASSWORD "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case PERSISTENT_SYM:
      DBUG_PRINT("info",("spider PERSISTENT_SYM"));
      if (str->append(STRING_WITH_LEN("PERSISTENT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case PHASE_SYM:
      DBUG_PRINT("info",("spider PHASE_SYM"));
      if (str->append(STRING_WITH_LEN("PHASE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case PLUGIN_SYM:
      DBUG_PRINT("info",("spider PLUGIN_SYM"));
      if (str->append(STRING_WITH_LEN("PLUGIN "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case PLUGINS_SYM:
      DBUG_PRINT("info",("spider PLUGINS_SYM"));
      if (str->append(STRING_WITH_LEN("PLUGINS "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case POINT_SYM:
      DBUG_PRINT("info",("spider POINT_SYM"));
      if (str->append(STRING_WITH_LEN("POINT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case POLYGON:
      DBUG_PRINT("info",("spider POLYGON"));
      if (str->append(STRING_WITH_LEN("POLYGON "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case PORT_SYM:
      DBUG_PRINT("info",("spider PORT_SYM"));
      if (str->append(STRING_WITH_LEN("PORT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#ifdef SPIDER_TOKEN_10_3
    case PREC_BELOW_CONTRACTION_TOKEN2:
      DBUG_PRINT("info",("spider PREC_BELOW_CONTRACTION_TOKEN2"));
      if (str->append(STRING_WITH_LEN("PREC_BELOW_CONTRACTION_TOKEN2 "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case PREC_BELOW_ESCAPE:
      DBUG_PRINT("info",("spider PREC_BELOW_ESCAPE"));
      if (str->append(STRING_WITH_LEN("PREC_BELOW_ESCAPE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case PREC_BELOW_IDENTIFIER_OPT_SPECIAL_CASE:
      DBUG_PRINT("info",("spider PREC_BELOW_IDENTIFIER_OPT_SPECIAL_CASE"));
      if (str->append(STRING_WITH_LEN("PREC_BELOW_IDENTIFIER_OPT_SPECIAL_CASE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case PREC_BELOW_NOT:
      DBUG_PRINT("info",("spider PREC_BELOW_NOT"));
      if (str->append(STRING_WITH_LEN("PREC_BELOW_NOT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#endif
    case PRECEDES_SYM:
      DBUG_PRINT("info",("spider PRECEDES_SYM"));
      if (str->append(STRING_WITH_LEN("PRECEDES "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case PRECEDING_SYM:
      DBUG_PRINT("info",("spider PRECEDING_SYM"));
      if (str->append(STRING_WITH_LEN("PRECEDING "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case PRECISION:
      DBUG_PRINT("info",("spider PRECISION"));
      if (str->append(STRING_WITH_LEN("PRECISION "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case PREPARE_SYM: /* PREPARE */
      DBUG_PRINT("info",("spider PREPARE_SYM"));
      if (str->append(STRING_WITH_LEN("PREPARE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case PRESERVE_SYM:
      DBUG_PRINT("info",("spider PRESERVE_SYM"));
      if (str->append(STRING_WITH_LEN("PRESERVE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case PREV_SYM:
      DBUG_PRINT("info",("spider PREV_SYM"));
      if (str->append(STRING_WITH_LEN("PREV "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case PREVIOUS_SYM:
      DBUG_PRINT("info",("spider PREVIOUS_SYM"));
      if (str->append(STRING_WITH_LEN("PREVIOUS "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case PRIMARY_SYM:
      DBUG_PRINT("info",("spider PRIMARY_SYM"));
      if (str->append(STRING_WITH_LEN("PRIMARY "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case PRIVILEGES:
      DBUG_PRINT("info",("spider PRIVILEGES"));
      if (str->append(STRING_WITH_LEN("PRIVILEGES "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case PROCEDURE_SYM:
      DBUG_PRINT("info",("spider PROCEDURE_SYM"));
      if (str->append(STRING_WITH_LEN("PROCEDURE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case  PROCESS:
      DBUG_PRINT("info",("spider  PROCESS"));
      if (str->append(STRING_WITH_LEN("PROCESS "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case PROCESSLIST_SYM:
      DBUG_PRINT("info",("spider PROCESSLIST_SYM"));
      if (str->append(STRING_WITH_LEN("PROCESSLIST "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case PROFILE_SYM:
      DBUG_PRINT("info",("spider PROFILE_SYM"));
      if (str->append(STRING_WITH_LEN("PROFILE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case PROFILES_SYM:
      DBUG_PRINT("info",("spider PROFILES_SYM"));
      if (str->append(STRING_WITH_LEN("PROFILES "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case PROXY_SYM:
      DBUG_PRINT("info",("spider PROXY_SYM"));
      if (str->append(STRING_WITH_LEN("PROXY "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case PURGE:
      DBUG_PRINT("info",("spider PURGE"));
      if (str->append(STRING_WITH_LEN("PURGE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case QUARTER_SYM:
      DBUG_PRINT("info",("spider QUARTER_SYM"));
      if (str->append(STRING_WITH_LEN("QUARTER "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case QUERY_SYM:
      DBUG_PRINT("info",("spider QUERY_SYM"));
      if (str->append(STRING_WITH_LEN("QUERY "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case QUICK:
      DBUG_PRINT("info",("spider QUICK"));
      if (str->append(STRING_WITH_LEN("QUICK "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#ifdef SPIDER_TOKEN_10_3
    case RAISE_MARIADB_SYM:
      DBUG_PRINT("info",("spider RAISE_MARIADB_SYM"));
      if (str->append(STRING_WITH_LEN("RAISE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case RAISE_ORACLE_SYM:
      DBUG_PRINT("info",("spider RAISE_ORACLE_SYM"));
      if (str->append(STRING_WITH_LEN("RAISE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#else
    case RAISE_SYM:
      DBUG_PRINT("info",("spider RAISE_SYM"));
      if (str->append(STRING_WITH_LEN("RAISE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#endif
    case RANGE_SYM:
      DBUG_PRINT("info",("spider RANGE_SYM"));
      if (str->append(STRING_WITH_LEN("RANGE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#ifdef SPIDER_TOKEN_10_3
    case RAW_MARIADB_SYM:
      DBUG_PRINT("info",("spider RAW_MARIADB_SYM"));
      if (str->append(STRING_WITH_LEN("RAW "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case RAW_ORACLE_SYM:
      DBUG_PRINT("info",("spider RAW_ORACLE_SYM"));
      if (str->append(STRING_WITH_LEN("RAW "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#else
    case RAW:
      DBUG_PRINT("info",("spider RAW"));
      if (str->append(STRING_WITH_LEN("RAW "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#endif
    case READ_SYM:
      DBUG_PRINT("info",("spider READ_SYM"));
      if (str->append(STRING_WITH_LEN("READ "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case READ_ONLY_SYM:
      DBUG_PRINT("info",("spider READ_ONLY_SYM"));
      if (str->append(STRING_WITH_LEN("READ_ONLY "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case READ_WRITE_SYM:
      DBUG_PRINT("info",("spider READ_WRITE_SYM"));
      if (str->append(STRING_WITH_LEN("READ_WRITE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case READS_SYM:
      DBUG_PRINT("info",("spider READS_SYM"));
      if (str->append(STRING_WITH_LEN("READS "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case REAL:
      DBUG_PRINT("info",("spider REAL"));
      if (str->append(STRING_WITH_LEN("REAL "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case REBUILD_SYM:
      DBUG_PRINT("info",("spider REBUILD_SYM"));
      if (str->append(STRING_WITH_LEN("REBUILD "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case RECOVER_SYM:
      DBUG_PRINT("info",("spider RECOVER_SYM"));
      if (str->append(STRING_WITH_LEN("RECOVER "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case RECURSIVE_SYM:
      DBUG_PRINT("info",("spider RECURSIVE_SYM"));
      if (str->append(STRING_WITH_LEN("RECURSIVE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case REDO_BUFFER_SIZE_SYM:
      DBUG_PRINT("info",("spider REDO_BUFFER_SIZE_SYM"));
      if (str->append(STRING_WITH_LEN("REDO_BUFFER_SIZE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case REDOFILE_SYM:
      DBUG_PRINT("info",("spider REDOFILE_SYM"));
      if (str->append(STRING_WITH_LEN("REDOFILE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case REDUNDANT_SYM:
      DBUG_PRINT("info",("spider REDUNDANT_SYM"));
      if (str->append(STRING_WITH_LEN("REDUNDANT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case REFERENCES:
      DBUG_PRINT("info",("spider REFERENCES"));
      if (str->append(STRING_WITH_LEN("REFERENCES "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case REGEXP:
      DBUG_PRINT("info",("spider REGEXP"));
      if (str->append(STRING_WITH_LEN("REGEXP "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case RELAY:
      DBUG_PRINT("info",("spider RELAY"));
      if (str->append(STRING_WITH_LEN("RELAY "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case RELAYLOG_SYM:
      DBUG_PRINT("info",("spider RELAYLOG_SYM"));
      if (str->append(STRING_WITH_LEN("RELAYLOG "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case RELAY_LOG_FILE_SYM:
      DBUG_PRINT("info",("spider RELAY_LOG_FILE_SYM"));
      if (str->append(STRING_WITH_LEN("RELAY_LOG_FILE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case RELAY_LOG_POS_SYM:
      DBUG_PRINT("info",("spider RELAY_LOG_POS_SYM"));
      if (str->append(STRING_WITH_LEN("RELAY_LOG_POS "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case RELEASE_SYM:
      DBUG_PRINT("info",("spider RELEASE_SYM"));
      if (str->append(STRING_WITH_LEN("RELEASE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case RELOAD:
      DBUG_PRINT("info",("spider RELOAD"));
      if (str->append(STRING_WITH_LEN("RELOAD "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case REMOVE_SYM:
      DBUG_PRINT("info",("spider REMOVE_SYM"));
      if (str->append(STRING_WITH_LEN("REMOVE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case RENAME:
      DBUG_PRINT("info",("spider RENAME"));
      if (str->append(STRING_WITH_LEN("RENAME "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case REORGANIZE_SYM:
      DBUG_PRINT("info",("spider REORGANIZE_SYM"));
      if (str->append(STRING_WITH_LEN("REORGANIZE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case REPAIR:
      DBUG_PRINT("info",("spider REPAIR"));
      if (str->append(STRING_WITH_LEN("REPAIR "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case REPEATABLE_SYM:
      DBUG_PRINT("info",("spider REPEATABLE_SYM"));
      if (str->append(STRING_WITH_LEN("REPEATABLE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case REPLACE:
      DBUG_PRINT("info",("spider REPLACE"));
      if (str->append(STRING_WITH_LEN("REPLACE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case REPLICATION:
      DBUG_PRINT("info",("spider REPLICATION"));
      if (str->append(STRING_WITH_LEN("REPLICATION "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case REPEAT_SYM:
      DBUG_PRINT("info",("spider REPEAT_SYM"));
      if (str->append(STRING_WITH_LEN("REPEAT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case REQUIRE_SYM:
      DBUG_PRINT("info",("spider REQUIRE_SYM"));
      if (str->append(STRING_WITH_LEN("REQUIRE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case RESET_SYM:
      DBUG_PRINT("info",("spider RESET_SYM"));
      if (str->append(STRING_WITH_LEN("RESET "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case RESIGNAL_SYM:
      DBUG_PRINT("info",("spider RESIGNAL_SYM"));
      if (str->append(STRING_WITH_LEN("RESIGNAL "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case RESTART_SYM:
      DBUG_PRINT("info",("spider RESTART_SYM"));
      if (str->append(STRING_WITH_LEN("RESTART "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case RESTORE_SYM:
      DBUG_PRINT("info",("spider RESTORE_SYM"));
      if (str->append(STRING_WITH_LEN("RESTORE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case RESTRICT:
      DBUG_PRINT("info",("spider RESTRICT"));
      if (str->append(STRING_WITH_LEN("RESTRICT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case RESUME_SYM:
      DBUG_PRINT("info",("spider RESUME_SYM"));
      if (str->append(STRING_WITH_LEN("RESUME "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case RETURNED_SQLSTATE_SYM:
      DBUG_PRINT("info",("spider RETURNED_SQLSTATE_SYM"));
      if (str->append(STRING_WITH_LEN("RETURNED_SQLSTATE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#ifdef SPIDER_TOKEN_10_3
    case RETURN_MARIADB_SYM:
      DBUG_PRINT("info",("spider RETURN_MARIADB_SYM"));
      if (str->append(STRING_WITH_LEN("RETURN "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case RETURN_ORACLE_SYM:
      DBUG_PRINT("info",("spider RETURN_ORACLE_SYM"));
      if (str->append(STRING_WITH_LEN("RETURN "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#else
    case RETURN_SYM:
      DBUG_PRINT("info",("spider RETURN_SYM"));
      if (str->append(STRING_WITH_LEN("RETURN "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#endif
    case RETURNING_SYM:
      DBUG_PRINT("info",("spider RETURNING_SYM"));
      if (str->append(STRING_WITH_LEN("RETURNING "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case RETURNS_SYM:
      DBUG_PRINT("info",("spider RETURNS_SYM"));
      if (str->append(STRING_WITH_LEN("RETURNS "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case REUSE_SYM:
      DBUG_PRINT("info",("spider REUSE_SYM"));
      if (str->append(STRING_WITH_LEN("REUSE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case REVERSE_SYM:
      DBUG_PRINT("info",("spider REVERSE_SYM"));
      if (str->append(STRING_WITH_LEN("REVERSE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case REVOKE:
      DBUG_PRINT("info",("spider REVOKE"));
      if (str->append(STRING_WITH_LEN("REVOKE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case RIGHT:
      DBUG_PRINT("info",("spider RIGHT"));
      if (str->append(STRING_WITH_LEN("RIGHT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case ROLE_SYM:
      DBUG_PRINT("info",("spider ROLE_SYM"));
      if (str->append(STRING_WITH_LEN("ROLE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case ROLLBACK_SYM:
      DBUG_PRINT("info",("spider ROLLBACK_SYM"));
      if (str->append(STRING_WITH_LEN("ROLLBACK "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case ROLLUP_SYM:
      DBUG_PRINT("info",("spider ROLLUP_SYM"));
      if (str->append(STRING_WITH_LEN("ROLLUP "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case ROUTINE_SYM:
      DBUG_PRINT("info",("spider ROUTINE_SYM"));
      if (str->append(STRING_WITH_LEN("ROUTINE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case ROW_SYM:
      DBUG_PRINT("info",("spider ROW_SYM"));
      if (str->append(STRING_WITH_LEN("ROW "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case ROWCOUNT_SYM:
      DBUG_PRINT("info",("spider ROWCOUNT_SYM"));
      if (str->append(STRING_WITH_LEN("ROWCOUNT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case ROWS_SYM:
      DBUG_PRINT("info",("spider ROWS_SYM"));
      if (str->append(STRING_WITH_LEN("ROWS "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#ifdef SPIDER_TOKEN_10_3
    case ROWTYPE_MARIADB_SYM:
      DBUG_PRINT("info",("spider ROWTYPE_MARIADB_SYM"));
      if (str->append(STRING_WITH_LEN("ROWTYPE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case ROWTYPE_ORACLE_SYM:
      DBUG_PRINT("info",("spider ROWTYPE_ORACLE_SYM"));
      if (str->append(STRING_WITH_LEN("ROWTYPE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#else
    case ROWTYPE_SYM:
      DBUG_PRINT("info",("spider ROWTYPE_SYM"));
      if (str->append(STRING_WITH_LEN("ROWTYPE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#endif
    case ROW_COUNT_SYM:
      DBUG_PRINT("info",("spider ROW_COUNT_SYM"));
      if (str->append(STRING_WITH_LEN("ROW_COUNT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case ROW_FORMAT_SYM:
      DBUG_PRINT("info",("spider ROW_FORMAT_SYM"));
      if (str->append(STRING_WITH_LEN("ROW_FORMAT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case RTREE_SYM:
      DBUG_PRINT("info",("spider RTREE_SYM"));
      if (str->append(STRING_WITH_LEN("RTREE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case SAVEPOINT_SYM:
      DBUG_PRINT("info",("spider SAVEPOINT_SYM"));
      if (str->append(STRING_WITH_LEN("SAVEPOINT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case SCHEDULE_SYM:
      DBUG_PRINT("info",("spider SCHEDULE_SYM"));
      if (str->append(STRING_WITH_LEN("SCHEDULE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case SCHEMA_NAME_SYM:
      DBUG_PRINT("info",("spider SCHEMA_NAME_SYM"));
      if (str->append(STRING_WITH_LEN("SCHEMA_NAME "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case SECOND_SYM:
      DBUG_PRINT("info",("spider SECOND_SYM"));
      if (str->append(STRING_WITH_LEN("SECOND "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case SECOND_MICROSECOND_SYM:
      DBUG_PRINT("info",("spider SECOND_MICROSECOND_SYM"));
      if (str->append(STRING_WITH_LEN("SECOND_MICROSECOND "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case SECURITY_SYM:
      DBUG_PRINT("info",("spider SECURITY_SYM"));
      if (str->append(STRING_WITH_LEN("SECURITY "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case SELECT_SYM: /* SELECT */
      DBUG_PRINT("info",("spider SELECT_SYM"));
      if (str->append(STRING_WITH_LEN("SELECT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case SENSITIVE_SYM:
      DBUG_PRINT("info",("spider SENSITIVE_SYM"));
      if (str->append(STRING_WITH_LEN("SENSITIVE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case SEPARATOR_SYM:
      DBUG_PRINT("info",("spider SEPARATOR_SYM"));
      if (str->append(STRING_WITH_LEN("SEPARATOR "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case SEQUENCE_SYM:
      DBUG_PRINT("info",("spider SEQUENCE_SYM"));
      if (str->append(STRING_WITH_LEN("SEQUENCE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case SERIAL_SYM:
      DBUG_PRINT("info",("spider SERIAL_SYM"));
      if (str->append(STRING_WITH_LEN("SERIAL "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case SERIALIZABLE_SYM:
      DBUG_PRINT("info",("spider SERIALIZABLE_SYM"));
      if (str->append(STRING_WITH_LEN("SERIALIZABLE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case SESSION_SYM:
      DBUG_PRINT("info",("spider SESSION_SYM"));
      if (str->append(STRING_WITH_LEN("SESSION "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#ifdef SPIDER_TOKEN_10_3
    case SERVER_OPTIONS:
      DBUG_PRINT("info",("spider SERVER_OPTIONS"));
      if (str->append(STRING_WITH_LEN("SERVER_OPTIONS "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#endif
    case SERVER_SYM:
      DBUG_PRINT("info",("spider SERVER_SYM"));
      if (str->append(STRING_WITH_LEN("SERVER "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case SET:
      DBUG_PRINT("info",("spider SET"));
      if (str->append(STRING_WITH_LEN("SET "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case SETVAL_SYM:
      DBUG_PRINT("info",("spider SETVAL_SYM"));
      if (str->append(STRING_WITH_LEN("SETVAL "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case SHARE_SYM:
      DBUG_PRINT("info",("spider SHARE_SYM"));
      if (str->append(STRING_WITH_LEN("SHARE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case SHOW:
      DBUG_PRINT("info",("spider SHOW"));
      if (str->append(STRING_WITH_LEN("SHOW "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case SHUTDOWN:
      DBUG_PRINT("info",("spider SHUTDOWN"));
      if (str->append(STRING_WITH_LEN("SHUTDOWN "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case SIGNAL_SYM:
      DBUG_PRINT("info",("spider SIGNAL_SYM"));
      if (str->append(STRING_WITH_LEN("SIGNAL "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case SIGNED_SYM:
      DBUG_PRINT("info",("spider SIGNED_SYM"));
      if (str->append(STRING_WITH_LEN("SIGNED "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case SIMPLE_SYM:
      DBUG_PRINT("info",("spider SIMPLE_SYM"));
      if (str->append(STRING_WITH_LEN("SIMPLE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case SLAVE:
      DBUG_PRINT("info",("spider SLAVE"));
      if (str->append(STRING_WITH_LEN("SLAVE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case SLAVES:
      DBUG_PRINT("info",("spider SLAVES"));
      if (str->append(STRING_WITH_LEN("SLAVES "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case SLAVE_POS_SYM:
      DBUG_PRINT("info",("spider SLAVE_POS_SYM"));
      if (str->append(STRING_WITH_LEN("SLAVE_POS "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case SLOW:
      DBUG_PRINT("info",("spider SLOW"));
      if (str->append(STRING_WITH_LEN("SLOW "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case SNAPSHOT_SYM:
      DBUG_PRINT("info",("spider SNAPSHOT_SYM"));
      if (str->append(STRING_WITH_LEN("SNAPSHOT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case SMALLINT:
      DBUG_PRINT("info",("spider SMALLINT"));
      if (str->append(STRING_WITH_LEN("SMALLINT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case SOCKET_SYM:
      DBUG_PRINT("info",("spider SOCKET_SYM"));
      if (str->append(STRING_WITH_LEN("SOCKET "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case SOFT_SYM:
      DBUG_PRINT("info",("spider SOFT_SYM"));
      if (str->append(STRING_WITH_LEN("SOFT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case SONAME_SYM:
      DBUG_PRINT("info",("spider SONAME_SYM"));
      if (str->append(STRING_WITH_LEN("SONAME "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case SOUNDS_SYM:
      DBUG_PRINT("info",("spider SOUNDS_SYM"));
      if (str->append(STRING_WITH_LEN("SOUNDS "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case SOURCE_SYM:
      DBUG_PRINT("info",("spider SOURCE_SYM"));
      if (str->append(STRING_WITH_LEN("SOURCE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case STORED_SYM:
      DBUG_PRINT("info",("spider STORED_SYM"));
      if (str->append(STRING_WITH_LEN("STORED "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case SPATIAL_SYM:
      DBUG_PRINT("info",("spider SPATIAL_SYM"));
      if (str->append(STRING_WITH_LEN("SPATIAL "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case SPECIFIC_SYM:
      DBUG_PRINT("info",("spider SPECIFIC_SYM"));
      if (str->append(STRING_WITH_LEN("SPECIFIC "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case REF_SYSTEM_ID_SYM:
      DBUG_PRINT("info",("spider REF_SYSTEM_ID_SYM"));
      if (str->append(STRING_WITH_LEN("REF_SYSTEM_ID "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case SQL_SYM:
      DBUG_PRINT("info",("spider SQL_SYM"));
      if (str->append(STRING_WITH_LEN("SQL "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case SQLEXCEPTION_SYM:
      DBUG_PRINT("info",("spider SQLEXCEPTION_SYM"));
      if (str->append(STRING_WITH_LEN("SQLEXCEPTION "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case SQLSTATE_SYM:
      DBUG_PRINT("info",("spider SQLSTATE_SYM"));
      if (str->append(STRING_WITH_LEN("SQLSTATE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case SQLWARNING_SYM:
      DBUG_PRINT("info",("spider SQLWARNING_SYM"));
      if (str->append(STRING_WITH_LEN("SQLWARNING "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case SQL_BIG_RESULT:
      DBUG_PRINT("info",("spider SQL_BIG_RESULT"));
      if (str->append(STRING_WITH_LEN("SQL_BIG_RESULT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case SQL_BUFFER_RESULT:
      DBUG_PRINT("info",("spider SQL_BUFFER_RESULT"));
      if (str->append(STRING_WITH_LEN("SQL_BUFFER_RESULT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case SQL_CACHE_SYM:
      DBUG_PRINT("info",("spider SQL_CACHE_SYM"));
      if (str->append(STRING_WITH_LEN("SQL_CACHE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case SQL_CALC_FOUND_ROWS:
      DBUG_PRINT("info",("spider SQL_CALC_FOUND_ROWS"));
      if (str->append(STRING_WITH_LEN("SQL_CALC_FOUND_ROWS "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case SQL_NO_CACHE_SYM:
      DBUG_PRINT("info",("spider SQL_NO_CACHE_SYM"));
      if (str->append(STRING_WITH_LEN("SQL_NO_CACHE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case SQL_SMALL_RESULT:
      DBUG_PRINT("info",("spider SQL_SMALL_RESULT"));
      if (str->append(STRING_WITH_LEN("SQL_SMALL_RESULT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case SQL_THREAD:
      DBUG_PRINT("info",("spider SQL_THREAD"));
      if (str->append(STRING_WITH_LEN("SQL_THREAD "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case SSL_SYM:
      DBUG_PRINT("info",("spider SSL_SYM"));
      if (str->append(STRING_WITH_LEN("SSL "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#ifdef SPIDER_TOKEN_10_4
    case STAGE_SYM:
      DBUG_PRINT("info",("spider STAGE_SYM"));
      if (str->append(STRING_WITH_LEN("STAGE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#endif
    case START_SYM:
      DBUG_PRINT("info",("spider START_SYM"));
      if (str->append(STRING_WITH_LEN("START "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case STARTING:
      DBUG_PRINT("info",("spider STARTING"));
      if (str->append(STRING_WITH_LEN("STARTING "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case STARTS_SYM:
      DBUG_PRINT("info",("spider STARTS_SYM"));
      if (str->append(STRING_WITH_LEN("STARTS "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case STATEMENT_SYM:
      DBUG_PRINT("info",("spider STATEMENT_SYM"));
      if (str->append(STRING_WITH_LEN("STATEMENT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case STATS_AUTO_RECALC_SYM:
      DBUG_PRINT("info",("spider STATS_AUTO_RECALC_SYM"));
      if (str->append(STRING_WITH_LEN("STATS_AUTO_RECALC "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case STATS_PERSISTENT_SYM:
      DBUG_PRINT("info",("spider STATS_PERSISTENT_SYM"));
      if (str->append(STRING_WITH_LEN("STATS_PERSISTENT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case STATS_SAMPLE_PAGES_SYM:
      DBUG_PRINT("info",("spider STATS_SAMPLE_PAGES_SYM"));
      if (str->append(STRING_WITH_LEN("STATS_SAMPLE_PAGES "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case STATUS_SYM:
      DBUG_PRINT("info",("spider STATUS_SYM"));
      if (str->append(STRING_WITH_LEN("STATUS "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case STOP_SYM:
      DBUG_PRINT("info",("spider STOP_SYM"));
      if (str->append(STRING_WITH_LEN("STOP "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case STORAGE_SYM:
      DBUG_PRINT("info",("spider STORAGE_SYM"));
      if (str->append(STRING_WITH_LEN("STORAGE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case STRAIGHT_JOIN:
      DBUG_PRINT("info",("spider STRAIGHT_JOIN"));
      if (str->append(STRING_WITH_LEN("STRAIGHT_JOIN "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case STRING_SYM:
      DBUG_PRINT("info",("spider STRING_SYM"));
      if (str->append(STRING_WITH_LEN("STRING "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case SUBCLASS_ORIGIN_SYM:
      DBUG_PRINT("info",("spider SUBCLASS_ORIGIN_SYM"));
      if (str->append(STRING_WITH_LEN("SUBCLASS_ORIGIN "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case SUBJECT_SYM:
      DBUG_PRINT("info",("spider SUBJECT_SYM"));
      if (str->append(STRING_WITH_LEN("SUBJECT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case SUBPARTITION_SYM:
      DBUG_PRINT("info",("spider SUBPARTITION_SYM"));
      if (str->append(STRING_WITH_LEN("SUBPARTITION "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case SUBPARTITIONS_SYM:
      DBUG_PRINT("info",("spider SUBPARTITIONS_SYM"));
      if (str->append(STRING_WITH_LEN("SUBPARTITIONS "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case SUPER_SYM:
      DBUG_PRINT("info",("spider SUPER_SYM"));
      if (str->append(STRING_WITH_LEN("SUPER "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case SUSPEND_SYM:
      DBUG_PRINT("info",("spider SUSPEND_SYM"));
      if (str->append(STRING_WITH_LEN("SUSPEND "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case SWAPS_SYM:
      DBUG_PRINT("info",("spider SWAPS_SYM"));
      if (str->append(STRING_WITH_LEN("SWAPS "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case SWITCHES_SYM:
      DBUG_PRINT("info",("spider SWITCHES_SYM"));
      if (str->append(STRING_WITH_LEN("SWITCHES "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case SYSTEM:
      DBUG_PRINT("info",("spider SYSTEM"));
      if (str->append(STRING_WITH_LEN("SYSTEM "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case SYSTEM_TIME_SYM:
      DBUG_PRINT("info",("spider SYSTEM_TIME_SYM"));
      if (str->append(STRING_WITH_LEN("SYSTEM_TIME "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case TABLE_SYM:
      DBUG_PRINT("info",("spider TABLE_SYM"));
      if (str->append(STRING_WITH_LEN("TABLE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case TABLE_NAME_SYM:
      DBUG_PRINT("info",("spider TABLE_NAME_SYM"));
      if (str->append(STRING_WITH_LEN("TABLE_NAME "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#ifdef SPIDER_TOKEN_10_3
    case TABLE_REF_PRIORITY:
      DBUG_PRINT("info",("spider TABLE_REF_PRIORITY"));
      if (str->append(STRING_WITH_LEN("TABLE_REF_PRIORITY "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#endif
    case TABLES:
      DBUG_PRINT("info",("spider TABLES"));
      if (str->append(STRING_WITH_LEN("TABLES "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case TABLESPACE:
      DBUG_PRINT("info",("spider TABLESPACE"));
      if (str->append(STRING_WITH_LEN("TABLESPACE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case TABLE_CHECKSUM_SYM:
      DBUG_PRINT("info",("spider TABLE_CHECKSUM_SYM"));
      if (str->append(STRING_WITH_LEN("TABLE_CHECKSUM "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case TEMPORARY:
      DBUG_PRINT("info",("spider TEMPORARY"));
      if (str->append(STRING_WITH_LEN("TEMPORARY "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case TEMPTABLE_SYM:
      DBUG_PRINT("info",("spider TEMPTABLE_SYM"));
      if (str->append(STRING_WITH_LEN("TEMPTABLE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case TERMINATED:
      DBUG_PRINT("info",("spider TERMINATED"));
      if (str->append(STRING_WITH_LEN("TERMINATED "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case TEXT_SYM:
      DBUG_PRINT("info",("spider TEXT_SYM"));
      if (str->append(STRING_WITH_LEN("TEXT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case THAN_SYM:
      DBUG_PRINT("info",("spider THAN_SYM"));
      if (str->append(STRING_WITH_LEN("THAN "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case THEN_SYM:
      DBUG_PRINT("info",("spider THEN_SYM"));
      if (str->append(STRING_WITH_LEN("THEN "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case TIES_SYM:
      DBUG_PRINT("info",("spider TIES_SYM"));
      if (str->append(STRING_WITH_LEN("TIES "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case TIME_SYM:
      DBUG_PRINT("info",("spider TIME_SYM"));
      if (str->append(STRING_WITH_LEN("TIME "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case TIMESTAMP:
      DBUG_PRINT("info",("spider TIMESTAMP"));
      if (str->append(STRING_WITH_LEN("TIMESTAMP "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case TIMESTAMP_ADD:
      DBUG_PRINT("info",("spider TIMESTAMP_ADD"));
      if (str->append(STRING_WITH_LEN("TIMESTAMPADD "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case TIMESTAMP_DIFF:
      DBUG_PRINT("info",("spider TIMESTAMP_DIFF"));
      if (str->append(STRING_WITH_LEN("TIMESTAMPDIFF "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case TINYBLOB:
      DBUG_PRINT("info",("spider TINYBLOB"));
      if (str->append(STRING_WITH_LEN("TINYBLOB "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case TINYINT:
      DBUG_PRINT("info",("spider TINYINT"));
      if (str->append(STRING_WITH_LEN("TINYINT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case TINYTEXT:
      DBUG_PRINT("info",("spider TINYTEXT"));
      if (str->append(STRING_WITH_LEN("TINYTEXT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case TO_SYM: /* TO */
      DBUG_PRINT("info",("spider TO_SYM"));
      if (str->append(STRING_WITH_LEN("TO "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case TRAILING:
      DBUG_PRINT("info",("spider TRAILING"));
      if (str->append(STRING_WITH_LEN("TRAILING "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case TRANSACTION_SYM:
      DBUG_PRINT("info",("spider TRANSACTION_SYM"));
      if (str->append(STRING_WITH_LEN("TRANSACTION "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case TRANSACTIONAL_SYM:
      DBUG_PRINT("info",("spider TRANSACTIONAL_SYM"));
      if (str->append(STRING_WITH_LEN("TRANSACTIONAL "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case TRIGGER_SYM:
      DBUG_PRINT("info",("spider TRIGGER_SYM"));
      if (str->append(STRING_WITH_LEN("TRIGGER "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case TRIGGERS_SYM:
      DBUG_PRINT("info",("spider TRIGGERS_SYM"));
      if (str->append(STRING_WITH_LEN("TRIGGERS "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case TRUE_SYM:
      DBUG_PRINT("info",("spider TRUE_SYM"));
      if (str->append(STRING_WITH_LEN("TRUE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case TRUNCATE_SYM:
      DBUG_PRINT("info",("spider TRUNCATE_SYM"));
      if (str->append(STRING_WITH_LEN("TRUNCATE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case TYPE_SYM:
      DBUG_PRINT("info",("spider TYPE_SYM"));
      if (str->append(STRING_WITH_LEN("TYPE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case TYPES_SYM:
      DBUG_PRINT("info",("spider TYPES_SYM"));
      if (str->append(STRING_WITH_LEN("TYPES "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#ifdef SPIDER_TOKEN_10_3
    case UDF_RETURNS_SYM:
      DBUG_PRINT("info",("spider UDF_RETURNS_SYM"));
      if (str->append(STRING_WITH_LEN("UDF_RETURNS "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#endif
    case UNBOUNDED_SYM:
      DBUG_PRINT("info",("spider UNBOUNDED_SYM"));
      if (str->append(STRING_WITH_LEN("UNBOUNDED "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case UNCOMMITTED_SYM:
      DBUG_PRINT("info",("spider UNCOMMITTED_SYM"));
      if (str->append(STRING_WITH_LEN("UNCOMMITTED "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case UNDEFINED_SYM:
      DBUG_PRINT("info",("spider UNDEFINED_SYM"));
      if (str->append(STRING_WITH_LEN("UNDEFINED "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case UNDO_BUFFER_SIZE_SYM:
      DBUG_PRINT("info",("spider UNDO_BUFFER_SIZE_SYM"));
      if (str->append(STRING_WITH_LEN("UNDO_BUFFER_SIZE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case UNDOFILE_SYM:
      DBUG_PRINT("info",("spider UNDOFILE_SYM"));
      if (str->append(STRING_WITH_LEN("UNDOFILE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case UNDO_SYM:
      DBUG_PRINT("info",("spider UNDO_SYM"));
      if (str->append(STRING_WITH_LEN("UNDO "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case UNICODE_SYM:
      DBUG_PRINT("info",("spider UNICODE_SYM"));
      if (str->append(STRING_WITH_LEN("UNICODE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case UNION_SYM:
      DBUG_PRINT("info",("spider UNION_SYM"));
      if (str->append(STRING_WITH_LEN("UNION "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case UNIQUE_SYM:
      DBUG_PRINT("info",("spider UNIQUE_SYM"));
      if (str->append(STRING_WITH_LEN("UNIQUE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case UNKNOWN_SYM:
      DBUG_PRINT("info",("spider UNKNOWN_SYM"));
      if (str->append(STRING_WITH_LEN("UNKNOWN "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case UNLOCK_SYM:
      DBUG_PRINT("info",("spider UNLOCK_SYM"));
      if (str->append(STRING_WITH_LEN("UNLOCK "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case UNINSTALL_SYM:
      DBUG_PRINT("info",("spider UNINSTALL_SYM"));
      if (str->append(STRING_WITH_LEN("UNINSTALL "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case UNSIGNED:
      DBUG_PRINT("info",("spider UNSIGNED"));
      if (str->append(STRING_WITH_LEN("UNSIGNED "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case UNTIL_SYM:
      DBUG_PRINT("info",("spider UNTIL_SYM"));
      if (str->append(STRING_WITH_LEN("UNTIL "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case UPDATE_SYM:
      DBUG_PRINT("info",("spider UPDATE_SYM"));
      if (str->append(STRING_WITH_LEN("UPDATE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case UPGRADE_SYM:
      DBUG_PRINT("info",("spider UPGRADE_SYM"));
      if (str->append(STRING_WITH_LEN("UPGRADE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case USAGE:
      DBUG_PRINT("info",("spider USAGE"));
      if (str->append(STRING_WITH_LEN("USAGE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case USE_SYM:
      DBUG_PRINT("info",("spider USE_SYM"));
      if (str->append(STRING_WITH_LEN("USE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#ifdef SPIDER_TOKEN_10_3
    case USER:
      DBUG_PRINT("info",("spider USER"));
      if (str->append(STRING_WITH_LEN("USER "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#endif
    case USER_SYM: /* USER */
      DBUG_PRINT("info",("spider USER_SYM"));
      if (str->append(STRING_WITH_LEN("USER "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case RESOURCES:
      DBUG_PRINT("info",("spider RESOURCES"));
      if (str->append(STRING_WITH_LEN("USER_RESOURCES "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case USE_FRM:
      DBUG_PRINT("info",("spider USE_FRM"));
      if (str->append(STRING_WITH_LEN("USE_FRM "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case USING: /* USING */
      DBUG_PRINT("info",("spider USING"));
      if (str->append(STRING_WITH_LEN("USING "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case UTC_DATE_SYM:
      DBUG_PRINT("info",("spider UTC_DATE_SYM"));
      if (str->append(STRING_WITH_LEN("UTC_DATE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case UTC_TIME_SYM:
      DBUG_PRINT("info",("spider UTC_TIME_SYM"));
      if (str->append(STRING_WITH_LEN("UTC_TIME "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case UTC_TIMESTAMP_SYM:
      DBUG_PRINT("info",("spider UTC_TIMESTAMP_SYM"));
      if (str->append(STRING_WITH_LEN("UTC_TIMESTAMP "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case VALUE_SYM:
      DBUG_PRINT("info",("spider VALUE_SYM"));
      if (str->append(STRING_WITH_LEN("VALUE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case VALUES:
      DBUG_PRINT("info",("spider VALUES"));
      if (str->append(STRING_WITH_LEN("VALUES "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#ifdef SPIDER_TOKEN_10_3
    case VALUES_IN_SYM:
      DBUG_PRINT("info",("spider VALUES_IN_SYM"));
      if (str->append(STRING_WITH_LEN("VALUES IN "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case VALUES_LESS_SYM:
      DBUG_PRINT("info",("spider VALUES_LESS_SYM"));
      if (str->append(STRING_WITH_LEN("VALUES LESS "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#endif
    case VARBINARY:
      DBUG_PRINT("info",("spider VARBINARY"));
      if (str->append(STRING_WITH_LEN("VARBINARY "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case VARCHAR:
      DBUG_PRINT("info",("spider VARCHAR"));
      if (str->append(STRING_WITH_LEN("VARCHAR "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#ifdef SPIDER_TOKEN_10_3
    case VARCHAR2_MARIADB_SYM:
      DBUG_PRINT("info",("spider VARCHAR2_MARIADB_SYM"));
      if (str->append(STRING_WITH_LEN("VARCHAR2 "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case VARCHAR2_ORACLE_SYM:
      DBUG_PRINT("info",("spider VARCHAR2_ORACLE_SYM"));
      if (str->append(STRING_WITH_LEN("VARCHAR2 "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#else
    case VARCHAR2:
      DBUG_PRINT("info",("spider VARCHAR2"));
      if (str->append(STRING_WITH_LEN("VARCHAR2 "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#endif
    case VARIABLES:
      DBUG_PRINT("info",("spider VARIABLES"));
      if (str->append(STRING_WITH_LEN("VARIABLES "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case VARYING:
      DBUG_PRINT("info",("spider VARYING"));
      if (str->append(STRING_WITH_LEN("VARYING "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case VIA_SYM:
      DBUG_PRINT("info",("spider VIA_SYM"));
      if (str->append(STRING_WITH_LEN("VIA "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case VIEW_SYM:
      DBUG_PRINT("info",("spider VIEW_SYM"));
      if (str->append(STRING_WITH_LEN("VIEW "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case VIRTUAL_SYM:
      DBUG_PRINT("info",("spider VIRTUAL_SYM"));
      if (str->append(STRING_WITH_LEN("VIRTUAL "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case VERSIONING_SYM:
      DBUG_PRINT("info",("spider VERSIONING_SYM"));
      if (str->append(STRING_WITH_LEN("VERSIONING "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case WAIT_SYM:
      DBUG_PRINT("info",("spider WAIT_SYM"));
      if (str->append(STRING_WITH_LEN("WAIT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case WARNINGS:
      DBUG_PRINT("info",("spider WARNINGS"));
      if (str->append(STRING_WITH_LEN("WARNINGS "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case WEEK_SYM:
      DBUG_PRINT("info",("spider WEEK_SYM"));
      if (str->append(STRING_WITH_LEN("WEEK "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case WEIGHT_STRING_SYM:
      DBUG_PRINT("info",("spider WEIGHT_STRING_SYM"));
      if (str->append(STRING_WITH_LEN("WEIGHT_STRING "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case WHEN_SYM:
      DBUG_PRINT("info",("spider WHEN_SYM"));
      if (str->append(STRING_WITH_LEN("WHEN "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case WHERE:
      DBUG_PRINT("info",("spider WHERE"));
      if (str->append(STRING_WITH_LEN("WHERE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case WHILE_SYM:
      DBUG_PRINT("info",("spider WHILE_SYM"));
      if (str->append(STRING_WITH_LEN("WHILE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case WINDOW_SYM :
      DBUG_PRINT("info",("spider WINDOW_SYM "));
      if (str->append(STRING_WITH_LEN("WINDOW "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case WITH:
      DBUG_PRINT("info",("spider WITH"));
      if (str->append(STRING_WITH_LEN("WITH "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#ifdef SPIDER_TOKEN_10_3
    case WITH_CUBE_SYM:
      DBUG_PRINT("info",("spider WITH_CUBE_SYM"));
      if (str->append(STRING_WITH_LEN("WITH_CUBE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case WITH_ROLLUP_SYM:
      DBUG_PRINT("info",("spider WITH_ROLLUP_SYM"));
      if (str->append(STRING_WITH_LEN("WITH_ROLLUP "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case WITH_SYSTEM_SYM:
      DBUG_PRINT("info",("spider WITH_SYSTEM_SYM"));
      if (str->append(STRING_WITH_LEN("WITH_SYSTEM "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#endif
    case WITHIN:
      DBUG_PRINT("info",("spider WITHIN"));
      if (str->append(STRING_WITH_LEN("WITHIN "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case WITHOUT:
      DBUG_PRINT("info",("spider WITHOUT"));
      if (str->append(STRING_WITH_LEN("WITHOUT "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case WORK_SYM:
      DBUG_PRINT("info",("spider WORK_SYM"));
      if (str->append(STRING_WITH_LEN("WORK "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case WRAPPER_SYM:
      DBUG_PRINT("info",("spider WRAPPER_SYM"));
      if (str->append(STRING_WITH_LEN("WRAPPER "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case WRITE_SYM:
      DBUG_PRINT("info",("spider WRITE_SYM"));
      if (str->append(STRING_WITH_LEN("WRITE "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case X509_SYM:
      DBUG_PRINT("info",("spider X509_SYM"));
      if (str->append(STRING_WITH_LEN("X509 "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case XOR:
      DBUG_PRINT("info",("spider XOR"));
      if (str->append(STRING_WITH_LEN("XOR "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case XA_SYM:
      DBUG_PRINT("info",("spider XA_SYM"));
      if (str->append(STRING_WITH_LEN("XA "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case XML_SYM:
      DBUG_PRINT("info",("spider XML_SYM"));
      if (str->append(STRING_WITH_LEN("XML "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case YEAR_SYM:
      DBUG_PRINT("info",("spider YEAR_SYM"));
      if (str->append(STRING_WITH_LEN("YEAR "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case YEAR_MONTH_SYM:
      DBUG_PRINT("info",("spider YEAR_MONTH_SYM"));
      if (str->append(STRING_WITH_LEN("YEAR_MONTH "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case ZEROFILL:
      DBUG_PRINT("info",("spider ZEROFILL"));
      if (str->append(STRING_WITH_LEN("ZEROFILL "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case OR2_SYM:
      DBUG_PRINT("info",("spider OR2_SYM"));
      if (str->append(STRING_WITH_LEN("|| "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;

    /* functions */
    case ADDDATE_SYM:
      DBUG_PRINT("info",("spider ADDDATE_SYM"));
      if (str->append(STRING_WITH_LEN("ADDDATE"))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case BIT_AND:
      DBUG_PRINT("info",("spider BIT_AND"));
      if (str->append(STRING_WITH_LEN("BIT_AND"))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case BIT_OR:
      DBUG_PRINT("info",("spider BIT_OR"));
      if (str->append(STRING_WITH_LEN("BIT_OR"))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case BIT_XOR:
      DBUG_PRINT("info",("spider BIT_XOR"));
      if (str->append(STRING_WITH_LEN("BIT_XOR"))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case CAST_SYM:
      DBUG_PRINT("info",("spider CAST_SYM"));
      if (str->append(STRING_WITH_LEN("CAST"))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case COUNT_SYM:
      DBUG_PRINT("info",("spider COUNT_SYM"));
      if (str->append(STRING_WITH_LEN("COUNT"))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case CUME_DIST_SYM:
      DBUG_PRINT("info",("spider CUME_DIST_SYM"));
      if (str->append(STRING_WITH_LEN("CUME_DIST"))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case DATE_ADD_INTERVAL:
      DBUG_PRINT("info",("spider DATE_ADD_INTERVAL"));
      if (str->append(STRING_WITH_LEN("DATE_ADD"))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case DATE_SUB_INTERVAL:
      DBUG_PRINT("info",("spider DATE_SUB_INTERVAL"));
      if (str->append(STRING_WITH_LEN("DATE_SUB"))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case DATE_FORMAT_SYM:
      DBUG_PRINT("info",("spider DATE_FORMAT_SYM"));
      if (str->append(STRING_WITH_LEN("DATE_FORMAT"))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#ifdef SPIDER_TOKEN_10_3
    case DECODE_MARIADB_SYM:
      DBUG_PRINT("info",("spider DECODE_MARIADB_SYM"));
      if (str->append(STRING_WITH_LEN("DECODE"))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case DECODE_ORACLE_SYM:
      DBUG_PRINT("info",("spider DECODE_ORACLE_SYM"));
      if (str->append(STRING_WITH_LEN("DECODE"))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#else
    case DECODE_SYM:
      DBUG_PRINT("info",("spider DECODE_SYM"));
      if (str->append(STRING_WITH_LEN("DECODE"))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#endif
    case DENSE_RANK_SYM:
      DBUG_PRINT("info",("spider DENSE_RANK_SYM"));
      if (str->append(STRING_WITH_LEN("DENSE_RANK"))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case EXTRACT_SYM:
      DBUG_PRINT("info",("spider EXTRACT_SYM"));
      if (str->append(STRING_WITH_LEN("EXTRACT"))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case FIRST_VALUE_SYM:
      DBUG_PRINT("info",("spider FIRST_VALUE_SYM"));
      if (str->append(STRING_WITH_LEN("FIRST_VALUE"))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case GROUP_CONCAT_SYM:
      DBUG_PRINT("info",("spider GROUP_CONCAT_SYM"));
      if (str->append(STRING_WITH_LEN("GROUP_CONCAT"))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case LAG_SYM:
      DBUG_PRINT("info",("spider LAG_SYM"));
      if (str->append(STRING_WITH_LEN("LAG"))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case LEAD_SYM:
      DBUG_PRINT("info",("spider LEAD_SYM"));
      if (str->append(STRING_WITH_LEN("LEAD"))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case MAX_SYM:
      DBUG_PRINT("info",("spider MAX_SYM"));
      if (str->append(STRING_WITH_LEN("MAX"))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case MEDIAN_SYM:
      DBUG_PRINT("info",("spider MEDIAN_SYM"));
      if (str->append(STRING_WITH_LEN("MEDIAN"))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case MIN_SYM:
      DBUG_PRINT("info",("spider MIN_SYM"));
      if (str->append(STRING_WITH_LEN("MIN"))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case NTH_VALUE_SYM:
      DBUG_PRINT("info",("spider NTH_VALUE_SYM"));
      if (str->append(STRING_WITH_LEN("NTH_VALUE"))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case NTILE_SYM:
      DBUG_PRINT("info",("spider NTILE_SYM"));
      if (str->append(STRING_WITH_LEN("NTILE"))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#ifdef SPIDER_TOKEN_10_4
    case PORTION_SYM:
      DBUG_PRINT("info",("spider PORTION_SYM"));
      if (str->append(STRING_WITH_LEN("PORTION "))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#endif
    case POSITION_SYM:
      DBUG_PRINT("info",("spider POSITION_SYM"));
      if (str->append(STRING_WITH_LEN("POSITION"))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#ifdef SPIDER_TOKEN_10_3
    case PERCENT_ORACLE_SYM:
      DBUG_PRINT("info",("spider PERCENT_ORACLE_SYM"));
      if (str->append(STRING_WITH_LEN("%"))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
#endif
    case PERCENT_RANK_SYM:
      DBUG_PRINT("info",("spider PERCENT_RANK_SYM"));
      if (str->append(STRING_WITH_LEN("PERCENT_RANK"))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case PERCENTILE_CONT_SYM:
      DBUG_PRINT("info",("spider PERCENTILE_CONT_SYM"));
      if (str->append(STRING_WITH_LEN("PERCENTILE_CONT"))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case PERCENTILE_DISC_SYM:
      DBUG_PRINT("info",("spider PERCENTILE_DISC_SYM"));
      if (str->append(STRING_WITH_LEN("PERCENTILE_DISC"))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case RANK_SYM:
      DBUG_PRINT("info",("spider RANK_SYM"));
      if (str->append(STRING_WITH_LEN("RANK"))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case ROW_NUMBER_SYM:
      DBUG_PRINT("info",("spider ROW_NUMBER_SYM"));
      if (str->append(STRING_WITH_LEN("ROW_NUMBER"))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case STD_SYM:
      DBUG_PRINT("info",("spider STD_SYM"));
      if (str->append(STRING_WITH_LEN("STD"))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case STDDEV_SAMP_SYM:
      DBUG_PRINT("info",("spider STDDEV_SAMP_SYM"));
      if (str->append(STRING_WITH_LEN("STDDEV_SAMP"))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case SUBDATE_SYM:
      DBUG_PRINT("info",("spider SUBDATE_SYM"));
      if (str->append(STRING_WITH_LEN("SUBDATE"))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case SUBSTRING:
      DBUG_PRINT("info",("spider SUBSTRING"));
      if (str->append(STRING_WITH_LEN("SUBSTRING"))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case SUM_SYM:
      DBUG_PRINT("info",("spider SUM_SYM"));
      if (str->append(STRING_WITH_LEN("SUM"))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case SYSDATE:
      DBUG_PRINT("info",("spider SYSDATE"));
      if (str->append(STRING_WITH_LEN("SYSDATE"))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case TRIM:
      DBUG_PRINT("info",("spider TRIM"));
      if (str->append(STRING_WITH_LEN("TRIM"))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case TRIM_ORACLE:
      DBUG_PRINT("info",("spider TRIM_ORACLE"));
      if (str->append(STRING_WITH_LEN("TRIM_ORACLE"))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case VARIANCE_SYM:
      DBUG_PRINT("info",("spider VARIANCE_SYM"));
      if (str->append(STRING_WITH_LEN("VARIANCE"))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;
    case VAR_SAMP_SYM:
      DBUG_PRINT("info",("spider VAR_SAMP_SYM"));
      if (str->append(STRING_WITH_LEN("VAR_SAMP"))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      break;

    case UNDERSCORE_CHARSET: /* like _utf8 */
      DBUG_PRINT("info",("spider UNDERSCORE_CHARSET"));
      {
        const char *name = yylval_tok->charset->name;
        uint len = strlen(name);
        if (str->reserve(len + 2)) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
        str->q_append(SPIDER_SQL_UNDERSCORE_STR, SPIDER_SQL_UNDERSCORE_LEN);
        str->q_append(name, len);
        str->q_append(SPIDER_SQL_SPACE_STR, SPIDER_SQL_SPACE_LEN);
      }
      break;
    case NCHAR_STRING: /* like n'string' */
      DBUG_PRINT("info",("spider NCHAR_STRING"));
      if (str->reserve(yylval_tok->lex_str.length * 2 + 3)) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      str->q_append(STRING_WITH_LEN("n"));
      str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
      str->append_for_single_quote(yylval_tok->lex_str.str, yylval_tok->lex_str.length);
      str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
      break;
    case IDENT_QUOTED: /* like `string` */
      DBUG_PRINT("info",("spider IDENT_QUOTED"));
      if (str->reserve(yylval_tok->lex_str.length + 2)) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      str->q_append(quote_char_for_ident.str, quote_char_for_ident.length);
      str->q_append(yylval_tok->lex_str.str, yylval_tok->lex_str.length);
      str->q_append(quote_char_for_ident.str, quote_char_for_ident.length);
      break;
    case IDENT: /* like _aaa */
      DBUG_PRINT("info",("spider IDENT"));
      if (str->reserve(yylval_tok->lex_str.length + 1)) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      str->q_append(yylval_tok->lex_str.str, yylval_tok->lex_str.length);
      str->q_append(SPIDER_SQL_SPACE_STR, SPIDER_SQL_SPACE_LEN);
      break;
    case HEX_NUM: /* like x'hex number' */
      DBUG_PRINT("info",("spider HEX_NUM"));
      if (str->reserve(yylval_tok->lex_str.length + 3)) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      str->q_append(STRING_WITH_LEN("0x"));
      str->q_append(yylval_tok->lex_str.str, yylval_tok->lex_str.length);
      str->q_append(SPIDER_SQL_SPACE_STR, SPIDER_SQL_SPACE_LEN);
      break;
    case BIN_NUM: /* like b'bin number' */
      DBUG_PRINT("info",("spider BIN_NUM"));
      if (str->reserve(yylval_tok->lex_str.length + 3)) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      str->q_append(STRING_WITH_LEN("0b"));
      str->q_append(yylval_tok->lex_str.str, yylval_tok->lex_str.length);
      str->q_append(SPIDER_SQL_SPACE_STR, SPIDER_SQL_SPACE_LEN);
      break;
    case FLOAT_NUM: /* like 1e+10 */
      DBUG_PRINT("info",("spider FLOAT_NUM"));
      if (str->reserve(yylval_tok->lex_str.length + 1)) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      str->q_append(yylval_tok->lex_str.str, yylval_tok->lex_str.length);
      str->q_append(SPIDER_SQL_SPACE_STR, SPIDER_SQL_SPACE_LEN);
      break;
    case NUM: /* like 10 */
      DBUG_PRINT("info",("spider NUM"));
      if (str->reserve(yylval_tok->lex_str.length + 1)) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      str->q_append(yylval_tok->lex_str.str, yylval_tok->lex_str.length);
      str->q_append(SPIDER_SQL_SPACE_STR, SPIDER_SQL_SPACE_LEN);
      break;
    case LONG_NUM: /* like 100000000000 */
      DBUG_PRINT("info",("spider LONG_NUM"));
      if (str->reserve(yylval_tok->lex_str.length + 1)) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      str->q_append(yylval_tok->lex_str.str, yylval_tok->lex_str.length);
      str->q_append(SPIDER_SQL_SPACE_STR, SPIDER_SQL_SPACE_LEN);
      break;
    case DECIMAL_NUM: /* like 1000000000000000000000 */
      DBUG_PRINT("info",("spider DECIMAL_NUM"));
      if (str->reserve(yylval_tok->lex_str.length + 1)) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      str->q_append(yylval_tok->lex_str.str, yylval_tok->lex_str.length);
      str->q_append(SPIDER_SQL_SPACE_STR, SPIDER_SQL_SPACE_LEN);
      break;
    case ULONGLONG_NUM: /* like 18446744073709551615 */
      DBUG_PRINT("info",("spider ULONGLONG_NUM"));
      if (str->reserve(yylval_tok->lex_str.length + 1)) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      str->q_append(yylval_tok->lex_str.str, yylval_tok->lex_str.length);
      str->q_append(SPIDER_SQL_SPACE_STR, SPIDER_SQL_SPACE_LEN);
      break;
    case HEX_STRING: /* like x'ab' */
      DBUG_PRINT("info",("spider HEX_STRING"));
      if (str->reserve(yylval_tok->lex_str.length + 3)) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      str->q_append(STRING_WITH_LEN("x"));
      str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
      str->q_append(yylval_tok->lex_str.str, yylval_tok->lex_str.length);
      str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
      break;
    case TEXT_STRING: /* like 'ab' */
      DBUG_PRINT("info",("spider TEXT_STRING"));
      if (str->reserve(yylval_tok->lex_str.length * 2 + 2)) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
      str->append_for_single_quote(yylval_tok->lex_str.str, yylval_tok->lex_str.length);
      str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
      break;
    case LEX_HOSTNAME: /* like @example.com */
      DBUG_PRINT("info",("spider LEX_HOSTNAME"));
      if (str->reserve(yylval_tok->lex_str.length + 1)) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);}
      str->q_append(yylval_tok->lex_str.str, yylval_tok->lex_str.length);
      str->q_append(SPIDER_SQL_SPACE_STR, SPIDER_SQL_SPACE_LEN);
      break;
    case PARAM_MARKER:
      DBUG_PRINT("info",("spider PARAM_MARKER"));
      if (str->append(STRING_WITH_LEN("?"))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);} break;
    case NOT2_SYM:
      DBUG_PRINT("info",("spider NOT2_SYM"));
      if (str->append(STRING_WITH_LEN("!"))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);} break;
    case SET_VAR:
      DBUG_PRINT("info",("spider SET_VAR"));
      if (str->append(STRING_WITH_LEN(":="))) {DBUG_RETURN(HA_ERR_OUT_OF_MEM);} break;
    case ABORT_SYM:
      /* syntax error */
      DBUG_PRINT("info",("spider ABORT_SYM"));
      /* just skip */
      DBUG_RETURN(ER_SPIDER_COND_SKIP_NUM);
    case END_OF_INPUT:
      DBUG_PRINT("info",("spider END_OF_INPUT"));
      /* nothing to do */
      break;
    default:
      DBUG_RETURN(ER_SPIDER_NOT_SUPPORTED_NUM);
  }
  DBUG_RETURN(0);
}

int spider_db_sql::append_table_option_name(
  int symbol_tok,
  union YYSTYPE *yylval_tok
) {
  DBUG_ENTER("spider_db_sql::append_table_option_name");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(0);
}

int spider_db_sql::append_table_option_value(
  int symbol_tok,
  union YYSTYPE *yylval_tok
) {
  DBUG_ENTER("spider_db_sql::append_table_option_value");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(0);
}

int spider_db_sql::append_table_option_character_set()
{
  DBUG_ENTER("spider_db_sql::append_table_option_character_set");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(0);
}

int spider_db_sql::append_table_option_data_directory()
{
  DBUG_ENTER("spider_db_sql::append_table_option_data_directory");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(0);
}

int spider_db_sql::append_table_option_index_directory()
{
  DBUG_ENTER("spider_db_sql::append_table_option_index_directory");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(0);
}

int spider_db_sql::append_table_option_with_system_versioning()
{
  DBUG_ENTER("spider_db_sql::append_table_option_with_system_versioning");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(0);
}

int spider_db_sql::append_ident(
  uint str_id,
  LEX_CSTRING *name
) {
  spider_string *str = &sql_str[str_id];
  DBUG_ENTER("spider_db_sql::append_ident");
  DBUG_PRINT("info",("spider this=%p", this));
  str->q_append(quote_char_for_ident.str, quote_char_for_ident.length);
  str->q_append(name->str, name->length);
  str->q_append(quote_char_for_ident.str, quote_char_for_ident.length);
  DBUG_RETURN(0);
}

int spider_db_sql::append_table_name(
  uint str_id,
  LEX_CSTRING *schema_name,
  LEX_CSTRING *table_name
) {
  int error_num;
  spider_string *str = &sql_str[str_id];
  DBUG_ENTER("spider_db_sql::append_table_name");
  DBUG_PRINT("info",("spider this=%p", this));
  if ((error_num = append_ident(str_id, schema_name)))
  {
    DBUG_RETURN(error_num);
  }
  str->q_append(SPIDER_SQL_DOT_STR, SPIDER_SQL_DOT_LEN);
  error_num = append_ident(str_id, table_name);
  DBUG_RETURN(error_num);
}

int spider_db_sql::append_table_name(
  LEX_CSTRING *schema_name,
  LEX_CSTRING *table_name
) {
  int error_num;
  spider_string *str = &sql_str[0];
  uint length = str->length();
  DBUG_ENTER("spider_db_sql::append_table_name");
  DBUG_PRINT("info",("spider this=%p", this));
  str->length(table_name_pos[0]);
  error_num = append_table_name(0, schema_name, table_name);
  str->length(length);
  DBUG_RETURN(error_num);
}

int spider_db_sql::append_spider_table(
  LEX_CSTRING *table_name,
  SPIDER_RWTBLTBL *rwtbltbl
) {
  int error_num;
  spider_string *str = &sql_str[0];
  SPIDER_RWTBLPTT *tp;
  DBUG_ENTER("spider_db_sql::append_spider_table");
  DBUG_PRINT("info",("spider this=%p", this));
  str->length(sql_end_pos[0]);
  if (!rwtbltbl->partition_method.length)
  {
    /* no partition definition */
    if (str->reserve(
      SPIDER_SQL_ENGINE_SPIDER_LEN +
      (table_name->length + rwtbltbl->connection_str.length +
        rwtbltbl->comment_str.length) * 2 + SPIDER_SQL_TABLE_LEN +
      SPIDER_SQL_COMMENT_LEN + SPIDER_SQL_CONNECTION_LEN + 7
    )) {
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    }
    str->q_append(SPIDER_SQL_ENGINE_SPIDER_STR, SPIDER_SQL_ENGINE_SPIDER_LEN);
    str->q_append(SPIDER_SQL_CONNECTION_STR, SPIDER_SQL_CONNECTION_LEN);
    str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
    str->q_append(SPIDER_SQL_TABLE_STR, SPIDER_SQL_TABLE_LEN);
    str->q_append(SPIDER_SQL_DOUBLE_QUOTE_STR, SPIDER_SQL_DOUBLE_QUOTE_LEN);
    str->append_for_single_quote(table_name->str, table_name->length);
    str->q_append(SPIDER_SQL_DOUBLE_QUOTE_STR, SPIDER_SQL_DOUBLE_QUOTE_LEN);
    if (rwtbltbl->connection_str.length)
    {
      str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
      str->append_for_single_quote(rwtbltbl->connection_str.str,
        rwtbltbl->connection_str.length);
    }
    str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
    if (rwtbltbl->comment_str.length)
    {
      str->q_append(SPIDER_SQL_COMMENT_STR, SPIDER_SQL_COMMENT_LEN);
      str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
      str->append_for_single_quote(rwtbltbl->comment_str.str,
        rwtbltbl->comment_str.length);
      str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
    }
    DBUG_RETURN(0);
  }
  if (str->reserve(
    SPIDER_SQL_ENGINE_SPIDER_LEN +
    (rwtbltbl->connection_str.length + rwtbltbl->comment_str.length) * 2 +
    SPIDER_SQL_COMMENT_LEN + SPIDER_SQL_CONNECTION_LEN + 4 +
    SPIDER_SQL_PARTITION_BY_LEN +
    (rwtbltbl->subpartition_method.length ? SPIDER_SQL_SUBPARTITION_BY_LEN : 0) +
    (
      rwtbltbl->partition_method.length + rwtbltbl->partition_expression.length +
      rwtbltbl->subpartition_method.length + rwtbltbl->subpartition_expression.length
    ) * 2 + 5
  )) {
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  str->q_append(SPIDER_SQL_ENGINE_SPIDER_STR, SPIDER_SQL_ENGINE_SPIDER_LEN);
  if (rwtbltbl->connection_str.length)
  {
    str->q_append(SPIDER_SQL_CONNECTION_STR, SPIDER_SQL_CONNECTION_LEN);
    str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
    str->append_for_single_quote(rwtbltbl->connection_str.str,
      rwtbltbl->connection_str.length);
    str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
  }
  if (rwtbltbl->comment_str.length)
  {
    str->q_append(SPIDER_SQL_COMMENT_STR, SPIDER_SQL_COMMENT_LEN);
    str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
    str->append_for_single_quote(rwtbltbl->comment_str.str,
      rwtbltbl->comment_str.length);
    str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
  }
  str->q_append(SPIDER_SQL_PARTITION_BY_STR, SPIDER_SQL_PARTITION_BY_LEN);
  str->append_escape_string(rwtbltbl->partition_method.str,
    rwtbltbl->partition_method.length);
  str->q_append(SPIDER_SQL_OPEN_PAREN_STR, SPIDER_SQL_OPEN_PAREN_LEN);
  str->append_escape_string(rwtbltbl->partition_expression.str,
    rwtbltbl->partition_expression.length);
  str->q_append(SPIDER_SQL_CLOSE_PAREN_STR, SPIDER_SQL_CLOSE_PAREN_LEN);

  if (rwtbltbl->subpartition_method.length)
  {
    str->q_append(SPIDER_SQL_SUBPARTITION_BY_STR, SPIDER_SQL_SUBPARTITION_BY_LEN);
    str->append_escape_string(rwtbltbl->subpartition_method.str,
      rwtbltbl->subpartition_method.length);
    str->q_append(SPIDER_SQL_OPEN_PAREN_STR, SPIDER_SQL_OPEN_PAREN_LEN);
    str->append_escape_string(rwtbltbl->subpartition_expression.str,
      rwtbltbl->subpartition_expression.length);
    str->q_append(SPIDER_SQL_CLOSE_PAREN_STR, SPIDER_SQL_CLOSE_PAREN_LEN);
  }

  str->q_append(SPIDER_SQL_OPEN_PAREN_STR, SPIDER_SQL_OPEN_PAREN_LEN);
  tp = rwtbltbl->tp;
  while (tp)
  {
    if ((error_num = append_spider_partition(table_name, rwtbltbl, tp)))
    {
      DBUG_RETURN(error_num);
    }
    tp = tp->next;
    if (tp)
    {
      str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
    }
  }
  if (str->reserve(SPIDER_SQL_CLOSE_PAREN_LEN))
  {
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  str->q_append(SPIDER_SQL_CLOSE_PAREN_STR, SPIDER_SQL_CLOSE_PAREN_LEN);
  DBUG_RETURN(0);
}

int spider_db_sql::append_spider_partition(
  LEX_CSTRING *table_name,
  SPIDER_RWTBLTBL *rwtbltbl,
  SPIDER_RWTBLPTT *rwtblptt
) {
  int error_num;
  spider_string *str = &sql_str[0];
  SPIDER_RWTBLSPTT *ts;
  DBUG_ENTER("spider_db_sql::append_spider_partition");
  DBUG_PRINT("info",("spider this=%p", this));
  if (!rwtblptt->ts)
  {
    /* no subpartition definition */
    if (str->reserve(
      SPIDER_SQL_PARTITION_LEN + SPIDER_SQL_VALUES_LESS_THAN_LEN + 2 +
      (
        table_name->length +
        rwtblptt->partition_name.length +
        rwtblptt->partition_description.length +
        rwtblptt->connection_str.length + rwtblptt->comment_str.length
      ) * 2 + SPIDER_SQL_TABLE_LEN + 3 +
      SPIDER_SQL_COMMENT_LEN + SPIDER_SQL_CONNECTION_LEN + 5 +
      SPIDER_SQL_COMMA_LEN
    )) {
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    }
    str->q_append(SPIDER_SQL_PARTITION_STR, SPIDER_SQL_PARTITION_LEN);
    str->append_escape_string(rwtblptt->partition_name.str,
      rwtblptt->partition_name.length);

    if (
     rwtbltbl->partition_method.length >= 5 &&
     !strncasecmp("range", rwtbltbl->partition_method.str, 5)
    ) {
      str->q_append(SPIDER_SQL_VALUES_LESS_THAN_STR,
        SPIDER_SQL_VALUES_LESS_THAN_LEN);
      if (
       rwtblptt->partition_description.length == 8 &&
       !strncasecmp("maxvalue", rwtblptt->partition_description.str, 8)
      ) {
        str->q_append(rwtblptt->partition_description.str,
          rwtblptt->partition_description.length);
      } else {
        str->q_append(SPIDER_SQL_OPEN_PAREN_STR, SPIDER_SQL_OPEN_PAREN_LEN);
        str->append_escape_string(rwtblptt->partition_description.str,
          rwtblptt->partition_description.length);
        str->q_append(SPIDER_SQL_CLOSE_PAREN_STR, SPIDER_SQL_CLOSE_PAREN_LEN);
      }
    } else if (
     rwtbltbl->partition_method.length >= 4 &&
     !strncasecmp("list", rwtbltbl->partition_method.str, 5)
    ) {
      str->q_append(SPIDER_SQL_VALUES_IN_STR, SPIDER_SQL_VALUES_IN_LEN);
      str->q_append(SPIDER_SQL_OPEN_PAREN_STR, SPIDER_SQL_OPEN_PAREN_LEN);
      str->append_escape_string(rwtblptt->partition_description.str,
        rwtblptt->partition_description.length);
      str->q_append(SPIDER_SQL_CLOSE_PAREN_STR, SPIDER_SQL_CLOSE_PAREN_LEN);
    }

    str->q_append(SPIDER_SQL_CONNECTION_STR, SPIDER_SQL_CONNECTION_LEN);
    str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
    str->q_append(SPIDER_SQL_TABLE_STR, SPIDER_SQL_TABLE_LEN);
    str->q_append(SPIDER_SQL_DOUBLE_QUOTE_STR, SPIDER_SQL_DOUBLE_QUOTE_LEN);
    str->append_for_single_quote(table_name->str, table_name->length);
    str->q_append(SPIDER_SQL_DOUBLE_QUOTE_STR, SPIDER_SQL_DOUBLE_QUOTE_LEN);
    if (rwtblptt->connection_str.length)
    {
      str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
      str->append_for_single_quote(rwtblptt->connection_str.str,
        rwtblptt->connection_str.length);
    }
    str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
    if (rwtblptt->comment_str.length)
    {
      str->q_append(SPIDER_SQL_COMMENT_STR, SPIDER_SQL_COMMENT_LEN);
      str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
      str->append_for_single_quote(rwtblptt->comment_str.str,
        rwtblptt->comment_str.length);
      str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
    }
    DBUG_RETURN(0);
  }
  if (str->reserve(
    SPIDER_SQL_PARTITION_LEN + SPIDER_SQL_VALUES_LESS_THAN_LEN + 2 +
    (
      rwtblptt->partition_name.length +
      rwtblptt->partition_description.length +
      rwtblptt->connection_str.length + rwtblptt->comment_str.length
    ) * 2 +
    SPIDER_SQL_COMMENT_LEN + SPIDER_SQL_CONNECTION_LEN + 5
  )) {
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  str->q_append(SPIDER_SQL_PARTITION_STR, SPIDER_SQL_PARTITION_LEN);
  str->append_escape_string(rwtblptt->partition_name.str,
    rwtblptt->partition_name.length);

  if (
   rwtbltbl->partition_method.length >= 5 &&
   !strncasecmp("range", rwtbltbl->partition_method.str, 5)
  ) {
    str->q_append(SPIDER_SQL_VALUES_LESS_THAN_STR,
      SPIDER_SQL_VALUES_LESS_THAN_LEN);
    if (
     rwtblptt->partition_description.length == 8 &&
     !strncasecmp("maxvalue", rwtblptt->partition_description.str, 8)
    ) {
      str->q_append(rwtblptt->partition_description.str,
        rwtblptt->partition_description.length);
    } else {
      str->q_append(SPIDER_SQL_OPEN_PAREN_STR, SPIDER_SQL_OPEN_PAREN_LEN);
      str->append_escape_string(rwtblptt->partition_description.str,
        rwtblptt->partition_description.length);
      str->q_append(SPIDER_SQL_CLOSE_PAREN_STR, SPIDER_SQL_CLOSE_PAREN_LEN);
    }
  } else if (
   rwtbltbl->partition_method.length >= 4 &&
   !strncasecmp("list", rwtbltbl->partition_method.str, 5)
  ) {
    str->q_append(SPIDER_SQL_VALUES_IN_STR, SPIDER_SQL_VALUES_IN_LEN);
    str->q_append(SPIDER_SQL_OPEN_PAREN_STR, SPIDER_SQL_OPEN_PAREN_LEN);
    str->append_escape_string(rwtblptt->partition_description.str,
      rwtblptt->partition_description.length);
    str->q_append(SPIDER_SQL_CLOSE_PAREN_STR, SPIDER_SQL_CLOSE_PAREN_LEN);
  }
  if (rwtblptt->connection_str.length)
  {
    str->q_append(SPIDER_SQL_CONNECTION_STR, SPIDER_SQL_CONNECTION_LEN);
    str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
    str->append_for_single_quote(rwtblptt->connection_str.str,
      rwtblptt->connection_str.length);
    str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
  }
  if (rwtblptt->comment_str.length)
  {
    str->q_append(SPIDER_SQL_COMMENT_STR, SPIDER_SQL_COMMENT_LEN);
    str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
    str->append_for_single_quote(rwtblptt->comment_str.str,
      rwtblptt->comment_str.length);
    str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
  }
  str->q_append(SPIDER_SQL_OPEN_PAREN_STR, SPIDER_SQL_OPEN_PAREN_LEN);
  ts = rwtblptt->ts;
  while (ts)
  {
    if ((error_num = append_spider_subpartition(table_name, rwtbltbl, ts)))
    {
      DBUG_RETURN(error_num);
    }
    ts = ts->next;
    if (ts)
    {
      str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
    }
  }
  if (str->reserve(SPIDER_SQL_CLOSE_PAREN_LEN + SPIDER_SQL_COMMA_LEN))
  {
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  str->q_append(SPIDER_SQL_CLOSE_PAREN_STR, SPIDER_SQL_CLOSE_PAREN_LEN);
  DBUG_RETURN(0);
}

int spider_db_sql::append_spider_subpartition(
  LEX_CSTRING *table_name,
  SPIDER_RWTBLTBL *rwtbltbl,
  SPIDER_RWTBLSPTT *rwtblsptt
) {
  spider_string *str = &sql_str[0];
  DBUG_ENTER("spider_db_sql::append_spider_subpartition");
  DBUG_PRINT("info",("spider this=%p", this));
  if (str->reserve(
    SPIDER_SQL_SUBPARTITION_LEN +
    (
      table_name->length +
      rwtblsptt->subpartition_name.length +
      rwtblsptt->subpartition_description.length +
      rwtblsptt->connection_str.length + rwtblsptt->comment_str.length
    ) * 2 + SPIDER_SQL_TABLE_LEN + 3 +
    SPIDER_SQL_COMMENT_LEN + SPIDER_SQL_CONNECTION_LEN + 5 +
    SPIDER_SQL_COMMA_LEN
  )) {
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  }
  str->q_append(SPIDER_SQL_SUBPARTITION_STR, SPIDER_SQL_SUBPARTITION_LEN);
  str->append_escape_string(rwtblsptt->subpartition_name.str,
    rwtblsptt->subpartition_name.length);

  str->q_append(SPIDER_SQL_CONNECTION_STR, SPIDER_SQL_CONNECTION_LEN);
  str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
  str->q_append(SPIDER_SQL_TABLE_STR, SPIDER_SQL_TABLE_LEN);
  str->q_append(SPIDER_SQL_DOUBLE_QUOTE_STR, SPIDER_SQL_DOUBLE_QUOTE_LEN);
  str->append_for_single_quote(table_name->str, table_name->length);
  str->q_append(SPIDER_SQL_DOUBLE_QUOTE_STR, SPIDER_SQL_DOUBLE_QUOTE_LEN);
  if (rwtblsptt->connection_str.length)
  {
    str->q_append(SPIDER_SQL_COMMA_STR, SPIDER_SQL_COMMA_LEN);
    str->append_for_single_quote(rwtblsptt->connection_str.str,
      rwtblsptt->connection_str.length);
  }
  str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
  if (rwtblsptt->comment_str.length)
  {
    str->q_append(SPIDER_SQL_COMMENT_STR, SPIDER_SQL_COMMENT_LEN);
    str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
    str->append_for_single_quote(rwtblsptt->comment_str.str,
      rwtblsptt->comment_str.length);
    str->q_append(SPIDER_SQL_VALUE_QUOTE_STR, SPIDER_SQL_VALUE_QUOTE_LEN);
  }
  DBUG_RETURN(0);
}

spider_db_result::spider_db_result(
  SPIDER_DB_CONN *in_db_conn
) : db_conn(in_db_conn), dbton_id(in_db_conn->dbton_id)
{
  DBUG_ENTER("spider_db_result::spider_db_result");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_VOID_RETURN;
}

#ifdef HA_HAS_CHECKSUM_EXTENDED
int spider_db_result::fetch_table_checksum(
  ha_spider *spider
) {
  DBUG_ENTER("spider_db_result::fetch_table_checksum");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(0);
}
#endif

spider_db_conn::spider_db_conn(
  SPIDER_CONN *in_conn
) : conn(in_conn), dbton_id(in_conn->dbton_id)
{
  DBUG_ENTER("spider_db_conn::spider_db_conn");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_VOID_RETURN;
}

#ifdef HA_HAS_CHECKSUM_EXTENDED
bool spider_db_share::checksum_support()
{
  DBUG_ENTER("spider_db_share::checksum_support");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(FALSE);
}

int spider_db_handler::checksum_table(
  int link_idx
) {
  DBUG_ENTER("spider_db_handler::checksum_table");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(0);
}
#endif
