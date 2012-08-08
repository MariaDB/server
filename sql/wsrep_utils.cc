/* Copyright 2010 Codership Oy <http://www.codership.com>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

//! @file declares symbols private to wsrep integration layer

#ifndef _GNU_SOURCE
#define _GNU_SOURCE // POSIX_SPAWN_USEVFORK flag
#endif

#include <spawn.h>    // posix_spawn()
#include <unistd.h>   // pipe()
#include <errno.h>    // errno
#include <string.h>   // strerror()
#include <sys/wait.h> // waitpid()

#include <sql_class.h>
#include "wsrep_priv.h"

extern char** environ; // environment variables

static wsp::string wsrep_PATH;

void
wsrep_prepend_PATH (const char* path)
{
    int count = 0;

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

        char* const new_path (reinterpret_cast<char*>(malloc(new_path_len)));

        if (new_path)
        {
            snprintf (new_path, new_path_len, "PATH=%s:%s", path,
                      old_path + strlen("PATH="));

            wsrep_PATH.set (new_path);
            environ[count] = new_path;
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

#define PIPE_READ  0
#define PIPE_WRITE 1
#define STDIN_FD   0
#define STDOUT_FD  1

#ifndef POSIX_SPAWN_USEVFORK
# define POSIX_SPAWN_USEVFORK 0
#endif

process::process (const char* cmd, const char* type)
    : str_(cmd ? strdup(cmd) : strdup("")), io_(NULL), err_(EINVAL), pid_(0)
{
    if (0 == str_)
    {
        WSREP_ERROR ("Can't allocate command line of size: %zu", strlen(cmd));
        err_ = ENOMEM;
        return;
    }

    if (0 == strlen(str_))
    {
        WSREP_ERROR ("Can't start a process: null or empty command line.");
        return;
    }

    if (NULL == type || (strcmp (type, "w") && strcmp(type, "r")))
    {
        WSREP_ERROR ("type argument should be either \"r\" or \"w\".");
        return;
    }

    int pipe_fds[2] = { -1, };
    if (::pipe(pipe_fds))
    {
        err_ = errno;
        WSREP_ERROR ("pipe() failed: %d (%s)", err_, strerror(err_));
        return;
    }

    // which end of pipe will be returned to parent
    int const parent_end (strcmp(type,"w") ? PIPE_READ : PIPE_WRITE);
    int const child_end  (parent_end == PIPE_READ ? PIPE_WRITE : PIPE_READ);
    int const close_fd   (parent_end == PIPE_READ ? STDOUT_FD : STDIN_FD);

    char* const pargv[4] = { strdup("sh"), strdup("-c"), strdup(str_), NULL };
    if (!(pargv[0] && pargv[1] && pargv[2]))
    {
        err_ = ENOMEM;
        WSREP_ERROR ("Failed to allocate pargv[] array.");
        goto cleanup_pipe;
    }

    posix_spawnattr_t attr;
    err_ = posix_spawnattr_init (&attr);
    if (err_)
    {
        WSREP_ERROR ("posix_spawnattr_init() failed: %d (%s)",
                     err_, strerror(err_));
        goto cleanup_pipe;
    }

    err_ = posix_spawnattr_setflags (&attr, POSIX_SPAWN_SETSIGDEF |
                                            POSIX_SPAWN_USEVFORK);
    if (err_)
    {
        WSREP_ERROR ("posix_spawnattr_setflags() failed: %d (%s)",
                     err_, strerror(err_));
        goto cleanup_attr;
    }

    posix_spawn_file_actions_t fact;
    err_ = posix_spawn_file_actions_init (&fact);
    if (err_)
    {
        WSREP_ERROR ("posix_spawn_file_actions_init() failed: %d (%s)",
                     err_, strerror(err_));
        goto cleanup_attr;
    }

    // close child's stdout|stdin depending on what we returning
    err_ = posix_spawn_file_actions_addclose (&fact, close_fd);
    if (err_)
    {
        WSREP_ERROR ("posix_spawn_file_actions_addclose() failed: %d (%s)",
                     err_, strerror(err_));
        goto cleanup_fact;
    }

    // substitute our pipe descriptor in place of the closed one
    err_ = posix_spawn_file_actions_adddup2 (&fact,
                                             pipe_fds[child_end], close_fd);
    if (err_)
    {
        WSREP_ERROR ("posix_spawn_file_actions_addup2() failed: %d (%s)",
                     err_, strerror(err_));
        goto cleanup_fact;
    }

    err_ = posix_spawnp (&pid_, pargv[0], &fact, &attr, pargv, environ);
    if (err_)
    {
        WSREP_ERROR ("posix_spawnp(%s) failed: %d (%s)",
                     pargv[2], err_, strerror(err_));
        pid_ = 0; // just to make sure it was not messed up in the call
        goto cleanup_fact;
    }

    io_ = fdopen (pipe_fds[parent_end], type);

    if (io_)
    {
        pipe_fds[parent_end] = -1; // skip close on cleanup
    }
    else
    {
        err_ = errno;
        WSREP_ERROR ("fdopen() failed: %d (%s)", err_, strerror(err_));
    }

cleanup_fact:
    int err; // to preserve err_ code
    err = posix_spawn_file_actions_destroy (&fact);
    if (err)
    {
        WSREP_ERROR ("posix_spawn_file_actions_destroy() failed: %d (%s)\n",
                     err, strerror(err));
    }

cleanup_attr:
    err = posix_spawnattr_destroy (&attr);
    if (err)
    {
        WSREP_ERROR ("posix_spawnattr_destroy() failed: %d (%s)",
                     err, strerror(err));
    }

cleanup_pipe:
    if (pipe_fds[0] >= 0) close (pipe_fds[0]);
    if (pipe_fds[1] >= 0) close (pipe_fds[1]);

    free (pargv[0]);
    free (pargv[1]);
    free (pargv[2]);
}

process::~process ()
{
    if (io_)
    {
        assert (pid_);
        assert (str_);

        WSREP_WARN("Closing pipe to child process: %s, PID(%ld) "
                   "which might still be running.", str_, (long)pid_);

        if (fclose (io_) == -1)
        {
            err_ = errno;
            WSREP_ERROR("fclose() failed: %d (%s)", err_, strerror(err_));
        }
    }

    if (str_) free (const_cast<char*>(str_));
}

int
process::wait ()
{
  if (pid_)
  {
      int status;
      if (-1 == waitpid(pid_, &status, 0))
      {
          err_ = errno; assert (err_);
          WSREP_ERROR("Waiting for process failed: %s, PID(%ld): %d (%s)",
                      str_, (long)pid_, err_, strerror (err_));
      }
      else
      {                // command completed, check exit status
          if (WIFEXITED (status)) {
              err_ = WEXITSTATUS (status);
          }
          else {       // command didn't complete with exit()
              WSREP_ERROR("Process was aborted.");
              err_ = errno ? errno : ECHILD;
          }

          if (err_) {
              switch (err_) /* Translate error codes to more meaningful */
              {
              case 126: err_ = EACCES; break; /* Permission denied */
              case 127: err_ = ENOENT; break; /* No such file or directory */
              }
              WSREP_ERROR("Process completed with error: %s: %d (%s)",
                          str_, err_, strerror(err_));
          }

          pid_ = 0;
          if (io_) fclose (io_);
          io_ = NULL;
      }
  }
  else {
      assert (NULL == io_);
      WSREP_ERROR("Command did not run: %s", str_);
  }

  return err_;
}

