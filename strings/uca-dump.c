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

struct uca_item_st
{
  uchar  num;
  uint16 weight[4][MY_UCA_MAX_WEIGHT_SIZE];
};

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

#define MAX_ALLOWED_CODE 0x10FFFF


typedef struct opt_st
{
  const char *name_prefix; /* Name that goes into all array names */
  const char *filename;    /* The filename or "-" for stdin */
  uint levels;             /* The number of levels to dump */
} OPT;


static OPT defaults=
{
  "uca",
  "-",
  3
};


/* Name prefix that goes into page weight array names after global_name_prefix */
static const char *pname_prefix[]= {"_p", "_p", "_p"};

/* Name suffix that goes into page weight array names after page number */
static const char *pname_suffix[]= {"", "_w2", "_w3"};


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


int main(int ac, char **av)
{
  char str[1024];
  char *weights[64];
  static struct uca_item_st uca[MAX_ALLOWED_CODE+1];
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

  bzero(uca, sizeof(uca));
  bzero(pageloaded, sizeof(pageloaded));
  
  while (fgets(str, sizeof(str), file))
  {
    char *comment;
    char *weight;
    char *s;
    size_t codenum;
    
    code= (my_wc_t) strtol(str,NULL,16);
    
    if (str[0]=='#' || (code > MAX_ALLOWED_CODE))
      continue;
    if ((comment=strchr(str,'#')))
    {
      *comment++= '\0';
      for ( ; *comment==' ' ; comment++);
    }else
      continue;
    
    if ((weight=strchr(str,';')))
    {
      *weight++= '\0';
      for ( ; *weight==' ' ; weight++);
    }
    else
      continue;
    
    codenum= 0;
    s= strtok(str, " \t");
    while (s)
    {
      s= strtok(NULL, " \t");
      codenum++;
    }
    
    if (codenum>1)
    {
      /* Multi-character weight, 
         i.e. contraction. 
         Not supported yet.
      */
      continue;
    }
    
    uca[code].num= 0;
    s= strtok(weight, " []");
    while (s)
    {
      weights[uca[code].num]= s;
      s= strtok(NULL, " []");
      uca[code].num++;
    }
    
    set_if_smaller(uca[code].num, MY_UCA_MAX_WEIGHT_SIZE-1);

    for (w=0; w < uca[code].num ; w++)
    {
      size_t partnum;
      
      partnum= 0;
      s= weights[w];
      while (*s)
      {
        char *endptr;
        uint part= (uint) strtoul(s + 1, &endptr, 16);
        uca[code].weight[partnum][w]= (uint16) part;
        s= endptr;
        partnum++;
      }
    }
    /* Mark that a character from this page was loaded */
    pageloaded[code >> MY_UCA_PSHIFT]++;
  }

  close_file(file);

  /* Now set implicit weights */
  for (code=0; code <= MAX_ALLOWED_CODE; code++)
  {
    uint level;

    if (uca[code].num)
      continue;

    for (level= 0; level < 4; level++)
    {
      MY_UCA_IMPLICIT_WEIGHT weight;
      weight= my_uca_520_implicit_weight_on_level(code, level);
      uca[code].weight[level][0]= weight.weight[0];
      uca[code].weight[level][1]= weight.weight[1];
    }
    uca[code].num= 2;
  }
  
  printf("#include \"my_uca.h\"\n");
  
  printf("#define MY_UCA_NPAGES %d\n",MY_UCA_NPAGES);
  printf("#define MY_UCA_NCHARS %d\n",MY_UCA_NCHARS);
  printf("#define MY_UCA_CMASK  %d\n",MY_UCA_CMASK);
  printf("#define MY_UCA_PSHIFT %d\n",MY_UCA_PSHIFT);

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
        for (num=0, i=0; i < uca[code].num; i++)
          if (uca[code].weight[w][i])
            num++;
        
        maxnum= maxnum < num ? num : maxnum;
        
        /* Check if default weight */
        if (w == 1 && num == 1)
        {
          /* 0020 0000 ... */
          if (uca[code].weight[w][0] == 0x0020)
            ndefs++;
        }
        else if (w == 2 && num == 1)
        {
          /* 0002 0000 ... */
          if (uca[code].weight[w][0] == 0x0002)
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
        default: mchars= uca[code].num;
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
        uint16 weight[MY_UCA_MAX_WEIGHT_SIZE];
        size_t num, i;
        
        code= page*MY_UCA_NCHARS+offs;
        
        bzero(weight,sizeof(weight));
        
        /* Copy non-zero weights */
        for (num=0, i=0; i < uca[code].num; i++)
        {
          if (uca[code].weight[w][i])
          {
            weight[num]= uca[code].weight[w][i];
            num++;
          }
        }
        
        for (i=0; i < maxnum; i++)
        {
          /* 
            Invert weights for secondary level to
            sort upper case letters before their
            lower case counter part.
          */
          int tmp= weight[i];
          if (w == 2 && tmp)
            tmp= (int)(0x20 - weight[i]);
          
          
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
  }

  
  printf("int main(void){ return 0;};\n");
  return 0;
}
