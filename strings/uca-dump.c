/* Copyright (c) 2004, 2006 MySQL AB
   Copyright (c) 2009-2011, Monty Program Ab
   Use is subject to license terms.
   Copyright (c) 2009-2011, Monty Program Ab

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1335  USA */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "my_global.h"
#include "m_ctype.h"
#include "ctype-uca.h"


#define MAX_ALLOWED_CODE 0x10FFFF


typedef struct opt_st
{
  const char *name_prefix; /* Name that goes into all array names */
  const char *filename;    /* The filename or "-" for stdin */
  uint levels;             /* The number of levels to dump */
  my_bool no_contractions;
  my_bool case_first_upper;
} OPT;


static OPT defaults=
{
  "uca",
  "-",
  3,
  FALSE,
  FALSE
};


typedef struct my_ducet_weight_st
{
  uint16 weight[4][MY_UCA_MAX_WEIGHT_SIZE];
  size_t weight_length;
} MY_DUCET_WEIGHT;


typedef struct my_ducet_single_char_t
{
  MY_DUCET_WEIGHT weight;
  my_bool is_variable;
} MY_DUCET_SINGLE_CHAR;


typedef struct my_ducet_char_t
{
  my_wc_t wc[MY_UCA_MAX_CONTRACTION];
  size_t length;
} MY_DUCET_CHARS;


typedef struct my_ducet_contraction_t
{
  MY_DUCET_CHARS chars;
  MY_DUCET_WEIGHT weights;
} MY_DUCET_CONTRACTION;


typedef struct my_ducet_contraction_list_st
{
  size_t nitems;
  MY_DUCET_CONTRACTION item[4*1024];
} MY_DUCET_CONTRACTION_LIST;


typedef struct my_ducet_logical_posision_st
{
  my_wc_t first;
  my_wc_t last;
} MY_DUCET_LOGICAL_POSITION;


typedef struct my_ducet_logical_positions_st
{
  MY_DUCET_LOGICAL_POSITION tertiary_ignorable;
  MY_DUCET_LOGICAL_POSITION secondary_ignorable;
  MY_DUCET_LOGICAL_POSITION primary_ignorable;
  MY_DUCET_LOGICAL_POSITION variable;
  MY_DUCET_LOGICAL_POSITION non_ignorable;
} MY_DUCET_LOGICAL_POSITIONS;


typedef struct my_allkeys_st
{
  MY_DUCET_SINGLE_CHAR single_chars[MAX_ALLOWED_CODE+1];
  MY_DUCET_CONTRACTION_LIST contractions;
  MY_DUCET_LOGICAL_POSITIONS logical_positions;
  uint version;
  char version_str[32];
} MY_DUCET;


static int
my_ducet_weight_cmp_on_level(const MY_DUCET_WEIGHT *a,
                             const MY_DUCET_WEIGHT *b,
                             uint level)
{
  uint i;
  for (i= 0; i < array_elements(a->weight[level]); i++)
  {
    int diff= (int) a->weight[level][i] - (int) b->weight[level][i];
    if (diff)
      return diff;
  }
  return 0;
}


static int
my_ducet_weight_cmp(const MY_DUCET_WEIGHT *a,
                    const MY_DUCET_WEIGHT *b)
{
  uint level;
  for (level= 0; level < array_elements(a->weight); level++)
  {
    int diff= my_ducet_weight_cmp_on_level(a, b, level);
    if (diff)
      return diff;
  }
  return 0;
}


/*
"3.11 Logical Reset Positions" says:

The CLDR table (based on UCA) has the following overall structure for weights,
going from low to high.

*/

static my_bool
my_ducet_weight_is_tertiary_ignorable(const MY_DUCET_WEIGHT *w)
{
  return w->weight[0][0] == 0 &&
         w->weight[1][0] == 0 &&
         w->weight[2][0] == 0;
}


static my_bool
my_ducet_weight_is_secondary_ignorable(const MY_DUCET_WEIGHT *w)
{
  return w->weight[0][0] == 0 &&
         w->weight[1][0] == 0 &&
         w->weight[2][0] != 0;
}


