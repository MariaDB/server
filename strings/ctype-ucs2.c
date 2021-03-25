/* Copyright (c) 2003, 2013, Oracle and/or its affiliates
   Copyright (c) 2009, 2016, MariaDB

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public
   License as published by the Free Software Foundation; version 2
   of the License.
   
   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.
   
   You should have received a copy of the GNU Library General Public
   License along with this library; if not, write to the Free
   Software Foundation, Inc., 51 Franklin St, Fifth Floor, Boston,
   MA 02110-1335  USA */

/* UCS2 support. Written by Alexander Barkov <bar@mysql.com> */

#include "strings_def.h"
#include <m_ctype.h>
#include <my_sys.h>
#include <stdarg.h>

#include "ctype-unidata.h"


#if defined(HAVE_CHARSET_utf16) || defined(HAVE_CHARSET_ucs2)
#define HAVE_CHARSET_mb2
#endif


#if defined(HAVE_CHARSET_mb2) || defined(HAVE_CHARSET_utf32)
#define HAVE_CHARSET_mb2_or_mb4
#endif


#ifndef EILSEQ
#define EILSEQ ENOENT
#endif

#undef  ULONGLONG_MAX
#define ULONGLONG_MAX                (~(ulonglong) 0)
#define MAX_NEGATIVE_NUMBER        ((ulonglong) 0x8000000000000000LL)
#define INIT_CNT  9
#define LFACTOR   1000000000ULL
#define LFACTOR1  10000000000ULL
#define LFACTOR2  100000000000ULL

#if defined(HAVE_CHARSET_utf32) || defined(HAVE_CHARSET_mb2)
static unsigned long lfactor[9]=
{ 1L, 10L, 100L, 1000L, 10000L, 100000L, 1000000L, 10000000L, 100000000L };
#endif


#ifdef HAVE_CHARSET_mb2_or_mb4
static size_t
my_caseup_str_mb2_or_mb4(CHARSET_INFO * cs  __attribute__((unused)), 
                         char * s __attribute__((unused)))
{
  DBUG_ASSERT(0);
  return 0;
}


static size_t
my_casedn_str_mb2_or_mb4(CHARSET_INFO *cs __attribute__((unused)), 
                         char * s __attribute__((unused)))
{
  DBUG_ASSERT(0);
  return 0;
}


static int
my_strcasecmp_mb2_or_mb4(CHARSET_INFO *cs __attribute__((unused)),
                         const char *s __attribute__((unused)),
                         const char *t __attribute__((unused)))
{
  DBUG_ASSERT(0);
  return 0;
}


typedef enum
{
  MY_CHAR_COPY_OK=       0, /* The character was Okey */
  MY_CHAR_COPY_ERROR=    1, /* The character was not Ok, and could not fix */
  MY_CHAR_COPY_FIXED=    2  /* The character was not Ok, was fixed to '?' */
} my_char_copy_status_t;


/*
  Copies an incomplete character, lef-padding it with 0x00 bytes.
  
  @param cs           Character set
  @param dst          The destination string
  @param dst_length   Space available in dst
  @param src          The source string
  @param src_length   Length of src
  @param nchars       Copy not more than nchars characters.
                      The "nchars" parameter of the caller.
                      Only 0 and non-0 are important here.
  @param fix          What to do if after zero-padding didn't get a valid 
                      character:
                      - FALSE - exit with error.
                      - TRUE  - try to put '?' instead.
  
  @return  MY_CHAR_COPY_OK     if after zero-padding got a valid character.
                               cs->mbmaxlen bytes were written to "dst".
  @return  MY_CHAR_COPY_FIXED  if after zero-padding did not get a valid
                               character, but wrote '?' to the destination
                               string instead.
                               cs->mbminlen bytes were written to "dst".
  @return  MY_CHAR_COPY_ERROR  If failed and nothing was written to "dst".
                               Possible reasons:
                               - dst_length was too short
                               - nchars was 0
                               - the character after padding appeared not
                                 to be valid, and could not fix it to '?'.
*/
static my_char_copy_status_t
my_copy_incomplete_char(CHARSET_INFO *cs,
                        char *dst, size_t dst_length,
                        const char *src, size_t src_length,
                        size_t nchars, my_bool fix)
{
  size_t pad_length;
  size_t src_offset= src_length % cs->mbminlen;
  if (dst_length < cs->mbminlen || !nchars)
    return MY_CHAR_COPY_ERROR;

  pad_length= cs->mbminlen - src_offset;
  bzero(dst, pad_length);
  memmove(dst + pad_length, src, src_offset);
  /*
    In some cases left zero-padding can create an incorrect character.
    For example:
      INSERT INTO t1 (utf32_column) VALUES (0x110000);
    We'll pad the value to 0x00110000, which is a wrong UTF32 sequence!
    The valid characters range is limited to 0x00000000..0x0010FFFF.
    
    Make sure we didn't pad to an incorrect character.
  */
  if (cs->cset->charlen(cs, (uchar *) dst, (uchar *) dst + cs->mbminlen) ==
      (int) cs->mbminlen)
    return MY_CHAR_COPY_OK;

  if (fix &&
      cs->cset->wc_mb(cs, '?', (uchar *) dst, (uchar *) dst + cs->mbminlen) ==
      (int) cs->mbminlen)
    return MY_CHAR_COPY_FIXED;

  return MY_CHAR_COPY_ERROR;
}


/*
  Copy an UCS2/UTF16/UTF32 string, fix bad characters.
*/
static size_t
my_copy_fix_mb2_or_mb4(CHARSET_INFO *cs,
                       char *dst, size_t dst_length,
                       const char *src, size_t src_length,
                       size_t nchars, MY_STRCOPY_STATUS *status)
{
  size_t length2, src_offset= src_length % cs->mbminlen;
  my_char_copy_status_t padstatus;
  
  if (!src_offset)
    return  my_copy_fix_mb(cs, dst, dst_length,
                               src, src_length, nchars, status);
  if ((padstatus= my_copy_incomplete_char(cs, dst, dst_length,
                                          src, src_length, nchars, TRUE)) ==
      MY_CHAR_COPY_ERROR)
  {
    status->m_source_end_pos= status->m_well_formed_error_pos= src;
    return 0;
  }
  length2= my_copy_fix_mb(cs, dst + cs->mbminlen, dst_length - cs->mbminlen,
                          src + src_offset, src_length - src_offset,
                          nchars - 1, status);
  if (padstatus == MY_CHAR_COPY_FIXED)
    status->m_well_formed_error_pos= src;
  return cs->mbminlen /* The left-padded character */ + length2;
}


static long
my_strntol_mb2_or_mb4(CHARSET_INFO *cs,
                      const char *nptr, size_t l, int base,
                      char **endptr, int *err)
{
  int      negative= 0;
  int      overflow;
  int      cnv;
  my_wc_t  wc;
  my_charset_conv_mb_wc mb_wc= cs->cset->mb_wc;
  register unsigned int cutlim;
  register uint32 cutoff;
  register uint32 res;
  register const uchar *s= (const uchar*) nptr;
  register const uchar *e= (const uchar*) nptr+l;
  const uchar *save;
  
  *err= 0;
  do
  {
    if ((cnv= mb_wc(cs, &wc, s, e)) > 0)
    {
      switch (wc)
      {
        case ' ' : break;
        case '\t': break;
        case '-' : negative= !negative; break;
        case '+' : break;
        default  : goto bs;
      }
    } 
    else /* No more characters or bad multibyte sequence */
    {
      if (endptr != NULL )
        *endptr= (char*) s;
      err[0]= (cnv==MY_CS_ILSEQ) ? EILSEQ : EDOM;
      return 0;
    } 
    s+= cnv;
  } while (1);
  
bs:

  overflow= 0;
  res= 0;
  save= s;
  cutoff= ((uint32)~0L) / (uint32) base;
  cutlim= (uint) (((uint32)~0L) % (uint32) base);
  
  do {
    if ((cnv= mb_wc(cs, &wc, s, e)) > 0)
    {
      s+= cnv;
      if (wc >= '0' && wc <= '9')
        wc-= '0';
      else if (wc >= 'A' && wc <= 'Z')
        wc= wc - 'A' + 10;
      else if (wc >= 'a' && wc <= 'z')
        wc= wc - 'a' + 10;
      else
        break;
      if ((int)wc >= base)
        break;
      if (res > cutoff || (res == cutoff && wc > cutlim))
        overflow= 1;
      else
      {
        res*= (uint32) base;
        res+= wc;
      }
    }
    else if (cnv == MY_CS_ILSEQ)
    {
      if (endptr !=NULL )
        *endptr = (char*) s;
      err[0]= EILSEQ;
      return 0;
    } 
    else
    {
      /* No more characters */
      break;
    }
  } while(1);
  
  if (endptr != NULL)
    *endptr = (char *) s;
  
  if (s == save)
  {
    err[0]= EDOM;
    return 0L;
  }
  
  if (negative)
  {
    if (res > (uint32) INT_MIN32)
      overflow= 1;
  }
  else if (res > INT_MAX32)
    overflow= 1;
  
  if (overflow)
  {
    err[0]= ERANGE;
    return negative ? INT_MIN32 : INT_MAX32;
  }
  
  return (negative ? -((long) res) : (long) res);
}


static ulong
my_strntoul_mb2_or_mb4(CHARSET_INFO *cs,
                       const char *nptr, size_t l, int base, 
                       char **endptr, int *err)
{
  int      negative= 0;
  int      overflow;
  int      cnv;
  my_wc_t  wc;
  my_charset_conv_mb_wc mb_wc= cs->cset->mb_wc;
  register unsigned int cutlim;
  register uint32 cutoff;
  register uint32 res;
  register const uchar *s= (const uchar*) nptr;
  register const uchar *e= (const uchar*) nptr + l;
  const uchar *save;
  
  *err= 0;
  do
  {
    if ((cnv= mb_wc(cs, &wc, s, e)) > 0)
    {
      switch (wc)
      {
        case ' ' : break;
        case '\t': break;
        case '-' : negative= !negative; break;
        case '+' : break;
        default  : goto bs;
      }
    } 
    else /* No more characters or bad multibyte sequence */
    {
      if (endptr !=NULL )
        *endptr= (char*)s;
      err[0]= (cnv == MY_CS_ILSEQ) ? EILSEQ : EDOM;
      return 0;
    } 
    s+= cnv;
  } while (1);
  
bs:

  overflow= 0;
  res= 0;
  save= s;
  cutoff= ((uint32)~0L) / (uint32) base;
  cutlim= (uint) (((uint32)~0L) % (uint32) base);
  
  do
  {
    if ((cnv= mb_wc(cs, &wc, s, e)) > 0)
    {
      s+= cnv;
      if (wc >= '0' && wc <= '9')
        wc-= '0';
      else if (wc >= 'A' && wc <= 'Z')
        wc= wc - 'A' + 10;
      else if (wc >= 'a' && wc <= 'z')
        wc= wc - 'a' + 10;
      else
        break;
      if ((int) wc >= base)
        break;
      if (res > cutoff || (res == cutoff && wc > cutlim))
        overflow = 1;
      else
      {
        res*= (uint32) base;
        res+= wc;
      }
    }
    else if (cnv == MY_CS_ILSEQ)
    {
      if (endptr != NULL )
        *endptr= (char*)s;
      err[0]= EILSEQ;
      return 0;
    } 
    else
    {
      /* No more characters */
      break;
    }
  } while(1);
  
  if (endptr != NULL)
    *endptr= (char *) s;
  
  if (s == save)
  {
    err[0]= EDOM;
    return 0L;
  }
  
  if (overflow)
  {
    err[0]= (ERANGE);
    return (~(uint32) 0);
  }
  
  return (negative ? -((long) res) : (long) res);
}


static longlong 
my_strntoll_mb2_or_mb4(CHARSET_INFO *cs,
                       const char *nptr, size_t l, int base,
                       char **endptr, int *err)
{
  int      negative=0;
  int      overflow;
  int      cnv;
  my_wc_t  wc;
  my_charset_conv_mb_wc mb_wc= cs->cset->mb_wc;
  register ulonglong    cutoff;
  register unsigned int cutlim;
  register ulonglong    res;
  register const uchar *s= (const uchar*) nptr;
  register const uchar *e= (const uchar*) nptr+l;
  const uchar *save;
  
  *err= 0;
  do
  {
    if ((cnv= mb_wc(cs, &wc, s, e)) > 0)
    {
      switch (wc)
      {
        case ' ' : break;
        case '\t': break;
        case '-' : negative= !negative; break;
        case '+' : break;
        default  : goto bs;
      }
    } 
    else /* No more characters or bad multibyte sequence */
    {
      if (endptr !=NULL )
        *endptr = (char*)s;
      err[0] = (cnv==MY_CS_ILSEQ) ? EILSEQ : EDOM;
      return 0;
    } 
    s+=cnv;
  } while (1);
  
bs:

  overflow = 0;
  res = 0;
  save = s;
  cutoff = (~(ulonglong) 0) / (unsigned long int) base;
  cutlim = (uint) ((~(ulonglong) 0) % (unsigned long int) base);

  do {
    if ((cnv= mb_wc(cs, &wc, s, e)) > 0)
    {
      s+=cnv;
      if ( wc>='0' && wc<='9')
        wc -= '0';
      else if ( wc>='A' && wc<='Z')
        wc = wc - 'A' + 10;
      else if ( wc>='a' && wc<='z')
        wc = wc - 'a' + 10;
      else
        break;
      if ((int)wc >= base)
        break;
      if (res > cutoff || (res == cutoff && wc > cutlim))
        overflow = 1;
      else
      {
        res *= (ulonglong) base;
        res += wc;
      }
    }
    else if (cnv==MY_CS_ILSEQ)
    {
      if (endptr !=NULL )
        *endptr = (char*)s;
      err[0]=EILSEQ;
      return 0;
    } 
    else
    {
      /* No more characters */
      break;
    }
  } while(1);
  
  if (endptr != NULL)
    *endptr = (char *) s;
  
  if (s == save)
  {
    err[0]=EDOM;
    return 0L;
  }
  
  if (negative)
  {
    if (res  > (ulonglong) LONGLONG_MIN)
      overflow = 1;
  }
  else if (res > (ulonglong) LONGLONG_MAX)
    overflow = 1;
  
  if (overflow)
  {
    err[0]=ERANGE;
    return negative ? LONGLONG_MIN : LONGLONG_MAX;
  }
  
  return (negative ? -((longlong)res) : (longlong)res);
}


