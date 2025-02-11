/* Copyright 2010-2015 Codership Oy <http://www.codership.com>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA
 */

//! @file some utility functions and classes not directly related to replication

#ifndef _GNU_SOURCE
#define _GNU_SOURCE // POSIX_SPAWN_USEVFORK flag
#endif

#include "mariadb.h"
#include "my_global.h"
#include "wsrep_api.h"
#include "wsrep_utils.h"
#include "wsrep_mysqld.h"
#include "wsrep_thd.h"

#include <sql_class.h>

#include <spawn.h>    // posix_spawn()
#include <unistd.h>   // pipe()
#include <errno.h>    // errno
#include <string.h>   // strerror()
#include <sys/wait.h> // waitpid()
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>    // getaddrinfo()

#ifdef HAVE_GETIFADDRS
#include <net/if.h>
#include <ifaddrs.h>
#endif /* HAVE_GETIFADDRS */

#define MY_MALLOC(arg) my_malloc(key_memory_WSREP, arg, MYF(0))
#define MY_REALLOC(p, arg) my_realloc(key_memory_WSREP, p, arg, MYF(MY_ALLOW_ZERO_PTR))
#define MY_STRDUP(arg) my_strdup(key_memory_WSREP, arg, MYF(0))
#define MY_FREE(arg) my_free(arg)

extern char** environ; // environment variables

static wsp::string wsrep_PATH;

void
wsrep_prepend_PATH (const char* path)
{
    int count= 0;

    while (environ[count])
    {
        if (strncmp (environ[count], "PATH=", 5))
        {
            count++;
            continue;
        }

        char* const old_path (environ[count]);

        if (strstr (old_path, path)) return; // path already there

        size_t const new_path_len(strlen(old_path) + strlen(":") +
                                  strlen(path) + 1);

        char* const new_path (static_cast<char*>(malloc(new_path_len)));

        if (new_path)
        {
            snprintf (new_path, new_path_len, "PATH=%s:%s", path,
                      old_path + strlen("PATH="));

            wsrep_PATH.set (new_path);
            environ[count]= new_path;
        }
        else
        {
            WSREP_ERROR ("Failed to allocate 'PATH' environment variable "
                         "buffer of size %zu.", new_path_len);
        }

        return;
    }

    WSREP_ERROR ("Failed to find 'PATH' environment variable. "
                 "State snapshot transfer may not be working.");
}