static my_bool
my_ducet_weight_is_primary_ignorable(const MY_DUCET_WEIGHT *w)
{
  return w->weight[0][0] == 0 &&
         w->weight[1][0] != 0 &&
         w->weight[2][0] != 0;
}


static my_bool
my_ducet_weight_is_primary_non_ignorable(const MY_DUCET_WEIGHT *w)
{
  return w->weight[0][0] > 0 && w->weight[0][0] < 0xFB00;
}


/*
  if alternate = non-ignorable
  p != ignore,
  if  alternate = shifted
  p, s, t = ignore
*/
static my_bool
my_ducet_single_char_is_variable(const MY_DUCET_SINGLE_CHAR *ch)
{
  return ch->is_variable &&
         my_ducet_weight_is_primary_non_ignorable(&ch->weight);
}


static void
my_ducet_logical_position_set(MY_DUCET_LOGICAL_POSITION *dst, my_wc_t wc)
{
  dst->first= dst->last= wc;
}


static void
my_ducet_logical_position_update(MY_DUCET_LOGICAL_POSITION *dst,
                                 const MY_DUCET *ducet, my_wc_t current)
{
  const MY_DUCET_SINGLE_CHAR *chars= ducet->single_chars;
  int diff;
  if (current >= array_elements(ducet->single_chars))
    return;
  if ((diff= my_ducet_weight_cmp(&chars[current].weight,
                                 &chars[dst->first].weight)) < 0 ||
      (diff == 0 && current < dst->first))
    dst->first= current;
  if ((diff= my_ducet_weight_cmp(&chars[current].weight,
                                 &chars[dst->last].weight)) > 0 ||
      (diff == 0 && current > dst->last))
    dst->last= current;
}


static void
my_ducet_logical_positions_init(MY_DUCET_LOGICAL_POSITIONS *dst,
                                const MY_DUCET *ducet)
{
  uint i;
  const MY_DUCET_SINGLE_CHAR *chars= ducet->single_chars;

  for (i= 0; i < array_elements(ducet->single_chars); i++)
  {
    if (my_ducet_weight_is_tertiary_ignorable(&chars[i].weight))
    {
      my_ducet_logical_position_set(&dst->tertiary_ignorable, i);
      break;
    }
  }

  for (i= 0; i < array_elements(ducet->single_chars); i++)
  {
    if (my_ducet_weight_is_secondary_ignorable(&chars[i].weight))
    {
      my_ducet_logical_position_set(&dst->secondary_ignorable, i);
      break;
    }
  }

  for (i= 0; i < array_elements(ducet->single_chars); i++)
  {
    if (my_ducet_weight_is_primary_ignorable(&chars[i].weight))
    {
      my_ducet_logical_position_set(&dst->primary_ignorable, i);
      break;
    }
  }

  for (i= 0; i < array_elements(ducet->single_chars); i++)
  {
    if (my_ducet_weight_is_primary_non_ignorable(&chars[i].weight))
    {
      my_ducet_logical_position_set(&dst->non_ignorable, i);
      break;
    }
  }

  for (i= 0; i < array_elements(ducet->single_chars); i++)
  {
    if (my_ducet_single_char_is_variable(&chars[i]))
    {
      my_ducet_logical_position_set(&dst->variable, i);
      break;
    }
  }

  for (i= 1; i < array_elements(ducet->single_chars); i++)
  {
    if (my_ducet_weight_is_primary_non_ignorable(&chars[i].weight))
      my_ducet_logical_position_update(&dst->non_ignorable, ducet, i);
    if (my_ducet_weight_is_primary_ignorable(&chars[i].weight))
      my_ducet_logical_position_update(&dst->primary_ignorable, ducet, i);
    if (my_ducet_weight_is_secondary_ignorable(&chars[i].weight))
      my_ducet_logical_position_update(&dst->secondary_ignorable, ducet, i);
    if (my_ducet_weight_is_tertiary_ignorable(&chars[i].weight))
      my_ducet_logical_position_update(&dst->tertiary_ignorable, ducet, i);
    if (my_ducet_single_char_is_variable(&chars[i]))
      my_ducet_logical_position_update(&dst->variable, ducet, i);
  }

  /*
    DUCET as of Unicode-14.0.0 does not have any secondary ignorable
    characters, i.e. with weights [p=0000, s=0000, t!=0000]
    For compatibility with 4.0.0 and 5.2.0 data in ctype-uca.c,
    let copy tertiary_ignorable to secondary_ignorable.
    It gives effectively the same result with just leaving
    secondary_ignorable as {first=U+0000,last=U+0000}.
  */
  if (dst->secondary_ignorable.first == 0 && dst->secondary_ignorable.last == 0)
  {
    dst->secondary_ignorable.first= dst->tertiary_ignorable.first;
    dst->secondary_ignorable.last= dst->tertiary_ignorable.last;
  }
}