static ulonglong
my_strntoull_mb2_or_mb4(CHARSET_INFO *cs,
                        const char *nptr, size_t l, int base,
                        char **endptr, int *err)
{
  int      negative= 0;
  int      overflow;
  int      cnv;
  my_wc_t  wc;
  my_charset_conv_mb_wc mb_wc= cs->cset->mb_wc;
  register ulonglong    cutoff;
  register unsigned int cutlim;
  register ulonglong    res;
  register const uchar *s= (const uchar*) nptr;
  register const uchar *e= (const uchar*) nptr + l;
  const uchar *save;
  
  *err= 0;
  do
  {
    if ((cnv= mb_wc(cs, &wc, s, e)) > 0)
    {
      switch (wc)
      {
        case ' ' : break;
        case '\t': break;
        case '-' : negative= !negative; break;
        case '+' : break;
        default  : goto bs;
      }
    } 
    else /* No more characters or bad multibyte sequence */
    {
      if (endptr !=NULL )
        *endptr = (char*)s;
      err[0]= (cnv==MY_CS_ILSEQ) ? EILSEQ : EDOM;
      return 0;
    } 
    s+=cnv;
  } while (1);
  
bs:

  overflow = 0;
  res = 0;
  save = s;
  cutoff = (~(ulonglong) 0) / (unsigned long int) base;
  cutlim = (uint) ((~(ulonglong) 0) % (unsigned long int) base);

  do
  {
    if ((cnv= mb_wc(cs, &wc, s, e)) > 0)
    {
      s+=cnv;
      if ( wc>='0' && wc<='9')
        wc -= '0';
      else if ( wc>='A' && wc<='Z')
        wc = wc - 'A' + 10;
      else if ( wc>='a' && wc<='z')
        wc = wc - 'a' + 10;
      else
        break;
      if ((int)wc >= base)
        break;
      if (res > cutoff || (res == cutoff && wc > cutlim))
        overflow = 1;
      else
      {
        res *= (ulonglong) base;
        res += wc;
      }
    }
    else if (cnv==MY_CS_ILSEQ)
    {
      if (endptr !=NULL )
        *endptr = (char*)s;
      err[0]= EILSEQ;
      return 0;
    } 
    else
    {
      /* No more characters */
      break;
    }
  } while(1);
  
  if (endptr != NULL)
    *endptr = (char *) s;
  
  if (s == save)
  {
    err[0]= EDOM;
    return 0L;
  }
  
  if (overflow)
  {
    err[0]= ERANGE;
    return (~(ulonglong) 0);
  }

  return (negative ? -((longlong) res) : (longlong) res);
}


static double
my_strntod_mb2_or_mb4(CHARSET_INFO *cs,
                      char *nptr, size_t length, 
                      char **endptr, int *err)
{
  char     buf[256];
  double   res;
  register char *b= buf;
  register const uchar *s= (const uchar*) nptr;
  const uchar *end;
  my_wc_t  wc;
  my_charset_conv_mb_wc mb_wc= cs->cset->mb_wc;
  int     cnv;

  *err= 0;
  /* Cut too long strings */
  if (length >= sizeof(buf))
    length= sizeof(buf) - 1;
  end= s + length;

  while ((cnv= mb_wc(cs, &wc, s, end)) > 0)
  {
    s+= cnv;
    if (wc > (int) (uchar) 'e' || !wc)
      break;                                        /* Can't be part of double */
    *b++= (char) wc;
  }

  *endptr= b;
  res= my_strtod(buf, endptr, err);
  *endptr= nptr + cs->mbminlen * (size_t) (*endptr - buf);
  return res;
}


static ulonglong
my_strntoull10rnd_mb2_or_mb4(CHARSET_INFO *cs,
                             const char *nptr, size_t length,
                             int unsign_fl,
                             char **endptr, int *err)
{
  char  buf[256], *b= buf;
  ulonglong res;
  const uchar *end, *s= (const uchar*) nptr;
  my_wc_t  wc;
  my_charset_conv_mb_wc mb_wc= cs->cset->mb_wc;
  int     cnv;

  /* Cut too long strings */
  if (length >= sizeof(buf))
    length= sizeof(buf)-1;
  end= s + length;

  while ((cnv= mb_wc(cs, &wc, s, end)) > 0)
  {
    s+= cnv;
    if (wc > (int) (uchar) 'e' || !wc)
      break;                            /* Can't be a number part */
    *b++= (char) wc;
  }

  res= my_strntoull10rnd_8bit(cs, buf, b - buf, unsign_fl, endptr, err);
  *endptr= (char*) nptr + cs->mbminlen * (size_t) (*endptr - buf);
  return res;
}


/*
  This is a fast version optimized for the case of radix 10 / -10
*/

static size_t
my_l10tostr_mb2_or_mb4(CHARSET_INFO *cs,
                       char *dst, size_t len, int radix, long int val)
{
  char buffer[66];
  register char *p, *db, *de;
  long int new_val;
  int  sl= 0;
  unsigned long int uval = (unsigned long int) val;
  
  p= &buffer[sizeof(buffer) - 1];
  *p= '\0';
  
  if (radix < 0)
  {
    if (val < 0)
    {
      sl= 1;
      /* Avoid integer overflow in (-val) for LONGLONG_MIN (BUG#31799). */
      uval  = (unsigned long int)0 - uval;
    }
  }
  
  new_val = (long) (uval / 10);
  *--p    = '0'+ (char) (uval - (unsigned long) new_val * 10);
  val= new_val;
  
  while (val != 0)
  {
    new_val= val / 10;
    *--p= '0' + (char) (val - new_val * 10);
    val= new_val;
  }
  
  if (sl)
  {
    *--p= '-';
  }
  
  for ( db= dst, de= dst + len ; (dst < de) && *p ; p++)
  {
    int cnvres= cs->cset->wc_mb(cs,(my_wc_t)p[0],(uchar*) dst, (uchar*) de);
    if (cnvres > 0)
      dst+= cnvres;
    else
      break;
  }
  return (int) (dst - db);
}


static size_t
my_ll10tostr_mb2_or_mb4(CHARSET_INFO *cs,
                        char *dst, size_t len, int radix, longlong val)
{
  char buffer[65];
  register char *p, *db, *de;
  long long_val;
  int sl= 0;
  ulonglong uval= (ulonglong) val;
  
  if (radix < 0)
  {
    if (val < 0)
    {
      sl= 1;
      /* Avoid integer overflow in (-val) for LONGLONG_MIN (BUG#31799). */
      uval = (ulonglong)0 - uval;
    }
  }
  
  p= &buffer[sizeof(buffer)-1];
  *p='\0';
  
  if (uval == 0)
  {
    *--p= '0';
    goto cnv;
  }
  
  while (uval > (ulonglong) LONG_MAX)
  {
    ulonglong quo= uval/(uint) 10;
    uint rem= (uint) (uval- quo* (uint) 10);
    *--p= '0' + rem;
    uval= quo;
  }
  
  long_val= (long) uval;
  while (long_val != 0)
  {
    long quo= long_val/10;
    *--p= (char) ('0' + (long_val - quo*10));
    long_val= quo;
  }
  
cnv:
  if (sl)
  {
    *--p= '-';
  }
  
  for ( db= dst, de= dst + len ; (dst < de) && *p ; p++)
  {
    int cnvres= cs->cset->wc_mb(cs, (my_wc_t) p[0], (uchar*) dst, (uchar*) de);
    if (cnvres > 0)
      dst+= cnvres;
    else
      break;
  }
  return (int) (dst -db);
}

#endif /* HAVE_CHARSET_mb2_or_mb4 */


#ifdef HAVE_CHARSET_mb2
/**
  Convert a Unicode code point to a digit.
  @param      wc  - the input Unicode code point
  @param[OUT] c   - the output character representing the digit value 0..9

  @return   0     - if wc is a good digit
  @return   1     - if wc is not a digit
*/
static inline my_bool
wc2digit_uchar(uchar *c, my_wc_t wc)
{
  return wc > '9' || (c[0]= (uchar) (wc - '0')) > 9;
}


static longlong
my_strtoll10_mb2(CHARSET_INFO *cs __attribute__((unused)),
                 const char *nptr, char **endptr, int *error)
{
  const uchar *s, *end, *start, *n_end, *true_end;
  uchar UNINIT_VAR(c);
  unsigned long i, j, k;
  ulonglong li;
  int negative;
  ulong cutoff, cutoff2, cutoff3;
  my_wc_t wc;
  int res;
  my_charset_conv_mb_wc mb_wc= cs->cset->mb_wc;

  s= (const uchar *) nptr;
  /* If fixed length string */
  if (endptr)
  {
    /*
      Make sure string length is even.
      Odd length indicates a bug in the caller.
      Assert in debug, round in production.
    */
    DBUG_ASSERT((*endptr - (const char *) s) % 2 == 0);
    end= s + ((*endptr - (const char*) s) / 2) * 2;

    for ( ; ; ) /* Skip leading spaces and tabs */
    {
      if ((res= mb_wc(cs, &wc, s, end)) <= 0)
        goto no_conv;
      s+= res;
      if (wc != ' ' && wc != '\t')
        break;
    }
  }
  else
  {
     /* We don't support null terminated strings in UCS2 */
     goto no_conv;
  }

  /* Check for a sign. */
  negative= 0;
  if (wc == '-')
  {
    *error= -1;                                        /* Mark as negative number */
    negative= 1;
    if ((res= mb_wc(cs, &wc, s, end)) <= 0)
      goto no_conv;
    s+= res; /* wc is now expected to hold the first digit. */
    cutoff=  MAX_NEGATIVE_NUMBER / LFACTOR2;
    cutoff2= (MAX_NEGATIVE_NUMBER % LFACTOR2) / 100;
    cutoff3=  MAX_NEGATIVE_NUMBER % 100;
  }
  else
  {
    *error= 0;
    if (wc == '+')
    {
      if ((res= mb_wc(cs, &wc, s, end)) <= 0)
        goto no_conv;
      s+= res; /* wc is now expected to hold the first digit. */
    }
    cutoff=  ULONGLONG_MAX / LFACTOR2;
    cutoff2= ULONGLONG_MAX % LFACTOR2 / 100;
    cutoff3=  ULONGLONG_MAX % 100;
  }

  /*
    The code below assumes that 'wc' holds the first digit
    and 's' points to the next character after it.

    Scan pre-zeros if any.
  */
  if (wc == '0')
  {
    i= 0;
    for ( ; ; s+= res)
    {
      if (s == end)
        goto end_i;                                /* Return 0 */
      if ((res= mb_wc(cs, &wc, s, end)) <= 0)
        goto no_conv;
      if (wc != '0')
        break;
    }
    n_end= s + 2 * INIT_CNT;
  }
  else
  {
    /* Read first digit to check that it's a valid number */
    if ((i= (wc - '0')) > 9)
      goto no_conv;
    n_end= s + 2 * (INIT_CNT-1);
  }

  /* Handle first 9 digits and store them in i */
  if (n_end > end)
    n_end= end;
  for ( ; ; s+= res)
  {
    if ((res= mb_wc(cs, &wc, s, n_end)) <= 0)
      break;
    if (wc2digit_uchar(&c, wc))
      goto end_i;
    i= i*10+c;
  }
  if (s == end)
    goto end_i;

  /* Handle next 9 digits and store them in j */
  j= 0;
  start= s;                                /* Used to know how much to shift i */
  n_end= true_end= s + 2 * INIT_CNT;
  if (n_end > end)
    n_end= end;
  do
  {
    if ((res= mb_wc(cs, &wc, s, end)) <= 0)
      goto no_conv;
    if (wc2digit_uchar(&c, wc))
      goto end_i_and_j;
    s+= res;
    j= j * 10 + c;
  } while (s != n_end);
  if (s == end)
  {
    if (s != true_end)
      goto end_i_and_j;
    goto end3;
  }

  /* Handle the next 1 or 2 digits and store them in k */
  if ((res= mb_wc(cs, &wc, s, end)) <= 0)
    goto no_conv;
  if ((k= (wc - '0')) > 9)
    goto end3;
  s+= res;

  if (s == end)
    goto end4;
  if ((res= mb_wc(cs, &wc, s, end)) <= 0)
    goto no_conv;
  if (wc2digit_uchar(&c, wc))
    goto end4;
  s+= res;
  k= k*10+c;
  *endptr= (char*) s;

  /* number string should have ended here */
  if (s != end && mb_wc(cs, &wc, s, end) > 0 && ((uchar) (wc - '0')) <= 9)
    goto overflow;

  /* Check that we didn't get an overflow with the last digit */
  if (i > cutoff || (i == cutoff && ((j > cutoff2 || j == cutoff2) &&
                                     k > cutoff3)))
    goto overflow;
  li=i*LFACTOR2+ (ulonglong) j*100 + k;
  return (longlong) li;

overflow:                                        /* *endptr is set here */
  *error= MY_ERRNO_ERANGE;
  return negative ? LONGLONG_MIN : (longlong) ULONGLONG_MAX;

end_i:
  *endptr= (char*) s;
  return (negative ? ((longlong) -(long) i) : (longlong) i);

end_i_and_j:
  li= (ulonglong) i * lfactor[(size_t) (s-start) / 2] + j;
  *endptr= (char*) s;
  return (negative ? -((longlong) li) : (longlong) li);

end3:
  li=(ulonglong) i*LFACTOR+ (ulonglong) j;
  *endptr= (char*) s;
  return (negative ? -((longlong) li) : (longlong) li);

end4:
  li=(ulonglong) i*LFACTOR1+ (ulonglong) j * 10 + k;
  *endptr= (char*) s;
  if (negative)
  {
   if (li > MAX_NEGATIVE_NUMBER)
     goto overflow;
   return -((longlong) li);
  }
  return (longlong) li;

no_conv:
  /* There was no number to convert.  */
  *error= MY_ERRNO_EDOM;
  *endptr= (char *) nptr;
  return 0;
}


static size_t
my_scan_mb2(CHARSET_INFO *cs __attribute__((unused)),
            const char *str, const char *end, int sequence_type)
{
  const char *str0= str;
  my_wc_t wc;
  my_charset_conv_mb_wc mb_wc= cs->cset->mb_wc;
  int res;

  switch (sequence_type)
  {
  case MY_SEQ_SPACES:
    for (res= mb_wc(cs, &wc, (const uchar *) str, (const uchar *) end);
         res > 0 && wc == ' ';
         str+= res,
         res= mb_wc(cs, &wc, (const uchar *) str, (const uchar *) end))
    {
    }
    return (size_t) (str - str0);
  case MY_SEQ_NONSPACES:
    DBUG_ASSERT(0); /* Not implemented */
    /* pass through */
  default:
    return 0;
  }
}


static void
my_fill_mb2(CHARSET_INFO *cs, char *s, size_t slen, int fill)
{
  char buf[10], *last;
  size_t buflen, remainder;

  DBUG_ASSERT((slen % 2) == 0);

  buflen= cs->cset->wc_mb(cs, (my_wc_t) fill, (uchar*) buf,
                          (uchar*) buf + sizeof(buf));

  DBUG_ASSERT(buflen > 0);

  /*
    "last" in the last position where a sequence of "buflen" bytes can start.
  */
  for (last= s + slen - buflen; s <= last; s+= buflen)
  {
    /* Enough space for the character */
    memcpy(s, buf, buflen);
  }

  /* 
    If there are some more space which is not enough
    for the whole multibyte character, then add trailing zeros.
  */
  if ((remainder= last + buflen - s) > 0)
    bzero(s, (size_t) remainder);
}


