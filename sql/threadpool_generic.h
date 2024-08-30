/* Copyright(C) 2019, 2020, MariaDB
 *
 * This program is free software; you can redistribute itand /or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02111 - 1301 USA*/

#if defined (HAVE_POOL_OF_THREADS)
#include <my_global.h>
#include <sql_plist.h>
#include <my_pthread.h>
#include <mysqld.h>
#include <threadpool.h>
#include <violite.h>

#ifdef _WIN32
#include <windows.h>
#include "threadpool_winsockets.h"
/* AIX may define this, too ?*/
#define HAVE_IOCP
#endif


#ifdef _WIN32
typedef HANDLE TP_file_handle;
#else
typedef int TP_file_handle;
#define  INVALID_HANDLE_VALUE -1
#endif

#ifdef __linux__
#include <sys/epoll.h>
typedef struct epoll_event native_event;
#elif defined(HAVE_KQUEUE)
#include <sys/event.h>
typedef struct kevent native_event;
#elif defined (__sun)
#include <port.h>
typedef port_event_t native_event;
#elif defined (HAVE_IOCP)
typedef OVERLAPPED_ENTRY native_event;
#else
#error threadpool is not available on this platform
#endif

struct thread_group_t;

/* Per-thread structure for workers */
struct worker_thread_t
{
  ulonglong  event_count; /* number of request handled by this thread */
  thread_group_t* thread_group;
  worker_thread_t* next_in_list;
  worker_thread_t** prev_in_list;
  mysql_cond_t  cond;
  bool          woken;
};

typedef I_P_List<worker_thread_t, I_P_List_adapter<worker_thread_t,
  & worker_thread_t::next_in_list,
  & worker_thread_t::prev_in_list>,
  I_P_List_counter
>
worker_list_t;

struct TP_connection_generic :public TP_connection
{
  TP_connection_generic(CONNECT* c);
  ~TP_connection_generic();

  int init() override { return 0; }
  void set_io_timeout(int sec) override;
  int  start_io() override;
  void wait_begin(int type) override;
  void wait_end() override;

  thread_group_t* thread_group;
  TP_connection_generic* next_in_queue;
  TP_connection_generic** prev_in_queue;
  ulonglong abs_wait_timeout;
  ulonglong enqueue_time;
  TP_file_handle fd;
  bool bound_to_poll_descriptor;
  int waiting;
  bool fix_group;
#ifdef _WIN32
  win_aiosocket win_sock{};
  void init_vio(st_vio *vio) override
  { win_sock.init(vio);}
#endif

};


typedef I_P_List<TP_connection_generic,
  I_P_List_adapter<TP_connection_generic,
  & TP_connection_generic::next_in_queue,
  & TP_connection_generic::prev_in_queue>,
  I_P_List_counter,
  I_P_List_fast_push_back<TP_connection_generic> >
  connection_queue_t;

const int NQUEUES = 2; /* We have high and low priority queues*/

enum class operation_origin
{
  WORKER,
  LISTENER
};

struct thread_group_counters_t
{
  ulonglong thread_creations;
  ulonglong thread_creations_due_to_stall;
  ulonglong wakes;
  ulonglong wakes_due_to_stall;
  ulonglong throttles;
  ulonglong stalls;
  ulonglong dequeues[2];
  ulonglong polls[2];
};

struct thread_group_t
{
  mysql_mutex_t mutex;
  connection_queue_t queues[NQUEUES];
  worker_list_t waiting_threads;
  worker_thread_t* listener;
  pthread_attr_t* pthread_attr;
  TP_file_handle  pollfd;
  int  thread_count;
  int  active_thread_count;
  int  connection_count;
  /* Stats for the deadlock detection timer routine.*/
  int io_event_count;
  int queue_event_count;
  ulonglong last_thread_creation_time;
  int  shutdown_pipe[2];
  bool shutdown;
  bool stalled;
  thread_group_counters_t counters;
  char pad[CPU_LEVEL1_DCACHE_LINESIZE];
};

#define TP_INCREMENT_GROUP_COUNTER(group,var) do {group->counters.var++;}while(0)

extern thread_group_t* all_groups;
#endif

