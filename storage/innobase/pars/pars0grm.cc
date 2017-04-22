/* A Bison parser, made by GNU Bison 3.0.4.  */

/* Bison implementation for Yacc-like parsers in C

   Copyright (C) 1984, 1989-1990, 2000-2015 Free Software Foundation, Inc.

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

/* As a special exception, you may create a larger work that contains
   part or all of the Bison parser skeleton and distribute that work
   under terms of your choice, so long as that work isn't itself a
   parser generator using the skeleton or a modified version thereof
   as a parser skeleton.  Alternatively, if you modify or redistribute
   the parser skeleton itself, you may (at your option) remove this
   special exception, which will cause the skeleton and the resulting
   Bison output files to be licensed under the GNU General Public
   License without this special exception.

   This special exception was added by the Free Software Foundation in
   version 2.2 of Bison.  */

/* C LALR(1) parser skeleton written by Richard Stallman, by
   simplifying the original so-called "semantic" parser.  */

/* All symbols defined below should begin with yy or YY, to avoid
   infringing on user name space.  This should be done even for local
   variables, as they might otherwise be expanded by user macros.
   There are some unavoidable exceptions within include files to
   define necessary library symbols; they are noted "INFRINGES ON
   USER NAME SPACE" below.  */

/* Identify Bison output.  */
#define YYBISON 1

/* Bison version.  */
#define YYBISON_VERSION "3.0.4"

/* Skeleton name.  */
#define YYSKELETON_NAME "yacc.c"

/* Pure parsers.  */
#define YYPURE 0

/* Push parsers.  */
#define YYPUSH 0

/* Pull parsers.  */
#define YYPULL 1




/* Copy the first part of user declarations.  */
#line 29 "pars0grm.y" /* yacc.c:339  */

/* The value of the semantic attribute is a pointer to a query tree node
que_node_t */

#include "univ.i"
#include <math.h>
#include "pars0pars.h"
#include "mem0mem.h"
#include "que0types.h"
#include "que0que.h"
#include "row0sel.h"

#define YYSTYPE que_node_t*

/* #define __STDC__ */
int
yylex(void);

#line 85 "pars0grm.cc" /* yacc.c:339  */

# ifndef YY_NULLPTR
#  if defined __cplusplus && 201103L <= __cplusplus
#   define YY_NULLPTR nullptr
#  else
#   define YY_NULLPTR 0
#  endif
# endif

/* Enabling verbose error messages.  */
#ifdef YYERROR_VERBOSE
# undef YYERROR_VERBOSE
# define YYERROR_VERBOSE 1
#else
# define YYERROR_VERBOSE 0
#endif

/* In a future release of Bison, this section will be replaced
   by #include "pars0grm.tab.h".  */
#ifndef YY_YY_PARS0GRM_TAB_H_INCLUDED
# define YY_YY_PARS0GRM_TAB_H_INCLUDED
/* Debug traces.  */
#ifndef YYDEBUG
# define YYDEBUG 0
#endif
#if YYDEBUG
extern int yydebug;
#endif

/* Token type.  */
#ifndef YYTOKENTYPE
# define YYTOKENTYPE
  enum yytokentype
  {
    PARS_INT_LIT = 258,
    PARS_FLOAT_LIT = 259,
    PARS_STR_LIT = 260,
    PARS_NULL_LIT = 261,
    PARS_ID_TOKEN = 262,
    PARS_AND_TOKEN = 263,
    PARS_OR_TOKEN = 264,
    PARS_NOT_TOKEN = 265,
    PARS_GE_TOKEN = 266,
    PARS_LE_TOKEN = 267,
    PARS_NE_TOKEN = 268,
    PARS_PROCEDURE_TOKEN = 269,
    PARS_IN_TOKEN = 270,
    PARS_OUT_TOKEN = 271,
    PARS_BINARY_TOKEN = 272,
    PARS_BLOB_TOKEN = 273,
    PARS_INT_TOKEN = 274,
    PARS_FLOAT_TOKEN = 275,
    PARS_CHAR_TOKEN = 276,
    PARS_IS_TOKEN = 277,
    PARS_BEGIN_TOKEN = 278,
    PARS_END_TOKEN = 279,
    PARS_IF_TOKEN = 280,
    PARS_THEN_TOKEN = 281,
    PARS_ELSE_TOKEN = 282,
    PARS_ELSIF_TOKEN = 283,
    PARS_LOOP_TOKEN = 284,
    PARS_WHILE_TOKEN = 285,
    PARS_RETURN_TOKEN = 286,
    PARS_SELECT_TOKEN = 287,
    PARS_SUM_TOKEN = 288,
    PARS_COUNT_TOKEN = 289,
    PARS_DISTINCT_TOKEN = 290,
    PARS_FROM_TOKEN = 291,
    PARS_WHERE_TOKEN = 292,
    PARS_FOR_TOKEN = 293,
    PARS_DDOT_TOKEN = 294,
    PARS_READ_TOKEN = 295,
    PARS_ORDER_TOKEN = 296,
    PARS_BY_TOKEN = 297,
    PARS_ASC_TOKEN = 298,
    PARS_DESC_TOKEN = 299,
    PARS_INSERT_TOKEN = 300,
    PARS_INTO_TOKEN = 301,
    PARS_VALUES_TOKEN = 302,
    PARS_UPDATE_TOKEN = 303,
    PARS_SET_TOKEN = 304,
    PARS_DELETE_TOKEN = 305,
    PARS_CURRENT_TOKEN = 306,
    PARS_OF_TOKEN = 307,
    PARS_CREATE_TOKEN = 308,
    PARS_TABLE_TOKEN = 309,
    PARS_INDEX_TOKEN = 310,
    PARS_UNIQUE_TOKEN = 311,
    PARS_CLUSTERED_TOKEN = 312,
    PARS_ON_TOKEN = 313,
    PARS_ASSIGN_TOKEN = 314,
    PARS_DECLARE_TOKEN = 315,
    PARS_CURSOR_TOKEN = 316,
    PARS_SQL_TOKEN = 317,
    PARS_OPEN_TOKEN = 318,
    PARS_FETCH_TOKEN = 319,
    PARS_CLOSE_TOKEN = 320,
    PARS_NOTFOUND_TOKEN = 321,
    PARS_TO_CHAR_TOKEN = 322,
    PARS_TO_NUMBER_TOKEN = 323,
    PARS_TO_BINARY_TOKEN = 324,
    PARS_BINARY_TO_NUMBER_TOKEN = 325,
    PARS_SUBSTR_TOKEN = 326,
    PARS_REPLSTR_TOKEN = 327,
    PARS_CONCAT_TOKEN = 328,
    PARS_INSTR_TOKEN = 329,
    PARS_LENGTH_TOKEN = 330,
    PARS_SYSDATE_TOKEN = 331,
    PARS_PRINTF_TOKEN = 332,
    PARS_ASSERT_TOKEN = 333,
    PARS_RND_TOKEN = 334,
    PARS_RND_STR_TOKEN = 335,
    PARS_ROW_PRINTF_TOKEN = 336,
    PARS_COMMIT_TOKEN = 337,
    PARS_ROLLBACK_TOKEN = 338,
    PARS_WORK_TOKEN = 339,
    PARS_UNSIGNED_TOKEN = 340,
    PARS_EXIT_TOKEN = 341,
    PARS_FUNCTION_TOKEN = 342,
    PARS_LOCK_TOKEN = 343,
    PARS_SHARE_TOKEN = 344,
    PARS_MODE_TOKEN = 345,
    PARS_LIKE_TOKEN = 346,
    PARS_LIKE_TOKEN_EXACT = 347,
    PARS_LIKE_TOKEN_PREFIX = 348,
    PARS_LIKE_TOKEN_SUFFIX = 349,
    PARS_LIKE_TOKEN_SUBSTR = 350,
    PARS_TABLE_NAME_TOKEN = 351,
    PARS_COMPACT_TOKEN = 352,
    PARS_BLOCK_SIZE_TOKEN = 353,
    PARS_BIGINT_TOKEN = 354,
    NEG = 355
  };
#endif

/* Value type.  */
#if ! defined YYSTYPE && ! defined YYSTYPE_IS_DECLARED
typedef int YYSTYPE;
# define YYSTYPE_IS_TRIVIAL 1
# define YYSTYPE_IS_DECLARED 1
#endif


extern YYSTYPE yylval;

int yyparse (void);

#endif /* !YY_YY_PARS0GRM_TAB_H_INCLUDED  */

/* Copy the second part of user declarations.  */

#line 237 "pars0grm.cc" /* yacc.c:358  */

#ifdef short
# undef short
#endif

#ifdef YYTYPE_UINT8
typedef YYTYPE_UINT8 yytype_uint8;
#else
typedef unsigned char yytype_uint8;
#endif

#ifdef YYTYPE_INT8
typedef YYTYPE_INT8 yytype_int8;
#else
typedef signed char yytype_int8;
#endif

#ifdef YYTYPE_UINT16
typedef YYTYPE_UINT16 yytype_uint16;
#else
typedef unsigned short int yytype_uint16;
#endif

#ifdef YYTYPE_INT16
typedef YYTYPE_INT16 yytype_int16;
#else
typedef short int yytype_int16;
#endif

#ifndef YYSIZE_T
# ifdef __SIZE_TYPE__
#  define YYSIZE_T __SIZE_TYPE__
# elif defined size_t
#  define YYSIZE_T size_t
# elif ! defined YYSIZE_T
#  include <stddef.h> /* INFRINGES ON USER NAME SPACE */
#  define YYSIZE_T size_t
# else
#  define YYSIZE_T unsigned int
# endif
#endif

#define YYSIZE_MAXIMUM ((YYSIZE_T) -1)

#ifndef YY_
# if defined YYENABLE_NLS && YYENABLE_NLS
#  if ENABLE_NLS
#   include <libintl.h> /* INFRINGES ON USER NAME SPACE */
#   define YY_(Msgid) dgettext ("bison-runtime", Msgid)
#  endif
# endif
# ifndef YY_
#  define YY_(Msgid) Msgid
# endif
#endif

#ifndef YY_ATTRIBUTE
# if (defined __GNUC__                                               \
      && (2 < __GNUC__ || (__GNUC__ == 2 && 96 <= __GNUC_MINOR__)))  \
     || defined __SUNPRO_C && 0x5110 <= __SUNPRO_C
#  define YY_ATTRIBUTE(Spec) __attribute__(Spec)
# else
#  define YY_ATTRIBUTE(Spec) /* empty */
# endif
#endif

#ifndef YY_ATTRIBUTE_PURE
# define YY_ATTRIBUTE_PURE   YY_ATTRIBUTE ((__pure__))
#endif

#ifndef YY_ATTRIBUTE_UNUSED
# define YY_ATTRIBUTE_UNUSED YY_ATTRIBUTE ((__unused__))
#endif

#if !defined _Noreturn \
     && (!defined __STDC_VERSION__ || __STDC_VERSION__ < 201112)
# if defined _MSC_VER && 1200 <= _MSC_VER
#  define _Noreturn __declspec (noreturn)
# else
#  define _Noreturn YY_ATTRIBUTE ((__noreturn__))
# endif
#endif

/* Suppress unused-variable warnings by "using" E.  */
#if ! defined lint || defined __GNUC__
# define YYUSE(E) ((void) (E))
#else
# define YYUSE(E) /* empty */
#endif

#if defined __GNUC__ && 407 <= __GNUC__ * 100 + __GNUC_MINOR__
/* Suppress an incorrect diagnostic about yylval being uninitialized.  */
# define YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN \
    _Pragma ("GCC diagnostic push") \
    _Pragma ("GCC diagnostic ignored \"-Wuninitialized\"")\
    _Pragma ("GCC diagnostic ignored \"-Wmaybe-uninitialized\"")
# define YY_IGNORE_MAYBE_UNINITIALIZED_END \
    _Pragma ("GCC diagnostic pop")
#else
# define YY_INITIAL_VALUE(Value) Value
#endif
#ifndef YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
# define YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
# define YY_IGNORE_MAYBE_UNINITIALIZED_END
#endif
#ifndef YY_INITIAL_VALUE
# define YY_INITIAL_VALUE(Value) /* Nothing. */
#endif


#if ! defined yyoverflow || YYERROR_VERBOSE

