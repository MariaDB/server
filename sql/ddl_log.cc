/*
   Copyright (c) 2000, 2019, Oracle and/or its affiliates.
   Copyright (c) 2010, 2020, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA
*/

#include "mariadb.h"
#include "mysqld.h"
#include "sql_class.h"                          // init_sql_alloc()
#include "log.h"                                // sql_print_error()
#include "ddl_log.h"
#include "ha_partition.h"                       // PAR_EXT
#include "sql_table.h"                          // build_table_filename
#include "sql_statistics.h"                     // rename_table_in_stats_tables
#include "sql_view.h"                           // mysql_rename_view()
#include "strfunc.h"                            // strconvert
#include "sql_show.h"                           // append_identifier()
#include "sql_db.h"                             // drop_database_objects()
#include <mysys_err.h>                          // EE_LINK


/*--------------------------------------------------------------------------

  MODULE: DDL log
  -----------------

  This module is used to ensure that we can recover from crashes that
  occur in the middle of a meta-data operation in MySQL. E.g. DROP
  TABLE t1, t2; We need to ensure that both t1 and t2 are dropped and
  not only t1 and also that each table drop is entirely done and not
  "half-baked".

  To support this we create log entries for each meta-data statement
  in the ddl log while we are executing. These entries are dropped
  when the operation is completed.

  At recovery those entries that were not completed will be executed.

  There is only one ddl log in the system and it is protected by a mutex
  and there is a global struct that contains information about its current
  state.

  DDl recovery after a crash works the following way:

  - ddl_log_initialize() initializes the global global_ddl_log variable
    and opens the binary log if it exists. If it doesn't exists a new one
    is created.
  - ddl_log_close_binlogged_events() loops over all log events and checks if
    their xid (stored in the EXECUTE_CODE event) is in the binary log.  If xid
    exists in the binary log the entry is marked as finished in the ddl log.
  - After a new binary log is created and is open for new entries,
    ddl_log_execute_recovery() is executed on remaining open events:
    - Loop over all events
     - For each entry with DDL_LOG_ENTRY_CODE execute the remaining phases
       in ddl_log_execute_entry_no_lock()

  The ddl_log.log file is created at startup and deleted when server goes down.
  After the final recovery phase is done, the file is truncated.

  History:
  First version written in 2006 by Mikael Ronstrom
  Second version in 2020 by Monty
--------------------------------------------------------------------------*/

#define DDL_LOG_MAGIC_LENGTH 4

uchar ddl_log_file_magic[]=
{ (uchar) 254, (uchar) 254, (uchar) 11, (uchar) 1 };

/* Action names for ddl_log_action_code */

const char *ddl_log_action_name[DDL_LOG_LAST_ACTION]=
{
  "Unknown", "delete", "rename",
  "replace", "exchange",
  "rename table", "rename view",
  "initialize drop table", "drop table",
  "drop view", "drop trigger", "drop db", "create table", "create view",
  "delete tmp file", "create trigger",
};

/* Number of phases per entry */
const uchar ddl_log_entry_phases[DDL_LOG_LAST_ACTION]=
{
  0, 1, 1, 2,
  (uchar) EXCH_PHASE_END, (uchar) DDL_RENAME_PHASE_END, 1, 1,
  (uchar) DDL_DROP_PHASE_END, 1, 1,
  (uchar) DDL_DROP_DB_PHASE_END, (uchar) DDL_CREATE_TABLE_PHASE_END,
  (uchar) DDL_CREATE_VIEW_PHASE_END, 0, (uchar) DDL_CREATE_TRIGGER_PHASE_END,
};


struct st_global_ddl_log
{
  uchar *file_entry_buf;
  DDL_LOG_MEMORY_ENTRY *first_free;
  DDL_LOG_MEMORY_ENTRY *first_used;
  File file_id;
  uint num_entries;
  uint name_pos;
  uint io_size;
  bool initialized;
  bool open;
};

/* The following structure is only used during startup recovery */
class st_ddl_recovery {
public:
  String drop_table;
  String drop_view;
  size_t drop_table_init_length, drop_view_init_length;
  char   current_db[NAME_LEN];
};

static st_global_ddl_log global_ddl_log;
static st_ddl_recovery   recovery_state;

mysql_mutex_t LOCK_gdl;

/* Positions to different data in a ddl log block */
#define DDL_LOG_ENTRY_TYPE_POS 0 /* 1 byte of ddl_log_entry_code */
/*
  Note that ACTION_TYPE and PHASE_POS must be after each other.
  See update_phase()
*/
#define DDL_LOG_ACTION_TYPE_POS 1 /* 1 byte of ddl_log_action_code */
#define DDL_LOG_PHASE_POS 2
#define DDL_LOG_NEXT_ENTRY_POS 4
#define DDL_LOG_XID_POS 8
#define DDL_LOG_ID_POS 16

/*
  Position to where names are stored in the ddl log blocks. The current
  value is stored in the header and can thus be changed if we need more
  space for constants in the header
*/
#define DDL_LOG_TMP_NAME_POS 48

/* Definitions for the ddl log header, the first block in the file */
/* IO_SIZE is stored in the header and can thus be changed */
#define DDL_LOG_IO_SIZE IO_SIZE

/* Header is stored in positions 0-3 */
#define DDL_LOG_IO_SIZE_POS 4
#define DDL_LOG_NAME_OFFSET_POS 6
/* Sum of the above variables */
#define DDL_LOG_HEADER_SIZE 4+2+2

const LEX_CSTRING file_action= { STRING_WITH_LEN(".file") };

/**
  Sync the ddl log file.

  @return Operation status
    @retval FALSE  Success
    @retval TRUE   Error
*/

static bool ddl_log_sync_file()
{
  DBUG_ENTER("ddl_log_sync_file");
  DBUG_RETURN(mysql_file_sync(global_ddl_log.file_id, MYF(MY_WME)));
}

/* Same as above, but ensure we have the LOCK_gdb locked */

static bool ddl_log_sync_no_lock()
{
  DBUG_ENTER("ddl_log_sync_no_lock");

  mysql_mutex_assert_owner(&LOCK_gdl);
  DBUG_RETURN(ddl_log_sync_file());
}


/**
  Create ddl log file name.
  @param file_name                   Filename setup
*/

static inline void create_ddl_log_file_name(char *file_name)
{
  fn_format(file_name, opt_ddl_recovery_file, mysql_data_home, ".log", 0);
}


/**
  Write ddl log header.

  @return Operation status
    @retval TRUE                      Error
    @retval FALSE                     Success
*/

static bool write_ddl_log_header()
{
  uchar header[DDL_LOG_HEADER_SIZE];
  DBUG_ENTER("write_ddl_log_header");

  memcpy(&header, ddl_log_file_magic, DDL_LOG_MAGIC_LENGTH);
  int2store(&header[DDL_LOG_IO_SIZE_POS],  global_ddl_log.io_size);
  int2store(&header[DDL_LOG_NAME_OFFSET_POS], global_ddl_log.name_pos);

  if (mysql_file_pwrite(global_ddl_log.file_id,
                        header, sizeof(header), 0,
                        MYF(MY_WME | MY_NABP)))
    DBUG_RETURN(TRUE);
  DBUG_RETURN(ddl_log_sync_file());
}


/**
  Read one entry from ddl log file.

  @param entry_pos                     Entry number to read

  @return Operation status
    @retval true   Error
    @retval false  Success
*/

static bool read_ddl_log_file_entry(uint entry_pos)
{
  uchar *file_entry_buf= global_ddl_log.file_entry_buf;
  size_t io_size= global_ddl_log.io_size;
  DBUG_ENTER("read_ddl_log_file_entry");

  mysql_mutex_assert_owner(&LOCK_gdl);
  DBUG_RETURN (mysql_file_pread(global_ddl_log.file_id,
                                file_entry_buf, io_size,
                                io_size * entry_pos,
                                MYF(MY_WME | MY_NABP)));
}


/**
  Write one entry to ddl log file.

  @param entry_pos  Entry number to write

  @return
    @retval true   Error
    @retval false  Success
*/

static bool write_ddl_log_file_entry(uint entry_pos)
{
  bool error= FALSE;
  File file_id= global_ddl_log.file_id;
  uchar *file_entry_buf= global_ddl_log.file_entry_buf;
  DBUG_ENTER("write_ddl_log_file_entry");

  mysql_mutex_assert_owner(&LOCK_gdl);          // To be removed
  DBUG_RETURN(mysql_file_pwrite(file_id, file_entry_buf,
                                global_ddl_log.io_size,
                                global_ddl_log.io_size * entry_pos,
                                MYF(MY_WME | MY_NABP)));
  DBUG_RETURN(error);
}


/**
  Update phase of ddl log entry

  @param entry_pos   ddl_log entry to update
  @param code        Type of entry. Normally DDL_LOG_ENTRY_CODE or
                      DDL_IGNORE_LOG_ENTRY_CODE
  @param phase        New phase

  @return
  @retval 0           ok
  @retval 1           Write error. Error given

 This is done without locks as it's guaranteed to be atomic
*/

static bool update_phase(uint entry_pos, uchar phase)
{
  DBUG_ENTER("update_phase");

  DBUG_RETURN(mysql_file_pwrite(global_ddl_log.file_id, &phase, 1,
                                global_ddl_log.io_size * entry_pos +
                                DDL_LOG_PHASE_POS,
                                MYF(MY_WME | MY_NABP)) ||
              ddl_log_sync_file());
}


static bool update_next_entry_pos(uint entry_pos, uint next_entry)
{
  uchar buff[4];
  DBUG_ENTER("update_next_entry_pos");

  int4store(buff, next_entry);
  DBUG_RETURN(mysql_file_pwrite(global_ddl_log.file_id, buff, sizeof(buff),
                                global_ddl_log.io_size * entry_pos +
                                DDL_LOG_NEXT_ENTRY_POS,
                                MYF(MY_WME | MY_NABP)));
}


static bool update_xid(uint entry_pos, ulonglong xid)
{
  uchar buff[8];
  DBUG_ENTER("update_xid");

  int8store(buff, xid);
  DBUG_RETURN(mysql_file_pwrite(global_ddl_log.file_id, buff, 8,
                                global_ddl_log.io_size * entry_pos +
                                DDL_LOG_XID_POS,
                                MYF(MY_WME | MY_NABP)) ||
              ddl_log_sync_file());
}


/*
  Disable an execute entry

  @param entry_pos  ddl_log entry to update

  Notes:
  We don't need sync here as this is mainly done during
  recover phase to mark already done entries. We instead sync all entries
  at the same time.
*/

static bool disable_execute_entry(uint entry_pos)
{
  uchar buff[1];
  DBUG_ENTER("disable_execute_entry");
  DBUG_PRINT("ddl_log", ("pos: %u", entry_pos));

  buff[0]= DDL_IGNORE_LOG_ENTRY_CODE;
  DBUG_RETURN(mysql_file_pwrite(global_ddl_log.file_id, buff, sizeof(buff),
                                global_ddl_log.io_size * entry_pos +
                                DDL_LOG_ENTRY_TYPE_POS,
                                MYF(MY_WME | MY_NABP)));
}

/*
  Disable an execute entry
*/

bool ddl_log_disable_execute_entry(DDL_LOG_MEMORY_ENTRY **active_entry)
{
  bool res= disable_execute_entry((*active_entry)->entry_pos);
  ddl_log_sync_no_lock();
  return res;
}


/*
  Check if an executive entry is active

  @return 0  Entry is active
  @return 1  Entry is not active
*/

static bool is_execute_entry_active(uint entry_pos)
{
  uchar buff[1];
  DBUG_ENTER("disable_execute_entry");

  if (mysql_file_pread(global_ddl_log.file_id, buff, sizeof(buff),
                       global_ddl_log.io_size * entry_pos +
                       DDL_LOG_ENTRY_TYPE_POS,
                       MYF(MY_WME | MY_NABP)))
    DBUG_RETURN(1);
  DBUG_RETURN(buff[0] == (uchar) DDL_LOG_EXECUTE_CODE);
}


