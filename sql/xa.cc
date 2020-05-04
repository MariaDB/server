/*
   Copyright (c) 2000, 2016, Oracle and/or its affiliates.
   Copyright (c) 2009, 2020, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
*/

#include "mariadb.h"
#include "sql_class.h"
#include "transaction.h"
#include "my_cpu.h"
#include <pfs_transaction_provider.h>
#include <mysql/psi/mysql_transaction.h>

static bool slave_applier_reset_xa_trans(THD *thd);

/***************************************************************************
  Handling of XA id caching
***************************************************************************/
struct XID_cache_insert_element
{
  enum xa_states xa_state;
  XID *xid;
  XID_cache_element *xid_cache_element;

  XID_cache_insert_element(enum xa_states xa_state_arg, XID *xid_arg):
    xa_state(xa_state_arg), xid(xid_arg) {}
};


class XID_cache_element
{
  /*
    m_state is used to prevent elements from being deleted while XA RECOVER
    iterates xid cache and to prevent recovered elments from being acquired by
    multiple threads.

    bits 1..29 are reference counter
    bit 30 is RECOVERED flag
    bit 31 is ACQUIRED flag (thread owns this xid)
    bit 32 is unused

    Newly allocated and deleted elements have m_state set to 0.

    On lock() m_state is atomically incremented. It also creates load-ACQUIRE
    memory barrier to make sure m_state is actually updated before furhter
    memory accesses. Attempting to lock an element that has neither ACQUIRED
    nor RECOVERED flag set returns failure and further accesses to element
    memory are forbidden.

    On unlock() m_state is decremented. It also creates store-RELEASE memory
    barrier to make sure m_state is actually updated after preceding memory
    accesses.

    ACQUIRED flag is set when thread registers it's xid or when thread acquires
    recovered xid.

    RECOVERED flag is set for elements found during crash recovery.

    ACQUIRED and RECOVERED flags are cleared before element is deleted from
    hash in a spin loop, after last reference is released.
  */
  std::atomic<int32_t> m_state;
public:
  static const int32 ACQUIRED= 1 << 30;
  static const int32 RECOVERED= 1 << 29;
  /* Error reported by the Resource Manager (RM) to the Transaction Manager. */
  uint rm_error;
  enum xa_states xa_state;
  XID xid;
  bool is_set(int32_t flag)
  { return m_state.load(std::memory_order_relaxed) & flag; }
  void set(int32_t flag)
  {
    DBUG_ASSERT(!is_set(ACQUIRED | RECOVERED));
    m_state.fetch_add(flag, std::memory_order_relaxed);
  }
  bool lock()
  {
    int32_t old= m_state.fetch_add(1, std::memory_order_acquire);
    if (old & (ACQUIRED | RECOVERED))
      return true;
    unlock();
    return false;
  }
  void unlock()
  { m_state.fetch_sub(1, std::memory_order_release); }
  void mark_uninitialized()
  {
    int32_t old= ACQUIRED;
    while (!m_state.compare_exchange_weak(old, 0,
                                          std::memory_order_relaxed,
                                          std::memory_order_relaxed))
    {
      old&= ACQUIRED | RECOVERED;
      (void) LF_BACKOFF();
    }
  }
  void acquired_to_recovered()
  {
    m_state.fetch_or(RECOVERED, std::memory_order_relaxed);
    m_state.fetch_and(~ACQUIRED, std::memory_order_release);
  }
  bool acquire_recovered()
  {
    int32_t old= RECOVERED;
    while (!m_state.compare_exchange_weak(old, ACQUIRED | RECOVERED,
                                          std::memory_order_acquire,
                                          std::memory_order_relaxed))
    {
      if (!(old & RECOVERED) || (old & ACQUIRED))
        return false;
      old= RECOVERED;
      (void) LF_BACKOFF();
    }
    return true;
  }
  static void lf_hash_initializer(LF_HASH *hash __attribute__((unused)),
                                  XID_cache_element *element,
                                  XID_cache_insert_element *new_element)
  {
    DBUG_ASSERT(!element->is_set(ACQUIRED | RECOVERED));
    element->rm_error= 0;
    element->xa_state= new_element->xa_state;
    element->xid.set(new_element->xid);
    new_element->xid_cache_element= element;
  }
  static void lf_alloc_constructor(uchar *ptr)
  {
    XID_cache_element *element= (XID_cache_element*) (ptr + LF_HASH_OVERHEAD);
    element->m_state= 0;
  }
  static void lf_alloc_destructor(uchar *ptr)
  {
    DBUG_ASSERT(!reinterpret_cast<XID_cache_element*>(ptr + LF_HASH_OVERHEAD)
		->is_set(ACQUIRED));
  }
  static uchar *key(const XID_cache_element *element, size_t *length,
                    my_bool not_used __attribute__((unused)))
  {
    *length= element->xid.key_length();
    return element->xid.key();
  }
};


