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

#ifndef LEX_CHARSET_COLLATIONS_INCLUDED
#define LEX_CHARSET_COLLATIONS_INCLUDED

#include "sql_used.h"

struct Charset_collation_map_st
{
public:

  struct Elem_st
  {
  protected:
    CHARSET_INFO *m_from; // From a character set
    CHARSET_INFO *m_to;   // To a collation
    static size_t print_lex_string(char *dst, const LEX_CSTRING &str)
    {
      memcpy(dst, str.str, str.length);
      return str.length;
    }
  public:
    /*
      Size in text format: 'utf8mb4=utf8mb4_unicode_ai_ci'
    */
    static constexpr size_t text_size_max()
    {
       return MY_CS_CHARACTER_SET_NAME_SIZE + 1 +
              MY_CS_COLLATION_NAME_SIZE;
    }
    CHARSET_INFO *from() const
    {
      return m_from;
    }
    CHARSET_INFO *to() const
    {
      return m_to;
    }
    void set_to(CHARSET_INFO *cl)
    {
      m_to= cl;
    }
    size_t print(char *dst) const
    {
      const char *dst0= dst;
      dst+= print_lex_string(dst, m_from->cs_name);
      *dst++= '=';
      dst+= print_lex_string(dst, m_to->coll_name);
      return (size_t) (dst - dst0);
    }
    int cmp_by_charset_id(const Elem_st &rhs) const
    {
      return m_from->number < rhs.m_from->number ? -1 :
             m_from->number > rhs.m_from->number ? +1 : 0;
    }
  };
  class Elem: public Elem_st
  {
  public:
    Elem(CHARSET_INFO *from, CHARSET_INFO *to)
    {
      m_from= from;
      m_to= to;
    }
  };
protected:
  Elem_st m_element[8]; // Should be enough for now
  uint m_count;
  uint m_version;

  static int cmp_by_charset_id(const void *a, const void *b)
  {
    return static_cast<const Elem_st*>(a)->
             cmp_by_charset_id(*static_cast<const Elem_st*>(b));
  }

  void sort()
  {
    qsort(m_element, m_count, sizeof(Elem_st), cmp_by_charset_id);
  }

  const Elem_st *find_elem_by_charset_id(uint id) const
  {
    if (!m_count)
      return NULL;
    int first= 0, last= ((int) m_count) - 1;
    for ( ; first <= last; )
    {
      const int middle= (first + last) / 2;
      DBUG_ASSERT(middle >= 0);
      DBUG_ASSERT(middle < (int) m_count);
      const uint middle_id= m_element[middle].from()->number;
      if (middle_id == id)
        return &m_element[middle];
      if (middle_id < id)
        first= middle + 1;
      else
        last= middle - 1;
    }
    return NULL;
  }

  bool insert(const Elem_st &elem)
  {
    DBUG_ASSERT(elem.from()->state & MY_CS_PRIMARY);
    if (m_count >= array_elements(m_element))
      return true;
    m_element[m_count]= elem;
    m_count++;
    sort();
    return false;
  }

  bool insert_or_replace(const Elem_st &elem)
  {
    DBUG_ASSERT(elem.from()->state & MY_CS_PRIMARY);
    const Elem_st *found= find_elem_by_charset_id(elem.from()->number);
    if (found)
    {
      const_cast<Elem_st*>(found)->set_to(elem.to());
      return false;
    }
    return insert(elem);
  }

public:
  void init()
  {
    m_count= 0;
    m_version= 0;
  }
  uint count() const
  {
    return m_count;
  }
  uint version() const
  {
    return m_version;
  }
  void set(const Charset_collation_map_st &rhs, uint version_increment)
  {
    uint version= m_version;
    *this= rhs;
    m_version= version + version_increment;
  }
  const Elem_st & operator[](uint pos) const
  {
    DBUG_ASSERT(pos < m_count);
    return m_element[pos];
  }
  bool insert_or_replace(const class Lex_exact_charset &cs,
                         const class Lex_extended_collation &cl,
                         bool error_on_conflicting_duplicate);
  bool insert_or_replace(const LEX_CSTRING &cs,
                         const LEX_CSTRING &cl,
                         bool error_on_conflicting_duplicate,
                         myf utf8_flag);
  CHARSET_INFO *get_collation_for_charset(Sql_used *used,
                                          CHARSET_INFO *cs) const
  {
    DBUG_ASSERT(cs->state & MY_CS_PRIMARY);
    const Elem_st *elem= find_elem_by_charset_id(cs->number);
    used->used|= Sql_used::CHARACTER_SET_COLLATIONS_USED;
    if (elem)
      return elem->to();
    return cs;
  }
  size_t text_format_nbytes_needed() const
  {
    return (Elem_st::text_size_max() + 1/* for ',' */) * m_count;
  }
  size_t print(char *dst, size_t nbytes_available) const
  {
    const char *dst0= dst;
    const char *end= dst + nbytes_available;
    for (uint i= 0; i < m_count; i++)
    {
      if (Elem_st::text_size_max() + 1/* for ',' */ > (size_t) (end - dst))
        break;
      if (i > 0)
        *dst++= ',';
      dst+= m_element[i].print(dst);
    }
    return dst - dst0;
  }
  static constexpr size_t binary_size_max()
  {
    return 1/*count*/ + 4 * array_elements(m_element);
  }
  size_t to_binary(char *dst) const
  {
    const char *dst0= dst;
    *dst++= (char) (uchar) m_count;
    for (uint i= 0; i < m_count; i++)
    {
      int2store(dst, (uint16) m_element[i].from()->number);
      dst+= 2;
      int2store(dst, (uint16) m_element[i].to()->number);
      dst+= 2;
    }
    return (size_t) (dst - dst0);
  }
  size_t from_binary(const char *src, size_t srclen)
  {
    const char *src0= src;
    init();
    if (!srclen)
      return 0; // Empty
    uint count= (uchar) *src++;
    if (srclen < 1 + 4 * count)
      return 0;
    for (uint i= 0; i < count; i++, src+= 4)
    {
      CHARSET_INFO *cs, *cl;
      if (!(cs= get_charset(uint2korr(src), MYF(0))) ||
          !(cl= get_charset(uint2korr(src + 2), MYF(0))))
      {
        /*
          Unpacking from binary format happens on the slave side.
          If for some reasons the slave does not know about a
          character set or a collation, just skip the pair here.
          This pair might not even be needed.
        */
        continue;
      }
      insert_or_replace(Elem(cs, cl));
    }
    return src - src0;
  }
  bool from_text(const LEX_CSTRING &str, myf utf8_flag);
};


#endif // LEX_CHARSET_COLLATIONS_INCLUDED
