/*****************************************************************************

Copyright (c) 1997, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2018, 2022, MariaDB Corporation.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA

*****************************************************************************/

/**************************************************//**
@file include/read0types.h
Cursor read

Created 2/16/1997 Heikki Tuuri
*******************************************************/

#pragma once

#include "dict0mem.h"
#include "trx0types.h"
#include "srw_lock.h"
#include <algorithm>

/**
  Read view lists the trx ids of those transactions for which a consistent read
  should not see the modifications to the database.
*/
class ReadViewBase
{
  /**
    The read should not see any transaction with trx id >= this value.
    In other words, this is the "high water mark".
  */
  trx_id_t m_low_limit_id= 0;

  /**
    The read should see all trx ids which are strictly
    smaller (<) than this value. In other words, this is the
    low water mark".
  */
  trx_id_t m_up_limit_id;

  /** Set of RW transactions that was active when this snapshot was taken */
  trx_ids_t m_ids;

  /**
    The view does not need to see the undo logs for transactions whose
    transaction number is strictly smaller (<) than this value: they can be
    removed in purge if not needed by other views.
  */
  trx_id_t m_low_limit_no;

protected:
  bool empty() { return m_ids.empty(); }

  /** @return the up limit id */
  trx_id_t up_limit_id() const { return m_up_limit_id; }

public:
  /**
    Append state from another view.

    This method is used to find min(m_low_limit_no), min(m_low_limit_id) and
    all transaction ids below min(m_low_limit_id). These values effectively
    form oldest view.

    @param other    view to copy from
  */
  void append(const ReadViewBase &other)
  {
    ut_ad(&other != this);
    if (m_low_limit_no > other.m_low_limit_no)
      m_low_limit_no= other.m_low_limit_no;
    if (m_low_limit_id > other.m_low_limit_id)
      m_low_limit_id= other.m_low_limit_id;

#ifdef WITH_INNODB_SCN
    if (innodb_use_scn)
    {
      if (m_up_limit_id > other.m_up_limit_id)
      {
        m_up_limit_id= other.m_up_limit_id;
      }

      ut_ad(m_up_limit_id <= m_low_limit_id);

      m_version= std::min(m_version, other.m_version);

      if (m_low_limit_no < m_version)
      {
        m_version= m_low_limit_no;
      }
      else
      {
        m_low_limit_no= m_version;
      }
    }
    else
#endif
    {
      trx_ids_t::iterator dst= m_ids.begin();
      for (const trx_id_t id : other.m_ids)
      {
        if (id >= m_low_limit_id)
          break;
      loop:
        if (dst == m_ids.end())
        {
          m_ids.push_back(id);
          dst= m_ids.end();
          continue;
        }
        if (*dst < id)
        {
          dst++;
          goto loop;
        }
        else if (*dst > id)
          dst= m_ids.insert(dst, id) + 1;
      }
      m_ids.erase(std::lower_bound(dst, m_ids.end(), m_low_limit_id),
                  m_ids.end());

      m_up_limit_id= m_ids.empty() ? m_low_limit_id : m_ids.front();
      ut_ad(m_up_limit_id <= m_low_limit_id);
    }
  }


  /**
    Creates a snapshot where exactly the transactions serialized before this
    point in time are seen in the view.

    @param[in,out] trx transaction
  */
  inline void snapshot(trx_t *trx);


  /**
    Check whether the changes by id are visible.
    @param[in] id transaction id to check against the view
    @return whether the view sees the modifications of id.
  */
  bool changes_visible(trx_id_t id) const
  MY_ATTRIBUTE((warn_unused_result))
  {
    if (id >= m_low_limit_id)
      return false;
    return id < m_up_limit_id ||
           m_ids.empty() ||
           !std::binary_search(m_ids.begin(), m_ids.end(), id);
  }

  /**
    @param id transaction to check
    @return true if view sees transaction id
  */
  bool sees(trx_id_t id) const { return id < m_up_limit_id; }

  /** @return the low limit no */
  trx_id_t low_limit_no() const { return m_low_limit_no; }

