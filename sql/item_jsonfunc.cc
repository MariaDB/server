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


#include "mariadb.h"
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


static bool append_simple(String *s, const char *a, size_t a_len)
{
  if (!s->realloc_with_extra_if_needed(s->length() + a_len))
  {
    s->q_append(a, a_len);
    return FALSE;
  }

  return TRUE;
}


static inline bool append_simple(String *s, const uchar *a, size_t a_len)
{
  return append_simple(s, (const char *) a, a_len);
}


/*
  Appends JSON string to the String object taking charsets in
  consideration.
*/
static int st_append_json(String *s,
             CHARSET_INFO *json_cs, const uchar *js, uint js_len)
{
  int str_len= js_len * s->charset()->mbmaxlen;

  if (!s->reserve(str_len, 1024) &&
      (str_len= json_unescape(json_cs, js, js + js_len,
         s->charset(), (uchar *) s->end(), (uchar *) s->end() + str_len)) > 0)
  {
    s->length(s->length() + str_len);
    return 0;
  }

  return str_len;
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


static const int TAB_SIZE_LIMIT= 8;
static const char tab_arr[TAB_SIZE_LIMIT+1]= "        ";

static int append_tab(String *js, int depth, int tab_size)
{
  if (js->append("\n", 1))
    return 1;
  for (int i=0; i<depth; i++)
  {
    if (js->append(tab_arr, tab_size))
      return 1;
  }
  return 0;
}


static int json_nice(json_engine_t *je, String *nice_js,
                     Item_func_json_format::formats mode, int tab_size=4)
{
  int depth= 0;
  const char *comma, *colon;
  uint comma_len, colon_len;
  int first_value= 1;

  DBUG_ASSERT(je->s.cs == nice_js->charset());
  DBUG_ASSERT(mode != Item_func_json_format::DETAILED ||
              (tab_size >= 0 && tab_size <= TAB_SIZE_LIMIT));

  comma= ", ";
  colon= "\": ";
  if (mode == Item_func_json_format::LOOSE)
  {
    comma_len= 2;
    colon_len= 3;
  }
  else if (mode == Item_func_json_format::DETAILED)
  {
    comma_len= 1;
    colon_len= 3;
  }
  else
  {
    comma_len= 1;
    colon_len= 2;
  }

  do
  {
    switch (je->state)
    {
    case JST_KEY:
      {
        const uchar *key_start= je->s.c_str;
        const uchar *key_end;

        do
        {
          key_end= je->s.c_str;
        } while (json_read_keyname_chr(je) == 0);
        
        if (unlikely(je->s.error))
          goto error;

        if (!first_value)
          nice_js->append(comma, comma_len);

        if (mode == Item_func_json_format::DETAILED &&
            append_tab(nice_js, depth, tab_size))
          goto error;

        nice_js->append("\"", 1);
        append_simple(nice_js, key_start, key_end - key_start);
        nice_js->append(colon, colon_len);
      }
      /* now we have key value to handle, so no 'break'. */
      DBUG_ASSERT(je->state == JST_VALUE);
      goto handle_value;

    case JST_VALUE:
      if (!first_value)
        nice_js->append(comma, comma_len);

      if (mode == Item_func_json_format::DETAILED &&
          depth > 0 &&
          append_tab(nice_js, depth, tab_size))
        goto error;

handle_value:
      if (json_read_value(je))
        goto error;
      if (json_value_scalar(je))
      {
        if (append_simple(nice_js, je->value_begin,
                          je->value_end - je->value_begin))
          goto error;

        first_value= 0;
      }
      else
      {
        if (mode == Item_func_json_format::DETAILED &&
            depth > 0 &&
            append_tab(nice_js, depth, tab_size))
          goto error;
        nice_js->append((je->value_type == JSON_VALUE_OBJECT) ? "{" : "[", 1);
        first_value= 1;
        depth++;
      }

      break;

    case JST_OBJ_END:
    case JST_ARRAY_END:
      depth--;
      if (mode == Item_func_json_format::DETAILED &&
          append_tab(nice_js, depth, tab_size))
        goto error;
      nice_js->append((je->state == JST_OBJ_END) ? "}": "]", 1);
      first_value= 0;
      break;

    default:
      break;
    };
  } while (json_scan_next(je) == 0);

  return je->s.error;

error:
  return 1;
}


#define report_json_error(js, je, n_param) \
  report_json_error_ex(js, je, func_name(), n_param, \
      Sql_condition::WARN_LEVEL_WARN)

void report_json_error_ex(String *js, json_engine_t *je,
                          const char *fname, int n_param,
                          Sql_condition::enum_warning_level lv)
{
  THD *thd= current_thd;
  int position= (int)((const char *) je->s.c_str - js->ptr());
  uint code;

  n_param++;

  switch (je->s.error)
  {
  case JE_BAD_CHR:
    code= ER_JSON_BAD_CHR;
    break;

  case JE_NOT_JSON_CHR:
    code= ER_JSON_NOT_JSON_CHR;
    break;

  case JE_EOS:
    code= ER_JSON_EOS;
    break;

  case JE_SYN:
  case JE_STRING_CONST:
    code= ER_JSON_SYNTAX;
    break;

  case JE_ESCAPING:
    code= ER_JSON_ESCAPING;
    break;

  case JE_DEPTH:
    code= ER_JSON_DEPTH;
    push_warning_printf(thd, lv, code, ER_THD(thd, code), JSON_DEPTH_LIMIT,
                        n_param, fname, position);
    return;

  default:
    return;
  }

  push_warning_printf(thd, lv, code, ER_THD(thd, code),
                      n_param, fname, position);
}



#define NO_WILDCARD_ALLOWED 1
#define SHOULD_END_WITH_ARRAY 2
#define TRIVIAL_PATH_NOT_ALLOWED 3

#define report_path_error(js, je, n_param) \
  report_path_error_ex(js, je, func_name(), n_param,\
      Sql_condition::WARN_LEVEL_WARN)

static void report_path_error_ex(String *ps, json_path_t *p,
                                 const char *fname, int n_param,
                                 Sql_condition::enum_warning_level lv)
{
  THD *thd= current_thd;
  int position= (int)((const char *) p->s.c_str - ps->ptr() + 1);
  uint code;

  n_param++;

  switch (p->s.error)
  {
  case JE_BAD_CHR:
  case JE_NOT_JSON_CHR:
  case JE_SYN:
    code= ER_JSON_PATH_SYNTAX;
    break;

  case JE_EOS:
    code= ER_JSON_PATH_EOS;
    break;

  case JE_DEPTH:
    code= ER_JSON_PATH_DEPTH;
    push_warning_printf(thd, lv, code, ER_THD(thd, code),
                        JSON_DEPTH_LIMIT, n_param, fname, position);
    return;

  case NO_WILDCARD_ALLOWED:
    code= ER_JSON_PATH_NO_WILDCARD;
    break;

  case TRIVIAL_PATH_NOT_ALLOWED:
    code= ER_JSON_PATH_EMPTY;
    break;


  default:
    return;
  }
  push_warning_printf(thd, lv, code, ER_THD(thd, code),
                      n_param, fname, position);
}



/*
  Checks if the path has '.*' '[*]' or '**' constructions
  and sets the NO_WILDCARD_ALLOWED error if the case.
*/
static int path_setup_nwc(json_path_t *p, CHARSET_INFO *i_cs,
                          const uchar *str, const uchar *end)
{
  if (!json_path_setup(p, i_cs, str, end))
  {
    if ((p->types_used & (JSON_PATH_WILD | JSON_PATH_DOUBLE_WILD)) == 0)
      return 0;
    p->s.error= NO_WILDCARD_ALLOWED;
  }

  return 1;
}


longlong Item_func_json_valid::val_int()
{
  String *js= args[0]->val_json(&tmp_value);

  if ((null_value= args[0]->null_value))
    return 0;

  return json_valid(js->ptr(), js->length(), js->charset());
}


bool Item_func_json_exists::fix_length_and_dec()
{
  if (Item_bool_func::fix_length_and_dec())
    return TRUE;
  maybe_null= 1;
  path.set_constant_flag(args[1]->const_item());
  return FALSE;
}


longlong Item_func_json_exists::val_int()
{
  json_engine_t je;
  uint array_counters[JSON_DEPTH_LIMIT];

  String *js= args[0]->val_json(&tmp_js);

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


bool Item_func_json_value::fix_length_and_dec()
{
  collation.set(args[0]->collation);
  max_length= args[0]->max_length;
  path.set_constant_flag(args[1]->const_item());
  maybe_null= 1;
  return FALSE;
}


/*
  Returns NULL, not an error if the found value
  is not a scalar.
*/
String *Item_func_json_value::val_str(String *str)
{
  json_engine_t je;
  String *js= args[0]->val_json(&tmp_js);
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

  str->length(0);
  str->set_charset(collation.collation);

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

  if (unlikely(check_and_get_value(&je, str, &error)))
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
  CHARSET_INFO *json_cs;
  const uchar *js;
  uint js_len;

  if (!json_value_scalar(je))
  {
    /* We only look for scalar values! */
    if (json_skip_level(je) || json_scan_next(je))
      *error= 1;
    return true;
  }

  if (je->value_type == JSON_VALUE_TRUE ||
      je->value_type == JSON_VALUE_FALSE)
  {
    json_cs= &my_charset_utf8mb4_bin;
    js= (const uchar *) ((je->value_type == JSON_VALUE_TRUE) ? "1" : "0");
    js_len= 1;
  }
  else
  {
    json_cs= je->s.cs;
    js= je->value;
    js_len= je->value_len;
  }


  return st_append_json(res, json_cs, js, js_len); 
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

  res->set((const char *) je->value, (uint32)(je->s.c_str - value), je->s.cs);
  return false;
}


bool Item_func_json_quote::fix_length_and_dec()
{
  collation.set(&my_charset_utf8mb4_bin);
  /*
    Odd but realistic worst case is when all characters
    of the argument turn into '\uXXXX\uXXXX', which is 12.
  */
  fix_char_length_ulonglong((ulonglong) args[0]->max_char_length() * 12 + 2);
  return FALSE;
}


String *Item_func_json_quote::val_str(String *str)
{
  String *s= args[0]->val_str(&tmp_s);

  if ((null_value= (args[0]->null_value ||
                    args[0]->result_type() != STRING_RESULT)))
    return NULL;

  str->length(0);
  str->set_charset(&my_charset_utf8mb4_bin);

  if (str->append("\"", 1) ||
      st_append_escaped(str, s) ||
      str->append("\"", 1))
  {
    /* Report an error. */
    null_value= 1;
    return 0;
  }

  return str;
}


bool Item_func_json_unquote::fix_length_and_dec()
{
  collation.set(&my_charset_utf8_general_ci,
                DERIVATION_COERCIBLE, MY_REPERTOIRE_ASCII);
  max_length= args[0]->max_length;
  maybe_null= 1;
  return FALSE;
}


String *Item_func_json_unquote::read_json(json_engine_t *je)
{
  String *js= args[0]->val_json(&tmp_s);

  if ((null_value= args[0]->null_value))
    return 0;

  json_scan_start(je, js->charset(),(const uchar *) js->ptr(),
                  (const uchar *) js->ptr() + js->length());

  je->value_type= (enum json_value_types) -1; /* To report errors right. */

  if (json_read_value(je))
    goto error;

  return js;

error:
  if (je->value_type == JSON_VALUE_STRING)
    report_json_error(js, je, 0);
  return js;
}


String *Item_func_json_unquote::val_str(String *str)
{
  json_engine_t je;
  int c_len;
  String *js;

  if (!(js= read_json(&je)))
    return NULL;

  if (unlikely(je.s.error) || je.value_type != JSON_VALUE_STRING)
    return js;

  str->length(0);
  str->set_charset(&my_charset_utf8_general_ci);

  if (str->realloc_with_extra_if_needed(je.value_len) ||
      (c_len= json_unescape(js->charset(),
        je.value, je.value + je.value_len,
        &my_charset_utf8_general_ci,
        (uchar *) str->ptr(), (uchar *) (str->ptr() + je.value_len))) < 0)
    goto error;

  str->length(c_len);
  return str;

error:
  report_json_error(js, &je, 0);
  return js;
}


static int alloc_tmp_paths(THD *thd, uint n_paths,
                           json_path_with_flags **paths, String **tmp_paths)
{
  if (n_paths > 0)
  {
    if (*tmp_paths == 0)
    {
      MEM_ROOT *root= thd->stmt_arena->mem_root;

      *paths= (json_path_with_flags *) alloc_root(root,
          sizeof(json_path_with_flags) * n_paths);

      *tmp_paths= new (root) String[n_paths];
      if (*paths == 0 || *tmp_paths == 0)
        return 1;

      for (uint c_path=0; c_path < n_paths; c_path++)
        (*tmp_paths)[c_path].set_charset(&my_charset_utf8_general_ci);
    }

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
  }
  Item_str_func::cleanup();
}


bool Item_func_json_extract::fix_length_and_dec()
{
  collation.set(args[0]->collation);
  max_length= args[0]->max_length * (arg_count - 1);

  mark_constant_paths(paths, args+1, arg_count-1);
  maybe_null= 1;
  return FALSE;
}


static bool path_exact(const json_path_with_flags *paths_list, int n_paths,
                       const json_path_t *p, json_value_types vt)
{
  for (; n_paths > 0; n_paths--, paths_list++)
  {
    if (json_path_compare(&paths_list->p, p, vt) == 0)
      return TRUE;
  }
  return FALSE;
}


static bool path_ok(const json_path_with_flags *paths_list, int n_paths,
                    const json_path_t *p, json_value_types vt)
{
  for (; n_paths > 0; n_paths--, paths_list++)
  {
    if (json_path_compare(&paths_list->p, p, vt) >= 0)
      return TRUE;
  }
  return FALSE;
}


String *Item_func_json_extract::read_json(String *str,
                                          json_value_types *type,
                                          char **out_val, int *value_len)
{
  String *js= args[0]->val_json(&tmp_js);
  json_engine_t je, sav_je;
  json_path_t p;
  const uchar *value;
  int not_first_value= 0;
  uint n_arg;
  size_t v_len;
  int possible_multiple_values;

  if ((null_value= args[0]->null_value))
    return 0;

  for (n_arg=1; n_arg < arg_count; n_arg++)
  {
    json_path_with_flags *c_path= paths + n_arg - 1;
    if (!c_path->parsed)
    {
      String *s_p= args[n_arg]->val_str(tmp_paths + (n_arg-1));
      if (s_p &&
          json_path_setup(&c_path->p,s_p->charset(),(const uchar *) s_p->ptr(),
                          (const uchar *) s_p->ptr() + s_p->length()))
      {
        report_path_error(s_p, &c_path->p, n_arg);
        goto return_null;
      }
      c_path->parsed= c_path->constant;
    }

    if (args[n_arg]->null_value)
      goto return_null;
  }

  possible_multiple_values= arg_count > 2 ||
    (paths[0].p.types_used & (JSON_PATH_WILD | JSON_PATH_DOUBLE_WILD));

  *type= possible_multiple_values ? JSON_VALUE_ARRAY : JSON_VALUE_NULL;

  if (str)
  {
    str->set_charset(js->charset());
    str->length(0);

    if (possible_multiple_values && str->append("[", 1))
      goto error;
  }

  json_get_path_start(&je, js->charset(),(const uchar *) js->ptr(),
                      (const uchar *) js->ptr() + js->length(), &p);

  while (json_get_path_next(&je, &p) == 0)
  {
    if (!path_exact(paths, arg_count-1, &p, je.value_type))
      continue;

    value= je.value_begin;

    if (*type == JSON_VALUE_NULL)
    {
      *type= je.value_type;
      *out_val= (char *) je.value;
      *value_len= je.value_len;
    }
    if (!str)
    {
      /* If str is NULL, we only care about the first found value. */
      goto return_ok;
    }

    if (json_value_scalar(&je))
      v_len= je.value_end - value;
    else
    {
      if (possible_multiple_values)
        sav_je= je;
      if (json_skip_level(&je))
        goto error;
      v_len= je.s.c_str - value;
      if (possible_multiple_values)
        je= sav_je;
    }

    if ((not_first_value && str->append(", ", 2)) ||
        str->append((const char *) value, v_len))
      goto error; /* Out of memory. */

    not_first_value= 1;

    if (!possible_multiple_values)
    {
      /* Loop to the end of the JSON just to make sure it's valid. */
      while (json_get_path_next(&je, &p) == 0) {}
      break;
    }
  }

  if (unlikely(je.s.error))
    goto error;

  if (!not_first_value)
  {
    /* Nothing was found. */
    goto return_null;
  }

  if (possible_multiple_values && str->append("]", 1))
    goto error; /* Out of memory. */

  js= str;
  json_scan_start(&je, js->charset(),(const uchar *) js->ptr(),
                  (const uchar *) js->ptr() + js->length());
  tmp_js.length(0);
  tmp_js.set_charset(js->charset());
  if (json_nice(&je, &tmp_js, Item_func_json_format::LOOSE))
    goto error;

return_ok:
  return &tmp_js;

error:
  report_json_error(js, &je, 0);
return_null:
  null_value= 1;
  return 0;
}


String *Item_func_json_extract::val_str(String *str)
{
  json_value_types type;
  char *value;
  int value_len;
  return read_json(str, &type, &value, &value_len);
}


longlong Item_func_json_extract::val_int()
{
  json_value_types type;
  char *value;
  int value_len;
  longlong i= 0;

  if (read_json(NULL, &type, &value, &value_len) != NULL)
  {
    switch (type)
    {
      case JSON_VALUE_NUMBER:
      case JSON_VALUE_STRING:
      {
        char *end;
        int err;
        i= my_strntoll(collation.collation, value, value_len, 10, &end, &err);
        break;
      }
      case JSON_VALUE_TRUE:
        i= 1;
        break;
      default:
        i= 0;
        break;
    };
  }
  return i;
}


double Item_func_json_extract::val_real()
{
  json_value_types type;
  char *value;
  int value_len;
  double d= 0.0;

  if (read_json(NULL, &type, &value, &value_len) != NULL)
  {
    switch (type)
    {
      case JSON_VALUE_STRING:
      case JSON_VALUE_NUMBER:
      {
        char *end;
        int err;
        d= my_strntod(collation.collation, value, value_len, &end, &err);
        break;
      }
      case JSON_VALUE_TRUE:
        d= 1.0;
        break;
      default:
        break;
    };
  }

  return d;
}


bool Item_func_json_contains::fix_length_and_dec()
{
  a2_constant= args[1]->const_item();
  a2_parsed= FALSE;
  maybe_null= 1;
  if (arg_count > 2)
    path.set_constant_flag(args[2]->const_item());
  return Item_bool_func::fix_length_and_dec();
}


static int find_key_in_object(json_engine_t *j, json_string_t *key)
{
  const uchar *c_str= key->c_str;

  while (json_scan_next(j) == 0 && j->state != JST_OBJ_END)
  {
    DBUG_ASSERT(j->state == JST_KEY);
    if (json_key_matches(j, key))
      return TRUE;
    if (json_skip_key(j))
      return FALSE;
    key->c_str= c_str;
  }

  return FALSE;
}


static int check_contains(json_engine_t *js, json_engine_t *value)
{
  json_engine_t loc_js;
  bool set_js;

  switch (js->value_type)
  {
  case JSON_VALUE_OBJECT:
  {
    json_string_t key_name;

    if (value->value_type != JSON_VALUE_OBJECT)
      return FALSE;

    loc_js= *js;
    set_js= FALSE;
    json_string_set_cs(&key_name, value->s.cs);
    while (json_scan_next(value) == 0 && value->state != JST_OBJ_END)
    {
      const uchar *k_start, *k_end;

      DBUG_ASSERT(value->state == JST_KEY);
      k_start= value->s.c_str;
      do
      {
        k_end= value->s.c_str;
      } while (json_read_keyname_chr(value) == 0);

      if (unlikely(value->s.error) || json_read_value(value))
        return FALSE;

      if (set_js)
        *js= loc_js;
      else
        set_js= TRUE;

      json_string_set_str(&key_name, k_start, k_end);
      if (!find_key_in_object(js, &key_name) ||
          json_read_value(js) ||
          !check_contains(js, value))
        return FALSE;
    }

    return value->state == JST_OBJ_END && !json_skip_level(js);
  }
  case JSON_VALUE_ARRAY:
    if (value->value_type != JSON_VALUE_ARRAY)
    {
      loc_js= *value;
      set_js= FALSE;
      while (json_scan_next(js) == 0 && js->state != JST_ARRAY_END)
      {
        int c_level, v_scalar;
        DBUG_ASSERT(js->state == JST_VALUE);
        if (json_read_value(js))
          return FALSE;

        if (!(v_scalar= json_value_scalar(js)))
          c_level= json_get_level(js);

        if (set_js)
          *value= loc_js;
        else
          set_js= TRUE;

        if (check_contains(js, value))
        {
          if (json_skip_level(js))
            return FALSE;
          return TRUE;
        }
        if (unlikely(value->s.error) || unlikely(js->s.error) ||
            (!v_scalar && json_skip_to_level(js, c_level)))
          return FALSE;
      }
      return FALSE;
    }
    /* else */
    loc_js= *js;
    set_js= FALSE;
    while (json_scan_next(value) == 0 && value->state != JST_ARRAY_END)
    {
      DBUG_ASSERT(value->state == JST_VALUE);
      if (json_read_value(value))
        return FALSE;

      if (set_js)
        *js= loc_js;
      else
        set_js= TRUE;
      if (!check_contains(js, value))
        return FALSE;
    }

    return value->state == JST_ARRAY_END;

  case JSON_VALUE_STRING:
    if (value->value_type != JSON_VALUE_STRING)
      return FALSE;
    /*
       TODO: make proper json-json comparison here that takes excapint
             into account.
     */
    return value->value_len == js->value_len &&
           memcmp(value->value, js->value, value->value_len) == 0;
  case JSON_VALUE_NUMBER:
    if (value->value_type == JSON_VALUE_NUMBER)
    {
      double d_j, d_v;
      char *end;
      int err;

      d_j= my_strntod(js->s.cs, (char *) js->value, js->value_len,
                      &end, &err);;
      d_v= my_strntod(value->s.cs, (char *) value->value, value->value_len,
                      &end, &err);;

      return (fabs(d_j - d_v) < 1e-12);
    }
    else
      return FALSE;

  default:
    break;
  }

  /*
    We have these not mentioned in the 'switch' above:

    case JSON_VALUE_TRUE:
    case JSON_VALUE_FALSE:
    case JSON_VALUE_NULL:
  */
  return value->value_type == js->value_type;
}


longlong Item_func_json_contains::val_int()
{
  String *js= args[0]->val_json(&tmp_js);
  json_engine_t je, ve;
  int result;

  if ((null_value= args[0]->null_value))
    return 0;

  if (!a2_parsed)
  {
    val= args[1]->val_json(&tmp_val);
    a2_parsed= a2_constant;
  }

  if (val == 0)
  {
    null_value= 1;
    return 0;
  }

  json_scan_start(&je, js->charset(),(const uchar *) js->ptr(),
                  (const uchar *) js->ptr() + js->length());

  if (arg_count>2) /* Path specified. */
  {
    uint array_counters[JSON_DEPTH_LIMIT];
    if (!path.parsed)
    {
      String *s_p= args[2]->val_str(&tmp_path);
      if (s_p &&
          path_setup_nwc(&path.p,s_p->charset(),(const uchar *) s_p->ptr(),
                         (const uchar *) s_p->end()))
      {
        report_path_error(s_p, &path.p, 2);
        goto return_null;
      }
      path.parsed= path.constant;
    }
    if (args[2]->null_value)
      goto return_null;

    path.cur_step= path.p.steps;
    if (json_find_path(&je, &path.p, &path.cur_step, array_counters))
    {
      if (je.s.error)
      {
        ve.s.error= 0;
        goto error;
      }

      return FALSE;
    }
  }

  json_scan_start(&ve, val->charset(),(const uchar *) val->ptr(),
                  (const uchar *) val->end());

  if (json_read_value(&je) || json_read_value(&ve))
    goto error;

  result= check_contains(&je, &ve);
  if (unlikely(je.s.error || ve.s.error))
    goto error;

  return result;

error:
  if (je.s.error)
    report_json_error(js, &je, 0);
  if (ve.s.error)
    report_json_error(val, &ve, 1);
return_null:
  null_value= 1;
  return 0;
}


bool Item_func_json_contains_path::fix_fields(THD *thd, Item **ref)
{
  return alloc_tmp_paths(thd, arg_count-2, &paths, &tmp_paths) ||
         (p_found= (bool *) alloc_root(thd->mem_root,
                                       (arg_count-2)*sizeof(bool))) == NULL ||
         Item_int_func::fix_fields(thd, ref);
}


bool Item_func_json_contains_path::fix_length_and_dec()
{
  ooa_constant= args[1]->const_item();
  ooa_parsed= FALSE;
  maybe_null= 1;
  mark_constant_paths(paths, args+2, arg_count-2);
  return Item_bool_func::fix_length_and_dec();
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


static int parse_one_or_all(const Item_func *f, Item *ooa_arg,
                            bool *ooa_parsed, bool ooa_constant, bool *mode_one)
{
  if (!*ooa_parsed)
  {
    char buff[20];
    String *res, tmp(buff, sizeof(buff), &my_charset_bin);
    if ((res= ooa_arg->val_str(&tmp)) == NULL)
      return TRUE;

    *mode_one=eq_ascii_string(res->charset(), "one",
                             res->ptr(), res->length());
    if (!*mode_one)
    {
      if (!eq_ascii_string(res->charset(), "all", res->ptr(), res->length()))
      {
        THD *thd= current_thd;
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                            ER_JSON_ONE_OR_ALL, ER_THD(thd, ER_JSON_ONE_OR_ALL),
                            f->func_name());
        *mode_one= TRUE;
        return TRUE;
      }
    }
    *ooa_parsed= ooa_constant;
  }
  return FALSE;
}


#ifdef DUMMY
longlong Item_func_json_contains_path::val_int()
{
  String *js= args[0]->val_json(&tmp_js);
  json_engine_t je;
  uint n_arg;
  longlong result;

  if ((null_value= args[0]->null_value))
    return 0;

  if (parse_one_or_all(this, args[1], &ooa_parsed, ooa_constant, &mode_one))
    goto return_null;

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
      {
        report_path_error(s_p, &c_path->p, n_arg-2);
        goto return_null;
      }
      c_path->parsed= c_path->constant;
    }

    if (args[n_arg]->null_value)
      goto return_null;

    json_scan_start(&je, js->charset(),(const uchar *) js->ptr(),
                    (const uchar *) js->ptr() + js->length());

    c_path->cur_step= c_path->p.steps;
    if (json_find_path(&je, &c_path->p, &c_path->cur_step, array_counters))
    {
      /* Path wasn't found. */
      if (je.s.error)
        goto js_error;

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

js_error:
  report_json_error(js, &je, 0);
return_null:
  null_value= 1;
  return 0;
}
#endif /*DUMMY*/

longlong Item_func_json_contains_path::val_int()
{
  String *js= args[0]->val_json(&tmp_js);
  json_engine_t je;
  uint n_arg;
  longlong result;
  json_path_t p;
  int n_found;
  LINT_INIT(n_found);

  if ((null_value= args[0]->null_value))
    return 0;

  if (parse_one_or_all(this, args[1], &ooa_parsed, ooa_constant, &mode_one))
    goto null_return;;

  for (n_arg=2; n_arg < arg_count; n_arg++)
  {
    json_path_with_flags *c_path= paths + n_arg - 2;
    if (!c_path->parsed)
    {
      String *s_p= args[n_arg]->val_str(tmp_paths + (n_arg-2));
      if (s_p &&
          json_path_setup(&c_path->p,s_p->charset(),(const uchar *) s_p->ptr(),
                          (const uchar *) s_p->ptr() + s_p->length()))
      {
        report_path_error(s_p, &c_path->p, n_arg);
        goto null_return;
      }
      c_path->parsed= c_path->constant;
    }
    if (args[n_arg]->null_value)
      goto null_return;
  }

  json_get_path_start(&je, js->charset(),(const uchar *) js->ptr(),
                      (const uchar *) js->ptr() + js->length(), &p);


  if (!mode_one)
  {
    bzero(p_found, (arg_count-2) * sizeof(bool));
    n_found= arg_count - 2;
  }
  else
    n_found= 0; /* Jost to prevent 'uninitialized value' warnings */

  result= 0;
  while (json_get_path_next(&je, &p) == 0)
  {
    int n_path= arg_count - 2;
    json_path_with_flags *c_path= paths;
    for (; n_path > 0; n_path--, c_path++)
    {
      if (json_path_compare(&c_path->p, &p, je.value_type) >= 0)
      {
        if (mode_one)
        {
          result= 1;
          break;
        }
        /* mode_all */
        if (p_found[n_path-1])
          continue; /* already found */
        if (--n_found == 0)
        {
          result= 1;
          break;
        }
        p_found[n_path-1]= TRUE;
      }
    }
  }

  if (likely(je.s.error == 0))
    return result;

  report_json_error(js, &je, 0);
null_return:
  null_value= 1;
  return 0;
}


static int append_json_value(String *str, Item *item, String *tmp_val)
{
  if (item->type_handler()->is_bool_type())
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
    String *sv= item->val_json(tmp_val);
    if (item->null_value)
      goto append_null;
    if (item->is_json_type())
      return str->append(sv->ptr(), sv->length());

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


bool Item_func_json_array::fix_length_and_dec()
{
  ulonglong char_length= 2;
  uint n_arg;

  result_limit= 0;

  if (arg_count == 0)
  {
    THD* thd= current_thd;
    collation.set(thd->variables.collation_connection,
                  DERIVATION_COERCIBLE, MY_REPERTOIRE_ASCII);
    tmp_val.set_charset(thd->variables.collation_connection);
    max_length= 2;
    return FALSE;
  }

  if (agg_arg_charsets_for_string_result(collation, args, arg_count))
    return TRUE;

  for (n_arg=0 ; n_arg < arg_count ; n_arg++)
    char_length+= args[n_arg]->max_char_length() + 4;

  fix_char_length_ulonglong(char_length);
  tmp_val.set_charset(collation.collation);
  return FALSE;
}


String *Item_func_json_array::val_str(String *str)
{
  DBUG_ASSERT(fixed == 1);
  uint n_arg;

  str->length(0);
  str->set_charset(collation.collation);

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

  if (result_limit == 0)
    result_limit= current_thd->variables.max_allowed_packet;

  if (str->length() <= result_limit)
    return str;

  push_warning_printf(current_thd, Sql_condition::WARN_LEVEL_WARN,
      ER_WARN_ALLOWED_PACKET_OVERFLOWED,
      ER_THD(current_thd, ER_WARN_ALLOWED_PACKET_OVERFLOWED),
      func_name(), result_limit);

err_return:
  /*TODO: Launch out of memory error. */
  null_value= 1;
  return NULL;
}


bool Item_func_json_array_append::fix_length_and_dec()
{
  uint n_arg;
  ulonglong char_length;

  collation.set(args[0]->collation);
  char_length= args[0]->max_char_length();

  for (n_arg= 1; n_arg < arg_count; n_arg+= 2)
  {
    paths[n_arg/2].set_constant_flag(args[n_arg]->const_item());
    char_length+= args[n_arg/2+1]->max_char_length() + 4;
  }

  fix_char_length_ulonglong(char_length);
  maybe_null= 1;
  return FALSE;
}


String *Item_func_json_array_append::val_str(String *str)
{
  json_engine_t je;
  String *js= args[0]->val_json(&tmp_js);
  uint n_arg, n_path;
  size_t str_rest_len;
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
          path_setup_nwc(&c_path->p,s_p->charset(),(const uchar *) s_p->ptr(),
                         (const uchar *) s_p->ptr() + s_p->length()))
      {
        report_path_error(s_p, &c_path->p, n_arg);
        goto return_null;
      }
      c_path->parsed= c_path->constant;
    }
    if (args[n_arg]->null_value)
      goto return_null;

    json_scan_start(&je, js->charset(),(const uchar *) js->ptr(),
                    (const uchar *) js->ptr() + js->length());

    c_path->cur_step= c_path->p.steps;

    if (json_find_path(&je, &c_path->p, &c_path->cur_step, array_counters))
    {
      if (je.s.error)
        goto js_error;

      goto return_null;
    }

    if (json_read_value(&je))
      goto js_error;

    str->length(0);
    str->set_charset(js->charset());
    if (str->reserve(js->length() + 8, 1024))
      goto return_null; /* Out of memory. */

    if (je.value_type == JSON_VALUE_ARRAY)
    {
      int n_items;
      if (json_skip_level_and_count(&je, &n_items))
        goto js_error;

      ar_end= je.s.c_str - je.sav_c_len;
      str_rest_len= js->length() - (ar_end - (const uchar *) js->ptr());
      str->q_append(js->ptr(), ar_end-(const uchar *) js->ptr());
      if (n_items)
        str->append(", ", 2);
      if (append_json_value(str, args[n_arg+1], &tmp_val))
        goto return_null; /* Out of memory. */

      if (str->reserve(str_rest_len, 1024))
        goto return_null; /* Out of memory. */
      str->q_append((const char *) ar_end, str_rest_len);
    }
    else
    {
      const uchar *c_from, *c_to;

      /* Wrap as an array. */
      str->q_append(js->ptr(), (const char *) je.value_begin - js->ptr());
      c_from= je.value_begin;

      if (je.value_type == JSON_VALUE_OBJECT)
      {
        if (json_skip_level(&je))
          goto js_error;
        c_to= je.s.c_str;
      }
      else
        c_to= je.value_end;

      if (str->append("[", 1) ||
          str->append((const char *) c_from, c_to - c_from) ||
          str->append(", ", 2) ||
          append_json_value(str, args[n_arg+1], &tmp_val) ||
          str->append("]", 1) ||
          str->append((const char *) je.s.c_str,
                      js->end() - (const char *) je.s.c_str))
        goto return_null; /* Out of memory. */
    }
    {
      /* Swap str and js. */
      if (str == &tmp_js)
      {
        str= js;
        js= &tmp_js;
      }
      else
      {
        js= str;
        str= &tmp_js;
      }
    }
  }

  json_scan_start(&je, js->charset(),(const uchar *) js->ptr(),
                  (const uchar *) js->ptr() + js->length());
  str->length(0);
  str->set_charset(js->charset());
  if (json_nice(&je, str, Item_func_json_format::LOOSE))
    goto js_error;

  return str;

js_error:
  report_json_error(js, &je, 0);

return_null:
  null_value= 1;
  return 0;
}


