/* Copyright (c) 2000, 2013, Oracle and/or its affiliates.
   Copyright (c) 2008, 2014, SkySQL Ab.

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

#include <my_global.h>
#include "sql_priv.h"
#include "unireg.h"
#include "sql_base.h"
#include "sql_parse.h"                          // check_access
#ifdef HAVE_REPLICATION

#include "rpl_mi.h"
#include "rpl_rli.h"
#include "sql_repl.h"
#include "sql_acl.h"                            // SUPER_ACL
#include "log_event.h"
#include "rpl_filter.h"
#include <my_dir.h>
#include "rpl_handler.h"
#include "debug_sync.h"


enum enum_gtid_until_state {
  GTID_UNTIL_NOT_DONE,
  GTID_UNTIL_STOP_AFTER_STANDALONE,
  GTID_UNTIL_STOP_AFTER_TRANSACTION
};


int max_binlog_dump_events = 0; // unlimited
my_bool opt_sporadic_binlog_dump_fail = 0;
#ifndef DBUG_OFF
static int binlog_dump_count = 0;
#endif

extern TYPELIB binlog_checksum_typelib;


static int
fake_event_header(String* packet, Log_event_type event_type, ulong extra_len,
                  my_bool *do_checksum, ha_checksum *crc, const char** errmsg,
                  enum enum_binlog_checksum_alg checksum_alg_arg, uint32 end_pos)
{
  char header[LOG_EVENT_HEADER_LEN];
  ulong event_len;

  *do_checksum= checksum_alg_arg != BINLOG_CHECKSUM_ALG_OFF &&
    checksum_alg_arg != BINLOG_CHECKSUM_ALG_UNDEF;

  /*
    'when' (the timestamp) is set to 0 so that slave could distinguish between
    real and fake Rotate events (if necessary)
  */
  memset(header, 0, 4);
  header[EVENT_TYPE_OFFSET] = (uchar)event_type;
  event_len=  LOG_EVENT_HEADER_LEN + extra_len +
    (*do_checksum ? BINLOG_CHECKSUM_LEN : 0);
  int4store(header + SERVER_ID_OFFSET, global_system_variables.server_id);
  int4store(header + EVENT_LEN_OFFSET, event_len);
  int2store(header + FLAGS_OFFSET, LOG_EVENT_ARTIFICIAL_F);
  // TODO: check what problems this may cause and fix them
  int4store(header + LOG_POS_OFFSET, end_pos);
  if (packet->append(header, sizeof(header)))
  {
    *errmsg= "Failed due to out-of-memory writing event";
    return -1;
  }
  if (*do_checksum)
  {
    *crc= my_checksum(0, (uchar*)header, sizeof(header));
  }
  return 0;
}


static int
fake_event_footer(String *packet, my_bool do_checksum, ha_checksum crc, const char **errmsg)
{
  if (do_checksum)
  {
    char b[BINLOG_CHECKSUM_LEN];
    int4store(b, crc);
    if (packet->append(b, sizeof(b)))
    {
      *errmsg= "Failed due to out-of-memory writing event checksum";
      return -1;
    }
  }
  return 0;
}


static int
fake_event_write(NET *net, String *packet, const char **errmsg)
{
  if (my_net_write(net, (uchar*) packet->ptr(), packet->length()))
  {
    *errmsg = "failed on my_net_write()";
    return -1;
  }
  return 0;
}


/*
  Helper structure, used to pass miscellaneous info from mysql_binlog_send()
  into the helper functions that it calls.
*/
struct binlog_send_info {
  rpl_binlog_state until_binlog_state;
  slave_connection_state gtid_state;
  THD *thd;
  NET *net;
  String *packet;
  char *const log_file_name; // ptr/alias to linfo.log_file_name
  slave_connection_state *until_gtid_state;
  slave_connection_state until_gtid_state_obj;
  Format_description_log_event *fdev;
  int mariadb_slave_capability;
  enum_gtid_skip_type gtid_skip_group;
  enum_gtid_until_state gtid_until_group;
  ushort flags;
  enum enum_binlog_checksum_alg current_checksum_alg;
  bool slave_gtid_strict_mode;
  bool send_fake_gtid_list;
  bool slave_gtid_ignore_duplicates;
  bool using_gtid_state;

  int error;
  const char *errmsg;
  char error_text[MAX_SLAVE_ERRMSG];
  rpl_gtid error_gtid;

  ulonglong heartbeat_period;

  /** start file/pos as requested by slave, for error message */
  char start_log_file_name[FN_REFLEN];
  my_off_t start_pos;

  /** last pos for error message */
  my_off_t last_pos;

#ifndef DBUG_OFF
  int left_events;
  uint dbug_reconnect_counter;
  ulong hb_info_counter;
#endif

  bool clear_initial_log_pos;
  bool should_stop;

  binlog_send_info(THD *thd_arg, String *packet_arg, ushort flags_arg,
                   char *lfn)
    : thd(thd_arg), net(&thd_arg->net), packet(packet_arg),
      log_file_name(lfn), until_gtid_state(NULL), fdev(NULL),
      gtid_skip_group(GTID_SKIP_NOT), gtid_until_group(GTID_UNTIL_NOT_DONE),
      flags(flags_arg), current_checksum_alg(BINLOG_CHECKSUM_ALG_UNDEF),
      slave_gtid_strict_mode(false), send_fake_gtid_list(false),
      slave_gtid_ignore_duplicates(false),
      error(0),
      errmsg("Unknown error"),
      heartbeat_period(0),
#ifndef DBUG_OFF
      left_events(max_binlog_dump_events),
      dbug_reconnect_counter(0),
      hb_info_counter(0),
#endif
      clear_initial_log_pos(false),
      should_stop(false)
  {
    error_text[0] = 0;
    bzero(&error_gtid, sizeof(error_gtid));
  }
};

// prototype
static int reset_transmit_packet(struct binlog_send_info *info, ushort flags,
                                 ulong *ev_offset, const char **errmsg);

/*
    fake_rotate_event() builds a fake (=which does not exist physically in any
    binlog) Rotate event, which contains the name of the binlog we are going to
    send to the slave (because the slave may not know it if it just asked for
    MASTER_LOG_FILE='', MASTER_LOG_POS=4).
    < 4.0.14, fake_rotate_event() was called only if the requested pos was 4.
    After this version we always call it, so that a 3.23.58 slave can rely on
    it to detect if the master is 4.0 (and stop) (the _fake_ Rotate event has
    zeros in the good positions which, by chance, make it possible for the 3.23
    slave to detect that this event is unexpected) (this is luck which happens
    because the master and slave disagree on the size of the header of
    Log_event).

    Relying on the event length of the Rotate event instead of these
    well-placed zeros was not possible as Rotate events have a variable-length
    part.
*/

static int fake_rotate_event(binlog_send_info *info, ulonglong position,
                             const char** errmsg, enum enum_binlog_checksum_alg checksum_alg_arg)
{
  DBUG_ENTER("fake_rotate_event");
  ulong ev_offset;
  char buf[ROTATE_HEADER_LEN+100];
  my_bool do_checksum;
  int err;
  char* p = info->log_file_name+dirname_length(info->log_file_name);
  uint ident_len = (uint) strlen(p);
  String *packet= info->packet;
  ha_checksum crc;

  /* reset transmit packet for the fake rotate event below */
  if (reset_transmit_packet(info, info->flags, &ev_offset, &info->errmsg))
    DBUG_RETURN(1);

  if ((err= fake_event_header(packet, ROTATE_EVENT,
                              ident_len + ROTATE_HEADER_LEN, &do_checksum,
                              &crc,
                              errmsg, checksum_alg_arg, 0)))
  {
    info->error= ER_UNKNOWN_ERROR;
    DBUG_RETURN(err);
  }

  int8store(buf+R_POS_OFFSET,position);
  packet->append(buf, ROTATE_HEADER_LEN);
  packet->append(p, ident_len);

  if (do_checksum)
  {
    crc= my_checksum(crc, (uchar*)buf, ROTATE_HEADER_LEN);
    crc= my_checksum(crc, (uchar*)p, ident_len);
  }

  if ((err= fake_event_footer(packet, do_checksum, crc, errmsg)) ||
      (err= fake_event_write(info->net, packet, errmsg)))
  {
    info->error= ER_UNKNOWN_ERROR;
    DBUG_RETURN(err);
  }
  DBUG_RETURN(0);
}


static int fake_gtid_list_event(binlog_send_info *info,
                                Gtid_list_log_event *glev, const char** errmsg,
                                uint32 current_pos)
{
  my_bool do_checksum;
  int err;
  ha_checksum crc;
  char buf[128];
  String str(buf, sizeof(buf), system_charset_info);
  String* packet= info->packet;

  str.length(0);
  if (glev->to_packet(&str))
  {
    info->error= ER_UNKNOWN_ERROR;
    *errmsg= "Failed due to out-of-memory writing Gtid_list event";
    return -1;
  }
  if ((err= fake_event_header(packet, GTID_LIST_EVENT,
                              str.length(), &do_checksum, &crc,
                              errmsg, info->current_checksum_alg, current_pos)))
  {
    info->error= ER_UNKNOWN_ERROR;
    return err;
  }

  packet->append(str);
  if (do_checksum)
  {
    crc= my_checksum(crc, (uchar*)str.ptr(), str.length());
  }

  if ((err= fake_event_footer(packet, do_checksum, crc, errmsg)) ||
      (err= fake_event_write(info->net, packet, errmsg)))
  {
    info->error= ER_UNKNOWN_ERROR;
    return err;
  }

  return 0;
}


/*
  Reset thread transmit packet buffer for event sending

  This function allocates header bytes for event transmission, and
  should be called before store the event data to the packet buffer.
*/
static int reset_transmit_packet(binlog_send_info *info, ushort flags,
                                 ulong *ev_offset, const char **errmsg)
{
  int ret= 0;
  String *packet= &info->thd->packet;

  /* reserve and set default header */
  packet->length(0);
  packet->set("\0", 1, &my_charset_bin);

  if (RUN_HOOK(binlog_transmit, reserve_header, (info->thd, flags, packet)))
  {
    info->error= ER_UNKNOWN_ERROR;
    *errmsg= "Failed to run hook 'reserve_header'";
    ret= 1;
  }
  *ev_offset= packet->length();
  return ret;
}

static int send_file(THD *thd)
{
  NET* net = &thd->net;
  int fd = -1, error = 1;
  size_t bytes;
  char fname[FN_REFLEN+1];
  const char *errmsg = 0;
  int old_timeout;
  unsigned long packet_len;
  uchar buf[IO_SIZE];				// It's safe to alloc this
  DBUG_ENTER("send_file");

  /*
    The client might be slow loading the data, give him wait_timeout to do
    the job
  */
  old_timeout= net->read_timeout;
  my_net_set_read_timeout(net, thd->variables.net_wait_timeout);

  /*
    We need net_flush here because the client will not know it needs to send
    us the file name until it has processed the load event entry
  */
  if (net_flush(net) || (packet_len = my_net_read(net)) == packet_error)
  {
    errmsg = "while reading file name";
    goto err;
  }

  // terminate with \0 for fn_format
  *((char*)net->read_pos +  packet_len) = 0;
  fn_format(fname, (char*) net->read_pos + 1, "", "", 4);
  // this is needed to make replicate-ignore-db
  if (!strcmp(fname,"/dev/null"))
    goto end;

  if ((fd= mysql_file_open(key_file_send_file,
                           fname, O_RDONLY, MYF(0))) < 0)
  {
    errmsg = "on open of file";
    goto err;
  }

  while ((long) (bytes= mysql_file_read(fd, buf, IO_SIZE, MYF(0))) > 0)
  {
    if (my_net_write(net, buf, bytes))
    {
      errmsg = "while writing data to client";
      goto err;
    }
  }

 end:
  if (my_net_write(net, (uchar*) "", 0) || net_flush(net) ||
      (my_net_read(net) == packet_error))
  {
    errmsg = "while negotiating file transfer close";
    goto err;
  }
  error = 0;

 err:
  my_net_set_read_timeout(net, old_timeout);
  if (fd >= 0)
    mysql_file_close(fd, MYF(0));
  if (errmsg)
  {
    sql_print_error("Failed in send_file() %s", errmsg);
    DBUG_PRINT("error", ("%s", errmsg));
  }
  DBUG_RETURN(error);
}


/**
   Internal to mysql_binlog_send() routine that recalculates checksum for
   a FD event (asserted) that needs additional arranment prior sending to slave.
*/
inline void fix_checksum(String *packet, ulong ev_offset)
{
  /* recalculate the crc for this event */
  uint data_len = uint4korr(packet->ptr() + ev_offset + EVENT_LEN_OFFSET);
  ha_checksum crc;
  DBUG_ASSERT(data_len == 
              LOG_EVENT_MINIMAL_HEADER_LEN + FORMAT_DESCRIPTION_HEADER_LEN +
              BINLOG_CHECKSUM_ALG_DESC_LEN + BINLOG_CHECKSUM_LEN);
  crc= my_checksum(0, (uchar *)packet->ptr() + ev_offset, data_len -
                   BINLOG_CHECKSUM_LEN);
  int4store(packet->ptr() + ev_offset + data_len - BINLOG_CHECKSUM_LEN, crc);
}


static user_var_entry * get_binlog_checksum_uservar(THD * thd)
{
  LEX_STRING name=  { C_STRING_WITH_LEN("master_binlog_checksum")};
  user_var_entry *entry= 
    (user_var_entry*) my_hash_search(&thd->user_vars, (uchar*) name.str,
                                  name.length);
  return entry;
}

/**
  Function for calling in mysql_binlog_send
  to check if slave initiated checksum-handshake.

  @param[in]    thd  THD to access a user variable

  @return        TRUE if handshake took place, FALSE otherwise
*/

static bool is_slave_checksum_aware(THD * thd)
{
  DBUG_ENTER("is_slave_checksum_aware");
  user_var_entry *entry= get_binlog_checksum_uservar(thd);
  DBUG_RETURN(entry? true  : false);
}

/**
  Function for calling in mysql_binlog_send
  to get the value of @@binlog_checksum of the master at
  time of checksum-handshake.

  The value tells the master whether to compute or not, and the slave
  to verify or not the first artificial Rotate event's checksum.

  @param[in]    thd  THD to access a user variable

  @return       value of @@binlog_checksum alg according to
                @c enum enum_binlog_checksum_alg
*/

static enum enum_binlog_checksum_alg get_binlog_checksum_value_at_connect(THD * thd)
{
  enum enum_binlog_checksum_alg ret;

  DBUG_ENTER("get_binlog_checksum_value_at_connect");
  user_var_entry *entry= get_binlog_checksum_uservar(thd);
  if (!entry)
  {
    ret= BINLOG_CHECKSUM_ALG_UNDEF;
  }
  else
  {
    DBUG_ASSERT(entry->type == STRING_RESULT);
    String str;
    uint dummy_errors;
    str.copy(entry->value, entry->length, &my_charset_bin, &my_charset_bin,
             &dummy_errors);
    ret= (enum_binlog_checksum_alg)
      (find_type ((char*) str.ptr(), &binlog_checksum_typelib, 1) - 1);
    DBUG_ASSERT(ret <= BINLOG_CHECKSUM_ALG_CRC32); // while it's just on CRC32 alg
  }
  DBUG_RETURN(ret);
}

/*
  Adjust the position pointer in the binary log file for all running slaves

  SYNOPSIS
    adjust_linfo_offsets()
    purge_offset	Number of bytes removed from start of log index file

  NOTES
    - This is called when doing a PURGE when we delete lines from the
      index log file

  REQUIREMENTS
    - Before calling this function, we have to ensure that no threads are
      using any binary log file before purge_offset.a

  TODO
    - Inform the slave threads that they should sync the position
      in the binary log file with flush_relay_log_info.
      Now they sync is done for next read.
*/

void adjust_linfo_offsets(my_off_t purge_offset)
{
  THD *tmp;

  mysql_mutex_lock(&LOCK_thread_count);
  I_List_iterator<THD> it(threads);

  while ((tmp=it++))
  {
    LOG_INFO* linfo;
    if ((linfo = tmp->current_linfo))
    {
      mysql_mutex_lock(&linfo->lock);
      /*
	Index file offset can be less that purge offset only if
	we just started reading the index file. In that case
	we have nothing to adjust
      */
      if (linfo->index_file_offset < purge_offset)
	linfo->fatal = (linfo->index_file_offset != 0);
      else
	linfo->index_file_offset -= purge_offset;
      mysql_mutex_unlock(&linfo->lock);
    }
  }
  mysql_mutex_unlock(&LOCK_thread_count);
}


bool log_in_use(const char* log_name)
{
  size_t log_name_len = strlen(log_name) + 1;
  THD *tmp;
  bool result = 0;

  mysql_mutex_lock(&LOCK_thread_count);
  I_List_iterator<THD> it(threads);

  while ((tmp=it++))
  {
    LOG_INFO* linfo;
    if ((linfo = tmp->current_linfo))
    {
      mysql_mutex_lock(&linfo->lock);
      result = !memcmp(log_name, linfo->log_file_name, log_name_len);
      mysql_mutex_unlock(&linfo->lock);
      if (result)
	break;
    }
  }

  mysql_mutex_unlock(&LOCK_thread_count);
  return result;
}

bool purge_error_message(THD* thd, int res)
{
  uint errcode;

  if ((errcode= purge_log_get_error_code(res)) != 0)
  {
    my_message(errcode, ER_THD(thd, errcode), MYF(0));
    return TRUE;
  }
  my_ok(thd);
  return FALSE;
}


/**
  Execute a PURGE BINARY LOGS TO <log> command.

  @param thd Pointer to THD object for the client thread executing the
  statement.

  @param to_log Name of the last log to purge.

  @retval FALSE success
  @retval TRUE failure
*/
bool purge_master_logs(THD* thd, const char* to_log)
{
  char search_file_name[FN_REFLEN];
  if (!mysql_bin_log.is_open())
  {
    my_ok(thd);
    return FALSE;
  }

  mysql_bin_log.make_log_name(search_file_name, to_log);
  return purge_error_message(thd,
			     mysql_bin_log.purge_logs(search_file_name, 0, 1,
						      1, NULL));
}


/**
  Execute a PURGE BINARY LOGS BEFORE <date> command.

  @param thd Pointer to THD object for the client thread executing the
  statement.

  @param purge_time Date before which logs should be purged.

  @retval FALSE success
  @retval TRUE failure
*/
bool purge_master_logs_before_date(THD* thd, time_t purge_time)
{
  if (!mysql_bin_log.is_open())
  {
    my_ok(thd);
    return 0;
  }
  return purge_error_message(thd,
                             mysql_bin_log.purge_logs_before_date(purge_time));
}

