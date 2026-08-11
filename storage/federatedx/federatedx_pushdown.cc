/*
   Copyright (c) 2019, 2020, MariaDB

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

/* !!! For inclusion into ha_federatedx.cc */
/* For CR_MIN_ERROR/CR_MAX_ERROR, the error code range of the client library */
#include <errmsg.h>

/*
  This is a quick a dirty implemention of the derived_handler and select_handler
  interfaces to be used to push select queries and the queries specifying
  derived tables into FEDERATEDX engine.
  The functions
    create_federatedx_derived_handler,
    create_federatedx_select_handler, and
    create_federatedx_multi_upddel_handler
  that return the corresponding interfaces for pushdown capabilities do
  not check a lot of things. In particular they do not check that the tables
  of the pushed queries belong to the same foreign server.

  The implementation is provided purely for testing purposes.
  The pushdown capabilities are enabled by turning on the plugin system
  variable federated_pushdown:
    set global federated_pushdown=1;
*/

/*
  Check if table and database names are equal on local and remote servers

  SYNOPSIS
    local_and_remote_names_match()
    tbl_share      Pointer to current table TABLE_SHARE structure
    fshare         Pointer to current table FEDERATEDX_SHARE structure

  DESCRIPTION
    FederatedX table on the local server may refer to a table having another
    name on the remote server. The remote table may even reside in a different
    database. For example:

    -- Remote server
    CREATE TABLE t1 (id int(32));

    -- Local server
    CREATE TABLE t2 ENGINE="FEDERATEDX"
    CONNECTION="mysql://joe:joespass@192.168.1.111:9308/federatedx/t1";

    It's not a problem while the federated_pushdown is disabled 'cause
    the CONNECTION strings are being parsed for every table during
    the execution, so the table names are translated from local to remote.
    But in case of the federated_pushdown the whole query is pushed down
    to the engine without any translation, so the remote server may try
    to select data from a nonexistent table (for example, query
    "SELECT * FROM t2" will try to retrieve data from nonexistent "t2").

    This function checks whether there is a mismatch between local and remote
    table/database names

  RETURN VALUE
    false           names are equal
    true            names are not equal

*/
bool local_and_remote_names_mismatch(const TABLE_SHARE *tbl_share,
                                     const FEDERATEDX_SHARE *fshare)
{
  return !tbl_share->db.streq(Lex_cstring_strlen(fshare->database)) ||
         !tbl_share->table_name.streq(Lex_cstring_strlen(fshare->table_name));
}


/*
  Whether two FederatedX tables live on the same remote server.

  The whole statement is sent to a single remote connection, so all the
  tables it touches must be reachable through the same one. A connection is
  identified by its scheme, host, port, socket, and user; the remote
  database may differ from table to table (one connection can address several
  databases), so it is not compared here.
*/
static bool same_remote_server(const FEDERATEDX_SHARE *a,
                               const FEDERATEDX_SHARE *b)
{
  auto str_eq= [](const char *x, const char *y)
  { return (!x || !y) ? x == y : !strcmp(x, y); };

  return a->port == b->port &&
         str_eq(a->scheme, b->scheme) &&
         str_eq(a->hostname, b->hostname) &&
         str_eq(a->socket, b->socket) &&
         str_eq(a->username, b->username);
}


/*
  Check that all tables in the sel_lex use the FederatedX storage engine and
  live on the same remote server, and return one of them.

  @param sel_lex    the select to check
  @param ref_share  in/out: the share of the first FederatedX table seen so
                    far across the whole statement, every other table is
                    required to match its remote server. Must point to a
                    nullptr on the top-level call.

  @return
    One of the tables from sel_lex
*/
static TABLE *get_fed_table_for_pushdown(SELECT_LEX *sel_lex,
                                         const FEDERATEDX_SHARE **ref_share)
{
  TABLE *pushdown_table= nullptr;
  if (!sel_lex->join)
    return nullptr;
  for (TABLE_LIST *tbl= sel_lex->join->tables_list; tbl; tbl= tbl->next_local)
  {
    if (!tbl->table)
      return nullptr;
    if (tbl->derived)
    {
      /*
        Skip derived table for now as they will be checked
        in the subsequent loop
      */
      continue;
    }

    /*
      Check that all tables are FederatedX tables.
      We intentionally don't support partitioned federatedx tables here, so
      use file->ht and not file->partition_ht().
    */
    if (tbl->table->file->ht != federatedx_hton)
      return nullptr;
    const FEDERATEDX_SHARE *fshare=
        ((ha_federatedx *) tbl->table->file)->get_federatedx_share();
    /*
      We print the local (frontend) query and run it on the remote server.
      This only works if the table name on the remote server is the same.
    */
    if (local_and_remote_names_mismatch(tbl->table->s, fshare))
      return nullptr;

    /* All the tables of the statement must be on the same remote server */
    if (!*ref_share)
      *ref_share= fshare;
    else if (!same_remote_server(*ref_share, fshare))
      return nullptr;

    if (!pushdown_table)
      pushdown_table= tbl->table;
  }

  for (SELECT_LEX_UNIT *un= sel_lex->first_inner_unit(); un;
       un= un->next_unit())
  {
    for (SELECT_LEX *sl= un->first_select(); sl; sl= sl->next_select())
    {
      auto inner_tbl= get_fed_table_for_pushdown(sl, ref_share);
      if (!inner_tbl)
        return nullptr;
      if (!pushdown_table)
        pushdown_table= inner_tbl;
    }
  }
  return pushdown_table;
}


