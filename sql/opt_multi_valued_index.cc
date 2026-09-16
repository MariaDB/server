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
#include "sql_table.h"
#include "unireg.h"                  /* extra2_read_len, extra2_write_len */
#include "item_func.h"
#include "item_jsonfunc.h"                    /* report_path_error_ex */
#include "sql_show.h"                         /* append_identifier */
#include "my_json_writer.h"
#include <mysql/plugin_ftparser.h>   /* MYSQL_FTPARSER_PARAM */

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
    The type handler a stored cast means, or NULL when it is not a cast a
    multi-valued index can be made of.

  @detail
    The one place that says which datatypes an index can be of, asked by the
    DDL of what the statement wrote and by the FRM reader of what the
    section holds. Whatever encode_mvi_key() grows -- DECIMAL and the
    temporal types are its standing TODO -- is a case here, and costs
    nothing on disk: the cast is stored the way a column's datatype is.

    A handler is returned rather than just "yes", so that the caller can
    check the datatype it has *is* this one and not merely something with
    the same field_type(): a user-defined type over a string would pass a
    field_type() test and then be encoded as the string it is not.
*/

const Type_handler *mvi_cast_handler(enum_field_types type, bool is_unsigned)
{
  switch (type) {
  case MYSQL_TYPE_LONG_BLOB:                    /* CHAR, BINARY, VARCHAR */
    return is_unsigned ? NULL : &type_handler_long_blob;
  case MYSQL_TYPE_LONGLONG:                     /* INT, SIGNED, UNSIGNED */
    if (is_unsigned)
      return &type_handler_ulonglong;
    else
      return &type_handler_slonglong;
  default:
    return NULL;
  }
}


const Type_handler *Mvi_decl::cast_type_handler() const
{
  const Type_handler *th= mvi_cast_handler(cast_type, cast_unsigned);
  /* Neither the DDL nor the FRM reader lets an unsupported cast through */
  DBUG_ASSERT(th);
  return th;
}


void Mvi_decl::append_cast_type(String *str) const
{
  char buf[32];
  size_t length;
  switch (cast_type) {
  case MYSQL_TYPE_LONG_BLOB:
    str->append(STRING_WITH_LEN("char("));
    length= (size_t) (longlong10_to_str(cast_length, buf, -10) - buf);
    str->append(buf, length);
    str->append(')');
    return;
  case MYSQL_TYPE_LONGLONG:
    if (cast_unsigned)
      str->append(STRING_WITH_LEN("unsigned"));
    else
      str->append(STRING_WITH_LEN("int"));
    return;
  default:
    break;
  }
  /*
    Whatever the encoding grows: the datatype's own name, with the length
    and the scale it was given. The same shape CAST() takes them in, so it
    reads back in, and nothing to write here when that day comes.
  */
  {
    const Name name= cast_type_handler()->name();
    str->append(name.ptr(), name.length());
    if (cast_length)
    {
      str->append('(');
      length= (size_t) (longlong10_to_str(cast_length, buf, -10) - buf);
      str->append(buf, length);
      if (cast_dec)
      {
        str->append(',');
        length= (size_t) (longlong10_to_str(cast_dec, buf, -10) - buf);
        str->append(buf, length);
      }
      str->append(')');
    }
  }
}


/*
  @brief
    Print the index expression the way it was written:

      CAST(<column> -> '<path>' AS <type> ARRAY)

  @detail
    Built by hand, because there is no expression to print: what the FRM
    keeps is the declaration, and this is the SQL that means it. The forms
    below are the ones a table definition is printed in elsewhere -- the
    column as an identifier, the path as a string literal in the charset
    Item_string::print() would use -- so that what SHOW CREATE TABLE
    prints parses straight back into the same declaration.
*/

