/* Copyright (c) 2008, 2023, Oracle and/or its affiliates.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License, version 2.0,
  as published by the Free Software Foundation.

  This program is also distributed with certain software (including
  but not limited to OpenSSL) that is licensed under separate terms,
  as designated in a particular file or component or in included license
  documentation.  The authors of MySQL hereby grant you an additional
  permission to link the program and your derivative works with the
  separately licensed software that they have included with MySQL.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License, version 2.0, for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software Foundation,
  51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

#include <my_global.h>
#include "table_session_account_connect_attrs.h"

THR_LOCK table_session_account_connect_attrs::m_table_lock;

PFS_engine_table_share_state
table_session_account_connect_attrs::m_share_state = {
  false /* m_checked */
};

PFS_engine_table_share
table_session_account_connect_attrs::m_share=
{
  { C_STRING_WITH_LEN("session_account_connect_attrs") },
  &pfs_readonly_world_acl,
  table_session_account_connect_attrs::create,
  NULL, /* write_row */
  NULL, /* delete_all_rows */
  cursor_by_thread_connect_attr::get_row_count,
  sizeof(pos_connect_attr_by_thread_by_attr), /* ref length */
  &m_table_lock,
  { C_STRING_WITH_LEN("CREATE TABLE session_account_connect_attrs("
                      "PROCESSLIST_ID INT NOT NULL comment 'Session connection identifier.',"
                      "ATTR_NAME VARCHAR(32) NOT NULL comment 'Attribute name.',"
                      "ATTR_VALUE VARCHAR(1024) comment 'Attribute value.',"
                      "ORDINAL_POSITION INT comment 'Order in which attribute was added to the connection attributes.'"
                      ") CHARACTER SET utf8 COLLATE utf8_bin") },
  false, /* m_perpetual */
  false, /* m_optional */
  &m_share_state
};

PFS_engine_table* table_session_account_connect_attrs::create()
{
  return new table_session_account_connect_attrs();
}

table_session_account_connect_attrs::table_session_account_connect_attrs()
  : table_session_connect(&m_share)
{}

bool
table_session_account_connect_attrs::thread_fits(PFS_thread *thread)
{
  PFS_thread *current_thread= PFS_thread::get_current_thread();
  /* The current thread may not have instrumentation attached. */
  if (current_thread == NULL)
    return false;

  /* The thread we compare to, by definition, has some instrumentation. */
  assert(thread != NULL);

  uint username_length= current_thread->m_username_length;
  uint hostname_length= current_thread->m_hostname_length;

  if (   (thread->m_username_length != username_length)
      || (thread->m_hostname_length != hostname_length))
    return false;

  if (memcmp(thread->m_username, current_thread->m_username, username_length) != 0)
    return false;

  if (memcmp(thread->m_hostname, current_thread->m_hostname, hostname_length) != 0)
    return false;

  return true;
}