void set_read_error(binlog_send_info *info, int error)
{
  if (error == LOG_READ_EOF)
  {
    return;
  }
  info->error= ER_MASTER_FATAL_ERROR_READING_BINLOG;
  switch (error) {
  case LOG_READ_BOGUS:
    info->errmsg= "bogus data in log event";
    break;
  case LOG_READ_TOO_LARGE:
    info->errmsg= "log event entry exceeded max_allowed_packet; "
        "Increase max_allowed_packet on master";
    break;
  case LOG_READ_IO:
    info->errmsg= "I/O error reading log event";
    break;
  case LOG_READ_MEM:
    info->errmsg= "memory allocation failed reading log event";
    break;
  case LOG_READ_TRUNC:
    info->errmsg= "binlog truncated in the middle of event; "
        "consider out of disk space on master";
    break;
  case LOG_READ_CHECKSUM_FAILURE:
    info->errmsg= "event read from binlog did not pass crc check";
    break;
  case LOG_READ_DECRYPT:
    info->errmsg= "event decryption failure";
    break;
  default:
    info->errmsg= "unknown error reading log event on the master";
    break;
  }
}


/**
  An auxiliary function for calling in mysql_binlog_send
  to initialize the heartbeat timeout in waiting for a binlogged event.

  @param[in]    thd  THD to access a user variable

  @return        heartbeat period an ulonglong of nanoseconds
                 or zero if heartbeat was not demanded by slave
*/ 
static ulonglong get_heartbeat_period(THD * thd)
{
  bool null_value;
  LEX_STRING name=  { C_STRING_WITH_LEN("master_heartbeat_period")};
  user_var_entry *entry= 
    (user_var_entry*) my_hash_search(&thd->user_vars, (uchar*) name.str,
                                  name.length);
  return entry? entry->val_int(&null_value) : 0;
}

/*
  Lookup the capabilities of the slave, which it announces by setting a value
  MARIA_SLAVE_CAPABILITY_XXX in @mariadb_slave_capability.

  Older MariaDB slaves, and other MySQL slaves, do not set
  @mariadb_slave_capability, corresponding to a capability of
  MARIA_SLAVE_CAPABILITY_UNKNOWN (0).
*/
static int
get_mariadb_slave_capability(THD *thd)
{
  bool null_value;
  const LEX_STRING name= { C_STRING_WITH_LEN("mariadb_slave_capability") };
  const user_var_entry *entry=
    (user_var_entry*) my_hash_search(&thd->user_vars, (uchar*) name.str,
                                  name.length);
  return entry ?
    (int)(entry->val_int(&null_value)) : MARIA_SLAVE_CAPABILITY_UNKNOWN;
}


/*
  Get the value of the @slave_connect_state user variable into the supplied
  String (this is the GTID connect state requested by the connecting slave).

  Returns false if error (ie. slave did not set the variable and does not
  want to use GTID to set start position), true if success.
*/
static bool
get_slave_connect_state(THD *thd, String *out_str)
{
  bool null_value;

  const LEX_STRING name= { C_STRING_WITH_LEN("slave_connect_state") };
  user_var_entry *entry=
    (user_var_entry*) my_hash_search(&thd->user_vars, (uchar*) name.str,
                                  name.length);
  return entry && entry->val_str(&null_value, out_str, 0) && !null_value;
}


static bool
get_slave_gtid_strict_mode(THD *thd)
{
  bool null_value;

  const LEX_STRING name= { C_STRING_WITH_LEN("slave_gtid_strict_mode") };
  user_var_entry *entry=
    (user_var_entry*) my_hash_search(&thd->user_vars, (uchar*) name.str,
                                  name.length);
  return entry && entry->val_int(&null_value) && !null_value;
}


static bool
get_slave_gtid_ignore_duplicates(THD *thd)
{
  bool null_value;

  const LEX_STRING name= { C_STRING_WITH_LEN("slave_gtid_ignore_duplicates") };
  user_var_entry *entry=
    (user_var_entry*) my_hash_search(&thd->user_vars, (uchar*) name.str,
                                     name.length);
  return entry && entry->val_int(&null_value) && !null_value;
}


/*
  Get the value of the @slave_until_gtid user variable into the supplied
  String (this is the GTID position specified for START SLAVE UNTIL
  master_gtid_pos='xxx').

  Returns false if error (ie. slave did not set the variable and is not doing
  START SLAVE UNTIL mater_gtid_pos='xxx'), true if success.
*/
static bool
get_slave_until_gtid(THD *thd, String *out_str)
{
  bool null_value;

  const LEX_STRING name= { C_STRING_WITH_LEN("slave_until_gtid") };
  user_var_entry *entry=
    (user_var_entry*) my_hash_search(&thd->user_vars, (uchar*) name.str,
                                  name.length);
  return entry && entry->val_str(&null_value, out_str, 0) && !null_value;
}


/*
  Function prepares and sends repliation heartbeat event.

  @param net                net object of THD
  @param packet             buffer to store the heartbeat instance
  @param event_coordinates  binlog file name and position of the last
                            real event master sent from binlog

  @note 
    Among three essential pieces of heartbeat data Log_event::when
    is computed locally.
    The  error to send is serious and should force terminating
    the dump thread.
*/
static int send_heartbeat_event(binlog_send_info *info,
                                NET* net, String* packet,
                                const struct event_coordinates *coord,
                                enum enum_binlog_checksum_alg checksum_alg_arg)
{
  DBUG_ENTER("send_heartbeat_event");

  ulong ev_offset;
  if (reset_transmit_packet(info, info->flags, &ev_offset, &info->errmsg))
    DBUG_RETURN(1);

  char header[LOG_EVENT_HEADER_LEN];
  my_bool do_checksum= checksum_alg_arg != BINLOG_CHECKSUM_ALG_OFF &&
    checksum_alg_arg != BINLOG_CHECKSUM_ALG_UNDEF;
  /*
    'when' (the timestamp) is set to 0 so that slave could distinguish between
    real and fake Rotate events (if necessary)
  */
  memset(header, 0, 4);  // when

  header[EVENT_TYPE_OFFSET] = HEARTBEAT_LOG_EVENT;

  char* p= coord->file_name + dirname_length(coord->file_name);

  uint ident_len = strlen(p);
  ulong event_len = ident_len + LOG_EVENT_HEADER_LEN +
    (do_checksum ? BINLOG_CHECKSUM_LEN : 0);
  int4store(header + SERVER_ID_OFFSET, global_system_variables.server_id);
  int4store(header + EVENT_LEN_OFFSET, event_len);
  int2store(header + FLAGS_OFFSET, 0);

  int4store(header + LOG_POS_OFFSET, coord->pos);  // log_pos

  packet->append(header, sizeof(header));
  packet->append(p, ident_len);             // log_file_name

  if (do_checksum)
  {
    char b[BINLOG_CHECKSUM_LEN];
    ha_checksum crc= my_checksum(0, (uchar*) header, sizeof(header));
    crc= my_checksum(crc, (uchar*) p, ident_len);
    int4store(b, crc);
    packet->append(b, sizeof(b));
  }

  if (my_net_write(net, (uchar*) packet->ptr(), packet->length()) ||
      net_flush(net))
  {
    info->error= ER_UNKNOWN_ERROR;
    DBUG_RETURN(-1);
  }

  DBUG_RETURN(0);
}


struct binlog_file_entry
{
  binlog_file_entry *next;
  char *name;
};

static binlog_file_entry *
get_binlog_list(MEM_ROOT *memroot)
{
  IO_CACHE *index_file;
  char fname[FN_REFLEN];
  size_t length;
  binlog_file_entry *current_list= NULL, *e;
  DBUG_ENTER("get_binlog_list");

  if (!mysql_bin_log.is_open())
  {
    my_error(ER_NO_BINARY_LOGGING, MYF(0));
    DBUG_RETURN(NULL);
  }

  mysql_bin_log.lock_index();
  index_file=mysql_bin_log.get_index_file();
  reinit_io_cache(index_file, READ_CACHE, (my_off_t) 0, 0, 0);

  /* The file ends with EOF or empty line */
  while ((length=my_b_gets(index_file, fname, sizeof(fname))) > 1)
  {
    --length;                                   /* Remove the newline */
    if (!(e= (binlog_file_entry *)alloc_root(memroot, sizeof(*e))) ||
        !(e->name= strmake_root(memroot, fname, length)))
    {
      mysql_bin_log.unlock_index();
      my_error(ER_OUTOFMEMORY, MYF(0), length + 1 + sizeof(*e));
      DBUG_RETURN(NULL);
    }
    e->next= current_list;
    current_list= e;
  }
  mysql_bin_log.unlock_index();

  DBUG_RETURN(current_list);
}

/*
  Find the Gtid_list_log_event at the start of a binlog.

  NULL for ok, non-NULL error message for error.

  If ok, then the event is returned in *out_gtid_list. This can be NULL if we
  get back to binlogs written by old server version without GTID support. If
  so, it means we have reached the point to start from, as no GTID events can
  exist in earlier binlogs.
*/
static const char *
get_gtid_list_event(IO_CACHE *cache, Gtid_list_log_event **out_gtid_list)
{
  Format_description_log_event init_fdle(BINLOG_VERSION);
  Format_description_log_event *fdle;
  Log_event *ev;
  const char *errormsg = NULL;

  *out_gtid_list= NULL;

  if (!(ev= Log_event::read_log_event(cache, 0, &init_fdle,
                                      opt_master_verify_checksum)) ||
      ev->get_type_code() != FORMAT_DESCRIPTION_EVENT)
  {
    if (ev)
      delete ev;
    return "Could not read format description log event while looking for "
      "GTID position in binlog";
  }

  fdle= static_cast<Format_description_log_event *>(ev);

  for (;;)
  {
    Log_event_type typ;

    ev= Log_event::read_log_event(cache, 0, fdle, opt_master_verify_checksum);
    if (!ev)
    {
      errormsg= "Could not read GTID list event while looking for GTID "
        "position in binlog";
      break;
    }
    typ= ev->get_type_code();
    if (typ == GTID_LIST_EVENT)
      break;                                    /* Done, found it */
    if (typ == START_ENCRYPTION_EVENT)
    {
      if (fdle->start_decryption((Start_encryption_log_event*) ev))
        errormsg= "Could not set up decryption for binlog.";
    }
    delete ev;
    if (typ == ROTATE_EVENT || typ == STOP_EVENT ||
        typ == FORMAT_DESCRIPTION_EVENT || typ == START_ENCRYPTION_EVENT)
      continue;                                 /* Continue looking */

    /* We did not find any Gtid_list_log_event, must be old binlog. */
    ev= NULL;
    break;
  }

  delete fdle;
  *out_gtid_list= static_cast<Gtid_list_log_event *>(ev);
  return errormsg;
}


/*
  Check if every GTID requested by the slave is contained in this (or a later)
  binlog file. Return true if so, false if not.

  We do the check with a single scan of the list of GTIDs, avoiding the need
  to build an in-memory hash or stuff like that.

  We need to check that slave did not request GTID D-S-N1, when the
  Gtid_list_log_event for this binlog file has D-S-N2 with N2 >= N1.
  (Because this means that requested GTID is in an earlier binlog).
  However, if the Gtid_list_log_event indicates that D-S-N1 is the very last
  GTID for domain D in prior binlog files, then it is ok to start from the
  very start of this binlog file. This special case is important, as it
  allows to purge old logs even if some domain is unused for long.

  In addition, we need to check that we do not have a GTID D-S-N3 in the
  Gtid_list_log_event where D is not present in the requested slave state at
  all. Since if D is not in requested slave state, it means that slave needs
  to start at the very first GTID in domain D.
*/
static bool
contains_all_slave_gtid(slave_connection_state *st, Gtid_list_log_event *glev)
{
  uint32 i;

  for (i= 0; i < glev->count; ++i)
  {
    uint32 gl_domain_id= glev->list[i].domain_id;
    const rpl_gtid *gtid= st->find(gl_domain_id);
    if (!gtid)
    {
      /*
        The slave needs to start from the very beginning of this domain, which
        is in an earlier binlog file. So we need to search back further.
      */
      return false;
    }
    if (gtid->server_id == glev->list[i].server_id &&
        gtid->seq_no <= glev->list[i].seq_no)
    {
      /*
        The slave needs to start after gtid, but it is contained in an earlier
        binlog file. So we need to search back further, unless it was the very
        last gtid logged for the domain in earlier binlog files.
      */
      if (gtid->seq_no < glev->list[i].seq_no)
        return false;

      /*
        The slave requested D-S-N1, which happens to be the last GTID logged
        in prior binlog files with same domain id D and server id S.

        The Gtid_list is kept sorted on domain_id, with the last GTID in each
        domain_id group being the last one logged. So if this is the last GTID
        within the domain_id group, then it is ok to start from the very
        beginning of this group, per the special case explained in comment at
        the start of this function. If not, then we need to search back further.
      */
      if (i+1 < glev->count && gl_domain_id == glev->list[i+1].domain_id)
        return false;
    }
  }

  return true;
}


static void
give_error_start_pos_missing_in_binlog(int *err, const char **errormsg,
                                       rpl_gtid *error_gtid)
{
  rpl_gtid binlog_gtid;

  if (mysql_bin_log.lookup_domain_in_binlog_state(error_gtid->domain_id,
                                                  &binlog_gtid) &&
      binlog_gtid.seq_no >= error_gtid->seq_no)
  {
    *errormsg= "Requested slave GTID state not found in binlog. The slave has "
      "probably diverged due to executing erroneous transactions";
    *err= ER_GTID_POSITION_NOT_FOUND_IN_BINLOG2;
  }
  else
  {
    *errormsg= "Requested slave GTID state not found in binlog";
    *err= ER_GTID_POSITION_NOT_FOUND_IN_BINLOG;
  }
}


/*
  Check the start GTID state requested by the slave against our binlog state.

  Give an error if the slave requests something that we do not have in our
  binlog.
*/

static int
check_slave_start_position(binlog_send_info *info, const char **errormsg,
                           rpl_gtid *error_gtid)
{
  uint32 i;
  int err;
  slave_connection_state::entry **delete_list= NULL;
  uint32 delete_idx= 0;
  slave_connection_state *st= &info->gtid_state;

  if (rpl_load_gtid_slave_state(info->thd))
  {
    *errormsg= "Failed to load replication slave GTID state";
    err= ER_CANNOT_LOAD_SLAVE_GTID_STATE;
    goto end;
  }

  for (i= 0; i < st->hash.records; ++i)
  {
    slave_connection_state::entry *slave_gtid_entry=
      (slave_connection_state::entry *)my_hash_element(&st->hash, i);
    rpl_gtid *slave_gtid= &slave_gtid_entry->gtid;
    rpl_gtid master_gtid;
    rpl_gtid master_replication_gtid;
    rpl_gtid start_gtid;
    bool start_at_own_slave_pos=
      rpl_global_gtid_slave_state->domain_to_gtid(slave_gtid->domain_id,
                                                  &master_replication_gtid) &&
      slave_gtid->server_id == master_replication_gtid.server_id &&
      slave_gtid->seq_no == master_replication_gtid.seq_no;

    if (mysql_bin_log.find_in_binlog_state(slave_gtid->domain_id,
                                           slave_gtid->server_id,
                                           &master_gtid) &&
        master_gtid.seq_no >= slave_gtid->seq_no)
    {
      /*
        If connecting slave requests to start at the GTID we last applied when
        we were ourselves a slave, then this GTID may not exist in our binlog
        (in case of --log-slave-updates=0). So set the flag to disable the
        error about missing GTID in the binlog in this case.
      */
      if (start_at_own_slave_pos)
        slave_gtid_entry->flags|= slave_connection_state::START_OWN_SLAVE_POS;
      continue;
    }

    if (!start_at_own_slave_pos)
    {
      rpl_gtid domain_gtid;
      slave_connection_state *until_gtid_state= info->until_gtid_state;
      rpl_gtid *until_gtid;

      if (!mysql_bin_log.lookup_domain_in_binlog_state(slave_gtid->domain_id,
                                                       &domain_gtid))
      {
        /*
          We do not have anything in this domain, neither in the binlog nor
          in the slave state. So we are probably one master in a multi-master
          setup, and this domain is served by a different master.

          But set a flag so that if we then ever _do_ happen to encounter
          anything in this domain, then we will re-check that the requested
          slave position exists, and give the error at that time if not.
        */
        slave_gtid_entry->flags|= slave_connection_state::START_ON_EMPTY_DOMAIN;
        continue;
      }

      if (info->slave_gtid_ignore_duplicates &&
          domain_gtid.seq_no < slave_gtid->seq_no)
      {
        /*
          When --gtid-ignore-duplicates, it is ok for the slave to request
          something that we do not have (yet) - they might already have gotten
          it through another path in a multi-path replication hierarchy.
        */
        continue;
      }

      if (until_gtid_state &&
          ( !(until_gtid= until_gtid_state->find(slave_gtid->domain_id)) ||
            (mysql_bin_log.find_in_binlog_state(until_gtid->domain_id,
                                                until_gtid->server_id,
                                                &master_gtid) &&
             master_gtid.seq_no >= until_gtid->seq_no)))
      {
        /*
          The slave requested to start from a position that is not (yet) in
          our binlog, but it also specified an UNTIL condition that _is_ in
          our binlog (or a missing UNTIL, which means stop at the very
          beginning). So the stop position is before the start position, and
          we just delete the entry from the UNTIL hash to mark that this
          domain has already reached the UNTIL condition.
        */
        if(until_gtid)
          until_gtid_state->remove(until_gtid);
        continue;
      }

      *error_gtid= *slave_gtid;
      give_error_start_pos_missing_in_binlog(&err, errormsg, error_gtid);
      goto end;
    }

    /*
      Ok, so connecting slave asked to start at a GTID that we do not have in
      our binlog, but it was in fact the last GTID we applied earlier, when we
      were acting as a replication slave.

      So this means that we were running as a replication slave without
      --log-slave-updates, but now we switched to be a master. It is worth it
      to handle this special case, as it allows users to run a simple
      master -> slave without --log-slave-updates, and then exchange slave and
      master, as long as they make sure the slave is caught up before switching.
    */

    /*
      First check if we logged something ourselves as a master after being a
      slave. This will be seen as a GTID with our own server_id and bigger
      seq_no than what is in the slave state.

      If we did not log anything ourselves, then start the connecting slave
      replicating from the current binlog end position, which in this case
      corresponds to our replication slave state and hence what the connecting
      slave is requesting.
    */
    if (mysql_bin_log.find_in_binlog_state(slave_gtid->domain_id,
                                           global_system_variables.server_id,
                                           &start_gtid) &&
        start_gtid.seq_no > slave_gtid->seq_no)
    {
      /*
        Start replication within this domain at the first GTID that we logged
        ourselves after becoming a master.

        Remember that this starting point is in fact a "fake" GTID which may
        not exists in the binlog, so that we do not complain about it in
        --gtid-strict-mode.
      */
      slave_gtid->server_id= global_system_variables.server_id;
      slave_gtid_entry->flags|= slave_connection_state::START_OWN_SLAVE_POS;
    }
    else if (mysql_bin_log.lookup_domain_in_binlog_state(slave_gtid->domain_id,
                                                         &start_gtid))
    {
      slave_gtid->server_id= start_gtid.server_id;
      slave_gtid->seq_no= start_gtid.seq_no;
    }
    else
    {
      /*
        We do not have _anything_ in our own binlog for this domain.  Just
        delete the entry in the slave connection state, then it will pick up
        anything new that arrives.

        We just queue up the deletion and do it later, after the loop, so that
        we do not mess up the iteration over the hash.
      */
      if (!delete_list)
      {
        if (!(delete_list= (slave_connection_state::entry **)
              my_malloc(sizeof(*delete_list) * st->hash.records, MYF(MY_WME))))
        {
          *errormsg= "Out of memory while checking slave start position";
          err= ER_OUT_OF_RESOURCES;
          goto end;
        }
      }
      delete_list[delete_idx++]= slave_gtid_entry;
    }
  }

  /* Do any delayed deletes from the hash. */
  if (delete_list)
  {
    for (i= 0; i < delete_idx; ++i)
      st->remove(&(delete_list[i]->gtid));
  }
  err= 0;

end:
  if (delete_list)
    my_free(delete_list);
  return err;
}