/**
  Read header of ddl log file.

  When we read the ddl log header we get information about maximum sizes
  of names in the ddl log and we also get information about the number
  of entries in the ddl log.

  This is read only once at server startup, so no mutex is needed.

  @return Last entry in ddl log (0 if no entries).
  @return -1 if log could not be opened or could not be read
*/

static int read_ddl_log_header(const char *file_name)
{
  uchar header[DDL_LOG_HEADER_SIZE];
  int max_entry;
  int file_id;
  uint io_size;
  DBUG_ENTER("read_ddl_log_header");

  if ((file_id= mysql_file_open(key_file_global_ddl_log,
                                               file_name,
                                               O_RDWR | O_BINARY, MYF(0))) < 0)
    DBUG_RETURN(-1);

  if (mysql_file_read(file_id,
                      header, sizeof(header), MYF(MY_WME | MY_NABP)))
  {
    /* Write message into error log */
    sql_print_error("Failed to read ddl log file '%s' during recovery",
                    file_name);
    goto err;
  }

  if (memcmp(header, ddl_log_file_magic, 4))
  {
    /* Probably upgrade from MySQL 10.5 or earlier */
    sql_print_warning("Wrong header in %s.  Assuming it is an old recovery file "
                      "from MariaDB 10.5 or earlier. Skipping DDL recovery",
                      file_name);
    goto err;
  }

  io_size=  uint2korr(&header[DDL_LOG_IO_SIZE_POS]);
  global_ddl_log.name_pos= uint2korr(&header[DDL_LOG_NAME_OFFSET_POS]);

  max_entry= (uint) (mysql_file_seek(file_id, 0L, MY_SEEK_END, MYF(0)) /
                     io_size);
  if (max_entry)
    max_entry--;                                // Don't count first block

  if (!(global_ddl_log.file_entry_buf= (uchar*)
        my_malloc(key_memory_DDL_LOG_MEMORY_ENTRY, io_size,
                  MYF(MY_WME | MY_ZEROFILL))))
    goto err;

  global_ddl_log.open= TRUE;
  global_ddl_log.file_id= file_id;
  global_ddl_log.num_entries= max_entry;
  global_ddl_log.io_size= io_size;
  DBUG_RETURN(max_entry);

err:
  if (file_id >= 0)
    my_close(file_id, MYF(0));
  /* We return -1 to force the ddl log to be re-created */
  DBUG_RETURN(-1);
}


/*
  Store and read strings in ddl log buffers

  Format is:
    2 byte: length (not counting end \0)
    X byte: string value of length 'length'
    1 byte: \0
*/

static uchar *store_string(uchar *pos, uchar *end, const LEX_CSTRING *str)
{
  uint32 length= (uint32) str->length;
  if (unlikely(pos + 2 + length +1 >= end))
  {
    DBUG_ASSERT(0);
    return end;                                 // Overflow
  }

  int2store(pos, length);
  if (likely(length))
    memcpy(pos+2, str->str, length);
  pos[2+length]= 0;                             // Store end \0
  return pos + 2 + length +1;
}


static LEX_CSTRING get_string(uchar **pos, const uchar *end)
{
  LEX_CSTRING tmp;
  uint32 length;
  if (likely(*pos + 3 < end))
  {
    length= uint2korr(*pos);
    if (likely(*pos + 2 + length +1 < end))
    {
      char *str= (char*) *pos+2;
      *pos= *pos + 2 + length + 1;
      tmp.str= str;
      tmp.length= length;
      return tmp;
    }
  }
  /*
    Overflow on read, should never happen
    Set *pos to end to ensure any future calls also returns empty string
  */
  DBUG_ASSERT(0);
  *pos= (uchar*) end;
  tmp.str= "";
  tmp.length= 0;
  return tmp;
}


/**
  Convert from ddl_log_entry struct to file_entry_buf binary blob.

  @param ddl_log_entry   filled in ddl_log_entry struct.
*/

static void set_global_from_ddl_log_entry(const DDL_LOG_ENTRY *ddl_log_entry)
{
  uchar *file_entry_buf= global_ddl_log.file_entry_buf, *pos, *end;

  mysql_mutex_assert_owner(&LOCK_gdl);
  DBUG_ASSERT(ddl_log_entry->entry_type == DDL_LOG_ENTRY_CODE ||
              ddl_log_entry->entry_type == DDL_TRY_LOG_ENTRY_CODE);


  file_entry_buf[DDL_LOG_ENTRY_TYPE_POS]=  (uchar) ddl_log_entry->entry_type;
  file_entry_buf[DDL_LOG_ACTION_TYPE_POS]= (uchar) ddl_log_entry->action_type;
  file_entry_buf[DDL_LOG_PHASE_POS]=       (uchar) ddl_log_entry->phase;
  int4store(file_entry_buf+DDL_LOG_NEXT_ENTRY_POS, ddl_log_entry->next_entry);
  int8store(file_entry_buf+DDL_LOG_XID_POS,  ddl_log_entry->xid);
  int8store(file_entry_buf+DDL_LOG_ID_POS,  ddl_log_entry->unique_id);
  bzero(file_entry_buf+DDL_LOG_ID_POS+8,
        global_ddl_log.name_pos - DDL_LOG_ID_POS - 8);

  pos= file_entry_buf + global_ddl_log.name_pos;
  end= file_entry_buf + global_ddl_log.io_size;

  pos= store_string(pos, end, &ddl_log_entry->handler_name);
  pos= store_string(pos, end, &ddl_log_entry->db);
  pos= store_string(pos, end, &ddl_log_entry->name);
  pos= store_string(pos, end, &ddl_log_entry->from_db);
  pos= store_string(pos, end, &ddl_log_entry->from_name);
  pos= store_string(pos, end, &ddl_log_entry->tmp_name);
  bzero(pos, global_ddl_log.io_size - (pos - file_entry_buf));
}


/*
  Calculate how much space we have left in the log entry for a string

  This can be used to check if we have space to store the query string
  in the block
*/

static size_t ddl_log_free_space_in_entry(const DDL_LOG_ENTRY *ddl_log_entry)
{
  size_t length= global_ddl_log.name_pos + 3*6;   // 3 byte per string below
  length+= ddl_log_entry->handler_name.length;
  length+= ddl_log_entry->db.length;
  length+= ddl_log_entry->name.length;
  length+= ddl_log_entry->from_db.length;
  length+= ddl_log_entry->from_name.length;
  length+= ddl_log_entry->tmp_name.length;
  return global_ddl_log.io_size - length - 3;   // 3 is for storing next string
}


/**
  Convert from file_entry_buf binary blob to ddl_log_entry struct.

  @param[out] ddl_log_entry   struct to fill in.

  @note Strings (names) are pointing to the global_ddl_log structure,
  so LOCK_gdl needs to be hold until they are read or copied.
*/

static void set_ddl_log_entry_from_global(DDL_LOG_ENTRY *ddl_log_entry,
                                          const uint read_entry)
{
  uchar *file_entry_buf= global_ddl_log.file_entry_buf, *pos;
  const uchar *end= file_entry_buf + global_ddl_log.io_size;
  uchar single_char;

  mysql_mutex_assert_owner(&LOCK_gdl);
  ddl_log_entry->entry_pos= read_entry;
  single_char= file_entry_buf[DDL_LOG_ENTRY_TYPE_POS];
  ddl_log_entry->entry_type= (enum ddl_log_entry_code) single_char;
  single_char= file_entry_buf[DDL_LOG_ACTION_TYPE_POS];
  ddl_log_entry->action_type= (enum ddl_log_action_code) single_char;
  ddl_log_entry->phase= file_entry_buf[DDL_LOG_PHASE_POS];
  ddl_log_entry->next_entry= uint4korr(&file_entry_buf[DDL_LOG_NEXT_ENTRY_POS]);
  ddl_log_entry->xid= uint8korr(file_entry_buf + DDL_LOG_XID_POS);
  ddl_log_entry->unique_id=  uint8korr(file_entry_buf + DDL_LOG_ID_POS);

  pos= file_entry_buf + global_ddl_log.name_pos;
  ddl_log_entry->handler_name= get_string(&pos, end);
  ddl_log_entry->db=           get_string(&pos, end);
  ddl_log_entry->name=         get_string(&pos, end);
  ddl_log_entry->from_db=      get_string(&pos, end);
  ddl_log_entry->from_name=    get_string(&pos, end);
  ddl_log_entry->tmp_name=     get_string(&pos, end);
}


/**
  Read a ddl log entry.

  Read a specified entry in the ddl log.

  @param read_entry               Number of entry to read
  @param[out] entry_info          Information from entry

  @return Operation status
    @retval TRUE                     Error
    @retval FALSE                    Success
*/

static bool read_ddl_log_entry(uint read_entry, DDL_LOG_ENTRY *ddl_log_entry)
{
  DBUG_ENTER("read_ddl_log_entry");

  if (read_ddl_log_file_entry(read_entry))
  {
    sql_print_error("Failed to read entry %u from ddl log", read_entry);
    DBUG_RETURN(TRUE);
  }
  set_ddl_log_entry_from_global(ddl_log_entry, read_entry);
  DBUG_RETURN(FALSE);
}


/**
   Create the ddl log file

  @return Operation status
    @retval TRUE                     Error
    @retval FALSE                    Success
*/

static bool create_ddl_log()
{
  char file_name[FN_REFLEN];
  DBUG_ENTER("create_ddl_log");

  global_ddl_log.open= 0;
  global_ddl_log.num_entries= 0;
  global_ddl_log.name_pos= DDL_LOG_TMP_NAME_POS;

  /*
    Fix file_entry_buf if the old log had a different io_size or if open of old
    log didn't succeed.
  */
  if (global_ddl_log.io_size != DDL_LOG_IO_SIZE)
  {
    uchar *ptr= (uchar*)
      my_realloc(key_memory_DDL_LOG_MEMORY_ENTRY,
                 global_ddl_log.file_entry_buf, IO_SIZE,
                 MYF(MY_WME | MY_ALLOW_ZERO_PTR));
    if (ptr)                                    // Resize succeded */
    {
      global_ddl_log.file_entry_buf= ptr;
      global_ddl_log.io_size= IO_SIZE;
    }
    if (!global_ddl_log.file_entry_buf)
      DBUG_RETURN(TRUE);
  }
  DBUG_ASSERT(global_ddl_log.file_entry_buf);
  bzero(global_ddl_log.file_entry_buf, global_ddl_log.io_size);
  create_ddl_log_file_name(file_name);
  if ((global_ddl_log.file_id=
       mysql_file_create(key_file_global_ddl_log,
                         file_name, CREATE_MODE,
                         O_RDWR | O_TRUNC | O_BINARY,
                         MYF(MY_WME | ME_ERROR_LOG))) < 0)
  {
    /* Couldn't create ddl log file, this is serious error */
    sql_print_error("Failed to create ddl log file: %s", file_name);
    my_free(global_ddl_log.file_entry_buf);
    global_ddl_log.file_entry_buf= 0;
    DBUG_RETURN(TRUE);
  }
  if (write_ddl_log_header())
  {
    (void) mysql_file_close(global_ddl_log.file_id, MYF(MY_WME));
    my_free(global_ddl_log.file_entry_buf);
    global_ddl_log.file_entry_buf= 0;
    DBUG_RETURN(TRUE);
  }
  global_ddl_log.open= TRUE;
  DBUG_RETURN(FALSE);
}


/**
  Open ddl log and initialise ddl log variables
*/

bool ddl_log_initialize()
{
  int num_entries;
  char file_name[FN_REFLEN];
  DBUG_ENTER("ddl_log_initialize");

  bzero(&global_ddl_log, sizeof(global_ddl_log));
  global_ddl_log.file_id= (File) -1;
  global_ddl_log.initialized= 1;

  mysql_mutex_init(key_LOCK_gdl, &LOCK_gdl, MY_MUTEX_INIT_SLOW);

  create_ddl_log_file_name(file_name);
  if (likely((num_entries= read_ddl_log_header(file_name)) < 0))
  {
    /* Fatal error, log not opened. Recreate it */
    if (create_ddl_log())
      DBUG_RETURN(1);
  }
  else
    global_ddl_log.num_entries= (uint) num_entries;
  DBUG_RETURN(0);
}


