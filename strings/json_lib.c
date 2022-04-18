/* Copyright (c) 2016, 2020, MariaDB Corporation.

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

#include <my_global.h>
#include <string.h>
#include <m_ctype.h>
#include "json_lib.h"

/*
  JSON escaping lets user specify UTF16 codes of characters.
  So we're going to need the UTF16 charset capabilities. Let's import
  them from the utf16 charset.
*/
int my_utf16_uni(CHARSET_INFO *cs,
                 my_wc_t *pwc, const uchar *s, const uchar *e);
int my_uni_utf16(CHARSET_INFO *cs, my_wc_t wc, uchar *s, uchar *e);


void json_string_set_str(json_string_t *s,
                         const uchar *str, const uchar *end)
{
  s->c_str= str;
  s->str_end= end;
}


void json_string_set_cs(json_string_t *s, CHARSET_INFO *i_cs)
{
  s->cs= i_cs;
  s->error= 0;
  s->wc= i_cs->cset->mb_wc;
}


static void json_string_setup(json_string_t *s,
                              CHARSET_INFO *i_cs, const uchar *str,
                              const uchar *end)
{
  json_string_set_cs(s, i_cs);
  json_string_set_str(s, str, end);
}


enum json_char_classes {
  C_EOS,    /* end of string */
  C_LCURB,  /* {  */
  C_RCURB,  /* } */
  C_LSQRB,  /* [ */
  C_RSQRB,  /* ] */
  C_COLON,  /* : */
  C_COMMA,  /* , */
  C_QUOTE,  /* " */
  C_DIGIT,  /* -0123456789 */
  C_LOW_F,  /* 'f' (for "false") */
  C_LOW_N,  /* 'n' (for "null") */
  C_LOW_T,  /* 't' (for "true") */
  C_ETC,    /* everything else */
  C_ERR,    /* character disallowed in JSON */
  C_BAD,    /* invalid character, charset handler cannot read it */
  NR_C_CLASSES, /* Counter for classes that handled with functions. */
  C_SPACE   /* space. Doesn't need specific handlers, so after the counter.*/
};


/*
  This array maps first 128 Unicode Code Points into classes.
  The remaining Unicode characters should be mapped to C_ETC.
*/

static enum json_char_classes json_chr_map[128] = {
  C_ERR,   C_ERR,   C_ERR,   C_ERR,   C_ERR,   C_ERR,   C_ERR,   C_ERR,
  C_ERR,   C_SPACE, C_SPACE, C_ERR,   C_ERR,   C_SPACE, C_ERR,   C_ERR,
  C_ERR,   C_ERR,   C_ERR,   C_ERR,   C_ERR,   C_ERR,   C_ERR,   C_ERR,
  C_ERR,   C_ERR,   C_ERR,   C_ERR,   C_ERR,   C_ERR,   C_ERR,   C_ERR,

  C_SPACE, C_ETC,   C_QUOTE, C_ETC,   C_ETC,   C_ETC,   C_ETC,   C_ETC,
  C_ETC,   C_ETC,   C_ETC,   C_ETC,   C_COMMA, C_DIGIT, C_ETC,   C_ETC,
  C_DIGIT, C_DIGIT, C_DIGIT, C_DIGIT, C_DIGIT, C_DIGIT, C_DIGIT, C_DIGIT,
  C_DIGIT, C_DIGIT, C_COLON, C_ETC,   C_ETC,   C_ETC,   C_ETC,   C_ETC,

  C_ETC,   C_ETC,   C_ETC,   C_ETC,   C_ETC,   C_ETC,   C_ETC,   C_ETC,
  C_ETC,   C_ETC,   C_ETC,   C_ETC,   C_ETC,   C_ETC,   C_ETC,   C_ETC,
  C_ETC,   C_ETC,   C_ETC,   C_ETC,   C_ETC,   C_ETC,   C_ETC,   C_ETC,
  C_ETC,   C_ETC,   C_ETC,   C_LSQRB, C_ETC,   C_RSQRB, C_ETC,   C_ETC,

  C_ETC,   C_ETC,   C_ETC,   C_ETC,   C_ETC,   C_ETC,   C_LOW_F, C_ETC,
  C_ETC,   C_ETC,   C_ETC,   C_ETC,   C_ETC,   C_ETC,   C_LOW_N, C_ETC,
  C_ETC,   C_ETC,   C_ETC,   C_ETC,   C_LOW_T, C_ETC,   C_ETC,   C_ETC,
  C_ETC,   C_ETC,   C_ETC,   C_LCURB, C_ETC,   C_RCURB, C_ETC,   C_ETC
};


/*
  JSON parser actually has more states than the 'enum json_states'
  declares. But the rest of the states aren't seen to the user so let's
  specify them here to avoid confusion.
*/

enum json_all_states {
  JST_DONE= NR_JSON_USER_STATES,         /* ok to finish     */
  JST_OBJ_CONT= NR_JSON_USER_STATES+1,   /* object continues */
  JST_ARRAY_CONT= NR_JSON_USER_STATES+2, /* array continues  */
  JST_READ_VALUE= NR_JSON_USER_STATES+3, /* value is being read */
  NR_JSON_STATES= NR_JSON_USER_STATES+4
};


typedef int (*json_state_handler)(json_engine_t *);


/* The string is broken. */
static int unexpected_eos(json_engine_t *j)
{
  j->s.error= JE_EOS;
  return 1;
}


/* This symbol here breaks the JSON syntax. */
static int syntax_error(json_engine_t *j)
{
  j->s.error= JE_SYN;
  return 1;
}


/* Value of object. */
static int mark_object(json_engine_t *j)
{
  j->state= JST_OBJ_START;
  if (++j->stack_p < JSON_DEPTH_LIMIT)
  {
    j->stack[j->stack_p]= JST_OBJ_CONT;
    return 0;
  }
  j->s.error= JE_DEPTH;
  return 1;
}


/* Read value of object. */
static int read_obj(json_engine_t *j)
{
  j->state= JST_OBJ_START;
  j->value_type= JSON_VALUE_OBJECT;
  j->value= j->value_begin;
  if (++j->stack_p < JSON_DEPTH_LIMIT)
  {
    j->stack[j->stack_p]= JST_OBJ_CONT;
    return 0;
  }
  j->s.error= JE_DEPTH;
  return 1;
}


/* Value of array. */
static int mark_array(json_engine_t *j)
{
  j->state= JST_ARRAY_START;
  if (++j->stack_p < JSON_DEPTH_LIMIT)
  {
    j->stack[j->stack_p]= JST_ARRAY_CONT;
    j->value= j->value_begin;
    return 0;
  }
  j->s.error= JE_DEPTH;
  return 1;
}

/* Read value of object. */
static int read_array(json_engine_t *j)
{
  j->state= JST_ARRAY_START;
  j->value_type= JSON_VALUE_ARRAY;
  j->value= j->value_begin;
  if (++j->stack_p < JSON_DEPTH_LIMIT)
  {
    j->stack[j->stack_p]= JST_ARRAY_CONT;
    return 0;
  }
  j->s.error= JE_DEPTH;
  return 1;
}



/*
  Character classes inside the JSON string constant.
  We mostly need this to parse escaping properly.
  Escapings available in JSON are:
  \" - quotation mark
  \\ - backslash
  \b - backspace UNICODE 8
  \f - formfeed UNICODE 12
  \n - newline UNICODE 10
  \r - carriage return UNICODE 13
  \t - horizontal tab UNICODE 9
  \u{four-hex-digits} - code in UCS16 character set
*/
enum json_string_char_classes {
  S_0= 0,
  S_1= 1,
  S_2= 2,
  S_3= 3,
  S_4= 4,
  S_5= 5,
  S_6= 6,
  S_7= 7,
  S_8= 8,
  S_9= 9,
  S_A= 10,
  S_B= 11,
  S_C= 12,
  S_D= 13,
  S_E= 14,
  S_F= 15,
  S_ETC= 36,    /* rest of characters. */
  S_QUOTE= 37,
  S_BKSL= 38, /* \ */
  S_ERR= 100,   /* disallowed */
};


/* This maps characters to their types inside a string constant. */
static enum json_string_char_classes json_instr_chr_map[128] = {
  S_ERR,   S_ERR,   S_ERR,   S_ERR,   S_ERR,   S_ERR,   S_ERR,   S_ERR,
  S_ERR,   S_ERR,   S_ERR,   S_ERR,   S_ERR,   S_ERR,   S_ERR,   S_ERR,
  S_ERR,   S_ERR,   S_ERR,   S_ERR,   S_ERR,   S_ERR,   S_ERR,   S_ERR,
  S_ERR,   S_ERR,   S_ERR,   S_ERR,   S_ERR,   S_ERR,   S_ERR,   S_ERR,

  S_ETC,   S_ETC,   S_QUOTE, S_ETC,   S_ETC,   S_ETC,   S_ETC,   S_ETC,
  S_ETC,   S_ETC,   S_ETC,   S_ETC,   S_ETC,   S_ETC,   S_ETC,   S_ETC,
  S_0,     S_1,     S_2,     S_3,     S_4,     S_5,     S_6,     S_7,
  S_8,     S_9,     S_ETC,   S_ETC,   S_ETC,   S_ETC,   S_ETC,   S_ETC,

  S_ETC,   S_A,     S_B,     S_C,     S_D,     S_E,     S_F,     S_ETC,
  S_ETC,   S_ETC,   S_ETC,   S_ETC,   S_ETC,   S_ETC,   S_ETC,   S_ETC,
  S_ETC,   S_ETC,   S_ETC,   S_ETC,   S_ETC,   S_ETC,   S_ETC,   S_ETC,
  S_ETC,   S_ETC,   S_ETC,   S_ETC,   S_BKSL,  S_ETC,   S_ETC,   S_ETC,

