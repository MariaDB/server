/* Copyright (c) 2000, 2013, Oracle and/or its affiliates.
   Copyright (c) 2016, 2020, MariaDB

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

#ifdef USE_PRAGMA_IMPLEMENTATION
#pragma implementation				// gcc: Class implementation
#endif

#include <my_global.h>
#include <my_sys.h>
#include <m_string.h>
#include <m_ctype.h>
#include <mysql_com.h>

#include "sql_string.h"

/*****************************************************************************
** String functions
*****************************************************************************/

bool String::real_alloc(size_t length)
{
  size_t arg_length= ALIGN_SIZE(length + 1);
  DBUG_ASSERT(arg_length > length);
  if (arg_length <= length)
    return TRUE;                                 /* Overflow */
  str_length=0;
  if (Alloced_length < arg_length)
  {
    free();
    if (!(Ptr=(char*) my_malloc(arg_length,MYF(MY_WME |
                                               (thread_specific ?
                                                MY_THREAD_SPECIFIC : 0)))))
      return TRUE;
    DBUG_ASSERT(length < UINT_MAX32);
    Alloced_length=(uint32) arg_length;
    alloced=1;
  }
  Ptr[0]=0;
  return FALSE;
}


/**
   Allocates a new buffer on the heap for this String.

   - If the String's internal buffer is privately owned and heap allocated,
     one of the following is performed.

     - If the requested length is greater than what fits in the buffer, a new
       buffer is allocated, data moved and the old buffer freed.

     - If the requested length is less or equal to what fits in the buffer, a
       null character is inserted at the appropriate position.

   - If the String does not keep a private buffer on the heap, such a buffer
     will be allocated and the string copied accoring to its length, as found
     in String::length().
 
   For C compatibility, the new string buffer is null terminated.

   @param alloc_length The requested string size in characters, excluding any
   null terminator.

   @retval false Either the copy operation is complete or, if the size of the
   new buffer is smaller than the currently allocated buffer (if one exists),
   no allocation occurred.

   @retval true An error occurred when attempting to allocate memory.
*/
bool String::realloc_raw(size_t alloc_length)
{
  if (Alloced_length <= alloc_length)
  {
    char *new_ptr;
    uint32 len= ALIGN_SIZE(alloc_length+1);
    DBUG_ASSERT(len > alloc_length);
    if (len <= alloc_length)
      return TRUE;                                 /* Overflow */
    if (alloced)
    {
      if (!(new_ptr= (char*) my_realloc(Ptr,len,
                                        MYF(MY_WME |
                                            (thread_specific ?
                                             MY_THREAD_SPECIFIC : 0)))))
        return TRUE;				// Signal error
    }
    else if ((new_ptr= (char*) my_malloc(len,
                                         MYF(MY_WME |
                                             (thread_specific ?
                                              MY_THREAD_SPECIFIC : 0)))))
    {
      if (str_length > len - 1)
        str_length= 0;
      if (str_length)				// Avoid bugs in memcpy on AIX
	memcpy(new_ptr,Ptr,str_length);
      new_ptr[str_length]=0;
      alloced=1;
    }
    else
      return TRUE;			// Signal error
    Ptr= new_ptr;
    DBUG_ASSERT(len < UINT_MAX32);
    Alloced_length=  (uint32)len;
  }
  return FALSE;
}

bool String::set_int(longlong num, bool unsigned_flag, CHARSET_INFO *cs)
{
  uint l=20*cs->mbmaxlen+1;
  int base= unsigned_flag ? 10 : -10;

  if (alloc(l))
    return TRUE;
  str_length=(uint32) (cs->cset->longlong10_to_str)(cs,Ptr,l,base,num);
  str_charset=cs;
  return FALSE;
}

bool String::set_real(double num,uint decimals, CHARSET_INFO *cs)
{
  char buff[FLOATING_POINT_BUFFER];
  uint dummy_errors;
  size_t len;

  str_charset=cs;
  if (decimals >= FLOATING_POINT_DECIMALS)
  {
    len= my_gcvt(num, MY_GCVT_ARG_DOUBLE, sizeof(buff) - 1, buff, NULL);
    return copy(buff, (uint)len, &my_charset_latin1, cs, &dummy_errors);
  }
  len= my_fcvt(num, decimals, buff, NULL);
  return copy(buff, (uint32) len, &my_charset_latin1, cs,
              &dummy_errors);
}


