/* Copyright 2008-2015 Codership Oy <http://www.codership.com>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

#include "wsrep_sst.h"

#include <mysqld.h>
#include <m_ctype.h>
#include <my_sys.h>
#include <strfunc.h>
#include <sql_class.h>
#include <set_var.h>
#include <sql_acl.h>
#include <sql_reload.h>
#include <sql_parse.h>
#include "wsrep_priv.h"
#include "wsrep_utils.h"
#include "wsrep_xid.h"
#include <cstdio>
#include <cstdlib>

char wsrep_defaults_file[FN_REFLEN * 2 + 10 +
                         sizeof(WSREP_SST_OPT_CONF) +
                         sizeof(WSREP_SST_OPT_EXTRA_CONF)] = {0};

const char* wsrep_sst_method          = WSREP_SST_DEFAULT;
const char* wsrep_sst_receive_address = WSREP_SST_ADDRESS_AUTO;
const char* wsrep_sst_donor           = "";
      char* wsrep_sst_auth            = NULL;

// container for real auth string
static const char* sst_auth_real      = NULL;
my_bool wsrep_sst_donor_rejects_queries = FALSE;

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

bool wsrep_sst_method_update (sys_var *self, THD* thd, enum_var_type type)
{
    return 0;
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
                    WSREP_SST_OPT_EXTRA_CONF, " '", my_defaults_extra_file, "' ", NULL);
  }
}


// TODO: Improve address verification.
static bool sst_receive_address_check (const char* str)
{
    if (!strncasecmp(str, "127.0.0.1", strlen("127.0.0.1")) ||
        !strncasecmp(str, "localhost", strlen("localhost")))
    {
        return 1;
    }

    return 0;
}

bool  wsrep_sst_receive_address_check (sys_var *self, THD* thd, set_var* var)
{
  char addr_buf[FN_REFLEN];

  if ((! var->save_result.string_value.str) ||
      (var->save_result.string_value.length > (FN_REFLEN - 1))) // safety
  {
    goto err;
  }

  memcpy(addr_buf, var->save_result.string_value.str,
         var->save_result.string_value.length);
  addr_buf[var->save_result.string_value.length]= 0;

  if (sst_receive_address_check(addr_buf))
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
    v= my_strdup(value, MYF(0));
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
    sst_auth_real = v;

    // mask wsrep_sst_auth
    if (strlen(sst_auth_real))
    {
      if (wsrep_sst_auth) { my_free((void*) wsrep_sst_auth); }
      wsrep_sst_auth= my_strdup(WSREP_SST_AUTH_MASK, MYF(0));
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

void wsrep_sst_auth_init (const char* value)
{
    if (wsrep_sst_auth == value) wsrep_sst_auth = NULL;
    if (value) sst_auth_real_set (value);
}

bool  wsrep_sst_donor_check (sys_var *self, THD* thd, set_var* var)
{
  return 0;
}

bool wsrep_sst_donor_update (sys_var *self, THD* thd, enum_var_type type)
{
    return 0;
}

static wsrep_uuid_t cluster_uuid = WSREP_UUID_UNDEFINED;

bool wsrep_before_SE()
{
  return (wsrep_provider != NULL
          && strcmp (wsrep_provider,   WSREP_NONE)
          && strcmp (wsrep_sst_method, WSREP_SST_SKIP)
          && strcmp (wsrep_sst_method, WSREP_SST_MYSQLDUMP));
}

static bool            sst_complete = false;
static bool            sst_needed   = false;

void wsrep_sst_grab ()
{
  WSREP_INFO("wsrep_sst_grab()");
  if (mysql_mutex_lock (&LOCK_wsrep_sst)) abort();
  sst_complete = false;
  mysql_mutex_unlock (&LOCK_wsrep_sst);
}

// Wait for end of SST
bool wsrep_sst_wait ()
{
  if (mysql_mutex_lock (&LOCK_wsrep_sst)) abort();
  while (!sst_complete)
  {
    WSREP_INFO("Waiting for SST to complete.");
    mysql_cond_wait (&COND_wsrep_sst, &LOCK_wsrep_sst);
  }

  if (local_seqno >= 0)
  {
    WSREP_INFO("SST complete, seqno: %lld", (long long) local_seqno);
  }
  else
  {
    WSREP_ERROR("SST failed: %d (%s)",
                int(-local_seqno), strerror(-local_seqno));
  }

  mysql_mutex_unlock (&LOCK_wsrep_sst);

  return (local_seqno >= 0);
}

// Signal end of SST
void wsrep_sst_complete (const wsrep_uuid_t* sst_uuid,
                         wsrep_seqno_t       sst_seqno,
                         bool                needed)
{
  if (mysql_mutex_lock (&LOCK_wsrep_sst)) abort();
  if (!sst_complete)
  {
    sst_complete = true;
    sst_needed   = needed;
    local_uuid   = *sst_uuid;
    local_seqno  = sst_seqno;
    mysql_cond_signal (&COND_wsrep_sst);
  }
  else
  {
    /* This can happen when called from wsrep_synced_cb().
       At the moment there is no way to check there
       if main thread is still waiting for signal,
       so wsrep_sst_complete() is called from there
       each time wsrep_ready changes from FALSE -> TRUE.
    */
    WSREP_DEBUG("Nobody is waiting for SST.");
  }
  mysql_mutex_unlock (&LOCK_wsrep_sst);
}

