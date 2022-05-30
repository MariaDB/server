/* Copyright (C) 2019, 2021 MariaDB Corporation Ab

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the
   Free Software Foundation, Inc.
   51 Franklin Street, Fifth Floor, Boston, MA 02111-1301 USA
*/

/*
  Implementation of S3 storage engine.

  Storage format:

  The S3 engine is read only storage engine. The data is stored in
  same format as a non transactional Aria table in BLOCK_RECORD format.
  This makes it easy to cache both index and rows in the page cache.
  Data and index file are split into blocks of 's3_block_size', default
  4M.

  The table and it's associated files are stored in S3 into the following
  locations:

  frm file (for discovery):
  aws_bucket/database/table/frm

  First index block (contains description if the Aria file):
  aws_bucket/database/table/aria

  Rest of the index file:
  aws_bucket/database/table/index/block_number

  Data file:
  aws_bucket/database/table/data/block_number

  block_number is 6 digits decimal number, prefixed with 0
  (Can be larger than 6 numbers, the prefix is just for nice output)

  frm and base blocks are small (just the needed data).
  index and blocks are of size 's3_block_size'

  If compression is used, then original block size is s3_block_size
  but the stored block will be the size of the compressed block.

  Implementation:
  The s3 engine inherits from the ha_maria handler.

  It uses Aria code and relies on Aria being enabled. We don't have to check
  that Aria is enabled though, because Aria is a mandatory plugin, and
  the server will refuse to start if Aria failed to initialize.

  s3 will use it's own page cache to not interfere with normal Aria
  usage but also to ensure that the S3 page cache is large enough
  (with a 4M s3_block_size the engine will need a large cache to work,
  at least s3_block_size * 32. The default cache is 512M.
*/

#define MYSQL_SERVER 1
#include <my_global.h>
#include <m_string.h>
#include "maria_def.h"
#include "sql_class.h"
#include <mysys_err.h>
#include <libmarias3/marias3.h>
#include <discover.h>
#include "ha_s3.h"
#include "s3_func.h"
#include "aria_backup.h"

#define DEFAULT_AWS_HOST_NAME "s3.amazonaws.com"

static PAGECACHE s3_pagecache;
static ulong s3_block_size, s3_protocol_version;
static ulong s3_pagecache_division_limit, s3_pagecache_age_threshold;
static ulong s3_pagecache_file_hash_size;
static ulonglong s3_pagecache_buffer_size;
static char *s3_bucket, *s3_access_key=0, *s3_secret_key=0, *s3_region;
static char *s3_host_name;
static int s3_port;
static my_bool s3_use_http;
static char *s3_tmp_access_key=0, *s3_tmp_secret_key=0;
static my_bool s3_debug= 0, s3_slave_ignore_updates= 0;
static my_bool s3_replicate_alter_as_create_select= 0;
handlerton *s3_hton= 0;

/* Don't show access or secret keys to users if they exists */

static void update_access_key(MYSQL_THD thd,
                              struct st_mysql_sys_var *var,
                              void *var_ptr, const void *save)
{
  my_free(s3_access_key);
  s3_access_key= 0;
  /* Don't show real key to user in SHOW VARIABLES */
  if (s3_tmp_access_key[0])
  {
    s3_access_key= s3_tmp_access_key;
    s3_tmp_access_key= my_strdup(PSI_NOT_INSTRUMENTED, "*****", MYF(MY_WME));
  }
}

static void update_secret_key(MYSQL_THD thd,
                              struct st_mysql_sys_var *var,
                              void *var_ptr, const void *save)
{
  my_free(s3_secret_key);
  s3_secret_key= 0;
  /* Don't show real key to user in SHOW VARIABLES */
  if (s3_tmp_secret_key[0])
  {
    s3_secret_key= s3_tmp_secret_key;
    s3_tmp_secret_key= my_strdup(PSI_NOT_INSTRUMENTED, "*****", MYF(MY_WME));
  }
}

/* Define system variables for S3 */

static MYSQL_SYSVAR_ULONG(block_size, s3_block_size,
       PLUGIN_VAR_RQCMDARG,
       "Block size for S3", 0, 0,
       4*1024*1024, 65536, 16*1024*1024, 8192);

static MYSQL_SYSVAR_BOOL(debug, s3_debug,
       PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
      "Generates trace file from libmarias3 on stderr for debugging",
       0, 0, 0);

static MYSQL_SYSVAR_BOOL(slave_ignore_updates, s3_slave_ignore_updates,
       PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
      "If the slave has shares same S3 storage as the master",
       0, 0, 0);

static MYSQL_SYSVAR_BOOL(replicate_alter_as_create_select,
                         s3_replicate_alter_as_create_select,
       PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
       "When converting S3 table to local table, log all rows in binary log",
       0, 0, 1);

