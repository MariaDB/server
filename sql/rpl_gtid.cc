/* Copyright (c) 2013, Kristian Nielsen and MariaDB Services Ab.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */


/* Definitions for MariaDB global transaction ID (GTID). */


#include "sql_priv.h"
#include "my_sys.h"
#include "unireg.h"
#include "my_global.h"
#include "sql_base.h"
#include "sql_parse.h"
#include "key.h"
#include "rpl_gtid.h"
#include "rpl_rli.h"


const LEX_STRING rpl_gtid_slave_state_table_name=
  { C_STRING_WITH_LEN("gtid_slave_pos") };


void
rpl_slave_state::update_state_hash(uint64 sub_id, rpl_gtid *gtid)
{
  int err;
  /*
    Add the gtid to the HASH in the replication slave state.

    We must do this only _after_ commit, so that for parallel replication,
    there will not be an attempt to delete the corresponding table row before
    it is even committed.
  */
  lock();
  err= update(gtid->domain_id, gtid->server_id, sub_id, gtid->seq_no);
  unlock();
  if (err)
  {
    sql_print_warning("Slave: Out of memory during slave state maintenance. "
                      "Some no longer necessary rows in table "
                      "mysql.%s may be left undeleted.",
                      rpl_gtid_slave_state_table_name.str);
    /*
      Such failure is not fatal. We will fail to delete the row for this
      GTID, but it will do no harm and will be removed automatically on next
      server restart.
    */
  }
}


int
rpl_slave_state::record_and_update_gtid(THD *thd, Relay_log_info *rli)
{
  uint64 sub_id;

  /*
    Update the GTID position, if we have it and did not already update
    it in a GTID transaction.
  */
  if ((sub_id= rli->gtid_sub_id))
  {
    rli->gtid_sub_id= 0;
    if (record_gtid(thd, &rli->current_gtid, sub_id, false, false))
      return 1;
    update_state_hash(sub_id, &rli->current_gtid);
  }
  return 0;
}


rpl_slave_state::rpl_slave_state()
  : inited(false), loaded(false)
{
  my_hash_init(&hash, &my_charset_bin, 32, offsetof(element, domain_id),
               sizeof(uint32), NULL, my_free, HASH_UNIQUE);
}


rpl_slave_state::~rpl_slave_state()
{
}


void
rpl_slave_state::init()
{
  DBUG_ASSERT(!inited);
  mysql_mutex_init(key_LOCK_slave_state, &LOCK_slave_state, MY_MUTEX_INIT_SLOW);
  inited= true;
}


void
rpl_slave_state::truncate_hash()
{
  uint32 i;

  for (i= 0; i < hash.records; ++i)
  {
    element *e= (element *)my_hash_element(&hash, i);
    list_element *l= e->list;
    list_element *next;
    while (l)
    {
      next= l->next;
      my_free(l);
      l= next;
    }
    /* The element itself is freed by the hash element free function. */
  }
  my_hash_reset(&hash);
}

void
rpl_slave_state::deinit()
{
  if (!inited)
    return;
  truncate_hash();
  my_hash_free(&hash);
  mysql_mutex_destroy(&LOCK_slave_state);
}


int
rpl_slave_state::update(uint32 domain_id, uint32 server_id, uint64 sub_id,
                        uint64 seq_no)
{
  element *elem= NULL;
  list_element *list_elem= NULL;

  if (!(elem= get_element(domain_id)))
    return 1;

  if (!(list_elem= (list_element *)my_malloc(sizeof(*list_elem), MYF(MY_WME))))
    return 1;
  list_elem->server_id= server_id;
  list_elem->sub_id= sub_id;
  list_elem->seq_no= seq_no;

  elem->add(list_elem);
  return 0;
}


struct rpl_slave_state::element *
rpl_slave_state::get_element(uint32 domain_id)
{
  struct element *elem;

  elem= (element *)my_hash_search(&hash, (const uchar *)&domain_id, 0);
  if (elem)
    return elem;

  if (!(elem= (element *)my_malloc(sizeof(*elem), MYF(MY_WME))))
    return NULL;
  elem->list= NULL;
  elem->last_sub_id= 0;
  elem->domain_id= domain_id;
  if (my_hash_insert(&hash, (uchar *)elem))
  {
    my_free(elem);
    return NULL;
  }
  return elem;
}


int
rpl_slave_state::put_back_list(uint32 domain_id, list_element *list)
{
  element *e;
  if (!(e= (element *)my_hash_search(&hash, (const uchar *)&domain_id, 0)))
    return 1;
  while (list)
  {
    list_element *next= list->next;
    e->add(list);
    list= next;
  }
  return 0;
}


