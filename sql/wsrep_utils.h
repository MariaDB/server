/* Copyright (C) 2013 Codership Oy <info@codership.com>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License along
   with this program; if not, write to the Free Software Foundation, Inc.,
   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA. */

#ifndef WSREP_UTILS_H
#define WSREP_UTILS_H

#include "wsrep_priv.h"

unsigned int wsrep_check_ip (const char* addr);
size_t wsrep_guess_ip (char* buf, size_t buf_len);
size_t wsrep_guess_address(char* buf, size_t buf_len);

namespace wsp {
class node_status
{
public:
  node_status() : status(WSREP_MEMBER_UNDEFINED) {}
  void set(wsrep_member_status_t new_status,
           const wsrep_view_info_t* view = 0)
  {
    if (status != new_status || 0 != view)
    {
      wsrep_notify_status(new_status, view);
      status = new_status;
    }
  }
  wsrep_member_status_t get() const { return status; }
private:
  wsrep_member_status_t status;
};
} /* namespace wsp */

extern wsp::node_status local_status;

namespace wsp {
/* A small class to run external programs. */
class process
{
private:
    const char* const str_;
    FILE*       io_;
    int         err_;
    pid_t       pid_;

public:
/*! @arg type is a pointer to a null-terminated string which  must  contain
         either  the  letter  'r'  for  reading  or the letter 'w' for writing.
 */
    process  (const char* cmd, const char* type);
    ~process ();

    FILE* pipe () { return io_;  }
    int   error() { return err_; }
    int   wait ();
    const char* cmd() { return str_; }
};

class thd
{
  class thd_init
  {
  public:
    thd_init()  { my_thread_init(); }
    ~thd_init() { my_thread_end();  }
  }
  init;

  thd (const thd&);
  thd& operator= (const thd&);

public:

  thd(my_bool wsrep_on);
  ~thd();
  THD* const ptr;
};

class string
{
public:
    string() : string_(0) {}
    void set(char* str) { if (string_) free (string_); string_ = str; }
    ~string() { set (0); }
private:
    char* string_;
};

#ifdef REMOVED
class lock
{
  pthread_mutex_t* const mtx_;

public:

  lock (pthread_mutex_t* mtx) : mtx_(mtx)
  {
    int err = pthread_mutex_lock (mtx_);

    if (err)
    {
      WSREP_ERROR("Mutex lock failed: %s", strerror(err));
      abort();
    }
  }

  virtual ~lock ()
  {
    int err = pthread_mutex_unlock (mtx_);

    if (err)
    {
      WSREP_ERROR("Mutex unlock failed: %s", strerror(err));
      abort();
    }
  }

  inline void wait (pthread_cond_t* cond)
  {
    pthread_cond_wait (cond, mtx_);
  }

private:

  lock (const lock&);
  lock& operator=(const lock&);

};

class monitor
{
  int             mutable refcnt;
  pthread_mutex_t mutable mtx;
  pthread_cond_t  mutable cond;

public:

  monitor() : refcnt(0)
  {
    pthread_mutex_init (&mtx, NULL);
    pthread_cond_init  (&cond, NULL);
  }

  ~monitor()
  {
    pthread_mutex_destroy (&mtx);
    pthread_cond_destroy  (&cond);
  }

  void enter() const
  {
    lock l(&mtx);

    while (refcnt)
    {
      l.wait(&cond);
    }
    refcnt++;
  }

  void leave() const
  {
    lock l(&mtx);

    refcnt--;
    if (refcnt == 0)
    {
      pthread_cond_signal (&cond);
    }
  }

private:

  monitor (const monitor&);
  monitor& operator= (const monitor&);
};

class critical
{
  const monitor& mon;

public:

  critical(const monitor& m) : mon(m) { mon.enter(); }

  ~critical() { mon.leave(); }

private:

  critical (const critical&);
  critical& operator= (const critical&);
};
#endif

} // namespace wsrep

#endif /* WSREP_UTILS_H */
