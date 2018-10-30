/* Copyright 2018 Codership Oy <info@codership.com>

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

/*
  Wsrep typedefs to better conform to coding style.
 */
#ifndef WSREP_TYPES_H
#define WSREP_TYPES_H

#include "wsrep/seqno.hpp"
#include "wsrep/view.hpp"

typedef wsrep::id Wsrep_id;
typedef wsrep::seqno Wsrep_seqno;
typedef wsrep::view Wsrep_view;

#endif /* WSREP_TYPES_H */