/* The parser invokes alloca or malloc; define the necessary symbols.  */

# ifdef YYSTACK_USE_ALLOCA
#  if YYSTACK_USE_ALLOCA
#   ifdef __GNUC__
#    define YYSTACK_ALLOC __builtin_alloca
#   elif defined __BUILTIN_VA_ARG_INCR
#    include <alloca.h> /* INFRINGES ON USER NAME SPACE */
#   elif defined _AIX
#    define YYSTACK_ALLOC __alloca
#   elif defined _MSC_VER
#    include <malloc.h> /* INFRINGES ON USER NAME SPACE */
#    define alloca _alloca
#   else
#    define YYSTACK_ALLOC alloca
#    if ! defined _ALLOCA_H && ! defined EXIT_SUCCESS
#     include <stdlib.h> /* INFRINGES ON USER NAME SPACE */
      /* Use EXIT_SUCCESS as a witness for stdlib.h.  */
#     ifndef EXIT_SUCCESS
#      define EXIT_SUCCESS 0
#     endif
#    endif
#   endif
#  endif
# endif

# ifdef YYSTACK_ALLOC
   /* Pacify GCC's 'empty if-body' warning.  */
#  define YYSTACK_FREE(Ptr) do { /* empty */; } while (0)
#  ifndef YYSTACK_ALLOC_MAXIMUM
    /* The OS might guarantee only one guard page at the bottom of the stack,
       and a page size can be as small as 4096 bytes.  So we cannot safely
       invoke alloca (N) if N exceeds 4096.  Use a slightly smaller number
       to allow for a few compiler-allocated temporary stack slots.  */
#   define YYSTACK_ALLOC_MAXIMUM 4032 /* reasonable circa 2006 */
#  endif
# else
#  define YYSTACK_ALLOC YYMALLOC
#  define YYSTACK_FREE YYFREE
#  ifndef YYSTACK_ALLOC_MAXIMUM
#   define YYSTACK_ALLOC_MAXIMUM YYSIZE_MAXIMUM
#  endif
#  if (defined __cplusplus && ! defined EXIT_SUCCESS \
       && ! ((defined YYMALLOC || defined malloc) \
             && (defined YYFREE || defined free)))
#   include <stdlib.h> /* INFRINGES ON USER NAME SPACE */
#   ifndef EXIT_SUCCESS
#    define EXIT_SUCCESS 0
#   endif
#  endif
#  ifndef YYMALLOC
#   define YYMALLOC malloc
#   if ! defined malloc && ! defined EXIT_SUCCESS
void *malloc (YYSIZE_T); /* INFRINGES ON USER NAME SPACE */
#   endif
#  endif
#  ifndef YYFREE
#   define YYFREE free
#   if ! defined free && ! defined EXIT_SUCCESS
void free (void *); /* INFRINGES ON USER NAME SPACE */
#   endif
#  endif
# endif
#endif /* ! defined yyoverflow || YYERROR_VERBOSE */


#if (! defined yyoverflow \
     && (! defined __cplusplus \
         || (defined YYSTYPE_IS_TRIVIAL && YYSTYPE_IS_TRIVIAL)))

/* A type that is properly aligned for any stack member.  */
union yyalloc
{
  yytype_int16 yyss_alloc;
  YYSTYPE yyvs_alloc;
};

/* The size of the maximum gap between one aligned stack and the next.  */
# define YYSTACK_GAP_MAXIMUM (sizeof (union yyalloc) - 1)

/* The size of an array large to enough to hold all stacks, each with
   N elements.  */
# define YYSTACK_BYTES(N) \
     ((N) * (sizeof (yytype_int16) + sizeof (YYSTYPE)) \
      + YYSTACK_GAP_MAXIMUM)

# define YYCOPY_NEEDED 1

/* Relocate STACK from its old location to the new one.  The
   local variables YYSIZE and YYSTACKSIZE give the old and new number of
   elements in the stack, and YYPTR gives the new location of the
   stack.  Advance YYPTR to a properly aligned location for the next
   stack.  */
# define YYSTACK_RELOCATE(Stack_alloc, Stack)                           \
    do                                                                  \
      {                                                                 \
        YYSIZE_T yynewbytes;                                            \
        YYCOPY (&yyptr->Stack_alloc, Stack, yysize);                    \
        Stack = &yyptr->Stack_alloc;                                    \
        yynewbytes = yystacksize * sizeof (*Stack) + YYSTACK_GAP_MAXIMUM; \
        yyptr += yynewbytes / sizeof (*yyptr);                          \
      }                                                                 \
    while (0)

#endif

#if defined YYCOPY_NEEDED && YYCOPY_NEEDED
/* Copy COUNT objects from SRC to DST.  The source and destination do
   not overlap.  */
# ifndef YYCOPY
#  if defined __GNUC__ && 1 < __GNUC__
#   define YYCOPY(Dst, Src, Count) \
      __builtin_memcpy (Dst, Src, (Count) * sizeof (*(Src)))
#  else
#   define YYCOPY(Dst, Src, Count)              \
      do                                        \
        {                                       \
          YYSIZE_T yyi;                         \
          for (yyi = 0; yyi < (Count); yyi++)   \
            (Dst)[yyi] = (Src)[yyi];            \
        }                                       \
      while (0)
#  endif
# endif
#endif /* !YYCOPY_NEEDED */

/* YYFINAL -- State number of the termination state.  */
#define YYFINAL  5
/* YYLAST -- Last index in YYTABLE.  */
#define YYLAST   780

/* YYNTOKENS -- Number of terminals.  */
#define YYNTOKENS  116
/* YYNNTS -- Number of nonterminals.  */
#define YYNNTS  72
/* YYNRULES -- Number of rules.  */
#define YYNRULES  178
/* YYNSTATES -- Number of states.  */
#define YYNSTATES  345

/* YYTRANSLATE[YYX] -- Symbol number corresponding to YYX as returned
   by yylex, with out-of-bounds checking.  */
#define YYUNDEFTOK  2
#define YYMAXUTOK   355

#define YYTRANSLATE(YYX)                                                \
  ((unsigned int) (YYX) <= YYMAXUTOK ? yytranslate[YYX] : YYUNDEFTOK)

/* YYTRANSLATE[TOKEN-NUM] -- Symbol number corresponding to TOKEN-NUM
   as returned by yylex, without out-of-bounds checking.  */
static const yytype_uint8 yytranslate[] =
{
       0,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,   108,     2,     2,
     110,   111,   105,   104,   113,   103,     2,   106,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,   109,
     101,   100,   102,   112,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,   114,     2,   115,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     1,     2,     3,     4,
       5,     6,     7,     8,     9,    10,    11,    12,    13,    14,
      15,    16,    17,    18,    19,    20,    21,    22,    23,    24,
      25,    26,    27,    28,    29,    30,    31,    32,    33,    34,
      35,    36,    37,    38,    39,    40,    41,    42,    43,    44,
      45,    46,    47,    48,    49,    50,    51,    52,    53,    54,
      55,    56,    57,    58,    59,    60,    61,    62,    63,    64,
      65,    66,    67,    68,    69,    70,    71,    72,    73,    74,
      75,    76,    77,    78,    79,    80,    81,    82,    83,    84,
      85,    86,    87,    88,    89,    90,    91,    92,    93,    94,
      95,    96,    97,    98,    99,   107
};

#if YYDEBUG
  /* YYRLINE[YYN] -- Source line where rule number YYN was defined.  */
static const yytype_uint16 yyrline[] =
{
       0,   160,   160,   163,   164,   165,   166,   167,   168,   169,
     170,   171,   172,   173,   174,   175,   176,   177,   178,   179,
     180,   181,   182,   183,   184,   188,   189,   194,   195,   197,
     198,   199,   200,   201,   202,   203,   204,   205,   206,   207,
     208,   209,   211,   212,   213,   214,   215,   216,   217,   218,
     219,   221,   226,   227,   228,   229,   231,   232,   233,   234,
     235,   236,   237,   240,   242,   243,   247,   253,   258,   259,
     260,   264,   268,   269,   274,   275,   276,   281,   282,   283,
     287,   288,   293,   299,   306,   307,   308,   313,   315,   318,
     322,   323,   327,   328,   333,   334,   339,   340,   341,   345,
     346,   353,   368,   373,   376,   384,   390,   391,   396,   402,
     411,   419,   427,   434,   442,   450,   456,   463,   469,   470,
     475,   476,   478,   482,   489,   495,   505,   509,   513,   520,
     527,   531,   539,   548,   549,   554,   555,   560,   561,   567,
     568,   574,   575,   580,   581,   586,   597,   598,   603,   604,
     608,   609,   613,   627,   628,   632,   637,   642,   643,   644,
     645,   646,   650,   655,   663,   664,   665,   670,   676,   678,
     679,   683,   691,   697,   698,   701,   703,   704,   708
};
#endif

#if YYDEBUG || YYERROR_VERBOSE || 0
/* YYTNAME[SYMBOL-NUM] -- String name of the symbol SYMBOL-NUM.
   First, the terminals, then, starting at YYNTOKENS, nonterminals.  */
static const char *const yytname[] =
{
  "$end", "error", "$undefined", "PARS_INT_LIT", "PARS_FLOAT_LIT",
  "PARS_STR_LIT", "PARS_NULL_LIT", "PARS_ID_TOKEN", "PARS_AND_TOKEN",
  "PARS_OR_TOKEN", "PARS_NOT_TOKEN", "PARS_GE_TOKEN", "PARS_LE_TOKEN",
  "PARS_NE_TOKEN", "PARS_PROCEDURE_TOKEN", "PARS_IN_TOKEN",
  "PARS_OUT_TOKEN", "PARS_BINARY_TOKEN", "PARS_BLOB_TOKEN",
  "PARS_INT_TOKEN", "PARS_FLOAT_TOKEN", "PARS_CHAR_TOKEN", "PARS_IS_TOKEN",
  "PARS_BEGIN_TOKEN", "PARS_END_TOKEN", "PARS_IF_TOKEN", "PARS_THEN_TOKEN",
  "PARS_ELSE_TOKEN", "PARS_ELSIF_TOKEN", "PARS_LOOP_TOKEN",
  "PARS_WHILE_TOKEN", "PARS_RETURN_TOKEN", "PARS_SELECT_TOKEN",
  "PARS_SUM_TOKEN", "PARS_COUNT_TOKEN", "PARS_DISTINCT_TOKEN",
  "PARS_FROM_TOKEN", "PARS_WHERE_TOKEN", "PARS_FOR_TOKEN",
  "PARS_DDOT_TOKEN", "PARS_READ_TOKEN", "PARS_ORDER_TOKEN",
  "PARS_BY_TOKEN", "PARS_ASC_TOKEN", "PARS_DESC_TOKEN",
  "PARS_INSERT_TOKEN", "PARS_INTO_TOKEN", "PARS_VALUES_TOKEN",
  "PARS_UPDATE_TOKEN", "PARS_SET_TOKEN", "PARS_DELETE_TOKEN",
  "PARS_CURRENT_TOKEN", "PARS_OF_TOKEN", "PARS_CREATE_TOKEN",
  "PARS_TABLE_TOKEN", "PARS_INDEX_TOKEN", "PARS_UNIQUE_TOKEN",
  "PARS_CLUSTERED_TOKEN", "PARS_ON_TOKEN", "PARS_ASSIGN_TOKEN",
  "PARS_DECLARE_TOKEN", "PARS_CURSOR_TOKEN", "PARS_SQL_TOKEN",
  "PARS_OPEN_TOKEN", "PARS_FETCH_TOKEN", "PARS_CLOSE_TOKEN",
  "PARS_NOTFOUND_TOKEN", "PARS_TO_CHAR_TOKEN", "PARS_TO_NUMBER_TOKEN",
  "PARS_TO_BINARY_TOKEN", "PARS_BINARY_TO_NUMBER_TOKEN",
  "PARS_SUBSTR_TOKEN", "PARS_REPLSTR_TOKEN", "PARS_CONCAT_TOKEN",
  "PARS_INSTR_TOKEN", "PARS_LENGTH_TOKEN", "PARS_SYSDATE_TOKEN",
  "PARS_PRINTF_TOKEN", "PARS_ASSERT_TOKEN", "PARS_RND_TOKEN",
  "PARS_RND_STR_TOKEN", "PARS_ROW_PRINTF_TOKEN", "PARS_COMMIT_TOKEN",
  "PARS_ROLLBACK_TOKEN", "PARS_WORK_TOKEN", "PARS_UNSIGNED_TOKEN",
  "PARS_EXIT_TOKEN", "PARS_FUNCTION_TOKEN", "PARS_LOCK_TOKEN",
  "PARS_SHARE_TOKEN", "PARS_MODE_TOKEN", "PARS_LIKE_TOKEN",
  "PARS_LIKE_TOKEN_EXACT", "PARS_LIKE_TOKEN_PREFIX",
  "PARS_LIKE_TOKEN_SUFFIX", "PARS_LIKE_TOKEN_SUBSTR",
  "PARS_TABLE_NAME_TOKEN", "PARS_COMPACT_TOKEN", "PARS_BLOCK_SIZE_TOKEN",
  "PARS_BIGINT_TOKEN", "'='", "'<'", "'>'", "'-'", "'+'", "'*'", "'/'",
  "NEG", "'%'", "';'", "'('", "')'", "'?'", "','", "'{'", "'}'", "$accept",
  "top_statement", "statement", "statement_list", "exp", "function_name",
  "question_mark_list", "stored_procedure_call",
  "predefined_procedure_call", "predefined_procedure_name",
  "user_function_call", "table_list", "variable_list", "exp_list",
  "select_item", "select_item_list", "select_list", "search_condition",
  "for_update_clause", "lock_shared_clause", "order_direction",
  "order_by_clause", "select_statement", "insert_statement_start",
  "insert_statement", "column_assignment", "column_assignment_list",
  "cursor_positioned", "update_statement_start",
  "update_statement_searched", "update_statement_positioned",
  "delete_statement_start", "delete_statement_searched",
  "delete_statement_positioned", "row_printf_statement",
  "assignment_statement", "elsif_element", "elsif_list", "else_part",
  "if_statement", "while_statement", "for_statement", "exit_statement",
  "return_statement", "open_cursor_statement", "close_cursor_statement",
  "fetch_statement", "column_def", "column_def_list", "opt_column_len",
  "opt_unsigned", "opt_not_null", "compact", "block_size", "create_table",
  "column_list", "unique_def", "clustered_def", "create_index",
  "table_name", "commit_statement", "rollback_statement", "type_name",
  "parameter_declaration", "parameter_declaration_list",
  "variable_declaration", "variable_declaration_list",
  "cursor_declaration", "function_declaration", "declaration",
  "declaration_list", "procedure_definition", YY_NULLPTR
};
#endif