/*
  Find the name of the binlog file to start reading for a slave that connects
  using GTID state.

  Returns the file name in out_name, which must be of size at least FN_REFLEN.

  Returns NULL on ok, error message on error.

  In case of non-error return, the returned binlog file is guaranteed to
  contain the first event to be transmitted to the slave for every domain
  present in our binlogs. It is still necessary to skip all GTIDs up to
  and including the GTID requested by slave within each domain.

  However, as a special case, if the event to be sent to the slave is the very
  first event (within that domain) in the returned binlog, then nothing should
  be skipped, so that domain is deleted from the passed in slave connection
  state.

  This is necessary in case the slave requests a GTID within a replication
  domain that has long been inactive. The binlog file containing that GTID may
  have been long since purged. However, as long as no GTIDs after that have
  been purged, we have the GTID requested by slave in the Gtid_list_log_event
  of the latest binlog. So we can start from there, as long as we delete the
  corresponding entry in the slave state so we do not wrongly skip any events
  that might turn up if that domain becomes active again, vainly looking for
  the requested GTID that was already purged.
*/
static const char *
gtid_find_binlog_file(slave_connection_state *state, char *out_name,
                      slave_connection_state *until_gtid_state)
{
  MEM_ROOT memroot;
  binlog_file_entry *list;
  Gtid_list_log_event *glev= NULL;
  const char *errormsg= NULL;
  char buf[FN_REFLEN];

  init_alloc_root(&memroot, 10*(FN_REFLEN+sizeof(binlog_file_entry)), 0,
                  MYF(MY_THREAD_SPECIFIC));
  if (!(list= get_binlog_list(&memroot)))
  {
    errormsg= "Out of memory while looking for GTID position in binlog";
    goto end;
  }

  while (list)
  {
    File file;
    IO_CACHE cache;

    if (!list->next)
    {
      /*
        It should be safe to read the currently used binlog, as we will only
        read the header part that is already written.

        But if that does not work on windows, then we will need to cache the
        event somewhere in memory I suppose - that could work too.
      */
    }
    /*
      Read the Gtid_list_log_event at the start of the binlog file to
      get the binlog state.
    */
    if (normalize_binlog_name(buf, list->name, false))
    {
      errormsg= "Failed to determine binlog file name while looking for "
        "GTID position in binlog";
      goto end;
    }
    bzero((char*) &cache, sizeof(cache));
    if ((file= open_binlog(&cache, buf, &errormsg)) == (File)-1)
      goto end;
    errormsg= get_gtid_list_event(&cache, &glev);
    end_io_cache(&cache);
    mysql_file_close(file, MYF(MY_WME));
    if (errormsg)
      goto end;

    if (!glev || contains_all_slave_gtid(state, glev))
    {
      strmake(out_name, buf, FN_REFLEN);

      if (glev)
      {
        uint32 i;

        /*
          As a special case, we allow to start from binlog file N if the
          requested GTID is the last event (in the corresponding domain) in
          binlog file (N-1), but then we need to remove that GTID from the slave
          state, rather than skipping events waiting for it to turn up.

          If slave is doing START SLAVE UNTIL, check for any UNTIL conditions
          that are already included in a previous binlog file. Delete any such
          from the UNTIL hash, to mark that such domains have already reached
          their UNTIL condition.
        */
        for (i= 0; i < glev->count; ++i)
        {
          const rpl_gtid *gtid= state->find(glev->list[i].domain_id);
          if (!gtid)
          {
            /*
              Contains_all_slave_gtid() returns false if there is any domain in
              Gtid_list_event which is not in the requested slave position.

              We may delete a domain from the slave state inside this loop, but
              we only do this when it is the very last GTID logged for that
              domain in earlier binlogs, and then we can not encounter it in any
              further GTIDs in the Gtid_list.
            */
            DBUG_ASSERT(0);
          } else if (gtid->server_id == glev->list[i].server_id &&
                     gtid->seq_no == glev->list[i].seq_no)
          {
            /*
              The slave requested to start from the very beginning of this
              domain in this binlog file. So delete the entry from the state,
              we do not need to skip anything.
            */
            state->remove(gtid);
          }

          if (until_gtid_state &&
              (gtid= until_gtid_state->find(glev->list[i].domain_id)) &&
              gtid->server_id == glev->list[i].server_id &&
              gtid->seq_no <= glev->list[i].seq_no)
          {
            /*
              We've already reached the stop position in UNTIL for this domain,
              since it is before the start position.
            */
            until_gtid_state->remove(gtid);
          }
        }
      }

      goto end;
    }
    delete glev;
    glev= NULL;
    list= list->next;
  }

  /* We reached the end without finding anything. */
  errormsg= "Could not find GTID state requested by slave in any binlog "
    "files. Probably the slave state is too old and required binlog files "
    "have been purged.";

end:
  if (glev)
    delete glev;

  free_root(&memroot, MYF(0));
  return errormsg;
}


/*
  Given an old-style binlog position with file name and file offset, find the
  corresponding gtid position. If the offset is not at an event boundary, give
  an error.

  Return NULL on ok, error message string on error.

  ToDo: Improve the performance of this by using binlog index files.
*/
static const char *
gtid_state_from_pos(const char *name, uint32 offset,
                    slave_connection_state *gtid_state)
{
  IO_CACHE cache;
  File file;
  const char *errormsg= NULL;
  bool found_gtid_list_event= false;
  bool found_format_description_event= false;
  bool valid_pos= false;
  enum enum_binlog_checksum_alg current_checksum_alg= BINLOG_CHECKSUM_ALG_UNDEF;
  int err;
  String packet;
  Format_description_log_event *fdev= NULL;

  if (gtid_state->load((const rpl_gtid *)NULL, 0))
  {
    errormsg= "Internal error (out of memory?) initializing slave state "
      "while scanning binlog to find start position";
    return errormsg;
  }

  if ((file= open_binlog(&cache, name, &errormsg)) == (File)-1)
    return errormsg;

  if (!(fdev= new Format_description_log_event(3)))
  {
    errormsg= "Out of memory initializing format_description event "
      "while scanning binlog to find start position";
    goto end;
  }

  /*
    First we need to find the initial GTID_LIST_EVENT. We need this even
    if the offset is at the very start of the binlog file.

    But if we do not find any GTID_LIST_EVENT, then this is an old binlog
    with no GTID information, so we return empty GTID state.
  */
  for (;;)
  {
    Log_event_type typ;
    uint32 cur_pos;

    cur_pos= (uint32)my_b_tell(&cache);
    if (cur_pos == offset)
      valid_pos= true;
    if (found_format_description_event && found_gtid_list_event &&
        cur_pos >= offset)
      break;

    packet.length(0);
    err= Log_event::read_log_event(&cache, &packet, fdev,
                         opt_master_verify_checksum ? current_checksum_alg
                                                    : BINLOG_CHECKSUM_ALG_OFF);
    if (err)
    {
      errormsg= "Could not read binlog while searching for slave start "
        "position on master";
      goto end;
    }
    /*
      The cast to uchar is needed to avoid a signed char being converted to a
      negative number.
    */
    typ= (Log_event_type)(uchar)packet[EVENT_TYPE_OFFSET];
    if (typ == FORMAT_DESCRIPTION_EVENT)
    {
      Format_description_log_event *tmp;

      if (found_format_description_event)
      {
        errormsg= "Duplicate format description log event found while "
          "searching for old-style position in binlog";
        goto end;
      }

      current_checksum_alg= get_checksum_alg(packet.ptr(), packet.length());
      found_format_description_event= true;
      if (!(tmp= new Format_description_log_event(packet.ptr(), packet.length(),
                                                  fdev)))
      {
        errormsg= "Corrupt Format_description event found or out-of-memory "
          "while searching for old-style position in binlog";
        goto end;
      }
      delete fdev;
      fdev= tmp;
    }
    else if (typ == START_ENCRYPTION_EVENT)
    {
      uint sele_len = packet.length();
      if (current_checksum_alg == BINLOG_CHECKSUM_ALG_CRC32)
      {
        sele_len -= BINLOG_CHECKSUM_LEN;
      }
      Start_encryption_log_event sele(packet.ptr(), sele_len, fdev);
      if (fdev->start_decryption(&sele))
      {
        errormsg= "Could not start decryption of binlog.";
        goto end;
      }
    }
    else if (typ != FORMAT_DESCRIPTION_EVENT && !found_format_description_event)
    {
      errormsg= "Did not find format description log event while searching "
        "for old-style position in binlog";
      goto end;
    }
    else if (typ == ROTATE_EVENT || typ == STOP_EVENT ||
             typ == BINLOG_CHECKPOINT_EVENT)
      continue;                                 /* Continue looking */
    else if (typ == GTID_LIST_EVENT)
    {
      rpl_gtid *gtid_list;
      bool status;
      uint32 list_len;

      if (found_gtid_list_event)
      {
        errormsg= "Found duplicate Gtid_list_log_event while scanning binlog "
          "to find slave start position";
        goto end;
      }
      status= Gtid_list_log_event::peek(packet.ptr(), packet.length(),
                                        current_checksum_alg,
                                        &gtid_list, &list_len, fdev);
      if (status)
      {
        errormsg= "Error reading Gtid_list_log_event while searching "
          "for old-style position in binlog";
        goto end;
      }
      err= gtid_state->load(gtid_list, list_len);
      my_free(gtid_list);
      if (err)
      {
        errormsg= "Internal error (out of memory?) initialising slave state "
          "while scanning binlog to find start position";
        goto end;
      }
      found_gtid_list_event= true;
    }
    else if (!found_gtid_list_event)
    {
      /* We did not find any Gtid_list_log_event, must be old binlog. */
      goto end;
    }
    else if (typ == GTID_EVENT)
    {
      rpl_gtid gtid;
      uchar flags2;
      if (Gtid_log_event::peek(packet.ptr(), packet.length(),
                               current_checksum_alg, &gtid.domain_id,
                               &gtid.server_id, &gtid.seq_no, &flags2, fdev))
      {
        errormsg= "Corrupt gtid_log_event found while scanning binlog to find "
          "initial slave position";
        goto end;
      }
      if (gtid_state->update(&gtid))
      {
        errormsg= "Internal error (out of memory?) updating slave state while "
          "scanning binlog to find start position";
        goto end;
      }
    }
  }

  if (!valid_pos)
  {
    errormsg= "Slave requested incorrect position in master binlog. "
      "Requested position %u in file '%s', but this position does not "
      "correspond to the location of any binlog event.";
  }

end:
  delete fdev;
  end_io_cache(&cache);
  mysql_file_close(file, MYF(MY_WME));

  return errormsg;
}


int
gtid_state_from_binlog_pos(const char *in_name, uint32 pos, String *out_str)
{
  slave_connection_state gtid_state;
  const char *lookup_name;
  char name_buf[FN_REFLEN];
  LOG_INFO linfo;

  if (!mysql_bin_log.is_open())
  {
    my_error(ER_NO_BINARY_LOGGING, MYF(0));
    return 1;
  }

  if (in_name && in_name[0])
  {
    mysql_bin_log.make_log_name(name_buf, in_name);
    lookup_name= name_buf;
  }
  else
    lookup_name= NULL;
  linfo.index_file_offset= 0;
  if (mysql_bin_log.find_log_pos(&linfo, lookup_name, 1))
    return 1;

  if (pos < 4)
    pos= 4;

  if (gtid_state_from_pos(linfo.log_file_name, pos, &gtid_state) ||
      gtid_state.to_string(out_str))
    return 1;
  return 0;
}


static bool
is_until_reached(binlog_send_info *info, ulong *ev_offset,
                 Log_event_type event_type, const char **errmsg,
                 uint32 current_pos)
{
  switch (info->gtid_until_group)
  {
  case GTID_UNTIL_NOT_DONE:
    return false;
  case GTID_UNTIL_STOP_AFTER_STANDALONE:
    if (Log_event::is_part_of_group(event_type))
      return false;
    break;
  case GTID_UNTIL_STOP_AFTER_TRANSACTION:
    if (event_type != XID_EVENT &&
        (event_type != QUERY_EVENT ||
         !Query_log_event::peek_is_commit_rollback
               (info->packet->ptr()+*ev_offset,
                info->packet->length()-*ev_offset,
                info->current_checksum_alg)))
      return false;
    break;
  }

  /*
    The last event group has been sent, now the START SLAVE UNTIL condition
    has been reached.

    Send a last fake Gtid_list_log_event with a flag set to mark that we
    stop due to UNTIL condition.
  */
  if (reset_transmit_packet(info, info->flags, ev_offset, errmsg))
    return true;
  Gtid_list_log_event glev(&info->until_binlog_state,
                           Gtid_list_log_event::FLAG_UNTIL_REACHED);
  if (fake_gtid_list_event(info, &glev, errmsg, current_pos))
    return true;
  *errmsg= NULL;
  return true;
}


