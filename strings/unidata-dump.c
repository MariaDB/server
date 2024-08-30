const char COPYING[]= "\
/* Copyright (c) 2000, 2013, Oracle and/or its affiliates.\n\
   Copyright (c) 2009, 2023, MariaDB Corporation.\n\
\n\
   This program is free software; you can redistribute it and/or modify\n\
   it under the terms of the GNU General Public License as published by\n\
   the Free Software Foundation; version 2 of the License.\n\
\n\
   This program is distributed in the hope that it will be useful,\n\
   but WITHOUT ANY WARRANTY; without even the implied warranty of\n\
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the\n\
   GNU General Public License for more details.\n\
\n\
   You should have received a copy of the GNU General Public License\n\
   along with this program; if not, write to the Free Software\n\
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA\n\
*/\n";

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_UNI_CHAR 0x10FFFF
#define MAX_UNI_PAGE 0x10FF

#define STRING_WITH_LEN(X) (X), ((size_t) (sizeof(X) - 1))


typedef unsigned int my_wchar_t;

/* Character types, as in m_ctype.h */
#define _MY_U   01      /* Upper case */
#define _MY_L   02      /* Lower case */
#define _MY_NMR 04      /* Numeral (digit) */
#define _MY_SPC 010     /* Spacing character */
#define _MY_PNT 020     /* Punctuation */
#define _MY_CTR 040     /* Control character */
#define _MY_B   0100    /* Blank */
#define _MY_X   0200    /* heXadecimal digit */

#define CT_MAX    _MY_X
#define CT_CJK    _MY_L | _MY_U
#define CT_HANGUL _MY_L | _MY_U
#define CT_NONE  0


/* Decomposition types */
typedef enum
{
  DT_UNKNOWN,
  DT_FONT,
  DT_NOBREAK,
  DT_INITIAL,
  DT_MEDIAL,
  DT_FINAL,
  DT_ISOLATED,
  DT_CIRCLE,
  DT_SUPER,
  DT_SUB,
  DT_VERTICAL,
  DT_WIDE,
  DT_NARROW,
  DT_SMALL,
  DT_SQUARE,
  DT_FRACTION,
  DT_COMPAT
} decomposition_type_t;


typedef enum
{
  PAGE_DATA_USELESS=    0,
  PAGE_DATA_IMPORTANT=  1,
  PAGE_DATA_DUMMY=      2
} page_data_type_t;


typedef struct
{
  page_data_type_t page_tab;
  int page_overridden;
  int page_ctype;
} PAGE_STAT;


typedef struct
{
  const char *mode_name;
  int  print_ctype;
  int  print_toupper;
  int  print_tolower;
  int  print_noaccent;
  int  print_noaccent_tolower;
  int  print_noaccent_toupper;
  int  print_curly_brackets_in_items;
  int  print_curly_brackets_in_index;
  int  chars_per_line;
  int  single_array;
  int  pages_per_line_in_index;
  int  const_data;
  const char *page_data_type_name;
  const char *page_name;
  const char *page_name_derived;
  const char *index_data_type_name;
  const char *index_name;
} UNIDATA_OPT_MODE;


typedef struct
{
  my_wchar_t  max_char;
  my_wchar_t  dummy_pages_codepoint_max;
  const char *filename;
  UNIDATA_OPT_MODE mode;
} UNIDATA_OPT;


my_wchar_t npages_by_opt(const UNIDATA_OPT *opt)
{
  return (opt->max_char + 1) / 256;
}


typedef struct my_ctype_name_st
{
  const char *name;
  int val;
  int to_be_decomposed;
} MY_CTYPE_NAME_ST;


