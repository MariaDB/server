/* Copyright 2008-2020 Codership Oy <http://www.codership.com>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

#include "mariadb.h"
#include "wsrep_sst.h"
#include <inttypes.h>
#include <ctype.h>
#include <mysqld.h>
#include <m_ctype.h>
#include <strfunc.h>
#include <sql_class.h>
#include <set_var.h>
#include <sql_acl.h>
#include <sql_reload.h>
#include <sql_parse.h>
#include "wsrep_priv.h"
#include "wsrep_utils.h"
#include "wsrep_xid.h"
#include "wsrep_thd.h"
#include "wsrep_mysqld.h"

#include <cstdio>
#include <cstdlib>

#include <my_service_manager.h>

static char wsrep_defaults_file[FN_REFLEN * 2 + 10 + 30 +
                                sizeof(WSREP_SST_OPT_CONF) +
                                sizeof(WSREP_SST_OPT_CONF_SUFFIX) +
                                sizeof(WSREP_SST_OPT_CONF_EXTRA)]= {0};

const char* wsrep_sst_method         = WSREP_SST_DEFAULT;
const char* wsrep_sst_receive_address= WSREP_SST_ADDRESS_AUTO;
const char* wsrep_sst_donor          = "";
const char* wsrep_sst_auth           = NULL;

// container for real auth string
static const char* sst_auth_real     = NULL;
my_bool wsrep_sst_donor_rejects_queries= FALSE;

#define WSREP_EXTEND_TIMEOUT_INTERVAL 60
#define WSREP_TIMEDWAIT_SECONDS 30

bool sst_joiner_completed            = false;
bool sst_donor_completed             = false;

struct sst_thread_arg
{
  const char*     cmd;
  char**          env;
  char*           ret_str;
  int             err;
  mysql_mutex_t   lock;
  mysql_cond_t    cond;

  sst_thread_arg (const char* c, char** e)
    : cmd(c), env(e), ret_str(0), err(-1)
  {
    mysql_mutex_init(key_LOCK_wsrep_sst_thread, &lock, MY_MUTEX_INIT_FAST);
    mysql_cond_init(key_COND_wsrep_sst_thread, &cond, NULL);
  }

  ~sst_thread_arg()
  {
    mysql_cond_destroy  (&cond);
    mysql_mutex_unlock  (&lock);
    mysql_mutex_destroy (&lock);
  }
};

static void wsrep_donor_monitor_end(void)
{
  mysql_mutex_lock(&LOCK_wsrep_donor_monitor);
  sst_donor_completed= true;
  mysql_cond_signal(&COND_wsrep_donor_monitor);
  mysql_mutex_unlock(&LOCK_wsrep_donor_monitor);
}

static void wsrep_joiner_monitor_end(void)
{
  mysql_mutex_lock(&LOCK_wsrep_joiner_monitor);
  sst_joiner_completed= true;
  mysql_cond_signal(&COND_wsrep_joiner_monitor);
  mysql_mutex_unlock(&LOCK_wsrep_joiner_monitor);
}

static void* wsrep_sst_donor_monitor_thread(void *arg __attribute__((unused)))
{
  int ret= 0;
  unsigned long time_waited= 0;

  mysql_mutex_lock(&LOCK_wsrep_donor_monitor);

  WSREP_INFO("Donor monitor thread started to monitor");

  wsp::thd thd(FALSE); // we turn off wsrep_on for this THD so that it can
                       // operate with wsrep_ready == OFF

  while (!sst_donor_completed)
  {
    timespec ts;
    set_timespec(ts, WSREP_TIMEDWAIT_SECONDS);
    time_t start_time= time(NULL);
    ret= mysql_cond_timedwait(&COND_wsrep_donor_monitor, &LOCK_wsrep_donor_monitor, &ts);
    time_t end_time= time(NULL);
    time_waited+= difftime(end_time, start_time);

    if (ret == ETIMEDOUT && !sst_donor_completed)
    {
      WSREP_DEBUG("Donor waited %lu sec, extending systemd startup timeout as SST"
                 "is not completed",
                 time_waited);
      service_manager_extend_timeout(WSREP_EXTEND_TIMEOUT_INTERVAL,
        "WSREP state transfer ongoing...");
    }
  }

  WSREP_INFO("Donor monitor thread ended with total time %lu sec", time_waited);
  mysql_mutex_unlock(&LOCK_wsrep_donor_monitor);

  return NULL;
}

static void* wsrep_sst_joiner_monitor_thread(void *arg __attribute__((unused)))
{
  int ret= 0;
  unsigned long time_waited= 0;

  mysql_mutex_lock(&LOCK_wsrep_joiner_monitor);

  WSREP_INFO("Joiner monitor thread started to monitor");

  wsp::thd thd(FALSE); // we turn off wsrep_on for this THD so that it can
                       // operate with wsrep_ready == OFF

  while (!sst_joiner_completed)
  {
    timespec ts;
    set_timespec(ts, WSREP_TIMEDWAIT_SECONDS);
    time_t start_time= time(NULL);
    ret= mysql_cond_timedwait(&COND_wsrep_joiner_monitor, &LOCK_wsrep_joiner_monitor, &ts);
    time_t end_time= time(NULL);
    time_waited+= difftime(end_time, start_time);

    if (ret == ETIMEDOUT && !sst_joiner_completed)
    {
      WSREP_DEBUG("Joiner waited %lu sec, extending systemd startup timeout as SST"
                 "is not completed",
                 time_waited);
      service_manager_extend_timeout(WSREP_EXTEND_TIMEOUT_INTERVAL,
        "WSREP state transfer ongoing...");
    }
  }

  WSREP_INFO("Joiner monitor thread ended with total time %lu sec", time_waited);
  mysql_mutex_unlock(&LOCK_wsrep_joiner_monitor);

  return NULL;
}

bool wsrep_sst_method_check (sys_var *self, THD* thd, set_var* var)
{
  if ((! var->save_result.string_value.str) ||
      (var->save_result.string_value.length == 0 ))
  {
    my_error(ER_WRONG_VALUE_FOR_VAR, MYF(0), var->var->name.str,
             var->save_result.string_value.str ?
             var->save_result.string_value.str : "NULL");
    return 1;
  }

  return 0;
}

static const char* data_home_dir;

void wsrep_set_data_home_dir(const char *data_dir)
{
  data_home_dir= (data_dir && *data_dir) ? data_dir : NULL;
}

static void make_wsrep_defaults_file()
{
  if (!wsrep_defaults_file[0])
  {
    char *ptr= wsrep_defaults_file;
    char *end= ptr + sizeof(wsrep_defaults_file);
    if (my_defaults_file)
      ptr= strxnmov(ptr, end - ptr,
                    WSREP_SST_OPT_CONF, " '", my_defaults_file, "' ", NULL);

    if (my_defaults_extra_file)
      ptr= strxnmov(ptr, end - ptr,
                    WSREP_SST_OPT_CONF_EXTRA, " '", my_defaults_extra_file, "' ", NULL);

    if (my_defaults_group_suffix)
      ptr= strxnmov(ptr, end - ptr,
                    WSREP_SST_OPT_CONF_SUFFIX, " '", my_defaults_group_suffix, "' ", NULL);
  }
}


bool  wsrep_sst_receive_address_check (sys_var *self, THD* thd, set_var* var)
{
  if ((! var->save_result.string_value.str) ||
      (var->save_result.string_value.length > (FN_REFLEN - 1))) // safety
  {
    goto err;
  }

  return 0;

err:
  my_error(ER_WRONG_VALUE_FOR_VAR, MYF(0), var->var->name.str,
           var->save_result.string_value.str ?
           var->save_result.string_value.str : "NULL");
  return 1;
}

bool wsrep_sst_receive_address_update (sys_var *self, THD* thd,
                                       enum_var_type type)
{
    return 0;
}

bool wsrep_sst_auth_check (sys_var *self, THD* thd, set_var* var)
{
    return 0;
}

static bool sst_auth_real_set (const char* value)
{
  const char* v= NULL;

  if (value)
  {
    v= my_strdup(PSI_INSTRUMENT_ME, value, MYF(0));
  }
  else                                          // its NULL
  {
    wsrep_sst_auth_free();
    return 0;
  }

  if (v)
  {
    // set sst_auth_real
    if (sst_auth_real) { my_free((void *) sst_auth_real); }
    sst_auth_real= v;

    // mask wsrep_sst_auth
    if (strlen(sst_auth_real))
    {
      if (wsrep_sst_auth) { my_free((void*) wsrep_sst_auth); }
      wsrep_sst_auth= my_strdup(PSI_INSTRUMENT_ME, WSREP_SST_AUTH_MASK, MYF(0));
    }
    else
    {
      if (wsrep_sst_auth) { my_free((void*) wsrep_sst_auth); }
      wsrep_sst_auth= NULL;
    }

    return 0;
  }
  return 1;
}

void wsrep_sst_auth_free()
{
  if (wsrep_sst_auth) { my_free((void *) wsrep_sst_auth); }
  if (sst_auth_real) { my_free((void *) sst_auth_real); }
  wsrep_sst_auth= NULL;
  sst_auth_real= NULL;
}

bool wsrep_sst_auth_update (sys_var *self, THD* thd, enum_var_type type)
{
  return sst_auth_real_set (wsrep_sst_auth);
}

void wsrep_sst_auth_init ()
{
  sst_auth_real_set(wsrep_sst_auth);
}

bool  wsrep_sst_donor_check (sys_var *self, THD* thd, set_var* var)
{
  return 0;
}

bool wsrep_sst_donor_update (sys_var *self, THD* thd, enum_var_type type)
{
  return 0;
}


bool wsrep_before_SE()
{
  return (wsrep_provider != NULL
          && strcmp (wsrep_provider,   WSREP_NONE)
          && strcmp (wsrep_sst_method, WSREP_SST_SKIP)
          && strcmp (wsrep_sst_method, WSREP_SST_MYSQLDUMP));
}

// Signal end of SST
static bool wsrep_sst_complete (THD*                thd,
                                int const           rcode,
                                wsrep::gtid const   sst_gtid)
{
  Wsrep_client_service client_service(thd, thd->wsrep_cs());
  Wsrep_server_state& server_state= Wsrep_server_state::instance();
  enum wsrep::server_state::state state= server_state.state();
  bool failed= false;
  char start_pos_buf[FN_REFLEN];
  ssize_t len= wsrep::print_to_c_str(sst_gtid, start_pos_buf, FN_REFLEN-1);
  start_pos_buf[len]='\0';

  // Do not call sst_received if we are not in joiner or
  // initialized state on server. This is because it
  // assumes we are on those states. Give error if we are
  // in incorrect state.
  if ((state == Wsrep_server_state::s_joiner ||
       state == Wsrep_server_state::s_initialized))
  {
    Wsrep_server_state::instance().sst_received(client_service,
       rcode);
    WSREP_INFO("SST succeeded for position %s", start_pos_buf);
  }
  else
  {
    WSREP_ERROR("SST failed for position %s initialized %d server_state %s",
                start_pos_buf,
                server_state.is_initialized(),
                wsrep::to_c_string(state));
    failed= true;
  }

  wsrep_joiner_monitor_end();
  return failed;
}

  /*
  If wsrep provider is loaded, inform that the new state snapshot
  has been received. Also update the local checkpoint.

  @param thd       [IN]
  @param uuid      [IN]               Initial state UUID
  @param seqno     [IN]               Initial state sequence number
  @param state     [IN]               Always NULL, also ignored by wsrep provider (?)
  @param state_len [IN]               Always 0, also ignored by wsrep provider (?)
  @return true when successful, false if error
*/
bool wsrep_sst_received (THD*                thd,
                         const wsrep_uuid_t& uuid,
                         wsrep_seqno_t const seqno,
                         const void* const   state,
                         size_t const        state_len)
{
  bool error= false;
  /*
    To keep track of whether the local uuid:seqno should be updated. Also, note
    that local state (uuid:seqno) is updated/checkpointed only after we get an
    OK from wsrep provider. By doing so, the values remain consistent across
    the server & wsrep provider.
  */
    /*
      TODO: Handle backwards compatibility. WSREP API v25 does not have
      wsrep schema.
    */
    /*
      Logical SST methods (mysqldump etc) don't update InnoDB sys header.
      Reset the SE checkpoint before recovering view in order to avoid
      sanity check failure.
     */
    wsrep::gtid const sst_gtid(wsrep::id(uuid.data, sizeof(uuid.data)),
                               wsrep::seqno(seqno));

    if (!wsrep_before_SE()) {
      wsrep_set_SE_checkpoint(wsrep::gtid::undefined(), wsrep_gtid_server.undefined());
      wsrep_set_SE_checkpoint(sst_gtid, wsrep_gtid_server.gtid());
    }
    wsrep_verify_SE_checkpoint(uuid, seqno);

    /*
      Both wsrep_init_SR() and wsrep_recover_view() may use
      wsrep thread pool. Restore original thd context before returning.
    */
    if (thd) {
      wsrep_store_threadvars(thd);
    }
    else {
      set_current_thd(nullptr);
    }

    /* During sst WSREP(thd) is not yet set for joiner. */
    if (WSREP_ON)
    {
      int const rcode(seqno < 0 ? seqno : 0);
      error= wsrep_sst_complete(thd,rcode, sst_gtid);
    }

    return error;
}

