/* Copyright (c) 2000, 2011, Oracle and/or its affiliates.
   Copyright (c) 2009, 2020, MariaDB Corporation.

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

#include "strings_def.h"
#include <m_ctype.h>
#include <my_sys.h>
#include <my_base.h>
#include <my_handler_errors.h>
#include <mysql_com.h>                        /* For FLOATING_POINT_DECIMALS */

#define MAX_ARGS 32                           /* max positional args count*/
#define MAX_PRINT_INFO 32                     /* max print position count */
#define MAX_WIDTH      65535

#define LENGTH_ARG     1
#define WIDTH_ARG      2
#define PREZERO_ARG    4
#define ESCAPED_ARG    8 

typedef struct pos_arg_info ARGS_INFO;
typedef struct print_info PRINT_INFO;

struct pos_arg_info
{
  char arg_type;                              /* argument type */
  uint have_longlong;                         /* used from integer values */
  char *str_arg;                              /* string value of the arg */
  longlong longlong_arg;                      /* integer value of the arg */
  double double_arg;                          /* double value of the arg */
};


struct print_info
{
  char arg_type;                              /* argument type */
  size_t arg_idx;                             /* index of the positional arg */
  size_t length;                              /* print width or arg index */
  size_t width;                               /* print width or arg index */
  uint flags;
  const char *begin;                          /**/
  const char *end;                            /**/
};


/**
  Calculates print length or index of positional argument

  @param fmt         processed string
  @param length      print length or index of positional argument
  @param pre_zero    returns flags with PREZERO_ARG set if necessary

  @retval
    string position right after length digits
*/

static const char *get_length(const char *fmt, size_t *length, uint *pre_zero)
{
  
  for (; my_isdigit(&my_charset_latin1, *fmt); fmt++)
  {
    *length= *length * 10 + (uint)(*fmt - '0');
    if (!*length)
      *pre_zero|= PREZERO_ARG;                  /* first digit was 0 */
  }
  return fmt;
}


/*
  Get argument for '*' parameter

  @param fmt         processed string
  @param args_arr    Arguments to printf
  @param arg_count   Number of arguments to printf
  @param length      returns length of argument
  @param flag        returns flags with PREZERO_ARG set if necessary

  @return new fmt
*/

static const char *get_length_arg(const char *fmt, ARGS_INFO *args_arr,
                                  size_t *arg_count, size_t *length, uint *flags)
{
  fmt= get_length(fmt+1, length, flags);
  *arg_count= MY_MAX(*arg_count, *length);
  (*length)--;    
  DBUG_ASSERT(*fmt == '$' && *length < MAX_ARGS);
  args_arr[*length].arg_type= 'd';
  args_arr[*length].have_longlong= 0;
  return fmt+1;
}

/**
  Calculates print width or index of positional argument

  @param fmt            processed string
  @param have_longlong  TRUE if longlong is required

  @retval
    string position right after modifier symbol
*/

static const char *check_longlong(const char *fmt, uint *have_longlong)
{
  *have_longlong= 0;
  if (*fmt == 'l')
  {
    fmt++;
    if (*fmt != 'l')
      *have_longlong= (sizeof(long) == sizeof(longlong));
    else
    {
      fmt++;
      *have_longlong= 1;
    }
  }
  else if (*fmt == 'z')
  {
    fmt++;
    *have_longlong= (sizeof(size_t) == sizeof(longlong));
  }
  else if (*fmt == 'p')
    *have_longlong= (sizeof(void *) == sizeof(longlong));
  return fmt;
}


/**
  Returns escaped string

  @param cs         string charset
  @param to         buffer where escaped string will be placed
  @param end        end of buffer
  @param par        string to escape
  @param par_len    string length
  @param quote_char character for quoting

  @retval
    position in buffer which points on the end of escaped string
*/

