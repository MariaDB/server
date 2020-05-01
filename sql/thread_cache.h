/*
  Copyright (C) 2020 MariaDB Foundation

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA
*/


/**
  MariaDB thread cache for "one thread per connection" scheduler.

  Thread cache allows to re-use threads (as well as THD objects) for
  subsequent connections.
*/
class Thread_cache
{
  mutable mysql_cond_t COND_thread_cache;
  mutable mysql_cond_t COND_flush_thread_cache;
  mutable mysql_mutex_t LOCK_thread_cache;
  /** Queue of new connection requests. */
  I_List<CONNECT> list;
  /** Number of threads parked in the cache. */
  ulong cached_thread_count;
  /** Number of active flush requests. */
  uint32_t kill_cached_threads;
  /**
    PFS stuff, only used during initialization.
    Unfortunately needs to survive till destruction.
  */
  PSI_cond_key key_COND_thread_cache, key_COND_flush_thread_cache;
  PSI_mutex_key key_LOCK_thread_cache;

public:
  void init()
  {
#ifdef HAVE_PSI_INTERFACE
    PSI_cond_info conds[]=
    {
      { &key_COND_thread_cache, "COND_thread_cache", PSI_FLAG_GLOBAL },
      { &key_COND_flush_thread_cache, "COND_flush_thread_cache",
        PSI_FLAG_GLOBAL }
    };
    PSI_mutex_info mutexes[]=
    {
      { &key_LOCK_thread_cache, "LOCK_thread_cache", PSI_FLAG_GLOBAL }
    };
    mysql_mutex_register("sql", mutexes, array_elements(mutexes));
    mysql_cond_register("sql", conds, array_elements(conds));
#endif
    mysql_mutex_init(key_LOCK_thread_cache, &LOCK_thread_cache,
                     MY_MUTEX_INIT_FAST);
    mysql_cond_init(key_COND_thread_cache, &COND_thread_cache, 0);
    mysql_cond_init(key_COND_flush_thread_cache, &COND_flush_thread_cache, 0);
    list.empty();
    kill_cached_threads= 0;
    cached_thread_count= 0;
  }


  void destroy()
  {
    DBUG_ASSERT(cached_thread_count == 0);
    DBUG_ASSERT(list.is_empty());
    mysql_cond_destroy(&COND_flush_thread_cache);
    mysql_cond_destroy(&COND_thread_cache);
    mysql_mutex_destroy(&LOCK_thread_cache);
  }


  /**
    Flushes thread cache.

    Awakes parked threads and requests them to shutdown.
    Waits until last parked thread leaves the cache.
  */
  void flush()
  {
    mysql_mutex_lock(&LOCK_thread_cache);
    kill_cached_threads++;
    while (cached_thread_count)
    {
      mysql_cond_broadcast(&COND_thread_cache);
      mysql_cond_wait(&COND_flush_thread_cache, &LOCK_thread_cache);
    }
    kill_cached_threads--;
    mysql_mutex_unlock(&LOCK_thread_cache);
  }


  /**
    Flushes thread cache and forbids threads parking in the cache.

    This is a pre-shutdown hook.
  */
  void final_flush()
  {
    kill_cached_threads++;
    flush();
  }


  /**
    Requests parked thread to serve new connection.

    @return
      @retval true connection is enqueued and parked thread is about to serve it
      @retval false thread cache is empty
  */
  bool enqueue(CONNECT *connect)
  {
    mysql_mutex_lock(&LOCK_thread_cache);
    if (cached_thread_count)
    {
      list.push_back(connect);
      cached_thread_count--;
      mysql_mutex_unlock(&LOCK_thread_cache);
      mysql_cond_signal(&COND_thread_cache);
      return true;
    }
    mysql_mutex_unlock(&LOCK_thread_cache);
    return false;
  }


  /**
    Parks thread in the cache.

    Thread execution is suspended until either of the following occurs:
    - thread is requested to serve new connection;
    - thread cache is flushed;
    - THREAD_CACHE_TIMEOUT elapsed.

    @return
      @retval pointer to CONNECT if requested to serve new connection
      @retval 0 if thread cache is flushed or on timeout
  */
  CONNECT *park()
  {
    struct timespec abstime;
    CONNECT *connect;
    bool flushed= false;
    DBUG_ENTER("Thread_cache::park");
    set_timespec(abstime, THREAD_CACHE_TIMEOUT);

    /*
      Delete the instrumentation for the job that just completed,
      before parking this pthread in the cache (blocked on COND_thread_cache).
    */
    PSI_CALL_delete_current_thread();

#ifndef DBUG_OFF
    while (_db_is_pushed_())
      _db_pop_();
#endif

    mysql_mutex_lock(&LOCK_thread_cache);
    if ((connect= list.get()))
      cached_thread_count++;
    else if (cached_thread_count < thread_cache_size && !kill_cached_threads)
    {
      /* Don't kill the thread, just put it in cache for reuse */
      DBUG_PRINT("info", ("Adding thread to cache"));
      cached_thread_count++;
      for (;;)
      {
        int error= mysql_cond_timedwait(&COND_thread_cache, &LOCK_thread_cache,
                                         &abstime);
        flushed= kill_cached_threads;
        if ((connect= list.get()))
          break;
        else if (flushed || error == ETIMEDOUT || error == ETIME)
        {
          /*
            If timeout, end thread.
            If a new thread is requested, we will handle the call, even if we
            got a timeout (as we are already awake and free)
          */
          cached_thread_count--;
          break;
        }
      }
    }
    mysql_mutex_unlock(&LOCK_thread_cache);
    if (flushed)
      mysql_cond_signal(&COND_flush_thread_cache);
    DBUG_RETURN(connect);
  }


  /** Returns the number of parked threads. */
  ulong size() const
  {
    mysql_mutex_lock(&LOCK_thread_cache);
    ulong r= cached_thread_count;
    mysql_mutex_unlock(&LOCK_thread_cache);
    return r;
  }
};

extern Thread_cache thread_cache;
