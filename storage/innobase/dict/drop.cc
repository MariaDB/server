/*****************************************************************************

Copyright (c) 2021, 2022, MariaDB Corporation.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA

*****************************************************************************/

/**
@file dict/drop.cc
Data Dictionary Language operations that delete .ibd files */

/* We implement atomic data dictionary operations as follows.

1. A data dictionary transaction is started.
2. We acquire exclusive lock on all the tables that are to be dropped
during the execution of the transaction.
3. We lock the data dictionary cache.
4. All metadata tables will be updated within the single DDL transaction,
including deleting or renaming InnoDB persistent statistics.
4b. If any lock wait would occur while we are holding the dict_sys latches,
we will instantly report a timeout error and roll back the transaction.
5. The transaction metadata is marked as committed.
6. If any files were deleted, we will durably write FILE_DELETE
to the redo log and start deleting the files.
6b. Also purge after a commit may perform file deletion. This is also the
recovery mechanism if the server was killed between step 5 and 6.
7. We unlock the data dictionary cache.
8. The file handles of the unlinked files will be closed. This will actually
reclaim the space in the file system (delete-on-close semantics).

Notes:

(a) Purge will be locked out by MDL. For internal tables related to
FULLTEXT INDEX, purge will not acquire MDL on the user table name,
and therefore, when we are dropping any FTS_ tables, we must suspend
and resume purge to prevent a race condition.

(b) If a transaction needs to both drop and create a table by some
name, it must rename the table in between. This is used by
ha_innobase::truncate() and fts_drop_common_tables().

(c) No data is ever destroyed before the transaction is committed,
so we can trivially roll back the transaction at any time.
Lock waits during a DDL operation are no longer a fatal error
that would cause the InnoDB to hang or to intentionally crash.
(Only ALTER TABLE...DISCARD TABLESPACE may discard data before commit.)

(d) The only changes to the data dictionary cache that are performed
before transaction commit and must be rolled back explicitly are as follows:
(d1) fts_optimize_add_table() to undo fts_optimize_remove_table()
*/

#include "trx0purge.h"
#include "dict0dict.h"
#include "dict0stats.h"
#include "dict0stats_bg.h"

#include "dict0defrag_bg.h"
#include "btr0defragment.h"
#include "lock0lock.h"

#include "que0que.h"
#include "pars0pars.h"

/** Try to drop the foreign key constraints for a persistent table.
@param name        name of persistent table
@return error code */
dberr_t trx_t::drop_table_foreign(const table_name_t &name)
{
  ut_ad(dict_sys.locked());
  ut_ad(state == TRX_STATE_ACTIVE);
  ut_ad(dict_operation);
  ut_ad(dict_operation_lock_mode);

  if (!dict_sys.sys_foreign || !dict_sys.sys_foreign_cols)
    return DB_SUCCESS;

  pars_info_t *info= pars_info_create();
  pars_info_add_str_literal(info, "name", name.m_name);
  return que_eval_sql(info,
                      "PROCEDURE DROP_FOREIGN() IS\n"
                      "fid CHAR;\n"

                      "DECLARE CURSOR fk IS\n"
                      "SELECT ID FROM SYS_FOREIGN\n"
                      "WHERE FOR_NAME=:name\n"
                      "AND TO_BINARY(FOR_NAME)=TO_BINARY(:name)\n"
                      "FOR UPDATE;\n"

                      "BEGIN\n"
                      "OPEN fk;\n"
                      "WHILE 1=1 LOOP\n"
                      "  FETCH fk INTO fid;\n"
                      "  IF (SQL % NOTFOUND)THEN RETURN;END IF;\n"
                      "  DELETE FROM SYS_FOREIGN_COLS"
                      " WHERE ID=fid;\n"
                      "  DELETE FROM SYS_FOREIGN WHERE ID=fid;\n"
                      "END LOOP;\n"
                      "CLOSE fk;\n"
                      "END;\n", this);
}

/** Try to drop the statistics for a persistent table.
@param name        name of persistent table
@return error code */
dberr_t trx_t::drop_table_statistics(const table_name_t &name)
{
  ut_ad(dict_sys.locked());
  ut_ad(dict_operation_lock_mode);

  if (strstr(name.m_name, "/" TEMP_FILE_PREFIX_INNODB) ||
      !strcmp(name.m_name, TABLE_STATS_NAME) ||
      !strcmp(name.m_name, INDEX_STATS_NAME))
    return DB_SUCCESS;

  char db[MAX_DB_UTF8_LEN], table[MAX_TABLE_UTF8_LEN];
  dict_fs2utf8(name.m_name, db, sizeof db, table, sizeof table);

  dberr_t err= dict_stats_delete_from_table_stats(db, table, this);
  if (err == DB_SUCCESS || err == DB_STATS_DO_NOT_EXIST)
  {
    err= dict_stats_delete_from_index_stats(db, table, this);
    if (err == DB_STATS_DO_NOT_EXIST)
      err= DB_SUCCESS;
  }
  return err;
}

