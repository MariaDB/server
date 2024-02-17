/* Copyright (c) 2023, MariaDB

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


/**
  @file

  @brief
  This file defines all vector functions
*/

#include <my_global.h>
#include "item.h"
#include "item_vectorfunc.h"

key_map Item_func_vec_distance::part_of_sortkey() const
{
  key_map map(0);
  if (Item_field *item= get_field_arg())
  {
    Field *f= item->field;
    for (uint i= f->table->s->keys; i < f->table->s->total_keys; i++)
      if (f->table->s->key_info[i].algorithm == HA_KEY_ALG_VECTOR &&
          f->key_start.is_set(i))
        map.set_bit(i);
  }
  return map;
}

double Item_func_vec_distance::val_real()
{
  String *r1= args[0]->val_str();
  String *r2= args[1]->val_str();
  null_value= !r1 || !r2 || r1->length() != r2->length() ||
              r1->length() % sizeof(float);
  if (null_value)
    return 0;
  float *v1= (float*)r1->ptr();
  float *v2= (float*)r2->ptr();
  return euclidean_vec_distance(v1, v2, (r1->length()) / sizeof(float));
}

double euclidean_vec_distance(float *v1, float *v2, size_t v_len)
{
  float *p1= v1;
  float *p2= v2;
  double d= 0;
  for (size_t i= 0; i < v_len; p1++, p2++, i++)
  {
    float dist= *p1 - *p2;
    d+= dist * dist;
  }
  return sqrt(d);
}
