/* Copyright (c) 2026, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

#include "mariadb.h"
#include "disconnect_watcher.h"

#if !defined(EMBEDDED_LIBRARY) && (defined(__linux__) || defined(_WIN32) || \
                                   defined(DISCONNECT_WATCHER_KQUEUE))

#include "sql_class.h"
#include "sql_show.h"                           // find_thread_by_id
#include <atomic>

/*
  Synchronization: the watcher holds no lock of its own while it touches a THD.
  Like KILL, it finds the THD by thread id in server_threads, which returns it
  with LOCK_thd_kill held, so the THD cannot be deleted. It never uses the
  socket itself. The watcher may thus act on a stale event, for a THD that is
  already gone or closing; it is harmless, as the id is never reused.
  The owner thread unregisters the socket before closing it.
*/

static bool is_socket_vio(Vio *vio)
{
  switch (vio_type(vio)) {
  case VIO_TYPE_TCPIP:
  case VIO_TYPE_SOCKET:
  case VIO_TYPE_SSL:
    return true;
  default:
    return false;
  }
}


/* The socket reported an error or hangup, kill the connection like KILL does */
static void handle_socket_event(my_thread_id id)
{
  if (abort_loop)                               // shutdown kills everything
    return;
  THD *thd= find_thread_by_id(id, false);
  if (!thd)
    return;                                     // connection is gone

  mysql_mutex_lock(&thd->LOCK_thd_data);
  thd->awake_no_mutex(KILL_DISCONNECT);
  mysql_mutex_unlock(&thd->LOCK_thd_data);
  mysql_mutex_unlock(&thd->LOCK_thd_kill);
}


#ifdef __linux__

#include <sys/epoll.h>

/*
  One epoll set with all client sockets, in the oneshot mode. EPOLLERR and
  EPOLLHUP are always reported, data is not requested.
  A close by a TCP peer, which is also how a crashed client looks, is only a
  FIN, reported by EPOLLRDHUP alone. A unix socket reports its peer closing
  with EPOLLHUP, and only a half-close with EPOLLRDHUP, so EPOLLRDHUP is
  requested for TCP only.
  The epoll fd itself is polled by the connection accepting loop in the main
  thread, which calls disconnect_watcher_process() when it becomes readable.
*/
static std::atomic<int> epfd{-1};

/* The transport below TLS is not seen in vio_type(), ask the socket */
static bool is_unix_socket(Vio *vio)
{
  sockaddr_storage addr;
  socklen_t len= sizeof(addr);
  return !getsockname(vio_fd(vio), (sockaddr *) &addr, &len) &&
         addr.ss_family == AF_UNIX;
}

bool disconnect_watcher_start()
{
  if ((epfd= epoll_create1(EPOLL_CLOEXEC)) >= 0)
    return false;
  sql_print_warning("Can't start the client disconnect watcher (errno: %iE)",
                    errno);
  return true;
}


void disconnect_watcher_stop()
{
  int fd= epfd.exchange(-1);
  if (fd >= 0)
    close(fd);
}


int disconnect_watcher_fd()
{
  return epfd;
}


void disconnect_watcher_process()
{
  epoll_event events[16];
  int n;
  while ((n= epoll_wait(epfd, events, 16, 0)) > 0)
  {
    for (int i= 0; i < n; i++)
      handle_socket_event((my_thread_id) events[i].data.u64);
    if (n < 16)
      break;
  }
}


void *disconnect_watcher_add(ulonglong thread_id, Vio *vio)
{
  if (epfd < 0 || !vio || !is_socket_vio(vio))
    return nullptr;
  epoll_event ev= {};
  ev.events= EPOLLONESHOT;
  if (!is_unix_socket(vio))
    ev.events|= EPOLLRDHUP;
  ev.data.u64= thread_id;
  return epoll_ctl(epfd, EPOLL_CTL_ADD, vio_fd(vio), &ev) ? nullptr : &epfd;
}


void disconnect_watcher_remove(void **handle, Vio *vio)
{
  if (!*handle)
    return;
  *handle= nullptr;
  /*
    The socket is still open, and will be closed after this. An event that is
    already dequeued is handled safely, see above.
  */
  if (epfd >= 0 && vio)
    epoll_ctl(epfd, EPOLL_CTL_DEL, vio_fd(vio), nullptr);
}

#elif defined(DISCONNECT_WATCHER_KQUEUE)

#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>
#include <fcntl.h>

/*
  One kqueue with all client sockets, polled by the connection accepting loop
  like the epoll fd on Linux. A read filter fires on data too, so its low
  water mark is set out of reach, leaving only EV_EOF and socket errors, which
  are reported regardless of the mark. EV_EOF is also set by an orderly close.
*/
static std::atomic<int> kq{-1};

bool disconnect_watcher_start()
{
  if ((kq= kqueue()) >= 0)
  {
    fcntl(kq, F_SETFD, FD_CLOEXEC);
    return false;
  }
  sql_print_warning("Can't start the client disconnect watcher (errno: %iE)",
                    errno);
  return true;
}


void disconnect_watcher_stop()
{
  int fd= kq.exchange(-1);
  if (fd >= 0)
    close(fd);
}


int disconnect_watcher_fd()
{
  return kq;
}


void disconnect_watcher_process()
{
  struct kevent events[16];
  static const struct timespec no_wait= {0, 0};
  int n;
  while ((n= kevent(kq, nullptr, 0, events, 16, &no_wait)) > 0)
  {
    for (int i= 0; i < n; i++)
      if (events[i].flags & (EV_EOF | EV_ERROR))
        handle_socket_event((my_thread_id) (uintptr_t) events[i].udata);
    if (n < 16)
      break;
  }
}


