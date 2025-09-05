/*  Copyright (c) 2018, 2024, Oracle and/or its affiliates.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License, version 2.0,
    as published by the Free Software Foundation.

    This program is designed to work with certain software (including
    but not limited to OpenSSL) that is licensed under separate terms,
    as designated in a particular file or component or in included license
    documentation.  The authors of MySQL hereby grant you an additional
    permission to link the program and your derivative works with the
    separately licensed software that they have either included with
    the program or referenced in the documentation.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License, version 2.0, for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

*/
#include "sql_plugin.h"
#include "my_global.h"
#include "mysql/plugin.h"
#include "mysql/service_clone_protocol.h"

#include "my_byteorder.h"
#include "mysql.h"
#include "mysqld.h"
#include "protocol.h"
#include "set_var.h"
#include "sql_class.h"
#include "sql_show.h"
// #include "ssl_init_callback.h"
#include "sys_vars_shared.h"
#include "sql_common.h"
#include "backup.h"
#include "mdl.h"
#include <cctype>

/** The minimum idle timeout in seconds. It is kept at 8 hours which is also
the Server default. Currently recipient sends ACK during state transition.
In future we could have better time controlled ACK. */
static const uint32_t MIN_IDLE_TIME_OUT_SEC = 8 * 60 * 60;

/** Minimum read timeout in seconds. Maintain above the donor ACK frequency. */
static const uint32_t MIN_READ_TIME_OUT_SEC = 30;

/** Minimum write timeout in seconds. Disallow configuring it to too low. We
might need a separate clone configuration in future or retry on failure. */
static const uint32_t MIN_WRITE_TIME_OUT_SEC = 60;

/** Set Network read timeout.
@param[in,out]	net	network object
@param[in]	timeout	time out in seconds */
static void set_read_timeout(NET *net, uint32_t timeout)
{
  if (timeout < MIN_READ_TIME_OUT_SEC) {
    timeout = MIN_READ_TIME_OUT_SEC;
  }
  my_net_set_read_timeout(net, timeout);
}

/** Set Network write timeout.
@param[in,out]	net	network object
@param[in]	timeout	time out in seconds */
static void set_write_timeout(NET *net, uint32_t timeout)
{
  if (timeout < MIN_WRITE_TIME_OUT_SEC) {
    timeout = MIN_WRITE_TIME_OUT_SEC;
  }
  my_net_set_write_timeout(net, timeout);
}

/** Set Network idle timeout.
@param[in,out]	net	network object
@param[in]	timeout	time out in seconds */
static void set_idle_timeout(NET *net, uint32_t timeout)
{
  if (timeout < MIN_IDLE_TIME_OUT_SEC) {
    timeout = MIN_IDLE_TIME_OUT_SEC;
  }
  my_net_set_read_timeout(net, timeout);
}

MYSQL_THD create_thd();
void destroy_thd(MYSQL_THD thd);

THD* clone_start_statement(THD *thd, PSI_thread_key thread_key,
                           PSI_statement_key statement_key,
			   const char *thd_name)
{
  if (!thd) {
    my_thread_init();
    /* Create thread with input key for PFS */
    thd= create_thd();
#ifdef HAVE_PSI_THREAD_INTERFACE
    PSI_thread *psi= PSI_CALL_new_thread(thread_key, NULL, 0);
    PSI_CALL_set_thread_os_id(psi);
    PSI_CALL_set_thread(psi);
#endif
    my_thread_set_name(thd_name);
  }

  /* Create and set PFS statement key */
  if (statement_key != PSI_NOT_INSTRUMENTED) {
    if (thd->m_statement_psi == nullptr) {
      thd->m_statement_psi = MYSQL_START_STATEMENT(
          &thd->m_statement_state, statement_key, thd->get_db(),
          thd->db.length, thd->charset(), nullptr);
    } else if (thd->get_command() != COM_STMT_EXECUTE) {
      thd->m_statement_psi=
          MYSQL_REFINE_STATEMENT(thd->m_statement_psi, statement_key);
    }
  }
  return thd;
}

