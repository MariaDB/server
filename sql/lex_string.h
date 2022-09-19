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

#include "sql_alloc.h"

typedef struct st_mysql_const_lex_string LEX_CSTRING;


class Lex_cstring : public LEX_CSTRING, public Sql_alloc
{
  public:
  constexpr Lex_cstring()
   :LEX_CSTRING({NULL, 0})
  { }
  constexpr Lex_cstring(const LEX_CSTRING &str)
   :LEX_CSTRING(str)
  { }
  constexpr Lex_cstring(const char *_str, size_t _len)
   :LEX_CSTRING({_str, _len})
  { }
  Lex_cstring(const char *_str)
  {
    str= _str;
    length= strlen(str);
  }
  Lex_cstring(const char *start, const char *end)
  {
    DBUG_ASSERT(start <= end);
    str= start;
    length= end - start;
  }

  bool bin_eq(const LEX_CSTRING &rhs) const
  {
    return length == rhs.length && !memcmp(str, rhs.str, length);
  }

  bool strdup(MEM_ROOT *mem_root, const char *_str, size_t _len)
  {
    if (!_str)
    {
      str= NULL;
      length= 0;
      return false;
    }
    length= _len;
    str= strmake_root(mem_root, _str, length);
    return !str;
  }
  bool strdup(MEM_ROOT *mem_root, const char *_str)
  {
    if (!_str)
    {
      str= NULL;
      length= 0;
      return false;
    }
    return strdup(mem_root, _str, strlen(_str));
  }
  bool strdup(MEM_ROOT *mem_root, const Lex_cstring &_str)
  {
    if (!_str.str)
    {
      str= NULL;
      length= 0;
      return false;
    }
    return strdup(mem_root, _str.str, _str.length);;
  }
  void set(const char *_str, size_t _len)
  {
    str= _str;
    length= _len;
  }

  /*
    Trim left white spaces.
    Assumes that there are no multi-bytes characters
    that can be considered white-space.
  */
  Lex_cstring ltrim_whitespace(CHARSET_INFO *cs) const
  {
    DBUG_ASSERT(cs->mbminlen == 1);
    Lex_cstring str= *this;
    while (str.length > 0 && my_isspace(cs, str.str[0]))
    {
      str.length--;
      str.str++;
    }
    return str;
  }

  /*
    Trim right white spaces.
    Assumes that there are no multi-bytes characters
    that can be considered white-space.
    Also, assumes that the character set supports backward space parsing.
  */
  Lex_cstring rtrim_whitespace(CHARSET_INFO *cs) const
  {
    DBUG_ASSERT(cs->mbminlen == 1);
    Lex_cstring str= *this;
    while (str.length > 0 && my_isspace(cs, str.str[str.length - 1]))
    {
      str.length --;
    }
    return str;
  }

  /*
    Trim all spaces.
  */
  Lex_cstring trim_whitespace(CHARSET_INFO *cs) const
  {
    return ltrim_whitespace(cs).rtrim_whitespace(cs);
  }

  /*
    Trim all spaces and return the length of the leading space sequence.
  */
  Lex_cstring trim_whitespace(CHARSET_INFO *cs, size_t *prefix_length) const
  {
    Lex_cstring tmp= Lex_cstring(*this).ltrim_whitespace(cs);
    if (prefix_length)
      *prefix_length= tmp.str - str;
    return tmp.rtrim_whitespace(cs);
  }

  /*
    Return the "n" leftmost bytes if this[0] is longer than "n" bytes,
    or return this[0] itself otherwise.
  */
  Lex_cstring left(size_t n) const
  {
    return Lex_cstring(str, MY_MIN(length, n));
  }
  /*
    If this[0] is shorter than "pos" bytes, then return an empty string.
    Otherwise, return a substring of this[0] starting from
    the byte position "pos" until the end.
  */
  Lex_cstring substr(size_t pos) const
  {
    return length <= pos ? Lex_cstring(str + length, (size_t) 0) :
                           Lex_cstring(str + pos, length - pos);
  }
  // Check if a prefix of this[0] is equal to "rhs".
  bool starts_with(const LEX_CSTRING &rhs) const
  {
    DBUG_ASSERT(str);
    DBUG_ASSERT(rhs.str);
    return length >= rhs.length && !memcmp(str, rhs.str, rhs.length);
  }
  Lex_cstring print() const
  {
    return str ? *this : "(NULL)";
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
  bool is_empty() const
  {
    DBUG_ASSERT(!length || str);
    return length == 0;
  }
};


class Lex_cstring_strlen: public Lex_cstring
{
public:
  explicit Lex_cstring_strlen(const char *from)
   :Lex_cstring(from, from ? strlen(from) : 0)
  { }
};

class Scope_malloc
{
  void * addr;

public:
  template <class PTR>
  Scope_malloc(PTR alloced) : addr((void *)alloced)
  {
    DBUG_ASSERT(addr);
  }
  template <class PTR>
  Scope_malloc(PTR &assign, size_t Size, myf MyFlags= 0)
  {
    addr= my_malloc(PSI_NOT_INSTRUMENTED, Size, MyFlags);
    assign= (PTR) addr;
  }
  ~Scope_malloc()
  {
    my_free(addr);
  }
};

/* Functions to compare if two lex strings are equal */

/*
  This should be replaced by C++20 "spaceship" operator (<=>)
*/
template <typename T>
inline int
cmp_any(T a, T b)
{
  if (a < b)
    return -1;
  if (b < a)
    return 1;
  return 0;
}

/*
  Compare to LEX_CSTRING's and return 0 if equal
*/

static inline bool cmp(const LEX_CSTRING *a, const LEX_CSTRING *b)
{
  return a->length != b->length ||
    (a->length && memcmp(a->str, b->str, a->length));
}
static inline bool cmp(const LEX_CSTRING a, const LEX_CSTRING b)
{
  return a.length != b.length || (a.length && memcmp(a.str, b.str, a.length));
}
static inline bool cmp_prefix(const LEX_CSTRING a, const LEX_CSTRING b)
{
  int ret= cmp_any(a.length, b.length);
  if (!a.length || !b.length)
    return ret;

  return memcmp(a.str, b.str, ret > 0 ? b.length : a.length);
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