static int sst_scan_uuid_seqno (const char* str,
                                wsrep_uuid_t* uuid, wsrep_seqno_t* seqno)
{
  int offt= wsrep_uuid_scan (str, strlen(str), uuid);
  errno= 0;                                     /* Reset the errno */
  if (offt > 0 && strlen(str) > (unsigned int)offt && ':' == str[offt])
  {
    *seqno= strtoll (str + offt + 1, NULL, 10);
    if (*seqno != LLONG_MAX || errno != ERANGE)
    {
      return 0;
    }
  }

  WSREP_ERROR("Failed to parse uuid:seqno pair: '%s'", str);
  return -EINVAL;
}

// get rid of trailing \n
static char* my_fgets (char* buf, size_t buf_len, FILE* stream)
{
   char* ret= fgets (buf, buf_len, stream);

   if (ret)
   {
       size_t len= strlen(ret);
       if (len > 0 && ret[len - 1] == '\n') ret[len - 1]= '\0';
   }

   return ret;
}

/*
  Generate "name 'value'" string.
*/
static char* generate_name_value(const char* name, const char* value)
{
  size_t name_len= strlen(name);
  size_t value_len= strlen(value);
  char* buf=
    (char*) my_malloc(PSI_INSTRUMENT_ME, (name_len + value_len + 5), MYF(0));
  if (buf)
  {
    char* ref= buf;
    *ref++ = ' ';
    memcpy(ref, name, name_len * sizeof(char));
    ref += name_len;
    *ref++ = ' ';
    *ref++ = '\'';
    memcpy(ref, value, value_len * sizeof(char));
    ref += value_len;
    *ref++ = '\'';
    *ref = 0;
  }
  return buf;
}
/*
  Generate binlog option string for sst_donate_other(), sst_prepare_other().

  Returns zero on success, negative error code otherwise.

  String containing binlog name is stored in param ret if binlog is enabled
  and GTID mode is on, otherwise empty string. Returned string should be
  freed with my_free().
 */
static int generate_binlog_opt_val(char** ret)
{
  DBUG_ASSERT(ret);
  *ret= NULL;
  if (opt_bin_log)
  {
    assert(opt_bin_logname);
    *ret= strcmp(opt_bin_logname, "0") ?
      generate_name_value(WSREP_SST_OPT_BINLOG,
                          opt_bin_logname) :
      my_strdup(PSI_INSTRUMENT_ME, "", MYF(0));
  }
  else
  {
    *ret= my_strdup(PSI_INSTRUMENT_ME, "", MYF(0));
  }
  if (!*ret) return -ENOMEM;
  return 0;
}

