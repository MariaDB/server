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

/* drop and alter of tables */

#include "mariadb.h"
#include "sql_priv.h"
#include "unireg.h"
#include "debug_sync.h"
#include "sql_table.h"
#include "sql_parse.h"                        // test_if_data_home_dir
#include "sql_cache.h"                          // query_cache_*
#include "sql_base.h"   // lock_table_names
#include "lock.h"       // mysql_unlock_tables
#include "strfunc.h"    // find_type2, find_set
#include "sql_truncate.h"                       // regenerate_locked_table 
#include "sql_partition.h"                      // mem_alloc_error,
                                                // partition_info
                                                // NOT_A_PARTITION_ID
#include "sql_db.h"                             // load_db_opt_by_name
#include "records.h"             // init_read_record, end_read_record
#include "filesort.h"            // filesort_free_buffers
#include "sql_select.h"                // setup_order
#include "sql_handler.h"               // mysql_ha_rm_tables
#include "discover.h"                  // readfrm
#include "my_pthread.h"                // pthread_mutex_t
#include "log_event.h"                 // Query_log_event
#include "sql_statistics.h"
#include <hash.h>
#include <myisam.h>
#include <my_dir.h>
#include "create_options.h"
#include "sp_head.h"
#include "sp.h"
#include "sql_trigger.h"
#include "sql_parse.h"
#include "sql_show.h"
#include "transaction.h"
#include "sql_audit.h"
#include "sql_sequence.h"
#include "tztime.h"
#include <algorithm>

#ifdef __WIN__
#include <io.h>
#endif

const char *primary_key_name="PRIMARY";

static int check_if_keyname_exists(const char *name,KEY *start, KEY *end);
static char *make_unique_key_name(THD *, const char *, KEY *, KEY *);
static bool make_unique_constraint_name(THD *, LEX_CSTRING *, const char *,
                                        List<Virtual_column_info> *, uint *);
static const char *make_unique_invisible_field_name(THD *, const char *,
                                                    List<Create_field> *);
static int copy_data_between_tables(THD *, TABLE *,TABLE *,
                                    List<Create_field> &, bool, uint, ORDER *,
                                    ha_rows *, ha_rows *,
                                    Alter_info::enum_enable_or_disable,
                                    Alter_table_ctx *);
static int mysql_prepare_create_table(THD *, HA_CREATE_INFO *, Alter_info *,
                                      uint *, handler *, KEY **, uint *, int);
static uint blob_length_by_type(enum_field_types type);
static bool fix_constraints_names(THD *, List<Virtual_column_info> *,
                                  const HA_CREATE_INFO *);

/**
  @brief Helper function for explain_filename
  @param thd          Thread handle
  @param to_p         Explained name in system_charset_info
  @param end_p        End of the to_p buffer
  @param name         Name to be converted
  @param name_len     Length of the name, in bytes
*/
static char* add_identifier(THD* thd, char *to_p, const char * end_p,
                            const char* name, size_t name_len)
{
  uint res;
  uint errors;
  const char *conv_name, *conv_name_end;
  char tmp_name[FN_REFLEN];
  char conv_string[FN_REFLEN];
  int quote;

  DBUG_ENTER("add_identifier");
  if (!name[name_len])
    conv_name= name;
  else
  {
    strnmov(tmp_name, name, name_len);
    tmp_name[name_len]= 0;
    conv_name= tmp_name;
  }
  res= strconvert(&my_charset_filename, conv_name, name_len,
                  system_charset_info,
                  conv_string, FN_REFLEN, &errors);
  if (unlikely(!res || errors))
  {
    DBUG_PRINT("error", ("strconvert of '%s' failed with %u (errors: %u)", conv_name, res, errors));
    conv_name= name;
    conv_name_end= name + name_len;
  }
  else
  {
    DBUG_PRINT("info", ("conv '%s' -> '%s'", conv_name, conv_string));
    conv_name= conv_string;
    conv_name_end= conv_string + res;
  }

  quote= (likely(thd) ?
          get_quote_char_for_identifier(thd, conv_name, res - 1) :
          '`');

  if (quote != EOF && (end_p - to_p > 2))
  {
    *(to_p++)= (char) quote;
    while (*conv_name && (end_p - to_p - 1) > 0)
    {
      int length= my_charlen(system_charset_info, conv_name, conv_name_end);
      if (length <= 0)
        length= 1;
      if (length == 1 && *conv_name == (char) quote)
      { 
        if ((end_p - to_p) < 3)
          break;
        *(to_p++)= (char) quote;
        *(to_p++)= *(conv_name++);
      }
      else if (((long) length) < (end_p - to_p))
      {
        to_p= strnmov(to_p, conv_name, length);
        conv_name+= length;
      }
      else
        break;                               /* string already filled */
    }
    if (end_p > to_p) {
      *(to_p++)= (char) quote;
      if (end_p > to_p)
	*to_p= 0; /* terminate by NUL, but do not include it in the count */
    }
  }
  else
    to_p= strnmov(to_p, conv_name, end_p - to_p);
  DBUG_RETURN(to_p);
}


/**
  @brief Explain a path name by split it to database, table etc.
  
  @details Break down the path name to its logic parts
  (database, table, partition, subpartition).
  filename_to_tablename cannot be used on partitions, due to the #P# part.
  There can be up to 6 '#', #P# for partition, #SP# for subpartition
  and #TMP# or #REN# for temporary or renamed partitions.
  This should be used when something should be presented to a user in a
  diagnostic, error etc. when it would be useful to know what a particular
  file [and directory] means. Such as SHOW ENGINE STATUS, error messages etc.

  Examples:

    t1#P#p1                 table t1 partition p1
    t1#P#p1#SP#sp1          table t1 partition p1 subpartition sp1
    t1#P#p1#SP#sp1#TMP#     table t1 partition p1 subpartition sp1 temporary
    t1#P#p1#SP#sp1#REN#     table t1 partition p1 subpartition sp1 renamed

   @param      thd          Thread handle
   @param      from         Path name in my_charset_filename
                            Null terminated in my_charset_filename, normalized
                            to use '/' as directory separation character.
   @param      to           Explained name in system_charset_info
   @param      to_length    Size of to buffer
   @param      explain_mode Requested output format.
                            EXPLAIN_ALL_VERBOSE ->
                            [Database `db`, ]Table `tbl`[,[ Temporary| Renamed]
                            Partition `p` [, Subpartition `sp`]]
                            EXPLAIN_PARTITIONS_VERBOSE -> `db`.`tbl`
                            [[ Temporary| Renamed] Partition `p`
                            [, Subpartition `sp`]]
                            EXPLAIN_PARTITIONS_AS_COMMENT -> `db`.`tbl` |*
                            [,[ Temporary| Renamed] Partition `p`
                            [, Subpartition `sp`]] *|
                            (| is really a /, and it is all in one line)

   @retval     Length of returned string
*/

uint explain_filename(THD* thd,
		      const char *from,
                      char *to,
                      uint to_length,
                      enum_explain_filename_mode explain_mode)
{
  char *to_p= to;
  char *end_p= to_p + to_length;
  const char *db_name= NULL;
  size_t  db_name_len= 0;
  const char *table_name;
  size_t  table_name_len= 0;
  const char *part_name= NULL;
  size_t  part_name_len= 0;
  const char *subpart_name= NULL;
  size_t  subpart_name_len= 0;
  uint part_type= NORMAL_PART_NAME;

  const char *tmp_p;
  DBUG_ENTER("explain_filename");
  DBUG_PRINT("enter", ("from '%s'", from));
  tmp_p= from;
  table_name= from;
  /*
    If '/' then take last directory part as database.
    '/' is the directory separator, not FN_LIB_CHAR
  */
  while ((tmp_p= strchr(tmp_p, '/')))
  {
    db_name= table_name;
    /* calculate the length */
    db_name_len= (int)(tmp_p - db_name);
    tmp_p++;
    table_name= tmp_p;
  }
  tmp_p= table_name;
  /* Look if there are partition tokens in the table name. */
  while ((tmp_p= strchr(tmp_p, '#')))
  {
    tmp_p++;
    switch (tmp_p[0]) {
    case 'P':
    case 'p':
      if (tmp_p[1] == '#')
      {
        part_name= tmp_p + 2;
        tmp_p+= 2;
      }
      break;
    case 'S':
    case 's':
      if ((tmp_p[1] == 'P' || tmp_p[1] == 'p') && tmp_p[2] == '#')
      {
        part_name_len= (int)(tmp_p - part_name - 1);
        subpart_name= tmp_p + 3;
	tmp_p+= 3;
      }
      break;
    case 'T':
    case 't':
      if ((tmp_p[1] == 'M' || tmp_p[1] == 'm') &&
          (tmp_p[2] == 'P' || tmp_p[2] == 'p') &&
          tmp_p[3] == '#' && !tmp_p[4])
      {
        part_type= TEMP_PART_NAME;
        tmp_p+= 4;
      }
      break;
    case 'R':
    case 'r':
      if ((tmp_p[1] == 'E' || tmp_p[1] == 'e') &&
          (tmp_p[2] == 'N' || tmp_p[2] == 'n') &&
          tmp_p[3] == '#' && !tmp_p[4])
      {
        part_type= RENAMED_PART_NAME;
        tmp_p+= 4;
      }
      break;
    default:
      /* Not partition name part. */
      ;
    }
  }
  if (part_name)
  {
    table_name_len= (int)(part_name - table_name - 3);
    if (subpart_name)
      subpart_name_len= strlen(subpart_name);
    else
      part_name_len= strlen(part_name);
    if (part_type != NORMAL_PART_NAME)
    {
      if (subpart_name)
        subpart_name_len-= 5;
      else
        part_name_len-= 5;
    }
  }
  else
    table_name_len= strlen(table_name);
  if (db_name)
  {
    if (explain_mode == EXPLAIN_ALL_VERBOSE)
    {
      to_p= strnmov(to_p, ER_THD_OR_DEFAULT(thd, ER_DATABASE_NAME),
                                            end_p - to_p);
      *(to_p++)= ' ';
      to_p= add_identifier(thd, to_p, end_p, db_name, db_name_len);
      to_p= strnmov(to_p, ", ", end_p - to_p);
    }
    else
    {
      to_p= add_identifier(thd, to_p, end_p, db_name, db_name_len);
      to_p= strnmov(to_p, ".", end_p - to_p);
    }
  }
  if (explain_mode == EXPLAIN_ALL_VERBOSE)
  {
    to_p= strnmov(to_p, ER_THD_OR_DEFAULT(thd, ER_TABLE_NAME), end_p - to_p);
    *(to_p++)= ' ';
    to_p= add_identifier(thd, to_p, end_p, table_name, table_name_len);
  }
  else
    to_p= add_identifier(thd, to_p, end_p, table_name, table_name_len);
  if (part_name)
  {
    if (explain_mode == EXPLAIN_PARTITIONS_AS_COMMENT)
      to_p= strnmov(to_p, " /* ", end_p - to_p);
    else if (explain_mode == EXPLAIN_PARTITIONS_VERBOSE)
      to_p= strnmov(to_p, " ", end_p - to_p);
    else
      to_p= strnmov(to_p, ", ", end_p - to_p);
    if (part_type != NORMAL_PART_NAME)
    {
      if (part_type == TEMP_PART_NAME)
        to_p= strnmov(to_p, ER_THD_OR_DEFAULT(thd, ER_TEMPORARY_NAME),
                      end_p - to_p);
      else
        to_p= strnmov(to_p, ER_THD_OR_DEFAULT(thd, ER_RENAMED_NAME),
                      end_p - to_p);
      to_p= strnmov(to_p, " ", end_p - to_p);
    }
    to_p= strnmov(to_p, ER_THD_OR_DEFAULT(thd, ER_PARTITION_NAME),
                  end_p - to_p);
    *(to_p++)= ' ';
    to_p= add_identifier(thd, to_p, end_p, part_name, part_name_len);
    if (subpart_name)
    {
      to_p= strnmov(to_p, ", ", end_p - to_p);
      to_p= strnmov(to_p, ER_THD_OR_DEFAULT(thd, ER_SUBPARTITION_NAME),
                    end_p - to_p);
      *(to_p++)= ' ';
      to_p= add_identifier(thd, to_p, end_p, subpart_name, subpart_name_len);
    }
    if (explain_mode == EXPLAIN_PARTITIONS_AS_COMMENT)
      to_p= strnmov(to_p, " */", end_p - to_p);
  }
  DBUG_PRINT("exit", ("to '%s'", to));
  DBUG_RETURN((uint)(to_p - to));
}


/*
  Translate a file name to a table name (WL #1324).

  SYNOPSIS
    filename_to_tablename()
      from                      The file name in my_charset_filename.
      to                OUT     The table name in system_charset_info.
      to_length                 The size of the table name buffer.

  RETURN
    Table name length.
*/

uint filename_to_tablename(const char *from, char *to, size_t to_length, 
                           bool stay_quiet)
{
  uint errors;
  size_t res;
  DBUG_ENTER("filename_to_tablename");
  DBUG_PRINT("enter", ("from '%s'", from));

  res= strconvert(&my_charset_filename, from, FN_REFLEN,
                  system_charset_info,  to, to_length, &errors);
  if (unlikely(errors)) // Old 5.0 name
  {
    res= (strxnmov(to, to_length, MYSQL50_TABLE_NAME_PREFIX,  from, NullS) -
          to);
    if (!stay_quiet)
      sql_print_error("Invalid (old?) table or database name '%s'", from);
  }

  DBUG_PRINT("exit", ("to '%s'", to));
  DBUG_RETURN((uint)res);
}


/**
  Check if given string begins with "#mysql50#" prefix
  
  @param   name          string to check cut 
  
  @retval
    FALSE  no prefix found
  @retval
    TRUE   prefix found
*/

bool check_mysql50_prefix(const char *name)
{
  return (name[0] == '#' && 
         !strncmp(name, MYSQL50_TABLE_NAME_PREFIX,
                  MYSQL50_TABLE_NAME_PREFIX_LENGTH));
}


/**
  Check if given string begins with "#mysql50#" prefix, cut it if so.
  
  @param   from          string to check and cut 
  @param   to[out]       buffer for result string
  @param   to_length     its size
  
  @retval
    0      no prefix found
  @retval
    non-0  result string length
*/

uint check_n_cut_mysql50_prefix(const char *from, char *to, size_t to_length)
{
  if (check_mysql50_prefix(from))
    return (uint) (strmake(to, from + MYSQL50_TABLE_NAME_PREFIX_LENGTH,
                           to_length - 1) - to);
  return 0;
}


static bool check_if_frm_exists(char *path, const char *db, const char *table)
{
  fn_format(path, table, db, reg_ext, MYF(0));
  return !access(path, F_OK);
}


/*
  Translate a table name to a file name (WL #1324).

  SYNOPSIS
    tablename_to_filename()
      from                      The table name in system_charset_info.
      to                OUT     The file name in my_charset_filename.
      to_length                 The size of the file name buffer.

  RETURN
    File name length.
*/

uint tablename_to_filename(const char *from, char *to, size_t to_length)
{
  uint errors, length;
  DBUG_ENTER("tablename_to_filename");
  DBUG_PRINT("enter", ("from '%s'", from));

  if ((length= check_n_cut_mysql50_prefix(from, to, to_length)))
  {
    /*
      Check if the name supplied is a valid mysql 5.0 name and 
      make the name a zero length string if it's not.
      Note that just returning zero length is not enough : 
      a lot of places don't check the return value and expect 
      a zero terminated string.
    */  
    if (check_table_name(to, length, TRUE))
    {
      to[0]= 0;
      length= 0;
    }
    DBUG_RETURN(length);
  }
  length= strconvert(system_charset_info, from, FN_REFLEN,
                     &my_charset_filename, to, to_length, &errors);
  if (check_if_legal_tablename(to) &&
      length + 4 < to_length)
  {
    memcpy(to + length, "@@@", 4);
    length+= 3;
  }
  DBUG_PRINT("exit", ("to '%s'", to));
  DBUG_RETURN(length);
}


/*
  Creates path to a file: mysql_data_dir/db/table.ext

  SYNOPSIS
   build_table_filename()
     buff                       Where to write result in my_charset_filename.
                                This may be the same as table_name.
     bufflen                    buff size
     db                         Database name in system_charset_info.
     table_name                 Table name in system_charset_info.
     ext                        File extension.
     flags                      FN_FROM_IS_TMP or FN_TO_IS_TMP or FN_IS_TMP
                                table_name is temporary, do not change.

  NOTES

    Uses database and table name, and extension to create
    a file name in mysql_data_dir. Database and table
    names are converted from system_charset_info into "fscs".
    Unless flags indicate a temporary table name.
    'db' is always converted.
    'ext' is not converted.

    The conversion suppression is required for ALTER TABLE. This
    statement creates intermediate tables. These are regular
    (non-temporary) tables with a temporary name. Their path names must
    be derivable from the table name. So we cannot use
    build_tmptable_filename() for them.

  RETURN
    path length
*/

uint build_table_filename(char *buff, size_t bufflen, const char *db,
                          const char *table_name, const char *ext, uint flags)
{
  char dbbuff[FN_REFLEN];
  char tbbuff[FN_REFLEN];
  DBUG_ENTER("build_table_filename");
  DBUG_PRINT("enter", ("db: '%s'  table_name: '%s'  ext: '%s'  flags: %x",
                       db, table_name, ext, flags));

  (void) tablename_to_filename(db, dbbuff, sizeof(dbbuff));

  /* Check if this is a temporary table name. Allow it if a corresponding .frm file exists */
  if (is_prefix(table_name, tmp_file_prefix) && strlen(table_name) < NAME_CHAR_LEN &&
      check_if_frm_exists(tbbuff, dbbuff, table_name))
    flags|= FN_IS_TMP;

  if (flags & FN_IS_TMP) // FN_FROM_IS_TMP | FN_TO_IS_TMP
    strmake(tbbuff, table_name, sizeof(tbbuff)-1);
  else
    (void) tablename_to_filename(table_name, tbbuff, sizeof(tbbuff));

  char *end = buff + bufflen;
  /* Don't add FN_ROOTDIR if mysql_data_home already includes it */
  char *pos = strnmov(buff, mysql_data_home, bufflen);
  size_t rootdir_len= strlen(FN_ROOTDIR);
  if (pos - rootdir_len >= buff &&
      memcmp(pos - rootdir_len, FN_ROOTDIR, rootdir_len) != 0)
    pos= strnmov(pos, FN_ROOTDIR, end - pos);
  pos= strxnmov(pos, end - pos, dbbuff, FN_ROOTDIR, NullS);
#ifdef USE_SYMDIR
  if (!(flags & SKIP_SYMDIR_ACCESS))
  {
    unpack_dirname(buff, buff);
    pos= strend(buff);
  }
#endif
  pos= strxnmov(pos, end - pos, tbbuff, ext, NullS);

  DBUG_PRINT("exit", ("buff: '%s'", buff));
  DBUG_RETURN((uint)(pos - buff));
}


/**
  Create path to a temporary table mysql_tmpdir/#sql1234_12_1
  (i.e. to its .FRM file but without an extension).

  @param thd      The thread handle.
  @param buff     Where to write result in my_charset_filename.
  @param bufflen  buff size

  @note
    Uses current_pid, thread_id, and tmp_table counter to create
    a file name in mysql_tmpdir.

  @return Path length.
*/

uint build_tmptable_filename(THD* thd, char *buff, size_t bufflen)
{
  DBUG_ENTER("build_tmptable_filename");

  char *p= strnmov(buff, mysql_tmpdir, bufflen);
  my_snprintf(p, bufflen - (p - buff), "/%s%lx_%llx_%x",
              tmp_file_prefix, current_pid,
              thd->thread_id, thd->tmp_table++);

  if (lower_case_table_names)
  {
    /* Convert all except tmpdir to lower case */
    my_casedn_str(files_charset_info, p);
  }

  size_t length= unpack_filename(buff, buff);
  DBUG_PRINT("exit", ("buff: '%s'", buff));
  DBUG_RETURN((uint)length);
}

/*
--------------------------------------------------------------------------

   MODULE: DDL log
   -----------------

   This module is used to ensure that we can recover from crashes that occur
   in the middle of a meta-data operation in MySQL. E.g. DROP TABLE t1, t2;
   We need to ensure that both t1 and t2 are dropped and not only t1 and
   also that each table drop is entirely done and not "half-baked".

   To support this we create log entries for each meta-data statement in the
   ddl log while we are executing. These entries are dropped when the
   operation is completed.

   At recovery those entries that were not completed will be executed.

   There is only one ddl log in the system and it is protected by a mutex
   and there is a global struct that contains information about its current
   state.

   History:
   First version written in 2006 by Mikael Ronstrom
--------------------------------------------------------------------------
*/

struct st_global_ddl_log
{
  /*
    We need to adjust buffer size to be able to handle downgrades/upgrades
    where IO_SIZE has changed. We'll set the buffer size such that we can
    handle that the buffer size was upto 4 times bigger in the version
    that wrote the DDL log.
  */
  char file_entry_buf[4*IO_SIZE];
  char file_name_str[FN_REFLEN];
  char *file_name;
  DDL_LOG_MEMORY_ENTRY *first_free;
  DDL_LOG_MEMORY_ENTRY *first_used;
  uint num_entries;
  File file_id;
  uint name_len;
  uint io_size;
  bool inited;
  bool do_release;
  bool recovery_phase;
  st_global_ddl_log() : inited(false), do_release(false) {}
};

st_global_ddl_log global_ddl_log;

mysql_mutex_t LOCK_gdl;

#define DDL_LOG_ENTRY_TYPE_POS 0
#define DDL_LOG_ACTION_TYPE_POS 1
#define DDL_LOG_PHASE_POS 2
#define DDL_LOG_NEXT_ENTRY_POS 4
#define DDL_LOG_NAME_POS 8

#define DDL_LOG_NUM_ENTRY_POS 0
#define DDL_LOG_NAME_LEN_POS 4
#define DDL_LOG_IO_SIZE_POS 8

/**
  Read one entry from ddl log file.

  @param entry_no                     Entry number to read

  @return Operation status
    @retval true   Error
    @retval false  Success
*/

static bool read_ddl_log_file_entry(uint entry_no)
{
  bool error= FALSE;
  File file_id= global_ddl_log.file_id;
  uchar *file_entry_buf= (uchar*)global_ddl_log.file_entry_buf;
  size_t io_size= global_ddl_log.io_size;
  DBUG_ENTER("read_ddl_log_file_entry");

  mysql_mutex_assert_owner(&LOCK_gdl);
  if (mysql_file_pread(file_id, file_entry_buf, io_size, io_size * entry_no,
                       MYF(MY_WME)) != io_size)
    error= TRUE;
  DBUG_RETURN(error);
}


/**
  Write one entry to ddl log file.

  @param entry_no                     Entry number to write

  @return Operation status
    @retval true   Error
    @retval false  Success
*/

static bool write_ddl_log_file_entry(uint entry_no)
{
  bool error= FALSE;
  File file_id= global_ddl_log.file_id;
  uchar *file_entry_buf= (uchar*)global_ddl_log.file_entry_buf;
  DBUG_ENTER("write_ddl_log_file_entry");

  mysql_mutex_assert_owner(&LOCK_gdl);
  if (mysql_file_pwrite(file_id, file_entry_buf,
                        IO_SIZE, IO_SIZE * entry_no, MYF(MY_WME)) != IO_SIZE)
    error= TRUE;
  DBUG_RETURN(error);
}


/**
  Sync the ddl log file.

  @return Operation status
    @retval FALSE  Success
    @retval TRUE   Error
*/


static bool sync_ddl_log_file()
{
  DBUG_ENTER("sync_ddl_log_file");
  DBUG_RETURN(mysql_file_sync(global_ddl_log.file_id, MYF(MY_WME)));
}


/**
  Write ddl log header.

  @return Operation status
    @retval TRUE                      Error
    @retval FALSE                     Success
*/

static bool write_ddl_log_header()
{
  uint16 const_var;
  DBUG_ENTER("write_ddl_log_header");

  int4store(&global_ddl_log.file_entry_buf[DDL_LOG_NUM_ENTRY_POS],
            global_ddl_log.num_entries);
  const_var= FN_REFLEN;
  int4store(&global_ddl_log.file_entry_buf[DDL_LOG_NAME_LEN_POS],
            (ulong) const_var);
  const_var= IO_SIZE;
  int4store(&global_ddl_log.file_entry_buf[DDL_LOG_IO_SIZE_POS],
            (ulong) const_var);
  if (write_ddl_log_file_entry(0UL))
  {
    sql_print_error("Error writing ddl log header");
    DBUG_RETURN(TRUE);
  }
  DBUG_RETURN(sync_ddl_log_file());
}


/**
  Create ddl log file name.
  @param file_name                   Filename setup
*/

static inline void create_ddl_log_file_name(char *file_name)
{
  strxmov(file_name, mysql_data_home, "/", "ddl_log.log", NullS);
}


/**
  Read header of ddl log file.

  When we read the ddl log header we get information about maximum sizes
  of names in the ddl log and we also get information about the number
  of entries in the ddl log.

  @return Last entry in ddl log (0 if no entries)
*/

static uint read_ddl_log_header()
{
  uchar *file_entry_buf= (uchar*)global_ddl_log.file_entry_buf;
  char file_name[FN_REFLEN];
  uint entry_no;
  bool successful_open= FALSE;
  DBUG_ENTER("read_ddl_log_header");

  mysql_mutex_init(key_LOCK_gdl, &LOCK_gdl, MY_MUTEX_INIT_SLOW);
  mysql_mutex_lock(&LOCK_gdl);
  create_ddl_log_file_name(file_name);
  if ((global_ddl_log.file_id= mysql_file_open(key_file_global_ddl_log,
                                               file_name,
                                               O_RDWR | O_BINARY, MYF(0))) >= 0)
  {
    if (read_ddl_log_file_entry(0UL))
    {
      /* Write message into error log */
      sql_print_error("Failed to read ddl log file in recovery");
    }
    else
      successful_open= TRUE;
  }
  if (successful_open)
  {
    entry_no= uint4korr(&file_entry_buf[DDL_LOG_NUM_ENTRY_POS]);
    global_ddl_log.name_len= uint4korr(&file_entry_buf[DDL_LOG_NAME_LEN_POS]);
    global_ddl_log.io_size= uint4korr(&file_entry_buf[DDL_LOG_IO_SIZE_POS]);
    DBUG_ASSERT(global_ddl_log.io_size <=
                sizeof(global_ddl_log.file_entry_buf));
  }
  else
  {
    entry_no= 0;
  }
  global_ddl_log.first_free= NULL;
  global_ddl_log.first_used= NULL;
  global_ddl_log.num_entries= 0;
  global_ddl_log.do_release= true;
  mysql_mutex_unlock(&LOCK_gdl);
  DBUG_RETURN(entry_no);
}


/**
  Convert from ddl_log_entry struct to file_entry_buf binary blob.

  @param ddl_log_entry   filled in ddl_log_entry struct.
*/

static void set_global_from_ddl_log_entry(const DDL_LOG_ENTRY *ddl_log_entry)
{
  mysql_mutex_assert_owner(&LOCK_gdl);
  global_ddl_log.file_entry_buf[DDL_LOG_ENTRY_TYPE_POS]=
                                    (char)DDL_LOG_ENTRY_CODE;
  global_ddl_log.file_entry_buf[DDL_LOG_ACTION_TYPE_POS]=
                                    (char)ddl_log_entry->action_type;
  global_ddl_log.file_entry_buf[DDL_LOG_PHASE_POS]= 0;
  int4store(&global_ddl_log.file_entry_buf[DDL_LOG_NEXT_ENTRY_POS],
            ddl_log_entry->next_entry);
  DBUG_ASSERT(strlen(ddl_log_entry->name) < FN_REFLEN);
  strmake(&global_ddl_log.file_entry_buf[DDL_LOG_NAME_POS],
          ddl_log_entry->name, FN_REFLEN - 1);
  if (ddl_log_entry->action_type == DDL_LOG_RENAME_ACTION ||
      ddl_log_entry->action_type == DDL_LOG_REPLACE_ACTION ||
      ddl_log_entry->action_type == DDL_LOG_EXCHANGE_ACTION)
  {
    DBUG_ASSERT(strlen(ddl_log_entry->from_name) < FN_REFLEN);
    strmake(&global_ddl_log.file_entry_buf[DDL_LOG_NAME_POS + FN_REFLEN],
          ddl_log_entry->from_name, FN_REFLEN - 1);
  }
  else
    global_ddl_log.file_entry_buf[DDL_LOG_NAME_POS + FN_REFLEN]= 0;
  DBUG_ASSERT(strlen(ddl_log_entry->handler_name) < FN_REFLEN);
  strmake(&global_ddl_log.file_entry_buf[DDL_LOG_NAME_POS + (2*FN_REFLEN)],
          ddl_log_entry->handler_name, FN_REFLEN - 1);
  if (ddl_log_entry->action_type == DDL_LOG_EXCHANGE_ACTION)
  {
    DBUG_ASSERT(strlen(ddl_log_entry->tmp_name) < FN_REFLEN);
    strmake(&global_ddl_log.file_entry_buf[DDL_LOG_NAME_POS + (3*FN_REFLEN)],
          ddl_log_entry->tmp_name, FN_REFLEN - 1);
  }
  else
    global_ddl_log.file_entry_buf[DDL_LOG_NAME_POS + (3*FN_REFLEN)]= 0;
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
  char *file_entry_buf= (char*) global_ddl_log.file_entry_buf;
  uint inx;
  uchar single_char;

  mysql_mutex_assert_owner(&LOCK_gdl);
  ddl_log_entry->entry_pos= read_entry;
  single_char= file_entry_buf[DDL_LOG_ENTRY_TYPE_POS];
  ddl_log_entry->entry_type= (enum ddl_log_entry_code)single_char;
  single_char= file_entry_buf[DDL_LOG_ACTION_TYPE_POS];
  ddl_log_entry->action_type= (enum ddl_log_action_code)single_char;
  ddl_log_entry->phase= file_entry_buf[DDL_LOG_PHASE_POS];
  ddl_log_entry->next_entry= uint4korr(&file_entry_buf[DDL_LOG_NEXT_ENTRY_POS]);
  ddl_log_entry->name= &file_entry_buf[DDL_LOG_NAME_POS];
  inx= DDL_LOG_NAME_POS + global_ddl_log.name_len;
  ddl_log_entry->from_name= &file_entry_buf[inx];
  inx+= global_ddl_log.name_len;
  ddl_log_entry->handler_name= &file_entry_buf[inx];
  if (ddl_log_entry->action_type == DDL_LOG_EXCHANGE_ACTION)
  {
    inx+= global_ddl_log.name_len;
    ddl_log_entry->tmp_name= &file_entry_buf[inx];
  }
  else
    ddl_log_entry->tmp_name= NULL;
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
    DBUG_RETURN(TRUE);
  }
  set_ddl_log_entry_from_global(ddl_log_entry, read_entry);
  DBUG_RETURN(FALSE);
}


/**
  Initialise ddl log.

  Write the header of the ddl log file and length of names. Also set
  number of entries to zero.

  @return Operation status
    @retval TRUE                     Error
    @retval FALSE                    Success
*/

static bool init_ddl_log()
{
  char file_name[FN_REFLEN];
  DBUG_ENTER("init_ddl_log");

  if (global_ddl_log.inited)
    goto end;

  global_ddl_log.io_size= IO_SIZE;
  global_ddl_log.name_len= FN_REFLEN;
  create_ddl_log_file_name(file_name);
  if ((global_ddl_log.file_id= mysql_file_create(key_file_global_ddl_log,
                                                 file_name, CREATE_MODE,
                                                 O_RDWR | O_TRUNC | O_BINARY,
                                                 MYF(MY_WME))) < 0)
  {
    /* Couldn't create ddl log file, this is serious error */
    sql_print_error("Failed to open ddl log file");
    DBUG_RETURN(TRUE);
  }
  global_ddl_log.inited= TRUE;
  if (write_ddl_log_header())
  {
    (void) mysql_file_close(global_ddl_log.file_id, MYF(MY_WME));
    global_ddl_log.inited= FALSE;
    DBUG_RETURN(TRUE);
  }

end:
  DBUG_RETURN(FALSE);
}


/**
  Sync ddl log file.

  @return Operation status
    @retval TRUE        Error
    @retval FALSE       Success
*/

static bool sync_ddl_log_no_lock()
{
  DBUG_ENTER("sync_ddl_log_no_lock");

  mysql_mutex_assert_owner(&LOCK_gdl);
  if ((!global_ddl_log.recovery_phase) &&
      init_ddl_log())
  {
    DBUG_RETURN(TRUE);
  }
  DBUG_RETURN(sync_ddl_log_file());
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

  @param entry_no     Entry position of record to change

  @return Operation status
    @retval TRUE      Error
    @retval FALSE     Success
*/

static bool deactivate_ddl_log_entry_no_lock(uint entry_no)
{
  uchar *file_entry_buf= (uchar*)global_ddl_log.file_entry_buf;
  DBUG_ENTER("deactivate_ddl_log_entry_no_lock");

  mysql_mutex_assert_owner(&LOCK_gdl);
  if (!read_ddl_log_file_entry(entry_no))
  {
    if (file_entry_buf[DDL_LOG_ENTRY_TYPE_POS] == DDL_LOG_ENTRY_CODE)
    {
      /*
        Log entry, if complete mark it done (IGNORE).
        Otherwise increase the phase by one.
      */
      if (file_entry_buf[DDL_LOG_ACTION_TYPE_POS] == DDL_LOG_DELETE_ACTION ||
          file_entry_buf[DDL_LOG_ACTION_TYPE_POS] == DDL_LOG_RENAME_ACTION ||
          (file_entry_buf[DDL_LOG_ACTION_TYPE_POS] == DDL_LOG_REPLACE_ACTION &&
           file_entry_buf[DDL_LOG_PHASE_POS] == 1) ||
          (file_entry_buf[DDL_LOG_ACTION_TYPE_POS] == DDL_LOG_EXCHANGE_ACTION &&
           file_entry_buf[DDL_LOG_PHASE_POS] >= EXCH_PHASE_TEMP_TO_FROM))
        file_entry_buf[DDL_LOG_ENTRY_TYPE_POS]= DDL_IGNORE_LOG_ENTRY_CODE;
      else if (file_entry_buf[DDL_LOG_ACTION_TYPE_POS] == DDL_LOG_REPLACE_ACTION)
      {
        DBUG_ASSERT(file_entry_buf[DDL_LOG_PHASE_POS] == 0);
        file_entry_buf[DDL_LOG_PHASE_POS]= 1;
      }
      else if (file_entry_buf[DDL_LOG_ACTION_TYPE_POS] == DDL_LOG_EXCHANGE_ACTION)
      {
        DBUG_ASSERT(file_entry_buf[DDL_LOG_PHASE_POS] <=
                                                 EXCH_PHASE_FROM_TO_NAME);
        file_entry_buf[DDL_LOG_PHASE_POS]++;
      }
      else
      {
        DBUG_ASSERT(0);
      }
      if (write_ddl_log_file_entry(entry_no))
      {
        sql_print_error("Error in deactivating log entry. Position = %u",
                        entry_no);
        DBUG_RETURN(TRUE);
      }
    }
  }
  else
  {
    sql_print_error("Failed in reading entry before deactivating it");
    DBUG_RETURN(TRUE);
  }
  DBUG_RETURN(FALSE);
}


/**
  Execute one action in a ddl log entry

  @param ddl_log_entry              Information in action entry to execute

  @return Operation status
    @retval TRUE                       Error
    @retval FALSE                      Success
*/

static int execute_ddl_log_action(THD *thd, DDL_LOG_ENTRY *ddl_log_entry)
{
  bool frm_action= FALSE;
  LEX_CSTRING handler_name;
  handler *file= NULL;
  MEM_ROOT mem_root;
  int error= TRUE;
  char to_path[FN_REFLEN];
  char from_path[FN_REFLEN];
#ifdef WITH_PARTITION_STORAGE_ENGINE
  char *par_ext= (char*)".par";
#endif
  handlerton *hton;
  DBUG_ENTER("execute_ddl_log_action");

  mysql_mutex_assert_owner(&LOCK_gdl);
  if (ddl_log_entry->entry_type == DDL_IGNORE_LOG_ENTRY_CODE)
  {
    DBUG_RETURN(FALSE);
  }
  DBUG_PRINT("ddl_log",
             ("execute type %c next %u name '%s' from_name '%s' handler '%s'"
              " tmp_name '%s'",
             ddl_log_entry->action_type,
             ddl_log_entry->next_entry,
             ddl_log_entry->name,
             ddl_log_entry->from_name,
             ddl_log_entry->handler_name,
             ddl_log_entry->tmp_name));
  handler_name.str= (char*)ddl_log_entry->handler_name;
  handler_name.length= strlen(ddl_log_entry->handler_name);
  init_sql_alloc(&mem_root, "execute_ddl_log_action", TABLE_ALLOC_BLOCK_SIZE,
                 0, MYF(MY_THREAD_SPECIFIC));
  if (!strcmp(ddl_log_entry->handler_name, reg_ext))
    frm_action= TRUE;
  else
  {
    plugin_ref plugin= ha_resolve_by_name(thd, &handler_name, false);
    if (!plugin)
    {
      my_error(ER_UNKNOWN_STORAGE_ENGINE, MYF(0), ddl_log_entry->handler_name);
      goto error;
    }
    hton= plugin_data(plugin, handlerton*);
    file= get_new_handler((TABLE_SHARE*)0, &mem_root, hton);
    if (unlikely(!file))
      goto error;
  }
  switch (ddl_log_entry->action_type)
  {
    case DDL_LOG_REPLACE_ACTION:
    case DDL_LOG_DELETE_ACTION:
    {
      if (ddl_log_entry->phase == 0)
      {
        if (frm_action)
        {
          strxmov(to_path, ddl_log_entry->name, reg_ext, NullS);
          if (unlikely((error= mysql_file_delete(key_file_frm, to_path,
                                                 MYF(MY_WME)))))
          {
            if (my_errno != ENOENT)
              break;
          }
#ifdef WITH_PARTITION_STORAGE_ENGINE
          strxmov(to_path, ddl_log_entry->name, par_ext, NullS);
          (void) mysql_file_delete(key_file_partition, to_path, MYF(MY_WME));
#endif
        }
        else
        {
          if (unlikely((error= file->ha_delete_table(ddl_log_entry->name))))
          {
            if (error != ENOENT && error != HA_ERR_NO_SUCH_TABLE)
              break;
          }
        }
        if ((deactivate_ddl_log_entry_no_lock(ddl_log_entry->entry_pos)))
          break;
        (void) sync_ddl_log_no_lock();
        error= FALSE;
        if (ddl_log_entry->action_type == DDL_LOG_DELETE_ACTION)
          break;
      }
      DBUG_ASSERT(ddl_log_entry->action_type == DDL_LOG_REPLACE_ACTION);
      /*
        Fall through and perform the rename action of the replace
        action. We have already indicated the success of the delete
        action in the log entry by stepping up the phase.
      */
    }
    /* fall through */
    case DDL_LOG_RENAME_ACTION:
    {
      error= TRUE;
      if (frm_action)
      {
        strxmov(to_path, ddl_log_entry->name, reg_ext, NullS);
        strxmov(from_path, ddl_log_entry->from_name, reg_ext, NullS);
        if (mysql_file_rename(key_file_frm, from_path, to_path, MYF(MY_WME)))
          break;
#ifdef WITH_PARTITION_STORAGE_ENGINE
        strxmov(to_path, ddl_log_entry->name, par_ext, NullS);
        strxmov(from_path, ddl_log_entry->from_name, par_ext, NullS);
        (void) mysql_file_rename(key_file_partition, from_path, to_path, MYF(MY_WME));
#endif
      }
      else
      {
        if (file->ha_rename_table(ddl_log_entry->from_name,
                                  ddl_log_entry->name))
          break;
      }
      if ((deactivate_ddl_log_entry_no_lock(ddl_log_entry->entry_pos)))
        break;
      (void) sync_ddl_log_no_lock();
      error= FALSE;
      break;
    }
    case DDL_LOG_EXCHANGE_ACTION:
    {
      /* We hold LOCK_gdl, so we can alter global_ddl_log.file_entry_buf */
      char *file_entry_buf= (char*)&global_ddl_log.file_entry_buf;
      /* not yet implemented for frm */
      DBUG_ASSERT(!frm_action);
      /*
        Using a case-switch here to revert all currently done phases,
        since it will fall through until the first phase is undone.
      */
      switch (ddl_log_entry->phase) {
        case EXCH_PHASE_TEMP_TO_FROM:
          /* tmp_name -> from_name possibly done */
          (void) file->ha_rename_table(ddl_log_entry->from_name,
                                       ddl_log_entry->tmp_name);
          /* decrease the phase and sync */
          file_entry_buf[DDL_LOG_PHASE_POS]--;
          if (write_ddl_log_file_entry(ddl_log_entry->entry_pos))
            break;
          if (sync_ddl_log_no_lock())
            break;
          /* fall through */
        case EXCH_PHASE_FROM_TO_NAME:
          /* from_name -> name possibly done */
          (void) file->ha_rename_table(ddl_log_entry->name,
                                       ddl_log_entry->from_name);
          /* decrease the phase and sync */
          file_entry_buf[DDL_LOG_PHASE_POS]--;
          if (write_ddl_log_file_entry(ddl_log_entry->entry_pos))
            break;
          if (sync_ddl_log_no_lock())
            break;
          /* fall through */
        case EXCH_PHASE_NAME_TO_TEMP:
          /* name -> tmp_name possibly done */
          (void) file->ha_rename_table(ddl_log_entry->tmp_name,
                                       ddl_log_entry->name);
          /* disable the entry and sync */
          file_entry_buf[DDL_LOG_ENTRY_TYPE_POS]= DDL_IGNORE_LOG_ENTRY_CODE;
          if (write_ddl_log_file_entry(ddl_log_entry->entry_pos))
            break;
          if (sync_ddl_log_no_lock())
            break;
          error= FALSE;
          break;
        default:
          DBUG_ASSERT(0);
          break;
      }

      break;
    }
    default:
      DBUG_ASSERT(0);
      break;
  }
  delete file;
error:
  free_root(&mem_root, MYF(0)); 
  DBUG_RETURN(error);
}


/**
  Get a free entry in the ddl log

  @param[out] active_entry     A ddl log memory entry returned

  @return Operation status
    @retval TRUE               Error
    @retval FALSE              Success
*/

static bool get_free_ddl_log_entry(DDL_LOG_MEMORY_ENTRY **active_entry,
                                   bool *write_header)
{
  DDL_LOG_MEMORY_ENTRY *used_entry;
  DDL_LOG_MEMORY_ENTRY *first_used= global_ddl_log.first_used;
  DBUG_ENTER("get_free_ddl_log_entry");

  if (global_ddl_log.first_free == NULL)
  {
    if (!(used_entry= (DDL_LOG_MEMORY_ENTRY*)my_malloc(
                              sizeof(DDL_LOG_MEMORY_ENTRY), MYF(MY_WME))))
    {
      sql_print_error("Failed to allocate memory for ddl log free list");
      DBUG_RETURN(TRUE);
    }
    global_ddl_log.num_entries++;
    used_entry->entry_pos= global_ddl_log.num_entries;
    *write_header= TRUE;
  }
  else
  {
    used_entry= global_ddl_log.first_free;
    global_ddl_log.first_free= used_entry->next_log_entry;
    *write_header= FALSE;
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
  Execute one entry in the ddl log.
  
  Executing an entry means executing a linked list of actions.

  @param first_entry           Reference to first action in entry

  @return Operation status
    @retval TRUE               Error
    @retval FALSE              Success
*/

static bool execute_ddl_log_entry_no_lock(THD *thd, uint first_entry)
{
  DDL_LOG_ENTRY ddl_log_entry;
  uint read_entry= first_entry;
  DBUG_ENTER("execute_ddl_log_entry_no_lock");

  mysql_mutex_assert_owner(&LOCK_gdl);
  do
  {
    if (read_ddl_log_entry(read_entry, &ddl_log_entry))
    {
      /* Write to error log and continue with next log entry */
      sql_print_error("Failed to read entry = %u from ddl log",
                      read_entry);
      break;
    }
    DBUG_ASSERT(ddl_log_entry.entry_type == DDL_LOG_ENTRY_CODE ||
                ddl_log_entry.entry_type == DDL_IGNORE_LOG_ENTRY_CODE);

    if (execute_ddl_log_action(thd, &ddl_log_entry))
    {
      /* Write to error log and continue with next log entry */
      sql_print_error("Failed to execute action for entry = %u from ddl log",
                      read_entry);
      break;
    }
    read_entry= ddl_log_entry.next_entry;
  } while (read_entry);
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

bool write_ddl_log_entry(DDL_LOG_ENTRY *ddl_log_entry,
                         DDL_LOG_MEMORY_ENTRY **active_entry)
{
  bool error, write_header;
  DBUG_ENTER("write_ddl_log_entry");

  mysql_mutex_assert_owner(&LOCK_gdl);
  if (init_ddl_log())
  {
    DBUG_RETURN(TRUE);
  }
  set_global_from_ddl_log_entry(ddl_log_entry);
  if (get_free_ddl_log_entry(active_entry, &write_header))
  {
    DBUG_RETURN(TRUE);
  }
  error= FALSE;
  DBUG_PRINT("ddl_log",
             ("write type %c next %u name '%s' from_name '%s' handler '%s'"
              " tmp_name '%s'",
             (char) global_ddl_log.file_entry_buf[DDL_LOG_ACTION_TYPE_POS],
             ddl_log_entry->next_entry,
             (char*) &global_ddl_log.file_entry_buf[DDL_LOG_NAME_POS],
             (char*) &global_ddl_log.file_entry_buf[DDL_LOG_NAME_POS
                                                    + FN_REFLEN],
             (char*) &global_ddl_log.file_entry_buf[DDL_LOG_NAME_POS
                                                    + (2*FN_REFLEN)],
             (char*) &global_ddl_log.file_entry_buf[DDL_LOG_NAME_POS
                                                    + (3*FN_REFLEN)]));
  if (unlikely(write_ddl_log_file_entry((*active_entry)->entry_pos)))
  {
    error= TRUE;
    sql_print_error("Failed to write entry_no = %u",
                    (*active_entry)->entry_pos);
  }
  if (write_header && likely(!error))
  {
    (void) sync_ddl_log_no_lock();
    if (write_ddl_log_header())
      error= TRUE;
  }
  if (unlikely(error))
    release_ddl_log_memory_entry(*active_entry);
  DBUG_RETURN(error);
}


/**
  @brief Write final entry in the ddl log.

  @details This is the last write in the ddl log. The previous log entries
  have already been written but not yet synched to disk.
  We write a couple of log entries that describes action to perform.
  This entries are set-up in a linked list, however only when a first
  execute entry is put as the first entry these will be executed.
  This routine writes this first.

  @param first_entry               First entry in linked list of entries
                                   to execute, if 0 = NULL it means that
                                   the entry is removed and the entries
                                   are put into the free list.
  @param complete                  Flag indicating we are simply writing
                                   info about that entry has been completed
  @param[in,out] active_entry      Entry to execute, 0 = NULL if the entry
                                   is written first time and needs to be
                                   returned. In this case the entry written
                                   is returned in this parameter

  @return Operation status
    @retval TRUE                   Error
    @retval FALSE                  Success
*/ 

bool write_execute_ddl_log_entry(uint first_entry,
                                 bool complete,
                                 DDL_LOG_MEMORY_ENTRY **active_entry)
{
  bool write_header= FALSE;
  char *file_entry_buf= (char*)global_ddl_log.file_entry_buf;
  DBUG_ENTER("write_execute_ddl_log_entry");

  mysql_mutex_assert_owner(&LOCK_gdl);
  if (init_ddl_log())
  {
    DBUG_RETURN(TRUE);
  }
  if (!complete)
  {
    /*
      We haven't synched the log entries yet, we synch them now before
      writing the execute entry. If complete is true we haven't written
      any log entries before, we are only here to write the execute
      entry to indicate it is done.
    */
    (void) sync_ddl_log_no_lock();
    file_entry_buf[DDL_LOG_ENTRY_TYPE_POS]= (char)DDL_LOG_EXECUTE_CODE;
  }
  else
    file_entry_buf[DDL_LOG_ENTRY_TYPE_POS]= (char)DDL_IGNORE_LOG_ENTRY_CODE;
  file_entry_buf[DDL_LOG_ACTION_TYPE_POS]= 0; /* Ignored for execute entries */
  file_entry_buf[DDL_LOG_PHASE_POS]= 0;
  int4store(&file_entry_buf[DDL_LOG_NEXT_ENTRY_POS], first_entry);
  file_entry_buf[DDL_LOG_NAME_POS]= 0;
  file_entry_buf[DDL_LOG_NAME_POS + FN_REFLEN]= 0;
  file_entry_buf[DDL_LOG_NAME_POS + 2*FN_REFLEN]= 0;
  if (!(*active_entry))
  {
    if (get_free_ddl_log_entry(active_entry, &write_header))
    {
      DBUG_RETURN(TRUE);
    }
    write_header= TRUE;
  }
  if (write_ddl_log_file_entry((*active_entry)->entry_pos))
  {
    sql_print_error("Error writing execute entry in ddl log");
    release_ddl_log_memory_entry(*active_entry);
    DBUG_RETURN(TRUE);
  }
  (void) sync_ddl_log_no_lock();
  if (write_header)
  {
    if (write_ddl_log_header())
    {
      release_ddl_log_memory_entry(*active_entry);
      DBUG_RETURN(TRUE);
    }
  }
  DBUG_RETURN(FALSE);
}


/**
  Deactivate an individual entry.

  @details see deactivate_ddl_log_entry_no_lock.

  @param entry_no     Entry position of record to change

  @return Operation status
    @retval TRUE      Error
    @retval FALSE     Success
*/

bool deactivate_ddl_log_entry(uint entry_no)
{
  bool error;
  DBUG_ENTER("deactivate_ddl_log_entry");

  mysql_mutex_lock(&LOCK_gdl);
  error= deactivate_ddl_log_entry_no_lock(entry_no);
  mysql_mutex_unlock(&LOCK_gdl);
  DBUG_RETURN(error);
}


/**
  Sync ddl log file.

  @return Operation status
    @retval TRUE        Error
    @retval FALSE       Success
*/

bool sync_ddl_log()
{
  bool error;
  DBUG_ENTER("sync_ddl_log");

  mysql_mutex_lock(&LOCK_gdl);
  error= sync_ddl_log_no_lock();
  mysql_mutex_unlock(&LOCK_gdl);

  DBUG_RETURN(error);
}


/**
  Release a log memory entry.
  @param log_memory_entry                Log memory entry to release
*/

void release_ddl_log_memory_entry(DDL_LOG_MEMORY_ENTRY *log_entry)
{
  DDL_LOG_MEMORY_ENTRY *first_free= global_ddl_log.first_free;
  DDL_LOG_MEMORY_ENTRY *next_log_entry= log_entry->next_log_entry;
  DDL_LOG_MEMORY_ENTRY *prev_log_entry= log_entry->prev_log_entry;
  DBUG_ENTER("release_ddl_log_memory_entry");

  mysql_mutex_assert_owner(&LOCK_gdl);
  global_ddl_log.first_free= log_entry;
  log_entry->next_log_entry= first_free;

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

bool execute_ddl_log_entry(THD *thd, uint first_entry)
{
  bool error;
  DBUG_ENTER("execute_ddl_log_entry");

  mysql_mutex_lock(&LOCK_gdl);
  error= execute_ddl_log_entry_no_lock(thd, first_entry);
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
  DBUG_VOID_RETURN;
}


/**
  Execute the ddl log at recovery of MySQL Server.
*/

void execute_ddl_log_recovery()
{
  uint num_entries, i;
  THD *thd;
  DDL_LOG_ENTRY ddl_log_entry;
  char file_name[FN_REFLEN];
  static char recover_query_string[]= "INTERNAL DDL LOG RECOVER IN PROGRESS";
  DBUG_ENTER("execute_ddl_log_recovery");

  /*
    Initialise global_ddl_log struct
  */
  bzero(global_ddl_log.file_entry_buf, sizeof(global_ddl_log.file_entry_buf));
  global_ddl_log.inited= FALSE;
  global_ddl_log.recovery_phase= TRUE;
  global_ddl_log.io_size= IO_SIZE;
  global_ddl_log.file_id= (File) -1;

  /*
    To be able to run this from boot, we allocate a temporary THD
  */
  if (!(thd=new THD(0)))
    DBUG_VOID_RETURN;
  thd->thread_stack= (char*) &thd;
  thd->store_globals();

  thd->set_query(recover_query_string, strlen(recover_query_string));

  /* this also initialize LOCK_gdl */
  num_entries= read_ddl_log_header();
  mysql_mutex_lock(&LOCK_gdl);
  for (i= 1; i < num_entries + 1; i++)
  {
    if (read_ddl_log_entry(i, &ddl_log_entry))
    {
      sql_print_error("Failed to read entry no = %u from ddl log",
                       i);
      continue;
    }
    if (ddl_log_entry.entry_type == DDL_LOG_EXECUTE_CODE)
    {
      if (execute_ddl_log_entry_no_lock(thd, ddl_log_entry.next_entry))
      {
        /* Real unpleasant scenario but we continue anyways.  */
        continue;
      }
    }
  }
  close_ddl_log();
  create_ddl_log_file_name(file_name);
  (void) mysql_file_delete(key_file_global_ddl_log, file_name, MYF(0));
  global_ddl_log.recovery_phase= FALSE;
  mysql_mutex_unlock(&LOCK_gdl);
  thd->reset_query();
  delete thd;
  DBUG_VOID_RETURN;
}


/**
  Release all memory allocated to the ddl log.
*/

void release_ddl_log()
{
  DDL_LOG_MEMORY_ENTRY *free_list;
  DDL_LOG_MEMORY_ENTRY *used_list;
  DBUG_ENTER("release_ddl_log");

  if (!global_ddl_log.do_release)
    DBUG_VOID_RETURN;

  mysql_mutex_lock(&LOCK_gdl);
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
  close_ddl_log();
  global_ddl_log.inited= 0;
  mysql_mutex_unlock(&LOCK_gdl);
  mysql_mutex_destroy(&LOCK_gdl);
  global_ddl_log.do_release= false;
  DBUG_VOID_RETURN;
}


/*
---------------------------------------------------------------------------

  END MODULE DDL log
  --------------------

---------------------------------------------------------------------------
*/


/**
   @brief construct a temporary shadow file name.

   @details Make a shadow file name used by ALTER TABLE to construct the
   modified table (with keeping the original). The modified table is then
   moved back as original table. The name must start with the temp file
   prefix so it gets filtered out by table files listing routines. 
    
   @param[out] buff      buffer to receive the constructed name
   @param      bufflen   size of buff
   @param      lpt       alter table data structure

   @retval     path length
*/

uint build_table_shadow_filename(char *buff, size_t bufflen, 
                                 ALTER_PARTITION_PARAM_TYPE *lpt)
{
  char tmp_name[FN_REFLEN];
  my_snprintf(tmp_name, sizeof (tmp_name), "%s-%s", tmp_file_prefix,
              lpt->table_name.str);
  return build_table_filename(buff, bufflen, lpt->db.str, tmp_name, "",
                              FN_IS_TMP);
}


/*
  SYNOPSIS
    mysql_write_frm()
    lpt                    Struct carrying many parameters needed for this
                           method
    flags                  Flags as defined below
      WFRM_INITIAL_WRITE        If set we need to prepare table before
                                creating the frm file
      WFRM_INSTALL_SHADOW       If set we should install the new frm
      WFRM_KEEP_SHARE           If set we know that the share is to be
                                retained and thus we should ensure share
                                object is correct, if not set we don't
                                set the new partition syntax string since
                                we know the share object is destroyed.
      WFRM_PACK_FRM             If set we should pack the frm file and delete
                                the frm file

  RETURN VALUES
    TRUE                   Error
    FALSE                  Success

  DESCRIPTION
    A support method that creates a new frm file and in this process it
    regenerates the partition data. It works fine also for non-partitioned
    tables since it only handles partitioned data if it exists.
*/

bool mysql_write_frm(ALTER_PARTITION_PARAM_TYPE *lpt, uint flags)
{
  /*
    Prepare table to prepare for writing a new frm file where the
    partitions in add/drop state have temporarily changed their state
    We set tmp_table to avoid get errors on naming of primary key index.
  */
  int error= 0;
  char path[FN_REFLEN+1];
  char shadow_path[FN_REFLEN+1];
  char shadow_frm_name[FN_REFLEN+1];
  char frm_name[FN_REFLEN+1];
#ifdef WITH_PARTITION_STORAGE_ENGINE
  char *part_syntax_buf;
  uint syntax_len;
#endif
  DBUG_ENTER("mysql_write_frm");

  /*
    Build shadow frm file name
  */
  build_table_shadow_filename(shadow_path, sizeof(shadow_path) - 1, lpt);
  strxmov(shadow_frm_name, shadow_path, reg_ext, NullS);
  if (flags & WFRM_WRITE_SHADOW)
  {
    if (mysql_prepare_create_table(lpt->thd, lpt->create_info, lpt->alter_info,
                                   &lpt->db_options, lpt->table->file,
                                   &lpt->key_info_buffer, &lpt->key_count,
                                   C_ALTER_TABLE))
    {
      DBUG_RETURN(TRUE);
    }
#ifdef WITH_PARTITION_STORAGE_ENGINE
    {
      partition_info *part_info= lpt->table->part_info;
      if (part_info)
      {
        part_syntax_buf= generate_partition_syntax_for_frm(lpt->thd, part_info,
                               &syntax_len, lpt->create_info, lpt->alter_info);
        if (!part_syntax_buf)
          DBUG_RETURN(TRUE);
        part_info->part_info_string= part_syntax_buf;
        part_info->part_info_len= syntax_len;
      }
    }
#endif
    /* Write shadow frm file */
    lpt->create_info->table_options= lpt->db_options;
    LEX_CUSTRING frm= build_frm_image(lpt->thd, lpt->table_name,
                                      lpt->create_info,
                                      lpt->alter_info->create_list,
                                      lpt->key_count, lpt->key_info_buffer,
                                      lpt->table->file);
    if (!frm.str)
    {
      error= 1;
      goto end;
    }

    int error= writefrm(shadow_path, lpt->db.str, lpt->table_name.str,
                        lpt->create_info->tmp_table(), frm.str, frm.length);
    my_free(const_cast<uchar*>(frm.str));

    if (unlikely(error) ||
        unlikely(lpt->table->file->
                 ha_create_partitioning_metadata(shadow_path,
                                                 NULL, CHF_CREATE_FLAG)))
    {
      mysql_file_delete(key_file_frm, shadow_frm_name, MYF(0));
      error= 1;
      goto end;
    }
  }
  if (flags & WFRM_INSTALL_SHADOW)
  {
#ifdef WITH_PARTITION_STORAGE_ENGINE
    partition_info *part_info= lpt->part_info;
#endif
    /*
      Build frm file name
    */
    build_table_filename(path, sizeof(path) - 1, lpt->db.str,
                         lpt->table_name.str, "", 0);
    strxnmov(frm_name, sizeof(frm_name), path, reg_ext, NullS);
    /*
      When we are changing to use new frm file we need to ensure that we
      don't collide with another thread in process to open the frm file.
      We start by deleting the .frm file and possible .par file. Then we
      write to the DDL log that we have completed the delete phase by
      increasing the phase of the log entry. Next step is to rename the
      new .frm file and the new .par file to the real name. After
      completing this we write a new phase to the log entry that will
      deactivate it.
    */
    if (mysql_file_delete(key_file_frm, frm_name, MYF(MY_WME)) ||
#ifdef WITH_PARTITION_STORAGE_ENGINE
        lpt->table->file->ha_create_partitioning_metadata(path, shadow_path,
                                                  CHF_DELETE_FLAG) ||
        deactivate_ddl_log_entry(part_info->frm_log_entry->entry_pos) ||
        (sync_ddl_log(), FALSE) ||
        mysql_file_rename(key_file_frm,
                          shadow_frm_name, frm_name, MYF(MY_WME)) ||
        lpt->table->file->ha_create_partitioning_metadata(path, shadow_path,
                                                  CHF_RENAME_FLAG))
#else
        mysql_file_rename(key_file_frm,
                          shadow_frm_name, frm_name, MYF(MY_WME)))
#endif
    {
      error= 1;
      goto err;
    }
#ifdef WITH_PARTITION_STORAGE_ENGINE
    if (part_info && (flags & WFRM_KEEP_SHARE))
    {
      TABLE_SHARE *share= lpt->table->s;
      char *tmp_part_syntax_str;
      part_syntax_buf= generate_partition_syntax_for_frm(lpt->thd,
                   part_info, &syntax_len, lpt->create_info, lpt->alter_info);
      if (!part_syntax_buf)
      {
        error= 1;
        goto err;
      }
      if (share->partition_info_buffer_size < syntax_len + 1)
      {
        share->partition_info_buffer_size= syntax_len+1;
        if (!(tmp_part_syntax_str= (char*) strmake_root(&share->mem_root,
                                                        part_syntax_buf,
                                                        syntax_len)))
        {
          error= 1;
          goto err;
        }
        share->partition_info_str= tmp_part_syntax_str;
      }
      else
        memcpy((char*) share->partition_info_str, part_syntax_buf,
               syntax_len + 1);
      share->partition_info_str_len= part_info->part_info_len= syntax_len;
      part_info->part_info_string= part_syntax_buf;
    }
#endif

err:
#ifdef WITH_PARTITION_STORAGE_ENGINE
    deactivate_ddl_log_entry(part_info->frm_log_entry->entry_pos);
    part_info->frm_log_entry= NULL;
    (void) sync_ddl_log();
#endif
    ;
  }

end:
  DBUG_RETURN(error);
}


/*
  SYNOPSIS
    write_bin_log()
    thd                           Thread object
    clear_error                   is clear_error to be called
    query                         Query to log
    query_length                  Length of query
    is_trans                      if the event changes either
                                  a trans or non-trans engine.

  RETURN VALUES
    NONE

  DESCRIPTION
    Write the binlog if open, routine used in multiple places in this
    file
*/

int write_bin_log(THD *thd, bool clear_error,
                  char const *query, ulong query_length, bool is_trans)
{
  int error= 0;
  if (mysql_bin_log.is_open())
  {
    int errcode= 0;
    thd_proc_info(thd, "Writing to binlog");
    if (clear_error)
      thd->clear_error();
    else
      errcode= query_error_code(thd, TRUE);
    error= thd->binlog_query(THD::STMT_QUERY_TYPE,
                             query, query_length, is_trans, FALSE, FALSE,
                             errcode) > 0;
    thd_proc_info(thd, 0);
  }
  return error;
}


/*
 delete (drop) tables.

  SYNOPSIS
   mysql_rm_table()
   thd			Thread handle
   tables		List of tables to delete
   if_exists		If 1, don't give error if one table doesn't exists
   drop_temporary       1 if DROP TEMPORARY
   drop_sequence        1 if DROP SEQUENCE

  NOTES
    Will delete all tables that can be deleted and give a compact error
    messages for tables that could not be deleted.
    If a table is in use, we will wait for all users to free the table
    before dropping it

    Wait if global_read_lock (FLUSH TABLES WITH READ LOCK) is set, but
    not if under LOCK TABLES.

  RETURN
    FALSE OK.  In this case ok packet is sent to user
    TRUE  Error

*/

bool mysql_rm_table(THD *thd,TABLE_LIST *tables, bool if_exists,
                    bool drop_temporary, bool drop_sequence)
{
  bool error;
  Drop_table_error_handler err_handler;
  TABLE_LIST *table;
  DBUG_ENTER("mysql_rm_table");

  /* Disable drop of enabled log tables, must be done before name locking */
  for (table= tables; table; table= table->next_local)
  {
    if (check_if_log_table(table, TRUE, "DROP"))
      DBUG_RETURN(true);
  }

  if (!drop_temporary)
  {
    if (!thd->locked_tables_mode)
    {
      if (drop_sequence)
      {
        /* We are trying to drop a sequence.
           Change all temporary tables that are not sequences to
           normal tables so that we can try to drop them instead.
           If we don't do this, we will get an error 'not a sequence'
           when trying to drop a sequence that is hidden by a temporary
           table.
        */
        for (table= tables; table; table= table->next_global)
        {
          if (table->open_type == OT_TEMPORARY_OR_BASE &&
            is_temporary_table(table) && !table->table->s->sequence)
          {
            thd->mark_tmp_table_as_free_for_reuse(table->table);
            table->table= NULL;
          }
        }
      }
      if (lock_table_names(thd, tables, NULL,
                           thd->variables.lock_wait_timeout, 0))
        DBUG_RETURN(true);
    }
    else
    {
      for (table= tables; table; table= table->next_local)
      {
        if (is_temporary_table(table))
        {
          /*
            A temporary table.

            Don't try to find a corresponding MDL lock or assign it
            to table->mdl_request.ticket. There can't be metadata
            locks for temporary tables: they are local to the session.

            Later in this function we release the MDL lock only if
            table->mdl_requeset.ticket is not NULL. Thus here we
            ensure that we won't release the metadata lock on the base
            table locked with LOCK TABLES as a side effect of temporary
            table drop.
          */
          DBUG_ASSERT(table->mdl_request.ticket == NULL);
        }
        else
        {
          /*
            Not a temporary table.

            Since 'tables' list can't contain duplicates (this is ensured
            by parser) it is safe to cache pointer to the TABLE instances
            in its elements.
          */
          table->table= find_table_for_mdl_upgrade(thd, table->db.str,
                                                   table->table_name.str, NULL);
          if (!table->table)
            DBUG_RETURN(true);
          table->mdl_request.ticket= table->table->mdl_ticket;
        }
      }
    }
    /* We remove statistics for table last, after we have the DDL lock */
    for (table= tables; table; table= table->next_local)
    {
      LEX_CSTRING db_name= table->db;
      LEX_CSTRING table_name= table->table_name;
      if (table->open_type == OT_BASE_ONLY ||
          !thd->find_temporary_table(table))
        (void) delete_statistics_for_table(thd, &db_name, &table_name);
    }
  }

  DBUG_EXECUTE_IF("ib_purge_virtual_mdev_16222_1",
                  DBUG_ASSERT(!debug_sync_set_action(
                                thd,
                                STRING_WITH_LEN("now SIGNAL drop_started"))););

  /* mark for close and remove all cached entries */
  thd->push_internal_handler(&err_handler);
  error= mysql_rm_table_no_locks(thd, tables, if_exists, drop_temporary,
                                 false, drop_sequence, false, false);
  thd->pop_internal_handler();

  if (unlikely(error))
    DBUG_RETURN(TRUE);
  my_ok(thd);
  DBUG_RETURN(FALSE);
}


/**
  Find the comment in the query.
  That's auxiliary function to be used handling DROP TABLE [comment].

  @param  thd             Thread handler
  @param  comment_pos     How many characters to skip before the comment.
                          Can be either 9 for DROP TABLE or
                          17 for DROP TABLE IF EXISTS
  @param  comment_start   returns the beginning of the comment if found.

  @retval  0  no comment found
  @retval  >0 the lenght of the comment found

*/
static uint32 comment_length(THD *thd, uint32 comment_pos,
                             const char **comment_start)
{
  /* We use uchar * here to make array indexing portable */
  const uchar *query= (uchar*) thd->query();
  const uchar *query_end= (uchar*) query + thd->query_length();
  const uchar *const state_map= thd->charset()->state_map;

  for (; query < query_end; query++)
  {
    if (state_map[static_cast<uchar>(*query)] == MY_LEX_SKIP)
      continue;
    if (comment_pos-- == 0)
      break;
  }
  if (query > query_end - 3 /* comment can't be shorter than 4 */ ||
      state_map[static_cast<uchar>(*query)] != MY_LEX_LONG_COMMENT || query[1] != '*')
    return 0;
  
  *comment_start= (char*) query;
  
  for (query+= 3; query < query_end; query++)
  {
    if (query[-1] == '*' && query[0] == '/')
      return (uint32)((char*) query - *comment_start + 1);
  }
  return 0;
}

/**
  Execute the drop of a normal or temporary table.

  @param  thd             Thread handler
  @param  tables          Tables to drop
  @param  if_exists       If set, don't give an error if table doesn't exists.
                          In this case we give an warning of level 'NOTE'
  @param  drop_temporary  Only drop temporary tables
  @param  drop_view       Allow to delete VIEW .frm
  @param  dont_log_query  Don't write query to log files. This will also not
                          generate warnings if the handler files doesn't exists
  @param  dont_free_locks Don't do automatic UNLOCK TABLE if no more locked
                          tables

  @retval  0  ok
  @retval  1  Error
  @retval -1  Thread was killed

  @note This function assumes that metadata locks have already been taken.
        It is also assumed that the tables have been removed from TDC.

  @note This function assumes that temporary tables to be dropped have
        been pre-opened using corresponding table list elements.

  @todo When logging to the binary log, we should log
        tmp_tables and transactional tables as separate statements if we
        are in a transaction;  This is needed to get these tables into the
        cached binary log that is only written on COMMIT.
        The current code only writes DROP statements that only uses temporary
        tables to the cache binary log.  This should be ok on most cases, but
        not all.
*/

int mysql_rm_table_no_locks(THD *thd, TABLE_LIST *tables, bool if_exists,
                            bool drop_temporary, bool drop_view,
                            bool drop_sequence,
                            bool dont_log_query,
                            bool dont_free_locks)
{
  TABLE_LIST *table;
  char path[FN_REFLEN + 1], wrong_tables_buff[160];
  LEX_CSTRING alias= null_clex_str;
  String wrong_tables(wrong_tables_buff, sizeof(wrong_tables_buff)-1,
                      system_charset_info);
  uint path_length= 0, errors= 0;
  int error= 0;
  int non_temp_tables_count= 0;
  bool non_tmp_error= 0;
  bool trans_tmp_table_deleted= 0, non_trans_tmp_table_deleted= 0;
  bool non_tmp_table_deleted= 0;
  bool is_drop_tmp_if_exists_added= 0;
  bool was_view= 0, was_table= 0, is_sequence;
  String built_query;
  String built_trans_tmp_query, built_non_trans_tmp_query;
  DBUG_ENTER("mysql_rm_table_no_locks");

  wrong_tables.length(0);
  /*
    Prepares the drop statements that will be written into the binary
    log as follows:

    1 - If we are not processing a "DROP TEMPORARY" it prepares a
    "DROP".

    2 - A "DROP" may result in a "DROP TEMPORARY" but the opposite is
    not true.

    3 - If the current format is row, the IF EXISTS token needs to be
    appended because one does not know if CREATE TEMPORARY was previously
    written to the binary log.

    4 - Add the IF_EXISTS token if necessary, i.e. if_exists is TRUE.

    5 - For temporary tables, there is a need to differentiate tables
    in transactional and non-transactional storage engines. For that,
    reason, two types of drop statements are prepared.

    The need to different the type of tables when dropping a temporary
    table stems from the fact that such drop does not commit an ongoing
    transaction and changes to non-transactional tables must be written
    ahead of the transaction in some circumstances.

    6- Slave SQL thread ignores all replicate-* filter rules
    for temporary tables with 'IF EXISTS' clause. (See sql/sql_parse.cc:
    mysql_execute_command() for details). These commands will be binlogged
    as they are, even if the default database (from USE `db`) is not present
    on the Slave. This can cause point in time recovery failures later
    when user uses the slave's binlog to re-apply. Hence at the time of binary
    logging, these commands will be written with fully qualified table names
    and use `db` will be suppressed.
  */
  if (!dont_log_query)
  {
    const char *object_to_drop= (drop_sequence) ? "SEQUENCE" : "TABLE";

    if (!drop_temporary)
    {
      const char *comment_start;
      uint32 comment_len;

      built_query.set_charset(thd->charset());
      built_query.append("DROP ");
      built_query.append(object_to_drop);
      built_query.append(' ');
      if (if_exists)
        built_query.append("IF EXISTS ");

      /* Preserve comment in original query */
      if ((comment_len= comment_length(thd, if_exists ? 17:9, &comment_start)))
      {
        built_query.append(comment_start, comment_len);
        built_query.append(" ");
      }
    }

    built_trans_tmp_query.set_charset(system_charset_info);
    built_trans_tmp_query.append("DROP TEMPORARY ");
    built_trans_tmp_query.append(object_to_drop);
    built_trans_tmp_query.append(' ');
    if (thd->is_current_stmt_binlog_format_row() || if_exists)
    {
      is_drop_tmp_if_exists_added= true;
      built_trans_tmp_query.append("IF EXISTS ");
    }
    built_non_trans_tmp_query.set_charset(system_charset_info);
    built_non_trans_tmp_query.copy(built_trans_tmp_query);
  }

  for (table= tables; table; table= table->next_local)
  {
    bool is_trans= 0;
    bool table_creation_was_logged= 0;
    bool real_table= FALSE;
    LEX_CSTRING db= table->db;
    handlerton *table_type= 0;
    // reset error state for this table
    error= 0;

    DBUG_PRINT("table", ("table_l: '%s'.'%s'  table: %p  s: %p",
                         table->db.str, table->table_name.str,  table->table,
                         table->table ?  table->table->s : NULL));

    /*
      If we are in locked tables mode and are dropping a temporary table,
      the ticket should be NULL to ensure that we don't release a lock
      on a base table later.
    */
    DBUG_ASSERT(!(thd->locked_tables_mode &&
                  table->open_type != OT_BASE_ONLY &&
                  thd->find_temporary_table(table) &&
                  table->mdl_request.ticket != NULL));

    if (table->open_type == OT_BASE_ONLY || !is_temporary_table(table))
      real_table= TRUE;
    else if (drop_sequence &&
            table->table->s->table_type != TABLE_TYPE_SEQUENCE)
    {
      was_table= (table->table->s->table_type == TABLE_TYPE_NORMAL);
      was_view= (table->table->s->table_type == TABLE_TYPE_VIEW);
      if (if_exists)
      {
        char buff[FN_REFLEN];
        String tbl_name(buff, sizeof(buff), system_charset_info);
        tbl_name.length(0);
        tbl_name.append(&db);
        tbl_name.append('.');
        tbl_name.append(&table->table_name);
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE,
                            ER_NOT_SEQUENCE2, ER_THD(thd, ER_NOT_SEQUENCE2),
                            tbl_name.c_ptr_safe());

        /*
          Our job is done here. This statement was added to avoid executing
          unnecessary code farther below which in some strange corner cases
          caused the server to crash (see MDEV-17896).
        */
        goto log_query;
      }
      error= 1;
      goto non_critical_err;
    }
    else
    {
      table_creation_was_logged= table->table->s->table_creation_was_logged;
      if (thd->drop_temporary_table(table->table, &is_trans, true))
      {
        error= 1;
        goto err;
      }
      table->table= 0;
    }

    if ((drop_temporary && if_exists) || !real_table)
    {
      /*
        This handles the case of temporary tables. We have the following cases:

          . "DROP TEMPORARY" was executed and a temporary table was affected
          (i.e. drop_temporary && !real_table) or the
          if_exists was specified (i.e. drop_temporary && if_exists).

          . "DROP" was executed but a temporary table was affected (.i.e
          !real_table).
      */
      if (!dont_log_query && table_creation_was_logged)
      {
        /*
          If there is an real_table, we don't know the type of the engine
          at this point. So, we keep it in the trx-cache.
        */
        is_trans= real_table ? TRUE : is_trans;
        if (is_trans)
          trans_tmp_table_deleted= TRUE;
        else
          non_trans_tmp_table_deleted= TRUE;

        String *built_ptr_query=
          (is_trans ? &built_trans_tmp_query : &built_non_trans_tmp_query);
        /*
          Write the database name if it is not the current one or if
          thd->db is NULL or 'IF EXISTS' clause is present in 'DROP TEMPORARY'
          query.
        */
        if (thd->db.str == NULL || cmp(&db, &thd->db) ||
            is_drop_tmp_if_exists_added )
        {
          append_identifier(thd, built_ptr_query, &db);
          built_ptr_query->append(".");
        }
        append_identifier(thd, built_ptr_query, &table->table_name);
        built_ptr_query->append(",");
      }
      /*
        This means that a temporary table was droped and as such there
        is no need to proceed with the code that tries to drop a regular
        table.
      */
      if (!real_table) continue;
    }
    else if (!drop_temporary)
    {
      non_temp_tables_count++;

      DBUG_ASSERT(thd->mdl_context.is_lock_owner(MDL_key::TABLE, table->db.str,
                                                 table->table_name.str,
                                                 MDL_SHARED));

      alias= (lower_case_table_names == 2) ? table->alias : table->table_name;
      /* remove .frm file and engine files */
      path_length= build_table_filename(path, sizeof(path) - 1, db.str, alias.str,
                                        reg_ext, 0);
    }
    DEBUG_SYNC(thd, "rm_table_no_locks_before_delete_table");
    if (drop_temporary ||
        (ha_table_exists(thd, &db, &alias, &table_type, &is_sequence) == 0 &&
         table_type == 0) ||
        (!drop_view && (was_view= (table_type == view_pseudo_hton))) ||
        (drop_sequence && !is_sequence))
    {
      /*
        One of the following cases happened:
          . "DROP TEMPORARY" but a temporary table was not found.
          . "DROP" but table was not found
          . "DROP TABLE" statement, but it's a view. 
          . "DROP SEQUENCE", but it's not a sequence
      */
      was_table= drop_sequence && table_type;
      if (if_exists)
      {
        char buff[FN_REFLEN];
        int err= (drop_sequence ? ER_UNKNOWN_SEQUENCES :
                  ER_BAD_TABLE_ERROR);
        String tbl_name(buff, sizeof(buff), system_charset_info);
        tbl_name.length(0);
        tbl_name.append(&db);
        tbl_name.append('.');
        tbl_name.append(&table->table_name);
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE,
                            err, ER_THD(thd, err),
                            tbl_name.c_ptr_safe());

        /*
          Our job is done here. This statement was added to avoid executing
          unnecessary code farther below which in some strange corner cases
          caused the server to crash (see MDEV-17896).
        */
        goto log_query;
      }
      else
      {
        non_tmp_error = (drop_temporary ? non_tmp_error : TRUE);
        error= 1;
        /*
          non critical error (only for this table), so we continue.
          Next we write it to wrong_tables and continue this loop
          The same as "goto non_critical_err".
        */
      }
    }
    else
    {
      char *end;
      int frm_delete_error= 0;
      /*
        It could happen that table's share in the table definition cache
        is the only thing that keeps the engine plugin loaded
        (if it is uninstalled and waits for the ref counter to drop to 0).

        In this case, the tdc_remove_table() below will release and unload
        the plugin. And ha_delete_table() will get a dangling pointer.

        Let's lock the plugin till the end of the statement.
      */
      if (table_type && table_type != view_pseudo_hton)
        ha_lock_engine(thd, table_type);

      if (thd->locked_tables_mode == LTM_LOCK_TABLES ||
          thd->locked_tables_mode == LTM_PRELOCKED_UNDER_LOCK_TABLES)
      {
        if (wait_while_table_is_used(thd, table->table, HA_EXTRA_NOT_USED))
        {
          error= -1;
          goto err;
        }
        /* the following internally does TDC_RT_REMOVE_ALL */
        close_all_tables_for_name(thd, table->table->s,
                                  HA_EXTRA_PREPARE_FOR_DROP, NULL);
        table->table= 0;
      }
      else
        tdc_remove_table(thd, TDC_RT_REMOVE_ALL, table->db.str, table->table_name.str,
                         false);

      /* Check that we have an exclusive lock on the table to be dropped. */
      DBUG_ASSERT(thd->mdl_context.is_lock_owner(MDL_key::TABLE, table->db.str,
                                                 table->table_name.str,
                                                 MDL_EXCLUSIVE));

      // Remove extension for delete
      *(end= path + path_length - reg_ext_length)= '\0';

      if ((error= ha_delete_table(thd, table_type, path, &db, &table->table_name,
                                  !dont_log_query)))
      {
        if (thd->is_killed())
        {
          error= -1;
          goto err;
        }
      }
      else
      {
        /* Delete the table definition file */
        strmov(end,reg_ext);
        if (table_type && table_type != view_pseudo_hton &&
            table_type->discover_table)
        {
          /*
            Table type is using discovery and may not need a .frm file.
            Delete it silently if it exists
          */
          (void) mysql_file_delete(key_file_frm, path, MYF(0));
        }
        else if (unlikely(mysql_file_delete(key_file_frm, path,
                                            MYF(MY_WME))))
        {
          frm_delete_error= my_errno;
          DBUG_ASSERT(frm_delete_error);
        }
      }

      if (likely(!error))
      {
        int trigger_drop_error= 0;

        if (likely(!frm_delete_error))
        {
          non_tmp_table_deleted= TRUE;
          trigger_drop_error=
            Table_triggers_list::drop_all_triggers(thd, &db, &table->table_name);
        }

        if (unlikely(trigger_drop_error) ||
            (frm_delete_error && frm_delete_error != ENOENT))
          error= 1;
        else if (frm_delete_error && if_exists)
          thd->clear_error();
      }
      non_tmp_error|= MY_TEST(error);
    }
non_critical_err:
    if (error)
    {
      if (wrong_tables.length())
        wrong_tables.append(',');
      wrong_tables.append(&db);
      wrong_tables.append('.');
      wrong_tables.append(&table->table_name);
      errors++;
    }
    else
    {
      PSI_CALL_drop_table_share(false, table->db.str, (uint)table->db.length,
                                table->table_name.str, (uint)table->table_name.length);
      mysql_audit_drop_table(thd, table);
    }

log_query:
    if (!dont_log_query && !drop_temporary)
    {
      non_tmp_table_deleted= (if_exists ? TRUE : non_tmp_table_deleted);
      /*
         Don't write the database name if it is the current one (or if
         thd->db is NULL).
       */
      if (thd->db.str == NULL || cmp(&db, &thd->db) != 0)
      {
        append_identifier(thd, &built_query, &db);
        built_query.append(".");
      }

      append_identifier(thd, &built_query, &table->table_name);
      built_query.append(",");
    }
    DBUG_PRINT("table", ("table: %p  s: %p", table->table,
                         table->table ?  table->table->s :  NULL));
  }
  DEBUG_SYNC(thd, "rm_table_no_locks_before_binlog");
  thd->thread_specific_used= TRUE;
  error= 0;
err:
  if (wrong_tables.length())
  {
    DBUG_ASSERT(errors);
    if (errors == 1 && was_view)
      my_error(ER_IT_IS_A_VIEW, MYF(0), wrong_tables.c_ptr_safe());
    else if (errors == 1 && drop_sequence && was_table)
      my_error(ER_NOT_SEQUENCE2, MYF(0), wrong_tables.c_ptr_safe());
    else if (errors > 1 || !thd->is_error())
      my_error((drop_sequence ? ER_UNKNOWN_SEQUENCES :
                ER_BAD_TABLE_ERROR),
               MYF(0), wrong_tables.c_ptr_safe());
    error= 1;
  }

  /*
    We are always logging drop of temporary tables.
    The reason is to handle the following case:
    - Use statement based replication
    - CREATE TEMPORARY TABLE foo (logged)
    - set row based replication
    - DROP TEMPORAY TABLE foo    (needs to be logged)
    This should be fixed so that we remember if creation of the
    temporary table was logged and only log it if the creation was
    logged.
  */

  if (non_trans_tmp_table_deleted ||
      trans_tmp_table_deleted || non_tmp_table_deleted)
  {
    if (non_trans_tmp_table_deleted || trans_tmp_table_deleted)
      thd->transaction.stmt.mark_dropped_temp_table();

    query_cache_invalidate3(thd, tables, 0);
    if (!dont_log_query && mysql_bin_log.is_open())
    {
      if (non_trans_tmp_table_deleted)
      {
          /* Chop of the last comma */
          built_non_trans_tmp_query.chop();
          built_non_trans_tmp_query.append(" /* generated by server */");
          error |= (thd->binlog_query(THD::STMT_QUERY_TYPE,
                                      built_non_trans_tmp_query.ptr(),
                                      built_non_trans_tmp_query.length(),
                                      FALSE, FALSE,
                                      is_drop_tmp_if_exists_added,
                                      0) > 0);
      }
      if (trans_tmp_table_deleted)
      {
          /* Chop of the last comma */
          built_trans_tmp_query.chop();
          built_trans_tmp_query.append(" /* generated by server */");
          error |= (thd->binlog_query(THD::STMT_QUERY_TYPE,
                                      built_trans_tmp_query.ptr(),
                                      built_trans_tmp_query.length(),
                                      TRUE, FALSE,
                                      is_drop_tmp_if_exists_added,
                                      0) > 0);
      }
      if (non_tmp_table_deleted)
      {
          /* Chop of the last comma */
          built_query.chop();
          built_query.append(" /* generated by server */");
          int error_code = non_tmp_error ?  thd->get_stmt_da()->sql_errno()
                                         : 0;
          error |= (thd->binlog_query(THD::STMT_QUERY_TYPE,
                                      built_query.ptr(),
                                      built_query.length(),
                                      TRUE, FALSE, FALSE,
                                      error_code) > 0);
      }
    }
  }

  if (!drop_temporary)
  {
    /*
      Under LOCK TABLES we should release meta-data locks on the tables
      which were dropped.

      Leave LOCK TABLES mode if we managed to drop all tables which were
      locked. Additional check for 'non_temp_tables_count' is to avoid
      leaving LOCK TABLES mode if we have dropped only temporary tables.
    */
    if (thd->locked_tables_mode)
    {
      if (thd->lock && thd->lock->table_count == 0 &&
          non_temp_tables_count > 0 && !dont_free_locks)
      {
        thd->locked_tables_list.unlock_locked_tables(thd);
        goto end;
      }
      for (table= tables; table; table= table->next_local)
      {
        /* Drop locks for all successfully dropped tables. */
        if (table->table == NULL && table->mdl_request.ticket)
        {
          /*
            Under LOCK TABLES we may have several instances of table open
            and locked and therefore have to remove several metadata lock
            requests associated with them.
          */
          thd->mdl_context.release_all_locks_for_name(table->mdl_request.ticket);
        }
      }
    }
    /*
      Rely on the caller to implicitly commit the transaction
      and release metadata locks.
    */
  }

end:
  DBUG_RETURN(error);
}

/**
  Log the drop of a table.

  @param thd	           Thread handler
  @param db_name           Database name
  @param table_name        Table name
  @param temporary_table   1 if table was a temporary table

  This code is only used in the case of failed CREATE OR REPLACE TABLE
  when the original table was dropped but we could not create the new one.
*/

bool log_drop_table(THD *thd, const LEX_CSTRING *db_name,
                    const LEX_CSTRING *table_name,
                    bool temporary_table)
{
  char buff[NAME_LEN*2 + 80];
  String query(buff, sizeof(buff), system_charset_info);
  bool error;
  DBUG_ENTER("log_drop_table");

  if (!mysql_bin_log.is_open())
    DBUG_RETURN(0);
  
  query.length(0);
  query.append(STRING_WITH_LEN("DROP "));
  if (temporary_table)
    query.append(STRING_WITH_LEN("TEMPORARY "));
  query.append(STRING_WITH_LEN("TABLE IF EXISTS "));
  append_identifier(thd, &query, db_name);
  query.append(".");
  append_identifier(thd, &query, table_name);
  query.append(STRING_WITH_LEN("/* Generated to handle "
                               "failed CREATE OR REPLACE */"));
  error= thd->binlog_query(THD::STMT_QUERY_TYPE,
                           query.ptr(), query.length(),
                           FALSE, FALSE, temporary_table, 0) > 0;
  DBUG_RETURN(error);
}


/**
  Quickly remove a table.

  @param thd         Thread context.
  @param base        The handlerton handle.
  @param db          The database name.
  @param table_name  The table name.
  @param flags       Flags for build_table_filename() as well as describing
                     if handler files / .FRM should be deleted as well.

  @return False in case of success, True otherwise.
*/

bool quick_rm_table(THD *thd, handlerton *base, const LEX_CSTRING *db,
                    const LEX_CSTRING *table_name, uint flags, const char *table_path)
{
  char path[FN_REFLEN + 1];
  int error= 0;
  DBUG_ENTER("quick_rm_table");

  size_t path_length= table_path ?
    (strxnmov(path, sizeof(path) - 1, table_path, reg_ext, NullS) - path) :
    build_table_filename(path, sizeof(path)-1, db->str, table_name->str, reg_ext, flags);
  if (mysql_file_delete(key_file_frm, path, MYF(0)))
    error= 1; /* purecov: inspected */
  path[path_length - reg_ext_length]= '\0'; // Remove reg_ext
  if (flags & NO_HA_TABLE)
  {
    handler *file= get_new_handler((TABLE_SHARE*) 0, thd->mem_root, base);
    if (!file)
      DBUG_RETURN(true);
    (void) file->ha_create_partitioning_metadata(path, NULL, CHF_DELETE_FLAG);
    delete file;
  }
  if (!(flags & (FRM_ONLY|NO_HA_TABLE)))
    error|= ha_delete_table(current_thd, base, path, db, table_name, 0);

  if (likely(error == 0))
  {
    PSI_CALL_drop_table_share(flags & FN_IS_TMP, db->str, (uint)db->length,
                              table_name->str, (uint)table_name->length);
  }

  DBUG_RETURN(error);
}


/*
  Sort keys in the following order:
  - PRIMARY KEY
  - UNIQUE keys where all column are NOT NULL
  - UNIQUE keys that don't contain partial segments
  - Other UNIQUE keys
  - LONG UNIQUE keys
  - Normal keys
  - Fulltext keys

  This will make checking for duplicated keys faster and ensure that
  PRIMARY keys are prioritized.
*/

static int sort_keys(KEY *a, KEY *b)
{
  ulong a_flags= a->flags, b_flags= b->flags;
  
  /*
    Do not reorder LONG_HASH indexes, because they must match the order
    of their LONG_UNIQUE_HASH_FIELD's.
  */
  if (a->algorithm == HA_KEY_ALG_LONG_HASH &&
      b->algorithm == HA_KEY_ALG_LONG_HASH)
    return a->usable_key_parts - b->usable_key_parts;

  if (a_flags & HA_NOSAME)
  {
    if (!(b_flags & HA_NOSAME))
      return -1;
    /*
      Long Unique keys should always be last unique key.
      Before this patch they used to change order wrt to partial keys (MDEV-19049)
    */
    if (a->algorithm == HA_KEY_ALG_LONG_HASH)
      return 1;
    if (b->algorithm == HA_KEY_ALG_LONG_HASH)
      return -1;
    if ((a_flags ^ b_flags) & HA_NULL_PART_KEY)
    {
      /* Sort NOT NULL keys before other keys */
      return (a_flags & HA_NULL_PART_KEY) ? 1 : -1;
    }
    if (a->name.str == primary_key_name)
      return -1;
    if (b->name.str == primary_key_name)
      return 1;
    /* Sort keys don't containing partial segments before others */
    if ((a_flags ^ b_flags) & HA_KEY_HAS_PART_KEY_SEG)
      return (a_flags & HA_KEY_HAS_PART_KEY_SEG) ? 1 : -1;
  }
  else if (b_flags & HA_NOSAME)
    return 1;					// Prefer b

  if ((a_flags ^ b_flags) & HA_FULLTEXT)
  {
    return (a_flags & HA_FULLTEXT) ? 1 : -1;
  }
  /*
    Prefer original key order.	usable_key_parts contains here
    the original key position.
  */
  return a->usable_key_parts - b->usable_key_parts;
}

/*
  Check TYPELIB (set or enum) for duplicates

  SYNOPSIS
    check_duplicates_in_interval()
    set_or_name   "SET" or "ENUM" string for warning message
    name	  name of the checked column
    typelib	  list of values for the column
    dup_val_count  returns count of duplicate elements

  DESCRIPTION
    This function prints an warning for each value in list
    which has some duplicates on its right

  RETURN VALUES
    0             ok
    1             Error
*/

bool check_duplicates_in_interval(const char *set_or_name,
                                  const char *name, TYPELIB *typelib,
                                  CHARSET_INFO *cs, unsigned int *dup_val_count)
{
  TYPELIB tmp= *typelib;
  const char **cur_value= typelib->type_names;
  unsigned int *cur_length= typelib->type_lengths;
  *dup_val_count= 0;  
  
  for ( ; tmp.count > 1; cur_value++, cur_length++)
  {
    tmp.type_names++;
    tmp.type_lengths++;
    tmp.count--;
    if (find_type2(&tmp, (const char*)*cur_value, *cur_length, cs))
    {
      THD *thd= current_thd;
      ErrConvString err(*cur_value, *cur_length, cs);
      if (current_thd->is_strict_mode())
      {
        my_error(ER_DUPLICATED_VALUE_IN_TYPE, MYF(0),
                 name, err.ptr(), set_or_name);
        return 1;
      }
      push_warning_printf(thd,Sql_condition::WARN_LEVEL_NOTE,
                          ER_DUPLICATED_VALUE_IN_TYPE,
                          ER_THD(thd, ER_DUPLICATED_VALUE_IN_TYPE),
                          name, err.ptr(), set_or_name);
      (*dup_val_count)++;
    }
  }
  return 0;
}


bool Column_definition::prepare_stage2_blob(handler *file,
                                            ulonglong table_flags,
                                            uint field_flags)
{
  if (table_flags & HA_NO_BLOBS)
  {
    my_error(ER_TABLE_CANT_HANDLE_BLOB, MYF(0), file->table_type());
    return true;
  }
  pack_flag= field_flags |
             pack_length_to_packflag(pack_length - portable_sizeof_char_ptr);
  if (charset->state & MY_CS_BINSORT)
    pack_flag|= FIELDFLAG_BINARY;
  length= 8;                        // Unireg field length
  return false;
}


bool Column_definition::prepare_stage2_typelib(const char *type_name,
                                               uint field_flags,
                                               uint *dup_val_count)
{
  pack_flag= pack_length_to_packflag(pack_length) | field_flags;
  if (charset->state & MY_CS_BINSORT)
    pack_flag|= FIELDFLAG_BINARY;
  return check_duplicates_in_interval(type_name, field_name.str, interval,
                                      charset, dup_val_count);
}


uint Column_definition::pack_flag_numeric(uint dec) const
{
  return (FIELDFLAG_NUMBER |
          (flags & UNSIGNED_FLAG ? 0 : FIELDFLAG_DECIMAL)  |
          (flags & ZEROFILL_FLAG ? FIELDFLAG_ZEROFILL : 0) |
          (dec << FIELDFLAG_DEC_SHIFT));
}


bool Column_definition::prepare_stage2_varchar(ulonglong table_flags)
{
  pack_flag= (charset->state & MY_CS_BINSORT) ? FIELDFLAG_BINARY : 0;
  return false;
}


/*
  Prepare a Column_definition instance for packing
  Members such as pack_flag are valid after this call.

  @param IN     handler      - storage engine handler,
                               or NULL if preparing for an SP variable
  @param IN     table_flags  - table flags

  @retval false  -  ok
  @retval true   -  error (not supported type, bad definition, etc)
*/

bool Column_definition::prepare_stage2(handler *file,
                                       ulonglong table_flags)
{
  DBUG_ENTER("Column_definition::prepare_stage2");

  /*
    This code came from mysql_prepare_create_table.
    Indent preserved to make patching easier
  */
  DBUG_ASSERT(charset);

  if (type_handler()->Column_definition_prepare_stage2(this, file, table_flags))
    DBUG_RETURN(true);

  if (!(flags & NOT_NULL_FLAG) ||
      (vcol_info))  /* Make virtual columns allow NULL values */
    pack_flag|= FIELDFLAG_MAYBE_NULL;
  if (flags & NO_DEFAULT_VALUE_FLAG)
    pack_flag|= FIELDFLAG_NO_DEFAULT;
  DBUG_RETURN(false);
}


/*
  Get character set from field object generated by parser using
  default values when not set.

  SYNOPSIS
    get_sql_field_charset()
    sql_field                 The sql_field object
    create_info               Info generated by parser

  RETURN VALUES
    cs                        Character set
*/

CHARSET_INFO* get_sql_field_charset(Column_definition *sql_field,
                                    HA_CREATE_INFO *create_info)
{
  CHARSET_INFO *cs= sql_field->charset;

  if (!cs)
    cs= create_info->default_table_charset;
  /*
    table_charset is set only in ALTER TABLE t1 CONVERT TO CHARACTER SET csname
    if we want change character set for all varchar/char columns.
    But the table charset must not affect the BLOB fields, so don't
    allow to change my_charset_bin to somethig else.
  */
  if (create_info->table_charset && cs != &my_charset_bin)
    cs= create_info->table_charset;
  return cs;
}


/**
   Modifies the first column definition whose SQL type is TIMESTAMP
   by adding the features DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP.

   If the first TIMESTAMP column appears to be nullable, or to have an
   explicit default, or to be a virtual column, or to be part of table period,
   then no promotion is done.

   @param column_definitions The list of column definitions, in the physical
                             order in which they appear in the table.
*/

void promote_first_timestamp_column(List<Create_field> *column_definitions)
{
  for (Create_field &column_definition : *column_definitions)
  {
    if (column_definition.is_timestamp_type() ||    // TIMESTAMP
        column_definition.unireg_check == Field::TIMESTAMP_OLD_FIELD) // Legacy
    {
      DBUG_PRINT("info", ("field-ptr:%p", column_definition.field));
      if ((column_definition.flags & NOT_NULL_FLAG) != 0 && // NOT NULL,
          column_definition.default_value == NULL &&   // no constant default,
          column_definition.unireg_check == Field::NONE && // no function default
          column_definition.vcol_info == NULL &&
          column_definition.period == NULL &&
          !(column_definition.flags & VERS_SYSTEM_FIELD)) // column isn't generated
      {
        DBUG_PRINT("info", ("First TIMESTAMP column '%s' was promoted to "
                            "DEFAULT CURRENT_TIMESTAMP ON UPDATE "
                            "CURRENT_TIMESTAMP",
                            column_definition.field_name.str
                            ));
        column_definition.unireg_check= Field::TIMESTAMP_DNUN_FIELD;
      }
      return;
    }
  }
}

static bool key_cmp(const Key_part_spec &a, const Key_part_spec &b)
{
  return a.length == b.length &&
         !lex_string_cmp(system_charset_info, &a.field_name, &b.field_name);
}

/**
  Check if there is a duplicate key. Report a warning for every duplicate key.

  @param thd              Thread context.
  @param key              Key to be checked.
  @param key_info         Key meta-data info.
  @param key_list         List of existing keys.
*/
static void check_duplicate_key(THD *thd, const Key *key, const KEY *key_info,
                                const List<Key> *key_list)
{
  /*
    We only check for duplicate indexes if it is requested and the
    key is not auto-generated.

    Check is requested if the key was explicitly created or altered
    by the user (unless it's a foreign key).
  */
  if (!key->key_create_info.check_for_duplicate_indexes || key->generated)
    return;

  for (const Key &k : *key_list)
  {
    // Looking for a similar key...

    if (&k == key)
      break;

    if (k.generated ||
        (key->type != k.type) ||
        (key->key_create_info.algorithm != k.key_create_info.algorithm) ||
        (key->columns.elements != k.columns.elements))
    {
      // Keys are different.
      continue;
    }

    if (std::equal(key->columns.begin(), key->columns.end(), k.columns.begin(),
                   key_cmp))
    {
      push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE, ER_DUP_INDEX,
                          ER_THD(thd, ER_DUP_INDEX), key_info->name.str);
      return;
    }
  }
}


bool Column_definition::prepare_stage1_typelib(THD *thd,
                                               MEM_ROOT *mem_root,
                                               handler *file,
                                               ulonglong table_flags)
{
  /*
    Pass the last parameter to prepare_interval_field() as follows:
    - If we are preparing for an SP variable (file is NULL), we pass "false",
      to force allocation and full copying of TYPELIB values on the given
      mem_root, even if no character set conversion is needed. This is needed
      because a life cycle of an SP variable is longer than the current query.

    - If we are preparing for a CREATE TABLE, (file != NULL), we pass "true".
      This will create the typelib in runtime memory - we will free the
      occupied memory at the same time when we free this
      sql_field -- at the end of execution.
      Pass "true" as the last argument to reuse "interval_list"
      values in "interval" in cases when no character conversion is needed,
      to avoid extra copying.
  */
  if (prepare_interval_field(mem_root, file != NULL))
    return true; // E.g. wrong values with commas: SET('a,b')
  create_length_to_internal_length_typelib();

  DBUG_ASSERT(file || !default_value); // SP variables have no default_value
  if (default_value && default_value->expr->basic_const_item())
  {
    if ((charset != default_value->expr->collation.collation &&
         prepare_stage1_convert_default(thd, mem_root, charset)) ||
         prepare_stage1_check_typelib_default())
      return true;
  }
  return false;
}


bool Column_definition::prepare_stage1_string(THD *thd,
                                              MEM_ROOT *mem_root,
                                              handler *file,
                                              ulonglong table_flags)
{
  create_length_to_internal_length_string();
  if (prepare_blob_field(thd))
    return true;
  DBUG_ASSERT(file || !default_value); // SP variables have no default_value
  /*
    Convert the default value from client character
    set into the column character set if necessary.
    We can only do this for constants as we have not yet run fix_fields.
    But not for blobs, as they will be stored as SQL expressions, not
    written down into the record image.
  */
  if (!(flags & BLOB_FLAG) && default_value &&
      default_value->expr->basic_const_item() &&
      charset != default_value->expr->collation.collation)
  {
    if (prepare_stage1_convert_default(thd, mem_root, charset))
      return true;
  }
  return false;
}


bool Column_definition::prepare_stage1_bit(THD *thd,
                                           MEM_ROOT *mem_root,
                                           handler *file,
                                           ulonglong table_flags)
{
  pack_flag= FIELDFLAG_NUMBER;
  if (!(table_flags & HA_CAN_BIT_FIELD))
    pack_flag|= FIELDFLAG_TREAT_BIT_AS_CHAR;
  create_length_to_internal_length_bit();
  return false;
}


bool Column_definition::prepare_stage1(THD *thd,
                                       MEM_ROOT *mem_root,
                                       handler *file,
                                       ulonglong table_flags)
{
  return type_handler()->Column_definition_prepare_stage1(thd, mem_root,
                                                          this, file,
                                                          table_flags);
}


bool Column_definition::prepare_stage1_convert_default(THD *thd,
                                                       MEM_ROOT *mem_root,
                                                       CHARSET_INFO *cs)
{
  DBUG_ASSERT(thd->mem_root == mem_root);
  Item *item;
  if (!(item= default_value->expr->safe_charset_converter(thd, cs)))
  {
    my_error(ER_INVALID_DEFAULT, MYF(0), field_name.str);
    return true; // Could not convert
  }
  /* Fix for prepare statement */
  thd->change_item_tree(&default_value->expr, item);
  return false;
}


bool Column_definition::prepare_stage1_check_typelib_default()
{
  StringBuffer<MAX_FIELD_WIDTH> str;
  String *def= default_value->expr->val_str(&str);
  bool not_found;
  if (def == NULL) /* SQL "NULL" maps to NULL */
  {
    not_found= flags & NOT_NULL_FLAG;
  }
  else
  {
    not_found= false;
    if (real_field_type() == MYSQL_TYPE_SET)
    {
      char *not_used;
      uint not_used2;
      find_set(interval, def->ptr(), def->length(),
               charset, &not_used, &not_used2, &not_found);
    }
    else /* MYSQL_TYPE_ENUM */
    {
      def->length(charset->cset->lengthsp(charset,
                                          def->ptr(), def->length()));
      not_found= !find_type2(interval, def->ptr(), def->length(), charset);
    }
  }
  if (not_found)
  {
    my_error(ER_INVALID_DEFAULT, MYF(0), field_name.str);
    return true;
  }
  return false;
}
/*
   This function adds a invisible field to field_list
   SYNOPSIS
    mysql_add_invisible_field()
      thd                      Thread Object
      field_list               list of all table fields
      field_name               name/prefix of invisible field
                               ( Prefix in the case when it is
                                *INVISIBLE_FULL*
                               and given name is duplicate)
      type_handler             field data type
      invisible
      default value
    RETURN VALUE
      Create_field pointer
*/
int mysql_add_invisible_field(THD *thd, List<Create_field> * field_list,
        const char *field_name, Type_handler *type_handler,
        field_visibility_t invisible, Item* default_value)
{
  Create_field *fld= new(thd->mem_root)Create_field();
  const char *new_name= NULL;
  /* Get unique field name if invisible == INVISIBLE_FULL */
  if (invisible == INVISIBLE_FULL)
  {
    if ((new_name= make_unique_invisible_field_name(thd, field_name,
                                                     field_list)))
    {
      fld->field_name.str= new_name;
      fld->field_name.length= strlen(new_name);
    }
    else
      return 1;  //Should not happen
  }
  else
  {
    fld->field_name.str= thd->strmake(field_name, strlen(field_name));
    fld->field_name.length= strlen(field_name);
  }
  fld->set_handler(type_handler);
  fld->invisible= invisible;
  if (default_value)
  {
    Virtual_column_info *v= new (thd->mem_root) Virtual_column_info();
    v->expr= default_value;
    v->utf8= 0;
    fld->default_value= v;
  }
  field_list->push_front(fld, thd->mem_root);
  return 0;
}

#define LONG_HASH_FIELD_NAME_LENGTH 30
static inline void make_long_hash_field_name(LEX_CSTRING *buf, uint num)
{
  buf->length= my_snprintf((char *)buf->str,
          LONG_HASH_FIELD_NAME_LENGTH, "DB_ROW_HASH_%u", num);
}

/**
  Add fully invisible hash field to table in case of long
  unique column
  @param  thd           Thread Context.
  @param  create_list   List of table fields.
  @param  key_info      current long unique key info
*/
static Create_field * add_hash_field(THD * thd, List<Create_field> *create_list,
                                      KEY *key_info)
{
  List_iterator<Create_field> it(*create_list);
  Create_field *dup_field, *cf= new (thd->mem_root) Create_field();
  cf->flags|= UNSIGNED_FLAG | LONG_UNIQUE_HASH_FIELD;
  cf->decimals= 0;
  cf->length= cf->char_length= cf->pack_length= HA_HASH_FIELD_LENGTH;
  cf->invisible= INVISIBLE_FULL;
  cf->pack_flag|= FIELDFLAG_MAYBE_NULL;
  cf->vcol_info= new (thd->mem_root) Virtual_column_info();
  cf->vcol_info->stored_in_db= false;
  uint num= 1;
  LEX_CSTRING field_name;
  field_name.str= (char *)thd->alloc(LONG_HASH_FIELD_NAME_LENGTH);
  make_long_hash_field_name(&field_name, num);
  /*
    Check for collisions
   */
  while ((dup_field= it++))
  {
    if (!my_strcasecmp(system_charset_info, field_name.str, dup_field->field_name.str))
    {
      num++;
      make_long_hash_field_name(&field_name, num);
      it.rewind();
    }
  }
  cf->field_name= field_name;
  cf->set_handler(&type_handler_longlong);
  key_info->algorithm= HA_KEY_ALG_LONG_HASH;
  create_list->push_back(cf,thd->mem_root);
  return cf;
}

Key *
mysql_add_invisible_index(THD *thd, List<Key> *key_list,
        LEX_CSTRING* field_name, enum Key::Keytype type)
{
  Key *key= NULL;
  key= new (thd->mem_root) Key(type, &null_clex_str, HA_KEY_ALG_UNDEF,
         false, DDL_options(DDL_options::OPT_NONE));
  key->columns.push_back(new(thd->mem_root) Key_part_spec(field_name, 0, true),
          thd->mem_root);
  key_list->push_back(key, thd->mem_root);
  return key;
}
/*
  Preparation for table creation

  SYNOPSIS
    mysql_prepare_create_table()
      thd                       Thread object.
      create_info               Create information (like MAX_ROWS).
      alter_info                List of columns and indexes to create
      db_options          INOUT Table options (like HA_OPTION_PACK_RECORD).
      file                      The handler for the new table.
      key_info_buffer     OUT   An array of KEY structs for the indexes.
      key_count           OUT   The number of elements in the array.
      create_table_mode         C_ORDINARY_CREATE, C_ALTER_TABLE,
                                C_CREATE_SELECT, C_ASSISTED_DISCOVERY

  DESCRIPTION
    Prepares the table and key structures for table creation.

  NOTES
    sets create_info->varchar if the table has a varchar

  RETURN VALUES
    FALSE    OK
    TRUE     error
*/

static int
mysql_prepare_create_table(THD *thd, HA_CREATE_INFO *create_info,
                           Alter_info *alter_info, uint *db_options,
                           handler *file, KEY **key_info_buffer,
                           uint *key_count, int create_table_mode)
{
  const char	*key_name;
  Create_field	*sql_field,*dup_field;
  uint		field,null_fields,max_key_length;
  ulong		record_offset= 0;
  KEY		*key_info;
  KEY_PART_INFO *key_part_info;
  int		field_no,dup_no;
  int		select_field_pos,auto_increment=0;
  List_iterator_fast<Create_field> it(alter_info->create_list);
  List_iterator<Create_field> it2(alter_info->create_list);
  uint total_uneven_bit_length= 0;
  int select_field_count= C_CREATE_SELECT(create_table_mode);
  bool tmp_table= create_table_mode == C_ALTER_TABLE;
  bool is_hash_field_needed= false;
  DBUG_ENTER("mysql_prepare_create_table");

  DBUG_EXECUTE_IF("test_pseudo_invisible",{
          mysql_add_invisible_field(thd, &alter_info->create_list,
                      "invisible", &type_handler_long, INVISIBLE_SYSTEM,
                      new (thd->mem_root)Item_int(thd, 9));
          });
  DBUG_EXECUTE_IF("test_completely_invisible",{
          mysql_add_invisible_field(thd, &alter_info->create_list,
                      "invisible", &type_handler_long, INVISIBLE_FULL,
                      new (thd->mem_root)Item_int(thd, 9));
          });
  DBUG_EXECUTE_IF("test_invisible_index",{
          LEX_CSTRING temp;
          temp.str= "invisible";
          temp.length= strlen("invisible");
          mysql_add_invisible_index(thd, &alter_info->key_list
                  , &temp, Key::MULTIPLE);
          });
  LEX_CSTRING* connect_string = &create_info->connect_string;
  if (connect_string->length != 0 &&
      connect_string->length > CONNECT_STRING_MAXLEN &&
      (system_charset_info->cset->charpos(system_charset_info,
                                          connect_string->str,
                                          (connect_string->str +
                                           connect_string->length),
                                          CONNECT_STRING_MAXLEN)
      < connect_string->length))
  {
    my_error(ER_WRONG_STRING_LENGTH, MYF(0),
             connect_string->str, "CONNECTION", CONNECT_STRING_MAXLEN);
    DBUG_RETURN(TRUE);
  }

  select_field_pos= alter_info->create_list.elements - select_field_count;
  null_fields= 0;
  create_info->varchar= 0;
  max_key_length= file->max_key_length();

  /* Handle creation of sequences */
  if (create_info->sequence)
  {
    if (!(file->ha_table_flags() & HA_CAN_TABLES_WITHOUT_ROLLBACK))
    {
      my_error(ER_ILLEGAL_HA_CREATE_OPTION, MYF(0), file->table_type(),
               "SEQUENCE");
      DBUG_RETURN(TRUE);
    }

    /* The user specified fields: check that structure is ok */
    if (check_sequence_fields(thd->lex, &alter_info->create_list))
      DBUG_RETURN(TRUE);
  }

  for (field_no=0; (sql_field=it++) ; field_no++)
  {
    /*
      Initialize length from its original value (number of characters),
      which was set in the parser. This is necessary if we're
      executing a prepared statement for the second time.
    */
    sql_field->length= sql_field->char_length;
    /* Set field charset. */
    sql_field->charset= get_sql_field_charset(sql_field, create_info);
    if ((sql_field->flags & BINCMP_FLAG) &&
        !(sql_field->charset= find_bin_collation(sql_field->charset)))
      DBUG_RETURN(true);

    /* Virtual fields are always NULL */
    if (sql_field->vcol_info)
      sql_field->flags&= ~NOT_NULL_FLAG;

    if (sql_field->prepare_stage1(thd, thd->mem_root,
                                  file, file->ha_table_flags()))
      DBUG_RETURN(true);

    if (sql_field->real_field_type() == MYSQL_TYPE_BIT &&
        file->ha_table_flags() & HA_CAN_BIT_FIELD)
      total_uneven_bit_length+= sql_field->length & 7;

    if (!(sql_field->flags & NOT_NULL_FLAG))
      null_fields++;

    if (check_column_name(sql_field->field_name.str))
    {
      my_error(ER_WRONG_COLUMN_NAME, MYF(0), sql_field->field_name.str);
      DBUG_RETURN(TRUE);
    }

    /* Check if we have used the same field name before */
    for (dup_no=0; (dup_field=it2++) != sql_field; dup_no++)
    {
      if (lex_string_cmp(system_charset_info,
                         &sql_field->field_name,
                         &dup_field->field_name) == 0)
      {
	/*
	  If this was a CREATE ... SELECT statement, accept a field
	  redefinition if we are changing a field in the SELECT part
	*/
	if (field_no < select_field_pos || dup_no >= select_field_pos ||
            dup_field->invisible >= INVISIBLE_SYSTEM)
	{
	  my_error(ER_DUP_FIELDNAME, MYF(0), sql_field->field_name.str);
	  DBUG_RETURN(TRUE);
	}
	else
	{
	  /* Field redefined */

          /*
            If we are replacing a BIT field, revert the increment
            of total_uneven_bit_length that was done above.
          */
          if (sql_field->real_field_type() == MYSQL_TYPE_BIT &&
              file->ha_table_flags() & HA_CAN_BIT_FIELD)
            total_uneven_bit_length-= sql_field->length & 7;

          /* 
            We're making one field from two, the result field will have
            dup_field->flags as flags. If we've incremented null_fields
            because of sql_field->flags, decrement it back.
          */
          if (!(sql_field->flags & NOT_NULL_FLAG))
            null_fields--;

          if (sql_field->redefine_stage1(dup_field, file, create_info))
            DBUG_RETURN(true);

	  it2.remove();			// Remove first (create) definition
	  select_field_pos--;
	  break;
	}
      }
    }
    /* Don't pack rows in old tables if the user has requested this */
    if ((sql_field->flags & BLOB_FLAG) ||
	(sql_field->real_field_type() == MYSQL_TYPE_VARCHAR &&
         create_info->row_type != ROW_TYPE_FIXED))
      (*db_options)|= HA_OPTION_PACK_RECORD;
    it2.rewind();
  }

  /* record_offset will be increased with 'length-of-null-bits' later */
  record_offset= 0;
  null_fields+= total_uneven_bit_length;

  it.rewind();
  while ((sql_field=it++))
  {
    DBUG_ASSERT(sql_field->charset != 0);
    if (sql_field->prepare_stage2(file, file->ha_table_flags()))
      DBUG_RETURN(TRUE);
    if (sql_field->real_field_type() == MYSQL_TYPE_VARCHAR)
      create_info->varchar= TRUE;
    sql_field->offset= record_offset;
    if (MTYP_TYPENR(sql_field->unireg_check) == Field::NEXT_NUMBER)
      auto_increment++;
    if (parse_option_list(thd, create_info->db_type, &sql_field->option_struct,
                          &sql_field->option_list,
                          create_info->db_type->field_options, FALSE,
                          thd->mem_root))
      DBUG_RETURN(TRUE);
    /*
      For now skip fields that are not physically stored in the database
      (virtual fields) and update their offset later 
      (see the next loop).
    */
    if (sql_field->stored_in_db())
      record_offset+= sql_field->pack_length;
    if (sql_field->flags & VERS_SYSTEM_FIELD)
      continue;
  }
  /* Update virtual fields' offset and give error if
     All fields are invisible */
  bool is_all_invisible= true;
  it.rewind();
  while ((sql_field=it++))
  {
    if (!sql_field->stored_in_db())
    {
      sql_field->offset= record_offset;
      record_offset+= sql_field->pack_length;
    }
    if (sql_field->invisible == VISIBLE)
      is_all_invisible= false;
  }
  if (is_all_invisible)
  {
    my_error(ER_TABLE_MUST_HAVE_COLUMNS, MYF(0));
    DBUG_RETURN(TRUE);
  }
  if (auto_increment > 1)
  {
    my_message(ER_WRONG_AUTO_KEY, ER_THD(thd, ER_WRONG_AUTO_KEY), MYF(0));
    DBUG_RETURN(TRUE);
  }
  if (auto_increment &&
      (file->ha_table_flags() & HA_NO_AUTO_INCREMENT))
  {
    my_error(ER_TABLE_CANT_HANDLE_AUTO_INCREMENT, MYF(0), file->table_type());
    DBUG_RETURN(TRUE);
  }

  /*
   CREATE TABLE[with auto_increment column] SELECT is unsafe as the rows
   inserted in the created table depends on the order of the rows fetched
   from the select tables. This order may differ on master and slave. We
   therefore mark it as unsafe.
  */
  if (select_field_count > 0 && auto_increment)
    thd->lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_CREATE_SELECT_AUTOINC);

  /* Create keys */

  List_iterator<Key> key_iterator(alter_info->key_list);
  List_iterator<Key> key_iterator2(alter_info->key_list);
  uint key_parts=0, fk_key_count=0;
  bool primary_key=0,unique_key=0;
  Key *key, *key2;
  uint tmp, key_number;
  /* special marker for keys to be ignored */
  static char ignore_key[1];

  /* Calculate number of key segements */
  *key_count= 0;

  while ((key=key_iterator++))
  {
    DBUG_PRINT("info", ("key name: '%s'  type: %d", key->name.str ? key->name.str :
                        "(none)" , key->type));
    if (key->type == Key::FOREIGN_KEY)
    {
      fk_key_count++;
      Foreign_key *fk_key= (Foreign_key*) key;
      if (fk_key->validate(alter_info->create_list))
        DBUG_RETURN(TRUE);
      if (fk_key->ref_columns.elements &&
	  fk_key->ref_columns.elements != fk_key->columns.elements)
      {
        my_error(ER_WRONG_FK_DEF, MYF(0),
                 (fk_key->name.str ? fk_key->name.str :
                                     "foreign key without name"),
                 ER_THD(thd, ER_KEY_REF_DO_NOT_MATCH_TABLE_REF));
	DBUG_RETURN(TRUE);
      }
      continue;
    }
    (*key_count)++;
    tmp=file->max_key_parts();
    if (key->columns.elements > tmp)
    {
      my_error(ER_TOO_MANY_KEY_PARTS,MYF(0),tmp);
      DBUG_RETURN(TRUE);
    }
    if (check_ident_length(&key->name))
      DBUG_RETURN(TRUE);
    key_iterator2.rewind ();
    if (key->type != Key::FOREIGN_KEY)
    {
      while ((key2 = key_iterator2++) != key)
      {
	/*
          foreign_key_prefix(key, key2) returns 0 if key or key2, or both, is
          'generated', and a generated key is a prefix of the other key.
          Then we do not need the generated shorter key.
        */
        if ((key2->type != Key::FOREIGN_KEY &&
             key2->name.str != ignore_key &&
             !foreign_key_prefix(key, key2)))
        {
          /* TODO: issue warning message */
          /* mark that the generated key should be ignored */
          if (!key2->generated ||
              (key->generated && key->columns.elements <
               key2->columns.elements))
            key->name.str= ignore_key;
          else
          {
            key2->name.str= ignore_key;
            key_parts-= key2->columns.elements;
            (*key_count)--;
          }
          break;
        }
      }
    }
    if (key->name.str != ignore_key)
      key_parts+=key->columns.elements;
    else
      (*key_count)--;
    if (key->name.str && !tmp_table && (key->type != Key::PRIMARY) &&
	!my_strcasecmp(system_charset_info, key->name.str, primary_key_name))
    {
      my_error(ER_WRONG_NAME_FOR_INDEX, MYF(0), key->name.str);
      DBUG_RETURN(TRUE);
    }
    if (key->type == Key::PRIMARY && key->name.str &&
        my_strcasecmp(system_charset_info, key->name.str, primary_key_name) != 0)
    {
      bool sav_abort_on_warning= thd->abort_on_warning;
      thd->abort_on_warning= FALSE; /* Don't make an error out of this. */
      push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                          ER_WRONG_NAME_FOR_INDEX,
                          "Name '%-.100s' ignored for PRIMARY key.",
                          key->name.str);
      thd->abort_on_warning= sav_abort_on_warning;
    }
  }
  tmp=file->max_keys();
  if (*key_count > tmp)
  {
    my_error(ER_TOO_MANY_KEYS,MYF(0),tmp);
    DBUG_RETURN(TRUE);
  }

  (*key_info_buffer)= key_info= (KEY*) thd->calloc(sizeof(KEY) * (*key_count));
  key_part_info=(KEY_PART_INFO*) thd->calloc(sizeof(KEY_PART_INFO)*key_parts);
  if (!*key_info_buffer || ! key_part_info)
    DBUG_RETURN(TRUE);				// Out of memory

  key_iterator.rewind();
  key_number=0;
  for (; (key=key_iterator++) ; key_number++)
  {
    uint key_length=0;
    Key_part_spec *column;

    is_hash_field_needed= false;
    if (key->name.str == ignore_key)
    {
      /* ignore redundant keys */
      do
	key=key_iterator++;
      while (key && key->name.str == ignore_key);
      if (!key)
	break;
    }

    switch (key->type) {
    case Key::MULTIPLE:
	key_info->flags= 0;
	break;
    case Key::FULLTEXT:
	key_info->flags= HA_FULLTEXT;
	if ((key_info->parser_name= &key->key_create_info.parser_name)->str)
          key_info->flags|= HA_USES_PARSER;
        else
          key_info->parser_name= 0;
	break;
    case Key::SPATIAL:
#ifdef HAVE_SPATIAL
	key_info->flags= HA_SPATIAL;
	break;
#else
	my_error(ER_FEATURE_DISABLED, MYF(0),
                 sym_group_geom.name, sym_group_geom.needed_define);
	DBUG_RETURN(TRUE);
#endif
    case Key::FOREIGN_KEY:
      key_number--;				// Skip this key
      continue;
    default:
      key_info->flags = HA_NOSAME;
      break;
    }
    if (key->generated)
      key_info->flags|= HA_GENERATED_KEY;

    key_info->user_defined_key_parts=(uint8) key->columns.elements;
    key_info->key_part=key_part_info;
    key_info->usable_key_parts= key_number;
    key_info->algorithm= key->key_create_info.algorithm;
    key_info->option_list= key->option_list;
    if (parse_option_list(thd, create_info->db_type, &key_info->option_struct,
                          &key_info->option_list,
                          create_info->db_type->index_options, FALSE,
                          thd->mem_root))
      DBUG_RETURN(TRUE);

    if (key->type == Key::FULLTEXT)
    {
      if (!(file->ha_table_flags() & HA_CAN_FULLTEXT))
      {
	my_error(ER_TABLE_CANT_HANDLE_FT, MYF(0), file->table_type());
	DBUG_RETURN(TRUE);
      }
    }
    /*
       Make SPATIAL to be RTREE by default
       SPATIAL only on BLOB or at least BINARY, this
       actually should be replaced by special GEOM type
       in near future when new frm file is ready
       checking for proper key parts number:
    */

    /* TODO: Add proper checks if handler supports key_type and algorithm */
    if (key_info->flags & HA_SPATIAL)
    {
      if (!(file->ha_table_flags() & HA_CAN_RTREEKEYS))
      {
	my_error(ER_TABLE_CANT_HANDLE_SPKEYS, MYF(0), file->table_type());
        DBUG_RETURN(TRUE);
      }
      if (key_info->user_defined_key_parts != 1)
      {
	my_error(ER_WRONG_ARGUMENTS, MYF(0), "SPATIAL INDEX");
	DBUG_RETURN(TRUE);
      }
    }
    else if (key_info->algorithm == HA_KEY_ALG_RTREE)
    {
#ifdef HAVE_RTREE_KEYS
      if ((key_info->user_defined_key_parts & 1) == 1)
      {
	my_error(ER_WRONG_ARGUMENTS, MYF(0), "RTREE INDEX");
	DBUG_RETURN(TRUE);
      }
      /* TODO: To be deleted */
      my_error(ER_NOT_SUPPORTED_YET, MYF(0), "RTREE INDEX");
      DBUG_RETURN(TRUE);
#else
      my_error(ER_FEATURE_DISABLED, MYF(0),
               sym_group_rtree.name, sym_group_rtree.needed_define);
      DBUG_RETURN(TRUE);
#endif
    }

    /* Take block size from key part or table part */
    /*
      TODO: Add warning if block size changes. We can't do it here, as
      this may depend on the size of the key
    */
    key_info->block_size= (key->key_create_info.block_size ?
                           key->key_create_info.block_size :
                           create_info->key_block_size);

    /*
      Remember block_size for the future if the block size was given
      either for key or table and it was given for the key during
      create/alter table or we have an active key_block_size for the
      table.
      The idea is that table specific key_block_size > 0 will only affect
      new keys and old keys will remember their original value.
    */
    if (key_info->block_size &&
        ((key->key_create_info.flags & HA_USES_BLOCK_SIZE) ||
         create_info->key_block_size))
      key_info->flags|= HA_USES_BLOCK_SIZE;

    List_iterator<Key_part_spec> cols(key->columns), cols2(key->columns);
    CHARSET_INFO *ft_key_charset=0;  // for FULLTEXT
    for (uint column_nr=0 ; (column=cols++) ; column_nr++)
    {
      Key_part_spec *dup_column;

      it.rewind();
      field=0;
      while ((sql_field=it++) &&
	     lex_string_cmp(system_charset_info,
                            &column->field_name,
                            &sql_field->field_name))
	field++;
      /*
         Either field is not present or field visibility is > INVISIBLE_USER
      */
      if (!sql_field || (sql_field->invisible > INVISIBLE_USER &&
                         !column->generated))
      {
	my_error(ER_KEY_COLUMN_DOES_NOT_EXITS, MYF(0), column->field_name.str);
	DBUG_RETURN(TRUE);
      }
      if (sql_field->invisible > INVISIBLE_USER &&
          !(sql_field->flags & VERS_SYSTEM_FIELD) &&
          !key->invisible && DBUG_EVALUATE_IF("test_invisible_index", 0, 1))
      {
        my_error(ER_KEY_COLUMN_DOES_NOT_EXITS, MYF(0), column->field_name.str);
        DBUG_RETURN(TRUE);
      }
      while ((dup_column= cols2++) != column)
      {
        if (!lex_string_cmp(system_charset_info,
                            &column->field_name, &dup_column->field_name))
	{
	  my_error(ER_DUP_FIELDNAME, MYF(0), column->field_name.str);
	  DBUG_RETURN(TRUE);
	}
      }

      if (sql_field->compression_method())
      {
        my_error(ER_COMPRESSED_COLUMN_USED_AS_KEY, MYF(0),
                 column->field_name.str);
        DBUG_RETURN(TRUE);
      }

      cols2.rewind();
      if (key->type == Key::FULLTEXT)
      {
	if ((sql_field->real_field_type() != MYSQL_TYPE_STRING &&
	     sql_field->real_field_type() != MYSQL_TYPE_VARCHAR &&
	     !f_is_blob(sql_field->pack_flag)) ||
	    sql_field->charset == &my_charset_bin ||
	    sql_field->charset->mbminlen > 1 || // ucs2 doesn't work yet
	    (ft_key_charset && sql_field->charset != ft_key_charset))
	{
	    my_error(ER_BAD_FT_COLUMN, MYF(0), column->field_name.str);
	    DBUG_RETURN(-1);
	}
	ft_key_charset=sql_field->charset;
	/*
	  for fulltext keys keyseg length is 1 for blobs (it's ignored in ft
	  code anyway, and 0 (set to column width later) for char's. it has
	  to be correct col width for char's, as char data are not prefixed
	  with length (unlike blobs, where ft code takes data length from a
	  data prefix, ignoring column->length).
	*/
        column->length= MY_TEST(f_is_blob(sql_field->pack_flag));
      }
      else
      {
	column->length*= sql_field->charset->mbmaxlen;

        if (key->type == Key::SPATIAL)
        {
          if (column->length)
          {
            my_error(ER_WRONG_SUB_KEY, MYF(0));
            DBUG_RETURN(TRUE);
          }
          if (!f_is_geom(sql_field->pack_flag))
          {
            my_error(ER_WRONG_ARGUMENTS, MYF(0), "SPATIAL INDEX");
            DBUG_RETURN(TRUE);
          }
        }

	if (f_is_blob(sql_field->pack_flag) ||
            (f_is_geom(sql_field->pack_flag) && key->type != Key::SPATIAL))
        {
          if (!(file->ha_table_flags() & HA_CAN_INDEX_BLOBS))
          {
            my_error(ER_BLOB_USED_AS_KEY, MYF(0), column->field_name.str,
                     file->table_type());
            DBUG_RETURN(TRUE);
          }
          if (f_is_geom(sql_field->pack_flag) && sql_field->geom_type ==
              Field::GEOM_POINT)
            column->length= MAX_LEN_GEOM_POINT_FIELD;
          if (!column->length)
          {
            if (key->type == Key::UNIQUE)
              is_hash_field_needed= true;
            else if (key->type == Key::MULTIPLE)
              column->length= file->max_key_length() + 1;
            else
            {
              my_error(ER_BLOB_KEY_WITHOUT_LENGTH, MYF(0), column->field_name.str);
              DBUG_RETURN(TRUE);
            }
          }
        }
#ifdef HAVE_SPATIAL
	if (key->type == Key::SPATIAL)
	{
	  if (!column->length)
	  {
	    /*
              4 is: (Xmin,Xmax,Ymin,Ymax), this is for 2D case
              Lately we'll extend this code to support more dimensions
	    */
	    column->length= 4*sizeof(double);
	  }
	}
#endif
        if (sql_field->vcol_info)
        {
          if (key->type == Key::PRIMARY)
          {
            my_error(ER_PRIMARY_KEY_BASED_ON_GENERATED_COLUMN, MYF(0));
            DBUG_RETURN(TRUE);
          }
          if (sql_field->vcol_info->flags & VCOL_NOT_STRICTLY_DETERMINISTIC)
          {
            /* use check_expression() to report an error */
            check_expression(sql_field->vcol_info, &sql_field->field_name,
                             VCOL_GENERATED_STORED);
            DBUG_ASSERT(thd->is_error());
            DBUG_RETURN(TRUE);
          }
        }
	if (!(sql_field->flags & NOT_NULL_FLAG))
	{
	  if (key->type == Key::PRIMARY)
	  {
	    /* Implicitly set primary key fields to NOT NULL for ISO conf. */
	    sql_field->flags|= NOT_NULL_FLAG;
	    sql_field->pack_flag&= ~FIELDFLAG_MAYBE_NULL;
            null_fields--;
	  }
	  else
          {
            key_info->flags|= HA_NULL_PART_KEY;
            if (!(file->ha_table_flags() & HA_NULL_IN_KEY))
            {
              my_error(ER_NULL_COLUMN_IN_INDEX, MYF(0), column->field_name.str);
              DBUG_RETURN(TRUE);
            }
            if (key->type == Key::SPATIAL)
            {
              my_message(ER_SPATIAL_CANT_HAVE_NULL,
                         ER_THD(thd, ER_SPATIAL_CANT_HAVE_NULL), MYF(0));
              DBUG_RETURN(TRUE);
            }
          }
	}
	if (MTYP_TYPENR(sql_field->unireg_check) == Field::NEXT_NUMBER)
	{
	  if (column_nr == 0 || (file->ha_table_flags() & HA_AUTO_PART_KEY))
	    auto_increment--;			// Field is used
	}
      }

      key_part_info->fieldnr= field;
      key_part_info->offset=  (uint16) sql_field->offset;
      key_part_info->key_type=sql_field->pack_flag;
      uint key_part_length= sql_field->key_length;

      if (column->length)
      {
        if (f_is_blob(sql_field->pack_flag))
        {
          key_part_length= MY_MIN(column->length,
                                  blob_length_by_type(sql_field->real_field_type())
                                  * sql_field->charset->mbmaxlen);
          if (key_part_length > max_key_length ||
              key_part_length > file->max_key_part_length())
          {
            if (key->type == Key::MULTIPLE)
            {
              key_part_length= MY_MIN(max_key_length, file->max_key_part_length());
              /* not a critical problem */
              push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE,
                                  ER_TOO_LONG_KEY, ER_THD(thd, ER_TOO_LONG_KEY),
                                  key_part_length);
              /* Align key length to multibyte char boundary */
              key_part_length-= key_part_length % sql_field->charset->mbmaxlen;
            }
            else
              is_hash_field_needed= true;
          }
        }
        // Catch invalid use of partial keys 
        else if (!f_is_geom(sql_field->pack_flag) &&
                 // is the key partial? 
                 column->length != key_part_length &&
                 // is prefix length bigger than field length? 
                 (column->length > key_part_length ||
                  // can the field have a partial key? 
                  !sql_field->type_handler()->type_can_have_key_part() ||
                  // a packed field can't be used in a partial key
                  f_is_packed(sql_field->pack_flag) ||
                  // does the storage engine allow prefixed search?
                  ((file->ha_table_flags() & HA_NO_PREFIX_CHAR_KEYS) &&
                   // and is this a 'unique' key?
                   (key_info->flags & HA_NOSAME))))
        {
          my_message(ER_WRONG_SUB_KEY, ER_THD(thd, ER_WRONG_SUB_KEY), MYF(0));
          DBUG_RETURN(TRUE);
        }
        else if (!(file->ha_table_flags() & HA_NO_PREFIX_CHAR_KEYS))
          key_part_length= column->length;
      }
      else if (key_part_length == 0 && (sql_field->flags & NOT_NULL_FLAG) &&
              !is_hash_field_needed)
      {
	my_error(ER_WRONG_KEY_COLUMN, MYF(0), file->table_type(),
                 column->field_name.str);
	  DBUG_RETURN(TRUE);
      }
      if (key_part_length > file->max_key_part_length() &&
          key->type != Key::FULLTEXT)
      {
        if (key->type == Key::MULTIPLE)
        {
          key_part_length= file->max_key_part_length();
          /* not a critical problem */
          push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE,
                              ER_TOO_LONG_KEY, ER_THD(thd, ER_TOO_LONG_KEY),
                              key_part_length);
          /* Align key length to multibyte char boundary */
          key_part_length-= key_part_length % sql_field->charset->mbmaxlen;
        }
        else
        {
          if (key->type == Key::UNIQUE)
          {
            is_hash_field_needed= true;
          }
          else
          {
            key_part_length= MY_MIN(max_key_length, file->max_key_part_length());
            my_error(ER_TOO_LONG_KEY, MYF(0), key_part_length);
            DBUG_RETURN(TRUE);
          }
        }
      }
      /* We can not store key_part_length more then 2^16 - 1 in frm */
      if (is_hash_field_needed && column->length > UINT_MAX16)
      {
        my_error(ER_TOO_LONG_KEYPART, MYF(0),  UINT_MAX16);
        DBUG_RETURN(TRUE);
      }
      else
        key_part_info->length= (uint16) key_part_length;
      /* Use packed keys for long strings on the first column */
      if (!((*db_options) & HA_OPTION_NO_PACK_KEYS) &&
          !((create_info->table_options & HA_OPTION_NO_PACK_KEYS)) &&
          (key_part_length >= KEY_DEFAULT_PACK_LENGTH &&
           (sql_field->real_field_type() == MYSQL_TYPE_STRING ||
            sql_field->real_field_type() == MYSQL_TYPE_VARCHAR ||
            f_is_blob(sql_field->pack_flag))) && !is_hash_field_needed)
      {
        if ((column_nr == 0 && f_is_blob(sql_field->pack_flag)) ||
            sql_field->real_field_type() == MYSQL_TYPE_VARCHAR)
          key_info->flags|= HA_BINARY_PACK_KEY | HA_VAR_LENGTH_KEY;
        else
          key_info->flags|= HA_PACK_KEY;
      }
      /* Check if the key segment is partial, set the key flag accordingly */
      if (key_part_length != sql_field->key_length &&
          key_part_length != sql_field->type_handler()->max_octet_length())
        key_info->flags|= HA_KEY_HAS_PART_KEY_SEG;

      key_length+= key_part_length;
      key_part_info++;

      /* Create the key name based on the first column (if not given) */
      if (column_nr == 0)
      {
	if (key->type == Key::PRIMARY)
	{
	  if (primary_key)
	  {
	    my_message(ER_MULTIPLE_PRI_KEY, ER_THD(thd, ER_MULTIPLE_PRI_KEY),
                       MYF(0));
	    DBUG_RETURN(TRUE);
	  }
	  key_name=primary_key_name;
	  primary_key=1;
	}
	else if (!(key_name= key->name.str))
	  key_name=make_unique_key_name(thd, sql_field->field_name.str,
					*key_info_buffer, key_info);
	if (check_if_keyname_exists(key_name, *key_info_buffer, key_info))
	{
	  my_error(ER_DUP_KEYNAME, MYF(0), key_name);
	  DBUG_RETURN(TRUE);
	}
	key_info->name.str= (char*) key_name;
        key_info->name.length= strlen(key_name);
      }
    }
    if (!key_info->name.str || check_column_name(key_info->name.str))
    {
      my_error(ER_WRONG_NAME_FOR_INDEX, MYF(0), key_info->name.str);
      DBUG_RETURN(TRUE);
    }
    if (key->type == Key::UNIQUE && !(key_info->flags & HA_NULL_PART_KEY))
      unique_key=1;
    key_info->key_length=(uint16) key_length;
    if (key_info->key_length > max_key_length && key->type == Key::UNIQUE)
      is_hash_field_needed= true;
    if (key_length > max_key_length && key->type != Key::FULLTEXT &&
        !is_hash_field_needed)
    {
      my_error(ER_TOO_LONG_KEY, MYF(0), max_key_length);
      DBUG_RETURN(TRUE);
    }

    if (is_hash_field_needed && key_info->algorithm != HA_KEY_ALG_UNDEF &&
       key_info->algorithm != HA_KEY_ALG_HASH )
    {
      my_error(ER_TOO_LONG_KEY, MYF(0), max_key_length);
      DBUG_RETURN(TRUE);
    }
    if (is_hash_field_needed ||
        (key_info->algorithm == HA_KEY_ALG_HASH &&
         key->type != Key::PRIMARY &&
         key_info->flags & HA_NOSAME &&
         !(file->ha_table_flags() & HA_CAN_HASH_KEYS ) &&
         file->ha_table_flags() & HA_CAN_VIRTUAL_COLUMNS))
    {
      Create_field *hash_fld= add_hash_field(thd, &alter_info->create_list,
                                             key_info);
      if (!hash_fld)
        DBUG_RETURN(TRUE);
      hash_fld->offset= record_offset;
      hash_fld->charset= create_info->default_table_charset;
      record_offset+= hash_fld->pack_length;
      if (key_info->flags & HA_NULL_PART_KEY)
        null_fields++;
      else
      {
        hash_fld->flags|= NOT_NULL_FLAG;
        hash_fld->pack_flag&= ~FIELDFLAG_MAYBE_NULL;
      }
    }
    if (validate_comment_length(thd, &key->key_create_info.comment,
                                INDEX_COMMENT_MAXLEN,
                                ER_TOO_LONG_INDEX_COMMENT,
                                key_info->name.str))
       DBUG_RETURN(TRUE);

    key_info->comment.length= key->key_create_info.comment.length;
    if (key_info->comment.length > 0)
    {
      key_info->flags|= HA_USES_COMMENT;
      key_info->comment.str= key->key_create_info.comment.str;
    }

    // Check if a duplicate index is defined.
    check_duplicate_key(thd, key, key_info, &alter_info->key_list);
    key_info++;
  }

  if (!unique_key && !primary_key && !create_info->sequence &&
      (file->ha_table_flags() & HA_REQUIRE_PRIMARY_KEY))
  {
    my_message(ER_REQUIRES_PRIMARY_KEY, ER_THD(thd, ER_REQUIRES_PRIMARY_KEY),
               MYF(0));
    DBUG_RETURN(TRUE);
  }
  if (auto_increment > 0)
  {
    my_message(ER_WRONG_AUTO_KEY, ER_THD(thd, ER_WRONG_AUTO_KEY), MYF(0));
    DBUG_RETURN(TRUE);
  }
  /* Sort keys in optimized order */
  my_qsort((uchar*) *key_info_buffer, *key_count, sizeof(KEY),
	   (qsort_cmp) sort_keys);
  create_info->null_bits= null_fields;

  /* Check fields. */
  it.rewind();
  while ((sql_field=it++))
  {
    Field::utype type= (Field::utype) MTYP_TYPENR(sql_field->unireg_check);

    /*
      Set NO_DEFAULT_VALUE_FLAG if this field doesn't have a default value and
      it is NOT NULL, not an AUTO_INCREMENT field, not a TIMESTAMP and not
      updated trough a NOW() function.
    */
    if (!sql_field->default_value &&
        !sql_field->has_default_function() &&
        (sql_field->flags & NOT_NULL_FLAG) &&
        (!sql_field->is_timestamp_type() ||
         opt_explicit_defaults_for_timestamp)&&
        !sql_field->vers_sys_field())
    {
      sql_field->flags|= NO_DEFAULT_VALUE_FLAG;
      sql_field->pack_flag|= FIELDFLAG_NO_DEFAULT;
    }

    if (thd->variables.sql_mode & MODE_NO_ZERO_DATE &&
        !sql_field->default_value && !sql_field->vcol_info &&
        !sql_field->vers_sys_field() &&
        sql_field->is_timestamp_type() &&
        !opt_explicit_defaults_for_timestamp &&
        (sql_field->flags & NOT_NULL_FLAG) &&
        (type == Field::NONE || type == Field::TIMESTAMP_UN_FIELD))
    {
      /*
        An error should be reported if:
          - NO_ZERO_DATE SQL mode is active;
          - there is no explicit DEFAULT clause (default column value);
          - this is a TIMESTAMP column;
          - the column is not NULL;
          - this is not the DEFAULT CURRENT_TIMESTAMP column.

        In other words, an error should be reported if
          - NO_ZERO_DATE SQL mode is active;
          - the column definition is equivalent to
            'column_name TIMESTAMP DEFAULT 0'.
      */

      my_error(ER_INVALID_DEFAULT, MYF(0), sql_field->field_name.str);
      DBUG_RETURN(TRUE);
    }
    if (sql_field->invisible == INVISIBLE_USER &&
        sql_field->flags & NOT_NULL_FLAG &&
        sql_field->flags & NO_DEFAULT_VALUE_FLAG)
    {
      my_error(ER_INVISIBLE_NOT_NULL_WITHOUT_DEFAULT, MYF(0),
                          sql_field->field_name.str);
      DBUG_RETURN(TRUE);
    }
  }

  /* Check table level constraints */
  create_info->check_constraint_list= &alter_info->check_constraint_list;
  {
    List_iterator_fast<Virtual_column_info> c_it(alter_info->check_constraint_list);
    Virtual_column_info *check;
    while ((check= c_it++))
    {
      if (!check->name.length || check->automatic_name)
        continue;

      {
        /* Check that there's no repeating table CHECK constraint names. */
        List_iterator_fast<Virtual_column_info>
          dup_it(alter_info->check_constraint_list);
        const Virtual_column_info *dup_check;
        while ((dup_check= dup_it++) && dup_check != check)
        {
          if (!lex_string_cmp(system_charset_info,
                              &check->name, &dup_check->name))
          {
            my_error(ER_DUP_CONSTRAINT_NAME, MYF(0), "CHECK", check->name.str);
            DBUG_RETURN(TRUE);
          }
        }
      }

      /* Check that there's no repeating key constraint names. */
      List_iterator_fast<Key> key_it(alter_info->key_list);
      while (const Key *key= key_it++)
      {
        /*
          Not all keys considered to be the CONSTRAINT
          Noly Primary Key UNIQUE and Foreign keys.
        */
        if (key->type != Key::PRIMARY && key->type != Key::UNIQUE &&
            key->type != Key::FOREIGN_KEY)
          continue;

        if (check->name.length == key->name.length &&
            my_strcasecmp(system_charset_info,
              check->name.str, key->name.str) == 0)
        {
          my_error(ER_DUP_CONSTRAINT_NAME, MYF(0), "CHECK", check->name.str);
          DBUG_RETURN(TRUE);
        }
      }

      if (check_string_char_length(&check->name, 0, NAME_CHAR_LEN,
                                   system_charset_info, 1))
      {
        my_error(ER_TOO_LONG_IDENT, MYF(0), check->name.str);
        DBUG_RETURN(TRUE);
      }
      if (check_expression(check, &check->name, VCOL_CHECK_TABLE))
        DBUG_RETURN(TRUE);
    }
  }

  /* Give warnings for not supported table options */
  extern handlerton *maria_hton;
  if (file->partition_ht() != maria_hton && create_info->transactional &&
      !file->has_transaction_manager())
      push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                          ER_ILLEGAL_HA_CREATE_OPTION,
                          ER_THD(thd, ER_ILLEGAL_HA_CREATE_OPTION),
                          file->engine_name()->str,
                          create_info->transactional == HA_CHOICE_YES
                          ? "TRANSACTIONAL=1" : "TRANSACTIONAL=0");

  if (parse_option_list(thd, file->partition_ht(), &create_info->option_struct,
                          &create_info->option_list,
                          file->partition_ht()->table_options, FALSE,
                          thd->mem_root))
      DBUG_RETURN(TRUE);

  DBUG_RETURN(FALSE);
}

/**
  check comment length of table, column, index and partition

  If comment lenght is more than the standard length
  truncate it and store the comment lenght upto the standard
  comment length size

  @param          thd             Thread handle
  @param[in,out]  comment         Comment
  @param          max_len         Maximum allowed comment length
  @param          err_code        Error message
  @param          name            Name of commented object

  @return Operation status
    @retval       true            Error found
    @retval       false           On Success
*/
bool validate_comment_length(THD *thd, LEX_CSTRING *comment, size_t max_len,
                             uint err_code, const char *name)
{
  DBUG_ENTER("validate_comment_length");
  if (comment->length == 0)
    DBUG_RETURN(false);

  size_t tmp_len=
      Well_formed_prefix(system_charset_info, *comment, max_len).length();
  if (tmp_len < comment->length)
  {
#if MARIADB_VERSION_ID < 100500
    if (comment->length <= max_len)
    {
      if (thd->is_strict_mode())
      {
         my_error(ER_INVALID_CHARACTER_STRING, MYF(0),
                  system_charset_info->csname, comment->str);
         DBUG_RETURN(true);
      }
      push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                          ER_INVALID_CHARACTER_STRING,
                          ER_THD(thd, ER_INVALID_CHARACTER_STRING),
                          system_charset_info->csname, comment->str);
      comment->length= tmp_len;
      DBUG_RETURN(false);
    }
#else
#error do it in TEXT_STRING_sys
#endif
    if (thd->is_strict_mode())
    {
       my_error(err_code, MYF(0), name, static_cast<ulong>(max_len));
       DBUG_RETURN(true);
    }
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN, err_code,
                        ER_THD(thd, err_code), name,
                        static_cast<ulong>(max_len));
    comment->length= tmp_len;
  }
  DBUG_RETURN(false);
}


/*
  Set table default charset, if not set

  SYNOPSIS
    set_table_default_charset()
    create_info        Table create information

  DESCRIPTION
    If the table character set was not given explicitly,
    let's fetch the database default character set and
    apply it to the table.
*/

static void set_table_default_charset(THD *thd, HA_CREATE_INFO *create_info,
                                      const LEX_CSTRING &db)
{
  /*
    If the table character set was not given explicitly,
    let's fetch the database default character set and
    apply it to the table.
  */
  if (!create_info->default_table_charset)
  {
    Schema_specification_st db_info;

    load_db_opt_by_name(thd, db.str, &db_info);

    create_info->default_table_charset= db_info.default_table_charset;
  }
}


/*
  Extend long VARCHAR fields to blob & prepare field if it's a blob

  SYNOPSIS
    prepare_blob_field()

  RETURN
    0	ok
    1	Error (sql_field can't be converted to blob)
        In this case the error is given
*/

bool Column_definition::prepare_blob_field(THD *thd)
{
  DBUG_ENTER("Column_definition::prepare_blob_field");

  if (length > MAX_FIELD_VARCHARLENGTH && !(flags & BLOB_FLAG))
  {
    /* Convert long VARCHAR columns to TEXT or BLOB */
    char warn_buff[MYSQL_ERRMSG_SIZE];

    if (thd->is_strict_mode())
    {
      my_error(ER_TOO_BIG_FIELDLENGTH, MYF(0), field_name.str,
               static_cast<ulong>(MAX_FIELD_VARCHARLENGTH / charset->mbmaxlen));
      DBUG_RETURN(1);
    }
    set_handler(&type_handler_blob);
    flags|= BLOB_FLAG;
    my_snprintf(warn_buff, sizeof(warn_buff), ER_THD(thd, ER_AUTO_CONVERT),
                field_name.str,
                (charset == &my_charset_bin) ? "VARBINARY" : "VARCHAR",
                (charset == &my_charset_bin) ? "BLOB" : "TEXT");
    push_warning(thd, Sql_condition::WARN_LEVEL_NOTE, ER_AUTO_CONVERT,
                 warn_buff);
  }

  if ((flags & BLOB_FLAG) && length)
  {
    if (real_field_type() == FIELD_TYPE_BLOB ||
        real_field_type() == FIELD_TYPE_TINY_BLOB ||
        real_field_type() == FIELD_TYPE_MEDIUM_BLOB)
    {
      /* The user has given a length to the blob column */
      set_handler(Type_handler::blob_type_handler((uint) length));
      pack_length= type_handler()->calc_pack_length(0);
    }
    length= key_length= 0;
  }
  DBUG_RETURN(0);
}


/*
  Preparation of Create_field for SP function return values.
  Based on code used in the inner loop of mysql_prepare_create_table()
  above.

  SYNOPSIS
    sp_prepare_create_field()
    thd                 Thread object
    mem_root            Memory root to allocate components on (e.g. interval)

  DESCRIPTION
    Prepares the field structures for field creation.

*/

bool Column_definition::sp_prepare_create_field(THD *thd, MEM_ROOT *mem_root)
{
  return prepare_stage1(thd, mem_root, NULL, HA_CAN_GEOMETRY) ||
         prepare_stage2(NULL, HA_CAN_GEOMETRY);
}


static bool vers_prepare_keys(THD *thd, HA_CREATE_INFO *create_info,
                         Alter_info *alter_info, KEY **key_info, uint key_count)
{
  DBUG_ASSERT(create_info->versioned());

  const char *row_start_field= create_info->vers_info.as_row.start;
  DBUG_ASSERT(row_start_field);
  const char *row_end_field= create_info->vers_info.as_row.end;
  DBUG_ASSERT(row_end_field);

  List_iterator<Key> key_it(alter_info->key_list);
  Key *key= NULL;
  while ((key=key_it++))
  {
    if (key->type != Key::PRIMARY && key->type != Key::UNIQUE)
      continue;

    Key_part_spec *key_part= NULL;
    List_iterator<Key_part_spec> part_it(key->columns);
    while ((key_part=part_it++))
    {
      if (!my_strcasecmp(system_charset_info,
                         row_start_field,
                         key_part->field_name.str) ||

          !my_strcasecmp(system_charset_info,
                         row_end_field,
                         key_part->field_name.str))
        break;
    }
    if (key_part)
      continue; // Key already contains Sys_start or Sys_end

    Key_part_spec *row_end=
        new (thd->mem_root) Key_part_spec(&create_info->vers_info.as_row.end, 0,
                                          true);
    key->columns.push_back(row_end);
  }

  return false;
}

handler *mysql_create_frm_image(THD *thd, const LEX_CSTRING &db,
                                const LEX_CSTRING &table_name,
                                HA_CREATE_INFO *create_info,
                                Alter_info *alter_info, int create_table_mode,
                                KEY **key_info, uint *key_count,
                                LEX_CUSTRING *frm)
{
  uint		db_options;
  handler       *file;
  DBUG_ENTER("mysql_create_frm_image");

  if (!alter_info->create_list.elements)
  {
    my_error(ER_TABLE_MUST_HAVE_COLUMNS, MYF(0));
    DBUG_RETURN(NULL);
  }

  set_table_default_charset(thd, create_info, db);

  db_options= create_info->table_options_with_row_type();

  if (unlikely(!(file= get_new_handler((TABLE_SHARE*) 0, thd->mem_root,
                                       create_info->db_type))))
    DBUG_RETURN(NULL);

#ifdef WITH_PARTITION_STORAGE_ENGINE
  partition_info *part_info= thd->work_part_info;

  if (!part_info && create_info->db_type->partition_flags &&
      (create_info->db_type->partition_flags() & HA_USE_AUTO_PARTITION))
  {
    /*
      Table is not defined as a partitioned table but the engine handles
      all tables as partitioned. The handler will set up the partition info
      object with the default settings.
    */
    thd->work_part_info= part_info= new partition_info();
    if (unlikely(!part_info))
      goto err;

    file->set_auto_partitions(part_info);
    part_info->default_engine_type= create_info->db_type;
    part_info->is_auto_partitioned= TRUE;
  }
  if (part_info)
  {
    /*
      The table has been specified as a partitioned table.
      If this is part of an ALTER TABLE the handler will be the partition
      handler but we need to specify the default handler to use for
      partitions also in the call to check_partition_info. We transport
      this information in the default_db_type variable, it is either
      DB_TYPE_DEFAULT or the engine set in the ALTER TABLE command.
    */
    handlerton *part_engine_type= create_info->db_type;
    char *part_syntax_buf;
    uint syntax_len;
    handlerton *engine_type;
    List_iterator<partition_element> part_it(part_info->partitions);
    partition_element *part_elem;

    while ((part_elem= part_it++))
    {
      if (part_elem->part_comment)
      {
        LEX_CSTRING comment= { part_elem->part_comment,
                               strlen(part_elem->part_comment)
        };
        if (validate_comment_length(thd, &comment,
                                     TABLE_PARTITION_COMMENT_MAXLEN,
                                     ER_TOO_LONG_TABLE_PARTITION_COMMENT,
                                     part_elem->partition_name))
          DBUG_RETURN(NULL);
        /* cut comment length. Safe to do in all cases */
        ((char*)part_elem->part_comment)[comment.length]= '\0';
      }
      if (part_elem->subpartitions.elements)
      {
        List_iterator<partition_element> sub_it(part_elem->subpartitions);
        partition_element *subpart_elem;
        while ((subpart_elem= sub_it++))
        {
          if (subpart_elem->part_comment)
          {
            LEX_CSTRING comment= {
              subpart_elem->part_comment, strlen(subpart_elem->part_comment)
            };
            if (validate_comment_length(thd, &comment,
                                         TABLE_PARTITION_COMMENT_MAXLEN,
                                         ER_TOO_LONG_TABLE_PARTITION_COMMENT,
                                         subpart_elem->partition_name))
              DBUG_RETURN(NULL);
            /* cut comment length. Safe to do in all cases */
            ((char*)subpart_elem->part_comment)[comment.length]= '\0';
          }
        }
      }
    } 

    if (create_info->tmp_table())
    {
      my_error(ER_PARTITION_NO_TEMPORARY, MYF(0));
      goto err;
    }
    if ((part_engine_type == partition_hton) &&
        part_info->default_engine_type)
    {
      /*
        This only happens at ALTER TABLE.
        default_engine_type was assigned from the engine set in the ALTER
        TABLE command.
      */
      ;
    }
    else
    {
      if (create_info->used_fields & HA_CREATE_USED_ENGINE)
      {
        part_info->default_engine_type= create_info->db_type;
      }
      else
      {
        if (part_info->default_engine_type == NULL)
        {
          part_info->default_engine_type= ha_default_handlerton(thd);
        }
      }
    }
    DBUG_PRINT("info", ("db_type = %s create_info->db_type = %s",
             ha_resolve_storage_engine_name(part_info->default_engine_type),
             ha_resolve_storage_engine_name(create_info->db_type)));
    if (part_info->check_partition_info(thd, &engine_type, file,
                                        create_info, FALSE))
      goto err;
    part_info->default_engine_type= engine_type;

    if (part_info->vers_info && !create_info->versioned())
    {
      my_error(ER_VERS_NOT_VERSIONED, MYF(0), table_name.str);
      goto err;
    }

    /*
      We reverse the partitioning parser and generate a standard format
      for syntax stored in frm file.
    */
    part_syntax_buf= generate_partition_syntax_for_frm(thd, part_info,
                                      &syntax_len, create_info, alter_info);
    if (!part_syntax_buf)
      goto err;
    part_info->part_info_string= part_syntax_buf;
    part_info->part_info_len= syntax_len;
    if ((!(engine_type->partition_flags &&
           ((engine_type->partition_flags() & HA_CAN_PARTITION) ||
            (part_info->part_type == VERSIONING_PARTITION &&
            engine_type->partition_flags() & HA_ONLY_VERS_PARTITION))
          )) ||
        create_info->db_type == partition_hton)
    {
      /*
        The handler assigned to the table cannot handle partitioning.
        Assign the partition handler as the handler of the table.
      */
      DBUG_PRINT("info", ("db_type: %s",
                        ha_resolve_storage_engine_name(create_info->db_type)));
      delete file;
      create_info->db_type= partition_hton;
      if (!(file= get_ha_partition(part_info)))
        DBUG_RETURN(NULL);

      /*
        If we have default number of partitions or subpartitions we
        might require to set-up the part_info object such that it
        creates a proper .par file. The current part_info object is
        only used to create the frm-file and .par-file.
      */
      if (part_info->use_default_num_partitions &&
          part_info->num_parts &&
          (int)part_info->num_parts !=
          file->get_default_no_partitions(create_info))
      {
        uint i;
        List_iterator<partition_element> part_it(part_info->partitions);
        part_it++;
        DBUG_ASSERT(thd->lex->sql_command != SQLCOM_CREATE_TABLE);
        for (i= 1; i < part_info->partitions.elements; i++)
          (part_it++)->part_state= PART_TO_BE_DROPPED;
      }
      else if (part_info->is_sub_partitioned() &&
               part_info->use_default_num_subpartitions &&
               part_info->num_subparts &&
               (int)part_info->num_subparts !=
                 file->get_default_no_partitions(create_info))
      {
        DBUG_ASSERT(thd->lex->sql_command != SQLCOM_CREATE_TABLE);
        part_info->num_subparts= file->get_default_no_partitions(create_info);
      }
    }
    else if (create_info->db_type != engine_type)
    {
      /*
        We come here when we don't use a partitioned handler.
        Since we use a partitioned table it must be "native partitioned".
        We have switched engine from defaults, most likely only specified
        engines in partition clauses.
      */
      delete file;
      if (unlikely(!(file= get_new_handler((TABLE_SHARE*) 0, thd->mem_root,
                                           engine_type))))
        DBUG_RETURN(NULL);
    }
  }
  /*
    Unless table's storage engine supports partitioning natively
    don't allow foreign keys on partitioned tables (they won't
    work work even with InnoDB beneath of partitioning engine).
    If storage engine handles partitioning natively (like NDB)
    foreign keys support is possible, so we let the engine decide.
  */
  if (create_info->db_type == partition_hton)
  {
    List_iterator_fast<Key> key_iterator(alter_info->key_list);
    Key *key;
    while ((key= key_iterator++))
    {
      if (key->type == Key::FOREIGN_KEY)
      {
        my_error(ER_FOREIGN_KEY_ON_PARTITIONED, MYF(0));
        goto err;
      }
    }
  }
#endif

  if (create_info->versioned())
  {
    if(vers_prepare_keys(thd, create_info, alter_info, key_info,
                                *key_count))
      goto err;
  }

  if (mysql_prepare_create_table(thd, create_info, alter_info, &db_options,
                                 file, key_info, key_count, create_table_mode))
    goto err;
  create_info->table_options=db_options;

  *frm= build_frm_image(thd, table_name, create_info,
                        alter_info->create_list, *key_count,
                        *key_info, file);

  if (frm->str)
    DBUG_RETURN(file);

err:
  delete file;
  DBUG_RETURN(NULL);
}


/**
  Create a table

  @param thd                 Thread object
  @param orig_db             Database for error messages
  @param orig_table_name     Table name for error messages
                             (it's different from table_name for ALTER TABLE)
  @param db                  Database
  @param table_name          Table name
  @param path                Path to table (i.e. to its .FRM file without
                             the extension).
  @param create_info         Create information (like MAX_ROWS)
  @param alter_info          Description of fields and keys for new table
  @param create_table_mode   C_ORDINARY_CREATE, C_ALTER_TABLE, C_ASSISTED_DISCOVERY
                             or any positive number (for C_CREATE_SELECT).
  @param[out] is_trans       Identifies the type of engine where the table
                             was created: either trans or non-trans.
  @param[out] key_info       Array of KEY objects describing keys in table
                             which was created.
  @param[out] key_count      Number of keys in table which was created.

  If one creates a temporary table, its is automatically opened and its
  TABLE_SHARE is added to THD::all_temp_tables list.

  Note that this function assumes that caller already have taken
  exclusive metadata lock on table being created or used some other
  way to ensure that concurrent operations won't intervene.
  mysql_create_table() is a wrapper that can be used for this.

  @retval 0 OK
  @retval 1 error
  @retval -1 table existed but IF NOT EXISTS was used
*/

static
int create_table_impl(THD *thd, const LEX_CSTRING &orig_db,
                      const LEX_CSTRING &orig_table_name,
                      const LEX_CSTRING &db, const LEX_CSTRING &table_name,
                      const char *path, const DDL_options_st options,
                      HA_CREATE_INFO *create_info, Alter_info *alter_info,
                      int create_table_mode, bool *is_trans, KEY **key_info,
                      uint *key_count, LEX_CUSTRING *frm)
{
  LEX_CSTRING	*alias;
  handler	*file= 0;
  int		error= 1;
  bool          frm_only= create_table_mode == C_ALTER_TABLE_FRM_ONLY;
  bool          internal_tmp_table= create_table_mode == C_ALTER_TABLE || frm_only;
  DBUG_ENTER("mysql_create_table_no_lock");
  DBUG_PRINT("enter", ("db: '%s'  table: '%s'  tmp: %d  path: %s",
                       db.str, table_name.str, internal_tmp_table, path));

  if (fix_constraints_names(thd, &alter_info->check_constraint_list,
                            create_info))
    DBUG_RETURN(1);

  if (thd->variables.sql_mode & MODE_NO_DIR_IN_CREATE)
  {
    if (create_info->data_file_name)
      push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                          WARN_OPTION_IGNORED,
                          ER_THD(thd, WARN_OPTION_IGNORED),
                          "DATA DIRECTORY");
    if (create_info->index_file_name)
      push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                          WARN_OPTION_IGNORED,
                          ER_THD(thd, WARN_OPTION_IGNORED),
                          "INDEX DIRECTORY");
    create_info->data_file_name= create_info->index_file_name= 0;
  }
  else
  {
    if (unlikely(error_if_data_home_dir(create_info->data_file_name,
                                        "DATA DIRECTORY")) ||
        unlikely(error_if_data_home_dir(create_info->index_file_name,
                                        "INDEX DIRECTORY")) ||
        unlikely(check_partition_dirs(thd->lex->part_info)))
      goto err;
  }

  alias= const_cast<LEX_CSTRING*>(table_case_name(create_info, &table_name));

  /* Check if table exists */
  if (create_info->tmp_table())
  {
    /*
      If a table exists, it must have been pre-opened. Try looking for one
      in-use in THD::all_temp_tables list of TABLE_SHAREs.
    */
    TABLE *tmp_table= thd->find_temporary_table(db.str, table_name.str);

    if (tmp_table)
    {
      bool table_creation_was_logged= tmp_table->s->table_creation_was_logged;
      if (options.or_replace())
      {
        /*
          We are using CREATE OR REPLACE on an existing temporary table
          Remove the old table so that we can re-create it.
        */
        if (thd->drop_temporary_table(tmp_table, NULL, true))
          goto err;
      }
      else if (options.if_not_exists())
        goto warn;
      else
      {
        my_error(ER_TABLE_EXISTS_ERROR, MYF(0), alias->str);
        goto err;
      }
      /*
        We have to log this query, even if it failed later to ensure the
        drop is done.
      */
      if (table_creation_was_logged)
      {
        thd->variables.option_bits|= OPTION_KEEP_LOG;
        thd->log_current_statement= 1;
        create_info->table_was_deleted= 1;
      }
    }
  }
  else
  {
    if (!internal_tmp_table && ha_table_exists(thd, &db, &table_name))
    {
      if (options.or_replace())
      {
        (void) delete_statistics_for_table(thd, &db, &table_name);

        TABLE_LIST table_list;
        table_list.init_one_table(&db, &table_name, 0, TL_WRITE_ALLOW_WRITE);
        table_list.table= create_info->table;

        if (check_if_log_table(&table_list, TRUE, "CREATE OR REPLACE"))
          goto err;
        
        /*
          Rollback the empty transaction started in mysql_create_table()
          call to open_and_lock_tables() when we are using LOCK TABLES.
        */
        (void) trans_rollback_stmt(thd);
        /* Remove normal table without logging. Keep tables locked */
        if (mysql_rm_table_no_locks(thd, &table_list, 0, 0, 0, 0, 1, 1))
          goto err;

        /*
          We have to log this query, even if it failed later to ensure the
          drop is done.
        */
        thd->variables.option_bits|= OPTION_KEEP_LOG;
        thd->log_current_statement= 1;
        create_info->table_was_deleted= 1;
        DBUG_EXECUTE_IF("send_kill_after_delete", thd->set_killed(KILL_QUERY); );

        /*
          Restart statement transactions for the case of CREATE ... SELECT.
        */
        if (thd->lex->first_select_lex()->item_list.elements &&
            restart_trans_for_tables(thd, thd->lex->query_tables))
          goto err;
      }
      else if (options.if_not_exists())
        goto warn;
      else
      {
        my_error(ER_TABLE_EXISTS_ERROR, MYF(0), table_name.str);
        goto err;
      }
    }
  }

  THD_STAGE_INFO(thd, stage_creating_table);

  if (check_engine(thd, orig_db.str, orig_table_name.str, create_info))
    goto err;

  if (create_table_mode == C_ASSISTED_DISCOVERY)
  {
    /* check that it's used correctly */
    DBUG_ASSERT(alter_info->create_list.elements == 0);
    DBUG_ASSERT(alter_info->key_list.elements == 0);

    TABLE_SHARE share;
    handlerton *hton= create_info->db_type;
    int ha_err;
    Field *no_fields= 0;

    if (!hton->discover_table_structure)
    {
      my_error(ER_TABLE_MUST_HAVE_COLUMNS, MYF(0));
      goto err;
    }

    init_tmp_table_share(thd, &share, db.str, 0, table_name.str, path);

    /* prepare everything for discovery */
    share.field= &no_fields;
    share.db_plugin= ha_lock_engine(thd, hton);
    share.option_list= create_info->option_list;
    share.connect_string= create_info->connect_string;

    if (parse_engine_table_options(thd, hton, &share))
      goto err;

    ha_err= hton->discover_table_structure(hton, thd, &share, create_info);

    /*
      if discovery failed, the plugin will be auto-unlocked, as it
      was locked on the THD, see above.
      if discovery succeeded, the plugin was replaced by a globally
      locked plugin, that will be unlocked by free_table_share()
    */
    if (ha_err)
      share.db_plugin= 0; // will be auto-freed, locked above on the THD

    free_table_share(&share);

    if (ha_err)
    {
      my_error(ER_GET_ERRNO, MYF(0), ha_err, hton_name(hton)->str);
      goto err;
    }
  }
  else
  {
    file= mysql_create_frm_image(thd, orig_db, orig_table_name, create_info,
                                 alter_info, create_table_mode, key_info,
                                 key_count, frm);
    /*
    TODO: remove this check of thd->is_error() (now it intercept
    errors in some val_*() methoids and bring some single place to
    such error interception).
    */
    if (!file || thd->is_error())
      goto err;

    if (thd->variables.keep_files_on_create)
      create_info->options|= HA_CREATE_KEEP_FILES;

    if (file->ha_create_partitioning_metadata(path, NULL, CHF_CREATE_FLAG))
      goto err;

    if (!frm_only)
    {
      if (ha_create_table(thd, path, db.str, table_name.str, create_info, frm))
      {
        file->ha_create_partitioning_metadata(path, NULL, CHF_DELETE_FLAG);
        deletefrm(path);
        goto err;
      }
    }
  }

  create_info->table= 0;
  if (!frm_only && create_info->tmp_table())
  {
    TABLE *table= thd->create_and_open_tmp_table(frm, path, db.str,
                                                 table_name.str,
                                                 false);

    if (!table)
    {
      (void) thd->rm_temporary_table(create_info->db_type, path);
      goto err;
    }

    if (is_trans != NULL)
      *is_trans= table->file->has_transactions();

    thd->thread_specific_used= TRUE;
    create_info->table= table;                  // Store pointer to table
  }

  error= 0;
err:
  THD_STAGE_INFO(thd, stage_after_create);
  delete file;
  DBUG_PRINT("exit", ("return: %d", error));
  DBUG_RETURN(error);

warn:
  error= -1;
  push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE,
                      ER_TABLE_EXISTS_ERROR,
                      ER_THD(thd, ER_TABLE_EXISTS_ERROR),
                      alias->str);
  goto err;
}

/**
  Simple wrapper around create_table_impl() to be used
  in various version of CREATE TABLE statement.

  @result
    1 unspefied error
    2 error; Don't log create statement
    0 ok
    -1 Table was used with IF NOT EXISTS and table existed (warning, not error)
*/

int mysql_create_table_no_lock(THD *thd, const LEX_CSTRING *db,
                               const LEX_CSTRING *table_name,
                               Table_specification_st *create_info,
                               Alter_info *alter_info, bool *is_trans,
                               int create_table_mode, TABLE_LIST *table_list)
{
  KEY *not_used_1;
  uint not_used_2;
  int res;
  char path[FN_REFLEN + 1];
  LEX_CUSTRING frm= {0,0};

  if (create_info->tmp_table())
    build_tmptable_filename(thd, path, sizeof(path));
  else
  {
    int length;
    const LEX_CSTRING *alias= table_case_name(create_info, table_name);
    length= build_table_filename(path, sizeof(path) - 1, db->str, alias->str, "", 0);
    // Check if we hit FN_REFLEN bytes along with file extension.
    if (length+reg_ext_length > FN_REFLEN)
    {
      my_error(ER_IDENT_CAUSES_TOO_LONG_PATH, MYF(0), (int) sizeof(path)-1,
               path);
      return true;
    }
  }

  res= create_table_impl(thd, *db, *table_name, *db, *table_name, path,
                         *create_info, create_info,
                         alter_info, create_table_mode,
                         is_trans, &not_used_1, &not_used_2, &frm);
  my_free(const_cast<uchar*>(frm.str));

  if (!res && create_info->sequence)
  {
    /* Set create_info.table if temporary table */
    if (create_info->tmp_table())
      table_list->table= create_info->table;
    else
      table_list->table= 0;
    res= sequence_insert(thd, thd->lex, table_list);
    if (res)
    {
      DBUG_ASSERT(thd->is_error());
      /* Drop the table as it wasn't completely done */
      if (!mysql_rm_table_no_locks(thd, table_list, 1,
                                   create_info->tmp_table(),
                                   false, true /* Sequence*/,
                                   true /* Don't log_query */,
                                   true /* Don't free locks */ ))
      {
        /*
          From the user point of view, the table creation failed
          We return 2 to indicate that this statement doesn't have
          to be logged.
        */
        res= 2;
      }
    }
  }

  return res;
}

/**
  Implementation of SQLCOM_CREATE_TABLE.

  Take the metadata locks (including a shared lock on the affected
  schema) and create the table. Is written to be called from
  mysql_execute_command(), to which it delegates the common parts
  with other commands (i.e. implicit commit before and after,
  close of thread tables.
*/

bool mysql_create_table(THD *thd, TABLE_LIST *create_table,
                        Table_specification_st *create_info,
                        Alter_info *alter_info)
{
  bool is_trans= FALSE;
  bool result;
  int create_table_mode;
  TABLE_LIST *pos_in_locked_tables= 0;
  MDL_ticket *mdl_ticket= 0;
  DBUG_ENTER("mysql_create_table");

  DBUG_ASSERT(create_table == thd->lex->query_tables);

  /* Copy temporarily the statement flags to thd for lock_table_names() */
  uint save_thd_create_info_options= thd->lex->create_info.options;
  thd->lex->create_info.options|= create_info->options;

  /* Open or obtain an exclusive metadata lock on table being created  */
  result= open_and_lock_tables(thd, *create_info, create_table, FALSE, 0);

  thd->lex->create_info.options= save_thd_create_info_options;

  if (result)
  {
    /* is_error() may be 0 if table existed and we generated a warning */
    DBUG_RETURN(thd->is_error());
  }
  /* The following is needed only in case of lock tables */
  if ((create_info->table= create_table->table))
  {
    pos_in_locked_tables= create_info->table->pos_in_locked_tables;
    mdl_ticket= create_table->table->mdl_ticket;
  }
  
  /* Got lock. */
  DEBUG_SYNC(thd, "locked_table_name");

  if (alter_info->create_list.elements || alter_info->key_list.elements)
    create_table_mode= C_ORDINARY_CREATE;
  else
    create_table_mode= C_ASSISTED_DISCOVERY;

  if (!opt_explicit_defaults_for_timestamp)
    promote_first_timestamp_column(&alter_info->create_list);

  /* We can abort create table for any table type */
  thd->abort_on_warning= thd->is_strict_mode();

  if (mysql_create_table_no_lock(thd, &create_table->db,
                                 &create_table->table_name, create_info,
                                 alter_info,
                                 &is_trans, create_table_mode,
                                 create_table) > 0)
  {
    result= 1;
    goto err;
  }

  /*
    Check if we are doing CREATE OR REPLACE TABLE under LOCK TABLES
    on a non temporary table
  */
  if (thd->locked_tables_mode && pos_in_locked_tables &&
      create_info->or_replace())
  {
    DBUG_ASSERT(thd->variables.option_bits & OPTION_TABLE_LOCK);
    /*
      Add back the deleted table and re-created table as a locked table
      This should always work as we have a meta lock on the table.
     */
    thd->locked_tables_list.add_back_last_deleted_lock(pos_in_locked_tables);
    if (thd->locked_tables_list.reopen_tables(thd, false))
    {
      thd->locked_tables_list.unlink_all_closed_tables(thd, NULL, 0);
      result= 1;
      goto err;
    }
    else
    {
      TABLE *table= pos_in_locked_tables->table;
      table->mdl_ticket->downgrade_lock(MDL_SHARED_NO_READ_WRITE);
    }
  }

err:
  thd->abort_on_warning= 0;

  /* In RBR or readonly server we don't need to log CREATE TEMPORARY TABLE */
  if (!result && create_info->tmp_table() &&
      (thd->is_current_stmt_binlog_format_row() || (opt_readonly && !thd->slave_thread)))
  {
    /* Note that table->s->table_creation_was_logged is not set! */
    DBUG_RETURN(result);
  }

  if (create_info->tmp_table())
    thd->transaction.stmt.mark_created_temp_table();

  /* Write log if no error or if we already deleted a table */
  if (likely(!result) || thd->log_current_statement)
  {
    if (unlikely(result) && create_info->table_was_deleted &&
        pos_in_locked_tables)
    {
      /*
        Possible locked table was dropped. We should remove meta data locks
        associated with it and do UNLOCK_TABLES if no more locked tables.
      */
      thd->locked_tables_list.unlock_locked_table(thd, mdl_ticket);
    }
    else if (likely(!result) && create_info->table)
    {
      /*
        Remember that table creation was logged so that we know if
        we should log a delete of it.
        If create_info->table was not set, it's a normal table and
        table_creation_was_logged will be set when the share is created.
      */
      create_info->table->s->table_creation_was_logged= 1;
    }
    if (unlikely(write_bin_log(thd, result ? FALSE : TRUE, thd->query(),
                               thd->query_length(), is_trans)))
      result= 1;
  }
  DBUG_RETURN(result);
}


/*
** Give the key name after the first field with an optional '_#' after
   @returns
    0        if keyname does not exists
    [1..)    index + 1 of duplicate key name
**/

static int
check_if_keyname_exists(const char *name, KEY *start, KEY *end)
{
  uint i= 1;
  for (KEY *key=start; key != end ; key++, i++)
    if (!my_strcasecmp(system_charset_info, name, key->name.str))
      return i;
  return 0;
}

/**
 Returns 1 if field name exists otherwise 0
*/
static bool
check_if_field_name_exists(const char *name, List<Create_field> * fields)
{
  Create_field *fld;
  List_iterator<Create_field>it(*fields);
  while ((fld = it++))
  {
    if (!my_strcasecmp(system_charset_info, fld->field_name.str, name))
      return 1;
  }
  return 0;
}

static char *
make_unique_key_name(THD *thd, const char *field_name,KEY *start,KEY *end)
{
  char buff[MAX_FIELD_NAME],*buff_end;

  if (!check_if_keyname_exists(field_name,start,end) &&
      my_strcasecmp(system_charset_info,field_name,primary_key_name))
    return (char*) field_name;			// Use fieldname
  buff_end=strmake(buff,field_name, sizeof(buff)-4);

  /*
    Only 3 chars + '\0' left, so need to limit to 2 digit
    This is ok as we can't have more than 100 keys anyway
  */
  for (uint i=2 ; i< 100; i++)
  {
    *buff_end= '_';
    int10_to_str(i, buff_end+1, 10);
    if (!check_if_keyname_exists(buff,start,end))
      return thd->strdup(buff);
  }
  return (char*) "not_specified";		// Should never happen
}

/**
   Make an unique name for constraints without a name
*/

static bool make_unique_constraint_name(THD *thd, LEX_CSTRING *name,
                                        const char *own_name_base,
                                        List<Virtual_column_info> *vcol,
                                        uint *nr)
{
  char buff[MAX_FIELD_NAME], *end;
  List_iterator_fast<Virtual_column_info> it(*vcol);
  end=strmov(buff, own_name_base ? own_name_base : "CONSTRAINT_");
  for (int round= 0;; round++)
  {
    Virtual_column_info *check;
    char *real_end= end;
    if (round == 1 && own_name_base)
        *end++= '_';
    // if own_base_name provided, try it first
    if (round != 0 || !own_name_base)
      real_end= int10_to_str((*nr)++, end, 10);
    it.rewind();
    while ((check= it++))
    {
      if (check->name.str &&
          !my_strcasecmp(system_charset_info, buff, check->name.str))
        break;
    }
    if (!check)                                 // Found unique name
    {
      name->length= (size_t) (real_end - buff);
      name->str= thd->strmake(buff, name->length);
      return (name->str == NULL);
    }
  }
  return FALSE;
}

/**
  INVISIBLE_FULL are internally created. They are completely invisible
  to Alter command (Opposite of SYSTEM_INVISIBLE which throws an
  error when same name column is added by Alter). So in the case of when
  user added a same column name as of INVISIBLE_FULL , we change
  INVISIBLE_FULL column name.
*/
static const
char * make_unique_invisible_field_name(THD *thd, const char *field_name,
                        List<Create_field> *fields)
{
  if (!check_if_field_name_exists(field_name, fields))
    return field_name;
  char buff[MAX_FIELD_NAME], *buff_end;
  buff_end= strmake_buf(buff, field_name);
  if (buff_end - buff < 5)
    return NULL; // Should not happen

  for (uint i=1 ; i < 10000; i++)
  {
    char *real_end= int10_to_str(i, buff_end, 10);
    if (check_if_field_name_exists(buff, fields))
      continue;
    return (const char *)thd->strmake(buff, real_end - buff);
  }
  return NULL; //Should not happen
}

/****************************************************************************
** Alter a table definition
****************************************************************************/

bool operator!=(const MYSQL_TIME &lhs, const MYSQL_TIME &rhs)
{
  return lhs.year != rhs.year || lhs.month != rhs.month || lhs.day != rhs.day ||
         lhs.hour != rhs.hour || lhs.minute != rhs.minute ||
         lhs.second_part != rhs.second_part || lhs.neg != rhs.neg ||
         lhs.time_type != rhs.time_type;
}

/**
  Rename a table.

  @param base      The handlerton handle.
  @param old_db    The old database name.
  @param old_name  The old table name.
  @param new_db    The new database name.
  @param new_name  The new table name.
  @param flags     flags
                   FN_FROM_IS_TMP old_name is temporary.
                   FN_TO_IS_TMP   new_name is temporary.
                   NO_FRM_RENAME  Don't rename the FRM file
                                  but only the table in the storage engine.
                   NO_HA_TABLE    Don't rename table in engine.
                   NO_FK_CHECKS   Don't check FK constraints during rename.

  @return false    OK
  @return true     Error
*/

bool
mysql_rename_table(handlerton *base, const LEX_CSTRING *old_db,
                   const LEX_CSTRING *old_name, const LEX_CSTRING *new_db,
                   const LEX_CSTRING *new_name, uint flags)
{
  THD *thd= current_thd;
  char from[FN_REFLEN + 1], to[FN_REFLEN + 1],
    lc_from[FN_REFLEN + 1], lc_to[FN_REFLEN + 1];
  char *from_base= from, *to_base= to;
  char tmp_name[SAFE_NAME_LEN+1], tmp_db_name[SAFE_NAME_LEN+1];
  handler *file;
  int error=0;
  ulonglong save_bits= thd->variables.option_bits;
  int length;
  DBUG_ENTER("mysql_rename_table");
  DBUG_ASSERT(base);
  DBUG_PRINT("enter", ("old: '%s'.'%s'  new: '%s'.'%s'",
                       old_db->str, old_name->str, new_db->str, new_name->str));

  // Temporarily disable foreign key checks
  if (flags & NO_FK_CHECKS) 
    thd->variables.option_bits|= OPTION_NO_FOREIGN_KEY_CHECKS;

  file= get_new_handler((TABLE_SHARE*) 0, thd->mem_root, base);

  build_table_filename(from, sizeof(from) - 1, old_db->str, old_name->str, "",
                       flags & FN_FROM_IS_TMP);
  length= build_table_filename(to, sizeof(to) - 1, new_db->str, new_name->str, "",
                               flags & FN_TO_IS_TMP);
  // Check if we hit FN_REFLEN bytes along with file extension.
  if (length+reg_ext_length > FN_REFLEN)
  {
    my_error(ER_IDENT_CAUSES_TOO_LONG_PATH, MYF(0), (int) sizeof(to)-1, to);
    DBUG_RETURN(TRUE);
  }

  /*
    If lower_case_table_names == 2 (case-preserving but case-insensitive
    file system) and the storage is not HA_FILE_BASED, we need to provide
    a lowercase file name, but we leave the .frm in mixed case.
   */
  if (lower_case_table_names == 2 && file &&
      !(file->ha_table_flags() & HA_FILE_BASED))
  {
    strmov(tmp_name, old_name->str);
    my_casedn_str(files_charset_info, tmp_name);
    strmov(tmp_db_name, old_db->str);
    my_casedn_str(files_charset_info, tmp_db_name);

    build_table_filename(lc_from, sizeof(lc_from) - 1, tmp_db_name, tmp_name,
                         "", flags & FN_FROM_IS_TMP);
    from_base= lc_from;

    strmov(tmp_name, new_name->str);
    my_casedn_str(files_charset_info, tmp_name);
    strmov(tmp_db_name, new_db->str);
    my_casedn_str(files_charset_info, tmp_db_name);

    build_table_filename(lc_to, sizeof(lc_to) - 1, tmp_db_name, tmp_name, "",
                         flags & FN_TO_IS_TMP);
    to_base= lc_to;
  }

  if (flags & NO_HA_TABLE)
  {
    if (rename_file_ext(from,to,reg_ext))
      error= my_errno;
    (void) file->ha_create_partitioning_metadata(to, from, CHF_RENAME_FLAG);
  }
  else if (!file || likely(!(error=file->ha_rename_table(from_base, to_base))))
  {
    if (!(flags & NO_FRM_RENAME) && unlikely(rename_file_ext(from,to,reg_ext)))
    {
      error=my_errno;
      if (file)
      {
        if (error == ENOENT)
          error= 0; // this is ok if file->ha_rename_table() succeeded
        else
          file->ha_rename_table(to_base, from_base); // Restore old file name
      }
    }
  }
  delete file;

  if (error == HA_ERR_WRONG_COMMAND)
    my_error(ER_NOT_SUPPORTED_YET, MYF(0), "ALTER TABLE");
  else if (error ==  ENOTDIR)
    my_error(ER_BAD_DB_ERROR, MYF(0), new_db->str);
  else if (error)
    my_error(ER_ERROR_ON_RENAME, MYF(0), from, to, error);

  else if (!(flags & FN_IS_TMP))
    mysql_audit_rename_table(thd, old_db, old_name, new_db, new_name);

  /*
    Remove the old table share from the pfs table share array. The new table
    share will be created when the renamed table is first accessed.
   */
  if (likely(error == 0))
  {
    PSI_CALL_drop_table_share(flags & FN_FROM_IS_TMP,
                              old_db->str, (uint)old_db->length,
                              old_name->str, (uint)old_name->length);
  }

  // Restore options bits to the original value
  thd->variables.option_bits= save_bits;

  DBUG_RETURN(error != 0);
}


/*
  Create a table identical to the specified table

  SYNOPSIS
    mysql_create_like_table()
    thd		Thread object
    table       Table list element for target table
    src_table   Table list element for source table
    create_info Create info

  RETURN VALUES
    FALSE OK
    TRUE  error
*/

bool mysql_create_like_table(THD* thd, TABLE_LIST* table,
                             TABLE_LIST* src_table,
                             Table_specification_st *create_info)
{
  Table_specification_st local_create_info;
  TABLE_LIST *pos_in_locked_tables= 0;
  Alter_info local_alter_info;
  Alter_table_ctx local_alter_ctx; // Not used
  int res= 1;
  bool is_trans= FALSE;
  bool do_logging= FALSE;
  uint not_used;
  int create_res;
  DBUG_ENTER("mysql_create_like_table");

#ifdef WITH_WSREP
  if (WSREP(thd) && !thd->wsrep_applier &&
      wsrep_create_like_table(thd, table, src_table, create_info))
  {
    DBUG_RETURN(res);
  }
#endif

  /*
    We the open source table to get its description in HA_CREATE_INFO
    and Alter_info objects. This also acquires a shared metadata lock
    on this table which ensures that no concurrent DDL operation will
    mess with it.
    Also in case when we create non-temporary table open_tables()
    call obtains an exclusive metadata lock on target table ensuring
    that we can safely perform table creation.
    Thus by holding both these locks we ensure that our statement is
    properly isolated from all concurrent operations which matter.
  */

  res= open_tables(thd, *create_info, &thd->lex->query_tables, &not_used, 0);

  if (res)
  {
    /* is_error() may be 0 if table existed and we generated a warning */
    res= thd->is_error();
    goto err;
  }
  /* Ensure we don't try to create something from which we select from */
  if (create_info->or_replace() && !create_info->tmp_table())
  {
    TABLE_LIST *duplicate;
    if ((duplicate= unique_table(thd, table, src_table, 0)))
    {
      update_non_unique_table_error(src_table, "CREATE", duplicate);
      goto err;
    }
  }

  src_table->table->use_all_columns();

  DEBUG_SYNC(thd, "create_table_like_after_open");

  /*
    Fill Table_specification_st and Alter_info with the source table description.
    Set OR REPLACE and IF NOT EXISTS option as in the CREATE TABLE LIKE
    statement.
  */
  local_create_info.init(create_info->create_like_options());
  local_create_info.db_type= src_table->table->s->db_type();
  local_create_info.row_type= src_table->table->s->row_type;
  if (mysql_prepare_alter_table(thd, src_table->table, &local_create_info,
                                &local_alter_info, &local_alter_ctx))
    goto err;
#ifdef WITH_PARTITION_STORAGE_ENGINE
  /* Partition info is not handled by mysql_prepare_alter_table() call. */
  if (src_table->table->part_info)
    thd->work_part_info= src_table->table->part_info->get_clone(thd);
#endif

  /*
    Adjust description of source table before using it for creation of
    target table.

    Similarly to SHOW CREATE TABLE we ignore MAX_ROWS attribute of
    temporary table which represents I_S table.
  */
  if (src_table->schema_table)
    local_create_info.max_rows= 0;
  /* Replace type of source table with one specified in the statement. */
  local_create_info.options&= ~HA_LEX_CREATE_TMP_TABLE;
  local_create_info.options|= create_info->options;
  /* Reset auto-increment counter for the new table. */
  local_create_info.auto_increment_value= 0;
  /*
    Do not inherit values of DATA and INDEX DIRECTORY options from
    the original table. This is documented behavior.
  */
  local_create_info.data_file_name= local_create_info.index_file_name= NULL;

  if (src_table->table->versioned() &&
      local_create_info.vers_info.fix_create_like(local_alter_info, local_create_info,
                                                  *src_table, *table))
  {
    goto err;
  }

  /* The following is needed only in case of lock tables */
  if ((local_create_info.table= thd->lex->query_tables->table))
    pos_in_locked_tables= local_create_info.table->pos_in_locked_tables;    

  res= ((create_res=
         mysql_create_table_no_lock(thd, &table->db, &table->table_name,
                                    &local_create_info, &local_alter_info,
                                    &is_trans, C_ORDINARY_CREATE,
                                    table)) > 0);
  /* Remember to log if we deleted something */
  do_logging= thd->log_current_statement;
  if (res)
    goto err;

  /*
    Check if we are doing CREATE OR REPLACE TABLE under LOCK TABLES
    on a non temporary table
  */
  if (thd->locked_tables_mode && pos_in_locked_tables &&
      create_info->or_replace())
  {
    /*
      Add back the deleted table and re-created table as a locked table
      This should always work as we have a meta lock on the table.
     */
    thd->locked_tables_list.add_back_last_deleted_lock(pos_in_locked_tables);
    if (thd->locked_tables_list.reopen_tables(thd, false))
    {
      thd->locked_tables_list.unlink_all_closed_tables(thd, NULL, 0);
      res= 1;                                   // We got an error
    }
    else
    {
      /*
        Get pointer to the newly opened table. We need this to ensure we
        don't reopen the table when doing statment logging below.
      */
      table->table= pos_in_locked_tables->table;
      table->table->mdl_ticket->downgrade_lock(MDL_SHARED_NO_READ_WRITE);
    }
  }
  else
  {
    /*
      Ensure that we have an exclusive lock on target table if we are creating
      non-temporary table.
    */
    DBUG_ASSERT((create_info->tmp_table()) ||
                thd->mdl_context.is_lock_owner(MDL_key::TABLE, table->db.str,
                                               table->table_name.str,
                                               MDL_EXCLUSIVE));
  }

  DEBUG_SYNC(thd, "create_table_like_before_binlog");

  /*
    We have to write the query before we unlock the tables.
  */
  if (thd->is_current_stmt_binlog_disabled())
    goto err;

  if (thd->is_current_stmt_binlog_format_row())
  {
    /*
       Since temporary tables are not replicated under row-based
       replication, CREATE TABLE ... LIKE ... needs special
       treatement.  We have four cases to consider, according to the
       following decision table:

           ==== ========= ========= ==============================
           Case    Target    Source Write to binary log
           ==== ========= ========= ==============================
           1       normal    normal Original statement
           2       normal temporary Generated statement if the table
                                    was created.
           3    temporary    normal Nothing
           4    temporary temporary Nothing
           ==== ========= ========= ==============================
    */
    if (!(create_info->tmp_table()))
    {
      if (src_table->table->s->tmp_table)               // Case 2
      {
        char buf[2048];
        String query(buf, sizeof(buf), system_charset_info);
        query.length(0);  // Have to zero it since constructor doesn't
        Open_table_context ot_ctx(thd, MYSQL_OPEN_REOPEN |
                                  MYSQL_OPEN_IGNORE_KILLED);
        bool new_table= FALSE; // Whether newly created table is open.

        if (create_res != 0)
        {
          /*
            Table or view with same name already existed and we where using
            IF EXISTS. Continue without logging anything.
          */
          do_logging= 0;
          goto err;
        }
        if (!table->table)
        {
          TABLE_LIST::enum_open_strategy save_open_strategy;
          int open_res;
          /* Force the newly created table to be opened */
          save_open_strategy= table->open_strategy;
          table->open_strategy= TABLE_LIST::OPEN_NORMAL;

          /*
            In order for show_create_table() to work we need to open
            destination table if it is not already open (i.e. if it
            has not existed before). We don't need acquire metadata
            lock in order to do this as we already hold exclusive
            lock on this table. The table will be closed by
            close_thread_table() at the end of this branch.
          */
          open_res= open_table(thd, table, &ot_ctx);
          /* Restore */
          table->open_strategy= save_open_strategy;
          if (open_res)
          {
            res= 1;
            goto err;
          }
          new_table= TRUE;
        }
        /*
          We have to re-test if the table was a view as the view may not
          have been opened until just above.
        */
        if (!table->view)
        {
          /*
            After opening a MERGE table add the children to the query list of
            tables, so that children tables info can be used on "CREATE TABLE"
            statement generation by the binary log.
            Note that placeholders don't have the handler open.
          */
          if (table->table->file->extra(HA_EXTRA_ADD_CHILDREN_LIST))
            goto err;

          /*
            As the reference table is temporary and may not exist on slave, we must
            force the ENGINE to be present into CREATE TABLE.
          */
          create_info->used_fields|= HA_CREATE_USED_ENGINE;

          int result __attribute__((unused))=
            show_create_table(thd, table, &query, create_info, WITH_DB_NAME);

          DBUG_ASSERT(result == 0); // show_create_table() always return 0
          do_logging= FALSE;
          if (write_bin_log(thd, TRUE, query.ptr(), query.length()))
          {
            res= 1;
            do_logging= 0;
            goto err;
          }

          if (new_table)
          {
            DBUG_ASSERT(thd->open_tables == table->table);
            /*
              When opening the table, we ignored the locked tables
              (MYSQL_OPEN_GET_NEW_TABLE). Now we can close the table
              without risking to close some locked table.
            */
            close_thread_table(thd, &thd->open_tables);
          }
        }
      }
      else                                      // Case 1
        do_logging= TRUE;
    }
    /*
      Case 3 and 4 does nothing under RBR
    */
  }
  else
  {
    DBUG_PRINT("info",
               ("res: %d  tmp_table: %d  create_info->table: %p",
                res, create_info->tmp_table(), local_create_info.table));
    if (create_info->tmp_table())
    {
      thd->transaction.stmt.mark_created_temp_table();
      if (!res && local_create_info.table)
      {
        /*
          Remember that tmp table creation was logged so that we know if
          we should log a delete of it.
        */
        local_create_info.table->s->table_creation_was_logged= 1;
      }
    }
    do_logging= TRUE;
  }

err:
  if (do_logging)
  {
    if (res && create_info->table_was_deleted)
    {
      /*
        Table was not deleted. Original table was deleted.
        We have to log it.
      */
      log_drop_table(thd, &table->db, &table->table_name, create_info->tmp_table());
    }
    else if (res != 2)                         // Table was not dropped
    {
      if (write_bin_log(thd, res ? FALSE : TRUE, thd->query(),
                        thd->query_length(), is_trans))
      res= 1;
    }
  }

  DBUG_RETURN(res != 0);
}


/* table_list should contain just one table */
int mysql_discard_or_import_tablespace(THD *thd,
                                       TABLE_LIST *table_list,
                                       bool discard)
{
  Alter_table_prelocking_strategy alter_prelocking_strategy;
  int error;
  DBUG_ENTER("mysql_discard_or_import_tablespace");

  mysql_audit_alter_table(thd, table_list);

  /*
    Note that DISCARD/IMPORT TABLESPACE always is the only operation in an
    ALTER TABLE
  */

  THD_STAGE_INFO(thd, stage_discard_or_import_tablespace);

 /*
   We set this flag so that ha_innobase::open and ::external_lock() do
   not complain when we lock the table
 */
  thd->tablespace_op= TRUE;
  /*
    Adjust values of table-level and metadata which was set in parser
    for the case general ALTER TABLE.
  */
  table_list->mdl_request.set_type(MDL_EXCLUSIVE);
  table_list->lock_type= TL_WRITE;
  /* Do not open views. */
  table_list->required_type= TABLE_TYPE_NORMAL;

  if (open_and_lock_tables(thd, table_list, FALSE, 0,
                           &alter_prelocking_strategy))
  {
    thd->tablespace_op=FALSE;
    DBUG_RETURN(-1);
  }

  error= table_list->table->file->ha_discard_or_import_tablespace(discard);

  THD_STAGE_INFO(thd, stage_end);

  if (unlikely(error))
    goto err;

  /*
    The 0 in the call below means 'not in a transaction', which means
    immediate invalidation; that is probably what we wish here
  */
  query_cache_invalidate3(thd, table_list, 0);

  /* The ALTER TABLE is always in its own transaction */
  error= trans_commit_stmt(thd);
  if (unlikely(trans_commit_implicit(thd)))
    error=1;
  if (likely(!error))
    error= write_bin_log(thd, FALSE, thd->query(), thd->query_length());

err:
  thd->tablespace_op=FALSE;

  if (likely(error == 0))
  {
    my_ok(thd);
    DBUG_RETURN(0);
  }

  table_list->table->file->print_error(error, MYF(0));

  DBUG_RETURN(-1);
}


/**
  Check if key is a candidate key, i.e. a unique index with no index
  fields partial or nullable.
*/

static bool is_candidate_key(KEY *key)
{
  KEY_PART_INFO *key_part;
  KEY_PART_INFO *key_part_end= key->key_part + key->user_defined_key_parts;

  if (!(key->flags & HA_NOSAME) || (key->flags & HA_NULL_PART_KEY) ||
      (key->flags & HA_KEY_HAS_PART_KEY_SEG))
    return false;

  for (key_part= key->key_part; key_part < key_part_end; key_part++)
  {
    if (key_part->key_part_flag & HA_PART_KEY_SEG)
      return false;
  }
  return true;
}


/*
   Preparation for table creation

   SYNOPSIS
     handle_if_exists_option()
       thd                       Thread object.
       table                     The altered table.
       alter_info                List of columns and indexes to create
       period_info               Application-time period info

   DESCRIPTION
     Looks for the IF [NOT] EXISTS options, checks the states and remove items
     from the list if existing found.

   RETURN VALUES
     TRUE error
     FALSE OK
*/

static bool
handle_if_exists_options(THD *thd, TABLE *table, Alter_info *alter_info,
                         Table_period_info *period_info)
{
  Field **f_ptr;
  DBUG_ENTER("handle_if_exists_option");

  /* Handle ADD COLUMN IF NOT EXISTS. */
  {
    List_iterator<Create_field> it(alter_info->create_list);
    Create_field *sql_field;

    while ((sql_field=it++))
    {
      if (!sql_field->create_if_not_exists || sql_field->change.str)
        continue;
      /*
         If there is a field with the same name in the table already,
         remove the sql_field from the list.
      */
      for (f_ptr=table->field; *f_ptr; f_ptr++)
      {
        if (lex_string_cmp(system_charset_info,
                           &sql_field->field_name,
                           &(*f_ptr)->field_name) == 0)
          goto drop_create_field;
      }
      {
        /*
          If in the ADD list there is a field with the same name,
          remove the sql_field from the list.
        */
        List_iterator<Create_field> chk_it(alter_info->create_list);
        Create_field *chk_field;
        while ((chk_field= chk_it++) && chk_field != sql_field)
        {
          if (lex_string_cmp(system_charset_info,
                             &sql_field->field_name,
                             &chk_field->field_name) == 0)
            goto drop_create_field;
        }
      }
      continue;
drop_create_field:
      push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE,
                          ER_DUP_FIELDNAME, ER_THD(thd, ER_DUP_FIELDNAME),
                          sql_field->field_name.str);
      it.remove();
      if (alter_info->create_list.is_empty())
      {
        alter_info->flags&= ~ALTER_PARSER_ADD_COLUMN;
        if (alter_info->key_list.is_empty())
          alter_info->flags&= ~(ALTER_ADD_INDEX | ALTER_ADD_FOREIGN_KEY);
      }
    }
  }

  /* Handle MODIFY COLUMN IF EXISTS. */
  {
    List_iterator<Create_field> it(alter_info->create_list);
    Create_field *sql_field;

    while ((sql_field=it++))
    {
      if (!sql_field->create_if_not_exists || !sql_field->change.str)
        continue;
      /*
         If there is NO field with the same name in the table already,
         remove the sql_field from the list.
      */
      for (f_ptr=table->field; *f_ptr; f_ptr++)
      {
        if (lex_string_cmp(system_charset_info,
                           &sql_field->change,
                           &(*f_ptr)->field_name) == 0)
        {
          break;
        }
      }
      if (unlikely(*f_ptr == NULL))
      {
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE,
                            ER_BAD_FIELD_ERROR,
                            ER_THD(thd, ER_BAD_FIELD_ERROR),
                            sql_field->change.str, table->s->table_name.str);
        it.remove();
        if (alter_info->create_list.is_empty())
        {
          alter_info->flags&= ~(ALTER_PARSER_ADD_COLUMN | ALTER_CHANGE_COLUMN);
          if (alter_info->key_list.is_empty())
            alter_info->flags&= ~ALTER_ADD_INDEX;
        }
      }
    }
  }

  /* Handle ALTER COLUMN IF EXISTS SET/DROP DEFAULT. */
  {
    List_iterator<Alter_column> it(alter_info->alter_list);
    Alter_column *acol;

    while ((acol=it++))
    {
      if (!acol->alter_if_exists)
        continue;
      /*
         If there is NO field with the same name in the table already,
         remove the acol from the list.
      */
      for (f_ptr=table->field; *f_ptr; f_ptr++)
      {
        if (my_strcasecmp(system_charset_info,
                           acol->name, (*f_ptr)->field_name.str) == 0)
          break;
      }
      if (unlikely(*f_ptr == NULL))
      {
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE,
                            ER_BAD_FIELD_ERROR,
                            ER_THD(thd, ER_BAD_FIELD_ERROR),
                            acol->name, table->s->table_name.str);
        it.remove();
        if (alter_info->alter_list.is_empty())
        {
          alter_info->flags&= ~(ALTER_CHANGE_COLUMN_DEFAULT);
        }
      }
    }
  }

  /* Handle DROP COLUMN/KEY IF EXISTS. */
  {
    List_iterator<Alter_drop> drop_it(alter_info->drop_list);
    Alter_drop *drop;
    bool remove_drop;
    ulonglong left_flags= 0;
    while ((drop= drop_it++))
    {
      ulonglong cur_flag= 0;
      switch (drop->type) {
      case Alter_drop::COLUMN:
        cur_flag= ALTER_PARSER_DROP_COLUMN;
        break;
      case Alter_drop::FOREIGN_KEY:
        cur_flag= ALTER_DROP_FOREIGN_KEY;
        break;
      case Alter_drop::KEY:
        cur_flag= ALTER_DROP_INDEX;
        break;
      default:
        break;
      }
      if (!drop->drop_if_exists)
      {
        left_flags|= cur_flag;
        continue;
      }
      remove_drop= TRUE;
      if (drop->type == Alter_drop::COLUMN)
      {
        /*
           If there is NO field with that name in the table,
           remove the 'drop' from the list.
        */
        for (f_ptr=table->field; *f_ptr; f_ptr++)
        {
          if (my_strcasecmp(system_charset_info,
                            drop->name, (*f_ptr)->field_name.str) == 0)
          {
            remove_drop= FALSE;
            break;
          }
        }
      }
      else if (drop->type == Alter_drop::CHECK_CONSTRAINT)
      {
        for (uint i=table->s->field_check_constraints;
             i < table->s->table_check_constraints;
             i++)
        {
          if (my_strcasecmp(system_charset_info, drop->name,
                            table->check_constraints[i]->name.str) == 0)
          {
            remove_drop= FALSE;
            break;
          }
        }
      }
      else if (drop->type == Alter_drop::PERIOD)
      {
        if (table->s->period.name.streq(drop->name))
          remove_drop= FALSE;
      }
      else /* Alter_drop::KEY and Alter_drop::FOREIGN_KEY */
      {
        uint n_key;
        if (drop->type != Alter_drop::FOREIGN_KEY)
        {
          for (n_key=0; n_key < table->s->keys; n_key++)
          {
            if (my_strcasecmp(system_charset_info,
                              drop->name,
                              table->key_info[n_key].name.str) == 0)
            {
              remove_drop= FALSE;
              break;
            }
          }
        }
        else
        {
          List <FOREIGN_KEY_INFO> fk_child_key_list;
          FOREIGN_KEY_INFO *f_key;
          table->file->get_foreign_key_list(thd, &fk_child_key_list);
          List_iterator<FOREIGN_KEY_INFO> fk_key_it(fk_child_key_list);
          while ((f_key= fk_key_it++))
          {
            if (my_strcasecmp(system_charset_info, f_key->foreign_id->str,
                  drop->name) == 0)
            {
              remove_drop= FALSE;
              break;
            }
          }
        }
      }

      if (!remove_drop)
      {
        /*
          Check if the name appears twice in the DROP list.
        */
        List_iterator<Alter_drop> chk_it(alter_info->drop_list);
        Alter_drop *chk_drop;
        while ((chk_drop= chk_it++) && chk_drop != drop)
        {
          if (drop->type == chk_drop->type &&
              my_strcasecmp(system_charset_info,
                            drop->name, chk_drop->name) == 0)
          {
            remove_drop= TRUE;
            break;
          }
        }
      }

      if (remove_drop)
      {
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE,
                            ER_CANT_DROP_FIELD_OR_KEY,
                            ER_THD(thd, ER_CANT_DROP_FIELD_OR_KEY),
                            drop->type_name(), drop->name);
        drop_it.remove();
      }
      else
        left_flags|= cur_flag;
    }
    /* Reset state to what's left in drop list */
    alter_info->flags&= ~(ALTER_PARSER_DROP_COLUMN |
                          ALTER_DROP_INDEX |
                          ALTER_DROP_FOREIGN_KEY);
    alter_info->flags|= left_flags;
  }

  /* ALTER TABLE ADD KEY IF NOT EXISTS */
  /* ALTER TABLE ADD FOREIGN KEY IF NOT EXISTS */
  {
    Key *key;
    List_iterator<Key> key_it(alter_info->key_list);
    uint n_key;
    const char *keyname= NULL;
    while ((key=key_it++))
    {
      if (!key->if_not_exists() && !key->or_replace())
        continue;

      /* Check if the table already has a PRIMARY KEY */
      bool dup_primary_key=
            key->type == Key::PRIMARY &&
            table->s->primary_key != MAX_KEY &&
            (keyname= table->s->key_info[table->s->primary_key].name.str) &&
            my_strcasecmp(system_charset_info, keyname, primary_key_name) == 0;
      if (dup_primary_key)
        goto remove_key;

      /* If the name of the key is not specified,     */
      /* let us check the name of the first key part. */
      if ((keyname= key->name.str) == NULL)
      {
        if (key->type == Key::PRIMARY)
          keyname= primary_key_name;
        else
        {
          List_iterator<Key_part_spec> part_it(key->columns);
          Key_part_spec *kp;
          if ((kp= part_it++))
            keyname= kp->field_name.str;
          if (keyname == NULL)
            continue;
        }
      }
      if (key->type != Key::FOREIGN_KEY)
      {
        for (n_key=0; n_key < table->s->keys; n_key++)
        {
          if (my_strcasecmp(system_charset_info,
                keyname, table->key_info[n_key].name.str) == 0)
          {
            goto remove_key;
          }
        }
      }
      else
      {
        List <FOREIGN_KEY_INFO> fk_child_key_list;
        FOREIGN_KEY_INFO *f_key;
        table->file->get_foreign_key_list(thd, &fk_child_key_list);
        List_iterator<FOREIGN_KEY_INFO> fk_key_it(fk_child_key_list);
        while ((f_key= fk_key_it++))
        {
          if (my_strcasecmp(system_charset_info, f_key->foreign_id->str,
                keyname) == 0)
            goto remove_key;
        }
      }

      {
        Key *chk_key;
        List_iterator<Key> chk_it(alter_info->key_list);
        const char *chkname;
        while ((chk_key=chk_it++) && chk_key != key)
        {
          if ((chkname= chk_key->name.str) == NULL)
          {
            List_iterator<Key_part_spec> part_it(chk_key->columns);
            Key_part_spec *kp;
            if ((kp= part_it++))
              chkname= kp->field_name.str;
            if (chkname == NULL)
              continue;
          }
          if (key->type == chk_key->type &&
              my_strcasecmp(system_charset_info, keyname, chkname) == 0)
            goto remove_key;
        }
      }
      continue;

remove_key:
      if (key->if_not_exists())
      {
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE,
                            ER_DUP_KEYNAME, ER_THD(thd, dup_primary_key
                            ? ER_MULTIPLE_PRI_KEY : ER_DUP_KEYNAME), keyname);
        key_it.remove();
        if (key->type == Key::FOREIGN_KEY)
        {
          /* ADD FOREIGN KEY appends two items. */
          key_it.remove();
        }
        if (alter_info->key_list.is_empty())
          alter_info->flags&= ~(ALTER_ADD_INDEX | ALTER_ADD_FOREIGN_KEY);
      }
      else
      {
        DBUG_ASSERT(key->or_replace());
        Alter_drop::drop_type type= (key->type == Key::FOREIGN_KEY) ?
          Alter_drop::FOREIGN_KEY : Alter_drop::KEY;
        Alter_drop *ad= new Alter_drop(type, key->name.str, FALSE);
        if (ad != NULL)
        {
          // Adding the index into the drop list for replacing
          alter_info->flags |= ALTER_DROP_INDEX;
          alter_info->drop_list.push_back(ad, thd->mem_root);
        }
      }
    }
  }

#ifdef WITH_PARTITION_STORAGE_ENGINE
  partition_info *tab_part_info= table->part_info;
  thd->work_part_info= thd->lex->part_info;
  if (tab_part_info)
  {
    /* ALTER TABLE ADD PARTITION IF NOT EXISTS */
    if ((alter_info->partition_flags & ALTER_PARTITION_ADD) &&
        thd->lex->create_info.if_not_exists())
    {
      partition_info *alt_part_info= thd->lex->part_info;
      if (alt_part_info)
      {
        List_iterator<partition_element> new_part_it(alt_part_info->partitions);
        partition_element *pe;
        while ((pe= new_part_it++))
        {
          if (!tab_part_info->has_unique_name(pe))
          {
            push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE,
                                ER_SAME_NAME_PARTITION,
                                ER_THD(thd, ER_SAME_NAME_PARTITION),
                                pe->partition_name);
            alter_info->partition_flags&= ~ALTER_PARTITION_ADD;
            thd->work_part_info= NULL;
            break;
          }
        }
      }
    }
    /* ALTER TABLE DROP PARTITION IF EXISTS */
    if ((alter_info->partition_flags & ALTER_PARTITION_DROP) &&
        thd->lex->if_exists())
    {
      List_iterator<const char> names_it(alter_info->partition_names);
      const char *name;

      while ((name= names_it++))
      {
        List_iterator<partition_element> part_it(tab_part_info->partitions);
        partition_element *part_elem;
        while ((part_elem= part_it++))
        {
          if (my_strcasecmp(system_charset_info,
                              part_elem->partition_name, name) == 0)
            break;
        }
        if (!part_elem)
        {
          push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE,
                              ER_DROP_PARTITION_NON_EXISTENT,
                              ER_THD(thd, ER_DROP_PARTITION_NON_EXISTENT),
                              "DROP");
          names_it.remove();
        }
      }
      if (alter_info->partition_names.elements == 0)
        alter_info->partition_flags&= ~ALTER_PARTITION_DROP;
    }
  }
#endif /*WITH_PARTITION_STORAGE_ENGINE*/

  /* ADD CONSTRAINT IF NOT EXISTS. */
  {
    List_iterator<Virtual_column_info> it(alter_info->check_constraint_list);
    Virtual_column_info *check;
    TABLE_SHARE *share= table->s;
    uint c;

    while ((check=it++))
    {
      if (!(check->flags & Alter_info::CHECK_CONSTRAINT_IF_NOT_EXISTS) &&
          check->name.length)
        continue;
      check->flags= 0;
      for (c= share->field_check_constraints;
           c < share->table_check_constraints ; c++)
      {
        Virtual_column_info *dup= table->check_constraints[c];
        if (dup->name.length == check->name.length &&
            lex_string_cmp(system_charset_info,
                           &check->name, &dup->name) == 0)
        {
          push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE,
            ER_DUP_CONSTRAINT_NAME, ER_THD(thd, ER_DUP_CONSTRAINT_NAME),
            "CHECK", check->name.str);
          it.remove();
          if (alter_info->check_constraint_list.elements == 0)
            alter_info->flags&= ~ALTER_ADD_CHECK_CONSTRAINT;

          break;
        }
      }
    }
  }

  /* ADD PERIOD */

  if (period_info->create_if_not_exists && table->s->period.name
      && table->s->period.name.streq(period_info->name))
  {
    DBUG_ASSERT(period_info->is_set());
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE,
                        ER_DUP_FIELDNAME, ER_THD(thd, ER_DUP_FIELDNAME),
                        period_info->name.str, table->s->table_name.str);

    List_iterator<Virtual_column_info> vit(alter_info->check_constraint_list);
    while (vit++ != period_info->constr)
    {
      // do nothing
    }
    vit.remove();

    *period_info= {};
  }

  DBUG_RETURN(false);
}


static bool fix_constraints_names(THD *thd, List<Virtual_column_info>
                                  *check_constraint_list,
                                  const HA_CREATE_INFO *create_info)
{
  List_iterator<Virtual_column_info> it((*check_constraint_list));
  Virtual_column_info *check;
  uint nr= 1;
  DBUG_ENTER("fix_constraints_names");
  if (!check_constraint_list)
    DBUG_RETURN(FALSE);
  // Prevent accessing freed memory during generating unique names
  while ((check=it++))
  {
    if (check->automatic_name)
    {
      check->name.str= NULL;
      check->name.length= 0;
    }
  }
  it.rewind();
  // Generate unique names if needed
  while ((check=it++))
  {
    if (!check->name.length)
    {
      check->automatic_name= TRUE;

      const char *own_name_base= create_info->period_info.constr == check
        ? create_info->period_info.name.str : NULL;

      if (make_unique_constraint_name(thd, &check->name,
                                      own_name_base,
                                      check_constraint_list,
                                      &nr))
        DBUG_RETURN(TRUE);
    }
  }
  DBUG_RETURN(FALSE);
}


static int compare_uint(const uint *s, const uint *t)
{
  return (*s < *t) ? -1 : ((*s > *t) ? 1 : 0);
}

static Compare_keys merge(Compare_keys current, Compare_keys add) {
  if (current == Compare_keys::Equal)
    return add;

  if (add == Compare_keys::Equal)
    return current;

  if (current == add)
    return current;

  if (current == Compare_keys::EqualButComment) {
    return Compare_keys::NotEqual;
  }

  if (current == Compare_keys::EqualButKeyPartLength) {
    if (add == Compare_keys::EqualButComment)
      return Compare_keys::NotEqual;
    DBUG_ASSERT(add == Compare_keys::NotEqual);
    return Compare_keys::NotEqual;
  }

  DBUG_ASSERT(current == Compare_keys::NotEqual);
  return current;
}

Compare_keys compare_keys_but_name(const KEY *table_key, const KEY *new_key,
                                   Alter_info *alter_info, const TABLE *table,
                                   const KEY *const new_pk,
                                   const KEY *const old_pk)
{
  if (table_key->algorithm != new_key->algorithm)
    return Compare_keys::NotEqual;

  if ((table_key->flags & HA_KEYFLAG_MASK) !=
      (new_key->flags & HA_KEYFLAG_MASK))
    return Compare_keys::NotEqual;

  if (table_key->user_defined_key_parts != new_key->user_defined_key_parts)
    return Compare_keys::NotEqual;

  if (table_key->block_size != new_key->block_size)
    return Compare_keys::NotEqual;

  /*
  Rebuild the index if following condition get satisfied:

  (i) Old table doesn't have primary key, new table has it and vice-versa
  (ii) Primary key changed to another existing index
  */
  if ((new_key == new_pk) != (table_key == old_pk))
    return Compare_keys::NotEqual;

  if (engine_options_differ(table_key->option_struct, new_key->option_struct,
                            table->file->ht->index_options))
    return Compare_keys::NotEqual;

  Compare_keys result= Compare_keys::Equal;

  for (const KEY_PART_INFO *
           key_part= table_key->key_part,
          *new_part= new_key->key_part,
          *end= table_key->key_part + table_key->user_defined_key_parts;
       key_part < end; key_part++, new_part++)
  {
    /*
      For prefix keys KEY_PART_INFO::field points to cloned Field
      object with adjusted length. So below we have to check field
      indexes instead of simply comparing pointers to Field objects.
    */
    const Create_field &new_field=
        *alter_info->create_list.elem(new_part->fieldnr);

    if (!new_field.field ||
        new_field.field->field_index != key_part->fieldnr - 1)
    {
      return Compare_keys::NotEqual;
    }

    auto compare= table->file->compare_key_parts(
        *table->field[key_part->fieldnr - 1], new_field, *key_part, *new_part);
    result= merge(result, compare);
  }

  /* Check that key comment is not changed. */
  if (cmp(table_key->comment, new_key->comment) != 0)
    result= merge(result, Compare_keys::EqualButComment);

  return result;
}

/**
   Compare original and new versions of a table and fill Alter_inplace_info
   describing differences between those versions.

   @param          thd                Thread
   @param          table              The original table.
   @param          varchar            Indicates that new definition has new
                                      VARCHAR column.
   @param[in/out]  ha_alter_info      Data structure which already contains
                                      basic information about create options,
                                      field and keys for the new version of
                                      table and which should be completed with
                                      more detailed information needed for
                                      in-place ALTER.

   First argument 'table' contains information of the original
   table, which includes all corresponding parts that the new
   table has in arguments create_list, key_list and create_info.

   Compare the changes between the original and new table definitions.
   The result of this comparison is then passed to SE which determines
   whether it can carry out these changes in-place.

   Mark any changes detected in the ha_alter_flags.
   We generally try to specify handler flags only if there are real
   changes. But in cases when it is cumbersome to determine if some
   attribute has really changed we might choose to set flag
   pessimistically, for example, relying on parser output only.

   If there are no data changes, but index changes, 'index_drop_buffer'
   and/or 'index_add_buffer' are populated with offsets into
   table->key_info or key_info_buffer respectively for the indexes
   that need to be dropped and/or (re-)created.

   Note that this function assumes that it is OK to change Alter_info
   and HA_CREATE_INFO which it gets. It is caller who is responsible
   for creating copies for this structures if he needs them unchanged.

   @retval true  error
   @retval false success
*/

static bool fill_alter_inplace_info(THD *thd, TABLE *table, bool varchar,
                                    Alter_inplace_info *ha_alter_info)
{
  Field **f_ptr, *field;
  List_iterator_fast<Create_field> new_field_it;
  Create_field *new_field;
  Alter_info *alter_info= ha_alter_info->alter_info;
  DBUG_ENTER("fill_alter_inplace_info");
  DBUG_PRINT("info", ("alter_info->flags: %llu", alter_info->flags));

  /* Allocate result buffers. */
  DBUG_ASSERT(ha_alter_info->rename_keys.mem_root() == thd->mem_root);
  if (! (ha_alter_info->index_drop_buffer=
          (KEY**) thd->alloc(sizeof(KEY*) * table->s->keys)) ||
      ! (ha_alter_info->index_add_buffer=
          (uint*) thd->alloc(sizeof(uint) *
                            alter_info->key_list.elements)) ||
      ha_alter_info->rename_keys.reserve(ha_alter_info->index_add_count))
    DBUG_RETURN(true);

  /*
    Copy parser flags, but remove some flags that handlers doesn't
    need to care about (old engines may not ignore these parser flags).
    ALTER_RENAME_COLUMN is replaced by ALTER_COLUMN_NAME.
    ALTER_CHANGE_COLUMN_DEFAULT is replaced by ALTER_CHANGE_COLUMN
    ALTER_PARSE_ADD_COLUMN, ALTER_PARSE_DROP_COLUMN, ALTER_ADD_INDEX and
    ALTER_DROP_INDEX are replaced with versions that have higher granuality.
  */

  alter_table_operations flags_to_remove=
      ALTER_ADD_INDEX | ALTER_DROP_INDEX | ALTER_PARSER_ADD_COLUMN |
      ALTER_PARSER_DROP_COLUMN | ALTER_COLUMN_ORDER | ALTER_RENAME_COLUMN |
      ALTER_CHANGE_COLUMN;

  if (!table->file->native_versioned())
    flags_to_remove|= ALTER_COLUMN_UNVERSIONED;

  ha_alter_info->handler_flags|= (alter_info->flags & ~flags_to_remove);
  /*
    Comparing new and old default values of column is cumbersome.
    So instead of using such a comparison for detecting if default
    has really changed we rely on flags set by parser to get an
    approximate value for storage engine flag.
  */
  if (alter_info->flags & ALTER_CHANGE_COLUMN)
    ha_alter_info->handler_flags|= ALTER_COLUMN_DEFAULT;

  /*
    If we altering table with old VARCHAR fields we will be automatically
    upgrading VARCHAR column types.
  */
  if (table->s->frm_version < FRM_VER_TRUE_VARCHAR && varchar)
    ha_alter_info->handler_flags|=  ALTER_STORED_COLUMN_TYPE;

  DBUG_PRINT("info", ("handler_flags: %llu", ha_alter_info->handler_flags));

  /*
    Go through fields in old version of table and detect changes to them.
    We don't want to rely solely on Alter_info flags for this since:
    a) new definition of column can be fully identical to the old one
       despite the fact that this column is mentioned in MODIFY clause.
    b) even if new column type differs from its old column from metadata
       point of view, it might be identical from storage engine point
       of view (e.g. when ENUM('a','b') is changed to ENUM('a','b',c')).
    c) flags passed to storage engine contain more detailed information
       about nature of changes than those provided from parser.
  */
  bool maybe_alter_vcol= false;
  uint field_stored_index= 0;
  for (f_ptr= table->field; (field= *f_ptr); f_ptr++,
                               field_stored_index+= field->stored_in_db())
  {
    /* Clear marker for renamed or dropped field
    which we are going to set later. */
    field->flags&= ~(FIELD_IS_RENAMED | FIELD_IS_DROPPED);

    /* Use transformed info to evaluate flags for storage engine. */
    uint new_field_index= 0, new_field_stored_index= 0;
    new_field_it.init(alter_info->create_list);
    while ((new_field= new_field_it++))
    {
      if (new_field->field == field)
        break;
      new_field_index++;
      new_field_stored_index+= new_field->stored_in_db();
    }

    if (new_field)
    {
      /* Field is not dropped. Evaluate changes bitmap for it. */

      /*
        Check if type of column has changed.
      */
      bool is_equal= field->is_equal(*new_field);
      if (!is_equal)
      {
        if (field->can_be_converted_by_engine(*new_field))
        {
          /*
            New column type differs from the old one, but storage engine can
            change it by itself.
            (for example, VARCHAR(300) is changed to VARCHAR(400)).
          */
          ha_alter_info->handler_flags|= ALTER_COLUMN_TYPE_CHANGE_BY_ENGINE;
        }
        else
        {
          /* New column type is incompatible with old one. */
          ha_alter_info->handler_flags|= field->stored_in_db()
                                             ? ALTER_STORED_COLUMN_TYPE
                                             : ALTER_VIRTUAL_COLUMN_TYPE;

          if (table->s->tmp_table == NO_TMP_TABLE)
          {
            delete_statistics_for_column(thd, table, field);
            KEY *key_info= table->key_info;
            for (uint i= 0; i < table->s->keys; i++, key_info++)
            {
              if (!field->part_of_key.is_set(i))
                continue;

              uint key_parts= table->actual_n_key_parts(key_info);
              for (uint j= 0; j < key_parts; j++)
              {
                if (key_info->key_part[j].fieldnr - 1 == field->field_index)
                {
                  delete_statistics_for_index(
                      thd, table, key_info,
                      j >= key_info->user_defined_key_parts);
                  break;
                }
              }
            }
          }
        }
      }

      if (field->vcol_info || new_field->vcol_info)
      {
        /* base <-> virtual or stored <-> virtual */
        if (field->stored_in_db() != new_field->stored_in_db())
          ha_alter_info->handler_flags|= ( ALTER_STORED_COLUMN_TYPE |
                                           ALTER_VIRTUAL_COLUMN_TYPE);
        if (field->vcol_info && new_field->vcol_info)
        {
          bool value_changes= !is_equal;
          alter_table_operations alter_expr;
          if (field->stored_in_db())
            alter_expr= ALTER_STORED_GCOL_EXPR;
          else
            alter_expr= ALTER_VIRTUAL_GCOL_EXPR;
          if (!field->vcol_info->is_equal(new_field->vcol_info))
          {
            ha_alter_info->handler_flags|= alter_expr;
            value_changes= true;
          }

          if ((ha_alter_info->handler_flags & ALTER_COLUMN_DEFAULT)
              && !(ha_alter_info->handler_flags & alter_expr))
          { /*
              a DEFAULT value of a some column was changed.  see if this vcol
              uses DEFAULT() function. The check is kind of expensive, so don't
              do it if ALTER_COLUMN_VCOL is already set.
            */
            if (field->vcol_info->expr->walk(
                                 &Item::check_func_default_processor, 0, 0))
            {
              ha_alter_info->handler_flags|= alter_expr;
              value_changes= true;
            }
          }

          if (field->vcol_info->is_in_partitioning_expr() ||
              field->flags & PART_KEY_FLAG || field->stored_in_db())
          {
            if (value_changes)
              ha_alter_info->handler_flags|= ALTER_COLUMN_VCOL;
            else
              maybe_alter_vcol= true;
          }
        }
        else /* base <-> stored */
          ha_alter_info->handler_flags|= ALTER_STORED_COLUMN_TYPE;
      }

      /* Check if field was renamed */
      if (lex_string_cmp(system_charset_info, &field->field_name,
                         &new_field->field_name))
      {
        field->flags|= FIELD_IS_RENAMED;
        ha_alter_info->handler_flags|= ALTER_COLUMN_NAME;
        rename_column_in_stat_tables(thd, table, field,
                                     new_field->field_name.str);
      }

      /* Check that NULL behavior is same for old and new fields */
      if ((new_field->flags & NOT_NULL_FLAG) !=
          (uint) (field->flags & NOT_NULL_FLAG))
      {
        if (new_field->flags & NOT_NULL_FLAG)
          ha_alter_info->handler_flags|= ALTER_COLUMN_NOT_NULLABLE;
        else
          ha_alter_info->handler_flags|= ALTER_COLUMN_NULLABLE;
      }

      /*
        We do not detect changes to default values in this loop.
        See comment above for more details.
      */

      /*
        Detect changes in column order.
      */
      if (field->stored_in_db())
      {
        if (field_stored_index != new_field_stored_index)
          ha_alter_info->handler_flags|= ALTER_STORED_COLUMN_ORDER;
      }
      else
      {
        if (field->field_index != new_field_index)
          ha_alter_info->handler_flags|= ALTER_VIRTUAL_COLUMN_ORDER;
      }

      /* Detect changes in storage type of column */
      if (new_field->field_storage_type() != field->field_storage_type())
        ha_alter_info->handler_flags|= ALTER_COLUMN_STORAGE_TYPE;

      /* Detect changes in column format of column */
      if (new_field->column_format() != field->column_format())
        ha_alter_info->handler_flags|= ALTER_COLUMN_COLUMN_FORMAT;

      if (engine_options_differ(field->option_struct, new_field->option_struct,
                                table->file->ht->field_options))
      {
        ha_alter_info->handler_flags|= ALTER_COLUMN_OPTION;
        ha_alter_info->create_info->fields_option_struct[f_ptr - table->field]=
          new_field->option_struct;
      }
    }
    else
    {
      // Field is not present in new version of table and therefore was dropped.
      field->flags|= FIELD_IS_DROPPED;
      if (field->stored_in_db())
        ha_alter_info->handler_flags|= ALTER_DROP_STORED_COLUMN;
      else
        ha_alter_info->handler_flags|= ALTER_DROP_VIRTUAL_COLUMN;
    }
  }

  if (maybe_alter_vcol)
  {
    /*
      What if one of the normal columns was altered and it was part of the some
      virtual column expression?  Currently we don't detect this correctly
      (FIXME), so let's just say that a vcol *might* be affected if any other
      column was altered.
    */
    if (ha_alter_info->handler_flags & (ALTER_STORED_COLUMN_TYPE |
                                        ALTER_VIRTUAL_COLUMN_TYPE |
                                        ALTER_COLUMN_NOT_NULLABLE |
                                        ALTER_COLUMN_OPTION))
      ha_alter_info->handler_flags|= ALTER_COLUMN_VCOL;
  }

  new_field_it.init(alter_info->create_list);
  while ((new_field= new_field_it++))
  {
    if (! new_field->field)
    {
      // Field is not present in old version of table and therefore was added.
      if (new_field->vcol_info)
      {
        if (new_field->stored_in_db())
          ha_alter_info->handler_flags|= ALTER_ADD_STORED_GENERATED_COLUMN;
        else
          ha_alter_info->handler_flags|= ALTER_ADD_VIRTUAL_COLUMN;
      }
      else
        ha_alter_info->handler_flags|= ALTER_ADD_STORED_BASE_COLUMN;
    }
  }

  /*
    Go through keys and check if the original ones are compatible
    with new table.
  */
  KEY *table_key;
  KEY *table_key_end= table->key_info + table->s->keys;
  KEY *new_key;
  KEY *new_key_end=
    ha_alter_info->key_info_buffer + ha_alter_info->key_count;
  /*
    Primary key index for the new table
  */
  const KEY* const new_pk= (ha_alter_info->key_count > 0 &&
                            (!my_strcasecmp(system_charset_info,
                                ha_alter_info->key_info_buffer->name.str,
                                primary_key_name) ||
                            is_candidate_key(ha_alter_info->key_info_buffer))) ?
                           ha_alter_info->key_info_buffer : NULL;
  const KEY *const old_pk= table->s->primary_key == MAX_KEY ? NULL :
                           table->key_info + table->s->primary_key;

  DBUG_PRINT("info", ("index count old: %d  new: %d",
                      table->s->keys, ha_alter_info->key_count));

  /*
    Step through all keys of the old table and search matching new keys.
  */
  ha_alter_info->index_drop_count= 0;
  ha_alter_info->index_add_count= 0;
  for (table_key= table->key_info; table_key < table_key_end; table_key++)
  {
    /* Search a new key with the same name. */
    for (new_key= ha_alter_info->key_info_buffer;
         new_key < new_key_end;
         new_key++)
    {
      if (!lex_string_cmp(system_charset_info, &table_key->name,
                          &new_key->name))
        break;
    }
    if (new_key >= new_key_end)
    {
      /* Key not found. Add the key to the drop buffer. */
      ha_alter_info->index_drop_buffer
        [ha_alter_info->index_drop_count++]=
        table_key;
      DBUG_PRINT("info", ("index dropped: '%s'", table_key->name.str));
      continue;
    }

    switch (compare_keys_but_name(table_key, new_key, alter_info, table, new_pk,
                                  old_pk))
    {
    case Compare_keys::Equal:
      continue;
    case Compare_keys::EqualButKeyPartLength:
      ha_alter_info->handler_flags|= ALTER_COLUMN_INDEX_LENGTH;
      continue;
    case Compare_keys::EqualButComment:
      ha_alter_info->handler_flags|= ALTER_CHANGE_INDEX_COMMENT;
      continue;
    case Compare_keys::NotEqual:
      break;
    }

    /* Key modified. Add the key / key offset to both buffers. */
    ha_alter_info->index_drop_buffer
      [ha_alter_info->index_drop_count++]=
      table_key;
    ha_alter_info->index_add_buffer
      [ha_alter_info->index_add_count++]=
      (uint)(new_key - ha_alter_info->key_info_buffer);
    /* Mark all old fields which are used in newly created index. */
    DBUG_PRINT("info", ("index changed: '%s'", table_key->name.str));
  }
  /*end of for (; table_key < table_key_end;) */

  /*
    Step through all keys of the new table and find matching old keys.
  */
  for (new_key= ha_alter_info->key_info_buffer;
       new_key < new_key_end;
       new_key++)
  {
    /* Search an old key with the same name. */
    for (table_key= table->key_info; table_key < table_key_end; table_key++)
    {
      if (!lex_string_cmp(system_charset_info, &table_key->name,
                          &new_key->name))
        break;
    }
    if (table_key >= table_key_end)
    {
      /* Key not found. Add the offset of the key to the add buffer. */
      ha_alter_info->index_add_buffer
        [ha_alter_info->index_add_count++]=
        (uint)(new_key - ha_alter_info->key_info_buffer);
      DBUG_PRINT("info", ("index added: '%s'", new_key->name.str));
    }
    else
      ha_alter_info->create_info->indexes_option_struct[table_key - table->key_info]=
        new_key->option_struct;
  }

  for (uint i= 0; i < ha_alter_info->index_add_count; i++)
  {
    uint *add_buffer= ha_alter_info->index_add_buffer;
    const KEY *new_key= ha_alter_info->key_info_buffer + add_buffer[i];

    for (uint j= 0; j < ha_alter_info->index_drop_count; j++)
    {
      KEY **drop_buffer= ha_alter_info->index_drop_buffer;
      const KEY *old_key= drop_buffer[j];

      if (compare_keys_but_name(old_key, new_key, alter_info, table, new_pk,
                                old_pk) != Compare_keys::Equal)
      {
        continue;
      }

      DBUG_ASSERT(
          lex_string_cmp(system_charset_info, &old_key->name, &new_key->name));

      ha_alter_info->handler_flags|= ALTER_RENAME_INDEX;
      ha_alter_info->rename_keys.push_back(
          Alter_inplace_info::Rename_key_pair(old_key, new_key));

      --ha_alter_info->index_add_count;
      --ha_alter_info->index_drop_count;
      memmove(add_buffer + i, add_buffer + i + 1,
              sizeof(add_buffer[0]) * (ha_alter_info->index_add_count - i));
      memmove(drop_buffer + j, drop_buffer + j + 1,
              sizeof(drop_buffer[0]) * (ha_alter_info->index_drop_count - j));
      --i; // this index once again
      break;
    }
  }

  /*
    Sort index_add_buffer according to how key_info_buffer is sorted.
    I.e. with primary keys first - see sort_keys().
  */
  my_qsort(ha_alter_info->index_add_buffer,
           ha_alter_info->index_add_count,
           sizeof(uint), (qsort_cmp) compare_uint);

  /* Now let us calculate flags for storage engine API. */

  /* Figure out what kind of indexes we are dropping. */
  KEY **dropped_key;
  KEY **dropped_key_end= ha_alter_info->index_drop_buffer +
                         ha_alter_info->index_drop_count;

  for (dropped_key= ha_alter_info->index_drop_buffer;
       dropped_key < dropped_key_end; dropped_key++)
  {
    table_key= *dropped_key;

    if (table_key->flags & HA_NOSAME)
    {
      if (table_key == old_pk)
        ha_alter_info->handler_flags|= ALTER_DROP_PK_INDEX;
      else
        ha_alter_info->handler_flags|= ALTER_DROP_UNIQUE_INDEX;
    }
    else
      ha_alter_info->handler_flags|= ALTER_DROP_NON_UNIQUE_NON_PRIM_INDEX;
  }

  /* Now figure out what kind of indexes we are adding. */
  for (uint add_key_idx= 0; add_key_idx < ha_alter_info->index_add_count; add_key_idx++)
  {
    new_key= ha_alter_info->key_info_buffer + ha_alter_info->index_add_buffer[add_key_idx];

    if (new_key->flags & HA_NOSAME)
    {
      if (new_key == new_pk)
        ha_alter_info->handler_flags|= ALTER_ADD_PK_INDEX;
      else
        ha_alter_info->handler_flags|= ALTER_ADD_UNIQUE_INDEX;
    }
    else
      ha_alter_info->handler_flags|= ALTER_ADD_NON_UNIQUE_NON_PRIM_INDEX;
  }

  DBUG_PRINT("exit", ("handler_flags: %llu", ha_alter_info->handler_flags));
  DBUG_RETURN(false);
}


/**
  Mark fields participating in newly added indexes in TABLE object which
  corresponds to new version of altered table.

  @param ha_alter_info  Alter_inplace_info describing in-place ALTER.
  @param altered_table  TABLE object for new version of TABLE in which
                        fields should be marked.
*/

static void update_altered_table(const Alter_inplace_info &ha_alter_info,
                                 TABLE *altered_table)
{
  uint field_idx, add_key_idx;
  KEY *key;
  KEY_PART_INFO *end, *key_part;

  /*
    Clear marker for all fields, as we are going to set it only
    for fields which participate in new indexes.
  */
  for (field_idx= 0; field_idx < altered_table->s->fields; ++field_idx)
    altered_table->field[field_idx]->flags&= ~FIELD_IN_ADD_INDEX;

  /*
    Go through array of newly added indexes and mark fields
    participating in them.
  */
  for (add_key_idx= 0; add_key_idx < ha_alter_info.index_add_count;
       add_key_idx++)
  {
    key= ha_alter_info.key_info_buffer +
         ha_alter_info.index_add_buffer[add_key_idx];

    end= key->key_part + key->user_defined_key_parts;
    for (key_part= key->key_part; key_part < end; key_part++)
      altered_table->field[key_part->fieldnr]->flags|= FIELD_IN_ADD_INDEX;
  }
}


/**
  Compare two tables to see if their metadata are compatible.
  One table specified by a TABLE instance, the other using Alter_info
  and HA_CREATE_INFO.

  @param[in]  table          The first table.
  @param[in]  alter_info     Alter options, fields and keys for the
                             second table.
  @param[in]  create_info    Create options for the second table.
  @param[out] metadata_equal Result of comparison.

  @retval true   error
  @retval false  success
*/

bool mysql_compare_tables(TABLE *table,
                          Alter_info *alter_info,
                          HA_CREATE_INFO *create_info,
                          bool *metadata_equal)
{
  DBUG_ENTER("mysql_compare_tables");

  uint changes= IS_EQUAL_NO;
  uint key_count;
  List_iterator_fast<Create_field> tmp_new_field_it;
  THD *thd= table->in_use;
  *metadata_equal= false;

  /*
    Create a copy of alter_info.
    To compare definitions, we need to "prepare" the definition - transform it
    from parser output to a format that describes the table layout (all column
    defaults are initialized, duplicate columns are removed). This is done by
    mysql_prepare_create_table.  Unfortunately, mysql_prepare_create_table
    performs its transformations "in-place", that is, modifies the argument.
    Since we would like to keep mysql_compare_tables() idempotent (not altering
    any of the arguments) we create a copy of alter_info here and pass it to
    mysql_prepare_create_table, then use the result to compare the tables, and
    then destroy the copy.
  */
  Alter_info tmp_alter_info(*alter_info, thd->mem_root);
  uint db_options= 0; /* not used */
  KEY *key_info_buffer= NULL;

  /* Create the prepared information. */
  int create_table_mode= table->s->tmp_table == NO_TMP_TABLE ?
                           C_ORDINARY_CREATE : C_ALTER_TABLE;
  if (mysql_prepare_create_table(thd, create_info, &tmp_alter_info,
                                 &db_options, table->file, &key_info_buffer,
                                 &key_count, create_table_mode))
    DBUG_RETURN(1);

  /* Some very basic checks. */
  if (table->s->fields != alter_info->create_list.elements ||
      table->s->db_type() != create_info->db_type ||
      table->s->tmp_table ||
      (table->s->row_type != create_info->row_type))
    DBUG_RETURN(false);

  /* Go through fields and check if they are compatible. */
  tmp_new_field_it.init(tmp_alter_info.create_list);
  for (Field **f_ptr= table->field; *f_ptr; f_ptr++)
  {
    Field *field= *f_ptr;
    Create_field *tmp_new_field= tmp_new_field_it++;

    /* Check that NULL behavior is the same. */
    if ((tmp_new_field->flags & NOT_NULL_FLAG) !=
	(uint) (field->flags & NOT_NULL_FLAG))
      DBUG_RETURN(false);

    /*
      mysql_prepare_alter_table() clears HA_OPTION_PACK_RECORD bit when
      preparing description of existing table. In ALTER TABLE it is later
      updated to correct value by create_table_impl() call.
      So to get correct value of this bit in this function we have to
      mimic behavior of create_table_impl().
    */
    if (create_info->row_type == ROW_TYPE_DYNAMIC ||
        create_info->row_type == ROW_TYPE_PAGE ||
	(tmp_new_field->flags & BLOB_FLAG) ||
	(tmp_new_field->real_field_type() == MYSQL_TYPE_VARCHAR &&
	create_info->row_type != ROW_TYPE_FIXED))
      create_info->table_options|= HA_OPTION_PACK_RECORD;

    /* Check if field was renamed */
    if (lex_string_cmp(system_charset_info,
                       &field->field_name,
                       &tmp_new_field->field_name))
      DBUG_RETURN(false);

    /* Evaluate changes bitmap and send to check_if_incompatible_data() */
    uint field_changes= field->is_equal(*tmp_new_field);
    if (field_changes != IS_EQUAL_YES)
      DBUG_RETURN(false);

    changes|= field_changes;
  }

  /* Check if changes are compatible with current handler. */
  if (table->file->check_if_incompatible_data(create_info, changes))
    DBUG_RETURN(false);

  /* Go through keys and check if they are compatible. */
  KEY *table_key;
  KEY *table_key_end= table->key_info + table->s->keys;
  KEY *new_key;
  KEY *new_key_end= key_info_buffer + key_count;

  /* Step through all keys of the first table and search matching keys. */
  for (table_key= table->key_info; table_key < table_key_end; table_key++)
  {
    /* Search a key with the same name. */
    for (new_key= key_info_buffer; new_key < new_key_end; new_key++)
    {
      if (!lex_string_cmp(system_charset_info, &table_key->name,
                          &new_key->name))
        break;
    }
    if (new_key >= new_key_end)
      DBUG_RETURN(false);

    /* Check that the key types are compatible. */
    if ((table_key->algorithm != new_key->algorithm) ||
	((table_key->flags & HA_KEYFLAG_MASK) !=
         (new_key->flags & HA_KEYFLAG_MASK)) ||
        (table_key->user_defined_key_parts !=
         new_key->user_defined_key_parts))
      DBUG_RETURN(false);

    /* Check that the key parts remain compatible. */
    KEY_PART_INFO *table_part;
    KEY_PART_INFO *table_part_end= table_key->key_part + table_key->user_defined_key_parts;
    KEY_PART_INFO *new_part;
    for (table_part= table_key->key_part, new_part= new_key->key_part;
         table_part < table_part_end;
         table_part++, new_part++)
    {
      /*
	Key definition is different if we are using a different field or
	if the used key part length is different. We know that the fields
        are equal. Comparing field numbers is sufficient.
      */
      if ((table_part->length != new_part->length) ||
          (table_part->fieldnr - 1 != new_part->fieldnr))
        DBUG_RETURN(false);
    }
  }

  /* Step through all keys of the second table and find matching keys. */
  for (new_key= key_info_buffer; new_key < new_key_end; new_key++)
  {
    /* Search a key with the same name. */
    for (table_key= table->key_info; table_key < table_key_end; table_key++)
    {
      if (!lex_string_cmp(system_charset_info, &table_key->name,
                          &new_key->name))
        break;
    }
    if (table_key >= table_key_end)
      DBUG_RETURN(false);
  }

  *metadata_equal= true; // Tables are compatible
  DBUG_RETURN(false);
}


/*
  Manages enabling/disabling of indexes for ALTER TABLE

  SYNOPSIS
    alter_table_manage_keys()
      table                  Target table
      indexes_were_disabled  Whether the indexes of the from table
                             were disabled
      keys_onoff             ENABLE | DISABLE | LEAVE_AS_IS

  RETURN VALUES
    FALSE  OK
    TRUE   Error
*/

static
bool alter_table_manage_keys(TABLE *table, int indexes_were_disabled,
                             Alter_info::enum_enable_or_disable keys_onoff)
{
  int error= 0;
  DBUG_ENTER("alter_table_manage_keys");
  DBUG_PRINT("enter", ("table=%p were_disabled=%d on_off=%d",
             table, indexes_were_disabled, keys_onoff));

  switch (keys_onoff) {
  case Alter_info::ENABLE:
    DEBUG_SYNC(table->in_use, "alter_table_enable_indexes");
    error= table->file->ha_enable_indexes(HA_KEY_SWITCH_NONUNIQ_SAVE);
    break;
  case Alter_info::LEAVE_AS_IS:
    if (!indexes_were_disabled)
      break;
    /* fall through */
  case Alter_info::DISABLE:
    error= table->file->ha_disable_indexes(HA_KEY_SWITCH_NONUNIQ_SAVE);
  }

  if (unlikely(error))
  {
    if (error == HA_ERR_WRONG_COMMAND)
    {
      THD *thd= table->in_use;
      push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE,
                          ER_ILLEGAL_HA, ER_THD(thd, ER_ILLEGAL_HA),
                          table->file->table_type(),
                          table->s->db.str, table->s->table_name.str);
      error= 0;
    }
    else
      table->file->print_error(error, MYF(0));
  }
  DBUG_RETURN(error);
}


/**
  Check if the pending ALTER TABLE operations support the in-place
  algorithm based on restrictions in the SQL layer or given the
  nature of the operations themselves. If in-place isn't supported,
  it won't be necessary to check with the storage engine.

  @param table        The original TABLE.
  @param create_info  Information from the parsing phase about new
                      table properties.
  @param alter_info   Data related to detected changes.

  @return false       In-place is possible, check with storage engine.
  @return true        Incompatible operations, must use table copy.
*/

static bool is_inplace_alter_impossible(TABLE *table,
                                        HA_CREATE_INFO *create_info,
                                        const Alter_info *alter_info)
{
  DBUG_ENTER("is_inplace_alter_impossible");

  /* At the moment we can't handle altering temporary tables without a copy. */
  if (table->s->tmp_table)
    DBUG_RETURN(true);

  /*
    For the ALTER TABLE tbl_name ORDER BY ... we always use copy
    algorithm. In theory, this operation can be done in-place by some
    engine, but since a) no current engine does this and b) our current
    API lacks infrastructure for passing information about table ordering
    to storage engine we simply always do copy now.

    ENABLE/DISABLE KEYS is a MyISAM/Heap specific operation that is
    not supported for in-place in combination with other operations.
    Alone, it will be done by simple_rename_or_index_change().
  */
  if (alter_info->flags & (ALTER_ORDER | ALTER_KEYS_ONOFF))
    DBUG_RETURN(true);

  /*
    If the table engine is changed explicitly (using ENGINE clause)
    or implicitly (e.g. when non-partitioned table becomes
    partitioned) a regular alter table (copy) needs to be
    performed.
  */
  if (create_info->db_type != table->s->db_type())
    DBUG_RETURN(true);

  /*
    There was a bug prior to mysql-4.0.25. Number of null fields was
    calculated incorrectly. As a result frm and data files gets out of
    sync after fast alter table. There is no way to determine by which
    mysql version (in 4.0 and 4.1 branches) table was created, thus we
    disable fast alter table for all tables created by mysql versions
    prior to 5.0 branch.
    See BUG#6236.
  */
  if (!table->s->mysql_version)
    DBUG_RETURN(true);

  /*
    If we are using a MySQL 5.7 table with virtual fields, ALTER TABLE must
    recreate the table as we need to rewrite generated fields
  */
  if (table->s->mysql_version > 50700 && table->s->mysql_version < 100000 &&
      table->s->virtual_fields)
    DBUG_RETURN(TRUE);

  DBUG_RETURN(false);
}


/**
  Perform in-place alter table.

  @param thd                Thread handle.
  @param table_list         TABLE_LIST for the table to change.
  @param table              The original TABLE.
  @param altered_table      TABLE object for new version of the table.
  @param ha_alter_info      Structure describing ALTER TABLE to be carried
                            out and serving as a storage place for data
                            used during different phases.
  @param target_mdl_request Metadata request/lock on the target table name.
  @param alter_ctx          ALTER TABLE runtime context.

  @retval   true              Error
  @retval   false             Success

  @note
    If mysql_alter_table does not need to copy the table, it is
    either an alter table where the storage engine does not
    need to know about the change, only the frm will change,
    or the storage engine supports performing the alter table
    operation directly, in-place without mysql having to copy
    the table.

  @note This function frees the TABLE object associated with the new version of
        the table and removes the .FRM file for it in case of both success and
        failure.
*/

static bool mysql_inplace_alter_table(THD *thd,
                                      TABLE_LIST *table_list,
                                      TABLE *table,
                                      TABLE *altered_table,
                                      Alter_inplace_info *ha_alter_info,
                                      MDL_request *target_mdl_request,
                                      Alter_table_ctx *alter_ctx)
{
  Open_table_context ot_ctx(thd, MYSQL_OPEN_REOPEN | MYSQL_OPEN_IGNORE_KILLED);
  handlerton *db_type= table->s->db_type();
  MDL_ticket *mdl_ticket= table->mdl_ticket;
  Alter_info *alter_info= ha_alter_info->alter_info;
  bool reopen_tables= false;
  bool res;
  const enum_alter_inplace_result inplace_supported=
    ha_alter_info->inplace_supported;

  DBUG_ENTER("mysql_inplace_alter_table");

  /* Downgrade DDL lock while we are waiting for exclusive lock below */
  backup_set_alter_copy_lock(thd, table);

  /*
    Upgrade to EXCLUSIVE lock if:
    - This is requested by the storage engine
    - Or the storage engine needs exclusive lock for just the prepare
      phase
    - Or requested by the user

    Note that we handle situation when storage engine needs exclusive
    lock for prepare phase under LOCK TABLES in the same way as when
    exclusive lock is required for duration of the whole statement.
  */
  if (inplace_supported == HA_ALTER_INPLACE_EXCLUSIVE_LOCK ||
      ((inplace_supported == HA_ALTER_INPLACE_COPY_NO_LOCK ||
        inplace_supported == HA_ALTER_INPLACE_COPY_LOCK ||
        inplace_supported == HA_ALTER_INPLACE_NOCOPY_NO_LOCK ||
        inplace_supported == HA_ALTER_INPLACE_NOCOPY_LOCK ||
        inplace_supported == HA_ALTER_INPLACE_INSTANT) &&
       (thd->locked_tables_mode == LTM_LOCK_TABLES ||
        thd->locked_tables_mode == LTM_PRELOCKED_UNDER_LOCK_TABLES)) ||
      alter_info->requested_lock == Alter_info::ALTER_TABLE_LOCK_EXCLUSIVE)
  {
    if (wait_while_table_is_used(thd, table, HA_EXTRA_FORCE_REOPEN))
      goto cleanup;
    /*
      Get rid of all TABLE instances belonging to this thread
      except one to be used for in-place ALTER TABLE.

      This is mostly needed to satisfy InnoDB assumptions/asserts.
    */
    close_all_tables_for_name(thd, table->s,
                              alter_ctx->is_table_renamed() ?
                              HA_EXTRA_PREPARE_FOR_RENAME :
			      HA_EXTRA_NOT_USED,
                              table);
    /*
      If we are under LOCK TABLES we will need to reopen tables which we
      just have closed in case of error.
    */
    reopen_tables= true;
  }
  else if (inplace_supported == HA_ALTER_INPLACE_COPY_LOCK ||
           inplace_supported == HA_ALTER_INPLACE_COPY_NO_LOCK ||
           inplace_supported == HA_ALTER_INPLACE_NOCOPY_LOCK ||
           inplace_supported == HA_ALTER_INPLACE_NOCOPY_NO_LOCK ||
           inplace_supported == HA_ALTER_INPLACE_INSTANT)
  {
    /*
      Storage engine has requested exclusive lock only for prepare phase
      and we are not under LOCK TABLES.
      Don't mark TABLE_SHARE as old in this case, as this won't allow opening
      of table by other threads during main phase of in-place ALTER TABLE.
    */
    if (thd->mdl_context.upgrade_shared_lock(table->mdl_ticket, MDL_EXCLUSIVE,
                                             thd->variables.lock_wait_timeout))
      goto cleanup;

    tdc_remove_table(thd, TDC_RT_REMOVE_NOT_OWN_KEEP_SHARE,
                     table->s->db.str, table->s->table_name.str,
                     false);
  }

  /*
    Upgrade to SHARED_NO_WRITE lock if:
    - The storage engine needs writes blocked for the whole duration
    - Or this is requested by the user
    Note that under LOCK TABLES, we will already have SHARED_NO_READ_WRITE.
  */
  if ((inplace_supported == HA_ALTER_INPLACE_SHARED_LOCK ||
       alter_info->requested_lock == Alter_info::ALTER_TABLE_LOCK_SHARED) &&
      thd->mdl_context.upgrade_shared_lock(table->mdl_ticket,
                                           MDL_SHARED_NO_WRITE,
                                           thd->variables.lock_wait_timeout))
    goto cleanup;

  // It's now safe to take the table level lock.
  if (lock_tables(thd, table_list, alter_ctx->tables_opened, 0))
    goto cleanup;

  DEBUG_SYNC(thd, "alter_table_inplace_after_lock_upgrade");
  THD_STAGE_INFO(thd, stage_alter_inplace_prepare);

  switch (inplace_supported) {
  case HA_ALTER_ERROR:
  case HA_ALTER_INPLACE_NOT_SUPPORTED:
    DBUG_ASSERT(0);
    // fall through
  case HA_ALTER_INPLACE_NO_LOCK:
  case HA_ALTER_INPLACE_INSTANT:
  case HA_ALTER_INPLACE_COPY_NO_LOCK:
  case HA_ALTER_INPLACE_NOCOPY_NO_LOCK:
    switch (alter_info->requested_lock) {
    case Alter_info::ALTER_TABLE_LOCK_DEFAULT:
    case Alter_info::ALTER_TABLE_LOCK_NONE:
      ha_alter_info->online= true;
      break;
    case Alter_info::ALTER_TABLE_LOCK_SHARED:
    case Alter_info::ALTER_TABLE_LOCK_EXCLUSIVE:
      break;
    }
    break;
  case HA_ALTER_INPLACE_EXCLUSIVE_LOCK:
  case HA_ALTER_INPLACE_SHARED_LOCK:
  case HA_ALTER_INPLACE_COPY_LOCK:
  case HA_ALTER_INPLACE_NOCOPY_LOCK:
    break;
  }

  if (table->file->ha_prepare_inplace_alter_table(altered_table,
                                                  ha_alter_info))
    goto rollback;

  /*
    Downgrade the lock if storage engine has told us that exclusive lock was
    necessary only for prepare phase (unless we are not under LOCK TABLES) and
    user has not explicitly requested exclusive lock.
  */
  if ((inplace_supported == HA_ALTER_INPLACE_COPY_NO_LOCK ||
       inplace_supported == HA_ALTER_INPLACE_COPY_LOCK ||
       inplace_supported == HA_ALTER_INPLACE_NOCOPY_LOCK ||
       inplace_supported == HA_ALTER_INPLACE_NOCOPY_NO_LOCK) &&
      !(thd->locked_tables_mode == LTM_LOCK_TABLES ||
        thd->locked_tables_mode == LTM_PRELOCKED_UNDER_LOCK_TABLES) &&
      (alter_info->requested_lock != Alter_info::ALTER_TABLE_LOCK_EXCLUSIVE))
  {
    /* If storage engine or user requested shared lock downgrade to SNW. */
    if (inplace_supported == HA_ALTER_INPLACE_COPY_LOCK ||
        inplace_supported == HA_ALTER_INPLACE_NOCOPY_LOCK ||
        alter_info->requested_lock == Alter_info::ALTER_TABLE_LOCK_SHARED)
      table->mdl_ticket->downgrade_lock(MDL_SHARED_NO_WRITE);
    else
    {
      DBUG_ASSERT(inplace_supported == HA_ALTER_INPLACE_COPY_NO_LOCK ||
                  inplace_supported == HA_ALTER_INPLACE_NOCOPY_NO_LOCK);
      table->mdl_ticket->downgrade_lock(MDL_SHARED_UPGRADABLE);
    }
  }

  DEBUG_SYNC(thd, "alter_table_inplace_after_lock_downgrade");
  THD_STAGE_INFO(thd, stage_alter_inplace);

  /* We can abort alter table for any table type */
  thd->abort_on_warning= !ha_alter_info->ignore && thd->is_strict_mode();
  res= table->file->ha_inplace_alter_table(altered_table, ha_alter_info);
  thd->abort_on_warning= false;
  if (res)
    goto rollback;

  DEBUG_SYNC(thd, "alter_table_inplace_before_lock_upgrade");
  // Upgrade to EXCLUSIVE before commit.
  if (wait_while_table_is_used(thd, table, HA_EXTRA_PREPARE_FOR_RENAME))
    goto rollback;

  /* Set MDL_BACKUP_DDL */
  if (backup_reset_alter_copy_lock(thd))
    goto rollback;

  /*
    If we are killed after this point, we should ignore and continue.
    We have mostly completed the operation at this point, there should
    be no long waits left.
  */

  DBUG_EXECUTE_IF("alter_table_rollback_new_index", {
      table->file->ha_commit_inplace_alter_table(altered_table,
                                                 ha_alter_info,
                                                 false);
      my_error(ER_UNKNOWN_ERROR, MYF(0));
      goto cleanup;
    });

  DEBUG_SYNC(thd, "alter_table_inplace_before_commit");
  THD_STAGE_INFO(thd, stage_alter_inplace_commit);

  {
    TR_table trt(thd, true);
    if (trt != *table_list && table->file->ht->prepare_commit_versioned)
    {
      ulonglong trx_start_id= 0;
      ulonglong trx_end_id= table->file->ht->prepare_commit_versioned(thd, &trx_start_id);
      if (trx_end_id)
      {
        if (!TR_table::use_transaction_registry)
        {
          my_error(ER_VERS_TRT_IS_DISABLED, MYF(0));
          goto rollback;
        }
        if (trt.update(trx_start_id, trx_end_id))
        {
          goto rollback;
        }
      }
    }

    if (table->file->ha_commit_inplace_alter_table(altered_table,
                                                  ha_alter_info,
                                                  true))
    {
      goto rollback;
    }
  }

  close_all_tables_for_name(thd, table->s,
                            alter_ctx->is_table_renamed() ?
                            HA_EXTRA_PREPARE_FOR_RENAME :
                            HA_EXTRA_NOT_USED,
                            NULL);
  table_list->table= table= NULL;

  /*
    Replace the old .FRM with the new .FRM, but keep the old name for now.
    Rename to the new name (if needed) will be handled separately below.

    TODO: remove this check of thd->is_error() (now it intercept
    errors in some val_*() methods and bring some single place to
    such error interception).
  */
  if (mysql_rename_table(db_type, &alter_ctx->new_db, &alter_ctx->tmp_name,
                         &alter_ctx->db, &alter_ctx->alias,
                         FN_FROM_IS_TMP | NO_HA_TABLE) ||
                         thd->is_error())
  {
    // Since changes were done in-place, we can't revert them.
    DBUG_RETURN(true);
  }

  table_list->mdl_request.ticket= mdl_ticket;
  if (open_table(thd, table_list, &ot_ctx))
    DBUG_RETURN(true);

  /*
    Tell the handler that the changed frm is on disk and table
    has been re-opened
  */
  table_list->table->file->ha_notify_table_changed();

  /*
    We might be going to reopen table down on the road, so we have to
    restore state of the TABLE object which we used for obtaining of
    handler object to make it usable for later reopening.
  */
  close_thread_table(thd, &thd->open_tables);
  table_list->table= NULL;

  // Rename altered table if requested.
  if (alter_ctx->is_table_renamed())
  {
    // Remove TABLE and TABLE_SHARE for old name from TDC.
    tdc_remove_table(thd, TDC_RT_REMOVE_ALL,
                     alter_ctx->db.str, alter_ctx->table_name.str, false);

    if (mysql_rename_table(db_type, &alter_ctx->db, &alter_ctx->table_name,
                           &alter_ctx->new_db, &alter_ctx->new_alias, 0))
    {
      /*
        If the rename fails we will still have a working table
        with the old name, but with other changes applied.
      */
      DBUG_RETURN(true);
    }
    if (Table_triggers_list::change_table_name(thd,
                                               &alter_ctx->db,
                                               &alter_ctx->alias,
                                               &alter_ctx->table_name,
                                               &alter_ctx->new_db,
                                               &alter_ctx->new_alias))
    {
      /*
        If the rename of trigger files fails, try to rename the table
        back so we at least have matching table and trigger files.
      */
      (void) mysql_rename_table(db_type,
                                &alter_ctx->new_db, &alter_ctx->new_alias,
                                &alter_ctx->db, &alter_ctx->alias, NO_FK_CHECKS);
      DBUG_RETURN(true);
    }
    rename_table_in_stat_tables(thd, &alter_ctx->db, &alter_ctx->alias,
                                &alter_ctx->new_db, &alter_ctx->new_alias);
  }

  DBUG_RETURN(false);

 rollback:
  table->file->ha_commit_inplace_alter_table(altered_table,
                                             ha_alter_info,
                                             false);
 cleanup:
  if (reopen_tables)
  {
    /* Close the only table instance which is still around. */
    close_all_tables_for_name(thd, table->s,
                              alter_ctx->is_table_renamed() ?
                              HA_EXTRA_PREPARE_FOR_RENAME :
                              HA_EXTRA_NOT_USED,
                              NULL);
    if (thd->locked_tables_list.reopen_tables(thd, false))
      thd->locked_tables_list.unlink_all_closed_tables(thd, NULL, 0);
    /* QQ; do something about metadata locks ? */
  }
  DBUG_RETURN(true);
}

/**
  maximum possible length for certain blob types.

  @param[in]      type        Blob type (e.g. MYSQL_TYPE_TINY_BLOB)

  @return
    length
*/

static uint
blob_length_by_type(enum_field_types type)
{
  switch (type)
  {
  case MYSQL_TYPE_TINY_BLOB:
    return 255;
  case MYSQL_TYPE_BLOB:
    return 65535;
  case MYSQL_TYPE_MEDIUM_BLOB:
    return 16777215;
  case MYSQL_TYPE_LONG_BLOB:
    return (uint) UINT_MAX32;
  default:
    DBUG_ASSERT(0); // we should never go here
    return 0;
  }
}


static inline
void append_drop_column(THD *thd, String *str, Field *field)
{
  if (str->length())
    str->append(STRING_WITH_LEN(", "));
  str->append(STRING_WITH_LEN("DROP COLUMN "));
  append_identifier(thd, str, &field->field_name);
}


/**
  Prepare column and key definitions for CREATE TABLE in ALTER TABLE.

  This function transforms parse output of ALTER TABLE - lists of
  columns and keys to add, drop or modify into, essentially,
  CREATE TABLE definition - a list of columns and keys of the new
  table. While doing so, it also performs some (bug not all)
  semantic checks.

  This function is invoked when we know that we're going to
  perform ALTER TABLE via a temporary table -- i.e. in-place ALTER TABLE
  is not possible, perhaps because the ALTER statement contains
  instructions that require change in table data, not only in
  table definition or indexes.

  @param[in,out]  thd         thread handle. Used as a memory pool
                              and source of environment information.
  @param[in]      table       the source table, open and locked
                              Used as an interface to the storage engine
                              to acquire additional information about
                              the original table.
  @param[in,out]  create_info A blob with CREATE/ALTER TABLE
                              parameters
  @param[in,out]  alter_info  Another blob with ALTER/CREATE parameters.
                              Originally create_info was used only in
                              CREATE TABLE and alter_info only in ALTER TABLE.
                              But since ALTER might end-up doing CREATE,
                              this distinction is gone and we just carry
                              around two structures.
  @param[in,out]  alter_ctx   Runtime context for ALTER TABLE.

  @return
    Fills various create_info members based on information retrieved
    from the storage engine.
    Sets create_info->varchar if the table has a VARCHAR column.
    Prepares alter_info->create_list and alter_info->key_list with
    columns and keys of the new table.

  @retval TRUE   error, out of memory or a semantical error in ALTER
                 TABLE instructions
  @retval FALSE  success
*/

bool
mysql_prepare_alter_table(THD *thd, TABLE *table,
                          HA_CREATE_INFO *create_info,
                          Alter_info *alter_info,
                          Alter_table_ctx *alter_ctx)
{
  /* New column definitions are added here */
  List<Create_field> new_create_list;
  /* New key definitions are added here */
  List<Key> new_key_list;
  List_iterator<Alter_drop> drop_it(alter_info->drop_list);
  List_iterator<Create_field> def_it(alter_info->create_list);
  List_iterator<Alter_column> alter_it(alter_info->alter_list);
  List_iterator<Key> key_it(alter_info->key_list);
  List_iterator<Create_field> find_it(new_create_list);
  List_iterator<Create_field> field_it(new_create_list);
  List<Key_part_spec> key_parts;
  List<Virtual_column_info> new_constraint_list;
  uint db_create_options= (table->s->db_create_options
                           & ~(HA_OPTION_PACK_RECORD));
  Item::func_processor_rename column_rename_param;
  uint used_fields, dropped_sys_vers_fields= 0;
  KEY *key_info=table->key_info;
  bool rc= TRUE;
  bool modified_primary_key= FALSE;
  bool vers_system_invisible= false;
  Create_field *def;
  Field **f_ptr,*field;
  MY_BITMAP *dropped_fields= NULL; // if it's NULL - no dropped fields
  bool drop_period= false;
  DBUG_ENTER("mysql_prepare_alter_table");

  /*
    Merge incompatible changes flag in case of upgrade of a table from an
    old MariaDB or MySQL version.  This ensures that we don't try to do an
    online alter table if field packing or character set changes are required.
  */
  create_info->used_fields|= table->s->incompatible_version;
  used_fields= create_info->used_fields;

  create_info->varchar= FALSE;
  /* Let new create options override the old ones */
  if (!(used_fields & HA_CREATE_USED_MIN_ROWS))
    create_info->min_rows= table->s->min_rows;
  if (!(used_fields & HA_CREATE_USED_MAX_ROWS))
    create_info->max_rows= table->s->max_rows;
  if (!(used_fields & HA_CREATE_USED_AVG_ROW_LENGTH))
    create_info->avg_row_length= table->s->avg_row_length;
  if (!(used_fields & HA_CREATE_USED_DEFAULT_CHARSET))
    create_info->default_table_charset= table->s->table_charset;
  if (!(used_fields & HA_CREATE_USED_AUTO) && table->found_next_number_field)
  {
    /* Table has an autoincrement, copy value to new table */
    table->file->info(HA_STATUS_AUTO);
    create_info->auto_increment_value= table->file->stats.auto_increment_value;
  }

  if (!(used_fields & HA_CREATE_USED_KEY_BLOCK_SIZE))
    create_info->key_block_size= table->s->key_block_size;

  if (!(used_fields & HA_CREATE_USED_STATS_SAMPLE_PAGES))
    create_info->stats_sample_pages= table->s->stats_sample_pages;

  if (!(used_fields & HA_CREATE_USED_STATS_AUTO_RECALC))
    create_info->stats_auto_recalc= table->s->stats_auto_recalc;

  if (!(used_fields & HA_CREATE_USED_TRANSACTIONAL))
    create_info->transactional= table->s->transactional;

  if (!(used_fields & HA_CREATE_USED_CONNECTION))
    create_info->connect_string= table->s->connect_string;

  if (!(used_fields & HA_CREATE_USED_SEQUENCE))
    create_info->sequence= table->s->table_type == TABLE_TYPE_SEQUENCE;

  column_rename_param.db_name=       table->s->db;
  column_rename_param.table_name=    table->s->table_name;
  if (column_rename_param.fields.copy(&alter_info->create_list, thd->mem_root))
    DBUG_RETURN(1);                             // OOM

  restore_record(table, s->default_values);     // Empty record for DEFAULT

  if ((create_info->fields_option_struct= (ha_field_option_struct**)
         thd->calloc(sizeof(void*) * table->s->fields)) == NULL ||
      (create_info->indexes_option_struct= (ha_index_option_struct**)
         thd->calloc(sizeof(void*) * table->s->keys)) == NULL)
    DBUG_RETURN(1);

  create_info->option_list= merge_engine_table_options(table->s->option_list,
                                        create_info->option_list, thd->mem_root);

  /*
    First collect all fields from table which isn't in drop_list
  */
  bitmap_clear_all(&table->tmp_set);
  for (f_ptr=table->field ; (field= *f_ptr) ; f_ptr++)
  {
    if (field->invisible == INVISIBLE_FULL)
        continue;
    Alter_drop *drop;
    if (field->type() == MYSQL_TYPE_VARCHAR)
      create_info->varchar= TRUE;
    /* Check if field should be dropped */
    drop_it.rewind();
    while ((drop=drop_it++))
    {
      if (drop->type == Alter_drop::COLUMN &&
          !my_strcasecmp(system_charset_info,field->field_name.str, drop->name))
        break;
    }
    /*
      DROP COLULMN xxx
      1. it does not see INVISIBLE_SYSTEM columns
      2. otherwise, normally a column is dropped
      3. unless it's a system versioning column (but see below).
    */
    if (drop && field->invisible < INVISIBLE_SYSTEM &&
        !(field->flags & VERS_SYSTEM_FIELD &&
          !(alter_info->flags & ALTER_DROP_SYSTEM_VERSIONING)))
    {
      /* Reset auto_increment value if it was dropped */
      if (MTYP_TYPENR(field->unireg_check) == Field::NEXT_NUMBER &&
          !(used_fields & HA_CREATE_USED_AUTO))
      {
        create_info->auto_increment_value=0;
        create_info->used_fields|=HA_CREATE_USED_AUTO;
      }
      if (table->s->tmp_table == NO_TMP_TABLE)
        (void) delete_statistics_for_column(thd, table, field);
      dropped_sys_vers_fields|= field->flags;
      drop_it.remove();
      dropped_fields= &table->tmp_set;
      bitmap_set_bit(dropped_fields, field->field_index);
      continue;
    }
    if (field->invisible == INVISIBLE_SYSTEM &&
        field->flags & VERS_SYSTEM_FIELD)
    {
      vers_system_invisible= true;
    }
    /* invisible versioning column is dropped automatically on DROP SYSTEM VERSIONING */
    if (!drop && field->invisible >= INVISIBLE_SYSTEM &&
        field->flags & VERS_SYSTEM_FIELD &&
        alter_info->flags & ALTER_DROP_SYSTEM_VERSIONING)
    {
      if (table->s->tmp_table == NO_TMP_TABLE)
        (void) delete_statistics_for_column(thd, table, field);
      continue;
    }

    /*
      If we are doing a rename of a column, update all references in virtual
      column expressions, constraints and defaults to use the new column name
    */
    if (alter_info->flags & ALTER_RENAME_COLUMN)
    {
      if (field->vcol_info)
        field->vcol_info->expr->walk(&Item::rename_fields_processor, 1,
                                     &column_rename_param);
      if (field->check_constraint)
        field->check_constraint->expr->walk(&Item::rename_fields_processor, 1,
                                            &column_rename_param);
      if (field->default_value)
        field->default_value->expr->walk(&Item::rename_fields_processor, 1,
                                         &column_rename_param);
      // Force reopen because new column name is on thd->mem_root
      table->mark_table_for_reopen();
    }

    /* Check if field is changed */
    def_it.rewind();
    while ((def=def_it++))
    {
      if (def->change.str &&
	  !lex_string_cmp(system_charset_info, &field->field_name,
                          &def->change))
	break;
    }
    if (def && field->invisible < INVISIBLE_SYSTEM)
    {						// Field is changed
      def->field=field;
      /*
        Add column being updated to the list of new columns.
        Note that columns with AFTER clauses are added to the end
        of the list for now. Their positions will be corrected later.
      */
      new_create_list.push_back(def, thd->mem_root);
      if (field->stored_in_db() != def->stored_in_db())
      {
        my_error(ER_UNSUPPORTED_ACTION_ON_GENERATED_COLUMN, MYF(0));
        goto err;
      }
      if (!def->after.str)
      {
        /*
          If this ALTER TABLE doesn't have an AFTER clause for the modified
          column then remove this column from the list of columns to be
          processed. So later we can iterate over the columns remaining
          in this list and process modified columns with AFTER clause or
          add new columns.
        */
	def_it.remove();
      }
    }
    else if (alter_info->flags & ALTER_DROP_SYSTEM_VERSIONING &&
             field->flags & VERS_SYSTEM_FIELD &&
             field->invisible < INVISIBLE_SYSTEM)
    {
      StringBuffer<NAME_LEN*3> tmp;
      append_drop_column(thd, &tmp, field);
      my_error(ER_MISSING, MYF(0), table->s->table_name.str, tmp.c_ptr());
      goto err;
    }
    else if (drop && field->invisible < INVISIBLE_SYSTEM &&
             field->flags & VERS_SYSTEM_FIELD &&
             !(alter_info->flags & ALTER_DROP_SYSTEM_VERSIONING))
    {
      /* "dropping" a versioning field only hides it from the user */
      def= new (thd->mem_root) Create_field(thd, field, field);
      def->invisible= INVISIBLE_SYSTEM;
      alter_info->flags|= ALTER_CHANGE_COLUMN;
      if (field->flags & VERS_SYS_START_FLAG)
        create_info->vers_info.as_row.start= def->field_name= Vers_parse_info::default_start;
      else
        create_info->vers_info.as_row.end= def->field_name= Vers_parse_info::default_end;
      new_create_list.push_back(def, thd->mem_root);
      dropped_sys_vers_fields|= field->flags;
      drop_it.remove();
    }
    else
    {
      /*
        This field was not dropped and not changed, add it to the list
        for the new table.
      */
      def= new (thd->mem_root) Create_field(thd, field, field);
      new_create_list.push_back(def, thd->mem_root);
      alter_it.rewind();			// Change default if ALTER
      Alter_column *alter;
      while ((alter=alter_it++))
      {
	if (!my_strcasecmp(system_charset_info,field->field_name.str,
                           alter->name))
	  break;
      }
      if (alter)
      {
	if ((def->default_value= alter->default_value))
          def->flags&= ~NO_DEFAULT_VALUE_FLAG;
        else
          def->flags|= NO_DEFAULT_VALUE_FLAG;
	alter_it.remove();
      }
    }
  }
  dropped_sys_vers_fields &= VERS_SYSTEM_FIELD;
  if ((dropped_sys_vers_fields ||
       alter_info->flags & ALTER_DROP_PERIOD) &&
      dropped_sys_vers_fields != VERS_SYSTEM_FIELD &&
      !vers_system_invisible)
  {
    StringBuffer<NAME_LEN*3> tmp;
    if (!(dropped_sys_vers_fields & VERS_SYS_START_FLAG))
      append_drop_column(thd, &tmp, table->vers_start_field());
    if (!(dropped_sys_vers_fields & VERS_SYS_END_FLAG))
      append_drop_column(thd, &tmp, table->vers_end_field());
    my_error(ER_MISSING, MYF(0), table->s->table_name.str, tmp.c_ptr());
    goto err;
  }
  else if (alter_info->flags & ALTER_DROP_PERIOD && vers_system_invisible)
  {
    my_error(ER_CANT_DROP_FIELD_OR_KEY, MYF(0), "PERIOD FOR SYSTEM_TIME on", table->s->table_name.str);
    goto err;
  }
  alter_info->flags &= ~(ALTER_DROP_PERIOD | ALTER_ADD_PERIOD);
  def_it.rewind();
  while ((def=def_it++))			// Add new columns
  {
    Create_field *find;
    if (def->change.str && ! def->field)
    {
      /*
        Check if there is modify for newly added field.
      */
      find_it.rewind();
      while((find=find_it++))
      {
        if (!my_strcasecmp(system_charset_info,find->field_name.str,
                           def->field_name.str))
          break;
      }

      if (likely(find && !find->field))
	find_it.remove();
      else
      {
        my_error(ER_BAD_FIELD_ERROR, MYF(0), def->change.str,
                 table->s->table_name.str);
        goto err;
      }
    }
    /*
      Check that the DATE/DATETIME not null field we are going to add is
      either has a default value or the '0000-00-00' is allowed by the
      set sql mode.
      If the '0000-00-00' value isn't allowed then raise the error_if_not_empty
      flag to allow ALTER TABLE only if the table to be altered is empty.
    */
    if ((def->real_field_type() == MYSQL_TYPE_DATE ||
         def->real_field_type() == MYSQL_TYPE_NEWDATE ||
         def->real_field_type() == MYSQL_TYPE_DATETIME ||
         def->real_field_type() == MYSQL_TYPE_DATETIME2) &&
         !alter_ctx->datetime_field &&
         !(~def->flags & (NO_DEFAULT_VALUE_FLAG | NOT_NULL_FLAG)) &&
         thd->variables.sql_mode & MODE_NO_ZERO_DATE)
    {
        alter_ctx->datetime_field= def;
        alter_ctx->error_if_not_empty= TRUE;
    }
    if (!def->after.str)
      new_create_list.push_back(def, thd->mem_root);
    else
    {
      if (def->change.str)
      {
        find_it.rewind();
        /*
          For columns being modified with AFTER clause we should first remove
          these columns from the list and then add them back at their correct
          positions.
        */
        while ((find=find_it++))
        {
          /*
            Create_fields representing changed columns are added directly
            from Alter_info::create_list to new_create_list. We can therefore
            safely use pointer equality rather than name matching here.
            This prevents removing the wrong column in case of column rename.
          */
          if (find == def)
          {
            find_it.remove();
            break;
          }
        }
      }
      if (def->after.str == first_keyword)
        new_create_list.push_front(def, thd->mem_root);
      else
      {
        find_it.rewind();
        while ((find=find_it++))
        {
          if (!lex_string_cmp(system_charset_info, &def->after,
                              &find->field_name))
            break;
        }
        if (unlikely(!find))
        {
          my_error(ER_BAD_FIELD_ERROR, MYF(0), def->after.str,
                   table->s->table_name.str);
          goto err;
        }
        find_it.after(def);			// Put column after this
      }
    }
    /*
      Check if there is alter for newly added field.
    */
    alter_it.rewind();
    Alter_column *alter;
    while ((alter=alter_it++))
    {
      if (!my_strcasecmp(system_charset_info,def->field_name.str,
                         alter->name))
        break;
    }
    if (alter)
    {
      if ((def->default_value= alter->default_value)) // Use new default
        def->flags&= ~NO_DEFAULT_VALUE_FLAG;
      else
        def->flags|= NO_DEFAULT_VALUE_FLAG;
      alter_it.remove();
    }
  }
  if (unlikely(alter_info->alter_list.elements))
  {
    my_error(ER_BAD_FIELD_ERROR, MYF(0),
             alter_info->alter_list.head()->name, table->s->table_name.str);
    goto err;
  }
  if (unlikely(!new_create_list.elements))
  {
    my_message(ER_CANT_REMOVE_ALL_FIELDS,
               ER_THD(thd, ER_CANT_REMOVE_ALL_FIELDS),
               MYF(0));
    goto err;
  }

  /*
    Collect all keys which isn't in drop list. Add only those
    for which some fields exists.
  */
  for (uint i=0 ; i < table->s->keys ; i++,key_info++)
  {
    bool long_hash_key= false;
    if (key_info->flags & HA_INVISIBLE_KEY)
      continue;
    const char *key_name= key_info->name.str;
    Alter_drop *drop;
    drop_it.rewind();
    while ((drop=drop_it++))
    {
      if (drop->type == Alter_drop::KEY &&
	  !my_strcasecmp(system_charset_info,key_name, drop->name))
	break;
    }
    if (drop)
    {
      if (table->s->tmp_table == NO_TMP_TABLE)
      {
        (void) delete_statistics_for_index(thd, table, key_info, FALSE);
        if (i == table->s->primary_key)
	{
          KEY *tab_key_info= table->key_info;
	  for (uint j=0; j < table->s->keys; j++, tab_key_info++)
	  {
            if (tab_key_info->user_defined_key_parts !=
                tab_key_info->ext_key_parts)
	      (void) delete_statistics_for_index(thd, table, tab_key_info,
                                                 TRUE);
	  }
	}
      }  
      drop_it.remove();
      continue;
    }

    if (key_info->algorithm == HA_KEY_ALG_LONG_HASH)
    {
      setup_keyinfo_hash(key_info);
      long_hash_key= true;
    }
    const char *dropped_key_part= NULL;
    KEY_PART_INFO *key_part= key_info->key_part;
    key_parts.empty();
    bool delete_index_stat= FALSE;
    for (uint j=0 ; j < key_info->user_defined_key_parts ; j++,key_part++)
    {
      Field *kfield= key_part->field;
      if (!kfield)
	continue;				// Wrong field (from UNIREG)
      const char *key_part_name=kfield->field_name.str;
      Create_field *cfield;
      uint key_part_length;

      field_it.rewind();
      while ((cfield=field_it++))
      {
	if (cfield->change.str)
	{
	  if (!my_strcasecmp(system_charset_info, key_part_name,
			     cfield->change.str))
	    break;
	}
	else if (!my_strcasecmp(system_charset_info,
				key_part_name, cfield->field_name.str))
	  break;
      }
      if (!cfield)
      {
        if (table->s->primary_key == i)
          modified_primary_key= TRUE;
        delete_index_stat= TRUE;
        if (!(kfield->flags & VERS_SYSTEM_FIELD))
          dropped_key_part= key_part_name;
	continue;				// Field is removed
      }
      key_part_length= key_part->length;
      if (cfield->field)			// Not new field
      {
        /*
          If the field can't have only a part used in a key according to its
          new type, or should not be used partially according to its
          previous type, or the field length is less than the key part
          length, unset the key part length.

          We also unset the key part length if it is the same as the
          old field's length, so the whole new field will be used.

          BLOBs may have cfield->length == 0, which is why we test it before
          checking whether cfield->length < key_part_length (in chars).
          
          In case of TEXTs we check the data type maximum length *in bytes*
          to key part length measured *in characters* (i.e. key_part_length
          devided to mbmaxlen). This is because it's OK to have:
          CREATE TABLE t1 (a tinytext, key(a(254)) character set utf8);
          In case of this example:
          - data type maximum length is 255.
          - key_part_length is 1016 (=254*4, where 4 is mbmaxlen)
         */
        if (!cfield->field->type_handler()->type_can_have_key_part() ||
            !cfield->type_handler()->type_can_have_key_part() ||
            /* spatial keys can't have sub-key length */
            (key_info->flags & HA_SPATIAL) ||
            (cfield->field->field_length == key_part_length &&
             !f_is_blob(key_part->key_type)) ||
            (cfield->length &&
             (((cfield->real_field_type() >= MYSQL_TYPE_TINY_BLOB &&
                cfield->real_field_type() <= MYSQL_TYPE_BLOB) ?
                blob_length_by_type(cfield->real_field_type()) :
                cfield->length) <
	     key_part_length / kfield->charset()->mbmaxlen)))
	  key_part_length= 0;			// Use whole field
      }
      key_part_length /= kfield->charset()->mbmaxlen;
      key_parts.push_back(new (thd->mem_root) Key_part_spec(&cfield->field_name,
                                                            key_part_length, true),
                          thd->mem_root);
    }
    if (table->s->tmp_table == NO_TMP_TABLE)
    {
      if (delete_index_stat) 
        (void) delete_statistics_for_index(thd, table, key_info, FALSE);
      else if (modified_primary_key &&
               key_info->user_defined_key_parts != key_info->ext_key_parts)
        (void) delete_statistics_for_index(thd, table, key_info, TRUE);
    }

    if (key_parts.elements)
    {
      KEY_CREATE_INFO key_create_info;
      Key *key;
      enum Key::Keytype key_type;
      LEX_CSTRING tmp_name;
      bzero((char*) &key_create_info, sizeof(key_create_info));
      if (key_info->algorithm == HA_KEY_ALG_LONG_HASH)
        key_info->algorithm= HA_KEY_ALG_UNDEF;
      key_create_info.algorithm= key_info->algorithm;
      /*
        We copy block size directly as some engines, like Area, sets this
        automatically
      */
      key_create_info.block_size= key_info->block_size;
      key_create_info.flags=      key_info->flags;  // HA_USE_BLOCK_SIZE
      if (key_info->flags & HA_USES_PARSER)
        key_create_info.parser_name= *plugin_name(key_info->parser);
      if (key_info->flags & HA_USES_COMMENT)
        key_create_info.comment= key_info->comment;

      /*
        We're refreshing an already existing index. Since the index is not
        modified, there is no need to check for duplicate indexes again.
      */
      key_create_info.check_for_duplicate_indexes= false;

      if (key_info->flags & HA_SPATIAL)
        key_type= Key::SPATIAL;
      else if (key_info->flags & HA_NOSAME)
      {
        if (! my_strcasecmp(system_charset_info, key_name, primary_key_name))
          key_type= Key::PRIMARY;
        else
          key_type= Key::UNIQUE;
        if (dropped_key_part)
        {
          my_error(ER_KEY_COLUMN_DOES_NOT_EXITS, MYF(0), dropped_key_part);
          if (long_hash_key)
          {
            key_info->algorithm= HA_KEY_ALG_LONG_HASH;
            re_setup_keyinfo_hash(key_info);
          }
          goto err;
        }
      }
      else if (key_info->flags & HA_FULLTEXT)
        key_type= Key::FULLTEXT;
      else
        key_type= Key::MULTIPLE;

      tmp_name.str= key_name;
      tmp_name.length= strlen(key_name);
      /* We dont need LONG_UNIQUE_HASH_FIELD flag because it will be autogenerated */
      key= new (thd->mem_root) Key(key_type, &tmp_name, &key_create_info,
                   MY_TEST(key_info->flags & HA_GENERATED_KEY),
                   &key_parts, key_info->option_list, DDL_options());
      new_key_list.push_back(key, thd->mem_root);
    }
    if (long_hash_key)
    {
      key_info->algorithm= HA_KEY_ALG_LONG_HASH;
      re_setup_keyinfo_hash(key_info);
    }
  }
  {
    Key *key;
    while ((key=key_it++))			// Add new keys
    {
      if (key->type == Key::FOREIGN_KEY &&
          ((Foreign_key *)key)->validate(new_create_list))
        goto err;
      new_key_list.push_back(key, thd->mem_root);
      if (key->name.str &&
	  !my_strcasecmp(system_charset_info, key->name.str, primary_key_name))
      {
	my_error(ER_WRONG_NAME_FOR_INDEX, MYF(0), key->name.str);
        goto err;
      }
    }
  }

  if (table->s->period.name)
  {
    drop_it.rewind();
    Alter_drop *drop;
    for (bool found= false; !found && (drop= drop_it++); )
    {
      found= drop->type == Alter_drop::PERIOD &&
             table->s->period.name.streq(drop->name);
    }

    if (drop)
    {
      drop_period= true;
      drop_it.remove();
    }
    else if (create_info->period_info.is_set() && table->s->period.name)
    {
      my_error(ER_MORE_THAN_ONE_PERIOD, MYF(0));
      goto err;
    }
    else
    {
      Field *s= table->s->period.start_field(table->s);
      Field *e= table->s->period.end_field(table->s);
      create_info->period_info.set_period(s->field_name, e->field_name);
      create_info->period_info.name= table->s->period.name;
    }
  }

  /* Add all table level constraints which are not in the drop list */
  if (table->s->table_check_constraints)
  {
    TABLE_SHARE *share= table->s;

    for (uint i= share->field_check_constraints;
         i < share->table_check_constraints ; i++)
    {
      Virtual_column_info *check= table->check_constraints[i];
      Alter_drop *drop;
      bool keep= true;
      drop_it.rewind();
      while ((drop=drop_it++))
      {
        if (drop->type == Alter_drop::CHECK_CONSTRAINT &&
            !my_strcasecmp(system_charset_info, check->name.str, drop->name))
        {
          drop_it.remove();
          keep= false;
          break;
        }
      }

      if (share->period.constr_name.streq(check->name.str))
      {
        if (!drop_period && !keep)
        {
          my_error(ER_PERIOD_CONSTRAINT_DROP, MYF(0), check->name.str,
                   share->period.name.str);
          goto err;
        }
        keep= keep && !drop_period;

        DBUG_ASSERT(create_info->period_info.constr == NULL || drop_period);

        if (keep)
        {
          Item *expr_copy= check->expr->get_copy(thd);
          check= new Virtual_column_info();
          check->name= share->period.constr_name;
          check->automatic_name= true;
          check->expr= expr_copy;
          create_info->period_info.constr= check;
        }
      }
      /* see if the constraint depends on *only* on dropped fields */
      if (keep && dropped_fields)
      {
        table->default_column_bitmaps();
        bitmap_clear_all(table->read_set);
        check->expr->walk(&Item::register_field_in_read_map, 1, 0);
        if (bitmap_is_subset(table->read_set, dropped_fields))
          keep= false;
        else if (bitmap_is_overlapping(dropped_fields, table->read_set))
        {
          bitmap_intersect(table->read_set, dropped_fields);
          uint field_nr= bitmap_get_first_set(table->read_set);
          my_error(ER_BAD_FIELD_ERROR, MYF(0),
                   table->field[field_nr]->field_name.str, "CHECK");
          goto err;
        }
      }
      if (keep)
      {
        if (alter_info->flags & ALTER_RENAME_COLUMN)
        {
          check->expr->walk(&Item::rename_fields_processor, 1,
                            &column_rename_param);
          // Force reopen because new column name is on thd->mem_root
          table->mark_table_for_reopen();
        }
        new_constraint_list.push_back(check, thd->mem_root);
      }
    }
  }

  if (!alter_info->check_constraint_list.is_empty())
  {
    /* Check the table FOREIGN KEYs for name duplications. */
    List <FOREIGN_KEY_INFO> fk_child_key_list;
    FOREIGN_KEY_INFO *f_key;
    table->file->get_foreign_key_list(thd, &fk_child_key_list);
    List_iterator<FOREIGN_KEY_INFO> fk_key_it(fk_child_key_list);
    while ((f_key= fk_key_it++))
    {
      List_iterator_fast<Virtual_column_info>
        c_it(alter_info->check_constraint_list);
      Virtual_column_info *check;
      while ((check= c_it++))
      {
        if (!check->name.length || check->automatic_name)
          continue;

        if (check->name.length == f_key->foreign_id->length &&
            my_strcasecmp(system_charset_info, f_key->foreign_id->str,
                          check->name.str) == 0)
        {
          my_error(ER_DUP_CONSTRAINT_NAME, MYF(0), "CHECK", check->name.str);
          goto err;
        }
      }
    }
  }

  /* Add new constraints */
  new_constraint_list.append(&alter_info->check_constraint_list);

  if (alter_info->drop_list.elements)
  {
    Alter_drop *drop;
    drop_it.rewind();
    while ((drop=drop_it++)) {
      switch (drop->type) {
      case Alter_drop::KEY:
      case Alter_drop::COLUMN:
      case Alter_drop::CHECK_CONSTRAINT:
      case Alter_drop::PERIOD:
        my_error(ER_CANT_DROP_FIELD_OR_KEY, MYF(0), drop->type_name(),
                 alter_info->drop_list.head()->name);
        goto err;
      case Alter_drop::FOREIGN_KEY:
        // Leave the DROP FOREIGN KEY names in the alter_info->drop_list.
        break;
      }
    }
  }

  if (!create_info->comment.str)
  {
    create_info->comment.str= table->s->comment.str;
    create_info->comment.length= table->s->comment.length;
  }

  table->file->update_create_info(create_info);
  if ((create_info->table_options &
       (HA_OPTION_PACK_KEYS | HA_OPTION_NO_PACK_KEYS)) ||
      (used_fields & HA_CREATE_USED_PACK_KEYS))
    db_create_options&= ~(HA_OPTION_PACK_KEYS | HA_OPTION_NO_PACK_KEYS);
  if ((create_info->table_options &
       (HA_OPTION_STATS_PERSISTENT | HA_OPTION_NO_STATS_PERSISTENT)) ||
      (used_fields & HA_CREATE_USED_STATS_PERSISTENT))
    db_create_options&= ~(HA_OPTION_STATS_PERSISTENT | HA_OPTION_NO_STATS_PERSISTENT);

  if (create_info->table_options &
      (HA_OPTION_CHECKSUM | HA_OPTION_NO_CHECKSUM))
    db_create_options&= ~(HA_OPTION_CHECKSUM | HA_OPTION_NO_CHECKSUM);
  if (create_info->table_options &
      (HA_OPTION_DELAY_KEY_WRITE | HA_OPTION_NO_DELAY_KEY_WRITE))
    db_create_options&= ~(HA_OPTION_DELAY_KEY_WRITE |
			  HA_OPTION_NO_DELAY_KEY_WRITE);
  create_info->table_options|= db_create_options;

  if (table->s->tmp_table)
    create_info->options|=HA_LEX_CREATE_TMP_TABLE;

  rc= FALSE;
  alter_info->create_list.swap(new_create_list);
  alter_info->key_list.swap(new_key_list);
  alter_info->check_constraint_list.swap(new_constraint_list);
err:
  DBUG_RETURN(rc);
}


/**
  Get Create_field object for newly created table by its name
  in the old version of table.

  @param alter_info  Alter_info describing newly created table.
  @param old_name    Name of field in old table.

  @returns Pointer to Create_field object, NULL - if field is
           not present in new version of table.
*/

static Create_field *get_field_by_old_name(Alter_info *alter_info,
                                           const char *old_name)
{
  List_iterator_fast<Create_field> new_field_it(alter_info->create_list);
  Create_field *new_field;

  while ((new_field= new_field_it++))
  {
    if (new_field->field &&
        (my_strcasecmp(system_charset_info,
                       new_field->field->field_name.str,
                       old_name) == 0))
      break;
  }
  return new_field;
}


/** Type of change to foreign key column, */

enum fk_column_change_type
{
  FK_COLUMN_NO_CHANGE, FK_COLUMN_DATA_CHANGE,
  FK_COLUMN_RENAMED, FK_COLUMN_DROPPED
};

/**
  Check that ALTER TABLE's changes on columns of a foreign key are allowed.

  @param[in]   thd              Thread context.
  @param[in]   alter_info       Alter_info describing changes to be done
                                by ALTER TABLE.
  @param[in]   fk_columns       List of columns of the foreign key to check.
  @param[out]  bad_column_name  Name of field on which ALTER TABLE tries to
                                do prohibited operation.

  @note This function takes into account value of @@foreign_key_checks
        setting.

  @retval FK_COLUMN_NO_CHANGE    No significant changes are to be done on
                                 foreign key columns.
  @retval FK_COLUMN_DATA_CHANGE  ALTER TABLE might result in value
                                 change in foreign key column (and
                                 foreign_key_checks is on).
  @retval FK_COLUMN_RENAMED      Foreign key column is renamed.
  @retval FK_COLUMN_DROPPED      Foreign key column is dropped.
*/

static enum fk_column_change_type
fk_check_column_changes(THD *thd, Alter_info *alter_info,
                        List<LEX_CSTRING> &fk_columns,
                        const char **bad_column_name)
{
  List_iterator_fast<LEX_CSTRING> column_it(fk_columns);
  LEX_CSTRING *column;

  *bad_column_name= NULL;

  while ((column= column_it++))
  {
    Create_field *new_field= get_field_by_old_name(alter_info, column->str);

    if (new_field)
    {
      Field *old_field= new_field->field;

      if (lex_string_cmp(system_charset_info, &old_field->field_name,
                         &new_field->field_name))
      {
        /*
          Copy algorithm doesn't support proper renaming of columns in
          the foreign key yet. At the moment we lack API which will tell
          SE that foreign keys should be updated to use new name of column
          like it happens in case of in-place algorithm.
        */
        *bad_column_name= column->str;
        return FK_COLUMN_RENAMED;
      }

      if ((old_field->is_equal(*new_field) == IS_EQUAL_NO) ||
          ((new_field->flags & NOT_NULL_FLAG) &&
           !(old_field->flags & NOT_NULL_FLAG)))
      {
        if (!(thd->variables.option_bits & OPTION_NO_FOREIGN_KEY_CHECKS))
        {
          /*
            Column in a FK has changed significantly. Unless
            foreign_key_checks are off we prohibit this since this
            means values in this column might be changed by ALTER
            and thus referential integrity might be broken,
          */
          *bad_column_name= column->str;
          return FK_COLUMN_DATA_CHANGE;
        }
      }
    }
    else
    {
      /*
        Column in FK was dropped. Most likely this will break
        integrity constraints of InnoDB data-dictionary (and thus
        InnoDB will emit an error), so we prohibit this right away
        even if foreign_key_checks are off.
        This also includes a rare case when another field replaces
        field being dropped since it is easy to break referential
        integrity in this case.
      */
      *bad_column_name= column->str;
      return FK_COLUMN_DROPPED;
    }
  }

  return FK_COLUMN_NO_CHANGE;
}


/**
  Check if ALTER TABLE we are about to execute using COPY algorithm
  is not supported as it might break referential integrity.

  @note If foreign_key_checks is disabled (=0), we allow to break
        referential integrity. But we still disallow some operations
        like dropping or renaming columns in foreign key since they
        are likely to break consistency of InnoDB data-dictionary
        and thus will end-up in error anyway.

  @param[in]  thd          Thread context.
  @param[in]  table        Table to be altered.
  @param[in]  alter_info   Lists of fields, keys to be changed, added
                           or dropped.
  @param[out] alter_ctx    ALTER TABLE runtime context.
                           Alter_table_ctx::fk_error_if_delete flag
                           is set if deletion during alter can break
                           foreign key integrity.

  @retval false  Success.
  @retval true   Error, ALTER - tries to do change which is not compatible
                 with foreign key definitions on the table.
*/

static bool fk_prepare_copy_alter_table(THD *thd, TABLE *table,
                                        Alter_info *alter_info,
                                        Alter_table_ctx *alter_ctx)
{
  List <FOREIGN_KEY_INFO> fk_parent_key_list;
  List <FOREIGN_KEY_INFO> fk_child_key_list;
  FOREIGN_KEY_INFO *f_key;

  DBUG_ENTER("fk_prepare_copy_alter_table");

  table->file->get_parent_foreign_key_list(thd, &fk_parent_key_list);

  /* OOM when building list. */
  if (unlikely(thd->is_error()))
    DBUG_RETURN(true);

  /*
    Remove from the list all foreign keys in which table participates as
    parent which are to be dropped by this ALTER TABLE. This is possible
    when a foreign key has the same table as child and parent.
  */
  List_iterator<FOREIGN_KEY_INFO> fk_parent_key_it(fk_parent_key_list);

  while ((f_key= fk_parent_key_it++))
  {
    Alter_drop *drop;
    List_iterator_fast<Alter_drop> drop_it(alter_info->drop_list);

    while ((drop= drop_it++))
    {
      /*
        InnoDB treats foreign key names in case-insensitive fashion.
        So we do it here too. For database and table name type of
        comparison used depends on lower-case-table-names setting.
        For l_c_t_n = 0 we use case-sensitive comparison, for
        l_c_t_n > 0 modes case-insensitive comparison is used.
      */
      if ((drop->type == Alter_drop::FOREIGN_KEY) &&
          (my_strcasecmp(system_charset_info, f_key->foreign_id->str,
                         drop->name) == 0) &&
          (lex_string_cmp(table_alias_charset, f_key->foreign_db,
                          &table->s->db) == 0) &&
          (lex_string_cmp(table_alias_charset, f_key->foreign_table,
                          &table->s->table_name) == 0))
        fk_parent_key_it.remove();
    }
  }

  /*
    If there are FKs in which this table is parent which were not
    dropped we need to prevent ALTER deleting rows from the table,
    as it might break referential integrity. OTOH it is OK to do
    so if foreign_key_checks are disabled.
  */
  if (!fk_parent_key_list.is_empty() &&
      !(thd->variables.option_bits & OPTION_NO_FOREIGN_KEY_CHECKS))
    alter_ctx->set_fk_error_if_delete_row(fk_parent_key_list.head());

  fk_parent_key_it.rewind();
  while ((f_key= fk_parent_key_it++))
  {
    enum fk_column_change_type changes;
    const char *bad_column_name;

    changes= fk_check_column_changes(thd, alter_info,
                                     f_key->referenced_fields,
                                     &bad_column_name);

    switch(changes)
    {
    case FK_COLUMN_NO_CHANGE:
      /* No significant changes. We can proceed with ALTER! */
      break;
    case FK_COLUMN_DATA_CHANGE:
    {
      char buff[NAME_LEN*2+2];
      strxnmov(buff, sizeof(buff)-1, f_key->foreign_db->str, ".",
               f_key->foreign_table->str, NullS);
      my_error(ER_FK_COLUMN_CANNOT_CHANGE_CHILD, MYF(0), bad_column_name,
               f_key->foreign_id->str, buff);
      DBUG_RETURN(true);
    }
    case FK_COLUMN_RENAMED:
      my_error(ER_ALTER_OPERATION_NOT_SUPPORTED_REASON, MYF(0),
               "ALGORITHM=COPY",
               ER_THD(thd, ER_ALTER_OPERATION_NOT_SUPPORTED_REASON_FK_RENAME),
               "ALGORITHM=INPLACE");
      DBUG_RETURN(true);
    case FK_COLUMN_DROPPED:
    {
      StringBuffer<NAME_LEN*2+2> buff(system_charset_info);
      LEX_CSTRING *db= f_key->foreign_db, *tbl= f_key->foreign_table;

      append_identifier(thd, &buff, db);
      buff.append('.');
      append_identifier(thd, &buff, tbl);
      my_error(ER_FK_COLUMN_CANNOT_DROP_CHILD, MYF(0), bad_column_name,
               f_key->foreign_id->str, buff.c_ptr());
      DBUG_RETURN(true);
    }
    default:
      DBUG_ASSERT(0);
    }
  }

  table->file->get_foreign_key_list(thd, &fk_child_key_list);

  /* OOM when building list. */
  if (unlikely(thd->is_error()))
    DBUG_RETURN(true);

  /*
    Remove from the list all foreign keys which are to be dropped
    by this ALTER TABLE.
  */
  List_iterator<FOREIGN_KEY_INFO> fk_key_it(fk_child_key_list);

  while ((f_key= fk_key_it++))
  {
    Alter_drop *drop;
    List_iterator_fast<Alter_drop> drop_it(alter_info->drop_list);

    while ((drop= drop_it++))
    {
      /* Names of foreign keys in InnoDB are case-insensitive. */
      if ((drop->type == Alter_drop::FOREIGN_KEY) &&
          (my_strcasecmp(system_charset_info, f_key->foreign_id->str,
                         drop->name) == 0))
        fk_key_it.remove();
    }
  }

  fk_key_it.rewind();
  while ((f_key= fk_key_it++))
  {
    enum fk_column_change_type changes;
    const char *bad_column_name;

    changes= fk_check_column_changes(thd, alter_info,
                                     f_key->foreign_fields,
                                     &bad_column_name);

    switch(changes)
    {
    case FK_COLUMN_NO_CHANGE:
      /* No significant changes. We can proceed with ALTER! */
      break;
    case FK_COLUMN_DATA_CHANGE:
      my_error(ER_FK_COLUMN_CANNOT_CHANGE, MYF(0), bad_column_name,
               f_key->foreign_id->str);
      DBUG_RETURN(true);
    case FK_COLUMN_RENAMED:
      my_error(ER_ALTER_OPERATION_NOT_SUPPORTED_REASON, MYF(0),
               "ALGORITHM=COPY",
               ER_THD(thd, ER_ALTER_OPERATION_NOT_SUPPORTED_REASON_FK_RENAME),
               "ALGORITHM=INPLACE");
      DBUG_RETURN(true);
    case FK_COLUMN_DROPPED:
      my_error(ER_FK_COLUMN_CANNOT_DROP, MYF(0), bad_column_name,
               f_key->foreign_id->str);
      DBUG_RETURN(true);
    default:
      DBUG_ASSERT(0);
    }
  }

  /*
    Normally, an attempt to modify an FK parent table will cause
    FK children to be prelocked, so the table-being-altered cannot
    be modified by a cascade FK action, because ALTER holds a lock
    and prelocking will wait.

    But if a new FK is being added by this very ALTER, then the target
    table is not locked yet (it's a temporary table). So, we have to
    lock FK parents explicitly.
  */
  if (alter_info->flags & ALTER_ADD_FOREIGN_KEY)
  {
    List_iterator<Key> fk_list_it(alter_info->key_list);

    while (Key *key= fk_list_it++)
    {
      if (key->type != Key::FOREIGN_KEY)
        continue;

      Foreign_key *fk= static_cast<Foreign_key*>(key);
      char dbuf[NAME_LEN];
      char tbuf[NAME_LEN];
      const char *ref_db= (fk->ref_db.str ?
                           fk->ref_db.str :
                           alter_ctx->new_db.str);
      const char *ref_table= fk->ref_table.str;
      MDL_request mdl_request;

      if (lower_case_table_names)
      {
        strmake_buf(dbuf, ref_db);
        my_casedn_str(system_charset_info, dbuf);
        strmake_buf(tbuf, ref_table);
        my_casedn_str(system_charset_info, tbuf);
        ref_db= dbuf;
        ref_table= tbuf;
      }

      mdl_request.init(MDL_key::TABLE, ref_db, ref_table, MDL_SHARED_NO_WRITE,
                       MDL_TRANSACTION);
      if (thd->mdl_context.acquire_lock(&mdl_request,
                                        thd->variables.lock_wait_timeout))
        DBUG_RETURN(true);
    }
  }

  DBUG_RETURN(false);
}

/**
  Rename temporary table and/or turn indexes on/off without touching .FRM.
  Its a variant of simple_rename_or_index_change() to be used exclusively
  for temporary tables.

  @param thd            Thread handler
  @param table_list     TABLE_LIST for the table to change
  @param keys_onoff     ENABLE or DISABLE KEYS?
  @param alter_ctx      ALTER TABLE runtime context.

  @return Operation status
    @retval false           Success
    @retval true            Failure
*/
static bool
simple_tmp_rename_or_index_change(THD *thd, TABLE_LIST *table_list,
                                  Alter_info::enum_enable_or_disable keys_onoff,
                                  Alter_table_ctx *alter_ctx)
{
  DBUG_ENTER("simple_tmp_rename_or_index_change");

  TABLE *table= table_list->table;
  bool error= false;

  DBUG_ASSERT(table->s->tmp_table);

  if (keys_onoff != Alter_info::LEAVE_AS_IS)
  {
    THD_STAGE_INFO(thd, stage_manage_keys);
    error= alter_table_manage_keys(table, table->file->indexes_are_disabled(),
                                   keys_onoff);
  }

  if (likely(!error) && alter_ctx->is_table_renamed())
  {
    THD_STAGE_INFO(thd, stage_rename);

    /*
      If THD::rename_temporary_table() fails, there is no need to rename it
      back to the original name (unlike the case for non-temporary tables),
      as it was an allocation error and the table was not renamed.
    */
    error= thd->rename_temporary_table(table, &alter_ctx->new_db,
                                       &alter_ctx->new_alias);
  }

  if (likely(!error))
  {
    /*
      We do not replicate alter table statement on temporary tables under
      ROW-based replication.
    */
    if (!thd->is_current_stmt_binlog_format_row())
    {
      error= write_bin_log(thd, true, thd->query(), thd->query_length()) != 0;
    }
    if (likely(!error))
      my_ok(thd);
  }

  DBUG_RETURN(error);
}


/**
  Rename table and/or turn indexes on/off without touching .FRM

  @param thd            Thread handler
  @param table_list     TABLE_LIST for the table to change
  @param keys_onoff     ENABLE or DISABLE KEYS?
  @param alter_ctx      ALTER TABLE runtime context.

  @return Operation status
    @retval false           Success
    @retval true            Failure
*/

static bool
simple_rename_or_index_change(THD *thd, TABLE_LIST *table_list,
                              Alter_info::enum_enable_or_disable keys_onoff,
                              Alter_table_ctx *alter_ctx)
{
  TABLE *table= table_list->table;
  MDL_ticket *mdl_ticket= table->mdl_ticket;
  int error= 0;
  enum ha_extra_function extra_func= thd->locked_tables_mode
                                       ? HA_EXTRA_NOT_USED
                                       : HA_EXTRA_FORCE_REOPEN;
  DBUG_ENTER("simple_rename_or_index_change");

  if (keys_onoff != Alter_info::LEAVE_AS_IS)
  {
    if (wait_while_table_is_used(thd, table, extra_func))
      DBUG_RETURN(true);

    // It's now safe to take the table level lock.
    if (lock_tables(thd, table_list, alter_ctx->tables_opened, 0))
      DBUG_RETURN(true);

    THD_STAGE_INFO(thd, stage_manage_keys);
    error= alter_table_manage_keys(table,
                                   table->file->indexes_are_disabled(),
                                   keys_onoff);
  }

  if (likely(!error) && alter_ctx->is_table_renamed())
  {
    THD_STAGE_INFO(thd, stage_rename);
    handlerton *old_db_type= table->s->db_type();
    /*
      Then do a 'simple' rename of the table. First we need to close all
      instances of 'source' table.
      Note that if wait_while_table_is_used() returns error here (i.e. if
      this thread was killed) then it must be that previous step of
      simple rename did nothing and therefore we can safely return
      without additional clean-up.
    */
    if (wait_while_table_is_used(thd, table, extra_func))
      DBUG_RETURN(true);
    close_all_tables_for_name(thd, table->s, HA_EXTRA_PREPARE_FOR_RENAME,
                              NULL);

    if (mysql_rename_table(old_db_type, &alter_ctx->db, &alter_ctx->table_name,
                           &alter_ctx->new_db, &alter_ctx->new_alias, 0))
      error= -1;
    else if (Table_triggers_list::change_table_name(thd,
                                                 &alter_ctx->db,
                                                 &alter_ctx->alias,
                                                 &alter_ctx->table_name,
                                                 &alter_ctx->new_db,
                                                 &alter_ctx->new_alias))
    {
      (void) mysql_rename_table(old_db_type,
                                &alter_ctx->new_db, &alter_ctx->new_alias,
                                &alter_ctx->db, &alter_ctx->table_name,
                                NO_FK_CHECKS);
      error= -1;
    }
    /* Update stat tables last. This is to be able to handle rename of a stat table */
    if (error == 0)
      (void) rename_table_in_stat_tables(thd, &alter_ctx->db,
                                         &alter_ctx->table_name,
                                         &alter_ctx->new_db,
                                         &alter_ctx->new_alias);
  }

  if (likely(!error))
  {
    error= write_bin_log(thd, TRUE, thd->query(), thd->query_length());

    if (likely(!error))
      my_ok(thd);
  }
  table_list->table= NULL;                    // For query cache
  query_cache_invalidate3(thd, table_list, 0);

  if ((thd->locked_tables_mode == LTM_LOCK_TABLES ||
       thd->locked_tables_mode == LTM_PRELOCKED_UNDER_LOCK_TABLES))
  {
    /*
      Under LOCK TABLES we should adjust meta-data locks before finishing
      statement. Otherwise we can rely on them being released
      along with the implicit commit.
    */
    if (alter_ctx->is_table_renamed())
      thd->mdl_context.release_all_locks_for_name(mdl_ticket);
    else
      mdl_ticket->downgrade_lock(MDL_SHARED_NO_READ_WRITE);
  }
  DBUG_RETURN(error != 0);
}


static void cleanup_table_after_inplace_alter_keep_files(TABLE *table)
{
  TABLE_SHARE *share= table->s;
  closefrm(table);
  free_table_share(share);
}


static void cleanup_table_after_inplace_alter(TABLE *table)
{
  table->file->ha_create_partitioning_metadata(table->s->normalized_path.str, 0,
                                               CHF_DELETE_FLAG);
  deletefrm(table->s->normalized_path.str);
  cleanup_table_after_inplace_alter_keep_files(table);
}


static int create_table_for_inplace_alter(THD *thd,
                                          const Alter_table_ctx &alter_ctx,
                                          LEX_CUSTRING *frm,
                                          TABLE_SHARE *share,
                                          TABLE *table)
{
  init_tmp_table_share(thd, share, alter_ctx.new_db.str, 0,
                       alter_ctx.new_name.str, alter_ctx.get_tmp_path());
  if (share->init_from_binary_frm_image(thd, true, frm->str, frm->length) ||
      open_table_from_share(thd, share, &alter_ctx.new_name, 0,
                            EXTRA_RECORD, thd->open_options,
                            table, false))
  {
    free_table_share(share);
    deletefrm(alter_ctx.get_tmp_path());
    return 1;
  }
  if (table->internal_tables && open_and_lock_internal_tables(table, false))
  {
    cleanup_table_after_inplace_alter(table);
    return 1;
  }
  return 0;
}


/**
  Alter table

  @param thd              Thread handle
  @param new_db           If there is a RENAME clause
  @param new_name         If there is a RENAME clause
  @param create_info      Information from the parsing phase about new
                          table properties.
  @param table_list       The table to change.
  @param alter_info       Lists of fields, keys to be changed, added
                          or dropped.
  @param order_num        How many ORDER BY fields has been specified.
  @param order            List of fields to ORDER BY.
  @param ignore           Whether we have ALTER IGNORE TABLE

  @retval   true          Error
  @retval   false         Success

  This is a veery long function and is everything but the kitchen sink :)
  It is used to alter a table and not only by ALTER TABLE but also
  CREATE|DROP INDEX are mapped on this function.

  When the ALTER TABLE statement just does a RENAME or ENABLE|DISABLE KEYS,
  or both, then this function short cuts its operation by renaming
  the table and/or enabling/disabling the keys. In this case, the FRM is
  not changed, directly by mysql_alter_table. However, if there is a
  RENAME + change of a field, or an index, the short cut is not used.
  See how `create_list` is used to generate the new FRM regarding the
  structure of the fields. The same is done for the indices of the table.

  Altering a table can be done in two ways. The table can be modified
  directly using an in-place algorithm, or the changes can be done using
  an intermediate temporary table (copy). In-place is the preferred
  algorithm as it avoids copying table data. The storage engine
  selects which algorithm to use in check_if_supported_inplace_alter()
  based on information about the table changes from fill_alter_inplace_info().
*/

bool mysql_alter_table(THD *thd, const LEX_CSTRING *new_db,
                       const LEX_CSTRING *new_name,
                       HA_CREATE_INFO *create_info,
                       TABLE_LIST *table_list,
                       Alter_info *alter_info,
                       uint order_num, ORDER *order, bool ignore)
{
  DBUG_ENTER("mysql_alter_table");

  /*
    Check if we attempt to alter mysql.slow_log or
    mysql.general_log table and return an error if
    it is the case.
    TODO: this design is obsolete and will be removed.
  */
  int table_kind= check_if_log_table(table_list, FALSE, NullS);

  if (table_kind)
  {
    /* Disable alter of enabled log tables */
    if (logger.is_log_table_enabled(table_kind))
    {
      my_error(ER_BAD_LOG_STATEMENT, MYF(0), "ALTER");
      DBUG_RETURN(true);
    }

    /* Disable alter of log tables to unsupported engine */
    if ((create_info->used_fields & HA_CREATE_USED_ENGINE) &&
        (!create_info->db_type || /* unknown engine */
         !(create_info->db_type->flags & HTON_SUPPORT_LOG_TABLES)))
    {
    unsupported:
      my_error(ER_UNSUPORTED_LOG_ENGINE, MYF(0),
               hton_name(create_info->db_type)->str);
      DBUG_RETURN(true);
    }

    if (create_info->db_type == maria_hton &&
        create_info->transactional != HA_CHOICE_NO)
      goto unsupported;

#ifdef WITH_PARTITION_STORAGE_ENGINE
    if (alter_info->partition_flags & ALTER_PARTITION_INFO)
    {
      my_error(ER_WRONG_USAGE, MYF(0), "PARTITION", "log table");
      DBUG_RETURN(true);
    }
#endif
  }

  THD_STAGE_INFO(thd, stage_init_update);

  /*
    Code below can handle only base tables so ensure that we won't open a view.
    Note that RENAME TABLE the only ALTER clause which is supported for views
    has been already processed.
  */
  table_list->required_type= TABLE_TYPE_NORMAL;

  Alter_table_prelocking_strategy alter_prelocking_strategy;

  DEBUG_SYNC(thd, "alter_table_before_open_tables");
  uint tables_opened;

  thd->open_options|= HA_OPEN_FOR_ALTER;
  thd->mdl_backup_ticket= 0;
  bool error= open_tables(thd, &table_list, &tables_opened, 0,
                          &alter_prelocking_strategy);
  thd->open_options&= ~HA_OPEN_FOR_ALTER;

  TABLE *table= table_list->table;
  bool versioned= table && table->versioned();

  if (versioned)
  {
    if (handlerton *hton1= create_info->db_type)
    {
      handlerton *hton2= table->file->partition_ht();
      if (hton1 != hton2 &&
          (ha_check_storage_engine_flag(hton1, HTON_NATIVE_SYS_VERSIONING) ||
           ha_check_storage_engine_flag(hton2, HTON_NATIVE_SYS_VERSIONING)))
      {
        my_error(ER_VERS_ALTER_ENGINE_PROHIBITED, MYF(0), table_list->db.str,
                 table_list->table_name.str);
        DBUG_RETURN(true);
      }
    }
    if (alter_info->vers_prohibited(thd))
    {
      my_error(ER_VERS_ALTER_NOT_ALLOWED, MYF(0),
               table_list->db.str, table_list->table_name.str);
      DBUG_RETURN(true);
    }
  }

  DEBUG_SYNC(thd, "alter_opened_table");

#ifdef WITH_WSREP
  DBUG_EXECUTE_IF("sync.alter_opened_table",
                  {
                    const char act[]=
                      "now "
                      "wait_for signal.alter_opened_table";
                    DBUG_ASSERT(!debug_sync_set_action(thd,
                                                       STRING_WITH_LEN(act)));
                  };);
#endif // WITH_WSREP

  if (unlikely(error))
    DBUG_RETURN(true);

  table->use_all_columns();
  MDL_ticket *mdl_ticket= table->mdl_ticket;

  /*
    Prohibit changing of the UNION list of a non-temporary MERGE table
    under LOCK tables. It would be quite difficult to reuse a shrinked
    set of tables from the old table or to open a new TABLE object for
    an extended list and verify that they belong to locked tables.
  */
  if ((thd->locked_tables_mode == LTM_LOCK_TABLES ||
       thd->locked_tables_mode == LTM_PRELOCKED_UNDER_LOCK_TABLES) &&
      (create_info->used_fields & HA_CREATE_USED_UNION) &&
      (table->s->tmp_table == NO_TMP_TABLE))
  {
    my_error(ER_LOCK_OR_ACTIVE_TRANSACTION, MYF(0));
    DBUG_RETURN(true);
  }

  Alter_table_ctx alter_ctx(thd, table_list, tables_opened, new_db, new_name);

  MDL_request target_mdl_request;

  /* Check that we are not trying to rename to an existing table */
  if (alter_ctx.is_table_renamed())
  {
    if (table->s->tmp_table != NO_TMP_TABLE)
    {
      /*
        Check whether a temporary table exists with same requested new name.
        If such table exists, there must be a corresponding TABLE_SHARE in
        THD::all_temp_tables list.
      */
      if (thd->find_tmp_table_share(alter_ctx.new_db.str, alter_ctx.new_name.str))
      {
        my_error(ER_TABLE_EXISTS_ERROR, MYF(0), alter_ctx.new_alias.str);
        DBUG_RETURN(true);
      }
    }
    else
    {
      MDL_request_list mdl_requests;
      MDL_request target_db_mdl_request;

      target_mdl_request.init(MDL_key::TABLE,
                              alter_ctx.new_db.str, alter_ctx.new_name.str,
                              MDL_EXCLUSIVE, MDL_TRANSACTION);
      mdl_requests.push_front(&target_mdl_request);

      /*
        If we are moving the table to a different database, we also
        need IX lock on the database name so that the target database
        is protected by MDL while the table is moved.
      */
      if (alter_ctx.is_database_changed())
      {
        target_db_mdl_request.init(MDL_key::SCHEMA, alter_ctx.new_db.str, "",
                                   MDL_INTENTION_EXCLUSIVE,
                                   MDL_TRANSACTION);
        mdl_requests.push_front(&target_db_mdl_request);
      }

      /*
        Protection against global read lock must have been acquired when table
        to be altered was being opened.
      */
      DBUG_ASSERT(thd->mdl_context.is_lock_owner(MDL_key::BACKUP,
                                                 "", "",
                                                 MDL_BACKUP_DDL));

      if (thd->mdl_context.acquire_locks(&mdl_requests,
                                         thd->variables.lock_wait_timeout))
        DBUG_RETURN(true);

      DEBUG_SYNC(thd, "locked_table_name");
      /*
        Table maybe does not exist, but we got an exclusive lock
        on the name, now we can safely try to find out for sure.
      */
      if (ha_table_exists(thd, &alter_ctx.new_db, &alter_ctx.new_name))
      {
        /* Table will be closed in do_command() */
        my_error(ER_TABLE_EXISTS_ERROR, MYF(0), alter_ctx.new_alias.str);
        DBUG_RETURN(true);
      }
    }
  }

  if (!create_info->db_type)
  {
#ifdef WITH_PARTITION_STORAGE_ENGINE
    if (table->part_info &&
        create_info->used_fields & HA_CREATE_USED_ENGINE)
    {
      /*
        This case happens when the user specified
        ENGINE = x where x is a non-existing storage engine
        We set create_info->db_type to default_engine_type
        to ensure we don't change underlying engine type
        due to a erroneously given engine name.
      */
      create_info->db_type= table->part_info->default_engine_type;
    }
    else
#endif
      create_info->db_type= table->s->db_type();
  }

  if (check_engine(thd, alter_ctx.new_db.str, alter_ctx.new_name.str, create_info))
    DBUG_RETURN(true);

  if (create_info->vers_info.fix_alter_info(thd, alter_info, create_info, table))
  {
    DBUG_RETURN(true);
  }

  if ((create_info->db_type != table->s->db_type() ||
       (alter_info->partition_flags & ALTER_PARTITION_INFO)) &&
      !table->file->can_switch_engines())
  {
    my_error(ER_ROW_IS_REFERENCED, MYF(0));
    DBUG_RETURN(true);
  }

  /*
   If foreign key is added then check permission to access parent table.

   In function "check_fk_parent_table_access", create_info->db_type is used
   to identify whether engine supports FK constraint or not. Since
   create_info->db_type is set here, check to parent table access is delayed
   till this point for the alter operation.
  */
  if ((alter_info->flags & ALTER_ADD_FOREIGN_KEY) &&
      check_fk_parent_table_access(thd, create_info, alter_info, new_db->str))
    DBUG_RETURN(true);

  /*
    If this is an ALTER TABLE and no explicit row type specified reuse
    the table's row type.
    Note: this is the same as if the row type was specified explicitly.
  */
  if (create_info->row_type == ROW_TYPE_NOT_USED)
  {
    /* ALTER TABLE without explicit row type */
    create_info->row_type= table->s->row_type;
  }
  else
  {
    /* ALTER TABLE with specific row type */
    create_info->used_fields |= HA_CREATE_USED_ROW_FORMAT;
  }

  DBUG_PRINT("info", ("old type: %s  new type: %s",
             ha_resolve_storage_engine_name(table->s->db_type()),
             ha_resolve_storage_engine_name(create_info->db_type)));
  if (ha_check_storage_engine_flag(table->s->db_type(), HTON_ALTER_NOT_SUPPORTED))
  {
    DBUG_PRINT("info", ("doesn't support alter"));
    my_error(ER_ILLEGAL_HA, MYF(0), hton_name(table->s->db_type())->str,
             alter_ctx.db.str, alter_ctx.table_name.str);
    DBUG_RETURN(true);
  }

  if (ha_check_storage_engine_flag(create_info->db_type,
                                   HTON_ALTER_NOT_SUPPORTED))
  {
    DBUG_PRINT("info", ("doesn't support alter"));
    my_error(ER_ILLEGAL_HA, MYF(0), hton_name(create_info->db_type)->str,
             alter_ctx.new_db.str, alter_ctx.new_name.str);
    DBUG_RETURN(true);
  }

  if (table->s->tmp_table == NO_TMP_TABLE)
    mysql_audit_alter_table(thd, table_list);

  THD_STAGE_INFO(thd, stage_setup);

  if (alter_info->flags & ALTER_DROP_CHECK_CONSTRAINT)
  {
    /*
      ALTER TABLE DROP CONSTRAINT
      should be replaced with ... DROP [FOREIGN] KEY
      if the constraint is the FOREIGN KEY or UNIQUE one.
    */

    List_iterator<Alter_drop> drop_it(alter_info->drop_list);
    Alter_drop *drop;
    List <FOREIGN_KEY_INFO> fk_child_key_list;
    table->file->get_foreign_key_list(thd, &fk_child_key_list);

    alter_info->flags&= ~ALTER_DROP_CHECK_CONSTRAINT;

    while ((drop= drop_it++))
    {
      if (drop->type == Alter_drop::CHECK_CONSTRAINT)
      {
        {
          /* Test if there is a FOREIGN KEY with this name. */
          FOREIGN_KEY_INFO *f_key;
          List_iterator<FOREIGN_KEY_INFO> fk_key_it(fk_child_key_list);

          while ((f_key= fk_key_it++))
          {
            if (my_strcasecmp(system_charset_info, f_key->foreign_id->str,
                  drop->name) == 0)
            {
              drop->type= Alter_drop::FOREIGN_KEY;
              alter_info->flags|= ALTER_DROP_FOREIGN_KEY;
              goto do_continue;
            }
          }
        }

        {
          /* Test if there is an UNIQUE with this name. */
          uint n_key;

          for (n_key=0; n_key < table->s->keys; n_key++)
          {
            if ((table->key_info[n_key].flags & HA_NOSAME) &&
                my_strcasecmp(system_charset_info,
                              drop->name, table->key_info[n_key].name.str) == 0) // Merge todo: review '.str'
            {
              drop->type= Alter_drop::KEY;
              alter_info->flags|= ALTER_DROP_INDEX;
              goto do_continue;
            }
          }
        }
      }
      alter_info->flags|= ALTER_DROP_CHECK_CONSTRAINT;
do_continue:;
    }
  }

  if (handle_if_exists_options(thd, table, alter_info,
                               &create_info->period_info) ||
      fix_constraints_names(thd, &alter_info->check_constraint_list,
                            create_info))
    DBUG_RETURN(true);

  /*
    Look if we have to do anything at all.
    ALTER can become NOOP after handling
    the IF (NOT) EXISTS options.
  */
  if (alter_info->flags == 0 && alter_info->partition_flags == 0)
  {
    my_snprintf(alter_ctx.tmp_buff, sizeof(alter_ctx.tmp_buff),
                ER_THD(thd, ER_INSERT_INFO), 0L, 0L,
                thd->get_stmt_da()->current_statement_warn_count());
    my_ok(thd, 0L, 0L, alter_ctx.tmp_buff);

    /* We don't replicate alter table statement on temporary tables */
    if (table->s->tmp_table == NO_TMP_TABLE ||
        !thd->is_current_stmt_binlog_format_row())
    {
      if (write_bin_log(thd, true, thd->query(), thd->query_length()))
        DBUG_RETURN(true);
    }

    DBUG_RETURN(false);
  }

  /*
     Test if we are only doing RENAME or KEYS ON/OFF. This works
     as we are testing if flags == 0 above.
  */
  if (!(alter_info->flags & ~(ALTER_RENAME | ALTER_KEYS_ONOFF)) &&
      alter_info->partition_flags == 0 &&
      alter_info->algorithm(thd) !=
      Alter_info::ALTER_TABLE_ALGORITHM_COPY)   // No need to touch frm.
  {
    bool res;

    if (!table->s->tmp_table)
    {
      // This requires X-lock, no other lock levels supported.
      if (alter_info->requested_lock != Alter_info::ALTER_TABLE_LOCK_DEFAULT &&
          alter_info->requested_lock != Alter_info::ALTER_TABLE_LOCK_EXCLUSIVE)
      {
        my_error(ER_ALTER_OPERATION_NOT_SUPPORTED, MYF(0),
                 "LOCK=NONE/SHARED", "LOCK=EXCLUSIVE");
        DBUG_RETURN(true);
      }
      res= simple_rename_or_index_change(thd, table_list,
                                         alter_info->keys_onoff,
                                         &alter_ctx);
    }
    else
    {
      res= simple_tmp_rename_or_index_change(thd, table_list,
                                             alter_info->keys_onoff,
                                             &alter_ctx);
    }
    DBUG_RETURN(res);
  }

  /* We have to do full alter table. */

#ifdef WITH_PARTITION_STORAGE_ENGINE
  bool partition_changed= false;
  bool fast_alter_partition= false;
  {
    if (prep_alter_part_table(thd, table, alter_info, create_info,
                              &alter_ctx, &partition_changed,
                              &fast_alter_partition))
    {
      DBUG_RETURN(true);
    }
  }
#endif

  if (mysql_prepare_alter_table(thd, table, create_info, alter_info,
                                &alter_ctx))
  {
    DBUG_RETURN(true);
  }

  set_table_default_charset(thd, create_info, alter_ctx.db);

  if (create_info->check_fields(thd, alter_info,
                                table_list->table_name, table_list->db) ||
      create_info->fix_period_fields(thd, alter_info))
    DBUG_RETURN(true);

  if (!opt_explicit_defaults_for_timestamp)
    promote_first_timestamp_column(&alter_info->create_list);

#ifdef WITH_PARTITION_STORAGE_ENGINE
  if (fast_alter_partition)
  {
    /*
      ALGORITHM and LOCK clauses are generally not allowed by the
      parser for operations related to partitioning.
      The exceptions are ALTER_PARTITION_INFO and ALTER_PARTITION_REMOVE.
      For consistency, we report ER_ALTER_OPERATION_NOT_SUPPORTED here.
    */
    if (alter_info->requested_lock !=
        Alter_info::ALTER_TABLE_LOCK_DEFAULT)
    {
      my_error(ER_ALTER_OPERATION_NOT_SUPPORTED_REASON, MYF(0),
               "LOCK=NONE/SHARED/EXCLUSIVE",
               ER_THD(thd, ER_ALTER_OPERATION_NOT_SUPPORTED_REASON_PARTITION),
               "LOCK=DEFAULT");
      DBUG_RETURN(true);
    }
    else if (alter_info->algorithm(thd) !=
             Alter_info::ALTER_TABLE_ALGORITHM_DEFAULT)
    {
      my_error(ER_ALTER_OPERATION_NOT_SUPPORTED_REASON, MYF(0),
               "ALGORITHM=COPY/INPLACE",
               ER_THD(thd, ER_ALTER_OPERATION_NOT_SUPPORTED_REASON_PARTITION),
               "ALGORITHM=DEFAULT");
      DBUG_RETURN(true);
    }

    /*
      Upgrade from MDL_SHARED_UPGRADABLE to MDL_SHARED_NO_WRITE.
      Afterwards it's safe to take the table level lock.
    */
    if ((thd->mdl_context.upgrade_shared_lock(mdl_ticket, MDL_SHARED_NO_WRITE,
             thd->variables.lock_wait_timeout)) ||
        lock_tables(thd, table_list, alter_ctx.tables_opened, 0))
    {
      DBUG_RETURN(true);
    }

    // In-place execution of ALTER TABLE for partitioning.
    DBUG_RETURN(fast_alter_partition_table(thd, table, alter_info,
                                           create_info, table_list,
                                           &alter_ctx.db,
                                           &alter_ctx.table_name));
  }
#endif

  /*
    Use copy algorithm if:
    - old_alter_table system variable is set without in-place requested using
      the ALGORITHM clause.
    - Or if in-place is impossible for given operation.
    - Changes to partitioning which were not handled by fast_alter_part_table()
      needs to be handled using table copying algorithm unless the engine
      supports auto-partitioning as such engines can do some changes
      using in-place API.
  */
  if ((thd->variables.alter_algorithm == Alter_info::ALTER_TABLE_ALGORITHM_COPY &&
       alter_info->algorithm(thd) !=
       Alter_info::ALTER_TABLE_ALGORITHM_INPLACE)
      || is_inplace_alter_impossible(table, create_info, alter_info)
      || IF_PARTITIONING((partition_changed &&
          !(table->s->db_type()->partition_flags() & HA_USE_AUTO_PARTITION)), 0))
  {
    if (alter_info->algorithm(thd) ==
        Alter_info::ALTER_TABLE_ALGORITHM_INPLACE)
    {
      my_error(ER_ALTER_OPERATION_NOT_SUPPORTED, MYF(0),
               "ALGORITHM=INPLACE", "ALGORITHM=COPY");
      DBUG_RETURN(true);
    }
    alter_info->set_requested_algorithm(
      Alter_info::ALTER_TABLE_ALGORITHM_COPY);
  }

  /*
    ALTER TABLE ... ENGINE to the same engine is a common way to
    request table rebuild. Set ALTER_RECREATE flag to force table
    rebuild.
  */
  if (create_info->db_type == table->s->db_type() &&
      create_info->used_fields & HA_CREATE_USED_ENGINE)
    alter_info->flags|= ALTER_RECREATE;

  /*
    If the old table had partitions and we are doing ALTER TABLE ...
    engine= <new_engine>, the new table must preserve the original
    partitioning. This means that the new engine is still the
    partitioning engine, not the engine specified in the parser.
    This is discovered in prep_alter_part_table, which in such case
    updates create_info->db_type.
    It's therefore important that the assignment below is done
    after prep_alter_part_table.
  */
  handlerton *new_db_type= create_info->db_type;
  handlerton *old_db_type= table->s->db_type();
  TABLE *new_table= NULL;
  ha_rows copied=0,deleted=0;

  /*
    Handling of symlinked tables:
    If no rename:
      Create new data file and index file on the same disk as the
      old data and index files.
      Copy data.
      Rename new data file over old data file and new index file over
      old index file.
      Symlinks are not changed.

   If rename:
      Create new data file and index file on the same disk as the
      old data and index files.  Create also symlinks to point at
      the new tables.
      Copy data.
      At end, rename intermediate tables, and symlinks to intermediate
      table, to final table name.
      Remove old table and old symlinks

    If rename is made to another database:
      Create new tables in new database.
      Copy data.
      Remove old table and symlinks.
  */
  char index_file[FN_REFLEN], data_file[FN_REFLEN];

  if (!alter_ctx.is_database_changed())
  {
    if (create_info->index_file_name)
    {
      /* Fix index_file_name to have 'tmp_name' as basename */
      strmov(index_file, alter_ctx.tmp_name.str);
      create_info->index_file_name=fn_same(index_file,
                                           create_info->index_file_name,
                                           1);
    }
    if (create_info->data_file_name)
    {
      /* Fix data_file_name to have 'tmp_name' as basename */
      strmov(data_file, alter_ctx.tmp_name.str);
      create_info->data_file_name=fn_same(data_file,
                                          create_info->data_file_name,
                                          1);
    }
  }
  else
  {
    /* Ignore symlink if db is changed. */
    create_info->data_file_name=create_info->index_file_name=0;
  }

  DEBUG_SYNC(thd, "alter_table_before_create_table_no_lock");

  /*
    Create .FRM for new version of table with a temporary name.
    We don't log the statement, it will be logged later.

    Keep information about keys in newly created table as it
    will be used later to construct Alter_inplace_info object
    and by fill_alter_inplace_info() call.
  */
  KEY *key_info;
  uint key_count;
  /*
    Remember if the new definition has new VARCHAR column;
    create_info->varchar will be reset in create_table_impl()/
    mysql_prepare_create_table().
  */
  bool varchar= create_info->varchar;
  LEX_CUSTRING frm= {0,0};

  tmp_disable_binlog(thd);
  create_info->options|=HA_CREATE_TMP_ALTER;
  create_info->alias= alter_ctx.table_name;
  error= create_table_impl(thd, alter_ctx.db, alter_ctx.table_name,
                           alter_ctx.new_db, alter_ctx.tmp_name,
                           alter_ctx.get_tmp_path(),
                           thd->lex->create_info, create_info, alter_info,
                           C_ALTER_TABLE_FRM_ONLY, NULL,
                           &key_info, &key_count, &frm);
  reenable_binlog(thd);
  if (unlikely(error))
  {
    my_free(const_cast<uchar*>(frm.str));
    DBUG_RETURN(true);
  }

  /* Remember that we have not created table in storage engine yet. */
  bool no_ha_table= true;

  if (alter_info->algorithm(thd) != Alter_info::ALTER_TABLE_ALGORITHM_COPY)
  {
    Alter_inplace_info ha_alter_info(create_info, alter_info,
                                     key_info, key_count,
                                     IF_PARTITIONING(thd->work_part_info, NULL),
                                     ignore, alter_ctx.error_if_not_empty);
    TABLE_SHARE altered_share;
    TABLE altered_table;
    bool use_inplace= true;

    /* Fill the Alter_inplace_info structure. */
    if (fill_alter_inplace_info(thd, table, varchar, &ha_alter_info))
      goto err_new_table_cleanup;

    /*
      We can ignore ALTER_COLUMN_ORDER and instead check
      ALTER_STORED_COLUMN_ORDER & ALTER_VIRTUAL_COLUMN_ORDER. This
      is ok as ALTER_COLUMN_ORDER may be wrong if we use AFTER last_field
      ALTER_COLUMN_NAME is set if field really was renamed.
    */

    if (!(ha_alter_info.handler_flags &
          ~(ALTER_COLUMN_ORDER | ALTER_RENAME_COLUMN)))
    {
      /*
        No-op ALTER, no need to call handler API functions.

        If this code path is entered for an ALTER statement that
        should not be a real no-op, new handler flags should be added
        and fill_alter_inplace_info() adjusted.

        Note that we can end up here if an ALTER statement has clauses
        that cancel each other out (e.g. ADD/DROP identically index).

        Also note that we ignore the LOCK clause here.

        TODO don't create partitioning metadata in the first place
      */
      table->file->ha_create_partitioning_metadata(alter_ctx.get_tmp_path(),
                                                   NULL, CHF_DELETE_FLAG);
      my_free(const_cast<uchar*>(frm.str));
      goto end_inplace;
    }

    // We assume that the table is non-temporary.
    DBUG_ASSERT(!table->s->tmp_table);

    if (create_table_for_inplace_alter(thd, alter_ctx, &frm, &altered_share,
                                       &altered_table))
      goto err_new_table_cleanup;

    /* Set markers for fields in TABLE object for altered table. */
    update_altered_table(ha_alter_info, &altered_table);

    /*
      Mark all columns in 'altered_table' as used to allow usage
      of its record[0] buffer and Field objects during in-place
      ALTER TABLE.
    */
    altered_table.column_bitmaps_set_no_signal(&altered_table.s->all_set,
                                               &altered_table.s->all_set);
    restore_record(&altered_table, s->default_values); // Create empty record
    /* Check that we can call default functions with default field values */
    thd->count_cuted_fields= CHECK_FIELD_EXPRESSION;
    altered_table.reset_default_fields();
    if (altered_table.default_field &&
        altered_table.update_default_fields(true))
    {
      cleanup_table_after_inplace_alter(&altered_table);
      goto err_new_table_cleanup;
    }
    thd->count_cuted_fields= CHECK_FIELD_IGNORE;

    if (alter_info->requested_lock == Alter_info::ALTER_TABLE_LOCK_NONE)
      ha_alter_info.online= true;
    // Ask storage engine whether to use copy or in-place
    ha_alter_info.inplace_supported=
      table->file->check_if_supported_inplace_alter(&altered_table,
                                                    &ha_alter_info);

    if (alter_info->supports_algorithm(thd, &ha_alter_info) ||
        alter_info->supports_lock(thd, &ha_alter_info))
    {
      cleanup_table_after_inplace_alter(&altered_table);
      goto err_new_table_cleanup;
    }

    // If SHARED lock and no particular algorithm was requested, use COPY.
    if (ha_alter_info.inplace_supported == HA_ALTER_INPLACE_EXCLUSIVE_LOCK &&
        alter_info->requested_lock == Alter_info::ALTER_TABLE_LOCK_SHARED &&
         alter_info->algorithm(thd) ==
                 Alter_info::ALTER_TABLE_ALGORITHM_DEFAULT &&
         thd->variables.alter_algorithm ==
                 Alter_info::ALTER_TABLE_ALGORITHM_DEFAULT)
      use_inplace= false;

    if (ha_alter_info.inplace_supported == HA_ALTER_INPLACE_NOT_SUPPORTED)
      use_inplace= false;

    if (use_inplace)
    {
      table->s->frm_image= &frm;
      /*
        Set the truncated column values of thd as warning
        for alter table.
      */
      Check_level_instant_set check_level_save(thd, CHECK_FIELD_WARN);
      int res= mysql_inplace_alter_table(thd, table_list, table, &altered_table,
                                         &ha_alter_info,
                                         &target_mdl_request, &alter_ctx);
      my_free(const_cast<uchar*>(frm.str));

      if (res)
      {
        cleanup_table_after_inplace_alter(&altered_table);
        DBUG_RETURN(true);
      }
      cleanup_table_after_inplace_alter_keep_files(&altered_table);

      goto end_inplace;
    }
    else
      cleanup_table_after_inplace_alter_keep_files(&altered_table);
  }

  /* ALTER TABLE using copy algorithm. */

  /* Check if ALTER TABLE is compatible with foreign key definitions. */
  if (fk_prepare_copy_alter_table(thd, table, alter_info, &alter_ctx))
    goto err_new_table_cleanup;

  if (!table->s->tmp_table)
  {
    // COPY algorithm doesn't work with concurrent writes.
    if (alter_info->requested_lock == Alter_info::ALTER_TABLE_LOCK_NONE)
    {
      my_error(ER_ALTER_OPERATION_NOT_SUPPORTED_REASON, MYF(0),
               "LOCK=NONE",
               ER_THD(thd, ER_ALTER_OPERATION_NOT_SUPPORTED_REASON_COPY),
               "LOCK=SHARED");
      goto err_new_table_cleanup;
    }

    // If EXCLUSIVE lock is requested, upgrade already.
    if (alter_info->requested_lock == Alter_info::ALTER_TABLE_LOCK_EXCLUSIVE &&
        wait_while_table_is_used(thd, table, HA_EXTRA_FORCE_REOPEN))
      goto err_new_table_cleanup;

    /*
      Otherwise upgrade to SHARED_NO_WRITE.
      Note that under LOCK TABLES, we will already have SHARED_NO_READ_WRITE.
    */
    if (alter_info->requested_lock != Alter_info::ALTER_TABLE_LOCK_EXCLUSIVE &&
        thd->mdl_context.upgrade_shared_lock(mdl_ticket, MDL_SHARED_NO_WRITE,
                                             thd->variables.lock_wait_timeout))
      goto err_new_table_cleanup;

    DEBUG_SYNC(thd, "alter_table_copy_after_lock_upgrade");
  }
  else
    thd->close_unused_temporary_table_instances(table_list);

  // It's now safe to take the table level lock.
  if (lock_tables(thd, table_list, alter_ctx.tables_opened,
                  MYSQL_LOCK_USE_MALLOC))
    goto err_new_table_cleanup;

  if (ha_create_table(thd, alter_ctx.get_tmp_path(),
                      alter_ctx.new_db.str, alter_ctx.new_name.str,
                      create_info, &frm))
    goto err_new_table_cleanup;

  /* Mark that we have created table in storage engine. */
  no_ha_table= false;
  DEBUG_SYNC(thd, "alter_table_intermediate_table_created");

  /* Open the table since we need to copy the data. */
  new_table= thd->create_and_open_tmp_table(&frm,
                                            alter_ctx.get_tmp_path(),
                                            alter_ctx.new_db.str,
                                            alter_ctx.new_name.str,
                                            true);
  if (!new_table)
    goto err_new_table_cleanup;

  if (table->s->tmp_table != NO_TMP_TABLE)
  {
    /* in case of alter temp table send the tracker in OK packet */
    SESSION_TRACKER_CHANGED(thd, SESSION_STATE_CHANGE_TRACKER, NULL);
  }

  /*
    Note: In case of MERGE table, we do not attach children. We do not
    copy data for MERGE tables. Only the children have data.
  */

  /* Copy the data if necessary. */
  thd->count_cuted_fields= CHECK_FIELD_WARN;	// calc cuted fields
  thd->cuted_fields=0L;

  /*
    We do not copy data for MERGE tables. Only the children have data.
    MERGE tables have HA_NO_COPY_ON_ALTER set.
  */
  if (!(new_table->file->ha_table_flags() & HA_NO_COPY_ON_ALTER))
  {
    new_table->next_number_field=new_table->found_next_number_field;
    THD_STAGE_INFO(thd, stage_copy_to_tmp_table);
    DBUG_EXECUTE_IF("abort_copy_table", {
        my_error(ER_LOCK_WAIT_TIMEOUT, MYF(0));
        goto err_new_table_cleanup;
      });
    if (copy_data_between_tables(thd, table, new_table,
                                 alter_info->create_list, ignore,
                                 order_num, order, &copied, &deleted,
                                 alter_info->keys_onoff,
                                 &alter_ctx))
    {
      goto err_new_table_cleanup;
    }
  }
  else
  {
    if (!table->s->tmp_table &&
        wait_while_table_is_used(thd, table, HA_EXTRA_FORCE_REOPEN))
      goto err_new_table_cleanup;
    THD_STAGE_INFO(thd, stage_manage_keys);
    alter_table_manage_keys(table, table->file->indexes_are_disabled(),
                            alter_info->keys_onoff);
    if (trans_commit_stmt(thd) || trans_commit_implicit(thd))
      goto err_new_table_cleanup;
  }
  thd->count_cuted_fields= CHECK_FIELD_IGNORE;

  if (table->s->tmp_table != NO_TMP_TABLE)
  {
    /* Close lock if this is a transactional table */
    if (thd->lock)
    {
      if (thd->locked_tables_mode != LTM_LOCK_TABLES &&
          thd->locked_tables_mode != LTM_PRELOCKED_UNDER_LOCK_TABLES)
      {
        mysql_unlock_tables(thd, thd->lock);
        thd->lock= NULL;
      }
      else
      {
        /*
          If LOCK TABLES list is not empty and contains this table,
          unlock the table and remove the table from this list.
        */
        mysql_lock_remove(thd, thd->lock, table);
      }
    }
    new_table->s->table_creation_was_logged=
      table->s->table_creation_was_logged;
    /* Remove link to old table and rename the new one */
    thd->drop_temporary_table(table, NULL, true);
    /* Should pass the 'new_name' as we store table name in the cache */
    if (thd->rename_temporary_table(new_table, &alter_ctx.new_db,
                                    &alter_ctx.new_name))
      goto err_new_table_cleanup;
    /* We don't replicate alter table statement on temporary tables */
    if (!thd->is_current_stmt_binlog_format_row() &&
        write_bin_log(thd, true, thd->query(), thd->query_length()))
      DBUG_RETURN(true);
    my_free(const_cast<uchar*>(frm.str));
    goto end_temporary;
  }

  /*
    Close the intermediate table that will be the new table, but do
    not delete it! Even though MERGE tables do not have their children
    attached here it is safe to call THD::drop_temporary_table().
  */
  thd->drop_temporary_table(new_table, NULL, false);
  new_table= NULL;

  DEBUG_SYNC(thd, "alter_table_before_rename_result_table");

  /*
    Data is copied. Now we:
    1) Wait until all other threads will stop using old version of table
       by upgrading shared metadata lock to exclusive one.
    2) Close instances of table open by this thread and replace them
       with placeholders to simplify reopen process.
    3) Rename the old table to a temp name, rename the new one to the
       old name.
    4) If we are under LOCK TABLES and don't do ALTER TABLE ... RENAME
       we reopen new version of table.
    5) Write statement to the binary log.
    6) If we are under LOCK TABLES and do ALTER TABLE ... RENAME we
       remove placeholders and release metadata locks.
    7) If we are not not under LOCK TABLES we rely on the caller
      (mysql_execute_command()) to release metadata locks.
  */

  THD_STAGE_INFO(thd, stage_rename_result_table);

  if (wait_while_table_is_used(thd, table, HA_EXTRA_PREPARE_FOR_RENAME))
    goto err_new_table_cleanup;

  close_all_tables_for_name(thd, table->s,
                            alter_ctx.is_table_renamed() ?
                            HA_EXTRA_PREPARE_FOR_RENAME:
                            HA_EXTRA_NOT_USED,
                            NULL);
  table_list->table= table= NULL;                  /* Safety */
  my_free(const_cast<uchar*>(frm.str));

  /*
    Rename the old table to temporary name to have a backup in case
    anything goes wrong while renaming the new table.
  */
  char backup_name_buff[FN_LEN];
  LEX_CSTRING backup_name;
  backup_name.str= backup_name_buff;

  backup_name.length= my_snprintf(backup_name_buff, sizeof(backup_name_buff),
                                  "%s2-%lx-%lx", tmp_file_prefix,
                                    current_pid, (long) thd->thread_id);
  if (lower_case_table_names)
    my_casedn_str(files_charset_info, backup_name_buff);
  if (mysql_rename_table(old_db_type, &alter_ctx.db, &alter_ctx.table_name,
                         &alter_ctx.db, &backup_name, FN_TO_IS_TMP))
  {
    // Rename to temporary name failed, delete the new table, abort ALTER.
    (void) quick_rm_table(thd, new_db_type, &alter_ctx.new_db,
                          &alter_ctx.tmp_name, FN_IS_TMP);
    goto err_with_mdl;
  }

  // Rename the new table to the correct name.
  if (mysql_rename_table(new_db_type, &alter_ctx.new_db, &alter_ctx.tmp_name,
                         &alter_ctx.new_db, &alter_ctx.new_alias,
                         FN_FROM_IS_TMP))
  {
    // Rename failed, delete the temporary table.
    (void) quick_rm_table(thd, new_db_type, &alter_ctx.new_db,
                          &alter_ctx.tmp_name, FN_IS_TMP);

    // Restore the backup of the original table to the old name.
    (void) mysql_rename_table(old_db_type, &alter_ctx.db, &backup_name,
                              &alter_ctx.db, &alter_ctx.alias,
                              FN_FROM_IS_TMP | NO_FK_CHECKS);
    goto err_with_mdl;
  }

  // Check if we renamed the table and if so update trigger files.
  if (alter_ctx.is_table_renamed())
  {
    if (Table_triggers_list::change_table_name(thd,
                                               &alter_ctx.db,
                                               &alter_ctx.alias,
                                               &alter_ctx.table_name,
                                               &alter_ctx.new_db,
                                               &alter_ctx.new_alias))
    {
      // Rename succeeded, delete the new table.
      (void) quick_rm_table(thd, new_db_type,
                            &alter_ctx.new_db, &alter_ctx.new_alias, 0);
      // Restore the backup of the original table to the old name.
      (void) mysql_rename_table(old_db_type, &alter_ctx.db, &backup_name,
                                &alter_ctx.db, &alter_ctx.alias,
                                FN_FROM_IS_TMP | NO_FK_CHECKS);
      goto err_with_mdl;
    }
    rename_table_in_stat_tables(thd, &alter_ctx.db, &alter_ctx.alias,
                                &alter_ctx.new_db, &alter_ctx.new_alias);
  }

  // ALTER TABLE succeeded, delete the backup of the old table.
  if (quick_rm_table(thd, old_db_type, &alter_ctx.db, &backup_name, FN_IS_TMP))
  {
    /*
      The fact that deletion of the backup failed is not critical
      error, but still worth reporting as it might indicate serious
      problem with server.
    */
    goto err_with_mdl_after_alter;
  }

end_inplace:

  if (thd->locked_tables_list.reopen_tables(thd, false))
    goto err_with_mdl_after_alter;

  THD_STAGE_INFO(thd, stage_end);

  DEBUG_SYNC(thd, "alter_table_before_main_binlog");

  DBUG_ASSERT(!(mysql_bin_log.is_open() &&
                thd->is_current_stmt_binlog_format_row() &&
                (create_info->tmp_table())));
  if (write_bin_log(thd, true, thd->query(), thd->query_length()))
    DBUG_RETURN(true);

  table_list->table= NULL;			// For query cache
  query_cache_invalidate3(thd, table_list, false);

  if (thd->locked_tables_mode == LTM_LOCK_TABLES ||
      thd->locked_tables_mode == LTM_PRELOCKED_UNDER_LOCK_TABLES)
  {
    if (alter_ctx.is_table_renamed())
      thd->mdl_context.release_all_locks_for_name(mdl_ticket);
    else
      mdl_ticket->downgrade_lock(MDL_SHARED_NO_READ_WRITE);
  }

end_temporary:
  my_snprintf(alter_ctx.tmp_buff, sizeof(alter_ctx.tmp_buff),
              ER_THD(thd, ER_INSERT_INFO),
	      (ulong) (copied + deleted), (ulong) deleted,
	      (ulong) thd->get_stmt_da()->current_statement_warn_count());
  my_ok(thd, copied + deleted, 0L, alter_ctx.tmp_buff);
  DEBUG_SYNC(thd, "alter_table_inplace_trans_commit");
  DBUG_RETURN(false);

err_new_table_cleanup:
  my_free(const_cast<uchar*>(frm.str));
  /*
    No default value was provided for a DATE/DATETIME field, the
    current sql_mode doesn't allow the '0000-00-00' value and
    the table to be altered isn't empty.
    Report error here.
  */
  if (unlikely(alter_ctx.error_if_not_empty &&
               thd->get_stmt_da()->current_row_for_warning()))
  {
    const char *f_val= "0000-00-00";
    const char *f_type= "date";
    switch (alter_ctx.datetime_field->real_field_type())
    {
      case MYSQL_TYPE_DATE:
      case MYSQL_TYPE_NEWDATE:
        break;
      case MYSQL_TYPE_DATETIME:
      case MYSQL_TYPE_DATETIME2:
        f_val= "0000-00-00 00:00:00";
        f_type= "datetime";
        break;
      default:
        /* Shouldn't get here. */
        DBUG_ASSERT(0);
    }
    bool save_abort_on_warning= thd->abort_on_warning;
    thd->abort_on_warning= true;
    thd->push_warning_truncated_value_for_field(Sql_condition::WARN_LEVEL_WARN,
                                                f_type, f_val,
                                                alter_ctx.new_db.str,
                                                alter_ctx.new_name.str,
                                                alter_ctx.datetime_field->
                                                field_name.str);
    thd->abort_on_warning= save_abort_on_warning;
  }

  if (new_table)
  {
    thd->drop_temporary_table(new_table, NULL, true);
  }
  else
    (void) quick_rm_table(thd, new_db_type,
                          &alter_ctx.new_db, &alter_ctx.tmp_name,
                          (FN_IS_TMP | (no_ha_table ? NO_HA_TABLE : 0)),
                          alter_ctx.get_tmp_path());

  DBUG_RETURN(true);

err_with_mdl_after_alter:
  /* the table was altered. binlog the operation */
  DBUG_ASSERT(!(mysql_bin_log.is_open() &&
                thd->is_current_stmt_binlog_format_row() &&
                (create_info->tmp_table())));
  write_bin_log(thd, true, thd->query(), thd->query_length());

err_with_mdl:
  /*
    An error happened while we were holding exclusive name metadata lock
    on table being altered. To be safe under LOCK TABLES we should
    remove all references to the altered table from the list of locked
    tables and release the exclusive metadata lock.
  */
  thd->locked_tables_list.unlink_all_closed_tables(thd, NULL, 0);
  if (!table_list->table)
    thd->mdl_context.release_all_locks_for_name(mdl_ticket);
  DBUG_RETURN(true);
}



/**
  Prepare the transaction for the alter table's copy phase.
*/

bool mysql_trans_prepare_alter_copy_data(THD *thd)
{
  DBUG_ENTER("mysql_trans_prepare_alter_copy_data");
  /*
    Turn off recovery logging since rollback of an alter table is to
    delete the new table so there is no need to log the changes to it.

    This needs to be done before external_lock.
  */
  DBUG_RETURN(ha_enable_transaction(thd, FALSE) != 0);
}


/**
  Commit the copy phase of the alter table.
*/

bool mysql_trans_commit_alter_copy_data(THD *thd)
{
  bool error= FALSE;
  uint save_unsafe_rollback_flags;
  DBUG_ENTER("mysql_trans_commit_alter_copy_data");

  /* Save flags as trans_commit_implicit are deleting them */
  save_unsafe_rollback_flags= thd->transaction.stmt.m_unsafe_rollback_flags;

  DEBUG_SYNC(thd, "alter_table_copy_trans_commit");

  if (ha_enable_transaction(thd, TRUE))
    DBUG_RETURN(TRUE);

  /*
    Ensure that the new table is saved properly to disk before installing
    the new .frm.
    And that InnoDB's internal latches are released, to avoid deadlock
    when waiting on other instances of the table before rename (Bug#54747).
  */
  if (trans_commit_stmt(thd))
    error= TRUE;
  if (trans_commit_implicit(thd))
    error= TRUE;

  thd->transaction.stmt.m_unsafe_rollback_flags= save_unsafe_rollback_flags;
  DBUG_RETURN(error);
}


static int
copy_data_between_tables(THD *thd, TABLE *from, TABLE *to,
			 List<Create_field> &create, bool ignore,
			 uint order_num, ORDER *order,
			 ha_rows *copied, ha_rows *deleted,
                         Alter_info::enum_enable_or_disable keys_onoff,
                         Alter_table_ctx *alter_ctx)
{
  int error= 1;
  Copy_field *copy= NULL, *copy_end;
  ha_rows found_count= 0, delete_count= 0;
  SORT_INFO  *file_sort= 0;
  READ_RECORD info;
  TABLE_LIST   tables;
  List<Item>   fields;
  List<Item>   all_fields;
  bool auto_increment_field_copied= 0;
  bool cleanup_done= 0;
  bool init_read_record_done= 0;
  sql_mode_t save_sql_mode= thd->variables.sql_mode;
  ulonglong prev_insert_id, time_to_report_progress;
  Field **dfield_ptr= to->default_field;
  uint save_to_s_default_fields= to->s->default_fields;
  bool make_versioned= !from->versioned() && to->versioned();
  bool make_unversioned= from->versioned() && !to->versioned();
  bool keep_versioned= from->versioned() && to->versioned();
  bool bulk_insert_started= 0;
  Field *to_row_start= NULL, *to_row_end= NULL, *from_row_end= NULL;
  MYSQL_TIME query_start;
  DBUG_ENTER("copy_data_between_tables");

  /* Two or 3 stages; Sorting, copying data and update indexes */
  thd_progress_init(thd, 2 + MY_TEST(order));

  if (!(copy= new (thd->mem_root) Copy_field[to->s->fields]))
    DBUG_RETURN(-1);

  if (mysql_trans_prepare_alter_copy_data(thd))
  {
    delete [] copy;
    DBUG_RETURN(-1);
  }

  /* We need external lock before we can disable/enable keys */
  if (to->file->ha_external_lock(thd, F_WRLCK))
  {
    /* Undo call to mysql_trans_prepare_alter_copy_data() */
    ha_enable_transaction(thd, TRUE);
    delete [] copy;
    DBUG_RETURN(-1);
  }

  backup_set_alter_copy_lock(thd, from);

  alter_table_manage_keys(to, from->file->indexes_are_disabled(), keys_onoff);

  from->default_column_bitmaps();

  /* We can abort alter table for any table type */
  thd->abort_on_warning= !ignore && thd->is_strict_mode();

  from->file->info(HA_STATUS_VARIABLE);
  to->file->extra(HA_EXTRA_PREPARE_FOR_ALTER_TABLE);
  to->file->ha_start_bulk_insert(from->file->stats.records,
                                 ignore ? 0 : HA_CREATE_UNIQUE_INDEX_BY_SORT);
  bulk_insert_started= 1;
  List_iterator<Create_field> it(create);
  Create_field *def;
  copy_end=copy;
  to->s->default_fields= 0;
  for (Field **ptr=to->field ; *ptr ; ptr++)
  {
    def=it++;
    if (def->field)
    {
      if (*ptr == to->next_number_field)
      {
        auto_increment_field_copied= TRUE;
        /*
          If we are going to copy contents of one auto_increment column to
          another auto_increment column it is sensible to preserve zeroes.
          This condition also covers case when we are don't actually alter
          auto_increment column.
        */
        if (def->field == from->found_next_number_field)
          thd->variables.sql_mode|= MODE_NO_AUTO_VALUE_ON_ZERO;
      }
      if (!(*ptr)->vcol_info)
      {
        bitmap_set_bit(from->read_set, def->field->field_index);
        (copy_end++)->set(*ptr,def->field,0);
      }
    }
    else
    {
      /*
        Update the set of auto-update fields to contain only the new fields
        added to the table. Only these fields should be updated automatically.
        Old fields keep their current values, and therefore should not be
        present in the set of autoupdate fields.
      */
      if ((*ptr)->default_value)
      {
        *(dfield_ptr++)= *ptr;
        ++to->s->default_fields;
      }
    }
  }
  if (dfield_ptr)
    *dfield_ptr= NULL;

  if (order)
  {
    if (to->s->primary_key != MAX_KEY &&
        to->file->ha_table_flags() & HA_TABLE_SCAN_ON_INDEX)
    {
      char warn_buff[MYSQL_ERRMSG_SIZE];
      bool save_abort_on_warning= thd->abort_on_warning;
      thd->abort_on_warning= false;
      my_snprintf(warn_buff, sizeof(warn_buff),
                  "ORDER BY ignored as there is a user-defined clustered index"
                  " in the table '%-.192s'", from->s->table_name.str);
      push_warning(thd, Sql_condition::WARN_LEVEL_WARN, ER_UNKNOWN_ERROR,
                   warn_buff);
      thd->abort_on_warning= save_abort_on_warning;
    }
    else
    {
      bzero((char *) &tables, sizeof(tables));
      tables.table= from;
      tables.alias= tables.table_name= from->s->table_name;
      tables.db= from->s->db;

      THD_STAGE_INFO(thd, stage_sorting);
      Filesort_tracker dummy_tracker(false);
      Filesort fsort(order, HA_POS_ERROR, true, NULL);

      if (thd->lex->first_select_lex()->setup_ref_array(thd, order_num) ||
          setup_order(thd, thd->lex->first_select_lex()->ref_pointer_array,
                      &tables, fields, all_fields, order))
        goto err;

      if (!(file_sort= filesort(thd, from, &fsort, &dummy_tracker)))
        goto err;
    }
    thd_progress_next_stage(thd);
  }

  if (make_versioned)
  {
    query_start= thd->query_start_TIME();
    to_row_start= to->vers_start_field();
    to_row_end= to->vers_end_field();
  }
  else if (make_unversioned)
  {
    from_row_end= from->vers_end_field();
  }

  if (from_row_end)
    bitmap_set_bit(from->read_set, from_row_end->field_index);

  from->file->column_bitmaps_signal();

  THD_STAGE_INFO(thd, stage_copy_to_tmp_table);
  /* Tell handler that we have values for all columns in the to table */
  to->use_all_columns();
  /* Add virtual columns to vcol_set to ensure they are updated */
  if (to->vfield)
    to->mark_virtual_columns_for_write(TRUE);
  if (init_read_record(&info, thd, from, (SQL_SELECT *) 0, file_sort, 1, 1,
                       FALSE))
    goto err;
  init_read_record_done= 1;

  if (ignore && !alter_ctx->fk_error_if_delete_row)
    to->file->extra(HA_EXTRA_IGNORE_DUP_KEY);
  thd->get_stmt_da()->reset_current_row_for_warning();
  restore_record(to, s->default_values);        // Create empty record
  to->reset_default_fields();

  thd->progress.max_counter= from->file->records();
  time_to_report_progress= MY_HOW_OFTEN_TO_WRITE/10;
  if (!ignore) /* for now, InnoDB needs the undo log for ALTER IGNORE */
    to->file->extra(HA_EXTRA_BEGIN_ALTER_COPY);

  while (likely(!(error= info.read_record())))
  {
    if (unlikely(thd->killed))
    {
      thd->send_kill_message();
      error= 1;
      break;
    }

    if (make_unversioned)
    {
      if (!from_row_end->is_max())
        continue; // Drop history rows.
    }

    if (unlikely(++thd->progress.counter >= time_to_report_progress))
    {
      time_to_report_progress+= MY_HOW_OFTEN_TO_WRITE/10;
      thd_progress_report(thd, thd->progress.counter,
                          thd->progress.max_counter);
    }

    /* Return error if source table isn't empty. */
    if (unlikely(alter_ctx->error_if_not_empty))
    {
      error= 1;
      break;
    }

    for (Copy_field *copy_ptr=copy ; copy_ptr != copy_end ; copy_ptr++)
    {
      copy_ptr->do_copy(copy_ptr);
    }

    if (make_versioned)
    {
      to_row_start->set_notnull();
      to_row_start->store_time(&query_start);
      to_row_end->set_max();
    }

    prev_insert_id= to->file->next_insert_id;
    if (to->default_field)
      to->update_default_fields(ignore);
    if (to->vfield)
      to->update_virtual_fields(to->file, VCOL_UPDATE_FOR_WRITE);

    /* This will set thd->is_error() if fatal failure */
    if (to->verify_constraints(ignore) == VIEW_CHECK_SKIP)
      continue;
    if (unlikely(thd->is_error()))
    {
      error= 1;
      break;
    }
    if (keep_versioned && to->versioned(VERS_TRX_ID))
      to->vers_write= false;

    if (to->next_number_field)
    {
      if (auto_increment_field_copied)
        to->auto_increment_field_not_null= TRUE;
      else
        to->next_number_field->reset();
    }
    error= to->file->ha_write_row(to->record[0]);
    to->auto_increment_field_not_null= FALSE;
    if (unlikely(error))
    {
      if (to->file->is_fatal_error(error, HA_CHECK_DUP))
      {
        /* Not a duplicate key error. */
	to->file->print_error(error, MYF(0));
        error= 1;
	break;
      }
      else
      {
        /* Duplicate key error. */
        if (unlikely(alter_ctx->fk_error_if_delete_row))
        {
          /*
            We are trying to omit a row from the table which serves as parent
            in a foreign key. This might have broken referential integrity so
            emit an error. Note that we can't ignore this error even if we are
            executing ALTER IGNORE TABLE. IGNORE allows to skip rows, but
            doesn't allow to break unique or foreign key constraints,
          */
          my_error(ER_FK_CANNOT_DELETE_PARENT, MYF(0),
                   alter_ctx->fk_error_id,
                   alter_ctx->fk_error_table);
          break;
        }

        if (ignore)
        {
          /* This ALTER IGNORE TABLE. Simply skip row and continue. */
          to->file->restore_auto_increment(prev_insert_id);
          delete_count++;
        }
        else
        {
          /* Ordinary ALTER TABLE. Report duplicate key error. */
          uint key_nr= to->file->get_dup_key(error);
          if ((int) key_nr >= 0)
          {
            const char *err_msg= ER_THD(thd, ER_DUP_ENTRY_WITH_KEY_NAME);
            if (key_nr == 0 && to->s->keys > 0 &&
                (to->key_info[0].key_part[0].field->flags &
                 AUTO_INCREMENT_FLAG))
              err_msg= ER_THD(thd, ER_DUP_ENTRY_AUTOINCREMENT_CASE);
            print_keydup_error(to,
                               key_nr >= to->s->keys ? NULL :
                                   &to->key_info[key_nr],
                               err_msg, MYF(0));
          }
          else
            to->file->print_error(error, MYF(0));
          break;
        }
      }
    }
    else
      found_count++;
    thd->get_stmt_da()->inc_current_row_for_warning();
  }

  THD_STAGE_INFO(thd, stage_enabling_keys);
  thd_progress_next_stage(thd);

  if (error > 0 && !from->s->tmp_table)
  {
    /* We are going to drop the temporary table */
    to->file->extra(HA_EXTRA_PREPARE_FOR_DROP);
  }
  if (unlikely(to->file->ha_end_bulk_insert()) && error <= 0)
  {
    /* Give error, if not already given */
    if (!thd->is_error())
      to->file->print_error(my_errno,MYF(0));
    error= 1;
  }
  bulk_insert_started= 0;
  if (!ignore)
    to->file->extra(HA_EXTRA_END_ALTER_COPY);

  cleanup_done= 1;
  to->file->extra(HA_EXTRA_NO_IGNORE_DUP_KEY);

  if (backup_reset_alter_copy_lock(thd))
    error= 1;

  if (unlikely(mysql_trans_commit_alter_copy_data(thd)))
    error= 1;

 err:
  if (bulk_insert_started)
    (void) to->file->ha_end_bulk_insert();

/* Free resources */
  if (init_read_record_done)
    end_read_record(&info);
  delete [] copy;
  delete file_sort;

  thd->variables.sql_mode= save_sql_mode;
  thd->abort_on_warning= 0;
  *copied= found_count;
  *deleted=delete_count;
  to->file->ha_release_auto_increment();
  to->s->default_fields= save_to_s_default_fields;

  if (!cleanup_done)
  {
    /* This happens if we get an error during initialization of data */
    DBUG_ASSERT(error);
    to->file->ha_end_bulk_insert();
    ha_enable_transaction(thd, TRUE);
  }

  if (to->file->ha_external_lock(thd,F_UNLCK))
    error=1;
  if (error < 0 && !from->s->tmp_table &&
      to->file->extra(HA_EXTRA_PREPARE_FOR_RENAME))
    error= 1;
  thd_progress_end(thd);
  DBUG_RETURN(error > 0 ? -1 : 0);
}


/*
  Recreates one table by calling mysql_alter_table().

  SYNOPSIS
    mysql_recreate_table()
    thd			Thread handler
    table_list          Table to recreate
    table_copy          Recreate the table by using
                        ALTER TABLE COPY algorithm

 RETURN
    Like mysql_alter_table().
*/

bool mysql_recreate_table(THD *thd, TABLE_LIST *table_list, bool table_copy)
{
  HA_CREATE_INFO create_info;
  Alter_info alter_info;
  TABLE_LIST *next_table= table_list->next_global;
  DBUG_ENTER("mysql_recreate_table");

  /* Set lock type which is appropriate for ALTER TABLE. */
  table_list->lock_type= TL_READ_NO_INSERT;
  /* Same applies to MDL request. */
  table_list->mdl_request.set_type(MDL_SHARED_NO_WRITE);
  /* hide following tables from open_tables() */
  table_list->next_global= NULL;

  bzero((char*) &create_info, sizeof(create_info));
  create_info.row_type=ROW_TYPE_NOT_USED;
  create_info.default_table_charset=default_charset_info;
  /* Force alter table to recreate table */
  alter_info.flags= (ALTER_CHANGE_COLUMN | ALTER_RECREATE);

  if (table_copy)
    alter_info.set_requested_algorithm(
      Alter_info::ALTER_TABLE_ALGORITHM_COPY);

  bool res= mysql_alter_table(thd, &null_clex_str, &null_clex_str, &create_info,
                                table_list, &alter_info, 0,
                                (ORDER *) 0, 0);
  table_list->next_global= next_table;
  DBUG_RETURN(res);
}


bool mysql_checksum_table(THD *thd, TABLE_LIST *tables,
                          HA_CHECK_OPT *check_opt)
{
  TABLE_LIST *table;
  List<Item> field_list;
  Item *item;
  Protocol *protocol= thd->protocol;
  DBUG_ENTER("mysql_checksum_table");

  /*
    CHECKSUM TABLE returns results and rollbacks statement transaction,
    so it should not be used in stored function or trigger.
  */
  DBUG_ASSERT(! thd->in_sub_stmt);

  field_list.push_back(item= new (thd->mem_root)
                       Item_empty_string(thd, "Table", NAME_LEN*2),
                       thd->mem_root);
  item->maybe_null= 1;
  field_list.push_back(item= new (thd->mem_root)
                       Item_int(thd, "Checksum", (longlong) 1,
                                MY_INT64_NUM_DECIMAL_DIGITS),
                       thd->mem_root);
  item->maybe_null= 1;
  if (protocol->send_result_set_metadata(&field_list,
                            Protocol::SEND_NUM_ROWS | Protocol::SEND_EOF))
    DBUG_RETURN(TRUE);

  /*
    Close all temporary tables which were pre-open to simplify
    privilege checking. Clear all references to closed tables.
  */
  close_thread_tables(thd);
  for (table= tables; table; table= table->next_local)
    table->table= NULL;

  /* Open one table after the other to keep lock time as short as possible. */
  for (table= tables; table; table= table->next_local)
  {
    char table_name[SAFE_NAME_LEN*2+2];
    TABLE *t;
    TABLE_LIST *save_next_global;

    strxmov(table_name, table->db.str ,".", table->table_name.str, NullS);

    /* Remember old 'next' pointer and break the list.  */
    save_next_global= table->next_global;
    table->next_global= NULL;
    table->lock_type= TL_READ;
    /* Allow to open real tables only. */
    table->required_type= TABLE_TYPE_NORMAL;

    if (thd->open_temporary_tables(table) ||
        open_and_lock_tables(thd, table, FALSE, 0))
    {
      t= NULL;
    }
    else
      t= table->table;

    table->next_global= save_next_global;

    protocol->prepare_for_resend();
    protocol->store(table_name, system_charset_info);

    if (!t)
    {
      /* Table didn't exist */
      protocol->store_null();
    }
    else
    {
      /* Call ->checksum() if the table checksum matches 'old_mode' settings */
      if (!(check_opt->flags & T_EXTEND) &&
          (((t->file->ha_table_flags() & HA_HAS_OLD_CHECKSUM) && thd->variables.old_mode) ||
           ((t->file->ha_table_flags() & HA_HAS_NEW_CHECKSUM) && !thd->variables.old_mode)))
      {
        if (t->file->info(HA_STATUS_VARIABLE) || t->file->stats.checksum_null)
          protocol->store_null();
        else
          protocol->store((longlong)t->file->stats.checksum);
      }
      else if (check_opt->flags & T_QUICK)
        protocol->store_null();
      else
      {
        int error= t->file->calculate_checksum();
        if (thd->killed)
        {
          /*
             we've been killed; let handler clean up, and remove the
             partial current row from the recordset (embedded lib)
          */
          t->file->ha_rnd_end();
          thd->protocol->remove_last_row();
          goto err;
        }
        if (error || t->file->stats.checksum_null)
          protocol->store_null();
        else
          protocol->store((longlong)t->file->stats.checksum);
      }
      trans_rollback_stmt(thd);
      close_thread_tables(thd);
    }

    if (thd->transaction_rollback_request)
    {
      /*
        If transaction rollback was requested we honor it. To do this we
        abort statement and return error as not only CHECKSUM TABLE is
        rolled back but the whole transaction in which it was used.
      */
      thd->protocol->remove_last_row();
      goto err;
    }

    /* Hide errors from client. Return NULL for problematic tables instead. */
    thd->clear_error();

    if (protocol->write())
      goto err;
  }

  my_eof(thd);
  DBUG_RETURN(FALSE);

err:
  DBUG_RETURN(TRUE);
}

/**
  @brief Check if the table can be created in the specified storage engine.

  Checks if the storage engine is enabled and supports the given table
  type (e.g. normal, temporary, system). May do engine substitution
  if the requested engine is disabled.

  @param thd          Thread descriptor.
  @param db_name      Database name.
  @param table_name   Name of table to be created.
  @param create_info  Create info from parser, including engine.

  @retval true  Engine not available/supported, error has been reported.
  @retval false Engine available/supported.
*/
bool check_engine(THD *thd, const char *db_name,
                  const char *table_name, HA_CREATE_INFO *create_info)
{
  DBUG_ENTER("check_engine");
  handlerton **new_engine= &create_info->db_type;
  handlerton *req_engine= *new_engine;
  handlerton *enf_engine= NULL;
  bool no_substitution= thd->variables.sql_mode & MODE_NO_ENGINE_SUBSTITUTION;
  *new_engine= ha_checktype(thd, req_engine, no_substitution);
  DBUG_ASSERT(*new_engine);
  if (!*new_engine)
    DBUG_RETURN(true);

  /* Enforced storage engine should not be used in
  ALTER TABLE that does not use explicit ENGINE = x to
  avoid unwanted unrelated changes.*/
  if (!(thd->lex->sql_command == SQLCOM_ALTER_TABLE &&
        !(create_info->used_fields & HA_CREATE_USED_ENGINE)))
    enf_engine= thd->variables.enforced_table_plugin ?
       plugin_hton(thd->variables.enforced_table_plugin) : NULL;

  if (enf_engine && enf_engine != *new_engine)
  {
    if (no_substitution)
    {
      const char *engine_name= ha_resolve_storage_engine_name(req_engine);
      my_error(ER_UNKNOWN_STORAGE_ENGINE, MYF(0), engine_name);
      DBUG_RETURN(TRUE);
    }
    *new_engine= enf_engine;
  }

  if (req_engine && req_engine != *new_engine)
  {
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE,
                        ER_WARN_USING_OTHER_HANDLER,
                        ER_THD(thd, ER_WARN_USING_OTHER_HANDLER),
                        ha_resolve_storage_engine_name(*new_engine),
                        table_name);
  }
  if (create_info->tmp_table() &&
      ha_check_storage_engine_flag(*new_engine, HTON_TEMPORARY_NOT_SUPPORTED))
  {
    if (create_info->used_fields & HA_CREATE_USED_ENGINE)
    {
      my_error(ER_ILLEGAL_HA_CREATE_OPTION, MYF(0),
               hton_name(*new_engine)->str, "TEMPORARY");
      *new_engine= 0;
      DBUG_RETURN(true);
    }
    *new_engine= myisam_hton;
  }

  DBUG_RETURN(false);
}


bool Sql_cmd_create_table_like::execute(THD *thd)
{
  DBUG_ENTER("Sql_cmd_create_table::execute");
  LEX *lex= thd->lex;
  SELECT_LEX *select_lex= lex->first_select_lex();
  TABLE_LIST *first_table= select_lex->table_list.first;
  DBUG_ASSERT(first_table == lex->query_tables);
  DBUG_ASSERT(first_table != 0);
  bool link_to_local;
  TABLE_LIST *create_table= first_table;
  TABLE_LIST *select_tables= lex->create_last_non_select_table->next_global;
  /* most outer SELECT_LEX_UNIT of query */
  SELECT_LEX_UNIT *unit= &lex->unit;
  int res= 0;

  const bool used_engine= lex->create_info.used_fields & HA_CREATE_USED_ENGINE;
  DBUG_ASSERT((m_storage_engine_name.str != NULL) == used_engine);
  if (used_engine)
  {
    if (resolve_storage_engine_with_error(thd, &lex->create_info.db_type,
                                          lex->create_info.tmp_table()))
      DBUG_RETURN(true); // Engine not found, substitution is not allowed

    if (!lex->create_info.db_type) // Not found, but substitution is allowed
    {
      lex->create_info.use_default_db_type(thd);
      push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                          ER_WARN_USING_OTHER_HANDLER,
                          ER_THD(thd, ER_WARN_USING_OTHER_HANDLER),
                          hton_name(lex->create_info.db_type)->str,
                          create_table->table_name.str);
    }
  }

  if (lex->tmp_table())
  {
    status_var_decrement(thd->status_var.com_stat[SQLCOM_CREATE_TABLE]);
    status_var_increment(thd->status_var.com_create_tmp_table);
  }

  /*
    Code below (especially in mysql_create_table() and select_create
    methods) may modify HA_CREATE_INFO structure in LEX, so we have to
    use a copy of this structure to make execution prepared statement-
    safe. A shallow copy is enough as this code won't modify any memory
    referenced from this structure.
  */
  Table_specification_st create_info(lex->create_info);
  /*
    We need to copy alter_info for the same reasons of re-execution
    safety, only in case of Alter_info we have to do (almost) a deep
    copy.
  */
  Alter_info alter_info(lex->alter_info, thd->mem_root);

  if (unlikely(thd->is_fatal_error))
  {
    /* If out of memory when creating a copy of alter_info. */
    res= 1;
    goto end_with_restore_list;
  }

  /* Check privileges */
  if ((res= create_table_precheck(thd, select_tables, create_table)))
    goto end_with_restore_list;

  /* Might have been updated in create_table_precheck */
  create_info.alias= create_table->alias;

  /* Fix names if symlinked or relocated tables */
  if (append_file_to_dir(thd, &create_info.data_file_name,
                         &create_table->table_name) ||
      append_file_to_dir(thd, &create_info.index_file_name,
                         &create_table->table_name))
    goto end_with_restore_list;

  /*
    If no engine type was given, work out the default now
    rather than at parse-time.
  */
  if (!(create_info.used_fields & HA_CREATE_USED_ENGINE))
    create_info.use_default_db_type(thd);
  /*
    If we are using SET CHARSET without DEFAULT, add an implicit
    DEFAULT to not confuse old users. (This may change).
  */
  if ((create_info.used_fields &
       (HA_CREATE_USED_DEFAULT_CHARSET | HA_CREATE_USED_CHARSET)) ==
      HA_CREATE_USED_CHARSET)
  {
    create_info.used_fields&= ~HA_CREATE_USED_CHARSET;
    create_info.used_fields|= HA_CREATE_USED_DEFAULT_CHARSET;
    create_info.default_table_charset= create_info.table_charset;
    create_info.table_charset= 0;
  }

  /*
    If we are a slave, we should add OR REPLACE if we don't have
    IF EXISTS. This will help a slave to recover from
    CREATE TABLE OR EXISTS failures by dropping the table and
    retrying the create.
  */
  if (thd->slave_thread &&
      slave_ddl_exec_mode_options == SLAVE_EXEC_MODE_IDEMPOTENT &&
      !lex->create_info.if_not_exists())
  {
    create_info.add(DDL_options_st::OPT_OR_REPLACE);
    create_info.add(DDL_options_st::OPT_OR_REPLACE_SLAVE_GENERATED);
  }

#ifdef WITH_PARTITION_STORAGE_ENGINE
  thd->work_part_info= 0;
  {
    partition_info *part_info= thd->lex->part_info;
    if (part_info && !(part_info= part_info->get_clone(thd)))
    {
      res= -1;
      goto end_with_restore_list;
    }
    thd->work_part_info= part_info;
  }
#endif

  if (select_lex->item_list.elements || select_lex->tvc) // With select or TVC
  {
    select_result *result;

    /*
      CREATE TABLE...IGNORE/REPLACE SELECT... can be unsafe, unless
      ORDER BY PRIMARY KEY clause is used in SELECT statement. We therefore
      use row based logging if mixed or row based logging is available.
      TODO: Check if the order of the output of the select statement is
      deterministic. Waiting for BUG#42415
    */
    if(lex->ignore)
      lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_CREATE_IGNORE_SELECT);

    if(lex->duplicates == DUP_REPLACE)
      lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_CREATE_REPLACE_SELECT);

    /*
      If:
      a) we inside an SP and there was NAME_CONST substitution,
      b) binlogging is on (STMT mode),
      c) we log the SP as separate statements
      raise a warning, as it may cause problems
      (see 'NAME_CONST issues' in 'Binary Logging of Stored Programs')
     */
    if (thd->query_name_consts && mysql_bin_log.is_open() &&
        thd->wsrep_binlog_format() == BINLOG_FORMAT_STMT &&
        !mysql_bin_log.is_query_in_union(thd, thd->query_id))
    {
      List_iterator_fast<Item> it(select_lex->item_list);
      Item *item;
      uint splocal_refs= 0;
      /* Count SP local vars in the top-level SELECT list */
      while ((item= it++))
      {
        if (item->get_item_splocal())
          splocal_refs++;
      }
      /*
        If it differs from number of NAME_CONST substitution applied,
        we may have a SOME_FUNC(NAME_CONST()) in the SELECT list,
        that may cause a problem with binary log (see BUG#35383),
        raise a warning.
      */
      if (splocal_refs != thd->query_name_consts)
        push_warning(thd,
                     Sql_condition::WARN_LEVEL_WARN,
                     ER_UNKNOWN_ERROR,
"Invoked routine ran a statement that may cause problems with "
"binary log, see 'NAME_CONST issues' in 'Binary Logging of Stored Programs' "
"section of the manual.");
    }

    select_lex->options|= SELECT_NO_UNLOCK;
    unit->set_limit(select_lex);

    /*
      Disable non-empty MERGE tables with CREATE...SELECT. Too
      complicated. See Bug #26379. Empty MERGE tables are read-only
      and don't allow CREATE...SELECT anyway.
    */
    if (create_info.used_fields & HA_CREATE_USED_UNION)
    {
      my_error(ER_WRONG_OBJECT, MYF(0), create_table->db.str,
               create_table->table_name.str, "BASE TABLE");
      res= 1;
      goto end_with_restore_list;
    }

    res= open_and_lock_tables(thd, create_info, lex->query_tables, TRUE, 0);
    if (unlikely(res))
    {
      /* Got error or warning. Set res to 1 if error */
      if (!(res= thd->is_error()))
        my_ok(thd);                           // CREATE ... IF NOT EXISTS
      goto end_with_restore_list;
    }

    /* Ensure we don't try to create something from which we select from */
    if (create_info.or_replace() && !create_info.tmp_table())
    {
      if (TABLE_LIST *duplicate= unique_table(thd, lex->query_tables,
                                              lex->query_tables->next_global,
                                              CHECK_DUP_FOR_CREATE |
                                              CHECK_DUP_SKIP_TEMP_TABLE))
      {
        update_non_unique_table_error(lex->query_tables, "CREATE",
                                      duplicate);
        res= TRUE;
        goto end_with_restore_list;
      }
    }
    {
      /*
        Remove target table from main select and name resolution
        context. This can't be done earlier as it will break view merging in
        statements like "CREATE TABLE IF NOT EXISTS existing_view SELECT".
      */
      lex->unlink_first_table(&link_to_local);

      /* Store reference to table in case of LOCK TABLES */
      create_info.table= create_table->table;

      /*
        select_create is currently not re-execution friendly and
        needs to be created for every execution of a PS/SP.
        Note: In wsrep-patch, CTAS is handled like a regular transaction.
      */
      if ((result= new (thd->mem_root) select_create(thd, create_table,
                                                     &create_info,
                                                     &alter_info,
                                                     select_lex->item_list,
                                                     lex->duplicates,
                                                     lex->ignore,
                                                     select_tables)))
      {
        /*
          CREATE from SELECT give its SELECT_LEX for SELECT,
          and item_list belong to SELECT
        */
        if (!(res= handle_select(thd, lex, result, 0)))
        {
          if (create_info.tmp_table())
            thd->variables.option_bits|= OPTION_KEEP_LOG;
        }
        delete result;
      }
      lex->link_first_table_back(create_table, link_to_local);
    }
  }
  else
  {
    /* regular create */
    if (create_info.like())
    {
      /* CREATE TABLE ... LIKE ... */
      res= mysql_create_like_table(thd, create_table, select_tables,
                                   &create_info);
    }
    else
    {
      if (create_info.fix_create_fields(thd, &alter_info, *create_table) ||
          create_info.check_fields(thd, &alter_info,
                                   create_table->table_name, create_table->db))
	goto end_with_restore_list;

      /*
        In STATEMENT format, we probably have to replicate also temporary
        tables, like mysql replication does. Also check if the requested
        engine is allowed/supported.
      */
      if (WSREP(thd) &&
          !check_engine(thd, create_table->db.str, create_table->table_name.str,
                        &create_info) &&
          (!thd->is_current_stmt_binlog_format_row() ||
           !create_info.tmp_table()))
      {
        WSREP_TO_ISOLATION_BEGIN(create_table->db.str, create_table->table_name.str, NULL);
      }
      /* Regular CREATE TABLE */
      res= mysql_create_table(thd, create_table, &create_info, &alter_info);
    }
    if (!res)
    {
      /* So that CREATE TEMPORARY TABLE gets to binlog at commit/rollback */
      if (create_info.tmp_table())
        thd->variables.option_bits|= OPTION_KEEP_LOG;
      /* in case of create temp tables if @@session_track_state_change is
         ON then send session state notification in OK packet */
      if (create_info.options & HA_LEX_CREATE_TMP_TABLE)
      {
        SESSION_TRACKER_CHANGED(thd, SESSION_STATE_CHANGE_TRACKER, NULL);
      }
      my_ok(thd);
    }
  }

end_with_restore_list:
  DBUG_RETURN(res);

#ifdef WITH_WSREP
wsrep_error_label:
  DBUG_RETURN(true);
#endif
}