String *Item_func_json_array_insert::val_str(String *str)
{
  json_engine_t je;
  String *js= args[0]->val_json(&tmp_js);
  uint n_arg, n_path;

  DBUG_ASSERT(fixed == 1);

  if ((null_value= args[0]->null_value))
    return 0;

  for (n_arg=1, n_path=0; n_arg < arg_count; n_arg+=2, n_path++)
  {
    uint array_counters[JSON_DEPTH_LIMIT];
    json_path_with_flags *c_path= paths + n_path;
    const char *item_pos;
    uint n_item;

    if (!c_path->parsed)
    {
      String *s_p= args[n_arg]->val_str(tmp_paths+n_path);
      if (s_p &&
          (path_setup_nwc(&c_path->p,s_p->charset(),(const uchar *) s_p->ptr(),
                          (const uchar *) s_p->ptr() + s_p->length()) ||
           c_path->p.last_step - 1 < c_path->p.steps ||
           c_path->p.last_step->type != JSON_PATH_ARRAY))
      {
        if (c_path->p.s.error == 0)
          c_path->p.s.error= SHOULD_END_WITH_ARRAY;
        
        report_path_error(s_p, &c_path->p, n_arg);

        goto return_null;
      }
      c_path->parsed= c_path->constant;
      c_path->p.last_step--;
    }
    if (args[n_arg]->null_value)
      goto return_null;

    json_scan_start(&je, js->charset(),(const uchar *) js->ptr(),
                    (const uchar *) js->ptr() + js->length());

    c_path->cur_step= c_path->p.steps;

    if (json_find_path(&je, &c_path->p, &c_path->cur_step, array_counters))
    {
      if (je.s.error)
        goto js_error;

      /* Can't find the array to insert. */
      continue;
    }

    if (json_read_value(&je))
      goto js_error;

    if (je.value_type != JSON_VALUE_ARRAY)
    {
      /* Must be an array. */
      continue;
    }

    item_pos= 0;
    n_item= 0;

    while (json_scan_next(&je) == 0 && je.state != JST_ARRAY_END)
    {
      DBUG_ASSERT(je.state == JST_VALUE);
      if (n_item == c_path->p.last_step[1].n_item)
      {
        item_pos= (const char *) je.s.c_str;
        break;
      }
      n_item++;

      if (json_read_value(&je) ||
          (!json_value_scalar(&je) && json_skip_level(&je)))
        goto js_error;
    }

    if (unlikely(je.s.error))
      goto js_error;

    str->length(0);
    str->set_charset(js->charset());
    if (item_pos)
    {
      if (append_simple(str, js->ptr(), item_pos - js->ptr()) ||
          (n_item > 0  && str->append(" ", 1)) ||
          append_json_value(str, args[n_arg+1], &tmp_val) ||
          str->append(",", 1) ||
          (n_item == 0  && str->append(" ", 1)) ||
          append_simple(str, item_pos, js->end() - item_pos))
        goto return_null; /* Out of memory. */
    }
    else
    {
      /* Insert position wasn't found - append to the array. */
      DBUG_ASSERT(je.state == JST_ARRAY_END);
      item_pos= (const char *) (je.s.c_str - je.sav_c_len);
      if (append_simple(str, js->ptr(), item_pos - js->ptr()) ||
          (n_item > 0  && str->append(", ", 2)) ||
          append_json_value(str, args[n_arg+1], &tmp_val) ||
          append_simple(str, item_pos, js->end() - item_pos))
        goto return_null; /* Out of memory. */
    }

    {
      /* Swap str and js. */
      if (str == &tmp_js)
      {
        str= js;
        js= &tmp_js;
      }
      else
      {
        js= str;
        str= &tmp_js;
      }
    }
  }

  json_scan_start(&je, js->charset(),(const uchar *) js->ptr(),
                  (const uchar *) js->ptr() + js->length());
  str->length(0);
  str->set_charset(js->charset());
  if (json_nice(&je, str, Item_func_json_format::LOOSE))
    goto js_error;

  return str;

js_error:
  report_json_error(js, &je, 0);
return_null:
  null_value= 1;
  return 0;
}