static LF_HASH xid_cache;
static bool xid_cache_inited;


enum xa_states XID_STATE::get_state_code() const
{
  return xid_cache_element ? xid_cache_element->xa_state : XA_NO_STATE;
}


bool THD::fix_xid_hash_pins()
{
  if (!xid_hash_pins)
    xid_hash_pins= lf_hash_get_pins(&xid_cache);
  return !xid_hash_pins;
}


void XID_STATE::set_error(uint error)
{
  if (is_explicit_XA())
    xid_cache_element->rm_error= error;
}


void XID_STATE::er_xaer_rmfail() const
{
  static const char *xa_state_names[]=
    { "ACTIVE", "IDLE", "PREPARED", "ROLLBACK ONLY", "NON-EXISTING"};
  my_error(ER_XAER_RMFAIL, MYF(0), xa_state_names[get_state_code()]);
}


/**
  Check that XA transaction has an uncommitted work. Report an error
  to the user in case when there is an uncommitted work for XA transaction.

  @return  result of check
    @retval  false  XA transaction is NOT in state IDLE, PREPARED
                    or ROLLBACK_ONLY.
    @retval  true   XA transaction is in state IDLE or PREPARED
                    or ROLLBACK_ONLY.
*/

bool XID_STATE::check_has_uncommitted_xa() const
{
  if (is_explicit_XA() && xid_cache_element->xa_state != XA_ACTIVE)
  {
    er_xaer_rmfail();
    return true;
  }
  return false;
}


XID *XID_STATE::get_xid() const
{
  DBUG_ASSERT(is_explicit_XA());
  return &xid_cache_element->xid;
}


void xid_cache_init()
{
  xid_cache_inited= true;
  lf_hash_init(&xid_cache, sizeof(XID_cache_element), LF_HASH_UNIQUE, 0, 0,
               (my_hash_get_key) XID_cache_element::key, &my_charset_bin);
  xid_cache.alloc.constructor= XID_cache_element::lf_alloc_constructor;
  xid_cache.alloc.destructor= XID_cache_element::lf_alloc_destructor;
  xid_cache.initializer=
    (lf_hash_initializer) XID_cache_element::lf_hash_initializer;
}


void xid_cache_free()
{
  if (xid_cache_inited)
  {
    lf_hash_destroy(&xid_cache);
    xid_cache_inited= false;
  }
}


/**
  Find recovered XA transaction by XID.
*/

static XID_cache_element *xid_cache_search(THD *thd, XID *xid)
{
  DBUG_ASSERT(thd->xid_hash_pins);
  XID_cache_element *element=
    (XID_cache_element*) lf_hash_search(&xid_cache, thd->xid_hash_pins,
                                        xid->key(), xid->key_length());
  if (element)
  {
    if (!element->acquire_recovered())
      element= 0;
    lf_hash_search_unpin(thd->xid_hash_pins);
    DEBUG_SYNC(thd, "xa_after_search");
  }
  return element;
}


bool xid_cache_insert(XID *xid)
{
  XID_cache_insert_element new_element(XA_PREPARED, xid);
  LF_PINS *pins;

  if (!(pins= lf_hash_get_pins(&xid_cache)))
    return true;

  int res= lf_hash_insert(&xid_cache, pins, &new_element);
  switch (res)
  {
  case 0:
    new_element.xid_cache_element->set(XID_cache_element::RECOVERED);
    break;
  case 1:
    res= 0;
  }
  lf_hash_put_pins(pins);
  return res;
}


bool xid_cache_insert(THD *thd, XID_STATE *xid_state, XID *xid)
{
  XID_cache_insert_element new_element(XA_ACTIVE, xid);

  if (thd->fix_xid_hash_pins())
    return true;

  int res= lf_hash_insert(&xid_cache, thd->xid_hash_pins, &new_element);
  switch (res)
  {
  case 0:
    xid_state->xid_cache_element= new_element.xid_cache_element;
    xid_state->xid_cache_element->set(XID_cache_element::ACQUIRED);
    break;
  case 1:
    my_error(ER_XAER_DUPID, MYF(0));
  }
  return res;
}


