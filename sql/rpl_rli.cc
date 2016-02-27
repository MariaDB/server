/* Copyright (c) 2006, 2013, Oracle and/or its affiliates.
   Copyright (c) 2010, 2013, Monty Program Ab

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software Foundation,
   51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA */

#include <my_global.h>
#include "sql_priv.h"
#include "unireg.h"                             // HAVE_*
#include "rpl_mi.h"
#include "rpl_rli.h"
#include "sql_base.h"                        // close_thread_tables
#include <my_dir.h>    // For MY_STAT
#include "sql_repl.h"  // For check_binlog_magic
#include "log_event.h" // Format_description_log_event, Log_event,
                       // FORMAT_DESCRIPTION_LOG_EVENT, ROTATE_EVENT,
                       // PREFIX_SQL_LOAD
#include "rpl_utility.h"
#include "transaction.h"
#include "sql_parse.h"                          // end_trans, ROLLBACK
#include <mysql/plugin.h>
#include <mysql/service_thd_wait.h>

static int count_relay_log_space(Relay_log_info* rli);

/**
   Current replication state (hash of last GTID executed, per replication
   domain).
*/
rpl_slave_state *rpl_global_gtid_slave_state;
/* Object used for MASTER_GTID_WAIT(). */
gtid_waiting rpl_global_gtid_waiting;


// Defined in slave.cc
int init_intvar_from_file(int* var, IO_CACHE* f, int default_val);
int init_strvar_from_file(char *var, int max_size, IO_CACHE *f,
			  const char *default_val);

Relay_log_info::Relay_log_info(bool is_slave_recovery)
  :Slave_reporting_capability("SQL"),
   no_storage(FALSE), replicate_same_server_id(::replicate_same_server_id),
   info_fd(-1), cur_log_fd(-1), relay_log(&sync_relaylog_period),
   sync_counter(0), is_relay_log_recovery(is_slave_recovery),
   save_temporary_tables(0), mi(0),
   inuse_relaylog_list(0), last_inuse_relaylog(0),
   cur_log_old_open_count(0), group_relay_log_pos(0), 
   event_relay_log_pos(0),
#if HAVE_valgrind
   is_fake(FALSE),
#endif
   group_master_log_pos(0), log_space_total(0), ignore_log_space_limit(0),
   last_master_timestamp(0), sql_thread_caught_up(true), slave_skip_counter(0),
   abort_pos_wait(0), slave_run_id(0), sql_driver_thd(),
   gtid_skip_flag(GTID_SKIP_NOT), inited(0), abort_slave(0), stop_for_until(0),
   slave_running(MYSQL_SLAVE_NOT_RUN), until_condition(UNTIL_NONE),
   until_log_pos(0), retried_trans(0), executed_entries(0),
   m_flags(0)
{
  DBUG_ENTER("Relay_log_info::Relay_log_info");

  relay_log.is_relay_log= TRUE;
#ifdef HAVE_PSI_INTERFACE
  relay_log.set_psi_keys(key_RELAYLOG_LOCK_index,
                         key_RELAYLOG_update_cond,
                         key_file_relaylog,
                         key_file_relaylog_index,
                         key_RELAYLOG_COND_queue_busy);
#endif

  group_relay_log_name[0]= event_relay_log_name[0]=
    group_master_log_name[0]= 0;
  until_log_name[0]= ign_master_log_name_end[0]= 0;
  max_relay_log_size= global_system_variables.max_relay_log_size;
  bzero((char*) &info_file, sizeof(info_file));
  bzero((char*) &cache_buf, sizeof(cache_buf));
  mysql_mutex_init(key_relay_log_info_run_lock, &run_lock, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_relay_log_info_data_lock,
                   &data_lock, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_relay_log_info_log_space_lock,
                   &log_space_lock, MY_MUTEX_INIT_FAST);
  mysql_cond_init(key_relay_log_info_data_cond, &data_cond, NULL);
  mysql_cond_init(key_relay_log_info_start_cond, &start_cond, NULL);
  mysql_cond_init(key_relay_log_info_stop_cond, &stop_cond, NULL);
  mysql_cond_init(key_relay_log_info_log_space_cond, &log_space_cond, NULL);
  relay_log.init_pthread_objects();
  DBUG_VOID_RETURN;
}


Relay_log_info::~Relay_log_info()
{
  DBUG_ENTER("Relay_log_info::~Relay_log_info");

  reset_inuse_relaylog();
  mysql_mutex_destroy(&run_lock);
  mysql_mutex_destroy(&data_lock);
  mysql_mutex_destroy(&log_space_lock);
  mysql_cond_destroy(&data_cond);
  mysql_cond_destroy(&start_cond);
  mysql_cond_destroy(&stop_cond);
  mysql_cond_destroy(&log_space_cond);
  relay_log.cleanup();
  DBUG_VOID_RETURN;
}


