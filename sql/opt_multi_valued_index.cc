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

/*
  Find Multi-Value Index created over array_indexed_expr.
*/
static Mv_index *get_mvi_index(List<Mv_index> *indexes,
                               Item *array_indexed_expr)
{
  Mv_index *index;
  List_iterator<Mv_index> it(*indexes);
  Item_func_mvi_encode *mvitem;
  while ((index= it++))
  {
    Field *vcol_field= index->vcol;
    DBUG_ASSERT(vcol_field->vcol_info->expr->type() == Item::FUNC_ITEM);
    DBUG_ASSERT(((Item_func *) vcol_field->vcol_info->expr)->functype() ==
                Item_func::MVI_ENCODE_FUNC);
    mvitem= (Item_func_mvi_encode *) vcol_field->vcol_info->expr;
    if (mvitem->arguments()[0]->eq(array_indexed_expr, true))
    {
      return index;
    }
  }
  return NULL;
}

/*
  Add one encoded element key to the access.
  
  TODO: String object live on MEM_ROOT and their destructor is never called
  (fix that or switch to something like LEX_STRINGs)
*/

bool Mvi_access::add_key(MEM_ROOT *mem_root, const String *key)
{
  String *s= new (mem_root) String;
  const char *copy= (const char *) memdup_root(mem_root, key->ptr(),
                                               key->length());
  if (!s || !copy)
    return true;
  s->set(copy, key->length(), &my_charset_latin1_bin);
  return encoded.push_back(s, mem_root);
}


/*
  @brief
    Build the boolean-mode fulltext query to find rows of interest.
    For conjunctive access it is

      '+encoded_foo +encoded_bar ...'

    For disjunctive access, it is

      'encoded_foo encoded_bar'
*/

bool Mvi_access::build_ft_query(String *out)
{
  List_iterator<String> it(encoded);
  String *key;
  out->length(0);
  out->set_charset(&my_charset_latin1_bin);
  while ((key= it++))
  {
    if ((out->length() && out->append(' ')) ||
        (conjunctive && out->append('+')) ||
        out->append(key->ptr(), key->length()))
      return true;
  }
  return !out->length();
}


/*
  @brief
    Check if we can use Multi-Value Index access to read rows for this
    predicate, if yes create an access descriptor.

  @detail
    Check if this item is a

      JSON_CONTAINS(array_indexed_expr, '[foo, bar, ... ]')

    If yes, collect the encoded element keys to search the index for.

    Elements that cannot be encoded for that index (e.g. because of a type
    mismatch) are skipped: the resulting access is a necessary, not a
    sufficient condition, and is only ever ANDed with this predicate.

  @return
    The access descriptor, or NULL if the predicate cannot use an MVI.
*/

Mvi_access *Item_func_json_contains::get_mvi_access(THD *thd,
                                                    List<Mv_index> *indexes)
{
  Mv_index *index;
  Mvi_access *access= NULL;
  StringBuffer<256> buf;
  const uchar *start, *end;
  DBUG_ASSERT(fixed());

  if (arg_count > 2 || !a2_constant)
    return NULL;
  /* Find the MVI that matches the first argument */
  if (!(index= get_mvi_index(indexes, args[0])))
    return NULL;

  CHARSET_INFO *cs= args[0]->collation.collation;
  Item_func_mvi_encode *mvitem=
    (Item_func_mvi_encode *) index->vcol->vcol_info->expr;
  /* Get ready to encode the element keys from the second argument */
  const Type_handler *cast_th= mvitem->cast_type().type_handler();

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
    /* A scalar: JSON_CONTAINS(expr, '123') */
    if (encode_mvi_key(&je, cast_th, cs, &buf))
      return NULL;
    if (!(access= new (thd->mem_root) Mvi_access(index, true)) ||
        access->add_key(thd->mem_root, &buf))
      return NULL;
    return access;
  }
  // JSON_VALUE_ARRAY

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
          return NULL;

        if (encode_mvi_key(&je, cast_th, cs, &buf))
          break;                            /* Skip: cannot be encoded */
        if (!access &&
            !(access= new (thd->mem_root) Mvi_access(index, true)))
          return NULL;
        if (access->add_key(thd->mem_root, &buf))
          return NULL;
        break;
      }
      default:
        return NULL;
    }
  } while (json_scan_next(&je) == 0);

  return access;
}


bool Item_func_json_contains::mvi_analyze(void *arg)
{
  Mvi_context *ctx= (Mvi_context *) arg;
  Mvi_access *access= get_mvi_access(ctx->thd, &ctx->indexes);
  if (access && ctx->accesses.push_back(access, ctx->thd->mem_root))
    return true;                                          /* Out of memory */
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
  {
    DBUG_ASSERT(access->index->vcol->table->tablenr < MAX_TABLES);
    best[access->index->vcol->table->tablenr] = access;
  }
}


/*
  @brief
    Collect the MVI accesses allowed by the top-level AND-parts of `conds'.

  @detail
    An MVI access only reads the rows the index scan matches, so we can only
    use it for a predicate that has to be true for every row of the result.
    That means the top-level conjuncts and nothing else: for

      json_contains(j1->'$.tags', '"a"') OR json_contains(j2->'$.tags', '"a"')

    a scan of either index would drop the rows that only match the other
    branch.
*/

