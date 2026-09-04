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

static bool encode_mvi_key(json_engine_t *je, const Type_handler *cast_th,
                           CHARSET_INFO *cs, String *buf)
{
  enum_field_types cast_ftype= cast_th->field_type();
  bool is_unsigned= cast_th->is_unsigned();
  StringBuffer<42> sorted;
  /* Skip encoding on type incompatibility */
  if (mvi_json_class(cast_ftype) != je->value_type)
    return true;
  /* 1. sort_string */
  sorted.length(0);
  /* TODO: handle temporal types and decimal */
  switch(cast_ftype)
  {
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
      return true;
  }

  /* 2. hex */
  buf->append_hex(sorted.c_ptr(), sorted.length());

  /* 3. pad */
  if (sorted.length() == 0)
    buf->append(STRING_WITH_LEN("xxxx"));
  else if (sorted.length() == 1)
    buf->append(STRING_WITH_LEN("xx"));

  return false;
}

String *Item_func_mvi_encode::val_str_ascii(String *buf)
{
  String *value= args[0]->val_json(&tmp_js);
  if ((null_value= !value))
    return nullptr;
  CHARSET_INFO *cs= value->charset();
  const Type_handler *cast_th= m_cast_type.type_handler();
  bool end_ok= false, at_least_one= false;
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
        /*
          TODO: do something different when an empty string is
          returned, i.e. at_least_one == false to avoid wasting index
          space?
        */
        if (at_least_one)
          buf->length(buf->length() - 1);
        end_ok = true;
        break;
      case JST_VALUE:
      {
        if (json_read_value(&je))
          goto json_error;

        if (!encode_mvi_key(&je, cast_th, cs, buf))
        {
          buf->append(' ');
          at_least_one= true;
        }
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

/* Collect all the MVI indexes in `join' */
static
bool collect_mvi_vcols_for_join(JOIN *join, List<Mv_index> *indexes)
{
  List_iterator<TABLE_LIST> ti(join->select_lex->leaf_tables);
  TABLE_LIST *tl;
  TABLE *table;
  THD *thd= join->thd;
  while ((tl= ti++))
  {
    if (!(table= tl->table)) // non-merged semi-join or something like that
      continue;
    for (uint i=0; i < table->s->keys; i++)
    {
      if (!table->keys_in_use_for_query.is_set(i))
        continue;

      KEY *key= &table->key_info[i];
      for (uint kp=0; kp < key->user_defined_key_parts; kp++)
      {
        /* TODO: "legacy" */
        if (!(key->flags & HA_FULLTEXT_legacy)) continue;
        Field *field= key->key_part[kp].field;
        if (field->invisible == INVISIBLE_FULL &&
            field->vcol_info &&
            field->vcol_info->expr->type() == Item::FUNC_ITEM &&
            ((Item_func *) field->vcol_info->expr)->functype() ==
            Item_func::MVI_ENCODE_FUNC)
        {
          Mv_index *index= new (thd->mem_root) Mv_index(field, i);
          if (indexes->push_back(index))
            return TRUE; // Out of memory
        }
      }
    }
  }
  return FALSE; // Ok
}

class Mvi_context
{
 public:
  THD *thd;
  /* All MV indexes in the JOIN */
  List<Mv_index> indexes;
  /* MVI accesses for all eligible predicates in WHERE */
  List<Mvi_access> accesses;

  Mvi_context(THD *thd_arg) : thd(thd_arg) {}
};

bool Item_func_json_contains::mvi_analyze(void *arg)
{
  Mvi_context *ctx= (Mvi_context *) arg;
  List_iterator<Mv_index> it(ctx->indexes);
  Field *vcol_field;
  Mv_index *index;
  CHARSET_INFO *cs= NULL;
  Item_func_mvi_encode *mvitem= NULL;
  DBUG_ASSERT(fixed());
  if (arg_count > 2 || !a2_constant)
    return false;
  /* Find the MVI that matches the first argument */
  while ((index= it++))
  {
    vcol_field= index->vcol;
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
  if (!index)
    return false;

  /* Get ready to construct the ft queries from the second argument */
  const Type_handler *cast_th= mvitem->cast_type().type_handler();
  const uchar *start, *end;
  Mvi_access *access= NULL;
  StringBuffer<256> buf;
  buf.length(0);
  buf.set_charset(&my_charset_latin1_bin);
  DBUG_ASSERT(fixed());
  if (!a2_parsed)
  {
    val= args[1]->val_json(&tmp_val);
    a2_parsed= true;
  }
  if (!val)
    return false;
  start= reinterpret_cast<const uchar *>(val->ptr());
  end= start + val->length();

  if (json_scan_start(&je, cs, start, end) || json_read_value(&je))
    return false;

  if (je.value_type == JSON_VALUE_UNINITIALIZED ||
      je.value_type == JSON_VALUE_OBJECT)
    return false;
  if (je.value_type != JSON_VALUE_ARRAY)
  {
    /* scalar */
    if (!encode_mvi_key(&je, cast_th, cs, &buf))
    {
      access= new (ctx->thd->mem_root) Mvi_access(index, true);
      /* TODO: there gotta be a less verbose way to construct s. */
      String *s= new (ctx->thd->mem_root) String;
      s->set_charset(&my_charset_latin1_bin);
      if (s->copy(buf.ptr(), buf.length(), &my_charset_latin1_bin))
        return true;
      access->encoded.push_back(s);
    }
    goto ok;
  }

  /* TODO: deduplicate? */
  do {
    buf.length(0);
    switch (je.state)
    {
      /* TODO: nested array? */
      case JST_ARRAY_START:
        continue;
      case JST_ARRAY_END:
        break;
      case JST_VALUE:
      {
        if (json_read_value(&je))
          return false;

        if (!encode_mvi_key(&je, cast_th, cs, &buf))
        {
          if (!access)
            access= new (ctx->thd->mem_root) Mvi_access(index, true);
          /* TODO: there gotta be a less verbose way to construct s. */
          String *s= new (ctx->thd->mem_root) String;
          s->set_charset(&my_charset_latin1_bin);
          if (s->copy(buf.ptr(), buf.length(), &my_charset_latin1_bin))
            return true;
          access->encoded.push_back(s);
        }
        break;
      }
      default:
        return false;
    }
  } while (json_scan_next(&je) == 0);

ok:
  if (access)
    ctx->accesses.push_back(access);
  return false;
}


Item *Item_func_json_contains::create_ft_for_mvi(THD *thd,
                                                 List<Mv_index> *indexes)
{
  List_iterator<Mv_index> it(*indexes);
  Mv_index *index;
  Field *vcol_field= NULL;
  CHARSET_INFO *cs= NULL;
  Item_func_mvi_encode *mvitem= NULL;
  DBUG_ASSERT(fixed());
  if (arg_count > 2 || !a2_constant)
    return NULL;
  while ((index= it++))
  {
    vcol_field= index->vcol;
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
  if (!index)
    return NULL;

  const Type_handler *cast_th= mvitem->cast_type().type_handler();
  StringBuffer<256> buf;
  const uchar *start, *end;
  List<Item> ifm_args;
  Item_field *ivcol;
  Item_string *ift_query;
  bool at_least_one= false;
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
    if (!encode_mvi_key(&je, cast_th, cs, &buf))
      at_least_one= true;
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
        if (at_least_one)
          buf.length(buf.length() - 1);
        break;
      case JST_VALUE:
      {
        if (json_read_value(&je))
          return NULL;

        buf.append('+');
        if (encode_mvi_key(&je, cast_th, cs, &buf))
          buf.length(buf.length() - 1);
        else
        {
          buf.append(' ');
          at_least_one= true;
        }
        break;
      }
      default:
        return NULL;
    }
  } while (json_scan_next(&je) == 0);

ok:
  if (!at_least_one)
    return NULL;
  ift_query= new (thd->mem_root) Item_string(thd, &my_charset_latin1_bin,
                                             buf.c_ptr(), buf.length());
  ifm_args.push_back(ift_query);
  ivcol= new (thd->mem_root) Item_field(thd, vcol_field);
  ifm_args.push_back(ivcol);
  return new (thd->mem_root) Item_func_match(thd, ifm_args, FT_BOOL);
}

static bool add_ft_for_mvi(Mvi_context *ctx, Item **conds_ref,
                           List<Item_func_match> *ftfunc_list)
{
  Item *conds= *conds_ref;
  Item *cond, *match;
  List<Item> matches;
  THD *thd= ctx->thd;
  if (conds->type() != Item::COND_ITEM)
  {
    if ((match= conds->create_ft_for_mvi(thd, &ctx->indexes)))
    {
      matches.push_back(match);
      ftfunc_list->push_back((Item_func_match *) match);
    }
  }
  else if (((Item_cond *) conds)->functype() == Item_func::COND_OR_FUNC)
    return false;
  else
  {
    List_iterator<Item> it(*((Item_cond *) conds)->argument_list());
    while ((cond= it++))
    {
      if ((match= cond->create_ft_for_mvi(thd, &ctx->indexes)))
      {
        matches.push_back(match);
        ftfunc_list->push_back((Item_func_match *) match);
      }
    }
  }
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

static void choose_mvi_access_for_tables(List<Mvi_access> *accesses, Mvi_access **best)
{
  List_iterator<Mvi_access> it(*accesses);
  /* TODO: cost based */
  /*
    TODO: merge

    json_contains(j->'$.tags','"a"') and
    json_contains(j->'$.tags','"b"')

    (+ta +tb)
  */
  while (Mvi_access *access= it++)
    best[access->index->vcol->table->tablenr] = access;
}

/* Build the scan and install it to join */
bool setup_mvi_quick(JOIN *join)
{
  Mvi_context ctx(join->thd);
  Mvi_access *best[MAX_TABLES];
  bzero(best, sizeof(best));
  if (!join->conds)
    return false;
  if (collect_mvi_vcols_for_join(join, &ctx.indexes))
    return true;
  if (!ctx.indexes.is_empty() &&
      join->conds->walk(&Item::mvi_analyze, &ctx, WALK_SUBQUERY))
    return true;
  choose_mvi_access_for_tables(&ctx.accesses, best);
  return false;
}

bool setup_mvi_for_join(JOIN *join)
{
  Mvi_context ctx(join->thd);
  if (!join->conds)
    return false;
  if (collect_mvi_vcols_for_join(join, &ctx.indexes))
    return true;
  if (!ctx.indexes.is_empty())
    return add_ft_for_mvi(&ctx, &join->conds, join->select_lex->ftfunc_list);
  return false;
}

enum json_value_types mvi_json_class(enum_field_types ftype)
{
  switch (ftype)
  {
    case MYSQL_TYPE_TINY:
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_INT24:
    case MYSQL_TYPE_LONG:
    case MYSQL_TYPE_LONGLONG:
    case MYSQL_TYPE_DOUBLE:
    case MYSQL_TYPE_DECIMAL:
    case MYSQL_TYPE_FLOAT:
    case MYSQL_TYPE_NEWDECIMAL:
      return JSON_VALUE_NUMBER;
    case MYSQL_TYPE_TIMESTAMP:
    case MYSQL_TYPE_DATE:
    case MYSQL_TYPE_TIME:
    case MYSQL_TYPE_DATETIME:
    case MYSQL_TYPE_YEAR:
    case MYSQL_TYPE_NEWDATE:
    case MYSQL_TYPE_VARCHAR:
    case MYSQL_TYPE_TIMESTAMP2:
    case MYSQL_TYPE_DATETIME2:
    case MYSQL_TYPE_TIME2:
    case MYSQL_TYPE_TINY_BLOB:
    case MYSQL_TYPE_MEDIUM_BLOB:
    case MYSQL_TYPE_LONG_BLOB:
    case MYSQL_TYPE_BLOB:
    case MYSQL_TYPE_VAR_STRING:
    case MYSQL_TYPE_STRING:
      return JSON_VALUE_STRING;
    default:
      return JSON_VALUE_UNINITIALIZED;
  }
}