int
rpl_slave_state::truncate_state_table(THD *thd)
{
  TABLE_LIST tlist;
  int err= 0;
  TABLE *table;

  tlist.init_one_table(STRING_WITH_LEN("mysql"),
                       rpl_gtid_slave_state_table_name.str,
                       rpl_gtid_slave_state_table_name.length,
                       NULL, TL_WRITE);
  if (!(err= open_and_lock_tables(thd, &tlist, FALSE, 0)))
  {
    table= tlist.table;
    table->no_replicate= 1;
    err= table->file->ha_truncate();

    if (err)
    {
      ha_rollback_trans(thd, FALSE);
      close_thread_tables(thd);
      ha_rollback_trans(thd, TRUE);
    }
    else
    {
      ha_commit_trans(thd, FALSE);
      close_thread_tables(thd);
      ha_commit_trans(thd, TRUE);
    }
    thd->mdl_context.release_transactional_locks();
  }

  return err;
}


static const TABLE_FIELD_TYPE mysql_rpl_slave_state_coltypes[4]= {
  { { C_STRING_WITH_LEN("domain_id") },
    { C_STRING_WITH_LEN("int(10) unsigned") },
    {NULL, 0} },
  { { C_STRING_WITH_LEN("sub_id") },
    { C_STRING_WITH_LEN("bigint(20) unsigned") },
    {NULL, 0} },
  { { C_STRING_WITH_LEN("server_id") },
    { C_STRING_WITH_LEN("int(10) unsigned") },
    {NULL, 0} },
  { { C_STRING_WITH_LEN("seq_no") },
    { C_STRING_WITH_LEN("bigint(20) unsigned") },
    {NULL, 0} },
};

static const uint mysql_rpl_slave_state_pk_parts[]= {0, 1};

static const TABLE_FIELD_DEF mysql_gtid_slave_pos_tabledef= {
  array_elements(mysql_rpl_slave_state_coltypes),
  mysql_rpl_slave_state_coltypes,
  array_elements(mysql_rpl_slave_state_pk_parts),
  mysql_rpl_slave_state_pk_parts
};

class Gtid_db_intact : public Table_check_intact
{
protected:
  void report_error(uint, const char *fmt, ...)
  {
    va_list args;
    va_start(args, fmt);
    error_log_print(ERROR_LEVEL, fmt, args);
    va_end(args);
  }
};

static Gtid_db_intact gtid_table_intact;

/*
  Check that the mysql.gtid_slave_pos table has the correct definition.
*/
int
gtid_check_rpl_slave_state_table(TABLE *table)
{
  int err;

  if ((err= gtid_table_intact.check(table, &mysql_gtid_slave_pos_tabledef)))
    my_error(ER_GTID_OPEN_TABLE_FAILED, MYF(0), "mysql",
             rpl_gtid_slave_state_table_name.str);
  return err;
}


