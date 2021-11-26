#ifndef SQL_STRING_INCLUDED
#define SQL_STRING_INCLUDED

/*
   Copyright (c) 2000, 2013, Oracle and/or its affiliates.
   Copyright (c) 2008, 2020, MariaDB Corporation.

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

/* This file is originally from the mysql distribution. Coded by monty */

#ifdef USE_PRAGMA_INTERFACE
#pragma interface			/* gcc class implementation */
#endif

#include "m_ctype.h"                            /* my_charset_bin */
#include <my_sys.h>              /* alloc_root, my_free, my_realloc */
#include "m_string.h"                           /* TRASH */
#include "sql_list.h"

class String;
#ifdef MYSQL_SERVER
extern PSI_memory_key key_memory_String_value;
#define STRING_PSI_MEMORY_KEY key_memory_String_value
#else
#define STRING_PSI_MEMORY_KEY PSI_NOT_INSTRUMENTED
#endif

typedef struct st_io_cache IO_CACHE;
typedef struct st_mem_root MEM_ROOT;
#define ASSERT_LENGTH(A) DBUG_ASSERT(str_length + (uint32) (A) <= Alloced_length)

#include "pack.h"
class Binary_string;
int sortcmp(const Binary_string *s, const Binary_string *t, CHARSET_INFO *cs);
int stringcmp(const Binary_string *s, const Binary_string *t);
String *copy_if_not_alloced(String *a,String *b,uint32 arg_length);
inline uint32 copy_and_convert(char *to, size_t to_length, CHARSET_INFO *to_cs,
                               const char *from, size_t from_length,
                               CHARSET_INFO *from_cs, uint *errors)
{
  return my_convert(to, (uint)to_length, to_cs, from, (uint)from_length,
                    from_cs, errors);
}


class String_copy_status: protected MY_STRCOPY_STATUS
{
public:
  const char *source_end_pos() const
  { return m_source_end_pos; }
  const char *well_formed_error_pos() const
  { return m_well_formed_error_pos; }
};


class Well_formed_prefix_status: public String_copy_status
{
public:
  Well_formed_prefix_status(CHARSET_INFO *cs,
                            const char *str, const char *end, size_t nchars)
  { cs->well_formed_char_length(str, end, nchars, this); }
};


class Well_formed_prefix: public Well_formed_prefix_status
{
  const char *m_str; // The beginning of the string
public:
  Well_formed_prefix(CHARSET_INFO *cs, const char *str, const char *end,
                     size_t nchars)
   :Well_formed_prefix_status(cs, str, end, nchars), m_str(str)
  { }
  Well_formed_prefix(CHARSET_INFO *cs, const char *str, size_t length,
                     size_t nchars)
   :Well_formed_prefix_status(cs, str, str + length, nchars), m_str(str)
  { }
  Well_formed_prefix(CHARSET_INFO *cs, const char *str, size_t length)
   :Well_formed_prefix_status(cs, str, str + length, length), m_str(str)
  { }
  Well_formed_prefix(CHARSET_INFO *cs, LEX_CSTRING str, size_t nchars)
   :Well_formed_prefix_status(cs, str.str, str.str + str.length, nchars),
    m_str(str.str)
  { }
  size_t length() const { return m_source_end_pos - m_str; }
};


class String_copier: public String_copy_status,
                     protected MY_STRCONV_STATUS
{
public:
  const char *cannot_convert_error_pos() const
  { return m_cannot_convert_error_pos; }
  const char *most_important_error_pos() const
  {
    return well_formed_error_pos() ? well_formed_error_pos() :
                                     cannot_convert_error_pos();
  }
  /*
    Convert a string between character sets.
    "dstcs" and "srccs" cannot be &my_charset_bin.
  */
  size_t convert_fix(CHARSET_INFO *dstcs, char *dst, size_t dst_length,
                     CHARSET_INFO *srccs, const char *src, size_t src_length,
                     size_t nchars)
  {
    return my_convert_fix(dstcs, dst, dst_length,
                          srccs, src, src_length, nchars, this, this);
  }
  /*
     Copy a string. Fix bad bytes/characters to '?'.
  */
  uint well_formed_copy(CHARSET_INFO *to_cs, char *to, size_t to_length,
                        CHARSET_INFO *from_cs, const char *from,
                        size_t from_length, size_t nchars);
  // Same as above, but without the "nchars" limit.
  uint well_formed_copy(CHARSET_INFO *to_cs, char *to, size_t to_length,
                        CHARSET_INFO *from_cs, const char *from,
                        size_t from_length)
  {
    return well_formed_copy(to_cs, to, to_length,
                            from_cs, from, from_length,
                            from_length /* No limit on "nchars"*/);
  }
};


