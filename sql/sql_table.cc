/*
   Copyright (c) 2000, 2015, Oracle and/or its affiliates.
   Copyright (c) 2010, 2016, MariaDB

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

/* drop and alter of tables */

#include <my_global.h>
#include "sql_priv.h"
#include "unireg.h"
#include "debug_sync.h"
#include "sql_table.h"
#include "sql_parse.h"                        // test_if_data_home_dir
#include "sql_cache.h"                          // query_cache_*
#include "sql_base.h"   // open_table_uncached, lock_table_names
#include "lock.h"       // mysql_unlock_tables
#include "strfunc.h"    // find_type2, find_set
#include "sql_truncate.h"                       // regenerate_locked_table 
#include "sql_partition.h"                      // mem_alloc_error,
                                                // generate_partition_syntax,
                                                // partition_info
                                                // NOT_A_PARTITION_ID
#include "sql_db.h"                             // load_db_opt_by_name
#include "sql_time.h"                  // make_truncated_value_warning
#include "records.h"             // init_read_record, end_read_record
#include "filesort.h"            // filesort_free_buffers
#include "sql_select.h"                // setup_order,
                                       // make_unireg_sortorder
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

#ifdef __WIN__
#include <io.h>
#endif

const char *primary_key_name="PRIMARY";

static bool check_if_keyname_exists(const char *name,KEY *start, KEY *end);
static char *make_unique_key_name(THD *thd, const char *field_name, KEY *start,
                                  KEY *end);
static int copy_data_between_tables(THD *thd, TABLE *from,TABLE *to,
                                    List<Create_field> &create, bool ignore,
				    uint order_num, ORDER *order,
				    ha_rows *copied,ha_rows *deleted,
                                    Alter_info::enum_enable_or_disable keys_onoff,
                                    Alter_table_ctx *alter_ctx);

static bool prepare_blob_field(THD *thd, Column_definition *sql_field);
static int mysql_prepare_create_table(THD *, HA_CREATE_INFO *, Alter_info *,
                                      uint *, handler *, KEY **, uint *, int);
static uint blob_length_by_type(enum_field_types type);

/**
  @brief Helper function for explain_filename
  @param thd          Thread handle
  @param to_p         Explained name in system_charset_info
  @param end_p        End of the to_p buffer
  @param name         Name to be converted
  @param name_len     Length of the name, in bytes
*/
static char* add_identifier(THD* thd, char *to_p, const char * end_p,
                            const char* name, uint name_len)
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
  if (!res || errors)
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

  quote = thd ? get_quote_char_for_identifier(thd, conv_name, res - 1) : '"';

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
  int  db_name_len= 0;
  const char *table_name;
  int  table_name_len= 0;
  const char *part_name= NULL;
  int  part_name_len= 0;
  const char *subpart_name= NULL;
  int  subpart_name_len= 0;
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
    db_name_len= tmp_p - db_name;
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
        part_name_len= tmp_p - part_name - 1;
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
    table_name_len= part_name - table_name - 3;
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
  DBUG_RETURN(to_p - to);
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

uint filename_to_tablename(const char *from, char *to, uint to_length, 
                           bool stay_quiet)
{
  uint errors;
  size_t res;
  DBUG_ENTER("filename_to_tablename");
  DBUG_PRINT("enter", ("from '%s'", from));

  res= strconvert(&my_charset_filename, from, FN_REFLEN,
                  system_charset_info,  to, to_length, &errors);
  if (errors) // Old 5.0 name
  {
    res= (strxnmov(to, to_length, MYSQL50_TABLE_NAME_PREFIX,  from, NullS) -
          to);
    if (!stay_quiet)
      sql_print_error("Invalid (old?) table or database name '%s'", from);
  }

  DBUG_PRINT("exit", ("to '%s'", to));
  DBUG_RETURN(res);
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

uint check_n_cut_mysql50_prefix(const char *from, char *to, uint to_length)
{
  if (check_mysql50_prefix(from))
    return (uint) (strmake(to, from + MYSQL50_TABLE_NAME_PREFIX_LENGTH,
                           to_length - 1) - to);
  return 0;
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

uint tablename_to_filename(const char *from, char *to, uint to_length)
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

  if (flags & FN_IS_TMP) // FN_FROM_IS_TMP | FN_TO_IS_TMP
    strmake(tbbuff, table_name, sizeof(tbbuff)-1);
  else
    (void) tablename_to_filename(table_name, tbbuff, sizeof(tbbuff));

  (void) tablename_to_filename(db, dbbuff, sizeof(dbbuff));

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
  DBUG_RETURN(pos - buff);
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
  DBUG_RETURN(length);
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
  uint io_size= global_ddl_log.io_size;
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
  LEX_STRING handler_name;
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
  init_sql_alloc(&mem_root, TABLE_ALLOC_BLOCK_SIZE, 0, MYF(MY_THREAD_SPECIFIC));
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
    if (!file)
    {
      mem_alloc_error(sizeof(handler));
      goto error;
    }
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
          if ((error= mysql_file_delete(key_file_frm, to_path, MYF(MY_WME))))
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
          if ((error= file->ha_delete_table(ddl_log_entry->name)))
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
  if (write_ddl_log_file_entry((*active_entry)->entry_pos))
  {
    error= TRUE;
    sql_print_error("Failed to write entry_no = %u",
                    (*active_entry)->entry_pos);
  }
  if (write_header && !error)
  {
    (void) sync_ddl_log_no_lock();
    if (write_ddl_log_header())
      error= TRUE;
  }
  if (error)
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
  if (!(thd=new THD))
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
  my_snprintf (tmp_name, sizeof (tmp_name), "%s-%s", tmp_file_prefix,
               lpt->table_name);
  return build_table_filename(buff, bufflen, lpt->db, tmp_name, "", FN_IS_TMP);
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
        if (!(part_syntax_buf= generate_partition_syntax(lpt->thd, part_info,
                                                         &syntax_len,
                                                         TRUE, TRUE,
                                                         lpt->create_info,
                                                         lpt->alter_info,
                                                         NULL)))
        {
          DBUG_RETURN(TRUE);
        }
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

    int error= writefrm(shadow_path, lpt->db, lpt->table_name,
                        lpt->create_info->tmp_table(), frm.str, frm.length);
    my_free(const_cast<uchar*>(frm.str));

    if (error || lpt->table->file->ha_create_partitioning_metadata(shadow_path,
                                       NULL, CHF_CREATE_FLAG))
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
    build_table_filename(path, sizeof(path) - 1, lpt->db,
                         lpt->table_name, "", 0);
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
      if (!(part_syntax_buf= generate_partition_syntax(lpt->thd, part_info,
                                                       &syntax_len,
                                                       TRUE, TRUE,
                                                       lpt->create_info,
                                                       lpt->alter_info,
                                                       NULL)))
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
                             errcode);
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

bool mysql_rm_table(THD *thd,TABLE_LIST *tables, my_bool if_exists,
                    my_bool drop_temporary)
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
    if (!in_bootstrap)
    {
      for (table= tables; table; table= table->next_local)
      {
        LEX_STRING db_name= { table->db, table->db_length };
        LEX_STRING table_name= { table->table_name, table->table_name_length };
        if (table->open_type == OT_BASE_ONLY ||
            !find_temporary_table(thd, table))
          (void) delete_statistics_for_table(thd, &db_name, &table_name);
      }
    }

    if (!thd->locked_tables_mode)
    {
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
          table->table= find_table_for_mdl_upgrade(thd, table->db,
                                                   table->table_name, false);
          if (!table->table)
            DBUG_RETURN(true);
          table->mdl_request.ticket= table->table->mdl_ticket;
        }
      }
    }
  }

  /* mark for close and remove all cached entries */
  thd->push_internal_handler(&err_handler);
  error= mysql_rm_table_no_locks(thd, tables, if_exists, drop_temporary,
                                 false, false, false);
  thd->pop_internal_handler();

  if (error)
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
      return (char*) query - *comment_start + 1;
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
                            bool dont_log_query,
                            bool dont_free_locks)
{
  TABLE_LIST *table;
  char path[FN_REFLEN + 1], wrong_tables_buff[160], *alias= NULL;
  String wrong_tables(wrong_tables_buff, sizeof(wrong_tables_buff)-1,
                      system_charset_info);
  uint path_length= 0, errors= 0;
  int error= 0;
  int non_temp_tables_count= 0;
  bool non_tmp_error= 0;
  bool trans_tmp_table_deleted= 0, non_trans_tmp_table_deleted= 0;
  bool non_tmp_table_deleted= 0;
  bool is_drop_tmp_if_exists_added= 0;
  bool was_view= 0;
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
    if (!drop_temporary)
    {
      const char *comment_start;
      uint32 comment_len;

      built_query.set_charset(thd->charset());
      if (if_exists)
        built_query.append("DROP TABLE IF EXISTS ");
      else
        built_query.append("DROP TABLE ");

      if ((comment_len= comment_length(thd, if_exists ? 17:9, &comment_start)))
      {
        built_query.append(comment_start, comment_len);
        built_query.append(" ");
      }
    }

    if (thd->is_current_stmt_binlog_format_row() || if_exists)
    {
      is_drop_tmp_if_exists_added= true;
      built_trans_tmp_query.set_charset(system_charset_info);
      built_trans_tmp_query.append("DROP TEMPORARY TABLE IF EXISTS ");
      built_non_trans_tmp_query.set_charset(system_charset_info);
      built_non_trans_tmp_query.append("DROP TEMPORARY TABLE IF EXISTS ");
    }
    else
    {
      built_trans_tmp_query.set_charset(system_charset_info);
      built_trans_tmp_query.append("DROP TEMPORARY TABLE ");
      built_non_trans_tmp_query.set_charset(system_charset_info);
      built_non_trans_tmp_query.append("DROP TEMPORARY TABLE ");
    }
  }

  for (table= tables; table; table= table->next_local)
  {
    bool is_trans= 0;
    bool table_creation_was_logged= 1;
    char *db=table->db;
    size_t db_length= table->db_length;
    handlerton *table_type= 0;

    DBUG_PRINT("table", ("table_l: '%s'.'%s'  table: 0x%lx  s: 0x%lx",
                         table->db, table->table_name, (long) table->table,
                         table->table ? (long) table->table->s : (long) -1));

    /*
      If we are in locked tables mode and are dropping a temporary table,
      the ticket should be NULL to ensure that we don't release a lock
      on a base table later.
    */
    DBUG_ASSERT(!(thd->locked_tables_mode &&
                  table->open_type != OT_BASE_ONLY &&
                  find_temporary_table(thd, table) &&
                  table->mdl_request.ticket != NULL));

    /*
      drop_temporary_table may return one of the following error codes:
      .  0 - a temporary table was successfully dropped.
      .  1 - a temporary table was not found.
      . -1 - a temporary table is used by an outer statement.
    */
    if (table->open_type == OT_BASE_ONLY || !is_temporary_table(table))
      error= 1;
    else
    {
      table_creation_was_logged= table->table->s->table_creation_was_logged;
      if ((error= drop_temporary_table(thd, table->table, &is_trans)) == -1)
      {
        DBUG_ASSERT(thd->in_sub_stmt);
        goto err;
      }
      table->table= 0;
    }

    if ((drop_temporary && if_exists) || !error)
    {
      /*
        This handles the case of temporary tables. We have the following cases:

          . "DROP TEMPORARY" was executed and a temporary table was affected
          (i.e. drop_temporary && !error) or the if_exists was specified (i.e.
          drop_temporary && if_exists).

          . "DROP" was executed but a temporary table was affected (.i.e
          !error).
      */
      if (!dont_log_query && table_creation_was_logged)
      {
        /*
          If there is an error, we don't know the type of the engine
          at this point. So, we keep it in the trx-cache.
        */
        is_trans= error ? TRUE : is_trans;
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
        if (thd->db == NULL || strcmp(db,thd->db) != 0
            || is_drop_tmp_if_exists_added )
        {
          append_identifier(thd, built_ptr_query, db, db_length);
          built_ptr_query->append(".");
        }
        append_identifier(thd, built_ptr_query, table->table_name,
                          table->table_name_length);
        built_ptr_query->append(",");
      }
      /*
        This means that a temporary table was droped and as such there
        is no need to proceed with the code that tries to drop a regular
        table.
      */
      if (!error) continue;
    }
    else if (!drop_temporary)
    {
      non_temp_tables_count++;

      DBUG_ASSERT(thd->mdl_context.is_lock_owner(MDL_key::TABLE, table->db,
                                                 table->table_name,
                                                 MDL_SHARED));

      alias= (lower_case_table_names == 2) ? table->alias : table->table_name;
      /* remove .frm file and engine files */
      path_length= build_table_filename(path, sizeof(path) - 1, db, alias,
                                        reg_ext, 0);

      /*
        This handles the case where a "DROP" was executed and a regular
        table "may be" dropped as drop_temporary is FALSE and error is
        TRUE. If the error was FALSE a temporary table was dropped and
        regardless of the status of drop_temporary a "DROP TEMPORARY"
        must be used.
      */
      if (!dont_log_query)
      {
        /*
          Note that unless if_exists is TRUE or a temporary table was deleted, 
          there is no means to know if the statement should be written to the
          binary log. See further information on this variable in what follows.
        */
        non_tmp_table_deleted= (if_exists ? TRUE : non_tmp_table_deleted);
        /*
          Don't write the database name if it is the current one (or if
          thd->db is NULL).
        */
        if (thd->db == NULL || strcmp(db,thd->db) != 0)
        {
          append_identifier(thd, &built_query, db, db_length);
          built_query.append(".");
        }

        append_identifier(thd, &built_query, table->table_name,
                          table->table_name_length);
        built_query.append(",");
      }
    }
    DEBUG_SYNC(thd, "rm_table_no_locks_before_delete_table");
    error= 0;
    if (drop_temporary ||
        (ha_table_exists(thd, db, alias, &table_type) == 0 && table_type == 0) ||
        (!drop_view && (was_view= (table_type == view_pseudo_hton))))
    {
      /*
        One of the following cases happened:
          . "DROP TEMPORARY" but a temporary table was not found.
          . "DROP" but table was not found
          . "DROP TABLE" statement, but it's a view. 
      */
      if (if_exists)
      {
        char buff[FN_REFLEN];
        String tbl_name(buff, sizeof(buff), system_charset_info);
        tbl_name.length(0);
        tbl_name.append(db);
        tbl_name.append('.');
        tbl_name.append(table->table_name);
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE,
                            ER_BAD_TABLE_ERROR,
                            ER_THD(thd, ER_BAD_TABLE_ERROR),
                            tbl_name.c_ptr_safe());
      }
      else
      {
        non_tmp_error = (drop_temporary ? non_tmp_error : TRUE);
        error= 1;
      }
    }
    else
    {
      char *end;
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

      if (thd->locked_tables_mode)
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
        tdc_remove_table(thd, TDC_RT_REMOVE_ALL, table->db, table->table_name,
                         false);

      /* Check that we have an exclusive lock on the table to be dropped. */
      DBUG_ASSERT(thd->mdl_context.is_lock_owner(MDL_key::TABLE, table->db,
                                                 table->table_name,
                                                 MDL_EXCLUSIVE));

      // Remove extension for delete
      *(end= path + path_length - reg_ext_length)= '\0';

      error= ha_delete_table(thd, table_type, path, db, table->table_name,
                             !dont_log_query);

      if (!error)
      {
        int frm_delete_error, trigger_drop_error= 0;
	/* Delete the table definition file */
	strmov(end,reg_ext);
        frm_delete_error= mysql_file_delete(key_file_frm, path, MYF(MY_WME));
        if (frm_delete_error)
          frm_delete_error= my_errno;
        else
        {
          non_tmp_table_deleted= TRUE;
          trigger_drop_error=
            Table_triggers_list::drop_all_triggers(thd, db, table->table_name);
        }

        if (trigger_drop_error ||
            (frm_delete_error && frm_delete_error != ENOENT))
          error= 1;
        else if (frm_delete_error && if_exists)
          thd->clear_error();
      }
      non_tmp_error= error ? TRUE : non_tmp_error;
    }
    if (error)
    {
      if (wrong_tables.length())
	wrong_tables.append(',');
      wrong_tables.append(db);
      wrong_tables.append('.');
      wrong_tables.append(table->table_name);
      errors++;
    }
    else
    {
      PSI_CALL_drop_table_share(false, table->db, table->db_length,
                                table->table_name, table->table_name_length);
      mysql_audit_drop_table(thd, table);
    }

    DBUG_PRINT("table", ("table: 0x%lx  s: 0x%lx", (long) table->table,
                         table->table ? (long) table->table->s : (long) -1));

    DBUG_EXECUTE_IF("bug43138",
                    my_printf_error(ER_BAD_TABLE_ERROR,
                                    ER_THD(thd, ER_BAD_TABLE_ERROR), MYF(0),
                                    table->table_name););
  }
  DEBUG_SYNC(thd, "rm_table_no_locks_before_binlog");
  thd->thread_specific_used|= (trans_tmp_table_deleted ||
                               non_trans_tmp_table_deleted);
  error= 0;