static MYSQL_SYSVAR_ENUM(protocol_version, s3_protocol_version,
                         PLUGIN_VAR_RQCMDARG,
                         "Protocol used to communication with S3. One of "
                         "\"Auto\", \"Amazon\" or \"Original\".",
                         NULL, NULL, 0, &s3_protocol_typelib);

static MYSQL_SYSVAR_ULONG(pagecache_age_threshold,
       s3_pagecache_age_threshold, PLUGIN_VAR_RQCMDARG,
       "This characterizes the number of hits a hot block has to be untouched "
       "until it is considered aged enough to be downgraded to a warm block. "
       "This specifies the percentage ratio of that number of hits to the "
       "total number of blocks in the page cache.", 0, 0,
       300, 100, ~ (ulong) 0L, 100);

static MYSQL_SYSVAR_ULONGLONG(pagecache_buffer_size, s3_pagecache_buffer_size,
       PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
       "The size of the buffer used for index blocks for S3 tables. "
       "Increase this to get better index handling (for all reads and "
       "multiple writes) to as much as you can afford.", 0, 0,
        128*1024*1024, 1024*1024*32, ~(ulonglong) 0, 8192);

static MYSQL_SYSVAR_ULONG(pagecache_division_limit,
                          s3_pagecache_division_limit,
       PLUGIN_VAR_RQCMDARG,
       "The minimum percentage of warm blocks in key cache", 0, 0,
       100,  1, 100, 1);

static MYSQL_SYSVAR_ULONG(pagecache_file_hash_size,
                          s3_pagecache_file_hash_size,
       PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
       "Number of hash buckets for open files.  If you have a lot "
       "of S3 files open you should increase this for faster flush of "
       "changes. A good value is probably 1/10 of number of possible open "
       "S3 files.", 0,0, 512, 32, 16384, 1);

static MYSQL_SYSVAR_STR(bucket, s3_bucket,
       PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
      "AWS bucket",
       0, 0, "MariaDB");
static MYSQL_SYSVAR_STR(host_name, s3_host_name,
       PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
      "AWS host name",
       0, 0, DEFAULT_AWS_HOST_NAME);
static MYSQL_SYSVAR_INT(port, s3_port,
       PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
      "Port number to connect to (0 means use default)",
       NULL /*check*/, NULL /*update*/, 0 /*default*/,
       0 /*min*/, 65535 /*max*/, 1 /*blk*/);
static MYSQL_SYSVAR_BOOL(use_http, s3_use_http,
       PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
      "If true, force use of HTTP protocol",
       NULL /*check*/, NULL /*update*/, 0 /*default*/);
static MYSQL_SYSVAR_STR(access_key, s3_tmp_access_key,
       PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY | PLUGIN_VAR_MEMALLOC,
      "AWS access key",
       0, update_access_key, "");
static MYSQL_SYSVAR_STR(secret_key, s3_tmp_secret_key,
       PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY | PLUGIN_VAR_MEMALLOC,
      "AWS secret key",
       0, update_secret_key, "");
static MYSQL_SYSVAR_STR(region, s3_region,
       PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
      "AWS region",
       0, 0, "");

ha_create_table_option s3_table_option_list[]=
{
  /*
    one numeric option, with the default of UINT_MAX32, valid
    range of values 0..UINT_MAX32, and a "block size" of 10
    (any value must be divisible by 10).
  */
  HA_TOPTION_SYSVAR("s3_block_size", s3_block_size, block_size),
  HA_TOPTION_ENUM("compression_algorithm", compression_algorithm, "none,zlib",
                  0),
  HA_TOPTION_END
};


/*****************************************************************************
 S3 handler code
******************************************************************************/

/**
   Create S3 handler
*/


ha_s3::ha_s3(handlerton *hton, TABLE_SHARE *table_arg)
  :ha_maria(hton, table_arg), in_alter_table(S3_NO_ALTER)
{
  /* Remove things that S3 doesn't support */
  int_table_flags&= ~(HA_BINLOG_ROW_CAPABLE | HA_BINLOG_STMT_CAPABLE |
                      HA_CAN_EXPORT);
  can_enable_indexes= 0;
}


/**
   Remember the handler to use for s3_block_read()

   @note
   In the future the ms3_st objects could be stored in
   a list in share. In this case we would however need a mutex
   to access the next free one. By using st_my_thread_var we
   can avoid the mutex with the small cost of having to call
   register handler in all handler functions that will access
   the page cache
*/

void ha_s3::register_handler(MARIA_HA *file)
{
  struct st_my_thread_var *thread= my_thread_var;
  thread->keycache_file= (void*) file;
}


/**
   Write a row

   When generating the table as part of ALTER TABLE, writes are allowed.
   When table is moved to S3, writes are not allowed.
*/

int ha_s3::write_row(const uchar *buf)
{
  DBUG_ENTER("ha_s3::write_row");
  if (in_alter_table)
    DBUG_RETURN(ha_maria::write_row(buf));
  DBUG_RETURN(HA_ERR_TABLE_READONLY);
}