thd::thd (my_bool won) : init(), ptr(new THD)
{
  if (ptr)
  {
    ptr->thread_stack= (char*) &ptr;
    ptr->store_globals();
    ptr->variables.option_bits&= ~OPTION_BIN_LOG; // disable binlog
    ptr->variables.wsrep_on = won;
    ptr->security_ctx->master_access= ~(ulong)0;
    lex_start(ptr);
  }
}

thd::~thd ()
{
  if (ptr)
  {
    delete ptr;
    my_pthread_setspecific_ptr (THR_THD, 0);
  }
}

} // namespace wsp

extern ulong my_bind_addr;
extern uint  mysqld_port;

size_t default_ip (char* buf, size_t buf_len)
{
  size_t ip_len = 0;

  if (htonl(INADDR_NONE) == my_bind_addr) {
    WSREP_ERROR("Networking not configured, cannot receive state transfer.");
    return 0;
  }

  if (htonl(INADDR_ANY) == my_bind_addr) {
    // binds to all interfaces, try to find the address of the first one
#if (TARGET_OS_LINUX == 1)
    const char cmd[] = "/sbin/ifconfig | "
        "grep -m1 -1 -E '^[a-z]?eth[0-9]' | tail -n 1 | "
        "awk '{ print $2 }' | awk -F : '{ print $2 }'";
#elif defined(__sun__)
    const char cmd[] = "/sbin/ifconfig -a | "
        "/usr/gnu/bin/grep -m1 -1 -E 'net[0-9]:' | tail -n 1 | awk '{ print $2 }'";
#else
    char *cmd;
#error "OS not supported"
#endif
    wsp::process proc (cmd, "r");

    if (NULL != proc.pipe()) {
      char* ret;

      ret = fgets (buf, buf_len, proc.pipe());

      if (proc.wait()) return 0;

      if (NULL == ret) {
        WSREP_ERROR("Failed to read output of: '%s'", cmd);
        return 0;
      }
    }
    else {
      WSREP_ERROR("Failed to execute: '%s'", cmd);
      return 0;
    }

    // clear possible \n at the end of ip string left by fgets()
    ip_len = strlen (buf);
    if (ip_len > 0 && '\n' == buf[ip_len - 1]) {
      ip_len--;
      buf[ip_len] = '\0';
    }

    if (INADDR_NONE == inet_addr(buf)) {
      if (strlen(buf) != 0) {
        WSREP_WARN("Shell command returned invalid address: '%s'", buf);
      }
      return 0;
    }
  }
  else {
    uint8_t* b = (uint8_t*)&my_bind_addr;
    ip_len = snprintf (buf, buf_len,
                       "%hhu.%hhu.%hhu.%hhu", b[0],b[1],b[2],b[3]);
  }

  return ip_len;
}

