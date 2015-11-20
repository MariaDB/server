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
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

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
  writer->add_member("r_loops").add_ll(get_r_loops());
  
  if (get_r_loops() && time_tracker.timed)
  {
    writer->add_member("r_total_time_ms").
            add_double(time_tracker.get_time_ms());
  }
  if (r_limit != HA_POS_ERROR)
  {
    writer->add_member("r_limit");
    if (r_limit == 0)
      writer->add_str(varied_str);
    else
      writer->add_ll((longlong) rint(r_limit/get_r_loops()));
  }

  writer->add_member("r_used_priority_queue"); 
  if (r_used_pq == get_r_loops())
    writer->add_bool(true);
  else if (r_used_pq == 0)
    writer->add_bool(false);
  else
    writer->add_str(varied_str);

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


/* 
  Report that we are doing a filesort. 
    @return 
      Tracker object to be used with filesort
*/

Filesort_tracker *Sort_and_group_tracker::report_sorting(THD *thd)
{
  DBUG_ASSERT(cur_action < MAX_QEP_ACTIONS);

  if (total_actions)
  {
    /* This is not the first execution. Check */
    if (qep_actions[cur_action] != EXPL_ACTION_FILESORT)
    {
      varied_executions= true;
      cur_action++;
      if (!dummy_fsort_tracker)
        dummy_fsort_tracker= new (thd->mem_root) Filesort_tracker(is_analyze);
      return dummy_fsort_tracker;
    }
    return qep_actions_data[cur_action++].filesort_tracker;
  }

  Filesort_tracker *fs_tracker= new(thd->mem_root)Filesort_tracker(is_analyze);
  qep_actions_data[cur_action].filesort_tracker= fs_tracker;
  qep_actions[cur_action++]= EXPL_ACTION_FILESORT;

  return fs_tracker;
}


void Sort_and_group_tracker::report_tmp_table(TABLE *tbl)
{
  DBUG_ASSERT(cur_action < MAX_QEP_ACTIONS);
  if (total_actions)
  {
    /* This is not the first execution. Check if the steps match.  */
    // todo: should also check that tmp.table kinds are the same.
    if (qep_actions[cur_action] != EXPL_ACTION_TEMPTABLE)
      varied_executions= true;
  }

  if (!varied_executions)
  {
    qep_actions[cur_action]= EXPL_ACTION_TEMPTABLE;
    // qep_actions_data[cur_action]= ....
  }
  
  cur_action++;
}


void Sort_and_group_tracker::report_duplicate_removal()
{
  DBUG_ASSERT(cur_action < MAX_QEP_ACTIONS);
  if (total_actions)
  {
    /* This is not the first execution. Check if the steps match.  */
    if (qep_actions[cur_action] != EXPL_ACTION_REMOVE_DUPS)
      varied_executions= true;
  }

  if (!varied_executions)
  {
    qep_actions[cur_action]= EXPL_ACTION_REMOVE_DUPS;
  }

  cur_action++;
}