int init_relay_log_info(Relay_log_info* rli,
			const char* info_fname)
{
  char fname[FN_REFLEN+128];
  int info_fd;
  const char* msg = 0;
  int error = 0;
  DBUG_ENTER("init_relay_log_info");
  DBUG_ASSERT(!rli->no_storage);         // Don't init if there is no storage

  if (rli->inited)                       // Set if this function called
    DBUG_RETURN(0);
  fn_format(fname, info_fname, mysql_data_home, "", 4+32);
  mysql_mutex_lock(&rli->data_lock);
  info_fd = rli->info_fd;
  rli->cur_log_fd = -1;
  rli->slave_skip_counter=0;
  rli->abort_pos_wait=0;
  rli->log_space_limit= relay_log_space_limit;
  rli->log_space_total= 0;

  char pattern[FN_REFLEN];
  (void) my_realpath(pattern, slave_load_tmpdir, 0);
  if (fn_format(pattern, PREFIX_SQL_LOAD, pattern, "",
            MY_SAFE_PATH | MY_RETURN_REAL_PATH) == NullS)
  {
    mysql_mutex_unlock(&rli->data_lock);
    sql_print_error("Unable to use slave's temporary directory %s",
                    slave_load_tmpdir);
    DBUG_RETURN(1);
  }
  unpack_filename(rli->slave_patternload_file, pattern);
  rli->slave_patternload_file_size= strlen(rli->slave_patternload_file);

  /*
    The relay log will now be opened, as a SEQ_READ_APPEND IO_CACHE.
    Note that the I/O thread flushes it to disk after writing every
    event, in flush_master_info(mi, 1, ?).
  */

  {
    /* Reports an error and returns, if the --relay-log's path 
       is a directory.*/
    if (opt_relay_logname && 
        opt_relay_logname[strlen(opt_relay_logname) - 1] == FN_LIBCHAR)
    {
      mysql_mutex_unlock(&rli->data_lock);
      sql_print_error("Path '%s' is a directory name, please specify \
a file name for --relay-log option", opt_relay_logname);
      DBUG_RETURN(1);
    }

    /* Reports an error and returns, if the --relay-log-index's path 
       is a directory.*/
    if (opt_relaylog_index_name && 
        opt_relaylog_index_name[strlen(opt_relaylog_index_name) - 1] 
        == FN_LIBCHAR)
    {
      mysql_mutex_unlock(&rli->data_lock);
      sql_print_error("Path '%s' is a directory name, please specify \
a file name for --relay-log-index option", opt_relaylog_index_name);
      DBUG_RETURN(1);
    }

    char buf[FN_REFLEN];
    const char *ln;
    static bool name_warning_sent= 0;
    ln= rli->relay_log.generate_name(opt_relay_logname, "-relay-bin",
                                     1, buf);
    /* We send the warning only at startup, not after every RESET SLAVE */
    if (!opt_relay_logname && !opt_relaylog_index_name && !name_warning_sent &&
        !opt_bootstrap)
    {
      /*
        User didn't give us info to name the relay log index file.
        Picking `hostname`-relay-bin.index like we do, causes replication to
        fail if this slave's hostname is changed later. So, we would like to
        instead require a name. But as we don't want to break many existing
        setups, we only give warning, not error.
      */
      sql_print_warning("Neither --relay-log nor --relay-log-index were used;"
                        " so replication "
                        "may break when this MySQL server acts as a "
                        "slave and has his hostname changed!! Please "
                        "use '--log-basename=#' or '--relay-log=%s' to avoid "
                        "this problem.", ln);
      name_warning_sent= 1;
    }

    /* For multimaster, add connection name to relay log filenames */
    Master_info* mi= rli->mi;
    char buf_relay_logname[FN_REFLEN], buf_relaylog_index_name_buff[FN_REFLEN];
    char *buf_relaylog_index_name= opt_relaylog_index_name;

    create_logfile_name_with_suffix(buf_relay_logname,
                                    sizeof(buf_relay_logname),
                                    ln, 1, &mi->cmp_connection_name);
    ln= buf_relay_logname;

    if (opt_relaylog_index_name)
    {
      buf_relaylog_index_name= buf_relaylog_index_name_buff; 
      create_logfile_name_with_suffix(buf_relaylog_index_name_buff,
                                      sizeof(buf_relaylog_index_name_buff),
                                      opt_relaylog_index_name, 0,
                                      &mi->cmp_connection_name);
    }

    /*
      note, that if open() fails, we'll still have index file open
      but a destructor will take care of that
    */
    if (rli->relay_log.open_index_file(buf_relaylog_index_name, ln, TRUE) ||
        rli->relay_log.open(ln, LOG_BIN, 0, 0, SEQ_READ_APPEND,
                            mi->rli.max_relay_log_size, 1, TRUE))
    {
      mysql_mutex_unlock(&rli->data_lock);
      sql_print_error("Failed when trying to open logs for '%s' in init_relay_log_info(). Error: %M", ln, my_errno);
      DBUG_RETURN(1);
    }
  }

  /* if file does not exist */
  if (access(fname,F_OK))
  {
    /*
      If someone removed the file from underneath our feet, just close
      the old descriptor and re-create the old file
    */
    if (info_fd >= 0)
      mysql_file_close(info_fd, MYF(MY_WME));
    if ((info_fd= mysql_file_open(key_file_relay_log_info,
                                  fname, O_CREAT|O_RDWR|O_BINARY, MYF(MY_WME))) < 0)
    {
      sql_print_error("Failed to create a new relay log info file (\
file '%s', errno %d)", fname, my_errno);
      msg= current_thd->get_stmt_da()->message();
      goto err;
    }
    if (init_io_cache(&rli->info_file, info_fd, IO_SIZE*2, READ_CACHE, 0L,0,
                      MYF(MY_WME)))
    {
      sql_print_error("Failed to create a cache on relay log info file '%s'",
                      fname);
      msg= current_thd->get_stmt_da()->message();
      goto err;
    }

    /* Init relay log with first entry in the relay index file */
    if (init_relay_log_pos(rli,NullS,BIN_LOG_HEADER_SIZE,0 /* no data lock */,
                           &msg, 0))
    {
      sql_print_error("Failed to open the relay log 'FIRST' (relay_log_pos 4)");
      goto err;
    }
    rli->group_master_log_name[0]= 0;
    rli->group_master_log_pos= 0;
    rli->info_fd= info_fd;
  }
  else // file exists
  {
    if (info_fd >= 0)
      reinit_io_cache(&rli->info_file, READ_CACHE, 0L,0,0);
    else
    {
      int error=0;
      if ((info_fd= mysql_file_open(key_file_relay_log_info,
                                    fname, O_RDWR|O_BINARY, MYF(MY_WME))) < 0)
      {
        sql_print_error("\
Failed to open the existing relay log info file '%s' (errno %d)",
                        fname, my_errno);
        error= 1;
      }
      else if (init_io_cache(&rli->info_file, info_fd,
                             IO_SIZE*2, READ_CACHE, 0L, 0, MYF(MY_WME)))
      {
        sql_print_error("Failed to create a cache on relay log info file '%s'",
                        fname);
        error= 1;
      }
      if (error)
      {
        if (info_fd >= 0)
          mysql_file_close(info_fd, MYF(0));
        rli->info_fd= -1;
        rli->relay_log.close(LOG_CLOSE_INDEX | LOG_CLOSE_STOP_EVENT);
        mysql_mutex_unlock(&rli->data_lock);
        DBUG_RETURN(1);
      }
    }

    rli->info_fd = info_fd;
    int relay_log_pos, master_log_pos, lines;
    char *first_non_digit;
    /*
      In MySQL 5.6, there is a MASTER_DELAY option to CHANGE MASTER. This is
      not yet merged into MariaDB (as of 10.0.13). However, we detect the
      presense of the new option in relay-log.info, as a placeholder for
      possible later merge of the feature, and to maintain file format
      compatibility with MySQL 5.6+.
    */
    int dummy_sql_delay;

    /*
      Starting from MySQL 5.6.x, relay-log.info has a new format.
      Now, its first line contains the number of lines in the file.
      By reading this number we can determine which version our master.info
      comes from. We can't simply count the lines in the file, since
      versions before 5.6.x could generate files with more lines than
      needed. If first line doesn't contain a number, or if it
      contains a number less than LINES_IN_RELAY_LOG_INFO_WITH_DELAY,
      then the file is treated like a file from pre-5.6.x version.
      There is no ambiguity when reading an old master.info: before
      5.6.x, the first line contained the binlog's name, which is
      either empty or has an extension (contains a '.'), so can't be
      confused with an integer.

      So we're just reading first line and trying to figure which
      version is this.
    */

    /*
      The first row is temporarily stored in mi->master_log_name, if
      it is line count and not binlog name (new format) it will be
      overwritten by the second row later.
    */
    if (init_strvar_from_file(rli->group_relay_log_name,
                              sizeof(rli->group_relay_log_name),
                              &rli->info_file, ""))
    {
      msg="Error reading slave log configuration";
      goto err;
    }

    lines= strtoul(rli->group_relay_log_name, &first_non_digit, 10);

    if (rli->group_relay_log_name[0] != '\0' &&
        *first_non_digit == '\0' &&
        lines >= LINES_IN_RELAY_LOG_INFO_WITH_DELAY)
    {
      DBUG_PRINT("info", ("relay_log_info file is in new format."));
      /* Seems to be new format => read relay log name from next line */
      if (init_strvar_from_file(rli->group_relay_log_name,
                                sizeof(rli->group_relay_log_name),
                                &rli->info_file, ""))
      {
        msg="Error reading slave log configuration";
        goto err;
      }
    }
    else
      DBUG_PRINT("info", ("relay_log_info file is in old format."));

    if (init_intvar_from_file(&relay_log_pos,
                              &rli->info_file, BIN_LOG_HEADER_SIZE) ||
        init_strvar_from_file(rli->group_master_log_name,
                              sizeof(rli->group_master_log_name),
                              &rli->info_file, "") ||
        init_intvar_from_file(&master_log_pos, &rli->info_file, 0) ||
        (lines >= LINES_IN_RELAY_LOG_INFO_WITH_DELAY &&
         init_intvar_from_file(&dummy_sql_delay, &rli->info_file, 0)))
    {
      msg="Error reading slave log configuration";
      goto err;
    }

    strmake_buf(rli->event_relay_log_name,rli->group_relay_log_name);
    rli->group_relay_log_pos= rli->event_relay_log_pos= relay_log_pos;
    rli->group_master_log_pos= master_log_pos;

    if (rli->is_relay_log_recovery && init_recovery(rli->mi, &msg))
      goto err;

    rli->relay_log_state.load(rpl_global_gtid_slave_state);
    if (init_relay_log_pos(rli,
                           rli->group_relay_log_name,
                           rli->group_relay_log_pos,
                           0 /* no data lock*/,
                           &msg, 0))
    {
      sql_print_error("Failed to open the relay log '%s' (relay_log_pos %llu)",
                      rli->group_relay_log_name, rli->group_relay_log_pos);
      goto err;
    }
  }

  DBUG_PRINT("info", ("my_b_tell(rli->cur_log)=%llu rli->event_relay_log_pos=%llu",
                      my_b_tell(rli->cur_log), rli->event_relay_log_pos));
  DBUG_ASSERT(rli->event_relay_log_pos >= BIN_LOG_HEADER_SIZE);
  DBUG_ASSERT(my_b_tell(rli->cur_log) == rli->event_relay_log_pos);

  /*
    Now change the cache from READ to WRITE - must do this
    before flush_relay_log_info
  */
  reinit_io_cache(&rli->info_file, WRITE_CACHE,0L,0,1);
  if ((error= flush_relay_log_info(rli)))
  {
    msg= "Failed to flush relay log info file";
    goto err;
  }
  if (count_relay_log_space(rli))
  {
    msg="Error counting relay log space";
    goto err;
  }
  rli->inited= 1;
  mysql_mutex_unlock(&rli->data_lock);
  DBUG_RETURN(error);

err:
  sql_print_error("%s", msg);
  end_io_cache(&rli->info_file);
  if (info_fd >= 0)
    mysql_file_close(info_fd, MYF(0));
  rli->info_fd= -1;
  rli->relay_log.close(LOG_CLOSE_INDEX | LOG_CLOSE_STOP_EVENT);
  mysql_mutex_unlock(&rli->data_lock);
  DBUG_RETURN(1);
}


static inline int add_relay_log(Relay_log_info* rli,LOG_INFO* linfo)
{
  MY_STAT s;
  DBUG_ENTER("add_relay_log");
  if (!mysql_file_stat(key_file_relaylog,
                       linfo->log_file_name, &s, MYF(0)))
  {
    sql_print_error("log %s listed in the index, but failed to stat",
                    linfo->log_file_name);
    DBUG_RETURN(1);
  }
  rli->log_space_total += s.st_size;
  DBUG_PRINT("info",("log_space_total: %llu", rli->log_space_total));
  DBUG_RETURN(0);
}