  S_ETC,   S_A,     S_B,     S_C,     S_D,     S_E,     S_F,     S_ETC,
  S_ETC,   S_ETC,   S_ETC,   S_ETC,   S_ETC,   S_ETC,   S_ETC,   S_ETC,
  S_ETC,   S_ETC,   S_ETC,   S_ETC,   S_ETC,   S_ETC,   S_ETC,   S_ETC,
  S_ETC,   S_ETC,   S_ETC,   S_ETC,   S_ETC,   S_ETC,   S_ETC,   S_ETC
};


static int read_4_hexdigits(json_string_t *s, uchar *dest)
{
  int i, t, c_len;
  for (i=0; i<4; i++)
  {
    if ((c_len= json_next_char(s)) <= 0)
      return s->error= json_eos(s) ? JE_EOS : JE_BAD_CHR;

    if (s->c_next >= 128 || (t= json_instr_chr_map[s->c_next]) > S_F)
      return s->error= JE_SYN;

    s->c_str+= c_len;
    dest[i/2]+= (i % 2) ? t : t*16;
  }
  return 0;
}


static int json_handle_esc(json_string_t *s)
{
  int t, c_len;
  
  if ((c_len= json_next_char(s)) <= 0)
    return s->error= json_eos(s) ? JE_EOS : JE_BAD_CHR;

  s->c_str+= c_len;
  switch (s->c_next)
  {
    case 'b':
      s->c_next= 8;
      return 0;
    case 'f':
      s->c_next= 12;
      return 0;
    case 'n':
      s->c_next= 10;
      return 0;
    case 'r':
      s->c_next= 13;
      return 0;
    case 't':
      s->c_next= 9;
      return 0;
  }

  if (s->c_next < 128 && (t= json_instr_chr_map[s->c_next]) == S_ERR)
  {
    s->c_str-= c_len;
    return s->error= JE_ESCAPING;
  }


  if (s->c_next != 'u')
    return 0;

  {
    /*
      Read the four-hex-digits code.
      If symbol is not in the Basic Multilingual Plane, we're reading
      the string for the next four digits to compose the UTF-16 surrogate pair.
    */
    uchar code[4]= {0,0,0,0};

    if (read_4_hexdigits(s, code))
      return 1;

    if ((c_len= my_utf16_uni(0, &s->c_next, code, code+2)) == 2)
      return 0;

    if (c_len != MY_CS_TOOSMALL4)
      return s->error= JE_BAD_CHR;

    if ((c_len= json_next_char(s)) <= 0)
      return s->error= json_eos(s) ? JE_EOS : JE_BAD_CHR;
    if (s->c_next != '\\')
      return s->error= JE_SYN;

    s->c_str+= c_len;
    if ((c_len= json_next_char(s)) <= 0)
      return s->error= json_eos(s) ? JE_EOS : JE_BAD_CHR;
    if (s->c_next != 'u')
      return s->error= JE_SYN;
    s->c_str+= c_len;

    if (read_4_hexdigits(s, code+2))
      return 1;

    if ((c_len= my_utf16_uni(0, &s->c_next, code, code+4)) == 4)
      return 0;
  }
  return s->error= JE_BAD_CHR;
}


int json_read_string_const_chr(json_string_t *js)
{
  int c_len;

  if ((c_len= json_next_char(js)) > 0)
  {
    js->c_str+= c_len;
    return (js->c_next == '\\') ? json_handle_esc(js) : 0;
  }
  js->error= json_eos(js) ? JE_EOS : JE_BAD_CHR; 
  return 1;
}


static int skip_str_constant(json_engine_t *j)
{
  int t, c_len;
  for (;;)
  {
    if ((c_len= json_next_char(&j->s)) > 0)
    {
      j->s.c_str+= c_len;
      if (j->s.c_next >= 128 || ((t=json_instr_chr_map[j->s.c_next]) <= S_ETC))
        continue;

      if (j->s.c_next == '"')
        break;
      if (j->s.c_next == '\\')
      {
        j->value_escaped= 1;
        if (json_handle_esc(&j->s))
          return 1;
        continue;
      }
      /* Symbol not allowed in JSON. */
      return j->s.error= JE_NOT_JSON_CHR;
    }
    else
      return j->s.error= json_eos(&j->s) ? JE_EOS : JE_BAD_CHR; 
  }

  j->state= j->stack[j->stack_p];
  return 0;
}


/* Scalar string. */
static int v_string(json_engine_t *j)
{
  return skip_str_constant(j) || json_scan_next(j);
}


/* Read scalar string. */
static int read_strn(json_engine_t *j)
{
  j->value= j->s.c_str;
  j->value_type= JSON_VALUE_STRING;
  j->value_escaped= 0;

  if (skip_str_constant(j))
    return 1;

  j->state= j->stack[j->stack_p];
  j->value_len= (int)(j->s.c_str - j->value) - 1;
  return 0;
}


/*
  We have dedicated parser for numeric constants. It's similar
  to the main JSON parser, we similarly define character classes,
  map characters to classes and implement the state-per-class
  table. Though we don't create functions that handle
  particular classes, just specify what new state should parser
  get in this case.
*/
enum json_num_char_classes {
  N_MINUS,
  N_PLUS,
  N_ZERO,
  N_DIGIT,
  N_POINT,
  N_E,
  N_END,
  N_EEND,
  N_ERR,
  N_NUM_CLASSES
};


static enum json_num_char_classes json_num_chr_map[128] = {
  N_ERR,   N_ERR,   N_ERR,   N_ERR,   N_ERR,   N_ERR,   N_ERR,   N_ERR,
  N_ERR,   N_END,   N_END,   N_ERR,   N_ERR,   N_END,   N_ERR,   N_ERR,
  N_ERR,   N_ERR,   N_ERR,   N_ERR,   N_ERR,   N_ERR,   N_ERR,   N_ERR,
  N_ERR,   N_ERR,   N_ERR,   N_ERR,   N_ERR,   N_ERR,   N_ERR,   N_ERR,

  N_END,   N_EEND,  N_EEND,  N_EEND,  N_EEND,  N_EEND,  N_EEND,  N_EEND,
  N_EEND,  N_EEND,  N_EEND,  N_PLUS,  N_END,   N_MINUS, N_POINT, N_EEND,
  N_ZERO,  N_DIGIT, N_DIGIT, N_DIGIT, N_DIGIT, N_DIGIT, N_DIGIT, N_DIGIT,
  N_DIGIT, N_DIGIT, N_EEND,  N_EEND,  N_EEND,  N_EEND,  N_EEND,  N_EEND,

  N_EEND,  N_EEND,  N_EEND,  N_EEND,  N_EEND,  N_E,     N_EEND,  N_EEND,
  N_EEND,  N_EEND,  N_EEND,  N_EEND,  N_EEND,  N_EEND,  N_EEND,  N_EEND,
  N_EEND,  N_EEND,  N_EEND,  N_EEND,  N_EEND,  N_EEND,  N_EEND,  N_EEND,
  N_EEND,  N_EEND,  N_EEND,  N_EEND,  N_EEND,  N_END,   N_EEND,  N_EEND,

  N_EEND,  N_EEND,  N_EEND,  N_EEND,  N_EEND,  N_E,     N_EEND,  N_EEND,
  N_EEND,  N_EEND,  N_EEND,  N_EEND,  N_EEND,  N_EEND,  N_EEND,  N_EEND,
  N_EEND,  N_EEND,  N_EEND,  N_EEND,  N_EEND,  N_EEND,  N_EEND,  N_EEND,
  N_EEND,  N_EEND,  N_EEND,  N_EEND,  N_EEND,   N_END,   N_EEND,  N_EEND,
};


enum json_num_states {
  NS_OK,  /* Number ended. */
  NS_GO,  /* Initial state. */
  NS_GO1, /* If the number starts with '-'. */
  NS_Z,   /* If the number starts with '0'. */
  NS_Z1,  /* If the numbers starts with '-0'. */
  NS_INT, /* Integer part. */
  NS_FRAC,/* Fractional part. */
  NS_EX,  /* Exponential part begins. */
  NS_EX1, /* Exponential part continues. */
  NS_NUM_STATES
};


static int json_num_states[NS_NUM_STATES][N_NUM_CLASSES]=
{
/*         -        +       0        1..9    POINT    E       END_OK   ERROR */
/*OK*/   { JE_SYN,  JE_SYN, JE_SYN,   JE_SYN, JE_SYN,  JE_SYN, JE_SYN, JE_BAD_CHR },
/*GO*/   { NS_GO1,  JE_SYN, NS_Z,     NS_INT, JE_SYN,  JE_SYN, JE_SYN, JE_BAD_CHR },
/*GO1*/  { JE_SYN,  JE_SYN, NS_Z1,    NS_INT, JE_SYN,  JE_SYN, JE_SYN, JE_BAD_CHR },
/*ZERO*/ { JE_SYN,  JE_SYN, JE_SYN,   JE_SYN, NS_FRAC, JE_SYN, NS_OK,  JE_BAD_CHR },
/*ZE1*/  { JE_SYN,  JE_SYN, JE_SYN,   JE_SYN, NS_FRAC, JE_SYN, NS_OK,  JE_BAD_CHR },
/*INT*/  { JE_SYN,  JE_SYN, NS_INT,   NS_INT, NS_FRAC, NS_EX,  NS_OK,  JE_BAD_CHR },
/*FRAC*/ { JE_SYN,  JE_SYN, NS_FRAC,  NS_FRAC,JE_SYN,  NS_EX,  NS_OK,  JE_BAD_CHR },
/*EX*/   { NS_EX,   NS_EX,  NS_EX1,   NS_EX1, JE_SYN,  JE_SYN, JE_SYN, JE_BAD_CHR }, 
/*EX1*/  { JE_SYN,  JE_SYN, NS_EX1,   NS_EX1, JE_SYN,  JE_SYN, NS_OK,  JE_BAD_CHR }
};


