
// vim:sw=2:ai

/*
 * Copyright (C) 2010-2011 DeNA Co.,Ltd.. All rights reserved.
 * Copyright (C) 2011-2017 Kentoku SHIBA
 * See COPYRIGHT.txt for details.
 */

#include <my_global.h>
#include <my_config.h>
#ifndef __WIN__
#include <sys/types.h>
#include <sys/un.h>
#endif

#include "mysql_version.h"
#include "hs_compat.h"
#if MYSQL_VERSION_ID < 50500
#include "mysql_priv.h"
#include <mysql/plugin.h>
#else
#if defined(MARIADB_BASE_VERSION) && MYSQL_VERSION_ID >= 100000
#include <my_global.h>
#endif
#include "sql_priv.h"
#include "probes_mysql.h"
#endif

#include "socket.hpp"
#include "string_util.hpp"
#include "fatal.hpp"

/*
struct sockaddr_un {
  short sun_family;
  char  sun_path[108];
};
*/

namespace dena {

void
ignore_sigpipe()
{
#if defined(SIGPIPE) && !defined(__WIN__)
  if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
    fatal_abort("SIGPIPE SIG_IGN");
  }
#endif
}

void
socket_args::set(const config& conf)
{
  timeout = (int) conf.get_int("timeout", 600);
  listen_backlog = (int) conf.get_int("listen_backlog", 256);
  String node = conf.get_str("host", "");
  String port = conf.get_str("port", "");
  if (node.length() || port.length()) {
    if (family == AF_UNIX || !strcmp(node.c_ptr(), "/")) {
      set_unix_domain(port.c_ptr());
    } else {
      const char *nd = !node.length() ? 0 : node.c_ptr();
      if (resolve(nd, port.c_ptr()) != 0) {
        String message(STRING_WITH_LEN("getaddrinfo failed: "),
                       &my_charset_bin);
        message.reserve(node.length() + sizeof(":") - 1 + port.length());
        message.append(node);
        message.q_append(":", sizeof(":") - 1);
        message.append(port);
        fatal_abort(message);
      }
    }
  }
  sndbuf = (int) conf.get_int("sndbuf", 0);
  rcvbuf = (int) conf.get_int("rcvbuf", 0);
}

void
socket_args::set_unix_domain(const char *path)
{
#ifndef __WIN__
  family = AF_UNIX; 
  addr = sockaddr_storage();
  addrlen = sizeof(sockaddr_un);
  sockaddr_un *const ap = reinterpret_cast<sockaddr_un *>(&addr);
  ap->sun_family = AF_UNIX;
  strncpy(ap->sun_path, path, sizeof(ap->sun_path) - 1);
#endif
}

int
socket_args::resolve(const char *node, const char *service)
{
  const int flags = (node == 0) ? AI_PASSIVE : 0;
  auto_addrinfo ai;
  addr = sockaddr_storage();
  addrlen = 0;
  const int r = ai.resolve(node, service, flags, family, socktype, protocol);
  if (r != 0) {
    return r;
  }
  memcpy(&addr, ai.get()->ai_addr, ai.get()->ai_addrlen);
  addrlen = ai.get()->ai_addrlen;
  return 0;
}

int
socket_set_timeout(auto_file& fd, const socket_args& args, String& err_r)
{
  if (!args.nonblocking) {
#if defined(SO_SNDTIMEO) && defined(SO_RCVTIMEO)
    if (args.recv_timeout != 0) {
#ifndef __WIN__
      struct timeval tv;
      tv.tv_sec = args.recv_timeout;
      tv.tv_usec = 0;
#else
      int tv = args.recv_timeout * 1000;
#endif
      if (setsockopt(fd.get(), SOL_SOCKET, SO_RCVTIMEO,
#ifndef __WIN__
          (const void *) &tv,
#else
          (const char *) &tv,
#endif
          sizeof(tv)) != 0) {
        return errno_string("setsockopt SO_RCVTIMEO", errno, err_r);
      }
    }
    if (args.send_timeout != 0) {
#ifndef __WIN__
      struct timeval tv;
      tv.tv_sec = args.send_timeout;
      tv.tv_usec = 0;
#else
      int tv = args.send_timeout * 1000;
#endif
      if (setsockopt(fd.get(), SOL_SOCKET, SO_SNDTIMEO,
#ifndef __WIN__
          (const void *) &tv,
#else
          (const char *) &tv,
#endif
          sizeof(tv)) != 0) {
        return errno_string("setsockopt SO_SNDTIMEO", errno, err_r);
      }
    }
#endif
  }
  return 0;
}

