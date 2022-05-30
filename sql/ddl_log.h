/*
   Copyright (c) 2000, 2019, Oracle and/or its affiliates.
   Copyright (c) 2010, 2021, MariaDB

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

/* External interfaces to ddl log functions */

#ifndef DDL_LOG_INCLUDED
#define DDL_LOG_INCLUDED

enum ddl_log_entry_code
{
  /*
    DDL_LOG_UNKOWN
      Here mainly to detect blocks that are all zero

    DDL_LOG_EXECUTE_CODE:
      This is a code that indicates that this is a log entry to
      be executed, from this entry a linked list of log entries
      can be found and executed.
    DDL_LOG_ENTRY_CODE:
      An entry to be executed in a linked list from an execute log
      entry.
    DDL_LOG_IGNORE_ENTRY_CODE:
      An entry that is to be ignored
  */
  DDL_LOG_UNKNOWN= 0,
  DDL_LOG_EXECUTE_CODE= 1,
  DDL_LOG_ENTRY_CODE= 2,
  DDL_LOG_IGNORE_ENTRY_CODE= 3,
  DDL_LOG_ENTRY_CODE_LAST= 4
};


/*
  When adding things below, also add an entry to ddl_log_action_names and
  ddl_log_entry_phases in ddl_log.cc
*/

enum ddl_log_action_code
{
  /*
    The type of action that a DDL_LOG_ENTRY_CODE entry is to
    perform.
  */
  DDL_LOG_UNKNOWN_ACTION= 0,

  /* Delete a .frm file or a table in the partition engine */
  DDL_LOG_DELETE_ACTION= 1,

  /* Rename a .frm fire a table in the partition engine */
  DDL_LOG_RENAME_ACTION= 2,

  /*
    Rename an entity after removing the previous entry with the
    new name, that is replace this entry.
  */
  DDL_LOG_REPLACE_ACTION= 3,

  /* Exchange two entities by renaming them a -> tmp, b -> a, tmp -> b */
  DDL_LOG_EXCHANGE_ACTION= 4,
  /*
    log do_rename(): Rename of .frm file, table, stat_tables and triggers
  */
  DDL_LOG_RENAME_TABLE_ACTION= 5,
  DDL_LOG_RENAME_VIEW_ACTION= 6,
  DDL_LOG_DROP_INIT_ACTION= 7,
  DDL_LOG_DROP_TABLE_ACTION= 8,
  DDL_LOG_DROP_VIEW_ACTION= 9,
  DDL_LOG_DROP_TRIGGER_ACTION= 10,
  DDL_LOG_DROP_DB_ACTION=11,
  DDL_LOG_CREATE_TABLE_ACTION=12,
  DDL_LOG_CREATE_VIEW_ACTION=13,
  DDL_LOG_DELETE_TMP_FILE_ACTION=14,
  DDL_LOG_CREATE_TRIGGER_ACTION=15,
  DDL_LOG_ALTER_TABLE_ACTION=16,
  DDL_LOG_STORE_QUERY_ACTION=17,
  DDL_LOG_LAST_ACTION                          /* End marker */
};


/* Number of phases for each ddl_log_action_code */
extern const uchar ddl_log_entry_phases[DDL_LOG_LAST_ACTION];


enum enum_ddl_log_exchange_phase {
  EXCH_PHASE_NAME_TO_TEMP= 0,
  EXCH_PHASE_FROM_TO_NAME= 1,
  EXCH_PHASE_TEMP_TO_FROM= 2,
  EXCH_PHASE_END
};

enum enum_ddl_log_rename_table_phase {
  DDL_RENAME_PHASE_TRIGGER= 0,
  DDL_RENAME_PHASE_STAT,
  DDL_RENAME_PHASE_TABLE,
  DDL_RENAME_PHASE_END
};

enum enum_ddl_log_drop_table_phase {
  DDL_DROP_PHASE_TABLE=0,
  DDL_DROP_PHASE_TRIGGER,
  DDL_DROP_PHASE_BINLOG,
  DDL_DROP_PHASE_RESET,  /* Reset found list of dropped tables */
  DDL_DROP_PHASE_END
};

enum enum_ddl_log_drop_db_phase {
  DDL_DROP_DB_PHASE_INIT=0,
  DDL_DROP_DB_PHASE_LOG,
  DDL_DROP_DB_PHASE_END
};

enum enum_ddl_log_create_table_phase {
  DDL_CREATE_TABLE_PHASE_INIT=0,
  DDL_CREATE_TABLE_PHASE_LOG,
  DDL_CREATE_TABLE_PHASE_END
};

enum enum_ddl_log_create_view_phase {
  DDL_CREATE_VIEW_PHASE_NO_OLD_VIEW,
  DDL_CREATE_VIEW_PHASE_DELETE_VIEW_COPY,
  DDL_CREATE_VIEW_PHASE_OLD_VIEW_COPIED,
  DDL_CREATE_VIEW_PHASE_END
};

