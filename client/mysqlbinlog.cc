/*
   Copyright (c) 2000, 2014, Oracle and/or its affiliates.
   Copyright (c) 2009, 2014, MariaDB

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

/* 

   TODO: print the catalog (some USE catalog.db ????).

   Standalone program to read a MySQL binary log (or relay log).

   Should be able to read any file of these categories, even with
   --start-position.
   An important fact: the Format_desc event of the log is at most the 3rd event
   of the log; if it is the 3rd then there is this combination:
   Format_desc_of_slave, Rotate_of_master, Format_desc_of_master.
*/

#define MYSQL_CLIENT
#undef MYSQL_SERVER
#define TABLE TABLE_CLIENT
/* This hack is here to avoid adding COMPRESSED data types to libmariadb. */
#define MYSQL_TYPE_TIME2 MYSQL_TYPE_TIME2,MYSQL_TYPE_BLOB_COMPRESSED=140,MYSQL_TYPE_VARCHAR_COMPRESSED=141
#include "client_priv.h"
#undef MYSQL_TYPE_TIME2
#include <my_time.h>
#include <sslopt-vars.h>
/* That one is necessary for defines of OPTION_NO_FOREIGN_KEY_CHECKS etc */
#include "sql_priv.h"
#include "sql_basic_types.h"
#include "log_event.h"
#include "compat56.h"
#include "sql_common.h"
#include "my_dir.h"
#include <welcome_copyright_notice.h> // ORACLE_WELCOME_COPYRIGHT_NOTICE
#include "sql_string.h"   // needed for Rpl_filter
#include "sql_list.h"     // needed for Rpl_filter
#include "rpl_filter.h"

#include "mysqld.h"

#include <algorithm>

#define my_net_write ma_net_write
#define net_flush ma_net_flush
#define cli_safe_read mysql_net_read_packet
#define my_net_read ma_net_read
extern "C" unsigned char *mysql_net_store_length(unsigned char *packet, size_t length);
#define net_store_length mysql_net_store_length

Rpl_filter *binlog_filter= 0;

#define BIN_LOG_HEADER_SIZE	4
#define PROBE_HEADER_LEN	(EVENT_LEN_OFFSET+4)

/* Needed for Rpl_filter */
CHARSET_INFO* system_charset_info= &my_charset_utf8_general_ci;

/* Needed for Flashback */
DYNAMIC_ARRAY binlog_events; // Storing the events output string
DYNAMIC_ARRAY events_in_stmt; // Storing the events that in one statement
String stop_event_string; // Storing the STOP_EVENT output string

char server_version[SERVER_VERSION_LENGTH];
ulong server_id = 0;

// needed by net_serv.c
ulong bytes_sent = 0L, bytes_received = 0L;
ulong mysqld_net_retry_count = 10L;
ulong open_files_limit;
ulong opt_binlog_rows_event_max_size;
ulonglong test_flags = 0;
ulong opt_binlog_rows_event_max_encoded_size= MAX_MAX_ALLOWED_PACKET;
static uint opt_protocol= 0;
static FILE *result_file;
static char *result_file_name= 0;
static const char *output_prefix= "";

#ifndef DBUG_OFF
static const char *default_dbug_option = "d:t:o,/tmp/mysqlbinlog.trace";
const char *current_dbug_option= default_dbug_option;
#endif
static const char *load_groups[]=
{ "mysqlbinlog", "client", "client-server", "client-mariadb", 0 };

static void error(const char *format, ...) ATTRIBUTE_FORMAT(printf, 1, 2);
static void warning(const char *format, ...) ATTRIBUTE_FORMAT(printf, 1, 2);

static bool one_database=0, one_table=0, to_last_remote_log= 0, disable_log_bin= 0;
static bool opt_hexdump= 0, opt_version= 0;
const char *base64_output_mode_names[]=
{"NEVER", "AUTO", "ALWAYS", "UNSPEC", "DECODE-ROWS", NullS};
TYPELIB base64_output_mode_typelib=
  { array_elements(base64_output_mode_names) - 1, "",
    base64_output_mode_names, NULL };
static enum_base64_output_mode opt_base64_output_mode= BASE64_OUTPUT_UNSPEC;
static char *opt_base64_output_mode_str= NullS;
static char* database= 0;
static char* table= 0;
static my_bool force_opt= 0, short_form= 0, remote_opt= 0;
static my_bool print_row_count= 0, print_row_event_positions= 0;
static my_bool print_row_count_used= 0, print_row_event_positions_used= 0;
static my_bool debug_info_flag, debug_check_flag;
static my_bool force_if_open_opt= 1;
static my_bool opt_raw_mode= 0, opt_stop_never= 0;
static ulong opt_stop_never_slave_server_id= 0;
static my_bool opt_verify_binlog_checksum= 1;
static ulonglong offset = 0;
static char* host = 0;
static int port= 0;
static uint my_end_arg;
static const char* sock= 0;
static char *opt_plugindir= 0, *opt_default_auth= 0;

static char* user = 0;
static char* pass = 0;
static char *charset= 0;

static uint verbose= 0;

static ulonglong start_position, stop_position;
#define start_position_mot ((my_off_t)start_position)
#define stop_position_mot  ((my_off_t)stop_position)

static char *start_datetime_str, *stop_datetime_str;
static my_time_t start_datetime= 0, stop_datetime= MY_TIME_T_MAX;
static ulonglong rec_count= 0;
static MYSQL* mysql = NULL;
static const char* dirname_for_local_load= 0;
static bool opt_skip_annotate_row_events= 0;

static my_bool opt_flashback;
#ifdef WHEN_FLASHBACK_REVIEW_READY
static my_bool opt_flashback_review;
static char *flashback_review_dbname, *flashback_review_tablename;
#endif

/**
  Pointer to the Format_description_log_event of the currently active binlog.

  This will be changed each time a new Format_description_log_event is
  found in the binlog. It is finally destroyed at program termination.
*/
static Format_description_log_event* glob_description_event= NULL;

/**
  Exit status for functions in this file.
*/
enum Exit_status {
  /** No error occurred and execution should continue. */
  OK_CONTINUE= 0,
  /** An error occurred and execution should stop. */
  ERROR_STOP,
  /** No error occurred but execution should stop. */
  OK_STOP,
  /** No error occurred - end of file reached. */
  OK_EOF,
};

/**
  Pointer to the last read Annotate_rows_log_event. Having read an
  Annotate_rows event, we should not print it immediatedly because all
  subsequent rbr events can be filtered away, and have to keep it for a while.
  Also because of that when reading a remote Annotate event we have to keep
  its binary log representation in a separately allocated buffer.
*/
static Annotate_rows_log_event *annotate_event= NULL;

void free_annotate_event()
{
  if (annotate_event)
  {
    delete annotate_event;
    annotate_event= 0;
  }
}

Log_event* read_remote_annotate_event(uchar* net_buf, ulong event_len,
                                      const char **error_msg)
{
  uchar *event_buf;
  Log_event* event;

  if (!(event_buf= (uchar*) my_malloc(event_len + 1, MYF(MY_WME))))
  {
    error("Out of memory");
    return 0;
  }

  memcpy(event_buf, net_buf, event_len);
  event_buf[event_len]= 0;

  if (!(event= Log_event::read_log_event((const char*) event_buf, event_len,
                                         error_msg, glob_description_event,
                                         opt_verify_binlog_checksum)))
  {
    my_free(event_buf);
    return 0;
  }
  /*
    Ensure the event->temp_buf is pointing to the allocated buffer.
    (TRUE = free temp_buf on the event deletion)
  */
  event->register_temp_buf((char*)event_buf, TRUE);

  return event;
}

void keep_annotate_event(Annotate_rows_log_event* event)
{
  free_annotate_event();
  annotate_event= event;
}

bool print_annotate_event(PRINT_EVENT_INFO *print_event_info)
{
  bool error= 0;
  if (annotate_event)
  {
    error= annotate_event->print(result_file, print_event_info);
    delete annotate_event;  // the event should not be printed more than once
    annotate_event= 0;
  }
  return error;
}

static Exit_status dump_local_log_entries(PRINT_EVENT_INFO *, const char*);
static Exit_status dump_remote_log_entries(PRINT_EVENT_INFO *, const char*);
static Exit_status dump_log_entries(const char* logname);
static Exit_status safe_connect();


class Load_log_processor
{
  char target_dir_name[FN_REFLEN];
  size_t target_dir_name_len;

  /*
    When we see first event corresponding to some LOAD DATA statement in
    binlog, we create temporary file to store data to be loaded.
    We add name of this file to file_names array using its file_id as index.
    If we have Create_file event (i.e. we have binary log in pre-5.0.3
    format) we also store save event object to be able which is needed to
    emit LOAD DATA statement when we will meet Exec_load_data event.
    If we have Begin_load_query event we simply store 0 in
    File_name_record::event field.
  */
  struct File_name_record
  {
    char *fname;
    Create_file_log_event *event;
  };
  /*
    @todo Should be a map (e.g., a hash map), not an array.  With the
    present implementation, the number of elements in this array is
    about the number of files loaded since the server started, which
    may be big after a few years.  We should be able to use existing
    library data structures for this. /Sven
  */
  DYNAMIC_ARRAY file_names;

  /**
    Looks for a non-existing filename by adding a numerical suffix to
    the given base name, creates the generated file, and returns the
    filename by modifying the filename argument.

    @param[in,out] filename Base filename

    @param[in,out] file_name_end Pointer to last character of
    filename.  The numerical suffix will be written to this position.
    Note that there must be a least five bytes of allocated memory
    after file_name_end.

    @retval -1 Error (can't find new filename).
    @retval >=0 Found file.
  */
  File create_unique_file(char *filename, char *file_name_end)
    {
      File res;
      /* If we have to try more than 1000 times, something is seriously wrong */
      for (uint version= 0; version<1000; version++)
      {
	sprintf(file_name_end,"-%x",version);
	if ((res= my_create(filename,0,
			    O_CREAT|O_EXCL|O_BINARY|O_WRONLY,MYF(0)))!=-1)
	  return res;
      }
      return -1;
    }

public:
  Load_log_processor() {}
  ~Load_log_processor() {}

  int init()
  {
    return my_init_dynamic_array(&file_names, sizeof(File_name_record),
                                 100, 100, MYF(0));
  }

  void init_by_dir_name(const char *dir)
    {
      target_dir_name_len= (convert_dirname(target_dir_name, dir, NullS) -
			    target_dir_name);
    }
  void init_by_cur_dir()
    {
      if (my_getwd(target_dir_name,sizeof(target_dir_name),MYF(MY_WME)))
	exit(1);
      target_dir_name_len= strlen(target_dir_name);
    }
  void destroy()
  {
    File_name_record *ptr= (File_name_record *)file_names.buffer;
    File_name_record *end= ptr + file_names.elements;
    for (; ptr < end; ptr++)
    {
      if (ptr->fname)
      {
        my_free(ptr->fname);
        delete ptr->event;
        bzero((char *)ptr, sizeof(File_name_record));
      }
    }

    delete_dynamic(&file_names);
  }

  /**
    Obtain Create_file event for LOAD DATA statement by its file_id
    and remove it from this Load_log_processor's list of events.

    Checks whether we have already seen a Create_file_log_event with
    the given file_id.  If yes, returns a pointer to the event and
    removes the event from array describing active temporary files.
    From this moment, the caller is responsible for freeing the memory
    occupied by the event.

    @param[in] file_id File id identifying LOAD DATA statement.

    @return Pointer to Create_file_log_event, or NULL if we have not
    seen any Create_file_log_event with this file_id.
  */
  Create_file_log_event *grab_event(uint file_id)
    {
      File_name_record *ptr;
      Create_file_log_event *res;

      if (file_id >= file_names.elements)
        return 0;
      ptr= dynamic_element(&file_names, file_id, File_name_record*);
      if ((res= ptr->event))
        bzero((char *)ptr, sizeof(File_name_record));
      return res;
    }

  /**
    Obtain file name of temporary file for LOAD DATA statement by its
    file_id and remove it from this Load_log_processor's list of events.

    @param[in] file_id Identifier for the LOAD DATA statement.

    Checks whether we have already seen Begin_load_query event for
    this file_id. If yes, returns the file name of the corresponding
    temporary file and removes the filename from the array of active
    temporary files.  From this moment, the caller is responsible for
    freeing the memory occupied by this name.

    @return String with the name of the temporary file, or NULL if we
    have not seen any Begin_load_query_event with this file_id.
  */
  char *grab_fname(uint file_id)
    {
      File_name_record *ptr;
      char *res= 0;

      if (file_id >= file_names.elements)
        return 0;
      ptr= dynamic_element(&file_names, file_id, File_name_record*);
      if (!ptr->event)
      {
        res= ptr->fname;
        bzero((char *)ptr, sizeof(File_name_record));
      }
      return res;
    }
  Exit_status process(Create_file_log_event *ce);
  Exit_status process(Begin_load_query_log_event *ce);
  Exit_status process(Append_block_log_event *ae);
  File prepare_new_file_for_old_format(Load_log_event *le, char *filename);
  Exit_status load_old_format_file(NET* net, const char *server_fname,
                                   uint server_fname_len, File file);
  Exit_status process_first_event(const char *bname, size_t blen,
                                  const uchar *block,
                                  size_t block_len, uint file_id,
                                  Create_file_log_event *ce);
};


