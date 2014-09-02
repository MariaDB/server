/* Copyright (c) 2006, 2012, Oracle and/or its affiliates.

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

#ifndef RPL_MI_H
#define RPL_MI_H

#ifdef HAVE_REPLICATION

#include "rpl_rli.h"
#include "rpl_reporting.h"
#include "my_sys.h"
#include "rpl_filter.h"
#include "keycaches.h"

typedef struct st_mysql MYSQL;

/*****************************************************************************
  Replication IO Thread

  Master_info contains:
    - information about how to connect to a master
    - current master log name
    - current master log offset
    - misc control variables

  Master_info is initialized once from the master.info file if such
  exists. Otherwise, data members corresponding to master.info fields
  are initialized with defaults specified by master-* options. The
  initialization is done through init_master_info() call.

  The format of master.info file:

  log_name
  log_pos
  master_host
  master_user
  master_pass
  master_port
  master_connect_retry

  To write out the contents of master.info file to disk ( needed every
  time we read and queue data from the master ), a call to
  flush_master_info() is required.

  To clean up, call end_master_info()

*****************************************************************************/

class Master_info : public Slave_reporting_capability
{
 public:
  enum enum_using_gtid {
    USE_GTID_NO= 0, USE_GTID_CURRENT_POS= 1, USE_GTID_SLAVE_POS= 2
  };

  Master_info(LEX_STRING *connection_name, bool is_slave_recovery);
  ~Master_info();
  bool shall_ignore_server_id(ulong s_id);
  void clear_in_memory_info(bool all);
  bool error()
  {
    /* If malloc() in initialization failed */
    return connection_name.str == 0;
  }
  static const char *using_gtid_astext(enum enum_using_gtid arg);

  /* the variables below are needed because we can change masters on the fly */
  char master_log_name[FN_REFLEN+6]; /* Room for multi-*/
  char host[HOSTNAME_LENGTH*SYSTEM_CHARSET_MBMAXLEN+1];
  char user[USERNAME_LENGTH+1];
  char password[MAX_PASSWORD_LENGTH*SYSTEM_CHARSET_MBMAXLEN+1];
  LEX_STRING connection_name;  		/* User supplied connection name */
  LEX_STRING cmp_connection_name;	/* Connection name in lower case */
  bool ssl; // enables use of SSL connection if true
  char ssl_ca[FN_REFLEN], ssl_capath[FN_REFLEN], ssl_cert[FN_REFLEN];
  char ssl_cipher[FN_REFLEN], ssl_key[FN_REFLEN];
  char ssl_crl[FN_REFLEN], ssl_crlpath[FN_REFLEN];
  bool ssl_verify_server_cert;

  my_off_t master_log_pos;
  File fd; // we keep the file open, so we need to remember the file pointer
  IO_CACHE file;

  mysql_mutex_t data_lock, run_lock, sleep_lock;
  mysql_cond_t data_cond, start_cond, stop_cond, sleep_cond;
  THD *io_thd;
  MYSQL* mysql;
  uint32 file_id;				/* for 3.23 load data infile */
  Relay_log_info rli;
  uint port;
  Rpl_filter* rpl_filter;      /* Each replication can set its filter rule*/
  /*
    to hold checksum alg in use until IO thread has received FD.
    Initialized to novalue, then set to the queried from master
    @@global.binlog_checksum and deactivated once FD has been received.
  */
  uint8 checksum_alg_before_fd;
  uint connect_retry;
#ifndef DBUG_OFF
  int events_till_disconnect;
#endif
  bool inited;
  volatile bool abort_slave;
  volatile uint slave_running;
  volatile ulong slave_run_id;
  /*
     The difference in seconds between the clock of the master and the clock of
     the slave (second - first). It must be signed as it may be <0 or >0.
     clock_diff_with_master is computed when the I/O thread starts; for this the
     I/O thread does a SELECT UNIX_TIMESTAMP() on the master.
     "how late the slave is compared to the master" is computed like this:
     clock_of_slave - last_timestamp_executed_by_SQL_thread - clock_diff_with_master

  */
  long clock_diff_with_master;
  /*
    Keeps track of the number of events before fsyncing.
    The option --sync-master-info determines how many
    events should happen before fsyncing.
  */
  uint sync_counter;
  float heartbeat_period;         // interface with CHANGE MASTER or master.info
  ulonglong received_heartbeats;  // counter of received heartbeat events
  DYNAMIC_ARRAY ignore_server_ids;
  ulong master_id;
  /*
    At reconnect and until the first rotate event is seen, prev_master_id is
    the value of master_id during the previous connection, used to detect
    silent change of master server during reconnects.
  */
  ulong prev_master_id;
  /*
    Which kind of GTID position (if any) is used when connecting to master.

    Note that you can not change the numeric values of these, they are used
    in master.info.
  */
  enum enum_using_gtid using_gtid;

