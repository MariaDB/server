/* Copyright (c) 2000-2003, 2005-2007 MySQL AB, 2009 Sun Microsystems, Inc.
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
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "strings_def.h"
#include <m_ctype.h>
#include <fcntl.h>
#include <my_xml.h>

#define ROW_LEN		16
#define ROW16_LEN	8
#define MAX_BUF		(64*1024)


#define MY_ALL_CHARSETS_SIZE 2048

static struct charset_info_st all_charsets[MY_ALL_CHARSETS_SIZE];
static uint refids[MY_ALL_CHARSETS_SIZE];

static CHARSET_INFO *inheritance_source(uint id)
{
  return &all_charsets[refids[id]];
}


void
print_array(FILE *f, const char *set, const char *name, const uchar *a, int n)
{
  int i;

  fprintf(f,"static const uchar %s_%s[] = {\n", name, set);
  
  for (i=0 ;i<n ; i++)
  {
    fprintf(f,"0x%02X",a[i]);
    fprintf(f, (i+1<n) ? "," :"" );
    fprintf(f, ((i+1) % ROW_LEN == n % ROW_LEN) ? "\n" : "" );
  }
  fprintf(f,"};\n\n");
}


void
print_array16(FILE *f, const char *set, const char *name, const uint16 *a, int n)
{
  int i;

  fprintf(f,"static const uint16 %s_%s[] = {\n", name, set);
  
  for (i=0 ;i<n ; i++)
  {
    fprintf(f,"0x%04X",a[i]);
    fprintf(f, (i+1<n) ? "," :"" );
    fprintf(f, ((i+1) % ROW16_LEN == n % ROW16_LEN) ? "\n" : "" );
  }
  fprintf(f,"};\n\n");
}


static uint get_collation_number(const char *name)
{
  CHARSET_INFO *cs;
  for (cs= all_charsets;
       cs < all_charsets + array_elements(all_charsets);
       cs++)
  {
    if (cs->name && !strcmp(cs->name, name))
      return cs->number;
  }
  return 0;
}


static uint
get_charset_number_internal(const char *charset_name, uint cs_flags)
{
  CHARSET_INFO *cs;
  for (cs= all_charsets;
       cs < all_charsets + array_elements(all_charsets);
       cs++)
  {
    if (cs->csname && (cs->state & cs_flags) &&
        !strcmp(cs->csname, charset_name))
      return cs->number;
  }  
  return 0;
}

char *mdup(const char *src, uint len)
{
  char *dst=(char*)malloc(len);
  if (!dst)
    exit(1);
  memcpy(dst,src,len);
  return dst;
}

static void simple_cs_copy_data(struct charset_info_st *to, CHARSET_INFO *from)
{
  to->number= from->number ? from->number : to->number;
  to->state|= from->state;

  if (from->csname)
    to->csname= strdup(from->csname);
  
  if (from->name)
    to->name= strdup(from->name);
  
  if (from->ctype)
    to->ctype= (uchar*) mdup((char*) from->ctype, MY_CS_CTYPE_TABLE_SIZE);
  if (from->to_lower)
    to->to_lower= (uchar*) mdup((char*) from->to_lower, MY_CS_TO_LOWER_TABLE_SIZE);
  if (from->to_upper)
    to->to_upper= (uchar*) mdup((char*) from->to_upper, MY_CS_TO_UPPER_TABLE_SIZE);
  if (from->sort_order)
  {
    to->sort_order= (uchar*) mdup((char*) from->sort_order, MY_CS_SORT_ORDER_TABLE_SIZE);
    /*
      set_max_sort_char(to);
    */
  }
  if (from->tab_to_uni)
  {
    uint sz= MY_CS_TO_UNI_TABLE_SIZE*sizeof(uint16);
    to->tab_to_uni= (uint16*)  mdup((char*)from->tab_to_uni, sz);
    /*
    create_fromuni(to);
    */
  }
}


