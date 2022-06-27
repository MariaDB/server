/*
   Copyright (c) 2000, 2016, Oracle and/or its affiliates.
   Copyright (c) 2010, 2020, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */


/* Copy data from a textfile to table */
/* 2006-12 Erik Wetterberg : LOAD XML added */

#include "mariadb.h"
#include "sql_priv.h"
#include "unireg.h"
#include "sql_load.h"
#include "sql_load.h"
#include "sql_cache.h"                          // query_cache_*
#include "sql_base.h"          // fill_record_n_invoke_before_triggers
#include <my_dir.h>
#include "sql_view.h"                           // check_key_in_view
#include "sql_insert.h" // check_that_all_fields_are_given_values,
                        // write_record
#include "sql_acl.h"    // INSERT_ACL, UPDATE_ACL
#include "log_event.h"  // Delete_file_log_event,
                        // Execute_load_query_log_event,
                        // LOG_EVENT_UPDATE_TABLE_MAP_VERSION_F
#include <m_ctype.h>
#include "rpl_mi.h"
#include "sql_repl.h"
#include "sp_head.h"
#include "sql_trigger.h"
#include "sql_derived.h"
#include "sql_show.h"

#include "wsrep_mysqld.h"

#include "scope.h"  // scope_exit

extern "C" int _my_b_net_read(IO_CACHE *info, uchar *Buffer, size_t Count);

class XML_TAG {
public:
  int level;
  String field;
  String value;
  XML_TAG(int l, const String &f, const String &v);
};


XML_TAG::XML_TAG(int l, const String &f, const String &v)
{
  level= l;
  field.append(f);
  value.append(v);
}


/*
  Field and line terminators must be interpreted as sequence of unsigned char.
  Otherwise, non-ascii terminators will be negative on some platforms,
  and positive on others (depending on the implementation of char).
*/
class Term_string
{
  const uchar *m_ptr;
  uint m_length;
  int m_initial_byte;
public:
  Term_string(const String &str) :
    m_ptr(static_cast<const uchar*>(static_cast<const void*>(str.ptr()))),
    m_length(str.length()),
    m_initial_byte((uchar) (str.length() ? str.ptr()[0] : INT_MAX))
  { }
  void set(const uchar *str, uint length, int initial_byte)
  {
    m_ptr= str;
    m_length= length;
    m_initial_byte= initial_byte;
  }
  void reset() { set(NULL, 0, INT_MAX); }
  const uchar *ptr() const { return m_ptr; }
  uint length() const { return m_length; }
  int initial_byte() const { return m_initial_byte; }
  bool eq(const Term_string &other) const
  {
    return length() == other.length() && !memcmp(ptr(), other.ptr(), length());
  }
};


#define GET (stack_pos != stack ? *--stack_pos : my_b_get(&cache))
#define PUSH(A) *(stack_pos++)=(A)

#ifdef WITH_WSREP
/** If requested by wsrep_load_data_splitting and streaming replication is
    not enabled, replicate a streaming fragment every 10,000 rows.*/
class Wsrep_load_data_split
{
public:
  Wsrep_load_data_split(THD *thd)
    : m_thd(thd)
    , m_load_data_splitting(wsrep_load_data_splitting)
    , m_fragment_unit(thd->wsrep_trx().streaming_context().fragment_unit())
    , m_fragment_size(thd->wsrep_trx().streaming_context().fragment_size())
  {
    if (WSREP(m_thd) && m_load_data_splitting)
    {
      /* Override streaming settings with backward compatible values for
         load data splitting */
      m_thd->wsrep_cs().streaming_params(wsrep::streaming_context::row, 10000);
    }
  }

  ~Wsrep_load_data_split()
  {
    if (WSREP(m_thd) && m_load_data_splitting)
    {
      /* Restore original settings */
      m_thd->wsrep_cs().streaming_params(m_fragment_unit, m_fragment_size);
    }
  }
private:
  THD *m_thd;
  my_bool m_load_data_splitting;
  enum wsrep::streaming_context::fragment_unit m_fragment_unit;
  size_t m_fragment_size;
};
#endif /* WITH_WSREP */

class READ_INFO: public Load_data_param
{
  File	file;
  String data;                          /* Read buffer */
  Term_string m_field_term;             /* FIELDS TERMINATED BY 'string' */
  Term_string m_line_term;              /* LINES TERMINATED BY 'string' */
  Term_string m_line_start;             /* LINES STARTING BY 'string' */
  int	enclosed_char,escape_char;
  int	*stack,*stack_pos;
  bool	found_end_of_line,start_of_line,eof;
  int level; /* for load xml */

  bool getbyte(char *to)
  {
    int chr= GET;
    if (chr == my_b_EOF)
      return (eof= true);
    *to= chr;
    return false;
  }

  /**
    Read a tail of a multi-byte character.
    The first byte of the character is assumed to be already
    read from the file and appended to "str".

    @returns  true  - if EOF happened unexpectedly
    @returns  false - no EOF happened: found a good multi-byte character,
                                       or a bad byte sequence

    Note:
    The return value depends only on EOF:
    - read_mbtail() returns "false" is a good character was read, but also
    - read_mbtail() returns "false" if an incomplete byte sequence was found
      and no EOF happened.

    For example, suppose we have an ujis file with bytes 0x8FA10A, where:
    - 0x8FA1 is an incomplete prefix of a 3-byte character
      (it should be [8F][A1-FE][A1-FE] to make a full 3-byte character)
    - 0x0A is a line demiliter
    This file has some broken data, the trailing [A1-FE] is missing.

    In this example it works as follows:
    - 0x8F is read from the file and put into "data" before the call
      for read_mbtail()
    - 0xA1 is read from the file and put into "data" by read_mbtail()
    - 0x0A is kept in the read queue, so the next read iteration after
      the current read_mbtail() call will normally find it and recognize as
      a line delimiter
    - the current call for read_mbtail() returns "false",
      because no EOF happened
  */
  bool read_mbtail(String *str)
  {
    int chlen;
    if ((chlen= charset()->charlen(str->end() - 1, str->end())) == 1)
      return false; // Single byte character found
    for (uint32 length0= str->length() - 1 ; MY_CS_IS_TOOSMALL(chlen); )
    {
      int chr= GET;
      if (chr == my_b_EOF)
      {
        DBUG_PRINT("info", ("read_mbtail: chlen=%d; unexpected EOF", chlen));
        return true; // EOF
      }
      str->append(chr);
      chlen= charset()->charlen(str->ptr() + length0, str->end());
      if (chlen == MY_CS_ILSEQ)
      {
        /**
          It has been an incomplete (but a valid) sequence so far,
          but the last byte turned it into a bad byte sequence.
          Unget the very last byte.
        */
        str->length(str->length() - 1);
        PUSH(chr);
        DBUG_PRINT("info", ("read_mbtail: ILSEQ"));
        return false; // Bad byte sequence
      }
    }
    DBUG_PRINT("info", ("read_mbtail: chlen=%d", chlen));
    return false; // Good multi-byte character
  }

public:
  bool error,line_cuted,found_null,enclosed;
  uchar	*row_start,			/* Found row starts here */
	*row_end;			/* Found row ends here */
  LOAD_FILE_IO_CACHE cache;

  READ_INFO(THD *thd, File file, const Load_data_param &param,
	    String &field_term,String &line_start,String &line_term,
	    String &enclosed,int escape,bool get_it_from_net, bool is_fifo);
  ~READ_INFO();
  int read_field();
  int read_fixed_length(void);
  int next_line(void);
  char unescape(char chr);
  bool terminator(const uchar *ptr, uint length);
  bool terminator(const Term_string &str)
  { return terminator(str.ptr(), str.length()); }
  bool terminator(int chr, const Term_string &str)
  { return str.initial_byte() == chr && terminator(str); }
  bool find_start_of_fields();
  /* load xml */
  List<XML_TAG> taglist;
  int read_value(int delim, String *val);
  int read_xml(THD *thd);
  int clear_level(int level);

  my_off_t file_length() { return cache.end_of_file; }
  my_off_t position()    { return my_b_tell(&cache); }

  /**
    skip all data till the eof.
  */
  void skip_data_till_eof()
  {
    while (GET != my_b_EOF)
      ;
  }
};