/*
  Helper function for mysql_binlog_send() to write an event down the slave
  connection.

  Returns NULL on success, error message string on error.
*/
static const char *
send_event_to_slave(binlog_send_info *info, Log_event_type event_type,
                    IO_CACHE *log, ulong ev_offset, rpl_gtid *error_gtid)
{
  my_off_t pos;
  String* const packet= info->packet;
  size_t len= packet->length();
  int mariadb_slave_capability= info->mariadb_slave_capability;
  enum enum_binlog_checksum_alg current_checksum_alg= info->current_checksum_alg;
  slave_connection_state *gtid_state= &info->gtid_state;
  slave_connection_state *until_gtid_state= info->until_gtid_state;

  if (event_type == GTID_LIST_EVENT &&
      info->using_gtid_state && until_gtid_state)
  {
    rpl_gtid *gtid_list;
    uint32 list_len;
    bool err;

    if (ev_offset > len ||
        Gtid_list_log_event::peek(packet->ptr()+ev_offset, len - ev_offset,
                                  current_checksum_alg,
                                  &gtid_list, &list_len, info->fdev))
    {
      info->error= ER_MASTER_FATAL_ERROR_READING_BINLOG;
      return "Failed to read Gtid_list_log_event: corrupt binlog";
    }
    err= info->until_binlog_state.load(gtid_list, list_len);
    my_free(gtid_list);
    if (err)
    {
      info->error= ER_MASTER_FATAL_ERROR_READING_BINLOG;
      return "Failed in internal GTID book-keeping: Out of memory";
    }
  }

  /* Skip GTID event groups until we reach slave position within a domain_id. */
  if (event_type == GTID_EVENT && info->using_gtid_state)
  {
    uchar flags2;
    slave_connection_state::entry *gtid_entry;
    rpl_gtid *gtid;

    if (gtid_state->count() > 0 || until_gtid_state)
    {
      rpl_gtid event_gtid;

      if (ev_offset > len ||
          Gtid_log_event::peek(packet->ptr()+ev_offset, len - ev_offset,
                               current_checksum_alg,
                               &event_gtid.domain_id, &event_gtid.server_id,
                               &event_gtid.seq_no, &flags2, info->fdev))
      {
        info->error= ER_MASTER_FATAL_ERROR_READING_BINLOG;
        return "Failed to read Gtid_log_event: corrupt binlog";
      }

      DBUG_EXECUTE_IF("gtid_force_reconnect_at_10_1_100",
        {
          rpl_gtid *dbug_gtid;
          if ((dbug_gtid= info->until_binlog_state.find_nolock(10,1)) &&
              dbug_gtid->seq_no == 100)
          {
            DBUG_SET("-d,gtid_force_reconnect_at_10_1_100");
            DBUG_SET_INITIAL("-d,gtid_force_reconnect_at_10_1_100");
            info->error= ER_UNKNOWN_ERROR;
            return "DBUG-injected forced reconnect";
          }
        });

      if (info->until_binlog_state.update_nolock(&event_gtid, false))
      {
        info->error= ER_MASTER_FATAL_ERROR_READING_BINLOG;
        return "Failed in internal GTID book-keeping: Out of memory";
      }

      if (gtid_state->count() > 0)
      {
        gtid_entry= gtid_state->find_entry(event_gtid.domain_id);
        if (gtid_entry != NULL)
        {
          gtid= &gtid_entry->gtid;
          if (gtid_entry->flags & slave_connection_state::START_ON_EMPTY_DOMAIN)
          {
            rpl_gtid master_gtid;
            if (!mysql_bin_log.find_in_binlog_state(gtid->domain_id,
                                                    gtid->server_id,
                                                    &master_gtid) ||
                master_gtid.seq_no < gtid->seq_no)
            {
              int err;
              const char *errormsg;
              *error_gtid= *gtid;
              give_error_start_pos_missing_in_binlog(&err, &errormsg, error_gtid);
              info->error= err;
              return errormsg;
            }
            gtid_entry->flags&= ~(uint32)slave_connection_state::START_ON_EMPTY_DOMAIN;
          }

          /* Skip this event group if we have not yet reached slave start pos. */
          if (event_gtid.server_id != gtid->server_id ||
              event_gtid.seq_no <= gtid->seq_no)
            info->gtid_skip_group= (flags2 & Gtid_log_event::FL_STANDALONE ?
                                GTID_SKIP_STANDALONE : GTID_SKIP_TRANSACTION);
          if (event_gtid.server_id == gtid->server_id &&
              event_gtid.seq_no >= gtid->seq_no)
          {
            if (info->slave_gtid_strict_mode &&
                event_gtid.seq_no > gtid->seq_no &&
                !(gtid_entry->flags & slave_connection_state::START_OWN_SLAVE_POS))
            {
              /*
                In strict mode, it is an error if the slave requests to start
                in a "hole" in the master's binlog: a GTID that does not
                exist, even though both the prior and subsequent seq_no exists
                for same domain_id and server_id.
              */
              info->error= ER_GTID_START_FROM_BINLOG_HOLE;
              *error_gtid= *gtid;
              return "The binlog on the master is missing the GTID requested "
                "by the slave (even though both a prior and a subsequent "
                "sequence number does exist), and GTID strict mode is enabled.";
            }

            /*
              Send a fake Gtid_list event to the slave.
              This allows the slave to update its current binlog position
              so MASTER_POS_WAIT() and MASTER_GTID_WAIT() can work.
              The fake event will be sent at the end of this event group.
            */
            info->send_fake_gtid_list= true;

            /*
              Delete this entry if we have reached slave start position (so we
              will not skip subsequent events and won't have to look them up
              and check).
            */
            gtid_state->remove(gtid);
          }
        }
      }

      if (until_gtid_state)
      {
        gtid= until_gtid_state->find(event_gtid.domain_id);
        if (gtid == NULL)
        {
          /*
            This domain already reached the START SLAVE UNTIL stop condition,
            so skip this event group.
          */
          info->gtid_skip_group = (flags2 & Gtid_log_event::FL_STANDALONE ?
                              GTID_SKIP_STANDALONE : GTID_SKIP_TRANSACTION);
        }
        else if (event_gtid.server_id == gtid->server_id &&
                 event_gtid.seq_no >= gtid->seq_no)
        {
          /*
            We have reached the stop condition.
            Delete this domain_id from the hash, so we will skip all further
            events in this domain and eventually stop when all domains are
            done.
          */
          uint64 until_seq_no= gtid->seq_no;
          until_gtid_state->remove(gtid);
          if (until_gtid_state->count() == 0)
            info->gtid_until_group= (flags2 & Gtid_log_event::FL_STANDALONE ?
                                     GTID_UNTIL_STOP_AFTER_STANDALONE :
                                     GTID_UNTIL_STOP_AFTER_TRANSACTION);
          if (event_gtid.seq_no > until_seq_no)
          {
            /*
              The GTID in START SLAVE UNTIL condition is missing in our binlog.
              This should normally not happen (user error), but since we can be
              sure that we are now beyond the position that the UNTIL condition
              should be in, we can just stop now. And we also need to skip this
              event group (as it is beyond the UNTIL condition).
            */
            info->gtid_skip_group = (flags2 & Gtid_log_event::FL_STANDALONE ?
                                GTID_SKIP_STANDALONE : GTID_SKIP_TRANSACTION);
          }
        }
      }
    }
  }

  /*
    Skip event group if we have not yet reached the correct slave GTID position.

    Note that slave that understands GTID can also tolerate holes, so there is
    no need to supply dummy event.
  */
  switch (info->gtid_skip_group)
  {
  case GTID_SKIP_STANDALONE:
    if (!Log_event::is_part_of_group(event_type))
      info->gtid_skip_group= GTID_SKIP_NOT;
    return NULL;
  case GTID_SKIP_TRANSACTION:
    if (event_type == XID_EVENT ||
        (event_type == QUERY_EVENT &&
         Query_log_event::peek_is_commit_rollback(packet->ptr() + ev_offset,
                                                  len - ev_offset,
                                                  current_checksum_alg)))
      info->gtid_skip_group= GTID_SKIP_NOT;
    return NULL;
  case GTID_SKIP_NOT:
    break;
  }

  /* Do not send annotate_rows events unless slave requested it. */
  if (event_type == ANNOTATE_ROWS_EVENT &&
      !(info->flags & BINLOG_SEND_ANNOTATE_ROWS_EVENT))
  {
    if (mariadb_slave_capability >= MARIA_SLAVE_CAPABILITY_TOLERATE_HOLES)
    {
      /* This slave can tolerate events omitted from the binlog stream. */
      return NULL;
    }
    else if (mariadb_slave_capability >= MARIA_SLAVE_CAPABILITY_ANNOTATE)
    {
      /*
        The slave did not request ANNOTATE_ROWS_EVENT (it does not need them as
        it will not log them in its own binary log). However, it understands the
        event and will just ignore it, and it would break if we omitted it,
        leaving a hole in the binlog stream. So just send the event as-is.
      */
    }
    else
    {
      /*
        The slave does not understand ANNOTATE_ROWS_EVENT.

        Older MariaDB slaves (and MySQL slaves) will break replication if there
        are holes in the binlog stream (they will miscompute the binlog offset
        and request the wrong position when reconnecting).

        So replace the event with a dummy event of the same size that will be
        a no-operation on the slave.
      */
      if (Query_log_event::dummy_event(packet, ev_offset, current_checksum_alg))
      {
        info->error= ER_MASTER_FATAL_ERROR_READING_BINLOG;
        return "Failed to replace row annotate event with dummy: too small event.";
      }
    }
  }

  /*
    Replace GTID events with old-style BEGIN events for slaves that do not
    understand global transaction IDs. For stand-alone events, where there is
    no terminating COMMIT query event, omit the GTID event or replace it with
    a dummy event, as appropriate.
  */
  if (event_type == GTID_EVENT &&
      mariadb_slave_capability < MARIA_SLAVE_CAPABILITY_GTID)
  {
    bool need_dummy=
      mariadb_slave_capability < MARIA_SLAVE_CAPABILITY_TOLERATE_HOLES;
    bool err= Gtid_log_event::make_compatible_event(packet, &need_dummy,
                                                    ev_offset,
                                                    current_checksum_alg);
    if (err)
    {
      info->error= ER_MASTER_FATAL_ERROR_READING_BINLOG;
      return "Failed to replace GTID event with backwards-compatible event: "
             "currupt event.";
    }
    if (!need_dummy)
      return NULL;
  }

  /*
    Do not send binlog checkpoint or gtid list events to a slave that does not
    understand it.
  */
  if ((unlikely(event_type == BINLOG_CHECKPOINT_EVENT) &&
       mariadb_slave_capability < MARIA_SLAVE_CAPABILITY_BINLOG_CHECKPOINT) ||
      (unlikely(event_type == GTID_LIST_EVENT) &&
       mariadb_slave_capability < MARIA_SLAVE_CAPABILITY_GTID))
  {
    if (mariadb_slave_capability >= MARIA_SLAVE_CAPABILITY_TOLERATE_HOLES)
    {
      /* This slave can tolerate events omitted from the binlog stream. */
      return NULL;
    }
    else
    {
      /*
        The slave does not understand BINLOG_CHECKPOINT_EVENT. Send a dummy
        event instead, with same length so slave does not get confused about
        binlog positions.
      */
      if (Query_log_event::dummy_event(packet, ev_offset, current_checksum_alg))
      {
        info->error= ER_MASTER_FATAL_ERROR_READING_BINLOG;
        return "Failed to replace binlog checkpoint or gtid list event with "
               "dummy: too small event.";
      }
    }
  }

  /*
    Skip events with the @@skip_replication flag set, if slave requested
    skipping of such events.
  */
  if (info->thd->variables.option_bits & OPTION_SKIP_REPLICATION)
  {
    /*
      The first byte of the packet is a '\0' to distinguish it from an error
      packet. So the actual event starts at offset +1.
    */
    uint16 event_flags= uint2korr(&((*packet)[FLAGS_OFFSET+1]));
    if (event_flags & LOG_EVENT_SKIP_REPLICATION_F)
      return NULL;
  }

  THD_STAGE_INFO(info->thd, stage_sending_binlog_event_to_slave);

  pos= my_b_tell(log);
  if (RUN_HOOK(binlog_transmit, before_send_event,
               (info->thd, info->flags, packet, info->log_file_name, pos)))
  {
    info->error= ER_UNKNOWN_ERROR;
    return "run 'before_send_event' hook failed";
  }

  if (my_net_write(info->net, (uchar*) packet->ptr(), len))
  {
    info->error= ER_UNKNOWN_ERROR;
    return "Failed on my_net_write()";
  }

  DBUG_PRINT("info", ("log event code %d", (*packet)[LOG_EVENT_OFFSET+1] ));
  if (event_type == LOAD_EVENT)
  {
    if (send_file(info->thd))
    {
      info->error= ER_UNKNOWN_ERROR;
      return "failed in send_file()";
    }
  }

  if (RUN_HOOK(binlog_transmit, after_send_event,
               (info->thd, info->flags, packet)))
  {
    info->error= ER_UNKNOWN_ERROR;
    return "Failed to run hook 'after_send_event'";
  }

  return NULL;    /* Success */
}

static int check_start_offset(binlog_send_info *info,
                              const char *log_file_name,
                              my_off_t pos)
{
  IO_CACHE log;
  File file= -1;

  /** check that requested position is inside of file */
  if ((file=open_binlog(&log, log_file_name, &info->errmsg)) < 0)
  {
    info->error= ER_MASTER_FATAL_ERROR_READING_BINLOG;
    return 1;
  }

  if (pos < BIN_LOG_HEADER_SIZE || pos > my_b_filelength(&log))
  {
    const char* msg= "Client requested master to start replication from "
        "impossible position";

    info->errmsg= NULL; // don't do further modifications of error_text
    snprintf(info->error_text, sizeof(info->error_text),
             "%s; the first event '%s' at %lld, "
             "the last event read from '%s' at %d, "
             "the last byte read from '%s' at %d.",
             msg,
             my_basename(info->start_log_file_name), pos,
             my_basename(info->start_log_file_name), BIN_LOG_HEADER_SIZE,
             my_basename(info->start_log_file_name), BIN_LOG_HEADER_SIZE);
    info->error= ER_MASTER_FATAL_ERROR_READING_BINLOG;
    goto err;
  }

err:
  end_io_cache(&log);
  mysql_file_close(file, MYF(MY_WME));
  return info->error;
}

static int init_binlog_sender(binlog_send_info *info,
                              LOG_INFO *linfo,
                              const char *log_ident,
                              my_off_t *pos)
{
  THD *thd= info->thd;
  int error;
  char str_buf[128];
  String connect_gtid_state(str_buf, sizeof(str_buf), system_charset_info);
  char str_buf2[128];
  String slave_until_gtid_str(str_buf2, sizeof(str_buf2), system_charset_info);
  connect_gtid_state.length(0);

  /** save start file/pos that was requested by slave */
  strmake(info->start_log_file_name, log_ident,
          sizeof(info->start_log_file_name));
  info->start_pos= *pos;

  /** init last pos */
  info->last_pos= *pos;

  info->current_checksum_alg= get_binlog_checksum_value_at_connect(thd);
  info->mariadb_slave_capability= get_mariadb_slave_capability(thd);
  info->using_gtid_state= get_slave_connect_state(thd, &connect_gtid_state);
  DBUG_EXECUTE_IF("simulate_non_gtid_aware_master",
                  info->using_gtid_state= false;);

  if (info->using_gtid_state)
  {
    info->slave_gtid_strict_mode= get_slave_gtid_strict_mode(thd);
    info->slave_gtid_ignore_duplicates= get_slave_gtid_ignore_duplicates(thd);
    if (get_slave_until_gtid(thd, &slave_until_gtid_str))
      info->until_gtid_state= &info->until_gtid_state_obj;
  }

  DBUG_EXECUTE_IF("binlog_force_reconnect_after_22_events",
    {
      DBUG_SET("-d,binlog_force_reconnect_after_22_events");
      DBUG_SET_INITIAL("-d,binlog_force_reconnect_after_22_events");
      info->dbug_reconnect_counter= 22;
    });

  if (global_system_variables.log_warnings > 1)
    sql_print_information(
        "Start binlog_dump to slave_server(%lu), pos(%s, %lu)",
        thd->variables.server_id, log_ident, (ulong)*pos);

#ifndef DBUG_OFF
  if (opt_sporadic_binlog_dump_fail && (binlog_dump_count++ % 2))
  {
    info->errmsg= "Master failed COM_BINLOG_DUMP to test if slave can recover";
    info->error= ER_UNKNOWN_ERROR;
    return 1;
  }
#endif

  if (!mysql_bin_log.is_open())
  {
    info->errmsg= "Binary log is not open";
    info->error= ER_MASTER_FATAL_ERROR_READING_BINLOG;
    return 1;
  }
  if (!server_id_supplied)
  {
    info->errmsg= "Misconfigured master - server id was not set";
    info->error= ER_MASTER_FATAL_ERROR_READING_BINLOG;
    return 1;
  }

  char search_file_name[FN_REFLEN];
  const char *name=search_file_name;
  if (info->using_gtid_state)
  {
    if (info->gtid_state.load(connect_gtid_state.c_ptr_quick(),
                             connect_gtid_state.length()))
    {
      info->errmsg= "Out of memory or malformed slave request when obtaining "
          "start position from GTID state";
      info->error= ER_UNKNOWN_ERROR;
      return 1;
    }
    if (info->until_gtid_state &&
        info->until_gtid_state->load(slave_until_gtid_str.c_ptr_quick(),
                                    slave_until_gtid_str.length()))
    {
      info->errmsg= "Out of memory or malformed slave request when "
          "obtaining UNTIL position sent from slave";
      info->error= ER_UNKNOWN_ERROR;
      return 1;
    }
    if ((error= check_slave_start_position(info, &info->errmsg,
                                           &info->error_gtid)))
    {
      info->error= error;
      return 1;
    }
    if ((info->errmsg= gtid_find_binlog_file(&info->gtid_state,
                                             search_file_name,
                                             info->until_gtid_state)))
    {
      info->error= ER_MASTER_FATAL_ERROR_READING_BINLOG;
      return 1;
    }

    /* start from beginning of binlog file */
    *pos = 4;
  }
  else
  {
    if (log_ident[0])
      mysql_bin_log.make_log_name(search_file_name, log_ident);
    else
      name=0; // Find first log
  }
  linfo->index_file_offset= 0;

  if (mysql_bin_log.find_log_pos(linfo, name, 1))
  {
    info->errmsg= "Could not find first log file name in binary "
        "log index file";
    info->error= ER_MASTER_FATAL_ERROR_READING_BINLOG;
    return 1;
  }

  // set current pos too
  linfo->pos= *pos;

  // note: publish that we use file, before we open it
  mysql_mutex_lock(&LOCK_thread_count);
  thd->current_linfo= linfo;
  mysql_mutex_unlock(&LOCK_thread_count);

  if (check_start_offset(info, linfo->log_file_name, *pos))
    return 1;

  if (*pos > BIN_LOG_HEADER_SIZE)
  {
    /*
      mark that first format descriptor with "log_pos=0", so the slave
      should not increment master's binlog position
      (rli->group_master_log_pos)
    */
    info->clear_initial_log_pos= true;
  }

  return 0;
}

/**
 * send format descriptor event for one binlog file
 */