bool String::copy()
{
  if (!alloced)
  {
    Alloced_length=0;				// Force realloc
    return realloc(str_length);
  }
  return FALSE;
}

/**
   Copies the internal buffer from str. If this String has a private heap
   allocated buffer where new data does not fit, a new buffer is allocated
   before copying and the old buffer freed. Character set information is also
   copied.
   
   @param str The string whose internal buffer is to be copied.
   
   @retval false Success.
   @retval true Memory allocation failed.
*/
bool String::copy(const String &str)
{
  if (alloc(str.str_length))
    return TRUE;
  if ((str_length=str.str_length))
    bmove(Ptr,str.Ptr,str_length);		// May be overlapping
  Ptr[str_length]=0;
  str_charset=str.str_charset;
  return FALSE;
}

bool String::copy(const char *str,size_t arg_length, CHARSET_INFO *cs)
{
  DBUG_ASSERT(arg_length < UINT_MAX32);
  if (alloc(arg_length))
    return TRUE;
  if (Ptr == str && arg_length == uint32(str_length))
  {
    /*
      This can happen in some cases. This code is here mainly to avoid
      warnings from valgrind, but can also be an indication of error.
    */
    DBUG_PRINT("warning", ("Copying string on itself: %p  %zu",
                           str, arg_length));
  }
  else if ((str_length=uint32(arg_length)))
    memcpy(Ptr,str,arg_length);
  Ptr[arg_length]=0;
  str_charset=cs;
  return FALSE;
}

/*
  Copy string, where strings may overlap.
  Same as String::copy, but use memmove instead of memcpy to avoid warnings
  from valgrind
*/

bool String::copy_or_move(const char *str,size_t arg_length, CHARSET_INFO *cs)
{
  DBUG_ASSERT(arg_length < UINT_MAX32);
  if (alloc(arg_length))
    return TRUE;
  if ((str_length=uint32(arg_length)))
    memmove(Ptr,str,arg_length);
  Ptr[arg_length]=0;
  str_charset=cs;
  return FALSE;
}


/*
  Checks that the source string can be just copied to the destination string
  without conversion.

  SYNPOSIS

  needs_conversion()
  arg_length		Length of string to copy.
  from_cs		Character set to copy from
  to_cs			Character set to copy to
  uint32 *offset	Returns number of unaligned characters.

  RETURN
   0  No conversion needed
   1  Either character set conversion or adding leading  zeros
      (e.g. for UCS-2) must be done

  NOTE
  to_cs may be NULL for "no conversion" if the system variable
  character_set_results is NULL.
*/

bool String::needs_conversion(uint32 arg_length,
			      CHARSET_INFO *from_cs,
			      CHARSET_INFO *to_cs,
			      uint32 *offset)
{
  *offset= 0;
  if (!to_cs ||
      (to_cs == &my_charset_bin) || 
      (to_cs == from_cs) ||
      my_charset_same(from_cs, to_cs) ||
      ((from_cs == &my_charset_bin) &&
       (!(*offset=(arg_length % to_cs->mbminlen)))))
    return FALSE;
  return TRUE;
}


/*
  Checks that the source string can just be copied to the destination string
  without conversion.
  Unlike needs_conversion it will require conversion on incoming binary data
  to ensure the data are verified for vailidity first.

  @param arg_length   Length of string to copy.
  @param from_cs      Character set to copy from
  @param to_cs        Character set to copy to

  @return conversion needed
*/
bool String::needs_conversion_on_storage(uint32 arg_length,
                                         CHARSET_INFO *cs_from,
                                         CHARSET_INFO *cs_to)
{
  uint32 offset;
  return (needs_conversion(arg_length, cs_from, cs_to, &offset) ||
          /* force conversion when storing a binary string */
          (cs_from == &my_charset_bin &&
          /* into a non-binary destination */
           cs_to != &my_charset_bin &&
           /* and any of the following is true :*/
           (
            /* it's a variable length encoding */
            cs_to->mbminlen != cs_to->mbmaxlen ||
            /* longer than 2 bytes : neither 1 byte nor ucs2 */
            cs_to->mbminlen > 2 ||
            /* and is not a multiple of the char byte size */
            0 != (arg_length % cs_to->mbmaxlen)
           )
          )
         );
}


