/*
   Copyright (c) 2018, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#ifndef VERS_STRING_INCLUDED
#define VERS_STRING_INCLUDED

/*
  LEX_CSTRING with comparison semantics.
*/

// db and table names: case sensitive (or insensitive) in table_alias_charset
struct Compare_table_names
{
  int operator()(const LEX_CSTRING& a, const LEX_CSTRING& b) const
  {
    DBUG_ASSERT(a.str[a.length] == 0);
    DBUG_ASSERT(b.str[b.length] == 0);
    return my_strnncoll(table_alias_charset,
                        (uchar*)a.str, a.length,
                        (uchar*)b.str, b.length);
  }
};

// column names and other identifiers: case insensitive in system_charset_info
struct Compare_identifiers
{
  int operator()(const LEX_CSTRING& a, const LEX_CSTRING& b) const
  {
    DBUG_ASSERT(a.str[a.length] == 0);
    DBUG_ASSERT(b.str[b.length] == 0);
    return my_strcasecmp(system_charset_info, a.str, b.str);
  }
};

class Lex_cstring : public LEX_CSTRING
{
  public:
  Lex_cstring()
  {
    str= NULL;
    length= 0;
  }
  Lex_cstring(const char *_str, size_t _len)
  {
    str= _str;
    length= _len;
  }
  void set(const char *_str, size_t _len)
  {
    str= _str;
    length= _len;
  }
};

template <class Compare>
struct Lex_cstring_with_compare : public Lex_cstring
{
public:
  Lex_cstring_with_compare() {}
  Lex_cstring_with_compare(const char *_str, size_t _len) :
    Lex_cstring(_str, _len)
  { }
  Lex_cstring_with_compare(const LEX_STRING src) :
    Lex_cstring(src.str, src.length)
  { }
  Lex_cstring_with_compare(const LEX_CSTRING src) : Lex_cstring(src.str, src.length)
  { }
  Lex_cstring_with_compare(const char *_str) : Lex_cstring(_str, strlen(_str))
  { }
  bool streq(const Lex_cstring_with_compare& b) const
  {
    return Lex_cstring::length == b.length && 0 == Compare()(*this, b);
  }
  operator const char* () const
  {
    return str;
  }
  operator bool () const
  {
    return str != NULL;
  }
};

typedef Lex_cstring_with_compare<Compare_identifiers> Lex_ident;
typedef Lex_cstring_with_compare<Compare_table_names> Lex_table_name;

#define XSTRING_WITH_LEN(X) (X).ptr(), (X).length()
#define DB_WITH_LEN(X) (X).db.str, (X).db.length
#define TABLE_NAME_WITH_LEN(X) (X).table_name.str, (X).table_name.length

#endif // VERS_STRING_INCLUDED
