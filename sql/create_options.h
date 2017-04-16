/* Copyright (C) 2010 Monty Program Ab

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02111-1301 USA */

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
  LEX_STRING name;
  LEX_STRING value;
  engine_option_value *next;    ///< parser puts them in a FIFO linked list
  bool parsed;                  ///< to detect unrecognized options
  bool quoted_value;            ///< option=VAL vs. option='VAL'

  engine_option_value(engine_option_value *src,
                      engine_option_value **start, engine_option_value **end) :
    name(src->name), value(src->value),
    next(NULL), parsed(src->parsed), quoted_value(src->quoted_value)
  {
    link(start, end);
  }
  engine_option_value(LEX_STRING &name_arg, LEX_STRING &value_arg, bool quoted,
                      engine_option_value **start, engine_option_value **end) :
    name(name_arg), value(value_arg),
    next(NULL), parsed(false), quoted_value(quoted)
  {
    link(start, end);
  }
  engine_option_value(LEX_STRING &name_arg,
                      engine_option_value **start, engine_option_value **end) :
    name(name_arg), value(null_lex_str),
    next(NULL), parsed(false), quoted_value(false)
  {
    link(start, end);
  }
  engine_option_value(LEX_STRING &name_arg, ulonglong value_arg,
                      engine_option_value **start, engine_option_value **end,
                      MEM_ROOT *root) :
    name(name_arg), next(NULL), parsed(false), quoted_value(false)
  {
    if ((value.str= (char *)alloc_root(root, 22)))
    {
      value.length= longlong10_to_str(value_arg, value.str, 10) - value.str;
      link(start, end);
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
bool parse_option_list(THD* thd, handlerton *hton, void *option_struct,
                       engine_option_value **option_list,
                       ha_create_table_option *rules,
                       bool suppress_warning, MEM_ROOT *root);
bool engine_table_options_frm_read(const uchar *buff, uint length,
                                   TABLE_SHARE *share);
engine_option_value *merge_engine_table_options(engine_option_value *source,
                                                engine_option_value *changes,
                                                MEM_ROOT *root);

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
