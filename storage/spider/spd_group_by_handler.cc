/* Copyright (C) 2008-2019 Kentoku Shiba
   Copyright (C) 2019 MariaDB corp

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
#include "sql_partition.h"
#include "ha_partition.h"
#include "sql_common.h"
#include <errmsg.h>
#include "spd_err.h"
#include "spd_param.h"
#include "spd_db_include.h"
#include "spd_include.h"
#include "ha_spider.h"
#include "spd_conn.h"
#include "spd_db_conn.h"
#include "spd_malloc.h"
#include "spd_table.h"
#include "spd_ping_table.h"
#include "spd_group_by_handler.h"

extern handlerton *spider_hton_ptr;
extern SPIDER_DBTON spider_dbton[SPIDER_DBTON_SIZE];

spider_fields::spider_fields() :
  dbton_count(0), current_dbton_num(0),
  table_count(0), current_table_num(0), table_holder(NULL),
  first_link_idx_chain(NULL), last_link_idx_chain(NULL), current_link_idx_chain(NULL),
  first_conn_holder(NULL), last_conn_holder(NULL), current_conn_holder(NULL),
  first_field_holder(NULL), last_field_holder(NULL), current_field_holder(NULL),
  first_field_chain(NULL), last_field_chain(NULL), current_field_chain(NULL)
{
  DBUG_ENTER("spider_fields::spider_fields");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_VOID_RETURN;
}

spider_fields::~spider_fields()
{
  DBUG_ENTER("spider_fields::~spider_fields");
  DBUG_PRINT("info",("spider this=%p", this));
  if (first_link_idx_chain)
  {
    while ((current_link_idx_chain = first_link_idx_chain))
    {
      first_link_idx_chain = current_link_idx_chain->next;
      spider_free(spider_current_trx, current_link_idx_chain, MYF(0));
    }
  }
  if (first_field_chain)
  {
    while ((current_field_chain = first_field_chain))
    {
      first_field_chain = current_field_chain->next;
      spider_free(spider_current_trx, current_field_chain, MYF(0));
    }
  }
  if (first_field_holder)
  {
    while ((current_field_holder = first_field_holder))
    {
      first_field_holder = current_field_holder->next;
      spider_free(spider_current_trx, current_field_holder, MYF(0));
    }
  }
  if (table_holder)
    spider_free(spider_current_trx, table_holder, MYF(0));
  if (first_conn_holder)
  {
    while ((current_conn_holder = first_conn_holder))
    {
      first_conn_holder = current_conn_holder->next;
      free_conn_holder(current_conn_holder);
    }
  }
  DBUG_VOID_RETURN;
}

void spider_fields::add_dbton_id(
  uint dbton_id_arg
) {
  uint roop_count;
  DBUG_ENTER("spider_fields::add_dbton_id");
  DBUG_PRINT("info",("spider this=%p", this));
  for (roop_count = 0; roop_count < dbton_count; ++roop_count)
  {
    if (dbton_ids[roop_count] == dbton_id_arg)
    {
      DBUG_VOID_RETURN;
    }
  }
  dbton_ids[roop_count] = dbton_id_arg;
  ++dbton_count;
  DBUG_VOID_RETURN;
}

void spider_fields::set_pos_to_first_dbton_id(
) {
  DBUG_ENTER("spider_fields::set_pos_to_first_dbton_id");
  DBUG_PRINT("info",("spider this=%p", this));
  current_dbton_num = 0;
  DBUG_VOID_RETURN;
}

uint spider_fields::get_next_dbton_id(
) {
  uint return_dbton_id;
  DBUG_ENTER("spider_fields::get_next_dbton_id");
  DBUG_PRINT("info",("spider this=%p", this));
  if (current_dbton_num >= dbton_count)
    DBUG_RETURN(SPIDER_DBTON_SIZE);
  return_dbton_id = dbton_ids[current_dbton_num];
  ++current_dbton_num;
  DBUG_RETURN(return_dbton_id);
}

int spider_fields::make_link_idx_chain(
  int link_status
) {
  uint roop_count, roop_count2;
  SPIDER_CONN *conn;
  SPIDER_CONN_HOLDER *conn_holder;
  SPIDER_TABLE_LINK_IDX_HOLDER *table_link_idx_holder;
  SPIDER_LINK_IDX_HOLDER *link_idx_holder, *add_link_idx_holder,
    *dup_link_idx_holder, *current_link_idx_holder;
  ha_spider *spider;
  SPIDER_LINK_IDX_CHAIN *link_idx_chain;
  SPIDER_SHARE *share;
  DBUG_ENTER("spider_fields::make_link_idx_chain");
  DBUG_PRINT("info",("spider this=%p", this));
  conn_holder = first_conn_holder;
  bool has_remain, skip;
  do {
    for (roop_count2 = 0; roop_count2 < table_count; ++roop_count2)
    {
      table_link_idx_holder = &conn_holder->table_link_idx_holder[roop_count2];
      link_idx_holder = table_link_idx_holder->first_link_idx_holder;
      dup_link_idx_holder = NULL;
      for (roop_count = 0;
        roop_count < conn_holder->link_idx_holder_count_max - 1; ++roop_count)
      {
        if (!link_idx_holder->next)
        {
          DBUG_PRINT("info",("spider fill link_idx_holder for %u",
            roop_count2));
          if (!(add_link_idx_holder = create_link_idx_holder()))
            DBUG_RETURN(HA_ERR_OUT_OF_MEM);
          dup_link_idx_holder = get_dup_link_idx_holder(
            table_link_idx_holder, dup_link_idx_holder);
          add_link_idx_holder->table_link_idx_holder =
            dup_link_idx_holder->table_link_idx_holder;
          add_link_idx_holder->link_idx = dup_link_idx_holder->link_idx;
          add_link_idx_holder->link_status = dup_link_idx_holder->link_status;
          link_idx_holder->next = add_link_idx_holder;
        }
        link_idx_holder = link_idx_holder->next;
      }
    }

    for (roop_count2 = 0; roop_count2 < table_count; ++roop_count2)
    {
      table_link_idx_holder = &conn_holder->table_link_idx_holder[roop_count2];
      table_link_idx_holder->current_link_idx_holder =
        table_link_idx_holder->first_link_idx_holder;
    }
    for (roop_count = 0;
      roop_count < conn_holder->link_idx_holder_count_max; ++roop_count)
    {
      link_idx_holder = NULL;
      for (roop_count2 = 0; roop_count2 < table_count; ++roop_count2)
      {
        table_link_idx_holder =
          &conn_holder->table_link_idx_holder[roop_count2];
        if (link_idx_holder)
        {
          link_idx_holder->next_table =
            table_link_idx_holder->current_link_idx_holder;
        }
        link_idx_holder = table_link_idx_holder->current_link_idx_holder;
        table_link_idx_holder->current_link_idx_holder = link_idx_holder->next;
      }
    }
  } while ((conn_holder = conn_holder->next));

  current_conn_holder = first_conn_holder;
  do {
    table_link_idx_holder =
      &current_conn_holder->table_link_idx_holder[0];
    table_link_idx_holder->current_link_idx_holder =
      table_link_idx_holder->first_link_idx_holder;
  } while ((current_conn_holder = current_conn_holder->next));

  spider = table_holder[0].spider;
  share = spider->share;
  DBUG_PRINT("info",("spider create link_idx_chain sorted by 0"));
  for (
    roop_count = spider_conn_link_idx_next(share->link_statuses,
      spider->conn_link_idx, -1, share->link_count,
      link_status);
    roop_count < share->link_count;
    roop_count = spider_conn_link_idx_next(share->link_statuses,
      spider->conn_link_idx, roop_count, share->link_count,
      link_status)
  ) {
    conn = spider->conns[roop_count];
    if (!conn->conn_holder_for_direct_join)
    {
      continue;
    }
    table_link_idx_holder =
      &conn->conn_holder_for_direct_join->table_link_idx_holder[0];
    link_idx_holder = table_link_idx_holder->current_link_idx_holder;
    table_link_idx_holder->current_link_idx_holder = link_idx_holder->next;
    DBUG_ASSERT(link_idx_holder->link_idx == (int) roop_count);
    if (!(link_idx_chain = create_link_idx_chain()))
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    if (!first_link_idx_chain)
    {
      first_link_idx_chain = link_idx_chain;
    } else {
      last_link_idx_chain->next = link_idx_chain;
    }
    last_link_idx_chain = link_idx_chain;
    link_idx_chain->conn = conn;
    link_idx_chain->link_idx_holder = link_idx_holder;
    do {
      if (link_idx_chain->link_status < link_idx_holder->link_status)
      {
        link_idx_chain->link_status = link_idx_holder->link_status;
      }
    } while ((link_idx_holder = link_idx_holder->next_table));
  }

  do {
    has_remain = FALSE;
    current_conn_holder = first_conn_holder;
    do {
      table_link_idx_holder =
        &current_conn_holder->table_link_idx_holder[0];
      link_idx_holder = table_link_idx_holder->current_link_idx_holder;
      if (link_idx_holder)
      {
        has_remain = TRUE;
        for (roop_count2 = 1; roop_count2 < table_count; ++roop_count2)
        {
          if (table_link_idx_holder[roop_count2].link_idx_holder_count ==
            current_conn_holder->link_idx_holder_count_max)
          {
            break;
          }
        }
        break;
      }
    } while ((current_conn_holder = current_conn_holder->next));

    if (has_remain)
    {
      current_conn_holder = first_conn_holder;
      do {
        table_link_idx_holder =
          &current_conn_holder->table_link_idx_holder[0];
        link_idx_holder = table_link_idx_holder->current_link_idx_holder;
        if (link_idx_holder)
        {
          for (roop_count = 1; roop_count <= roop_count2; ++roop_count)
          {
            link_idx_holder = link_idx_holder->next_table;
          }
          table_link_idx_holder[roop_count2].current_link_idx_holder =
            link_idx_holder;
        } else {
          table_link_idx_holder[roop_count2].current_link_idx_holder = NULL;
        }
      } while ((current_conn_holder = current_conn_holder->next));

      spider = table_holder[roop_count2].spider;
      share = spider->share;
      DBUG_PRINT("info",("spider create link_idx_chain sorted by %d",
        roop_count2));
      for (
        roop_count = spider_conn_link_idx_next(share->link_statuses,
          spider->conn_link_idx, -1, share->link_count,
          link_status);
        roop_count < share->link_count;
        roop_count = spider_conn_link_idx_next(share->link_statuses,
          spider->conn_link_idx, roop_count, share->link_count,
          link_status)
      ) {
        conn = spider->conns[roop_count];
        if (!conn->conn_holder_for_direct_join)
        {
          continue;
        }
        table_link_idx_holder =
          &conn->conn_holder_for_direct_join->table_link_idx_holder[0];
        link_idx_holder =
          table_link_idx_holder[roop_count2].current_link_idx_holder;
        skip = FALSE;
        if (link_idx_holder)
        {
          current_link_idx_holder = table_link_idx_holder->first_link_idx_holder;
          while (current_link_idx_holder != link_idx_holder)
          {
            if (current_link_idx_holder->link_idx ==
              link_idx_holder->link_idx)
            {
              skip = TRUE;
              break;
            }
            current_link_idx_holder = current_link_idx_holder->next;
          }
        }
        if (skip)
        {
          continue;
        }
        DBUG_PRINT("info",("spider create link_idx_chain for %d",
          roop_count2));
        table_link_idx_holder[roop_count2].current_link_idx_holder =
          link_idx_holder->next;
        link_idx_holder =
          table_link_idx_holder->current_link_idx_holder;
        table_link_idx_holder->current_link_idx_holder =
          link_idx_holder->next;
        if (!(link_idx_chain = create_link_idx_chain()))
          DBUG_RETURN(HA_ERR_OUT_OF_MEM);
        DBUG_ASSERT(first_link_idx_chain);
        last_link_idx_chain->next = link_idx_chain;
        last_link_idx_chain = link_idx_chain;
        link_idx_chain->conn = conn;
        link_idx_chain->link_idx_holder = link_idx_holder;
        do {
          if (link_idx_chain->link_status < link_idx_holder->link_status)
          {
            link_idx_chain->link_status = link_idx_holder->link_status;
          }
        } while ((link_idx_holder = link_idx_holder->next_table));
      }
    }
  } while (has_remain);
  DBUG_RETURN(0);
}

SPIDER_LINK_IDX_CHAIN *spider_fields::create_link_idx_chain(
) {
  DBUG_ENTER("spider_fields::create_link_idx_chain");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN((SPIDER_LINK_IDX_CHAIN *)
    spider_malloc(spider_current_trx, 254, sizeof(SPIDER_LINK_IDX_CHAIN),
    MYF(MY_WME | MY_ZEROFILL)));
}

void spider_fields::set_pos_to_first_link_idx_chain(
) {
  DBUG_ENTER("spider_fields::set_pos_to_first_link_idx_chain");
  DBUG_PRINT("info",("spider this=%p", this));
  current_link_idx_chain = first_link_idx_chain;
  DBUG_VOID_RETURN;
}

SPIDER_LINK_IDX_CHAIN *spider_fields::get_next_link_idx_chain(
) {
  SPIDER_LINK_IDX_CHAIN *return_link_idx_chain = current_link_idx_chain;
  DBUG_ENTER("spider_fields::get_next_link_idx_chain");
  DBUG_PRINT("info",("spider this=%p", this));
  if (current_link_idx_chain)
    current_link_idx_chain = current_link_idx_chain->next;
  DBUG_RETURN(return_link_idx_chain);
}

SPIDER_LINK_IDX_HOLDER *spider_fields::get_dup_link_idx_holder(
  SPIDER_TABLE_LINK_IDX_HOLDER *table_link_idx_holder,
  SPIDER_LINK_IDX_HOLDER *current
) {
  SPIDER_LINK_IDX_HOLDER *return_link_idx_holder;
  DBUG_ENTER("spider_fields::get_dup_link_idx_holder");
  DBUG_PRINT("info",("spider this=%p", this));
  if (!current)
  {
    return_link_idx_holder = table_link_idx_holder->first_link_idx_holder;
    do {
      if (return_link_idx_holder->link_status == SPIDER_LINK_STATUS_OK)
        break;
    } while ((return_link_idx_holder = return_link_idx_holder->next));
    if (!return_link_idx_holder)
    {
      return_link_idx_holder = table_link_idx_holder->first_link_idx_holder;
    }
  } else {
    if (current->link_status == SPIDER_LINK_STATUS_OK)
    {
      return_link_idx_holder = current;
      while ((return_link_idx_holder = return_link_idx_holder->next))
      {
        if (return_link_idx_holder->link_status == SPIDER_LINK_STATUS_OK)
          break;
      }
      if (!return_link_idx_holder)
      {
        return_link_idx_holder = table_link_idx_holder->first_link_idx_holder;
        do {
          if (
            return_link_idx_holder->link_status == SPIDER_LINK_STATUS_OK
          )
            break;
          DBUG_ASSERT(return_link_idx_holder != current);
        } while ((return_link_idx_holder = return_link_idx_holder->next));
      }
    } else {
      if (!current->next)
      {
        return_link_idx_holder = table_link_idx_holder->first_link_idx_holder;
      } else {
        return_link_idx_holder = current->next;
      }
    }
  }
  DBUG_RETURN(return_link_idx_holder);
}

bool spider_fields::check_link_ok_chain(
) {
  DBUG_ENTER("spider_fields::check_link_ok_chain");
  DBUG_PRINT("info",("spider this=%p", this));
  for (current_link_idx_chain = first_link_idx_chain; current_link_idx_chain;
    current_link_idx_chain = current_link_idx_chain->next)
  {
    DBUG_PRINT("info",("spider current_link_idx_chain=%p", current_link_idx_chain));
    DBUG_PRINT("info",("spider current_link_idx_chain->link_status=%d", current_link_idx_chain->link_status));
    if (current_link_idx_chain->link_status == SPIDER_LINK_STATUS_OK)
    {
      first_ok_link_idx_chain = current_link_idx_chain;
      DBUG_RETURN(FALSE);
    }
  }
  DBUG_RETURN(TRUE);
}

bool spider_fields::is_first_link_ok_chain(
  SPIDER_LINK_IDX_CHAIN *link_idx_chain_arg
) {
  DBUG_ENTER("spider_fields::is_first_link_ok_chain");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(first_ok_link_idx_chain == link_idx_chain_arg);
}

int spider_fields::get_ok_link_idx(
) {
  DBUG_ENTER("spider_fields::get_ok_link_idx");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(first_ok_link_idx_chain->link_idx_holder->link_idx);
}

void spider_fields::set_first_link_idx(
) {
  SPIDER_TABLE_HOLDER *table_holder;
  SPIDER_LINK_IDX_HOLDER *link_idx_holder;
  SPIDER_LINK_IDX_CHAIN *link_idx_chain;
  uint dbton_id;
  ha_spider *spider;
  spider_db_handler *dbton_hdl;
  DBUG_ENTER("spider_fields::set_first_link_idx");
  DBUG_PRINT("info",("spider this=%p", this));
  set_pos_to_first_dbton_id();
  while ((dbton_id = get_next_dbton_id()) < SPIDER_DBTON_SIZE)
  {
    set_pos_to_first_link_idx_chain();
    while ((link_idx_chain = get_next_link_idx_chain()))
    {
      if (link_idx_chain->conn->dbton_id == dbton_id)
      {
        break;
      }
    }
    DBUG_ASSERT(link_idx_chain);
    set_pos_to_first_table_on_link_idx_chain(link_idx_chain);

    set_pos_to_first_table_holder();
    while ((table_holder = get_next_table_holder()))
    {
      link_idx_holder = get_next_table_on_link_idx_chain(link_idx_chain);
      spider = table_holder->spider;
      dbton_hdl = spider->dbton_handler[dbton_id];
      dbton_hdl->first_link_idx = link_idx_holder->link_idx;
    }
  }
  DBUG_VOID_RETURN;
}

int spider_fields::add_link_idx(
  SPIDER_CONN_HOLDER *conn_holder_arg,
  ha_spider *spider_arg,
  int link_idx
) {
  SPIDER_TABLE_LINK_IDX_HOLDER *table_link_idx_holder;
  SPIDER_LINK_IDX_HOLDER *link_idx_holder;
  DBUG_ENTER("spider_fields::add_link_idx");
  DBUG_PRINT("info",("spider this=%p", this));
  table_link_idx_holder =
    &conn_holder_arg->table_link_idx_holder[spider_arg->idx_for_direct_join];
  if (!table_link_idx_holder->first_link_idx_holder)
  {
    link_idx_holder = create_link_idx_holder();
    DBUG_PRINT("info",("spider link_idx_holder=%p", link_idx_holder));
    if (!link_idx_holder)
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    table_link_idx_holder->first_link_idx_holder = link_idx_holder;
    table_link_idx_holder->last_link_idx_holder = link_idx_holder;
    table_link_idx_holder->table_holder =
      &table_holder[spider_arg->idx_for_direct_join];
  } else {
    link_idx_holder = create_link_idx_holder();
    DBUG_PRINT("info",("spider link_idx_holder=%p", link_idx_holder));
    if (!link_idx_holder)
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    table_link_idx_holder->last_link_idx_holder->next = link_idx_holder;
    table_link_idx_holder->last_link_idx_holder = link_idx_holder;
  }
  link_idx_holder->table_link_idx_holder = table_link_idx_holder;
  link_idx_holder->link_idx = link_idx;
  link_idx_holder->link_status = spider_conn_get_link_status(
    spider_arg->share->link_statuses, spider_arg->conn_link_idx,
    link_idx);
  ++table_link_idx_holder->link_idx_holder_count;
  if (conn_holder_arg->link_idx_holder_count_max <
    table_link_idx_holder->link_idx_holder_count)
  {
    conn_holder_arg->link_idx_holder_count_max =
      table_link_idx_holder->link_idx_holder_count;
  }
  DBUG_RETURN(0);
}

SPIDER_LINK_IDX_HOLDER *spider_fields::create_link_idx_holder(
) {
  DBUG_ENTER("spider_fields::create_link_idx_holder");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN((SPIDER_LINK_IDX_HOLDER *)
    spider_malloc(spider_current_trx, 253, sizeof(SPIDER_LINK_IDX_HOLDER),
    MYF(MY_WME | MY_ZEROFILL)));
}

void spider_fields::set_pos_to_first_table_on_link_idx_chain(
  SPIDER_LINK_IDX_CHAIN *link_idx_chain_arg
) {
  DBUG_ENTER("spider_fields::set_pos_to_first_table_on_link_idx_chain");
  DBUG_PRINT("info",("spider this=%p", this));
  link_idx_chain_arg->current_link_idx_holder =
    link_idx_chain_arg->link_idx_holder;
  DBUG_VOID_RETURN;
}

SPIDER_LINK_IDX_HOLDER *spider_fields::get_next_table_on_link_idx_chain(
  SPIDER_LINK_IDX_CHAIN *link_idx_chain_arg
) {
  SPIDER_LINK_IDX_HOLDER *return_link_idx_holder;
  DBUG_ENTER("spider_fields::get_next_table_on_link_idx_chain");
  DBUG_PRINT("info",("spider this=%p", this));
  if (!link_idx_chain_arg->current_link_idx_holder)
    DBUG_RETURN(NULL);
  return_link_idx_holder = link_idx_chain_arg->current_link_idx_holder;
  link_idx_chain_arg->current_link_idx_holder =
    link_idx_chain_arg->current_link_idx_holder->next_table;
  DBUG_RETURN(return_link_idx_holder);
}

SPIDER_CONN_HOLDER *spider_fields::add_conn(
  SPIDER_CONN *conn_arg,
  long access_balance
) {
  SPIDER_CONN_HOLDER *conn_holder;
  DBUG_ENTER("spider_fields::add_conn");
  DBUG_PRINT("info",("spider this=%p", this));
  if (!first_conn_holder)
  {
    conn_holder = create_conn_holder();
    DBUG_PRINT("info",("spider conn_holder=%p", conn_holder));
    if (!conn_holder)
      DBUG_RETURN(NULL);
    conn_holder->conn = conn_arg;
    conn_holder->access_balance = access_balance;
    first_conn_holder = conn_holder;
    last_conn_holder = conn_holder;
    conn_arg->conn_holder_for_direct_join = conn_holder;
    add_dbton_id(conn_arg->dbton_id);
  } else {
    conn_holder = first_conn_holder;
    do {
      if (conn_holder->conn == conn_arg)
        break;
    } while ((conn_holder = conn_holder->next));
    if (!conn_holder)
    {
      conn_holder = create_conn_holder();
      DBUG_PRINT("info",("spider conn_holder=%p", conn_holder));
      if (!conn_holder)
        DBUG_RETURN(NULL);
      conn_holder->conn = conn_arg;
      conn_holder->access_balance = access_balance;
      conn_holder->prev = last_conn_holder;
      last_conn_holder->next = conn_holder;
      last_conn_holder = conn_holder;
      conn_arg->conn_holder_for_direct_join = conn_holder;
      add_dbton_id(conn_arg->dbton_id);
    }
  }
  DBUG_RETURN(conn_holder);
}

SPIDER_CONN_HOLDER *spider_fields::create_conn_holder(
) {
  SPIDER_CONN_HOLDER *return_conn_holder;
  SPIDER_TABLE_LINK_IDX_HOLDER *table_link_idx_holder;
  DBUG_ENTER("spider_fields::create_conn_holder");
  DBUG_PRINT("info",("spider this=%p", this));
  return_conn_holder = (SPIDER_CONN_HOLDER *)
    spider_bulk_malloc(spider_current_trx, 252, MYF(MY_WME | MY_ZEROFILL),
      &return_conn_holder, (uint) (sizeof(SPIDER_CONN_HOLDER)),
      &table_link_idx_holder,
        (uint) (table_count * sizeof(SPIDER_TABLE_LINK_IDX_HOLDER)),
      NullS
    );
  if (!return_conn_holder)
    DBUG_RETURN(NULL);
  DBUG_PRINT("info",("spider table_count=%u", table_count));
  DBUG_PRINT("info",("spider table_link_idx_holder=%p", table_link_idx_holder));
  return_conn_holder->table_link_idx_holder = table_link_idx_holder;
  DBUG_RETURN(return_conn_holder);
}

void spider_fields::set_pos_to_first_conn_holder(
) {
  DBUG_ENTER("spider_fields::set_pos_to_first_conn_holder");
  DBUG_PRINT("info",("spider this=%p", this));
  current_conn_holder = first_conn_holder;
  DBUG_VOID_RETURN;
}

SPIDER_CONN_HOLDER *spider_fields::get_next_conn_holder(
) {
  SPIDER_CONN_HOLDER *return_conn_holder = current_conn_holder;
  DBUG_ENTER("spider_fields::get_next_conn_holder");
  DBUG_PRINT("info",("spider this=%p", this));
  if (current_conn_holder)
    current_conn_holder = current_conn_holder->next;
  DBUG_RETURN(return_conn_holder);
}

bool spider_fields::has_conn_holder(
) {
  DBUG_ENTER("spider_fields::has_conn_holder");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(first_conn_holder);
}

void spider_fields::clear_conn_holder_from_conn(
) {
  DBUG_ENTER("spider_fields::clear_conn_checked_for_same_conn");
  DBUG_PRINT("info",("spider this=%p", this));
  for (current_conn_holder = first_conn_holder; current_conn_holder;
    current_conn_holder = current_conn_holder->next)
  {
    current_conn_holder->checked_for_same_conn = FALSE;
  }
  DBUG_VOID_RETURN;
}

bool spider_fields::check_conn_same_conn(
  SPIDER_CONN *conn_arg
) {
  DBUG_ENTER("spider_fields::check_conn_same_conn");
  DBUG_PRINT("info",("spider this=%p", this));
  for (current_conn_holder = first_conn_holder; current_conn_holder;
    current_conn_holder = current_conn_holder->next)
  {
    if (current_conn_holder->conn == conn_arg)
    {
      current_conn_holder->checked_for_same_conn = TRUE;
      DBUG_RETURN(TRUE);
    }
  }
  DBUG_RETURN(FALSE);
}

bool spider_fields::remove_conn_if_not_checked(
) {
  SPIDER_CONN_HOLDER *conn_holder;
  bool removed = FALSE;
  DBUG_ENTER("spider_fields::remove_conn_if_not_checked");
  DBUG_PRINT("info",("spider this=%p", this));
  current_conn_holder = first_conn_holder;
  while (current_conn_holder)
  {
    if (!current_conn_holder->checked_for_same_conn)
    {
      removed = TRUE;
      DBUG_PRINT("info",("spider remove connection %p",
        current_conn_holder->conn));
      if (!current_conn_holder->prev)
      {
        first_conn_holder = current_conn_holder->next;
        if (current_conn_holder->next)
        {
          current_conn_holder->next->prev = NULL;
        } else {
          last_conn_holder = NULL;
        }
      } else {
        current_conn_holder->prev->next = current_conn_holder->next;
        if (current_conn_holder->next)
        {
          current_conn_holder->next->prev = current_conn_holder->prev;
        } else {
          last_conn_holder = current_conn_holder->prev;
          last_conn_holder->next = NULL;
        }
      }
      conn_holder = current_conn_holder->next;
      free_conn_holder(current_conn_holder);
      current_conn_holder = conn_holder;
    } else {
      current_conn_holder = current_conn_holder->next;
    }
  }
  DBUG_RETURN(removed);
}

void spider_fields::check_support_dbton(
  uchar *dbton_bitmap
) {
  SPIDER_CONN_HOLDER *conn_holder;
  DBUG_ENTER("spider_fields::check_support_dbton");
  DBUG_PRINT("info",("spider this=%p", this));
  current_conn_holder = first_conn_holder;
  while (current_conn_holder)
  {
    if (!spider_bit_is_set(dbton_bitmap, current_conn_holder->conn->dbton_id))
    {
      DBUG_PRINT("info",("spider remove connection %p",
        current_conn_holder->conn));
      if (!current_conn_holder->prev)
      {
        first_conn_holder = current_conn_holder->next;
        if (current_conn_holder->next)
        {
          current_conn_holder->next->prev = NULL;
        } else {
          last_conn_holder = NULL;
        }
      } else {
        current_conn_holder->prev->next = current_conn_holder->next;
        if (current_conn_holder->next)
        {
          current_conn_holder->next->prev = current_conn_holder->prev;
        } else {
          last_conn_holder = current_conn_holder->prev;
          last_conn_holder->next = NULL;
        }
      }
      conn_holder = current_conn_holder->next;
      free_conn_holder(current_conn_holder);
      current_conn_holder = conn_holder;
    } else {
      current_conn_holder = current_conn_holder->next;
    }
  }
  DBUG_VOID_RETURN;
}

void spider_fields::choose_a_conn(
) {
  SPIDER_CONN_HOLDER *conn_holder;
  longlong balance_total = 0, balance_val;
  double rand_val;
  THD *thd = table_holder[0].spider->wide_handler->trx->thd;
  DBUG_ENTER("spider_fields::choose_a_conn");
  DBUG_PRINT("info",("spider this=%p", this));
  for (current_conn_holder = first_conn_holder; current_conn_holder;
    current_conn_holder = current_conn_holder->next)
  {
    balance_total += current_conn_holder->access_balance;
  }

  rand_val = spider_rand(thd->variables.server_id + thd_get_thread_id(thd));
  balance_val = (longlong) (rand_val * balance_total);

  current_conn_holder = first_conn_holder;
  while (current_conn_holder)
  {
    if (balance_val < current_conn_holder->access_balance)
      break;
    balance_val -= current_conn_holder->access_balance;

    DBUG_PRINT("info",("spider remove connection %p",
      current_conn_holder->conn));
    first_conn_holder = current_conn_holder->next;
    DBUG_ASSERT(current_conn_holder->next);
    first_conn_holder->prev = NULL;
    free_conn_holder(current_conn_holder);
    current_conn_holder = first_conn_holder;
  }

  DBUG_PRINT("info",("spider chosen connection is %p",
    current_conn_holder->conn));
  last_conn_holder = current_conn_holder;
  current_conn_holder = current_conn_holder->next;
  last_conn_holder->next = NULL;

  while (current_conn_holder)
  {
    DBUG_PRINT("info",("spider remove connection %p",
    current_conn_holder->conn));
    conn_holder = current_conn_holder->next;
    free_conn_holder(current_conn_holder);
    current_conn_holder = conn_holder;
  }
  DBUG_VOID_RETURN;
}

void spider_fields::free_conn_holder(
  SPIDER_CONN_HOLDER *conn_holder_arg
) {
  uint roop_count;
  DBUG_ENTER("spider_fields::free_conn_holder");
  DBUG_PRINT("info",("spider this=%p", this));
  for (roop_count = 0; roop_count < table_count; ++roop_count)
  {
    if (conn_holder_arg->table_link_idx_holder[roop_count].first_link_idx_holder)
    {
      SPIDER_LINK_IDX_HOLDER *first_link_idx_holder, *current_link_idx_holder;
      first_link_idx_holder =
        conn_holder_arg->table_link_idx_holder[roop_count].first_link_idx_holder;
      while ((current_link_idx_holder = first_link_idx_holder))
      {
        first_link_idx_holder = current_link_idx_holder->next;
        spider_free(spider_current_trx, current_link_idx_holder, MYF(0));
      }
    }
  }
  conn_holder_arg->conn->conn_holder_for_direct_join = NULL;
  DBUG_PRINT("info",("spider free conn_holder=%p", conn_holder_arg));
  spider_free(spider_current_trx, conn_holder_arg, MYF(0));
  DBUG_VOID_RETURN;
}

SPIDER_TABLE_HOLDER *spider_fields::add_table(
  ha_spider *spider_arg
) {
  spider_string *str;
  uint length;
  char tmp_buf[SPIDER_SQL_INT_LEN + 2];
  SPIDER_TABLE_HOLDER *return_table_holder;
  SPIDER_FIELD_HOLDER *field_holder;
  TABLE *table = spider_arg->get_table();
  Field *field;
  DBUG_ENTER("spider_fields::add_table");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_PRINT("info",("spider table_count=%u", table_count));
  DBUG_PRINT("info",("spider idx_for_direct_join=%u",
    spider_arg->idx_for_direct_join));
  length = my_sprintf(tmp_buf, (tmp_buf, "t%u",
    spider_arg->idx_for_direct_join));
  str = &spider_arg->result_list.tmp_sqls[0];
  str->length(0);
  if (str->reserve(length + SPIDER_SQL_DOT_LEN))
  {
    DBUG_RETURN(NULL);
  }
  str->q_append(tmp_buf, length);
  str->q_append(SPIDER_SQL_DOT_STR, SPIDER_SQL_DOT_LEN);

  return_table_holder = &table_holder[spider_arg->idx_for_direct_join];
  return_table_holder->table = spider_arg->get_table();
  return_table_holder->spider = spider_arg;
  return_table_holder->alias = str;

  set_pos_to_first_field_holder();
  while ((field_holder = get_next_field_holder()))
  {
    if (!field_holder->spider)
    {
      field = field_holder->field;
      if (
        field->field_index < table->s->fields &&
        field == table->field[field->field_index]
      ) {
        field_holder->spider = spider_arg;
        field_holder->alias = str;
      }
    }
  }
  DBUG_RETURN(return_table_holder);
}

/**
  Verify that all fields in the query are members of tables that are in the
  query.

  @return TRUE              All fields in the query are members of tables
                            that are in the query.
          FALSE             At least one field in the query is not a
                            member of a table that is in the query.
*/