/**
  @brief Deactivate an individual entry.

  @details For complex rename operations we need to deactivate individual
  entries.

  During replace operations where we start with an existing table called
  t1 and a replacement table called t1#temp or something else and where
  we want to delete t1 and rename t1#temp to t1 this is not possible to
  do in a safe manner unless the ddl log is informed of the phases in
  the change.

  Delete actions are 1-phase actions that can be ignored immediately after
  being executed.
  Rename actions from x to y is also a 1-phase action since there is no
  interaction with any other handlers named x and y.
  Replace action where drop y and x -> y happens needs to be a two-phase
  action. Thus the first phase will drop y and the second phase will
  rename x -> y.

  @param entry_pos     Entry position of record to change

  @return Operation status
    @retval TRUE      Error
    @retval FALSE     Success
*/

static bool ddl_log_increment_phase_no_lock(uint entry_pos)
{
  uchar *file_entry_buf= global_ddl_log.file_entry_buf;
  DBUG_ENTER("ddl_log_increment_phase_no_lock");

  mysql_mutex_assert_owner(&LOCK_gdl);
  if (!read_ddl_log_file_entry(entry_pos))
  {
    ddl_log_entry_code  code=   ((ddl_log_entry_code)
                                 file_entry_buf[DDL_LOG_ENTRY_TYPE_POS]);
    ddl_log_action_code action= ((ddl_log_action_code)
                                 file_entry_buf[DDL_LOG_ACTION_TYPE_POS]);

    if ((code == DDL_LOG_ENTRY_CODE || code == DDL_TRY_LOG_ENTRY_CODE) &&
        action < (uint) DDL_LOG_LAST_ACTION)
    {
      /*
        Log entry:
        Increase the phase by one. If complete mark it done (IGNORE).
      */
      char phase= file_entry_buf[DDL_LOG_PHASE_POS]+ 1;
      if (ddl_log_entry_phases[action] <= phase)
      {
        DBUG_ASSERT(phase == ddl_log_entry_phases[action]);
        /* Same effect as setting DDL_IGNORE_LOG_ENTRY_CODE */
        phase= DDL_LOG_FINAL_PHASE;
      }
      file_entry_buf[DDL_LOG_PHASE_POS]= phase;
      if (update_phase(entry_pos, phase))
        DBUG_RETURN(TRUE);
    }
    else
    {
      /*
        Trying to deativate an execute entry or already deactive entry.
        This should not happen
      */
      DBUG_ASSERT(0);
    }
  }
  else
  {
    sql_print_error("Failed in reading entry before updating it");
    DBUG_RETURN(TRUE);
  }
  DBUG_RETURN(FALSE);
}


/*
  Ignore errors from the file system about:
  - Non existing tables or file (from drop table or delete file)
  - Error about tables files that already exists.
  - Error from delete table (from Drop_table_error_handler)
  - Wrong trigger definer   (from Drop_table_error_handler)
*/

class ddl_log_error_handler : public Internal_error_handler
{
public:
  int handled_errors;
  int unhandled_errors;
  int first_error;

  ddl_log_error_handler() : handled_errors(0), unhandled_errors(0),
                            first_error(0)
  {}

  bool handle_condition(THD *thd,
                        uint sql_errno,
                        const char* sqlstate,
                        Sql_condition::enum_warning_level *level,
                        const char* msg,
                        Sql_condition ** cond_hdl)
  {
    *cond_hdl= NULL;
    if (non_existing_table_error(sql_errno) || sql_errno == EE_LINK ||
        sql_errno == EE_DELETE || sql_errno == ER_TRG_NO_DEFINER)
    {
      handled_errors++;
      return TRUE;
    }
    if (!first_error)
      first_error= sql_errno;

    if (*level == Sql_condition::WARN_LEVEL_ERROR)
      unhandled_errors++;
    return FALSE;
  }

  bool safely_trapped_errors()
  {
    return (handled_errors > 0 && unhandled_errors == 0);
  }
};


/*
  Build a filename for a table, trigger file or .frm
  Delete also any temporary file suffixed with ~

  @return 0  Temporary file deleted
  @return 1  No temporary file found
*/

static bool build_filename_and_delete_tmp_file(char *path, size_t path_length,
                                               const LEX_CSTRING *db,
                                               const LEX_CSTRING *name,
                                               const char *ext,
                                               PSI_file_key psi_key)
{
  bool deleted;
  uint length= build_table_filename(path, path_length-1,
                                    db->str, name->str, ext, 0);
  path[length]= '~';
  path[length+1]= 0;
  deleted= mysql_file_delete(psi_key, path, MYF(0)) != 0;
  path[length]= 0;
  return deleted;
}


static LEX_CSTRING end_comment=
{ STRING_WITH_LEN(" /* generated by ddl log */")};


/**
  Log DROP query to binary log with comment

  This function is only run during recovery
*/

static void ddl_log_to_binary_log(THD *thd, String *query)
{
  LEX_CSTRING thd_db= thd->db;

  lex_string_set(&thd->db, recovery_state.current_db);
  query->length(query->length()-1);             // Removed end ','
  query->append(&end_comment);
  mysql_mutex_unlock(&LOCK_gdl);
  (void) thd->binlog_query(THD::STMT_QUERY_TYPE,
                           query->ptr(), query->length(),
                           TRUE, FALSE, FALSE, 0);
  mysql_mutex_lock(&LOCK_gdl);
  thd->db= thd_db;
}


/**
   Log DROP TABLE/VIEW to binary log when needed

   @result 0  Nothing was done
   @result 1  Query was logged to binary log & query was reset

   Logging happens in the following cases
   - This is the last DROP entry
   - The query could be longer than max_packet_length if we would add another
     table name to the query

   When we log, we always log all found tables and views at the same time. This
   is done to simply the exceute code as otherwise we would have to keep
   information of what was logged.
*/

static bool ddl_log_drop_to_binary_log(THD *thd, DDL_LOG_ENTRY *ddl_log_entry,
                                  String *query)
{
  DBUG_ENTER("ddl_log_binary_log");
  if (mysql_bin_log.is_open())
  {
    if (!ddl_log_entry->next_entry ||
        query->length() + end_comment.length + NAME_LEN + 100 >
        thd->variables.max_allowed_packet)
    {
      if (recovery_state.drop_table.length() >
          recovery_state.drop_table_init_length)
      {
        ddl_log_to_binary_log(thd, &recovery_state.drop_table);
        recovery_state.drop_table.length(recovery_state.drop_table_init_length);
      }
      if (recovery_state.drop_view.length() >
          recovery_state.drop_view_init_length)
      {
        ddl_log_to_binary_log(thd, &recovery_state.drop_view);
        recovery_state.drop_view.length(recovery_state.drop_view_init_length);
      }
      DBUG_RETURN(1);
    }
  }
  DBUG_RETURN(0);
}


/**
  Execute one action in a ddl log entry

  @param ddl_log_entry              Information in action entry to execute

  @return Operation status
    @retval TRUE                       Error
    @retval FALSE                      Success
*/

