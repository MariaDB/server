#ifndef CTYPE_UCA_1400_H
#define CTYPE_UCA_1400_H
/* Copyright (c) 2021, MariaDB

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


/*
  17000..187FF; Tangut             [6144]
  18800..18AFF; Tangut Components  [768]
  18D00..18D7F; Tangut Supplement  [128]
*/
static inline my_bool
my_uca_1400_is_assigned_tangut(my_wc_t code)
{
  return (code >= 0x17000 && code <= 0x187FF) ||
         (code >= 0x18800 && code <= 0x18AFF) ||
         (code >= 0x18D00 && code <= 0x18D7F);
}

static inline MY_UCA_IMPLICIT_WEIGHT
my_uca_1400_implicit_weight_primary_tangut(my_wc_t code)
{
  MY_UCA_IMPLICIT_WEIGHT res;
  res.weight[0]= 0xFB00;
  res.weight[1]= (uint16) (code - 0x17000) | 0x8000;
  return res;
}


/*
  1B170..1B2FF; Nushu  [400]
*/
static inline my_bool
my_uca_1400_is_assigned_nushu(my_wc_t code)
{
  return code >= 0x1B170 && code <= 0x1B2FF;
}

static inline MY_UCA_IMPLICIT_WEIGHT
my_uca_1400_implicit_weight_primary_nushu(my_wc_t code)
{
  MY_UCA_IMPLICIT_WEIGHT res;
  res.weight[0]= 0xFB01;
  res.weight[1]= (uint16) (code - 0x1B170) | 0x8000;
  return res;
}


/*
  18B00..18CFF; Khitan Small Script [512]
*/
static inline my_bool
my_uca_1400_is_assigned_khitan_small_script(my_wc_t code)
{
  return code >= 0x18B00 && code <= 0x18CFF;
}

static inline MY_UCA_IMPLICIT_WEIGHT
my_uca_1400_implicit_weight_primary_khitan(my_wc_t code)
{
  MY_UCA_IMPLICIT_WEIGHT res;
  res.weight[0]= 0xFB02;
  res.weight[1]= (uint16) (code - 0x18B00) | 0x8000;
  return res;
}


/*
  Unified_Ideograph=True AND
  ((Block=CJK_Unified_Ideograph) OR (Block=CJK_Compatibility_Ideographs))

  https://www.unicode.org/Public/14.0.0/ucd/Blocks.txt

  4E00..9FFF;   CJK Unified Ideographs
  F900..FAFF;   CJK Compatibility Ideographs

  https://www.unicode.org/Public/14.0.0/ucd/PropList.txt

  4E00..9FFF    ; Unified_Ideograph # Lo [20992] CJK UNIFIED IDEOGRAPH-4E00..CJK UNIFIED IDEOGRAPH-9FFF
  FA0E..FA0F    ; Unified_Ideograph # Lo   [2] CJK COMPATIBILITY IDEOGRAPH-FA0E..CJK COMPATIBILITY IDEOGRAPH-FA0F
  FA11          ; Unified_Ideograph # Lo       CJK COMPATIBILITY IDEOGRAPH-FA11
  FA13..FA14    ; Unified_Ideograph # Lo   [2] CJK COMPATIBILITY IDEOGRAPH-FA13..CJK COMPATIBILITY IDEOGRAPH-FA14
  FA1F          ; Unified_Ideograph # Lo       CJK COMPATIBILITY IDEOGRAPH-FA1F
  FA21          ; Unified_Ideograph # Lo       CJK COMPATIBILITY IDEOGRAPH-FA21
  FA23..FA24    ; Unified_Ideograph # Lo   [2] CJK COMPATIBILITY IDEOGRAPH-FA23..CJK COMPATIBILITY IDEOGRAPH-FA24
  FA27..FA29    ; Unified_Ideograph # Lo   [3] CJK COMPATIBILITY IDEOGRAPH-FA27..CJK COMPATIBILITY IDEOGRAPH-FA29
*/
static inline my_bool
my_uca_1400_is_core_han_unified_ideograph(my_wc_t code)
{
  return (code >= 0x4E00 && code <= 0x9FFF) ||
         (code >= 0xFA0E && code <= 0xFA0F) ||
         (code == 0xFA11) ||
         (code >= 0xFA13 && code <= 0xFA14) ||
         (code == 0xFA1F) ||
         (code == 0xFA21) ||
         (code >= 0xFA23 && code <= 0xFA24) ||
         (code >= 0xFA27 && code <= 0xFA29);
}


