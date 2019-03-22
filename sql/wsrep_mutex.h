/* Copyright 2018 Codership Oy <info@codership.com>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#ifndef WSREP_MUTEX_H
#define WSREP_MUTEX_H

/* wsrep-lib */
#include "wsrep/mutex.hpp"

/* implementation */
#include "my_pthread.h"

class Wsrep_mutex : public wsrep::mutex
{
public:
  Wsrep_mutex(mysql_mutex_t& mutex)
    : m_mutex(mutex)
  { }

  void lock()
  {
    mysql_mutex_lock(&m_mutex);
  }

  void unlock()
  {
    mysql_mutex_unlock(&m_mutex);
  }

  void* native()
  {
    return &m_mutex;
  }
private:
  mysql_mutex_t& m_mutex;
};

#endif /* WSREP_MUTEX_H */
