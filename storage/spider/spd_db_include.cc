/* Copyright (C) 2018-2020 Kentoku Shiba
   Copyright (C) 2018-2020 MariaDB corp

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
#include <my_global.h>
#include "mysql_version.h"
#include "spd_environ.h"
#include "sql_priv.h"
#include "probes_mysql.h"
#include "sql_class.h"
#include "sql_common.h"
#include <mysql.h>
#include <errmsg.h>
#include "spd_err.h"
#include "spd_db_include.h"
#include "spd_include.h"
#include "spd_conn.h"

extern SPIDER_DBTON spider_dbton[SPIDER_DBTON_SIZE];

spider_db_result::spider_db_result(
  SPIDER_DB_CONN *in_db_conn
) : db_conn(in_db_conn), dbton_id(in_db_conn->dbton_id)
{
  DBUG_ENTER("spider_db_result::spider_db_result");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_VOID_RETURN;
}

int spider_db_result::fetch_table_checksum(
  ha_spider *spider
) {
  DBUG_ENTER("spider_db_result::fetch_table_checksum");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(0);
}

uint spider_db_result::limit_mode()
{
  DBUG_ENTER("spider_db_result::limit_mode");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(spider_dbton[dbton_id].db_util->limit_mode());
}

spider_db_conn::spider_db_conn(
  SPIDER_CONN *in_conn
) : conn(in_conn), dbton_id(in_conn->dbton_id)
{
  DBUG_ENTER("spider_db_conn::spider_db_conn");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_VOID_RETURN;
}

bool spider_db_conn::set_loop_check_in_bulk_sql()
{
  DBUG_ENTER("spider_db_conn::set_loop_check_in_bulk_sql");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(FALSE);
}

int spider_db_conn::set_loop_check(
  int *need_mon
) {
  DBUG_ENTER("spider_db_conn::set_loop_check");
  DBUG_PRINT("info",("spider this=%p", this));
  /* nothing to do */
  DBUG_RETURN(0);
}

int spider_db_conn::fin_loop_check()
{
  st_spider_conn_loop_check *lcptr;
  DBUG_ENTER("spider_db_conn::fin_loop_check");
  DBUG_PRINT("info",("spider this=%p", this));
  if (conn->loop_check_queue.records)
  {
    uint l = 0;
    while ((lcptr = (SPIDER_CONN_LOOP_CHECK *) my_hash_element(
      &conn->loop_check_queue, l)))
    {
      lcptr->flag = 0;
      ++l;
    }
    my_hash_reset(&conn->loop_check_queue);
  }
  lcptr = conn->loop_check_ignored_first;
  while (lcptr)
  {
    lcptr->flag = 0;
    lcptr = lcptr->next;
  }
  conn->loop_check_ignored_first = NULL;
  lcptr = conn->loop_check_meraged_first;
  while (lcptr)
  {
    lcptr->flag = 0;
    lcptr = lcptr->next;
  }
  conn->loop_check_meraged_first = NULL;
  DBUG_RETURN(0);
}

uint spider_db_conn::limit_mode()
{
  DBUG_ENTER("spider_db_conn::limit_mode");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(spider_dbton[dbton_id].db_util->limit_mode());
}

int spider_db_util::append_loop_check(
  spider_string *str,
  SPIDER_CONN *conn
) {
  DBUG_ENTER("spider_db_util::append_loop_check");
  DBUG_PRINT("info",("spider this=%p", this));
  /* nothing to do */
  DBUG_RETURN(0);
}

bool spider_db_util::tables_on_different_db_are_joinable()
{
  DBUG_ENTER("spider_db_util::tables_on_different_db_are_joinable");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(FALSE);
}

bool spider_db_util::socket_has_default_value()
{
  DBUG_ENTER("spider_db_util::socket_has_default_value");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(FALSE);
}

bool spider_db_util::database_has_default_value()
{
  DBUG_ENTER("spider_db_util::database_has_default_value");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(FALSE);
}

bool spider_db_util::default_file_has_default_value()
{
  DBUG_ENTER("spider_db_util::default_file_has_default_value");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(FALSE);
}

bool spider_db_util::host_has_default_value()
{
  DBUG_ENTER("spider_db_util::host_has_default_value");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(FALSE);
}

bool spider_db_util::port_has_default_value()
{
  DBUG_ENTER("spider_db_util::port_has_default_value");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(FALSE);
}

bool spider_db_util::append_charset_name_before_string()
{
  DBUG_ENTER("spider_db_util::append_charset_name_before_string");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(FALSE);
}

uint spider_db_util::limit_mode()
{
  DBUG_ENTER("spider_db_util::limit_mode");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(0);
}

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

bool spider_db_handler::check_direct_update(
  st_select_lex *select_lex,
  longlong select_limit,
  longlong offset_limit
) {
  DBUG_ENTER("spider_db_handler::check_direct_update");
  DBUG_PRINT("info",("spider this=%p", this));
  if (
    select_limit != 9223372036854775807LL ||
    offset_limit != 0 ||
    select_lex->order_list.elements
  ) {
    DBUG_RETURN(TRUE);
  }
  DBUG_RETURN(FALSE);
}

bool spider_db_handler::check_direct_delete(
  st_select_lex *select_lex,
  longlong select_limit,
  longlong offset_limit
) {
  DBUG_ENTER("spider_db_handler::check_direct_delete");
  DBUG_PRINT("info",("spider this=%p", this));
  if (
    select_limit != 9223372036854775807LL ||
    offset_limit != 0 ||
    select_lex->order_list.elements
  ) {
    DBUG_RETURN(TRUE);
  }
  DBUG_RETURN(FALSE);
}