enum enum_ddl_log_create_trigger_phase {
  DDL_CREATE_TRIGGER_PHASE_NO_OLD_TRIGGER,
  DDL_CREATE_TRIGGER_PHASE_DELETE_COPY,
  DDL_CREATE_TRIGGER_PHASE_OLD_COPIED,
  DDL_CREATE_TRIGGER_PHASE_END
};

enum enum_ddl_log_alter_table_phase {
  DDL_ALTER_TABLE_PHASE_INIT,
  DDL_ALTER_TABLE_PHASE_RENAME_FAILED,
  DDL_ALTER_TABLE_PHASE_INPLACE_COPIED,
  DDL_ALTER_TABLE_PHASE_INPLACE,
  DDL_ALTER_TABLE_PHASE_PREPARE_INPLACE,
  DDL_ALTER_TABLE_PHASE_CREATED,
  DDL_ALTER_TABLE_PHASE_COPIED,
  DDL_ALTER_TABLE_PHASE_OLD_RENAMED,
  DDL_ALTER_TABLE_PHASE_UPDATE_TRIGGERS,
  DDL_ALTER_TABLE_PHASE_UPDATE_STATS,
  DDL_ALTER_TABLE_PHASE_UPDATE_BINARY_LOG,
  DDL_ALTER_TABLE_PHASE_END
};


/*
  Flags stored in DDL_LOG_ENTRY.flags
  The flag values can be reused for different commands
*/
#define DDL_LOG_FLAG_ALTER_RENAME         (1 << 0)
#define DDL_LOG_FLAG_ALTER_ENGINE_CHANGED (1 << 1)
#define DDL_LOG_FLAG_ONLY_FRM             (1 << 2)
#define DDL_LOG_FLAG_UPDATE_STAT          (1 << 3)
/*
  Set when using ALTER TABLE on a partitioned table and the table
  engine is not changed
*/
#define DDL_LOG_FLAG_ALTER_PARTITION      (1 << 4)

/*
  Setting ddl_log_entry.phase to this has the same effect as setting
  the phase to the maximum phase (..PHASE_END) for an entry.
*/

#define DDL_LOG_FINAL_PHASE ((uchar) 0xff)

typedef struct st_ddl_log_entry
{
  LEX_CSTRING name;
  LEX_CSTRING from_name;
  LEX_CSTRING handler_name;
  LEX_CSTRING db;
  LEX_CSTRING from_db;
  LEX_CSTRING from_handler_name;
  LEX_CSTRING tmp_name;                /* frm file or temporary file name */
  LEX_CSTRING extra_name;              /* Backup table name */
  uchar uuid[MY_UUID_SIZE];            // UUID for new frm file

  ulonglong xid;                       // Xid stored in the binary log
  /*
    unique_id can be used to store a unique number to check current state.
    Currently it is used to store new size of frm file, link to another ddl log
    entry or store an a uniq version for a storage engine in alter table.
    For execute entries this is reused as an execute counter to ensure we
    don't repeat an entry too many times if executing the entry fails.
  */
  ulonglong unique_id;
  uint next_entry;
  uint entry_pos;                      // Set by write_dll_log_entry()
  uint16 flags;                        // Flags unique for each command
  enum ddl_log_entry_code entry_type;  // Set automatically
  enum ddl_log_action_code action_type;
  /*
    Most actions have only one phase. REPLACE does however have two
    phases. The first phase removes the file with the new name if
    there was one there before and the second phase renames the
    old name to the new name.
  */
  uchar phase;                         // set automatically
} DDL_LOG_ENTRY;

typedef struct st_ddl_log_memory_entry
{
  uint entry_pos;
  struct st_ddl_log_memory_entry *next_log_entry;
  struct st_ddl_log_memory_entry *prev_log_entry;
  struct st_ddl_log_memory_entry *next_active_log_entry;
} DDL_LOG_MEMORY_ENTRY;


/*
  State of the ddl log during execution of a DDL.

  A ddl log state has one execute entry (main entry pointing to the first
  action entry) and many 'action entries' linked in a list in the order
  they should be executed.
  One recovery the log is parsed and all execute entries will be executed.

  All entries are stored as separate blocks in the ddl recovery file.
*/

typedef struct st_ddl_log_state
{
  /* List of ddl log entries */
  DDL_LOG_MEMORY_ENTRY *list;
  /* One execute entry per list */
  DDL_LOG_MEMORY_ENTRY *execute_entry;
  /*
    Entry used for PHASE updates. Normally same as first in 'list', but in
    case of a query log event, this points to the main event.
  */
  DDL_LOG_MEMORY_ENTRY *main_entry;
  uint16 flags;                                 /* Cache for flags */
  bool is_active() { return list != 0; }
} DDL_LOG_STATE;


/* These functions are for recovery */
bool ddl_log_initialize();
void ddl_log_release();
bool ddl_log_close_binlogged_events(HASH *xids);
int ddl_log_execute_recovery();

/* functions for updating the ddl log */
bool ddl_log_write_entry(DDL_LOG_ENTRY *ddl_log_entry,
                           DDL_LOG_MEMORY_ENTRY **active_entry);