static int generate_binlog_index_opt_val(char** ret)
{
  DBUG_ASSERT(ret);
  *ret= NULL;
  if (opt_binlog_index_name)
  {
    *ret= strcmp(opt_binlog_index_name, "0") ?
      generate_name_value(WSREP_SST_OPT_BINLOG_INDEX,
                          opt_binlog_index_name) :
      my_strdup(PSI_INSTRUMENT_ME, "", MYF(0));
  }
  else
  {
    *ret= my_strdup(PSI_INSTRUMENT_ME, "", MYF(0));
  }
  if (!*ret) return -ENOMEM;
  return 0;
}

// report progress event
static void sst_report_progress(int const       from,
                                long long const total_prev,
                                long long const total,
                                long long const complete)
{
  static char buf[128] = { '\0', };
  static size_t const buf_len= sizeof(buf) - 1;
  snprintf(buf, buf_len,
           "{ \"from\": %d, \"to\": %d, \"total\": %lld, \"done\": %lld, "
           "\"indefinite\": -1 }",
           from, WSREP_MEMBER_JOINED, total_prev + total, total_prev +complete);
  WSREP_DEBUG("REPORTING SST PROGRESS: '%s'", buf);
}

// process "complete" event from SST script feedback
static void sst_handle_complete(const char* const input,
                                long long const   total_prev,
                                long long*        total,
                                long long*        complete,
                                int const         from)
{
  long long x;
  int n= sscanf(input, " %lld", &x);
  if (n > 0 && x > *complete)
  {
    *complete= x;
    if (*complete > *total) *total= *complete;
    sst_report_progress(from, total_prev, *total, *complete);
  }
}

// process "total" event from SST script feedback
static void sst_handle_total(const char* const input,
                             long long*        total_prev,
                             long long*        total,
                             long long*        complete,
                             int const         from)
{
  long long x;
  int n= sscanf(input, " %lld", &x);
  if (n > 0)
  {
    // new stage starts, update total_prev
    *total_prev+= *total;
    *total= x;
    *complete= 0;
    sst_report_progress(from, *total_prev, *total, *complete);
  }
}

static void* sst_joiner_thread (void* a)
{
  sst_thread_arg* arg= (sst_thread_arg*) a;
  int err= 1;

  {
    THD* thd;
    static const char magic[]= "ready";
    static const size_t magic_len= sizeof(magic) - 1;
    const size_t out_len= 512;
    char out[out_len];

    WSREP_INFO("Running: '%s'", arg->cmd);

    wsp::process proc (arg->cmd, "r", arg->env);

    if (proc.pipe() && !proc.error())
    {
      const char* tmp= my_fgets (out, out_len, proc.pipe());

      if (!tmp || strlen(tmp) < (magic_len + 2) ||
          strncasecmp (tmp, magic, magic_len))
      {
        WSREP_ERROR("Failed to read '%s <addr>' from: %s\n\tRead: '%s'",
                    magic, arg->cmd, tmp);
        proc.wait();
        if (proc.error()) err= proc.error();
      }
      else
      {
        err= 0;
      }
    }
    else
    {
      err= proc.error();
      WSREP_ERROR("Failed to execute: %s : %d (%s)",
                  arg->cmd, err, strerror(err));
    }

    /*
      signal sst_prepare thread with ret code,
      it will go on sending SST request
    */
    mysql_mutex_lock   (&arg->lock);
    if (!err)
    {
      arg->ret_str= strdup (out + magic_len + 1);
      if (!arg->ret_str) err= ENOMEM;
    }
    arg->err= -err;
    mysql_cond_signal  (&arg->cond);
    mysql_mutex_unlock (&arg->lock); //! @note arg is unusable after that.

    if (err) return NULL; /* lp:808417 - return immediately, don't signal
                           * initializer thread to ensure single thread of
                           * shutdown. */

    wsrep_uuid_t  ret_uuid = WSREP_UUID_UNDEFINED;
    wsrep_seqno_t ret_seqno= WSREP_SEQNO_UNDEFINED;

    // current stage progress
    long long total= 0;
    long long complete= 0;
    // previous stages cumulative progress
    long long total_prev= 0;

    // in case of successful receiver start, wait for SST completion/end
    const char* tmp= NULL;
    err= EINVAL;

  wait_signal:
    tmp= my_fgets (out, out_len, proc.pipe());

    if (tmp)
    {
      static const char magic_total[]= "total";
      static const size_t total_len=strlen(magic_total);
      static const char magic_complete[]= "complete";
      static const size_t complete_len=strlen(magic_complete);
      static const int from= WSREP_MEMBER_JOINER;

      if (!strncasecmp (tmp, magic_complete, complete_len))
      {
        sst_handle_complete(tmp + complete_len, total_prev, &total, &complete,
                            from);
        goto wait_signal;
      }
      else if (!strncasecmp (tmp, magic_total, total_len))
      {
        sst_handle_total(tmp + total_len, &total_prev, &total, &complete, from);
        goto wait_signal;
      }
    }
    else
    {
      WSREP_ERROR("Failed to read uuid:seqno and wsrep_gtid_domain_id from "
                  "joiner script.");
      proc.wait();
      if (proc.error()) err= proc.error();
    }

    // this should be the final script output with GTID
    if (tmp)
    {
      proc.wait();
      // Read state ID (UUID:SEQNO) followed by wsrep_gtid_domain_id (if any).
      const char *pos= strchr(out, ' ');

      if (!pos) {

        if (wsrep_gtid_mode)
        {
          // There is no wsrep_gtid_domain_id (some older version SST script?).
          WSREP_WARN("Did not find domain ID from SST script output '%s'. "
                     "Domain ID must be set manually to keep binlog consistent",
                     out);
        }
        err= sst_scan_uuid_seqno (out, &ret_uuid, &ret_seqno);

      } else {
        // Scan state ID first followed by wsrep_gtid_domain_id.
        unsigned long int domain_id;

        // Null-terminate the state-id.
        out[pos - out]= 0;
        err= sst_scan_uuid_seqno (out, &ret_uuid, &ret_seqno);

        if (err)
        {
          goto err;
        }
        else if (wsrep_gtid_mode)
        {
          errno= 0;                             /* Reset the errno */
          domain_id= strtoul(pos + 1, NULL, 10);
          err= errno;

          /* Check if we received a valid gtid_domain_id. */
          if (err == EINVAL || err == ERANGE)
          {
            WSREP_ERROR("Failed to get donor wsrep_gtid_domain_id.");
            err= EINVAL;
            goto err;
          } else {
            wsrep_gtid_server.domain_id= (uint32) domain_id;
            wsrep_gtid_domain_id= (uint32)domain_id;
          }
        }
      }
    }

err:

    wsrep::gtid ret_gtid;

    if (err)
    {
      ret_gtid= wsrep::gtid::undefined();
    }
    else
    {
      ret_gtid= wsrep::gtid(wsrep::id(ret_uuid.data, sizeof(ret_uuid.data)),
                            wsrep::seqno(ret_seqno));
    }

    /*
      Tell initializer thread that SST is complete
      For that initialize a THD
    */
    if (my_thread_init())
    {
      WSREP_ERROR("my_thread_init() failed, can't signal end of SST. "
                  "Aborting.");
      unireg_abort(1);
    }

    thd= new THD(next_thread_id());

    if (!thd)
    {
      WSREP_ERROR("Failed to allocate THD to restore view from local state, "
                  "can't signal end of SST. Aborting.");
      unireg_abort(1);
    }

    thd->thread_stack= (char*) &thd;
    thd->security_ctx->skip_grants();
    thd->system_thread= SYSTEM_THREAD_GENERIC;
    thd->real_id= pthread_self();

    wsrep_assign_from_threadvars(thd);
    wsrep_store_threadvars(thd);

    /* */
    thd->variables.wsrep_on    = 0;
    /* No binlogging */
    thd->variables.sql_log_bin = 0;
    thd->variables.option_bits &= ~OPTION_BIN_LOG;
    /* No general log */
    thd->variables.option_bits |= OPTION_LOG_OFF;
    /* Read committed isolation to avoid gap locking */
    thd->variables.tx_isolation= ISO_READ_COMMITTED;

    wsrep_sst_complete (thd, -err, ret_gtid);

    delete thd;
    my_thread_end();
  }

  return NULL;
}