static void xid_cache_delete(THD *thd, XID_cache_element *&element)
{
  DBUG_ASSERT(thd->xid_hash_pins);
  element->mark_uninitialized();
  lf_hash_delete(&xid_cache, thd->xid_hash_pins,
                 element->xid.key(), element->xid.key_length());
}


void xid_cache_delete(THD *thd, XID_STATE *xid_state)
{
  DBUG_ASSERT(xid_state->is_explicit_XA());
  xid_cache_delete(thd, xid_state->xid_cache_element);
  xid_state->xid_cache_element= 0;
}


struct xid_cache_iterate_arg
{
  my_hash_walk_action action;
  void *argument;
};

static my_bool xid_cache_iterate_callback(XID_cache_element *element,
                                          xid_cache_iterate_arg *arg)
{
  my_bool res= FALSE;
  if (element->lock())
  {
    res= arg->action(element, arg->argument);
    element->unlock();
  }
  return res;
}

static int xid_cache_iterate(THD *thd, my_hash_walk_action action, void *arg)
{
  xid_cache_iterate_arg argument= { action, arg };
  return thd->fix_xid_hash_pins() ? -1 :
         lf_hash_iterate(&xid_cache, thd->xid_hash_pins,
                         (my_hash_walk_action) xid_cache_iterate_callback,
                         &argument);
}


/**
  Mark a XA transaction as rollback-only if the RM unilaterally
  rolled back the transaction branch.

  @note If a rollback was requested by the RM, this function sets
        the appropriate rollback error code and transits the state
        to XA_ROLLBACK_ONLY.

  @return TRUE if transaction was rolled back or if the transaction
          state is XA_ROLLBACK_ONLY. FALSE otherwise.
*/
static bool xa_trans_rolled_back(XID_cache_element *element)
{
  if (element->rm_error)
  {
    switch (element->rm_error) {
    case ER_LOCK_WAIT_TIMEOUT:
      my_error(ER_XA_RBTIMEOUT, MYF(0));
      break;
    case ER_LOCK_DEADLOCK:
      my_error(ER_XA_RBDEADLOCK, MYF(0));
      break;
    default:
      my_error(ER_XA_RBROLLBACK, MYF(0));
    }
    element->xa_state= XA_ROLLBACK_ONLY;
  }

  return element->xa_state == XA_ROLLBACK_ONLY;
}


/**
  Rollback the active XA transaction.

  @return TRUE if the rollback failed, FALSE otherwise.
*/

bool xa_trans_force_rollback(THD *thd)
{
  bool rc= false;

  if (ha_rollback_trans(thd, true))
  {
    my_error(ER_XAER_RMERR, MYF(0));
    rc= true;
  }
  thd->variables.option_bits&=
    ~(OPTION_BEGIN | OPTION_KEEP_LOG | OPTION_GTID_BEGIN);
  thd->transaction->all.reset();
  thd->server_status&=
    ~(SERVER_STATUS_IN_TRANS | SERVER_STATUS_IN_TRANS_READONLY);
  DBUG_PRINT("info", ("clearing SERVER_STATUS_IN_TRANS"));
  xid_cache_delete(thd, &thd->transaction->xid_state);

  trans_track_end_trx(thd);

  return rc;
}


/**
  Starts an XA transaction with the given xid value.

  @param thd    Current thread

  @retval FALSE  Success
  @retval TRUE   Failure
*/

bool trans_xa_start(THD *thd)
{
  DBUG_ENTER("trans_xa_start");

  if (thd->transaction->xid_state.is_explicit_XA() &&
      thd->transaction->xid_state.xid_cache_element->xa_state == XA_IDLE &&
      thd->lex->xa_opt == XA_RESUME)
  {
    bool not_equal=
      !thd->transaction->xid_state.xid_cache_element->xid.eq(thd->lex->xid);
    if (not_equal)
      my_error(ER_XAER_NOTA, MYF(0));
    else
    {
      thd->transaction->xid_state.xid_cache_element->xa_state= XA_ACTIVE;
      MYSQL_SET_TRANSACTION_XA_STATE(thd->m_transaction_psi, XA_ACTIVE);
    }
    DBUG_RETURN(not_equal);
  }

  /* TODO: JOIN is not supported yet. */
  if (thd->lex->xa_opt != XA_NONE)
    my_error(ER_XAER_INVAL, MYF(0));
  else if (!thd->lex->xid->gtrid_length)
    my_error(ER_XAER_INVAL, MYF(0));
  else if (thd->transaction->xid_state.is_explicit_XA())
    thd->transaction->xid_state.er_xaer_rmfail();
  else if (thd->locked_tables_mode || thd->in_active_multi_stmt_transaction())
    my_error(ER_XAER_OUTSIDE, MYF(0));
  else if (!trans_begin(thd))
  {
    MYSQL_SET_TRANSACTION_XID(thd->m_transaction_psi, thd->lex->xid, XA_ACTIVE);
    if (xid_cache_insert(thd, &thd->transaction->xid_state, thd->lex->xid))
    {
      trans_rollback(thd);
      DBUG_RETURN(true);
    }
    DBUG_RETURN(FALSE);
  }

  DBUG_RETURN(TRUE);
}


