/* Copyright (c) 2026, Kristian Nielsen and MariaDB Foundation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

#ifndef MY_XID_H
#define MY_XID_H

struct my_xid
{
  uint64 conn_id;
  uint32 commit_id;

  bool operator==(const my_xid &b)
  {
    return conn_id==b.conn_id && commit_id==b.commit_id;
  }
};

extern const uchar *my_xid_get_key(const void *p, size_t *out_len, my_bool);

#endif  /* MY_XID_H */