static int read_fixed_length(THD *thd, COPY_INFO &info, TABLE_LIST *table_list,
                             List<Item> &fields_vars, List<Item> &set_fields,
                             List<Item> &set_values, READ_INFO &read_info,
			     ulong skip_lines,
			     bool ignore_check_option_errors);
static int read_sep_field(THD *thd, COPY_INFO &info, TABLE_LIST *table_list,
                          List<Item> &fields_vars, List<Item> &set_fields,
                          List<Item> &set_values, READ_INFO &read_info,
			  String &enclosed, ulong skip_lines,
			  bool ignore_check_option_errors);

static int read_xml_field(THD *thd, COPY_INFO &info, TABLE_LIST *table_list,
                          List<Item> &fields_vars, List<Item> &set_fields,
                          List<Item> &set_values, READ_INFO &read_info,
                          String &enclosed, ulong skip_lines,
                          bool ignore_check_option_errors);

#ifndef EMBEDDED_LIBRARY
static bool write_execute_load_query_log_event(THD *, const sql_exchange*, const
           char*, const char*, bool, enum enum_duplicates, bool, bool, int);
#endif /* EMBEDDED_LIBRARY */


bool Load_data_param::add_outvar_field(THD *thd, const Field *field)
{
  if (field->flags & BLOB_FLAG)
  {
    m_use_blobs= true;
    m_fixed_length+= 256;  // Will be extended if needed
  }
  else
    m_fixed_length+= field->field_length;
  return false;
}


bool Load_data_param::add_outvar_user_var(THD *thd)
{
  if (m_is_fixed_length)
  {
    my_error(ER_LOAD_FROM_FIXED_SIZE_ROWS_TO_VAR, MYF(0));
    return true;
  }
  return false;
}


/*
  Execute LOAD DATA query

  SYNOPSYS
    mysql_load()
      thd - current thread
      ex  - sql_exchange object representing source file and its parsing rules
      table_list  - list of tables to which we are loading data
      fields_vars - list of fields and variables to which we read
                    data from file
      set_fields  - list of fields mentioned in set clause
      set_values  - expressions to assign to fields in previous list
      handle_duplicates - indicates whenever we should emit error or
                          replace row if we will meet duplicates.
      ignore -          - indicates whenever we should ignore duplicates
      read_file_from_client - is this LOAD DATA LOCAL ?

  RETURN VALUES
    TRUE - error / FALSE - success
*/

