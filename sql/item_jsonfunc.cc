/* Copyright (c) 2016, Monty Program Ab.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA */


#include <my_global.h>
#include "sql_priv.h"
#include "sql_class.h"
#include "item.h"


/*
  Compare ASCII string against the string with the specified
  character set.
  Only compares the equality, case insencitive.
*/
static bool eq_ascii_string(const CHARSET_INFO *cs,
                            const char *ascii,
                            const char *s,  uint32 s_len)
{
  const char *s_end= s + s_len;

  while (*ascii && s < s_end)
  {
    my_wc_t wc;
    int wc_len;

    wc_len= cs->cset->mb_wc(cs, &wc, (uchar *) s, (uchar *) s_end);
    if (wc_len <= 0 || (wc | 0x20) != (my_wc_t) *ascii)
      return 0;

    ascii++;
    s+= wc_len;
  }

  return *ascii == 0 && s >= s_end;
}


/*
  Appends arbitrary String to the JSON string taking charsets in
  consideration.
*/
static int st_append_escaped(String *s, const String *a)
{
  /*
    In the worst case one character from the 'a' string
    turns into '\uXXXX\uXXXX' which is 12.
  */
  int str_len= a->length() * 12 * s->charset()->mbmaxlen /
               a->charset()->mbminlen;
  if (!s->reserve(str_len, 1024) &&
      (str_len=
         json_escape(a->charset(), (uchar *) a->ptr(), (uchar *)a->end(),
                     s->charset(),
                     (uchar *) s->end(), (uchar *)s->end() + str_len)) > 0)
  {
    s->length(s->length() + str_len);
    return 0;
  }

  return a->length();
}


longlong Item_func_json_valid::val_int()
{
  String *js= args[0]->val_str(&tmp_value);
  json_engine_t je;

  if ((null_value= args[0]->null_value) || js == NULL)
    return 0;

  json_scan_start(&je, js->charset(), (const uchar *) js->ptr(),
                  (const uchar *) js->ptr()+js->length());

  while (json_scan_next(&je) == 0) {}

  return je.s.error == 0;
}


void Item_func_json_exists::fix_length_and_dec()
{
  Item_int_func::fix_length_and_dec();
  maybe_null= 1;
  path.set_constant_flag(args[1]->const_item());
}


longlong Item_func_json_exists::val_int()
{
  json_engine_t je;
  uint array_counters[JSON_DEPTH_LIMIT];

  String *js= args[0]->val_str(&tmp_js);

  if (!path.parsed)
  {
    String *s_p= args[1]->val_str(&tmp_path);
    if (s_p &&
        json_path_setup(&path.p, s_p->charset(), (const uchar *) s_p->ptr(),
                        (const uchar *) s_p->ptr() + s_p->length()))
      goto err_return;
    path.parsed= path.constant;
  }

  if ((null_value= args[0]->null_value || args[1]->null_value))
  {
    null_value= 1;
    return 0;
  }

  null_value= 0;
  json_scan_start(&je, js->charset(),(const uchar *) js->ptr(),
                  (const uchar *) js->ptr() + js->length());

  path.cur_step= path.p.steps;
  if (json_find_path(&je, &path.p, &path.cur_step, array_counters))
  {
    if (je.s.error)
      goto err_return;
    return 0;
  }

  return 1;

err_return:
  null_value= 1;
  return 0;
}


void Item_func_json_value::fix_length_and_dec()
{
  collation.set(args[0]->collation);
  max_length= args[0]->max_length;
  path.set_constant_flag(args[1]->const_item());
}