  /*
    This GTID position records how far we have fetched into the relay logs.
    This is used to continue fetching when the IO thread reconnects to the
    master.

    (Full slave stop/start does not use it, as it resets the relay logs).
  */
  slave_connection_state gtid_current_pos;
  /*
    If events_queued_since_last_gtid is non-zero, it is the number of events
    queued so far in the relaylog of a GTID-prefixed event group.
    It is zero when no partial event group has been queued at the moment.
  */
  uint64 events_queued_since_last_gtid;
  /*
    The GTID of the partially-queued event group, when
    events_queued_since_last_gtid is non-zero.
  */
  rpl_gtid last_queued_gtid;
  /* Whether last_queued_gtid had the FL_STANDALONE flag set. */
  bool last_queued_gtid_standalone;
  /*
    When slave IO thread needs to reconnect, gtid_reconnect_event_skip_count
    counts number of events to skip from the first GTID-prefixed event group,
    to avoid duplicating events in the relay log.
  */
  uint64 gtid_reconnect_event_skip_count;
  /* gtid_event_seen is false until we receive first GTID event from master. */
  bool gtid_event_seen;
};
int init_master_info(Master_info* mi, const char* master_info_fname,
		     const char* slave_info_fname,
		     bool abort_if_no_master_info_file,
		     int thread_mask);
void end_master_info(Master_info* mi);
int flush_master_info(Master_info* mi, 
                      bool flush_relay_log_cache, 
                      bool need_lock_relay_log);
int change_master_server_id_cmp(ulong *id1, ulong *id2);
void copy_filter_setting(Rpl_filter* dst_filter, Rpl_filter* src_filter);

/*
  Multi master are handled trough this struct.
  Changes to this needs to be protected by LOCK_active_mi;
*/

class Master_info_index
{
private:
  IO_CACHE index_file;
  char index_file_name[FN_REFLEN];

public:
  Master_info_index();
  ~Master_info_index();

  HASH master_info_hash;

  bool init_all_master_info();
  bool write_master_name_to_index_file(LEX_STRING *connection_name,
                                       bool do_sync);

  bool check_duplicate_master_info(LEX_STRING *connection_name,
                                   const char *host, uint port);
  bool add_master_info(Master_info *mi, bool write_to_file);
  bool remove_master_info(LEX_STRING *connection_name);
  Master_info *get_master_info(LEX_STRING *connection_name,
                               Sql_condition::enum_warning_level warning);
  bool give_error_if_slave_running();
  bool start_all_slaves(THD *thd);
  bool stop_all_slaves(THD *thd);
};


/*
  The class rpl_io_thread_info is the THD::system_thread_info for the IO thread.
*/
class rpl_io_thread_info
{
public:
};


bool check_master_connection_name(LEX_STRING *name);
void create_logfile_name_with_suffix(char *res_file_name, size_t length,
                             const char *info_file, 
                             bool append,
                             LEX_STRING *suffix);

uchar *get_key_master_info(Master_info *mi, size_t *length,
                           my_bool not_used __attribute__((unused)));
void free_key_master_info(Master_info *mi);


#endif /* HAVE_REPLICATION */
#endif /* RPL_MI_H */
