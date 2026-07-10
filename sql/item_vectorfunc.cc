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

#include "item_vectorfunc.h"
#include "vector_mhnsw.h"
#include "sql_type_vector.h"

static double calc_distance_euclidean(float *v1, float *v2, size_t v_len)
{
  double d= 0;
  for (size_t i= 0; i < v_len; i++, v1++, v2++)
  {
    double dist= get_float(v1) - get_float(v2);
    d+= dist * dist;
  }
  return sqrt(d);
}

static double calc_distance_cosine(float *v1, float *v2, size_t v_len)
{
  double dotp=0, abs1=0, abs2=0;
  for (size_t i= 0; i < v_len; i++, v1++, v2++)
  {
    float f1= get_float(v1), f2= get_float(v2);
    abs1+= f1 * f1;
    abs2+= f2 * f2;
    dotp+= f1 * f2;
  }
  return 1 - dotp/sqrt(abs1*abs2);
}

Item_func_vec_distance::Item_func_vec_distance(THD *thd, Item *a, Item *b,
                                               distance_kind kind)
 :Item_real_func(thd, a, b), kind(kind)
{
}

bool Item_func_vec_distance::fix_length_and_dec(THD *thd)
{
  switch (kind) {
  case EUCLIDEAN: calc_distance= calc_distance_euclidean; break;
  case COSINE:    calc_distance= calc_distance_cosine; break;
  case AUTO:
    for (uint i=0; i < 2; i++)
      if (auto *item= dynamic_cast<Item_field*>(args[i]->real_item()))
      {
        TABLE_SHARE *share= item->field->orig_table->s;
        if (share->tmp_table)
          break;
        Field *f= share->field[item->field->field_index];
        KEY *kinfo= share->key_info;
        for (uint j= share->keys; j < share->total_keys; j++)
          if (kinfo[j].algorithm == HA_KEY_ALG_VECTOR && f->key_start.is_set(j))
          {
            kind= mhnsw_uses_distance(f->table, kinfo + j);
            return fix_length_and_dec(thd);
          }
      }
    my_error(ER_VEC_DISTANCE_TYPE, MYF(0));
    return 1;
  }
  set_maybe_null(); // if wrong dimensions
  return Item_real_func::fix_length_and_dec(thd);
}

key_map Item_func_vec_distance::part_of_sortkey() const
{
  key_map map(0);
  if (Item_field *item= get_field_arg())
  {
    Field *f= item->field;
    KEY *keyinfo= f->table->s->key_info;
    for (uint i= f->table->s->keys; i < f->table->s->total_keys; i++)
      if (!keyinfo[i].is_ignored && keyinfo[i].algorithm == HA_KEY_ALG_VECTOR
          && f->key_start.is_set(i)
          && mhnsw_uses_distance(f->table, keyinfo + i) == kind)
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
  return calc_distance(v1, v2, (r1->length()) / sizeof(float));
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
  if ((null_value= args[0]->null_value))
    return nullptr;

  // Wrong size returns null
  if (r1->length() % 4)
  {
    THD *thd= current_thd;
    push_warning(thd, Sql_condition::WARN_LEVEL_WARN,
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
    float val= get_float(ptr);
    if (std::isinf(val))
      if (val < 0)
        str->append(STRING_WITH_LEN("-Inf"));
      else
        str->append(STRING_WITH_LEN("Inf"));
    else if (std::isnan(val))
      str->append(STRING_WITH_LEN("NaN"));
    else
    {
      char buf[MAX_FLOAT_STR_LENGTH+1];
      size_t l= my_gcvt(val, MY_GCVT_ARG_FLOAT, MAX_FLOAT_STR_LENGTH, buf, 0);
      str->append(buf, l);
    }

    ptr+= 4;
    if (r1->length() - i > 4)
      str->append(',');
  }
  str->append(']');

  return str;
}

Item_func_vec_totext::Item_func_vec_totext(THD *thd, Item *a)
    : Item_str_ascii_checksum_func(thd, a)
{
}

Item_func_vec_fromtext::Item_func_vec_fromtext(THD *thd, Item *a)
    : Item_str_func(thd, a)
{
  mem_root_inited= false;
}

bool Item_func_vec_fromtext::fix_length_and_dec(THD *thd)
{
  mem_root_dynamic_array_init(thd->mem_root, PSI_INSTRUMENT_MEM,
                              &je.stack, sizeof(int), NULL,
                              JSON_DEPTH_DEFAULT, JSON_DEPTH_INC, MYF(0));

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
  bool end_ok= false;
  String *value = args[0]->val_json(&tmp_js);

  if ((null_value= !value))
    return nullptr;

  buf->length(0);
  buf->set_charset(&my_charset_bin);
  CHARSET_INFO *cs= value->charset();
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
        float4store(f_bin, f);
        buf->append(f_bin, sizeof(f_bin));
        break;
      }
      default:
        goto error_format;
    }
  } while (json_scan_next(&je) == 0);

  if (!end_ok)
    goto error_format;

  if (Type_handler_vector::is_valid(buf->ptr(), buf->length()))
    return buf;

  null_value= true;
  push_warning_printf(current_thd, Sql_condition::WARN_LEVEL_WARN,
                      ER_TRUNCATED_WRONG_VALUE, ER(ER_TRUNCATED_WRONG_VALUE),
                      "vector", value->c_ptr_safe());
  return nullptr;

error_format:
  {
    int position= (int)((const char *) je.s.c_str - value->ptr());
    null_value= true;
    push_warning_printf(current_thd, Sql_condition::WARN_LEVEL_WARN,
                        ER_VECTOR_FORMAT_INVALID, ER(ER_VECTOR_FORMAT_INVALID),
                        position, value->c_ptr_safe());
    return nullptr;
  }

error:
  report_json_error_ex(value->ptr(), &je, func_name(),
                       0, Sql_condition::WARN_LEVEL_WARN);
  null_value= true;
  return nullptr;
}