String *Item_func_json_object::val_str(String *str)
{
  DBUG_ASSERT(fixed == 1);
  uint n_arg;

  str->length(0);
  str->set_charset(collation.collation);

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

  if (result_limit == 0)
    result_limit= current_thd->variables.max_allowed_packet;

  if (str->length() <= result_limit)
    return str;

  push_warning_printf(current_thd, Sql_condition::WARN_LEVEL_WARN,
      ER_WARN_ALLOWED_PACKET_OVERFLOWED,
      ER_THD(current_thd, ER_WARN_ALLOWED_PACKET_OVERFLOWED),
      func_name(), result_limit);

err_return:
  /*TODO: Launch out of memory error. */
  null_value= 1;
  return NULL;
}


static int do_merge(String *str, json_engine_t *je1, json_engine_t *je2)
{
  if (json_read_value(je1) || json_read_value(je2))
    return 1;

  if (je1->value_type == JSON_VALUE_OBJECT &&
      je2->value_type == JSON_VALUE_OBJECT)
  {
    json_engine_t sav_je1= *je1;
    json_engine_t sav_je2= *je2;

    int first_key= 1;
    json_string_t key_name;
  
    json_string_set_cs(&key_name, je1->s.cs);

    if (str->append("{", 1))
      return 3;
    while (json_scan_next(je1) == 0 &&
           je1->state != JST_OBJ_END)
    {
      const uchar *key_start, *key_end;
      /* Loop through the Json_1 keys and compare with the Json_2 keys. */
      DBUG_ASSERT(je1->state == JST_KEY);
      key_start= je1->s.c_str;
      do
      {
        key_end= je1->s.c_str;
      } while (json_read_keyname_chr(je1) == 0);

      if (unlikely(je1->s.error))
        return 1;

      if (first_key)
        first_key= 0;
      else
      {
        if (str->append(", ", 2))
          return 3;
        *je2= sav_je2;
      }

      if (str->append("\"", 1) ||
          append_simple(str, key_start, key_end - key_start) ||
          str->append("\":", 2))
        return 3;

      while (json_scan_next(je2) == 0 &&
          je2->state != JST_OBJ_END)
      {
        int ires;
        DBUG_ASSERT(je2->state == JST_KEY);
        json_string_set_str(&key_name, key_start, key_end);
        if (!json_key_matches(je2, &key_name))
        {
          if (je2->s.error || json_skip_key(je2))
            return 2;
          continue;
        }

        /* Json_2 has same key as Json_1. Merge them. */
        if ((ires= do_merge(str, je1, je2)))
          return ires;
        goto merged_j1;
      }
      if (unlikely(je2->s.error))
        return 2;

      key_start= je1->s.c_str;
      /* Just append the Json_1 key value. */
      if (json_skip_key(je1))
        return 1;
      if (append_simple(str, key_start, je1->s.c_str - key_start))
        return 3;

merged_j1:
      continue;
    }

    *je2= sav_je2;
    /*
      Now loop through the Json_2 keys.
      Skip if there is same key in Json_1
    */
    while (json_scan_next(je2) == 0 &&
           je2->state != JST_OBJ_END)
    {
      const uchar *key_start, *key_end;
      DBUG_ASSERT(je2->state == JST_KEY);
      key_start= je2->s.c_str;
      do
      {
        key_end= je2->s.c_str;
      } while (json_read_keyname_chr(je2) == 0);

      if (unlikely(je2->s.error))
        return 1;

      *je1= sav_je1;
      while (json_scan_next(je1) == 0 &&
             je1->state != JST_OBJ_END)
      {
        DBUG_ASSERT(je1->state == JST_KEY);
        json_string_set_str(&key_name, key_start, key_end);
        if (!json_key_matches(je1, &key_name))
        {
          if (unlikely(je1->s.error || json_skip_key(je1)))
            return 2;
          continue;
        }
        if (json_skip_key(je2) ||
            json_skip_level(je1))
          return 1;
        goto continue_j2;
      }

      if (unlikely(je1->s.error))
        return 2;

      if (first_key)
        first_key= 0;
      else if (str->append(", ", 2))
        return 3;

      if (json_skip_key(je2))
        return 1;

      if (str->append("\"", 1) ||
          append_simple(str, key_start, je2->s.c_str - key_start))
        return 3;

continue_j2:
      continue;
    }

    if (str->append("}", 1))
      return 3;
  }
  else
  {
    const uchar *end1, *beg1, *end2, *beg2;
    int n_items1=1, n_items2= 1;

    beg1= je1->value_begin;

    /* Merge as a single array. */
    if (je1->value_type == JSON_VALUE_ARRAY)
    {
      if (json_skip_level_and_count(je1, &n_items1))
        return 1;

      end1= je1->s.c_str - je1->sav_c_len;
    }
    else
    {
      if (str->append("[", 1))
        return 3;
      if (je1->value_type == JSON_VALUE_OBJECT)
      {
        if (json_skip_level(je1))
          return 1;
        end1= je1->s.c_str;
      }
      else
        end1= je1->value_end;
    }

    if (str->append((const char*) beg1, end1 - beg1))
      return 3;

    if (json_value_scalar(je2))
    {
      beg2= je2->value_begin;
      end2= je2->value_end;
    }
    else
    {
      if (je2->value_type == JSON_VALUE_OBJECT)
      {
        beg2= je2->value_begin;
        if (json_skip_level(je2))
          return 2;
      }
      else
      {
        beg2= je2->s.c_str;
        if (json_skip_level_and_count(je2, &n_items2))
          return 2;
      }
      end2= je2->s.c_str;
    }

    if ((n_items1 && n_items2 && str->append(", ", 2)) ||
        str->append((const char*) beg2, end2 - beg2))
      return 3;

    if (je2->value_type != JSON_VALUE_ARRAY &&
        str->append("]", 1))
      return 3;
  }

  return 0;
}