static size_t
my_vsnprintf_mb2(char *dst, size_t n, const char* fmt, va_list ap)
{
  char *start=dst, *end= dst + n - 1;
  for (; *fmt ; fmt++)
  {
    if (fmt[0] != '%')
    {
      if (dst == end)                     /* End of buffer */
        break;
      
      *dst++='\0';
      *dst++= *fmt;          /* Copy ordinary char */
      continue;
    }
    
    fmt++;
    
    /* Skip if max size is used (to be compatible with printf) */
    while ( (*fmt >= '0' && *fmt <= '9') || *fmt == '.' || *fmt == '-')
      fmt++;
    
    if (*fmt == 'l')
      fmt++;
    
    if (*fmt == 's')                      /* String parameter */
    {
      char *par= va_arg(ap, char *);
      size_t plen;
      size_t left_len= (size_t)(end-dst);
      if (!par)
        par= (char*) "(null)";
      plen= strlen(par);
      if (left_len <= plen * 2)
        plen = left_len / 2 - 1;

      for ( ; plen ; plen--, dst+=2, par++)
      {
        dst[0]= '\0';
        dst[1]= par[0];
      }
      continue;
    }
    else if (*fmt == 'd' || *fmt == 'u')  /* Integer parameter */
    {
      int iarg;
      char nbuf[16];
      char *pbuf= nbuf;
      
      if ((size_t) (end - dst) < 32)
        break;
      iarg= va_arg(ap, int);
      if (*fmt == 'd')
        int10_to_str((long) iarg, nbuf, -10);
      else
        int10_to_str((long) (uint) iarg, nbuf,10);

      for (; pbuf[0]; pbuf++)
      {
        *dst++= '\0';
        *dst++= *pbuf;
      }
      continue;
    }
    
    /* We come here on '%%', unknown code or too long parameter */
    if (dst == end)
      break;
    *dst++= '\0';
    *dst++= '%';                            /* % used as % or unknown code */
  }
  
  DBUG_ASSERT(dst <= end);
  *dst='\0';                                /* End of errmessage */
  return (size_t) (dst - start);
}


static size_t
my_snprintf_mb2(CHARSET_INFO *cs __attribute__((unused)),
                char* to, size_t n, const char* fmt, ...)
{
  size_t ret;
  va_list args;
  va_start(args,fmt);
  ret= my_vsnprintf_mb2(to, n, fmt, args);
  va_end(args);
  return ret;
}


static size_t
my_lengthsp_mb2(CHARSET_INFO *cs __attribute__((unused)),
                const char *ptr, size_t length)
{
  const char *end= ptr + length;
  while (end > ptr + 1 && end[-1] == ' ' && end[-2] == '\0')
    end-= 2;
  return (size_t) (end - ptr);
}

#endif /* HAVE_CHARSET_mb2*/


/*
  Next part is actually HAVE_CHARSET_utf16-specific,
  but the JSON functions needed my_utf16_uni()
  so the #ifdef was moved lower.
*/
#include "ctype-utf16.h"

#define IS_MB2_CHAR(b0,b1)       (!MY_UTF16_SURROGATE_HEAD(b0))
#define IS_MB4_CHAR(b0,b1,b2,b3) (MY_UTF16_HIGH_HEAD(b0) && MY_UTF16_LOW_HEAD(b2))

static inline int my_weight_mb2_utf16mb2_general_ci(uchar b0, uchar b1)
{
  my_wc_t wc= MY_UTF16_WC2(b0, b1);
  MY_UNICASE_CHARACTER *page= my_unicase_default_pages[wc >> 8];
  return (int) (page ? page[wc & 0xFF].sort : wc);
}
#define MY_FUNCTION_NAME(x)      my_ ## x ## _utf16_general_ci
#define DEFINE_STRNXFRM_UNICODE
#define DEFINE_STRNXFRM_UNICODE_NOPAD
#define MY_MB_WC(cs, pwc, s, e)  my_mb_wc_utf16_quick(pwc, s, e)
#define OPTIMIZE_ASCII           0
#define UNICASE_MAXCHAR          MY_UNICASE_INFO_DEFAULT_MAXCHAR
#define UNICASE_PAGE0            my_unicase_default_page00
#define UNICASE_PAGES            my_unicase_default_pages
#define WEIGHT_ILSEQ(x)          (0xFF0000 + (uchar) (x))
#define WEIGHT_MB2(b0,b1)        my_weight_mb2_utf16mb2_general_ci(b0,b1)
#define WEIGHT_MB4(b0,b1,b2,b3)  MY_CS_REPLACEMENT_CHARACTER
#include "strcoll.ic"

#define MY_FUNCTION_NAME(x)      my_ ## x ## _utf16_bin
#define WEIGHT_ILSEQ(x)          (0xFF0000 + (uchar) (x))
#define WEIGHT_MB2(b0,b1)        ((int) MY_UTF16_WC2(b0, b1))
#define WEIGHT_MB4(b0,b1,b2,b3)  ((int) MY_UTF16_WC4(b0, b1, b2, b3))
#include "strcoll.ic"

#define DEFINE_STRNNCOLLSP_NOPAD
#define MY_FUNCTION_NAME(x)      my_ ## x ## _utf16_general_nopad_ci
#define WEIGHT_ILSEQ(x)          (0xFF0000 + (uchar) (x))
#define WEIGHT_MB2(b0,b1)        my_weight_mb2_utf16mb2_general_ci(b0,b1)
#define WEIGHT_MB4(b0,b1,b2,b3)  MY_CS_REPLACEMENT_CHARACTER
#include "strcoll.ic"

#define DEFINE_STRNNCOLLSP_NOPAD
#define MY_FUNCTION_NAME(x)      my_ ## x ## _utf16_nopad_bin
#define WEIGHT_ILSEQ(x)          (0xFF0000 + (uchar) (x))
#define WEIGHT_MB2(b0,b1)        ((int) MY_UTF16_WC2(b0, b1))
#define WEIGHT_MB4(b0,b1,b2,b3)  ((int) MY_UTF16_WC4(b0, b1, b2, b3))
#include "strcoll.ic"

#undef IS_MB2_CHAR
#undef IS_MB4_CHAR

/*
  These two functions are used in JSON library, so made exportable
  and unconditionally compiled into the library.
*/

/*static*/ int
my_utf16_uni(CHARSET_INFO *cs __attribute__((unused)),
             my_wc_t *pwc, const uchar *s, const uchar *e)
{
  return my_mb_wc_utf16_quick(pwc, s, e);
}


/*static*/ int
my_uni_utf16(CHARSET_INFO *cs __attribute__((unused)),
             my_wc_t wc, uchar *s, uchar *e)
{
  if (wc <= 0xFFFF)
  {
    if (s + 2 > e)
      return MY_CS_TOOSMALL2;
    if (MY_UTF16_SURROGATE(wc))
      return MY_CS_ILUNI;
    *s++= (uchar) (wc >> 8);
    *s= (uchar) (wc & 0xFF);
    return 2;
  }

  if (wc <= 0x10FFFF)
  {
    if (s + 4 > e)
      return MY_CS_TOOSMALL4;
    *s++= (uchar) ((wc-= 0x10000) >> 18) | 0xD8;
    *s++= (uchar) (wc >> 10) & 0xFF;
    *s++= (uchar) ((wc >> 8) & 3) | 0xDC;
    *s= (uchar) wc & 0xFF;
    return 4;
  }

  return MY_CS_ILUNI;
}


#ifdef HAVE_CHARSET_utf16


static inline void
my_tolower_utf16(MY_UNICASE_INFO *uni_plane, my_wc_t *wc)
{
  MY_UNICASE_CHARACTER *page;
  if ((*wc <= uni_plane->maxchar) && (page= uni_plane->page[*wc >> 8]))
    *wc= page[*wc & 0xFF].tolower;
}


static inline void
my_toupper_utf16(MY_UNICASE_INFO *uni_plane, my_wc_t *wc)
{
  MY_UNICASE_CHARACTER *page;
  if ((*wc <= uni_plane->maxchar) && (page= uni_plane->page[*wc >> 8]))
    *wc= page[*wc & 0xFF].toupper;
}


static inline void
my_tosort_utf16(MY_UNICASE_INFO *uni_plane, my_wc_t *wc)
{
  if (*wc <= uni_plane->maxchar)
  {
    MY_UNICASE_CHARACTER *page;
    if ((page= uni_plane->page[*wc >> 8]))
      *wc= page[*wc & 0xFF].sort;
  }
  else
  {
    *wc= MY_CS_REPLACEMENT_CHARACTER;
  }
}



static size_t
my_caseup_utf16(CHARSET_INFO *cs, const char *src, size_t srclen,
                char *dst, size_t dstlen)
{
  my_wc_t wc;
  my_charset_conv_mb_wc mb_wc= cs->cset->mb_wc;
  my_charset_conv_wc_mb wc_mb= cs->cset->wc_mb;
  int res;
  const char *srcend= src + srclen;
  char *dstend= dst + dstlen;
  MY_UNICASE_INFO *uni_plane= cs->caseinfo;
  DBUG_ASSERT(srclen <= dstlen);
  
  while ((src < srcend) &&
         (res= mb_wc(cs, &wc, (uchar *) src, (uchar *) srcend)) > 0)
  {
    my_toupper_utf16(uni_plane, &wc);
    if (res != wc_mb(cs, wc, (uchar *) dst, (uchar *) dstend))
      break;
    src+= res;
    dst+= res;
  }
  return srclen;
}


static void
my_hash_sort_utf16_nopad(CHARSET_INFO *cs,
                         const uchar *s, size_t slen,
                         ulong *nr1, ulong *nr2)
{
  my_wc_t wc;
  my_charset_conv_mb_wc mb_wc= cs->cset->mb_wc;
  int res;
  const uchar *e= s + slen;
  MY_UNICASE_INFO *uni_plane= cs->caseinfo;
  register ulong m1= *nr1, m2= *nr2;

  while ((s < e) && (res= mb_wc(cs, &wc, (uchar *) s, (uchar *) e)) > 0)
  {
    my_tosort_utf16(uni_plane, &wc);
    MY_HASH_ADD_16(m1, m2, wc);
    s+= res;
  }
  *nr1= m1;
  *nr2= m2;
}


static void
my_hash_sort_utf16(CHARSET_INFO *cs, const uchar *s, size_t slen,
                   ulong *nr1, ulong *nr2)
{
  size_t lengthsp= cs->cset->lengthsp(cs, (const char *) s, slen);
  my_hash_sort_utf16_nopad(cs, s, lengthsp, nr1, nr2);
}


static size_t
my_casedn_utf16(CHARSET_INFO *cs, const char *src, size_t srclen,
                char *dst, size_t dstlen)
{
  my_wc_t wc;
  my_charset_conv_mb_wc mb_wc= cs->cset->mb_wc;
  my_charset_conv_wc_mb wc_mb= cs->cset->wc_mb;
  int res;
  const char *srcend= src + srclen;
  char *dstend= dst + dstlen;
  MY_UNICASE_INFO *uni_plane= cs->caseinfo;
  DBUG_ASSERT(srclen <= dstlen);

  while ((src < srcend) &&
         (res= mb_wc(cs, &wc, (uchar *) src, (uchar *) srcend)) > 0)
  {
    my_tolower_utf16(uni_plane, &wc);
    if (res != wc_mb(cs, wc, (uchar *) dst, (uchar *) dstend))
      break;
    src+= res;
    dst+= res;
  }
  return srclen;
}


static int
my_charlen_utf16(CHARSET_INFO *cs, const uchar *str, const uchar *end)
{
  my_wc_t wc;
  return cs->cset->mb_wc(cs, &wc, str, end);
}


#define MY_FUNCTION_NAME(x)       my_ ## x ## _utf16
#define CHARLEN(cs,str,end)       my_charlen_utf16(cs,str,end)
#define DEFINE_WELL_FORMED_CHAR_LENGTH_USING_CHARLEN
#include "ctype-mb.ic"
#undef MY_FUNCTION_NAME
#undef CHARLEN
#undef DEFINE_WELL_FORMED_CHAR_LENGTH_USING_CHARLEN
/* Defines my_well_formed_char_length_utf16 */


static size_t
my_numchars_utf16(CHARSET_INFO *cs,
                  const char *b, const char *e)
{
  size_t nchars= 0;
  for ( ; ; nchars++)
  {
    size_t charlen= my_ismbchar(cs, b, e);
    if (!charlen)
      break;
    b+= charlen;
  }
  return nchars;
}


static size_t
my_charpos_utf16(CHARSET_INFO *cs,
                 const char *b, const char *e, size_t pos)
{
  const char *b0= b;
  uint charlen;
  
  for ( ; pos; b+= charlen, pos--)
  {
    if (!(charlen= my_ismbchar(cs, b, e)))
      return (e + 2 - b0); /* Error, return pos outside the string */
  }
  return (size_t) (pos ? (e + 2 - b0) : (b - b0));
}


static int
my_wildcmp_utf16_ci(CHARSET_INFO *cs,
                    const char *str,const char *str_end,
                    const char *wildstr,const char *wildend,
                    int escape, int w_one, int w_many)
{
  MY_UNICASE_INFO *uni_plane= cs->caseinfo;
  return my_wildcmp_unicode(cs, str, str_end, wildstr, wildend,
                            escape, w_one, w_many, uni_plane); 
}


static int
my_wildcmp_utf16_bin(CHARSET_INFO *cs,
                     const char *str,const char *str_end,
                     const char *wildstr,const char *wildend,
                     int escape, int w_one, int w_many)
{
  return my_wildcmp_unicode(cs, str, str_end, wildstr, wildend,
                            escape, w_one, w_many, NULL); 
}


static void
my_hash_sort_utf16_nopad_bin(CHARSET_INFO *cs  __attribute__((unused)),
                             const uchar *pos, size_t len,
                             ulong *nr1, ulong *nr2)
{
  const uchar *end= pos + len;
  register ulong m1= *nr1, m2= *nr2;

  for ( ; pos < end ; pos++)
  {
    MY_HASH_ADD(m1, m2, (uint)*pos);
  }
  *nr1= m1;
  *nr2= m2;
}


static void
my_hash_sort_utf16_bin(CHARSET_INFO *cs,
                       const uchar *pos, size_t len, ulong *nr1, ulong *nr2)
{
  size_t lengthsp= cs->cset->lengthsp(cs, (const char *) pos, len);
  my_hash_sort_utf16_nopad_bin(cs, pos, lengthsp, nr1, nr2);
}