static int count_relay_log_space(Relay_log_info* rli)
{
  LOG_INFO linfo;
  DBUG_ENTER("count_relay_log_space");
  rli->log_space_total= 0;
  if (rli->relay_log.find_log_pos(&linfo, NullS, 1))
  {
    sql_print_error("Could not find first log while counting relay log space");
    DBUG_RETURN(1);
  }
  do
  {
    if (add_relay_log(rli,&linfo))
      DBUG_RETURN(1);
  } while (!rli->relay_log.find_next_log(&linfo, 1));
  /*
     As we have counted everything, including what may have written in a
     preceding write, we must reset bytes_written, or we may count some space
     twice.
  */
  rli->relay_log.reset_bytes_written();
  DBUG_RETURN(0);
}


/*
   Reset UNTIL condition for Relay_log_info

   SYNOPSYS
    clear_until_condition()
      rli - Relay_log_info structure where UNTIL condition should be reset
 */

void Relay_log_info::clear_until_condition()
{
  DBUG_ENTER("clear_until_condition");

  until_condition= Relay_log_info::UNTIL_NONE;
  until_log_name[0]= 0;
  until_log_pos= 0;
  DBUG_VOID_RETURN;
}


/*
  Read the correct format description event for starting to replicate from
  a given position in a relay log file.
*/
Format_description_log_event *
read_relay_log_description_event(IO_CACHE *cur_log, ulonglong start_pos,
                                 const char **errmsg)
{
  Log_event *ev;
  Format_description_log_event *fdev;
  bool found= false;

  /*
    By default the relay log is in binlog format 3 (4.0).
    Even if format is 4, this will work enough to read the first event
    (Format_desc) (remember that format 4 is just lenghtened compared to format
    3; format 3 is a prefix of format 4).
  */
  fdev= new Format_description_log_event(3);

  while (!found)
  {
    Log_event_type typ;

    /*
      Read the possible Format_description_log_event; if position
      was 4, no need, it will be read naturally.
    */
    DBUG_PRINT("info",("looking for a Format_description_log_event"));

    if (my_b_tell(cur_log) >= start_pos)
      break;

    if (!(ev= Log_event::read_log_event(cur_log, 0, fdev,
                                        opt_slave_sql_verify_checksum)))
    {
      DBUG_PRINT("info",("could not read event, cur_log->error=%d",
                         cur_log->error));
      if (cur_log->error) /* not EOF */
      {
        *errmsg= "I/O error reading event at position 4";
        delete fdev;
        return NULL;
      }
      break;
    }
    typ= ev->get_type_code();
    if (typ == FORMAT_DESCRIPTION_EVENT)
    {
      Format_description_log_event *old= fdev;
      DBUG_PRINT("info",("found Format_description_log_event"));
      fdev= (Format_description_log_event*) ev;
      fdev->copy_crypto_data(old);
      delete old;

      /*
        As ev was returned by read_log_event, it has passed is_valid(), so
        my_malloc() in ctor worked, no need to check again.
      */
      /*
        Ok, we found a Format_description event. But it is not sure that this
        describes the whole relay log; indeed, one can have this sequence
        (starting from position 4):
        Format_desc (of slave)
        Rotate (of master)
        Format_desc (of master)
        So the Format_desc which really describes the rest of the relay log
        is the 3rd event (it can't be further than that, because we rotate
        the relay log when we queue a Rotate event from the master).
        But what describes the Rotate is the first Format_desc.
        So what we do is:
        go on searching for Format_description events, until you exceed the
        position (argument 'pos') or until you find another event than Rotate
        or Format_desc.
      */
    }
    else if (typ == START_ENCRYPTION_EVENT)
    {
      if (fdev->start_decryption((Start_encryption_log_event*) ev))
      {
        *errmsg= "Unable to set up decryption of binlog.";
        delete ev;
        delete fdev;
        return NULL;
      }
      delete ev;
    }
    else
    {
      DBUG_PRINT("info",("found event of another type=%d", typ));
      found= (typ != ROTATE_EVENT);
      delete ev;
    }
  }
  return fdev;
}


/*
  Open the given relay log

  SYNOPSIS
    init_relay_log_pos()
    rli                 Relay information (will be initialized)
    log                 Name of relay log file to read from. NULL = First log
    pos                 Position in relay log file
    need_data_lock      Set to 1 if this functions should do mutex locks
    errmsg              Store pointer to error message here
    look_for_description_event
                        1 if we should look for such an event. We only need
                        this when the SQL thread starts and opens an existing
                        relay log and has to execute it (possibly from an
                        offset >4); then we need to read the first event of
                        the relay log to be able to parse the events we have
                        to execute.

  DESCRIPTION
  - Close old open relay log files.
  - If we are using the same relay log as the running IO-thread, then set
    rli->cur_log to point to the same IO_CACHE entry.
  - If not, open the 'log' binary file.

  TODO
    - check proper initialization of group_master_log_name/group_master_log_pos

  RETURN VALUES
    0   ok
    1   error.  errmsg is set to point to the error message
*/

int init_relay_log_pos(Relay_log_info* rli,const char* log,
                       ulonglong pos, bool need_data_lock,
                       const char** errmsg,
                       bool look_for_description_event)
{
  DBUG_ENTER("init_relay_log_pos");
  DBUG_PRINT("info", ("pos: %lu", (ulong) pos));

  *errmsg=0;
  mysql_mutex_t *log_lock= rli->relay_log.get_log_lock();

  if (need_data_lock)
    mysql_mutex_lock(&rli->data_lock);

  /*
    Slave threads are not the only users of init_relay_log_pos(). CHANGE MASTER
    is, too, and init_slave() too; these 2 functions allocate a description
    event in init_relay_log_pos, which is not freed by the terminating SQL slave
    thread as that thread is not started by these functions. So we have to free
    the description_event here, in case, so that there is no memory leak in
    running, say, CHANGE MASTER.
  */
  delete rli->relay_log.description_event_for_exec;
  /*
    By default the relay log is in binlog format 3 (4.0).
    Even if format is 4, this will work enough to read the first event
    (Format_desc) (remember that format 4 is just lenghtened compared to format
    3; format 3 is a prefix of format 4).
  */
  rli->relay_log.description_event_for_exec= new
    Format_description_log_event(3);

  mysql_mutex_lock(log_lock);

  /* Close log file and free buffers if it's already open */
  if (rli->cur_log_fd >= 0)
  {
    end_io_cache(&rli->cache_buf);
    mysql_file_close(rli->cur_log_fd, MYF(MY_WME));
    rli->cur_log_fd = -1;
  }

  rli->group_relay_log_pos = rli->event_relay_log_pos = pos;
  rli->clear_flag(Relay_log_info::IN_STMT);
  rli->clear_flag(Relay_log_info::IN_TRANSACTION);

  /*
    Test to see if the previous run was with the skip of purging
    If yes, we do not purge when we restart
  */
  if (rli->relay_log.find_log_pos(&rli->linfo, NullS, 1))
  {
    *errmsg="Could not find first log during relay log initialization";
    goto err;
  }

  if (log && rli->relay_log.find_log_pos(&rli->linfo, log, 1))
  {
    *errmsg="Could not find target log during relay log initialization";
    goto err;
  }
  strmake_buf(rli->group_relay_log_name,rli->linfo.log_file_name);
  strmake_buf(rli->event_relay_log_name,rli->linfo.log_file_name);
  if (rli->relay_log.is_active(rli->linfo.log_file_name))
  {
    /*
      The IO thread is using this log file.
      In this case, we will use the same IO_CACHE pointer to
      read data as the IO thread is using to write data.
    */
    my_b_seek((rli->cur_log=rli->relay_log.get_log_file()), (off_t)0);
    if (check_binlog_magic(rli->cur_log,errmsg))
      goto err;
    rli->cur_log_old_open_count=rli->relay_log.get_open_count();
  }
  else
  {
    /*
      Open the relay log and set rli->cur_log to point at this one
    */
    if ((rli->cur_log_fd=open_binlog(&rli->cache_buf,
                                     rli->linfo.log_file_name,errmsg)) < 0)
      goto err;
    rli->cur_log = &rli->cache_buf;
  }
  /*
    In all cases, check_binlog_magic() has been called so we're at offset 4 for
    sure.
  */
  if (pos > BIN_LOG_HEADER_SIZE) /* If pos<=4, we stay at 4 */
  {
    if (look_for_description_event)
    {
      Format_description_log_event *fdev;
      if (!(fdev= read_relay_log_description_event(rli->cur_log, pos, errmsg)))
        goto err;
      delete rli->relay_log.description_event_for_exec;
      rli->relay_log.description_event_for_exec= fdev;
    }
    my_b_seek(rli->cur_log,(off_t)pos);
    DBUG_PRINT("info", ("my_b_tell(rli->cur_log)=%llu rli->event_relay_log_pos=%llu",
                        my_b_tell(rli->cur_log), rli->event_relay_log_pos));

  }

err:
  /*
    If we don't purge, we can't honour relay_log_space_limit ;
    silently discard it
  */
  if (!relay_log_purge)
    rli->log_space_limit= 0;
  mysql_cond_broadcast(&rli->data_cond);

  mysql_mutex_unlock(log_lock);

  if (need_data_lock)
    mysql_mutex_unlock(&rli->data_lock);
  if (!rli->relay_log.description_event_for_exec->is_valid() && !*errmsg)
    *errmsg= "Invalid Format_description log event; could be out of memory";

  DBUG_RETURN ((*errmsg) ? 1 : 0);
}


