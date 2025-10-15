/* Copyright (C) 2013-2024 Codership Oy <info@codership.com>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License along
   with this program; if not, write to the Free Software Foundation, Inc.,
   51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA. */

#include "mariadb.h"
#include "mysql/service_wsrep.h"
#include "wsrep_applier.h"

#include "wsrep_priv.h"
#include "wsrep_binlog.h" // wsrep_dump_rbr_buf()
#include "wsrep_xid.h"
#include "wsrep_thd.h"
#include "wsrep_trans_observer.h"
#include "wsrep_schema.h" // wsrep_schema

#include "slave.h" // opt_log_slave_updates
#include "debug_sync.h"

/*
  read the first event from (*buf). The size of the (*buf) is (*buf_len).
  At the end (*buf) is shitfed to point to the following event or NULL and
  (*buf_len) will be changed to account just being read bytes of the 1st event.
*/
static Log_event* wsrep_read_log_event(
    char **arg_buf, size_t *arg_buf_len,
    const Format_description_log_event *description_event)
{
  DBUG_ENTER("wsrep_read_log_event");
  char *head= (*arg_buf);

  uint data_len= uint4korr(head + EVENT_LEN_OFFSET);
  uchar *buf= (uchar*) (*arg_buf);
  const char *error= 0;
  Log_event *res=  0;

  res= Log_event::read_log_event(buf, data_len, &error, description_event,
                                 true);

  if (!res)
  {
    DBUG_ASSERT(error != 0);
    sql_print_error("Error in Log_event::read_log_event(): "
                    "'%s', data_len: %d, event_type: %d",
                    error,data_len,head[EVENT_TYPE_OFFSET]);
  }
  (*arg_buf)+= data_len;
  (*arg_buf_len)-= data_len;
  DBUG_RETURN(res);
}

#include "transaction.h" // trans_commit(), trans_rollback()

void wsrep_set_apply_format(THD* thd, Format_description_log_event* ev)
{
  if (thd->wsrep_apply_format)
  {
    delete (Format_description_log_event*)thd->wsrep_apply_format;
  }
  thd->wsrep_apply_format= ev;
}

Format_description_log_event*
wsrep_get_apply_format(THD* thd)
{
  if (thd->wsrep_apply_format)
  {
    return (Format_description_log_event*) thd->wsrep_apply_format;
  }

  DBUG_ASSERT(thd->wsrep_rgi);

  return thd->wsrep_rgi->rli->relay_log.description_event_for_exec;
}

/* store error from rli */
static void wsrep_store_error_rli(const THD* const thd,
                                  wsrep::mutable_buffer& dst,
                                  bool const include_msg)
{
  Slave_reporting_capability* const rli= thd->wsrep_rgi->rli;
  if (rli && rli->last_error().number != 0)
  {
    auto error= rli->last_error();
    std::ostringstream os;
    if (include_msg)
    {
      os << error.message << ",";
    }
    os << " Error_code: " << error.number << ';';
    std::string const err_str= os.str();
    dst.resize(err_str.length() + 1);
    sprintf(dst.data(), "%s", err_str.c_str());

    WSREP_DEBUG("Error buffer (RLI) for thd %u seqno %lld, %zu bytes: '%s'",
                thd->thread_id, (long long)wsrep_thd_trx_seqno(thd),
                dst.size(), dst.size() ? dst.data() : "(null)");
  }
}

/* store error from diagnostic area */
static void wsrep_store_error_da(const THD* const thd,
                                 wsrep::mutable_buffer& dst,
                                 bool const include_msg)
{
  Diagnostics_area::Sql_condition_iterator it=
    thd->get_stmt_da()->sql_conditions();
  const Sql_condition* cond;

  static size_t const max_len= 2*MAX_SLAVE_ERRMSG; // 2x so that we have enough

  dst.resize(max_len);

  char* slider= dst.data();
  const char* const buf_end= slider + max_len - 1; // -1: leave space for \0

  for (cond= it++; cond && slider < buf_end; cond= it++)
  {
    uint const err_code= cond->get_sql_errno();
    const char* const err_str= cond->get_message_text();

    if (include_msg)
    {
      slider+= snprintf(slider, buf_end - slider, " %s, Error_code: %d;",
                        err_str, err_code);
    }
    else
    {
      slider+= snprintf(slider, buf_end - slider, " Error_code: %d;",
                        err_code);
    }
  }

  if (slider != dst.data())
  {
    *slider= '\0';
    slider++;
  }

  dst.resize(slider - dst.data());

  WSREP_DEBUG("Error buffer (DA) for thd %llu seqno %lld, %zu bytes: '%s'",
              thd->thread_id, (long long)wsrep_thd_trx_seqno(thd),
              dst.size(), dst.size() ? dst.data() : "(null)");
}