err:
  if (wrong_tables.length())
  {
    DBUG_ASSERT(errors);
    if (errors == 1 && was_view)
      my_printf_error(ER_IT_IS_A_VIEW, ER_THD(thd, ER_IT_IS_A_VIEW), MYF(0),
                      wrong_tables.c_ptr_safe());
    else if (errors > 1 || !thd->is_error())
      my_printf_error(ER_BAD_TABLE_ERROR, ER_THD(thd, ER_BAD_TABLE_ERROR),
                      MYF(0),
                      wrong_tables.c_ptr_safe());
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
    query_cache_invalidate3(thd, tables, 0);
    if (!dont_log_query && mysql_bin_log.is_open())
    {
      if (non_trans_tmp_table_deleted)
      {
          /* Chop of the last comma */
          built_non_trans_tmp_query.chop();
          built_non_trans_tmp_query.append(" /* generated by server */");
          error |= thd->binlog_query(THD::STMT_QUERY_TYPE,
                                     built_non_trans_tmp_query.ptr(),
                                     built_non_trans_tmp_query.length(),
                                     FALSE, FALSE,
                                     is_drop_tmp_if_exists_added,
                                     0);
      }
      if (trans_tmp_table_deleted)
      {
          /* Chop of the last comma */
          built_trans_tmp_query.chop();
          built_trans_tmp_query.append(" /* generated by server */");
          error |= thd->binlog_query(THD::STMT_QUERY_TYPE,
                                     built_trans_tmp_query.ptr(),
                                     built_trans_tmp_query.length(),
                                     TRUE, FALSE,
                                     is_drop_tmp_if_exists_added,
                                     0);
      }
      if (non_tmp_table_deleted)
      {
          /* Chop of the last comma */
          built_query.chop();
          built_query.append(" /* generated by server */");
          int error_code = non_tmp_error ?  thd->get_stmt_da()->sql_errno()
                                         : 0;
          error |= thd->binlog_query(THD::STMT_QUERY_TYPE,
                                     built_query.ptr(),
                                     built_query.length(),
                                     TRUE, FALSE, FALSE,
                                     error_code);
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

bool log_drop_table(THD *thd, const char *db_name, size_t db_name_length,
                    const char *table_name, size_t table_name_length,
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
  append_identifier(thd, &query, db_name, db_name_length);
  query.append(".");
  append_identifier(thd, &query, table_name, table_name_length);
  query.append(STRING_WITH_LEN("/* Generated to handle "
                               "failed CREATE OR REPLACE */"));
  error= thd->binlog_query(THD::STMT_QUERY_TYPE,
                           query.ptr(), query.length(),
                           FALSE, FALSE, temporary_table, 0);
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

bool quick_rm_table(THD *thd, handlerton *base, const char *db,
                    const char *table_name, uint flags)
{
  char path[FN_REFLEN + 1];
  bool error= 0;
  DBUG_ENTER("quick_rm_table");

  uint path_length= build_table_filename(path, sizeof(path) - 1,
                                         db, table_name, reg_ext, flags);
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
    PSI_CALL_drop_table_share(flags & FN_IS_TMP, db, strlen(db),
                              table_name, strlen(table_name));
  }

  DBUG_RETURN(error);
}


/*
  Sort keys in the following order:
  - PRIMARY KEY
  - UNIQUE keys where all column are NOT NULL
  - UNIQUE keys that don't contain partial segments
  - Other UNIQUE keys
  - Normal keys
  - Fulltext keys

  This will make checking for duplicated keys faster and ensure that
  PRIMARY keys are prioritized.
*/

static int sort_keys(KEY *a, KEY *b)
{
  ulong a_flags= a->flags, b_flags= b->flags;
  
  if (a_flags & HA_NOSAME)
  {
    if (!(b_flags & HA_NOSAME))
      return -1;
    if ((a_flags ^ b_flags) & HA_NULL_PART_KEY)
    {
      /* Sort NOT NULL keys before other keys */
      return (a_flags & HA_NULL_PART_KEY) ? 1 : -1;
    }
    if (a->name == primary_key_name)
      return -1;
    if (b->name == primary_key_name)
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
  return ((a->usable_key_parts < b->usable_key_parts) ? -1 :
	  (a->usable_key_parts > b->usable_key_parts) ? 1 :
	  0);
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


/*
  Check TYPELIB (set or enum) max and total lengths

  SYNOPSIS
    calculate_interval_lengths()
    cs            charset+collation pair of the interval
    typelib       list of values for the column
    max_length    length of the longest item
    tot_length    sum of the item lengths

  DESCRIPTION
    After this function call:
    - ENUM uses max_length
    - SET uses tot_length.

  RETURN VALUES
    void
*/
void calculate_interval_lengths(CHARSET_INFO *cs, TYPELIB *interval,
                                uint32 *max_length, uint32 *tot_length)
{
  const char **pos;
  uint *len;
  *max_length= *tot_length= 0;
  for (pos= interval->type_names, len= interval->type_lengths;
       *pos ; pos++, len++)
  {
    size_t length= cs->cset->numchars(cs, *pos, *pos + *len);
    *tot_length+= length;
    set_if_bigger(*max_length, (uint32)length);
  }
}


/*
  Prepare a create_table instance for packing

  SYNOPSIS
    prepare_create_field()
    sql_field     field to prepare for packing
    blob_columns  count for BLOBs
    table_flags   table flags

  DESCRIPTION
    This function prepares a Create_field instance.
    Fields such as pack_flag are valid after this call.

  RETURN VALUES
   0	ok
   1	Error
*/

int prepare_create_field(Column_definition *sql_field,
			 uint *blob_columns, 
			 longlong table_flags)
{
  unsigned int dup_val_count;
  DBUG_ENTER("prepare_create_field");

  /*
    This code came from mysql_prepare_create_table.
    Indent preserved to make patching easier
  */
  DBUG_ASSERT(sql_field->charset);

  switch (sql_field->sql_type) {
  case MYSQL_TYPE_BLOB:
  case MYSQL_TYPE_MEDIUM_BLOB:
  case MYSQL_TYPE_TINY_BLOB:
  case MYSQL_TYPE_LONG_BLOB:
    sql_field->pack_flag=FIELDFLAG_BLOB |
      pack_length_to_packflag(sql_field->pack_length -
                              portable_sizeof_char_ptr);
    if (sql_field->charset->state & MY_CS_BINSORT)
      sql_field->pack_flag|=FIELDFLAG_BINARY;
    sql_field->length=8;			// Unireg field length
    sql_field->unireg_check=Field::BLOB_FIELD;
    (*blob_columns)++;
    break;
  case MYSQL_TYPE_GEOMETRY:
#ifdef HAVE_SPATIAL
    if (!(table_flags & HA_CAN_GEOMETRY))
    {
      my_printf_error(ER_CHECK_NOT_IMPLEMENTED, ER(ER_CHECK_NOT_IMPLEMENTED),
                      MYF(0), "GEOMETRY");
      DBUG_RETURN(1);
    }
    sql_field->pack_flag=FIELDFLAG_GEOM |
      pack_length_to_packflag(sql_field->pack_length -
                              portable_sizeof_char_ptr);
    if (sql_field->charset->state & MY_CS_BINSORT)
      sql_field->pack_flag|=FIELDFLAG_BINARY;
    sql_field->length=8;			// Unireg field length
    sql_field->unireg_check=Field::BLOB_FIELD;
    (*blob_columns)++;
    break;
#else
    my_printf_error(ER_FEATURE_DISABLED,ER(ER_FEATURE_DISABLED), MYF(0),
                    sym_group_geom.name, sym_group_geom.needed_define);
    DBUG_RETURN(1);
#endif /*HAVE_SPATIAL*/
  case MYSQL_TYPE_VARCHAR:
#ifndef QQ_ALL_HANDLERS_SUPPORT_VARCHAR
    if (table_flags & HA_NO_VARCHAR)
    {
      /* convert VARCHAR to CHAR because handler is not yet up to date */
      sql_field->sql_type=    MYSQL_TYPE_VAR_STRING;
      sql_field->pack_length= calc_pack_length(sql_field->sql_type,
                                               (uint) sql_field->length);
      if ((sql_field->length / sql_field->charset->mbmaxlen) >
          MAX_FIELD_CHARLENGTH)
      {
        my_printf_error(ER_TOO_BIG_FIELDLENGTH, ER(ER_TOO_BIG_FIELDLENGTH),
                        MYF(0), sql_field->field_name,
                        static_cast<ulong>(MAX_FIELD_CHARLENGTH));
        DBUG_RETURN(1);
      }
    }
#endif
    /* fall through */
  case MYSQL_TYPE_STRING:
    sql_field->pack_flag=0;
    if (sql_field->charset->state & MY_CS_BINSORT)
      sql_field->pack_flag|=FIELDFLAG_BINARY;
    break;
  case MYSQL_TYPE_ENUM:
    sql_field->pack_flag=pack_length_to_packflag(sql_field->pack_length) |
      FIELDFLAG_INTERVAL;
    if (sql_field->charset->state & MY_CS_BINSORT)
      sql_field->pack_flag|=FIELDFLAG_BINARY;
    sql_field->unireg_check=Field::INTERVAL_FIELD;
    if (check_duplicates_in_interval("ENUM",sql_field->field_name,
                                     sql_field->interval,
                                     sql_field->charset, &dup_val_count))
      DBUG_RETURN(1);
    break;
  case MYSQL_TYPE_SET:
    sql_field->pack_flag=pack_length_to_packflag(sql_field->pack_length) |
      FIELDFLAG_BITFIELD;
    if (sql_field->charset->state & MY_CS_BINSORT)
      sql_field->pack_flag|=FIELDFLAG_BINARY;
    sql_field->unireg_check=Field::BIT_FIELD;
    if (check_duplicates_in_interval("SET",sql_field->field_name,
                                     sql_field->interval,
                                     sql_field->charset, &dup_val_count))
      DBUG_RETURN(1);
    /* Check that count of unique members is not more then 64 */
    if (sql_field->interval->count -  dup_val_count > sizeof(longlong)*8)
    {
       my_error(ER_TOO_BIG_SET, MYF(0), sql_field->field_name);
       DBUG_RETURN(1);
    }
    break;
  case MYSQL_TYPE_DATE:			// Rest of string types
  case MYSQL_TYPE_NEWDATE:
  case MYSQL_TYPE_TIME:
  case MYSQL_TYPE_DATETIME:
  case MYSQL_TYPE_TIME2:
  case MYSQL_TYPE_DATETIME2:
  case MYSQL_TYPE_NULL:
    sql_field->pack_flag=f_settype((uint) sql_field->sql_type);
    break;
  case MYSQL_TYPE_BIT:
    /* 
      We have sql_field->pack_flag already set here, see
      mysql_prepare_create_table().
    */
    break;
  case MYSQL_TYPE_NEWDECIMAL:
    sql_field->pack_flag=(FIELDFLAG_NUMBER |
                          (sql_field->flags & UNSIGNED_FLAG ? 0 :
                           FIELDFLAG_DECIMAL) |
                          (sql_field->flags & ZEROFILL_FLAG ?
                           FIELDFLAG_ZEROFILL : 0) |
                          (sql_field->decimals << FIELDFLAG_DEC_SHIFT));
    break;
  case MYSQL_TYPE_TIMESTAMP:
  case MYSQL_TYPE_TIMESTAMP2:
    /* fall-through */
  default:
    sql_field->pack_flag=(FIELDFLAG_NUMBER |
                          (sql_field->flags & UNSIGNED_FLAG ? 0 :
                           FIELDFLAG_DECIMAL) |
                          (sql_field->flags & ZEROFILL_FLAG ?
                           FIELDFLAG_ZEROFILL : 0) |
                          f_settype((uint) sql_field->sql_type) |
                          (sql_field->decimals << FIELDFLAG_DEC_SHIFT));
    break;
  }
  if (!(sql_field->flags & NOT_NULL_FLAG) ||
      (sql_field->vcol_info))  /* Make virtual columns allow NULL values */
    sql_field->pack_flag|= FIELDFLAG_MAYBE_NULL;
  if (sql_field->flags & NO_DEFAULT_VALUE_FLAG)
    sql_field->pack_flag|= FIELDFLAG_NO_DEFAULT;
  DBUG_RETURN(0);
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

CHARSET_INFO* get_sql_field_charset(Create_field *sql_field,
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
   explicit default, or to be a virtual column, then no promition is done.

   @param column_definitions The list of column definitions, in the physical
                             order in which they appear in the table.
 */
void promote_first_timestamp_column(List<Create_field> *column_definitions)
{
  List_iterator<Create_field> it(*column_definitions);
  Create_field *column_definition;

  while ((column_definition= it++) != NULL)
  {
    if (is_timestamp_type(column_definition->sql_type) ||              // TIMESTAMP
        column_definition->unireg_check == Field::TIMESTAMP_OLD_FIELD) // Legacy
    {
      if ((column_definition->flags & NOT_NULL_FLAG) != 0 && // NOT NULL,
          column_definition->def == NULL &&            // no constant default,
          column_definition->unireg_check == Field::NONE && // no function default
          column_definition->vcol_info == NULL)
      {
        DBUG_PRINT("info", ("First TIMESTAMP column '%s' was promoted to "
                            "DEFAULT CURRENT_TIMESTAMP ON UPDATE "
                            "CURRENT_TIMESTAMP",
                            column_definition->field_name
                            ));
        column_definition->unireg_check= Field::TIMESTAMP_DNUN_FIELD;
      }
      return;
    }
  }
}


/**
  Check if there is a duplicate key. Report a warning for every duplicate key.

  @param thd              Thread context.
  @param key              Key to be checked.
  @param key_info         Key meta-data info.
  @param key_list         List of existing keys.
*/
static void check_duplicate_key(THD *thd,
                                Key *key, KEY *key_info,
                                List<Key> *key_list)
{
  /*
    We only check for duplicate indexes if it is requested and the
    key is not auto-generated.

    Check is requested if the key was explicitly created or altered
    by the user (unless it's a foreign key).
  */
  if (!key->key_create_info.check_for_duplicate_indexes || key->generated)
    return;

  List_iterator<Key> key_list_iterator(*key_list);
  List_iterator<Key_part_spec> key_column_iterator(key->columns);
  Key *k;

  while ((k= key_list_iterator++))
  {
    // Looking for a similar key...

    if (k == key)
      break;

    if (k->generated ||
        (key->type != k->type) ||
        (key->key_create_info.algorithm != k->key_create_info.algorithm) ||
        (key->columns.elements != k->columns.elements))
    {
      // Keys are different.
      continue;
    }

    /*
      Keys 'key' and 'k' might be identical.
      Check that the keys have identical columns in the same order.
    */

    List_iterator<Key_part_spec> k_column_iterator(k->columns);

    bool all_columns_are_identical= true;

    key_column_iterator.rewind();

    for (uint i= 0; i < key->columns.elements; ++i)
    {
      Key_part_spec *c1= key_column_iterator++;
      Key_part_spec *c2= k_column_iterator++;

      DBUG_ASSERT(c1 && c2);

      if (my_strcasecmp(system_charset_info,
                        c1->field_name.str, c2->field_name.str) ||
          (c1->length != c2->length))
      {
        all_columns_are_identical= false;
        break;
      }
    }

    // Report a warning if we have two identical keys.

    DBUG_ASSERT(thd->lex->query_tables->alias);
    if (all_columns_are_identical)
    {
      push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE,
                          ER_DUP_INDEX, ER_THD(thd, ER_DUP_INDEX),
                          key_info->name,
                          thd->lex->query_tables->db,
                          thd->lex->query_tables->alias);
      break;
    }
  }
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
  uint		field,null_fields,blob_columns,max_key_length;
  ulong		record_offset= 0;
  KEY		*key_info;
  KEY_PART_INFO *key_part_info;
  int		field_no,dup_no;
  int		select_field_pos,auto_increment=0;
  List_iterator<Create_field> it(alter_info->create_list);
  List_iterator<Create_field> it2(alter_info->create_list);
  uint total_uneven_bit_length= 0;
  int select_field_count= C_CREATE_SELECT(create_table_mode);
  bool tmp_table= create_table_mode == C_ALTER_TABLE;
  DBUG_ENTER("mysql_prepare_create_table");

  select_field_pos= alter_info->create_list.elements - select_field_count;
  null_fields=blob_columns=0;
  create_info->varchar= 0;
  max_key_length= file->max_key_length();

  for (field_no=0; (sql_field=it++) ; field_no++)
  {
    CHARSET_INFO *save_cs;

    /*
      Initialize length from its original value (number of characters),
      which was set in the parser. This is necessary if we're
      executing a prepared statement for the second time.
    */
    sql_field->length= sql_field->char_length;
    /* Set field charset. */
    save_cs= sql_field->charset= get_sql_field_charset(sql_field, create_info);
    if ((sql_field->flags & BINCMP_FLAG) &&
	!(sql_field->charset= find_bin_collation(sql_field->charset)))
      DBUG_RETURN(TRUE);

    /*
      Convert the default value from client character
      set into the column character set if necessary.
    */
    if (sql_field->def && 
        save_cs != sql_field->def->collation.collation &&
        (sql_field->sql_type == MYSQL_TYPE_VAR_STRING ||
         sql_field->sql_type == MYSQL_TYPE_STRING ||
         sql_field->sql_type == MYSQL_TYPE_SET ||
         sql_field->sql_type == MYSQL_TYPE_ENUM))
    {
      /*
        Starting from 5.1 we work here with a copy of Create_field
        created by the caller, not with the instance that was
        originally created during parsing. It's OK to create
        a temporary item and initialize with it a member of the
        copy -- this item will be thrown away along with the copy
        at the end of execution, and thus not introduce a dangling
        pointer in the parsed tree of a prepared statement or a
        stored procedure statement.
      */
      sql_field->def= sql_field->def->safe_charset_converter(thd, save_cs);

      if (sql_field->def == NULL)
      {
        /* Could not convert */
        my_error(ER_INVALID_DEFAULT, MYF(0), sql_field->field_name);
        DBUG_RETURN(TRUE);
      }
    }

    if (sql_field->sql_type == MYSQL_TYPE_SET ||
        sql_field->sql_type == MYSQL_TYPE_ENUM)
    {
      uint32 dummy;
      CHARSET_INFO *cs= sql_field->charset;
      TYPELIB *interval= sql_field->interval;

      /*
        Create typelib from interval_list, and if necessary
        convert strings from client character set to the
        column character set.
      */
      if (!interval)
      {
        /*
          Create the typelib in runtime memory - we will free the
          occupied memory at the same time when we free this
          sql_field -- at the end of execution.
        */
        interval= sql_field->interval= typelib(thd->mem_root,
                                               sql_field->interval_list);
        List_iterator<String> int_it(sql_field->interval_list);
        String conv, *tmp;
        char comma_buf[5]; /* 5 bytes for 'filename' charset */
        DBUG_ASSERT(sizeof(comma_buf) >= cs->mbmaxlen);
        int comma_length= cs->cset->wc_mb(cs, ',', (uchar*) comma_buf,
                                          (uchar*) comma_buf +
                                          sizeof(comma_buf));
        DBUG_ASSERT(comma_length > 0);
        for (uint i= 0; (tmp= int_it++); i++)
        {
          size_t lengthsp;
          if (String::needs_conversion(tmp->length(), tmp->charset(),
                                       cs, &dummy))
          {
            uint cnv_errs;
            conv.copy(tmp->ptr(), tmp->length(), tmp->charset(), cs, &cnv_errs);
            interval->type_names[i]= strmake_root(thd->mem_root, conv.ptr(),
                                                  conv.length());
            interval->type_lengths[i]= conv.length();
          }

          // Strip trailing spaces.
          lengthsp= cs->cset->lengthsp(cs, interval->type_names[i],
                                       interval->type_lengths[i]);
          interval->type_lengths[i]= lengthsp;
          ((uchar *)interval->type_names[i])[lengthsp]= '\0';
          if (sql_field->sql_type == MYSQL_TYPE_SET)
          {
            if (cs->coll->instr(cs, interval->type_names[i], 
                                interval->type_lengths[i], 
                                comma_buf, comma_length, NULL, 0))
            {
              ErrConvString err(tmp->ptr(), tmp->length(), cs);
              my_error(ER_ILLEGAL_VALUE_FOR_TYPE, MYF(0), "set", err.ptr());
              DBUG_RETURN(TRUE);
            }
          }
        }
        sql_field->interval_list.empty(); // Don't need interval_list anymore
      }

      if (sql_field->sql_type == MYSQL_TYPE_SET)
      {
        uint32 field_length;
        if (sql_field->def != NULL)
        {
          char *not_used;
          uint not_used2;
          bool not_found= 0;
          String str, *def= sql_field->def->val_str(&str);
          if (def == NULL) /* SQL "NULL" maps to NULL */
          {
            if ((sql_field->flags & NOT_NULL_FLAG) != 0)
            {
              my_error(ER_INVALID_DEFAULT, MYF(0), sql_field->field_name);
              DBUG_RETURN(TRUE);
            }

            /* else, NULL is an allowed value */
            (void) find_set(interval, NULL, 0,
                            cs, &not_used, &not_used2, &not_found);
          }
          else /* not NULL */
          {
            (void) find_set(interval, def->ptr(), def->length(),
                            cs, &not_used, &not_used2, &not_found);
          }

          if (not_found)
          {
            my_error(ER_INVALID_DEFAULT, MYF(0), sql_field->field_name);
            DBUG_RETURN(TRUE);
          }
        }
        calculate_interval_lengths(cs, interval, &dummy, &field_length);
        sql_field->length= field_length + (interval->count - 1);
      }
      else  /* MYSQL_TYPE_ENUM */
      {
        uint32 field_length;
        DBUG_ASSERT(sql_field->sql_type == MYSQL_TYPE_ENUM);
        if (sql_field->def != NULL)
        {
          String str, *def= sql_field->def->val_str(&str);
          if (def == NULL) /* SQL "NULL" maps to NULL */
          {
            if ((sql_field->flags & NOT_NULL_FLAG) != 0)
            {
              my_error(ER_INVALID_DEFAULT, MYF(0), sql_field->field_name);
              DBUG_RETURN(TRUE);
            }

            /* else, the defaults yield the correct length for NULLs. */
          } 
          else /* not NULL */
          {
            def->length(cs->cset->lengthsp(cs, def->ptr(), def->length()));
            if (find_type2(interval, def->ptr(), def->length(), cs) == 0) /* not found */
            {
              my_error(ER_INVALID_DEFAULT, MYF(0), sql_field->field_name);
              DBUG_RETURN(TRUE);
            }
          }
        }
        calculate_interval_lengths(cs, interval, &field_length, &dummy);
        sql_field->length= field_length;
      }
      set_if_smaller(sql_field->length, MAX_FIELD_WIDTH-1);
    }

    if (sql_field->sql_type == MYSQL_TYPE_BIT)
    { 
      sql_field->pack_flag= FIELDFLAG_NUMBER;
      if (file->ha_table_flags() & HA_CAN_BIT_FIELD)
        total_uneven_bit_length+= sql_field->length & 7;
      else
        sql_field->pack_flag|= FIELDFLAG_TREAT_BIT_AS_CHAR;
    }

    sql_field->create_length_to_internal_length();
    if (prepare_blob_field(thd, sql_field))
      DBUG_RETURN(TRUE);

    if (!(sql_field->flags & NOT_NULL_FLAG))
      null_fields++;

    if (check_column_name(sql_field->field_name))
    {
      my_error(ER_WRONG_COLUMN_NAME, MYF(0), sql_field->field_name);
      DBUG_RETURN(TRUE);
    }

    /* Check if we have used the same field name before */
    for (dup_no=0; (dup_field=it2++) != sql_field; dup_no++)
    {
      if (my_strcasecmp(system_charset_info,
			sql_field->field_name,
			dup_field->field_name) == 0)
      {
	/*
	  If this was a CREATE ... SELECT statement, accept a field
	  redefinition if we are changing a field in the SELECT part
	*/
	if (field_no < select_field_pos || dup_no >= select_field_pos)
	{
	  my_error(ER_DUP_FIELDNAME, MYF(0), sql_field->field_name);
	  DBUG_RETURN(TRUE);
	}
	else
	{
	  /* Field redefined */

          /*
            If we are replacing a BIT field, revert the increment
            of total_uneven_bit_length that was done above.
          */
          if (sql_field->sql_type == MYSQL_TYPE_BIT &&
              file->ha_table_flags() & HA_CAN_BIT_FIELD)
            total_uneven_bit_length-= sql_field->length & 7;

	  sql_field->def=		dup_field->def;
	  sql_field->sql_type=		dup_field->sql_type;

          /*
            If we are replacing a field with a BIT field, we need
            to initialize pack_flag. Note that we do not need to
            increment total_uneven_bit_length here as this dup_field
            has already been processed.
          */
          if (sql_field->sql_type == MYSQL_TYPE_BIT)
          {
            sql_field->pack_flag= FIELDFLAG_NUMBER;
            if (!(file->ha_table_flags() & HA_CAN_BIT_FIELD))
              sql_field->pack_flag|= FIELDFLAG_TREAT_BIT_AS_CHAR;
          }

	  sql_field->charset=		(dup_field->charset ?
					 dup_field->charset :
					 create_info->default_table_charset);
	  sql_field->length=		dup_field->char_length;
          sql_field->pack_length=	dup_field->pack_length;
          sql_field->key_length=	dup_field->key_length;
	  sql_field->decimals=		dup_field->decimals;
	  sql_field->create_length_to_internal_length();
	  sql_field->unireg_check=	dup_field->unireg_check;
          /* 
            We're making one field from two, the result field will have
            dup_field->flags as flags. If we've incremented null_fields
            because of sql_field->flags, decrement it back.
          */
          if (!(sql_field->flags & NOT_NULL_FLAG))
            null_fields--;
	  sql_field->flags=		dup_field->flags;
          sql_field->interval=          dup_field->interval;
          sql_field->vcol_info=         dup_field->vcol_info;
	  it2.remove();			// Remove first (create) definition
	  select_field_pos--;
	  break;
	}
      }
    }
    /* Don't pack rows in old tables if the user has requested this */
    if ((sql_field->flags & BLOB_FLAG) ||
	(sql_field->sql_type == MYSQL_TYPE_VARCHAR &&
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

    if (prepare_create_field(sql_field, &blob_columns, 
			     file->ha_table_flags()))
      DBUG_RETURN(TRUE);
    if (sql_field->sql_type == MYSQL_TYPE_VARCHAR)
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
  }
  /* Update virtual fields' offset*/
  it.rewind();
  while ((sql_field=it++))
  {
    if (!sql_field->stored_in_db())
    {
      sql_field->offset= record_offset;
      record_offset+= sql_field->pack_length;
    }
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

  if (blob_columns && (file->ha_table_flags() & HA_NO_BLOBS))
  {
    my_error(ER_TABLE_CANT_HANDLE_BLOB, MYF(0), file->table_type());
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
      if (((Foreign_key *)key)->validate(alter_info->create_list))
        DBUG_RETURN(TRUE);
      Foreign_key *fk_key= (Foreign_key*) key;
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
    if (check_string_char_length(&key->name, 0, NAME_CHAR_LEN,
                                 system_charset_info, 1))
    {
      my_error(ER_TOO_LONG_IDENT, MYF(0), key->name.str);
      DBUG_RETURN(TRUE);
    }
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

    if (key_info->block_size)
      key_info->flags|= HA_USES_BLOCK_SIZE;

    List_iterator<Key_part_spec> cols(key->columns), cols2(key->columns);
    CHARSET_INFO *ft_key_charset=0;  // for FULLTEXT
    for (uint column_nr=0 ; (column=cols++) ; column_nr++)
    {
      Key_part_spec *dup_column;

      it.rewind();
      field=0;
      while ((sql_field=it++) &&
	     my_strcasecmp(system_charset_info,
			   column->field_name.str,
			   sql_field->field_name))
	field++;
      if (!sql_field)
      {
	my_error(ER_KEY_COLUMN_DOES_NOT_EXITS, MYF(0), column->field_name.str);
	DBUG_RETURN(TRUE);
      }
      while ((dup_column= cols2++) != column)
      {
        if (!my_strcasecmp(system_charset_info,
	     	           column->field_name.str, dup_column->field_name.str))
	{
	  my_printf_error(ER_DUP_FIELDNAME,
			  ER_THD(thd, ER_DUP_FIELDNAME),MYF(0),
			  column->field_name.str);
	  DBUG_RETURN(TRUE);
	}
      }
      cols2.rewind();
      if (key->type == Key::FULLTEXT)
      {
	if ((sql_field->sql_type != MYSQL_TYPE_STRING &&
	     sql_field->sql_type != MYSQL_TYPE_VARCHAR &&
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
	    my_error(ER_BLOB_KEY_WITHOUT_LENGTH, MYF(0), column->field_name.str);
	    DBUG_RETURN(TRUE);
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
        if (!sql_field->stored_in_db())
        {
          /* Key fields must always be physically stored. */
          my_error(ER_KEY_BASED_ON_GENERATED_VIRTUAL_COLUMN, MYF(0));
          DBUG_RETURN(TRUE);
        }
        if (key->type == Key::PRIMARY && sql_field->vcol_info)
        {
          my_error(ER_PRIMARY_KEY_BASED_ON_VIRTUAL_COLUMN, MYF(0));
          DBUG_RETURN(TRUE);
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
                               blob_length_by_type(sql_field->sql_type)
                               * sql_field->charset->mbmaxlen);
	  if (key_part_length > max_key_length ||
	      key_part_length > file->max_key_part_length())
	  {
	    key_part_length= MY_MIN(max_key_length, file->max_key_part_length());
	    if (key->type == Key::MULTIPLE)
	    {
	      /* not a critical problem */
	      push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                                  ER_TOO_LONG_KEY,
                                  ER_THD(thd, ER_TOO_LONG_KEY),
                                  key_part_length);
              /* Align key length to multibyte char boundary */
              key_part_length-= key_part_length % sql_field->charset->mbmaxlen;
	    }
	    else
	    {
	      my_error(ER_TOO_LONG_KEY, MYF(0), key_part_length);
	      DBUG_RETURN(TRUE);
	    }
	  }
	}
        // Catch invalid use of partial keys 
	else if (!f_is_geom(sql_field->pack_flag) &&
                 // is the key partial? 
                 column->length != key_part_length &&
                 // is prefix length bigger than field length? 
                 (column->length > key_part_length ||
                  // can the field have a partial key? 
                  !Field::type_can_have_key_part (sql_field->sql_type) ||
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
      else if (key_part_length == 0 && (sql_field->flags & NOT_NULL_FLAG))
      {
	my_error(ER_WRONG_KEY_COLUMN, MYF(0), file->table_type(),
                 column->field_name.str);
	  DBUG_RETURN(TRUE);
      }
      if (key_part_length > file->max_key_part_length() &&
          key->type != Key::FULLTEXT)
      {
        key_part_length= file->max_key_part_length();
	if (key->type == Key::MULTIPLE)
	{
	  /* not a critical problem */
	  push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                              ER_TOO_LONG_KEY, ER_THD(thd, ER_TOO_LONG_KEY),
                              key_part_length);
          /* Align key length to multibyte char boundary */
          key_part_length-= key_part_length % sql_field->charset->mbmaxlen;
	}
	else
	{
	  my_error(ER_TOO_LONG_KEY, MYF(0), key_part_length);
	  DBUG_RETURN(TRUE);
	}
      }
      key_part_info->length= (uint16) key_part_length;
      /* Use packed keys for long strings on the first column */
      if (!((*db_options) & HA_OPTION_NO_PACK_KEYS) &&
          !((create_info->table_options & HA_OPTION_NO_PACK_KEYS)) &&
	  (key_part_length >= KEY_DEFAULT_PACK_LENGTH &&
	   (sql_field->sql_type == MYSQL_TYPE_STRING ||
	    sql_field->sql_type == MYSQL_TYPE_VARCHAR ||
	    sql_field->pack_flag & FIELDFLAG_BLOB)))
      {
	if ((column_nr == 0 && (sql_field->pack_flag & FIELDFLAG_BLOB)) ||
            sql_field->sql_type == MYSQL_TYPE_VARCHAR)
	  key_info->flags|= HA_BINARY_PACK_KEY | HA_VAR_LENGTH_KEY;
	else
	  key_info->flags|= HA_PACK_KEY;
      }
      /* Check if the key segment is partial, set the key flag accordingly */
      if (key_part_length != sql_field->key_length)
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
	  key_name=make_unique_key_name(thd, sql_field->field_name,
					*key_info_buffer, key_info);
	if (check_if_keyname_exists(key_name, *key_info_buffer, key_info))
	{
	  my_error(ER_DUP_KEYNAME, MYF(0), key_name);
	  DBUG_RETURN(TRUE);
	}
	key_info->name=(char*) key_name;
      }
    }
    if (!key_info->name || check_column_name(key_info->name))
    {
      my_error(ER_WRONG_NAME_FOR_INDEX, MYF(0), key_info->name);
      DBUG_RETURN(TRUE);
    }
    if (key->type == Key::UNIQUE && !(key_info->flags & HA_NULL_PART_KEY))
      unique_key=1;
    key_info->key_length=(uint16) key_length;
    if (key_length > max_key_length && key->type != Key::FULLTEXT)
    {
      my_error(ER_TOO_LONG_KEY,MYF(0),max_key_length);
      DBUG_RETURN(TRUE);
    }

    if (validate_comment_length(thd, &key->key_create_info.comment,
                                INDEX_COMMENT_MAXLEN, ER_TOO_LONG_INDEX_COMMENT,
                                key_info->name))
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

  if (!unique_key && !primary_key &&
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
    if (!sql_field->def &&
        !sql_field->has_default_function() &&
        (sql_field->flags & NOT_NULL_FLAG) &&
        !is_timestamp_type(sql_field->sql_type))
    {
      sql_field->flags|= NO_DEFAULT_VALUE_FLAG;
      sql_field->pack_flag|= FIELDFLAG_NO_DEFAULT;
    }

    if (thd->variables.sql_mode & MODE_NO_ZERO_DATE &&
        !sql_field->def &&
        is_timestamp_type(sql_field->sql_type) &&
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

      my_error(ER_INVALID_DEFAULT, MYF(0), sql_field->field_name);
      DBUG_RETURN(TRUE);
    }
  }

  /* Give warnings for not supported table options */
#if defined(WITH_ARIA_STORAGE_ENGINE)
  extern handlerton *maria_hton;
  if (file->ht != maria_hton)
#endif
    if (create_info->transactional)
      push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                          ER_ILLEGAL_HA_CREATE_OPTION,
                          ER_THD(thd, ER_ILLEGAL_HA_CREATE_OPTION),
                          file->engine_name()->str,
                          "TRANSACTIONAL=1");

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
bool validate_comment_length(THD *thd, LEX_STRING *comment, size_t max_len,
                             uint err_code, const char *name)
{
  DBUG_ENTER("validate_comment_length");
  uint tmp_len= my_charpos(system_charset_info, comment->str,
                           comment->str + comment->length, max_len);
  if (tmp_len < comment->length)
  {
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
    If the table character set was not given explicitely,
    let's fetch the database default character set and
    apply it to the table.
*/

static void set_table_default_charset(THD *thd,
				      HA_CREATE_INFO *create_info, char *db)
{
  /*
    If the table character set was not given explicitly,
    let's fetch the database default character set and
    apply it to the table.
  */
  if (!create_info->default_table_charset)
  {
    Schema_specification_st db_info;

    load_db_opt_by_name(thd, db, &db_info);

    create_info->default_table_charset= db_info.default_table_charset;
  }
}


/*
  Extend long VARCHAR fields to blob & prepare field if it's a blob

  SYNOPSIS
    prepare_blob_field()
    sql_field		Field to check

  RETURN
    0	ok
    1	Error (sql_field can't be converted to blob)
        In this case the error is given
*/

static bool prepare_blob_field(THD *thd, Column_definition *sql_field)
{
  DBUG_ENTER("prepare_blob_field");

  if (sql_field->length > MAX_FIELD_VARCHARLENGTH &&
      !(sql_field->flags & BLOB_FLAG))
  {
    /* Convert long VARCHAR columns to TEXT or BLOB */
    char warn_buff[MYSQL_ERRMSG_SIZE];

    if (sql_field->def || thd->is_strict_mode())
    {
      my_error(ER_TOO_BIG_FIELDLENGTH, MYF(0), sql_field->field_name,
               static_cast<ulong>(MAX_FIELD_VARCHARLENGTH /
                                  sql_field->charset->mbmaxlen));
      DBUG_RETURN(1);
    }
    sql_field->sql_type= MYSQL_TYPE_BLOB;
    sql_field->flags|= BLOB_FLAG;
    my_snprintf(warn_buff, sizeof(warn_buff), ER_THD(thd, ER_AUTO_CONVERT),
                sql_field->field_name,
                (sql_field->charset == &my_charset_bin) ? "VARBINARY" :
                "VARCHAR",
                (sql_field->charset == &my_charset_bin) ? "BLOB" : "TEXT");
    push_warning(thd, Sql_condition::WARN_LEVEL_NOTE, ER_AUTO_CONVERT,
                 warn_buff);
  }

  if ((sql_field->flags & BLOB_FLAG) && sql_field->length)
  {
    if (sql_field->sql_type == FIELD_TYPE_BLOB ||
        sql_field->sql_type == FIELD_TYPE_TINY_BLOB ||
        sql_field->sql_type == FIELD_TYPE_MEDIUM_BLOB)
    {
      /* The user has given a length to the blob column */
      sql_field->sql_type= get_blob_type_from_length(sql_field->length);
      sql_field->pack_length= calc_pack_length(sql_field->sql_type, 0);
    }
    sql_field->length= 0;
  }
  DBUG_RETURN(0);
}


/*
  Preparation of Create_field for SP function return values.
  Based on code used in the inner loop of mysql_prepare_create_table()
  above.

  SYNOPSIS
    sp_prepare_create_field()
    thd			Thread object
    sql_field		Field to prepare

  DESCRIPTION
    Prepares the field structures for field creation.

*/

void sp_prepare_create_field(THD *thd, Column_definition *sql_field)
{
  if (sql_field->sql_type == MYSQL_TYPE_SET ||
      sql_field->sql_type == MYSQL_TYPE_ENUM)
  {
    uint32 field_length, dummy;
    if (sql_field->sql_type == MYSQL_TYPE_SET)
    {
      calculate_interval_lengths(sql_field->charset,
                                 sql_field->interval, &dummy, 
                                 &field_length);
      sql_field->length= field_length + 
                         (sql_field->interval->count - 1);
    }
    else /* MYSQL_TYPE_ENUM */
    {
      calculate_interval_lengths(sql_field->charset,
                                 sql_field->interval,
                                 &field_length, &dummy);
      sql_field->length= field_length;
    }
    set_if_smaller(sql_field->length, MAX_FIELD_WIDTH-1);
  }

  if (sql_field->sql_type == MYSQL_TYPE_BIT)
  {
    sql_field->pack_flag= FIELDFLAG_NUMBER |
                          FIELDFLAG_TREAT_BIT_AS_CHAR;
  }
  sql_field->create_length_to_internal_length();
  DBUG_ASSERT(sql_field->def == 0);
  /* Can't go wrong as sql_field->def is not defined */
  (void) prepare_blob_field(thd, sql_field);
}


handler *mysql_create_frm_image(THD *thd,
                                const char *db, const char *table_name,
                                HA_CREATE_INFO *create_info,
                                Alter_info *alter_info, int create_table_mode,
                                KEY **key_info,
                                uint *key_count,
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

  set_table_default_charset(thd, create_info, (char*) db);

  db_options= create_info->table_options;
  if (create_info->row_type == ROW_TYPE_DYNAMIC ||
      create_info->row_type == ROW_TYPE_PAGE)
    db_options|= HA_OPTION_PACK_RECORD;

  if (!(file= get_new_handler((TABLE_SHARE*) 0, thd->mem_root,
                              create_info->db_type)))
  {
    mem_alloc_error(sizeof(handler));
    DBUG_RETURN(NULL);
  }
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
    if (!part_info)
    {
      mem_alloc_error(sizeof(partition_info));
      goto err;
    }
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
        LEX_STRING comment= {
          part_elem->part_comment, strlen(part_elem->part_comment)
        };
        if (validate_comment_length(thd, &comment,
                                     TABLE_PARTITION_COMMENT_MAXLEN,
                                     ER_TOO_LONG_TABLE_PARTITION_COMMENT,
                                     part_elem->partition_name))
          DBUG_RETURN(NULL);
        part_elem->part_comment[comment.length]= '\0';
      }
      if (part_elem->subpartitions.elements)
      {
        List_iterator<partition_element> sub_it(part_elem->subpartitions);
        partition_element *subpart_elem;
        while ((subpart_elem= sub_it++))
        {
          if (subpart_elem->part_comment)
          {
            LEX_STRING comment= {
              subpart_elem->part_comment, strlen(subpart_elem->part_comment)
            };
            if (validate_comment_length(thd, &comment,
                                         TABLE_PARTITION_COMMENT_MAXLEN,
                                         ER_TOO_LONG_TABLE_PARTITION_COMMENT,
                                         subpart_elem->partition_name))
              DBUG_RETURN(NULL);
            subpart_elem->part_comment[comment.length]= '\0';
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

    /*
      We reverse the partitioning parser and generate a standard format
      for syntax stored in frm file.
    */
    if (!(part_syntax_buf= generate_partition_syntax(thd, part_info,
                                                     &syntax_len,
                                                     TRUE, TRUE,
                                                     create_info,
                                                     alter_info,
                                                     NULL)))
      goto err;
    part_info->part_info_string= part_syntax_buf;
    part_info->part_info_len= syntax_len;
    if ((!(engine_type->partition_flags &&
           engine_type->partition_flags() & HA_CAN_PARTITION)) ||
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
      if (!(file= get_new_handler((TABLE_SHARE*) 0, thd->mem_root,
                                  engine_type)))
      {
        mem_alloc_error(sizeof(handler));
        DBUG_RETURN(NULL);
      }
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

  if (mysql_prepare_create_table(thd, create_info, alter_info, &db_options,
                                 file, key_info, key_count,
                                 create_table_mode))
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

  If one creates a temporary table, this is automatically opened

  Note that this function assumes that caller already have taken
  exclusive metadata lock on table being created or used some other
  way to ensure that concurrent operations won't intervene.
  mysql_create_table() is a wrapper that can be used for this.

  @retval 0 OK
  @retval 1 error
  @retval -1 table existed but IF EXISTS was used
*/

static
int create_table_impl(THD *thd,
                       const char *orig_db, const char *orig_table_name,
                       const char *db, const char *table_name,
                       const char *path,
                       const DDL_options_st options,
                       HA_CREATE_INFO *create_info,
                       Alter_info *alter_info,
                       int create_table_mode,
                       bool *is_trans,
                       KEY **key_info,
                       uint *key_count,
                       LEX_CUSTRING *frm)
{
  const char	*alias;
  handler	*file= 0;
  int		error= 1;
  bool          frm_only= create_table_mode == C_ALTER_TABLE_FRM_ONLY;
  bool          internal_tmp_table= create_table_mode == C_ALTER_TABLE || frm_only;
  DBUG_ENTER("mysql_create_table_no_lock");
  DBUG_PRINT("enter", ("db: '%s'  table: '%s'  tmp: %d  path: %s",
                       db, table_name, internal_tmp_table, path));

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
  if (error_if_data_home_dir(create_info->data_file_name,  "DATA DIRECTORY") ||
      error_if_data_home_dir(create_info->index_file_name, "INDEX DIRECTORY")||
      check_partition_dirs(thd->lex->part_info))
    goto err;

  alias= table_case_name(create_info, table_name);

  /* Check if table exists */
  if (create_info->tmp_table())
  {
    TABLE *tmp_table;
    if (find_and_use_temporary_table(thd, db, table_name, &tmp_table))
      goto err;
    if (tmp_table)
    {
      bool table_creation_was_logged= tmp_table->s->table_creation_was_logged;
      if (options.or_replace())
      {
        bool tmp;
        /*
          We are using CREATE OR REPLACE on an existing temporary table
          Remove the old table so that we can re-create it.
        */
        if (drop_temporary_table(thd, tmp_table, &tmp))
          goto err;
      }
      else if (options.if_not_exists())
        goto warn;
      else
      {
        my_error(ER_TABLE_EXISTS_ERROR, MYF(0), alias);
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
    if (!internal_tmp_table && ha_table_exists(thd, db, table_name))
    {
      if (options.or_replace())
      {
        TABLE_LIST table_list;
        table_list.init_one_table(db, strlen(db), table_name,
                                  strlen(table_name), table_name,
                                  TL_WRITE_ALLOW_WRITE);
        table_list.table= create_info->table;

        if (check_if_log_table(&table_list, TRUE, "CREATE OR REPLACE"))
          goto err;
        
        /*
          Rollback the empty transaction started in mysql_create_table()
          call to open_and_lock_tables() when we are using LOCK TABLES.
        */
        (void) trans_rollback_stmt(thd);
        /* Remove normal table without logging. Keep tables locked */
        if (mysql_rm_table_no_locks(thd, &table_list, 0, 0, 0, 1, 1))
          goto err;

        /*
          We have to log this query, even if it failed later to ensure the
          drop is done.
        */
        thd->variables.option_bits|= OPTION_KEEP_LOG;
        thd->log_current_statement= 1;
        create_info->table_was_deleted= 1;
        DBUG_EXECUTE_IF("send_kill_after_delete", thd->killed= KILL_QUERY; );

        /*
          Restart statement transactions for the case of CREATE ... SELECT.
        */
        if (thd->lex->select_lex.item_list.elements &&
            restart_trans_for_tables(thd, thd->lex->query_tables))
          goto err;
      }
      else if (options.if_not_exists())
        goto warn;
      else
      {
        my_error(ER_TABLE_EXISTS_ERROR, MYF(0), table_name);
        goto err;
      }
    }
  }

  THD_STAGE_INFO(thd, stage_creating_table);

  if (check_engine(thd, orig_db, orig_table_name, create_info))
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

    init_tmp_table_share(thd, &share, db, 0, table_name, path);

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
    if (!file)
      goto err;
    if (rea_create_table(thd, frm, path, db, table_name, create_info,
                         file, frm_only))
      goto err;
  }

  create_info->table= 0;
  if (!frm_only && create_info->tmp_table())
  {
    /*
      Open a table (skipping table cache) and add it into
      THD::temporary_tables list.
    */

    TABLE *table= open_table_uncached(thd, create_info->db_type, frm, path,
                                      db, table_name, true, true);

    if (!table)
    {
      (void) rm_temporary_table(create_info->db_type, path);
      goto err;
    }

    if (is_trans != NULL)
      *is_trans= table->file->has_transactions();

    thd->thread_specific_used= TRUE;
    create_info->table= table;                  // Store pointer to table
  }
#ifdef WITH_PARTITION_STORAGE_ENGINE
  else if (thd->work_part_info && frm_only)
  {
    /*
      For partitioned tables we can't find some problems with table
      until table is opened. Therefore in order to disallow creation
      of corrupted tables we have to try to open table as the part
      of its creation process.
      In cases when both .FRM and SE part of table are created table
      is implicitly open in ha_create_table() call.
      In cases when we create .FRM without SE part we have to open
      table explicitly.
    */
    TABLE table;
    TABLE_SHARE share;

    init_tmp_table_share(thd, &share, db, 0, table_name, path);

    bool result= (open_table_def(thd, &share, GTS_TABLE) ||
                  open_table_from_share(thd, &share, "", 0, (uint) READ_ALL,
                                        0, &table, true));
    if (!result)
      (void) closefrm(&table, 0);

    free_table_share(&share);

    if (result)
    {
      char frm_name[FN_REFLEN];
      strxnmov(frm_name, sizeof(frm_name), path, reg_ext, NullS);
      (void) mysql_file_delete(key_file_frm, frm_name, MYF(0));
      (void) file->ha_create_partitioning_metadata(path, NULL, CHF_DELETE_FLAG);
      goto err;
    }
  }
#endif

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
                      alias);
  goto err;
}

/**
  Simple wrapper around create_table_impl() to be used
  in various version of CREATE TABLE statement.
*/

int mysql_create_table_no_lock(THD *thd,
                                const char *db, const char *table_name,
                                Table_specification_st *create_info,
                                Alter_info *alter_info, bool *is_trans,
                                int create_table_mode)
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
    const char *alias= table_case_name(create_info, table_name);
    length= build_table_filename(path, sizeof(path) - 1, db, alias,
                                 "", 0);
    // Check if we hit FN_REFLEN bytes along with file extension.
    if (length+reg_ext_length > FN_REFLEN)
    {
      my_error(ER_IDENT_CAUSES_TOO_LONG_PATH, MYF(0), sizeof(path)-1, path);
      return true;
    }
  }

  res= create_table_impl(thd, db, table_name, db, table_name, path,
                         *create_info, create_info,
                         alter_info, create_table_mode,
                         is_trans, &not_used_1, &not_used_2, &frm);
  my_free(const_cast<uchar*>(frm.str));
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
  const char *db= create_table->db;
  const char *table_name= create_table->table_name;
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

  if (mysql_create_table_no_lock(thd, db, table_name, create_info, alter_info,
                                 &is_trans, create_table_mode) > 0)
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
    /*
      Add back the deleted table and re-created table as a locked table
      This should always work as we have a meta lock on the table.
     */
    thd->locked_tables_list.add_back_last_deleted_lock(pos_in_locked_tables);
    if (thd->locked_tables_list.reopen_tables(thd))
    {
      thd->locked_tables_list.unlink_all_closed_tables(thd, NULL, 0);
      result= 1;
    }
    else
    {
      TABLE *table= pos_in_locked_tables->table;
      table->mdl_ticket->downgrade_lock(MDL_SHARED_NO_READ_WRITE);
    }
  }

err:
  /* In RBR we don't need to log CREATE TEMPORARY TABLE */
  if (thd->is_current_stmt_binlog_format_row() && create_info->tmp_table())
    DBUG_RETURN(result);

  /* Write log if no error or if we already deleted a table */
  if (!result || thd->log_current_statement)
  {
    if (result && create_info->table_was_deleted)
    {
      /*
        Possible locked table was dropped. We should remove meta data locks
        associated with it and do UNLOCK_TABLES if no more locked tables.
      */
      thd->locked_tables_list.unlock_locked_table(thd, mdl_ticket);
    }
    else if (!result && create_info->tmp_table() && create_info->table)
    {
      /*
        Remember that tmp table creation was logged so that we know if
        we should log a delete of it.
      */
      create_info->table->s->table_creation_was_logged= 1;
    }
    if (write_bin_log(thd, result ? FALSE : TRUE, thd->query(),
                      thd->query_length(), is_trans))
      result= 1;
  }
  DBUG_RETURN(result);
}


/*
** Give the key name after the first field with an optional '_#' after
**/

static bool
check_if_keyname_exists(const char *name, KEY *start, KEY *end)
{
  for (KEY *key=start ; key != end ; key++)
    if (!my_strcasecmp(system_charset_info,name,key->name))
      return 1;
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


/****************************************************************************
** Alter a table definition
****************************************************************************/


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
mysql_rename_table(handlerton *base, const char *old_db,
                   const char *old_name, const char *new_db,
                   const char *new_name, uint flags)
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
                       old_db, old_name, new_db, new_name));

  // Temporarily disable foreign key checks
  if (flags & NO_FK_CHECKS) 
    thd->variables.option_bits|= OPTION_NO_FOREIGN_KEY_CHECKS;

  file= get_new_handler((TABLE_SHARE*) 0, thd->mem_root, base);

  build_table_filename(from, sizeof(from) - 1, old_db, old_name, "",
                       flags & FN_FROM_IS_TMP);
  length= build_table_filename(to, sizeof(to) - 1, new_db, new_name, "",
                               flags & FN_TO_IS_TMP);
  // Check if we hit FN_REFLEN bytes along with file extension.
  if (length+reg_ext_length > FN_REFLEN)
  {
    my_error(ER_IDENT_CAUSES_TOO_LONG_PATH, MYF(0), sizeof(to)-1, to);
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
    strmov(tmp_name, old_name);
    my_casedn_str(files_charset_info, tmp_name);
    strmov(tmp_db_name, old_db);
    my_casedn_str(files_charset_info, tmp_db_name);

    build_table_filename(lc_from, sizeof(lc_from) - 1, tmp_db_name, tmp_name,
                         "", flags & FN_FROM_IS_TMP);
    from_base= lc_from;

    strmov(tmp_name, new_name);
    my_casedn_str(files_charset_info, tmp_name);
    strmov(tmp_db_name, new_db);
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
  else if (!file || !(error=file->ha_rename_table(from_base, to_base)))
  {
    if (!(flags & NO_FRM_RENAME) && rename_file_ext(from,to,reg_ext))
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
                              old_db, strlen(old_db),
                              old_name, strlen(old_name));
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
  bool res= TRUE;
  bool is_trans= FALSE;
  bool do_logging= FALSE;
  uint not_used;
  int create_res;
  DBUG_ENTER("mysql_create_like_table");

#ifdef WITH_WSREP
  if (WSREP_ON && !thd->wsrep_applier &&
      wsrep_create_like_table(thd, table, src_table, create_info))
    DBUG_RETURN(res);
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

  /* Copy temporarily the statement flags to thd for lock_table_names() */
  // QQ: is this really needed???
  uint save_thd_create_info_options= thd->lex->create_info.options;
  thd->lex->create_info.options|= create_info->options;
  res= open_tables(thd, &thd->lex->query_tables, &not_used, 0);
  thd->lex->create_info.options= save_thd_create_info_options;

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
  local_create_info.options|= create_info->tmp_table();
  /* Reset auto-increment counter for the new table. */
  local_create_info.auto_increment_value= 0;
  /*
    Do not inherit values of DATA and INDEX DIRECTORY options from
    the original table. This is documented behavior.
  */
  local_create_info.data_file_name= local_create_info.index_file_name= NULL;

  /* The following is needed only in case of lock tables */
  if ((local_create_info.table= thd->lex->query_tables->table))
    pos_in_locked_tables= local_create_info.table->pos_in_locked_tables;    

  res= ((create_res=
         mysql_create_table_no_lock(thd, table->db, table->table_name,
                                    &local_create_info, &local_alter_info,
                                    &is_trans, C_ORDINARY_CREATE)) > 0);
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
    if (thd->locked_tables_list.reopen_tables(thd))
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
                thd->mdl_context.is_lock_owner(MDL_key::TABLE, table->db,
                                               table->table_name,
                                               MDL_EXCLUSIVE));
  }

  DEBUG_SYNC(thd, "create_table_like_before_binlog");

  /*
    We have to write the query before we unlock the tables.
  */
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
    if (!res && create_info->tmp_table() && local_create_info.table)
    {
      /*
        Remember that tmp table creation was logged so that we know if
        we should log a delete of it.
      */
      local_create_info.table->s->table_creation_was_logged= 1;
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
      log_drop_table(thd, table->db, table->db_length,
                     table->table_name, table->table_name_length,
                     create_info->tmp_table());
    }
    else if (write_bin_log(thd, res ? FALSE : TRUE, thd->query(),
                           thd->query_length(), is_trans))
      res= 1;
  }

  DBUG_RETURN(res);
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
  table_list->required_type= FRMTYPE_TABLE;

  if (open_and_lock_tables(thd, table_list, FALSE, 0,
                           &alter_prelocking_strategy))
  {
    thd->tablespace_op=FALSE;
    DBUG_RETURN(-1);
  }

  error= table_list->table->file->ha_discard_or_import_tablespace(discard);

  THD_STAGE_INFO(thd, stage_end);

  if (error)
    goto err;

  /*
    The 0 in the call below means 'not in a transaction', which means
    immediate invalidation; that is probably what we wish here
  */
  query_cache_invalidate3(thd, table_list, 0);

  /* The ALTER TABLE is always in its own transaction */
  error= trans_commit_stmt(thd);
  if (trans_commit_implicit(thd))
    error=1;
  if (error)
    goto err;
  error= write_bin_log(thd, FALSE, thd->query(), thd->query_length());

err:
  thd->tablespace_op=FALSE;

  if (error == 0)
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

  if (!(key->flags & HA_NOSAME) || (key->flags & HA_NULL_PART_KEY))
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

   DESCRIPTION
     Looks for the IF [NOT] EXISTS options, checks the states and remove items
     from the list if existing found.

   RETURN VALUES
     NONE
*/

static void
handle_if_exists_options(THD *thd, TABLE *table, Alter_info *alter_info)
{
  Field **f_ptr;
  DBUG_ENTER("handle_if_exists_option");

  /* Handle ADD COLUMN IF NOT EXISTS. */
  {
    List_iterator<Create_field> it(alter_info->create_list);
    Create_field *sql_field;

    while ((sql_field=it++))
    {
      if (!sql_field->create_if_not_exists || sql_field->change)
        continue;
      /*
         If there is a field with the same name in the table already,
         remove the sql_field from the list.
      */
      for (f_ptr=table->field; *f_ptr; f_ptr++)
      {
        if (my_strcasecmp(system_charset_info,
              sql_field->field_name, (*f_ptr)->field_name) == 0)
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
          if (my_strcasecmp(system_charset_info,
                sql_field->field_name, chk_field->field_name) == 0)
            goto drop_create_field;
        }
      }
      continue;
drop_create_field:
      push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE,
                          ER_DUP_FIELDNAME, ER_THD(thd, ER_DUP_FIELDNAME),
                          sql_field->field_name);
      it.remove();
      if (alter_info->create_list.is_empty())
      {
        alter_info->flags&= ~Alter_info::ALTER_ADD_COLUMN;
        if (alter_info->key_list.is_empty())
          alter_info->flags&= ~(Alter_info::ALTER_ADD_INDEX |
              Alter_info::ADD_FOREIGN_KEY);
      }
    }
  }

  /* Handle MODIFY COLUMN IF EXISTS. */
  {
    List_iterator<Create_field> it(alter_info->create_list);
    Create_field *sql_field;

    while ((sql_field=it++))
    {
      if (!sql_field->create_if_not_exists || !sql_field->change)
        continue;
      /*
         If there is NO field with the same name in the table already,
         remove the sql_field from the list.
      */
      for (f_ptr=table->field; *f_ptr; f_ptr++)
      {
        if (my_strcasecmp(system_charset_info,
              sql_field->change, (*f_ptr)->field_name) == 0)
        {
          break;
        }
      }
      if (*f_ptr == NULL)
      {
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE,
                            ER_BAD_FIELD_ERROR,
                            ER_THD(thd, ER_BAD_FIELD_ERROR),
                            sql_field->change, table->s->table_name.str);
        it.remove();
        if (alter_info->create_list.is_empty())
        {
          alter_info->flags&= ~(Alter_info::ALTER_ADD_COLUMN |
                                Alter_info::ALTER_CHANGE_COLUMN);
          if (alter_info->key_list.is_empty())
            alter_info->flags&= ~Alter_info::ALTER_ADD_INDEX;
        }
      }
    }
  }

  /* Handle DROP COLUMN/KEY IF EXISTS. */
  {
    List_iterator<Alter_drop> drop_it(alter_info->drop_list);
    Alter_drop *drop;
    bool remove_drop;
    while ((drop= drop_it++))
    {
      if (!drop->drop_if_exists)
        continue;
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
                            drop->name, (*f_ptr)->field_name) == 0)
          {
            remove_drop= FALSE;
            break;
          }
        }
      }
      else /* Alter_drop::KEY */
      {
        uint n_key;
        if (drop->type != Alter_drop::FOREIGN_KEY)
        {
          for (n_key=0; n_key < table->s->keys; n_key++)
          {
            if (my_strcasecmp(system_charset_info,
                  drop->name, table->key_info[n_key].name) == 0)
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
                            drop->name);
        drop_it.remove();
        if (alter_info->drop_list.is_empty())
          alter_info->flags&= ~(Alter_info::ALTER_DROP_COLUMN |
                                Alter_info::ALTER_DROP_INDEX  |
                                Alter_info::DROP_FOREIGN_KEY);
      }
    }
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
      bool dup_primary_key= key->type == Key::PRIMARY &&
                            table->s->primary_key != MAX_KEY;
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
                keyname, table->key_info[n_key].name) == 0)
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
                key->name.str) == 0)
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
            if (keyname == NULL)
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
          alter_info->flags&= ~(Alter_info::ALTER_ADD_INDEX |
              Alter_info::ADD_FOREIGN_KEY);
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
          alter_info->flags |= Alter_info::ALTER_DROP_INDEX;
          alter_info->drop_list.push_back(ad, thd->mem_root);
        }
      }
    }
  }
  
