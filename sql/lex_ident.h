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

#include "my_global.h"
#include "m_ctype.h"
#include "char_buffer.h"
#include "lex_string.h"
#include "my_sys.h"

extern MYSQL_PLUGIN_IMPORT CHARSET_INFO *table_alias_charset;


/*
  LEX_CSTRING with comparison semantics.
*/

// db and table names: case sensitive (or insensitive) in table_alias_charset
struct Compare_table_names
{
  CHARSET_INFO *charset_info() const
  {
    return table_alias_charset;
  }
};


// case insensitive identifiers
struct Compare_ident_ci
{
  CHARSET_INFO *charset_info() const
  {
    return &my_charset_utf8mb3_general1400_as_ci;
  }
};


/*
  Possible identifier values:
    1.  {ptr==NULL,length==0} is valid and means "NULL identifier".
    2a. {ptr<>NULL,length==0} means "empty identifier".
    2b. {ptr<>NULL,length>0}  means "not empty identifier.
  In case of 2a and 2b, ptr must be a '\0'-terminated string.

  Comparison operands passed to streq() are not required to be 0-terminated.

  Debug validation is done during comparison time:
  - inside methods of this class
  - inside st_charset_info::streq() in include/m_ctype.h
  The caller must make sure to maintain the object in the valid state,
  as well as provide valid LEX_CSTRING instances for comparison.

  For better code stability, the Lex_cstring base should eventually be
  encapsulated, so the object debug validation is done at constructor
  time rather than at comparison time.
*/

template <class Compare>
class Lex_ident : public Lex_cstring
{
protected:
  // Make sure the object is valid
  bool is_valid_ident() const
  {
    // NULL identifier, or 0-terminated identifier
    return (str == NULL && length == 0) || str[length] == 0;
  }
public:

  constexpr Lex_ident() = default;
  explicit constexpr Lex_ident(const LEX_CSTRING &str)
   :Lex_cstring(str)
  { }
  constexpr Lex_ident(const char *str, size_t len)
   :Lex_cstring(str, len)
  { }
  Lex_ident(const char *start, const char *end)
   :Lex_ident(start, end)
  { }
  Lex_ident(const Lex_cstring_strlen &str)
   :Lex_cstring(str)
  { }
  explicit operator bool () const
  {
    return str != NULL;
  }
  static CHARSET_INFO *charset_info()
  {
    return Compare().charset_info();
  }
  /*
    Compare two not necessarily 0-terminated LEX_CSTRING instances.
    Both sides can also be valid NULL identifiers.
  */
  static bool streq(const LEX_CSTRING &a, const LEX_CSTRING &b)
  {
    return Compare().charset_info()->streq(a, b);
  }
  /*
    Compare the object to a not necessarily 0-terminated LEX_CSTRING.
    Both "this" and rhs can also be NULL identifiers.
  */
  bool streq(const LEX_CSTRING &rhs) const
  {
    DBUG_ASSERT(is_valid_ident());
    return Compare().charset_info()->streq(*this, rhs);
  }
  /*
    Compare two objects.
    Both "this" and rhs can also be NULL identifiers.
  */
  bool streq(const Lex_ident &b) const
  {
    DBUG_ASSERT(is_valid_ident());
    DBUG_ASSERT(b.is_valid_ident());
    return Compare().charset_info()->streq(*this, b);
  }
};

extern MYSQL_PLUGIN_IMPORT CHARSET_INFO *table_alias_charset;

/*
  Identifiers for the database objects stored on disk,
  e.g. databases, tables, triggers.
  Their sensitivity depends on table_alias_charset, i.e. on
  - the datadir filesystem case sensitivity, and
  - the value of --lower-case-table-names
*/
class Lex_ident_fs: public Lex_ident<Compare_table_names>
{
protected:
  static bool check_body(const char *name, size_t length,
                         bool disallow_path_chars);
public:
  using Lex_ident::Lex_ident;
#ifndef DBUG_OFF
  bool is_in_lower_case() const;
  bool ok_for_lower_case_names() const;
#endif
  bool check_db_name_quick() const
  {
    return !length || length > NAME_LEN || str[length-1] == ' ';
  }
};