/*
  A wrapper for the top-level call, see get_fed_table_for_pushdown() above
*/
static TABLE *get_fed_table_for_pushdown(SELECT_LEX *sel_lex)
{
  const FEDERATEDX_SHARE *ref_share= nullptr;
  return get_fed_table_for_pushdown(sel_lex, &ref_share);
}


/*
  Check that all tables in the lex_unit use the FederatedX storage engine
  and return one of them

  @return
    One of the tables from lex_unit
*/
static TABLE *get_fed_table_for_unit_pushdown(SELECT_LEX_UNIT *lex_unit)
{
  TABLE *table= nullptr;
  const FEDERATEDX_SHARE *ref_share= nullptr;
  for (auto sel_lex= lex_unit->first_select(); sel_lex;
       sel_lex= sel_lex->next_select())
  {
    auto next_tbl= get_fed_table_for_pushdown(sel_lex, &ref_share);
    if (!next_tbl)
      return nullptr;
    if (!table)
      table= next_tbl;
  }
  return table;
}


static derived_handler*
create_federatedx_derived_handler(THD* thd, TABLE_LIST *derived)
{
  if (!use_pushdown)
    return 0;

  SELECT_LEX_UNIT *unit= derived->derived;

  auto tbl= get_fed_table_for_unit_pushdown(unit);
  if (!tbl)
    return nullptr;

  return new ha_federatedx_derived_handler(thd, derived, tbl);
}


/*
  Implementation class of the derived_handler interface for FEDERATEDX:
  class implementation
*/

ha_federatedx_derived_handler::ha_federatedx_derived_handler(THD *thd,
                                                             TABLE_LIST *dt,
                                                             TABLE *tbl)
  : derived_handler(thd, federatedx_hton),
    federatedx_handler_base(thd, tbl)
{
  derived= dt;

  query.length(0);
  dt->derived->print(&query,
                     enum_query_type(QT_VIEW_INTERNAL |
                                     QT_ITEM_ORIGINAL_FUNC_NULLIF |
                                     QT_PARSABLE));
}

ha_federatedx_derived_handler::~ha_federatedx_derived_handler() = default;

int federatedx_handler_base::end_scan_()
{
  DBUG_ENTER("ha_federatedx_derived_handler::end_scan");

  (*iop)->free_result(stored_result);

  free_share(txn, share);

  DBUG_RETURN(0);
}


static bool is_supported_by_select_handler(enum_sql_command sql_command)
{
  return sql_command == SQLCOM_SELECT || sql_command == SQLCOM_INSERT_SELECT;
}


/*
  Create FederatedX select handler for processing either a single select
  (in this case sel_lex is initialized and lex_unit==NULL)
  or a select that is part of a unit
  (in this case both sel_lex and lex_unit are initialized)
*/
static select_handler *
create_federatedx_select_handler(THD *thd, SELECT_LEX *sel_lex,
                                 SELECT_LEX_UNIT *lex_unit)
{
  if (!use_pushdown || !is_supported_by_select_handler(thd->lex->sql_command))
    return nullptr;

  if (lex_unit && sel_lex->master_unit()->with_clause)
    return nullptr;

  auto tbl= get_fed_table_for_pushdown(sel_lex);
  if (!tbl)
    return nullptr;

  if (sel_lex->uncacheable & UNCACHEABLE_SIDEEFFECT)
    return NULL;

  return new ha_federatedx_select_handler(thd, sel_lex, lex_unit, tbl);
}


