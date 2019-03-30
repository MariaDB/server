/*
   Copyright (c) 2000, 2016, Oracle and/or its affiliates.
   Copyright (c) 2009, 2019, MariaDB Corporation.

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

enum xa_states { XA_ACTIVE= 0, XA_IDLE, XA_PREPARED, XA_ROLLBACK_ONLY };

class XID_cache_element;

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
    XID_cache_element *element= (XID_cache_element*) (ptr + LF_HASH_OVERHEAD);
    DBUG_ASSERT(!element->is_set(ACQUIRED));
  }
  static uchar *key(const XID_cache_element *element, size_t *length,
                    my_bool not_used __attribute__((unused)))
  {
    *length= element->xid.key_length();
    return element->xid.key();
  }
};


struct XID_STATE {
  XID_cache_element *xid_cache_element;
  /*
    Binary logging status.
    It is set to TRUE at XA PREPARE if the transaction was written
    to the binlog.
    Naturally FALSE means the transaction was not written to
    the binlog. Happens if the trnasaction did not modify anything
    or binlogging was turned off. In that case we shouldn't binlog
    the consequent XA COMMIT/ROLLBACK.
    The recovered transaction after server restart sets it to TRUE always.
    That can cause inconsistencies (shoud be fixed?).
  */
  bool is_binlogged;

  bool check_has_uncommitted_xa() const;
  bool is_explicit_XA() const { return xid_cache_element != 0; }
  void set_error(uint error);
  void er_xaer_rmfail() const;
  XID *get_xid() const;
  void reset()
  {
    //TODO: what's an equivalent
    //xid.null();
    is_binlogged= false;
  }
  void set_binlogged()
  { is_binlogged= true; }
  void unset_binlogged()
  { is_binlogged= false; }
};

void xid_cache_init(void);
void xid_cache_free(void);
bool xid_cache_insert(XID *xid);
bool xid_cache_insert(THD *thd, XID_STATE *xid_state, XID *xid);
void xid_cache_delete(THD *thd, XID_STATE *xid_state);

bool trans_xa_start(THD *thd);
bool trans_xa_end(THD *thd);
bool trans_xa_prepare(THD *thd);
bool trans_xa_commit(THD *thd);
bool trans_xa_rollback(THD *thd);
bool trans_xa_detach(THD *thd);
bool mysql_xa_recover(THD *thd);
bool applier_reset_xa_trans(THD *thd);