static int ddl_log_execute_action(THD *thd, MEM_ROOT *mem_root,
                                  DDL_LOG_ENTRY *ddl_log_entry)
{
  LEX_CSTRING handler_name;
  handler *file= NULL;
  char to_path[FN_REFLEN+1], from_path[FN_REFLEN+1];
  handlerton *hton= 0;
  ddl_log_error_handler no_such_table_handler;
  uint entry_pos= ddl_log_entry->entry_pos;
  int error;
  enum
  {
    ACT_HANDLER,
    ACT_PARTITION,
    ACT_FILE
  } frm_action= ACT_HANDLER;
  DBUG_ENTER("ddl_log_execute_action");

  mysql_mutex_assert_owner(&LOCK_gdl);
  DBUG_PRINT("ddl_log",
             ("pos: %u->%u  entry: %u  action: %u (%s) phase: %u  "
              "handler: '%s'  name: '%s'  from_name: '%s'  tmp_name: '%s'",
              ddl_log_entry->entry_pos,
              ddl_log_entry->next_entry,
              (uint) ddl_log_entry->entry_type,
              (uint) ddl_log_entry->action_type,
              ddl_log_action_name[ddl_log_entry->action_type],
              (uint) ddl_log_entry->phase,
              ddl_log_entry->handler_name.str,
              ddl_log_entry->name.str,
              ddl_log_entry->from_name.str,
              ddl_log_entry->tmp_name.str));

  if (ddl_log_entry->entry_type == DDL_IGNORE_LOG_ENTRY_CODE ||
      ddl_log_entry->phase == DDL_LOG_FINAL_PHASE)
    DBUG_RETURN(FALSE);

  handler_name=    ddl_log_entry->handler_name;
  thd->push_internal_handler(&no_such_table_handler);

  if (!strcmp(ddl_log_entry->handler_name.str, reg_ext))
    frm_action= ACT_PARTITION;
  else if (!strcmp(ddl_log_entry->handler_name.str, file_action.str))
    frm_action= ACT_FILE;
  else if (ddl_log_entry->handler_name.length)
  {
    plugin_ref plugin= my_plugin_lock_by_name(thd, &handler_name,
                                              MYSQL_STORAGE_ENGINE_PLUGIN);
    if (!plugin)
    {
      my_error(ER_UNKNOWN_STORAGE_ENGINE, MYF(0), ddl_log_entry->handler_name);
      goto end;
    }
    hton= plugin_hton(plugin);
    file= get_new_handler((TABLE_SHARE*)0, mem_root, hton);
    if (unlikely(!file))
      goto end;
  }

  switch (ddl_log_entry->action_type) {
  case DDL_LOG_REPLACE_ACTION:
  {
    if (ddl_log_entry->phase == 0 && frm_action != ACT_HANDLER)
    {
      /* If new file doesn't exist or a special file keep an old one. */
      const char *from_name;
      MY_STAT stat_info;
      if (frm_action == ACT_PARTITION)
      {
        strxmov(from_path, ddl_log_entry->from_name.str, reg_ext, NullS);
        from_name= from_path;
      }
      else
        from_name= ddl_log_entry->from_name.str;
      if (!mysql_file_stat(key_file_frm, from_name, &stat_info, MYF(0)))
        break;
      if (!MY_S_ISREG(stat_info.st_mode) && !MY_S_ISLNK(stat_info.st_mode))
        break;
    }
  }
  /* fall through */
  case DDL_LOG_DELETE_ACTION:
  {
    if (ddl_log_entry->phase == 0)
    {
      if (frm_action != ACT_HANDLER)
      {
        if (frm_action == ACT_PARTITION)
        {
          strxmov(to_path, ddl_log_entry->name.str, reg_ext, NullS);
          if (unlikely((error= mysql_file_delete(key_file_frm, to_path,
                                                 MYF(MY_WME |
                                                     MY_IGNORE_ENOENT)))))
            break;
#ifdef WITH_PARTITION_STORAGE_ENGINE
          strxmov(to_path, ddl_log_entry->name.str, PAR_EXT, NullS);
          (void) mysql_file_delete(key_file_partition_ddl_log, to_path, MYF(0));
#endif
        } // if (ACT_PARTITION)
        else
        {
          DBUG_ASSERT(frm_action == ACT_FILE);
          if (unlikely((error= mysql_file_delete(key_file_frm, ddl_log_entry->name.str,
                                                 MYF(MY_WME |
                                                     MY_IGNORE_ENOENT)))))
            break;
        } // if (ACT_FILE)
      }
      else
      {
        if (unlikely((error= hton->drop_table(hton, ddl_log_entry->name.str))))
        {
          if (!non_existing_table_error(error))
            break;
        }
      } // if (ACT_HANDLER)
      if (ddl_log_increment_phase_no_lock(entry_pos))
        break;
      (void) ddl_log_sync_no_lock();
      error= 0;
      if (ddl_log_entry->action_type == DDL_LOG_DELETE_ACTION)
        break;
    }
  }
  DBUG_ASSERT(ddl_log_entry->action_type == DDL_LOG_REPLACE_ACTION);
  /*
    Fall through and perform the rename action of the replace
    action. We have already indicated the success of the delete
    action in the log entry by stepping up the phase.
  */
  /* fall through */
  case DDL_LOG_RENAME_ACTION:
  {
    error= TRUE;
    if (frm_action != ACT_HANDLER)
    {
      if (frm_action == ACT_PARTITION)
      {
        strxmov(to_path, ddl_log_entry->name.str, reg_ext, NullS);
        strxmov(from_path, ddl_log_entry->from_name.str, reg_ext, NullS);
        (void) mysql_file_rename(key_file_frm, from_path, to_path, MYF(MY_WME));
#ifdef WITH_PARTITION_STORAGE_ENGINE
        strxmov(to_path, ddl_log_entry->name.str, PAR_EXT, NullS);
        strxmov(from_path, ddl_log_entry->from_name.str, PAR_EXT, NullS);
        (void) mysql_file_rename(key_file_partition_ddl_log, from_path, to_path,
                                 MYF(MY_WME));
#endif
      } // if (ACT_PARTITION)
      else
      {
        DBUG_ASSERT (frm_action == ACT_FILE);
        if (mysql_file_rename(key_file_frm, ddl_log_entry->from_name.str,
                              ddl_log_entry->name.str, MYF(MY_WME)))
          break;
      } // if (ACT_FILE)
    }
    else
      (void) file->ha_rename_table(ddl_log_entry->from_name.str,
                                   ddl_log_entry->name.str);
    if (ddl_log_increment_phase_no_lock(entry_pos))
      break;
    (void) ddl_log_sync_no_lock();
    break;
  }
  case DDL_LOG_EXCHANGE_ACTION:
  {
    /* We hold LOCK_gdl, so we can alter global_ddl_log.file_entry_buf */
    uchar *file_entry_buf= global_ddl_log.file_entry_buf;
    /* not yet implemented for frm */
    DBUG_ASSERT(!frm_action);
    /*
      Using a case-switch here to revert all currently done phases,
      since it will fall through until the first phase is undone.
    */
    switch (ddl_log_entry->phase) {
    case EXCH_PHASE_TEMP_TO_FROM:
      /* tmp_name -> from_name possibly done */
      (void) file->ha_rename_table(ddl_log_entry->from_name.str,
                                   ddl_log_entry->tmp_name.str);
      /* decrease the phase and sync */
      file_entry_buf[DDL_LOG_PHASE_POS]--;
      if (write_ddl_log_file_entry(entry_pos))
        break;
      (void) ddl_log_sync_no_lock();
      /* fall through */
    case EXCH_PHASE_FROM_TO_NAME:
      /* from_name -> name possibly done */
      (void) file->ha_rename_table(ddl_log_entry->name.str,
                                   ddl_log_entry->from_name.str);
      /* decrease the phase and sync */
      file_entry_buf[DDL_LOG_PHASE_POS]--;
      if (write_ddl_log_file_entry(entry_pos))
        break;
      (void) ddl_log_sync_no_lock();
      /* fall through */
    case EXCH_PHASE_NAME_TO_TEMP:
      /* name -> tmp_name possibly done */
      (void) file->ha_rename_table(ddl_log_entry->tmp_name.str,
                                    ddl_log_entry->name.str);
      /* disable the entry and sync */
      file_entry_buf[DDL_LOG_ENTRY_TYPE_POS]= DDL_IGNORE_LOG_ENTRY_CODE;
      (void) write_ddl_log_file_entry(entry_pos);
      (void) ddl_log_sync_no_lock();
      break;
    }
    break;
  }
  case DDL_LOG_RENAME_TABLE_ACTION:
  {
    /*
      We should restore things by renaming from
      'entry->name' to 'entry->from_name'

      In the following code 'to_' stands for what the table was renamed to
      that we have to rename back.
    */
    size_t fr_length, to_length;
    LEX_CSTRING from_table, to_table, to_converted_name;
    from_table= ddl_log_entry->from_name;
    to_table= ddl_log_entry->name;

    /* Some functions wants to have the lower case table name as an argument */
    if (lower_case_table_names)
    {
      uint errors;
      to_converted_name.str= to_path;
      to_converted_name.length=
        strconvert(system_charset_info, to_table.str, to_table.length,
                   files_charset_info, from_path, FN_REFLEN, &errors);
    }
    else
      to_converted_name= to_table;

    switch (ddl_log_entry->phase) {
    case DDL_RENAME_PHASE_TRIGGER:
    {
      MDL_request mdl_request;

      build_filename_and_delete_tmp_file(to_path, sizeof(to_path),
                                         &ddl_log_entry->db,
                                         &ddl_log_entry->name,
                                         TRG_EXT,
                                         key_file_trg);
      build_filename_and_delete_tmp_file(from_path, sizeof(from_path),
                                         &ddl_log_entry->from_db,
                                         &ddl_log_entry->from_name,
                                         TRG_EXT, key_file_trg);

      if (!access(from_path, F_OK))
      {
        /*
          The original file was never renamed or we crashed in recovery
          just after renaming back the file.
          In this case the current file is correct and we can remove any
          left over copied files
        */
        (void) mysql_file_delete(key_file_trg, to_path, MYF(0));
      }
      else if (!access(to_path, F_OK))
      {
        /* .TRG file was renamed. Rename it back */
        /*
          We have to create a MDL lock as change_table_names() checks that we
          have a mdl locks for the table
        */
        MDL_REQUEST_INIT(&mdl_request, MDL_key::TABLE,
                         ddl_log_entry->db.str,
                         to_converted_name.str,
                         MDL_EXCLUSIVE, MDL_EXPLICIT);
        error= thd->mdl_context.acquire_lock(&mdl_request, 1);
        /* acquire_locks() should never fail during recovery */
        DBUG_ASSERT(error == 0);

        (void) Table_triggers_list::change_table_name(thd,
                                                      &ddl_log_entry->db,
                                                      &to_table,
                                                      &to_converted_name,
                                                      &ddl_log_entry->from_db,
                                                      &from_table);

        thd->mdl_context.release_lock(mdl_request.ticket);
      }
      if (ddl_log_increment_phase_no_lock(entry_pos))
        break;
      (void) ddl_log_sync_no_lock();
    }
    /* fall through */
    case DDL_RENAME_PHASE_STAT:
    {
      (void) rename_table_in_stat_tables(thd,
                                         &ddl_log_entry->db,
                                         &to_converted_name,
                                         &ddl_log_entry->from_db,
                                         &from_table);
      if (ddl_log_increment_phase_no_lock(entry_pos))
        break;
      (void) ddl_log_sync_no_lock();
    }
    /* fall through */
    case DDL_RENAME_PHASE_TABLE:
      /* Restore frm and table to original names */
      to_length= build_table_filename(to_path, sizeof(to_path) - 1,
                                      ddl_log_entry->db.str,
                                      ddl_log_entry->name.str,
                                      reg_ext, 0);
      fr_length= build_table_filename(from_path, sizeof(from_path) - 1,
                                      ddl_log_entry->from_db.str,
                                      ddl_log_entry->from_name.str,
                                      reg_ext, 0);
      (void) mysql_file_rename(key_file_frm, to_path, from_path, MYF(MY_WME));

      if (file->needs_lower_case_filenames())
      {
        build_lower_case_table_filename(to_path, sizeof(to_path) - 1,
                                        &ddl_log_entry->db,
                                        &to_table, 0);
        build_lower_case_table_filename(from_path, sizeof(from_path) - 1,
                                        &ddl_log_entry->from_db,
                                        &from_table, 0);
      }
      else
      {
        /* remove extension from file name */
        DBUG_ASSERT(to_length != 0 && fr_length != 0);
        to_path[to_length - reg_ext_length]= 0;
        from_path[fr_length - reg_ext_length]= 0;
      }

      file->ha_rename_table(to_path, from_path);
      /* disable the entry and sync */
      (void) update_phase(entry_pos, DDL_LOG_FINAL_PHASE);
      break;
    default:
      DBUG_ASSERT(0);
      break;
    }
    break;
  }
  case DDL_LOG_RENAME_VIEW_ACTION:
  {
    LEX_CSTRING from_table, to_table;
    from_table= ddl_log_entry->from_name;
    to_table=   ddl_log_entry->name;

    /* Delete any left over .frm~ files */
    build_filename_and_delete_tmp_file(to_path, sizeof(to_path) - 1,
                                       &ddl_log_entry->db,
                                       &ddl_log_entry->name,
                                       reg_ext,
                                       key_file_fileparser);
    build_filename_and_delete_tmp_file(from_path, sizeof(from_path) - 1,
                                       &ddl_log_entry->from_db,
                                       &ddl_log_entry->from_name,
                                       reg_ext, key_file_fileparser);

    /* Rename view back if the original rename did succeed */
    if (!access(to_path, F_OK))
      (void) mysql_rename_view(thd,
                               &ddl_log_entry->from_db, &from_table,
                               &ddl_log_entry->db, &to_table);
    (void) update_phase(entry_pos, DDL_LOG_FINAL_PHASE);
  }
  break;
  /*
    Initialize variables for DROP TABLE and DROP VIEW
    In normal cases a query only contains one action. However in case of
    DROP DATABASE we may get a mix of both and we have to keep these
    separate.
  */
  case DDL_LOG_DROP_INIT_ACTION:
  {
    LEX_CSTRING *comment= &ddl_log_entry->tmp_name;
    recovery_state.drop_table.length(0);
    recovery_state.drop_table.set_charset(default_charset_info);
    recovery_state.drop_table.append(STRING_WITH_LEN("DROP TABLE IF EXISTS "));
    if (comment->length)
    {
      recovery_state.drop_table.append(comment);
      recovery_state.drop_table.append(' ');
    }
    recovery_state.drop_table_init_length= recovery_state.drop_table.length();

    recovery_state.drop_view.length(0);
    recovery_state.drop_view.set_charset(default_charset_info);
    recovery_state.drop_view.append(STRING_WITH_LEN("DROP VIEW IF EXISTS "));
    recovery_state.drop_view_init_length= recovery_state.drop_view.length();

    strmake(recovery_state.current_db,
            ddl_log_entry->from_db.str, sizeof(recovery_state.current_db)-1);
    /* We don't increment phase as we want to retry this in case of crash */
    break;
  }
  case DDL_LOG_DROP_TABLE_ACTION:
  {
    LEX_CSTRING db, table, path;
    db=     ddl_log_entry->db;
    table=  ddl_log_entry->name;
    /* Note that path is without .frm extension */
    path=   ddl_log_entry->tmp_name;

    switch (ddl_log_entry->phase) {
    case DDL_DROP_PHASE_TABLE:
      if (hton)
      {
        if ((error= hton->drop_table(hton, path.str)))
        {
          if (!non_existing_table_error(error))
            break;
          error= -1;
        }
      }
      else
        error= ha_delete_table_force(thd, path.str, &db, &table);
      if (error <= 0)
      {
        /* Not found or already deleted. Delete .frm if it exists */
        strxnmov(to_path, sizeof(to_path)-1, path.str, reg_ext, NullS);
        mysql_file_delete(key_file_frm, to_path, MYF(MY_WME|MY_IGNORE_ENOENT));
      }
      if (ddl_log_increment_phase_no_lock(entry_pos))
        break;
      (void) ddl_log_sync_no_lock();
      /* Fall through */
    case DDL_DROP_PHASE_TRIGGER:
      Table_triggers_list::drop_all_triggers(thd, &db, &table,
                                             MYF(MY_WME | MY_IGNORE_ENOENT));
      if (ddl_log_increment_phase_no_lock(entry_pos))
        break;
      (void) ddl_log_sync_no_lock();
      /* Fall through */

    case DDL_DROP_PHASE_COLLECT:
      if (strcmp(recovery_state.current_db, db.str))
      {
        append_identifier(thd, &recovery_state.drop_table, &db);
        recovery_state.drop_table.append('.');
      }
      append_identifier(thd, &recovery_state.drop_table, &table);
      recovery_state.drop_table.append(',');
      /* We don't increment phase as we want to retry this in case of crash */

      if (ddl_log_drop_to_binary_log(thd, ddl_log_entry,
                                     &recovery_state.drop_table))
      {
        if (ddl_log_increment_phase_no_lock(entry_pos))
          break;
      }
      break;
    case DDL_DROP_PHASE_RESET:
      /* We have already logged all previous drop's. Clear the query */
      recovery_state.drop_table.length(recovery_state.drop_table_init_length);
      recovery_state.drop_view.length(recovery_state.drop_view_init_length);
      break;
    }
    break;
  }
  case DDL_LOG_DROP_VIEW_ACTION:
  {
    LEX_CSTRING db, table, path;
    db=     ddl_log_entry->db;
    table=  ddl_log_entry->name;
    /* Note that for views path is WITH .frm extension */
    path=   ddl_log_entry->tmp_name;

    if (ddl_log_entry->phase == 0)
    {
      mysql_file_delete(key_file_frm, path.str, MYF(MY_WME|MY_IGNORE_ENOENT));
      if (strcmp(recovery_state.current_db, db.str))
      {
        append_identifier(thd, &recovery_state.drop_view, &db);
        recovery_state.drop_view.append('.');
      }
      append_identifier(thd, &recovery_state.drop_view, &table);
      recovery_state.drop_view.append(',');

      if (ddl_log_drop_to_binary_log(thd, ddl_log_entry,
                                     &recovery_state.drop_view))
      {
        if (ddl_log_increment_phase_no_lock(entry_pos))
          break;
      }
    }
    else
    {
      /* We have already logged all previous drop's. Clear the query */
      recovery_state.drop_table.length(recovery_state.drop_table_init_length);
      recovery_state.drop_view.length(recovery_state.drop_table_init_length);
    }
    break;
  }
  case DDL_LOG_DROP_TRIGGER_ACTION:
  {
    MY_STAT stat_info;
    off_t frm_length= 1;                        // Impossible length
    LEX_CSTRING thd_db= thd->db;

    /* Delete trigger temporary file if it still exists */
    if (!build_filename_and_delete_tmp_file(to_path, sizeof(to_path) - 1,
                                            &ddl_log_entry->db,
                                            &ddl_log_entry->name,
                                            TRG_EXT,
                                            key_file_fileparser))
    {
      /* Temporary file existed and was deleted, nothing left to do */
      (void) update_phase(entry_pos, DDL_LOG_FINAL_PHASE);
      break;
    }
    /*
      We can use length of TRG file as an indication if trigger was removed.
      If there is no file, then it means that this was the last trigger
      and the file was removed.
     */
    if (my_stat(to_path, &stat_info, MYF(0)))
      frm_length= (off_t) stat_info.st_size;
    if (frm_length != (off_t) ddl_log_entry->unique_id &&
        mysql_bin_log.is_open())
    {
      /*
        File size changed and it was not binlogged (as this entry was
        executed)
      */
      (void) rm_trigname_file(to_path, &ddl_log_entry->db,
                              &ddl_log_entry->from_name,
                              MYF(0));

      recovery_state.drop_table.length(0);
      recovery_state.drop_table.set_charset(default_charset_info);
      if (ddl_log_entry->tmp_name.length)
      {
        /* We can use the original query */
        recovery_state.drop_table.append(&ddl_log_entry->tmp_name);
      }
      else
      {
        /* Generate new query */
        recovery_state.drop_table.append(STRING_WITH_LEN("DROP TRIGGER IF "
                                                         "EXISTS "));
        append_identifier(thd, &recovery_state.drop_table,
                          &ddl_log_entry->from_name);
        recovery_state.drop_table.append(&end_comment);
      }
      mysql_mutex_unlock(&LOCK_gdl);
      thd->db= ddl_log_entry->db;
      (void) thd->binlog_query(THD::STMT_QUERY_TYPE,
                               recovery_state.drop_table.ptr(),
                               recovery_state.drop_table.length(), TRUE, FALSE,
                               FALSE, 0);
      thd->db= thd_db;
      mysql_mutex_lock(&LOCK_gdl);
    }
    (void) update_phase(entry_pos, DDL_LOG_FINAL_PHASE);
    break;
  }
  case DDL_LOG_DROP_DB_ACTION:
  {
    LEX_CSTRING db, path;
    db=     ddl_log_entry->db;
    path=   ddl_log_entry->tmp_name;

    switch (ddl_log_entry->phase) {
    case DDL_DROP_DB_PHASE_INIT:
      drop_database_objects(thd, &path, &db,
                            !my_strcasecmp(system_charset_info,
                                           MYSQL_SCHEMA_NAME.str, db.str));

      strxnmov(to_path, sizeof(to_path)-1, path.str, MY_DB_OPT_FILE, NullS);
      mysql_file_delete_with_symlink(key_file_misc, to_path, "", MYF(0));

      (void) rm_dir_w_symlink(path.str, 0);
      if (ddl_log_increment_phase_no_lock(entry_pos))
        break;
      /* Fall through */
    case DDL_DROP_DB_PHASE_LOG:
    {
      String *query= &recovery_state.drop_table;

      query->length(0);
      query->append(STRING_WITH_LEN("DROP DATABASE IF EXISTS "));
      append_identifier(thd, query, &db);
      query->append(&end_comment);

      mysql_mutex_unlock(&LOCK_gdl);
      (void) thd->binlog_query(THD::STMT_QUERY_TYPE,
                               query->ptr(), query->length(),
                               TRUE, FALSE, FALSE, 0);
      mysql_mutex_lock(&LOCK_gdl);
      (void) update_phase(entry_pos, DDL_LOG_FINAL_PHASE);
      break;
    }
    }
    break;
  }
  case DDL_LOG_CREATE_TABLE_ACTION:
  {
    LEX_CSTRING db, table, path;
    db=     ddl_log_entry->db;
    table=  ddl_log_entry->name;
    path=   ddl_log_entry->tmp_name;

    /* Don't delete the table if we didn't create it */
    if (ddl_log_entry->unique_id == 0)
    {
      if (hton)
      {
        if ((error= hton->drop_table(hton, path.str)))
        {
          if (!non_existing_table_error(error))
            break;
          error= -1;
        }
      }
      else
        error= ha_delete_table_force(thd, path.str, &db, &table);
    }
    strxnmov(to_path, sizeof(to_path)-1, path.str, reg_ext, NullS);
    mysql_file_delete(key_file_frm, to_path, MYF(MY_WME|MY_IGNORE_ENOENT));
    if (ddl_log_entry->phase == DDL_CREATE_TABLE_PHASE_LOG)
    {
      /*
        The server logged CREATE TABLE ... SELECT into binary log
        before crashing. As the commit failed and we have delete the
        table above, we have now to log the DROP of the created table.
      */

      String *query= &recovery_state.drop_table;
      query->length(0);
      query->append(STRING_WITH_LEN("DROP TABLE IF EXISTS "));
      append_identifier(thd, query, &db);
      query->append('.');
      append_identifier(thd, query, &table);
      query->append(&end_comment);

      mysql_mutex_unlock(&LOCK_gdl);
      (void) thd->binlog_query(THD::STMT_QUERY_TYPE,
                               query->ptr(), query->length(),
                               TRUE, FALSE, FALSE, 0);
      mysql_mutex_lock(&LOCK_gdl);
    }
    (void) update_phase(entry_pos, DDL_LOG_FINAL_PHASE);
    break;
  }
  case DDL_LOG_CREATE_VIEW_ACTION:
  {
    char *path= to_path;
    size_t path_length= ddl_log_entry->tmp_name.length;
    memcpy(path, ddl_log_entry->tmp_name.str, path_length+1);
    path[path_length+1]= 0;               // Prepare for extending

    /* Remove temporary parser file */
    path[path_length]='~';
    mysql_file_delete(key_file_fileparser, path,
                      MYF(MY_WME|MY_IGNORE_ENOENT));
    path[path_length]= 0;

    switch (ddl_log_entry->phase) {
    case DDL_CREATE_VIEW_PHASE_NO_OLD_VIEW:
    {
      /*
        No old view exists, so we can just delete the .frm and temporary files
      */
      path[path_length]='-';
      mysql_file_delete(key_file_fileparser, path,
                        MYF(MY_WME|MY_IGNORE_ENOENT));
      path[path_length]= 0;
      mysql_file_delete(key_file_frm, path, MYF(MY_WME|MY_IGNORE_ENOENT));
      break;
    }
    case DDL_CREATE_VIEW_PHASE_DELETE_VIEW_COPY:
    {
      /*
        Old view existed. We crashed before we had done a copy and change
        state to DDL_CREATE_VIEW_PHASE_OLD_VIEW_COPIED
      */
      path[path_length]='-';
      mysql_file_delete(key_file_fileparser, path,
                        MYF(MY_WME|MY_IGNORE_ENOENT));
      path[path_length]= 0;
      break;
    }
    case DDL_CREATE_VIEW_PHASE_OLD_VIEW_COPIED:
    {
      /*
        Old view existed copied to '-' file. Restore it
      */
      memcpy(from_path, path, path_length+2);
      from_path[path_length]='-';
      if (!access(from_path, F_OK))
        mysql_file_rename(key_file_fileparser, from_path, path, MYF(MY_WME));
      break;
    }
    }
    (void) update_phase(entry_pos, DDL_LOG_FINAL_PHASE);
    break;
  }
  case DDL_LOG_DELETE_TMP_FILE_ACTION:
  {
    LEX_CSTRING path= ddl_log_entry->tmp_name;
    DBUG_ASSERT(ddl_log_entry->unique_id <= UINT_MAX32);
    if (!ddl_log_entry->unique_id ||
        !is_execute_entry_active((uint) ddl_log_entry->unique_id))
      mysql_file_delete(key_file_fileparser, path.str,
                        MYF(MY_WME|MY_IGNORE_ENOENT));
    (void) update_phase(entry_pos, DDL_LOG_FINAL_PHASE);
    break;
  }
  case DDL_LOG_CREATE_TRIGGER_ACTION:
  {
    LEX_CSTRING db, table, trigger;
    db=      ddl_log_entry->db;
    table=   ddl_log_entry->name;
    trigger= ddl_log_entry->tmp_name;

    /* Delete backup .TRG (trigger file) if it exists */
    (void) build_filename_and_delete_tmp_file(to_path, sizeof(to_path) - 1,
                                              &db, &table,
                                              TRG_EXT,
                                              key_file_fileparser);
    (void) build_filename_and_delete_tmp_file(to_path, sizeof(to_path) - 1,
                                              &db, &trigger,
                                              TRN_EXT,
                                              key_file_fileparser);
    switch (ddl_log_entry->phase) {
    case DDL_CREATE_TRIGGER_PHASE_DELETE_COPY:
    {
      size_t length;
      /* Delete copy of .TRN and .TRG files */
      length= build_table_filename(to_path, sizeof(to_path) - 1,
                                   db.str, table.str, TRG_EXT, 0);
      to_path[length]= '-';
      to_path[length+1]= 0;
      mysql_file_delete(key_file_fileparser, to_path,
                        MYF(MY_WME|MY_IGNORE_ENOENT));

      length= build_table_filename(to_path, sizeof(to_path) - 1,
                                   db.str, trigger.str, TRN_EXT, 0);
      to_path[length]= '-';
      to_path[length+1]= 0;
      mysql_file_delete(key_file_fileparser, to_path,
                        MYF(MY_WME|MY_IGNORE_ENOENT));
    }
    /* Nothing else to do */
    (void) update_phase(entry_pos, DDL_LOG_FINAL_PHASE);
    break;
    case DDL_CREATE_TRIGGER_PHASE_OLD_COPIED:
    {
      LEX_CSTRING path= {to_path, 0};
      size_t length;
      /* Restore old version if the .TRN and .TRG files */
      length= build_table_filename(to_path, sizeof(to_path) - 1,
                                   db.str, table.str, TRG_EXT, 0);
      to_path[length]='-';
      to_path[length+1]= 0;
      path.length= length+1;
      /* an old TRN file only exist in the case if REPLACE was used */
      if (!access(to_path, F_OK))
        sql_restore_definition_file(&path);

      length= build_table_filename(to_path, sizeof(to_path) - 1,
                                   db.str, trigger.str, TRN_EXT, 0);
      to_path[length]='-';
      to_path[length+1]= 0;
      path.length= length+1;
      if (!access(to_path, F_OK))
        sql_restore_definition_file(&path);
      else
      {
        /*
          There was originally no .TRN for this trigger.
          Delete the newly created one.
        */
        to_path[length]= 0;
        mysql_file_delete(key_file_fileparser, to_path,
                          MYF(MY_WME|MY_IGNORE_ENOENT));
      }
      (void) update_phase(entry_pos, DDL_LOG_FINAL_PHASE);
      break;
    }
    case DDL_CREATE_TRIGGER_PHASE_NO_OLD_TRIGGER:
    {
      /* No old trigger existed. We can just delete the .TRN and .TRG files */
      build_table_filename(to_path, sizeof(to_path) - 1,
                           db.str, table.str, TRG_EXT, 0);
      mysql_file_delete(key_file_fileparser, to_path,
                        MYF(MY_WME|MY_IGNORE_ENOENT));
      build_table_filename(to_path, sizeof(to_path) - 1,
                           db.str, trigger.str, TRN_EXT, 0);
      mysql_file_delete(key_file_fileparser, to_path,
                        MYF(MY_WME|MY_IGNORE_ENOENT));
      (void) update_phase(entry_pos, DDL_LOG_FINAL_PHASE);
      break;
    }
    break;
  }
  break;
  }
  default:
    DBUG_ASSERT(0);
    break;
  }

end:
  delete file;
  if ((error= no_such_table_handler.unhandled_errors > 0))
    my_errno= no_such_table_handler.first_error;
  thd->pop_internal_handler();
  DBUG_RETURN(error);
}