/**
  A valid database name identifier,
  checked with check_name().
  It's not known if it was lower-cased or is
  in the user typed way.
*/
class Lex_ident_db: public Lex_ident_fs
{
public:
  bool is_null() const
  {
    return length == 0 && str == NULL;
  }
  // {empty_c_string,0} is used by derived tables
  bool is_empty() const
  {
    return length == 0 && str != NULL;
  }
  static bool check_name(const LEX_CSTRING &str);
  static bool check_name_with_error(const LEX_CSTRING &str);
public:
  constexpr Lex_ident_db()
   :Lex_ident_fs()
  { }
  explicit Lex_ident_db(const LEX_CSTRING &str)
   :Lex_ident_fs(str)
  {
    DBUG_SLOW_ASSERT(is_null() || is_empty() || !check_name(*this));
  }
  Lex_ident_db(const char *str, size_t length)
   :Lex_ident_fs(str, length)
  {
    DBUG_SLOW_ASSERT(is_null() || is_empty() || !check_name(*this));
  }
};


/**
  A normalized database name:
  - checked with Lex_ident_db::check_name()
  - lower-cased if lower_case_table_names>0
*/
class Lex_ident_db_normalized: public Lex_ident_db
{
public:
  Lex_ident_db_normalized()
  { }
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


class Lex_ident_table: public Lex_ident_fs
{
public:
  static bool check_name(const LEX_CSTRING &str, bool check_for_path_chars);
public:
  using Lex_ident_fs::Lex_ident_fs;
};


class Lex_ident_trigger: public Lex_ident_fs
{
public:
  using Lex_ident_fs::Lex_ident_fs;
};


/*
  A case insensitive identifier.
*/
class Lex_ident_ci: public Lex_ident<Compare_ident_ci>
{
public:
  using Lex_ident::Lex_ident;
};


class Lex_ident_column: public Lex_ident_ci
{
public:
  using Lex_ident_ci::Lex_ident_ci;
};


class Lex_ident_sys_var: public Lex_ident_ci
{
public:
  using Lex_ident_ci::Lex_ident_ci;
};


class Lex_ident_user_var: public Lex_ident_ci
{
public:
  using Lex_ident_ci::Lex_ident_ci;
};


class Lex_ident_ps: public Lex_ident_ci
{
public:
  using Lex_ident_ci::Lex_ident_ci;
};


class Lex_ident_i_s_db: public Lex_ident_ci
{
public:
  using Lex_ident_ci::Lex_ident_ci;
};


class Lex_ident_i_s_table: public Lex_ident_ci
{
public:
  using Lex_ident_ci::Lex_ident_ci;
};


class Lex_ident_window: public Lex_ident_ci
{
public:
  using Lex_ident_ci::Lex_ident_ci;
};


class Lex_ident_routine: public Lex_ident_ci
{
public:
  static bool check_name_with_error(const LEX_CSTRING &name);
public:
  Lex_ident_routine()
  { }
  explicit Lex_ident_routine(const LEX_CSTRING &name)
   :Lex_ident_ci(name)
  {
    DBUG_ASSERT(!check_name_with_error(name));
  }
};


class Lex_ident_partition: public Lex_ident_ci
{
public:
  using Lex_ident_ci::Lex_ident_ci;
};


class Lex_ident_with_element: public Lex_ident_ci
{
public:
  using Lex_ident_ci::Lex_ident_ci;
};


class Lex_ident_rpl_filter: public Lex_ident_ci
{
public:
  using Lex_ident_ci::Lex_ident_ci;
};


class Lex_ident_master_info: public Lex_ident_ci
{
public:
  using Lex_ident_ci::Lex_ident_ci;
};


class Lex_ident_host: public Lex_ident_ci
{
public:
  using Lex_ident_ci::Lex_ident_ci;
};


class Lex_ident_locale: public Lex_ident_ci
{
public:
  using Lex_ident_ci::Lex_ident_ci;
};


class Lex_ident_plugin: public Lex_ident_ci
{
public:
  using Lex_ident_ci::Lex_ident_ci;
};


class Lex_ident_engine: public Lex_ident_ci
{
public:
  using Lex_ident_ci::Lex_ident_ci;
};


class Lex_ident_server: public Lex_ident_ci
{
public:
  using Lex_ident_ci::Lex_ident_ci;
};


class Lex_ident_savepoint: public Lex_ident_ci
{
public:
  using Lex_ident_ci::Lex_ident_ci;
};


class Lex_ident_charset: public Lex_ident_ci
{
public:
  using Lex_ident_ci::Lex_ident_ci;
};



static inline constexpr
Lex_ident_table operator""_Lex_ident_table(const char *str, size_t length)
{
  return Lex_ident_table(str, length);
}


static inline constexpr
Lex_ident_column operator""_Lex_ident_column(const char *str, size_t length)
{
  return Lex_ident_column(str, length);
}

static inline constexpr
Lex_ident_i_s_table operator""_Lex_ident_i_s_table(const char *str,
                                                   size_t length)
{
  return Lex_ident_i_s_table(str, length);
}

static inline constexpr
Lex_ident_engine operator""_Lex_ident_engine(const char *str, size_t length)
{
  return Lex_ident_engine(str, length);
}


static inline constexpr
Lex_ident_locale operator""_Lex_ident_locale(const char *str, size_t length)
{
  return Lex_ident_locale(str, length);
}


static inline constexpr
Lex_ident_charset operator""_Lex_ident_charset(const char *str, size_t length)
{
  return Lex_ident_charset(str, length);
}


static inline constexpr
Lex_ident_plugin operator""_Lex_ident_plugin(const char *str, size_t length)
{
  return Lex_ident_plugin(str, length);
}


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
  Lex_ident_db::check_name().