/*
  Returns NULL, not an error if the found value
  is not a scalar.
*/
String *Item_func_json_value::val_str(String *str)
{
  json_engine_t je;
  String *js= args[0]->val_str(&tmp_js);
  int error= 0;
  uint array_counters[JSON_DEPTH_LIMIT];

  if (!path.parsed)
  {
    String *s_p= args[1]->val_str(&tmp_path);
    if (s_p &&
        json_path_setup(&path.p, s_p->charset(), (const uchar *) s_p->ptr(),
                        (const uchar *) s_p->ptr() + s_p->length()))
      goto err_return;
    path.parsed= path.constant;
  }

  if ((null_value= args[0]->null_value || args[1]->null_value))
    return NULL;

  json_scan_start(&je, js->charset(),(const uchar *) js->ptr(),
                  (const uchar *) js->ptr() + js->length());

  path.cur_step= path.p.steps;
continue_search:
  if (json_find_path(&je, &path.p, &path.cur_step, array_counters))
  {
    if (je.s.error)
      goto err_return;

    null_value= 1;
    return 0;
  }

  if (json_read_value(&je))
    goto err_return;

  if (check_and_get_value(&je, str, &error))
  {
    if (error)
      goto err_return;
    goto continue_search;
  }

  return str;

err_return:
  null_value= 1;
  return 0;
}


bool Item_func_json_value::check_and_get_value(json_engine_t *je, String *res,
                                               int *error)
{
  if (!json_value_scalar(je))
  {
    /* We only look for scalar values! */
    if (json_skip_level(je) || json_scan_next(je))
      *error= 1;
    return true;
  }

  res->set((const char *) je->value, je->value_len, je->s.cs);
  return false;
}


bool Item_func_json_query::check_and_get_value(json_engine_t *je, String *res,
                                               int *error)
{
  const uchar *value;
  if (json_value_scalar(je))
  {
    /* We skip scalar values. */
    if (json_scan_next(je))
      *error= 1;
    return true;
  }

  value= je->value;
  if (json_skip_level(je))
  {
    *error= 1;
    return true;
  }

  res->set((const char *) je->value, je->s.c_str - value, je->s.cs);
  return false;
}


void Item_func_json_quote::fix_length_and_dec()
{
  collation.set(args[0]->collation);
  /*
    Odd but realistic worst case is when all characters
    of the argument turn into '\uXXXX\uXXXX', which is 12.
  */
  max_length= args[0]->max_length * 12;
}


String *Item_func_json_quote::val_str(String *str)
{
  String *s= args[0]->val_str(&tmp_s);

  if ((null_value= args[0]->null_value))
    return NULL;

  str->length(0);
  str->set_charset(s->charset());

  if (st_append_escaped(str, s))
  {
    /* Report an error. */
    null_value= 1;
    return 0;
  }

  return str;
}


static int alloc_tmp_paths(THD *thd, uint n_paths,
                           json_path_with_flags **paths,String **tmp_paths)
{
  if (n_paths > 0)
  {
    *paths= (json_path_with_flags *) alloc_root(thd->mem_root,
        sizeof(json_path_with_flags) * n_paths);
    *tmp_paths= (String *) alloc_root(thd->mem_root, sizeof(String) * n_paths);
    if (*paths == 0 || *tmp_paths == 0)
      return 1;

    bzero(*tmp_paths, sizeof(String) * n_paths);

    return 0;
  }

  /* n_paths == 0 */
  *paths= 0;
  *tmp_paths= 0;
  return 0;
}


static void mark_constant_paths(json_path_with_flags *p,
                                Item** args, uint n_args)
{
  uint n;
  for (n= 0; n < n_args; n++)
    p[n].set_constant_flag(args[n]->const_item());
}


bool Item_json_str_multipath::fix_fields(THD *thd, Item **ref)
{
  return alloc_tmp_paths(thd, get_n_paths(), &paths, &tmp_paths) ||
         Item_str_func::fix_fields(thd, ref);
}


void Item_json_str_multipath::cleanup()
{
  if (tmp_paths)
  {
    for (uint i= get_n_paths(); i>0; i--)
      tmp_paths[i-1].free();
    tmp_paths= 0;
  }
  Item_str_func::cleanup();
}


