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

class Json_writer_object;

/* An MVI index */
struct Mv_index : public Sql_alloc
{
  Field *vcol;                  /* The hidden vcol of the index */
  uint keyno;                   /* The keyno of the index */
  Mv_index(Field *vcol_arg, uint keyno_arg)
    : vcol(vcol_arg), keyno(keyno_arg) {}
};

/* Access descriptor for a predicate */
struct Mvi_access : public Sql_alloc
{
  Mv_index *index;
  List<String> encoded;         /* encoded element keys */
  bool conjunctive;             /* CONTAINS -> AND, OVERLAPS -> OR */
  /*
    The estimate for this access, produced by estimate_records().
    HA_POS_ERROR means we haven't estimated it yet.
  */
  ha_rows records;
  double read_time;
  Mvi_access(Mv_index *idx, bool conj)
    : index(idx), conjunctive(conj), records(HA_POS_ERROR), read_time(0.0) {}

  /* Build: Add one encoded element key */
  bool add_key(MEM_ROOT *mem_root, const String *key);

  /* Usage: Estimate how many records this access will read */
  void estimate_records();

  /*
    Usage: false when estimate_records() could not put a price on the access.
    Such an access must not be used: we have no idea what it costs.
  */
  bool cost_is_known() const { return read_time != DBL_MAX; }

  /* Usage: Build the fulltext query searching for the element keys */
  bool build_ft_query(String *out);

  /* Usage: describe this access in the optimizer trace */
  void print_json(THD *thd, Json_writer_object *trace_object);
};


/*
  The state of the MVI analysis of one table. It only lives for the duration
  of setup_mvi_access_for_table(): the access that analysis settles on is
  what outlives it.
*/
class Mvi_context : public Sql_alloc
{
 public:
  THD *thd;
  /* The MV indexes of the table */
  List<Mv_index> indexes;
  /* MVI accesses for all eligible predicates on the table */
  List<Mvi_access> accesses;

  Mvi_context(THD *thd_arg) : thd(thd_arg) {}
};

/* Return the compatible json type */
enum json_value_types mvi_json_class(enum_field_types ftype);

/*
  Encode one JSON value into the form it has in the index. Returns true if
  the value cannot be encoded for this index and has to be skipped.
  Shared with opt_mvi_jsonfuncs.cc.
*/
bool encode_mvi_key(json_engine_t *je, const Type_handler *cast_th,
                    CHARSET_INFO *cs, String *buf);

/*
  Is `field' the internal column that holds the keys of a multi-valued index?
*/
bool is_mvi_vcol(const Field *field);
bool is_mvi_vcol(const Create_field *field);

/* Is key #keyno of `table' a multi-valued index? */
bool is_mvi_key(const TABLE *table, uint keyno);

/*
  Print the expression key #keyno was declared with, in the CAST(... ARRAY)
  form, for SHOW CREATE TABLE
*/
void print_mvi_key_expr(String *str, const TABLE *table, uint keyno);

/*
  DDL: handle a `(CAST(expr AS type ARRAY))' key part of the key being
  defined. Returns NULL if an error was raised.
*/
Key_part_spec *add_mvi_key_part(THD *thd, Item *expr,
                                const Lex_cast_type_st &cast_type);

/*
  Analyze `cond' and pick the MVI access `tab' will use, if any, and let the
  range analysis see it
*/
bool setup_mvi_access_for_table(THD *thd, JOIN_TAB *tab, Item *cond);

/* Create a quick select for the MVI access to `tab', if there is one */
QUICK_SELECT_I *get_best_mvi_access(THD *thd, JOIN_TAB *tab);
