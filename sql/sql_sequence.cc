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

#include "sql_class.h"
#include "sql_list.h"
#include "sql_sequence.h"
#include "ha_sequence.h"
#include "sql_base.h"
#include "transaction.h"
#include "lock.h"

struct Field_definition
{
  const char *field_name;
  uint length;
  enum enum_field_types sql_type;
  LEX_STRING comment;
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
  {"next_value", 21, MYSQL_TYPE_LONGLONG, {C_STRING_WITH_LEN("next not cached value")},
   FL},
  {"min_value", 21, MYSQL_TYPE_LONGLONG, {C_STRING_WITH_LEN("min value")}, FL},
  {"max_value", 21, MYSQL_TYPE_LONGLONG, {C_STRING_WITH_LEN("max value")}, FL},
  {"start", 21, MYSQL_TYPE_LONGLONG, {C_STRING_WITH_LEN("start value")},  FL},
  {"increment", 21, MYSQL_TYPE_LONGLONG,
   {C_STRING_WITH_LEN("increment value")}, FL},
  {"cache", 21, MYSQL_TYPE_LONGLONG, {C_STRING_WITH_LEN("cache size")}, FL},
  {"cycle", 1, MYSQL_TYPE_TINY, {C_STRING_WITH_LEN("cycle state")},
   FL | UNSIGNED_FLAG },
  {"round", 21, MYSQL_TYPE_LONGLONG,
   {C_STRING_WITH_LEN("How many cycles has been done")}, FL},
  {NULL, 0, MYSQL_TYPE_LONGLONG, {C_STRING_WITH_LEN("")}, 0}
};

#undef FL


#define MAX_AUTO_INCREMENT_VALUE 65535

/*
  Check whether sequence values are valid.
  Sets default values for fields that are not used, according to Oracle spec.

  Note that reserved_until is not checked as it's ok that it's outside of
  the range (to indicate that sequence us used up).

  RETURN VALUES
     false      valid
     true       invalid
*/

bool sequence_definition::check_and_adjust()
{
  longlong max_increment;
  DBUG_ENTER("sequence_definition::check");

  /*
    If min_value is not set, set it to LONGLONG_MIN or 1, depending on
    increment
  */
  if (!(used_fields & seq_field_used_min_value))
    min_value= increment < 0 ? LONGLONG_MIN+1 : 1;

  /*
    If min_value is not set, set it to LONGLONG_MAX or -1, depending on
    increment
  */
  if (!(used_fields & seq_field_used_max_value))
    max_value= increment < 0 ? -1 : LONGLONG_MAX-1;

  if (!(used_fields & seq_field_used_start))
  {
    /* Use min_value or max_value for start depending on increment */
    start= increment < 0 ? max_value : min_value;
  }

  /* To ensure that cache * increment will never overflow */
  max_increment= increment ? labs(increment) : MAX_AUTO_INCREMENT_VALUE;

  if (max_value >= start &&
      max_value > min_value &&
      start >= min_value &&
      max_value != LONGLONG_MAX &&
      min_value != LONGLONG_MIN &&
      cache < (LONGLONG_MAX - max_increment) / max_increment)
    DBUG_RETURN(FALSE);
  DBUG_RETURN(TRUE);
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
  Check the sequence fields through seq_fields when create sequence.qq

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
                      field->field_name) ||
        field->flags != field_def->flags ||
        field->sql_type != field_def->sql_type)
    {
      reason= field->field_name;
      goto err;
    }
  }
  DBUG_RETURN(FALSE);

err:
  my_error(ER_SEQUENCE_INVALID_TABLE_STRUCTURE, MYF(0),
           lex->select_lex.table_list.first->db,
           lex->select_lex.table_list.first->table_name, reason);
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
    if (unlikely(!(new_field= new Create_field())))
      DBUG_RETURN(TRUE); /* purify inspected */

    new_field->field_name=  field_info->field_name;
    new_field->sql_type=    field_info->sql_type;
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