/*
  Write a gtid to the replication slave state table.

  Do it as part of the transaction, to get slave crash safety, or as a separate
  transaction if !in_transaction (eg. MyISAM or DDL).

    gtid    The global transaction id for this event group.
    sub_id  Value allocated within the sub_id when the event group was
            read (sub_id must be consistent with commit order in master binlog).

  Note that caller must later ensure that the new gtid and sub_id is inserted
  into the appropriate HASH element with rpl_slave_state.add(), so that it can
  be deleted later. But this must only be done after COMMIT if in transaction.
*/
int
rpl_slave_state::record_gtid(THD *thd, const rpl_gtid *gtid, uint64 sub_id,
                             bool in_transaction, bool in_statement)
{
  TABLE_LIST tlist;
  int err= 0;
  bool table_opened= false;
  TABLE *table;
  list_element *elist= 0, *next;
  element *elem;
  ulonglong thd_saved_option= thd->variables.option_bits;
  Query_tables_list lex_backup;

  if (unlikely(!loaded))
  {
    /*
      Probably the mysql.gtid_slave_pos table is missing (eg. upgrade) or
      corrupt.

      We already complained loudly about this, but we can try to continue
      until the DBA fixes it.
    */
    return 0;
  }

  if (!in_statement)
    mysql_reset_thd_for_next_command(thd, 0);

  DBUG_EXECUTE_IF("gtid_inject_record_gtid",
                  {
                    my_error(ER_CANNOT_UPDATE_GTID_STATE, MYF(0));
                    return 1;
                  } );

  thd->lex->reset_n_backup_query_tables_list(&lex_backup);
  tlist.init_one_table(STRING_WITH_LEN("mysql"),
                       rpl_gtid_slave_state_table_name.str,
                       rpl_gtid_slave_state_table_name.length,
                       NULL, TL_WRITE);
  if ((err= open_and_lock_tables(thd, &tlist, FALSE, 0)))
    goto end;
  table_opened= true;
  table= tlist.table;

  if ((err= gtid_check_rpl_slave_state_table(table)))
    goto end;

  table->no_replicate= 1;
  if (!in_transaction)
    thd->variables.option_bits&=
      ~(ulonglong)(OPTION_NOT_AUTOCOMMIT|OPTION_BEGIN);

  bitmap_set_all(table->write_set);

  table->field[0]->store((ulonglong)gtid->domain_id, true);
  table->field[1]->store(sub_id, true);
  table->field[2]->store((ulonglong)gtid->server_id, true);
  table->field[3]->store(gtid->seq_no, true);
  DBUG_EXECUTE_IF("inject_crash_before_write_rpl_slave_state", DBUG_SUICIDE(););
  if ((err= table->file->ha_write_row(table->record[0])))
  {
    table->file->print_error(err, MYF(0));
    goto end;
  }

  lock();
  if ((elem= get_element(gtid->domain_id)) == NULL)
  {
    unlock();
    my_error(ER_OUT_OF_RESOURCES, MYF(0));
    err= 1;
    goto end;
  }
  elist= elem->grab_list();
  unlock();

  if (!elist)
    goto end;

  /* Now delete any already committed rows. */
  bitmap_set_bit(table->read_set, table->field[0]->field_index);
  bitmap_set_bit(table->read_set, table->field[1]->field_index);

  if ((err= table->file->ha_index_init(0, 0)))
  {
    table->file->print_error(err, MYF(0));
    goto end;
  }
  while (elist)
  {
    uchar key_buffer[4+8];

    DBUG_EXECUTE_IF("gtid_slave_pos_simulate_failed_delete",
                    { err= ENOENT;
                      table->file->print_error(err, MYF(0));
                      /* `break' does not work in DBUG_EXECUTE_IF */
                      goto dbug_break; });

    next= elist->next;

    table->field[1]->store(elist->sub_id, true);
    /* domain_id is already set in table->record[0] from write_row() above. */
    key_copy(key_buffer, table->record[0], &table->key_info[0], 0, false);
    if (table->file->ha_index_read_map(table->record[1], key_buffer,
                                       HA_WHOLE_KEY, HA_READ_KEY_EXACT))
      /* We cannot find the row, assume it is already deleted. */
      ;
    else if ((err= table->file->ha_delete_row(table->record[1])))
      table->file->print_error(err, MYF(0));
    /*
      In case of error, we still discard the element from the list. We do
      not want to endlessly error on the same element in case of table
      corruption or such.
    */
    my_free(elist);
    elist= next;
    if (err)
      break;
  }
IF_DBUG(dbug_break:, )
  table->file->ha_index_end();

  if(!err && opt_bin_log &&
     (err= mysql_bin_log.bump_seq_no_counter_if_needed(gtid->domain_id,
                                                       gtid->seq_no)))
    my_error(ER_OUT_OF_RESOURCES, MYF(0));

end:

  if (table_opened)
  {
    if (err)
    {
      /*
        If error, we need to put any remaining elist back into the HASH so we
        can do another delete attempt later.
      */
      if (elist)
      {
        lock();
        put_back_list(gtid->domain_id, elist);
        unlock();
      }

      ha_rollback_trans(thd, FALSE);
      close_thread_tables(thd);
    }
    else
    {
      ha_commit_trans(thd, FALSE);
      close_thread_tables(thd);
    }
    if (in_transaction)
      thd->mdl_context.release_statement_locks();
    else
      thd->mdl_context.release_transactional_locks();
  }
  thd->lex->restore_backup_query_tables_list(&lex_backup);
  thd->variables.option_bits= thd_saved_option;
  return err;
}


uint64
rpl_slave_state::next_sub_id(uint32 domain_id)
{
  uint64 sub_id= 0;
  element *elem;

  lock();
  elem= get_element(domain_id);
  if (elem)
    sub_id= ++elem->last_sub_id;
  unlock();

  return sub_id;
}


bool
rpl_slave_state_tostring_helper(String *dest, const rpl_gtid *gtid, bool *first)
{
  if (*first)
    *first= false;
  else
    if (dest->append(",",1))
      return true;
  return
    dest->append_ulonglong(gtid->domain_id) ||
    dest->append("-",1) ||
    dest->append_ulonglong(gtid->server_id) ||
    dest->append("-",1) ||
    dest->append_ulonglong(gtid->seq_no);
}