static MY_COLLATION_HANDLER my_collation_utf16_general_ci_handler =
{
  NULL,                /* init */
  my_strnncoll_utf16_general_ci,
  my_strnncollsp_utf16_general_ci,
  my_strnxfrm_utf16_general_ci,
  my_strnxfrmlen_unicode,
  my_like_range_generic,
  my_wildcmp_utf16_ci,
  my_strcasecmp_mb2_or_mb4,
  my_instr_mb,
  my_hash_sort_utf16,
  my_propagate_simple
};


static MY_COLLATION_HANDLER my_collation_utf16_bin_handler =
{
  NULL,                /* init */
  my_strnncoll_utf16_bin,
  my_strnncollsp_utf16_bin,
  my_strnxfrm_unicode_full_bin,
  my_strnxfrmlen_unicode_full_bin,
  my_like_range_generic,
  my_wildcmp_utf16_bin,
  my_strcasecmp_mb2_or_mb4,
  my_instr_mb,
  my_hash_sort_utf16_bin,
  my_propagate_simple
};


static MY_COLLATION_HANDLER my_collation_utf16_general_nopad_ci_handler =
{
  NULL,                /* init */
  my_strnncoll_utf16_general_ci,
  my_strnncollsp_utf16_general_nopad_ci,
  my_strnxfrm_nopad_utf16_general_ci,
  my_strnxfrmlen_unicode,
  my_like_range_generic,
  my_wildcmp_utf16_ci,
  my_strcasecmp_mb2_or_mb4,
  my_instr_mb,
  my_hash_sort_utf16_nopad,
  my_propagate_simple
};


static MY_COLLATION_HANDLER my_collation_utf16_nopad_bin_handler =
{
  NULL,                /* init */
  my_strnncoll_utf16_bin,
  my_strnncollsp_utf16_nopad_bin,
  my_strnxfrm_unicode_full_nopad_bin,
  my_strnxfrmlen_unicode_full_bin,
  my_like_range_generic,
  my_wildcmp_utf16_bin,
  my_strcasecmp_mb2_or_mb4,
  my_instr_mb,
  my_hash_sort_utf16_nopad_bin,
  my_propagate_simple
};


MY_CHARSET_HANDLER my_charset_utf16_handler=
{
  NULL,                /* init         */
  my_numchars_utf16,
  my_charpos_utf16,
  my_lengthsp_mb2,
  my_numcells_mb,
  my_utf16_uni,        /* mb_wc        */
  my_uni_utf16,        /* wc_mb        */
  my_mb_ctype_mb,
  my_caseup_str_mb2_or_mb4,
  my_casedn_str_mb2_or_mb4,
  my_caseup_utf16,
  my_casedn_utf16,
  my_snprintf_mb2,
  my_l10tostr_mb2_or_mb4,
  my_ll10tostr_mb2_or_mb4,
  my_fill_mb2,
  my_strntol_mb2_or_mb4,
  my_strntoul_mb2_or_mb4,
  my_strntoll_mb2_or_mb4,
  my_strntoull_mb2_or_mb4,
  my_strntod_mb2_or_mb4,
  my_strtoll10_mb2,
  my_strntoull10rnd_mb2_or_mb4,
  my_scan_mb2,
  my_charlen_utf16,
  my_well_formed_char_length_utf16,
  my_copy_fix_mb2_or_mb4,
  my_uni_utf16,
};


struct charset_info_st my_charset_utf16_general_ci=
{
  54,0,0,              /* number       */
  MY_CS_COMPILED|MY_CS_PRIMARY|MY_CS_STRNXFRM|MY_CS_UNICODE|MY_CS_NONASCII,
  "utf16",             /* cs name    */
  "utf16_general_ci",  /* name         */
  "UTF-16 Unicode",    /* comment      */
  NULL,                /* tailoring    */
  NULL,                /* ctype        */
  NULL,                /* to_lower     */
  NULL,                /* to_upper     */
  NULL,                /* sort_order   */
  NULL,                /* uca          */
  NULL,                /* tab_to_uni   */
  NULL,                /* tab_from_uni */
  &my_unicase_default, /* caseinfo     */
  NULL,                /* state_map    */
  NULL,                /* ident_map    */
  1,                   /* strxfrm_multiply */
  1,                   /* caseup_multiply  */
  1,                   /* casedn_multiply  */
  2,                   /* mbminlen     */
  4,                   /* mbmaxlen     */
  0,                   /* min_sort_char */
  0xFFFF,              /* max_sort_char */
  ' ',                 /* pad char      */
  0,                   /* escape_with_backslash_is_dangerous */
  1,                   /* levels_for_order   */
  &my_charset_utf16_handler,
  &my_collation_utf16_general_ci_handler
};


struct charset_info_st my_charset_utf16_bin=
{
  55,0,0,              /* number       */
  MY_CS_COMPILED|MY_CS_BINSORT|MY_CS_STRNXFRM|MY_CS_UNICODE|MY_CS_NONASCII,
  "utf16",             /* cs name      */
  "utf16_bin",         /* name         */
  "UTF-16 Unicode",    /* comment      */
  NULL,                /* tailoring    */
  NULL,                /* ctype        */
  NULL,                /* to_lower     */
  NULL,                /* to_upper     */
  NULL,                /* sort_order   */
  NULL,                /* uca          */
  NULL,                /* tab_to_uni   */
  NULL,                /* tab_from_uni */
  &my_unicase_default, /* caseinfo     */
  NULL,                /* state_map    */
  NULL,                /* ident_map    */
  1,                   /* strxfrm_multiply */
  1,                   /* caseup_multiply  */
  1,                   /* casedn_multiply  */
  2,                   /* mbminlen     */
  4,                   /* mbmaxlen     */
  0,                   /* min_sort_char */
  0xFFFF,              /* max_sort_char */
  ' ',                 /* pad char      */
  0,                   /* escape_with_backslash_is_dangerous */
  1,                   /* levels_for_order   */
  &my_charset_utf16_handler,
  &my_collation_utf16_bin_handler
};


struct charset_info_st my_charset_utf16_general_nopad_ci=
{
  MY_NOPAD_ID(54),0,0, /* number           */
  MY_CS_COMPILED|MY_CS_STRNXFRM|MY_CS_UNICODE|MY_CS_NONASCII|MY_CS_NOPAD,
  "utf16",             /* cs name          */
  "utf16_general_nopad_ci", /* name        */
  "UTF-16 Unicode",    /* comment          */
  NULL,                /* tailoring        */
  NULL,                /* ctype            */
  NULL,                /* to_lower         */
  NULL,                /* to_upper         */
  NULL,                /* sort_order       */
  NULL,                /* uca              */
  NULL,                /* tab_to_uni       */
  NULL,                /* tab_from_uni     */
  &my_unicase_default, /* caseinfo         */
  NULL,                /* state_map        */
  NULL,                /* ident_map        */
  1,                   /* strxfrm_multiply */
  1,                   /* caseup_multiply  */
  1,                   /* casedn_multiply  */
  2,                   /* mbminlen         */
  4,                   /* mbmaxlen         */
  0,                   /* min_sort_char    */
  0xFFFF,              /* max_sort_char    */
  ' ',                 /* pad char         */
  0,                   /* escape_with_backslash_is_dangerous */
  1,                   /* levels_for_order */
  &my_charset_utf16_handler,
  &my_collation_utf16_general_nopad_ci_handler
};


struct charset_info_st my_charset_utf16_nopad_bin=
{
  MY_NOPAD_ID(55),0,0, /* number           */
  MY_CS_COMPILED|MY_CS_BINSORT|MY_CS_STRNXFRM|MY_CS_UNICODE|MY_CS_NONASCII|
  MY_CS_NOPAD,
  "utf16",             /* cs name          */
  "utf16_nopad_bin",   /* name             */
  "UTF-16 Unicode",    /* comment          */
  NULL,                /* tailoring        */
  NULL,                /* ctype            */
  NULL,                /* to_lower         */
  NULL,                /* to_upper         */
  NULL,                /* sort_order       */
  NULL,                /* uca              */
  NULL,                /* tab_to_uni       */
  NULL,                /* tab_from_uni     */
  &my_unicase_default, /* caseinfo         */
  NULL,                /* state_map        */
  NULL,                /* ident_map        */
  1,                   /* strxfrm_multiply */
  1,                   /* caseup_multiply  */
  1,                   /* casedn_multiply  */
  2,                   /* mbminlen         */
  4,                   /* mbmaxlen         */
  0,                   /* min_sort_char    */
  0xFFFF,              /* max_sort_char    */
  ' ',                 /* pad char         */
  0,                   /* escape_with_backslash_is_dangerous */
  1,                   /* levels_for_order */
  &my_charset_utf16_handler,
  &my_collation_utf16_nopad_bin_handler
};


#define IS_MB2_CHAR(b0,b1)       (!MY_UTF16_SURROGATE_HEAD(b1))
#define IS_MB4_CHAR(b0,b1,b2,b3) (MY_UTF16_HIGH_HEAD(b1) && MY_UTF16_LOW_HEAD(b3))

#define MY_FUNCTION_NAME(x)      my_ ## x ## _utf16le_general_ci
#define DEFINE_STRNXFRM_UNICODE
#define DEFINE_STRNXFRM_UNICODE_NOPAD
#define MY_MB_WC(cs, pwc, s, e)  (cs->cset->mb_wc(cs, pwc, s, e))
#define OPTIMIZE_ASCII           0
#define UNICASE_MAXCHAR          MY_UNICASE_INFO_DEFAULT_MAXCHAR
#define UNICASE_PAGE0            my_unicase_default_page00
#define UNICASE_PAGES            my_unicase_default_pages
#define WEIGHT_ILSEQ(x)          (0xFF0000 + (uchar) (x))
#define WEIGHT_MB2(b0,b1)        my_weight_mb2_utf16mb2_general_ci(b1,b0)
#define WEIGHT_MB4(b0,b1,b2,b3)  MY_CS_REPLACEMENT_CHARACTER
#include "strcoll.ic"

#define MY_FUNCTION_NAME(x)      my_ ## x ## _utf16le_bin
#define WEIGHT_ILSEQ(x)          (0xFF0000 + (uchar) (x))
#define WEIGHT_MB2(b0,b1)        ((int) MY_UTF16_WC2(b1, b0))
#define WEIGHT_MB4(b0,b1,b2,b3)  ((int) MY_UTF16_WC4(b1, b0, b3, b2))
#include "strcoll.ic"

#define DEFINE_STRNNCOLLSP_NOPAD
#define MY_FUNCTION_NAME(x)      my_ ## x ## _utf16le_general_nopad_ci
#define WEIGHT_ILSEQ(x)          (0xFF0000 + (uchar) (x))
#define WEIGHT_MB2(b0,b1)        my_weight_mb2_utf16mb2_general_ci(b1,b0)
#define WEIGHT_MB4(b0,b1,b2,b3)  MY_CS_REPLACEMENT_CHARACTER
#include "strcoll.ic"

#define DEFINE_STRNNCOLLSP_NOPAD
#define MY_FUNCTION_NAME(x)      my_ ## x ## _utf16le_nopad_bin
#define WEIGHT_ILSEQ(x)          (0xFF0000 + (uchar) (x))
#define WEIGHT_MB2(b0,b1)        ((int) MY_UTF16_WC2(b1, b0))
#define WEIGHT_MB4(b0,b1,b2,b3)  ((int) MY_UTF16_WC4(b1, b0, b3, b2))
#include "strcoll.ic"

#undef IS_MB2_CHAR
#undef IS_MB4_CHAR

static int
my_utf16le_uni(CHARSET_INFO *cs __attribute__((unused)),
               my_wc_t *pwc, const uchar *s, const uchar *e)
{
  my_wc_t lo;

  if (s + 2 > e)
    return MY_CS_TOOSMALL2;

  if ((*pwc= uint2korr(s)) < MY_UTF16_SURROGATE_HIGH_FIRST ||
      (*pwc > MY_UTF16_SURROGATE_LOW_LAST))
    return 2; /* [0000-D7FF,E000-FFFF] */

  if (*pwc >= MY_UTF16_SURROGATE_LOW_FIRST)
    return MY_CS_ILSEQ; /* [DC00-DFFF] Low surrogate part without high part */

  if (s + 4  > e)
    return MY_CS_TOOSMALL4;

  s+= 2;

  if ((lo= uint2korr(s)) < MY_UTF16_SURROGATE_LOW_FIRST ||
      lo > MY_UTF16_SURROGATE_LOW_LAST)
    return MY_CS_ILSEQ; /* Expected low surrogate part, got something else */

  *pwc= 0x10000 + (((*pwc & 0x3FF) << 10) | (lo & 0x3FF));
  return 4;
}


static int
my_uni_utf16le(CHARSET_INFO *cs __attribute__((unused)),
               my_wc_t wc, uchar *s, uchar *e)
{
  uint32 first, second, total;
  if (wc < MY_UTF16_SURROGATE_HIGH_FIRST ||
      (wc > MY_UTF16_SURROGATE_LOW_LAST &&
       wc <= 0xFFFF))
  {
    if (s + 2 > e)
      return MY_CS_TOOSMALL2;
    int2store(s, wc);
    return 2; /* [0000-D7FF,E000-FFFF] */
  }

  if (wc < 0xFFFF || wc > 0x10FFFF)
    return MY_CS_ILUNI; /* [D800-DFFF,10FFFF+] */

  if (s + 4 > e)
    return MY_CS_TOOSMALL4;

  wc-= 0x10000;
  first=  (0xD800 | ((wc >> 10) & 0x3FF));
  second= (0xDC00 | (wc & 0x3FF));
  total=  first | (second << 16);
  int4store(s, total);
  return 4; /* [010000-10FFFF] */
}


static size_t
my_lengthsp_utf16le(CHARSET_INFO *cs __attribute__((unused)),
                    const char *ptr, size_t length)
{
  const char *end= ptr + length;
  while (end > ptr + 1 && uint2korr(end - 2) == ' ')
    end-= 2;
  return (size_t) (end - ptr);
}


static MY_COLLATION_HANDLER my_collation_utf16le_general_ci_handler =
{
  NULL,                /* init */
  my_strnncoll_utf16le_general_ci,
  my_strnncollsp_utf16le_general_ci,
  my_strnxfrm_utf16le_general_ci,
  my_strnxfrmlen_unicode,
  my_like_range_generic,
  my_wildcmp_utf16_ci,
  my_strcasecmp_mb2_or_mb4,
  my_instr_mb,
  my_hash_sort_utf16,
  my_propagate_simple
};


static MY_COLLATION_HANDLER my_collation_utf16le_bin_handler =
{
  NULL,                /* init */
  my_strnncoll_utf16le_bin,
  my_strnncollsp_utf16le_bin,
  my_strnxfrm_unicode_full_bin,
  my_strnxfrmlen_unicode_full_bin,
  my_like_range_generic,
  my_wildcmp_utf16_bin,
  my_strcasecmp_mb2_or_mb4,
  my_instr_mb,
  my_hash_sort_utf16_bin,
  my_propagate_simple
};


