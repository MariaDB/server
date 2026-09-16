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

class Alter_info;
class Json_writer_object;
class Key;

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

  /*
    Build: can `other' be folded into this access? Two accesses on the same
    index that both require all of their keys are the same thing as one
    access requiring the union of the keys.
  */
  bool can_merge(const Mvi_access *other) const
  { return index == other->index && conjunctive && other->conjunctive; }

  /* Build: fold `other' into this access. can_merge() must hold */
  bool merge(MEM_ROOT *mem_root, Mvi_access *other);

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
  of setup_mvi_access_for_table(): the accesses that analysis settles on are
  what outlive it.
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
  A key is one fulltext token, so it cannot be longer than the maximum
  token size the engine will index: 84 characters (HA_FT_MAXCHARLEN, which
  is also the default and the maximum of innodb_ft_max_token_size). The key
  is the hex of the key image, so that image is at most half of it. An
  image longer than that is cut down to it, making the key a prefix key:
  see encode_mvi_key().

  Note however myisam and aria uses open upper bound which limits max
  len to 41 instead of 42.

  An engine whose token size limits are narrower than what an index can
  produce cannot hold that index: see mvi_keys_fit_fulltext().
*/
#define MVI_KEY_IMAGE_MAX_LEN 41
#define MVI_ENCODED_KEY_MAX_LEN (MVI_KEY_IMAGE_MAX_LEN * 2)

/*
  The shortest a key can be. An image of one byte, or of none at all, is
  padded out to this, so that the engine's minimum token size -- 3 for
  InnoDB, 4 for MyISAM and Aria, by default -- does not drop it.
*/
#define MVI_ENCODED_KEY_MIN_LEN 4

/*
  What the server puts in MYSQL_FTPARSER_PARAM::ftparser_arg for a
  multi-valued index: what the mvi fulltext parser needs to turn a document
  into the keys of that index, and nothing it has to allocate. The parser
  runs on the engine's threads, while a DML commit or an index build is
  going on, so this is prepared once where the table is opened and lives as
  long as the table does.

  Read-only once prepared, so one of these serves however many threads are
  parsing for the index at the same time. Everything that changes as a
  document is read is on the stack of the parse.
*/

struct Mvi_parser_arg
{
  /* Where the array is inside the document, parsed */
  json_path_t path;
  /* The type the elements are cast to, which is what makes a key a key */
  const Type_handler *cast_th;
};

/*
  Prepare one on `mem_root'. Returns true if `path' does not parse, in which
  case nothing was prepared.
*/
bool mvi_parser_arg_init(MEM_ROOT *mem_root, Mvi_parser_arg *arg,
                         const LEX_CSTRING *path, CHARSET_INFO *path_cs,
                         const Type_handler *cast_th);

/*
  The document half of the mvi fulltext parser: turn the document in
  `param' into the keys of the index `arg' describes and hand each one to
  the server. Lives next to the encoding rather than in the plugin, so that
  everything that decides what a key is stays in one file.
*/
int mvi_tokenize_document(struct st_mysql_ftparser_param *param,
                          Mvi_parser_arg *arg);

/*
  Encode one JSON value into the form it has in the index, appending it to
  `buf'. Returns true if the value cannot be encoded for this index and has
  to be skipped, in which case nothing is appended.
  Shared with opt_mvi_jsonfuncs.cc.
*/
bool encode_mvi_key(json_engine_t *je, const Type_handler *cast_th,
                    CHARSET_INFO *cs, String *buf);

/*
  @brief
    Walk a JSON array, encoding its elements for a multi-valued index of
    the cast_th datatype.

  @detail
    Both sides of the index read the elements this way: MVI_ENCODE, which
    turns them into the fulltext document of a row, and the optimizer,
    which turns them into the keys to search that document for. They agree
    on what the keys of a document are because this is where the keys are
    made.

    An element that is an array is walked too, so that the keys of a nested
    array are the keys of its elements, with MVI_NESTED_START and
    MVI_NESTED_END around them.

    A key is appended to the `key' buffer the iterator was given. The
    caller decides what that buffer is: MVI_ENCODE hands over the document
    it is building, and gets the key encoded into it with nothing to copy
    afterwards; the optimizer hands over a scratch buffer and empties it
    between keys. An element that turns out to have no key leaves the
    buffer as it was.

    Usage:

      Mvi_array_iterator it(je, cs, cast_th, &buf);
      for (event= it.start(str, end); !mvi_walk_stopped(event);
           event= it.next())
      { ... }
*/

class Mvi_array_iterator
{
  json_engine_t * const m_je;
  CHARSET_INFO * const m_cs;
  const Type_handler * const m_cast_th;
  String * const m_key;
  int m_depth;                  /* The array we are in. 1 is the outer one */
  /*
    Same as m_depth except when closing an array - m_event_depth
    remains inside the array while m_depth is outside
  */
  int m_event_depth;
public:

  /*
    What an Mvi_array_iterator stopped at. Everything from MVI_WALK_END on
    is the end of the walk.
  */
  enum Event
  {
    MVI_KEY,              /* An element is read and encoded */
    MVI_NO_KEY,           /* An element that is not: an object, or one that
                             cannot be encoded in the index datatype */
    MVI_NESTED_START,     /* An element that is an array was opened */
    MVI_NESTED_END,       /* ... and closed. depth() is its depth */

    MVI_WALK_END,         /* The array was walked to its end */
    MVI_WALK_NOT_ARRAY,   /* The document is not an array. *je holds the value */
    MVI_WALK_BAD_FORMAT,  /* The document is not a JSON we can make sense of */
    MVI_WALK_JSON_ERROR   /* Malformed JSON. The error is in je->s.error */
  };

  Mvi_array_iterator(json_engine_t *je, CHARSET_INFO *cs,
                     const Type_handler *cast_th, String *key)
   : m_je(je), m_cs(cs), m_cast_th(cast_th), m_key(key),
     m_depth(0), m_event_depth(0) {}

  /* Position on the first element of the array between `start' and `end' */
  Event start(const uchar *start, const uchar *end);

  /* Move on to the next element */
  Event next();

  /*
    The depth of the array the last event is about: the one that was opened
    or closed for MVI_NESTED_START / MVI_NESTED_END, the one the element
    belongs to for MVI_KEY / MVI_NO_KEY.
  */
  int depth() const { return m_event_depth; }
private:
  Event read_and_encode_element();
};

inline bool mvi_walk_stopped(Mvi_array_iterator::Event event)
{ return event >= Mvi_array_iterator::MVI_WALK_END; }

/*
  Will `file' index every key an MVI of the cast_th datatype produces? A
  key the engine drops for being too short or too long is one we would
  search the index for and never find, which makes the index unusable.
*/
bool mvi_keys_fit_fulltext(const handler *file, const Type_handler *cast_th,
                           bool report_error_if_unfit= false);

/*
  DDL: the same as mvi_keys_fit_fulltext, raising
  ER_MVI_KEY_TOKEN_SIZE when they do not fit. Returns true if an error
  was raised.
*/
bool check_mvi_token_size(const handler *file, const Create_field *column);

Item *mvi_desugar_whole_document(THD *thd, Item *column);

/*
  DDL: can the multi-valued index whose keys `column' holds be built from
  the column its expression reads them out of? Returns true if an error was
  raised.
*/
bool check_mvi_base_column(Alter_info *alter_info, const Create_field *column);

/*
  FRM: the definition of key `key' as a multi-valued index, or NULL when it
  is not one. `create_fields' is the columns of the table the key belongs to.
*/
Virtual_column_info *mvi_key_spec(List<Create_field> &create_fields,
                                  const KEY *key);

/*
  DDL: the same, earlier -- off a Key, before the key parts are numbered.
  `create_list' is the columns the statement defines.
*/
Virtual_column_info *mvi_key_decl(List<Create_field> &create_list,
                                  const Key *key);

/*
  DDL: do two such declarations describe the same keys? Neither may be NULL.
*/
bool mvi_decls_eq(Virtual_column_info *a, Virtual_column_info *b);

/*
  Open: check the EXTRA2_MVI_SPEC image of `table' against its keys. Returns
  true if they disagree, which makes the table unopenable.
*/
bool check_mvi_spec(const TABLE *table);

/*
  Optimizer: is `expr' the document that a multi-valued index declared over
  `indexed' holds the keys of? A bare column and that column at path '$'
  are the same document.
*/
bool mvi_same_document(THD* thd, Item *indexed, Item *expr);

/*
  Open: does `table' have a multi-valued index whose keys the engine
  would drop? Answered once, into table->mvi_keys_dropped, because every
  write asks.
*/
void mvi_set_keys_readonly(TABLE *table);

/*
  Write: is `field' the column of a multi-valued index whose keys the
  engine will not hold? Raises ER_MVI_KEY_TOKEN_SIZE when it is.
*/
bool mvi_report_unfit_keys(const TABLE *table, const Field *field);

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
  DDL: name the internal columns of the multi-valued indexes this ALTER
  TABLE adds, which the parser could only name provisionally. Returns true
  if an error was raised.
*/
bool mvi_name_new_vcols(THD *thd, TABLE *table, Alter_info *alter_info);

/*
  Analyze `cond' and pick the MVI access `tab' will use, if any, and let the
  range analysis see it
*/
bool setup_mvi_access_for_table(THD *thd, JOIN_TAB *tab, Item *cond);

/* Create a quick select for the MVI access to `tab', if there is one */
QUICK_SELECT_I *get_best_mvi_access(THD *thd, JOIN_TAB *tab);