# ifdef YYPRINT
/* YYTOKNUM[NUM] -- (External) token number corresponding to the
   (internal) symbol number NUM (which must be that of a token).  */
static const yytype_uint16 yytoknum[] =
{
       0,   256,   257,   258,   259,   260,   261,   262,   263,   264,
     265,   266,   267,   268,   269,   270,   271,   272,   273,   274,
     275,   276,   277,   278,   279,   280,   281,   282,   283,   284,
     285,   286,   287,   288,   289,   290,   291,   292,   293,   294,
     295,   296,   297,   298,   299,   300,   301,   302,   303,   304,
     305,   306,   307,   308,   309,   310,   311,   312,   313,   314,
     315,   316,   317,   318,   319,   320,   321,   322,   323,   324,
     325,   326,   327,   328,   329,   330,   331,   332,   333,   334,
     335,   336,   337,   338,   339,   340,   341,   342,   343,   344,
     345,   346,   347,   348,   349,   350,   351,   352,   353,   354,
      61,    60,    62,    45,    43,    42,    47,   355,    37,    59,
      40,    41,    63,    44,   123,   125
};
# endif

#define YYPACT_NINF -176

#define yypact_value_is_default(Yystate) \
  (!!((Yystate) == (-176)))

#define YYTABLE_NINF -1

#define yytable_value_is_error(Yytable_value) \
  0

  /* YYPACT[STATE-NUM] -- Index in YYTABLE of the portion describing
     STATE-NUM.  */
static const yytype_int16 yypact[] =
{
      20,    21,    41,   -64,   -59,  -176,  -176,    48,    54,  -176,
     -74,    12,    12,    45,    48,  -176,  -176,  -176,  -176,  -176,
    -176,  -176,    69,  -176,    12,  -176,     8,   -32,   -43,  -176,
    -176,  -176,  -176,   -13,  -176,    72,    81,   445,  -176,    75,
     -11,    42,   530,   530,  -176,    16,    99,    67,    -3,    78,
     -14,   108,   109,   110,  -176,  -176,  -176,    86,    36,    44,
    -176,   122,  -176,   216,  -176,    22,    23,    25,     6,    26,
      93,    27,    33,    93,    46,    51,    53,    56,    61,    63,
      64,    66,    68,    70,    71,    76,    79,    89,    94,    95,
      86,  -176,   530,  -176,  -176,  -176,  -176,    43,   530,    49,
    -176,  -176,  -176,  -176,  -176,  -176,  -176,  -176,  -176,  -176,
    -176,   530,   530,   570,    77,   603,    80,    96,  -176,   674,
    -176,   -38,   118,   161,    -3,  -176,  -176,   129,    -3,    -3,
    -176,   148,  -176,   163,  -176,  -176,  -176,  -176,    97,  -176,
    -176,  -176,   530,  -176,   100,  -176,  -176,   481,  -176,  -176,
    -176,  -176,  -176,  -176,  -176,  -176,  -176,  -176,  -176,  -176,
    -176,  -176,  -176,  -176,  -176,  -176,  -176,  -176,  -176,  -176,
     102,   674,   149,   220,   155,    14,    91,   530,   530,   530,
     530,   530,   445,   219,   530,   530,   530,   530,   530,   530,
     530,   530,   445,   530,   -24,   218,   267,    -3,   530,  -176,
     221,  -176,   117,  -176,   179,   228,   124,   674,   -65,   530,
     185,   674,  -176,  -176,  -176,  -176,   220,   220,    19,    19,
     674,   136,  -176,    19,    19,    19,     3,     3,    14,    14,
     -57,   326,   554,   231,   128,  -176,   130,  -176,    -1,  -176,
     610,   142,  -176,   131,   238,   242,   141,  -176,   130,  -176,
     -52,  -176,   530,   -51,   246,   445,   530,  -176,   227,   233,
    -176,   229,  -176,   151,  -176,   252,   530,    -3,   225,   530,
     530,   221,    12,  -176,   -48,   207,   156,   153,   164,   674,
    -176,  -176,   445,   626,  -176,   250,  -176,  -176,  -176,  -176,
     230,   194,   655,   674,  -176,   173,   187,   238,    -3,  -176,
    -176,  -176,   445,  -176,  -176,   270,   245,   445,   284,   204,
    -176,   192,  -176,   181,   445,   203,   253,  -176,   386,   193,
    -176,   286,   205,  -176,   296,   217,   299,   279,  -176,   303,
    -176,   307,  -176,   -47,  -176,    30,  -176,  -176,  -176,  -176,
     305,  -176,  -176,  -176,  -176
};

  /* YYDEFACT[STATE-NUM] -- Default reduction number in state STATE-NUM.
     Performed when YYTABLE does not specify something else to do.  Zero
     means the default is an error.  */
static const yytype_uint8 yydefact[] =
{
       0,     0,     0,     0,     0,     1,     2,   164,     0,   165,
       0,     0,     0,     0,     0,   160,   161,   157,   159,   158,
     162,   163,   168,   166,     0,   169,   175,     0,     0,   170,
     173,   174,   176,     0,   167,     0,     0,     0,   177,     0,
       0,     0,     0,     0,   127,    84,     0,     0,     0,     0,
     148,     0,     0,     0,    68,    69,    70,     0,     0,     0,
     126,     0,    25,     0,     3,     0,     0,     0,     0,     0,
      90,     0,     0,    90,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,   172,     0,    29,    30,    31,    32,    27,     0,    33,
      52,    53,    54,    55,    56,    57,    58,    59,    60,    61,
      62,     0,     0,     0,     0,     0,     0,     0,    87,    80,
      85,    89,     0,     0,     0,   153,   154,     0,     0,     0,
     149,   150,   128,     0,   129,   115,   155,   156,     0,   178,
      26,     4,    77,    11,     0,   104,    12,     0,   110,   111,
      16,    17,   113,   114,    14,    15,    13,    10,     8,     5,
       6,     7,     9,    18,    20,    19,    23,    24,    21,    22,
       0,   116,     0,    49,     0,    38,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,    77,     0,     0,     0,    74,     0,     0,     0,   102,
       0,   112,     0,   151,     0,    74,    63,    78,     0,    77,
       0,    91,   171,    50,    51,    39,    47,    48,    44,    45,
      46,   120,    41,    40,    42,    43,    35,    34,    36,    37,
       0,     0,     0,     0,     0,    75,    88,    86,    90,    72,
       0,     0,   106,   109,     0,     0,    75,   131,   130,    64,
       0,    67,     0,     0,     0,     0,     0,   118,   122,     0,
      28,     0,    83,     0,    81,     0,     0,     0,    92,     0,
       0,     0,     0,   133,     0,     0,     0,     0,     0,    79,
     103,   108,   121,     0,   119,     0,   124,    82,    76,    73,
       0,    94,     0,   105,   107,   135,   141,     0,     0,    71,
      66,    65,     0,   123,    93,     0,    99,     0,     0,   137,
     142,   143,   134,     0,   117,     0,     0,   101,     0,     0,
     138,   139,     0,   145,     0,     0,     0,     0,   136,     0,
     132,     0,   146,     0,    95,    96,   125,   140,   144,   152,
       0,    97,    98,   100,   147
};

  /* YYPGOTO[NTERM-NUM].  */
static const yytype_int16 yypgoto[] =
{
    -176,  -176,   -62,  -175,   -40,  -176,  -176,  -176,  -176,  -176,
    -176,  -176,   111,  -166,   119,  -176,  -176,   -67,  -176,  -176,
    -176,  -176,   -33,  -176,  -176,    47,  -176,   240,  -176,  -176,
    -176,  -176,  -176,  -176,  -176,  -176,    59,  -176,  -176,  -176,
    -176,  -176,  -176,  -176,  -176,  -176,  -176,    17,  -176,  -176,
    -176,  -176,  -176,  -176,  -176,  -176,  -176,  -176,  -176,  -115,
    -176,  -176,   -12,   313,  -176,   293,  -176,  -176,  -176,   295,
    -176,  -176
};

  /* YYDEFGOTO[NTERM-NUM].  */
static const yytype_int16 yydefgoto[] =
{
      -1,     2,    62,    63,   207,   114,   250,    64,    65,    66,
     247,   238,   236,   208,   120,   121,   122,   148,   291,   306,
     343,   317,    67,    68,    69,   242,   243,   149,    70,    71,
      72,    73,    74,    75,    76,    77,   257,   258,   259,    78,
      79,    80,    81,    82,    83,    84,    85,   273,   274,   309,
     321,   330,   311,   323,    86,   333,   131,   204,    87,   127,
      88,    89,    20,     9,    10,    25,    26,    30,    31,    32,
      33,     3
};

  /* YYTABLE[YYPACT[STATE-NUM]] -- What to do in state STATE-NUM.  If
     positive, shift that token.  If negative, reduce the rule whose
     number is the opposite.  If YYTABLE_NINF, syntax error.  */