static MY_CTYPE_NAME_ST my_ctype_name[]=
{
  {"Lu", _MY_U, 1},                /* Letter, Uppercase          */
  {"Ll", _MY_L, 1},                /* Letter, Lowercase          */
  {"Lt", _MY_U, 1},                /* Letter, Titlecase          */
  {"Lo", _MY_L, 1},                /* Letter, other              */
  {"Lm", _MY_L, 0},                /* Letter, Modifier           */

  {"Nd", _MY_NMR, 0},              /* Number, Decimal Digit      */
  {"Nl", _MY_NMR|_MY_U|_MY_L, 0},  /* Number, Letter             */
  {"No", _MY_NMR|_MY_PNT, 0},      /* Number, Other              */

  {"Mn", _MY_L|_MY_PNT, 0},        /* Mark, Nonspacing           */
  {"Mc", _MY_L|_MY_PNT, 1},        /* Mark, Spacing Combining    */
  {"Me", _MY_L|_MY_PNT, 0},        /* Mark, Enclosing            */

  {"Pc", _MY_PNT, 0},              /* Punctuation, Connector     */
  {"Pd", _MY_PNT, 0},              /* Punctuation, Dash          */
  {"Ps", _MY_PNT, 0},              /* Punctuation, Open          */
  {"Pe", _MY_PNT, 0},              /* Punctuation, Close         */
  {"Pi", _MY_PNT, 0},              /* Punctuation, Initial quote */
  {"Pf", _MY_PNT, 0},              /* Punctuation, Final quote   */
  {"Po", _MY_PNT, 0},              /* Punctuation, Other         */

  {"Sm", _MY_PNT, 0},              /* Symbol, Math               */
  {"Sc", _MY_PNT, 0},              /* Symbol, Currency           */
  {"Sk", _MY_PNT, 0},              /* Symbol, Modifier           */
  {"So", _MY_PNT, 0},              /* Symbol, Other              */

  {"Zs", _MY_SPC, 0},              /* Separator, Space           */
  {"Zl", _MY_SPC, 0},              /* Separator, Line            */
  {"Zp", _MY_SPC, 0},              /* Separator, Paragraph       */

  {"Cc", _MY_CTR, 0},              /* Other, Control             */
  {"Cf", _MY_CTR, 0},              /* Other, Format              */
  {"Cs", _MY_CTR, 0},              /* Other, Surrogate           */
  {"Co", _MY_CTR, 0},              /* Other, Private Use         */
  {"Cn", _MY_CTR, 0},              /* Other, Not Assigned        */
  {NULL, 0, 0}
};


static const MY_CTYPE_NAME_ST *
ctype_name_st_find(my_wchar_t codepoint, const char *tok)
{
  MY_CTYPE_NAME_ST *p;
  for (p= my_ctype_name; p->name; p++)
  {
    if (!strncasecmp(p->name, tok, 2))
      return p;
  }
  return NULL;
}


static int
ctype_name_st_to_num(const MY_CTYPE_NAME_ST *st, my_wchar_t codepoint)
{
  if ((codepoint >= 'a' && codepoint <= 'z') ||
      (codepoint >= 'A' && codepoint <= 'Z'))
    return st->val | _MY_X;
  return st->val;
}


static UNIDATA_OPT opt_caseinfo=
{
  0x10FFFF, /* max_char */
  0x7FF,    /* dummy_pages_codepoint_max == utf8 mb2 range */
  NULL,     /*filename*/
  {
    "caseinfo", /* mode name */
    0,        /* print_ctype */
    1,        /* print_toupper */
    1,        /* print_tolower */
    0,        /* print_noaccent */
    0,        /* print_noaccent_tolower */
    1,        /* print_noaccent_toupper */
    1,        /* print_curly_brackets_in_items */
    0,        /* print_curly_brackets_in_index */
    2,        /* chars_per_line */
    0,        /* single_array */
    8,        /* pages_per_line_in_index */
    0,        /* const_data */
    "MY_UNICASE_CHARACTER",    /* page_data_type_name */
    "plane",                   /* page_name */
    NULL,                      /* page_name_derived */
    "MY_UNICASE_CHARACTER *",  /* index_data_type_name */
    "my_unicase_default_pages" /* index_name */
  }
};


static UNIDATA_OPT opt_casefold=
{
  0x10FFFF, /* max_char */
  0x7FF,    /* dummy_pages_codepoint_max == utf8 mb2 range */
  NULL,      /*filename*/
  {
    "casefold", /* mode name */
    0,        /* print_ctype */
    1,        /* print_toupper */
    1,        /* print_tolower */
    0,        /* print_noaccent */
    0,        /* print_noaccent_tolower */
    0,        /* print_noaccent_toupper */
    1,        /* print_curly_brackets_in_items */
    0,        /* print_curly_brackets_in_index */
    2,        /* chars_per_line */
    0,        /* single_array */
    8,        /* pages_per_line_in_index */
    1,        /* const_data */
    "MY_CASEFOLD_CHARACTER" ,   /* page_data_type_name */
    "page",                     /* page_name */
    NULL,                       /* page_name_derived */
    "MY_CASEFOLD_CHARACTER *",  /* index_data_type_name */
    "my_casefold_default_pages" /* index_name */
  }
};