bool spider_fields::all_query_fields_are_query_table_members()
{
  SPIDER_FIELD_HOLDER *field_holder;
  DBUG_ENTER("spider_fields::all_query_fields_are_query_table_members");
  DBUG_PRINT("info",("spider this=%p", this));

  set_pos_to_first_field_holder();
  while ((field_holder = get_next_field_holder()))
  {
    if (!field_holder->spider)
    {
      DBUG_PRINT("info", ("spider field is not a member of a query table"));
      DBUG_RETURN(FALSE);
    }
  }

  DBUG_RETURN(TRUE);
}

int spider_fields::create_table_holder(
  uint table_count_arg
) {
  DBUG_ENTER("spider_fields::create_table_holder");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_ASSERT(!table_holder);
  table_holder = (SPIDER_TABLE_HOLDER *)
    spider_malloc(spider_current_trx, 249,
    table_count_arg * sizeof(SPIDER_TABLE_HOLDER),
    MYF(MY_WME | MY_ZEROFILL));
  if (!table_holder)
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  table_count = table_count_arg;
  current_table_num = 0;
  DBUG_RETURN(0);
}

void spider_fields::set_pos_to_first_table_holder(
) {
  DBUG_ENTER("spider_fields::set_pos_to_first_table_holder");
  DBUG_PRINT("info",("spider this=%p", this));
  current_table_num = 0;
  DBUG_VOID_RETURN;
}

