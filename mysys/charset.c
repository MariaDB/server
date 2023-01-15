/*
   Copyright (c) 2000, 2011, Oracle and/or its affiliates

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

#include "mysys_priv.h"
#include "mysys_err.h"
#include <m_ctype.h>
#include <m_string.h>
#include <my_dir.h>
#include <my_xml.h>
#ifdef HAVE_LANGINFO_H
#include <langinfo.h>
#endif
#ifdef HAVE_LOCALE_H
#include <locale.h>
#endif

/*
  The code below implements this functionality:
  
    - Initializing charset related structures
    - Loading dynamic charsets
    - Searching for a proper CHARSET_INFO 
      using charset name, collation name or collation ID
    - Setting server default character set
*/

my_bool my_charset_same(CHARSET_INFO *cs1, CHARSET_INFO *cs2)
{
  return ((cs1 == cs2) || !strcmp(cs1->csname,cs2->csname));
}


static uint
get_collation_number_internal(const char *name)
{
  CHARSET_INFO **cs;
  for (cs= all_charsets;
       cs < all_charsets + array_elements(all_charsets);
       cs++)
  {
    if ( cs[0] && cs[0]->name && 
         !my_strcasecmp(&my_charset_latin1, cs[0]->name, name))
      return cs[0]->number;
  }  
  return 0;
}


static my_bool is_multi_byte_ident(CHARSET_INFO *cs, uchar ch)
{
  int chlen= my_charlen(cs, (const char *) &ch, (const char *) &ch + 1);
  return MY_CS_IS_TOOSMALL(chlen) ? TRUE : FALSE;
}

static my_bool init_state_maps(struct charset_info_st *cs)
{
  uint i;
  uchar *state_map;
  uchar *ident_map;

  if (!(cs->state_map= state_map= (uchar*) my_once_alloc(256, MYF(MY_WME))))
    return 1;
    
  if (!(cs->ident_map= ident_map= (uchar*) my_once_alloc(256, MYF(MY_WME))))
    return 1;

  /* Fill state_map with states to get a faster parser */
  for (i=0; i < 256 ; i++)
  {
    if (my_isalpha(cs,i))
      state_map[i]=(uchar) MY_LEX_IDENT;
    else if (my_isdigit(cs,i))
      state_map[i]=(uchar) MY_LEX_NUMBER_IDENT;
    else if (is_multi_byte_ident(cs, i))
      state_map[i]=(uchar) MY_LEX_IDENT;
    else if (my_isspace(cs,i))
      state_map[i]=(uchar) MY_LEX_SKIP;
    else
      state_map[i]=(uchar) MY_LEX_CHAR;
  }
  state_map[(uchar)'_']=state_map[(uchar)'$']=(uchar) MY_LEX_IDENT;
  state_map[(uchar)'\'']=(uchar) MY_LEX_STRING;
  state_map[(uchar)'.']=(uchar) MY_LEX_REAL_OR_POINT;
  state_map[(uchar)'>']=state_map[(uchar)'=']=state_map[(uchar)'!']= (uchar) MY_LEX_CMP_OP;
  state_map[(uchar)'<']= (uchar) MY_LEX_LONG_CMP_OP;
  state_map[(uchar)'&']=state_map[(uchar)'|']=(uchar) MY_LEX_BOOL;
  state_map[(uchar)'#']=(uchar) MY_LEX_COMMENT;
  state_map[(uchar)';']=(uchar) MY_LEX_SEMICOLON;
  state_map[(uchar)':']=(uchar) MY_LEX_SET_VAR;
  state_map[0]=(uchar) MY_LEX_EOL;
  state_map[(uchar)'\\']= (uchar) MY_LEX_ESCAPE;
  state_map[(uchar)'/']= (uchar) MY_LEX_LONG_COMMENT;
  state_map[(uchar)'*']= (uchar) MY_LEX_END_LONG_COMMENT;
  state_map[(uchar)'@']= (uchar) MY_LEX_USER_END;
  state_map[(uchar) '`']= (uchar) MY_LEX_USER_VARIABLE_DELIMITER;
  state_map[(uchar)'"']= (uchar) MY_LEX_STRING_OR_DELIMITER;
  state_map[(uchar)'-']= (uchar) MY_LEX_MINUS_OR_COMMENT;
  state_map[(uchar)',']= (uchar) MY_LEX_COMMA;
  state_map[(uchar)'?']= (uchar) MY_LEX_PLACEHOLDER;

  /*
    Create a second map to make it faster to find identifiers
  */
  for (i=0; i < 256 ; i++)
  {
    ident_map[i]= (uchar) (state_map[i] == MY_LEX_IDENT ||
			   state_map[i] == MY_LEX_NUMBER_IDENT);
  }

  /* Special handling of hex and binary strings */
  state_map[(uchar)'x']= state_map[(uchar)'X']= (uchar) MY_LEX_IDENT_OR_HEX;
  state_map[(uchar)'b']= state_map[(uchar)'B']= (uchar) MY_LEX_IDENT_OR_BIN;
  state_map[(uchar)'n']= state_map[(uchar)'N']= (uchar) MY_LEX_IDENT_OR_NCHAR;
  return 0;
}


static MY_COLLATION_HANDLER *get_simple_collation_handler_by_flags(uint flags)
{
  return flags & MY_CS_BINSORT ?
           (flags & MY_CS_NOPAD ?
            &my_collation_8bit_nopad_bin_handler :
            &my_collation_8bit_bin_handler) :
           (flags & MY_CS_NOPAD ?
            &my_collation_8bit_simple_nopad_ci_handler :
            &my_collation_8bit_simple_ci_handler);
}


static void simple_cs_init_functions(struct charset_info_st *cs)
{
  cs->coll= get_simple_collation_handler_by_flags(cs->state);
  cs->cset= &my_charset_8bit_handler;
}



