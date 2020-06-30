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
#include "mysqld.h"


typedef struct st_mysql_const_lex_string LEX_CSTRING;


class Lex_cstring : public LEX_CSTRING, public Sql_alloc
{
  public:
  Lex_cstring()
  {
    str= NULL;
    length= 0;
  }
  Lex_cstring(const LEX_CSTRING &str)
  {
    LEX_CSTRING::operator=(str);
  }
  Lex_cstring(const char *_str, size_t _len)
  {
    str= _str;
    length= _len;
  }
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
};


class Lex_cstring_strlen: public Lex_cstring
{
public:
  Lex_cstring_strlen(const char *from)
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
static inline int cmp_ident(const LEX_CSTRING a, const LEX_CSTRING b)
{
  return my_strcasecmp(system_charset_info, a.str, b.str);
}
static inline int cmp_table(const LEX_CSTRING a, const LEX_CSTRING b)
{
  return my_strcasecmp(table_alias_charset, a.str, b.str);
}
struct Lex_ident_lt
{
  bool operator() (const Lex_cstring &lhs, const Lex_cstring &rhs) const
  {
    return cmp_ident(lhs, rhs) < 0;
  }
};


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