/*
  Copy a multi-byte character sets with adding leading zeros.

  SYNOPSIS

  copy_aligned()
  str			String to copy
  arg_length		Length of string. This should NOT be dividable with
			cs->mbminlen.
  offset		arg_length % cs->mb_minlength
  cs			Character set for 'str'

  NOTES
    For real multi-byte, ascii incompatible charactser sets,
    like UCS-2, add leading zeros if we have an incomplete character.
    Thus, 
      SELECT _ucs2 0xAA 
    will automatically be converted into
      SELECT _ucs2 0x00AA

  RETURN
    0  ok
    1  error
*/

bool String::copy_aligned(const char *str,uint32 arg_length, uint32 offset,
			  CHARSET_INFO *cs)
{
  /* How many bytes are in incomplete character */
  offset= cs->mbminlen - offset; /* How many zeros we should prepend */
  DBUG_ASSERT(offset && offset != cs->mbminlen);

  uint32 aligned_length= arg_length + offset;
  if (alloc(aligned_length))
    return TRUE;
  
  /*
    Note, this is only safe for big-endian UCS-2.
    If we add little-endian UCS-2 sometimes, this code
    will be more complicated. But it's OK for now.
  */
  bzero((char*) Ptr, offset);
  memcpy(Ptr + offset, str, arg_length);
  Ptr[aligned_length]=0;
  /* str_length is always >= 0 as arg_length is != 0 */
  str_length= aligned_length;
  str_charset= cs;
  return FALSE;
}


bool String::set_or_copy_aligned(const char *str,uint32 arg_length,
				 CHARSET_INFO *cs)
{
  /* How many bytes are in incomplete character */
  uint32 offset= (arg_length % cs->mbminlen); 
  
  if (!offset)
  {
    /* All characters are complete, just use given string */
    set(str, arg_length, cs);
    return FALSE;
  }
  return copy_aligned(str, arg_length, offset, cs);
}


/**
   Copies the character data into this String, with optional character set
   conversion.

   @return
   FALSE ok
   TRUE  Could not allocate result buffer

*/

bool String::copy(const char *str, uint32 arg_length,
		  CHARSET_INFO *from_cs, CHARSET_INFO *to_cs, uint *errors)
{
  uint32 offset;

  DBUG_ASSERT(!str || str != Ptr || !alloced);
  
  if (!needs_conversion(arg_length, from_cs, to_cs, &offset))
  {
    *errors= 0;
    return copy(str, arg_length, to_cs);
  }
  if ((from_cs == &my_charset_bin) && offset)
  {
    *errors= 0;
    return copy_aligned(str, arg_length, offset, to_cs);
  }
  uint32 new_length= to_cs->mbmaxlen*arg_length;
  if (alloc(new_length))
    return TRUE;
  str_length=copy_and_convert((char*) Ptr, new_length, to_cs,
                              str, arg_length, from_cs, errors);
  str_charset=to_cs;
  return FALSE;
}


/*
  Set a string to the value of a latin1-string, keeping the original charset
  
  SYNOPSIS
    copy_or_set()
    str			String of a simple charset (latin1)
    arg_length		Length of string

  IMPLEMENTATION
    If string object is of a simple character set, set it to point to the
    given string.
    If not, make a copy and convert it to the new character set.

  RETURN
    0	ok
    1	Could not allocate result buffer

*/

bool String::set_ascii(const char *str, uint32 arg_length)
{
  if (str_charset->mbminlen == 1)
  {
    set(str, arg_length, str_charset);
    return 0;
  }
  uint dummy_errors;
  return copy(str, arg_length, &my_charset_latin1, str_charset, &dummy_errors);
}


/* This is used by mysql.cc */

bool String::fill(uint32 max_length,char fill_char)
{
  if (str_length > max_length)
    Ptr[str_length=max_length]=0;
  else
  {
    if (realloc(max_length))
      return TRUE;
    bfill(Ptr+str_length,max_length-str_length,fill_char);
    str_length=max_length;
  }
  return FALSE;
}

void String::strip_sp()
{
   while (str_length && my_isspace(str_charset,Ptr[str_length-1]))
    str_length--;
}

bool String::append(const String &s)
{
  if (s.length())
  {
    if (realloc_with_extra_if_needed(str_length+s.length()))
      return TRUE;
    memcpy(Ptr+str_length,s.ptr(),s.length());
    str_length+=s.length();
  }
  return FALSE;
}


/*
  Append an ASCII string to the a string of the current character set
*/