/**
  Put a XA transaction in the IDLE state.

  @param thd    Current thread

  @retval FALSE  Success
  @retval TRUE   Failure
*/

bool trans_xa_end(THD *thd)
{
  DBUG_ENTER("trans_xa_end");

  /* TODO: SUSPEND and FOR MIGRATE are not supported yet. */
  if (thd->lex->xa_opt != XA_NONE)
    my_error(ER_XAER_INVAL, MYF(0));
  else if (!thd->transaction->xid_state.is_explicit_XA() ||
           thd->transaction->xid_state.xid_cache_element->xa_state != XA_ACTIVE)
    thd->transaction->xid_state.er_xaer_rmfail();
  else if (!thd->transaction->xid_state.xid_cache_element->xid.eq(thd->lex->xid))
    my_error(ER_XAER_NOTA, MYF(0));
  else if (!xa_trans_rolled_back(thd->transaction->xid_state.xid_cache_element))
  {
    thd->transaction->xid_state.xid_cache_element->xa_state= XA_IDLE;
    MYSQL_SET_TRANSACTION_XA_STATE(thd->m_transaction_psi, XA_IDLE);
  }

  DBUG_RETURN(thd->is_error() ||
    thd->transaction->xid_state.xid_cache_element->xa_state != XA_IDLE);
}


/**
  Put a XA transaction in the PREPARED state.

  @param thd    Current thread

  @retval FALSE  Success
  @retval TRUE   Failure
*/

bool trans_xa_prepare(THD *thd)
{
  int res= 1;

  DBUG_ENTER("trans_xa_prepare");

  if (!thd->transaction->xid_state.is_explicit_XA() ||
      thd->transaction->xid_state.xid_cache_element->xa_state != XA_IDLE)
    thd->transaction->xid_state.er_xaer_rmfail();
  else if (!thd->transaction->xid_state.xid_cache_element->xid.eq(thd->lex->xid))
    my_error(ER_XAER_NOTA, MYF(0));
  else
  {
    /*
      Acquire metadata lock which will ensure that COMMIT is blocked
      by active FLUSH TABLES WITH READ LOCK (and vice versa COMMIT in
      progress blocks FTWRL).

      We allow FLUSHer to COMMIT; we assume FLUSHer knows what it does.
    */
    MDL_request mdl_request;
    MDL_REQUEST_INIT(&mdl_request, MDL_key::BACKUP, "", "", MDL_BACKUP_COMMIT,
                     MDL_STATEMENT);
    if (thd->mdl_context.acquire_lock(&mdl_request,
                                      thd->variables.lock_wait_timeout) ||
        ha_prepare(thd))
    {
      if (!mdl_request.ticket)
        ha_rollback_trans(thd, TRUE);
      thd->variables.option_bits&= ~(OPTION_BEGIN | OPTION_KEEP_LOG);
      thd->transaction->all.reset();
      thd->server_status&=
        ~(SERVER_STATUS_IN_TRANS | SERVER_STATUS_IN_TRANS_READONLY);
      xid_cache_delete(thd, &thd->transaction->xid_state);
      my_error(ER_XA_RBROLLBACK, MYF(0));
    }
    else
    {
      thd->transaction->xid_state.xid_cache_element->xa_state= XA_PREPARED;
      MYSQL_SET_TRANSACTION_XA_STATE(thd->m_transaction_psi, XA_PREPARED);
      res= thd->variables.pseudo_slave_mode || thd->slave_thread ?
        slave_applier_reset_xa_trans(thd) : 0;
    }
  }

  DBUG_RETURN(res);
}