static int cs_copy_data(struct charset_info_st *to, CHARSET_INFO *from)
{
  to->number= from->number ? from->number : to->number;

  if (from->csname)
    if (!(to->csname= my_once_strdup(from->csname,MYF(MY_WME))))
      goto err;
  
  if (from->name)
    if (!(to->name= my_once_strdup(from->name,MYF(MY_WME))))
      goto err;
  
  if (from->comment)
    if (!(to->comment= my_once_strdup(from->comment,MYF(MY_WME))))
      goto err;
  
  if (from->ctype)
  {
    if (!(to->ctype= (uchar*) my_once_memdup((char*) from->ctype,
					     MY_CS_CTYPE_TABLE_SIZE,
					     MYF(MY_WME))))
      goto err;
    if (init_state_maps(to))
      goto err;
  }
  if (from->to_lower)
    if (!(to->to_lower= (uchar*) my_once_memdup((char*) from->to_lower,
						MY_CS_TO_LOWER_TABLE_SIZE,
						MYF(MY_WME))))
      goto err;

  if (from->to_upper)
    if (!(to->to_upper= (uchar*) my_once_memdup((char*) from->to_upper,
						MY_CS_TO_UPPER_TABLE_SIZE,
						MYF(MY_WME))))
      goto err;
  if (from->sort_order)
  {
    if (!(to->sort_order= (uchar*) my_once_memdup((char*) from->sort_order,
						  MY_CS_SORT_ORDER_TABLE_SIZE,
						  MYF(MY_WME))))
      goto err;

  }
  if (from->tab_to_uni)
  {
    uint sz= MY_CS_TO_UNI_TABLE_SIZE*sizeof(uint16);
    if (!(to->tab_to_uni= (uint16*)  my_once_memdup((char*)from->tab_to_uni,
						    sz, MYF(MY_WME))))
      goto err;
  }
  if (from->tailoring)
    if (!(to->tailoring= my_once_strdup(from->tailoring,MYF(MY_WME))))
      goto err;

  return 0;

err:
  return 1;
}


static my_bool simple_8bit_charset_data_is_full(CHARSET_INFO *cs)
{
  return cs->ctype && cs->to_upper && cs->to_lower && cs->tab_to_uni;
}


/**
  Inherit missing 8bit charset data from another collation.
  Arrays pointed by refcs must be in the permanent memory already,
  e.g. static memory, or allocated by my_once_xxx().
*/
static void
inherit_charset_data(struct charset_info_st *cs, CHARSET_INFO *refcs)
{
  if (!cs->to_upper)
    cs->to_upper= refcs->to_upper;
  if (!cs->to_lower)
    cs->to_lower= refcs->to_lower;
  if (!cs->ctype)
    cs->ctype= refcs->ctype;
  if (!cs->tab_to_uni)
    cs->tab_to_uni= refcs->tab_to_uni;
}


static my_bool simple_8bit_collation_data_is_full(CHARSET_INFO *cs)
{
  return cs->sort_order || (cs->state & MY_CS_BINSORT);
}


/**
  Inherit 8bit simple collation data from another collation.
  refcs->sort_order must be in the permanent memory already,
  e.g. static memory, or allocated by my_once_xxx().
*/
static void
inherit_collation_data(struct charset_info_st *cs, CHARSET_INFO *refcs)
{
  if (!simple_8bit_collation_data_is_full(cs))
    cs->sort_order= refcs->sort_order;
}


static my_bool simple_cs_is_full(CHARSET_INFO *cs)
{
  return  cs->number && cs->csname && cs->name &&
          simple_8bit_charset_data_is_full(cs) &&
          (simple_8bit_collation_data_is_full(cs) || cs->tailoring);
}


#if defined(HAVE_UCA_COLLATIONS) && (defined(HAVE_CHARSET_ucs2) || defined(HAVE_CHARSET_utf8))
/**
  Initialize a loaded collation.
  @param [OUT] to     - The new charset_info_st structure to initialize.
  @param [IN]  from   - A template collation, to fill the missing data from.
  @param [IN]  loaded - The collation data loaded from the LDML file.
                        some data may be missing in "loaded".
*/
static void
copy_uca_collation(struct charset_info_st *to, CHARSET_INFO *from,
                   CHARSET_INFO *loaded)
{
  to->cset= from->cset;
  to->coll= from->coll;
  /*
    Single-level UCA collation have strnxfrm_multiple=8.
    In case of a multi-level UCA collation we use strnxfrm_multiply=4.
    That means MY_COLLATION_HANDLER::strnfrmlen() will request the caller
    to allocate a buffer smaller size for each level, for performance purpose,
    and to fit longer VARCHARs to @@max_sort_length.
    This makes filesort produce non-precise order for some rare Unicode
    characters that produce more than 4 weights (long expansions).
    UCA requires 2 bytes per weight multiplied by the number of levels.
    In case of a 2-level collation, each character requires 4*2=8 bytes.
    Therefore, the longest VARCHAR that fits into the default @@max_sort_length
    is 1024/8=VARCHAR(128). With strnxfrm_multiply==8, only VARCHAR(64)
    would fit.
    Note, the built-in collation utf8_thai_520_w2 also uses strnxfrm_multiply=4,
    for the same purpose.
    TODO: we could add a new LDML syntax to choose strxfrm_multiply value.
  */
  to->strxfrm_multiply= loaded->levels_for_order > 1 ?
                        4 : from->strxfrm_multiply;
  to->min_sort_char= from->min_sort_char;
  to->max_sort_char= from->max_sort_char;
  to->mbminlen= from->mbminlen;
  to->mbmaxlen= from->mbmaxlen;
  to->caseup_multiply= from->caseup_multiply;
  to->casedn_multiply= from->casedn_multiply;
  to->state|= MY_CS_AVAILABLE | MY_CS_LOADED |
              MY_CS_STRNXFRM  | MY_CS_UNICODE;
}
#endif


