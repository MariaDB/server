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
#include "my_json_writer.h"

static QUICK_SELECT_I *create_quick_mvi_select(THD *thd, TABLE *table, Mvi_access *access);

void Item_func_mvi_encode::append_cast_type(String *str)
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

bool encode_mvi_key(json_engine_t *je, const Type_handler *cast_th,
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
    Estimate how many records this access will read, and simplify the access
    if that lets us read fewer.

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
      keys, so the rarest key alone bounds the result. Use its estimate, and
      drop the other keys from the query: reading the rarest key and letting
      the WHERE clause discard the rest is not worse than having the engine
      intersect the terms. This is the trade-off collect_mvi_keys() already
      makes for the keys it cannot encode - a shorter AND matches a superset
      of the rows, and the JSON predicate does the exact filtering.
      Keys the engine cannot estimate take no part in the choice. If it
      could not estimate a single one of them we know nothing at all, so the
      access is priced out just like a disjunctive one.

    TODO: read_time only accounts for reading the rows, not for the fulltext
    search that produces their rowids.
*/

void Mvi_access::estimate_records()
{
  TABLE *table= index->vcol->table;
  handler *file= table->file;
  List_iterator<String> it(encoded);
  String *key, *rarest= NULL;
  ha_rows sum= 0, min_rows= 0;
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
    sum+= rows;
    if (!rarest || rows < min_rows)
    {
      min_rows= rows;
      rarest= key;
    }
  }

  if (!conjunctive && have_unknown_estimate)
  {
    /* 
      Disjunctive means we have to read all keys. For at least one, we have no idea
      how many matches it has.  Fall back to full scan.
    */
    records= table->stat_records();
    read_time= DBL_MAX;
    return;
  }
  if (conjunctive && !rarest)
  {
    /* Nothing was estimated. Fall back to full table scan */
    records= table->stat_records();
    read_time= DBL_MAX;
    return;
  }

  if (conjunctive)
  {
    /* Search for the rarest key only */
    it.rewind();
    while ((key= it++))
    {
      if (key != rarest)
        it.remove();
    }
    records= min_rows;
  }
  else
    records= sum;

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
    The analysis itself is scratch state: what we leave behind is the one
    access we've settled on, in tab->mvi_access. It and the Mv_index it
    refers to live on the MEM_ROOT, so they outlive `ctx'.

    A fulltext key never gets a bit in const_keys or keys, so we set them
    here. The const_keys bit is what makes the range analysis run for this
    table, where get_best_mvi_access() turns the access into a quick select;
    the keys bit puts the index into EXPLAIN's possible_keys.

  @return
    true   Out of memory
    false  Ok, tab->mvi_access is set if the table has an MVI access
*/

bool setup_mvi_access_for_table(THD *thd, JOIN_TAB *tab, Item *cond)
{
  Mvi_context ctx(thd);
  Mvi_access *best= NULL;
  if (!cond)
    return false;
  if (collect_mvi_indexes_for_table(thd, tab->table, &ctx.indexes))
    return true;
  /* Most tables have no MVI. Leave before we walk the condition */
  if (ctx.indexes.is_empty())
    return false;
  if (collect_mvi_accesses(&ctx, cond))
    return true;

  List_iterator<Mvi_access> it(ctx.accesses);
  /* TODO: cost based */
  /*
    TODO: merge

    json_contains(j->'$.tags','"a"') and
    json_contains(j->'$.tags','"b"')

    (+ta +tb)
  */
  while (Mvi_access *access= it++)
  {
    /*
      An access can only be on this table: ctx.indexes holds this table's
      indexes and get_mvi_index() matches the predicate against those.
    */
    DBUG_ASSERT(access->index->vcol->table == tab->table);
    best= access;
  }
  if (!best)
    return false;

  tab->mvi_access= best;
  tab->const_keys.set_bit(best->index->keyno);
  tab->keys.set_bit(best->index->keyno);
  return false;
}


/*
  @brief
    Create a quick select for the MVI access to `tab', if there is one.

  @detail
    The range optimizer cannot produce this access (it skips fulltext keys),
    so the caller creates it here and compares its cost with whatever
    test_quick_select() came up with.
*/

QUICK_SELECT_I *get_best_mvi_access(THD *thd, JOIN_TAB *tab)
{
  TABLE *table= tab->table;
  Mvi_access *access= tab->mvi_access;
  if (!access)
    return NULL;
  /*
    estimate_records() drops element keys from the access, so it must run
    only once even if we are called again for the same table.
  */
  if (access->records == HA_POS_ERROR)
    access->estimate_records();
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
    access->print_json(thd, &trace_mvi);
  }
  /*
    best_access_path() takes a quick select to be cheaper than a table scan
    without checking (the range optimizer only proposes a quick when it is),
    so an access we could not put a price on has to be dropped here.
  */
  if (!access->cost_is_known())
    return NULL;
  return create_quick_mvi_select(thd, table, access);
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