size_t my_copy_with_hex_escaping(CHARSET_INFO *cs,
                                 char *dst, size_t dstlen,
                                 const char *src, size_t srclen);
uint convert_to_printable(char *to, size_t to_len,
                          const char *from, size_t from_len,
                          CHARSET_INFO *from_cs, size_t nbytes= 0);
size_t convert_to_printable_required_length(uint len);


class Charset
{
  CHARSET_INFO *m_charset;
public:
  Charset() :m_charset(&my_charset_bin) { }
  Charset(CHARSET_INFO *cs) :m_charset(cs) { }

  CHARSET_INFO *charset() const { return m_charset; }
  bool use_mb() const { return m_charset->use_mb(); }
  uint mbminlen() const { return m_charset->mbminlen; }
  uint mbmaxlen() const { return m_charset->mbmaxlen; }
  bool is_good_for_ft() const
  {
    // Binary and UCS2/UTF16/UTF32 are not supported
    return m_charset != &my_charset_bin && m_charset->mbminlen == 1;
  }

  size_t numchars(const char *str, const char *end) const
  {
    return m_charset->numchars(str, end);
  }
  size_t lengthsp(const char *str, size_t length) const
  {
    return m_charset->lengthsp(str, length);
  }
  size_t charpos(const char *str, const char *end, size_t pos) const
  {
    return m_charset->charpos(str, end, pos);
  }
  void set_charset(CHARSET_INFO *charset_arg)
  {
    m_charset= charset_arg;
  }
  void set_charset(const Charset &other)
  {
    m_charset= other.m_charset;
  }
  void swap(Charset &other)
  {
    swap_variables(CHARSET_INFO*, m_charset, other.m_charset);
  }
  bool same_encoding(const Charset &other) const
  {
    return my_charset_same(m_charset, other.m_charset);
  }
  /*
    Collation name without the character set name.
    For example, in case of "latin1_swedish_ci",
    this method returns "_swedish_ci".
  */
  LEX_CSTRING collation_specific_name() const;
  bool encoding_allows_reinterpret_as(CHARSET_INFO *cs) const;
  bool eq_collation_specific_names(CHARSET_INFO *cs) const;
};


/**
   Storage for strings with both length and allocated length.
   Automatically grows on demand.
*/

class Binary_string: public Sql_alloc
{
protected:
  char *Ptr;
  uint32 str_length, Alloced_length, extra_alloc;
  bool alloced, thread_specific;
  void init_private_data()
  {
    Ptr= 0;
    Alloced_length= extra_alloc= str_length= 0;
    alloced= thread_specific= false;
  }
  inline void free_buffer()
  {
    if (alloced)
    {
      alloced=0;
      my_free(Ptr);
    }
  }
public:
  Binary_string()
  {
    init_private_data();
  }
  explicit Binary_string(size_t length_arg)
  {
    init_private_data();
    (void) real_alloc(length_arg);
  }
  /*
    NOTE: If one intend to use the c_ptr() method, the following two
    contructors need the size of memory for STR to be at least LEN+1 (to make
    room for zero termination).
  */
  Binary_string(const char *str, size_t len)
  {
    Ptr= (char*) str;
    str_length= (uint32) len;
    Alloced_length= 0;                          /* Memory cannot be written to */
    extra_alloc= 0;
    alloced= thread_specific= 0;
  }
  Binary_string(char *str, size_t len)
  {
    Ptr= str;
    str_length= Alloced_length= (uint32) len;
    extra_alloc= 0;
    alloced= thread_specific= 0;
  }
  explicit Binary_string(const Binary_string &str)
  {
    Ptr= str.Ptr;
    str_length= str.str_length;
    Alloced_length= str.Alloced_length;
    extra_alloc= 0;
    alloced= thread_specific= 0;
  }

  ~Binary_string()
  {
    free();
  }

