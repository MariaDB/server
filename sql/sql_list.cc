/* Copyright (c) 2000, 2010, Oracle and/or its affiliates. All rights reserved.

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

#include "mariadb.h"
#include "sql_list.h"

list_node end_of_list;


void free_list(I_List <i_string> *list)
{
  i_string *tmp;
  while ((tmp= list->get()))
    delete tmp;
}


bool base_list::copy(const base_list *rhs, MEM_ROOT *mem_root)
{
  bool error= 0;
  if (rhs->elements)
  {
    /*
      It's okay to allocate an array of nodes at once: we never
      call a destructor for list_node objects anyway.
    */
    if ((first= (list_node*) alloc_root(mem_root,
                                        sizeof(list_node) * rhs->elements)))
    {
      elements= rhs->elements;
      list_node *dst= first;
      list_node *src= rhs->first;
      for (; dst < first + elements - 1; dst++, src= src->next)
      {
        dst->info= src->info;
        dst->next= dst + 1;
      }
      /* Copy the last node */
      dst->info= src->info;
      dst->next= &end_of_list;
      /* Setup 'last' member */
      last= &dst->next;
      return 0;
    }
    error= 1;
  }
  elements= 0;
  first= &end_of_list;
  last= &first;
  return error;
}