int
rpl_slave_state::iterate(int (*cb)(rpl_gtid *, void *), void *data,
                         rpl_gtid *extra_gtids, uint32 num_extra)
{
  uint32 i;
  HASH gtid_hash;
  uchar *rec;
  rpl_gtid *gtid;
  int res= 1;

  my_hash_init(&gtid_hash, &my_charset_bin, 32, offsetof(rpl_gtid, domain_id),
               sizeof(uint32), NULL, NULL, HASH_UNIQUE);
  for (i= 0; i < num_extra; ++i)
    if (extra_gtids[i].server_id == global_system_variables.server_id &&
        my_hash_insert(&gtid_hash, (uchar *)(&extra_gtids[i])))
      goto err;

  lock();

  for (i= 0; i < hash.records; ++i)
  {
    uint64 best_sub_id;
    rpl_gtid best_gtid;
    element *e= (element *)my_hash_element(&hash, i);
    list_element *l= e->list;

    if (!l)
      continue;                                 /* Nothing here */

    best_gtid.domain_id= e->domain_id;
    best_gtid.server_id= l->server_id;
    best_gtid.seq_no= l->seq_no;
    best_sub_id= l->sub_id;
    while ((l= l->next))
    {
      if (l->sub_id > best_sub_id)
      {
        best_sub_id= l->sub_id;
        best_gtid.server_id= l->server_id;
        best_gtid.seq_no= l->seq_no;
      }
    }

    /* Check if we have something newer in the extra list. */
    rec= my_hash_search(&gtid_hash, (const uchar *)&best_gtid.domain_id, 0);
    if (rec)
    {
      gtid= (rpl_gtid *)rec;
      if (gtid->seq_no > best_gtid.seq_no)
        memcpy(&best_gtid, gtid, sizeof(best_gtid));
      if (my_hash_delete(&gtid_hash, rec))
      {
        unlock();
        goto err;
      }
    }

    if ((res= (*cb)(&best_gtid, data)))
    {
      unlock();
      goto err;
    }
  }

  unlock();

  /* Also add any remaining extra domain_ids. */
  for (i= 0; i < gtid_hash.records; ++i)
  {
    gtid= (rpl_gtid *)my_hash_element(&gtid_hash, i);
    if ((res= (*cb)(gtid, data)))
      goto err;
  }

  res= 0;

err:
  my_hash_free(&gtid_hash);

  return res;
}


struct rpl_slave_state_tostring_data {
  String *dest;
  bool first;
};
static int
rpl_slave_state_tostring_cb(rpl_gtid *gtid, void *data)
{
  rpl_slave_state_tostring_data *p= (rpl_slave_state_tostring_data *)data;
  return rpl_slave_state_tostring_helper(p->dest, gtid, &p->first);
}


/*
  Prepare the current slave state as a string, suitable for sending to the
  master to request to receive binlog events starting from that GTID state.

  The state consists of the most recently applied GTID for each domain_id,
  ie. the one with the highest sub_id within each domain_id.

  Optinally, extra_gtids is a list of GTIDs from the binlog. This is used when
  a server was previously a master and now needs to connect to a new master as
  a slave. For each domain_id, if the GTID in the binlog was logged with our
  own server_id _and_ has a higher seq_no than what is in the slave state,
  then this should be used as the position to start replicating at. This
  allows to promote a slave as new master, and connect the old master as a
  slave with MASTER_GTID_POS=AUTO.
*/
int
rpl_slave_state::tostring(String *dest, rpl_gtid *extra_gtids, uint32 num_extra)
{
  struct rpl_slave_state_tostring_data data;
  data.first= true;
  data.dest= dest;

  return iterate(rpl_slave_state_tostring_cb, &data, extra_gtids, num_extra);
}


/*
  Lookup a domain_id in the current replication slave state.

  Returns false if the domain_id has no entries in the slave state.
  Otherwise returns true, and fills in out_gtid with the corresponding
  GTID.
*/
bool
rpl_slave_state::domain_to_gtid(uint32 domain_id, rpl_gtid *out_gtid)
{
  element *elem;
  list_element *list;
  uint64 best_sub_id;

  lock();
  elem= (element *)my_hash_search(&hash, (const uchar *)&domain_id, 0);
  if (!elem || !(list= elem->list))
  {
    unlock();
    return false;
  }

  out_gtid->domain_id= domain_id;
  out_gtid->server_id= list->server_id;
  out_gtid->seq_no= list->seq_no;
  best_sub_id= list->sub_id;

  while ((list= list->next))
  {
    if (best_sub_id > list->sub_id)
      continue;
    best_sub_id= list->sub_id;
    out_gtid->server_id= list->server_id;
    out_gtid->seq_no= list->seq_no;
  }

  unlock();
  return true;
}


/*
  Parse a GTID at the start of a string, and update the pointer to point
  at the first character after the parsed GTID.

  Returns 0 on ok, non-zero on parse error.
*/
static int
gtid_parser_helper(char **ptr, char *end, rpl_gtid *out_gtid)
{
  char *q;
  char *p= *ptr;
  uint64 v1, v2, v3;
  int err= 0;

  q= end;
  v1= (uint64)my_strtoll10(p, &q, &err);
  if (err != 0 || v1 > (uint32)0xffffffff || q == end || *q != '-')
    return 1;
  p= q+1;
  q= end;
  v2= (uint64)my_strtoll10(p, &q, &err);
  if (err != 0 || v2 > (uint32)0xffffffff || q == end || *q != '-')
    return 1;
  p= q+1;
  q= end;
  v3= (uint64)my_strtoll10(p, &q, &err);
  if (err != 0)
    return 1;

  out_gtid->domain_id= v1;
  out_gtid->server_id= v2;
  out_gtid->seq_no= v3;
  *ptr= q;
  return 0;
}