  inline uint32 length() const { return str_length;}
  inline char& operator [] (size_t i) const { return Ptr[i]; }
  inline void length(size_t len) { str_length=(uint32)len ; }
  inline bool is_empty() const { return (str_length == 0); }
  inline const char *ptr() const { return Ptr; }
  inline const char *end() const { return Ptr + str_length; }
  bool has_8bit_bytes() const
  {
    for (const char *c= ptr(), *c_end= end(); c < c_end; c++)
    {
      if (!my_isascii(*c))
        return true;
    }
    return false;
  }

  bool bin_eq(const Binary_string *other) const
  {
    return length() == other->length() &&
           !memcmp(ptr(), other->ptr(), length());
  }

  /*
    PMG 2004.11.12
    This is a method that works the same as perl's "chop". It simply
    drops the last character of a string. This is useful in the case
    of the federated storage handler where I'm building a unknown
    number, list of values and fields to be used in a sql insert
    statement to be run on the remote server, and have a comma after each.
    When the list is complete, I "chop" off the trailing comma

    ex.
      String stringobj;
      stringobj.append("VALUES ('foo', 'fi', 'fo',");
      stringobj.chop();
      stringobj.append(")");

    In this case, the value of string was:

    VALUES ('foo', 'fi', 'fo',
    VALUES ('foo', 'fi', 'fo'
    VALUES ('foo', 'fi', 'fo')
  */
  inline void chop()
  {
    if (str_length)
    {
      str_length--;
      Ptr[str_length]= '\0';
      DBUG_ASSERT(strlen(Ptr) == str_length);
    }
  }

  // Returns offset to substring or -1
  int strstr(const Binary_string &search, uint32 offset=0);
  // Returns offset to substring or -1
  int strrstr(const Binary_string &search, uint32 offset=0);

  /*
    The following append operations do not extend the strings and in production
    mode do NOT check that alloced memory!
    q_*** methods writes values of parameters itself
    qs_*** methods writes string representation of value
  */
  void q_append(const char c)
  {
    ASSERT_LENGTH(1);
    Ptr[str_length++] = c;
  }
  void q_append2b(const uint32 n)
  {
    ASSERT_LENGTH(2);
    int2store(Ptr + str_length, n);
    str_length += 2;
  }
  void q_append(const uint32 n)
  {
    ASSERT_LENGTH(4);
    int4store(Ptr + str_length, n);
    str_length += 4;
  }
  void q_append(double d)
  {
    ASSERT_LENGTH(8);
    float8store(Ptr + str_length, d);
    str_length += 8;
  }
  void q_append(double *d)
  {
    ASSERT_LENGTH(8);
    float8store(Ptr + str_length, *d);
    str_length += 8;
  }
  void q_append(const char *data, size_t data_len)
  {
    ASSERT_LENGTH(data_len);
    if (data_len)
      memcpy(Ptr + str_length, data, data_len);
    DBUG_ASSERT(str_length <= UINT_MAX32 - data_len);
    str_length += (uint)data_len;
  }
  void q_append(const LEX_CSTRING *ls)
  {
    DBUG_ASSERT(ls->length < UINT_MAX32 &&
                ((ls->length == 0 && !ls->str) ||
                 ls->length == strlen(ls->str)));
    q_append(ls->str, (uint32) ls->length);
  }

  void write_at_position(uint32 position, uint32 value)
  {
    DBUG_ASSERT(str_length >= position + 4);
    int4store(Ptr + position,value);
  }

  void qs_append(const LEX_CSTRING *ls)
  {
    DBUG_ASSERT(ls->length < UINT_MAX32 &&
                ((ls->length == 0 && !ls->str) ||
                 ls->length == strlen(ls->str)));
    qs_append(ls->str, (uint32)ls->length);
  }
  void qs_append(const char *str, size_t len);
  void qs_append_hex(const char *str, uint32 len);
  void qs_append(double d);
  void qs_append(const double *d);
  inline void qs_append(const char c)
  {
    ASSERT_LENGTH(1);
    Ptr[str_length]= c;
    str_length++;
  }
  void qs_append(int i);
  void qs_append(uint i)
  {
    qs_append((ulonglong)i);
  }
  void qs_append(ulong i)
  {
    qs_append((ulonglong)i);
  }
  void qs_append(ulonglong i);
  void qs_append(longlong i, int radix)
  {
    ASSERT_LENGTH(22);
    char *buff= Ptr + str_length;
    char *end= ll2str(i, buff, radix, 0);
    str_length+= (uint32) (end-buff);
  }