String *Item_func_json_merge::val_str(String *str)
{
  DBUG_ASSERT(fixed == 1);
  json_engine_t je1, je2;
  String *js1= args[0]->val_json(&tmp_js1), *js2=NULL;
  uint n_arg;
  LINT_INIT(js2);

  if (args[0]->null_value)
    goto null_return;

  for (n_arg=1; n_arg < arg_count; n_arg++)
  {
    str->set_charset(js1->charset());
    str->length(0);

    js2= args[n_arg]->val_json(&tmp_js2);
    if (args[n_arg]->null_value)
      goto null_return;

    json_scan_start(&je1, js1->charset(),(const uchar *) js1->ptr(),
                    (const uchar *) js1->ptr() + js1->length());

    json_scan_start(&je2, js2->charset(),(const uchar *) js2->ptr(),
                    (const uchar *) js2->ptr() + js2->length());

    if (do_merge(str, &je1, &je2))
      goto error_return;

    {
      /* Swap str and js1. */
      if (str == &tmp_js1)
      {
        str= js1;
        js1= &tmp_js1;
      }
      else
      {
        js1= str;
        str= &tmp_js1;
      }
    }
  }

  json_scan_start(&je1, js1->charset(),(const uchar *) js1->ptr(),
                  (const uchar *) js1->ptr() + js1->length());
  str->length(0);
  str->set_charset(js1->charset());
  if (json_nice(&je1, str, Item_func_json_format::LOOSE))
    goto error_return;

  null_value= 0;
  return str;

error_return:
  if (je1.s.error)
    report_json_error(js1, &je1, 0);
  if (je2.s.error)
    report_json_error(js2, &je2, n_arg);
null_return:
  null_value= 1;
  return NULL;
}