static char *backtick_string(CHARSET_INFO *cs, char *to, const char *end,
                             char *par, size_t par_len, char quote_char,
                             my_bool cut)
{
  char *last[3]= {0,0,0};
  uint char_len;
  char *start= to;
  char *par_end= par + par_len;
  size_t buff_length= (size_t) (end - to);
  uint index= 0;

  if (buff_length <= par_len)
    goto err;
  *start++= quote_char;

  for ( ; par < par_end; par+= char_len)
  {
    uchar c= *(uchar *) par;
    if (cut)
    {
      last[index]= start;
      index= (index + 1) % 3;
    }
    char_len= my_ci_charlen_fix(cs, (const uchar *) par, (const uchar *) par_end);
    if (char_len == 1 && c == (uchar) quote_char )
    {
      if (start + 1 >= end)
        goto err;
      *start++= quote_char;
    }
    if (start + char_len >= end)
      goto err;
    start= strnmov(start, par, char_len);
  }

  if (start + 1 >= end)
    goto err;

  if (cut)
  {
    uint dots= 0;
    start= NULL;
    for (; dots < 3; dots++)
    {
      if (index == 0)
        index= 2;
      else
        index--;
      if (!last[index])
        break;
      start= last[index];
    }
    if (start == NULL)
      goto err; // there was no characters at all
    for (; dots; dots--)
      *start++= '.';

  }
  *start++= quote_char;
  return start;

err:
    *to='\0';
  return to;
}


/**
  Prints string argument
*/

static char *process_str_arg(CHARSET_INFO *cs, char *to, const char *end,
                             longlong length_arg, size_t width, char *par,
                             uint print_type, my_bool nice_cut)
{
  int well_formed_error;
  uint dots= 0;
  size_t plen, left_len= (size_t) (end - to) + 1, slen=0;
  my_bool left_fill= 1;
  size_t length;

  /*
    The sign of the length argument specific the string should be right
    or left adjusted
  */
  if (length_arg < 0)
  {
    length= (size_t) -length_arg;
    left_fill= 0;
  }
  else
    length= (size_t) length_arg;

  if (!par)
    par = (char*) "(null)";

  if (nice_cut)
  {
    plen= slen= strnlen(par, width + 1);
    if (plen > width)
      plen= width;
    if (left_len <= plen)
    {
      plen = left_len - 1;
      length= plen;
    }
    if ((slen > plen))
    {
      if (plen < 3)
      {
        dots= (uint) plen;
        plen= 0;
      }
      else
      {
        dots= 3;
        plen-= 3;
      }
    }
  }
  else
  {
    plen= slen= strnlen(par, width);
    dots= 0;
    if (left_len <= plen)
    {
      plen = left_len - 1;
      length= plen;
    }
  }

  plen= my_well_formed_length(cs, par, par + plen, width, &well_formed_error);
  if (print_type & ESCAPED_ARG)
  {
    const char *org_to= to;
    to= backtick_string(cs, to, end, par, plen + dots, '`', MY_TEST(dots));
    plen= (size_t) (to - org_to);
    dots= 0;
  }
  else
  {
    if (left_fill)
    {
      if (plen + dots < length)
        to= strfill(to, length - plen - dots, ' ');
    }
    to= strnmov(to,par,plen);
    if (dots)
      to= strfill(to, dots, '.');
  }

  if (!left_fill && plen + dots < length)
    to= strfill(to, length - plen - dots, ' ');
  return to;
}


/**
  Prints binary argument
*/

static char *process_bin_arg(char *to, char *end, size_t width, char *par)
{
  DBUG_ASSERT(to <= end);
  if (to + width + 1 > end)
    width= end - to - 1;  /* sign doesn't matter */
  memmove(to, par, width);
  to+= width;
  return to;
}


/**
  Prints double or float argument
*/

static char *process_dbl_arg(char *to, char *end, size_t width,
                             double par, char arg_type)
{
  if (width == MAX_WIDTH)
    width= FLT_DIG; /* width not set, use default */
  else if (width >= FLOATING_POINT_DECIMALS)
    width= FLOATING_POINT_DECIMALS - 1; /* max.precision for my_fcvt() */
  width= MY_MIN(width, (size_t)(end-to) - 1);
  
  if (arg_type == 'f')
    to+= my_fcvt(par, (int)width , to, NULL);
  else
    to+= my_gcvt(par, MY_GCVT_ARG_DOUBLE, (int) width , to, NULL);
  return to;
}


/**
  Prints integer argument
*/