void Item_func_json_extract::fix_length_and_dec()
{
  collation.set(args[0]->collation);
  max_length= args[0]->max_length * (arg_count - 1);

  mark_constant_paths(paths, args+1, arg_count-1);
}


String *Item_func_json_extract::val_str(String *str)
{
  String *js= args[0]->val_str(&tmp_js);
  json_engine_t je;
  bool multiple_values_found= FALSE;
  const uchar *value;
  const char *first_value= NULL, *first_p_value;
  uint n_arg, v_len, first_len, first_p_len;
  uint array_counters[JSON_DEPTH_LIMIT];

  if ((null_value= args[0]->null_value))
    return 0;

  str->set_charset(js->charset());
  str->length(0);

  for (n_arg=1; n_arg < arg_count; n_arg++)
  {
    json_path_with_flags *c_path= paths + n_arg - 1;
    if (!c_path->parsed)
    {
      String *s_p= args[n_arg]->val_str(tmp_paths + (n_arg-1));
      if (s_p &&
          json_path_setup(&c_path->p,s_p->charset(),(const uchar *) s_p->ptr(),
                          (const uchar *) s_p->ptr() + s_p->length()))
        goto error;
      c_path->parsed= c_path->constant;
    }

    json_scan_start(&je, js->charset(),(const uchar *) js->ptr(),
                    (const uchar *) js->ptr() + js->length());

    c_path->cur_step= c_path->p.steps;

    if (json_find_path(&je, &c_path->p, &c_path->cur_step, array_counters))
    {
      /* Path wasn't found. */
      if (je.s.error)
        goto error;

      continue;
    }

    if (json_read_value(&je))
      goto error;

    value= je.value_begin;
    if (json_value_scalar(&je))
      v_len= je.value_end - value;
    else
    {
      if (json_skip_level(&je))
        goto error;
      v_len= je.s.c_str - value;
    }

    if (!multiple_values_found)
    {
      if (first_value == NULL)
      {
        /*
          Just remember the first value as we don't know yet
          if we need to create an array out of it or not.
        */
        first_value= (const char *) value;
        first_len= v_len;
        /*
         We need this as we have to preserve quotes around string
          constants if we use the value to create an array. Otherwise
          we get the value without the quotes.
        */
        first_p_value= (const char *) je.value;
        first_p_len= je.value_len;
        continue;
      }
      else
      {
        multiple_values_found= TRUE; /* We have to make an JSON array. */
        if (str->append("[", 1) ||
            str->append(first_value, first_len))
          goto error; /* Out of memory. */
      }

    }
    if (str->append(", ", 2) ||
        str->append((const char *) value, v_len))
      goto error; /* Out of memory. */
  }

  if (first_value == NULL)
  {
    /* Nothing was found. */
    null_value= 1;
    return 0;
  }

  if (multiple_values_found ?
        str->append("]") :
        str->append(first_p_value, first_p_len))
    goto error; /* Out of memory. */

  return str;

error:
  /* TODO: launch error messages. */
  null_value= 1;
  return 0;
}


