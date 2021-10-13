/* Copyright (c) 2006, 2017, Oracle and/or its affiliates.
   Copyright (c) 2010, 2017, MariaDB Corporation

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

#include <my_global.h> // For HAVE_REPLICATION
#include "sql_priv.h"
#include <my_dir.h>
#include "rpl_mi.h"
#include "slave.h"
#include "strfunc.h"
#include "sql_repl.h"

#ifdef HAVE_REPLICATION

#define DEFAULT_CONNECT_RETRY 60

static void init_master_log_pos(Master_info* mi);

Master_info::Master_info(LEX_STRING *connection_name_arg,
                         bool is_slave_recovery)
  :Slave_reporting_capability("I/O"),
   ssl(0), ssl_verify_server_cert(1), fd(-1), io_thd(0), 
   rli(is_slave_recovery), port(MYSQL_PORT),
   checksum_alg_before_fd(BINLOG_CHECKSUM_ALG_UNDEF),
   connect_retry(DEFAULT_CONNECT_RETRY), inited(0), abort_slave(0),
   slave_running(MYSQL_SLAVE_NOT_RUN), slave_run_id(0),
   clock_diff_with_master(0),
   sync_counter(0), heartbeat_period(0), received_heartbeats(0),
   master_id(0), prev_master_id(0),
   using_gtid(USE_GTID_NO), events_queued_since_last_gtid(0),
   gtid_reconnect_event_skip_count(0), gtid_event_seen(false),
   in_start_all_slaves(0), in_stop_all_slaves(0), in_flush_all_relay_logs(0),
   users(0), killed(0)
{
  host[0] = 0; user[0] = 0; password[0] = 0;
  ssl_ca[0]= 0; ssl_capath[0]= 0; ssl_cert[0]= 0;
  ssl_cipher[0]= 0; ssl_key[0]= 0;
  ssl_crl[0]= 0; ssl_crlpath[0]= 0;

  /*
    Store connection name and lower case connection name
    It's safe to ignore any OMM errors as this is checked by error()
  */
  connection_name.length= cmp_connection_name.length=
    connection_name_arg->length;
  if ((connection_name.str= (char*) my_malloc(connection_name_arg->length*2+2,
                                              MYF(MY_WME))))
  {
    cmp_connection_name.str= (connection_name.str +
                              connection_name_arg->length+1);
    strmake(connection_name.str, connection_name_arg->str,
            connection_name.length);
    memcpy(cmp_connection_name.str, connection_name_arg->str,
           connection_name.length+1);
    my_casedn_str(system_charset_info, cmp_connection_name.str);
  }
  /*
    When MySQL restarted, all Rpl_filter settings which aren't in the my.cnf
    will be lost. If you want to lose a setting after restart, you
    should add them into my.cnf
  */
  rpl_filter= get_or_create_rpl_filter(connection_name.str, 
                                       connection_name.length);
  copy_filter_setting(rpl_filter, global_rpl_filter);

  parallel_mode= rpl_filter->get_parallel_mode();

  my_init_dynamic_array(&ignore_server_ids,
                        sizeof(global_system_variables.server_id), 16, 16,
                        MYF(0));
  bzero((char*) &file, sizeof(file));
  mysql_mutex_init(key_master_info_run_lock, &run_lock, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_master_info_data_lock, &data_lock, MY_MUTEX_INIT_FAST);
  mysql_mutex_init(key_master_info_start_stop_lock, &start_stop_lock,
                   MY_MUTEX_INIT_SLOW);
  mysql_mutex_setflags(&run_lock, MYF_NO_DEADLOCK_DETECTION);
  mysql_mutex_setflags(&data_lock, MYF_NO_DEADLOCK_DETECTION);
  mysql_mutex_init(key_master_info_sleep_lock, &sleep_lock, MY_MUTEX_INIT_FAST);
  mysql_cond_init(key_master_info_data_cond, &data_cond, NULL);
  mysql_cond_init(key_master_info_start_cond, &start_cond, NULL);
  mysql_cond_init(key_master_info_stop_cond, &stop_cond, NULL);
  mysql_cond_init(key_master_info_sleep_cond, &sleep_cond, NULL);
}


/**
   Wait until no one is using Master_info
*/

void Master_info::wait_until_free()
{
  mysql_mutex_lock(&sleep_lock);
  killed= 1;
  while (users)
    mysql_cond_wait(&sleep_cond, &sleep_lock);
  mysql_mutex_unlock(&sleep_lock);
}

/**
   Delete master_info
*/

Master_info::~Master_info()
{
  wait_until_free();
  my_free(connection_name.str);
  delete_dynamic(&ignore_server_ids);
  mysql_mutex_destroy(&run_lock);
  mysql_mutex_destroy(&data_lock);
  mysql_mutex_destroy(&sleep_lock);
  mysql_mutex_destroy(&start_stop_lock);
  mysql_cond_destroy(&data_cond);
  mysql_cond_destroy(&start_cond);
  mysql_cond_destroy(&stop_cond);
  mysql_cond_destroy(&sleep_cond);
}

/**
   A comparison function to be supplied as argument to @c sort_dynamic()
   and @c bsearch()

   @return -1 if first argument is less, 0 if it equal to, 1 if it is greater
   than the second
*/
static int change_master_id_cmp(const void *id1, const void *id2)
{
  return (*(ulong *) id1 - *(ulong *) id2);
}

/**
   Reports if the s_id server has been configured to ignore events 
   it generates with

      CHANGE MASTER IGNORE_SERVER_IDS= ( list of server ids )

   Method is called from the io thread event receiver filtering.

   @param      s_id    the master server identifier

   @retval   TRUE    if s_id is in the list of ignored master  servers,
   @retval   FALSE   otherwise.
 */
bool Master_info::shall_ignore_server_id(ulong s_id)
{
  if (likely(ignore_server_ids.elements == 1))
    return (* (ulong*) dynamic_array_ptr(&ignore_server_ids, 0)) == s_id;
  else
    return bsearch((const ulong *) &s_id,
                   ignore_server_ids.buffer,
                   ignore_server_ids.elements, sizeof(ulong),
                   change_master_id_cmp) != NULL;
}

void Master_info::clear_in_memory_info(bool all)
{
  init_master_log_pos(this);
  if (all)
  {
    port= MYSQL_PORT;
    host[0] = 0; user[0] = 0; password[0] = 0;
    domain_id_filter.clear_ids();
    reset_dynamic(&ignore_server_ids);
  }
}


const char *
Master_info::using_gtid_astext(enum enum_using_gtid arg)
{
  switch (arg)
  {
  case USE_GTID_NO:
    return "No";
  case USE_GTID_SLAVE_POS:
    return "Slave_Pos";
  default:
    DBUG_ASSERT(arg == USE_GTID_CURRENT_POS);
    return "Current_Pos";
  }
}


void init_master_log_pos(Master_info* mi)
{
  DBUG_ENTER("init_master_log_pos");

  mi->master_log_name[0] = 0;
  mi->master_log_pos = BIN_LOG_HEADER_SIZE;             // skip magic number
  mi->using_gtid= Master_info::USE_GTID_NO;
  mi->gtid_current_pos.reset();
  mi->events_queued_since_last_gtid= 0;
  mi->gtid_reconnect_event_skip_count= 0;
  mi->gtid_event_seen= false;

  /* Intentionally init ssl_verify_server_cert to 0, no option available  */
  mi->ssl_verify_server_cert= 0;
  /* 
    always request heartbeat unless master_heartbeat_period is set
    explicitly zero.  Here is the default value for heartbeat period
    if CHANGE MASTER did not specify it.  (no data loss in conversion
    as hb period has a max)
  */
  mi->heartbeat_period= (float) MY_MIN(SLAVE_MAX_HEARTBEAT_PERIOD,
                                    (slave_net_timeout/2.0));
  DBUG_ASSERT(mi->heartbeat_period > (float) 0.001
              || mi->heartbeat_period == 0);

  DBUG_VOID_RETURN;
}