static int send_format_descriptor_event(binlog_send_info *info, IO_CACHE *log,
                                        LOG_INFO *linfo, my_off_t start_pos)
{
  int error;
  ulong ev_offset;
  THD *thd= info->thd;
  String *packet= info->packet;
  Log_event_type event_type;

  /**
   * 1) reset fdev before each log-file
   * 2) read first event, should be the format descriptor
   * 3) read second event, *might* be start encryption event
   *    if it's isn't, seek back to undo this read
   */
  if (info->fdev != NULL)
    delete info->fdev;

  if (!(info->fdev= new Format_description_log_event(3)))
  {
    info->errmsg= "Out of memory initializing format_description event";
    info->error= ER_MASTER_FATAL_ERROR_READING_BINLOG;
    return 1;
  }

  /* reset transmit packet for the event read from binary log file */
  if (reset_transmit_packet(info, info->flags, &ev_offset, &info->errmsg))
    return 1;

  /*
    Try to find a Format_description_log_event at the beginning of
    the binlog
  */
  info->last_pos= my_b_tell(log);
  error= Log_event::read_log_event(log, packet, info->fdev,
                                   opt_master_verify_checksum
                                   ? info->current_checksum_alg
                                   : BINLOG_CHECKSUM_ALG_OFF);
  linfo->pos= my_b_tell(log);

  if (error)
  {
    set_read_error(info, error);
    return 1;
  }

  event_type= (Log_event_type)((uchar)(*packet)[LOG_EVENT_OFFSET+ev_offset]);

  /*
    The packet has offsets equal to the normal offsets in a
    binlog event + ev_offset (the first ev_offset characters are
    the header (default \0)).
  */
  DBUG_PRINT("info",
             ("Looked for a Format_description_log_event, "
              "found event type %d", (int)event_type));

  if (event_type != FORMAT_DESCRIPTION_EVENT)
  {
    info->error= ER_MASTER_FATAL_ERROR_READING_BINLOG;
    info->errmsg= "Failed to find format descriptor event in start of binlog";
    sql_print_warning("Failed to find format descriptor event in "
                      "start of binlog: %s",
                      info->log_file_name);
    return 1;
  }

  info->current_checksum_alg= get_checksum_alg(packet->ptr() + ev_offset,
                                               packet->length() - ev_offset);

  DBUG_ASSERT(info->current_checksum_alg == BINLOG_CHECKSUM_ALG_OFF ||
              info->current_checksum_alg == BINLOG_CHECKSUM_ALG_UNDEF ||
              info->current_checksum_alg == BINLOG_CHECKSUM_ALG_CRC32);

  if (!is_slave_checksum_aware(thd) &&
      info->current_checksum_alg != BINLOG_CHECKSUM_ALG_OFF &&
      info->current_checksum_alg != BINLOG_CHECKSUM_ALG_UNDEF)
  {
    info->error= ER_MASTER_FATAL_ERROR_READING_BINLOG;
    info->errmsg= "Slave can not handle replication events with the "
        "checksum that master is configured to log";
    sql_print_warning("Master is configured to log replication events "
                      "with checksum, but will not send such events to "
                      "slaves that cannot process them");
    return 1;
  }

  uint ev_len= packet->length() - ev_offset;
  if (info->current_checksum_alg != BINLOG_CHECKSUM_ALG_UNDEF)
    ev_len-= BINLOG_CHECKSUM_LEN;

  Format_description_log_event *tmp;
  if (!(tmp= new Format_description_log_event(packet->ptr() + ev_offset,
                                              ev_len, info->fdev)))
  {
    info->error= ER_MASTER_FATAL_ERROR_READING_BINLOG;
    info->errmsg= "Corrupt Format_description event found "
        "or out-of-memory";
    return 1;
  }
  delete info->fdev;
  info->fdev= tmp;

  (*packet)[FLAGS_OFFSET+ev_offset] &= ~LOG_EVENT_BINLOG_IN_USE_F;

  if (info->clear_initial_log_pos)
  {
    info->clear_initial_log_pos= false;
    /*
      mark that this event with "log_pos=0", so the slave
      should not increment master's binlog position
      (rli->group_master_log_pos)
    */
    int4store((char*) packet->ptr()+LOG_POS_OFFSET+ev_offset, (ulong) 0);

    /*
      if reconnect master sends FD event with `created' as 0
      to avoid destroying temp tables.
    */
    int4store((char*) packet->ptr()+LOG_EVENT_MINIMAL_HEADER_LEN+
              ST_CREATED_OFFSET+ev_offset, (ulong) 0);

    /* fix the checksum due to latest changes in header */
    if (info->current_checksum_alg != BINLOG_CHECKSUM_ALG_OFF &&
      info->current_checksum_alg != BINLOG_CHECKSUM_ALG_UNDEF)
      fix_checksum(packet, ev_offset);
  }
  else if (info->using_gtid_state)
  {
    /*
      If this event has the field `created' set, then it will cause the
      slave to delete all active temporary tables. This must not happen
      if the slave received any later GTIDs in a previous connect, as
      those GTIDs might have created new temporary tables that are still
      needed.

      So here, we check if the starting GTID position was already
      reached before this format description event. If not, we clear the
      `created' flag to preserve temporary tables on the slave. (If the
      slave connects at a position past this event, it means that it
      already received and handled it in a previous connect).
    */
    if (!info->gtid_state.is_pos_reached())
    {
      int4store((char*) packet->ptr()+LOG_EVENT_MINIMAL_HEADER_LEN+
                ST_CREATED_OFFSET+ev_offset, (ulong) 0);
      if (info->current_checksum_alg != BINLOG_CHECKSUM_ALG_OFF &&
          info->current_checksum_alg != BINLOG_CHECKSUM_ALG_UNDEF)
        fix_checksum(packet, ev_offset);
    }
  }

  /* send it */
  if (my_net_write(info->net, (uchar*) packet->ptr(), packet->length()))
  {
    info->errmsg= "Failed on my_net_write()";
    info->error= ER_UNKNOWN_ERROR;
    return 1;
  }

  /*
    Read the following Start_encryption_log_event but don't send it to slave.
    Slave doesn't need to know whether master's binlog is encrypted,
    and if it'll want to encrypt its logs, it should generate its own
    random nonce, not use the one from the master.
  */
  packet->length(0);
  info->last_pos= linfo->pos;
  error= Log_event::read_log_event(log, packet, info->fdev,
                                   opt_master_verify_checksum
                                   ? info->current_checksum_alg
                                   : BINLOG_CHECKSUM_ALG_OFF);
  linfo->pos= my_b_tell(log);

  if (error)
  {
    set_read_error(info, error);
    return 1;
  }

  event_type= (Log_event_type)((uchar)(*packet)[LOG_EVENT_OFFSET]);
  if (event_type == START_ENCRYPTION_EVENT)
  {
    Start_encryption_log_event *sele= (Start_encryption_log_event *)
      Log_event::read_log_event(packet->ptr(), packet->length(), &info->errmsg,
                                info->fdev, BINLOG_CHECKSUM_ALG_OFF);
    if (!sele)
    {
      info->error= ER_MASTER_FATAL_ERROR_READING_BINLOG;
      return 1;
    }

    if (info->fdev->start_decryption(sele))
    {
      info->error= ER_MASTER_FATAL_ERROR_READING_BINLOG;
      info->errmsg= "Could not decrypt binlog: encryption key error";
      return 1;
    }
    delete sele;
  }
  else if (start_pos == BIN_LOG_HEADER_SIZE)
  {
    /*
      not Start_encryption_log_event - seek back. But only if
      send_one_binlog_file() isn't going to seek anyway
    */
    my_b_seek(log, info->last_pos);
    linfo->pos= info->last_pos;
  }


  /** all done */
  return 0;
}

static bool should_stop(binlog_send_info *info)
{
  return
      info->net->error ||
      info->net->vio == NULL ||
      info->thd->killed ||
      info->error != 0 ||
      info->should_stop;
}

/**
 * wait for new events to enter binlog
 * this function will send heartbeats while waiting if so configured
 */
static int wait_new_events(binlog_send_info *info,         /* in */
                           LOG_INFO* linfo,                /* in */
                           char binlog_end_pos_filename[], /* out */
                           my_off_t *end_pos_ptr)          /* out */
{
  int ret= 1;
  PSI_stage_info old_stage;

  mysql_bin_log.lock_binlog_end_pos();
  info->thd->ENTER_COND(mysql_bin_log.get_log_cond(),
                        mysql_bin_log.get_binlog_end_pos_lock(),
                        &stage_master_has_sent_all_binlog_to_slave,
                        &old_stage);

  while (!should_stop(info))
  {
    *end_pos_ptr= mysql_bin_log.get_binlog_end_pos(binlog_end_pos_filename);
    if (strcmp(linfo->log_file_name, binlog_end_pos_filename) != 0)
    {
      /* there has been a log file switch, we don't need to wait */
      ret= 0;
      break;
    }

    if (linfo->pos < *end_pos_ptr)
    {
      /* there is data to read, we don't need to wait */
      ret= 0;
      break;
    }

    if (info->heartbeat_period)
    {
      struct timespec ts;
      set_timespec_nsec(ts, info->heartbeat_period);
      ret= mysql_bin_log.wait_for_update_binlog_end_pos(info->thd, &ts);
      if (ret == ETIMEDOUT || ret == ETIME)
      {
        struct event_coordinates coord = { linfo->log_file_name, linfo->pos };
#ifndef DBUG_OFF
        const ulong hb_info_counter_limit = 3;
        if (info->hb_info_counter < hb_info_counter_limit)
        {
          sql_print_information("master sends heartbeat message %s:%llu",
                                linfo->log_file_name, linfo->pos);
          info->hb_info_counter++;
          if (info->hb_info_counter == hb_info_counter_limit)
            sql_print_information("the rest of heartbeat info skipped ...");
        }
#endif
        mysql_bin_log.unlock_binlog_end_pos();
        ret= send_heartbeat_event(info,
                                  info->net, info->packet, &coord,
                                  info->current_checksum_alg);
        mysql_bin_log.lock_binlog_end_pos();

        if (ret)
        {
          ret= 1; // error
          break;
        }
        /**
         * re-read heartbeat period after each sent
         */
        info->heartbeat_period= get_heartbeat_period(info->thd);
      }
      else if (ret != 0)
      {
        ret= 1; // error
        break;
      }
    }
    else
    {
      ret= mysql_bin_log.wait_for_update_binlog_end_pos(info->thd, NULL);
      if (ret != 0 && ret != ETIMEDOUT && ret != ETIME)
      {
        ret= 1; // error
        break;
      }
    }
  }

  /* it releases the lock set in ENTER_COND */
  info->thd->EXIT_COND(&old_stage);
  return ret;
}

/**
 * get end pos of current log file, this function
 * will wait if there is nothing available
 */
static my_off_t get_binlog_end_pos(binlog_send_info *info,
                                   IO_CACHE* log,
                                   LOG_INFO* linfo)
{
  my_off_t log_pos= my_b_tell(log);

  /**
   * get current binlog end pos
   */
  mysql_bin_log.lock_binlog_end_pos();
  char binlog_end_pos_filename[FN_REFLEN];
  my_off_t end_pos= mysql_bin_log.get_binlog_end_pos(binlog_end_pos_filename);
  mysql_bin_log.unlock_binlog_end_pos();

  do
  {
    if (strcmp(binlog_end_pos_filename, linfo->log_file_name) != 0)
    {
      /**
       * this file is not active, since it's not written to again,
       * it safe to check file length and use that as end_pos
       */
      end_pos= my_b_filelength(log);

      if (log_pos == end_pos)
        return 0;        // already at end of file inactive file
      else
        return end_pos;  // return size of inactive file
    }
    else
    {
      /**
       * this is the active file
       */

      if (log_pos < end_pos)
      {
        /**
         * there is data available to read
         */
        return end_pos;
      }

      /**
       * check if we should wait for more data
       */
      if ((info->flags & BINLOG_DUMP_NON_BLOCK) ||
          (info->thd->variables.server_id == 0))
      {
        info->should_stop= true;
        return 0;
      }

      /**
       * flush data before waiting
       */
      if (net_flush(info->net))
      {
        info->errmsg= "failed on net_flush()";
        info->error= ER_UNKNOWN_ERROR;
        return 1;
      }

      if (wait_new_events(info, linfo, binlog_end_pos_filename, &end_pos))
        return 1;
    }
  } while (!should_stop(info));

  return 0;
}

/**
 * This function sends events from one binlog file
 * but only up until end_pos
 *
 * return 0 - OK
 *        else NOK
 */
static int send_events(binlog_send_info *info, IO_CACHE* log, LOG_INFO* linfo,
                       my_off_t end_pos)
{
  int error;
  ulong ev_offset;

  String *packet= info->packet;
  linfo->pos= my_b_tell(log);
  info->last_pos= my_b_tell(log);

  while (linfo->pos < end_pos)
  {
    if (should_stop(info))
      return 0;

    /* reset the transmit packet for the event read from binary log
       file */
    if (reset_transmit_packet(info, info->flags, &ev_offset, &info->errmsg))
      return 1;

    info->last_pos= linfo->pos;
    error= Log_event::read_log_event(log, packet, info->fdev,
                       opt_master_verify_checksum ? info->current_checksum_alg
                                                  : BINLOG_CHECKSUM_ALG_OFF);
    linfo->pos= my_b_tell(log);

    if (error)
    {
      set_read_error(info, error);
      return 1;
    }

    Log_event_type event_type=
        (Log_event_type)((uchar)(*packet)[LOG_EVENT_OFFSET+ev_offset]);

#ifndef DBUG_OFF
    if (info->dbug_reconnect_counter > 0)
    {
      --info->dbug_reconnect_counter;
      if (info->dbug_reconnect_counter == 0)
      {
        info->errmsg= "DBUG-injected forced reconnect";
        info->error= ER_UNKNOWN_ERROR;
        return 1;
      }
    }
#endif

#ifdef ENABLED_DEBUG_SYNC
    DBUG_EXECUTE_IF("dump_thread_wait_before_send_xid",
                    {
                      if (event_type == XID_EVENT)
                      {
                        net_flush(info->net);
                        const char act[]=
                            "now "
                            "wait_for signal.continue";
                        DBUG_ASSERT(debug_sync_service);
                        DBUG_ASSERT(!debug_sync_set_action(
                            info->thd,
                            STRING_WITH_LEN(act)));

                        const char act2[]=
                            "now "
                            "signal signal.continued";
                        DBUG_ASSERT(!debug_sync_set_action(
                            info->thd,
                            STRING_WITH_LEN(act2)));
                      }
                    });
#endif

    if (event_type != START_ENCRYPTION_EVENT &&
        ((info->errmsg= send_event_to_slave(info, event_type, log,
                                           ev_offset, &info->error_gtid))))
      return 1;

    if (unlikely(info->send_fake_gtid_list) &&
        info->gtid_skip_group == GTID_SKIP_NOT)
    {
      Gtid_list_log_event glev(&info->until_binlog_state, 0);

      if (reset_transmit_packet(info, info->flags, &ev_offset, &info->errmsg) ||
          fake_gtid_list_event(info, &glev, &info->errmsg, my_b_tell(log)))
      {
        info->error= ER_UNKNOWN_ERROR;
        return 1;
      }
      info->send_fake_gtid_list= false;
    }

    if (info->until_gtid_state &&
        is_until_reached(info, &ev_offset, event_type, &info->errmsg,
                         my_b_tell(log)))
    {
      if (info->errmsg)
      {
        info->error= ER_UNKNOWN_ERROR;
        return 1;
      }
      info->should_stop= true;
      return 0;
    }

    /* Abort server before it sends the XID_EVENT */
    DBUG_EXECUTE_IF("crash_before_send_xid",
                    {
                      if (event_type == XID_EVENT)
                      {
                        my_sleep(2000000);
                        DBUG_SUICIDE();
                      }
                    });
  }

  return 0;
}

/**
 * This function sends one binlog file to slave
 *
 * return 0 - OK
 *        1 - NOK
 */
static int send_one_binlog_file(binlog_send_info *info,
                                IO_CACHE* log,
                                LOG_INFO* linfo,
                                my_off_t start_pos)
{
  mysql_mutex_assert_not_owner(mysql_bin_log.get_log_lock());

  /* seek to the requested position, to start the requested dump */
  if (start_pos != BIN_LOG_HEADER_SIZE)
  {
    my_b_seek(log, start_pos);
    linfo->pos= start_pos;
  }

  while (!should_stop(info))
  {
    /**
     * get end pos of current log file, this function
     * will wait if there is nothing available
     */
    my_off_t end_pos= get_binlog_end_pos(info, log, linfo);
    if (end_pos <= 1)
    {
      /** end of file or error */
      return end_pos;
    }

    /**
     * send events from current position up to end_pos
     */
    if (send_events(info, log, linfo, end_pos))
      return 1;
  }

  return 1;
}

void mysql_binlog_send(THD* thd, char* log_ident, my_off_t pos,
                       ushort flags)
{
  LOG_INFO linfo;

  IO_CACHE log;
  File file = -1;
  String* const packet= &thd->packet;

  binlog_send_info infoobj(thd, packet, flags, linfo.log_file_name);
  binlog_send_info *info= &infoobj;

  int old_max_allowed_packet= thd->variables.max_allowed_packet;
  thd->variables.max_allowed_packet= MAX_MAX_ALLOWED_PACKET;

  DBUG_ENTER("mysql_binlog_send");
  DBUG_PRINT("enter",("log_ident: '%s'  pos: %ld", log_ident, (long) pos));

  bzero((char*) &log,sizeof(log));

  if (init_binlog_sender(info, &linfo, log_ident, &pos))
    goto err;

  /*
     run hook first when all check has been made that slave seems to
     be requesting a reasonable position. i.e when transmit actually starts
  */
  if (RUN_HOOK(binlog_transmit, transmit_start, (thd, flags, log_ident, pos)))
  {
    info->errmsg= "Failed to run hook 'transmit_start'";
    info->error= ER_UNKNOWN_ERROR;
    goto err;
  }

  /*
    heartbeat_period from @master_heartbeat_period user variable
    NOTE: this is initialized after transmit_start-hook so that
    the hook can affect value of heartbeat period
  */
  info->heartbeat_period= get_heartbeat_period(thd);

  while (!should_stop(info))
  {
    /*
      Tell the client about the log name with a fake Rotate event;
      this is needed even if we also send a Format_description_log_event
      just after, because that event does not contain the binlog's name.
      Note that as this Rotate event is sent before
      Format_description_log_event, the slave cannot have any info to
      understand this event's format, so the header len of
      Rotate_log_event is FROZEN (so in 5.0 it will have a header shorter
      than other events except FORMAT_DESCRIPTION_EVENT).
      Before 4.0.14 we called fake_rotate_event below only if (pos ==
      BIN_LOG_HEADER_SIZE), because if this is false then the slave
      already knows the binlog's name.
      Since, we always call fake_rotate_event; if the slave already knew
      the log's name (ex: CHANGE MASTER TO MASTER_LOG_FILE=...) this is
      useless but does not harm much. It is nice for 3.23 (>=.58) slaves
      which test Rotate events to see if the master is 4.0 (then they
      choose to stop because they can't replicate 4.0); by always calling
      fake_rotate_event we are sure that 3.23.58 and newer will detect the
      problem as soon as replication starts (BUG#198).
      Always calling fake_rotate_event makes sending of normal
      (=from-binlog) Rotate events a priori unneeded, but it is not so
      simple: the 2 Rotate events are not equivalent, the normal one is
      before the Stop event, the fake one is after. If we don't send the
      normal one, then the Stop event will be interpreted (by existing 4.0
      slaves) as "the master stopped", which is wrong. So for safety,
      given that we want minimum modification of 4.0, we send the normal
      and fake Rotates.
    */
    if (fake_rotate_event(info, pos, &info->errmsg, info->current_checksum_alg))
    {
      /*
        This error code is not perfect, as fake_rotate_event() does not
        read anything from the binlog; if it fails it's because of an
        error in my_net_write(), fortunately it will say so in errmsg.
      */
      info->error= ER_MASTER_FATAL_ERROR_READING_BINLOG;
      goto err;
    }

    if ((file=open_binlog(&log, linfo.log_file_name, &info->errmsg)) < 0)
    {
      info->error= ER_MASTER_FATAL_ERROR_READING_BINLOG;
      goto err;
    }

    if (send_format_descriptor_event(info, &log, &linfo, pos))
    {
      info->error= ER_MASTER_FATAL_ERROR_READING_BINLOG;
      goto err;
    }

    /*
      We want to corrupt the first event that will be sent to the slave.
      But we do not want the corruption to happen early, eg. when client does
      BINLOG_GTID_POS(). So test case sets a DBUG trigger which causes us to
      set the real DBUG injection here.
    */
    DBUG_EXECUTE_IF("corrupt_read_log_event2_set",
                    {
                      DBUG_SET("-d,corrupt_read_log_event2_set");
                      DBUG_SET("+d,corrupt_read_log_event2");
                    });

    /*
      Handle the case of START SLAVE UNTIL with an UNTIL condition already
      fulfilled at the start position.

      We will send one event, the format_description, and then stop.
    */
    if (info->until_gtid_state && info->until_gtid_state->count() == 0)
      info->gtid_until_group= GTID_UNTIL_STOP_AFTER_STANDALONE;

    THD_STAGE_INFO(thd, stage_sending_binlog_event_to_slave);
    if (send_one_binlog_file(info, &log, &linfo, pos))
      break;

    if (should_stop(info))
      break;

    DBUG_EXECUTE_IF("wait_after_binlog_EOF",
                    {
                      const char act[]= "now wait_for signal.rotate_finished";
                      DBUG_ASSERT(!debug_sync_set_action(current_thd,
                                                         STRING_WITH_LEN(act)));
                    };);

    THD_STAGE_INFO(thd,
                   stage_finished_reading_one_binlog_switching_to_next_binlog);
    if (mysql_bin_log.find_next_log(&linfo, 1))
    {
      info->errmsg= "could not find next log";
      info->error= ER_MASTER_FATAL_ERROR_READING_BINLOG;
      break;
    }

    /** start from start of next file */
    pos= BIN_LOG_HEADER_SIZE;

    /** close current cache/file */
    end_io_cache(&log);
    mysql_file_close(file, MYF(MY_WME));
    file= -1;
  }

err:
  THD_STAGE_INFO(thd, stage_waiting_to_finalize_termination);
  RUN_HOOK(binlog_transmit, transmit_stop, (thd, flags));

  const bool binlog_open = my_b_inited(&log);
  if (file >= 0)
  {
    end_io_cache(&log);
    mysql_file_close(file, MYF(MY_WME));
  }

  mysql_mutex_lock(&LOCK_thread_count);
  thd->current_linfo = 0;
  mysql_mutex_unlock(&LOCK_thread_count);
  thd->variables.max_allowed_packet= old_max_allowed_packet;
  delete info->fdev;

  if (info->error == ER_MASTER_FATAL_ERROR_READING_BINLOG && binlog_open)
  {
    /*
       detailing the fatal error message with coordinates
       of the last position read.
    */
    my_snprintf(info->error_text, sizeof(info->error_text),
                "%s; the first event '%s' at %lld, "
                "the last event read from '%s' at %lld, "
                "the last byte read from '%s' at %lld.",
                info->errmsg,
                my_basename(info->start_log_file_name), info->start_pos,
                my_basename(info->log_file_name), info->last_pos,
                my_basename(info->log_file_name), linfo.pos);
  }
  else if (info->error == ER_GTID_POSITION_NOT_FOUND_IN_BINLOG)
  {
    my_snprintf(info->error_text, sizeof(info->error_text),
                "Error: connecting slave requested to start from GTID "
                "%u-%u-%llu, which is not in the master's binlog",
                info->error_gtid.domain_id,
                info->error_gtid.server_id,
                info->error_gtid.seq_no);
    /* Use this error code so slave will know not to try reconnect. */
    info->error= ER_MASTER_FATAL_ERROR_READING_BINLOG;
  }
  else if (info->error == ER_GTID_POSITION_NOT_FOUND_IN_BINLOG2)
  {
    my_snprintf(info->error_text, sizeof(info->error_text),
                "Error: connecting slave requested to start from GTID "
                "%u-%u-%llu, which is not in the master's binlog. Since the "
                "master's binlog contains GTIDs with higher sequence numbers, "
                "it probably means that the slave has diverged due to "
                "executing extra erroneous transactions",
                info->error_gtid.domain_id,
                info->error_gtid.server_id,
                info->error_gtid.seq_no);
    /* Use this error code so slave will know not to try reconnect. */
    info->error= ER_MASTER_FATAL_ERROR_READING_BINLOG;
  }
  else if (info->error == ER_GTID_START_FROM_BINLOG_HOLE)
  {
    my_snprintf(info->error_text, sizeof(info->error_text),
                "The binlog on the master is missing the GTID %u-%u-%llu "
                "requested by the slave (even though both a prior and a "
                "subsequent sequence number does exist), and GTID strict mode "
                "is enabled",
                info->error_gtid.domain_id,
                info->error_gtid.server_id,
                info->error_gtid.seq_no);
    /* Use this error code so slave will know not to try reconnect. */
    info->error= ER_MASTER_FATAL_ERROR_READING_BINLOG;
  }
  else if (info->error == ER_CANNOT_LOAD_SLAVE_GTID_STATE)
  {
    my_snprintf(info->error_text, sizeof(info->error_text),
                "Failed to load replication slave GTID state from table %s.%s",
                "mysql", rpl_gtid_slave_state_table_name.str);
    info->error= ER_MASTER_FATAL_ERROR_READING_BINLOG;
  }
  else if (info->error != 0 && info->errmsg != NULL)
    strcpy(info->error_text, info->errmsg);

  if (info->error == 0)
  {
    my_eof(thd);
  }
  else
  {
    my_message(info->error, info->error_text, MYF(0));
  }

  DBUG_VOID_RETURN;
}


