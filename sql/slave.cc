/* Copyright (c) 2000, 2015, Oracle and/or its affiliates.
   Copyright (c) 2008, 2015, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */


/**
  @addtogroup Replication
  @{

  @file

  @brief Code to run the io thread and the sql thread on the
  replication slave.
*/

#include <my_global.h>
#include "sql_priv.h"
#include "slave.h"
#include "sql_parse.h"                         // execute_init_command
#include "sql_table.h"                         // mysql_rm_table
#include "rpl_mi.h"
#include "rpl_rli.h"
#include "sql_repl.h"
#include "rpl_filter.h"
#include "repl_failsafe.h"
#include "transaction.h"
#include <thr_alarm.h>
#include <my_dir.h>
#include <sql_common.h>
#include <errmsg.h>
#include <mysqld_error.h>
#include <mysys_err.h>
#include "rpl_handler.h"
#include <signal.h>
#include <mysql.h>
#include <myisam.h>

#include "sql_base.h"                           // close_thread_tables
#include "tztime.h"                             // struct Time_zone
#include "log_event.h"                          // Rotate_log_event,
                                                // Create_file_log_event,
                                                // Format_description_log_event
#include "wsrep_mysqld.h"

#ifdef HAVE_REPLICATION

#include "rpl_tblmap.h"
#include "debug_sync.h"
#include "rpl_parallel.h"


#define FLAGSTR(V,F) ((V)&(F)?#F" ":"")

#define MAX_SLAVE_RETRY_PAUSE 5
/*
  a parameter of sql_slave_killed() to defer the killed status
*/
#define SLAVE_WAIT_GROUP_DONE 60
bool use_slave_mask = 0;
MY_BITMAP slave_error_mask;
char slave_skip_error_names[SHOW_VAR_FUNC_BUFF_SIZE];

char* slave_load_tmpdir = 0;
Master_info *active_mi= 0;
Master_info_index *master_info_index;
my_bool replicate_same_server_id;
ulonglong relay_log_space_limit = 0;

const char *relay_log_index= 0;
const char *relay_log_basename= 0;

LEX_STRING default_master_connection_name= { (char*) "", 0 };

/*
  When slave thread exits, we need to remember the temporary tables so we
  can re-use them on slave start.

  TODO: move the vars below under Master_info
*/

int disconnect_slave_event_count = 0, abort_slave_event_count = 0;

static pthread_key(Master_info*, RPL_MASTER_INFO);

enum enum_slave_reconnect_actions
{
  SLAVE_RECON_ACT_REG= 0,
  SLAVE_RECON_ACT_DUMP= 1,
  SLAVE_RECON_ACT_EVENT= 2,
  SLAVE_RECON_ACT_MAX
};

enum enum_slave_reconnect_messages
{
  SLAVE_RECON_MSG_WAIT= 0,
  SLAVE_RECON_MSG_KILLED_WAITING= 1,
  SLAVE_RECON_MSG_AFTER= 2,
  SLAVE_RECON_MSG_FAILED= 3,
  SLAVE_RECON_MSG_COMMAND= 4,
  SLAVE_RECON_MSG_KILLED_AFTER= 5,
  SLAVE_RECON_MSG_MAX
};

static const char *reconnect_messages[SLAVE_RECON_ACT_MAX][SLAVE_RECON_MSG_MAX]=
{
  {
    "Waiting to reconnect after a failed registration on master",
    "Slave I/O thread killed while waiting to reconnect after a failed \
registration on master",
    "Reconnecting after a failed registration on master",
    "failed registering on master, reconnecting to try again, \
log '%s' at position %llu%s",
    "COM_REGISTER_SLAVE",
    "Slave I/O thread killed during or after reconnect"
  },
  {
    "Waiting to reconnect after a failed binlog dump request",
    "Slave I/O thread killed while retrying master dump",
    "Reconnecting after a failed binlog dump request",
    "failed dump request, reconnecting to try again, log '%s' at position %llu%s",
    "COM_BINLOG_DUMP",
    "Slave I/O thread killed during or after reconnect"
  },
  {
    "Waiting to reconnect after a failed master event read",
    "Slave I/O thread killed while waiting to reconnect after a failed read",
    "Reconnecting after a failed master event read",
    "Slave I/O thread: Failed reading log event, reconnecting to retry, \
log '%s' at position %llu%s",
    "",
    "Slave I/O thread killed during or after a reconnect done to recover from \
failed read"
  }
};
 

typedef enum { SLAVE_THD_IO, SLAVE_THD_SQL} SLAVE_THD_TYPE;

static int process_io_rotate(Master_info* mi, Rotate_log_event* rev);
static int process_io_create_file(Master_info* mi, Create_file_log_event* cev);
static bool wait_for_relay_log_space(Relay_log_info* rli);
static bool io_slave_killed(Master_info* mi);
static bool sql_slave_killed(rpl_group_info *rgi);
static int init_slave_thread(THD*, Master_info *, SLAVE_THD_TYPE);
static void print_slave_skip_errors(void);
static int safe_connect(THD* thd, MYSQL* mysql, Master_info* mi);
static int safe_reconnect(THD*, MYSQL*, Master_info*, bool);
static int connect_to_master(THD*, MYSQL*, Master_info*, bool, bool);
static Log_event* next_event(rpl_group_info* rgi, ulonglong *event_size);
static int queue_event(Master_info* mi,const char* buf,ulong event_len);
static int terminate_slave_thread(THD *, mysql_mutex_t *, mysql_cond_t *,
                                  volatile uint *, bool);
static bool check_io_slave_killed(Master_info *mi, const char *info);
static bool send_show_master_info_data(THD *, Master_info *, bool, String *);
/*
  Function to set the slave's max_allowed_packet based on the value
  of slave_max_allowed_packet.

    @in_param    thd    Thread handler for slave
    @in_param    mysql  MySQL connection handle
*/

static void set_slave_max_allowed_packet(THD *thd, MYSQL *mysql)
{
  DBUG_ENTER("set_slave_max_allowed_packet");
  // thd and mysql must be valid
  DBUG_ASSERT(thd && mysql);

  thd->variables.max_allowed_packet= slave_max_allowed_packet;
  thd->net.max_packet_size= slave_max_allowed_packet;
  /*
    Adding MAX_LOG_EVENT_HEADER_LEN to the max_packet_size on the I/O
    thread and the mysql->option max_allowed_packet, since a
    replication event can become this much  larger than
    the corresponding packet (query) sent from client to master.
  */
  thd->net.max_packet_size+= MAX_LOG_EVENT_HEADER;
  /*
    Skipping the setting of mysql->net.max_packet size to slave
    max_allowed_packet since this is done during mysql_real_connect.
  */
  mysql->options.max_allowed_packet=
    slave_max_allowed_packet+MAX_LOG_EVENT_HEADER;
  DBUG_VOID_RETURN;
}

/*
  Find out which replications threads are running

  SYNOPSIS
    init_thread_mask()
    mask                Return value here
    mi                  master_info for slave
    inverse             If set, returns which threads are not running

  IMPLEMENTATION
    Get a bit mask for which threads are running so that we can later restart
    these threads.

  RETURN
    mask        If inverse == 0, running threads
                If inverse == 1, stopped threads
*/

void init_thread_mask(int* mask,Master_info* mi,bool inverse)
{
  bool set_io = mi->slave_running, set_sql = mi->rli.slave_running;
  register int tmp_mask=0;
  DBUG_ENTER("init_thread_mask");

  if (set_io)
    tmp_mask |= SLAVE_IO;
  if (set_sql)
    tmp_mask |= SLAVE_SQL;
  if (inverse)
    tmp_mask^= (SLAVE_IO | SLAVE_SQL);
  *mask = tmp_mask;
  DBUG_VOID_RETURN;
}


/*
  lock_slave_threads()
*/

void lock_slave_threads(Master_info* mi)
{
  DBUG_ENTER("lock_slave_threads");

  //TODO: see if we can do this without dual mutex
  mysql_mutex_lock(&mi->run_lock);
  mysql_mutex_lock(&mi->rli.run_lock);
  DBUG_VOID_RETURN;
}


/*
  unlock_slave_threads()
*/

void unlock_slave_threads(Master_info* mi)
{
  DBUG_ENTER("unlock_slave_threads");

  //TODO: see if we can do this without dual mutex
  mysql_mutex_unlock(&mi->rli.run_lock);
  mysql_mutex_unlock(&mi->run_lock);
  DBUG_VOID_RETURN;
}

#ifdef HAVE_PSI_INTERFACE
static PSI_thread_key key_thread_slave_io, key_thread_slave_sql;

static PSI_thread_info all_slave_threads[]=
{
  { &key_thread_slave_io, "slave_io", PSI_FLAG_GLOBAL},
  { &key_thread_slave_sql, "slave_sql", PSI_FLAG_GLOBAL}
};

static void init_slave_psi_keys(void)
{
  const char* category= "sql";
  int count;

  if (PSI_server == NULL)
    return;

  count= array_elements(all_slave_threads);
  PSI_server->register_thread(category, all_slave_threads, count);
}
#endif /* HAVE_PSI_INTERFACE */


static bool slave_init_thread_running;


pthread_handler_t
handle_slave_init(void *arg __attribute__((unused)))
{
  THD *thd;

  my_thread_init();
  thd= new THD;
  thd->thread_stack= (char*) &thd;           /* Set approximate stack start */
  mysql_mutex_lock(&LOCK_thread_count);
  thd->thread_id= thread_id++;
  mysql_mutex_unlock(&LOCK_thread_count);
  thd->system_thread = SYSTEM_THREAD_SLAVE_INIT;
  thread_safe_increment32(&service_thread_count);
  thd->store_globals();
  thd->security_ctx->skip_grants();
  thd->set_command(COM_DAEMON);

  thd_proc_info(thd, "Loading slave GTID position from table");
  if (rpl_load_gtid_slave_state(thd))
    sql_print_warning("Failed to load slave replication state from table "
                      "%s.%s: %u: %s", "mysql",
                      rpl_gtid_slave_state_table_name.str,
                      thd->get_stmt_da()->sql_errno(),
                      thd->get_stmt_da()->message());

  delete thd;
  thread_safe_decrement32(&service_thread_count);
  signal_thd_deleted();
  my_thread_end();

  mysql_mutex_lock(&LOCK_slave_init);
  slave_init_thread_running= false;
  mysql_cond_broadcast(&COND_slave_init);
  mysql_mutex_unlock(&LOCK_slave_init);

  return 0;
}


/*
  Start the slave init thread.

  This thread is used to load the GTID state from mysql.gtid_slave_pos at
  server start; reading from table requires valid THD, which is otherwise not
  available during server init.
*/
static int
run_slave_init_thread()
{
  pthread_t th;

  slave_init_thread_running= true;
  if (mysql_thread_create(key_thread_slave_init, &th, &connection_attrib,
                          handle_slave_init, NULL))
  {
    sql_print_error("Failed to create thread while initialising slave");
    return 1;
  }

  mysql_mutex_lock(&LOCK_slave_init);
  while (slave_init_thread_running)
    mysql_cond_wait(&COND_slave_init, &LOCK_slave_init);
  mysql_mutex_unlock(&LOCK_slave_init);

  return 0;
}


/* Initialize slave structures */

int init_slave()
{
  DBUG_ENTER("init_slave");
  int error= 0;

#ifdef HAVE_PSI_INTERFACE
  init_slave_psi_keys();
#endif

  if (run_slave_init_thread())
    return 1;

  if (global_rpl_thread_pool.init(opt_slave_parallel_threads))
    return 1;

  /*
    This is called when mysqld starts. Before client connections are
    accepted. However bootstrap may conflict with us if it does START SLAVE.
    So it's safer to take the lock.
  */
  mysql_mutex_lock(&LOCK_active_mi);

  if (pthread_key_create(&RPL_MASTER_INFO, NULL))
    goto err;

  master_info_index= new Master_info_index;
  if (!master_info_index || master_info_index->init_all_master_info())
  {
    sql_print_error("Failed to initialize multi master structures");
    mysql_mutex_unlock(&LOCK_active_mi);
    DBUG_RETURN(1);
  }
  if (!(active_mi= new Master_info(&default_master_connection_name,
                                   relay_log_recovery)) ||
      active_mi->error())
  {
    delete active_mi;
    active_mi= 0;
    goto err;
  }

  if (master_info_index->add_master_info(active_mi, FALSE))
  {
    delete active_mi;
    active_mi= 0;
    goto err;
  }

  /*
    If --slave-skip-errors=... was not used, the string value for the
    system variable has not been set up yet. Do it now.
  */
  if (!use_slave_mask)
  {
    print_slave_skip_errors();
  }

  /*
    If master_host is not specified, try to read it from the master_info file.
    If master_host is specified, create the master_info file if it doesn't
    exists.
  */

  if (init_master_info(active_mi,master_info_file,relay_log_info_file,
                       1, (SLAVE_IO | SLAVE_SQL)))
  {
    sql_print_error("Failed to initialize the master info structure");
    goto err;
  }

  /* If server id is not set, start_slave_thread() will say it */

  if (active_mi->host[0] && !opt_skip_slave_start)
  {
    if (start_slave_threads(0, /* No active thd */
                            1 /* need mutex */,
                            0 /* no wait for start*/,
                            active_mi,
                            master_info_file,
                            relay_log_info_file,
                            SLAVE_IO | SLAVE_SQL))
    {
      sql_print_error("Failed to create slave threads");
      goto err;
    }
  }

end:
  mysql_mutex_unlock(&LOCK_active_mi);
  DBUG_RETURN(error);

err:
  sql_print_error("Failed to allocate memory for the Master Info structure");
  error= 1;
  goto end;
}

/*
  Updates the master info based on the information stored in the
  relay info and ignores relay logs previously retrieved by the IO 
  thread, which thus starts fetching again based on to the  
  group_master_log_pos and group_master_log_name. Eventually, the old
  relay logs will be purged by the normal purge mechanism.

  In the feature, we should improve this routine in order to avoid throwing
  away logs that are safely stored in the disk. Note also that this recovery 
  routine relies on the correctness of the relay-log.info and only tolerates 
  coordinate problems in master.info.
  
  In this function, there is no need for a mutex as the caller 
  (i.e. init_slave) already has one acquired.
  
  Specifically, the following structures are updated:
 
  1 - mi->master_log_pos  <-- rli->group_master_log_pos
  2 - mi->master_log_name <-- rli->group_master_log_name
  3 - It moves the relay log to the new relay log file, by
      rli->group_relay_log_pos  <-- BIN_LOG_HEADER_SIZE;
      rli->event_relay_log_pos  <-- BIN_LOG_HEADER_SIZE;
      rli->group_relay_log_name <-- rli->relay_log.get_log_fname();
      rli->event_relay_log_name <-- rli->relay_log.get_log_fname();
  
   If there is an error, it returns (1), otherwise returns (0).
 */
int init_recovery(Master_info* mi, const char** errmsg)
{
  DBUG_ENTER("init_recovery");
 
  Relay_log_info *rli= &mi->rli;
  if (rli->group_master_log_name[0])
  {
    mi->master_log_pos= MY_MAX(BIN_LOG_HEADER_SIZE,
                             rli->group_master_log_pos);
    strmake_buf(mi->master_log_name, rli->group_master_log_name);
 
    sql_print_warning("Recovery from master pos %ld and file %s.",
                      (ulong) mi->master_log_pos, mi->master_log_name);
 
    strmake_buf(rli->group_relay_log_name, rli->relay_log.get_log_fname());
    strmake_buf(rli->event_relay_log_name, rli->relay_log.get_log_fname());
 
    rli->group_relay_log_pos= rli->event_relay_log_pos= BIN_LOG_HEADER_SIZE;
  }

  DBUG_RETURN(0);
}

 
/**
  Convert slave skip errors bitmap into a printable string.
*/

static void print_slave_skip_errors(void)
{
  /*
    To be safe, we want 10 characters of room in the buffer for a number
    plus terminators. Also, we need some space for constant strings.
    10 characters must be sufficient for a number plus {',' | '...'}
    plus a NUL terminator. That is a max 6 digit number.
  */
  const size_t MIN_ROOM= 10;
  DBUG_ENTER("print_slave_skip_errors");
  DBUG_ASSERT(sizeof(slave_skip_error_names) > MIN_ROOM);
  DBUG_ASSERT(MAX_SLAVE_ERROR <= 999999); // 6 digits

  /* Make @@slave_skip_errors show the nice human-readable value.  */
  opt_slave_skip_errors= slave_skip_error_names;

  if (!use_slave_mask || bitmap_is_clear_all(&slave_error_mask))
  {
    /* purecov: begin tested */
    memcpy(slave_skip_error_names, STRING_WITH_LEN("OFF"));
    /* purecov: end */
  }
  else if (bitmap_is_set_all(&slave_error_mask))
  {
    /* purecov: begin tested */
    memcpy(slave_skip_error_names, STRING_WITH_LEN("ALL"));
    /* purecov: end */
  }
  else
  {
    char *buff= slave_skip_error_names;
    char *bend= buff + sizeof(slave_skip_error_names);
    int  errnum;

    for (errnum= 0; errnum < MAX_SLAVE_ERROR; errnum++)
    {
      if (bitmap_is_set(&slave_error_mask, errnum))
      {
        if (buff + MIN_ROOM >= bend)
          break; /* purecov: tested */
        buff= int10_to_str(errnum, buff, 10);
        *buff++= ',';
      }
    }
    if (buff != slave_skip_error_names)
      buff--; // Remove last ','
    if (errnum < MAX_SLAVE_ERROR)
    {
      /* Couldn't show all errors */
      buff= strmov(buff, "..."); /* purecov: tested */
    }
    *buff=0;
  }
  DBUG_PRINT("init", ("error_names: '%s'", slave_skip_error_names));
  DBUG_VOID_RETURN;
}

/*
  Init function to set up array for errors that should be skipped for slave

  SYNOPSIS
    init_slave_skip_errors()
    arg         List of errors numbers to skip, separated with ','

  NOTES
    Called from get_options() in mysqld.cc on start-up
*/

void init_slave_skip_errors(const char* arg)
{
  const char *p;
  DBUG_ENTER("init_slave_skip_errors");

  if (my_bitmap_init(&slave_error_mask,0,MAX_SLAVE_ERROR,0))
  {
    fprintf(stderr, "Badly out of memory, please check your system status\n");
    exit(1);
  }
  use_slave_mask = 1;
  for (;my_isspace(system_charset_info,*arg);++arg)
    /* empty */;
  if (!my_strnncoll(system_charset_info,(uchar*)arg,4,(const uchar*)"all",4))
  {
    bitmap_set_all(&slave_error_mask);
    print_slave_skip_errors();
    DBUG_VOID_RETURN;
  }
  for (p= arg ; *p; )
  {
    long err_code;
    if (!(p= str2int(p, 10, 0, LONG_MAX, &err_code)))
      break;
    if (err_code < MAX_SLAVE_ERROR)
       bitmap_set_bit(&slave_error_mask,(uint)err_code);
    while (!my_isdigit(system_charset_info,*p) && *p)
      p++;
  }
  /* Convert slave skip errors bitmap into a printable string. */
  print_slave_skip_errors();
  DBUG_VOID_RETURN;
}

int terminate_slave_threads(Master_info* mi,int thread_mask,bool skip_lock)
{
  DBUG_ENTER("terminate_slave_threads");

  if (!mi->inited)
    DBUG_RETURN(0); /* successfully do nothing */
  int error,force_all = (thread_mask & SLAVE_FORCE_ALL);
  mysql_mutex_t *sql_lock = &mi->rli.run_lock, *io_lock = &mi->run_lock;
  mysql_mutex_t *log_lock= mi->rli.relay_log.get_log_lock();

  if (thread_mask & (SLAVE_SQL|SLAVE_FORCE_ALL))
  {
    DBUG_PRINT("info",("Terminating SQL thread"));
    if (mi->using_parallel() && mi->rli.abort_slave && mi->rli.stop_for_until)
    {
      mi->rli.stop_for_until= false;
      mi->rli.parallel.stop_during_until();
    }
    else
      mi->rli.abort_slave=1;
    if ((error=terminate_slave_thread(mi->rli.sql_driver_thd, sql_lock,
                                      &mi->rli.stop_cond,
                                      &mi->rli.slave_running,
                                      skip_lock)) &&
        !force_all)
      DBUG_RETURN(error);

    mysql_mutex_lock(log_lock);

    DBUG_PRINT("info",("Flushing relay-log info file."));
    if (current_thd)
      THD_STAGE_INFO(current_thd, stage_flushing_relay_log_info_file);
    if (flush_relay_log_info(&mi->rli))
      DBUG_RETURN(ER_ERROR_DURING_FLUSH_LOGS);
    
    if (my_sync(mi->rli.info_fd, MYF(MY_WME)))
      DBUG_RETURN(ER_ERROR_DURING_FLUSH_LOGS);

    mysql_mutex_unlock(log_lock);
  }
  if (opt_slave_parallel_threads > 0 &&
      !master_info_index->any_slave_sql_running())
    rpl_parallel_inactivate_pool(&global_rpl_thread_pool);
  if (thread_mask & (SLAVE_IO|SLAVE_FORCE_ALL))
  {
    DBUG_PRINT("info",("Terminating IO thread"));
    mi->abort_slave=1;
    if ((error=terminate_slave_thread(mi->io_thd, io_lock,
                                      &mi->stop_cond,
                                      &mi->slave_running,
                                      skip_lock)) &&
        !force_all)
      DBUG_RETURN(error);

    mysql_mutex_lock(log_lock);

    DBUG_PRINT("info",("Flushing relay log and master info file."));
    if (current_thd)
      THD_STAGE_INFO(current_thd, stage_flushing_relay_log_and_master_info_repository);
    if (flush_master_info(mi, TRUE, FALSE))
      DBUG_RETURN(ER_ERROR_DURING_FLUSH_LOGS);

    if (mi->rli.relay_log.is_open() &&
        my_sync(mi->rli.relay_log.get_log_file()->file, MYF(MY_WME)))
      DBUG_RETURN(ER_ERROR_DURING_FLUSH_LOGS);

    if (my_sync(mi->fd, MYF(MY_WME)))
      DBUG_RETURN(ER_ERROR_DURING_FLUSH_LOGS);

    mysql_mutex_unlock(log_lock);
  }
  DBUG_RETURN(0); 
}


/**
   Wait for a slave thread to terminate.

   This function is called after requesting the thread to terminate
   (by setting @c abort_slave member of @c Relay_log_info or @c
   Master_info structure to 1). Termination of the thread is
   controlled with the the predicate <code>*slave_running</code>.

   Function will acquire @c term_lock before waiting on the condition
   unless @c skip_lock is true in which case the mutex should be owned
   by the caller of this function and will remain acquired after
   return from the function.

   @param term_lock
          Associated lock to use when waiting for @c term_cond

   @param term_cond
          Condition that is signalled when the thread has terminated

   @param slave_running
          Pointer to predicate to check for slave thread termination

   @param skip_lock
          If @c true the lock will not be acquired before waiting on
          the condition. In this case, it is assumed that the calling
          function acquires the lock before calling this function.

   @retval 0 All OK ER_SLAVE_NOT_RUNNING otherwise.

   @note  If the executing thread has to acquire term_lock (skip_lock
          is false), the negative running status does not represent
          any issue therefore no error is reported.

 */
static int
terminate_slave_thread(THD *thd,
                       mysql_mutex_t *term_lock,
                       mysql_cond_t *term_cond,
                       volatile uint *slave_running,
                       bool skip_lock)
{
  DBUG_ENTER("terminate_slave_thread");
  if (!skip_lock)
  {
    mysql_mutex_lock(term_lock);
  }
  else
  {
    mysql_mutex_assert_owner(term_lock);
  }
  if (!*slave_running)
  {
    if (!skip_lock)
    {
      /*
        if run_lock (term_lock) is acquired locally then either
        slave_running status is fine
      */
      mysql_mutex_unlock(term_lock);
      DBUG_RETURN(0);
    }
    else
    {
      DBUG_RETURN(ER_SLAVE_NOT_RUNNING);
    }
  }
  DBUG_ASSERT(thd != 0);
  THD_CHECK_SENTRY(thd);

  /*
    Is is critical to test if the slave is running. Otherwise, we might
    be referening freed memory trying to kick it
  */

  while (*slave_running)                        // Should always be true
  {
    int error __attribute__((unused));
    DBUG_PRINT("loop", ("killing slave thread"));

    mysql_mutex_lock(&thd->LOCK_thd_data);
#ifndef DONT_USE_THR_ALARM
    /*
      Error codes from pthread_kill are:
      EINVAL: invalid signal number (can't happen)
      ESRCH: thread already killed (can happen, should be ignored)
    */
    int err __attribute__((unused))= pthread_kill(thd->real_id, thr_client_alarm);
    DBUG_ASSERT(err != EINVAL);
#endif
    thd->awake(NOT_KILLED);

    mysql_mutex_unlock(&thd->LOCK_thd_data);

    /*
      There is a small chance that slave thread might miss the first
      alarm. To protect againts it, resend the signal until it reacts
    */
    struct timespec abstime;
    set_timespec(abstime,2);
    error= mysql_cond_timedwait(term_cond, term_lock, &abstime);
    DBUG_ASSERT(error == ETIMEDOUT || error == 0);
  }

  DBUG_ASSERT(*slave_running == 0);

  if (!skip_lock)
    mysql_mutex_unlock(term_lock);
  DBUG_RETURN(0);
}


int start_slave_thread(
#ifdef HAVE_PSI_INTERFACE
                       PSI_thread_key thread_key,
#endif
                       pthread_handler h_func, mysql_mutex_t *start_lock,
                       mysql_mutex_t *cond_lock,
                       mysql_cond_t *start_cond,
                       volatile uint *slave_running,
                       volatile ulong *slave_run_id,
                       Master_info* mi)
{
  pthread_t th;
  ulong start_id;
  int error;
  DBUG_ENTER("start_slave_thread");

  DBUG_ASSERT(mi->inited);

  if (start_lock)
    mysql_mutex_lock(start_lock);
  if (!global_system_variables.server_id)
  {
    if (start_cond)
      mysql_cond_broadcast(start_cond);
    if (start_lock)
      mysql_mutex_unlock(start_lock);
    sql_print_error("Server id not set, will not start slave");
    DBUG_RETURN(ER_BAD_SLAVE);
  }

  if (*slave_running)
  {
    if (start_cond)
      mysql_cond_broadcast(start_cond);
    if (start_lock)
      mysql_mutex_unlock(start_lock);
    DBUG_RETURN(ER_SLAVE_MUST_STOP);
  }
  start_id= *slave_run_id;
  DBUG_PRINT("info",("Creating new slave thread"));
  if ((error = mysql_thread_create(thread_key,
                           &th, &connection_attrib, h_func, (void*)mi)))
  {
    sql_print_error("Can't create slave thread (errno= %d).", error);
    if (start_lock)
      mysql_mutex_unlock(start_lock);
    DBUG_RETURN(ER_SLAVE_THREAD);
  }
  if (start_cond && cond_lock) // caller has cond_lock
  {
    THD* thd = current_thd;
    while (start_id == *slave_run_id)
    {
      DBUG_PRINT("sleep",("Waiting for slave thread to start"));
      PSI_stage_info saved_stage= {0, "", 0};
      thd->ENTER_COND(start_cond, cond_lock,
                      & stage_waiting_for_slave_thread_to_start,
                      & saved_stage);
      /*
        It is not sufficient to test this at loop bottom. We must test
        it after registering the mutex in enter_cond(). If the kill
        happens after testing of thd->killed and before the mutex is
        registered, we could otherwise go waiting though thd->killed is
        set.
      */
      if (!thd->killed)
        mysql_cond_wait(start_cond, cond_lock);
      thd->EXIT_COND(& saved_stage);
      mysql_mutex_lock(cond_lock); // re-acquire it as exit_cond() released
      if (thd->killed)
      {
        if (start_lock)
          mysql_mutex_unlock(start_lock);
        DBUG_RETURN(thd->killed_errno());
      }
    }
  }
  if (start_lock)
    mysql_mutex_unlock(start_lock);
  DBUG_RETURN(0);
}


/*
  start_slave_threads()

  NOTES
    SLAVE_FORCE_ALL is not implemented here on purpose since it does not make
    sense to do that for starting a slave--we always care if it actually
    started the threads that were not previously running
*/

int start_slave_threads(THD *thd,
                        bool need_slave_mutex, bool wait_for_start,
                        Master_info* mi, const char* master_info_fname,
                        const char* slave_info_fname, int thread_mask)
{
  mysql_mutex_t *lock_io=0, *lock_sql=0, *lock_cond_io=0, *lock_cond_sql=0;
  mysql_cond_t* cond_io=0, *cond_sql=0;
  int error=0;
  const char *errmsg;
  DBUG_ENTER("start_slave_threads");

  if (need_slave_mutex)
  {
    lock_io = &mi->run_lock;
    lock_sql = &mi->rli.run_lock;
  }
  if (wait_for_start)
  {
    cond_io = &mi->start_cond;
    cond_sql = &mi->rli.start_cond;
    lock_cond_io = &mi->run_lock;
    lock_cond_sql = &mi->rli.run_lock;
  }

  /*
    If we are using GTID and both SQL and IO threads are stopped, then get
    rid of all relay logs.

    Relay logs are not very useful when using GTID, except as a buffer
    between the fetch in the IO thread and the apply in SQL thread. However
    while one of the threads is running, they are in use and cannot be
    removed.
  */
  if (mi->using_gtid != Master_info::USE_GTID_NO &&
      !mi->slave_running && !mi->rli.slave_running)
  {
    /*
      purge_relay_logs() clears the mi->rli.group_master_log_pos.
      So save and restore them, like we do in CHANGE MASTER.
      (We are not going to use them for GTID, but it might be worth to
      keep them in case connection with GTID fails and user wants to go
      back and continue with previous old-style replication coordinates).
    */
    mi->master_log_pos = MY_MAX(BIN_LOG_HEADER_SIZE,
                                mi->rli.group_master_log_pos);
    strmake(mi->master_log_name, mi->rli.group_master_log_name,
            sizeof(mi->master_log_name)-1);
    purge_relay_logs(&mi->rli, thd, 0, &errmsg);
    mi->rli.group_master_log_pos= mi->master_log_pos;
    strmake(mi->rli.group_master_log_name, mi->master_log_name,
            sizeof(mi->rli.group_master_log_name)-1);

    error= rpl_load_gtid_state(&mi->gtid_current_pos, mi->using_gtid ==
                                             Master_info::USE_GTID_CURRENT_POS);
    mi->events_queued_since_last_gtid= 0;
    mi->gtid_reconnect_event_skip_count= 0;

    mi->rli.restart_gtid_pos.reset();
  }

  if (!error && (thread_mask & SLAVE_IO))
    error= start_slave_thread(
#ifdef HAVE_PSI_INTERFACE
                              key_thread_slave_io,
#endif
                              handle_slave_io, lock_io, lock_cond_io,
                              cond_io,
                              &mi->slave_running, &mi->slave_run_id,
                              mi);
  if (!error && (thread_mask & SLAVE_SQL))
  {
    if (opt_slave_parallel_threads > 0)
      error= rpl_parallel_activate_pool(&global_rpl_thread_pool);
    if (!error)
      error= start_slave_thread(
#ifdef HAVE_PSI_INTERFACE
                              key_thread_slave_sql,
#endif
                              handle_slave_sql, lock_sql, lock_cond_sql,
                              cond_sql,
                              &mi->rli.slave_running, &mi->rli.slave_run_id,
                              mi);
    if (error)
      terminate_slave_threads(mi, thread_mask & SLAVE_IO, !need_slave_mutex);
  }
  DBUG_RETURN(error);
}


/*
  Release slave threads at time of executing shutdown.

  SYNOPSIS
    end_slave()
*/

void end_slave()
{
  DBUG_ENTER("end_slave");

  /*
    This is called when the server terminates, in close_connections().
    It terminates slave threads. However, some CHANGE MASTER etc may still be
    running presently. If a START SLAVE was in progress, the mutex lock below
    will make us wait until slave threads have started, and START SLAVE
    returns, then we terminate them here.

    We can also be called by cleanup(), which only happens if some
    startup parameter to the server was wrong.
  */
  mysql_mutex_lock(&LOCK_active_mi);
  /* This will call terminate_slave_threads() on all connections */
  delete master_info_index;
  master_info_index= 0;
  active_mi= 0;
  mysql_mutex_unlock(&LOCK_active_mi);
  global_rpl_thread_pool.destroy();
  free_all_rpl_filters();
  DBUG_VOID_RETURN;
}