  /* Mark variable thread specific it it's not allocated already */
  inline void set_thread_specific()
  {
    if (!alloced)
      thread_specific= 1;
  }
  bool is_alloced() const { return alloced; }
  inline uint32 alloced_length() const { return Alloced_length;}
  inline uint32 extra_allocation() const { return extra_alloc;}
  inline void extra_allocation(size_t len) { extra_alloc= (uint32)len; }
  inline void mark_as_const() { Alloced_length= 0;}

  inline bool uses_buffer_owned_by(const Binary_string *s) const
  {
    return (s->alloced && Ptr >= s->Ptr && Ptr < s->Ptr + s->Alloced_length);
  }

  /* Swap two string objects. Efficient way to exchange data without memcpy. */
  void swap(Binary_string &s)
  {
    swap_variables(char *, Ptr, s.Ptr);
    swap_variables(uint32, str_length, s.str_length);
    swap_variables(uint32, Alloced_length, s.Alloced_length);
    swap_variables(bool, alloced, s.alloced);
  }

  /**
     Points the internal buffer to the supplied one. The old buffer is freed.
     @param str Pointer to the new buffer.
     @param arg_length Length of the new buffer in characters, excluding any
            null character.
     @note The new buffer will not be null terminated.
  */
  void set_alloced(char *str, size_t length, size_t alloced_length)
  {
    free_buffer();
    Ptr= str;
    str_length= (uint32) length;
    DBUG_ASSERT(alloced_length < UINT_MAX32);
    Alloced_length= (uint32) alloced_length;
  }
  inline void set(char *str, size_t arg_length)
  {
    set_alloced(str, arg_length, arg_length);
  }
  inline void set(const char *str, size_t length)
  {
    free_buffer();
    Ptr= (char*) str;
    str_length= (uint32) length;
    Alloced_length= 0;
  }

  void set(Binary_string &str, size_t offset, size_t length)
  {
    DBUG_ASSERT(&str != this);
    free_buffer();
    Ptr= str.Ptr + offset;
    str_length= (uint32) length;
    Alloced_length= 0;
    if (str.Alloced_length)
      Alloced_length= (uint32) (str.Alloced_length - offset);
  }
  LEX_CSTRING to_lex_cstring() const
  {
    LEX_CSTRING tmp= {Ptr, str_length};
    return tmp;
  }
  inline LEX_CSTRING *get_value(LEX_CSTRING *res)
  {
    res->str=    Ptr;
    res->length= str_length;
    return res;
  }

  /* Take over handling of buffer from some other object */
  void reset(char *ptr_arg, size_t length_arg, size_t alloced_length_arg)
  {
    set_alloced(ptr_arg, length_arg, alloced_length_arg);
    alloced= ptr_arg != 0;
  }

  /* Forget about the buffer, let some other object handle it */
  char *release()
  {
    char *old= Ptr;
    init_private_data();
    return old;
  }

  /*
    This is used to set a new buffer for String.
    However if the String already has an allocated buffer, it will
    keep that one.
    It's not to be used to set the value or length of the string.
  */
  inline void set_buffer_if_not_allocated(char *str, size_t arg_length)
  {
    if (!alloced)
    {
      /*
        Following should really set str_length= 0, but some code may
        depend on that the String length is same as buffer length.
      */
      Ptr= str;
      str_length= Alloced_length= (uint32) arg_length;
    }
    /* One should set str_length before using it */
    MEM_UNDEFINED(&str_length, sizeof(str_length));
  }

  inline Binary_string& operator=(const Binary_string &s)
  {
    if (&s != this)
    {
      /*
        It is forbidden to do assignments like
        some_string = substring_of_that_string
      */
      DBUG_ASSERT(!s.uses_buffer_owned_by(this));
      set_alloced((char *) s.Ptr, s.str_length, s.Alloced_length);
    }
    return *this;
  }

  bool set_hex(ulonglong num);
  bool set_hex(const char *str, uint32 len);
  bool set_fcvt(double num, uint decimals);

  bool copy();                                  // Alloc string if not alloced
  bool copy(const Binary_string &s);            // Allocate new string
  bool copy(const char *s, size_t arg_length);	// Allocate new string
  bool copy_or_move(const char *s,size_t arg_length);

  /**
    Convert a string to a printable format.
    All non-convertable and control characters are replaced to 5-character
    sequences '\hhhh'.
  */
  bool copy_printable_hhhh(CHARSET_INFO *to_cs,
                           CHARSET_INFO *from_cs,
                           const char *from, size_t from_length);