static UNIDATA_OPT opt_casefold_tr=
{
  0x10FFFF, /* max_char */
  0x7FF,    /* dummy_pages_codepoint_max == utf8 mb2 range */
  NULL,      /*filename*/
  {
    "casefold-tr", /* mode name */
    0,        /* print_ctype */
    1,        /* print_toupper */
    1,        /* print_tolower */
    0,        /* print_noaccent */
    0,        /* print_noaccent_tolower */
    0,        /* print_noaccent_toupper */
    1,        /* print_curly_brackets_in_items */
    0,        /* print_curly_brackets_in_index */
    2,        /* chars_per_line */
    0,        /* single_array */
    8,        /* pages_per_line_in_index */
    1,        /* const_data */
    "MY_CASEFOLD_CHARACTER" ,   /* page_data_type_name */
    "page_tr",                  /* page_name */
    "page",                     /* page_name_derived */
    "MY_CASEFOLD_CHARACTER *",  /* index_data_type_name */
    "my_casefold_tr_pages"      /* index_name */
  }
};


static UNIDATA_OPT opt_weight_general_ci=
{
  0xFFFF,   /* max_char */
  0x7FF,    /* dummy_pages_codepoint_max == utf8 mb2 range */
  NULL,      /*filename*/
  {
    "weight_general_ci", /* mode name */
    0,        /* print_ctype */
    0,        /* print_toupper */
    0,        /* print_tolower */
    0,        /* print_noaccent */
    0,        /* print_noaccent_tolower */
    1,        /* print_noaccent_toupper */
    0,        /* print_curly_brackets_in_items */
    0,        /* print_curly_brackets_in_index */
    8,        /* chars_per_line */
    0,        /* single_array */
    2,        /* pages_per_line_in_index */
    1,        /* const_data */
    "uint16",                   /* page_data_type_name */
    "weight_general_ci_page",   /* page_name */
    NULL,                       /* page_name_derived */
    "uint16 *",                 /* index_data_type_name */
    "weight_general_ci_index"   /* index_name */
  }
};


static UNIDATA_OPT opt_weight_general_mysql500_ci=
{
  0xFFFF,   /* max_char */
  0x7FF,    /* dummy_pages_codepoint_max == utf8 mb2 range */
  NULL,      /*filename*/
  {
    "weight_general_mysql500_ci", /* mode name */
    0,        /* print_ctype */
    0,        /* print_toupper */
    0,        /* print_tolower */
    0,        /* print_noaccent */
    0,        /* print_noaccent_tolower */
    1,        /* print_noaccent_toupper */
    0,        /* print_curly_brackets_in_items */
    0,        /* print_curly_brackets_in_index */
    8,        /* chars_per_line */
    0,        /* single_array */
    2,        /* pages_per_line_in_index */
    1,        /* const_data */
    "uint16",                        /* page_data_type_name */
    "weight_general_mysql500_ci_page", /* page_name */
    "weight_general_ci_page",          /* page_name_derived */
    "uint16 *",                        /* index_data_type_name */
    "weight_general_mysql500_ci_index" /* index_name */
  }
};


static UNIDATA_OPT opt_ctype=
{
  0x10FFFF, /* max_char */
  0x7FF,    /* dummy_pages_codepoint_max == utf8 mb2 range */
  NULL,     /*filename*/
  {
    "ctype",  /* mode name */
    1,        /* print_ctype */
    0,        /* print_toupper */
    0,        /* print_tolower */
    0,        /* print_noaccent */
    0,        /* print_noaccent_tolower */
    0,        /* print_noaccent_toupper */
    0,        /* print_curly_brackets_in_items */
    1,        /* print_curly_brackets_in_index */
    16,       /* chars_per_line */
    0,        /* single_array */
    1,        /* pages_per_line_in_index */
    1,        /* const_data */
    "unsigned char",           /* page_data_type_name */
    "uctype_page",             /* page_name */
    NULL,                      /* page_name_derived */
    "MY_UNI_CTYPE",            /* index_data_type_name */
    "my_uni_ctype"             /* index_name */
  }
};


