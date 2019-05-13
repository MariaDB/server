/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2012 Kouhei Sutou <kou@clear-code.com>

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

#include "mrn_auto_increment_value_lock.hpp"

#if MYSQL_VERSION_ID >= 50500
#  define AUTO_INCREMENT_VALUE_MUTEX(table_share) (&(table_share->LOCK_ha_data))
#else
#  define AUTO_INCREMENT_VALUE_MUTEX(table_share) (&(table_share->mutex))
#endif

namespace mrn {
  AutoIncrementValueLock::AutoIncrementValueLock(TABLE_SHARE *table_share)
    : table_share_(table_share),
      need_lock_(table_share_->tmp_table == NO_TMP_TABLE) {
    if (need_lock_) {
      mysql_mutex_lock(AUTO_INCREMENT_VALUE_MUTEX(table_share_));
    }
  }

  AutoIncrementValueLock::~AutoIncrementValueLock() {
    if (need_lock_) {
      mysql_mutex_unlock(AUTO_INCREMENT_VALUE_MUTEX(table_share_));
    }
  }
}