namespace wsp
{

bool
env::ctor_common(char** e)
{
    env_= static_cast<char**>(MY_MALLOC((len_ + 1) * sizeof(char*)));

    if (env_)
    {
        for (size_t i(0); i < len_; ++i)
        {
            assert(e[i]); // caller should make sure about len_
            env_[i]= MY_STRDUP(e[i]);
            if (!env_[i])
            {
                errno_= errno;
                WSREP_ERROR("Failed to allocate env. var: %s", e[i]);
                return true;
            }
        }

        env_[len_]= NULL;
        return false;
    }
    else
    {
        errno_= errno;
        WSREP_ERROR("Failed to allocate env. var vector of length: %zu", len_);
        return true;
    }
}

void
env::dtor()
{
    if (env_)
    {
        /* don't need to go beyond the first NULL */
        for (size_t i(0); env_[i] != NULL; ++i) { MY_FREE(env_[i]); }
        MY_FREE(env_);
        env_= NULL;
    }
    len_= 0;
}

env::env(char** e)
    : len_(0), env_(NULL), errno_(0)
{
    if (!e) { e= environ; }
    /* count the size of the vector */
    while (e[len_]) { ++len_; }

    if (ctor_common(e)) dtor();
}

env::env(const env& e)
    : len_(e.len_), env_(0), errno_(0)
{
    if (ctor_common(e.env_)) dtor();
}

env::~env() { dtor(); }

int
env::append(const char* val)
{
    char** tmp= static_cast<char**>(MY_REALLOC(env_, (len_ + 2)*sizeof(char*)));

    if (tmp)
    {
        env_= tmp;
        env_[len_]= MY_STRDUP(val);

        if (env_[len_])
        {
            ++len_;
            env_[len_]= NULL;
        }
        else errno_= errno;
    }
    else errno_= errno;

    return errno_;
}

#define READ_END  0
#define WRITE_END 1
#define STDIN_FD  0
#define STDOUT_FD 1

#ifndef POSIX_SPAWN_USEVFORK
# define POSIX_SPAWN_USEVFORK 0
#endif

static int
add_file_actions(posix_spawn_file_actions_t *fact,
                 int close_fd, int dup_fd, int unused_fd)
{
    // close child's stdout|stdin fd
    int err= posix_spawn_file_actions_addclose(fact, close_fd);
    if (err)
    {
        WSREP_ERROR ("posix_spawn_file_actions_addclose() failed: %d (%s)",
                   err, strerror(err));
        return err;
    }

    // substitute our pipe descriptor in place of the closed one
    err= posix_spawn_file_actions_adddup2(fact, dup_fd, close_fd);
    if (err)
    {
        WSREP_ERROR ("posix_spawn_file_actions_addup2() failed: %d (%s)",
                     err, strerror(err));
        return err;
    }

    // close unused end of the pipe
    err= posix_spawn_file_actions_addclose(fact, unused_fd);
    if (err)
    {
        WSREP_ERROR ("posix_spawn_file_actions_addclose(2) failed: %d (%s)",
                     err, strerror(err));
        return err;
    }

    return 0;
}

void
process::setup_parent_pipe_end(io_direction      direction,
                               int               pipe_fds[],
                               int const         pipe_end,
                               const char* const mode)
{
    io_[direction] = fdopen(pipe_fds[pipe_end], mode);

    if (io_[direction])
    {
        pipe_fds[pipe_end]= -1; // skip close on cleanup
    }
    else
    {
        err_= errno;
        WSREP_ERROR("fdopen() failed on '%s' pipe: %d (%s)",
                    mode, err_, strerror(err_));
    }
}

void
process::close_io(io_direction const direction, bool const warn)
{
    if (io_[direction])
    {
        if (warn)
        {
            WSREP_WARN("Closing pipe to child process: %s, PID(%ld) "
                       "which might still be running.", str_, (long)pid_);
        }

        if (fclose(io_[direction]) == -1)
        {
            err_= errno;
            WSREP_ERROR("fclose(%d) failed: %d (%s)",
                        direction, err_, strerror(err_));
        }

        io_[direction]= NULL;
    }
}

process::process (const char* cmd, const char* type, char** env)
    :
    str_(cmd ? MY_STRDUP(cmd) : MY_STRDUP("")),
    io_{ NULL, NULL },
    err_(EINVAL),
    pid_(0)
{
    if (0 == str_)
    {
        WSREP_ERROR ("Can't allocate command line of size: %zu", strlen(cmd));
        err_= ENOMEM;
        return;
    }

    if (0 == strlen(str_))
    {
        WSREP_ERROR ("Can't start a process: null or empty command line.");
        return;
    }

    if (NULL == type ||
        (strncmp(type, "w", 1) && strncmp(type, "r", 1) && strncmp(type, "rw", 2)))
    {
        WSREP_ERROR ("type argument should be either \"r\", \"w\" or \"rw\".");
        return;
    }

    if (NULL == env) { env= environ; } // default to global environment

    bool const read_from_child= strchr(type, 'r');
    bool const write_to_child= strchr(type, 'w');

    int read_pipe[2]=  { -1, -1 };
    int write_pipe[2]= { -1, -1 };

    char* const pargv[4]= { MY_STRDUP("sh"), MY_STRDUP("-c"), MY_STRDUP(str_), NULL };
    if (!(pargv[0] && pargv[1] && pargv[2]))
    {
        err_= ENOMEM;
        WSREP_ERROR ("Failed to allocate pargv[] array.");
        goto cleanup_pargv;
    }

    if (read_from_child && ::pipe(read_pipe))
    {
        err_= errno;
        WSREP_ERROR ("pipe(read_pipe) failed: %d (%s)", err_, strerror(err_));
        goto cleanup_pargv;
    }

    if (write_to_child && ::pipe(write_pipe))
    {
        err_= errno;
        WSREP_ERROR ("pipe(write_pipe) failed: %d (%s)", err_, strerror(err_));
        goto cleanup_pipes;
    }

    posix_spawnattr_t attr;
    err_= posix_spawnattr_init (&attr);
    if (err_)
    {
        WSREP_ERROR ("posix_spawnattr_init() failed: %d (%s)",
                     err_, strerror(err_));
        goto cleanup_pipes;
    }

    /* make sure that no signlas are masked in child process */
    sigset_t sigmask_empty; sigemptyset(&sigmask_empty);
    err_= posix_spawnattr_setsigmask(&attr, &sigmask_empty);
    if (err_)
    {
        WSREP_ERROR ("posix_spawnattr_setsigmask() failed: %d (%s)",
                     err_, strerror(err_));
        goto cleanup_attr;
    }

    /* make sure the following signals are not ignored in child process */
    sigset_t default_signals; sigemptyset(&default_signals);
    sigaddset(&default_signals, SIGHUP);
    sigaddset(&default_signals, SIGINT);
    sigaddset(&default_signals, SIGQUIT);
    sigaddset(&default_signals, SIGPIPE);
    sigaddset(&default_signals, SIGTERM);
    sigaddset(&default_signals, SIGCHLD);
    err_= posix_spawnattr_setsigdefault(&attr, &default_signals);
    if (err_)
    {
        WSREP_ERROR ("posix_spawnattr_setsigdefault() failed: %d (%s)",
                     err_, strerror(err_));
        goto cleanup_attr;
    }

    err_= posix_spawnattr_setflags (&attr, POSIX_SPAWN_SETSIGDEF  |
                                           POSIX_SPAWN_SETSIGMASK |
                                           POSIX_SPAWN_USEVFORK);
    if (err_)
    {
        WSREP_ERROR ("posix_spawnattr_setflags() failed: %d (%s)",
                     err_, strerror(err_));
        goto cleanup_attr;
    }

    posix_spawn_file_actions_t fact;
    err_= posix_spawn_file_actions_init (&fact);
    if (err_)
    {
        WSREP_ERROR ("posix_spawn_file_actions_init() failed: %d (%s)",
                     err_, strerror(err_));
        goto cleanup_attr;
    }

    /* Add file actions for the child (fd substitution, close unused fds) */
    if (read_from_child)
    {
        err_= add_file_actions(&fact, STDOUT_FD, read_pipe[WRITE_END],
                               read_pipe[READ_END]);
        if (err_) goto cleanup_fact;
    }

    if (write_to_child)
    {
        err_= add_file_actions(&fact, STDIN_FD, write_pipe[READ_END],
                               write_pipe[WRITE_END]);
        if (err_) goto cleanup_fact;
    }

    err_= posix_spawnp (&pid_, pargv[0], &fact, &attr, pargv, env);
    if (err_)
    {
        WSREP_ERROR ("posix_spawnp(%s) failed: %d (%s)",
                     pargv[2], err_, strerror(err_));
        pid_= 0; // just to make sure it was not messed up in the call
        goto cleanup_fact;
    }

    if (read_from_child)
    {
        setup_parent_pipe_end(READ, read_pipe, READ_END, "r");
        assert(from());
    }

    if (write_to_child)
    {
        setup_parent_pipe_end(WRITE, write_pipe, WRITE_END, "w");
        assert(to());
    }

cleanup_fact:
    int err; // to preserve err_ code
    err= posix_spawn_file_actions_destroy (&fact);
    if (err)
    {
        WSREP_ERROR ("posix_spawn_file_actions_destroy() failed: %d (%s)\n",
                     err, strerror(err));
    }

cleanup_attr:
    err= posix_spawnattr_destroy (&attr);
    if (err)
    {
        WSREP_ERROR ("posix_spawnattr_destroy() failed: %d (%s)",
                     err, strerror(err));
    }

cleanup_pipes:
    if (read_pipe[0] >= 0) close (read_pipe[0]);
    if (read_pipe[1] >= 0) close (read_pipe[1]);
    if (write_pipe[0] >= 0) close (write_pipe[0]);
    if (write_pipe[1] >= 0) close (write_pipe[1]);

cleanup_pargv:
    MY_FREE(pargv[0]);
    MY_FREE(pargv[1]);
    MY_FREE(pargv[2]);
}

process::~process ()
{
    close_io(READ, true);
    close_io(WRITE, true);

    if (str_) MY_FREE(const_cast<char*>(str_));
}

int
process::wait ()
{
  if (pid_)
  {
      int status;
      if (-1 == waitpid(pid_, &status, 0))
      {
          err_= errno; assert (err_);
          WSREP_ERROR("Waiting for process failed: %s, PID(%ld): %d (%s)",
                      str_, (long)pid_, err_, strerror (err_));
      }
      else
      {                // command completed, check exit status
          if (WIFEXITED (status)) {
              err_= WEXITSTATUS (status);
          }
          else {       // command didn't complete with exit()
              WSREP_ERROR("Process was aborted.");
              err_= errno ? errno : ECHILD;
          }

          if (err_) {
              switch (err_) /* Translate error codes to more meaningful */
              {
              case 126: err_= EACCES; break; /* Permission denied */
              case 127: err_= ENOENT; break; /* No such file or directory */
              case 143: err_= EINTR;  break; /* Subprocess killed */
              }
              WSREP_ERROR("Process completed with error: %s: %d (%s)",
                          str_, err_, strerror(err_));
          }

          pid_= 0;
          close_io(READ,  false);
          close_io(WRITE, false);
       }
  }
  else {
      assert (NULL == io_[READ]);
      assert (NULL == io_[WRITE]);
      WSREP_ERROR("Command did not run: %s", str_);
  }

  return err_;
}

thd::thd (my_bool ini, bool system_thread)
    :
    init(ini),
    ptr(init.err_ ? nullptr : new THD(0))
{
  if (ptr)
  {
    ptr->real_id= pthread_self();
    wsrep_assign_from_threadvars(ptr);
    wsrep_store_threadvars(ptr);

    ptr->variables.tx_isolation= ISO_READ_COMMITTED;
    ptr->variables.sql_log_bin = 0;
    ptr->variables.option_bits &= ~OPTION_BIN_LOG; // disable binlog
    ptr->variables.option_bits |=  OPTION_LOG_OFF; // disable general log
    ptr->variables.wsrep_on = false;
    ptr->security_context()->skip_grants();

    if (system_thread)
    {
      ptr->system_thread= SYSTEM_THREAD_GENERIC;
    }
    ptr->security_ctx->master_access= ALL_KNOWN_ACL;
    lex_start(ptr);
  }
}

thd::~thd ()
{
  if (ptr)
  {
    delete ptr;
    set_current_thd(nullptr);
  }
}

mysql::mysql() :
  mysql_(mysql_init(NULL))
{
  int err = 0;
  if (mysql_real_connect_local(mysql_) == NULL) {
      err = mysql_errno(mysql_);
      WSREP_ERROR("mysql::mysql() mysql_real_connect() failed: %d (%s)",
                  err, mysql_error(mysql_));
  }
}

mysql::~mysql()
{
  mysql_close(mysql_);
}

int
mysql::disable_replication()
{
  int err = execute("SET SESSION sql_log_bin = OFF;");
  if (err) {
    WSREP_ERROR("sst_user::user() disabling log_bin failed: %d (%s)",
                err, errstr());
  }
  else {
    err = execute("SET SESSION wsrep_on = OFF;");
    if (err) {
      WSREP_ERROR("sst_user::user() disabling wsrep replication failed: %d (%s)",
                  err, errstr());
    }
  }
  return err;
}

} // namespace wsp