static MY_COLLATION_HANDLER my_collation_utf16le_general_nopad_ci_handler =
{
  NULL,                /* init */
  my_strnncoll_utf16le_general_ci,
  my_strnncollsp_utf16le_general_nopad_ci,
  my_strnxfrm_nopad_utf16le_general_ci,
  my_strnxfrmlen_unicode,
  my_like_range_generic,
  my_wildcmp_utf16_ci,
  my_strcasecmp_mb2_or_mb4,
  my_instr_mb,
  my_hash_sort_utf16_nopad,
  my_propagate_simple
};


static MY_COLLATION_HANDLER my_collation_utf16le_nopad_bin_handler =
{
  NULL,                /* init */
  my_strnncoll_utf16le_bin,
  my_strnncollsp_utf16le_nopad_bin,
  my_strnxfrm_unicode_full_nopad_bin,
  my_strnxfrmlen_unicode_full_bin,
  my_like_range_generic,
  my_wildcmp_utf16_bin,
  my_strcasecmp_mb2_or_mb4,
  my_instr_mb,
  my_hash_sort_utf16_nopad_bin,
  my_propagate_simple
};


static MY_CHARSET_HANDLER my_charset_utf16le_handler=
{
  NULL,                /* init         */
  my_numchars_utf16,
  my_charpos_utf16,
  my_lengthsp_utf16le,
  my_numcells_mb,
  my_utf16le_uni,      /* mb_wc        */
  my_uni_utf16le,      /* wc_mb        */
  my_mb_ctype_mb,
  my_caseup_str_mb2_or_mb4,
  my_casedn_str_mb2_or_mb4,
  my_caseup_utf16,
  my_casedn_utf16,
  my_snprintf_mb2,
  my_l10tostr_mb2_or_mb4,
  my_ll10tostr_mb2_or_mb4,
  my_fill_mb2,
  my_strntol_mb2_or_mb4,
  my_strntoul_mb2_or_mb4,
  my_strntoll_mb2_or_mb4,
  my_strntoull_mb2_or_mb4,
  my_strntod_mb2_or_mb4,
  my_strtoll10_mb2,
  my_strntoull10rnd_mb2_or_mb4,
  my_scan_mb2,
  my_charlen_utf16,
  my_well_formed_char_length_utf16,
  my_copy_fix_mb2_or_mb4,
  my_uni_utf16le,
};


struct charset_info_st my_charset_utf16le_general_ci=
{
  56,0,0,              /* number       */
  MY_CS_COMPILED|MY_CS_PRIMARY|MY_CS_STRNXFRM|MY_CS_UNICODE|MY_CS_NONASCII,
  "utf16le",           /* cs name    */
  "utf16le_general_ci",/* name         */
  "UTF-16LE Unicode",  /* comment      */
  NULL,                /* tailoring    */
  NULL,                /* ctype        */
  NULL,                /* to_lower     */
  NULL,                /* to_upper     */
  NULL,                /* sort_order   */
  NULL,                /* uca          */
  NULL,                /* tab_to_uni   */
  NULL,                /* tab_from_uni */
  &my_unicase_default, /* caseinfo     */
  NULL,                /* state_map    */
  NULL,                /* ident_map    */
  1,                   /* strxfrm_multiply */
  1,                   /* caseup_multiply  */
  1,                   /* casedn_multiply  */
  2,                   /* mbminlen     */
  4,                   /* mbmaxlen     */
  0,                   /* min_sort_char */
  0xFFFF,              /* max_sort_char */
  ' ',                 /* pad char      */
  0,                   /* escape_with_backslash_is_dangerous */
  1,                   /* levels_for_order   */
  &my_charset_utf16le_handler,
  &my_collation_utf16le_general_ci_handler
};


struct charset_info_st my_charset_utf16le_bin=
{
  62,0,0,              /* number       */
  MY_CS_COMPILED|MY_CS_BINSORT|MY_CS_STRNXFRM|MY_CS_UNICODE|MY_CS_NONASCII,
  "utf16le",           /* cs name      */
  "utf16le_bin",       /* name         */
  "UTF-16LE Unicode",  /* comment      */
  NULL,                /* tailoring    */
  NULL,                /* ctype        */
  NULL,                /* to_lower     */
  NULL,                /* to_upper     */
  NULL,                /* sort_order   */
  NULL,                /* uca          */
  NULL,                /* tab_to_uni   */
  NULL,                /* tab_from_uni */
  &my_unicase_default, /* caseinfo     */
  NULL,                /* state_map    */
  NULL,                /* ident_map    */
  1,                   /* strxfrm_multiply */
  1,                   /* caseup_multiply  */
  1,                   /* casedn_multiply  */
  2,                   /* mbminlen     */
  4,                   /* mbmaxlen     */
  0,                   /* min_sort_char */
  0xFFFF,              /* max_sort_char */
  ' ',                 /* pad char      */
  0,                   /* escape_with_backslash_is_dangerous */
  1,                   /* levels_for_order   */
  &my_charset_utf16le_handler,
  &my_collation_utf16le_bin_handler
};


struct charset_info_st my_charset_utf16le_general_nopad_ci=
{
  MY_NOPAD_ID(56),0,0, /* number           */
  MY_CS_COMPILED|MY_CS_STRNXFRM|MY_CS_UNICODE|MY_CS_NONASCII|MY_CS_NOPAD,
  "utf16le",           /* cs name          */
  "utf16le_general_nopad_ci",/* name       */
  "UTF-16LE Unicode",  /* comment          */
  NULL,                /* tailoring        */
  NULL,                /* ctype            */
  NULL,                /* to_lower         */
  NULL,                /* to_upper         */
  NULL,                /* sort_order       */
  NULL,                /* uca              */
  NULL,                /* tab_to_uni       */
  NULL,                /* tab_from_uni     */
  &my_unicase_default, /* caseinfo         */
  NULL,                /* state_map        */
  NULL,                /* ident_map        */
  1,                   /* strxfrm_multiply */
  1,                   /* caseup_multiply  */
  1,                   /* casedn_multiply  */
  2,                   /* mbminlen         */
  4,                   /* mbmaxlen         */
  0,                   /* min_sort_char    */
  0xFFFF,              /* max_sort_char    */
  ' ',                 /* pad char         */
  0,                   /* escape_with_backslash_is_dangerous */
  1,                   /* levels_for_order */
  &my_charset_utf16le_handler,
  &my_collation_utf16le_general_nopad_ci_handler
};


struct charset_info_st my_charset_utf16le_nopad_bin=
{
  MY_NOPAD_ID(62),0,0, /* number           */
  MY_CS_COMPILED|MY_CS_BINSORT|MY_CS_STRNXFRM|MY_CS_UNICODE|MY_CS_NONASCII|
  MY_CS_NOPAD,
  "utf16le",           /* cs name          */
  "utf16le_nopad_bin", /* name             */
  "UTF-16LE Unicode",  /* comment          */
  NULL,                /* tailoring        */
  NULL,                /* ctype            */
  NULL,                /* to_lower         */
  NULL,                /* to_upper         */
  NULL,                /* sort_order       */
  NULL,                /* uca              */
  NULL,                /* tab_to_uni       */
  NULL,                /* tab_from_uni     */
  &my_unicase_default, /* caseinfo         */
  NULL,                /* state_map        */
  NULL,                /* ident_map        */
  1,                   /* strxfrm_multiply */
  1,                   /* caseup_multiply  */
  1,                   /* casedn_multiply  */
  2,                   /* mbminlen         */
  4,                   /* mbmaxlen         */
  0,                   /* min_sort_char    */
  0xFFFF,              /* max_sort_char    */
  ' ',                 /* pad char         */
  0,                   /* escape_with_backslash_is_dangerous */
  1,                   /* levels_for_order */
  &my_charset_utf16le_handler,
  &my_collation_utf16le_nopad_bin_handler
};


#endif /* HAVE_CHARSET_utf16 */


#ifdef HAVE_CHARSET_utf32

#include "ctype-utf32.h"

/*
  Check is b0 and b1 start a valid UTF32 four-byte sequence.
  Don't accept characters greater than U+10FFFF.
*/
#define IS_UTF32_MBHEAD4(b0,b1) (!(b0) && ((uchar) (b1) <= 0x10))

#define IS_MB4_CHAR(b0,b1,b2,b3)   (IS_UTF32_MBHEAD4(b0,b1))


static inline int my_weight_utf32_general_ci(uchar b0, uchar b1,
                                             uchar b2, uchar b3)
{
  my_wc_t wc= MY_UTF32_WC4(b0, b1, b2, b3);
  if (wc <= 0xFFFF)
  {
    MY_UNICASE_CHARACTER *page= my_unicase_default_pages[wc >> 8];
    return (int) (page ? page[wc & 0xFF].sort : wc);
  }
  return MY_CS_REPLACEMENT_CHARACTER;
}
#define MY_FUNCTION_NAME(x)      my_ ## x ## _utf32_general_ci
#define DEFINE_STRNXFRM_UNICODE
#define DEFINE_STRNXFRM_UNICODE_NOPAD
#define MY_MB_WC(cs, pwc, s, e)  my_mb_wc_utf32_quick(pwc, s, e)
#define OPTIMIZE_ASCII           0
#define UNICASE_MAXCHAR          MY_UNICASE_INFO_DEFAULT_MAXCHAR
#define UNICASE_PAGE0            my_unicase_default_page00
#define UNICASE_PAGES            my_unicase_default_pages
#define WEIGHT_ILSEQ(x)          (0xFF0000 + (uchar) (x))
#define WEIGHT_MB4(b0,b1,b2,b3)  my_weight_utf32_general_ci(b0, b1, b2, b3)
#include "strcoll.ic"

#define MY_FUNCTION_NAME(x)      my_ ## x ## _utf32_bin
#define WEIGHT_ILSEQ(x)          (0xFF0000 + (uchar) (x))
#define WEIGHT_MB4(b0,b1,b2,b3)  ((int) MY_UTF32_WC4(b0, b1, b2, b3))
#include "strcoll.ic"

#define DEFINE_STRNNCOLLSP_NOPAD
#define MY_FUNCTION_NAME(x)      my_ ## x ## _utf32_general_nopad_ci
#define WEIGHT_ILSEQ(x)          (0xFF0000 + (uchar) (x))
#define WEIGHT_MB4(b0,b1,b2,b3)  my_weight_utf32_general_ci(b0, b1, b2, b3)
#include "strcoll.ic"

#define DEFINE_STRNNCOLLSP_NOPAD
#define MY_FUNCTION_NAME(x)      my_ ## x ## _utf32_nopad_bin
#define WEIGHT_ILSEQ(x)          (0xFF0000 + (uchar) (x))
#define WEIGHT_MB4(b0,b1,b2,b3)  ((int) MY_UTF32_WC4(b0, b1, b2, b3))
#include "strcoll.ic"

#undef IS_MB2_CHAR
#undef IS_MB4_CHAR


static int
my_utf32_uni(CHARSET_INFO *cs __attribute__((unused)),
             my_wc_t *pwc, const uchar *s, const uchar *e)
{
  return my_mb_wc_utf32_quick(pwc, s, e);
}


static int
my_uni_utf32(CHARSET_INFO *cs __attribute__((unused)),
             my_wc_t wc, uchar *s, uchar *e)
{
  if (s + 4 > e) 
    return MY_CS_TOOSMALL4;

  if (wc > 0x10FFFF)  
    return MY_CS_ILUNI;

  s[0]= (uchar) (wc >> 24);
  s[1]= (uchar) (wc >> 16) & 0xFF;
  s[2]= (uchar) (wc >> 8)  & 0xFF;
  s[3]= (uchar) wc & 0xFF;
  return 4;
}


static inline void
my_tolower_utf32(MY_UNICASE_INFO *uni_plane, my_wc_t *wc)
{
  MY_UNICASE_CHARACTER *page;
  if ((*wc <= uni_plane->maxchar) && (page= uni_plane->page[*wc >> 8]))
    *wc= page[*wc & 0xFF].tolower;
}


static inline void
my_toupper_utf32(MY_UNICASE_INFO *uni_plane, my_wc_t *wc)
{
  MY_UNICASE_CHARACTER *page;
  if ((*wc <= uni_plane->maxchar) && (page= uni_plane->page[*wc >> 8]))
    *wc= page[*wc & 0xFF].toupper;
}


static inline void
my_tosort_utf32(MY_UNICASE_INFO *uni_plane, my_wc_t *wc)
{
  if (*wc <= uni_plane->maxchar)
  {
    MY_UNICASE_CHARACTER *page;
    if ((page= uni_plane->page[*wc >> 8]))
      *wc= page[*wc & 0xFF].sort;
  }
  else
  {
    *wc= MY_CS_REPLACEMENT_CHARACTER;
  }
}


static size_t
my_lengthsp_utf32(CHARSET_INFO *cs __attribute__((unused)),
                  const char *ptr, size_t length)
{
  const char *end= ptr + length;
  DBUG_ASSERT((length % 4) == 0);
  while (end > ptr + 3 && end[-1] == ' ' && !end[-2] && !end[-3] && !end[-4])
    end-= 4;
  return (size_t) (end - ptr);
}


static size_t
my_caseup_utf32(CHARSET_INFO *cs, const char *src, size_t srclen,
                char *dst, size_t dstlen)
{
  my_wc_t wc;
  int res;
  const char *srcend= src + srclen;
  char *dstend= dst + dstlen;
  MY_UNICASE_INFO *uni_plane= cs->caseinfo;
  DBUG_ASSERT(srclen <= dstlen);
  
  while ((src < srcend) &&
         (res= my_utf32_uni(cs, &wc, (uchar *)src, (uchar*) srcend)) > 0)
  {
    my_toupper_utf32(uni_plane, &wc);
    if (res != my_uni_utf32(cs, wc, (uchar*) dst, (uchar*) dstend))
      break;
    src+= res;
    dst+= res;
  }
  return srclen;
}


static void
my_hash_sort_utf32_nopad(CHARSET_INFO *cs, const uchar *s, size_t slen,
                         ulong *nr1, ulong *nr2)
{
  my_wc_t wc;
  int res;
  const uchar *e= s + slen;
  MY_UNICASE_INFO *uni_plane= cs->caseinfo;
  register ulong m1= *nr1, m2= *nr2;

  while ((res= my_utf32_uni(cs, &wc, (uchar*) s, (uchar*) e)) > 0)
  {
    my_tosort_utf32(uni_plane, &wc);
    MY_HASH_ADD(m1, m2, (uint) (wc >> 24));
    MY_HASH_ADD(m1, m2, (uint) (wc >> 16) & 0xFF);
    MY_HASH_ADD(m1, m2, (uint) (wc >> 8)  & 0xFF);
    MY_HASH_ADD(m1, m2, (uint) (wc & 0xFF));
    s+= res;
  }
  *nr1= m1;
  *nr2= m2;
}


