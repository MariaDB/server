#ifndef LEX_IDENT_INCLUDED
#define LEX_IDENT_INCLUDED
/*
   Copyright (c) 2023, MariaDB

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


extern MYSQL_PLUGIN_IMPORT CHARSET_INFO *table_alias_charset;


/*
  Identifiers for the database objects stored on disk,
  e.g. databases, tables, triggers.
*/
class Lex_ident_fs: public LEX_CSTRING
{
public:
  static CHARSET_INFO *charset_info()
  {
    return table_alias_charset;
  }
public:
  Lex_ident_fs()
   :LEX_CSTRING({0,0})
  { }
  Lex_ident_fs(const char *str, size_t length)
   :LEX_CSTRING({str, length})
  { }
  explicit Lex_ident_fs(const LEX_CSTRING &str)
   :LEX_CSTRING(str)
  { }
#if MYSQL_VERSION_ID<=110501
private:
  static bool is_valid_ident(const LEX_CSTRING &str)
  {
    // NULL identifier, or 0-terminated identifier
    return (str.str == NULL && str.length == 0) || str.str[str.length] == 0;
  }
public:
  bool streq(const LEX_CSTRING &rhs) const
  {
    DBUG_ASSERT(is_valid_ident(*this));
    DBUG_ASSERT(is_valid_ident(rhs));
    return length == rhs.length &&
           my_strcasecmp(charset_info(), str, rhs.str) == 0;
  }
#else
/*
  Starting from 11.5.1 streq() is inherited from the base.
  The above implementations of streq() and is_valid_ident() should be removed.
*/
#error Remove streq() above.
#endif
};


class Lex_ident_db: public Lex_ident_fs
{
public:
  using Lex_ident_fs::Lex_ident_fs;
};


class Lex_ident_table: public Lex_ident_fs
{
public:
  using Lex_ident_fs::Lex_ident_fs;
};


#endif // LEX_IDENT_INCLUDED