static void
my_ducet_weight_normalize_on_level(MY_DUCET_WEIGHT *weights,
                                   uint level,
                                   const OPT *options)
{
  uint dst, src;
  for (src= 0, dst= 0; src < array_elements(weights->weight[level]); src++)
  {
    if (weights->weight[level][src] != 0)
      weights->weight[level][dst++]= weights->weight[level][src];
  }
  for ( ; dst < array_elements(weights->weight[level]) ; dst++)
    weights->weight[level][dst]= 0;
  if (options->case_first_upper && level == 2)
  {
    /*
      Invert weights for secondary level to
      sort upper case letters before their
      lower case counter part.
    */
    for (dst= 0; dst < array_elements(weights->weight[level]); dst++)
    {
      if (weights->weight[level][dst] == 0)
        break;
      if (weights->weight[level][dst] >= 0x20)
      {
        fprintf(stderr, "Secondary level is too large: %04X\n",
                (int) weights->weight[level][dst]);
      }
      weights->weight[level][dst]= (uint16) (0x20 - weights->weight[level][dst]);
    }
  }
}


static void
my_ducet_weight_normalize(MY_DUCET_WEIGHT *weights, const OPT *options)
{
  uint i;
  for (i= 0; i < array_elements(weights->weight); i++)
    my_ducet_weight_normalize_on_level(weights, i, options);
}


static void
my_ducet_normalize(MY_DUCET *ducet, const OPT *options)
{
  uint i;
  for (i= 0; i < array_elements(ducet->single_chars); i++)
    my_ducet_weight_normalize(&ducet->single_chars[i].weight, options);
  for (i= 0; i < array_elements(ducet->contractions.item); i++)
    my_ducet_weight_normalize(&ducet->contractions.item[i].weights, options);
}


static my_bool
my_ducet_contraction_list_add(MY_DUCET_CONTRACTION_LIST *dst,
                              const MY_DUCET_CHARS *chars,
                              const MY_DUCET_WEIGHT *weights)
{
  if (dst->nitems >= array_elements(dst->item))
  {
    fprintf(stderr, "Too many contractions\n");
    return TRUE;
  }
  dst->item[dst->nitems].chars= *chars;
  dst->item[dst->nitems].weights= *weights;
  dst->nitems++;
  return FALSE;
}


#if 0
#define MY_UCA_NPAGES	1024
#define MY_UCA_NCHARS	64
#define MY_UCA_CMASK	63
#define MY_UCA_PSHIFT	6
#else
#define MY_UCA_NPAGES	4352 /* 0x110000 characters / 0x100 chars per page */
#define MY_UCA_NCHARS	256
#define MY_UCA_CMASK	255
#define MY_UCA_PSHIFT	8
#endif



/* Name prefix that goes into page weight array names after global_name_prefix */
static const char *pname_prefix[]= {"_p", "_p", "_p"};

/* Name suffix that goes into page weight array names after page number */
static const char *pname_suffix[]= {"", "_secondary", "_tertiary"};


void usage(const char *prog)
{
  printf("Usage:\n");
  printf("%s [options] filename\n", prog);
}


static inline int lstrncmp(const char *str, const LEX_CSTRING lstr)
{
  return strncmp(lstr.str, str, lstr.length);
}