static uint json_num_state_flags[NS_NUM_STATES]=
{
/*OK*/   0,
/*GO*/   0,
/*GO1*/  JSON_NUM_NEG,
/*ZERO*/ 0,
/*ZE1*/  0,
/*INT*/  0,
/*FRAC*/ JSON_NUM_FRAC_PART,
/*EX*/   JSON_NUM_EXP,
/*EX1*/  0,
};


static int skip_num_constant(json_engine_t *j)
{
  int state= json_num_states[NS_GO][json_num_chr_map[j->s.c_next]];
  int c_len;

  j->num_flags= 0;
  for (;;)
  {
    j->num_flags|= json_num_state_flags[state];
    if ((c_len= json_next_char(&j->s)) > 0 && j->s.c_next < 128)
    {
      if ((state= json_num_states[state][json_num_chr_map[j->s.c_next]]) > 0)
      {
        j->s.c_str+= c_len;
        continue;
      }
      break;
    }

    if ((j->s.error=
          json_eos(&j->s) ? json_num_states[state][N_END] : JE_BAD_CHR) < 0)
      return 1;
    else
      break;
  }

  j->state= j->stack[j->stack_p];
  return 0;
}


/* Scalar numeric. */
static int v_number(json_engine_t *j)
{
  return skip_num_constant(j) || json_scan_next(j);
}


/* Read numeric constant. */
static int read_num(json_engine_t *j)
{
  j->value= j->value_begin;
  if (skip_num_constant(j) == 0)
  {
    j->value_type= JSON_VALUE_NUMBER;
    j->value_len= (int)(j->s.c_str - j->value_begin);
    return 0;
  }
  return 1;
}


/* Check that the JSON string matches the argument and skip it. */
static int skip_string_verbatim(json_string_t *s, const char *str)
{
  int c_len;
  while (*str)
  {
    if ((c_len= json_next_char(s)) > 0)
    {
      if (s->c_next == (my_wc_t) *(str++))
      {
        s->c_str+= c_len;
        continue;
      }
      return s->error= JE_SYN;
    }
    return s->error= json_eos(s) ? JE_EOS : JE_BAD_CHR; 
  }

  return 0;
}


/* Scalar false. */
static int v_false(json_engine_t *j)
{
  if (skip_string_verbatim(&j->s, "alse"))
   return 1;
  j->state= j->stack[j->stack_p];
  return json_scan_next(j);
}


/* Scalar null. */
static int v_null(json_engine_t *j)
{
  if (skip_string_verbatim(&j->s, "ull"))
   return 1;
  j->state= j->stack[j->stack_p];
  return json_scan_next(j);
}


/* Scalar true. */
static int v_true(json_engine_t *j)
{
  if (skip_string_verbatim(&j->s, "rue"))
   return 1;
  j->state= j->stack[j->stack_p];
  return json_scan_next(j);
}


/* Read false. */
static int read_false(json_engine_t *j)
{
  j->value_type= JSON_VALUE_FALSE;
  j->value= j->value_begin;
  j->state= j->stack[j->stack_p];
  j->value_len= 5;
  return skip_string_verbatim(&j->s, "alse");
}


/* Read null. */
static int read_null(json_engine_t *j)
{
  j->value_type= JSON_VALUE_NULL;
  j->value= j->value_begin;
  j->state= j->stack[j->stack_p];
  j->value_len= 4;
  return skip_string_verbatim(&j->s, "ull");
}


/* Read true. */
static int read_true(json_engine_t *j)
{
  j->value_type= JSON_VALUE_TRUE;
  j->value= j->value_begin;
  j->state= j->stack[j->stack_p];
  j->value_len= 4;
  return skip_string_verbatim(&j->s, "rue");
}


/* Disallowed character. */
static int not_json_chr(json_engine_t *j)
{
  j->s.error= JE_NOT_JSON_CHR;
  return 1;
}


/* Bad character. */
static int bad_chr(json_engine_t *j)
{
  j->s.error= JE_BAD_CHR;
  return 1;
}


/* Correct finish. */
static int done(json_engine_t *j  __attribute__((unused)))
{
  return 1;
}


/* End of the object. */
static int end_object(json_engine_t *j)
{
  j->stack_p--;
  j->state= JST_OBJ_END;
  return 0;
}


/* End of the array. */
static int end_array(json_engine_t *j)
{
  j->stack_p--;
  j->state= JST_ARRAY_END;
  return 0;
}


/* Start reading key name. */
static int read_keyname(json_engine_t *j)
{
  j->state= JST_KEY;
  return 0;
}


static void get_first_nonspace(json_string_t *js, int *t_next, int *c_len)
{
  do
  {
    if ((*c_len= json_next_char(js)) <= 0)
      *t_next= json_eos(js) ? C_EOS : C_BAD;
    else
    {
      *t_next= (js->c_next < 128) ? json_chr_map[js->c_next] : C_ETC;
      js->c_str+= *c_len;
    }
  } while (*t_next == C_SPACE);
}


/* Next key name. */
static int next_key(json_engine_t *j)
{
  int t_next, c_len;
  get_first_nonspace(&j->s, &t_next, &c_len);

  if (t_next == C_QUOTE)
  {
    j->state= JST_KEY;
    return 0;
  }

  j->s.error= (t_next == C_EOS)  ? JE_EOS :
              ((t_next == C_BAD) ? JE_BAD_CHR :
                                   JE_SYN);
  return 1;
}


/* Forward declarations. */
static int skip_colon(json_engine_t *j);
static int skip_key(json_engine_t *j);
static int struct_end_cb(json_engine_t *j);
static int struct_end_qb(json_engine_t *j);
static int struct_end_cm(json_engine_t *j);
static int struct_end_eos(json_engine_t *j);


static int next_item(json_engine_t *j)
{
  j->state= JST_VALUE;
  return 0;
}


static int array_item(json_engine_t *j)
{
  j->state= JST_VALUE;
  j->s.c_str-= j->sav_c_len;
  return 0;
}


static json_state_handler json_actions[NR_JSON_STATES][NR_C_CLASSES]=
/*
   EOS              {            }             [             ]
   :                ,            "             -0..9         f
   n                t              ETC          ERR           BAD
*/
{
  {/*VALUE*/
    unexpected_eos, mark_object, syntax_error, mark_array,   syntax_error,
    syntax_error,   syntax_error,v_string,     v_number,     v_false,
    v_null,         v_true,       syntax_error, not_json_chr, bad_chr},
  {/*KEY*/
    unexpected_eos, skip_key,    skip_key,     skip_key,     skip_key,
    skip_key,       skip_key,    skip_colon,   skip_key,     skip_key,
    skip_key,       skip_key,     skip_key,     not_json_chr, bad_chr},
  {/*OBJ_START*/
    unexpected_eos, syntax_error, end_object,  syntax_error, syntax_error,
    syntax_error,   syntax_error, read_keyname, syntax_error, syntax_error,
    syntax_error,   syntax_error,   syntax_error,    not_json_chr, bad_chr},
  {/*OBJ_END*/
    struct_end_eos, syntax_error, struct_end_cb, syntax_error, struct_end_qb,
    syntax_error,   struct_end_cm,syntax_error,  syntax_error, syntax_error,
    syntax_error,   syntax_error,  syntax_error,    not_json_chr, bad_chr},
  {/*ARRAY_START*/
    unexpected_eos, array_item,   syntax_error, array_item,   end_array,
    syntax_error,   syntax_error, array_item,  array_item,  array_item,
    array_item,    array_item,    syntax_error,    not_json_chr, bad_chr},
  {/*ARRAY_END*/
    struct_end_eos, syntax_error, struct_end_cb, syntax_error,  struct_end_qb,
    syntax_error,   struct_end_cm, syntax_error, syntax_error,  syntax_error,
    syntax_error,   syntax_error,  syntax_error,    not_json_chr, bad_chr},
  {/*DONE*/
    done,           syntax_error, syntax_error, syntax_error, syntax_error,
    syntax_error,   syntax_error, syntax_error, syntax_error, syntax_error,
    syntax_error,   syntax_error, syntax_error, not_json_chr, bad_chr},
  {/*OBJ_CONT*/
    unexpected_eos, syntax_error, end_object,    syntax_error,   syntax_error,
    syntax_error,   next_key,     syntax_error,  syntax_error,   syntax_error,
    syntax_error,    syntax_error,    syntax_error,    not_json_chr, bad_chr},
  {/*ARRAY_CONT*/
    unexpected_eos, syntax_error, syntax_error,  syntax_error, end_array,
    syntax_error,   next_item,    syntax_error,  syntax_error, syntax_error,
    syntax_error,    syntax_error,    syntax_error,    not_json_chr, bad_chr},
  {/*READ_VALUE*/
    unexpected_eos, read_obj,     syntax_error,  read_array,    syntax_error,
    syntax_error,   syntax_error, read_strn,     read_num,      read_false,
    read_null,      read_true,    syntax_error,    not_json_chr, bad_chr},
};



int json_scan_start(json_engine_t *je,
                    CHARSET_INFO *i_cs, const uchar *str, const uchar *end)
{
  static const uchar no_time_to_die= 0;

  json_string_setup(&je->s, i_cs, str, end);
  je->stack[0]= JST_DONE;
  je->stack_p= 0;
  je->state= JST_VALUE;
  je->killed_ptr = (uchar*)&no_time_to_die;
  return 0;
}