void Mvi_decl::print(THD *thd, String *str, const Lex_ident_column &base) const
{
  String path_str(path.str, path.length, mvi_path_charset());
  str->append(STRING_WITH_LEN("cast(json_extract("));
  append_identifier(thd, str, base.str, base.length);
  str->append(STRING_WITH_LEN(",'"));
  path_str.print(str, &my_charset_utf8mb4_general_ci);
  str->append(STRING_WITH_LEN("') as "));
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
    predicate is rechecked on every row the index produces.

    The collation is not folded into the image, and trailing spaces are not
    trimmed out of it, because the predicates this index answers do not ask
    the collation about anything. json_string_compare(), which is what
    decides whether a document element and a value searched for are the
    same string, is a memcmp when neither side is escaped and a comparison
    of decoded characters when one is; it takes a CHARSET_INFO only to
    decode. So two elements are the same value when their characters are
    the same and not otherwise, and that is what the image has to
    distinguish.

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
      /*
        The element's own bytes are the image: point at them instead of
        copying. Nothing may write through `sorted' from here on -- the
        bytes belong to the caller, and on the write path they are the row
        InnoDB is about to store, see mvi_tokenize_document() and
        fts_fetch_doc_from_rec().
      */
      sorted.set((char *) je->value, je->value_len, &my_charset_latin1_bin);
      break;
    default:
      return true;
  }

  /* 2. cut what the engine would not index down to what it will, see above */
  if (sorted.length() > MVI_KEY_IMAGE_MAX_LEN)
    sorted.length(MVI_KEY_IMAGE_MAX_LEN);

  /*
    3. hex. ptr() and not c_ptr(): the latter NUL-terminates in place when
    the buffer has room past the string, and cutting the image down in step
    2 leaves exactly that -- so it would write a NUL over the first byte
    the image does not cover, which for a string is the document's own.
  */
  buf->append_hex(sorted.ptr(), sorted.length());

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
    Prepare the argument the mvi fulltext parser is handed for one
    multi-valued index.

  @detail
    The path is parsed here, once, because the parser cannot allocate: it
    runs on the engine's threads while a commit or an index build is going
    on. json_find_path() does not write to the path -- it walks it with a
    cursor and counters the caller owns -- so what comes out of here is
    read-only and one of these serves every parse of the index.

  @return
    true   The path does not parse, and nothing was prepared
*/

bool mvi_parser_arg_init(MEM_ROOT *mem_root, Mvi_parser_arg *arg,
                         const LEX_CSTRING *path, CHARSET_INFO *path_cs,
                         const Type_handler *cast_th)
{
  bzero(arg, sizeof(*arg));
  mem_root_dynamic_array_init(mem_root, PSI_INSTRUMENT_MEM, &arg->path.steps,
                              sizeof(json_path_step_t), NULL,
                              JSON_DEPTH_DEFAULT, JSON_DEPTH_INC, MYF(0));
  if (json_path_setup(&arg->path, path_cs, (const uchar *) path->str,
                      (const uchar *) path->str + path->length))
    return true;
  arg->cast_th= cast_th;
  return false;
}


/*
  @brief
    The document half of the mvi fulltext parser: the keys of one
    document.

  @detail
    The document is the value of the column the index is over, so the array
    to index is somewhere inside it and the path says where. Once found, its
    elements are walked and encoded by the same iterator MVI_ENCODE uses,
    which is what the iterator is for: the keys of a document are made in
    one place, so the write side and the query side cannot come to different
    conclusions about what they are.

    A document with no array at that path simply has no keys. Neither has
    one that is not JSON at all, or an array whose elements cannot be
    encoded in this index's datatype. None of that is an error here: a row
    with no key in the index is a row the index cannot be used to find,
    which the query side already has to allow for, see collect_mvi_keys().

    Everything that changes while the document is read is on the stack, so
    that two threads parsing for the same index share nothing but the
    read-only *arg. Both dynamic arrays here are buffered on the stack and
    can never have to grow, because json_lib refuses to scan deeper than
    JSON_DEPTH_LIMIT and json_path_setup() refuses a path with that many
    steps, which is what the two are indexed by. Hence the NULL MEM_ROOT:
    growing them would be a bug, not an allocation.

  @return
    0, except when the server refuses a word
*/

int mvi_tokenize_document(MYSQL_FTPARSER_PARAM *param, Mvi_parser_arg *arg)
{
  MYSQL_FTPARSER_BOOLEAN_INFO bool_info=
    { FT_TOKEN_WORD, 0, 0, 0, 0, ' ', 0 };
  json_engine_t je;
  json_path_step_t *cur_step;
  int je_stack_buffer[JSON_DEPTH_LIMIT];
  int array_counters_buffer[JSON_DEPTH_LIMIT];
  MEM_ROOT_DYNAMIC_ARRAY array_counters;
  const uchar *doc= (const uchar *) param->doc;
  const uchar *array_start, *array_end;
  /*
    The charset of the column, which json_lib and the encoding both want
    without the const the ftparser interface hands it over with.
  */
  CHARSET_INFO *cs= const_cast<CHARSET_INFO *>(param->cs);
  StringBuffer<MVI_ENCODED_KEY_MAX_LEN> key;
  Mvi_array_iterator::Event event;

  key.set_charset(&my_charset_latin1_bin);
  /*
    One key at a time is built in `key' and the buffer is reused for the
    next one, so nothing the server is handed here outlives the
    mysql_add_word() call it is handed to. MyISAM and Aria build the index
    from a tree of pointers into the document, which stays put for them,
    and only copy a word when asked -- so ask, or the index fills up with
    whatever is left on this stack frame. InnoDB copies either way.
  */
  param->flags|= MYSQL_FTFLAGS_NEED_COPY;
  initJsonArray(NULL, &je.stack, sizeof(int), je_stack_buffer, 0);
  initJsonArray(NULL, &array_counters, sizeof(int), array_counters_buffer, 0);

  if (json_scan_start(&je, cs, doc, doc + param->length))
    return 0;

  /* json_find_path() moves this cursor along, so it starts at the front */
  cur_step= (json_path_step_t *) arg->path.steps.buffer;
  if (json_find_path(&je, &arg->path, &cur_step, &array_counters) ||
      json_read_value(&je) ||
      je.value_type != JSON_VALUE_ARRAY)
    return 0;                                   /* No array there */

  /* The text of the array, from its '[' to just past its ']' */
  array_start= je.value;
  if (json_skip_level(&je))
    return 0;
  array_end= je.s.c_str;

  Mvi_array_iterator it(&je, cs, arg->cast_th, &key);
  for (event= it.start(array_start, array_end);
       !mvi_walk_stopped(event);
       event= it.next())
  {
    if (event != Mvi_array_iterator::MVI_KEY)
      continue;
    if (param->mysql_add_word(param, key.ptr(), (int) key.length(),
                              &bool_info))
      return 1;
    /* The iterator appends, so empty the buffer before the next key */
    key.length(0);
  }
  return 0;
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
  /* TODO: length can be wrong when padding happens */
  fix_length_and_charset(args[0]->max_char_length() * 2,
                         &my_charset_latin1_bin);
  set_maybe_null();
  return false;
}