static bool io_slave_killed(Master_info* mi)
{
  DBUG_ENTER("io_slave_killed");

  DBUG_ASSERT(mi->slave_running); // tracking buffer overrun
  DBUG_RETURN(mi->abort_slave || abort_loop || mi->io_thd->killed);
}

/**
   The function analyzes a possible killed status and makes
   a decision whether to accept it or not.
   Normally upon accepting the sql thread goes to shutdown.
   In the event of deffering decision @rli->last_event_start_time waiting
   timer is set to force the killed status be accepted upon its expiration.

   @param thd   pointer to a THD instance
   @param rli   pointer to Relay_log_info instance

   @return TRUE the killed status is recognized, FALSE a possible killed
           status is deferred.
*/
static bool sql_slave_killed(rpl_group_info *rgi)
{
  bool ret= FALSE;
  Relay_log_info *rli= rgi->rli;
  THD *thd= rgi->thd;
  DBUG_ENTER("sql_slave_killed");

  DBUG_ASSERT(rli->sql_driver_thd == thd);
  DBUG_ASSERT(rli->slave_running == 1);// tracking buffer overrun
  if (abort_loop || rli->sql_driver_thd->killed || rli->abort_slave)
  {
    /*
      The transaction should always be binlogged if OPTION_KEEP_LOG is
      set (it implies that something can not be rolled back). And such
      case should be regarded similarly as modifing a
      non-transactional table because retrying of the transaction will
      lead to an error or inconsistency as well.

      Example: OPTION_KEEP_LOG is set if a temporary table is created
      or dropped.

      Note that transaction.all.modified_non_trans_table may be 1
      if last statement was a single row transaction without begin/end.
      Testing this flag must always be done in connection with
      rli->is_in_group().
    */

    if ((thd->transaction.all.modified_non_trans_table ||
         (thd->variables.option_bits & OPTION_KEEP_LOG)) &&
        rli->is_in_group())
    {
      char msg_stopped[]=
        "... Slave SQL Thread stopped with incomplete event group "
        "having non-transactional changes. "
        "If the group consists solely of row-based events, you can try "
        "to restart the slave with --slave-exec-mode=IDEMPOTENT, which "
        "ignores duplicate key, key not found, and similar errors (see "
        "documentation for details).";

      DBUG_PRINT("info", ("modified_non_trans_table: %d  OPTION_BEGIN: %d  "
                          "OPTION_KEEP_LOG: %d  is_in_group: %d",
                          thd->transaction.all.modified_non_trans_table,
                          MY_TEST(thd->variables.option_bits & OPTION_BEGIN),
                          MY_TEST(thd->variables.option_bits & OPTION_KEEP_LOG),
                          rli->is_in_group()));

      if (rli->abort_slave)
      {
        DBUG_PRINT("info",
                   ("Request to stop slave SQL Thread received while "
                    "applying a group that has non-transactional "
                    "changes; waiting for completion of the group ... "));

        /*
          Slave sql thread shutdown in face of unfinished group
          modified Non-trans table is handled via a timer. The slave
          may eventually give out to complete the current group and in
          that case there might be issues at consequent slave restart,
          see the error message.  WL#2975 offers a robust solution
          requiring to store the last exectuted event's coordinates
          along with the group's coordianates instead of waiting with
          @c last_event_start_time the timer.
        */

        if (rgi->last_event_start_time == 0)
          rgi->last_event_start_time= my_time(0);
        ret= difftime(my_time(0), rgi->last_event_start_time) <=
          SLAVE_WAIT_GROUP_DONE ? FALSE : TRUE;

        DBUG_EXECUTE_IF("stop_slave_middle_group", 
                        DBUG_EXECUTE_IF("incomplete_group_in_relay_log",
                                        ret= TRUE;);); // time is over

        if (ret == 0)
        {
          rli->report(WARNING_LEVEL, 0, rgi->gtid_info(),
                      "Request to stop slave SQL Thread received while "
                      "applying a group that has non-transactional "
                      "changes; waiting for completion of the group ... ");
        }
        else
        {
          rli->report(ERROR_LEVEL, ER_SLAVE_FATAL_ERROR, rgi->gtid_info(),
                      ER_THD(thd, ER_SLAVE_FATAL_ERROR), msg_stopped);
        }
      }
      else
      {
        ret= TRUE;
        rli->report(ERROR_LEVEL, ER_SLAVE_FATAL_ERROR, rgi->gtid_info(),
                    ER_THD(thd, ER_SLAVE_FATAL_ERROR),
                    msg_stopped);
      }
    }
    else
    {
      ret= TRUE;
    }
  }
  if (ret)
    rgi->last_event_start_time= 0;
  
  DBUG_RETURN(ret);
}


/*
  skip_load_data_infile()

  NOTES
    This is used to tell a 3.23 master to break send_file()
*/

void skip_load_data_infile(NET *net)
{
  DBUG_ENTER("skip_load_data_infile");

  (void)net_request_file(net, "/dev/null");
  (void)my_net_read(net);                               // discard response
  (void)net_write_command(net, 0, (uchar*) "", 0, (uchar*) "", 0); // ok
  DBUG_VOID_RETURN;
}


bool net_request_file(NET* net, const char* fname)
{
  DBUG_ENTER("net_request_file");
  DBUG_RETURN(net_write_command(net, 251, (uchar*) fname, strlen(fname),
                                (uchar*) "", 0));
}

/*
  From other comments and tests in code, it looks like
  sometimes Query_log_event and Load_log_event can have db == 0
  (see rewrite_db() above for example)
  (cases where this happens are unclear; it may be when the master is 3.23).
*/

const char *print_slave_db_safe(const char* db)
{
  DBUG_ENTER("*print_slave_db_safe");

  DBUG_RETURN((db ? db : ""));
}

#endif /* HAVE_REPLICATION */

int init_strvar_from_file(char *var, int max_size, IO_CACHE *f,
                                 const char *default_val)
{
  uint length;
  DBUG_ENTER("init_strvar_from_file");

  if ((length=my_b_gets(f,var, max_size)))
  {
    char* last_p = var + length -1;
    if (*last_p == '\n')
      *last_p = 0; // if we stopped on newline, kill it
    else
    {
      /*
        If we truncated a line or stopped on last char, remove all chars
        up to and including newline.
      */
      int c;
      while (((c=my_b_get(f)) != '\n' && c != my_b_EOF)) ;
    }
    DBUG_RETURN(0);
  }
  else if (default_val)
  {
    strmake(var,  default_val, max_size-1);
    DBUG_RETURN(0);
  }
  DBUG_RETURN(1);
}

/*
  when moving these functions to mysys, don't forget to
  remove slave.cc from libmysqld/CMakeLists.txt
*/
int init_intvar_from_file(int* var, IO_CACHE* f, int default_val)
{
  char buf[32];
  DBUG_ENTER("init_intvar_from_file");


  if (my_b_gets(f, buf, sizeof(buf)))
  {
    *var = atoi(buf);
    DBUG_RETURN(0);
  }
  else if (default_val)
  {
    *var = default_val;
    DBUG_RETURN(0);
  }
  DBUG_RETURN(1);
}

int init_floatvar_from_file(float* var, IO_CACHE* f, float default_val)
{
  char buf[16];
  DBUG_ENTER("init_floatvar_from_file");


  if (my_b_gets(f, buf, sizeof(buf)))
  {
    if (sscanf(buf, "%f", var) != 1)
      DBUG_RETURN(1);
    else
      DBUG_RETURN(0);
  }
  else if (default_val != 0.0)
  {
    *var = default_val;
    DBUG_RETURN(0);
  }
  DBUG_RETURN(1);
}


/**
   A master info read method

   This function is called from @c init_master_info() along with
   relatives to restore some of @c active_mi members.
   Particularly, this function is responsible for restoring
   IGNORE_SERVER_IDS list of servers whose events the slave is
   going to ignore (to not log them in the relay log).
   Items being read are supposed to be decimal output of values of a
   type shorter or equal of @c long and separated by the single space.
   It also used to restore DO_DOMAIN_IDS & IGNORE_DOMAIN_IDS lists.

   @param arr         @c DYNAMIC_ARRAY pointer to storage for servers id
   @param f           @c IO_CACHE pointer to the source file

   @retval 0         All OK
   @retval non-zero  An error
*/

int init_dynarray_intvar_from_file(DYNAMIC_ARRAY* arr, IO_CACHE* f)
{
  int ret= 0;
  char buf[16 * (sizeof(long)*4 + 1)]; // static buffer to use most of times
  char *buf_act= buf; // actual buffer can be dynamic if static is short
  char *token, *last;
  uint num_items;     // number of items of `arr'
  size_t read_size;
  DBUG_ENTER("init_dynarray_intvar_from_file");

  if ((read_size= my_b_gets(f, buf_act, sizeof(buf))) == 0)
  {
    DBUG_RETURN(0);                             // no line in master.info
  }
  if (read_size + 1 == sizeof(buf) && buf[sizeof(buf) - 2] != '\n')
  {
    /*
      short read happend; allocate sufficient memory and make the 2nd read
    */
    char buf_work[(sizeof(long)*3 + 1)*16];
    memcpy(buf_work, buf, sizeof(buf_work));
    num_items= atoi(strtok_r(buf_work, " ", &last));
    size_t snd_size;
    /*
      max size lower bound approximate estimation bases on the formula:
      (the items number + items themselves) * 
          (decimal size + space) - 1 + `\n' + '\0'
    */
    size_t max_size= (1 + num_items) * (sizeof(long)*3 + 1) + 1;
    buf_act= (char*) my_malloc(max_size, MYF(MY_WME));
    memcpy(buf_act, buf, read_size);
    snd_size= my_b_gets(f, buf_act + read_size, max_size - read_size);
    if (snd_size == 0 ||
        ((snd_size + 1 == max_size - read_size) &&  buf_act[max_size - 2] != '\n'))
    {
      /*
        failure to make the 2nd read or short read again
      */
      ret= 1;
      goto err;
    }
  }
  token= strtok_r(buf_act, " ", &last);
  if (token == NULL)
  {
    ret= 1;
    goto err;
  }
  num_items= atoi(token);
  for (uint i=0; i < num_items; i++)
  {
    token= strtok_r(NULL, " ", &last);
    if (token == NULL)
    {
      ret= 1;
      goto err;
    }
    else
    {
      ulong val= atol(token);
      insert_dynamic(arr, (uchar *) &val);
    }
  }
err:
  if (buf_act != buf)
    my_free(buf_act);
  DBUG_RETURN(ret);
}

#ifdef HAVE_REPLICATION

/*
  Check if the error is caused by network.
  @param[in]   errorno   Number of the error.
  RETURNS:
  TRUE         network error
  FALSE        not network error
*/

bool is_network_error(uint errorno)
{ 
  if (errorno == CR_CONNECTION_ERROR || 
      errorno == CR_CONN_HOST_ERROR ||
      errorno == CR_SERVER_GONE_ERROR ||
      errorno == CR_SERVER_LOST ||
      errorno == ER_CON_COUNT_ERROR ||
      errorno == ER_CONNECTION_KILLED ||
      errorno == ER_NEW_ABORTING_CONNECTION ||
      errorno == ER_NET_READ_INTERRUPTED ||
      errorno == ER_SERVER_SHUTDOWN)
    return TRUE;

  return FALSE;   
}


/*
  Note that we rely on the master's version (3.23, 4.0.14 etc) instead of
  relying on the binlog's version. This is not perfect: imagine an upgrade
  of the master without waiting that all slaves are in sync with the master;
  then a slave could be fooled about the binlog's format. This is what happens
  when people upgrade a 3.23 master to 4.0 without doing RESET MASTER: 4.0
  slaves are fooled. So we do this only to distinguish between 3.23 and more
  recent masters (it's too late to change things for 3.23).

  RETURNS
  0       ok
  1       error
  2       transient network problem, the caller should try to reconnect
*/

static int get_master_version_and_clock(MYSQL* mysql, Master_info* mi)
{
  char err_buff[MAX_SLAVE_ERRMSG], err_buff2[MAX_SLAVE_ERRMSG];
  const char* errmsg= 0;
  int err_code= 0;
  MYSQL_RES *master_res= 0;
  MYSQL_ROW master_row;
  uint version= mysql_get_server_version(mysql) / 10000;
  DBUG_ENTER("get_master_version_and_clock");

  /*
    Free old description_event_for_queue (that is needed if we are in
    a reconnection).
  */
  delete mi->rli.relay_log.description_event_for_queue;
  mi->rli.relay_log.description_event_for_queue= 0;

  if (!my_isdigit(&my_charset_bin,*mysql->server_version))
  {
    errmsg= err_buff2;
    snprintf(err_buff2, sizeof(err_buff2),
             "Master reported unrecognized MySQL version: %s",
             mysql->server_version);
    err_code= ER_SLAVE_FATAL_ERROR;
    sprintf(err_buff, ER_DEFAULT(err_code), err_buff2);
  }
  else
  {
    /*
      Note the following switch will bug when we have MySQL branch 30 ;)
    */
    switch (version) {
    case 0:
    case 1:
    case 2:
      errmsg= err_buff2;
      snprintf(err_buff2, sizeof(err_buff2),
               "Master reported unrecognized MySQL version: %s",
               mysql->server_version);
      err_code= ER_SLAVE_FATAL_ERROR;
      sprintf(err_buff, ER_DEFAULT(err_code), err_buff2);
      break;
    case 3:
      mi->rli.relay_log.description_event_for_queue= new
        Format_description_log_event(1, mysql->server_version);
      break;
    case 4:
      mi->rli.relay_log.description_event_for_queue= new
        Format_description_log_event(3, mysql->server_version);
      break;
    default:
      /*
        Master is MySQL >=5.0. Give a default Format_desc event, so that we can
        take the early steps (like tests for "is this a 3.23 master") which we
        have to take before we receive the real master's Format_desc which will
        override this one. Note that the Format_desc we create below is garbage
        (it has the format of the *slave*); it's only good to help know if the
        master is 3.23, 4.0, etc.
      */
      mi->rli.relay_log.description_event_for_queue= new
        Format_description_log_event(4, mysql->server_version);
      break;
    }
  }

  /*
     This does not mean that a 5.0 slave will be able to read a 6.0 master; but
     as we don't know yet, we don't want to forbid this for now. If a 5.0 slave
     can't read a 6.0 master, this will show up when the slave can't read some
     events sent by the master, and there will be error messages.
  */

  if (errmsg)
    goto err;

  /* as we are here, we tried to allocate the event */
  if (!mi->rli.relay_log.description_event_for_queue)
  {
    errmsg= "default Format_description_log_event";
    err_code= ER_SLAVE_CREATE_EVENT_FAILURE;
    sprintf(err_buff, ER_DEFAULT(err_code), errmsg);
    goto err;
  }

  /*
    FD_q's (A) is set initially from RL's (A): FD_q.(A) := RL.(A).
    It's necessary to adjust FD_q.(A) at this point because in the following
    course FD_q is going to be dumped to RL.
    Generally FD_q is derived from a received FD_m (roughly FD_q := FD_m) 
    in queue_event and the master's (A) is installed.
    At one step with the assignment the Relay-Log's checksum alg is set to 
    a new value: RL.(A) := FD_q.(A). If the slave service is stopped
    the last time assigned RL.(A) will be passed over to the restarting
    service (to the current execution point).
    RL.A is a "codec" to verify checksum in queue_event() almost all the time
    the first fake Rotate event.
    Starting from this point IO thread will executes the following checksum
    warmup sequence  of actions:

    FD_q.A := RL.A,
    A_m^0 := master.@@global.binlog_checksum,
    {queue_event(R_f): verifies(R_f, A_m^0)},
    {queue_event(FD_m): verifies(FD_m, FD_m.A), dump(FD_q), rotate(RL),
                        FD_q := FD_m, RL.A := FD_q.A)}

    See legends definition on MYSQL_BIN_LOG::relay_log_checksum_alg
    docs lines (binlog.h).
    In above A_m^0 - the value of master's
    @@binlog_checksum determined in the upcoming handshake (stored in
    mi->checksum_alg_before_fd).


    After the warm-up sequence IO gets to "normal" checksum verification mode
    to use RL.A in 
    
    {queue_event(E_m): verifies(E_m, RL.A)}

    until it has received a new FD_m.
  */
  mi->rli.relay_log.description_event_for_queue->checksum_alg=
    mi->rli.relay_log.relay_log_checksum_alg;

  DBUG_ASSERT(mi->rli.relay_log.description_event_for_queue->checksum_alg !=
              BINLOG_CHECKSUM_ALG_UNDEF);
  DBUG_ASSERT(mi->rli.relay_log.relay_log_checksum_alg !=
              BINLOG_CHECKSUM_ALG_UNDEF); 
  /*
    Compare the master and slave's clock. Do not die if master's clock is
    unavailable (very old master not supporting UNIX_TIMESTAMP()?).
  */

#ifdef ENABLED_DEBUG_SYNC
  DBUG_EXECUTE_IF("dbug.before_get_UNIX_TIMESTAMP",
                  {
                    const char act[]=
                      "now "
                      "wait_for signal.get_unix_timestamp";
                    DBUG_ASSERT(debug_sync_service);
                    DBUG_ASSERT(!debug_sync_set_action(current_thd,
                                                       STRING_WITH_LEN(act)));
                  };);
#endif

  master_res= NULL;
  if (!mysql_real_query(mysql, STRING_WITH_LEN("SELECT UNIX_TIMESTAMP()")) &&
      (master_res= mysql_store_result(mysql)) &&
      (master_row= mysql_fetch_row(master_res)))
  {
    mi->clock_diff_with_master=
      (long) (time((time_t*) 0) - strtoul(master_row[0], 0, 10));
  }
  else if (check_io_slave_killed(mi, NULL))
    goto slave_killed_err;
  else if (is_network_error(mysql_errno(mysql)))
  {
    mi->report(WARNING_LEVEL, mysql_errno(mysql), NULL,
               "Get master clock failed with error: %s", mysql_error(mysql));
    goto network_err;
  }
  else 
  {
    mi->clock_diff_with_master= 0; /* The "most sensible" value */
    sql_print_warning("\"SELECT UNIX_TIMESTAMP()\" failed on master, "
                      "do not trust column Seconds_Behind_Master of SHOW "
                      "SLAVE STATUS. Error: %s (%d)",
                      mysql_error(mysql), mysql_errno(mysql));
  }
  if (master_res)
  {
    mysql_free_result(master_res);
    master_res= NULL;
  }

  /*
    Check that the master's server id and ours are different. Because if they
    are equal (which can result from a simple copy of master's datadir to slave,
    thus copying some my.cnf), replication will work but all events will be
    skipped.
    Do not die if SHOW VARIABLES LIKE 'SERVER_ID' fails on master (very old
    master?).
    Note: we could have put a @@SERVER_ID in the previous SELECT
    UNIX_TIMESTAMP() instead, but this would not have worked on 3.23 masters.
  */
#ifdef ENABLED_DEBUG_SYNC
  DBUG_EXECUTE_IF("dbug.before_get_SERVER_ID",
                  {
                    const char act[]=
                      "now "
                      "wait_for signal.get_server_id";
                    DBUG_ASSERT(debug_sync_service);
                    DBUG_ASSERT(!debug_sync_set_action(current_thd, 
                                                       STRING_WITH_LEN(act)));
                  };);
#endif
  master_res= NULL;
  master_row= NULL;
  if (!mysql_real_query(mysql,
                        STRING_WITH_LEN("SHOW VARIABLES LIKE 'SERVER_ID'")) &&
      (master_res= mysql_store_result(mysql)) &&
      (master_row= mysql_fetch_row(master_res)))
  {
    if ((global_system_variables.server_id ==
             (mi->master_id= strtoul(master_row[1], 0, 10))) &&
        !mi->rli.replicate_same_server_id)
    {
      errmsg= "The slave I/O thread stops because master and slave have equal \
MySQL server ids; these ids must be different for replication to work (or \
the --replicate-same-server-id option must be used on slave but this does \
not always make sense; please check the manual before using it).";
      err_code= ER_SLAVE_FATAL_ERROR;
      sprintf(err_buff, ER_DEFAULT(err_code), errmsg);
      goto err;
    }
  }
  else if (mysql_errno(mysql))
  {
    if (check_io_slave_killed(mi, NULL))
      goto slave_killed_err;
    else if (is_network_error(mysql_errno(mysql)))
    {
      mi->report(WARNING_LEVEL, mysql_errno(mysql), NULL,
                 "Get master SERVER_ID failed with error: %s", mysql_error(mysql));
      goto network_err;
    }
    /* Fatal error */
    errmsg= "The slave I/O thread stops because a fatal error is encountered \
when it try to get the value of SERVER_ID variable from master.";
    err_code= mysql_errno(mysql);
    sprintf(err_buff, "%s Error: %s", errmsg, mysql_error(mysql));
    goto err;
  }
  else if (!master_row && master_res)
  {
    mi->report(WARNING_LEVEL, ER_UNKNOWN_SYSTEM_VARIABLE, NULL,
               "Unknown system variable 'SERVER_ID' on master, \
maybe it is a *VERY OLD MASTER*.");
  }
  if (master_res)
  {
    mysql_free_result(master_res);
    master_res= NULL;
  }
  if (mi->master_id == 0 && mi->ignore_server_ids.elements > 0)
  {
    errmsg= "Slave configured with server id filtering could not detect the master server id.";
    err_code= ER_SLAVE_FATAL_ERROR;
    sprintf(err_buff, ER_DEFAULT(err_code), errmsg);
    goto err;
  }

  /*
    Check that the master's global character_set_server and ours are the same.
    Not fatal if query fails (old master?).
    Note that we don't check for equality of global character_set_client and
    collation_connection (neither do we prevent their setting in
    set_var.cc). That's because from what I (Guilhem) have tested, the global
    values of these 2 are never used (new connections don't use them).
    We don't test equality of global collation_database either as it's is
    going to be deprecated (made read-only) in 4.1 very soon.
    The test is only relevant if master < 5.0.3 (we'll test only if it's older
    than the 5 branch; < 5.0.3 was alpha...), as >= 5.0.3 master stores
    charset info in each binlog event.
    We don't do it for 3.23 because masters <3.23.50 hang on
    SELECT @@unknown_var (BUG#7965 - see changelog of 3.23.50). So finally we
    test only if master is 4.x.
  */

  /* redundant with rest of code but safer against later additions */
  if (version == 3)
    goto err;

  if (version == 4)
  {
    master_res= NULL;
    if (!mysql_real_query(mysql,
                          STRING_WITH_LEN("SELECT @@GLOBAL.COLLATION_SERVER")) &&
        (master_res= mysql_store_result(mysql)) &&
        (master_row= mysql_fetch_row(master_res)))
    {
      if (strcmp(master_row[0], global_system_variables.collation_server->name))
      {
        errmsg= "The slave I/O thread stops because master and slave have \
different values for the COLLATION_SERVER global variable. The values must \
be equal for the Statement-format replication to work";
        err_code= ER_SLAVE_FATAL_ERROR;
        sprintf(err_buff, ER_DEFAULT(err_code), errmsg);
        goto err;
      }
    }
    else if (check_io_slave_killed(mi, NULL))
      goto slave_killed_err;
    else if (is_network_error(mysql_errno(mysql)))
    {
      mi->report(WARNING_LEVEL, mysql_errno(mysql), NULL,
                 "Get master COLLATION_SERVER failed with error: %s", mysql_error(mysql));
      goto network_err;
    }
    else if (mysql_errno(mysql) != ER_UNKNOWN_SYSTEM_VARIABLE)
    {
      /* Fatal error */
      errmsg= "The slave I/O thread stops because a fatal error is encountered \
when it try to get the value of COLLATION_SERVER global variable from master.";
      err_code= mysql_errno(mysql);
      sprintf(err_buff, "%s Error: %s", errmsg, mysql_error(mysql));
      goto err;
    }
    else
      mi->report(WARNING_LEVEL, ER_UNKNOWN_SYSTEM_VARIABLE, NULL,
                 "Unknown system variable 'COLLATION_SERVER' on master, \
maybe it is a *VERY OLD MASTER*. *NOTE*: slave may experience \
inconsistency if replicated data deals with collation.");

    if (master_res)
    {
      mysql_free_result(master_res);
      master_res= NULL;
    }
  }

  /*
    Perform analogous check for time zone. Theoretically we also should
    perform check here to verify that SYSTEM time zones are the same on
    slave and master, but we can't rely on value of @@system_time_zone
    variable (it is time zone abbreviation) since it determined at start
    time and so could differ for slave and master even if they are really
    in the same system time zone. So we are omiting this check and just
    relying on documentation. Also according to Monty there are many users
    who are using replication between servers in various time zones. Hence
    such check will broke everything for them. (And now everything will
    work for them because by default both their master and slave will have
    'SYSTEM' time zone).
    This check is only necessary for 4.x masters (and < 5.0.4 masters but
    those were alpha).
  */
  if (version == 4)
  {
    master_res= NULL;
    if (!mysql_real_query(mysql, STRING_WITH_LEN("SELECT @@GLOBAL.TIME_ZONE")) &&
        (master_res= mysql_store_result(mysql)) &&
        (master_row= mysql_fetch_row(master_res)))
    {
      if (strcmp(master_row[0],
                 global_system_variables.time_zone->get_name()->ptr()))
      {
        errmsg= "The slave I/O thread stops because master and slave have \
different values for the TIME_ZONE global variable. The values must \
be equal for the Statement-format replication to work";
        err_code= ER_SLAVE_FATAL_ERROR;
        sprintf(err_buff, ER_DEFAULT(err_code), errmsg);
        goto err;
      }
    }
    else if (check_io_slave_killed(mi, NULL))
      goto slave_killed_err;
    else if (is_network_error(err_code= mysql_errno(mysql)))
    {
      mi->report(ERROR_LEVEL, err_code, NULL,
                 "Get master TIME_ZONE failed with error: %s",
                 mysql_error(mysql));
      goto network_err;
    }
    else if (err_code == ER_UNKNOWN_SYSTEM_VARIABLE)
    {
      /* We use ERROR_LEVEL to get the error logged to file */
      mi->report(ERROR_LEVEL, err_code, NULL,

                 "MySQL master doesn't have a TIME_ZONE variable. Note that"
                 "if your timezone is not same between master and slave, your "
                 "slave may get wrong data into timestamp columns");
    }
    else
    {
      /* Fatal error */
      errmsg= "The slave I/O thread stops because a fatal error is encountered \
when it try to get the value of TIME_ZONE global variable from master.";
      sprintf(err_buff, "%s Error: %s", errmsg, mysql_error(mysql));
      goto err;
    }
    if (master_res)
    {
      mysql_free_result(master_res);
      master_res= NULL;
    }
  }

  if (mi->heartbeat_period != 0.0)
  {
    const char query_format[]= "SET @master_heartbeat_period= %llu";
    char query[sizeof(query_format) + 32];
    /* 
       the period is an ulonglong of nano-secs. 
    */
    my_snprintf(query, sizeof(query), query_format,
                (ulonglong) (mi->heartbeat_period*1000000000UL));

    DBUG_EXECUTE_IF("simulate_slave_heartbeat_network_error",
                    { static ulong dbug_count= 0;
                      if (++dbug_count < 3)
                        goto heartbeat_network_error;
                    });
    if (mysql_real_query(mysql, query, strlen(query)))
    {
      if (check_io_slave_killed(mi, NULL))
        goto slave_killed_err;

      if (is_network_error(mysql_errno(mysql)))
      {
      IF_DBUG(heartbeat_network_error: , )
        mi->report(WARNING_LEVEL, mysql_errno(mysql), NULL,
                   "SET @master_heartbeat_period to master failed with error: %s",
                   mysql_error(mysql));
        mysql_free_result(mysql_store_result(mysql));
        goto network_err;
      }
      else
      {
        /* Fatal error */
        errmsg= "The slave I/O thread stops because a fatal error is encountered "
          "when it tries to SET @master_heartbeat_period on master.";
        err_code= ER_SLAVE_FATAL_ERROR;
        sprintf(err_buff, "%s Error: %s", errmsg, mysql_error(mysql));
        mysql_free_result(mysql_store_result(mysql));
        goto err;
      }
    }
    mysql_free_result(mysql_store_result(mysql));
  }
 
  /*
    Querying if master is capable to checksum and notifying it about own
    CRC-awareness. The master's side instant value of @@global.binlog_checksum 
    is stored in the dump thread's uservar area as well as cached locally
    to become known in consensus by master and slave.
  */
  DBUG_EXECUTE_IF("simulate_slave_unaware_checksum",
                  mi->checksum_alg_before_fd= BINLOG_CHECKSUM_ALG_OFF;
                  goto past_checksum;);
  {
    int rc;
    const char query[]= "SET @master_binlog_checksum= @@global.binlog_checksum";
    master_res= NULL;
    mi->checksum_alg_before_fd= BINLOG_CHECKSUM_ALG_UNDEF; //initially undefined
    /*
      @c checksum_alg_before_fd is queried from master in this block.
      If master is old checksum-unaware the value stays undefined.
      Once the first FD will be received its alg descriptor will replace
      the being queried one.
    */
    rc= mysql_real_query(mysql, query, strlen(query));
    if (rc != 0)
    {
      if (check_io_slave_killed(mi, NULL))
        goto slave_killed_err;

      if (mysql_errno(mysql) == ER_UNKNOWN_SYSTEM_VARIABLE)
      {
        /* Ignore this expected error if not a high error level */
        if (global_system_variables.log_warnings > 1)
        {
          // this is tolerable as OM -> NS is supported
          mi->report(WARNING_LEVEL, mysql_errno(mysql), NULL,
                     "Notifying master by %s failed with "
                     "error: %s", query, mysql_error(mysql));
        }
      }
      else
      {
        if (is_network_error(mysql_errno(mysql)))
        {
          mi->report(WARNING_LEVEL, mysql_errno(mysql), NULL,
                     "Notifying master by %s failed with "
                     "error: %s", query, mysql_error(mysql));
          mysql_free_result(mysql_store_result(mysql));
          goto network_err;
        }
        else
        {
          errmsg= "The slave I/O thread stops because a fatal error is encountered "
            "when it tried to SET @master_binlog_checksum on master.";
          err_code= ER_SLAVE_FATAL_ERROR;
          sprintf(err_buff, "%s Error: %s", errmsg, mysql_error(mysql));
          mysql_free_result(mysql_store_result(mysql));
          goto err;
        }
      }
    }
    else
    {
      mysql_free_result(mysql_store_result(mysql));
      if (!mysql_real_query(mysql,
                            STRING_WITH_LEN("SELECT @master_binlog_checksum")) &&
          (master_res= mysql_store_result(mysql)) &&
          (master_row= mysql_fetch_row(master_res)) &&
          (master_row[0] != NULL))
      {
        mi->checksum_alg_before_fd= (enum_binlog_checksum_alg)
          (find_type(master_row[0], &binlog_checksum_typelib, 1) - 1);
        // valid outcome is either of
        DBUG_ASSERT(mi->checksum_alg_before_fd == BINLOG_CHECKSUM_ALG_OFF ||
                    mi->checksum_alg_before_fd == BINLOG_CHECKSUM_ALG_CRC32);
      }
      else if (check_io_slave_killed(mi, NULL))
        goto slave_killed_err;
      else if (is_network_error(mysql_errno(mysql)))
      {
        mi->report(WARNING_LEVEL, mysql_errno(mysql), NULL,
                   "Get master BINLOG_CHECKSUM failed with error: %s", mysql_error(mysql));
        goto network_err;
      }
      else
      {
        errmsg= "The slave I/O thread stops because a fatal error is encountered "
          "when it tried to SELECT @master_binlog_checksum.";
        err_code= ER_SLAVE_FATAL_ERROR;
        sprintf(err_buff, "%s Error: %s", errmsg, mysql_error(mysql));
        mysql_free_result(mysql_store_result(mysql));
        goto err;
      }
    }
    if (master_res)
    {
      mysql_free_result(master_res);
      master_res= NULL;
    }
  }

#ifndef DBUG_OFF
past_checksum:
#endif

  /*
    Request the master to filter away events with the @@skip_replication flag
    set, if we are running with
    --replicate-events-marked-for-skip=FILTER_ON_MASTER.
  */
  if (opt_replicate_events_marked_for_skip == RPL_SKIP_FILTER_ON_MASTER)
  {
    if (mysql_real_query(mysql, STRING_WITH_LEN("SET skip_replication=1")))
    {
      err_code= mysql_errno(mysql);
      if (is_network_error(err_code))
      {
        mi->report(ERROR_LEVEL, err_code, NULL,
                   "Setting master-side filtering of @@skip_replication failed "
                   "with error: %s", mysql_error(mysql));
        goto network_err;
      }
      else if (err_code == ER_UNKNOWN_SYSTEM_VARIABLE)
      {
        /*
          The master is older than the slave and does not support the
          @@skip_replication feature.
          This is not a problem, as such master will not generate events with
          the @@skip_replication flag set in the first place. We will still
          do slave-side filtering of such events though, to handle the (rare)
          case of downgrading a master and receiving old events generated from
          before the downgrade with the @@skip_replication flag set.
        */
        DBUG_PRINT("info", ("Old master does not support master-side filtering "
                            "of @@skip_replication events."));
      }
      else
      {
        /* Fatal error */
        errmsg= "The slave I/O thread stops because a fatal error is "
          "encountered when it tries to request filtering of events marked "
          "with the @@skip_replication flag.";
        sprintf(err_buff, "%s Error: %s", errmsg, mysql_error(mysql));
        goto err;
      }
    }
  }

  /* Announce MariaDB slave capabilities. */
  DBUG_EXECUTE_IF("simulate_slave_capability_none", goto after_set_capability;);
  {
    int rc= DBUG_EVALUATE_IF("simulate_slave_capability_old_53",
        mysql_real_query(mysql, STRING_WITH_LEN("SET @mariadb_slave_capability="
                         STRINGIFY_ARG(MARIA_SLAVE_CAPABILITY_ANNOTATE))),
        mysql_real_query(mysql, STRING_WITH_LEN("SET @mariadb_slave_capability="
                         STRINGIFY_ARG(MARIA_SLAVE_CAPABILITY_MINE))));
    if (rc)
    {
      err_code= mysql_errno(mysql);
      if (is_network_error(err_code))
      {
        mi->report(ERROR_LEVEL, err_code, NULL,
                   "Setting @mariadb_slave_capability failed with error: %s",
                   mysql_error(mysql));
        goto network_err;
      }
      else
      {
        /* Fatal error */
        errmsg= "The slave I/O thread stops because a fatal error is "
          "encountered when it tries to set @mariadb_slave_capability.";
        sprintf(err_buff, "%s Error: %s", errmsg, mysql_error(mysql));
        goto err;
      }
    }
  }
#ifndef DBUG_OFF
after_set_capability:
#endif

  if (mi->using_gtid != Master_info::USE_GTID_NO)
  {
    /* Request dump to start from slave replication GTID state. */
    int rc;
    char str_buf[256];
    String query_str(str_buf, sizeof(str_buf), system_charset_info);
    query_str.length(0);

    /*
      Read the master @@GLOBAL.gtid_domain_id variable.
      This is mostly to check that master is GTID aware, but we could later
      perhaps use it to check that different multi-source masters are correctly
      configured with distinct domain_id.
    */
    if (mysql_real_query(mysql,
                         STRING_WITH_LEN("SELECT @@GLOBAL.gtid_domain_id")) ||
        !(master_res= mysql_store_result(mysql)) ||
        !(master_row= mysql_fetch_row(master_res)))
    {
      err_code= mysql_errno(mysql);
      if (is_network_error(err_code))
      {
        mi->report(ERROR_LEVEL, err_code, NULL,
                   "Get master @@GLOBAL.gtid_domain_id failed with error: %s",
                   mysql_error(mysql));
        goto network_err;
      }
      else
      {
        errmsg= "The slave I/O thread stops because master does not support "
          "MariaDB global transaction id. A fatal error is encountered when "
          "it tries to SELECT @@GLOBAL.gtid_domain_id.";
        sprintf(err_buff, "%s Error: %s", errmsg, mysql_error(mysql));
        goto err;
      }
    }
    mysql_free_result(master_res);
    master_res= NULL;

    query_str.append(STRING_WITH_LEN("SET @slave_connect_state='"),
                     system_charset_info);
    if (mi->gtid_current_pos.append_to_string(&query_str))
    {
      err_code= ER_OUTOFMEMORY;
      errmsg= "The slave I/O thread stops because a fatal out-of-memory "
        "error is encountered when it tries to compute @slave_connect_state.";
      sprintf(err_buff, "%s Error: Out of memory", errmsg);
      goto err;
    }
    query_str.append(STRING_WITH_LEN("'"), system_charset_info);

    rc= mysql_real_query(mysql, query_str.ptr(), query_str.length());
    if (rc)
    {
      err_code= mysql_errno(mysql);
      if (is_network_error(err_code))
      {
        mi->report(ERROR_LEVEL, err_code, NULL,
                   "Setting @slave_connect_state failed with error: %s",
                   mysql_error(mysql));
        goto network_err;
      }
      else
      {
        /* Fatal error */
        errmsg= "The slave I/O thread stops because a fatal error is "
          "encountered when it tries to set @slave_connect_state.";
        sprintf(err_buff, "%s Error: %s", errmsg, mysql_error(mysql));
        goto err;
      }
    }

    query_str.length(0);
    if (query_str.append(STRING_WITH_LEN("SET @slave_gtid_strict_mode="),
                         system_charset_info) ||
        query_str.append_ulonglong(opt_gtid_strict_mode != false))
    {
      err_code= ER_OUTOFMEMORY;
      errmsg= "The slave I/O thread stops because a fatal out-of-memory "
        "error is encountered when it tries to set @slave_gtid_strict_mode.";
      sprintf(err_buff, "%s Error: Out of memory", errmsg);
      goto err;
    }

    rc= mysql_real_query(mysql, query_str.ptr(), query_str.length());
    if (rc)
    {
      err_code= mysql_errno(mysql);
      if (is_network_error(err_code))
      {
        mi->report(ERROR_LEVEL, err_code, NULL,
                   "Setting @slave_gtid_strict_mode failed with error: %s",
                   mysql_error(mysql));
        goto network_err;
      }
      else
      {
        /* Fatal error */
        errmsg= "The slave I/O thread stops because a fatal error is "
          "encountered when it tries to set @slave_gtid_strict_mode.";
        sprintf(err_buff, "%s Error: %s", errmsg, mysql_error(mysql));
        goto err;
      }
    }

    query_str.length(0);
    if (query_str.append(STRING_WITH_LEN("SET @slave_gtid_ignore_duplicates="),
                         system_charset_info) ||
        query_str.append_ulonglong(opt_gtid_ignore_duplicates != false))
    {
      err_code= ER_OUTOFMEMORY;
      errmsg= "The slave I/O thread stops because a fatal out-of-memory error "
        "is encountered when it tries to set @slave_gtid_ignore_duplicates.";
      sprintf(err_buff, "%s Error: Out of memory", errmsg);
      goto err;
    }

    rc= mysql_real_query(mysql, query_str.ptr(), query_str.length());
    if (rc)
    {
      err_code= mysql_errno(mysql);
      if (is_network_error(err_code))
      {
        mi->report(ERROR_LEVEL, err_code, NULL,
                   "Setting @slave_gtid_ignore_duplicates failed with "
                   "error: %s", mysql_error(mysql));
        goto network_err;
      }
      else
      {
        /* Fatal error */
        errmsg= "The slave I/O thread stops because a fatal error is "
          "encountered when it tries to set @slave_gtid_ignore_duplicates.";
        sprintf(err_buff, "%s Error: %s", errmsg, mysql_error(mysql));
        goto err;
      }
    }

    if (mi->rli.until_condition == Relay_log_info::UNTIL_GTID)
    {
      query_str.length(0);
      query_str.append(STRING_WITH_LEN("SET @slave_until_gtid='"),
                       system_charset_info);
      if (mi->rli.until_gtid_pos.append_to_string(&query_str))
      {
        err_code= ER_OUTOFMEMORY;
        errmsg= "The slave I/O thread stops because a fatal out-of-memory "
          "error is encountered when it tries to compute @slave_until_gtid.";
        sprintf(err_buff, "%s Error: Out of memory", errmsg);
        goto err;
      }
      query_str.append(STRING_WITH_LEN("'"), system_charset_info);

      rc= mysql_real_query(mysql, query_str.ptr(), query_str.length());
      if (rc)
      {
        err_code= mysql_errno(mysql);
        if (is_network_error(err_code))
        {
          mi->report(ERROR_LEVEL, err_code, NULL,
                     "Setting @slave_until_gtid failed with error: %s",
                     mysql_error(mysql));
          goto network_err;
        }
        else
        {
          /* Fatal error */
          errmsg= "The slave I/O thread stops because a fatal error is "
            "encountered when it tries to set @slave_until_gtid.";
          sprintf(err_buff, "%s Error: %s", errmsg, mysql_error(mysql));
          goto err;
        }
      }
    }
  }
  else
  {
    /*
      If we are not using GTID to connect this time, then instead request
      the corresponding GTID position from the master, so that the user
      can reconnect the next time using MASTER_GTID_POS=AUTO.
    */
    char quote_buf[2*sizeof(mi->master_log_name)+1];
    char str_buf[28+2*sizeof(mi->master_log_name)+10];
    String query(str_buf, sizeof(str_buf), system_charset_info);
    query.length(0);

    query.append("SELECT binlog_gtid_pos('");
    escape_quotes_for_mysql(&my_charset_bin, quote_buf, sizeof(quote_buf),
                            mi->master_log_name, strlen(mi->master_log_name));
    query.append(quote_buf);
    query.append("',");
    query.append_ulonglong(mi->master_log_pos);
    query.append(")");

    if (!mysql_real_query(mysql, query.c_ptr_safe(), query.length()) &&
        (master_res= mysql_store_result(mysql)) &&
        (master_row= mysql_fetch_row(master_res)) &&
        (master_row[0] != NULL))
    {
      rpl_global_gtid_slave_state->load(mi->io_thd, master_row[0],
                                        strlen(master_row[0]), false, false);
    }
    else if (check_io_slave_killed(mi, NULL))
      goto slave_killed_err;
    else if (is_network_error(mysql_errno(mysql)))
    {
      mi->report(WARNING_LEVEL, mysql_errno(mysql), NULL,
                 "Get master GTID position failed with error: %s", mysql_error(mysql));
      goto network_err;
    }
    else
    {
      /*
        ToDo: If the master does not have the binlog_gtid_pos() function, it
        just means that it is an old master with no GTID support, so we should
        do nothing.

        However, if binlog_gtid_pos() exists, but fails or returns NULL, then
        it means that the requested position is not valid. We could use this
        to catch attempts to replicate from within the middle of an event,
        avoiding strange failures or possible corruption.
      */
    }
    if (master_res)
    {
      mysql_free_result(master_res);
      master_res= NULL;
    }
  }

err:
  if (errmsg)
  {
    if (master_res)
      mysql_free_result(master_res);
    DBUG_ASSERT(err_code != 0);
    mi->report(ERROR_LEVEL, err_code, NULL, "%s", err_buff);
    DBUG_RETURN(1);
  }

  DBUG_RETURN(0);

network_err:
  if (master_res)
    mysql_free_result(master_res);
  DBUG_RETURN(2);

slave_killed_err:
  if (master_res)
    mysql_free_result(master_res);
  DBUG_RETURN(2);
}