size_t default_address(char* buf, size_t buf_len)
{
  size_t addr_len = default_ip (buf, buf_len);

  if (addr_len && addr_len < buf_len) {
    addr_len += snprintf (buf + addr_len, buf_len - addr_len,
                          ":%u", mysqld_port);
  }

  return addr_len;
}

/*
 * WSREPXid
 */

#define WSREP_XID_PREFIX "WSREPXid"
#define WSREP_XID_PREFIX_LEN MYSQL_XID_PREFIX_LEN
#define WSREP_XID_UUID_OFFSET 8
#define WSREP_XID_SEQNO_OFFSET (WSREP_XID_UUID_OFFSET + sizeof(wsrep_uuid_t))
#define WSREP_XID_GTRID_LEN (WSREP_XID_SEQNO_OFFSET + sizeof(wsrep_seqno_t))

void wsrep_xid_init(XID* xid, const wsrep_uuid_t* uuid, wsrep_seqno_t seqno)
{
  xid->formatID= 1;
  xid->gtrid_length= WSREP_XID_GTRID_LEN;
  xid->bqual_length= 0;
  memset(xid->data, 0, sizeof(xid->data));
  memcpy(xid->data, WSREP_XID_PREFIX, WSREP_XID_PREFIX_LEN);
  memcpy(xid->data + WSREP_XID_UUID_OFFSET, uuid, sizeof(wsrep_uuid_t));
  memcpy(xid->data + WSREP_XID_SEQNO_OFFSET, &seqno, sizeof(wsrep_seqno_t));
}

const wsrep_uuid_t* wsrep_xid_uuid(const XID* xid)
{
  if (wsrep_is_wsrep_xid(xid))
    return reinterpret_cast<const wsrep_uuid_t*>(xid->data
                                                 + WSREP_XID_UUID_OFFSET);
  else
    return &WSREP_UUID_UNDEFINED;
}

wsrep_seqno_t wsrep_xid_seqno(const XID* xid)
{

  if (wsrep_is_wsrep_xid(xid))
  {
    wsrep_seqno_t seqno;
    memcpy(&seqno, xid->data + WSREP_XID_SEQNO_OFFSET, sizeof(wsrep_seqno_t));
    return seqno;
  }
  else
  {
    return WSREP_SEQNO_UNDEFINED;
  }
}

extern "C"
int wsrep_is_wsrep_xid(const void* xid_ptr)
{
  const XID* xid= reinterpret_cast<const XID*>(xid_ptr);
  return (xid->formatID      == 1                   &&
          xid->gtrid_length  == WSREP_XID_GTRID_LEN &&
          xid->bqual_length  == 0                   &&
          !memcmp(xid->data, WSREP_XID_PREFIX, WSREP_XID_PREFIX_LEN));
}