/**
  Execute a START SLAVE statement.

  @param thd Pointer to THD object for the client thread executing the
  statement.

  @param mi Pointer to Master_info object for the slave's IO thread.

  @param net_report If true, saves the exit status into thd->stmt_da.

  @retval 0 success
  @retval 1 error
  @retval -1 fatal error
*/

int start_slave(THD* thd , Master_info* mi,  bool net_report)
{
  int slave_errno= 0;
  int thread_mask;
  char master_info_file_tmp[FN_REFLEN];
  char relay_log_info_file_tmp[FN_REFLEN];
  DBUG_ENTER("start_slave");

  if (check_access(thd, SUPER_ACL, any_db, NULL, NULL, 0, 0))
    DBUG_RETURN(-1);

  create_logfile_name_with_suffix(master_info_file_tmp,
                                  sizeof(master_info_file_tmp),
                                  master_info_file, 0,
                                  &mi->cmp_connection_name);
  create_logfile_name_with_suffix(relay_log_info_file_tmp,
                                  sizeof(relay_log_info_file_tmp),
                                  relay_log_info_file, 0,
                                  &mi->cmp_connection_name);

  lock_slave_threads(mi);  // this allows us to cleanly read slave_running
  // Get a mask of _stopped_ threads
  init_thread_mask(&thread_mask,mi,1 /* inverse */);

  if (thd->lex->mi.gtid_pos_str.str)
  {
    if (thread_mask != (SLAVE_IO|SLAVE_SQL))
    {
      slave_errno= ER_SLAVE_WAS_RUNNING;
      goto err;
    }
    if (thd->lex->slave_thd_opt)
    {
      slave_errno= ER_BAD_SLAVE_UNTIL_COND;
      goto err;
    }
    if (mi->using_gtid == Master_info::USE_GTID_NO)
    {
      slave_errno= ER_UNTIL_REQUIRES_USING_GTID;
      goto err;
    }
  }

  /*
    Below we will start all stopped threads.  But if the user wants to
    start only one thread, do as if the other thread was running (as we
    don't wan't to touch the other thread), so set the bit to 0 for the
    other thread
  */
  if (thd->lex->slave_thd_opt)
    thread_mask&= thd->lex->slave_thd_opt;
  if (thread_mask) //some threads are stopped, start them
  {
    if (init_master_info(mi,master_info_file_tmp,relay_log_info_file_tmp, 0,
			 thread_mask))
      slave_errno=ER_MASTER_INFO;
    else if (!server_id_supplied)
    {
      slave_errno= ER_BAD_SLAVE; net_report= 0;
      my_message(slave_errno, "Misconfigured slave: server_id was not set; Fix in config file",
                   MYF(0));
    }
    else if (!*mi->host)
    {
      slave_errno= ER_BAD_SLAVE; net_report= 0;
      my_message(slave_errno, "Misconfigured slave: MASTER_HOST was not set; Fix in config file or with CHANGE MASTER TO",
                 MYF(0));
    }
    else
    {
      /*
        If we will start SQL thread we will care about UNTIL options If
        not and they are specified we will ignore them and warn user
        about this fact.
      */
      if (thread_mask & SLAVE_SQL)
      {
        mysql_mutex_lock(&mi->rli.data_lock);

        if (thd->lex->mi.pos)
        {
          if (thd->lex->mi.relay_log_pos)
            slave_errno=ER_BAD_SLAVE_UNTIL_COND;
          mi->rli.until_condition= Relay_log_info::UNTIL_MASTER_POS;
          mi->rli.until_log_pos= thd->lex->mi.pos;
          /*
             We don't check thd->lex->mi.log_file_name for NULL here
             since it is checked in sql_yacc.yy
          */
          strmake_buf(mi->rli.until_log_name, thd->lex->mi.log_file_name);
        }
        else if (thd->lex->mi.relay_log_pos)
        {
          mi->rli.until_condition= Relay_log_info::UNTIL_RELAY_POS;
          mi->rli.until_log_pos= thd->lex->mi.relay_log_pos;
          strmake_buf(mi->rli.until_log_name, thd->lex->mi.relay_log_name);
        }
        else if (thd->lex->mi.gtid_pos_str.str)
        {
          if (mi->rli.until_gtid_pos.load(thd->lex->mi.gtid_pos_str.str,
                                          thd->lex->mi.gtid_pos_str.length))
          {
            slave_errno= ER_INCORRECT_GTID_STATE;
            mysql_mutex_unlock(&mi->rli.data_lock);
            goto err;
          }
          mi->rli.until_condition= Relay_log_info::UNTIL_GTID;
        }
        else
          mi->rli.clear_until_condition();

        if (mi->rli.until_condition == Relay_log_info::UNTIL_MASTER_POS ||
            mi->rli.until_condition == Relay_log_info::UNTIL_RELAY_POS)
        {
          /* Preparing members for effective until condition checking */
          const char *p= fn_ext(mi->rli.until_log_name);
          char *p_end;
          if (*p)
          {
            //p points to '.'
            mi->rli.until_log_name_extension= strtoul(++p,&p_end, 10);
            /*
              p_end points to the first invalid character. If it equals
              to p, no digits were found, error. If it contains '\0' it
              means  conversion went ok.
            */
            if (p_end==p || *p_end)
              slave_errno=ER_BAD_SLAVE_UNTIL_COND;
          }
          else
            slave_errno=ER_BAD_SLAVE_UNTIL_COND;

          /* mark the cached result of the UNTIL comparison as "undefined" */
          mi->rli.until_log_names_cmp_result=
            Relay_log_info::UNTIL_LOG_NAMES_CMP_UNKNOWN;
        }

        if (mi->rli.until_condition != Relay_log_info::UNTIL_NONE)
        {
          /* Issuing warning then started without --skip-slave-start */
          if (!opt_skip_slave_start)
            push_warning(thd, Sql_condition::WARN_LEVEL_NOTE,
                         ER_MISSING_SKIP_SLAVE,
                         ER_THD(thd, ER_MISSING_SKIP_SLAVE));
        }

        mysql_mutex_unlock(&mi->rli.data_lock);
      }
      else if (thd->lex->mi.pos || thd->lex->mi.relay_log_pos)
        push_warning(thd,
                     Sql_condition::WARN_LEVEL_NOTE, ER_UNTIL_COND_IGNORED,
                     ER_THD(thd, ER_UNTIL_COND_IGNORED));

      if (!slave_errno)
        slave_errno = start_slave_threads(thd,
                                          0 /*no mutex */,
                                          1 /* wait for start */,
                                          mi,
                                          master_info_file_tmp,
                                          relay_log_info_file_tmp,
                                          thread_mask);
    }
  }
  else
  {
    /* no error if all threads are already started, only a warning */
    push_warning(thd, Sql_condition::WARN_LEVEL_NOTE, ER_SLAVE_WAS_RUNNING,
                 ER_THD(thd, ER_SLAVE_WAS_RUNNING));
  }

err:
  unlock_slave_threads(mi);
  thd_proc_info(thd, 0);

  if (slave_errno)
  {
    if (net_report)
      my_error(slave_errno, MYF(0),
               (int) mi->connection_name.length,
               mi->connection_name.str);
    DBUG_RETURN(slave_errno == ER_BAD_SLAVE ? -1 : 1);
  }

  DBUG_RETURN(0);
}


/**
  Execute a STOP SLAVE statement.

  @param thd Pointer to THD object for the client thread executing the
  statement.

  @param mi Pointer to Master_info object for the slave's IO thread.

  @param net_report If true, saves the exit status into thd->stmt_da.

  @retval 0 success
  @retval 1 error
  @retval -1 error
*/

int stop_slave(THD* thd, Master_info* mi, bool net_report )
{
  int slave_errno;
  DBUG_ENTER("stop_slave");
  DBUG_PRINT("enter",("Connection: %s", mi->connection_name.str));

  if (check_access(thd, SUPER_ACL, any_db, NULL, NULL, 0, 0))
    DBUG_RETURN(-1);
  THD_STAGE_INFO(thd, stage_killing_slave);
  int thread_mask;
  lock_slave_threads(mi);
  // Get a mask of _running_ threads
  init_thread_mask(&thread_mask,mi,0 /* not inverse*/);
  /*
    Below we will stop all running threads.
    But if the user wants to stop only one thread, do as if the other thread
    was stopped (as we don't wan't to touch the other thread), so set the
    bit to 0 for the other thread
  */
  if (thd->lex->slave_thd_opt)
    thread_mask &= thd->lex->slave_thd_opt;

  if (thread_mask)
  {
    slave_errno= terminate_slave_threads(mi,thread_mask,
                                         1 /*skip lock */);
  }
  else
  {
    //no error if both threads are already stopped, only a warning
    slave_errno= 0;
    push_warning(thd, Sql_condition::WARN_LEVEL_NOTE, ER_SLAVE_WAS_NOT_RUNNING,
                 ER_THD(thd, ER_SLAVE_WAS_NOT_RUNNING));
  }
  unlock_slave_threads(mi);

  if (slave_errno)
  {
    if (net_report)
      my_message(slave_errno, ER_THD(thd, slave_errno), MYF(0));
    DBUG_RETURN(1);
  }

  DBUG_RETURN(0);
}


/**
  Execute a RESET SLAVE statement.

  @param thd Pointer to THD object of the client thread executing the
  statement.

  @param mi Pointer to Master_info object for the slave.

  @retval 0 success
  @retval 1 error
*/
int reset_slave(THD *thd, Master_info* mi)
{
  MY_STAT stat_area;
  char fname[FN_REFLEN];
  int thread_mask= 0, error= 0;
  uint sql_errno=ER_UNKNOWN_ERROR;
  const char* errmsg= "Unknown error occurred while reseting slave";
  char master_info_file_tmp[FN_REFLEN];
  char relay_log_info_file_tmp[FN_REFLEN];
  DBUG_ENTER("reset_slave");

  lock_slave_threads(mi);
  init_thread_mask(&thread_mask,mi,0 /* not inverse */);
  if (thread_mask) // We refuse if any slave thread is running
  {
    unlock_slave_threads(mi);
    my_error(ER_SLAVE_MUST_STOP, MYF(0), (int) mi->connection_name.length,
             mi->connection_name.str);
    DBUG_RETURN(ER_SLAVE_MUST_STOP);
  }

  // delete relay logs, clear relay log coordinates
  if ((error= purge_relay_logs(&mi->rli, thd,
			       1 /* just reset */,
			       &errmsg)))
  {
    sql_errno= ER_RELAY_LOG_FAIL;
    goto err;
  }

  /* Clear master's log coordinates and associated information */
  mi->clear_in_memory_info(thd->lex->reset_slave_info.all);

  /*
     Reset errors (the idea is that we forget about the
     old master).
  */
  mi->clear_error();
  mi->rli.clear_error();
  mi->rli.clear_until_condition();
  mi->rli.slave_skip_counter= 0;

  // close master_info_file, relay_log_info_file, set mi->inited=rli->inited=0
  end_master_info(mi);

  // and delete these two files
  create_logfile_name_with_suffix(master_info_file_tmp,
                                  sizeof(master_info_file_tmp),
                                  master_info_file, 0,
                                  &mi->cmp_connection_name);
  create_logfile_name_with_suffix(relay_log_info_file_tmp,
                                  sizeof(relay_log_info_file_tmp),
                                  relay_log_info_file, 0,
                                  &mi->cmp_connection_name);

  fn_format(fname, master_info_file_tmp, mysql_data_home, "", 4+32);
  if (mysql_file_stat(key_file_master_info, fname, &stat_area, MYF(0)) &&
      mysql_file_delete(key_file_master_info, fname, MYF(MY_WME)))
  {
    error=1;
    goto err;
  }
  else if (global_system_variables.log_warnings > 1)
    sql_print_information("Deleted Master_info file '%s'.", fname);

  // delete relay_log_info_file
  fn_format(fname, relay_log_info_file_tmp, mysql_data_home, "", 4+32);
  if (mysql_file_stat(key_file_relay_log_info, fname, &stat_area, MYF(0)) &&
      mysql_file_delete(key_file_relay_log_info, fname, MYF(MY_WME)))
  {
    error=1;
    goto err;
  }
  else if (global_system_variables.log_warnings > 1)
    sql_print_information("Deleted Master_info file '%s'.", fname);

  RUN_HOOK(binlog_relay_io, after_reset_slave, (thd, mi));
err:
  unlock_slave_threads(mi);
  if (error)
    my_error(sql_errno, MYF(0), errmsg);
  DBUG_RETURN(error);
}

/*

  Kill all Binlog_dump threads which previously talked to the same slave
  ("same" means with the same server id). Indeed, if the slave stops, if the
  Binlog_dump thread is waiting (mysql_cond_wait) for binlog update, then it
  will keep existing until a query is written to the binlog. If the master is
  idle, then this could last long, and if the slave reconnects, we could have 2
  Binlog_dump threads in SHOW PROCESSLIST, until a query is written to the
  binlog. To avoid this, when the slave reconnects and sends COM_BINLOG_DUMP,
  the master kills any existing thread with the slave's server id (if this id
  is not zero; it will be true for real slaves, but false for mysqlbinlog when
  it sends COM_BINLOG_DUMP to get a remote binlog dump).

  SYNOPSIS
    kill_zombie_dump_threads()
    slave_server_id     the slave's server id

*/


void kill_zombie_dump_threads(uint32 slave_server_id)
{
  mysql_mutex_lock(&LOCK_thread_count);
  I_List_iterator<THD> it(threads);
  THD *tmp;

  while ((tmp=it++))
  {
    if (tmp->get_command() == COM_BINLOG_DUMP &&
       tmp->variables.server_id == slave_server_id)
    {
      mysql_mutex_lock(&tmp->LOCK_thd_data);    // Lock from delete
      break;
    }
  }
  mysql_mutex_unlock(&LOCK_thread_count);
  if (tmp)
  {
    /*
      Here we do not call kill_one_thread() as
      it will be slow because it will iterate through the list
      again. We just to do kill the thread ourselves.
    */
    tmp->awake(KILL_QUERY);
    mysql_mutex_unlock(&tmp->LOCK_thd_data);
  }
}

/**
   Get value for a string parameter with error checking

   Note that in case of error the original string should not be updated!

   @ret 0 ok
   @ret 1 error
*/

static bool get_string_parameter(char *to, const char *from, size_t length,
                                 const char *name, CHARSET_INFO *cs)
{
  if (from)                                     // Empty paramaters allowed
  {
    size_t from_length= strlen(from);
    uint from_numchars= cs->cset->numchars(cs, from, from + from_length);
    if (from_numchars > length / cs->mbmaxlen)
    {
      my_error(ER_WRONG_STRING_LENGTH, MYF(0), from, name, length / cs->mbmaxlen);
      return 1;
    }
    memcpy(to, from, from_length+1);
  }
  return 0;
}


