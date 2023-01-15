/* Copyright (C) 2018-2019 Kentoku Shiba

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
#if MYSQL_VERSION_ID < 50500
#include "mysql_priv.h"
#include <mysql/plugin.h>
#else
#include "sql_priv.h"
#include "probes_mysql.h"
#include "sql_class.h"
#endif
#include "sql_common.h"
#include <mysql.h>
#include <errmsg.h>
#include "spd_err.h"
#include "spd_db_include.h"
#include "spd_include.h"

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