/**
  Creates and opens a new temporary file in the directory specified by previous call to init_by_dir_name() or init_by_cur_dir().

  @param[in] le The basename of the created file will start with the
  basename of the file pointed to by this Load_log_event.

  @param[out] filename Buffer to save the filename in.

  @return File handle >= 0 on success, -1 on error.
*/
File Load_log_processor::prepare_new_file_for_old_format(Load_log_event *le,
							 char *filename)
{
  size_t len;
  char *tail;
  File file;
  
  fn_format(filename, le->fname, target_dir_name, "", MY_REPLACE_DIR);
  len= strlen(filename);
  tail= filename + len;
  
  if ((file= create_unique_file(filename,tail)) < 0)
  {
    error("Could not construct local filename %s.",filename);
    return -1;
  }
  
  le->set_fname_outside_temp_buf(filename,len+strlen(tail));
  
  return file;
}


/**
  Reads a file from a server and saves it locally.

  @param[in,out] net The server to read from.

  @param[in] server_fname The name of the file that the server should
  read.

  @param[in] server_fname_len The length of server_fname.

  @param[in,out] file The file to write to.

  @retval ERROR_STOP An error occurred - the program should terminate.
  @retval OK_CONTINUE No error, the program should continue.
*/
Exit_status Load_log_processor::load_old_format_file(NET* net,
                                                     const char*server_fname,
                                                     uint server_fname_len,
                                                     File file)
{
  uchar buf[FN_REFLEN+1];
  buf[0] = 0;
  memcpy(buf + 1, server_fname, server_fname_len + 1);
  if (my_net_write(net, buf, server_fname_len +2) || net_flush(net))
  {
    error("Failed requesting the remote dump of %s.", server_fname);
    return ERROR_STOP;
  }
  
  for (;;)
  {
    ulong packet_len = my_net_read(net);
    if (packet_len == 0)
    {
      if (my_net_write(net, (uchar*) "", 0) || net_flush(net))
      {
        error("Failed sending the ack packet.");
        return ERROR_STOP;
      }
      /*
	we just need to send something, as the server will read but
	not examine the packet - this is because mysql_load() sends 
	an OK when it is done
      */
      break;
    }
    else if (packet_len == packet_error)
    {
      error("Failed reading a packet during the dump of %s.", server_fname);
      return ERROR_STOP;
    }
    
    if (packet_len > UINT_MAX)
    {
      error("Illegal length of packet read from net.");
      return ERROR_STOP;
    }
    if (my_write(file, (uchar*) net->read_pos, 
		 (uint) packet_len, MYF(MY_WME|MY_NABP)))
      return ERROR_STOP;
  }
  
  return OK_CONTINUE;
}


/**
  Process the first event in the sequence of events representing a
  LOAD DATA statement.

  Creates a temporary file to be used in LOAD DATA and writes first
  block of data to it. Registers its file name (and optional
  Create_file event) in the array of active temporary files.

  @param bname Base name for temporary file to be created.
  @param blen Base name length.
  @param block First block of data to be loaded.
  @param block_len First block length.
  @param file_id Identifies the LOAD DATA statement.
  @param ce Pointer to Create_file event object if we are processing
  this type of event.

  @retval ERROR_STOP An error occurred - the program should terminate.
  @retval OK_CONTINUE No error, the program should continue.
*/
Exit_status Load_log_processor::process_first_event(const char *bname,
                                                    size_t blen,
                                                    const uchar *block,
                                                    size_t block_len,
                                                    uint file_id,
                                                    Create_file_log_event *ce)
{
  size_t full_len= target_dir_name_len + blen + 9 + 9 + 1;
  Exit_status retval= OK_CONTINUE;
  char *fname, *ptr;
  File file;
  File_name_record rec;
  DBUG_ENTER("Load_log_processor::process_first_event");

  if (!(fname= (char*) my_malloc(full_len,MYF(MY_WME))))
  {
    error("Out of memory.");
    delete ce;
    DBUG_RETURN(ERROR_STOP);
  }

  memcpy(fname, target_dir_name, target_dir_name_len);
  ptr= fname + target_dir_name_len;
  memcpy(ptr,bname,blen);
  ptr+= blen;
  ptr+= sprintf(ptr, "-%x", file_id);

  if ((file= create_unique_file(fname,ptr)) < 0)
  {
    error("Could not construct local filename %s%s.",
          target_dir_name,bname);
    my_free(fname);
    delete ce;
    DBUG_RETURN(ERROR_STOP);
  }

  rec.fname= fname;
  rec.event= ce;

  /*
     fname is freed in process_event()
     after Execute_load_query_log_event or Execute_load_log_event
     will have been processed, otherwise in Load_log_processor::destroy()
  */
  if (set_dynamic(&file_names, (uchar*)&rec, file_id))
  {
    error("Out of memory.");
    my_free(fname);
    delete ce;
    DBUG_RETURN(ERROR_STOP);
  }

  if (ce)
    ce->set_fname_outside_temp_buf(fname, strlen(fname));

  if (my_write(file, (uchar*)block, block_len, MYF(MY_WME|MY_NABP)))
  {
    error("Failed writing to file.");
    retval= ERROR_STOP;
  }
  if (my_close(file, MYF(MY_WME)))
  {
    error("Failed closing file.");
    retval= ERROR_STOP;
  }
  DBUG_RETURN(retval);
}


/**
  Process the given Create_file_log_event.

  @see Load_log_processor::process_first_event(const char*,uint,const char*,uint,uint,Create_file_log_event*)

  @param ce Create_file_log_event to process.

  @retval ERROR_STOP An error occurred - the program should terminate.
  @retval OK_CONTINUE No error, the program should continue.
*/
Exit_status  Load_log_processor::process(Create_file_log_event *ce)
{
  const char *bname= ce->fname + dirname_length(ce->fname);
  size_t blen= ce->fname_len - (bname-ce->fname);

  return process_first_event(bname, blen, ce->block, ce->block_len,
                             ce->file_id, ce);
}


/**
  Process the given Begin_load_query_log_event.

  @see Load_log_processor::process_first_event(const char*,uint,const char*,uint,uint,Create_file_log_event*)

  @param ce Begin_load_query_log_event to process.

  @retval ERROR_STOP An error occurred - the program should terminate.
  @retval OK_CONTINUE No error, the program should continue.
*/
Exit_status Load_log_processor::process(Begin_load_query_log_event *blqe)
{
  return process_first_event("SQL_LOAD_MB", 11, blqe->block, blqe->block_len,
                             blqe->file_id, 0);
}


/**
  Process the given Append_block_log_event.

  Appends the chunk of the file contents specified by the event to the
  file created by a previous Begin_load_query_log_event or
  Create_file_log_event.

  If the file_id for the event does not correspond to any file
  previously registered through a Begin_load_query_log_event or
  Create_file_log_event, this member function will print a warning and
  return OK_CONTINUE.  It is safe to return OK_CONTINUE, because no
  query will be written for this event.  We should not print an error
  and fail, since the missing file_id could be because a (valid)
  --start-position has been specified after the Begin/Create event but
  before this Append event.

  @param ae Append_block_log_event to process.

  @retval ERROR_STOP An error occurred - the program should terminate.

  @retval OK_CONTINUE No error, the program should continue.
*/
Exit_status Load_log_processor::process(Append_block_log_event *ae)
{
  DBUG_ENTER("Load_log_processor::process");
  const char* fname= ((ae->file_id < file_names.elements) ?
                       dynamic_element(&file_names, ae->file_id,
                                       File_name_record*)->fname : 0);

  if (fname)
  {
    File file;
    Exit_status retval= OK_CONTINUE;
    if (((file= my_open(fname,
			O_APPEND|O_BINARY|O_WRONLY,MYF(MY_WME))) < 0))
    {
      error("Failed opening file %s", fname);
      DBUG_RETURN(ERROR_STOP);
    }
    if (my_write(file,(uchar*)ae->block,ae->block_len,MYF(MY_WME|MY_NABP)))
    {
      error("Failed writing to file %s", fname);
      retval= ERROR_STOP;
    }
    if (my_close(file,MYF(MY_WME)))
    {
      error("Failed closing file %s", fname);
      retval= ERROR_STOP;
    }
    DBUG_RETURN(retval);
  }

  /*
    There is no Create_file event (a bad binlog or a big
    --start-position). Assuming it's a big --start-position, we just do
    nothing and print a warning.
  */
  warning("Ignoring Append_block as there is no "
          "Create_file event for file_id: %u", ae->file_id);
  DBUG_RETURN(OK_CONTINUE);
}


static Load_log_processor load_processor;


/**
  Replace windows-style backslashes by forward slashes so it can be
  consumed by the mysql client, which requires Unix path.

  @todo This is only useful under windows, so may be ifdef'ed out on
  other systems.  /Sven

  @todo If a Create_file_log_event contains a filename with a
  backslash (valid under unix), then we have problems under windows.
  /Sven

  @param[in,out] fname Filename to modify. The filename is modified
  in-place.
*/
static void convert_path_to_forward_slashes(char *fname)
{
  while (*fname)
  {
    if (*fname == '\\')
      *fname= '/';
    fname++;
  }
}


/**
  Indicates whether the given database should be filtered out,
  according to the --database=X option.

  @param log_dbname Name of database.

  @return nonzero if the database with the given name should be
  filtered out, 0 otherwise.
*/
static bool shall_skip_database(const char *log_dbname)
{
  return one_database &&
         (log_dbname != NULL) &&
         strcmp(log_dbname, database);
}


/**
  Print "use <db>" statement when current db is to be changed.

  We have to control emiting USE statements according to rewrite-db options.
  We have to do it here (see process_event() below) and to suppress
  producing USE statements by corresponding log event print-functions.
*/

static void
print_use_stmt(PRINT_EVENT_INFO* pinfo, const Query_log_event *ev)
{
  const char* db= ev->db;
  const size_t db_len= ev->db_len;

  // pinfo->db is the current db.
  // If current db is the same as required db, do nothing.
  if ((ev->flags & LOG_EVENT_SUPPRESS_USE_F) || !db ||
      !memcmp(pinfo->db, db, db_len + 1))
    return;

  // Current db and required db are different.
  // Check for rewrite rule for required db. (Note that in a rewrite rule
  // neither db_from nor db_to part can be empty).
  size_t len_to= 0;
  const char *db_to= binlog_filter->get_rewrite_db(db, &len_to);

  // If there is no rewrite rule for db (in this case len_to is left = 0),
  // printing of the corresponding USE statement is left for log event
  // print-function.
  if (!len_to)
    return;

  // In case of rewrite rule print USE statement for db_to
  my_fprintf(result_file, "use %`s%s\n", db_to, pinfo->delimiter);

  // Copy the *original* db to pinfo to suppress emiting
  // of USE stmts by log_event print-functions.
  memcpy(pinfo->db, db, db_len + 1);
}


/**
   Print "SET skip_replication=..." statement when needed.

   Not all servers support this (only MariaDB from some version on). So we
   mark the SET to only execute from the version of MariaDB that supports it,
   and also only output it if we actually see events with the flag set, to not
   get spurious errors on MySQL@Oracle servers of higher version that do not
   support the flag.

   So we start out assuming @@skip_replication is 0, and only output a SET
   statement when it changes.
*/
static void
print_skip_replication_statement(PRINT_EVENT_INFO *pinfo, const Log_event *ev)
{
  bool cur_val;

  cur_val= (ev->flags & LOG_EVENT_SKIP_REPLICATION_F) != 0;
  if (cur_val == pinfo->skip_replication)
    return;                                     /* Not changed. */
  fprintf(result_file, "/*!50521 SET skip_replication=%d*/%s\n",
          cur_val, pinfo->delimiter);
  pinfo->skip_replication= cur_val;
}

/**
  Indicates whether the given table should be filtered out,
  according to the --table=X option.

  @param log_tblname Name of table.

  @return nonzero if the table with the given name should be
  filtered out, 0 otherwise.
*/
static bool shall_skip_table(const char *log_tblname)
{
  return one_table &&
         (log_tblname != NULL) &&
         strcmp(log_tblname, table);
}


/**
  Prints the given event in base64 format.

  The header is printed to the head cache and the body is printed to
  the body cache of the print_event_info structure.  This allows all
  base64 events corresponding to the same statement to be joined into
  one BINLOG statement.

  @param[in] ev Log_event to print.
  @param[in,out] result_file FILE to which the output will be written.
  @param[in,out] print_event_info Parameters and context state
  determining how to print.

  @retval ERROR_STOP An error occurred - the program should terminate.
  @retval OK_CONTINUE No error, the program should continue.
*/
static Exit_status
write_event_header_and_base64(Log_event *ev, FILE *result_file,
                              PRINT_EVENT_INFO *print_event_info)
{
  IO_CACHE *head= &print_event_info->head_cache;
  IO_CACHE *body= &print_event_info->body_cache;
  DBUG_ENTER("write_event_header_and_base64");

  /* Write header and base64 output to cache */
  if (ev->print_header(head, print_event_info, FALSE))
    DBUG_RETURN(ERROR_STOP);

  DBUG_ASSERT(print_event_info->base64_output_mode == BASE64_OUTPUT_ALWAYS);

  if (ev->print_base64(body, print_event_info,
                       print_event_info->base64_output_mode !=
                       BASE64_OUTPUT_DECODE_ROWS))
    DBUG_RETURN(ERROR_STOP);

  /* Read data from cache and write to result file */
  if (copy_event_cache_to_file_and_reinit(head, result_file) ||
      copy_event_cache_to_file_and_reinit(body, result_file))
  {
    error("Error writing event to file.");
    DBUG_RETURN(ERROR_STOP);
  }
  DBUG_RETURN(OK_CONTINUE);
}