static const yytype_uint16 yytable[] =
{
      21,   140,   113,   115,   125,   119,   152,   221,   195,   199,
      37,   233,    27,   201,   202,    24,   181,   231,    35,    93,
      94,    95,    96,    97,   135,   230,    98,   181,     4,    15,
      16,    17,   181,    18,     1,   145,   266,    13,    45,    14,
     129,     5,   130,   253,    36,     6,   251,    28,   252,   116,
     117,     7,   171,   144,   260,     8,   252,   170,   173,   277,
     280,   278,   252,   296,   339,   297,   340,    22,    28,    11,
      12,   175,   176,   341,   342,   196,    24,    34,    99,    39,
     282,   234,   239,   100,   101,   102,   103,   104,    40,   105,
     106,   107,   108,   126,   183,   109,   110,    90,    91,   177,
     178,    92,   179,   180,   181,   183,   123,   211,   189,   190,
     183,    19,   267,   124,   128,   132,   133,   134,    45,   111,
     136,   118,   187,   188,   189,   190,   112,   314,   137,   138,
     147,   141,   318,   142,   143,   146,   150,   216,   217,   218,
     219,   220,   151,    41,   223,   224,   225,   226,   227,   228,
     229,   172,   289,   232,   197,   154,   119,   174,   240,   140,
     155,    42,   156,   255,   256,   157,    43,    44,    45,   140,
     158,   268,   159,   160,    46,   161,   198,   162,   200,   163,
     164,    47,   183,   313,    48,   165,    49,   191,   166,    50,
     193,   184,   185,   186,   187,   188,   189,   190,   167,    51,
      52,    53,   215,   168,   169,   203,   194,   206,    54,   205,
     209,   212,   279,    55,    56,   213,   283,    57,    58,    59,
     140,   214,    60,    41,   222,   235,   211,   244,   241,   292,
     293,   179,   180,   181,   245,   246,   249,   254,   263,   264,
     139,    42,   270,   265,   271,   272,    43,    44,    45,   275,
      61,   276,   140,   281,    46,   256,   140,   285,   286,   288,
     295,    47,   287,   290,    48,   298,    49,   299,   300,    50,
      93,    94,    95,    96,    97,   303,   301,    98,   304,    51,
      52,    53,   305,   308,   310,   315,   316,   319,    54,   320,
     322,   324,   325,    55,    56,   326,   329,    57,    58,    59,
     116,   117,    60,   332,   328,   331,   335,   334,   336,   337,
     338,   183,   344,   153,   312,   237,   248,   284,   294,    29,
     184,   185,   186,   187,   188,   189,   190,    23,    38,    99,
      61,     0,     0,    41,   100,   101,   102,   103,   104,     0,
     105,   106,   107,   108,     0,     0,   109,   110,     0,     0,
     261,    42,     0,     0,     0,     0,    43,    44,    45,     0,
       0,     0,     0,     0,    46,     0,     0,     0,     0,     0,
     111,    47,     0,     0,    48,     0,    49,   112,     0,    50,
       0,     0,     0,     0,     0,     0,     0,     0,     0,    51,
      52,    53,     0,    41,     0,     0,     0,     0,    54,     0,
       0,     0,     0,    55,    56,     0,     0,    57,    58,    59,
     327,    42,    60,     0,     0,     0,    43,    44,    45,     0,
       0,     0,     0,     0,    46,     0,     0,     0,     0,     0,
       0,    47,     0,     0,    48,     0,    49,     0,     0,    50,
      61,     0,     0,     0,     0,     0,     0,     0,     0,    51,
      52,    53,    41,     0,     0,     0,     0,     0,    54,     0,
       0,     0,     0,    55,    56,     0,     0,    57,    58,    59,
      42,     0,    60,     0,     0,    43,    44,    45,     0,     0,
       0,     0,     0,    46,    93,    94,    95,    96,    97,     0,
      47,    98,     0,    48,     0,    49,     0,     0,    50,     0,
      61,     0,     0,     0,     0,     0,     0,     0,    51,    52,
      53,     0,     0,     0,     0,     0,     0,    54,     0,     0,
       0,     0,    55,    56,     0,     0,    57,    58,    59,     0,
       0,    60,   210,    93,    94,    95,    96,    97,     0,     0,
      98,     0,     0,    99,     0,     0,     0,     0,   100,   101,
     102,   103,   104,     0,   105,   106,   107,   108,     0,    61,
     109,   110,   177,   178,     0,   179,   180,   181,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,   177,   178,
       0,   179,   180,   181,   111,     0,     0,     0,     0,     0,
       0,   112,    99,     0,     0,     0,   182,   100,   101,   102,
     103,   104,     0,   105,   106,   107,   108,     0,     0,   109,
     110,   177,   178,     0,   179,   180,   181,     0,   177,   178,
       0,   179,   180,   181,     0,     0,     0,     0,     0,     0,
       0,     0,   192,   111,   177,   178,     0,   179,   180,   181,
     112,     0,     0,     0,     0,   183,     0,     0,     0,   269,
       0,     0,   302,     0,   184,   185,   186,   187,   188,   189,
     190,   183,     0,   177,   178,   262,   179,   180,   181,     0,
     184,   185,   186,   187,   188,   189,   190,     0,     0,     0,
       0,     0,   177,   178,   307,   179,   180,   181,     0,     0,
       0,     0,     0,     0,   183,     0,     0,     0,     0,     0,
       0,   183,     0,   184,   185,   186,   187,   188,   189,   190,
     184,   185,   186,   187,   188,   189,   190,   183,     0,     0,
       0,     0,     0,     0,     0,     0,   184,   185,   186,   187,
     188,   189,   190,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,   183,     0,     0,     0,
       0,     0,     0,     0,     0,   184,   185,   186,   187,   188,
     189,   190,     0,     0,     0,   183,     0,     0,     0,     0,
       0,     0,     0,     0,   184,   185,   186,   187,   188,   189,
     190
};

static const yytype_int16 yycheck[] =
{
      12,    63,    42,    43,     7,    45,    73,   182,    46,   124,
      23,    35,    24,   128,   129,     7,    13,   192,    61,     3,
       4,     5,     6,     7,    57,   191,    10,    13,     7,    17,
      18,    19,    13,    21,    14,    68,    37,   111,    32,   113,
      54,     0,    56,   209,    87,   109,   111,    60,   113,    33,
      34,   110,    92,    47,   111,     7,   113,    90,    98,   111,
     111,   113,   113,   111,   111,   113,   113,    22,    60,    15,
      16,   111,   112,    43,    44,   113,     7,   109,    62,     7,
     255,   105,   197,    67,    68,    69,    70,    71,     7,    73,
      74,    75,    76,    96,    91,    79,    80,    22,   109,     8,
       9,    59,    11,    12,    13,    91,     7,   147,   105,   106,
      91,    99,   113,    46,    36,     7,     7,     7,    32,   103,
      84,   105,   103,   104,   105,   106,   110,   302,    84,     7,
      37,   109,   307,   110,   109,   109,   109,   177,   178,   179,
     180,   181,   109,     7,   184,   185,   186,   187,   188,   189,
     190,   108,   267,   193,    36,   109,   196,   108,   198,   221,
     109,    25,   109,    27,    28,   109,    30,    31,    32,   231,
     109,   238,   109,   109,    38,   109,    15,   109,    49,   109,
     109,    45,    91,   298,    48,   109,    50,   110,   109,    53,
     110,   100,   101,   102,   103,   104,   105,   106,   109,    63,
      64,    65,   111,   109,   109,    57,   110,   110,    72,    46,
     110,   109,   252,    77,    78,    66,   256,    81,    82,    83,
     282,    66,    86,     7,     5,     7,   266,   110,     7,   269,
     270,    11,    12,    13,    55,     7,   112,    52,     7,   111,
      24,    25,   100,   113,   113,     7,    30,    31,    32,     7,
     114,   110,   314,     7,    38,    28,   318,    24,    29,     7,
     272,    45,   111,    38,    48,    58,    50,   111,   115,    53,
       3,     4,     5,     6,     7,    25,   112,    10,    48,    63,
      64,    65,    88,   110,    97,    15,    41,     3,    72,    85,
      98,   110,    89,    77,    78,    42,    10,    81,    82,    83,
      33,    34,    86,     7,   111,   100,     7,    90,    29,     6,
       3,    91,     7,    73,   297,   196,   205,   258,   271,    26,
     100,   101,   102,   103,   104,   105,   106,    14,    33,    62,
     114,    -1,    -1,     7,    67,    68,    69,    70,    71,    -1,
      73,    74,    75,    76,    -1,    -1,    79,    80,    -1,    -1,
      24,    25,    -1,    -1,    -1,    -1,    30,    31,    32,    -1,
      -1,    -1,    -1,    -1,    38,    -1,    -1,    -1,    -1,    -1,
     103,    45,    -1,    -1,    48,    -1,    50,   110,    -1,    53,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    63,
      64,    65,    -1,     7,    -1,    -1,    -1,    -1,    72,    -1,
      -1,    -1,    -1,    77,    78,    -1,    -1,    81,    82,    83,
      24,    25,    86,    -1,    -1,    -1,    30,    31,    32,    -1,
      -1,    -1,    -1,    -1,    38,    -1,    -1,    -1,    -1,    -1,
      -1,    45,    -1,    -1,    48,    -1,    50,    -1,    -1,    53,
     114,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    63,
      64,    65,     7,    -1,    -1,    -1,    -1,    -1,    72,    -1,
      -1,    -1,    -1,    77,    78,    -1,    -1,    81,    82,    83,
      25,    -1,    86,    -1,    -1,    30,    31,    32,    -1,    -1,
      -1,    -1,    -1,    38,     3,     4,     5,     6,     7,    -1,
      45,    10,    -1,    48,    -1,    50,    -1,    -1,    53,    -1,
     114,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    63,    64,
      65,    -1,    -1,    -1,    -1,    -1,    -1,    72,    -1,    -1,
      -1,    -1,    77,    78,    -1,    -1,    81,    82,    83,    -1,
      -1,    86,    51,     3,     4,     5,     6,     7,    -1,    -1,
      10,    -1,    -1,    62,    -1,    -1,    -1,    -1,    67,    68,
      69,    70,    71,    -1,    73,    74,    75,    76,    -1,   114,
      79,    80,     8,     9,    -1,    11,    12,    13,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,     8,     9,
      -1,    11,    12,    13,   103,    -1,    -1,    -1,    -1,    -1,
      -1,   110,    62,    -1,    -1,    -1,    26,    67,    68,    69,
      70,    71,    -1,    73,    74,    75,    76,    -1,    -1,    79,
      80,     8,     9,    -1,    11,    12,    13,    -1,     8,     9,
      -1,    11,    12,    13,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    29,   103,     8,     9,    -1,    11,    12,    13,
     110,    -1,    -1,    -1,    -1,    91,    -1,    -1,    -1,    39,
      -1,    -1,    26,    -1,   100,   101,   102,   103,   104,   105,
     106,    91,    -1,     8,     9,   111,    11,    12,    13,    -1,
     100,   101,   102,   103,   104,   105,   106,    -1,    -1,    -1,
      -1,    -1,     8,     9,    29,    11,    12,    13,    -1,    -1,
      -1,    -1,    -1,    -1,    91,    -1,    -1,    -1,    -1,    -1,
      -1,    91,    -1,   100,   101,   102,   103,   104,   105,   106,
     100,   101,   102,   103,   104,   105,   106,    91,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,   100,   101,   102,   103,
     104,   105,   106,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    91,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,   100,   101,   102,   103,   104,
     105,   106,    -1,    -1,    -1,    91,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,   100,   101,   102,   103,   104,   105,
     106
};

  /* YYSTOS[STATE-NUM] -- The (internal number of the) accessing
     symbol of state STATE-NUM.  */
static const yytype_uint8 yystos[] =
{
       0,    14,   117,   187,     7,     0,   109,   110,     7,   179,
     180,    15,    16,   111,   113,    17,    18,    19,    21,    99,
     178,   178,    22,   179,     7,   181,   182,   178,    60,   181,
     183,   184,   185,   186,   109,    61,    87,    23,   185,     7,
       7,     7,    25,    30,    31,    32,    38,    45,    48,    50,
      53,    63,    64,    65,    72,    77,    78,    81,    82,    83,
      86,   114,   118,   119,   123,   124,   125,   138,   139,   140,
     144,   145,   146,   147,   148,   149,   150,   151,   155,   156,
     157,   158,   159,   160,   161,   162,   170,   174,   176,   177,
      22,   109,    59,     3,     4,     5,     6,     7,    10,    62,
      67,    68,    69,    70,    71,    73,    74,    75,    76,    79,
      80,   103,   110,   120,   121,   120,    33,    34,   105,   120,
     130,   131,   132,     7,    46,     7,    96,   175,    36,    54,
      56,   172,     7,     7,     7,   138,    84,    84,     7,    24,
     118,   109,   110,   109,    47,   138,   109,    37,   133,   143,
     109,   109,   133,   143,   109,   109,   109,   109,   109,   109,
     109,   109,   109,   109,   109,   109,   109,   109,   109,   109,
     138,   120,   108,   120,   108,   120,   120,     8,     9,    11,
      12,    13,    26,    91,   100,   101,   102,   103,   104,   105,
     106,   110,    29,   110,   110,    46,   113,    36,    15,   175,
      49,   175,   175,    57,   173,    46,   110,   120,   129,   110,
      51,   120,   109,    66,    66,   111,   120,   120,   120,   120,
     120,   119,     5,   120,   120,   120,   120,   120,   120,   120,
     129,   119,   120,    35,   105,     7,   128,   130,   127,   175,
     120,     7,   141,   142,   110,    55,     7,   126,   128,   112,
     122,   111,   113,   129,    52,    27,    28,   152,   153,   154,
     111,    24,   111,     7,   111,   113,    37,   113,   133,    39,
     100,   113,     7,   163,   164,     7,   110,   111,   113,   120,
     111,     7,   119,   120,   152,    24,    29,   111,     7,   175,
      38,   134,   120,   120,   141,   178,   111,   113,    58,   111,
     115,   112,    26,    25,    48,    88,   135,    29,   110,   165,
      97,   168,   163,   175,   119,    15,    41,   137,   119,     3,
      85,   166,    98,   169,   110,    89,    42,    24,   111,    10,
     167,   100,     7,   171,    90,     7,    29,     6,     3,   111,
     113,    43,    44,   136,     7
};

  /* YYR1[YYN] -- Symbol number of symbol that rule YYN derives.  */