/* Skip colon and the value. */
static int skip_colon(json_engine_t *j)
{
  int t_next, c_len;

  get_first_nonspace(&j->s, &t_next, &c_len);

  if (t_next == C_COLON)
  {
    get_first_nonspace(&j->s, &t_next, &c_len);
    return json_actions[JST_VALUE][t_next](j);
 }

  j->s.error= (t_next == C_EOS)  ? JE_EOS :
              ((t_next == C_BAD) ? JE_BAD_CHR:
                                   JE_SYN);

  return 1;
}


/* Skip colon and the value. */
static int skip_key(json_engine_t *j)
{
  int t_next, c_len;

  if (j->s.c_next< 128 && json_instr_chr_map[j->s.c_next] == S_BKSL &&
      json_handle_esc(&j->s))
    return 1;

  while (json_read_keyname_chr(j) == 0) {}

  if (j->s.error)
    return 1;

  get_first_nonspace(&j->s, &t_next, &c_len);
  return json_actions[JST_VALUE][t_next](j);
}


/*
  Handle EOS after the end of an object or array.
  To do that we should pop the stack to see if
  we are inside an object, or an array, and
  run our 'state machine' accordingly.
*/
static int struct_end_eos(json_engine_t *j)
{ return json_actions[j->stack[j->stack_p]][C_EOS](j); }


/*
  Handle '}' after the end of an object or array.
  To do that we should pop the stack to see if
  we are inside an object, or an array, and
  run our 'state machine' accordingly.
*/
static int struct_end_cb(json_engine_t *j)
{ return json_actions[j->stack[j->stack_p]][C_RCURB](j); }


/*
  Handle ']' after the end of an object or array.
  To do that we should pop the stack to see if
  we are inside an object, or an array, and
  run our 'state machine' accordingly.
*/
static int struct_end_qb(json_engine_t *j)
{ return json_actions[j->stack[j->stack_p]][C_RSQRB](j); }


/*
  Handle ',' after the end of an object or array.
  To do that we should pop the stack to see if
  we are inside an object, or an array, and
  run our 'state machine' accordingly.
*/
static int struct_end_cm(json_engine_t *j)
{ return json_actions[j->stack[j->stack_p]][C_COMMA](j); }


int json_read_keyname_chr(json_engine_t *j)
{
  int c_len, t;

  if ((c_len= json_next_char(&j->s)) > 0)
  {
    j->s.c_str+= c_len;
    if (j->s.c_next>= 128 || (t= json_instr_chr_map[j->s.c_next]) <= S_ETC)
      return 0;

    switch (t)
    {
    case S_QUOTE:
      for (;;)  /* Skip spaces until ':'. */
      {
        if ((c_len= json_next_char(&j->s)) > 0)
        {
          if (j->s.c_next == ':')
          {
            j->s.c_str+= c_len;
            j->state= JST_VALUE;
            return 1;
          }

          if (j->s.c_next < 128 && json_chr_map[j->s.c_next] == C_SPACE)
          {
            j->s.c_str+= c_len;
            continue;
          }
          j->s.error= JE_SYN;
          break;
        }
        j->s.error= json_eos(&j->s) ? JE_EOS : JE_BAD_CHR;
        break;
      }
      return 1;
    case S_BKSL:
      return json_handle_esc(&j->s);
    case S_ERR:
      j->s.c_str-= c_len;
      j->s.error= JE_STRING_CONST;
      return 1;
    }
  }
  j->s.error= json_eos(&j->s) ? JE_EOS : JE_BAD_CHR; 
  return 1;
}


int json_read_value(json_engine_t *j)
{
  int t_next, c_len, res;

  j->value_type= JSON_VALUE_UNINITIALIZED;
  if (j->state == JST_KEY)
  {
    while (json_read_keyname_chr(j) == 0) {}

    if (j->s.error)
      return 1;
  }

  get_first_nonspace(&j->s, &t_next, &c_len);

  j->value_begin= j->s.c_str-c_len;
  res= json_actions[JST_READ_VALUE][t_next](j);
  j->value_end= j->s.c_str;
  return res;
}


int json_scan_next(json_engine_t *j)
{
  int t_next;

  get_first_nonspace(&j->s, &t_next, &j->sav_c_len);
  return *j->killed_ptr || json_actions[j->state][t_next](j);
}


enum json_path_chr_classes {
  P_EOS,    /* end of string */
  P_USD,    /* $ */
  P_ASTER,  /* * */
  P_LSQRB,  /* [ */
  P_T,      /* t (for to) */
  P_RSQRB,  /* ] */
  P_POINT,  /* . */
  P_NEG,    /* hyphen (for negative index in path) */
  P_ZERO,   /* 0 */
  P_DIGIT,  /* 123456789 */
  P_L,      /* l (for "lax") */
  P_S,      /* s (for "strict") */
  P_SPACE,  /* space */
  P_BKSL,   /* \ */
  P_QUOTE,  /* " */
  P_ETC,    /* everything else */
  P_ERR,    /* character disallowed in JSON*/
  P_BAD,    /* invalid character */
  N_PATH_CLASSES,
};


static enum json_path_chr_classes json_path_chr_map[128] = {
  P_ERR,   P_ERR,   P_ERR,   P_ERR,   P_ERR,   P_ERR,   P_ERR,   P_ERR,
  P_ERR,   P_SPACE, P_SPACE, P_ERR,   P_ERR,   P_SPACE, P_ERR,   P_ERR,
  P_ERR,   P_ERR,   P_ERR,   P_ERR,   P_ERR,   P_ERR,   P_ERR,   P_ERR,
  P_ERR,   P_ERR,   P_ERR,   P_ERR,   P_ERR,   P_ERR,   P_ERR,   P_ERR,

  P_SPACE, P_ETC,   P_QUOTE, P_ETC,   P_USD,   P_ETC,   P_ETC,   P_ETC,
  P_ETC,   P_ETC,   P_ASTER, P_ETC,   P_ETC,   P_NEG,   P_POINT, P_ETC,
  P_ZERO,  P_DIGIT, P_DIGIT, P_DIGIT, P_DIGIT, P_DIGIT, P_DIGIT, P_DIGIT,
  P_DIGIT, P_DIGIT, P_ETC,   P_ETC,   P_ETC,   P_ETC,   P_ETC,   P_ETC,

  P_ETC,   P_ETC,   P_ETC,   P_ETC,   P_ETC,   P_ETC,   P_ETC,   P_ETC,
  P_ETC,   P_ETC,   P_ETC,   P_ETC,   P_L,     P_ETC,   P_ETC,   P_ETC,
  P_ETC,   P_ETC,   P_S,     P_ETC,   P_T,   P_ETC,   P_ETC,   P_ETC,
  P_ETC,   P_ETC,   P_ETC,   P_LSQRB, P_BKSL, P_RSQRB, P_ETC,   P_ETC,

  P_ETC,   P_ETC,   P_ETC,   P_ETC,   P_ETC,   P_ETC,   P_ETC,   P_ETC,
  P_ETC,   P_ETC,   P_ETC,   P_ETC,   P_L,     P_ETC,   P_ETC,   P_ETC,
  P_ETC,   P_ETC,   P_S,     P_ETC,   P_T,   P_ETC,   P_ETC,   P_ETC,
  P_ETC,   P_ETC,   P_ETC,   P_ETC,   P_ETC,   P_ETC,   P_ETC,   P_ETC
};


enum json_path_states {
  PS_GO,  /* Initial state. */
  PS_LAX, /* Parse the 'lax' keyword. */
  PS_PT,  /* New path's step begins. */
  PS_AR,  /* Parse array step. */
  PS_SAR, /* space after the '['. */
  PS_AWD, /* Array wildcard. */
  PS_NEG,  /*  Parse '-' (hyphen) */
  PS_Z,   /* '0' (as an array item number). */
  PS_INT, /* Parse integer (as an array item number). */
  PS_AS,  /* Space. */
  PS_KEY, /* Key. */
  PS_KNM, /* Parse key name. */
  PS_KWD, /* Key wildcard. */
  PS_AST, /* Asterisk. */
  PS_DWD, /* Double wildcard. */
  PS_KEYX, /* Key started with quote ("). */
  PS_KNMX, /* Parse quoted key name. */
  PS_LAST, /* Parse 'last' keyword */
  PS_T,    /* Parse 'to' keyword. */
  N_PATH_STATES, /* Below are states that aren't in the transitions table. */
  PS_SCT,  /* Parse the 'strict' keyword. */
  PS_EKY,  /* '.' after the keyname so next step is the key. */
  PS_EKYX, /* Closing " for the quoted keyname. */
  PS_EAR,  /* '[' after the keyname so next step is the array. */
  PS_ESC,  /* Escaping in the keyname. */
  PS_ESCX, /* Escaping in the quoted keyname. */
  PS_OK,   /* Path normally ended. */
  PS_KOK   /* EOS after the keyname so end the path normally. */
};