/*
  Update the slave replication state with the GTID position obtained from
  master when connecting with old-style (filename,offset) position.

  If RESET is true then all existing entries are removed. Otherwise only
  domain_ids mentioned in the STATE_FROM_MASTER are changed.

  Returns 0 if ok, non-zero if error.
*/
int
rpl_slave_state::load(THD *thd, char *state_from_master, size_t len,
                      bool reset, bool in_statement)
{
  char *end= state_from_master + len;

  if (reset)
  {
    if (truncate_state_table(thd))
      return 1;
    truncate_hash();
  }
  if (state_from_master == end)
    return 0;
  for (;;)
  {
    rpl_gtid gtid;
    uint64 sub_id;

    if (gtid_parser_helper(&state_from_master, end, &gtid) ||
        !(sub_id= next_sub_id(gtid.domain_id)) ||
        record_gtid(thd, &gtid, sub_id, false, in_statement) ||
        update(gtid.domain_id, gtid.server_id, sub_id, gtid.seq_no))
      return 1;
    if (state_from_master == end)
      break;
    if (*state_from_master != ',')
      return 1;
    ++state_from_master;
  }
  return 0;
}


bool
rpl_slave_state::is_empty()
{
  uint32 i;
  bool result= true;

  lock();
  for (i= 0; i < hash.records; ++i)
  {
    element *e= (element *)my_hash_element(&hash, i);
    if (e->list)
    {
      result= false;
      break;
    }
  }
  unlock();

  return result;
}


rpl_binlog_state::rpl_binlog_state()
{
  my_hash_init(&hash, &my_charset_bin, 32, offsetof(element, domain_id),
               sizeof(uint32), NULL, my_free, HASH_UNIQUE);
  mysql_mutex_init(key_LOCK_binlog_state, &LOCK_binlog_state,
                   MY_MUTEX_INIT_SLOW);
  initialized= 1;
}


void
rpl_binlog_state::reset()
{
  uint32 i;

  for (i= 0; i < hash.records; ++i)
    my_hash_free(&((element *)my_hash_element(&hash, i))->hash);
  my_hash_reset(&hash);
}

void rpl_binlog_state::free()
{
  if (initialized)
  {
    initialized= 0;
    reset();
    my_hash_free(&hash);
    mysql_mutex_destroy(&LOCK_binlog_state);
  }
}


bool
rpl_binlog_state::load(struct rpl_gtid *list, uint32 count)
{
  uint32 i;

  reset();
  for (i= 0; i < count; ++i)
  {
    if (update(&(list[i]), false))
      return true;
  }
  return false;
}


rpl_binlog_state::~rpl_binlog_state()
{
  free();
}


/*
  Update replication state with a new GTID.

  If the (domain_id, server_id) pair already exists, then the new GTID replaces
  the old one for that domain id. Else a new entry is inserted.

  Returns 0 for ok, 1 for error.
*/
int
rpl_binlog_state::update(const struct rpl_gtid *gtid, bool strict)
{
  element *elem;

  if ((elem= (element *)my_hash_search(&hash,
                                       (const uchar *)(&gtid->domain_id), 0)))
  {
    if (strict && elem->last_gtid && elem->last_gtid->seq_no >= gtid->seq_no)
    {
      my_error(ER_GTID_STRICT_OUT_OF_ORDER, MYF(0), gtid->domain_id,
               gtid->server_id, gtid->seq_no, elem->last_gtid->domain_id,
               elem->last_gtid->server_id, elem->last_gtid->seq_no);
      return 1;
    }
    if (elem->seq_no_counter < gtid->seq_no)
      elem->seq_no_counter= gtid->seq_no;
    if (!elem->update_element(gtid))
      return 0;
  }
  else if (!alloc_element(gtid))
    return 0;

  my_error(ER_OUT_OF_RESOURCES, MYF(0));
  return 1;
}


/*
  Fill in a new GTID, allocating next sequence number, and update state
  accordingly.
*/
int
rpl_binlog_state::update_with_next_gtid(uint32 domain_id, uint32 server_id,
                                        rpl_gtid *gtid)
{
  element *elem;

  gtid->domain_id= domain_id;
  gtid->server_id= server_id;

  if ((elem= (element *)my_hash_search(&hash, (const uchar *)(&domain_id), 0)))
  {
    gtid->seq_no= ++elem->seq_no_counter;
    if (!elem->update_element(gtid))
      return 0;
  }
  else
  {
    gtid->seq_no= 1;
    if (!alloc_element(gtid))
      return 0;
  }

  my_error(ER_OUT_OF_RESOURCES, MYF(0));
  return 1;
}