  /** @return the low limit id */
  trx_id_t low_limit_id() const { return m_low_limit_id; }

  /** Clamp the low limit id for purge_sys.end_view */
  void clamp_low_limit_id(trx_id_t limit)
  {
    if (m_low_limit_id > limit)
      m_low_limit_id= limit;
  }

#ifdef WITH_INNODB_SCN
public:
  ReadViewBase()
  {
    m_low_limit_id= 0;
    m_up_limit_id= 0;
    m_low_limit_no= 0;
    m_version= 0;
    m_trx= nullptr;
  }
  ~ReadViewBase()= default;
  /** SCN set that are being committed but not finished yet */
  mutable trx_ids_set_t m_committing_scns;

  /** IDs set that are being committed but not finished yet */
  mutable trx_ids_set_t m_committing_ids;

  /** Version of the snapshot */
  trx_id_t m_version;

  trx_t *m_trx;

  /** Check whether the changes on record are visible.
  @param[in]  index index object
  @param[in]  block the block that contains record
  @param[in]  rec   clust record
  @param[in]  offsets offset of the record
  @param[in]  creator_trx_id trx id of creating transaction
  @return whether the view sees */
  bool changes_visible(const dict_index_t *index, buf_block_t *block,
                       const rec_t *rec, const rec_offs *offsets,
                       const trx_id_t creator_trx_id) const;

  /**
  @param scn  scn to check
  @return true if view sees transaction scn */
  bool sees_version(trx_id_t scn) const
  {
    if (scn == TRX_ID_MAX)
      return false;
    if (m_committing_scns.find(scn) != m_committing_scns.end())
    {
      /* Being committed while opening read view, always not visible */
      return false;
    }

    return (m_version > scn);
  }

  /**
  @return version number of the view */
  trx_id_t version() { return m_version; }

  /** Store trx pointer which create this read view */
  void set_trx(trx_t *trx) { m_trx= trx; }

  friend class trx_sys_t;
#endif
};


/** A ReadView with extra members required for trx_t::read_view. */
class ReadView: public ReadViewBase
{
  /**
    View state.

    Implemented as atomic to allow mutex-free view close and re-use.
    Non-owner thread is allowed to call is_open() alone without mutex
    protection as well. E.g. trx_sys.view_count() does this.

    If non-owner thread intends to access other members as well, both
    is_open() and other members accesses must be protected by m_mutex.
    E.g. copy_to().
  */
  std::atomic<bool> m_open;

  /** For synchronisation with purge coordinator. */
  mutable srw_mutex m_mutex;

  /**
    trx id of creating transaction.
    Used exclusively by the read view owner thread.
  */
  trx_id_t m_creator_trx_id;

public:
  ReadView()
  {
#ifdef WITH_INNODB_SCN
    memset(reinterpret_cast<void *>(&m_mutex), 0, sizeof m_mutex);
    m_mutex.init();
    m_creator_trx_id= 0;
    m_open= false;
#else
    memset(reinterpret_cast<void*>(this), 0, sizeof *this);
    m_mutex.init();
#endif
  }
  ~ReadView() { m_mutex.destroy(); }


  /**
    Opens a read view where exactly the transactions serialized before this
    point in time are seen in the view.

    View becomes visible to purge thread. Intended to be called by the ReadView
    owner thread.

    @param[in,out] trx transaction
  */
  void open(trx_t *trx);


  /**
    Closes the view.

    View becomes not visible to purge thread. Intended to be called by the
    ReadView owner thread.
  */
  void close() { m_open.store(false, std::memory_order_relaxed); }


  /** Returns true if view is open. */
  bool is_open() const { return m_open.load(std::memory_order_relaxed); }


  /**
    Sets the creator transaction id.

    This should be set only for views created by RW transactions.
    Intended to be called by the ReadView owner thread.
  */
  void set_creator_trx_id(trx_id_t id)
  {
    ut_ad(m_creator_trx_id == 0);
    m_creator_trx_id= id;
  }


