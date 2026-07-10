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

#pragma once

#include <mysql.h>
#include <vector>
#include <queue>
#include <string>
#ifdef _WIN32
#include <windows.h>
#else
#include <poll.h>
#endif

/*
  Implementation of asynchronous mariadb connection pool.

  This pool consists of set of MYSQL* connections, created by C API
  function. The intention is that all connections have the same state
  same server, by the same user etc.

  The "asynchronous" means the queries are executed on the server
  without waiting for the server reply. The queries are submitted
  with mysql_send_query(), and completions are picked by poll/IOCP.
*/

namespace async_pool
{
typedef void (*query_completion_handler)(MYSQL *mysql, const char *query, bool success, void *context);

struct pooled_connection
#ifdef _WIN32
    : OVERLAPPED
#endif
{
  MYSQL *mysql;
  query_completion_handler on_completion=NULL;
  void *context=NULL;
  std::string query;
  bool in_use=false;
  bool release_connection=false;
#ifdef _WIN32
  bool is_pipe;
  HANDLE handle;
#else
  int fd;
#endif
  pooled_connection(MYSQL *mysql);
};


struct connection_pool
{
private:
  std::vector<pooled_connection> all_connections;
  std::queue<pooled_connection *> free_connections;
  pooled_connection *get_connection();
  void wait_for_completions();
  void complete_query(pooled_connection *c);
  void add_to_pollset(pooled_connection *c);

#ifdef _WIN32
  HANDLE iocp=nullptr;
#else
  std::vector<pollfd> pollset;
#endif
public:
  ~connection_pool();

  /**
    Add connections to the connection pool

    @param con  - connections
    @param n_connections - number of connections
  */
  void init(MYSQL *con[], size_t n_connections);

  /**
  Send query to the connection pool
  Executes query on a connection in the pool, using mysql_send_query

  @param query         - query string
  @param on_completion - callback function to be called on completion
  @param context       - user context that will be passed to the callback function
  @param release_connecton - if true, the connection should be released to the
         pool after the query is executed. If you execute another
         mysql_send_query() on the same connection, set this to false.

  Note: the function will block if there are no free connections in the pool.

  @return return code of mysql_send_query
  */
  int execute_async(const char *query, query_completion_handler on_completion, void *context, bool release_connecton=true);

  /** Waits for all outstanding queries to complete.*/
  void wait_all();

  /** Execute callback for each connection in the pool. */
  void for_each_connection(void (*f)(MYSQL *mysql));

  /**
    Closes all connections in pool and frees all resources.
    Does not wait for pending queries to complete
    (use wait_all() for that)
  */
  int close();
};

} // namespace async_pool