void wsrep_sst_received (wsrep_t*            const wsrep,
                         const wsrep_uuid_t&       uuid,
                         wsrep_seqno_t       const seqno,
                         const void*         const state,
                         size_t              const state_len)
{
    wsrep_get_SE_checkpoint(local_uuid, local_seqno);

    if (memcmp(&local_uuid, &uuid, sizeof(wsrep_uuid_t)) ||
        local_seqno < seqno || seqno < 0)
    {
        wsrep_set_SE_checkpoint(uuid, seqno);
        local_uuid = uuid;
        local_seqno = seqno;
    }
    else if (local_seqno > seqno)
    {
        WSREP_WARN("SST postion is in the past: %lld, current: %lld. "
                   "Can't continue.",
                   (long long)seqno, (long long)local_seqno);
        unireg_abort(1);
    }

#ifdef GTID_SUPPORT
    wsrep_init_sidno(uuid);
#endif /* GTID_SUPPORT */

    if (wsrep)
    {
        int const rcode(seqno < 0 ? seqno : 0);
        wsrep_gtid_t const state_id = {
            uuid, (rcode ? WSREP_SEQNO_UNDEFINED : seqno)
        };

        wsrep->sst_received(wsrep, &state_id, state, state_len, rcode);
    }
}

// Let applier threads to continue
void wsrep_sst_continue ()
{
  if (sst_needed)
  {
    WSREP_INFO("Signalling provider to continue.");
    wsrep_sst_received (wsrep, local_uuid, local_seqno, NULL, 0);
  }
}

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

static int sst_scan_uuid_seqno (const char* str,
                                wsrep_uuid_t* uuid, wsrep_seqno_t* seqno)
{
  int offt = wsrep_uuid_scan (str, strlen(str), uuid);
  if (offt > 0 && strlen(str) > (unsigned int)offt && ':' == str[offt])
  {
    *seqno = strtoll (str + offt + 1, NULL, 10);
    if (*seqno != LLONG_MAX || errno != ERANGE)
    {
      return 0;
    }
  }

  WSREP_ERROR("Failed to parse uuid:seqno pair: '%s'", str);
  return EINVAL;
}

// get rid of trailing \n
static char* my_fgets (char* buf, size_t buf_len, FILE* stream)
{
   char* ret= fgets (buf, buf_len, stream);

   if (ret)
   {
       size_t len = strlen(ret);
       if (len > 0 && ret[len - 1] == '\n') ret[len - 1] = '\0';
   }

   return ret;
}