  /**
    Writes the limits to the file.
    @param file file to write to
  */
  void print_limits(FILE *file) const
  {
    m_mutex.wr_lock();
    if (is_open())
      fprintf(file, "Trx read view will not see trx with"
                    " id >= " TRX_ID_FMT ", sees < " TRX_ID_FMT "\n",
                    low_limit_id(), up_limit_id());
    m_mutex.wr_unlock();
  }


  /**
    A wrapper around ReadViewBase::changes_visible().
    Intended to be called by the ReadView owner thread.
  */
  bool changes_visible(trx_id_t id) const
  { return id == m_creator_trx_id || ReadViewBase::changes_visible(id); }

  /**
    A wrapper around ReadViewBase::append().
    Intended to be called by the purge coordinator task.
  */
  void append_to(ReadViewBase *to) const
  {
    m_mutex.wr_lock();
    if (is_open())
      to->append(*this);
    m_mutex.wr_unlock();
  }

  /**
    Declare the object mostly unaccessible.
  */
  void mem_noaccess() const
  {
    MEM_NOACCESS(&m_open, sizeof m_open);
    /* m_mutex is accessed via trx_sys.rw_trx_hash */
    MEM_NOACCESS(&m_creator_trx_id, sizeof m_creator_trx_id);
  }

public:
  /** Check whether the changes on record are visible.
  @param[in]  index index object
  @param[in]  block the block that contains record
  @param[in]  rec   clust record
  @param[in]  offsets offset of the record
  @return whether the view sees */
  bool changes_visible(const dict_index_t *index, buf_block_t *block,
                       const rec_t *rec, const rec_offs *offsets, trx_id_t trx_id) const
  {
#ifdef WITH_INNODB_SCN
    if (innodb_use_scn)
    {
      return ReadViewBase::changes_visible(index, block, rec, offsets,
                                           m_creator_trx_id);
    }
    else
#endif
    {
      return changes_visible(trx_id);
    }
  }
  friend class trx_sys_t;
};

#ifdef WITH_INNODB_SCN

#define SCN_MAP_MAX_SIZE (1 * 1024 * 1024)
#define CLEANOUT_ARRAY_MAX_SIZE 16384

/** A map used to store mapping of trx id to scn. */
class Scn_Map
{
public:
  Scn_Map() = default;
  ~Scn_Map() = default;

  struct Elem
  {
  public:
    Elem()
    {
      m_id= 0;
      m_scn= 0;
      memset((void*)&m_lock, 0, sizeof(m_lock));
      m_lock.SRW_LOCK_INIT(PSI_NOT_INSTRUMENTED);
    }
    ~Elem() { m_lock.destroy(); }

    bool store(trx_id_t id, trx_id_t scn)
    {
      if (!m_lock.wr_lock_try())
      {
        return false;
      }

      /* Now safe to store */
      m_id= id;
      m_scn= scn;

      m_lock.wr_unlock();

      return true;
    }

    trx_id_t read(trx_id_t id)
    {
      trx_id_t ret= 0;
      if (m_id != id)
      {
        /* quick checking */
        return ret;
      }

      if (!m_lock.rd_lock_try())
      {
        return ret;
      }

      if (id != m_id)
      {
        m_lock.rd_unlock();
        return ret;
      }

      ret= m_scn;

      m_lock.rd_unlock();
      return ret;
    }

    srw_spin_lock m_lock;
    trx_id_t m_id;
    trx_id_t m_scn;
  };

  inline bool store(trx_id_t id, trx_id_t scn)
  {
    return m_elems[(id / 2) % SCN_MAP_MAX_SIZE].store(id, scn);
  }

  inline trx_id_t read(trx_id_t id)
  {
    return m_elems[(id / 2) % SCN_MAP_MAX_SIZE].read(id);
  }

private:
  struct Elem m_elems[SCN_MAP_MAX_SIZE];
};

using PageSets = std::unordered_map<uint64_t, table_id_t>;