int mysql_load(THD *thd, const sql_exchange *ex, TABLE_LIST *table_list,
	        List<Item> &fields_vars, List<Item> &set_fields,
                List<Item> &set_values,
                enum enum_duplicates handle_duplicates, bool ignore,
                bool read_file_from_client)
{
  char name[FN_REFLEN];
  File file;
  TABLE *table= NULL;
  int error= 0;
  bool is_fifo=0;
#ifndef EMBEDDED_LIBRARY
  killed_state killed_status;
  bool is_concurrent;
#endif
  const char *db= table_list->db.str;		// This is never null
  /*
    If path for file is not defined, we will use the current database.
    If this is not set, we will use the directory where the table to be
    loaded is located
  */
  const char *tdb= thd->db.str ? thd->db.str : db; // Result is never null
  ulong skip_lines= ex->skip_lines;
  bool transactional_table __attribute__((unused));
  DBUG_ENTER("mysql_load");

#ifdef WITH_WSREP
  Wsrep_load_data_split wsrep_load_data_split(thd);
#endif /* WITH_WSREP */
  /*
    Bug #34283
    mysqlbinlog leaves tmpfile after termination if binlog contains
    load data infile, so in mixed mode we go to row-based for
    avoiding the problem.
  */
  thd->set_current_stmt_binlog_format_row_if_mixed();

#ifdef EMBEDDED_LIBRARY
  read_file_from_client  = 0; //server is always in the same process 
#endif

  if (ex->escaped->length() > 1 || ex->enclosed->length() > 1)
  {
    my_message(ER_WRONG_FIELD_TERMINATORS,
               ER_THD(thd, ER_WRONG_FIELD_TERMINATORS),
	       MYF(0));
    DBUG_RETURN(TRUE);
  }

  /* Report problems with non-ascii separators */
  if (!ex->escaped->is_ascii() || !ex->enclosed->is_ascii() ||
      !ex->field_term->is_ascii() ||
      !ex->line_term->is_ascii() || !ex->line_start->is_ascii())
  {
    push_warning(thd, Sql_condition::WARN_LEVEL_WARN,
                 WARN_NON_ASCII_SEPARATOR_NOT_IMPLEMENTED,
                 ER_THD(thd, WARN_NON_ASCII_SEPARATOR_NOT_IMPLEMENTED));
  } 

  if (open_and_lock_tables(thd, table_list, TRUE, 0))
    DBUG_RETURN(TRUE);
  if (table_list->handle_derived(thd->lex, DT_MERGE_FOR_INSERT))
    DBUG_RETURN(TRUE);
  if (thd->lex->handle_list_of_derived(table_list, DT_PREPARE))
    DBUG_RETURN(TRUE);

  if (setup_tables_and_check_access(thd,
                                    &thd->lex->first_select_lex()->context,
                                    &thd->lex->first_select_lex()->
                                      top_join_list,
                                    table_list,
                                    thd->lex->first_select_lex()->leaf_tables,
                                    FALSE,
                                    INSERT_ACL | UPDATE_ACL,
                                    INSERT_ACL | UPDATE_ACL, FALSE))
     DBUG_RETURN(-1);
  if (!table_list->table ||               // do not suport join view
      !table_list->single_table_updatable() || // and derived tables
      check_key_in_view(thd, table_list))
  {
    my_error(ER_NON_UPDATABLE_TABLE, MYF(0), table_list->alias.str, "LOAD");
    DBUG_RETURN(TRUE);
  }
  if (table_list->is_multitable())
  {
    my_error(ER_WRONG_USAGE, MYF(0), "Multi-table VIEW", "LOAD");
    DBUG_RETURN(TRUE);
  }
  if (table_list->prepare_where(thd, 0, TRUE) ||
      table_list->prepare_check_option(thd))
  {
    DBUG_RETURN(TRUE);
  }
  thd_proc_info(thd, "Executing");
  /*
    Let us emit an error if we are loading data to table which is used
    in subselect in SET clause like we do it for INSERT.

    The main thing to fix to remove this restriction is to ensure that the
    table is marked to be 'used for insert' in which case we should never
    mark this table as 'const table' (ie, one that has only one row).
  */
  if (unique_table(thd, table_list, table_list->next_global, 0))
  {
    my_error(ER_UPDATE_TABLE_USED, MYF(0), table_list->table_name.str,
             "LOAD DATA");
    DBUG_RETURN(TRUE);
  }

  table= table_list->table;
  transactional_table= table->file->has_transactions_and_rollback();
#ifndef EMBEDDED_LIBRARY
  is_concurrent= (table_list->lock_type == TL_WRITE_CONCURRENT_INSERT);
#endif

  if (check_duplic_insert_without_overlaps(thd, table, handle_duplicates) != 0)
    DBUG_RETURN(true);

  auto scope_cleaner = make_scope_exit(
    [&fields_vars]() {
      fields_vars.empty();
    }
  );

  if (!fields_vars.elements)
  {
    Field_iterator_table_ref field_iterator;
    field_iterator.set(table_list);
    for (; !field_iterator.end_of_fields(); field_iterator.next())
    {
      if (field_iterator.field() &&
              field_iterator.field()->invisible > VISIBLE)
        continue;
      Item *item;
      if (!(item= field_iterator.create_item(thd)))
        DBUG_RETURN(TRUE);
      fields_vars.push_back(item->real_item(), thd->mem_root);
    }
    bitmap_set_all(table->write_set);
    /*
      Let us also prepare SET clause, altough it is probably empty
      in this case.
    */
    if (setup_fields(thd, Ref_ptr_array(),
                     set_fields, MARK_COLUMNS_WRITE, 0, NULL, 0) ||
        setup_fields(thd, Ref_ptr_array(),
                     set_values, MARK_COLUMNS_READ, 0, NULL, 0))
      DBUG_RETURN(TRUE);
  }
  else
  {						// Part field list
    scope_cleaner.release();
    /* TODO: use this conds for 'WITH CHECK OPTIONS' */
    if (setup_fields(thd, Ref_ptr_array(),
                     fields_vars, MARK_COLUMNS_WRITE, 0, NULL, 0) ||
        setup_fields(thd, Ref_ptr_array(),
                     set_fields, MARK_COLUMNS_WRITE, 0, NULL, 0) ||
        check_that_all_fields_are_given_values(thd, table, table_list))
      DBUG_RETURN(TRUE);
    /* Fix the expressions in SET clause */
    if (setup_fields(thd, Ref_ptr_array(),
                     set_values, MARK_COLUMNS_READ, 0, NULL, 0))
      DBUG_RETURN(TRUE);
  }
  switch_to_nullable_trigger_fields(fields_vars, table);
  switch_to_nullable_trigger_fields(set_fields, table);
  switch_to_nullable_trigger_fields(set_values, table);

  table->prepare_triggers_for_insert_stmt_or_event();
  table->mark_columns_needed_for_insert();

  Load_data_param param(ex->cs ? ex->cs : thd->variables.collation_database,
                        !ex->field_term->length() && !ex->enclosed->length());
  List_iterator_fast<Item> it(fields_vars);
  Item *item;

  while ((item= it++))
  {
    const Load_data_outvar *var= item->get_load_data_outvar_or_error();
    if (!var || var->load_data_add_outvar(thd, &param))
      DBUG_RETURN(true);
  }
  if (param.use_blobs() && !ex->line_term->length() && !ex->field_term->length())
  {
    my_message(ER_BLOBS_AND_NO_TERMINATED,
               ER_THD(thd, ER_BLOBS_AND_NO_TERMINATED), MYF(0));
    DBUG_RETURN(TRUE);
  }

  /* We can't give an error in the middle when using LOCAL files */
  if (read_file_from_client && handle_duplicates == DUP_ERROR)
    ignore= 1;

#ifndef EMBEDDED_LIBRARY
  if (read_file_from_client)
  {
    (void)net_request_file(&thd->net,ex->file_name);
    file = -1;
  }
  else
#endif
  {
#ifdef DONT_ALLOW_FULL_LOAD_DATA_PATHS
    ex->file_name+=dirname_length(ex->file_name);
#endif
    if (!dirname_length(ex->file_name))
    {
      strxnmov(name, FN_REFLEN-1, mysql_real_data_home, tdb, NullS);
      (void) fn_format(name, ex->file_name, name, "",
		       MY_RELATIVE_PATH | MY_UNPACK_FILENAME);
    }
    else
    {
      (void) fn_format(name, ex->file_name, mysql_real_data_home, "",
                       MY_RELATIVE_PATH | MY_UNPACK_FILENAME |
                       MY_RETURN_REAL_PATH);
    }

    if (thd->rgi_slave)
    {
#if defined(HAVE_REPLICATION) && !defined(MYSQL_CLIENT)
      if (strncmp(thd->rgi_slave->rli->slave_patternload_file, name,
                  thd->rgi_slave->rli->slave_patternload_file_size))
      {
        /*
          LOAD DATA INFILE in the slave SQL Thread can only read from 
          --slave-load-tmpdir". This should never happen. Please, report a bug.
        */

        sql_print_error("LOAD DATA INFILE in the slave SQL Thread can only read from --slave-load-tmpdir. " \
                        "Please, report a bug.");
        my_error(ER_OPTION_PREVENTS_STATEMENT, MYF(0), "--slave-load-tmpdir");
        DBUG_RETURN(TRUE);
      }
#else
      /*
        This is impossible and should never happen.
      */
      DBUG_ASSERT(FALSE); 
#endif
    }
    else if (!is_secure_file_path(name))
    {
      /* Read only allowed from within dir specified by secure_file_priv */
      my_error(ER_OPTION_PREVENTS_STATEMENT, MYF(0), "--secure-file-priv");
      DBUG_RETURN(TRUE);
    }

#if !defined(_WIN32)
    MY_STAT stat_info;
    if (!my_stat(name, &stat_info, MYF(MY_WME)))
      DBUG_RETURN(TRUE);

    // if we are not in slave thread, the file must be:
    if (!thd->slave_thread &&
        !((stat_info.st_mode & S_IFLNK) != S_IFLNK &&   // symlink
          ((stat_info.st_mode & S_IFREG) == S_IFREG ||  // regular file
           (stat_info.st_mode & S_IFIFO) == S_IFIFO)))  // named pipe
    {
      my_error(ER_TEXTFILE_NOT_READABLE, MYF(0), name);
      DBUG_RETURN(TRUE);
    }
    if ((stat_info.st_mode & S_IFIFO) == S_IFIFO)
      is_fifo= 1;
#endif
    if ((file= mysql_file_open(key_file_load,
                               name, O_RDONLY, MYF(MY_WME))) < 0)

      DBUG_RETURN(TRUE);
  }

  COPY_INFO info;
  bzero((char*) &info,sizeof(info));
  info.ignore= ignore;
  info.handle_duplicates=handle_duplicates;
  info.escape_char= (ex->escaped->length() && (ex->escaped_given() ||
                    !(thd->variables.sql_mode & MODE_NO_BACKSLASH_ESCAPES)))
                    ? (*ex->escaped)[0] : INT_MAX;

  READ_INFO read_info(thd, file, param,
                      *ex->field_term, *ex->line_start,
                      *ex->line_term, *ex->enclosed,
		      info.escape_char, read_file_from_client, is_fifo);
  if (unlikely(read_info.error))
  {
    if (file >= 0)
      mysql_file_close(file, MYF(0));           // no files in net reading
    DBUG_RETURN(TRUE);				// Can't allocate buffers
  }

#ifndef EMBEDDED_LIBRARY
  if (mysql_bin_log.is_open())
  {
    read_info.cache.thd = thd;
    read_info.cache.wrote_create_file = 0;
    read_info.cache.last_pos_in_file = HA_POS_ERROR;
    read_info.cache.log_delayed= transactional_table;
  }
#endif /*!EMBEDDED_LIBRARY*/

  thd->count_cuted_fields= CHECK_FIELD_WARN;		/* calc cuted fields */
  thd->cuted_fields=0L;
  /* Skip lines if there is a line terminator */
  if (ex->line_term->length() && ex->filetype != FILETYPE_XML)
  {
    /* ex->skip_lines needs to be preserved for logging */
    while (skip_lines > 0)
    {
      skip_lines--;
      if (read_info.next_line())
	break;
    }
  }

  thd_proc_info(thd, "Reading file");
  if (likely(!(error= MY_TEST(read_info.error))))
  {
    table->reset_default_fields();
    table->next_number_field=table->found_next_number_field;
    if (ignore ||
	handle_duplicates == DUP_REPLACE)
      table->file->extra(HA_EXTRA_IGNORE_DUP_KEY);
    if (handle_duplicates == DUP_REPLACE &&
        (!table->triggers ||
         !table->triggers->has_delete_triggers()))
        table->file->extra(HA_EXTRA_WRITE_CAN_REPLACE);
    if (thd->locked_tables_mode <= LTM_LOCK_TABLES)
      table->file->ha_start_bulk_insert((ha_rows) 0);
    table->copy_blobs=1;

    thd->abort_on_warning= !ignore && thd->is_strict_mode();
    thd->get_stmt_da()->reset_current_row_for_warning(1);

    bool create_lookup_handler= handle_duplicates != DUP_ERROR;
    if ((table_list->table->file->ha_table_flags() & HA_DUPLICATE_POS))
    {
      create_lookup_handler= true;
      if ((error= table_list->table->file->ha_rnd_init_with_error(0)))
        goto err;
    }
    table->file->prepare_for_insert(create_lookup_handler);
    thd_progress_init(thd, 2);
    fix_rownum_pointers(thd, thd->lex->current_select, &info.copied);
    if (table_list->table->validate_default_values_of_unset_fields(thd))
    {
      read_info.error= true;
      error= 1;
    }
    else if (ex->filetype == FILETYPE_XML) /* load xml */
      error= read_xml_field(thd, info, table_list, fields_vars,
                            set_fields, set_values, read_info,
                            *(ex->line_term), skip_lines, ignore);
    else if (read_info.is_fixed_length())
      error= read_fixed_length(thd, info, table_list, fields_vars,
                               set_fields, set_values, read_info,
			       skip_lines, ignore);
    else
      error= read_sep_field(thd, info, table_list, fields_vars,
                            set_fields, set_values, read_info,
                            *ex->enclosed, skip_lines, ignore);

    if (table_list->table->file->ha_table_flags() & HA_DUPLICATE_POS)
      table_list->table->file->ha_rnd_end();

    thd_proc_info(thd, "End bulk insert");
    if (likely(!error))
      thd_progress_next_stage(thd);
    if (thd->locked_tables_mode <= LTM_LOCK_TABLES &&
        table->file->ha_end_bulk_insert() && !error)
    {
      table->file->print_error(my_errno, MYF(0));
      error= 1;
    }
    table->file->extra(HA_EXTRA_NO_IGNORE_DUP_KEY);
    table->file->extra(HA_EXTRA_WRITE_CANNOT_REPLACE);
    table->next_number_field=0;
  }
  if (file >= 0)
    mysql_file_close(file, MYF(0));
  free_blobs(table);				/* if pack_blob was used */
  table->copy_blobs=0;
  thd->count_cuted_fields= CHECK_FIELD_IGNORE;
  /* 
     simulated killing in the middle of per-row loop
     must be effective for binlogging
  */
  DBUG_EXECUTE_IF("simulate_kill_bug27571",
                  {
                    error=1;
                    thd->set_killed(KILL_QUERY);
                  };);

#ifndef EMBEDDED_LIBRARY
  killed_status= (error == 0) ? NOT_KILLED : thd->killed;
#endif

  /*
    We must invalidate the table in query cache before binlog writing and
    ha_autocommit_...
  */
  query_cache_invalidate3(thd, table_list, 0);
  if (error)
  {
    if (read_file_from_client)
      read_info.skip_data_till_eof();

#ifndef EMBEDDED_LIBRARY
    if (mysql_bin_log.is_open())
    {
      {
	/*
          Make sure last block (the one which caused the error) gets
          logged.
	*/
        log_loaded_block(&read_info.cache, 0, 0);
	/* If the file was not empty, wrote_create_file is true */
        if (read_info.cache.wrote_create_file)
	{
          int errcode= query_error_code(thd, killed_status == NOT_KILLED);
          
          /* since there is already an error, the possible error of
             writing binary log will be ignored */
	  if (thd->transaction->stmt.modified_non_trans_table)
            (void) write_execute_load_query_log_event(thd, ex,
                                                      table_list->db.str,
                                                      table_list->table_name.str,
                                                      is_concurrent,
                                                      handle_duplicates, ignore,
                                                      transactional_table,
                                                      errcode);
	  else
	  {
	    Delete_file_log_event d(thd, db, transactional_table);
	    (void) mysql_bin_log.write(&d);
	  }
	}
      }
    }
#endif /*!EMBEDDED_LIBRARY*/
    error= -1;				// Error on read
    goto err;
  }
  sprintf(name, ER_THD(thd, ER_LOAD_INFO),
          (ulong) info.records, (ulong) info.deleted,
	  (ulong) (info.records - info.copied),
          (long) thd->get_stmt_da()->current_statement_warn_count());

  if (thd->transaction->stmt.modified_non_trans_table)
    thd->transaction->all.modified_non_trans_table= TRUE;
  thd->transaction->all.m_unsafe_rollback_flags|=
    (thd->transaction->stmt.m_unsafe_rollback_flags & THD_TRANS::DID_WAIT);
#ifndef EMBEDDED_LIBRARY
  if (mysql_bin_log.is_open())
  {
    /*
      We need to do the job that is normally done inside
      binlog_query() here, which is to ensure that the pending event
      is written before tables are unlocked and before any other
      events are written.  We also need to update the table map
      version for the binary log to mark that table maps are invalid
      after this point.
     */
    if (thd->is_current_stmt_binlog_format_row())
      error= thd->binlog_flush_pending_rows_event(TRUE, transactional_table);
    else
    {
      /*
        As already explained above, we need to call log_loaded_block() to have
        the last block logged
      */
      log_loaded_block(&read_info.cache, 0, 0);
      if (read_info.cache.wrote_create_file)
      {
        int errcode= query_error_code(thd, killed_status == NOT_KILLED);
        error= write_execute_load_query_log_event(thd, ex,
                                                  table_list->db.str,
                                                  table_list->table_name.str,
                                                  is_concurrent,
                                                  handle_duplicates, ignore,
                                                  transactional_table,
                                                  errcode);
      }

      /*
        Flushing the IO CACHE while writing the execute load query log event
        may result in error (for instance, because the max_binlog_size has been 
        reached, and rotation of the binary log failed).
      */
      error= error || mysql_bin_log.get_log_file()->error;
    }
    if (unlikely(error))
      goto err;
  }
#endif /*!EMBEDDED_LIBRARY*/

  /* ok to client sent only after binlog write and engine commit */
  my_ok(thd, info.copied + info.deleted, 0L, name);
err:
  DBUG_ASSERT(transactional_table || !(info.copied || info.deleted) ||
              thd->transaction->stmt.modified_non_trans_table);
  table->file->ha_release_auto_increment();
  table->auto_increment_field_not_null= FALSE;
  thd->abort_on_warning= 0;
  DBUG_RETURN(error);
}


