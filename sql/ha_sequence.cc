/*
   Copyright (c) 2017, Aliyun and/or its affiliates.
   Copyright (c) 2017, MariaDB corporation

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

#include <my_global.h>
#include "sql_list.h"
#include "table.h"
#include "sql_sequence.h"
#include "ha_sequence.h"
#include "sql_plugin.h"
#include "mysql/plugin.h"
#include "sql_priv.h"
#include "sql_parse.h"
#include "sql_table.h"
#include "sql_update.h"
#include "sql_base.h"
#include "log_event.h"

/*
  Table flags we should inherit and disable from the original engine.
  We add HA_STATS_RECORDS_IS_EXACT as ha_sequence::info() will ensure
  that records is always 1
*/

#define SEQUENCE_ENABLED_TABLE_FLAGS  (HA_STATS_RECORDS_IS_EXACT | \
                                       HA_PERSISTENT_TABLE)
#define SEQUENCE_DISABLED_TABLE_FLAGS  (HA_CAN_SQL_HANDLER | \
                                        HA_CAN_INSERT_DELAYED | \
                                        HA_BINLOG_STMT_CAPABLE)
handlerton *sql_sequence_hton;

/*
  Create a sequence handler
*/

ha_sequence::ha_sequence(handlerton *hton, TABLE_SHARE *share)
  :handler(hton, share), sequence_locked(0)
{
  sequence= share->sequence;
  DBUG_ASSERT(share->sequence);
}

/**
   Destructor method must remove the underlying handler
*/
ha_sequence::~ha_sequence()
{
  delete file;
}

/**
   Sequence table open method

   @param name            Path to file (dbname and tablename)
   @param mode            mode
   @param flags           Flags how to open file

   RETURN VALUES
   @retval 0              Success
   @retval != 0           Failure
*/

int ha_sequence::open(const char *name, int mode, uint flags)
{
  int error;
  DBUG_ENTER("ha_sequence::open");
  DBUG_ASSERT(table->s == table_share && file);

  file->table= table;
  if (!(error= file->open(name, mode, flags)))
  {
    /*
      Copy values set by handler::open() in the underlying handler
      Reuse original storage engine data for duplicate key reference
      It would be easier to do this if we would have another handler
      call:  fixup_after_open()...
    */
    ref=        file->ref;
    ref_length= file->ref_length;
    dup_ref=    file->dup_ref;

    /*
      ha_open() sets the following for us. We have to set this for the
      underlying handler
    */
    file->cached_table_flags= file->table_flags();

    file->reset_statistics();
    internal_tmp_table= file->internal_tmp_table=
      MY_TEST(flags & HA_OPEN_INTERNAL_TABLE);
    reset_statistics();

    /* Don't try to read the inital row the call is part of create code */
    if (!(flags & (HA_OPEN_FOR_CREATE | HA_OPEN_FOR_REPAIR)))
    {
      if ((error= table->s->sequence->read_initial_values(table)))
        file->ha_close();
    }
  }
  DBUG_RETURN(error);
}

/*
  Clone the sequence. Needed if table is used by range optimization
  (Very, very unlikely)
*/

handler *ha_sequence::clone(const char *name, MEM_ROOT *mem_root)
{
  ha_sequence *new_handler;
  DBUG_ENTER("ha_sequence::clone");
  if (!(new_handler= new (mem_root) ha_sequence(ht, table_share)))
    DBUG_RETURN(NULL);

  /*
    Allocate new_handler->ref here because otherwise ha_open will allocate it
    on this->table->mem_root and we will not be able to reclaim that memory
    when the clone handler object is destroyed.
  */

  if (!(new_handler->ref= (uchar*) alloc_root(mem_root,
                                              ALIGN_SIZE(ref_length)*2)))
    goto err;

  if (new_handler->ha_open(table, name,
                           table->db_stat,
                           HA_OPEN_IGNORE_IF_LOCKED | HA_OPEN_NO_PSI_CALL))
    goto err;

  /* Reuse original storage engine data for duplicate key reference */
  new_handler->ref=        file->ref;
  new_handler->ref_length= file->ref_length;
  new_handler->dup_ref=    file->dup_ref;

  DBUG_RETURN((handler*) new_handler);

err:
  delete new_handler;
  DBUG_RETURN(NULL);
}


