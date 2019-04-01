/* Copyright (C) 2015 Codership Oy <info@codership.com>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License along
   with this program; if not, write to the Free Software Foundation, Inc.,
   51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA. */

#ifndef WSREP_XID_H
#define WSREP_XID_H

#include <my_config.h>

#ifdef WITH_WSREP

#include "wsrep_mysqld.h"
#include "wsrep/gtid.hpp"
#include "handler.h" // XID typedef

void wsrep_xid_init(xid_t*, const wsrep::gtid&, const wsrep_server_gtid_t&);
const wsrep::id& wsrep_xid_uuid(const XID&);
wsrep::seqno wsrep_xid_seqno(const XID&);

template<typename T> T wsrep_get_SE_checkpoint();
bool wsrep_set_SE_checkpoint(const wsrep::gtid& gtid, const wsrep_server_gtid_t&);
//void wsrep_get_SE_checkpoint(XID&);             /* uncomment if needed */
//void wsrep_set_SE_checkpoint(XID&);             /* uncomment if needed */

void wsrep_sort_xid_array(XID *array, int len);

#endif /* WITH_WSREP */
#endif /* WSREP_UTILS_H */