static bool wait_for_relay_log_space(Relay_log_info* rli)
{
  bool slave_killed=0;
  bool ignore_log_space_limit;
  Master_info* mi = rli->mi;
  PSI_stage_info old_stage;
  THD* thd = mi->io_thd;
  DBUG_ENTER("wait_for_relay_log_space");

  mysql_mutex_lock(&rli->log_space_lock);
  thd->ENTER_COND(&rli->log_space_cond,
                  &rli->log_space_lock,
                  &stage_waiting_for_relay_log_space,
                  &old_stage);
  while (rli->log_space_limit < rli->log_space_total &&
         !(slave_killed=io_slave_killed(mi)) &&
         !rli->ignore_log_space_limit)
    mysql_cond_wait(&rli->log_space_cond, &rli->log_space_lock);

  ignore_log_space_limit= rli->ignore_log_space_limit;
  rli->ignore_log_space_limit= 0;

  thd->EXIT_COND(&old_stage);

  /* 
    Makes the IO thread read only one event at a time
    until the SQL thread is able to purge the relay 
    logs, freeing some space.

    Therefore, once the SQL thread processes this next 
    event, it goes to sleep (no more events in the queue),
    sets ignore_log_space_limit=true and wakes the IO thread. 
    However, this event may have been enough already for 
    the SQL thread to purge some log files, freeing 
    rli->log_space_total .

    This guarantees that the SQL and IO thread move
    forward only one event at a time (to avoid deadlocks), 
    when the relay space limit is reached. It also 
    guarantees that when the SQL thread is prepared to
    rotate (to be able to purge some logs), the IO thread
    will know about it and will rotate.

    NOTE: The ignore_log_space_limit is only set when the SQL
          thread sleeps waiting for events.

   */

  if (ignore_log_space_limit)
  {
#ifndef DBUG_OFF
    {
      DBUG_PRINT("info", ("log_space_limit=%llu log_space_total=%llu "
                          "ignore_log_space_limit=%d "
                          "sql_force_rotate_relay=%d", 
                        rli->log_space_limit, rli->log_space_total,
                        (int) rli->ignore_log_space_limit,
                        (int) rli->sql_force_rotate_relay));
    }
#endif
    if (rli->sql_force_rotate_relay)
    {
      mysql_mutex_lock(&mi->data_lock);
      rotate_relay_log(rli->mi);
      mysql_mutex_unlock(&mi->data_lock);
      rli->sql_force_rotate_relay= false;
    }
  }

  DBUG_RETURN(slave_killed);
}


/*
  Builds a Rotate from the ignored events' info and writes it to relay log.

  SYNOPSIS
  write_ignored_events_info_to_relay_log()
    thd             pointer to I/O thread's thd
    mi

  DESCRIPTION
    Slave I/O thread, going to die, must leave a durable trace of the
    ignored events' end position for the use of the slave SQL thread, by
    calling this function. Only that thread can call it (see assertion).
 */
static void write_ignored_events_info_to_relay_log(THD *thd, Master_info *mi)
{
  Relay_log_info *rli= &mi->rli;
  mysql_mutex_t *log_lock= rli->relay_log.get_log_lock();
  DBUG_ENTER("write_ignored_events_info_to_relay_log");

  DBUG_ASSERT(thd == mi->io_thd);
  mysql_mutex_lock(log_lock);
  if (rli->ign_master_log_name_end[0] || rli->ign_gtids.count())
  {
    Rotate_log_event *rev= NULL;
    Gtid_list_log_event *glev= NULL;
    if (rli->ign_master_log_name_end[0])
    {
      rev= new Rotate_log_event(rli->ign_master_log_name_end,
                                0, rli->ign_master_log_pos_end,
                                Rotate_log_event::DUP_NAME);
      rli->ign_master_log_name_end[0]= 0;
      if (unlikely(!(bool)rev))
        mi->report(ERROR_LEVEL, ER_SLAVE_CREATE_EVENT_FAILURE, NULL,
                   ER_THD(thd, ER_SLAVE_CREATE_EVENT_FAILURE),
                   "Rotate_event (out of memory?),"
                   " SHOW SLAVE STATUS may be inaccurate");
    }
    if (rli->ign_gtids.count())
    {
      glev= new Gtid_list_log_event(&rli->ign_gtids,
                                    Gtid_list_log_event::FLAG_IGN_GTIDS);
      rli->ign_gtids.reset();
      if (unlikely(!(bool)glev))
        mi->report(ERROR_LEVEL, ER_SLAVE_CREATE_EVENT_FAILURE, NULL,
                   ER_THD(thd, ER_SLAVE_CREATE_EVENT_FAILURE),
                   "Gtid_list_event (out of memory?),"
                   " gtid_slave_pos may be inaccurate");
    }

    /* Can unlock before writing as slave SQL thd will soon see our event. */
    mysql_mutex_unlock(log_lock);
    if (rev)
    {
      DBUG_PRINT("info",("writing a Rotate event to track down ignored events"));
      rev->server_id= 0; // don't be ignored by slave SQL thread
      if (unlikely(rli->relay_log.append(rev)))
        mi->report(ERROR_LEVEL, ER_SLAVE_RELAY_LOG_WRITE_FAILURE, NULL,
                   ER_THD(thd, ER_SLAVE_RELAY_LOG_WRITE_FAILURE),
                   "failed to write a Rotate event"
                   " to the relay log, SHOW SLAVE STATUS may be"
                   " inaccurate");
      delete rev;
    }
    if (glev)
    {
      DBUG_PRINT("info",("writing a Gtid_list event to track down ignored events"));
      glev->server_id= 0; // don't be ignored by slave SQL thread
      glev->set_artificial_event(); // Don't mess up Exec_Master_Log_Pos
      if (unlikely(rli->relay_log.append(glev)))
        mi->report(ERROR_LEVEL, ER_SLAVE_RELAY_LOG_WRITE_FAILURE, NULL,
                   ER_THD(thd, ER_SLAVE_RELAY_LOG_WRITE_FAILURE),
                   "failed to write a Gtid_list event to the relay log, "
                   "gtid_slave_pos may be inaccurate");
      delete glev;
    }
    if (likely (rev || glev))
    {
      rli->relay_log.harvest_bytes_written(&rli->log_space_total);
      if (flush_master_info(mi, TRUE, TRUE))
        sql_print_error("Failed to flush master info file");
    }
  }
  else
    mysql_mutex_unlock(log_lock);
  DBUG_VOID_RETURN;
}


int register_slave_on_master(MYSQL* mysql, Master_info *mi,
                             bool *suppress_warnings)
{
  uchar buf[1024], *pos= buf;
  uint report_host_len=0, report_user_len=0, report_password_len=0;
  DBUG_ENTER("register_slave_on_master");

  *suppress_warnings= FALSE;
  if (report_host)
    report_host_len= strlen(report_host);
  if (report_host_len > HOSTNAME_LENGTH)
  {
    sql_print_warning("The length of report_host is %d. "
                      "It is larger than the max length(%d), so this "
                      "slave cannot be registered to the master.",
                      report_host_len, HOSTNAME_LENGTH);
    DBUG_RETURN(0);
  }

  if (report_user)
    report_user_len= strlen(report_user);
  if (report_user_len > USERNAME_LENGTH)
  {
    sql_print_warning("The length of report_user is %d. "
                      "It is larger than the max length(%d), so this "
                      "slave cannot be registered to the master.",
                      report_user_len, USERNAME_LENGTH);
    DBUG_RETURN(0);
  }

  if (report_password)
    report_password_len= strlen(report_password);
  if (report_password_len > MAX_PASSWORD_LENGTH)
  {
    sql_print_warning("The length of report_password is %d. "
                      "It is larger than the max length(%d), so this "
                      "slave cannot be registered to the master.",
                      report_password_len, MAX_PASSWORD_LENGTH);
    DBUG_RETURN(0);
  }

  int4store(pos, global_system_variables.server_id); pos+= 4;
  pos= net_store_data(pos, (uchar*) report_host, report_host_len);
  pos= net_store_data(pos, (uchar*) report_user, report_user_len);
  pos= net_store_data(pos, (uchar*) report_password, report_password_len);
  int2store(pos, (uint16) report_port); pos+= 2;
  /* 
    Fake rpl_recovery_rank, which was removed in BUG#13963,
    so that this server can register itself on old servers,
    see BUG#49259.
   */
  int4store(pos, /* rpl_recovery_rank */ 0);    pos+= 4;
  /* The master will fill in master_id */
  int4store(pos, 0);                    pos+= 4;

  if (simple_command(mysql, COM_REGISTER_SLAVE, buf, (size_t) (pos- buf), 0))
  {
    if (mysql_errno(mysql) == ER_NET_READ_INTERRUPTED)
    {
      *suppress_warnings= TRUE;                 // Suppress reconnect warning
    }
    else if (!check_io_slave_killed(mi, NULL))
    {
      char buf[256];
      my_snprintf(buf, sizeof(buf), "%s (Errno: %d)", mysql_error(mysql), 
                  mysql_errno(mysql));
      mi->report(ERROR_LEVEL, ER_SLAVE_MASTER_COM_FAILURE, NULL,
                 ER(ER_SLAVE_MASTER_COM_FAILURE), "COM_REGISTER_SLAVE", buf);
    }
    DBUG_RETURN(1);
  }
  DBUG_RETURN(0);
}


/**
  Execute a SHOW SLAVE STATUS statement.

  @param thd Pointer to THD object for the client thread executing the
  statement.

  @param mi Pointer to Master_info object for the IO thread.

  @retval FALSE success
  @retval TRUE failure
*/

bool show_master_info(THD *thd, Master_info *mi, bool full)
{
  DBUG_ENTER("show_master_info");
  String gtid_pos;
  List<Item> field_list;

  if (full && rpl_global_gtid_slave_state->tostring(&gtid_pos, NULL, 0))
    DBUG_RETURN(TRUE);
  show_master_info_get_fields(thd, &field_list, full, gtid_pos.length());
  if (thd->protocol->send_result_set_metadata(&field_list,
                       Protocol::SEND_NUM_ROWS | Protocol::SEND_EOF))
    DBUG_RETURN(TRUE);
  if (send_show_master_info_data(thd, mi, full, &gtid_pos))
    DBUG_RETURN(TRUE);
  my_eof(thd);
  DBUG_RETURN(FALSE);
}

void show_master_info_get_fields(THD *thd, List<Item> *field_list,
                                 bool full, size_t gtid_pos_length)
{
  Master_info *mi;
  MEM_ROOT *mem_root= thd->mem_root;
  DBUG_ENTER("show_master_info_get_fields");

  if (full)
  {
    field_list->push_back(new (mem_root)
                          Item_empty_string(thd, "Connection_name",
                                            MAX_CONNECTION_NAME),
                          mem_root);
    field_list->push_back(new (mem_root)
                          Item_empty_string(thd, "Slave_SQL_State", 30),
                          mem_root);
  }

  field_list->push_back(new (mem_root)
                        Item_empty_string(thd, "Slave_IO_State", 30),
                        mem_root);
  field_list->push_back(new (mem_root)
                        Item_empty_string(thd, "Master_Host", sizeof(mi->host)),
                        mem_root);
  field_list->push_back(new (mem_root)
                        Item_empty_string(thd, "Master_User", sizeof(mi->user)),
                        mem_root);
  field_list->push_back(new (mem_root)
                        Item_return_int(thd, "Master_Port", 7, MYSQL_TYPE_LONG),
                        mem_root);
  field_list->push_back(new (mem_root)
                        Item_return_int(thd, "Connect_Retry", 10,
                                        MYSQL_TYPE_LONG),
                        mem_root);
  field_list->push_back(new (mem_root)
                        Item_empty_string(thd, "Master_Log_File", FN_REFLEN),
                        mem_root);
  field_list->push_back(new (mem_root)
                        Item_return_int(thd, "Read_Master_Log_Pos", 10,
                                        MYSQL_TYPE_LONGLONG),
                        mem_root);
  field_list->push_back(new (mem_root)
                        Item_empty_string(thd, "Relay_Log_File", FN_REFLEN),
                        mem_root);
  field_list->push_back(new (mem_root)
                        Item_return_int(thd, "Relay_Log_Pos", 10,
                                        MYSQL_TYPE_LONGLONG),
                        mem_root);
  field_list->push_back(new (mem_root)
                        Item_empty_string(thd, "Relay_Master_Log_File",
                                          FN_REFLEN),
                        mem_root);
  field_list->push_back(new (mem_root)
                        Item_empty_string(thd, "Slave_IO_Running", 3),
                        mem_root);
  field_list->push_back(new (mem_root)
                        Item_empty_string(thd, "Slave_SQL_Running", 3),
                        mem_root);
  field_list->push_back(new (mem_root)
                        Item_empty_string(thd, "Replicate_Do_DB", 20),
                        mem_root);
  field_list->push_back(new (mem_root)
                        Item_empty_string(thd, "Replicate_Ignore_DB", 20),
                        mem_root);
  field_list->push_back(new (mem_root)
                        Item_empty_string(thd, "Replicate_Do_Table", 20),
                        mem_root);
  field_list->push_back(new (mem_root)
                        Item_empty_string(thd, "Replicate_Ignore_Table", 23),
                        mem_root);
  field_list->push_back(new (mem_root)
                        Item_empty_string(thd, "Replicate_Wild_Do_Table", 24),
                        mem_root);
  field_list->push_back(new (mem_root)
                        Item_empty_string(thd, "Replicate_Wild_Ignore_Table",
                                          28),
                        mem_root);
  field_list->push_back(new (mem_root)
                        Item_return_int(thd, "Last_Errno", 4, MYSQL_TYPE_LONG),
                        mem_root);
  field_list->push_back(new (mem_root)
                        Item_empty_string(thd, "Last_Error", 20),
                        mem_root);
  field_list->push_back(new (mem_root)
                        Item_return_int(thd, "Skip_Counter", 10,
                                        MYSQL_TYPE_LONG),
                        mem_root);
  field_list->push_back(new (mem_root)
                        Item_return_int(thd, "Exec_Master_Log_Pos", 10,
                                        MYSQL_TYPE_LONGLONG),
                        mem_root);
  field_list->push_back(new (mem_root)
                        Item_return_int(thd, "Relay_Log_Space", 10,
                                        MYSQL_TYPE_LONGLONG),
                        mem_root);
  field_list->push_back(new (mem_root)
                        Item_empty_string(thd, "Until_Condition", 6),
                        mem_root);
  field_list->push_back(new (mem_root)
                        Item_empty_string(thd, "Until_Log_File", FN_REFLEN),
                        mem_root);
  field_list->push_back(new (mem_root)
                        Item_return_int(thd, "Until_Log_Pos", 10,
                                        MYSQL_TYPE_LONGLONG),
                        mem_root);
  field_list->push_back(new (mem_root)
                        Item_empty_string(thd, "Master_SSL_Allowed", 7),
                        mem_root);
  field_list->push_back(new (mem_root)
                        Item_empty_string(thd, "Master_SSL_CA_File",
                                          sizeof(mi->ssl_ca)),
                        mem_root);
  field_list->push_back(new (mem_root)
                        Item_empty_string(thd, "Master_SSL_CA_Path",
                                          sizeof(mi->ssl_capath)),
                        mem_root);
  field_list->push_back(new (mem_root)
                        Item_empty_string(thd, "Master_SSL_Cert",
                                          sizeof(mi->ssl_cert)),
                        mem_root);
  field_list->push_back(new (mem_root)
                        Item_empty_string(thd, "Master_SSL_Cipher",
                                          sizeof(mi->ssl_cipher)),
                        mem_root);
  field_list->push_back(new (mem_root)
                        Item_empty_string(thd, "Master_SSL_Key",
                                          sizeof(mi->ssl_key)),
                        mem_root);
  field_list->push_back(new (mem_root)
                        Item_return_int(thd, "Seconds_Behind_Master", 10,
                                        MYSQL_TYPE_LONGLONG),
                        mem_root);
  field_list->push_back(new (mem_root)
                        Item_empty_string(thd, "Master_SSL_Verify_Server_Cert",
                                          3),
                        mem_root);
  field_list->push_back(new (mem_root)
                        Item_return_int(thd, "Last_IO_Errno", 4,
                                        MYSQL_TYPE_LONG),
                        mem_root);
  field_list->push_back(new (mem_root)
                        Item_empty_string(thd, "Last_IO_Error", 20),
                        mem_root);
  field_list->push_back(new (mem_root)
                        Item_return_int(thd, "Last_SQL_Errno", 4,
                                        MYSQL_TYPE_LONG),
                        mem_root);
  field_list->push_back(new (mem_root)
                        Item_empty_string(thd, "Last_SQL_Error", 20),
                        mem_root);
  field_list->push_back(new (mem_root)
                        Item_empty_string(thd, "Replicate_Ignore_Server_Ids",
                                          FN_REFLEN),
                        mem_root);
  field_list->push_back(new (mem_root)
                        Item_return_int(thd, "Master_Server_Id", sizeof(ulong),
                                            MYSQL_TYPE_LONG),
                        mem_root);
  field_list->push_back(new (mem_root)
                        Item_empty_string(thd, "Master_SSL_Crl",
                                          sizeof(mi->ssl_crl)),
                        mem_root);
  field_list->push_back(new (mem_root)
                        Item_empty_string(thd, "Master_SSL_Crlpath",
                                          sizeof(mi->ssl_crlpath)),
                        mem_root);
  field_list->push_back(new (mem_root)
                        Item_empty_string(thd, "Using_Gtid",
                                          sizeof("Current_Pos")-1),
                        mem_root);
  field_list->push_back(new (mem_root)
                        Item_empty_string(thd, "Gtid_IO_Pos", 30),
                        mem_root);
  field_list->push_back(new (mem_root)
                        Item_empty_string(thd, "Replicate_Do_Domain_Ids",
                                          FN_REFLEN),
                        mem_root);
  field_list->push_back(new (mem_root)
                        Item_empty_string(thd, "Replicate_Ignore_Domain_Ids",
                                          FN_REFLEN),
                        mem_root);
  field_list->push_back(new (mem_root)
                        Item_empty_string(thd, "Parallel_Mode",
                                          sizeof("conservative")-1),
                        mem_root);
  if (full)
  {
    field_list->push_back(new (mem_root)
                          Item_return_int(thd, "Retried_transactions", 10,
                                          MYSQL_TYPE_LONG),
                          mem_root);
    field_list->push_back(new (mem_root)
                          Item_return_int(thd, "Max_relay_log_size", 10,
                                          MYSQL_TYPE_LONGLONG),
                          mem_root);
    field_list->push_back(new (mem_root)
                          Item_return_int(thd, "Executed_log_entries", 10,
                                          MYSQL_TYPE_LONG),
                          mem_root);
    field_list->push_back(new (mem_root)
                          Item_return_int(thd, "Slave_received_heartbeats", 10,
                                          MYSQL_TYPE_LONG),
                          mem_root);
    field_list->push_back(new (mem_root)
                          Item_float(thd, "Slave_heartbeat_period", 0.0, 3, 10),
                          mem_root);
    field_list->push_back(new (mem_root)
                          Item_empty_string(thd, "Gtid_Slave_Pos",
                                            gtid_pos_length),
                          mem_root);
  }
  DBUG_VOID_RETURN;
}

/* Text for Slave_IO_Running */
static const char *slave_running[]= { "No", "Connecting", "Preparing", "Yes" };