/*
  Generate opt_binlog_opt_val for sst_donate_other(), sst_prepare_other().

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
        my_strdup(opt_bin_logname, MYF(0)) : my_strdup("", MYF(0));
  }
  else
  {
    *ret= my_strdup("", MYF(0));
  }
  if (!*ret) return -ENOMEM;
  return 0;
}

static void* sst_joiner_thread (void* a)
{
  sst_thread_arg* arg= (sst_thread_arg*) a;
  int err= 1;

  {
    const char magic[] = "ready";
    const size_t magic_len = sizeof(magic) - 1;
    const size_t out_len = 512;
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
        if (proc.error()) err = proc.error();
      }
      else
      {
        err = 0;
      }
    }
    else
    {
      err = proc.error();
      WSREP_ERROR("Failed to execute: %s : %d (%s)",
                  arg->cmd, err, strerror(err));
    }

    // signal sst_prepare thread with ret code,
    // it will go on sending SST request
    mysql_mutex_lock   (&arg->lock);
    if (!err)
    {
      arg->ret_str = strdup (out + magic_len + 1);
      if (!arg->ret_str) err = ENOMEM;
    }
    arg->err = -err;
    mysql_cond_signal  (&arg->cond);
    mysql_mutex_unlock (&arg->lock); //! @note arg is unusable after that.

    if (err) return NULL; /* lp:808417 - return immediately, don't signal
                           * initializer thread to ensure single thread of
                           * shutdown. */

    wsrep_uuid_t  ret_uuid  = WSREP_UUID_UNDEFINED;
    wsrep_seqno_t ret_seqno = WSREP_SEQNO_UNDEFINED;

    // in case of successfull receiver start, wait for SST completion/end
    char* tmp = my_fgets (out, out_len, proc.pipe());

    proc.wait();
    err= EINVAL;

    if (!tmp)
    {
      WSREP_ERROR("Failed to read uuid:seqno and wsrep_gtid_domain_id from "
                  "joiner script.");
      if (proc.error()) err = proc.error();
    }
    else
    {
      // Read state ID (UUID:SEQNO) followed by wsrep_gtid_domain_id (if any).
      const char *pos= strchr(out, ' ');

      if (!pos) {
        // There is no wsrep_gtid_domain_id (some older version SST script?).
        err= sst_scan_uuid_seqno (out, &ret_uuid, &ret_seqno);

      } else {
        // Scan state ID first followed by wsrep_gtid_domain_id.
        char uuid[512];
        long int domain_id;
        size_t len= pos - out + 1;

        if (len > sizeof(uuid)) goto err;       // safety check
        memcpy(uuid, out, len);                 // including '\0'
        err= sst_scan_uuid_seqno (uuid, &ret_uuid, &ret_seqno);

        if (err)
        {
          goto err;
        }
        else if (wsrep_gtid_mode)
        {
          errno= 0;                             /* Reset the errno */
          domain_id= strtol(pos + 1, NULL, 10);
          err= errno;

          /* Check if we received a valid gtid_domain_id. */
          if (err == EINVAL || err == ERANGE || domain_id < 0x0 || domain_id > 0xFFFF)
          {
            WSREP_ERROR("Failed to get donor wsrep_gtid_domain_id.");
            err= EINVAL;
            goto err;
          } else {
            wsrep_gtid_domain_id= (uint32) domain_id;
          }
        }
      }
    }

err:

    if (err)
    {
      ret_uuid= WSREP_UUID_UNDEFINED;
      ret_seqno= -err;
    }

    // Tell initializer thread that SST is complete
    wsrep_sst_complete (&ret_uuid, ret_seqno, true);
  }

  return NULL;
}

#define WSREP_SST_AUTH_ENV "WSREP_SST_OPT_AUTH"

static int sst_append_auth_env(wsp::env& env, const char* sst_auth)
{
  int const sst_auth_size= strlen(WSREP_SST_AUTH_ENV) + 1 /* = */
    + (sst_auth ? strlen(sst_auth) : 0) + 1 /* \0 */;

  wsp::string sst_auth_str(sst_auth_size); // for automatic cleanup on return
  if (!sst_auth_str()) return -ENOMEM;

  int ret= snprintf(sst_auth_str(), sst_auth_size, "%s=%s",
                    WSREP_SST_AUTH_ENV, sst_auth ? sst_auth : "");

  if (ret < 0 || ret >= sst_auth_size)
  {
    WSREP_ERROR("sst_append_auth_env(): snprintf() failed: %d", ret);
    return (ret < 0 ? ret : -EMSGSIZE);
  }

  env.append(sst_auth_str());
  return -env.error();
}

