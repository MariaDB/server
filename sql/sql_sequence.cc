/*
   Copyright (c) 2017, MariaDB Corporation, Alibaba Corporation

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
*/

#include "mariadb.h"
#include "sql_class.h"
#include "sql_list.h"
#include "sql_sequence.h"
#include "ha_sequence.h"
#include "sql_base.h"
#include "sql_table.h"                          // write_bin_log
#include "transaction.h"
#include "lock.h"
#include "sql_acl.h"

struct Field_definition
{
  const char *field_name;
  uint length;
  const Type_handler *type_handler;
  LEX_CSTRING comment;
  ulong flags;
};

/*
  Structure for all SEQUENCE tables

  Note that the first field is named "next_val" to all us to have
  NEXTVAL a reserved word that will on access be changed to
  NEXTVAL(sequence_table). For this to work, the table can't have
  a column named NEXTVAL.
*/

#define FL (NOT_NULL_FLAG | NO_DEFAULT_VALUE_FLAG)

static Field_definition sequence_structure[]=
{
  {"next_not_cached_value", 21, &type_handler_slonglong,
   {STRING_WITH_LEN("")}, FL},
  {"minimum_value", 21, &type_handler_slonglong, {STRING_WITH_LEN("")}, FL},
  {"maximum_value", 21, &type_handler_slonglong, {STRING_WITH_LEN("")}, FL},
  {"start_value", 21, &type_handler_slonglong, {STRING_WITH_LEN("start value when sequences is created or value if RESTART is used")},  FL},
  {"increment", 21, &type_handler_slonglong,
   {STRING_WITH_LEN("increment value")}, FL},
  {"cache_size", 21, &type_handler_ulonglong, {STRING_WITH_LEN("")},
   FL | UNSIGNED_FLAG},
  {"cycle_option", 1, &type_handler_utiny, {STRING_WITH_LEN("0 if no cycles are allowed, 1 if the sequence should begin a new cycle when maximum_value is passed")},
   FL | UNSIGNED_FLAG },
  {"cycle_count", 21, &type_handler_slonglong,
   {STRING_WITH_LEN("How many cycles have been done")}, FL},
  {NULL, 0, &type_handler_slonglong, {STRING_WITH_LEN("")}, 0}
};

#undef FL


#define MAX_AUTO_INCREMENT_VALUE 65535

/*
  Check whether sequence values are valid.
  Sets default values for fields that are not used, according to Oracle spec.

  RETURN VALUES
     false      valid
     true       invalid
*/

bool sequence_definition::check_and_adjust(bool set_reserved_until)
{
  longlong max_increment;
  DBUG_ENTER("sequence_definition::check");

  if (!(real_increment= increment))
    real_increment= global_system_variables.auto_increment_increment;

  /*
    If min_value is not set, set it to LONGLONG_MIN or 1, depending on
    real_increment
  */
  if (!(used_fields & seq_field_used_min_value))
    min_value= real_increment < 0 ? LONGLONG_MIN+1 : 1;

  /*
    If max_value is not set, set it to LONGLONG_MAX or -1, depending on
    real_increment
  */
  if (!(used_fields & seq_field_used_max_value))
    max_value= real_increment < 0 ? -1 : LONGLONG_MAX-1;

  if (!(used_fields & seq_field_used_start))
  {
    /* Use min_value or max_value for start depending on real_increment */
    start= real_increment < 0 ? max_value : min_value;
  }

  if (set_reserved_until)
    reserved_until= start;

  adjust_values(reserved_until);

  /* To ensure that cache * real_increment will never overflow */
  max_increment= (real_increment ?
                  llabs(real_increment) :
                  MAX_AUTO_INCREMENT_VALUE);

  if (max_value >= start &&
      max_value > min_value &&
      start >= min_value &&
      max_value != LONGLONG_MAX &&
      min_value != LONGLONG_MIN &&
      cache < (LONGLONG_MAX - max_increment) / max_increment &&
      ((real_increment > 0 && reserved_until >= min_value) ||
       (real_increment < 0 && reserved_until <= max_value)))
    DBUG_RETURN(FALSE);

  DBUG_RETURN(TRUE);                           // Error
}


/*
  Read sequence values from a table
*/