  bool append_ulonglong(ulonglong val);
  bool append_longlong(longlong val);

  bool append(const char *s, size_t size)
  {
    if (!size)
      return false;
    if (realloc_with_extra_if_needed(str_length + size))
      return true;
    q_append(s, size);
    return false;
  }
  bool append(const LEX_CSTRING &s)
  {
    return append(s.str, s.length);
  }
  bool append(const Binary_string &s)
  {
    return append(s.ptr(), s.length());
  }
  bool append(IO_CACHE* file, uint32 arg_length);

  inline bool append_char(char chr)
  {
    if (str_length < Alloced_length)
    {
      Ptr[str_length++]= chr;
    }
    else
    {
      if (unlikely(realloc_with_extra(str_length + 1)))
	return true;
      Ptr[str_length++]= chr;
    }
    return false;
  }
  bool append_hex(const char *src, uint32 srclen)
  {
    for (const char *src_end= src + srclen ; src != src_end ; src++)
    {
      if (unlikely(append_char(_dig_vec_lower[((uchar) *src) >> 4])) ||
          unlikely(append_char(_dig_vec_lower[((uchar) *src) & 0x0F])))
        return true;
    }
    return false;
  }

  bool append_with_step(const char *s, uint32 arg_length, uint32 step_alloc)
  {
    uint32 new_length= arg_length + str_length;
    if (new_length > Alloced_length &&
        unlikely(realloc(new_length + step_alloc)))
      return true;
    q_append(s, arg_length);
    return false;
  }

  inline char *c_ptr()
  {
    if (unlikely(!Ptr))
      return (char*) "";
    /*
      Here we assume that any buffer used to initalize String has
      an end \0 or have at least an accessable character at end.
      This is to handle the case of String("Hello",5) and
      String("hello",5) efficiently.

      We have two options here. To test for !Alloced_length or !alloced.
      Using "Alloced_length" is slightly safer so that we do not read
      from potentially unintialized memory (normally not dangerous but
      may give warnings in valgrind), but "alloced" is safer as there
      are less change to get memory loss from code that is using
      String((char*), length) or String.set((char*), length) and does
      not free things properly (and there is several places in the code
      where this happens and it is hard to find out if any of these will call
      c_ptr().
    */
    if (unlikely(!alloced && !Ptr[str_length]))
      return Ptr;
    if (str_length < Alloced_length)
    {
      Ptr[str_length]=0;
      return Ptr;
    }
    (void) realloc(str_length);               /* This will add end \0 */
    return Ptr;
  }
  /*
    One should use c_ptr() instead for most cases. This will be deleted soon,
    kept for compatiblity.
  */
  inline char *c_ptr_quick()
  {
    return c_ptr_safe();
  }
  /*
    This is to be used only in the case when one cannot use c_ptr().
    The cases are:
    - When one initializes String with an external buffer and length and
      buffer[length] could be uninitalized when c_ptr() is called.
    - When valgrind gives warnings about uninitialized memory with c_ptr().
  */
  inline char *c_ptr_safe()
  {
    if (Ptr && str_length < Alloced_length)
      Ptr[str_length]=0;
    else
      (void) realloc(str_length);
    return Ptr;
  }

  inline void free()
  {
    free_buffer();
    /*
      We have to clear the values as some Strings, like in Field, are
      reused after free(). Because of this we cannot use MEM_UNDEFINED() here.
    */
    Ptr= 0;
    str_length= 0;
    Alloced_length= extra_alloc= 0;
  }

  inline bool alloc(size_t arg_length)
  {
    /*
      Allocate if we need more space or if we don't have done any
      allocation yet (we don't want to have Ptr to be NULL for empty strings).

      Note that if arg_length == Alloced_length then we don't allocate.
      This ensures we don't do any extra allocations in protocol and String:int,
      but the string will not be atomically null terminated if c_ptr() is not
      called.
    */
    if (arg_length <= Alloced_length && Alloced_length)
      return 0;
    return real_alloc(arg_length);
  }
  bool real_alloc(size_t arg_length);  // Empties old string
  bool realloc_raw(size_t arg_length);
  bool realloc(size_t arg_length)
  {
    if (realloc_raw(arg_length+1))
      return TRUE;
    Ptr[arg_length]= 0; // This make other funcs shorter
    return FALSE;
  }
  bool realloc_with_extra(size_t arg_length)
  {
    if (extra_alloc < 4096)
      extra_alloc= extra_alloc*2+128;
    if (realloc_raw(arg_length + extra_alloc))
      return TRUE;
    Ptr[arg_length]=0;        // This make other funcs shorter
    return FALSE;
  }
  bool realloc_with_extra_if_needed(size_t arg_length)
  {
    if (arg_length < Alloced_length)
    {
      Ptr[arg_length]=0; // behave as if realloc was called.
      return 0;
    }
    return realloc_with_extra(arg_length);
  }
  // Shrink the buffer, but only if it is allocated on the heap.
  void shrink(size_t arg_length);