static ssize_t sst_prepare_other (const char*  method,
                                  const char*  sst_auth,
                                  const char*  addr_in,
                                  const char** addr_out)
{
  int const cmd_len= 4096;
  wsp::string cmd_str(cmd_len);

  if (!cmd_str())
  {
    WSREP_ERROR("sst_prepare_other(): could not allocate cmd buffer of %d bytes",
                cmd_len);
    return -ENOMEM;
  }

  const char* binlog_opt= "";
  char* binlog_opt_val= NULL;

  int ret;
  if ((ret= generate_binlog_opt_val(&binlog_opt_val)))
  {
    WSREP_ERROR("sst_prepare_other(): generate_binlog_opt_val() failed: %d",
                ret);
    return ret;
  }
  if (strlen(binlog_opt_val)) binlog_opt= WSREP_SST_OPT_BINLOG;

  make_wsrep_defaults_file();

  ret= snprintf (cmd_str(), cmd_len,
                 "wsrep_sst_%s "
                 WSREP_SST_OPT_ROLE" 'joiner' "
                 WSREP_SST_OPT_ADDR" '%s' "
                 WSREP_SST_OPT_DATA" '%s' "
                 " %s "
                 WSREP_SST_OPT_PARENT" '%d'"
                 " %s '%s' ",
                 method, addr_in, mysql_real_data_home,
                 wsrep_defaults_file, (int)getpid(),
                 binlog_opt, binlog_opt_val);
  my_free(binlog_opt_val);

  if (ret < 0 || ret >= cmd_len)
  {
    WSREP_ERROR("sst_prepare_other(): snprintf() failed: %d", ret);
    return (ret < 0 ? ret : -EMSGSIZE);
  }

  wsp::env env(NULL);
  if (env.error())
  {
    WSREP_ERROR("sst_prepare_other(): env. var ctor failed: %d", -env.error());
    return -env.error();
  }

  if ((ret= sst_append_auth_env(env, sst_auth)))
  {
    WSREP_ERROR("sst_prepare_other(): appending auth failed: %d", ret);
    return ret;
  }

  pthread_t tmp;
  sst_thread_arg arg(cmd_str(), env());
  mysql_mutex_lock (&arg.lock);
  ret = pthread_create (&tmp, NULL, sst_joiner_thread, &arg);
  if (ret)
  {
    WSREP_ERROR("sst_prepare_other(): pthread_create() failed: %d (%s)",
                ret, strerror(ret));
    return -ret;
  }
  mysql_cond_wait (&arg.cond, &arg.lock);

  *addr_out= arg.ret_str;

  if (!arg.err)
    ret = strlen(*addr_out);
  else
  {
    assert (arg.err < 0);
    ret = arg.err;
  }

  pthread_detach (tmp);

  return ret;
}

extern uint  mysqld_port;

/*! Just tells donor where to send mysqldump */
static ssize_t sst_prepare_mysqldump (const char*  addr_in,
                                      const char** addr_out)
{
  ssize_t ret = strlen (addr_in);

  if (!strrchr(addr_in, ':'))
  {
    ssize_t s = ret + 7;
    char* tmp = (char*) malloc (s);

    if (tmp)
    {
      ret= snprintf (tmp, s, "%s:%u", addr_in, mysqld_port);

      if (ret > 0 && ret < s)
      {
        *addr_out= tmp;
        return ret;
      }
      if (ret > 0) /* buffer too short */ ret = -EMSGSIZE;
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

  return ret;
}

static bool SE_initialized = false;

ssize_t wsrep_sst_prepare (void** msg)
{
  const char* addr_in=  NULL;
  const char* addr_out= NULL;

  if (!strcmp(wsrep_sst_method, WSREP_SST_SKIP))
  {
    ssize_t ret = strlen(WSREP_STATE_TRANSFER_TRIVIAL) + 1;
    *msg = strdup(WSREP_STATE_TRANSFER_TRIVIAL);
    if (!msg)
    {
      WSREP_ERROR("Could not allocate %zd bytes for state request", ret);
      unireg_abort(1);
    }
    return ret;
  }

  /*
    Figure out SST receive address. Common for all SST methods.
  */
  char ip_buf[256];
  const ssize_t ip_max= sizeof(ip_buf);

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
      unireg_abort(1);
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
      unireg_abort(1);
    }
  }

  ssize_t addr_len= -ENOSYS;
  if (!strcmp(wsrep_sst_method, WSREP_SST_MYSQLDUMP))
  {
    addr_len= sst_prepare_mysqldump (addr_in, &addr_out);
    if (addr_len < 0) unireg_abort(1);
  }
  else
  {
    /*! A heuristic workaround until we learn how to stop and start engines */
    if (SE_initialized)
    {
      // we already did SST at initializaiton, now engines are running
      // sql_print_information() is here because the message is too long
      // for WSREP_INFO.
      sql_print_information ("WSREP: "
                 "You have configured '%s' state snapshot transfer method "
                 "which cannot be performed on a running server. "
                 "Wsrep provider won't be able to fall back to it "
                 "if other means of state transfer are unavailable. "
                 "In that case you will need to restart the server.",
                 wsrep_sst_method);
      *msg = 0;
      return 0;
    }

    addr_len = sst_prepare_other (wsrep_sst_method, sst_auth_real,
                                  addr_in, &addr_out);
    if (addr_len < 0)
    {
      WSREP_ERROR("Failed to prepare for '%s' SST. Unrecoverable.",
                   wsrep_sst_method);
      unireg_abort(1);
    }
  }

  size_t const method_len(strlen(wsrep_sst_method));
  size_t const msg_len   (method_len + addr_len + 2 /* + auth_len + 1*/);

  *msg = malloc (msg_len);
  if (NULL != *msg) {
    char* const method_ptr(reinterpret_cast<char*>(*msg));
    strcpy (method_ptr, wsrep_sst_method);
    char* const addr_ptr(method_ptr + method_len + 1);
    strcpy (addr_ptr, addr_out);

    WSREP_INFO ("Prepared SST request: %s|%s", method_ptr, addr_ptr);
  }
  else {
    WSREP_ERROR("Failed to allocate SST request of size %zu. Can't continue.",
                msg_len);
    unireg_abort(1);
  }

  if (addr_out != addr_in) /* malloc'ed */ free ((char*)addr_out);

  return msg_len;
}

