/* Copyright (c) 2000, 2019, Oracle and/or its affiliates.
   Copyright (c) 2010, 2024, MariaDB Corporation.

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


#ifndef LEX_IDENT_SYS
#define LEX_IDENT_SYS

#include "lex_ident_cli.h"
#include "sql_alloc.h"

extern "C" MYSQL_PLUGIN_IMPORT CHARSET_INFO *system_charset_info;

struct Lex_ident_sys_st: public LEX_CSTRING, Sql_alloc
{
public:
  bool copy_ident_cli(const THD *thd, const Lex_ident_cli_st *str);
  bool copy_keyword(const THD *thd, const Lex_ident_cli_st *str);
  bool copy_sys(const THD *thd, const LEX_CSTRING *str);
  bool convert(const THD *thd, const LEX_CSTRING *str, CHARSET_INFO *cs);
  bool copy_or_convert(const THD *thd, const Lex_ident_cli_st *str,
                       CHARSET_INFO *cs);
  bool is_null() const { return str == NULL; }
  bool to_size_number(ulonglong *to) const;
  void set_valid_utf8(const LEX_CSTRING *name)
  {
    DBUG_ASSERT(Well_formed_prefix(system_charset_info, name->str,
                                   name->length).length() == name->length);
    str= name->str ; length= name->length;
  }
};


class Lex_ident_sys: public Lex_ident_sys_st
{
public:
  Lex_ident_sys(const THD *thd, const Lex_ident_cli_st *str)
  {
    if (copy_ident_cli(thd, str))
      *this= Lex_ident_sys();
  }
  Lex_ident_sys()
  {
    ((LEX_CSTRING &) *this)= {nullptr, 0};
  }
  Lex_ident_sys(const char *name, size_t length)
  {
    LEX_CSTRING tmp= {name, length};
    set_valid_utf8(&tmp);
  }
  Lex_ident_sys(const THD *thd, const LEX_CSTRING *str)
  {
    set_valid_utf8(str);
  }
  Lex_ident_sys & operator=(const Lex_ident_sys_st &name)
  {
    Lex_ident_sys_st::operator=(name);
    return *this;
  }
};


#endif // LEX_IDENT_SYS
