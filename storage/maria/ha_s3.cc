/* Copyright (C) 2019 MariaDB Corppration AB

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
  The s3 engine inherits from the ha_maria handler

  s3 will use it's own page cache to not interfere with normal Aria
  usage but also to ensure that the S3 page cache is large enough
  (with a 4M s3_block_size the engine will need a large cache to work,
  at least s3_block_size * 32. The default cache is 512M.
*/

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
static char *s3_tmp_access_key=0, *s3_tmp_secret_key=0;
static my_bool s3_debug= 0;
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
    s3_tmp_access_key= my_strdup("*****", MYF(MY_WME));
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
    s3_tmp_secret_key= my_strdup("*****", MYF(MY_WME));
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

static MYSQL_SYSVAR_ENUM(protocol_version, s3_protocol_version,
                         PLUGIN_VAR_RQCMDARG,
                         "Protocol used to communication with S3. One of "
                         "\"Amazon\" or \"Original\".",
                         NULL, NULL, 1, &s3_protocol_typelib);

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
      "AWS bucket",
       0, 0, DEFAULT_AWS_HOST_NAME);
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
  :ha_maria(hton, table_arg), in_alter_table(0)
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

int ha_s3::write_row(uchar *buf)
{
  if (in_alter_table)
    return ha_maria::write_row(buf);
  return HA_ERR_WRONG_COMMAND;
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
  info->protocol_version= (uint8_t) s3_protocol_version+1;
  lex_string_set(&info->host_name,  s3_host_name);
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
  return s3_info_init(s3_info);
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
  if (!strncmp(s3_info.table.str, "#sql-", 5))
    DBUG_RETURN(ha_maria::delete_table(name));

  if (error)
    DBUG_RETURN(HA_ERR_UNSUPPORTED);

  if (!(s3_client= s3_open_connection(&s3_info)))
    DBUG_RETURN(HA_ERR_NO_SUCH_TABLE);
  error= aria_delete_from_s3(s3_client, s3_info.bucket.str,
                             s3_info.database.str,
                             s3_info.table.str,0);
  ms3_deinit(s3_client);
  DBUG_RETURN(error);
}

/**
   Copy an Aria table to S3 or rename a table in S3

   The copy happens as part of the rename in ALTER TABLE when all data
   is in an Aria table and we now have to copy it to S3.

   If the table is an old table already in S3, we should just rename it.
*/

int ha_s3::rename_table(const char *from, const char *to)
{
  S3_INFO to_s3_info, from_s3_info;
  char to_name[FN_REFLEN], from_name[FN_REFLEN], frm_name[FN_REFLEN];
  ms3_st *s3_client;
  MY_STAT stat_info;
  int error;
  DBUG_ENTER("ha_s3::rename_table");

  if (s3_info_init(&to_s3_info, to, to_name, NAME_LEN))
    DBUG_RETURN(HA_ERR_UNSUPPORTED);
  if (!(s3_client= s3_open_connection(&to_s3_info)))
    DBUG_RETURN(HA_ERR_NO_SUCH_TABLE);

  /*
    Check if this is a on disk table created by ALTER TABLE that should be
    copied to S3. We know this is the case if the table is a temporary table
    and the .MAI file for the table is on disk
  */
  fn_format(frm_name, from, "", reg_ext, MYF(0));
  if (!strncmp(from + dirname_length(from), "#sql-", 5) &&
      my_stat(frm_name, &stat_info, MYF(0)))
  {
    /*
      The table is a temporary table as part of ALTER TABLE.
      Copy the on disk temporary Aria table to S3.
    */
    error= aria_copy_to_s3(s3_client, to_s3_info.bucket.str, from,
                           to_s3_info.database.str,
                           to_s3_info.table.str,
                           0, 0, 0, 0);
    if (!error)
    {
      /* Remove original files table files, keep .frm */
      fn_format(from_name, from, "", MARIA_NAME_DEXT,
                MY_APPEND_EXT|MY_UNPACK_FILENAME);
      my_delete(from_name, MYF(MY_WME | ME_WARNING));
      fn_format(from_name, from, "", MARIA_NAME_IEXT,
                MY_APPEND_EXT|MY_UNPACK_FILENAME);
      my_delete(from_name, MYF(MY_WME | ME_WARNING));
    }
  }
  else
  {
    /* The table is an internal S3 table. Do the renames */
    s3_info_init(&from_s3_info, from, from_name, NAME_LEN);

    error= aria_rename_s3(s3_client, to_s3_info.bucket.str,
                          from_s3_info.database.str,
                          from_s3_info.table.str,
                          to_s3_info.database.str,
                          to_s3_info.table.str);
  }
  ms3_deinit(s3_client);
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

  if (ha_create_info->tmp_table())
    DBUG_RETURN(HA_ERR_UNSUPPORTED);

  if (!s3_usable())
    DBUG_RETURN(HA_ERR_UNSUPPORTED);

  /* Force the table to a format suitable for S3 */
  ha_create_info->row_type= ROW_TYPE_PAGE;
  ha_create_info->transactional= HA_CHOICE_NO;
  error= ha_maria::create(name, table_arg, ha_create_info);
  if (error)
    DBUG_RETURN(error);

  /* Create the .frm file. Needed for ha_s3::rename_table() later  */
  if (!table_arg->s->read_frm_image((const uchar**) &frm_ptr, &frm_len))
  {
    table_arg->s->write_frm_image(frm_ptr, frm_len);
    table_arg->s->free_frm_image(frm_ptr);
  }

  DBUG_RETURN(0);
}

