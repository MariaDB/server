/*
   Copyright (c) 2026, MariaDB plc

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; version 2 of
   the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA
*/
#ifndef SQL_MULTI_KEY_INCLUDED
#define SQL_MULTI_KEY_INCLUDED

class Copy_field;
class Field;
class Item;
class THD;
class Virtual_tmp_table;
typedef struct st_join_table JOIN_TAB;

class Multi_key_params
{
public:
  Field **param= nullptr;
  uint params= 0;
  uint rec_length= 0;

  /* Copies the earlier tables' fields cond names, rewriting cond onto them.
     true on failure, no such field or none of an engine other than tab's, a
     BLOB or uneven BIT one, or a maybe-NULL key on a part not null_rejecting */
  bool init(THD *thd, JOIN_TAB *tab, Item **cond);
  /* Packs the combination now in the record buffers into record. true when
     cp_buffer_from_ref() fails: as join_read_always_key() reads no row then,
     the tuple is not sent. */
  bool pack(uchar *record);
  void move_to(uchar *record);
  /* A call must not hold two tuples the key finds equal, and dedup must not
     merge two the backend could answer apart: cmp_key compares their keys as
     the index does, cmp every parameter bit for bit. */
  int cmp_key(const uchar *a, const uchar *b) const;
  int cmp(const uchar *a, const uchar *b) const;

private:
  JOIN_TAB *join_tab= nullptr;
  Virtual_tmp_table *param_table= nullptr;
  Copy_field *copy_field= nullptr;
  uchar *base= nullptr;
};

#endif