int process_option(OPT *options, const char *opt)
{
  static const LEX_CSTRING opt_name_prefix= {STRING_WITH_LEN("--name-prefix=")};
  static const LEX_CSTRING opt_levels= {STRING_WITH_LEN("--levels=")};
  static const LEX_CSTRING opt_no_contractions= {STRING_WITH_LEN("--no-contractions")};
  static const LEX_CSTRING opt_case_first= {STRING_WITH_LEN("--case-first=")};
  if (!lstrncmp(opt, opt_name_prefix))
  {
    options->name_prefix= opt + opt_name_prefix.length;
    return 0;
  }
  if (!lstrncmp(opt, opt_levels))
  {
    options->levels= (uint) strtoul(opt + opt_levels.length, NULL, 10);
    if (options->levels < 1 || options->levels > 3)
    {
      printf("Bad --levels value\n");
      return 1;
    }
    return 0;
  }
  if (!lstrncmp(opt, opt_case_first))
  {
    const char *value= opt + opt_case_first.length;
    if (!strcasecmp(value, "upper"))
    {
      options->case_first_upper= TRUE;
      return 0;
    }
    if (!strcasecmp(value, "lower"))
    {
      options->case_first_upper= FALSE;
      return 0;
    }
    fprintf(stderr, "Bad option: %s\n", opt);
    return 1;
  }
  if (!strcmp(opt, opt_no_contractions.str))
  {
    options->no_contractions= TRUE;
    return 0;
  }
  printf("Unknown option: %s\n", opt);
  return 1;
}


int process_options(OPT *options, int ac, char **av)
{
  int i;
  for (i= 1; i < ac; i++)
  {
    if (!strncmp(av[i], "--", 2))
    {
      if (process_option(options, av[i]))
        return 1;
    }
    else
    {
      if (i + 1 != ac)
      {
        usage(av[0]);
        return 1;
      }
      options->filename= av[i];
      return 0;
    }
  }
  usage(av[0]);
  return 1;
}


FILE *open_file(const char *name)
{
  if (!strcmp(name, "-"))
    return stdin;
  return fopen(name, "r");
}


void close_file(FILE *file)
{
  if (file != stdin)
    fclose(file);
}


char *strrtrim(char *str)
{
  char *end= str + strlen(str);
  for ( ; str < end; end--)
  {
    if (end[-1] != '\r' && end[-1] != '\n' &&
        end[-1] != ' '  && end[-1] != '\t')
      break;
    end[-1]= '\0';
  }
  return str;
}


/*
  Parse a line starting with '@'.
  As of 14.0.0, allkeys.txt has @version and @implicitweights lines.
  Only @version is parsed here.

  It could also be possible to parse @implicitweights to automatically
  generate routines responsible for implicit weight handling for Siniform
  ideographic scripts (Tangut, Nushu, Khitan). But as there are only a few
  of them at the moment, it was easier to write these routines in ctype-uca.h
  manually. So @implicitweights lines are ignored here.
*/
my_bool parse_at_line(MY_DUCET *ducet, const char *str)
{
  static const LEX_CSTRING version= {STRING_WITH_LEN("@version ")};
  if (!lstrncmp(str, version))
  {
    /*
      Examples:
        @version 4.0.0
        @version 5.2.0
        @version 14.0.0
    */
    const char *src= str + version.length;
    long n[3]= {0};
    uint pos;
    int length;

    length= snprintf(ducet->version_str, sizeof(ducet->version_str)-1,
                     "%s", src);
    ducet->version_str[length]= '\0';

    for (pos= 0 ; pos < 3; pos++)
    {
      char *endptr;
      n[pos]= strtol(src, &endptr, 10);
      if (*endptr != '.' && *endptr != '\r' && *endptr != '\n' && *endptr != 0)
        return TRUE;
      src= endptr + 1;
    }
    ducet->version= MY_UCA_VERSION_ID(n[0], n[1], n[2]);
  }
  return FALSE;
}


static void
parse_chars(MY_DUCET_CHARS *dst, char *str)
{
  char *s;
  const char *delim= " \t";
  dst->length= 0;
  for (s= strtok(str, delim); s ; s= strtok(NULL, delim))
  {
    my_wc_t code= (my_wc_t) strtoul(s, NULL, 16);
    if (dst->length < array_elements(dst->wc))
      dst->wc[dst->length]= code;
    dst->length++;
  }
}


