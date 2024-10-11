/*
   Copyright (c) 2025, Rakuten Securities
   Copyright (c) 2025, MariaDB plc

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
#ifndef FIELD_COMPOSITE_INCLUDED
#define FIELD_COMPOSITE_INCLUDED

#include "field.h"

class Field_composite: public Field_null
{
public:
  Field_composite(uchar *ptr_arg, const LEX_CSTRING *field_name_arg)
    :Field_null(ptr_arg, 0, Field::NONE, field_name_arg, &my_charset_bin)
    {}
  en_fieldtype tmp_engine_column_type(bool use_packed_rows) const override
  {
    DBUG_ASSERT(0);
    return Field::tmp_engine_column_type(use_packed_rows);
  }
  enum_conv_type rpl_conv_type_from(const Conv_source &source,
                                    const Relay_log_info *rli,
                                    const Conv_param &param) const override
  {
    DBUG_ASSERT(0);
    return CONV_TYPE_IMPOSSIBLE;
  }

  virtual uint rows() const { return 0; }
  virtual bool get_key(String *key, bool is_first) { return true; }
  virtual bool get_next_key(const String *curr_key, String *next_key)
  {
    return true;
  }
  virtual bool get_prior_key(const String *curr_key, String *prior_key)
  {
    return true;
  }
  virtual Item_field *element_by_key(THD *thd, String *key) { return NULL; }
  virtual Item_field *element_by_key(THD *thd, String *key) const
  {
    return NULL;
  }
  virtual Item **element_addr_by_key(THD *thd, String *key) { return NULL; }
  virtual bool delete_all_elements() { return true; }
  virtual bool delete_element_by_key(String *key) { return true; }
};

#endif /* FIELD_COMPOSITE_INCLUDED */