#ifndef EMBEDDED_LIBRARY

/* Not a very useful function; just to avoid duplication of code */
static bool write_execute_load_query_log_event(THD *thd, const sql_exchange* ex,
                                               const char* db_arg,  /* table's database */
                                               const char* table_name_arg,
                                               bool is_concurrent,
                                               enum enum_duplicates duplicates,
                                               bool ignore,
                                               bool transactional_table,
                                               int errcode)
{
  char                *load_data_query;
  my_off_t            fname_start,
                      fname_end;
  List<Item>           fv;
  Item                *item, *val;
  int                  n;
  const char          *tdb= (thd->db.str != NULL ? thd->db.str : db_arg);
  const char          *qualify_db= NULL;
  char                command_buffer[1024];
  String              query_str(command_buffer, sizeof(command_buffer),
                              system_charset_info);

  Load_log_event       lle(thd, ex, tdb, table_name_arg, fv, is_concurrent,
                           duplicates, ignore, transactional_table);

  /*
    force in a LOCAL if there was one in the original.
  */
  if (thd->lex->local_file)
    lle.set_fname_outside_temp_buf(ex->file_name, strlen(ex->file_name));

  query_str.length(0);
  if (!thd->db.str || strcmp(db_arg, thd->db.str))
  {
    /*
      If used database differs from table's database, 
      prefix table name with database name so that it 
      becomes a FQ name.
     */
    qualify_db= db_arg;
  }
  lle.print_query(thd, FALSE, (const char*) ex->cs ? ex->cs->cs_name.str : NULL,
                  &query_str, &fname_start, &fname_end, qualify_db);

  /*
    prepare fields-list and SET if needed; print_query won't do that for us.
  */
  if (!thd->lex->field_list.is_empty())
  {
    List_iterator<Item>  li(thd->lex->field_list);

    query_str.append(STRING_WITH_LEN(" ("));
    n= 0;

    while ((item= li++))
    {
      if (n++)
        query_str.append(STRING_WITH_LEN(", "));
      const Load_data_outvar *var= item->get_load_data_outvar();
      DBUG_ASSERT(var);
      var->load_data_print_for_log_event(thd, &query_str);
    }
    query_str.append(')');
  }

  if (!thd->lex->update_list.is_empty())
  {
    List_iterator<Item> lu(thd->lex->update_list);
    List_iterator<Item> lv(thd->lex->value_list);

    query_str.append(STRING_WITH_LEN(" SET "));
    n= 0;

    while ((item= lu++))
    {
      val= lv++;
      if (n++)
        query_str.append(STRING_WITH_LEN(", "));
      append_identifier(thd, &query_str, &item->name);
      query_str.append(&val->name);
    }
  }

  if (!(load_data_query= (char *)thd->strmake(query_str.ptr(), query_str.length())))
    return TRUE;

  Execute_load_query_log_event
    e(thd, load_data_query, query_str.length(),
      (uint) (fname_start - 1), (uint) fname_end,
      (duplicates == DUP_REPLACE) ? LOAD_DUP_REPLACE :
      (ignore ? LOAD_DUP_IGNORE : LOAD_DUP_ERROR),
      transactional_table, FALSE, FALSE, errcode);
  return mysql_bin_log.write(&e);
}

#endif

/****************************************************************************
** Read of rows of fixed size + optional garage + optonal newline
****************************************************************************/

