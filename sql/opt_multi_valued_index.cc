/*
   Copyright (c) 2026, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA */

#include "mariadb.h"
#include "sql_select.h"
#include "item_func.h"

void Item_func_mvi_encode::print(String *str, enum_query_type query_type)
{
  char buf[32];
  size_t length;
  str->append(func_name_cstring());
  str->append('(');
  args[0]->print(str, query_type);
  str->append(',');
  const Name name= m_cast_type.type_handler()->name();
  switch (m_cast_type.type_handler()->field_type())
  {
    case MYSQL_TYPE_LONG_BLOB:
      str->append(STRING_WITH_LEN("char"));
      str->append('(');
      length= (size_t) (longlong10_to_str(m_cast_type.length(), buf, -10) - buf);
      str->append(buf, length);
      str->append(')');
      break;
    default:
      str->append(name.ptr(), name.length());
      break;
  }
  /* TODO: this is copied from another print() implementation */
  if (decimals && decimals != NOT_FIXED_DEC)
  {
    str->append('(');
    length= (size_t) (longlong10_to_str(decimals, buf, -10) - buf);
    str->append(buf, length);
    str->append(')');
  }
  str->append(')');
}

/* TODO: this duplicates logic in Item_func_json_extract::val_int */
static longlong json_value_to_longlong(enum json_value_types type,
                                       CHARSET_INFO *cs,
                                       char* value, int value_len)
{
  switch (type)
  {
    case JSON_VALUE_NUMBER:
    case JSON_VALUE_STRING:
    {
      char *end;
      int err;
      return cs->strntoll(value, value_len, 10, &end, &err);
    }
    case JSON_VALUE_TRUE:
      return 1;
    default:
      return 0;
  };
}

/* Lifted from Type_handler method of the same name */
static void store_sort_key_longlong(uchar *to, bool unsigned_flag,
                                    longlong value)
{
  to[7]= (uchar) value;
  to[6]= (uchar) (value >> 8);
  to[5]= (uchar) (value >> 16);
  to[4]= (uchar) (value >> 24);
  to[3]= (uchar) (value >> 32);
  to[2]= (uchar) (value >> 40);
  to[1]= (uchar) (value >> 48);
  to[0]= (uchar) (value >> 56) ^ (unsigned_flag ? 0 : 128);
}

static void encode_mvi_key(json_engine_t *je, const Type_handler *cast_th,
                           CHARSET_INFO *cs, String *buf)
{
  enum_field_types cast_ftype= cast_th->field_type();
  bool is_unsigned= cast_th->is_unsigned();
  StringBuffer<42> sorted;
  /* 1. sort_string */
  sorted.length(0);
  /* TODO: handle temporal types and decimal */
  switch(cast_ftype)
  {
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_LONGLONG:
      store_sort_key_longlong(
      (uchar *) sorted.c_ptr(), is_unsigned,
      json_value_to_longlong(je->value_type, cs,
                             (char *) je->value, je->value_len));
      sorted.length(8);
      break;
      /* TODO: unquote? */
      /* CHAR(n) => LONG BLOB */
    case MYSQL_TYPE_LONG_BLOB:
    {
      /* Trim trailing whitespaces if possible */
      if (!(cs->state & MY_CS_NOPAD))
        je->value_len= (int) cs->lengthsp((const char *) je->value,
                                         je->value_len);
      if (my_binary_compare(cs))
        sorted.set((char *) je->value, je->value_len,
                   &my_charset_latin1_bin);
      else
      {
        my_strnxfrm_ret_t rc= cs->strnxfrm(
        (uchar *) sorted.c_ptr(), 42, 42, je->value, je->value_len, 0);
        sorted.length(rc.m_result_length);
      }
      break;
    }
    default:
      break;
  }

  /* 2. hex */
  buf->append_hex(sorted.c_ptr(), sorted.length());

  /* 3. pad */
  if (sorted.length() == 0)
    buf->append(STRING_WITH_LEN("xxxx"));
  else if (sorted.length() == 1)
    buf->append(STRING_WITH_LEN("xx"));

  /* 4. space */
  buf->append(STRING_WITH_LEN(" "));
}

