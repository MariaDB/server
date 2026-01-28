/* Copyright (c) 2025, MariaDB Corporation & Rakuten Corp.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

#ifndef INTEGER_CURSOR_INCLUDED
#define INTEGER_CURSOR_INCLUDED

struct cursor_statement_t
{
  int cursor_id;
  int cursor_stmt_cmd;
};

class Dbms_sql
{
public:
  Dbms_sql()
   :cursor_list(PSI_INSTRUMENT_MEM), last_cursor(-1),
    in_dbmssql_execute_dynamic_mode(false)
  { }

  int cursor_idx(int cursor_id)
  {
    for (int i= 0; i < (int) cursor_list.elements(); i++)
    {
      if (cursor_list[i].cursor_id == cursor_id)
        return i;
    }
    return -1; // Not found
  }

  int add_cursor()
  {
    int tmp_cursor= (last_cursor + 1) % INT_MAX;
    int cursor_pos= cursor_idx(tmp_cursor);
    int elements = (int) cursor_list.elements();
    while (tmp_cursor != last_cursor && cursor_pos != -1
        && cursor_pos < elements)
    {
      tmp_cursor= (tmp_cursor + 1) % INT_MAX;
      cursor_pos= cursor_idx(tmp_cursor);
      elements = (int) cursor_list.elements();
    }

    if (tmp_cursor == last_cursor)
    {
      // Return -1 to indicate all possible cursor integers are used
      return -1;
    }
    last_cursor= tmp_cursor;
    cursor_statement_t new_cursor_stmt;
    new_cursor_stmt.cursor_id= tmp_cursor;
    cursor_list.append(new_cursor_stmt);
    return cursor_list.back()->cursor_id;
  }

  bool del_cursor(int cursor_id)
  {
    for (size_t i= 0; i < cursor_list.elements(); i++)
    {
      if (((cursor_statement_t) cursor_list[i]).cursor_id == cursor_id)
      {
        cursor_list.del(i);
        return 0;
      }
    }
    return 1;
  }

  Dynamic_array<cursor_statement_t> cursor_list;
  int last_cursor;
  String dbms_sql_code_str;
  bool in_dbmssql_execute_dynamic_mode;
};

#endif // INTEGER_CURSOR_INCLUDED