static int json_path_transitions[N_PATH_STATES][N_PATH_CLASSES]=
{
/*
            EOS       $,      *       [       to       ]       .       -
            0       1..9    L       S       SPACE   \       "       ETC
            ERR              BAD
*/
/* GO  */ { JE_EOS, PS_PT, JE_SYN, JE_SYN, JE_SYN, JE_SYN, JE_SYN, JE_SYN,
            JE_SYN, JE_SYN, PS_LAX, PS_SCT, PS_GO, JE_SYN, JE_SYN, JE_SYN,
            JE_NOT_JSON_CHR, JE_BAD_CHR},
/* LAX */ { JE_EOS, JE_SYN, JE_SYN, JE_SYN, JE_SYN, JE_SYN, JE_SYN, JE_SYN,
            JE_SYN, JE_SYN, PS_LAX, JE_SYN, PS_GO, JE_SYN, JE_SYN, JE_SYN,
            JE_NOT_JSON_CHR, JE_BAD_CHR},
/* PT */  { PS_OK, JE_SYN, PS_AST, PS_AR, JE_SYN, JE_SYN, PS_KEY, JE_SYN,
            JE_SYN, JE_SYN, JE_SYN, JE_SYN, JE_SYN, JE_SYN, JE_SYN, JE_SYN,
            JE_NOT_JSON_CHR, JE_BAD_CHR},
/* AR */  { JE_EOS, JE_SYN, PS_AWD, JE_SYN, JE_SYN, JE_SYN, JE_SYN, PS_NEG,
            PS_Z, PS_INT, PS_LAST, JE_SYN, PS_SAR, JE_SYN, JE_SYN, JE_SYN,
            JE_NOT_JSON_CHR, JE_BAD_CHR},
/* SAR */ { JE_EOS, JE_SYN, PS_AWD, JE_SYN, JE_SYN, PS_PT, JE_SYN, JE_SYN,
            PS_Z, PS_INT, PS_LAST, JE_SYN, PS_SAR, JE_SYN, JE_SYN, JE_SYN,
            JE_NOT_JSON_CHR, JE_BAD_CHR},
/* AWD */ { JE_EOS, JE_SYN, JE_SYN, JE_SYN, JE_SYN, PS_PT, JE_SYN, JE_SYN,
            JE_SYN, JE_SYN, JE_SYN, JE_SYN, PS_AS, JE_SYN, JE_SYN, JE_SYN,
            JE_NOT_JSON_CHR, JE_BAD_CHR},
/* NEG */ { JE_EOS, JE_SYN, JE_SYN, JE_SYN, JE_SYN, JE_SYN, JE_SYN, JE_SYN,
            PS_INT, PS_INT, JE_SYN, JE_SYN, JE_SYN, JE_SYN, JE_SYN, JE_SYN,
            JE_NOT_JSON_CHR, JE_BAD_CHR},
/* Z */   { JE_EOS, JE_SYN, JE_SYN, JE_SYN, JE_SYN, PS_PT, JE_SYN, JE_SYN,
            JE_SYN, JE_SYN, JE_SYN, JE_SYN, PS_AS, JE_SYN, JE_SYN, JE_SYN,
            JE_NOT_JSON_CHR, JE_BAD_CHR},
/* INT */ { JE_EOS, JE_SYN, JE_SYN, JE_SYN, JE_SYN, PS_PT, JE_SYN, JE_SYN,
            PS_INT, PS_INT, JE_SYN, JE_SYN, PS_AS, JE_SYN, JE_SYN, JE_SYN,
            JE_NOT_JSON_CHR, JE_BAD_CHR},
/* AS */  { JE_EOS, JE_SYN, JE_SYN, JE_SYN, PS_T, PS_PT, JE_SYN, PS_NEG,
            PS_Z, PS_INT, PS_LAST, PS_AS, JE_SYN, JE_SYN, JE_SYN,
            JE_NOT_JSON_CHR, JE_BAD_CHR},
/* KEY */ { JE_EOS, PS_KNM, PS_KWD, JE_SYN, PS_KNM, PS_KNM, JE_SYN, JE_SYN,
            PS_KNM, PS_KNM, PS_KNM, PS_KNM, PS_KNM, JE_SYN, PS_KEYX, PS_KNM,
            JE_NOT_JSON_CHR, JE_BAD_CHR},
/* KNM */ { PS_KOK, PS_KNM, PS_AST, PS_EAR, PS_KNM, PS_KNM, PS_EKY, JE_SYN,
            PS_KNM, PS_KNM, PS_KNM, PS_KNM, PS_KNM, PS_ESC, PS_KNM, PS_KNM,
            JE_NOT_JSON_CHR, JE_BAD_CHR},
/* KWD */ { PS_OK, JE_SYN, JE_SYN, PS_AR, JE_SYN, JE_SYN, PS_EKY, JE_SYN,
            JE_SYN, JE_SYN, JE_SYN, JE_SYN, JE_SYN, JE_SYN, JE_SYN, JE_SYN,
            JE_NOT_JSON_CHR, JE_BAD_CHR},
/* AST */ { JE_SYN, JE_SYN, PS_DWD, JE_SYN, JE_SYN, JE_SYN, JE_SYN, JE_SYN,
            JE_SYN, JE_SYN, JE_SYN, JE_SYN, JE_SYN, JE_SYN, JE_SYN, JE_SYN,
            JE_NOT_JSON_CHR, JE_BAD_CHR},
/* DWD */ { JE_SYN, JE_SYN, PS_AST, PS_AR, JE_SYN, JE_SYN, PS_KEY, JE_SYN,
            JE_SYN, JE_SYN, JE_SYN, JE_SYN, JE_SYN, JE_SYN, JE_SYN, JE_SYN,
            JE_NOT_JSON_CHR, JE_BAD_CHR},
/* KEYX*/ { JE_EOS, PS_KNMX, PS_KNMX, PS_KNMX, PS_KNMX, PS_KNMX, PS_KNMX, JE_SYN,
            PS_KNMX, PS_KNMX, PS_KNMX, PS_KNMX, PS_KNMX, PS_ESCX, PS_EKYX, PS_KNMX,
            JE_NOT_JSON_CHR, JE_BAD_CHR},
/* KNMX */{ JE_EOS, PS_KNMX, PS_KNMX, PS_KNMX, PS_KNMX, PS_KNMX, PS_KNMX, JE_SYN,
            PS_KNMX, PS_KNMX, PS_KNMX, PS_KNMX, PS_KNMX, PS_ESCX, PS_EKYX, PS_KNMX,
            JE_NOT_JSON_CHR, JE_BAD_CHR},
/* LAST */{ JE_SYN, JE_SYN, JE_SYN, JE_SYN, JE_SYN, PS_PT, JE_SYN, PS_NEG,
            JE_SYN, JE_SYN, JE_SYN, JE_SYN, PS_AS, JE_SYN, JE_SYN, JE_SYN,
            JE_SYN, JE_BAD_CHR},
/* T */   { JE_SYN, JE_SYN, JE_SYN, JE_SYN, JE_SYN, PS_PT, JE_SYN, PS_NEG,
            JE_SYN, JE_SYN, JE_SYN, JE_SYN, PS_AS, JE_SYN, JE_SYN, JE_SYN,
            JE_SYN, JE_BAD_CHR},
};


int json_path_setup(json_path_t *p,
                    CHARSET_INFO *i_cs, const uchar *str, const uchar *end)
{
 int c_len, t_next, state= PS_GO, is_negative_index= 0, is_last= 0,
  prev_value=0, is_to= 0, *cur_val;
  enum json_path_step_types double_wildcard= JSON_PATH_KEY_NULL;
  json_string_setup(&p->s, i_cs, str, end);

  p->steps[0].type= JSON_PATH_ARRAY_WILD;
  p->last_step= p->steps;
  p->mode_strict= FALSE;
  p->types_used= JSON_PATH_KEY_NULL;

  do
  {
    if ((c_len= json_next_char(&p->s)) <= 0)
      t_next= json_eos(&p->s) ? P_EOS : P_BAD;
    else
      t_next= (p->s.c_next >= 128) ? P_ETC : json_path_chr_map[p->s.c_next];

    if ((state= json_path_transitions[state][t_next]) < 0)
      return p->s.error= state;

    p->s.c_str+= c_len;

    switch (state)
    {
    case PS_LAX:
      if ((p->s.error= skip_string_verbatim(&p->s, "ax")))
        return 1;
      p->mode_strict= FALSE;
      continue;
    case PS_SCT:
      if ((p->s.error= skip_string_verbatim(&p->s, "rict")))
        return 1;
      p->mode_strict= TRUE;
      state= PS_LAX;
      continue;
    case PS_KWD:
    case PS_AWD:
      p->last_step->type|= JSON_PATH_WILD;
      p->types_used|= JSON_PATH_WILD;
      continue;
    case PS_INT:
      cur_val= is_to ? &(p->last_step->n_item_end) :
                       &(p->last_step->n_item);
      if (is_last)
      {
        prev_value*= 10;
        prev_value-= p->s.c_next - '0';
        *cur_val= -1 + prev_value;
      }
      else
      {
        (*cur_val)*= 10;
        if (is_negative_index)
          *cur_val-= p->s.c_next - '0';
        else
          *cur_val+= p->s.c_next - '0';
      }
      continue;
    case PS_EKYX:
      p->last_step->key_end= p->s.c_str - c_len;
      state= PS_PT;
      continue;
    case PS_EKY:
      p->last_step->key_end= p->s.c_str - c_len;
      state= PS_KEY;
      /* fall through */
    case PS_KEY:
      p->last_step++;
      is_to= 0;
      prev_value= 0;
      is_negative_index= 0;
      is_last= 0;
      if (p->last_step - p->steps >= JSON_DEPTH_LIMIT)
        return p->s.error= JE_DEPTH;
      p->types_used|= p->last_step->type= JSON_PATH_KEY | double_wildcard;
      double_wildcard= JSON_PATH_KEY_NULL;
      /* fall through */
    case PS_KEYX:
      p->last_step->key= p->s.c_str;
      continue;
    case PS_EAR:
      p->last_step->key_end= p->s.c_str - c_len;
      state= PS_AR;
      /* fall through */
    case PS_AR:
      p->last_step++;
      is_last= 0;
      is_to= 0;
      prev_value= 0;
      is_negative_index= 0;
      if (p->last_step - p->steps >= JSON_DEPTH_LIMIT)
        return p->s.error= JE_DEPTH;
      p->types_used|= p->last_step->type= JSON_PATH_ARRAY | double_wildcard;
      double_wildcard= JSON_PATH_KEY_NULL;
      p->last_step->n_item= 0;
      continue;
    case PS_ESC:
      if (json_handle_esc(&p->s))
        return 1;
      state= PS_KNM;
      continue;
    case PS_ESCX:
      if (json_handle_esc(&p->s))
        return 1;
      state= PS_KNMX;
      continue;
    case PS_KOK:
      p->last_step->key_end= p->s.c_str - c_len;
      state= PS_OK;
      break; /* 'break' as the loop supposed to end after that. */
    case PS_DWD:
      double_wildcard= JSON_PATH_DOUBLE_WILD;
      continue;
    case PS_NEG:
       p->types_used|= JSON_PATH_NEGATIVE_INDEX;
       is_negative_index= 1;
       continue;
    case PS_LAST:
      if ((p->s.error= skip_string_verbatim(&p->s, "ast")))
       return 1;
      p->types_used|= JSON_PATH_NEGATIVE_INDEX;
      is_last= 1;
      if (is_to)
        p->last_step->n_item_end= -1;
      else
        p->last_step->n_item= -1;
      continue;
    case PS_T:
      if ((p->s.error= skip_string_verbatim(&p->s, "o")))
        return 1;
      is_to= 1;
      is_negative_index= 0;
      is_last= 0;
      prev_value= 0;
      p->last_step->n_item_end= 0;
      p->last_step->type|= JSON_PATH_ARRAY_RANGE;
      p->types_used|= JSON_PATH_ARRAY_RANGE;
      continue;
    };
  } while (state != PS_OK);

  return double_wildcard ? (p->s.error= JE_SYN) : 0;
}