/**
  Execute a CHANGE MASTER statement.

  @param thd Pointer to THD object for the client thread executing the
  statement.

  @param mi Pointer to Master_info object belonging to the slave's IO
  thread.

  @param master_info_added Out parameter saying if the Master_info *mi was
  added to the global list of masters. This is useful in error conditions
  to know if caller should free Master_info *mi.

  @retval FALSE success
  @retval TRUE error
*/
bool change_master(THD* thd, Master_info* mi, bool *master_info_added)
{
  int thread_mask;
  const char* errmsg= 0;
  bool need_relay_log_purge= 1;
  bool ret= FALSE;
  char saved_host[HOSTNAME_LENGTH + 1];
  uint saved_port;
  char saved_log_name[FN_REFLEN];
  Master_info::enum_using_gtid saved_using_gtid;
  char master_info_file_tmp[FN_REFLEN];
  char relay_log_info_file_tmp[FN_REFLEN];
  my_off_t saved_log_pos;
  LEX_MASTER_INFO* lex_mi= &thd->lex->mi;
  DYNAMIC_ARRAY *do_ids, *ignore_ids;

  DBUG_ENTER("change_master");

  mysql_mutex_assert_owner(&LOCK_active_mi);
  DBUG_ASSERT(master_info_index);

  *master_info_added= false;
  /* 
    We need to check if there is an empty master_host. Otherwise
    change master succeeds, a master.info file is created containing 
    empty master_host string and when issuing: start slave; an error
    is thrown stating that the server is not configured as slave.
    (See BUG#28796).
  */
  if (lex_mi->host && !*lex_mi->host) 
  {
    my_error(ER_WRONG_ARGUMENTS, MYF(0), "MASTER_HOST");
    DBUG_RETURN(TRUE);
  }
  if (master_info_index->check_duplicate_master_info(&lex_mi->connection_name,
                                                     lex_mi->host,
                                                     lex_mi->port))
    DBUG_RETURN(TRUE);

  lock_slave_threads(mi);
  init_thread_mask(&thread_mask,mi,0 /*not inverse*/);
  if (thread_mask) // We refuse if any slave thread is running
  {
    my_error(ER_SLAVE_MUST_STOP, MYF(0), (int) mi->connection_name.length,
             mi->connection_name.str);
    ret= TRUE;
    goto err;
  }

  THD_STAGE_INFO(thd, stage_changing_master);

  create_logfile_name_with_suffix(master_info_file_tmp,
                                  sizeof(master_info_file_tmp),
                                  master_info_file, 0,
                                  &mi->cmp_connection_name);
  create_logfile_name_with_suffix(relay_log_info_file_tmp,
                                  sizeof(relay_log_info_file_tmp),
                                  relay_log_info_file, 0,
                                  &mi->cmp_connection_name);

  /* if new Master_info doesn't exists, add it */
  if (!master_info_index->get_master_info(&mi->connection_name,
                                          Sql_condition::WARN_LEVEL_NOTE))
  {
    if (master_info_index->add_master_info(mi, TRUE))
    {
      my_error(ER_MASTER_INFO, MYF(0),
               (int) lex_mi->connection_name.length,
               lex_mi->connection_name.str);
      ret= TRUE;
      goto err;
    }
    *master_info_added= true;
  }
  if (global_system_variables.log_warnings > 1)
    sql_print_information("Master connection name: '%.*s'  "
                          "Master_info_file: '%s'  "
                          "Relay_info_file: '%s'",
                          (int) mi->connection_name.length,
                          mi->connection_name.str,
                          master_info_file_tmp, relay_log_info_file_tmp);

  if (init_master_info(mi, master_info_file_tmp, relay_log_info_file_tmp, 0,
		       thread_mask))
  {
    my_error(ER_MASTER_INFO, MYF(0),
             (int) lex_mi->connection_name.length,
             lex_mi->connection_name.str);
    ret= TRUE;
    goto err;
  }

  /*
    Data lock not needed since we have already stopped the running threads,
    and we have the hold on the run locks which will keep all threads that
    could possibly modify the data structures from running
  */

  /*
    Before processing the command, save the previous state.
  */
  strmake_buf(saved_host, mi->host);
  saved_port= mi->port;
  strmake_buf(saved_log_name, mi->master_log_name);
  saved_log_pos= mi->master_log_pos;
  saved_using_gtid= mi->using_gtid;

  /*
    If the user specified host or port without binlog or position,
    reset binlog's name to FIRST and position to 4.
  */

  if ((lex_mi->host || lex_mi->port) && !lex_mi->log_file_name && !lex_mi->pos)
  {
    mi->master_log_name[0] = 0;
    mi->master_log_pos= BIN_LOG_HEADER_SIZE;
  }

  if (lex_mi->log_file_name)
    strmake_buf(mi->master_log_name, lex_mi->log_file_name);
  if (lex_mi->pos)
  {
    mi->master_log_pos= lex_mi->pos;
  }
  DBUG_PRINT("info", ("master_log_pos: %lu", (ulong) mi->master_log_pos));

  if (get_string_parameter(mi->host, lex_mi->host, sizeof(mi->host)-1,
                           "MASTER_HOST", system_charset_info) ||
      get_string_parameter(mi->user, lex_mi->user, sizeof(mi->user)-1,
                           "MASTER_USER", system_charset_info) ||
      get_string_parameter(mi->password, lex_mi->password,
                           sizeof(mi->password)-1, "MASTER_PASSWORD",
                           &my_charset_bin))
  {
    ret= TRUE;
    goto err;
  }

  if (lex_mi->port)
    mi->port = lex_mi->port;
  if (lex_mi->connect_retry)
    mi->connect_retry = lex_mi->connect_retry;
  if (lex_mi->heartbeat_opt != LEX_MASTER_INFO::LEX_MI_UNCHANGED)
    mi->heartbeat_period = lex_mi->heartbeat_period;
  else
    mi->heartbeat_period= (float) MY_MIN(SLAVE_MAX_HEARTBEAT_PERIOD,
                                      (slave_net_timeout/2.0));
  mi->received_heartbeats= 0; // counter lives until master is CHANGEd

  /*
    Reset the last time server_id list if the current CHANGE MASTER
    is mentioning IGNORE_SERVER_IDS= (...)
  */
  if (lex_mi->repl_ignore_server_ids_opt == LEX_MASTER_INFO::LEX_MI_ENABLE)
  {
    /* Check if the list contains replicate_same_server_id */
    for (uint i= 0; i < lex_mi->repl_ignore_server_ids.elements; i ++)
    {
      ulong s_id;
      get_dynamic(&lex_mi->repl_ignore_server_ids, (uchar*) &s_id, i);
      if (s_id == global_system_variables.server_id && replicate_same_server_id)
      {
        my_error(ER_SLAVE_IGNORE_SERVER_IDS, MYF(0), static_cast<int>(s_id));
        ret= TRUE;
        goto err;
      }
    }

    /* All ok. Update the old server ids with the new ones. */
    update_change_master_ids(&lex_mi->repl_ignore_server_ids,
                             &mi->ignore_server_ids);
  }

  if (lex_mi->ssl != LEX_MASTER_INFO::LEX_MI_UNCHANGED)
    mi->ssl= (lex_mi->ssl == LEX_MASTER_INFO::LEX_MI_ENABLE);

  if (lex_mi->ssl_verify_server_cert != LEX_MASTER_INFO::LEX_MI_UNCHANGED)
    mi->ssl_verify_server_cert=
      (lex_mi->ssl_verify_server_cert == LEX_MASTER_INFO::LEX_MI_ENABLE);

  if (lex_mi->ssl_ca)
    strmake_buf(mi->ssl_ca, lex_mi->ssl_ca);
  if (lex_mi->ssl_capath)
    strmake_buf(mi->ssl_capath, lex_mi->ssl_capath);
  if (lex_mi->ssl_cert)
    strmake_buf(mi->ssl_cert, lex_mi->ssl_cert);
  if (lex_mi->ssl_cipher)
    strmake_buf(mi->ssl_cipher, lex_mi->ssl_cipher);
  if (lex_mi->ssl_key)
    strmake_buf(mi->ssl_key, lex_mi->ssl_key);
  if (lex_mi->ssl_crl)
    strmake_buf(mi->ssl_crl, lex_mi->ssl_crl);
  if (lex_mi->ssl_crlpath)
    strmake_buf(mi->ssl_crlpath, lex_mi->ssl_crlpath);

#ifndef HAVE_OPENSSL
  if (lex_mi->ssl || lex_mi->ssl_ca || lex_mi->ssl_capath ||
      lex_mi->ssl_cert || lex_mi->ssl_cipher || lex_mi->ssl_key ||
      lex_mi->ssl_verify_server_cert || lex_mi->ssl_crl || lex_mi->ssl_crlpath)
    push_warning(thd, Sql_condition::WARN_LEVEL_NOTE,
                 ER_SLAVE_IGNORED_SSL_PARAMS,
                 ER_THD(thd, ER_SLAVE_IGNORED_SSL_PARAMS));
#endif

  if (lex_mi->relay_log_name)
  {
    need_relay_log_purge= 0;
    char relay_log_name[FN_REFLEN];
    mi->rli.relay_log.make_log_name(relay_log_name, lex_mi->relay_log_name);
    strmake_buf(mi->rli.group_relay_log_name, relay_log_name);
    strmake_buf(mi->rli.event_relay_log_name, relay_log_name);
  }

  if (lex_mi->relay_log_pos)
  {
    need_relay_log_purge= 0;
    mi->rli.group_relay_log_pos= mi->rli.event_relay_log_pos= lex_mi->relay_log_pos;
  }

  if (lex_mi->use_gtid_opt == LEX_MASTER_INFO::LEX_GTID_SLAVE_POS)
    mi->using_gtid= Master_info::USE_GTID_SLAVE_POS;
  else if (lex_mi->use_gtid_opt == LEX_MASTER_INFO::LEX_GTID_CURRENT_POS)
    mi->using_gtid= Master_info::USE_GTID_CURRENT_POS;
  else if (lex_mi->use_gtid_opt == LEX_MASTER_INFO::LEX_GTID_NO ||
           lex_mi->log_file_name || lex_mi->pos ||
           lex_mi->relay_log_name || lex_mi->relay_log_pos)
    mi->using_gtid= Master_info::USE_GTID_NO;

  do_ids= ((lex_mi->repl_do_domain_ids_opt ==
            LEX_MASTER_INFO::LEX_MI_ENABLE) ?
           &lex_mi->repl_do_domain_ids : NULL);

  ignore_ids= ((lex_mi->repl_ignore_domain_ids_opt ==
                LEX_MASTER_INFO::LEX_MI_ENABLE) ?
               &lex_mi->repl_ignore_domain_ids : NULL);

  /*
    Note: mi->using_gtid stores the previous state in case no MASTER_USE_GTID
    is specified.
  */
  if (mi->domain_id_filter.update_ids(do_ids, ignore_ids, mi->using_gtid))
  {
    my_error(ER_MASTER_INFO, MYF(0),
             (int) lex_mi->connection_name.length,
             lex_mi->connection_name.str);
    ret= TRUE;
    goto err;
  }

  /*
    If user did specify neither host nor port nor any log name nor any log
    pos, i.e. he specified only user/password/master_connect_retry, he probably
    wants replication to resume from where it had left, i.e. from the
    coordinates of the **SQL** thread (imagine the case where the I/O is ahead
    of the SQL; restarting from the coordinates of the I/O would lose some
    events which is probably unwanted when you are just doing minor changes
    like changing master_connect_retry).
    A side-effect is that if only the I/O thread was started, this thread may
    restart from ''/4 after the CHANGE MASTER. That's a minor problem (it is a
    much more unlikely situation than the one we are fixing here).
    Note: coordinates of the SQL thread must be read here, before the
    'if (need_relay_log_purge)' block which resets them.
  */
  if (!lex_mi->host && !lex_mi->port &&
      !lex_mi->log_file_name && !lex_mi->pos &&
      need_relay_log_purge)
   {
     /*
       Sometimes mi->rli.master_log_pos == 0 (it happens when the SQL thread is
       not initialized), so we use a MY_MAX().
       What happens to mi->rli.master_log_pos during the initialization stages
       of replication is not 100% clear, so we guard against problems using
       MY_MAX().
      */
     mi->master_log_pos = MY_MAX(BIN_LOG_HEADER_SIZE,
			      mi->rli.group_master_log_pos);
     strmake_buf(mi->master_log_name, mi->rli.group_master_log_name);
  }

  /*
    Relay log's IO_CACHE may not be inited, if rli->inited==0 (server was never
    a slave before).
  */
  if (flush_master_info(mi, FALSE, FALSE))
  {
    my_error(ER_RELAY_LOG_INIT, MYF(0), "Failed to flush master info file");
    ret= TRUE;
    goto err;
  }
  if (need_relay_log_purge)
  {
    THD_STAGE_INFO(thd, stage_purging_old_relay_logs);
    if (purge_relay_logs(&mi->rli, thd,
			 0 /* not only reset, but also reinit */,
			 &errmsg))
    {
      my_error(ER_RELAY_LOG_FAIL, MYF(0), errmsg);
      ret= TRUE;
      goto err;
    }
  }
  else
  {
    const char* msg;
    /* Relay log is already initialized */
    if (init_relay_log_pos(&mi->rli,
			   mi->rli.group_relay_log_name,
			   mi->rli.group_relay_log_pos,
			   0 /*no data lock*/,
			   &msg, 0))
    {
      my_error(ER_RELAY_LOG_INIT, MYF(0), msg);
      ret= TRUE;
      goto err;
    }
  }
  /*
    Coordinates in rli were spoilt by the 'if (need_relay_log_purge)' block,
    so restore them to good values. If we left them to ''/0, that would work;
    but that would fail in the case of 2 successive CHANGE MASTER (without a
    START SLAVE in between): because first one would set the coords in mi to
    the good values of those in rli, the set those in rli to ''/0, then
    second CHANGE MASTER would set the coords in mi to those of rli, i.e. to
    ''/0: we have lost all copies of the original good coordinates.
    That's why we always save good coords in rli.
  */
  mi->rli.group_master_log_pos= mi->master_log_pos;
  DBUG_PRINT("info", ("master_log_pos: %lu", (ulong) mi->master_log_pos));
  strmake_buf(mi->rli.group_master_log_name,mi->master_log_name);

  if (!mi->rli.group_master_log_name[0]) // uninitialized case
    mi->rli.group_master_log_pos=0;

  mysql_mutex_lock(&mi->rli.data_lock);
  mi->rli.abort_pos_wait++; /* for MASTER_POS_WAIT() to abort */
  /* Clear the errors, for a clean start */
  mi->rli.clear_error();
  mi->rli.clear_until_condition();
  mi->rli.slave_skip_counter= 0;

  sql_print_information("'CHANGE MASTER TO executed'. "
    "Previous state master_host='%s', master_port='%u', master_log_file='%s', "
    "master_log_pos='%ld'. "
    "New state master_host='%s', master_port='%u', master_log_file='%s', "
    "master_log_pos='%ld'.", saved_host, saved_port, saved_log_name,
    (ulong) saved_log_pos, mi->host, mi->port, mi->master_log_name,
    (ulong) mi->master_log_pos);
  if (saved_using_gtid != Master_info::USE_GTID_NO ||
      mi->using_gtid != Master_info::USE_GTID_NO)
    sql_print_information("Previous Using_Gtid=%s. New Using_Gtid=%s",
                          mi->using_gtid_astext(saved_using_gtid),
                          mi->using_gtid_astext(mi->using_gtid));

  /*
    If we don't write new coordinates to disk now, then old will remain in
    relay-log.info until START SLAVE is issued; but if mysqld is shutdown
    before START SLAVE, then old will remain in relay-log.info, and will be the
    in-memory value at restart (thus causing errors, as the old relay log does
    not exist anymore).
  */
  flush_relay_log_info(&mi->rli);
  mysql_cond_broadcast(&mi->data_cond);
  mysql_mutex_unlock(&mi->rli.data_lock);

err:
  unlock_slave_threads(mi);
  if (ret == FALSE)
    my_ok(thd);
  DBUG_RETURN(ret);
}


/**
  Execute a RESET MASTER statement.

  @param thd Pointer to THD object of the client thread executing the
  statement.

  @retval 0 success
  @retval 1 error
*/
int reset_master(THD* thd, rpl_gtid *init_state, uint32 init_state_len,
                 ulong next_log_number)
{
  if (!mysql_bin_log.is_open())
  {
    my_message(ER_FLUSH_MASTER_BINLOG_CLOSED,
               ER_THD(thd, ER_FLUSH_MASTER_BINLOG_CLOSED),
               MYF(ME_BELL+ME_WAITTANG));
    return 1;
  }

  if (mysql_bin_log.reset_logs(thd, 1, init_state, init_state_len,
                               next_log_number))
    return 1;
  RUN_HOOK(binlog_transmit, after_reset_master, (thd, 0 /* flags */));
  return 0;
}