static bool collect_mvi_accesses(Mvi_context *ctx, Item *conds)
{
  Item *cond;
  if (conds->type() != Item::COND_ITEM)
    return conds->mvi_analyze(ctx);
  if (((Item_cond *) conds)->functype() != Item_func::COND_AND_FUNC)
    return false;
  List_iterator<Item> it(*((Item_cond *) conds)->argument_list());
  while ((cond= it++))
  {
    /*
      No recursion: a nested Item_cond is either an already-flattened AND or
      an OR, and Item::mvi_analyze() ignores both.
    */
    if (cond->mvi_analyze(ctx))
      return true;
  }
  return false;
}


/*
  @brief
    Analyze the WHERE clause and find the MVI accesses it allows.

  @detail
    The accesses are saved in join->mvi_ctx, where get_best_mvi_access() picks
    them up during the range analysis of each table.
*/

bool setup_mvi_quick(JOIN *join)
{
  THD *thd= join->thd;
  Mvi_context *ctx;
  /* mvi_ctx must describe this analysis only, including on the early exits */
  join->mvi_ctx= NULL;
  if (!join->conds)
    return false;
  if (!(ctx= new (thd->mem_root) Mvi_context(thd)))
    return true;
  if (collect_mvi_vcols_for_join(join, &ctx->indexes))
    return true;
  if (ctx->indexes.is_empty())
    return false;
  if (collect_mvi_accesses(ctx, join->conds))
    return true;
  if (ctx->accesses.is_empty())
    return false;
  choose_mvi_access_for_tables(&ctx->accesses, ctx->best);
  join->mvi_ctx= ctx;
  return false;
}


Mvi_access *JOIN::get_mvi_access_for_table(TABLE *table)
{
  if (!mvi_ctx)
    return NULL;
  DBUG_ASSERT(table->tablenr < MAX_TABLES);
  return mvi_ctx->best[table->tablenr];
}


/*
  @brief
    Create a quick select for the best MVI access to `table', if there is one.

  @detail
    The range optimizer cannot produce this access (it skips fulltext keys),
    so the caller creates it here and compares its cost with whatever
    test_quick_select() came up with.
*/

QUICK_SELECT_I *get_best_mvi_access(THD *thd, JOIN *join, TABLE *table)
{
  Mvi_access *access= join->get_mvi_access_for_table(table);
  if (!access)
    return NULL;
  return new QUICK_MVI_SELECT(thd, table, access);
}


/****************************************************************************
  QUICK_MVI_SELECT - reading a multi-valued index
****************************************************************************/

QUICK_MVI_SELECT::QUICK_MVI_SELECT(THD *thd, TABLE *table,
                                   Mvi_access *access_arg)
  : access(access_arg), ft_handler(NULL)
{
  head= table;
  index= access->index->keyno;
  record= head->record[0];
  /*
    TODO: get a real estimate from the engine (see fulltext_estimate()).
    Until then, use numbers low enough that the MVI scan is preferred over a
    table scan.
  */
  records= 10;
  read_time= 0.001;
}


QUICK_MVI_SELECT::~QUICK_MVI_SELECT()
{
  handler *file= head->file;
  if (ft_handler)
  {
    file->ha_ft_end();                /* ft_end() + file->ft_handler= NULL */
    /*
      We created the FT_INFO, so we free it. For an Item_func_match this is
      done by Item_func_match::cleanup().
    */
    ft_handler->please->close_search(ft_handler);
    ft_handler= NULL;
  }
  if (file->inited != handler::NONE)
    file->ha_index_or_rnd_end();
}


int QUICK_MVI_SELECT::reset()
{
  handler *file= head->file;
  int error;

  if (!ft_handler)
  {
    if (access->build_ft_query(&query))
      return HA_ERR_OUT_OF_MEM;
    if (!(ft_handler= file->ft_init_ext(FT_BOOL, index, &query)))
      return HA_ERR_WRONG_COMMAND;    /* the error is already reported */
    /*
      ft_init() and ha_ft_read() both work off handler::ft_handler (and
      ha_innobase::ft_init() dereferences it without checking), so it has to
      be set before we go any further.
    */
    file->ft_handler= ft_handler;
    head->fulltext_searched= 1;
  }
  if (!file->inited && (error= file->ha_index_init(index, 1)))
    return error;
  /* This rewinds the search, so it is also right for a repeated reset() */
  return file->ft_init();
}


int QUICK_MVI_SELECT::get_next()
{
  return head->file->ha_ft_read(record);
}


void QUICK_MVI_SELECT::add_keys_and_lengths(String *key_names,
                                            String *used_lengths)
{
  bool first= TRUE;

  add_key_and_length(key_names, used_lengths, &first);
}


Explain_quick_select *QUICK_MVI_SELECT::get_explain(MEM_ROOT *local_alloc)
{
  Explain_quick_select *res;
  if ((res= new (local_alloc) Explain_quick_select(QS_TYPE_MVI)))
    res->range.set(local_alloc, &head->key_info[index], max_used_key_length);
  return res;
}


#ifndef DBUG_OFF
void QUICK_MVI_SELECT::dbug_dump(int indent, bool verbose)
{
  fprintf(DBUG_FILE, "%*squick_mvi_select: index %s (%d)\n",
          indent, "", head->key_info[index].name.str, index);
}
#endif

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