static int add_collation(struct charset_info_st *cs)
{
  if (cs->name && (cs->number ||
                   (cs->number=get_collation_number_internal(cs->name))) &&
      cs->number < array_elements(all_charsets))
  {
    struct charset_info_st *newcs;
    if (!(newcs= (struct charset_info_st*) all_charsets[cs->number]))
    {
      if (!(all_charsets[cs->number]= newcs=
         (struct charset_info_st*) my_once_alloc(sizeof(CHARSET_INFO),MYF(0))))
        return MY_XML_ERROR;
      bzero(newcs,sizeof(CHARSET_INFO));
    }
    
    if (cs->primary_number == cs->number)
      cs->state |= MY_CS_PRIMARY;
      
    if (cs->binary_number == cs->number)
      cs->state |= MY_CS_BINSORT;
    
    newcs->state|= cs->state;
    
    if (!(newcs->state & MY_CS_COMPILED))
    {
      if (cs_copy_data(newcs,cs))
        return MY_XML_ERROR;

      newcs->caseup_multiply= newcs->casedn_multiply= 1;
      newcs->levels_for_order= 1;
      
      if (!strcmp(cs->csname,"ucs2") )
      {
#if defined(HAVE_CHARSET_ucs2) && defined(HAVE_UCA_COLLATIONS)
        copy_uca_collation(newcs, newcs->state & MY_CS_NOPAD ?
                                  &my_charset_ucs2_unicode_nopad_ci :
                                  &my_charset_ucs2_unicode_ci,
                                  cs);
        newcs->state|= MY_CS_AVAILABLE | MY_CS_LOADED | MY_CS_NONASCII;
#endif        
      }
      else if (!strcmp(cs->csname, "utf8") || !strcmp(cs->csname, "utf8mb3"))
      {
#if defined (HAVE_CHARSET_utf8) && defined(HAVE_UCA_COLLATIONS)
        copy_uca_collation(newcs, newcs->state & MY_CS_NOPAD ?
                                  &my_charset_utf8_unicode_nopad_ci :
                                  &my_charset_utf8_unicode_ci,
                                  cs);
        newcs->ctype= my_charset_utf8_unicode_ci.ctype;
        if (init_state_maps(newcs))
          return MY_XML_ERROR;
#endif
      }
      else if (!strcmp(cs->csname, "utf8mb4"))
      {
#if defined (HAVE_CHARSET_utf8mb4) && defined(HAVE_UCA_COLLATIONS)
        copy_uca_collation(newcs, newcs->state & MY_CS_NOPAD ?
                                  &my_charset_utf8mb4_unicode_nopad_ci :
                                  &my_charset_utf8mb4_unicode_ci,
                                  cs);
        newcs->ctype= my_charset_utf8mb4_unicode_ci.ctype;
        newcs->state|= MY_CS_AVAILABLE | MY_CS_LOADED;
#endif
      }
      else if (!strcmp(cs->csname, "utf16"))
      {
#if defined (HAVE_CHARSET_utf16) && defined(HAVE_UCA_COLLATIONS)
        copy_uca_collation(newcs, newcs->state & MY_CS_NOPAD ?
                                  &my_charset_utf16_unicode_nopad_ci :
                                  &my_charset_utf16_unicode_ci,
                                  cs);
        newcs->state|= MY_CS_AVAILABLE | MY_CS_LOADED | MY_CS_NONASCII;
#endif
      }
      else if (!strcmp(cs->csname, "utf32"))
      {
#if defined (HAVE_CHARSET_utf32) && defined(HAVE_UCA_COLLATIONS)
        copy_uca_collation(newcs, newcs->state & MY_CS_NOPAD ?
                                  &my_charset_utf32_unicode_nopad_ci :
                                  &my_charset_utf32_unicode_ci,
                                  cs);
        newcs->state|= MY_CS_AVAILABLE | MY_CS_LOADED | MY_CS_NONASCII;
#endif
      }
      else
      {
        simple_cs_init_functions(newcs);
        newcs->mbminlen= 1;
        newcs->mbmaxlen= 1;
        newcs->strxfrm_multiply= 1;
        if (simple_cs_is_full(newcs))
        {
          newcs->state |= MY_CS_LOADED;
        }
        newcs->state|= MY_CS_AVAILABLE;
      }
    }
    else
    {
      /*
        We need the below to make get_charset_name()
        and get_charset_number() working even if a
        character set has not been really incompiled.
        The above functions are used for example
        in error message compiler extra/comp_err.c.
        If a character set was compiled, this information
        will get lost and overwritten in add_compiled_collation().
      */
      newcs->number= cs->number;
      if (cs->comment)
	if (!(newcs->comment= my_once_strdup(cs->comment,MYF(MY_WME))))
	  return MY_XML_ERROR;
      if (cs->csname)
        if (!(newcs->csname= my_once_strdup(cs->csname,MYF(MY_WME))))
	  return MY_XML_ERROR;
      if (cs->name)
	if (!(newcs->name= my_once_strdup(cs->name,MYF(MY_WME))))
	  return MY_XML_ERROR;
    }
    cs->number= 0;
    cs->primary_number= 0;
    cs->binary_number= 0;
    cs->name= NULL;
    cs->state= 0;
    cs->sort_order= NULL;
    cs->tailoring= NULL;
  }
  return MY_XML_OK;
}


/**
  Report character set initialization errors and warnings.
  Be silent by default: no warnings on the client side.
*/
static void
default_reporter(enum loglevel level  __attribute__ ((unused)),
                 const char *format  __attribute__ ((unused)),
                 ...)
{
}
my_error_reporter my_charset_error_reporter= default_reporter;


/**
  Wrappers for memory functions my_malloc (and friends)
  with C-compatbile API without extra "myf" argument.
*/
static void *
my_once_alloc_c(size_t size)
{ return my_once_alloc(size, MYF(MY_WME)); }


static void *
my_malloc_c(size_t size)
{ return my_malloc(size, MYF(MY_WME)); }


static void *
my_realloc_c(void *old, size_t size)
{ return my_realloc(old, size, MYF(MY_WME|MY_ALLOW_ZERO_PTR)); }


/**
  Initialize character set loader to use mysys memory management functions.
  @param loader  Loader to initialize
*/
void
my_charset_loader_init_mysys(MY_CHARSET_LOADER *loader)
{
  loader->error[0]= '\0';
  loader->once_alloc= my_once_alloc_c;
  loader->malloc= my_malloc_c;
  loader->realloc= my_realloc_c;
  loader->free= my_free;
  loader->reporter= my_charset_error_reporter;
  loader->add_collation= add_collation;
}


#define MY_MAX_ALLOWED_BUF 1024*1024
#define MY_CHARSET_INDEX "Index.xml"

const char *charsets_dir= NULL;


