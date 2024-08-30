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


class Simple_tokenizer
{
  const char *m_ptr;
  const char *m_end;
public:
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
  void get_spaces()
  {
    for ( ; !eof(); m_ptr++)
    {
      if (m_ptr[0] != ' ')
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


#endif // SIMPLE_TOKENIZER_INCLUDED