static void inherit_data(struct charset_info_st *cs, CHARSET_INFO *refcs)
{
  if (refcs->ctype &&
      !memcmp(cs->ctype, refcs->ctype, MY_CS_CTYPE_TABLE_SIZE))
    cs->ctype= NULL;
  if (refcs->to_lower &&
      !memcmp(cs->to_lower, refcs->to_lower, MY_CS_TO_LOWER_TABLE_SIZE))
     cs->to_lower= NULL;
  if (refcs->to_upper &&
      !memcmp(cs->to_upper, refcs->to_upper, MY_CS_TO_LOWER_TABLE_SIZE))
    cs->to_upper= NULL;
  if (refcs->tab_to_uni &&
      !memcmp(cs->tab_to_uni, refcs->tab_to_uni,
              MY_CS_TO_UNI_TABLE_SIZE * sizeof(uint16)))
    cs->tab_to_uni= NULL;
}


static my_bool simple_cs_is_full(CHARSET_INFO *cs)
{
  return ((cs->csname && cs->tab_to_uni && cs->ctype && cs->to_upper &&
	   cs->to_lower) &&
	  (cs->number && cs->name && 
	  (cs->sort_order || (cs->state & MY_CS_BINSORT))));
}

static int add_collation(struct charset_info_st *cs)
{
  if (cs->name &&
      (cs->number || (cs->number= get_collation_number(cs->name))))
  {
    if (!(all_charsets[cs->number].state & MY_CS_COMPILED))
    {
      simple_cs_copy_data(&all_charsets[cs->number],cs);
      
    }
    
    cs->number= 0;
    cs->name= NULL;
    cs->state= 0;
    cs->sort_order= NULL;
    cs->state= 0;
  }
  return MY_XML_OK;
}


static void
default_reporter(enum loglevel level  __attribute__ ((unused)),
                 const char *format  __attribute__ ((unused)),
                 ...)
{
}


static void
my_charset_loader_init(MY_CHARSET_LOADER *loader)
{
  loader->error[0]= '\0';
  loader->once_alloc= malloc;
  loader->malloc= malloc;
  loader->realloc= realloc;
  loader->free= free;
  loader->reporter= default_reporter;
  loader->add_collation= add_collation;
}


static int my_read_charset_file(const char *filename)
{
  char buf[MAX_BUF];
  int  fd;
  uint len;
  MY_CHARSET_LOADER loader;
  
  my_charset_loader_init(&loader);
  if ((fd=open(filename,O_RDONLY)) < 0)
  {
    fprintf(stderr,"Can't open '%s'\n",filename);
    return 1;
  }
  
  len=read(fd,buf,MAX_BUF);
  DBUG_ASSERT(len < MAX_BUF);
  close(fd);
  
  if (my_parse_charset_xml(&loader, buf, len))
  {
    fprintf(stderr, "Error while parsing '%s': %s\n", filename, loader.error);
    exit(1);
  }
  
  return FALSE;
}


void print_arrays(FILE *f, CHARSET_INFO *cs)
{
  if (cs->ctype)
    print_array(f, cs->name, "ctype",      cs->ctype,      MY_CS_CTYPE_TABLE_SIZE);
  if (cs->to_lower)
    print_array(f, cs->name, "to_lower",   cs->to_lower,   MY_CS_TO_LOWER_TABLE_SIZE);
  if (cs->to_upper)
    print_array(f, cs->name, "to_upper",   cs->to_upper,   MY_CS_TO_UPPER_TABLE_SIZE);
  if (cs->sort_order)
    print_array(f, cs->name, "sort_order", cs->sort_order, MY_CS_SORT_ORDER_TABLE_SIZE);
  if (cs->tab_to_uni)
    print_array16(f, cs->name, "to_uni",     cs->tab_to_uni, MY_CS_TO_UNI_TABLE_SIZE);
}