void clone_finish_statement(THD *thd)
{
  assert(thd->m_statement_psi == nullptr);
  thd->set_psi(nullptr);
  destroy_thd(thd);
  my_thread_end();
}

void clone_get_error(THD * thd, uint32_t *err_num, const char **err_mesg)
{
  *err_num = 0;
  *err_mesg = nullptr;
  /* Check if THD exists. */
  if (thd == nullptr) {
    return;
  }
  /* Check if DA exists. */
  auto da = thd->get_stmt_da();
  if (da == nullptr || !da->is_error()) {
    return;
  }
  /* Get error from DA. */
  *err_num = da->sql_errno();
  *err_mesg = da->message();
}

int clone_get_command(THD *thd, uchar *command, uchar **com_buffer,
                      size_t *buffer_length)
{
  NET *net = &thd->net;

  if (net->last_errno != 0) {
    return static_cast<int>(net->last_errno);
  }

  /* flush any data in write buffer */
  if (!net_flush(net)) {
    net_new_transaction(net);

    /* Set idle timeout while waiting for commands. Earlier we used server
    configuration "wait_timeout" but this causes unwanted timeout in clone
    when user configures the value too low. */
    set_idle_timeout(net, thd->variables.net_wait_timeout);

    *buffer_length = my_net_read(net);

    set_read_timeout(net, thd->variables.net_read_timeout);
    set_write_timeout(net, thd->variables.net_write_timeout);

    if (*buffer_length != packet_error && *buffer_length != 0) {
      *com_buffer = net->read_pos;
      *command = **com_buffer;

      ++(*com_buffer);
      --(*buffer_length);

      return 0;
    }
  }

  int err = static_cast<int>(net->last_errno);

  if (err == 0) {
    net->last_errno = ER_NET_PACKETS_OUT_OF_ORDER;
    err = ER_NET_PACKETS_OUT_OF_ORDER;
    my_error(err, MYF(0));
  }
  return err;
}

int clone_send_response(THD * thd, bool secure, uchar *packet, size_t length)
{
  NET *net = &thd->net;

  if (net->last_errno != 0) {
    return static_cast<int>(net->last_errno);
  }

  auto conn_type= vio_type(net->vio);

  if (secure && conn_type != VIO_TYPE_SSL) {
    my_error(ER_CLONE_ENCRYPTION, MYF(0));
    return ER_CLONE_ENCRYPTION;
  }

  net_clear(net, true);

  if (!my_net_write(net, packet, length) && !net_flush(net)) {
    return 0;
  }

  const int err = static_cast<int>(net->last_errno);

  assert(err != 0);
  return err;
}

/**
  Get configuration parameter value in utf8
  @param[in]   thd   server session THD
  @param[in]   config_name  parameter name
  @param[out]  utf8_val     parameter value in utf8 string
  @return error code.
*/
static int get_utf8_config(THD *thd, std::string config_name,
                           String &utf8_val)
{
  char val_buf[1024];
  SHOW_VAR show;
  show.type= SHOW_SYS;

  /* Get system configuration parameter. */
  mysql_prlock_rdlock(&LOCK_system_variables_hash);
  auto var= intern_find_sys_var(config_name.c_str(), config_name.length());
  mysql_prlock_unlock(&LOCK_system_variables_hash);

  if (var == nullptr) {
    my_error(ER_INTERNAL_ERROR, MYF(0),
             "Clone failed to get system configuration parameter.");
    return ER_INTERNAL_ERROR;
  }

  show.value= reinterpret_cast<char *>(var);
  show.name= var->name.str;

  mysql_mutex_lock(&LOCK_global_system_variables);
  size_t val_length;
  const CHARSET_INFO *fromcs;

  auto value= get_one_variable(thd, &show, OPT_GLOBAL, SHOW_SYS, nullptr,
                                &fromcs, val_buf, &val_length);

  uint dummy_err;
  const CHARSET_INFO *tocs= &my_charset_utf8mb4_bin;
  utf8_val.copy(value, val_length, fromcs, tocs, &dummy_err);

  mysql_mutex_unlock(&LOCK_global_system_variables);
  return 0;
}

