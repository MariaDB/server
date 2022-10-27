/*****************************************************************************

Copyright (c) 2013, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, 2018, MariaDB Corporation.

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

/******************************************************************//**
@file include/sync0policy.h
Policies for mutexes.

Created 2012-08-21 Sunny Bains.
***********************************************************************/

#ifndef sync0policy_h
#define sync0policy_h

#include "ut0rnd.h"
#include "os0thread.h"
#include "srv0mon.h"
#include "sync0debug.h"

#ifdef UNIV_DEBUG

template <typename Mutex> class MutexDebug: public latch_t
{
  /** Mutex to check for lock order violation */
  const Mutex *m_mutex;
  /** Filename from where enter was called */
  const char *m_filename;
  /** Line mumber in filename */
  unsigned m_line;
  /** Thread ID of the thread that owns the mutex */
  os_thread_id_t m_thread_id;
  /** Mutex protecting the above members */
  mutable OSMutex m_debug_mutex;


  void set(const Mutex *mutex, const char *filename, unsigned line,
           os_thread_id_t thread_id)
  {
    m_debug_mutex.enter();
    m_mutex= mutex;
    m_filename= filename;
    m_line= line;
    m_thread_id= thread_id;
    m_debug_mutex.exit();
  }


  const MutexDebug get() const
  {
    MutexDebug ret;
    m_debug_mutex.enter();
    ret.m_mutex= m_mutex;
    ret.m_filename= m_filename;
    ret.m_line= m_line;
    ret.m_thread_id= m_thread_id;
    m_debug_mutex.exit();
    return ret;
  }


  /**
    Called either when mutex is locked or destroyed. Thus members are protected
    from concurrent modification.
  */
  void assert_clean_context()
  {
    ut_ad(!m_mutex);
    ut_ad(!m_filename);
    ut_ad(!m_line);
    ut_ad(m_thread_id == os_thread_id_t(ULINT_UNDEFINED));
  }


public:
  /**
    Called when the mutex is "created". Note: Not from the constructor
    but when the mutex is initialised.
    @param[in]  id  Mutex ID
  */
  void init(latch_id_t id)
  {
    ut_ad(id != LATCH_ID_NONE);
    m_id= id;
    m_debug_mutex.init();
    set(0, 0, 0, os_thread_id_t(ULINT_UNDEFINED));
  }


  /** Mutex is being destroyed. */
  void destroy()
  {
    assert_clean_context();
    m_debug_mutex.destroy();
  }


  /**
    Called when an attempt is made to lock the mutex
    @param[in]  mutex    Mutex instance to be locked
    @param[in]  filename Filename from where it was called
    @param[in]  line     Line number from where it was called
  */
  void enter(const Mutex &mutex, const char *filename, unsigned line)
  {
    MutexDebug context;
    ut_ad(!is_owned());
    context.init(m_id);
    context.set(&mutex, filename, line, os_thread_get_curr_id());
    /* Check for latch order violation. */
    sync_check_lock_validate(&context);
    context.set(0, 0, 0, os_thread_id_t(ULINT_UNDEFINED));
    context.destroy();
  }


  /**
    Called when the mutex is locked
    @param[in]  mutex    Mutex instance that was locked
    @param[in]  filename Filename from where it was called
    @param[in]  line     Line number from where it was called
  */
  void locked(const Mutex &mutex, const char *filename, unsigned line)
  {
    assert_clean_context();
    set(&mutex, filename, line, os_thread_get_curr_id());
    sync_check_lock_granted(this);
  }


  /**
    Called when the mutex is released
    @param[in]  mutex  Mutex that was released
  */
  void release(const Mutex &mutex)
  {
    ut_ad(is_owned());
    set(0, 0, 0, os_thread_id_t(ULINT_UNDEFINED));
    sync_check_unlock(this);
  }


  /** @return true if thread owns the mutex */
  bool is_owned() const
  {
    return os_thread_eq(get_thread_id(), os_thread_get_curr_id());
  }


  /** @return the name of the file from the mutex was acquired */
  const char* get_enter_filename() const { return get().m_filename; }


  /** @return the name of the file from the mutex was acquired */
  unsigned get_enter_line() const { return get().m_line; }


  /** @return id of the thread that was trying to acquire the mutex */
  os_thread_id_t get_thread_id() const { return get().m_thread_id; }


  /**
    Print information about the latch
    @return the string representation
  */
  virtual std::string to_string() const
  {
    std::ostringstream msg;
    const MutexDebug ctx= get();

    msg << m_mutex->policy().to_string();
    if (ctx.m_mutex)
      msg << " addr: " << ctx.m_mutex << " acquired: "
          << sync_basename(ctx.get_enter_filename()) << ":"
          << ctx.get_enter_line();
    else
      msg << "Not locked";

    return(msg.str());
  }
};
#endif /* UNIV_DEBUG */

