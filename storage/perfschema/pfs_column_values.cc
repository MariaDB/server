/* Copyright (c) 2008, 2011, Oracle and/or its affiliates. All rights reserved.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software Foundation,
  51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA */

/**
  @file storage/perfschema/pfs_column_values.cc
  Literal values for columns used in the performance
  schema tables (implementation).
*/

#include "my_global.h"
#include "pfs_column_values.h"

LEX_CSTRING PERFORMANCE_SCHEMA_str=
{ STRING_WITH_LEN("performance_schema") };

LEX_CSTRING mutex_instrument_prefix=
{ STRING_WITH_LEN("wait/synch/mutex/") };

LEX_CSTRING rwlock_instrument_prefix=
{ STRING_WITH_LEN("wait/synch/rwlock/") };

LEX_CSTRING cond_instrument_prefix=
{ STRING_WITH_LEN("wait/synch/cond/") };

LEX_CSTRING thread_instrument_prefix=
{ STRING_WITH_LEN("thread/") };

LEX_CSTRING file_instrument_prefix=
{ STRING_WITH_LEN("wait/io/file/") };

LEX_CSTRING stage_instrument_prefix=
{ STRING_WITH_LEN("stage/") };

LEX_CSTRING statement_instrument_prefix=
{ STRING_WITH_LEN("statement/") };

LEX_CSTRING socket_instrument_prefix=
{ STRING_WITH_LEN("wait/io/socket/") };