static void
my_hash_sort_utf32(CHARSET_INFO *cs, const uchar *s, size_t slen,
                   ulong *nr1, ulong *nr2)
{
  size_t lengthsp= my_lengthsp_utf32(cs, (const char *) s, slen);
  my_hash_sort_utf32_nopad(cs, s, lengthsp, nr1, nr2);
}


static size_t
my_casedn_utf32(CHARSET_INFO *cs, const char *src, size_t srclen,
                char *dst, size_t dstlen)
{
  my_wc_t wc;
  int res;
  const char *srcend= src + srclen;
  char *dstend= dst + dstlen;
  MY_UNICASE_INFO *uni_plane= cs->caseinfo;
  DBUG_ASSERT(srclen <= dstlen);

  while ((res= my_utf32_uni(cs, &wc, (uchar*) src, (uchar*) srcend)) > 0)
  {
    my_tolower_utf32(uni_plane,&wc);
    if (res != my_uni_utf32(cs, wc, (uchar*) dst, (uchar*) dstend))
      break;
    src+= res;
    dst+= res;
  }
  return srclen;
}


static int
my_charlen_utf32(CHARSET_INFO *cs __attribute__((unused)),
                 const uchar *b, const uchar *e)
{
  return b + 4 > e ? MY_CS_TOOSMALL4 :
         IS_UTF32_MBHEAD4(b[0], b[1]) ? 4 : MY_CS_ILSEQ;
}


#define MY_FUNCTION_NAME(x)       my_ ## x ## _utf32
#define CHARLEN(cs,str,end)       my_charlen_utf32(cs,str,end)
#define DEFINE_WELL_FORMED_CHAR_LENGTH_USING_CHARLEN
#include "ctype-mb.ic"
#undef MY_FUNCTION_NAME
#undef CHARLEN
#undef DEFINE_WELL_FORMED_CHAR_LENGTH_USING_CHARLEN
/* Defines my_well_formed_char_length_utf32 */


static size_t
my_vsnprintf_utf32(char *dst, size_t n, const char* fmt, va_list ap)
{
  char *start= dst, *end= dst + n;
  DBUG_ASSERT((n % 4) == 0);
  for (; *fmt ; fmt++)
  {
    if (fmt[0] != '%')
    {
      if (dst >= end)                        /* End of buffer */
        break;
      
      *dst++= '\0';
      *dst++= '\0';
      *dst++= '\0';
      *dst++= *fmt;        /* Copy ordinary char */
      continue;
    }
    
    fmt++;
    
    /* Skip if max size is used (to be compatible with printf) */
    while ( (*fmt>='0' && *fmt<='9') || *fmt == '.' || *fmt == '-')
      fmt++;
    
    if (*fmt == 'l')
      fmt++;
    
    if (*fmt == 's')                                /* String parameter */
    {
      reg2 char *par= va_arg(ap, char *);
      size_t plen;
      size_t left_len= (size_t)(end - dst);
      if (!par) par= (char*)"(null)";
      plen= strlen(par);
      if (left_len <= plen*4)
        plen= left_len / 4 - 1;

      for ( ; plen ; plen--, dst+= 4, par++)
      {
        dst[0]= '\0';
        dst[1]= '\0';
        dst[2]= '\0';
        dst[3]= par[0];
      }
      continue;
    }
    else if (*fmt == 'd' || *fmt == 'u')        /* Integer parameter */
    {
      register int iarg;
      char nbuf[16];
      char *pbuf= nbuf;
      
      if ((size_t) (end - dst) < 64)
        break;
      iarg= va_arg(ap, int);
      if (*fmt == 'd')
        int10_to_str((long) iarg, nbuf, -10);
      else
        int10_to_str((long) (uint) iarg,nbuf,10);

      for (; pbuf[0]; pbuf++)
      {
        *dst++= '\0';
        *dst++= '\0';
        *dst++= '\0';
        *dst++= *pbuf;
      }
      continue;
    }
    
    /* We come here on '%%', unknown code or too long parameter */
    if (dst == end)
      break;
    *dst++= '\0';
    *dst++= '\0';
    *dst++= '\0';
    *dst++= '%';    /* % used as % or unknown code */
  }
  
  DBUG_ASSERT(dst < end);
  *dst++= '\0';
  *dst++= '\0';
  *dst++= '\0';
  *dst++= '\0';     /* End of errmessage */
  return (size_t) (dst - start - 4);
}


static size_t
my_snprintf_utf32(CHARSET_INFO *cs __attribute__((unused)),
                  char* to, size_t n, const char* fmt, ...)
{
  size_t ret;
  va_list args;
  va_start(args,fmt);
  ret= my_vsnprintf_utf32(to, n, fmt, args);
  va_end(args);
  return ret;
}


static longlong
my_strtoll10_utf32(CHARSET_INFO *cs __attribute__((unused)),
                   const char *nptr, char **endptr, int *error)
{
  const char *s, *end, *start, *n_end, *true_end;
  uchar c;
  unsigned long i, j, k;
  ulonglong li;
  int negative;
  ulong cutoff, cutoff2, cutoff3;

  s= nptr;
  /* If fixed length string */
  if (endptr)
  {
    /* Make sure string length is even */
    end= s + ((*endptr - s) / 4) * 4;
    while (s < end && !s[0] && !s[1] && !s[2] &&
           (s[3] == ' ' || s[3] == '\t'))
      s+= 4;
    if (s == end)
      goto no_conv;
  }
  else
  {
     /* We don't support null terminated strings in UCS2 */
     goto no_conv;
  }

  /* Check for a sign. */
  negative= 0;
  if (!s[0] && !s[1] && !s[2] && s[3] == '-')
  {
    *error= -1;                                        /* Mark as negative number */
    negative= 1;
    s+= 4;
    if (s == end)
      goto no_conv;
    cutoff=  MAX_NEGATIVE_NUMBER / LFACTOR2;
    cutoff2= (MAX_NEGATIVE_NUMBER % LFACTOR2) / 100;
    cutoff3=  MAX_NEGATIVE_NUMBER % 100;
  }
  else
  {
    *error= 0;
    if (!s[0] && !s[1] && !s[2] && s[3] == '+')
    {
      s+= 4;
      if (s == end)
        goto no_conv;
    }
    cutoff=  ULONGLONG_MAX / LFACTOR2;
    cutoff2= ULONGLONG_MAX % LFACTOR2 / 100;
    cutoff3=  ULONGLONG_MAX % 100;
  }

  /* Handle case where we have a lot of pre-zero */
  if (!s[0] && !s[1] && !s[2] && s[3] == '0')
  {
    i= 0;
    do
    {
      s+= 4;
      if (s == end)
        goto end_i;                                /* Return 0 */
    }
    while (!s[0] && !s[1] && !s[2] && s[3] == '0');
    n_end= s + 4 * INIT_CNT;
  }
  else
  {
    /* Read first digit to check that it's a valid number */
    if (s[0] || s[1] || s[2] || (c= (s[3]-'0')) > 9)
      goto no_conv;
    i= c;
    s+= 4;
    n_end= s + 4 * (INIT_CNT-1);
  }

  /* Handle first 9 digits and store them in i */
  if (n_end > end)
    n_end= end;
  for (; s != n_end ; s+= 4)
  {
    if (s[0] || s[1] || s[2] || (c= (s[3] - '0')) > 9)
      goto end_i;
    i= i * 10 + c;
  }
  if (s == end)
    goto end_i;

  /* Handle next 9 digits and store them in j */
  j= 0;
  start= s;                                /* Used to know how much to shift i */
  n_end= true_end= s + 4 * INIT_CNT;
  if (n_end > end)
    n_end= end;
  do
  {
    if (s[0] || s[1] || s[2] || (c= (s[3] - '0')) > 9)
      goto end_i_and_j;
    j= j * 10 + c;
    s+= 4;
  } while (s != n_end);
  if (s == end)
  {
    if (s != true_end)
      goto end_i_and_j;
    goto end3;
  }
  if (s[0] || s[1] || s[2] || (c= (s[3] - '0')) > 9)
    goto end3;

  /* Handle the next 1 or 2 digits and store them in k */
  k=c;
  s+= 4;
  if (s == end || s[0] || s[1] || s[2] || (c= (s[3]-'0')) > 9)
    goto end4;
  k= k * 10 + c;
  s+= 4;
  *endptr= (char*) s;

  /* number string should have ended here */
  if (s != end && !s[0] && !s[1] && !s[2] && (c= (s[3] - '0')) <= 9)
    goto overflow;

  /* Check that we didn't get an overflow with the last digit */
  if (i > cutoff || (i == cutoff && ((j > cutoff2 || j == cutoff2) &&
                                     k > cutoff3)))
    goto overflow;
  li= i * LFACTOR2+ (ulonglong) j * 100 + k;
  return (longlong) li;

overflow:                                        /* *endptr is set here */
  *error= MY_ERRNO_ERANGE;
  return negative ? LONGLONG_MIN : (longlong) ULONGLONG_MAX;

end_i:
  *endptr= (char*) s;
  return (negative ? ((longlong) -(long) i) : (longlong) i);

end_i_and_j:
  li= (ulonglong) i * lfactor[(size_t) (s-start) / 4] + j;
  *endptr= (char*) s;
  return (negative ? -((longlong) li) : (longlong) li);

end3:
  li= (ulonglong) i*LFACTOR+ (ulonglong) j;
  *endptr= (char*) s;
  return (negative ? -((longlong) li) : (longlong) li);

end4:
  li= (ulonglong) i*LFACTOR1+ (ulonglong) j * 10 + k;
  *endptr= (char*) s;
  if (negative)
  {
   if (li > MAX_NEGATIVE_NUMBER)
     goto overflow;
   return -((longlong) li);
  }
  return (longlong) li;

no_conv:
  /* There was no number to convert.  */
  *error= MY_ERRNO_EDOM;
  *endptr= (char *) nptr;
  return 0;
}


static size_t
my_numchars_utf32(CHARSET_INFO *cs __attribute__((unused)),
                  const char *b, const char *e)
{
  return (size_t) (e - b) / 4;
}


static size_t
my_charpos_utf32(CHARSET_INFO *cs __attribute__((unused)),
                 const char *b, const char *e, size_t pos)
{
  size_t string_length= (size_t) (e - b);
  return pos * 4 > string_length ? string_length + 4 : pos * 4;
}


static
void my_fill_utf32(CHARSET_INFO *cs,
                   char *s, size_t slen, int fill)
{
  char buf[10];
#ifdef DBUG_ASSERT_EXISTS
  uint buflen;
#endif
  char *e= s + slen;
  
  DBUG_ASSERT((slen % 4) == 0);

#ifdef DBUG_ASSERT_EXISTS
  buflen=
#endif
    cs->cset->wc_mb(cs, (my_wc_t) fill, (uchar*) buf,
                    (uchar*) buf + sizeof(buf));
  DBUG_ASSERT(buflen == 4);
  while (s < e)
  {
    memcpy(s, buf, 4);
    s+= 4;
  }
}


static int
my_wildcmp_utf32_ci(CHARSET_INFO *cs,
                    const char *str, const char *str_end,
                    const char *wildstr, const char *wildend,
                    int escape, int w_one, int w_many)
{
  MY_UNICASE_INFO *uni_plane= cs->caseinfo;
  return my_wildcmp_unicode(cs, str, str_end, wildstr, wildend,
                            escape, w_one, w_many, uni_plane); 
}


static int
my_wildcmp_utf32_bin(CHARSET_INFO *cs,
                     const char *str,const char *str_end,
                     const char *wildstr,const char *wildend,
                     int escape, int w_one, int w_many)
{
  return my_wildcmp_unicode(cs, str, str_end, wildstr, wildend,
                            escape, w_one, w_many, NULL); 
}


static size_t
my_scan_utf32(CHARSET_INFO *cs,
              const char *str, const char *end, int sequence_type)
{
  const char *str0= str;
  
  switch (sequence_type)
  {
  case MY_SEQ_SPACES:
    for ( ; str < end; )
    {
      my_wc_t wc;
      int res= my_utf32_uni(cs, &wc, (uchar*) str, (uchar*) end);
      if (res < 0 || wc != ' ')
        break;
      str+= res;
    }
    return (size_t) (str - str0);
  case MY_SEQ_NONSPACES:
    DBUG_ASSERT(0); /* Not implemented */
    /* pass through */
  default:
    return 0;
  }
}


static MY_COLLATION_HANDLER my_collation_utf32_general_ci_handler =
{
  NULL, /* init */
  my_strnncoll_utf32_general_ci,
  my_strnncollsp_utf32_general_ci,
  my_strnxfrm_utf32_general_ci,
  my_strnxfrmlen_unicode,
  my_like_range_generic,
  my_wildcmp_utf32_ci,
  my_strcasecmp_mb2_or_mb4,
  my_instr_mb,
  my_hash_sort_utf32,
  my_propagate_simple
};


static MY_COLLATION_HANDLER my_collation_utf32_bin_handler =
{
  NULL, /* init */
  my_strnncoll_utf32_bin,
  my_strnncollsp_utf32_bin,
  my_strnxfrm_unicode_full_bin,
  my_strnxfrmlen_unicode_full_bin,
  my_like_range_generic,
  my_wildcmp_utf32_bin,
  my_strcasecmp_mb2_or_mb4,
  my_instr_mb,
  my_hash_sort_utf32,
  my_propagate_simple
};


static MY_COLLATION_HANDLER my_collation_utf32_general_nopad_ci_handler =
{
  NULL, /* init */
  my_strnncoll_utf32_general_ci,
  my_strnncollsp_utf32_general_nopad_ci,
  my_strnxfrm_nopad_utf32_general_ci,
  my_strnxfrmlen_unicode,
  my_like_range_generic,
  my_wildcmp_utf32_ci,
  my_strcasecmp_mb2_or_mb4,
  my_instr_mb,
  my_hash_sort_utf32_nopad,
  my_propagate_simple
};


static MY_COLLATION_HANDLER my_collation_utf32_nopad_bin_handler =
{
  NULL, /* init */
  my_strnncoll_utf32_bin,
  my_strnncollsp_utf32_nopad_bin,
  my_strnxfrm_unicode_full_nopad_bin,
  my_strnxfrmlen_unicode_full_bin,
  my_like_range_generic,
  my_wildcmp_utf32_bin,
  my_strcasecmp_mb2_or_mb4,
  my_instr_mb,
  my_hash_sort_utf32_nopad,
  my_propagate_simple
};


MY_CHARSET_HANDLER my_charset_utf32_handler=
{
  NULL, /* init */
  my_numchars_utf32,
  my_charpos_utf32,
  my_lengthsp_utf32,
  my_numcells_mb,
  my_utf32_uni,
  my_uni_utf32,
  my_mb_ctype_mb,
  my_caseup_str_mb2_or_mb4,
  my_casedn_str_mb2_or_mb4,
  my_caseup_utf32,
  my_casedn_utf32,
  my_snprintf_utf32,
  my_l10tostr_mb2_or_mb4,
  my_ll10tostr_mb2_or_mb4,
  my_fill_utf32,
  my_strntol_mb2_or_mb4,
  my_strntoul_mb2_or_mb4,
  my_strntoll_mb2_or_mb4,
  my_strntoull_mb2_or_mb4,
  my_strntod_mb2_or_mb4,
  my_strtoll10_utf32,
  my_strntoull10rnd_mb2_or_mb4,
  my_scan_utf32,
  my_charlen_utf32,
  my_well_formed_char_length_utf32,
  my_copy_fix_mb2_or_mb4,
  my_uni_utf32,
};


