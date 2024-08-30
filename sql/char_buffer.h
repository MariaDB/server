#ifndef CHAR_BUFFER_INCLUDED
#define CHAR_BUFFER_INCLUDED
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


/*
  A string buffer with length.
  This template class is useful to store things like database, table names,
  whose maximum length a small fixed known value. Mainly to be used as
  stack variables to store temporary values.

  Can store exact string copies or casefolded string copies.
  The stored value is returned as a LEX_CSTRING.
*/
template<size_t buff_sz>
class CharBuffer
{
  char m_buff[buff_sz + 1 /* one extra byte for '\0' */];
  size_t m_length;
  bool is_sane() const
  {
    return m_length <= buff_sz; // One byte is still left for '\0'
  }
  bool buffer_overlaps(const LEX_CSTRING &str) const
  {
    return str.str + str.length >= m_buff && str.str <= m_buff + sizeof(m_buff);
  }
public:
  constexpr size_t max_data_size() const
  {
    return buff_sz; // The maximum data size, without the trailing '\0' byte.
  }
  size_t available_size() const
  {
    DBUG_ASSERT(is_sane());
    return buff_sz - m_length;
  }
  CharBuffer()
   :m_length(0)
  {
    m_buff[0]= '\0';
  }
  CharBuffer<buff_sz> & copy(const LEX_CSTRING &str)
  {
    DBUG_ASSERT(!buffer_overlaps(str));
    m_length= MY_MIN(buff_sz, str.length);
    memcpy(m_buff, str.str, m_length);
    m_buff[m_length]= '\0';
    return *this;
  }
  CharBuffer<buff_sz> & copy_casedn(CHARSET_INFO *cs, const LEX_CSTRING &str)
  {
    DBUG_ASSERT(!buffer_overlaps(str));
    m_length= cs->cset->casedn(cs, str.str, str.length, m_buff, buff_sz);
    /*
      casedn() never writes outsize of buff_sz (unless a bug in casedn()),
      so it's safe to write '\0' at the position m_length:
    */
    DBUG_ASSERT(is_sane());
    m_buff[m_length]= '\0';
    return *this;
  }
  CharBuffer<buff_sz> & copy_caseup(CHARSET_INFO *cs, const LEX_CSTRING &str)
  {
    DBUG_ASSERT(!buffer_overlaps(str));
    m_length= cs->cset->caseup(cs, str.str, str.length, m_buff, buff_sz);
    DBUG_ASSERT(is_sane());
    m_buff[m_length]= '\0'; // See comments in copy_casedn()
    return *this;
  }
  CharBuffer<buff_sz> & copy_casedn(CHARSET_INFO *cs, const LEX_CSTRING &str,
                                    bool casedn)
  {
    casedn ? copy_casedn(cs, str) : copy(str);
    return *this;
  }

  // Append one character
  CharBuffer<buff_sz> & append_char(char ch)
  {
    DBUG_ASSERT(is_sane());
    if (available_size())
    {
      m_buff[m_length++]= ch;
      m_buff[m_length]= '\0';
    }
    DBUG_ASSERT(is_sane());
    return *this;
  }

  // Append a string
  CharBuffer<buff_sz> & append(const LEX_CSTRING &str)
  {
    DBUG_ASSERT(is_sane());
    DBUG_ASSERT(!buffer_overlaps(str));
    size_t len= MY_MIN(available_size(), str.length);
    memcpy(m_buff + m_length, str.str, len);
    m_length+= len;
    DBUG_ASSERT(is_sane());
    m_buff[m_length]= '\0';
    return *this;
  }

  // Append a string with casedn conversion
  CharBuffer<buff_sz> & append_casedn(CHARSET_INFO *cs, const LEX_CSTRING &str)
  {
    DBUG_ASSERT(is_sane());
    DBUG_ASSERT(!buffer_overlaps(str));
    size_t casedn_length= cs->casedn(str.str, str.length,
                                     m_buff + m_length, available_size());
    m_length+= casedn_length;
    DBUG_ASSERT(is_sane());
    m_buff[m_length]= '\0';
    return *this;
  }

  CharBuffer<buff_sz> & append_opt_casedn(CHARSET_INFO *cs,
                                          const LEX_CSTRING &str,
                                          bool casedn)
  {
    return casedn ? append_casedn(cs, str) : append(str);
  }

  // Append a string with caseup conversion
  CharBuffer<buff_sz> & append_caseup(CHARSET_INFO *cs, const LEX_CSTRING &str)
  {
    DBUG_ASSERT(is_sane());
    DBUG_ASSERT(!buffer_overlaps(str));
    size_t casedn_length= cs->caseup(str.str, str.length,
                                     m_buff + m_length, available_size());
    m_length+= casedn_length;
    DBUG_ASSERT(is_sane());
    m_buff[m_length]= '\0';
    return *this;
  }

  CharBuffer<buff_sz> & truncate(size_t length)
  {
    DBUG_ASSERT(is_sane());
    if (m_length > length)
    {
      m_length= length;
      m_buff[m_length]= '\0';
      DBUG_ASSERT(is_sane());
    }
    return *this;
  }

  LEX_CSTRING to_lex_cstring() const
  {
    return LEX_CSTRING{m_buff, m_length};
  }

  const char *ptr() const { return m_buff; }
  size_t length() const { return m_length; }
  const char *end() const { return m_buff + m_length; }

};

#endif // CHAR_BUFFER_INCLUDED
