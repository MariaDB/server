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
  Notices client sockets that failed (reset etc), while the server is busy
  executing a statement and does not read from or write to the socket.
  The statement is then aborted with KILL_DISCONNECT, like KILL QUERY, unless
  the connection is idle. If a statement is aborted, the connection ends too.
  If the kill comes when nothing is aborted, for example the statement has
  finished and the client already sent COM_QUIT, nothing happens: the
  connection reads the next command, or the end of the stream, as usual.
  Only the events the OS reports on its own are used: EPOLLERR and EPOLLHUP
  on Linux, and EPOLLRDHUP for TCP, FD_CLOSE on Windows, EV_EOF of kqueue on
  BSD and macOS. Incoming data is not reported.
  A Windows named pipe is watched through its local client process, the
  statement is aborted when the process exits.

  A connection is watched from the end of login until THD::disconnect().
*/

#include "violite.h"

#if defined(__FreeBSD__) || defined(__APPLE__) || defined(__NetBSD__) || \
    defined(__OpenBSD__) || defined(__DragonFly__)
#define DISCONNECT_WATCHER_KQUEUE
#endif

/* Systems that have a watcher; elsewhere the disconnect_watcher is always OFF */
#if !defined(EMBEDDED_LIBRARY) && (defined(__linux__) || defined(_WIN32) || \
                                   defined(DISCONNECT_WATCHER_KQUEUE))
#define HAVE_DISCONNECT_WATCHER
#endif

#ifdef HAVE_DISCONNECT_WATCHER
bool disconnect_watcher_start();
void disconnect_watcher_stop();
#if defined(__linux__) || defined(DISCONNECT_WATCHER_KQUEUE)
/* The fd to poll in the main loop, and what to call when it is readable */
int disconnect_watcher_fd();
void disconnect_watcher_process();
#endif
/*
  Both are called by the thread that owns the connection, with no THD locks
  held. add() returns an opaque handle, NULL if the connection is not watched.
  remove() must be called before the socket is closed, and resets the handle.
  The watcher refers to a connection only by its id.
*/
void *disconnect_watcher_add(ulonglong thread_id, Vio *vio);
void disconnect_watcher_remove(void **handle, Vio *vio);
#else
static inline bool disconnect_watcher_start() { return false; }
static inline void disconnect_watcher_stop() {}
static inline int disconnect_watcher_fd() { return -1; }
static inline void disconnect_watcher_process() {}
static inline void *disconnect_watcher_add(ulonglong, Vio *) { return NULL; }
static inline void disconnect_watcher_remove(void **, Vio *) {}
#endif

#endif