static const yytype_uint8 yyr1[] =
{
       0,   116,   117,   118,   118,   118,   118,   118,   118,   118,
     118,   118,   118,   118,   118,   118,   118,   118,   118,   118,
     118,   118,   118,   118,   118,   119,   119,   120,   120,   120,
     120,   120,   120,   120,   120,   120,   120,   120,   120,   120,
     120,   120,   120,   120,   120,   120,   120,   120,   120,   120,
     120,   120,   121,   121,   121,   121,   121,   121,   121,   121,
     121,   121,   121,   122,   122,   122,   123,   124,   125,   125,
     125,   126,   127,   127,   128,   128,   128,   129,   129,   129,
     130,   130,   130,   130,   131,   131,   131,   132,   132,   132,
     133,   133,   134,   134,   135,   135,   136,   136,   136,   137,
     137,   138,   139,   140,   140,   141,   142,   142,   143,   144,
     145,   146,   147,   148,   149,   150,   151,   152,   153,   153,
     154,   154,   154,   155,   156,   157,   158,   159,   160,   161,
     162,   162,   163,   164,   164,   165,   165,   166,   166,   167,
     167,   168,   168,   169,   169,   170,   171,   171,   172,   172,
     173,   173,   174,   175,   175,   176,   177,   178,   178,   178,
     178,   178,   179,   179,   180,   180,   180,   181,   182,   182,
     182,   183,   184,   185,   185,   186,   186,   186,   187
};

  /* YYR2[YYN] -- Number of symbols on the right hand side of rule YYN.  */
static const yytype_uint8 yyr2[] =
{
       0,     2,     2,     1,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     1,     2,     1,     4,     1,
       1,     1,     1,     1,     3,     3,     3,     3,     2,     3,
       3,     3,     3,     3,     3,     3,     3,     3,     3,     2,
       3,     3,     1,     1,     1,     1,     1,     1,     1,     1,
       1,     1,     1,     0,     1,     3,     6,     4,     1,     1,
       1,     3,     1,     3,     0,     1,     3,     0,     1,     3,
       1,     4,     5,     4,     0,     1,     3,     1,     3,     1,
       0,     2,     0,     2,     0,     4,     0,     1,     1,     0,
       4,     8,     3,     5,     2,     3,     1,     3,     4,     4,
       2,     2,     3,     2,     2,     2,     3,     4,     1,     2,
       0,     2,     1,     7,     6,    10,     1,     1,     2,     2,
       4,     4,     5,     1,     3,     0,     3,     0,     1,     0,
       2,     0,     1,     0,     3,     8,     1,     3,     0,     1,
       0,     1,    10,     1,     1,     2,     2,     1,     1,     1,
       1,     1,     3,     3,     0,     1,     3,     3,     0,     1,
       2,     6,     4,     1,     1,     0,     1,     2,    11
};


#define yyerrok         (yyerrstatus = 0)
#define yyclearin       (yychar = YYEMPTY)
#define YYEMPTY         (-2)
#define YYEOF           0

#define YYACCEPT        goto yyacceptlab
#define YYABORT         goto yyabortlab
#define YYERROR         goto yyerrorlab


#define YYRECOVERING()  (!!yyerrstatus)

#define YYBACKUP(Token, Value)                                  \
do                                                              \
  if (yychar == YYEMPTY)                                        \
    {                                                           \
      yychar = (Token);                                         \
      yylval = (Value);                                         \
      YYPOPSTACK (yylen);                                       \
      yystate = *yyssp;                                         \
      goto yybackup;                                            \
    }                                                           \
  else                                                          \
    {                                                           \
      yyerror (YY_("syntax error: cannot back up")); \
      YYERROR;                                                  \
    }                                                           \
while (0)

/* Error token number */
#define YYTERROR        1
#define YYERRCODE       256



/* Enable debugging if requested.  */
#if YYDEBUG

# ifndef YYFPRINTF
#  include <stdio.h> /* INFRINGES ON USER NAME SPACE */
#  define YYFPRINTF fprintf
# endif

# define YYDPRINTF(Args)                        \
do {                                            \
  if (yydebug)                                  \
    YYFPRINTF Args;                             \
} while (0)

/* This macro is provided for backward compatibility. */
#ifndef YY_LOCATION_PRINT
# define YY_LOCATION_PRINT(File, Loc) ((void) 0)
#endif


# define YY_SYMBOL_PRINT(Title, Type, Value, Location)                    \
do {                                                                      \
  if (yydebug)                                                            \
    {                                                                     \
      YYFPRINTF (stderr, "%s ", Title);                                   \
      yy_symbol_print (stderr,                                            \
                  Type, Value); \
      YYFPRINTF (stderr, "\n");                                           \
    }                                                                     \
} while (0)


/*----------------------------------------.
| Print this symbol's value on YYOUTPUT.  |
`----------------------------------------*/

static void
yy_symbol_value_print (FILE *yyoutput, int yytype, YYSTYPE const * const yyvaluep)
{
  FILE *yyo = yyoutput;
  YYUSE (yyo);
  if (!yyvaluep)
    return;
# ifdef YYPRINT
  if (yytype < YYNTOKENS)
    YYPRINT (yyoutput, yytoknum[yytype], *yyvaluep);
# endif
  YYUSE (yytype);
}


/*--------------------------------.
| Print this symbol on YYOUTPUT.  |
`--------------------------------*/

static void
yy_symbol_print (FILE *yyoutput, int yytype, YYSTYPE const * const yyvaluep)
{
  YYFPRINTF (yyoutput, "%s %s (",
             yytype < YYNTOKENS ? "token" : "nterm", yytname[yytype]);

  yy_symbol_value_print (yyoutput, yytype, yyvaluep);
  YYFPRINTF (yyoutput, ")");
}

/*------------------------------------------------------------------.
| yy_stack_print -- Print the state stack from its BOTTOM up to its |
| TOP (included).                                                   |
`------------------------------------------------------------------*/

static void
yy_stack_print (yytype_int16 *yybottom, yytype_int16 *yytop)
{
  YYFPRINTF (stderr, "Stack now");
  for (; yybottom <= yytop; yybottom++)
    {
      int yybot = *yybottom;
      YYFPRINTF (stderr, " %d", yybot);
    }
  YYFPRINTF (stderr, "\n");
}

# define YY_STACK_PRINT(Bottom, Top)                            \
do {                                                            \
  if (yydebug)                                                  \
    yy_stack_print ((Bottom), (Top));                           \
} while (0)


/*------------------------------------------------.
| Report that the YYRULE is going to be reduced.  |
`------------------------------------------------*/

static void
yy_reduce_print (yytype_int16 *yyssp, YYSTYPE *yyvsp, int yyrule)
{
  unsigned long int yylno = yyrline[yyrule];
  int yynrhs = yyr2[yyrule];
  int yyi;
  YYFPRINTF (stderr, "Reducing stack by rule %d (line %lu):\n",
             yyrule - 1, yylno);
  /* The symbols being reduced.  */
  for (yyi = 0; yyi < yynrhs; yyi++)
    {
      YYFPRINTF (stderr, "   $%d = ", yyi + 1);
      yy_symbol_print (stderr,
                       yystos[yyssp[yyi + 1 - yynrhs]],
                       &(yyvsp[(yyi + 1) - (yynrhs)])
                                              );
      YYFPRINTF (stderr, "\n");
    }
}

# define YY_REDUCE_PRINT(Rule)          \
do {                                    \
  if (yydebug)                          \
    yy_reduce_print (yyssp, yyvsp, Rule); \
} while (0)

/* Nonzero means print parse trace.  It is left uninitialized so that
   multiple parsers can coexist.  */
int yydebug;
#else /* !YYDEBUG */
# define YYDPRINTF(Args)
# define YY_SYMBOL_PRINT(Title, Type, Value, Location)
# define YY_STACK_PRINT(Bottom, Top)
# define YY_REDUCE_PRINT(Rule)
#endif /* !YYDEBUG */


/* YYINITDEPTH -- initial size of the parser's stacks.  */
#ifndef YYINITDEPTH
# define YYINITDEPTH 200
#endif

/* YYMAXDEPTH -- maximum size the stacks can grow to (effective only
   if the built-in stack extension method is used).

   Do not make this value too large; the results are undefined if
   YYSTACK_ALLOC_MAXIMUM < YYSTACK_BYTES (YYMAXDEPTH)
   evaluated with infinite-precision integer arithmetic.  */

#ifndef YYMAXDEPTH
# define YYMAXDEPTH 10000
#endif


#if YYERROR_VERBOSE

# ifndef yystrlen
#  if defined __GLIBC__ && defined _STRING_H
#   define yystrlen strlen
#  else
/* Return the length of YYSTR.  */
static YYSIZE_T
yystrlen (const char *yystr)
{
  YYSIZE_T yylen;
  for (yylen = 0; yystr[yylen]; yylen++)
    continue;
  return yylen;
}
#  endif
# endif

# ifndef yystpcpy
#  if defined __GLIBC__ && defined _STRING_H && defined _GNU_SOURCE
#   define yystpcpy stpcpy
#  else
/* Copy YYSRC to YYDEST, returning the address of the terminating '\0' in
   YYDEST.  */
static char *
yystpcpy (char *yydest, const char *yysrc)
{
  char *yyd = yydest;
  const char *yys = yysrc;

  while ((*yyd++ = *yys++) != '\0')
    continue;

  return yyd - 1;
}
#  endif
# endif

# ifndef yytnamerr
/* Copy to YYRES the contents of YYSTR after stripping away unnecessary
   quotes and backslashes, so that it's suitable for yyerror.  The
   heuristic is that double-quoting is unnecessary unless the string
   contains an apostrophe, a comma, or backslash (other than
   backslash-backslash).  YYSTR is taken from yytname.  If YYRES is
   null, do not copy; instead, return the length of what the result
   would have been.  */
static YYSIZE_T
yytnamerr (char *yyres, const char *yystr)
{
  if (*yystr == '"')
    {
      YYSIZE_T yyn = 0;
      char const *yyp = yystr;

      for (;;)
        switch (*++yyp)
          {
          case '\'':
          case ',':
            goto do_not_strip_quotes;

          case '\\':
            if (*++yyp != '\\')
              goto do_not_strip_quotes;
            /* Fall through.  */
          default:
            if (yyres)
              yyres[yyn] = *yyp;
            yyn++;
            break;

          case '"':
            if (yyres)
              yyres[yyn] = '\0';
            return yyn;
          }
    do_not_strip_quotes: ;
    }

  if (! yyres)
    return yystrlen (yystr);

  return yystpcpy (yyres, yystr) - yyres;
}
# endif

/* Copy into *YYMSG, which is of size *YYMSG_ALLOC, an error message
   about the unexpected token YYTOKEN for the state stack whose top is
   YYSSP.

   Return 0 if *YYMSG was successfully written.  Return 1 if *YYMSG is
   not large enough to hold the message.  In that case, also set
   *YYMSG_ALLOC to the required number of bytes.  Return 2 if the
   required number of bytes is too large to store.  */
