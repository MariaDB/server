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

#include <cmath>
#include <my_global.h>

#include "item.h"
#include "item_vectorfunc.h"
#include "json_lib.h"
#include "m_ctype.h"
#include "sql_const.h"
#include "sql_error.h"

key_map Item_func_vec_distance::part_of_sortkey() const
{
  key_map map(0);
  if (Item_field *item= get_field_arg())
  {
    Field *f= item->field;
    for (uint i= f->table->s->keys; i < f->table->s->total_keys; i++)
      if (f->table->s->key_info[i].algorithm == HA_KEY_ALG_MHNSW &&
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
  float *v1= (float *) r1->ptr();
  float *v2= (float *) r2->ptr();
  return euclidean_vec_distance(v1, v2, (r1->length()) / sizeof(float));
}

bool Item_func_vec_totext::fix_length_and_dec(THD *thd)
{
  decimals= 0;
  max_length= ((args[0]->max_length / 4) *
               (MAX_FLOAT_STR_LENGTH + 1 /* comma */)) + 2 /* braces */;
  fix_length_and_charset(max_length, default_charset());
  set_maybe_null();
  return false;
}

String *Item_func_vec_totext::val_str_ascii(String *str)
{
  String *r1= args[0]->val_str();
  if (args[0]->null_value)
  {
    null_value= true;
    return nullptr;
  }

  // Wrong size returns null
  if (r1->length() % 4)
  {
    THD *thd= current_thd;
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                        ER_VECTOR_BINARY_FORMAT_INVALID,
                        ER_THD(thd, ER_VECTOR_BINARY_FORMAT_INVALID));
    null_value= true;
    return nullptr;
  }

  str->length(0);
  str->set_charset(&my_charset_numeric);
  str->reserve(r1->length() / 4 * (MAX_FLOAT_STR_LENGTH + 1) + 2);
  str->append('[');
  const char *ptr= r1->ptr();
  for (size_t i= 0; i < r1->length(); i+= 4)
  {
    float val= *reinterpret_cast<const float *>(ptr);
    if (std::isinf(val))
      if (val < 0)
        str->append(STRING_WITH_LEN("-Inf"));
      else
        str->append(STRING_WITH_LEN("Inf"));
    else if (std::isnan(val))
      str->append(STRING_WITH_LEN("NaN"));
    else
      str->append_float(val, FLT_DIG);

    ptr+= 4;
    if (r1->length() - i > 4)
      str->append(',');
  }
  str->append(']');

  return str;
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

Item_func_vec_totext::Item_func_vec_totext(THD *thd, Item *a)
    : Item_str_ascii_checksum_func(thd, a)
{
}

Item_func_vec_fromtext::Item_func_vec_fromtext(THD *thd, Item *a)
    : Item_str_func(thd, a)
{
}

bool Item_func_vec_fromtext::fix_length_and_dec(THD *thd)
{
  decimals= 0;
  /* Worst case scenario, for a valid input we have a string of the form:
     [1,2,3,4,5,...] single digit numbers.
     This means we can have (max_length - 1) / 2 floats.
     Each float takes 4 bytes, so we do (max_length - 1) * 2. */
  fix_length_and_charset((args[0]->max_length - 1) * 2, &my_charset_bin);
  set_maybe_null();
  return false;
}

String *Item_func_vec_fromtext::val_str(String *buf)
{
  json_engine_t je;
  bool end_ok= false;
  String *value = args[0]->val_json(&tmp_js);
  CHARSET_INFO *cs= value->charset();

  buf->length(0);

  if (!value)
  {
    null_value= true;
    return nullptr;
  }

  const uchar *start= reinterpret_cast<const uchar *>(value->ptr());
  const uchar *end= start + value->length();

  if (json_scan_start(&je, cs, start, end) ||
      json_read_value(&je))
    goto error;

  if (je.value_type != JSON_VALUE_ARRAY)
    goto error_format;

  /* Accept only arrays of floats. */
  do {
    switch (je.state)
    {
      case JST_ARRAY_START:
        continue;
      case JST_ARRAY_END:
        end_ok = true;
        break;
      case JST_VALUE:
      {
        if (json_read_value(&je))
          goto error;

        if (je.value_type != JSON_VALUE_NUMBER)
          goto error_format;

        int error;
        char *start= (char *)je.value_begin, *end;
        float f= (float)cs->strntod(start, je.value_len, &end, &error);
        if (unlikely(error))
            goto error_format;

        char f_bin[4];
        float4store(&f_bin, f);
        buf->append(f_bin[0]);
        buf->append(f_bin[1]);
        buf->append(f_bin[2]);
        buf->append(f_bin[3]);
        break;
      }
      default:
        goto error_format;
    }
  } while (json_scan_next(&je) == 0);

  if (!end_ok)
    goto error_format;

  return buf;

error_format:
  {
    int position= (int)((const char *) je.s.c_str - value->ptr());
    null_value= true;
    THD *thd= current_thd;
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                        ER_VECTOR_FORMAT_INVALID,
                        ER_THD(thd, ER_VECTOR_FORMAT_INVALID),
                        position,
                        value->ptr());
    return nullptr;
  }

error:
  report_json_error_ex(value->ptr(), &je, func_name(),
                       0, Sql_condition::WARN_LEVEL_WARN);
  null_value= true;
  return nullptr;
}