/**
  Commit and terminate the a XA transaction.

  @param thd    Current thread

  @retval FALSE  Success
  @retval TRUE   Failure
*/

bool trans_xa_commit(THD *thd)
{
  bool res= true;
  XID_STATE &xid_state= thd->transaction->xid_state;

  DBUG_ENTER("trans_xa_commit");

  if (!xid_state.is_explicit_XA() ||
      !xid_state.xid_cache_element->xid.eq(thd->lex->xid))
  {
    if (thd->in_multi_stmt_transaction_mode())
    {
      /*
        Not allow to commit from inside an not-"native" to xid
        ongoing transaction: the commit effect can't be reversed.
      */
      my_error(ER_XAER_OUTSIDE, MYF(0));
      DBUG_RETURN(TRUE);
    }
    if (thd->lex->xa_opt != XA_NONE)
    {
      /*
        Not allow to commit with one phase a prepared xa out of compatibility
        with the native commit branch's error out.
      */
      my_error(ER_XAER_INVAL, MYF(0));
      DBUG_RETURN(TRUE);
    }
    if (thd->fix_xid_hash_pins())
    {
      my_error(ER_OUT_OF_RESOURCES, MYF(0));
      DBUG_RETURN(TRUE);
    }

    if (auto xs= xid_cache_search(thd, thd->lex->xid))
    {
      res= xa_trans_rolled_back(xs);
      /*
        Acquire metadata lock which will ensure that COMMIT is blocked
        by active FLUSH TABLES WITH READ LOCK (and vice versa COMMIT in
        progress blocks FTWRL).

        We allow FLUSHer to COMMIT; we assume FLUSHer knows what it does.
      */
      MDL_request mdl_request;
      MDL_REQUEST_INIT(&mdl_request, MDL_key::BACKUP, "", "", MDL_BACKUP_COMMIT,
                       MDL_STATEMENT);
      if (thd->mdl_context.acquire_lock(&mdl_request,
                                        thd->variables.lock_wait_timeout))
      {
        /*
          We can't rollback an XA transaction on lock failure due to
          Innodb redo log and bin log update is involved in rollback.
          Return error to user for a retry.
        */
        DBUG_ASSERT(thd->is_error());

        xs->acquired_to_recovered();
        DBUG_RETURN(true);
      }
      DBUG_ASSERT(!xid_state.xid_cache_element);

      if (thd->wait_for_prior_commit())
      {
        DBUG_ASSERT(thd->is_error());

        xs->acquired_to_recovered();
        DBUG_RETURN(true);
      }

      xid_state.xid_cache_element= xs;
      ha_commit_or_rollback_by_xid(thd->lex->xid, !res);
      xid_state.xid_cache_element= 0;

      res= res || thd->is_error();
      xid_cache_delete(thd, xs);
    }
    else
      my_error(ER_XAER_NOTA, MYF(0));
    DBUG_RETURN(res);
  }

  if (xa_trans_rolled_back(xid_state.xid_cache_element))
  {
    xa_trans_force_rollback(thd);
    DBUG_RETURN(thd->is_error());
  }
  else if (xid_state.xid_cache_element->xa_state == XA_IDLE &&
           thd->lex->xa_opt == XA_ONE_PHASE)
  {
    int r= ha_commit_trans(thd, TRUE);
    if ((res= MY_TEST(r)))
      my_error(r == 1 ? ER_XA_RBROLLBACK : ER_XAER_RMERR, MYF(0));
  }
  else if (thd->transaction->xid_state.xid_cache_element->xa_state == XA_PREPARED)
  {
    MDL_request mdl_request;
    if (thd->lex->xa_opt != XA_NONE)
    {
      my_error(ER_XAER_INVAL, MYF(0));
      DBUG_RETURN(TRUE);
    }

    /*
      Acquire metadata lock which will ensure that COMMIT is blocked
      by active FLUSH TABLES WITH READ LOCK (and vice versa COMMIT in
      progress blocks FTWRL).

      We allow FLUSHer to COMMIT; we assume FLUSHer knows what it does.
    */
    MDL_REQUEST_INIT(&mdl_request, MDL_key::BACKUP, "", "", MDL_BACKUP_COMMIT,
                     MDL_TRANSACTION);

    if (thd->mdl_context.acquire_lock(&mdl_request,
                                      thd->variables.lock_wait_timeout))
    {
      /*
        We can't rollback an XA transaction on lock failure due to
        Innodb redo log and bin log update is involved in rollback.
        Return error to user for a retry.
      */
      my_error(ER_XAER_RMERR, MYF(0));
      DBUG_RETURN(true);
    }
    else
    {
      DEBUG_SYNC(thd, "trans_xa_commit_after_acquire_commit_lock");

      if ((res= MY_TEST(ha_commit_one_phase(thd, 1))))
        my_error(ER_XAER_RMERR, MYF(0));
      else
      {
        /*
          Since we don't call ha_commit_trans() for prepared transactions,
          we need to explicitly mark the transaction as committed.
        */
        MYSQL_COMMIT_TRANSACTION(thd->m_transaction_psi);
      }

      thd->m_transaction_psi= NULL;
    }
  }
  else
  {
    xid_state.er_xaer_rmfail();
    DBUG_RETURN(TRUE);
  }

  thd->variables.option_bits&= ~(OPTION_BEGIN | OPTION_KEEP_LOG);
  thd->transaction->all.reset();
  thd->server_status&=
    ~(SERVER_STATUS_IN_TRANS | SERVER_STATUS_IN_TRANS_READONLY);
  DBUG_PRINT("info", ("clearing SERVER_STATUS_IN_TRANS"));
  xid_cache_delete(thd, &xid_state);

  trans_track_end_trx(thd);
  /* The transaction should be marked as complete in P_S. */
  DBUG_ASSERT(thd->m_transaction_psi == NULL || res);
  DBUG_RETURN(res);
}