bool Item_func_json_length::fix_length_and_dec()
{
  if (arg_count > 1)
    path.set_constant_flag(args[1]->const_item());
  maybe_null= 1;
  max_length= 10;
  return FALSE;
}


longlong Item_func_json_length::val_int()
{
  String *js= args[0]->val_json(&tmp_js);
  json_engine_t je;
  uint length= 0;
  uint array_counters[JSON_DEPTH_LIMIT];
  int err;

  if ((null_value= args[0]->null_value))
    return 0;

  json_scan_start(&je, js->charset(),(const uchar *) js->ptr(),
                  (const uchar *) js->ptr() + js->length());

  if (arg_count > 1)
  {
    /* Path specified - let's apply it. */
    if (!path.parsed)
    {
      String *s_p= args[1]->val_str(&tmp_path);
      if (s_p &&
          path_setup_nwc(&path.p, s_p->charset(), (const uchar *) s_p->ptr(),
                         (const uchar *) s_p->ptr() + s_p->length()))
      {
        report_path_error(s_p, &path.p, 1);
        goto null_return;
      }
      path.parsed= path.constant;
    }
    if (args[1]->null_value)
      goto null_return;

    path.cur_step= path.p.steps;
    if (json_find_path(&je, &path.p, &path.cur_step, array_counters))
    {
      if (je.s.error)
        goto err_return;
      goto null_return;
    }
  }
  

  if (json_read_value(&je))
    goto err_return;

  if (json_value_scalar(&je))
    return 1;

  while (!(err= json_scan_next(&je)) &&
         je.state != JST_OBJ_END && je.state != JST_ARRAY_END)
  {
    switch (je.state)
    {
    case JST_VALUE:
    case JST_KEY:
      length++;
      break;
    case JST_OBJ_START:
    case JST_ARRAY_START:
      if (json_skip_level(&je))
        goto err_return;
      break;
    default:
      break;
    };
  }

  if (!err)
  {
    /* Parse to the end of the JSON just to check it's valid. */
    while (json_scan_next(&je) == 0) {}
  }

  if (likely(!je.s.error))
    return length;

err_return:
  report_json_error(js, &je, 0);
null_return:
  null_value= 1;
  return 0;
}