/**
  Parses the IO_CACHE for "key=" and returns the "key".
  If no '=' found, returns the whole line (for END_MARKER).

  @param key      [OUT]               Key buffer
  @param max_size [IN]                Maximum buffer size
  @param f        [IN]                IO_CACHE file
  @param found_equal [OUT]            Set true if a '=' was found.

  @retval 0                           Either "key=" or '\n' found
  @retval 1                           EOF
*/
static int
read_mi_key_from_file(char *key, int max_size, IO_CACHE *f, bool *found_equal)
{
  int i= 0, c;

  DBUG_ENTER("read_key_from_file");

  *found_equal= false;
  if (max_size <= 0)
    DBUG_RETURN(1);
  for (;;)
  {
    if (i >= max_size-1)
    {
      key[i] = '\0';
      DBUG_RETURN(0);
    }
    c= my_b_get(f);
    if (c == my_b_EOF)
    {
      DBUG_RETURN(1);
    }
    else if (c == '\n')
    {
      key[i]= '\0';
      DBUG_RETURN(0);
    }
    else if (c == '=')
    {
      key[i]= '\0';
      *found_equal= true;
      DBUG_RETURN(0);
    }
    else
    {
      key[i]= c;
      ++i;
    }
  }
  /* NotReached */
}

enum {
  LINES_IN_MASTER_INFO_WITH_SSL= 14,

  /* 5.1.16 added value of master_ssl_verify_server_cert */
  LINE_FOR_MASTER_SSL_VERIFY_SERVER_CERT= 15,

  /* 5.5 added value of master_heartbeat_period */
  LINE_FOR_MASTER_HEARTBEAT_PERIOD= 16,

  /* MySQL Cluster 6.3 added master_bind */
  LINE_FOR_MASTER_BIND = 17,

  /* 6.0 added value of master_ignore_server_id */
  LINE_FOR_REPLICATE_IGNORE_SERVER_IDS= 18,

  /* 6.0 added value of master_uuid */
  LINE_FOR_MASTER_UUID= 19,

  /* line for master_retry_count */
  LINE_FOR_MASTER_RETRY_COUNT= 20,

  /* line for ssl_crl */
  LINE_FOR_SSL_CRL= 21,

  /* line for ssl_crl */
  LINE_FOR_SSL_CRLPATH= 22,

  /* MySQL 5.6 fixed-position lines. */
  LINE_FOR_FIRST_MYSQL_5_6=23,
  LINE_FOR_LAST_MYSQL_5_6=23,
  /* Reserved lines for MySQL future versions. */
  LINE_FOR_LAST_MYSQL_FUTURE=33,
  /* Number of (fixed-position) lines used when saving master info file */
  LINES_IN_MASTER_INFO= LINE_FOR_LAST_MYSQL_FUTURE
};