bool String::append(const char *s,size_t size)
{
  DBUG_ASSERT(size <= UINT_MAX32);
  uint32 arg_length= (uint32) size;
  if (!arg_length)
    return FALSE;

  /*
    For an ASCII incompatible string, e.g. UCS-2, we need to convert
  */
  if (str_charset->mbminlen > 1)
  {
    uint32 add_length=arg_length * str_charset->mbmaxlen;
    uint dummy_errors;
    if (realloc_with_extra_if_needed(str_length+ add_length))
      return TRUE;
    str_length+= copy_and_convert(Ptr+str_length, add_length, str_charset,
				  s, arg_length, &my_charset_latin1,
                                  &dummy_errors);
    return FALSE;
  }

  /*
    For an ASCII compatinble string we can just append.
  */
  if (realloc_with_extra_if_needed(str_length+arg_length))
    return TRUE;
  memcpy(Ptr+str_length,s,arg_length);
  str_length+=arg_length;
  return FALSE;
}


/*
  Append a 0-terminated ASCII string
*/

bool String::append(const char *s)
{
  return append(s, (uint) strlen(s));
}

bool String::append_longlong(longlong val)
{
  if (realloc(str_length+MAX_BIGINT_WIDTH+2))
    return TRUE;
  char *end= (char*) longlong10_to_str(val, (char*) Ptr + str_length, -10);
  str_length= (uint32)(end - Ptr);
  return FALSE;
}


bool String::append_ulonglong(ulonglong val)
{
  if (realloc(str_length+MAX_BIGINT_WIDTH+2))
    return TRUE;
  char *end= (char*) longlong10_to_str(val, (char*) Ptr + str_length, 10);
  str_length= (uint32) (end - Ptr);
  return FALSE;
}

/*
  Append a string in the given charset to the string
  with character set recoding
*/

bool String::append(const char *s,uint32 arg_length, CHARSET_INFO *cs)
{
  if (!arg_length)
    return false;

  uint32 offset;

  if (needs_conversion(arg_length, cs, str_charset, &offset))
  {
    uint32 add_length;
    if ((cs == &my_charset_bin) && offset)
    {
      DBUG_ASSERT(str_charset->mbminlen > offset);
      offset= str_charset->mbminlen - offset; // How many characters to pad
      add_length= arg_length + offset;
      if (realloc(str_length + add_length))
        return TRUE;
      bzero((char*) Ptr + str_length, offset);
      memcpy(Ptr + str_length + offset, s, arg_length);
      str_length+= add_length;
      return FALSE;
    }

    add_length= arg_length / cs->mbminlen * str_charset->mbmaxlen;
    uint dummy_errors;
    if (realloc_with_extra_if_needed(str_length + add_length)) 
      return TRUE;
    str_length+= copy_and_convert(Ptr+str_length, add_length, str_charset,
				  s, arg_length, cs, &dummy_errors);
  }
  else
  {
    if (realloc_with_extra_if_needed(str_length + arg_length)) 
      return TRUE;
    memcpy(Ptr + str_length, s, arg_length);
    str_length+= arg_length;
  }
  return FALSE;
}

bool String::append(IO_CACHE* file, uint32 arg_length)
{
  if (realloc_with_extra_if_needed(str_length+arg_length))
    return TRUE;
  if (my_b_read(file, (uchar*) Ptr + str_length, arg_length))
  {
    shrink(str_length);
    return TRUE;
  }
  str_length+=arg_length;
  return FALSE;
}


/**
  Append a parenthesized number to String.
  Used in various pieces of SHOW related code.

  @param nr     Number
  @param radix  Radix, optional parameter, 10 by default.
*/
bool String::append_parenthesized(long nr, int radix)
{
  char buff[64], *end;
  buff[0]= '(';
  end= int10_to_str(nr, buff + 1, radix);
  *end++ = ')';
  return append(buff, (uint) (end - buff));
}


bool String::append_with_prefill(const char *s,uint32 arg_length,
		 uint32 full_length, char fill_char)
{
  int t_length= arg_length > full_length ? arg_length : full_length;

  if (realloc_with_extra_if_needed(str_length + t_length))
    return TRUE;
  t_length= full_length - arg_length;
  if (t_length > 0)
  {
    bfill(Ptr+str_length, t_length, fill_char);
    str_length=str_length + t_length;
  }
  append(s, arg_length);
  return FALSE;
}

uint32 String::numchars() const
{
  return (uint32) str_charset->cset->numchars(str_charset, Ptr, Ptr+str_length);
}

