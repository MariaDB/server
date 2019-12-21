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


#ifndef LEX_STRING_INCLUDED
#define LEX_STRING_INCLUDED


typedef struct st_mysql_const_lex_string LEX_CSTRING;


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
  Lex_cstring(const char *start, const char *end)
  {
    DBUG_ASSERT(start <= end);
    str= start;
    length= end - start;
  }
  Lex_cstring(const LEX_CSTRING &src)
  {
    str= src.str;
    length= src.length;
  }
  void set(const char *_str, size_t _len)
  {
    str= _str;
    length= _len;
  }
  Lex_cstring *strdup_root(MEM_ROOT &mem_root)
  {
    Lex_cstring *dst=
        (Lex_cstring *) alloc_root(&mem_root, sizeof(Lex_cstring));
    if (!dst)
      return NULL;
    if (!str)
    {
      dst->str= NULL;
      dst->length= 0;
      return dst;
    }
    dst->str= (const char *) memdup_root(&mem_root, str, length + 1);
    if (!dst->str)
      return NULL;
    dst->length= length;
    return dst;
  }
  int cmp(const Lex_cstring& rhs) const
  {
    if (length < rhs.length)
      return -1;
    if (length > rhs.length)
      return 1;
    return memcmp(str, rhs.str, length);
  }
  int cmp(const char* rhs) const
  {
    if (!str)
      return -1;
    if (!rhs)
      return 1;
    return strcmp(str, rhs);
  }
};

struct Lex_cstring_lt
{
  bool operator() (const Lex_cstring &lhs, const Lex_cstring &rhs) const
  {
    return lhs.cmp(rhs) < 0;
  }

};

class Lex_cstring_strlen: public Lex_cstring
{
public:
  Lex_cstring_strlen(const char *from)
   :Lex_cstring(from, from ? strlen(from) : 0)
  { }
};


/* Functions to compare if two lex strings are equal */

static inline bool lex_string_cmp(CHARSET_INFO *charset, const LEX_CSTRING *a,
                                  const LEX_CSTRING *b)
{
  return my_strcasecmp(charset, a->str, b->str);
}

/*
  Compare to LEX_CSTRING's and return 0 if equal
*/

static inline bool cmp(const LEX_CSTRING *a, const LEX_CSTRING *b)
{
  return (a->length != b->length ||
          memcmp(a->str, b->str, a->length));
}
static inline bool cmp(const LEX_CSTRING a, const LEX_CSTRING b)
{
  return a.length != b.length || memcmp(a.str, b.str, a.length);
}

/*
  Compare if two LEX_CSTRING are equal. Assumption is that
  character set is ASCII (like for plugin names)
*/

static inline bool lex_string_eq(const LEX_CSTRING *a, const LEX_CSTRING *b)
{
  if (a->length != b->length)
    return 0;                                   /* Different */
  return strcasecmp(a->str, b->str) == 0;
}

/*
  To be used when calling lex_string_eq with STRING_WITH_LEN() as second
  argument
*/

static inline bool lex_string_eq(const LEX_CSTRING *a, const char *b, size_t b_length)
{
  if (a->length != b_length)
    return 0;                                   /* Different */
  return strcasecmp(a->str, b) == 0;
}

inline
LEX_CSTRING *make_clex_string(MEM_ROOT *mem_root, const char* str, size_t length)
{
  LEX_CSTRING *lex_str;
  char *tmp;
  if (unlikely(!(lex_str= (LEX_CSTRING *)alloc_root(mem_root,
                                                    sizeof(LEX_CSTRING) +
                                                    (str ? (length + 1) : 0)))))
    return 0;
  if (str)
  {
    tmp= (char*) (lex_str+1);
    lex_str->str= tmp;
    memcpy(tmp, str, length);
    tmp[length]= 0;
    lex_str->length= length;
  }
  else
  {
    DBUG_ASSERT(!length);
    lex_str->str= NULL;
    lex_str->length= 0;
  }
  return lex_str;
}

inline
LEX_CSTRING *make_clex_string(MEM_ROOT *mem_root, const LEX_CSTRING from)
{
  return make_clex_string(mem_root, from.str, from.length);
}

#endif /* LEX_STRING_INCLUDED */