#define WSREP_SST_AUTH_ENV        "WSREP_SST_OPT_AUTH"
#define WSREP_SST_REMOTE_AUTH_ENV "WSREP_SST_OPT_REMOTE_AUTH"
#define DATA_HOME_DIR_ENV         "INNODB_DATA_HOME_DIR"

static int sst_append_env_var(wsp::env&   env,
                              const char* const var,
                              const char* const val)
{
  int const env_str_size= strlen(var) + 1 /* = */
                          + (val ? strlen(val) : 0) + 1 /* \0 */;

  wsp::string env_str(env_str_size); // for automatic cleanup on return
  if (!env_str()) return -ENOMEM;

  int ret= snprintf(env_str(), env_str_size, "%s=%s", var, val ? val : "");

  if (ret < 0 || ret >= env_str_size)
  {
    WSREP_ERROR("sst_append_env_var(): snprintf(%s=%s) failed: %d",
                var, val, ret);
    return (ret < 0 ? ret : -EMSGSIZE);
  }

  env.append(env_str());
  return -env.error();
}

#ifdef _WIN32
/*
  Space, single quote, ampersand, backquote, I/O redirection
  characters, caret, all brackets, plus, exclamation and comma
  characters require text to be enclosed in double quotes:
*/
#define IS_SPECIAL(c) \
  (isspace(c) || c == '\'' || c == '&' || c == '`' || c == '|' || \
                 c ==  '>' || c == '<' || c == ';' || c == '^' || \
                 c ==  '[' || c == ']' || c == '{' || c == '}' || \
                 c ==  '(' || c == ')' || c == '+' || c == '!' || \
                 c ==  ',')
/*
  Inside values, equals character are interpreted as special
  character and requires quotation:
*/
#define IS_SPECIAL_V(c) (IS_SPECIAL(c) || c == '=')
/*
  Double quotation mark and percent characters require escaping:
*/
#define IS_REQ_ESCAPING(c) (c == '""' || c == '%')
#else
/*
  Space, single quote, ampersand, backquote, and I/O redirection
  characters require text to be enclosed in double quotes. The
  semicolon is used to separate shell commands, so it must be
  enclosed in double quotes as well:
*/
#define IS_SPECIAL(c) \
  (isspace(c) || c == '\'' || c == '&' || c == '`' || c == '|' || \
                 c ==  '>' || c == '<' || c == ';')
/*
  Inside values, characters are interpreted as in parameter names:
*/
#define IS_SPECIAL_V(c) IS_SPECIAL(c)
/*
  Double quotation mark and backslash characters require
  backslash prefixing, the dollar symbol is used to substitute
  a variable value, therefore it also requires escaping:
*/
#define IS_REQ_ESCAPING(c) (c == '"' || c == '\\' || c == '$')
#endif

static size_t estimate_cmd_len (bool* extra_args)
{
  /*
    The length of the area reserved for the control parameters
    of the SST script (excluding the copying of the original
    mysqld arguments):
  */
  size_t cmd_len= 4096;
  bool extra= false;
  /*
    If mysqld was started with arguments, add them all:
  */
  if (orig_argc > 1)
  {
    for (int i = 1; i < orig_argc; i++)
    {
      const char* arg= orig_argv[i];
      size_t n= strlen(arg);
      if (n == 0) continue;
      cmd_len += n;
      bool quotation= false;
      char c;
      while ((c = *arg++) != 0)
      {
        if (IS_SPECIAL(c))
        {
          quotation= true;
        }
        else if (IS_REQ_ESCAPING(c))
        {
          cmd_len++;
#ifdef _WIN32
          quotation= true;
#endif
        }
        /*
          If the equals symbol is encountered, then we need to separately
          process the right side:
        */
        else if (c == '=')
        {
          /* Perhaps we need to quote the left part of the argument: */
          if (quotation)
          {
            cmd_len += 2;
            /*
              Reset the quotation flag, since now the status for
              the right side of the expression will be saved here:
            */
            quotation= false;
          }
          while ((c = *arg++) != 0)
          {
            if (IS_SPECIAL_V(c))
            {
              quotation= true;
            }
            else if (IS_REQ_ESCAPING(c))
            {
              cmd_len++;
#ifdef _WIN32
              quotation= true;
#endif
            }
          }
          break;
        }
      }
      /* Perhaps we need to quote the entire argument or its right part: */
      if (quotation)
      {
        cmd_len += 2;
      }
    }
    extra = true;
    cmd_len += strlen(WSREP_SST_OPT_MYSQLD);
    /*
      Add the separating spaces between arguments,
      and one additional space before "--mysqld-args":
    */
    cmd_len += orig_argc;
  }
  *extra_args= extra;
  return cmd_len;
}

static void copy_orig_argv (char* cmd_str)
{
  /*
     If mysqld was started with arguments, copy them all:
  */
  if (orig_argc > 1)
  {
    size_t n = strlen(WSREP_SST_OPT_MYSQLD);
    *cmd_str++ = ' ';
    memcpy(cmd_str, WSREP_SST_OPT_MYSQLD, n * sizeof(char));
    cmd_str += n;
    for (int i = 1; i < orig_argc; i++)
    {
      char* arg= orig_argv[i];
      n = strlen(arg);
      if (n == 0) continue;
      *cmd_str++ = ' ';
      bool quotation= false;
      bool plain= true;
      char *arg_scan= arg;
      char c;
      while ((c = *arg_scan++) != 0)
      {
        if (IS_SPECIAL(c))
        {
          quotation= true;
        }
        else if (IS_REQ_ESCAPING(c))
        {
          plain= false;
#ifdef _WIN32
          quotation= true;
#endif
        }
        /*
          If the equals symbol is encountered, then we need to separately
          process the right side:
        */
        else if (c == '=')
        {
          /* Calculate length of the Left part of the argument: */
          size_t m = (size_t) (arg_scan - arg) - 1;
          if (m)
          {
            /* Perhaps we need to quote the left part of the argument: */
            if (quotation)
            {
              *cmd_str++ = '"';
            }
            /*
              If there were special characters inside, then we can use
              the fast memcpy function:
            */
            if (plain)
            {
              memcpy(cmd_str, arg, m * sizeof(char));
              cmd_str += m;
              /* Left part of the argument has already been processed: */
              n -= m;
              arg += m;
            }
            /* Otherwise we need to prefix individual characters: */
            else
            {
              n -= m;
              while (m)
              {
                c = *arg++;
                if (IS_REQ_ESCAPING(c))
                {
#ifdef _WIN32
                  *cmd_str++ = c;
#else
                  *cmd_str++ = '\\';
#endif
                }
                *cmd_str++ = c;
                m--;
              }
              /*
                Reset the plain string flag, since now the status for
                the right side of the expression will be saved here:
              */
              plain= true;
            }
            /* Perhaps we need to quote the left part of the argument: */
            if (quotation)
            {
              *cmd_str++ = '"';
              /*
                Reset the quotation flag, since now the status for
                the right side of the expression will be saved here:
              */
              quotation= false;
            }
          }
          /* Copy equals symbol: */
          *cmd_str++ = '=';
          arg++;
          n--;
          /* Let's deal with the left side of the expression: */
          while ((c = *arg_scan++) != 0)
          {
            if (IS_SPECIAL_V(c))
            {
              quotation= true;
            }
            else if (IS_REQ_ESCAPING(c))
            {
              plain= false;
#ifdef _WIN32
              quotation= true;
#endif
            }
          }
          break;
        }
      }
      if (n)
      {
        /* Perhaps we need to quote the entire argument or its right part: */
        if (quotation)
        {
          *cmd_str++ = '"';
        }
        /*
          If there were no special characters inside, then we can use
          the fast memcpy function:
        */
        if (plain)
        {
          memcpy(cmd_str, arg, n * sizeof(char));
          cmd_str += n;
        }
        /* Otherwise we need to prefix individual characters: */
        else
        {
          while ((c = *arg++) != 0)
          {
            if (IS_REQ_ESCAPING(c))
            {
#ifdef _WIN32
              *cmd_str++ = c;
#else
              *cmd_str++ = '\\';
#endif
            }
            *cmd_str++ = c;
          }
        }
        /* Perhaps we need to quote the entire argument or its right part: */
        if (quotation)
        {
          *cmd_str++ = '"';
        }
      }
    }
    /*
      Add a terminating null character (not counted in the length,
      since we've overwritten the original null character which
      was previously added by snprintf:
    */
    *cmd_str = 0;
  }
}

