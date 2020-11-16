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
  Lex_cstring(const LEX_CSTRING &str)
  {
    LEX_CSTRING::operator=(str);
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
  void set(const char *_str, size_t _len)
  {
    str= _str;
    length= _len;
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
  return a->length != b->length ||
    (a->length && memcmp(a->str, b->str, a->length));
}
static inline bool cmp(const LEX_CSTRING a, const LEX_CSTRING b)
{
  return a.length != b.length || (a.length && memcmp(a.str, b.str, a.length));
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

#endif /* LEX_STRING_INCLUDED */