/*
  @brief
    Build the EXTRA2_MVI_SPEC image: what each multi-valued index of the
    table was declared with.

  @detail
    A version, the number of entries, and then one entry per multi-valued
    index. Sparse rather than one entry per key, unlike EXTRA2_INDEX_FLAGS:
    most tables have no multi-valued index at all, and an empty image means
    the section is not written.

      version       1 byte, MVI_SPEC_VERSION
      entries       1 or 3 bytes, see extra2_write_len()
      then per entry:
        length      1 or 3 bytes: the bytes of the entry that follow
        keyno       1 byte
        tag         1 byte, Mvi_decl::Tag: what shape the rest has

      tag ARRAY_AT_PATH:
        cast type   1 byte, enum_field_types, as a column's datatype is
        cast flags  1 byte, 1 = unsigned
        cast length 4 bytes
        cast dec    1 byte
        path length 1 or 3 bytes
        path        `path length' bytes, in mvi_path_charset()

    The entry length is what makes the section walkable by a reader that
    does not understand every entry in it, and the tag is where a
    differently shaped declaration goes -- see Mvi_decl::Tag. Neither is a
    licence to skip: a reader that finds bytes it cannot account for
    refuses the table, because a declaration it half understands would
    build and search a different index than the FRM describes. What changes
    what the keys are bumps MVI_SPEC_VERSION.

    The base column is not in here. The key part of a multi-valued index is
    that column, so the key definition already says which one it is -- which
    is also why renaming it needs nothing of us.

    Read back by mvi_read_specs().

  @return
    true if an error was raised
*/

bool mvi_spec_image(String *image, uint keys, const KEY *key_info)
{
  /* Write no image at all, to test that opening the table refuses it */
  DBUG_EXECUTE_IF("mvi_skip_spec_image", return false;);

  /* extra2_write_len() leaves the marker byte of a long length alone */
  uchar len_buf[3]= { 0, 0, 0 };
  uchar head[MVI_SPEC_ENTRY_HEAD_LEN];
  uint n_specs= 0;
  size_t len;

  for (uint i= 0; i < keys; i++)
    if (key_info[i].mvi_decl)
      n_specs++;
  if (!n_specs)
    return false;

  len= (size_t) (extra2_write_len(len_buf, n_specs) - len_buf);
  if (image->append((char) MVI_SPEC_VERSION) ||
      image->append((char*) len_buf, len))
    return true;                                // Out of memory

  for (uint i= 0; i < keys; i++)
  {
    const Mvi_decl *decl= key_info[i].mvi_decl;
    size_t entry_len;
    if (!decl)
      continue;
    DBUG_ASSERT(i <= 0xFF);                     /* MAX_KEY is 64 */
    /* Never empty, and short enough for the section, see MVI_PATH_MAX_LEN */
    DBUG_ASSERT(decl->path.length &&
                decl->path.length <= MVI_PATH_MAX_LEN);

    head[0]= (uchar) i;
    head[1]= (uchar) Mvi_decl::ARRAY_AT_PATH;
    head[2]= (uchar) decl->cast_type;
    head[3]= decl->cast_unsigned ? MVI_CAST_UNSIGNED : 0;
    int4store(head + 4, decl->cast_length);
    head[8]= decl->cast_dec;

    entry_len= sizeof(head) + extra2_str_size(decl->path.length);
    len_buf[0]= len_buf[1]= len_buf[2]= 0;
    len= (size_t) (extra2_write_len(len_buf, entry_len) - len_buf);
    if (image->append((char*) len_buf, len) ||
        image->append((char*) head, sizeof(head)))
      return true;                              // Out of memory

    len_buf[0]= len_buf[1]= len_buf[2]= 0;
    len= (size_t) (extra2_write_len(len_buf, decl->path.length) - len_buf);
    if (image->append((char*) len_buf, len) ||
        image->append(decl->path.str, decl->path.length))
      return true;                              // Out of memory
  }
  return false;
}