static int
yysyntax_error (YYSIZE_T *yymsg_alloc, char **yymsg,
                yytype_int16 *yyssp, int yytoken)
{
  YYSIZE_T yysize0 = yytnamerr (YY_NULLPTR, yytname[yytoken]);
  YYSIZE_T yysize = yysize0;
  enum { YYERROR_VERBOSE_ARGS_MAXIMUM = 5 };
  /* Internationalized format string. */
  const char *yyformat = YY_NULLPTR;
  /* Arguments of yyformat. */
  char const *yyarg[YYERROR_VERBOSE_ARGS_MAXIMUM];
  /* Number of reported tokens (one for the "unexpected", one per
     "expected"). */
  int yycount = 0;

  /* There are many possibilities here to consider:
     - If this state is a consistent state with a default action, then
       the only way this function was invoked is if the default action
       is an error action.  In that case, don't check for expected
       tokens because there are none.
     - The only way there can be no lookahead present (in yychar) is if
       this state is a consistent state with a default action.  Thus,
       detecting the absence of a lookahead is sufficient to determine
       that there is no unexpected or expected token to report.  In that
       case, just report a simple "syntax error".
     - Don't assume there isn't a lookahead just because this state is a
       consistent state with a default action.  There might have been a
       previous inconsistent state, consistent state with a non-default
       action, or user semantic action that manipulated yychar.
     - Of course, the expected token list depends on states to have
       correct lookahead information, and it depends on the parser not
       to perform extra reductions after fetching a lookahead from the
       scanner and before detecting a syntax error.  Thus, state merging
       (from LALR or IELR) and default reductions corrupt the expected
       token list.  However, the list is correct for canonical LR with
       one exception: it will still contain any token that will not be
       accepted due to an error action in a later state.
  */
  if (yytoken != YYEMPTY)
    {
      int yyn = yypact[*yyssp];
      yyarg[yycount++] = yytname[yytoken];
      if (!yypact_value_is_default (yyn))
        {
          /* Start YYX at -YYN if negative to avoid negative indexes in
             YYCHECK.  In other words, skip the first -YYN actions for
             this state because they are default actions.  */
          int yyxbegin = yyn < 0 ? -yyn : 0;
          /* Stay within bounds of both yycheck and yytname.  */
          int yychecklim = YYLAST - yyn + 1;
          int yyxend = yychecklim < YYNTOKENS ? yychecklim : YYNTOKENS;
          int yyx;

          for (yyx = yyxbegin; yyx < yyxend; ++yyx)
            if (yycheck[yyx + yyn] == yyx && yyx != YYTERROR
                && !yytable_value_is_error (yytable[yyx + yyn]))
              {
                if (yycount == YYERROR_VERBOSE_ARGS_MAXIMUM)
                  {
                    yycount = 1;
                    yysize = yysize0;
                    break;
                  }
                yyarg[yycount++] = yytname[yyx];
                {
                  YYSIZE_T yysize1 = yysize + yytnamerr (YY_NULLPTR, yytname[yyx]);
                  if (! (yysize <= yysize1
                         && yysize1 <= YYSTACK_ALLOC_MAXIMUM))
                    return 2;
                  yysize = yysize1;
                }
              }
        }
    }

  switch (yycount)
    {
# define YYCASE_(N, S)                      \
      case N:                               \
        yyformat = S;                       \
      break
      YYCASE_(0, YY_("syntax error"));
      YYCASE_(1, YY_("syntax error, unexpected %s"));
      YYCASE_(2, YY_("syntax error, unexpected %s, expecting %s"));
      YYCASE_(3, YY_("syntax error, unexpected %s, expecting %s or %s"));
      YYCASE_(4, YY_("syntax error, unexpected %s, expecting %s or %s or %s"));
      YYCASE_(5, YY_("syntax error, unexpected %s, expecting %s or %s or %s or %s"));
# undef YYCASE_
    }

  {
    YYSIZE_T yysize1 = yysize + yystrlen (yyformat);
    if (! (yysize <= yysize1 && yysize1 <= YYSTACK_ALLOC_MAXIMUM))
      return 2;
    yysize = yysize1;
  }

  if (*yymsg_alloc < yysize)
    {
      *yymsg_alloc = 2 * yysize;
      if (! (yysize <= *yymsg_alloc
             && *yymsg_alloc <= YYSTACK_ALLOC_MAXIMUM))
        *yymsg_alloc = YYSTACK_ALLOC_MAXIMUM;
      return 1;
    }

  /* Avoid sprintf, as that infringes on the user's name space.
     Don't have undefined behavior even if the translation
     produced a string with the wrong number of "%s"s.  */
  {
    char *yyp = *yymsg;
    int yyi = 0;
    while ((*yyp = *yyformat) != '\0')
      if (*yyp == '%' && yyformat[1] == 's' && yyi < yycount)
        {
          yyp += yytnamerr (yyp, yyarg[yyi++]);
          yyformat += 2;
        }
      else
        {
          yyp++;
          yyformat++;
        }
  }
  return 0;
}
#endif /* YYERROR_VERBOSE */

/*-----------------------------------------------.
| Release the memory associated to this symbol.  |
`-----------------------------------------------*/

static void
yydestruct (const char *yymsg, int yytype, YYSTYPE *yyvaluep)
{
  YYUSE (yyvaluep);
  if (!yymsg)
    yymsg = "Deleting";
  YY_SYMBOL_PRINT (yymsg, yytype, yyvaluep, yylocationp);

  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  YYUSE (yytype);
  YY_IGNORE_MAYBE_UNINITIALIZED_END
}




/* The lookahead symbol.  */
static int yychar;

/* The semantic value of the lookahead symbol.  */
YYSTYPE yylval;
/* Number of syntax errors so far.  */
static int yynerrs;


/*----------.
| yyparse.  |
`----------*/

int
yyparse (void)
{
    int yystate;
    /* Number of tokens to shift before error messages enabled.  */
    int yyerrstatus;

    /* The stacks and their tools:
       'yyss': related to states.
       'yyvs': related to semantic values.

       Refer to the stacks through separate pointers, to allow yyoverflow
       to reallocate them elsewhere.  */

    /* The state stack.  */
    yytype_int16 yyssa[YYINITDEPTH];
    yytype_int16 *yyss;
    yytype_int16 *yyssp;

    /* The semantic value stack.  */
    YYSTYPE yyvsa[YYINITDEPTH];
    YYSTYPE *yyvs;
    YYSTYPE *yyvsp;

    YYSIZE_T yystacksize;

  int yyn;
  int yyresult;
  /* Lookahead token as an internal (translated) token number.  */
  int yytoken = 0;
  /* The variables used to return semantic value and location from the
     action routines.  */
  YYSTYPE yyval;

#if YYERROR_VERBOSE
  /* Buffer for error messages, and its allocated size.  */
  char yymsgbuf[128];
  char *yymsg = yymsgbuf;
  YYSIZE_T yymsg_alloc = sizeof yymsgbuf;
#endif

#define YYPOPSTACK(N)   (yyvsp -= (N), yyssp -= (N))

  /* The number of symbols on the RHS of the reduced rule.
     Keep to zero when no symbol should be popped.  */
  int yylen = 0;

  yyssp = yyss = yyssa;
  yyvsp = yyvs = yyvsa;
  yystacksize = YYINITDEPTH;

  YYDPRINTF ((stderr, "Starting parse\n"));

  yystate = 0;
  yyerrstatus = 0;
  yynerrs = 0;
  yychar = YYEMPTY; /* Cause a token to be read.  */
  goto yysetstate;

/*------------------------------------------------------------.
| yynewstate -- Push a new state, which is found in yystate.  |
`------------------------------------------------------------*/
 yynewstate:
  /* In all cases, when you get here, the value and location stacks
     have just been pushed.  So pushing a state here evens the stacks.  */
  yyssp++;

 yysetstate:
  *yyssp = yystate;

  if (yyss + yystacksize - 1 <= yyssp)
    {
      /* Get the current used size of the three stacks, in elements.  */
      YYSIZE_T yysize = yyssp - yyss + 1;

#ifdef yyoverflow
      {
        /* Give user a chance to reallocate the stack.  Use copies of
           these so that the &'s don't force the real ones into
           memory.  */
        YYSTYPE *yyvs1 = yyvs;
        yytype_int16 *yyss1 = yyss;

        /* Each stack pointer address is followed by the size of the
           data in use in that stack, in bytes.  This used to be a
           conditional around just the two extra args, but that might
           be undefined if yyoverflow is a macro.  */
        yyoverflow (YY_("memory exhausted"),
                    &yyss1, yysize * sizeof (*yyssp),
                    &yyvs1, yysize * sizeof (*yyvsp),
                    &yystacksize);

        yyss = yyss1;
        yyvs = yyvs1;
      }
#else /* no yyoverflow */
# ifndef YYSTACK_RELOCATE
      goto yyexhaustedlab;
# else
      /* Extend the stack our own way.  */
      if (YYMAXDEPTH <= yystacksize)
        goto yyexhaustedlab;
      yystacksize *= 2;
      if (YYMAXDEPTH < yystacksize)
        yystacksize = YYMAXDEPTH;

      {
        yytype_int16 *yyss1 = yyss;
        union yyalloc *yyptr =
          (union yyalloc *) YYSTACK_ALLOC (YYSTACK_BYTES (yystacksize));
        if (! yyptr)
          goto yyexhaustedlab;
        YYSTACK_RELOCATE (yyss_alloc, yyss);
        YYSTACK_RELOCATE (yyvs_alloc, yyvs);
#  undef YYSTACK_RELOCATE
        if (yyss1 != yyssa)
          YYSTACK_FREE (yyss1);
      }
# endif
#endif /* no yyoverflow */

      yyssp = yyss + yysize - 1;
      yyvsp = yyvs + yysize - 1;

      YYDPRINTF ((stderr, "Stack size increased to %lu\n",
                  (unsigned long int) yystacksize));

      if (yyss + yystacksize - 1 <= yyssp)
        YYABORT;
    }

  YYDPRINTF ((stderr, "Entering state %d\n", yystate));

  if (yystate == YYFINAL)
    YYACCEPT;

  goto yybackup;

/*-----------.
| yybackup.  |
`-----------*/
yybackup:

  /* Do appropriate processing given the current state.  Read a
     lookahead token if we need one and don't already have one.  */

  /* First try to decide what to do without reference to lookahead token.  */
  yyn = yypact[yystate];
  if (yypact_value_is_default (yyn))
    goto yydefault;

  /* Not known => get a lookahead token if don't already have one.  */

  /* YYCHAR is either YYEMPTY or YYEOF or a valid lookahead symbol.  */
  if (yychar == YYEMPTY)
    {
      YYDPRINTF ((stderr, "Reading a token: "));
      yychar = yylex ();
    }

  if (yychar <= YYEOF)
    {
      yychar = yytoken = YYEOF;
      YYDPRINTF ((stderr, "Now at end of input.\n"));
    }
  else
    {
      yytoken = YYTRANSLATE (yychar);
      YY_SYMBOL_PRINT ("Next token is", yytoken, &yylval, &yylloc);
    }

  /* If the proper action on seeing token YYTOKEN is to reduce or to
     detect an error, take that action.  */
  yyn += yytoken;
  if (yyn < 0 || YYLAST < yyn || yycheck[yyn] != yytoken)
    goto yydefault;
  yyn = yytable[yyn];
  if (yyn <= 0)
    {
      if (yytable_value_is_error (yyn))
        goto yyerrlab;
      yyn = -yyn;
      goto yyreduce;
    }

  /* Count tokens shifted since error; after three, turn off error
     status.  */
  if (yyerrstatus)
    yyerrstatus--;

  /* Shift the lookahead token.  */
  YY_SYMBOL_PRINT ("Shifting", yytoken, &yylval, &yylloc);

  /* Discard the shifted token.  */
  yychar = YYEMPTY;

  yystate = yyn;
  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  *++yyvsp = yylval;
  YY_IGNORE_MAYBE_UNINITIALIZED_END

  goto yynewstate;


/*-----------------------------------------------------------.
| yydefault -- do the default action for the current state.  |
`-----------------------------------------------------------*/
yydefault:
  yyn = yydefact[yystate];
  if (yyn == 0)
    goto yyerrlab;
  goto yyreduce;


/*-----------------------------.
| yyreduce -- Do a reduction.  |
`-----------------------------*/
yyreduce:
  /* yyn is the number of a rule to reduce with.  */
  yylen = yyr2[yyn];

  /* If YYLEN is nonzero, implement the default value of the action:
     '$$ = $1'.

     Otherwise, the following line sets YYVAL to garbage.
     This behavior is undocumented and Bison
     users should not rely upon it.  Assigning to YYVAL
     unconditionally makes the parser a bit smaller, and it avoids a
     GCC warning that YYVAL may be used uninitialized.  */
  yyval = yyvsp[1-yylen];


  YY_REDUCE_PRINT (yyn);
  switch (yyn)
    {
        case 25:
#line 188 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = que_node_list_add_last(NULL, (yyvsp[0])); }
#line 1701 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 26:
#line 190 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = que_node_list_add_last((yyvsp[-1]), (yyvsp[0])); }
#line 1707 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 27:
#line 194 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = (yyvsp[0]);}
#line 1713 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 28:
#line 196 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_func((yyvsp[-3]), (yyvsp[-1])); }
#line 1719 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 29:
#line 197 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = (yyvsp[0]);}
#line 1725 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 30:
#line 198 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = (yyvsp[0]);}
#line 1731 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 31:
#line 199 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = (yyvsp[0]);}
#line 1737 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 32:
#line 200 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = (yyvsp[0]);}
#line 1743 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 33:
#line 201 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = (yyvsp[0]);}
#line 1749 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 34:
#line 202 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_op('+', (yyvsp[-2]), (yyvsp[0])); }
#line 1755 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 35:
#line 203 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_op('-', (yyvsp[-2]), (yyvsp[0])); }
#line 1761 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 36:
#line 204 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_op('*', (yyvsp[-2]), (yyvsp[0])); }
#line 1767 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 37:
#line 205 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_op('/', (yyvsp[-2]), (yyvsp[0])); }
#line 1773 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 38:
#line 206 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_op('-', (yyvsp[0]), NULL); }
#line 1779 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 39:
#line 207 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = (yyvsp[-1]); }
#line 1785 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 40:
#line 208 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_op('=', (yyvsp[-2]), (yyvsp[0])); }
#line 1791 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 41:
#line 210 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_op(PARS_LIKE_TOKEN, (yyvsp[-2]), (yyvsp[0])); }
#line 1797 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 42:
#line 211 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_op('<', (yyvsp[-2]), (yyvsp[0])); }
#line 1803 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 43:
#line 212 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_op('>', (yyvsp[-2]), (yyvsp[0])); }
#line 1809 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 44:
#line 213 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_op(PARS_GE_TOKEN, (yyvsp[-2]), (yyvsp[0])); }
#line 1815 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 45:
#line 214 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_op(PARS_LE_TOKEN, (yyvsp[-2]), (yyvsp[0])); }
#line 1821 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 46:
#line 215 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_op(PARS_NE_TOKEN, (yyvsp[-2]), (yyvsp[0])); }
#line 1827 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 47:
#line 216 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_op(PARS_AND_TOKEN, (yyvsp[-2]), (yyvsp[0])); }
#line 1833 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 48:
#line 217 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_op(PARS_OR_TOKEN, (yyvsp[-2]), (yyvsp[0])); }
#line 1839 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 49:
#line 218 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_op(PARS_NOT_TOKEN, (yyvsp[0]), NULL); }
#line 1845 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 50:
#line 220 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_op(PARS_NOTFOUND_TOKEN, (yyvsp[-2]), NULL); }
#line 1851 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 51:
#line 222 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_op(PARS_NOTFOUND_TOKEN, (yyvsp[-2]), NULL); }
#line 1857 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 52:
#line 226 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = &pars_to_char_token; }
#line 1863 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 53:
#line 227 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = &pars_to_number_token; }
#line 1869 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 54:
#line 228 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = &pars_to_binary_token; }
#line 1875 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 55:
#line 230 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = &pars_binary_to_number_token; }
#line 1881 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 56:
#line 231 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = &pars_substr_token; }
#line 1887 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 57:
#line 232 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = &pars_concat_token; }
#line 1893 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 58:
#line 233 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = &pars_instr_token; }
#line 1899 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 59:
#line 234 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = &pars_length_token; }
#line 1905 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 60:
#line 235 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = &pars_sysdate_token; }
#line 1911 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 61:
#line 236 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = &pars_rnd_token; }
#line 1917 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 62:
#line 237 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = &pars_rnd_str_token; }
#line 1923 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 66:
#line 248 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_stored_procedure_call(
					static_cast<sym_node_t*>((yyvsp[-4]))); }
