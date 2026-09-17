/* Copyright (c) 2026, MariaDB plc.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA
*/

#include <my_global.h>
#include "hlindex.h"
#include "json.h"
#include "create_options.h"
#include "sql_tree.h"
#include "sql_hset.h"
#include "item_func.h"
#include <json_lib.h>
#include "key.h"
#include <math.h>

ha_create_table_option json_index_options[]=
{
  HA_IOPTION_END
};

static struct st_mysql_storage_engine json_daemon=
{ MYSQL_DAEMON_INTERFACE_VERSION };

st_plugin_int *json_index_plugin;

enum Json_index_fields { FIELD_TYP, FIELD_VALUE, FIELD_TREF };

class json_index : public hlindex
{
  int parse_array_first(TABLE *tbl, const uchar *rec, json_engine_t *je,
                        String *str);
  int parse_array_next(json_engine_t *je);
  uchar match_key[JSON_INDEX_KEY_MAX_LEN];
  uint match_key_len;
  bool match_started;
  /* A second handler to do deletes during the read_next scan */
  handler *delete_handler;
public:
  json_index(TABLE *t) :hlindex(t), match_started(false), delete_handler(0)
  {}
  ~json_index() { DBUG_ASSERT(!delete_handler); }
  int insert_row(TABLE *tbl, KEY *keyinfo) override;
  int read_first(TABLE *tbl, KEY *keyinfo, const uchar *value,
                 size_t value_len, ulonglong limit) override;
  int read_next(TABLE *tbl) override;
  int read_end(TABLE *tbl) override;
  int delete_row(TABLE *tbl, const uchar *rec, KEY *keyinfo) override;
  int delete_all(TABLE *tbl, KEY *keyinfo, bool truncate) override;
  ha_rows records_in_range(TABLE *tbl, KEY *keyinfo, const uchar *value,
                           size_t value_len, Cost_estimate *cost) override;
  bool make_key(THD *thd, Item *value, uchar *key_buf) override;
  void print_key(String *out, const uchar *key, size_t key_len) override;
  bool reading() override
  { return table->file->inited != handler::NONE; }
};


int json_index::parse_array_first(TABLE *tbl, const uchar *rec,
                                  json_engine_t *je, String *str)
{
  tbl->file->position(rec);
  table->field[FIELD_TREF]->store_binary(tbl->file->ref, tbl->file->ref_length);

  mem_root_dynamic_array_init(tbl->in_use->mem_root, PSI_INSTRUMENT_MEM,
                              &je->stack, sizeof(int), NULL,
                              JSON_DEPTH_DEFAULT, JSON_DEPTH_INC, MYF(0));

  if (json_scan_start(je, str->charset(), (const uchar*)str->ptr(),
                      (const uchar*)str->end()) ||
      json_read_value(je) || je->value_type != JSON_VALUE_ARRAY ||
      je->state != JST_ARRAY_START)
    return HA_ERR_BAD_FIELD_VALUE;

  return parse_array_next(je);
}

/*
  Encode the JSON value "je" is currently positioned on (a scalar: bool,
  null, number or string) as a type tag ("*tag_out", one of the
  JSON_VALUE_* constants) plus a type-specific payload written to "buf"
  (empty for bool/null, which the tag alone already fully describes).
  Shared by parse_array_next() (encoding one array element read off a
  json_engine_t) and json_index_encode_value() (encoding a lookup value
  straight from an Item, via a one-token json_engine_t scan).
*/
static int json_index_encode_token(json_engine_t *je, uchar *tag_out,
                                   uchar *buf, size_t buf_size, int *len_out)
{
  int len= 0;
  *tag_out= (uchar) je->value_type;
  switch (je->value_type)
  {
    case JSON_VALUE_UNINITIALIZED:
    case JSON_VALUE_OBJECT:
    case JSON_VALUE_ARRAY:
      return HA_ERR_BAD_FIELD_VALUE;

    case JSON_VALUE_TRUE:
    case JSON_VALUE_FALSE:
    case JSON_VALUE_NULL:
      break;

    case JSON_VALUE_NUMBER:
      {
        int er;
        char *s= (char *)je->value_begin, *e;
        double d= je->s.cs->strntod(s, je->value_len, &e, &er);
        if (er)
          return HA_ERR_BAD_FIELD_VALUE;
        float8store(buf, d);
        len= 8;
      }
      break;

    case JSON_VALUE_STRING:
      len= json_unescape(je->s.cs, je->value, je->value + je->value_len,
                         je->s.cs, buf, buf + buf_size);
      if (len == JSON_ERROR_OUT_OF_SPACE)
        len= (int) buf_size;
      else if (len < 0)
        return HA_ERR_BAD_FIELD_VALUE;
      break;
  }
  *len_out= len;
  return 0;
}

