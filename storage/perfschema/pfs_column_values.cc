/* Copyright (c) 2008, 2011, Oracle and/or its affiliates. All rights reserved.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License, version 2.0,
  as published by the Free Software Foundation.

  This program is also distributed with certain software (including
  but not limited to OpenSSL) that is licensed under separate terms,
  as designated in a particular file or component or in included license
  documentation.  The authors of MySQL hereby grant you an additional
  permission to link the program and your derivative works with the
  separately licensed software that they have included with MySQL.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License, version 2.0, for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software Foundation,
  51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

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