/*
  (Unified_Ideograph=True AND NOT
   ((Block=CJK_Unified_Ideograph) OR (Block=CJK_Compatibility_Ideographs))

  https://www.unicode.org/Public/14.0.0/ucd/Blocks.txt

  3400..4DBF;   CJK Unified Ideographs Extension A
  20000..2A6DF; CJK Unified Ideographs Extension B
  2A700..2B73F; CJK Unified Ideographs Extension C
  2B740..2B81F; CJK Unified Ideographs Extension D
  2B820..2CEAF; CJK Unified Ideographs Extension E
  2CEB0..2EBEF; CJK Unified Ideographs Extension F
  30000..3134F; CJK Unified Ideographs Extension G

  https://www.unicode.org/Public/14.0.0/ucd/PropList.txt

  3400..4DBF    ; Unified_Ideograph # Lo [6592] CJK UNIFIED IDEOGRAPH-3400..CJK UNIFIED IDEOGRAPH-4DBF
  20000..2A6DF  ; Unified_Ideograph # Lo [42720] CJK UNIFIED IDEOGRAPH-20000..CJK UNIFIED IDEOGRAPH-2A6DF
  2A700..2B738  ; Unified_Ideograph # Lo [4153] CJK UNIFIED IDEOGRAPH-2A700..CJK UNIFIED IDEOGRAPH-2B738
  2B740..2B81D  ; Unified_Ideograph # Lo [222] CJK UNIFIED IDEOGRAPH-2B740..CJK UNIFIED IDEOGRAPH-2B81D
  2B820..2CEA1  ; Unified_Ideograph # Lo [5762] CJK UNIFIED IDEOGRAPH-2B820..CJK UNIFIED IDEOGRAPH-2CEA1
  2CEB0..2EBE0  ; Unified_Ideograph # Lo [7473] CJK UNIFIED IDEOGRAPH-2CEB0..CJK UNIFIED IDEOGRAPH-2EBE0
  30000..3134A  ; Unified_Ideograph # Lo [4939] CJK UNIFIED IDEOGRAPH-30000..CJK UNIFIED IDEOGRAPH-3134A
*/
static inline my_bool
my_uca_1400_is_other_han_unified_ideograph(my_wc_t code)
{
  return (code >= 0x3400 && code <= 0x4DBF) ||
         (code >= 0x20000 && code <= 0x2A6DF) ||
         (code >= 0x2A700 && code <= 0x2B738) ||
         (code >= 0x2B740 && code <= 0x2B81D) ||
         (code >= 0x2B820 && code <= 0x2CEA1) ||
         (code >= 0x2CEB0 && code <= 0x2EBE0) ||
         (code >= 0x30000 && code <= 0x3134A);
}


/*
  See section "Computing Implicit Weights" in
  https://unicode.org/reports/tr10/#Values_For_Base_Table
*/
static inline MY_UCA_IMPLICIT_WEIGHT
my_uca_1400_implicit_weight_primary(my_wc_t code)
{
  if (my_uca_1400_is_core_han_unified_ideograph(code))
    return my_uca_implicit_weight_primary_default(0xFB40, code);

  if (my_uca_1400_is_other_han_unified_ideograph(code))
    return my_uca_implicit_weight_primary_default(0xFB80, code);

  if (my_uca_1400_is_assigned_tangut(code))
    return my_uca_1400_implicit_weight_primary_tangut(code);

  if (my_uca_1400_is_assigned_nushu(code))
    return my_uca_1400_implicit_weight_primary_nushu(code);

  if (my_uca_1400_is_assigned_khitan_small_script(code))
    return my_uca_1400_implicit_weight_primary_khitan(code);

  /* Unassigned - Any other code point */
  return my_uca_implicit_weight_primary_default(0xFBC0, code);
}


#define MY_UCA1400_COLLATION_ID_POSSIBLE_MIN 2048
#define MY_UCA1400_COLLATION_ID_POSSIBLE_MAX 4095

static inline my_bool
my_collation_id_is_uca1400(uint id)
{
  return (my_bool) (id >= MY_UCA1400_COLLATION_ID_POSSIBLE_MIN &&
                    id <= MY_UCA1400_COLLATION_ID_POSSIBLE_MAX);
}