bool sequence_insert(THD *thd, LEX *lex, TABLE_LIST *table_list)
{
  int error;
  TABLE *table;
  TABLE_LIST::enum_open_strategy save_open_strategy;
  sequence_definition *seq= lex->create_info.seq_create_info;
  bool temporary_table= table_list->table != 0;
  MY_BITMAP *save_write_set;
  DBUG_ENTER("sequence_insert");

  /* If not temporary table */
  if (!temporary_table)
  {
    /* Table was locked as part of create table. Free it but keep MDL locks */
    close_thread_tables(thd);
    table_list->lock_type= TL_WRITE_DEFAULT;
    table_list->updating=  1;
    /*
      The FOR CREATE flag is needed to ensure that ha_open() doesn't try to
      read the not yet existing row in the sequence table
    */
    thd->open_options|= HA_OPEN_FOR_CREATE;
    save_open_strategy= table_list->open_strategy;
    table_list->open_strategy= TABLE_LIST::OPEN_IF_EXISTS;
    table_list->open_type= OT_BASE_ONLY;
    error= open_and_lock_tables(thd, table_list, FALSE,
                                MYSQL_LOCK_IGNORE_TIMEOUT |
                                MYSQL_OPEN_HAS_MDL_LOCK);
    table_list->open_strategy= save_open_strategy;
    thd->open_options&= ~HA_OPEN_FOR_CREATE;
    if (error)
      DBUG_RETURN(TRUE); /* purify inspected */
  }

  table= table_list->table;

  /*
    seq is 0 if sequence was created with CREATE TABLE instead of
    CREATE SEQUENCE
  */
  if (!seq)
  {
    if (!(seq= new (thd->mem_root) sequence_definition))
      DBUG_RETURN(TRUE);                        // EOM
  }

  seq->reserved_until= seq->start;
  seq->store_fields(table);
  /* Store the sequence values in table share */
  table->s->sequence->copy(seq);

  /*
    Sequence values will be replicated as a statement
    like 'create sequence'. So disable binary log temporarily
  */
  tmp_disable_binlog(thd);
  save_write_set= table->write_set;
  table->write_set= &table->s->all_set;
  error= table->file->ha_write_row(table->record[0]);
  reenable_binlog(thd);
  table->write_set= save_write_set;

  if (error)
    table->file->print_error(error, MYF(0));
  else
  {
    /*
      Sequence structure is up to date and table has one row,
      sequence is now usable
    */
    table->s->sequence->initialized= 1;
  }

  trans_commit_stmt(thd);
  trans_commit_implicit(thd);
  if (!temporary_table)
    close_thread_tables(thd);
  thd->mdl_context.release_transactional_locks();
  DBUG_RETURN(error);
}


/* Create a SQUENCE object */

SEQUENCE::SEQUENCE() :initialized(0), all_values_used(0), table(0)
{
  mysql_mutex_init(key_LOCK_SEQUENCE, &mutex, MY_MUTEX_INIT_SLOW);
}

SEQUENCE::~SEQUENCE()
{
  mysql_mutex_destroy(&mutex);
}


/**
   Read values from the sequence tables to table_share->sequence.
   This is called from ha_open() when the table is not yet locked
*/

int SEQUENCE::read_initial_values(TABLE *table_arg)
{
  int error= 0;
  enum thr_lock_type save_lock_type;
  MDL_request mdl_request;                      // Empty constructor!
  DBUG_ENTER("SEQUENCE::read_initial_values");

  if (likely(initialized))
    DBUG_RETURN(0);
  table= table_arg;
  mysql_mutex_lock(&mutex);
  if (unlikely(!initialized))
  {
    MYSQL_LOCK *lock;
    bool mdl_lock_used= 0;
    THD *thd= table->in_use;
    bool has_active_transaction= !thd->transaction.stmt.is_empty();
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

      mdl_request.init(MDL_key::TABLE,
                       table->s->db.str,
                       table->s->table_name.str,
                       MDL_SHARED_READ, MDL_EXPLICIT);
      mdl_requests.push_front(&mdl_request);
      if (thd->mdl_context.acquire_locks(&mdl_requests,
                                         thd->variables.lock_wait_timeout))
        DBUG_RETURN(HA_ERR_LOCK_WAIT_TIMEOUT);
    }
    save_lock_type= table->reginfo.lock_type;
    table->reginfo.lock_type= TL_READ;
    if (!(lock= mysql_lock_tables(thd, &table, 1,
                                  MYSQL_LOCK_IGNORE_GLOBAL_READ_ONLY)))
    {
      if (mdl_lock_used)
        thd->mdl_context.release_lock(mdl_request.ticket);
      DBUG_RETURN(HA_ERR_LOCK_WAIT_TIMEOUT);
    }
    if (!(error= read_stored_values()))
      initialized= 1;
    mysql_unlock_tables(thd, lock, 0);
    if (mdl_lock_used)
      thd->mdl_context.release_lock(mdl_request.ticket);

    /* Reset value to default */
    table->reginfo.lock_type= save_lock_type;
    /*
      Doing mysql_lock_tables() may have started a read only transaction.
      If that happend, it's better that we commit it now, as a lot of
      code assumes that there is no active stmt transaction directly after
      open_tables()
    */
    if (!has_active_transaction && !thd->transaction.stmt.is_empty())
      trans_commit_stmt(thd);
  }
  mysql_mutex_unlock(&mutex);
  DBUG_RETURN(error);
}

