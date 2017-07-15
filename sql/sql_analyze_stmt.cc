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
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02111-1301 USA */

#ifdef USE_PRAGMA_IMPLEMENTATION
#pragma implementation				// gcc: Class implementation
#endif

#include <my_global.h>
#include "sql_priv.h"
#include "sql_select.h"
#include "my_json_writer.h"

void Filesort_tracker::print_json_members(Json_writer *writer)
{
  const char *varied_str= "(varied across executions)";

  if (!get_r_loops())
    writer->add_member("r_loops").add_null();
  else
    writer->add_member("r_loops").add_ll(get_r_loops());
  
  if (get_r_loops() && time_tracker.timed)
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
      writer->add_ll((longlong) rint(r_limit/get_r_loops()));
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
    writer->add_member("r_output_rows").add_ll((longlong) rint(r_output_rows /
                                                              get_r_loops()));

  if (sort_passes)
  {
    writer->add_member("r_sort_passes").add_ll((longlong) rint(sort_passes /
                                                               get_r_loops()));
  }

  if (sort_buffer_size != 0)
  {
    writer->add_member("r_buffer_size");
    if (sort_buffer_size == ulonglong(-1))
      writer->add_str(varied_str);
    else
      writer->add_size(sort_buffer_size);
  }
}


static 
uchar *get_sp_call_counter_name(void *arg, 
                                size_t *length,
                                my_bool not_used __attribute__((unused)))
{
  Stored_routine_tracker::SP_call_counter *counter=
    (Stored_routine_tracker::SP_call_counter*)arg;

  *length= counter->name.length;
  return (uchar*)counter->name.str;
}


extern "C" void free_stored_routine_tracker(void *arg)
{
  Stored_routine_tracker::SP_call_counter *counter=
    (Stored_routine_tracker::SP_call_counter*)arg;
  delete counter;
}


Stored_routine_tracker::Stored_routine_tracker()
//: time_tracker(false) //todo remove this
{
  //TODO: tracker should only be used when doing timing.
  (void)my_hash_init(&name_to_counter,
                     system_charset_info, 10, 0, 0,
                     (my_hash_get_key) get_sp_call_counter_name,
                     free_stored_routine_tracker, HASH_UNIQUE);
}


Stored_routine_tracker::~Stored_routine_tracker()
{
  my_hash_free(&name_to_counter);
}


void Stored_routine_tracker::report_routine_start(const LEX_STRING *qname)
{
  SP_call_counter *cntr;

  if (!(cntr= (SP_call_counter*) my_hash_search(&name_to_counter,
                                                (const uchar*)qname->str,
                                                qname->length)))
  {
    if ((cntr= new SP_call_counter))
    {
      cntr->name= *qname;
      my_hash_insert(&name_to_counter, (uchar*)cntr);
    }
  }

  if (cntr)
    ANALYZE_START_TRACKING(&cntr->count);
}

void Stored_routine_tracker::report_routine_end(const LEX_STRING *qname)
{
  SP_call_counter *cntr;
  if ((cntr= (SP_call_counter*) my_hash_search(&name_to_counter,
                                               (const uchar*)qname->str,
                                               qname->length)))
  {
    ANALYZE_STOP_TRACKING(&cntr->count);
  }
}

void Stored_routine_tracker::print_json_members(Json_writer *writer)
{
  if (name_to_counter.records)
  {
    writer->add_member("r_stored_routines").start_object();
    uint i;
    for (i= 0; i < name_to_counter.records; ++i)
    {
      SP_call_counter *cntr= 
        (SP_call_counter*)my_hash_element(&name_to_counter, i);
      writer->start_object();
      writer->add_member("qname").add_str(cntr->name.str);
      writer->add_member("r_count").add_ll(cntr->count.get_loops());
      writer->add_member("r_total_time_ms").add_double(cntr->count.get_time_ms());
      writer->end_object();
    }
    writer->end_object();
  }
}