SPIDER_TABLE_HOLDER *spider_fields::get_next_table_holder(
) {
  SPIDER_TABLE_HOLDER *return_table_holder;
  DBUG_ENTER("spider_fields::get_next_table_holder");
  DBUG_PRINT("info",("spider this=%p", this));
  if (current_table_num >= table_count)
    DBUG_RETURN(NULL);
  return_table_holder = &table_holder[current_table_num];
  ++current_table_num;
  DBUG_RETURN(return_table_holder);
}

SPIDER_TABLE_HOLDER *spider_fields::get_table_holder(TABLE *table)
{
  uint table_num;
  DBUG_ENTER("spider_fields::get_table_holder");
  DBUG_PRINT("info",("spider this=%p", this));
  for (table_num = 0; table_num < table_count; ++table_num)
  {
    if (table_holder[table_num].table == table)
      DBUG_RETURN(&table_holder[table_num]);
  }
  DBUG_RETURN(NULL);
}

uint spider_fields::get_table_count()
{
  DBUG_ENTER("spider_fields::get_table_count");
  DBUG_RETURN(table_count);
}

int spider_fields::add_field(
  Field *field_arg
) {
  SPIDER_FIELD_HOLDER *field_holder;
  SPIDER_FIELD_CHAIN *field_chain;
  DBUG_ENTER("spider_fields::add_field");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_PRINT("info",("spider field=%p", field_arg));
  if (!first_field_holder)
  {
    field_holder = create_field_holder();
    DBUG_PRINT("info",("spider field_holder=%p", field_holder));
    if (!field_holder)
      DBUG_RETURN(HA_ERR_OUT_OF_MEM);
    field_holder->field = field_arg;
    first_field_holder = field_holder;
    last_field_holder = field_holder;
  } else {
    field_holder = first_field_holder;
    do {
      if (field_holder->field == field_arg)
        break;
    } while ((field_holder = field_holder->next));
    if (!field_holder)
    {
      field_holder = create_field_holder();
      DBUG_PRINT("info",("spider field_holder=%p", field_holder));
      if (!field_holder)
        DBUG_RETURN(HA_ERR_OUT_OF_MEM);
      field_holder->field = field_arg;
      last_field_holder->next = field_holder;
      last_field_holder = field_holder;
    }
  }
  field_chain = create_field_chain();
  DBUG_PRINT("info",("spider field_chain=%p", field_chain));
  if (!field_chain)
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);
  field_chain->field_holder = field_holder;
  if (!first_field_chain)
  {
    first_field_chain = field_chain;
    last_field_chain = field_chain;
  } else {
    last_field_chain->next = field_chain;
    last_field_chain = field_chain;
  }
  DBUG_RETURN(0);
}