static my_bool
my_read_charset_file(MY_CHARSET_LOADER *loader,
                     const char *filename,
                     myf myflags)
{
  uchar *buf;
  int  fd;
  size_t len, tmp_len;
  MY_STAT stat_info;
  
  if (!my_stat(filename, &stat_info, MYF(myflags)) ||
       ((len= (uint)stat_info.st_size) > MY_MAX_ALLOWED_BUF) ||
       !(buf= (uchar*) my_malloc(len,myflags)))
    return TRUE;
  
  if ((fd= mysql_file_open(key_file_charset, filename, O_RDONLY, myflags)) < 0)
    goto error;
  tmp_len= mysql_file_read(fd, buf, len, myflags);
  mysql_file_close(fd, myflags);
  if (tmp_len != len)
    goto error;
  
  if (my_parse_charset_xml(loader, (char *) buf, len))
  {
    my_printf_error(EE_UNKNOWN_CHARSET, "Error while parsing '%s': %s\n",
                    MYF(0), filename, loader->error);
    goto error;
  }
  
  my_free(buf);
  return FALSE;

error:
  my_free(buf);
  return TRUE;
}


char *get_charsets_dir(char *buf)
{
  const char *sharedir= SHAREDIR;
  char *res;
  DBUG_ENTER("get_charsets_dir");

  if (charsets_dir != NULL)
    strmake(buf, charsets_dir, FN_REFLEN-1);
  else
  {
    if (test_if_hard_path(sharedir) ||
	is_prefix(sharedir, DEFAULT_CHARSET_HOME))
      strxmov(buf, sharedir, "/", CHARSET_DIR, NullS);
    else
      strxmov(buf, DEFAULT_CHARSET_HOME, "/", sharedir, "/", CHARSET_DIR,
	      NullS);
  }
  res= convert_dirname(buf,buf,NullS);
  DBUG_PRINT("info",("charsets dir: '%s'", buf));
  DBUG_RETURN(res);
}

CHARSET_INFO *all_charsets[MY_ALL_CHARSETS_SIZE]={NULL};
CHARSET_INFO *default_charset_info = &my_charset_latin1;

void add_compiled_collation(struct charset_info_st *cs)
{
  DBUG_ASSERT(cs->number < array_elements(all_charsets));
  all_charsets[cs->number]= cs;
  cs->state|= MY_CS_AVAILABLE;
}


static my_pthread_once_t charsets_initialized= MY_PTHREAD_ONCE_INIT;
static my_pthread_once_t charsets_template= MY_PTHREAD_ONCE_INIT;

typedef struct
{
  ulonglong use_count;
} MY_COLLATION_STATISTICS;


static MY_COLLATION_STATISTICS my_collation_statistics[MY_ALL_CHARSETS_SIZE];


my_bool my_collation_is_known_id(uint id)
{
  return id > 0 && id < array_elements(all_charsets) && all_charsets[id] ?
         TRUE : FALSE;
}


/*
  Collation use statistics functions do not lock
  counters to avoid mutex contention. This can lose
  some counter increments with high thread concurrency.
  But this should be Ok, as we don't need exact numbers.
*/
static inline void my_collation_statistics_inc_use_count(uint id)
{
  DBUG_ASSERT(my_collation_is_known_id(id));
  my_collation_statistics[id].use_count++;
}


ulonglong my_collation_statistics_get_use_count(uint id)
{
  DBUG_ASSERT(my_collation_is_known_id(id));
  return my_collation_statistics[id].use_count;
}


const char *my_collation_get_tailoring(uint id)
{
  /* all_charsets[id]->tailoring is never changed after server startup. */
  DBUG_ASSERT(my_collation_is_known_id(id));
  return all_charsets[id]->tailoring;
}


static void init_available_charsets(void)
{
  char fname[FN_REFLEN + sizeof(MY_CHARSET_INDEX)];
  struct charset_info_st **cs;
  MY_CHARSET_LOADER loader;

  bzero((char*) &all_charsets,sizeof(all_charsets));
  bzero((char*) &my_collation_statistics, sizeof(my_collation_statistics));
  init_compiled_charsets(MYF(0));

  /* Copy compiled charsets */
  for (cs= (struct charset_info_st**) all_charsets;
       cs < (struct charset_info_st**) all_charsets +
            array_elements(all_charsets)-1 ;
       cs++)
  {
    if (*cs)
    {
      DBUG_ASSERT(cs[0]->mbmaxlen <= MY_CS_MBMAXLEN);
      if (cs[0]->ctype)
        if (init_state_maps(*cs))
          *cs= NULL;
    }
  }

  my_charset_loader_init_mysys(&loader);
  strmov(get_charsets_dir(fname), MY_CHARSET_INDEX);
  my_read_charset_file(&loader, fname, MYF(0));
}


void free_charsets(void)
{
  charsets_initialized= charsets_template;
}


static const char*
get_collation_name_alias(const char *name, char *buf, size_t bufsize)
{
  if (!strncasecmp(name, "utf8mb3_", 8))
  {
    my_snprintf(buf, bufsize, "utf8_%s", name + 8);
    return buf;
  }
  return NULL;
}


uint get_collation_number(const char *name)
{
  uint id;
  char alias[64];
  my_pthread_once(&charsets_initialized, init_available_charsets);
  if ((id= get_collation_number_internal(name)))
    return id;
  if ((name= get_collation_name_alias(name, alias, sizeof(alias))))
    return get_collation_number_internal(name);
  return 0;
}


static uint
get_charset_number_internal(const char *charset_name, uint cs_flags)
{
  CHARSET_INFO **cs;
  
  for (cs= all_charsets;
       cs < all_charsets + array_elements(all_charsets);
       cs++)
  {
    if ( cs[0] && cs[0]->csname && (cs[0]->state & cs_flags) &&
         !my_strcasecmp(&my_charset_latin1, cs[0]->csname, charset_name))
      return cs[0]->number;
  }  
  return 0;
}


static const char*
get_charset_name_alias(const char *name)
{
  if (!my_strcasecmp(&my_charset_latin1, name, "utf8mb3"))
    return "utf8";
  return NULL;
}