int json_skip_to_level(json_engine_t *j, int level)
{
  do {
    if (j->stack_p < level)
      return 0;
  } while (json_scan_next(j) == 0);

  return 1;
}


/*
  works as json_skip_level() but also counts items on the current
  level skipped.
*/
int json_skip_level_and_count(json_engine_t *j, int *n_items_skipped)
{
  int level= j->stack_p;

  *n_items_skipped= 0;
  while (json_scan_next(j) == 0)
  {
    if (j->stack_p < level)
      return 0;
    if (j->stack_p == level && j->state == JST_VALUE)
      (*n_items_skipped)++;
  }

  return 1;
}


int json_skip_array_and_count(json_engine_t *je, int *n_items)
{
  json_engine_t j= *je;
  *n_items= 0;

  return json_skip_level_and_count(&j, n_items); 
}


int json_skip_key(json_engine_t *j)
{
  if (json_read_value(j))
    return 1;

  if (json_value_scalar(j))
    return 0;

  return json_skip_level(j);
}


#define SKIPPED_STEP_MARK ((int) ~0)

/*
  Current step of the patch matches the JSON construction.
  Now we should either stop the search or go to the next
  step of the path.
*/
static int handle_match(json_engine_t *je, json_path_t *p,
                        json_path_step_t **p_cur_step, int *array_counters)
{
  json_path_step_t *next_step= *p_cur_step + 1;

  DBUG_ASSERT(*p_cur_step < p->last_step);

  if (json_read_value(je))
    return 1;

  if (json_value_scalar(je))
  {
    while (next_step->type == JSON_PATH_ARRAY && next_step->n_item == 0)
    {
      if (++next_step > p->last_step)
      {
        je->s.c_str= je->value_begin;
        return 1;
      }
    }
    return 0;
  }

  if (next_step->type == JSON_PATH_ARRAY && next_step->n_item == 0 &&
      je->value_type & JSON_VALUE_OBJECT)
  {
    do
    {
      array_counters[next_step - p->steps]= SKIPPED_STEP_MARK;
      if (++next_step > p->last_step)
      {
        je->s.c_str= je->value_begin;
        je->stack_p--;
        return 1;
      }
    } while (next_step->type == JSON_PATH_ARRAY && next_step->n_item == 0);
  }

  if ((int) je->value_type !=
      (int) (next_step->type & JSON_PATH_KEY_OR_ARRAY))
    return json_skip_level(je);

  array_counters[next_step - p->steps]= 0;
  if (next_step->type & JSON_PATH_ARRAY)
  {
    int array_size;
    if (next_step->n_item >= 0)
      array_size= 0;
    else
    {
      json_engine_t j2= *je;
      if (json_skip_array_and_count(&j2, &array_size))
      {
        *je= j2;
        return 1;
      }
      array_size= -array_size;
    }
    array_counters[next_step - p->steps]= array_size;
  }

  *p_cur_step= next_step;
  return 0;
}


/*
  Check if the name of the current JSON key matches
  the step of the path.
*/
int json_key_matches(json_engine_t *je, json_string_t *k)
{
  while (json_read_keyname_chr(je) == 0)
  {
    if (json_read_string_const_chr(k) ||
        je->s.c_next != k->c_next)
      return 0;
  }

  return json_read_string_const_chr(k);
}


int json_find_path(json_engine_t *je,
                   json_path_t *p, json_path_step_t **p_cur_step,
                   int *array_counters)
{
  json_string_t key_name;
  int res= 0;

  json_string_set_cs(&key_name, p->s.cs);

  do
  {
    json_path_step_t *cur_step= *p_cur_step;
    switch (je->state)
    {
    case JST_KEY:
      DBUG_ASSERT(cur_step->type & JSON_PATH_KEY);
      if (!(cur_step->type & JSON_PATH_WILD))
      {
        json_string_set_str(&key_name, cur_step->key, cur_step->key_end);
        if (!json_key_matches(je, &key_name))
        {
          if (json_skip_key(je))
            goto exit;
          continue;
        }
      }
      if (cur_step == p->last_step ||
          handle_match(je, p, p_cur_step, array_counters))
        goto exit;
      break;
    case JST_VALUE:
      DBUG_ASSERT(cur_step->type & JSON_PATH_ARRAY);
      if (cur_step->type & JSON_PATH_ARRAY_RANGE)
      {
        res= (cur_step->n_item <= array_counters[cur_step - p->steps] &&
              cur_step->n_item_end >= array_counters[cur_step - p->steps]);
        array_counters[cur_step - p->steps]++;
      }
      else
        res= cur_step->n_item == array_counters[cur_step - p->steps]++;
      if ((cur_step->type & JSON_PATH_WILD) || res)
      {
        /* Array item matches. */
        if (cur_step == p->last_step ||
            handle_match(je, p, p_cur_step, array_counters))
          goto exit;
      }
      else
        json_skip_array_item(je);
      break;
    case JST_OBJ_END:
      do
      {
        (*p_cur_step)--;
      } while (*p_cur_step > p->steps &&
               array_counters[*p_cur_step - p->steps] == SKIPPED_STEP_MARK);
      break;
    case JST_ARRAY_END:
      (*p_cur_step)--;
      break;
    default:
      DBUG_ASSERT(0);
      break;
    };
  } while (json_scan_next(je) == 0);

  /* No luck. */
  return 1;

exit:
  return je->s.error;
}


int json_find_paths_first(json_engine_t *je, json_find_paths_t *state,
                          uint n_paths, json_path_t *paths, uint *path_depths)
{
  state->n_paths= n_paths;
  state->paths= paths;
  state->cur_depth= 0;
  state->path_depths= path_depths;
  return json_find_paths_next(je, state);
}


int json_find_paths_next(json_engine_t *je, json_find_paths_t *state)
{
  uint p_c;
  int path_found, no_match_found;
  do
  {
    switch (je->state)
    {
    case JST_KEY:
      path_found= FALSE;
      no_match_found= TRUE;
      for (p_c=0; p_c < state->n_paths; p_c++)
      {
        json_path_step_t *cur_step;
        if (state->path_depths[p_c] <
              state->cur_depth /* Path already failed. */ ||
            !((cur_step= state->paths[p_c].steps + state->cur_depth)->type &
              JSON_PATH_KEY))
          continue;

        if (!(cur_step->type & JSON_PATH_WILD))
        {
          json_string_t key_name;
          json_string_setup(&key_name, state->paths[p_c].s.cs,
                            cur_step->key, cur_step->key_end);
          if (!json_key_matches(je, &key_name))
            continue;
        }
        if (cur_step == state->paths[p_c].last_step + state->cur_depth)
          path_found= TRUE;
        else
        {
          no_match_found= FALSE;
          state->path_depths[p_c]= state->cur_depth + 1;
        }
      }
      if (path_found)
        /* Return the result. */
        goto exit;
      if (no_match_found)
      {
        /* No possible paths left to check. Just skip the level. */
        if (json_skip_level(je))
          goto exit;
      }

      break;
    case JST_VALUE:
      path_found= FALSE;
      no_match_found= TRUE;
      for (p_c=0; p_c < state->n_paths; p_c++)
      {
        json_path_step_t *cur_step;
        if (state->path_depths[p_c]< state->cur_depth /* Path already failed. */ ||
            !((cur_step= state->paths[p_c].steps + state->cur_depth)->type &
              JSON_PATH_ARRAY))
          continue;
        if (cur_step->type & JSON_PATH_WILD ||
            cur_step->n_item == state->array_counters[state->cur_depth])
        {
          /* Array item matches. */
          if (cur_step == state->paths[p_c].last_step + state->cur_depth)
            path_found= TRUE;
          else
          {
            no_match_found= FALSE;
            state->path_depths[p_c]= state->cur_depth + 1;
          }
        }
      }

      if (path_found)
        goto exit;

      if (no_match_found)
        json_skip_array_item(je);

      state->array_counters[state->cur_depth]++;
      break;
    case JST_OBJ_START:
    case JST_ARRAY_START:
      for (p_c=0; p_c < state->n_paths; p_c++)
      {
        if (state->path_depths[p_c] < state->cur_depth)
          /* Path already failed. */
          continue;
        if (state->paths[p_c].steps[state->cur_depth].type &
            ((je->state == JST_OBJ_START) ? JSON_PATH_KEY : JSON_PATH_ARRAY))
          state->path_depths[p_c]++;
      }
      state->cur_depth++;
      break;
    case JST_OBJ_END:
    case JST_ARRAY_END:
      for (p_c=0; p_c < state->n_paths; p_c++)
      {
        if (state->path_depths[p_c] < state->cur_depth)
          continue;
        state->path_depths[p_c]--;
      }
      state->cur_depth--;
      break;
    default:
      DBUG_ASSERT(0);
      break;
    };
  } while (json_scan_next(je) == 0);

  /* No luck. */
  return 1;

exit:
  return je->s.error;
}