/* Returns INADDR_NONE, INADDR_ANY, INADDR_LOOPBACK or something else */
unsigned int wsrep_check_ip (const char* const addr, bool *is_ipv6)
{
  unsigned int ret= INADDR_NONE;
  struct addrinfo *res, hints;

  memset (&hints, 0, sizeof(hints));
  hints.ai_flags= AI_PASSIVE/*|AI_ADDRCONFIG*/;
  hints.ai_socktype= SOCK_STREAM;
  hints.ai_family= AF_UNSPEC;

  *is_ipv6= false;

  char *end;
  char address[INET6_ADDRSTRLEN];

  end= strcend(addr, ',');
  strmake(address, addr, (uint) (end - addr));

  int gai_ret= getaddrinfo(address, NULL, &hints, &res);
  if (0 == gai_ret)
  {
    if (AF_INET == res->ai_family) /* IPv4 */
    {
      struct sockaddr_in* a= (struct sockaddr_in*)res->ai_addr;
      ret= htonl(a->sin_addr.s_addr);
    }
    else /* IPv6 */
    {
      struct sockaddr_in6* a= (struct sockaddr_in6*)res->ai_addr;
      if (IN6_IS_ADDR_UNSPECIFIED(&a->sin6_addr))
        ret= INADDR_ANY;
      else if (IN6_IS_ADDR_LOOPBACK(&a->sin6_addr))
        ret= INADDR_LOOPBACK;
      else
        ret= 0xdeadbeef;

      *is_ipv6= true;
    }
    freeaddrinfo (res);
  }
  else {
    WSREP_ERROR ("getaddrinfo() failed on '%s': %d (%s)",
                 addr, gai_ret, gai_strerror(gai_ret));
  }

  return ret;
}