void sequence_definition::read_fields(TABLE *table)
{
  my_bitmap_map *old_map= dbug_tmp_use_all_columns(table, table->read_set);
  reserved_until= table->field[0]->val_int();
  min_value=      table->field[1]->val_int();
  max_value=      table->field[2]->val_int();
  start=          table->field[3]->val_int();
  increment=      table->field[4]->val_int();
  cache=          table->field[5]->val_int();
  cycle=          table->field[6]->val_int();
  round=          table->field[7]->val_int();
  dbug_tmp_restore_column_map(table->read_set, old_map);
  used_fields= ~(uint) 0;
  print_dbug();
}


/*
  Store sequence into a table row
*/

void sequence_definition::store_fields(TABLE *table)
{
  my_bitmap_map *old_map= dbug_tmp_use_all_columns(table, table->write_set);

  /* zero possible delete markers & null bits */
  memcpy(table->record[0], table->s->default_values, table->s->null_bytes);
  table->field[0]->store(reserved_until, 0);
  table->field[1]->store(min_value, 0);
  table->field[2]->store(max_value, 0);
  table->field[3]->store(start, 0);
  table->field[4]->store(increment, 0);
  table->field[5]->store(cache, 0);
  table->field[6]->store((longlong) cycle != 0, 0);
  table->field[7]->store((longlong) round, 1);

  dbug_tmp_restore_column_map(table->write_set, old_map);
  print_dbug();
}


/*
  Check the sequence fields through seq_fields when creating a sequence.

  RETURN VALUES
    false       Success
    true        Failure
*/

bool check_sequence_fields(LEX *lex, List<Create_field> *fields)
{
  Create_field *field;
  List_iterator_fast<Create_field> it(*fields);
  uint field_count;
  uint field_no;
  const char *reason;
  DBUG_ENTER("check_sequence_fields");

  field_count= fields->elements;
  if (field_count != array_elements(sequence_structure)-1)
  {
    reason= "Wrong number of columns";
    goto err;
  }
  if (lex->alter_info.key_list.elements > 0)
  {
    reason= "Sequence tables cannot have any keys";
    goto err;
  }

  for (field_no= 0; (field= it++); field_no++)
  {
    Field_definition *field_def= &sequence_structure[field_no];
    if (my_strcasecmp(system_charset_info, field_def->field_name,
                      field->field_name.str) ||
        field->flags != field_def->flags ||
        field->type_handler() != field_def->type_handler)
    {
      reason= field->field_name.str;
      goto err;
    }
  }
  DBUG_RETURN(FALSE);

err:
  my_error(ER_SEQUENCE_INVALID_TABLE_STRUCTURE, MYF(0),
           lex->first_select_lex()->table_list.first->db.str,
           lex->first_select_lex()->table_list.first->table_name.str, reason);
  DBUG_RETURN(TRUE);
}


/*
  Create the fields for a SEQUENCE TABLE

  RETURN VALUES
    false       Success
    true        Failure (out of memory)
*/

bool prepare_sequence_fields(THD *thd, List<Create_field> *fields)
{
  Field_definition *field_info;
  DBUG_ENTER("prepare_sequence_fields");

  for (field_info= sequence_structure; field_info->field_name ; field_info++)
  {
    Create_field *new_field;
    LEX_CSTRING field_name= {field_info->field_name,
                             strlen(field_info->field_name)};

    if (unlikely(!(new_field= new Create_field())))
      DBUG_RETURN(TRUE); /* purify inspected */

    new_field->field_name=  field_name;
    new_field->set_handler(field_info->type_handler);
    new_field->length=      field_info->length;
    new_field->char_length= field_info->length;
    new_field->comment=     field_info->comment;
    new_field->flags=       field_info->flags;
    if (unlikely(fields->push_back(new_field)))
      DBUG_RETURN(TRUE); /* purify inspected */
  }
  DBUG_RETURN(FALSE);
}

/*
  Initialize the sequence table record as part of CREATE SEQUENCE

  Store one row with sequence information.

  RETURN VALUES
    false       Success
    true        Failure. Error reported.

  NOTES
    This function is called as part of CREATE SEQUENCE. When called
    there are now active transactions and no open tables.
    There is also a MDL lock on the table.
*/

