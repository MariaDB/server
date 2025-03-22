/* Copyright (c) 2025, MariaDB

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

#include "strings_def.h"
#include "m_ctype.h"
#include "ctype-uca.h"
#include "ctype-unidata.h"
#include "ctype-uca1400data.h"


/*
  Return UCA-4.0.0 compatible ID, e.g. for use in the protocol
  with the old clients.
*/
uint my_uca1400_collation_id_uca400_compat(uint id)
{
  uint tlid= my_uca1400_collation_id_to_tailoring_id(id);
  my_cs_encoding_t csid= my_uca1400_collation_id_to_charset_id(id);
  MY_UCA1400_COLLATION_DEFINITION *def;
  DBUG_ASSERT(my_collation_id_is_uca1400(id));
  if (!(def= &my_uca1400_collation_definitions[tlid])->name)
    return id;
  switch (csid) {
  case MY_CS_ENCODING_UTF8MB3: return def->id_utf8mb3;
  case MY_CS_ENCODING_UTF8MB4: return def->id_utf8mb4;
  case MY_CS_ENCODING_UCS2:    return def->id_ucs2;
  case MY_CS_ENCODING_UTF16:   return def->id_utf16;
  case MY_CS_ENCODING_UTF32:   return def->id_utf32;
  }
  return id;
}


LEX_CSTRING my_ci_get_collation_name_uca1400_context(CHARSET_INFO *cs)
{
  LEX_CSTRING res;
  DBUG_ASSERT(my_collation_id_is_uca1400(cs->number));

  if (cs->coll_name.length <= cs->cs_name.length ||
      cs->coll_name.str[cs->cs_name.length] != '_')
  {
    DBUG_ASSERT(0);
    return cs->coll_name;
  }
  res.str= cs->coll_name.str + cs->cs_name.length + 1;
  res.length= cs->coll_name.length - cs->cs_name.length - 1;
  return res;
}


extern struct charset_info_st my_charset_utf8mb3_unicode_520_nopad_ci;
extern struct charset_info_st my_charset_utf8mb3_unicode_520_ci;
extern struct charset_info_st my_charset_utf8mb4_unicode_520_nopad_ci;
extern struct charset_info_st my_charset_utf8mb4_unicode_520_ci;
extern struct charset_info_st my_charset_ucs2_unicode_520_nopad_ci;
extern struct charset_info_st my_charset_ucs2_unicode_520_ci;
extern struct charset_info_st my_charset_utf16_unicode_520_nopad_ci;
extern struct charset_info_st my_charset_utf16_unicode_520_ci;
extern struct charset_info_st my_charset_utf32_unicode_520_nopad_ci;
extern struct charset_info_st my_charset_utf32_unicode_520_ci;
extern struct charset_info_st my_charset_utf8mb4_turkish_uca_ci;


static
CHARSET_INFO *my_uca1400_collation_source(my_cs_encoding_t cs_id,
                                          uint nopad_flags)
{
  switch (cs_id) {
  case MY_CS_ENCODING_UTF8MB3:
    return nopad_flags ? &my_charset_utf8mb3_unicode_520_nopad_ci :
                         &my_charset_utf8mb3_unicode_520_ci;
  case MY_CS_ENCODING_UTF8MB4:
    return nopad_flags ? &my_charset_utf8mb4_unicode_520_nopad_ci :
                         &my_charset_utf8mb4_unicode_520_ci;
#ifdef HAVE_CHARSET_ucs2
  case MY_CS_ENCODING_UCS2:
    return nopad_flags ? &my_charset_ucs2_unicode_520_nopad_ci :
                         &my_charset_ucs2_unicode_520_ci;
#endif
#ifdef HAVE_CHARSET_utf16
  case MY_CS_ENCODING_UTF16:
    return nopad_flags ? &my_charset_utf16_unicode_520_nopad_ci :
                         &my_charset_utf16_unicode_520_ci;
#endif
#ifdef HAVE_CHARSET_utf32
  case MY_CS_ENCODING_UTF32:
    return nopad_flags ? &my_charset_utf32_unicode_520_nopad_ci :
                         &my_charset_utf32_unicode_520_ci;
#endif
  }
  return NULL;
}