/** Handler of SCN Manager */
class SCN_Mgr
{
  /** Multiple producer-one consumer array */
  class CleanoutArray
  {
    struct ValueType
    {
      std::mutex mtx;
      uint64_t page_id;
      table_id_t table_id;
      ValueType() : page_id(0), table_id(0) {}
      ValueType(uint64_t p, table_id_t t) : page_id(p), table_id(t) {}
      bool is_empty() { return (page_id == 0 || table_id == 0); }
      void set_no_lock(uint64_t p, table_id_t t)
      {
        page_id= p;
        table_id= t;
      }
      void set(uint64_t p, table_id_t t)
      {
        std::unique_lock<std::mutex> lck(mtx);
        set_no_lock(p, t);
      }
    };

  public:
    CleanoutArray(uint64_t size)
    {
      m_size= size;
      m_array= new ValueType[m_size];
      m_free_index= 0;
      m_consume_index= 0;
    }

    ~CleanoutArray() { delete[] m_array; }

    /* Multiple-producer */
    bool add(uint64_t value, table_id_t table_id)
    {
      int count= 0;
      while (count++ < 10)
      {
        uint64_t free_index= m_free_index.load(std::memory_order_acquire);
        uint64_t comsume_index=
            m_consume_index.load(std::memory_order_acquire);

        if (free_index - comsume_index == m_size)
        {
          /* array is full, return directly */
          return false;
        }

        if (!m_free_index.compare_exchange_weak(free_index, free_index + 1,
                                                std::memory_order_release,
                                                std::memory_order_relaxed))
        {
          continue;
        }

        m_array[free_index % m_size].set(value, table_id);

        return true;
      }

      return false;
    }

    /* One-consumer
      @return true success
    */
    bool get(uint64_t &page_id, table_id_t &table_id)
    {
      uint64_t consume_index= m_consume_index.load(std::memory_order_acquire);
      if (consume_index == m_free_index.load(std::memory_order_acquire))
      {
        /*empty */
        return false;
      }
      auto idx= consume_index % m_size;
      ValueType &value= m_array[idx];
      if (value.mtx.try_lock() == false)
      {
        return false;
      }
      if (value.is_empty())
      {
        value.mtx.unlock();
        return false;
      }
      /* take value and reset it to zero */
      page_id= value.page_id;
      table_id= value.table_id;
      value.set_no_lock(0, 0);
      value.mtx.unlock();
      m_consume_index.fetch_add(1, std::memory_order_release);
      return true;
    }

    bool is_empty() const
    {
      return (m_consume_index.load(std::memory_order_acquire) ==
              m_free_index.load(std::memory_order_acquire));
    }

  private:
    uint64_t m_size;

    ValueType *m_array;

    std::atomic<uint64_t> m_consume_index;

    std::atomic<uint64_t> m_free_index;
  };

public:
  /** Constructer */
  SCN_Mgr() = default;

  /** Destructor */
  ~SCN_Mgr() = default;

  class CleanoutWorker
  {
  public:
    CleanoutWorker(uint32_t);
    ~CleanoutWorker();

    void add_page(uint64_t compact_page_id, table_id_t table_id);

    void take_pages(PageSets &pages);

    bool is_empty() const { return m_pages.is_empty(); }

    bool is_running() { return m_task.is_running(); }

    tpool::waitable_task *get_task() { return &m_task; }

    THD *get_thd() const { return m_thd; }

  private:
    uint32_t m_id;
    CleanoutArray m_pages;
    tpool::waitable_task m_task;
    THD *m_thd;
  };

  void set_startup_id(trx_id_t up_limit_id)
  {
    if (m_startup_id == 0 || m_startup_id > up_limit_id)
    {
      m_startup_id= up_limit_id;
    }
  }

  void set_startup_scn(trx_id_t max_scn)
  {
    /* SCN before this should be visible to all session */
    ut_a(max_scn > 2);
    m_startup_scn= max_scn - 2;
  }

  trx_id_t startup_scn() { return m_startup_scn; }

  /**
  @return true if it's SCN number */
  static inline bool is_scn(trx_id_t id) { return ((id & 1) != 0); }

  /** Store scn of the transaction for fast lookup
  @param[in]  id    transaction id
  @param[in]  scn   transaction no while committing
  @return true if success */
  bool store_scn(trx_id_t id, trx_id_t scn)
  {
    return m_scn_map.store(id, scn);
  }