static bool print_base64(PRINT_EVENT_INFO *print_event_info, Log_event *ev)
{
  /*
    These events must be printed in base64 format, if printed.
    base64 format requires a FD event to be safe, so if no FD
    event has been printed, we give an error.  Except if user
    passed --short-form, because --short-form disables printing
    row events.
  */

  if (!print_event_info->printed_fd_event && !short_form &&
      opt_base64_output_mode != BASE64_OUTPUT_DECODE_ROWS &&
      opt_base64_output_mode != BASE64_OUTPUT_NEVER)
  {
    const char* type_str= ev->get_type_str();
    error("malformed binlog: it does not contain any "
            "Format_description_log_event. Found a %s event, which "
            "is not safe to process without a "
            "Format_description_log_event.",
            type_str);
    return 1;
  }

  return ev->print(result_file, print_event_info);
}


static bool print_row_event(PRINT_EVENT_INFO *print_event_info, Log_event *ev,
                            ulonglong table_id, bool is_stmt_end)
{
  Table_map_log_event *ignored_map= 
    print_event_info->m_table_map_ignored.get_table(table_id);
  bool skip_event= (ignored_map != NULL);
  char ll_buff[21];
  bool result= 0;

  if (opt_flashback)
  {
    Rows_log_event *e= (Rows_log_event*) ev;
    // The last Row_log_event will be the first event in Flashback
    if (is_stmt_end)
      e->clear_flags(Rows_log_event::STMT_END_F);
    // The first Row_log_event will be the last event in Flashback
    if (events_in_stmt.elements == 0)
      e->set_flags(Rows_log_event::STMT_END_F);
    // Update the temp_buf
    e->update_flags();

    if (insert_dynamic(&events_in_stmt, (uchar *) &ev))
    {
      error("Out of memory: can't allocate memory to store the flashback events.");
      exit(1);
    }
  }

  /* 
     end of statement check:
       i) destroy/free ignored maps
      ii) if skip event
            a) since we are skipping the last event,
               append END-MARKER(') to body cache (if required)

            b) flush cache now
   */
  if (is_stmt_end)
  {
    /* 
      Now is safe to clear ignored map (clear_tables will also
      delete original table map events stored in the map).
    */
    if (print_event_info->m_table_map_ignored.count() > 0)
      print_event_info->m_table_map_ignored.clear_tables();

    /* 
      If there is a kept Annotate event and all corresponding
      rbr-events were filtered away, the Annotate event was not
      freed and it is just the time to do it.
    */
      free_annotate_event();

    /* 
       One needs to take into account an event that gets
       filtered but was last event in the statement. If this is
       the case, previous rows events that were written into
       IO_CACHEs still need to be copied from cache to
       result_file (as it would happen in ev->print(...) if
       event was not skipped).
    */
    if (skip_event)
    {
      // append END-MARKER(') with delimiter
      IO_CACHE *const body_cache= &print_event_info->body_cache;
      if (my_b_tell(body_cache))
        my_b_printf(body_cache, "'%s\n", print_event_info->delimiter);

      // flush cache
      if ((copy_event_cache_to_file_and_reinit(&print_event_info->head_cache, result_file) ||
          copy_event_cache_to_file_and_reinit(&print_event_info->body_cache, result_file)))
        return 1;
    }
  }

  /* skip the event check */
  if (skip_event)
    return 0;

  if (!opt_flashback)
    result= print_base64(print_event_info, ev);
  else
  {
    if (is_stmt_end)
    {
      Log_event *e= NULL;

      // Print the row_event from the last one to the first one
      for (uint i= events_in_stmt.elements; i > 0; --i)
      {
        e= *(dynamic_element(&events_in_stmt, i - 1, Log_event**));
        result= result || print_base64(print_event_info, e);
      }
      // Copy all output into the Log_event
      ev->output_buf.copy(e->output_buf);
      // Delete Log_event
      for (uint i= 0; i < events_in_stmt.elements-1; ++i)
      {
        e= *(dynamic_element(&events_in_stmt, i, Log_event**));
        delete e;
      }
      reset_dynamic(&events_in_stmt);
    }
  }

  if (is_stmt_end && !result)
  {
    if (print_event_info->print_row_count)
      fprintf(result_file, "# Number of rows: %s\n",
              llstr(print_event_info->row_events, ll_buff));
    print_event_info->row_events= 0;
  }
  return result;
}


/**
  Print the given event, and either delete it or delegate the deletion
  to someone else.

  The deletion may be delegated in two cases: (1) the event is a
  Format_description_log_event, and is saved in
  glob_description_event; (2) the event is a Create_file_log_event,
  and is saved in load_processor.

  @param[in,out] print_event_info Parameters and context state
  determining how to print.
  @param[in] ev Log_event to process.
  @param[in] pos Offset from beginning of binlog file.
  @param[in] logname Name of input binlog.

  @retval ERROR_STOP An error occurred - the program should terminate.
  @retval OK_CONTINUE No error, the program should continue.
  @retval OK_STOP No error, but the end of the specified range of
  events to process has been reached and the program should terminate.
*/
Exit_status process_event(PRINT_EVENT_INFO *print_event_info, Log_event *ev,
                          my_off_t pos, const char *logname)
{
  char ll_buff[21];
  Log_event_type ev_type= ev->get_type_code();
  my_bool destroy_evt= TRUE;
  DBUG_ENTER("process_event");
  Exit_status retval= OK_CONTINUE;
  IO_CACHE *const head= &print_event_info->head_cache;

  /* Bypass flashback settings to event */
  ev->is_flashback= opt_flashback;
#ifdef WHEN_FLASHBACK_REVIEW_READY
  ev->need_flashback_review= opt_flashback_review;
#endif

  /*
    Format events are not concerned by --offset and such, we always need to
    read them to be able to process the wanted events.
  */
  if (((rec_count >= offset) &&
       (ev->when >= start_datetime)) ||
      (ev_type == FORMAT_DESCRIPTION_EVENT))
  {
    if (ev_type != FORMAT_DESCRIPTION_EVENT)
    {
      /*
        We have found an event after start_datetime, from now on print
        everything (in case the binlog has timestamps increasing and
        decreasing, we do this to avoid cutting the middle).
      */
      start_datetime= 0;
      offset= 0; // print everything and protect against cycling rec_count
      /*
        Skip events according to the --server-id flag.  However, don't
        skip format_description or rotate events, because they they
        are really "global" events that are relevant for the entire
        binlog, even if they have a server_id.  Also, we have to read
        the format_description event so that we can parse subsequent
        events.
      */
      if (ev_type != ROTATE_EVENT &&
          server_id && (server_id != ev->server_id))
        goto end;
    }
    if ((ev->when >= stop_datetime)
        || (pos >= stop_position_mot))
    {
      /* end the program */
      retval= OK_STOP;
      goto end;
    }
    if (print_row_event_positions)
      fprintf(result_file, "# at %s\n",llstr(pos,ll_buff));

    if (!opt_hexdump)
      print_event_info->hexdump_from= 0; /* Disabled */
    else
      print_event_info->hexdump_from= pos;

    print_event_info->base64_output_mode= opt_base64_output_mode;

    DBUG_PRINT("debug", ("event_type: %s", ev->get_type_str()));

    switch (ev_type) {
    case QUERY_EVENT:
    case QUERY_COMPRESSED_EVENT:
    {
      Query_log_event *qe= (Query_log_event*)ev;
      if (!qe->is_trans_keyword())
      {
        if (shall_skip_database(qe->db))
          goto end;
      }
      else
      {
        /*
          In case the event for one of these statements is obtained
          from binary log 5.0, make it compatible with 5.1
        */
        qe->flags|= LOG_EVENT_SUPPRESS_USE_F;
      }
      print_use_stmt(print_event_info, qe);
      if (opt_base64_output_mode == BASE64_OUTPUT_ALWAYS)
      {
        if ((retval= write_event_header_and_base64(ev, result_file,
                                                   print_event_info)) !=
            OK_CONTINUE)
          goto end;
      }
      else
      {
        print_skip_replication_statement(print_event_info, ev);
        if (ev->print(result_file, print_event_info))
          goto err;
      }
      if (head->error == -1)
        goto err;
      break;
    }

    case CREATE_FILE_EVENT:
    {
      Create_file_log_event* ce= (Create_file_log_event*)ev;
      /*
        We test if this event has to be ignored. If yes, we don't save
        this event; this will have the good side-effect of ignoring all
        related Append_block and Exec_load.
        Note that Load event from 3.23 is not tested.
      */
      if (shall_skip_database(ce->db))
        goto end;                // Next event
      /*
	We print the event, but with a leading '#': this is just to inform 
	the user of the original command; the command we want to execute 
	will be a derivation of this original command (we will change the 
	filename and use LOCAL), prepared in the 'case EXEC_LOAD_EVENT' 
	below.
      */
      if (opt_base64_output_mode == BASE64_OUTPUT_ALWAYS)
      {
        if ((retval= write_event_header_and_base64(ce, result_file,
                                                   print_event_info)) !=
            OK_CONTINUE)
          goto end;
      }
      else
      {
        print_skip_replication_statement(print_event_info, ev);
        if (ce->print(result_file, print_event_info, TRUE))
          goto err;
      }
      // If this binlog is not 3.23 ; why this test??
      if (glob_description_event->binlog_version >= 3)
      {
        /*
          transfer the responsibility for destroying the event to
          load_processor
        */
        ev= NULL;
        if ((retval= load_processor.process(ce)) != OK_CONTINUE)
          goto end;
      }
      break;
    }

    case APPEND_BLOCK_EVENT:
      /*
        Append_block_log_events can safely print themselves even if
        the subsequent call load_processor.process fails, because the
        output of Append_block_log_event::print is only a comment.
      */
      if (ev->print(result_file, print_event_info))
        goto err;
      if ((retval= load_processor.process((Append_block_log_event*) ev)) !=
          OK_CONTINUE)
        goto end;
      break;

    case EXEC_LOAD_EVENT:
    {
      if (ev->print(result_file, print_event_info))
        goto err;
      Execute_load_log_event *exv= (Execute_load_log_event*)ev;
      Create_file_log_event *ce= load_processor.grab_event(exv->file_id);
      /*
	if ce is 0, it probably means that we have not seen the Create_file
	event (a bad binlog, or most probably --start-position is after the
	Create_file event). Print a warning comment.
      */
      if (ce)
      {
        bool error;
        /*
          We must not convert earlier, since the file is used by
          my_open() in Load_log_processor::append().
        */
        convert_path_to_forward_slashes((char*) ce->fname);
	error= ce->print(result_file, print_event_info, TRUE);
	my_free((void*)ce->fname);
	delete ce;
        if (error)
          goto err;
      }
      else
        warning("Ignoring Execute_load_log_event as there is no "
                "Create_file event for file_id: %u", exv->file_id);
      break;
    }
    case FORMAT_DESCRIPTION_EVENT:
      delete glob_description_event;
      glob_description_event= (Format_description_log_event*) ev;
      destroy_evt= 0;
      print_event_info->common_header_len=
        glob_description_event->common_header_len;
      if (ev->print(result_file, print_event_info))
        goto err;
      if (!remote_opt)
      {
        ev->free_temp_buf(); // free memory allocated in dump_local_log_entries
      }
      else
      {
        /*
          disassociate but not free dump_remote_log_entries time memory
        */
        ev->temp_buf= 0;
      }
      /*
        We don't want this event to be deleted now, so let's hide it (I
        (Guilhem) should later see if this triggers a non-serious Valgrind
        error). Not serious error, because we will free description_event
        later.
      */
      ev= 0;
      if (!force_if_open_opt &&
          (glob_description_event->flags & LOG_EVENT_BINLOG_IN_USE_F))
      {
        error("Attempting to dump binlog '%s', which was not closed properly. "
              "Most probably, mysqld is still writing it, or it crashed. "
              "Rerun with --force-if-open to ignore this problem.", logname);
        DBUG_RETURN(ERROR_STOP);
      }
      break;
    case BEGIN_LOAD_QUERY_EVENT:
      if (ev->print(result_file, print_event_info))
        goto err;
      if ((retval= load_processor.process((Begin_load_query_log_event*) ev)) !=
          OK_CONTINUE)
        goto end;
      break;
    case EXECUTE_LOAD_QUERY_EVENT:
    {
      Execute_load_query_log_event *exlq= (Execute_load_query_log_event*)ev;
      char *fname= load_processor.grab_fname(exlq->file_id);

      if (!shall_skip_database(exlq->db))
      {
        print_use_stmt(print_event_info, exlq);
        if (fname)
        {
          convert_path_to_forward_slashes(fname);
          print_skip_replication_statement(print_event_info, ev);
          if (exlq->print(result_file, print_event_info, fname))
          {
            my_free(fname);
            goto err;
          }
        }
        else
          warning("Ignoring Execute_load_query since there is no "
                  "Begin_load_query event for file_id: %u", exlq->file_id);
      }
      my_free(fname);
      break;
    }
    case ANNOTATE_ROWS_EVENT:
      if (!opt_skip_annotate_row_events)
      {
        /*
          We don't print Annotate event just now because all subsequent
          rbr-events can be filtered away. Instead we'll keep the event
          till it will be printed together with the first not filtered
          away Table map or the last rbr will be processed.
        */
        keep_annotate_event((Annotate_rows_log_event*) ev);
        destroy_evt= FALSE;
      }
      break;
    case TABLE_MAP_EVENT:
    {
      Table_map_log_event *map= ((Table_map_log_event *)ev);
      if (shall_skip_database(map->get_db_name()) ||
          shall_skip_table(map->get_table_name()))
      {
        print_event_info->m_table_map_ignored.set_table(map->get_table_id(), map);
        destroy_evt= FALSE;
        goto end;
      }
#ifdef WHEN_FLASHBACK_REVIEW_READY
      /* Create review table for Flashback */
      if (opt_flashback_review)
      {
        // Check if the table was already created?
        Table_map_log_event *exist_table;
        exist_table= print_event_info->m_table_map.get_table(map->get_table_id());

        if (!exist_table)
        {

          MYSQL *conn;
          MYSQL_RES *res;
          MYSQL_ROW row;
          char tmp_sql[8096];
          int  tmp_sql_offset;

          conn = mysql_init(NULL);
          if (!mysql_real_connect(conn, host, user, pass,
                map->get_db_name(), port, sock, 0))
          {
            fprintf(stderr, "%s\n", mysql_error(conn));
            exit(1);
          }

          if (mysql_query(conn, "SET group_concat_max_len=10000;"))
          {
            fprintf(stderr, "%s\n", mysql_error(conn));
            exit(1);
          }

          memset(tmp_sql, 0, sizeof(tmp_sql));
          sprintf(tmp_sql, " "
                  "SELECT Group_concat(cols) "
                  "FROM   (SELECT 'op_type char(1)' cols "
                  "  UNION ALL "
                  "  SELECT Concat('`', column_name, '_old` ', column_type, ' ', "
                  "    IF(character_set_name IS NOT NULL, "
                  "      Concat('character set ', character_set_name, ' '), ' '), "
                  "    IF(collation_name IS NOT NULL, "
                  "      Concat('collate ', collation_name, ' '), ' ')) cols "
                  "  FROM   information_schema.columns "
                  "  WHERE  table_schema = '%s' "
                  "  AND table_name = '%s' "
                  "  UNION ALL "
                  "  SELECT Concat('`', column_name, '_new` ', column_type, ' ', "
                  "    IF(character_set_name IS NOT NULL, "
                  "      Concat('character set ', character_set_name, ' '), ' '), "
                  "    IF(collation_name IS NOT NULL, "
                  "      Concat('collate ', collation_name, ' '), ' ')) cols "
                  "  FROM   information_schema.columns "
                  "  WHERE  table_schema = '%s' "
                  "  AND table_name = '%s') tmp;",
            map->get_db_name(), map->get_table_name(),
            map->get_db_name(), map->get_table_name());

          if (mysql_query(conn, tmp_sql))
          {
            fprintf(stderr, "%s\n", mysql_error(conn));
            exit(1);
          }
          res = mysql_use_result(conn);
          if ((row = mysql_fetch_row(res)) != NULL)  // only one row
          {
            if (flashback_review_dbname)
            {
              ev->set_flashback_review_dbname(flashback_review_dbname);
            }
            else
            {
              ev->set_flashback_review_dbname(map->get_db_name());
            }
            if (flashback_review_tablename)
            {
              ev->set_flashback_review_tablename(flashback_review_tablename);
            }
            else
            {
              memset(tmp_sql, 0, sizeof(tmp_sql));
              sprintf(tmp_sql, "__%s", map->get_table_name());
              ev->set_flashback_review_tablename(tmp_sql);
            }
            memset(tmp_sql, 0, sizeof(tmp_sql));
            tmp_sql_offset= sprintf(tmp_sql, "CREATE TABLE IF NOT EXISTS");
            tmp_sql_offset+= sprintf(tmp_sql + tmp_sql_offset, " `%s`.`%s` (%s) %s",
                                     ev->get_flashback_review_dbname(),
                                     ev->get_flashback_review_tablename(),
                                     row[0],
                                     print_event_info->delimiter);
          }
          fprintf(result_file, "%s\n", tmp_sql);
          mysql_free_result(res);
          mysql_close(conn);
        }
        else
        {
          char tmp_str[128];

          if (flashback_review_dbname)
            ev->set_flashback_review_dbname(flashback_review_dbname);
          else
            ev->set_flashback_review_dbname(map->get_db_name());

          if (flashback_review_tablename)
            ev->set_flashback_review_tablename(flashback_review_tablename);
          else
          {
            memset(tmp_str, 0, sizeof(tmp_str));
            sprintf(tmp_str, "__%s", map->get_table_name());
            ev->set_flashback_review_tablename(tmp_str);
          }
        }
      }
#endif

      /*
        The Table map is to be printed, so it's just the time when we may
        print the kept Annotate event (if there is any).
        print_annotate_event() also deletes the kept Annotate event.
      */
      if (print_annotate_event(print_event_info))
        goto err;

      size_t len_to= 0;
      const char* db_to= binlog_filter->get_rewrite_db(map->get_db_name(), &len_to);
      if (len_to && map->rewrite_db(db_to, len_to, glob_description_event))
      {
        error("Could not rewrite database name");
        goto err;
      }
      if (print_base64(print_event_info, ev))
        goto err;
      if (opt_flashback)
        reset_dynamic(&events_in_stmt);
      break;
    }
    case WRITE_ROWS_EVENT:
    case DELETE_ROWS_EVENT:
    case UPDATE_ROWS_EVENT:
    case WRITE_ROWS_EVENT_V1:
    case UPDATE_ROWS_EVENT_V1:
    case DELETE_ROWS_EVENT_V1:
    case WRITE_ROWS_COMPRESSED_EVENT:
    case DELETE_ROWS_COMPRESSED_EVENT:
    case UPDATE_ROWS_COMPRESSED_EVENT:
    case WRITE_ROWS_COMPRESSED_EVENT_V1:
    case UPDATE_ROWS_COMPRESSED_EVENT_V1:
    case DELETE_ROWS_COMPRESSED_EVENT_V1:
    {
      Rows_log_event *e= (Rows_log_event*) ev;
      bool is_stmt_end= e->get_flags(Rows_log_event::STMT_END_F);
      if (!print_event_info->found_row_event)
      {
        print_event_info->found_row_event= 1;
        print_event_info->row_events= 0;
      }
      if (print_row_event(print_event_info, ev, e->get_table_id(),
                          e->get_flags(Rows_log_event::STMT_END_F)))
        goto err;
      DBUG_PRINT("info", ("is_stmt_end: %d", (int) is_stmt_end));
      if (is_stmt_end)
        print_event_info->found_row_event= 0;
      else if (opt_flashback)
        destroy_evt= FALSE;
      break;
    }
    case PRE_GA_WRITE_ROWS_EVENT:
    case PRE_GA_DELETE_ROWS_EVENT:
    case PRE_GA_UPDATE_ROWS_EVENT:
    {
      Old_rows_log_event *e= (Old_rows_log_event*) ev;
      bool is_stmt_end= e->get_flags(Rows_log_event::STMT_END_F);
      if (print_row_event(print_event_info, ev, e->get_table_id(),
                          e->get_flags(Old_rows_log_event::STMT_END_F)))
        goto err;
      DBUG_PRINT("info", ("is_stmt_end: %d", (int) is_stmt_end));
      if (!is_stmt_end && opt_flashback)
        destroy_evt= FALSE;
      break;
    }
    case START_ENCRYPTION_EVENT:
      glob_description_event->start_decryption((Start_encryption_log_event*)ev);
      /* fall through */
    default:
      print_skip_replication_statement(print_event_info, ev);
      if (ev->print(result_file, print_event_info))
        goto err;
    }
  }

  goto end;

err:
  retval= ERROR_STOP;
end:
  rec_count++;

  DBUG_PRINT("info", ("end event processing"));
  /*
    Destroy the log_event object. 
    MariaDB MWL#36: mainline does this:
      If reading from a remote host,
      set the temp_buf to NULL so that memory isn't freed twice.
    We no longer do that, we use Rpl_filter::event_owns_temp_buf instead.
  */
  if (ev)
  {
    /* Holding event output if needed */
    if (!ev->output_buf.is_empty())
    {
      LEX_STRING tmp_str;

      tmp_str.length= ev->output_buf.length();
      tmp_str.str=    ev->output_buf.release();

      if (opt_flashback)
      {
        if (ev_type == STOP_EVENT)
          stop_event_string.reset(tmp_str.str, tmp_str.length, tmp_str.length,
                                  &my_charset_bin);
        else
        {
          if (insert_dynamic(&binlog_events, (uchar *) &tmp_str))
          {
            error("Out of memory: can't allocate memory to store the flashback events.");
            exit(1);
          }
        }
      }
      else
      {
        my_fwrite(result_file, (const uchar *) tmp_str.str, tmp_str.length,
                  MYF(MY_NABP));
        my_free(tmp_str.str);
      }
    }

    if (remote_opt)
      ev->temp_buf= 0;
    if (destroy_evt) /* destroy it later if not set (ignored table map) */
      delete ev;
  }
  DBUG_PRINT("exit",("return: %d", retval));
  DBUG_RETURN(retval);
}


