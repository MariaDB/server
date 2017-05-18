/* Copyright 2013-2015 Codership Oy <http://www.codership.com>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02111-1301 USA */

#ifndef WSREP_APPLIER_H
#define WSREP_APPLIER_H

#include <my_config.h>
#include "../wsrep/wsrep_api.h"

#include "sql_class.h" // THD class

void wsrep_set_apply_format(THD* thd, Format_description_log_event* ev);
Format_description_log_event* wsrep_get_apply_format(THD* thd);

int               wsrep_apply_events(THD*        thd,
                                     const void* events_buf,
                                     size_t      buf_len);

/* wsrep callback prototypes */

int               wsrep_apply_cb(void*                   ctx,
                                 uint32_t                flags,
                                 const wsrep_buf_t*      buf,
                                 const wsrep_trx_meta_t* meta,
                                 void**                  err_buf,
                                 size_t*                 err_len);
/* Applier error codes, when nothing better is available. */
#define WSREP_RET_SUCCESS      0 // Success
#define WSREP_ERR_GENERIC      1 // When in doubt (MySQL default error code)
#define WSREP_ERR_BAD_EVENT    2 // Can't parse event
#define WSREP_ERR_NOT_FOUND    3 // Key. table, schema not found
#define WSREP_ERR_EXISTS       4 // Key, table, schema already exists
#define WSREP_ERR_WRONG_TYPE   5 // Incompatible data type
#define WSREP_ERR_FAILED       6 // Operation failed for some internal reason
#define WSREP_ERR_ABORTED      7 // Operation was aborted externally

struct wsrep_error
{
  char*  str;
  size_t len;
};

void wsrep_get_thd_error(const THD* thd, wsrep_error& err);

wsrep_cb_status_t wsrep_commit_cb(void*                   ctx,
                                  uint32_t                flags,
                                  const wsrep_trx_meta_t* meta,
                                  wsrep_bool_t*           exit);

wsrep_cb_status_t wsrep_unordered_cb(void*              ctx,
                                     const wsrep_buf_t* data);

#endif /* WSREP_APPLIER_H */