  Note, the database name passed to the constructor can originally
  come from the parser and can be of an arbitrary long length.
  Let's reserve additional buffer space for one extra character
  (SYSTEM_CHARSET_MBMAXLEN bytes), so Lex_ident_db::check_name() can
  still detect too long names even if the constructor cuts the data.
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
    if (Lex_ident_db::check_name(tmp))
      return Lex_ident_db();
    return Lex_ident_db(tmp);
  }
  Lex_ident_db to_lex_ident_db_with_error() const
  {
    const LEX_CSTRING tmp= to_lex_cstring();
    if (Lex_ident_db::check_name_with_error(tmp))
      return Lex_ident_db();
    return Lex_ident_db(tmp);
  }
};


class Identifier_chain2
{
  LEX_CSTRING m_name[2];
public:
  Identifier_chain2()
   :m_name{Lex_cstring(), Lex_cstring()}
  { }
  Identifier_chain2(const LEX_CSTRING &a, const LEX_CSTRING &b)
   :m_name{a, b}
  { }

  const LEX_CSTRING& operator [] (size_t i) const
  {
    return m_name[i];
  }

  static Identifier_chain2 split(const LEX_CSTRING &txt)
  {
    DBUG_ASSERT(txt.str[txt.length] == '\0'); // Expect 0-terminated input
    const char *dot= strchr(txt.str, '.');
    if (!dot)
      return Identifier_chain2(Lex_cstring(), txt);
    size_t length0= dot - txt.str;
    Lex_cstring name0(txt.str, length0);
    Lex_cstring name1(txt.str + length0 + 1, txt.length - length0 - 1);
    return Identifier_chain2(name0, name1);
  }

  // The minimum possible buffer size for the make_sep_name*() functions
  static constexpr size_t min_sep_name_size()
  {
    /*
      The minimal possible buffer is 4 bytes: 'd/t\0'
      where 'd' is the database name, 't' is the table name.
      Callers should never pass a smaller buffer.
    */
    return 4;
  }

  // Export as a qualified name string: 'db.name'
  size_t make_sep_name(char *dst, size_t dstlen, int sep) const
  {
    DBUG_ASSERT(dstlen >= min_sep_name_size());
    return my_snprintf(dst, dstlen, "%.*s%c%.*s",
                       (int) m_name[0].length, m_name[0].str, sep,
                       (int) m_name[1].length, m_name[1].str);
  }

  // Export as a qualified name string 'db.name', lower-casing 'db' and 'name'
  size_t make_sep_name_casedn(char *dst, size_t dst_size, int sep) const
  {
    DBUG_ASSERT(dst_size >= min_sep_name_size());
    CHARSET_INFO *cs= &my_charset_utf8mb3_general_ci;
    char *ptr= dst, *end= dst + dst_size;
    ptr+= cs->casedn(m_name[0].str, m_name[0].length, ptr, end - ptr - 2);
    *ptr++= (char) sep;
    ptr+= cs->casedn_z(m_name[1].str, m_name[1].length, ptr, end - ptr);
    return ptr - dst;
  }