static ssize_t sst_prepare_other (const char*  method,
                                  const char*  sst_auth,
                                  const char*  addr_in,
                                  const char** addr_out)
{
  bool extra_args;
  size_t const cmd_len= estimate_cmd_len(&extra_args);
  wsp::string cmd_str(cmd_len);

  if (!cmd_str())
  {
    WSREP_ERROR("sst_prepare_other(): could not allocate cmd buffer of %zd bytes",
                cmd_len);
    return -ENOMEM;
  }

  char* binlog_opt_val= NULL;
  char* binlog_index_opt_val= NULL;

  int ret;
  if ((ret= generate_binlog_opt_val(&binlog_opt_val)))
  {
    WSREP_ERROR("sst_prepare_other(): generate_binlog_opt_val() failed: %d",
                ret);
    return ret;
  }

  if ((ret= generate_binlog_index_opt_val(&binlog_index_opt_val)))
  {
    WSREP_ERROR("sst_prepare_other(): generate_binlog_index_opt_val() failed %d",
                ret);
    if (binlog_opt_val) my_free(binlog_opt_val);
    return ret;
  }

  make_wsrep_defaults_file();

  ret= snprintf (cmd_str(), cmd_len,
                 "wsrep_sst_%s "
                 WSREP_SST_OPT_ROLE " 'joiner' "
                 WSREP_SST_OPT_ADDR " '%s' "
                 WSREP_SST_OPT_DATA " '%s' "
                 "%s"
                 WSREP_SST_OPT_PARENT " '%d'"
                 "%s"
                 "%s",
                 method, addr_in, mysql_real_data_home,
                 wsrep_defaults_file,
                 (int)getpid(),
                 binlog_opt_val, binlog_index_opt_val);

  my_free(binlog_opt_val);
  my_free(binlog_index_opt_val);

  if (ret < 0 || size_t(ret) >= cmd_len)
  {
    WSREP_ERROR("sst_prepare_other(): snprintf() failed: %d", ret);
    return (ret < 0 ? ret : -EMSGSIZE);
  }

  if (extra_args)
    copy_orig_argv(cmd_str() + ret);

  wsp::env env(NULL);
  if (env.error())
  {
    WSREP_ERROR("sst_prepare_other(): env. var ctor failed: %d", -env.error());
    return -env.error();
  }

  if ((ret= sst_append_env_var(env, WSREP_SST_AUTH_ENV, sst_auth)))
  {
    WSREP_ERROR("sst_prepare_other(): appending auth failed: %d", ret);
    return ret;
  }

  if (data_home_dir)
  {
    if ((ret= sst_append_env_var(env, DATA_HOME_DIR_ENV, data_home_dir)))
    {
      WSREP_ERROR("sst_prepare_other(): appending data "
                  "directory failed: %d", ret);
      return ret;
    }
  }

  pthread_t tmp, monitor;
  sst_thread_arg arg(cmd_str(), env());

  mysql_mutex_lock (&arg.lock);

  ret = mysql_thread_create (key_wsrep_sst_joiner_monitor, &monitor, NULL, wsrep_sst_joiner_monitor_thread, NULL);

  if (ret)
  {
    WSREP_ERROR("sst_prepare_other(): mysql_thread_create() failed: %d (%s)",
                ret, strerror(ret));
    return -ret;
  }

  sst_joiner_completed= false;

  ret= mysql_thread_create (key_wsrep_sst_joiner, &tmp, NULL, sst_joiner_thread, &arg);

  if (ret)
  {
    WSREP_ERROR("sst_prepare_other(): mysql_thread_create() failed: %d (%s)",
                ret, strerror(ret));

    pthread_detach(monitor);
    return -ret;
  }

  mysql_cond_wait (&arg.cond, &arg.lock);

  *addr_out= arg.ret_str;

  if (!arg.err)
    ret= strlen(*addr_out);
  else
  {
    assert (arg.err < 0);
    ret= arg.err;
  }

  pthread_detach (tmp);
  pthread_detach (monitor);

  return ret;
}

extern uint  mysqld_port;

/*! Just tells donor where to send mysqldump */
static ssize_t sst_prepare_mysqldump (const char*  addr_in,
                                      const char** addr_out)
{
  ssize_t ret= strlen (addr_in);

  if (!strrchr(addr_in, ':'))
  {
    ssize_t s= ret + 7;
    char* tmp= (char*) malloc (s);

    if (tmp)
    {
      ret= snprintf (tmp, s, "%s:%u", addr_in, mysqld_port);

      if (ret > 0 && ret < s)
      {
        *addr_out= tmp;
        return ret;
      }
      if (ret > 0) /* buffer too short */ ret= -EMSGSIZE;
      free (tmp);
    }
    else {
      ret= -ENOMEM;
    }

    WSREP_ERROR ("Could not prepare state transfer request: "
                 "adding default port failed: %zd.", ret);
  }
  else {
    *addr_out= addr_in;
  }

  pthread_t monitor;
  ret = mysql_thread_create (key_wsrep_sst_joiner_monitor, &monitor, NULL, wsrep_sst_joiner_monitor_thread, NULL);

  if (ret)
  {
    WSREP_ERROR("sst_prepare_other(): mysql_thread_create() failed: %d (%s)",
                ret, strerror(ret));
    return -ret;
  }

  sst_joiner_completed= false;
  pthread_detach (monitor);

  return ret;
}

