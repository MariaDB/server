/* Copyright (C) 2010, 2021, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

/**
  @file

  Engine defined options of tables/fields/keys in CREATE/ALTER TABLE.
*/

#ifndef SQL_CREATE_OPTIONS_INCLUDED
#define SQL_CREATE_OPTIONS_INCLUDED

#include "sql_class.h"

enum { ENGINE_OPTION_MAX_LENGTH=32767 };

class engine_option_value: public Sql_alloc
{
 public:
  LEX_CSTRING name;
  LEX_CSTRING value;
  engine_option_value *next;    ///< parser puts them in a FIFO linked list
  bool parsed;                  ///< to detect unrecognized options
  bool quoted_value;            ///< option=VAL vs. option='VAL'

  engine_option_value(engine_option_value *src) :
    name(src->name), value(src->value),
    next(NULL), parsed(src->parsed), quoted_value(src->quoted_value)
  {
  }
  engine_option_value(LEX_CSTRING &name_arg, LEX_CSTRING &value_arg,
                      bool quoted) :
    name(name_arg), value(value_arg),
    next(NULL), parsed(false), quoted_value(quoted)
  {
  }
  engine_option_value(LEX_CSTRING &name_arg):
    name(name_arg), value(null_clex_str),
    next(NULL), parsed(false), quoted_value(false)
  {
  }
  engine_option_value(LEX_CSTRING &name_arg, ulonglong value_arg,
                      MEM_ROOT *root) :
    name(name_arg), next(NULL), parsed(false), quoted_value(false)
  {
    char *str;
    if (likely((value.str= str= (char *)alloc_root(root, 22))))
    {
      value.length= longlong10_to_str(value_arg, str, 10) - str;
    }
  }
  static uchar *frm_read(const uchar *buff, const uchar *buff_end,
                         engine_option_value **start,
                         engine_option_value **end, MEM_ROOT *root);
  void link(engine_option_value **start, engine_option_value **end);
  uint frm_length();
  uchar *frm_image(uchar *buff);
};

typedef struct st_key KEY;
class Create_field;

bool resolve_sysvar_table_options(handlerton *hton);
void free_sysvar_table_options(handlerton *hton);
bool parse_engine_table_options(THD *thd, handlerton *ht, TABLE_SHARE *share);
#ifdef WITH_PARTITION_STORAGE_ENGINE
bool parse_engine_part_options(THD *thd, TABLE *table);
#endif
bool parse_option_list(THD* thd, handlerton *hton, void *option_struct,
                       engine_option_value **option_list,
                       ha_create_table_option *rules,
                       bool suppress_warning, MEM_ROOT *root);
bool engine_table_options_frm_read(const uchar *buff, size_t length,
                                   TABLE_SHARE *share);
bool merge_engine_options(engine_option_value *source,
                          engine_option_value *changes,
                          engine_option_value **out, MEM_ROOT *root);

uint engine_table_options_frm_length(engine_option_value *table_option_list,
                                     List<Create_field> &create_fields,
                                     uint keys, KEY *key_info);
uchar *engine_table_options_frm_image(uchar *buff,
                                      engine_option_value *table_option_list,
                                      List<Create_field> &create_fields,
                                      uint keys, KEY *key_info);

bool engine_options_differ(void *old_struct, void *new_struct,
                           ha_create_table_option *rules);
bool is_engine_option_known(engine_option_value *opt,
                            ha_create_table_option *rules);
#endif