#ifdef WITH_PARTITION_STORAGE_ENGINE
  partition_info *tab_part_info= table->part_info;
  if (tab_part_info)
  {
    /* ALTER TABLE ADD PARTITION IF NOT EXISTS */
    if ((alter_info->flags & Alter_info::ALTER_ADD_PARTITION) &&
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
            alter_info->flags&= ~Alter_info::ALTER_ADD_PARTITION;
            thd->lex->part_info= NULL;
            break;
          }
        }
      }
    }
    /* ALTER TABLE DROP PARTITION IF EXISTS */
    if ((alter_info->flags & Alter_info::ALTER_DROP_PARTITION) &&
        thd->lex->if_exists())
    {
      List_iterator<char> names_it(alter_info->partition_names);
      char *name;

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
        alter_info->flags&= ~Alter_info::ALTER_DROP_PARTITION;
    }
  }
#endif /*WITH_PARTITION_STORAGE_ENGINE*/

  DBUG_VOID_RETURN;
}


/**
  Get Create_field object for newly created table by field index.

  @param alter_info  Alter_info describing newly created table.
  @param idx         Field index.
*/

static Create_field *get_field_by_index(Alter_info *alter_info, uint idx)
{
  List_iterator_fast<Create_field> field_it(alter_info->create_list);
  uint field_idx= 0;
  Create_field *field;

  while ((field= field_it++) && field_idx < idx)
  { field_idx++; }

  return field;
}