uint get_charset_number(const char *charset_name, uint cs_flags)
{
  uint id;
  my_pthread_once(&charsets_initialized, init_available_charsets);
  if ((id= get_charset_number_internal(charset_name, cs_flags)))
    return id;
  if ((charset_name= get_charset_name_alias(charset_name)))
    return get_charset_number_internal(charset_name, cs_flags);
  return 0;
}
                  

const char *get_charset_name(uint charset_number)
{
  my_pthread_once(&charsets_initialized, init_available_charsets);

  if (charset_number < array_elements(all_charsets))
  {
    CHARSET_INFO *cs= all_charsets[charset_number];

    if (cs && (cs->number == charset_number) && cs->name)
      return (char*) cs->name;
  }
  
  return "?";   /* this mimics find_type() */
}


static CHARSET_INFO *inheritance_source_by_id(CHARSET_INFO *cs, uint refid)
{
  CHARSET_INFO *refcs;
  return refid && refid != cs->number &&
         (refcs= all_charsets[refid]) &&
         (refcs->state & MY_CS_AVAILABLE) ? refcs : NULL;
}


static CHARSET_INFO *find_collation_data_inheritance_source(CHARSET_INFO *cs)
{
  const char *beg, *end;
  if (cs->tailoring &&
      !strncmp(cs->tailoring, "[import ", 8) &&
      (end= strchr(cs->tailoring + 8, ']')) &&
      (beg= cs->tailoring + 8) + MY_CS_NAME_SIZE > end)
  {
    char name[MY_CS_NAME_SIZE + 1];
    memcpy(name, beg, end - beg);
    name[end - beg]= '\0';
    return inheritance_source_by_id(cs, get_collation_number(name));
  }
  return NULL;
}


static CHARSET_INFO *find_charset_data_inheritance_source(CHARSET_INFO *cs)
{
  uint refid= get_charset_number_internal(cs->csname, MY_CS_PRIMARY);
  return inheritance_source_by_id(cs, refid);
}


static CHARSET_INFO *
get_internal_charset(MY_CHARSET_LOADER *loader, uint cs_number, myf flags)
{
  char  buf[FN_REFLEN];
  struct charset_info_st *cs;

  DBUG_ASSERT(cs_number < array_elements(all_charsets));

  if ((cs= (struct charset_info_st*) all_charsets[cs_number]))
  {
    if (cs->state & MY_CS_READY)  /* if CS is already initialized */
    {
      my_collation_statistics_inc_use_count(cs_number);
      return cs;
    }

    /*
      To make things thread safe we are not allowing other threads to interfere
      while we may changing the cs_info_table
    */
    mysql_mutex_lock(&THR_LOCK_charset);

    if (!(cs->state & (MY_CS_COMPILED|MY_CS_LOADED))) /* if CS is not in memory */
    {
      MY_CHARSET_LOADER loader;
      strxmov(get_charsets_dir(buf), cs->csname, ".xml", NullS);
      my_charset_loader_init_mysys(&loader);
      my_read_charset_file(&loader, buf, flags);
    }

    if (cs->state & MY_CS_AVAILABLE)
    {
      if (!(cs->state & MY_CS_READY))
      {
        if (!simple_8bit_charset_data_is_full(cs))
        {
          CHARSET_INFO *refcs= find_charset_data_inheritance_source(cs);
          if (refcs)
            inherit_charset_data(cs, refcs);
        }
        if (!simple_8bit_collation_data_is_full(cs))
        {
          CHARSET_INFO *refcl= find_collation_data_inheritance_source(cs);
          if (refcl)
            inherit_collation_data(cs, refcl);
        }

        if ((cs->cset->init && cs->cset->init(cs, loader)) ||
            (cs->coll->init && cs->coll->init(cs, loader)))
        {
          cs= NULL;
        }
        else
          cs->state|= MY_CS_READY;
      }
      my_collation_statistics_inc_use_count(cs_number);
    }
    else
      cs= NULL;

    mysql_mutex_unlock(&THR_LOCK_charset);
  }
  return cs;
}


CHARSET_INFO *get_charset(uint cs_number, myf flags)
{
  CHARSET_INFO *cs= NULL;

  if (cs_number == default_charset_info->number)
    return default_charset_info;

  my_pthread_once(&charsets_initialized, init_available_charsets);

  if (cs_number < array_elements(all_charsets))
  {
    MY_CHARSET_LOADER loader;
    my_charset_loader_init_mysys(&loader);
    cs= get_internal_charset(&loader, cs_number, flags);
  }

  if (!cs && (flags & MY_WME))
  {
    char index_file[FN_REFLEN + sizeof(MY_CHARSET_INDEX)], cs_string[23];
    strmov(get_charsets_dir(index_file),MY_CHARSET_INDEX);
    cs_string[0]='#';
    int10_to_str(cs_number, cs_string+1, 10);
    my_error(EE_UNKNOWN_CHARSET, MYF(ME_BELL), cs_string, index_file);
  }
  return cs;
}


/**
  Find collation by name: extended version of get_charset_by_name()
  to return error messages to the caller.
  @param   loader  Character set loader
  @param   name    Collation name
  @param   flags   Flags
  @return          NULL on error, pointer to collation on success
*/

CHARSET_INFO *
my_collation_get_by_name(MY_CHARSET_LOADER *loader,
                         const char *name, myf flags)
{
  uint cs_number;
  CHARSET_INFO *cs;
  my_pthread_once(&charsets_initialized, init_available_charsets);

  cs_number= get_collation_number(name);
  my_charset_loader_init_mysys(loader);
  cs= cs_number ? get_internal_charset(loader, cs_number, flags) : NULL;

  if (!cs && (flags & MY_WME))
  {
    char index_file[FN_REFLEN + sizeof(MY_CHARSET_INDEX)];
    strmov(get_charsets_dir(index_file),MY_CHARSET_INDEX);
    my_error(EE_UNKNOWN_COLLATION, MYF(ME_BELL), name, index_file);
  }
  return cs;
}


CHARSET_INFO *get_charset_by_name(const char *cs_name, myf flags)
{
  MY_CHARSET_LOADER loader;
  my_charset_loader_init_mysys(&loader);
  return my_collation_get_by_name(&loader, cs_name, flags);
}