/* Helper functions for update. */
int
rpl_binlog_state::element::update_element(const rpl_gtid *gtid)
{
  rpl_gtid *lookup_gtid;

  /*
    By far the most common case is that successive events within same
    replication domain have the same server id (it changes only when
    switching to a new master). So save a hash lookup in this case.
  */
  if (likely(last_gtid && last_gtid->server_id == gtid->server_id))
  {
    last_gtid->seq_no= gtid->seq_no;
    return 0;
  }

  lookup_gtid= (rpl_gtid *)
    my_hash_search(&hash, (const uchar *)&gtid->server_id, 0);
  if (lookup_gtid)
  {
    lookup_gtid->seq_no= gtid->seq_no;
    last_gtid= lookup_gtid;
    return 0;
  }

  /* Allocate a new GTID and insert it. */
  lookup_gtid= (rpl_gtid *)my_malloc(sizeof(*lookup_gtid), MYF(MY_WME));
  if (!lookup_gtid)
    return 1;
  memcpy(lookup_gtid, gtid, sizeof(*lookup_gtid));
  if (my_hash_insert(&hash, (const uchar *)lookup_gtid))
  {
    my_free(lookup_gtid);
    return 1;
  }
  last_gtid= lookup_gtid;
  return 0;
}


int
rpl_binlog_state::alloc_element(const rpl_gtid *gtid)
{
  element *elem;
  rpl_gtid *lookup_gtid;

  /* First time we see this domain_id; allocate a new element. */
  elem= (element *)my_malloc(sizeof(*elem), MYF(MY_WME));
  lookup_gtid= (rpl_gtid *)my_malloc(sizeof(*lookup_gtid), MYF(MY_WME));
  if (elem && lookup_gtid)
  {
    elem->domain_id= gtid->domain_id;
    my_hash_init(&elem->hash, &my_charset_bin, 32,
                 offsetof(rpl_gtid, server_id), sizeof(uint32), NULL, my_free,
                 HASH_UNIQUE);
    elem->last_gtid= lookup_gtid;
    elem->seq_no_counter= gtid->seq_no;
    memcpy(lookup_gtid, gtid, sizeof(*lookup_gtid));
    if (0 == my_hash_insert(&elem->hash, (const uchar *)lookup_gtid))
    {
      lookup_gtid= NULL;                        /* Do not free. */
      if (0 == my_hash_insert(&hash, (const uchar *)elem))
        return 0;
    }
    my_hash_free(&elem->hash);
  }

  /* An error. */
  if (elem)
    my_free(elem);
  if (lookup_gtid)
    my_free(lookup_gtid);
  return 1;
}


/*
  Check that a new GTID can be logged without creating an out-of-order
  sequence number with existing GTIDs.
*/
bool
rpl_binlog_state::check_strict_sequence(uint32 domain_id, uint32 server_id,
                                        uint64 seq_no)
{
  element *elem;

  if ((elem= (element *)my_hash_search(&hash,
                                       (const uchar *)(&domain_id), 0)) &&
      elem->last_gtid && elem->last_gtid->seq_no >= seq_no)
  {
    my_error(ER_GTID_STRICT_OUT_OF_ORDER, MYF(0), domain_id, server_id, seq_no,
             elem->last_gtid->domain_id, elem->last_gtid->server_id,
             elem->last_gtid->seq_no);
    return 1;
  }
  return 0;
}


/*
  When we see a new GTID that will not be binlogged (eg. slave thread
  with --log-slave-updates=0), then we need to remember to allocate any
  GTID seq_no of our own within that domain starting from there.

  Returns 0 if ok, non-zero if out-of-memory.
*/
int
rpl_binlog_state::bump_seq_no_if_needed(uint32 domain_id, uint64 seq_no)
{
  element *elem;

  if ((elem= (element *)my_hash_search(&hash, (const uchar *)(&domain_id), 0)))
  {
    if (elem->seq_no_counter < seq_no)
      elem->seq_no_counter= seq_no;
    return 0;
  }

  /* We need to allocate a new, empty element to remember the next seq_no. */
  if (!(elem= (element *)my_malloc(sizeof(*elem), MYF(MY_WME))))
    return 1;

  elem->domain_id= domain_id;
  my_hash_init(&elem->hash, &my_charset_bin, 32,
               offsetof(rpl_gtid, server_id), sizeof(uint32), NULL, my_free,
               HASH_UNIQUE);
  elem->last_gtid= NULL;
  elem->seq_no_counter= seq_no;
  if (0 == my_hash_insert(&hash, (const uchar *)elem))
    return 0;

  my_hash_free(&elem->hash);
  my_free(elem);
  return 1;
}