/**
  Get a free entry in the ddl log

  @param[out] active_entry     A ddl log memory entry returned
  @param[out] write_header     Set to 1 if ddl log was enlarged

  @return Operation status
    @retval TRUE               Error
    @retval FALSE              Success
*/

static bool ddl_log_get_free_entry(DDL_LOG_MEMORY_ENTRY **active_entry)
{
  DDL_LOG_MEMORY_ENTRY *used_entry;
  DDL_LOG_MEMORY_ENTRY *first_used= global_ddl_log.first_used;
  DBUG_ENTER("ddl_log_get_free_entry");

  if (global_ddl_log.first_free == NULL)
  {
    if (!(used_entry= ((DDL_LOG_MEMORY_ENTRY*)
                       my_malloc(key_memory_DDL_LOG_MEMORY_ENTRY,
                                 sizeof(DDL_LOG_MEMORY_ENTRY), MYF(MY_WME)))))
    {
      sql_print_error("Failed to allocate memory for ddl log free list");
      *active_entry= 0;
      DBUG_RETURN(TRUE);
    }
    global_ddl_log.num_entries++;
    used_entry->entry_pos= global_ddl_log.num_entries;
  }
  else
  {
    used_entry= global_ddl_log.first_free;
    global_ddl_log.first_free= used_entry->next_log_entry;
  }
  /*
    Move from free list to used list
  */
  used_entry->next_log_entry= first_used;
  used_entry->prev_log_entry= NULL;
  used_entry->next_active_log_entry= NULL;
  global_ddl_log.first_used= used_entry;
  if (first_used)
    first_used->prev_log_entry= used_entry;

  *active_entry= used_entry;
  DBUG_RETURN(FALSE);
}