struct charset_info_st my_charset_utf32_general_ci=
{
  60,0,0,              /* number       */
  MY_CS_COMPILED|MY_CS_PRIMARY|MY_CS_STRNXFRM|MY_CS_UNICODE|MY_CS_NONASCII,
  "utf32",             /* cs name    */
  "utf32_general_ci",  /* name         */
  "UTF-32 Unicode",    /* comment      */
  NULL,                /* tailoring    */
  NULL,                /* ctype        */
  NULL,                /* to_lower     */
  NULL,                /* to_upper     */
  NULL,                /* sort_order   */
  NULL,                /* uca          */
  NULL,                /* tab_to_uni   */
  NULL,                /* tab_from_uni */
  &my_unicase_default, /* caseinfo     */
  NULL,                /* state_map    */
  NULL,                /* ident_map    */
  1,                   /* strxfrm_multiply */
  1,                   /* caseup_multiply  */
  1,                   /* casedn_multiply  */
  4,                   /* mbminlen     */
  4,                   /* mbmaxlen     */
  0,                   /* min_sort_char */
  0xFFFF,              /* max_sort_char */
  ' ',                 /* pad char      */
  0,                   /* escape_with_backslash_is_dangerous */
  1,                   /* levels_for_order   */
  &my_charset_utf32_handler,
  &my_collation_utf32_general_ci_handler
};


struct charset_info_st my_charset_utf32_bin=
{
  61,0,0,              /* number       */
  MY_CS_COMPILED|MY_CS_BINSORT|MY_CS_STRNXFRM|MY_CS_UNICODE|MY_CS_NONASCII,
  "utf32",             /* cs name    */
  "utf32_bin",         /* name         */
  "UTF-32 Unicode",    /* comment      */
  NULL,                /* tailoring    */
  NULL,                /* ctype        */
  NULL,                /* to_lower     */
  NULL,                /* to_upper     */
  NULL,                /* sort_order   */
  NULL,                /* uca          */
  NULL,                /* tab_to_uni   */
  NULL,                /* tab_from_uni */
  &my_unicase_default, /* caseinfo     */
  NULL,                /* state_map    */
  NULL,                /* ident_map    */
  1,                   /* strxfrm_multiply */
  1,                   /* caseup_multiply  */
  1,                   /* casedn_multiply  */
  4,                   /* mbminlen     */
  4,                   /* mbmaxlen     */
  0,                   /* min_sort_char */
  0xFFFF,              /* max_sort_char */
  ' ',                 /* pad char      */
  0,                   /* escape_with_backslash_is_dangerous */
  1,                   /* levels_for_order   */
  &my_charset_utf32_handler,
  &my_collation_utf32_bin_handler
};


struct charset_info_st my_charset_utf32_general_nopad_ci=
{
  MY_NOPAD_ID(60),0,0, /* number           */
  MY_CS_COMPILED|MY_CS_STRNXFRM|MY_CS_UNICODE|MY_CS_NONASCII|MY_CS_NOPAD,
  "utf32",             /* cs name          */
  "utf32_general_nopad_ci", /* name        */
  "UTF-32 Unicode",    /* comment          */
  NULL,                /* tailoring        */
  NULL,                /* ctype            */
  NULL,                /* to_lower         */
  NULL,                /* to_upper         */
  NULL,                /* sort_order       */
  NULL,                /* uca              */
  NULL,                /* tab_to_uni       */
  NULL,                /* tab_from_uni     */
  &my_unicase_default, /* caseinfo         */
  NULL,                /* state_map        */
  NULL,                /* ident_map        */
  1,                   /* strxfrm_multiply */
  1,                   /* caseup_multiply  */
  1,                   /* casedn_multiply  */
  4,                   /* mbminlen         */
  4,                   /* mbmaxlen         */
  0,                   /* min_sort_char    */
  0xFFFF,              /* max_sort_char    */
  ' ',                 /* pad char         */
  0,                   /* escape_with_backslash_is_dangerous */
  1,                   /* levels_for_order */
  &my_charset_utf32_handler,
  &my_collation_utf32_general_nopad_ci_handler
};


struct charset_info_st my_charset_utf32_nopad_bin=
{
  MY_NOPAD_ID(61),0,0, /* number           */
  MY_CS_COMPILED|MY_CS_BINSORT|MY_CS_STRNXFRM|MY_CS_UNICODE|MY_CS_NONASCII|
  MY_CS_NOPAD,
  "utf32",             /* cs name          */
  "utf32_nopad_bin",   /* name             */
  "UTF-32 Unicode",    /* comment          */
  NULL,                /* tailoring        */
  NULL,                /* ctype            */
  NULL,                /* to_lower         */
  NULL,                /* to_upper         */
  NULL,                /* sort_order       */
  NULL,                /* uca              */
  NULL,                /* tab_to_uni       */
  NULL,                /* tab_from_uni     */
  &my_unicase_default, /* caseinfo         */
  NULL,                /* state_map        */
  NULL,                /* ident_map        */
  1,                   /* strxfrm_multiply */
  1,                   /* caseup_multiply  */
  1,                   /* casedn_multiply  */
  4,                   /* mbminlen         */
  4,                   /* mbmaxlen         */
  0,                   /* min_sort_char    */
  0xFFFF,              /* max_sort_char    */
  ' ',                 /* pad char         */
  0,                   /* escape_with_backslash_is_dangerous */
  1,                   /* levels_for_order */
  &my_charset_utf32_handler,
  &my_collation_utf32_nopad_bin_handler
};


#endif /* HAVE_CHARSET_utf32 */


#ifdef HAVE_CHARSET_ucs2

#include "ctype-ucs2.h"

static const uchar ctype_ucs2[] = {
    0,
   32, 32, 32, 32, 32, 32, 32, 32, 32, 40, 40, 40, 40, 40, 32, 32,
   32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32,
   72, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
  132,132,132,132,132,132,132,132,132,132, 16, 16, 16, 16, 16, 16,
   16,129,129,129,129,129,129,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1, 16, 16, 16, 16, 16,
   16,130,130,130,130,130,130,  2,  2,  2,  2,  2,  2,  2,  2,  2,
    2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2, 16, 16, 16, 16, 32,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0
};

static const uchar to_lower_ucs2[] = {
    0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15,
   16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
   32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47,
   48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63,
   64, 97, 98, 99,100,101,102,103,104,105,106,107,108,109,110,111,
  112,113,114,115,116,117,118,119,120,121,122, 91, 92, 93, 94, 95,
   96, 97, 98, 99,100,101,102,103,104,105,106,107,108,109,110,111,
  112,113,114,115,116,117,118,119,120,121,122,123,124,125,126,127,
  128,129,130,131,132,133,134,135,136,137,138,139,140,141,142,143,
  144,145,146,147,148,149,150,151,152,153,154,155,156,157,158,159,
  160,161,162,163,164,165,166,167,168,169,170,171,172,173,174,175,
  176,177,178,179,180,181,182,183,184,185,186,187,188,189,190,191,
  192,193,194,195,196,197,198,199,200,201,202,203,204,205,206,207,
  208,209,210,211,212,213,214,215,216,217,218,219,220,221,222,223,
  224,225,226,227,228,229,230,231,232,233,234,235,236,237,238,239,
  240,241,242,243,244,245,246,247,248,249,250,251,252,253,254,255
};

static const uchar to_upper_ucs2[] = {
    0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15,
   16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
   32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47,
   48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63,
   64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79,
   80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95,
   96, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79,
   80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90,123,124,125,126,127,
  128,129,130,131,132,133,134,135,136,137,138,139,140,141,142,143,
  144,145,146,147,148,149,150,151,152,153,154,155,156,157,158,159,
  160,161,162,163,164,165,166,167,168,169,170,171,172,173,174,175,
  176,177,178,179,180,181,182,183,184,185,186,187,188,189,190,191,
  192,193,194,195,196,197,198,199,200,201,202,203,204,205,206,207,
  208,209,210,211,212,213,214,215,216,217,218,219,220,221,222,223,
  224,225,226,227,228,229,230,231,232,233,234,235,236,237,238,239,
  240,241,242,243,244,245,246,247,248,249,250,251,252,253,254,255
};


/* Definitions for strcoll.ic */
#define IS_MB2_CHAR(x,y)            (1)
#define UCS2_CODE(b0,b1)            (((uchar) b0) << 8 | ((uchar) b1))


static inline int my_weight_mb2_ucs2_general_ci(uchar b0, uchar b1)
{
  my_wc_t wc= UCS2_CODE(b0, b1);
  MY_UNICASE_CHARACTER *page= my_unicase_default_pages[wc >> 8];
  return (int) (page ? page[wc & 0xFF].sort : wc);
}


#define MY_FUNCTION_NAME(x)      my_ ## x ## _ucs2_general_ci
#define DEFINE_STRNXFRM_UNICODE
#define DEFINE_STRNXFRM_UNICODE_NOPAD
#define MY_MB_WC(cs, pwc, s, e)  my_mb_wc_ucs2_quick(pwc, s, e)
#define OPTIMIZE_ASCII           0
#define UNICASE_MAXCHAR          MY_UNICASE_INFO_DEFAULT_MAXCHAR
#define UNICASE_PAGE0            my_unicase_default_page00
#define UNICASE_PAGES            my_unicase_default_pages
#define WEIGHT_ILSEQ(x)          (0xFF0000 + (uchar) (x))
#define WEIGHT_MB2(b0,b1)        my_weight_mb2_ucs2_general_ci(b0,b1)
#include "strcoll.ic"


#define MY_FUNCTION_NAME(x)      my_ ## x ## _ucs2_bin
#define DEFINE_STRNXFRM_UNICODE_BIN2
#define MY_MB_WC(cs, pwc, s, e)  my_mb_wc_ucs2_quick(pwc, s, e)
#define OPTIMIZE_ASCII           0
#define WEIGHT_ILSEQ(x)          (0xFF0000 + (uchar) (x))
#define WEIGHT_MB2(b0,b1)        UCS2_CODE(b0,b1)
#include "strcoll.ic"


#define DEFINE_STRNNCOLLSP_NOPAD
#define MY_FUNCTION_NAME(x)    my_ ## x ## _ucs2_general_nopad_ci
#define WEIGHT_ILSEQ(x)        (0xFF0000 + (uchar) (x))
#define WEIGHT_MB2(b0,b1)      my_weight_mb2_ucs2_general_ci(b0,b1)
#include "strcoll.ic"


#define DEFINE_STRNNCOLLSP_NOPAD
#define MY_FUNCTION_NAME(x)    my_ ## x ## _ucs2_nopad_bin
#define WEIGHT_ILSEQ(x)        (0xFF0000 + (uchar) (x))
#define WEIGHT_MB2(b0,b1)      UCS2_CODE(b0,b1)
#include "strcoll.ic"


static int
my_charlen_ucs2(CHARSET_INFO *cs __attribute__((unused)),
		const uchar *s, const uchar *e)
{
  return s + 2 > e ? MY_CS_TOOSMALLN(2) : 2;
}


static int my_ucs2_uni(CHARSET_INFO *cs __attribute__((unused)),
		       my_wc_t * pwc, const uchar *s, const uchar *e)
{
  return my_mb_wc_ucs2_quick(pwc, s, e);
}

static int my_uni_ucs2(CHARSET_INFO *cs __attribute__((unused)) ,
		       my_wc_t wc, uchar *r, uchar *e)
{
  if ( r+2 > e ) 
    return MY_CS_TOOSMALL2;

  if (wc > 0xFFFF) /* UCS2 does not support characters outside BMP */
    return MY_CS_ILUNI;

  r[0]= (uchar) (wc >> 8);
  r[1]= (uchar) (wc & 0xFF);
  return 2;
}


static inline void
my_tolower_ucs2(MY_UNICASE_INFO *uni_plane, my_wc_t *wc)
{
  MY_UNICASE_CHARACTER *page;
  if ((page= uni_plane->page[(*wc >> 8) & 0xFF]))
    *wc= page[*wc & 0xFF].tolower;
}


static inline void
my_toupper_ucs2(MY_UNICASE_INFO *uni_plane, my_wc_t *wc)
{
  MY_UNICASE_CHARACTER *page;
  if ((page= uni_plane->page[(*wc >> 8) & 0xFF]))
    *wc= page[*wc & 0xFF].toupper;
}


static inline void
my_tosort_ucs2(MY_UNICASE_INFO *uni_plane, my_wc_t *wc)
{
  MY_UNICASE_CHARACTER *page;
  if ((page= uni_plane->page[(*wc >> 8) & 0xFF]))
    *wc= page[*wc & 0xFF].sort;
}

static size_t my_caseup_ucs2(CHARSET_INFO *cs, const char *src, size_t srclen,
                           char *dst, size_t dstlen)
{
  my_wc_t wc;
  int res;
  const char *srcend= src + srclen;
  char *dstend= dst + dstlen;
  MY_UNICASE_INFO *uni_plane= cs->caseinfo;
  DBUG_ASSERT(srclen <= dstlen);
  
  while ((src < srcend) &&
         (res= my_ucs2_uni(cs, &wc, (uchar *)src, (uchar*) srcend)) > 0)
  {
    my_toupper_ucs2(uni_plane, &wc);
    if (res != my_uni_ucs2(cs, wc, (uchar*) dst, (uchar*) dstend))
      break;
    src+= res;
    dst+= res;
  }
  return srclen;
}


static void
my_hash_sort_ucs2_nopad(CHARSET_INFO *cs, const uchar *s, size_t slen,
                        ulong *nr1, ulong *nr2)
{
  my_wc_t wc;
  int res;
  const uchar *e=s+slen;
  MY_UNICASE_INFO *uni_plane= cs->caseinfo;
  register ulong m1= *nr1, m2= *nr2;

  while ((s < e) && (res=my_ucs2_uni(cs,&wc, (uchar *)s, (uchar*)e)) >0)
  {
    my_tosort_ucs2(uni_plane, &wc);
    MY_HASH_ADD_16(m1, m2, wc);
    s+=res;
  }
  *nr1= m1;
  *nr2= m2;
}


static void my_hash_sort_ucs2(CHARSET_INFO *cs, const uchar *s, size_t slen,
			      ulong *nr1, ulong *nr2)
{
  size_t lengthsp= my_lengthsp_mb2(cs, (const char *) s, slen);
  my_hash_sort_ucs2_nopad(cs, s, lengthsp, nr1, nr2);
}

