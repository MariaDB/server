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
#include "my_sys.h"              /* alloc_root, my_free, my_realloc */
#include "m_string.h"                           /* TRASH */
#include "sql_list.h"

class String;
typedef struct st_io_cache IO_CACHE;
typedef struct st_mem_root MEM_ROOT;

#include "pack.h"
int sortcmp(const String *a,const String *b, CHARSET_INFO *cs);
String *copy_if_not_alloced(String *a,String *b,uint32 arg_length);
inline uint32 copy_and_convert(char *to, uint32 to_length,
                               CHARSET_INFO *to_cs,
                               const char *from, uint32 from_length,
                               CHARSET_INFO *from_cs, uint *errors)
{
  return my_convert(to, to_length, to_cs, from, from_length, from_cs, errors);
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
  { cs->cset->well_formed_char_length(cs, str, end, nchars, this); }
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
  Well_formed_prefix(CHARSET_INFO *cs, LEX_STRING str, size_t nchars)
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
  size_t convert_fix(CHARSET_INFO *dstcs, char *dst, uint dst_length,
                     CHARSET_INFO *srccs, const char *src, uint src_length,
                     uint nchars)
  {
    return my_convert_fix(dstcs, dst, dst_length,
                          srccs, src, src_length, nchars, this, this);
  }
  /*
     Copy a string. Fix bad bytes/characters to '?'.
  */
  uint well_formed_copy(CHARSET_INFO *to_cs, char *to, uint to_length,
                        CHARSET_INFO *from_cs, const char *from,
                        uint from_length, uint nchars);
  // Same as above, but without the "nchars" limit.
  uint well_formed_copy(CHARSET_INFO *to_cs, char *to, uint to_length,
                        CHARSET_INFO *from_cs, const char *from,
                        uint from_length)
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

class String : public Sql_alloc
{
  char *Ptr;
  uint32 str_length,Alloced_length, extra_alloc;
  bool alloced,thread_specific;
  CHARSET_INFO *str_charset;
public:
  String()
  { 
    Ptr=0; str_length=Alloced_length=extra_alloc=0;
    alloced= thread_specific= 0; 
    str_charset= &my_charset_bin; 
  }
  String(uint32 length_arg)
  { 
    alloced= thread_specific= 0;
    Alloced_length= extra_alloc= 0; (void) real_alloc(length_arg); 
    str_charset= &my_charset_bin;
  }
  String(const char *str, CHARSET_INFO *cs)
  { 
    Ptr=(char*) str; str_length= (uint32) strlen(str);
    Alloced_length= extra_alloc= 0;
    alloced= thread_specific= 0;
    str_charset=cs;
  }
  /*
    NOTE: If one intend to use the c_ptr() method, the following two
    contructors need the size of memory for STR to be at least LEN+1 (to make
    room for zero termination).
  */
  String(const char *str,uint32 len, CHARSET_INFO *cs)
  { 
    Ptr=(char*) str; str_length=len; Alloced_length= extra_alloc=0;
    alloced= thread_specific= 0;
    str_charset=cs;
  }
  String(char *str,uint32 len, CHARSET_INFO *cs)
  { 
    Ptr=(char*) str; Alloced_length=str_length=len; extra_alloc= 0;
    alloced= thread_specific= 0;
    str_charset=cs;
  }
  String(const String &str)
  { 
    Ptr=str.Ptr ; str_length=str.str_length ;
    Alloced_length=str.Alloced_length; extra_alloc= 0;
    alloced= thread_specific= 0;
    str_charset=str.str_charset;
  }
  ~String() { free(); }

  /* Mark variable thread specific it it's not allocated already */
  inline void set_thread_specific()
  {
    if (!alloced)
      thread_specific= 1;
  }
  inline void set_charset(CHARSET_INFO *charset_arg)
  { str_charset= charset_arg; }
  inline CHARSET_INFO *charset() const { return str_charset; }
  inline uint32 length() const { return str_length;}
  inline uint32 alloced_length() const { return Alloced_length;}
  inline uint32 extra_allocation() const { return extra_alloc;}
  inline char& operator [] (uint32 i) const { return Ptr[i]; }
  inline void length(uint32 len) { str_length=len ; }
  inline void extra_allocation(uint32 len) { extra_alloc= len; }
  inline bool is_empty() const { return (str_length == 0); }
  inline void mark_as_const() { Alloced_length= 0;}
  inline const char *ptr() const { return Ptr; }
  inline const char *end() const { return Ptr + str_length; }
  inline char *c_ptr()
  {
    DBUG_ASSERT(!alloced || !Ptr || !Alloced_length || 
                (Alloced_length >= (str_length + 1)));

    if (!Ptr || Ptr[str_length])		/* Should be safe */
      (void) realloc(str_length);
    return Ptr;
  }
  inline char *c_ptr_quick()
  {
    if (Ptr && str_length < Alloced_length)
      Ptr[str_length]=0;
    return Ptr;
  }
  inline char *c_ptr_safe()
  {
    if (Ptr && str_length < Alloced_length)
      Ptr[str_length]=0;
    else
      (void) realloc(str_length);
    return Ptr;
  }
  LEX_STRING lex_string() const
  {
    LEX_STRING str = { (char*) ptr(), length() };
    return str;
  }
  LEX_CSTRING lex_cstring() const
  {
    LEX_CSTRING skr = { ptr(), length() };
    return skr;
  }

  size_t lengthsp() const
  {
    return str_charset->cset->lengthsp(str_charset, Ptr, str_length);
  }

  void set(String &str,uint32 offset,uint32 arg_length)
  {
    DBUG_ASSERT(&str != this);
    free();
    Ptr=(char*) str.ptr()+offset; str_length=arg_length;
    if (str.Alloced_length)
      Alloced_length=str.Alloced_length-offset;
    str_charset=str.str_charset;
  }


  /**
     Points the internal buffer to the supplied one. The old buffer is freed.
     @param str Pointer to the new buffer.
     @param arg_length Length of the new buffer in characters, excluding any 
            null character.
     @param cs Character set to use for interpreting string data.
     @note The new buffer will not be null terminated.
  */
  inline void set(char *str,uint32 arg_length, CHARSET_INFO *cs)
  {
    free();
    Ptr=(char*) str; str_length=Alloced_length=arg_length;
    str_charset=cs;
  }
  inline void set(const char *str,uint32 arg_length, CHARSET_INFO *cs)
  {
    free();
    Ptr=(char*) str; str_length=arg_length;
    str_charset=cs;
  }
  bool set_ascii(const char *str, uint32 arg_length);
  inline void set_quick(char *str,uint32 arg_length, CHARSET_INFO *cs)
  {
    if (!alloced)
    {
      Ptr=(char*) str; str_length=Alloced_length=arg_length;
    }
    str_charset=cs;
  }
  bool set_int(longlong num, bool unsigned_flag, CHARSET_INFO *cs);
  bool set(int num, CHARSET_INFO *cs) { return set_int(num, false, cs); }
  bool set(uint num, CHARSET_INFO *cs) { return set_int(num, true, cs); }
  bool set(long num, CHARSET_INFO *cs) { return set_int(num, false, cs); }
  bool set(ulong num, CHARSET_INFO *cs) { return set_int(num, true, cs); }
  bool set(longlong num, CHARSET_INFO *cs) { return set_int(num, false, cs); }
  bool set(ulonglong num, CHARSET_INFO *cs) { return set_int((longlong)num, true, cs); }
  bool set_real(double num,uint decimals, CHARSET_INFO *cs);

  /* Take over handling of buffer from some other object */
  void reset(char *ptr_arg, uint32 length_arg, uint32 alloced_length_arg,
             CHARSET_INFO *cs)
  { 
    free();
    Ptr= ptr_arg;
    str_length= length_arg;
    Alloced_length= alloced_length_arg;
    str_charset= cs;
    alloced= ptr_arg != 0;
  }

  /* Forget about the buffer, let some other object handle it */
  char *release()
  {
    char *old= Ptr;
    Ptr=0; str_length= Alloced_length= extra_alloc= 0;
    alloced= thread_specific= 0;
    return old;
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
    str_length--;
    Ptr[str_length]= '\0';
    DBUG_ASSERT(strlen(Ptr) == str_length);
  }

  inline void free()
  {
    if (alloced)
    {
      alloced=0;
      my_free(Ptr);
    }
    Alloced_length= extra_alloc= 0;
    Ptr=0;
    str_length=0;				/* Safety */
  }
  inline bool alloc(size_t arg_length)
  {
    if (arg_length < Alloced_length)
      return 0;
    return real_alloc(arg_length);
  }
  bool real_alloc(size_t arg_length);			// Empties old string
  bool realloc_raw(size_t arg_length);
  bool realloc(size_t arg_length)
  {
    if (realloc_raw(arg_length))
      return TRUE;
    Ptr[arg_length]=0;        // This make other funcs shorter
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
  inline void shrink(size_t arg_length)
  {
    if (!is_alloced())
      return;
    if (ALIGN_SIZE(arg_length+1) < Alloced_length)
    {
      char *new_ptr;
      if (!(new_ptr=(char*)
            my_realloc(Ptr, arg_length,MYF((thread_specific ?
                                            MY_THREAD_SPECIFIC : 0)))))
      {
	Alloced_length = 0;
	real_alloc(arg_length);
      }
      else
      {
	Ptr=new_ptr;
	Alloced_length=(uint32)arg_length;
      }
    }
  }
  bool is_alloced() const { return alloced; }
  inline String& operator = (const String &s)
  {
    if (&s != this)
    {
      /*
        It is forbidden to do assignments like 
        some_string = substring_of_that_string
       */
      DBUG_ASSERT(!s.uses_buffer_owned_by(this));
      free();
      Ptr=s.Ptr ; str_length=s.str_length ; Alloced_length=s.Alloced_length;
      str_charset=s.str_charset;
    }
    return *this;
  }

  bool copy();					// Alloc string if not alloced
  bool copy(const String &s);			// Allocate new string
  bool copy(const char *s,size_t arg_length, CHARSET_INFO *cs);	// Allocate new string
  bool copy_or_move(const char *s,size_t arg_length, CHARSET_INFO *cs);
  static bool needs_conversion(uint32 arg_length,
  			       CHARSET_INFO *cs_from, CHARSET_INFO *cs_to,
			       uint32 *offset);
  static bool needs_conversion_on_storage(uint32 arg_length,
                                          CHARSET_INFO *cs_from,
                                          CHARSET_INFO *cs_to);
  bool copy_aligned(const char *s, uint32 arg_length, uint32 offset,
		    CHARSET_INFO *cs);
  bool set_or_copy_aligned(const char *s, uint32 arg_length, CHARSET_INFO *cs);
  bool copy(const char*s,uint32 arg_length, CHARSET_INFO *csfrom,
	    CHARSET_INFO *csto, uint *errors);
  bool copy(const String *str, CHARSET_INFO *tocs, uint *errors)
  {
    return copy(str->ptr(), str->length(), str->charset(), tocs, errors);
  }
  bool copy(CHARSET_INFO *tocs,
            CHARSET_INFO *fromcs, const char *src, uint32 src_length,
            uint32 nchars, String_copier *copier)
  {
    if (alloc(tocs->mbmaxlen * src_length))
      return true;
    str_length= copier->well_formed_copy(tocs, Ptr, Alloced_length,
                                         fromcs, src, src_length, nchars);
    str_charset= tocs;
    return false;
  }
  void move(String &s)
  {
    free();
    Ptr=s.Ptr ; str_length=s.str_length ; Alloced_length=s.Alloced_length;
    extra_alloc= s.extra_alloc;
    alloced= s.alloced;
    thread_specific= s.thread_specific;
    s.alloced= 0;
  }
  bool append(const String &s);
  bool append(const char *s);
  bool append(const LEX_STRING *ls) { return append(ls->str, (uint32) ls->length); }
  bool append(const LEX_CSTRING *ls) { return append(ls->str, (uint32) ls->length); }
  bool append(const char *s, size_t size);
  bool append(const char *s, uint arg_length, CHARSET_INFO *cs);
  bool append_ulonglong(ulonglong val);
  bool append_longlong(longlong val);
  bool append(IO_CACHE* file, uint32 arg_length);
  bool append_with_prefill(const char *s, uint32 arg_length, 
			   uint32 full_length, char fill_char);
  bool append_parenthesized(long nr, int radix= 10);
  int strstr(const String &search,uint32 offset=0); // Returns offset to substring or -1
  int strrstr(const String &search,uint32 offset=0); // Returns offset to substring or -1
  bool replace(uint32 offset,uint32 arg_length,const char *to,uint32 length);
  bool replace(uint32 offset,uint32 arg_length,const String &to);
  inline bool append(char chr)
  {
    if (str_length < Alloced_length)
    {
      Ptr[str_length++]=chr;
    }
    else
    {
      if (realloc_with_extra(str_length + 1))
	return 1;
      Ptr[str_length++]=chr;
    }
    return 0;
  }
  bool append_hex(const char *src, uint32 srclen)
  {
    for (const char *src_end= src + srclen ; src != src_end ; src++)
    {
      if (append(_dig_vec_lower[((uchar) *src) >> 4]) ||
          append(_dig_vec_lower[((uchar) *src) & 0x0F]))
        return true;
    }
    return false;
  }
  bool append_hex(const uchar *src, uint32 srclen)
  {
    return append_hex((const char*)src, srclen);
  }
  bool fill(uint32 max_length,char fill);
  void strip_sp();
  friend int sortcmp(const String *a,const String *b, CHARSET_INFO *cs);
  friend int stringcmp(const String *a,const String *b);
  friend String *copy_if_not_alloced(String *a,String *b,uint32 arg_length);
  friend class Field;
  uint32 numchars() const;
  int charpos(longlong i,uint32 offset=0);

  int reserve(uint32 space_needed)
  {
    return realloc(str_length + space_needed);
  }
  int reserve(uint32 space_needed, uint32 grow_by);

  /*
    The following append operations do NOT check alloced memory
    q_*** methods writes values of parameters itself
    qs_*** methods writes string representation of value
  */
  void q_append(const char c)
  {
    Ptr[str_length++] = c;
  }
  void q_append2b(const uint32 n)
  {
    int2store(Ptr + str_length, n);
    str_length += 2;
  }
  void q_append(const uint32 n)
  {
    int4store(Ptr + str_length, n);
    str_length += 4;
  }
  void q_append(double d)
  {
    float8store(Ptr + str_length, d);
    str_length += 8;
  }
  void q_append(double *d)
  {
    float8store(Ptr + str_length, *d);
    str_length += 8;
  }
  void q_append(const char *data, size_t data_len)
  {
    if (data_len)
      memcpy(Ptr + str_length, data, data_len);
    DBUG_ASSERT(str_length <= UINT_MAX32 - data_len);
    str_length += (uint)data_len;
  }

  void write_at_position(int position, uint32 value)
  {
    int4store(Ptr + position,value);
  }

  void qs_append(const char *str)
  {
    qs_append(str, (uint32)strlen(str));
  }
  void qs_append(const char *str, uint32 len);
  void qs_append(double d);
  void qs_append(double *d);
  inline void qs_append(const char c)
  {
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
    char *buff= Ptr + str_length;
    char *end= ll2str(i, buff, radix, 0);
    str_length+= (int) (end-buff);
  }

  /* Inline (general) functions used by the protocol functions */

  inline char *prep_append(uint32 arg_length, uint32 step_alloc)
  {
    uint32 new_length= arg_length + str_length;
    if (new_length > Alloced_length)
    {
      if (realloc(new_length + step_alloc))
        return 0;
    }
    uint32 old_length= str_length;
    str_length+= arg_length;
    return Ptr+ old_length;			/* Area to use */
  }


  inline bool append(const char *s, uint32 arg_length, uint32 step_alloc)
  {
    uint32 new_length= arg_length + str_length;
    if (new_length > Alloced_length && realloc(new_length + step_alloc))
      return TRUE;
    memcpy(Ptr+str_length, s, arg_length);
    str_length+= arg_length;
    return FALSE;
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

  bool append_for_single_quote(const char *st, uint len);
  bool append_for_single_quote(const String *s)
  {
    return append_for_single_quote(s->ptr(), s->length());
  }
  bool append_for_single_quote(const char *st)
  {
    size_t len= strlen(st);
    DBUG_ASSERT(len < UINT_MAX32);
    return append_for_single_quote(st, (uint32) len);
  }

  /* Swap two string objects. Efficient way to exchange data without memcpy. */
  void swap(String &s);

  inline bool uses_buffer_owned_by(const String *s) const
  {
    return (s->alloced && Ptr >= s->Ptr && Ptr < s->Ptr + s->str_length);
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
    for (const char *c= ptr(), *c_end= c + length(); c < c_end; c++)
    {
      if (!my_isascii(*c))
        return FALSE;
    }
    return TRUE;
  }
  bool bin_eq(const String *other) const
  {
    return length() == other->length() &&
           !memcmp(ptr(), other->ptr(), length());
  }
  bool eq(const String *other, CHARSET_INFO *cs) const
  {
    return !sortcmp(this, other, cs);
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
};


static inline bool check_if_only_end_space(CHARSET_INFO *cs,
                                           const char *str, 
                                           const char *end)
{
  return str+ cs->cset->scan(cs, str, end, MY_SEQ_SPACES) == end;
}

int append_query_string(CHARSET_INFO *csinfo, String *to,
                        const char *str, size_t len, bool no_backslash);

#endif /* SQL_STRING_INCLUDED */