/**
  Roll back and terminate a XA transaction.

  @param thd    Current thread

  @retval FALSE  Success
  @retval TRUE   Failure
*/

bool trans_xa_rollback(THD *thd)
{
  XID_STATE &xid_state= thd->transaction->xid_state;

  DBUG_ENTER("trans_xa_rollback");

  if (!xid_state.is_explicit_XA() ||
      !xid_state.xid_cache_element->xid.eq(thd->lex->xid))
  {
    if (thd->in_multi_stmt_transaction_mode())
    {
      my_error(ER_XAER_OUTSIDE, MYF(0));
      DBUG_RETURN(TRUE);
    }
    if (thd->fix_xid_hash_pins())
    {
      my_error(ER_OUT_OF_RESOURCES, MYF(0));
      DBUG_RETURN(TRUE);
    }

    if (auto xs= xid_cache_search(thd, thd->lex->xid))
    {
      MDL_request mdl_request;
      MDL_REQUEST_INIT(&mdl_request, MDL_key::BACKUP, "", "", MDL_BACKUP_COMMIT,
                       MDL_STATEMENT);
      if (thd->mdl_context.acquire_lock(&mdl_request,
                                        thd->variables.lock_wait_timeout))
      {
        /*
          We can't rollback an XA transaction on lock failure due to
          Innodb redo log and bin log update is involved in rollback.
          Return error to user for a retry.
        */
        DBUG_ASSERT(thd->is_error());

        xs->acquired_to_recovered();
        DBUG_RETURN(true);
      }
      xa_trans_rolled_back(xs);
      DBUG_ASSERT(!xid_state.xid_cache_element);

      if (thd->wait_for_prior_commit())
      {
        DBUG_ASSERT(thd->is_error());
        xs->acquired_to_recovered();
        DBUG_RETURN(true);
      }

      xid_state.xid_cache_element= xs;
      ha_commit_or_rollback_by_xid(thd->lex->xid, 0);
      xid_state.xid_cache_element= 0;
      xid_cache_delete(thd, xs);
    }
    else
      my_error(ER_XAER_NOTA, MYF(0));
    DBUG_RETURN(thd->get_stmt_da()->is_error());
  }

  if (xid_state.xid_cache_element->xa_state == XA_ACTIVE)
  {
    xid_state.er_xaer_rmfail();
    DBUG_RETURN(TRUE);
  }

  MDL_request mdl_request;
  MDL_REQUEST_INIT(&mdl_request, MDL_key::BACKUP, "", "", MDL_BACKUP_COMMIT,
      MDL_STATEMENT);
  if (thd->mdl_context.acquire_lock(&mdl_request,
        thd->variables.lock_wait_timeout))
  {
    /*
      We can't rollback an XA transaction on lock failure due to
      Innodb redo log and bin log update is involved in rollback.
      Return error to user for a retry.
    */
    my_error(ER_XAER_RMERR, MYF(0));
    DBUG_RETURN(true);
  }

  DBUG_RETURN(xa_trans_force_rollback(thd));
}