/**
  Find character set by name: extended version of get_charset_by_csname()
  to return error messages to the caller.
  @param   loader   Character set loader
  @param   name     Collation name
  @param   cs_flags Character set flags (e.g. default or binary collation)
  @param   flags    Flags
  @return           NULL on error, pointer to collation on success
*/
CHARSET_INFO *
my_charset_get_by_name(MY_CHARSET_LOADER *loader,
                       const char *cs_name, uint cs_flags, myf flags)
{
  uint cs_number;
  CHARSET_INFO *cs;
  DBUG_ENTER("get_charset_by_csname");
  DBUG_PRINT("enter",("name: '%s'", cs_name));

  my_pthread_once(&charsets_initialized, init_available_charsets);

  cs_number= get_charset_number(cs_name, cs_flags);
  cs= cs_number ? get_internal_charset(loader, cs_number, flags) : NULL;

  if (!cs && (flags & MY_WME))
  {
    char index_file[FN_REFLEN + sizeof(MY_CHARSET_INDEX)];
    strmov(get_charsets_dir(index_file),MY_CHARSET_INDEX);
    my_error(EE_UNKNOWN_CHARSET, MYF(ME_BELL), cs_name, index_file);
  }

  DBUG_RETURN(cs);
}


CHARSET_INFO *
get_charset_by_csname(const char *cs_name, uint cs_flags, myf flags)
{
  MY_CHARSET_LOADER loader;
  my_charset_loader_init_mysys(&loader);
  return my_charset_get_by_name(&loader, cs_name, cs_flags, flags);
}


/**
  Resolve character set by the character set name (utf8, latin1, ...).

  The function tries to resolve character set by the specified name. If
  there is character set with the given name, it is assigned to the "cs"
  parameter and FALSE is returned. If there is no such character set,
  "default_cs" is assigned to the "cs" and TRUE is returned.

  @param[in] cs_name    Character set name.
  @param[in] default_cs Default character set.
  @param[out] cs        Variable to store character set.

  @return FALSE if character set was resolved successfully; TRUE if there
  is no character set with given name.
*/

my_bool resolve_charset(const char *cs_name,
                        CHARSET_INFO *default_cs,
                        CHARSET_INFO **cs)
{
  *cs= get_charset_by_csname(cs_name, MY_CS_PRIMARY, MYF(0));

  if (*cs == NULL)
  {
    *cs= default_cs;
    return TRUE;
  }

  return FALSE;
}


/**
  Resolve collation by the collation name (utf8_general_ci, ...).

  The function tries to resolve collation by the specified name. If there
  is collation with the given name, it is assigned to the "cl" parameter
  and FALSE is returned. If there is no such collation, "default_cl" is
  assigned to the "cl" and TRUE is returned.

  @param[out] cl        Variable to store collation.
  @param[in] cl_name    Collation name.
  @param[in] default_cl Default collation.

  @return FALSE if collation was resolved successfully; TRUE if there is no
  collation with given name.
*/

my_bool resolve_collation(const char *cl_name,
                          CHARSET_INFO *default_cl,
                          CHARSET_INFO **cl)
{
  *cl= get_charset_by_name(cl_name, MYF(0));

  if (*cl == NULL)
  {
    *cl= default_cl;
    return TRUE;
  }

  return FALSE;
}


/*
  Escape string with backslashes (\)

  SYNOPSIS
    escape_string_for_mysql()
    charset_info        Charset of the strings
    to                  Buffer for escaped string
    to_length           Length of destination buffer, or 0
    from                The string to escape
    length              The length of the string to escape

  DESCRIPTION
    This escapes the contents of a string by adding backslashes before special
    characters, and turning others into specific escape sequences, such as
    turning newlines into \n and null bytes into \0.

  NOTE
    To maintain compatibility with the old C API, to_length may be 0 to mean
    "big enough"

  RETURN VALUES
    (size_t) -1 The escaped string did not fit in the to buffer
    #           The length of the escaped string
*/

size_t escape_string_for_mysql(CHARSET_INFO *charset_info,
                               char *to, size_t to_length,
                               const char *from, size_t length)
{
  const char *to_start= to;
  const char *end, *to_end=to_start + (to_length ? to_length-1 : 2*length);
  my_bool overflow= FALSE;
  for (end= from + length; from < end; from++)
  {
    char escape= 0;
#ifdef USE_MB
    int tmp_length= use_mb(charset_info) ? my_charlen(charset_info, from, end) :
                                           1;
    if (tmp_length > 1)
    {
      if (to + tmp_length > to_end)
      {
        overflow= TRUE;
        break;
      }
      while (tmp_length--)
	*to++= *from++;
      from--;
      continue;
    }
    /*
     If the next character appears to begin a multi-byte character, we
     escape that first byte of that apparent multi-byte character. (The
     character just looks like a multi-byte character -- if it were actually
     a multi-byte character, it would have been passed through in the test
     above.)

     Without this check, we can create a problem by converting an invalid
     multi-byte character into a valid one. For example, 0xbf27 is not
     a valid GBK character, but 0xbf5c is. (0x27 = ', 0x5c = \)
    */
    if (tmp_length < 1) /* Bad byte sequence */
      escape= *from;
    else
#endif
    switch (*from) {
    case 0:				/* Must be escaped for 'mysql' */
      escape= '0';
      break;
    case '\n':				/* Must be escaped for logs */
      escape= 'n';
      break;
    case '\r':
      escape= 'r';
      break;
    case '\\':
      escape= '\\';
      break;
    case '\'':
      escape= '\'';
      break;
    case '"':				/* Better safe than sorry */
      escape= '"';
      break;
    case '\032':			/* This gives problems on Win32 */
      escape= 'Z';
      break;
    }
    if (escape)
    {
      if (to + 2 > to_end)
      {
        overflow= TRUE;
        break;
      }
      *to++= '\\';
      *to++= escape;
    }
    else
    {
      if (to + 1 > to_end)
      {
        overflow= TRUE;
        break;
      }
      *to++= *from;
    }
  }
  *to= 0;
  return overflow ? (size_t) -1 : (size_t) (to - to_start);
}