static int compare_uint(const uint *s, const uint *t)
{
  return (*s < *t) ? -1 : ((*s > *t) ? 1 : 0);
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

static bool fill_alter_inplace_info(THD *thd,
                                    TABLE *table,
                                    bool varchar,
                                    Alter_inplace_info *ha_alter_info)
{
  Field **f_ptr, *field;
  List_iterator_fast<Create_field> new_field_it;
  Create_field *new_field;
  KEY_PART_INFO *key_part, *new_part;
  KEY_PART_INFO *end;
  uint candidate_key_count= 0;
  Alter_info *alter_info= ha_alter_info->alter_info;
  DBUG_ENTER("fill_alter_inplace_info");

  /* Allocate result buffers. */
  if (! (ha_alter_info->index_drop_buffer=
          (KEY**) thd->alloc(sizeof(KEY*) * table->s->keys)) ||
      ! (ha_alter_info->index_add_buffer=
          (uint*) thd->alloc(sizeof(uint) *
                            alter_info->key_list.elements)))
    DBUG_RETURN(true);

  /* First we setup ha_alter_flags based on what was detected by parser. */
  if (alter_info->flags & Alter_info::ALTER_ADD_COLUMN)
    ha_alter_info->handler_flags|= Alter_inplace_info::ADD_COLUMN;
  if (alter_info->flags & Alter_info::ALTER_DROP_COLUMN)
    ha_alter_info->handler_flags|= Alter_inplace_info::DROP_COLUMN;
  /*
    Comparing new and old default values of column is cumbersome.
    So instead of using such a comparison for detecting if default
    has really changed we rely on flags set by parser to get an
    approximate value for storage engine flag.
  */
  if (alter_info->flags & (Alter_info::ALTER_CHANGE_COLUMN |
                           Alter_info::ALTER_CHANGE_COLUMN_DEFAULT))
    ha_alter_info->handler_flags|= Alter_inplace_info::ALTER_COLUMN_DEFAULT;
  if (alter_info->flags & Alter_info::ADD_FOREIGN_KEY)
    ha_alter_info->handler_flags|= Alter_inplace_info::ADD_FOREIGN_KEY;
  if (alter_info->flags & Alter_info::DROP_FOREIGN_KEY)
    ha_alter_info->handler_flags|= Alter_inplace_info::DROP_FOREIGN_KEY;
  if (alter_info->flags & Alter_info::ALTER_OPTIONS)
    ha_alter_info->handler_flags|= Alter_inplace_info::CHANGE_CREATE_OPTION;
  if (alter_info->flags & Alter_info::ALTER_RENAME)
    ha_alter_info->handler_flags|= Alter_inplace_info::ALTER_RENAME;
  /* Check partition changes */
  if (alter_info->flags & Alter_info::ALTER_ADD_PARTITION)
    ha_alter_info->handler_flags|= Alter_inplace_info::ADD_PARTITION;
  if (alter_info->flags & Alter_info::ALTER_DROP_PARTITION)
    ha_alter_info->handler_flags|= Alter_inplace_info::DROP_PARTITION;
  if (alter_info->flags & Alter_info::ALTER_PARTITION)
    ha_alter_info->handler_flags|= Alter_inplace_info::ALTER_PARTITION;
  if (alter_info->flags & Alter_info::ALTER_COALESCE_PARTITION)
    ha_alter_info->handler_flags|= Alter_inplace_info::COALESCE_PARTITION;
  if (alter_info->flags & Alter_info::ALTER_REORGANIZE_PARTITION)
    ha_alter_info->handler_flags|= Alter_inplace_info::REORGANIZE_PARTITION;
  if (alter_info->flags & Alter_info::ALTER_TABLE_REORG)
    ha_alter_info->handler_flags|= Alter_inplace_info::ALTER_TABLE_REORG;
  if (alter_info->flags & Alter_info::ALTER_REMOVE_PARTITIONING)
    ha_alter_info->handler_flags|= Alter_inplace_info::ALTER_REMOVE_PARTITIONING;
  if (alter_info->flags & Alter_info::ALTER_ALL_PARTITION)
    ha_alter_info->handler_flags|= Alter_inplace_info::ALTER_ALL_PARTITION;
  /* Check for: ALTER TABLE FORCE, ALTER TABLE ENGINE and OPTIMIZE TABLE. */
  if (alter_info->flags & Alter_info::ALTER_RECREATE)
    ha_alter_info->handler_flags|= Alter_inplace_info::RECREATE_TABLE;

  /*
    If we altering table with old VARCHAR fields we will be automatically
    upgrading VARCHAR column types.
  */
  if (table->s->frm_version < FRM_VER_TRUE_VARCHAR && varchar)
    ha_alter_info->handler_flags|=  Alter_inplace_info::ALTER_COLUMN_TYPE;

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
  for (f_ptr= table->field; (field= *f_ptr); f_ptr++)
  {
    /* Clear marker for renamed or dropped field
    which we are going to set later. */
    field->flags&= ~(FIELD_IS_RENAMED | FIELD_IS_DROPPED);

    /* Use transformed info to evaluate flags for storage engine. */
    uint new_field_index= 0;
    new_field_it.init(alter_info->create_list);
    while ((new_field= new_field_it++))
    {
      if (new_field->field == field)
        break;
      new_field_index++;
    }

    if (new_field)
    {
      /* Field is not dropped. Evaluate changes bitmap for it. */

      /*
        Check if type of column has changed to some incompatible type.
      */
      uint is_equal= field->is_equal(new_field);
      switch (is_equal)
      {
      case IS_EQUAL_NO:
        /* New column type is incompatible with old one. */
        ha_alter_info->handler_flags|= Alter_inplace_info::ALTER_COLUMN_TYPE;
        if (table->s->tmp_table == NO_TMP_TABLE)
        {
          delete_statistics_for_column(thd, table, field);
          KEY *key_info= table->key_info; 
          for (uint i=0; i < table->s->keys; i++, key_info++)
          {
            if (field->part_of_key.is_set(i))
            {
              uint key_parts= table->actual_n_key_parts(key_info);
              for (uint j= 0; j < key_parts; j++)
              {
                if (key_info->key_part[j].fieldnr-1 == field->field_index)
                {
                  delete_statistics_for_index(thd, table, key_info,
                                       j >= key_info->user_defined_key_parts);
                  break;
                }
              }           
            }
          }      
        }
        break;
      case IS_EQUAL_YES:
        /*
          New column is the same as the old one or the fully compatible with
          it (for example, ENUM('a','b') was changed to ENUM('a','b','c')).
          Such a change if any can ALWAYS be carried out by simply updating
          data-dictionary without even informing storage engine.
          No flag is set in this case.
        */
        break;
      case IS_EQUAL_PACK_LENGTH:
        /*
          New column type differs from the old one, but has compatible packed
          data representation. Depending on storage engine, such a change can
          be carried out by simply updating data dictionary without changing
          actual data (for example, VARCHAR(300) is changed to VARCHAR(400)).
        */
        ha_alter_info->handler_flags|= Alter_inplace_info::
                                         ALTER_COLUMN_EQUAL_PACK_LENGTH;
        break;
      default:
        DBUG_ASSERT(0);
        /* Safety. */
        ha_alter_info->handler_flags|= Alter_inplace_info::ALTER_COLUMN_TYPE;
      }

      /*
        Check if the column is computed and either
        is stored or is used in the partitioning expression.
      */
      if (field->vcol_info && 
          (field->stored_in_db() || field->vcol_info->is_in_partitioning_expr()))
      {
        if (is_equal == IS_EQUAL_NO ||
            !field->vcol_info->is_equal(new_field->vcol_info))
          ha_alter_info->handler_flags|= Alter_inplace_info::ALTER_COLUMN_VCOL;
        else
          maybe_alter_vcol= true;
      }

      /* Check if field was renamed */
      if (my_strcasecmp(system_charset_info, field->field_name,
                        new_field->field_name))
      {
        field->flags|= FIELD_IS_RENAMED;
        ha_alter_info->handler_flags|= Alter_inplace_info::ALTER_COLUMN_NAME;
        rename_column_in_stat_tables(thd, table, field,
                                     new_field->field_name);
      }

      /* Check that NULL behavior is same for old and new fields */
      if ((new_field->flags & NOT_NULL_FLAG) !=
          (uint) (field->flags & NOT_NULL_FLAG))
      {
        if (new_field->flags & NOT_NULL_FLAG)
          ha_alter_info->handler_flags|=
            Alter_inplace_info::ALTER_COLUMN_NOT_NULLABLE;
        else
          ha_alter_info->handler_flags|=
            Alter_inplace_info::ALTER_COLUMN_NULLABLE;
      }

      /*
        We do not detect changes to default values in this loop.
        See comment above for more details.
      */

      /*
        Detect changes in column order.
      */
      if (field->field_index != new_field_index)
        ha_alter_info->handler_flags|= Alter_inplace_info::ALTER_COLUMN_ORDER;

      /* Detect changes in storage type of column */
      if (new_field->field_storage_type() != field->field_storage_type())
        ha_alter_info->handler_flags|=
          Alter_inplace_info::ALTER_COLUMN_STORAGE_TYPE;

      /* Detect changes in column format of column */
      if (new_field->column_format() != field->column_format())
        ha_alter_info->handler_flags|=
          Alter_inplace_info::ALTER_COLUMN_COLUMN_FORMAT;

      if (engine_options_differ(field->option_struct, new_field->option_struct,
                                table->file->ht->field_options))
      {
        ha_alter_info->handler_flags|= Alter_inplace_info::ALTER_COLUMN_OPTION;
        ha_alter_info->create_info->fields_option_struct[f_ptr - table->field]=
          new_field->option_struct;
      }

    }
    else
    {
      /*
        Field is not present in new version of table and therefore was dropped.
        Corresponding storage engine flag should be already set.
      */
      DBUG_ASSERT(ha_alter_info->handler_flags & Alter_inplace_info::DROP_COLUMN);
      field->flags|= FIELD_IS_DROPPED;
    }
  }

  if (maybe_alter_vcol)
  {
    /*
      No virtual column was altered, but perhaps one of the other columns was,
      and that column was part of the vcol expression?
      We don't detect this correctly (FIXME), so let's just say that a vcol
      *might* be affected if any other column was altered.
    */
    if (ha_alter_info->handler_flags &
          ( Alter_inplace_info::ALTER_COLUMN_TYPE
          | Alter_inplace_info::ALTER_COLUMN_NOT_NULLABLE
          | Alter_inplace_info::ALTER_COLUMN_OPTION ))
      ha_alter_info->handler_flags|= Alter_inplace_info::ALTER_COLUMN_VCOL;
  }

  new_field_it.init(alter_info->create_list);
  while ((new_field= new_field_it++))
  {
    if (! new_field->field)
    {
      /*
        Field is not present in old version of table and therefore was added.
        Again corresponding storage engine flag should be already set.
      */
      DBUG_ASSERT(ha_alter_info->handler_flags & Alter_inplace_info::ADD_COLUMN);

      if (new_field->vcol_info && 
          (new_field->stored_in_db() || new_field->vcol_info->is_in_partitioning_expr()))
      {
        ha_alter_info->handler_flags|= Alter_inplace_info::ALTER_COLUMN_VCOL;
      }
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
      if (! strcmp(table_key->name, new_key->name))
        break;
    }
    if (new_key >= new_key_end)
    {
      /* Key not found. Add the key to the drop buffer. */
      ha_alter_info->index_drop_buffer
        [ha_alter_info->index_drop_count++]=
        table_key;
      DBUG_PRINT("info", ("index dropped: '%s'", table_key->name));
      continue;
    }

    /* Check that the key types are compatible between old and new tables. */
    if ((table_key->algorithm != new_key->algorithm) ||
        ((table_key->flags & HA_KEYFLAG_MASK) !=
         (new_key->flags & HA_KEYFLAG_MASK)) ||
        (table_key->user_defined_key_parts !=
         new_key->user_defined_key_parts))
      goto index_changed;

    if (engine_options_differ(table_key->option_struct, new_key->option_struct,
                              table->file->ht->index_options))
      goto index_changed;

    /*
      Check that the key parts remain compatible between the old and
      new tables.
    */
    end= table_key->key_part + table_key->user_defined_key_parts;
    for (key_part= table_key->key_part, new_part= new_key->key_part;
         key_part < end;
         key_part++, new_part++)
    {
      /*
        Key definition has changed if we are using a different field or
        if the used key part length is different. It makes sense to
        check lengths first as in case when fields differ it is likely
        that lengths differ too and checking fields is more expensive
        in general case.
      */
      if (key_part->length != new_part->length)
        goto index_changed;

      new_field= get_field_by_index(alter_info, new_part->fieldnr);

      /*
        For prefix keys KEY_PART_INFO::field points to cloned Field
        object with adjusted length. So below we have to check field
        indexes instead of simply comparing pointers to Field objects.
      */
      if (! new_field->field ||
          new_field->field->field_index != key_part->fieldnr - 1)
        goto index_changed;
    }

    /* Check that key comment is not changed. */
    if (table_key->comment.length != new_key->comment.length ||
        (table_key->comment.length &&
         memcmp(table_key->comment.str, new_key->comment.str,
                table_key->comment.length) != 0))
      goto index_changed;

    continue;

  index_changed:
    /* Key modified. Add the key / key offset to both buffers. */
    ha_alter_info->index_drop_buffer
      [ha_alter_info->index_drop_count++]=
      table_key;
    ha_alter_info->index_add_buffer
      [ha_alter_info->index_add_count++]=
      new_key - ha_alter_info->key_info_buffer;
    /* Mark all old fields which are used in newly created index. */
    DBUG_PRINT("info", ("index changed: '%s'", table_key->name));
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
      if (! strcmp(table_key->name, new_key->name))
        break;
    }
    if (table_key >= table_key_end)
    {
      /* Key not found. Add the offset of the key to the add buffer. */
      ha_alter_info->index_add_buffer
        [ha_alter_info->index_add_count++]=
        new_key - ha_alter_info->key_info_buffer;
      DBUG_PRINT("info", ("index added: '%s'", new_key->name));
    }
    else
      ha_alter_info->create_info->indexes_option_struct[table_key - table->key_info]=
        new_key->option_struct;
  }

  /*
    Sort index_add_buffer according to how key_info_buffer is sorted.
    I.e. with primary keys first - see sort_keys().
  */
  my_qsort(ha_alter_info->index_add_buffer,
           ha_alter_info->index_add_count,
           sizeof(uint), (qsort_cmp) compare_uint);

  /* Now let us calculate flags for storage engine API. */

  /* Count all existing candidate keys. */
  for (table_key= table->key_info; table_key < table_key_end; table_key++)
  {
    /*
      Check if key is a candidate key, This key is either already primary key
      or could be promoted to primary key if the original primary key is
      dropped.
      In MySQL one is allowed to create primary key with partial fields (i.e.
      primary key which is not considered candidate). For simplicity we count
      such key as a candidate key here.
    */
    if (((uint) (table_key - table->key_info) == table->s->primary_key) ||
        is_candidate_key(table_key))
      candidate_key_count++;
  }

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
      /*
        Unique key. Check for PRIMARY KEY. Also see comment about primary
        and candidate keys above.
      */
      if ((uint) (table_key - table->key_info) == table->s->primary_key)
      {
        ha_alter_info->handler_flags|= Alter_inplace_info::DROP_PK_INDEX;
        candidate_key_count--;
      }
      else
      {
        ha_alter_info->handler_flags|= Alter_inplace_info::DROP_UNIQUE_INDEX;
        if (is_candidate_key(table_key))
          candidate_key_count--;
      }
    }
    else
      ha_alter_info->handler_flags|= Alter_inplace_info::DROP_INDEX;
  }

  /* Now figure out what kind of indexes we are adding. */
  for (uint add_key_idx= 0; add_key_idx < ha_alter_info->index_add_count; add_key_idx++)
  {
    new_key= ha_alter_info->key_info_buffer + ha_alter_info->index_add_buffer[add_key_idx];

    if (new_key->flags & HA_NOSAME)
    {
      bool is_pk= !my_strcasecmp(system_charset_info, new_key->name, primary_key_name);

      if ((!(new_key->flags & HA_KEY_HAS_PART_KEY_SEG) &&
           !(new_key->flags & HA_NULL_PART_KEY)) ||
          is_pk)
      {
        /* Candidate key or primary key! */
        if (candidate_key_count == 0 || is_pk)
          ha_alter_info->handler_flags|= Alter_inplace_info::ADD_PK_INDEX;
        else
          ha_alter_info->handler_flags|= Alter_inplace_info::ADD_UNIQUE_INDEX;
        candidate_key_count++;
      }
      else
      {
        ha_alter_info->handler_flags|= Alter_inplace_info::ADD_UNIQUE_INDEX;
      }
    }
    else
      ha_alter_info->handler_flags|= Alter_inplace_info::ADD_INDEX;
  }

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
	(tmp_new_field->sql_type == MYSQL_TYPE_VARCHAR &&
	create_info->row_type != ROW_TYPE_FIXED))
      create_info->table_options|= HA_OPTION_PACK_RECORD;

    /* Check if field was renamed */
    if (my_strcasecmp(system_charset_info,
		      field->field_name,
		      tmp_new_field->field_name))
      DBUG_RETURN(false);

    /* Evaluate changes bitmap and send to check_if_incompatible_data() */
    uint field_changes= field->is_equal(tmp_new_field);
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
      if (! strcmp(table_key->name, new_key->name))
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
      if (! strcmp(table_key->name, new_key->name))
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
    /* fall-through: disabled indexes */
  case Alter_info::DISABLE:
    error= table->file->ha_disable_indexes(HA_KEY_SWITCH_NONUNIQ_SAVE);
  }

  if (error == HA_ERR_WRONG_COMMAND)
  {
    THD *thd= table->in_use;
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE,
                        ER_ILLEGAL_HA, ER_THD(thd, ER_ILLEGAL_HA),
                        table->file->table_type(),
                        table->s->db.str, table->s->table_name.str);
    error= 0;
  }
  else if (error)
    table->file->print_error(error, MYF(0));

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
  if (alter_info->flags & (Alter_info::ALTER_ORDER |
                           Alter_info::ALTER_KEYS_ONOFF))
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
  @param inplace_supported  Enum describing the locking requirements.
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
                                      enum_alter_inplace_result inplace_supported,
                                      MDL_request *target_mdl_request,
                                      Alter_table_ctx *alter_ctx)
{
  Open_table_context ot_ctx(thd, MYSQL_OPEN_REOPEN | MYSQL_OPEN_IGNORE_KILLED);
  handlerton *db_type= table->s->db_type();
  MDL_ticket *mdl_ticket= table->mdl_ticket;
  HA_CREATE_INFO *create_info= ha_alter_info->create_info;
  Alter_info *alter_info= ha_alter_info->alter_info;
  bool reopen_tables= false;

  DBUG_ENTER("mysql_inplace_alter_table");

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
      ((inplace_supported == HA_ALTER_INPLACE_SHARED_LOCK_AFTER_PREPARE ||
        inplace_supported == HA_ALTER_INPLACE_NO_LOCK_AFTER_PREPARE) &&
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
  else if (inplace_supported == HA_ALTER_INPLACE_SHARED_LOCK_AFTER_PREPARE ||
           inplace_supported == HA_ALTER_INPLACE_NO_LOCK_AFTER_PREPARE)
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
  {
    goto cleanup;
  }

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
  case HA_ALTER_INPLACE_NO_LOCK_AFTER_PREPARE:
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
  case HA_ALTER_INPLACE_SHARED_LOCK_AFTER_PREPARE:
  case HA_ALTER_INPLACE_SHARED_LOCK:
    break;
  }

  if (table->file->ha_prepare_inplace_alter_table(altered_table,
                                                  ha_alter_info))
  {
    goto rollback;
  }

  /*
    Downgrade the lock if storage engine has told us that exclusive lock was
    necessary only for prepare phase (unless we are not under LOCK TABLES) and
    user has not explicitly requested exclusive lock.
  */
  if ((inplace_supported == HA_ALTER_INPLACE_SHARED_LOCK_AFTER_PREPARE ||
       inplace_supported == HA_ALTER_INPLACE_NO_LOCK_AFTER_PREPARE) &&
      !(thd->locked_tables_mode == LTM_LOCK_TABLES ||
        thd->locked_tables_mode == LTM_PRELOCKED_UNDER_LOCK_TABLES) &&
      (alter_info->requested_lock != Alter_info::ALTER_TABLE_LOCK_EXCLUSIVE))
  {
    /* If storage engine or user requested shared lock downgrade to SNW. */
    if (inplace_supported == HA_ALTER_INPLACE_SHARED_LOCK_AFTER_PREPARE ||
        alter_info->requested_lock == Alter_info::ALTER_TABLE_LOCK_SHARED)
      table->mdl_ticket->downgrade_lock(MDL_SHARED_NO_WRITE);
    else
    {
      DBUG_ASSERT(inplace_supported == HA_ALTER_INPLACE_NO_LOCK_AFTER_PREPARE);
      table->mdl_ticket->downgrade_lock(MDL_SHARED_UPGRADABLE);
    }
  }

  DEBUG_SYNC(thd, "alter_table_inplace_after_lock_downgrade");
  THD_STAGE_INFO(thd, stage_alter_inplace);

  if (table->file->ha_inplace_alter_table(altered_table,
                                          ha_alter_info))
  {
    goto rollback;
  }

  // Upgrade to EXCLUSIVE before commit.
  if (wait_while_table_is_used(thd, table, HA_EXTRA_PREPARE_FOR_RENAME))
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

  if (table->file->ha_commit_inplace_alter_table(altered_table,
                                                 ha_alter_info,
                                                 true))
  {
    goto rollback;
  }

  close_all_tables_for_name(thd, table->s,
                            alter_ctx->is_table_renamed() ?
                            HA_EXTRA_PREPARE_FOR_RENAME :
                            HA_EXTRA_NOT_USED,
                            NULL);
  table_list->table= table= NULL;
  close_temporary_table(thd, altered_table, true, false);

  /*
    Replace the old .FRM with the new .FRM, but keep the old name for now.
    Rename to the new name (if needed) will be handled separately below.
  */
  if (mysql_rename_table(db_type, alter_ctx->new_db, alter_ctx->tmp_name,
                         alter_ctx->db, alter_ctx->alias,
                         FN_FROM_IS_TMP | NO_HA_TABLE))
  {
    // Since changes were done in-place, we can't revert them.
    (void) quick_rm_table(thd, db_type,
                          alter_ctx->new_db, alter_ctx->tmp_name,
                          FN_IS_TMP | NO_HA_TABLE);
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
                     alter_ctx->db, alter_ctx->table_name, false);

    if (mysql_rename_table(db_type, alter_ctx->db, alter_ctx->table_name,
                           alter_ctx->new_db, alter_ctx->new_alias, 0))
    {
      /*
        If the rename fails we will still have a working table
        with the old name, but with other changes applied.
      */
      DBUG_RETURN(true);
    }
    if (Table_triggers_list::change_table_name(thd,
                                               alter_ctx->db,
                                               alter_ctx->alias,
                                               alter_ctx->table_name,
                                               alter_ctx->new_db,
                                               alter_ctx->new_alias))
    {
      /*
        If the rename of trigger files fails, try to rename the table
        back so we at least have matching table and trigger files.
      */
      (void) mysql_rename_table(db_type,
                                alter_ctx->new_db, alter_ctx->new_alias,
                                alter_ctx->db, alter_ctx->alias, NO_FK_CHECKS);
      DBUG_RETURN(true);
    }
    rename_table_in_stat_tables(thd, alter_ctx->db,alter_ctx->alias,
                                alter_ctx->new_db, alter_ctx->new_alias);
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
    if (thd->locked_tables_list.reopen_tables(thd))
      thd->locked_tables_list.unlink_all_closed_tables(thd, NULL, 0);
    /* QQ; do something about metadata locks ? */
  }
  close_temporary_table(thd, altered_table, true, false);
  // Delete temporary .frm/.par
  (void) quick_rm_table(thd, create_info->db_type, alter_ctx->new_db,
                        alter_ctx->tmp_name, FN_IS_TMP | NO_HA_TABLE);
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
    return 4294967295U;
  default:
    DBUG_ASSERT(0); // we should never go here
    return 0;
  }
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
  uint db_create_options= (table->s->db_create_options
                           & ~(HA_OPTION_PACK_RECORD));
  uint used_fields;
  KEY *key_info=table->key_info;
  bool rc= TRUE;
  bool modified_primary_key= FALSE;
  Create_field *def;
  Field **f_ptr,*field;
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
  for (f_ptr=table->field ; (field= *f_ptr) ; f_ptr++)
  {
    Alter_drop *drop;
    if (field->type() == MYSQL_TYPE_VARCHAR)
      create_info->varchar= TRUE;
    /* Check if field should be dropped */
    drop_it.rewind();
    while ((drop=drop_it++))
    {
      if (drop->type == Alter_drop::COLUMN &&
	  !my_strcasecmp(system_charset_info,field->field_name, drop->name))
      {
	/* Reset auto_increment value if it was dropped */
	if (MTYP_TYPENR(field->unireg_check) == Field::NEXT_NUMBER &&
	    !(used_fields & HA_CREATE_USED_AUTO))
	{
	  create_info->auto_increment_value=0;
	  create_info->used_fields|=HA_CREATE_USED_AUTO;
	}
	break;
      }
    }
    if (drop)
    {
      if (table->s->tmp_table == NO_TMP_TABLE)
        (void) delete_statistics_for_column(thd, table, field);
      drop_it.remove();
      continue;
    }
    /* Check if field is changed */
    def_it.rewind();
    while ((def=def_it++))
    {
      if (def->change &&
	  !my_strcasecmp(system_charset_info,field->field_name, def->change))
	break;
    }
    if (def)
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
        my_error(ER_UNSUPPORTED_ACTION_ON_VIRTUAL_COLUMN, MYF(0));
        goto err;
      }
      if (!def->after)
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
	if (!my_strcasecmp(system_charset_info,field->field_name, alter->name))
	  break;
      }
      if (alter)
      {
	if (def->sql_type == MYSQL_TYPE_BLOB)
	{
	  my_error(ER_BLOB_CANT_HAVE_DEFAULT, MYF(0), def->change);
          goto err;
	}
	if ((def->def=alter->def))              // Use new default
          def->flags&= ~NO_DEFAULT_VALUE_FLAG;
        else
          def->flags|= NO_DEFAULT_VALUE_FLAG;
	alter_it.remove();
      }
    }
  }
  def_it.rewind();
  while ((def=def_it++))			// Add new columns
  {
    if (def->change && ! def->field)
    {
      my_error(ER_BAD_FIELD_ERROR, MYF(0), def->change,
               table->s->table_name.str);
      goto err;
    }
    /*
      Check that the DATE/DATETIME not null field we are going to add is
      either has a default value or the '0000-00-00' is allowed by the
      set sql mode.
      If the '0000-00-00' value isn't allowed then raise the error_if_not_empty
      flag to allow ALTER TABLE only if the table to be altered is empty.
    */
    if ((def->sql_type == MYSQL_TYPE_DATE ||
         def->sql_type == MYSQL_TYPE_NEWDATE ||
         def->sql_type == MYSQL_TYPE_DATETIME ||
         def->sql_type == MYSQL_TYPE_DATETIME2) &&
         !alter_ctx->datetime_field &&
         !(~def->flags & (NO_DEFAULT_VALUE_FLAG | NOT_NULL_FLAG)) &&
         thd->variables.sql_mode & MODE_NO_ZERO_DATE)
    {
        alter_ctx->datetime_field= def;
        alter_ctx->error_if_not_empty= TRUE;
    }
    if (!def->after)
      new_create_list.push_back(def, thd->mem_root);
    else
    {
      Create_field *find;
      if (def->change)
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
      if (def->after == first_keyword)
        new_create_list.push_front(def, thd->mem_root);
      else
      {
        find_it.rewind();
        while ((find=find_it++))
        {
          if (!my_strcasecmp(system_charset_info, def->after, find->field_name))
            break;
        }
        if (!find)
        {
          my_error(ER_BAD_FIELD_ERROR, MYF(0), def->after, table->s->table_name.str);
          goto err;
        }
        find_it.after(def);			// Put column after this
      }
    }
  }
  if (alter_info->alter_list.elements)
  {
    my_error(ER_BAD_FIELD_ERROR, MYF(0),
             alter_info->alter_list.head()->name, table->s->table_name.str);
    goto err;
  }
  if (!new_create_list.elements)
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
    char *key_name= key_info->name;
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

    KEY_PART_INFO *key_part= key_info->key_part;
    key_parts.empty();
    bool delete_index_stat= FALSE;
    for (uint j=0 ; j < key_info->user_defined_key_parts ; j++,key_part++)
    {
      if (!key_part->field)
	continue;				// Wrong field (from UNIREG)
      const char *key_part_name=key_part->field->field_name;
      Create_field *cfield;
      uint key_part_length;

      field_it.rewind();
      while ((cfield=field_it++))
      {
	if (cfield->change)
	{
	  if (!my_strcasecmp(system_charset_info, key_part_name,
			     cfield->change))
	    break;
	}
	else if (!my_strcasecmp(system_charset_info,
				key_part_name, cfield->field_name))
	  break;
      }
      if (!cfield)
      {
        if (table->s->primary_key == i)
          modified_primary_key= TRUE;
        delete_index_stat= TRUE;
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
        if (!Field::type_can_have_key_part(cfield->field->type()) ||
            !Field::type_can_have_key_part(cfield->sql_type) ||
            /* spatial keys can't have sub-key length */
            (key_info->flags & HA_SPATIAL) ||
            (cfield->field->field_length == key_part_length &&
             !f_is_blob(key_part->key_type)) ||
            (cfield->length && (((cfield->sql_type >= MYSQL_TYPE_TINY_BLOB &&
                                  cfield->sql_type <= MYSQL_TYPE_BLOB) ? 
                                blob_length_by_type(cfield->sql_type) :
                                cfield->length) <
	     key_part_length / key_part->field->charset()->mbmaxlen)))
	  key_part_length= 0;			// Use whole field
      }
      key_part_length /= key_part->field->charset()->mbmaxlen;
      key_parts.push_back(new Key_part_spec(cfield->field_name,
                                            strlen(cfield->field_name),
					    key_part_length),
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
      bzero((char*) &key_create_info, sizeof(key_create_info));

      key_create_info.algorithm= key_info->algorithm;
      if (key_info->flags & HA_USES_BLOCK_SIZE)
        key_create_info.block_size= key_info->block_size;
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
      }
      else if (key_info->flags & HA_FULLTEXT)
        key_type= Key::FULLTEXT;
      else
        key_type= Key::MULTIPLE;

      key= new Key(key_type, key_name, strlen(key_name),
                   &key_create_info,
                   MY_TEST(key_info->flags & HA_GENERATED_KEY),
                   key_parts, key_info->option_list, DDL_options());
      new_key_list.push_back(key, thd->mem_root);
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

  if (alter_info->drop_list.elements)
  {
    Alter_drop *drop;
    drop_it.rewind();
    while ((drop=drop_it++)) {
      switch (drop->type) {
      case Alter_drop::KEY:
      case Alter_drop::COLUMN:
        my_error(ER_CANT_DROP_FIELD_OR_KEY, MYF(0),
                 alter_info->drop_list.head()->name);
        goto err;
      case Alter_drop::FOREIGN_KEY:
        // Leave the DROP FOREIGN KEY names in the alter_info->drop_list.
        break;
      }
    }
  }
  if (alter_info->alter_list.elements)
  {
    my_error(ER_CANT_DROP_FIELD_OR_KEY, MYF(0),
             alter_info->alter_list.head()->name);
    goto err;
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
                       new_field->field->field_name,
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
                        List<LEX_STRING> &fk_columns,
                        const char **bad_column_name)
{
  List_iterator_fast<LEX_STRING> column_it(fk_columns);
  LEX_STRING *column;

  *bad_column_name= NULL;

  while ((column= column_it++))
  {
    Create_field *new_field= get_field_by_old_name(alter_info, column->str);

    if (new_field)
    {
      Field *old_field= new_field->field;

      if (my_strcasecmp(system_charset_info, old_field->field_name,
                        new_field->field_name))
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

      if ((old_field->is_equal(new_field) == IS_EQUAL_NO) ||
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
  if (thd->is_error())
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
          (my_strcasecmp(table_alias_charset, f_key->foreign_db->str,
                         table->s->db.str) == 0) &&
          (my_strcasecmp(table_alias_charset, f_key->foreign_table->str,
                         table->s->table_name.str) == 0))
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
      char buff[NAME_LEN*2+2];
      strxnmov(buff, sizeof(buff)-1, f_key->foreign_db->str, ".",
               f_key->foreign_table->str, NullS);
      my_error(ER_FK_COLUMN_CANNOT_DROP_CHILD, MYF(0), bad_column_name,
               f_key->foreign_id->str, buff);
      DBUG_RETURN(true);
    }
    default:
      DBUG_ASSERT(0);
    }
  }

  table->file->get_foreign_key_list(thd, &fk_child_key_list);

  /* OOM when building list. */
  if (thd->is_error())
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

  DBUG_RETURN(false);
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

    error= alter_table_manage_keys(table,
                                   table->file->indexes_are_disabled(),
                                   keys_onoff);
  }

  if (!error && alter_ctx->is_table_renamed())
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
    close_all_tables_for_name(thd, table->s, HA_EXTRA_PREPARE_FOR_RENAME, NULL);

    LEX_STRING old_db_name= { alter_ctx->db, strlen(alter_ctx->db) };
    LEX_STRING old_table_name=
               { alter_ctx->table_name, strlen(alter_ctx->table_name) };
    LEX_STRING new_db_name= { alter_ctx->new_db, strlen(alter_ctx->new_db) };
    LEX_STRING new_table_name=
               { alter_ctx->new_alias, strlen(alter_ctx->new_alias) };
    (void) rename_table_in_stat_tables(thd, &old_db_name, &old_table_name,
                                       &new_db_name, &new_table_name);

    if (mysql_rename_table(old_db_type, alter_ctx->db, alter_ctx->table_name,
                           alter_ctx->new_db, alter_ctx->new_alias, 0))
      error= -1;
    else if (Table_triggers_list::change_table_name(thd,
                                                    alter_ctx->db,
                                                    alter_ctx->alias,
                                                    alter_ctx->table_name,
                                                    alter_ctx->new_db,
                                                    alter_ctx->new_alias))
    {
      (void) mysql_rename_table(old_db_type,
                                alter_ctx->new_db, alter_ctx->new_alias,
                                alter_ctx->db, alter_ctx->table_name,
                                NO_FK_CHECKS);
      error= -1;
    }
  }

  if (!error)
  {
    error= write_bin_log(thd, TRUE, thd->query(), thd->query_length());

    if (!error)
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

bool mysql_alter_table(THD *thd,char *new_db, char *new_name,
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
      my_error(ER_UNSUPORTED_LOG_ENGINE, MYF(0),
               hton_name(create_info->db_type)->str);
      DBUG_RETURN(true);
    }

#ifdef WITH_PARTITION_STORAGE_ENGINE
    if (alter_info->flags & Alter_info::ALTER_PARTITION)
    {
      my_error(ER_WRONG_USAGE, MYF(0), "PARTITION", "log table");
      DBUG_RETURN(true);
    }
#endif
  }

  THD_STAGE_INFO(thd, stage_init);

  /*
    Code below can handle only base tables so ensure that we won't open a view.
    Note that RENAME TABLE the only ALTER clause which is supported for views
    has been already processed.
  */
  table_list->required_type= FRMTYPE_TABLE;

  Alter_table_prelocking_strategy alter_prelocking_strategy;

  DEBUG_SYNC(thd, "alter_table_before_open_tables");
  uint tables_opened;

  thd->open_options|= HA_OPEN_FOR_ALTER;
  bool error= open_tables(thd, &table_list, &tables_opened, 0,
                          &alter_prelocking_strategy);
  thd->open_options&= ~HA_OPEN_FOR_ALTER;

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

  if (error)
    DBUG_RETURN(true);

  TABLE *table= table_list->table;
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
      if (find_temporary_table(thd, alter_ctx.new_db, alter_ctx.new_name))
      {
        my_error(ER_TABLE_EXISTS_ERROR, MYF(0), alter_ctx.new_alias);
        DBUG_RETURN(true);
      }
    }
    else
    {
      MDL_request_list mdl_requests;
      MDL_request target_db_mdl_request;

      target_mdl_request.init(MDL_key::TABLE,
                              alter_ctx.new_db, alter_ctx.new_name,
                              MDL_EXCLUSIVE, MDL_TRANSACTION);
      mdl_requests.push_front(&target_mdl_request);

      /*
        If we are moving the table to a different database, we also
        need IX lock on the database name so that the target database
        is protected by MDL while the table is moved.
      */
      if (alter_ctx.is_database_changed())
      {
        target_db_mdl_request.init(MDL_key::SCHEMA, alter_ctx.new_db, "",
                                   MDL_INTENTION_EXCLUSIVE,
                                   MDL_TRANSACTION);
        mdl_requests.push_front(&target_db_mdl_request);
      }

      /*
        Global intention exclusive lock must have been already acquired when
        table to be altered was open, so there is no need to do it here.
      */
      DBUG_ASSERT(thd->mdl_context.is_lock_owner(MDL_key::GLOBAL,
                                                 "", "",
                                                 MDL_INTENTION_EXCLUSIVE));

      if (thd->mdl_context.acquire_locks(&mdl_requests,
                                         thd->variables.lock_wait_timeout))
        DBUG_RETURN(true);

      DEBUG_SYNC(thd, "locked_table_name");
      /*
        Table maybe does not exist, but we got an exclusive lock
        on the name, now we can safely try to find out for sure.
      */
      if (ha_table_exists(thd, alter_ctx.new_db, alter_ctx.new_name, 0))
      {
        /* Table will be closed in do_command() */
        my_error(ER_TABLE_EXISTS_ERROR, MYF(0), alter_ctx.new_alias);
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

  if (check_engine(thd, alter_ctx.new_db, alter_ctx.new_name, create_info))
    DBUG_RETURN(true);

  if ((create_info->db_type != table->s->db_type() ||
       alter_info->flags & Alter_info::ALTER_PARTITION) &&
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
  if ((alter_info->flags & Alter_info::ADD_FOREIGN_KEY) &&
      check_fk_parent_table_access(thd, create_info, alter_info))
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
             alter_ctx.db, alter_ctx.table_name);
    DBUG_RETURN(true);
  }

  if (ha_check_storage_engine_flag(create_info->db_type,
                                   HTON_ALTER_NOT_SUPPORTED))
  {
    DBUG_PRINT("info", ("doesn't support alter"));
    my_error(ER_ILLEGAL_HA, MYF(0), hton_name(create_info->db_type)->str,
             alter_ctx.new_db, alter_ctx.new_name);
    DBUG_RETURN(true);
  }

  if (table->s->tmp_table == NO_TMP_TABLE)
    mysql_audit_alter_table(thd, table_list);

  THD_STAGE_INFO(thd, stage_setup);

  handle_if_exists_options(thd, table, alter_info);

  /*
    Look if we have to do anything at all.
    ALTER can become NOOP after handling
    the IF (NOT) EXISTS options.
  */
  if (alter_info->flags == 0)
  {
    my_snprintf(alter_ctx.tmp_name, sizeof(alter_ctx.tmp_name),
                ER_THD(thd, ER_INSERT_INFO), 0L, 0L,
                thd->get_stmt_da()->current_statement_warn_count());
    my_ok(thd, 0L, 0L, alter_ctx.tmp_name);

    if (write_bin_log(thd, true, thd->query(), thd->query_length()))
      DBUG_RETURN(true);

    DBUG_RETURN(false);
  }

  if (!(alter_info->flags & ~(Alter_info::ALTER_RENAME |
                              Alter_info::ALTER_KEYS_ONOFF)) &&
      alter_info->requested_algorithm !=
      Alter_info::ALTER_TABLE_ALGORITHM_COPY &&
      !table->s->tmp_table) // no need to touch frm
  {
    // This requires X-lock, no other lock levels supported.
    if (alter_info->requested_lock != Alter_info::ALTER_TABLE_LOCK_DEFAULT &&
        alter_info->requested_lock != Alter_info::ALTER_TABLE_LOCK_EXCLUSIVE)
    {
      my_error(ER_ALTER_OPERATION_NOT_SUPPORTED, MYF(0),
               "LOCK=NONE/SHARED", "LOCK=EXCLUSIVE");
      DBUG_RETURN(true);
    }
    bool res= simple_rename_or_index_change(thd, table_list,
                                            alter_info->keys_onoff,
                                            &alter_ctx);
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

  if (!opt_explicit_defaults_for_timestamp)
    promote_first_timestamp_column(&alter_info->create_list);

#ifdef WITH_PARTITION_STORAGE_ENGINE
  if (fast_alter_partition)
  {
    /*
      ALGORITHM and LOCK clauses are generally not allowed by the
      parser for operations related to partitioning.
      The exceptions are ALTER_PARTITION and ALTER_REMOVE_PARTITIONING.
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
    else if (alter_info->requested_algorithm !=
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
    if (thd->mdl_context.upgrade_shared_lock(mdl_ticket, MDL_SHARED_NO_WRITE,
                                             thd->variables.lock_wait_timeout)
        || lock_tables(thd, table_list, alter_ctx.tables_opened, 0))
    {
      DBUG_RETURN(true);
    }

    // In-place execution of ALTER TABLE for partitioning.
    DBUG_RETURN(fast_alter_partition_table(thd, table, alter_info,
                                           create_info, table_list,
                                           alter_ctx.db,
                                           alter_ctx.table_name));
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
  if ((thd->variables.old_alter_table &&
       alter_info->requested_algorithm !=
       Alter_info::ALTER_TABLE_ALGORITHM_INPLACE)
      || is_inplace_alter_impossible(table, create_info, alter_info)
#ifdef WITH_PARTITION_STORAGE_ENGINE
      || (partition_changed &&
          !(table->s->db_type()->partition_flags() & HA_USE_AUTO_PARTITION))
#endif
     )
  {
    if (alter_info->requested_algorithm ==
        Alter_info::ALTER_TABLE_ALGORITHM_INPLACE)
    {
      my_error(ER_ALTER_OPERATION_NOT_SUPPORTED, MYF(0),
               "ALGORITHM=INPLACE", "ALGORITHM=COPY");
      DBUG_RETURN(true);
    }
    alter_info->requested_algorithm= Alter_info::ALTER_TABLE_ALGORITHM_COPY;
  }

  /*
    ALTER TABLE ... ENGINE to the same engine is a common way to
    request table rebuild. Set ALTER_RECREATE flag to force table
    rebuild.
  */
  if (create_info->db_type == table->s->db_type() &&
      create_info->used_fields & HA_CREATE_USED_ENGINE)
    alter_info->flags|= Alter_info::ALTER_RECREATE;

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
      strmov(index_file, alter_ctx.tmp_name);
      create_info->index_file_name=fn_same(index_file,
                                           create_info->index_file_name,
                                           1);
    }
    if (create_info->data_file_name)
    {
      /* Fix data_file_name to have 'tmp_name' as basename */
      strmov(data_file, alter_ctx.tmp_name);
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
  /* We can abort alter table for any table type */
  thd->abort_on_warning= !ignore && thd->is_strict_mode();

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
  error= create_table_impl(thd,
                           alter_ctx.db, alter_ctx.table_name,
                           alter_ctx.new_db, alter_ctx.tmp_name,
                           alter_ctx.get_tmp_path(),
                           thd->lex->create_info, create_info, alter_info,
                           C_ALTER_TABLE_FRM_ONLY, NULL,
                           &key_info, &key_count, &frm);
  reenable_binlog(thd);
  thd->abort_on_warning= false;
  if (error)
  {
    my_free(const_cast<uchar*>(frm.str));
    DBUG_RETURN(true);
  }

  /* Remember that we have not created table in storage engine yet. */
  bool no_ha_table= true;

  if (alter_info->requested_algorithm != Alter_info::ALTER_TABLE_ALGORITHM_COPY)
  {
    Alter_inplace_info ha_alter_info(create_info, alter_info,
                                     key_info, key_count,
                                     IF_PARTITIONING(thd->work_part_info, NULL),
                                     ignore);
    TABLE *altered_table= NULL;
    bool use_inplace= true;

    /* Fill the Alter_inplace_info structure. */
    if (fill_alter_inplace_info(thd, table, varchar, &ha_alter_info))
      goto err_new_table_cleanup;

    if (ha_alter_info.handler_flags == 0)
    {
      /*
        No-op ALTER, no need to call handler API functions.

        If this code path is entered for an ALTER statement that
        should not be a real no-op, new handler flags should be added
        and fill_alter_inplace_info() adjusted.

        Note that we can end up here if an ALTER statement has clauses
        that cancel each other out (e.g. ADD/DROP identically index).

        Also note that we ignore the LOCK clause here.

         TODO don't create the frm in the first place
      */
      deletefrm(alter_ctx.get_tmp_path());
      my_free(const_cast<uchar*>(frm.str));
      goto end_inplace;
    }

    // We assume that the table is non-temporary.
    DBUG_ASSERT(!table->s->tmp_table);

    if (!(altered_table= open_table_uncached(thd, new_db_type, &frm,
                                             alter_ctx.get_tmp_path(),
                                             alter_ctx.new_db,
                                             alter_ctx.tmp_name,
                                             true, false)))
      goto err_new_table_cleanup;

    /* Set markers for fields in TABLE object for altered table. */
    update_altered_table(ha_alter_info, altered_table);

    /*
      Mark all columns in 'altered_table' as used to allow usage
      of its record[0] buffer and Field objects during in-place
      ALTER TABLE.
    */
    altered_table->column_bitmaps_set_no_signal(&altered_table->s->all_set,
                                                &altered_table->s->all_set);
    restore_record(altered_table, s->default_values); // Create empty record
    if (altered_table->default_field && altered_table->update_default_fields())
      goto err_new_table_cleanup;

    // Ask storage engine whether to use copy or in-place
    enum_alter_inplace_result inplace_supported=
      table->file->check_if_supported_inplace_alter(altered_table,
                                                    &ha_alter_info);

    switch (inplace_supported) {
    case HA_ALTER_INPLACE_EXCLUSIVE_LOCK:
      // If SHARED lock and no particular algorithm was requested, use COPY.
      if (alter_info->requested_lock ==
          Alter_info::ALTER_TABLE_LOCK_SHARED &&
          alter_info->requested_algorithm ==
          Alter_info::ALTER_TABLE_ALGORITHM_DEFAULT)
      {
        use_inplace= false;
      }
      // Otherwise, if weaker lock was requested, report errror.
      else if (alter_info->requested_lock ==
               Alter_info::ALTER_TABLE_LOCK_NONE ||
               alter_info->requested_lock ==
               Alter_info::ALTER_TABLE_LOCK_SHARED)
      {
        ha_alter_info.report_unsupported_error("LOCK=NONE/SHARED",
                                               "LOCK=EXCLUSIVE");
        close_temporary_table(thd, altered_table, true, false);
        goto err_new_table_cleanup;
      }
      break;
    case HA_ALTER_INPLACE_SHARED_LOCK_AFTER_PREPARE:
    case HA_ALTER_INPLACE_SHARED_LOCK:
      // If weaker lock was requested, report errror.
      if (alter_info->requested_lock ==
          Alter_info::ALTER_TABLE_LOCK_NONE)
      {
        ha_alter_info.report_unsupported_error("LOCK=NONE", "LOCK=SHARED");
        close_temporary_table(thd, altered_table, true, false);
        goto err_new_table_cleanup;
      }
      break;
    case HA_ALTER_INPLACE_NO_LOCK_AFTER_PREPARE:
    case HA_ALTER_INPLACE_NO_LOCK:
      break;
    case HA_ALTER_INPLACE_NOT_SUPPORTED:
      // If INPLACE was requested, report error.
      if (alter_info->requested_algorithm ==
          Alter_info::ALTER_TABLE_ALGORITHM_INPLACE)
      {
        ha_alter_info.report_unsupported_error("ALGORITHM=INPLACE",
                                               "ALGORITHM=COPY");
        close_temporary_table(thd, altered_table, true, false);
        goto err_new_table_cleanup;
      }
      // COPY with LOCK=NONE is not supported, no point in trying.
      if (alter_info->requested_lock ==
          Alter_info::ALTER_TABLE_LOCK_NONE)
      {
        ha_alter_info.report_unsupported_error("LOCK=NONE", "LOCK=SHARED");
        close_temporary_table(thd, altered_table, true, false);
        goto err_new_table_cleanup;
      }
      // Otherwise use COPY
      use_inplace= false;
      break;
    case HA_ALTER_ERROR:
    default:
      close_temporary_table(thd, altered_table, true, false);
      goto err_new_table_cleanup;
    }

    if (use_inplace)
    {
      table->s->frm_image= &frm;
      int res= mysql_inplace_alter_table(thd, table_list, table, altered_table,
                                         &ha_alter_info, inplace_supported,
                                         &target_mdl_request, &alter_ctx);
      my_free(const_cast<uchar*>(frm.str));

      if (res)
        DBUG_RETURN(true);

      goto end_inplace;
    }
    else
    {
      close_temporary_table(thd, altered_table, true, false);
    }
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

  // It's now safe to take the table level lock.
  if (lock_tables(thd, table_list, alter_ctx.tables_opened, 0))
    goto err_new_table_cleanup;

  if (ha_create_table(thd, alter_ctx.get_tmp_path(),
                      alter_ctx.new_db, alter_ctx.tmp_name,
                      create_info, &frm))
    goto err_new_table_cleanup;

  /* Mark that we have created table in storage engine. */
  no_ha_table= false;

  if (create_info->tmp_table())
  {
    if (!open_table_uncached(thd, new_db_type, &frm,
                             alter_ctx.get_tmp_path(),
                             alter_ctx.new_db, alter_ctx.tmp_name,
                             true, true))
      goto err_new_table_cleanup;
  }

  /* Open the table since we need to copy the data. */
  if (table->s->tmp_table != NO_TMP_TABLE)
  {
    TABLE_LIST tbl;
    tbl.init_one_table(alter_ctx.new_db, strlen(alter_ctx.new_db),
                       alter_ctx.tmp_name, strlen(alter_ctx.tmp_name),
                       alter_ctx.tmp_name, TL_READ_NO_INSERT);
    /* Table is in thd->temporary_tables */
    (void) open_temporary_table(thd, &tbl);
    new_table= tbl.table;
  }
  else
  {
    /* table is a normal table: Create temporary table in same directory */
    /* Open our intermediate table. */
    new_table= open_table_uncached(thd, new_db_type, &frm,
                                   alter_ctx.get_tmp_path(),
                                   alter_ctx.new_db, alter_ctx.tmp_name,
                                   true, true);
  }
  if (!new_table)
    goto err_new_table_cleanup;
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
      goto err_new_table_cleanup;
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
    close_temporary_table(thd, table, true, true);
    /* Should pass the 'new_name' as we store table name in the cache */
    if (rename_temporary_table(thd, new_table,
                               alter_ctx.new_db, alter_ctx.new_name))
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
    not delete it! Even altough MERGE tables do not have their children
    attached here it is safe to call close_temporary_table().
  */
  close_temporary_table(thd, new_table, true, false);
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
  char backup_name[32];
  my_snprintf(backup_name, sizeof(backup_name), "%s2-%lx-%lx", tmp_file_prefix,
              current_pid, thd->thread_id);
  if (lower_case_table_names)
    my_casedn_str(files_charset_info, backup_name);
  if (mysql_rename_table(old_db_type, alter_ctx.db, alter_ctx.table_name,
                         alter_ctx.db, backup_name, FN_TO_IS_TMP))
  {
    // Rename to temporary name failed, delete the new table, abort ALTER.
    (void) quick_rm_table(thd, new_db_type, alter_ctx.new_db,
                          alter_ctx.tmp_name, FN_IS_TMP);
    goto err_with_mdl;
  }

  // Rename the new table to the correct name.
  if (mysql_rename_table(new_db_type, alter_ctx.new_db, alter_ctx.tmp_name,
                         alter_ctx.new_db, alter_ctx.new_alias,
                         FN_FROM_IS_TMP))
  {
    // Rename failed, delete the temporary table.
    (void) quick_rm_table(thd, new_db_type, alter_ctx.new_db,
                          alter_ctx.tmp_name, FN_IS_TMP);

    // Restore the backup of the original table to the old name.
    (void) mysql_rename_table(old_db_type, alter_ctx.db, backup_name,
                              alter_ctx.db, alter_ctx.alias,
                              FN_FROM_IS_TMP | NO_FK_CHECKS);
    goto err_with_mdl;
  }

  // Check if we renamed the table and if so update trigger files.
  if (alter_ctx.is_table_renamed())
  {
    if (Table_triggers_list::change_table_name(thd,
                                               alter_ctx.db,
                                               alter_ctx.alias,
                                               alter_ctx.table_name,
                                               alter_ctx.new_db,
                                               alter_ctx.new_alias))
    {
      // Rename succeeded, delete the new table.
      (void) quick_rm_table(thd, new_db_type,
                            alter_ctx.new_db, alter_ctx.new_alias, 0);
      // Restore the backup of the original table to the old name.
      (void) mysql_rename_table(old_db_type, alter_ctx.db, backup_name,
                                alter_ctx.db, alter_ctx.alias,
                                FN_FROM_IS_TMP | NO_FK_CHECKS);
      goto err_with_mdl;
    }
    rename_table_in_stat_tables(thd, alter_ctx.db,alter_ctx.alias,
                                alter_ctx.new_db, alter_ctx.new_alias);
  }

  // ALTER TABLE succeeded, delete the backup of the old table.
  if (quick_rm_table(thd, old_db_type, alter_ctx.db, backup_name, FN_IS_TMP))
  {
    /*
      The fact that deletion of the backup failed is not critical
      error, but still worth reporting as it might indicate serious
      problem with server.
    */
    goto err_with_mdl_after_alter;
  }

end_inplace:

  if (thd->locked_tables_list.reopen_tables(thd))
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
  my_snprintf(alter_ctx.tmp_name, sizeof(alter_ctx.tmp_name),
              ER_THD(thd, ER_INSERT_INFO),
	      (ulong) (copied + deleted), (ulong) deleted,
	      (ulong) thd->get_stmt_da()->current_statement_warn_count());
  my_ok(thd, copied + deleted, 0L, alter_ctx.tmp_name);
  DBUG_RETURN(false);

err_new_table_cleanup:
  my_free(const_cast<uchar*>(frm.str));
  if (new_table)
  {
    /* close_temporary_table() frees the new_table pointer. */
    close_temporary_table(thd, new_table, true, true);
  }
  else
    (void) quick_rm_table(thd, new_db_type,
                          alter_ctx.new_db, alter_ctx.tmp_name,
                          (FN_IS_TMP | (no_ha_table ? NO_HA_TABLE : 0)));

  /*
    No default value was provided for a DATE/DATETIME field, the
    current sql_mode doesn't allow the '0000-00-00' value and
    the table to be altered isn't empty.
    Report error here.
  */
  if (alter_ctx.error_if_not_empty &&
      thd->get_stmt_da()->current_row_for_warning())
  {
    const char *f_val= 0;
    enum enum_mysql_timestamp_type t_type= MYSQL_TIMESTAMP_DATE;
    switch (alter_ctx.datetime_field->sql_type)
    {
      case MYSQL_TYPE_DATE:
      case MYSQL_TYPE_NEWDATE:
        f_val= "0000-00-00";
        t_type= MYSQL_TIMESTAMP_DATE;
        break;
      case MYSQL_TYPE_DATETIME:
      case MYSQL_TYPE_DATETIME2:
        f_val= "0000-00-00 00:00:00";
        t_type= MYSQL_TIMESTAMP_DATETIME;
        break;
      default:
        /* Shouldn't get here. */
        DBUG_ASSERT(0);
    }
    bool save_abort_on_warning= thd->abort_on_warning;
    thd->abort_on_warning= true;
    make_truncated_value_warning(thd, Sql_condition::WARN_LEVEL_WARN,
                                 f_val, strlength(f_val), t_type,
                                 alter_ctx.datetime_field->field_name);
    thd->abort_on_warning= save_abort_on_warning;
  }

  DBUG_RETURN(true);

err_with_mdl_after_alter:
  /* the table was altered. binlog the operation */
  write_bin_log(thd, true, thd->query(), thd->query_length());

err_with_mdl:
  /*
    An error happened while we were holding exclusive name metadata lock
    on table being altered. To be safe under LOCK TABLES we should
    remove all references to the altered table from the list of locked
    tables and release the exclusive metadata lock.
  */
  thd->locked_tables_list.unlink_all_closed_tables(thd, NULL, 0);
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
  if (ha_enable_transaction(thd, FALSE))
    DBUG_RETURN(TRUE);
  DBUG_RETURN(FALSE);
}


/**
  Commit the copy phase of the alter table.
*/

bool mysql_trans_commit_alter_copy_data(THD *thd)
{
  bool error= FALSE;
  DBUG_ENTER("mysql_trans_commit_alter_copy_data");

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
  SORT_FIELD *sortorder;
  SORT_INFO  *file_sort= 0;
  READ_RECORD info;
  TABLE_LIST   tables;
  List<Item>   fields;
  List<Item>   all_fields;
  bool auto_increment_field_copied= 0;
  bool init_read_record_done= 0;
  ulonglong save_sql_mode= thd->variables.sql_mode;
  ulonglong prev_insert_id, time_to_report_progress;
  Field **dfield_ptr= to->default_field;
  DBUG_ENTER("copy_data_between_tables");

  /* Two or 3 stages; Sorting, copying data and update indexes */
  thd_progress_init(thd, 2 + MY_TEST(order));

  if (mysql_trans_prepare_alter_copy_data(thd))
    DBUG_RETURN(-1);

  if (!(copy= new Copy_field[to->s->fields]))
    DBUG_RETURN(-1);				/* purecov: inspected */

  /* We need external lock before we can disable/enable keys */
  if (to->file->ha_external_lock(thd, F_WRLCK))
    DBUG_RETURN(-1);

  alter_table_manage_keys(to, from->file->indexes_are_disabled(), keys_onoff);

  /* We can abort alter table for any table type */
  thd->abort_on_warning= !ignore && thd->is_strict_mode();

  from->file->info(HA_STATUS_VARIABLE);
  to->file->ha_start_bulk_insert(from->file->stats.records,
                                 ignore ? 0 : HA_CREATE_UNIQUE_INDEX_BY_SORT);

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
      (copy_end++)->set(*ptr,def->field,0);
    }
    else
    {
      /*
        Update the set of auto-update fields to contain only the new fields
        added to the table. Only these fields should be updated automatically.
        Old fields keep their current values, and therefore should not be
        present in the set of autoupdate fields.
      */
      if ((*ptr)->has_insert_default_function())
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
      my_snprintf(warn_buff, sizeof(warn_buff), 
                  "ORDER BY ignored as there is a user-defined clustered index"
                  " in the table '%-.192s'", from->s->table_name.str);
      push_warning(thd, Sql_condition::WARN_LEVEL_WARN, ER_UNKNOWN_ERROR,
                   warn_buff);
    }
    else
    {
      uint length= 0;
      bzero((char *) &tables, sizeof(tables));
      tables.table= from;
      tables.alias= tables.table_name= from->s->table_name.str;
      tables.db= from->s->db.str;

      THD_STAGE_INFO(thd, stage_sorting);
      Filesort_tracker dummy_tracker(false);
      if (thd->lex->select_lex.setup_ref_array(thd, order_num) ||
          setup_order(thd, thd->lex->select_lex.ref_pointer_array,
                      &tables, fields, all_fields, order) ||
          !(sortorder= make_unireg_sortorder(thd, order, &length, NULL)) ||
          !(file_sort= filesort(thd, from, sortorder, length,
                               NULL, HA_POS_ERROR,
                               true,
                                &dummy_tracker)))
        goto err;
    }
    thd_progress_next_stage(thd);
  }

  THD_STAGE_INFO(thd, stage_copy_to_tmp_table);
  /* Tell handler that we have values for all columns in the to table */
  to->use_all_columns();
  to->mark_virtual_columns_for_write(TRUE);
  if (init_read_record(&info, thd, from, (SQL_SELECT *) 0, file_sort, 1, 1,
                       FALSE))
    goto err;
  init_read_record_done= 1;

  if (ignore && !alter_ctx->fk_error_if_delete_row)
    to->file->extra(HA_EXTRA_IGNORE_DUP_KEY);
  thd->get_stmt_da()->reset_current_row_for_warning();
  restore_record(to, s->default_values);        // Create empty record
  if (to->default_field && to->update_default_fields())
    goto err;

  thd->progress.max_counter= from->file->records();
  time_to_report_progress= MY_HOW_OFTEN_TO_WRITE/10;

  while (!(error=info.read_record(&info)))
  {
    if (thd->killed)
    {
      thd->send_kill_message();
      error= 1;
      break;
    }
    if (from->vfield)
      update_virtual_fields(thd, from);
    if (++thd->progress.counter >= time_to_report_progress)
    {
      time_to_report_progress+= MY_HOW_OFTEN_TO_WRITE/10;
      thd_progress_report(thd, thd->progress.counter,
                          thd->progress.max_counter);
    }

    /* Return error if source table isn't empty. */
    if (alter_ctx->error_if_not_empty)
    {
      error= 1;
      break;
    }
    if (to->next_number_field)
    {
      if (auto_increment_field_copied)
        to->auto_increment_field_not_null= TRUE;
      else
        to->next_number_field->reset();
    }
    
    for (Copy_field *copy_ptr=copy ; copy_ptr != copy_end ; copy_ptr++)
    {
      copy_ptr->do_copy(copy_ptr);
    }
    prev_insert_id= to->file->next_insert_id;
    if (to->vfield)
      update_virtual_fields(thd, to, VCOL_UPDATE_FOR_WRITE);
    if (thd->is_error())
    {
      error= 1;
      break;
    }
    error=to->file->ha_write_row(to->record[0]);
    to->auto_increment_field_not_null= FALSE;
    if (error)
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
        if (alter_ctx->fk_error_if_delete_row)
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
            if (key_nr == 0 &&
                (to->key_info[0].key_part[0].field->flags &
                 AUTO_INCREMENT_FLAG))
              err_msg= ER_THD(thd, ER_DUP_ENTRY_AUTOINCREMENT_CASE);
            print_keydup_error(to, key_nr == MAX_KEY ? NULL :
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

  if (error > 0)
  {
    /* We are going to drop the temporary table */
    to->file->extra(HA_EXTRA_PREPARE_FOR_DROP);
  }
  if (to->file->ha_end_bulk_insert() && error <= 0)
  {
    to->file->print_error(my_errno,MYF(0));
    error= 1;
  }
  to->file->extra(HA_EXTRA_NO_IGNORE_DUP_KEY);

  if (mysql_trans_commit_alter_copy_data(thd))
    error= 1;

 err:
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
  if (to->file->ha_external_lock(thd,F_UNLCK))
    error=1;
  if (error < 0 && to->file->extra(HA_EXTRA_PREPARE_FOR_RENAME))
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
  alter_info.flags= (Alter_info::ALTER_CHANGE_COLUMN |
                     Alter_info::ALTER_RECREATE);

  if (table_copy)
    alter_info.requested_algorithm= Alter_info::ALTER_TABLE_ALGORITHM_COPY;

  bool res= mysql_alter_table(thd, NullS, NullS, &create_info,
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

    strxmov(table_name, table->db ,".", table->table_name, NullS);

    /* Remember old 'next' pointer and break the list.  */
    save_next_global= table->next_global;
    table->next_global= NULL;
    table->lock_type= TL_READ;
    /* Allow to open real tables only. */
    table->required_type= FRMTYPE_TABLE;

    if (open_temporary_tables(thd, table) ||
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
	protocol->store((ulonglong)t->file->checksum());
      else if (check_opt->flags & T_QUICK)
	protocol->store_null();
      else
      {
	/* calculating table's checksum */
	ha_checksum crc= 0;
        uchar null_mask=256 -  (1 << t->s->last_null_bit_pos);

        t->use_all_columns();

	if (t->file->ha_rnd_init(1))
	  protocol->store_null();
	else
	{
	  for (;;)
	  {
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
	    ha_checksum row_crc= 0;
            int error= t->file->ha_rnd_next(t->record[0]);
            if (unlikely(error))
            {
              if (error == HA_ERR_RECORD_DELETED)
                continue;
              break;
            }
	    if (t->s->null_bytes)
            {
              /* fix undefined null bits */
              t->record[0][t->s->null_bytes-1] |= null_mask;
              if (!(t->s->db_create_options & HA_OPTION_PACK_RECORD))
                t->record[0][0] |= 1;

	      row_crc= my_checksum(row_crc, t->record[0], t->s->null_bytes);
            }

	    for (uint i= 0; i < t->s->fields; i++ )
	    {
	      Field *f= t->field[i];

              if (! thd->variables.old_mode && f->is_real_null(0))
                continue;
             /*
               BLOB and VARCHAR have pointers in their field, we must convert
               to string; GEOMETRY is implemented on top of BLOB.
               BIT may store its data among NULL bits, convert as well.
             */
              switch (f->type()) {
                case MYSQL_TYPE_BLOB:
                case MYSQL_TYPE_VARCHAR:
                case MYSQL_TYPE_GEOMETRY:
                case MYSQL_TYPE_BIT:
                {
                  String tmp;
                  f->val_str(&tmp);
                  row_crc= my_checksum(row_crc, (uchar*) tmp.ptr(),
                           tmp.length());
                  break;
                }
                default:
                  row_crc= my_checksum(row_crc, f->ptr, f->pack_length());
                  break;
	      }
	    }

	    crc+= row_crc;
	  }
	  protocol->store((ulonglong)crc);
          t->file->ha_rnd_end();
	}
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
      my_error(ER_UNKNOWN_STORAGE_ENGINE, MYF(0), engine_name, engine_name);
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
