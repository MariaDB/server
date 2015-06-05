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

void Filesort_tracker::print_json(Json_writer *writer)
{
  const char *varied_str= "(varied across executions)";
  writer->add_member("r_loops").add_ll(r_loops);
  
  if (r_limit != HA_POS_ERROR)
  {
    writer->add_member("r_limit");
    if (r_limit == 0)
      writer->add_str(varied_str);
    else
      writer->add_ll(rint(r_limit/r_loops));
  }

  writer->add_member("r_used_priority_queue"); 
  if (r_used_pq == r_loops)
    writer->add_bool(true);
  else if (r_used_pq == 0)
    writer->add_bool(false);
  else
    writer->add_str(varied_str);

  writer->add_member("r_output_rows").add_ll(rint(r_output_rows / r_loops));

  if (sort_passes)
    writer->add_member("r_sort_passes").add_ll(rint(sort_passes / r_loops));

  if (sort_buffer_size != 0)
  {
    writer->add_member("r_buffer_size");
    if (sort_buffer_size == ulonglong(-1))
      writer->add_str(varied_str);
    else
      writer->add_size(sort_buffer_size);
  }
}