int json_index::parse_array_next(json_engine_t *je)
{
  if (json_scan_next(je))
    return HA_ERR_BAD_FIELD_VALUE;

  if (je->state == JST_ARRAY_END)
    return EOF;
  if (je->state != JST_VALUE)
    return HA_ERR_BAD_FIELD_VALUE;
  if (json_read_value(je))
    return HA_ERR_BAD_FIELD_VALUE;

  uchar tag, buf[JSON_INDEX_VALUE_MAX_LEN];
  int len, err;
  if ((err= json_index_encode_token(je, &tag, buf, sizeof(buf), &len)))
    return err;
  table->field[FIELD_TYP]->store((longlong) tag, true);
  table->field[FIELD_VALUE]->store_binary(buf, len);
  return 0;
}

/*
  Encode "value" the same way json_index unrolls a JSON array element into
  the hlindex table's `typ`/`value` columns: a type tag plus a
  type-specific payload (see parse_array_next() for the array-element side
  of this same encoding), and store them into "typ_field"/"value_field"
  (the hlindex table's table->field[FIELD_TYP]/[FIELD_VALUE]).

  "value" must already evaluate (via val_str()) to valid JSON text -- e.g.
  Item_func_member_of's json_quote_item, which renders a plain SQL scalar
  the same way val_bool()'s JSON_CONTAINS(array, JSON_QUOTE(value)) does,
  so an indexed lookup matches the same rows the non-indexed evaluation
  would. Turning an arbitrary SQL Item into JSON text is the caller's job,
  not this function's -- it only knows how to canonicalize JSON text into
  a lookup value.

  Used by json_index::make_key() to prepare a lookup for
  "value MEMBER OF (json_array_column)" -- it then key_copy()'s
  typ_field/value_field into an actual lookup key.

  Returns true if "value" cannot be a member of any JSON array (e.g. it is
  SQL NULL, or a JSON object/array) -- the caller must not build a key from
  typ_field/value_field in that case.
*/
static bool json_index_encode_value(THD *thd, Item *value, Field *typ_field,
                                    Field *value_field)
{
  StringBuffer<64> qbuf;
  String *json_text= value->val_str(&qbuf);
  if (!json_text)
    return true; // SQL NULL: cannot be a JSON array member

  json_engine_t je;
  mem_root_dynamic_array_init(thd->mem_root, PSI_INSTRUMENT_MEM, &je.stack,
                              sizeof(int), NULL, JSON_DEPTH_DEFAULT,
                              JSON_DEPTH_INC, MYF(0));

  if (json_scan_start(&je, json_text->charset(), (const uchar*) json_text->ptr(),
                      (const uchar*) json_text->end()) ||
      json_read_value(&je))
    return true;

  uchar tag, buf[JSON_INDEX_VALUE_MAX_LEN];
  int len;
  if (json_index_encode_token(&je, &tag, buf, sizeof(buf), &len))
    return true;
  typ_field->store((longlong) tag, true);
  value_field->store_binary(buf, len);
  return false;
}

bool json_index::make_key(THD *thd, Item *value, uchar *key_buf)
{
  if (json_index_encode_value(thd, value, table->field[FIELD_TYP],
                              table->field[FIELD_VALUE]))
    return true; // NULL, or a JSON object/array

  /*
    Build a lookup key for the (typ,value) keypart prefix of the hlindex
    table's only index (the (typ,value,tref) PRIMARY KEY) from the fields
    json_index_encode_value() just stored, using the standard key_copy()
    encoding (typ's raw byte, then value's HA_KEY_BLOB_LENGTH-byte length
    prefix + payload) so the result can be passed straight to
    handler::ha_index_read_map(), handler::ha_index_next_same() or
    handler::records_in_range().
  */
  key_copy(key_buf, table->record[0], table->key_info, JSON_INDEX_KEY_MAX_LEN);
  return false;
}