/**
   Open table


   @notes
   Table is read only, except if opened by ALTER as in this case we
   are creating the S3 table.
*/

int ha_s3::open(const char *name, int mode, uint open_flags)
{
  int res;
  S3_INFO s3_info;
  DBUG_ENTER("ha_s3:open");

  if (!s3_usable())
    DBUG_RETURN(HA_ERR_UNSUPPORTED);

  if (mode != O_RDONLY && !(open_flags & HA_OPEN_FOR_CREATE))
    DBUG_RETURN(EACCES);

  open_args= 0;
  if (!(open_flags & HA_OPEN_FOR_CREATE))
  {
    (void) s3_info_init(&s3_info);
    s3_info.tabledef_version= table->s->tabledef_version;
    
    /* Pass the above arguments to maria_open() */
    open_args= &s3_info;
  }

  if (!(res= ha_maria::open(name, mode, open_flags)))
  {
    if ((open_flags & HA_OPEN_FOR_CREATE))
      in_alter_table= 1;
    else
    {
      /*
        We have to modify the pagecache callbacks for the data file,
        index file and for bitmap handling
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
  Check if a table is in S3 as part of discovery
*/

static int s3_discover_table(handlerton *hton, THD* thd, TABLE_SHARE *share)
{
  S3_INFO s3_info;
  S3_BLOCK block;
  ms3_st *s3_client;
  int error;
  DBUG_ENTER("s3_discover_table");

  if (s3_info_init(&s3_info))
    DBUG_RETURN(HA_ERR_NO_SUCH_TABLE);
  if (!(s3_client= s3_open_connection(&s3_info)))
    DBUG_RETURN(HA_ERR_NO_SUCH_TABLE);

  s3_info.database=   share->db;
  s3_info.table=      share->table_name;

  if (s3_get_frm(s3_client, &s3_info, &block))
  {
    s3_free(&block);
    ms3_deinit(s3_client);
    DBUG_RETURN(HA_ERR_NO_SUCH_TABLE);
  }
  error= share->init_from_binary_frm_image(thd, 1,
                                           block.str, block.length);
  s3_free(&block);
  ms3_deinit(s3_client);
  DBUG_RETURN((my_errno= error));
}


/**
  Check if a table exists

   @return 0 frm doesn't exists
   @return 1 frm exists
*/

static int s3_discover_table_existance(handlerton *hton, const char *db,
                                       const char *table_name)
{
  S3_INFO s3_info;
  ms3_st *s3_client;
  int res;
  DBUG_ENTER("s3_discover_table_existance");

  if (s3_info_init(&s3_info))
    DBUG_RETURN(0);
  if (!(s3_client= s3_open_connection(&s3_info)))
    DBUG_RETURN(0);

  s3_info.database.str=    db;
  s3_info.database.length= strlen(db);
  s3_info.table.str=       table_name;
  s3_info.table.length=    strlen(table_name);

  res= s3_frm_exists(s3_client, &s3_info);
  ms3_deinit(s3_client);
  DBUG_RETURN(res == 0);                        // Return 1 if exists
}


/**
  Return a list of all S3 tables in a database
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

  if (s3_info_init(&s3_info))
    DBUG_RETURN(0);
  if (!(s3_client= s3_open_connection(&s3_info)))
    DBUG_RETURN(0);

  strxnmov(aws_path, sizeof(aws_path)-1, db->str, "/", NullS);

  if ((error= ms3_list_dir(s3_client, s3_info.bucket.str, aws_path, &org_list)))
    goto end;
  
  for (list= org_list ; list ; list= list->next)
  {
    const char *name= list->key + db->length + 1;   // Skip database and /
    size_t name_length= strlen(name)-1;             // Remove end /
    result->add_table(name, name_length);
  }
  if (org_list)
    ms3_list_free(org_list);
end:
  ms3_deinit(s3_client);
  DBUG_RETURN(0);
}

/**
  Update the .frm file in S3
*/

static int s3_notify_tabledef_changed(handlerton *hton __attribute__((unused)),
                                      LEX_CSTRING *db, LEX_CSTRING *table,
                                      LEX_CUSTRING *frm,
                                      LEX_CUSTRING *org_tabledef_version)
{
  char aws_path[AWS_PATH_LENGTH];
  S3_INFO s3_info;
  ms3_st *s3_client;
  int error= 0;
  DBUG_ENTER("s3_notify_tabledef_changed");

  if (s3_info_init(&s3_info))
    DBUG_RETURN(0);
  if (!(s3_client= s3_open_connection(&s3_info)))
    DBUG_RETURN(0);

  s3_info.database=    *db;
  s3_info.table=       *table;
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
  ms3_deinit(s3_client);
  DBUG_RETURN(error);
}
  

static int ha_s3_init(void *p)
{
  bool res;
  static const char *no_exts[]= { 0 };
  DBUG_ASSERT(maria_hton);

  s3_hton= (handlerton *)p;

  /* Use Aria engine as a base */
  memcpy(s3_hton, maria_hton, sizeof(*s3_hton));
  s3_hton->db_type= DB_TYPE_S3;
  s3_hton->create= s3_create_handler;
  s3_hton->panic=  s3_hton_panic;
  s3_hton->table_options= s3_table_option_list;
  s3_hton->discover_table= s3_discover_table;
  s3_hton->discover_table_names= s3_discover_table_names;
  s3_hton->discover_table_existence= s3_discover_table_existance;
  s3_hton->notify_tabledef_changed= s3_notify_tabledef_changed;
  s3_hton->tablefile_extensions= no_exts;
  s3_hton->commit= 0;
  s3_hton->rollback= 0;
  s3_hton->checkpoint_state= 0;
  s3_hton->flush_logs= 0;
  s3_hton->show_status= 0;
  s3_hton->prepare_for_backup= 0;
  s3_hton->end_backup= 0;
  s3_hton->flags= 0;
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
  return res ? HA_ERR_INITIALIZATION : 0;
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
  MYSQL_SYSVAR(bucket),
  MYSQL_SYSVAR(access_key),
  MYSQL_SYSVAR(secret_key),
  MYSQL_SYSVAR(region),

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
  NULL,                         /* Plugin Deinit    */
  0x0100,                       /* 1.0              */
  status_variables,             /* status variables */
  system_variables,             /* system variables */
  "1.0",                        /* string version   */
  MariaDB_PLUGIN_MATURITY_ALPHA /* maturity         */
}
maria_declare_plugin_end;
