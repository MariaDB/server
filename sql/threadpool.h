/* Copyright (C) 2012, 2020, MariaDB

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

#pragma once
#ifdef HAVE_POOL_OF_THREADS
#define MAX_THREAD_GROUPS 100000

/* Threadpool parameters */
extern uint threadpool_min_threads;  /* Minimum threads in pool */
extern uint threadpool_idle_timeout; /* Shutdown idle worker threads  after this timeout */
extern uint threadpool_size; /* Number of parallel executing threads */
extern uint threadpool_max_size;
extern uint threadpool_stall_limit;  /* time interval in milliseconds for stall checks*/
extern uint threadpool_max_threads;  /* Maximum threads in pool */
extern uint threadpool_oversubscribe;  /* Maximum active threads in group */
extern uint threadpool_prio_kickup_timer;  /* Time before low prio item gets prio boost */
extern my_bool threadpool_exact_stats; /* Better queueing time stats for information_schema, at small performance cost */
extern my_bool threadpool_dedicated_listener; /* Listener thread does not pick up work items. */
#ifdef _WIN32
extern uint threadpool_mode; /* Thread pool implementation , windows or generic */
#define TP_MODE_WINDOWS 0
#define TP_MODE_GENERIC 1
#endif

#define DEFAULT_THREADPOOL_STALL_LIMIT 500U

struct TP_connection;
struct st_vio;

extern void tp_callback(TP_connection *c);
extern void tp_timeout_handler(TP_connection *c);



/*
  Threadpool statistics
*/
struct TP_STATISTICS
{
  /* Current number of worker thread. */
  Atomic_counter<uint32_t> num_worker_threads;
};

extern TP_STATISTICS tp_stats;


/* Functions to set threadpool parameters */
extern void tp_set_min_threads(uint val);
extern void tp_set_max_threads(uint val);
extern void tp_set_threadpool_size(uint val);
extern void tp_set_threadpool_stall_limit(uint val);
extern int tp_get_idle_thread_count();
extern int tp_get_thread_count();


enum  TP_PRIORITY {
  TP_PRIORITY_HIGH,
  TP_PRIORITY_LOW,
  TP_PRIORITY_AUTO
};


enum TP_STATE
{
  TP_STATE_IDLE,
  TP_STATE_RUNNING,
  TP_STATE_PENDING
};

/*
  Connection structure, encapsulates THD + structures for asynchronous
  IO and pool.

  Platform specific parts are specified in subclasses called connection_t,
  inside threadpool_win.cc and threadpool_unix.cc
*/

class CONNECT;

struct TP_connection
{
  THD*        thd;
  CONNECT*    connect;
  TP_STATE    state;
  TP_PRIORITY priority;
  TP_connection(CONNECT *c) :
    thd(0),
    connect(c),
    state(TP_STATE_IDLE),
    priority(TP_PRIORITY_HIGH)
  {}

  virtual ~TP_connection() = default;

  /* Initialize io structures windows threadpool, epoll etc */
  virtual int init() = 0;

  virtual void set_io_timeout(int sec) = 0;

  /* Read for the next client command (async) with specified timeout */
  virtual int start_io() = 0;

  virtual void wait_begin(int type)= 0;
  virtual void wait_end() = 0;
  IF_WIN(virtual,) void init_vio(st_vio *){};
};


struct TP_pool
{
  virtual ~TP_pool() = default;
  virtual int init()= 0;
  virtual TP_connection *new_connection(CONNECT *)= 0;
  virtual void add(TP_connection *c)= 0;
  virtual int set_max_threads(uint){ return 0; }
  virtual int set_min_threads(uint){ return 0; }
  virtual int set_pool_size(uint){ return 0; }
  virtual int set_idle_timeout(uint){ return 0; }
  virtual int set_oversubscribe(uint){ return 0; }
  virtual int set_stall_limit(uint){ return 0; }
  virtual int get_thread_count() { return tp_stats.num_worker_threads; }
  virtual int get_idle_thread_count(){ return 0; }
  virtual void resume(TP_connection* c)=0;
};

#ifdef _WIN32

struct TP_pool_win:TP_pool
{
  TP_pool_win();
  int init() override;
  ~TP_pool_win() override;
  TP_connection *new_connection(CONNECT *c) override;
  void add(TP_connection *) override;
  int set_max_threads(uint) override;
  int set_min_threads(uint) override;
  void resume(TP_connection *c) override;
};
#endif

struct TP_pool_generic :TP_pool
{
  TP_pool_generic();
  ~TP_pool_generic();
  int init() override;
  TP_connection *new_connection(CONNECT *c) override;
  void add(TP_connection *) override;
  int set_pool_size(uint) override;
  int set_stall_limit(uint) override;
  int get_idle_thread_count() override;
  void resume(TP_connection* c) override;
};

#endif /* HAVE_POOL_OF_THREADS */