int init_master_info(Master_info* mi, const char* master_info_fname,
                     const char* slave_info_fname,
                     bool abort_if_no_master_info_file,
                     int thread_mask)
{
  int fd,error;
  char fname[FN_REFLEN+128];
  DBUG_ENTER("init_master_info");

  if (mi->inited)
  {
    /*
      We have to reset read position of relay-log-bin as we may have
      already been reading from 'hotlog' when the slave was stopped
      last time. If this case pos_in_file would be set and we would
      get a crash when trying to read the signature for the binary
      relay log.

      We only rewind the read position if we are starting the SQL
      thread. The handle_slave_sql thread assumes that the read
      position is at the beginning of the file, and will read the
      "signature" and then fast-forward to the last position read.
    */
    if (thread_mask & SLAVE_SQL)
    {
      bool hot_log= FALSE;
      /* 
         my_b_seek does an implicit flush_io_cache, so we need to:

         1. check if this log is active (hot)
         2. if it is we keep log_lock until the seek ends, otherwise 
            release it right away.

         If we did not take log_lock, SQL thread might race with IO
         thread for the IO_CACHE mutex.

       */
      mysql_mutex_t *log_lock= mi->rli.relay_log.get_log_lock();
      mysql_mutex_lock(log_lock);
      hot_log= mi->rli.relay_log.is_active(mi->rli.linfo.log_file_name);

      if (!hot_log)
        mysql_mutex_unlock(log_lock);

      my_b_seek(mi->rli.cur_log, (my_off_t) 0);

      if (hot_log)
        mysql_mutex_unlock(log_lock);
    }
    DBUG_RETURN(0);
  }

  mi->mysql=0;
  mi->file_id=1;
  fn_format(fname, master_info_fname, mysql_data_home, "", 4+32);

  /*
    We need a mutex while we are changing master info parameters to
    keep other threads from reading bogus info
  */

  mysql_mutex_lock(&mi->data_lock);
  fd = mi->fd;

  /* does master.info exist ? */

  if (access(fname,F_OK))
  {
    if (abort_if_no_master_info_file)
    {
      mysql_mutex_unlock(&mi->data_lock);
      DBUG_RETURN(0);
    }
    /*
      if someone removed the file from underneath our feet, just close
      the old descriptor and re-create the old file
    */
    if (fd >= 0)
      mysql_file_close(fd, MYF(MY_WME));
    if ((fd= mysql_file_open(key_file_master_info,
                             fname, O_CREAT|O_RDWR|O_BINARY, MYF(MY_WME))) < 0 )
    {
      sql_print_error("Failed to create a new master info file (\
file '%s', errno %d)", fname, my_errno);
      goto err;
    }
    if (init_io_cache(&mi->file, fd, IO_SIZE*2, READ_CACHE, 0L,0,
                      MYF(MY_WME)))
    {
      sql_print_error("Failed to create a cache on master info file (\
file '%s')", fname);
      goto err;
    }

    mi->fd = fd;
    mi->clear_in_memory_info(false);

  }
  else // file exists
  {
    if (fd >= 0)
      reinit_io_cache(&mi->file, READ_CACHE, 0L,0,0);
    else
    {
      if ((fd= mysql_file_open(key_file_master_info,
                               fname, O_RDWR|O_BINARY, MYF(MY_WME))) < 0 )
      {
        sql_print_error("Failed to open the existing master info file (\
file '%s', errno %d)", fname, my_errno);
        goto err;
      }
      if (init_io_cache(&mi->file, fd, IO_SIZE*2, READ_CACHE, 0L,
                        0, MYF(MY_WME)))
      {
        sql_print_error("Failed to create a cache on master info file (\
file '%s')", fname);
        goto err;
      }
    }

    mi->fd = fd;
    int port, connect_retry, master_log_pos, lines;
    int ssl= 0, ssl_verify_server_cert= 0;
    float master_heartbeat_period= 0.0;
    char *first_non_digit;
    char buf[HOSTNAME_LENGTH+1];

    /*
       Starting from 4.1.x master.info has new format. Now its
       first line contains number of lines in file. By reading this
       number we will be always distinguish to which version our
       master.info corresponds to. We can't simply count lines in
       file since versions before 4.1.x could generate files with more
       lines than needed.
       If first line doesn't contain a number or contain number less than
       LINES_IN_MASTER_INFO_WITH_SSL then such file is treated like file
       from pre 4.1.1 version.
       There is no ambiguity when reading an old master.info, as before
       4.1.1, the first line contained the binlog's name, which is either
       empty or has an extension (contains a '.'), so can't be confused
       with an integer.

       So we're just reading first line and trying to figure which version
       is this.
    */

    /*
       The first row is temporarily stored in mi->master_log_name,
       if it is line count and not binlog name (new format) it will be
       overwritten by the second row later.
    */
    if (init_strvar_from_file(mi->master_log_name,
                              sizeof(mi->master_log_name), &mi->file,
                              ""))
      goto errwithmsg;

    lines= strtoul(mi->master_log_name, &first_non_digit, 10);

    if (mi->master_log_name[0]!='\0' &&
        *first_non_digit=='\0' && lines >= LINES_IN_MASTER_INFO_WITH_SSL)
    {
      /* Seems to be new format => read master log name from next line */
      if (init_strvar_from_file(mi->master_log_name,
            sizeof(mi->master_log_name), &mi->file, ""))
        goto errwithmsg;
    }
    else
      lines= 7;

    if (init_intvar_from_file(&master_log_pos, &mi->file, 4) ||
        init_strvar_from_file(mi->host, sizeof(mi->host), &mi->file, 0) ||
        init_strvar_from_file(mi->user, sizeof(mi->user), &mi->file, "test") ||
        init_strvar_from_file(mi->password, SCRAMBLED_PASSWORD_CHAR_LENGTH+1,
                              &mi->file, 0) ||
        init_intvar_from_file(&port, &mi->file, MYSQL_PORT) ||
        init_intvar_from_file(&connect_retry, &mi->file,
                              DEFAULT_CONNECT_RETRY))
      goto errwithmsg;

    /*
       If file has ssl part use it even if we have server without
       SSL support. But these options will be ignored later when
       slave will try connect to master, so in this case warning
       is printed.
     */
    if (lines >= LINES_IN_MASTER_INFO_WITH_SSL)
    {
      if (init_intvar_from_file(&ssl, &mi->file, 0) ||
          init_strvar_from_file(mi->ssl_ca, sizeof(mi->ssl_ca),
                                &mi->file, 0) ||
          init_strvar_from_file(mi->ssl_capath, sizeof(mi->ssl_capath),
                                &mi->file, 0) ||
          init_strvar_from_file(mi->ssl_cert, sizeof(mi->ssl_cert),
                                &mi->file, 0) ||
          init_strvar_from_file(mi->ssl_cipher, sizeof(mi->ssl_cipher),
                                &mi->file, 0) ||
          init_strvar_from_file(mi->ssl_key, sizeof(mi->ssl_key),
                                &mi->file, 0))
        goto errwithmsg;

      /*
        Starting from 5.1.16 ssl_verify_server_cert might be
        in the file
      */
      if (lines >= LINE_FOR_MASTER_SSL_VERIFY_SERVER_CERT &&
          init_intvar_from_file(&ssl_verify_server_cert, &mi->file, 0))
        goto errwithmsg;
      /*
        Starting from 6.0 master_heartbeat_period might be
        in the file
      */
      if (lines >= LINE_FOR_MASTER_HEARTBEAT_PERIOD &&
          init_floatvar_from_file(&master_heartbeat_period, &mi->file, 0.0))
        goto errwithmsg;
      /*
	Starting from MySQL Cluster 6.3 master_bind might be in the file
	(this is just a reservation to avoid future upgrade problems) 
       */
      if (lines >= LINE_FOR_MASTER_BIND &&
	  init_strvar_from_file(buf, sizeof(buf), &mi->file, ""))
	  goto errwithmsg;
      /*
        Starting from 6.0 list of server_id of ignorable servers might be
        in the file
      */
      if (lines >= LINE_FOR_REPLICATE_IGNORE_SERVER_IDS &&
          init_dynarray_intvar_from_file(&mi->ignore_server_ids, &mi->file))
      {
        sql_print_error("Failed to initialize master info ignore_server_ids");
        goto errwithmsg;
      }

      /* reserved */
      if (lines >= LINE_FOR_MASTER_UUID &&
	  init_strvar_from_file(buf, sizeof(buf), &mi->file, ""))
	  goto errwithmsg;

      /* Starting from 5.5 the master_retry_count may be in the repository. */
      if (lines >= LINE_FOR_MASTER_RETRY_COUNT &&
	  init_strvar_from_file(buf, sizeof(buf), &mi->file, ""))
	  goto errwithmsg;

      if (lines >= LINE_FOR_SSL_CRLPATH &&
	  (init_strvar_from_file(mi->ssl_crl, sizeof(mi->ssl_crl),
                                 &mi->file, "") ||
	   init_strvar_from_file(mi->ssl_crlpath, sizeof(mi->ssl_crlpath),
                                 &mi->file, "")))
	  goto errwithmsg;

      /*
        Starting with MariaDB 10.0, we use a key=value syntax, which is nicer
        in several ways. But we leave a bunch of empty lines to accomodate
        any future old-style additions in MySQL (this will make it easier for
        users moving from MariaDB to MySQL, to not have MySQL try to
        interpret a MariaDB key=value line.)
      */
      if (lines >= LINE_FOR_LAST_MYSQL_FUTURE)
      {
        uint i;
        bool got_eq;
        bool seen_using_gtid= false;
        bool seen_do_domain_ids=false, seen_ignore_domain_ids=false;

        /* Skip lines used by / reserved for MySQL >= 5.6. */
        for (i= LINE_FOR_FIRST_MYSQL_5_6; i <= LINE_FOR_LAST_MYSQL_FUTURE; ++i)
        {
          if (init_strvar_from_file(buf, sizeof(buf), &mi->file, ""))
          goto errwithmsg;
        }

        /*
          Parse any extra key=value lines. read_key_from_file() parses the file
          for "key=" and returns the "key" if found. The "value" can then the
          parsed on case by case basis. The "unknown" lines would be ignored to
          facilitate downgrades.
          10.0 does not have the END_MARKER before any left-overs at the end
          of the file. So ignore any but the first occurrence of a key.
        */
        while (!read_mi_key_from_file(buf, sizeof(buf), &mi->file, &got_eq))
        {
          if (got_eq && !seen_using_gtid && !strcmp(buf, "using_gtid"))
          {
            int val;
            if (!init_intvar_from_file(&val, &mi->file, 0))
            {
              if (val == Master_info::USE_GTID_CURRENT_POS)
                mi->using_gtid= Master_info::USE_GTID_CURRENT_POS;
              else if (val == Master_info::USE_GTID_SLAVE_POS)
                mi->using_gtid= Master_info::USE_GTID_SLAVE_POS;
              else
                mi->using_gtid= Master_info::USE_GTID_NO;
              seen_using_gtid= true;
            } else {
              sql_print_error("Failed to initialize master info using_gtid");
              goto errwithmsg;
            }
          }
          else if (got_eq && !seen_do_domain_ids && !strcmp(buf, "do_domain_ids"))
          {
            if (mi->domain_id_filter.init_ids(&mi->file,
                                              Domain_id_filter::DO_DOMAIN_IDS))
            {
              sql_print_error("Failed to initialize master info do_domain_ids");
              goto errwithmsg;
            }
            seen_do_domain_ids= true;
          }
          else if (got_eq && !seen_ignore_domain_ids &&
                   !strcmp(buf, "ignore_domain_ids"))
          {
            if (mi->domain_id_filter.init_ids(&mi->file,
                                              Domain_id_filter::IGNORE_DOMAIN_IDS))
            {
              sql_print_error("Failed to initialize master info "
                              "ignore_domain_ids");
              goto errwithmsg;
            }
            seen_ignore_domain_ids= true;
          }
          else if (!got_eq && !strcmp(buf, "END_MARKER"))
          {
            /*
              Guard agaist extra left-overs at the end of file, in case a later
              update causes the file to shrink compared to earlier contents.
            */
            break;
          }
        }
      }
    }

#ifndef HAVE_OPENSSL
    if (ssl)
      sql_print_warning("SSL information in the master info file "
                      "('%s') are ignored because this MySQL slave was "
                      "compiled without SSL support.", fname);
#endif /* HAVE_OPENSSL */

    /*
      This has to be handled here as init_intvar_from_file can't handle
      my_off_t types
    */
    mi->master_log_pos= (my_off_t) master_log_pos;
    mi->port= (uint) port;
    mi->connect_retry= (uint) connect_retry;
    mi->ssl= (my_bool) ssl;
    mi->ssl_verify_server_cert= ssl_verify_server_cert;
    mi->heartbeat_period= MY_MIN(SLAVE_MAX_HEARTBEAT_PERIOD, master_heartbeat_period);
  }
  DBUG_PRINT("master_info",("log_file_name: %s  position: %ld",
                            mi->master_log_name,
                            (ulong) mi->master_log_pos));

  mi->rli.mi= mi;
  if (mi->rli.init(slave_info_fname))
    goto err;

  mi->inited = 1;
  mi->rli.is_relay_log_recovery= FALSE;
  // now change cache READ -> WRITE - must do this before flush_master_info
  reinit_io_cache(&mi->file, WRITE_CACHE, 0L, 0, 1);
  if ((error= MY_TEST(flush_master_info(mi, TRUE, TRUE))))
    sql_print_error("Failed to flush master info file");
  mysql_mutex_unlock(&mi->data_lock);
  DBUG_RETURN(error);

errwithmsg:
  sql_print_error("Error reading master configuration");

err:
  if (fd >= 0)
  {
    mysql_file_close(fd, MYF(0));
    end_io_cache(&mi->file);
  }
  mi->fd= -1;
  mysql_mutex_unlock(&mi->data_lock);
  DBUG_RETURN(1);
}