#ifdef BACKSLASH_MBTAIL
static CHARSET_INFO *fs_cset_cache= NULL;

CHARSET_INFO *fs_character_set()
{
  if (!fs_cset_cache)
  {
    char buf[10]= "cp";
    GetLocaleInfo(LOCALE_SYSTEM_DEFAULT, LOCALE_IDEFAULTANSICODEPAGE,
                  buf+2, sizeof(buf)-3);
    /*
      We cannot call get_charset_by_name here
      because fs_character_set() is executed before
      LOCK_THD_charset mutex initialization, which
      is used inside get_charset_by_name.
      As we're now interested in cp932 only,
      let's just detect it using strcmp().
    */
    fs_cset_cache= 
                #ifdef HAVE_CHARSET_cp932
                        !strcmp(buf, "cp932") ? &my_charset_cp932_japanese_ci : 
                #endif
                        &my_charset_bin;
  }
  return fs_cset_cache;
}
#endif

/*
  Escape apostrophes by doubling them up

  SYNOPSIS
    escape_quotes_for_mysql()
    charset_info        Charset of the strings
    to                  Buffer for escaped string
    to_length           Length of destination buffer, or 0
    from                The string to escape
    length              The length of the string to escape

  DESCRIPTION
    This escapes the contents of a string by doubling up any apostrophes that
    it contains. This is used when the NO_BACKSLASH_ESCAPES SQL_MODE is in
    effect on the server.

  NOTE
    To be consistent with escape_string_for_mysql(), to_length may be 0 to
    mean "big enough"

  RETURN VALUES
    ~0          The escaped string did not fit in the to buffer
    >=0         The length of the escaped string
*/

size_t escape_quotes_for_mysql(CHARSET_INFO *charset_info,
                               char *to, size_t to_length,
                               const char *from, size_t length)
{
  const char *to_start= to;
  const char *end, *to_end=to_start + (to_length ? to_length-1 : 2*length);
  my_bool overflow= FALSE;
#ifdef USE_MB
  my_bool use_mb_flag= use_mb(charset_info);
#endif
  for (end= from + length; from < end; from++)
  {
#ifdef USE_MB
    int tmp_length;
    if (use_mb_flag && (tmp_length= my_ismbchar(charset_info, from, end)))
    {
      if (to + tmp_length > to_end)
      {
        overflow= TRUE;
        break;
      }
      while (tmp_length--)
	*to++= *from++;
      from--;
      continue;
    }
    /*
      We don't have the same issue here with a non-multi-byte character being
      turned into a multi-byte character by the addition of an escaping
      character, because we are only escaping the ' character with itself.
     */
#endif
    if (*from == '\'')
    {
      if (to + 2 > to_end)
      {
        overflow= TRUE;
        break;
      }
      *to++= '\'';
      *to++= '\'';
    }
    else
    {
      if (to + 1 > to_end)
      {
        overflow= TRUE;
        break;
      }
      *to++= *from;
    }
  }
  *to= 0;
  return overflow ? (ulong)~0 : (ulong) (to - to_start);
}


typedef enum my_cs_match_type_enum
{
  /* MySQL and OS charsets are fully compatible */
  my_cs_exact,
  /* MySQL charset is very close to OS charset  */
  my_cs_approx,
  /*
    MySQL knows this charset, but it is not supported as client character set.
  */
  my_cs_unsupp
} my_cs_match_type;


typedef struct str2str_st
{
  const char* os_name;
  const char* my_name;
  my_cs_match_type param;
} MY_CSET_OS_NAME;