/*
  Write binlog state to text file, so we can read it in again without having
  to scan last binlog file (normal shutdown/startup, not crash recovery).

  The most recent GTID within each domain_id is written after any other GTID
  within this domain.
*/
int
rpl_binlog_state::write_to_iocache(IO_CACHE *dest)
{
  ulong i, j;
  char buf[21];

  for (i= 0; i < hash.records; ++i)
  {
    size_t res;
    element *e= (element *)my_hash_element(&hash, i);
    if (!e->last_gtid)
    {
      DBUG_ASSERT(e->hash.records == 0);
      continue;
    }
    for (j= 0; j <= e->hash.records; ++j)
    {
      const rpl_gtid *gtid;
      if (j < e->hash.records)
      {
        gtid= (const rpl_gtid *)my_hash_element(&e->hash, j);
        if (gtid == e->last_gtid)
          continue;
      }
      else
        gtid= e->last_gtid;

      longlong10_to_str(gtid->seq_no, buf, 10);
      res= my_b_printf(dest, "%u-%u-%s\n", gtid->domain_id, gtid->server_id, buf);
      if (res == (size_t) -1)
        return 1;
    }
  }

  return 0;
}


int
rpl_binlog_state::read_from_iocache(IO_CACHE *src)
{
  /* 10-digit - 10-digit - 20-digit \n \0 */
  char buf[10+1+10+1+20+1+1];
  char *p, *end;
  rpl_gtid gtid;

  reset();
  for (;;)
  {
    size_t res= my_b_gets(src, buf, sizeof(buf));
    if (!res)
      break;
    p= buf;
    end= buf + res;
    if (gtid_parser_helper(&p, end, &gtid))
      return 1;
    if (update(&gtid, false))
      return 1;
  }
  return 0;
}


rpl_gtid *
rpl_binlog_state::find(uint32 domain_id, uint32 server_id)
{
  element *elem;
  if (!(elem= (element *)my_hash_search(&hash, (const uchar *)&domain_id, 0)))
    return NULL;
  return (rpl_gtid *)my_hash_search(&elem->hash, (const uchar *)&server_id, 0);
}

rpl_gtid *
rpl_binlog_state::find_most_recent(uint32 domain_id)
{
  element *elem;

  elem= (element *)my_hash_search(&hash, (const uchar *)&domain_id, 0);
  if (elem && elem->last_gtid)
    return elem->last_gtid;
  return NULL;
}


uint32
rpl_binlog_state::count()
{
  uint32 c= 0;
  uint32 i;

  for (i= 0; i < hash.records; ++i)
    c+= ((element *)my_hash_element(&hash, i))->hash.records;

  return c;
}


int
rpl_binlog_state::get_gtid_list(rpl_gtid *gtid_list, uint32 list_size)
{
  uint32 i, j, pos;

  pos= 0;
  for (i= 0; i < hash.records; ++i)
  {
    element *e= (element *)my_hash_element(&hash, i);
    if (!e->last_gtid)
    {
      DBUG_ASSERT(e->hash.records==0);
      continue;
    }
    for (j= 0; j <= e->hash.records; ++j)
    {
      const rpl_gtid *gtid;
      if (j < e->hash.records)
      {
        gtid= (rpl_gtid *)my_hash_element(&e->hash, j);
        if (gtid == e->last_gtid)
          continue;
      }
      else
        gtid= e->last_gtid;

      if (pos >= list_size)
        return 1;
      memcpy(&gtid_list[pos++], gtid, sizeof(*gtid));
    }
  }

  return 0;
}


/*
  Get a list of the most recently binlogged GTID, for each domain_id.

  This can be used when switching from being a master to being a slave,
  to know where to start replicating from the new master.

  The returned list must be de-allocated with my_free().

  Returns 0 for ok, non-zero for out-of-memory.
*/
int
rpl_binlog_state::get_most_recent_gtid_list(rpl_gtid **list, uint32 *size)
{
  uint32 i;
  uint32 alloc_size, out_size;

  alloc_size= hash.records;
  if (!(*list= (rpl_gtid *)my_malloc(alloc_size * sizeof(rpl_gtid),
                                     MYF(MY_WME))))
    return 1;
  out_size= 0;
  for (i= 0; i < alloc_size; ++i)
  {
    element *e= (element *)my_hash_element(&hash, i);
    if (!e->last_gtid)
      continue;
    memcpy(&((*list)[out_size++]), e->last_gtid, sizeof(rpl_gtid));
  }

  *size= out_size;
  return 0;
}


bool
rpl_binlog_state::append_pos(String *str)
{
  uint32 i;
  bool first= true;

  for (i= 0; i < hash.records; ++i)
  {
    element *e= (element *)my_hash_element(&hash, i);
    if (e->last_gtid &&
        rpl_slave_state_tostring_helper(str, e->last_gtid, &first))
      return true;
  }

  return false;
}


slave_connection_state::slave_connection_state()
{
  my_hash_init(&hash, &my_charset_bin, 32,
               offsetof(rpl_gtid, domain_id), sizeof(uint32), NULL, my_free,
               HASH_UNIQUE);
}


slave_connection_state::~slave_connection_state()
{
  my_hash_free(&hash);
}