/*
  Map the create table to the original storage engine
*/

int ha_sequence::create(const char *name, TABLE *form,
                        HA_CREATE_INFO *create_info)
{
  DBUG_ASSERT(create_info->sequence);
  /* Sequence tables has one and only one row */
  create_info->max_rows= create_info->min_rows= 1;
  return (file->create(name, form, create_info));
}

/**
  Sequence write row method.

  A sequence table has only one row. Any inserts in the table
  will update this row.

  @retval 0     Success
  @retval != 0  Failure

  NOTES:
    sequence_locked is set if we are called from SEQUENCE::next_value
    In this case the mutex is already locked and we should not update
    the sequence with 'buf' as the sequence object is already up to date.
*/

int ha_sequence::write_row(uchar *buf)
{
  int error;
  DBUG_ENTER("ha_sequence::write_row");
  DBUG_ASSERT(table->record[0] == buf);

  row_already_logged= 0;
  if (!sequence->initialized)
  {
    /* This calls is from ha_open() as part of create table */
    DBUG_RETURN(file->write_row(buf));
  }

  /*
    User tries to write a row
    - Check that row is an accurate object
    - Update the first row in the table
  */

  sequence_definition tmp_seq;
  tmp_seq.read_fields(table);
  if (tmp_seq.check_and_adjust())
    DBUG_RETURN(HA_ERR_SEQUENCE_INVALID_DATA);

  /*
    Lock sequence to ensure that no one can come in between
    while sequence, table and binary log are updated.
  */
  if (!sequence_locked)                         // If not from next_value()
    sequence->lock();

  if (!(error= file->update_first_row(buf)))
  {
    Log_func *log_func= Write_rows_log_event::binlog_row_logging_function;
    if (!sequence_locked)
      sequence->copy(&tmp_seq);
    rows_changed++;
    /* We have to do the logging while we hold the sequence mutex */
    error= binlog_log_row(table, 0, buf, log_func);
    row_already_logged= 1;
  }

  sequence->all_values_used= 0;
  if (!sequence_locked)
    sequence->unlock();
  DBUG_RETURN(error);
}


int ha_sequence::update_row(const uchar *old_data, const uchar *new_data)
{
  int error;
  sequence_definition tmp_seq;
  DBUG_ENTER("ha_sequence::update_row");
  DBUG_ASSERT(new_data == table->record[0]);

  row_already_logged= 0;

  tmp_seq.read_fields(table);
  if (tmp_seq.check_and_adjust())
    DBUG_RETURN(HA_ERR_SEQUENCE_INVALID_DATA);

  /*
    Lock sequence to ensure that no one can come in between
    while sequence, table and binary log is updated.
  */
  sequence->lock();
  if (!(error= file->update_row(old_data, new_data)))
  {
    sequence->copy(&tmp_seq);
    rows_changed++;
    /* We have to do the logging while we hold the sequence mutex */
    error= binlog_log_row(table, old_data, new_data,
                          Update_rows_log_event::binlog_row_logging_function);
    row_already_logged= 1;
  }
  sequence->all_values_used= 0;
  sequence->unlock();
  DBUG_RETURN(error);
}


/*
  Inherit the sequence base table flags.
*/

handler::Table_flags ha_sequence::table_flags() const
{
  DBUG_ENTER("ha_sequence::table_flags");
  DBUG_RETURN((file->table_flags() & ~SEQUENCE_DISABLED_TABLE_FLAGS) |
              SEQUENCE_ENABLED_TABLE_FLAGS);
}


int ha_sequence::info(uint flag)
{
  DBUG_ENTER("ha_sequence::info");
  file->info(flag);
  /* Inform optimizer that we have always only one record */
  stats= file->stats;
  stats.records= 1;
  DBUG_RETURN(false);
}

