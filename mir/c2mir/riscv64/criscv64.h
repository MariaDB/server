/* This file is a part of MIR project.
   Copyright (C) 2020-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.
*/

#include <stdint.h>

#define MIR_CHAR_BIT 8

typedef int8_t mir_schar;
typedef int16_t mir_short;
typedef int32_t mir_int;
typedef int64_t mir_long;
typedef int64_t mir_llong;

#define MIR_SCHAR_MIN INT8_MIN
#define MIR_SCHAR_MAX INT8_MAX
#define MIR_SHORT_MIN INT16_MIN
#define MIR_SHORT_MAX INT16_MAX
#define MIR_INT_MIN INT32_MIN
#define MIR_INT_MAX INT32_MAX
#define MIR_LONG_MIN INT64_MIN
#define MIR_LONG_MAX INT64_MAX
#define MIR_LLONG_MIN INT64_MIN
#define MIR_LLONG_MAX INT64_MAX

typedef uint8_t mir_uchar;
typedef uint16_t mir_ushort;
typedef uint32_t mir_uint;
typedef uint64_t mir_ulong;
typedef uint64_t mir_ullong;
typedef uint32_t mir_wchar;
typedef uint16_t mir_char16;
typedef uint32_t mir_char32;

#define MIR_UCHAR_MAX UINT8_MAX
#define MIR_USHORT_MAX UINT16_MAX
#define MIR_UINT_MAX UINT32_MAX
#define MIR_ULONG_MAX UINT64_MAX
#define MIR_ULLONG_MAX UINT64_MAX
#define MIR_WCHAR_MIN 0
#define MIR_WCHAR_MAX UINT32_MAX

typedef mir_schar mir_char;
#define MIR_CHAR_MIN MIR_SCHAR_MIN
#define MIR_CHAR_MAX MIR_SCHAR_MAX

typedef float mir_float;
typedef double mir_double;
typedef long double mir_ldouble;

typedef uint8_t mir_bool;
typedef int64_t mir_ptrdiff_t;
typedef uint64_t mir_size_t;

#define MIR_SIZE_MAX UINT64_MAX