/** Try to drop a persistent table.
@param table       persistent table
@param fk          whether to drop FOREIGN KEY metadata
@return error code */
dberr_t trx_t::drop_table(const dict_table_t &table)
{
  ut_ad(dict_sys.locked());
  ut_ad(state == TRX_STATE_ACTIVE);
  ut_ad(dict_operation);
  ut_ad(dict_operation_lock_mode);
  ut_ad(!table.is_temporary());
  /* The table must be exclusively locked by this transaction. */
  ut_ad(table.get_ref_count() <= 1);
  ut_ad(table.n_lock_x_or_s == 1);
  ut_ad(UT_LIST_GET_LEN(table.locks) >= 1);
#ifdef UNIV_DEBUG
  bool found_x= false;
  for (lock_t *lock= UT_LIST_GET_FIRST(table.locks); lock;
       lock= UT_LIST_GET_NEXT(un_member.tab_lock.locks, lock))
  {
    ut_ad(lock->trx == this);
    switch (lock->type_mode) {
    case LOCK_TABLE | LOCK_X:
      found_x= true;
      break;
    case LOCK_TABLE | LOCK_IX:
    case LOCK_TABLE | LOCK_AUTO_INC:
      break;
    default:
      ut_ad("unexpected lock type" == 0);
    }
  }
  ut_ad(found_x);
#endif

  if (dict_sys.sys_virtual)
  {
    pars_info_t *info= pars_info_create();
    pars_info_add_ull_literal(info, "id", table.id);
    if (dberr_t err= que_eval_sql(info,
                                  "PROCEDURE DROP_VIRTUAL() IS\n"
                                  "BEGIN\n"
                                  "DELETE FROM SYS_VIRTUAL"
                                  " WHERE TABLE_ID=:id;\n"
                                  "END;\n", this))
      return err;
  }

  /* Once DELETE FROM SYS_INDEXES is committed, purge may invoke
  dict_drop_index_tree(). */

  if (!(table.flags2 & (DICT_TF2_FTS_HAS_DOC_ID | DICT_TF2_FTS)));
  else if (dberr_t err= fts_drop_tables(this, table))
  {
    ib::error() << "Unable to remove FTS tables for "
                << table.name << ": " << err;
    return err;
  }

  mod_tables.emplace(const_cast<dict_table_t*>(&table), undo_no).
    first->second.set_dropped();

  pars_info_t *info= pars_info_create();
  pars_info_add_ull_literal(info, "id", table.id);
  return que_eval_sql(info,
                      "PROCEDURE DROP_TABLE() IS\n"
                      "iid CHAR;\n"

                      "DECLARE CURSOR idx IS\n"
                      "SELECT ID FROM SYS_INDEXES\n"
                      "WHERE TABLE_ID=:id FOR UPDATE;\n"

                      "BEGIN\n"

                      "DELETE FROM SYS_TABLES WHERE ID=:id;\n"
                      "DELETE FROM SYS_COLUMNS WHERE TABLE_ID=:id;\n"

                      "OPEN idx;\n"
                      "WHILE 1 = 1 LOOP\n"
                      "  FETCH idx INTO iid;\n"
                      "  IF (SQL % NOTFOUND) THEN EXIT; END IF;\n"
                      "  DELETE FROM SYS_INDEXES WHERE CURRENT OF idx;\n"
                      "  DELETE FROM SYS_FIELDS WHERE INDEX_ID=iid;\n"
                      "END LOOP;\n"
                      "CLOSE idx;\n"

                      "END;\n", this);
}

/** Commit the transaction, possibly after drop_table().
@param deleted   handles of data files that were deleted */
void trx_t::commit(std::vector<pfs_os_file_t> &deleted)
{
  ut_ad(dict_operation);
  commit_persist();
  if (dict_operation)
  {
    ut_ad(dict_sys.locked());
    lock_sys.wr_lock(SRW_LOCK_CALL);
    mutex_lock();
    lock_release_on_drop(this);
    ut_ad(UT_LIST_GET_LEN(lock.trx_locks) == 0);
    ut_ad(ib_vector_is_empty(autoinc_locks));
    mem_heap_empty(lock.lock_heap);
    lock.table_locks.clear();
    lock.was_chosen_as_deadlock_victim= false;
    lock.n_rec_locks= 0;
    while (dict_table_t *table= UT_LIST_GET_FIRST(lock.evicted_tables))
    {
      UT_LIST_REMOVE(lock.evicted_tables, table);
      dict_mem_table_free(table);
    }
    dict_operation= false;
    id= 0;
    mutex_unlock();

    for (const auto &p : mod_tables)
    {
      if (p.second.is_dropped())
      {
        dict_table_t *table= p.first;
        dict_stats_recalc_pool_del(table->id, true);
        dict_stats_defrag_pool_del(table, nullptr);
        if (btr_defragment_active)
          btr_defragment_remove_table(table);
        const fil_space_t *space= table->space;
        ut_ad(!strstr(table->name.m_name, "/FTS_") ||
              purge_sys.must_wait_FTS());
        dict_sys.remove(table);
        if (const auto id= space ? space->id : 0)
        {
          pfs_os_file_t d= fil_delete_tablespace(id);
          if (d != OS_FILE_CLOSED)
            deleted.emplace_back(d);
        }
      }
    }

    lock_sys.wr_unlock();

    mysql_mutex_lock(&lock_sys.wait_mutex);
    lock_sys.deadlock_check();
    mysql_mutex_unlock(&lock_sys.wait_mutex);
  }
  commit_cleanup();
}