bool ddl_log_write_execute_entry(uint first_entry,
                                 DDL_LOG_MEMORY_ENTRY **active_entry);
bool ddl_log_disable_execute_entry(DDL_LOG_MEMORY_ENTRY **active_entry);

void ddl_log_complete(DDL_LOG_STATE *ddl_log_state);
bool ddl_log_revert(THD *thd, DDL_LOG_STATE *ddl_log_state);

bool ddl_log_update_phase(DDL_LOG_STATE *entry, uchar phase);
bool ddl_log_add_flag(DDL_LOG_STATE *entry, uint16 flag);
bool ddl_log_update_unique_id(DDL_LOG_STATE *state, ulonglong id);
bool ddl_log_update_xid(DDL_LOG_STATE *state, ulonglong xid);
bool ddl_log_disable_entry(DDL_LOG_STATE *state);
bool ddl_log_increment_phase(uint entry_pos);
void ddl_log_release_memory_entry(DDL_LOG_MEMORY_ENTRY *log_entry);
bool ddl_log_sync();
bool ddl_log_execute_entry(THD *thd, uint first_entry);

void ddl_log_release_entries(DDL_LOG_STATE *ddl_log_state);
bool ddl_log_rename_table(THD *thd, DDL_LOG_STATE *ddl_state,
                          handlerton *hton,
                          const LEX_CSTRING *org_db,
                          const LEX_CSTRING *org_alias,
                          const LEX_CSTRING *new_db,
                          const LEX_CSTRING *new_alias);
bool ddl_log_rename_view(THD *thd, DDL_LOG_STATE *ddl_state,
                         const LEX_CSTRING *org_db,
                         const LEX_CSTRING *org_alias,
                         const LEX_CSTRING *new_db,
                         const LEX_CSTRING *new_alias);
bool ddl_log_drop_table_init(THD *thd, DDL_LOG_STATE *ddl_state,
                             const LEX_CSTRING *db,
                             const LEX_CSTRING *comment);
bool ddl_log_drop_view_init(THD *thd, DDL_LOG_STATE *ddl_state,
                            const LEX_CSTRING *db);
bool ddl_log_drop_table(THD *thd, DDL_LOG_STATE *ddl_state,
                        handlerton *hton,
                        const LEX_CSTRING *path,
                        const LEX_CSTRING *db,
                        const LEX_CSTRING *table);
bool ddl_log_drop_view(THD *thd, DDL_LOG_STATE *ddl_state,
                        const LEX_CSTRING *path,
                        const LEX_CSTRING *db,
                        const LEX_CSTRING *table);
bool ddl_log_drop_trigger(THD *thd, DDL_LOG_STATE *ddl_state,
                          const LEX_CSTRING *db,
                          const LEX_CSTRING *table,
                          const LEX_CSTRING *trigger_name,
                          const LEX_CSTRING *query);
bool ddl_log_drop_view(THD *thd, DDL_LOG_STATE *ddl_state,
                        const LEX_CSTRING *path,
                        const LEX_CSTRING *db,
                        const LEX_CSTRING *table);
bool ddl_log_drop_view(THD *thd, DDL_LOG_STATE *ddl_state,
                       const LEX_CSTRING *db);
bool ddl_log_drop_db(THD *thd, DDL_LOG_STATE *ddl_state,
                     const LEX_CSTRING *db, const LEX_CSTRING *path);
bool ddl_log_create_table(THD *thd, DDL_LOG_STATE *ddl_state,
                          handlerton *hton,
                          const LEX_CSTRING *path,
                          const LEX_CSTRING *db,
                          const LEX_CSTRING *table,
                          bool only_frm);
bool ddl_log_create_view(THD *thd, DDL_LOG_STATE *ddl_state,
                         const LEX_CSTRING *path,
                         enum_ddl_log_create_view_phase phase);
bool ddl_log_delete_tmp_file(THD *thd, DDL_LOG_STATE *ddl_state,
                             const LEX_CSTRING *path,
                             DDL_LOG_STATE *depending_state);
bool ddl_log_create_trigger(THD *thd, DDL_LOG_STATE *ddl_state,
                            const LEX_CSTRING *db, const LEX_CSTRING *table,
                            const LEX_CSTRING *trigger_name,
                            enum_ddl_log_create_trigger_phase phase);
bool ddl_log_alter_table(THD *thd, DDL_LOG_STATE *ddl_state,
                         handlerton *org_hton,
                         const LEX_CSTRING *db, const LEX_CSTRING *table,
                         handlerton *new_hton,
                         handlerton *partition_underlying_hton,
                         const LEX_CSTRING *new_db,
                         const LEX_CSTRING *new_table,
                         const LEX_CSTRING *frm_path,
                         const LEX_CSTRING *backup_table_name,
                         const LEX_CUSTRING *version,
                         ulonglong table_version,
                         bool is_renamed);
bool ddl_log_store_query(THD *thd, DDL_LOG_STATE *ddl_log_state,
                         const char *query, size_t length);
extern mysql_mutex_t LOCK_gdl;
#endif /* DDL_LOG_INCLUDED */