int opt_set_mode(UNIDATA_OPT *to, const char *name_and_value, const char *value)
{
  if (!strcmp(value, "casefold"))
  {
    to->mode= opt_casefold.mode;
    return 0;
  }
  else if (!strcmp(value, "casefold-tr"))
  {
    to->mode= opt_casefold_tr.mode;
    return 0;
  }
  else if (!strcmp(value, "caseinfo"))
  {
    to->mode= opt_caseinfo.mode;
    return 0;
  }
  else if (!strcmp(value, "weight_general_ci"))
  {
    to->mode= opt_weight_general_ci.mode;
    return 0;
  }
  else if (!strcmp(value, "weight_general_mysql500_ci"))
  {
    to->mode= opt_weight_general_mysql500_ci.mode;
    return 0;
  }
  else if (!strcmp(value, "ctype"))
  {
    to->mode= opt_ctype.mode;
    return 0;
  }
  fprintf(stderr, "Bad option: %s\n", name_and_value);
  return 1;
}


static decomposition_type_t
get_decomposition_type(const char *str)
{
  if (!strcmp(str, "<font>"))     return DT_FONT;
  if (!strcmp(str, "<noBreak>"))  return DT_NOBREAK;
  if (!strcmp(str, "<initial>"))  return DT_INITIAL;
  if (!strcmp(str, "<medial>"))   return DT_MEDIAL;
  if (!strcmp(str, "<final>"))    return DT_FINAL;
  if (!strcmp(str, "<isolated>")) return DT_ISOLATED;
  if (!strcmp(str, "<circle>"))   return DT_CIRCLE;
  if (!strcmp(str, "<super>"))    return DT_SUPER;
  if (!strcmp(str, "<sub>"))      return DT_SUB;
  if (!strcmp(str, "<vertical>")) return DT_VERTICAL;
  if (!strcmp(str, "<wide>"))     return DT_WIDE;
  if (!strcmp(str, "<narrow>"))   return DT_NARROW;
  if (!strcmp(str, "<small>"))    return DT_SMALL;
  if (!strcmp(str, "<square>"))   return DT_SQUARE;
  if (!strcmp(str, "<fraction>")) return DT_FRACTION;
  if (!strcmp(str, "<compat>"))   return DT_COMPAT;
  return DT_UNKNOWN;
}


#define MAX_DECOMP 20


typedef struct
{
  int  ctype;
  int  toupper;
  int  tolower;
  int  noaccent;
  int  noaccent_tolower;
  int  noaccent_toupper;
  int  decomp_type;
  int  decomp[MAX_DECOMP];
  int  to_be_decomposed;
} UNIDATA_CHAR;



/************* Initialization functions *********/


static int
strip_accent(UNIDATA_CHAR *code, int i)
{
  if (code[i].decomp[0] &&
      code[i].decomp[1] >= 0x0300 &&
      code[i].decomp[1] <= 0x036F &&
      code[i].decomp[2] == 0)
    return strip_accent(code, code[i].decomp[0]);
  return i;
}


static void
set_noaccent(const UNIDATA_OPT *opt, UNIDATA_CHAR *code)
{
  my_wchar_t i;
  for (i= 0; i <= opt->max_char; i++)
  {
    code[i].noaccent= strip_accent(code, i);
  }
}


static void
set_noaccent_tolower(const UNIDATA_OPT *opt, UNIDATA_CHAR *code)
{
  my_wchar_t i;
  for (i= 0; i <= opt->max_char; i++)
  {
    code[i].noaccent_tolower= code[code[i].noaccent].tolower;
  }
}


static void
set_noaccent_toupper(const UNIDATA_OPT *opt, UNIDATA_CHAR *code)
{
  my_wchar_t i;
  for (i= 0; i <= opt->max_char; i++)
  {
    code[i].noaccent_toupper= code[code[i].noaccent].toupper;
  }
}


static void
set_default_case_folding(const UNIDATA_OPT *opt, UNIDATA_CHAR *code)
{
  my_wchar_t i;
  for (i= 0; i <= opt->max_char; i++)
  {
    code[i].tolower= i;
    code[i].toupper= i;
  }
}