static int
read_fixed_length(THD *thd, COPY_INFO &info, TABLE_LIST *table_list,
                  List<Item> &fields_vars, List<Item> &set_fields,
                  List<Item> &set_values, READ_INFO &read_info,
                  ulong skip_lines, bool ignore_check_option_errors)
{
  List_iterator_fast<Item> it(fields_vars);
  Item *item;
  TABLE *table= table_list->table;
  bool err, progress_reports;
  ulonglong counter, time_to_report_progress;
  DBUG_ENTER("read_fixed_length");

  counter= 0;
  time_to_report_progress= MY_HOW_OFTEN_TO_WRITE/10;
  progress_reports= 1;
  if ((thd->progress.max_counter= read_info.file_length()) == ~(my_off_t) 0)
    progress_reports= 0;

  while (!read_info.read_fixed_length())
  {
    if (thd->killed)
    {
      thd->send_kill_message();
      DBUG_RETURN(1);
    }
    if (progress_reports)
    {
      thd->progress.counter= read_info.position();
      if (++counter >= time_to_report_progress)
      {
        time_to_report_progress+= MY_HOW_OFTEN_TO_WRITE/10;
        thd_progress_report(thd, thd->progress.counter,
                            thd->progress.max_counter);
      }
    }
    if (skip_lines)
    {
      /*
	We could implement this with a simple seek if:
	- We are not using DATA INFILE LOCAL
	- escape character is  ""
	- line starting prefix is ""
      */
      skip_lines--;
      continue;
    }
    it.rewind();
    uchar *pos=read_info.row_start;
#ifdef HAVE_valgrind
    read_info.row_end[0]=0;
#endif

    restore_record(table, s->default_values);

    while ((item= it++))
    {
      Load_data_outvar *dst= item->get_load_data_outvar();
      DBUG_ASSERT(dst);
      if (pos == read_info.row_end)
      {
        if (dst->load_data_set_no_data(thd, &read_info))
          DBUG_RETURN(1);
      }
      else
      {
        uint length, fixed_length= dst->load_data_fixed_length();
        uchar save_chr;
        if ((length=(uint) (read_info.row_end - pos)) > fixed_length)
          length= fixed_length;
        save_chr= pos[length]; pos[length]= '\0'; // Safeguard aganst malloc
        dst->load_data_set_value(thd, (const char *) pos, length, &read_info);
        pos[length]= save_chr;
        if ((pos+= length) > read_info.row_end)
          pos= read_info.row_end;               // Fills rest with space
      }
    }
    if (pos != read_info.row_end)
    {
      thd->cuted_fields++;			/* To long row */
      push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                          ER_WARN_TOO_MANY_RECORDS,
                          ER_THD(thd, ER_WARN_TOO_MANY_RECORDS),
                          thd->get_stmt_da()->current_row_for_warning());
    }

    if (thd->killed ||
        fill_record_n_invoke_before_triggers(thd, table, set_fields, set_values,
                                             ignore_check_option_errors,
                                             TRG_EVENT_INSERT))
      DBUG_RETURN(1);

    switch (table_list->view_check_option(thd, ignore_check_option_errors)) {
    case VIEW_CHECK_SKIP:
      read_info.next_line();
      goto continue_loop;
    case VIEW_CHECK_ERROR:
      DBUG_RETURN(-1);
    }

    err= write_record(thd, table, &info);
    table->auto_increment_field_not_null= FALSE;
    if (err)
      DBUG_RETURN(1);
   
    /*
      We don't need to reset auto-increment field since we are restoring
      its default value at the beginning of each loop iteration.
    */
    if (read_info.next_line())			// Skip to next line
      break;
    if (read_info.line_cuted)
    {
      thd->cuted_fields++;			/* To long row */
      push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                          ER_WARN_TOO_MANY_RECORDS,
                          ER_THD(thd, ER_WARN_TOO_MANY_RECORDS),
                          thd->get_stmt_da()->current_row_for_warning());
    }
    thd->get_stmt_da()->inc_current_row_for_warning();
continue_loop:;
  }
  DBUG_RETURN(MY_TEST(read_info.error));
}


static int
read_sep_field(THD *thd, COPY_INFO &info, TABLE_LIST *table_list,
               List<Item> &fields_vars, List<Item> &set_fields,
               List<Item> &set_values, READ_INFO &read_info,
	       String &enclosed, ulong skip_lines,
	       bool ignore_check_option_errors)
{
  List_iterator_fast<Item> it(fields_vars);
  Item *item;
  TABLE *table= table_list->table;
  uint enclosed_length;
  bool err, progress_reports;
  ulonglong counter, time_to_report_progress;
  DBUG_ENTER("read_sep_field");

  enclosed_length=enclosed.length();

  counter= 0;
  time_to_report_progress= MY_HOW_OFTEN_TO_WRITE/10;
  progress_reports= 1;
  if ((thd->progress.max_counter= read_info.file_length()) == ~(my_off_t) 0)
    progress_reports= 0;

  for (;;it.rewind())
  {
    if (thd->killed)
    {
      thd->send_kill_message();
      DBUG_RETURN(1);
    }

    if (progress_reports)
    {
      thd->progress.counter= read_info.position();
      if (++counter >= time_to_report_progress)
      {
        time_to_report_progress+= MY_HOW_OFTEN_TO_WRITE/10;
        thd_progress_report(thd, thd->progress.counter,
                            thd->progress.max_counter);
      }
    }
    restore_record(table, s->default_values);

    while ((item= it++))
    {
      uint length;
      uchar *pos;
      if (read_info.read_field())
	break;

      /* If this line is to be skipped we don't want to fill field or var */
      if (skip_lines)
        continue;

      pos=read_info.row_start;
      length=(uint) (read_info.row_end-pos);

      Load_data_outvar *dst= item->get_load_data_outvar_or_error();
      DBUG_ASSERT(dst);

      if ((!read_info.enclosed &&
           (enclosed_length && length == 4 &&
            !memcmp(pos, STRING_WITH_LEN("NULL")))) ||
	  (length == 1 && read_info.found_null))
      {
        if (dst->load_data_set_null(thd, &read_info))
          DBUG_RETURN(1);
      }
      else
      {
        read_info.row_end[0]= 0;  // Safe to change end marker
        if (dst->load_data_set_value(thd, (const char *) pos, length, &read_info))
          DBUG_RETURN(1);
      }
    }

    if (unlikely(thd->is_error()))
      read_info.error= 1;
    if (unlikely(read_info.error))
      break;

    if (skip_lines)
    {
      skip_lines--;
      continue;
    }
    if (item)
    {
      /* Have not read any field, thus input file is simply ended */
      if (item == fields_vars.head())
	break;
      for (; item ; item= it++)
      {
        Load_data_outvar *dst= item->get_load_data_outvar_or_error();
        DBUG_ASSERT(dst);
        if (unlikely(dst->load_data_set_no_data(thd, &read_info)))
          DBUG_RETURN(1);
      }
    }

    if (unlikely(thd->killed) ||
        unlikely(fill_record_n_invoke_before_triggers(thd, table, set_fields,
                                                      set_values,
                                                      ignore_check_option_errors,
                                                      TRG_EVENT_INSERT)))
      DBUG_RETURN(1);

    switch (table_list->view_check_option(thd,
                                          ignore_check_option_errors)) {
    case VIEW_CHECK_SKIP:
      read_info.next_line();
      goto continue_loop;
    case VIEW_CHECK_ERROR:
      DBUG_RETURN(-1);
    }

    err= write_record(thd, table, &info);
    table->auto_increment_field_not_null= FALSE;
    if (err)
      DBUG_RETURN(1);
    /*
      We don't need to reset auto-increment field since we are restoring
      its default value at the beginning of each loop iteration.
    */
    if (read_info.next_line())			// Skip to next line
      break;
    if (read_info.line_cuted)
    {
      thd->cuted_fields++;			/* To long row */
      push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                          ER_WARN_TOO_MANY_RECORDS,
                          ER_THD(thd, ER_WARN_TOO_MANY_RECORDS),
                          thd->get_stmt_da()->current_row_for_warning());
      if (thd->killed)
        DBUG_RETURN(1);
    }
    thd->get_stmt_da()->inc_current_row_for_warning();
continue_loop:;
  }
  DBUG_RETURN(MY_TEST(read_info.error));
}