/*
  Create FederatedX handler for processing a whole multi-table UPDATE/DELETE
*/
static multi_upddel_handler *
create_federatedx_multi_upddel_handler(THD *thd, LEX *lex)
{
  DBUG_ASSERT(lex->sql_command == SQLCOM_UPDATE ||
              lex->sql_command == SQLCOM_UPDATE_MULTI ||
              lex->sql_command == SQLCOM_DELETE ||
              lex->sql_command == SQLCOM_DELETE_MULTI);

  /* Is pushdown enabled by @@federatedx_use_pushdown? */
  if (!use_pushdown)
    return nullptr;

  /*
    SELECT_LEX::print() reproduces neither the IGNORE modifier nor the
    LOW_PRIORITY/QUICK ones. Of these only IGNORE changes the outcome of the
    statement: a pushed down UPDATE IGNORE/DELETE IGNORE would turn an error
    the remote server ignores into a real one, so such statements are executed
    locally. LOW_PRIORITY and QUICK only affect local locking and index
    housekeeping; they change neither the affected rows nor the errors raised,
    and the row-by-row path does not forward them to the remote server either,
    so dropping them here is harmless and needs no guard.
  */
  if (lex->ignore)
    return nullptr;

  SELECT_LEX *sel_lex= lex->first_select_lex();

  auto tbl= get_fed_table_for_pushdown(sel_lex);
  if (!tbl)
    return nullptr;

  if (sel_lex->uncacheable & UNCACHEABLE_SIDEEFFECT)
    return nullptr;

  return new ha_federatedx_multi_upddel_handler(thd, lex, tbl);
}

/*
  Create FederatedX select handler for processing a unit as a whole.
  Term "unit" stands for multiple SELECTs combined with
  UNION/EXCEPT/INTERSECT operators
*/
static select_handler *
create_federatedx_unit_handler(THD *thd, SELECT_LEX_UNIT *sel_unit)
{
  if (!use_pushdown)
    return nullptr;

  auto tbl= get_fed_table_for_unit_pushdown(sel_unit);
  if (!tbl)
    return nullptr;

  if (sel_unit->uncacheable & UNCACHEABLE_SIDEEFFECT)
    return nullptr;

  return new ha_federatedx_select_handler(thd, sel_unit, tbl);
}


/*
  Implementation class of the select_handler interface for FEDERATEDX:
  class implementation
*/

federatedx_handler_base::federatedx_handler_base(THD *thd_arg, TABLE *tbl_arg)
 : share(NULL), txn(NULL), iop(NULL), stored_result(NULL),
   query(thd_arg->charset()),
   query_table(tbl_arg)
{}

ha_federatedx_select_handler::~ha_federatedx_select_handler() = default;

ha_federatedx_select_handler::ha_federatedx_select_handler(
    THD *thd, SELECT_LEX_UNIT *lex_unit, TABLE *tbl)
  : select_handler(thd, federatedx_hton, lex_unit), 
    federatedx_handler_base(thd, tbl)
{
  query.length(0);
  lex_unit->print(&query, PRINT_QUERY_TYPE);
}

ha_federatedx_select_handler::ha_federatedx_select_handler(
    THD *thd, SELECT_LEX *select_lex, SELECT_LEX_UNIT *lex_unit, TABLE *tbl)
    : select_handler(thd, federatedx_hton, select_lex, lex_unit),
      federatedx_handler_base(thd, tbl)
{
  query.length(0);
  if (get_pushdown_type() == select_pushdown_type::SINGLE_SELECT)
  {
    /*
      Must use SELECT_LEX_UNIT::print() instead of SELECT_LEX::print() here
      to print possible CTEs which are stored at SELECT_LEX_UNIT::with_clause
    */
    select_lex->master_unit()->print(&query, PRINT_QUERY_TYPE);
  }
  else if (get_pushdown_type() == select_pushdown_type::PART_OF_UNIT)
  {
    /*
      CTEs are not supported for partial select pushdown so use
      SELECT_LEX::print() here
    */
    select_lex->print(thd, &query, PRINT_QUERY_TYPE);
  }
  else
  {
    /*
      Other select_pushdown_types are not allowed in this constructor.
      The case of select_pushdown_type::WHOLE_UNIT is handled at another
      overload of the constuctor
    */
    DBUG_ASSERT(0);
  }
}

/*
  Implementation class of the multi_upddel_handler interface for FEDERATEDX:
  class implementation
*/

ha_federatedx_multi_upddel_handler::ha_federatedx_multi_upddel_handler(
    THD *thd, LEX *lex_arg, TABLE *tbl)
    : multi_upddel_handler(thd, federatedx_hton, lex_arg),
      federatedx_handler_base(thd, tbl)
{
  query.length(0);
  /*
    Print the whole statement back. SELECT_LEX::print() produces
      update <join> set <assignments> where <cond>
    for an UPDATE and
      delete from <targets> using <join> where <cond>
    for a DELETE, both of which the remote server understands.

    Must go through SELECT_LEX_UNIT::print() rather than call
    SELECT_LEX::print() directly, because a possible WITH clause is stored at
    SELECT_LEX_UNIT::with_clause and is printed only by the former.
  */
  lex->unit.print(&query, FEDERATEDX_PRINT_UPD_DEL_QUERY_TYPE);
}