static bool send_show_master_info_data(THD *thd, Master_info *mi, bool full,
                                       String *gtid_pos)
{
  DBUG_ENTER("send_show_master_info_data");

  if (mi->host[0])
  {
    DBUG_PRINT("info",("host is set: '%s'", mi->host));
    String *packet= &thd->packet;
    Protocol *protocol= thd->protocol;
    Rpl_filter *rpl_filter= mi->rpl_filter;
    StringBuffer<256> tmp;

    protocol->prepare_for_resend();

    /*
      slave_running can be accessed without run_lock but not other
      non-volotile members like mi->io_thd, which is guarded by the mutex.
    */
    if (full)
      protocol->store(mi->connection_name.str, mi->connection_name.length,
                      &my_charset_bin);
    mysql_mutex_lock(&mi->run_lock);
    if (full)
    {
      /*
        Show what the sql driver replication thread is doing
        This is only meaningful if there is only one slave thread.
      */
      protocol->store(mi->rli.sql_driver_thd ?
                      mi->rli.sql_driver_thd->get_proc_info() : "",
                      &my_charset_bin);
    }
    protocol->store(mi->io_thd ? mi->io_thd->get_proc_info() : "", &my_charset_bin);
    mysql_mutex_unlock(&mi->run_lock);

    mysql_mutex_lock(&mi->data_lock);
    mysql_mutex_lock(&mi->rli.data_lock);
    mysql_mutex_lock(&mi->err_lock);
    mysql_mutex_lock(&mi->rli.err_lock);
    protocol->store(mi->host, &my_charset_bin);
    protocol->store(mi->user, &my_charset_bin);
    protocol->store((uint32) mi->port);
    protocol->store((uint32) mi->connect_retry);
    protocol->store(mi->master_log_name, &my_charset_bin);
    protocol->store((ulonglong) mi->master_log_pos);
    protocol->store(mi->rli.group_relay_log_name +
                    dirname_length(mi->rli.group_relay_log_name),
                    &my_charset_bin);
    protocol->store((ulonglong) mi->rli.group_relay_log_pos);
    protocol->store(mi->rli.group_master_log_name, &my_charset_bin);
    protocol->store(slave_running[mi->slave_running], &my_charset_bin);
    protocol->store(mi->rli.slave_running ? "Yes":"No", &my_charset_bin);
    protocol->store(rpl_filter->get_do_db());
    protocol->store(rpl_filter->get_ignore_db());

    rpl_filter->get_do_table(&tmp);
    protocol->store(&tmp);
    rpl_filter->get_ignore_table(&tmp);
    protocol->store(&tmp);
    rpl_filter->get_wild_do_table(&tmp);
    protocol->store(&tmp);
    rpl_filter->get_wild_ignore_table(&tmp);
    protocol->store(&tmp);

    protocol->store(mi->rli.last_error().number);
    protocol->store(mi->rli.last_error().message, &my_charset_bin);
    protocol->store((uint32) mi->rli.slave_skip_counter);
    protocol->store((ulonglong) mi->rli.group_master_log_pos);
    protocol->store((ulonglong) mi->rli.log_space_total);

    protocol->store(
      mi->rli.until_condition==Relay_log_info::UNTIL_NONE ? "None":
        ( mi->rli.until_condition==Relay_log_info::UNTIL_MASTER_POS? "Master":
          ( mi->rli.until_condition==Relay_log_info::UNTIL_RELAY_POS? "Relay":
            "Gtid")), &my_charset_bin);
    protocol->store(mi->rli.until_log_name, &my_charset_bin);
    protocol->store((ulonglong) mi->rli.until_log_pos);

#ifdef HAVE_OPENSSL
    protocol->store(mi->ssl? "Yes":"No", &my_charset_bin);
#else
    protocol->store(mi->ssl? "Ignored":"No", &my_charset_bin);
#endif
    protocol->store(mi->ssl_ca, &my_charset_bin);
    protocol->store(mi->ssl_capath, &my_charset_bin);
    protocol->store(mi->ssl_cert, &my_charset_bin);
    protocol->store(mi->ssl_cipher, &my_charset_bin);
    protocol->store(mi->ssl_key, &my_charset_bin);

    /*
      Seconds_Behind_Master: if SQL thread is running and I/O thread is
      connected, we can compute it otherwise show NULL (i.e. unknown).
    */
    if ((mi->slave_running == MYSQL_SLAVE_RUN_READING) &&
        mi->rli.slave_running)
    {
      long time_diff;
      bool idle;
      time_t stamp= mi->rli.last_master_timestamp;

      if (!stamp)
        idle= true;
      else
      {
        idle= mi->rli.sql_thread_caught_up;
        if (mi->using_parallel() && idle && !mi->rli.parallel.workers_idle())
          idle= false;
      }
      if (idle)
        time_diff= 0;
      else
      {
        time_diff= ((long)(time(0) - stamp) - mi->clock_diff_with_master);
      /*
        Apparently on some systems time_diff can be <0. Here are possible
        reasons related to MySQL:
        - the master is itself a slave of another master whose time is ahead.
        - somebody used an explicit SET TIMESTAMP on the master.
        Possible reason related to granularity-to-second of time functions
        (nothing to do with MySQL), which can explain a value of -1:
        assume the master's and slave's time are perfectly synchronized, and
        that at slave's connection time, when the master's timestamp is read,
        it is at the very end of second 1, and (a very short time later) when
        the slave's timestamp is read it is at the very beginning of second
        2. Then the recorded value for master is 1 and the recorded value for
        slave is 2. At SHOW SLAVE STATUS time, assume that the difference
        between timestamp of slave and rli->last_master_timestamp is 0
        (i.e. they are in the same second), then we get 0-(2-1)=-1 as a result.
        This confuses users, so we don't go below 0.

        last_master_timestamp == 0 (an "impossible" timestamp 1970) is a
        special marker to say "consider we have caught up".
      */
        if (time_diff < 0)
          time_diff= 0;
      }
      protocol->store((longlong)time_diff);
    }
    else
    {
      protocol->store_null();
    }
    protocol->store(mi->ssl_verify_server_cert? "Yes":"No", &my_charset_bin);

    // Last_IO_Errno
    protocol->store(mi->last_error().number);
    // Last_IO_Error
    protocol->store(mi->last_error().message, &my_charset_bin);
    // Last_SQL_Errno
    protocol->store(mi->rli.last_error().number);
    // Last_SQL_Error
    protocol->store(mi->rli.last_error().message, &my_charset_bin);
    // Replicate_Ignore_Server_Ids
    prot_store_ids(thd, &mi->ignore_server_ids);
    // Master_Server_id
    protocol->store((uint32) mi->master_id);
    // Master_Ssl_Crl
    protocol->store(mi->ssl_ca, &my_charset_bin);
    // Master_Ssl_Crlpath
    protocol->store(mi->ssl_capath, &my_charset_bin);
    // Using_Gtid
    protocol->store(mi->using_gtid_astext(mi->using_gtid), &my_charset_bin);
    // Gtid_IO_Pos
    {
      mi->gtid_current_pos.to_string(&tmp);
      protocol->store(tmp.ptr(), tmp.length(), &my_charset_bin);
    }

    // Replicate_Do_Domain_Ids & Replicate_Ignore_Domain_Ids
    mi->domain_id_filter.store_ids(thd);

    // Parallel_Mode
    {
      const char *mode_name= get_type(&slave_parallel_mode_typelib,
                                      mi->parallel_mode);
      protocol->store(mode_name, strlen(mode_name), &my_charset_bin);
    }

    if (full)
    {
      protocol->store((uint32)    mi->rli.retried_trans);
      protocol->store((ulonglong) mi->rli.max_relay_log_size);
      protocol->store((uint32)    mi->rli.executed_entries);
      protocol->store((uint32)    mi->received_heartbeats);
      protocol->store((double)    mi->heartbeat_period, 3, &tmp);
      protocol->store(gtid_pos->ptr(), gtid_pos->length(), &my_charset_bin);
    }

    mysql_mutex_unlock(&mi->rli.err_lock);
    mysql_mutex_unlock(&mi->err_lock);
    mysql_mutex_unlock(&mi->rli.data_lock);
    mysql_mutex_unlock(&mi->data_lock);

    if (my_net_write(&thd->net, (uchar*) thd->packet.ptr(), packet->length()))
      DBUG_RETURN(TRUE);
  }
  DBUG_RETURN(FALSE);
}


/* Used to sort connections by name */

static int cmp_mi_by_name(const Master_info **arg1,
                          const Master_info **arg2)
{
  return my_strcasecmp(system_charset_info, (*arg1)->connection_name.str,
                       (*arg2)->connection_name.str);
}


/**
  Execute a SHOW FULL SLAVE STATUS statement.

  @param thd Pointer to THD object for the client thread executing the
  statement.

  Elements are sorted according to the original connection_name.

  @retval FALSE success
  @retval TRUE failure

  @note
  master_info_index is protected by LOCK_active_mi.
*/

bool show_all_master_info(THD* thd)
{
  uint i, elements;
  String gtid_pos;
  Master_info **tmp;
  List<Item> field_list;
  DBUG_ENTER("show_master_info");
  mysql_mutex_assert_owner(&LOCK_active_mi);

  gtid_pos.length(0);
  if (rpl_append_gtid_state(&gtid_pos, true))
  {
    my_error(ER_OUT_OF_RESOURCES, MYF(0));
    DBUG_RETURN(TRUE);
  }

  show_master_info_get_fields(thd, &field_list, 1, gtid_pos.length());
  if (thd->protocol->send_result_set_metadata(&field_list,
                       Protocol::SEND_NUM_ROWS | Protocol::SEND_EOF))
    DBUG_RETURN(TRUE);

  if (!master_info_index ||
      !(elements= master_info_index->master_info_hash.records))
    goto end;

  /*
    Sort lines to get them into a predicted order
    (needed for test cases and to not confuse users)
  */
  if (!(tmp= (Master_info**) thd->alloc(sizeof(Master_info*) * elements)))
    DBUG_RETURN(TRUE);

  for (i= 0; i < elements; i++)
  {
    tmp[i]= (Master_info *) my_hash_element(&master_info_index->
                                            master_info_hash, i);
  }
  my_qsort(tmp, elements, sizeof(Master_info*), (qsort_cmp) cmp_mi_by_name);

  for (i= 0; i < elements; i++)
  {
    if (send_show_master_info_data(thd, tmp[i], 1, &gtid_pos))
      DBUG_RETURN(TRUE);
  }

end:
  my_eof(thd);
  DBUG_RETURN(FALSE);
}


void set_slave_thread_options(THD* thd)
{
  DBUG_ENTER("set_slave_thread_options");
  /*
     It's nonsense to constrain the slave threads with max_join_size; if a
     query succeeded on master, we HAVE to execute it. So set
     OPTION_BIG_SELECTS. Setting max_join_size to HA_POS_ERROR is not enough
     (and it's not needed if we have OPTION_BIG_SELECTS) because an INSERT
     SELECT examining more than 4 billion rows would still fail (yes, because
     when max_join_size is 4G, OPTION_BIG_SELECTS is automatically set, but
     only for client threads.
  */
  ulonglong options= thd->variables.option_bits | OPTION_BIG_SELECTS;
  if (opt_log_slave_updates)
    options|= OPTION_BIN_LOG;
  else
    options&= ~OPTION_BIN_LOG;
  thd->variables.option_bits= options;
  thd->variables.completion_type= 0;
  DBUG_VOID_RETURN;
}

void set_slave_thread_default_charset(THD* thd, rpl_group_info *rgi)
{
  DBUG_ENTER("set_slave_thread_default_charset");

  thd->variables.character_set_client=
    global_system_variables.character_set_client;
  thd->variables.collation_connection=
    global_system_variables.collation_connection;
  thd->variables.collation_server=
    global_system_variables.collation_server;
  thd->update_charset();

  thd->system_thread_info.rpl_sql_info->cached_charset_invalidate();
  DBUG_VOID_RETURN;
}

/*
  init_slave_thread()
*/

static int init_slave_thread(THD* thd, Master_info *mi,
                             SLAVE_THD_TYPE thd_type)
{
  DBUG_ENTER("init_slave_thread");
  int simulate_error __attribute__((unused))= 0;
  DBUG_EXECUTE_IF("simulate_io_slave_error_on_init",
                  simulate_error|= (1 << SLAVE_THD_IO););
  DBUG_EXECUTE_IF("simulate_sql_slave_error_on_init",
                  simulate_error|= (1 << SLAVE_THD_SQL););

  thd->system_thread = (thd_type == SLAVE_THD_SQL) ?
    SYSTEM_THREAD_SLAVE_SQL : SYSTEM_THREAD_SLAVE_IO;
  thread_safe_increment32(&service_thread_count);

  /* We must call store_globals() before doing my_net_init() */
  if (init_thr_lock() || thd->store_globals() ||
      my_net_init(&thd->net, 0, thd, MYF(MY_THREAD_SPECIFIC)) ||
      IF_DBUG(simulate_error & (1<< thd_type), 0))
  {
    thd->cleanup();
    DBUG_RETURN(-1);
  }

  thd->security_ctx->skip_grants();
  thd->slave_thread= 1;
  thd->connection_name= mi->connection_name;
  thd->variables.sql_log_slow= opt_log_slow_slave_statements;
  thd->variables.log_slow_filter= global_system_variables.log_slow_filter;
  set_slave_thread_options(thd);
  thd->client_capabilities = CLIENT_LOCAL_FILES;
  mysql_mutex_lock(&LOCK_thread_count);
  thd->thread_id= thd->variables.pseudo_thread_id= thread_id++;
  mysql_mutex_unlock(&LOCK_thread_count);

  if (thd_type == SLAVE_THD_SQL)
    THD_STAGE_INFO(thd, stage_waiting_for_the_next_event_in_relay_log);
  else
    THD_STAGE_INFO(thd, stage_waiting_for_master_update);
  thd->set_time();
  /* Do not use user-supplied timeout value for system threads. */
  thd->variables.lock_wait_timeout= LONG_TIMEOUT;
  DBUG_RETURN(0);
}

/*
  Sleep for a given amount of time or until killed.

  @param thd        Thread context of the current thread.
  @param seconds    The number of seconds to sleep.
  @param func       Function object to check if the thread has been killed.
  @param info       The Rpl_info object associated with this sleep.

  @retval True if the thread has been killed, false otherwise.
*/
template <typename killed_func, typename rpl_info>
static bool slave_sleep(THD *thd, time_t seconds,
                        killed_func func, rpl_info info)
{

  bool ret;
  struct timespec abstime;

  mysql_mutex_t *lock= &info->sleep_lock;
  mysql_cond_t *cond= &info->sleep_cond;

  /* Absolute system time at which the sleep time expires. */
  set_timespec(abstime, seconds);
  mysql_mutex_lock(lock);
  thd->ENTER_COND(cond, lock, NULL, NULL);

  while (! (ret= func(info)))
  {
    int error= mysql_cond_timedwait(cond, lock, &abstime);
    if (error == ETIMEDOUT || error == ETIME)
      break;
  }
  /* Implicitly unlocks the mutex. */
  thd->EXIT_COND(NULL);
  return ret;
}


static int request_dump(THD *thd, MYSQL* mysql, Master_info* mi,
			bool *suppress_warnings)
{
  uchar buf[FN_REFLEN + 10];
  int len;
  ushort binlog_flags = 0; // for now
  char* logname = mi->master_log_name;
  DBUG_ENTER("request_dump");
  
  *suppress_warnings= FALSE;

  if (opt_log_slave_updates && opt_replicate_annotate_row_events)
    binlog_flags|= BINLOG_SEND_ANNOTATE_ROWS_EVENT;

  if (RUN_HOOK(binlog_relay_io,
               before_request_transmit,
               (thd, mi, binlog_flags)))
    DBUG_RETURN(1);
  
  // TODO if big log files: Change next to int8store()
  int4store(buf, (ulong) mi->master_log_pos);
  int2store(buf + 4, binlog_flags);
  int4store(buf + 6, global_system_variables.server_id);
  len = (uint) strlen(logname);
  memcpy(buf + 10, logname,len);
  if (simple_command(mysql, COM_BINLOG_DUMP, buf, len + 10, 1))
  {
    /*
      Something went wrong, so we will just reconnect and retry later
      in the future, we should do a better error analysis, but for
      now we just fill up the error log :-)
    */
    if (mysql_errno(mysql) == ER_NET_READ_INTERRUPTED)
      *suppress_warnings= TRUE;                 // Suppress reconnect warning
    else
      sql_print_error("Error on COM_BINLOG_DUMP: %d  %s, will retry in %d secs",
                      mysql_errno(mysql), mysql_error(mysql),
                      mi->connect_retry);
    DBUG_RETURN(1);
  }

  DBUG_RETURN(0);
}


/*
  Read one event from the master

  SYNOPSIS
    read_event()
    mysql               MySQL connection
    mi                  Master connection information
    suppress_warnings   TRUE when a normal net read timeout has caused us to
                        try a reconnect.  We do not want to print anything to
                        the error log in this case because this a anormal
                        event in an idle server.

    RETURN VALUES
    'packet_error'      Error
    number              Length of packet
*/

static ulong read_event(MYSQL* mysql, Master_info *mi, bool* suppress_warnings)
{
  ulong len;
  DBUG_ENTER("read_event");

  *suppress_warnings= FALSE;
  /*
    my_real_read() will time us out
    We check if we were told to die, and if not, try reading again
  */
#ifndef DBUG_OFF
  if (disconnect_slave_event_count && !(mi->events_till_disconnect--))
    DBUG_RETURN(packet_error);
#endif

  len = cli_safe_read(mysql);
  if (len == packet_error || (long) len < 1)
  {
    if (mysql_errno(mysql) == ER_NET_READ_INTERRUPTED)
    {
      /*
        We are trying a normal reconnect after a read timeout;
        we suppress prints to .err file as long as the reconnect
        happens without problems
      */
      *suppress_warnings= TRUE;
    }
    else
      sql_print_error("Error reading packet from server: %s ( server_errno=%d)",
                      mysql_error(mysql), mysql_errno(mysql));
    DBUG_RETURN(packet_error);
  }

  /* Check if eof packet */
  if (len < 8 && mysql->net.read_pos[0] == 254)
  {
    sql_print_information("Slave: received end packet from server, apparent "
                          "master shutdown: %s",
                     mysql_error(mysql));
     DBUG_RETURN(packet_error);
  }

  DBUG_PRINT("exit", ("len: %lu  net->read_pos[4]: %d",
                      len, mysql->net.read_pos[4]));
  DBUG_RETURN(len - 1);
}

/*
  Check if the current error is of temporary nature of not.
  Some errors are temporary in nature, such as
  ER_LOCK_DEADLOCK and ER_LOCK_WAIT_TIMEOUT.
*/
int
has_temporary_error(THD *thd)
{
  DBUG_ENTER("has_temporary_error");

  DBUG_EXECUTE_IF("all_errors_are_temporary_errors",
                  if (thd->get_stmt_da()->is_error())
                  {
                    thd->clear_error();
                    my_error(ER_LOCK_DEADLOCK, MYF(0));
                  });

  /*
    If there is no message in THD, we can't say if it's a temporary
    error or not. This is currently the case for Incident_log_event,
    which sets no message. Return FALSE.
  */
  if (!thd->is_error())
    DBUG_RETURN(0);

  /*
    Temporary error codes:
    currently, InnoDB deadlock detected by InnoDB or lock
    wait timeout (innodb_lock_wait_timeout exceeded
  */
  if (thd->get_stmt_da()->sql_errno() == ER_LOCK_DEADLOCK ||
      thd->get_stmt_da()->sql_errno() == ER_LOCK_WAIT_TIMEOUT)
    DBUG_RETURN(1);

  DBUG_RETURN(0);
}


/**
  Applies the given event and advances the relay log position.

  In essence, this function does:

  @code
    ev->apply_event(rli);
    ev->update_pos(rli);
  @endcode

  But it also does some maintainance, such as skipping events if
  needed and reporting errors.

  If the @c skip flag is set, then it is tested whether the event
  should be skipped, by looking at the slave_skip_counter and the
  server id.  The skip flag should be set when calling this from a
  replication thread but not set when executing an explicit BINLOG
  statement.

  @retval 0 OK.

  @retval 1 Error calling ev->apply_event().

  @retval 2 No error calling ev->apply_event(), but error calling
  ev->update_pos().
*/
int apply_event_and_update_pos(Log_event* ev, THD* thd,
                               rpl_group_info *rgi,
                               rpl_parallel_thread *rpt)
{
  int exec_res= 0;
  Relay_log_info* rli= rgi->rli;
  DBUG_ENTER("apply_event_and_update_pos");

  DBUG_PRINT("exec_event",("%s(type_code: %d; server_id: %d)",
                           ev->get_type_str(), ev->get_type_code(),
                           ev->server_id));
  DBUG_PRINT("info", ("thd->options: '%s%s%s'  rgi->last_event_start_time: %lu",
                      FLAGSTR(thd->variables.option_bits, OPTION_NOT_AUTOCOMMIT),
                      FLAGSTR(thd->variables.option_bits, OPTION_BEGIN),
                      FLAGSTR(thd->variables.option_bits, OPTION_GTID_BEGIN),
                      (ulong) rgi->last_event_start_time));

  /*
    Execute the event to change the database and update the binary
    log coordinates, but first we set some data that is needed for
    the thread.

    The event will be executed unless it is supposed to be skipped.

    Queries originating from this server must be skipped.  Low-level
    events (Format_description_log_event, Rotate_log_event,
    Stop_log_event) from this server must also be skipped. But for
    those we don't want to modify 'group_master_log_pos', because
    these events did not exist on the master.
    Format_description_log_event is not completely skipped.

    Skip queries specified by the user in 'slave_skip_counter'.  We
    can't however skip events that has something to do with the log
    files themselves.

    Filtering on own server id is extremely important, to ignore
    execution of events created by the creation/rotation of the relay
    log (remember that now the relay log starts with its Format_desc,
    has a Rotate etc).
  */

  /* Use the original server id for logging. */
  thd->variables.server_id = ev->server_id;
  thd->set_time();                            // time the query
  thd->lex->current_select= 0;
  if (!ev->when)
  {
    my_hrtime_t hrtime= my_hrtime();
    ev->when= hrtime_to_my_time(hrtime);
    ev->when_sec_part= hrtime_sec_part(hrtime);
  }
  thd->variables.option_bits=
    (thd->variables.option_bits & ~OPTION_SKIP_REPLICATION) |
    (ev->flags & LOG_EVENT_SKIP_REPLICATION_F ? OPTION_SKIP_REPLICATION : 0);
  ev->thd = thd; // because up to this point, ev->thd == 0

  int reason= ev->shall_skip(rgi);
  if (reason == Log_event::EVENT_SKIP_COUNT)
  {
    DBUG_ASSERT(rli->slave_skip_counter > 0);
    rli->slave_skip_counter--;
  }
  mysql_mutex_unlock(&rli->data_lock);
  DBUG_EXECUTE_IF("inject_slave_sql_before_apply_event",
    {
      DBUG_ASSERT(!debug_sync_set_action
                  (thd, STRING_WITH_LEN("now WAIT_FOR continue")));
      DBUG_SET_INITIAL("-d,inject_slave_sql_before_apply_event");
    };);
  if (reason == Log_event::EVENT_SKIP_NOT)
    exec_res= ev->apply_event(rgi);

#ifdef WITH_WSREP
    if (exec_res && thd->wsrep_conflict_state != NO_CONFLICT)
    {
      WSREP_DEBUG("SQL apply failed, res %d conflict state: %d",
                  exec_res, thd->wsrep_conflict_state);
      rli->abort_slave= 1;
      rli->report(ERROR_LEVEL, ER_UNKNOWN_COM_ERROR, rgi->gtid_info(),
                  "Node has dropped from cluster");
    }
#endif

#ifndef DBUG_OFF
  /*
    This only prints information to the debug trace.

    TODO: Print an informational message to the error log?
  */
  static const char *const explain[] = {
    // EVENT_SKIP_NOT,
    "not skipped",
    // EVENT_SKIP_IGNORE,
    "skipped because event should be ignored",
    // EVENT_SKIP_COUNT
    "skipped because event skip counter was non-zero"
  };
  DBUG_PRINT("info", ("OPTION_BEGIN: %d  IN_STMT: %d  IN_TRANSACTION: %d",
                      MY_TEST(thd->variables.option_bits & OPTION_BEGIN),
                      rli->get_flag(Relay_log_info::IN_STMT),
                      rli->get_flag(Relay_log_info::IN_TRANSACTION)));
  DBUG_PRINT("skip_event", ("%s event was %s",
                            ev->get_type_str(), explain[reason]));
#endif

  DBUG_PRINT("info", ("apply_event error = %d", exec_res));
  if (exec_res == 0)
  {
    int error= ev->update_pos(rgi);
#ifdef HAVE_valgrind
    if (!rli->is_fake)
#endif
    {
      DBUG_PRINT("info", ("update_pos error = %d", error));
      DBUG_PRINT("info", ("group %llu %s", rli->group_relay_log_pos,
                          rli->group_relay_log_name));
      DBUG_PRINT("info", ("event %llu %s", rli->event_relay_log_pos,
                          rli->event_relay_log_name));
    }
    /*
      The update should not fail, so print an error message and
      return an error code.

      TODO: Replace this with a decent error message when merged
      with BUG#24954 (which adds several new error message).
    */
    if (error)
    {
      rli->report(ERROR_LEVEL, ER_UNKNOWN_ERROR, rgi->gtid_info(),
                  "It was not possible to update the positions"
                  " of the relay log information: the slave may"
                  " be in an inconsistent state."
                  " Stopped in %s position %llu",
                  rli->group_relay_log_name, rli->group_relay_log_pos);
      DBUG_RETURN(2);
    }
  }
  else
  {
    /*
      Make sure we do not erroneously update gtid_slave_pos with a lingering
      GTID from this failed event group (MDEV-4906).
    */
    rgi->gtid_pending= false;
  }

  DBUG_RETURN(exec_res ? 1 : 0);
}


/**
   Keep the relay log transaction state up to date.

   The state reflects how things are after the given event, that has just been
   read from the relay log, is executed.

   This is only needed to ensure we:
   - Don't abort the sql driver thread in the middle of an event group.
   - Don't rotate the io thread in the middle of a statement or transaction.
     The mechanism is that the io thread, when it needs to rotate the relay
     log, will wait until the sql driver has read all the cached events
     and then continue reading events one by one from the master until
     the sql threads signals that log doesn't have an active group anymore.

     There are two possible cases. We keep them as 2 separate flags mainly
     to make debugging easier.

     - IN_STMT is set when we have read an event that should be used
       together with the next event.  This is for example setting a
       variable that is used when executing the next statement.
     - IN_TRANSACTION is set when we are inside a BEGIN...COMMIT group

     To test the state one should use the is_in_group() function.
*/

inline void update_state_of_relay_log(Relay_log_info *rli, Log_event *ev)
{
  Log_event_type typ= ev->get_type_code();

  /* check if we are in a multi part event */
  if (ev->is_part_of_group())
    rli->set_flag(Relay_log_info::IN_STMT);
  else if (Log_event::is_group_event(typ))
  {
    /*
      If it was not a is_part_of_group() and not a group event (like
      rotate) then we can reset the IN_STMT flag.  We have the above
      if only to allow us to have a rotate element anywhere.
    */
    rli->clear_flag(Relay_log_info::IN_STMT);
  }

  /* Check for an event that starts or stops a transaction */
  if (typ == QUERY_EVENT)
  {
    Query_log_event *qev= (Query_log_event*) ev;
    /*
      Trivial optimization to avoid the following somewhat expensive
      checks.
    */
    if (qev->q_len <= sizeof("ROLLBACK"))
    {
      if (qev->is_begin())
        rli->set_flag(Relay_log_info::IN_TRANSACTION);
      if (qev->is_commit() || qev->is_rollback())
        rli->clear_flag(Relay_log_info::IN_TRANSACTION);
    }
  }
  if (typ == XID_EVENT)
    rli->clear_flag(Relay_log_info::IN_TRANSACTION);
  if (typ == GTID_EVENT &&
      !(((Gtid_log_event*) ev)->flags2 & Gtid_log_event::FL_STANDALONE))
  {
    /* This GTID_EVENT will generate a BEGIN event */
    rli->set_flag(Relay_log_info::IN_TRANSACTION);
  }

  DBUG_PRINT("info", ("event: %u  IN_STMT: %d  IN_TRANSACTION: %d",
                      (uint) typ,
                      rli->get_flag(Relay_log_info::IN_STMT),
                      rli->get_flag(Relay_log_info::IN_TRANSACTION)));
}


/**
  Top-level function for executing the next event from the relay log.

  This function reads the event from the relay log, executes it, and
  advances the relay log position.  It also handles errors, etc.

  This function may fail to apply the event for the following reasons:

   - The position specfied by the UNTIL condition of the START SLAVE
     command is reached.

   - It was not possible to read the event from the log.

   - The slave is killed.

   - An error occurred when applying the event, and the event has been
     tried slave_trans_retries times.  If the event has been retried
     fewer times, 0 is returned.

   - init_master_info or init_relay_log_pos failed. (These are called
     if a failure occurs when applying the event.)

   - An error occurred when updating the binlog position.

  @retval 0 The event was applied.

  @retval 1 The event was not applied.
*/

static int exec_relay_log_event(THD* thd, Relay_log_info* rli,
                                rpl_group_info *serial_rgi)
{
  ulonglong event_size;
  DBUG_ENTER("exec_relay_log_event");

  /*
    We acquire this mutex since we need it for all operations except
    event execution. But we will release it in places where we will
    wait for something for example inside of next_event().
  */
  mysql_mutex_lock(&rli->data_lock);

  Log_event *ev= next_event(serial_rgi, &event_size);

  if (sql_slave_killed(serial_rgi))
  {
    mysql_mutex_unlock(&rli->data_lock);
    delete ev;
    DBUG_RETURN(1);
  }
  if (ev)
  {
    int exec_res;
    Log_event_type typ= ev->get_type_code();

    /*
      Even if we don't execute this event, we keep the master timestamp,
      so that seconds behind master shows correct delta (there are events
      that are not replayed, so we keep falling behind).

      If it is an artificial event, or a relay log event (IO thread generated
      event) or ev->when is set to 0, we don't update the
      last_master_timestamp.

      In parallel replication, we might queue a large number of events, and
      the user might be surprised to see a claim that the slave is up to date
      long before those queued events are actually executed.
     */
    if (!rli->mi->using_parallel() &&
        !(ev->is_artificial_event() || ev->is_relay_log_event() || (ev->when == 0)))
    {
      rli->last_master_timestamp= ev->when + (time_t) ev->exec_time;
      DBUG_ASSERT(rli->last_master_timestamp >= 0);
    }

    /*
      This tests if the position of the beginning of the current event
      hits the UNTIL barrier.
    */
    if ((rli->until_condition == Relay_log_info::UNTIL_MASTER_POS ||
         rli->until_condition == Relay_log_info::UNTIL_RELAY_POS) &&
        (ev->server_id != global_system_variables.server_id ||
         rli->replicate_same_server_id) &&
         rli->is_until_satisfied((rli->get_flag(Relay_log_info::IN_TRANSACTION) || !ev->log_pos)
                                  ? rli->group_master_log_pos
                                  : ev->log_pos - ev->data_written))
    {
      sql_print_information("Slave SQL thread stopped because it reached its"
                            " UNTIL position %llu", rli->until_pos());
      /*
        Setting abort_slave flag because we do not want additional
        message about error in query execution to be printed.
      */
      rli->abort_slave= 1;
      rli->stop_for_until= true;
      mysql_mutex_unlock(&rli->data_lock);
      delete ev;
      DBUG_RETURN(1);
    }

    { /**
         The following failure injecion works in cooperation with tests 
         setting @@global.debug= 'd,incomplete_group_in_relay_log'.
         Xid or Commit events are not executed to force the slave sql
         read hanging if the realy log does not have any more events.
      */
      DBUG_EXECUTE_IF("incomplete_group_in_relay_log",
                      if ((typ == XID_EVENT) ||
                          ((typ == QUERY_EVENT) &&
                           strcmp("COMMIT", ((Query_log_event *) ev)->query) == 0))
                      {
                        DBUG_ASSERT(thd->transaction.all.modified_non_trans_table);
                        rli->abort_slave= 1;
                        mysql_mutex_unlock(&rli->data_lock);
                        delete ev;
                        serial_rgi->inc_event_relay_log_pos();
                        DBUG_RETURN(0);
                      };);
    }

    update_state_of_relay_log(rli, ev);

    if (rli->mi->using_parallel())
    {
      int res= rli->parallel.do_event(serial_rgi, ev, event_size);
      if (res >= 0)
        DBUG_RETURN(res);
      /*
        Else we proceed to execute the event non-parallel.
        This is the case for pre-10.0 events without GTID, and for handling
        slave_skip_counter.
      */
    }

    if (typ == GTID_EVENT)
    {
      Gtid_log_event *gev= static_cast<Gtid_log_event *>(ev);

      /*
        For GTID, allocate a new sub_id for the given domain_id.
        The sub_id must be allocated in increasing order of binlog order.
      */
      if (event_group_new_gtid(serial_rgi, gev))
      {
        sql_print_error("Error reading relay log event: %s", "slave SQL thread "
                        "aborted because of out-of-memory error");
        mysql_mutex_unlock(&rli->data_lock);
        delete ev;
        DBUG_RETURN(1);
      }

      if (opt_gtid_ignore_duplicates)
      {
        int res= rpl_global_gtid_slave_state->check_duplicate_gtid
          (&serial_rgi->current_gtid, serial_rgi);
        if (res < 0)
        {
          sql_print_error("Error processing GTID event: %s", "slave SQL "
                          "thread aborted because of out-of-memory error");
          mysql_mutex_unlock(&rli->data_lock);
          delete ev;
          DBUG_RETURN(1);
        }
        /*
          If we need to skip this event group (because the GTID was already
          applied), then do it using the code for slave_skip_counter, which
          is able to handle skipping until the end of the event group.
        */
        if (!res)
          rli->slave_skip_counter= 1;
      }
    }

    serial_rgi->future_event_relay_log_pos= rli->future_event_relay_log_pos;
    serial_rgi->event_relay_log_name= rli->event_relay_log_name;
    serial_rgi->event_relay_log_pos= rli->event_relay_log_pos;
    exec_res= apply_event_and_update_pos(ev, thd, serial_rgi, NULL);

#ifdef WITH_WSREP
    WSREP_DEBUG("apply_event_and_update_pos() result: %d", exec_res);
#endif /* WITH_WSREP */

    delete_or_keep_event_post_apply(serial_rgi, typ, ev);

    /*
      update_log_pos failed: this should not happen, so we don't
      retry.
    */
    if (exec_res == 2)
      DBUG_RETURN(1);

#ifdef WITH_WSREP
    mysql_mutex_lock(&thd->LOCK_wsrep_thd);
    if (thd->wsrep_conflict_state == NO_CONFLICT)
    {
      mysql_mutex_unlock(&thd->LOCK_wsrep_thd);
#endif /* WITH_WSREP */
    if (slave_trans_retries)
    {
      int UNINIT_VAR(temp_err);
      if (exec_res && (temp_err= has_temporary_error(thd)))
      {
        const char *errmsg;
        rli->clear_error();
        /*
          We were in a transaction which has been rolled back because of a
          temporary error;
          let's seek back to BEGIN log event and retry it all again.
	  Note, if lock wait timeout (innodb_lock_wait_timeout exceeded)
	  there is no rollback since 5.0.13 (ref: manual).
          We have to not only seek but also

          a) init_master_info(), to seek back to hot relay log's start
          for later (for when we will come back to this hot log after
          re-processing the possibly existing old logs where BEGIN is:
          check_binlog_magic() will then need the cache to be at
          position 0 (see comments at beginning of
          init_master_info()).
          b) init_relay_log_pos(), because the BEGIN may be an older relay log.
        */
        if (serial_rgi->trans_retries < slave_trans_retries)
        {
          if (init_master_info(rli->mi, 0, 0, 0, SLAVE_SQL))
            sql_print_error("Failed to initialize the master info structure");
          else if (init_relay_log_pos(rli,
                                      rli->group_relay_log_name,
                                      rli->group_relay_log_pos,
                                      1, &errmsg, 1))
            sql_print_error("Error initializing relay log position: %s",
                            errmsg);
          else
          {
            exec_res= 0;
            serial_rgi->cleanup_context(thd, 1);
            /* chance for concurrent connection to get more locks */
            slave_sleep(thd, MY_MIN(serial_rgi->trans_retries,
                                    MAX_SLAVE_RETRY_PAUSE),
                       sql_slave_killed, serial_rgi);
            serial_rgi->trans_retries++;
            mysql_mutex_lock(&rli->data_lock); // because of SHOW STATUS
            rli->retried_trans++;
            statistic_increment(slave_retried_transactions, LOCK_status);
            mysql_mutex_unlock(&rli->data_lock);
            DBUG_PRINT("info", ("Slave retries transaction "
                                "rgi->trans_retries: %lu",
                                serial_rgi->trans_retries));
          }
        }
        else
          sql_print_error("Slave SQL thread retried transaction %lu time(s) "
                          "in vain, giving up. Consider raising the value of "
                          "the slave_transaction_retries variable.",
                          slave_trans_retries);
      }
      else if ((exec_res && !temp_err) ||
               (opt_using_transactions &&
                rli->group_relay_log_pos == rli->event_relay_log_pos))
      {
        /*
          Only reset the retry counter if the entire group succeeded
          or failed with a non-transient error.  On a successful
          event, the execution will proceed as usual; in the case of a
          non-transient error, the slave will stop with an error.
         */
        serial_rgi->trans_retries= 0; // restart from fresh
        DBUG_PRINT("info", ("Resetting retry counter, rgi->trans_retries: %lu",
                            serial_rgi->trans_retries));
      }
    }
#ifdef WITH_WSREP
    }
    else
      mysql_mutex_unlock(&thd->LOCK_wsrep_thd);
#endif /* WITH_WSREP */

    thread_safe_increment64(&rli->executed_entries);
    DBUG_RETURN(exec_res);
  }
  mysql_mutex_unlock(&rli->data_lock);
  rli->report(ERROR_LEVEL, ER_SLAVE_RELAY_LOG_READ_FAILURE, NULL,
              ER_THD(thd, ER_SLAVE_RELAY_LOG_READ_FAILURE), "\
Could not parse relay log event entry. The possible reasons are: the master's \
binary log is corrupted (you can check this by running 'mysqlbinlog' on the \
binary log), the slave's relay log is corrupted (you can check this by running \
'mysqlbinlog' on the relay log), a network problem, or a bug in the master's \
or slave's MySQL code. If you want to check the master's binary log or slave's \
relay log, you will be able to know their names by issuing 'SHOW SLAVE STATUS' \
on this slave.\
");
  DBUG_RETURN(1);
}


