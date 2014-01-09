/* Copyright 2013 Codership Oy <http://www.codership.com>

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

#ifndef WSREP_APPLIER_H
#define WSREP_APPLIER_H

#include <sys/types.h>

/* wsrep callback prototypes */

wsrep_cb_status_t wsrep_apply_cb(void *ctx,
                                 const void* buf, size_t buf_len,
                                 uint32_t flags,
                                 const wsrep_trx_meta_t* meta);

wsrep_cb_status_t wsrep_commit_cb(void *ctx,
                                  uint32_t flags,
                                  const wsrep_trx_meta_t* meta,
                                  wsrep_bool_t* exit,
                                  bool commit);

wsrep_cb_status_t wsrep_unordered_cb(void*       ctx,
                                     const void* data,
                                     size_t      size);

#endif /* WSREP_APPLIER_H */