longlong Item_func_json_extract::val_int()
{
  String *js= args[0]->val_str(&tmp_js);
  json_engine_t je;
  uint n_arg;
  uint array_counters[JSON_DEPTH_LIMIT];

  if ((null_value= args[0]->null_value))
    return 0;

  for (n_arg=1; n_arg < arg_count; n_arg++)
  {
    json_path_with_flags *c_path= paths + n_arg - 1;
    if (!c_path->parsed)
    {
      String *s_p= args[n_arg]->val_str(tmp_paths+(n_arg-1));
      if (s_p &&
          json_path_setup(&c_path->p,s_p->charset(),(const uchar *) s_p->ptr(),
                          (const uchar *) s_p->ptr() + s_p->length()))
        goto error;
      c_path->parsed= c_path->constant;
    }

    json_scan_start(&je, js->charset(),(const uchar *) js->ptr(),
                    (const uchar *) js->ptr() + js->length());

    c_path->cur_step= c_path->p.steps;

    if (json_find_path(&je, &c_path->p, &c_path->cur_step, array_counters))
    {
      /* Path wasn't found. */
      if (je.s.error)
        goto error;

      continue;
    }

    if (json_read_value(&je))
      goto error;

    if (json_value_scalar(&je))
    {
      int err;
      char *v_end= (char *) je.value_end;
      return (je.s.cs->cset->strtoll10)(je.s.cs, (const char *) je.value_begin,
                                        &v_end, &err);
    }
    else
      break;
  }

  /* Nothing was found. */
  null_value= 1;
  return 0;

error:
  /* TODO: launch error messages. */
  null_value= 1;
  return 0;
}


bool Item_func_json_contains::fix_fields(THD *thd, Item **ref)
{
  return alloc_tmp_paths(thd, arg_count-2, &paths, &tmp_paths) ||
         Item_int_func::fix_fields(thd, ref);
}


void Item_func_json_contains::fix_length_and_dec()
{
  a2_constant= args[1]->const_item();
  a2_parsed= FALSE;
  mark_constant_paths(paths, args+2, arg_count-2);
  Item_int_func::fix_length_and_dec();
}


void Item_func_json_contains::cleanup()
{
  if (tmp_paths)
  {
    for (uint i= arg_count-2; i>0; i--)
      tmp_paths[i-1].free();
    tmp_paths= 0;
  }
  Item_int_func::cleanup();
}


longlong Item_func_json_contains::val_int()
{
  String *js= args[0]->val_str(&tmp_js);
  json_engine_t je;
  uint n_arg;

  if ((null_value= args[0]->null_value))
    return 0;

  if (!a2_parsed)
  {
    val= args[1]->val_str(&tmp_val);
    a2_parsed= a2_constant;
  }

  if (val == 0)
  {
    null_value= 1;
    return 0;
  }

  json_scan_start(&je, js->charset(),(const uchar *) js->ptr(),
                  (const uchar *) js->ptr() + js->length());

  if (arg_count<3) /* No path specified. */
  {
    if (json_read_value(&je))
      goto error;
    String jv_str((const char *)je.value_begin,
                  je.value_end - je.value_begin, js->charset());
    return val->eq(&jv_str, js->charset());
  }

  for (n_arg=2; n_arg < arg_count; n_arg++)
  {
    uint array_counters[JSON_DEPTH_LIMIT];
    json_path_with_flags *c_path= paths + n_arg - 2;
    if (!c_path->parsed)
    {
      String *s_p= args[n_arg]->val_str(tmp_paths+(n_arg-2));
      if (s_p &&
          json_path_setup(&c_path->p,s_p->charset(),(const uchar *) s_p->ptr(),
                          (const uchar *) s_p->ptr() + s_p->length()))
        goto error;
      c_path->parsed= c_path->constant;
    }

    json_scan_start(&je, js->charset(),(const uchar *) js->ptr(),
                    (const uchar *) js->ptr() + js->length());

    c_path->cur_step= c_path->p.steps;
    if (json_find_path(&je, &c_path->p, &c_path->cur_step, array_counters))
    {
      /* Path wasn't found. */
      if (je.s.error)
        goto error;
      continue;
    }

    if (json_read_value(&je))
      goto error;
    String jv_str((const char *)je.value_begin,
                  je.value_end - je.value_begin, js->charset());
    if (val->eq(&jv_str, js->charset()))
      return 1;
  }


  return 0;

error:
  null_value= 1;
  return 0;
}


bool Item_func_json_contains_path::fix_fields(THD *thd, Item **ref)
{
  return alloc_tmp_paths(thd, arg_count-2, &paths, &tmp_paths) ||
         Item_int_func::fix_fields(thd, ref);
}