bool sequence_insert(THD *thd, LEX *lex, TABLE_LIST *org_table_list)
{
  int error;
  TABLE *table;
  Reprepare_observer *save_reprepare_observer;
  sequence_definition *seq= lex->create_info.seq_create_info;
  bool temporary_table= org_table_list->table != 0;
  Open_tables_backup open_tables_backup;
  Query_tables_list query_tables_list_backup;
  TABLE_LIST table_list;                        // For sequence table
  DBUG_ENTER("sequence_insert");

  /*
    seq is 0 if sequence was created with CREATE TABLE instead of
    CREATE SEQUENCE
  */
  if (!seq)
  {
    if (!(seq= new (thd->mem_root) sequence_definition))
      DBUG_RETURN(TRUE);
  }

  /* If not temporary table */
  if (!temporary_table)
  {
    /*
      The following code works like open_system_tables_for_read()
      The idea is:
      - Copy the table_list object for the sequence that was created
      - Backup the current state of open tables and create a new
        environment for open tables without any tables opened
     - open the newly sequence table for write
     This is safe as the sequence table has a mdl lock thanks to the
     create sequence statement that is calling this function
    */

    table_list.init_one_table(&org_table_list->db,
                              &org_table_list->table_name, 
                              NULL, TL_WRITE_DEFAULT);
    table_list.updating=  1;
    table_list.open_strategy= TABLE_LIST::OPEN_IF_EXISTS;
    table_list.open_type= OT_BASE_ONLY;

    DBUG_ASSERT(!thd->locked_tables_mode ||
                (thd->variables.option_bits & OPTION_TABLE_LOCK));
    lex->reset_n_backup_query_tables_list(&query_tables_list_backup);
    thd->reset_n_backup_open_tables_state(&open_tables_backup);

    /*
      The FOR CREATE flag is needed to ensure that ha_open() doesn't try to
      read the not yet existing row in the sequence table
    */
    thd->open_options|= HA_OPEN_FOR_CREATE;
    /*
      We have to reset the reprepare observer to be able to open the
      table under prepared statements.
    */
    save_reprepare_observer= thd->m_reprepare_observer;
    thd->m_reprepare_observer= 0;
    lex->sql_command= SQLCOM_CREATE_SEQUENCE;
    error= open_and_lock_tables(thd, &table_list, FALSE,
                                MYSQL_LOCK_IGNORE_TIMEOUT |
                                MYSQL_OPEN_HAS_MDL_LOCK);
    thd->open_options&= ~HA_OPEN_FOR_CREATE;
    thd->m_reprepare_observer= save_reprepare_observer;
    if (unlikely(error))
    {
      lex->restore_backup_query_tables_list(&query_tables_list_backup);
      thd->restore_backup_open_tables_state(&open_tables_backup);
      DBUG_RETURN(error);
    }
    table= table_list.table;
  }
  else
    table= org_table_list->table;

  seq->reserved_until= seq->start;
  error= seq->write_initial_sequence(table);

  trans_commit_stmt(thd);
  trans_commit_implicit(thd);

  if (!temporary_table)
  {
    close_thread_tables(thd);
    lex->restore_backup_query_tables_list(&query_tables_list_backup);
    thd->restore_backup_open_tables_state(&open_tables_backup);

    /* OPTION_TABLE_LOCK was reset in trans_commit_implicit */
    if (thd->locked_tables_mode)
      thd->variables.option_bits|= OPTION_TABLE_LOCK;
  }
  DBUG_RETURN(error);
}


/* Create a SQUENCE object */

SEQUENCE::SEQUENCE() :all_values_used(0), initialized(SEQ_UNINTIALIZED)
{
  mysql_rwlock_init(key_LOCK_SEQUENCE, &mutex);
}

SEQUENCE::~SEQUENCE()
{
  mysql_rwlock_destroy(&mutex);
}

/*
  The following functions is to ensure that we when reserve new values
  trough sequence object sequence we have only one writer at at time.
  A sequence table can have many readers (trough normal SELECT's).

  We mark that we have a write lock in the table object so that
  ha_sequence::ha_write() can check if we have a lock. If already locked, then
  ha_write() knows that we are running a sequence operation. If not, then
  ha_write() knows that it's an INSERT.
*/

void SEQUENCE::write_lock(TABLE *table)
{
  DBUG_ASSERT(((ha_sequence*) table->file)->is_locked() == 0);
  mysql_rwlock_wrlock(&mutex);
  ((ha_sequence*) table->file)->write_lock();
}
void SEQUENCE::write_unlock(TABLE *table)
{
  ((ha_sequence*) table->file)->unlock();
  mysql_rwlock_unlock(&mutex);
}
void SEQUENCE::read_lock(TABLE *table)
{
  if (!((ha_sequence*) table->file)->is_locked())
    mysql_rwlock_rdlock(&mutex);
}
void SEQUENCE::read_unlock(TABLE *table)
{
  if (!((ha_sequence*) table->file)->is_locked())
    mysql_rwlock_unlock(&mutex);
}