/*
  Create a hash from the slave GTID state that is sent to master when slave
  connects to start replication.

  The state is sent as <GTID>,<GTID>,...,<GTID>, for example:

     0-2-112,1-4-1022

  The state gives for each domain_id the GTID to start replication from for
  the corresponding replication stream. So domain_id must be unique.

  Returns 0 if ok, non-zero if error due to malformed input.

  Note that input string is built by slave server, so it will not be incorrect
  unless bug/corruption/malicious server. So we just need basic sanity check,
  not fancy user-friendly error message.
*/

int
slave_connection_state::load(char *slave_request, size_t len)
{
  char *p, *end;
  uchar *rec;
  rpl_gtid *gtid;
  const rpl_gtid *gtid2;

  reset();
  p= slave_request;
  end= slave_request + len;
  if (p == end)
    return 0;
  for (;;)
  {
    if (!(rec= (uchar *)my_malloc(sizeof(*gtid), MYF(MY_WME))))
    {
      my_error(ER_OUTOFMEMORY, MYF(0), sizeof(*gtid));
      return 1;
    }
    gtid= (rpl_gtid *)rec;
    if (gtid_parser_helper(&p, end, gtid))
    {
      my_free(rec);
      my_error(ER_INCORRECT_GTID_STATE, MYF(0));
      return 1;
    }
    if ((gtid2= (const rpl_gtid *)
         my_hash_search(&hash, (const uchar *)(&gtid->domain_id), 0)))
    {
      my_error(ER_DUPLICATE_GTID_DOMAIN, MYF(0), gtid->domain_id,
               gtid->server_id, (ulonglong)gtid->seq_no, gtid2->domain_id,
               gtid2->server_id, (ulonglong)gtid2->seq_no, gtid->domain_id);
      my_free(rec);
      return 1;
    }
    if (my_hash_insert(&hash, rec))
    {
      my_free(rec);
      my_error(ER_OUT_OF_RESOURCES, MYF(0));
      return 1;
    }
    if (p == end)
      break;                                         /* Finished. */
    if (*p != ',')
    {
      my_error(ER_INCORRECT_GTID_STATE, MYF(0));
      return 1;
    }
    ++p;
  }

  return 0;
}


int
slave_connection_state::load(const rpl_gtid *gtid_list, uint32 count)
{
  uint32 i;

  reset();
  for (i= 0; i < count; ++i)
    if (update(&gtid_list[i]))
      return 1;
  return 0;
}


static int
slave_connection_state_load_cb(rpl_gtid *gtid, void *data)
{
  slave_connection_state *state= (slave_connection_state *)data;
  return state->update(gtid);
}


/*
  Same as rpl_slave_state::tostring(), but populates a slave_connection_state
  instead.
*/
int
slave_connection_state::load(rpl_slave_state *state,
                             rpl_gtid *extra_gtids, uint32 num_extra)
{
  reset();
  return state->iterate(slave_connection_state_load_cb, this,
                        extra_gtids, num_extra);
}


rpl_gtid *
slave_connection_state::find(uint32 domain_id)
{
  return (rpl_gtid *) my_hash_search(&hash, (const uchar *)(&domain_id), 0);
}


int
slave_connection_state::update(const rpl_gtid *in_gtid)
{
  rpl_gtid *new_gtid;
  uchar *rec= my_hash_search(&hash, (const uchar *)(&in_gtid->domain_id), 0);
  if (rec)
  {
    memcpy(rec, in_gtid, sizeof(*in_gtid));
    return 0;
  }

  if (!(new_gtid= (rpl_gtid *)my_malloc(sizeof(*new_gtid), MYF(MY_WME))))
    return 1;
  memcpy(new_gtid, in_gtid, sizeof(*new_gtid));
  if (my_hash_insert(&hash, (uchar *)new_gtid))
  {
    my_free(new_gtid);
    return 1;
  }

  return 0;
}


void
slave_connection_state::remove(const rpl_gtid *in_gtid)
{
  uchar *rec= my_hash_search(&hash, (const uchar *)(&in_gtid->domain_id), 0);
#ifndef DBUG_OFF
  bool err;
  rpl_gtid *slave_gtid= (rpl_gtid *)rec;
  DBUG_ASSERT(rec /* We should never try to remove not present domain_id. */);
  DBUG_ASSERT(slave_gtid->server_id == in_gtid->server_id);
  DBUG_ASSERT(slave_gtid->seq_no == in_gtid->seq_no);
#endif

  IF_DBUG(err=, )
    my_hash_delete(&hash, rec);
  DBUG_ASSERT(!err);
}


int
slave_connection_state::to_string(String *out_str)
{
  out_str->length(0);
  return append_to_string(out_str);
}


int
slave_connection_state::append_to_string(String *out_str)
{
  uint32 i;
  bool first;

  first= true;
  for (i= 0; i < hash.records; ++i)
  {
    const rpl_gtid *gtid= (const rpl_gtid *)my_hash_element(&hash, i);
    if (rpl_slave_state_tostring_helper(out_str, gtid, &first))
      return 1;
  }
  return 0;
}