  void move(Binary_string &s)
  {
    set_alloced(s.Ptr, s.str_length, s.Alloced_length);
    extra_alloc= s.extra_alloc;
    alloced= s.alloced;
    thread_specific= s.thread_specific;
    s.alloced= 0;
  }
  bool fill(size_t max_length,char fill);
  /*
    Replace substring with string
    If wrong parameter or not enough memory, do nothing
  */
  bool replace(uint32 offset,uint32 arg_length, const char *to, uint32 length);
  bool replace(uint32 offset,uint32 arg_length, const Binary_string &to)
  {
    return replace(offset,arg_length,to.ptr(),to.length());
  }

  int reserve(size_t space_needed)
  {
    DBUG_ASSERT((ulonglong) str_length + space_needed < UINT_MAX32);
    return realloc(str_length + space_needed);
  }
  int reserve(size_t space_needed, size_t grow_by);

  inline char *prep_append(uint32 arg_length, uint32 step_alloc)
  {
    uint32 new_length= arg_length + str_length;
    if (new_length > Alloced_length)
    {
      if (unlikely(realloc(new_length + step_alloc)))
        return 0;
    }
    uint32 old_length= str_length;
    str_length+= arg_length;
    return Ptr + old_length;                  // Area to use
  }


  void q_net_store_length(ulonglong length)
  {
    DBUG_ASSERT(Alloced_length >= (str_length + net_length_size(length)));
    char *pos= (char *) net_store_length((uchar *)(Ptr + str_length), length);
    str_length= uint32(pos - Ptr);
  }
  void q_net_store_data(const uchar *from, size_t length)
  {
    DBUG_ASSERT(length < UINT_MAX32);
    DBUG_ASSERT(Alloced_length >= (str_length + length +
                                   net_length_size(length)));
    q_net_store_length(length);
    q_append((const char *)from, (uint32) length);
  }
};


class String: public Charset, public Binary_string
{
public:
  String() { }
  String(size_t length_arg) :Binary_string(length_arg)
  { }
  /*
    NOTE: If one intend to use the c_ptr() method, the following two
    contructors need the size of memory for STR to be at least LEN+1 (to make
    room for zero termination).
  */
  String(const char *str, size_t len, CHARSET_INFO *cs)
   :Charset(cs), Binary_string(str, len)
  { }
  String(char *str, size_t len, CHARSET_INFO *cs)
   :Charset(cs), Binary_string(str, len)
  { }
  String(const String &str)
   :Charset(str), Binary_string(str)
  { }

  void set(String &str,size_t offset,size_t arg_length)
  {
    Binary_string::set(str, offset, arg_length);
    set_charset(str);
  }
  inline void set(char *str,size_t arg_length, CHARSET_INFO *cs)
  {
    Binary_string::set(str, arg_length);
    set_charset(cs);
  }
  inline void set(const char *str,size_t arg_length, CHARSET_INFO *cs)
  {
    Binary_string::set(str, arg_length);
    set_charset(cs);
  }
  bool set_ascii(const char *str, size_t arg_length);
  inline void set_buffer_if_not_allocated(char *str,size_t arg_length,
                                          CHARSET_INFO *cs)
  {
    Binary_string::set_buffer_if_not_allocated(str, arg_length);
    set_charset(cs);
  }
  bool set_int(longlong num, bool unsigned_flag, CHARSET_INFO *cs);
  bool set(int num, CHARSET_INFO *cs) { return set_int(num, false, cs); }
  bool set(uint num, CHARSET_INFO *cs) { return set_int(num, true, cs); }
  bool set(long num, CHARSET_INFO *cs) { return set_int(num, false, cs); }
  bool set(ulong num, CHARSET_INFO *cs) { return set_int(num, true, cs); }
  bool set(longlong num, CHARSET_INFO *cs) { return set_int(num, false, cs); }
  bool set(ulonglong num, CHARSET_INFO *cs) { return set_int((longlong)num, true, cs); }
  bool set_real(double num,uint decimals, CHARSET_INFO *cs);
  bool set_fcvt(double num, uint decimals)
  {
    set_charset(&my_charset_latin1);
    return Binary_string::set_fcvt(num, decimals);
  }

