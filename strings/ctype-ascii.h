#ifndef CTYPE_ASCII_INCLUDED
#define CTYPE_ASCII_INCLUDED

#include "myisampack.h"

/*
  Magic expression. It uses the fact that for any byte value X in
  the range 0..31 (0x00..0x1F) the expression (X+31)*5 returns
  the 7th bit (0x80) set only for the following six (out of 32) values:
    0x00, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F.
  These values correspond to offsets of non-letter characters
  in the ASCII table:

  The following macro sets the bit 0x20 for the following characters:
  ----------------  --------------------------------
  Magic bit         10000000000000000000000000011111
  ASCII 0x00..0x1F  ................................ Control
  ASCII 0x20..0x3F  ................................ Punctuation, digits
  ASCII 0x40..0x5F  @ABCDEFGHIJKLMNOPQRSTUVWXYZ[\]^_
  ASCII 0x60..0x7F  `abcdefghijklmnopqrstuvwxyz{|}~.
  ----------------  --------------------------------
  We shift the magic bit 0x80 right twice to make it 0x20.
  So on the ranges [40..5F] and [60..7F] the expression
  has the bit 0x20 set for all non-letter characters.
  Note, other bits contain garbage.

  Requirements:
    All bytes must be in the range [00..7F],
    to avoid overflow and carry to the next byte.
*/
#define MY_ASCII_20_IS_SET_IF_NOT_LETTER_MAGIC(i) \
  (((((i)+0x1F1F1F1F1F1F1F1FULL) & 0x1F1F1F1F1F1F1F1F) * 5) >> 2)


/*
  The following macro returns the bit 0x20 set to:
  - 1 for input bytes in the ranges [60..7F] or [E0..FF]
  - 0 otherwise
  Bytes in the ranges [40..7F] and [C0..FF] have the bit 0x40 set.
  Bytes in the ranges [60..7F] and [E0..FF] have the bit 0x20 set.
    Hex      BinHi BinLo
    ----     -1--  ----
    0x[4C]X  .10.  ....
    0x[5D]X  .10.  ....
    0x[6E]X  .11.  ....
    0x[7F]X  .11.  ....
*/
#define MY_ASCII_20_IS_SET_IF_RANGE_60_7F_OR_E0_FF(i) (((i) >> 1) & ((i)))


/*
  The following macro evaluates to exactly 0x20 for all
  lower case ASCII letters [a-z], and to 0x00 otherwise:

  Value     Range       Character range                   Subrange
  --------  --------    --------------------------------  -------
  00000000  0x00..0x3F  Control, punctuation, digits
  00100000  0x40..0x5F  @ABCDEFGHIJKLMNOPQRSTUVWXYZ[\]^_  letters A-Z
  00000000  0x40..0x5F  @ABCDEFGHIJKLMNOPQRSTUVWXYZ[\]^_  non-letters
  00100000  0x60..0x7F  `abcdefghijklmnopqrstuvwxyz{|}~.  letters a-z
  00000000  0x60..0x7F  `abcdefghijklmnopqrstuvwxyz{|}~.  non-letters

  Requirements:
    All bytes must be in the range [00..7F].
    See the comments in MY_ASCII_20_IS_SET_IF_NOT_LETTER_MAGIC().
*/

#define MY_ASCII_20_IF_IS_LOWER_LETTER(i) \
  (MY_ASCII_20_IS_SET_IF_RANGE_60_7F_OR_E0_FF(i) & \
   ~MY_ASCII_20_IS_SET_IF_NOT_LETTER_MAGIC(i) & \
   0x2020202020202020)

/*
  Convert lower case ASCII letters to upper case by unsetting
  the bit 0x20 with help of the magic expression.

  Requirements:
    All bytes must be in the range [00..7F].
    See the comments in MY_ASCII_20_IS_SET_IF_NOT_LETTER_MAGIC()
*/
#define MY_ASCII_TOUPPER_MAGIC(i) \
  (i ^ MY_ASCII_20_IF_IS_LOWER_LETTER(i))