/**
  Execute a SHOW BINLOG EVENTS statement.

  @param thd Pointer to THD object for the client thread executing the
  statement.

  @retval FALSE success
  @retval TRUE failure
*/
bool mysql_show_binlog_events(THD* thd)
{
  Protocol *protocol= thd->protocol;
  List<Item> field_list;
  const char *errmsg = 0;
  bool ret = TRUE;
  IO_CACHE log;
  File file = -1;
  MYSQL_BIN_LOG *binary_log= NULL;
  int old_max_allowed_packet= thd->variables.max_allowed_packet;
  Master_info *mi= 0;
  LOG_INFO linfo;
  LEX_MASTER_INFO *lex_mi= &thd->lex->mi;

  DBUG_ENTER("mysql_show_binlog_events");

  Log_event::init_show_field_list(thd, &field_list);
  if (protocol->send_result_set_metadata(&field_list,
                            Protocol::SEND_NUM_ROWS | Protocol::SEND_EOF))
    DBUG_RETURN(TRUE);

  Format_description_log_event *description_event= new
    Format_description_log_event(3); /* MySQL 4.0 by default */

  DBUG_ASSERT(thd->lex->sql_command == SQLCOM_SHOW_BINLOG_EVENTS ||
              thd->lex->sql_command == SQLCOM_SHOW_RELAYLOG_EVENTS);

  /* select which binary log to use: binlog or relay */
  if ( thd->lex->sql_command == SQLCOM_SHOW_BINLOG_EVENTS )
  {
    binary_log= &mysql_bin_log;
  }
  else  /* showing relay log contents */
  {
    if (!lex_mi->connection_name.str)
      lex_mi->connection_name= thd->variables.default_master_connection;
    mysql_mutex_lock(&LOCK_active_mi);
    if (!master_info_index ||
        !(mi= master_info_index->
          get_master_info(&lex_mi->connection_name,
                          Sql_condition::WARN_LEVEL_ERROR)))
    {
      mysql_mutex_unlock(&LOCK_active_mi);
      DBUG_RETURN(TRUE);
    }
    binary_log= &(mi->rli.relay_log);
  }

  if (binary_log->is_open())
  {
    SELECT_LEX_UNIT *unit= &thd->lex->unit;
    ha_rows event_count, limit_start, limit_end;
    my_off_t pos = MY_MAX(BIN_LOG_HEADER_SIZE, lex_mi->pos); // user-friendly
    char search_file_name[FN_REFLEN], *name;
    const char *log_file_name = lex_mi->log_file_name;
    mysql_mutex_t *log_lock = binary_log->get_log_lock();
    Log_event* ev;

    if (mi)
    {
      /* We can unlock the mutex as we have a lock on the file */
      mysql_mutex_unlock(&LOCK_active_mi);
      mi= 0;
    }

    unit->set_limit(thd->lex->current_select);
    limit_start= unit->offset_limit_cnt;
    limit_end= unit->select_limit_cnt;

    name= search_file_name;
    if (log_file_name)
      binary_log->make_log_name(search_file_name, log_file_name);
    else
      name=0;					// Find first log

    linfo.index_file_offset = 0;

    if (binary_log->find_log_pos(&linfo, name, 1))
    {
      errmsg = "Could not find target log";
      goto err;
    }

    mysql_mutex_lock(&LOCK_thread_count);
    thd->current_linfo = &linfo;
    mysql_mutex_unlock(&LOCK_thread_count);

    if ((file=open_binlog(&log, linfo.log_file_name, &errmsg)) < 0)
      goto err;

    /*
      to account binlog event header size
    */
    thd->variables.max_allowed_packet += MAX_LOG_EVENT_HEADER;

    mysql_mutex_lock(log_lock);

    /*
      open_binlog() sought to position 4.
      Read the first event in case it's a Format_description_log_event, to
      know the format. If there's no such event, we are 3.23 or 4.x. This
      code, like before, can't read 3.23 binlogs.
      Also read the second event, in case it's a Start_encryption_log_event.
      This code will fail on a mixed relay log (one which has Format_desc then
      Rotate then Format_desc).
    */

    my_off_t scan_pos = BIN_LOG_HEADER_SIZE;
    while (scan_pos < pos)
    {
      ev= Log_event::read_log_event(&log, (mysql_mutex_t*)0, description_event,
                                    opt_master_verify_checksum);
      scan_pos = my_b_tell(&log);
      if (ev == NULL || !ev->is_valid())
      {
        mysql_mutex_unlock(log_lock);
        errmsg = "Wrong offset or I/O error";
        goto err;
      }
      if (ev->get_type_code() == FORMAT_DESCRIPTION_EVENT)
      {
        delete description_event;
        description_event= (Format_description_log_event*) ev;
      }
      else
      {
        if (ev->get_type_code() == START_ENCRYPTION_EVENT)
        {
          if (description_event->start_decryption((Start_encryption_log_event*) ev))
          {
            delete ev;
            mysql_mutex_unlock(log_lock);
            errmsg = "Could not initialize decryption of binlog.";
            goto err;
          }
        }
        delete ev;
        break;
      }
    }

    my_b_seek(&log, pos);

    for (event_count = 0;
         (ev = Log_event::read_log_event(&log, (mysql_mutex_t*) 0,
                                         description_event,
                                         opt_master_verify_checksum)); )
    {
      if (event_count >= limit_start &&
	  ev->net_send(protocol, linfo.log_file_name, pos))
      {
	errmsg = "Net error";
	delete ev;
        mysql_mutex_unlock(log_lock);
	goto err;
      }

      if (ev->get_type_code() == FORMAT_DESCRIPTION_EVENT)
      {
        Format_description_log_event* new_fdle=
          (Format_description_log_event*) ev;
        new_fdle->copy_crypto_data(description_event);
        delete description_event;
        description_event= new_fdle;
      }
      else
      {
        if (ev->get_type_code() == START_ENCRYPTION_EVENT)
        {
          if (description_event->start_decryption((Start_encryption_log_event*) ev))
          {
            errmsg = "Error starting decryption";
            delete ev;
            mysql_mutex_unlock(log_lock);
            goto err;
          }
        }
        delete ev;
      }

      pos = my_b_tell(&log);

      if (++event_count >= limit_end)
	break;
    }

    if (event_count < limit_end && log.error)
    {
      errmsg = "Wrong offset or I/O error";
      mysql_mutex_unlock(log_lock);
      goto err;
    }

    mysql_mutex_unlock(log_lock);
  }
  else if (mi)
    mysql_mutex_unlock(&LOCK_active_mi);

  // Check that linfo is still on the function scope.
  DEBUG_SYNC(thd, "after_show_binlog_events");

  ret= FALSE;

err:
  delete description_event;
  if (file >= 0)
  {
    end_io_cache(&log);
    mysql_file_close(file, MYF(MY_WME));
  }

  if (errmsg)
    my_error(ER_ERROR_WHEN_EXECUTING_COMMAND, MYF(0),
             "SHOW BINLOG EVENTS", errmsg);
  else
    my_eof(thd);

  mysql_mutex_lock(&LOCK_thread_count);
  thd->current_linfo = 0;
  mysql_mutex_unlock(&LOCK_thread_count);
  thd->variables.max_allowed_packet= old_max_allowed_packet;
  DBUG_RETURN(ret);
}


void show_binlog_info_get_fields(THD *thd, List<Item> *field_list)
{
  MEM_ROOT *mem_root= thd->mem_root;
  field_list->push_back(new (mem_root)
                        Item_empty_string(thd, "File", FN_REFLEN),
                        mem_root);
  field_list->push_back(new (mem_root)
                        Item_return_int(thd, "Position", 20,
                                        MYSQL_TYPE_LONGLONG),
                        mem_root);
  field_list->push_back(new (mem_root)
                        Item_empty_string(thd, "Binlog_Do_DB", 255),
                        mem_root);
  field_list->push_back(new (mem_root)
                        Item_empty_string(thd, "Binlog_Ignore_DB", 255),
                        mem_root);
}


/**
  Execute a SHOW MASTER STATUS statement.

  @param thd Pointer to THD object for the client thread executing the
  statement.

  @retval FALSE success
  @retval TRUE failure
*/
bool show_binlog_info(THD* thd)
{
  Protocol *protocol= thd->protocol;
  DBUG_ENTER("show_binlog_info");

  List<Item> field_list;
  show_binlog_info_get_fields(thd, &field_list);

  if (protocol->send_result_set_metadata(&field_list,
                            Protocol::SEND_NUM_ROWS | Protocol::SEND_EOF))
    DBUG_RETURN(TRUE);
  protocol->prepare_for_resend();

  if (mysql_bin_log.is_open())
  {
    LOG_INFO li;
    mysql_bin_log.get_current_log(&li);
    int dir_len = dirname_length(li.log_file_name);
    protocol->store(li.log_file_name + dir_len, &my_charset_bin);
    protocol->store((ulonglong) li.pos);
    protocol->store(binlog_filter->get_do_db());
    protocol->store(binlog_filter->get_ignore_db());
    if (protocol->write())
      DBUG_RETURN(TRUE);
  }
  my_eof(thd);
  DBUG_RETURN(FALSE);
}


void show_binlogs_get_fields(THD *thd, List<Item> *field_list)
{
  MEM_ROOT *mem_root= thd->mem_root;
  field_list->push_back(new (mem_root)
                        Item_empty_string(thd, "Log_name", 255),
                        mem_root);
  field_list->push_back(new (mem_root)
                        Item_return_int(thd, "File_size", 20,
                                        MYSQL_TYPE_LONGLONG),
                        mem_root);
}


/**
  Execute a SHOW BINARY LOGS statement.

  @param thd Pointer to THD object for the client thread executing the
  statement.

  @retval FALSE success
  @retval TRUE failure
*/
bool show_binlogs(THD* thd)
{
  IO_CACHE *index_file;
  LOG_INFO cur;
  File file;
  char fname[FN_REFLEN];
  List<Item> field_list;
  uint length;
  int cur_dir_len;
  Protocol *protocol= thd->protocol;
  DBUG_ENTER("show_binlogs");

  if (!mysql_bin_log.is_open())
  {
    my_error(ER_NO_BINARY_LOGGING, MYF(0));
    DBUG_RETURN(TRUE);
  }

  show_binlogs_get_fields(thd, &field_list);

  if (protocol->send_result_set_metadata(&field_list,
                            Protocol::SEND_NUM_ROWS | Protocol::SEND_EOF))
    DBUG_RETURN(TRUE);
  
  mysql_mutex_lock(mysql_bin_log.get_log_lock());
  mysql_bin_log.lock_index();
  index_file=mysql_bin_log.get_index_file();
  
  mysql_bin_log.raw_get_current_log(&cur); // dont take mutex
  mysql_mutex_unlock(mysql_bin_log.get_log_lock()); // lockdep, OK
  
  cur_dir_len= dirname_length(cur.log_file_name);

  reinit_io_cache(index_file, READ_CACHE, (my_off_t) 0, 0, 0);

  /* The file ends with EOF or empty line */
  while ((length=my_b_gets(index_file, fname, sizeof(fname))) > 1)
  {
    int dir_len;
    ulonglong file_length= 0;                   // Length if open fails
    fname[--length] = '\0';                     // remove the newline

    protocol->prepare_for_resend();
    dir_len= dirname_length(fname);
    length-= dir_len;
    protocol->store(fname + dir_len, length, &my_charset_bin);

    if (!(strncmp(fname+dir_len, cur.log_file_name+cur_dir_len, length)))
      file_length= cur.pos;  /* The active log, use the active position */
    else
    {
      /* this is an old log, open it and find the size */
      if ((file= mysql_file_open(key_file_binlog,
                                 fname, O_RDONLY | O_SHARE | O_BINARY,
                                 MYF(0))) >= 0)
      {
        file_length= (ulonglong) mysql_file_seek(file, 0L, MY_SEEK_END, MYF(0));
        mysql_file_close(file, MYF(0));
      }
    }
    protocol->store(file_length);
    if (protocol->write())
      goto err;
  }
  if(index_file->error == -1)
    goto err;
  mysql_bin_log.unlock_index();
  my_eof(thd);
  DBUG_RETURN(FALSE);

err:
  mysql_bin_log.unlock_index();
  DBUG_RETURN(TRUE);
}

/**
   Load data's io cache specific hook to be executed
   before a chunk of data is being read into the cache's buffer
   The fuction instantianates and writes into the binlog
   replication events along LOAD DATA processing.
   
   @param file  pointer to io-cache
   @retval 0 success
   @retval 1 failure
*/
int log_loaded_block(IO_CACHE* file, uchar *Buffer, size_t Count)
{
  DBUG_ENTER("log_loaded_block");
  LOAD_FILE_IO_CACHE *lf_info= static_cast<LOAD_FILE_IO_CACHE*>(file);
  uint block_len;
  /* buffer contains position where we started last read */
  uchar* buffer= (uchar*) my_b_get_buffer_start(file);
  uint max_event_size= lf_info->thd->variables.max_allowed_packet;

  if (lf_info->thd->is_current_stmt_binlog_format_row())
    goto ret;
  if (lf_info->last_pos_in_file != HA_POS_ERROR &&
      lf_info->last_pos_in_file >= my_b_get_pos_in_file(file))
    goto ret;
  
  for (block_len= (uint) (my_b_get_bytes_in_buffer(file)); block_len > 0;
       buffer += MY_MIN(block_len, max_event_size),
       block_len -= MY_MIN(block_len, max_event_size))
  {
    lf_info->last_pos_in_file= my_b_get_pos_in_file(file);
    if (lf_info->wrote_create_file)
    {
      Append_block_log_event a(lf_info->thd, lf_info->thd->db, buffer,
                               MY_MIN(block_len, max_event_size),
                               lf_info->log_delayed);
      if (mysql_bin_log.write(&a))
        DBUG_RETURN(1);
    }
    else
    {
      Begin_load_query_log_event b(lf_info->thd, lf_info->thd->db,
                                   buffer,
                                   MY_MIN(block_len, max_event_size),
                                   lf_info->log_delayed);
      if (mysql_bin_log.write(&b))
        DBUG_RETURN(1);
      lf_info->wrote_create_file= 1;
    }
  }
ret:
  int res= Buffer ? lf_info->real_read_function(file, Buffer, Count) : 0;
  DBUG_RETURN(res);
}


/**
   Initialise the slave replication state from the mysql.gtid_slave_pos table.

   This is called each time an SQL thread starts, but the data is only actually
   loaded on the first call.

   The slave state is the last GTID applied on the slave within each
   replication domain.

   To avoid row lock contention, there are multiple rows for each domain_id.
   The one containing the current slave state is the one with the maximal
   sub_id value, within each domain_id.

    CREATE TABLE mysql.gtid_slave_pos (
      domain_id INT UNSIGNED NOT NULL,
      sub_id BIGINT UNSIGNED NOT NULL,
      server_id INT UNSIGNED NOT NULL,
      seq_no BIGINT UNSIGNED NOT NULL,
      PRIMARY KEY (domain_id, sub_id))
*/

void
rpl_init_gtid_slave_state()
{
  rpl_global_gtid_slave_state= new rpl_slave_state;
}


void
rpl_deinit_gtid_slave_state()
{
  delete rpl_global_gtid_slave_state;
}


void
rpl_init_gtid_waiting()
{
  rpl_global_gtid_waiting.init();
}


void
rpl_deinit_gtid_waiting()
{
  rpl_global_gtid_waiting.destroy();
}


/*
  Format the current GTID state as a string, for returning the value of
  @@global.gtid_slave_pos.

  If the flag use_binlog is true, then the contents of the binary log (if
  enabled) is merged into the current GTID state (@@global.gtid_current_pos).
*/
int
rpl_append_gtid_state(String *dest, bool use_binlog)
{
  int err;
  rpl_gtid *gtid_list= NULL;
  uint32 num_gtids= 0;

  if (use_binlog && opt_bin_log &&
      (err= mysql_bin_log.get_most_recent_gtid_list(&gtid_list, &num_gtids)))
    return err;

  err= rpl_global_gtid_slave_state->tostring(dest, gtid_list, num_gtids);
  my_free(gtid_list);

  return err;
}


/*
  Load the current GTID position into a slave_connection_state, for use when
  connecting to a master server with GTID.

  If the flag use_binlog is true, then the contents of the binary log (if
  enabled) is merged into the current GTID state (master_use_gtid=current_pos).
*/
int
rpl_load_gtid_state(slave_connection_state *state, bool use_binlog)
{
  int err;
  rpl_gtid *gtid_list= NULL;
  uint32 num_gtids= 0;

  if (use_binlog && opt_bin_log &&
      (err= mysql_bin_log.get_most_recent_gtid_list(&gtid_list, &num_gtids)))
    return err;

  err= state->load(rpl_global_gtid_slave_state, gtid_list, num_gtids);
  my_free(gtid_list);

  return err;
}


bool
rpl_gtid_pos_check(THD *thd, char *str, size_t len)
{
  slave_connection_state tmp_slave_state;
  bool gave_conflict_warning= false, gave_missing_warning= false;

  /* Check that we can parse the supplied string. */
  if (tmp_slave_state.load(str, len))
    return true;

  /*
    Check our own binlog for any of our own transactions that are newer
    than the GTID state the user is requesting. Any such transactions would
    result in an out-of-order binlog, which could break anyone replicating
    with us as master.

    So give an error if this is found, requesting the user to do a
    RESET MASTER (to clean up the binlog) if they really want this.
  */
  if (mysql_bin_log.is_open())
  {
    rpl_gtid *binlog_gtid_list= NULL;
    uint32 num_binlog_gtids= 0;
    uint32 i;

    if (mysql_bin_log.get_most_recent_gtid_list(&binlog_gtid_list,
                                                &num_binlog_gtids))
    {
      my_error(ER_OUT_OF_RESOURCES, MYF(MY_WME));
      return true;
    }
    for (i= 0; i < num_binlog_gtids; ++i)
    {
      rpl_gtid *binlog_gtid= &binlog_gtid_list[i];
      rpl_gtid *slave_gtid;
      if (binlog_gtid->server_id != global_system_variables.server_id)
        continue;
      if (!(slave_gtid= tmp_slave_state.find(binlog_gtid->domain_id)))
      {
        if (opt_gtid_strict_mode)
        {
          my_error(ER_MASTER_GTID_POS_MISSING_DOMAIN, MYF(0),
                   binlog_gtid->domain_id, binlog_gtid->domain_id,
                   binlog_gtid->server_id, binlog_gtid->seq_no);
          break;
        }
        else if (!gave_missing_warning)
        {
          push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                              ER_MASTER_GTID_POS_MISSING_DOMAIN,
                              ER_THD(thd, ER_MASTER_GTID_POS_MISSING_DOMAIN),
                              binlog_gtid->domain_id, binlog_gtid->domain_id,
                              binlog_gtid->server_id, binlog_gtid->seq_no);
          gave_missing_warning= true;
        }
      }
      else if (slave_gtid->seq_no < binlog_gtid->seq_no)
      {
        if (opt_gtid_strict_mode)
        {
          my_error(ER_MASTER_GTID_POS_CONFLICTS_WITH_BINLOG, MYF(0),
                   slave_gtid->domain_id, slave_gtid->server_id,
                   slave_gtid->seq_no, binlog_gtid->domain_id,
                   binlog_gtid->server_id, binlog_gtid->seq_no);
          break;
        }
        else if (!gave_conflict_warning)
        {
          push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                              ER_MASTER_GTID_POS_CONFLICTS_WITH_BINLOG,
                              ER_THD(thd, ER_MASTER_GTID_POS_CONFLICTS_WITH_BINLOG),
                              slave_gtid->domain_id, slave_gtid->server_id,
                              slave_gtid->seq_no, binlog_gtid->domain_id,
                              binlog_gtid->server_id, binlog_gtid->seq_no);
          gave_conflict_warning= true;
        }
      }
    }
    my_free(binlog_gtid_list);
    if (i != num_binlog_gtids)
      return true;
  }

  return false;
}


bool
rpl_gtid_pos_update(THD *thd, char *str, size_t len)
{
  if (rpl_global_gtid_slave_state->load(thd, str, len, true, true))
  {
    my_error(ER_FAILED_GTID_STATE_INIT, MYF(0));
    return true;
  }
  else
    return false;
}


#endif /* HAVE_REPLICATION */
