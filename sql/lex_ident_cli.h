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


#ifndef LEX_IDENT_CLI
#define LEX_IDENT_CLI

#include "my_global.h"
#include "m_ctype.h"


/**
  A string with metadata. Usually points to a string in the client
  character set, but unlike Lex_ident_cli_st (see below) it does not
  necessarily point to a query fragment. It can also point to memory
  of other kinds (e.g. an additional THD allocated memory buffer
  not overlapping with the current query text).

  We'll add more flags here eventually, to know if the string has, e.g.:
  - multi-byte characters
  - bad byte sequences
  - backslash escapes:   'a\nb'
  and reuse the original query fragments instead of making the string
  copy too early, in Lex_input_stream::get_text().
  This will allow to avoid unnecessary copying, as well as
  create more optimal Item types in sql_yacc.yy
*/
struct Lex_string_with_metadata_st: public LEX_CSTRING
{
private:
  bool m_is_8bit; // True if the string has 8bit characters
  char m_quote;   // Quote character, or 0 if not quoted
public:
  void set_8bit(bool is_8bit) { m_is_8bit= is_8bit; }
  void set_metadata(bool is_8bit, char quote)
  {
    m_is_8bit= is_8bit;
    m_quote= quote;
  }
  void set(const char *s, size_t len, bool is_8bit, char quote)
  {
    str= s;
    length= len;
    set_metadata(is_8bit, quote);
  }
  void set(const LEX_CSTRING *s, bool is_8bit, char quote)
  {
    ((LEX_CSTRING &)*this)= *s;
    set_metadata(is_8bit, quote);
  }
  bool is_8bit() const { return m_is_8bit; }
  bool is_quoted() const { return m_quote != '\0'; }
  char quote() const { return m_quote; }
  // Get string repertoire by the 8-bit flag and the character set
  my_repertoire_t repertoire(CHARSET_INFO *cs) const
  {
    return !m_is_8bit && my_charset_is_ascii_based(cs) ?
           MY_REPERTOIRE_ASCII : MY_REPERTOIRE_UNICODE30;
  }
  // Get string repertoire by the 8-bit flag, for ASCII-based character sets
  my_repertoire_t repertoire() const
  {
    return !m_is_8bit ? MY_REPERTOIRE_ASCII : MY_REPERTOIRE_UNICODE30;
  }
};


/*
  Used to store identifiers in the client character set.
  Points to a query fragment.
*/
struct Lex_ident_cli_st: public Lex_string_with_metadata_st
{
public:
  Lex_ident_cli_st & set_keyword(const char *s, size_t len)
  {
    set(s, len, false, '\0');
    return *this;
  }
  Lex_ident_cli_st & set_ident(const char *s, size_t len, bool is_8bit)
  {
    set(s, len, is_8bit, '\0');
    return *this;
  }
  Lex_ident_cli_st & set_ident_quoted(const char *s, size_t len,
                                      bool is_8bit, char quote)
  {
    set(s, len, is_8bit, quote);
    return *this;
  }
  Lex_ident_cli_st & set_unquoted(const LEX_CSTRING *s, bool is_8bit)
  {
    set(s, is_8bit, '\0');
    return *this;
  }
  const char *pos() const { return str - is_quoted(); }
  const char *end() const { return str + length + is_quoted(); }
};


class Lex_ident_cli: public Lex_ident_cli_st
{
public:
  Lex_ident_cli(const LEX_CSTRING *s, bool is_8bit)
  {
    set_unquoted(s, is_8bit);
  }
  Lex_ident_cli(const char *s, size_t len)
  {
    set_ident(s, len, false);
  }
};

#endif // LEX_IDENT_CLI