static bool check_io_slave_killed(Master_info *mi, const char *info)
{
  if (io_slave_killed(mi))
  {
    if (info && global_system_variables.log_warnings)
      sql_print_information("%s", info);
    return TRUE;
  }
  return FALSE;
}

/**
  @brief Try to reconnect slave IO thread.

  @details Terminates current connection to master, sleeps for
  @c mi->connect_retry msecs and initiates new connection with
  @c safe_reconnect(). Variable pointed by @c retry_count is increased -
  if it exceeds @c master_retry_count then connection is not re-established
  and function signals error.
  Unless @c suppres_warnings is TRUE, a warning is put in the server error log
  when reconnecting. The warning message and messages used to report errors
  are taken from @c messages array. In case @c master_retry_count is exceeded,
  no messages are added to the log.

  @param[in]     thd                 Thread context.
  @param[in]     mysql               MySQL connection.
  @param[in]     mi                  Master connection information.
  @param[in,out] retry_count         Number of attempts to reconnect.
  @param[in]     suppress_warnings   TRUE when a normal net read timeout 
                                     has caused to reconnecting.
  @param[in]     messages            Messages to print/log, see 
                                     reconnect_messages[] array.

  @retval        0                   OK.
  @retval        1                   There was an error.
*/

static int try_to_reconnect(THD *thd, MYSQL *mysql, Master_info *mi,
                            uint *retry_count, bool suppress_warnings,
                            const char *messages[SLAVE_RECON_MSG_MAX])
{
  mi->slave_running= MYSQL_SLAVE_RUN_NOT_CONNECT;
  thd->proc_info= messages[SLAVE_RECON_MSG_WAIT];
#ifdef SIGNAL_WITH_VIO_CLOSE  
  thd->clear_active_vio();
#endif
  end_server(mysql);
  if ((*retry_count)++)
  {
    if (*retry_count > master_retry_count)
      return 1;                             // Don't retry forever
    slave_sleep(thd, mi->connect_retry, io_slave_killed, mi);
  }
  if (check_io_slave_killed(mi, messages[SLAVE_RECON_MSG_KILLED_WAITING]))
    return 1;
  thd->proc_info = messages[SLAVE_RECON_MSG_AFTER];
  if (!suppress_warnings) 
  {
    char buf[256];
    StringBuffer<100> tmp;
    if (mi->using_gtid != Master_info::USE_GTID_NO)
    {
      tmp.append(STRING_WITH_LEN("; GTID position '"));
      mi->gtid_current_pos.append_to_string(&tmp);
      if (mi->events_queued_since_last_gtid == 0)
        tmp.append(STRING_WITH_LEN("'"));
      else
      {
        tmp.append(STRING_WITH_LEN("', GTID event skip "));
        tmp.append_ulonglong((ulonglong)mi->events_queued_since_last_gtid);
      }
    }
    my_snprintf(buf, sizeof(buf), messages[SLAVE_RECON_MSG_FAILED], 
                IO_RPL_LOG_NAME, mi->master_log_pos,
                tmp.c_ptr_safe());
    /* 
      Raise a warining during registering on master/requesting dump.
      Log a message reading event.
    */
    if (messages[SLAVE_RECON_MSG_COMMAND][0])
    {
      mi->report(WARNING_LEVEL, ER_SLAVE_MASTER_COM_FAILURE, NULL,
                 ER_THD(thd, ER_SLAVE_MASTER_COM_FAILURE), 
                 messages[SLAVE_RECON_MSG_COMMAND], buf);
    }
    else
    {
      sql_print_information("%s", buf);
    }
  }
  if (safe_reconnect(thd, mysql, mi, 1) || io_slave_killed(mi))
  {
    if (global_system_variables.log_warnings)
      sql_print_information("%s", messages[SLAVE_RECON_MSG_KILLED_AFTER]);
    return 1;
  }
  return 0;
}


/**
  Slave IO thread entry point.

  @param arg Pointer to Master_info struct that holds information for
  the IO thread.

  @return Always 0.
*/
pthread_handler_t handle_slave_io(void *arg)
{
  THD *thd; // needs to be first for thread_stack
  MYSQL *mysql;
  Master_info *mi = (Master_info*)arg;
  Relay_log_info *rli= &mi->rli;
  uint retry_count;
  bool suppress_warnings;
  int ret;
  rpl_io_thread_info io_info;
#ifndef DBUG_OFF
  mi->dbug_do_disconnect= false;
#endif
  // needs to call my_thread_init(), otherwise we get a coredump in DBUG_ stuff
  my_thread_init();
  DBUG_ENTER("handle_slave_io");

  DBUG_ASSERT(mi->inited);
  mysql= NULL ;
  retry_count= 0;

  thd= new THD; // note that contructor of THD uses DBUG_ !

  mysql_mutex_lock(&mi->run_lock);
  /* Inform waiting threads that slave has started */
  mi->slave_run_id++;

#ifndef DBUG_OFF
  mi->events_till_disconnect = disconnect_slave_event_count;
#endif

  THD_CHECK_SENTRY(thd);
  mi->io_thd = thd;

  pthread_detach_this_thread();
  thd->thread_stack= (char*) &thd; // remember where our stack is
  mi->clear_error();
  if (init_slave_thread(thd, mi, SLAVE_THD_IO))
  {
    mysql_cond_broadcast(&mi->start_cond);
    sql_print_error("Failed during slave I/O thread initialization");
    goto err_during_init;
  }
  thd->system_thread_info.rpl_io_info= &io_info;
  mysql_mutex_lock(&LOCK_thread_count);
  threads.append(thd);
  mysql_mutex_unlock(&LOCK_thread_count);
  mi->slave_running = MYSQL_SLAVE_RUN_NOT_CONNECT;
  mi->abort_slave = 0;
  mysql_mutex_unlock(&mi->run_lock);
  mysql_cond_broadcast(&mi->start_cond);

  DBUG_PRINT("master_info",("log_file_name: '%s'  position: %llu",
                            mi->master_log_name, mi->master_log_pos));

  /* This must be called before run any binlog_relay_io hooks */
  my_pthread_setspecific_ptr(RPL_MASTER_INFO, mi);

  /* Load the set of seen GTIDs, if we did not already. */
  if (rpl_load_gtid_slave_state(thd))
  {
    mi->report(ERROR_LEVEL, thd->get_stmt_da()->sql_errno(), NULL,
                "Unable to load replication GTID slave state from mysql.%s: %s",
                rpl_gtid_slave_state_table_name.str,
                thd->get_stmt_da()->message());
    /*
      If we are using old-style replication, we can continue, even though we
      then will not be able to record the GTIDs we receive. But if using GTID,
      we must give up.
    */
    if (mi->using_gtid != Master_info::USE_GTID_NO || opt_gtid_strict_mode)
      goto err;
  }


  if (RUN_HOOK(binlog_relay_io, thread_start, (thd, mi)))
  {
    mi->report(ERROR_LEVEL, ER_SLAVE_FATAL_ERROR, NULL,
               ER_THD(thd, ER_SLAVE_FATAL_ERROR),
               "Failed to run 'thread_start' hook");
    goto err;
  }

  if (!(mi->mysql = mysql = mysql_init(NULL)))
  {
    mi->report(ERROR_LEVEL, ER_SLAVE_FATAL_ERROR, NULL,
               ER_THD(thd, ER_SLAVE_FATAL_ERROR), "error in mysql_init()");
    goto err;
  }

  THD_STAGE_INFO(thd, stage_connecting_to_master);
  // we can get killed during safe_connect
  if (!safe_connect(thd, mysql, mi))
  {
    if (mi->using_gtid == Master_info::USE_GTID_NO)
      sql_print_information("Slave I/O thread: connected to master '%s@%s:%d',"
                            "replication started in log '%s' at position %llu",
                            mi->user, mi->host, mi->port,
                            IO_RPL_LOG_NAME, mi->master_log_pos);
    else
    {
      StringBuffer<100> tmp;
      mi->gtid_current_pos.to_string(&tmp);
      sql_print_information("Slave I/O thread: connected to master '%s@%s:%d',"
                            "replication starts at GTID position '%s'",
                            mi->user, mi->host, mi->port, tmp.c_ptr_safe());
    }
  }
  else
  {
    sql_print_information("Slave I/O thread killed while connecting to master");
    goto err;
  }

connected:

  if (mi->using_gtid != Master_info::USE_GTID_NO)
  {
    /*
      When the IO thread (re)connects to the master using GTID, it will
      connect at the start of an event group. But the IO thread may have
      previously logged part of the following event group to the relay
      log.

      When the IO and SQL thread are started together, we erase any previous
      relay logs, but this is not possible/desirable while the SQL thread is
      running. To avoid duplicating partial event groups in the relay logs in
      this case, we remember the count of events in any partially logged event
      group before the reconnect, and then here at connect we set up a counter
      to skip the already-logged part of the group.
    */
    mi->gtid_reconnect_event_skip_count= mi->events_queued_since_last_gtid;
    mi->gtid_event_seen= false;
  }

#ifdef ENABLED_DEBUG_SYNC
    DBUG_EXECUTE_IF("dbug.before_get_running_status_yes",
                    {
                      const char act[]=
                        "now "
                        "wait_for signal.io_thread_let_running";
                      DBUG_ASSERT(debug_sync_service);
                      DBUG_ASSERT(!debug_sync_set_action(thd, 
                                                         STRING_WITH_LEN(act)));
                    };);
#endif

  // TODO: the assignment below should be under mutex (5.0)
  mi->slave_running= MYSQL_SLAVE_RUN_CONNECT;
  thd->slave_net = &mysql->net;
  THD_STAGE_INFO(thd, stage_checking_master_version);
  ret= get_master_version_and_clock(mysql, mi);
  if (ret == 1)
    /* Fatal error */
    goto err;

  if (ret == 2) 
  { 
    if (check_io_slave_killed(mi, "Slave I/O thread killed "
                              "while calling get_master_version_and_clock(...)"))
      goto err;
    suppress_warnings= FALSE;
    /*
      Try to reconnect because the error was caused by a transient network
      problem
    */
    if (try_to_reconnect(thd, mysql, mi, &retry_count, suppress_warnings,
                             reconnect_messages[SLAVE_RECON_ACT_REG]))
      goto err;
    goto connected;
  } 

  if (mi->rli.relay_log.description_event_for_queue->binlog_version > 1)
  {
    /*
      Register ourselves with the master.
    */
    THD_STAGE_INFO(thd, stage_registering_slave_on_master);
    if (register_slave_on_master(mysql, mi, &suppress_warnings))
    {
      if (!check_io_slave_killed(mi, "Slave I/O thread killed "
                                "while registering slave on master"))
      {
        sql_print_error("Slave I/O thread couldn't register on master");
        if (try_to_reconnect(thd, mysql, mi, &retry_count, suppress_warnings,
                             reconnect_messages[SLAVE_RECON_ACT_REG]))
          goto err;
      }
      else
        goto err;
      goto connected;
    }
  }

  DBUG_PRINT("info",("Starting reading binary log from master"));
  while (!io_slave_killed(mi))
  {
    THD_STAGE_INFO(thd, stage_requesting_binlog_dump);
    if (request_dump(thd, mysql, mi, &suppress_warnings))
    {
      sql_print_error("Failed on request_dump()");
      if (check_io_slave_killed(mi, NullS) ||
        try_to_reconnect(thd, mysql, mi, &retry_count, suppress_warnings,
                         reconnect_messages[SLAVE_RECON_ACT_DUMP]))
        goto err;
      goto connected;
    }

    const char *event_buf;

    mi->slave_running= MYSQL_SLAVE_RUN_READING;
    DBUG_ASSERT(mi->last_error().number == 0);
    while (!io_slave_killed(mi))
    {
      ulong event_len;
      /*
         We say "waiting" because read_event() will wait if there's nothing to
         read. But if there's something to read, it will not wait. The
         important thing is to not confuse users by saying "reading" whereas
         we're in fact receiving nothing.
      */
      THD_STAGE_INFO(thd, stage_waiting_for_master_to_send_event);
      event_len= read_event(mysql, mi, &suppress_warnings);
      if (check_io_slave_killed(mi, NullS))
        goto err;

      if (event_len == packet_error)
      {
        uint mysql_error_number= mysql_errno(mysql);
        switch (mysql_error_number) {
        case CR_NET_PACKET_TOO_LARGE:
          sql_print_error("\
Log entry on master is longer than slave_max_allowed_packet (%lu) on \
slave. If the entry is correct, restart the server with a higher value of \
slave_max_allowed_packet",
                         slave_max_allowed_packet);
          mi->report(ERROR_LEVEL, ER_NET_PACKET_TOO_LARGE, NULL,
                     "%s", "Got a packet bigger than 'slave_max_allowed_packet' bytes");
          goto err;
        case ER_MASTER_FATAL_ERROR_READING_BINLOG:
          mi->report(ERROR_LEVEL, ER_MASTER_FATAL_ERROR_READING_BINLOG, NULL,
                     ER_THD(thd, ER_MASTER_FATAL_ERROR_READING_BINLOG),
                     mysql_error_number, mysql_error(mysql));
          goto err;
        case ER_OUT_OF_RESOURCES:
          sql_print_error("\
Stopping slave I/O thread due to out-of-memory error from master");
          mi->report(ERROR_LEVEL, ER_OUT_OF_RESOURCES, NULL,
                     "%s", ER_THD(thd, ER_OUT_OF_RESOURCES));
          goto err;
        }
        if (try_to_reconnect(thd, mysql, mi, &retry_count, suppress_warnings,
                             reconnect_messages[SLAVE_RECON_ACT_EVENT]))
          goto err;
        goto connected;
      } // if (event_len == packet_error)

      retry_count=0;                    // ok event, reset retry counter
      THD_STAGE_INFO(thd, stage_queueing_master_event_to_the_relay_log);
      event_buf= (const char*)mysql->net.read_pos + 1;
      if (RUN_HOOK(binlog_relay_io, after_read_event,
                   (thd, mi,(const char*)mysql->net.read_pos + 1,
                    event_len, &event_buf, &event_len)))
      {
        mi->report(ERROR_LEVEL, ER_SLAVE_FATAL_ERROR, NULL,
                   ER_THD(thd, ER_SLAVE_FATAL_ERROR),
                   "Failed to run 'after_read_event' hook");
        goto err;
      }

      /* XXX: 'synced' should be updated by queue_event to indicate
         whether event has been synced to disk */
      bool synced= 0;
      if (queue_event(mi, event_buf, event_len))
      {
        mi->report(ERROR_LEVEL, ER_SLAVE_RELAY_LOG_WRITE_FAILURE, NULL,
                   ER_THD(thd, ER_SLAVE_RELAY_LOG_WRITE_FAILURE),
                   "could not queue event from master");
        goto err;
      }

      if (RUN_HOOK(binlog_relay_io, after_queue_event,
                   (thd, mi, event_buf, event_len, synced)))
      {
        mi->report(ERROR_LEVEL, ER_SLAVE_FATAL_ERROR, NULL,
                   ER_THD(thd, ER_SLAVE_FATAL_ERROR),
                   "Failed to run 'after_queue_event' hook");
        goto err;
      }

      if (mi->using_gtid == Master_info::USE_GTID_NO &&
          flush_master_info(mi, TRUE, TRUE))
      {
        sql_print_error("Failed to flush master info file");
        goto err;
      }
      /*
        See if the relay logs take too much space.
        We don't lock mi->rli.log_space_lock here; this dirty read saves time
        and does not introduce any problem:
        - if mi->rli.ignore_log_space_limit is 1 but becomes 0 just after (so
        the clean value is 0), then we are reading only one more event as we
        should, and we'll block only at the next event. No big deal.
        - if mi->rli.ignore_log_space_limit is 0 but becomes 1 just
        after (so the clean value is 1), then we are going into
        wait_for_relay_log_space() for no reason, but this function
        will do a clean read, notice the clean value and exit
        immediately.
      */
#ifndef DBUG_OFF
      {
        DBUG_PRINT("info", ("log_space_limit=%llu log_space_total=%llu "
                            "ignore_log_space_limit=%d",
                            rli->log_space_limit, rli->log_space_total,
                            (int) rli->ignore_log_space_limit));
      }
#endif

      if (rli->log_space_limit && rli->log_space_limit <
          rli->log_space_total &&
          !rli->ignore_log_space_limit)
        if (wait_for_relay_log_space(rli))
        {
          sql_print_error("Slave I/O thread aborted while waiting for relay \
log space");
          goto err;
        }
    }
  }

  // error = 0;
err:
  // print the current replication position
  if (mi->using_gtid == Master_info::USE_GTID_NO)
    sql_print_information("Slave I/O thread exiting, read up to log '%s', "
                          "position %llu", IO_RPL_LOG_NAME, mi->master_log_pos);
  else
  {
    StringBuffer<100> tmp;
    mi->gtid_current_pos.to_string(&tmp);
    sql_print_information("Slave I/O thread exiting, read up to log '%s', "
                          "position %llu; GTID position %s",
                          IO_RPL_LOG_NAME, mi->master_log_pos,
                          tmp.c_ptr_safe());
  }
  RUN_HOOK(binlog_relay_io, thread_stop, (thd, mi));
  thd->reset_query();
  thd->reset_db(NULL, 0);
  if (mysql)
  {
    /*
      Here we need to clear the active VIO before closing the
      connection with the master.  The reason is that THD::awake()
      might be called from terminate_slave_thread() because somebody
      issued a STOP SLAVE.  If that happends, the close_active_vio()
      can be called in the middle of closing the VIO associated with
      the 'mysql' object, causing a crash.
    */
#ifdef SIGNAL_WITH_VIO_CLOSE
    thd->clear_active_vio();
#endif
    mysql_close(mysql);
    mi->mysql=0;
  }
  write_ignored_events_info_to_relay_log(thd, mi);
  if (mi->using_gtid != Master_info::USE_GTID_NO)
    flush_master_info(mi, TRUE, TRUE);
  THD_STAGE_INFO(thd, stage_waiting_for_slave_mutex_on_exit);
  thd->add_status_to_global();
  mysql_mutex_lock(&mi->run_lock);

err_during_init:
  /* Forget the relay log's format */
  delete mi->rli.relay_log.description_event_for_queue;
  mi->rli.relay_log.description_event_for_queue= 0;
  // TODO: make rpl_status part of Master_info
  change_rpl_status(RPL_ACTIVE_SLAVE,RPL_IDLE_SLAVE);

  mysql_mutex_lock(&LOCK_thread_count);
  thd->unlink();
  mysql_mutex_unlock(&LOCK_thread_count);
  delete thd;
  thread_safe_decrement32(&service_thread_count);
  signal_thd_deleted();

  mi->abort_slave= 0;
  mi->slave_running= MYSQL_SLAVE_NOT_RUN;
  mi->io_thd= 0;
  /*
    Note: the order of the two following calls (first broadcast, then unlock)
    is important. Otherwise a killer_thread can execute between the calls and
    delete the mi structure leading to a crash! (see BUG#25306 for details)
   */ 
  mysql_cond_broadcast(&mi->stop_cond);       // tell the world we are done
  DBUG_EXECUTE_IF("simulate_slave_delay_at_terminate_bug38694", sleep(5););
  mysql_mutex_unlock(&mi->run_lock);

  DBUG_LEAVE;                                   // Must match DBUG_ENTER()
  my_thread_end();
#ifdef HAVE_OPENSSL
  ERR_remove_state(0);
#endif
  pthread_exit(0);
  return 0;                                     // Avoid compiler warnings
}

/*
  Check the temporary directory used by commands like
  LOAD DATA INFILE.

  As the directory never changes during a mysqld run, we only
  test this once and cache the result. This also resolve a race condition
  when this can be run by multiple threads at the same time.
 */

static bool check_temp_dir_run= 0;
static int check_temp_dir_result= 0;

static 
int check_temp_dir(char* tmp_file)
{
  File fd;
  int result= 1;                                // Assume failure
  MY_DIR *dirp;
  char tmp_dir[FN_REFLEN];
  size_t tmp_dir_size;
  DBUG_ENTER("check_temp_dir");

  mysql_mutex_lock(&LOCK_thread_count);
  if (check_temp_dir_run)
  {
    result= check_temp_dir_result;
    goto end;
  }
  check_temp_dir_run= 1;

  /*
    Get the directory from the temporary file.
  */
  dirname_part(tmp_dir, tmp_file, &tmp_dir_size);

  /*
    Check if the directory exists.
   */
  if (!(dirp=my_dir(tmp_dir,MYF(MY_WME))))
    goto end;
  my_dirend(dirp);

  /*
    Check permissions to create a file. We use O_TRUNC to ensure that
    things works even if we happen to have and old file laying around.
   */
  if ((fd= mysql_file_create(key_file_misc,
                             tmp_file, CREATE_MODE,
                             O_WRONLY | O_BINARY | O_TRUNC | O_NOFOLLOW,
                             MYF(MY_WME))) < 0)
    goto end;

  result= 0;                                    // Directory name ok
  /*
    Clean up.
   */
  mysql_file_close(fd, MYF(0));
  mysql_file_delete(key_file_misc, tmp_file, MYF(0));

end:
  check_temp_dir_result= result;
  mysql_mutex_unlock(&LOCK_thread_count);
  DBUG_RETURN(result);
}


void
slave_output_error_info(rpl_group_info *rgi, THD *thd)
{
  /*
    retrieve as much info as possible from the thd and, error
    codes and warnings and print this to the error log as to
    allow the user to locate the error
  */
  Relay_log_info *rli= rgi->rli;
  uint32 const last_errno= rli->last_error().number;

  if (thd->is_error())
  {
    char const *const errmsg= thd->get_stmt_da()->message();

    DBUG_PRINT("info",
               ("thd->get_stmt_da()->sql_errno()=%d; rli->last_error.number=%d",
                thd->get_stmt_da()->sql_errno(), last_errno));
    if (last_errno == 0)
    {
      /*
        This function is reporting an error which was not reported
        while executing exec_relay_log_event().
      */ 
      rli->report(ERROR_LEVEL, thd->get_stmt_da()->sql_errno(),
                  rgi->gtid_info(), "%s", errmsg);
    }
    else if (last_errno != thd->get_stmt_da()->sql_errno())
    {
      /*
       * An error was reported while executing exec_relay_log_event()
       * however the error code differs from what is in the thread.
       * This function prints out more information to help finding
       * what caused the problem.
       */  
      sql_print_error("Slave (additional info): %s Error_code: %d",
                      errmsg, thd->get_stmt_da()->sql_errno());
    }
  }

  /* Print any warnings issued */
  Diagnostics_area::Sql_condition_iterator it=
    thd->get_stmt_da()->sql_conditions();
  const Sql_condition *err;
  /*
    Added controlled slave thread cancel for replication
    of user-defined variables.
  */
  bool udf_error = false;
  while ((err= it++))
  {
    if (err->get_sql_errno() == ER_CANT_OPEN_LIBRARY)
      udf_error = true;
    sql_print_warning("Slave: %s Error_code: %d", err->get_message_text(), err->get_sql_errno());
  }
  if (udf_error)
  {
    StringBuffer<100> tmp;
    if (rli->mi->using_gtid != Master_info::USE_GTID_NO)
    {
      tmp.append(STRING_WITH_LEN("; GTID position '"));
      rpl_append_gtid_state(&tmp, false);
      tmp.append(STRING_WITH_LEN("'"));
    }
    sql_print_error("Error loading user-defined library, slave SQL "
      "thread aborted. Install the missing library, and restart the "
      "slave SQL thread with \"SLAVE START\". We stopped at log '%s' "
      "position %llu%s", RPL_LOG_NAME, rli->group_master_log_pos,
      tmp.c_ptr_safe());
  }
  else
  {
    StringBuffer<100> tmp;
    if (rli->mi->using_gtid != Master_info::USE_GTID_NO)
    {
      tmp.append(STRING_WITH_LEN("; GTID position '"));
      rpl_append_gtid_state(&tmp, false);
      tmp.append(STRING_WITH_LEN("'"));
    }
    sql_print_error("Error running query, slave SQL thread aborted. "
                    "Fix the problem, and restart the slave SQL thread "
                    "with \"SLAVE START\". We stopped at log '%s' position "
                    "%llu%s", RPL_LOG_NAME, rli->group_master_log_pos,
                    tmp.c_ptr_safe());
  }
}