longlong Item_func_json_depth::val_int()
{
  String *js= args[0]->val_json(&tmp_js);
  json_engine_t je;
  uint depth= 0, c_depth= 0;
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
    case JST_KEY:
      if (inc_depth)
      {
        c_depth++;
        inc_depth= FALSE;
        if (c_depth > depth)
          depth= c_depth;
      }
      break;
    case JST_OBJ_START:
    case JST_ARRAY_START:
      inc_depth= TRUE;
      break;
    case JST_OBJ_END:
    case JST_ARRAY_END:
      if (!inc_depth)
        c_depth--;
      inc_depth= FALSE;
      break;
    default:
      break;
    }
  } while (json_scan_next(&je) == 0);

  if (likely(!je.s.error))
    return depth;

  report_json_error(js, &je, 0);
  null_value= 1;
  return 0;
}


bool Item_func_json_type::fix_length_and_dec()
{
  collation.set(&my_charset_utf8_general_ci);
  max_length= 12;
  maybe_null= 1;
  return FALSE;
}


String *Item_func_json_type::val_str(String *str)
{
  String *js= args[0]->val_json(&tmp_js);
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
    type= (je.num_flags & JSON_NUM_FRAC_PART) ?  "DOUBLE" : "INTEGER";
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
  report_json_error(js, &je, 0);
  null_value= 1;
  return 0;
}


bool Item_func_json_insert::fix_length_and_dec()
{
  uint n_arg;
  ulonglong char_length;

  collation.set(args[0]->collation);
  char_length= args[0]->max_char_length();

  for (n_arg= 1; n_arg < arg_count; n_arg+= 2)
  {
    paths[n_arg/2].set_constant_flag(args[n_arg]->const_item());
    char_length+= args[n_arg/2+1]->max_char_length() + 4;
  }

  fix_char_length_ulonglong(char_length);
  maybe_null= 1;
  return FALSE;
}


String *Item_func_json_insert::val_str(String *str)
{
  json_engine_t je;
  String *js= args[0]->val_json(&tmp_js);
  uint n_arg, n_path;
  json_string_t key_name;

  DBUG_ASSERT(fixed == 1);

  if ((null_value= args[0]->null_value))
    return 0;

  str->set_charset(collation.collation);
  tmp_js.set_charset(collation.collation);
  json_string_set_cs(&key_name, collation.collation);

  for (n_arg=1, n_path=0; n_arg < arg_count; n_arg+=2, n_path++)
  {
    uint array_counters[JSON_DEPTH_LIMIT];
    json_path_with_flags *c_path= paths + n_path;
    const char *v_to;
    const json_path_step_t *lp;

    if (!c_path->parsed)
    {
      String *s_p= args[n_arg]->val_str(tmp_paths+n_path);
      if (s_p)
      {
        if (path_setup_nwc(&c_path->p,s_p->charset(),
                           (const uchar *) s_p->ptr(),
                           (const uchar *) s_p->ptr() + s_p->length()))
        {
          report_path_error(s_p, &c_path->p, n_arg);
          goto return_null;
        }

        /* We search to the last step. */
        c_path->p.last_step--;
      }
      c_path->parsed= c_path->constant;
    }
    if (args[n_arg]->null_value)
      goto return_null;

    json_scan_start(&je, js->charset(),(const uchar *) js->ptr(),
                    (const uchar *) js->ptr() + js->length());

    if (c_path->p.last_step < c_path->p.steps)
      goto v_found;

    c_path->cur_step= c_path->p.steps;

    if (c_path->p.last_step >= c_path->p.steps &&
        json_find_path(&je, &c_path->p, &c_path->cur_step, array_counters))
    {
      if (je.s.error)
        goto js_error;
      continue;
    }

    if (json_read_value(&je))
      goto js_error;

    lp= c_path->p.last_step+1;
    if (lp->type & JSON_PATH_ARRAY)
    {
      uint n_item= 0;

      if (je.value_type != JSON_VALUE_ARRAY)
      {
        const uchar *v_from= je.value_begin;
        int do_array_autowrap;

        if (mode_insert)
        {
          if (mode_replace)
            do_array_autowrap= lp->n_item > 0;
          else
          {
            if (lp->n_item == 0)
              continue;
            do_array_autowrap= 1;
          }
        }
        else
        {
          if (lp->n_item)
            continue;
          do_array_autowrap= 0;
        }


        str->length(0);
        /* Wrap the value as an array. */
        if (append_simple(str, js->ptr(), (const char *) v_from - js->ptr()) ||
            (do_array_autowrap && str->append("[", 1)))
          goto js_error; /* Out of memory. */

        if (je.value_type == JSON_VALUE_OBJECT)
        {
          if (json_skip_level(&je))
            goto js_error;
        }

        if ((do_array_autowrap &&
             (append_simple(str, v_from, je.s.c_str - v_from) ||
              str->append(", ", 2))) ||
            append_json_value(str, args[n_arg+1], &tmp_val) ||
            (do_array_autowrap && str->append("]", 1)) ||
            append_simple(str, je.s.c_str, js->end()-(const char *) je.s.c_str))
          goto js_error; /* Out of memory. */

        goto continue_point;
      }

      while (json_scan_next(&je) == 0 && je.state != JST_ARRAY_END)
      {
        switch (je.state)
        {
        case JST_VALUE:
          if (n_item == lp->n_item)
            goto v_found;
          n_item++;
          if (json_skip_array_item(&je))
            goto js_error;
          break;
        default:
          break;
        }
      }

      if (unlikely(je.s.error))
        goto js_error;

      if (!mode_insert)
        continue;

      v_to= (const char *) (je.s.c_str - je.sav_c_len);
      str->length(0);
      if (append_simple(str, js->ptr(), v_to - js->ptr()) ||
          (n_item > 0 && str->append(", ", 2)) ||
          append_json_value(str, args[n_arg+1], &tmp_val) ||
          append_simple(str, v_to, js->end() - v_to))
        goto js_error; /* Out of memory. */
    }
    else /*JSON_PATH_KEY*/
    {
      uint n_key= 0;

      if (je.value_type != JSON_VALUE_OBJECT)
        continue;

      while (json_scan_next(&je) == 0 && je.state != JST_OBJ_END)
      {
        switch (je.state)
        {
        case JST_KEY:
          json_string_set_str(&key_name, lp->key, lp->key_end);
          if (json_key_matches(&je, &key_name))
            goto v_found;
          n_key++;
          if (json_skip_key(&je))
            goto js_error;
          break;
        default:
          break;
        }
      }

      if (unlikely(je.s.error))
        goto js_error;

      if (!mode_insert)
        continue;

      v_to= (const char *) (je.s.c_str - je.sav_c_len);
      str->length(0);
      if (append_simple(str, js->ptr(), v_to - js->ptr()) ||
          (n_key > 0 && str->append(", ", 2)) ||
          str->append("\"", 1) ||
          append_simple(str, lp->key, lp->key_end - lp->key) ||
          str->append("\":", 2) ||
          append_json_value(str, args[n_arg+1], &tmp_val) ||
          append_simple(str, v_to, js->end() - v_to))
        goto js_error; /* Out of memory. */
    }

    goto continue_point;

v_found:

    if (!mode_replace)
      continue;

    if (json_read_value(&je))
      goto js_error;

    v_to= (const char *) je.value_begin;
    str->length(0);
    if (!json_value_scalar(&je))
    {
      if (json_skip_level(&je))
        goto js_error;
    }

    if (append_simple(str, js->ptr(), v_to - js->ptr()) ||
        append_json_value(str, args[n_arg+1], &tmp_val) ||
        append_simple(str, je.s.c_str, js->end()-(const char *) je.s.c_str))
      goto js_error; /* Out of memory. */
continue_point:
    {
      /* Swap str and js. */
      if (str == &tmp_js)
      {
        str= js;
        js= &tmp_js;
      }
      else
      {
        js= str;
        str= &tmp_js;
      }
    }
  }

  json_scan_start(&je, js->charset(),(const uchar *) js->ptr(),
                  (const uchar *) js->ptr() + js->length());
  str->length(0);
  if (json_nice(&je, str, Item_func_json_format::LOOSE))
    goto js_error;

  return str;

js_error:
  report_json_error(js, &je, 0);
return_null:
  null_value= 1;
  return 0;
}


