/* Copyright (c) 2023, MariaDB Corporation.

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

#ifndef SIMPLE_TOKENIZER_INCLUDED
#define SIMPLE_TOKENIZER_INCLUDED


#include "lex_ident.h"
#include "scan_char.h"

/**
  A tokenizer for an ASCII7 input
*/
class Simple_tokenizer
{
protected:
  const char *m_ptr;
  const char *m_end;
public:
  Simple_tokenizer(const LEX_CSTRING &str)
   :m_ptr(str.str), m_end(str.str + str.length)
  { }
  Simple_tokenizer(const char *str, size_t length)
   :m_ptr(str), m_end(str + length)
  { }
  const char *ptr() const
  {
    return m_ptr;
  }
  bool eof() const
  {
    return m_ptr >= m_end;
  }
  bool is_space() const
  {
    return m_ptr[0] == ' ' || m_ptr[0] == '\r' || m_ptr[0] == '\n';
  }
  void get_spaces()
  {
    for ( ; !eof(); m_ptr++)
    {
      if (!is_space())
        break;
    }
  }
  bool is_ident_start(char ch) const
  {
    return (ch >= 'a' && ch <= 'z') ||
           (ch >= 'A' && ch <= 'Z') ||
           ch == '_';
  }
  bool is_ident_body(char ch) const
  {
    return is_ident_start(ch) ||
           (ch >= '0' && ch <= '9');
  }
  bool is_ident_start() const
  {
    return !eof() && is_ident_start(*m_ptr);
  }
  bool is_ident_body() const
  {
    return !eof() && is_ident_body(*m_ptr);
  }
  LEX_CSTRING get_ident()
  {
    get_spaces();
    if (!is_ident_start())
      return {m_ptr,0};
    const char *start= m_ptr++;
    for ( ; is_ident_body(); m_ptr++)
    { }
    LEX_CSTRING res= {start, (size_t) (m_ptr - start)};
    return res;
  }
  bool get_char(char ch)
  {
    get_spaces();
    if (eof() || *m_ptr != ch)
      return true;
    m_ptr++;
    return false;
  }
};


/**
  A tokenizer for a character set aware input.
*/
class Extended_string_tokenizer: public Simple_tokenizer
{
protected:

  CHARSET_INFO *m_cs;

  class Token_metadata
  {
  public:
    bool m_extended_chars:1;
    bool m_double_quotes:1;
    Token_metadata()
     :m_extended_chars(false), m_double_quotes(false)
    { }
  };

  class Token_with_metadata: public Lex_cstring,
                             public Token_metadata
  {
  public:
    Token_with_metadata()
    { }
    Token_with_metadata(const char *str, size_t length,
                        const Token_metadata &metadata)
     :Lex_cstring(str, length), Token_metadata(metadata)
    { }
    Token_with_metadata(const char *str)
     :Lex_cstring(str, (size_t) 0), Token_metadata()
    { }
  };

  /*
    Get a non-delimited identifier for a 8-bit character set
  */
  Token_with_metadata get_ident_8bit(const char *str, const char *end) const
  {
    DBUG_ASSERT(m_cs->mbmaxlen == 1);
    Token_with_metadata res(str);
    for ( ; str < end && m_cs->ident_map[(uchar) *str]; str++, res.length++)
    {
      if (*str & 0x80)
        res.m_extended_chars= true;
    }
    return res;
  }

  /*
    Get a non-identifier for a multi-byte character set
  */
  Token_with_metadata get_ident_mb(const char *str, const char *end) const
  {
    DBUG_ASSERT(m_cs->mbmaxlen > 1);
    Token_with_metadata res(str);
    for ( ; m_cs->ident_map[(uchar) *str]; )
    {
      int char_length= m_cs->charlen(str, end);
      if (char_length <= 0)
        break;
      str+= char_length;
      res.length+= (size_t) char_length;
      res.m_extended_chars|= char_length > 1;
    }
    return res;
  }

  /*
    Get a non-delimited identifier
  */
  Token_with_metadata get_ident(const char *str, const char *end)
  {
    return m_cs->mbmaxlen == 1 ? get_ident_8bit(str, end) :
                                 get_ident_mb(str, end);
  }

  /*
    Get a quoted string or a quoted identifier.
    The quote character is determined by the current head character
    pointed by str. The result is returned together with the left
    and the right quotes.
  */
  Token_with_metadata get_quoted_string(const char *str, const char *end)
  {
    Token_with_metadata res(str);
    const Scan_char quote(m_cs, str, end);
    if (quote.length() <= 0)
    {
      /*
        Could not get the left quote character:
        - the end of the input reached, or
        - a bad byte sequence found.
        Return a null token to signal the error to the caller.
      */
      return Token_with_metadata();
    }
    str+= quote.length();
    res.length+= (size_t) quote.length();

    for ( ; ; )
    {
      const Scan_char ch(m_cs, str, end);
      if (ch.length() <= 0)
      {
        /*
          Could not find the right quote character:
          - the end of the input reached before the quote was not found, or
          - a bad byte sequences found
          Return a null token to signal the error to the caller.
        */
        return Token_with_metadata();
      }
      str+= ch.length();
      res.length+= (size_t) ch.length();
      if (quote.eq(ch))
      {
        if (quote.eq_safe(Scan_char(m_cs, str, end)))
        {
          /*
            Two quotes in a row found:
            - `a``b`
            - "a""b"
          */
          str+= quote.length();
          res.length+= (size_t) quote.length();
          res.m_extended_chars|= quote.length() > 1;
          res.m_double_quotes= true;
          continue;
        }
        return res; // The right quote found
      }
      res.m_extended_chars|= ch.length() > 1;
    }
    return res;
  }

public:
  Extended_string_tokenizer(CHARSET_INFO *cs, const LEX_CSTRING &str)
   :Simple_tokenizer(str),
    m_cs(cs)
  { }

  // Skip all leading spaces
  void get_spaces()
  {
    for ( ; !eof(); m_ptr++)
    {
      if (!my_isspace(m_cs, *m_ptr))
        break;
    }
  }

  /*
    Get a non-delimited identifier.
    Can return an empty token if the head character is not an identifier
    character.
  */
  Token_with_metadata get_ident()
  {
    const Token_with_metadata tok= get_ident(m_ptr, m_end);
    m_ptr+= tok.length;
    return tok;
  }

  /*
    Get a quoted string or a quoted identifier.
    Can return a null token if there were errors
    (e.g. unexpected end of the input, bad byte sequence).
  */
  Token_with_metadata get_quoted_string()
  {
    const Token_with_metadata tok= get_quoted_string(m_ptr, m_end);
    m_ptr+= tok.length;
    return tok;
  }

};


#endif // SIMPLE_TOKENIZER_INCLUDED
