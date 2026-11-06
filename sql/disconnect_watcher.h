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

#ifndef DISCONNECT_WATCHER_INCLUDED
#define DISCONNECT_WATCHER_INCLUDED

/*
  Notices client sockets that were closed or failed (FIN, reset etc) and does
  not touch the socket. THD::client_gone is then set. A running command that
  expects an answer is aborted like KILL CONNECTION does, and so is the next
  such command, if the connection is idle. COM_QUIT and COM_STMT_CLOSE are not
  aborted, a client can send them and disconnect.
  Connections with Galera replication (WSREP) are not aborted.

  Only events that need no read are used: EPOLLERR, EPOLLHUP and EPOLLRDHUP on
  Linux, FD_CLOSE on Windows, EV_EOF of kqueue on BSD, EVFILT_SOCK on macOS. A
  Windows named pipe is watched through its local client process.

  A connection is watched from the end of login until COM_QUIT, or
  end_connection(), whichever comes first.
*/

#include "violite.h"

/* A connection that is watched, opaque to the callers */
struct Disconnect_watch;

#if defined(__FreeBSD__) || defined(__APPLE__) || defined(__NetBSD__) || \
    defined(__OpenBSD__) || defined(__DragonFly__)
#define DISCONNECT_WATCHER_KQUEUE
#endif

/* Systems that have a watcher, elsewhere disconnect_watcher does not exist */
#if !defined(EMBEDDED_LIBRARY) && (defined(__linux__) || defined(_WIN32) || \
                                   defined(DISCONNECT_WATCHER_KQUEUE))
#define HAVE_DISCONNECT_WATCHER
#endif

#ifdef HAVE_DISCONNECT_WATCHER
int disconnect_watcher_start();
void disconnect_watcher_stop();
void disconnect_watcher_end();
/** The watcher is working, it started and is not stopping */
bool disconnect_watcher_running();

/**
  Start watching a connection. Called by the thread that owns the connection,
  with no THD locks held.

  @param thread_id  id of the connection, the watcher refers to it by that only
  @param vio        connection of the client

  @return an opaque handle for disconnect_watcher_remove(), NULL if the
          connection is not watched
*/
Disconnect_watch *disconnect_watcher_add(my_thread_id thread_id, Vio *vio);

/**
  Stop watching a connection. Called by the thread that owns the connection,
  with no THD locks held, before the socket is closed.

  @param handle  [in,out] handle from disconnect_watcher_add(), reset to NULL
  @param vio     connection of the client
*/
void disconnect_watcher_remove(Disconnect_watch **handle, Vio *vio);
#else
static inline int disconnect_watcher_start() { return 0; }
static inline void disconnect_watcher_stop() {}
static inline void disconnect_watcher_end() {}
static inline bool disconnect_watcher_running() { return false; }
static inline Disconnect_watch *disconnect_watcher_add(my_thread_id, Vio *)
{
  return NULL;
}
static inline void disconnect_watcher_remove(Disconnect_watch **, Vio *) {}
#endif

#endif