  /** Quickly lookup scn of relative transaction id
  @param[in]  id    transaction id
  @param[out] version scn of trx if it's being committed
  @return TRX_ID_MAX if still active, or 0 if not found, or scn value */
  trx_id_t get_scn_fast(trx_id_t id, trx_id_t *version= nullptr);

  /** Get SCN with relative transaction id
  @param[in]  id        transaction id
  @param[in]  index     index object where roll_ptr resides on
  @param[out] version scn of trx if it's being committed
  @param[in]  roll_ptr  rollback pointer of clust record */
  trx_id_t get_scn(trx_id_t id, const dict_index_t *index, roll_ptr_t roll_ptr,
                   trx_id_t *version= nullptr);

  /** Write SCN to clust record
  @param[in]  current_id      trx id of current session
  @param[in]  mtr             mini transaction
  @param[in]  rec             target record
  @param[in]  index           index object
  @param[in]  offsets         offset array for the record
  */
  void set_scn(trx_id_t current_id, mtr_t *mtr, buf_block_t *block, rec_t *rec,
               dict_index_t *index, const rec_offs *offsets);

  /** Add cursor to background set which needs to be cleanout
  @param[in]  block     the block object where record resides in
  @param[in]  pos       the start position where scn should be written to
  @param[in]  id        transaction id
  @param[in]  scn       scn of id
  @param[in]  table_id  the id of table where record resides in
  */
  void add_lazy_cursor(buf_block_t *block, rec_t *rec, ulint trx_id_offset,
                       trx_id_t id, trx_id_t scn, table_id_t table_id);

  /** Get offset where scn is stored
  @param[in]  index     index object
  @param[in]  offsets   offset array of the clust record
  @return offset where scn is stored */
  ulint scn_offset(const dict_index_t *index, const rec_offs *offsets);

  void init_for_background_task();

  /** Start background threads */
  void start();

  /** Stop background threads */
  void stop();

  /** Cleanout records on one block
  @param[in] block      the block object to be cleanout
  @param[in] mtr        mini transaction */
  void batch_write(buf_block_t *block, mtr_t *mtr);

  /** Background thread for writing back SCN */
  void cleanout_task(uint32_t slot);

  /* Periodically generate safe up limit id for taking snapshot */
  void view_task();

  void cleanout_task_monitor();

  /**@return limit no before which purging is safe. While
  taking snapshot, it'll be used by read view to avoid
  iterating lf_hash */
  trx_id_t safe_limit_no()
  {
    return m_safe_limit_no.load(std::memory_order_relaxed);
  }

  /**@return min active transaction id. This is not
  an accurate number */
  trx_id_t min_active_id()
  {
    return m_min_active_id.load(std::memory_order_relaxed);
  }

private:
  /** Set scn to specified position
  @param[in]  mtr     mini transaction
  @param[in]  ptr     position where scn is written to
  @param[in]  id      transaction id
  @param[in]  scn     scn number of transaction */
  void set_scn(mtr_t *mtr, buf_block_t *block, rec_t *rec, ulint trx_id_offset,
               trx_id_t id, trx_id_t scn);

  /** Storing trx id->scn mapping */
  Scn_Map m_scn_map;

  /** Storing trx id->scn mapping to avoid duplicate
  looking up */
  Scn_Map m_random_map;

  CleanoutWorker **m_cleanout_workers{nullptr};

  /** up transaction id on startup */
  trx_id_t m_startup_id{0};

  /** SCN number taken on startup */
  trx_id_t m_startup_scn{0};

  /** Min active transaction id */
  std::atomic<trx_id_t> m_min_active_id{0};

  std::atomic<trx_id_t> m_safe_limit_no{0};

  /** Flag to tell if background threads should stop or not */
  std::atomic<bool> m_abort{false};

  std::unique_ptr<tpool::timer> m_view_task_timer;

  std::unique_ptr<tpool::timer> m_cleanout_task_timer;
};

extern SCN_Mgr scn_mgr;
#endif /* WITH_INNODB_SCN */