/****************************************************************************
** Read rows in xml format
****************************************************************************/
static int
read_xml_field(THD *thd, COPY_INFO &info, TABLE_LIST *table_list,
               List<Item> &fields_vars, List<Item> &set_fields,
               List<Item> &set_values, READ_INFO &read_info,
               String &row_tag, ulong skip_lines,
               bool ignore_check_option_errors)
{
  List_iterator_fast<Item> it(fields_vars);
  Item *item;
  TABLE *table= table_list->table;
  bool no_trans_update_stmt;
  DBUG_ENTER("read_xml_field");
  
  no_trans_update_stmt= !table->file->has_transactions_and_rollback();
  
  for ( ; ; it.rewind())
  {
    bool err;
    if (thd->killed)
    {
      thd->send_kill_message();
      DBUG_RETURN(1);
    }
    
    // read row tag and save values into tag list
    if (read_info.read_xml(thd))
      break;
    
    List_iterator_fast<XML_TAG> xmlit(read_info.taglist);
    xmlit.rewind();
    XML_TAG *tag= NULL;
    
#ifndef DBUG_OFF
    DBUG_PRINT("read_xml_field", ("skip_lines=%d", (int) skip_lines));
    while ((tag= xmlit++))
    {
      DBUG_PRINT("read_xml_field", ("got tag:%i '%s' '%s'",
                                    tag->level, tag->field.c_ptr(),
                                    tag->value.c_ptr()));
    }
#endif
    
    restore_record(table, s->default_values);
    
    while ((item= it++))
    {
      /* If this line is to be skipped we don't want to fill field or var */
      if (skip_lines)
        continue;
      
      /* find field in tag list */
      xmlit.rewind();
      tag= xmlit++;
      
      while(tag && strcmp(tag->field.c_ptr(), item->name.str) != 0)
        tag= xmlit++;

      Load_data_outvar *dst= item->get_load_data_outvar_or_error();
      DBUG_ASSERT(dst);
      if (!tag ? dst->load_data_set_null(thd, &read_info) :
                 dst->load_data_set_value(thd, tag->value.ptr(),
                                          tag->value.length(),
                                          &read_info))
        DBUG_RETURN(1);
    }
    
    if (unlikely(read_info.error))
      break;
    
    if (skip_lines)
    {
      skip_lines--;
      continue;
    }

    DBUG_ASSERT(!item);

    if (thd->killed ||
        fill_record_n_invoke_before_triggers(thd, table, set_fields, set_values,
                                             ignore_check_option_errors,
                                             TRG_EVENT_INSERT))
      DBUG_RETURN(1);

    switch (table_list->view_check_option(thd,
                                          ignore_check_option_errors)) {
    case VIEW_CHECK_SKIP:
      read_info.next_line();
      goto continue_loop;
    case VIEW_CHECK_ERROR:
      DBUG_RETURN(-1);
    }
    
    err= write_record(thd, table, &info);
    table->auto_increment_field_not_null= false;
    if (err)
      DBUG_RETURN(1);
    
    /*
      We don't need to reset auto-increment field since we are restoring
      its default value at the beginning of each loop iteration.
    */
    thd->transaction->stmt.modified_non_trans_table= no_trans_update_stmt;
    thd->get_stmt_da()->inc_current_row_for_warning();
    continue_loop:;
  }
  DBUG_RETURN(MY_TEST(read_info.error) || thd->is_error());
} /* load xml end */


/* Unescape all escape characters, mark \N as null */

char
READ_INFO::unescape(char chr)
{
  /* keep this switch synchornous with the ESCAPE_CHARS macro */
  switch(chr) {
  case 'n': return '\n';
  case 't': return '\t';
  case 'r': return '\r';
  case 'b': return '\b';
  case '0': return 0;				// Ascii null
  case 'Z': return '\032';			// Win32 end of file
  case 'N': found_null=1;

    /* fall through */
  default:  return chr;
  }
}


/*
  Read a line using buffering
  If last line is empty (in line mode) then it isn't outputed
*/


READ_INFO::READ_INFO(THD *thd, File file_par,
                     const Load_data_param &param,
		     String &field_term, String &line_start, String &line_term,
		     String &enclosed_par, int escape, bool get_it_from_net,
		     bool is_fifo)
  :Load_data_param(param),
   file(file_par),
   m_field_term(field_term), m_line_term(line_term), m_line_start(line_start),
   escape_char(escape), found_end_of_line(false), eof(false),
   error(false), line_cuted(false), found_null(false)
{
  data.set_thread_specific();
  /*
    Field and line terminators must be interpreted as sequence of unsigned char.
    Otherwise, non-ascii terminators will be negative on some platforms,
    and positive on others (depending on the implementation of char).
  */

  level= 0; /* for load xml */
  start_of_line= line_start.length() != 0;
  /* If field_terminator == line_terminator, don't use line_terminator */
  if (m_field_term.eq(m_line_term))
    m_line_term.reset();
  enclosed_char= enclosed_par.length() ? (uchar) enclosed_par[0] : INT_MAX;

  /* Set of a stack for unget if long terminators */
  uint length= MY_MAX(charset()->mbmaxlen, MY_MAX(m_field_term.length(),
                                                  m_line_term.length())) + 1;
  set_if_bigger(length,line_start.length());
  stack= stack_pos= (int*) thd->alloc(sizeof(int) * length);

  DBUG_ASSERT(m_fixed_length < UINT_MAX32);
  if (data.reserve((size_t) m_fixed_length))
    error=1; /* purecov: inspected */
  else
  {
    if (init_io_cache(&cache,(get_it_from_net) ? -1 : file, 0,
		      (get_it_from_net) ? READ_NET :
		      (is_fifo ? READ_FIFO : READ_CACHE),0L,1,
		      MYF(MY_WME | MY_THREAD_SPECIFIC)))
    {
      error=1;
    }
    else
    {
#ifndef EMBEDDED_LIBRARY
      if (get_it_from_net)
	cache.read_function = _my_b_net_read;

      if (mysql_bin_log.is_open())
      {
        cache.real_read_function= cache.read_function;
        cache.read_function= log_loaded_block;
      }
#endif
    }
  }
}


READ_INFO::~READ_INFO()
{
  ::end_io_cache(&cache);
  List_iterator<XML_TAG> xmlit(taglist);
  XML_TAG *t;
  while ((t= xmlit++))
    delete(t);
}


inline bool READ_INFO::terminator(const uchar *ptr, uint length)
{
  int chr=0;					// Keep gcc happy
  uint i;
  for (i=1 ; i < length ; i++)
  {
    if ((chr=GET) != *(uchar*)++ptr)
    {
      break;
    }
  }
  if (i == length)
    return true;
  PUSH(chr);
  while (i-- > 1)
    PUSH(*--ptr);
  return false;
}


/**
  Read a field.

  The data in the loaded file was presumably escaped using
  - either select_export::send_data() OUTFILE
  - or mysql_real_escape_string()
  using the same character set with the one specified in the current
  "LOAD DATA INFILE ... CHARACTER SET ..." (or the default LOAD character set).

  Note, non-escaped multi-byte characters are scanned as a single entity.
  This is needed to correctly distinguish between:
  - 0x5C as an escape character versus
  - 0x5C as the second byte in a multi-byte sequence (big5, cp932, gbk, sjis)

  Parts of escaped multi-byte characters are scanned on different loop
  iterations. See the comment about 0x5C handling in select_export::send_data()
  in sql_class.cc.

  READ_INFO::read_field() does not check wellformedness.
  Raising wellformedness errors or warnings in READ_INFO::read_field()
  would be wrong, as the data after unescaping can go into a BLOB field,
  or into a TEXT/VARCHAR field of a different character set.
  The loop below only makes sure to revert escaping made by
  select_export::send_data() or mysql_real_escape_string().
  Wellformedness is checked later, during Field::store(str,length,cs) time.

  Note, in some cases users can supply data which did not go through
  escaping properly. For example, utf8 "\<C3><A4>"
  (backslash followed by LATIN SMALL LETTER A WITH DIAERESIS)
  is improperly escaped data that could not be generated by
  select_export::send_data() / mysql_real_escape_string():
  - either there should be two backslashes:   "\\<C3><A4>"
  - or there should be no backslashes at all: "<C3><A4>"
  "\<C3>" and "<A4> are scanned on two different loop iterations and
  store "<C3><A4>" into the field.

  Note, adding useless escapes before multi-byte characters like in the
  example above is safe in case of utf8, but is not safe in case of
  character sets that have escape_with_backslash_is_dangerous==TRUE,
  such as big5, cp932, gbk, sjis. This can lead to mis-interpretation of the
  data. Suppose we have a big5 character "<EE><5C>" followed by <30> (digit 0).
  If we add an extra escape before this sequence, then we'll get
  <5C><EE><5C><30>. The first loop iteration will turn <5C><EE> into <EE>.
  The second loop iteration will turn <5C><30> into <30>.
  So the program that generates a dump file for further use with LOAD DATA
  must make sure to use escapes properly.
*/