/*
  Execute a multi-table UPDATE/DELETE on the remote server and report
  how many rows it has changed
*/

int ha_federatedx_multi_upddel_handler::batch_update_delete(
    ha_rows *found_rows, ha_rows *affected_rows)
{
  THD *thd= query_table->in_use;
  int rc;
  DBUG_ENTER("ha_federatedx_multi_upddel_handler::batch_update_delete");

  ha_federatedx *h= (ha_federatedx *) query_table->file;
  iop= &h->io;
  share= get_share(query_table->s->table_name.str, query_table,
                   h->option_struct);
  txn= h->get_txn(thd);

  /* no need for savepoint in autocommit mode */
  if (!(thd->variables.option_bits & (OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN)))
    txn->stmt_autocommit();

  if ((rc= txn->acquire(share, thd, FALSE, iop)))
  {
    free_share(txn, share);
    share= NULL;
    DBUG_RETURN(rc);
  }

  if ((*iop)->query(query.ptr(), query.length()))
  {
    /*
      The whole statement was executed by the remote server, so an error it
      reports is an error of this statement. Pass its error code through
      instead of hiding it behind ER_QUERY_ON_FOREIGN_DATA_SOURCE, so that the
      client sees the same error it would have seen had the statement been
      executed locally, e.g. ER_DIVISION_BY_ZERO or ER_DUP_ENTRY.

      Errors of the client library (a lost connection and the like) are not
      server error codes and are reported as a foreign data source failure.
    */
    const int remote_errno= (*iop)->error_code();
    if (remote_errno < CR_MIN_ERROR || remote_errno > CR_MAX_ERROR)
      my_message(remote_errno, (*iop)->error_str(), MYF(0));
    else
      my_error(ER_QUERY_ON_FOREIGN_DATA_SOURCE, MYF(0), (*iop)->error_str());
    rc= HA_FEDERATEDX_ERROR_WITH_REMOTE_SYSTEM;
  }
  else
  {
    /*
      For an UPDATE the remote server reports both the number of matched and
      of changed rows (matched_rows() parses it out of the info string), for a
      DELETE matched_rows() returns the number of deleted rows just like
      affected_rows() does.
    */
    *affected_rows= (ha_rows) (*iop)->affected_rows();
    *found_rows= (ha_rows) (*iop)->matched_rows();
    rc= 0;
  }

  free_share(txn, share);
  share= NULL;
  DBUG_RETURN(rc);
}


int federatedx_handler_base::init_scan_()
{
  THD *thd= query_table->in_use;
  int rc= 0;

  DBUG_ENTER("ha_federatedx_select_handler::init_scan");

  ha_federatedx *h= (ha_federatedx *) query_table->file;
  iop= &h->io;
  share= get_share(query_table->s->table_name.str, query_table,
                   h->option_struct);
  txn= h->get_txn(thd);
  if ((rc= txn->acquire(share, thd, TRUE, iop)))
    DBUG_RETURN(rc);

  if ((*iop)->query(query.ptr(), query.length()))
    goto err;

  stored_result= (*iop)->store_result();
  if (!stored_result)
      goto err;

  DBUG_RETURN(0);

err:
  DBUG_RETURN(HA_FEDERATEDX_ERROR_WITH_REMOTE_SYSTEM);
}

int federatedx_handler_base::next_row_(TABLE *table)
{
  int rc= 0;
  FEDERATEDX_IO_ROW *row;
  ulong *lengths;
  Field **field;
  int column= 0;
  Time_zone *saved_time_zone= table->in_use->variables.time_zone;
  DBUG_ENTER("ha_federatedx_select_handler::next_row");

  if ((rc= txn->acquire(share, table->in_use, TRUE, iop)))
    DBUG_RETURN(rc);

  if (!(row= (*iop)->fetch_row(stored_result)))
    DBUG_RETURN(HA_ERR_END_OF_FILE);

  /* Convert row to internal format */
  table->in_use->variables.time_zone= UTC;
  lengths= (*iop)->fetch_lengths(stored_result);

  for (field= table->field; *field; field++, column++)
  {
    if ((*iop)->is_column_null(row, column))
       (*field)->set_null();
    else
    {
      (*field)->set_notnull();
      (*field)->store((*iop)->get_column_data(row, column),
                      lengths[column], &my_charset_bin);
    }
  }
  table->in_use->variables.time_zone= saved_time_zone;

  DBUG_RETURN(rc);
}

int ha_federatedx_select_handler::end_scan()
{
  free_tmp_table(thd, table);
  table= 0;

  return federatedx_handler_base::end_scan_();
}