SPIDER_FIELD_HOLDER *spider_fields::create_field_holder(
) {
  DBUG_ENTER("spider_fields::create_field_holder");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN((SPIDER_FIELD_HOLDER *)
    spider_malloc(spider_current_trx, 250, sizeof(SPIDER_FIELD_HOLDER),
    MYF(MY_WME | MY_ZEROFILL)));
}

void spider_fields::set_pos_to_first_field_holder(
) {
  DBUG_ENTER("spider_fields::set_pos_to_first_field_holder");
  DBUG_PRINT("info",("spider this=%p", this));
  current_field_holder = first_field_holder;
  DBUG_VOID_RETURN;
}

SPIDER_FIELD_HOLDER *spider_fields::get_next_field_holder(
) {
  SPIDER_FIELD_HOLDER *return_field_holder = current_field_holder;
  DBUG_ENTER("spider_fields::get_next_field_holder");
  DBUG_PRINT("info",("spider this=%p", this));
  if (current_field_holder)
    current_field_holder = current_field_holder->next;
  DBUG_RETURN(return_field_holder);
}

SPIDER_FIELD_CHAIN *spider_fields::create_field_chain(
) {
  DBUG_ENTER("spider_fields::create_field_chain");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN((SPIDER_FIELD_CHAIN *)
    spider_malloc(spider_current_trx, 251, sizeof(SPIDER_FIELD_CHAIN),
    MYF(MY_WME | MY_ZEROFILL)));
}