// helper method for donors
static int sst_run_shell (const char* cmd_str, char** env, int max_tries)
{
  int ret = 0;

  for (int tries=1; tries <= max_tries; tries++)
  {
    wsp::process proc (cmd_str, "r", env);

    if (NULL != proc.pipe())
    {
      proc.wait();
    }

    if ((ret = proc.error()))
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
    wsrep_ready_set (FALSE); // this will be resotred when donor becomes synced
    WSREP_INFO("Rejecting client queries for the duration of SST.");
    if (TRUE == close_conn) wsrep_close_client_connections(FALSE);
}

static int sst_donate_mysqldump (const char*         addr,
                                 const wsrep_uuid_t* uuid,
                                 const char*         uuid_str,
                                 wsrep_seqno_t       seqno,
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

  int const cmd_len= 4096;
  wsp::string  cmd_str(cmd_len);

  if (!cmd_str())
  {
    WSREP_ERROR("sst_donate_mysqldump(): "
                "could not allocate cmd buffer of %d bytes", cmd_len);
    return -ENOMEM;
  }

  if (!bypass && wsrep_sst_donor_rejects_queries) sst_reject_queries(TRUE);

  make_wsrep_defaults_file();

  int ret= snprintf (cmd_str(), cmd_len,
                     "wsrep_sst_mysqldump "
                     WSREP_SST_OPT_HOST" '%s' "
                     WSREP_SST_OPT_PORT" '%d' "
                     WSREP_SST_OPT_LPORT" '%u' "
                     WSREP_SST_OPT_SOCKET" '%s' "
                     " %s "
                     WSREP_SST_OPT_GTID" '%s:%lld' "
                     WSREP_SST_OPT_GTID_DOMAIN_ID" '%d'"
                     "%s",
                     host, port, mysqld_port, mysqld_unix_port,
                     wsrep_defaults_file, uuid_str,
                     (long long)seqno, wsrep_gtid_domain_id,
                     bypass ? " " WSREP_SST_OPT_BYPASS : "");

  if (ret < 0 || ret >= cmd_len)
  {
    WSREP_ERROR("sst_donate_mysqldump(): snprintf() failed: %d", ret);
    return (ret < 0 ? ret : -EMSGSIZE);
  }

  WSREP_DEBUG("Running: '%s'", cmd_str());

  ret= sst_run_shell (cmd_str(), env, 3);

  wsrep_gtid_t const state_id = { *uuid, (ret ? WSREP_SEQNO_UNDEFINED : seqno)};

  wsrep->sst_sent (wsrep, &state_id, ret);

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
      WSREP_WARN("Current client character set is non-supported parser character set: %s", current_charset->csname);
      thd->variables.character_set_client = &my_charset_latin1;
      WSREP_WARN("For SST temporally setting character set to : %s",
	      my_charset_latin1.csname);
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

  thd->variables.character_set_client = current_charset;

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
             (long long)wsrep_locked_seqno, wsrep_gtid_domain_id);
    err= sst_create_file(flush_success, content);
  }

  return err;
}