/* Return true if S3 can be used */

static my_bool s3_usable()
{
  return (s3_access_key != 0 && s3_secret_key != 0 && s3_region != 0 &&
          s3_bucket != 0);
}


static my_bool s3_info_init(S3_INFO *info)
{
  if (!s3_usable())
    return 1;
  info->protocol_version= (uint8_t) s3_protocol_version;
  lex_string_set(&info->host_name,  s3_host_name);
  info->port= s3_port;
  info->use_http= s3_use_http;
  lex_string_set(&info->access_key, s3_access_key);
  lex_string_set(&info->secret_key, s3_secret_key);
  lex_string_set(&info->region,     s3_region);
  lex_string_set(&info->bucket,     s3_bucket);
  return 0;
}


/**
   Fill information in S3_INFO including paths to table and database

   Notes:
     Database and table name are set even if s3 variables are not
     initialized. This is needed by s3::drop_table
*/

static my_bool s3_info_init(S3_INFO *s3_info, const char *path,
                            char *database_buff, size_t database_length)
{
  set_database_and_table_from_path(s3_info, path);
  /* Fix database as it's not \0 terminated */
  strmake(database_buff, s3_info->database.str,
          MY_MIN(database_length, s3_info->database.length));
  s3_info->database.str= database_buff;
  s3_info->base_table= s3_info->table;
  return s3_info_init(s3_info);
}

/*
  Check if table is a temporary table

  Returns 1 if table is a temporary table that should be stored in Aria
  (to later be copied to S3 with a name change)
*/

static int is_mariadb_internal_tmp_table(const char *table_name)
{
  int length;
  const int p_length= sizeof(tmp_file_prefix);  // prefix + '-'
  /* Temporary table from ALTER TABLE */
  if (!strncmp(table_name, tmp_file_prefix "-" , p_length))
  {
    /*
      Internal temporary tables used by ALTER TABLE and ALTER PARTITION
      should be stored in S3
    */
    if (!strncmp(table_name+p_length, "backup-", sizeof("backup-")-1) ||
        !strncmp(table_name+p_length, "exchange-", sizeof("exchange-")-1) ||
        !strncmp(table_name+p_length, "temptable-", sizeof("temptable-")-1))
      return 0;
    /* Other temporary tables should be stored in Aria on local disk */
    return 1;
  }
  length= strlen(table_name);
  if (length > 5 && !strncmp(table_name + length - 5, "#TMP#", 5))
    return 1;
  return 0;
}


/**
  Drop S3 table
*/

int ha_s3::delete_table(const char *name)
{
  ms3_st *s3_client;
  S3_INFO s3_info;
  int error;
  char database[NAME_LEN+1];
  DBUG_ENTER("ha_s3::delete_table");

  error= s3_info_init(&s3_info, name, database, sizeof(database)-1);

  /* If internal on disk temporary table, let Aria take care of it */
  if (is_mariadb_internal_tmp_table(s3_info.table.str))
    DBUG_RETURN(ha_maria::delete_table(name));

  if (error)
    DBUG_RETURN(HA_ERR_UNSUPPORTED);

  if (!(s3_client= s3_open_connection(&s3_info)))
    DBUG_RETURN(HA_ERR_NO_CONNECTION);
  error= aria_delete_from_s3(s3_client, s3_info.bucket.str,
                             s3_info.database.str,
                             s3_info.table.str,0);
  s3_deinit(s3_client);
  DBUG_RETURN(error);
}

/*
  The table is a temporary table as part of ALTER TABLE.

  Copy the on disk 'temporary' Aria table to S3 and delete the Aria table
*/

static int move_table_to_s3(ms3_st *s3_client,
                            S3_INFO *to_s3_info,
                            const char *local_name,
                            bool is_partition)
{
  int error;
  DBUG_ASSERT(!is_mariadb_internal_tmp_table(to_s3_info->table.str));

  if (!(error= aria_copy_to_s3(s3_client, to_s3_info->bucket.str, local_name,
                               to_s3_info->database.str,
                               to_s3_info->table.str,
                               0, 0, 1, 0, !is_partition)))
  {
    /* Table now in S3. Remove original files table files, keep .frm */
    error= maria_delete_table_files(local_name, 1, 0);
  }
  return error;
}


/**
   Copy an Aria table to S3 or rename a table in S3

   The copy happens as part of the rename in ALTER TABLE when all data
   is in an Aria table and we now have to copy it to S3.

   If the table is an old table already in S3, we should just rename it.
*/