void spider_fields::set_pos_to_first_field_chain(
) {
  DBUG_ENTER("spider_fields::set_pos_to_first_field_chain");
  DBUG_PRINT("info",("spider this=%p", this));
  current_field_chain = first_field_chain;
  DBUG_VOID_RETURN;
}

SPIDER_FIELD_CHAIN *spider_fields::get_next_field_chain(
) {
  SPIDER_FIELD_CHAIN *return_field_chain = current_field_chain;
  DBUG_ENTER("spider_fields::get_next_field_chain");
  DBUG_PRINT("info",("spider this=%p", this));
  if (current_field_chain)
    current_field_chain = current_field_chain->next;
  DBUG_RETURN(return_field_chain);
}

void spider_fields::set_field_ptr(
  Field **field_arg
) {
  DBUG_ENTER("spider_fields::set_field_ptr");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_PRINT("info",("spider field_ptr=%p", field_arg));
  first_field_ptr = field_arg;
  current_field_ptr = field_arg;
  DBUG_VOID_RETURN;
}

Field **spider_fields::get_next_field_ptr(
) {
  Field **return_field_ptr = current_field_ptr;
  DBUG_ENTER("spider_fields::get_next_field_ptr");
  DBUG_PRINT("info",("spider this=%p", this));
  if (*current_field_ptr)
    current_field_ptr++;
  DBUG_PRINT("info",("spider field_ptr=%p", return_field_ptr));
  DBUG_RETURN(return_field_ptr);
}

int spider_fields::ping_table_mon_from_table(
  SPIDER_LINK_IDX_CHAIN *link_idx_chain
) {
  int error_num = 0, error_num_buf;
  ha_spider *tmp_spider;
  SPIDER_SHARE *tmp_share;
  int tmp_link_idx;
  SPIDER_TABLE_HOLDER *table_holder;
  SPIDER_LINK_IDX_HOLDER *link_idx_holder;
  DBUG_ENTER("spider_fields::ping_table_mon_from_table");
  set_pos_to_first_table_on_link_idx_chain(link_idx_chain);
  set_pos_to_first_table_holder();
  while ((table_holder = get_next_table_holder()))
  {
    link_idx_holder = get_next_table_on_link_idx_chain(link_idx_chain);
    tmp_spider = table_holder->spider;
    tmp_link_idx = link_idx_holder->link_idx;
    tmp_share = tmp_spider->share;
    if (tmp_share->monitoring_kind[tmp_link_idx])
    {
      error_num_buf = spider_ping_table_mon_from_table(
          tmp_spider->wide_handler->trx,
          tmp_spider->wide_handler->trx->thd,
          tmp_share,
          tmp_link_idx,
          (uint32) tmp_share->monitoring_sid[tmp_link_idx],
          tmp_share->table_name,
          tmp_share->table_name_length,
          tmp_spider->conn_link_idx[tmp_link_idx],
          NULL,
          0,
          tmp_share->monitoring_kind[tmp_link_idx],
          tmp_share->monitoring_limit[tmp_link_idx],
          tmp_share->monitoring_flag[tmp_link_idx],
          TRUE
        );
      if (!error_num)
        error_num = error_num_buf;
    }
  }
  DBUG_RETURN(error_num);
}

spider_group_by_handler::spider_group_by_handler(
  THD *thd_arg,
  Query *query_arg,
  spider_fields *fields_arg
) : group_by_handler(thd_arg, spider_hton_ptr),
  query(*query_arg), fields(fields_arg)
{
  DBUG_ENTER("spider_group_by_handler::spider_group_by_handler");
  fields->set_pos_to_first_table_holder();
  SPIDER_TABLE_HOLDER *table_holder = fields->get_next_table_holder();
  spider = table_holder->spider;
  trx = spider->wide_handler->trx;
  DBUG_VOID_RETURN;
}

spider_group_by_handler::~spider_group_by_handler()
{
  DBUG_ENTER("spider_group_by_handler::~spider_group_by_handler");
  delete fields;
  DBUG_VOID_RETURN;
}

