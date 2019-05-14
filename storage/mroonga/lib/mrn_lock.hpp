/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2013-2015  Kouhei Sutou <kou@clear-code.com>

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA
*/

#ifndef MRN_LOCK_HPP_
#define MRN_LOCK_HPP_

#include <mrn_mysql.h>
#include <mrn_mysql_compat.h>

namespace mrn {
  class Lock {
  public:
    Lock(mysql_mutex_t *mutex, bool execute=true);
    ~Lock();
  private:
    mysql_mutex_t *mutex_;
    bool execute_;
  };
}

#endif /* MRN_LOCK_HPP_ */