void *disconnect_watcher_add(ulonglong thread_id, Vio *vio)
{
  if (kq < 0 || !vio || !is_socket_vio(vio))
    return nullptr;
  struct kevent ev;
  EV_SET(&ev, vio_fd(vio), EVFILT_READ, EV_ADD | EV_ONESHOT, NOTE_LOWAT,
         0x7fffffff, (void *) (uintptr_t) thread_id);
  return kevent(kq, &ev, 1, nullptr, 0, nullptr) ? nullptr : &kq;
}


void disconnect_watcher_remove(void **handle, Vio *vio)
{
  if (!*handle)
    return;
  *handle= nullptr;
  /* ENOENT is fine, the oneshot filter is deleted when it fires */
  if (kq >= 0 && vio)
  {
    struct kevent ev;
    EV_SET(&ev, vio_fd(vio), EVFILT_READ, EV_DELETE, 0, 0, nullptr);
    kevent(kq, &ev, 1, nullptr, 0, nullptr);
  }
}

#else  /* _WIN32 */

/*
  A socket is watched with an event set by WSAEventSelect(FD_CLOSE), a named
  pipe by its client process handle. A thread pool waits for them. It is a
  pool of its own, with a single thread, which runs all the callbacks one by one,
  like the main loop handles epoll events. The callback gets the connection id
  by value: it may run after the connection is gone, and handle_socket_event()
  ignores ids it does not find.

  Polling the socket with the AFD driver and one completion port for all of them
  would need no handle per connection. It cannot be used though: while such
  a poll is pending, select(), which the vio layer waits with, loses
  the incoming data notification of the same socket.
*/

static PTP_POOL pool;
static TP_CALLBACK_ENVIRON env;
static std::atomic<bool> stopping;

struct Watch
{
  PTP_WAIT wait;
  HANDLE handle;                        // event, or client process
};


/* The pool thread is a mysys thread, until it exits when the pool is closed */
struct Mysys_thread_scope
{
  Mysys_thread_scope() { my_thread_init(); }
  ~Mysys_thread_scope() { my_thread_end(); }
};

static VOID CALLBACK on_signaled(PTP_CALLBACK_INSTANCE, PVOID context,
                                 PTP_WAIT, TP_WAIT_RESULT result)
{
  if (result != WAIT_OBJECT_0 || stopping)
    return;
  static thread_local Mysys_thread_scope scope;
  handle_socket_event((my_thread_id) (ULONG_PTR) context);
}


bool disconnect_watcher_start()
{
  if ((pool= CreateThreadpool(nullptr)) &&
      SetThreadpoolThreadMinimum(pool, 1) &&
      (SetThreadpoolThreadMaximum(pool, 1), true))
  {
    InitializeThreadpoolEnvironment(&env);
    SetThreadpoolCallbackPool(&env, pool);
    return false;
  }
  sql_print_warning("Can't start the client disconnect watcher (error: %lu)",
                    GetLastError());
  if (pool)
    CloseThreadpool(pool);
  pool= nullptr;
  return true;
}


/*
  Callbacks from now on do nothing. The pool, and its thread, go away when
  the connections have removed their waits, and my_thread_global_end() waits
  for the thread.
*/
void disconnect_watcher_stop()
{
  if (pool && !stopping.exchange(true))
    CloseThreadpool(pool);
}


/*
  The client process of a local named pipe, opened for waiting. Not opened
  directly, the client is impersonated: the server may not be allowed to
  open it. The handle also keeps the process id from being reused.
  Remote clients are not watched, their process id is not valid here.
*/
static HANDLE open_pipe_client(HANDLE pipe)
{
  char host[256];
  ULONG pid;
  if (GetNamedPipeClientComputerNameA(pipe, host, sizeof(host)) ||
      GetLastError() != ERROR_PIPE_LOCAL ||
      !GetNamedPipeClientProcessId(pipe, &pid))
    return nullptr;
  HANDLE process= OpenProcess(SYNCHRONIZE, FALSE, pid);
  if (!process && GetLastError() == ERROR_ACCESS_DENIED &&
      ImpersonateNamedPipeClient(pipe))
  {
    process= OpenProcess(SYNCHRONIZE, FALSE, pid);
    RevertToSelf();
  }
  return process;
}


void *disconnect_watcher_add(ulonglong thread_id, Vio *vio)
{
  if (!vio || !pool || stopping)
    return nullptr;
  bool is_event= is_socket_vio(vio);
  HANDLE h;
  if (is_event)
    h= CreateEvent(nullptr, TRUE, FALSE, nullptr);
  else if (vio_type(vio) == VIO_TYPE_NAMEDPIPE)
    h= open_pipe_client(vio->hPipe);
  else
    return nullptr;
  if (!h)
    return nullptr;

  Watch *w= new (std::nothrow) Watch();
  if (w)
  {
    w->handle= h;
    if ((w->wait= CreateThreadpoolWait(on_signaled, (PVOID) thread_id, &env)))
    {
      if (!is_event || !WSAEventSelect(vio_fd(vio), h, FD_CLOSE))
      {
        SetThreadpoolWait(w->wait, h, nullptr);
        return w;
      }
      CloseThreadpoolWait(w->wait);
    }
    delete w;
  }
  CloseHandle(h);
  return nullptr;
}


void disconnect_watcher_remove(void **handle, Vio *)
{
  Watch *w= static_cast<Watch*>(*handle);
  if (!w)
    return;
  *handle= nullptr;
  /*
    Closing the wait cancels it, and only then the handle can be closed. A
    callback already queued may still run, it does no harm. The socket is
    closed next, which ends its event selection.
  */
  CloseThreadpoolWait(w->wait);
  CloseHandle(w->handle);
  delete w;
}

#endif /* _WIN32 */

#endif
