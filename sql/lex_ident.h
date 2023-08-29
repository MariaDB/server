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

#include "char_buffer.h"


/*
  Identifiers for the database objects stored on disk,
  e.g. databases, tables, triggers.
*/
class Lex_ident_fs: public LEX_CSTRING
{
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
  static bool check_body(const char *name, size_t length,
                         bool disallow_path_chars);
  bool check_db_name() const;
  bool check_db_name_with_error() const;
#ifndef DBUG_OFF
  bool ok_for_lower_case_names() const;
#endif
};


/**
  A valid database name identifier,
  checked with check_db_name().
  It's not known if it was lower-cased or is
  in the user typed way.
*/
class Lex_ident_db: public Lex_ident_fs
{
public:
  Lex_ident_db()
   :Lex_ident_fs(NULL, 0)
  { }
  Lex_ident_db(const char *str, size_t length)
   :Lex_ident_fs(str, length)
  {
    DBUG_SLOW_ASSERT(!check_db_name());
  }
};


/**
  A normalized database name:
  - checked with check_db_name()
  - lower-cased if lower_case_table_names>0
*/
class Lex_ident_db_normalized: public Lex_ident_db
{
public:
  Lex_ident_db_normalized(const char *str, size_t length)
   :Lex_ident_db(str, length)
  {
    DBUG_SLOW_ASSERT(ok_for_lower_case_names());
  }
  explicit Lex_ident_db_normalized(const LEX_CSTRING &str)
   :Lex_ident_db(str.str, str.length)
  {
    DBUG_SLOW_ASSERT(ok_for_lower_case_names());
  }
};


template<size_t buff_sz>
class IdentBuffer: public CharBuffer<buff_sz>
{
  constexpr static CHARSET_INFO *charset()
  {
    return &my_charset_utf8mb3_general_ci;
  }
public:
  IdentBuffer()
  { }
  IdentBuffer<buff_sz> & copy_casedn(const LEX_CSTRING &str)
  {
    CharBuffer<buff_sz>::copy_casedn(charset(), str);
    return *this;
  }
};


template<size_t buff_sz>
class IdentBufferCasedn: public IdentBuffer<buff_sz>
{
public:
  IdentBufferCasedn(const LEX_CSTRING &str)
  {
    IdentBuffer<buff_sz>::copy_casedn(str);
  }
};


/*
  A helper class to store temporary database names in a buffer.
  After constructing it's typically should be checked using
  Lex_ident_fs::check_db_name().

  Note, the database name passed to the constructor can originally
  come from the parser and can be of an atribtrary long length.
  Let's reserve additional buffer space for one extra character
  (SYSTEM_CHARSET_MBMAXLEN bytes), so check_db_name() can still
  detect too long names even if the constructor cuts the data.
*/
class DBNameBuffer: public CharBuffer<SAFE_NAME_LEN + MY_CS_MBMAXLEN>
{
public:
  DBNameBuffer()
  { }
  DBNameBuffer(const LEX_CSTRING &db, bool casedn)
  {
    copy_casedn(&my_charset_utf8mb3_general_ci, db, casedn);
  }
  Lex_ident_db to_lex_ident_db() const
  {
    const LEX_CSTRING tmp= to_lex_cstring();
    if (Lex_ident_fs(tmp).check_db_name())
      return Lex_ident_db();
    return Lex_ident_db(tmp.str, tmp.length);
  }
  Lex_ident_db to_lex_ident_db_with_error() const
  {
    const LEX_CSTRING tmp= to_lex_cstring();
    if (Lex_ident_fs(tmp).check_db_name_with_error())
      return Lex_ident_db();
    return Lex_ident_db(tmp.str, tmp.length);
  }
};


#endif // LEX_IDENT_INCLUDED