int spider_group_by_handler::init_scan()
{
  int error_num, link_idx;
  uint dbton_id;
  spider_db_handler *dbton_hdl;
  st_select_lex *select_lex;
  longlong select_limit;
  longlong direct_order_limit;
  SPIDER_SHARE *share = spider->share;
  SPIDER_CONN *conn;
  SPIDER_RESULT_LIST *result_list = &spider->result_list;
  SPIDER_LINK_IDX_CHAIN *link_idx_chain;
  SPIDER_LINK_IDX_HOLDER *link_idx_holder;
  DBUG_ENTER("spider_group_by_handler::init_scan");
  store_error = 0;
#ifndef DBUG_OFF
  Field **field;
  for (
    field = table->field;
    *field;
    field++
  ) {
    DBUG_PRINT("info",("spider field_name=%s",
      SPIDER_field_name_str(*field)));
  }
#endif

  if (trx->thd->killed)
  {
    my_error(ER_QUERY_INTERRUPTED, MYF(0));
    DBUG_RETURN(ER_QUERY_INTERRUPTED);
  }

  spider->use_fields = TRUE;
  spider->fields = fields;

  spider->check_pre_call(TRUE);

  spider->pushed_pos = NULL;
  result_list->sorted = (query.group_by || query.order_by);
  spider_set_result_list_param(spider);
  spider->mrr_with_cnt = FALSE;
  spider->init_index_handler = FALSE;
  spider->use_spatial_index = FALSE;
  result_list->check_direct_order_limit = FALSE;
  spider->select_column_mode = 0;
  spider->search_link_idx = fields->get_ok_link_idx();
  spider->result_link_idx = spider->search_link_idx;

  spider_db_free_one_result_for_start_next(spider);

  spider->sql_kinds = SPIDER_SQL_KIND_SQL;
  for (link_idx = 0; link_idx < (int) share->link_count; ++link_idx)
    spider->sql_kind[link_idx] = SPIDER_SQL_KIND_SQL;

  spider->do_direct_update = FALSE;
  spider->direct_update_kinds = 0;
  spider_get_select_limit(spider, &select_lex, &select_limit, &offset_limit);
  direct_order_limit = spider_param_direct_order_limit(thd,
    share->direct_order_limit);
  if (
    direct_order_limit &&
    select_lex->limit_params.explicit_limit &&
    !(select_lex->options & OPTION_FOUND_ROWS) &&
    select_limit < direct_order_limit /* - offset_limit */
  ) {
    result_list->internal_limit = select_limit /* + offset_limit */;
    result_list->split_read = select_limit /* + offset_limit */;
    result_list->bgs_split_read = select_limit /* + offset_limit */;

    result_list->split_read_base = 9223372036854775807LL;
    result_list->semi_split_read = 0;
    result_list->semi_split_read_limit = 9223372036854775807LL;
    result_list->first_read = 9223372036854775807LL;
    result_list->second_read = 9223372036854775807LL;
    trx->direct_order_limit_count++;
  }
  result_list->semi_split_read_base = 0;
  result_list->set_split_read = TRUE;
  if ((error_num = spider_set_conn_bg_param(spider)))
    DBUG_RETURN(error_num);
  DBUG_PRINT("info",("spider result_list.finish_flg = FALSE"));
  result_list->finish_flg = FALSE;
  result_list->record_num = 0;
  result_list->keyread = FALSE;
  result_list->desc_flg = FALSE;
  result_list->sorted = FALSE;
  result_list->key_info = NULL;
  result_list->key_order = 0;
  result_list->limit_num =
    result_list->internal_limit >= result_list->split_read ?
    result_list->split_read : result_list->internal_limit;

  if (select_lex->limit_params.explicit_limit)
  {
    result_list->internal_offset += offset_limit;
  } else {
    offset_limit = 0;
  }

  /* making a query */
  fields->set_pos_to_first_dbton_id();
  while ((dbton_id = fields->get_next_dbton_id()) < SPIDER_DBTON_SIZE)
  {
    dbton_hdl = spider->dbton_handler[dbton_id];
    result_list->direct_distinct = query.distinct;
    fields->set_pos_to_first_field_chain();
    if ((error_num = dbton_hdl->reset_sql(SPIDER_SQL_TYPE_SELECT_SQL)))
    {
      DBUG_RETURN(error_num);
    }
    if ((error_num = dbton_hdl->append_select_part(SPIDER_SQL_TYPE_SELECT_SQL)))
    {
      DBUG_RETURN(error_num);
    }
    fields->set_field_ptr(table->field);
    if ((error_num = dbton_hdl->append_list_item_select_part(
      query.select, NULL, 0, TRUE, fields, SPIDER_SQL_TYPE_SELECT_SQL)))
    {
      DBUG_RETURN(error_num);
    }
    if ((error_num = dbton_hdl->append_from_and_tables_part(
      fields, SPIDER_SQL_TYPE_SELECT_SQL)))
    {
      DBUG_RETURN(error_num);
    }
    if (query.where)
    {
      if ((error_num =
        dbton_hdl->append_where_part(SPIDER_SQL_TYPE_SELECT_SQL)))
      {
        DBUG_RETURN(error_num);
      }
      if ((error_num = dbton_hdl->append_item_type_part(
        query.where, NULL, 0, TRUE, fields, SPIDER_SQL_TYPE_SELECT_SQL)))
      {
        DBUG_RETURN(error_num);
      }
    }
    if (query.group_by)
    {
      if ((error_num = dbton_hdl->append_group_by_part(
        query.group_by, NULL, 0, TRUE, fields, SPIDER_SQL_TYPE_SELECT_SQL)))
      {
        DBUG_RETURN(error_num);
      }
    }
    if (query.having)
    {
      if ((error_num =
        dbton_hdl->append_having_part(SPIDER_SQL_TYPE_SELECT_SQL)))
      {
        DBUG_RETURN(error_num);
      }
      if ((error_num = dbton_hdl->append_item_type_part(
        query.having, NULL, 0, TRUE, fields, SPIDER_SQL_TYPE_SELECT_SQL)))
      {
        DBUG_RETURN(error_num);
      }
    }
    if (query.order_by)
    {
      if ((error_num = dbton_hdl->append_order_by_part(
        query.order_by, NULL, 0, TRUE, fields, SPIDER_SQL_TYPE_SELECT_SQL)))
      {
        DBUG_RETURN(error_num);
      }
    }
    if ((error_num = dbton_hdl->append_limit_part(result_list->internal_offset,
      result_list->limit_num, SPIDER_SQL_TYPE_SELECT_SQL)))
    {
      DBUG_RETURN(error_num);
    }
    if ((error_num = dbton_hdl->append_select_lock_part(
      SPIDER_SQL_TYPE_SELECT_SQL)))
    {
      DBUG_RETURN(error_num);
    }
  }

  fields->set_pos_to_first_link_idx_chain();
  while ((link_idx_chain = fields->get_next_link_idx_chain()))
  {
    conn = link_idx_chain->conn;
    link_idx_holder = link_idx_chain->link_idx_holder;
    link_idx = link_idx_holder->link_idx;
    dbton_hdl = spider->dbton_handler[conn->dbton_id];
    spider->link_idx_chain = link_idx_chain;
    if (result_list->bgs_phase > 0)
    {
      if ((error_num = spider_check_and_init_casual_read(trx->thd, spider,
        link_idx)))
        DBUG_RETURN(error_num);
      if ((error_num = spider_bg_conn_search(spider, link_idx,
        dbton_hdl->first_link_idx, TRUE, FALSE,
        !fields->is_first_link_ok_chain(link_idx_chain))))
      {
        if (
          error_num != HA_ERR_END_OF_FILE &&
          spider->need_mons[link_idx]
        ) {
          error_num = fields->ping_table_mon_from_table(link_idx_chain);
        }
        if ((error_num = spider->check_error_mode_eof(error_num)) == HA_ERR_END_OF_FILE)
        {
          store_error = HA_ERR_END_OF_FILE;
          error_num = 0;
        }
        DBUG_RETURN(error_num);
      }
    } else {
      pthread_mutex_assert_not_owner(&conn->mta_conn_mutex);
      if (dbton_hdl->need_lock_before_set_sql_for_exec(
        SPIDER_SQL_TYPE_SELECT_SQL))
      {
        pthread_mutex_lock(&conn->mta_conn_mutex);
        SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
      }
      if ((error_num =
        dbton_hdl->set_sql_for_exec(SPIDER_SQL_TYPE_SELECT_SQL, link_idx,
        link_idx_chain)))
      {
        if (dbton_hdl->need_lock_before_set_sql_for_exec(
          SPIDER_SQL_TYPE_SELECT_SQL))
        {
          SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
          pthread_mutex_unlock(&conn->mta_conn_mutex);
        }
        DBUG_RETURN(error_num);
      }
      if (!dbton_hdl->need_lock_before_set_sql_for_exec(
        SPIDER_SQL_TYPE_SELECT_SQL))
      {
        pthread_mutex_lock(&conn->mta_conn_mutex);
        SPIDER_SET_FILE_POS(&conn->mta_conn_mutex_file_pos);
      }
      conn->need_mon = &spider->need_mons[link_idx];
      DBUG_ASSERT(!conn->mta_conn_mutex_lock_already);
      DBUG_ASSERT(!conn->mta_conn_mutex_unlock_later);
      conn->mta_conn_mutex_lock_already = TRUE;
      conn->mta_conn_mutex_unlock_later = TRUE;
      if ((error_num = spider_db_set_names(spider, conn,
        link_idx)))
      {
        DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
        DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
        conn->mta_conn_mutex_lock_already = FALSE;
        conn->mta_conn_mutex_unlock_later = FALSE;
        SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
        pthread_mutex_unlock(&conn->mta_conn_mutex);
        if (
          spider->need_mons[link_idx]
        ) {
          error_num = fields->ping_table_mon_from_table(link_idx_chain);
        }
        if ((error_num = spider->check_error_mode_eof(error_num)) == HA_ERR_END_OF_FILE)
        {
          store_error = HA_ERR_END_OF_FILE;
          error_num = 0;
        }
        DBUG_RETURN(error_num);
      }
      spider_conn_set_timeout_from_share(conn, link_idx,
        trx->thd, share);
      if (dbton_hdl->execute_sql(
        SPIDER_SQL_TYPE_SELECT_SQL,
        conn,
        spider->result_list.quick_mode,
        &spider->need_mons[link_idx])
      ) {
        DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
        DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
        conn->mta_conn_mutex_lock_already = FALSE;
        conn->mta_conn_mutex_unlock_later = FALSE;
        error_num = spider_db_errorno(conn);
        if (
          spider->need_mons[link_idx]
        ) {
          error_num = fields->ping_table_mon_from_table(link_idx_chain);
        }
        if ((error_num = spider->check_error_mode_eof(error_num)) == HA_ERR_END_OF_FILE)
        {
          store_error = HA_ERR_END_OF_FILE;
          error_num = 0;
        }
        DBUG_RETURN(error_num);
      }
      spider->connection_ids[link_idx] = conn->connection_id;
      DBUG_ASSERT(conn->mta_conn_mutex_lock_already);
      DBUG_ASSERT(conn->mta_conn_mutex_unlock_later);
      conn->mta_conn_mutex_lock_already = FALSE;
      conn->mta_conn_mutex_unlock_later = FALSE;
      if (fields->is_first_link_ok_chain(link_idx_chain))
      {
        if ((error_num = spider_db_store_result(spider, link_idx, table)))
        {
          if (
            error_num != HA_ERR_END_OF_FILE &&
            spider->need_mons[link_idx]
          ) {
            error_num = fields->ping_table_mon_from_table(link_idx_chain);
          }
          if ((error_num = spider->check_error_mode_eof(error_num)) == HA_ERR_END_OF_FILE)
          {
            store_error = HA_ERR_END_OF_FILE;
            error_num = 0;
          }
          DBUG_RETURN(error_num);
        }
        spider->result_link_idx = link_idx;
        spider->result_link_idx_chain = link_idx_chain;
      } else {
        spider_db_discard_result(spider, link_idx, conn);
        SPIDER_CLEAR_FILE_POS(&conn->mta_conn_mutex_file_pos);
        pthread_mutex_unlock(&conn->mta_conn_mutex);
      }
    }
  }

  first = TRUE;
  DBUG_RETURN(0);
}