typedef struct my_uca1400_collation_definition_st
{
  const char * tailoring;
  const char * name;
  uint16 id_utf8mb3;
  uint16 id_utf8mb4;
  uint16 id_ucs2;
  uint16 id_utf16;
  uint16 id_utf32;
} MY_UCA1400_COLLATION_DEFINITION;


/*
  UCA1400 collation ID:

  1000 0000 0000   0x800  2048
  1111 1111 1111   0xFFF  4095
  1ccc tttt tPST

  c - charset ID (utf8mb3=0, utf8mb4=1, ucs2=2, utf16=3, utf32=4)
  p - PAD/NO PAD
  S - secondary level is enabled
  T - tertiary level is enabled
*/


static inline my_cs_encoding_t
my_uca1400_collation_id_to_charset_id(uint id)
{
  DBUG_ASSERT(id);
  return (my_cs_encoding_t) ((id >> 8) & 0x07);
}


static inline uint
my_uca1400_collation_id_to_tailoring_id(uint id)
{
  DBUG_ASSERT(id);
  return (id >> 3) & 0x1F;
}


static inline my_bool
my_uca1400_collation_id_to_nopad_flag(uint id)
{
  DBUG_ASSERT(id);
  return (my_bool) ((id >> 2) & 0x01);
}

static inline my_bool
my_uca1400_collation_id_to_secondary_level_flag(uint id)
{
  DBUG_ASSERT(id);
  return (my_bool) ((id >> 1) & 0x01);
}

static inline my_bool
my_uca1400_collation_id_to_tertiary_level_flag(uint id)
{
  DBUG_ASSERT(id);
  return (my_bool) ((id >> 0) & 0x01);
}

static inline uint
my_uca1400_collation_id_to_level_flags(uint id)
{
  my_bool secondary_level, tertiary_level;
  DBUG_ASSERT(id);
  secondary_level= my_uca1400_collation_id_to_secondary_level_flag(id);
  tertiary_level=  my_uca1400_collation_id_to_tertiary_level_flag(id);
  return (1 << MY_CS_LEVEL_BIT_PRIMARY) |
         (secondary_level ? 1 << MY_CS_LEVEL_BIT_SECONDARY : 0) |
         (tertiary_level  ? 1 << MY_CS_LEVEL_BIT_TERTIARY  : 0);
}


/*
  Return an UCA-14.0.0 collation properties using its ID.
*/
static inline uca_collation_def_param_t
my_uca1400_collation_param_by_id(uint id)
{
  uca_collation_def_param_t res;
  DBUG_ASSERT(id);
  res.cs_id= my_uca1400_collation_id_to_charset_id(id);
  res.tailoring_id= my_uca1400_collation_id_to_tailoring_id(id);
  res.nopad_flags= my_uca1400_collation_id_to_nopad_flag(id);
  res.level_flags= my_uca1400_collation_id_to_level_flags(id);
  return res;
}


uint
my_uca1400_make_builtin_collation_id(my_cs_encoding_t charset_id,
                                     uint tailoring_id,
                                     my_bool nopad,
                                     my_bool secondary_level,
                                     my_bool tertiary_level);

LEX_CSTRING
my_uca1400_collation_build_name(char *buffer, size_t buffer_size,
                                const LEX_CSTRING *cs_name,
                                const char *tailoring_name,
                                const uca_collation_def_param_t *prm);

my_bool
my_uca1400_collation_alloc_and_init(MY_CHARSET_LOADER *loader,
                                    LEX_CSTRING name,
                                    LEX_CSTRING comment,
                                    const uca_collation_def_param_t *param,
                                    uint id);

LEX_CSTRING my_ci_get_collation_name_uca1400_context(CHARSET_INFO *cs);

uint my_uca1400_collation_id_uca400_compat(uint id);

my_bool my_uca1400_collation_definitions_add(MY_CHARSET_LOADER *loader);


/* Exported data */
#define MY_UCA1400_COLLATION_DEFINITION_COUNT 26

extern MY_UCA1400_COLLATION_DEFINITION
my_uca1400_collation_definitions[MY_UCA1400_COLLATION_DEFINITION_COUNT];

extern MY_UCA_INFO my_uca_v1400;


extern MY_UCA_INFO my_uca1400_info_tailored[MY_CS_ENCODING_LAST+1]
                                      [MY_UCA1400_COLLATION_DEFINITION_COUNT];

#endif /* CTYPE_UCA_1400_H */