int String::charpos(longlong i,uint32 offset)
{
  if (i <= 0)
    return (int)i;
  return (int)str_charset->cset->charpos(str_charset,Ptr+offset,Ptr+str_length,(size_t)i);
}

int String::strstr(const String &s,uint32 offset)
{
  if (s.length()+offset <= str_length)
  {
    if (!s.length())
      return ((int) offset);	// Empty string is always found

    const char *str = Ptr+offset;
    const char *search=s.ptr();
    const char *end=Ptr+str_length-s.length()+1;
    const char *search_end=s.ptr()+s.length();
skip:
    while (str != end)
    {
      if (*str++ == *search)
      {
	char *i,*j;
	i=(char*) str; j=(char*) search+1;
	while (j != search_end)
	  if (*i++ != *j++) goto skip;
	return (int) (str-Ptr) -1;
      }
    }
  }
  return -1;
}

/*
** Search string from end. Offset is offset to the end of string
*/

int String::strrstr(const String &s,uint32 offset)
{
  if (s.length() <= offset && offset <= str_length)
  {
    if (!s.length())
      return offset;				// Empty string is always found
    const char *str = Ptr+offset-1;
    const char *search=s.ptr()+s.length()-1;

    const char *end=Ptr+s.length()-2;
    const char *search_end=s.ptr()-1;
skip:
    while (str != end)
    {
      if (*str-- == *search)
      {
	char *i,*j;
	i=(char*) str; j=(char*) search-1;
	while (j != search_end)
	  if (*i-- != *j--) goto skip;
	return (int) (i-Ptr) +1;
      }
    }
  }
  return -1;
}

/*
  Replace substring with string
  If wrong parameter or not enough memory, do nothing
*/

bool String::replace(uint32 offset,uint32 arg_length,const String &to)
{
  return replace(offset,arg_length,to.ptr(),to.length());
}

bool String::replace(uint32 offset,uint32 arg_length,
                     const char *to, uint32 to_length)
{
  long diff = (long) to_length-(long) arg_length;
  if (offset+arg_length <= str_length)
  {
    if (diff < 0)
    {
      if (to_length)
	memcpy(Ptr+offset,to,to_length);
      bmove(Ptr+offset+to_length,Ptr+offset+arg_length,
	    str_length-offset-arg_length);
    }
    else
    {
      if (diff)
      {
	if (realloc_with_extra_if_needed(str_length+(uint32) diff))
	  return TRUE;
	bmove_upp((uchar*) Ptr+str_length+diff, (uchar*) Ptr+str_length,
		  str_length-offset-arg_length);
      }
      if (to_length)
	memcpy(Ptr+offset,to,to_length);
    }
    str_length+=(uint32) diff;
  }
  return FALSE;
}


// added by Holyfoot for "geometry" needs
int String::reserve(uint32 space_needed, uint32 grow_by)
{
  if (Alloced_length < str_length + space_needed)
  {
    if (realloc(Alloced_length + MY_MAX(space_needed, grow_by) - 1))
      return TRUE;
  }
  return FALSE;
}

void String::qs_append(const char *str, uint32 len)
{
  memcpy(Ptr + str_length, str, len + 1);
  str_length += len;
}

void String::qs_append(double d)
{
  char *buff = Ptr + str_length;
  str_length+= (uint32) my_gcvt(d, MY_GCVT_ARG_DOUBLE, FLOATING_POINT_BUFFER - 1, buff,
                       NULL);
}

void String::qs_append(double *d)
{
  double ld;
  float8get(ld, (char*) d);
  qs_append(ld);
}

void String::qs_append(int i)
{
  char *buff= Ptr + str_length;
  char *end= int10_to_str(i, buff, -10);
  str_length+= (int) (end-buff);
}

void String::qs_append(ulonglong i)
{
  char *buff= Ptr + str_length;
  char *end= longlong10_to_str(i, buff, 10);
  str_length+= (int) (end-buff);
}

/*
  Compare strings according to collation, without end space.

  SYNOPSIS
    sortcmp()
    s		First string
    t		Second string
    cs		Collation

  NOTE:
    Normally this is case sensitive comparison

  RETURN
  < 0	s < t
  0	s == t
  > 0	s > t
*/


int sortcmp(const String *s,const String *t, CHARSET_INFO *cs)
{
 return cs->coll->strnncollsp(cs,
                              (uchar *) s->ptr(),s->length(),
                              (uchar *) t->ptr(),t->length());
}