std::string wsrep_sst_prepare()
{
  const ssize_t ip_max= 256;
  char ip_buf[ip_max];
  const char* addr_in=  NULL;
  const char* addr_out= NULL;
  const char* method;

  if (!strcmp(wsrep_sst_method, WSREP_SST_SKIP))
  {
    return WSREP_STATE_TRANSFER_TRIVIAL;
  }

  /*
    Figure out SST receive address. Common for all SST methods.
  */
  // Attempt 1: wsrep_sst_receive_address
  if (wsrep_sst_receive_address &&
      strcmp (wsrep_sst_receive_address, WSREP_SST_ADDRESS_AUTO))
  {
    addr_in= wsrep_sst_receive_address;
  }

  //Attempt 2: wsrep_node_address
  else if (wsrep_node_address && strlen(wsrep_node_address))
  {
    wsp::Address addr(wsrep_node_address);

    if (!addr.is_valid())
    {
      WSREP_ERROR("Could not parse wsrep_node_address : %s",
                  wsrep_node_address);
      throw wsrep::runtime_error("Failed to prepare for SST. Unrecoverable");
    }
    memcpy(ip_buf, addr.get_address(), addr.get_address_len());
    addr_in= ip_buf;
  }
  // Attempt 3: Try to get the IP from the list of available interfaces.
  else
  {
    ssize_t ret= wsrep_guess_ip (ip_buf, ip_max);

    if (ret && ret < ip_max)
    {
      addr_in= ip_buf;
    }
    else
    {
      WSREP_ERROR("Failed to guess address to accept state transfer. "
                  "wsrep_sst_receive_address must be set manually.");
      throw wsrep::runtime_error("Could not prepare state transfer request");
    }
  }

  ssize_t addr_len= -ENOSYS;
  method = wsrep_sst_method;
  if (!strcmp(method, WSREP_SST_MYSQLDUMP))
  {
    addr_len= sst_prepare_mysqldump (addr_in, &addr_out);
    if (addr_len < 0)
    {
      throw wsrep::runtime_error("Could not prepare mysqldimp address");
    }
  }
  else
  {
    /*! A heuristic workaround until we learn how to stop and start engines */
    if (Wsrep_server_state::instance().is_initialized() &&
        Wsrep_server_state::instance().state() == Wsrep_server_state::s_joiner)
    {
      if (!strcmp(method, WSREP_SST_XTRABACKUP) ||
          !strcmp(method, WSREP_SST_XTRABACKUPV2))
      {
         WSREP_WARN("The %s SST method is deprecated, so it is automatically "
                    "replaced by %s", method, WSREP_SST_MARIABACKUP);
         method = WSREP_SST_MARIABACKUP;
      }
      // we already did SST at initializaiton, now engines are running
      // sql_print_information() is here because the message is too long
      // for WSREP_INFO.
      sql_print_information ("WSREP: "
                 "You have configured '%s' state snapshot transfer method "
                 "which cannot be performed on a running server. "
                 "Wsrep provider won't be able to fall back to it "
                 "if other means of state transfer are unavailable. "
                 "In that case you will need to restart the server.",
                 method);
      return "";
    }

    addr_len = sst_prepare_other (method, sst_auth_real,
                                  addr_in, &addr_out);
    if (addr_len < 0)
    {
      WSREP_ERROR("Failed to prepare for '%s' SST. Unrecoverable.",
                   method);
      throw wsrep::runtime_error("Failed to prepare for SST. Unrecoverable");
    }
  }

  std::string ret;
  ret += method;
  ret.push_back('\0');
  ret += addr_out;

  const char* method_ptr(ret.data());
  const char* addr_ptr(ret.data() + strlen(method_ptr) + 1);
  WSREP_DEBUG("Prepared SST request: %s|%s", method_ptr, addr_ptr);

  if (addr_out != addr_in) /* malloc'ed */ free ((char*)addr_out);

  return ret;
}

// helper method for donors
static int sst_run_shell (const char* cmd_str, char** env, int max_tries)
{
  int ret= 0;

  for (int tries=1; tries <= max_tries; tries++)
  {
    wsp::process proc (cmd_str, "r", env);

    if (NULL != proc.pipe())
    {
      proc.wait();
    }

    if ((ret= proc.error()))
    {
      WSREP_ERROR("Try %d/%d: '%s' failed: %d (%s)",
                  tries, max_tries, proc.cmd(), ret, strerror(ret));
      sleep (1);
    }
    else
    {
      WSREP_DEBUG("SST script successfully completed.");
      break;
    }
  }

  return -ret;
}

static void sst_reject_queries(my_bool close_conn)
{
  WSREP_INFO("Rejecting client queries for the duration of SST.");
  if (TRUE == close_conn) wsrep_close_client_connections(FALSE);
}

static int sst_donate_mysqldump (const char*         addr,
                                 const wsrep::gtid&  gtid,
                                 bool                bypass,
                                 char**              env) // carries auth info
{
  char host[256];
  wsp::Address address(addr);
  if (!address.is_valid())
  {
    WSREP_ERROR("Could not parse SST address : %s", addr);
    return 0;
  }
  memcpy(host, address.get_address(), address.get_address_len());
  int port= address.get_port();
  bool extra_args;
  size_t const cmd_len= estimate_cmd_len(&extra_args);
  wsp::string cmd_str(cmd_len);

  if (!cmd_str())
  {
    WSREP_ERROR("sst_donate_mysqldump(): "
                "could not allocate cmd buffer of %zd bytes", cmd_len);
    return -ENOMEM;
  }

  /*
    we enable new client connections so that mysqldump donation can connect in,
    but we reject local connections from modifyingcdata during SST, to keep
    data intact
  */
  if (!bypass && wsrep_sst_donor_rejects_queries) sst_reject_queries(TRUE);

  make_wsrep_defaults_file();

  std::ostringstream uuid_oss;
  uuid_oss << gtid.id();
  int ret= snprintf (cmd_str(), cmd_len,
                     "wsrep_sst_mysqldump "
                     WSREP_SST_OPT_ADDR " '%s' "
                     WSREP_SST_OPT_PORT " '%u' "
                     WSREP_SST_OPT_LPORT " '%u' "
                     WSREP_SST_OPT_SOCKET " '%s' "
                     "%s"
                     WSREP_SST_OPT_GTID " '%s:%lld,%d-%d-%llu' "
                     WSREP_SST_OPT_GTID_DOMAIN_ID " '%d'"
                     "%s",
                     addr, port, mysqld_port, mysqld_unix_port,
                     wsrep_defaults_file,
                     uuid_oss.str().c_str(), gtid.seqno().get(),
                     wsrep_gtid_server.domain_id, wsrep_gtid_server.server_id,
                     wsrep_gtid_server.seqno(),
                     wsrep_gtid_server.domain_id,
                     bypass ? " " WSREP_SST_OPT_BYPASS : "");

  if (ret < 0 || size_t(ret) >= cmd_len)
  {
    WSREP_ERROR("sst_donate_mysqldump(): snprintf() failed: %d", ret);
    return (ret < 0 ? ret : -EMSGSIZE);
  }

  if (extra_args)
    copy_orig_argv(cmd_str() + ret);

  WSREP_DEBUG("Running: '%s'", cmd_str());

  ret= sst_run_shell (cmd_str(), env, 3);

  wsrep::gtid sst_sent_gtid(ret == 0 ?
                            gtid :
                            wsrep::gtid(gtid.id(),
                                        wsrep::seqno::undefined()));
  Wsrep_server_state::instance().sst_sent(sst_sent_gtid, ret);

  wsrep_donor_monitor_end();

  return ret;
}

wsrep_seqno_t wsrep_locked_seqno= WSREP_SEQNO_UNDEFINED;

/*
  Create a file under data directory.
*/
static int sst_create_file(const char *name, const char *content)
{
  int err= 0;
  char *real_name;
  char *tmp_name;
  ssize_t len;
  FILE *file;

  len= strlen(mysql_real_data_home) + strlen(name) + 2;
  real_name= (char *) alloca(len);

  snprintf(real_name, (size_t) len, "%s/%s", mysql_real_data_home, name);

  tmp_name= (char *) alloca(len + 4);
  snprintf(tmp_name, (size_t) len + 4, "%s.tmp", real_name);

  file= fopen(tmp_name, "w+");

  if (0 == file)
  {
    err= errno;
    WSREP_ERROR("Failed to open '%s': %d (%s)", tmp_name, err, strerror(err));
  }
  else
  {
    // Write the specified content into the file.
    if (content != NULL)
    {
      fprintf(file, "%s\n", content);
      fsync(fileno(file));
    }

    fclose(file);

    if (rename(tmp_name, real_name) == -1)
    {
      err= errno;
      WSREP_ERROR("Failed to rename '%s' to '%s': %d (%s)", tmp_name,
                  real_name, err, strerror(err));
    }
  }

  return err;
}

static int run_sql_command(THD *thd, const char *query)
{
  thd->set_query((char *)query, strlen(query));

  Parser_state ps;
  if (ps.init(thd, thd->query(), thd->query_length()))
  {
    WSREP_ERROR("SST query: %s failed", query);
    return -1;
  }

  mysql_parse(thd, thd->query(), thd->query_length(), &ps);
  if (thd->is_error())
  {
    int const err= thd->get_stmt_da()->sql_errno();
    WSREP_WARN ("Error executing '%s': %d (%s)%s",
                query, err, thd->get_stmt_da()->message(),
                err == ER_UNKNOWN_SYSTEM_VARIABLE ?
                ". Was mysqld built with --with-innodb-disallow-writes ?" : "");
    thd->clear_error();
    return -1;
  }
  return 0;
}


