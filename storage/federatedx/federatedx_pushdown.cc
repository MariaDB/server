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


static derived_handler*
create_federatedx_derived_handler(THD* thd, TABLE_LIST *derived)
{
  if (!use_pushdown)
    return 0;

  ha_federatedx_derived_handler* handler = NULL;
  handlerton *ht= 0;

  SELECT_LEX_UNIT *unit= derived->derived;

  for (SELECT_LEX *sl= unit->first_select(); sl; sl= sl->next_select())
  {
    if (!(sl->join))
      return 0;
    for (TABLE_LIST *tbl= sl->join->tables_list; tbl; tbl= tbl->next_local)
    {
      if (!tbl->table)
	return 0;
      if (!ht)
        ht= tbl->table->file->partition_ht();
      else if (ht != tbl->table->file->partition_ht())
        return 0;
    }
  }

  handler= new ha_federatedx_derived_handler(thd, derived);

  return handler;
}


/*
  Implementation class of the derived_handler interface for FEDERATEDX:
  class implementation
*/

ha_federatedx_derived_handler::ha_federatedx_derived_handler(THD *thd,
                                                             TABLE_LIST *dt)
  : derived_handler(thd, federatedx_hton),
    share(NULL), txn(NULL), iop(NULL), stored_result(NULL)
{
  derived= dt;
}

ha_federatedx_derived_handler::~ha_federatedx_derived_handler() {}

int ha_federatedx_derived_handler::init_scan()
{
  THD *thd;
  int rc= 0;

  DBUG_ENTER("ha_federatedx_derived_handler::init_scan");

  TABLE *table= derived->get_first_table()->table;
  ha_federatedx *h= (ha_federatedx *) table->file;
  iop= &h->io;
  share= get_share(table->s->table_name.str, table);
  thd= table->in_use;
  txn= h->get_txn(thd);
  if ((rc= txn->acquire(share, thd, TRUE, iop)))
    DBUG_RETURN(rc);

  if ((*iop)->query(derived->derived_spec.str, derived->derived_spec.length))
    goto err;

  stored_result= (*iop)->store_result();
  if (!stored_result)
      goto err;

  DBUG_RETURN(0);

err:
  DBUG_RETURN(HA_FEDERATEDX_ERROR_WITH_REMOTE_SYSTEM);
}

int ha_federatedx_derived_handler::next_row()
{
  int rc;
  FEDERATEDX_IO_ROW *row;
  ulong *lengths;
  Field **field;
  int column= 0;
  Time_zone *saved_time_zone= table->in_use->variables.time_zone;
  DBUG_ENTER("ha_federatedx_derived_handler::next_row");

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

int ha_federatedx_derived_handler::end_scan()
{
  DBUG_ENTER("ha_federatedx_derived_handler::end_scan");

  (*iop)->free_result(stored_result);

  free_share(txn, share);

  DBUG_RETURN(0);
}

void ha_federatedx_derived_handler::print_error(int, unsigned long)
{
}


static select_handler*
create_federatedx_select_handler(THD* thd, SELECT_LEX *sel)
{
  if (!use_pushdown)
    return 0;

  ha_federatedx_select_handler* handler = NULL;
  handlerton *ht= 0;

  for (TABLE_LIST *tbl= thd->lex->query_tables; tbl; tbl= tbl->next_global)
  {
    if (!tbl->table)
      return 0;
    if (!ht)
      ht= tbl->table->file->partition_ht();
    else if (ht != tbl->table->file->partition_ht())
      return 0;
  }

  /*
    Currently, ha_federatedx_select_handler::init_scan just takes the
    thd->query and sends it to the backend.
    This obviously won't work if the SELECT uses an "INTO @var" or
    "INTO OUTFILE". It is also unlikely to work if the select has some
    other kind of side effect.
  */
  if (sel->uncacheable & UNCACHEABLE_SIDEEFFECT)
    return NULL;

  handler= new ha_federatedx_select_handler(thd, sel);

  return handler;
}

/*
  Implementation class of the select_handler interface for FEDERATEDX:
  class implementation
*/

ha_federatedx_select_handler::ha_federatedx_select_handler(THD *thd,
                                                           SELECT_LEX *sel)
  : select_handler(thd, federatedx_hton),
    share(NULL), txn(NULL), iop(NULL), stored_result(NULL)
{
  select= sel;
}

ha_federatedx_select_handler::~ha_federatedx_select_handler() {}

int ha_federatedx_select_handler::init_scan()
{
  int rc= 0;

  DBUG_ENTER("ha_federatedx_select_handler::init_scan");

  TABLE *table= 0;
  for (TABLE_LIST *tbl= thd->lex->query_tables; tbl; tbl= tbl->next_global)
  {
    if (!tbl->table)
      continue;
    table= tbl->table;
    break;
  }
  ha_federatedx *h= (ha_federatedx *) table->file;
  iop= &h->io;
  share= get_share(table->s->table_name.str, table);
  txn= h->get_txn(thd);
  if ((rc= txn->acquire(share, thd, TRUE, iop)))
    DBUG_RETURN(rc);

  if ((*iop)->query(thd->query(), thd->query_length()))
    goto err;

  stored_result= (*iop)->store_result();
  if (!stored_result)
      goto err;

  DBUG_RETURN(0);

err:
  DBUG_RETURN(HA_FEDERATEDX_ERROR_WITH_REMOTE_SYSTEM);
}

int ha_federatedx_select_handler::next_row()
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
  DBUG_ENTER("ha_federatedx_derived_handler::end_scan");

  free_tmp_table(thd, table);
  table= 0;

  (*iop)->free_result(stored_result);

  free_share(txn, share);

  DBUG_RETURN(0);
}

void ha_federatedx_select_handler::print_error(int error, myf error_flag)
{
  select_handler::print_error(error, error_flag);
}