int spider_group_by_handler::next_row()
{
  int error_num, link_idx;
  spider_db_handler *dbton_hdl;
  SPIDER_CONN *conn;
  SPIDER_LINK_IDX_CHAIN *link_idx_chain;
  SPIDER_LINK_IDX_HOLDER *link_idx_holder;
  DBUG_ENTER("spider_group_by_handler::next_row");
  if (trx->thd->killed)
  {
    my_error(ER_QUERY_INTERRUPTED, MYF(0));
    DBUG_RETURN(ER_QUERY_INTERRUPTED);
  }
  if (store_error)
  {
    if (store_error == HA_ERR_END_OF_FILE)
    {
      table->status = STATUS_NOT_FOUND;
    }
    DBUG_RETURN(store_error);
  }
  if (first)
  {
    first = FALSE;
    if (spider->use_pre_call)
    {
      if (spider->store_error_num)
      {
        if (spider->store_error_num == HA_ERR_END_OF_FILE)
          table->status = STATUS_NOT_FOUND;
        DBUG_RETURN(spider->store_error_num);
      }
      if (spider->result_list.bgs_phase > 0)
      {
        fields->set_pos_to_first_link_idx_chain();
        while ((link_idx_chain = fields->get_next_link_idx_chain()))
        {
          conn = link_idx_chain->conn;
          link_idx_holder = link_idx_chain->link_idx_holder;
          link_idx = link_idx_holder->link_idx;
          dbton_hdl = spider->dbton_handler[conn->dbton_id];
          spider->link_idx_chain = link_idx_chain;
          if ((error_num = spider_bg_conn_search(spider, link_idx,
            dbton_hdl->first_link_idx, TRUE, TRUE,
            !fields->is_first_link_ok_chain(link_idx_chain))))
          {
            if (
              error_num != HA_ERR_END_OF_FILE &&
              spider->need_mons[link_idx]
            ) {
              error_num = fields->ping_table_mon_from_table(link_idx_chain);
            }
            if ((error_num = spider->check_error_mode_eof(error_num)) == HA_ERR_END_OF_FILE)
            {
              table->status = STATUS_NOT_FOUND;
            }
            DBUG_RETURN(error_num);
          }
        }
      }
      spider->use_pre_call = FALSE;
    }
  } else if (offset_limit)
  {
    --offset_limit;
    DBUG_RETURN(0);
  }
  if ((error_num = spider_db_seek_next(table->record[0], spider,
    spider->search_link_idx, table)))
  {
    if ((error_num = spider->check_error_mode_eof(error_num)) == HA_ERR_END_OF_FILE)
    {
      table->status = STATUS_NOT_FOUND;
    }
    DBUG_RETURN(error_num);
  }
  DBUG_RETURN(0);
}

int spider_group_by_handler::end_scan()
{
  DBUG_ENTER("spider_group_by_handler::end_scan");
  DBUG_RETURN(0);
}

