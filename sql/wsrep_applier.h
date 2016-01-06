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

#include <my_config.h>
#include "../wsrep/wsrep_api.h"

void wsrep_set_apply_format(THD* thd, Format_description_log_event* ev);
Format_description_log_event* wsrep_get_apply_format(THD* thd);

/* wsrep callback prototypes */

wsrep_cb_status_t wsrep_apply_cb(void* const ctx,
                                 const void* const buf, size_t const buf_len,
                                 uint32_t const flags,
                                 const wsrep_trx_meta_t* meta);

wsrep_cb_status_t wsrep_commit_cb(void* const ctx,
                                  uint32_t const flags,
                                  const wsrep_trx_meta_t* meta,
                                  wsrep_bool_t* const exit,
                                  bool const commit);

wsrep_cb_status_t wsrep_unordered_cb(void*       const ctx,
                                     const void* const data,
                                     size_t      const size);

#endif /* WSREP_APPLIER_H */