static int sst_flush_tables(THD* thd)
{
  WSREP_INFO("Flushing tables for SST...");

  int err= 0;
  int not_used;
  /*
    Files created to notify the SST script about the outcome of table flush
    operation.
  */
  const char *flush_success= "tables_flushed";
  const char *flush_error= "sst_error";

  CHARSET_INFO *current_charset= thd->variables.character_set_client;

  if (!is_supported_parser_charset(current_charset))
  {
      /* Do not use non-supported parser character sets */
      WSREP_WARN("Current client character set is non-supported parser character set: %s", current_charset->cs_name.str);
      thd->variables.character_set_client= &my_charset_latin1;
      WSREP_WARN("For SST temporally setting character set to : %s",
                 my_charset_latin1.cs_name.str);
  }

  if (run_sql_command(thd, "FLUSH TABLES WITH READ LOCK"))
  {
    err= -1;
  }
  else
  {
    /*
      Make sure logs are flushed after global read lock acquired. In case
      reload fails, we must also release the acquired FTWRL.
    */
    if (reload_acl_and_cache(thd, REFRESH_ENGINE_LOG | REFRESH_BINARY_LOG,
                             (TABLE_LIST*) 0, &not_used))
    {
      thd->global_read_lock.unlock_global_read_lock(thd);
      err= -1;
    }
  }

  thd->variables.character_set_client= current_charset;

  if (err)
  {
    WSREP_ERROR("Failed to flush and lock tables");

    /*
      The SST must be aborted as the flush tables failed. Notify this to SST
      script by creating the error file.
    */
    int tmp;
    if ((tmp= sst_create_file(flush_error, NULL))) {
      err= tmp;
    }
  }
  else
  {
    WSREP_INFO("Tables flushed.");
    /*
      Tables have been flushed. Create a file with cluster state ID and
      wsrep_gtid_domain_id.
    */
    char content[100];
    snprintf(content, sizeof(content), "%s:%lld %d\n", wsrep_cluster_state_uuid,
             (long long)wsrep_locked_seqno, wsrep_gtid_server.domain_id);
    err= sst_create_file(flush_success, content);

    const char base_name[]= "tables_flushed";
    ssize_t const full_len= strlen(mysql_real_data_home) + strlen(base_name)+2;
    char *real_name= (char*) malloc(full_len);
    sprintf(real_name, "%s/%s", mysql_real_data_home, base_name);
    char *tmp_name= (char*) malloc(full_len + 4);
    sprintf(tmp_name, "%s.tmp", real_name);

    FILE* file= fopen(tmp_name, "w+");
    if (0 == file)
    {
      err= errno;
      WSREP_ERROR("Failed to open '%s': %d (%s)", tmp_name, err,strerror(err));
    }
    else
    {
      Wsrep_server_state& server_state= Wsrep_server_state::instance();
      std::ostringstream uuid_oss;

      uuid_oss << server_state.current_view().state_id().id();

      fprintf(file, "%s:%lld %u\n",
              uuid_oss.str().c_str(), server_state.pause_seqno().get(),
              wsrep_gtid_server.domain_id);
      fsync(fileno(file));
      fclose(file);
      if (rename(tmp_name, real_name) == -1)
      {
        err= errno;
        WSREP_ERROR("Failed to rename '%s' to '%s': %d (%s)",
                     tmp_name, real_name, err,strerror(err));
      }
    }
    free(real_name);
    free(tmp_name);
  }

  return err;
}


static void sst_disallow_writes (THD* thd, bool yes)
{
  char query_str[64]= { 0, };
  ssize_t const query_max= sizeof(query_str) - 1;
  CHARSET_INFO *current_charset;

  current_charset= thd->variables.character_set_client;

  if (!is_supported_parser_charset(current_charset))
  {
      /* Do not use non-supported parser character sets */
      WSREP_WARN("Current client character set is non-supported parser character set: %s", current_charset->cs_name.str);
      thd->variables.character_set_client= &my_charset_latin1;
      WSREP_WARN("For SST temporally setting character set to : %s",
                 my_charset_latin1.cs_name.str);
  }

  snprintf (query_str, query_max, "SET GLOBAL innodb_disallow_writes=%d",
            yes ? 1 : 0);

  if (run_sql_command(thd, query_str))
  {
    WSREP_ERROR("Failed to disallow InnoDB writes");
  }
  thd->variables.character_set_client= current_charset;
}

static void* sst_donor_thread (void* a)
{
  sst_thread_arg* arg= (sst_thread_arg*)a;

  WSREP_INFO("Running: '%s'", arg->cmd);

  int  err= 1;
  bool locked= false;

  const char*  out= NULL;
  const size_t out_len= 128;
  char         out_buf[out_len];

  wsrep_uuid_t  ret_uuid= WSREP_UUID_UNDEFINED;
  // seqno of complete SST
  wsrep_seqno_t ret_seqno= WSREP_SEQNO_UNDEFINED;

  // We turn off wsrep_on for this THD so that it can
  // operate with wsrep_ready == OFF
  // We also set this SST thread THD as system thread
  wsp::thd thd(FALSE, true);
  wsp::process proc(arg->cmd, "r", arg->env);

  err= -proc.error();

/* Inform server about SST script startup and release TO isolation */
  mysql_mutex_lock   (&arg->lock);
  arg->err= -err;
  mysql_cond_signal  (&arg->cond);
  mysql_mutex_unlock (&arg->lock); //! @note arg is unusable after that.

  if (proc.pipe() && !err)
  {
    long long total= 0;
    long long complete= 0;
    // total form previous stages
    long long total_prev= 0;

wait_signal:
    out= my_fgets (out_buf, out_len, proc.pipe());

    if (out)
    {
      static const char magic_flush[]= "flush tables";
      static const char magic_cont[]= "continue";
      static const char magic_done[]= "done";
      static const size_t done_len=strlen(magic_done);
      static const char magic_total[]= "total";
      static const size_t total_len=strlen(magic_total);
      static const char magic_complete[]= "complete";
      static const size_t complete_len=strlen(magic_complete);
      static const int from= WSREP_MEMBER_DONOR;

      if (!strncasecmp (out, magic_complete, complete_len))
      {
        sst_handle_complete(out + complete_len, total_prev, &total, &complete,
                            from);
        goto wait_signal;
      }
      else if (!strncasecmp (out, magic_total, total_len))
      {
        sst_handle_total(out + total_len, &total_prev, &total, &complete, from);
        goto wait_signal;
      }
      else if (!strcasecmp (out, magic_flush))
      {
        err= sst_flush_tables (thd.ptr);
        if (!err)
        {
          sst_disallow_writes (thd.ptr, true);
          /*
            Lets also keep statements that modify binary logs (like RESET LOGS,
            RESET MASTER) from proceeding until the files have been transferred
            to the joiner node.
          */
          if (mysql_bin_log.is_open())
          {
            mysql_mutex_lock(mysql_bin_log.get_log_lock());
          }

          locked= true;
          goto wait_signal;
        }
      }
      else if (!strcasecmp (out, magic_cont))
      {
        if (locked)
        {
          if (mysql_bin_log.is_open())
          {
            mysql_mutex_assert_owner(mysql_bin_log.get_log_lock());
            mysql_mutex_unlock(mysql_bin_log.get_log_lock());
          }
          sst_disallow_writes (thd.ptr, false);
          thd.ptr->global_read_lock.unlock_global_read_lock(thd.ptr);
          locked= false;
        }
        err=  0;
        goto wait_signal;
      }
      else if (!strncasecmp (out, magic_done, done_len))
      {
        err= sst_scan_uuid_seqno (out + strlen(magic_done) + 1,
                                  &ret_uuid, &ret_seqno);
      }
      else
      {
        WSREP_WARN("Received unknown signal: '%s'", out);
        err = -EINVAL;
        proc.wait();
      }
    }
    else
    {
      WSREP_ERROR("Failed to read from: %s", proc.cmd());
      proc.wait();
    }
    if (!err && proc.error()) err= -proc.error();
  }
  else
  {
    WSREP_ERROR("Failed to execute: %s : %d (%s)",
                proc.cmd(), err, strerror(err));
  }

  if (locked) // don't forget to unlock server before return
  {
    if (mysql_bin_log.is_open())
    {
      mysql_mutex_assert_owner(mysql_bin_log.get_log_lock());
      mysql_mutex_unlock(mysql_bin_log.get_log_lock());
    }
    sst_disallow_writes (thd.ptr, false);
    thd.ptr->global_read_lock.unlock_global_read_lock(thd.ptr);
  }

  wsrep::gtid gtid(wsrep::id(ret_uuid.data, sizeof(ret_uuid.data)),
                   wsrep::seqno(err ? wsrep::seqno::undefined() :
                                wsrep::seqno(ret_seqno)));

  Wsrep_server_state::instance().sst_sent(gtid, err);

  proc.wait();

  wsrep_donor_monitor_end();
  return nullptr;
}