/*
  Compare strings byte by byte. End spaces are also compared.

  SYNOPSIS
    stringcmp()
    s		First string
    t		Second string

  NOTE:
    Strings are compared as a stream of uchars

  RETURN
  < 0	s < t
  0	s == t
  > 0	s > t
*/


int stringcmp(const String *s,const String *t)
{
  uint32 s_len=s->length(),t_len=t->length(),len=MY_MIN(s_len,t_len);
  int cmp= len ? memcmp(s->ptr(), t->ptr(), len) : 0;
  return (cmp) ? cmp : (int) (s_len - t_len);
}


/**
  Return a string which has the same value with "from" and
  which is safe to modify, trying to avoid unnecessary allocation
  and copying when possible.

  @param to           Buffer. Must not be a constant string.
  @param from         Some existing value. We'll try to reuse it.
                      Can be a constant or a variable string.
  @param from_length  The total size that will be possibly needed.
                      Note, can be 0.

  Note, in some cases "from" and "to" can point to the same object.

  If "from" is a variable string and its allocated memory is enough
  to store "from_length" bytes, then "from" is returned as is.

  If "from" is a variable string and its allocated memory is not enough
  to store "from_length" bytes, then "from" is reallocated and returned.

  Otherwise (if "from" is a constant string, or looks like a constant string),
  then "to" is reallocated to fit "from_length" bytes, the value is copied
  from "from" to "to", then "to" is returned.
*/
String *copy_if_not_alloced(String *to,String *from,uint32 from_length)
{
  DBUG_ASSERT(to);
  /*
    If "from" is a constant string, e.g.:
       SELECT INSERT('', <pos>, <length>, <replacement>);
    we should not return it. See MDEV-9332.

    The code below detects different string types:

    a. All constant strings have Alloced_length==0 and alloced==false.
       They point to a static memory array, or a mem_root memory,
       and should stay untouched until the end of their life cycle.
       Not safe to reuse.

    b. Some variable string have Alloced_length==0 and alloced==false initially,
       they are not bound to any char array and allocate space on the first use
       (and become #d). A typical example of such String is Item::str_value.
       This type of string could be reused, but there is no a way to distinguish
       them from the true constant strings (#a).
       Not safe to reuse.

    c. Some variable strings have Alloced_length>0 and alloced==false.
       They point to a fixed size writtable char array (typically on stack)
       initially but can later allocate more space on the heap when the
       fixed size array is too small (these strings become #d after allocation).
       Safe to reuse.

    d. Some variable strings have Alloced_length>0 and alloced==true.
       They already store data on the heap.
       Safe to reuse.

    e. Some strings can have Alloced_length==0 and alloced==true.
       This type of strings allocate space on the heap, but then are marked
       as constant strings using String::mark_as_const().
       A typical example - the result of a character set conversion
       of a constant string.
       Not safe to reuse.
  */
  if (from->Alloced_length > 0) // "from" is  #c or #d (not a constant)
  {
    if (from->Alloced_length >= from_length)
      return from; // #c or #d (large enough to store from_length bytes)

    if (from->alloced)
    {
      (void) from->realloc(from_length);
      return from; // #d (reallocated to fit from_length bytes)
    }
    /*
      "from" is of type #c. It currently points to a writtable char array
      (typically on stack), but is too small for "from_length" bytes.
      We need to reallocate either "from" or "to".

      "from" typically points to a temporary buffer inside Item_xxx::val_str(),
      or to Item::str_value, and thus is "less permanent" than "to".

      Reallocating "to" may give more benifits:
      - "to" can point to a "more permanent" storage and can be reused
        for multiple rows, e.g. str_buffer in Protocol::send_result_set_row(),
        which is passed to val_str() for all string type rows.
      - "from" can stay pointing to its original fixed size stack char array,
        and thus reduce the total amount of my_alloc/my_free.
    */
  }

  if (from == to)
  {
    /*
      Possible string types:
      #a  not possible (constants should not be passed as "to")
      #b  possible     (a fresh variable with no associated char buffer)
      #c  possible     (a variable with a char buffer,
                        in case it's smaller than fixed_length)
      #d  not possible (handled earlier)
      #e  not possible (constants should not be passed as "to")

      If a string of types #a or #e appears here, that means the caller made
      something wrong. Otherwise, it's safe to reallocate and return "to".

      Note, as we can't distinguish between #a and #b for sure,
      so we can't assert "not #a", but we can at least assert "not #e".
    */
    DBUG_ASSERT(!from->alloced || from->Alloced_length > 0); // Not #e

    (void) from->realloc(from_length);
    return from;
  }
  if (from->uses_buffer_owned_by(to))
  {
    DBUG_ASSERT(!from->alloced);
    DBUG_ASSERT(to->alloced);
    /*
      "from" is a constant string pointing to a fragment of alloced string "to":
        to=  xxxFFFyyy
      - FFF is the part of "to" pointed by "from"
      - xxx is the part of "to" before "from"
      - yyy is the part of "to" after "from"
    */
    uint32 xxx_length= (uint32) (from->ptr() - to->ptr());
    uint32 yyy_length= (uint32) (to->end() - from->end());
    DBUG_ASSERT(to->length() >= yyy_length);
    to->length(to->length() - yyy_length); // Remove the "yyy" part
    DBUG_ASSERT(to->length() >= xxx_length);
    to->replace(0, xxx_length, "", 0);     // Remove the "xxx" part
    to->realloc(from_length);
    to->str_charset= from->str_charset;
    return to;
  }
  if (to->realloc(from_length))
    return from;				// Actually an error
  if ((to->str_length=MY_MIN(from->str_length,from_length)))
    memcpy(to->Ptr,from->Ptr,to->str_length);
  to->str_charset=from->str_charset;
  return to; // "from" was of types #a, #b, #e, or small #c.
}