int READ_INFO::read_field()
{
  int chr,found_enclosed_char;

  found_null=0;
  if (found_end_of_line)
    return 1;					// One have to call next_line

  /* Skip until we find 'line_start' */

  if (start_of_line)
  {						// Skip until line_start
    start_of_line=0;
    if (find_start_of_fields())
      return 1;
  }
  if ((chr=GET) == my_b_EOF)
  {
    found_end_of_line=eof=1;
    return 1;
  }
  data.length(0);
  if (chr == enclosed_char)
  {
    found_enclosed_char=enclosed_char;
    data.append(chr);                            // If error
  }
  else
  {
    found_enclosed_char= INT_MAX;
    PUSH(chr);
  }

  for (;;)
  {
    // Make sure we have enough space for the longest multi-byte character.
    while (data.length() + charset()->mbmaxlen <= data.alloced_length())
    {
      chr = GET;
      if (chr == my_b_EOF)
	goto found_eof;
      if (chr == escape_char)
      {
	if ((chr=GET) == my_b_EOF)
	{
	  data.append(escape_char);
	  goto found_eof;
	}
        /*
          When escape_char == enclosed_char, we treat it like we do for
          handling quotes in SQL parsing -- you can double-up the
          escape_char to include it literally, but it doesn't do escapes
          like \n. This allows: LOAD DATA ... ENCLOSED BY '"' ESCAPED BY '"'
          with data like: "fie""ld1", "field2"
         */
        if (escape_char != enclosed_char || chr == escape_char)
        {
          data.append(unescape((char) chr));
          continue;
        }
        PUSH(chr);
        chr= escape_char;
      }
#ifdef ALLOW_LINESEPARATOR_IN_STRINGS
      if (chr == m_line_term.initial_byte())
#else
      if (chr == m_line_term.initial_byte() && found_enclosed_char == INT_MAX)
#endif
      {
	if (terminator(m_line_term))
	{					// Maybe unexpected linefeed
	  enclosed=0;
	  found_end_of_line=1;
	  row_start= (uchar *) data.ptr();
	  row_end= (uchar *) data.end();
	  return 0;
	}
      }
      if (chr == found_enclosed_char)
      {
	if ((chr=GET) == found_enclosed_char)
	{					// Remove dupplicated
	  data.append(chr);
	  continue;
	}
	// End of enclosed field if followed by field_term or line_term
	if (chr == my_b_EOF || terminator(chr, m_line_term))
        {
          /* Maybe unexpected linefeed */
	  enclosed=1;
	  found_end_of_line=1;
	  row_start= (uchar *) data.ptr() + 1;
	  row_end=  (uchar *) data.end();
	  return 0;
	}
	if (terminator(chr, m_field_term))
	{
	  enclosed=1;
	  row_start= (uchar *) data.ptr() + 1;
	  row_end=  (uchar *) data.end();
	  return 0;
	}
	/*
	  The string didn't terminate yet.
	  Store back next character for the loop
	*/
	PUSH(chr);
	/* copy the found term character to 'to' */
	chr= found_enclosed_char;
      }
      else if (chr == m_field_term.initial_byte() &&
               found_enclosed_char == INT_MAX)
      {
	if (terminator(m_field_term))
	{
	  enclosed=0;
	  row_start= (uchar *) data.ptr();
	  row_end= (uchar *) data.end();
	  return 0;
	}
      }
      data.append(chr);
      if (charset()->use_mb() && read_mbtail(&data))
        goto found_eof;
    }
    /*
    ** We come here if buffer is too small. Enlarge it and continue
    */
    if (data.reserve(IO_SIZE))
      return (error= 1);
  }

found_eof:
  enclosed=0;
  found_end_of_line=eof=1;
  row_start= (uchar *) data.ptr();
  row_end= (uchar *) data.end();
  return 0;
}

/*
  Read a row with fixed length.

  NOTES
    The row may not be fixed size on disk if there are escape
    characters in the file.

  IMPLEMENTATION NOTE
    One can't use fixed length with multi-byte charset **

  RETURN
    0  ok
    1  error
*/

int READ_INFO::read_fixed_length()
{
  int chr;
  if (found_end_of_line)
    return 1;					// One have to call next_line

  if (start_of_line)
  {						// Skip until line_start
    start_of_line=0;
    if (find_start_of_fields())
      return 1;
  }

  for (data.length(0); data.length() < m_fixed_length ; )
  {
    if ((chr=GET) == my_b_EOF)
      goto found_eof;
    if (chr == escape_char)
    {
      if ((chr=GET) == my_b_EOF)
      {
	data.append(escape_char);
	goto found_eof;
      }
      data.append((uchar) unescape((char) chr));
      continue;
    }
    if (terminator(chr, m_line_term))
    {						// Maybe unexpected linefeed
      found_end_of_line= true;
      break;
    }
    data.append(chr);
  }
  row_start= (uchar *) data.ptr();
  row_end= (uchar *) data.end();			// Found full line
  return 0;

found_eof:
  found_end_of_line=eof=1;
  row_start= (uchar *) data.ptr();
  row_end= (uchar *) data.end();
  return data.length() == 0 ? 1 : 0;
}


int READ_INFO::next_line()
{
  line_cuted=0;
  start_of_line= m_line_start.length() != 0;
  if (found_end_of_line || eof)
  {
    found_end_of_line=0;
    return eof;
  }
  found_end_of_line=0;
  if (!m_line_term.length())
    return 0;					// No lines
  for (;;)
  {
    int chlen;
    char buf[MY_CS_MBMAXLEN];

    if (getbyte(&buf[0]))
      return 1; // EOF

    if (charset()->use_mb() &&
        (chlen= charset()->charlen(buf, buf + 1)) != 1)
    {
      uint i;
      for (i= 1; MY_CS_IS_TOOSMALL(chlen); )
      {
        DBUG_ASSERT(i < sizeof(buf));
        DBUG_ASSERT(chlen != 1);
        if (getbyte(&buf[i++]))
          return 1; // EOF
        chlen= charset()->charlen(buf, buf + i);
      }

      /*
        Either a complete multi-byte sequence,
        or a broken byte sequence was found.
        Check if the sequence is a prefix of the "LINES TERMINATED BY" string.
      */
      if ((uchar) buf[0] == m_line_term.initial_byte() &&
          i <= m_line_term.length() &&
          !memcmp(buf, m_line_term.ptr(), i))
      {
        if (m_line_term.length() == i)
        {
          /*
            We found a "LINES TERMINATED BY" string that consists
            of a single multi-byte character.
          */
          return 0;
        }
        /*
          buf[] is a prefix of "LINES TERMINATED BY".
          Now check the suffix. Length of the suffix of line_term_ptr
          that still needs to be checked is (line_term_length - i).
          Note, READ_INFO::terminator() assumes that the leftmost byte of the
          argument is already scanned from the file and is checked to
          be a known prefix (e.g. against line_term.initial_char()).
          So we need to pass one extra byte.
        */
        if (terminator(m_line_term.ptr() + i - 1,
                       m_line_term.length() - i + 1))
          return 0;
      }
      /*
        Here we have a good multi-byte sequence or a broken byte sequence,
        and the sequence is not equal to "LINES TERMINATED BY".
        No needs to check for escape_char, because:
        - multi-byte escape characters in "FIELDS ESCAPED BY" are not
          supported and are rejected at parse time.
        - broken single-byte sequences are not recognized as escapes,
          they are considered to be a part of the data and are converted to
          question marks.
      */
      line_cuted= true;
      continue;
    }
    if (buf[0] == escape_char)
    {
      line_cuted= true;
      if (GET == my_b_EOF)
        return 1;
      continue;
    }
    if (terminator(buf[0], m_line_term))
      return 0;
    line_cuted= true;
  }
}