/**
  Release a log memory entry.
  @param log_memory_entry                Log memory entry to release
*/

void ddl_log_release_memory_entry(DDL_LOG_MEMORY_ENTRY *log_entry)
{
  DDL_LOG_MEMORY_ENTRY *next_log_entry= log_entry->next_log_entry;
  DDL_LOG_MEMORY_ENTRY *prev_log_entry= log_entry->prev_log_entry;
  DBUG_ENTER("ddl_log_release_memory_entry");

  mysql_mutex_assert_owner(&LOCK_gdl);
  log_entry->next_log_entry= global_ddl_log.first_free;
  global_ddl_log.first_free= log_entry;

  if (prev_log_entry)
    prev_log_entry->next_log_entry= next_log_entry;
  else
    global_ddl_log.first_used= next_log_entry;
  if (next_log_entry)
    next_log_entry->prev_log_entry= prev_log_entry;
  DBUG_VOID_RETURN;
}


/**
  Execute one entry in the ddl log.

  Executing an entry means executing a linked list of actions.

  @param first_entry           Reference to first action in entry

  @return Operation status
    @retval TRUE               Error
    @retval FALSE              Success
*/

static bool ddl_log_execute_entry_no_lock(THD *thd, uint first_entry)
{
  DDL_LOG_ENTRY ddl_log_entry;
  uint read_entry= first_entry;
  MEM_ROOT mem_root;
  DBUG_ENTER("ddl_log_execute_entry_no_lock");

  mysql_mutex_assert_owner(&LOCK_gdl);
  init_sql_alloc(key_memory_gdl, &mem_root, TABLE_ALLOC_BLOCK_SIZE, 0,
                 MYF(MY_THREAD_SPECIFIC));
  do
  {
    if (read_ddl_log_entry(read_entry, &ddl_log_entry))
    {
      /* Error logged to error log. Continue with next log entry */
      break;
    }
    DBUG_ASSERT(ddl_log_entry.entry_type != DDL_LOG_EXECUTE_CODE);

    if (ddl_log_execute_action(thd, &mem_root, &ddl_log_entry))
    {
      uint action_type= ddl_log_entry.action_type;
      if (action_type >= DDL_LOG_LAST_ACTION)
        action_type= 0;

      /* Write to error log and continue with next log entry */
      sql_print_error("Got error %d when trying to execute action for entry %u "
                      "of type '%s'",
                      (int) my_errno, read_entry,
                      ddl_log_action_name[action_type]);
      if (ddl_log_entry.entry_type != DDL_TRY_LOG_ENTRY_CODE)
        break; // TODO: do we need this break at all?
    }
    read_entry= ddl_log_entry.next_entry;
  } while (read_entry);

  free_root(&mem_root, MYF(0));
  DBUG_RETURN(FALSE);
}


/*
  External interface methods for the DDL log Module
  ---------------------------------------------------
*/

/**
  Write a ddl log entry.

  A careful write of the ddl log is performed to ensure that we can
  handle crashes occurring during CREATE and ALTER TABLE processing.

  @param ddl_log_entry         Information about log entry
  @param[out] entry_written    Entry information written into

  @return Operation status
    @retval TRUE               Error
    @retval FALSE              Success
*/

bool ddl_log_write_entry(DDL_LOG_ENTRY *ddl_log_entry,
                         DDL_LOG_MEMORY_ENTRY **active_entry)
{
  bool error;
  DBUG_ENTER("ddl_log_write_entry");

  *active_entry= 0;
  mysql_mutex_assert_owner(&LOCK_gdl);
  DBUG_ASSERT(global_ddl_log.open);
  if (unlikely(!global_ddl_log.open))
  {
    my_error(ER_INTERNAL_ERROR, MYF(0), "ddl log not initialized");
    DBUG_RETURN(TRUE);
  }

  ddl_log_entry->entry_type= DDL_LOG_ENTRY_CODE;
  set_global_from_ddl_log_entry(ddl_log_entry);
  if (ddl_log_get_free_entry(active_entry))
    DBUG_RETURN(TRUE);

  error= FALSE;
  DBUG_PRINT("ddl_log",
             ("pos: %u->%u  entry: %u  action: %u (%s) phase: %u  "
              "handler: '%s'  name: '%s'  from_name: '%s'  tmp_name: '%s'",
              (*active_entry)->entry_pos,
              ddl_log_entry->next_entry,
              (uint) ddl_log_entry->entry_type,
              (uint) ddl_log_entry->action_type,
              ddl_log_action_name[ddl_log_entry->action_type],
              (uint) ddl_log_entry->phase,
              ddl_log_entry->handler_name.str,
              ddl_log_entry->name.str,
              ddl_log_entry->from_name.str,
              ddl_log_entry->tmp_name.str));

  if (unlikely(write_ddl_log_file_entry((*active_entry)->entry_pos)))
  {
    ddl_log_release_memory_entry(*active_entry);
    *active_entry= 0;
    error= TRUE;
    sql_print_error("Failed to write entry_pos = %u",
                    (*active_entry)->entry_pos);
  }
  DBUG_RETURN(error);
}


/**
  @brief Write or update execute entry in the ddl log.

  @details An execute entry points to the first entry that should
  be excuted during recovery. In some cases it's only written once,
  in other cases it's updated for each log entry to point to the new
  header for the list.

  When called, the previous log entries have already been written but not yet
  synched to disk.  We write a couple of log entries that describe
  action to perform.  These entries are set-up in a linked list,
  however only when an execute entry is put as the first entry these will be
  executed during recovery.

  @param first_entry               First entry in linked list of entries
                                   to execute.
  @param[in,out] active_entry      Entry to execute, 0 = NULL if the entry
                                   is written first time and needs to be
                                   returned. In this case the entry written
                                   is returned in this parameter
  @return Operation status
    @retval TRUE                   Error
    @retval FALSE                  Success
*/

bool ddl_log_write_execute_entry(uint first_entry,
                                 DDL_LOG_MEMORY_ENTRY **active_entry)
{
  uchar *file_entry_buf= global_ddl_log.file_entry_buf;
  bool got_free_entry= 0;
  DBUG_ENTER("ddl_log_write_execute_entry");

  mysql_mutex_assert_owner(&LOCK_gdl);
  /*
    We haven't synched the log entries yet, we synch them now before
    writing the execute entry. If complete is true we haven't written
    any log entries before, we are only here to write the execute
    entry to indicate it is done.
  */
  (void) ddl_log_sync_no_lock();
  file_entry_buf[DDL_LOG_ENTRY_TYPE_POS]= (uchar)DDL_LOG_EXECUTE_CODE;
  file_entry_buf[DDL_LOG_ACTION_TYPE_POS]= 0; /* Ignored for execute entries */
  file_entry_buf[DDL_LOG_PHASE_POS]= 0;
  int4store(file_entry_buf + DDL_LOG_NEXT_ENTRY_POS, first_entry);
  int8store(file_entry_buf + DDL_LOG_XID_POS, 0);
  bzero(file_entry_buf + global_ddl_log.name_pos, global_ddl_log.io_size -
        global_ddl_log.name_pos);

  if (!(*active_entry))
  {
    if (ddl_log_get_free_entry(active_entry))
      DBUG_RETURN(TRUE);
    got_free_entry= TRUE;
  }
  DBUG_PRINT("ddl_log",
             ("pos: %u->%u  entry: %u",
             (*active_entry)->entry_pos, first_entry,
             (uint) global_ddl_log.file_entry_buf[DDL_LOG_ENTRY_TYPE_POS]));
  if (write_ddl_log_file_entry((*active_entry)->entry_pos))
  {
    if (got_free_entry)
    {
      ddl_log_release_memory_entry(*active_entry);
      *active_entry= 0;
    }
    sql_print_error("Error writing execute entry in ddl log");
    DBUG_RETURN(TRUE);
  }
  (void) ddl_log_sync_no_lock();
  DBUG_RETURN(FALSE);
}


/**
  Increment phase for entry. Will deactivate entry after all phases are done

  @details see ddl_log_increment_phase_no_lock.

  @param entry_pos     Entry position of record to change

  @return Operation status
    @retval TRUE      Error
    @retval FALSE     Success
*/

bool ddl_log_increment_phase(uint entry_pos)
{
  bool error;
  DBUG_ENTER("ddl_log_increment_phase");
  DBUG_PRINT("ddl_log", ("pos: %u", entry_pos));

  mysql_mutex_lock(&LOCK_gdl);
  error= ddl_log_increment_phase_no_lock(entry_pos);
  mysql_mutex_unlock(&LOCK_gdl);
  DBUG_RETURN(error);
}


/**
  Sync ddl log file.

  @return Operation status
    @retval TRUE        Error
    @retval FALSE       Success
*/