bool trans_xa_detach(THD *thd)
{
  DBUG_ASSERT(thd->transaction->xid_state.is_explicit_XA());

  if (thd->transaction->xid_state.xid_cache_element->xa_state != XA_PREPARED)
    return xa_trans_force_rollback(thd);
  else if (!thd->transaction->all.is_trx_read_write())
  {
    thd->transaction->xid_state.set_error(ER_XA_RBROLLBACK);
    ha_rollback_trans(thd, true);
  }

  thd->transaction->xid_state.xid_cache_element->acquired_to_recovered();
  thd->transaction->xid_state.xid_cache_element= 0;
  thd->transaction->cleanup();

  Ha_trx_info *ha_info, *ha_info_next;
  for (ha_info= thd->transaction->all.ha_list;
       ha_info;
       ha_info= ha_info_next)
  {
    ha_info_next= ha_info->next();
    ha_info->reset(); /* keep it conveniently zero-filled */
  }

  thd->transaction->all.ha_list= 0;
  thd->transaction->all.no_2pc= 0;
  return false;
}


/**
  return the XID as it appears in the SQL function's arguments.
  So this string can be passed to XA START, XA PREPARE etc...

  @note
    the 'buf' has to have space for at least SQL_XIDSIZE bytes.
*/


/*
  'a'..'z' 'A'..'Z', '0'..'9'
  and '-' '_' ' ' symbols don't have to be
  converted.
*/

static const char xid_needs_conv[128]=
{
  1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
  1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
  0,1,1,1,1,1,1,1,1,1,1,1,1,0,1,1,
  0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,
  1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,0,
  1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,1,1,1,1,1
};

/*
  The size of XID string representation in the form
  'gtrid', 'bqual', formatID
  see xid_t::get_sql_string() for details.
*/
#define SQL_XIDSIZE (XIDDATASIZE * 2 + 8 + MY_INT64_NUM_DECIMAL_DIGITS)

/* The 'buf' has to have space for at least SQL_XIDSIZE bytes. */
static uint get_sql_xid(XID *xid, char *buf)
{
  int tot_len= xid->gtrid_length + xid->bqual_length;
  int i;
  const char *orig_buf= buf;

  for (i=0; i<tot_len; i++)
  {
    uchar c= ((uchar *) xid->data)[i];
    if (c >= 128 || xid_needs_conv[c])
      break;
  }

  if (i >= tot_len)
  {
    /* No need to convert characters to hexadecimals. */
    *buf++= '\'';
    memcpy(buf, xid->data, xid->gtrid_length);
    buf+= xid->gtrid_length;
    *buf++= '\'';
    if (xid->bqual_length > 0 || xid->formatID != 1)
    {
      *buf++= ',';
      *buf++= '\'';
      memcpy(buf, xid->data+xid->gtrid_length, xid->bqual_length);
      buf+= xid->bqual_length;
      *buf++= '\'';
    }
  }
  else
  {
    *buf++= 'X';
    *buf++= '\'';
    for (i= 0; i < xid->gtrid_length; i++)
    {
      *buf++=_dig_vec_lower[((uchar*) xid->data)[i] >> 4];
      *buf++=_dig_vec_lower[((uchar*) xid->data)[i] & 0x0f];
    }
    *buf++= '\'';
    if (xid->bqual_length > 0 || xid->formatID != 1)
    {
      *buf++= ',';
      *buf++= 'X';
      *buf++= '\'';
      for (; i < tot_len; i++)
      {
        *buf++=_dig_vec_lower[((uchar*) xid->data)[i] >> 4];
        *buf++=_dig_vec_lower[((uchar*) xid->data)[i] & 0x0f];
      }
      *buf++= '\'';
    }
  }

  if (xid->formatID != 1)
  {
    *buf++= ',';
    buf+= my_longlong10_to_str_8bit(&my_charset_bin, buf,
            MY_INT64_NUM_DECIMAL_DIGITS, -10, xid->formatID);
  }

  return (uint)(buf - orig_buf);
}


/**
  return the list of XID's to a client, the same way SHOW commands do.

  @note
    I didn't find in XA specs that an RM cannot return the same XID twice,
    so mysql_xa_recover does not filter XID's to ensure uniqueness.
    It can be easily fixed later, if necessary.
*/