/** Collect the metrics per mutex instance, no aggregation. */
template <typename Mutex>
struct GenericPolicy
{
public:
	/** Called when the mutex is "created". Note: Not from the constructor
	but when the mutex is initialised.
	@param[in]	id              Mutex ID
	@param[in]	filename	File where mutex was created
	@param[in]	line		Line in filename */
	void init(
		const Mutex&,
		latch_id_t		id,
		const char*		filename,
		uint32_t		line)
		UNIV_NOTHROW
	{
		m_id = id;

		latch_meta_t&	meta = sync_latch_get_meta(id);

		ut_ad(meta.get_id() == id);

		meta.get_counter()->single_register(&m_count);

		sync_file_created_register(this, filename, uint16_t(line));
	}

	/** Called when the mutex is destroyed. */
	void destroy()
		UNIV_NOTHROW
	{
		latch_meta_t&	meta = sync_latch_get_meta(m_id);

		meta.get_counter()->single_deregister(&m_count);

		sync_file_created_deregister(this);
	}

	/** Called after a successful mutex acquire.
	@param[in]	n_spins		Number of times the thread did
					spins while trying to acquire the mutex
	@param[in]	n_waits		Number of times the thread waited
					in some type of OS queue */
	void add(
		uint32_t	n_spins,
		uint32_t	n_waits)
		UNIV_NOTHROW
	{
		/* Currently global on/off. Keeps things simple and fast */

		if (!m_count.m_enabled) {

			return;
		}

		m_count.m_spins += n_spins;
		m_count.m_waits += n_waits;

		++m_count.m_calls;
	}

	/** Print the information about the latch
	@return the string representation */
	std::string print() const
		UNIV_NOTHROW;

	/** @return the latch ID */
	latch_id_t get_id() const
		UNIV_NOTHROW
	{
		return(m_id);
	}


  /** @return the string representation */
  std::string to_string() const
  { return sync_mutex_to_string(get_id(), sync_file_created_get(this)); }

#ifdef UNIV_DEBUG
  MutexDebug<Mutex> context;
#endif

private:
  /** The user visible counters, registered with the meta-data. */
  latch_meta_t::CounterType::Count m_count;

	/** Latch meta data ID */
	latch_id_t		m_id;
};

/** Track agregate metrics policy, used by the page mutex. There are just
too many of them to count individually. */
template <typename Mutex>
class BlockMutexPolicy
{
public:
	/** Called when the mutex is "created". Note: Not from the constructor
	but when the mutex is initialised.
	@param[in]	id              Mutex ID */
	void init(const Mutex&, latch_id_t id, const char*, uint32)
		UNIV_NOTHROW
	{
		/* It can be LATCH_ID_BUF_BLOCK_MUTEX or
		LATCH_ID_BUF_POOL_ZIP. Unfortunately, they
		are mapped to the same mutex type in the
		buffer pool code. */

		m_id = id;

		latch_meta_t&	meta = sync_latch_get_meta(m_id);

		ut_ad(meta.get_id() == id);

		m_count = meta.get_counter()->sum_register();
	}

	/** Called when the mutex is destroyed. */
	void destroy()
		UNIV_NOTHROW
	{
		m_count = NULL;
	}

	/** Called after a successful mutex acquire.
	@param[in]	n_spins		Number of times the thread did
					spins while trying to acquire the mutex
	@param[in]	n_waits		Number of times the thread waited
					in some type of OS queue */
	void add(
		uint32_t	n_spins,
		uint32_t	n_waits)
		UNIV_NOTHROW
	{
		if (!m_count->m_enabled) {

			return;
		}

		m_count->m_spins += n_spins;
		m_count->m_waits += n_waits;

		++m_count->m_calls;
	}

	/** Print the information about the latch
	@return the string representation */
	std::string print() const
		UNIV_NOTHROW;

	/** @return the latch ID */
	latch_id_t get_id() const
	{
		return(m_id);
	}


  /**
    I don't think it makes sense to keep track of the file name
    and line number for each block mutex. Too much of overhead. Use the
    latch id to figure out the location from the source.

    @return the string representation
  */
  std::string to_string() const
  { return(sync_mutex_to_string(get_id(), "buf0buf.cc:0")); }

#ifdef UNIV_DEBUG
  MutexDebug<Mutex> context;
#endif

private:
  /** The user visible counters, registered with the meta-data. */
  latch_meta_t::CounterType::Count *m_count;

	/** Latch meta data ID */
	latch_id_t		m_id;
};
#endif /* sync0policy_h */