group_by_handler *spider_create_group_by_handler(
  THD *thd,
  Query *query
) {
  spider_group_by_handler *group_by_handler;
  Item *item;
  TABLE_LIST *from;
  SPIDER_CONN *conn;
  ha_spider *spider;
  SPIDER_SHARE *share;
  int roop_count, lock_mode;
  List_iterator_fast<Item> it(*query->select);
  uchar dbton_bitmap[spider_bitmap_size(SPIDER_DBTON_SIZE)];
  uchar dbton_bitmap_tmp[spider_bitmap_size(SPIDER_DBTON_SIZE)];
  ORDER *order;
  bool keep_going;
  bool find_dbton = FALSE;
  spider_fields *fields = NULL, *fields_arg = NULL;
  uint table_idx, dbton_id;
  long tgt_link_status;
  DBUG_ENTER("spider_create_group_by_handler");

  switch (thd_sql_command(thd))
  {
    case SQLCOM_UPDATE:
    case SQLCOM_UPDATE_MULTI:
    case SQLCOM_DELETE:
    case SQLCOM_DELETE_MULTI:
      DBUG_PRINT("info",("spider update and delete does not support this feature"));
      DBUG_RETURN(NULL);
    default:
      break;
  }

  from = query->from;
  do {
    DBUG_PRINT("info",("spider from=%p", from));
    if (from->table->const_table)
      continue;
    if (from->table->part_info)
    {
      DBUG_PRINT("info",("spider partition handler"));
      partition_info *part_info = from->table->part_info;
      uint bits = bitmap_bits_set(&part_info->read_partitions);
      DBUG_PRINT("info",("spider bits=%u", bits));
      if (bits != 1)
      {
        DBUG_PRINT("info",("spider using multiple partitions is not supported by this feature yet"));
        DBUG_RETURN(NULL);
      }
    }
  } while ((from = from->next_local));

  table_idx = 0;
  from = query->from;
  while (from && from->table->const_table)
  {
    from = from->next_local;
  }
  if (!from)
  {
    /* all tables are const_table */
    DBUG_RETURN(NULL);
  }
  if (from->table->part_info)
  {
    partition_info *part_info = from->table->part_info;
    uint part = bitmap_get_first_set(&part_info->read_partitions);
    ha_partition *partition = (ha_partition *) from->table->file;
    handler **handlers = partition->get_child_handlers();
    spider = (ha_spider *) handlers[part];
  } else {
    spider = (ha_spider *) from->table->file;
  }
  share = spider->share;
  spider->idx_for_direct_join = table_idx;
  ++table_idx;
  memset(dbton_bitmap, 0, spider_bitmap_size(SPIDER_DBTON_SIZE));
  for (roop_count = 0; roop_count < (int) share->use_dbton_count; ++roop_count)
  {
    dbton_id = share->use_sql_dbton_ids[roop_count];
    if (
      spider_dbton[dbton_id].support_direct_join &&
      spider_dbton[dbton_id].support_direct_join()
    ) {
      spider_set_bit(dbton_bitmap, dbton_id);
    }
  }
  while ((from = from->next_local))
  {
    if (from->table->const_table)
      continue;
    if (from->table->part_info)
    {
      partition_info *part_info = from->table->part_info;
      uint part = bitmap_get_first_set(&part_info->read_partitions);
      ha_partition *partition = (ha_partition *) from->table->file;
      handler **handlers = partition->get_child_handlers();
      spider = (ha_spider *) handlers[part];
    } else {
      spider = (ha_spider *) from->table->file;
    }
    share = spider->share;
    spider->idx_for_direct_join = table_idx;
    ++table_idx;
    memset(dbton_bitmap_tmp, 0, spider_bitmap_size(SPIDER_DBTON_SIZE));
    for (roop_count = 0; roop_count < (int) share->use_dbton_count; ++roop_count)
    {
      dbton_id = share->use_sql_dbton_ids[roop_count];
      if (
        spider_dbton[dbton_id].support_direct_join &&
        spider_dbton[dbton_id].support_direct_join()
      ) {
        spider_set_bit(dbton_bitmap_tmp, dbton_id);
      }
    }
    for (roop_count = 0;
      roop_count < spider_bitmap_size(SPIDER_DBTON_SIZE); ++roop_count)
    {
      dbton_bitmap[roop_count] &= dbton_bitmap_tmp[roop_count];
    }
  }

  from = query->from;
  do {
    if (from->table->const_table)
      continue;
    if (from->table->part_info)
    {
      partition_info *part_info = from->table->part_info;
      uint part = bitmap_get_first_set(&part_info->read_partitions);
      ha_partition *partition = (ha_partition *) from->table->file;
      handler **handlers = partition->get_child_handlers();
      spider = (ha_spider *) handlers[part];
    } else {
      spider = (ha_spider *) from->table->file;
    }
    share = spider->share;
    if (spider_param_skip_default_condition(thd,
      share->skip_default_condition))
    {
      /* find skip_default_condition = 1 */
      break;
    }
  } while ((from = from->next_local));

  for (roop_count = 0; roop_count < SPIDER_DBTON_SIZE; ++roop_count)
  {
    if (spider_bit_is_set(dbton_bitmap, roop_count))
    {
      if (!fields)
      {
        fields_arg = new spider_fields();
        if (!fields_arg)
        {
          DBUG_RETURN(NULL);
        }
      }
      keep_going = TRUE;
      it.init(*query->select);
      while ((item = it++))
      {
        DBUG_PRINT("info",("spider select item=%p", item));
        if (item->const_item())
        {
          DBUG_PRINT("info",("spider const item"));
          continue;
        }
        if (spider_db_print_item_type(item, NULL, spider, NULL, NULL, 0,
          roop_count, TRUE, fields_arg))
        {
          DBUG_PRINT("info",("spider dbton_id=%d can't create select", roop_count));
          spider_clear_bit(dbton_bitmap, roop_count);
          keep_going = FALSE;
          break;
        }
      }
      if (keep_going)
      {
        if (spider_dbton[roop_count].db_util->append_from_and_tables(
          spider, fields_arg, NULL, query->from, table_idx))
        {
          DBUG_PRINT("info",("spider dbton_id=%d can't create from", roop_count));
          spider_clear_bit(dbton_bitmap, roop_count);
          keep_going = FALSE;
        }
      }
      if (keep_going)
      {
        DBUG_PRINT("info",("spider query->where=%p", query->where));
        if (query->where)
        {
          if (spider_db_print_item_type(query->where, NULL, spider, NULL, NULL, 0,
            roop_count, TRUE, fields_arg))
          {
            DBUG_PRINT("info",("spider dbton_id=%d can't create where", roop_count));
            spider_clear_bit(dbton_bitmap, roop_count);
            keep_going = FALSE;
          }
        }
      }
      if (keep_going)
      {
        DBUG_PRINT("info",("spider query->group_by=%p", query->group_by));
        if (query->group_by)
        {
          for (order = query->group_by; order; order = order->next)
          {
            if (spider_db_print_item_type((*order->item), NULL, spider, NULL, NULL, 0,
              roop_count, TRUE, fields_arg))
            {
              DBUG_PRINT("info",("spider dbton_id=%d can't create group by", roop_count));
              spider_clear_bit(dbton_bitmap, roop_count);
              keep_going = FALSE;
              break;
            }
          }
        }
      }
      if (keep_going)
      {
        DBUG_PRINT("info",("spider query->order_by=%p", query->order_by));
        if (query->order_by)
        {
          for (order = query->order_by; order; order = order->next)
          {
            if (spider_db_print_item_type((*order->item), NULL, spider, NULL, NULL, 0,
              roop_count, TRUE, fields_arg))
            {
              DBUG_PRINT("info",("spider dbton_id=%d can't create order by", roop_count));
              spider_clear_bit(dbton_bitmap, roop_count);
              keep_going = FALSE;
              break;
            }
          }
        }
      }
      if (keep_going)
      {
        DBUG_PRINT("info",("spider query->having=%p", query->having));
        if (query->having)
        {
          if (spider_db_print_item_type(query->having, NULL, spider, NULL, NULL, 0,
            roop_count, TRUE, fields_arg))
          {
            DBUG_PRINT("info",("spider dbton_id=%d can't create having", roop_count));
            spider_clear_bit(dbton_bitmap, roop_count);
            keep_going = FALSE;
          }
        }
      }
      if (keep_going)
      {
        find_dbton = TRUE;
        fields = fields_arg;
        fields_arg = NULL;
      } else {
        delete fields_arg;
      }
    }
  }
  if (!find_dbton)
  {
    DBUG_RETURN(NULL);
  }

  if (fields->create_table_holder(table_idx))
  {
    delete fields;
    DBUG_RETURN(NULL);
  }

  from = query->from;
  while (from->table->const_table)
  {
    from = from->next_local;
  }
  if (from->table->part_info)
  {
    partition_info *part_info = from->table->part_info;
    uint part = bitmap_get_first_set(&part_info->read_partitions);
    ha_partition *partition = (ha_partition *) from->table->file;
    handler **handlers = partition->get_child_handlers();
    spider = (ha_spider *) handlers[part];
  } else {
    spider = (ha_spider *) from->table->file;
  }
  share = spider->share;
  lock_mode = spider_conn_lock_mode(spider);
  if (lock_mode)
  {
    tgt_link_status = SPIDER_LINK_STATUS_RECOVERY;
  } else {
    tgt_link_status = SPIDER_LINK_STATUS_OK;
  }
  DBUG_PRINT("info",("spider s->db=%s", from->table->s->db.str));
  DBUG_PRINT("info",("spider s->table_name=%s", from->table->s->table_name.str));
  if (!fields->add_table(spider))
  {
    DBUG_PRINT("info",("spider can not add a table"));
    delete fields;
    DBUG_RETURN(NULL);
  }
  if (spider->dml_init())
  {
    DBUG_PRINT("info",("spider can not init for dml"));
    delete fields;
    DBUG_RETURN(NULL);
  }
  for (
    roop_count = spider_conn_link_idx_next(share->link_statuses,
      spider->conn_link_idx, -1, share->link_count,
      tgt_link_status);
    roop_count < (int) share->link_count;
    roop_count = spider_conn_link_idx_next(share->link_statuses,
      spider->conn_link_idx, roop_count, share->link_count,
      tgt_link_status)
  ) {
    if (spider_param_use_handler(thd, share->use_handlers[roop_count]))
    {
      DBUG_PRINT("info",("spider direct_join does not support use_handler"));
      if (lock_mode)
      {
        delete fields;
        DBUG_RETURN(NULL);
      }
      continue;
    }
    conn = spider->conns[roop_count];
    DBUG_PRINT("info",("spider roop_count=%d", roop_count));
    DBUG_PRINT("info",("spider conn=%p", conn));
    DBUG_ASSERT(conn);
    if (conn->table_lock)
    {
      DBUG_PRINT("info",("spider direct_join does not support with lock tables yet"));
      if (lock_mode)
      {
        delete fields;
        DBUG_RETURN(NULL);
      }
      continue;
    }
    if (!fields->add_conn(conn,
      share->access_balances[spider->conn_link_idx[roop_count]]))
    {
      DBUG_PRINT("info",("spider can not create conn_holder"));
      delete fields;
      DBUG_RETURN(NULL);
    }
    if (fields->add_link_idx(conn->conn_holder_for_direct_join, spider, roop_count))
    {
      DBUG_PRINT("info",("spider can not create link_idx_holder"));
      delete fields;
      DBUG_RETURN(NULL);
    }
  }
  if (!fields->has_conn_holder())
  {
    delete fields;
    DBUG_RETURN(NULL);
  }

  while ((from = from->next_local))
  {
    if (from->table->const_table)
      continue;
    fields->clear_conn_holder_from_conn();

    if (from->table->part_info)
    {
      partition_info *part_info = from->table->part_info;
      uint part = bitmap_get_first_set(&part_info->read_partitions);
      ha_partition *partition = (ha_partition *) from->table->file;
      handler **handlers = partition->get_child_handlers();
      spider = (ha_spider *) handlers[part];
    } else {
      spider = (ha_spider *) from->table->file;
    }
    share = spider->share;
    if (!fields->add_table(spider))
    {
      DBUG_PRINT("info",("spider can not add a table"));
      delete fields;
      DBUG_RETURN(NULL);
    }
    DBUG_PRINT("info",("spider s->db=%s", from->table->s->db.str));
    DBUG_PRINT("info",("spider s->table_name=%s", from->table->s->table_name.str));
    if (spider->dml_init())
    {
      DBUG_PRINT("info",("spider can not init for dml"));
      delete fields;
      DBUG_RETURN(NULL);
    }
    for (
      roop_count = spider_conn_link_idx_next(share->link_statuses,
        spider->conn_link_idx, -1, share->link_count,
        tgt_link_status);
      roop_count < (int) share->link_count;
      roop_count = spider_conn_link_idx_next(share->link_statuses,
        spider->conn_link_idx, roop_count, share->link_count,
        tgt_link_status)
    ) {
      DBUG_PRINT("info",("spider roop_count=%d", roop_count));
      if (spider_param_use_handler(thd, share->use_handlers[roop_count]))
      {
        DBUG_PRINT("info",("spider direct_join does not support use_handler"));
        if (lock_mode)
        {
          delete fields;
          DBUG_RETURN(NULL);
        }
        continue;
      }
      conn = spider->conns[roop_count];
      DBUG_PRINT("info",("spider conn=%p", conn));
      if (!fields->check_conn_same_conn(conn))
      {
        DBUG_PRINT("info",("spider connection %p can not be used for this query with locking",
          conn));
        if (lock_mode)
        {
          delete fields;
          DBUG_RETURN(NULL);
        }
        continue;
      }
      if (fields->add_link_idx(conn->conn_holder_for_direct_join, spider, roop_count))
      {
        DBUG_PRINT("info",("spider can not create link_idx_holder"));
        delete fields;
        DBUG_RETURN(NULL);
      }
    }

    if (fields->remove_conn_if_not_checked())
    {
      if (lock_mode)
      {
        DBUG_PRINT("info",("spider some connections can not be used for this query with locking"));
        delete fields;
        DBUG_RETURN(NULL);
      }
    }
    if (!fields->has_conn_holder())
    {
      delete fields;
      DBUG_RETURN(NULL);
    }
  }

  if (!fields->all_query_fields_are_query_table_members())
  {
    DBUG_PRINT("info", ("spider found a query field that is not a query table member"));
    delete fields;
    DBUG_RETURN(NULL);
  }

  fields->check_support_dbton(dbton_bitmap);
  if (!fields->has_conn_holder())
  {
    DBUG_PRINT("info",("spider all chosen connections can't match dbton_id"));
    delete fields;
    DBUG_RETURN(NULL);
  }

  /* choose a connection */
  if (!lock_mode)
  {
    fields->choose_a_conn();
  }

  if (fields->make_link_idx_chain(tgt_link_status))
  {
    DBUG_PRINT("info",("spider can not create link_idx_chain"));
    delete fields;
    DBUG_RETURN(NULL);
  }

  /* choose link_id */
  if (fields->check_link_ok_chain())
  {
    DBUG_PRINT("info",("spider do not have link ok status"));
    delete fields;
    DBUG_RETURN(NULL);
  }

  fields->set_first_link_idx();

  if (!(group_by_handler = new spider_group_by_handler(thd, query, fields)))
  {
    DBUG_PRINT("info",("spider can't create group_by_handler"));
    delete fields;
    DBUG_RETURN(NULL);
  }
  query->distinct = FALSE;
  query->where = NULL;
  query->group_by = NULL;
  query->having = NULL;
  query->order_by = NULL;
  DBUG_RETURN(group_by_handler);
}