int ha_s3::rename_table(const char *from, const char *to)
{
  S3_INFO to_s3_info;
  char to_name[NAME_LEN+1], frm_name[FN_REFLEN];
  ms3_st *s3_client;
  MY_STAT stat_info;
  int error;
  bool is_partition= (strstr(from, "#P#") != NULL) ||
                     (strstr(to, "#P#") != NULL);
  DBUG_ENTER("ha_s3::rename_table");

  if (s3_info_init(&to_s3_info, to, to_name, sizeof(to_name)-1))
    DBUG_RETURN(HA_ERR_UNSUPPORTED);
  if (!(s3_client= s3_open_connection(&to_s3_info)))
    DBUG_RETURN(HA_ERR_NO_SUCH_TABLE);

  /*
    Check if this is a on disk table created by ALTER TABLE that should be
    copied to S3. We know this is the case if the table is a temporary table
    and the .MAI file for the table is on disk
  */
  fn_format(frm_name, from, "", reg_ext, MYF(0));
  if (is_mariadb_internal_tmp_table(from + dirname_length(from)) &&
      (is_partition || my_stat(frm_name, &stat_info, MYF(0))))
  {
    error= move_table_to_s3(s3_client, &to_s3_info, from, is_partition);
  }
  else
  {
    char from_name[NAME_LEN+1];
    S3_INFO from_s3_info;
    /* The table is an internal S3 table. Do the renames */
    s3_info_init(&from_s3_info, from, from_name, sizeof(from_name)-1);

    if (is_mariadb_internal_tmp_table(to + dirname_length(to)))
    {
      /*
        The table is renamed to a temporary table. This only happens
        in the case of an ALTER PARTITION failure and there will be soon
        a delete issued for the temporary table. The only thing we can do
        is to remove the from table. We will get an extra errors for the
        uppcoming but we will ignore this minor problem for now as this
        is an unlikely event and the extra warnings are just annoying,
        not critical.
      */
      error= aria_delete_from_s3(s3_client, from_s3_info.bucket.str,
                                 from_s3_info.database.str,
                                 from_s3_info.table.str,0);
    }
    else
      error= aria_rename_s3(s3_client, to_s3_info.bucket.str,
                          from_s3_info.database.str,
                          from_s3_info.table.str,
                          to_s3_info.database.str,
                          to_s3_info.table.str,
                          !is_partition &&
                          !current_thd->lex->alter_info.partition_flags);
  }
  s3_deinit(s3_client);
  DBUG_RETURN(error);
}


/**
   Create a s3 table.

   @notes
   One can only create an s3 table as part of ALTER TABLE
   The table is created as a non transactional Aria table with
   BLOCK_RECORD format
*/