static const MY_CSET_OS_NAME charsets[] =
{
#ifdef _WIN32
  {"cp437",          "cp850",    my_cs_approx},
  {"cp850",          "cp850",    my_cs_exact},
  {"cp852",          "cp852",    my_cs_exact},
  {"cp858",          "cp850",    my_cs_approx},
  {"cp866",          "cp866",    my_cs_exact},
  {"cp874",          "tis620",   my_cs_approx},
  {"cp932",          "cp932",    my_cs_exact},
  {"cp936",          "gbk",      my_cs_approx},
  {"cp949",          "euckr",    my_cs_approx},
  {"cp950",          "big5",     my_cs_exact},
  {"cp1200",         "utf16le",  my_cs_unsupp},
  {"cp1201",         "utf16",    my_cs_unsupp},
  {"cp1250",         "cp1250",   my_cs_exact},
  {"cp1251",         "cp1251",   my_cs_exact},
  {"cp1252",         "latin1",   my_cs_exact},
  {"cp1253",         "greek",    my_cs_exact},
  {"cp1254",         "latin5",   my_cs_exact},
  {"cp1255",         "hebrew",   my_cs_approx},
  {"cp1256",         "cp1256",   my_cs_exact},
  {"cp1257",         "cp1257",   my_cs_exact},
  {"cp10000",        "macroman", my_cs_exact},
  {"cp10001",        "sjis",     my_cs_approx},
  {"cp10002",        "big5",     my_cs_approx},
  {"cp10008",        "gb2312",   my_cs_approx},
  {"cp10021",        "tis620",   my_cs_approx},
  {"cp10029",        "macce",    my_cs_exact},
  {"cp12001",        "utf32",    my_cs_unsupp},
  {"cp20107",        "swe7",     my_cs_exact},
  {"cp20127",        "latin1",   my_cs_approx},
  {"cp20866",        "koi8r",    my_cs_exact},
  {"cp20932",        "ujis",     my_cs_exact},
  {"cp20936",        "gb2312",   my_cs_approx},
  {"cp20949",        "euckr",    my_cs_approx},
  {"cp21866",        "koi8u",    my_cs_exact},
  {"cp28591",        "latin1",   my_cs_approx},
  {"cp28592",        "latin2",   my_cs_exact},
  {"cp28597",        "greek",    my_cs_exact},
  {"cp28598",        "hebrew",   my_cs_exact},
  {"cp28599",        "latin5",   my_cs_exact},
  {"cp28603",        "latin7",   my_cs_exact},
#ifdef UNCOMMENT_THIS_WHEN_WL_4579_IS_DONE
  {"cp28605",        "latin9",   my_cs_exact},
#endif
  {"cp38598",        "hebrew",   my_cs_exact},
  {"cp51932",        "ujis",     my_cs_exact},
  {"cp51936",        "gb2312",   my_cs_exact},
  {"cp51949",        "euckr",    my_cs_exact},
  {"cp51950",        "big5",     my_cs_exact},
#ifdef UNCOMMENT_THIS_WHEN_WL_WL_4024_IS_DONE
  {"cp54936",        "gb18030",  my_cs_exact},
#endif
  {"cp65001",        "utf8",     my_cs_exact},

#else /* not Windows */

  {"646",            "latin1",   my_cs_approx}, /* Default on Solaris */
  {"ANSI_X3.4-1968", "latin1",   my_cs_approx},
  {"ansi1251",       "cp1251",   my_cs_exact},
  {"armscii8",       "armscii8", my_cs_exact},
  {"armscii-8",      "armscii8", my_cs_exact},
  {"ASCII",          "latin1",   my_cs_approx},
  {"Big5",           "big5",     my_cs_exact},
  {"cp1251",         "cp1251",   my_cs_exact},
  {"cp1255",         "hebrew",   my_cs_approx},
  {"CP866",          "cp866",    my_cs_exact},
  {"eucCN",          "gb2312",   my_cs_exact},
  {"euc-CN",         "gb2312",   my_cs_exact},
  {"eucJP",          "ujis",     my_cs_exact},
  {"euc-JP",         "ujis",     my_cs_exact},
  {"eucKR",          "euckr",    my_cs_exact},
  {"euc-KR",         "euckr",    my_cs_exact},
#ifdef UNCOMMENT_THIS_WHEN_WL_WL_4024_IS_DONE
  {"gb18030",        "gb18030",  my_cs_exact},
#endif
  {"gb2312",         "gb2312",   my_cs_exact},
  {"gbk",            "gbk",      my_cs_exact},
  {"georgianps",     "geostd8",  my_cs_exact},
  {"georgian-ps",    "geostd8",  my_cs_exact},
  {"IBM-1252",       "cp1252",   my_cs_exact},

  {"iso88591",       "latin1",   my_cs_approx},
  {"ISO_8859-1",     "latin1",   my_cs_approx},
  {"ISO8859-1",      "latin1",   my_cs_approx},
  {"ISO-8859-1",     "latin1",   my_cs_approx},

  {"iso885913",      "latin7",   my_cs_exact},
  {"ISO_8859-13",    "latin7",   my_cs_exact},
  {"ISO8859-13",     "latin7",   my_cs_exact},
  {"ISO-8859-13",    "latin7",   my_cs_exact},

#ifdef UNCOMMENT_THIS_WHEN_WL_4579_IS_DONE
  {"iso885915",      "latin9",   my_cs_exact},
  {"ISO_8859-15",    "latin9",   my_cs_exact},
  {"ISO8859-15",     "latin9",   my_cs_exact},
  {"ISO-8859-15",    "latin9",   my_cs_exact},
#endif

  {"iso88592",       "latin2",   my_cs_exact},
  {"ISO_8859-2",     "latin2",   my_cs_exact},
  {"ISO8859-2",      "latin2",   my_cs_exact},
  {"ISO-8859-2",     "latin2",   my_cs_exact},

  {"iso88597",       "greek",    my_cs_exact},
  {"ISO_8859-7",     "greek",    my_cs_exact},
  {"ISO8859-7",      "greek",    my_cs_exact},
  {"ISO-8859-7",     "greek",    my_cs_exact},

  {"iso88598",       "hebrew",   my_cs_exact},
  {"ISO_8859-8",     "hebrew",   my_cs_exact},
  {"ISO8859-8",      "hebrew",   my_cs_exact},
  {"ISO-8859-8",     "hebrew",   my_cs_exact},

  {"iso88599",       "latin5",   my_cs_exact},
  {"ISO_8859-9",     "latin5",   my_cs_exact},
  {"ISO8859-9",      "latin5",   my_cs_exact},
  {"ISO-8859-9",     "latin5",   my_cs_exact},

  {"koi8r",          "koi8r",    my_cs_exact},
  {"KOI8-R",         "koi8r",    my_cs_exact},
  {"koi8u",          "koi8u",    my_cs_exact},
  {"KOI8-U",         "koi8u",    my_cs_exact},

  {"roman8",         "hp8",      my_cs_exact}, /* Default on HP UX */

  {"Shift_JIS",      "sjis",     my_cs_exact},
  {"SJIS",           "sjis",     my_cs_exact},
  {"shiftjisx0213",  "sjis",     my_cs_exact},

  {"tis620",         "tis620",   my_cs_exact},
  {"tis-620",        "tis620",   my_cs_exact},

  {"ujis",           "ujis",     my_cs_exact},

  {"US-ASCII",       "latin1",   my_cs_approx},

  {"utf8",           "utf8",     my_cs_exact},
  {"utf-8",          "utf8",     my_cs_exact},
#endif
  {NULL,             NULL,       0}
};


static const char*
my_os_charset_to_mysql_charset(const char* csname)
{
  const MY_CSET_OS_NAME* csp;
  for (csp = charsets; csp->os_name; csp++)
  {
    if (!strcasecmp(csp->os_name, csname))
    {
      switch (csp->param)
      {
      case my_cs_exact:
        return csp->my_name;

      case my_cs_approx:
        /*
          Maybe we should print a warning eventually:
          character set correspondence is not exact.
        */
        return csp->my_name;

      default:
        return NULL;
      }
    }
  }
  return NULL;
}

const char* my_default_csname()
{
  const char* csname = NULL;
#ifdef _WIN32
  char cpbuf[64];
  int cp = GetConsoleCP();
  if (cp == 0)
    cp = GetACP();
  snprintf(cpbuf, sizeof(cpbuf), "cp%d", (int)cp);
  csname = my_os_charset_to_mysql_charset(cpbuf);
#elif defined(HAVE_SETLOCALE) && defined(HAVE_NL_LANGINFO)
  if (setlocale(LC_CTYPE, "") && (csname = nl_langinfo(CODESET)))
    csname = my_os_charset_to_mysql_charset(csname);
#endif
  return csname ? csname : MYSQL_DEFAULT_CHARSET_NAME;
}