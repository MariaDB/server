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
#include "sql_table.h"                        /* make_internal_field_name */
#include "item_func.h"
#include "my_json_writer.h"

static QUICK_SELECT_I *create_quick_mvi_select(THD *thd, TABLE *table,
                                               Mvi_access *access);

/*
  Append to *str string representation of m_cast_type.
*/
void Item_func_mvi_encode::append_cast_type(String *str) const
{
  char buf[32];
  size_t length;
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
    case MYSQL_TYPE_LONGLONG:
      if (m_cast_type.type_handler()->is_unsigned())
        str->append(STRING_WITH_LEN("unsigned"));
      else
        str->append(STRING_WITH_LEN("int"));
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
}


void Item_func_mvi_encode::print(String *str, enum_query_type query_type)
{
  str->append(func_name_cstring());
  str->append('(');
  args[0]->print(str, query_type);
  str->append(',');
  append_cast_type(str);
  str->append(')');
}


/*
  @brief
    Print the index expression the way it was written:

      CAST(<expr> AS <type> ARRAY)

  @detail
    print() cannot do this. Its output is what pack_expression() writes into
    the FRM, and that is read back as a call of mvi_encode(), which is the
    only form the parser accepts outside an index definition.
*/

void Item_func_mvi_encode::print_as_array_cast(String *str)
{
  str->append(STRING_WITH_LEN("cast("));
  /* The same flags the other parts of a table definition are printed with */
  args[0]->print_for_table_def(str);
  str->append(STRING_WITH_LEN(" as "));
  append_cast_type(str);
  str->append(STRING_WITH_LEN(" array)"));
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

/* Copied from Type_handler::store_sort_key_longlong */
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


/*
  @brief
    Encode the current JSON value in *je to either store or look it up in
    Multi-Value Index. The index uses cast_th datatype.

  @detail
    The encoded value shouldn't have space, punctuation or other similar
    characters, as we're using the default Fulltext parser and want the
    encoded value treated as one "term".

    If the value cannot be encoded this means it is not stored, also
    searches won't find any matches for it.

    A key image longer than a fulltext token can be is cut short instead:
    the engine drops a token that long, on the DML path and on the index
    build path alike (fts_check_token()), and a value with no key in the
    index is a value the index cannot be used for at all. Two values that
    agree on the first MVI_KEY_IMAGE_MAX_LEN bytes of their image then
    share a key, which costs false positives and nothing else -- the
    predicate is rechecked on every row the index produces. The strnxfrm()
    branch below has always worked that way; it asks for exactly that many
    bytes of weights and cannot get more back.

  @return
    false   Encoded successfully, the key is appended to *buf
    true    The JSON value cannot be represented in the index datatype.
            Nothing is appended.
*/

bool encode_mvi_key(json_engine_t *je, const Type_handler *cast_th,
                    CHARSET_INFO *cs, String *buf)
{
  enum_field_types cast_ftype= cast_th->field_type();
  bool is_unsigned= cast_th->is_unsigned();
  StringBuffer<MVI_KEY_IMAGE_MAX_LEN> sorted;
  /* Skip encoding on type incompatibility */
  if (mvi_json_class(cast_ftype) != je->value_type)
    return true;
  /* 1. sort_string */
  sorted.length(0);
  /* TODO: handle temporal types and decimal */
  switch(cast_ftype)
  {
    case MYSQL_TYPE_LONGLONG:
    {
      longlong val= json_value_to_longlong(je->value_type, cs,
                                           (char *) je->value,
                                           je->value_len);
      sorted.length(8);
      store_sort_key_longlong((uchar *) sorted.c_ptr(),
                              is_unsigned, val);
      break;
    }
    /* TODO: unquote? */
    /* CHAR(n) => LONG BLOB */
    case MYSQL_TYPE_LONG_BLOB:
    {
      /* Trim trailing whitespaces if possible */
      if (!(cs->state & MY_CS_NOPAD))
      {
        je->value_len= (int) cs->lengthsp((const char *) je->value,
                                         je->value_len);
      }
      if (my_binary_compare(cs))
      {
        sorted.set((char *) je->value, je->value_len,
                   &my_charset_latin1_bin);
      }
      else
      {
        // TODO: Is this ever used outside of "SELECT MVI_ENCODE()" ?
        my_strnxfrm_ret_t rc=
          cs->strnxfrm((uchar *) sorted.c_ptr(),
                       /*buffer_size*/ MVI_KEY_IMAGE_MAX_LEN,
                       /*n_weights*/ MVI_KEY_IMAGE_MAX_LEN,
                       je->value, je->value_len, 0);
        sorted.length(rc.m_result_length);
      }
      break;
    }
    default:
      return true;
  }

  /* 2. cut what the engine would not index down to what it will, see above */
  if (sorted.length() > MVI_KEY_IMAGE_MAX_LEN)
    sorted.length(MVI_KEY_IMAGE_MAX_LEN);

  /* 3. hex */
  buf->append_hex(sorted.c_ptr(), sorted.length());

  /* 4. pad */
  if (sorted.length() == 0)
    buf->append(STRING_WITH_LEN("xxxx"));
  else if (sorted.length() == 1)
    buf->append(STRING_WITH_LEN("xx"));

  return false;
}


/*
  @brief
    Read the element the scan is positioned on. See Mvi_array_iterator.

    TODO: deduplicate, so that ["34567", "34567"] yield only one key
*/

Mvi_array_iterator::Event Mvi_array_iterator::read_and_encode_element()
{
  uint32 key_start;
  if (json_read_value(m_je))
    return MVI_WALK_JSON_ERROR;

  if (m_je->value_type == JSON_VALUE_ARRAY)
  {
    DBUG_ASSERT(m_je->state == JST_ARRAY_START);
    m_event_depth= ++m_depth;
    return MVI_NESTED_START;
  }
  m_event_depth= m_depth;

  if (m_je->value_type == JSON_VALUE_OBJECT)
    return json_skip_level(m_je) ? MVI_WALK_JSON_ERROR : MVI_NO_KEY;

  key_start= m_key->length();
  if (encode_mvi_key(m_je, m_cast_th, m_cs, m_key))
  {
    m_key->length(key_start);   /* Leave the buffer as we found it */
    return MVI_NO_KEY;
  }
  return MVI_KEY;
}


Mvi_array_iterator::Event Mvi_array_iterator::start(const uchar *start, const uchar *end)
{
  if (json_scan_start(m_je, m_cs, start, end) || json_read_value(m_je))
    return MVI_WALK_JSON_ERROR;

  if (m_je->value_type != JSON_VALUE_ARRAY)
    return MVI_WALK_NOT_ARRAY;

  /* The scan is on the JST_ARRAY_START of the array we are to walk */
  DBUG_ASSERT(m_je->state == JST_ARRAY_START);
  m_depth= m_event_depth= 1;
  return next();
}


Mvi_array_iterator::Event Mvi_array_iterator::next()
{
  /* The scan ending before the array is closed is an error */
  if (json_scan_next(m_je))
    return MVI_WALK_JSON_ERROR;

  switch (m_je->state)
  {
    case JST_ARRAY_END:
      m_event_depth= m_depth--;
      /* Trailing junk after the outer array is ignored */
      return m_depth == 0 ? MVI_WALK_END : MVI_NESTED_END;
    case JST_VALUE:
      return read_and_encode_element();
    default:
      /*
        A nested array is opened by read_and_encode_element() or
        Mvi_array_iterator::start. The json_scan_next at the beginning
        of this function would have updated any encountered
        JST_ARRAY_START state to something else
      */
      DBUG_ASSERT(m_je->state != JST_ARRAY_START);
      return MVI_WALK_BAD_FORMAT;
  }
}


/*
  @brief
    Parse the JSON array argument and return a string that will be fed to the
    fulltext index.

  @detail
    The keys are encoded straight into *buf, so there is nothing to copy.
    A separator follows every one of them, including the last, which is
    taken back off at the end: putting it in front of every key but the
    first would leave one behind when an element turns out to have no key.
*/

String *Item_func_mvi_encode::val_str_ascii(String *buf)
{
  String *value= args[0]->val_json(&tmp_js);
  Mvi_array_iterator::Event event;
  DBUG_ASSERT(fixed());
  if ((null_value= !value))
    return nullptr;
  buf->length(0);
  buf->set_charset(&my_charset_latin1_bin);

  Mvi_array_iterator it(&je, value->charset(), m_cast_type.type_handler(),
                        buf);
  for (event= it.start(reinterpret_cast<const uchar *>(value->ptr()),
                       reinterpret_cast<const uchar *>(value->end()));
       !mvi_walk_stopped(event);
       event= it.next())
  {
    /*
      The key is already in place, only the separator is left to add.
      An append that fails is out of memory, and a document that
      silently loses a key could result in false negatives, so give up
      on the row instead.
    */
    if (event == Mvi_array_iterator::MVI_KEY &&
        buf->append(' '))
    {
      null_value= true;
      return nullptr;
    }
  }

  switch (event)
  {
    case Mvi_array_iterator::MVI_WALK_END:
      break;
    case Mvi_array_iterator::MVI_WALK_NOT_ARRAY:
    case Mvi_array_iterator::MVI_WALK_BAD_FORMAT:
      goto error_format;
    default:                    /* MVI_WALK_JSON_ERROR */
      goto json_error;
  }

  /* Take the separator that follows the last key back off */
  if (buf->length())
    buf->length(buf->length() - 1);

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
  @brief
    If `field' is the internal column that holds the keys of a multi-valued
    index, return the mvi_encode() call that computes them.

  @detail
    Only the multi-valued index DDL creates a hidden column computed by
    MVI_ENCODE(), so this identifies one for certain.
*/

static Item_func_mvi_encode *mvi_expr(field_visibility_t invisible,
                                      const Virtual_column_info *vcol_info)
{
  Item *expr;
  if (invisible != INVISIBLE_FULL || !vcol_info ||
      !(expr= vcol_info->expr) ||
      expr->type() != Item::FUNC_ITEM ||
      ((Item_func *) expr)->functype() != Item_func::MVI_ENCODE_FUNC)
    return NULL;
  return (Item_func_mvi_encode *) expr;
}


bool is_mvi_vcol(const Field *field)
{
  return mvi_expr(field->invisible, field->vcol_info) != NULL;
}


/* The same, on the way in: for a column that is being created */
bool is_mvi_vcol(const Create_field *field)
{
  return mvi_expr(field->invisible, field->vcol_info) != NULL;
}


/*
  @brief
    Is key #keyno of `table' a multi-valued index, that is, a fulltext key
    over one internal MVI column?

  @detail
    init_key_part_spec() does not allow such a key to have more than one key
    part. The check is here as well because a table created before it was
    added may still have one, and there is no single expression to show for
    it. The optimizer does use each of its parts, see
    collect_mvi_indexes_for_table().
*/

static Item_func_mvi_encode *mvi_key_expr(const TABLE *table, uint keyno)
{
  KEY *key= table->s->key_info + keyno;
  /* TODO: "legacy" */
  if (!(key->flags & HA_FULLTEXT_legacy) || key->user_defined_key_parts != 1)
    return NULL;
  /*
    Take the field from the TABLE and not from the key part: the share's
    Field objects have no expression, parse_vcol_defs() builds one for each
    TABLE of the share.
  */
  Field *field= table->field[key->key_part[0].fieldnr - 1];
  return mvi_expr(field->invisible, field->vcol_info);
}


bool is_mvi_key(const TABLE *table, uint keyno)
{
  return mvi_key_expr(table, keyno) != NULL;
}


void print_mvi_key_expr(String *str, const TABLE *table, uint keyno)
{
  Item_func_mvi_encode *mvi= mvi_key_expr(table, keyno);
  DBUG_ASSERT(mvi);
  mvi->print_as_array_cast(str);
}


/*
  @brief
    Can an index over an ARRAY be of the type `key' was declared with?

  @detail
    Only a plain KEY can. What the server builds is a fulltext index over
    the encoded elements of the array, which does not implement what any of
    the other types would promise: UNIQUE and PRIMARY KEY would not be
    enforced, and MATCH() against a FULLTEXT one would find nothing. They
    used to be accepted and quietly turned into a plain index.

  @return
    true   No, and an error is raised
*/

static bool check_mvi_key_type(const Key *key)
{
  const char *type= NULL;
  switch (key->type) {
  case Key::PRIMARY:     type= "PRIMARY KEY"; break;
  case Key::UNIQUE:      type= "UNIQUE";      break;
  case Key::FULLTEXT:    type= "FULLTEXT";    break;
  case Key::SPATIAL:     type= "SPATIAL";     break;
  case Key::VECTOR:      type= "VECTOR";      break;
  case Key::MULTIPLE:    /* A plain KEY: the only type an ARRAY can have */
  case Key::FOREIGN_KEY: /* Both of these are built with Key::MULTIPLE, so */
  case Key::IGNORE_KEY:  /* they never reach us under their own name */
    break;
  }
  if (!type)
    return false;
  my_error(ER_WRONG_USAGE, MYF(0), type, "ARRAY");
  return true;
}


/*
  @brief
    Handle a `(CAST(expr AS type ARRAY))' key part: turn the key being
    defined into a multi-valued index over a new internal column.

  @detail
    There is no field to index directly, so the DDL builds one: a hidden
    stored column computed by MVI_ENCODE(), holding the encoded elements of
    the array, and a fulltext index over it. That pairing is what a
    multi-valued index is, see is_mvi_key().

    Both the column and the key are invisible: there is no syntax that would
    name the column, and SHOW CREATE TABLE prints the key with the expression
    it was declared with instead, see print_mvi_key_expr().

  @return
    The key part naming the new column, or NULL if an error was raised
*/

Key_part_spec *add_mvi_key_part(THD *thd, Item *expr,
                                const Lex_cast_type_st &cast_type)
{
  LEX *lex= thd->lex;
  Key *key= lex->last_key;

  /*
    An index over an ARRAY has exactly one key part. Catch a second one here,
    before the type of the key is overwritten below and check_mvi_key_type()
    starts seeing FULLTEXT instead of what the user wrote. A part that comes
    *after* the ARRAY one is caught in init_key_part_spec().
  */
  if (unlikely(key->columns.elements))
  {
    my_error(ER_TOO_MANY_KEY_PARTS, MYF(0), 1);
    return NULL;
  }
  if (unlikely(check_mvi_key_type(key)))
    return NULL;

  /* TODO: check fts_min_token_size is 4, warn if not */
  Create_field *f= new (thd->mem_root) Create_field();
  Item *vcol_expr=
    new (thd->mem_root) Item_func_mvi_encode(thd, expr, cast_type);
  if (unlikely(!f || !vcol_expr))
    return NULL;

  /* Has to run before `f' joins the list it looks for a free name in */
  const Lex_ident_column fname=
    make_internal_field_name(thd, "DB_MVI_", &lex->alter_info.create_list);

  Virtual_column_info *v= add_virtual_expression(thd, vcol_expr);
  if (unlikely(!v))
    return NULL;
  v->set_vcol_type(VCOL_GENERATED_STORED);

  f->invisible= INVISIBLE_FULL;
  f->set_handler(&type_handler_blob);
  f->charset= &my_charset_latin1_bin;
  f->vcol_info= v;
  lex->init_last_field(f, &fname);
  lex->alter_info.create_list.push_back(f, thd->mem_root);

  key->type= Key::FULLTEXT;
  key->invisible= true;

  return new (thd->mem_root) Key_part_spec(&fname, 0, /*gen=*/true);
}


/* Collect all the MVI indexes of `table' */
static
bool collect_mvi_indexes_for_table(THD *thd, TABLE *table,
                                   List<Mv_index> *indexes)
{
  for (uint i=0; i < table->s->keys; i++)
  {
    if (!table->keys_in_use_for_query.is_set(i))
      continue;

    KEY *key= &table->key_info[i];
    /* TODO: "legacy" */
    if (!(key->flags & HA_FULLTEXT_legacy))
      continue;
    for (uint kp=0; kp < key->user_defined_key_parts; kp++)
    {
      Field *field= key->key_part[kp].field;
      if (!is_mvi_vcol(field))
        continue;
      Mv_index *index= new (thd->mem_root) Mv_index(field, i);
      if (indexes->push_back(index))
        return TRUE; // Out of memory
    }
  }
  return FALSE; // Ok
}


/*
  Add one encoded element key to the access.
  
  TODO: String object live on MEM_ROOT and their destructor is never called
  (fix that or switch to something like LEX_STRINGs)
*/

bool Mvi_access::add_key(MEM_ROOT *mem_root, const String *key)
{
  List_iterator<String> it(encoded);
  String *have;
  /*
    A key we already search for adds nothing: '+ka +ka' matches what '+ka'
    matches, and so does 'ka ka'. The lists are a handful of elements, so
    the scan is cheaper than the extra fulltext term would be.
  */
  while ((have= it++))
  {
    if (have->length() == key->length() &&
        !memcmp(have->ptr(), key->ptr(), key->length()))
      return false;
  }

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
    Fold another access on the same index into this one.

  @detail
    collect_mvi_accesses() only takes the top-level AND-parts of the
    condition, so every access it produces has to be true for every row of
    the result. Two conjunctive accesses on one index therefore require the
    union of their keys, and one search for '+ka +kb' finds what two separate
    searches would - more selectively than either, at the price of one.

    Only conjunctive accesses merge. Two disjunctive ones would need
    '(ka kb) (kc kd)' to mean (a OR b) AND (c OR d), which the boolean-mode
    query build_ft_query() puts together has no syntax for. They stay
    separate candidates and get_best_mvi_access() picks between them.
*/

bool Mvi_access::merge(MEM_ROOT *mem_root, Mvi_access *other)
{
  List_iterator<String> it(other->encoded);
  String *key;
  DBUG_ASSERT(can_merge(other));
  /* Merging changes the key set, so it has to happen before we cost it */
  DBUG_ASSERT(records == HA_POS_ERROR);
  while ((key= it++))
  {
    if (add_key(mem_root, key))
      return true;
  }
  return false;
}


/*
  @brief
    Estimate how many records this access will read.

  @detail
    The engine gives us an estimate for one element key at a time (the
    fulltext analogue of records_in_range()). We combine the estimates the
    way the query combines the keys:

    - Disjunctive access (JSON_OVERLAPS) reads the rows of every key, so the
      estimates add up. A key the engine cannot estimate leaves us with no
      idea of what the scan costs, and we cannot leave that key out: dropping
      it from an OR loses the rows that only have that key. Price the access
      out of the plan instead.

    - Conjunctive access (JSON_CONTAINS) reads the rows that have all of the
      keys. The engine estimates one key at a time and cannot intersect them
      for us, so assume the keys are independent:

        rows ~ N * PROD(r_i / N)

      clamped to the rarest key, which is a hard upper bound. The assumption
      under-estimates correlated keys - the elements of a tag array often
      are - but the rarest key alone over-estimates by orders of magnitude
      as soon as the keys are at all selective, and every term we keep in
      the query is a term the engine intersects instead of us fetching the
      row and having the WHERE clause discard it.

      Keys the engine cannot estimate take no part in the estimate but stay
      in the query: a longer AND only narrows the scan, and the JSON
      predicate does the exact filtering either way. If it could not
      estimate a single one of them we know nothing at all, so the access is
      priced out just like a disjunctive one.

    TODO: read_time only accounts for reading the rows, not for the fulltext
    search that produces their rowids.
*/

void Mvi_access::estimate_records()
{
  TABLE *table= index->vcol->table;
  handler *file= table->file;
  List_iterator<String> it(encoded);
  String *key;
  const double n_rows= rows2double(table->stat_records());
  double sum= 0.0, isect= n_rows;
  ha_rows min_rows= HA_POS_ERROR;
  uint n_estimated= 0;
  bool have_unknown_estimate= false;

  while ((key= it++))
  {
    ha_rows rows= file->fulltext_estimate(index->keyno, key->ptr(),
                                          (uint) key->length());
    if (rows == HA_POS_ERROR)
    {
      have_unknown_estimate= true;
      continue;
    }
    n_estimated++;
    sum+= rows2double(rows);
    set_if_smaller(min_rows, rows);
    if (n_rows >= 1.0)
      isect*= rows2double(rows) / n_rows;
  }

  /*
    Fall back to full scan if:
    1. Disjunctive: we have to read all keys, but for at least one, we
       have no idea how many matches it has, OR
    2. Conjunctive: nothing was estimated
  */
  if ((!conjunctive && have_unknown_estimate) ||
      (conjunctive && !n_estimated))
  {
    records= table->stat_records();
    read_time= DBL_MAX;
    return;
  }

  if (conjunctive)
  {
    /* The rows that have all of the keys, see above */
    records= n_rows >= 1.0 ? (ha_rows) isect : (ha_rows) 1;
    set_if_smaller(records, min_rows);
  }
  else
    records= (ha_rows) sum;

  set_if_smaller(records, table->stat_records());
  set_if_bigger(records, (ha_rows) 1);
  read_time= file->cost(file->ha_rnd_pos_call_and_compare_time(records));
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
    Print the index this access uses and the element keys it will search that
    index for into the optimizer trace.

  @detail
    The keys are printed in their encoded form. That is what is stored in the
    index and what we search for, but it is not readable.

    "match" tells whether a row has to have all of the keys (JSON_CONTAINS)
    or just one of them (JSON_OVERLAPS).
*/

void Mvi_access::print_json(THD *thd, Json_writer_object *trace_object)
{
  KEY *key_info= index->vcol->table->key_info + index->keyno;
  List_iterator<String> it(encoded);
  String *key;
  trace_object->add("index", key_info->name).
                 add("match", conjunctive ? "all" : "any");
  if (cost_is_known())
    trace_object->add("rows", records).add("cost", read_time);
  else
    trace_object->add("usable", false).
                  add("cause", "the engine cannot estimate one of the keys");
  Json_writer_array trace_ranges(thd, "ranges");
  while ((key= it++))
    trace_ranges.add(key->ptr(), key->length());
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
    Analyze `cond' and pick the MVI access `tab' will use, if any, and let
    the range analysis see it.

  @param cond  The condition the rows of this table have to satisfy: the
               WHERE clause, or the ON expression when the table is on the
               inner side of an outer join. That is what the range analysis
               of this table uses, too.

  @detail
    The analysis itself is scratch state: what we leave behind is the list of
    accesses in tab->mvi_accesses. They and the Mv_index objects they refer
    to live on the MEM_ROOT, so they outlive `ctx'.

    Accesses on one index that both require all of their keys are merged
    here, see Mvi_access::merge(). What is left is one candidate per index
    and kind, and get_best_mvi_access() prices those and picks one. We put
    no price on anything here: the estimate probes the engine's fulltext
    index, and this runs for every table of the join.

    A fulltext key never gets a bit in const_keys or keys, so we set them
    here. The const_keys bit is what makes the range analysis run for this
    table, where get_best_mvi_access() turns the access into a quick select;
    the keys bit puts the index into EXPLAIN's possible_keys.

  @return
    true   Out of memory
    false  Ok, tab->mvi_accesses is set if the table has any MVI access
*/

bool setup_mvi_access_for_table(THD *thd, JOIN_TAB *tab, Item *cond)
{
  Mvi_context ctx(thd);
  MEM_ROOT *mem_root= thd->mem_root;
  List<Mvi_access> *kept;

  if (!cond)
    return false;
  if (collect_mvi_indexes_for_table(thd, tab->table, &ctx.indexes))
    return true;
  /* Most tables have no MVI. Leave before we walk the condition */
  if (ctx.indexes.is_empty())
    return false;
  if (collect_mvi_accesses(&ctx, cond))
    return true;
  if (ctx.accesses.is_empty())
    return false;

  if (!(kept= new (mem_root) List<Mvi_access>))
    return true;

  List_iterator<Mvi_access> it(ctx.accesses);
  while (Mvi_access *access= it++)
  {
    Mvi_access *into;
    /*
      An access can only be on this table: ctx.indexes holds this table's
      indexes and get_mvi_index() matches the predicate against those.
    */
    DBUG_ASSERT(access->index->vcol->table == tab->table);

    /* Fold it into an access we already keep, if the two are compatible */
    List_iterator<Mvi_access> kit(*kept);
    while ((into= kit++))
    {
      if (into->can_merge(access))
        break;
    }
    if (into)
    {
      if (into->merge(mem_root, access))
        return true;
      continue;
    }

    if (kept->push_back(access, mem_root))
      return true;
    tab->const_keys.set_bit(access->index->keyno);
    tab->keys.set_bit(access->index->keyno);
  }

  tab->mvi_accesses= kept;
  return false;
}


/*
  @brief
    Create a quick select for the MVI access to `tab', if there is one.

  @detail
    The range optimizer cannot produce this access (it skips fulltext keys),
    so the caller creates it here and compares its cost with whatever
    test_quick_select() came up with.

    This is the only place an MVI access is priced. Where a table has more
    than one - accesses on different indexes, which cannot be merged into a
    single fulltext search - the cheapest one wins, on the same read_time
    scale keep_cheaper_quick() then uses against the range access.
*/

QUICK_SELECT_I *get_best_mvi_access(THD *thd, JOIN_TAB *tab)
{
  TABLE *table= tab->table;
  Mvi_access *access, *best= NULL;

  if (!tab->mvi_accesses)
    return NULL;

  List_iterator<Mvi_access> it(*tab->mvi_accesses);
  while ((access= it++))
  {
    /* estimate_records() probes the engine, so do it at most once */
    if (access->records == HA_POS_ERROR)
      access->estimate_records();
    /*
      best_access_path() takes a quick select to be cheaper than a table
      scan without checking (the range optimizer only proposes a quick when
      it is), so an access we could not put a price on is no use to us.
    */
    if (!access->cost_is_known())
      continue;
    if (!best || access->read_time < best->read_time)
      best= access;
  }

  if (unlikely(thd->trace_started()))
  {
    /*
      We are inside the "rows_estimation" array, so we need an object of our
      own before we can add anything by name. Without it the writer hits an
      assertion in Single_line_formatting_helper::on_add_member().
    */
    Json_writer_object trace_wrapper(thd);
    Json_writer_object trace_mvi(thd, "multi_value_index_use");
    trace_mvi.add_table_name(table);
    Json_writer_array trace_candidates(thd, "candidates");
    it.rewind();
    while ((access= it++))
    {
      Json_writer_object trace_one(thd);
      access->print_json(thd, &trace_one);
      if (access == best)
        trace_one.add("chosen", true);
    }
  }

  if (!best)
    return NULL;
  return create_quick_mvi_select(thd, table, best);
}


/****************************************************************************
  QUICK_MVI_SELECT - reading a multi-valued index
****************************************************************************/
struct Mvi_access;

/*
  Quick select that reads a multi-valued index.

  It runs a boolean-mode fulltext search over the index's hidden vcol, looking
  for the encoded element keys of the JSON predicate this access was built
  from. The scan is a necessary, not a sufficient condition: the JSON
  predicate stays in the WHERE clause and does the exact filtering.

  Unlike FT_SELECT, there is no Item_func_match to have created the FT_INFO
  for us, so we create it ourselves in reset() and own it.

  The methods are implemented in opt_multi_valued_index.cc.
*/

class QUICK_MVI_SELECT: public QUICK_SELECT_I
{
  Mvi_access *access;
  FT_INFO *ft_handler;
  StringBuffer<256> query;              /* the boolean-mode ft query */
public:
  QUICK_MVI_SELECT(THD *thd, TABLE *table, Mvi_access *access_arg);
  ~QUICK_MVI_SELECT();
  int init() override { return 0; }
  int reset() override;
  int get_next() override;
  bool reverse_sorted() override { return false; }
  /*
    Fulltext results come back ordered by relevance, not by key, so there is
    no sorted output to offer. QS_TYPE_MVI is not one of the types the
    ORDER BY-by-index code paths consider, so they never ask.
  */
  void need_sorted_output() override {}
  int get_type() override { return QS_TYPE_MVI; }
  void add_keys_and_lengths(String *key_names, String *used_lengths) override;
  void add_used_key_part_to_set() override {}
  Explain_quick_select *get_explain(MEM_ROOT *alloc) override;
#ifndef DBUG_OFF
  void dbug_dump(int indent, bool verbose) override;
#endif
};

static QUICK_SELECT_I *create_quick_mvi_select(THD *thd, TABLE *table, Mvi_access *access)
{
  return new QUICK_MVI_SELECT(thd, table, access);
}

QUICK_MVI_SELECT::QUICK_MVI_SELECT(THD *thd, TABLE *table,
                                   Mvi_access *access_arg)
  : access(access_arg), ft_handler(NULL)
{
  head= table;
  index= access->index->keyno;
  record= head->record[0];
  records= access->records;
  read_time= access->read_time;
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