/*
  Fill ideographs
*/

static void
fill_cjk(UNIDATA_CHAR *code)
{
  size_t i;
  /* CJK Ideographs Extension A (U+3400 - U+4DB5) */
  for(i=0x3400;i<=0x4DB5;i++)
  {
    code[i].tolower=i;
    code[i].ctype= CT_CJK;
  }
  /* CJK Ideographs (U+4E00 - U+9FA5) */
  for(i=0x4E00;i<=0x9FA5;i++)
  {
    code[i].tolower=i;
    code[i].ctype= CT_CJK;
  }
  /* Hangul Syllables (U+AC00 - U+D7A3)  */
  for(i=0xAC00;i<=0xD7A3;i++)
  {
    code[i].tolower=i;
    code[i].ctype= CT_HANGUL;
  }
}


/************* Loading functions ***************/


static void handle_general_category(const UNIDATA_OPT *opt,
                                    UNIDATA_CHAR *ch,
                                    const char *tok,
                                    my_wchar_t codepoint)
{
  /*
    TODO: check if ctype is set correctly.
    A difference can break fulltext indexes.
  */

  const MY_CTYPE_NAME_ST *ct= ctype_name_st_find(
                                (my_wchar_t) codepoint, tok);
  if (ct)
  {
    ch->ctype|= ctype_name_st_to_num(
                                 ct,
                                 (my_wchar_t) codepoint);
    ch->to_be_decomposed= ct->to_be_decomposed;
  }
}


int handle_decomposition(UNIDATA_CHAR *ch, char *tok, const char *str)
{
  char *lt, *part;
  size_t num;

  if (!ch->to_be_decomposed)
    return 0; /* Decompose only letters */

  for (part= strtok_r(tok, " ", &lt), num= 0;
       part;
       part= strtok_r(NULL, " ", &lt))
  {
    char *end;
    if (part[0] == '<')
    {
      if ((ch->decomp_type= get_decomposition_type(part)) == DT_UNKNOWN)
      {
        fprintf(stderr, "Unknown decomposition type:\n%s\n", str);
        return 1;
      }
      continue;
    }

    if (num + 1 >= MAX_DECOMP)
    {
      fprintf(stderr, "Too many decomposition parts:\n%s\n", str);
      return 1;
    }
    ch->decomp[num]= strtol(part,&end,16);
    ch->decomp[num+1]= 0;
    num++;
  }
  return 0;
}


static int
parse_unidata_line(const UNIDATA_OPT *opt, char *str, UNIDATA_CHAR *unidata)
{
  unsigned long codepoint= 0;
  int fieldno= 0;
  char *s;

  for (s= str; *s; fieldno++)
  {
    char *tok= s, *e;

    if ((e= strchr(s,';')))
    {
      *e= '\0';
      s= e + 1;
    }
    else
    {
      s+= strlen(s);
    }

    switch (fieldno)
    {
      case 0:                                  /* Code point */
        codepoint= strtoul(tok, NULL, 16);
        if (codepoint > opt->max_char)
          return 1;
        break;
      case 1:                                  /* name */
        break;
      case 2:                                  /* general category */
        handle_general_category(opt, &unidata[codepoint],
                                tok, (my_wchar_t) codepoint);
        break;
      case 3:                                  /* Canonical combining class */
        break;
      case 4:                                  /* BiDi class */
        break;
      case 5:                                  /* Decomposition type */
        if (tok[0] && handle_decomposition(&unidata[codepoint], tok, str))
          return -1;
        break;
      case 6:                                  /* Numeric_Type, Numeric Value */
        break;
      case 7:                                  /* Numeric_Type, Numeric Value */
         break;
      case 8:                                  /* Numeric_Type, Numeric Value */
        break;
      case 9:                                  /* BiDi mirrored */
        break;
      case 10:                                 /* Unicode_1_Name */
        break;
      case 11:                                 /* ISO_Comment    */
        break;
      case 12:                                 /*Simple_Uppercase_Mapping*/
        if (tok[0])
          unidata[codepoint].toupper= strtol(tok, NULL, 16);
        break;
      case 13:                                 /*Simple_Lowercase_Mapping*/
        if (tok[0])
          unidata[codepoint].tolower= strtol(tok, NULL, 16);
        break;
      case 14:                                 /* Simple_Titlecase_Mapping */
        break;
    }
  }

  return 0;
}