/* store error info after applying error */
void wsrep_store_error(const THD* const thd,
                       wsrep::mutable_buffer& dst,
                       bool const include_msg)
{
  dst.clear();
  wsrep_store_error_da(thd, dst, include_msg);
  if (dst.size() == 0)
  {
    wsrep_store_error_rli(thd, dst, include_msg);
  }
  if (dst.size() == 0)
  {
    WSREP_WARN("Failed to get apply error description from either "
               "Relay_log_info or Diagnostics_area, will use random data.");
    DBUG_ASSERT(0);
    uintptr_t const n1= reinterpret_cast<uintptr_t>(&dst);
    uintptr_t const n2= reinterpret_cast<uintptr_t>(thd);
    uintptr_t const data= n1 ^ (n2 < 1);
    const char* const data_ptr= reinterpret_cast<const char*>(&data);
    dst.push_back(data_ptr, data_ptr + sizeof(data));
  }
}

static int apply_events(THD*        thd,
                        Relay_log_info* rli,
                        const void* events_buf,
			size_t      buf_len,
                        const LEX_STRING &savepoint,
                        bool set_savepoint)
{
  char *buf= (char *)events_buf;
  int rcode= 0;
  int event= 1;
  Log_event_type typ;

  DBUG_ENTER("apply_events");
  if (!buf_len) WSREP_DEBUG("empty rbr buffer to apply: %lld",
                            (long long) wsrep_thd_trx_seqno(thd));

  thd->variables.gtid_seq_no= 0;
  if (wsrep_gtid_mode)
    thd->variables.gtid_domain_id= wsrep_gtid_server.domain_id;
  else
    thd->variables.gtid_domain_id= global_system_variables.gtid_domain_id;

  bool in_trans = thd->in_active_multi_stmt_transaction();
  if (in_trans && set_savepoint) {
    if (wsrep_applier_retry_count > 0 && !thd->wsrep_trx().is_streaming() &&
        trans_savepoint(thd, savepoint)) {
      rcode = 1;
      goto error;
    }
  }

  while (buf_len)
  {
    int exec_res;
    Log_event* ev= wsrep_read_log_event(&buf, &buf_len,
                                          wsrep_get_apply_format(thd));
    if (!ev)
    {
      WSREP_ERROR("applier could not read binlog event, seqno: %lld, len: %zu",
                  (long long)wsrep_thd_trx_seqno(thd), buf_len);
      rcode= WSREP_ERR_BAD_EVENT;
      goto error;
    }

    typ= ev->get_type_code();

    switch (typ) {
    case FORMAT_DESCRIPTION_EVENT:
      wsrep_set_apply_format(thd, (Format_description_log_event*)ev);
      continue;
    case GTID_EVENT:
      {
        Gtid_log_event *gtid_ev= (Gtid_log_event*)ev;
        thd->variables.server_id= gtid_ev->server_id;
        thd->variables.gtid_domain_id= gtid_ev->domain_id;
        if ((gtid_ev->server_id == wsrep_gtid_server.server_id) &&
            (gtid_ev->domain_id == wsrep_gtid_server.domain_id))
        {
          thd->variables.wsrep_gtid_seq_no= gtid_ev->seq_no;
        }
        else
        {
          thd->variables.gtid_seq_no= gtid_ev->seq_no;
        }

        if (wsrep_gtid_mode)
          wsrep_schema->store_gtid_event(thd, gtid_ev);

        delete ev;
      }
      continue;
    default:
      break;
    }

    if (!thd->variables.gtid_seq_no && wsrep_thd_is_toi(thd) &&
        (ev->get_type_code() == QUERY_EVENT))
    { 
      uint64 seqno= wsrep_gtid_server.seqno_inc();
      thd->wsrep_current_gtid_seqno= seqno;
      if (mysql_bin_log.is_open() && wsrep_gtid_mode)
      { 
        thd->variables.gtid_seq_no= seqno;
      }
    }

    if (LOG_EVENT_IS_WRITE_ROW(typ) ||
        LOG_EVENT_IS_UPDATE_ROW(typ) ||
        LOG_EVENT_IS_DELETE_ROW(typ))
    {
      Rows_log_event* rle = static_cast<Rows_log_event*>(ev);
      if (thd_test_options(thd, OPTION_RELAXED_UNIQUE_CHECKS))
      {
        rle->set_flags(Rows_log_event::RELAXED_UNIQUE_CHECKS_F);
      }
      if (thd_test_options(thd, OPTION_NO_FOREIGN_KEY_CHECKS))
      {
        rle->set_flags(Rows_log_event::NO_FOREIGN_KEY_CHECKS_F);
      }
    }

    /* Use the original server id for logging. */
    thd->set_server_id(ev->server_id);
    thd->lex->current_select= 0;
    thd->variables.option_bits=
      (thd->variables.option_bits & ~OPTION_SKIP_REPLICATION) |
      (ev->flags & LOG_EVENT_SKIP_REPLICATION_F ?  OPTION_SKIP_REPLICATION : 0);

    if (ev->get_type_code() == GTID_EVENT)
    {
      thd->variables.option_bits &= ~OPTION_GTID_BEGIN;
    }

    ev->thd= thd;
    thd->set_time();

    if (!ev->when)
    {
      my_hrtime_t hrtime= my_hrtime();
      ev->when= hrtime_to_my_time(hrtime);
      ev->when_sec_part= hrtime_sec_part(hrtime);
    }

    exec_res= ev->apply_event(thd->wsrep_rgi);

    DBUG_PRINT("info", ("exec_event result: %d", exec_res));

    if (exec_res)
    {
      WSREP_WARN("Event %d %s apply failed: %d, seqno %lld",
                 event, ev->get_type_str(), exec_res,
                 (long long) wsrep_thd_trx_seqno(thd));
      rcode= exec_res;
      /* stop processing for the first error */
      delete ev;
      goto error;
    }

    /* Transaction was started by the event, set the savepoint to rollback to
     * in case of failure. */
    if (!in_trans && thd->in_active_multi_stmt_transaction()) {
      in_trans = true;
      if (wsrep_applier_retry_count > 0 && !thd->wsrep_trx().is_streaming()
          && set_savepoint && trans_savepoint(thd, savepoint)) {
        delete ev;
        rcode = 1;
        goto error;
      }
    }

    event++;

    delete_or_keep_event_post_apply(thd->wsrep_rgi, typ, ev);
  }

error:
  if (thd->killed == KILL_CONNECTION)
    WSREP_INFO("applier aborted: %lld", (long long)wsrep_thd_trx_seqno(thd));

  wsrep_set_apply_format(thd, NULL);

  DBUG_RETURN(rcode);
}