/*
  @brief
    Read the FRM's EXTRA2_MVI_SPEC section. See mvi_spec_image() for the
    bytes.

  @detail
    All of it lands on the share, because a declaration is neither an Item
    nor a Field and does not depend on a THD: the key keeps it for as long
    as the share lives, and so does the argument built here for the key's
    fulltext parser, which the engine's threads read while a commit or an
    index build is going on.

    An entry this cannot account for to the last byte is an FRM this server
    did not write, or one a newer server did: a version, a tag or a cast it
    does not know, a key of the wrong shape, or an entry that does not end
    where its length says. So is a key of the right shape with no entry at
    all: such a key names the mvi fulltext parser, which no table
    definition may, see check_mvi_key_parser(). Refuse the table either
    way, rather than open it as the plain fulltext key it would otherwise
    look like.

  @return
    true   The table cannot be opened
*/

bool mvi_read_specs(TABLE_SHARE *share, const LEX_CUSTRING *section)
{
  const uchar *pos= section->str;
  const uchar *end= pos + section->length;

  if (section->length)
  {
    size_t n_specs;

    if (pos >= end || *pos++ != MVI_SPEC_VERSION)
      return true;
    n_specs= extra2_read_len(&pos, end);
    for (size_t i= 0; i < n_specs; i++)
    {
      Mvi_decl *decl;
      KEY *key;
      const uchar *entry_end;
      size_t entry_len, path_len;
      uint keyno;
      uchar cast_flags;

      entry_len= extra2_read_len(&pos, end);
      if (entry_len < MVI_SPEC_ENTRY_HEAD_LEN || pos + entry_len > end)
        return true;
      entry_end= pos + entry_len;

      keyno= *pos++;
      if (keyno >= share->total_keys || !mvi_key_names_parser(share, keyno))
        return true;
      key= share->key_info + keyno;
      if (key->mvi_decl)                        /* Described twice */
        return true;
      if (*pos++ != Mvi_decl::ARRAY_AT_PATH)
        return true;

      if (!(decl= new (&share->mem_root) Mvi_decl()))
        return true;                            // Out of memory
      decl->cast_type= (enum_field_types) *pos++;
      cast_flags= *pos++;
      if (cast_flags & ~MVI_CAST_UNSIGNED)
        return true;
      decl->cast_unsigned= (cast_flags & MVI_CAST_UNSIGNED) != 0;
      decl->cast_length= uint4korr(pos);
      pos+= 4;
      decl->cast_dec= *pos++;
      if (!mvi_cast_handler(decl->cast_type, decl->cast_unsigned))
        return true;

      /* The path, never empty: a bare base column is kept as '$' */
      path_len= extra2_read_len(&pos, entry_end);
      if (!path_len || pos + path_len > entry_end)
        return true;
      if (!(decl->path.str= strmake_root(&share->mem_root,
                                         (const char *) pos, path_len)))
        return true;                            // Out of memory
      decl->path.length= path_len;
      pos+= path_len;

      /* Nothing in the entry this version of the section does not know */
      if (pos != entry_end)
        return true;

      if (!(key->ftparser_arg= mvi_make_parser_arg(&share->mem_root, decl)))
        return true;
      key->mvi_decl= decl;
      share->mvi_keys.set_bit(keyno);
    }
    if (pos != end)
      return true;
  }

  /* A key that names the parser and was not described by any entry */
  for (uint keyno= 0; keyno < share->total_keys; keyno++)
    if (mvi_key_names_parser(share, keyno) && !share->key_info[keyno].mvi_decl)
      return true;
  return false;
}


/*
  @brief
    The range of key lengths, in characters, an MVI of the cast_th
    datatype can produce.

  @detail
    The other half of mvi_cast_handler(): that says which datatypes an
    index can be of, this says how long their keys are. A datatype
    encode_mvi_key() grows needs a case in both.

  @return
    true  It produces no keys at all, and *min_chars and *max_chars are
          untouched. encode_mvi_key() has no image for the datatype, so
          nothing is ever stored or searched for -- which mvi_cast_handler()
          refuses, so nothing a declaration names reaches this.
*/