/****************************************************************************
  Help functions
****************************************************************************/

/**
  Copy string with HEX-encoding of "bad" characters.

  @details This functions copies the string pointed by "src"
  to the string pointed by "dst". Not more than "srclen" bytes
  are read from "src". Any sequences of bytes representing
  a not-well-formed substring (according to cs) are hex-encoded,
  and all well-formed substrings (according to cs) are copied as is.
  Not more than "dstlen" bytes are written to "dst". The number 
  of bytes written to "dst" is returned.
  
   @param      cs       character set pointer of the destination string
   @param[out] dst      destination string
   @param      dstlen   size of dst
   @param      src      source string
   @param      srclen   length of src

   @retval     result length
*/

size_t
my_copy_with_hex_escaping(CHARSET_INFO *cs,
                          char *dst, size_t dstlen,
                          const char *src, size_t srclen)
{
  const char *srcend= src + srclen;
  char *dst0= dst;

  for ( ; src < srcend ; )
  {
    size_t chlen;
    if ((chlen= my_ismbchar(cs, src, srcend)))
    {
      if (dstlen < chlen)
        break; /* purecov: inspected */
      memcpy(dst, src, chlen);
      src+= chlen;
      dst+= chlen;
      dstlen-= chlen;
    }
    else if (*src & 0x80)
    {
      if (dstlen < 4)
        break; /* purecov: inspected */
      *dst++= '\\';
      *dst++= 'x';
      *dst++= _dig_vec_upper[((unsigned char) *src) >> 4];
      *dst++= _dig_vec_upper[((unsigned char) *src) & 15];
      src++;
      dstlen-= 4;
    }
    else
    {
      if (dstlen < 1)
        break; /* purecov: inspected */
      *dst++= *src++;
      dstlen--;
    }
  }
  return dst - dst0;
}


/*
  Copy a string,
  with optional character set conversion,
  with optional left padding (for binary -> UCS2 conversion)

  Bad input bytes are replaced to '?'.

  The string that is written to "to" is always well-formed.

  @param to                  The destination string
  @param to_length           Space available in "to"
  @param to_cs               Character set of the "to" string
  @param from                The source string
  @param from_length         Length of the "from" string
  @param from_cs             Character set of the "from" string
  @param nchars              Copy not more than "nchars" characters

  The members as set as follows:
  m_well_formed_error_pos    To the position when "from" is not well formed
                             or NULL otherwise.
  m_cannot_convert_error_pos To the position where a not convertable
                             character met, or NULL otherwise.
  m_source_end_pos           To the position where scanning of the "from"
                             string stopped.

  @returns                   number of bytes that were written to 'to'
*/
uint
String_copier::well_formed_copy(CHARSET_INFO *to_cs,
                                char *to, uint to_length,
                                CHARSET_INFO *from_cs,
                                const char *from, uint from_length,
                                uint nchars)
{
  if ((to_cs == &my_charset_bin) || 
      (from_cs == &my_charset_bin) ||
      (to_cs == from_cs) ||
      my_charset_same(from_cs, to_cs))
  {
    m_cannot_convert_error_pos= NULL;
    return (uint) to_cs->cset->copy_fix(to_cs, to, to_length, from, from_length,
                                 nchars, this);
  }
  return (uint) my_convert_fix(to_cs, to, to_length, from_cs, from, from_length,
                        nchars, this, this);
}



