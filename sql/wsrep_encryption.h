/* Copyright 2019 Codership Oy <info@codership.com>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#ifndef WSREP_ENCRYPTION_H
#define WSREP_ENCRYPTION_H

#include "wsrep/encryption_service.hpp"

/**
 * Check if encryption is enabled and extract key.
 */
void wsrep_enable_encryption();

class Wsrep_encryption_service : public wsrep::encryption_service
{
public:
  int do_crypt(void**                ctx,
               wsrep::const_buffer&  key,
               const char            (*iv)[32],
               wsrep::const_buffer&  input,
               void*                 output,
               bool                  encrypt,
               bool                  last);
  bool encryption_enabled();
};

#endif /* WSREP_ENCRYPTION_H */