int wsrep_apply_events(THD*                       thd,
                        Relay_log_info*            rli,
                        const wsrep::const_buffer& data,
                        wsrep::mutable_buffer&     err,
                        bool const                 include_msg)
{
  static char savepoint_name[20] = "wsrep_retry";
  static size_t savepoint_name_len = strlen(savepoint_name);
  static const LEX_STRING savepoint= { savepoint_name, savepoint_name_len };
  uint n_retries = 0;
  bool savepoint_exists = false;

  int ret= apply_events(thd, rli, data.data(), data.size(), savepoint, true);

  while (ret && n_retries < wsrep_applier_retry_count &&
	 (savepoint_exists = trans_savepoint_exists(thd, savepoint))) {
    /* applying failed, retry applying events */

    /* rollback to savepoint without telling Wsrep-lib */
    thd->variables.wsrep_on = false;
    thd->wsrep_applier_in_rollback= true;
    if (FALSE != trans_rollback_to_savepoint(thd, savepoint)) {
      thd->variables.wsrep_on = true;
      thd->wsrep_applier_in_rollback= false;
      break;
    }
    thd->variables.wsrep_on = true;
    thd->wsrep_applier_in_rollback= false;

    /* reset THD object for retry */
    thd->clear_error();
    thd->reset_for_next_command();

    /* retry applying events */
    ret= apply_events(thd, rli, data.data(), data.size(), savepoint, false);
    n_retries++;
  }

  if (savepoint_exists) {
    trans_release_savepoint(thd, savepoint);
  }

  if (ret || wsrep_thd_has_ignored_error(thd))
  {
    if (ret) {
      wsrep_store_error(thd, err, include_msg);
    }
    wsrep_dump_rbr_buf_with_header(thd, data.data(), data.size());
  }

  return ret;
}