static void
parse_weights(MY_DUCET_WEIGHT *dst, my_bool *is_variable, char *weight)
{
  const char *delim= " []";
  size_t w;
  char *weights[64];
  char *s;
  dst->weight_length= 0;
  *is_variable= FALSE;
  for (s= strtok(weight, delim) ; s ; s= strtok(NULL, delim))
  {
    if (dst->weight_length < array_elements(weights))
      weights[dst->weight_length]= s;
    dst->weight_length++;
  }

  set_if_smaller(dst->weight_length, MY_UCA_MAX_WEIGHT_SIZE-1);

  for (w= 0; w < dst->weight_length ; w++)
  {
    size_t partnum= 0;
    for (s= weights[w]; *s ;)
    {
      char *endptr;
      uint part= (uint) strtoul(s + 1, &endptr, 16);
      if (w == 0 && s[0] == '*')
        *is_variable= TRUE;
      if (part > 0xFFFF)
        fprintf(stderr, "Weight is too large: %X\n", (uint) part);
      dst->weight[partnum][w]= (uint16) part;
      s= endptr;
      partnum++;
    }
  }
}


static void
print_one_logical_position(const OPT *options,
                       const char *name,
                       const char *name2,
                       my_wc_t value)
{
  printf("#define %s_%s%s 0x%04X\n",
         options->name_prefix, name, name2, (int) value);
}


static void
my_ducet_weight_print_canonical(const MY_DUCET_WEIGHT *src)
{
  uint i;
  for (i= 0; i < array_elements(src->weight[0]); i++)
  {
    my_bool zero= src->weight[0][i] == 0 &&
                  src->weight[1][i] == 0 &&
                  src->weight[2][i] == 0;
    if (zero && i > 0)
      break;
    printf("[.%04X.%04X.%04X]",
           src->weight[0][i],
           src->weight[1][i],
           src->weight[2][i]);
  }
}


static void
my_ducet_logical_position_print(const MY_DUCET_LOGICAL_POSITION *src,
                                const char *name,
                                const MY_DUCET *ducet,
                                const OPT *options)
{
  printf("/*\n");
  my_ducet_weight_print_canonical(&ducet->single_chars[src->first].weight);
  printf("\n");
  my_ducet_weight_print_canonical(&ducet->single_chars[src->last].weight);
  printf("\n*/\n");
  print_one_logical_position(options, name, "_first", src->first);
  print_one_logical_position(options, name, "_last", src->last);
  printf("\n");
}


static void
print_logical_positions(const MY_DUCET_LOGICAL_POSITIONS *src,
                        const MY_DUCET *ducet,
                        const OPT *opt)
{
  my_ducet_logical_position_print(&src->tertiary_ignorable, "tertiary_ignorable", ducet, opt);
  my_ducet_logical_position_print(&src->secondary_ignorable, "secondary_ignorable", ducet, opt);
  my_ducet_logical_position_print(&src->primary_ignorable, "primary_ignorable", ducet, opt);
  my_ducet_logical_position_print(&src->variable, "variable", ducet, opt);
  my_ducet_logical_position_print(&src->non_ignorable, "non_ignorable", ducet, opt);
}


static void
print_version(const MY_DUCET *ducet, const OPT *opt)
{
  printf("\n");
  printf("#define %s_version %d /* %s */\n",
         opt->name_prefix, ducet->version, ducet->version_str);
  printf("\n");
}


static void
print_contraction(const MY_DUCET_CONTRACTION *c,
                  uint level,
                  const OPT *options)
{
  size_t j;
  printf("{");
  printf("{");
  for (j= 0; j < array_elements(c->chars.wc); j++)
  {
    if (j > 0)
      printf(", ");
    if (c->chars.wc[j])
      printf("0x%04X", (uint) c->chars.wc[j]);
    else
    {
      printf("0");
      break;
    }
  }
  printf("}, ");
  printf("{");
  for (j= 0; j < array_elements(c->weights.weight[level]); j++)
  {
    if (j > 0)
      printf(", ");
    if (c->weights.weight[level][j])
      printf("0x%04X", (uint) c->weights.weight[level][j]);
    else
    {
      printf("0");
      break;
    }
  }
  printf("}, FALSE");
  printf("},\n");
}