/**
   Read values from the sequence tables to table_share->sequence.
   This is called from ha_open() when the table is not yet locked
*/

int SEQUENCE::read_initial_values(TABLE *table)
{
  int error= 0;
  enum thr_lock_type save_lock_type;
  MDL_request mdl_request;                      // Empty constructor!
  DBUG_ENTER("SEQUENCE::read_initial_values");

  if (likely(initialized != SEQ_UNINTIALIZED))
    DBUG_RETURN(0);
  write_lock(table);
  if (likely(initialized == SEQ_UNINTIALIZED))
  {
    MYSQL_LOCK *lock;
    bool mdl_lock_used= 0;
    THD *thd= table->in_use;
    bool has_active_transaction= !thd->transaction->stmt.is_empty();
    /*
      There is already a mdl_ticket for this table. However, for list_fields
      the MDL lock is of type MDL_SHARED_HIGH_PRIO which is not usable
      for doing a able lock. Get a proper read lock to solve this.
    */
    if (table->mdl_ticket == 0)
    {
      MDL_request_list mdl_requests;
      mdl_lock_used= 1;
      /*
        This happens if first request is SHOW CREATE TABLE or LIST FIELDS
        where we don't have a mdl lock on the table
      */

      MDL_REQUEST_INIT(&mdl_request, MDL_key::TABLE, table->s->db.str,
                       table->s->table_name.str, MDL_SHARED_READ,
                       MDL_EXPLICIT);
      mdl_requests.push_front(&mdl_request);
      if (thd->mdl_context.acquire_locks(&mdl_requests,
                                         thd->variables.lock_wait_timeout))
      {
        write_unlock(table);
        DBUG_RETURN(HA_ERR_LOCK_WAIT_TIMEOUT);
      }
    }
    save_lock_type= table->reginfo.lock_type;
    table->reginfo.lock_type= TL_READ;
    if (!(lock= mysql_lock_tables(thd, &table, 1,
                                  MYSQL_LOCK_IGNORE_GLOBAL_READ_ONLY)))
    {
      if (mdl_lock_used)
        thd->mdl_context.release_lock(mdl_request.ticket);
      write_unlock(table);
      DBUG_RETURN(HA_ERR_LOCK_WAIT_TIMEOUT);
    }
    DBUG_ASSERT(table->reginfo.lock_type == TL_READ);
    if (likely(!(error= read_stored_values(table))))
      initialized= SEQ_READY_TO_USE;
    mysql_unlock_tables(thd, lock);
    if (mdl_lock_used)
      thd->mdl_context.release_lock(mdl_request.ticket);

    /* Reset value to default */
    table->reginfo.lock_type= save_lock_type;
    /*
      Doing mysql_lock_tables() may have started a read only transaction.
      If that happend, it's better that we commit it now, as a lot of
      code assumes that there is no active stmt transaction directly after
      open_tables().
      But we also don't want to commit the stmt transaction while in a
      substatement, see MDEV-15977.
    */
    if (!has_active_transaction && !thd->transaction->stmt.is_empty() &&
        !thd->in_sub_stmt)
      trans_commit_stmt(thd);
  }
  write_unlock(table);
  DBUG_RETURN(error);
}


/*
  Do the actiual reading of data from sequence table and
  update values in the sequence object.

  Called once from when table is opened
*/

int SEQUENCE::read_stored_values(TABLE *table)
{
  int error;
  my_bitmap_map *save_read_set;
  DBUG_ENTER("SEQUENCE::read_stored_values");

  save_read_set= tmp_use_all_columns(table, table->read_set);
  error= table->file->ha_read_first_row(table->record[0], MAX_KEY);
  tmp_restore_column_map(table->read_set, save_read_set);

  if (unlikely(error))
  {
    table->file->print_error(error, MYF(0));
    DBUG_RETURN(error);
  }
  read_fields(table);
  adjust_values(reserved_until);

  all_values_used= 0;
  DBUG_RETURN(0);
}


/*
  Adjust values after reading a the stored state
*/