#line 1930 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 67:
#line 254 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_procedure_call((yyvsp[-3]), (yyvsp[-1])); }
#line 1936 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 68:
#line 258 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = &pars_replstr_token; }
#line 1942 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 69:
#line 259 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = &pars_printf_token; }
#line 1948 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 70:
#line 260 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = &pars_assert_token; }
#line 1954 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 71:
#line 264 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = (yyvsp[-2]); }
#line 1960 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 72:
#line 268 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = que_node_list_add_last(NULL, (yyvsp[0])); }
#line 1966 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 73:
#line 270 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = que_node_list_add_last((yyvsp[-2]), (yyvsp[0])); }
#line 1972 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 74:
#line 274 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = NULL; }
#line 1978 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 75:
#line 275 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = que_node_list_add_last(NULL, (yyvsp[0])); }
#line 1984 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 76:
#line 277 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = que_node_list_add_last((yyvsp[-2]), (yyvsp[0])); }
#line 1990 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 77:
#line 281 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = NULL; }
#line 1996 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 78:
#line 282 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = que_node_list_add_last(NULL, (yyvsp[0]));}
#line 2002 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 79:
#line 283 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = que_node_list_add_last((yyvsp[-2]), (yyvsp[0])); }
#line 2008 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 80:
#line 287 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = (yyvsp[0]); }
#line 2014 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 81:
#line 289 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_func(&pars_count_token,
				          que_node_list_add_last(NULL,
					    sym_tab_add_int_lit(
						pars_sym_tab_global, 1))); }
#line 2023 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 82:
#line 294 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_func(&pars_count_token,
					    que_node_list_add_last(NULL,
						pars_func(&pars_distinct_token,
						     que_node_list_add_last(
								NULL, (yyvsp[-1]))))); }
#line 2033 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 83:
#line 300 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_func(&pars_sum_token,
						que_node_list_add_last(NULL,
									(yyvsp[-1]))); }
#line 2041 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 84:
#line 306 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = NULL; }
#line 2047 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 85:
#line 307 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = que_node_list_add_last(NULL, (yyvsp[0])); }
#line 2053 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 86:
#line 309 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = que_node_list_add_last((yyvsp[-2]), (yyvsp[0])); }
#line 2059 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 87:
#line 313 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_select_list(&pars_star_denoter,
								NULL); }
#line 2066 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 88:
#line 316 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_select_list(
					(yyvsp[-2]), static_cast<sym_node_t*>((yyvsp[0]))); }
#line 2073 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 89:
#line 318 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_select_list((yyvsp[0]), NULL); }
#line 2079 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 90:
#line 322 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = NULL; }
#line 2085 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 91:
#line 323 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = (yyvsp[0]); }
#line 2091 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 92:
#line 327 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = NULL; }
#line 2097 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 93:
#line 329 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = &pars_update_token; }
#line 2103 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 94:
#line 333 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = NULL; }
#line 2109 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 95:
#line 335 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = &pars_share_token; }
#line 2115 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 96:
#line 339 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = &pars_asc_token; }
#line 2121 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 97:
#line 340 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = &pars_asc_token; }
#line 2127 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 98:
#line 341 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = &pars_desc_token; }
#line 2133 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 99:
#line 345 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = NULL; }
#line 2139 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 100:
#line 347 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_order_by(
					static_cast<sym_node_t*>((yyvsp[-1])),
					static_cast<pars_res_word_t*>((yyvsp[0]))); }
#line 2147 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 101:
#line 358 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_select_statement(
					static_cast<sel_node_t*>((yyvsp[-6])),
					static_cast<sym_node_t*>((yyvsp[-4])),
					static_cast<que_node_t*>((yyvsp[-3])),
					static_cast<pars_res_word_t*>((yyvsp[-2])),
					static_cast<pars_res_word_t*>((yyvsp[-1])),
					static_cast<order_node_t*>((yyvsp[0]))); }
#line 2159 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 102:
#line 369 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = (yyvsp[0]); }
#line 2165 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 103:
#line 374 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_insert_statement(
					static_cast<sym_node_t*>((yyvsp[-4])), (yyvsp[-1]), NULL); }
#line 2172 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 104:
#line 377 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_insert_statement(
					static_cast<sym_node_t*>((yyvsp[-1])),
					NULL,
					static_cast<sel_node_t*>((yyvsp[0]))); }
#line 2181 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 105:
#line 384 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_column_assignment(
					static_cast<sym_node_t*>((yyvsp[-2])),
					static_cast<que_node_t*>((yyvsp[0]))); }
#line 2189 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 106:
#line 390 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = que_node_list_add_last(NULL, (yyvsp[0])); }
#line 2195 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 107:
#line 392 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = que_node_list_add_last((yyvsp[-2]), (yyvsp[0])); }
#line 2201 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 108:
#line 398 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = (yyvsp[0]); }
#line 2207 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 109:
#line 404 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_update_statement_start(
					FALSE,
					static_cast<sym_node_t*>((yyvsp[-2])),
					static_cast<col_assign_node_t*>((yyvsp[0]))); }
#line 2216 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 110:
#line 412 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_update_statement(
					static_cast<upd_node_t*>((yyvsp[-1])),
					NULL,
					static_cast<que_node_t*>((yyvsp[0]))); }
#line 2225 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 111:
#line 420 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_update_statement(
					static_cast<upd_node_t*>((yyvsp[-1])),
					static_cast<sym_node_t*>((yyvsp[0])),
					NULL); }
#line 2234 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 112:
#line 428 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_update_statement_start(
					TRUE,
					static_cast<sym_node_t*>((yyvsp[0])), NULL); }
#line 2242 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 113:
#line 435 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_update_statement(
					static_cast<upd_node_t*>((yyvsp[-1])),
					NULL,
					static_cast<que_node_t*>((yyvsp[0]))); }
#line 2251 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 114:
#line 443 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_update_statement(
					static_cast<upd_node_t*>((yyvsp[-1])),
					static_cast<sym_node_t*>((yyvsp[0])),
					NULL); }
#line 2260 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 115:
#line 451 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_row_printf_statement(
					static_cast<sel_node_t*>((yyvsp[0]))); }
#line 2267 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 116:
#line 457 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_assignment_statement(
					static_cast<sym_node_t*>((yyvsp[-2])),
					static_cast<que_node_t*>((yyvsp[0]))); }
#line 2275 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 117:
#line 465 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_elsif_element((yyvsp[-2]), (yyvsp[0])); }
#line 2281 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 118:
#line 469 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = que_node_list_add_last(NULL, (yyvsp[0])); }
#line 2287 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 119:
#line 471 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = que_node_list_add_last((yyvsp[-1]), (yyvsp[0])); }
#line 2293 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 120:
#line 475 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = NULL; }
#line 2299 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 121:
#line 477 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = (yyvsp[0]); }
#line 2305 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 122:
#line 478 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = (yyvsp[0]); }
#line 2311 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 123:
#line 485 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_if_statement((yyvsp[-5]), (yyvsp[-3]), (yyvsp[-2])); }
#line 2317 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 124:
#line 491 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_while_statement((yyvsp[-4]), (yyvsp[-2])); }
#line 2323 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 125:
#line 499 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_for_statement(
					static_cast<sym_node_t*>((yyvsp[-8])),
					(yyvsp[-6]), (yyvsp[-4]), (yyvsp[-2])); }
#line 2331 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 126:
#line 505 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_exit_statement(); }
#line 2337 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 127:
#line 509 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_return_statement(); }
#line 2343 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 128:
#line 514 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_open_statement(
						ROW_SEL_OPEN_CURSOR,
						static_cast<sym_node_t*>((yyvsp[0]))); }
#line 2351 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 129:
#line 521 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_open_statement(
						ROW_SEL_CLOSE_CURSOR,
						static_cast<sym_node_t*>((yyvsp[0]))); }