/*
  RETURN
     2 - flush relay log failed
     1 - flush master info failed
     0 - all ok
*/
int flush_master_info(Master_info* mi, 
                      bool flush_relay_log_cache, 
                      bool need_lock_relay_log)
{
  IO_CACHE* file = &mi->file;
  char lbuf[22];
  int err= 0;

  DBUG_ENTER("flush_master_info");
  DBUG_PRINT("enter",("master_pos: %ld", (long) mi->master_log_pos));

  /*
    Flush the relay log to disk. If we don't do it, then the relay log while
    have some part (its last kilobytes) in memory only, so if the slave server
    dies now, with, say, from master's position 100 to 150 in memory only (not
    on disk), and with position 150 in master.info, then when the slave
    restarts, the I/O thread will fetch binlogs from 150, so in the relay log
    we will have "[0, 100] U [150, infinity[" and nobody will notice it, so the
    SQL thread will jump from 100 to 150, and replication will silently break.

    When we come to this place in code, relay log may or not be initialized;
    the caller is responsible for setting 'flush_relay_log_cache' accordingly.
  */
  if (flush_relay_log_cache)
  {
    mysql_mutex_t *log_lock= mi->rli.relay_log.get_log_lock();
    IO_CACHE *log_file= mi->rli.relay_log.get_log_file();

    if (need_lock_relay_log)
      mysql_mutex_lock(log_lock);

    mysql_mutex_assert_owner(log_lock);
    err= flush_io_cache(log_file);

    if (need_lock_relay_log)
      mysql_mutex_unlock(log_lock);

    if (err)
      DBUG_RETURN(2);
  }

  /*
    produce a line listing the total number and all the ignored server_id:s
  */
  char* ignore_server_ids_buf;
  {
    ignore_server_ids_buf=
      (char *) my_malloc((sizeof(global_system_variables.server_id) * 3 + 1) *
                         (1 + mi->ignore_server_ids.elements), MYF(MY_WME));
    if (!ignore_server_ids_buf)
      DBUG_RETURN(1);                           /* error */
    ulong cur_len= sprintf(ignore_server_ids_buf, "%u",
                           mi->ignore_server_ids.elements);
    for (ulong i= 0; i < mi->ignore_server_ids.elements; i++)
    {
      ulong s_id;
      get_dynamic(&mi->ignore_server_ids, (uchar*) &s_id, i);
      cur_len+= sprintf(ignore_server_ids_buf + cur_len, " %lu", s_id);
    }
  }

  char *do_domain_ids_buf= 0, *ignore_domain_ids_buf= 0;

  do_domain_ids_buf=
    mi->domain_id_filter.as_string(Domain_id_filter::DO_DOMAIN_IDS);
  if (do_domain_ids_buf == NULL)
  {
    err= 1;                                     /* error */
    goto done;
  }

  ignore_domain_ids_buf=
    mi->domain_id_filter.as_string(Domain_id_filter::IGNORE_DOMAIN_IDS);
  if (ignore_domain_ids_buf == NULL)
  {
    err= 1;                                     /* error */
    goto done;
  }

  /*
    We flushed the relay log BEFORE the master.info file, because if we crash
    now, we will get a duplicate event in the relay log at restart. If we
    flushed in the other order, we would get a hole in the relay log.
    And duplicate is better than hole (with a duplicate, in later versions we
    can add detection and scrap one event; with a hole there's nothing we can
    do).
  */

  /*
     In certain cases this code may create master.info files that seems
     corrupted, because of extra lines filled with garbage in the end
     file (this happens if new contents take less space than previous
     contents of file). But because of number of lines in the first line
     of file we don't care about this garbage.
  */
  char heartbeat_buf[FLOATING_POINT_BUFFER];
  my_fcvt(mi->heartbeat_period, 3, heartbeat_buf, NULL);
  my_b_seek(file, 0L);
  my_b_printf(file,
              "%u\n%s\n%s\n%s\n%s\n%s\n%d\n%d\n%d\n%s\n%s\n%s\n%s\n%s\n%d\n%s\n%s\n%s\n%s\n%d\n%s\n%s\n"
              "\n\n\n\n\n\n\n\n\n\n\n"
              "using_gtid=%d\n"
              "do_domain_ids=%s\n"
              "ignore_domain_ids=%s\n"
              "END_MARKER\n",
              LINES_IN_MASTER_INFO,
              mi->master_log_name, llstr(mi->master_log_pos, lbuf),
              mi->host, mi->user,
              mi->password, mi->port, mi->connect_retry,
              (int)(mi->ssl), mi->ssl_ca, mi->ssl_capath, mi->ssl_cert,
              mi->ssl_cipher, mi->ssl_key, mi->ssl_verify_server_cert,
              heartbeat_buf, "", ignore_server_ids_buf,
              "", 0,
              mi->ssl_crl, mi->ssl_crlpath, mi->using_gtid,
              do_domain_ids_buf, ignore_domain_ids_buf);
  err= flush_io_cache(file);
  if (sync_masterinfo_period && !err &&
      ++(mi->sync_counter) >= sync_masterinfo_period)
  {
    err= my_sync(mi->fd, MYF(MY_WME));
    mi->sync_counter= 0;
  }

  /* Fix err; flush_io_cache()/my_sync() may return -1 */
  err= (err != 0) ? 1 : 0;

done:
  my_free(ignore_server_ids_buf);
  my_free(do_domain_ids_buf);
  my_free(ignore_domain_ids_buf);
  DBUG_RETURN(err);
}


void end_master_info(Master_info* mi)
{
  DBUG_ENTER("end_master_info");

  if (!mi->inited)
    DBUG_VOID_RETURN;
  if (mi->fd >= 0)
  {
    end_io_cache(&mi->file);
    mysql_file_close(mi->fd, MYF(MY_WME));
    mi->fd = -1;
  }
  mi->inited = 0;

  DBUG_VOID_RETURN;
}

/* Multi-Master By P.Linux */
uchar *get_key_master_info(Master_info *mi, size_t *length,
                           my_bool not_used __attribute__((unused)))
{
  /* Return lower case name */
  *length= mi->cmp_connection_name.length;
  return (uchar*) mi->cmp_connection_name.str;
}

/*
  Delete a master info

  Called from my_hash_delete(&master_info_hash)
  Stops associated slave threads and frees master_info
*/

void free_key_master_info(Master_info *mi)
{
  DBUG_ENTER("free_key_master_info");
  mysql_mutex_unlock(&LOCK_active_mi);

  /* Ensure that we are not in reset_slave while this is done */
  mi->lock_slave_threads();
  terminate_slave_threads(mi,SLAVE_FORCE_ALL);
  /* We use 2 here instead of 1 just to make it easier when debugging */
  mi->killed= 2;
  end_master_info(mi);
  end_relay_log_info(&mi->rli);
  mi->unlock_slave_threads();
  delete mi;

  mysql_mutex_lock(&LOCK_active_mi);
  DBUG_VOID_RETURN;
}

/**
   Check if connection name for master_info is valid.

   It's valid if it's a valid system name of length less than
   MAX_CONNECTION_NAME.

   @return
   0 ok
   1 error
*/

bool check_master_connection_name(LEX_STRING *name)
{
  if (name->length >= MAX_CONNECTION_NAME)
    return 1;
  return 0;
}
 

/**
   Create a log file with a given suffix.

   @param
   res_file_name	Store result here
   length		Length of res_file_name buffer
   info_file		Original file name (prefix)
   append		1 if we should add suffix last (not before ext)
   suffix		Suffix

   @note
   The suffix is added before the extension of the file name prefixed with '-'.
   The suffix is also converted to lower case and we transform
   all not safe character, as we do with MySQL table names.

   If suffix is an empty string, then we don't add any suffix.
   This is to allow one to use this function also to generate old
   file names without a prefix.
*/

void create_logfile_name_with_suffix(char *res_file_name, size_t length,
                                     const char *info_file, bool append,
                                     LEX_STRING *suffix)
{
  char buff[MAX_CONNECTION_NAME+1],
    res[MAX_CONNECTION_NAME * MAX_FILENAME_MBWIDTH+1], *p;

  p= strmake(res_file_name, info_file, length);
  /* If not empty suffix and there is place left for some part of the suffix */
  if (suffix->length != 0 && p <= res_file_name + length -1)
  {
    const char *info_file_end= info_file + (p - res_file_name);
    const char *ext= append ? info_file_end : fn_ext2(info_file);
    size_t res_length, ext_pos, from_length;
    uint errors;

    /* Create null terminated string */
    from_length= strmake(buff, suffix->str, suffix->length) - buff;
    /* Convert to characters usable in a file name */
    res_length= strconvert(system_charset_info, buff, from_length,
                           &my_charset_filename, res, sizeof(res), &errors);
    
    ext_pos= (size_t) (ext - info_file);
    length-= (suffix->length - ext_pos); /* Leave place for extension */
    p= res_file_name + ext_pos;
    *p++= '-';                           /* Add separator */
    p= strmake(p, res, MY_MIN((size_t) (length - (p - res_file_name)),
                           res_length));
    /* Add back extension. We have checked above that there is space for it */
    strmov(p, ext);
  }
}

