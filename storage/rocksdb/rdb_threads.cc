/*
   Portions Copyright (c) 2015-Present, Facebook, Inc.
   Portions Copyright (c) 2012, Monty Program Ab

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

#ifdef USE_PRAGMA_IMPLEMENTATION
#pragma implementation // gcc: Class implementation
#endif

/* The C++ file's header */
#include "./rdb_threads.h"

namespace myrocks {

void *Rdb_thread::thread_func(void *const thread_ptr) {
  DBUG_ASSERT(thread_ptr != nullptr);
  Rdb_thread *const thread = static_cast<Rdb_thread *const>(thread_ptr);
  if (!thread->m_run_once.exchange(true)) {
    thread->run();
    thread->uninit();
  }
  return nullptr;
}

void Rdb_thread::init(
#ifdef HAVE_PSI_INTERFACE
    my_core::PSI_mutex_key stop_bg_psi_mutex_key,
    my_core::PSI_cond_key stop_bg_psi_cond_key
#endif
    ) {
  DBUG_ASSERT(!m_run_once);
  mysql_mutex_init(stop_bg_psi_mutex_key, &m_signal_mutex, MY_MUTEX_INIT_FAST);
  mysql_cond_init(stop_bg_psi_cond_key, &m_signal_cond, nullptr);
}

void Rdb_thread::uninit() {
  mysql_mutex_destroy(&m_signal_mutex);
  mysql_cond_destroy(&m_signal_cond);
}

int Rdb_thread::create_thread(const std::string &thread_name
#ifdef HAVE_PSI_INTERFACE
                              ,
                              PSI_thread_key background_psi_thread_key
#endif
                              ) {
  DBUG_ASSERT(!thread_name.empty());

  int err = mysql_thread_create(background_psi_thread_key, &m_handle, nullptr,
                                thread_func, this);

  if (!err) {
    /*
      mysql_thread_create() ends up doing some work underneath and setting the
      thread name as "my-func". This isn't what we want. Our intent is to name
      the threads according to their purpose so that when displayed under the
      debugger then they'll be more easily identifiable. Therefore we'll reset
      the name if thread was successfully created.
    */
    err = pthread_setname_np(m_handle, thread_name.c_str());
  }

  return err;
}

void Rdb_thread::signal(const bool &stop_thread) {
  mysql_mutex_lock(&m_signal_mutex);
  if (stop_thread) {
    m_stop = true;
  }
  mysql_cond_signal(&m_signal_cond);
  mysql_mutex_unlock(&m_signal_mutex);
}

} // namespace myrocks