static struct my_option my_options[] =
{
  {"help", '?', "Display this help and exit.",
   0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"base64-output", OPT_BASE64_OUTPUT_MODE,
    /* 'unspec' is not mentioned because it is just a placeholder. */
   "Determine when the output statements should be base64-encoded BINLOG "
   "statements: 'never' doesn't print binlog row events and should not be "
   "used when directing output to a MariaDB master; "
   "'decode-rows' decodes row events into commented SQL statements if the "
   "--verbose option is also given; "
   "'auto' prints base64 only when necessary (i.e., for row-based events and "
   "format description events); "
   "'always' prints base64 whenever possible. "
   "--base64-output with no 'name' argument is equivalent to "
   "--base64-output=always and is also deprecated.  If no "
   "--base64-output[=name] option is given at all, the default is 'auto'.",
   &opt_base64_output_mode_str, &opt_base64_output_mode_str,
   0, GET_STR, OPT_ARG, 0, 0, 0, 0, 0, 0},
  /*
    mysqlbinlog needs charsets knowledge, to be able to convert a charset
    number found in binlog to a charset name (to be able to print things
    like this:
    SET @`a`:=_cp850 0x4DFC6C6C6572 COLLATE `cp850_general_ci`;
  */
  {"character-sets-dir", OPT_CHARSETS_DIR,
   "Directory for character set files.", &charsets_dir,
   &charsets_dir, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"database", 'd', "List entries for just this database (local log only).",
   &database, &database, 0, GET_STR_ALLOC, REQUIRED_ARG,
   0, 0, 0, 0, 0, 0},
#ifndef DBUG_OFF
  {"debug", '#', "Output debug log.", &current_dbug_option,
   &current_dbug_option, 0, GET_STR, OPT_ARG, 0, 0, 0, 0, 0, 0},
#endif
  {"debug-check", OPT_DEBUG_CHECK, "Check memory and open file usage at exit .",
   &debug_check_flag, &debug_check_flag, 0,
   GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"debug-info", OPT_DEBUG_INFO, "Print some debug info at exit.",
   &debug_info_flag, &debug_info_flag,
   0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"default_auth", OPT_DEFAULT_AUTH,
   "Default authentication client-side plugin to use.",
   &opt_default_auth, &opt_default_auth, 0,
   GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"disable-log-bin", 'D', "Disable binary log. This is useful, if you "
    "enabled --to-last-log and are sending the output to the same MariaDB server. "
    "This way you could avoid an endless loop. You would also like to use it "
    "when restoring after a crash to avoid duplication of the statements you "
    "already have. NOTE: you will need a SUPER privilege to use this option.",
   &disable_log_bin, &disable_log_bin, 0, GET_BOOL,
   NO_ARG, 0, 0, 0, 0, 0, 0},
  {"flashback", 'B', "Flashback feature can rollback you committed data to a special time point.",
#ifdef WHEN_FLASHBACK_REVIEW_READY
   "before Flashback feature writing a row, original row can insert to review-dbname.review-tablename,"
   "and mysqlbinlog will login mysql by user(-u) and password(-p) and host(-h).",
#endif
   &opt_flashback, &opt_flashback, 0, GET_BOOL, NO_ARG, 0, 0, 0, 0,
   0, 0},
  {"force-if-open", 'F', "Force if binlog was not closed properly.",
   &force_if_open_opt, &force_if_open_opt, 0, GET_BOOL, NO_ARG,
   1, 0, 0, 0, 0, 0},
  {"force-read", 'f', "Force reading unknown binlog events.",
   &force_opt, &force_opt, 0, GET_BOOL, NO_ARG, 0, 0, 0, 0,
   0, 0},
  {"hexdump", 'H', "Augment output with hexadecimal and ASCII event dump.",
   &opt_hexdump, &opt_hexdump, 0, GET_BOOL, NO_ARG,
   0, 0, 0, 0, 0, 0},
  {"host", 'h', "Get the binlog from server.", &host, &host,
   0, GET_STR_ALLOC, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"local-load", 'l', "Prepare local temporary files for LOAD DATA INFILE in the specified directory.",
   &dirname_for_local_load, &dirname_for_local_load, 0,
   GET_STR_ALLOC, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"offset", 'o', "Skip the first N entries.", &offset, &offset,
   0, GET_ULL, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"password", 'p', "Password to connect to remote server.",
   0, 0, 0, GET_STR, OPT_ARG, 0, 0, 0, 0, 0, 0},
  {"plugin_dir", OPT_PLUGIN_DIR, "Directory for client-side plugins.",
    &opt_plugindir, &opt_plugindir, 0,
   GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"port", 'P', "Port number to use for connection or 0 for default to, in "
   "order of preference, my.cnf, $MYSQL_TCP_PORT, "
#if MYSQL_PORT_DEFAULT == 0
   "/etc/services, "
#endif
   "built-in default (" STRINGIFY_ARG(MYSQL_PORT) ").",
   &port, &port, 0, GET_INT, REQUIRED_ARG,
   0, 0, 0, 0, 0, 0},
  {"protocol", OPT_MYSQL_PROTOCOL,
   "The protocol to use for connection (tcp, socket, pipe).",
   0, 0, 0, GET_STR,  REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"read-from-remote-server", 'R', "Read binary logs from a MariaDB server.",
   &remote_opt, &remote_opt, 0, GET_BOOL, NO_ARG, 0, 0, 0, 0,
   0, 0},
  {"raw", 0, "Requires -R. Output raw binlog data instead of SQL "
   "statements. Output files named after server logs.",
   &opt_raw_mode, &opt_raw_mode, 0, GET_BOOL, NO_ARG, 0, 0, 0, 0,
   0, 0},
  {"result-file", 'r', "Direct output to a given file. With --raw this is a "
   "prefix for the file names.",
   &result_file_name, &result_file_name, 0, GET_STR, REQUIRED_ARG,
   0, 0, 0, 0, 0, 0},
#ifdef WHEN_FLASHBACK_REVIEW_READY
  {"review", opt_flashback_review, "Print review sql in output file.",
   &opt_flashback_review, &opt_flashback_review, 0, GET_BOOL, NO_ARG, 0, 0, 0, 0,
   0, 0},
  {"review-dbname", opt_flashback_flashback_review_dbname,
   "Writing flashback original row data into this db",
   &flashback_review_dbname, &flashback_review_dbname,
   0, GET_STR_ALLOC, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"review-tablename", opt_flashback_flashback_review_tablename,
   "Writing flashback original row data into this table",
   &flashback_review_tablename, &flashback_review_tablename,
   0, GET_STR_ALLOC, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
#endif
  {"print-row-count", OPT_PRINT_ROW_COUNT,
   "Print row counts for each row events",
   &print_row_count, &print_row_count, 0, GET_BOOL, NO_ARG, 1, 0, 0, 0,
   0, 0},
  {"print-row-event-positions", OPT_PRINT_ROW_EVENT_POSITIONS,
   "Print row event positions",
   &print_row_event_positions, &print_row_event_positions, 0, GET_BOOL,
   NO_ARG, 1, 0, 0, 0, 0, 0},
  {"server-id", 0,
   "Extract only binlog entries created by the server having the given id.",
   &server_id, &server_id, 0, GET_ULONG,
   REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"set-charset", OPT_SET_CHARSET,
   "Add 'SET NAMES character_set' to the output.", &charset,
   &charset, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"short-form", 's', "Just show regular queries: no extra info, no "
   "row-based events and no row counts. This is mainly for testing only, "
   "and should not be used to feed to the MariaDB server. "
   "If you want to just suppress base64-output, you can instead "
   "use --base64-output=never",
   &short_form, &short_form, 0, GET_BOOL, NO_ARG, 0, 0, 0, 0,
   0, 0},
  {"socket", 'S', "The socket file to use for connection.",
   &sock, &sock, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0,
   0, 0},
#include <sslopt-longopts.h>
  {"start-datetime", OPT_START_DATETIME,
   "Start reading the binlog at first event having a datetime equal or "
   "posterior to the argument; the argument must be a date and time "
   "in the local time zone, in any format accepted by the MariaDB server "
   "for DATETIME and TIMESTAMP types, for example: 2004-12-25 11:25:56 "
   "(you should probably use quotes for your shell to set it properly).",
   &start_datetime_str, &start_datetime_str,
   0, GET_STR_ALLOC, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"start-position", 'j',
   "Start reading the binlog at position N. Applies to the first binlog "
   "passed on the command line.",
   &start_position, &start_position, 0, GET_ULL,
   REQUIRED_ARG, BIN_LOG_HEADER_SIZE, BIN_LOG_HEADER_SIZE,
   /*
     COM_BINLOG_DUMP accepts only 4 bytes for the position
     so remote log reading has lower limit.
   */
   (ulonglong)(0xffffffffffffffffULL), 0, 0, 0},
  {"stop-datetime", OPT_STOP_DATETIME,
   "Stop reading the binlog at first event having a datetime equal or "
   "posterior to the argument; the argument must be a date and time "
   "in the local time zone, in any format accepted by the MariaDB server "
   "for DATETIME and TIMESTAMP types, for example: 2004-12-25 11:25:56 "
   "(you should probably use quotes for your shell to set it properly).",
   &stop_datetime_str, &stop_datetime_str,
   0, GET_STR_ALLOC, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"stop-never", 0, "Wait for more data from the server "
   "instead of stopping at the end of the last log. Implies --to-last-log.",
   &opt_stop_never, &opt_stop_never, 0,
   GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"stop-never-slave-server-id", 0,
   "The slave server_id used for --read-from-remote-server --stop-never.",
   &opt_stop_never_slave_server_id, &opt_stop_never_slave_server_id, 0,
   GET_ULONG, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"stop-position", OPT_STOP_POSITION,
   "Stop reading the binlog at position N. Applies to the last binlog "
   "passed on the command line.",
   &stop_position, &stop_position, 0, GET_ULL,
   REQUIRED_ARG, (longlong)(~(my_off_t)0), BIN_LOG_HEADER_SIZE,
   (ulonglong)(~(my_off_t)0), 0, 0, 0},
  {"table", 'T', "List entries for just this table (local log only).",
   &table, &table, 0, GET_STR_ALLOC, REQUIRED_ARG,
   0, 0, 0, 0, 0, 0},
  {"to-last-log", 't', "Requires -R. Will not stop at the end of the \
requested binlog but rather continue printing until the end of the last \
binlog of the MariaDB server. If you send the output to the same MariaDB server, \
that may lead to an endless loop.",
   &to_last_remote_log, &to_last_remote_log, 0, GET_BOOL,
   NO_ARG, 0, 0, 0, 0, 0, 0},
  {"user", 'u', "Connect to the remote server as username.",
   &user, &user, 0, GET_STR_ALLOC, REQUIRED_ARG, 0, 0, 0, 0,
   0, 0},
  {"verbose", 'v', "Reconstruct SQL statements out of row events. "
                   "-v -v adds comments on column data types.",
   0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"version", 'V', "Print version and exit.", 0, 0, 0, GET_NO_ARG, NO_ARG, 0,
   0, 0, 0, 0, 0},
  {"open_files_limit", OPT_OPEN_FILES_LIMIT,
   "Used to reserve file descriptors for use by this program.",
   &open_files_limit, &open_files_limit, 0, GET_ULONG,
   REQUIRED_ARG, MY_NFILE, 8, OS_FILE_LIMIT, 0, 1, 0},
  {"binlog-row-event-max-size", 0,
   "The maximum size of a row-based binary log event in bytes. Rows will be "
   "grouped into events smaller than this size if possible. "
   "This value must be a multiple of 256.",
   &opt_binlog_rows_event_max_size, &opt_binlog_rows_event_max_size, 0,
   GET_ULONG, REQUIRED_ARG, UINT_MAX,  256, ULONG_MAX,  0, 256,  0},
#ifndef DBUG_OFF
  {"debug-binlog-row-event-max-encoded-size", 0,
   "The maximum size of base64-encoded rows-event in one BINLOG pseudo-query "
   "instance. When the computed actual size exceeds the limit "
   "the BINLOG's argument string is fragmented in two.",
   &opt_binlog_rows_event_max_encoded_size,
   &opt_binlog_rows_event_max_encoded_size, 0,
   GET_ULONG, REQUIRED_ARG, UINT_MAX/4,  256, ULONG_MAX,  0, 256,  0},
#endif
  {"verify-binlog-checksum", 'c', "Verify checksum binlog events.",
   (uchar**) &opt_verify_binlog_checksum, (uchar**) &opt_verify_binlog_checksum,
   0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"rewrite-db", OPT_REWRITE_DB,
   "Updates to a database with a different name than the original. \
Example: rewrite-db='from->to'.",
   0, 0, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"skip-annotate-row-events", OPT_SKIP_ANNOTATE_ROWS_EVENTS,
   "Don't print Annotate_rows events stored in the binary log.",
   (uchar**) &opt_skip_annotate_row_events,
   (uchar**) &opt_skip_annotate_row_events,
   0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {0, 0, 0, 0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0}
};


/**
  Auxiliary function used by error() and warning().

  Prints the given text (normally "WARNING: " or "ERROR: "), followed
  by the given vprintf-style string, followed by a newline.

  @param format Printf-style format string.
  @param args List of arguments for the format string.
  @param msg Text to print before the string.
*/
static void error_or_warning(const char *format, va_list args, const char *msg)
{
  if (result_file)
    fflush(result_file);
  fprintf(stderr, "%s: ", msg);
  vfprintf(stderr, format, args);
  fprintf(stderr, "\n");
  fflush(stderr);
}

/**
  Prints a message to stderr, prefixed with the text "ERROR: " and
  suffixed with a newline.

  @param format Printf-style format string, followed by printf
  varargs.
*/
static void error(const char *format,...)
{
  va_list args;
  va_start(args, format);
  error_or_warning(format, args, "ERROR");
  va_end(args);
}


/**
  This function is used in log_event.cc to report errors.

  @param format Printf-style format string, followed by printf
  varargs.
*/
static void sql_print_error(const char *format,...)
{
  va_list args;
  va_start(args, format);
  error_or_warning(format, args, "ERROR");
  va_end(args);
}

/**
  Prints a message to stderr, prefixed with the text "WARNING: " and
  suffixed with a newline.

  @param format Printf-style format string, followed by printf
  varargs.
*/
static void warning(const char *format,...)
{
  va_list args;
  va_start(args, format);
  error_or_warning(format, args, "WARNING");
  va_end(args);
}

/**
  Frees memory for global variables in this file.
*/
static void cleanup()
{
  DBUG_ENTER("cleanup");
  my_free(pass);
  my_free(database);
  my_free(table);
  my_free(host);
  my_free(user);
  my_free(const_cast<char*>(dirname_for_local_load));
  my_free(start_datetime_str);
  my_free(stop_datetime_str);

  delete binlog_filter;
  delete glob_description_event;
  if (mysql)
    mysql_close(mysql);
  DBUG_VOID_RETURN;
}


static void print_version()
{
  printf("%s Ver 3.4 for %s at %s\n", my_progname, SYSTEM_TYPE, MACHINE_TYPE);
}


static void usage()
{
  print_version();
  puts(ORACLE_WELCOME_COPYRIGHT_NOTICE("2000"));
  printf("\
Dumps a MariaDB binary log in a format usable for viewing or for piping to\n\
the mysql command line client.\n\n");
  printf("Usage: %s [options] log-files\n", my_progname);
  print_defaults("my",load_groups);
  puts("");
  my_print_help(my_options);
  my_print_variables(my_options);
}


static my_time_t convert_str_to_timestamp(const char* str)
{
  MYSQL_TIME_STATUS status;
  MYSQL_TIME l_time;
  long dummy_my_timezone;
  uint dummy_in_dst_time_gap;
  
  /* We require a total specification (date AND time) */
  if (str_to_datetime_or_date(str, (uint) strlen(str), &l_time, 0, &status) ||
      l_time.time_type != MYSQL_TIMESTAMP_DATETIME || status.warnings)
  {
    error("Incorrect date and time argument: %s", str);
    exit(1);
  }
  /*
    Note that Feb 30th, Apr 31st cause no error messages and are mapped to
    the next existing day, like in mysqld. Maybe this could be changed when
    mysqld is changed too (with its "strict" mode?).
  */
  return
    my_system_gmt_sec(&l_time, &dummy_my_timezone, &dummy_in_dst_time_gap);
}


extern "C" my_bool
get_one_option(int optid, const struct my_option *opt __attribute__((unused)),
	       char *argument)
{
  bool tty_password=0;
  switch (optid) {
#ifndef DBUG_OFF
  case '#':
    if (!argument)
      argument= (char*) default_dbug_option;
    current_dbug_option= argument;
    DBUG_PUSH(argument);
    break;
#endif
#include <sslopt-case.h>
  case 'B':
    opt_flashback= 1;
    break;
  case 'd':
    one_database = 1;
    break;
  case 'p':
    if (argument == disabled_my_option)
      argument= (char*) "";                     // Don't require password
    if (argument)
    {
      my_free(pass);
      char *start=argument;
      pass= my_strdup(argument,MYF(MY_FAE));
      while (*argument) *argument++= 'x';		/* Destroy argument */
      if (*start)
        start[1]=0;				/* Cut length of argument */
    }
    else
      tty_password=1;
    break;
  case 'R':
    remote_opt= 1;
    break;
  case 'T':
    one_table= 1;
    break;
  case OPT_MYSQL_PROTOCOL:
    if ((opt_protocol= find_type_with_warning(argument, &sql_protocol_typelib,
                                              opt->name)) <= 0)
    {
      sf_leaking_memory= 1; /* no memory leak reports here */
      exit(1);
    }
    break;
#ifdef WHEN_FLASHBACK_REVIEW_READY
  case opt_flashback_review:
    opt_flashback_review= 1;
    break;
#endif
  case OPT_START_DATETIME:
    start_datetime= convert_str_to_timestamp(start_datetime_str);
    break;
  case OPT_STOP_DATETIME:
    stop_datetime= convert_str_to_timestamp(stop_datetime_str);
    break;
  case OPT_BASE64_OUTPUT_MODE:
    if (argument == NULL)
      opt_base64_output_mode= BASE64_OUTPUT_ALWAYS;
    else
    {
      int val;

      if ((val= find_type_with_warning(argument, &base64_output_mode_typelib,
                                       opt->name)) <= 0)
      {
        sf_leaking_memory= 1; /* no memory leak reports here */
        exit(1);
      }
      opt_base64_output_mode= (enum_base64_output_mode) (val - 1);
    }
    break;
  case OPT_REWRITE_DB:    // db_from->db_to
  {
    /* See also handling of OPT_REPLICATE_REWRITE_DB in sql/mysqld.cc */
    char* ptr;
    char* key= argument;  // db-from
    char* val;            // db-to

    // Where key begins
    while (*key && my_isspace(&my_charset_latin1, *key))
      key++;

    // Where val begins
    if (!(ptr= strstr(argument, "->")))
    {
      sql_print_error("Bad syntax in rewrite-db: missing '->'!\n");
      return 1;
    }
    val= ptr + 2;
    while (*val && my_isspace(&my_charset_latin1, *val))
      val++;

    // Write \0 and skip blanks at the end of key
    *ptr-- = 0;
    while (my_isspace(&my_charset_latin1, *ptr) && ptr > argument)
      *ptr-- = 0;

    if (!*key)
    {
      sql_print_error("Bad syntax in rewrite-db: empty db-from!\n");
      return 1;
    }

    // Skip blanks at the end of val
    ptr= val;
    while (*ptr && !my_isspace(&my_charset_latin1, *ptr))
      ptr++;
    *ptr= 0;

    if (!*val)
    {
      sql_print_error("Bad syntax in rewrite-db: empty db-to!\n");
      return 1;
    }

    binlog_filter->add_db_rewrite(key, val);
    break;
  }
  case OPT_PRINT_ROW_COUNT:
    print_row_count_used= 1;
    break;
  case OPT_PRINT_ROW_EVENT_POSITIONS:
    print_row_event_positions_used= 1;
    break;
  case 'v':
    if (argument == disabled_my_option)
      verbose= 0;
    else
      verbose++;
    break;
  case 'V':
    print_version();
    opt_version= 1;
    break;
  case '?':
    usage();
    opt_version= 1;
    break;
  }
  if (tty_password)
    pass= get_tty_password(NullS);

  return 0;
}


static int parse_args(int *argc, char*** argv)
{
  int ho_error;

  if ((ho_error=handle_options(argc, argv, my_options, get_one_option)))
    exit(ho_error);
  if (debug_info_flag)
    my_end_arg= MY_CHECK_ERROR | MY_GIVE_INFO;
  else if (debug_check_flag)
    my_end_arg= MY_CHECK_ERROR;
  if (start_position > UINT_MAX32 && remote_opt)
  {
    /* Here we just emulate old behaviour of option limit handling */
    fprintf(stderr, "Warning: option 'start-position': unsigned value %llu "
            "adjusted to 4294967295 (limitation of the client-server protocol)",
            start_position);
    start_position= UINT_MAX32;
  }
  return 0;
}


/**
  Create and initialize the global mysql object, and connect to the
  server.

  @retval ERROR_STOP An error occurred - the program should terminate.
  @retval OK_CONTINUE No error, the program should continue.
*/
static Exit_status safe_connect()
{
  my_bool reconnect= 1;
  /* Close any old connections to MySQL */
  if (mysql)
    mysql_close(mysql);

  mysql= mysql_init(NULL);

  if (!mysql)
  {
    error("Failed on mysql_init.");
    return ERROR_STOP;
  }

#ifdef HAVE_OPENSSL
  if (opt_use_ssl)
  {
    mysql_ssl_set(mysql, opt_ssl_key, opt_ssl_cert, opt_ssl_ca,
                  opt_ssl_capath, opt_ssl_cipher);
    mysql_options(mysql, MYSQL_OPT_SSL_CRL, opt_ssl_crl);
    mysql_options(mysql, MYSQL_OPT_SSL_CRLPATH, opt_ssl_crlpath);
  }
  mysql_options(mysql,MYSQL_OPT_SSL_VERIFY_SERVER_CERT,
                (char*)&opt_ssl_verify_server_cert);
#endif /*HAVE_OPENSSL*/

  if (opt_plugindir && *opt_plugindir)
    mysql_options(mysql, MYSQL_PLUGIN_DIR, opt_plugindir);

  if (opt_default_auth && *opt_default_auth)
    mysql_options(mysql, MYSQL_DEFAULT_AUTH, opt_default_auth);

  if (opt_protocol)
    mysql_options(mysql, MYSQL_OPT_PROTOCOL, (char*) &opt_protocol);
  mysql_options(mysql, MYSQL_OPT_CONNECT_ATTR_RESET, 0);
  mysql_options4(mysql, MYSQL_OPT_CONNECT_ATTR_ADD,
                 "program_name", "mysqlbinlog");
  if (!mysql_real_connect(mysql, host, user, pass, 0, port, sock, 0))
  {
    error("Failed on connect: %s", mysql_error(mysql));
    return ERROR_STOP;
  }
  mysql_options(mysql, MYSQL_OPT_RECONNECT, &reconnect);
  return OK_CONTINUE;
}


/**
  High-level function for dumping a named binlog.

  This function calls dump_remote_log_entries() or
  dump_local_log_entries() to do the job.

  @param[in] logname Name of input binlog.

  @retval ERROR_STOP An error occurred - the program should terminate.
  @retval OK_CONTINUE No error, the program should continue.
  @retval OK_STOP No error, but the end of the specified range of
  events to process has been reached and the program should terminate.
*/
static Exit_status dump_log_entries(const char* logname)
{
  Exit_status rc;
  PRINT_EVENT_INFO print_event_info;

  if (!print_event_info.init_ok())
    return ERROR_STOP;
  /*
     Set safe delimiter, to dump things
     like CREATE PROCEDURE safely
  */
  if (!opt_raw_mode)
    fprintf(result_file, "DELIMITER /*!*/;\n");
  strmov(print_event_info.delimiter, "/*!*/;");
  
  if (short_form)
  {
    if (!print_row_event_positions_used)
      print_row_event_positions= 0;
    if (!print_row_count_used)
      print_row_count = 0;
  }
  if (opt_flashback)
  {
    if (!print_row_event_positions_used)
      print_row_event_positions= 0;
  }

  print_event_info.verbose= short_form ? 0 : verbose;
  print_event_info.short_form= short_form;
  print_event_info.print_row_count= print_row_count;
  print_event_info.file= result_file;
  fflush(result_file);
  rc= (remote_opt ? dump_remote_log_entries(&print_event_info, logname) :
       dump_local_log_entries(&print_event_info, logname));

  if (rc == ERROR_STOP)
    return rc;

  /* Set delimiter back to semicolon */
  if (!opt_raw_mode && !opt_flashback)
    fprintf(result_file, "DELIMITER ;\n");
  strmov(print_event_info.delimiter, ";");
  return rc;
}


/**
  When reading a remote binlog, this function is used to grab the
  Format_description_log_event in the beginning of the stream.
  
  This is not as smart as check_header() (used for local log); it will
  not work for a binlog which mixes format. TODO: fix this.

  @retval ERROR_STOP An error occurred - the program should terminate.
  @retval OK_CONTINUE No error, the program should continue.
*/
static Exit_status check_master_version()
{
  MYSQL_RES* res = 0;
  MYSQL_ROW row;
  uint version;

  if (mysql_query(mysql, "SELECT VERSION()") ||
      !(res = mysql_store_result(mysql)))
  {
    error("Could not find server version: "
          "Query failed when checking master version: %s", mysql_error(mysql));
    return ERROR_STOP;
  }
  if (!(row = mysql_fetch_row(res)))
  {
    error("Could not find server version: "
          "Master returned no rows for SELECT VERSION().");
    goto err;
  }

  if (!(version = atoi(row[0])))
  {
    error("Could not find server version: "
          "Master reported NULL for the version.");
    goto err;
  }
  /* 
     Make a notice to the server that this client
     is checksum-aware. It does not need the first fake Rotate
     necessary checksummed. 
     That preference is specified below.
  */
  if (mysql_query(mysql, "SET @master_binlog_checksum='NONE'"))
  {
    error("Could not notify master about checksum awareness."
          "Master returned '%s'", mysql_error(mysql));
    goto err;
  }

  /*
    Announce our capabilities to the server, so it will send us all the events
    that we know about.
  */
  if (mysql_query(mysql, "SET @mariadb_slave_capability="
                  STRINGIFY_ARG(MARIA_SLAVE_CAPABILITY_MINE)))
  {
    error("Could not inform master about capability. Master returned '%s'",
          mysql_error(mysql));
    goto err;
  }

  delete glob_description_event;
  glob_description_event= NULL;

  switch (version) {
  case 3:
    glob_description_event= new Format_description_log_event(1);
    break;
  case 4:
    glob_description_event= new Format_description_log_event(3);
    break;
  case 5:
  case 10:
    /*
      The server is soon going to send us its Format_description log
      event, unless it is a 5.0 server with 3.23 or 4.0 binlogs.
      So we first assume that this is 4.0 (which is enough to read the
      Format_desc event if one comes).
    */
    glob_description_event= new Format_description_log_event(3);
    break;
  default:
    error("Could not find server version: "
          "Master reported unrecognized MariaDB version '%s'.", row[0]);
    goto err;
  }
  if (!glob_description_event || !glob_description_event->is_valid())
  {
    error("Failed creating Format_description_log_event; out of memory?");
    goto err;
  }

  mysql_free_result(res);
  return OK_CONTINUE;

err:
  mysql_free_result(res);
  return ERROR_STOP;
}


static Exit_status handle_event_text_mode(PRINT_EVENT_INFO *print_event_info,
                                          ulong *len,
                                          const char* logname,
                                          uint logname_len, my_off_t old_off)
{
  const char *error_msg;
  Log_event *ev;
  NET *net= &mysql->net;
  DBUG_ENTER("handle_event_text_mode");

  if (net->read_pos[5] == ANNOTATE_ROWS_EVENT)
  {
    if (!(ev= read_remote_annotate_event(net->read_pos + 1, *len - 1,
                                         &error_msg)))
    {
      error("Could not construct annotate event object: %s", error_msg);
      DBUG_RETURN(ERROR_STOP);
    }   
  }
  else
  {
    if (!(ev= Log_event::read_log_event((const char*) net->read_pos + 1 ,
                                        *len - 1, &error_msg,
                                        glob_description_event,
                                        opt_verify_binlog_checksum)))
    {
      error("Could not construct log event object: %s", error_msg);
      DBUG_RETURN(ERROR_STOP);
    }   
    /*
      If reading from a remote host, ensure the temp_buf for the
      Log_event class is pointing to the incoming stream.
    */
    ev->register_temp_buf((char *) net->read_pos + 1, FALSE);
  }

  Log_event_type type= ev->get_type_code();
  if (glob_description_event->binlog_version >= 3 ||
      (type != LOAD_EVENT && type != CREATE_FILE_EVENT))
  {
    /*
      If this is a Rotate event, maybe it's the end of the requested binlog;
      in this case we are done (stop transfer).
      This is suitable for binlogs, not relay logs (but for now we don't read
      relay logs remotely because the server is not able to do that). If one
      day we read relay logs remotely, then we will have a problem with the
      detection below: relay logs contain Rotate events which are about the
      binlogs, so which would trigger the end-detection below.
    */
    if (type == ROTATE_EVENT)
    {
      Rotate_log_event *rev= (Rotate_log_event *)ev;
      /*
        If this is a fake Rotate event, and not about our log, we can stop
        transfer. If this a real Rotate event (so it's not about our log,
        it's in our log describing the next log), we print it (because it's
        part of our log) and then we will stop when we receive the fake one
        soon.
      */
      if (rev->when == 0)
      {
        *len= 1; // fake Rotate, so don't increment old_off
        if (!to_last_remote_log)
        {
          if ((rev->ident_len != logname_len) ||
              memcmp(rev->new_log_ident, logname, logname_len))
          {
            delete ev;
            DBUG_RETURN(OK_EOF);
          }
          /*
            Otherwise, this is a fake Rotate for our log, at the very
            beginning for sure. Skip it, because it was not in the original
            log. If we are running with to_last_remote_log, we print it,
            because it serves as a useful marker between binlogs then.
          */
          delete ev;
          DBUG_RETURN(OK_CONTINUE);
        }
      }
    }
    else if (type == FORMAT_DESCRIPTION_EVENT)
    {
      /*
        This could be an fake Format_description_log_event that server
        (5.0+) automatically sends to a slave on connect, before sending
        a first event at the requested position.  If this is the case,
        don't increment old_off. Real Format_description_log_event always
        starts from BIN_LOG_HEADER_SIZE position.
      */
      if (old_off != BIN_LOG_HEADER_SIZE)
        *len= 1;         // fake event, don't increment old_off
    }
    Exit_status retval= process_event(print_event_info, ev, old_off, logname);
    if (retval != OK_CONTINUE)
      DBUG_RETURN(retval);
  }
  else
  {
    Load_log_event *le= (Load_log_event*)ev;
    const char *old_fname= le->fname;
    uint old_len= le->fname_len;
    File file;
    Exit_status retval;
    char fname[FN_REFLEN+1];

    if ((file= load_processor.prepare_new_file_for_old_format(le,fname)) < 0)
    {
      DBUG_RETURN(ERROR_STOP);
    }

    retval= process_event(print_event_info, ev, old_off, logname);
    if (retval != OK_CONTINUE)
    {
      my_close(file,MYF(MY_WME));
      DBUG_RETURN(retval);
    }
    retval= load_processor.load_old_format_file(net,old_fname,old_len,file);
    my_close(file,MYF(MY_WME));
    if (retval != OK_CONTINUE)
      DBUG_RETURN(retval);
  }

  DBUG_RETURN(OK_CONTINUE);
}


static char out_file_name[FN_REFLEN + 1];

static Exit_status handle_event_raw_mode(PRINT_EVENT_INFO *print_event_info,
                                         ulong *len,
                                         const char* logname, uint logname_len)
{
  const char *error_msg;
  const unsigned char *read_pos= mysql->net.read_pos + 1;
  Log_event_type type;
  DBUG_ENTER("handle_event_raw_mode");
  DBUG_ASSERT(opt_raw_mode && remote_opt);

  type= (Log_event_type) read_pos[EVENT_TYPE_OFFSET];

  if (type == HEARTBEAT_LOG_EVENT)
    DBUG_RETURN(OK_CONTINUE);

  if (type == ROTATE_EVENT || type == FORMAT_DESCRIPTION_EVENT)
  {
    Log_event *ev;
    if (!(ev= Log_event::read_log_event((const char*) read_pos ,
                                        *len - 1, &error_msg,
                                        glob_description_event,
                                        opt_verify_binlog_checksum)))
    {
      error("Could not construct %s event object: %s",
            type == ROTATE_EVENT ? "rotate" : "format description", error_msg);
      DBUG_RETURN(ERROR_STOP);
    }
    /*
      If reading from a remote host, ensure the temp_buf for the
      Log_event class is pointing to the incoming stream.
    */
    ev->register_temp_buf((char *) read_pos, FALSE);

    if (type == ROTATE_EVENT)
    {
      Exit_status ret_val= OK_CONTINUE;
      Rotate_log_event *rev= (Rotate_log_event *)ev;
      char *pe= strmake(out_file_name, output_prefix, sizeof(out_file_name)-1);
      strmake(pe, rev->new_log_ident, sizeof(out_file_name) - (pe-out_file_name));

      /*
        If this is a fake Rotate event, and not about our log, we can stop
        transfer. If this a real Rotate event (so it's not about our log,
        it's in our log describing the next log), we print it (because it's
        part of our log) and then we will stop when we receive the fake one
        soon.
      */
      if (rev->when == 0)
      {
        if (!to_last_remote_log)
        {
          if ((rev->ident_len != logname_len) ||
              memcmp(rev->new_log_ident, logname, logname_len))
          {
            ret_val= OK_EOF;
          }
          /*
            Otherwise, this is a fake Rotate for our log, at the very
            beginning for sure. Skip it, because it was not in the original
            log. If we are running with to_last_remote_log, we print it,
            because it serves as a useful marker between binlogs then.
          */
        }
        *len= 1; // fake Rotate, so don't increment old_off
        ev->temp_buf= 0;
        delete ev;
        DBUG_RETURN(ret_val);
      }
      ev->temp_buf= 0;
      delete ev;
    }
    else /* if (type == FORMAT_DESCRIPTION_EVENT) */
    {
      DBUG_ASSERT(type == FORMAT_DESCRIPTION_EVENT);

      if (result_file)
        my_fclose(result_file, MYF(0));

      if (!(result_file= my_fopen(out_file_name,
                                  O_WRONLY | O_BINARY, MYF(MY_WME))))
      {
        error("Could not create output log file: %s", out_file_name);
        DBUG_RETURN(ERROR_STOP);
      }
      /* TODO - add write error simulation here */

      if (my_fwrite(result_file, (const uchar *) BINLOG_MAGIC,
                    BIN_LOG_HEADER_SIZE, MYF(MY_NABP)))
      {
        error("Could not write into log file '%s'", out_file_name);
        DBUG_RETURN(ERROR_STOP);
      }
      print_event_info->file= result_file;

      delete glob_description_event;
      glob_description_event= (Format_description_log_event*) ev;
      print_event_info->common_header_len=
        glob_description_event->common_header_len;
      ev->temp_buf= 0;
      /* We do not want to delete the event here. */
    }
  }

  if (my_fwrite(result_file, read_pos, *len - 1, MYF(MY_NABP)))
  {
    error("Could not write into log file '%s'", out_file_name);
    DBUG_RETURN(ERROR_STOP);
  }

  DBUG_RETURN(OK_CONTINUE);
}


/**
  Requests binlog dump from a remote server and prints the events it
  receives.

  @param[in,out] print_event_info Parameters and context state
  determining how to print.
  @param[in] logname Name of input binlog.

  @retval ERROR_STOP An error occurred - the program should terminate.
  @retval OK_CONTINUE No error, the program should continue.
  @retval OK_STOP No error, but the end of the specified range of
  events to process has been reached and the program should terminate.
*/
static Exit_status dump_remote_log_entries(PRINT_EVENT_INFO *print_event_info,
                                           const char* logname)

{
  uchar buf[128];
  ulong len;
  uint logname_len;
  NET* net;
  my_off_t old_off= start_position_mot;
  Exit_status retval= OK_CONTINUE;
  short binlog_flags = 0; 
  ulong slave_id;
  DBUG_ENTER("dump_remote_log_entries");

  /*
    Even if we already read one binlog (case of >=2 binlogs on command line),
    we cannot re-use the same connection as before, because it is now dead
    (COM_BINLOG_DUMP kills the thread when it finishes).
  */
  if ((retval= safe_connect()) != OK_CONTINUE)
    DBUG_RETURN(retval);
  net= &mysql->net;

  if ((retval= check_master_version()) != OK_CONTINUE)
    DBUG_RETURN(retval);

  /*
    COM_BINLOG_DUMP accepts only 4 bytes for the position, so we are forced to
    cast to uint32.
  */
  DBUG_ASSERT(start_position <= UINT_MAX32);
  int4store(buf, (uint32)start_position);
  if (!opt_skip_annotate_row_events)
    binlog_flags|= BINLOG_SEND_ANNOTATE_ROWS_EVENT;
  if (!opt_stop_never)
    binlog_flags|= BINLOG_DUMP_NON_BLOCK;

  int2store(buf + BIN_LOG_HEADER_SIZE, binlog_flags);

  size_t tlen = strlen(logname);
  if (tlen > sizeof(buf) - 10)
  {
    error("Log name too long.");
    DBUG_RETURN(ERROR_STOP);
  }
  logname_len = (uint) tlen;
  if (opt_stop_never)
  {
    DBUG_ASSERT(to_last_remote_log);
    slave_id= (opt_stop_never_slave_server_id == 0) ?
                1 : opt_stop_never_slave_server_id;
  }
  else
    slave_id= 0;
  int4store(buf + 6, slave_id);
  memcpy(buf + 10, logname, logname_len);
  if (simple_command(mysql, COM_BINLOG_DUMP, buf, logname_len + 10, 1))
  {
    error("Got fatal error sending the log dump command.");
    DBUG_RETURN(ERROR_STOP);
  }

  for (;;)
  {
    len= cli_safe_read(mysql);
    if (len == packet_error)
    {
      error("Got error reading packet from server: %s", mysql_error(mysql));
      DBUG_RETURN(ERROR_STOP);
    }
    if (len < 8 && net->read_pos[0] == 254)
      break; // end of data
    DBUG_PRINT("info",( "len: %lu  net->read_pos[5]: %d\n",
			len, net->read_pos[5]));
    if (opt_raw_mode)
    {
      retval= handle_event_raw_mode(print_event_info, &len,
                                    logname, logname_len);
    }
    else
    {
      retval= handle_event_text_mode(print_event_info, &len,
                                     logname, logname_len, old_off);
    }
    if (retval != OK_CONTINUE)
    {
      if (retval == OK_EOF)
        break;
      DBUG_RETURN(retval);
    }

    /*
      Let's adjust offset for remote log as for local log to produce
      similar text and to have --stop-position to work identically.
    */
    old_off+= len-1;
  }

  DBUG_RETURN(OK_CONTINUE);
}


/**
  Reads the @c Format_description_log_event from the beginning of a
  local input file.

  The @c Format_description_log_event is only read if it is outside
  the range specified with @c --start-position; otherwise, it will be
  seen later.  If this is an old binlog, a fake @c
  Format_description_event is created.  This also prints a @c
  Format_description_log_event to the output, unless we reach the
  --start-position range.  In this case, it is assumed that a @c
  Format_description_log_event will be found when reading events the
  usual way.

  @param file The file to which a @c Format_description_log_event will
  be printed.

  @param[in,out] print_event_info Parameters and context state
  determining how to print.

  @param[in] logname Name of input binlog.

  @retval ERROR_STOP An error occurred - the program should terminate.
  @retval OK_CONTINUE No error, the program should continue.
  @retval OK_STOP No error, but the end of the specified range of
  events to process has been reached and the program should terminate.
*/
static Exit_status check_header(IO_CACHE* file,
                                PRINT_EVENT_INFO *print_event_info,
                                const char* logname)
{
  uchar header[BIN_LOG_HEADER_SIZE];
  uchar buf[PROBE_HEADER_LEN];
  my_off_t tmp_pos, pos;
  MY_STAT my_file_stat;

  delete glob_description_event;
  if (!(glob_description_event= new Format_description_log_event(3)))
  {
    error("Failed creating Format_description_log_event; out of memory?");
    return ERROR_STOP;
  }

  pos= my_b_tell(file);

  /* fstat the file to check if the file is a regular file. */
  if (my_fstat(file->file, &my_file_stat, MYF(0)) == -1)
  {
    error("Unable to stat the file.");
    return ERROR_STOP;
  }
  if ((my_file_stat.st_mode & S_IFMT) == S_IFREG)
    my_b_seek(file, (my_off_t)0);

  if (my_b_read(file, header, sizeof(header)))
  {
    error("Failed reading header; probably an empty file.");
    return ERROR_STOP;
  }
  if (memcmp(header, BINLOG_MAGIC, sizeof(header)))
  {
    error("File is not a binary log file.");
    return ERROR_STOP;
  }

  /*
    Imagine we are running with --start-position=1000. We still need
    to know the binlog format's. So we still need to find, if there is
    one, the Format_desc event, or to know if this is a 3.23
    binlog. So we need to first read the first events of the log,
    those around offset 4.  Even if we are reading a 3.23 binlog from
    the start (no --start-position): we need to know the header length
    (which is 13 in 3.23, 19 in 4.x) to be able to successfully print
    the first event (Start_log_event_v3). So even in this case, we
    need to "probe" the first bytes of the log *before* we do a real
    read_log_event(). Because read_log_event() needs to know the
    header's length to work fine.
  */
  for(;;)
  {
    tmp_pos= my_b_tell(file); /* should be 4 the first time */
    if (my_b_read(file, buf, sizeof(buf)))
    {
      if (file->error)
      {
        error("Could not read entry at offset %llu: "
              "Error in log format or read error.", (ulonglong)tmp_pos);
        return ERROR_STOP;
      }
      /*
        Otherwise this is just EOF : this log currently contains 0-2
        events.  Maybe it's going to be filled in the next
        milliseconds; then we are going to have a problem if this a
        3.23 log (imagine we are locally reading a 3.23 binlog which
        is being written presently): we won't know it in
        read_log_event() and will fail().  Similar problems could
        happen with hot relay logs if --start-position is used (but a
        --start-position which is posterior to the current size of the log).
        These are rare problems anyway (reading a hot log + when we
        read the first events there are not all there yet + when we
        read a bit later there are more events + using a strange
        --start-position).
      */
      break;
    }
    else
    {
      DBUG_PRINT("info",("buf[EVENT_TYPE_OFFSET=%d]=%d",
                         EVENT_TYPE_OFFSET, buf[EVENT_TYPE_OFFSET]));
      /* always test for a Start_v3, even if no --start-position */
      if (buf[EVENT_TYPE_OFFSET] == START_EVENT_V3)
      {
        /* This is 3.23 or 4.x */
        if (uint4korr(buf + EVENT_LEN_OFFSET) < 
            (LOG_EVENT_MINIMAL_HEADER_LEN + START_V3_HEADER_LEN))
        {
          /* This is 3.23 (format 1) */
          delete glob_description_event;
          if (!(glob_description_event= new Format_description_log_event(1)))
          {
            error("Failed creating Format_description_log_event; "
                  "out of memory?");
            return ERROR_STOP;
          }
        }
        break;
      }
      else if (tmp_pos >= start_position)
        break;
      else if (buf[EVENT_TYPE_OFFSET] == FORMAT_DESCRIPTION_EVENT)
      {
        /* This is 5.0 */
        Format_description_log_event *new_description_event;
        my_b_seek(file, tmp_pos); /* seek back to event's start */
        if (!(new_description_event= (Format_description_log_event*) 
              Log_event::read_log_event(file, glob_description_event,
                                        opt_verify_binlog_checksum)))
          /* EOF can't be hit here normally, so it's a real error */
        {
          error("Could not read a Format_description_log_event event at "
                "offset %llu; this could be a log format error or read error.",
                (ulonglong)tmp_pos);
          return ERROR_STOP;
        }
        if (opt_base64_output_mode == BASE64_OUTPUT_AUTO
            || opt_base64_output_mode == BASE64_OUTPUT_ALWAYS)
        {
          /*
            process_event will delete *description_event and set it to
            the new one, so we should not do it ourselves in this
            case.
          */
          Exit_status retval= process_event(print_event_info,
                                            new_description_event, tmp_pos,
                                            logname);
          if (retval != OK_CONTINUE)
            return retval;
        }
        else
        {
          delete glob_description_event;
          glob_description_event= new_description_event;
        }
        DBUG_PRINT("info",("Setting description_event"));
      }
      else if (buf[EVENT_TYPE_OFFSET] == ROTATE_EVENT)
      {
        Log_event *ev;
        my_b_seek(file, tmp_pos); /* seek back to event's start */
        if (!(ev= Log_event::read_log_event(file, glob_description_event,
                                            opt_verify_binlog_checksum)))
        {
          /* EOF can't be hit here normally, so it's a real error */
          error("Could not read a Rotate_log_event event at offset %llu;"
                " this could be a log format error or read error.",
                (ulonglong)tmp_pos);
          return ERROR_STOP;
        }
        delete ev;
      }
      else
        break;
    }
  }
  my_b_seek(file, pos);
  return OK_CONTINUE;
}


/**
  Reads a local binlog and prints the events it sees.

  @param[in] logname Name of input binlog.

  @param[in,out] print_event_info Parameters and context state
  determining how to print.

  @retval ERROR_STOP An error occurred - the program should terminate.
  @retval OK_CONTINUE No error, the program should continue.
  @retval OK_STOP No error, but the end of the specified range of
  events to process has been reached and the program should terminate.
*/
static Exit_status dump_local_log_entries(PRINT_EVENT_INFO *print_event_info,
                                          const char* logname)
{
  File fd = -1;
  IO_CACHE cache,*file= &cache;
  uchar tmp_buff[BIN_LOG_HEADER_SIZE];
  Exit_status retval= OK_CONTINUE;

  if (logname && strcmp(logname, "-") != 0)
  {
    /* read from normal file */
    if ((fd = my_open(logname, O_RDONLY | O_BINARY, MYF(MY_WME))) < 0)
      return ERROR_STOP;
    if (init_io_cache(file, fd, 0, READ_CACHE, start_position_mot, 0,
		      MYF(MY_WME | MY_NABP)))
    {
      my_close(fd, MYF(MY_WME));
      return ERROR_STOP;
    }
    if ((retval= check_header(file, print_event_info, logname)) != OK_CONTINUE)
      goto end;
  }
  else
  {
    /* read from stdin */
    /*
      Windows opens stdin in text mode by default. Certain characters
      such as CTRL-Z are interpreted as events and the read() method
      will stop. CTRL-Z is the EOF marker in Windows. to get past this
      you have to open stdin in binary mode. Setmode() is used to set
      stdin in binary mode. Errors on setting this mode result in 
      halting the function and printing an error message to stderr.
    */
#if defined (__WIN__) || defined(_WIN64)
    if (_setmode(fileno(stdin), O_BINARY) == -1)
    {
      error("Could not set binary mode on stdin.");
      return ERROR_STOP;
    }
#endif 
    if (init_io_cache(file, my_fileno(stdin), 0, READ_CACHE, (my_off_t) 0,
		      0, MYF(MY_WME | MY_NABP | MY_DONT_CHECK_FILESIZE)))
    {
      error("Failed to init IO cache.");
      return ERROR_STOP;
    }
    if ((retval= check_header(file, print_event_info, logname)) != OK_CONTINUE)
      goto end;
    if (start_position)
    {
      /* skip 'start_position' characters from stdin */
      uchar buff[IO_SIZE];
      my_off_t length,tmp;
      for (length= start_position_mot ; length > 0 ; length-=tmp)
      {
	tmp= MY_MIN(length,sizeof(buff));
	if (my_b_read(file, buff, (uint) tmp))
        {
          error("Failed reading from file.");
          goto err;
        }
      }
    }
  }

  if (!glob_description_event || !glob_description_event->is_valid())
  {
    error("Invalid Format_description log event; could be out of memory.");
    goto err;
  }

  if (!start_position && my_b_read(file, tmp_buff, BIN_LOG_HEADER_SIZE))
  {
    error("Failed reading from file.");
    goto err;
  }
  for (;;)
  {
    char llbuff[21];
    my_off_t old_off = my_b_tell(file);

    Log_event* ev = Log_event::read_log_event(file, glob_description_event,
                                              opt_verify_binlog_checksum);
    if (!ev)
    {
      /*
        if binlog wasn't closed properly ("in use" flag is set) don't complain
        about a corruption, but treat it as EOF and move to the next binlog.
      */
      if (glob_description_event->flags & LOG_EVENT_BINLOG_IN_USE_F)
        file->error= 0;
      else if (file->error)
      {
        error("Could not read entry at offset %s: "
              "Error in log format or read error.",
              llstr(old_off,llbuff));
        goto err;
      }
      // file->error == 0 means EOF, that's OK, we break in this case
      goto end;
    }
    if ((retval= process_event(print_event_info, ev, old_off, logname)) !=
        OK_CONTINUE)
      goto end;
  }

  /* NOTREACHED */

err:
  retval= ERROR_STOP;

end:
  if (fd >= 0)
    my_close(fd, MYF(MY_WME));
  /*
    Since the end_io_cache() writes to the
    file errors may happen.
   */
  if (end_io_cache(file))
    retval= ERROR_STOP;

  return retval;
}


int main(int argc, char** argv)
{
  char **defaults_argv;
  Exit_status retval= OK_CONTINUE;
  ulonglong save_stop_position;
  MY_INIT(argv[0]);
  DBUG_ENTER("main");
  DBUG_PROCESS(argv[0]);

  my_init_time(); // for time functions
  tzset(); // set tzname

  load_defaults_or_exit("my", load_groups, &argc, &argv);
  defaults_argv= argv;

  if (!(binlog_filter= new Rpl_filter))
  {
    error("Failed to create Rpl_filter");
    goto err;
  }

  parse_args(&argc, (char***)&argv);

  if (!argc || opt_version)
  {
    if (!opt_version)
    {
      usage();
      retval= ERROR_STOP;
    }
    goto err;
  }

  if (opt_base64_output_mode == BASE64_OUTPUT_UNSPEC)
    opt_base64_output_mode= BASE64_OUTPUT_AUTO;

  my_set_max_open_files(open_files_limit);

  if (opt_flashback)
  {
    my_init_dynamic_array(&binlog_events, sizeof(LEX_STRING), 1024, 1024,
                          MYF(0));
    my_init_dynamic_array(&events_in_stmt, sizeof(Rows_log_event*), 1024, 1024,
                          MYF(0));
  }
  if (opt_stop_never)
    to_last_remote_log= TRUE;

  if (opt_raw_mode)
  {
    if (!remote_opt)
    {
      error("The --raw mode only works with --read-from-remote-server");
      exit(1);
    }
    if (one_database)
      warning("The --database option is ignored in raw mode");

    if (stop_position != (ulonglong)(~(my_off_t)0))
      warning("The --stop-position option is ignored in raw mode");

    if (stop_datetime != MY_TIME_T_MAX)
      warning("The --stop-datetime option is ignored in raw mode");
    result_file= 0;
    if (result_file_name)
      output_prefix= result_file_name;
  }
  else
  {
    if (result_file_name)
    {
      if (!(result_file= my_fopen(result_file_name,
                                  O_WRONLY | O_BINARY, MYF(MY_WME))))
      {
        error("Could not create log file '%s'", result_file_name);
        exit(1);
      }
    }
    else
      result_file= stdout;
  }

  MY_TMPDIR tmpdir;
  tmpdir.list= 0;
  if (!dirname_for_local_load)
  {
    if (init_tmpdir(&tmpdir, 0))
    {
      retval= ERROR_STOP;
      goto err;
    }
    dirname_for_local_load= my_strdup(my_tmpdir(&tmpdir), MY_WME);
  }

  if (load_processor.init())
  {
    retval= ERROR_STOP;
    goto err;
  }
  if (dirname_for_local_load)
    load_processor.init_by_dir_name(dirname_for_local_load);
  else
    load_processor.init_by_cur_dir();

  if (!opt_raw_mode)
  {
    fprintf(result_file, "/*!50530 SET @@SESSION.PSEUDO_SLAVE_MODE=1*/;\n");

    fprintf(result_file,
	    "/*!40019 SET @@session.max_insert_delayed_threads=0*/;\n");

    if (disable_log_bin)
      fprintf(result_file,
              "/*!32316 SET @OLD_SQL_LOG_BIN=@@SQL_LOG_BIN, SQL_LOG_BIN=0*/;\n");

    /*
      In mysqlbinlog|mysql, don't want mysql to be disconnected after each
      transaction (which would be the case with GLOBAL.COMPLETION_TYPE==2).
    */
    fprintf(result_file,
            "/*!50003 SET @OLD_COMPLETION_TYPE=@@COMPLETION_TYPE,"
            "COMPLETION_TYPE=0*/;\n");

    if (charset)
      fprintf(result_file,
              "\n/*!40101 SET @OLD_CHARACTER_SET_CLIENT=@@CHARACTER_SET_CLIENT */;"
              "\n/*!40101 SET @OLD_CHARACTER_SET_RESULTS=@@CHARACTER_SET_RESULTS */;"
              "\n/*!40101 SET @OLD_COLLATION_CONNECTION=@@COLLATION_CONNECTION */;"  
              "\n/*!40101 SET NAMES %s */;\n", charset);
  }

  for (save_stop_position= stop_position, stop_position= ~(my_off_t)0 ;
       (--argc >= 0) ; )
  {
    if (argc == 0) // last log, --stop-position applies
      stop_position= save_stop_position;
    if ((retval= dump_log_entries(*argv++)) != OK_CONTINUE)
      break;

    // For next log, --start-position does not apply
    start_position= BIN_LOG_HEADER_SIZE;
  }

  /*
    If enable flashback, need to print the events from the end to the
    beginning
  */
  if (opt_flashback && retval != ERROR_STOP)
  {
    for (uint i= binlog_events.elements; i > 0; --i)
    {
      LEX_STRING *event_str= dynamic_element(&binlog_events, i - 1,
                                             LEX_STRING*);
      fprintf(result_file, "%s", event_str->str);
      my_free(event_str->str);
    }
    fprintf(result_file, "COMMIT\n/*!*/;\n");
    delete_dynamic(&binlog_events);
    delete_dynamic(&events_in_stmt);
  }

  /* Set delimiter back to semicolon */
  if (retval != ERROR_STOP)
  {
    if (!stop_event_string.is_empty())
      fprintf(result_file, "%s", stop_event_string.ptr());
    if (!opt_raw_mode && opt_flashback)
      fprintf(result_file, "DELIMITER ;\n");
  }

  if (retval != ERROR_STOP && !opt_raw_mode)
  {
    /*
      Issue a ROLLBACK in case the last printed binlog was crashed and had half
      of transaction.
    */
    fprintf(result_file,
            "# End of log file\nROLLBACK /* added by mysqlbinlog */;\n"
            "/*!50003 SET COMPLETION_TYPE=@OLD_COMPLETION_TYPE*/;\n");
    if (disable_log_bin)
      fprintf(result_file, "/*!32316 SET SQL_LOG_BIN=@OLD_SQL_LOG_BIN*/;\n");

    if (charset)
      fprintf(result_file,
              "/*!40101 SET CHARACTER_SET_CLIENT=@OLD_CHARACTER_SET_CLIENT */;\n"
              "/*!40101 SET CHARACTER_SET_RESULTS=@OLD_CHARACTER_SET_RESULTS */;\n"
              "/*!40101 SET COLLATION_CONNECTION=@OLD_COLLATION_CONNECTION */;\n");

    fprintf(result_file, "/*!50530 SET @@SESSION.PSEUDO_SLAVE_MODE=0*/;\n");
  }

  if (tmpdir.list)
    free_tmpdir(&tmpdir);
  if (result_file && result_file != stdout)
    my_fclose(result_file, MYF(0));
  cleanup();
  free_annotate_event();
  free_defaults(defaults_argv);
  my_free_open_file_info();
  load_processor.destroy();
  mysql_server_end();
  /* We cannot free DBUG, it is used in global destructors after exit(). */
  my_end(my_end_arg | MY_DONT_FREE_DBUG);

  exit(retval == ERROR_STOP ? 1 : 0);
  /* Keep compilers happy. */
  DBUG_RETURN(retval == ERROR_STOP ? 1 : 0);

err:
  cleanup();
  free_defaults(defaults_argv);
  my_end(my_end_arg);
  exit(retval == ERROR_STOP ? 1 : 0);
  DBUG_RETURN(retval == ERROR_STOP ? 1 : 0);
}

uint e_key_get_latest_version_func(uint) { return 1; }
uint e_key_get_func(uint, uint, uchar*, uint*) { return 1; }
uint e_ctx_size_func(uint, uint) { return 1; }
int e_ctx_init_func(void *, const uchar*, uint, const uchar*, uint,
                    int, uint, uint) { return 1; }
int e_ctx_update_func(void *, const uchar*, uint, uchar*, uint*) { return 1; }
int e_ctx_finish_func(void *, uchar*, uint*) { return 1; }
uint e_encrypted_length_func(uint, uint, uint) { return 1; }

struct encryption_service_st encryption_handler=
{
  e_key_get_latest_version_func,
  e_key_get_func,
  e_ctx_size_func,
  e_ctx_init_func,
  e_ctx_update_func,
  e_ctx_finish_func,
  e_encrypted_length_func
};

/*
  We must include this here as it's compiled with different options for
  the server
*/

#include "rpl_tblmap.cc"
#undef TABLE
#include "my_decimal.h"
#include "decimal.c"
#include "my_decimal.cc"
#include "../sql-common/my_time.c"
#include "password.c"
#include "log_event.cc"
#include "log_event_old.cc"
#include "rpl_utility.cc"
#include "sql_string.cc"
#include "sql_list.cc"
#include "rpl_filter.cc"
#include "compat56.cc"