void sequence_definition::adjust_values(longlong next_value)
{
  next_free_value= next_value;
  if (!(real_increment= increment))
  {
    longlong offset= 0;
    longlong off, to_add;
    /* Use auto_increment_increment and auto_increment_offset */

    if ((real_increment= global_system_variables.auto_increment_increment)
        != 1)
      offset= (global_system_variables.auto_increment_offset %
               global_system_variables.auto_increment_increment);

    /*
      Ensure that next_free_value has the right offset, so that we
      can generate a serie by just adding real_increment.
    */
    off= next_free_value % real_increment;
    if (off < 0)
      off+= real_increment;
    to_add= (real_increment + offset - off) % real_increment;

    /*
      Check if add will make next_free_value bigger than max_value,
      taken into account that next_free_value or max_value addition
      may overflow
    */
    if (next_free_value > max_value - to_add ||
        next_free_value + to_add > max_value)
      next_free_value= max_value+1;
    else
    {
      next_free_value+= to_add;
      DBUG_ASSERT(llabs(next_free_value % real_increment) == offset);
    }
  }
}


/**
   Write initial sequence information for CREATE and ALTER to sequence table
*/

int sequence_definition::write_initial_sequence(TABLE *table)
{
  int error;
  MY_BITMAP *save_write_set;

  store_fields(table);
  /* Store the sequence values in table share */
  table->s->sequence->copy(this);
  /*
    Sequence values will be replicated as a statement
    like 'create sequence'. So disable row logging for this table & statement
  */
  table->file->row_logging= table->file->row_logging_init= 0;
  save_write_set= table->write_set;
  table->write_set= &table->s->all_set;
  table->s->sequence->initialized= SEQUENCE::SEQ_IN_PREPARE;
  error= table->file->ha_write_row(table->record[0]);
  table->s->sequence->initialized= SEQUENCE::SEQ_UNINTIALIZED;
  table->write_set= save_write_set;
  if (unlikely(error))
    table->file->print_error(error, MYF(0));
  else
  {
    /*
      Sequence structure is up to date and table has one row,
      sequence is now usable
    */
    table->s->sequence->initialized= SEQUENCE::SEQ_READY_TO_USE;
  }
  return error;
}


/**
   Store current sequence values into the sequence table
*/

int sequence_definition::write(TABLE *table, bool all_fields)
{
  int error;
  MY_BITMAP *save_rpl_write_set, *save_write_set, *save_read_set;
  DBUG_ASSERT(((ha_sequence*) table->file)->is_locked());

  save_rpl_write_set= table->rpl_write_set;
  if (likely(!all_fields))
  {
    /* Only write next_value and round to binary log */
    table->rpl_write_set= &table->def_rpl_write_set;
    bitmap_clear_all(table->rpl_write_set);
    bitmap_set_bit(table->rpl_write_set, NEXT_FIELD_NO);
    bitmap_set_bit(table->rpl_write_set, ROUND_FIELD_NO);
  }
  else
    table->rpl_write_set= &table->s->all_set;

  /* Update table */
  save_write_set= table->write_set;
  save_read_set=  table->read_set;
  table->read_set= table->write_set= &table->s->all_set;
  table->file->column_bitmaps_signal();
  store_fields(table);
  if (unlikely((error= table->file->ha_write_row(table->record[0]))))
    table->file->print_error(error, MYF(0));
  table->rpl_write_set= save_rpl_write_set;
  table->read_set=  save_read_set;
  table->write_set= save_write_set;
  table->file->column_bitmaps_signal();
  return error;
}


/**
   Get next value for sequence

   @param in   table  Sequence table
   @param in   second_round
                      1 if recursive call (out of values once)
   @param out  error  Set this to <> 0 in case of error
                      push_warning_printf(WARN_LEVEL_WARN) has been called


   @retval     0      Next number or error. Check error variable
               #      Next sequence number

   NOTES:
     Return next_free_value and increment next_free_value to next allowed
     value or reserved_value if out of range
     if next_free_value >= reserved_value reserve a new range by writing
     a record to the sequence table.

  The state of the variables:
    next_free_value contains next value to use. It may be
    bigger than max_value or less than min_value if end of sequence.
    reserved_until contains the last value written to the file. All
    values up to this one can be used.
    If next_free_value >= reserved_until we have to reserve new
    values from the sequence.
*/