void json_index::print_key(String *out, const uchar *key, size_t key_len)
{
  DBUG_ASSERT(key_len == JSON_INDEX_KEY_MAX_LEN);
  uchar typ= key[0];
  uint len= uint2korr(key + 1);
  const uchar *payload= key + 1 + HA_KEY_BLOB_LENGTH;

  switch (typ)
  {
  case JSON_VALUE_NULL:
    out->append(STRING_WITH_LEN("null"));
    break;
  case JSON_VALUE_TRUE:
    out->append(STRING_WITH_LEN("true"));
    break;
  case JSON_VALUE_FALSE:
    out->append(STRING_WITH_LEN("false"));
    break;
  case JSON_VALUE_NUMBER:
  {
    double d;
    float8get(d, payload);
    out->qs_append(d);
    break;
  }
  case JSON_VALUE_STRING:
    out->append('"');
    out->append((const char*) payload, len);
    out->append('"');
    break;
  default:
    out->append('?');
    break;
  }
}

ha_rows json_index::records_in_range(TABLE *tbl, KEY *keyinfo,
                                     const uchar *value, size_t value_len,
                                     Cost_estimate *cost)
{
  DBUG_ASSERT(keyinfo->algorithm == HA_KEY_ALG_ARRAY);
  DBUG_ASSERT(value_len == JSON_INDEX_KEY_MAX_LEN);

  key_range min_range, max_range;
  min_range.key= max_range.key= value;
  min_range.length= max_range.length= (uint) value_len;
  min_range.keypart_map= max_range.keypart_map= (key_part_map) 3; // typ,value
  min_range.flag= HA_READ_KEY_EXACT;
  max_range.flag= HA_READ_AFTER_KEY;
  page_range pages= unused_page_range;

  ha_rows rows= table->file->records_in_range(0, &min_range, &max_range,
                                              &pages);
  if (rows != HA_POS_ERROR)
  {
    DBUG_ASSERT(rows >= 1);
    cost->reset(tbl->file);
    cost->index_cost= table->file->ha_key_scan_time(0, rows);
    cost->row_cost= tbl->file->ha_rnd_pos_time(rows);
  }
  return rows;
}

int json_index::insert_row(TABLE *tbl, KEY *keyinfo)
{
  MY_BITMAP *old_map= dbug_tmp_use_all_columns(tbl, &tbl->read_set);
  Field *json_field= keyinfo->key_part->field;
  StringBuffer<1024> buf;
  String *res= json_field->val_str(&buf);
  int err= HA_ERR_BAD_FIELD_VALUE;
  json_engine_t je;

  DBUG_ASSERT(keyinfo->algorithm == HA_KEY_ALG_ARRAY);
  DBUG_ASSERT(keyinfo->usable_key_parts == 1);
  DBUG_ASSERT(tbl->file->ref_length <= table->field[FIELD_TREF]->field_length);

  /*
    NULL (e.g. a generated column whose path didn't match, the main use
    case for indexing an array nested inside a larger JSON document --
    generated columns can never be declared NOT NULL) has no array to
    index: leave err at its HA_ERR_BAD_FIELD_VALUE default, same as any
    other value json_index can't index.
  */
  if (res)
    for (err= parse_array_first(tbl, tbl->record[0], &je, res); !err;
         err= parse_array_next(&je))
    {
      if ((err= table->file->ha_write_row(table->record[0])) &&
          err != HA_ERR_FOUND_DUPP_KEY) // skip duplicates in the array
        break;
    }

  dbug_tmp_restore_column_map(&tbl->read_set, old_map);
  if (err == EOF || err == 0)
    return 0;
  if (err == HA_ERR_BAD_FIELD_VALUE)
    tbl->file->lookup_errkey= keyinfo - tbl->key_info;
  return err;
}