  bool set_hex(ulonglong num)
  {
    set_charset(&my_charset_latin1);
    return Binary_string::set_hex(num);
  }
  bool set_hex(const char *str, uint32 len)
  {
    set_charset(&my_charset_latin1);
    return Binary_string::set_hex(str, len);
  }

  /* Take over handling of buffer from some other object */
  void reset(char *ptr_arg, size_t length_arg, size_t alloced_length_arg,
             CHARSET_INFO *cs)
  {
    Binary_string::reset(ptr_arg, length_arg, alloced_length_arg);
    set_charset(cs);
  }

  inline String& operator = (const String &s)
  {
    if (&s != this)
    {
      set_charset(s);
      Binary_string::operator=(s);
    }
    return *this;
  }

  bool copy()
  {
    return Binary_string::copy();
  }
  bool copy(const String &s)
  {
    set_charset(s);
    return Binary_string::copy(s);
  }
  bool copy(const char *s, size_t arg_length, CHARSET_INFO *cs)
  {
    set_charset(cs);
    return Binary_string::copy(s, arg_length);
  }
  bool copy_or_move(const char *s, size_t arg_length, CHARSET_INFO *cs)
  {
    set_charset(cs);
    return Binary_string::copy_or_move(s, arg_length);
  }
  static bool needs_conversion(size_t arg_length,
  			       CHARSET_INFO *cs_from, CHARSET_INFO *cs_to,
			       uint32 *offset);
  static bool needs_conversion_on_storage(size_t arg_length,
                                          CHARSET_INFO *cs_from,
                                          CHARSET_INFO *cs_to);
  bool copy_aligned(const char *s, size_t arg_length, size_t offset,
		    CHARSET_INFO *cs);
  bool set_or_copy_aligned(const char *s, size_t arg_length, CHARSET_INFO *cs);
  bool can_be_safely_converted_to(CHARSET_INFO *tocs) const
  {
    if (charset() == &my_charset_bin)
      return Well_formed_prefix(tocs, ptr(), length()).length() == length();
    String try_val;
    uint try_conv_error= 0;
    try_val.copy(ptr(), length(), charset(), tocs, &try_conv_error);
    return try_conv_error == 0;
  }
  bool copy(const char*s, size_t arg_length, CHARSET_INFO *csfrom,
	    CHARSET_INFO *csto, uint *errors);
  bool copy(const String *str, CHARSET_INFO *tocs, uint *errors)
  {
    return copy(str->ptr(), str->length(), str->charset(), tocs, errors);
  }
  bool copy(CHARSET_INFO *tocs,
            CHARSET_INFO *fromcs, const char *src, size_t src_length,
            size_t nchars, String_copier *copier)
  {
    if (unlikely(alloc(tocs->mbmaxlen * src_length)))
      return true;
    str_length= copier->well_formed_copy(tocs, Ptr, alloced_length(),
                                         fromcs, src, (uint) src_length,
                                         (uint) nchars);
    set_charset(tocs);
    return false;
  }
  // Append without character set conversion
  bool append(const String &s)
  {
    return Binary_string::append(s);
  }
  inline bool append(char chr)
  {
    return Binary_string::append_char(chr);
  }
  bool append_hex(const char *src, uint32 srclen)
  {
    return Binary_string::append_hex(src, srclen);
  }
  bool append_hex(const uchar *src, uint32 srclen)
  {
    return Binary_string::append_hex((const char*)src, srclen);
  }
  bool append_introducer_and_hex(const String *str)
  {
    return
      append('_')   ||
      append(str->charset()->cs_name) ||
      append(STRING_WITH_LEN(" 0x")) ||
      append_hex(str->ptr(), (uint32) str->length());
  }
  bool append(IO_CACHE* file, uint32 arg_length)
  {
    return Binary_string::append(file, arg_length);
  }
  inline bool append(const char *s, uint32 arg_length, uint32 step_alloc)
  {
    return append_with_step(s, arg_length, step_alloc);
  }