int ha_sequence::external_lock(THD *thd, int lock_type)
{
  int error= file->external_lock(thd, lock_type);

  /*
    Copy lock flag to satisfy DBUG_ASSERT checks in ha_* functions in
    handler.cc when we later call it with file->ha_..()
  */
  file->m_lock_type= lock_type;
  return error;
}

/*
  Squence engine error deal method
*/

void ha_sequence::print_error(int error, myf errflag)
{
  char *sequence_db=   table_share->db.str;
  char *sequence_name= table_share->table_name.str;
  DBUG_ENTER("ha_sequence::print_error");

  switch (error) {
  case HA_ERR_SEQUENCE_INVALID_DATA:
  {
    my_error(ER_SEQUENCE_INVALID_DATA, MYF(errflag), sequence_db,
             sequence_name);
    DBUG_VOID_RETURN;
  }
  case HA_ERR_SEQUENCE_RUN_OUT:
  {
    my_error(ER_SEQUENCE_RUN_OUT, MYF(errflag), sequence_db, sequence_name);
    DBUG_VOID_RETURN;
  }
  case HA_ERR_WRONG_COMMAND:
    my_error(ER_ILLEGAL_HA, MYF(0), "SEQUENCE", table_share->db.str,
             table_share->table_name.str);
    DBUG_VOID_RETURN;
  }
  file->print_error(error, errflag);
  DBUG_VOID_RETURN;
}

/*****************************************************************************
  Sequence plugin interface
*****************************************************************************/

/*
 Create an new handler
*/

static handler *sequence_create_handler(handlerton *hton,
                                        TABLE_SHARE *share,
                                        MEM_ROOT *mem_root)
{
  DBUG_ENTER("sequence_create_handler");
  DBUG_RETURN(new (mem_root) ha_sequence(hton, share));
}


/*
  Sequence engine end.

  SYNOPSIS
    sequence_end()
    p                           handlerton.
    type                        panic type.
  RETURN VALUES
    0           Success
    !=0         Failure
*/
static int sequence_end(handlerton* hton,
                        ha_panic_function type __attribute__((unused)))
{
  DBUG_ENTER("sequence_end");
  DBUG_RETURN(0);
}


/*
  Sequence engine init.

  SYNOPSIS
    sequence_initialize()

    @param p    handlerton.

    retval 0    Success
    retval !=0  Failure
*/

static int sequence_initialize(void *p)
{
  handlerton *local_sequence_hton= (handlerton *)p;
  DBUG_ENTER("sequence_initialize");

  local_sequence_hton->state= SHOW_OPTION_YES;
  local_sequence_hton->db_type= DB_TYPE_SEQUENCE;
  local_sequence_hton->create= sequence_create_handler;
  local_sequence_hton->panic= sequence_end;
  local_sequence_hton->flags= (HTON_NOT_USER_SELECTABLE |
                               HTON_HIDDEN |
                               HTON_TEMPORARY_NOT_SUPPORTED |
                               HTON_ALTER_NOT_SUPPORTED |
                               HTON_NO_PARTITION);
  DBUG_RETURN(0);
}


static struct st_mysql_storage_engine sequence_storage_engine=
{ MYSQL_HANDLERTON_INTERFACE_VERSION };

maria_declare_plugin(sql_sequence)
{
  MYSQL_STORAGE_ENGINE_PLUGIN,
  &sequence_storage_engine,
  "SQL_SEQUENCE",
  "jianwei.zhao @ Aliyun & Monty @ MariaDB corp",
  "Sequence Storage Engine for CREATE SEQUENCE",
  PLUGIN_LICENSE_GPL,
  sequence_initialize,        /* Plugin Init */
  NULL,                       /* Plugin Deinit */
  0x0100,                     /* 1.0 */
  NULL,                       /* status variables                */
  NULL,                       /* system variables                */
  "1.0",                      /* string version                  */
  MariaDB_PLUGIN_MATURITY_ALPHA /* maturity                     */
}
maria_declare_plugin_end;