MY_UCA_INFO my_uca_v1400=
{
  {
    {
      0x10FFFF,      /* maxchar           */
      (uchar *) uca1400_length,
      (uint16 **) uca1400_weight,
      {              /* Contractions:     */
        array_elements(uca1400_contractions), /* nitems */
        uca1400_contractions,                 /* item   */
        NULL         /*   flags           */
      },
      0,             /* levelno */
      {0},           /* contraction_hash   */
      NULL           /* booster            */
    },

    {
      0x10FFFF,      /* maxchar */
      (uchar *) uca1400_length_secondary,
      (uint16 **) uca1400_weight_secondary,
      {              /* Contractions: */
        array_elements(uca1400_contractions_secondary), /* nitems */
        uca1400_contractions_secondary,                 /* item   */
        NULL         /*   flags */
      },
      1,             /* levelno */
      {0},           /* contraction_hash   */
      NULL           /* booster            */
    },

    {
      0x10FFFF,      /* maxchar */
      (uchar *) uca1400_length_tertiary,
      (uint16 **) uca1400_weight_tertiary,
      {              /* Contractions: */
        array_elements(uca1400_contractions_tertiary), /* nitems */
        uca1400_contractions_tertiary,                 /* item   */
        NULL         /*   flags */
      },
      2,             /* levelno */
      {0},           /* contraction_hash   */
      NULL           /* booster            */
    }

  },

  uca1400_non_ignorable_first,
  uca1400_non_ignorable_last,

  uca1400_primary_ignorable_first,
  uca1400_primary_ignorable_last,

  uca1400_secondary_ignorable_first,
  uca1400_secondary_ignorable_last,

  uca1400_tertiary_ignorable_first,
  uca1400_tertiary_ignorable_last,

  0x0000,    /* first_trailing */
  0x0000,    /* last_trailing  */

  uca1400_variable_first,
  uca1400_variable_last,

  /* Misc */
  uca1400_version
};


MY_UCA_INFO
my_uca1400_info_tailored[MY_CS_ENCODING_LAST+1]
                        [MY_UCA1400_COLLATION_DEFINITION_COUNT];


uint my_uca1400_make_builtin_collation_id(my_cs_encoding_t charset_id,
                                          uint tailoring_id,
                                          my_bool nopad,
                                          my_bool secondary_level,
                                          my_bool tertiary_level)
{
  if (!my_uca1400_collation_definitions[tailoring_id].tailoring)
    return 0;
  return MY_UCA1400_COLLATION_ID_POSSIBLE_MIN +
         (charset_id << 8) +
         (tailoring_id << 3) +
         (nopad << 2) +
         (secondary_level << 1) +
         (tertiary_level << 0);
}


uca_collation_def_param_t
my_uca1400_collation_param_by_id(uint id)
{
  uca_collation_def_param_t res;
  my_bool secondary_level= my_uca1400_collation_id_to_secondary_level_flag(id);
  my_bool tertiary_level=  my_uca1400_collation_id_to_tertiary_level_flag(id);

  res.cs_id= my_uca1400_collation_id_to_charset_id(id);
  res.tailoring_id= my_uca1400_collation_id_to_tailoring_id(id);
  res.nopad_flags= my_uca1400_collation_id_to_nopad_flag(id);
  res.level_flags= (1 << MY_CS_LEVEL_BIT_PRIMARY) |
                   (secondary_level ? 1 << MY_CS_LEVEL_BIT_SECONDARY : 0) |
                   (tertiary_level  ? 1 << MY_CS_LEVEL_BIT_TERTIARY  : 0);
  return res;
}


LEX_CSTRING
my_uca1400_collation_build_name(char *buffer, size_t buffer_size,
                                const LEX_CSTRING *cs_name,
                                const char *tailoring_name,
                                const uca_collation_def_param_t *prm)
{
  LEX_CSTRING res;
  res.str= buffer;
  res.length=
    my_snprintf(buffer, buffer_size, "%.*s_uca1400%s%s%s%s%s",
         (int) cs_name->length, cs_name->str,
         tailoring_name[0] ? "_" : "",
         tailoring_name,
         prm->nopad_flags ? "_nopad" : "",
         prm->level_flags & (1<<MY_CS_LEVEL_BIT_SECONDARY) ? "_as" : "_ai",
         prm->level_flags & (1<<MY_CS_LEVEL_BIT_TERTIARY) ? "_cs" : "_ci");
  return res;
}