longlong SEQUENCE::next_value(TABLE *table, bool second_round, int *error)
{
  longlong res_value, org_reserved_until, add_to;
  bool out_of_values;
  DBUG_ENTER("SEQUENCE::next_value");

  *error= 0;
  if (!second_round)
    write_lock(table);

  res_value= next_free_value;
  next_free_value= increment_value(next_free_value);

  if ((real_increment > 0 && res_value < reserved_until) ||
      (real_increment < 0 && res_value > reserved_until))
  {
    write_unlock(table);
    DBUG_RETURN(res_value);
  }

  if (all_values_used)
    goto err;

  org_reserved_until= reserved_until;

  /*
    Out of cached values, reserve 'cache' new ones
    The cache value is checked on insert so the following can't
    overflow
  */
  add_to= cache ? real_increment * cache : real_increment;
  out_of_values= 0;

  if (real_increment > 0)
  {
    if (reserved_until + add_to > max_value ||
        reserved_until > max_value - add_to)
    {
      reserved_until= max_value + 1;
      out_of_values= res_value >= reserved_until;
    }
    else
      reserved_until+= add_to;
  }
  else
  {
    if (reserved_until + add_to < min_value ||
        reserved_until < min_value - add_to)
    {
      reserved_until= min_value - 1;
      out_of_values= res_value <= reserved_until;
    }
    else
      reserved_until+= add_to;
  }
  if (out_of_values)
  {
    if (!cycle || second_round)
      goto err;
    round++;
    reserved_until= real_increment >0 ? min_value : max_value;
    adjust_values(reserved_until);              // Fix next_free_value
    /*
      We have to do everything again to ensure that the given range was
      not empty, which could happen if increment == 0
    */
    DBUG_RETURN(next_value(table, 1, error));
  }

  if (unlikely((*error= write(table, 0))))
  {
    reserved_until= org_reserved_until;
    next_free_value= res_value;
  }

  write_unlock(table);
  DBUG_RETURN(res_value);

err:
  write_unlock(table);
  my_error(ER_SEQUENCE_RUN_OUT, MYF(0), table->s->db.str,
           table->s->table_name.str);
  *error= ER_SEQUENCE_RUN_OUT;
  all_values_used= 1;
  DBUG_RETURN(0);
}


/*
   The following functions is to detect if a table has been dropped
   and re-created since last call to PREVIOUS VALUE.

   This is needed as we don't delete dropped sequences from THD->sequence
   for DROP TABLE.
*/

bool SEQUENCE_LAST_VALUE::check_version(TABLE *table)
{
  DBUG_ASSERT(table->s->tabledef_version.length == MY_UUID_SIZE);
  return memcmp(table->s->tabledef_version.str, table_version,
                MY_UUID_SIZE) != 0;
}

void SEQUENCE_LAST_VALUE::set_version(TABLE *table)
{
  memcpy(table_version, table->s->tabledef_version.str, MY_UUID_SIZE);
}

/**
   Set the next value for sequence

   @param in   table       Sequence table
   @param in   next_val    Next free value
   @param in   next_round  Round for 'next_value' (in case of cycles)
   @param in   is_used     1 if next_val is already used

   @retval     0      ok, value adjusted
               -1     value was less than current value
               1      error when storing value

   @comment
   A new value is set only if "nextval,next_round" is less than
   "next_free_value,round". This is needed as in replication
   setvalue() calls may come out to the slave out-of-order.
   Storing only the highest value ensures that sequence object will always
   contain the highest used value when the slave is promoted to a master.
*/

int SEQUENCE::set_value(TABLE *table, longlong next_val, ulonglong next_round,
                         bool is_used)
{
  int error= -1;
  bool needs_to_be_stored= 0;
  longlong org_reserved_until=  reserved_until;
  longlong org_next_free_value= next_free_value;
  ulonglong org_round= round;
  DBUG_ENTER("SEQUENCE::set_value");

  write_lock(table);
  if (is_used)
    next_val= increment_value(next_val);

  if (round > next_round)
    goto end;                                   // error = -1
  if (round == next_round)
  {
    if (real_increment > 0  ?
        next_val < next_free_value :
        next_val > next_free_value)
      goto end;                                 // error = -1
    if (next_val == next_free_value)
    {
      error= 0;
      goto end;
    }
  }
  else if (cycle == 0)
  {
    // round < next_round && no cycles, which is impossible
    my_error(ER_SEQUENCE_RUN_OUT, MYF(0), table->s->db.str,
             table->s->table_name.str);
    error= 1;
    goto end;
  }
  else
    needs_to_be_stored= 1;

  round= next_round;
  adjust_values(next_val);
  if ((real_increment > 0 ?
       next_free_value > reserved_until :
       next_free_value < reserved_until) ||
      needs_to_be_stored)
  {
    reserved_until= next_free_value;
    if (write(table, 0))
    {
      reserved_until=  org_reserved_until;
      next_free_value= org_next_free_value;
      round= org_round;
      error= 1;
      goto end;
    }
  }
  error= 0;

end:
  write_unlock(table);
  DBUG_RETURN(error);
}