void Item_func_json_contains_path::fix_length_and_dec()
{
  ooa_constant= args[1]->const_item();
  ooa_parsed= FALSE;
  mark_constant_paths(paths, args+2, arg_count-2);
  Item_int_func::fix_length_and_dec();
}


void Item_func_json_contains_path::cleanup()
{
  if (tmp_paths)
  {
    for (uint i= arg_count-2; i>0; i--)
      tmp_paths[i-1].free();
    tmp_paths= 0;
  }
  Item_int_func::cleanup();
}


longlong Item_func_json_contains_path::val_int()
{
  String *js= args[0]->val_str(&tmp_js);
  json_engine_t je;
  uint n_arg;
  longlong result;

  if ((null_value= args[0]->null_value))
    return 0;

  if (!ooa_parsed)
  {
    char buff[20];
    String *res, tmp(buff, sizeof(buff), &my_charset_bin);
    res= args[1]->val_str(&tmp);
    mode_one=eq_ascii_string(res->charset(), "one",
                             res->ptr(), res->length());
    if (!mode_one)
    {
      if (!eq_ascii_string(res->charset(), "all", res->ptr(), res->length()))
        goto error;
    }
    ooa_parsed= ooa_constant;
  }

  result= !mode_one;
  for (n_arg=2; n_arg < arg_count; n_arg++)
  {
    uint array_counters[JSON_DEPTH_LIMIT];
    json_path_with_flags *c_path= paths + n_arg - 2;
    if (!c_path->parsed)
    {
      String *s_p= args[n_arg]->val_str(tmp_paths+(n_arg-2));
      if (s_p &&
          json_path_setup(&c_path->p,s_p->charset(),(const uchar *) s_p->ptr(),
                          (const uchar *) s_p->ptr() + s_p->length()))
        goto error;
      c_path->parsed= c_path->constant;
    }

    json_scan_start(&je, js->charset(),(const uchar *) js->ptr(),
                    (const uchar *) js->ptr() + js->length());

    c_path->cur_step= c_path->p.steps;
    if (json_find_path(&je, &c_path->p, &c_path->cur_step, array_counters))
    {
      /* Path wasn't found. */
      if (je.s.error)
        goto error;

      if (!mode_one)
      {
        result= 0;
        break;
      }
    }
    else if (mode_one)
    {
      result= 1;
      break;
    }
  }


  return result;

error:
  null_value= 1;
  return 0;
}


static int append_json_value(String *str, Item *item, String *tmp_val)
{
  if (item->is_bool_type())
  {
    longlong v_int= item->val_int();
    const char *t_f;
    int t_f_len;

    if (item->null_value)
      goto append_null;

    if (v_int)
    {
      t_f= "true";
      t_f_len= 4;
    }
    else
    {
      t_f= "false";
      t_f_len= 5;
    }

    return str->append(t_f, t_f_len);
  }
  {
    String *sv= item->val_str(tmp_val);
    if (item->null_value)
      goto append_null;
    if (item->result_type() == STRING_RESULT)
    {
      return str->append("\"", 1) ||
             st_append_escaped(str, sv) ||
             str->append("\"", 1);
    }
    return st_append_escaped(str, sv);
  }

append_null:
  return str->append("null", 4);
}


static int append_json_keyname(String *str, Item *item, String *tmp_val)
{
  String *sv= item->val_str(tmp_val);
  if (item->null_value)
    goto append_null;

  return str->append("\"", 1) ||
         st_append_escaped(str, sv) ||
         str->append("\": ", 3);

append_null:
  return str->append("\"\": ", 4);
}


void Item_func_json_array::fix_length_and_dec()
{
  ulonglong char_length= 4;
  uint n_arg;

  if (agg_arg_charsets_for_string_result(collation, args, arg_count))
    return;

  for (n_arg=0 ; n_arg < arg_count ; n_arg++)
    char_length+= args[n_arg]->max_char_length() + 2;

  fix_char_length_ulonglong(char_length);
  tmp_val.set_charset(collation.collation);
}