static char *process_int_arg(char *to, const char *end, size_t length,
                             longlong par, char arg_type, uint print_type)
{
  size_t res_length, to_length;
  char *store_start= to, *store_end;
  char buff[32];

  if ((to_length= (size_t) (end-to)) < 16 || length)
    store_start= buff;

  if (arg_type == 'd' || arg_type == 'i')
    store_end= longlong10_to_str(par, store_start, -10);
  else if (arg_type == 'u')
    store_end= longlong10_to_str(par, store_start, 10);
  else if (arg_type == 'p')
  {
    store_start[0]= '0';
    store_start[1]= 'x';
    store_end= ll2str(par, store_start + 2, 16, 0);
  }
  else if (arg_type == 'o')
  {
    store_end= ll2str(par, store_start, 8, 0);
  }
  else
  {
    DBUG_ASSERT(arg_type == 'X' || arg_type =='x');
    store_end= ll2str(par, store_start, 16, (arg_type == 'X'));
  }

  if ((res_length= (size_t) (store_end - store_start)) > to_length)
    return to;                           /* num doesn't fit in output */
  /* If %#d syntax was used, we have to pre-zero/pre-space the string */
  if (store_start == buff)
  {
    length= MY_MIN(length, to_length);
    if (res_length < length)
    {
      size_t diff= (length- res_length);
      bfill(to, diff, (print_type & PREZERO_ARG) ? '0' : ' ');
      if (arg_type == 'p' && print_type & PREZERO_ARG)
      {
        if (diff > 1)
          to[1]= 'x';
        else
          store_start[0]= 'x';
        store_start[1]= '0';
      }
      to+= diff;
    }
    bmove(to, store_start, res_length);
  }
  to+= res_length;
  return to;
}


/**
  Processed positional arguments.

  @param cs         string charset
  @param to         buffer where processed string will be place
  @param end        end of buffer
  @param par        format string
  @param arg_index  arg index of the first occurrence of positional arg
  @param ap         list of parameters

  @retval
    end of buffer where processed string is placed
*/