String *Item_func_mvi_encode::val_str_ascii(String *buf)
{
  String *value= args[0]->val_json(&tmp_js);
  CHARSET_INFO *cs= value->charset();
  const Type_handler *cast_th= m_cast_type.type_handler();
  bool end_ok= false;
  const uchar *start= reinterpret_cast<const uchar *>(value->ptr());
  const uchar *end= start + value->length();
  DBUG_ASSERT(fixed());
  buf->length(0);
  buf->set_charset(&my_charset_latin1_bin);

  if (json_scan_start(&je, cs, start, end) ||
      json_read_value(&je))
    goto json_error;

  if (je.value_type != JSON_VALUE_ARRAY)
    goto error_format;

  /* TODO: deduplicate, so that ["34567", 34567] yield only one token */
  do {
    switch (je.state)
    {
      case JST_ARRAY_START:
        continue;
      case JST_ARRAY_END:
        buf->length(buf->length() - 1);
        end_ok = true;
        break;
      case JST_VALUE:
      {
        if (json_read_value(&je))
          goto json_error;

        encode_mvi_key(&je, cast_th, cs, buf);
        break;
      }
      default:
        goto error_format;
    }
  } while (json_scan_next(&je) == 0);

  if (end_ok)
    return buf;

error_format:
  {
    int position= (int) ((const char *) je.s.c_str - value->ptr());
    /* TODO: fix error */
    push_warning_printf(current_thd, Sql_condition::WARN_LEVEL_WARN,
                        ER_VECTOR_FORMAT_INVALID, ER(ER_VECTOR_FORMAT_INVALID),
                        position, value->c_ptr_safe());
    null_value= true;
    return nullptr;
  }

json_error:
  report_json_error_ex(value->ptr(), &je, func_name(),
                       0, Sql_condition::WARN_LEVEL_WARN);
  null_value= true;
  return nullptr;
}

bool Item_func_mvi_encode::fix_length_and_dec(THD *thd)
{
  /* TODO: validate args[0] is a json array */
  mem_root_dynamic_array_init(thd->mem_root, PSI_INSTRUMENT_MEM,
                              &je.stack, sizeof(int), NULL,
                              JSON_DEPTH_DEFAULT, JSON_DEPTH_INC, MYF(0));
  decimals= 0;
  fix_length_and_charset(args[0]->max_char_length() * 2,
                         &my_charset_latin1_bin);
  set_maybe_null();
  return false;
}


/*
  Collect indexed columns used by ARRAY fulltext indexes.
*/

static
bool collect_mvi_vcols_for_join(JOIN *join, List<Field> *vcol_fields)
{
  List_iterator<TABLE_LIST> ti(join->select_lex->leaf_tables);
  TABLE_LIST *tl;
  TABLE *table;
  while ((tl= ti++))
  {
    if (!(table= tl->table)) // non-merged semi-join or something like that
      continue;
    // TODO: Make use of iterator to loop through
    // keys_in_use_for_query, instead.
    for (uint i=0; i < table->s->keys; i++)
    {
      // note: we could also support histograms here
      //    (probably elsewhere as here we don't have access to the conditions)
      if (!table->keys_in_use_for_query.is_set(i))
        continue;

      KEY *key= &table->key_info[i];
      for (uint kp=0; kp < key->user_defined_key_parts; kp++)
      {
        Field *field= key->key_part[kp].field;
        if (field->invisible == INVISIBLE_FULL &&
            field->vcol_info &&
            field->vcol_info->expr->type() == Item::FUNC_ITEM &&
            ((Item_func *) field->vcol_info->expr)->functype() ==
              Item_func::MVI_ENCODE_FUNC &&
            vcol_fields->push_back(field))
          return TRUE; // Out of memory
      }
    }
  }
  return FALSE; // Ok
}

class Mvi_context
{
 public:
  THD *thd;
  /* Virtual columns with fulltext index that we can try substituting */
  List<Field> vcol_fields;

  Mvi_context(THD *thd_arg) : thd(thd_arg) {}
};


/*
  Create a fulltext search item that matches this JSON_CONTAINS(...) predicate.
  
  @detail
    Check if this item is 

      JSON_CONTAINS(json_field, '[foo, bar, ... ]')

    and then create nd return

      MATCH vcol AGAINST ('+encoded_foo +encoded_bar ...' IN BOOLEAN MODE)
*/