void dispcset(FILE *f,CHARSET_INFO *cs)
{
  fprintf(f,"{\n");
  fprintf(f,"  %d,%d,%d,\n",cs->number,0,0);
  fprintf(f,"  MY_CS_COMPILED%s%s%s%s%s,\n",
          cs->state & MY_CS_BINSORT         ? "|MY_CS_BINSORT"   : "",
          cs->state & MY_CS_PRIMARY         ? "|MY_CS_PRIMARY"   : "",
          cs->state & MY_CS_CSSORT          ? "|MY_CS_CSSORT"    : "",
          cs->state & MY_CS_PUREASCII       ? "|MY_CS_PUREASCII" : "",
          cs->state & MY_CS_NONASCII        ? "|MY_CS_NONASCII"  : "");
  
  if (cs->name)
  {
    CHARSET_INFO *srccs= inheritance_source(cs->number);
    fprintf(f,"  \"%s\",                     /* cset name     */\n",cs->csname);
    fprintf(f,"  \"%s\",                     /* coll name     */\n",cs->name);
    fprintf(f,"  \"\",                       /* comment       */\n");
    fprintf(f,"  NULL,                       /* tailoring     */\n");

    fprintf(f,"  ctype_%s,                   /* ctype         */\n",
            cs->ctype ? cs->name : srccs->name);
    fprintf(f,"  to_lower_%s,                /* lower         */\n",
            cs->to_lower ? cs->name : srccs->name);
    fprintf(f,"  to_upper_%s,                /* upper         */\n",
            cs->to_upper ? cs->name : srccs->name);
    if (cs->sort_order)
      fprintf(f,"  sort_order_%s,            /* sort_order    */\n",cs->name);
    else
      fprintf(f,"  NULL,                     /* sort_order    */\n");
    fprintf(f,"  NULL,                       /* uca           */\n");
    fprintf(f,"  to_uni_%s,                  /* to_uni        */\n",
            cs->tab_to_uni ? cs->name : srccs->name);
  }
  else
  {
    fprintf(f,"  NULL,                       /* cset name     */\n");
    fprintf(f,"  NULL,                       /* coll name     */\n");
    fprintf(f,"  NULL,                       /* comment       */\n");
    fprintf(f,"  NULL,                       /* tailoging     */\n");
    fprintf(f,"  NULL,                       /* ctype         */\n");
    fprintf(f,"  NULL,                       /* lower         */\n");
    fprintf(f,"  NULL,                       /* upper         */\n");
    fprintf(f,"  NULL,                       /* sort order    */\n");
    fprintf(f,"  NULL,                       /* uca           */\n");
    fprintf(f,"  NULL,                       /* to_uni        */\n");
  }

  fprintf(f,"  NULL,                       /* from_uni      */\n");
  fprintf(f,"  &my_unicase_default,        /* caseinfo      */\n");
  fprintf(f,"  NULL,                       /* state map     */\n");
  fprintf(f,"  NULL,                       /* ident map     */\n");
  fprintf(f,"  1,                          /* strxfrm_multiply*/\n");
  fprintf(f,"  1,                          /* caseup_multiply*/\n");
  fprintf(f,"  1,                          /* casedn_multiply*/\n");
  fprintf(f,"  1,                          /* mbminlen      */\n");
  fprintf(f,"  1,                          /* mbmaxlen      */\n");
  fprintf(f,"  0,                          /* min_sort_char */\n");
  fprintf(f,"  255,                        /* max_sort_char */\n");
  fprintf(f,"  ' ',                        /* pad_char      */\n");
  fprintf(f,"  0,                          /* escape_with_backslash_is_dangerous */\n");
  fprintf(f,"  1,                          /* levels_for_order   */\n");
  fprintf(f,"  &my_charset_8bit_handler,\n");
  if (cs->state & MY_CS_BINSORT)
    fprintf(f,"  &my_collation_8bit_bin_handler,\n");
  else
    fprintf(f,"  &my_collation_8bit_simple_ci_handler,\n");
  fprintf(f,"}\n");
}


