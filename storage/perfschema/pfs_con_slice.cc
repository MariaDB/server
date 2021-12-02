/* Copyright (c) 2010, 2015, Oracle and/or its affiliates. All rights reserved.

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
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */


#include "my_global.h"
#include "my_pthread.h"
#include "pfs_con_slice.h"
#include "pfs_stat.h"
#include "pfs_global.h"
#include "pfs_instr_class.h"

/**
  @file storage/perfschema/pfs_con_slice.cc
  Performance schema connection slice (implementation).
*/

/**
  @addtogroup Performance_schema_buffers
  @{
*/

PFS_single_stat *
PFS_connection_slice::alloc_waits_slice(uint sizing)
{
  PFS_single_stat *slice= NULL;
  uint index;

  if (sizing > 0)
  {
    slice= PFS_MALLOC_ARRAY(sizing, sizeof(PFS_single_stat), PFS_single_stat,
                            MYF(MY_ZEROFILL));
    if (unlikely(slice == NULL))
      return NULL;

    for (index= 0; index < sizing; index++)
      slice[index].reset();
  }

  return slice;
}

PFS_stage_stat *
PFS_connection_slice::alloc_stages_slice(uint sizing)
{
  PFS_stage_stat *slice= NULL;
  uint index;

  if (sizing > 0)
  {
    slice= PFS_MALLOC_ARRAY(sizing, sizeof(PFS_stage_stat), PFS_stage_stat,
                            MYF(MY_ZEROFILL));
    if (unlikely(slice == NULL))
      return NULL;

    for (index= 0; index < sizing; index++)
      slice[index].reset();
  }

  return slice;
}

PFS_statement_stat *
PFS_connection_slice::alloc_statements_slice(uint sizing)
{
  PFS_statement_stat *slice= NULL;
  uint index;

  if (sizing > 0)
  {
    slice= PFS_MALLOC_ARRAY(sizing, sizeof(PFS_statement_stat), PFS_statement_stat,
                            MYF(MY_ZEROFILL));
    if (unlikely(slice == NULL))
      return NULL;

    for (index= 0; index < sizing; index++)
      slice[index].reset();
  }

  return slice;
}

void PFS_connection_slice::reset_waits_stats()
{
  PFS_single_stat *stat= m_instr_class_waits_stats;
  PFS_single_stat *stat_last= stat + wait_class_max;
  for ( ; stat < stat_last; stat++)
    stat->reset();
}

void PFS_connection_slice::reset_stages_stats()
{
  PFS_stage_stat *stat= m_instr_class_stages_stats;
  PFS_stage_stat *stat_last= stat + stage_class_max;
  for ( ; stat < stat_last; stat++)
    stat->reset();
}

void PFS_connection_slice::reset_statements_stats()
{
  PFS_statement_stat *stat= m_instr_class_statements_stats;
  PFS_statement_stat *stat_last= stat + statement_class_max;
  for ( ; stat < stat_last; stat++)
    stat->reset();
}

/** @} */