using Clone_Values= std::vector<std::string>;
using Clone_Key_Values= std::vector<std::pair<std::string, std::string>>;

int clone_get_charsets(MYSQL_THD thd, void *char_sets)
{
  auto charset_vals= static_cast<Clone_Values *>(char_sets);

  for (CHARSET_INFO **cs= all_charsets;
       cs < all_charsets + array_elements(all_charsets); cs++)
  {
    CHARSET_INFO *tmp_cs= cs[0];
    if (tmp_cs && (tmp_cs->state & MY_CS_PRIMARY) &&
        (tmp_cs->state & MY_CS_AVAILABLE))
    {
      std::string charset;
      /* Set the collation name. */
      charset.assign(tmp_cs->coll_name.str, tmp_cs->coll_name.length);
      charset_vals->push_back(charset);
    }
  }
  return 0;
}

int clone_get_configs(THD * thd, void *configs)
{
  int err= 0;
  auto key_vals= static_cast<Clone_Key_Values*>(configs);

  for (auto &key_val : *key_vals)
  {
    String utf8_str;
    auto &config_name= key_val.first;
    err = get_utf8_config(thd, config_name, utf8_str);

    if (err != 0)
      break;

    auto &config_val= key_val.second;
    config_val.assign(utf8_str.c_ptr_quick());
  }
  return err;
}

/**
 Says whether a character is a digit or a dot.
 @param c character
 @return true if c is a digit or a dot, otherwise false
 */
static bool is_digit_or_dot(char c) { return std::isdigit(c) || c == '.'; }

/**
 Compares versions, ignoring suffixes, i.e. 8.0.25 should be the same
 as 8.0.25-debug, but 8.0.25 isn't the same as 8.0.251.
 @param ver1 version1 string
 @param ver2 version2 string
 @return true if versions match (ignoring suffixes), false otherwise
 */
static bool compare_prefix_version(std::string ver1, std::string ver2)
{
  size_t i;
  /* we iterate  over both versions */
  for (i= 0; i < ver1.size() && i < ver2.size(); i++)
  {
    if (!is_digit_or_dot(ver1[i]))
      /*  If in one version we have something else than digit or dot,
      we check what's in other version - if we also have a suffix or still
      a version. */
      return !is_digit_or_dot(ver2[i]);

    /* We still compare version, and have a difference */
    if (ver1[i] != ver2[i]) return false;
  }
  if (i < ver1.size())
    /* we finished iterate over ver2, but still have some digits in ver1 */
    return !std::isdigit(ver1[i]);

  if (i < ver2.size())
    /* we finished iterate over ver1, but still have some digits in ver2 */
    return !std::isdigit(ver2[i]);

  return true;
}

int clone_set_backup_stage(MYSQL_THD thd, uchar stage)
{
  return run_backup_stage(thd, static_cast<backup_stages>(stage));
}

int clone_backup_lock(MYSQL_THD thd, const char *db,
                      const char *tbl)
{
  MDL_request request;
  MDL_REQUEST_INIT(&request,MDL_key::TABLE, db, tbl,
                   MDL_SHARED_HIGH_PRIO, MDL_EXPLICIT);
  if (thd->mdl_context.acquire_lock(&request,
                                    thd->variables.lock_wait_timeout))
    return 1;
  thd->mdl_backup_lock = request.ticket;
  return 0;
}

int clone_backup_unlock(MYSQL_THD thd)
{
  if (thd->mdl_backup_lock)
    thd->mdl_context.release_lock(thd->mdl_backup_lock);
  thd->mdl_backup_lock= 0;
  return 0;
}