static bool mvi_key_length_range(const Type_handler *cast_th,
                                 uint *min_chars, uint *max_chars)
{
  switch (cast_th->field_type())
  {
    case MYSQL_TYPE_LONGLONG:
      /* Always the 8 byte image of the integer, in hex */
      *min_chars= *max_chars= 8 * 2;
      return false;
    case MYSQL_TYPE_LONG_BLOB:
      /* Anything from the empty string to a key image that is cut short */
      *min_chars= MVI_ENCODED_KEY_MIN_LEN;
      *max_chars= MVI_ENCODED_KEY_MAX_LEN;
      return false;
    default:
      return true;
  }
}


/*
  @brief
    Does `file' hold every key an MVI of the cast_th datatype produces?

  @detail
    A key outside the engine's token size limits is dropped, and dropped
    silently, when the row is written and when the index is built. It is
    then a key the query side would search for and not find, so a row is
    lost -- the one thing the index must never do. An index that can
    produce such a key is refused at DDL time and ignored by the optimizer
    if it is there anyway, which it can be: the limits are settings, and a
    server can be restarted with different ones, or the table copied to a
    server that has them.

    An engine that does not report its limits is taken at its word.

  @param cast_th  The datatype of the ARRAY cast, which is what decides
                  how long the keys are, see mvi_key_length_range()
*/

bool mvi_keys_fit_fulltext(const handler *file, const Type_handler *cast_th,
                           bool report_error_if_unfit)
{
  uint key_min, key_max, ft_min, ft_max;
  bool fit= mvi_key_length_range(cast_th, &key_min, &key_max) ||
            file->fulltext_token_size_limits(&ft_min, &ft_max) ||
            (key_min >= ft_min && key_max <= ft_max);
  if (!fit && report_error_if_unfit)
    my_error(ER_MVI_KEY_TOKEN_SIZE, MYF(0), key_min, key_max,
             file->table_type(), ft_min, ft_max);
  return fit;
}


bool check_mvi_token_size(const handler *file, const Mvi_decl *decl)
{
  return !mvi_keys_fit_fulltext(file, decl->cast_type_handler(),
                                /*report_error_if_unfit=*/true);
}


/*
  @brief
    Refuse a write that a multi-valued index of `table' cannot hold the keys
    of.

  @detail
    The engine drops a key outside its fulltext token size limits as the row
    is written, not as it is searched for, so the row would be missing from
    the index for good: once the settings are wide again the optimizer uses
    the index and does not find that row. A corrupted index, in other words,
    and the one thing a multi-valued index must never be. Refuse the write
    instead and leave the table readable, so that the index can be dropped
    or the settings put back.

    Which indexes are affected is asked here and not once when the table is
    opened, so that this and the optimizer, which leaves such an index
    unused, read the limits in the same place at the same time and cannot
    come to different conclusions -- see collect_mvi_indexes_for_table().
    It costs a walk of the multi-valued indexes of the table, and only of a
    table that has one.

    What the walk decides is whether the statement writes the document any
    of them reads: an UPDATE that leaves the base column alone gives the
    engine no reason to re-read it, so no entry is rewritten and none can go
    missing. A DELETE only removes entries, which cannot corrupt anything,
    and is not asked at all.

  @return
    true   The write must not happen, and an error is raised
*/

bool mvi_report_unfit_write(TABLE *table, bool is_update)
{
  DBUG_ASSERT(!table->s->mvi_keys.is_clear_all());
  /* Only a key the engine has, so s->keys and not s->total_keys */
  for (uint keyno= 0; keyno < table->s->keys; keyno++)
  {
    const KEY *key= table->key_info + keyno;
    if (!key->mvi_decl)
      continue;
    /* The key part of a multi-valued index is the base column itself */
    if (is_update &&
        !bitmap_is_set(table->write_set, key->key_part[0].fieldnr - 1))
      continue;
    if (!mvi_keys_fit_fulltext(table->file,
                               key->mvi_decl->cast_type_handler(),
                               /*report_error_if_unfit=*/true))
      return true;
  }
  return false;
}


/*
  @brief
    Could key #keyno of `share' be a multi-valued index?

  @detail
    A multi-valued index is a fulltext key over one stored column, parsed
    by the mvi fulltext parser. Nothing else this server writes names that
    parser, and no table definition may either, see check_mvi_key_parser().

    That is the whole shape of one. What array it is over and what its
    elements are encoded as is in EXTRA2_MVI_SPEC, and a key of this shape
    with nothing there is a key whose declaration went missing -- which is
    what makes the section checkable at all, now that the keys no longer
    describe themselves.
*/