/*
  Waits until the SQL thread reaches (has executed up to) the
  log/position or timed out.

  SYNOPSIS
    wait_for_pos()
    thd             client thread that sent SELECT MASTER_POS_WAIT
    log_name        log name to wait for
    log_pos         position to wait for
    timeout         timeout in seconds before giving up waiting

  NOTES
    timeout is longlong whereas it should be ulong ; but this is
    to catch if the user submitted a negative timeout.

  RETURN VALUES
    -2          improper arguments (log_pos<0)
                or slave not running, or master info changed
                during the function's execution,
                or client thread killed. -2 is translated to NULL by caller
    -1          timed out
    >=0         number of log events the function had to wait
                before reaching the desired log/position
 */

int Relay_log_info::wait_for_pos(THD* thd, String* log_name,
                                    longlong log_pos,
                                    longlong timeout)
{
  int event_count = 0;
  ulong init_abort_pos_wait;
  int error=0;
  struct timespec abstime; // for timeout checking
  PSI_stage_info old_stage;
  DBUG_ENTER("Relay_log_info::wait_for_pos");

  if (!inited)
    DBUG_RETURN(-2);

  DBUG_PRINT("enter",("log_name: '%s'  log_pos: %lu  timeout: %lu",
                      log_name->c_ptr(), (ulong) log_pos, (ulong) timeout));

  set_timespec(abstime,timeout);
  mysql_mutex_lock(&data_lock);
  thd->ENTER_COND(&data_cond, &data_lock,
                  &stage_waiting_for_the_slave_thread_to_advance_position,
                  &old_stage);
  /*
     This function will abort when it notices that some CHANGE MASTER or
     RESET MASTER has changed the master info.
     To catch this, these commands modify abort_pos_wait ; We just monitor
     abort_pos_wait and see if it has changed.
     Why do we have this mechanism instead of simply monitoring slave_running
     in the loop (we do this too), as CHANGE MASTER/RESET SLAVE require that
     the SQL thread be stopped?
     This is becasue if someones does:
     STOP SLAVE;CHANGE MASTER/RESET SLAVE; START SLAVE;
     the change may happen very quickly and we may not notice that
     slave_running briefly switches between 1/0/1.
  */
  init_abort_pos_wait= abort_pos_wait;

  /*
    We'll need to
    handle all possible log names comparisons (e.g. 999 vs 1000).
    We use ulong for string->number conversion ; this is no
    stronger limitation than in find_uniq_filename in sql/log.cc
  */
  ulong log_name_extension;
  char log_name_tmp[FN_REFLEN]; //make a char[] from String

  strmake(log_name_tmp, log_name->ptr(), MY_MIN(log_name->length(), FN_REFLEN-1));

  char *p= fn_ext(log_name_tmp);
  char *p_end;
  if (!*p || log_pos<0)
  {
    error= -2; //means improper arguments
    goto err;
  }
  // Convert 0-3 to 4
  log_pos= MY_MAX(log_pos, BIN_LOG_HEADER_SIZE);
  /* p points to '.' */
  log_name_extension= strtoul(++p, &p_end, 10);
  /*
    p_end points to the first invalid character.
    If it equals to p, no digits were found, error.
    If it contains '\0' it means conversion went ok.
  */
  if (p_end==p || *p_end)
  {
    error= -2;
    goto err;
  }

  /* The "compare and wait" main loop */
  while (!thd->killed &&
         init_abort_pos_wait == abort_pos_wait &&
         slave_running)
  {
    bool pos_reached;
    int cmp_result= 0;

    DBUG_PRINT("info",
               ("init_abort_pos_wait: %ld  abort_pos_wait: %ld",
                init_abort_pos_wait, abort_pos_wait));
    DBUG_PRINT("info",("group_master_log_name: '%s'  pos: %lu",
                       group_master_log_name, (ulong) group_master_log_pos));

    /*
      group_master_log_name can be "", if we are just after a fresh
      replication start or after a CHANGE MASTER TO MASTER_HOST/PORT
      (before we have executed one Rotate event from the master) or
      (rare) if the user is doing a weird slave setup (see next
      paragraph).  If group_master_log_name is "", we assume we don't
      have enough info to do the comparison yet, so we just wait until
      more data. In this case master_log_pos is always 0 except if
      somebody (wrongly) sets this slave to be a slave of itself
      without using --replicate-same-server-id (an unsupported
      configuration which does nothing), then group_master_log_pos
      will grow and group_master_log_name will stay "".
    */
    if (*group_master_log_name)
    {
      char *basename= (group_master_log_name +
                       dirname_length(group_master_log_name));
      /*
        First compare the parts before the extension.
        Find the dot in the master's log basename,
        and protect against user's input error :
        if the names do not match up to '.' included, return error
      */
      char *q= (char*)(fn_ext(basename)+1);
      if (strncmp(basename, log_name_tmp, (int)(q-basename)))
      {
        error= -2;
        break;
      }
      // Now compare extensions.
      char *q_end;
      ulong group_master_log_name_extension= strtoul(q, &q_end, 10);
      if (group_master_log_name_extension < log_name_extension)
        cmp_result= -1 ;
      else
        cmp_result= (group_master_log_name_extension > log_name_extension) ? 1 : 0 ;

      pos_reached= ((!cmp_result && group_master_log_pos >= (ulonglong)log_pos) ||
                    cmp_result > 0);
      if (pos_reached || thd->killed)
        break;
    }

    //wait for master update, with optional timeout.

    DBUG_PRINT("info",("Waiting for master update"));
    /*
      We are going to mysql_cond_(timed)wait(); if the SQL thread stops it
      will wake us up.
    */
    thd_wait_begin(thd, THD_WAIT_BINLOG);
    if (timeout > 0)
    {
      /*
        Note that mysql_cond_timedwait checks for the timeout
        before for the condition ; i.e. it returns ETIMEDOUT
        if the system time equals or exceeds the time specified by abstime
        before the condition variable is signaled or broadcast, _or_ if
        the absolute time specified by abstime has already passed at the time
        of the call.
        For that reason, mysql_cond_timedwait will do the "timeoutting" job
        even if its condition is always immediately signaled (case of a loaded
        master).
      */
      error= mysql_cond_timedwait(&data_cond, &data_lock, &abstime);
    }
    else
      mysql_cond_wait(&data_cond, &data_lock);
    thd_wait_end(thd);
    DBUG_PRINT("info",("Got signal of master update or timed out"));
    if (error == ETIMEDOUT || error == ETIME)
    {
      error= -1;
      break;
    }
    error=0;
    event_count++;
    DBUG_PRINT("info",("Testing if killed or SQL thread not running"));
  }

err:
  thd->EXIT_COND(&old_stage);
  DBUG_PRINT("exit",("killed: %d  abort: %d  slave_running: %d \
improper_arguments: %d  timed_out: %d",
                     thd->killed_errno(),
                     (int) (init_abort_pos_wait != abort_pos_wait),
                     (int) slave_running,
                     (int) (error == -2),
                     (int) (error == -1)));
  if (thd->killed || init_abort_pos_wait != abort_pos_wait ||
      !slave_running)
  {
    error= -2;
  }
  DBUG_RETURN( error ? error : event_count );
}