static my_bool xa_recover_callback(XID_cache_element *xs, Protocol *protocol,
                  char *data, uint data_len, CHARSET_INFO *data_cs)
{
  if (xs->xa_state == XA_PREPARED)
  {
    protocol->prepare_for_resend();
    protocol->store_longlong((longlong) xs->xid.formatID, FALSE);
    protocol->store_longlong((longlong) xs->xid.gtrid_length, FALSE);
    protocol->store_longlong((longlong) xs->xid.bqual_length, FALSE);
    protocol->store(data, data_len, data_cs);
    if (protocol->write())
      return TRUE;
  }
  return FALSE;
}


static my_bool xa_recover_callback_short(XID_cache_element *xs,
                                         Protocol *protocol)
{
  return xa_recover_callback(xs, protocol, xs->xid.data,
      xs->xid.gtrid_length + xs->xid.bqual_length, &my_charset_bin);
}


static my_bool xa_recover_callback_verbose(XID_cache_element *xs,
                                           Protocol *protocol)
{
  char buf[SQL_XIDSIZE];
  uint len= get_sql_xid(&xs->xid, buf);
  return xa_recover_callback(xs, protocol, buf, len,
                             &my_charset_utf8mb3_general_ci);
}


bool mysql_xa_recover(THD *thd)
{
  List<Item> field_list;
  Protocol *protocol= thd->protocol;
  MEM_ROOT *mem_root= thd->mem_root;
  my_hash_walk_action action;
  DBUG_ENTER("mysql_xa_recover");

  field_list.push_back(new (mem_root)
                       Item_int(thd, "formatID", 0,
                                MY_INT32_NUM_DECIMAL_DIGITS), mem_root);
  field_list.push_back(new (mem_root)
                       Item_int(thd, "gtrid_length", 0,
                                MY_INT32_NUM_DECIMAL_DIGITS), mem_root);
  field_list.push_back(new (mem_root)
                       Item_int(thd, "bqual_length", 0,
                                MY_INT32_NUM_DECIMAL_DIGITS), mem_root);
  {
    uint len;
    CHARSET_INFO *cs;

    if (thd->lex->verbose)
    {
      len= SQL_XIDSIZE;
      cs= &my_charset_utf8mb3_general_ci;
      action= (my_hash_walk_action) xa_recover_callback_verbose;
    }
    else
    {
      len= XIDDATASIZE;
      cs= &my_charset_bin;
      action= (my_hash_walk_action) xa_recover_callback_short;
    }

    field_list.push_back(new (mem_root)
                         Item_empty_string(thd, "data", len, cs), mem_root);
  }

  if (protocol->send_result_set_metadata(&field_list,
                            Protocol::SEND_NUM_ROWS | Protocol::SEND_EOF))
    DBUG_RETURN(1);

  if (xid_cache_iterate(thd, action, protocol))
    DBUG_RETURN(1);
  my_eof(thd);
  DBUG_RETURN(0);
}


/**
  This is a specific to (pseudo-) slave applier collection of standard cleanup
  actions to reset XA transaction state sim to @c ha_commit_one_phase.
  THD of the slave applier is dissociated from a transaction object in engine
  that continues to exist there.

  @param  THD current thread
  @return the value of is_error()
*/

static bool slave_applier_reset_xa_trans(THD *thd)
{
  thd->variables.option_bits&= ~(OPTION_BEGIN | OPTION_KEEP_LOG);
  thd->server_status&=
    ~(SERVER_STATUS_IN_TRANS | SERVER_STATUS_IN_TRANS_READONLY);
  DBUG_PRINT("info", ("clearing SERVER_STATUS_IN_TRANS"));

  thd->transaction->xid_state.xid_cache_element->acquired_to_recovered();
  thd->transaction->xid_state.xid_cache_element= 0;

  for (Ha_trx_info *ha_info= thd->transaction->all.ha_list, *ha_info_next;
       ha_info; ha_info= ha_info_next)
  {
    ha_info_next= ha_info->next();
    ha_info->reset();
  }
  thd->transaction->all.ha_list= 0;

  ha_close_connection(thd);
  thd->transaction->cleanup();
  thd->transaction->all.reset();

  DBUG_ASSERT(!thd->transaction->all.ha_list);
  DBUG_ASSERT(!thd->transaction->all.no_2pc);

  thd->has_waiter= false;
  MYSQL_COMMIT_TRANSACTION(thd->m_transaction_psi); // TODO/Fixme: commit?
  thd->m_transaction_psi= NULL;
  return thd->is_error();
}