static int
load_unidata_file(const UNIDATA_OPT *opt, FILE *f, UNIDATA_CHAR *unidata)
{
  char str[1024];

  while (fgets(str, sizeof(str), f))
  {
    if (parse_unidata_line(opt, str, unidata) < 0)
      return 1;
  }
  return 0;
}


static int
load_unidata(const UNIDATA_OPT *opt, UNIDATA_CHAR *unidata)
{
  FILE *f;
  int rc;
  if (!(f= fopen(opt->filename, "r")))
  {
    fprintf(stderr, "Could not open file '%s'\n", opt->filename);
    return 1;
  }
  rc= load_unidata_file(opt, f, unidata);
  fclose(f);
  return rc;
}

/************** Printing functions ********************/

static void
print_one_char(const UNIDATA_OPT *opt, UNIDATA_CHAR *data, int code)
{
  UNIDATA_CHAR *ch= &data[code];
  const char *comma= "";

  if (opt->mode.print_curly_brackets_in_items)
    printf("{");

  if (opt->mode.print_ctype)
  {
    printf("%s", comma);
    printf("%3d", ch->ctype);
    comma= ",";
  }

  if (opt->mode.print_toupper)
  {
    printf("%s", comma);
    printf("0x%04X", ch->toupper);
    comma= ",";
  }

  if (opt->mode.print_tolower)
  {
    printf("%s", comma);
    printf("0x%04X", ch->tolower);
    comma= ",";
  }

  if (opt->mode.print_noaccent)
  {
    printf("%s", comma);
    printf("0x%04X", ch->noaccent);
    comma= ",";
  }

  if (opt->mode.print_noaccent_tolower)
  {
    printf("%s", comma);
    printf("0x%04X", ch->noaccent_tolower);
    comma= ",";
  }

  if (opt->mode.print_noaccent_toupper)
  {
    printf("%s", comma);
    printf("0x%04X", ch->noaccent_toupper);
    comma= ",";
  }

  if (opt->mode.print_curly_brackets_in_items)
    printf("}");

  if (opt->mode.single_array ||
      (code & 0xFF) != 0xFF) /* Don't print comma for the last char in a page */
    printf(",");
  else
    printf(" ");
}


static void
print_one_page(const UNIDATA_OPT *opt, UNIDATA_CHAR *data,
               my_wchar_t pageno, const PAGE_STAT *pstat)
{
  my_wchar_t charnum;

  if (!opt->mode.single_array || pageno == 0)
  {
    printf("%s%s%s %s%02X[256]={%s\n",
           pageno == 0 ? "" : "static ",
           opt->mode.const_data ? "const " : "",
           opt->mode.page_data_type_name, opt->mode.page_name,
           (unsigned int) pageno,
           pstat[pageno].page_tab == PAGE_DATA_DUMMY ?
           " /* This page is dummy */" : "");
  }

  for (charnum= 0; charnum < 256; charnum++)
  {
    my_wchar_t codepoint= (pageno << 8) + charnum;
    my_wchar_t rem= charnum % opt->mode.chars_per_line;
    if (!rem)
      printf("  ");
    print_one_char(opt, data, codepoint);
    if (rem + 1 == opt->mode.chars_per_line)
    {
      printf(" /* %04X */", (codepoint + 1) - opt->mode.chars_per_line);
      printf("\n");
    }
  }
  if (!opt->mode.single_array)
    printf("};\n\n");
}


static const char *page_name_in_index(const UNIDATA_OPT *opt,
                                      const PAGE_STAT *pstat,
                                      my_wchar_t pageno)
{
  if (!opt->mode.page_name_derived)
    return opt->mode.page_name;

  return pstat[pageno].page_overridden ?
         opt->mode.page_name :
         opt->mode.page_name_derived;
}


