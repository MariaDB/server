/* Copyright (c) 2008, 2011, Oracle and/or its affiliates. All rights reserved.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software Foundation,
  51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA */

#include <my_global.h>
#include "table_session_connect_attrs.h"

THR_LOCK table_session_connect_attrs::m_table_lock;

PFS_engine_table_share
table_session_connect_attrs::m_share=
{
  { C_STRING_WITH_LEN("session_connect_attrs") },
  &pfs_readonly_acl,
  &table_session_connect_attrs::create,
  NULL, /* write_row */
  NULL, /* delete_all_rows */
  NULL, /* get_row_count */
  1000, /* records */
  sizeof(pos_connect_attr_by_thread_by_attr), /* ref length */
  &m_table_lock,
  { C_STRING_WITH_LEN("CREATE TABLE session_connect_attrs("
                      "PROCESSLIST_ID INT NOT NULL,"
                      "ATTR_NAME VARCHAR(32) NOT NULL,"
                      "ATTR_VALUE VARCHAR(1024),"
                      "ORDINAL_POSITION INT"
                      ") CHARACTER SET utf8 COLLATE utf8_bin") }
};

PFS_engine_table* table_session_connect_attrs::create()
{
  return new table_session_connect_attrs();
}

table_session_connect_attrs::table_session_connect_attrs()
  : table_session_connect(&m_share)
{}