Item *Item_func_json_contains::create_ft_for_mvi(THD *thd,
                                                 List<Field> *vcol_fields)
{
  List_iterator<Field> it(*vcol_fields);
  Field *vcol_field;
  CHARSET_INFO *cs;
  Item_func_mvi_encode *mvitem;
  DBUG_ASSERT(fixed());
  if (arg_count > 2 || !a2_constant)
    return NULL;
  while ((vcol_field= it++))
  {
    DBUG_ASSERT(vcol_field->vcol_info->expr->type() == FUNC_ITEM);
    DBUG_ASSERT(((Item_func *) vcol_field->vcol_info->expr)->functype() ==
                MVI_ENCODE_FUNC);
    mvitem= (Item_func_mvi_encode *) vcol_field->vcol_info->expr;
    if (mvitem->arguments()[0]->eq(args[0], true))
    {
      cs= mvitem->arguments()[0]->collation.collation;
      break;
    }
  }
  if (!vcol_field)
    return NULL;

  const Type_handler *cast_th= mvitem->cast_type().type_handler();
  StringBuffer<42> sorted;
  StringBuffer<256> buf;
  const uchar *start, *end;
  List<Item> ifm_args;
  Item_field *ivcol;
  Item_string *ift_query;
  DBUG_ASSERT(fixed());
  buf.length(0);
  buf.set_charset(&my_charset_latin1_bin);
  if (!a2_parsed)
  {
    val= args[1]->val_json(&tmp_val);
    a2_parsed= true;
  }
  if (!val)
    return NULL;
  start= reinterpret_cast<const uchar *>(val->ptr());
  end= start + val->length();

  if (json_scan_start(&je, cs, start, end) || json_read_value(&je))
    return NULL;

  if (je.value_type == JSON_VALUE_UNINITIALIZED ||
      je.value_type == JSON_VALUE_OBJECT)
    return NULL;
  if (je.value_type != JSON_VALUE_ARRAY)
  {
    /* scalar */
    encode_mvi_key(&je, cast_th, cs, &buf);
    buf.length(buf.length() - 1);
    goto ok;
  }

  /* TODO: deduplicate? */
  do {
    switch (je.state)
    {
      /* TODO: nested array? */
      case JST_ARRAY_START:
        continue;
      case JST_ARRAY_END:
        buf.length(buf.length() - 1);
        break;
      case JST_VALUE:
      {
        if (json_read_value(&je))
          return NULL;

        buf.append('+');
        encode_mvi_key(&je, cast_th, cs, &buf);
        break;
      }
      default:
        return NULL;
    }
  } while (json_scan_next(&je) == 0);

ok:
  ift_query= new (thd->mem_root) Item_string(thd, &my_charset_latin1_bin,
                                             buf.c_ptr(), buf.length());
  ifm_args.push_back(ift_query);
  ivcol= new (thd->mem_root) Item_field(thd, vcol_field);
  ifm_args.push_back(ivcol);
  return new (thd->mem_root) Item_func_match(thd, ifm_args, FT_BOOL);
}

/*
  Walk (*conds_ref) and add conditions for multi-value index predicates.

  Since we add fulltext predicates, also add them into *ftfunc_list.
*/
static bool add_ft_for_mvi(Mvi_context *ctx, Item **conds_ref,
                           List<Item_func_match> *ftfunc_list)
{
  Item *conds= *conds_ref;
  Item *cond, *match;
  List<Item> matches;
  THD *thd= ctx->thd;
  if (conds->type() != Item::COND_ITEM)
  {
    if ((match= conds->create_ft_for_mvi(thd, &ctx->vcol_fields)))
    {
      matches.push_back(match);
      ftfunc_list->push_back((Item_func_match *) match);
    }
  }
  else
  {
    List_iterator<Item> it(*((Item_cond *) conds)->argument_list());
    while ((cond= it++))
    {
      if ((match= cond->create_ft_for_mvi(thd, &ctx->vcol_fields)))
      {
        matches.push_back(match);
        ftfunc_list->push_back((Item_func_match *) match);
      }
    }
  }
  // TODO: Does this distinguish between AND/OR ??? 
  if (matches.elements == 1)
    cond= matches.pop();
  else
    cond= new (thd->mem_root) Item_cond_and(thd, matches);
  if (cond &&
      ((cond->fix_fields(thd, &cond) ||
        !(conds= and_items(thd, conds, cond)) ||
        conds->fix_fields(thd, &conds))))
    return true;
  *conds_ref= conds;
  return false;
}

bool setup_mvi_for_join(JOIN *join)
{
  Mvi_context ctx(join->thd);
  if (collect_mvi_vcols_for_join(join, &ctx.vcol_fields))
    return true;
  if (!ctx.vcol_fields.is_empty() && join->conds)
    return add_ft_for_mvi(&ctx, &join->conds, join->select_lex->ftfunc_list);
  return false;
}