  // Export as a qualified name, optionally lower-casing only the 'name' part
  size_t make_sep_name_casedn_part1(char *dst, size_t dst_size, int sep) const
  {
    DBUG_ASSERT(dst_size >= min_sep_name_size());
    CHARSET_INFO *cs= &my_charset_utf8mb3_general_ci;
    char *ptr= dst, *end= dst + dst_size;
    ptr+= cs->opt_casedn(m_name[0].str, m_name[0].length,
                         ptr, end - ptr - 2, false);
    *ptr++= (char) sep;
    ptr+= cs->casedn_z(m_name[1].str, m_name[1].length, ptr, end - ptr);
    return ptr - dst;
  }

  /*
    Export as a qualified name, e.g. 'db.name', 0-terminated,
    optionally lower-casing both 'db' and 'name' parts.

    @param [OUT] dst      - the destination
    @param       dst_size - number of bytes available in dst
    @param       sep      - the separator character
    @param       casedn   - whether to convert components to lower case
    @return               - number of bytes written to "dst", not counting
                            the trailing '\0' byte.
  */
  size_t make_sep_name_opt_casedn(char *dst, size_t dst_size,
                                  int sep, bool casedn) const
  {
    DBUG_ASSERT(m_name[0].length + m_name[1].length + 2 < FN_REFLEN - 1);
    return casedn ? make_sep_name_casedn(dst, dst_size, sep) :
                    make_sep_name(dst, dst_size, sep);
  }

  /*
    Export as a qualified name string 'db.name',
    using the dot character as a separator,
    optionally cover-casing the 'name' part.
  */
  size_t make_sep_name_opt_casedn_part1(char *dst, size_t dst_size,
                                        int sep,
                                        bool casedn_part1) const
  {
    return casedn_part1 ? make_sep_name_casedn_part1(dst, dst_size, sep) :
                          make_sep_name(dst, dst_size, sep);
  }

  /*
    Export as a qualified name string, allocated on mem_root,
    using the dot character as a separator,
    optionally lower-casing the 'name' part.
  */
  LEX_CSTRING make_sep_name_opt_casedn_part1(MEM_ROOT *mem_root,
                                             int sep,
                                             bool casedn_part1) const
  {
    LEX_STRING dst;
    /* format: [pkg + dot] + name + '\0' */
    size_t dst_size= m_name[0].length + 1 /*dot*/ + m_name[1].length + 1/*\0*/;
    if (unlikely(!(dst.str= (char*) alloc_root(mem_root, dst_size))))
      return {NULL, 0};
    if (!m_name[0].length)
    {
      DBUG_ASSERT(!casedn_part1); // Should not be called this way
      dst.length= my_snprintf(dst.str, dst_size, "%.*s",
                              (int) m_name[1].length, m_name[1].str);
      return {dst.str, dst.length};
    }
    dst.length= make_sep_name_opt_casedn_part1(dst.str, dst_size,
                                               sep, casedn_part1);
    return {dst.str, dst.length};
  }

  /*
    Export as a qualified name string 'db.name',
    using the dot character as a separator,
    lower-casing the 'name' part.
  */
  size_t make_qname_casedn_part1(char *dst, size_t dst_size) const
  {
    return make_sep_name_casedn_part1(dst, dst_size, '.');
  }

  // Export as a qualified name string: 'db.name' using the dot character.
  size_t make_qname(char *dst, size_t dstlen) const
  {
    return make_sep_name(dst, dstlen, '.');
  }

  /*
    Export as a qualified name string 'db.name', allocated on mem_root,
    using the dot character as a separator.
  */
  LEX_CSTRING make_qname(MEM_ROOT *mem_root) const
  {
    return make_sep_name_opt_casedn_part1(mem_root, '.', false);
  }

  /*
    Export as a qualified name string 'db.name', allocated on mem_root,
    using the dot character as a separator,
    lower-casing the 'name' part.
  */
  LEX_CSTRING make_qname_casedn_part1(MEM_ROOT *mem_root) const
  {
    return make_sep_name_opt_casedn_part1(mem_root, '.', true);
  }
};


#endif // LEX_IDENT_INCLUDED