int json_index::delete_row(TABLE *tbl, const uchar *rec, KEY *keyinfo)
{
  MY_BITMAP *old_map= dbug_tmp_use_all_columns(tbl, &tbl->read_set);
  Field *json_field= keyinfo->key_part->field;
  my_ptrdiff_t ptrdiff= rec - tbl->record[0];
  StringBuffer<1024> buf;
  uchar *key;
  String *res;
  int err;
  json_engine_t je;
  handler *h;

  json_field->move_field_offset(ptrdiff);
  res= json_field->val_str(&buf);
  json_field->move_field_offset(-ptrdiff);

  DBUG_ASSERT(keyinfo->algorithm == HA_KEY_ALG_ARRAY);
  DBUG_ASSERT(keyinfo->usable_key_parts == 1);
  DBUG_ASSERT(tbl->file->ref_length <= table->field[FIELD_TREF]->field_length);

  /*
    NULL was never indexed (see json_index::insert_row()), so there's
    nothing to look up and delete -- skip straight to the no-op return
    below without even opening a handler for it.
  */
  if (!res)
  {
    err= HA_ERR_BAD_FIELD_VALUE;
    goto ret;
  }

  h= table->file;
  if (h->inited != handler::NONE)
  {
    /*
      This is DELETE that uses hlindex to find rows,
      like DELETE t WHERE 5 MEMBER OF (c).
      (cannot be UPDATE - used_key_is_modified check prevents it).
      To not destroy the read scan, let's use a separate handler below.
    */
    if (!((h= delete_handler)))
    {
      if (!(h= table->file->clone(table->s->normalized_path.str,
                                  tbl->in_use->mem_root)))
      {
        err= HA_ERR_OUT_OF_MEM;
        goto ret;
      }
      if ((err= h->ha_external_lock(tbl->in_use, F_WRLCK)))
      {
        delete h;
        goto ret;
      }
      delete_handler= h;
    }
  }

  if ((err= h->ha_index_init(0, false)))
    goto ret;

  key= (uchar*) my_alloca(table->key_info->key_length);
  for (err= parse_array_first(tbl, rec, &je, res); !err;
       err= parse_array_next(&je))
  {
    key_copy(key, table->record[0], table->key_info,
             table->key_info->key_length);
    err= h->ha_index_read_map(table->record[0], key, HA_WHOLE_KEY,
                              HA_READ_KEY_EXACT);
    if (err == HA_ERR_KEY_NOT_FOUND) // already gone, e.g. a duplicate
      continue;                      // value elsewhere in the same array
    if (err || (err= h->ha_delete_row(table->record[0])))
      break;
  }
  my_afree(key);
  h->ha_index_end();

ret:
  dbug_tmp_restore_column_map(&tbl->read_set, old_map);
  if (err == EOF || err == 0 || err == HA_ERR_BAD_FIELD_VALUE)
    return 0;
  return err;
}

int json_index::delete_all(TABLE *tbl, KEY *keyinfo, bool truncate)
{
  DBUG_ASSERT(keyinfo->algorithm == HA_KEY_ALG_ARRAY);
  DBUG_ASSERT(keyinfo->usable_key_parts == 1);
  return truncate ? table->file->truncate() : table->file->delete_all_rows();
}

int json_index::read_first(TABLE *tbl, KEY *keyinfo, const uchar *value,
                           size_t value_len, ulonglong limit)
{
  DBUG_ASSERT(keyinfo->algorithm == HA_KEY_ALG_ARRAY);
  DBUG_ASSERT(keyinfo->usable_key_parts == 1);
  DBUG_ASSERT(value && value_len == JSON_INDEX_KEY_MAX_LEN);

  match_started= false;

  /*
    "value" is already the key_copy()-formatted lookup key -- built once by
    Item_func_member_of::get_mm_leaf() (sql/item_jsonfunc.cc) via
    make_key(), and carried here as QUICK_RANGE::min_key/min_length -- so
    there is nothing left to encode.
  */
  memcpy(match_key, value, value_len);
  match_key_len= (uint) value_len;

  /*
    (typ,value,tref) is the whole table -- all three columns are in the
    PRIMARY KEY read below, so there is no row to go back to (a noop for
    InnoDB, and saves the .MYD read for MyISAM).
  */
  if (!table->file->keyread_enabled())
    table->file->ha_start_keyread(0);
  if (int err= table->file->ha_index_init(0, true))
    return err;
  if (int err= tbl->file->ha_rnd_init(0))
    return err; // read_end() will still close the index cursor above
  return read_next(tbl);
}