static void print_page_index(const UNIDATA_OPT *opt,
                             const PAGE_STAT *pstat)
{
  my_wchar_t page;
  my_wchar_t npages= npages_by_opt(opt);
  int printing_ctype= !strcmp(opt->mode.index_data_type_name, "MY_UNI_CTYPE");

  printf("%s%s %s[%d]={\n",
         opt->mode.const_data ? "const " : "",
         opt->mode.index_data_type_name, opt->mode.index_name,
         (unsigned int) npages);

  for (page= 0; page < npages; page++)
  {
    my_wchar_t rem= page % opt->mode.pages_per_line_in_index;
    if (!rem)
      printf("  ");
    if (opt->mode.print_curly_brackets_in_index)
      printf("{");
    if (printing_ctype)
      printf("%d,", pstat[page].page_ctype);

    if (pstat[page].page_tab)
      printf("%s%02X", page_name_in_index(opt, pstat, page),  page);
    else
      printf("NULL");

    if (opt->mode.print_curly_brackets_in_index)
      printf("}");

    if (page + 1 < npages)
      printf(",");

    if (rem + 1 == opt->mode.pages_per_line_in_index)
      printf("\n");
    else
      printf(" ");
  }
  printf("};\n");
}


static void print(UNIDATA_OPT *opt, UNIDATA_CHAR *unidata, const PAGE_STAT *pstat)
{
  my_wchar_t npages= npages_by_opt(opt);
  my_wchar_t page;

  /* Print all pages */
  for (page= 0; page < npages; page++)
  {
    if (opt->mode.page_name_derived && !pstat[page].page_overridden)
      continue;
    if (opt->mode.single_array || pstat[page].page_tab)
      print_one_page(opt, unidata, page, pstat);
  }

  /* Print index */
  if (!opt->mode.single_array)
    print_page_index(opt, pstat);
}


void print_command_line_options(int ac, char **av)
{
  int i;
  printf("/*\n");
  printf("  Generated by:\n");
  for (i= 0; i < ac; i++)
  {
    printf("    %s%s%s\n", i > 0 ? " " : "", av[i], i+1 < ac ? " \\" :"");
  }
  printf("\n");
  printf("*/\n");
}


static void calc_page_parameters(const UNIDATA_OPT *opt, const UNIDATA_CHAR *code,
                                 PAGE_STAT *pstat)
{
  my_wchar_t npages= npages_by_opt(opt);
  my_wchar_t page;
  for(page= 0; page < npages; page++)
  {
    int ntype[CT_MAX + 1], t;
    int character, done=0;

    memset(ntype,0,sizeof(ntype));
    for(character= 0;character < 256; character++)
    {
      size_t cod= (page << 8) + character;
      const UNIDATA_CHAR *ch= &code[cod];
      ntype[ch->ctype]++;

      if((ch->tolower  != cod ||
          ch->toupper  != cod ||
          ch->noaccent != cod ||
          ch->noaccent_toupper != cod) &&
         (opt->mode.print_tolower ||
          opt->mode.print_toupper ||
          opt->mode.print_noaccent ||
          opt->mode.print_noaccent_toupper))
      {
        pstat[page].page_tab= PAGE_DATA_IMPORTANT;
      }
    }

    if (opt->mode.print_ctype)
    {
      for (t= 0; t <= CT_MAX; t++)
      {
        if(ntype[t]==256)
        {
          /* All ctypes are the same */
          pstat[page].page_ctype= t;
          done=1;
          break;
        }
      }
    }
    else
    {
      done= 1; /* Don't need ctype */
    }

    if(!done)
    {
      /* Mixed page, lets create the table */
      pstat[page].page_ctype= CT_NONE;
      pstat[page].page_tab= PAGE_DATA_IMPORTANT;
    }
    if (!pstat[page].page_tab &&
        page <= (opt->dummy_pages_codepoint_max >> 8))
      pstat[page].page_tab= PAGE_DATA_DUMMY;
  }
}


static UNIDATA_CHAR code[MAX_UNI_CHAR + 1];
static PAGE_STAT pstat[MAX_UNI_PAGE + 1];


int usage(int ac, char **av)
{
  fprintf(stderr, "Usage: %s filename\n", av[0]);
  return 1;
}


const char *one_opt(const char *option, const char *name, size_t length)
{
  if (!strncmp(option, name, length))
    return option + length;
  return 0;
}


int get_option_bool(int *to, const char *name_and_value, const char *value)
{
  if (!strcmp(value, "1"))
    *to= 1;
  else if (!strcmp(value, "0"))
    *to= 0;
  else
  {
    fprintf(stderr, "Bad option: %s\n", name_and_value);
    return 1;
  }
  return 0;
}