void copy_filter_setting(Rpl_filter* dst_filter, Rpl_filter* src_filter)
{
  char buf[256];
  String tmp(buf, sizeof(buf), &my_charset_bin);

  dst_filter->get_do_db(&tmp);
  if (tmp.is_empty())
  {
    src_filter->get_do_db(&tmp);
    if (!tmp.is_empty())
      dst_filter->set_do_db(tmp.ptr());
  }

  dst_filter->get_do_table(&tmp);
  if (tmp.is_empty())
  {
    src_filter->get_do_table(&tmp);
    if (!tmp.is_empty())
      dst_filter->set_do_table(tmp.ptr());
  }

  dst_filter->get_ignore_db(&tmp);
  if (tmp.is_empty())
  {
    src_filter->get_ignore_db(&tmp);
    if (!tmp.is_empty())
      dst_filter->set_ignore_db(tmp.ptr());
  }

  dst_filter->get_ignore_table(&tmp);
  if (tmp.is_empty())
  {
    src_filter->get_ignore_table(&tmp);
    if (!tmp.is_empty())
      dst_filter->set_ignore_table(tmp.ptr());
  }

  dst_filter->get_wild_do_table(&tmp);
  if (tmp.is_empty())
  {
    src_filter->get_wild_do_table(&tmp);
    if (!tmp.is_empty())
      dst_filter->set_wild_do_table(tmp.ptr());
  }

  dst_filter->get_wild_ignore_table(&tmp);
  if (tmp.is_empty())
  {
    src_filter->get_wild_ignore_table(&tmp);
    if (!tmp.is_empty())
      dst_filter->set_wild_ignore_table(tmp.ptr());
  }

  if (dst_filter->rewrite_db_is_empty())
  {
    if (!src_filter->rewrite_db_is_empty())
      dst_filter->copy_rewrite_db(src_filter);
  }
}

Master_info_index::Master_info_index()
{
  size_t filename_length, dir_length;
  /*
    Create the Master_info index file by prepending 'multi-' before
    the master_info_file file name.
  */
  fn_format(index_file_name, master_info_file, mysql_data_home,
            "", MY_UNPACK_FILENAME);
  filename_length= strlen(index_file_name) + 1; /* Count 0 byte */
  dir_length= dirname_length(index_file_name);
  bmove_upp((uchar*) index_file_name + filename_length + 6,
            (uchar*) index_file_name + filename_length,
            filename_length - dir_length);
  memcpy(index_file_name + dir_length, "multi-", 6);

  bzero((char*) &index_file, sizeof(index_file));
  index_file.file= -1;
}


/**
   Free all connection threads

   This is done during early stages of shutdown
   to give connection threads and slave threads time
   to die before ~Master_info_index is called
*/

void Master_info_index::free_connections()
{
  mysql_mutex_assert_owner(&LOCK_active_mi);
  my_hash_reset(&master_info_hash);
}


/**
   Free all connection threads and free structures
*/

Master_info_index::~Master_info_index()
{
  my_hash_free(&master_info_hash);
  end_io_cache(&index_file);
  if (index_file.file >= 0)
    my_close(index_file.file, MYF(MY_WME));
}


/* Load All Master_info from master.info.index File
 * RETURN:
 *   0 - All Success
 *   1 - All Fail
 *   2 - Some Success, Some Fail
 */

bool Master_info_index::init_all_master_info()
{
  int thread_mask;
  int err_num= 0, succ_num= 0; // The number of success read Master_info
  char sign[MAX_CONNECTION_NAME+1];
  File index_file_nr;
  THD *thd;
  DBUG_ENTER("init_all_master_info");

  DBUG_ASSERT(master_info_index);

  if ((index_file_nr= my_open(index_file_name,
                              O_RDWR | O_CREAT | O_BINARY ,
                              MYF(MY_WME | ME_NOREFRESH))) < 0 ||
      my_sync(index_file_nr, MYF(MY_WME)) ||
      init_io_cache(&index_file, index_file_nr,
                    IO_SIZE, READ_CACHE,
                    my_seek(index_file_nr,0L,MY_SEEK_END,MYF(0)),
                    0, MYF(MY_WME | MY_WAIT_IF_FULL)))
  {
    if (index_file_nr >= 0)
      my_close(index_file_nr,MYF(0));

    sql_print_error("Creation of Master_info index file '%s' failed",
                    index_file_name);
    DBUG_RETURN(1);
  }

  /* Initialize Master_info Hash Table */
  if (my_hash_init(&master_info_hash, system_charset_info, 
                   MAX_REPLICATION_THREAD, 0, 0, 
                   (my_hash_get_key) get_key_master_info, 
                   (my_hash_free_key)free_key_master_info, HASH_UNIQUE))
  {                                                      
    sql_print_error("Initializing Master_info hash table failed");
    DBUG_RETURN(1);
  }

  thd= new THD(next_thread_id());  /* Needed by start_slave_threads */
  thd->thread_stack= (char*) &thd;
  thd->store_globals();

  reinit_io_cache(&index_file, READ_CACHE, 0L,0,0);
  while (!init_strvar_from_file(sign, sizeof(sign),
                                &index_file, NULL))
  {
    LEX_STRING connection_name;
    Master_info *mi;
    char buf_master_info_file[FN_REFLEN];
    char buf_relay_log_info_file[FN_REFLEN];

    connection_name.str=    sign;
    connection_name.length= strlen(sign);
    if (!(mi= new Master_info(&connection_name, relay_log_recovery)) ||
        mi->error())
    {
      delete mi;
      goto error;
    }

    init_thread_mask(&thread_mask,mi,0 /*not inverse*/);

    create_logfile_name_with_suffix(buf_master_info_file,
                                    sizeof(buf_master_info_file),
                                    master_info_file, 0,
                                    &mi->cmp_connection_name);
    create_logfile_name_with_suffix(buf_relay_log_info_file,
                                    sizeof(buf_relay_log_info_file),
                                    relay_log_info_file, 0,
                                    &mi->cmp_connection_name);
    if (global_system_variables.log_warnings > 1)
      sql_print_information("Reading Master_info: '%s'  Relay_info:'%s'",
                            buf_master_info_file, buf_relay_log_info_file);

    mi->lock_slave_threads();
    if (init_master_info(mi, buf_master_info_file, buf_relay_log_info_file, 
                         0, thread_mask))
    {
      err_num++;
      sql_print_error("Initialized Master_info from '%s' failed",
                      buf_master_info_file);
      if (!master_info_index->get_master_info(&connection_name,
                                              Sql_condition::WARN_LEVEL_NOTE))
      {
        /* Master_info is not in HASH; Add it */
        if (master_info_index->add_master_info(mi, FALSE))
          goto error;
        succ_num++;
        mi->unlock_slave_threads();
      }
      else
      {
        /* Master_info already in HASH */
        sql_print_error(ER_THD_OR_DEFAULT(current_thd,
                                          ER_CONNECTION_ALREADY_EXISTS),
                        (int) connection_name.length, connection_name.str,
                        (int) connection_name.length, connection_name.str);
        mi->unlock_slave_threads();
        delete mi;
      }
      continue;
    }
    else
    {
      /* Initialization of Master_info succeded. Add it to HASH */
      if (global_system_variables.log_warnings > 1)
        sql_print_information("Initialized Master_info from '%s'",
                              buf_master_info_file);
      if (master_info_index->get_master_info(&connection_name,
                                             Sql_condition::WARN_LEVEL_NOTE))
      {
        /* Master_info was already registered */
        sql_print_error(ER_THD_OR_DEFAULT(current_thd,
                                          ER_CONNECTION_ALREADY_EXISTS),
                        (int) connection_name.length, connection_name.str,
                        (int) connection_name.length, connection_name.str);
        mi->unlock_slave_threads();
        delete mi;
        continue;
      }

      /* Master_info was not registered; add it */
      if (master_info_index->add_master_info(mi, FALSE))
        goto error;
      succ_num++;

      if (!opt_skip_slave_start)
      {
        if (start_slave_threads(current_thd,
                                1 /* need mutex */,
                                1 /* wait for start*/,
                                mi,
                                buf_master_info_file,
                                buf_relay_log_info_file,
                                SLAVE_IO | SLAVE_SQL))
        {
          sql_print_error("Failed to create slave threads for connection '%.*s'",
                          (int) connection_name.length,
                          connection_name.str);
          continue;
        }
        if (global_system_variables.log_warnings)
          sql_print_information("Started replication for '%.*s'",
                                (int) connection_name.length,
                                connection_name.str);
      }
      mi->unlock_slave_threads();
    }
  }
  thd->reset_globals();
  delete thd;

  if (!err_num) // No Error on read Master_info
  {
    if (global_system_variables.log_warnings > 1)
      sql_print_information("Reading of all Master_info entries succeeded");
    DBUG_RETURN(0);
  }
  if (succ_num) // Have some Error and some Success
  {
    sql_print_warning("Reading of some Master_info entries failed");
    DBUG_RETURN(1);
  }

  sql_print_error("Reading of all Master_info entries failed!");
  DBUG_RETURN(1);

error:
  thd->reset_globals();
  delete thd;
  DBUG_RETURN(1);
}