int
socket_set_options(auto_file& fd, const socket_args& args, String& err_r)
{
  if (args.timeout != 0 && !args.nonblocking) {
#if defined(SO_SNDTIMEO) && defined(SO_RCVTIMEO)
#ifndef __WIN__
    struct timeval tv;
    tv.tv_sec = args.timeout;
    tv.tv_usec = 0;
#else
    int tv = args.timeout * 1000;
#endif
    if (setsockopt(fd.get(), SOL_SOCKET, SO_RCVTIMEO,
#ifndef __WIN__
        (const void *) &tv,
#else
        (const char *) &tv,
#endif
        sizeof(tv)) != 0) {
      return errno_string("setsockopt SO_RCVTIMEO", errno, err_r);
    }
#ifndef __WIN__
    tv.tv_sec = args.timeout;
    tv.tv_usec = 0;
#else
    tv = args.timeout * 1000;
#endif
    if (setsockopt(fd.get(), SOL_SOCKET, SO_SNDTIMEO,
#ifndef __WIN__
        (const void *) &tv,
#else
        (const char *) &tv,
#endif
        sizeof(tv)) != 0) {
      return errno_string("setsockopt SO_RCVTIMEO", errno, err_r);
    }
#endif
  }
#ifndef __WIN__
  if (args.nonblocking && fcntl(fd.get(), F_SETFL, O_NONBLOCK) != 0) {
    return errno_string("fcntl O_NONBLOCK", errno, err_r);
  }
#endif
  if (args.sndbuf != 0) {
    const int v = args.sndbuf;
    if (setsockopt(fd.get(), SOL_SOCKET, SO_SNDBUF,
#ifndef __WIN__
        (const void *) &v,
#else
        (const char *) &v,
#endif
        sizeof(v)) != 0) {
      return errno_string("setsockopt SO_SNDBUF", errno, err_r);
    }
  }
  if (args.rcvbuf != 0) {
    const int v = args.rcvbuf;
    if (setsockopt(fd.get(), SOL_SOCKET, SO_RCVBUF,
#ifndef __WIN__
        (const void *) &v,
#else
        (const char *) &v,
#endif
        sizeof(v)) != 0) {
      return errno_string("setsockopt SO_RCVBUF", errno, err_r);
    }
  }
  return 0;
}

int
socket_open(auto_file& fd, const socket_args& args, String& err_r)
{
  fd.reset((int) socket(args.family, args.socktype, args.protocol));
  if (fd.get() < 0) {
    return errno_string("socket", errno, err_r);
  }
  return socket_set_options(fd, args, err_r);
}

int
socket_connect(auto_file& fd, const socket_args& args, String& err_r)
{
  int r = 0;
  if ((r = socket_open(fd, args, err_r)) != 0) {
    return r;
  }
  if (connect(fd.get(), reinterpret_cast<const sockaddr *>(&args.addr),
    args.addrlen) != 0) {
    if (!args.nonblocking
#ifndef __WIN__
      || errno != EINPROGRESS
#endif
    ) {
      return errno_string("connect", errno, err_r);
    }
  }
  return 0;
}

int
socket_bind(auto_file& fd, const socket_args& args, String& err_r)
{
  fd.reset((int) socket(args.family, args.socktype, args.protocol));
  if (fd.get() < 0) {
    return errno_string("socket", errno, err_r);
  }
  if (args.reuseaddr) {
#ifndef __WIN__
    if (args.family == AF_UNIX) {
      const sockaddr_un *const ap =
        reinterpret_cast<const sockaddr_un *>(&args.addr);
      if (unlink(ap->sun_path) != 0 && errno != ENOENT) {
        return errno_string("unlink uds", errno, err_r);
      }
    } else {
#endif
      int v = 1;
      if (setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR,
#ifndef __WIN__
        (const void *) &v,
#else
        (const char *) &v,
#endif
        sizeof(v)) != 0) {
        return errno_string("setsockopt SO_REUSEADDR", errno, err_r);
      }
#ifndef __WIN__
    }
#endif
  }
  if (bind(fd.get(), reinterpret_cast<const sockaddr *>(&args.addr),
    args.addrlen) != 0) {
    return errno_string("bind", errno, err_r);
  }
  if (listen(fd.get(), args.listen_backlog) != 0) {
    return errno_string("listen", errno, err_r);
  }
#ifndef __WIN__
  if (args.nonblocking && fcntl(fd.get(), F_SETFL, O_NONBLOCK) != 0) {
    return errno_string("fcntl O_NONBLOCK", errno, err_r);
  }
#endif
  return 0;
}

int
socket_accept(int listen_fd, auto_file& fd, const socket_args& args,
  sockaddr_storage& addr_r, socklen_t& addrlen_r, String& err_r)
{
  fd.reset((int) accept(listen_fd, reinterpret_cast<sockaddr *>(&addr_r),
                        &addrlen_r));
  if (fd.get() < 0) {
    return errno_string("accept", errno, err_r);
  }
  return socket_set_options(fd, args, err_r);
}

};