void Relay_log_info::inc_group_relay_log_pos(ulonglong log_pos,
                                             rpl_group_info *rgi,
                                             bool skip_lock)
{
  DBUG_ENTER("Relay_log_info::inc_group_relay_log_pos");

  if (!skip_lock)
    mysql_mutex_lock(&data_lock);
  rgi->inc_event_relay_log_pos();
  DBUG_PRINT("info", ("log_pos: %lu  group_master_log_pos: %lu",
                      (long) log_pos, (long) group_master_log_pos));
  if (rgi->is_parallel_exec)
  {
    /* In case of parallel replication, do not update the position backwards. */
    int cmp= strcmp(group_relay_log_name, rgi->event_relay_log_name);
    if (cmp < 0)
    {
      group_relay_log_pos= rgi->future_event_relay_log_pos;
      strmake_buf(group_relay_log_name, rgi->event_relay_log_name);
      notify_group_relay_log_name_update();
    } else if (cmp == 0 && group_relay_log_pos < rgi->future_event_relay_log_pos)
      group_relay_log_pos= rgi->future_event_relay_log_pos;

    /*
      In the parallel case we need to update the master_log_name here, rather
      than in Rotate_log_event::do_update_pos().
    */
    cmp= strcmp(group_master_log_name, rgi->future_event_master_log_name);
    if (cmp <= 0)
    {
      if (cmp < 0)
      {
        strcpy(group_master_log_name, rgi->future_event_master_log_name);
        group_master_log_pos= log_pos;
      }
      else if (group_master_log_pos < log_pos)
        group_master_log_pos= log_pos;
    }

    /*
      In the parallel case, we only update the Seconds_Behind_Master at the
      end of a transaction. In the non-parallel case, the value is updated as
      soon as an event is read from the relay log; however this would be too
      confusing for the user, seeing the slave reported as up-to-date when
      potentially thousands of events are still queued up for worker threads
      waiting for execution.
    */
    if (rgi->last_master_timestamp &&
        rgi->last_master_timestamp > last_master_timestamp)
      last_master_timestamp= rgi->last_master_timestamp;
  }
  else
  {
    /* Non-parallel case. */
    group_relay_log_pos= event_relay_log_pos;
    strmake_buf(group_relay_log_name, event_relay_log_name);
    notify_group_relay_log_name_update();
    if (log_pos) // not 3.23 binlogs (no log_pos there) and not Stop_log_event
      group_master_log_pos= log_pos;
  }

  /*
    If the slave does not support transactions and replicates a transaction,
    users should not trust group_master_log_pos (which they can display with
    SHOW SLAVE STATUS or read from relay-log.info), because to compute
    group_master_log_pos the slave relies on log_pos stored in the master's
    binlog, but if we are in a master's transaction these positions are always
    the BEGIN's one (excepted for the COMMIT), so group_master_log_pos does
    not advance as it should on the non-transactional slave (it advances by
    big leaps, whereas it should advance by small leaps).
  */
  /*
    In 4.x we used the event's len to compute the positions here. This is
    wrong if the event was 3.23/4.0 and has been converted to 5.0, because
    then the event's len is not what is was in the master's binlog, so this
    will make a wrong group_master_log_pos (yes it's a bug in 3.23->4.0
    replication: Exec_master_log_pos is wrong). Only way to solve this is to
    have the original offset of the end of the event the relay log. This is
    what we do in 5.0: log_pos has become "end_log_pos" (because the real use
    of log_pos in 4.0 was to compute the end_log_pos; so better to store
    end_log_pos instead of begin_log_pos.
    If we had not done this fix here, the problem would also have appeared
    when the slave and master are 5.0 but with different event length (for
    example the slave is more recent than the master and features the event
    UID). It would give false MASTER_POS_WAIT, false Exec_master_log_pos in
    SHOW SLAVE STATUS, and so the user would do some CHANGE MASTER using this
    value which would lead to badly broken replication.
    Even the relay_log_pos will be corrupted in this case, because the len is
    the relay log is not "val".
    With the end_log_pos solution, we avoid computations involving lengthes.
  */
  mysql_cond_broadcast(&data_cond);
  if (!skip_lock)
    mysql_mutex_unlock(&data_lock);
  DBUG_VOID_RETURN;
}


void Relay_log_info::close_temporary_tables()
{
  TABLE *table,*next;
  DBUG_ENTER("Relay_log_info::close_temporary_tables");

  for (table=save_temporary_tables ; table ; table=next)
  {
    next=table->next;

    /* Reset in_use as the table may have been created by another thd */
    table->in_use=0;
    /*
      Don't ask for disk deletion. For now, anyway they will be deleted when
      slave restarts, but it is a better intention to not delete them.
    */
    DBUG_PRINT("info", ("table: 0x%lx", (long) table));
    close_temporary(table, 1, 0);
  }
  save_temporary_tables= 0;
  slave_open_temp_tables= 0;
  DBUG_VOID_RETURN;
}

/*
  purge_relay_logs()

  @param rli		Relay log information
  @param thd		thread id. May be zero during startup

  NOTES
    Assumes to have a run lock on rli and that no slave thread are running.
*/

int purge_relay_logs(Relay_log_info* rli, THD *thd, bool just_reset,
                     const char** errmsg)
{
  int error=0;
  DBUG_ENTER("purge_relay_logs");

  /*
    Even if rli->inited==0, we still try to empty rli->master_log_* variables.
    Indeed, rli->inited==0 does not imply that they already are empty.
    It could be that slave's info initialization partly succeeded :
    for example if relay-log.info existed but *relay-bin*.*
    have been manually removed, init_relay_log_info reads the old
    relay-log.info and fills rli->master_log_*, then init_relay_log_info
    checks for the existence of the relay log, this fails and
    init_relay_log_info leaves rli->inited to 0.
    In that pathological case, rli->master_log_pos* will be properly reinited
    at the next START SLAVE (as RESET SLAVE or CHANGE
    MASTER, the callers of purge_relay_logs, will delete bogus *.info files
    or replace them with correct files), however if the user does SHOW SLAVE
    STATUS before START SLAVE, he will see old, confusing rli->master_log_*.
    In other words, we reinit rli->master_log_* for SHOW SLAVE STATUS
    to display fine in any case.
  */

  rli->group_master_log_name[0]= 0;
  rli->group_master_log_pos= 0;

  if (!rli->inited)
  {
    DBUG_PRINT("info", ("rli->inited == 0"));
    DBUG_RETURN(0);
  }

  DBUG_ASSERT(rli->slave_running == 0);
  DBUG_ASSERT(rli->mi->slave_running == 0);

  mysql_mutex_lock(&rli->data_lock);

  /*
    we close the relay log fd possibly left open by the slave SQL thread,
    to be able to delete it; the relay log fd possibly left open by the slave
    I/O thread will be closed naturally in reset_logs() by the
    close(LOG_CLOSE_TO_BE_OPENED) call
  */
  if (rli->cur_log_fd >= 0)
  {
    end_io_cache(&rli->cache_buf);
    mysql_file_close(rli->cur_log_fd, MYF(MY_WME));
    rli->cur_log_fd= -1;
  }

  if (rli->relay_log.reset_logs(thd, !just_reset, NULL, 0, 0))
  {
    *errmsg = "Failed during log reset";
    error=1;
    goto err;
  }
  rli->relay_log_state.load(rpl_global_gtid_slave_state);
  if (!just_reset)
  {
    /* Save name of used relay log file */
    strmake_buf(rli->group_relay_log_name, rli->relay_log.get_log_fname());
    strmake_buf(rli->event_relay_log_name, rli->relay_log.get_log_fname());
    rli->group_relay_log_pos= rli->event_relay_log_pos= BIN_LOG_HEADER_SIZE;
    rli->log_space_total= 0;

    if (count_relay_log_space(rli))
    {
      *errmsg= "Error counting relay log space";
      error=1;
      goto err;
    }
    error= init_relay_log_pos(rli, rli->group_relay_log_name,
                              rli->group_relay_log_pos,
                              0 /* do not need data lock */, errmsg, 0);
  }
  else
  {
    /* Ensure relay log names are not used */
    rli->group_relay_log_name[0]= rli->event_relay_log_name[0]= 0;
  }

err:
  DBUG_PRINT("info",("log_space_total: %llu",rli->log_space_total));
  mysql_mutex_unlock(&rli->data_lock);
  DBUG_RETURN(error);
}


/*
     Check if condition stated in UNTIL clause of START SLAVE is reached.
   SYNOPSYS
     Relay_log_info::is_until_satisfied()
     master_beg_pos    position of the beginning of to be executed event
                       (not log_pos member of the event that points to the
                        beginning of the following event)


   DESCRIPTION
     Checks if UNTIL condition is reached. Uses caching result of last
     comparison of current log file name and target log file name. So cached
     value should be invalidated if current log file name changes
     (see Relay_log_info::notify_... functions).

     This caching is needed to avoid of expensive string comparisons and
     strtol() conversions needed for log names comparison. We don't need to
     compare them each time this function is called, we only need to do this
     when current log name changes. If we have UNTIL_MASTER_POS condition we
     need to do this only after Rotate_log_event::do_apply_event() (which is
     rare, so caching gives real benifit), and if we have UNTIL_RELAY_POS
     condition then we should invalidate cached comarison value after
     inc_group_relay_log_pos() which called for each group of events (so we
     have some benefit if we have something like queries that use
     autoincrement or if we have transactions).

     Should be called ONLY if until_condition != UNTIL_NONE !
   RETURN VALUE
     true - condition met or error happened (condition seems to have
            bad log file name)
     false - condition not met
*/