int ha_s3::create(const char *name, TABLE *table_arg,
                  HA_CREATE_INFO *ha_create_info)
{
  uchar *frm_ptr;
  size_t frm_len;
  int error;
  TABLE_SHARE *share= table_arg->s;
  DBUG_ENTER("ha_s3::create");

  if (!(ha_create_info->options & HA_CREATE_TMP_ALTER) ||
      ha_create_info->tmp_table())
    DBUG_RETURN(HA_ERR_WRONG_COMMAND);

  if (share->table_type == TABLE_TYPE_SEQUENCE)
    DBUG_RETURN(HA_ERR_UNSUPPORTED);

  /* When using partitions, S3 only supports adding and remove partitions */
  if ((table_arg->in_use->lex->alter_info.partition_flags &
       ~(ALTER_PARTITION_REMOVE | ALTER_PARTITION_ADD | ALTER_PARTITION_INFO)))
    DBUG_RETURN(HA_ERR_UNSUPPORTED);

  if (!s3_usable())
    DBUG_RETURN(HA_ERR_UNSUPPORTED);

  /* Force the table to a format suitable for S3 */
  ha_create_info->row_type= ROW_TYPE_PAGE;
  ha_create_info->transactional= HA_CHOICE_NO;
  error= ha_maria::create(name, table_arg, ha_create_info);
  if (error)
    DBUG_RETURN(error);

#ifdef MOVE_FILES_TO_S3_ON_CREATE
  /*
    If we are in ADD PARTITION and we created a new table (not
    temporary table, which will be moved as part of the final rename),
    we should move it S3 right away. The other option would to move
    it as part of close(). We prefer to do this here as there is no error
    checking with close() which would leave incomplete tables around in
    case of failures. The downside is that we can't move rows around as
    part of changing partitions, but that is not a big problem with S3
    as it's readonly anyway.
  */
  if (!is_mariadb_internal_tmp_table(name + dirname_length(name)) &&
      strstr(name, "#P#"))
  {
    S3_INFO to_s3_info;
    char database[NAME_LEN+1];
    ms3_st *s3_client;

    if (s3_info_init(&to_s3_info, name, database, sizeof(database)-1))
      DBUG_RETURN(HA_ERR_UNSUPPORTED);
    if (!(s3_client= s3_open_connection(&to_s3_info)))
      DBUG_RETURN(HA_ERR_NO_CONNECTION);

    /* Note that if error is set, then the empty temp table was not removed */
    error= move_table_to_s3(s3_client, &to_s3_info, name, 1);
    s3_deinit(s3_client);
    if (error)
      maria_delete_table_files(name, 1, 0);
  else
#endif /* MOVE_TABLE_TO_S3 */
  {
    /* Create the .frm file. Needed for ha_s3::rename_table() later  */
    if (!table_arg->s->read_frm_image((const uchar**) &frm_ptr, &frm_len))
    {
      table_arg->s->write_frm_image(frm_ptr, frm_len);
      table_arg->s->free_frm_image(frm_ptr);
    }
  }
  DBUG_RETURN(error);
}

/**
   Open table

   @notes
   Table is read only, except if opened by ALTER as in this case we
   are creating the S3 table.
*/

int ha_s3::open(const char *name, int mode, uint open_flags)
{
  bool internal_tmp_table= 0;
  int res;
  S3_INFO s3_info;
  DBUG_ENTER("ha_s3:open");

  if (!s3_usable())
    DBUG_RETURN(HA_ERR_UNSUPPORTED);

  /*
    On slaves with s3_slave_ignore_updates set we allow tables to be
    opened in write mode to be able to ignore queries that modify
    the table trough handler::check_if_updates_are_ignored().

    This is needed for the slave to be able to handle
    CREATE TABLE t1...
    INSERT INTO TABLE t1 ....
    ALTER TABLE t1 ENGINE=S3
    If this is not done, the insert will fail on the slave if the
    master has already executed the ALTER TABLE.

    We also have to allow open for create, as part of
    ALTER TABLE ... ENGINE=S3.

    Otherwise we only allow the table to be open in read mode
  */
  if (mode != O_RDONLY && !(open_flags & HA_OPEN_FOR_CREATE) &&
      !s3_slave_ignore_updates)
    DBUG_RETURN(EACCES);

  open_args= 0;
  internal_tmp_table= is_mariadb_internal_tmp_table(name +
                                                    dirname_length(name));

  if (!(open_flags & HA_OPEN_FOR_CREATE) && !internal_tmp_table)
  {
    (void) s3_info_init(&s3_info);
    s3_info.tabledef_version= table->s->tabledef_version;
    s3_info.base_table= table->s->table_name;

    /* Pass the above arguments to maria_open() */
    open_args= &s3_info;
    in_alter_table= S3_NO_ALTER;
  }
  else
  {
    /*
      Table was created as an Aria table that will be moved to S3 either
      by rename_table() or external_lock()
    */
    bool is_partition= (strstr(name, "#P#") != NULL);
    in_alter_table= (!is_partition ? S3_ALTER_TABLE :
                     internal_tmp_table ? S3_ADD_TMP_PARTITION :
                     S3_ADD_PARTITION);
  }
  DBUG_PRINT("info", ("in_alter_table: %d", in_alter_table));

  if (!(res= ha_maria::open(name, mode, open_flags)))
  {
    if (open_args)
    {
      /*
        Table is in S3. We have to modify the pagecache callbacks for the
        data file, index file and for bitmap handling.
      */
      file->s->pagecache= &s3_pagecache;
      file->dfile.big_block_size= file->s->kfile.big_block_size=
        file->s->bitmap.file.big_block_size= file->s->base.s3_block_size;
      file->s->kfile.head_blocks= file->s->base.keystart / file->s->block_size;
    }
  }
  open_args= 0;
  DBUG_RETURN(res);
}


int ha_s3::external_lock(THD * thd, int lock_type)
{
  int error;
  DBUG_ENTER("ha_s3::external_lock");

  error= ha_maria::external_lock(thd, lock_type);
  if (in_alter_table == S3_ADD_PARTITION && !error && lock_type == F_UNLCK)
  {
    /*
      This was a new partition. All data is now copied to the table
      so it's time to move it to S3)
    */

    MARIA_SHARE *share= file->s;
    uint org_open_count;

    /* First, flush all data to the Aria table */
    if (flush_pagecache_blocks(share->pagecache, &share->kfile,
                               FLUSH_RELEASE))
      error= my_errno;
    if (flush_pagecache_blocks(share->pagecache, &share->bitmap.file,
                               FLUSH_RELEASE))
      error= my_errno;
    org_open_count= share->state.open_count;
    if (share->global_changed)
      share->state.open_count--;
    if (_ma_state_info_write(share, MA_STATE_INFO_WRITE_DONT_MOVE_OFFSET |
                             MA_STATE_INFO_WRITE_LOCK))
      error= my_errno;
    share->state.open_count= org_open_count;

    if (!error)
    {
      S3_INFO to_s3_info;
      char database[NAME_LEN+1], *name= file->s->open_file_name.str;
      ms3_st *s3_client;

      /* Copy data to S3 */
      if (s3_info_init(&to_s3_info, name, database, sizeof(database)-1))
        DBUG_RETURN(HA_ERR_UNSUPPORTED);
      if (!(s3_client= s3_open_connection(&to_s3_info)))
        DBUG_RETURN(HA_ERR_NO_CONNECTION);

      /*
        Note that if error is set, then the empty temp table was not
        removed
      */
      error= move_table_to_s3(s3_client, &to_s3_info, name, 1);
      s3_deinit(s3_client);

      maria_delete_table_files(name, 1, 0);
    }
  }
  DBUG_RETURN(error);
}


/******************************************************************************
 Storage engine handler definitions
******************************************************************************/

/**
   Free all resources for s3
*/

static handler *s3_create_handler(handlerton *hton,
                                  TABLE_SHARE * table,
                                  MEM_ROOT *mem_root)
{
  return new (mem_root) ha_s3(hton, table);
}


static int s3_hton_panic(handlerton *hton, ha_panic_function flag)
{
  if (flag == HA_PANIC_CLOSE && s3_hton)
  {
    end_pagecache(&s3_pagecache, TRUE);
    s3_deinit_library();
    my_free(s3_access_key);
    my_free(s3_secret_key);
    s3_access_key= s3_secret_key= 0;
    s3_hton= 0;
  }
  return 0;
}


/**
  Check if a table is in S3 as part of discovery. Returns TABLE_SHARE if found.

  @param hton           S3 handlerton
  @param thd            MariaDB thd
  @param [out] share    If table exists, this is updated to contain the found
                        TABLE_SHARE (based on the .frm in S3)

  @return 0  Table exists
  @return #  Error number
*/

static int s3_discover_table(handlerton *hton, THD* thd, TABLE_SHARE *share)
{
  S3_INFO s3_info;
  S3_BLOCK frm_block, par_block;
  ms3_st *s3_client;
  int error;
  DBUG_ENTER("s3_discover_table");

  if (s3_info_init(&s3_info))
    DBUG_RETURN(HA_ERR_NO_SUCH_TABLE);
  if (!(s3_client= s3_open_connection(&s3_info)))
    DBUG_RETURN(HA_ERR_NO_CONNECTION);

  s3_info.database=   share->db;
  s3_info.table=      share->table_name;
  s3_info.base_table= share->table_name;

  if (s3_get_def(s3_client, &s3_info, &frm_block, "frm"))
  {
    s3_free(&frm_block);
    s3_deinit(s3_client);
    DBUG_RETURN(HA_ERR_NO_SUCH_TABLE);
  }
  (void) s3_get_def(s3_client, &s3_info, &par_block, "par");

  error= share->init_from_binary_frm_image(thd, 1,
                                           frm_block.str, frm_block.length,
                                           par_block.str, par_block.length);
  s3_free(&frm_block);
  s3_free(&par_block);
  s3_deinit(s3_client);
  DBUG_RETURN((my_errno= error));
}


/**
  Check if a table exists

   @return 0 frm doesn't exists
   @return 1 frm exists
*/

static int s3_discover_table_existence(handlerton *hton, const char *db,
                                       const char *table_name)
{
  S3_INFO s3_info;
  ms3_st *s3_client;
  int res;
  DBUG_ENTER("s3_discover_table_existence");

  /* Ignore names in "mysql" database to speed up boot */
  if (!strcmp(db, MYSQL_SCHEMA_NAME.str))
    DBUG_RETURN(0);

  if (s3_info_init(&s3_info))
    DBUG_RETURN(0);
  if (!(s3_client= s3_open_connection(&s3_info)))
    DBUG_RETURN(0);

  s3_info.database.str=    db;
  s3_info.database.length= strlen(db);
  s3_info.table.str=       table_name;
  s3_info.table.length=    strlen(table_name);

  res= s3_frm_exists(s3_client, &s3_info);
  s3_deinit(s3_client);
  DBUG_PRINT("exit", ("exists: %d", res == 0));
  DBUG_RETURN(res == 0);                        // Return 1 if exists
}


/**
  Return a list of all S3 tables in a database

  Partitoned tables are not shown
*/

static int s3_discover_table_names(handlerton *hton __attribute__((unused)),
                                   LEX_CSTRING *db,
                                   MY_DIR *dir __attribute__((unused)),
                                   handlerton::discovered_list *result)
{
  char aws_path[AWS_PATH_LENGTH];
  S3_INFO s3_info;
  ms3_st *s3_client;
  ms3_list_st *list, *org_list= 0;
  int error;
  DBUG_ENTER("s3_discover_table_names");

  /* Ignore names in "mysql" database to speed up boot */
  if (!strcmp(db->str, MYSQL_SCHEMA_NAME.str))
    DBUG_RETURN(0);

  if (s3_info_init(&s3_info))
    DBUG_RETURN(0);
  if (!(s3_client= s3_open_connection(&s3_info)))
    DBUG_RETURN(0);

  strxnmov(aws_path, sizeof(aws_path)-1, db->str, "/", NullS);

  if ((error= ms3_list_dir(s3_client, s3_info.bucket.str, aws_path, &org_list)))
    goto end;

  for (list= org_list ; list ; list= list->next)
  {
    const char *name= list->key + db->length + 1;   // Skip database and '/'
    if (!strstr(name, "#P#"))
    {
      size_t name_length= strlen(name)-1;             // Remove end '/'
      result->add_table(name, name_length);
    }
  }
  if (org_list)
    ms3_list_free(org_list);
end:
  s3_deinit(s3_client);
  DBUG_RETURN(0);
}

/*
  Check if definition of table in S3 is same as in MariaDB.
  This also covers the case where the table is not in S3 anymore.

  Called when a copy of the S3 table is taken from the MariaDB table cache

  TODO: Could possible be optimized by checking if the file on S3 is
        of same time, data and size since when table was originally opened.
*/

int ha_s3::discover_check_version()
{
  S3_INFO s3_info= *file->s->s3_path;
  s3_info.tabledef_version= table->s->tabledef_version;
  /*
    We have to change the database and table as the table may part of a
    partitoned table. In this case we want to check the frm file for the
    partitioned table, not the part table.
  */
  s3_info.base_table= table->s->table_name;
  return (s3_check_frm_version(file->s3, &s3_info) ?
          HA_ERR_TABLE_DEF_CHANGED : 0);
}


/**
  Update the .frm file in S3
*/

static int s3_notify_tabledef_changed(handlerton *,
                                      LEX_CSTRING *db, LEX_CSTRING *table,
                                      LEX_CUSTRING *frm,
                                      LEX_CUSTRING *org_tabledef_version,
                                      handler *)
{
  char aws_path[AWS_PATH_LENGTH];
  S3_INFO s3_info;
  ms3_st *s3_client;
  int error= 0;
  DBUG_ENTER("s3_notify_tabledef_changed");

  if (strstr(table->str, "#P#"))
    DBUG_RETURN(0);                             // Ignore partitions

  if (s3_info_init(&s3_info))
    DBUG_RETURN(0);
  if (!(s3_client= s3_open_connection(&s3_info)))
    DBUG_RETURN(0);

  s3_info.database=    *db;
  s3_info.base_table=  *table;
  s3_info.tabledef_version= *org_tabledef_version;
  if (s3_check_frm_version(s3_client, &s3_info))
  {
    error= 1;
    goto err;
  }

  strxnmov(aws_path, sizeof(aws_path)-1, db->str, "/", table->str, "/frm",
           NullS);

  if (s3_put_object(s3_client, s3_info.bucket.str, aws_path, (uchar*) frm->str,
                    frm->length, 0))
    error= 2;

err:
  s3_deinit(s3_client);
  DBUG_RETURN(error);
}


/**
   Update the .frm and .par file of a partitioned table stored in s3

   Logic is:
   - Skip temporary tables used internally by ALTER TABLE and ALTER PARTITION
   - In case of delete, delete the .frm and .par file from S3
   - In case of create, copy the .frm and .par files to S3
   - In case of rename:
      - Delete from old_path if not internal temporary file and if exists
      - Copy new .frm and .par file to S3

   To ensure that this works with the reply logic from ALTER PARTITION
   there should be no errors, only notes, for deletes.
*/

static int s3_create_partitioning_metadata(const char *path,
                                           const char *old_path,
                                           chf_create_flags action_flag)
{
  ms3_st *s3_client;
  S3_INFO s3_info;
  int error= 0;
  char database[NAME_LEN+1];
  const char *tmp_path;
  DBUG_ENTER("s3_create_partitioning_metadata");

  /* Path is empty in case of delete */
  tmp_path= path ? path : old_path;

  if (s3_info_init(&s3_info, tmp_path, database, sizeof(database)-1))
    DBUG_RETURN(HA_ERR_UNSUPPORTED);
  if (!(s3_client= s3_open_connection(&s3_info)))
    DBUG_RETURN(HA_ERR_NO_CONNECTION);

  switch (action_flag) {
  case CHF_DELETE_FLAG:
  case CHF_RENAME_FLAG:
  {
    if (!is_mariadb_internal_tmp_table(old_path + dirname_length(old_path)))
    {
      S3_INFO s3_info2;
      char database2[NAME_LEN+1];
      s3_info_init(&s3_info2, old_path, database2, sizeof(database2)-1);

      partition_delete_from_s3(s3_client, s3_info2.bucket.str,
                               s3_info2.database.str, s3_info2.table.str,
                               MYF(ME_NOTE));
    }
    if (action_flag == CHF_DELETE_FLAG)
      break;
  }
  /* Fall through */
  case CHF_CREATE_FLAG:
    if (!is_mariadb_internal_tmp_table(path + dirname_length(path)))
      error= partition_copy_to_s3(s3_client, s3_info.bucket.str,
                                  path, old_path,
                                  s3_info.database.str, s3_info.table.str);
    break;
  case CHF_INDEX_FLAG:
    break;
  }
  s3_deinit(s3_client);
  DBUG_RETURN(error);
}


/**
   Initialize s3 plugin
*/

static int ha_s3_init(void *p)
{
  bool res;
  static const char *no_exts[]= { 0 };

  s3_hton= (handlerton *)p;
  s3_hton->db_type= DB_TYPE_S3;
  s3_hton->create= s3_create_handler;
  s3_hton->panic=  s3_hton_panic;
  s3_hton->table_options= s3_table_option_list;
  s3_hton->discover_table= s3_discover_table;
  s3_hton->discover_table_names= s3_discover_table_names;
  s3_hton->discover_table_existence= s3_discover_table_existence;
  s3_hton->notify_tabledef_changed= s3_notify_tabledef_changed;
  s3_hton->create_partitioning_metadata= s3_create_partitioning_metadata;
  s3_hton->tablefile_extensions= no_exts;
  s3_hton->commit= 0;
  s3_hton->rollback= 0;
  s3_hton->checkpoint_state= 0;
  s3_hton->flush_logs= 0;
  s3_hton->show_status= 0;
  s3_hton->prepare_for_backup= 0;
  s3_hton->end_backup= 0;
  s3_hton->flags= ((s3_slave_ignore_updates ? HTON_IGNORE_UPDATES : 0) |
                   (s3_replicate_alter_as_create_select ?
                    HTON_TABLE_MAY_NOT_EXIST_ON_SLAVE : 0));
  /* Copy global arguments to s3_access_key and s3_secret_key */
  update_access_key(0,0,0,0);
  update_secret_key(0,0,0,0);

  if ((res= !init_pagecache(&s3_pagecache,
                            (size_t) s3_pagecache_buffer_size,
                            s3_pagecache_division_limit,
                            s3_pagecache_age_threshold, maria_block_size,
                            s3_pagecache_file_hash_size, 0)))
    s3_hton= 0;
  s3_pagecache.big_block_read= s3_block_read;
  s3_pagecache.big_block_free= s3_free;
  s3_init_library();
  if (s3_debug)
    ms3_debug();

  struct s3_func s3f_real =
  {
    ms3_set_option, s3_free, ms3_deinit, s3_unique_file_number,
    read_index_header, s3_check_frm_version, s3_info_copy,
    set_database_and_table_from_path, s3_open_connection
  };
  s3f= s3f_real;

  return res ? HA_ERR_INITIALIZATION : 0;
}

static int ha_s3_deinit(void*)
{
  bzero(&s3f, sizeof(s3f));
  return 0;
}

static SHOW_VAR status_variables[]= {
  {"pagecache_blocks_not_flushed",
   (char*) &s3_pagecache.global_blocks_changed, SHOW_LONG},
  {"pagecache_blocks_unused",
   (char*) &s3_pagecache.blocks_unused, SHOW_LONG},
  {"pagecache_blocks_used",
   (char*) &s3_pagecache.blocks_used, SHOW_LONG},
  {"pagecache_read_requests",
   (char*) &s3_pagecache.global_cache_r_requests, SHOW_LONGLONG},
  {"pagecache_reads",
   (char*) &s3_pagecache.global_cache_read, SHOW_LONGLONG},
  {NullS, NullS, SHOW_LONG}
};


static struct st_mysql_sys_var* system_variables[]= {
  MYSQL_SYSVAR(block_size),
  MYSQL_SYSVAR(debug),
  MYSQL_SYSVAR(protocol_version),
  MYSQL_SYSVAR(pagecache_age_threshold),
  MYSQL_SYSVAR(pagecache_buffer_size),
  MYSQL_SYSVAR(pagecache_division_limit),
  MYSQL_SYSVAR(pagecache_file_hash_size),
  MYSQL_SYSVAR(host_name),
  MYSQL_SYSVAR(port),
  MYSQL_SYSVAR(use_http),
  MYSQL_SYSVAR(bucket),
  MYSQL_SYSVAR(access_key),
  MYSQL_SYSVAR(secret_key),
  MYSQL_SYSVAR(region),
  MYSQL_SYSVAR(slave_ignore_updates),
  MYSQL_SYSVAR(replicate_alter_as_create_select),
  NULL
};

struct st_mysql_storage_engine s3_storage_engine=
{ MYSQL_HANDLERTON_INTERFACE_VERSION };

maria_declare_plugin(s3)
{
  MYSQL_STORAGE_ENGINE_PLUGIN,
  &s3_storage_engine,
  "S3",
  "MariaDB Corporation Ab",
  "Read only table stored in S3. Created by running "
  "ALTER TABLE table_name ENGINE=s3",
  PLUGIN_LICENSE_GPL,
  ha_s3_init,                   /* Plugin Init      */
  ha_s3_deinit,                 /* Plugin Deinit    */
  0x0100,                       /* 1.0              */
  status_variables,             /* status variables */
  system_variables,             /* system variables */
  "1.0",                        /* string version   */
  MariaDB_PLUGIN_MATURITY_STABLE/* maturity         */
}
maria_declare_plugin_end;
