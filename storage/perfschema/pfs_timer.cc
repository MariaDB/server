/* Copyright (c) 2008, 2023, Oracle and/or its affiliates.

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
  @file storage/perfschema/pfs_timer.cc
  Performance schema timers (implementation).
*/

#include "my_global.h"
#include "pfs_timer.h"
#include "my_rdtsc.h"
#include "log.h" /* sql_print_warning */

static ulonglong cycle_v0;
static ulonglong nanosec_v0;
static ulonglong microsec_v0;
static ulonglong millisec_v0;

static ulong cycle_to_pico; /* 1000 at 1 GHz, 333 at 3GHz, 250 at 4GHz */
static ulong nanosec_to_pico; /* In theory, 1 000 */
static ulong microsec_to_pico; /* In theory, 1 000 000 */
static ulong millisec_to_pico; /* In theory, 1 000 000 000, fits in uint32 */

/* Indexed by enum enum_timer_name */
static struct time_normalizer to_pico_data[FIRST_TIMER_NAME + COUNT_TIMER_NAME]=
{
  { 0, 0}, /* unused */
  { 0, 0}, /* cycle */
  { 0, 0}, /* nanosec */
  { 0, 0}, /* microsec */
  { 0, 0}, /* millisec */
};

static inline ulong round_to_ulong(double value)
{
  return (ulong) (value + 0.5);
}

void init_timers(void)
{
  double pico_frequency= 1.0e12;

  cycle_v0= my_timer_cycles();
  nanosec_v0= my_timer_nanoseconds();
  microsec_v0= my_timer_microseconds();
  millisec_v0= my_timer_milliseconds();

  if (sys_timer_info.cycles.frequency > 0)
    cycle_to_pico= round_to_ulong(pico_frequency/
                                  (double)sys_timer_info.cycles.frequency);
  else
    cycle_to_pico= 0;

  if (sys_timer_info.nanoseconds.frequency > 0)
    nanosec_to_pico= round_to_ulong(pico_frequency/
                                    (double)sys_timer_info.nanoseconds.frequency);
  else
    nanosec_to_pico= 0;

  if (sys_timer_info.microseconds.frequency > 0)
    microsec_to_pico= round_to_ulong(pico_frequency/
                                     (double)sys_timer_info.microseconds.frequency);
  else
    microsec_to_pico= 0;

  if (sys_timer_info.milliseconds.frequency > 0)
    millisec_to_pico= round_to_ulong(pico_frequency/
                                     (double)sys_timer_info.milliseconds.frequency);
  else
    millisec_to_pico= 0;


  to_pico_data[TIMER_NAME_CYCLE].m_v0= cycle_v0;
  to_pico_data[TIMER_NAME_CYCLE].m_factor= cycle_to_pico;

  to_pico_data[TIMER_NAME_NANOSEC].m_v0= nanosec_v0;
  to_pico_data[TIMER_NAME_NANOSEC].m_factor= nanosec_to_pico;

  to_pico_data[TIMER_NAME_MICROSEC].m_v0= microsec_v0;
  to_pico_data[TIMER_NAME_MICROSEC].m_factor= microsec_to_pico;

  to_pico_data[TIMER_NAME_MILLISEC].m_v0= millisec_v0;
  to_pico_data[TIMER_NAME_MILLISEC].m_factor= millisec_to_pico;

  if (cycle_to_pico == 0)
    sql_print_warning("The CYCLE timer is not available. "
                      "WAIT events in the performance_schema will not be timed.");

#ifdef HAVE_NANOSEC_TIMER
  if (nanosec_to_pico == 0)
    sql_print_warning("The NANOSECOND timer is not available. "
                      "IDLE/STAGE/STATEMENT/TRANSACTION events in the performance_schema will not be timed.");
#else
  if (microsec_to_pico == 0)
    sql_print_warning("The MICROSECOND timer is not available. "
                      "IDLE/STAGE/STATEMENT/TRANSACTION events in the performance_schema will not be timed.");
#endif
}

time_normalizer *time_normalizer::get_idle()
{
  return &to_pico_data[USED_TIMER_NAME];
}

time_normalizer *time_normalizer::get_wait()
{
  return &to_pico_data[TIMER_NAME_CYCLE];
}

time_normalizer *time_normalizer::get_stage()
{
  return &to_pico_data[USED_TIMER_NAME];
}

time_normalizer *time_normalizer::get_statement()
{
  return &to_pico_data[USED_TIMER_NAME];
}

time_normalizer *time_normalizer::get_transaction()
{
  return &to_pico_data[USED_TIMER_NAME];
}

void time_normalizer::to_pico(ulonglong start, ulonglong end,
                              ulonglong *pico_start, ulonglong *pico_end, ulonglong *pico_wait)
{
  if (start == 0)
  {
    *pico_start= 0;
    *pico_end= 0;
    *pico_wait= 0;
  }
  else
  {
    *pico_start= (start - m_v0) * m_factor;
    if (end == 0)
    {
      *pico_end= 0;
      *pico_wait= 0;
    }
    else
    {
      *pico_end= (end - m_v0) * m_factor;
      *pico_wait= (end - start) * m_factor;
    }
  }
}