static char *process_args(CHARSET_INFO *cs, char *to, char *end,
                          const char* fmt, size_t arg_index, va_list ap)
{
  ARGS_INFO args_arr[MAX_ARGS];
  PRINT_INFO print_arr[MAX_PRINT_INFO];
  size_t idx= 0, arg_count= arg_index;

start:
  /* Here we are at the beginning of positional argument, right after $ */
  arg_index--;
  print_arr[idx].flags= 0;
  if (*fmt == '`')
  {
    print_arr[idx].flags|= ESCAPED_ARG;
    fmt++;
  }
  if (*fmt == '-')
    fmt++;
  print_arr[idx].length= print_arr[idx].width= 0;
  /* Get print length */
  if (*fmt == '*')
  {          
    fmt= get_length_arg(fmt, args_arr, &arg_count, &print_arr[idx].length,
                        &print_arr[idx].flags);
    print_arr[idx].flags|= LENGTH_ARG;
  }
  else
    fmt= get_length(fmt, &print_arr[idx].length, &print_arr[idx].flags);
  
  if (*fmt == '.')
  {
    uint unused_flags= 0;
    fmt++;
    /* Get print width */
    if (*fmt == '*')
    {
      fmt= get_length_arg(fmt, args_arr, &arg_count, &print_arr[idx].width,
                          &unused_flags);
      print_arr[idx].flags|= WIDTH_ARG;
    }
    else
      fmt= get_length(fmt, &print_arr[idx].width, &unused_flags);
  }
  else
    print_arr[idx].width= MAX_WIDTH;

  fmt= check_longlong(fmt, &args_arr[arg_index].have_longlong);
  args_arr[arg_index].arg_type= print_arr[idx].arg_type= *fmt;
  
  print_arr[idx].arg_idx= arg_index;
  print_arr[idx].begin= ++fmt;

  while (*fmt && *fmt != '%')
    fmt++;

  if (!*fmt)                                  /* End of format string */
  {
    uint i;
    print_arr[idx].end= fmt;
    /* Obtain parameters from the list */
    for (i= 0 ; i < arg_count; i++)
    {
      switch (args_arr[i].arg_type) {
      case 's':
      case 'b':
      case 'T':
        args_arr[i].str_arg= va_arg(ap, char *);
        break;
      case 'f':
      case 'g':
        args_arr[i].double_arg= va_arg(ap, double);
        break;
      case 'd':
      case 'i':
      case 'u':
      case 'x':
      case 'X':
      case 'o':
      case 'p':
        if (args_arr[i].have_longlong)
          args_arr[i].longlong_arg= va_arg(ap,longlong);
        else if (args_arr[i].arg_type == 'd' || args_arr[i].arg_type == 'i')
          args_arr[i].longlong_arg= va_arg(ap, int);
        else
          args_arr[i].longlong_arg= va_arg(ap, uint);
        break;
      case 'M':
      case 'c':
        args_arr[i].longlong_arg= va_arg(ap, int);
        break;
      default:
        DBUG_ASSERT(0);
      }
    }
    /* Print result string */
    for (i= 0; i <= idx; i++)
    {
      size_t width= 0, length= 0;
      switch (print_arr[i].arg_type) {
      case 's':
      case 'T':
      {
        longlong min_field_width;
        char *par= args_arr[print_arr[i].arg_idx].str_arg;
        width= (print_arr[i].flags & WIDTH_ARG)
          ? (size_t)args_arr[print_arr[i].width].longlong_arg
          : print_arr[i].width;
        min_field_width= (print_arr[i].flags & LENGTH_ARG)
          ? args_arr[print_arr[i].length].longlong_arg
          : (longlong) print_arr[i].length;
        to= process_str_arg(cs, to, end, min_field_width, width, par,
                            print_arr[i].flags,
                            (print_arr[i].arg_type == 'T'));
        break;
      }
      case 'b':
      {
        char *par = args_arr[print_arr[i].arg_idx].str_arg;
        width= (print_arr[i].flags & WIDTH_ARG)
          ? (size_t)args_arr[print_arr[i].width].longlong_arg
          : print_arr[i].width;
        to= process_bin_arg(to, end, width, par);
        break;
      }
      case 'c':
      {
        if (to == end)
          break;
        *to++= (char) args_arr[print_arr[i].arg_idx].longlong_arg;
        break;
      }
      case 'f':
      case 'g':
      {
        double d= args_arr[print_arr[i].arg_idx].double_arg;
        width= (print_arr[i].flags & WIDTH_ARG) ?
          (uint)args_arr[print_arr[i].width].longlong_arg : print_arr[i].width;
        to= process_dbl_arg(to, end, width, d, print_arr[i].arg_type);
        break;
      }
      case 'd':
      case 'i':
      case 'u':
      case 'x':
      case 'X':
      case 'o':
      case 'p':
      {
        /* Integer parameter */
        longlong larg;
        length= (print_arr[i].flags & LENGTH_ARG)
          ? (size_t)args_arr[print_arr[i].length].longlong_arg
          : print_arr[i].length;

        larg = args_arr[print_arr[i].arg_idx].longlong_arg;
        to= process_int_arg(to, end, length, larg, print_arr[i].arg_type,
                            print_arr[i].flags);
        break;
      }
      case 'M':
      {
        longlong larg;
        const char *real_end;

        width= (print_arr[i].flags & WIDTH_ARG)
          ? (size_t)args_arr[print_arr[i].width].longlong_arg
          : print_arr[i].width;

        real_end= MY_MIN(to + width, end);

        larg = args_arr[print_arr[i].arg_idx].longlong_arg;
        to= process_int_arg(to, real_end, 0, larg, 'd', print_arr[i].flags);
        if (real_end - to >= 3)
        {
          char errmsg_buff[MYSYS_STRERROR_SIZE];
          *to++= ' ';
          *to++= '"';
          my_strerror(errmsg_buff, sizeof(errmsg_buff), (int) larg);
          to= process_str_arg(cs, to, real_end, 0, width, errmsg_buff,
                              print_arr[i].flags, 1);
          if (real_end > to) *to++= '"';
        }
        break;
      }
      default:
        break;
      }

      if (to == end)
        break;

      /* Copy data after the % format expression until next % */
      length= MY_MIN(end - to , print_arr[i].end - print_arr[i].begin);
      if (to + length < end)
        length++;
      to= strnmov(to, print_arr[i].begin, length);
    }
    DBUG_ASSERT(to <= end);
    *to='\0';				/* End of errmessage */
    return to;
  }
  else
  {
    uint unused_flags= 0;
    /* Process next positional argument*/
    DBUG_ASSERT(*fmt == '%');
    print_arr[idx].end= fmt - 1;
    idx++;
    fmt++;
    arg_index= 0;
    fmt= get_length(fmt, &arg_index, &unused_flags);
    DBUG_ASSERT(*fmt == '$');
    fmt++;
    arg_count= MY_MAX(arg_count, arg_index);
    goto start;
  }

  return 0;
}