  // Append with optional character set conversion from ASCII (e.g. to UCS2)
  bool append(const LEX_STRING *ls)
  {
    DBUG_ASSERT(ls->length < UINT_MAX32 &&
                ((ls->length == 0 && !ls->str) ||
                 ls->length == strlen(ls->str)));
    return append(ls->str, (uint32) ls->length);
  }
  bool append(const LEX_CSTRING *ls)
  {
    DBUG_ASSERT(ls->length < UINT_MAX32 &&
                ((ls->length == 0 && !ls->str) ||
                 ls->length == strlen(ls->str)));
    return append(ls->str, (uint32) ls->length);
  }
  bool append(const LEX_CSTRING &ls)
  {
    return append(&ls);
  }
  bool append(const char *s, size_t size);
  bool append_with_prefill(const char *s, uint32 arg_length,
			   uint32 full_length, char fill_char);
  bool append_parenthesized(long nr, int radix= 10);

  // Append with optional character set conversion from cs to charset()
  bool append(const char *s, size_t arg_length, CHARSET_INFO *cs);
  bool append(const LEX_CSTRING &s, CHARSET_INFO *cs)
  {
    return append(s.str, s.length, cs);
  }

  void strip_sp();
  friend String *copy_if_not_alloced(String *a,String *b,uint32 arg_length);
  friend class Field;
  uint32 numchars() const
  {
    return (uint32) Charset::numchars(ptr(), end());
  }
  int charpos(longlong i, uint32 offset=0)
  {
    if (i <= 0)
      return (int) i;
    return (int) Charset::charpos(ptr() + offset, end(), (size_t) i);
  }
  size_t lengthsp() const
  {
    return Charset::lengthsp(Ptr, str_length);
  }

  void print(String *to) const;
  void print_with_conversion(String *to, CHARSET_INFO *cs) const;
  void print(String *to, CHARSET_INFO *cs) const
  {
    if (my_charset_same(charset(), cs))
      print(to);
    else
      print_with_conversion(to, cs);
  }

  bool append_for_single_quote(const char *st, size_t len);
  bool append_for_single_quote(const String *s)
  {
    return append_for_single_quote(s->ptr(), s->length());
  }

  void swap(String &s)
  {
    Charset::swap(s);
    Binary_string::swap(s);
  }

  uint well_formed_length() const
  {
    return (uint) Well_formed_prefix(charset(), ptr(), length()).length();
  }
  bool is_ascii() const
  {
    if (length() == 0)
      return TRUE;
    if (charset()->mbminlen > 1)
      return FALSE;
    return !has_8bit_bytes();
  }
  bool eq(const String *other, CHARSET_INFO *cs) const
  {
    return !sortcmp(this, other, cs);
  }
private:
  bool append_semi_hex(const char *s, uint len, CHARSET_INFO *cs);
};


// The following class is a backport from MySQL 5.6:
/**
  String class wrapper with a preallocated buffer of size buff_sz

  This class allows to replace sequences of:
     char buff[12345];
     String str(buff, sizeof(buff));
     str.length(0);
  with a simple equivalent declaration:
     StringBuffer<12345> str;
*/

template<size_t buff_sz>
class StringBuffer : public String
{
  char buff[buff_sz];

public:
  StringBuffer() : String(buff, buff_sz, &my_charset_bin) { length(0); }
  explicit StringBuffer(CHARSET_INFO *cs) : String(buff, buff_sz, cs)
  {
    length(0);
  }
  void set_buffer_if_not_allocated(CHARSET_INFO *cs)
  {
    if (!is_alloced())
    {
      Ptr= buff;
      Alloced_length= (uint32) buff_sz;
    }
    str_length= 0;                          /* Safety, not required */
    /* One should set str_length before using it */
    MEM_UNDEFINED(&str_length, sizeof(str_length));
    set_charset(cs);
  }
};


template<size_t buff_sz>
class BinaryStringBuffer : public Binary_string
{
  char buff[buff_sz];
public:
  BinaryStringBuffer() : Binary_string(buff, buff_sz) { length(0); }
};

static inline bool check_if_only_end_space(CHARSET_INFO *cs,
                                           const char *str,
                                           const char *end)
{
  return str + cs->scan(str, end, MY_SEQ_SPACES) == end;
}

int append_query_string(CHARSET_INFO *csinfo, String *to,
                        const char *str, size_t len, bool no_backslash);

#endif /* SQL_STRING_INCLUDED */