bool ddl_log_sync()
{
  bool error;
  DBUG_ENTER("ddl_log_sync");

  mysql_mutex_lock(&LOCK_gdl);
  error= ddl_log_sync_no_lock();
  mysql_mutex_unlock(&LOCK_gdl);

  DBUG_RETURN(error);
}


/**
  Execute one entry in the ddl log.

  Executing an entry means executing a linked list of actions.

  @param first_entry           Reference to first action in entry

  @return Operation status
    @retval TRUE               Error
    @retval FALSE              Success
*/

bool ddl_log_execute_entry(THD *thd, uint first_entry)
{
  bool error;
  DBUG_ENTER("ddl_log_execute_entry");

  mysql_mutex_lock(&LOCK_gdl);
  error= ddl_log_execute_entry_no_lock(thd, first_entry);
  mysql_mutex_unlock(&LOCK_gdl);
  DBUG_RETURN(error);
}


/**
  Close the ddl log.
*/

static void close_ddl_log()
{
  DBUG_ENTER("close_ddl_log");
  if (global_ddl_log.file_id >= 0)
  {
    (void) mysql_file_close(global_ddl_log.file_id, MYF(MY_WME));
    global_ddl_log.file_id= (File) -1;
  }
  global_ddl_log.open= 0;
  DBUG_VOID_RETURN;
}


/**
  Loop over ddl log excute entries and mark those that are already stored
  in the binary log as completed

  @return
  @retval 0 ok
  @return 1 fail (write error)

*/

bool ddl_log_close_binlogged_events(HASH *xids)
{
  uint i;
  DDL_LOG_ENTRY ddl_log_entry;
  DBUG_ENTER("ddl_log_close_binlogged_events");

  if (global_ddl_log.num_entries == 0 || xids->records == 0)
    DBUG_RETURN(0);

  mysql_mutex_lock(&LOCK_gdl);
  for (i= 1; i <= global_ddl_log.num_entries; i++)
  {
    if (read_ddl_log_entry(i, &ddl_log_entry))
      break;                                    // Read error. Ignore
    DBUG_PRINT("xid",("xid: %llu", ddl_log_entry.xid));
    if (ddl_log_entry.entry_type == DDL_LOG_EXECUTE_CODE &&
        ddl_log_entry.xid != 0 &&
        my_hash_search(xids, (uchar*) &ddl_log_entry.xid,
                       sizeof(ddl_log_entry.xid)))
    {
      if (disable_execute_entry(i))
      {
        mysql_mutex_unlock(&LOCK_gdl);
        DBUG_RETURN(1);                         // Write error. Fatal!
      }
    }
  }
  (void) ddl_log_sync_no_lock();
  mysql_mutex_unlock(&LOCK_gdl);
  DBUG_RETURN(0);
}


/**
  Execute the ddl log at recovery of MySQL Server.

  @return
  @retval 0     Ok.
  @retval > 0   Fatal error. We have to abort (can't create ddl log)
  @return < -1  Recovery failed, but new log exists and is usable

*/

int ddl_log_execute_recovery()
{
  uint i;
  int error= 0;
  THD *thd;
  DDL_LOG_ENTRY ddl_log_entry;
  static char recover_query_string[]= "INTERNAL DDL LOG RECOVER IN PROGRESS";
  DBUG_ENTER("ddl_log_execute_recovery");

  if (global_ddl_log.num_entries == 0)
    DBUG_RETURN(0);

  /*
    To be able to run this from boot, we allocate a temporary THD
  */
  if (!(thd=new THD(0)))
  {
    DBUG_ASSERT(0);                             // Fatal error
    DBUG_RETURN(1);
  }
  thd->thread_stack= (char*) &thd;
  thd->store_globals();
  recovery_state.drop_table.free();
  recovery_state.drop_view.free();

  thd->set_query(recover_query_string, strlen(recover_query_string));

  mysql_mutex_lock(&LOCK_gdl);
  for (i= 1; i <= global_ddl_log.num_entries; i++)
  {
    if (read_ddl_log_entry(i, &ddl_log_entry))
    {
      error= -1;
      continue;
    }
    if (ddl_log_entry.entry_type == DDL_LOG_EXECUTE_CODE)
    {
      if (ddl_log_execute_entry_no_lock(thd, ddl_log_entry.next_entry))
      {
        /* Real unpleasant scenario but we continue anyways.  */
        error= -1;
        continue;
      }
    }
  }
  recovery_state.drop_table.free();
  recovery_state.drop_view.free();
  close_ddl_log();
  mysql_mutex_unlock(&LOCK_gdl);
  thd->reset_query();
  delete thd;

  /*
    Create a new ddl_log to get rid of old stuff and ensure that header matches
    the current source version
   */
  if (create_ddl_log())
    DBUG_RETURN(1);
  DBUG_RETURN(error);
}


/**
  Release all memory allocated to the ddl log and delete the ddl log
*/

void ddl_log_release()
{
  char file_name[FN_REFLEN];
  DDL_LOG_MEMORY_ENTRY *free_list;
  DDL_LOG_MEMORY_ENTRY *used_list;
  DBUG_ENTER("ddl_log_release");

  if (!global_ddl_log.initialized)
    DBUG_VOID_RETURN;

  global_ddl_log.initialized= 0;

  free_list= global_ddl_log.first_free;
  used_list= global_ddl_log.first_used;
  while (used_list)
  {
    DDL_LOG_MEMORY_ENTRY *tmp= used_list->next_log_entry;
    my_free(used_list);
    used_list= tmp;
  }
  while (free_list)
  {
    DDL_LOG_MEMORY_ENTRY *tmp= free_list->next_log_entry;
    my_free(free_list);
    free_list= tmp;
  }
  my_free(global_ddl_log.file_entry_buf);
  global_ddl_log.file_entry_buf= 0;
  close_ddl_log();

  create_ddl_log_file_name(file_name);
  (void) mysql_file_delete(key_file_global_ddl_log, file_name, MYF(0));
  mysql_mutex_destroy(&LOCK_gdl);
  DBUG_VOID_RETURN;
}


/**
   Methods for DDL_LOG_STATE
*/

static void add_log_entry(DDL_LOG_STATE *state,
                          DDL_LOG_MEMORY_ENTRY *log_entry)
{
  log_entry->next_active_log_entry= state->list;
  state->list= log_entry;
}


void ddl_log_release_entries(DDL_LOG_STATE *ddl_log_state)
{
  DDL_LOG_MEMORY_ENTRY *next;
  for (DDL_LOG_MEMORY_ENTRY *log_entry= ddl_log_state->list;
       log_entry;
       log_entry= next)
  {
    next= log_entry->next_active_log_entry;
    ddl_log_release_memory_entry(log_entry);
  }
  ddl_log_state->list= 0;

  if (ddl_log_state->execute_entry)
    ddl_log_release_memory_entry(ddl_log_state->execute_entry);
}


/****************************************************************************
   Implementations of common ddl entries
*****************************************************************************/

/**
   Complete ddl logging.  This is done when all statements has completed
   successfully and we can disable the execute log entry.
*/

void ddl_log_complete(DDL_LOG_STATE *state)
{
  DBUG_ENTER("ddl_log_complete");

  if (unlikely(!state->list))
    DBUG_VOID_RETURN;                           // ddl log not used

  mysql_mutex_lock(&LOCK_gdl);
  if (likely(state->execute_entry))
    ddl_log_disable_execute_entry(&state->execute_entry);
  ddl_log_release_entries(state);
  mysql_mutex_unlock(&LOCK_gdl);
  state->list= 0;
  DBUG_VOID_RETURN;
};


/**
  Revert (execute) all entries in the ddl log
*/

void ddl_log_revert(THD *thd, DDL_LOG_STATE *state)
{
  DBUG_ENTER("ddl_log_revert");

  if (unlikely(!state->list))
    DBUG_VOID_RETURN;                           // ddl log not used

  mysql_mutex_lock(&LOCK_gdl);
  if (likely(state->execute_entry))
  {
    ddl_log_execute_entry_no_lock(thd, state->list->entry_pos);
    ddl_log_disable_execute_entry(&state->execute_entry);
  }
  ddl_log_release_entries(state);
  mysql_mutex_unlock(&LOCK_gdl);
  state->list= 0;
  DBUG_VOID_RETURN;
}


/*
  Update phase of last created ddl log entry
*/

bool ddl_log_update_phase(DDL_LOG_STATE *state, uchar phase)
{
  DBUG_ENTER("ddl_log_update_phase");
  DBUG_RETURN(update_phase(state->list->entry_pos, phase));
}


/**
   Disable last ddl entry
*/

bool ddl_log_disable_entry(DDL_LOG_STATE *state)
{
  DBUG_ENTER("ddl_log_disable_entry");
  /* The following may not be true in case of temporary tables */
  if (likely(state->list))
    DBUG_RETURN(update_phase(state->list->entry_pos, DDL_LOG_FINAL_PHASE));
  DBUG_RETURN(0);
}


/**
   Update XID for execute event
*/

bool ddl_log_update_xid(DDL_LOG_STATE *state, ulonglong xid)
{
  DBUG_ENTER("ddl_log_update_xid");
  DBUG_PRINT("enter", ("xid: %llu", xid));
  /* The following may not be true in case of temporary tables */
  if (likely(state->execute_entry))
    DBUG_RETURN(update_xid(state->execute_entry->entry_pos, xid));
  DBUG_RETURN(0);
}


/*
  Write ddl_log_entry and write or update ddl_execute_entry
*/

static bool ddl_log_write(DDL_LOG_STATE *ddl_state,
                          DDL_LOG_ENTRY *ddl_log_entry)
{
  int error;
  DDL_LOG_MEMORY_ENTRY *log_entry;
  DBUG_ENTER("ddl_log_write");

  mysql_mutex_lock(&LOCK_gdl);
  error= ((ddl_log_write_entry(ddl_log_entry, &log_entry)) ||
          ddl_log_write_execute_entry(log_entry->entry_pos,
                                      &ddl_state->execute_entry));
  mysql_mutex_unlock(&LOCK_gdl);
  if (error)
  {
    if (log_entry)
      ddl_log_release_memory_entry(log_entry);
    DBUG_RETURN(1);
  }
  add_log_entry(ddl_state, log_entry);
  DBUG_RETURN(0);
}


/**
   Logging of rename table
*/

bool ddl_log_rename_table(THD *thd, DDL_LOG_STATE *ddl_state,
                          handlerton *hton,
                          const LEX_CSTRING *org_db,
                          const LEX_CSTRING *org_alias,
                          const LEX_CSTRING *new_db,
                          const LEX_CSTRING *new_alias)
{
  DDL_LOG_ENTRY ddl_log_entry;
  DBUG_ENTER("ddl_log_rename_file");

  bzero(&ddl_log_entry, sizeof(ddl_log_entry));

  ddl_log_entry.action_type=  DDL_LOG_RENAME_TABLE_ACTION;
  ddl_log_entry.next_entry=   ddl_state->list ? ddl_state->list->entry_pos : 0;
  lex_string_set(&ddl_log_entry.handler_name,
                 ha_resolve_storage_engine_name(hton));
  ddl_log_entry.db=           *const_cast<LEX_CSTRING*>(new_db);
  ddl_log_entry.name=         *const_cast<LEX_CSTRING*>(new_alias);
  ddl_log_entry.from_db=      *const_cast<LEX_CSTRING*>(org_db);
  ddl_log_entry.from_name=    *const_cast<LEX_CSTRING*>(org_alias);
  ddl_log_entry.phase=        DDL_RENAME_PHASE_TABLE;

  DBUG_RETURN(ddl_log_write(ddl_state, &ddl_log_entry));
}

/*
  Logging of rename view
*/

