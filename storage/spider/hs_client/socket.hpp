
// vim:sw=2:ai

/*
 * Copyright (C) 2010-2011 DeNA Co.,Ltd.. All rights reserved.
 * Copyright (C) 2011 Kentoku SHIBA
 * See COPYRIGHT.txt for details.
 */

#ifndef DENA_SOCKET_HPP
#define DENA_SOCKET_HPP

#include "mysql_version.h"
#if MYSQL_VERSION_ID < 50500
#include "mysql_priv.h"
#include <mysql/plugin.h>
#else
#include "sql_priv.h"
#include "probes_mysql.h"
#endif

#include "auto_addrinfo.hpp"
#include "auto_file.hpp"
#include "config.hpp"

namespace dena {

struct socket_args {
  sockaddr_storage addr;
  socklen_t addrlen;
  int family;
  int socktype;
  int protocol;
  int timeout;
  int send_timeout;
  int recv_timeout;
  int listen_backlog;
  bool reuseaddr;
  bool nonblocking;
  bool use_epoll;
  int sndbuf;
  int rcvbuf;
  socket_args() : addr(), addrlen(0), family(AF_INET), socktype(SOCK_STREAM),
    protocol(0), timeout(600), send_timeout(600), recv_timeout(600),
    listen_backlog(256), reuseaddr(true), nonblocking(false), use_epoll(false),
    sndbuf(0), rcvbuf(0) { }
  void set(const config& conf);
  void set_unix_domain(const char *path);
  int resolve(const char *node, const char *service);
};

void ignore_sigpipe();
int socket_set_timeout(auto_file& fd, const socket_args& args, String& err_r);
int socket_bind(auto_file& fd, const socket_args& args, String& err_r);
int socket_connect(auto_file& fd, const socket_args& args, String& err_r);
int socket_accept(int listen_fd, auto_file& fd, const socket_args& args,
  sockaddr_storage& addr_r, socklen_t& addrlen_r, String& err_r);

};

#endif