static void
fprint_copyright(FILE *file)
{
  fprintf(file,
"/* Copyright 2000-2008 MySQL AB, 2008 Sun Microsystems, Inc.\n"
"   Copyright (c) 2000, 2011, Oracle and/or its affiliates.\n"
"   Copyright 2008-2016 MariaDB Corporation\n"
"\n"
"   This program is free software; you can redistribute it and/or modify\n"
"   it under the terms of the GNU General Public License as published by\n"
"   the Free Software Foundation; version 2 of the License.\n"
"\n"
"   This program is distributed in the hope that it will be useful,\n"
"   but WITHOUT ANY WARRANTY; without even the implied warranty of\n"
"   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the\n"
"   GNU General Public License for more details.\n"
"\n"
"   You should have received a copy of the GNU General Public License\n"
"   along with this program; if not, write to the Free Software\n"
"   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA */\n"
"\n");
}


int
main(int argc, char **argv  __attribute__((unused)))
{
  struct charset_info_st ncs, *cs;
  char filename[256];
  FILE *f= stdout;
  
  if (argc < 2)
  {
    fprintf(stderr, "usage: %s source-dir\n", argv[0]);
    exit(EXIT_FAILURE);
  }
  
  bzero((void*)&ncs,sizeof(ncs));
  bzero((void*)&all_charsets,sizeof(all_charsets));
  bzero((void*) refids, sizeof(refids));
  
  sprintf(filename,"%s/%s",argv[1],"Index.xml");
  my_read_charset_file(filename);
  
  for (cs= all_charsets;
       cs < all_charsets + array_elements(all_charsets);
       cs++)
  {
    if (cs->number && !(cs->state & MY_CS_COMPILED))
    {
      if ( (!simple_cs_is_full(cs)) && (cs->csname))
      {
        sprintf(filename,"%s/%s.xml",argv[1],cs->csname);
        my_read_charset_file(filename);
      }
      cs->state|= MY_CS_LOADED;
    }
  }
  
  fprintf(f, "/*\n");
  fprintf(f, "  This file was generated by the conf_to_src utility. "
          "Do not edit it directly,\n");
  fprintf(f, "  edit the XML definitions in sql/share/charsets/ instead.\n\n");
  fprintf(f, "  To re-generate, run the following in the strings/ "
          "directory:\n");
  fprintf(f, "    ./conf_to_src ../sql/share/charsets/ > FILE\n");
  fprintf(f, "*/\n\n");
  fprint_copyright(f);
  fprintf(f,"#include \"strings_def.h\"\n");
  fprintf(f,"#include <m_ctype.h>\n\n");
  
  
  for (cs= all_charsets;
       cs < all_charsets + array_elements(all_charsets);
       cs++)
  {
    if (cs->state & MY_CS_LOADED)
    {
      uint refid= get_charset_number_internal(cs->csname, MY_CS_PRIMARY);
      cs->state|= my_8bit_charset_flags_from_data(cs) |
                  my_8bit_collation_flags_from_data(cs);
      if (refid && cs->number != refid)
      {
        CHARSET_INFO *refcs= &all_charsets[refid];
        refids[cs->number]= refid;
        inherit_data(cs, refcs);
      }
      fprintf(f,"#ifdef HAVE_CHARSET_%s\n",cs->csname);
      print_arrays(f, cs);
      fprintf(f,"#endif\n");
      fprintf(f,"\n");
    }
  }
  
  fprintf(f,"struct charset_info_st compiled_charsets[] = {\n");
  for (cs= all_charsets;
       cs < all_charsets + array_elements(all_charsets);
       cs++)
  {
    if (cs->state & MY_CS_LOADED)
    {
      fprintf(f,"#ifdef HAVE_CHARSET_%s\n",cs->csname);
      dispcset(f,cs);
      fprintf(f,",\n");
      fprintf(f,"#endif\n");
    }
  }
  
  dispcset(f,&ncs);
  fprintf(f,"};\n");
  
  return 0;
}