static void sst_disallow_writes (THD* thd, bool yes)
{
  char query_str[64] = { 0, };
  ssize_t const query_max = sizeof(query_str) - 1;
  CHARSET_INFO *current_charset;

  current_charset = thd->variables.character_set_client;

  if (!is_supported_parser_charset(current_charset))
  {
      /* Do not use non-supported parser character sets */
      WSREP_WARN("Current client character set is non-supported parser character set: %s", current_charset->csname);
      thd->variables.character_set_client = &my_charset_latin1;
      WSREP_WARN("For SST temporally setting character set to : %s",
	      my_charset_latin1.csname);
  }

  snprintf (query_str, query_max, "SET GLOBAL innodb_disallow_writes=%d",
            yes ? 1 : 0);

  if (run_sql_command(thd, query_str))
  {
    WSREP_ERROR("Failed to disallow InnoDB writes");
  }
  thd->variables.character_set_client = current_charset;
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
  wsrep_seqno_t ret_seqno= WSREP_SEQNO_UNDEFINED; // seqno of complete SST

  wsp::thd thd(FALSE); // we turn off wsrep_on for this THD so that it can
                       // operate with wsrep_ready == OFF
  wsp::process proc(arg->cmd, "r", arg->env);

  err= proc.error();

/* Inform server about SST script startup and release TO isolation */
  mysql_mutex_lock   (&arg->lock);
  arg->err = -err;
  mysql_cond_signal  (&arg->cond);
  mysql_mutex_unlock (&arg->lock); //! @note arg is unusable after that.

  if (proc.pipe() && !err)
  {
wait_signal:
    out= my_fgets (out_buf, out_len, proc.pipe());

    if (out)
    {
      const char magic_flush[]= "flush tables";
      const char magic_cont[]= "continue";
      const char magic_done[]= "done";

      if (!strcasecmp (out, magic_flush))
      {
        err= sst_flush_tables (thd.ptr);
        if (!err)
        {
          sst_disallow_writes (thd.ptr, true);
          locked= true;
          goto wait_signal;
        }
      }
      else if (!strcasecmp (out, magic_cont))
      {
        if (locked)
        {
          sst_disallow_writes (thd.ptr, false);
          thd.ptr->global_read_lock.unlock_global_read_lock (thd.ptr);
          locked= false;
        }
        err=  0;
        goto wait_signal;
      }
      else if (!strncasecmp (out, magic_done, strlen(magic_done)))
      {
        err= sst_scan_uuid_seqno (out + strlen(magic_done) + 1,
                                  &ret_uuid, &ret_seqno);
      }
      else
      {
        WSREP_WARN("Received unknown signal: '%s'", out);
      }
    }
    else
    {
      WSREP_ERROR("Failed to read from: %s", proc.cmd());
      proc.wait();
    }
    if (!err && proc.error()) err= proc.error();
  }
  else
  {
    WSREP_ERROR("Failed to execute: %s : %d (%s)",
                proc.cmd(), err, strerror(err));
  }

  if (locked) // don't forget to unlock server before return
  {
    sst_disallow_writes (thd.ptr, false);
    thd.ptr->global_read_lock.unlock_global_read_lock (thd.ptr);
  }

  // signal to donor that SST is over
  struct wsrep_gtid const state_id = {
      ret_uuid, err ? WSREP_SEQNO_UNDEFINED : ret_seqno
  };
  wsrep->sst_sent (wsrep, &state_id, -err);
  proc.wait();

  return NULL;
}