/* Write new master.info to master.info.index File */
bool Master_info_index::write_master_name_to_index_file(LEX_STRING *name,
                                                        bool do_sync)
{
  DBUG_ASSERT(my_b_inited(&index_file) != 0);
  DBUG_ENTER("write_master_name_to_index_file");

  /* Don't write default slave to master_info.index */
  if (name->length == 0)
    DBUG_RETURN(0);

  reinit_io_cache(&index_file, WRITE_CACHE,
                  my_b_filelength(&index_file), 0, 0);

  if (my_b_write(&index_file, (uchar*) name->str, name->length) ||
      my_b_write(&index_file, (uchar*) "\n", 1) ||
      flush_io_cache(&index_file) ||
      (do_sync && my_sync(index_file.file, MYF(MY_WME))))
  {
    sql_print_error("Write of new Master_info for '%.*s' to index file failed",
                    (int) name->length, name->str);
    DBUG_RETURN(1);
  }

  DBUG_RETURN(0);
}


/**
   Get Master_info for a connection and lock the object from deletion

   @param
   connection_name	Connection name
   warning		WARN_LEVEL_NOTE -> Don't print anything
			WARN_LEVEL_WARN -> Issue warning if not exists
			WARN_LEVEL_ERROR-> Issue error if not exists
*/

Master_info *get_master_info(const LEX_STRING *connection_name,
                             Sql_condition::enum_warning_level warning)
{
  Master_info *mi;
  DBUG_ENTER("get_master_info");

  /* Protect against inserts into hash */
  mysql_mutex_lock(&LOCK_active_mi);
  /*
    The following can only be true during shutdown when slave has been killed
    but some other threads are still trying to access slave statistics.
  */
  if (unlikely(!master_info_index)) 
  {
    if (warning != Sql_condition::WARN_LEVEL_NOTE)
      my_error(WARN_NO_MASTER_INFO,
               MYF(warning == Sql_condition::WARN_LEVEL_WARN ?
                   ME_JUST_WARNING : 0),
               (int) connection_name->length, connection_name->str);
    mysql_mutex_unlock(&LOCK_active_mi);
    DBUG_RETURN(0);
  }
  if ((mi= master_info_index->get_master_info(connection_name, warning)))
  {
    /*
      We have to use sleep_lock here. If we would use LOCK_active_mi
      then we would take locks in wrong order in Master_info::release()
    */
    mysql_mutex_lock(&mi->sleep_lock);
    mi->users++;
    DBUG_PRINT("info",("users: %d", mi->users));
    mysql_mutex_unlock(&mi->sleep_lock);
  }
  mysql_mutex_unlock(&LOCK_active_mi);
  DBUG_RETURN(mi);
}


/**
   Release master info.
   Signals ~Master_info that it's now safe to delete it
*/

void Master_info::release()
{
  mysql_mutex_lock(&sleep_lock);
  if (!--users && killed)
  {
    /* Signal ~Master_info that it's ok to now free it */
    mysql_cond_signal(&sleep_cond);
  }
  mysql_mutex_unlock(&sleep_lock);
}


/**
   Get Master_info for a connection

   @param
   connection_name	Connection name
   warning		WARN_LEVEL_NOTE -> Don't print anything
			WARN_LEVEL_WARN -> Issue warning if not exists
			WARN_LEVEL_ERROR-> Issue error if not exists
*/

Master_info *
Master_info_index::get_master_info(const LEX_STRING *connection_name,
                                   Sql_condition::enum_warning_level warning)
{
  Master_info *mi;
  char buff[MAX_CONNECTION_NAME+1], *res;
  uint buff_length;
  DBUG_ENTER("get_master_info");
  DBUG_PRINT("enter",
             ("connection_name: '%.*s'", (int) connection_name->length,
              connection_name->str));

  /* Make name lower case for comparison */
  res= strmake(buff, connection_name->str, connection_name->length);
  my_casedn_str(system_charset_info, buff); 
  buff_length= (size_t) (res-buff);

  mi= (Master_info*) my_hash_search(&master_info_hash,
                                    (uchar*) buff, buff_length);
  if (!mi && warning != Sql_condition::WARN_LEVEL_NOTE)
  {
    my_error(WARN_NO_MASTER_INFO,
             MYF(warning == Sql_condition::WARN_LEVEL_WARN ? ME_JUST_WARNING :
                 0),
             (int) connection_name->length,
             connection_name->str);
  }
  DBUG_RETURN(mi);
}


/* Check Master_host & Master_port is duplicated or not */
bool Master_info_index::check_duplicate_master_info(LEX_STRING *name_arg,
                                                    const char *host,
                                                    uint port)
{
  Master_info *mi;
  DBUG_ENTER("check_duplicate_master_info");

  mysql_mutex_assert_owner(&LOCK_active_mi);
  DBUG_ASSERT(master_info_index);

  /* Get full host and port name */
  if ((mi= master_info_index->get_master_info(name_arg,
                                              Sql_condition::WARN_LEVEL_NOTE)))
  {
    if (!host)
      host= mi->host;
    if (!port)
      port= mi->port;
  }
  if (!host || !port)
    DBUG_RETURN(FALSE);                         // Not comparable yet

  for (uint i= 0; i < master_info_hash.records; ++i)
  {
    Master_info *tmp_mi;
    tmp_mi= (Master_info *) my_hash_element(&master_info_hash, i);
    if (tmp_mi == mi)
      continue;                                 // Current connection
    if (!strcasecmp(host, tmp_mi->host) && port == tmp_mi->port)
    {
      my_error(ER_CONNECTION_ALREADY_EXISTS, MYF(0),
               (int) name_arg->length,
               name_arg->str,
               (int) tmp_mi->connection_name.length,
               tmp_mi->connection_name.str);
      DBUG_RETURN(TRUE);
    }
  }
  DBUG_RETURN(FALSE);
}


/* Add a Master_info class to Hash Table */
bool Master_info_index::add_master_info(Master_info *mi, bool write_to_file)
{
  /*
    We have to protect against shutdown to ensure we are not calling
    my_hash_insert() while my_hash_free() is in progress
  */
  if (unlikely(shutdown_in_progress) ||
      !my_hash_insert(&master_info_hash, (uchar*) mi))
  {
    if (global_system_variables.log_warnings > 1)
      sql_print_information("Added new Master_info '%.*s' to hash table",
                            (int) mi->connection_name.length,
                            mi->connection_name.str);
    if (write_to_file)
      return write_master_name_to_index_file(&mi->connection_name, 1);
    return FALSE;
  }

  /* Impossible error (EOM) ? */
  sql_print_error("Adding new entry '%.*s' to master_info failed",
                  (int) mi->connection_name.length,
                  mi->connection_name.str);
  return TRUE;
}


/**
   Remove a Master_info class From Hash Table

   TODO: Change this to use my_rename() to make the file name creation
   atomic
*/

