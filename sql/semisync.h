/* Copyright (C) 2007 Google Inc.
   Copyright (C) 2008 MySQL AB

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


#ifndef SEMISYNC_H
#define SEMISYNC_H

#include "mysqld.h"
#include "log_event.h"
#include "replication.h"

/**
   This class is used to trace function calls and other process
   information
*/
class Trace {
public:
  static const unsigned long k_trace_function;
  static const unsigned long k_trace_general;
  static const unsigned long k_trace_detail;
  static const unsigned long k_trace_net_wait;

  unsigned long           m_trace_level;                      /* the level for tracing */

  Trace()
    :m_trace_level(0L)
  {}
  Trace(unsigned long trace_level)
    :m_trace_level(trace_level)
  {}
};

/**
   Base class for semi-sync master and slave classes
*/
class Repl_semi_sync_base
  :public Trace {
public:
  static const unsigned char  k_sync_header[2];     /* three byte packet header */

  /* Constants in network packet header. */
  static const unsigned char k_packet_magic_num;
  static const unsigned char k_packet_flag_sync;
};

/* The layout of a semisync slave reply packet:
   1 byte for the magic num
   8 bytes for the binlog positon
   n bytes for the binlog filename, terminated with a '\0'
*/
#define REPLY_MAGIC_NUM_LEN 1
#define REPLY_BINLOG_POS_LEN 8
#define REPLY_BINLOG_NAME_LEN (FN_REFLEN + 1)
#define REPLY_MAGIC_NUM_OFFSET 0
#define REPLY_BINLOG_POS_OFFSET (REPLY_MAGIC_NUM_OFFSET + REPLY_MAGIC_NUM_LEN)
#define REPLY_BINLOG_NAME_OFFSET (REPLY_BINLOG_POS_OFFSET + REPLY_BINLOG_POS_LEN)
#define REPLY_MESSAGE_MAX_LENGTH \
    (REPLY_MAGIC_NUM_LEN + REPLY_BINLOG_POS_LEN + REPLY_BINLOG_NAME_LEN)

#endif /* SEMISYNC_H */
