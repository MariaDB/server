/* Copyright (C) 2007 Google Inc.
   Copyright (C) 2008 MySQL AB
   Use is subject to license terms

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1335  USA */


#include <my_global.h>
#include "semisync.h"

const unsigned char Repl_semi_sync_base::k_packet_magic_num= 0xef;
const unsigned char Repl_semi_sync_base::k_packet_flag_sync= 0x01;


const unsigned long Trace::k_trace_general=  0x0001;
const unsigned long Trace::k_trace_detail=   0x0010;
const unsigned long Trace::k_trace_net_wait= 0x0020;
const unsigned long Trace::k_trace_function= 0x0040;

const unsigned char  Repl_semi_sync_base::k_sync_header[2]=
  {Repl_semi_sync_base::k_packet_magic_num, 0};
