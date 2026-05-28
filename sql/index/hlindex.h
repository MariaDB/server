/*
   Copyright (c) 2026, MariaDB plc

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

#include "handler.h"

/*
  singleton with index options and tabledef
  `- hlindexton
  in TABLE_SHARE - TABLE_SHARE and shared context
  `- hlindex_share
  in TABLE - TABLE or read context
  `- hlindex

*/

class hlindex : public Sql_alloc
{
public:
  TABLE *table;
  bool update_needed; // index columns are in write_set, only used for UPDATE
  hlindex(TABLE *t) : table(t), update_needed(false) { }
  virtual ~hlindex();

  virtual int read_first(TABLE *tbl, KEY *keyinfo, const uchar *value,
                         size_t value_len, ulonglong limit) = 0;
  virtual int read_next(TABLE *tbl) = 0;
  virtual int read_end(TABLE *tbl) = 0;

  virtual int insert_row(TABLE *tbl, KEY *keyinfo) = 0;
  virtual int delete_row(TABLE *tbl, const uchar *rec, KEY *keyinfo) = 0;
  virtual int delete_all(TABLE *tbl, KEY *keyinfo, bool truncate) = 0;

  /*
    Row-count estimate for a read_first(tbl, keyinfo, value, value_len, ...)
    lookup, for the range optimizer's cost estimation (see
    Item_func_member_of::get_mm_leaf(), check_quick_select() in
    opt_range.cc). How that estimate is obtained (if at all) is this
    hlindex's own business -- e.g. it may have no single underlying
    handler to call records_in_range() on. Also fills in *cost with this
    hlindex's own idea of the cost of finding those rows (not including
    the cost of then fetching/comparing the resulting base table rows,
    which the caller adds itself).

    Not every hlindex can be looked up this way (e.g. vector search isn't),
    so this is not abstract -- the default is "no estimate available",
    which is what HA_POS_ERROR always means here.
  */
  virtual ha_rows records_in_range(TABLE *tbl, KEY *keyinfo,
                                   const uchar *value, size_t value_len,
                                   Cost_estimate *cost)
  { return HA_POS_ERROR; }

  /*
    Encode "value" into key_buf as a read_first() lookup key, for the range
    optimizer (see Item_func_member_of::get_mm_leaf() in item_jsonfunc.cc).
    How a value is turned into a lookup key -- and how large key_buf needs
    to be -- is this hlindex's own business, not the range optimizer's.

    Returns true if "value" can never match anything (e.g. it is SQL NULL),
    in which case key_buf is left untouched and the caller must not call
    read_first() with it.

    Only a hlindex whose KEY_PART image_type get_mm_leaf() acts on
    (Field::itMVI, currently ARRAY only) is ever asked this, so the
    default is unreachable, not a graceful "unsupported" answer.
  */
  virtual bool make_key(THD *thd, Item *value, uchar *key_buf)
  { DBUG_ASSERT(0); return true; }

  /*
    Render a make_key()-built lookup key as human-readable text, for
    EXPLAIN/optimizer trace range printing (see print_range() in
    opt_range.cc). The key is not a valid key image for the indexed
    column's own type, so the generic Field-based key printing can't be
    used on it -- only this hlindex knows its own key format.

    Default: no decoding available.
  */
  virtual void print_key(String *out, const uchar *key, size_t key_len)
  { out->append('?'); }

  virtual bool reading() = 0;
};

class hlindex_share : public Sql_alloc
{
public:
  hlindex_share(TABLE_SHARE *s) : s(s) {}
  virtual hlindex *create(TABLE *tbl, MEM_ROOT *mem_root) = 0;
  virtual ~hlindex_share();

  TABLE_SHARE *s;
};

struct hlindexton : public transaction_participant
{
  ha_create_table_option *options;
  const LEX_CSTRING (*table_def)(THD *thd, KEY *keyinfo, uint ref_length);
  hlindex_share *(*create)(TABLE_SHARE *share, MEM_ROOT *mem_root);
  uint (*uses_distance)(KEY *keyinfo);
};