#line 2359 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 130:
#line 528 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_fetch_statement(
					static_cast<sym_node_t*>((yyvsp[-2])),
					static_cast<sym_node_t*>((yyvsp[0])), NULL); }
#line 2367 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 131:
#line 532 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_fetch_statement(
					static_cast<sym_node_t*>((yyvsp[-2])),
					NULL,
					static_cast<sym_node_t*>((yyvsp[0]))); }
#line 2376 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 132:
#line 540 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_column_def(
					static_cast<sym_node_t*>((yyvsp[-4])),
					static_cast<pars_res_word_t*>((yyvsp[-3])),
					static_cast<sym_node_t*>((yyvsp[-2])),
					(yyvsp[-1]), (yyvsp[0])); }
#line 2386 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 133:
#line 548 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = que_node_list_add_last(NULL, (yyvsp[0])); }
#line 2392 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 134:
#line 550 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = que_node_list_add_last((yyvsp[-2]), (yyvsp[0])); }
#line 2398 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 135:
#line 554 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = NULL; }
#line 2404 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 136:
#line 556 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = (yyvsp[-1]); }
#line 2410 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 137:
#line 560 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = NULL; }
#line 2416 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 138:
#line 562 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = &pars_int_token;
					/* pass any non-NULL pointer */ }
#line 2423 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 139:
#line 567 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = NULL; }
#line 2429 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 140:
#line 569 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = &pars_int_token;
					/* pass any non-NULL pointer */ }
#line 2436 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 141:
#line 574 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = NULL; }
#line 2442 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 142:
#line 575 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = &pars_int_token;
					/* pass any non-NULL pointer */ }
#line 2449 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 143:
#line 580 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = NULL; }
#line 2455 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 144:
#line 582 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = (yyvsp[0]); }
#line 2461 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 145:
#line 589 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_create_table(
					static_cast<sym_node_t*>((yyvsp[-5])),
					static_cast<sym_node_t*>((yyvsp[-3])),
					static_cast<sym_node_t*>((yyvsp[-1])),
					static_cast<sym_node_t*>((yyvsp[0]))); }
#line 2471 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 146:
#line 597 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = que_node_list_add_last(NULL, (yyvsp[0])); }
#line 2477 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 147:
#line 599 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = que_node_list_add_last((yyvsp[-2]), (yyvsp[0])); }
#line 2483 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 148:
#line 603 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = NULL; }
#line 2489 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 149:
#line 604 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = &pars_unique_token; }
#line 2495 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 150:
#line 608 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = NULL; }
#line 2501 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 151:
#line 609 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = &pars_clustered_token; }
#line 2507 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 152:
#line 618 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_create_index(
					static_cast<pars_res_word_t*>((yyvsp[-8])),
					static_cast<pars_res_word_t*>((yyvsp[-7])),
					static_cast<sym_node_t*>((yyvsp[-5])),
					static_cast<sym_node_t*>((yyvsp[-3])),
					static_cast<sym_node_t*>((yyvsp[-1]))); }
#line 2518 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 153:
#line 627 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = (yyvsp[0]); }
#line 2524 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 154:
#line 628 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = (yyvsp[0]); }
#line 2530 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 155:
#line 633 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_commit_statement(); }
#line 2536 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 156:
#line 638 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_rollback_statement(); }
#line 2542 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 157:
#line 642 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = &pars_int_token; }
#line 2548 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 158:
#line 643 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = &pars_bigint_token; }
#line 2554 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 159:
#line 644 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = &pars_char_token; }
#line 2560 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 160:
#line 645 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = &pars_binary_token; }
#line 2566 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 161:
#line 646 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = &pars_blob_token; }
#line 2572 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 162:
#line 651 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_parameter_declaration(
					static_cast<sym_node_t*>((yyvsp[-2])),
					PARS_INPUT,
					static_cast<pars_res_word_t*>((yyvsp[0]))); }
#line 2581 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 163:
#line 656 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_parameter_declaration(
					static_cast<sym_node_t*>((yyvsp[-2])),
					PARS_OUTPUT,
					static_cast<pars_res_word_t*>((yyvsp[0]))); }
#line 2590 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 164:
#line 663 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = NULL; }
#line 2596 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 165:
#line 664 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = que_node_list_add_last(NULL, (yyvsp[0])); }
#line 2602 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 166:
#line 666 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = que_node_list_add_last((yyvsp[-2]), (yyvsp[0])); }
#line 2608 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 167:
#line 671 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_variable_declaration(
					static_cast<sym_node_t*>((yyvsp[-2])),
					static_cast<pars_res_word_t*>((yyvsp[-1]))); }
#line 2616 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 171:
#line 685 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_cursor_declaration(
					static_cast<sym_node_t*>((yyvsp[-3])),
					static_cast<sel_node_t*>((yyvsp[-1]))); }
#line 2624 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 172:
#line 692 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_function_declaration(
					static_cast<sym_node_t*>((yyvsp[-1]))); }
#line 2631 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 178:
#line 714 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_procedure_definition(
					static_cast<sym_node_t*>((yyvsp[-9])),
					static_cast<sym_node_t*>((yyvsp[-7])),
					(yyvsp[-1])); }
#line 2640 "pars0grm.cc" /* yacc.c:1646  */
    break;


#line 2644 "pars0grm.cc" /* yacc.c:1646  */
      default: break;
    }
  /* User semantic actions sometimes alter yychar, and that requires
     that yytoken be updated with the new translation.  We take the
     approach of translating immediately before every use of yytoken.
     One alternative is translating here after every semantic action,
     but that translation would be missed if the semantic action invokes
     YYABORT, YYACCEPT, or YYERROR immediately after altering yychar or
     if it invokes YYBACKUP.  In the case of YYABORT or YYACCEPT, an
     incorrect destructor might then be invoked immediately.  In the
     case of YYERROR or YYBACKUP, subsequent parser actions might lead
     to an incorrect destructor call or verbose syntax error message
     before the lookahead is translated.  */
  YY_SYMBOL_PRINT ("-> $$ =", yyr1[yyn], &yyval, &yyloc);

  YYPOPSTACK (yylen);
  yylen = 0;
  YY_STACK_PRINT (yyss, yyssp);

  *++yyvsp = yyval;

  /* Now 'shift' the result of the reduction.  Determine what state
     that goes to, based on the state we popped back to and the rule
     number reduced by.  */

  yyn = yyr1[yyn];

  yystate = yypgoto[yyn - YYNTOKENS] + *yyssp;
  if (0 <= yystate && yystate <= YYLAST && yycheck[yystate] == *yyssp)
    yystate = yytable[yystate];
  else
    yystate = yydefgoto[yyn - YYNTOKENS];

  goto yynewstate;


/*--------------------------------------.
| yyerrlab -- here on detecting error.  |
`--------------------------------------*/
yyerrlab:
  /* Make sure we have latest lookahead translation.  See comments at
     user semantic actions for why this is necessary.  */
  yytoken = yychar == YYEMPTY ? YYEMPTY : YYTRANSLATE (yychar);

  /* If not already recovering from an error, report this error.  */
  if (!yyerrstatus)
    {
      ++yynerrs;
#if ! YYERROR_VERBOSE
      yyerror (YY_("syntax error"));
#else
# define YYSYNTAX_ERROR yysyntax_error (&yymsg_alloc, &yymsg, \
                                        yyssp, yytoken)
      {
        char const *yymsgp = YY_("syntax error");
        int yysyntax_error_status;
        yysyntax_error_status = YYSYNTAX_ERROR;
        if (yysyntax_error_status == 0)
          yymsgp = yymsg;
        else if (yysyntax_error_status == 1)
          {
            if (yymsg != yymsgbuf)
              YYSTACK_FREE (yymsg);
            yymsg = (char *) YYSTACK_ALLOC (yymsg_alloc);
            if (!yymsg)
              {
                yymsg = yymsgbuf;
                yymsg_alloc = sizeof yymsgbuf;
                yysyntax_error_status = 2;
              }
            else
              {
                yysyntax_error_status = YYSYNTAX_ERROR;
                yymsgp = yymsg;
              }
          }
        yyerror (yymsgp);
        if (yysyntax_error_status == 2)
          goto yyexhaustedlab;
      }
# undef YYSYNTAX_ERROR
#endif
    }



  if (yyerrstatus == 3)
    {
      /* If just tried and failed to reuse lookahead token after an
         error, discard it.  */

      if (yychar <= YYEOF)
        {
          /* Return failure if at end of input.  */
          if (yychar == YYEOF)
            YYABORT;
        }
      else
        {
          yydestruct ("Error: discarding",
                      yytoken, &yylval);
          yychar = YYEMPTY;
        }
    }

  /* Else will try to reuse lookahead token after shifting the error
     token.  */
  goto yyerrlab1;


/*---------------------------------------------------.
| yyerrorlab -- error raised explicitly by YYERROR.  |
`---------------------------------------------------*/
yyerrorlab:

  /* Pacify compilers like GCC when the user code never invokes
     YYERROR and the label yyerrorlab therefore never appears in user
     code.  */
  if (/*CONSTCOND*/ 0)
     goto yyerrorlab;

  /* Do not reclaim the symbols of the rule whose action triggered
     this YYERROR.  */
  YYPOPSTACK (yylen);
  yylen = 0;
  YY_STACK_PRINT (yyss, yyssp);
  yystate = *yyssp;
  goto yyerrlab1;


/*-------------------------------------------------------------.
| yyerrlab1 -- common code for both syntax error and YYERROR.  |
`-------------------------------------------------------------*/
yyerrlab1:
  yyerrstatus = 3;      /* Each real token shifted decrements this.  */

  for (;;)
    {
      yyn = yypact[yystate];
      if (!yypact_value_is_default (yyn))
        {
          yyn += YYTERROR;
          if (0 <= yyn && yyn <= YYLAST && yycheck[yyn] == YYTERROR)
            {
              yyn = yytable[yyn];
              if (0 < yyn)
                break;
            }
        }

      /* Pop the current state because it cannot handle the error token.  */
      if (yyssp == yyss)
        YYABORT;


      yydestruct ("Error: popping",
                  yystos[yystate], yyvsp);
      YYPOPSTACK (1);
      yystate = *yyssp;
      YY_STACK_PRINT (yyss, yyssp);
    }

  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  *++yyvsp = yylval;
  YY_IGNORE_MAYBE_UNINITIALIZED_END


  /* Shift the error token.  */
  YY_SYMBOL_PRINT ("Shifting", yystos[yyn], yyvsp, yylsp);

  yystate = yyn;
  goto yynewstate;


/*-------------------------------------.
| yyacceptlab -- YYACCEPT comes here.  |
`-------------------------------------*/
yyacceptlab:
  yyresult = 0;
  goto yyreturn;

/*-----------------------------------.
| yyabortlab -- YYABORT comes here.  |
`-----------------------------------*/
yyabortlab:
  yyresult = 1;
  goto yyreturn;

#if !defined yyoverflow || YYERROR_VERBOSE
/*-------------------------------------------------.
| yyexhaustedlab -- memory exhaustion comes here.  |
`-------------------------------------------------*/
yyexhaustedlab:
  yyerror (YY_("memory exhausted"));
  yyresult = 2;
  /* Fall through.  */
#endif

yyreturn:
  if (yychar != YYEMPTY)
    {
      /* Make sure we have latest lookahead translation.  See comments at
         user semantic actions for why this is necessary.  */
      yytoken = YYTRANSLATE (yychar);
      yydestruct ("Cleanup: discarding lookahead",
                  yytoken, &yylval);
    }
  /* Do not reclaim the symbols of the rule whose action triggered
     this YYABORT or YYACCEPT.  */
  YYPOPSTACK (yylen);
  YY_STACK_PRINT (yyss, yyssp);
  while (yyssp != yyss)
    {
      yydestruct ("Cleanup: popping",
                  yystos[*yyssp], yyvsp);
      YYPOPSTACK (1);
    }
#ifndef yyoverflow
  if (yyss != yyssa)
    YYSTACK_FREE (yyss);
#endif
#if YYERROR_VERBOSE
  if (yymsg != yymsgbuf)
    YYSTACK_FREE (yymsg);
#endif
  return yyresult;
}
#line 720 "pars0grm.y" /* yacc.c:1906  */

