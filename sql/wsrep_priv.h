/* Copyright 2010 Codership Oy <http://www.codership.com>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

//! @file declares symbols private to wsrep integration layer

#ifndef WSREP_PRIV_H
#define WSREP_PRIV_H

#include "wsrep_mysqld.h"
#include "../wsrep/wsrep_api.h"

#include <log.h>
#include <pthread.h>
#include <cstdio>

void    wsrep_ready_set (my_bool x);

ssize_t wsrep_sst_prepare   (void** msg);
wsrep_cb_status wsrep_sst_donate_cb (void* app_ctx,
                                     void* recv_ctx,
                                     const void* msg, size_t msg_len,
                                     const wsrep_gtid_t* state_id,
                                     const char* state, size_t state_len,
                                     bool bypass);

extern wsrep_uuid_t  local_uuid;
extern wsrep_seqno_t local_seqno;

// a helper function
void wsrep_sst_received(wsrep_t*, const wsrep_uuid_t*, wsrep_seqno_t,
                               const void*, size_t);
/*! SST thread signals init thread about sst completion */
void wsrep_sst_complete(const wsrep_uuid_t*, wsrep_seqno_t, bool);

void wsrep_notify_status (wsrep_member_status_t new_status,
                          const wsrep_view_info_t* view = 0);

#endif /* WSREP_PRIV_H */
