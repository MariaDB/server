/* Copyright (c) 2000, 2013, Oracle and/or its affiliates.
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

/*
  A better inplementation of the UNIX ctype(3) library.
*/

#ifndef _m_ctype_h
#define _m_ctype_h

#include <my_attribute.h>
#include <m_string.h>

enum loglevel {
   ERROR_LEVEL=       0,
   WARNING_LEVEL=     1,
   INFORMATION_LEVEL= 2
};

#ifdef	__cplusplus
extern "C" {
#endif

#define MY_CS_NAME_SIZE			32
#define MY_CS_CTYPE_TABLE_SIZE		257
#define MY_CS_TO_LOWER_TABLE_SIZE	256
#define MY_CS_TO_UPPER_TABLE_SIZE	256
#define MY_CS_SORT_ORDER_TABLE_SIZE	256
#define MY_CS_TO_UNI_TABLE_SIZE		256

#define CHARSET_DIR	"charsets/"

#define my_wc_t ulong

#define MY_CS_REPLACEMENT_CHARACTER 0xFFFD

/**
  Maximum character length of a string produced by wc_to_printable().
  Note, wc_to_printable() is currently limited to BMP.
  One non-printable or non-convertable character can produce a string
  with at most 5 characters: \hhhh.
  If we ever modify wc_to_printable() to support supplementary characters,
  e.g. \+hhhhhh, this constant should be changed to 8.
  Note, maximum octet length of a wc_to_printable() result can be calculated
  as: (MY_CS_PRINTABLE_CHAR_LENGTH*cs->mbminlen).
*/
#define MY_CS_PRINTABLE_CHAR_LENGTH  5


/*
  On i386 we store Unicode->CS conversion tables for
  some character sets using Big-endian order,
  to copy two bytes at once.
  This gives some performance improvement.
*/
#ifdef __i386__
#define MB2(x)                (((x) >> 8) + (((x) & 0xFF) << 8))
#define MY_PUT_MB2(s, code)   { *((uint16*)(s))= (code); }
#else
#define MB2(x)                (x)
#define MY_PUT_MB2(s, code)   { (s)[0]= code >> 8; (s)[1]= code & 0xFF; }
#endif

typedef const struct my_charset_handler_st MY_CHARSET_HANDLER;
typedef const struct my_collation_handler_st MY_COLLATION_HANDLER;

typedef const struct unicase_info_st MY_UNICASE_INFO;
typedef const struct uni_ctype_st MY_UNI_CTYPE;
typedef const struct my_uni_idx_st MY_UNI_IDX;
typedef uint16 decimal_digits_t;

typedef struct unicase_info_char_st
{
  uint32 toupper;
  uint32 tolower;
  uint32 sort;
} MY_UNICASE_CHARACTER;


struct unicase_info_st
{
  my_wc_t maxchar;
  MY_UNICASE_CHARACTER **page;
};


extern MY_UNICASE_INFO my_unicase_default;
extern MY_UNICASE_INFO my_unicase_turkish;
extern MY_UNICASE_INFO my_unicase_mysql500;
extern MY_UNICASE_INFO my_unicase_unicode520;

#define MY_UCA_MAX_CONTRACTION 6
/*
  The DUCET tables in ctype-uca.c are dumped with a limit of 8 weights
  per character. cs->strxfrm_multiply is set to 8 for all UCA based collations.

  In language-specific UCA collations (with tailorings) we also do not allow
  a single character to have more than 8 weights to stay with the same
  strxfrm_multiply limit. Note, contractions are allowed to have twice longer
  weight strings (up to 16 weights). As a contraction consists of at
  least 2 characters, this makes sure that strxfrm_multiply ratio of 8
  is respected.
*/
#define MY_UCA_MAX_WEIGHT_SIZE (8+1)               /* Including 0 terminator */
#define MY_UCA_CONTRACTION_MAX_WEIGHT_SIZE (2*8+1) /* Including 0 terminator */
#define MY_UCA_WEIGHT_LEVELS   2

typedef struct my_contraction_t
{
  my_wc_t ch[MY_UCA_MAX_CONTRACTION];   /* Character sequence              */
  uint16 weight[MY_UCA_CONTRACTION_MAX_WEIGHT_SIZE];/* Its weight string, 0-terminated */
  my_bool with_context;
} MY_CONTRACTION;


typedef struct my_contraction_list_t
{
  size_t nitems;         /* Number of items in the list                  */
  MY_CONTRACTION *item;  /* List of contractions                         */
  char *flags;           /* Character flags, e.g. "is contraction head") */
} MY_CONTRACTIONS;

my_bool my_uca_can_be_contraction_head(const MY_CONTRACTIONS *c, my_wc_t wc);
my_bool my_uca_can_be_contraction_tail(const MY_CONTRACTIONS *c, my_wc_t wc);
const uint16 *my_uca_contraction2_weight(const MY_CONTRACTIONS *c,
                                         my_wc_t wc1, my_wc_t wc2);


/* Collation weights on a single level (e.g. primary, secondary, tertiarty) */
typedef struct my_uca_level_info_st
{
  my_wc_t maxchar;
  uchar   *lengths;
  uint16  **weights;
  MY_CONTRACTIONS contractions;
  uint    levelno;
} MY_UCA_WEIGHT_LEVEL;


typedef struct uca_info_st
{
  MY_UCA_WEIGHT_LEVEL level[MY_UCA_WEIGHT_LEVELS];

  /* Logical positions */
  my_wc_t first_non_ignorable;
  my_wc_t last_non_ignorable;
  my_wc_t first_primary_ignorable;
  my_wc_t last_primary_ignorable;
  my_wc_t first_secondary_ignorable;
  my_wc_t last_secondary_ignorable;
  my_wc_t first_tertiary_ignorable;
  my_wc_t last_tertiary_ignorable;
  my_wc_t first_trailing;
  my_wc_t last_trailing;
  my_wc_t first_variable;
  my_wc_t last_variable;

} MY_UCA_INFO;



extern MY_UCA_INFO my_uca_v400;


struct uni_ctype_st
{
  uchar  pctype;
  const uchar  *ctype;
};

extern MY_UNI_CTYPE my_uni_ctype[256];

/* wm_wc and wc_mb return codes */
#define MY_CS_ILSEQ	0     /* Wrong by sequence: wb_wc                   */
#define MY_CS_ILUNI	0     /* Cannot encode Unicode to charset: wc_mb    */
#define MY_CS_TOOSMALL  -101  /* Need at least one byte:    wc_mb and mb_wc */
#define MY_CS_TOOSMALL2 -102  /* Need at least two bytes:   wc_mb and mb_wc */
#define MY_CS_TOOSMALL3 -103  /* Need at least three bytes: wc_mb and mb_wc */
/* These following three are currently not really used */
#define MY_CS_TOOSMALL4 -104  /* Need at least 4 bytes: wc_mb and mb_wc */
#define MY_CS_TOOSMALL5 -105  /* Need at least 5 bytes: wc_mb and mb_wc */
#define MY_CS_TOOSMALL6 -106  /* Need at least 6 bytes: wc_mb and mb_wc */
/* A helper macros for "need at least n bytes" */
#define MY_CS_TOOSMALLN(n)    (-100-(n))

#define MY_CS_MBMAXLEN  6     /* Maximum supported mbmaxlen */
#define MY_CS_IS_TOOSMALL(rc) ((rc) >= MY_CS_TOOSMALL6 && (rc) <= MY_CS_TOOSMALL)

#define MY_SEQ_INTTAIL	1
#define MY_SEQ_SPACES	2
#define MY_SEQ_NONSPACES 3 /* Skip non-space characters, including bad bytes */

        /* My charsets_list flags */
#define MY_CS_COMPILED  1      /* compiled-in sets               */
#define MY_CS_CONFIG    2      /* sets that have a *.conf file   */
#define MY_CS_INDEX     4      /* sets listed in the Index file  */
#define MY_CS_LOADED    8      /* sets that are currently loaded */
#define MY_CS_BINSORT	16     /* if binary sort order           */
#define MY_CS_PRIMARY	32     /* if primary collation           */
#define MY_CS_STRNXFRM	64     /* if strnxfrm is used for sort   */
#define MY_CS_UNICODE	128    /* is a charset is BMP Unicode    */
#define MY_CS_READY	256    /* if a charset is initialized    */
#define MY_CS_AVAILABLE	512    /* If either compiled-in or loaded*/
#define MY_CS_CSSORT	1024   /* if case sensitive sort order   */	
#define MY_CS_HIDDEN	2048   /* don't display in SHOW          */	
#define MY_CS_PUREASCII 4096   /* if a charset is pure ascii     */
#define MY_CS_NONASCII  8192   /* if not ASCII-compatible        */
#define MY_CS_UNICODE_SUPPLEMENT 16384 /* Non-BMP Unicode characters */
#define MY_CS_LOWER_SORT 32768 /* If use lower case as weight   */
#define MY_CS_STRNXFRM_BAD_NWEIGHTS 0x10000 /* strnxfrm ignores "nweights" */
#define MY_CS_NOPAD   0x20000  /* if does not ignore trailing spaces */
#define MY_CS_NON1TO1 0x40000  /* Has a complex mapping from characters
                                  to weights, e.g. contractions, expansions,
                                  ignorable characters */
#define MY_CHARSET_UNDEFINED 0

/* Character repertoire flags */
typedef enum enum_repertoire_t
{
  MY_REPERTOIRE_NONE=        0,
  MY_REPERTOIRE_ASCII=       1, /* Pure ASCII            U+0000..U+007F */
  MY_REPERTOIRE_EXTENDED=    2, /* Extended characters:  U+0080..U+FFFF */
  MY_REPERTOIRE_UNICODE30=   3  /* ASCII | EXTENDED:     U+0000..U+FFFF */
} my_repertoire_t;


/* Flags for strxfrm */
#define MY_STRXFRM_LEVEL1          0x00000001 /* for primary weights   */
#define MY_STRXFRM_LEVEL2          0x00000002 /* for secondary weights */
#define MY_STRXFRM_LEVEL3          0x00000004 /* for tertiary weights  */
#define MY_STRXFRM_LEVEL4          0x00000008 /* fourth level weights  */
#define MY_STRXFRM_LEVEL5          0x00000010 /* fifth level weights   */
#define MY_STRXFRM_LEVEL6          0x00000020 /* sixth level weights   */
#define MY_STRXFRM_LEVEL_ALL       0x0000003F /* Bit OR for the above six */
#define MY_STRXFRM_NLEVELS         6          /* Number of possible levels*/

#define MY_STRXFRM_PAD_WITH_SPACE  0x00000040 /* if pad result with spaces */
#define MY_STRXFRM_PAD_TO_MAXLEN   0x00000080 /* if pad tail(for filesort) */

#define MY_STRXFRM_DESC_LEVEL1     0x00000100 /* if desc order for level1 */
#define MY_STRXFRM_DESC_LEVEL2     0x00000200 /* if desc order for level2 */
#define MY_STRXFRM_DESC_LEVEL3     0x00000300 /* if desc order for level3 */
#define MY_STRXFRM_DESC_LEVEL4     0x00000800 /* if desc order for level4 */
#define MY_STRXFRM_DESC_LEVEL5     0x00001000 /* if desc order for level5 */
#define MY_STRXFRM_DESC_LEVEL6     0x00002000 /* if desc order for level6 */
#define MY_STRXFRM_DESC_SHIFT      8

#define MY_STRXFRM_UNUSED_00004000 0x00004000 /* for future extensions     */
#define MY_STRXFRM_UNUSED_00008000 0x00008000 /* for future extensions     */

#define MY_STRXFRM_REVERSE_LEVEL1  0x00010000 /* if reverse order for level1 */
#define MY_STRXFRM_REVERSE_LEVEL2  0x00020000 /* if reverse order for level2 */
#define MY_STRXFRM_REVERSE_LEVEL3  0x00040000 /* if reverse order for level3 */
#define MY_STRXFRM_REVERSE_LEVEL4  0x00080000 /* if reverse order for level4 */
#define MY_STRXFRM_REVERSE_LEVEL5  0x00100000 /* if reverse order for level5 */
#define MY_STRXFRM_REVERSE_LEVEL6  0x00200000 /* if reverse order for level6 */
#define MY_STRXFRM_REVERSE_SHIFT   16

/*
   Collation IDs for MariaDB that should not conflict with MySQL.
   We reserve 256..511, because MySQL will most likely use this range
   when the range 0..255 is full.

   We use the next 256 IDs starting from 512 and divide
   them into 8 chunks, 32 collations each, as follows:

   512 + (0..31)    for single byte collations (e.g. latin9)
   512 + (32..63)   reserved (e.g. for utf32le, or more single byte collations)
   512 + (64..95)   for utf8
   512 + (96..127)  for utf8mb4
   512 + (128..159) for ucs2
   512 + (160..192) for utf16
   512 + (192..223) for utf16le
   512 + (224..255) for utf32
*/
#define MY_PAGE2_COLLATION_ID_8BIT     0x200
#define MY_PAGE2_COLLATION_ID_RESERVED 0x220
#define MY_PAGE2_COLLATION_ID_UTF8     0x240
#define MY_PAGE2_COLLATION_ID_UTF8MB4  0x260
#define MY_PAGE2_COLLATION_ID_UCS2     0x280
#define MY_PAGE2_COLLATION_ID_UTF16    0x2A0
#define MY_PAGE2_COLLATION_ID_UTF16LE  0x2C0
#define MY_PAGE2_COLLATION_ID_UTF32    0x2E0

struct my_uni_idx_st
{
  uint16 from;
  uint16 to;
  const uchar *tab;
};

typedef struct
{
  uint beg;
  uint end;
  uint mb_len;
} my_match_t;

enum my_lex_states
{
  MY_LEX_START, MY_LEX_CHAR, MY_LEX_IDENT, 
  MY_LEX_IDENT_SEP, MY_LEX_IDENT_START,
  MY_LEX_REAL, MY_LEX_HEX_NUMBER, MY_LEX_BIN_NUMBER,
  MY_LEX_CMP_OP, MY_LEX_LONG_CMP_OP, MY_LEX_STRING, MY_LEX_COMMENT, MY_LEX_END,
  MY_LEX_OPERATOR_OR_IDENT, MY_LEX_NUMBER_IDENT, MY_LEX_INT_OR_REAL,
  MY_LEX_REAL_OR_POINT, MY_LEX_BOOL, MY_LEX_EOL, MY_LEX_ESCAPE, 
  MY_LEX_LONG_COMMENT, MY_LEX_END_LONG_COMMENT, MY_LEX_SEMICOLON, 
  MY_LEX_SET_VAR, MY_LEX_USER_END, MY_LEX_HOSTNAME, MY_LEX_SKIP, 
  MY_LEX_USER_VARIABLE_DELIMITER, MY_LEX_SYSTEM_VAR,
  MY_LEX_IDENT_OR_KEYWORD,
  MY_LEX_IDENT_OR_HEX, MY_LEX_IDENT_OR_BIN, MY_LEX_IDENT_OR_NCHAR,
  MY_LEX_STRING_OR_DELIMITER, MY_LEX_MINUS_OR_COMMENT, MY_LEX_PLACEHOLDER,
  MY_LEX_COMMA
};

struct charset_info_st;

typedef struct my_charset_loader_st
{
  char error[128];
  void *(*once_alloc)(size_t);
  void *(*malloc)(size_t);
  void *(*realloc)(void *, size_t);
  void (*free)(void *);
  void (*reporter)(enum loglevel, const char *format, ...);
  int  (*add_collation)(struct charset_info_st *cs);
} MY_CHARSET_LOADER;


extern int (*my_string_stack_guard)(int);

/* See strings/CHARSET_INFO.txt for information about this structure  */
struct my_collation_handler_st
{
  my_bool (*init)(struct charset_info_st *, MY_CHARSET_LOADER *);
  /* Collation routines */
  int     (*strnncoll)(CHARSET_INFO *,
		       const uchar *, size_t, const uchar *, size_t, my_bool);
  int     (*strnncollsp)(CHARSET_INFO *,
                         const uchar *, size_t, const uchar *, size_t);
  /*
    strnncollsp_nchars() - similar to strnncollsp() but assumes that both
                           strings were originally CHAR(N) values with the
                           same N, then were optionally space-padded,
                           or optionally space-trimmed.

                           In other words, this function compares in the way
                           if we insert both values into a CHAR(N) column
                           and then compare the two column values.

    It compares the same amount of characters from the two strings.
    This is especially important for NOPAD collations.

    If CHAR_LENGTH of the two strings are different,
    the shorter string is virtually padded with trailing spaces
    up to CHAR_LENGTH of the longer string, to guarantee that the
    same amount of characters are compared.
    This is important if the two CHAR(N) strings are space-trimmed 
    (e.g. like in InnoDB compact format for CHAR).

    The function compares not more than "nchars" characters only.
    This can be useful to compare CHAR(N) space-padded strings
    (when the exact N is known) without having to truncate them before
    the comparison.

    For example, Field_string stores a "CHAR(3) CHARACTER SET utf8mb4" value
    of "aaa" as 12 bytes in a record buffer:
    - 3 bytes of the actual data, followed by
    - 9 bytes of spaces (just fillers, not real data)
    The caller can pass nchars=3 to compare CHAR(3) record values.
    In such case, the comparator won't go inside the 9 bytes of the fillers.

    If N is not known, the caller can pass max(len1,len2) as the "nchars" value
    (i.e. the maximum of the OCTET_LENGTH of the two strings).

    Notes on complex collations.

    This function counts contraction parts as individual characters.
    For example, the Czech letter 'ch' (in Czech collations)
    is ordinarily counted by the "nchars" limit as TWO characters
    (although it is only one letter).
    This corresponds to what CHAR(N) does in INSERT.

    If the "nchars" limit tears apart a contraction, only the part fitting
    into "nchars" characters is used. For example, in case of a Czech collation,
    the string "ach" with nchars=2 is compared as 'ac': the contraction
    'ch' is torn apart and the letter 'c' acts as an individual character.
    This emulates the same comparison result with the scenario when we insert
    'ach' into a CHAR(2) column and then compare it.
  */
  int     (*strnncollsp_nchars)(CHARSET_INFO *,
                                const uchar *str1, size_t len1,
                                const uchar *str2, size_t len2,
                                size_t nchars);
  size_t     (*strnxfrm)(CHARSET_INFO *,
                         uchar *dst, size_t dstlen, uint nweights,
                         const uchar *src, size_t srclen, uint flags);
  size_t    (*strnxfrmlen)(CHARSET_INFO *, size_t); 
  my_bool (*like_range)(CHARSET_INFO *,
			const char *s, size_t s_length,
			pchar w_prefix, pchar w_one, pchar w_many, 
			size_t res_length,
			char *min_str, char *max_str,
			size_t *min_len, size_t *max_len);
  int     (*wildcmp)(CHARSET_INFO *,
  		     const char *str,const char *str_end,
                     const char *wildstr,const char *wildend,
                     int escape,int w_one, int w_many);

  int  (*strcasecmp)(CHARSET_INFO *, const char *, const char *);
  
  uint (*instr)(CHARSET_INFO *,
                const char *b, size_t b_length,
                const char *s, size_t s_length,
                my_match_t *match, uint nmatch);
  
  /* Hash calculation */
  void (*hash_sort)(CHARSET_INFO *cs, const uchar *key, size_t len,
		    ulong *nr1, ulong *nr2); 
  my_bool (*propagate)(CHARSET_INFO *cs, const uchar *str, size_t len);
  /*
    Make minimum and maximum strings for the collation.
    Put not more than "nchars" characters.
  */
  size_t (*min_str)(CHARSET_INFO *cs, uchar *dst, size_t dstlen, size_t nchars);
  size_t (*max_str)(CHARSET_INFO *cs, uchar *dst, size_t dstlen, size_t nchars);
};

extern MY_COLLATION_HANDLER my_collation_8bit_bin_handler;
extern MY_COLLATION_HANDLER my_collation_8bit_simple_ci_handler;
extern MY_COLLATION_HANDLER my_collation_8bit_nopad_bin_handler;
extern MY_COLLATION_HANDLER my_collation_8bit_simple_nopad_ci_handler;

/* Some typedef to make it easy for C++ to make function pointers */
typedef int (*my_charset_conv_mb_wc)(CHARSET_INFO *, my_wc_t *,
                                     const uchar *, const uchar *);
typedef int (*my_charset_conv_wc_mb)(CHARSET_INFO *, my_wc_t,
                                     uchar *, uchar *);
typedef size_t (*my_charset_conv_case)(CHARSET_INFO *,
                                       const char *, size_t, char *, size_t);

/*
  A structure to return the statistics of a native string copying,
  when no Unicode conversion is involved.

  The structure is OK to be uninitialized before calling a copying routine.
  A copying routine must populate the structure as follows:
    - m_source_end_pos must be set by to a non-NULL value
      in the range of the input string.
    - m_well_formed_error_pos must be set to NULL if the string was
      well formed, or to the position of the leftmost bad byte sequence.
*/
typedef struct
{
  const char *m_source_end_pos;        /* Position where reading stopped */
  const char *m_well_formed_error_pos; /* Position where a bad byte was found*/
} MY_STRCOPY_STATUS;


/*
  A structure to return the statistics of a Unicode string conversion.
*/
typedef struct
{
  const char *m_cannot_convert_error_pos;
} MY_STRCONV_STATUS;


/* See strings/CHARSET_INFO.txt about information on this structure  */
struct my_charset_handler_st
{
  my_bool (*init)(struct charset_info_st *, MY_CHARSET_LOADER *loader);
  /* Multibyte routines */
  size_t  (*numchars)(CHARSET_INFO *, const char *b, const char *e);
  size_t  (*charpos)(CHARSET_INFO *, const char *b, const char *e,
                     size_t pos);
  size_t  (*lengthsp)(CHARSET_INFO *, const char *ptr, size_t length);
  size_t  (*numcells)(CHARSET_INFO *, const char *b, const char *e);
  
  /* Unicode conversion */
  my_charset_conv_mb_wc mb_wc;
  my_charset_conv_wc_mb wc_mb;

  /* CTYPE scanner */
  int (*ctype)(CHARSET_INFO *cs, int *ctype,
               const uchar *s, const uchar *e);
  
  /* Functions for case and sort conversion */
  size_t  (*caseup_str)(CHARSET_INFO *, char *);
  size_t  (*casedn_str)(CHARSET_INFO *, char *);

  my_charset_conv_case caseup;
  my_charset_conv_case casedn;

  /* Charset dependent snprintf() */
  size_t (*snprintf)(CHARSET_INFO *, char *to, size_t n,
                     const char *fmt,
                     ...) ATTRIBUTE_FORMAT_FPTR(printf, 4, 5);
  size_t (*long10_to_str)(CHARSET_INFO *, char *to, size_t n,
                          int radix, long int val);
  size_t (*longlong10_to_str)(CHARSET_INFO *, char *to, size_t n,
                              int radix, longlong val);
  
  void (*fill)(CHARSET_INFO *, char *to, size_t len, int fill);
  
  /* String-to-number conversion routines */
  long        (*strntol)(CHARSET_INFO *, const char *s, size_t l,
			 int base, char **e, int *err);
  ulong      (*strntoul)(CHARSET_INFO *, const char *s, size_t l,
			 int base, char **e, int *err);
  longlong   (*strntoll)(CHARSET_INFO *, const char *s, size_t l,
			 int base, char **e, int *err);
  ulonglong (*strntoull)(CHARSET_INFO *, const char *s, size_t l,
			 int base, char **e, int *err);
  double      (*strntod)(CHARSET_INFO *, char *s, size_t l, char **e,
			 int *err);
  longlong    (*strtoll10)(CHARSET_INFO *cs,
                           const char *nptr, char **endptr, int *error);
  ulonglong   (*strntoull10rnd)(CHARSET_INFO *cs,
                                const char *str, size_t length,
                                int unsigned_fl,
                                char **endptr, int *error);
  size_t        (*scan)(CHARSET_INFO *, const char *b, const char *e,
                        int sq);

  /* String copying routines and helpers for them */
  /*
    charlen() - calculate length of the left-most character in bytes.
    @param  cs    Character set
    @param  str   The beginning of the string
    @param  end   The end of the string
    
    @return       MY_CS_ILSEQ if a bad byte sequence was found.
    @return       MY_CS_TOOSMALLN(x) if the string ended unexpectedly.
    @return       a positive number in the range 1..mbmaxlen,
                  if a valid character was found.
  */
  int (*charlen)(CHARSET_INFO *cs, const uchar *str, const uchar *end);
  /*
    well_formed_char_length() - returns character length of a string.
    
    @param cs          Character set
    @param str         The beginning of the string
    @param end         The end of the string
    @param nchars      Not more than "nchars" left-most characters are checked.
    @param status[OUT] Additional statistics is returned here.
                       "status" can be uninitialized before the call,
                       and it is fully initialized after the call.
    
    status->m_source_end_pos is set to the position where reading stopped.
    
    If a bad byte sequence is found, the function returns immediately and
    status->m_well_formed_error_pos is set to the position where a bad byte
    sequence was found.
    
    status->m_well_formed_error_pos is set to NULL if no bad bytes were found.
    If status->m_well_formed_error_pos is NULL after the call, that means:
    - either the function reached the end of the string,
    - or all "nchars" characters were read.
    The caller can check status->m_source_end_pos to detect which of these two
    happened.
  */
  size_t (*well_formed_char_length)(CHARSET_INFO *cs,
                                    const char *str, const char *end,
                                    size_t nchars,
                                    MY_STRCOPY_STATUS *status);

  /*
    copy_fix() - copy a string, replace bad bytes to '?'.
    Not more than "nchars" characters are copied.

    status->m_source_end_pos is set to a position in the range
    between "src" and "src + src_length", where reading stopped.

    status->m_well_formed_error_pos is set to NULL if the string
    in the range "src" and "status->m_source_end_pos" was well formed,
    or is set to a position between "src" and "src + src_length" where
    the leftmost bad byte sequence was found.
  */
  size_t  (*copy_fix)(CHARSET_INFO *,
                      char *dst, size_t dst_length,
                      const char *src, size_t src_length,
                      size_t nchars, MY_STRCOPY_STATUS *status);
  /**
    Write a character to the target string, using its native code.
    For Unicode character sets (utf8, ucs2, utf16, utf16le, utf32, filename)
    native codes are equivalent to Unicode code points.
    For 8bit character sets the native code is just the byte value.
    For Asian characters sets:
    - MB1 native code is just the byte value (e.g. on the ASCII range)
    - MB2 native code is ((b0 << 8) + b1).
    - MB3 native code is ((b0 <<16) + (b1 << 8) + b2)
    Note, CHARSET_INFO::min_sort_char and CHARSET_INFO::max_sort_char
    are defined in native notation and should be written using
    my_ci_native_to_mb() rather than my_ci_wc_mb().
  */
  my_charset_conv_wc_mb native_to_mb;
  my_charset_conv_wc_mb wc_to_printable;
};

extern MY_CHARSET_HANDLER my_charset_8bit_handler;
extern MY_CHARSET_HANDLER my_charset_ucs2_handler;
extern MY_CHARSET_HANDLER my_charset_utf8mb3_handler;


/*
  We define this CHARSET_INFO_DEFINED here to prevent a repeat of the
  typedef in hash.c, which will cause a compiler error.
*/
#define CHARSET_INFO_DEFINED


/* See strings/CHARSET_INFO.txt about information on this structure  */
struct charset_info_st
{
  uint      number;
  uint      primary_number;
  uint      binary_number;
  uint      state;
  LEX_CSTRING cs_name;
  LEX_CSTRING coll_name;
  const char *comment;
  const char *tailoring;
  const uchar *m_ctype;
  const uchar *to_lower;
  const uchar *to_upper;
  const uchar *sort_order;
  MY_UCA_INFO *uca;
  const uint16 *tab_to_uni;
  MY_UNI_IDX  *tab_from_uni;
  MY_UNICASE_INFO *caseinfo;
  const uchar  *state_map;
  const uchar  *ident_map;
  uint      strxfrm_multiply;
  uchar     caseup_multiply;
  uchar     casedn_multiply;
  uint      mbminlen;
  uint      mbmaxlen;
  /*
    min_sort_char and max_sort_char represent the minimum
    and the maximum character in the collation respectively.

    For Unicode collations, these numbers are Unicode code points.
    For non-Unicode collations these numbers are native character codes.
    For example, in all 8bit collations these numbers are
    in the range 0x00..0xFF.

    min_sort_char and max_sort_char normally should not be used directly.
    They are used internally in the following virtual functions:
    - MY_COLLATION_HANDLER::like_range()
    - MY_COLLATION_HANDLER::min_str()
    - MY_COLLATION_HANDLER::max_str()
  */
  my_wc_t   min_sort_char;
  my_wc_t   max_sort_char;
  uchar     pad_char;
  my_bool   escape_with_backslash_is_dangerous;
  uchar     levels_for_order;
  
  MY_CHARSET_HANDLER *cset;
  MY_COLLATION_HANDLER *coll;

#ifdef __cplusplus
  /* Character set routines */
  bool use_mb() const
  {
    return mbmaxlen > 1;
  }

  size_t numchars(const char *b, const char *e) const
  {
    return (cset->numchars)(this, b, e);
  }

  size_t charpos(const char *b, const char *e, size_t pos) const
  {
    return (cset->charpos)(this, b, e, pos);
  }
  size_t charpos(const uchar *b, const uchar *e, size_t pos) const
  {
    return (cset->charpos)(this, (const char *) b, (const char*) e, pos);
  }

  size_t lengthsp(const char *str, size_t length) const
  {
    return (cset->lengthsp)(this, str, length);
  }

  size_t numcells(const char *b, const char *e) const
  {
    return (cset->numcells)(this, b, e);
  }

  size_t caseup(const char *src, size_t srclen,
                char *dst, size_t dstlen) const
  {
    return (cset->caseup)(this, src, srclen, dst, dstlen);
  }

  size_t casedn(const char *src, size_t srclen,
                char *dst, size_t dstlen) const
  {
    return (cset->casedn)(this, src, srclen, dst, dstlen);
  }

  size_t long10_to_str(char *dst, size_t dstlen,
                       int radix, long int val) const
  {
    return (cset->long10_to_str)(this, dst, dstlen, radix, val);
  }

  size_t (longlong10_to_str)(char *dst, size_t dstlen,
                             int radix, longlong val) const
  {
    return (cset->longlong10_to_str)(this, dst, dstlen, radix, val);
  }

  int mb_wc(my_wc_t *wc, const uchar *b, const uchar *e) const
  {
    return (cset->mb_wc)(this, wc, b, e);
  }

  int wc_mb(my_wc_t wc, uchar *s, uchar *e) const
  {
    return (cset->wc_mb)(this, wc, s, e);
  }

  int native_to_mb(my_wc_t wc, uchar *s, uchar *e) const
  {
    return (cset->native_to_mb)(this, wc, s, e);
  }

  int wc_to_printable(my_wc_t wc, uchar *s, uchar *e) const
  {
    return (cset->wc_to_printable)(this, wc, s, e);
  }

  int ctype(int *to, const uchar *s, const uchar *e) const
  {
    return (cset->ctype)(this, to, s, e);
  }

  void fill(char *to, size_t len, int ch) const
  {
    (cset->fill)(this, to, len, ch);
  }

  long strntol(const char *str, size_t length,
               int base, char **endptr, int *error) const
  {
    return (cset->strntol)(this, str, length, base, endptr, error);
  }

  ulong strntoul(const char *str, size_t length,
                 int base, char **endptr, int *error) const
  {
    return (cset->strntoul)(this, str, length, base, endptr, error);
  }

  longlong strntoll(const char *str, size_t length,
                    int base, char **endptr, int *error) const
  {
    return (cset->strntoll)(this, str, length, base, endptr, error);
  }

  ulonglong strntoull(const char *str, size_t length,
                      int base, char **endptr, int *error) const
  {
    return (cset->strntoull)(this, str, length, base, endptr, error);
  }

  double strntod(char *str, size_t length,
                 char **endptr, int *error) const
  {
    return (cset->strntod)(this, str, length, endptr, error);
  }

  longlong strtoll10(const char *str, char **endptr, int *error) const
  {
    return (cset->strtoll10)(this, str, endptr, error);
  }

  ulonglong strntoull10rnd(const char *str, size_t length, int unsigned_fl,
                           char **endptr, int *error) const
  {
    return (cset->strntoull10rnd)(this, str, length, unsigned_fl, endptr, error);
  }

  size_t scan(const char *b, const char *e, int seq) const
  {
    return (cset->scan)(this, b, e, seq);
  }

  int charlen(const uchar *str, const uchar *end) const
  {
    return (cset->charlen)(this, str, end);
  }
  int charlen(const char *str, const char *end) const
  {
    return (cset->charlen)(this, (const uchar *) str, (const uchar *) end);
  }

  uint charlen_fix(const uchar *str, const uchar *end) const
  {
    int char_length= (cset->charlen)(this, str, end);
    DBUG_ASSERT(str < end);
    return char_length > 0 ? (uint) char_length : (uint) 1U;
  }
  uint charlen_fix(const char *str, const char *end) const
  {
    return charlen_fix((const uchar *) str, (const uchar *) end);
  }

  size_t well_formed_char_length(const char *str, const char *end,
                                 size_t nchars,
                                 MY_STRCOPY_STATUS *status) const
  {
    return (cset->well_formed_char_length)(this, str, end, nchars, status);
  }

  size_t copy_fix(char *dst, size_t dst_length,
                  const char *src, size_t src_length,
                  size_t nchars, MY_STRCOPY_STATUS *status) const
  {
    return (cset->copy_fix)(this, dst, dst_length, src, src_length, nchars,
                          status);
  }

  /* Collation routines */
  int strnncoll(const uchar *a, size_t alen,
                const uchar *b, size_t blen, my_bool b_is_prefix= FALSE) const
  {
    return (coll->strnncoll)(this, a, alen, b, blen, b_is_prefix);
  }
  int strnncoll(const char *a, size_t alen,
                const char *b, size_t blen, my_bool b_is_prefix= FALSE) const
  {
    return (coll->strnncoll)(this,
                             (const uchar *) a, alen,
                             (const uchar *) b, blen, b_is_prefix);
  }

  int strnncollsp(const uchar *a, size_t alen,
                  const uchar *b, size_t blen) const
  {
    return (coll->strnncollsp)(this, a, alen, b, blen);
  }
  int strnncollsp(const char *a, size_t alen,
                  const char *b, size_t blen) const
  {
    return (coll->strnncollsp)(this, (uchar *) a, alen, (uchar *) b, blen);
  }

  size_t strnxfrm(char *dst, size_t dstlen, uint nweights,
                  const char *src, size_t srclen, uint flags) const
  {
    return (coll->strnxfrm)(this,
                            (uchar *) dst, dstlen, nweights,
                            (const uchar *) src, srclen, flags);
  }
  size_t strnxfrm(uchar *dst, size_t dstlen, uint nweights,
                  const uchar *src, size_t srclen, uint flags) const
  {
    return (coll->strnxfrm)(this,
                            dst, dstlen, nweights,
                            src, srclen, flags);
  }
  size_t strnxfrm(uchar *dst, size_t dstlen,
                  const uchar *src, size_t srclen) const
  {
    return (coll->strnxfrm)(this,
                            dst, dstlen, (uint) dstlen,
                            src, srclen, MY_STRXFRM_PAD_WITH_SPACE);
  }

  size_t strnxfrmlen(size_t length) const
  {
    return (coll->strnxfrmlen)(this, length);
  }

  my_bool like_range(const char *s, size_t s_length,
                     pchar w_prefix, pchar w_one, pchar w_many,
                     size_t res_length,
                     char *min_str, char *max_str,
                     size_t *min_len, size_t *max_len) const
  {
    return (coll->like_range)(this, s, s_length,
                              w_prefix, w_one, w_many,
                              res_length, min_str, max_str,
                              min_len, max_len);
  }

  int wildcmp(const char *str,const char *str_end,
              const char *wildstr,const char *wildend,
              int escape,int w_one, int w_many) const
  {
    return (coll->wildcmp)(this, str, str_end, wildstr, wildend, escape, w_one, w_many);
  }

  uint instr(const char *b, size_t b_length,
             const char *s, size_t s_length,
             my_match_t *match, uint nmatch) const
  {
    return (coll->instr)(this, b, b_length, s, s_length, match, nmatch);
  }

  void hash_sort(const uchar *key, size_t len, ulong *nr1, ulong *nr2) const
  {
    (coll->hash_sort)(this, key, len, nr1, nr2);
  }

  my_bool propagate(const uchar *str, size_t len) const
  {
    return (coll->propagate)(this, str, len);
  }

  size_t min_str(uchar *dst, size_t dstlen, size_t nchars) const
  {
    return (coll->min_str)(this, dst, dstlen, nchars);
  }

  size_t max_str(uchar *dst, size_t dstlen, size_t nchars) const
  {
    return (coll->max_str)(this, dst, dstlen, nchars);
  }

#endif /* __cplusplus */
};


/* Character set routines */

static inline my_bool
my_ci_init_charset(struct charset_info_st *ci, MY_CHARSET_LOADER *loader)
{
  if (!ci->cset->init)
    return FALSE;
  return (ci->cset->init)(ci, loader);
}


static inline my_bool
my_ci_use_mb(CHARSET_INFO *ci)
{
  return ci->mbmaxlen > 1 ? TRUE : FALSE;
}

static inline size_t
my_ci_numchars(CHARSET_INFO *cs, const char *b, const char *e)
{
  return (cs->cset->numchars)(cs, b, e);
}

static inline size_t
my_ci_charpos(CHARSET_INFO *cs, const char *b, const char *e, size_t pos)
{
  return (cs->cset->charpos)(cs, b, e, pos);
}

static inline size_t
my_ci_lengthsp(CHARSET_INFO *cs, const char *str, size_t length)
{
  return (cs->cset->lengthsp)(cs, str, length);
}

static inline size_t
my_ci_numcells(CHARSET_INFO *cs, const char *b, const char *e)
{
  return (cs->cset->numcells)(cs, b, e);
}

static inline size_t
my_ci_caseup(CHARSET_INFO *ci,
             const char *src, size_t srclen,
             char *dst, size_t dstlen)
{
  return (ci->cset->caseup)(ci, src, srclen, dst, dstlen);
}

static inline size_t
my_ci_casedn(CHARSET_INFO *ci,
             const char *src, size_t srclen,
             char *dst, size_t dstlen)
{
  return (ci->cset->casedn)(ci, src, srclen, dst, dstlen);
}

static inline size_t
my_ci_long10_to_str(CHARSET_INFO *cs, char *dst, size_t dstlen,
                    int radix, long int val)
{
  return (cs->cset->long10_to_str)(cs, dst, dstlen, radix, val);
}

static inline size_t
my_ci_longlong10_to_str(CHARSET_INFO *cs, char *dst, size_t dstlen,
                        int radix, longlong val)
{
  return (cs->cset->longlong10_to_str)(cs, dst, dstlen, radix, val);
}

#define my_ci_mb_wc(s, pwc, b, e)        ((s)->cset->mb_wc)(s, pwc, b, e)
#define my_ci_wc_mb(s, wc, b, e)         ((s)->cset->wc_mb)(s, wc, b, e)
#define my_ci_native_to_mb(s, wc, b, e)  ((s)->cset->native_to_mb)(s, wc, b, e)
#define my_ci_ctype(s, pctype, b, e)     ((s)->cset->ctype)(s, pctype, b, e)

static inline void
my_ci_fill(CHARSET_INFO *cs, char *to, size_t len, int ch)
{
  (cs->cset->fill)(cs, to, len, ch);
}

static inline long
my_ci_strntol(CHARSET_INFO *cs, const char *str, size_t length,
              int base, char **endptr, int *error)
{
  return (cs->cset->strntol)(cs, str, length, base, endptr, error);
}

static inline ulong
my_ci_strntoul(CHARSET_INFO *cs, const char *str, size_t length,
               int base, char **endptr, int *error)
{
  return (cs->cset->strntoul)(cs, str, length, base, endptr, error);
}

static inline longlong
my_ci_strntoll(CHARSET_INFO *cs, const char *str, size_t length,
               int base, char **endptr, int *error)
{
  return (cs->cset->strntoll)(cs, str, length, base, endptr, error);
}

static inline ulonglong
my_ci_strntoull(CHARSET_INFO *cs, const char *str, size_t length,
                int base, char **endptr, int *error)
{
  return (cs->cset->strntoull)(cs, str, length, base, endptr, error);
}

static inline double
my_ci_strntod(CHARSET_INFO *cs, char *str, size_t length,
              char **endptr, int *error)
{
  return (cs->cset->strntod)(cs, str, length, endptr, error);
}

static inline longlong
my_ci_strtoll10(CHARSET_INFO *cs, const char *str, char **endptr, int *error)
{
  return (cs->cset->strtoll10)(cs, str, endptr, error);
}

static inline ulonglong
my_ci_strntoull10rnd(CHARSET_INFO *cs,
                     const char *str, size_t length, int unsigned_fl,
                     char **endptr, int *error)
{
  return (cs->cset->strntoull10rnd)(cs, str, length, unsigned_fl, endptr, error);
}


static inline size_t
my_ci_scan(CHARSET_INFO *cs, const char *b, const char *e, int seq)
{
  return (cs->cset->scan)(cs, b, e, seq);
}


/**
  Return length of the leftmost character in a string.
  @param cs  - character set
  @param str - the beginning of the string
  @param end - the string end (the next byte after the string)
  @return  <=0 on errors (EOL, wrong byte sequence)
  @return    1 on a single byte character
  @return   >1 on a multi-byte character

  Note, inlike my_ismbchar(), 1 is returned for a single byte character.
*/

static inline int
my_ci_charlen(CHARSET_INFO *cs, const uchar *str, const uchar *end)
{
  return (cs->cset->charlen)(cs, str, end);
}


static inline size_t
my_ci_well_formed_char_length(CHARSET_INFO *cs,
                              const char *str, const char *end,
                              size_t nchars,
                              MY_STRCOPY_STATUS *status)
{
  return (cs->cset->well_formed_char_length)(cs, str, end, nchars, status);
}


static inline size_t
my_ci_copy_fix(CHARSET_INFO *cs,
               char *dst, size_t dst_length,
               const char *src, size_t src_length,
               size_t nchars, MY_STRCOPY_STATUS *status)
{
  return (cs->cset->copy_fix)(cs, dst, dst_length, src, src_length, nchars,
                              status);
}


/* Collation routines */

static inline my_bool
my_ci_init_collation(struct charset_info_st *ci, MY_CHARSET_LOADER *loader)
{
  if (!ci->coll->init)
    return FALSE;
  return (ci->coll->init)(ci, loader);
}


static inline int
my_ci_strnncoll(CHARSET_INFO *ci,
                const uchar *a, size_t alen,
                const uchar *b, size_t blen,
                my_bool b_is_prefix)
{
  return (ci->coll->strnncoll)(ci, a, alen, b, blen, b_is_prefix);
}

static inline int
my_ci_strnncollsp(CHARSET_INFO *ci,
                  const uchar *a, size_t alen,
                  const uchar *b, size_t blen)
{
  return (ci->coll->strnncollsp)(ci, a, alen, b, blen);
}


static inline my_bool
my_ci_like_range(CHARSET_INFO *ci,
                 const char *s, size_t s_length,
                 pchar w_prefix, pchar w_one, pchar w_many,
                 size_t res_length,
                 char *min_str, char *max_str,
                 size_t *min_len, size_t *max_len)
{
  return (ci->coll->like_range)(ci, s, s_length,
                                w_prefix, w_one, w_many,
                                res_length, min_str, max_str,
                                min_len, max_len);
}


static inline uint
my_ci_instr(CHARSET_INFO *ci,
            const char *b, size_t b_length,
            const char *s, size_t s_length,
            my_match_t *match, uint nmatch)
{
  return (ci->coll->instr)(ci, b, b_length, s, s_length, match, nmatch);
}


static inline void
my_ci_hash_sort(CHARSET_INFO *ci,
                const uchar *key, size_t len,
                ulong *nr1, ulong *nr2)
{
  (ci->coll->hash_sort)(ci, key, len, nr1, nr2);
}


#define ILLEGAL_CHARSET_INFO_NUMBER (~0U)

extern MYSQL_PLUGIN_IMPORT struct charset_info_st my_charset_bin;
extern MYSQL_PLUGIN_IMPORT struct charset_info_st my_charset_latin1;
extern MYSQL_PLUGIN_IMPORT struct charset_info_st my_charset_latin1_nopad;
extern MYSQL_PLUGIN_IMPORT struct charset_info_st my_charset_filename;
extern MYSQL_PLUGIN_IMPORT struct charset_info_st my_charset_utf8mb3_general_ci;

extern struct charset_info_st my_charset_big5_bin;
extern struct charset_info_st my_charset_big5_chinese_ci;
extern struct charset_info_st my_charset_big5_nopad_bin;
extern struct charset_info_st my_charset_big5_chinese_nopad_ci;
extern struct charset_info_st my_charset_cp1250_czech_cs;
extern struct charset_info_st my_charset_cp932_bin;
extern struct charset_info_st my_charset_cp932_japanese_ci;
extern struct charset_info_st my_charset_cp932_nopad_bin;
extern struct charset_info_st my_charset_cp932_japanese_nopad_ci;
extern struct charset_info_st my_charset_eucjpms_bin;
extern struct charset_info_st my_charset_eucjpms_japanese_ci;
extern struct charset_info_st my_charset_eucjpms_nopad_bin;
extern struct charset_info_st my_charset_eucjpms_japanese_nopad_ci;
extern struct charset_info_st my_charset_euckr_bin;
extern struct charset_info_st my_charset_euckr_korean_ci;
extern struct charset_info_st my_charset_euckr_nopad_bin;
extern struct charset_info_st my_charset_euckr_korean_nopad_ci;
extern struct charset_info_st my_charset_gb2312_bin;
extern struct charset_info_st my_charset_gb2312_chinese_ci;
extern struct charset_info_st my_charset_gb2312_nopad_bin;
extern struct charset_info_st my_charset_gb2312_chinese_nopad_ci;
extern struct charset_info_st my_charset_gbk_bin;
extern struct charset_info_st my_charset_gbk_chinese_ci;
extern struct charset_info_st my_charset_gbk_nopad_bin;
extern struct charset_info_st my_charset_gbk_chinese_nopad_ci;
extern struct charset_info_st my_charset_latin1_bin;
extern struct charset_info_st my_charset_latin1_nopad_bin;
extern struct charset_info_st my_charset_latin1_german2_ci;
extern struct charset_info_st my_charset_latin2_czech_cs;
extern struct charset_info_st my_charset_sjis_bin;
extern struct charset_info_st my_charset_sjis_japanese_ci;
extern struct charset_info_st my_charset_sjis_nopad_bin;
extern struct charset_info_st my_charset_sjis_japanese_nopad_ci;
extern struct charset_info_st my_charset_tis620_bin;
extern struct charset_info_st my_charset_tis620_thai_ci;
extern struct charset_info_st my_charset_tis620_nopad_bin;
extern struct charset_info_st my_charset_tis620_thai_nopad_ci;
extern struct charset_info_st my_charset_ucs2_bin;
extern struct charset_info_st my_charset_ucs2_general_ci;
extern struct charset_info_st my_charset_ucs2_nopad_bin;
extern struct charset_info_st my_charset_ucs2_general_nopad_ci;
extern struct charset_info_st my_charset_ucs2_general_mysql500_ci;
extern struct charset_info_st my_charset_ucs2_unicode_ci;
extern struct charset_info_st my_charset_ucs2_unicode_nopad_ci;
extern struct charset_info_st my_charset_ucs2_general_mysql500_ci;
extern struct charset_info_st my_charset_ujis_bin;
extern struct charset_info_st my_charset_ujis_japanese_ci;
extern struct charset_info_st my_charset_ujis_nopad_bin;
extern struct charset_info_st my_charset_ujis_japanese_nopad_ci;
extern struct charset_info_st my_charset_utf16_bin;
extern struct charset_info_st my_charset_utf16_general_ci;
extern struct charset_info_st my_charset_utf16_unicode_ci;
extern struct charset_info_st my_charset_utf16_unicode_nopad_ci;
extern struct charset_info_st my_charset_utf16le_bin;
extern struct charset_info_st my_charset_utf16le_general_ci;
extern struct charset_info_st my_charset_utf16_general_nopad_ci;
extern struct charset_info_st my_charset_utf16_nopad_bin;
extern struct charset_info_st my_charset_utf16le_nopad_bin;
extern struct charset_info_st my_charset_utf16le_general_nopad_ci;
extern struct charset_info_st my_charset_utf32_bin;
extern struct charset_info_st my_charset_utf32_general_ci;
extern struct charset_info_st my_charset_utf32_unicode_ci;
extern struct charset_info_st my_charset_utf32_unicode_nopad_ci;
extern struct charset_info_st my_charset_utf32_nopad_bin;
extern struct charset_info_st my_charset_utf32_general_nopad_ci;
extern MYSQL_PLUGIN_IMPORT struct charset_info_st my_charset_utf8mb3_bin;
extern struct charset_info_st my_charset_utf8mb3_nopad_bin;
extern struct charset_info_st my_charset_utf8mb3_general_nopad_ci;
extern struct charset_info_st my_charset_utf8mb3_general_mysql500_ci;
extern struct charset_info_st my_charset_utf8mb3_unicode_ci;
extern struct charset_info_st my_charset_utf8mb3_unicode_nopad_ci;
extern MYSQL_PLUGIN_IMPORT struct charset_info_st my_charset_utf8mb4_bin;
extern struct charset_info_st my_charset_utf8mb4_general_ci;
extern struct charset_info_st my_charset_utf8mb4_nopad_bin;
extern struct charset_info_st my_charset_utf8mb4_general_nopad_ci;
extern struct charset_info_st my_charset_utf8mb4_unicode_ci;
extern struct charset_info_st my_charset_utf8mb4_unicode_nopad_ci;

/*
  Contextually typed collations, e.g.:
    CHAR(10) COLLATE DEFAULT
    CHAR(10) BINARY
*/
extern struct charset_info_st my_collation_contextually_typed_default;
extern struct charset_info_st my_collation_contextually_typed_binary;


#define MY_UTF8MB3                 "utf8mb3"
#define MY_UTF8MB4                 "utf8mb4"

my_bool my_cs_have_contractions(CHARSET_INFO *cs);
my_bool my_cs_can_be_contraction_head(CHARSET_INFO *cs, my_wc_t wc);
my_bool my_cs_can_be_contraction_tail(CHARSET_INFO *cs, my_wc_t wc);
const uint16 *my_cs_contraction2_weight(CHARSET_INFO *cs, my_wc_t wc1,
                                         my_wc_t wc2);

/* declarations for simple charsets */
extern size_t my_strnxfrm_simple(CHARSET_INFO *,
                                 uchar *dst, size_t dstlen, uint nweights,
                                 const uchar *src, size_t srclen, uint flags); 
size_t  my_strnxfrmlen_simple(CHARSET_INFO *, size_t); 
extern int  my_strnncoll_simple(CHARSET_INFO *, const uchar *, size_t,
				const uchar *, size_t, my_bool);

extern int  my_strnncollsp_simple(CHARSET_INFO *, const uchar *, size_t,
                                  const uchar *, size_t);

extern void my_hash_sort_simple(CHARSET_INFO *cs,
				const uchar *key, size_t len,
				ulong *nr1, ulong *nr2); 

extern void my_hash_sort_simple_nopad(CHARSET_INFO *cs,
				      const uchar *key, size_t len,
				      ulong *nr1, ulong *nr2);

extern void my_hash_sort_bin(CHARSET_INFO *cs,
                             const uchar *key, size_t len, ulong *nr1,
                             ulong *nr2);

/**
  Compare a string to an array of spaces, for PAD SPACE comparison.
  The function iterates through the string and compares every byte to 0x20.
  @param       - the string
  @param       - its length
  @return <0   - if a byte less than 0x20 was found in the string.
  @return  0   - if all bytes in the string were 0x20, or if length was 0.
  @return >0   - if a byte greater than 0x20 was found in the string.
*/
extern int my_strnncollsp_padspace_bin(const uchar *str, size_t length);

extern size_t my_lengthsp_8bit(CHARSET_INFO *cs, const char *ptr, size_t length);

extern uint my_instr_simple(CHARSET_INFO *,
                            const char *b, size_t b_length,
                            const char *s, size_t s_length,
                            my_match_t *match, uint nmatch);

size_t my_copy_8bit(CHARSET_INFO *,
                    char *dst, size_t dst_length,
                    const char *src, size_t src_length,
                    size_t nchars, MY_STRCOPY_STATUS *);
size_t my_copy_fix_mb(CHARSET_INFO *cs,
                      char *dst, size_t dst_length,
                      const char *src, size_t src_length,
                      size_t nchars, MY_STRCOPY_STATUS *);

/* Functions for 8bit */
extern size_t my_caseup_str_8bit(CHARSET_INFO *, char *);
extern size_t my_casedn_str_8bit(CHARSET_INFO *, char *);
extern size_t my_caseup_8bit(CHARSET_INFO *,
                             const char *src, size_t srclen,
                             char *dst, size_t dstlen);
extern size_t my_casedn_8bit(CHARSET_INFO *,
                             const char *src, size_t srclen,
                             char *dst, size_t dstlen);

extern int my_strcasecmp_8bit(CHARSET_INFO * cs, const char *, const char *);

int my_mb_wc_8bit(CHARSET_INFO *cs,my_wc_t *wc, const uchar *s,const uchar *e);
int my_wc_mb_8bit(CHARSET_INFO *cs,my_wc_t wc, uchar *s, uchar *e);
int my_wc_mb_bin(CHARSET_INFO *cs,my_wc_t wc, uchar *s, uchar *e);

int my_mb_ctype_8bit(CHARSET_INFO *,int *, const uchar *,const uchar *);
int my_mb_ctype_mb(CHARSET_INFO *,int *, const uchar *,const uchar *);

size_t my_scan_8bit(CHARSET_INFO *cs, const char *b, const char *e, int sq);

size_t my_snprintf_8bit(CHARSET_INFO *, char *to, size_t n,
                        const char *fmt, ...)
  ATTRIBUTE_FORMAT(printf, 4, 5);

long       my_strntol_8bit(CHARSET_INFO *, const char *s, size_t l, int base,
                           char **e, int *err);
ulong      my_strntoul_8bit(CHARSET_INFO *, const char *s, size_t l, int base,
			    char **e, int *err);
longlong   my_strntoll_8bit(CHARSET_INFO *, const char *s, size_t l, int base,
			    char **e, int *err);
ulonglong my_strntoull_8bit(CHARSET_INFO *, const char *s, size_t l, int base,
			    char **e, int *err);
double      my_strntod_8bit(CHARSET_INFO *, char *s, size_t l,char **e,
			    int *err);
size_t my_long10_to_str_8bit(CHARSET_INFO *, char *to, size_t l, int radix,
                             long int val);
size_t my_longlong10_to_str_8bit(CHARSET_INFO *, char *to, size_t l, int radix,
                                 longlong val);

longlong my_strtoll10_8bit(CHARSET_INFO *cs,
                           const char *nptr, char **endptr, int *error);
longlong my_strtoll10_ucs2(CHARSET_INFO *cs, 
                           const char *nptr, char **endptr, int *error);

ulonglong my_strntoull10rnd_8bit(CHARSET_INFO *cs,
                                 const char *str, size_t length, int
                                 unsigned_fl, char **endptr, int *error);
ulonglong my_strntoull10rnd_ucs2(CHARSET_INFO *cs, 
                                 const char *str, size_t length,
                                 int unsigned_fl, char **endptr, int *error);

void my_fill_8bit(CHARSET_INFO *cs, char* to, size_t l, int fill);

/* For 8-bit character set */
my_bool  my_like_range_simple(CHARSET_INFO *cs,
			      const char *ptr, size_t ptr_length,
			      pbool escape, pbool w_one, pbool w_many,
			      size_t res_length,
			      char *min_str, char *max_str,
			      size_t *min_length, size_t *max_length);

/* For ASCII-based multi-byte character sets with mbminlen=1 */
my_bool  my_like_range_mb(CHARSET_INFO *cs,
			  const char *ptr, size_t ptr_length,
			  pbool escape, pbool w_one, pbool w_many,
			  size_t res_length,
			  char *min_str, char *max_str,
			  size_t *min_length, size_t *max_length);

/* For other character sets, with arbitrary mbminlen and mbmaxlen numbers */
my_bool  my_like_range_generic(CHARSET_INFO *cs,
                               const char *ptr, size_t ptr_length,
                               pbool escape, pbool w_one, pbool w_many,
                               size_t res_length,
                               char *min_str, char *max_str,
                               size_t *min_length, size_t *max_length);

int my_wildcmp_8bit(CHARSET_INFO *,
		    const char *str,const char *str_end,
		    const char *wildstr,const char *wildend,
		    int escape, int w_one, int w_many);

int my_wildcmp_bin(CHARSET_INFO *,
		   const char *str,const char *str_end,
		   const char *wildstr,const char *wildend,
		   int escape, int w_one, int w_many);

size_t my_numchars_8bit(CHARSET_INFO *, const char *b, const char *e);
size_t my_numcells_8bit(CHARSET_INFO *, const char *b, const char *e);
size_t my_charpos_8bit(CHARSET_INFO *, const char *b, const char *e, size_t pos);
size_t my_well_formed_char_length_8bit(CHARSET_INFO *cs,
                                       const char *b, const char *e,
                                       size_t nchars,
                                       MY_STRCOPY_STATUS *status);
int my_charlen_8bit(CHARSET_INFO *, const uchar *str, const uchar *end);


/* Functions for multibyte charsets */
extern size_t my_caseup_str_mb(CHARSET_INFO *, char *);
extern size_t my_casedn_str_mb(CHARSET_INFO *, char *);
extern size_t my_caseup_mb(CHARSET_INFO *,
                           const char *src, size_t srclen,
                           char *dst, size_t dstlen);
extern size_t my_casedn_mb(CHARSET_INFO *,
                           const char *src, size_t srclen,
                           char *dst, size_t dstlen);
extern size_t my_caseup_ujis(CHARSET_INFO *,
                             const char *src, size_t srclen,
                             char *dst, size_t dstlen);
extern size_t my_casedn_ujis(CHARSET_INFO *,
                             const char *src, size_t srclen,
                             char *dst, size_t dstlen);
extern int my_strcasecmp_mb(CHARSET_INFO * cs,const char *, const char *);

int my_wildcmp_mb(CHARSET_INFO *,
		  const char *str,const char *str_end,
		  const char *wildstr,const char *wildend,
		  int escape, int w_one, int w_many);
size_t my_numchars_mb(CHARSET_INFO *, const char *b, const char *e);
size_t my_numcells_mb(CHARSET_INFO *, const char *b, const char *e);
size_t my_charpos_mb(CHARSET_INFO *, const char *b, const char *e, size_t pos);
uint my_instr_mb(CHARSET_INFO *,
                 const char *b, size_t b_length,
                 const char *s, size_t s_length,
                 my_match_t *match, uint nmatch);

int my_wildcmp_mb_bin(CHARSET_INFO *cs,
                      const char *str,const char *str_end,
                      const char *wildstr,const char *wildend,
                      int escape, int w_one, int w_many);

int my_strcasecmp_mb_bin(CHARSET_INFO * cs __attribute__((unused)),
                         const char *s, const char *t);

void my_hash_sort_mb_bin(CHARSET_INFO *cs __attribute__((unused)),
                         const uchar *key, size_t len,ulong *nr1, ulong *nr2);

void my_hash_sort_mb_nopad_bin(CHARSET_INFO *cs __attribute__((unused)),
                               const uchar *key, size_t len,
                               ulong *nr1, ulong *nr2);

size_t my_strnxfrm_mb(CHARSET_INFO *,
                      uchar *dst, size_t dstlen, uint nweights,
                      const uchar *src, size_t srclen, uint flags);

size_t my_strnxfrm_mb_nopad(CHARSET_INFO *,
			    uchar *dst, size_t dstlen, uint nweights,
			    const uchar *src, size_t srclen, uint flags);

size_t  my_strnxfrmlen_unicode(CHARSET_INFO *, size_t); 

size_t my_strnxfrm_unicode_full_bin(CHARSET_INFO *,
                                    uchar *dst, size_t dstlen,
                                    uint nweights, const uchar *src,
                                    size_t srclen, uint flags);

size_t my_strnxfrm_unicode_full_nopad_bin(CHARSET_INFO *,
					  uchar *dst, size_t dstlen,
					  uint nweights, const uchar *src,
					  size_t srclen, uint flags);

size_t  my_strnxfrmlen_unicode_full_bin(CHARSET_INFO *, size_t); 

int my_wildcmp_unicode(CHARSET_INFO *cs,
                       const char *str, const char *str_end,
                       const char *wildstr, const char *wildend,
                       int escape, int w_one, int w_many,
                       MY_UNICASE_INFO *weights);

extern my_bool my_parse_charset_xml(MY_CHARSET_LOADER *loader,
                                    const char *buf, size_t buflen);
extern char *my_strchr(CHARSET_INFO *cs, const char *str, const char *end,
                       pchar c);
extern size_t my_strcspn(CHARSET_INFO *cs, const char *str, const char *end,
                         const char *accept);

my_bool my_propagate_simple(CHARSET_INFO *cs, const uchar *str, size_t len);
my_bool my_propagate_complex(CHARSET_INFO *cs, const uchar *str, size_t len);


typedef struct 
{
  size_t char_length;
  my_repertoire_t repertoire;
} MY_STRING_METADATA;

void my_string_metadata_get(MY_STRING_METADATA *metadata,
                            CHARSET_INFO *cs, const char *str, size_t len);
my_repertoire_t my_string_repertoire(CHARSET_INFO *cs,
                                     const char *str, size_t len);
my_bool my_charset_is_ascii_based(CHARSET_INFO *cs);
my_repertoire_t my_charset_repertoire(CHARSET_INFO *cs);

uint my_strxfrm_flag_normalize(uint flags, uint nlevels);
void my_strxfrm_desc_and_reverse(uchar *str, uchar *strend,
                                 uint flags, uint level);
size_t my_strxfrm_pad_desc_and_reverse(CHARSET_INFO *cs,
                                       uchar *str, uchar *frmend, uchar *strend,
                                       uint nweights, uint flags, uint level);
size_t my_strxfrm_pad_desc_and_reverse_nopad(CHARSET_INFO *cs,
					     uchar *str, uchar *frmend,
					     uchar *strend, uint nweights,
					     uint flags, uint level);

const MY_CONTRACTIONS *my_charset_get_contractions(CHARSET_INFO *cs,
                                                   int level);

extern size_t my_vsnprintf_ex(CHARSET_INFO *cs, char *to, size_t n,
                              const char* fmt, va_list ap);

/*
  Convert a string between two character sets.
  Bad byte sequences as well as characters that cannot be
  encoded in the destination character set are replaced to '?'.
*/
uint32 my_convert(char *to, uint32 to_length, CHARSET_INFO *to_cs,
                  const char *from, uint32 from_length,
                  CHARSET_INFO *from_cs, uint *errors);

/**
  An extended version of my_convert(), to pass non-default mb_wc() and wc_mb().
  For example, String::copy_printable() which is used in
  Protocol::store_warning() uses this to escape control
  and non-convertable characters.
*/
uint32 my_convert_using_func(char *to, size_t to_length, CHARSET_INFO *to_cs,
                             my_charset_conv_wc_mb mb_wc,
                             const char *from, size_t from_length,
                             CHARSET_INFO *from_cs,
                             my_charset_conv_mb_wc wc_mb,
                             uint *errors);
/*
  Convert a string between two character sets.
  Bad byte sequences as well as characters that cannot be
  encoded in the destination character set are replaced to '?'.
  Not more than "nchars" characters are copied.
  Conversion statistics is returned in "status" and is set as follows:
  - status->m_native_copy_status.m_source_end_pos - to the position
    between (src) and (src+src_length), where the function stopped reading
    the source string.
  - status->m_native_copy_status.m_well_formed_error_pos - to the position
    between (src) and (src+src_length), where the first badly formed byte
    sequence was found, or to NULL if the string was well formed in the
    given range.
  - status->m_cannot_convert_error_pos - to the position 
    between (src) and (src+src_length), where the first character that
    cannot be represented in the destination character set was found,
    or to NULL if all characters in the given range were successfully
    converted.

  "src" is allowed to be a NULL pointer. In this case "src_length" must
  be equal to 0. All "status" members are initialized to NULL, and 0 is
  returned.
*/
size_t my_convert_fix(CHARSET_INFO *dstcs, char *dst, size_t dst_length,
                      CHARSET_INFO *srccs, const char *src, size_t src_length,
                      size_t nchars,
                      MY_STRCOPY_STATUS *copy_status,
                      MY_STRCONV_STATUS *conv_status);

#define	_MY_U	01	/* Upper case */
#define	_MY_L	02	/* Lower case */
#define	_MY_NMR	04	/* Numeral (digit) */
#define	_MY_SPC	010	/* Spacing character */
#define	_MY_PNT	020	/* Punctuation */
#define	_MY_CTR	040	/* Control character */
#define	_MY_B	0100	/* Blank */
#define	_MY_X	0200	/* heXadecimal digit */


#define	my_isascii(c)	(!((c) & ~0177))
#define	my_toascii(c)	((c) & 0177)
#define my_tocntrl(c)	((c) & 31)
#define my_toprint(c)	((c) | 64)
#define my_toupper(s,c)	(char) ((s)->to_upper[(uchar) (c)])
#define my_tolower(s,c)	(char) ((s)->to_lower[(uchar) (c)])
#define	my_isalpha(s, c)  (((s)->m_ctype+1)[(uchar) (c)] & (_MY_U | _MY_L))
#define	my_isupper(s, c)  (((s)->m_ctype+1)[(uchar) (c)] & _MY_U)
#define	my_islower(s, c)  (((s)->m_ctype+1)[(uchar) (c)] & _MY_L)
#define	my_isdigit(s, c)  (((s)->m_ctype+1)[(uchar) (c)] & _MY_NMR)
#define	my_isxdigit(s, c) (((s)->m_ctype+1)[(uchar) (c)] & _MY_X)
#define	my_isalnum(s, c)  (((s)->m_ctype+1)[(uchar) (c)] & (_MY_U | _MY_L | _MY_NMR))
#define	my_isspace(s, c)  (((s)->m_ctype+1)[(uchar) (c)] & _MY_SPC)
#define	my_ispunct(s, c)  (((s)->m_ctype+1)[(uchar) (c)] & _MY_PNT)
#define	my_isprint(s, c)  (((s)->m_ctype+1)[(uchar) (c)] & (_MY_PNT | _MY_U | _MY_L | _MY_NMR | _MY_B))
#define	my_isgraph(s, c)  (((s)->m_ctype+1)[(uchar) (c)] & (_MY_PNT | _MY_U | _MY_L | _MY_NMR))
#define	my_iscntrl(s, c)  (((s)->m_ctype+1)[(uchar) (c)] & _MY_CTR)

/* Some macros that should be cleaned up a little */
#define my_isvar(s,c)                 (my_isalnum(s,c) || (c) == '_')
#define my_isvar_start(s,c)           (my_isalpha(s,c) || (c) == '_')

#define my_binary_compare(s)	      ((s)->state  & MY_CS_BINSORT)
#define use_strnxfrm(s)               ((s)->state  & MY_CS_STRNXFRM)
#define my_strnncoll(s, a, b, c, d) ((s)->coll->strnncoll((s), (a), (b), (c), (d), 0))
#define my_strcasecmp(s, a, b)        ((s)->coll->strcasecmp((s), (a), (b)))

/**
  Detect if the leftmost character in a string is a valid multi-byte character
  and return its length, or return 0 otherwise.
  @param cs  - character set
  @param str - the beginning of the string
  @param end - the string end (the next byte after the string)
  @return    >0, for a multi-byte character
  @rerurn    0,  for a single byte character, broken sequence, empty string.
*/
static inline
uint my_ismbchar(CHARSET_INFO *cs, const char *str, const char *end)
{
  int char_length= (cs->cset->charlen)(cs, (const uchar *) str,
                                           (const uchar *) end);
  return char_length > 1 ? (uint) char_length : 0U;
}


/**
  Convert broken and incomplete byte sequences to 1 byte.
*/
static inline
uint my_ci_charlen_fix(CHARSET_INFO *cs, const uchar *str, const uchar *end)
{
  int char_length= my_ci_charlen(cs, str, end);
  DBUG_ASSERT(str < end);
  return char_length > 0 ? (uint) char_length : (uint) 1U;
}


/*
  A compatibility replacement pure C function for the former
    cs->cset->well_formed_len().
  In C++ code please use Well_formed_prefix::length() instead.
*/
static inline size_t
my_well_formed_length(CHARSET_INFO *cs, const char *b, const char *e,
                      size_t nchars, int *error)
{
  MY_STRCOPY_STATUS status;
  (void) my_ci_well_formed_char_length(cs, b, e, nchars, &status);
  *error= status.m_well_formed_error_pos == NULL ? 0 : 1;
  return (size_t) (status.m_source_end_pos - b);
}


#define my_caseup_str(s, a)           ((s)->cset->caseup_str((s), (a)))
#define my_casedn_str(s, a)           ((s)->cset->casedn_str((s), (a)))

/* XXX: still need to take care of this one */
#ifdef MY_CHARSET_TIS620
#error The TIS620 charset is broken at the moment.  Tell tim to fix it.
#define USE_TIS620
#include "t_ctype.h"
#endif

#ifdef	__cplusplus
}
#endif

#endif /* _m_ctype_h */