bool Item_func_json_remove::fix_length_and_dec()
{
  collation.set(args[0]->collation);
  max_length= args[0]->max_length;

  mark_constant_paths(paths, args+1, arg_count-1);
  maybe_null= 1;
  return FALSE;
}


String *Item_func_json_remove::val_str(String *str)
{
  json_engine_t je;
  String *js= args[0]->val_json(&tmp_js);
  uint n_arg, n_path;
  json_string_t key_name;

  DBUG_ASSERT(fixed == 1);

  if (args[0]->null_value)
    goto null_return;

  str->set_charset(js->charset());
  json_string_set_cs(&key_name, js->charset());

  for (n_arg=1, n_path=0; n_arg < arg_count; n_arg++, n_path++)
  {
    uint array_counters[JSON_DEPTH_LIMIT];
    json_path_with_flags *c_path= paths + n_path;
    const char *rem_start= 0, *rem_end;
    const json_path_step_t *lp;
    uint n_item= 0;

    if (!c_path->parsed)
    {
      String *s_p= args[n_arg]->val_str(tmp_paths+n_path);
      if (s_p)
      {
        if (path_setup_nwc(&c_path->p,s_p->charset(),
                           (const uchar *) s_p->ptr(),
                           (const uchar *) s_p->ptr() + s_p->length()))
        {
          report_path_error(s_p, &c_path->p, n_arg);
          goto null_return;
        }

        /* We search to the last step. */
        c_path->p.last_step--;
        if (c_path->p.last_step < c_path->p.steps)
        {
          c_path->p.s.error= TRIVIAL_PATH_NOT_ALLOWED;
          report_path_error(s_p, &c_path->p, n_arg);
          goto null_return;
        }
      }
      c_path->parsed= c_path->constant;
    }
    if (args[n_arg]->null_value)
      goto null_return;

    json_scan_start(&je, js->charset(),(const uchar *) js->ptr(),
                    (const uchar *) js->ptr() + js->length());

    c_path->cur_step= c_path->p.steps;

    if (json_find_path(&je, &c_path->p, &c_path->cur_step, array_counters))
    {
      if (je.s.error)
        goto js_error;
    }

    if (json_read_value(&je))
      goto js_error;

    lp= c_path->p.last_step+1;
    if (lp->type & JSON_PATH_ARRAY)
    {
      if (je.value_type != JSON_VALUE_ARRAY)
        continue;

      while (json_scan_next(&je) == 0 && je.state != JST_ARRAY_END)
      {
        switch (je.state)
        {
        case JST_VALUE:
          if (n_item == lp->n_item)
          {
            rem_start= (const char *) (je.s.c_str -
                         (n_item ? je.sav_c_len : 0));
            goto v_found;
          }
          n_item++;
          if (json_skip_array_item(&je))
            goto js_error;
          break;
        default:
          break;
        }
      }

      if (unlikely(je.s.error))
        goto js_error;

      continue;
    }
    else /*JSON_PATH_KEY*/
    {
      if (je.value_type != JSON_VALUE_OBJECT)
        continue;

      while (json_scan_next(&je) == 0 && je.state != JST_OBJ_END)
      {
        switch (je.state)
        {
        case JST_KEY:
          if (n_item == 0)
            rem_start= (const char *) (je.s.c_str - je.sav_c_len);
          json_string_set_str(&key_name, lp->key, lp->key_end);
          if (json_key_matches(&je, &key_name))
            goto v_found;

          if (json_skip_key(&je))
            goto js_error;

          rem_start= (const char *) je.s.c_str;
          n_item++;
          break;
        default:
          break;
        }
      }

      if (unlikely(je.s.error))
        goto js_error;

      continue;
    }

v_found:

    if (json_skip_key(&je) || json_scan_next(&je))
      goto js_error;

    rem_end= (je.state == JST_VALUE && n_item == 0) ? 
      (const char *) je.s.c_str : (const char *) (je.s.c_str - je.sav_c_len);

    str->length(0);

    if (append_simple(str, js->ptr(), rem_start - js->ptr()) ||
        (je.state == JST_KEY && n_item > 0 && str->append(",", 1)) ||
        append_simple(str, rem_end, js->end() - rem_end))
          goto js_error; /* Out of memory. */

    {
      /* Swap str and js. */
      if (str == &tmp_js)
      {
        str= js;
        js= &tmp_js;
      }
      else
      {
        js= str;
        str= &tmp_js;
      }
    }
  }

  json_scan_start(&je, js->charset(),(const uchar *) js->ptr(),
                  (const uchar *) js->ptr() + js->length());
  str->length(0);
  str->set_charset(js->charset());
  if (json_nice(&je, str, Item_func_json_format::LOOSE))
    goto js_error;

  null_value= 0;
  return str;

js_error:
  report_json_error(js, &je, 0);
null_return:
  null_value= 1;
  return 0;
}


bool Item_func_json_keys::fix_length_and_dec()
{
  collation.set(args[0]->collation);
  max_length= args[0]->max_length;
  maybe_null= 1;
  if (arg_count > 1)
    path.set_constant_flag(args[1]->const_item());
  return FALSE;
}


/*
  That function is for Item_func_json_keys::val_str exclusively.
  It utilizes the fact the resulting string is in specific format:
        ["key1", "key2"...]
*/
static int check_key_in_list(String *res,
                             const uchar *key, int key_len)
{
  const uchar *c= (const uchar *) res->ptr() + 2; /* beginning '["' */
  const uchar *end= (const uchar *) res->end() - 1; /* ending '"' */

  while (c < end)
  {
    int n_char;
    for (n_char=0; c[n_char] != '"' && n_char < key_len; n_char++)
    {
      if (c[n_char] != key[n_char])
        break;
    }
    if (c[n_char] == '"')
    {
      if (n_char == key_len)
        return 1;
    }
    else
    {
      while (c[n_char] != '"')
        n_char++;
    }
    c+= n_char + 4; /* skip ', "' */
  }
  return 0;
}


String *Item_func_json_keys::val_str(String *str)
{
  json_engine_t je;
  String *js= args[0]->val_json(&tmp_js);
  uint n_keys= 0;
  uint array_counters[JSON_DEPTH_LIMIT];

  if ((args[0]->null_value))
    goto null_return;

  json_scan_start(&je, js->charset(),(const uchar *) js->ptr(),
                  (const uchar *) js->ptr() + js->length());

  if (arg_count < 2)
    goto skip_search;

  if (!path.parsed)
  {
    String *s_p= args[1]->val_str(&tmp_path);
    if (s_p &&
        path_setup_nwc(&path.p, s_p->charset(), (const uchar *) s_p->ptr(),
                       (const uchar *) s_p->ptr() + s_p->length()))
      {
        report_path_error(s_p, &path.p, 1);
        goto null_return;
      }
    path.parsed= path.constant;
  }

  if (args[1]->null_value)
    goto null_return;

  path.cur_step= path.p.steps;

  if (json_find_path(&je, &path.p, &path.cur_step, array_counters))
  {
    if (je.s.error)
      goto err_return;

    goto null_return;
  }

skip_search:
  if (json_read_value(&je))
    goto err_return;

  if (je.value_type != JSON_VALUE_OBJECT)
    goto null_return;
  
  str->length(0);
  if (str->append("[", 1))
    goto err_return; /* Out of memory. */
  /* Parse the OBJECT collecting the keys. */
  while (json_scan_next(&je) == 0 && je.state != JST_OBJ_END)
  {
    const uchar *key_start, *key_end;
    int key_len;

    switch (je.state)
    {
    case JST_KEY:
      key_start= je.s.c_str;
      do
      {
        key_end= je.s.c_str;
      } while (json_read_keyname_chr(&je) == 0);
      if (unlikely(je.s.error))
        goto err_return;
      key_len= (int)(key_end - key_start);

      if (!check_key_in_list(str, key_start, key_len))
      { 
        if ((n_keys > 0 && str->append(", ", 2)) ||
          str->append("\"", 1) ||
          append_simple(str, key_start, key_len) ||
          str->append("\"", 1))
        goto err_return;
        n_keys++;
      }
      break;
    case JST_OBJ_START:
    case JST_ARRAY_START:
      if (json_skip_level(&je))
        break;
      break;
    default:
      break;
    }
  }

  if (unlikely(je.s.error || str->append("]", 1)))
    goto err_return;

  null_value= 0;
  return str;

err_return:
  report_json_error(js, &je, 0);
null_return:
  null_value= 1;
  return 0;
}


