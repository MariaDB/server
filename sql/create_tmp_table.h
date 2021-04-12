#ifndef CREATE_TMP_TABLE_INCLUDED
#define CREATE_TMP_TABLE_INCLUDED

/* Copyright (c) 2021, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */


/*
  Class for creating internal tempory tables in sql_select.cc
*/

class Create_tmp_table: public Data_type_statistics
{
protected:
  // The following members are initialized only in start()
  Field **m_from_field, **m_default_field;
  KEY_PART_INFO *m_key_part_info;
  uchar	*m_group_buff, *m_bitmaps;
  // The following members are initialized in ctor
  uint  m_alloced_field_count;
  bool  m_using_unique_constraint;
  uint m_temp_pool_slot;
  ORDER *m_group;
  bool m_distinct;
  bool m_save_sum_fields;
  bool m_with_cycle;
  ulonglong m_select_options;
  ha_rows m_rows_limit;
  uint m_group_null_items;

  // counter for distinct/other fields
  uint m_field_count[2];
  // counter for distinct/other fields which can be NULL
  uint m_null_count[2];
  // counter for distinct/other  blob fields
  uint m_blobs_count[2];
  // counter for "tails" of bit fields which do not fit in a byte
  uint m_uneven_bit[2];

public:
  enum counter {distinct, other};
  /*
    shows which field we are processing: distinct/other (set in processing
    cycles)
  */
  counter current_counter;
  Create_tmp_table(ORDER *group, bool distinct, bool save_sum_fields,
                   ulonglong select_options, ha_rows rows_limit);
  virtual ~Create_tmp_table() {}
  virtual bool choose_engine(THD *thd, TABLE *table, TMP_TABLE_PARAM *param);
  void add_field(TABLE *table, Field *field, uint fieldnr,
                 bool force_not_null_cols);
  TABLE *start(THD *thd,
               TMP_TABLE_PARAM *param,
               const LEX_CSTRING *table_alias);
  bool add_fields(THD *thd, TABLE *table,
                  TMP_TABLE_PARAM *param, List<Item> &fields);

  bool add_schema_fields(THD *thd, TABLE *table,
                         TMP_TABLE_PARAM *param,
                         const ST_SCHEMA_TABLE &schema_table);

  bool finalize(THD *thd, TABLE *table, TMP_TABLE_PARAM *param,
                bool do_not_open, bool keep_row_order);
  void cleanup_on_failure(THD *thd, TABLE *table);
};

#endif /* CREATE_TMP_TABLE_INCLUDED */