bool Relay_log_info::is_until_satisfied(my_off_t master_beg_pos)
{
  const char *log_name;
  ulonglong log_pos;
  DBUG_ENTER("Relay_log_info::is_until_satisfied");

  if (until_condition == UNTIL_MASTER_POS)
  {
    log_name= (mi->using_parallel() ?
               future_event_master_log_name : group_master_log_name);
    log_pos= master_beg_pos;
  }
  else
  {
    DBUG_ASSERT(until_condition == UNTIL_RELAY_POS);
    log_name= group_relay_log_name;
    log_pos= group_relay_log_pos;
  }

  DBUG_PRINT("info", ("group_master_log_name='%s', group_master_log_pos=%llu",
                      group_master_log_name, group_master_log_pos));
  DBUG_PRINT("info", ("group_relay_log_name='%s', group_relay_log_pos=%llu",
                      group_relay_log_name, group_relay_log_pos));
  DBUG_PRINT("info", ("(%s) log_name='%s', log_pos=%llu",
                      until_condition == UNTIL_MASTER_POS ? "master" : "relay",
                      log_name, log_pos));
  DBUG_PRINT("info", ("(%s) until_log_name='%s', until_log_pos=%llu",
                      until_condition == UNTIL_MASTER_POS ? "master" : "relay",
                      until_log_name, until_log_pos));

  if (until_log_names_cmp_result == UNTIL_LOG_NAMES_CMP_UNKNOWN)
  {
    /*
      We have no cached comparison results so we should compare log names
      and cache result.
      If we are after RESET SLAVE, and the SQL slave thread has not processed
      any event yet, it could be that group_master_log_name is "". In that case,
      just wait for more events (as there is no sensible comparison to do).
    */

    if (*log_name)
    {
      const char *basename= log_name + dirname_length(log_name);

      const char *q= (const char*)(fn_ext(basename)+1);
      if (strncmp(basename, until_log_name, (int)(q-basename)) == 0)
      {
        /* Now compare extensions. */
        char *q_end;
        ulong log_name_extension= strtoul(q, &q_end, 10);
        if (log_name_extension < until_log_name_extension)
          until_log_names_cmp_result= UNTIL_LOG_NAMES_CMP_LESS;
        else
          until_log_names_cmp_result=
            (log_name_extension > until_log_name_extension) ?
            UNTIL_LOG_NAMES_CMP_GREATER : UNTIL_LOG_NAMES_CMP_EQUAL ;
      }
      else
      {
        /* Probably error so we aborting */
        sql_print_error("Slave SQL thread is stopped because UNTIL "
                        "condition is bad.");
        DBUG_RETURN(TRUE);
      }
    }
    else
      DBUG_RETURN(until_log_pos == 0);
  }

  DBUG_RETURN(((until_log_names_cmp_result == UNTIL_LOG_NAMES_CMP_EQUAL &&
           log_pos >= until_log_pos) ||
          until_log_names_cmp_result == UNTIL_LOG_NAMES_CMP_GREATER));
}


void Relay_log_info::stmt_done(my_off_t event_master_log_pos, THD *thd,
                               rpl_group_info *rgi)
{
  DBUG_ENTER("Relay_log_info::stmt_done");

  DBUG_ASSERT(rgi->rli == this);
  /*
    If in a transaction, and if the slave supports transactions, just
    inc_event_relay_log_pos(). We only have to check for OPTION_BEGIN
    (not OPTION_NOT_AUTOCOMMIT) as transactions are logged with
    BEGIN/COMMIT, not with SET AUTOCOMMIT= .

    We can't use rgi->rli->get_flag(IN_TRANSACTION) here as OPTION_BEGIN
    is also used for single row transactions.

    CAUTION: opt_using_transactions means innodb || bdb ; suppose the
    master supports InnoDB and BDB, but the slave supports only BDB,
    problems will arise: - suppose an InnoDB table is created on the
    master, - then it will be MyISAM on the slave - but as
    opt_using_transactions is true, the slave will believe he is
    transactional with the MyISAM table. And problems will come when
    one does START SLAVE; STOP SLAVE; START SLAVE; (the slave will
    resume at BEGIN whereas there has not been any rollback).  This is
    the problem of using opt_using_transactions instead of a finer
    "does the slave support _transactional handler used on the
    master_".

    More generally, we'll have problems when a query mixes a
    transactional handler and MyISAM and STOP SLAVE is issued in the
    middle of the "transaction". START SLAVE will resume at BEGIN
    while the MyISAM table has already been updated.
  */
  if ((rgi->thd->variables.option_bits & OPTION_BEGIN) &&
      opt_using_transactions)
    rgi->inc_event_relay_log_pos();
  else
  {
    inc_group_relay_log_pos(event_master_log_pos, rgi);
    if (rpl_global_gtid_slave_state->record_and_update_gtid(thd, rgi))
    {
      report(WARNING_LEVEL, ER_CANNOT_UPDATE_GTID_STATE, rgi->gtid_info(),
             "Failed to update GTID state in %s.%s, slave state may become "
             "inconsistent: %d: %s",
             "mysql", rpl_gtid_slave_state_table_name.str,
             thd->get_stmt_da()->sql_errno(), thd->get_stmt_da()->message());
      /*
        At this point we are not in a transaction (for example after DDL),
        so we can not roll back. Anyway, normally updates to the slave
        state table should not fail, and if they do, at least we made the
        DBA aware of the problem in the error log.
      */
    }
    DBUG_EXECUTE_IF("inject_crash_before_flush_rli", DBUG_SUICIDE(););
    if (mi->using_gtid == Master_info::USE_GTID_NO)
      flush_relay_log_info(this);
    DBUG_EXECUTE_IF("inject_crash_after_flush_rli", DBUG_SUICIDE(););
  }
  DBUG_VOID_RETURN;
}


int
Relay_log_info::alloc_inuse_relaylog(const char *name)
{
  inuse_relaylog *ir;
  uint32 gtid_count;
  rpl_gtid *gtid_list;

  if (!(ir= (inuse_relaylog *)my_malloc(sizeof(*ir), MYF(MY_WME|MY_ZEROFILL))))
  {
    my_error(ER_OUTOFMEMORY, MYF(0), (int)sizeof(*ir));
    return 1;
  }
  gtid_count= relay_log_state.count();
  if (!(gtid_list= (rpl_gtid *)my_malloc(sizeof(*gtid_list)*gtid_count,
                                         MYF(MY_WME))))
  {
    my_free(ir);
    my_error(ER_OUTOFMEMORY, MYF(0), (int)sizeof(*gtid_list)*gtid_count);
    return 1;
  }
  if (relay_log_state.get_gtid_list(gtid_list, gtid_count))
  {
    my_free(gtid_list);
    my_free(ir);
    DBUG_ASSERT(0 /* Should not be possible as we allocated correct length */);
    my_error(ER_OUT_OF_RESOURCES, MYF(0));
    return 1;
  }
  ir->rli= this;
  strmake_buf(ir->name, name);
  ir->relay_log_state= gtid_list;
  ir->relay_log_state_count= gtid_count;

  if (!inuse_relaylog_list)
    inuse_relaylog_list= ir;
  else
  {
    last_inuse_relaylog->completed= true;
    last_inuse_relaylog->next= ir;
  }
  last_inuse_relaylog= ir;

  return 0;
}


void
Relay_log_info::free_inuse_relaylog(inuse_relaylog *ir)
{
  my_free(ir->relay_log_state);
  my_free(ir);
}


void
Relay_log_info::reset_inuse_relaylog()
{
  inuse_relaylog *cur= inuse_relaylog_list;
  while (cur)
  {
    DBUG_ASSERT(cur->queued_count == cur->dequeued_count);
    inuse_relaylog *next= cur->next;
    free_inuse_relaylog(cur);
    cur= next;
  }
  inuse_relaylog_list= last_inuse_relaylog= NULL;
}


int
Relay_log_info::update_relay_log_state(rpl_gtid *gtid_list, uint32 count)
{
  int res= 0;
  while (count)
  {
    if (relay_log_state.update_nolock(gtid_list, false))
      res= 1;
    ++gtid_list;
    --count;
  }
  return res;
}