bool Item_func_json_search::fix_fields(THD *thd, Item **ref)
{
  if (Item_json_str_multipath::fix_fields(thd, ref))
    return TRUE;

  if (arg_count < 4)
  {
    escape= '\\';
    return FALSE;
  }

  return fix_escape_item(thd, args[3], &tmp_js, true,
                         args[0]->collation.collation, &escape);
}


static const uint SQR_MAX_BLOB_WIDTH= (uint) sqrt(MAX_BLOB_WIDTH);

bool Item_func_json_search::fix_length_and_dec()
{
  collation.set(args[0]->collation);

  /*
    It's rather difficult to estimate the length of the result.
    I belive arglen^2 is the reasonable upper limit.
  */
  if (args[0]->max_length > SQR_MAX_BLOB_WIDTH)
    max_length= MAX_BLOB_WIDTH;
  else
  {
    max_length= args[0]->max_length;
    max_length*= max_length;
  }

  ooa_constant= args[1]->const_item();
  ooa_parsed= FALSE;

  if (arg_count > 4)
    mark_constant_paths(paths, args+4, arg_count-4);
  maybe_null= 1;
  return FALSE;
}


int Item_func_json_search::compare_json_value_wild(json_engine_t *je,
                                                   const String *cmp_str)
{
  if (je->value_type != JSON_VALUE_STRING || !je->value_escaped)
    return my_wildcmp(collation.collation,
        (const char *) je->value, (const char *) (je->value + je->value_len),
        cmp_str->ptr(), cmp_str->end(), escape, wild_one, wild_many) ? 0 : 1;

  {
    int esc_len;
    if (esc_value.alloced_length() < (uint) je->value_len &&
        esc_value.alloc((je->value_len / 1024 + 1) * 1024))
      return 0;

    esc_len= json_unescape(je->s.cs, je->value, je->value + je->value_len,
                           je->s.cs, (uchar *) esc_value.ptr(),
                           (uchar *) (esc_value.ptr() + 
                                      esc_value.alloced_length()));
    if (esc_len <= 0)
      return 0;

    return my_wildcmp(collation.collation,
        esc_value.ptr(), esc_value.ptr() + esc_len,
        cmp_str->ptr(), cmp_str->end(), escape, wild_one, wild_many) ? 0 : 1;
  }
}


static int append_json_path(String *str, const json_path_t *p)
{
  const json_path_step_t *c;

  if (str->append("\"$", 2))
    return TRUE;

  for (c= p->steps+1; c <= p->last_step; c++)
  {
    if (c->type & JSON_PATH_KEY)
    {
      if (str->append(".", 1) ||
          append_simple(str, c->key, c->key_end-c->key))
        return TRUE;
    }
    else /*JSON_PATH_ARRAY*/
    {

      if (str->append("[", 1) ||
          str->append_ulonglong(c->n_item) ||
          str->append("]", 1))
        return TRUE;
    }
  }

  return str->append("\"", 1);
}


String *Item_func_json_search::val_str(String *str)
{
  String *js= args[0]->val_json(&tmp_js);
  String *s_str= args[2]->val_str(&tmp_path);
  json_engine_t je;
  json_path_t p, sav_path;
  uint n_arg;

  if (args[0]->null_value || args[2]->null_value)
    goto null_return;

  if (parse_one_or_all(this, args[1], &ooa_parsed, ooa_constant, &mode_one))
    goto null_return;

  n_path_found= 0;
  str->set_charset(js->charset());
  str->length(0);

  for (n_arg=4; n_arg < arg_count; n_arg++)
  {
    json_path_with_flags *c_path= paths + n_arg - 4;
    if (!c_path->parsed)
    {
      String *s_p= args[n_arg]->val_str(tmp_paths + (n_arg-4));
      if (s_p &&
          json_path_setup(&c_path->p,s_p->charset(),(const uchar *) s_p->ptr(),
                          (const uchar *) s_p->ptr() + s_p->length()))
      {
        report_path_error(s_p, &c_path->p, n_arg);
        goto null_return;
      }
      c_path->parsed= c_path->constant;
    }
    if (args[n_arg]->null_value)
      goto null_return;
  }

  json_get_path_start(&je, js->charset(),(const uchar *) js->ptr(),
                      (const uchar *) js->ptr() + js->length(), &p);

  while (json_get_path_next(&je, &p) == 0)
  {
    if (json_value_scalar(&je))
    {
      if ((arg_count < 5 || path_ok(paths, arg_count - 4, &p, je.value_type)) &&
          compare_json_value_wild(&je, s_str) != 0)
      {
        ++n_path_found;
        if (n_path_found == 1)
        {
          sav_path= p;
          sav_path.last_step= sav_path.steps + (p.last_step - p.steps);
        }
        else
        {
          if (n_path_found == 2)
          {
            if (str->append("[", 1) ||
                append_json_path(str, &sav_path))
                goto js_error;
          }
          if (str->append(", ", 2) || append_json_path(str, &p))
            goto js_error;
        }
        if (mode_one)
          goto end;
      }
    }
  }

  if (unlikely(je.s.error))
    goto js_error;

end:
  if (n_path_found == 0)
    goto null_return;
  if (n_path_found == 1)
  {
    if (append_json_path(str, &sav_path))
      goto js_error;
  }
  else
  {
    if (str->append("]", 1))
      goto js_error;
  }

  null_value= 0;
  return str;


js_error:
  report_json_error(js, &je, 0);
null_return:
  null_value= 1;
  return 0;
}


const char *Item_func_json_format::func_name() const
{
  switch (fmt)
  {
  case COMPACT:
    return "json_compact";
  case LOOSE:
    return "json_loose";
  case DETAILED:
    return "json_detailed";
  default:
    DBUG_ASSERT(0);
  };

  return "";
}


bool Item_func_json_format::fix_length_and_dec()
{
  decimals= 0;
  max_length= args[0]->max_length;
  maybe_null= 1;
  return FALSE;
}


String *Item_func_json_format::val_str(String *str)
{
  String *js= args[0]->val_json(&tmp_js);
  json_engine_t je;
  int tab_size= 4;

  if ((null_value= args[0]->null_value))
    return 0;

  if (fmt == DETAILED)
  {
    if (arg_count > 1)
    {
      tab_size= (int)args[1]->val_int();
      if (args[1]->null_value)
      {
        null_value= 1;
        return 0;
      }
    }
    if (tab_size < 0)
      tab_size= 0;
    else if (tab_size > TAB_SIZE_LIMIT)
      tab_size= TAB_SIZE_LIMIT;
  }

  json_scan_start(&je, js->charset(), (const uchar *) js->ptr(),
                  (const uchar *) js->ptr()+js->length());

  str->length(0);
  str->set_charset(js->charset());
  if (json_nice(&je, str, fmt, tab_size))
  {
    null_value= 1;
    report_json_error(js, &je, 0);
    return 0;
  }

  return str;
}


String *Item_func_json_format::val_json(String *str)
{
  String *js= args[0]->val_json(&tmp_js);
  if ((null_value= args[0]->null_value))
    return 0;
  return js;
}

int Arg_comparator::compare_json_str_basic(Item *j, Item *s)
{
  String *js,*str;
  int c_len;
  json_engine_t je;

  if ((js= j->val_str(&value1)))
  {
    json_scan_start(&je, js->charset(), (const uchar *) js->ptr(),
                    (const uchar *) js->ptr()+js->length());
     if (json_read_value(&je))
       goto error;
     if (je.value_type == JSON_VALUE_STRING)
     {
       if (value2.realloc_with_extra_if_needed(je.value_len) ||
         (c_len= json_unescape(js->charset(), je.value,
                               je.value + je.value_len,
                               &my_charset_utf8_general_ci,
                               (uchar *) value2.ptr(),
                               (uchar *) (value2.ptr() + je.value_len))) < 0)
         goto error;

       value2.length(c_len);
       js= &value2;
       str= &value1;
     }
     else
     {
       str= &value2;
     }


     if ((str= s->val_str(str)))
     {
       if (set_null)
         owner->null_value= 0;
       return sortcmp(js, str, compare_collation());
     }
  }

error:
  if (set_null)
    owner->null_value= 1;
  return -1;
}


int Arg_comparator::compare_e_json_str_basic(Item *j, Item *s)
{
  String *res1,*res2;
  json_value_types type;
  char *value;
  int value_len, c_len;
  Item_func_json_extract *e= (Item_func_json_extract *) j;

  res1= e->read_json(&value1, &type, &value, &value_len);
  res2= s->val_str(&value2);

  if (!res1 || !res2)
    return MY_TEST(res1 == res2);

  if (type == JSON_VALUE_STRING)
  {
    if (value1.realloc_with_extra_if_needed(value_len) ||
        (c_len= json_unescape(value1.charset(), (uchar *) value,
                              (uchar *) value+value_len,
                              &my_charset_utf8_general_ci,
                              (uchar *) value1.ptr(),
                              (uchar *) (value1.ptr() + value_len))) < 0)
      return 1;
    value1.length(c_len);
    res1= &value1;
  }

  return MY_TEST(sortcmp(res1, res2, compare_collation()) == 0);
}
