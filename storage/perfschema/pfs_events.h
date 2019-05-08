/* Copyright (c) 2010, 2011, Oracle and/or its affiliates. All rights reserved.

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

#ifndef PFS_EVENTS_H
#define PFS_EVENTS_H

/**
  @file storage/perfschema/pfs_events.h
  Events data structures (declarations).
*/

#include "pfs_column_types.h"

struct PFS_instr_class;

/** An event record. */
struct PFS_events
{
  /** THREAD_ID. */
  ulonglong m_thread_internal_id;
  /** EVENT_ID. */
  ulonglong m_event_id;
  /** END_EVENT_ID. */
  ulonglong m_end_event_id;
  /** NESTING_EVENT_ID. */
  ulonglong m_nesting_event_id;
  /**
    Timer start.
    This member is populated only if m_class->m_timed is true.
  */
  ulonglong m_timer_start;
  /**
    Timer end.
    This member is populated only if m_class->m_timed is true.
  */
  ulonglong m_timer_end;
  /** Instrument metadata. */
  PFS_instr_class *m_class;
  /** Location of the instrumentation in the source code (file name). */
  const char *m_source_file;
  /** (EVENT_TYPE) */
  enum_event_type m_event_type;
  /** NESTING_EVENT_TYPE */
  enum_event_type m_nesting_event_type;
  /** Location of the instrumentation in the source code (line number). */
  uint m_source_line;
};

#endif