/*
  For extra safety let's define and check a set of flags
  which are not expected for UCA 1400 collations.
*/
static inline uint
uca1400_unexpected_flags()
{
  return  MY_CS_BINSORT|
          MY_CS_PRIMARY|
          MY_CS_PUREASCII|
          MY_CS_LOWER_SORT;
}


static void
my_uca1400_collation_definition_init(MY_CHARSET_LOADER *loader,
                                     struct charset_info_st *dst,
                                     const uca_collation_def_param_t *param)
{
  const MY_UCA1400_COLLATION_DEFINITION *def=
    &my_uca1400_collation_definitions[param->tailoring_id];

  /* Copy the entire charset_info_st from an in-compiled one. */
  *dst= *my_uca1400_collation_source(param->cs_id, param->nopad_flags);

  /* Now replace some members according to param */
  DBUG_ASSERT((dst->state & uca1400_unexpected_flags()) == 0);
  dst->uca= &my_uca_v1400;
  dst->tailoring= def->tailoring;
  if (def->tailoring == my_charset_utf8mb4_turkish_uca_ci.tailoring)
    dst->casefold= &my_casefold_unicode1400tr;
  else
    dst->casefold= &my_casefold_unicode1400;

  dst->state|= param->nopad_flags;
  my_ci_set_level_flags(dst, param->level_flags);
}


my_bool
my_uca1400_collation_alloc_and_init(MY_CHARSET_LOADER *loader,
                                    LEX_CSTRING name,
                                    LEX_CSTRING comment,
                                    const uca_collation_def_param_t *param,
                                    uint id)
{
  struct charset_info_st *dst;

  if (!(dst= my_ci_alloc(loader, name, &name, comment, &comment)))
    return TRUE;

  my_uca1400_collation_definition_init(loader, dst, param);

  dst->number= id;
  dst->coll_name= name;
  dst->comment= comment.str;

  return (loader->add_collation)(dst) != 0;
}


static
my_bool my_uca1400_collation_definition_add(MY_CHARSET_LOADER *loader, uint id)
{
  char coll_name_buffer[MY_CS_COLLATION_NAME_SIZE + 1];
  LEX_CSTRING coll_name;
  LEX_CSTRING comment= {"",0};
  uca_collation_def_param_t param= my_uca1400_collation_param_by_id(id);
  CHARSET_INFO *src= my_uca1400_collation_source(param.cs_id,
                                                 param.nopad_flags);
  const MY_UCA1400_COLLATION_DEFINITION *def=
    &my_uca1400_collation_definitions[param.tailoring_id];

  DBUG_ASSERT(id);

  coll_name= my_uca1400_collation_build_name(coll_name_buffer,
                                             sizeof(coll_name_buffer),
                                             &src->cs_name,
                                             def->name,
                                             &param);

  return my_uca1400_collation_alloc_and_init(loader, coll_name, comment,
                                             &param, id);
}


my_bool my_uca1400_collation_definitions_add(MY_CHARSET_LOADER *loader)
{
  my_cs_encoding_t charset_id;
  for (charset_id= (my_cs_encoding_t) 0;
       charset_id <= (my_cs_encoding_t) MY_CS_ENCODING_LAST;
       charset_id++)
  {
    uint tailoring_id;
    for (tailoring_id= 0 ;
         tailoring_id < MY_UCA1400_COLLATION_DEFINITION_COUNT;
         tailoring_id++)
    {
      uint nopad;
      for (nopad= 0; nopad < 2; nopad++)
      {
        uint secondary_level;
        for (secondary_level= 0; secondary_level < 2; secondary_level++)
        {
          uint tertiary_level;
          for (tertiary_level= 0; tertiary_level < 2; tertiary_level++)
          {
            uint collation_id= my_uca1400_make_builtin_collation_id(charset_id,
                                                     tailoring_id,
                                                     (my_bool) nopad,
                                                     (my_bool) secondary_level,
                                                     (my_bool) tertiary_level);
            if (collation_id &&
                my_uca1400_collation_definition_add(loader, collation_id))
              return TRUE;
          }
        }
      }
    }
  }
  return FALSE;
}