static int sst_donate_other (const char*   method,
                             const char*   addr,
                             const char*   uuid,
                             wsrep_seqno_t seqno,
                             bool          bypass,
                             char**        env) // carries auth info
{
  int const cmd_len= 4096;
  wsp::string  cmd_str(cmd_len);

  if (!cmd_str())
  {
    WSREP_ERROR("sst_donate_other(): "
                "could not allocate cmd buffer of %d bytes", cmd_len);
    return -ENOMEM;
  }

  const char* binlog_opt= "";
  char* binlog_opt_val= NULL;

  int ret;
  if ((ret= generate_binlog_opt_val(&binlog_opt_val)))
  {
    WSREP_ERROR("sst_donate_other(): generate_binlog_opt_val() failed: %d",ret);
    return ret;
  }
  if (strlen(binlog_opt_val)) binlog_opt= WSREP_SST_OPT_BINLOG;

  make_wsrep_defaults_file();

  ret= snprintf (cmd_str(), cmd_len,
                 "wsrep_sst_%s "
                 WSREP_SST_OPT_ROLE" 'donor' "
                 WSREP_SST_OPT_ADDR" '%s' "
                 WSREP_SST_OPT_SOCKET" '%s' "
                 WSREP_SST_OPT_DATA" '%s' "
                 " %s "
                 " %s '%s' "
                 WSREP_SST_OPT_GTID" '%s:%lld' "
                 WSREP_SST_OPT_GTID_DOMAIN_ID" '%d'"
                 "%s",
                 method, addr, mysqld_unix_port, mysql_real_data_home,
                 wsrep_defaults_file,
                 binlog_opt, binlog_opt_val,
                 uuid, (long long) seqno, wsrep_gtid_domain_id,
                 bypass ? " " WSREP_SST_OPT_BYPASS : "");
  my_free(binlog_opt_val);

  if (ret < 0 || ret >= cmd_len)
  {
    WSREP_ERROR("sst_donate_other(): snprintf() failed: %d", ret);
    return (ret < 0 ? ret : -EMSGSIZE);
  }

  if (!bypass && wsrep_sst_donor_rejects_queries) sst_reject_queries(FALSE);

  pthread_t tmp;
  sst_thread_arg arg(cmd_str(), env);
  mysql_mutex_lock (&arg.lock);
  ret = pthread_create (&tmp, NULL, sst_donor_thread, &arg);
  if (ret)
  {
    WSREP_ERROR("sst_donate_other(): pthread_create() failed: %d (%s)",
                ret, strerror(ret));
    return ret;
  }
  mysql_cond_wait (&arg.cond, &arg.lock);

  WSREP_INFO("sst_donor_thread signaled with %d", arg.err);
  return arg.err;
}

wsrep_cb_status_t wsrep_sst_donate_cb (void* app_ctx, void* recv_ctx,
                                       const void* msg, size_t msg_len,
                                       const wsrep_gtid_t* current_gtid,
                                       const char* state, size_t state_len,
                                       bool bypass)
{
  /* This will be reset when sync callback is called.
   * Should we set wsrep_ready to FALSE here too? */

  wsrep_config_state.set(WSREP_MEMBER_DONOR);

  const char* method = (char*)msg;
  size_t method_len  = strlen (method);
  const char* data   = method + method_len + 1;

  char uuid_str[37];
  wsrep_uuid_print (&current_gtid->uuid, uuid_str, sizeof(uuid_str));

  wsp::env env(NULL);
  if (env.error())
  {
    WSREP_ERROR("wsrep_sst_donate_cb(): env var ctor failed: %d", -env.error());
    return WSREP_CB_FAILURE;
  }

  int ret;
  if ((ret= sst_append_auth_env(env, sst_auth_real)))
  {
    WSREP_ERROR("wsrep_sst_donate_cb(): appending auth env failed: %d", ret);
    return WSREP_CB_FAILURE;
  }

  if (!strcmp (WSREP_SST_MYSQLDUMP, method))
  {
    ret = sst_donate_mysqldump(data, &current_gtid->uuid, uuid_str,
                               current_gtid->seqno, bypass, env());
  }
  else
  {
    ret = sst_donate_other(method, data, uuid_str,
                           current_gtid->seqno, bypass, env());
  }

  return (ret >= 0 ? WSREP_CB_SUCCESS : WSREP_CB_FAILURE);
}

void wsrep_SE_init_grab()
{
  if (mysql_mutex_lock (&LOCK_wsrep_sst_init)) abort();
}

void wsrep_SE_init_wait()
{
  while (SE_initialized == false)
  {
    mysql_cond_wait (&COND_wsrep_sst_init, &LOCK_wsrep_sst_init);
  }
  mysql_mutex_unlock (&LOCK_wsrep_sst_init);
}

void wsrep_SE_init_done()
{
  mysql_cond_signal (&COND_wsrep_sst_init);
  mysql_mutex_unlock (&LOCK_wsrep_sst_init);
}

void wsrep_SE_initialized()
{
  SE_initialized = true;
}