String *Item_func_json_array::val_str(String *str)
{
  DBUG_ASSERT(fixed == 1);
  uint n_arg;

  str->length(0);

  if (str->append("[", 1) ||
      ((arg_count > 0) && append_json_value(str, args[0],&tmp_val)))
    goto err_return;

  for (n_arg=1; n_arg < arg_count; n_arg++)
  {
    if (str->append(", ", 2) ||
        append_json_value(str, args[n_arg], &tmp_val))
      goto err_return;
  }

  if (str->append("]", 1))
    goto err_return;

  return str;

err_return:
  /*TODO: Launch out of memory error. */
  null_value= 1;
  return NULL;
}


void Item_func_json_array_append::fix_length_and_dec()
{
  uint n_arg;
  ulonglong char_length;

  collation.set(args[0]->collation);
  char_length= args[0]->max_char_length();

  for (n_arg= 1; n_arg < arg_count; n_arg+= 2)
  {
    paths[n_arg-1].set_constant_flag(args[n_arg]->const_item());
    char_length+= args[n_arg+1]->max_char_length() + 4;
  }

  fix_char_length_ulonglong(char_length);
}


String *Item_func_json_array_append::val_str(String *str)
{
  json_engine_t je;
  String *js= args[0]->val_str(&tmp_js);
  uint n_arg, n_path, str_rest_len;
  const uchar *ar_end;

  DBUG_ASSERT(fixed == 1);

  if ((null_value= args[0]->null_value))
    return 0;

  for (n_arg=1, n_path=0; n_arg < arg_count; n_arg+=2, n_path++)
  {
    uint array_counters[JSON_DEPTH_LIMIT];
    json_path_with_flags *c_path= paths + n_path;
    if (!c_path->parsed)
    {
      String *s_p= args[n_arg]->val_str(tmp_paths+n_path);
      if (s_p &&
          json_path_setup(&c_path->p,s_p->charset(),(const uchar *) s_p->ptr(),
                          (const uchar *) s_p->ptr() + s_p->length()))
        goto error;
      c_path->parsed= c_path->constant;
    }
    if (args[n_arg]->null_value)
    {
      null_value= 1;
      return 0;
    }

    json_scan_start(&je, js->charset(),(const uchar *) js->ptr(),
                    (const uchar *) js->ptr() + js->length());

    c_path->cur_step= c_path->p.steps;

    if (json_find_path(&je, &c_path->p, &c_path->cur_step, array_counters))
    {
      if (je.s.error)
        goto error;
      null_value= 1;
      return 0;
    }

    if (json_read_value(&je))
      goto error;

    if (je.value_type != JSON_VALUE_ARRAY)
    {
      /* Must be an array. */
      goto error;
    }

    if (json_skip_level(&je))
      goto error;

    str->length(0);
    str->set_charset(js->charset());
    if (str->reserve(js->length() + 8, 1024))
      goto error; /* Out of memory. */
    ar_end= je.s.c_str - je.sav_c_len;
    str_rest_len= js->length() - (ar_end - (const uchar *) js->ptr());
    str->q_append(js->ptr(), ar_end-(const uchar *) js->ptr());
    str->append(", ", 2);
    if (append_json_value(str, args[n_arg+1], &tmp_val))
      goto error; /* Out of memory. */

    if (str->reserve(str_rest_len, 1024))
      goto error; /* Out of memory. */
    str->q_append((const char *) ar_end, str_rest_len);
  }

  return str;

error:
  null_value= 1;
  return 0;
}