#if !defined(MYSQL_CLIENT) && defined(HAVE_REPLICATION)
int
rpl_load_gtid_slave_state(THD *thd)
{
  TABLE_LIST tlist;
  TABLE *table;
  bool table_opened= false;
  bool table_scanned= false;
  bool array_inited= false;
  struct local_element { uint64 sub_id; rpl_gtid gtid; };
  struct local_element tmp_entry, *entry;
  HASH hash;
  DYNAMIC_ARRAY array;
  int err= 0;
  uint32 i;
  DBUG_ENTER("rpl_load_gtid_slave_state");

  mysql_mutex_lock(&rpl_global_gtid_slave_state->LOCK_slave_state);
  bool loaded= rpl_global_gtid_slave_state->loaded;
  mysql_mutex_unlock(&rpl_global_gtid_slave_state->LOCK_slave_state);
  if (loaded)
    DBUG_RETURN(0);

  my_hash_init(&hash, &my_charset_bin, 32,
               offsetof(local_element, gtid) + offsetof(rpl_gtid, domain_id),
               sizeof(uint32), NULL, my_free, HASH_UNIQUE);
  if ((err= my_init_dynamic_array(&array, sizeof(local_element), 0, 0, MYF(0))))
    goto end;
  array_inited= true;

  thd->reset_for_next_command();

  tlist.init_one_table(STRING_WITH_LEN("mysql"),
                       rpl_gtid_slave_state_table_name.str,
                       rpl_gtid_slave_state_table_name.length,
                       NULL, TL_READ);
  if ((err= open_and_lock_tables(thd, &tlist, FALSE, 0)))
    goto end;
  table_opened= true;
  table= tlist.table;

  if ((err= gtid_check_rpl_slave_state_table(table)))
    goto end;

  bitmap_set_all(table->read_set);
  if ((err= table->file->ha_rnd_init_with_error(1)))
  {
    table->file->print_error(err, MYF(0));
    goto end;
  }
  table_scanned= true;
  for (;;)
  {
    uint32 domain_id, server_id;
    uint64 sub_id, seq_no;
    uchar *rec;

    if ((err= table->file->ha_rnd_next(table->record[0])))
    {
      if (err == HA_ERR_RECORD_DELETED)
        continue;
      else if (err == HA_ERR_END_OF_FILE)
        break;
      else
      {
        table->file->print_error(err, MYF(0));
        goto end;
      }
    }
    domain_id= (ulonglong)table->field[0]->val_int();
    sub_id= (ulonglong)table->field[1]->val_int();
    server_id= (ulonglong)table->field[2]->val_int();
    seq_no= (ulonglong)table->field[3]->val_int();
    DBUG_PRINT("info", ("Read slave state row: %u-%u-%lu sub_id=%lu\n",
                        (unsigned)domain_id, (unsigned)server_id,
                        (ulong)seq_no, (ulong)sub_id));

    tmp_entry.sub_id= sub_id;
    tmp_entry.gtid.domain_id= domain_id;
    tmp_entry.gtid.server_id= server_id;
    tmp_entry.gtid.seq_no= seq_no;
    if ((err= insert_dynamic(&array, (uchar *)&tmp_entry)))
    {
      my_error(ER_OUT_OF_RESOURCES, MYF(0));
      goto end;
    }

    if ((rec= my_hash_search(&hash, (const uchar *)&domain_id, 0)))
    {
      entry= (struct local_element *)rec;
      if (entry->sub_id >= sub_id)
        continue;
      entry->sub_id= sub_id;
      DBUG_ASSERT(entry->gtid.domain_id == domain_id);
      entry->gtid.server_id= server_id;
      entry->gtid.seq_no= seq_no;
    }
    else
    {
      if (!(entry= (struct local_element *)my_malloc(sizeof(*entry),
                                                     MYF(MY_WME))))
      {
        my_error(ER_OUTOFMEMORY, MYF(0), (int)sizeof(*entry));
        err= 1;
        goto end;
      }
      entry->sub_id= sub_id;
      entry->gtid.domain_id= domain_id;
      entry->gtid.server_id= server_id;
      entry->gtid.seq_no= seq_no;
      if ((err= my_hash_insert(&hash, (uchar *)entry)))
      {
        my_free(entry);
        my_error(ER_OUT_OF_RESOURCES, MYF(0));
        goto end;
      }
    }
  }

  mysql_mutex_lock(&rpl_global_gtid_slave_state->LOCK_slave_state);
  if (rpl_global_gtid_slave_state->loaded)
  {
    mysql_mutex_unlock(&rpl_global_gtid_slave_state->LOCK_slave_state);
    goto end;
  }

  for (i= 0; i < array.elements; ++i)
  {
    get_dynamic(&array, (uchar *)&tmp_entry, i);
    if ((err= rpl_global_gtid_slave_state->update(tmp_entry.gtid.domain_id,
                                                 tmp_entry.gtid.server_id,
                                                 tmp_entry.sub_id,
                                                 tmp_entry.gtid.seq_no,
                                                 NULL)))
    {
      mysql_mutex_unlock(&rpl_global_gtid_slave_state->LOCK_slave_state);
      my_error(ER_OUT_OF_RESOURCES, MYF(0));
      goto end;
    }
  }

  for (i= 0; i < hash.records; ++i)
  {
    entry= (struct local_element *)my_hash_element(&hash, i);
    if (opt_bin_log &&
        mysql_bin_log.bump_seq_no_counter_if_needed(entry->gtid.domain_id,
                                                    entry->gtid.seq_no))
    {
      mysql_mutex_unlock(&rpl_global_gtid_slave_state->LOCK_slave_state);
      my_error(ER_OUT_OF_RESOURCES, MYF(0));
      goto end;
    }
  }

  rpl_global_gtid_slave_state->loaded= true;
  mysql_mutex_unlock(&rpl_global_gtid_slave_state->LOCK_slave_state);

  err= 0;                                       /* Clear HA_ERR_END_OF_FILE */

end:
  if (table_scanned)
  {
    table->file->ha_index_or_rnd_end();
    ha_commit_trans(thd, FALSE);
    ha_commit_trans(thd, TRUE);
  }
  if (table_opened)
  {
    close_thread_tables(thd);
    thd->mdl_context.release_transactional_locks();
  }
  if (array_inited)
    delete_dynamic(&array);
  my_hash_free(&hash);
  DBUG_RETURN(err);
}


void
rpl_group_info::reinit(Relay_log_info *rli)
{
  this->rli= rli;
  tables_to_lock= NULL;
  tables_to_lock_count= 0;
  trans_retries= 0;
  last_event_start_time= 0;
  gtid_sub_id= 0;
  commit_id= 0;
  gtid_pending= false;
  worker_error= 0;
  row_stmt_start_timestamp= 0;
  long_find_row_note_printed= false;
  did_mark_start_commit= false;
  gtid_ev_flags2= 0;
  last_master_timestamp = 0;
  gtid_ignore_duplicate_state= GTID_DUPLICATE_NULL;
  speculation= SPECULATE_NO;
  commit_orderer.reinit();
}

rpl_group_info::rpl_group_info(Relay_log_info *rli)
  : thd(0), wait_commit_sub_id(0),
    wait_commit_group_info(0), parallel_entry(0),
    deferred_events(NULL), m_annotate_event(0), is_parallel_exec(false)
{
  reinit(rli);
  bzero(&current_gtid, sizeof(current_gtid));
  mysql_mutex_init(key_rpl_group_info_sleep_lock, &sleep_lock,
                   MY_MUTEX_INIT_FAST);
  mysql_cond_init(key_rpl_group_info_sleep_cond, &sleep_cond, NULL);
}


rpl_group_info::~rpl_group_info()
{
  free_annotate_event();
  delete deferred_events;
  mysql_mutex_destroy(&sleep_lock);
  mysql_cond_destroy(&sleep_cond);
}


int
event_group_new_gtid(rpl_group_info *rgi, Gtid_log_event *gev)
{
  uint64 sub_id= rpl_global_gtid_slave_state->next_sub_id(gev->domain_id);
  if (!sub_id)
  {
    /* Out of memory caused hash insertion to fail. */
    return 1;
  }
  rgi->gtid_sub_id= sub_id;
  rgi->current_gtid.domain_id= gev->domain_id;
  rgi->current_gtid.server_id= gev->server_id;
  rgi->current_gtid.seq_no= gev->seq_no;
  rgi->commit_id= gev->commit_id;
  rgi->gtid_pending= true;
  return 0;
}


void
delete_or_keep_event_post_apply(rpl_group_info *rgi,
                                Log_event_type typ, Log_event *ev)
{
  /*
    ToDo: This needs to work on rpl_group_info, not Relay_log_info, to be
    thread-safe for parallel replication.
  */

  switch (typ) {
  case FORMAT_DESCRIPTION_EVENT:
    /*
      Format_description_log_event should not be deleted because it
      will be used to read info about the relay log's format;
      it will be deleted when the SQL thread does not need it,
      i.e. when this thread terminates.
    */
    break;
  case ANNOTATE_ROWS_EVENT:
    /*
      Annotate_rows event should not be deleted because after it has
      been applied, thd->query points to the string inside this event.
      The thd->query will be used to generate new Annotate_rows event
      during applying the subsequent Rows events.
    */
    rgi->set_annotate_event((Annotate_rows_log_event*) ev);
    break;
  case DELETE_ROWS_EVENT_V1:
  case UPDATE_ROWS_EVENT_V1:
  case WRITE_ROWS_EVENT_V1:
  case DELETE_ROWS_EVENT:
  case UPDATE_ROWS_EVENT:
  case WRITE_ROWS_EVENT:
    /*
      After the last Rows event has been applied, the saved Annotate_rows
      event (if any) is not needed anymore and can be deleted.
    */
    if (((Rows_log_event*)ev)->get_flags(Rows_log_event::STMT_END_F))
      rgi->free_annotate_event();
    /* fall through */
  default:
    DBUG_PRINT("info", ("Deleting the event after it has been executed"));
    if (!rgi->is_deferred_event(ev))
      delete ev;
    break;
  }
}