/*
  Append characters to a single-quoted string '...', escaping special
  characters with backslashes as necessary.
  Does not add the enclosing quotes, this is left up to caller.
*/
#define APPEND(X)   if (append(X)) return 1; else break
bool String::append_for_single_quote(const char *st, uint len)
{
  const char *end= st+len;
  for (; st < end; st++)
  {
    uchar c= *st;
    switch (c)
    {
    case '\\':   APPEND(STRING_WITH_LEN("\\\\"));
    case '\0':   APPEND(STRING_WITH_LEN("\\0"));
    case '\'':   APPEND(STRING_WITH_LEN("\\'"));
    case '\n':   APPEND(STRING_WITH_LEN("\\n"));
    case '\r':   APPEND(STRING_WITH_LEN("\\r"));
    case '\032': APPEND(STRING_WITH_LEN("\\Z"));
    default:     APPEND(c);
    }
  }
  return 0;
}

void String::print(String *str) const
{
  str->append_for_single_quote(Ptr, str_length);
}


void String::print_with_conversion(String *print, CHARSET_INFO *cs) const
{
  StringBuffer<256> tmp(cs);
  uint errors= 0;
  tmp.copy(this, cs, &errors);
  tmp.print(print);
}


/*
  Exchange state of this object and argument.

  SYNOPSIS
    String::swap()

  RETURN
    Target string will contain state of this object and vice versa.
*/

void String::swap(String &s)
{
  swap_variables(char *, Ptr, s.Ptr);
  swap_variables(uint32, str_length, s.str_length);
  swap_variables(uint32, Alloced_length, s.Alloced_length);
  swap_variables(bool, alloced, s.alloced);
  swap_variables(CHARSET_INFO*, str_charset, s.str_charset);
}


/**
  Convert string to printable ASCII string

  @details This function converts input string "from" replacing non-ASCII bytes
  with hexadecimal sequences ("\xXX") optionally appending "..." to the end of
  the resulting string.
  This function used in the ER_TRUNCATED_WRONG_VALUE_FOR_FIELD error messages,
  e.g. when a string cannot be converted to a result charset.


  @param    to          output buffer
  @param    to_len      size of the output buffer (8 bytes or greater)
  @param    from        input string
  @param    from_len    size of the input string
  @param    from_cs     input charset
  @param    nbytes      maximal number of bytes to convert (from_len if 0)

  @return   number of bytes in the output string
*/

uint convert_to_printable(char *to, size_t to_len,
                          const char *from, size_t from_len,
                          CHARSET_INFO *from_cs, size_t nbytes /*= 0*/)
{
  /* needs at least 8 bytes for '\xXX...' and zero byte */
  DBUG_ASSERT(to_len >= 8);

  char *t= to;
  char *t_end= to + to_len - 1; // '- 1' is for the '\0' at the end
  const char *f= from;
  const char *f_end= from + (nbytes ? MY_MIN(from_len, nbytes) : from_len);
  char *dots= to; // last safe place to append '...'

  if (!f || t == t_end)
    return 0;

  for (; t < t_end && f < f_end; f++)
  {
    /*
      If the source string is ASCII compatible (mbminlen==1)
      and the source character is in ASCII printable range (0x20..0x7F),
      then display the character as is.
      
      Otherwise, if the source string is not ASCII compatible (e.g. UCS2),
      or the source character is not in the printable range,
      then print the character using HEX notation.
    */
    if (((unsigned char) *f) >= 0x20 &&
        ((unsigned char) *f) <= 0x7F &&
        from_cs->mbminlen == 1)
    {
      *t++= *f;
    }
    else
    {
      if (t_end - t < 4) // \xXX
        break;
      *t++= '\\';
      *t++= 'x';
      *t++= _dig_vec_upper[((unsigned char) *f) >> 4];
      *t++= _dig_vec_upper[((unsigned char) *f) & 0x0F];
    }
    if (t_end - t >= 3) // '...'
      dots= t;
  }
  if (f < from + from_len)
    memcpy(dots, STRING_WITH_LEN("...\0"));
  else
    *t= '\0';
  return (uint) (t - to);
}