int json_append_ascii(CHARSET_INFO *json_cs,
                      uchar *json, uchar *json_end,
                      const uchar *ascii, const uchar *ascii_end)
{
  const uchar *json_start= json;
  while (ascii < ascii_end)
  {
    int c_len;
    if ((c_len= my_ci_wc_mb(json_cs, (my_wc_t) *ascii, json, json_end)) > 0)
    {
      json+= c_len;
      ascii++;
      continue;
    }

    /* Error return. */
    return c_len;
  }

  return (int)(json - json_start);
}


int json_unescape(CHARSET_INFO *json_cs,
                  const uchar *json_str, const uchar *json_end,
                  CHARSET_INFO *res_cs, uchar *res, uchar *res_end)
{
  json_string_t s;
  const uchar *res_b= res;

  json_string_setup(&s, json_cs, json_str, json_end);
  while (json_read_string_const_chr(&s) == 0)
  {
    int c_len;
    if ((c_len= my_ci_wc_mb(res_cs, s.c_next, res, res_end)) > 0)
    {
      res+= c_len;
      continue;
    }
    if (c_len == MY_CS_ILUNI)
    {
      /*
        Result charset doesn't support the json's character.
        Let's replace it with the '?' symbol.
      */
      if ((c_len= my_ci_wc_mb(res_cs, '?', res, res_end)) > 0)
      {
        res+= c_len;
        continue;
      }
    }
    /* Result buffer is too small. */
    return -1;
  }

  return s.error==JE_EOS ? (int)(res - res_b) : -1;
}


/* When we need to replace a character with the escaping. */
enum json_esc_char_classes {
  ESC_= 0,    /* No need to escape. */
  ESC_U= 'u', /* Character not allowed in JSON. Always escape as \uXXXX. */
  ESC_B= 'b', /* Backspace. Escape as \b */
  ESC_F= 'f', /* Formfeed. Escape as \f */
  ESC_N= 'n', /* Newline. Escape as \n */
  ESC_R= 'r', /* Return. Escape as \r */
  ESC_T= 't', /* Tab. Escape as \s */
  ESC_BS= '\\'  /* Backslash or '"'. Escape by the \\ prefix. */
};


/* This specifies how we should escape the character. */
static enum json_esc_char_classes json_escape_chr_map[0x60] = {
  ESC_U,   ESC_U,   ESC_U,   ESC_U,   ESC_U,   ESC_U,   ESC_U,   ESC_U,
  ESC_B,   ESC_T,   ESC_N,   ESC_U,   ESC_F,   ESC_R,   ESC_U,   ESC_U,
  ESC_U,   ESC_U,   ESC_U,   ESC_U,   ESC_U,   ESC_U,   ESC_U,   ESC_U,
  ESC_U,   ESC_U,   ESC_U,   ESC_U,   ESC_U,   ESC_U,   ESC_U,   ESC_U,

  ESC_,    ESC_,    ESC_BS,  ESC_,    ESC_,    ESC_,    ESC_,    ESC_,
  ESC_,    ESC_,    ESC_,    ESC_,    ESC_,    ESC_,    ESC_,    ESC_,
  ESC_,    ESC_,    ESC_,    ESC_,    ESC_,    ESC_,    ESC_,    ESC_,
  ESC_,    ESC_,    ESC_,    ESC_,    ESC_,    ESC_,    ESC_,    ESC_,

  ESC_,    ESC_,    ESC_,    ESC_,    ESC_,    ESC_,    ESC_,    ESC_,
  ESC_,    ESC_,    ESC_,    ESC_,    ESC_,    ESC_,    ESC_,    ESC_,
  ESC_,    ESC_,    ESC_,    ESC_,    ESC_,    ESC_,    ESC_,    ESC_,
  ESC_,    ESC_,    ESC_,    ESC_,    ESC_BS,  ESC_,    ESC_,    ESC_,
};


static const char hexconv[16] = "0123456789ABCDEF";


int json_escape(CHARSET_INFO *str_cs,
                const uchar *str, const uchar *str_end,
                CHARSET_INFO *json_cs, uchar *json, uchar *json_end)
{
  const uchar *json_start= json;

  while (str < str_end)
  {
    my_wc_t c_chr;
    int c_len;
    if ((c_len= my_ci_mb_wc(str_cs, &c_chr, str, str_end)) > 0)
    {
      enum json_esc_char_classes c_class;
      
      str+= c_len;
      if (c_chr >= 0x60 || (c_class= json_escape_chr_map[c_chr]) == ESC_)
      {
        if ((c_len= my_ci_wc_mb(json_cs, c_chr, json, json_end)) > 0)
        {
          json+= c_len;
          continue;
        }
        if (c_len < 0)
        {
          /* JSON buffer is depleted. */
          return JSON_ERROR_OUT_OF_SPACE;
        }

        /* JSON charset cannot convert this character. */
        c_class= ESC_U;
      }

      if ((c_len= my_ci_wc_mb(json_cs, '\\', json, json_end)) <= 0 ||
          (c_len= my_ci_wc_mb(json_cs, (c_class == ESC_BS) ? c_chr : c_class,
                                       json+= c_len, json_end)) <= 0)
      {
        /* JSON buffer is depleted. */
        return JSON_ERROR_OUT_OF_SPACE;
      }
      json+= c_len;

      if (c_class != ESC_U)
        continue;

      {
        /* We have to use /uXXXX escaping. */
        uchar utf16buf[4];
        uchar code_str[8];
        int u_len= my_uni_utf16(0, c_chr, utf16buf, utf16buf + 4);

        code_str[0]= hexconv[utf16buf[0] >> 4];
        code_str[1]= hexconv[utf16buf[0] & 15];
        code_str[2]= hexconv[utf16buf[1] >> 4];
        code_str[3]= hexconv[utf16buf[1] & 15];

        if (u_len > 2)
        {
          code_str[4]= hexconv[utf16buf[2] >> 4];
          code_str[5]= hexconv[utf16buf[2] & 15];
          code_str[6]= hexconv[utf16buf[3] >> 4];
          code_str[7]= hexconv[utf16buf[3] & 15];
        }
        
        if ((c_len= json_append_ascii(json_cs, json, json_end,
                                      code_str, code_str+u_len*2)) > 0)
        {
          json+= c_len;
          continue;
        }
        /* JSON buffer is depleted. */
        return JSON_ERROR_OUT_OF_SPACE;
      }
    }
    else /* c_len == 0, an illegal symbol. */
      return JSON_ERROR_ILLEGAL_SYMBOL;
  }

  return (int)(json - json_start);
}


int json_get_path_start(json_engine_t *je, CHARSET_INFO *i_cs,
                        const uchar *str, const uchar *end,
                        json_path_t *p)
{
  json_scan_start(je, i_cs, str, end);
  p->last_step= p->steps - 1; 
  return 0;
}


int json_get_path_next(json_engine_t *je, json_path_t *p)
{
  if (p->last_step < p->steps)
  {
    if (json_read_value(je))
      return 1;

    p->last_step= p->steps;
    p->steps[0].type= JSON_PATH_ARRAY_WILD;
    p->steps[0].n_item= 0;
    return 0;
  }
  else
  {
    if (json_value_scalar(je))
    {
      if (p->last_step->type & JSON_PATH_ARRAY)
        p->last_step->n_item++;
    }
    else
    {
      p->last_step++;
      p->last_step->type= (enum json_path_step_types) je->value_type;
      p->last_step->n_item= 0;
    }

    if (json_scan_next(je))
      return 1;
  }

  do
  {
    switch (je->state)
    {
    case JST_KEY:
      p->last_step->key= je->s.c_str;
      do
      {
        p->last_step->key_end= je->s.c_str;
      } while (json_read_keyname_chr(je) == 0);
      if (je->s.error)
        return 1;
      /* Now we have je.state == JST_VALUE, so let's handle it. */

      /* fall through */
    case JST_VALUE:
      if (json_read_value(je))
        return 1;
      return 0;
    case JST_OBJ_END:
    case JST_ARRAY_END:
      p->last_step--;
      if (p->last_step->type & JSON_PATH_ARRAY)
        p->last_step->n_item++;
      break;
    default:
      break;
    }
  } while (json_scan_next(je) == 0);

  return 1;
}


int json_path_parts_compare(
    const json_path_step_t *a, const json_path_step_t *a_end,
    const json_path_step_t *b, const json_path_step_t *b_end,
    enum json_value_types vt, const int *array_sizes)
{
  int res, res2;
  const json_path_step_t *temp_b= b;