void rpl_group_info::cleanup_context(THD *thd, bool error)
{
  DBUG_ENTER("rpl_group_info::cleanup_context");
  DBUG_PRINT("enter", ("error: %d", (int) error));
  
  DBUG_ASSERT(this->thd == thd);
  /*
    1) Instances of Table_map_log_event, if ::do_apply_event() was called on them,
    may have opened tables, which we cannot be sure have been closed (because
    maybe the Rows_log_event have not been found or will not be, because slave
    SQL thread is stopping, or relay log has a missing tail etc). So we close
    all thread's tables. And so the table mappings have to be cancelled.
    2) Rows_log_event::do_apply_event() may even have started statements or
    transactions on them, which we need to rollback in case of error.
    3) If finding a Format_description_log_event after a BEGIN, we also need
    to rollback before continuing with the next events.
    4) so we need this "context cleanup" function.
  */
  if (error)
  {
    trans_rollback_stmt(thd); // if a "statement transaction"
    /* trans_rollback() also resets OPTION_GTID_BEGIN */
    trans_rollback(thd);      // if a "real transaction"
    /*
      Now that we have rolled back the transaction, make sure we do not
      erroneously update the GTID position.
    */
    gtid_pending= false;
  }
  m_table_map.clear_tables();
  slave_close_thread_tables(thd);
  if (error)
  {
    thd->mdl_context.release_transactional_locks();

    if (thd == rli->sql_driver_thd)
    {
      /*
        Reset flags. This is needed to handle incident events and errors in
        the relay log noticed by the sql driver thread.
      */
      rli->clear_flag(Relay_log_info::IN_STMT);
      rli->clear_flag(Relay_log_info::IN_TRANSACTION);
    }

    /*
      Ensure we always release the domain for others to process, when using
      --gtid-ignore-duplicates.
    */
    if (gtid_ignore_duplicate_state != GTID_DUPLICATE_NULL)
      rpl_global_gtid_slave_state->release_domain_owner(this);
  }

  /*
    Cleanup for the flags that have been set at do_apply_event.
  */
  thd->variables.option_bits&= ~OPTION_NO_FOREIGN_KEY_CHECKS;
  thd->variables.option_bits&= ~OPTION_RELAXED_UNIQUE_CHECKS;

  /*
    Reset state related to long_find_row notes in the error log:
    - timestamp
    - flag that decides whether the slave prints or not
  */
  reset_row_stmt_start_timestamp();
  unset_long_find_row_note_printed();

  DBUG_EXECUTE_IF("inject_sleep_gtid_100_x_x", {
      if (current_gtid.domain_id == 100)
        my_sleep(50000);
    };);

  DBUG_VOID_RETURN;
}


void rpl_group_info::clear_tables_to_lock()
{
  DBUG_ENTER("rpl_group_info::clear_tables_to_lock()");
#ifndef DBUG_OFF
  /**
    When replicating in RBR and MyISAM Merge tables are involved
    open_and_lock_tables (called in do_apply_event) appends the 
    base tables to the list of tables_to_lock. Then these are 
    removed from the list in close_thread_tables (which is called 
    before we reach this point).

    This assertion just confirms that we get no surprises at this
    point.
   */
  uint i=0;
  for (TABLE_LIST *ptr= tables_to_lock ; ptr ; ptr= ptr->next_global, i++) ;
  DBUG_ASSERT(i == tables_to_lock_count);
#endif  

  while (tables_to_lock)
  {
    uchar* to_free= reinterpret_cast<uchar*>(tables_to_lock);
    if (tables_to_lock->m_tabledef_valid)
    {
      tables_to_lock->m_tabledef.table_def::~table_def();
      tables_to_lock->m_tabledef_valid= FALSE;
    }

    /*
      If blob fields were used during conversion of field values 
      from the master table into the slave table, then we need to 
      free the memory used temporarily to store their values before
      copying into the slave's table.
    */
    if (tables_to_lock->m_conv_table)
      free_blobs(tables_to_lock->m_conv_table);

    tables_to_lock=
      static_cast<RPL_TABLE_LIST*>(tables_to_lock->next_global);
    tables_to_lock_count--;
    my_free(to_free);
  }
  DBUG_ASSERT(tables_to_lock == NULL && tables_to_lock_count == 0);
  DBUG_VOID_RETURN;
}


void rpl_group_info::slave_close_thread_tables(THD *thd)
{
  DBUG_ENTER("rpl_group_info::slave_close_thread_tables(THD *thd)");
  thd->get_stmt_da()->set_overwrite_status(true);
  thd->is_error() ? trans_rollback_stmt(thd) : trans_commit_stmt(thd);
  thd->get_stmt_da()->set_overwrite_status(false);

  close_thread_tables(thd);
  /*
    - If transaction rollback was requested due to deadlock
    perform it and release metadata locks.
    - If inside a multi-statement transaction,
    defer the release of metadata locks until the current
    transaction is either committed or rolled back. This prevents
    other statements from modifying the table for the entire
    duration of this transaction.  This provides commit ordering
    and guarantees serializability across multiple transactions.
    - If in autocommit mode, or outside a transactional context,
    automatically release metadata locks of the current statement.
  */
  if (thd->transaction_rollback_request)
  {
    trans_rollback_implicit(thd);
    thd->mdl_context.release_transactional_locks();
  }
  else if (! thd->in_multi_stmt_transaction_mode())
    thd->mdl_context.release_transactional_locks();
  else
    thd->mdl_context.release_statement_locks();

  clear_tables_to_lock();
  DBUG_VOID_RETURN;
}



static void
mark_start_commit_inner(rpl_parallel_entry *e, group_commit_orderer *gco,
                        rpl_group_info *rgi)
{
  group_commit_orderer *tmp;
  uint64 count= ++e->count_committing_event_groups;
  /* Signal any following GCO whose wait_count has been reached now. */
  tmp= gco;
  while ((tmp= tmp->next_gco))
  {
    uint64 wait_count= tmp->wait_count;
    if (wait_count > count)
      break;
    mysql_cond_broadcast(&tmp->COND_group_commit_orderer);
  }
}


void
rpl_group_info::mark_start_commit_no_lock()
{
  if (did_mark_start_commit)
    return;
  mark_start_commit_inner(parallel_entry, gco, this);
  did_mark_start_commit= true;
}


void
rpl_group_info::mark_start_commit()
{
  rpl_parallel_entry *e;

  if (did_mark_start_commit)
    return;

  e= this->parallel_entry;
  mysql_mutex_lock(&e->LOCK_parallel_entry);
  mark_start_commit_inner(e, gco, this);
  mysql_mutex_unlock(&e->LOCK_parallel_entry);
  did_mark_start_commit= true;
}


/*
  Format the current GTID as a string suitable for printing in error messages.

  The string is stored in a buffer inside rpl_group_info, so remains valid
  until next call to gtid_info() or until destruction of rpl_group_info.

  If no GTID is available, then NULL is returned.
*/
char *
rpl_group_info::gtid_info()
{
  if (!gtid_sub_id || !current_gtid.seq_no)
    return NULL;
  my_snprintf(gtid_info_buf, sizeof(gtid_info_buf), "Gtid %u-%u-%llu",
              current_gtid.domain_id, current_gtid.server_id,
              current_gtid.seq_no);
  return gtid_info_buf;
}


/*
  Undo the effect of a prior mark_start_commit().

  This is only used for retrying a transaction in parallel replication, after
  we have encountered a deadlock or other temporary error.

  When we get such a deadlock, it means that the current group of transactions
  did not yet all start committing (else they would not have deadlocked). So
  we will not yet have woken up anything in the next group, our rgi->gco is
  still live, and we can simply decrement the counter (to be incremented again
  later, when the retry succeeds and reaches the commit step).
*/
void
rpl_group_info::unmark_start_commit()
{
  rpl_parallel_entry *e;

  if (!did_mark_start_commit)
    return;

  e= this->parallel_entry;
  mysql_mutex_lock(&e->LOCK_parallel_entry);
  --e->count_committing_event_groups;
  mysql_mutex_unlock(&e->LOCK_parallel_entry);
  did_mark_start_commit= false;
}


rpl_sql_thread_info::rpl_sql_thread_info(Rpl_filter *filter)
  : rpl_filter(filter)
{
  cached_charset_invalidate();
}


void rpl_sql_thread_info::cached_charset_invalidate()
{
  DBUG_ENTER("rpl_group_info::cached_charset_invalidate");

  /* Full of zeroes means uninitialized. */
  bzero(cached_charset, sizeof(cached_charset));
  DBUG_VOID_RETURN;
}


bool rpl_sql_thread_info::cached_charset_compare(char *charset) const
{
  DBUG_ENTER("rpl_group_info::cached_charset_compare");

  if (memcmp(cached_charset, charset, sizeof(cached_charset)))
  {
    memcpy(const_cast<char*>(cached_charset), charset, sizeof(cached_charset));
    DBUG_RETURN(1);
  }
  DBUG_RETURN(0);
}

#endif
