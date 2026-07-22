/*
   Copyright (c) 2015 MariaDB Corporation Ab

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

#ifdef USE_PRAGMA_IMPLEMENTATION
#pragma implementation				// gcc: Class implementation
#endif

#include "mariadb.h"
#include "sql_priv.h"
#include "sql_select.h"
#include "my_json_writer.h"

void Filesort_tracker::print_json_members(Json_writer *writer)
{
  const char *varied_str= "(varied across executions)";
  String str;

  if (!get_r_loops())
    writer->add_member("r_loops").add_null();
  else
    writer->add_member("r_loops").add_ll(get_r_loops());
  
  if (time_tracker.has_timed_statistics())
  {
    writer->add_member("r_total_time_ms").
            add_double(time_tracker.get_time_ms());
  }
  if (r_limit != HA_POS_ERROR)
  {
    writer->add_member("r_limit");
    if (!get_r_loops())
      writer->add_null();
    else if (r_limit == 0)
      writer->add_str(varied_str);
    else
      writer->add_ll(r_limit);
  }

  writer->add_member("r_used_priority_queue"); 
  if (!get_r_loops())
    writer->add_null();
  else if (r_used_pq == get_r_loops())
    writer->add_bool(true);
  else if (r_used_pq == 0)
    writer->add_bool(false);
  else
    writer->add_str(varied_str);

  if (!get_r_loops())
    writer->add_member("r_output_rows").add_null();
  else
    writer->add_member("r_output_rows").add_ll(
                        (longlong) rint((double)r_output_rows / get_r_loops()));

  if (sort_passes)
  {
    writer->add_member("r_sort_passes").add_ll(
                        (longlong) rint((double)sort_passes / get_r_loops()));
  }

  if (sort_buffer_size != 0)
  {
    writer->add_member("r_buffer_size");
    if (sort_buffer_size == ulonglong(-1))
      writer->add_str(varied_str);
    else
      writer->add_size(sort_buffer_size);
  }

  get_data_format(&str);
  writer->add_member("r_sort_mode").add_str(str.ptr(), str.length());
}

void Filesort_tracker::get_data_format(String *str)
{
  if (r_sort_keys_packed)
    str->append(STRING_WITH_LEN("packed_sort_key"));
  else
    str->append(STRING_WITH_LEN("sort_key"));
  str->append(',');

  if (r_using_addons)
  {
    if (r_packed_addon_fields)
      str->append(STRING_WITH_LEN("packed_addon_fields"));
    else
      str->append(STRING_WITH_LEN("addon_fields"));
  }
  else
    str->append(STRING_WITH_LEN("rowid"));
}

void attach_gap_time_tracker(THD *thd, Gap_time_tracker *gap_tracker,
                             ulonglong timeval)
{
  thd->gap_tracker_data.bill_to= gap_tracker;
  thd->gap_tracker_data.start_time= timeval;
}

void process_gap_time_tracker(THD *thd, ulonglong timeval)
{
  if (thd->gap_tracker_data.bill_to)
  {
    thd->gap_tracker_data.bill_to->log_time(thd->gap_tracker_data.start_time,
                                            timeval);
    thd->gap_tracker_data.bill_to= NULL;
  }
}