static size_t my_casedn_ucs2(CHARSET_INFO *cs, const char *src, size_t srclen,
                           char *dst, size_t dstlen)
{
  my_wc_t wc;
  int res;
  const char *srcend= src + srclen;
  char *dstend= dst + dstlen;
  MY_UNICASE_INFO *uni_plane= cs->caseinfo;
  DBUG_ASSERT(srclen <= dstlen);

  while ((src < srcend) &&
         (res= my_ucs2_uni(cs, &wc, (uchar*) src, (uchar*) srcend)) > 0)
  {
    my_tolower_ucs2(uni_plane, &wc);
    if (res != my_uni_ucs2(cs, wc, (uchar*) dst, (uchar*) dstend))
      break;
    src+= res;
    dst+= res;
  }
  return srclen;
}


static void
my_fill_ucs2(CHARSET_INFO *cs __attribute__((unused)), 
             char *s, size_t l, int fill)
{
  DBUG_ASSERT(fill <= 0xFFFF);
#ifdef WAITING_FOR_GCC_VECTORIZATION_BUG_TO_BE_FIXED
  /*
    This code with int2store() is known to be faster on some processors,
    but crashes on other processors due to a possible bug in GCC's
    -ftree-vectorization (which is enabled in -O3) in case of
    a   non-aligned memory. See here for details:
    http://gcc.gnu.org/bugzilla/show_bug.cgi?id=58039
  */
  char *last= s + l - 2;
  uint16 tmp= (fill >> 8) + ((fill & 0xFF) << 8); /* swap bytes */
  DBUG_ASSERT(fill <= 0xFFFF);
  for ( ; s <= last; s+= 2)
    int2store(s, tmp); /* store little-endian */
#else
  for ( ; l >= 2; s[0]= (fill >> 8), s[1]= (fill & 0xFF), s+= 2, l-= 2);
#endif
}


static
size_t my_numchars_ucs2(CHARSET_INFO *cs __attribute__((unused)),
                        const char *b, const char *e)
{
  return (size_t) (e-b)/2;
}


static
size_t my_charpos_ucs2(CHARSET_INFO *cs __attribute__((unused)),
                       const char *b  __attribute__((unused)),
                       const char *e  __attribute__((unused)),
                       size_t pos)
{
  size_t string_length= (size_t) (e - b);
  return pos > string_length ? string_length + 2 : pos * 2;
}


static size_t
my_well_formed_char_length_ucs2(CHARSET_INFO *cs __attribute__((unused)),
                                const char *b, const char *e,
                                size_t nchars, MY_STRCOPY_STATUS *status)
{
  size_t length= e - b;
  if (nchars * 2 <= length)
  {
    status->m_well_formed_error_pos= NULL;
    status->m_source_end_pos= b + (nchars * 2);
    return nchars;
  }
  if (length % 2)
  {
    status->m_well_formed_error_pos= status->m_source_end_pos= e - 1;
  }
  else
  {
    status->m_well_formed_error_pos= NULL;
    status->m_source_end_pos= e;
  }
  return length / 2;
}


static
int my_wildcmp_ucs2_ci(CHARSET_INFO *cs,
		    const char *str,const char *str_end,
		    const char *wildstr,const char *wildend,
		    int escape, int w_one, int w_many)
{
  MY_UNICASE_INFO *uni_plane= cs->caseinfo;
  return my_wildcmp_unicode(cs,str,str_end,wildstr,wildend,
                            escape,w_one,w_many,uni_plane); 
}


static
int my_wildcmp_ucs2_bin(CHARSET_INFO *cs,
		    const char *str,const char *str_end,
		    const char *wildstr,const char *wildend,
		    int escape, int w_one, int w_many)
{
  return my_wildcmp_unicode(cs,str,str_end,wildstr,wildend,
                            escape,w_one,w_many,NULL); 
}


static void
my_hash_sort_ucs2_nopad_bin(CHARSET_INFO *cs __attribute__((unused)),
                            const uchar *key, size_t len,
                            ulong *nr1, ulong *nr2)
{
  const uchar *end= key + len;
  register ulong m1= *nr1, m2= *nr2;
  for ( ; key < end ; key++)
  {
    MY_HASH_ADD(m1, m2, (uint)*key);
  }
  *nr1= m1;
  *nr2= m2;
}


static void
my_hash_sort_ucs2_bin(CHARSET_INFO *cs,
                      const uchar *key, size_t len, ulong *nr1, ulong *nr2)
{
  size_t lengthsp= my_lengthsp_mb2(cs, (const char *) key, len);
  my_hash_sort_ucs2_nopad_bin(cs, key, lengthsp, nr1, nr2);
}


static MY_COLLATION_HANDLER my_collation_ucs2_general_ci_handler =
{
    NULL,		/* init */
    my_strnncoll_ucs2_general_ci,
    my_strnncollsp_ucs2_general_ci,
    my_strnxfrm_ucs2_general_ci,
    my_strnxfrmlen_unicode,
    my_like_range_generic,
    my_wildcmp_ucs2_ci,
    my_strcasecmp_mb2_or_mb4,
    my_instr_mb,
    my_hash_sort_ucs2,
    my_propagate_simple
};


static MY_COLLATION_HANDLER my_collation_ucs2_bin_handler =
{
    NULL,		/* init */
    my_strnncoll_ucs2_bin,
    my_strnncollsp_ucs2_bin,
    my_strnxfrm_ucs2_bin,
    my_strnxfrmlen_unicode,
    my_like_range_generic,
    my_wildcmp_ucs2_bin,
    my_strcasecmp_mb2_or_mb4,
    my_instr_mb,
    my_hash_sort_ucs2_bin,
    my_propagate_simple
};


static MY_COLLATION_HANDLER my_collation_ucs2_general_nopad_ci_handler =
{
    NULL,		/* init */
    my_strnncoll_ucs2_general_ci,
    my_strnncollsp_ucs2_general_nopad_ci,
    my_strnxfrm_nopad_ucs2_general_ci,
    my_strnxfrmlen_unicode,
    my_like_range_generic,
    my_wildcmp_ucs2_ci,
    my_strcasecmp_mb2_or_mb4,
    my_instr_mb,
    my_hash_sort_ucs2_nopad,
    my_propagate_simple
};


static MY_COLLATION_HANDLER my_collation_ucs2_nopad_bin_handler =
{
    NULL,		/* init */
    my_strnncoll_ucs2_bin,
    my_strnncollsp_ucs2_nopad_bin,
    my_strnxfrm_nopad_ucs2_bin,
    my_strnxfrmlen_unicode,
    my_like_range_generic,
    my_wildcmp_ucs2_bin,
    my_strcasecmp_mb2_or_mb4,
    my_instr_mb,
    my_hash_sort_ucs2_nopad_bin,
    my_propagate_simple
};


MY_CHARSET_HANDLER my_charset_ucs2_handler=
{
    NULL,		/* init */
    my_numchars_ucs2,
    my_charpos_ucs2,
    my_lengthsp_mb2,
    my_numcells_mb,
    my_ucs2_uni,	/* mb_wc        */
    my_uni_ucs2,	/* wc_mb        */
    my_mb_ctype_mb,
    my_caseup_str_mb2_or_mb4,
    my_casedn_str_mb2_or_mb4,
    my_caseup_ucs2,
    my_casedn_ucs2,
    my_snprintf_mb2,
    my_l10tostr_mb2_or_mb4,
    my_ll10tostr_mb2_or_mb4,
    my_fill_ucs2,
    my_strntol_mb2_or_mb4,
    my_strntoul_mb2_or_mb4,
    my_strntoll_mb2_or_mb4,
    my_strntoull_mb2_or_mb4,
    my_strntod_mb2_or_mb4,
    my_strtoll10_mb2,
    my_strntoull10rnd_mb2_or_mb4,
    my_scan_mb2,
    my_charlen_ucs2,
    my_well_formed_char_length_ucs2,
    my_copy_fix_mb2_or_mb4,
    my_uni_ucs2,
};


struct charset_info_st my_charset_ucs2_general_ci=
{
    35,0,0,		/* number       */
    MY_CS_COMPILED|MY_CS_PRIMARY|MY_CS_STRNXFRM|MY_CS_UNICODE|MY_CS_NONASCII,
    "ucs2",		/* cs name    */
    "ucs2_general_ci",	/* name         */
    "",			/* comment      */
    NULL,		/* tailoring    */
    ctype_ucs2,		/* ctype        */
    to_lower_ucs2,	/* to_lower     */
    to_upper_ucs2,	/* to_upper     */
    to_upper_ucs2,	/* sort_order   */
    NULL,		/* uca          */
    NULL,		/* tab_to_uni   */
    NULL,		/* tab_from_uni */
    &my_unicase_default,/* caseinfo     */
    NULL,		/* state_map    */
    NULL,		/* ident_map    */
    1,			/* strxfrm_multiply */
    1,                  /* caseup_multiply  */
    1,                  /* casedn_multiply  */
    2,			/* mbminlen     */
    2,			/* mbmaxlen     */
    0,			/* min_sort_char */
    0xFFFF,		/* max_sort_char */
    ' ',                /* pad char      */
    0,                  /* escape_with_backslash_is_dangerous */
    1,                  /* levels_for_order   */
    &my_charset_ucs2_handler,
    &my_collation_ucs2_general_ci_handler
};


struct charset_info_st my_charset_ucs2_general_mysql500_ci=
{
  159, 0, 0,                                       /* number           */
  MY_CS_COMPILED|MY_CS_STRNXFRM|MY_CS_UNICODE|MY_CS_NONASCII, /* state */
  "ucs2",                                          /* cs name          */
  "ucs2_general_mysql500_ci",                      /* name             */
  "",                                              /* comment          */
  NULL,                                            /* tailoring        */
  ctype_ucs2,                                      /* ctype            */
  to_lower_ucs2,                                   /* to_lower         */
  to_upper_ucs2,                                   /* to_upper         */
  to_upper_ucs2,                                   /* sort_order       */
  NULL,                                            /* uca              */
  NULL,                                            /* tab_to_uni       */
  NULL,                                            /* tab_from_uni     */
  &my_unicase_mysql500,                            /* caseinfo         */
  NULL,                                            /* state_map        */
  NULL,                                            /* ident_map        */
  1,                                               /* strxfrm_multiply */
  1,                                               /* caseup_multiply  */
  1,                                               /* casedn_multiply  */
  2,                                               /* mbminlen         */
  2,                                               /* mbmaxlen         */
  0,                                               /* min_sort_char    */
  0xFFFF,                                          /* max_sort_char    */
  ' ',                                             /* pad char         */
  0,                          /* escape_with_backslash_is_dangerous    */
  1,                                               /* levels_for_order   */
  &my_charset_ucs2_handler,
  &my_collation_ucs2_general_ci_handler
};


struct charset_info_st my_charset_ucs2_bin=
{
    90,0,0,		/* number       */
    MY_CS_COMPILED|MY_CS_BINSORT|MY_CS_UNICODE|MY_CS_NONASCII,
    "ucs2",		/* cs name    */
    "ucs2_bin",		/* name         */
    "",			/* comment      */
    NULL,		/* tailoring    */
    ctype_ucs2,		/* ctype        */
    to_lower_ucs2,	/* to_lower     */
    to_upper_ucs2,	/* to_upper     */
    NULL,		/* sort_order   */
    NULL,		/* uca          */
    NULL,		/* tab_to_uni   */
    NULL,		/* tab_from_uni */
    &my_unicase_default,/* caseinfo     */
    NULL,		/* state_map    */
    NULL,		/* ident_map    */
    1,			/* strxfrm_multiply */
    1,                  /* caseup_multiply  */
    1,                  /* casedn_multiply  */
    2,			/* mbminlen     */
    2,			/* mbmaxlen     */
    0,			/* min_sort_char */
    0xFFFF,		/* max_sort_char */
    ' ',                /* pad char      */
    0,                  /* escape_with_backslash_is_dangerous */
    1,                  /* levels_for_order   */
    &my_charset_ucs2_handler,
    &my_collation_ucs2_bin_handler
};


struct charset_info_st my_charset_ucs2_general_nopad_ci=
{
    MY_NOPAD_ID(35),0,0,     /* number           */
    MY_CS_COMPILED|MY_CS_STRNXFRM|MY_CS_UNICODE|MY_CS_NONASCII|MY_CS_NOPAD,
    "ucs2",                  /* cs name          */
    "ucs2_general_nopad_ci", /* name             */
    "",                      /* comment          */
    NULL,                    /* tailoring        */
    ctype_ucs2,              /* ctype            */
    to_lower_ucs2,           /* to_lower         */
    to_upper_ucs2,           /* to_upper         */
    to_upper_ucs2,           /* sort_order       */
    NULL,                    /* uca              */
    NULL,                    /* tab_to_uni       */
    NULL,                    /* tab_from_uni     */
    &my_unicase_default,     /* caseinfo         */
    NULL,                    /* state_map        */
    NULL,                    /* ident_map        */
    1,                       /* strxfrm_multiply */
    1,                       /* caseup_multiply  */
    1,                       /* casedn_multiply  */
    2,                       /* mbminlen         */
    2,                       /* mbmaxlen         */
    0,                       /* min_sort_char    */
    0xFFFF,                  /* max_sort_char    */
    ' ',                     /* pad char         */
    0,                       /* escape_with_backslash_is_dangerous */
    1,                       /* levels_for_order */
    &my_charset_ucs2_handler,
    &my_collation_ucs2_general_nopad_ci_handler
};


struct charset_info_st my_charset_ucs2_nopad_bin=
{
    MY_NOPAD_ID(90),0,0,     /* number           */
    MY_CS_COMPILED|MY_CS_BINSORT|MY_CS_UNICODE|MY_CS_NONASCII|MY_CS_NOPAD,
    "ucs2",                  /* cs name          */
    "ucs2_nopad_bin",        /* name             */
    "",                      /* comment          */
    NULL,                    /* tailoring        */
    ctype_ucs2,              /* ctype            */
    to_lower_ucs2,           /* to_lower         */
    to_upper_ucs2,           /* to_upper         */
    NULL,                    /* sort_order       */
    NULL,                    /* uca              */
    NULL,                    /* tab_to_uni       */
    NULL,                    /* tab_from_uni     */
    &my_unicase_default,     /* caseinfo         */
    NULL,                    /* state_map        */
    NULL,                    /* ident_map        */
    1,                       /* strxfrm_multiply */
    1,                       /* caseup_multiply  */
    1,                       /* casedn_multiply  */
    2,                       /* mbminlen         */
    2,                       /* mbmaxlen         */
    0,                       /* min_sort_char    */
    0xFFFF,                  /* max_sort_char    */
    ' ',                     /* pad char         */
    0,                       /* escape_with_backslash_is_dangerous */
    1,                       /* levels_for_order */
    &my_charset_ucs2_handler,
    &my_collation_ucs2_nopad_bin_handler
};

#endif /* HAVE_CHARSET_ucs2 */
