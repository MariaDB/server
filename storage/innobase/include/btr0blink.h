/****************************************************************************

Copyright (c) 2026, MariaDB plc

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

*****************************************************************************/

#ifndef btr0blink_h
#define btr0blink_h

#include "dict0boot.h"
#include "dict0mem.h"
#include "fsp0fsp.h"
#include "univ.i"

/** Whether a table shape is supported by the B-link proof of concept. */
inline bool blink_table_shape_ok(const dict_table_t *table) noexcept
{
  return table->not_redundant() && table->space && !table->space->zip_size() &&
    !table->is_temporary() && !dict_is_sys_table(table->id) &&
    !(table->flags2 & DICT_TF2_FTS);
}

/** Whether an index shape is supported by the B-link proof of concept. */
inline bool blink_index_shape_ok(const dict_index_t *index) noexcept
{
  return blink_table_shape_ok(index->table) &&
    !(index->type & (DICT_FTS | DICT_SPATIAL)) && index->is_committed();
}

/** Whether operations on an index must use the B-link proof-of-concept path. */
inline bool use_blink_path(const dict_index_t *index) noexcept
{
  return (index->type & DICT_BLINK) && blink_index_shape_ok(index);
}

#endif /* btr0blink_h */