  while (a <= a_end)
  {
    if (b > b_end)
    {
      while (vt != JSON_VALUE_ARRAY &&
             (a->type & JSON_PATH_ARRAY_WILD) == JSON_PATH_ARRAY &&
             a->n_item == 0)
      {
        if (++a > a_end)
          return 0;
      }
      return -2;
    }

    DBUG_ASSERT((b->type & (JSON_PATH_WILD | JSON_PATH_DOUBLE_WILD)) == 0);

    if (a->type & JSON_PATH_ARRAY)
    {
      if (b->type & JSON_PATH_ARRAY)
      {
        int res= 0, corrected_n_item_a= 0;
        if (array_sizes)
          corrected_n_item_a= a->n_item < 0 ?
                                array_sizes[b-temp_b] + a->n_item : a->n_item;
        if (a->type & JSON_PATH_ARRAY_RANGE)
        {
          int corrected_n_item_end_a= 0;
          if (array_sizes)
            corrected_n_item_end_a= a->n_item_end < 0 ?
                                    array_sizes[b-temp_b] + a->n_item_end :
                                    a->n_item_end;
          res= b->n_item >= corrected_n_item_a &&
                b->n_item <= corrected_n_item_end_a;
        }
        else
         res= corrected_n_item_a == b->n_item;

        if ((a->type & JSON_PATH_WILD) || res)
          goto step_fits;
        goto step_failed;
      }
      if ((a->type & JSON_PATH_WILD) == 0 && a->n_item == 0)
        goto step_fits_autowrap;
      goto step_failed;
    }
    else /* JSON_PATH_KEY */
    {
      if (!(b->type & JSON_PATH_KEY))
        goto step_failed;
    
      if (!(a->type & JSON_PATH_WILD) &&
          (a->key_end - a->key != b->key_end - b->key ||
           memcmp(a->key, b->key, a->key_end - a->key) != 0))
        goto step_failed;

      goto step_fits;
    }
step_failed:
    if (!(a->type & JSON_PATH_DOUBLE_WILD))
      return -1;
    b++;
    continue;

step_fits:
    b++;
    if (!(a->type & JSON_PATH_DOUBLE_WILD))
    {
      a++;
      continue;
    }

    /* Double wild handling needs recursions. */
    res= json_path_parts_compare(a+1, a_end, b, b_end, vt,
                                 array_sizes ? array_sizes + (b - temp_b) :
                                               NULL);
    if (res == 0)
      return 0;

    res2= json_path_parts_compare(a, a_end, b, b_end, vt,
                                  array_sizes ? array_sizes + (b - temp_b) :
                                                NULL);

    return (res2 >= 0) ? res2 : res;

step_fits_autowrap:
    if (!(a->type & JSON_PATH_DOUBLE_WILD))
    {
      a++;
      continue;
    }

    /* Double wild handling needs recursions. */
    res= json_path_parts_compare(a+1, a_end, b+1, b_end, vt,
                                 array_sizes ? array_sizes + (b - temp_b) :
                                               NULL);
    if (res == 0)
      return 0;

    res2= json_path_parts_compare(a, a_end, b+1, b_end, vt,
                                  array_sizes ? array_sizes + (b - temp_b) :
                                                NULL);

    return (res2 >= 0) ? res2 : res;

  }

  return b <= b_end;
}


int json_path_compare(const json_path_t *a, const json_path_t *b,
                      enum json_value_types vt, const int *array_size)
{
  return json_path_parts_compare(a->steps+1, a->last_step,
                                 b->steps+1, b->last_step, vt, array_size);
}


static enum json_types smart_read_value(json_engine_t *je,
                                        const char **value, int *value_len)
{
  if (json_read_value(je))
    goto err_return;

  *value= (char *) je->value;

  if (json_value_scalar(je))
    *value_len= je->value_len;
  else
  {
    if (json_skip_level(je))
      goto err_return;

    *value_len= (int) ((char *) je->s.c_str - *value);
  }

  compile_time_assert((int) JSON_VALUE_OBJECT == (int) JSV_OBJECT);
  compile_time_assert((int) JSON_VALUE_ARRAY == (int) JSV_ARRAY);
  compile_time_assert((int) JSON_VALUE_STRING == (int) JSV_STRING);
  compile_time_assert((int) JSON_VALUE_NUMBER == (int) JSV_NUMBER);
  compile_time_assert((int) JSON_VALUE_TRUE == (int) JSV_TRUE);
  compile_time_assert((int) JSON_VALUE_FALSE == (int) JSV_FALSE);
  compile_time_assert((int) JSON_VALUE_NULL == (int) JSV_NULL);

  return (enum json_types) je->value_type;

err_return:
  return JSV_BAD_JSON;
}


enum json_types json_type(const char *js, const char *js_end,
                          const char **value, int *value_len)
{
  json_engine_t je;

  json_scan_start(&je, &my_charset_utf8mb4_bin,(const uchar *) js,
                  (const uchar *) js_end);

  return smart_read_value(&je, value, value_len);
}


enum json_types json_get_array_item(const char *js, const char *js_end,
                                    int n_item,
                                    const char **value, int *value_len)
{
  json_engine_t je;
  int c_item= 0;

  json_scan_start(&je, &my_charset_utf8mb4_bin,(const uchar *) js,
                  (const uchar *) js_end);

  if (json_read_value(&je) ||
      je.value_type != JSON_VALUE_ARRAY)
    goto err_return;

  while (!json_scan_next(&je))
  {
    switch (je.state)
    {
    case JST_VALUE:
      if (c_item == n_item)
        return smart_read_value(&je, value, value_len);

      if (json_skip_key(&je))
        goto err_return;

      c_item++;
      break;

    case JST_ARRAY_END:
      *value= (const char *) (je.s.c_str - je.sav_c_len);
      *value_len= c_item;
      return JSV_NOTHING;
    }
  }

err_return:
  return JSV_BAD_JSON;
}


/** Simple json lookup for a value by the key.

  Expects JSON object.
  Only scans the 'first level' of the object, not
  the nested structures.

  @param js          [in]       json object to search in
  @param js_end      [in]       end of json string
  @param key         [in]       key to search for
  @param key_end     [in]         - " -
  @param value_start [out]      pointer into js (value or closing })
  @param value_len   [out]      length of the value found or number of keys

  @retval the type of the key value
  @retval JSV_BAD_JSON - syntax error found reading JSON.
                         or not JSON object.
  @retval JSV_NOTHING - no such key found.
*/
enum json_types json_get_object_key(const char *js, const char *js_end,
                                    const char *key,
                                    const char **value, int *value_len)
{
  const char *key_end= key + strlen(key);
  json_engine_t je;
  json_string_t key_name;
  int n_keys= 0;

  json_string_set_cs(&key_name, &my_charset_utf8mb4_bin);

  json_scan_start(&je, &my_charset_utf8mb4_bin,(const uchar *) js,
                  (const uchar *) js_end);

  if (json_read_value(&je) ||
      je.value_type != JSON_VALUE_OBJECT)
    goto err_return;

  while (!json_scan_next(&je))
  {
    switch (je.state)
    {
    case JST_KEY:
      n_keys++;
      json_string_set_str(&key_name, (const uchar *) key,
                          (const uchar *) key_end);
      if (json_key_matches(&je, &key_name))
        return smart_read_value(&je, value, value_len);

      if (json_skip_key(&je))
        goto err_return;

      break;

    case JST_OBJ_END:
      *value= (const char *) (je.s.c_str - je.sav_c_len);
      *value_len= n_keys;
      return JSV_NOTHING;
    }
  }

err_return:
  return JSV_BAD_JSON;
}


enum json_types json_get_object_nkey(const char *js __attribute__((unused)),
                                     const char *js_end __attribute__((unused)),
                                     int nkey __attribute__((unused)),
                                     const char **keyname __attribute__((unused)),
                                     const char **keyname_end __attribute__((unused)),
                                     const char **value __attribute__((unused)),
                                     int *value_len __attribute__((unused)))
{
  return JSV_NOTHING;
}


/** Check if json is valid (well-formed)

  @retval 0 - success, json is well-formed
  @retval 1 - error, json is invalid
*/
int json_valid(const char *js, size_t js_len, CHARSET_INFO *cs)
{
  json_engine_t je;
  json_scan_start(&je, cs, (const uchar *) js, (const uchar *) js + js_len);
  while (json_scan_next(&je) == 0) /* no-op */ ;
  return je.s.error == 0;
}


/*
  Expects the JSON object as an js argument, and the key name.
  Looks for this key in the object and returns
  the location of all the text related to it.
  The text includes the comma, separating this key.

  comma_pos - the hint where the comma is. It is important
       if you plan to replace the key rather than just cut.
    1  - comma is on the left
    2  - comma is on the right.
    0  - no comma at all (the object has just this single key)
 
  if no such key found *key_start is set to NULL.
*/
int json_locate_key(const char *js, const char *js_end,
                    const char *kname,
                    const char **key_start, const char **key_end,
                    int *comma_pos)
{
  const char *kname_end= kname + strlen(kname);
  json_engine_t je;
  json_string_t key_name;
  int t_next, c_len, match_result;

  json_string_set_cs(&key_name, &my_charset_utf8mb4_bin);

  json_scan_start(&je, &my_charset_utf8mb4_bin,(const uchar *) js,
                  (const uchar *) js_end);

  if (json_read_value(&je) ||
      je.value_type != JSON_VALUE_OBJECT)
    goto err_return;

  *key_start= (const char *) je.s.c_str;
  *comma_pos= 0;

  while (!json_scan_next(&je))
  {
    switch (je.state)
    {
    case JST_KEY:
      json_string_set_str(&key_name, (const uchar *) kname,
                          (const uchar *) kname_end);
      match_result= json_key_matches(&je, &key_name);
      if (json_skip_key(&je))
        goto err_return;
      get_first_nonspace(&je.s, &t_next, &c_len);
      je.s.c_str-= c_len;

      if (match_result)
      {
        *key_end= (const char *) je.s.c_str;

        if (*comma_pos == 1)
          return 0;

        DBUG_ASSERT(*comma_pos == 0);

        if (t_next == C_COMMA)
        {
          *key_end+= c_len;
          *comma_pos= 2;
        }
        else if (t_next == C_RCURB)
          *comma_pos= 0;
        else
          goto err_return;
        return 0;
      }

      *key_start= (const char *) je.s.c_str;
      *comma_pos= 1;
      break;

    case JST_OBJ_END:
      *key_start= NULL;
      return 0;
    }
  }

err_return:
  return 1;

}