bool READ_INFO::find_start_of_fields()
{
  for (int chr= GET ; chr != my_b_EOF ; chr= GET)
  {
    if (terminator(chr, m_line_start))
      return false;
  }
  return (found_end_of_line= eof= true);
}


/*
  Clear taglist from tags with a specified level
*/
int READ_INFO::clear_level(int level_arg)
{
  DBUG_ENTER("READ_INFO::read_xml clear_level");
  List_iterator<XML_TAG> xmlit(taglist);
  xmlit.rewind();
  XML_TAG *tag;
  
  while ((tag= xmlit++))
  {
     if(tag->level >= level_arg)
     {
       xmlit.remove();
       delete tag;
     }
  }
  DBUG_RETURN(0);
}


/*
  Convert an XML entity to Unicode value.
  Return -1 on error;
*/
static int
my_xml_entity_to_char(const char *name, uint length)
{
  if (length == 2)
  {
    if (!memcmp(name, "gt", length))
      return '>';
    if (!memcmp(name, "lt", length))
      return '<';
  }
  else if (length == 3)
  {
    if (!memcmp(name, "amp", length))
      return '&';
  }
  else if (length == 4)
  {
    if (!memcmp(name, "quot", length))
      return '"';
    if (!memcmp(name, "apos", length))
      return '\'';
  }
  return -1;
}


/**
  @brief Convert newline, linefeed, tab to space
  
  @param chr    character
  
  @details According to the "XML 1.0" standard,
           only space (#x20) characters, carriage returns,
           line feeds or tabs are considered as spaces.
           Convert all of them to space (#x20) for parsing simplicity.
*/
static int
my_tospace(int chr)
{
  return (chr == '\t' || chr == '\r' || chr == '\n') ? ' ' : chr;
}


/*
  Read an xml value: handle multibyte and xml escape
*/
int READ_INFO::read_value(int delim, String *val)
{
  int chr;
  String tmp;

  for (chr= GET; my_tospace(chr) != delim && chr != my_b_EOF; chr= GET)
  {
    if(chr == '&')
    {
      tmp.length(0);
      for (chr= my_tospace(GET) ; chr != ';' ; chr= my_tospace(GET))
      {
        if (chr == my_b_EOF)
          return chr;
        tmp.append(chr);
      }
      if ((chr= my_xml_entity_to_char(tmp.ptr(), tmp.length())) >= 0)
        val->append(chr);
      else
      {
        val->append('&');
        val->append(tmp);
        val->append(';'); 
      }
    }
    else
    {
      val->append(chr);
      if (charset()->use_mb() && read_mbtail(val))
        return my_b_EOF;
    }
  }            
  return my_tospace(chr);
}


/*
  Read a record in xml format
  tags and attributes are stored in taglist
  when tag set in ROWS IDENTIFIED BY is closed, we are ready and return
*/
int READ_INFO::read_xml(THD *thd)
{
  DBUG_ENTER("READ_INFO::read_xml");
  int chr, chr2, chr3;
  int delim= 0;
  String tag, attribute, value;
  bool in_tag= false;
  
  tag.length(0);
  attribute.length(0);
  value.length(0);
  
  for (chr= my_tospace(GET); chr != my_b_EOF ; )
  {
    switch(chr){
    case '<':  /* read tag */
        /* TODO: check if this is a comment <!-- comment -->  */
      chr= my_tospace(GET);
      if(chr == '!')
      {
        chr2= GET;
        chr3= GET;
        
        if(chr2 == '-' && chr3 == '-')
        {
          chr2= 0;
          chr3= 0;
          chr= my_tospace(GET);
          
          while(chr != '>' || chr2 != '-' || chr3 != '-')
          {
            if(chr == '-')
            {
              chr3= chr2;
              chr2= chr;
            }
            else if (chr2 == '-')
            {
              chr2= 0;
              chr3= 0;
            }
            chr= my_tospace(GET);
            if (chr == my_b_EOF)
              goto found_eof;
          }
          break;
        }
      }
      
      tag.length(0);
      while(chr != '>' && chr != ' ' && chr != '/' && chr != my_b_EOF)
      {
        if(chr != delim) /* fix for the '<field name =' format */
          tag.append(chr);
        chr= my_tospace(GET);
      }
      
      // row tag should be in ROWS IDENTIFIED BY '<row>' - stored in line_term 
      if((tag.length() == m_line_term.length() - 2) &&
         (memcmp(tag.ptr(), m_line_term.ptr() + 1, tag.length()) == 0))
      {
        DBUG_PRINT("read_xml", ("start-of-row: %i %s %s", 
                                level,tag.c_ptr_safe(), m_line_term.ptr()));
      }
      
      if(chr == ' ' || chr == '>')
      {
        level++;
        clear_level(level + 1);
      }
      
      if (chr == ' ')
        in_tag= true;
      else 
        in_tag= false;
      break;
      
    case ' ': /* read attribute */
      while(chr == ' ')  /* skip blanks */
        chr= my_tospace(GET);
      
      if(!in_tag)
        break;
      
      while(chr != '=' && chr != '/' && chr != '>' && chr != my_b_EOF)
      {
        attribute.append(chr);
        chr= my_tospace(GET);
      }
      break;
      
    case '>': /* end tag - read tag value */
      in_tag= false;
      chr= read_value('<', &value);
      if(chr == my_b_EOF)
        goto found_eof;
      
      /* save value to list */
      if (tag.length() > 0 && value.length() > 0)
      {
        DBUG_PRINT("read_xml", ("lev:%i tag:%s val:%s",
                                level,tag.c_ptr_safe(), value.c_ptr_safe()));
        XML_TAG *tmp= new XML_TAG(level, tag, value);
        if (!tmp || taglist.push_front(tmp, thd->mem_root))
          DBUG_RETURN(1);                       // End of memory
      }
      tag.length(0);
      value.length(0);
      attribute.length(0);
      break;
      
    case '/': /* close tag */
      chr= my_tospace(GET);
      /* Decrease the 'level' only when (i) It's not an */
      /* (without space) empty tag i.e. <tag/> or, (ii) */
      /* It is of format <row col="val" .../>           */
      if(chr != '>' || in_tag)
      {
        level--;
        in_tag= false;
      }
      if(chr != '>')   /* if this is an empty tag <tag   /> */
        tag.length(0); /* we should keep tag value          */
      while(chr != '>' && chr != my_b_EOF)
      {
        tag.append(chr);
        chr= my_tospace(GET);
      }
      
      if((tag.length() == m_line_term.length() - 2) &&
         (memcmp(tag.ptr(), m_line_term.ptr() + 1, tag.length()) == 0))
      {
         DBUG_PRINT("read_xml", ("found end-of-row %i %s", 
                                 level, tag.c_ptr_safe()));
         DBUG_RETURN(0); //normal return
      }
      chr= my_tospace(GET);
      break;   
      
    case '=': /* attribute name end - read the value */
      //check for tag field and attribute name
      if(!strcmp(tag.c_ptr_safe(), "field") &&
         !strcmp(attribute.c_ptr_safe(), "name"))
      {
        /*
          this is format <field name="xx">xx</field>
          where actual fieldname is in attribute
        */
        delim= my_tospace(GET);
        tag.length(0);
        attribute.length(0);
        chr= '<'; /* we pretend that it is a tag */
        level--;
        break;
      }
      
      //check for " or '
      chr= GET;
      if (chr == my_b_EOF)
        goto found_eof;
      if(chr == '"' || chr == '\'')
      {
        delim= chr;
      }
      else
      {
        delim= ' '; /* no delimiter, use space */
        PUSH(chr);
      }
      
      chr= read_value(delim, &value);
      if (attribute.length() > 0 && value.length() > 0)
      {
        DBUG_PRINT("read_xml", ("lev:%i att:%s val:%s",
                                level + 1,
                                attribute.c_ptr_safe(),
                                value.c_ptr_safe()));
        XML_TAG *tmp= new XML_TAG(level + 1, attribute, value);
        if (!tmp || taglist.push_front(tmp, thd->mem_root))
          DBUG_RETURN(1);                       // End of memory
      }
      attribute.length(0);
      value.length(0);
      if (chr != ' ')
        chr= my_tospace(GET);
      break;
    
    default:
      chr= my_tospace(GET);
    } /* end switch */
  } /* end while */
  
found_eof:
  DBUG_PRINT("read_xml",("Found eof"));
  eof= 1;
  DBUG_RETURN(1);
}