/**
  Produces output string according to a format string

  See the detailed documentation around my_snprintf_service_st

  @param cs         string charset
  @param to         buffer where processed string will be place
  @param n          size of buffer
  @param par        format string
  @param ap         list of parameters

  @retval
    length of result string
*/

size_t my_vsnprintf_ex(CHARSET_INFO *cs, char *to, size_t n,
                       const char* fmt, va_list ap)
{
  char *start=to, *end=to+n-1;
  size_t length, width;
  uint print_type, have_longlong;

  for (; *fmt ; fmt++)
  {
    if (*fmt != '%')
    {
      if (to == end)                            /* End of buffer */
	break;
      *to++= *fmt;                            /* Copy ordinary char */
      continue;
    }
    fmt++;					/* skip '%' */

    length= width= 0;
    print_type= 0;

    /* Read max fill size (only used with %d and %u) */
    if (my_isdigit(&my_charset_latin1, *fmt))
    {
      fmt= get_length(fmt, &length, &print_type);
      if (*fmt == '$')
      {
        to= process_args(cs, to, end, (fmt+1), length, ap);
        return (size_t) (to - start);
      }
    }
    else
    {
      if (*fmt == '`')
      {
        print_type|= ESCAPED_ARG;
        fmt++;
      }
      if (*fmt == '-')
        fmt++;
      if (*fmt == '*')
      {
        fmt++;
        length= va_arg(ap, int);
      }
      else
        fmt= get_length(fmt, &length, &print_type);
    }

    if (*fmt == '.')
    {
      uint unused_flags= 0;
      fmt++;
      if (*fmt == '*')
      {
        fmt++;
        width= va_arg(ap, int);
      }
      else
        fmt= get_length(fmt, &width, &unused_flags);
    }
    else
      width= MAX_WIDTH;

    fmt= check_longlong(fmt, &have_longlong);

    if (*fmt == 's' || *fmt == 'T')			/* String parameter */
    {
      reg2 char *par= va_arg(ap, char *);
      to= process_str_arg(cs, to, end, (longlong) length, width, par,
                          print_type, (*fmt == 'T'));
      continue;
    }
    else if (*fmt == 'b')				/* Buffer parameter */
    {
      char *par = va_arg(ap, char *);
      to= process_bin_arg(to, end, width, par);
      continue;
    }
    else if (*fmt == 'f' || *fmt == 'g')
    {
      double d;
#if __has_feature(memory_sanitizer)         /* QQ: MSAN has double trouble? */
      __msan_check_mem_is_initialized(ap, sizeof(double));
#endif
      d= va_arg(ap, double);
#if __has_feature(memory_sanitizer)        /* QQ: MSAN has double trouble? */
      __msan_unpoison(&d, sizeof(double));
#endif
      to= process_dbl_arg(to, end, width, d, *fmt);
      continue;
    }
    else if (*fmt == 'd' || *fmt == 'i' || *fmt == 'u' || *fmt == 'x' || 
             *fmt == 'X' || *fmt == 'p' || *fmt == 'o')
    {
      /* Integer parameter */
      longlong larg;

      if (have_longlong)
        larg = va_arg(ap,longlong);
      else if (*fmt == 'd' || *fmt == 'i')
        larg = va_arg(ap, int);
      else
        larg= va_arg(ap, uint);

      to= process_int_arg(to, end, length, larg, *fmt, print_type);
      continue;
    }
    else if (*fmt == 'c')                       /* Character parameter */
    {
      register int larg;
      if (to == end)
        break;
      larg = va_arg(ap, int);
      *to++= (char) larg;
      continue;
    }
    else if (*fmt == 'M')
    {
      int larg= va_arg(ap, int);
      const char *real_end= MY_MIN(to + width, end);

      to= process_int_arg(to, real_end, 0, larg, 'd', print_type);
      if (real_end - to >= 3)
      {
        char errmsg_buff[MYSYS_STRERROR_SIZE];
        *to++= ' ';
        *to++= '"';
        my_strerror(errmsg_buff, sizeof(errmsg_buff), (int) larg);
        to= process_str_arg(cs, to, real_end, 0, width, errmsg_buff,
                            print_type, 1);
        if (real_end > to) *to++= '"';
      }
      continue;
    }

    /* We come here on '%%', unknown code or too long parameter */
    if (to >= end)
      break;
    *to++='%';				/* % used as % or unknown code */
  }
  DBUG_ASSERT(to <= end);
  *to='\0';				/* End of errmessage */
  return (size_t) (to - start);
}


