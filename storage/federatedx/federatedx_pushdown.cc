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


/*
  This is a quick a dirty implemention of the derived_handler and select_handler
  interfaces to be used to push select queries and the queries specifying
  derived tables into FEDERATEDX engine.
  The functions
    create_federatedx_derived_handler and
    create_federatedx_select_handler
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

  if (lower_case_table_names)
  {
    if (strcasecmp(fshare->database, tbl_share->db.str) != 0)
      return true;
  }
  else
  {
    if (strncmp(fshare->database, tbl_share->db.str, tbl_share->db.length) != 0)
      return true;
  }

  return my_strnncoll(system_charset_info, (uchar *) fshare->table_name,
                      strlen(fshare->table_name),
                      (uchar *) tbl_share->table_name.str,
                      tbl_share->table_name.length) != 0;
}


/*
  Check that all tables in the sel_lex use the FederatedX storage engine
  and return one of them
  @return
    One of the tables from sel_lex
*/
static TABLE *get_fed_table_for_pushdown(SELECT_LEX *sel_lex)
{
  TABLE *table= nullptr;
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
      We intentionally don't support partitioned federatedx tables here, so
      use file->ht and not file->partition_ht().
    */
    if (tbl->table->file->ht != federatedx_hton)
      return nullptr;
    const FEDERATEDX_SHARE *fshare=
        ((ha_federatedx *) tbl->table->file)->get_federatedx_share();
    if (local_and_remote_names_mismatch(tbl->table->s, fshare))
      return nullptr;

    if (!table)
      table= tbl->table;
  }

  for (SELECT_LEX_UNIT *un= sel_lex->first_inner_unit(); un;
       un= un->next_unit())
  {
    for (SELECT_LEX *sl= un->first_select(); sl; sl= sl->next_select())
    {
      auto inner_tbl= get_fed_table_for_pushdown(sl);
      if (!inner_tbl)
        return nullptr;
      if (!table)
        table= inner_tbl;
    }
  }
  return table;
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
  for (auto sel_lex= lex_unit->first_select(); sel_lex;
       sel_lex= sel_lex->next_select())
  {
    auto next_tbl= get_fed_table_for_pushdown(sel_lex);
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
    return 0;

  auto tbl= get_fed_table_for_pushdown(sel_lex);
  if (!tbl)
    return nullptr;

  if (sel_lex->uncacheable & UNCACHEABLE_SIDEEFFECT)
    return NULL;

  return new ha_federatedx_select_handler(thd, sel_lex, lex_unit, tbl);
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

int federatedx_handler_base::init_scan_()
{
  THD *thd= query_table->in_use;
  int rc= 0;

  DBUG_ENTER("ha_federatedx_select_handler::init_scan");

  ha_federatedx *h= (ha_federatedx *) query_table->file;
  iop= &h->io;
  share= get_share(query_table->s->table_name.str, query_table);
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