/**
  Slave SQL thread entry point.

  @param arg Pointer to Relay_log_info object that holds information
  for the SQL thread.

  @return Always 0.
*/
pthread_handler_t handle_slave_sql(void *arg)
{
  THD *thd;                     /* needs to be first for thread_stack */
  char saved_log_name[FN_REFLEN];
  char saved_master_log_name[FN_REFLEN];
  my_off_t UNINIT_VAR(saved_log_pos);
  my_off_t UNINIT_VAR(saved_master_log_pos);
  String saved_skip_gtid_pos;
  my_off_t saved_skip= 0;
  Master_info *mi= ((Master_info*)arg);
  Relay_log_info* rli = &mi->rli;
  my_bool wsrep_node_dropped __attribute__((unused)) = FALSE;
  const char *errmsg;
  rpl_group_info *serial_rgi;
  rpl_sql_thread_info sql_info(mi->rpl_filter);

  // needs to call my_thread_init(), otherwise we get a coredump in DBUG_ stuff
  my_thread_init();
  DBUG_ENTER("handle_slave_sql");

#ifdef WITH_WSREP
 wsrep_restart_point:
#endif

  serial_rgi= new rpl_group_info(rli);
  thd = new THD; // note that contructor of THD uses DBUG_ !
  thd->thread_stack = (char*)&thd; // remember where our stack is
  thd->system_thread_info.rpl_sql_info= &sql_info;

  DBUG_ASSERT(rli->inited);
  DBUG_ASSERT(rli->mi == mi);
  mysql_mutex_lock(&rli->run_lock);
  DBUG_ASSERT(!rli->slave_running);
  errmsg= 0;
#ifndef DBUG_OFF
  rli->events_till_abort = abort_slave_event_count;
#endif

  /*
    THD for the sql driver thd. In parallel replication this is the thread
    that reads things from the relay log and calls rpl_parallel::do_event()
    to execute queries.

    In single thread replication this is the THD for the thread that is
    executing SQL queries too.
  */
  serial_rgi->thd= rli->sql_driver_thd= thd;
  
  /* Inform waiting threads that slave has started */
  rli->slave_run_id++;
  rli->slave_running= MYSQL_SLAVE_RUN_NOT_CONNECT;

  pthread_detach_this_thread();
  if (init_slave_thread(thd, mi, SLAVE_THD_SQL))
  {
    /*
      TODO: this is currently broken - slave start and change master
      will be stuck if we fail here
    */
    mysql_cond_broadcast(&rli->start_cond);
    rli->report(ERROR_LEVEL, ER_SLAVE_FATAL_ERROR, NULL,
                "Failed during slave thread initialization");
    goto err_during_init;
  }
  thd->init_for_queries();
  thd->rgi_slave= serial_rgi;
  if ((serial_rgi->deferred_events_collecting= mi->rpl_filter->is_on()))
  {
    serial_rgi->deferred_events= new Deferred_log_events(rli);
  }

  /*
    binlog_annotate_row_events must be TRUE only after an Annotate_rows event
    has been received and only till the last corresponding rbr event has been
    applied. In all other cases it must be FALSE.
  */
  thd->variables.binlog_annotate_row_events= 0;
  mysql_mutex_lock(&LOCK_thread_count);
  threads.append(thd);
  mysql_mutex_unlock(&LOCK_thread_count);
  /*
    We are going to set slave_running to 1. Assuming slave I/O thread is
    alive and connected, this is going to make Seconds_Behind_Master be 0
    i.e. "caught up". Even if we're just at start of thread. Well it's ok, at
    the moment we start we can think we are caught up, and the next second we
    start receiving data so we realize we are not caught up and
    Seconds_Behind_Master grows. No big deal.
  */
  rli->abort_slave = 0;
  rli->stop_for_until= false;
  mysql_mutex_unlock(&rli->run_lock);
  mysql_cond_broadcast(&rli->start_cond);

  /*
    Reset errors for a clean start (otherwise, if the master is idle, the SQL
    thread may execute no Query_log_event, so the error will remain even
    though there's no problem anymore). Do not reset the master timestamp
    (imagine the slave has caught everything, the STOP SLAVE and START SLAVE:
    as we are not sure that we are going to receive a query, we want to
    remember the last master timestamp (to say how many seconds behind we are
    now.
    But the master timestamp is reset by RESET SLAVE & CHANGE MASTER.
  */
  rli->clear_error();
  rli->parallel.reset();

  //tell the I/O thread to take relay_log_space_limit into account from now on
  rli->ignore_log_space_limit= 0;

  serial_rgi->gtid_sub_id= 0;
  serial_rgi->gtid_pending= false;
  rli->gtid_skip_flag = GTID_SKIP_NOT;
  if (init_relay_log_pos(rli,
                         rli->group_relay_log_name,
                         rli->group_relay_log_pos,
                         1 /*need data lock*/, &errmsg,
                         1 /*look for a description_event*/))
  { 
    rli->report(ERROR_LEVEL, ER_SLAVE_FATAL_ERROR, NULL,
                "Error initializing relay log position: %s", errmsg);
    goto err;
  }
  rli->reset_inuse_relaylog();
  if (rli->alloc_inuse_relaylog(rli->group_relay_log_name))
    goto err;

  strcpy(rli->future_event_master_log_name, rli->group_master_log_name);
  THD_CHECK_SENTRY(thd);
#ifndef DBUG_OFF
  {
    DBUG_PRINT("info", ("my_b_tell(rli->cur_log)=%llu "
                        "rli->event_relay_log_pos=%llu",
                        my_b_tell(rli->cur_log), rli->event_relay_log_pos));
    DBUG_ASSERT(rli->event_relay_log_pos >= BIN_LOG_HEADER_SIZE);
    /*
      Wonder if this is correct. I (Guilhem) wonder if my_b_tell() returns the
      correct position when it's called just after my_b_seek() (the questionable
      stuff is those "seek is done on next read" comments in the my_b_seek()
      source code).
      The crude reality is that this assertion randomly fails whereas
      replication seems to work fine. And there is no easy explanation why it
      fails (as we my_b_seek(rli->event_relay_log_pos) at the very end of
      init_relay_log_pos() called above). Maybe the assertion would be
      meaningful if we held rli->data_lock between the my_b_seek() and the
      DBUG_ASSERT().
    */
#ifdef SHOULD_BE_CHECKED
    DBUG_ASSERT(my_b_tell(rli->cur_log) == rli->event_relay_log_pos);
#endif
  }
#endif

#ifdef WITH_WSREP
  thd->wsrep_exec_mode= LOCAL_STATE;
  /* synchronize with wsrep replication */
  if (WSREP_ON)
    wsrep_ready_wait();
#endif
  DBUG_PRINT("master_info",("log_file_name: %s  position: %llu",
                            rli->group_master_log_name,
                            rli->group_master_log_pos));
  if (global_system_variables.log_warnings)
  {
    StringBuffer<100> tmp;
    if (mi->using_gtid != Master_info::USE_GTID_NO)
    {
      tmp.append(STRING_WITH_LEN("; GTID position '"));
      rpl_append_gtid_state(&tmp,
                            mi->using_gtid==Master_info::USE_GTID_CURRENT_POS);
      tmp.append(STRING_WITH_LEN("'"));
    }
    sql_print_information("Slave SQL thread initialized, starting replication "
                          "in log '%s' at position %llu, relay log '%s' "
                          "position: %llu%s", RPL_LOG_NAME,
                    rli->group_master_log_pos, rli->group_relay_log_name,
                    rli->group_relay_log_pos, tmp.c_ptr_safe());
  }

  if (check_temp_dir(rli->slave_patternload_file))
  {
    rli->report(ERROR_LEVEL, thd->get_stmt_da()->sql_errno(), NULL,
                "Unable to use slave's temporary directory %s - %s", 
                slave_load_tmpdir, thd->get_stmt_da()->message());
    goto err;
  }

  /* Load the set of seen GTIDs, if we did not already. */
  if (rpl_load_gtid_slave_state(thd))
  {
    rli->report(ERROR_LEVEL, thd->get_stmt_da()->sql_errno(), NULL,
                "Unable to load replication GTID slave state from mysql.%s: %s",
                rpl_gtid_slave_state_table_name.str,
                thd->get_stmt_da()->message());
    /*
      If we are using old-style replication, we can continue, even though we
      then will not be able to record the GTIDs we receive. But if using GTID,
      we must give up.
    */
    if (mi->using_gtid != Master_info::USE_GTID_NO || opt_gtid_strict_mode)
      goto err;
  }

  /* execute init_slave variable */
  if (opt_init_slave.length)
  {
    execute_init_command(thd, &opt_init_slave, &LOCK_sys_init_slave);
    if (thd->is_slave_error)
    {
      rli->report(ERROR_LEVEL, thd->get_stmt_da()->sql_errno(), NULL,
                  "Slave SQL thread aborted. Can't execute init_slave query");
      goto err;
    }
  }

  /*
    First check until condition - probably there is nothing to execute. We
    do not want to wait for next event in this case.
  */
  mysql_mutex_lock(&rli->data_lock);
  if (rli->slave_skip_counter)
  {
    strmake_buf(saved_log_name, rli->group_relay_log_name);
    strmake_buf(saved_master_log_name, rli->group_master_log_name);
    saved_log_pos= rli->group_relay_log_pos;
    saved_master_log_pos= rli->group_master_log_pos;
    if (mi->using_gtid != Master_info::USE_GTID_NO)
    {
      saved_skip_gtid_pos.append(STRING_WITH_LEN(", GTID '"));
      rpl_append_gtid_state(&saved_skip_gtid_pos, false);
      saved_skip_gtid_pos.append(STRING_WITH_LEN("'; "));
    }
    saved_skip= rli->slave_skip_counter;
  }
  if ((rli->until_condition == Relay_log_info::UNTIL_MASTER_POS ||
       rli->until_condition == Relay_log_info::UNTIL_RELAY_POS) &&
      rli->is_until_satisfied(rli->group_master_log_pos))
  {
    sql_print_information("Slave SQL thread stopped because it reached its"
                          " UNTIL position %llu", rli->until_pos());
    mysql_mutex_unlock(&rli->data_lock);
    goto err;
  }
  mysql_mutex_unlock(&rli->data_lock);

  /* Read queries from the IO/THREAD until this thread is killed */

  while (!sql_slave_killed(serial_rgi))
  {
    THD_STAGE_INFO(thd, stage_reading_event_from_the_relay_log);
    THD_CHECK_SENTRY(thd);

    if (saved_skip && rli->slave_skip_counter == 0)
    {
      StringBuffer<100> tmp;
      if (mi->using_gtid != Master_info::USE_GTID_NO)
      {
        tmp.append(STRING_WITH_LEN(", GTID '"));
        rpl_append_gtid_state(&tmp, false);
        tmp.append(STRING_WITH_LEN("'; "));
      }

      sql_print_information("'SQL_SLAVE_SKIP_COUNTER=%ld' executed at "
        "relay_log_file='%s', relay_log_pos='%ld', master_log_name='%s', "
        "master_log_pos='%ld'%s and new position at "
        "relay_log_file='%s', relay_log_pos='%ld', master_log_name='%s', "
        "master_log_pos='%ld'%s ",
        (ulong) saved_skip, saved_log_name, (ulong) saved_log_pos,
        saved_master_log_name, (ulong) saved_master_log_pos,
        saved_skip_gtid_pos.c_ptr_safe(),
        rli->group_relay_log_name, (ulong) rli->group_relay_log_pos,
        rli->group_master_log_name, (ulong) rli->group_master_log_pos,
        tmp.c_ptr_safe());
      saved_skip= 0;
      saved_skip_gtid_pos.free();
    }

    if (exec_relay_log_event(thd, rli, serial_rgi))
    {
#ifdef WITH_WSREP
      if (thd->wsrep_conflict_state != NO_CONFLICT)
      {
        wsrep_node_dropped= TRUE;
        rli->abort_slave= TRUE;
      }
#endif /* WITH_WSREP */

      DBUG_PRINT("info", ("exec_relay_log_event() failed"));
      // do not scare the user if SQL thread was simply killed or stopped
      if (!sql_slave_killed(serial_rgi))
      {
        slave_output_error_info(serial_rgi, thd);
        if (WSREP_ON && rli->last_error().number == ER_UNKNOWN_COM_ERROR)
          wsrep_node_dropped= TRUE;
      }
      goto err;
    }
  }

  if (mi->using_parallel())
    rli->parallel.wait_for_done(thd, rli);

  /* Thread stopped. Print the current replication position to the log */
  {
    StringBuffer<100> tmp;
    if (mi->using_gtid != Master_info::USE_GTID_NO)
    {
      tmp.append(STRING_WITH_LEN("; GTID position '"));
      rpl_append_gtid_state(&tmp, false);
      tmp.append(STRING_WITH_LEN("'"));
    }
    sql_print_information("Slave SQL thread exiting, replication stopped in "
                          "log '%s' at position %llu%s", RPL_LOG_NAME,
                          rli->group_master_log_pos, tmp.c_ptr_safe());
  }

 err:

  /*
    Once again, in case we aborted with an error and skipped the first one.
    (We want the first one to be before the printout of stop position to
    get the correct position printed.)
  */
  if (mi->using_parallel())
    rli->parallel.wait_for_done(thd, rli);

  /*
    Some events set some playgrounds, which won't be cleared because thread
    stops. Stopping of this thread may not be known to these events ("stop"
    request is detected only by the present function, not by events), so we
    must "proactively" clear playgrounds:
  */
  thd->clear_error();
  serial_rgi->cleanup_context(thd, 1);
  /*
    Some extra safety, which should not been needed (normally, event deletion
    should already have done these assignments (each event which sets these
    variables is supposed to set them to 0 before terminating)).
  */
  thd->catalog= 0;
  thd->reset_query();
  thd->reset_db(NULL, 0);
  if (rli->mi->using_gtid != Master_info::USE_GTID_NO)
  {
    ulong domain_count;

    flush_relay_log_info(rli);
    if (mi->using_parallel())
    {
      /*
        In parallel replication GTID mode, we may stop with different domains
        at different positions in the relay log.

        To handle this when we restart the SQL thread, mark the current
        per-domain position in the Relay_log_info.
      */
      mysql_mutex_lock(&rpl_global_gtid_slave_state->LOCK_slave_state);
      domain_count= rpl_global_gtid_slave_state->count();
      mysql_mutex_unlock(&rpl_global_gtid_slave_state->LOCK_slave_state);
      if (domain_count > 1)
      {
        inuse_relaylog *ir;

        /*
          Load the starting GTID position, so that we can skip already applied
          GTIDs when we restart the SQL thread. And set the start position in
          the relay log back to a known safe place to start (prior to any not
          yet applied transaction in any domain).
        */
        rli->restart_gtid_pos.load(rpl_global_gtid_slave_state, NULL, 0);
        if ((ir= rli->inuse_relaylog_list))
        {
          rpl_gtid *gtid= ir->relay_log_state;
          uint32 count= ir->relay_log_state_count;
          while (count > 0)
          {
            process_gtid_for_restart_pos(rli, gtid);
            ++gtid;
            --count;
          }
          strmake_buf(rli->group_relay_log_name, ir->name);
          rli->group_relay_log_pos= BIN_LOG_HEADER_SIZE;
          rli->relay_log_state.load(ir->relay_log_state, ir->relay_log_state_count);
        }
      }
    }
  }
  THD_STAGE_INFO(thd, stage_waiting_for_slave_mutex_on_exit);
  thd->add_status_to_global();
  mysql_mutex_lock(&rli->run_lock);
err_during_init:
  /* We need data_lock, at least to wake up any waiting master_pos_wait() */
  mysql_mutex_lock(&rli->data_lock);
  DBUG_ASSERT(rli->slave_running == MYSQL_SLAVE_RUN_NOT_CONNECT); // tracking buffer overrun
  /* When master_pos_wait() wakes up it will check this and terminate */
  rli->slave_running= MYSQL_SLAVE_NOT_RUN;
  /* Forget the relay log's format */
  delete rli->relay_log.description_event_for_exec;
  rli->relay_log.description_event_for_exec= 0;
  rli->reset_inuse_relaylog();
  /* Wake up master_pos_wait() */
  mysql_mutex_unlock(&rli->data_lock);
  DBUG_PRINT("info",("Signaling possibly waiting master_pos_wait() functions"));
  mysql_cond_broadcast(&rli->data_cond);
  rli->ignore_log_space_limit= 0; /* don't need any lock */
  /* we die so won't remember charset - re-update them on next thread start */
  thd->system_thread_info.rpl_sql_info->cached_charset_invalidate();

  /*
    TODO: see if we can do this conditionally in next_event() instead
    to avoid unneeded position re-init
  */
  thd->temporary_tables = 0; // remove tempation from destructor to close them
  THD_CHECK_SENTRY(thd);
  rli->sql_driver_thd= 0;
  mysql_mutex_lock(&LOCK_thread_count);
  thd->rgi_fake= thd->rgi_slave= NULL;
  delete serial_rgi;
  mysql_mutex_unlock(&LOCK_thread_count);

#ifdef WITH_WSREP
  /*
    If slave stopped due to node going non primary, we set global flag to
    trigger automatic restart of slave when node joins back to cluster.
  */
  if (WSREP_ON && wsrep_node_dropped && wsrep_restart_slave)
  {
    if (wsrep_ready)
    {
      WSREP_INFO("Slave error due to node temporarily non-primary"
                 "SQL slave will continue");
      wsrep_node_dropped= FALSE;
      mysql_mutex_unlock(&rli->run_lock);
      WSREP_DEBUG("wsrep_conflict_state now: %d", thd->wsrep_conflict_state);
      WSREP_INFO("slave restart: %d", thd->wsrep_conflict_state);
      thd->wsrep_conflict_state= NO_CONFLICT;
      goto wsrep_restart_point;
    } else {
      WSREP_INFO("Slave error due to node going non-primary");
      WSREP_INFO("wsrep_restart_slave was set and therefore slave will be "
                 "automatically restarted when node joins back to cluster.");
      wsrep_restart_slave_activated= TRUE;
    }
  }
#endif /* WITH_WSREP */

 /*
  Note: the order of the broadcast and unlock calls below (first broadcast, then unlock)
  is important. Otherwise a killer_thread can execute between the calls and
  delete the mi structure leading to a crash! (see BUG#25306 for details)
 */ 
  mysql_cond_broadcast(&rli->stop_cond);
  DBUG_EXECUTE_IF("simulate_slave_delay_at_terminate_bug38694", sleep(5););
  mysql_mutex_unlock(&rli->run_lock);  // tell the world we are done

  /*
    Deactivate the parallel replication thread pool, if there are now no more
    SQL threads running. Do this here, when we have released all locks, but
    while our THD (and current_thd) is still valid.
  */
  mysql_mutex_lock(&LOCK_active_mi);
  if (opt_slave_parallel_threads > 0 &&
      master_info_index &&// master_info_index is set to NULL on server shutdown
      !master_info_index->any_slave_sql_running())
    rpl_parallel_inactivate_pool(&global_rpl_thread_pool);
  mysql_mutex_unlock(&LOCK_active_mi);

  mysql_mutex_lock(&LOCK_thread_count);
  thd->unlink();
  mysql_mutex_unlock(&LOCK_thread_count);
  delete thd;
  thread_safe_decrement32(&service_thread_count);
  signal_thd_deleted();

  DBUG_LEAVE;                                   // Must match DBUG_ENTER()
  my_thread_end();
#ifdef HAVE_OPENSSL
  ERR_remove_state(0);
#endif
  pthread_exit(0);
  return 0;                                     // Avoid compiler warnings
}


/*
  process_io_create_file()
*/

static int process_io_create_file(Master_info* mi, Create_file_log_event* cev)
{
  int error = 1;
  ulong num_bytes;
  bool cev_not_written;
  THD *thd = mi->io_thd;
  NET *net = &mi->mysql->net;
  DBUG_ENTER("process_io_create_file");

  if (unlikely(!cev->is_valid()))
    DBUG_RETURN(1);

  if (!mi->rpl_filter->db_ok(cev->db))
  {
    skip_load_data_infile(net);
    DBUG_RETURN(0);
  }
  DBUG_ASSERT(cev->inited_from_old);
  thd->file_id = cev->file_id = mi->file_id++;
  thd->variables.server_id = cev->server_id;
  cev_not_written = 1;

  if (unlikely(net_request_file(net,cev->fname)))
  {
    sql_print_error("Slave I/O: failed requesting download of '%s'",
                    cev->fname);
    goto err;
  }

  /*
    This dummy block is so we could instantiate Append_block_log_event
    once and then modify it slightly instead of doing it multiple times
    in the loop
  */
  {
    Append_block_log_event aev(thd,0,0,0,0);

    for (;;)
    {
      if (unlikely((num_bytes=my_net_read(net)) == packet_error))
      {
        sql_print_error("Network read error downloading '%s' from master",
                        cev->fname);
        goto err;
      }
      if (unlikely(!num_bytes)) /* eof */
      {
	/* 3.23 master wants it */
        net_write_command(net, 0, (uchar*) "", 0, (uchar*) "", 0);
        /*
          If we wrote Create_file_log_event, then we need to write
          Execute_load_log_event. If we did not write Create_file_log_event,
          then this is an empty file and we can just do as if the LOAD DATA
          INFILE had not existed, i.e. write nothing.
        */
        if (unlikely(cev_not_written))
          break;
        Execute_load_log_event xev(thd,0,0);
        xev.log_pos = cev->log_pos;
        if (unlikely(mi->rli.relay_log.append(&xev)))
        {
          mi->report(ERROR_LEVEL, ER_SLAVE_RELAY_LOG_WRITE_FAILURE, NULL,
                     ER_THD(thd, ER_SLAVE_RELAY_LOG_WRITE_FAILURE),
                     "error writing Exec_load event to relay log");
          goto err;
        }
        mi->rli.relay_log.harvest_bytes_written(&mi->rli.log_space_total);
        break;
      }
      if (unlikely(cev_not_written))
      {
        cev->block = net->read_pos;
        cev->block_len = num_bytes;
        if (unlikely(mi->rli.relay_log.append(cev)))
        {
          mi->report(ERROR_LEVEL, ER_SLAVE_RELAY_LOG_WRITE_FAILURE, NULL,
                     ER_THD(thd, ER_SLAVE_RELAY_LOG_WRITE_FAILURE),
                     "error writing Create_file event to relay log");
          goto err;
        }
        cev_not_written=0;
        mi->rli.relay_log.harvest_bytes_written(&mi->rli.log_space_total);
      }
      else
      {
        aev.block = net->read_pos;
        aev.block_len = num_bytes;
        aev.log_pos = cev->log_pos;
        if (unlikely(mi->rli.relay_log.append(&aev)))
        {
          mi->report(ERROR_LEVEL, ER_SLAVE_RELAY_LOG_WRITE_FAILURE, NULL,
                     ER_THD(thd, ER_SLAVE_RELAY_LOG_WRITE_FAILURE),
                     "error writing Append_block event to relay log");
          goto err;
        }
        mi->rli.relay_log.harvest_bytes_written(&mi->rli.log_space_total) ;
      }
    }
  }
  error=0;
err:
  DBUG_RETURN(error);
}


/*
  Start using a new binary log on the master

  SYNOPSIS
    process_io_rotate()
    mi                  master_info for the slave
    rev                 The rotate log event read from the binary log

  DESCRIPTION
    Updates the master info with the place in the next binary
    log where we should start reading.
    Rotate the relay log to avoid mixed-format relay logs.

  NOTES
    We assume we already locked mi->data_lock

  RETURN VALUES
    0           ok
    1           Log event is illegal

*/

static int process_io_rotate(Master_info *mi, Rotate_log_event *rev)
{
  DBUG_ENTER("process_io_rotate");
  mysql_mutex_assert_owner(&mi->data_lock);

  if (unlikely(!rev->is_valid()))
    DBUG_RETURN(1);

  /* Safe copy as 'rev' has been "sanitized" in Rotate_log_event's ctor */
  memcpy(mi->master_log_name, rev->new_log_ident, rev->ident_len+1);
  mi->master_log_pos= rev->pos;
  DBUG_PRINT("info", ("master_log_pos: '%s' %lu",
                      mi->master_log_name, (ulong) mi->master_log_pos));
#ifndef DBUG_OFF
  /*
    If we do not do this, we will be getting the first
    rotate event forever, so we need to not disconnect after one.
  */
  if (disconnect_slave_event_count)
    mi->events_till_disconnect++;
#endif

  /*
    If description_event_for_queue is format <4, there is conversion in the
    relay log to the slave's format (4). And Rotate can mean upgrade or
    nothing. If upgrade, it's to 5.0 or newer, so we will get a Format_desc, so
    no need to reset description_event_for_queue now. And if it's nothing (same
    master version as before), no need (still using the slave's format).
  */
  if (mi->rli.relay_log.description_event_for_queue->binlog_version >= 4)
  {
    DBUG_ASSERT(mi->rli.relay_log.description_event_for_queue->checksum_alg ==
                mi->rli.relay_log.relay_log_checksum_alg);
    
    delete mi->rli.relay_log.description_event_for_queue;
    /* start from format 3 (MySQL 4.0) again */
    mi->rli.relay_log.description_event_for_queue= new
      Format_description_log_event(3);
    mi->rli.relay_log.description_event_for_queue->checksum_alg=
      mi->rli.relay_log.relay_log_checksum_alg;    
  }
  /*
    Rotate the relay log makes binlog format detection easier (at next slave
    start or mysqlbinlog)
  */
  DBUG_RETURN(rotate_relay_log(mi) /* will take the right mutexes */);
}

/*
  Reads a 3.23 event and converts it to the slave's format. This code was
  copied from MySQL 4.0.
*/
static int queue_binlog_ver_1_event(Master_info *mi, const char *buf,
                           ulong event_len)
{
  const char *errmsg = 0;
  ulong inc_pos;
  bool ignore_event= 0;
  char *tmp_buf = 0;
  Relay_log_info *rli= &mi->rli;
  DBUG_ENTER("queue_binlog_ver_1_event");

  /*
    If we get Load event, we need to pass a non-reusable buffer
    to read_log_event, so we do a trick
  */
  if ((uchar)buf[EVENT_TYPE_OFFSET] == LOAD_EVENT)
  {
    if (unlikely(!(tmp_buf=(char*)my_malloc(event_len+1,MYF(MY_WME)))))
    {
      mi->report(ERROR_LEVEL, ER_SLAVE_FATAL_ERROR, NULL,
                 ER(ER_SLAVE_FATAL_ERROR), "Memory allocation failed");
      DBUG_RETURN(1);
    }
    memcpy(tmp_buf,buf,event_len);
    /*
      Create_file constructor wants a 0 as last char of buffer, this 0 will
      serve as the string-termination char for the file's name (which is at the
      end of the buffer)
      We must increment event_len, otherwise the event constructor will not see
      this end 0, which leads to segfault.
    */
    tmp_buf[event_len++]=0;
    int4store(tmp_buf+EVENT_LEN_OFFSET, event_len);
    buf = (const char*)tmp_buf;
  }
  /*
    This will transform LOAD_EVENT into CREATE_FILE_EVENT, ask the master to
    send the loaded file, and write it to the relay log in the form of
    Append_block/Exec_load (the SQL thread needs the data, as that thread is not
    connected to the master).
  */
  Log_event *ev=
    Log_event::read_log_event(buf, event_len, &errmsg,
                              mi->rli.relay_log.description_event_for_queue, 0);
  if (unlikely(!ev))
  {
    sql_print_error("Read invalid event from master: '%s',\
 master could be corrupt but a more likely cause of this is a bug",
                    errmsg);
    my_free(tmp_buf);
    DBUG_RETURN(1);
  }

  mysql_mutex_lock(&mi->data_lock);
  ev->log_pos= mi->master_log_pos; /* 3.23 events don't contain log_pos */
  switch (ev->get_type_code()) {
  case STOP_EVENT:
    ignore_event= 1;
    inc_pos= event_len;
    break;
  case ROTATE_EVENT:
    if (unlikely(process_io_rotate(mi,(Rotate_log_event*)ev)))
    {
      delete ev;
      mysql_mutex_unlock(&mi->data_lock);
      DBUG_RETURN(1);
    }
    inc_pos= 0;
    break;
  case CREATE_FILE_EVENT:
    /*
      Yes it's possible to have CREATE_FILE_EVENT here, even if we're in
      queue_old_event() which is for 3.23 events which don't comprise
      CREATE_FILE_EVENT. This is because read_log_event() above has just
      transformed LOAD_EVENT into CREATE_FILE_EVENT.
    */
  {
    /* We come here when and only when tmp_buf != 0 */
    DBUG_ASSERT(tmp_buf != 0);
    inc_pos=event_len;
    ev->log_pos+= inc_pos;
    int error = process_io_create_file(mi,(Create_file_log_event*)ev);
    delete ev;
    mi->master_log_pos += inc_pos;
    DBUG_PRINT("info", ("master_log_pos: %lu", (ulong) mi->master_log_pos));
    mysql_mutex_unlock(&mi->data_lock);
    my_free(tmp_buf);
    DBUG_RETURN(error);
  }
  default:
    inc_pos= event_len;
    break;
  }
  if (likely(!ignore_event))
  {
    if (ev->log_pos)
      /*
         Don't do it for fake Rotate events (see comment in
      Log_event::Log_event(const char* buf...) in log_event.cc).
      */
      ev->log_pos+= event_len; /* make log_pos be the pos of the end of the event */
    if (unlikely(rli->relay_log.append(ev)))
    {
      delete ev;
      mysql_mutex_unlock(&mi->data_lock);
      DBUG_RETURN(1);
    }
    rli->relay_log.harvest_bytes_written(&rli->log_space_total);
  }
  delete ev;
  mi->master_log_pos+= inc_pos;
  DBUG_PRINT("info", ("master_log_pos: %lu", (ulong) mi->master_log_pos));
  mysql_mutex_unlock(&mi->data_lock);
  DBUG_RETURN(0);
}

/*
  Reads a 4.0 event and converts it to the slave's format. This code was copied
  from queue_binlog_ver_1_event(), with some affordable simplifications.
*/
static int queue_binlog_ver_3_event(Master_info *mi, const char *buf,
                           ulong event_len)
{
  const char *errmsg = 0;
  ulong inc_pos;
  char *tmp_buf = 0;
  Relay_log_info *rli= &mi->rli;
  DBUG_ENTER("queue_binlog_ver_3_event");

  /* read_log_event() will adjust log_pos to be end_log_pos */
  Log_event *ev=
    Log_event::read_log_event(buf,event_len, &errmsg,
                              mi->rli.relay_log.description_event_for_queue, 0);
  if (unlikely(!ev))
  {
    sql_print_error("Read invalid event from master: '%s',\
 master could be corrupt but a more likely cause of this is a bug",
                    errmsg);
    my_free(tmp_buf);
    DBUG_RETURN(1);
  }
  mysql_mutex_lock(&mi->data_lock);
  switch (ev->get_type_code()) {
  case STOP_EVENT:
    goto err;
  case ROTATE_EVENT:
    if (unlikely(process_io_rotate(mi,(Rotate_log_event*)ev)))
    {
      delete ev;
      mysql_mutex_unlock(&mi->data_lock);
      DBUG_RETURN(1);
    }
    inc_pos= 0;
    break;
  default:
    inc_pos= event_len;
    break;
  }

  if (unlikely(rli->relay_log.append(ev)))
  {
    delete ev;
    mysql_mutex_unlock(&mi->data_lock);
    DBUG_RETURN(1);
  }
  rli->relay_log.harvest_bytes_written(&rli->log_space_total);
  delete ev;
  mi->master_log_pos+= inc_pos;
err:
  DBUG_PRINT("info", ("master_log_pos: %lu", (ulong) mi->master_log_pos));
  mysql_mutex_unlock(&mi->data_lock);
  DBUG_RETURN(0);
}

/*
  queue_old_event()

  Writes a 3.23 or 4.0 event to the relay log, after converting it to the 5.0
  (exactly, slave's) format. To do the conversion, we create a 5.0 event from
  the 3.23/4.0 bytes, then write this event to the relay log.

  TODO:
    Test this code before release - it has to be tested on a separate
    setup with 3.23 master or 4.0 master
*/

static int queue_old_event(Master_info *mi, const char *buf,
                           ulong event_len)
{
  DBUG_ENTER("queue_old_event");

  switch (mi->rli.relay_log.description_event_for_queue->binlog_version)
  {
  case 1:
      DBUG_RETURN(queue_binlog_ver_1_event(mi,buf,event_len));
  case 3:
      DBUG_RETURN(queue_binlog_ver_3_event(mi,buf,event_len));
  default: /* unsupported format; eg version 2 */
    DBUG_PRINT("info",("unsupported binlog format %d in queue_old_event()",
                       mi->rli.relay_log.description_event_for_queue->binlog_version));
    DBUG_RETURN(1);
  }
}

/*
  queue_event()

  If the event is 3.23/4.0, passes it to queue_old_event() which will convert
  it. Otherwise, writes a 5.0 (or newer) event to the relay log. Then there is
  no format conversion, it's pure read/write of bytes.
  So a 5.0.0 slave's relay log can contain events in the slave's format or in
  any >=5.0.0 format.
*/