bool ddl_log_rename_view(THD *thd, DDL_LOG_STATE *ddl_state,
                         const LEX_CSTRING *org_db,
                         const LEX_CSTRING *org_alias,
                         const LEX_CSTRING *new_db,
                         const LEX_CSTRING *new_alias)
{
  DDL_LOG_ENTRY ddl_log_entry;
  DBUG_ENTER("ddl_log_rename_file");

  bzero(&ddl_log_entry, sizeof(ddl_log_entry));

  ddl_log_entry.action_type=  DDL_LOG_RENAME_VIEW_ACTION;
  ddl_log_entry.next_entry=   ddl_state->list ? ddl_state->list->entry_pos : 0;
  ddl_log_entry.db=           *const_cast<LEX_CSTRING*>(new_db);
  ddl_log_entry.name=         *const_cast<LEX_CSTRING*>(new_alias);
  ddl_log_entry.from_db=      *const_cast<LEX_CSTRING*>(org_db);
  ddl_log_entry.from_name=    *const_cast<LEX_CSTRING*>(org_alias);

  DBUG_RETURN(ddl_log_write(ddl_state, &ddl_log_entry));
}


/**
   Logging of DROP TABLE and DROP VIEW

   Note that in contrast to rename, which are re-done in reverse order,
   deletes are stored in a linked list according to delete order. This
   is to ensure that the tables, for the query generated for binlog,
   is in original delete order.
*/

static bool ddl_log_drop_init(THD *thd, DDL_LOG_STATE *ddl_state,
                              ddl_log_action_code action_code,
                              const LEX_CSTRING *db,
                              const LEX_CSTRING *comment)
{
  DDL_LOG_ENTRY ddl_log_entry;
  DBUG_ENTER("ddl_log_drop_file");

  bzero(&ddl_log_entry, sizeof(ddl_log_entry));

  ddl_log_entry.action_type=  action_code;
  ddl_log_entry.from_db=      *const_cast<LEX_CSTRING*>(db);
  ddl_log_entry.tmp_name=     *const_cast<LEX_CSTRING*>(comment);

  DBUG_RETURN(ddl_log_write(ddl_state, &ddl_log_entry));
}


bool ddl_log_drop_table_init(THD *thd, DDL_LOG_STATE *ddl_state,
                             const LEX_CSTRING *db,
                             const LEX_CSTRING *comment)
{
  return ddl_log_drop_init(thd, ddl_state, DDL_LOG_DROP_INIT_ACTION,
                           db, comment);
}

bool ddl_log_drop_view_init(THD *thd, DDL_LOG_STATE *ddl_state,
                            const LEX_CSTRING *db)
{
  return ddl_log_drop_init(thd, ddl_state, DDL_LOG_DROP_INIT_ACTION,
                           db, &empty_clex_str);
}

static bool ddl_log_drop(THD *thd, DDL_LOG_STATE *ddl_state,
                         ddl_log_action_code action_code,
                         uint phase,
                         handlerton *hton,
                         const LEX_CSTRING *path,
                         const LEX_CSTRING *db,
                         const LEX_CSTRING *table)
{
  DDL_LOG_ENTRY ddl_log_entry;
  DDL_LOG_MEMORY_ENTRY *log_entry;
  DBUG_ENTER("ddl_log_drop");

  DBUG_ASSERT(ddl_state->list);
  bzero(&ddl_log_entry, sizeof(ddl_log_entry));

  ddl_log_entry.action_type=  action_code;
  if (hton)
    lex_string_set(&ddl_log_entry.handler_name,
                   ha_resolve_storage_engine_name(hton));
  ddl_log_entry.db=           *const_cast<LEX_CSTRING*>(db);
  ddl_log_entry.name=         *const_cast<LEX_CSTRING*>(table);
  ddl_log_entry.tmp_name=     *const_cast<LEX_CSTRING*>(path);
  ddl_log_entry.phase=        (uchar) phase;

  mysql_mutex_lock(&LOCK_gdl);
  if (ddl_log_write_entry(&ddl_log_entry, &log_entry))
    goto error;

  (void) ddl_log_sync_no_lock();
  if (update_next_entry_pos(ddl_state->list->entry_pos,
                            log_entry->entry_pos))
  {
    ddl_log_release_memory_entry(log_entry);
    goto error;
  }

  mysql_mutex_unlock(&LOCK_gdl);
  add_log_entry(ddl_state, log_entry);
  DBUG_RETURN(0);

error:
  mysql_mutex_unlock(&LOCK_gdl);
  DBUG_RETURN(1);
}


bool ddl_log_drop_table(THD *thd, DDL_LOG_STATE *ddl_state,
                        handlerton *hton,
                        const LEX_CSTRING *path,
                        const LEX_CSTRING *db,
                        const LEX_CSTRING *table)
{
  DBUG_ENTER("ddl_log_drop_table");
  DBUG_RETURN(ddl_log_drop(thd, ddl_state,
                           DDL_LOG_DROP_TABLE_ACTION, DDL_DROP_PHASE_TABLE,
                           hton, path, db, table));
}


bool ddl_log_drop_view(THD *thd, DDL_LOG_STATE *ddl_state,
                        const LEX_CSTRING *path,
                        const LEX_CSTRING *db,
                        const LEX_CSTRING *table)
{
  DBUG_ENTER("ddl_log_drop_view");
  DBUG_RETURN(ddl_log_drop(thd, ddl_state,
                           DDL_LOG_DROP_VIEW_ACTION, 0,
                           (handlerton*) 0, path, db, table));
}


bool ddl_log_drop_trigger(THD *thd, DDL_LOG_STATE *ddl_state,
                          const LEX_CSTRING *db,
                          const LEX_CSTRING *table,
                          const LEX_CSTRING *trigger_name,
                          const LEX_CSTRING *query)
{
  DDL_LOG_ENTRY ddl_log_entry;
  MY_STAT stat_info;
  char path[FN_REFLEN+1];
  off_t frm_length= 0;
  size_t max_query_length;
  DBUG_ENTER("ddl_log_drop_trigger");

  build_table_filename(path, sizeof(path)-1, db->str, table->str, TRG_EXT, 0);

  /* We can use length of frm file as an indication if trigger was removed */
  if (my_stat(path, &stat_info, MYF(MY_WME | ME_WARNING)))
    frm_length= (off_t) stat_info.st_size;

  bzero(&ddl_log_entry, sizeof(ddl_log_entry));

  ddl_log_entry.action_type=  DDL_LOG_DROP_TRIGGER_ACTION;
  ddl_log_entry.unique_id=    (ulonglong) frm_length;
  ddl_log_entry.db=           *const_cast<LEX_CSTRING*>(db);
  ddl_log_entry.name=         *const_cast<LEX_CSTRING*>(table);
  ddl_log_entry.from_name=    *const_cast<LEX_CSTRING*>(trigger_name);

  /*
    If we can store query as is, we store it. Otherwise it will be
    re-generated on recovery
  */

  max_query_length= ddl_log_free_space_in_entry(&ddl_log_entry);
  if (max_query_length >= query->length)
    ddl_log_entry.tmp_name=     *const_cast<LEX_CSTRING*>(query);

  DBUG_RETURN(ddl_log_write(ddl_state, &ddl_log_entry));
}


/**
   Log DROP DATABASE

   This is logged after all DROP TABLE's for the database.
   As now know we are going to log DROP DATABASE to the binary log, we want
   to ignore want to ignore all preceding DROP TABLE entries. We do that by
   linking this entry directly after the execute entry and forgetting the
   link to the previous entries (not setting ddl_log_entry.next_entry)
*/

bool ddl_log_drop_db(THD *thd, DDL_LOG_STATE *ddl_state,
                     const LEX_CSTRING *db, const LEX_CSTRING *path)
{
  DDL_LOG_ENTRY ddl_log_entry;
  DBUG_ENTER("ddl_log_drop_db");

  bzero(&ddl_log_entry, sizeof(ddl_log_entry));
  ddl_log_entry.action_type=  DDL_LOG_DROP_DB_ACTION;
  ddl_log_entry.db=           *const_cast<LEX_CSTRING*>(db);
  ddl_log_entry.tmp_name=     *const_cast<LEX_CSTRING*>(path);
  DBUG_RETURN(ddl_log_write(ddl_state, &ddl_log_entry));
}


/**
   Log CREATE TABLE

   @param only_frm  On recovery, only drop the .frm. This is needed for
                    example when deleting a table that was discovered.
*/

bool ddl_log_create_table(THD *thd, DDL_LOG_STATE *ddl_state,
                          handlerton *hton,
                          const LEX_CSTRING *path,
                          const LEX_CSTRING *db,
                          const LEX_CSTRING *table,
                          bool only_frm)
{
  DDL_LOG_ENTRY ddl_log_entry;
  DBUG_ENTER("ddl_log_create_table");

  bzero(&ddl_log_entry, sizeof(ddl_log_entry));
  ddl_log_entry.action_type= DDL_LOG_CREATE_TABLE_ACTION;
  if (hton)
    lex_string_set(&ddl_log_entry.handler_name,
                   ha_resolve_storage_engine_name(hton));
  ddl_log_entry.db=           *const_cast<LEX_CSTRING*>(db);
  ddl_log_entry.name=         *const_cast<LEX_CSTRING*>(table);
  ddl_log_entry.tmp_name=     *const_cast<LEX_CSTRING*>(path);
  ddl_log_entry.unique_id=    only_frm;

  DBUG_RETURN(ddl_log_write(ddl_state, &ddl_log_entry));
}


/**
   Log CREATE VIEW
*/

bool ddl_log_create_view(THD *thd, DDL_LOG_STATE *ddl_state,
                         const LEX_CSTRING *path,
                         enum_ddl_log_create_view_phase phase)
{
  DDL_LOG_ENTRY ddl_log_entry;
  DBUG_ENTER("ddl_log_create_view");

  bzero(&ddl_log_entry, sizeof(ddl_log_entry));
  ddl_log_entry.action_type=  DDL_LOG_CREATE_VIEW_ACTION;
  ddl_log_entry.tmp_name=     *const_cast<LEX_CSTRING*>(path);
  ddl_log_entry.phase=        (uchar) phase;
  DBUG_RETURN(ddl_log_write(ddl_state, &ddl_log_entry));
}


/**
  Log creation of temporary file that should be deleted during recovery

  @param thd             Thread handler
  @param ddl_log_state   ddl_state
  @param path            Path to file to be deleted
  @param depending_state If not NULL, then do not delete the temp file if this
                         entry exists and is active.
*/

bool ddl_log_delete_tmp_file(THD *thd, DDL_LOG_STATE *ddl_state,
                             const LEX_CSTRING *path,
                             DDL_LOG_STATE *depending_state)
{
  DDL_LOG_ENTRY ddl_log_entry;
  DBUG_ENTER("ddl_log_delete_tmp_file");

  bzero(&ddl_log_entry, sizeof(ddl_log_entry));
  ddl_log_entry.action_type=  DDL_LOG_DELETE_TMP_FILE_ACTION;
  ddl_log_entry.next_entry=   ddl_state->list ? ddl_state->list->entry_pos : 0;
  ddl_log_entry.tmp_name=     *const_cast<LEX_CSTRING*>(path);
  if (depending_state)
    ddl_log_entry.unique_id=  depending_state->execute_entry->entry_pos;
  DBUG_RETURN(ddl_log_write(ddl_state, &ddl_log_entry));
}


/**
   Log CREATE TRIGGER
*/

bool ddl_log_create_trigger(THD *thd, DDL_LOG_STATE *ddl_state,
                            const LEX_CSTRING *db, const LEX_CSTRING *table,
                            const LEX_CSTRING *trigger_name,
                            enum_ddl_log_create_trigger_phase phase)
{
  DDL_LOG_ENTRY ddl_log_entry;
  DBUG_ENTER("ddl_log_create_view");

  bzero(&ddl_log_entry, sizeof(ddl_log_entry));
  ddl_log_entry.action_type=  DDL_LOG_CREATE_TRIGGER_ACTION;
  ddl_log_entry.db=           *const_cast<LEX_CSTRING*>(db);
  ddl_log_entry.name=         *const_cast<LEX_CSTRING*>(table);
  ddl_log_entry.tmp_name=     *const_cast<LEX_CSTRING*>(trigger_name);
  ddl_log_entry.phase=        (uchar) phase;
  DBUG_RETURN(ddl_log_write(ddl_state, &ddl_log_entry));
}
