/* Copyright (C) 2015 Codership Oy <info@codership.com>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License along
   with this program; if not, write to the Free Software Foundation, Inc.,
   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA. */


#include "my_global.h"
#include "wsrep_api.h"
#include "wsrep_mysqld.h"
#include "wsrep_thd_pool.h"
#include "wsrep_utils.h"
#include "sql_class.h"

#include <list>

static THD* wsrep_thd_pool_new_thd()
{
  THD *thd= new THD(next_thread_id());
  thd->thread_stack= (char*) &thd;
  thd->security_ctx->skip_grants();
  thd->system_thread= SYSTEM_THREAD_GENERIC;

  mysql_mutex_lock(&LOCK_thread_count);

  thd->real_id=pthread_self(); // Keep purify happy

  WSREP_DEBUG("Wsrep_thd_pool: creating system thread: %lld",
              (long long)thd->thread_id);
  thd->prior_thr_create_utime= thd->start_utime= thd->thr_create_utime;
  (void) mysql_mutex_unlock(&LOCK_thread_count);

  /* */
  thd->variables.wsrep_on    = 0;
  /* No binlogging */
  thd->variables.sql_log_bin = 0;
  thd->variables.option_bits &= ~OPTION_BIN_LOG;
  /* No general log */
  thd->variables.option_bits |= OPTION_LOG_OFF;
  /* Read committed isolation to avoid gap locking */
  thd->variables.tx_isolation= ISO_READ_COMMITTED;

  return thd;
}

Wsrep_thd_pool::Wsrep_thd_pool(size_t threads)
  :
  threads_(threads),
  pool_()
{
  WSREP_DEBUG("Wsrep_thd_pool constructor");
  wsp::auto_lock lock(&LOCK_wsrep_thd_pool);
  pool_.reserve(threads);
  for (size_t i= 0; i < threads; ++i)
  {
    pool_.push_back(wsrep_thd_pool_new_thd());
  }
}

Wsrep_thd_pool::~Wsrep_thd_pool()
{
  wsp::auto_lock lock(&LOCK_wsrep_thd_pool);
  while (!pool_.empty())
  {
    THD *thd= pool_.back();
    WSREP_DEBUG("Wsrep_thd_pool: closing thread %lld",
                (long long)thd->thread_id);

     delete thd;

    pool_.pop_back();
  }
}

THD* Wsrep_thd_pool::get_thd(THD* thd)
{
  wsp::auto_lock lock(&LOCK_wsrep_thd_pool);
  THD *ret= NULL;
  if (pool_.empty())
  {
    ret= wsrep_thd_pool_new_thd();
  }
  else
  {
    ret= pool_.back();
    pool_.pop_back();
  }
  if (thd)
  {
    ret->thread_stack= thd->thread_stack;
  }
  else
  {
    ret->thread_stack= (char*) &ret;
  }
  ret->store_globals();
  return ret;
}

void Wsrep_thd_pool::release_thd(THD* thd)
{
  DBUG_ASSERT(!thd->mdl_context.has_locks());
  DBUG_ASSERT(!thd->open_tables);
  DBUG_ASSERT(thd->transaction.stmt.is_empty());
  wsp::auto_lock lock(&LOCK_wsrep_thd_pool);
  if (pool_.size() < threads_)
  {
    pool_.push_back(thd);
  }
  else
  {
    delete thd;
  }
}