static void
print_contraction_list(const MY_DUCET_CONTRACTION_LIST *src, uint level, const OPT *opt)
{
  size_t i;
  printf("\n\n/* Contractions, level %d */\n", level);
  printf("static MY_CONTRACTION %s_contractions%s[%d]={\n",
         opt->name_prefix, pname_suffix[level], (int) src->nitems);
  for (i= 0; i < src->nitems; i++)
  {
    const MY_DUCET_CONTRACTION *c= &src->item[i];
    print_contraction(c, level, opt);
  }
  printf("};\n\n");
}


int main(int ac, char **av)
{
  char str[1024];
  static MY_DUCET ducet;
  my_wc_t code;
  uint w;
  int pageloaded[MY_UCA_NPAGES];
  FILE *file;
  OPT options= defaults;

  if (process_options(&options, ac, av))
    return 1;

  if (!(file= open_file(options.filename)))
  {
    printf("Could not open %s for reading\n", options.filename);
    return 1;
  }

  bzero(&ducet, sizeof(ducet));
  bzero(pageloaded, sizeof(pageloaded));
  
  while (fgets(str, sizeof(str), file))
  {
    char *comment;
    char *weight;
    MY_DUCET_CHARS chr = {0};

    if (str[0] == '#')
      continue;

    if (str[0] == '@')
    {
      parse_at_line(&ducet, strrtrim(str));
      continue;
    }

    if ((weight= strchr(str, ';')))
    {
      *weight++= '\0';
      for ( ; *weight==' ' ; weight++);
    }
    else
      continue;

    if ((comment=strchr(weight, '#')))
    {
      *comment++= '\0';
    }else
      continue;

    parse_chars(&chr, str);
    if (!chr.length)
      continue;

    if (chr.length == 1)
    {
      if (chr.wc[0] > MAX_ALLOWED_CODE)
        continue;
      parse_weights(&ducet.single_chars[chr.wc[0]].weight,
                    &ducet.single_chars[chr.wc[0]].is_variable,
                    weight);
      /* Mark that a character from this page was loaded */
      pageloaded[chr.wc[0] >> MY_UCA_PSHIFT]++;
    }
    else
    {
      MY_DUCET_WEIGHT weights= {0};
      my_bool dummy;
      if (chr.length >= MY_UCA_MAX_CONTRACTION)
      {
        fprintf(stderr, "Too long contraction: %d\n", (int) chr.length);
        continue;
      }
      parse_weights(&weights, &dummy, weight);
      my_ducet_contraction_list_add(&ducet.contractions, &chr, &weights);
      continue;
    }
  }

  close_file(file);

  /* Now set implicit weights */
  for (code=0; code <= MAX_ALLOWED_CODE; code++)
  {
    uint level;

    if (ducet.single_chars[code].weight.weight_length)
      continue;

    for (level= 0; level < 4; level++)
    {
      MY_UCA_IMPLICIT_WEIGHT weight;
      weight= my_uca_implicit_weight_on_level(ducet.version, code, level);
      ducet.single_chars[code].weight.weight[level][0]= weight.weight[0];
      ducet.single_chars[code].weight.weight[level][1]= weight.weight[1];
    }
    ducet.single_chars[code].weight.weight_length= 2;
  }

  my_ducet_normalize(&ducet, &options);
  my_ducet_logical_positions_init(&ducet.logical_positions, &ducet);

  printf("/*\n");
  printf("  Generated from allkeys.txt version '%s'\n", ducet.version_str);
  printf("*/\n");

  for (w=0; w < options.levels; w++)
  {
    size_t page;
    int pagemaxlen[MY_UCA_NPAGES];

    for (page=0; page < MY_UCA_NPAGES; page++)
    {
      size_t offs;
      size_t maxnum= 0;
      size_t nchars= 0;
      size_t mchars;
      size_t ndefs= 0;
      size_t code_line_start= page * MY_UCA_NCHARS;
      
      pagemaxlen[page]= 0;
      
      /*
        Skip this page if no weights were loaded
      */
      
      if (!pageloaded[page])
        continue;
      
      /* 
        Calculate maximum weight
        length for this page
      */
      
      for (offs=0; offs < MY_UCA_NCHARS; offs++)
      {
        size_t i, num;
        
        code= page*MY_UCA_NCHARS+offs;
        
        /* Calculate only non-zero weights */
        for (num=0, i=0; i < ducet.single_chars[code].weight.weight_length; i++)
          if (ducet.single_chars[code].weight.weight[w][i])
            num++;
        
        maxnum= maxnum < num ? num : maxnum;
        
        /* Check if default weight */
        if (w == 1 && num == 1)
        {
          /* 0020 0000 ... */
          if (ducet.single_chars[code].weight.weight[w][0] == 0x0020)
            ndefs++;
        }
        else if (w == 2 && num == 1)
        {
          /* 0002 0000 ... */
          if (ducet.single_chars[code].weight.weight[w][0] == 0x0002)
            ndefs++;
        }
      } 
      maxnum++;
      
      /*
        If the page have only default weights
        then no needs to dump it, skip.
      */
      if (ndefs == MY_UCA_NCHARS)
      {
        continue;
      }
      switch (maxnum)
      {
        case 0: mchars= 8; break;
        case 1: mchars= 8; break;
        case 2: mchars= 8; break;
        case 3: mchars= 9; break;
        case 4: mchars= 8; break;
        default: mchars= ducet.single_chars[code].weight.weight_length;
      }
      
      pagemaxlen[page]= (int) maxnum;


      /*
        Now print this page
      */
      
      
      printf("static const uint16 %s%s%03X%s[]= { /* %04X (%d weights per char) */\n",
              options.name_prefix, pname_prefix[w], (int) page, pname_suffix[w],
              (int) page*MY_UCA_NCHARS, (int) maxnum);
      
      for (offs=0; offs < MY_UCA_NCHARS; offs++)
      {
        size_t i;
        
        code= page*MY_UCA_NCHARS+offs;
        
        for (i=0; i < maxnum; i++)
        {
          int tmp= ducet.single_chars[code].weight.weight[w][i];
          printf("0x%04X", tmp);
          if ((offs+1 != MY_UCA_NCHARS) || (i+1!=maxnum))
            printf(",");
          else
            printf(" ");
          nchars++;
        }
        if (nchars >=mchars)
        {
          printf(" /* %04X */\n", (int) code_line_start);
          code_line_start= code + 1;
          nchars=0;
        }
        else
        {
          printf(" ");
        }
      }
      printf("};\n\n");
    }

    printf("const uchar %s_length%s[%d]={\n",
           options.name_prefix, pname_suffix[w], MY_UCA_NPAGES);
    for (page=0; page < MY_UCA_NPAGES; page++)
    {
      printf("%d%s%s",pagemaxlen[page],page<MY_UCA_NPAGES-1?",":"",(page+1) % 16 ? "":"\n");
    }
    printf("};\n");


    printf("static const uint16 *%s_weight%s[%d]={\n",
           options.name_prefix, pname_suffix[w], MY_UCA_NPAGES);
    for (page=0; page < MY_UCA_NPAGES; page++)
    {
      const char *comma= page < MY_UCA_NPAGES-1 ? "," : "";
      const char *nline= (page+1) % 4 ? "" : "\n";
      if (!pagemaxlen[page])
        printf("NULL       %s%s%s", w ? " ": "",  comma , nline);
      else
        printf("%s%s%03X%s%s%s",
               options.name_prefix, pname_prefix[w], (int) page, pname_suffix[w],
               comma, nline);
    }
    printf("};\n");

    if (!options.no_contractions)
      print_contraction_list(&ducet.contractions, w, &options);
  }
  print_version(&ducet, &options);
  print_logical_positions(&ducet.logical_positions, &ducet, &options);
  
  return 0;
}