static int sst_donate_other (const char*        method,
                             const char*        addr,
                             const wsrep::gtid& gtid,
                             bool               bypass,
                             char**             env) // carries auth info
{
  bool extra_args;
  size_t const cmd_len= estimate_cmd_len(&extra_args);
  wsp::string cmd_str(cmd_len);

  if (!cmd_str())
  {
    WSREP_ERROR("sst_donate_other(): "
                "could not allocate cmd buffer of %zd bytes", cmd_len);
    return -ENOMEM;
  }

  char* binlog_opt_val= NULL;
  char* binlog_index_opt_val= NULL;

  int ret;
  if ((ret= generate_binlog_opt_val(&binlog_opt_val)))
  {
    WSREP_ERROR("sst_donate_other(): generate_binlog_opt_val() failed: %d",ret);
    return ret;
  }

  if ((ret= generate_binlog_index_opt_val(&binlog_index_opt_val)))
  {
    WSREP_ERROR("sst_prepare_other(): generate_binlog_index_opt_val() failed %d",
                ret);
    if (binlog_opt_val) my_free(binlog_opt_val);
    return ret;
  }

  make_wsrep_defaults_file();

  std::ostringstream uuid_oss;
  uuid_oss << gtid.id();
  ret= snprintf (cmd_str(), cmd_len,
                 "wsrep_sst_%s "
                 WSREP_SST_OPT_ROLE " 'donor' "
                 WSREP_SST_OPT_ADDR " '%s' "
                 WSREP_SST_OPT_LPORT " '%u' "
                 WSREP_SST_OPT_SOCKET " '%s' "
                 WSREP_SST_OPT_DATA " '%s' "
                 "%s"
                 WSREP_SST_OPT_GTID " '%s:%lld' "
                 WSREP_SST_OPT_GTID_DOMAIN_ID " '%d'"
                 "%s"
                 "%s"
                 "%s",
                 method, addr, mysqld_port, mysqld_unix_port,
                 mysql_real_data_home,
                 wsrep_defaults_file,
                 uuid_oss.str().c_str(), gtid.seqno().get(), wsrep_gtid_server.domain_id,
                 binlog_opt_val, binlog_index_opt_val,
                 bypass ? " " WSREP_SST_OPT_BYPASS : "");

  my_free(binlog_opt_val);
  my_free(binlog_index_opt_val);

  if (ret < 0 || size_t(ret) >= cmd_len)
  {
    WSREP_ERROR("sst_donate_other(): snprintf() failed: %d", ret);
    return (ret < 0 ? ret : -EMSGSIZE);
  }

  if (extra_args)
    copy_orig_argv(cmd_str() + ret);

  if (!bypass && wsrep_sst_donor_rejects_queries) sst_reject_queries(FALSE);

  pthread_t tmp;
  sst_thread_arg arg(cmd_str(), env);

  mysql_mutex_lock (&arg.lock);

  ret= mysql_thread_create (key_wsrep_sst_donor, &tmp, NULL, sst_donor_thread, &arg);

  if (ret)
  {
    WSREP_ERROR("sst_donate_other(): mysql_thread_create() failed: %d (%s)",
                ret, strerror(ret));
    return ret;
  }

  mysql_cond_wait (&arg.cond, &arg.lock);

  WSREP_INFO("sst_donor_thread signaled with %d", arg.err);
  return arg.err;
}

/* return true if character can be a part of a filename */
static bool filename_char(int const c)
{
  return isalnum(c) || (c == '-') || (c == '_') || (c == '.');
}

/* return true if character can be a part of an address string */
static bool address_char(int const c)
{
  return filename_char(c) ||
         (c == ':') || (c == '[') || (c == ']') || (c == '/');
}

static bool check_request_str(const char* const str,
                              bool (*check) (int c))
{
  for (size_t i(0); str[i] != '\0'; ++i)
  {
    if (!check(str[i]))
    {
      WSREP_WARN("Illegal character in state transfer request: %i (%c).",
                 str[i], str[i]);
      return true;
    }
  }

  return false;
}

int wsrep_sst_donate(const std::string& msg,
                     const wsrep::gtid& current_gtid,
                     const bool         bypass)
{
  const char* method= msg.data();
  size_t method_len= strlen (method);

  if (check_request_str(method, filename_char))
  {
    WSREP_ERROR("Bad SST method name. SST canceled.");
    return WSREP_CB_FAILURE;
  }

  const char* data= method + method_len + 1;

  /* check for auth@addr separator */
  const char* addr= strrchr(data, '@');
  wsp::string remote_auth;
  if (addr)
  {
    remote_auth.set(strndup(data, addr - data));
    addr++;
  }
  else
  {
    // no auth part
    addr= data;
  }

  if (check_request_str(addr, address_char))
  {
    WSREP_ERROR("Bad SST address string. SST canceled.");
    return WSREP_CB_FAILURE;
  }

  wsp::env env(NULL);
  if (env.error())
  {
    WSREP_ERROR("wsrep_sst_donate_cb(): env var ctor failed: %d", -env.error());
    return WSREP_CB_FAILURE;
  }

  int ret;
  if ((ret= sst_append_env_var(env, WSREP_SST_AUTH_ENV, sst_auth_real)))
  {
    WSREP_ERROR("wsrep_sst_donate_cb(): appending auth env failed: %d", ret);
    return WSREP_CB_FAILURE;
  }

  if (remote_auth())
  {
    if ((ret= sst_append_env_var(env, WSREP_SST_REMOTE_AUTH_ENV,remote_auth())))
    {
      WSREP_ERROR("wsrep_sst_donate_cb(): appending remote auth env failed: "
                  "%d", ret);
      return WSREP_CB_FAILURE;
    }
  }

  if (data_home_dir)
  {
    if ((ret= sst_append_env_var(env, DATA_HOME_DIR_ENV, data_home_dir)))
    {
      WSREP_ERROR("wsrep_sst_donate_cb(): appending data "
                  "directory failed: %d", ret);
      return WSREP_CB_FAILURE;
    }
  }

  sst_donor_completed= false;
  pthread_t monitor;

  ret= mysql_thread_create (key_wsrep_sst_donor_monitor, &monitor, NULL, wsrep_sst_donor_monitor_thread, NULL);

  if (ret)
  {
    WSREP_ERROR("sst_donate: mysql_thread_create() failed: %d (%s)",
                ret, strerror(ret));
    return WSREP_CB_FAILURE;
  }

  if (!strcmp (WSREP_SST_MYSQLDUMP, method))
  {
    ret= sst_donate_mysqldump(addr, current_gtid, bypass, env());
  }
  else
  {
    ret= sst_donate_other(method, addr, current_gtid, bypass, env());
  }

  return (ret >= 0 ? 0 : 1);
}