int get_option_codepoint(my_wchar_t *to, const char *name_and_value, const char *value)
{
  unsigned long codepoint= value[0]=='0' && value[1]=='x' ?
                           strtoul(value + 2, NULL, 16) :
                           strtoul(value, NULL, 10);
  if (codepoint > MAX_UNI_CHAR)
  {
    fprintf(stderr, "Too large --max-char: %s\n", name_and_value);
    return 1;
  }
  *to= (my_wchar_t) codepoint;
  return 0;
}


int process_param(UNIDATA_OPT *opt, int ac, char **av)
{
  int i;
  if (ac < 2)
    return usage(ac, av);
  for (i= 1; i < ac; i++)
  {
    const char *op;
    if ((op= one_opt(av[i], STRING_WITH_LEN("--mode="))))
    {
      if (opt_set_mode(opt, av[i], op))
        return 1;
    }
    else if ((op= one_opt(av[i], STRING_WITH_LEN("--max-char="))))
    {
      if (get_option_codepoint(&opt->max_char, av[i], op))
        return 1;
    }
    else if ((op= one_opt(av[i], STRING_WITH_LEN("--print-toupper="))))
    {
      if (get_option_bool(&opt->mode.print_toupper, av[i], op))
        return 1;
    }
    else if ((op= one_opt(av[i], STRING_WITH_LEN("--print-tolower="))))
    {
      if (get_option_bool(&opt->mode.print_tolower, av[i], op))
        return 1;
    }
    else if ((op= one_opt(av[i], STRING_WITH_LEN("--print-noaccent-toupper="))))
    {
      if (get_option_bool(&opt->mode.print_noaccent_toupper, av[i], op))
        return 1;
    }
    else if ((op= one_opt(av[i], STRING_WITH_LEN("--page-name="))))
    {
      opt->mode.page_name= op;
    }
    else if ((op= one_opt(av[i], STRING_WITH_LEN("--page-name-derived="))))
    {
      opt->mode.page_name_derived= op;
    }
    else if ((op= one_opt(av[i], STRING_WITH_LEN("--index-name="))))
    {
      opt->mode.index_name= op;
    }
    else
    {
      if (av[i][0] == '-' && av[i][1] == '-')
      {
        fprintf(stderr, "Unknown option: %s\n", av[i]);
        return 1;
      }
      break;
    }
  }
  if (i + 1 != ac)
    return usage(ac, av);
  opt->filename= av[i];
  return 0;
}


int main(int ac,char **av)
{
  UNIDATA_OPT opt= opt_caseinfo;

  if (process_param(&opt, ac, av))
    return 1;

  memset(code,0,sizeof(code));
  memset(pstat,0,sizeof(pstat));

  set_default_case_folding(&opt, code);

  fill_cjk(code);

  if (load_unidata(&opt, code))
    return 1;

  set_noaccent(&opt, code);
  set_noaccent_tolower(&opt, code);
  set_noaccent_toupper(&opt, code);

  /*
    Bug#8385: utf8_general_ci treats cyrillic letters I and SHORT I as the same
    Because of decomposition applied, noaccent_toupper for the following letters:
      U+0419 CYRILLIC CAPITAL LETTER SHORT I
      U+0439 CYRILLIC SMALL LETTER SHORT I
    was set to:
      U+418 CYRILLIC CAPITAL LETTER I
    Reset it back to U+0419.
  */
  code[0x0419].noaccent_toupper= 0x0419;
  code[0x0439].noaccent_toupper= 0x0419;

  /*
    Bug#27877 incorrect german order in utf8_general_ci
  */
  if (strcmp(opt.mode.mode_name, "weight_general_mysql500_ci"))
  {
    code[0x00DF].noaccent_toupper= code['s'].noaccent_toupper;
  }
  else
    pstat[0].page_overridden= 1;

  if (!strcmp(opt.mode.mode_name, "casefold-tr"))
  {
    code[0x49].tolower= 0x0131;
    code[0x69].toupper= 0x0130;
    pstat[0].page_overridden= 1;
  }

  calc_page_parameters(&opt, code, pstat);

  printf("%s\n", COPYING);
  print_command_line_options(ac, av);
  print(&opt, code, pstat);

  return 0;
}