static int queue_event(Master_info* mi,const char* buf, ulong event_len)
{
  int error= 0;
  StringBuffer<1024> error_msg;
  ulonglong inc_pos;
  ulonglong event_pos;
  Relay_log_info *rli= &mi->rli;
  mysql_mutex_t *log_lock= rli->relay_log.get_log_lock();
  ulong s_id;
  bool unlock_data_lock= TRUE;
  bool gtid_skip_enqueue= false;
  bool got_gtid_event= false;
  rpl_gtid event_gtid;

  /*
    FD_q must have been prepared for the first R_a event
    inside get_master_version_and_clock()
    Show-up of FD:s affects checksum_alg at once because
    that changes FD_queue.
  */
  enum enum_binlog_checksum_alg checksum_alg=
    mi->checksum_alg_before_fd != BINLOG_CHECKSUM_ALG_UNDEF ?
    mi->checksum_alg_before_fd : mi->rli.relay_log.relay_log_checksum_alg;

  char *save_buf= NULL; // needed for checksumming the fake Rotate event
  char rot_buf[LOG_EVENT_HEADER_LEN + ROTATE_HEADER_LEN + FN_REFLEN];

  DBUG_ASSERT(checksum_alg == BINLOG_CHECKSUM_ALG_OFF || 
              checksum_alg == BINLOG_CHECKSUM_ALG_UNDEF || 
              checksum_alg == BINLOG_CHECKSUM_ALG_CRC32); 

  DBUG_ENTER("queue_event");
  /*
    FD_queue checksum alg description does not apply in a case of
    FD itself. The one carries both parts of the checksum data.
  */
  if (buf[EVENT_TYPE_OFFSET] == FORMAT_DESCRIPTION_EVENT)
  {
    checksum_alg= get_checksum_alg(buf, event_len);
  }
  else if (buf[EVENT_TYPE_OFFSET] == START_EVENT_V3)
  {
    // checksum behaviour is similar to the pre-checksum FD handling
    mi->checksum_alg_before_fd= BINLOG_CHECKSUM_ALG_UNDEF;
    mi->rli.relay_log.description_event_for_queue->checksum_alg=
      mi->rli.relay_log.relay_log_checksum_alg= checksum_alg=
      BINLOG_CHECKSUM_ALG_OFF;
  }

  // does not hold always because of old binlog can work with NM 
  // DBUG_ASSERT(checksum_alg != BINLOG_CHECKSUM_ALG_UNDEF);

  // should hold unless manipulations with RL. Tests that do that
  // will have to refine the clause.
  DBUG_ASSERT(mi->rli.relay_log.relay_log_checksum_alg !=
              BINLOG_CHECKSUM_ALG_UNDEF);
              
  // Emulate the network corruption
  DBUG_EXECUTE_IF("corrupt_queue_event",
    if (buf[EVENT_TYPE_OFFSET] != FORMAT_DESCRIPTION_EVENT)
    {
      char *debug_event_buf_c = (char*) buf;
      int debug_cor_pos = rand() % (event_len - BINLOG_CHECKSUM_LEN);
      debug_event_buf_c[debug_cor_pos] =~ debug_event_buf_c[debug_cor_pos];
      DBUG_PRINT("info", ("Corrupt the event at queue_event: byte on position %d", debug_cor_pos));
      DBUG_SET("-d,corrupt_queue_event");
    }
  );
                                              
  if (event_checksum_test((uchar *) buf, event_len, checksum_alg))
  {
    error= ER_NETWORK_READ_EVENT_CHECKSUM_FAILURE;
    unlock_data_lock= FALSE;
    goto err;
  }

  if (mi->rli.relay_log.description_event_for_queue->binlog_version<4 &&
      (uchar)buf[EVENT_TYPE_OFFSET] != FORMAT_DESCRIPTION_EVENT /* a way to escape */)
    DBUG_RETURN(queue_old_event(mi,buf,event_len));

  mysql_mutex_lock(&mi->data_lock);

  switch ((uchar)buf[EVENT_TYPE_OFFSET]) {
  case STOP_EVENT:
    /*
      We needn't write this event to the relay log. Indeed, it just indicates a
      master server shutdown. The only thing this does is cleaning. But
      cleaning is already done on a per-master-thread basis (as the master
      server is shutting down cleanly, it has written all DROP TEMPORARY TABLE
      prepared statements' deletion are TODO only when we binlog prep stmts).

      We don't even increment mi->master_log_pos, because we may be just after
      a Rotate event. Btw, in a few milliseconds we are going to have a Start
      event from the next binlog (unless the master is presently running
      without --log-bin).
    */
    goto err;
  case ROTATE_EVENT:
  {
    Rotate_log_event rev(buf, checksum_alg != BINLOG_CHECKSUM_ALG_OFF ?
                         event_len - BINLOG_CHECKSUM_LEN : event_len,
                         mi->rli.relay_log.description_event_for_queue);

    if (unlikely(mi->gtid_reconnect_event_skip_count) &&
        unlikely(!mi->gtid_event_seen) &&
        rev.is_artificial_event() &&
        (mi->prev_master_id != mi->master_id ||
         strcmp(rev.new_log_ident, mi->master_log_name) != 0))
    {
      /*
        Artificial Rotate_log_event is the first event we receive at the start
        of each master binlog file. It gives the name of the new binlog file.

        Normally, we already have this name from the real rotate event at the
        end of the previous binlog file (unless we are making a new connection
        using GTID). But if the master server restarted/crashed, there is no
        rotate event at the end of the prior binlog file, so the name is new.

        We use this fact to handle a special case of master crashing. If the
        master crashed while writing the binlog, it might end with a partial
        event group lacking the COMMIT/XID event, which must be rolled
        back. If the slave IO thread happens to get a disconnect in the middle
        of exactly this event group, it will try to reconnect at the same GTID
        and skip already fetched events. However, that GTID did not commit on
        the master before the crash, so it does not really exist, and the
        master will connect the slave at the next following GTID starting in
        the next binlog. This could confuse the slave and make it mix the
        start of one event group with the end of another.

        But we detect this case here, by noticing the change of binlog name
        which detects the missing rotate event at the end of the previous
        binlog file. In this case, we reset the counters to make us not skip
        the next event group, and queue an artificial Format Description
        event. The previously fetched incomplete event group will then be
        rolled back when the Format Description event is executed by the SQL
        thread.

        A similar case is if the reconnect somehow connects to a different
        master server (like due to a network proxy or IP address takeover).
        We detect this case by noticing a change of server_id and in this
        case likewise rollback the partially received event group.
      */
      Format_description_log_event fdle(4);

      if (mi->prev_master_id != mi->master_id)
        sql_print_warning("The server_id of master server changed in the "
                          "middle of GTID %u-%u-%llu. Assuming a change of "
                          "master server, so rolling back the previously "
                          "received partial transaction. Expected: %lu, "
                          "received: %lu", mi->last_queued_gtid.domain_id,
                          mi->last_queued_gtid.server_id,
                          mi->last_queued_gtid.seq_no,
                          mi->prev_master_id, mi->master_id);
      else if (strcmp(rev.new_log_ident, mi->master_log_name) != 0)
        sql_print_warning("Unexpected change of master binlog file name in the "
                          "middle of GTID %u-%u-%llu, assuming that master has "
                          "crashed and rolling back the transaction. Expected: "
                          "'%s', received: '%s'",
                          mi->last_queued_gtid.domain_id,
                          mi->last_queued_gtid.server_id,
                          mi->last_queued_gtid.seq_no,
                          mi->master_log_name, rev.new_log_ident);

      mysql_mutex_lock(log_lock);
      if (likely(!rli->relay_log.write_event(&fdle) &&
                 !rli->relay_log.flush_and_sync(NULL)))
      {
        rli->relay_log.harvest_bytes_written(&rli->log_space_total);
      }
      else
      {
        error= ER_SLAVE_RELAY_LOG_WRITE_FAILURE;
        mysql_mutex_unlock(log_lock);
        goto err;
      }
      rli->relay_log.signal_update();
      mysql_mutex_unlock(log_lock);

      mi->gtid_reconnect_event_skip_count= 0;
      mi->events_queued_since_last_gtid= 0;
    }
    mi->prev_master_id= mi->master_id;

    if (unlikely(process_io_rotate(mi, &rev)))
    {
      error= ER_SLAVE_RELAY_LOG_WRITE_FAILURE;
      goto err;
    }
    /* 
       Checksum special cases for the fake Rotate (R_f) event caused by the protocol
       of events generation and serialization in RL where Rotate of master is 
       queued right next to FD of slave.
       Since it's only FD that carries the alg desc of FD_s has to apply to R_m.
       Two special rules apply only to the first R_f which comes in before any FD_m.
       The 2nd R_f should be compatible with the FD_s that must have taken over
       the last seen FD_m's (A).
       
       RSC_1: If OM \and fake Rotate \and slave is configured to
              to compute checksum for its first FD event for RL
              the fake Rotate gets checksummed here.
    */
    if (uint4korr(&buf[0]) == 0 && checksum_alg == BINLOG_CHECKSUM_ALG_OFF &&
        mi->rli.relay_log.relay_log_checksum_alg != BINLOG_CHECKSUM_ALG_OFF)
    {
      ha_checksum rot_crc= 0;
      event_len += BINLOG_CHECKSUM_LEN;
      memcpy(rot_buf, buf, event_len - BINLOG_CHECKSUM_LEN);
      int4store(&rot_buf[EVENT_LEN_OFFSET],
                uint4korr(&rot_buf[EVENT_LEN_OFFSET]) + BINLOG_CHECKSUM_LEN);
      rot_crc= my_checksum(rot_crc, (const uchar *) rot_buf,
                           event_len - BINLOG_CHECKSUM_LEN);
      int4store(&rot_buf[event_len - BINLOG_CHECKSUM_LEN], rot_crc);
      DBUG_ASSERT(event_len == uint4korr(&rot_buf[EVENT_LEN_OFFSET]));
      DBUG_ASSERT(mi->rli.relay_log.description_event_for_queue->checksum_alg ==
                  mi->rli.relay_log.relay_log_checksum_alg);
      /* the first one */
      DBUG_ASSERT(mi->checksum_alg_before_fd != BINLOG_CHECKSUM_ALG_UNDEF);
      save_buf= (char *) buf;
      buf= rot_buf;
    }
    else
      /*
        RSC_2: If NM \and fake Rotate \and slave does not compute checksum
        the fake Rotate's checksum is stripped off before relay-logging.
      */
      if (uint4korr(&buf[0]) == 0 && checksum_alg != BINLOG_CHECKSUM_ALG_OFF &&
          mi->rli.relay_log.relay_log_checksum_alg == BINLOG_CHECKSUM_ALG_OFF)
      {
        event_len -= BINLOG_CHECKSUM_LEN;
        memcpy(rot_buf, buf, event_len);
        int4store(&rot_buf[EVENT_LEN_OFFSET],
                  uint4korr(&rot_buf[EVENT_LEN_OFFSET]) - BINLOG_CHECKSUM_LEN);
        DBUG_ASSERT(event_len == uint4korr(&rot_buf[EVENT_LEN_OFFSET]));
        DBUG_ASSERT(mi->rli.relay_log.description_event_for_queue->checksum_alg ==
                    mi->rli.relay_log.relay_log_checksum_alg);
        /* the first one */
        DBUG_ASSERT(mi->checksum_alg_before_fd != BINLOG_CHECKSUM_ALG_UNDEF);
        save_buf= (char *) buf;
        buf= rot_buf;
      }
    /*
      Now the I/O thread has just changed its mi->master_log_name, so
      incrementing mi->master_log_pos is nonsense.
    */
    inc_pos= 0;
    break;
  }
  case FORMAT_DESCRIPTION_EVENT:
  {
    /*
      Create an event, and save it (when we rotate the relay log, we will have
      to write this event again).
    */
    /*
      We are the only thread which reads/writes description_event_for_queue.
      The relay_log struct does not move (though some members of it can
      change), so we needn't any lock (no rli->data_lock, no log lock).
    */
    Format_description_log_event* tmp;
    const char* errmsg;
    // mark it as undefined that is irrelevant anymore
    mi->checksum_alg_before_fd= BINLOG_CHECKSUM_ALG_UNDEF;
    if (!(tmp= (Format_description_log_event*)
          Log_event::read_log_event(buf, event_len, &errmsg,
                                    mi->rli.relay_log.description_event_for_queue,
                                    1)))
    {
      error= ER_SLAVE_RELAY_LOG_WRITE_FAILURE;
      goto err;
    }
    tmp->copy_crypto_data(mi->rli.relay_log.description_event_for_queue);
    delete mi->rli.relay_log.description_event_for_queue;
    mi->rli.relay_log.description_event_for_queue= tmp;
    if (tmp->checksum_alg == BINLOG_CHECKSUM_ALG_UNDEF)
      tmp->checksum_alg= BINLOG_CHECKSUM_ALG_OFF;

    /* installing new value of checksum Alg for relay log */
    mi->rli.relay_log.relay_log_checksum_alg= tmp->checksum_alg;

    /*
      Do not queue any format description event that we receive after a
      reconnect where we are skipping over a partial event group received
      before the reconnect.

      (If we queued such an event, and it was the first format_description
      event after master restart, the slave SQL thread would think that
      the partial event group before it in the relay log was from a
      previous master crash and should be rolled back).
    */
    if (unlikely(mi->gtid_reconnect_event_skip_count && !mi->gtid_event_seen))
        gtid_skip_enqueue= true;

    /*
       Though this does some conversion to the slave's format, this will
       preserve the master's binlog format version, and number of event types.
    */
    /*
       If the event was not requested by the slave (the slave did not ask for
       it), i.e. has end_log_pos=0, we do not increment mi->master_log_pos
    */
    inc_pos= uint4korr(buf+LOG_POS_OFFSET) ? event_len : 0;
    DBUG_PRINT("info",("binlog format is now %d",
                       mi->rli.relay_log.description_event_for_queue->binlog_version));

  }
  break;

  case HEARTBEAT_LOG_EVENT:
  {
    /*
      HB (heartbeat) cannot come before RL (Relay)
    */
    Heartbeat_log_event hb(buf,
                           mi->rli.relay_log.relay_log_checksum_alg
                           != BINLOG_CHECKSUM_ALG_OFF ?
                           event_len - BINLOG_CHECKSUM_LEN : event_len,
                           mi->rli.relay_log.description_event_for_queue);
    if (!hb.is_valid())
    {
      error= ER_SLAVE_HEARTBEAT_FAILURE;
      error_msg.append(STRING_WITH_LEN("inconsistent heartbeat event content;"));
      error_msg.append(STRING_WITH_LEN("the event's data: log_file_name "));
      error_msg.append(hb.get_log_ident(), (uint) strlen(hb.get_log_ident()));
      error_msg.append(STRING_WITH_LEN(" log_pos "));
      error_msg.append_ulonglong(hb.log_pos);
      goto err;
    }
    mi->received_heartbeats++;
    /* 
       compare local and event's versions of log_file, log_pos.
       
       Heartbeat is sent only after an event corresponding to the corrdinates
       the heartbeat carries.
       Slave can not have a higher coordinate except in the only
       special case when mi->master_log_name, master_log_pos have never
       been updated by Rotate event i.e when slave does not have any history
       with the master (and thereafter mi->master_log_pos is NULL).

       Slave can have lower coordinates, if some event from master was omitted.

       TODO: handling `when' for SHOW SLAVE STATUS' snds behind
    */
    if ((memcmp(mi->master_log_name, hb.get_log_ident(), hb.get_ident_len())
         && mi->master_log_name != NULL)
        || mi->master_log_pos > hb.log_pos)
    {
      /* missed events of heartbeat from the past */
      error= ER_SLAVE_HEARTBEAT_FAILURE;
      error_msg.append(STRING_WITH_LEN("heartbeat is not compatible with local info;"));
      error_msg.append(STRING_WITH_LEN("the event's data: log_file_name "));
      error_msg.append(hb.get_log_ident(), (uint) strlen(hb.get_log_ident()));
      error_msg.append(STRING_WITH_LEN(" log_pos "));
      error_msg.append_ulonglong(hb.log_pos);
      goto err;
    }

    /*
      Heartbeat events doesn't count in the binlog size, so we don't have to
      increment mi->master_log_pos
    */
    goto skip_relay_logging;
  }
  break;

  case GTID_LIST_EVENT:
  {
    const char *errmsg;
    Gtid_list_log_event *glev;
    Log_event *tmp;
    uint32 flags;

    if (!(tmp= Log_event::read_log_event(buf, event_len, &errmsg,
           mi->rli.relay_log.description_event_for_queue,
           opt_slave_sql_verify_checksum)))
    {
      error= ER_SLAVE_RELAY_LOG_WRITE_FAILURE;
      goto err;
    }
    glev= static_cast<Gtid_list_log_event *>(tmp);
    event_pos= glev->log_pos;
    flags= glev->gl_flags;
    delete glev;

    /*
      We use fake Gtid_list events to update the old-style position (among
      other things).

      Early code created fake Gtid_list events with zero log_pos, those should
      not modify old-style position.
    */
    if (event_pos == 0 || event_pos <= mi->master_log_pos)
      inc_pos= 0;
    else
      inc_pos= event_pos - mi->master_log_pos;

    if (mi->rli.until_condition == Relay_log_info::UNTIL_GTID &&
        flags & Gtid_list_log_event::FLAG_UNTIL_REACHED)
    {
      char str_buf[128];
      String str(str_buf, sizeof(str_buf), system_charset_info);
      mi->rli.until_gtid_pos.to_string(&str);
      sql_print_information("Slave I/O thread stops because it reached its"
                            " UNTIL master_gtid_pos %s", str.c_ptr_safe());
      mi->abort_slave= true;
    }
  }
  break;

  case GTID_EVENT:
  {
    DBUG_EXECUTE_IF("kill_slave_io_after_2_events",
                    {
                      mi->dbug_do_disconnect= true;
                      mi->dbug_event_counter= 2;
                    };);

    uchar gtid_flag;

    if (Gtid_log_event::peek(buf, event_len, checksum_alg,
                             &event_gtid.domain_id, &event_gtid.server_id,
                             &event_gtid.seq_no, &gtid_flag,
                             rli->relay_log.description_event_for_queue))
    {
      error= ER_SLAVE_RELAY_LOG_WRITE_FAILURE;
      goto err;
    }
    got_gtid_event= true;
    if (mi->using_gtid == Master_info::USE_GTID_NO)
      goto default_action;
    if (unlikely(!mi->gtid_event_seen))
    {
      mi->gtid_event_seen= true;
      if (mi->gtid_reconnect_event_skip_count)
      {
        /*
          If we are reconnecting, and we need to skip a partial event group
          already queued to the relay log before the reconnect, then we check
          that we actually get the same event group (same GTID) as before, so
          we do not end up with half of one group and half another.

          The only way we should be able to receive a different GTID than what
          we expect is if the binlog on the master (or more likely the whole
          master server) was replaced with a different one, on the same IP
          address, _and_ the new master happens to have domains in a different
          order so we get the GTID from a different domain first. Still, it is
          best to protect against this case.
        */
        if (event_gtid.domain_id != mi->last_queued_gtid.domain_id ||
            event_gtid.server_id != mi->last_queued_gtid.server_id ||
            event_gtid.seq_no != mi->last_queued_gtid.seq_no)
        {
          bool first;
          error= ER_SLAVE_UNEXPECTED_MASTER_SWITCH;
          error_msg.append(STRING_WITH_LEN("Expected: "));
          first= true;
          rpl_slave_state_tostring_helper(&error_msg, &mi->last_queued_gtid,
                                          &first);
          error_msg.append(STRING_WITH_LEN(", received: "));
          first= true;
          rpl_slave_state_tostring_helper(&error_msg, &event_gtid, &first);
          goto err;
        }
      }
    }

    if (unlikely(mi->gtid_reconnect_event_skip_count))
    {
      goto default_action;
    }

    /*
      We have successfully queued to relay log everything before this GTID, so
      in case of reconnect we can start from after any previous GTID.
      (Normally we would have updated gtid_current_pos earlier at the end of
      the previous event group, but better leave an extra check here for
      safety).
    */
    if (mi->events_queued_since_last_gtid)
    {
      mi->gtid_current_pos.update(&mi->last_queued_gtid);
      mi->events_queued_since_last_gtid= 0;
    }
    mi->last_queued_gtid= event_gtid;
    mi->last_queued_gtid_standalone=
      (gtid_flag & Gtid_log_event::FL_STANDALONE) != 0;

    /* Should filter all the subsequent events in the current GTID group? */
    mi->domain_id_filter.do_filter(event_gtid.domain_id);

    ++mi->events_queued_since_last_gtid;
    inc_pos= event_len;
  }
  break;

#ifndef DBUG_OFF
  case XID_EVENT:
    DBUG_EXECUTE_IF("slave_discard_xid_for_gtid_0_x_1000",
    {
      /* Inject an event group that is missing its XID commit event. */
      if (mi->last_queued_gtid.domain_id == 0 &&
          mi->last_queued_gtid.seq_no == 1000)
        goto skip_relay_logging;
    });
    /* Fall through to default case ... */
#endif

  default:
  default_action:
    DBUG_EXECUTE_IF("kill_slave_io_after_2_events",
                    {
                      if (mi->dbug_do_disconnect &&
                          (((uchar)buf[EVENT_TYPE_OFFSET] == QUERY_EVENT) ||
                           ((uchar)buf[EVENT_TYPE_OFFSET] == TABLE_MAP_EVENT))
                          && (--mi->dbug_event_counter == 0))
                      {
                        error= ER_SLAVE_RELAY_LOG_WRITE_FAILURE;
                        mi->dbug_do_disconnect= false;  /* Safety */
                        goto err;
                      }
                    };);

    DBUG_EXECUTE_IF("kill_slave_io_before_commit",
                    {
                      if ((uchar)buf[EVENT_TYPE_OFFSET] == XID_EVENT ||
                          ((uchar)buf[EVENT_TYPE_OFFSET] == QUERY_EVENT &&
                           Query_log_event::peek_is_commit_rollback(buf, event_len,
                                                                    checksum_alg)))
                      {
                        error= ER_SLAVE_RELAY_LOG_WRITE_FAILURE;
                        goto err;
                      }
                    };);

    if (mi->using_gtid != Master_info::USE_GTID_NO && mi->gtid_event_seen)
    {
      if (unlikely(mi->gtid_reconnect_event_skip_count))
      {
        --mi->gtid_reconnect_event_skip_count;
        gtid_skip_enqueue= true;
      }
      else if (mi->events_queued_since_last_gtid)
        ++mi->events_queued_since_last_gtid;
    }

    inc_pos= event_len;
    break;
  }

  /*
    If we filter events master-side (eg. @@skip_replication), we will see holes
    in the event positions from the master. If we see such a hole, adjust
    mi->master_log_pos accordingly so we maintain the correct position (for
    reconnect, MASTER_POS_WAIT(), etc.)
  */
  if (inc_pos > 0 &&
      event_len >= LOG_POS_OFFSET+4 &&
      (event_pos= uint4korr(buf+LOG_POS_OFFSET)) > mi->master_log_pos + inc_pos)
  {
    inc_pos= event_pos - mi->master_log_pos;
    DBUG_PRINT("info", ("Adjust master_log_pos %llu->%llu to account for "
                        "master-side filtering",
                        mi->master_log_pos + inc_pos, event_pos));
  }

  /*
     If this event is originating from this server, don't queue it.
     We don't check this for 3.23 events because it's simpler like this; 3.23
     will be filtered anyway by the SQL slave thread which also tests the
     server id (we must also keep this test in the SQL thread, in case somebody
     upgrades a 4.0 slave which has a not-filtered relay log).

     ANY event coming from ourselves can be ignored: it is obvious for queries;
     for STOP_EVENT/ROTATE_EVENT/START_EVENT: these cannot come from ourselves
     (--log-slave-updates would not log that) unless this slave is also its
     direct master (an unsupported, useless setup!).
  */

  mysql_mutex_lock(log_lock);
  s_id= uint4korr(buf + SERVER_ID_OFFSET);
  /*
    Write the event to the relay log, unless we reconnected in the middle
    of an event group and now need to skip the initial part of the group that
    we already wrote before reconnecting.
  */
  if (unlikely(gtid_skip_enqueue))
  {
    mi->master_log_pos+= inc_pos;
    if ((uchar)buf[EVENT_TYPE_OFFSET] == FORMAT_DESCRIPTION_EVENT &&
        s_id == mi->master_id)
    {
      /*
        If we write this master's description event in the middle of an event
        group due to GTID reconnect, SQL thread will think that master crashed
        in the middle of the group and roll back the first half, so we must not.

        But we still have to write an artificial copy of the masters description
        event, to override the initial slave-version description event so that
        SQL thread has the right information for parsing the events it reads.
      */
      rli->relay_log.description_event_for_queue->created= 0;
      rli->relay_log.description_event_for_queue->set_artificial_event();
      if (rli->relay_log.append_no_lock
          (rli->relay_log.description_event_for_queue))
        error= ER_SLAVE_RELAY_LOG_WRITE_FAILURE;
      else
        rli->relay_log.harvest_bytes_written(&rli->log_space_total);
    }
    else if (mi->gtid_reconnect_event_skip_count == 0)
    {
      /*
        Add a fake rotate event so that SQL thread can see the old-style
        position where we re-connected in the middle of a GTID event group.
      */
      Rotate_log_event fake_rev(mi->master_log_name, 0, mi->master_log_pos, 0);
      fake_rev.server_id= mi->master_id;
      if (rli->relay_log.append_no_lock(&fake_rev))
        error= ER_SLAVE_RELAY_LOG_WRITE_FAILURE;
      else
        rli->relay_log.harvest_bytes_written(&rli->log_space_total);
    }
  }
  else
  if ((s_id == global_system_variables.server_id &&
       !mi->rli.replicate_same_server_id) ||
      event_that_should_be_ignored(buf) ||
      /*
        the following conjunction deals with IGNORE_SERVER_IDS, if set
        If the master is on the ignore list, execution of
        format description log events and rotate events is necessary.
      */
      (mi->ignore_server_ids.elements > 0 &&
       mi->shall_ignore_server_id(s_id) &&
       /* everything is filtered out from non-master */
       (s_id != mi->master_id ||
        /* for the master meta information is necessary */
        (buf[EVENT_TYPE_OFFSET] != FORMAT_DESCRIPTION_EVENT &&
         buf[EVENT_TYPE_OFFSET] != ROTATE_EVENT))) ||

      /*
        Check whether it needs to be filtered based on domain_id
        (DO_DOMAIN_IDS/IGNORE_DOMAIN_IDS).
      */
      (mi->domain_id_filter.is_group_filtered() &&
       Log_event::is_group_event((Log_event_type)(uchar)
                                 buf[EVENT_TYPE_OFFSET])))
  {
    /*
      Do not write it to the relay log.
      a) We still want to increment mi->master_log_pos, so that we won't
      re-read this event from the master if the slave IO thread is now
      stopped/restarted (more efficient if the events we are ignoring are big
      LOAD DATA INFILE).
      b) We want to record that we are skipping events, for the information of
      the slave SQL thread, otherwise that thread may let
      rli->group_relay_log_pos stay too small if the last binlog's event is
      ignored.
      But events which were generated by this slave and which do not exist in
      the master's binlog (i.e. Format_desc, Rotate & Stop) should not increment
      mi->master_log_pos.
      If the event is originated remotely and is being filtered out by
      IGNORE_SERVER_IDS it increments mi->master_log_pos
      as well as rli->group_relay_log_pos.
    */
    if (!(s_id == global_system_variables.server_id &&
          !mi->rli.replicate_same_server_id) ||
        (buf[EVENT_TYPE_OFFSET] != FORMAT_DESCRIPTION_EVENT &&
         buf[EVENT_TYPE_OFFSET] != ROTATE_EVENT &&
         buf[EVENT_TYPE_OFFSET] != STOP_EVENT))
    {
      mi->master_log_pos+= inc_pos;
      memcpy(rli->ign_master_log_name_end, mi->master_log_name, FN_REFLEN);
      DBUG_ASSERT(rli->ign_master_log_name_end[0]);
      rli->ign_master_log_pos_end= mi->master_log_pos;
      if (got_gtid_event)
        rli->ign_gtids.update(&event_gtid);
    }
    rli->relay_log.signal_update(); // the slave SQL thread needs to re-check
    DBUG_PRINT("info", ("master_log_pos: %lu, event originating from %u server, ignored",
                        (ulong) mi->master_log_pos, uint4korr(buf + SERVER_ID_OFFSET)));
  }
  else
  {
    if (likely(!rli->relay_log.write_event_buffer((uchar*)buf, event_len)))
    {
      mi->master_log_pos+= inc_pos;
      DBUG_PRINT("info", ("master_log_pos: %lu", (ulong) mi->master_log_pos));
      rli->relay_log.harvest_bytes_written(&rli->log_space_total);
    }
    else
    {
      error= ER_SLAVE_RELAY_LOG_WRITE_FAILURE;
    }
    rli->ign_master_log_name_end[0]= 0; // last event is not ignored
    if (got_gtid_event)
      rli->ign_gtids.remove_if_present(&event_gtid);
    if (save_buf != NULL)
      buf= save_buf;
  }
  mysql_mutex_unlock(log_lock);

  if (!error &&
      mi->using_gtid != Master_info::USE_GTID_NO &&
      mi->events_queued_since_last_gtid > 0 &&
      ( (mi->last_queued_gtid_standalone &&
         !Log_event::is_part_of_group((Log_event_type)(uchar)
                                      buf[EVENT_TYPE_OFFSET])) ||
        (!mi->last_queued_gtid_standalone &&
         ((uchar)buf[EVENT_TYPE_OFFSET] == XID_EVENT ||
          ((uchar)buf[EVENT_TYPE_OFFSET] == QUERY_EVENT &&
           Query_log_event::peek_is_commit_rollback(buf, event_len,
                                                    checksum_alg))))))
    {
      /*
        The whole of the current event group is queued. So in case of
        reconnect we can start from after the current GTID.
      */
      mi->gtid_current_pos.update(&mi->last_queued_gtid);
      mi->events_queued_since_last_gtid= 0;

      /* Reset the domain_id_filter flag. */
      mi->domain_id_filter.reset_filter();
    }

skip_relay_logging:

err:
  if (unlock_data_lock)
    mysql_mutex_unlock(&mi->data_lock);
  DBUG_PRINT("info", ("error: %d", error));

  /*
    Do not print ER_SLAVE_RELAY_LOG_WRITE_FAILURE error here, as the caller
    handle_slave_io() prints it on return.
  */
  if (error && error != ER_SLAVE_RELAY_LOG_WRITE_FAILURE)
    mi->report(ERROR_LEVEL, error, NULL, ER_DEFAULT(error),
               error_msg.ptr());

  DBUG_RETURN(error);
}


void end_relay_log_info(Relay_log_info* rli)
{
  DBUG_ENTER("end_relay_log_info");

  if (!rli->inited)
    DBUG_VOID_RETURN;
  if (rli->info_fd >= 0)
  {
    end_io_cache(&rli->info_file);
    mysql_file_close(rli->info_fd, MYF(MY_WME));
    rli->info_fd = -1;
  }
  if (rli->cur_log_fd >= 0)
  {
    end_io_cache(&rli->cache_buf);
    mysql_file_close(rli->cur_log_fd, MYF(MY_WME));
    rli->cur_log_fd = -1;
  }
  rli->inited = 0;
  rli->relay_log.close(LOG_CLOSE_INDEX | LOG_CLOSE_STOP_EVENT);
  rli->relay_log.harvest_bytes_written(&rli->log_space_total);
  /*
    Delete the slave's temporary tables from memory.
    In the future there will be other actions than this, to ensure persistance
    of slave's temp tables after shutdown.
  */
  rli->close_temporary_tables();
  DBUG_VOID_RETURN;
}


/**
  Hook to detach the active VIO before closing a connection handle.

  The client API might close the connection (and associated data)
  in case it encounters a unrecoverable (network) error. This hook
  is called from the client code before the VIO handle is deleted
  allows the thread to detach the active vio so it does not point
  to freed memory.

  Other calls to THD::clear_active_vio throughout this module are
  redundant due to the hook but are left in place for illustrative
  purposes.
*/

extern "C" void slave_io_thread_detach_vio()
{
#ifdef SIGNAL_WITH_VIO_CLOSE
  THD *thd= current_thd;
  if (thd && thd->slave_thread)
    thd->clear_active_vio();
#endif
}


/*
  Try to connect until successful or slave killed

  SYNPOSIS
    safe_connect()
    thd                 Thread handler for slave
    mysql               MySQL connection handle
    mi                  Replication handle

  RETURN
    0   ok
    #   Error
*/

static int safe_connect(THD* thd, MYSQL* mysql, Master_info* mi)
{
  DBUG_ENTER("safe_connect");

  DBUG_RETURN(connect_to_master(thd, mysql, mi, 0, 0));
}


/*
  SYNPOSIS
    connect_to_master()

  IMPLEMENTATION
    Try to connect until successful or slave killed or we have retried
    master_retry_count times
*/

