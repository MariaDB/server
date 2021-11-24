#ifndef CTYPE_UCA_H
#define CTYPE_UCA_H
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

#define MY_UCA_VERSION_ID(x,y,z)  ((uint) ((x) * 100 + (y) * 10 + (z)))

/*
  Implicit weight handling is done according to
  the section "Computing Implicit Weights" in
    https://unicode.org/reports/tr10/#Values_For_Base_Table
  (as of Unicode 14.0.0)

  Implicit weights for a code CP are constructed as follows:
    [.AAAA.0020.0002][.BBBB.0000.0000]

  - There are two primary weights, depending on the character type and block.
  - There is one weight on the secondary and tertiary levels.

  AAAA and BBBB are computed using different formulas for:
  - Siniform ideographic scripts
  - Han
  - Unassigned characters
*/

typedef struct my_uca_implict_weight_t
{
  uint16 weight[2];
} MY_UCA_IMPLICIT_WEIGHT;


/*
  By default, implicit weights for a code CP are constructed as follows:
    [.AAAA.0020.0002][.BBBB.0000.0000]

  where AAAA and BBBB are :
    AAAA= BASE + (CP >> 15);
    BBBB= (CP & 0x7FFF) | 0x8000;

  This formula covers the following implicit weight subtypes:
  - Core Han Unified Ideographs
  - All other Han Unified Ideographs
  - Unassigned characters
  Every mentioned subtype passes a different BASE.

  This formula does not cover Siniform ideographic scripts.
  They are handled by separate functions.
*/
static inline MY_UCA_IMPLICIT_WEIGHT
my_uca_implicit_weight_primary_default(uint16 base, my_wc_t code)
{
  MY_UCA_IMPLICIT_WEIGHT res;
  res.weight[0]= (uint16) ((code >> 15) + base);
  res.weight[1]= (uint16) ((code & 0x7FFF)|0x8000);
  return res;
}


/**
  Calculate Unicode-5.2.0 implicit weight on the primary level.

  According to UCA, BASE is calculated as follows:
  - FB40 for Unified_Ideograph=True AND
             ((Block=CJK_Unified_Ideograph) OR
              (Block=CJK_Compatibility_Ideographs))
  - FB80 for Unified_Ideograph=True AND NOT
             ((Block=CJK_Unified_Ideograph) OR
              (Block=CJK_Compatibility_Ideographs))
  - FBC0 for any other code point

  But for Unicode-5.2.0 and Unicode-4.0.0 we used
  a simplified formula as implemented before.
*/
static inline MY_UCA_IMPLICIT_WEIGHT
my_uca_520_implicit_weight_primary(my_wc_t code)
{
  uint16 base;
  /*
  3400;<CJK Ideograph Extension A, First>
  4DB5;<CJK Ideograph Extension A, Last>
  4E00;<CJK Ideograph, First>
  9FA5;<CJK Ideograph, Last>
  */
  if (code >= 0x3400 && code <= 0x4DB5)
    base= 0xFB80;
  else if (code >= 0x4E00 && code <= 0x9FA5)
    base= 0xFB40;
  else
    base= 0xFBC0;

  return my_uca_implicit_weight_primary_default(base, code);
}


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


static inline MY_UCA_IMPLICIT_WEIGHT
my_uca_implicit_weight_primary(uint version, my_wc_t code)
{
  return version >= 1400 ?
         my_uca_1400_implicit_weight_primary(code) :
         my_uca_520_implicit_weight_primary(code);
}


static inline MY_UCA_IMPLICIT_WEIGHT
my_uca_implicit_weight_secondary()
{
  MY_UCA_IMPLICIT_WEIGHT res;
  res.weight[0]= 0x0020;
  res.weight[1]= 0;
  return res;
}


static inline MY_UCA_IMPLICIT_WEIGHT
my_uca_implicit_weight_tertiary()
{
  MY_UCA_IMPLICIT_WEIGHT res;
  res.weight[0]= 0x0002;
  res.weight[1]= 0;
  return res;
}


static inline MY_UCA_IMPLICIT_WEIGHT
my_uca_implicit_weight_quaternary()
{
  MY_UCA_IMPLICIT_WEIGHT res;
  res.weight[0]= 0x0001;
  res.weight[1]= 0;
  return res;
}


static inline MY_UCA_IMPLICIT_WEIGHT
my_uca_implicit_weight_on_level(uint version, my_wc_t code, uint level)
{
  switch (level) {
  case 0:
    return my_uca_implicit_weight_primary(version, code);
  case 1:
    return my_uca_implicit_weight_secondary();
  case 2:
    return my_uca_implicit_weight_tertiary();
  default:
    break;
  }
  return my_uca_implicit_weight_quaternary();
}


#endif /* CTYPE_UCA_H */