bool Master_info_index::remove_master_info(Master_info *mi)
{
  DBUG_ENTER("remove_master_info");
  mysql_mutex_assert_owner(&LOCK_active_mi);

  // Delete Master_info and rewrite others to file
  if (!my_hash_delete(&master_info_hash, (uchar*) mi))
  {
    File index_file_nr;

    // Close IO_CACHE and FILE handler fisrt
    end_io_cache(&index_file);
    my_close(index_file.file, MYF(MY_WME));

    // Reopen File and truncate it
    if ((index_file_nr= my_open(index_file_name,
                                O_RDWR | O_CREAT | O_TRUNC | O_BINARY ,
                                MYF(MY_WME))) < 0 ||
        init_io_cache(&index_file, index_file_nr,
                      IO_SIZE, WRITE_CACHE,
                      my_seek(index_file_nr,0L,MY_SEEK_END,MYF(0)),
                      0, MYF(MY_WME | MY_WAIT_IF_FULL)))
    {
      int error= my_errno;
      if (index_file_nr >= 0)
        my_close(index_file_nr,MYF(0));

      sql_print_error("Create of Master Info Index file '%s' failed with "
                      "error: %M",
                      index_file_name, error);
      DBUG_RETURN(TRUE);
    }

    // Rewrite Master_info.index
    for (uint i= 0; i< master_info_hash.records; ++i)
    {
      Master_info *tmp_mi;
      tmp_mi= (Master_info *) my_hash_element(&master_info_hash, i);
      write_master_name_to_index_file(&tmp_mi->connection_name, 0);
    }
    if (my_sync(index_file_nr, MYF(MY_WME)))
      DBUG_RETURN(TRUE);
  }
  DBUG_RETURN(FALSE);
}


/**
   give_error_if_slave_running()

   @param
   already_locked  0 if we need to lock, 1 if we have LOCK_active_mi_locked  

   @return
   TRUE  	If some slave is running.  An error is printed
   FALSE	No slave is running
*/

bool give_error_if_slave_running(bool already_locked)
{
  bool ret= 0;
  DBUG_ENTER("give_error_if_slave_running");

  if (!already_locked)
    mysql_mutex_lock(&LOCK_active_mi);
  if (!master_info_index)
  {
    my_error(ER_SERVER_SHUTDOWN, MYF(0));
    ret= 1;
  }
  else
  {
    HASH *hash= &master_info_index->master_info_hash;
    for (uint i= 0; i< hash->records; ++i)
    {
      Master_info *mi;
      mi= (Master_info *) my_hash_element(hash, i);
      if (mi->rli.slave_running != MYSQL_SLAVE_NOT_RUN)
      {
        my_error(ER_SLAVE_MUST_STOP, MYF(0), (int) mi->connection_name.length,
                 mi->connection_name.str);
        ret= 1;
        break;
      }
    }
  }
  if (!already_locked)
    mysql_mutex_unlock(&LOCK_active_mi);
  DBUG_RETURN(ret);
}


/**
   any_slave_sql_running()

   @return
   0            No Slave SQL thread is running
   #		Number of slave SQL thread running

   Note that during shutdown we return 1. This is needed to ensure we
   don't try to resize thread pool during shutdown as during shutdown
   master_info_hash may be freeing the hash and during that time
   hash entries can't be accessed.
*/

uint any_slave_sql_running()
{
  uint count= 0;
  HASH *hash;
  DBUG_ENTER("any_slave_sql_running");

  mysql_mutex_lock(&LOCK_active_mi);
  if (unlikely(shutdown_in_progress || !master_info_index))
  {
    mysql_mutex_unlock(&LOCK_active_mi);
    DBUG_RETURN(1);
  }
  hash= &master_info_index->master_info_hash;
  for (uint i= 0; i< hash->records; ++i)
  {
    Master_info *mi= (Master_info *)my_hash_element(hash, i);
    if (mi->rli.slave_running != MYSQL_SLAVE_NOT_RUN)
      count++;
  }
  mysql_mutex_unlock(&LOCK_active_mi);
  DBUG_RETURN(count);
}


/**
   Master_info_index::start_all_slaves()

   Start all slaves that was not running.

   @return
   TRUE  	Error
   FALSE	Everything ok.

   This code is written so that we don't keep LOCK_active_mi active
   while we are starting a slave.
*/

bool Master_info_index::start_all_slaves(THD *thd)
{
  bool result= FALSE;
  DBUG_ENTER("start_all_slaves");
  mysql_mutex_assert_owner(&LOCK_active_mi);

  for (uint i= 0; i< master_info_hash.records; i++)
  {
    Master_info *mi;
    mi= (Master_info *) my_hash_element(&master_info_hash, i);
    mi->in_start_all_slaves= 0;
  }

  for (uint i= 0; i< master_info_hash.records; )
  {
    int error;
    Master_info *mi;
    mi= (Master_info *) my_hash_element(&master_info_hash, i);

    /*
      Try to start all slaves that are configured (host is defined)
      and are not already running
    */
    if (!((mi->slave_running == MYSQL_SLAVE_NOT_RUN ||
           !mi->rli.slave_running) && *mi->host) ||
        mi->in_start_all_slaves)
    {
      i++;
      continue;
    }
    mi->in_start_all_slaves= 1;

    mysql_mutex_lock(&mi->sleep_lock);
    mi->users++;                                // Mark used
    mysql_mutex_unlock(&mi->sleep_lock);
    mysql_mutex_unlock(&LOCK_active_mi);
    error= start_slave(thd, mi, 1);
    mi->release();
    mysql_mutex_lock(&LOCK_active_mi);
    if (error)
    {
      my_error(ER_CANT_START_STOP_SLAVE, MYF(0),
               "START",
               (int) mi->connection_name.length,
               mi->connection_name.str);
      result= 1;
      if (error < 0)                            // fatal error
        break;
    }
    else if (thd)
      push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE,
                          ER_SLAVE_STARTED, ER_THD(thd, ER_SLAVE_STARTED),
                          (int) mi->connection_name.length,
                          mi->connection_name.str);
    /* Restart from first element as master_info_hash may have changed */
    i= 0;
    continue;
  }
  DBUG_RETURN(result);
}


/**
   Master_info_index::stop_all_slaves()

   Start all slaves that was not running.

   @param	thread id from user

   @return
   TRUE  	Error
   FALSE	Everything ok.

   This code is written so that we don't keep LOCK_active_mi active
   while we are stopping a slave.
*/

bool Master_info_index::stop_all_slaves(THD *thd)
{
  bool result= FALSE;
  DBUG_ENTER("stop_all_slaves");
  mysql_mutex_assert_owner(&LOCK_active_mi);
  DBUG_ASSERT(thd);

  for (uint i= 0; i< master_info_hash.records; i++)
  {
    Master_info *mi;
    mi= (Master_info *) my_hash_element(&master_info_hash, i);
    mi->in_stop_all_slaves= 0;
  }

  for (uint i= 0; i< master_info_hash.records ;)
  {
    int error;
    Master_info *mi;
    mi= (Master_info *) my_hash_element(&master_info_hash, i);
    if (!(mi->slave_running != MYSQL_SLAVE_NOT_RUN ||
          mi->rli.slave_running) ||
        mi->in_stop_all_slaves)
    {
      i++;
      continue;
    }
    mi->in_stop_all_slaves= 1;                  // Protection for loops

    mysql_mutex_lock(&mi->sleep_lock);
    mi->users++;                                // Mark used
    mysql_mutex_unlock(&mi->sleep_lock);
    mysql_mutex_unlock(&LOCK_active_mi);
    error= stop_slave(thd, mi, 1);
    mi->release();
    mysql_mutex_lock(&LOCK_active_mi);
    if (error)
    {
      my_error(ER_CANT_START_STOP_SLAVE, MYF(0),
               "STOP",
               (int) mi->connection_name.length,
               mi->connection_name.str);
      result= 1;
      if (error < 0)                            // Fatal error
        break;
    }
    else
      push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE,
                          ER_SLAVE_STOPPED, ER_THD(thd, ER_SLAVE_STOPPED),
                          (int) mi->connection_name.length,
                          mi->connection_name.str);
    /* Restart from first element as master_info_hash may have changed */
    i= 0;
    continue;
  }
  DBUG_RETURN(result);
}

Domain_id_filter::Domain_id_filter() : m_filter(false)
{
  for (int i= DO_DOMAIN_IDS; i <= IGNORE_DOMAIN_IDS; i ++)
  {
    my_init_dynamic_array(&m_domain_ids[i], sizeof(ulong), 16, 16, MYF(0));
  }
}

Domain_id_filter::~Domain_id_filter()
{
  for (int i= DO_DOMAIN_IDS; i <= IGNORE_DOMAIN_IDS; i ++)
  {
    delete_dynamic(&m_domain_ids[i]);
  }
}