int json_index::read_next(TABLE *tbl)
{
  int err= match_started
    ? table->file->ha_index_next_same(table->record[0], match_key,
                                      match_key_len)
    : table->file->ha_index_read_map(table->record[0], match_key,
                                     (key_part_map) 3 /* typ,value */,
                                     HA_READ_KEY_EXACT);
  match_started= true;
  if (err)
    return err == HA_ERR_KEY_NOT_FOUND ? HA_ERR_END_OF_FILE : err;

  StringBuffer<64> tref_buf;
  String *tref= table->field[FIELD_TREF]->val_str(&tref_buf);
  return tbl->file->ha_rnd_pos(tbl->record[0], (uchar*) tref->ptr());
}

int json_index::read_end(TABLE *tbl)
{
  if (table->file->inited == handler::INDEX)
    table->file->ha_index_end();
  table->file->ha_end_keyread();
  if (tbl->file->inited == handler::RND)
    tbl->file->ha_rnd_end();
  if (delete_handler)
  {
    delete_handler->ha_external_lock(tbl->in_use, F_UNLCK);
    delete_handler->ha_close();
    delete delete_handler;
    delete_handler= 0;
  }
  return 0;
}

class json_index_share : public hlindex_share
{
public:
  json_index_share(TABLE_SHARE *s) : hlindex_share(s) {}
  hlindex *create(TABLE *tbl, MEM_ROOT *mem_root) override
  { return new (mem_root) json_index(tbl); }
};

static const LEX_CSTRING json_index_table_def(THD *thd, uint ref_length)
{
  /*
    typ is one of the JSON_VALUE_* constants (json_lib.h), stored as its
    own column instead of a leading tag byte inside "value", so the
    hlindex table stays plain SQL to look at (e.g. in the optimizer trace,
    or just poking at it by hand) rather than an opaque blob.
  */
  const char templ[]="CREATE TABLE i (                "
                     "  typ tinyint unsigned not null, "
                     "  value varbinary("
                             STRINGIFY_ARG(JSON_INDEX_VALUE_MAX_LEN) "),"
                     "  tref varbinary(%u),            "
                     "  PRIMARY KEY (typ,value,tref))  ";
  size_t len= sizeof(templ) + 32;
  char *s= thd->alloc(len);
  len= my_snprintf(s, len, templ, ref_length);
  return {s, len};
}

struct hlindexton json_hliton=
{
  {0, 0, 0,
  nullptr,                        /* close_connection */
  nullptr,                        /* savepoint_set */
  nullptr,
  nullptr,                        /*savepoint_rollback_can_release_mdl*/
  nullptr,                        /*savepoint_release*/
  nullptr, nullptr,
  nullptr,                        /* prepare */
  nullptr,                        /* recover */
  nullptr, nullptr,               /* commit/rollback_by_xid */
  nullptr, nullptr,               /* recover_rollback_by_xid/recovery_done */
  nullptr, nullptr, nullptr,      /* snapshot, commit/prepare_ordered */
  nullptr, nullptr},              /* checkpoint, versioned */
  json_index_options,            /* options */
  json_index_table_def,          /* table_def */
  [](TABLE_SHARE *s, MEM_ROOT *mem_root) -> hlindex_share* {
    return new (mem_root) json_index_share(s);
  },
  nullptr                         /* uses_distance */
};

static int json_init(void *p)
{
  json_index_plugin= (st_plugin_int *)p;
  json_index_plugin->data= &json_hliton;
  if (setup_transaction_participant(json_index_plugin))
    return 1;
  return resolve_sysvar_table_options(json_index_options); // XXX move it out
}

maria_declare_plugin(json)
{
  MYSQL_DAEMON_PLUGIN,
  &json_daemon, "json", "MariaDB plc", "A plugin for json indexes",
  PLUGIN_LICENSE_GPL, json_init, NULL, 0x0100, NULL,
  NULL, "1.0", MariaDB_PLUGIN_MATURITY_STABLE
}
maria_declare_plugin_end;