/*
  Convert a string (consisting of 8 bytes stored in uint64)
  to upper case algorithmically.

  Requirements:
    All bytes must be in the range [00..0x7F].
    See the comments in MY_ASCII_20_IS_SET_IF_NOT_LETTER_MAGIC().
    The result on 8bit data is unpredictable!!!
    The caller should make sure not to pass 8bit data.
*/
static inline ulonglong my_ascii_to_upper_magic_uint64(ulonglong i)
{
  return MY_ASCII_TOUPPER_MAGIC(i);
}


/*
  Check if:
  - both strings "a" and "b" have at least 4 bytes, and
  - both strings have only 7bit data.
*/
static inline int
my_strcoll_ascii_4bytes_found(const uchar *a, const uchar *ae,
                              const uchar *b, const uchar *be)
{
  return a + 4 <= ae && b + 4 <= be        &&
         (uint4korr(b) & 0x80808080) == 0  &&
         (uint4korr(a) & 0x80808080) == 0;
}


/*
  Compare the leading four 7bit ASCII bytes in two strings case insensitively
  by converting letters [a-z] to upper case [A-Z].

  Requirements:
  - The input strings must have at least four bytes, and
  - The leading four bytes in both strings must be 7bit ASCII.
  The caller must make sure to provide only strings that meet
  these requirements. The result on 8-bit data is unpredictable
  as 8-bit bytes may cause overflow in my_ascii_to_upper_magic_uint64().
  See comments above.
*/
static inline int
my_strcoll_ascii_toupper_4bytes(const uchar *a, const uchar *b)
{
  ulonglong abn= (((ulonglong) mi_uint4korr(a)) << 32) | mi_uint4korr(b);
  abn= my_ascii_to_upper_magic_uint64(abn);
  if ((uint32) (abn >> 32) == (uint32) abn)
    return 0;
  return ((uint32) (abn >> 32)) < ((uint32) abn) ? -1 : + 1;
}


/*
  Compare the leading eight 7bit ASCII bytes in two strings case insensitively
  by converting letters [a-z] to upper case [A-Z].

  Requirements:
  - The input strings must have at least eight bytes, and
  - The leading eight bytes in both strings must be 7bit ASCII.
  See comments in my_strcoll_ascii_toupper_4bytes().
*/
static inline int
my_strcoll_ascii_toupper_8bytes(const uchar *a, const uchar *b)
{
  /*
    TODO:
    Try to get advantage of SIMD instructions by massive comparison
    (16 bytes at a time) of characters against (x>='a' && x<='z') using:
    - either explicit intrinsics
    - or a loop that can get vectorized automatically by some compilers.
  */
  ulonglong an= mi_uint8korr(a);
  ulonglong bn= mi_uint8korr(b);
  an= my_ascii_to_upper_magic_uint64(an);
  bn= my_ascii_to_upper_magic_uint64(bn);
  return an == bn ? 0 : an < bn ? -1 : +1;
}


/*
  Compare the leading four 7bit ASCII bytes in two strings in binary style.
*/
static inline int
my_strcoll_mb7_bin_4bytes(const uchar *a, const uchar *b)
{
  uint32 an= mi_uint4korr(a);
  uint32 bn= mi_uint4korr(b);
  return an == bn ? 0 : an < bn ? -1 : +1;
}


/*
  Compare the leading four 7bit ASCII bytes in two strings in binary style.
*/
static inline int
my_strcoll_mb7_bin_8bytes(const uchar *a, const uchar *b)
{
  ulonglong an= mi_uint8korr(a);
  ulonglong bn= mi_uint8korr(b);
  return an == bn ? 0 : an < bn ? -1 : +1;
}

#endif /* CTYPE_ASCII_INCLUDED */