/**
  Update m_filter flag for the current group by looking up its domain id in the
  domain ids list. DO_DOMAIN_IDS list is only looked-up is both (do & ignore)
  list are non-empty.
*/
void Domain_id_filter::do_filter(ulong domain_id)
{
  DYNAMIC_ARRAY *do_domain_ids= &m_domain_ids[DO_DOMAIN_IDS];
  DYNAMIC_ARRAY *ignore_domain_ids= &m_domain_ids[IGNORE_DOMAIN_IDS];

  if (do_domain_ids->elements > 0)
  {
    if (likely(do_domain_ids->elements == 1))
      m_filter= ((* (ulong *) dynamic_array_ptr(do_domain_ids, 0))
                 != domain_id);
    else
      m_filter= (bsearch((const ulong *) &domain_id, do_domain_ids->buffer,
                         do_domain_ids->elements, sizeof(ulong),
                         change_master_id_cmp) == NULL);
  }
  else if (ignore_domain_ids->elements > 0)
  {
    if (likely(ignore_domain_ids->elements == 1))
      m_filter= ((* (ulong *) dynamic_array_ptr(ignore_domain_ids, 0)) ==
                 domain_id);
    else
      m_filter= (bsearch((const ulong *) &domain_id, ignore_domain_ids->buffer,
                         ignore_domain_ids->elements, sizeof(ulong),
                         change_master_id_cmp) != NULL);
  }
  return;
}

/**
  Reset m_filter. It should be called when IO thread receives COMMIT_EVENT or
  XID_EVENT.
*/
void Domain_id_filter::reset_filter()
{
  m_filter= false;
}

void Domain_id_filter::clear_ids()
{
  reset_dynamic(&m_domain_ids[DO_DOMAIN_IDS]);
  reset_dynamic(&m_domain_ids[IGNORE_DOMAIN_IDS]);
}

/**
  Update the do/ignore domain id filter lists.

  @param do_ids     [IN]            domain ids to be kept
  @param ignore_ids [IN]            domain ids to be filtered out
  @param using_gtid [IN]            use GTID?

  @retval false                     Success
          true                      Error
*/
bool Domain_id_filter::update_ids(DYNAMIC_ARRAY *do_ids,
                                  DYNAMIC_ARRAY *ignore_ids,
                                  bool using_gtid)
{
  bool do_list_empty, ignore_list_empty;

  if (do_ids)
  {
    do_list_empty= (do_ids->elements > 0) ? false : true;
  } else {
    do_list_empty= (m_domain_ids[DO_DOMAIN_IDS].elements > 0) ? false : true;
  }

  if (ignore_ids)
  {
    ignore_list_empty= (ignore_ids->elements > 0) ? false : true;
  } else {
    ignore_list_empty= (m_domain_ids[IGNORE_DOMAIN_IDS].elements > 0) ? false :
      true;
  }

  if (!do_list_empty && !ignore_list_empty)
  {
    sql_print_error("Both DO_DOMAIN_IDS & IGNORE_DOMAIN_IDS lists can't be "
                    "non-empty at the same time");
    return true;
  }

  if (using_gtid == Master_info::USE_GTID_NO &&
      (!do_list_empty || !ignore_list_empty))
  {
    sql_print_error("DO_DOMAIN_IDS or IGNORE_DOMAIN_IDS lists can't be "
                    "non-empty in non-GTID mode (MASTER_USE_GTID=no)");
    return true;
  }

  if (do_ids)
    update_change_master_ids(do_ids, &m_domain_ids[DO_DOMAIN_IDS]);

  if (ignore_ids)
    update_change_master_ids(ignore_ids, &m_domain_ids[IGNORE_DOMAIN_IDS]);

  m_filter= false;

  return false;
}

/**
  Serialize and store the ids from domain id lists into the thd's protocol
  buffer.

  @param thd [IN]                   thread handler

  @retval void
*/
void Domain_id_filter::store_ids(THD *thd)
{
  for (int i= DO_DOMAIN_IDS; i <= IGNORE_DOMAIN_IDS; i ++)
  {
    prot_store_ids(thd, &m_domain_ids[i]);
  }
}

/**
  Initialize the given domain_id list (DYNAMIC_ARRAY) with the
  space-separated list of numbers from the specified IO_CACHE where
  the first number represents the total number of entries to follows.

  @param f    [IN]                  IO_CACHE file
  @param type [IN]                  domain id list type

  @retval false                     Success
          true                      Error
*/
bool Domain_id_filter::init_ids(IO_CACHE *f, enum_list_type type)
{
  return init_dynarray_intvar_from_file(&m_domain_ids[type], f);
}

/**
  Return the elements of the give domain id list type as string.

  @param type [IN]                  domain id list type

  @retval                           a string buffer storing the total number
                                    of elements followed by the individual
                                    elements (space-separated) in the
                                    specified list.

  Note: Its caller's responsibility to free the returned string buffer.
*/
char *Domain_id_filter::as_string(enum_list_type type)
{
  char *buf;
  size_t sz;
  DYNAMIC_ARRAY *ids= &m_domain_ids[type];

  sz= (sizeof(ulong) * 3 + 1) * (1 + ids->elements);

  if (!(buf= (char *) my_malloc(sz, MYF(MY_WME))))
    return NULL;

  // Store the total number of elements followed by the individual elements.
  ulong cur_len= sprintf(buf, "%u", ids->elements);
  sz-= cur_len;

  for (uint i= 0; i < ids->elements; i++)
  {
    ulong domain_id;
    get_dynamic(ids, (void *) &domain_id, i);
    cur_len+= my_snprintf(buf + cur_len, sz, " %u", domain_id);
    sz-= cur_len;
  }
  return buf;
}

void update_change_master_ids(DYNAMIC_ARRAY *new_ids, DYNAMIC_ARRAY *old_ids)
{
  reset_dynamic(old_ids);

  /* bsearch requires an ordered list. */
  sort_dynamic(new_ids, change_master_id_cmp);

  for (uint i= 0; i < new_ids->elements; i++)
  {
    ulong id;
    get_dynamic(new_ids, (void *) &id, i);

    if (bsearch((const ulong *) &id, old_ids->buffer, old_ids->elements,
                sizeof(ulong), change_master_id_cmp) == NULL)
    {
      insert_dynamic(old_ids, (ulong *) &id);
    }
  }
  return;
}

/**
  Serialize and store the ids from the given ids DYNAMIC_ARRAY into the thd's
  protocol buffer.

  @param thd [IN]                   thread handler
  @param ids [IN]                   ids list

  @retval void
*/

void prot_store_ids(THD *thd, DYNAMIC_ARRAY *ids)
{
  char buff[FN_REFLEN];
  uint i, cur_len;

  for (i= 0, buff[0]= 0, cur_len= 0; i < ids->elements; i++)
  {
    ulong id, len;
    char dbuff[FN_REFLEN];
    get_dynamic(ids, (void *) &id, i);
    len= sprintf(dbuff, (i == 0 ? "%lu" : ", %lu"), id);
    if (cur_len + len + 4 > FN_REFLEN)
    {
      /*
        break the loop whenever remained space could not fit
        ellipses on the next cycle
      */
      sprintf(dbuff + cur_len, "...");
      break;
    }
    cur_len += sprintf(buff + cur_len, "%s", dbuff);
  }
  thd->protocol->store(buff, &my_charset_bin);
  return;
}

bool Master_info_index::flush_all_relay_logs()
{
  DBUG_ENTER("flush_all_relay_logs");
  bool result= false;
  int error= 0;
  mysql_mutex_lock(&LOCK_active_mi);
  for (uint i= 0; i< master_info_hash.records; i++)
  {
    Master_info *mi;
    mi= (Master_info *) my_hash_element(&master_info_hash, i);
    mi->in_flush_all_relay_logs= 0;
  }
  for (uint i=0; i < master_info_hash.records;)
  {
    Master_info *mi;
    mi= (Master_info *)my_hash_element(&master_info_hash, i);
    DBUG_ASSERT(mi);

    if (mi->in_flush_all_relay_logs)
    {
      i++;
      continue;
    }
    mi->in_flush_all_relay_logs= 1;

    mysql_mutex_lock(&mi->sleep_lock);
    mi->users++;                                // Mark used
    mysql_mutex_unlock(&mi->sleep_lock);
    mysql_mutex_unlock(&LOCK_active_mi);

    mysql_mutex_lock(&mi->data_lock);
    error= rotate_relay_log(mi);
    mysql_mutex_unlock(&mi->data_lock);
    mi->release();
    mysql_mutex_lock(&LOCK_active_mi);

    if (error)
    {
      result= true;
      break;
    }
    /* Restart from first element as master_info_hash may have changed */
    i= 0;
    continue;
  }
  mysql_mutex_unlock(&LOCK_active_mi);
  DBUG_RETURN(result);
}

#endif /* HAVE_REPLICATION */
