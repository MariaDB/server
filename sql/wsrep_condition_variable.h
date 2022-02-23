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

#ifndef WSREP_CONDITION_VARIABLE_H
#define WSREP_CONDITION_VARIABLE_H

/* wsrep-lib */
#include "wsrep/condition_variable.hpp"

/* implementation */
#include "my_pthread.h"

class Wsrep_condition_variable : public wsrep::condition_variable
{
public:

  Wsrep_condition_variable(mysql_cond_t* cond)
    : m_cond(cond)
  { }
  ~Wsrep_condition_variable()
  { }

  void notify_one()
  {
    mysql_cond_signal(m_cond);
  }

  void notify_all()
  {
    mysql_cond_broadcast(m_cond);
  }

  void wait(wsrep::unique_lock<wsrep::mutex>& lock)
  {
    mysql_mutex_t* mutex= static_cast<mysql_mutex_t*>(lock.mutex()->native());
    mysql_cond_wait(m_cond, mutex);
  }
private:
  mysql_cond_t* m_cond;
};

#endif /* WSREP_CONDITION_VARIABLE_H */