String *Item_func_json_object::val_str(String *str)
{
  DBUG_ASSERT(fixed == 1);
  uint n_arg;

  str->length(0);

  if (str->append("{", 1) ||
      (arg_count > 0 &&
       (append_json_keyname(str, args[0], &tmp_val) ||
        append_json_value(str, args[1], &tmp_val))))
    goto err_return;

  for (n_arg=2; n_arg < arg_count; n_arg+=2)
  {
    if (str->append(", ", 2) ||
        append_json_keyname(str, args[n_arg], &tmp_val) ||
        append_json_value(str, args[n_arg+1], &tmp_val))
      goto err_return;
  }

  if (str->append("}", 1))
    goto err_return;

  return str;

err_return:
  /*TODO: Launch out of memory error. */
  null_value= 1;
  return NULL;
}


String *Item_func_json_merge::val_str(String *str)
{
  DBUG_ASSERT(fixed == 1);
  uint n_arg;

  str->length(0);

  if (str->append("[", 1) ||
      ((arg_count > 0) && append_json_value(str, args[0], &tmp_val)))
    goto err_return;

  for (n_arg=1; n_arg < arg_count; n_arg++)
  {
    if (str->append(", ", 2) ||
        append_json_value(str, args[n_arg], &tmp_val))
      goto err_return;
  }

  if (str->append("]", 1))
    goto err_return;

  return str;

err_return:
  /*TODO: Launch out of memory error. */
  null_value= 1;
  return NULL;
}


longlong Item_func_json_length::val_int()
{
  String *js= args[0]->val_str(&tmp_js);
  json_engine_t je;
  uint length= 0;

  if ((null_value= args[0]->null_value))
    return 0;


  json_scan_start(&je, js->charset(),(const uchar *) js->ptr(),
                  (const uchar *) js->ptr() + js->length());

  do
  {
    if (je.state == JST_VALUE)
      length++;
  } while (json_scan_next(&je) == 0);

  if (je.s.error)
  {
    null_value= 1;
    return 0;
  }
    
  return length;

}


longlong Item_func_json_depth::val_int()
{
  String *js= args[0]->val_str(&tmp_js);
  json_engine_t je;
  uint depth= 0;
  bool inc_depth= TRUE;

  if ((null_value= args[0]->null_value))
    return 0;


  json_scan_start(&je, js->charset(),(const uchar *) js->ptr(),
                  (const uchar *) js->ptr() + js->length());

  do
  {
    switch (je.state)
    {
    case JST_VALUE:
      if (inc_depth)
      {
        depth++;
        inc_depth= FALSE;
      }
      break;
    case JST_OBJ_START:
    case JST_ARRAY_START:
      inc_depth= TRUE;
      break;
    default:
      break;
    }
  } while (json_scan_next(&je) == 0);

  if (je.s.error)
  {
    null_value= 1;
    return 0;
  }
    
  return depth;
}


void Item_func_json_type::fix_length_and_dec()
{
  collation.set(&my_charset_utf8_general_ci);
  max_length= 12;
}


String *Item_func_json_type::val_str(String *str)
{
  String *js= args[0]->val_str(&tmp_js);
  json_engine_t je;
  const char *type;

  if ((null_value= args[0]->null_value))
    return 0;


  json_scan_start(&je, js->charset(),(const uchar *) js->ptr(),
                  (const uchar *) js->ptr() + js->length());

  if (json_read_value(&je))
    goto error;

  switch (je.value_type)
  {
  case JSON_VALUE_OBJECT:
    type= "OBJECT";
    break;
  case JSON_VALUE_ARRAY:
    type= "ARRAY";
    break;
  case JSON_VALUE_STRING:
    type= "STRING";
    break;
  case JSON_VALUE_NUMBER:
    type= "NUMBER";
    break;
  case JSON_VALUE_TRUE:
  case JSON_VALUE_FALSE:
    type= "BOOLEAN";
    break;
  default:
    type= "NULL";
    break;
  }

  str->set(type, strlen(type), &my_charset_utf8_general_ci);
  return str;

error:
  null_value= 1;
  return 0;
}