bool mvi_key_names_parser(const TABLE_SHARE *share, uint keyno)
{
  DBUG_ASSERT(keyno < share->total_keys);
  const KEY *key= share->key_info + keyno;
  /* TODO: "legacy" */
  if (!(key->flags & HA_FULLTEXT_legacy) ||
      key->user_defined_key_parts != 1 ||
      !(key->flags & HA_USES_PARSER) || !key->parser)
    return false;
  return Lex_ident_column(*plugin_name(key->parser)).
           streq(Lex_cstring_strlen(MVI_PARSER_NAME));
}


/*
  @brief
    The declaration of key #keyno of `table', or NULL when it is not a
    multi-valued index.

  @detail
    What mvi_read_specs() read out of the FRM, or what the DDL put there.
    Nothing else identifies such a key: the key definition itself only says
    it is a fulltext key over a column, which it shares with a plain
    fulltext key over the very same column.
*/

const Mvi_decl *mvi_key_decl(const TABLE *table, uint keyno)
{
  DBUG_ASSERT(keyno < table->s->total_keys);
  return table->key_info[keyno].mvi_decl;
}


void print_mvi_key_expr(THD *thd, String *str, const TABLE_SHARE *share,
                        uint keyno)
{
  const KEY *key= share->key_info + keyno;
  DBUG_ASSERT(key->mvi_decl);
  /* The key part is the base column, see add_mvi_key_part() */
  key->mvi_decl->print(thd, str,
                       share->field[key->key_part[0].fieldnr - 1]->field_name);
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
    The base column of a `<column> -> '<path>'' expression, or NULL when
    `expr' is not of that form.

  @detail
    That form is all a multi-valued index can be declared over. It is
    narrower than what the expression machinery could evaluate, on purpose:
    an index of it *is* (column, path, cast type), which is all the FRM
    keeps of it, see Mvi_decl. The keys of a row then come from the bytes
    of one column, which is what lets them be produced by reading that
    column and looking inside it, rather than by evaluating an expression
    the server has to materialise first.

    The parser builds the same Item_func_json_extract for the `->' operator
    and for a written-out json_extract(), so what SHOW CREATE TABLE prints
    feeds straight back in.
*/

static Item_field *mvi_base_column(Item *expr)
{
  if (expr->type() != Item::FUNC_ITEM)
    return NULL;
  Item_func *func= (Item_func *) expr;
  /* json_extract() takes a path per key; an index is over exactly one */
  if (func->functype() != Item_func::JSON_EXTRACT_FUNC ||
      func->argument_count() != 2)
    return NULL;
  Item **args= func->arguments();
  if (args[0]->type() != Item::FIELD_ITEM || !args[1]->basic_const_item())
    return NULL;
  return (Item_field *) args[0];
}

/*
  @brief
    DDL: check the column can be used for a multi-valued index

  @detail
    The base column -- the one the array is read out of -- has to be
    stored, because it is the column the key part is over. The engine reads
    it out of the clustered index record to build the document it tokenizes,
    and a virtual column has no place there: InnoDB refuses a FULLTEXT index
    over one outright. Refuse it at the multi-valued index instead, with an
    error that says why.

  @return
    true   The index cannot be built from it, and an error is raised
*/

bool check_mvi_base_column(const Create_field &column)
{
  if (likely(column.stored_in_db()))
    return false;
  my_error(ER_MVI_BAD_BASE_COLUMN, MYF(0), column.field_name.str);
  return true;
}


/*
  @brief
    Build what the mvi fulltext parser is handed for the index `decl'
    declares.

  @detail
    The parser needs the path to the array and the datatype the elements
    are cast to, and needs them without having to allocate or evaluate
    anything: it runs on the engine's threads while a commit or an index
    build is going on. Both are in the declaration, so all that is left to
    do here is parse the path, once.

  @return
    The argument, allocated on `mem_root', or NULL if the path does not
    parse -- which a declaration this server wrote never does, see
    mvi_decl_from_key_part() -- or on out of memory
*/

Mvi_parser_arg *mvi_make_parser_arg(MEM_ROOT *mem_root, const Mvi_decl *decl)
{
  Mvi_parser_arg *arg=
    (Mvi_parser_arg *) alloc_root(mem_root, sizeof(*arg));
  if (!arg)
    return NULL;                                // Out of memory
  if (mvi_parser_arg_init(mem_root, arg, &decl->path, mvi_path_charset(),
                          decl->cast_type_handler()))
    return NULL;
  return arg;
}


/*
  @brief
    Record in `decl' the cast a `CAST(... AS <type> ARRAY)' key part asks
    for, if it is one a multi-valued index can be made of.

  @detail
    The handler has to be the one mvi_cast_handler() names for the datatype
    it reports, and not merely something that reports the same one: a
    user-defined type over a string would pass a field_type() test and then
    be encoded as the string it is not.

  @return
    true   Not a cast an index can be made of, and an error is raised
*/

static bool mvi_decl_set_cast(const Lex_cast_type_st &cast_type,
                              Mvi_decl *decl)
{
  const Type_handler *th= cast_type.type_handler();
  const enum_field_types type= th->field_type();
  const bool is_unsigned= th->is_unsigned();

  if (unlikely(mvi_cast_handler(type, is_unsigned) != th))
  {
    /*
      encode_mvi_key() has no key image for it, so such an index would hold
      no keys and find no rows. It used to be accepted and be exactly that.
    */
    my_error(ER_WRONG_USAGE, MYF(0), th->name().ptr(), "ARRAY");
    return true;
  }
  decl->cast_type= type;
  decl->cast_unsigned= is_unsigned;
  decl->cast_length= cast_type.length();
  decl->cast_dec= cast_type.dec();
  return false;
}


/*
  @brief
    Build the declaration a `CAST(<expr> AS <type> ARRAY)' key part means,
    and say which column it is over.

  @detail
    <expr> is `<column> -> '<path>'', see mvi_base_column() -- or a bare
    column, which is the whole document and is kept as the path '$', so
    that there is one form from here on: in the FRM, in what SHOW CREATE
    TABLE prints, and on the query side.

    The path is parsed here, once, rather than left for the fulltext parser
    to trip over on the first row: json_path_setup() is what that parser
    walks the document with, so a path it does not accept is a key
    definition that could never produce a key.

  @return
    The declaration, allocated on thd->mem_root, or NULL if an error was
    raised. *base is the column it is over.
*/

static Mvi_decl *mvi_decl_from_key_part(THD *thd, Item *expr,
                                        const Lex_cast_type_st &cast_type,
                                        Item_field **base)
{
  StringBuffer<MAX_FIELD_WIDTH> raw, path;
  /* A bare column is the whole document, which is the path '$' */
  LEX_CSTRING given= { "$", 1 };
  CHARSET_INFO *given_cs= mvi_path_charset();
  Mvi_decl *decl;
  json_path_t jp;
  json_path_step_t step_buffer[JSON_DEPTH_LIMIT];
  uint errors;

  if (unlikely(!(decl= new (thd->mem_root) Mvi_decl())))
    return NULL;                                // Out of memory
  if (unlikely(mvi_decl_set_cast(cast_type, decl)))
    return NULL;

  if (expr->type() == Item::FIELD_ITEM)
    *base= (Item_field *) expr;
  else
  {
    String *str;
    if (unlikely(!(*base= mvi_base_column(expr))))
    {
      my_error(ER_MVI_BAD_EXPR, MYF(0));
      return NULL;
    }
    /* mvi_base_column() has checked it is a literal, so this evaluates */
    if (unlikely(!(str= ((Item_func *) expr)->arguments()[1]->val_str(&raw))))
    {
      my_error(ER_MVI_BAD_EXPR, MYF(0));
      return NULL;
    }
    given.str= str->ptr();
    given.length= str->length();
    given_cs= str->charset();
  }

  if (unlikely(path.copy(given.str, given.length, given_cs,
                         mvi_path_charset(), &errors) || errors ||
               path.length() > MVI_PATH_MAX_LEN))
  {
    my_error(ER_MVI_BAD_EXPR, MYF(0));
    return NULL;
  }

  /*
    Stack-buffered: json_path_setup() stops at JSON_DEPTH_LIMIT steps, which
    is what this is sized by, so it can never have to grow. Hence the NULL
    MEM_ROOT -- growing it would be a bug, not an allocation.
  */
  initJsonArray(NULL, &jp.steps, sizeof(json_path_step_t), step_buffer, 0);
  if (unlikely(json_path_setup(&jp, mvi_path_charset(),
                               (const uchar *) path.ptr(),
                               (const uchar *) path.end())))
  {
    report_path_error_ex(path.ptr(), &jp, "json_extract", 0,
                         Sql_condition::WARN_LEVEL_ERROR);
    /* Not every way a path can fail has a message of its own */
    if (!thd->is_error())
      my_error(ER_MVI_BAD_EXPR, MYF(0));
    return NULL;
  }

  /*
    A path that can match more than once in a document is a path that
    would lose rows. The fulltext parser reads the array at the first
    match and stops there, see mvi_tokenize_document(), so a row whose
    second match holds the value searched for has no key in the index and
    nothing the query side could search for would find it -- a false
    negative, the one thing this index must never produce. Indexing every
    match is what it would take to allow these, and the query side would
    have to union the keys the same way.
  */
  if (unlikely(jp.types_used & (JSON_PATH_WILD | JSON_PATH_DOUBLE_WILD |
                                JSON_PATH_ARRAY_RANGE)))
  {
    my_error(ER_JSON_PATH_NO_WILDCARD, MYF(0), 1, "json_extract");
    return NULL;
  }

  decl->path.length= path.length();
  if (unlikely(!(decl->path.str= strmake_root(thd->mem_root, path.ptr(),
                                              path.length()))))
    return NULL;                                // Out of memory
  return decl;
}


/*
  @brief
    Handle a `(CAST(expr AS type ARRAY))' key part: turn the key being
    defined into a multi-valued index over the column the array is in.

  @detail
    The key part is that column, and where the array is inside it together
    with the datatype its elements are cast to becomes the key's
    declaration -- which is what goes into the FRM's EXTRA2_MVI_SPEC
    section and what comes back out of it. Nothing is materialised: the
    engine reads the column and the mvi fulltext parser looks inside it for
    the array, see mvi_tokenize_document().

    So the key is a fulltext key, over a column the user wrote, parsed by
    a parser the user may not name. That last part is what keeps the two
    apart: a fulltext key over the same column with no declaration is a
    plain fulltext key over its text, and there would otherwise be nothing
    to tell it from a multi-valued index whose declaration went missing.

    SHOW CREATE TABLE prints neither the parser nor the column, but the
    expression the index was declared with, which is also the only form
    that reads back in, see print_mvi_key_expr().

  @return
    The key part naming the base column, or NULL if an error was raised
*/

Key_part_spec *add_mvi_key_part(THD *thd, Item *expr,
                                const Lex_cast_type_st &cast_type)
{
  LEX *lex= thd->lex;
  Key *key= lex->last_key;
  Item_field *base;
  Mvi_decl *decl;

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

  /*
    Whether the base column is one the index can be built from, and whether
    the engine will hold the keys, are both settled once the columns and the
    engine are known, in init_key_part_spec(): the statement has named
    neither yet.
  */
  if (unlikely(!(decl= mvi_decl_from_key_part(thd, expr, cast_type, &base))))
    return NULL;

  /*
    The parser is put on the key in check_mvi_key_parser(), once whatever
    the statement says about it has been applied: WITH PARSER comes after
    the key parts, so it has not been seen yet.
  */
  key->type= Key::FULLTEXT;
  key->mvi_decl= decl;

  return new (thd->mem_root) Key_part_spec(&base->field_name, 0);
}


/*
  @brief
    DDL: settle which fulltext parser a key is to be parsed by, now that
    the whole key definition has been read.

  @detail
    The parser is how a multi-valued index tells itself apart from a plain
    fulltext key over the same column: see add_mvi_key_part(). So it works
    in both directions.

    A key that is a multi-valued index gets it, whatever the statement says
    -- and if the statement says something else, that is an error rather
    than something to override, because such an index would hold encoded
    elements produced by something that does not encode them.

    A key that is not one may not name it. Such a key would be
    indistinguishable from a multi-valued index whose declaration is
    missing from the FRM, and the server could no longer say which of the
    two it is looking at. Nothing is lost by refusing it either: with no
    declaration to read, the parser does exactly what the built-in one
    does.

  @return
    true   An error was raised
*/

bool check_mvi_key_parser(Key *key)
{
  const LEX_CSTRING &name= key->key_create_info.parser_name;
  const bool named_ours= name.str &&
    Lex_ident_column(name).streq(Lex_cstring_strlen(MVI_PARSER_NAME));

  if (!key->mvi_decl)
  {
    if (!named_ours)
      return false;
    my_error(ER_MVI_RESERVED_PARSER, MYF(0), MVI_PARSER_NAME);
    return true;
  }
  if (name.str && !named_ours)
  {
    my_error(ER_WRONG_USAGE, MYF(0), "WITH PARSER",
             "a multi-valued index");
    return true;
  }
  key->key_create_info.parser_name= Lex_cstring_strlen(MVI_PARSER_NAME);
  return false;
}


/* Collect all the MVI indexes of `table' */
static
bool collect_mvi_indexes_for_table(THD *thd, TABLE *table,
                                   List<Mv_index> *indexes)
{
  if (table->s->mvi_keys.is_clear_all())
    return FALSE;                               /* Not one index of them */
  for (uint i=0; i < table->s->keys; i++)
  {
    /* Only a key the engine has, so s->keys and not s->total_keys */
    const Mvi_decl *decl;
    if (!table->keys_in_use_for_query.is_set(i) ||
        !(decl= table->key_info[i].mvi_decl))
      continue;
    /*
      The engine's token size limits are checked at DDL time, but they
      are settings: this table may have been created when they were
      wider, or on another server.
    */
    if (!mvi_keys_fit_fulltext(table->file, decl->cast_type_handler()))
      continue;
    Mv_index *index= new (thd->mem_root) Mv_index(decl, table, i);
    if (!index || indexes->push_back(index))
      return TRUE; // Out of memory
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
  TABLE *table= index->table;
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
  KEY *key_info= index->table->key_info + index->keyno;
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
    DBUG_ASSERT(access->index->table == tab->table);

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