bool Sql_cmd_alter_sequence::execute(THD *thd)
{
  int error= 0;
  int trapped_errors= 0;
  LEX *lex= thd->lex;
  TABLE_LIST *first_table= lex->query_tables;
  TABLE *table;
  sequence_definition *new_seq= lex->create_info.seq_create_info;
  SEQUENCE *seq;
  No_such_table_error_handler no_such_table_handler;
  DBUG_ENTER("Sql_cmd_alter_sequence::execute");

  if (check_access(thd, ALTER_ACL, first_table->db.str,
                   &first_table->grant.privilege,
                   &first_table->grant.m_internal,
                   0, 0))
    DBUG_RETURN(TRUE);                  /* purecov: inspected */

  if (check_grant(thd, ALTER_ACL, first_table, FALSE, 1, FALSE))
    DBUG_RETURN(TRUE);                  /* purecov: inspected */

  if (if_exists())
    thd->push_internal_handler(&no_such_table_handler);
  error= open_and_lock_tables(thd, first_table, FALSE, 0);
  if (if_exists())
  {
    trapped_errors= no_such_table_handler.safely_trapped_errors();
    thd->pop_internal_handler();
  }
  if (unlikely(error))
  {
    if (trapped_errors)
    {
      StringBuffer<FN_REFLEN> tbl_name;
      tbl_name.append(&first_table->db);
      tbl_name.append('.');
      tbl_name.append(&first_table->table_name);
      push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE,
                          ER_UNKNOWN_SEQUENCES,
                          ER_THD(thd, ER_UNKNOWN_SEQUENCES),
                          tbl_name.c_ptr_safe());
      my_ok(thd);
      DBUG_RETURN(FALSE);
    }
    DBUG_RETURN(TRUE);
  }

  table= first_table->table;
  seq= table->s->sequence;
  new_seq->reserved_until= seq->reserved_until;

  /* Copy from old sequence those fields that the user didn't specified */
  if (!(new_seq->used_fields & seq_field_used_increment))
    new_seq->increment= seq->increment;
  if (!(new_seq->used_fields & seq_field_used_min_value))
    new_seq->min_value= seq->min_value;
  if (!(new_seq->used_fields & seq_field_used_max_value))
    new_seq->max_value= seq->max_value;
  if (!(new_seq->used_fields & seq_field_used_start))
    new_seq->start=          seq->start;
  if (!(new_seq->used_fields & seq_field_used_cache))
    new_seq->cache= seq->cache;
  if (!(new_seq->used_fields & seq_field_used_cycle))
    new_seq->cycle= seq->cycle;

  /* If we should restart from a new value */
  if (new_seq->used_fields & seq_field_used_restart)
  {
    if (!(new_seq->used_fields & seq_field_used_restart_value))
      new_seq->restart=      new_seq->start;
    new_seq->reserved_until= new_seq->restart;
  }

  /* Let check_and_adjust think all fields are used */
  new_seq->used_fields= ~0;
  if (new_seq->check_and_adjust(0))
  {
    my_error(ER_SEQUENCE_INVALID_DATA, MYF(0),
             first_table->db.str,
             first_table->table_name.str);
    error= 1;
    goto end;
  }

  table->s->sequence->write_lock(table);
  if (likely(!(error= new_seq->write(table, 1))))
  {
    /* Store the sequence values in table share */
    table->s->sequence->copy(new_seq);
  }
  else
    table->file->print_error(error, MYF(0));
  table->s->sequence->write_unlock(table);
  if (trans_commit_stmt(thd))
    error= 1;
  if (trans_commit_implicit(thd))
    error= 1;
  if (likely(!error))
    error= write_bin_log(thd, 1, thd->query(), thd->query_length());
  if (likely(!error))
    my_ok(thd);

end:
  DBUG_RETURN(error);
}
