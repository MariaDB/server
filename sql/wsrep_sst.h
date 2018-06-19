/* Copyright (C) 2013-2018 Codership Oy <info@codership.com>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License along
   with this program; if not, write to the Free Software Foundation, Inc.,
   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA. */

#include <my_config.h>

#ifndef WSREP_SST_H
#define WSREP_SST_H

#include "wsrep/gtid.hpp"
#include <my_global.h>
#include <string>

/* system variables */
extern const char* wsrep_sst_method;
extern const char* wsrep_sst_receive_address;
extern const char* wsrep_sst_donor;
extern const char* wsrep_sst_auth;
extern my_bool wsrep_sst_donor_rejects_queries;

/**
   Return a string containing the state transfer request string.
   Note that the string may contain a '\0' in the middle.
*/
std::string wsrep_sst_prepare();

/**
   Donate a SST.

  @param request SST request string received from the joiner. Note that
                 the string may contain a '\0' in the middle.
  @param gtid    Current position of the donor
  @param bypass  If true, full SST is not needed. Joiner needs to be
                 notified that it can continue starting from gtid.
 */
int wsrep_sst_donate(const std::string& request,
                     const wsrep::gtid& gtid,
                     bool bypass);

#else
#define wsrep_SE_initialized() do { } while(0)
#define wsrep_SE_init_grab() do { } while(0)
#define wsrep_SE_init_done() do { } while(0)
#define wsrep_sst_continue() (0)

#endif /* WITH_WSREP */
#endif /* WSREP_SST_H */