static int connect_to_master(THD* thd, MYSQL* mysql, Master_info* mi,
                             bool reconnect, bool suppress_warnings)
{
  int slave_was_killed;
  int last_errno= -2;                           // impossible error
  ulong err_count=0;
  my_bool my_true= 1;
  DBUG_ENTER("connect_to_master");
  set_slave_max_allowed_packet(thd, mysql);
#ifndef DBUG_OFF
  mi->events_till_disconnect = disconnect_slave_event_count;
#endif
  ulong client_flag= 0;
  if (opt_slave_compressed_protocol)
    client_flag=CLIENT_COMPRESS;                /* We will use compression */

  mysql_options(mysql, MYSQL_OPT_CONNECT_TIMEOUT, (char *) &slave_net_timeout);
  mysql_options(mysql, MYSQL_OPT_READ_TIMEOUT, (char *) &slave_net_timeout);
  mysql_options(mysql, MYSQL_OPT_USE_THREAD_SPECIFIC_MEMORY,
                (char*) &my_true);

#ifdef HAVE_OPENSSL
  if (mi->ssl)
  {
    mysql_ssl_set(mysql,
                  mi->ssl_key[0]?mi->ssl_key:0,
                  mi->ssl_cert[0]?mi->ssl_cert:0,
                  mi->ssl_ca[0]?mi->ssl_ca:0,
                  mi->ssl_capath[0]?mi->ssl_capath:0,
                  mi->ssl_cipher[0]?mi->ssl_cipher:0);
    mysql_options(mysql, MYSQL_OPT_SSL_VERIFY_SERVER_CERT,
                  &mi->ssl_verify_server_cert);
    mysql_options(mysql, MYSQL_OPT_SSL_CRLPATH, 
                  mi->ssl_crlpath[0] ? mi->ssl_crlpath : 0);
    mysql_options(mysql, MYSQL_OPT_SSL_VERIFY_SERVER_CERT,
                  &mi->ssl_verify_server_cert);
  }
#endif

  /*
    If server's default charset is not supported (like utf16, utf32) as client
    charset, then set client charset to 'latin1' (default client charset).
  */
  if (is_supported_parser_charset(default_charset_info))
    mysql_options(mysql, MYSQL_SET_CHARSET_NAME, default_charset_info->csname);
  else
  {
    sql_print_information("'%s' can not be used as client character set. "
                          "'%s' will be used as default client character set "
                          "while connecting to master.",
                          default_charset_info->csname,
                          default_client_charset_info->csname);
    mysql_options(mysql, MYSQL_SET_CHARSET_NAME,
                  default_client_charset_info->csname);
  }

  /* This one is not strictly needed but we have it here for completeness */
  mysql_options(mysql, MYSQL_SET_CHARSET_DIR, (char *) charsets_dir);

  /* Set MYSQL_PLUGIN_DIR in case master asks for an external authentication plugin */
  if (opt_plugin_dir_ptr && *opt_plugin_dir_ptr)
    mysql_options(mysql, MYSQL_PLUGIN_DIR, opt_plugin_dir_ptr);

  /* we disallow empty users */
  if (mi->user == NULL || mi->user[0] == 0)
  {
    mi->report(ERROR_LEVEL, ER_SLAVE_FATAL_ERROR, NULL,
               ER_THD(thd, ER_SLAVE_FATAL_ERROR),
               "Invalid (empty) username when attempting to "
               "connect to the master server. Connection attempt "
               "terminated.");
    DBUG_RETURN(1);
  }
  while (!(slave_was_killed = io_slave_killed(mi)) &&
         (reconnect ? mysql_reconnect(mysql) != 0 :
          mysql_real_connect(mysql, mi->host, mi->user, mi->password, 0,
                             mi->port, 0, client_flag) == 0))
  {
    /* Don't repeat last error */
    if ((int)mysql_errno(mysql) != last_errno)
    {
      last_errno=mysql_errno(mysql);
      suppress_warnings= 0;
      mi->report(ERROR_LEVEL, last_errno, NULL,
                 "error %s to master '%s@%s:%d'"
                 " - retry-time: %d  retries: %lu  message: %s",
                 (reconnect ? "reconnecting" : "connecting"),
                 mi->user, mi->host, mi->port,
                 mi->connect_retry, master_retry_count,
                 mysql_error(mysql));
    }
    /*
      By default we try forever. The reason is that failure will trigger
      master election, so if the user did not set master_retry_count we
      do not want to have election triggered on the first failure to
      connect
    */
    if (++err_count == master_retry_count)
    {
      slave_was_killed=1;
      if (reconnect)
        change_rpl_status(RPL_ACTIVE_SLAVE,RPL_LOST_SOLDIER);
      break;
    }
    slave_sleep(thd,mi->connect_retry,io_slave_killed, mi);
  }

  if (!slave_was_killed)
  {
    mi->clear_error(); // clear possible left over reconnect error
    if (reconnect)
    {
      if (!suppress_warnings && global_system_variables.log_warnings)
        sql_print_information("Slave: connected to master '%s@%s:%d',"
                              "replication resumed in log '%s' at "
                              "position %llu", mi->user, mi->host, mi->port,
                              IO_RPL_LOG_NAME, mi->master_log_pos);
    }
    else
    {
      change_rpl_status(RPL_IDLE_SLAVE,RPL_ACTIVE_SLAVE);
      general_log_print(thd, COM_CONNECT_OUT, "%s@%s:%d",
                        mi->user, mi->host, mi->port);
    }
#ifdef SIGNAL_WITH_VIO_CLOSE
    thd->set_active_vio(mysql->net.vio);
#endif
  }
  mysql->reconnect= 1;
  DBUG_PRINT("exit",("slave_was_killed: %d", slave_was_killed));
  DBUG_RETURN(slave_was_killed);
}


/*
  safe_reconnect()

  IMPLEMENTATION
    Try to connect until successful or slave killed or we have retried
    master_retry_count times
*/

static int safe_reconnect(THD* thd, MYSQL* mysql, Master_info* mi,
                          bool suppress_warnings)
{
  DBUG_ENTER("safe_reconnect");
  DBUG_RETURN(connect_to_master(thd, mysql, mi, 1, suppress_warnings));
}


#ifdef NOT_USED
MYSQL *rpl_connect_master(MYSQL *mysql)
{
  Master_info *mi= my_pthread_getspecific_ptr(Master_info*, RPL_MASTER_INFO);
  bool allocated= false;
  my_bool my_true= 1;
  THD *thd;

  if (!mi)
  {
    sql_print_error("'rpl_connect_master' must be called in slave I/O thread context.");
    return NULL;
  }
  thd= mi->io_thd;
  if (!mysql)
  {
    if(!(mysql= mysql_init(NULL)))
    {
      sql_print_error("rpl_connect_master: failed in mysql_init()");
      return NULL;
    }
    allocated= true;
  }

  /*
    XXX: copied from connect_to_master, this function should not
    change the slave status, so we cannot use connect_to_master
    directly
    
    TODO: make this part a seperate function to eliminate duplication
  */
  mysql_options(mysql, MYSQL_OPT_CONNECT_TIMEOUT, (char *) &slave_net_timeout);
  mysql_options(mysql, MYSQL_OPT_READ_TIMEOUT, (char *) &slave_net_timeout);
  mysql_options(mysql, MYSQL_OPT_USE_THREAD_SPECIFIC_MEMORY,
                (char*) &my_true);

#ifdef HAVE_OPENSSL
  if (mi->ssl)
  {
    mysql_ssl_set(mysql,
                  mi->ssl_key[0]?mi->ssl_key:0,
                  mi->ssl_cert[0]?mi->ssl_cert:0,
                  mi->ssl_ca[0]?mi->ssl_ca:0,
                  mi->ssl_capath[0]?mi->ssl_capath:0,
                  mi->ssl_cipher[0]?mi->ssl_cipher:0);
    mysql_options(mysql, MYSQL_OPT_SSL_VERIFY_SERVER_CERT,
                  &mi->ssl_verify_server_cert);
  }
#endif

  mysql_options(mysql, MYSQL_SET_CHARSET_NAME, default_charset_info->csname);
  /* This one is not strictly needed but we have it here for completeness */
  mysql_options(mysql, MYSQL_SET_CHARSET_DIR, (char *) charsets_dir);

  if (mi->user == NULL
      || mi->user[0] == 0
      || io_slave_killed( mi)
      || !mysql_real_connect(mysql, mi->host, mi->user, mi->password, 0,
                             mi->port, 0, 0))
  {
    if (!io_slave_killed( mi))
      sql_print_error("rpl_connect_master: error connecting to master: %s (server_error: %d)",
                      mysql_error(mysql), mysql_errno(mysql));
    
    if (allocated)
      mysql_close(mysql);                       // this will free the object
    return NULL;
  }
  return mysql;
}
#endif

/*
  Store the file and position where the execute-slave thread are in the
  relay log.

  SYNOPSIS
    flush_relay_log_info()
    rli                 Relay log information

  NOTES
    - As this is only called by the slave thread or on STOP SLAVE, with the
      log_lock grabbed and the slave thread stopped, we don't need to have 
      a lock here.
    - If there is an active transaction, then we don't update the position
      in the relay log.  This is to ensure that we re-execute statements
      if we die in the middle of an transaction that was rolled back.
    - As a transaction never spans binary logs, we don't have to handle the
      case where we do a relay-log-rotation in the middle of the transaction.
      If this would not be the case, we would have to ensure that we
      don't delete the relay log file where the transaction started when
      we switch to a new relay log file.

  TODO
    - Change the log file information to a binary format to avoid calling
      longlong2str.

  RETURN VALUES
    0   ok
    1   write error
*/

bool flush_relay_log_info(Relay_log_info* rli)
{
  bool error=0;
  DBUG_ENTER("flush_relay_log_info");

  if (unlikely(rli->no_storage))
    DBUG_RETURN(0);

  IO_CACHE *file = &rli->info_file;
  char buff[FN_REFLEN*2+22*2+4], *pos;

  my_b_seek(file, 0L);
  pos=strmov(buff, rli->group_relay_log_name);
  *pos++='\n';
  pos= longlong10_to_str(rli->group_relay_log_pos, pos, 10);
  *pos++='\n';
  pos=strmov(pos, rli->group_master_log_name);
  *pos++='\n';
  pos=longlong10_to_str(rli->group_master_log_pos, pos, 10);
  *pos='\n';
  if (my_b_write(file, (uchar*) buff, (size_t) (pos-buff)+1))
    error=1;
  if (flush_io_cache(file))
    error=1;
  if (sync_relayloginfo_period &&
      !error &&
      ++(rli->sync_counter) >= sync_relayloginfo_period)
  {
    if (my_sync(rli->info_fd, MYF(MY_WME)))
      error=1;
    rli->sync_counter= 0;
  }
  /* 
    Flushing the relay log is done by the slave I/O thread 
    or by the user on STOP SLAVE. 
   */
  DBUG_RETURN(error);
}


/*
  Called when we notice that the current "hot" log got rotated under our feet.
*/

static IO_CACHE *reopen_relay_log(Relay_log_info *rli, const char **errmsg)
{
  DBUG_ENTER("reopen_relay_log");
  DBUG_ASSERT(rli->cur_log != &rli->cache_buf);
  DBUG_ASSERT(rli->cur_log_fd == -1);

  IO_CACHE *cur_log = rli->cur_log=&rli->cache_buf;
  if ((rli->cur_log_fd=open_binlog(cur_log,rli->event_relay_log_name,
                                   errmsg)) <0)
    DBUG_RETURN(0);
  /*
    We want to start exactly where we was before:
    relay_log_pos       Current log pos
    pending             Number of bytes already processed from the event
  */
  rli->event_relay_log_pos= MY_MAX(rli->event_relay_log_pos, BIN_LOG_HEADER_SIZE);
  my_b_seek(cur_log,rli->event_relay_log_pos);
  DBUG_RETURN(cur_log);
}


/**
  Reads next event from the relay log.  Should be called from the
  slave IO thread.

  @param rli Relay_log_info structure for the slave IO thread.

  @return The event read, or NULL on error.  If an error occurs, the
  error is reported through the sql_print_information() or
  sql_print_error() functions.

  The size of the read event (in bytes) is returned in *event_size.
*/
static Log_event* next_event(rpl_group_info *rgi, ulonglong *event_size)
{
  Log_event* ev;
  Relay_log_info *rli= rgi->rli;
  IO_CACHE* cur_log = rli->cur_log;
  mysql_mutex_t *log_lock = rli->relay_log.get_log_lock();
  const char* errmsg=0;
  DBUG_ENTER("next_event");

  DBUG_ASSERT(rgi->thd != 0 && rgi->thd == rli->sql_driver_thd);
  *event_size= 0;

#ifndef DBUG_OFF
  if (abort_slave_event_count && !rli->events_till_abort--)
    DBUG_RETURN(0);
#endif

  /*
    For most operations we need to protect rli members with data_lock,
    so we assume calling function acquired this mutex for us and we will
    hold it for the most of the loop below However, we will release it
    whenever it is worth the hassle,  and in the cases when we go into a
    mysql_cond_wait() with the non-data_lock mutex
  */
  mysql_mutex_assert_owner(&rli->data_lock);

  while (!sql_slave_killed(rgi))
  {
    /*
      We can have two kinds of log reading:
      hot_log:
        rli->cur_log points at the IO_CACHE of relay_log, which
        is actively being updated by the I/O thread. We need to be careful
        in this case and make sure that we are not looking at a stale log that
        has already been rotated. If it has been, we reopen the log.

      The other case is much simpler:
        We just have a read only log that nobody else will be updating.
    */
    ulonglong old_pos;
    bool hot_log;
    if ((hot_log = (cur_log != &rli->cache_buf)))
    {
      DBUG_ASSERT(rli->cur_log_fd == -1); // foreign descriptor
      mysql_mutex_lock(log_lock);

      /*
        Reading xxx_file_id is safe because the log will only
        be rotated when we hold relay_log.LOCK_log
      */
      if (rli->relay_log.get_open_count() != rli->cur_log_old_open_count)
      {
        // The master has switched to a new log file; Reopen the old log file
        cur_log=reopen_relay_log(rli, &errmsg);
        mysql_mutex_unlock(log_lock);
        if (!cur_log)                           // No more log files
          goto err;
        hot_log=0;                              // Using old binary log
      }
    }
    /* 
      As there is no guarantee that the relay is open (for example, an I/O
      error during a write by the slave I/O thread may have closed it), we
      have to test it.
    */
    if (!my_b_inited(cur_log))
      goto err;
#ifndef DBUG_OFF
    {
      /* This is an assertion which sometimes fails, let's try to track it */
      DBUG_PRINT("info", ("my_b_tell(cur_log)=%llu rli->event_relay_log_pos=%llu",
                          my_b_tell(cur_log), rli->event_relay_log_pos));
      DBUG_ASSERT(my_b_tell(cur_log) >= BIN_LOG_HEADER_SIZE);
      DBUG_ASSERT(rli->mi->using_parallel() ||
                  my_b_tell(cur_log) == rli->event_relay_log_pos);
    }
#endif
    /*
      Relay log is always in new format - if the master is 3.23, the
      I/O thread will convert the format for us.
      A problem: the description event may be in a previous relay log. So if
      the slave has been shutdown meanwhile, we would have to look in old relay
      logs, which may even have been deleted. So we need to write this
      description event at the beginning of the relay log.
      When the relay log is created when the I/O thread starts, easy: the
      master will send the description event and we will queue it.
      But if the relay log is created by new_file(): then the solution is:
      MYSQL_BIN_LOG::open() will write the buffered description event.
    */
    old_pos= rli->event_relay_log_pos;
    if ((ev= Log_event::read_log_event(cur_log,0,
                                       rli->relay_log.description_event_for_exec,
                                       opt_slave_sql_verify_checksum)))

    {
      /*
        read it while we have a lock, to avoid a mutex lock in
        inc_event_relay_log_pos()
      */
      rli->future_event_relay_log_pos= my_b_tell(cur_log);
      *event_size= rli->future_event_relay_log_pos - old_pos;

      if (hot_log)
        mysql_mutex_unlock(log_lock);
      rli->sql_thread_caught_up= false;
      DBUG_RETURN(ev);
    }
    if (opt_reckless_slave)                     // For mysql-test
      cur_log->error = 0;
    if (cur_log->error < 0)
    {
      errmsg = "slave SQL thread aborted because of I/O error";
      if (hot_log)
        mysql_mutex_unlock(log_lock);
      goto err;
    }
    if (!cur_log->error) /* EOF */
    {
      /*
        On a hot log, EOF means that there are no more updates to
        process and we must block until I/O thread adds some and
        signals us to continue
      */
      if (hot_log)
      {
        /*
          We say in Seconds_Behind_Master that we have "caught up". Note that
          for example if network link is broken but I/O slave thread hasn't
          noticed it (slave_net_timeout not elapsed), then we'll say "caught
          up" whereas we're not really caught up. Fixing that would require
          internally cutting timeout in smaller pieces in network read, no
          thanks. Another example: SQL has caught up on I/O, now I/O has read
          a new event and is queuing it; the false "0" will exist until SQL
          finishes executing the new event; it will be look abnormal only if
          the events have old timestamps (then you get "many", 0, "many").

          Transient phases like this can be fixed with implemeting
          Heartbeat event which provides the slave the status of the
          master at time the master does not have any new update to send.
          Seconds_Behind_Master would be zero only when master has no
          more updates in binlog for slave. The heartbeat can be sent
          in a (small) fraction of slave_net_timeout. Until it's done
          rli->sql_thread_caught_up is temporarely (for time of waiting for
          the following event) set whenever EOF is reached.
        */
        rli->sql_thread_caught_up= true;

        DBUG_ASSERT(rli->relay_log.get_open_count() ==
                    rli->cur_log_old_open_count);

        if (rli->ign_master_log_name_end[0])
        {
          /* We generate and return a Rotate, to make our positions advance */
          DBUG_PRINT("info",("seeing an ignored end segment"));
          ev= new Rotate_log_event(rli->ign_master_log_name_end,
                                   0, rli->ign_master_log_pos_end,
                                   Rotate_log_event::DUP_NAME);
          rli->ign_master_log_name_end[0]= 0;
          mysql_mutex_unlock(log_lock);
          if (unlikely(!ev))
          {
            errmsg= "Slave SQL thread failed to create a Rotate event "
              "(out of memory?), SHOW SLAVE STATUS may be inaccurate";
            goto err;
          }
          ev->server_id= 0; // don't be ignored by slave SQL thread
          DBUG_RETURN(ev);
        }

        if (rli->ign_gtids.count())
        {
          /* We generate and return a Gtid_list, to update gtid_slave_pos. */
          DBUG_PRINT("info",("seeing ignored end gtids"));
          ev= new Gtid_list_log_event(&rli->ign_gtids,
                                      Gtid_list_log_event::FLAG_IGN_GTIDS);
          rli->ign_gtids.reset();
          mysql_mutex_unlock(log_lock);
          if (unlikely(!ev))
          {
            errmsg= "Slave SQL thread failed to create a Gtid_list event "
              "(out of memory?), gtid_slave_pos may be inaccurate";
            goto err;
          }
          ev->server_id= 0; // don't be ignored by slave SQL thread
          ev->set_artificial_event(); // Don't mess up Exec_Master_Log_Pos
          DBUG_RETURN(ev);
        }

        /*
          We have to check sql_slave_killed() here an extra time.
          Otherwise we may miss a wakeup, since last check was done
          without holding LOCK_log.
        */
        if (sql_slave_killed(rgi))
        {
          mysql_mutex_unlock(log_lock);
          break;
        }

        /*
          We can, and should release data_lock while we are waiting for
          update. If we do not, show slave status will block
        */
        mysql_mutex_unlock(&rli->data_lock);

        /*
          Possible deadlock :
          - the I/O thread has reached log_space_limit
          - the SQL thread has read all relay logs, but cannot purge for some
          reason:
            * it has already purged all logs except the current one
            * there are other logs than the current one but they're involved in
            a transaction that finishes in the current one (or is not finished)
          Solution :
          Wake up the possibly waiting I/O thread, and set a boolean asking
          the I/O thread to temporarily ignore the log_space_limit
          constraint, because we do not want the I/O thread to block because of
          space (it's ok if it blocks for any other reason (e.g. because the
          master does not send anything). Then the I/O thread stops waiting
          and reads one more event and starts honoring log_space_limit again.

          If the SQL thread needs more events to be able to rotate the log (it
          might need to finish the current group first), then it can ask for
          one more at a time. Thus we don't outgrow the relay log indefinitely,
          but rather in a controlled manner, until the next rotate.

          When the SQL thread starts it sets ignore_log_space_limit to false. 
          We should also reset ignore_log_space_limit to 0 when the user does 
          RESET SLAVE, but in fact, no need as RESET SLAVE requires that the
          slave be stopped, and the SQL thread sets ignore_log_space_limit
          to 0 when
          it stops.
        */
        mysql_mutex_lock(&rli->log_space_lock);

        /* 
          If we have reached the limit of the relay space and we
          are going to sleep, waiting for more events:

          1. If outside a group, SQL thread asks the IO thread 
             to force a rotation so that the SQL thread purges 
             logs next time it processes an event (thus space is
             freed).

          2. If in a group, SQL thread asks the IO thread to 
             ignore the limit and queues yet one more event 
             so that the SQL thread finishes the group and 
             is are able to rotate and purge sometime soon.
         */
        if (rli->log_space_limit && 
            rli->log_space_limit < rli->log_space_total)
        {
          /* force rotation if not in an unfinished group */
          rli->sql_force_rotate_relay= !rli->is_in_group();

          /* ask for one more event */
          rli->ignore_log_space_limit= true;
        }

        mysql_cond_broadcast(&rli->log_space_cond);
        mysql_mutex_unlock(&rli->log_space_lock);
        // Note that wait_for_update_relay_log unlocks lock_log !
        rli->relay_log.wait_for_update_relay_log(rli->sql_driver_thd);
        // re-acquire data lock since we released it earlier
        mysql_mutex_lock(&rli->data_lock);
        rli->sql_thread_caught_up= false;
        continue;
      }
      /*
        If the log was not hot, we need to move to the next log in
        sequence. The next log could be hot or cold, we deal with both
        cases separately after doing some common initialization
      */
      end_io_cache(cur_log);
      DBUG_ASSERT(rli->cur_log_fd >= 0);
      mysql_file_close(rli->cur_log_fd, MYF(MY_WME));
      rli->cur_log_fd = -1;
      rli->last_inuse_relaylog->completed= true;
      rli->relay_log.description_event_for_exec->reset_crypto();

      if (relay_log_purge)
      {
        /*
          purge_first_log will properly set up relay log coordinates in rli.
          If the group's coordinates are equal to the event's coordinates
          (i.e. the relay log was not rotated in the middle of a group),
          we can purge this relay log too.
          We do ulonglong and string comparisons, this may be slow but
          - purging the last relay log is nice (it can save 1GB of disk), so we
          like to detect the case where we can do it, and given this,
          - I see no better detection method
          - purge_first_log is not called that often
        */
        if (rli->relay_log.purge_first_log
            (rli,
             rli->group_relay_log_pos == rli->event_relay_log_pos
             && !strcmp(rli->group_relay_log_name,rli->event_relay_log_name)))
        {
          errmsg = "Error purging processed logs";
          goto err;
        }
      }
      else
      {
        /*
          If hot_log is set, then we already have a lock on
          LOCK_log.  If not, we have to get the lock.

          According to Sasha, the only time this code will ever be executed
          is if we are recovering from a bug.
        */
        if (rli->relay_log.find_next_log(&rli->linfo, !hot_log))
        {
          errmsg = "error switching to the next log";
          goto err;
        }
        rli->event_relay_log_pos = BIN_LOG_HEADER_SIZE;
        strmake_buf(rli->event_relay_log_name,rli->linfo.log_file_name);
        flush_relay_log_info(rli);
      }

      /*
        Now we want to open this next log. To know if it's a hot log (the one
        being written by the I/O thread now) or a cold log, we can use
        is_active(); if it is hot, we use the I/O cache; if it's cold we open
        the file normally. But if is_active() reports that the log is hot, this
        may change between the test and the consequence of the test. So we may
        open the I/O cache whereas the log is now cold, which is nonsense.
        To guard against this, we need to have LOCK_log.
      */

      DBUG_PRINT("info",("hot_log: %d",hot_log));
      if (!hot_log) /* if hot_log, we already have this mutex */
        mysql_mutex_lock(log_lock);
      if (rli->relay_log.is_active(rli->linfo.log_file_name))
      {
        rli->cur_log= cur_log= rli->relay_log.get_log_file();
        rli->cur_log_old_open_count= rli->relay_log.get_open_count();
        DBUG_ASSERT(rli->cur_log_fd == -1);

        /*
           When the SQL thread is [stopped and] (re)started the
           following may happen:

           1. Log was hot at stop time and remains hot at restart

              SQL thread reads again from hot_log (SQL thread was
              reading from the active log when it was stopped and the
              very same log is still active on SQL thread restart).

              In this case, my_b_seek is performed on cur_log, while
              cur_log points to relay_log.get_log_file();

           2. Log was hot at stop time but got cold before restart

              The log was hot when SQL thread stopped, but it is not
              anymore when the SQL thread restarts.

              In this case, the SQL thread reopens the log, using
              cache_buf, ie, cur_log points to &cache_buf, and thence
              its coordinates are reset.

           3. Log was already cold at stop time

              The log was not hot when the SQL thread stopped, and, of
              course, it will not be hot when it restarts.

              In this case, the SQL thread opens the cold log again,
              using cache_buf, ie, cur_log points to &cache_buf, and
              thence its coordinates are reset.

           4. Log was hot at stop time, DBA changes to previous cold
              log and restarts SQL thread

              The log was hot when the SQL thread was stopped, but the
              user changed the coordinates of the SQL thread to
              restart from a previous cold log.

              In this case, at start time, cur_log points to a cold
              log, opened using &cache_buf as cache, and coordinates
              are reset. However, as it moves on to the next logs, it
              will eventually reach the hot log. If the hot log is the
              same at the time the SQL thread was stopped, then
              coordinates were not reset - the cur_log will point to
              relay_log.get_log_file(), and not a freshly opened
              IO_CACHE through cache_buf. For this reason we need to
              deploy a my_b_seek before calling check_binlog_magic at
              this point of the code (see: BUG#55263 for more
              details).
          
          NOTES: 
            - We must keep the LOCK_log to read the 4 first bytes, as
              this is a hot log (same as when we call read_log_event()
              above: for a hot log we take the mutex).

            - Because of scenario #4 above, we need to have a
              my_b_seek here. Otherwise, we might hit the assertion
              inside check_binlog_magic.
        */

        my_b_seek(cur_log, (my_off_t) 0);
        if (check_binlog_magic(cur_log,&errmsg))
        {
          if (!hot_log)
            mysql_mutex_unlock(log_lock);
          goto err;
        }
        if (rli->alloc_inuse_relaylog(rli->linfo.log_file_name))
        {
          if (!hot_log)
            mysql_mutex_unlock(log_lock);
          goto err;
        }
        if (!hot_log)
          mysql_mutex_unlock(log_lock);
        continue;
      }
      if (!hot_log)
        mysql_mutex_unlock(log_lock);
      /*
        if we get here, the log was not hot, so we will have to open it
        ourselves. We are sure that the log is still not hot now (a log can get
        from hot to cold, but not from cold to hot). No need for LOCK_log.
      */
      // open_binlog() will check the magic header
      if ((rli->cur_log_fd=open_binlog(cur_log,rli->linfo.log_file_name,
                                       &errmsg)) <0)
        goto err;
      if (rli->alloc_inuse_relaylog(rli->linfo.log_file_name))
        goto err;
    }
    else
    {
      /*
        Read failed with a non-EOF error.
        TODO: come up with something better to handle this error
      */
      if (hot_log)
        mysql_mutex_unlock(log_lock);
      sql_print_error("Slave SQL thread: I/O error reading \
event(errno: %d  cur_log->error: %d)",
                      my_errno,cur_log->error);
      // set read position to the beginning of the event
      my_b_seek(cur_log,rli->event_relay_log_pos);
      /* otherwise, we have had a partial read */
      errmsg = "Aborting slave SQL thread because of partial event read";
      break;                                    // To end of function
    }
  }
  if (!errmsg && global_system_variables.log_warnings)
  {
    sql_print_information("Error reading relay log event: %s",
                          "slave SQL thread was killed");
    DBUG_RETURN(0);
  }

err:
  if (errmsg)
    sql_print_error("Error reading relay log event: %s", errmsg);
  DBUG_RETURN(0);
}

/*
  Rotate a relay log (this is used only by FLUSH LOGS; the automatic rotation
  because of size is simpler because when we do it we already have all relevant
  locks; here we don't, so this function is mainly taking locks).
  Returns nothing as we cannot catch any error (MYSQL_BIN_LOG::new_file()
  is void).
*/

int rotate_relay_log(Master_info* mi)
{
  DBUG_ENTER("rotate_relay_log");
  Relay_log_info* rli= &mi->rli;
  int error= 0;

  DBUG_EXECUTE_IF("crash_before_rotate_relaylog", DBUG_SUICIDE(););

  /*
     We need to test inited because otherwise, new_file() will attempt to lock
     LOCK_log, which may not be inited (if we're not a slave).
  */
  if (!rli->inited)
  {
    DBUG_PRINT("info", ("rli->inited == 0"));
    goto end;
  }

  /* If the relay log is closed, new_file() will do nothing. */
  if ((error= rli->relay_log.new_file()))
    goto end;

  /*
    We harvest now, because otherwise BIN_LOG_HEADER_SIZE will not immediately
    be counted, so imagine a succession of FLUSH LOGS  and assume the slave
    threads are started:
    relay_log_space decreases by the size of the deleted relay log, but does
    not increase, so flush-after-flush we may become negative, which is wrong.
    Even if this will be corrected as soon as a query is replicated on the
    slave (because the I/O thread will then call harvest_bytes_written() which
    will harvest all these BIN_LOG_HEADER_SIZE we forgot), it may give strange
    output in SHOW SLAVE STATUS meanwhile. So we harvest now.
    If the log is closed, then this will just harvest the last writes, probably
    0 as they probably have been harvested.

    Note that it needs to be protected by mi->data_lock.
  */
  mysql_mutex_assert_owner(&mi->data_lock);
  rli->relay_log.harvest_bytes_written(&rli->log_space_total);
end:
  DBUG_RETURN(error);
}


/**
   Detects, based on master's version (as found in the relay log), if master
   has a certain bug.
   @param rli Relay_log_info which tells the master's version
   @param bug_id Number of the bug as found in bugs.mysql.com
   @param report bool report error message, default TRUE

   @param pred Predicate function that will be called with @c param to
   check for the bug. If the function return @c true, the bug is present,
   otherwise, it is not.

   @param param  State passed to @c pred function.

   @return TRUE if master has the bug, FALSE if it does not.
*/
bool rpl_master_has_bug(const Relay_log_info *rli, uint bug_id, bool report,
                        bool (*pred)(const void *), const void *param)
{
  struct st_version_range_for_one_bug {
    uint        bug_id;
    const uchar introduced_in[3]; // first version with bug
    const uchar fixed_in[3];      // first version with fix
  };
  static struct st_version_range_for_one_bug versions_for_all_bugs[]=
  {
    {24432, { 5, 0, 24 }, { 5, 0, 38 } },
    {24432, { 5, 1, 12 }, { 5, 1, 17 } },
    {33029, { 5, 0,  0 }, { 5, 0, 58 } },
    {33029, { 5, 1,  0 }, { 5, 1, 12 } },
    {37426, { 5, 1,  0 }, { 5, 1, 26 } },
  };
  const uchar *master_ver=
    rli->relay_log.description_event_for_exec->server_version_split.ver;

  DBUG_ASSERT(sizeof(rli->relay_log.description_event_for_exec->server_version_split.ver) == 3);

  for (uint i= 0;
       i < sizeof(versions_for_all_bugs)/sizeof(*versions_for_all_bugs);i++)
  {
    const uchar *introduced_in= versions_for_all_bugs[i].introduced_in,
      *fixed_in= versions_for_all_bugs[i].fixed_in;
    if ((versions_for_all_bugs[i].bug_id == bug_id) &&
        (memcmp(introduced_in, master_ver, 3) <= 0) &&
        (memcmp(fixed_in,      master_ver, 3) >  0) &&
        (pred == NULL || (*pred)(param)))
    {
      if (!report)
	return TRUE;
      // a short message for SHOW SLAVE STATUS (message length constraints)
      my_printf_error(ER_UNKNOWN_ERROR, "master may suffer from"
                      " http://bugs.mysql.com/bug.php?id=%u"
                      " so slave stops; check error log on slave"
                      " for more info", MYF(0), bug_id);
      // a verbose message for the error log
      rli->report(ERROR_LEVEL, ER_UNKNOWN_ERROR, NULL,
                  "According to the master's version ('%s'),"
                  " it is probable that master suffers from this bug:"
                      " http://bugs.mysql.com/bug.php?id=%u"
                      " and thus replicating the current binary log event"
                      " may make the slave's data become different from the"
                      " master's data."
                      " To take no risk, slave refuses to replicate"
                      " this event and stops."
                      " We recommend that all updates be stopped on the"
                      " master and slave, that the data of both be"
                      " manually synchronized,"
                      " that master's binary logs be deleted,"
                      " that master be upgraded to a version at least"
                      " equal to '%d.%d.%d'. Then replication can be"
                      " restarted.",
                      rli->relay_log.description_event_for_exec->server_version,
                      bug_id,
                      fixed_in[0], fixed_in[1], fixed_in[2]);
      return TRUE;
    }
  }
  return FALSE;
}

/**
   BUG#33029, For all 5.0 up to 5.0.58 exclusive, and 5.1 up to 5.1.12
   exclusive, if one statement in a SP generated AUTO_INCREMENT value
   by the top statement, all statements after it would be considered
   generated AUTO_INCREMENT value by the top statement, and a
   erroneous INSERT_ID value might be associated with these statement,
   which could cause duplicate entry error and stop the slave.

   Detect buggy master to work around.
 */
bool rpl_master_erroneous_autoinc(THD *thd)
{
  if (thd->rgi_slave)
  {
    DBUG_EXECUTE_IF("simulate_bug33029", return TRUE;);
    return rpl_master_has_bug(thd->rgi_slave->rli, 33029, FALSE, NULL, NULL);
  }
  return FALSE;
}

/**
  @} (end of group Replication)
*/

#endif /* HAVE_REPLICATION */