extern char* my_bind_addr_str;

size_t wsrep_guess_ip (char* buf, size_t buf_len)
{
  size_t ret= 0;

  // Attempt 1: Try to get the IP from bind-address.
  // Skip if empty or bind-address=*
  if (my_bind_addr_str && my_bind_addr_str[0] != '\0' &&
      strcmp(my_bind_addr_str, "*") != 0)
  {
    bool unused;
    unsigned int const ip_type= wsrep_check_ip(my_bind_addr_str, &unused);

    if (INADDR_NONE == ip_type) {
      WSREP_ERROR("Networking not configured, cannot receive state "
                  "transfer.");
      ret= 0;
      goto done;
    } else if (INADDR_ANY != ip_type) {
      strncpy (buf, my_bind_addr_str, buf_len);
      ret= strlen(buf);
      goto done;
    }
  }

  // Attempt 2: mysqld binds to all interfaces, try IP from wsrep_node_address.
  if (wsrep_node_address && wsrep_node_address[0] != '\0') {
    wsp::Address addr(wsrep_node_address);
    if (!addr.is_valid())
    {
      WSREP_WARN("Could not parse wsrep_node_address : %s",
                 wsrep_node_address);
      ret= 0;
      goto done;
    }

    /* Safety check: Buffer length should be sufficiently large. */
    DBUG_ASSERT(buf_len >= addr.get_address_len());

    memcpy(buf, addr.get_address(), addr.get_address_len());
    ret= addr.get_address_len();
    goto done;
  }

  /*
    Attempt 3: Try to get the IP from the list of available interfaces.

    getifaddrs() is avaiable at least on Linux since glib 2.3, FreeBSD,
    MAC OSX, OpenSolaris, Solaris.

    On platforms which do not support getifaddrs() this function returns
    a failure and user is prompted to do manual configuration.
  */
#if HAVE_GETIFADDRS
  struct ifaddrs *ifaddr, *ifa;
  int family;

  if (getifaddrs(&ifaddr) == 0)
  {
    for (ifa= ifaddr; ifa != NULL; ifa= ifa->ifa_next)
    {
      if (!ifa->ifa_addr)
        continue;

      family= ifa->ifa_addr->sa_family;

      if ((family != AF_INET) && (family != AF_INET6))
        continue;

      // Skip loopback interfaces (like lo:127.0.0.1)
      if (ifa->ifa_flags & IFF_LOOPBACK)
        continue;

      /*
        Get IP address from the socket address. The resulting address may have
        zone ID appended for IPv6 addresses (<address>%<zone-id>).
      */
      if (vio_getnameinfo(ifa->ifa_addr, buf, buf_len, NULL, 0, NI_NUMERICHOST))
        continue;

      freeifaddrs(ifaddr);

      ret= strlen(buf);
      goto done;
    }
    freeifaddrs(ifaddr);
  }
#endif /* HAVE_GETIFADDRS */

done:
  WSREP_DEBUG("wsrep_guess_ip() : %s", (ret > 0) ? buf : "????");
  return ret;
}

/* returns the length of the host part of the address string */
size_t wsrep_host_len(const char* const addr, size_t const addr_len)
{
  // check for IPv6 notation first
  const char* const bracket= ('[' == addr[0] ? strchr(addr, ']') : NULL);

  if (bracket) { // IPv6
    return (bracket - addr + 1);
  }
  else { // host part ends at ':' or end of string
    const char* const colon= strchr(addr, ':');
    return (colon ? colon - addr : addr_len);
  }
}
