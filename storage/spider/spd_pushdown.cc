#define MYSQL_SERVER 1
#include <my_global.h>
#include "table.h"
#include "sql_class.h"
#include "spd_db_include.h"
#include "spd_include.h"
#include "spd_malloc.h"
#include "ha_spider.h"
#include "spd_conn.h"
#include "spd_db_conn.h"
#include "partition_element.h"
#include "spd_table.h"
#include "spd_pushdown.h"
#include "spd_ping_table.h"

extern handlerton *spider_hton_ptr;

spider_fields::spider_fields() :
  dbton_count(0), current_dbton_num(0),
  table_count(0), table_holder(NULL),
  first_link_idx_chain(NULL), last_link_idx_chain(NULL), current_link_idx_chain(NULL),
  first_conn_holder(NULL), last_conn_holder(NULL), current_conn_holder(NULL)
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
    spider_malloc(spider_current_trx, SPD_MID_FIELDS_CREATE_LINK_IDX_CHAIN_1, sizeof(SPIDER_LINK_IDX_CHAIN),
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

    for (uint i= 0; i < table_count; i++)
    {
      link_idx_holder = get_next_table_on_link_idx_chain(link_idx_chain);
      spider = table_holder[i].spider;
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
    spider_malloc(spider_current_trx, SPD_MID_FIELDS_CREATE_LINK_IDX_HOLDER_1, sizeof(SPIDER_LINK_IDX_HOLDER),
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
    spider_bulk_malloc(spider_current_trx, SPD_MID_FIELDS_CREATE_CONN_HOLDER_1, MYF(MY_WME | MY_ZEROFILL),
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

bool spider_fields::has_conn_holder(
) {
  DBUG_ENTER("spider_fields::has_conn_holder");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_RETURN(first_conn_holder);
}

/* Mark checked_for_same_conn to false for all conn holders */
void spider_fields::clear_conn_holder_checked(
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

/* Set current conn holder to be the first conn holder with a matching
  conn and mark its checked_for_same_conn to be true. Return true if
  one is found and vice versa. */
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

/* Remove all conn holders with false checked_for_same_conn */
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
  if (conn_holder_arg->conn)
    conn_holder_arg->conn->conn_holder_for_direct_join = NULL;
  DBUG_PRINT("info",("spider free conn_holder=%p", conn_holder_arg));
  spider_free(spider_current_trx, conn_holder_arg, MYF(0));
  DBUG_VOID_RETURN;
}

/* Add the table associated with an ha_spider to a table_holder.
Return the table_holder. */
SPIDER_TABLE_HOLDER *spider_add_table_holder(
  ha_spider *spider_arg,
  SPIDER_TABLE_HOLDER *table_holder
) {
  spider_string *str;
  uint length;
  char tmp_buf[SPIDER_SQL_INT_LEN + 2];
  SPIDER_TABLE_HOLDER *return_table_holder;
  DBUG_ENTER("spider_fields::add_table");
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

  DBUG_RETURN(return_table_holder);
}

/* Return the table that field belongs to, or NULL if none exists. */
SPIDER_TABLE_HOLDER *spider_fields::find_table(Field *field)
{
  for (uint i = 0; i < table_count; i++)
    if (field->table == table_holder[i].table)
      return &table_holder[i];
  return NULL;
}

void spider_fields::set_table_holder(SPIDER_TABLE_HOLDER *table_holder_arg,
                                     uint table_count_arg)
{
  table_holder= table_holder_arg;
  table_count= table_count_arg;
}

/* Allocate space for table_count_arg table holders. */
SPIDER_TABLE_HOLDER *spider_create_table_holder(
  uint table_count_arg
) {
  SPIDER_TABLE_HOLDER* table_holder;
  DBUG_ENTER("spider_create_table_holder");
  if (table_count_arg == 0)
    DBUG_RETURN(0);
  table_holder = (SPIDER_TABLE_HOLDER *)
    spider_malloc(spider_current_trx, SPD_MID_CREATE_TABLE_HOLDER_1,
    table_count_arg * sizeof(SPIDER_TABLE_HOLDER),
    MYF(MY_WME | MY_ZEROFILL));
  DBUG_RETURN(table_holder);
}

/* Return pointer to the first table holder. */
SPIDER_TABLE_HOLDER *spider_fields::get_first_table_holder()
{
  DBUG_ENTER("spider_fields::get_first_spider");
  DBUG_RETURN(table_holder);
}

/* Return the first table holder associated with a given table, or
NULL if not found. */
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

void spider_fields::set_field_ptr(
  Field **field_arg
) {
  DBUG_ENTER("spider_fields::set_field_ptr");
  DBUG_PRINT("info",("spider this=%p", this));
  DBUG_PRINT("info",("spider field_ptr=%p", field_arg));
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
  SPIDER_LINK_IDX_HOLDER *link_idx_holder;
  DBUG_ENTER("spider_fields::ping_table_mon_from_table");
  set_pos_to_first_table_on_link_idx_chain(link_idx_chain);
  for (uint i= 0; i < table_count; i++)
  {
    link_idx_holder = get_next_table_on_link_idx_chain(link_idx_chain);
    tmp_spider = table_holder[i].spider;
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

int spider_make_query(const Query& query, spider_fields* fields,
                      ha_spider *spider, TABLE *table)
{
  uint dbton_id;
  spider_db_handler* dbton_hdl;
  SPIDER_RESULT_LIST *result_list = &spider->result_list;
  int error_num;
  DBUG_ENTER("spider_make_query");

  fields->set_pos_to_first_dbton_id();
  while ((dbton_id = fields->get_next_dbton_id()) < SPIDER_DBTON_SIZE)
  {
    dbton_hdl = spider->dbton_handler[dbton_id];
    result_list->direct_distinct = query.distinct;
    if ((error_num = dbton_hdl->reset_sql(SPIDER_SQL_TYPE_SELECT_SQL)))
      DBUG_RETURN(error_num);
    if ((error_num = dbton_hdl->append_select_part(SPIDER_SQL_TYPE_SELECT_SQL)))
      DBUG_RETURN(error_num);
    fields->set_field_ptr(table->field);
    if ((error_num = dbton_hdl->append_list_item_select_part(
           query.select, NULL, 0, TRUE, fields, SPIDER_SQL_TYPE_SELECT_SQL,
           query.n_aux)))
      DBUG_RETURN(error_num);
    if ((error_num = dbton_hdl->append_from_and_tables_part(
           fields, SPIDER_SQL_TYPE_SELECT_SQL)))
      DBUG_RETURN(error_num);
    if (query.where)
    {
      if ((error_num =
           dbton_hdl->append_where_part(SPIDER_SQL_TYPE_SELECT_SQL)))
        DBUG_RETURN(error_num);
      if ((error_num = dbton_hdl->append_item_type_part(
             query.where, NULL, 0, TRUE, fields, SPIDER_SQL_TYPE_SELECT_SQL)))
        DBUG_RETURN(error_num);
    }
    if (query.group_by)
    {
      if ((error_num = dbton_hdl->append_group_by_part(
             query.group_by, NULL, 0, TRUE, fields, SPIDER_SQL_TYPE_SELECT_SQL)))
        DBUG_RETURN(error_num);
    }
    if (query.having)
    {
      if ((error_num =
           dbton_hdl->append_having_part(SPIDER_SQL_TYPE_SELECT_SQL)))
        DBUG_RETURN(error_num);
      if ((error_num = dbton_hdl->append_item_type_part(
             query.having, NULL, 0, TRUE, fields, SPIDER_SQL_TYPE_SELECT_SQL)))
        DBUG_RETURN(error_num);
    }
    if (query.order_by)
    {
      if ((error_num = dbton_hdl->append_order_by_part(
             query.order_by, NULL, 0, TRUE, fields,
             SPIDER_SQL_TYPE_SELECT_SQL)))
        DBUG_RETURN(error_num);
    }
    if ((error_num = dbton_hdl->append_limit_part(
           result_list->internal_offset, result_list->limit_num,
           SPIDER_SQL_TYPE_SELECT_SQL)))
      DBUG_RETURN(error_num);
    if ((error_num = dbton_hdl->append_select_lock_part(
           SPIDER_SQL_TYPE_SELECT_SQL)))
      DBUG_RETURN(error_num);
  }
  DBUG_RETURN(0);
}