/*
  Read data from sequence table and update values
  Done when table is opened
*/

int SEQUENCE::read_stored_values()
{
  int error;
  my_bitmap_map *save_read_set;
  DBUG_ENTER("SEQUENCE::read_stored_values");
  mysql_mutex_assert_owner(&mutex);

  save_read_set= tmp_use_all_columns(table, table->read_set);
  error= table->file->ha_read_first_row(table->record[0], MAX_KEY);
  tmp_restore_column_map(table->read_set, save_read_set);

  if (error)
  {
    table->file->print_error(error, MYF(0));
    DBUG_RETURN(error);
  }
  read_fields(table);
  adjust_values();

  all_values_used= 0;
  DBUG_RETURN(0);
}


/*
  Adjust values after reading a the stored state
*/

void SEQUENCE::adjust_values()
{
  offset= 0;
  next_free_value= reserved_until;
  if (!(real_increment= increment))
  {
    longlong off, to_add;
    /* Use auto_increment_increment and auto_increment_offset */

    if ((real_increment= global_system_variables.auto_increment_increment)
        != 1)
      offset= global_system_variables.auto_increment_offset;

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
      DBUG_ASSERT(next_free_value % real_increment == offset &&
                  next_free_value >= reserved_until);
    }
  }
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
  MY_BITMAP *save_rpl_write_set, *save_write_set;
  DBUG_ENTER("SEQUENCE::next_value");

  *error= 0;
  if (!second_round)
    lock();

  res_value= next_free_value;

  /* Increment next_free_value */
  if (real_increment > 0)
  {
    if (next_free_value + real_increment > max_value ||
        next_free_value > max_value - real_increment)
      next_free_value= max_value + 1;
    else
      next_free_value+= real_increment;
  }
  else
  {
    if (next_free_value + real_increment < min_value ||
        next_free_value < min_value - real_increment)
      next_free_value= min_value - 1;
    else
      next_free_value+= real_increment;
  }

  if ((real_increment > 0 && res_value < reserved_until) ||
      (real_increment < 0 && res_value > reserved_until))
  {
    unlock();
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
  add_to= cache ? real_increment * cache : 1;
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
    adjust_values();                            // Fix next_free_value
    /*
      We have to do everything again to ensure that the given range was
      not empty, which could happen if increment == 0
    */
    DBUG_RETURN(next_value(table, 1, error));
  }

  /* Log a full insert (ok as table is small) */
  save_rpl_write_set= table->rpl_write_set;

  /* Update table */
  save_write_set= table->write_set;
  table->rpl_write_set= table->write_set= &table->s->all_set;
  store_fields(table);
  /* Tell ha_sequence::write_row that we already hold the mutex */
  ((ha_sequence*) table->file)->sequence_locked= 1;
  if ((*error= table->file->ha_write_row(table->record[0])))
  {
    table->file->print_error(*error, MYF(0));
    /* Restore original range */
    reserved_until= org_reserved_until;
    next_free_value= res_value;
  }
  ((ha_sequence*) table->file)->sequence_locked= 0;
  table->rpl_write_set= save_rpl_write_set;
  table->write_set= save_write_set;

  unlock();
  DBUG_RETURN(res_value);

err:
  unlock();
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