/*
  Limited snprintf() implementations

  exported to plugins as a service, see the detailed documentation
  around my_snprintf_service_st
*/

size_t my_vsnprintf(char *to, size_t n, const char* fmt, va_list ap)
{
  return my_vsnprintf_ex(&my_charset_latin1, to, n, fmt, ap);
}


size_t my_snprintf(char* to, size_t n, const char* fmt, ...)
{
  size_t result;
  va_list args;
  va_start(args,fmt);
  result= my_vsnprintf(to, n, fmt, args);
  va_end(args);
  return result;
}


/**
  Writes output to the stream according to a format string.

  @param stream     file to write to
  @param format     string format
  @param args       list of parameters

  @retval
    number of the characters written.
*/

int my_vfprintf(FILE *stream, const char* format, va_list args)
{
  char cvtbuf[1024];
  int alloc= 0;
  char *p= cvtbuf;
  size_t cur_len= sizeof(cvtbuf), actual;
  int ret;

  /*
    We do not know how much buffer we need.
    So start with a reasonably-sized stack-allocated buffer, and increase
    it exponentially until it is big enough.
  */
  for (;;)
  {
    size_t new_len;
    actual= my_vsnprintf(p, cur_len, format, args);
    if (actual < cur_len - 1)
      break;
    /*
      Not enough space (or just enough with nothing to spare - but we cannot
      distinguish this case from the return value). Allocate a bigger buffer
      and try again.
    */
    if (alloc)
      my_free(p);
    else
      alloc= 1;
    new_len= cur_len*2;
    if (new_len < cur_len)
      return 0;                                 /* Overflow */
    cur_len= new_len;
    p= my_malloc(PSI_INSTRUMENT_ME, cur_len, MYF(MY_FAE));
    if (!p)
      return 0;
  }
  ret= (int) actual;
  if (fputs(p, stream) < 0)
    ret= -1;
  if (alloc)
    my_free(p);
  return ret;
}

int my_fprintf(FILE *stream, const char* format, ...)
{
  int result;
  va_list args;
  va_start(args, format);
  result= my_vfprintf(stream, format, args);
  va_end(args);
  return result;
}


/*
  Return system error text for given error number

  @param buf        Buffer (of size MYSYS_STRERROR_SIZE)
  @param len        Length of buffer
  @param nr         Error number
*/

const char* my_strerror(char *buf, size_t len, int nr)
{
  char *msg= NULL;

  buf[0]= '\0';                                  /* failsafe */

  if (nr <= 0)
  {
    strmake(buf, (nr == 0 ?
                  "Internal error/check (Not system error)" :
                  "Internal error < 0 (Not system error)"),
            len-1);
    return buf;
  }

  /*
    These (handler-) error messages are shared by perror, as required
    by the principle of least surprise.
  */
  if ((nr >= HA_ERR_FIRST) && (nr <= HA_ERR_LAST))
  {
    msg= (char *) handler_error_messages[nr - HA_ERR_FIRST];
    strmake(buf, msg, len - 1);
  }
  else
  {
    /*
      On Windows, do things the Windows way. On a system that supports both
      the GNU and the XSI variant, use whichever was configured (GNU); if
      this choice is not advertised, use the default (POSIX/XSI).  Testing
      for __GNUC__ is not sufficient to determine whether this choice exists.
    */
#if defined(_WIN32)
    strerror_s(buf, len, nr);
#elif ((defined _POSIX_C_SOURCE && (_POSIX_C_SOURCE >= 200112L)) ||    \
       (defined _XOPEN_SOURCE   && (_XOPEN_SOURCE >= 600)))      &&    \
      ! defined _GNU_SOURCE
    strerror_r(nr, buf, len);             /* I can build with or without GNU */
#elif defined(__GLIBC__) && defined (_GNU_SOURCE)
    char *r= strerror_r(nr, buf, len);
    if (r != buf)                         /* Want to help, GNU? */
      strmake(buf, r, len - 1);           /* Then don't. */
#else
    strerror_r(nr, buf, len);
#endif
  }

  /*
    strerror() return values are implementation-dependent, so let's
    be pragmatic.
  */
  if (!buf[0])
    strmake(buf, "unknown error", len - 1);
  return buf;
}
