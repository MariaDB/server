/*
   Copyright (c) 2023, MariaDB.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335
   USA
*/

/*
  Connection pool, with parallel query execution
  Does not use threads, but IO multiplexing via mysql_send_query and
  poll/iocp to wait for completions
*/
#include <my_global.h>
#ifdef _WIN32
#include <winsock2.h>
#else
#include <poll.h>
#endif
#include "connection_pool.h"
#include <my_compiler.h>

namespace async_pool
{
static ATTRIBUTE_NORETURN void die(const char *format, ...)
{
  va_list args;
  va_start(args, format);
  vfprintf(stderr, format, args);
  va_end(args);
  abort();
}

pooled_connection *connection_pool::get_connection()
{
  while (free_connections.empty())
    wait_for_completions();
  auto c= free_connections.front();
  free_connections.pop();
  return c;
}

#ifdef _WIN32
void connection_pool::add_to_pollset(pooled_connection *c)
{
  DWORD err= ERROR_SUCCESS;
  static char ch;
  WSABUF buf;
  buf.len= 0;
  buf.buf= &ch;
  if (!c->is_pipe)
  {
    /* Do async io (sockets). */
    DWORD flags= 0;
    if (WSARecv((SOCKET) c->handle, &buf, 1, 0, &flags, c, NULL))
      err= WSAGetLastError();
  }
  else
  {
    /* Do async read (named pipe) */
    if (!ReadFile(c->handle, buf.buf, buf.len, 0, c))
      err= GetLastError();
  }

  if (err && err != ERROR_IO_PENDING)
    die("%s failed: %d\n", c->is_pipe ? "ReadFile" : "WSARecv", err);
}

/*
  Wait for completions of queries.Uses IOCP on windows to wait for completions.
  (ReadFile/WSARecv with 0 bytes serves as readiness notification)
*/
void connection_pool::wait_for_completions()
{
  ULONG n;
  OVERLAPPED_ENTRY events[32];
  if (!GetQueuedCompletionStatusEx(iocp, events, array_elements(events), &n, INFINITE,
                                   FALSE))
  {
    die("GetQueuedCompletionStatusEx failed: %d\n", GetLastError());
  }

  for (ULONG i= 0; i < n; i++)
  {
    auto c= (pooled_connection *) events[i].lpOverlapped;
    if (!c)
      die("GetQueuedCompletionStatusEx unexpected return");
    DBUG_ASSERT(c->mysql);
    DBUG_ASSERT(!events[i].lpCompletionKey);
    DBUG_ASSERT(!events[i].dwNumberOfBytesTransferred);
    complete_query(c);
  }
}
#else /* !_WIN32 */
void connection_pool::add_to_pollset(pooled_connection *c)
{
  size_t idx= c - &all_connections[0];
  pollfd *pfd= &pollset[idx];
  pfd->fd= c->fd;
  pfd->events= POLLIN;
  pfd->revents= 0;
}

/* something Linux-ish, can be returned for POLLIN event*/
#ifndef POLLRDHUP
#define POLLRDHUP 0
#endif

void connection_pool::wait_for_completions()
{
  int n;
  while ((n= poll(pollset.data(), pollset.size(), -1)) <= 0)
  {
    if (errno == EINTR)
      continue;
    die("poll failed: %d\n", errno);
  }

  for (uint i= 0; n > 0 && i < pollset.size(); i++)
  {
    pollfd* pfd= &pollset[i];
    if (pfd->revents &
        (POLLIN | POLLPRI | POLLHUP | POLLRDHUP| POLLERR | POLLNVAL))
    {
      pfd->events= 0;
      pfd->revents= 0;
      pfd->fd= -1;
      complete_query(&all_connections[i]);
      n--;
    }
  }
  if (n)
    die("poll() failed to find free connection: %d\n");
}
#endif

void connection_pool::complete_query(pooled_connection *c)
{
  int err= mysql_read_query_result(c->mysql);
  if (c->on_completion)
    c->on_completion(c->mysql, c->query.c_str(), !err, c->context);
  if (c->release_connection)
  {
    c->in_use= false;
    free_connections.push(c);
  }
}

connection_pool::~connection_pool()
{
  close();
}

void connection_pool::init(MYSQL *con[], size_t n)
{
#ifdef _WIN32
  iocp= CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
  if (!iocp)
    die("CreateIoCompletionPort failed: %d\n", GetLastError());
#else
  pollset.resize(n);
  for (auto &pfd : pollset)
    pfd.fd= -1;
#endif


  for (size_t i= 0; i < n; i++)
    all_connections.emplace_back(con[i]);

  for (auto &con : all_connections)
  {
    free_connections.push(&con);
#ifdef _WIN32
    if (!CreateIoCompletionPort(con.handle, iocp, 0, 0))
      die("CreateIoCompletionPort failed: %d\n", GetLastError());
#endif
  }
}

int connection_pool::execute_async(const char *query,
                                   query_completion_handler on_completion,
                                   void *context, bool release_connecton)
{
  auto c= get_connection();
  c->context= context;
  c->on_completion= on_completion;
  c->release_connection= release_connecton;
  c->query= query;

  int ret= mysql_send_query(c->mysql, query, (unsigned long) c->query.size());
  if (ret)
  {
    free_connections.push(c);
    return ret;
  }

  c->in_use= true;
  add_to_pollset(c);
  return 0;
}

/*
  Wait until all queries are completed and all 
  connections are idle.
*/
void connection_pool::wait_all()
{
  while (free_connections.size() != all_connections.size())
    wait_for_completions();
}

void connection_pool::for_each_connection(void(*f)(MYSQL *mysql))
{
  for (auto &c : all_connections)
   f(c.mysql);
}

int connection_pool::close()
{
  for (auto &c : all_connections)
    mysql_close(c.mysql);

  all_connections.clear();
  while (!free_connections.empty())
    free_connections.pop();
#ifdef _WIN32
  if (iocp)
  {
    CloseHandle(iocp);
    iocp= nullptr;
  }
#endif
  return 0;
}

pooled_connection::pooled_connection(MYSQL *c)
{
  mysql= c;
#ifdef _WIN32
  OVERLAPPED *ov= static_cast<OVERLAPPED *>(this);
  memset(ov, 0, sizeof(OVERLAPPED));
  mysql_protocol_type protocol;
  if (c->host && !strcmp(c->host, "."))
    protocol= MYSQL_PROTOCOL_PIPE;
  else
    protocol= (mysql_protocol_type) c->options.protocol;
  is_pipe= protocol == MYSQL_PROTOCOL_PIPE;
  handle= (HANDLE) mysql_get_socket(c);
#else
  fd= mysql_get_socket(c);
#endif
}

} // namespace async_pool
