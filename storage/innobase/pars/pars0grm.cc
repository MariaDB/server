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
    PARS_FIXBINARY_LIT = 261,
    PARS_BLOB_LIT = 262,
    PARS_NULL_LIT = 263,
    PARS_ID_TOKEN = 264,
    PARS_AND_TOKEN = 265,
    PARS_OR_TOKEN = 266,
    PARS_NOT_TOKEN = 267,
    PARS_GE_TOKEN = 268,
    PARS_LE_TOKEN = 269,
    PARS_NE_TOKEN = 270,
    PARS_PROCEDURE_TOKEN = 271,
    PARS_IN_TOKEN = 272,
    PARS_OUT_TOKEN = 273,
    PARS_BINARY_TOKEN = 274,
    PARS_BLOB_TOKEN = 275,
    PARS_INT_TOKEN = 276,
    PARS_INTEGER_TOKEN = 277,
    PARS_FLOAT_TOKEN = 278,
    PARS_CHAR_TOKEN = 279,
    PARS_IS_TOKEN = 280,
    PARS_BEGIN_TOKEN = 281,
    PARS_END_TOKEN = 282,
    PARS_IF_TOKEN = 283,
    PARS_THEN_TOKEN = 284,
    PARS_ELSE_TOKEN = 285,
    PARS_ELSIF_TOKEN = 286,
    PARS_LOOP_TOKEN = 287,
    PARS_WHILE_TOKEN = 288,
    PARS_RETURN_TOKEN = 289,
    PARS_SELECT_TOKEN = 290,
    PARS_SUM_TOKEN = 291,
    PARS_COUNT_TOKEN = 292,
    PARS_DISTINCT_TOKEN = 293,
    PARS_FROM_TOKEN = 294,
    PARS_WHERE_TOKEN = 295,
    PARS_FOR_TOKEN = 296,
    PARS_DDOT_TOKEN = 297,
    PARS_READ_TOKEN = 298,
    PARS_ORDER_TOKEN = 299,
    PARS_BY_TOKEN = 300,
    PARS_ASC_TOKEN = 301,
    PARS_DESC_TOKEN = 302,
    PARS_INSERT_TOKEN = 303,
    PARS_INTO_TOKEN = 304,
    PARS_VALUES_TOKEN = 305,
    PARS_UPDATE_TOKEN = 306,
    PARS_SET_TOKEN = 307,
    PARS_DELETE_TOKEN = 308,
    PARS_CURRENT_TOKEN = 309,
    PARS_OF_TOKEN = 310,
    PARS_CREATE_TOKEN = 311,
    PARS_TABLE_TOKEN = 312,
    PARS_INDEX_TOKEN = 313,
    PARS_UNIQUE_TOKEN = 314,
    PARS_CLUSTERED_TOKEN = 315,
    PARS_ON_TOKEN = 316,
    PARS_ASSIGN_TOKEN = 317,
    PARS_DECLARE_TOKEN = 318,
    PARS_CURSOR_TOKEN = 319,
    PARS_SQL_TOKEN = 320,
    PARS_OPEN_TOKEN = 321,
    PARS_FETCH_TOKEN = 322,
    PARS_CLOSE_TOKEN = 323,
    PARS_NOTFOUND_TOKEN = 324,
    PARS_TO_CHAR_TOKEN = 325,
    PARS_TO_NUMBER_TOKEN = 326,
    PARS_TO_BINARY_TOKEN = 327,
    PARS_BINARY_TO_NUMBER_TOKEN = 328,
    PARS_SUBSTR_TOKEN = 329,
    PARS_REPLSTR_TOKEN = 330,
    PARS_CONCAT_TOKEN = 331,
    PARS_INSTR_TOKEN = 332,
    PARS_LENGTH_TOKEN = 333,
    PARS_SYSDATE_TOKEN = 334,
    PARS_PRINTF_TOKEN = 335,
    PARS_ASSERT_TOKEN = 336,
    PARS_RND_TOKEN = 337,
    PARS_RND_STR_TOKEN = 338,
    PARS_ROW_PRINTF_TOKEN = 339,
    PARS_COMMIT_TOKEN = 340,
    PARS_ROLLBACK_TOKEN = 341,
    PARS_WORK_TOKEN = 342,
    PARS_UNSIGNED_TOKEN = 343,
    PARS_EXIT_TOKEN = 344,
    PARS_FUNCTION_TOKEN = 345,
    PARS_LOCK_TOKEN = 346,
    PARS_SHARE_TOKEN = 347,
    PARS_MODE_TOKEN = 348,
    PARS_LIKE_TOKEN = 349,
    PARS_LIKE_TOKEN_EXACT = 350,
    PARS_LIKE_TOKEN_PREFIX = 351,
    PARS_LIKE_TOKEN_SUFFIX = 352,
    PARS_LIKE_TOKEN_SUBSTR = 353,
    PARS_TABLE_NAME_TOKEN = 354,
    PARS_COMPACT_TOKEN = 355,
    PARS_BLOCK_SIZE_TOKEN = 356,
    PARS_BIGINT_TOKEN = 357,
    NEG = 358
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

#line 240 "pars0grm.cc" /* yacc.c:358  */

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
#define YYLAST   824

/* YYNTOKENS -- Number of terminals.  */
#define YYNTOKENS  119
/* YYNNTS -- Number of nonterminals.  */
#define YYNNTS  72
/* YYNRULES -- Number of rules.  */
#define YYNRULES  181
/* YYNSTATES -- Number of states.  */
#define YYNSTATES  348

/* YYTRANSLATE[YYX] -- Symbol number corresponding to YYX as returned
   by yylex, with out-of-bounds checking.  */
#define YYUNDEFTOK  2
#define YYMAXUTOK   358

#define YYTRANSLATE(YYX)                                                \
  ((unsigned int) (YYX) <= YYMAXUTOK ? yytranslate[YYX] : YYUNDEFTOK)

/* YYTRANSLATE[TOKEN-NUM] -- Symbol number corresponding to TOKEN-NUM
   as returned by yylex, without out-of-bounds checking.  */
static const yytype_uint8 yytranslate[] =
{
       0,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,   111,     2,     2,
     113,   114,   108,   107,   116,   106,     2,   109,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,   112,
     104,   103,   105,   115,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,   117,     2,   118,     2,     2,     2,     2,
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
      95,    96,    97,    98,    99,   100,   101,   102,   110
};

#if YYDEBUG
  /* YYRLINE[YYN] -- Source line where rule number YYN was defined.  */
static const yytype_uint16 yyrline[] =
{
       0,   163,   163,   166,   167,   168,   169,   170,   171,   172,
     173,   174,   175,   176,   177,   178,   179,   180,   181,   182,
     183,   184,   185,   186,   187,   191,   192,   197,   198,   200,
     201,   202,   203,   204,   205,   206,   207,   208,   209,   210,
     211,   212,   213,   214,   216,   217,   218,   219,   220,   221,
     222,   223,   224,   226,   231,   232,   233,   234,   236,   237,
     238,   239,   240,   241,   242,   245,   247,   248,   252,   258,
     263,   264,   265,   269,   273,   274,   279,   280,   281,   286,
     287,   288,   292,   293,   298,   304,   311,   312,   313,   318,
     320,   323,   327,   328,   332,   333,   338,   339,   344,   345,
     346,   350,   351,   358,   373,   378,   381,   389,   395,   396,
     401,   407,   416,   424,   432,   439,   447,   455,   461,   468,
     474,   475,   480,   481,   483,   487,   494,   500,   510,   514,
     518,   525,   532,   536,   544,   553,   554,   559,   560,   565,
     566,   572,   573,   579,   580,   585,   586,   591,   602,   603,
     608,   609,   613,   614,   618,   632,   633,   637,   642,   647,
     648,   649,   650,   651,   652,   656,   661,   669,   670,   671,
     676,   682,   684,   685,   689,   697,   703,   704,   707,   709,
     710,   714
};
#endif

#if YYDEBUG || YYERROR_VERBOSE || 0
/* YYTNAME[SYMBOL-NUM] -- String name of the symbol SYMBOL-NUM.
   First, the terminals, then, starting at YYNTOKENS, nonterminals.  */
static const char *const yytname[] =
{
  "$end", "error", "$undefined", "PARS_INT_LIT", "PARS_FLOAT_LIT",
  "PARS_STR_LIT", "PARS_FIXBINARY_LIT", "PARS_BLOB_LIT", "PARS_NULL_LIT",
  "PARS_ID_TOKEN", "PARS_AND_TOKEN", "PARS_OR_TOKEN", "PARS_NOT_TOKEN",
  "PARS_GE_TOKEN", "PARS_LE_TOKEN", "PARS_NE_TOKEN",
  "PARS_PROCEDURE_TOKEN", "PARS_IN_TOKEN", "PARS_OUT_TOKEN",
  "PARS_BINARY_TOKEN", "PARS_BLOB_TOKEN", "PARS_INT_TOKEN",
  "PARS_INTEGER_TOKEN", "PARS_FLOAT_TOKEN", "PARS_CHAR_TOKEN",
  "PARS_IS_TOKEN", "PARS_BEGIN_TOKEN", "PARS_END_TOKEN", "PARS_IF_TOKEN",
  "PARS_THEN_TOKEN", "PARS_ELSE_TOKEN", "PARS_ELSIF_TOKEN",
  "PARS_LOOP_TOKEN", "PARS_WHILE_TOKEN", "PARS_RETURN_TOKEN",
  "PARS_SELECT_TOKEN", "PARS_SUM_TOKEN", "PARS_COUNT_TOKEN",
  "PARS_DISTINCT_TOKEN", "PARS_FROM_TOKEN", "PARS_WHERE_TOKEN",
  "PARS_FOR_TOKEN", "PARS_DDOT_TOKEN", "PARS_READ_TOKEN",
  "PARS_ORDER_TOKEN", "PARS_BY_TOKEN", "PARS_ASC_TOKEN", "PARS_DESC_TOKEN",
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
     355,   356,   357,    61,    60,    62,    45,    43,    42,    47,
     358,    37,    59,    40,    41,    63,    44,   123,   125
};
# endif

#define YYPACT_NINF -179

#define yypact_value_is_default(Yystate) \
  (!!((Yystate) == (-179)))

#define YYTABLE_NINF -1

#define yytable_value_is_error(Yytable_value) \
  0

  /* YYPACT[STATE-NUM] -- Index in YYTABLE of the portion describing
     STATE-NUM.  */
static const yytype_int16 yypact[] =
{
      35,    50,    72,   -37,   -36,  -179,  -179,    67,    49,  -179,
     -78,    13,    13,    53,    67,  -179,  -179,  -179,  -179,  -179,
    -179,  -179,  -179,    76,  -179,    13,  -179,     7,   -31,   -34,
    -179,  -179,  -179,  -179,   -14,  -179,    77,    83,   583,  -179,
      78,   -10,    42,   284,   284,  -179,    17,    96,    58,     2,
      69,   -16,   105,   107,   108,  -179,  -179,  -179,    84,    31,
      37,  -179,   113,  -179,   403,  -179,    14,    15,    19,    -4,
      21,    89,    23,    24,    89,    25,    26,    32,    33,    44,
      45,    47,    51,    52,    54,    55,    56,    57,    60,    62,
      63,    84,  -179,   284,  -179,  -179,  -179,  -179,  -179,  -179,
      43,   284,    59,  -179,  -179,  -179,  -179,  -179,  -179,  -179,
    -179,  -179,  -179,  -179,   284,   284,   571,    70,   612,    73,
      74,  -179,   699,  -179,   -45,    95,   145,     2,  -179,  -179,
     136,     2,     2,  -179,   129,  -179,   116,  -179,  -179,  -179,
    -179,    79,  -179,  -179,  -179,   284,  -179,    80,  -179,  -179,
     194,  -179,  -179,  -179,  -179,  -179,  -179,  -179,  -179,  -179,
    -179,  -179,  -179,  -179,  -179,  -179,  -179,  -179,  -179,  -179,
    -179,  -179,  -179,    82,   699,   121,   715,   122,     3,   210,
     284,   284,   284,   284,   284,   583,   190,   284,   284,   284,
     284,   284,   284,   284,   284,   583,   284,   -29,   187,   173,
       2,   284,  -179,   195,  -179,    92,  -179,   149,   199,    97,
     699,   -72,   284,   156,   699,  -179,  -179,  -179,  -179,   715,
     715,     4,     4,   699,   343,  -179,     4,     4,     4,    12,
      12,     3,     3,   -69,   463,   226,   204,   101,  -179,   100,
    -179,   -32,  -179,   642,   114,  -179,   103,   217,   218,   117,
    -179,   100,  -179,   -66,  -179,   284,   -59,   220,   583,   284,
    -179,   202,   207,  -179,   203,  -179,   128,  -179,   244,   284,
       2,   216,   284,   284,   195,    13,  -179,   -52,   200,   146,
     144,   154,   699,  -179,  -179,   583,   672,  -179,   246,  -179,
    -179,  -179,  -179,   224,   189,   679,   699,  -179,   165,   181,
     217,     2,  -179,  -179,  -179,   583,  -179,  -179,   265,   239,
     583,   281,   197,  -179,   193,  -179,   182,   583,   205,   253,
    -179,   523,   185,  -179,   289,   206,  -179,   293,   212,   294,
     274,  -179,   300,  -179,   307,  -179,   -51,  -179,    22,  -179,
    -179,  -179,  -179,   302,  -179,  -179,  -179,  -179
};

  /* YYDEFACT[STATE-NUM] -- Default reduction number in state STATE-NUM.
     Performed when YYTABLE does not specify something else to do.  Zero
     means the default is an error.  */
static const yytype_uint8 yydefact[] =
{
       0,     0,     0,     0,     0,     1,     2,   167,     0,   168,
       0,     0,     0,     0,     0,   163,   164,   159,   160,   162,
     161,   165,   166,   171,   169,     0,   172,   178,     0,     0,
     173,   176,   177,   179,     0,   170,     0,     0,     0,   180,
       0,     0,     0,     0,     0,   129,    86,     0,     0,     0,
       0,   150,     0,     0,     0,    70,    71,    72,     0,     0,
       0,   128,     0,    25,     0,     3,     0,     0,     0,     0,
       0,    92,     0,     0,    92,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,   175,     0,    29,    30,    31,    32,    33,    34,
      27,     0,    35,    54,    55,    56,    57,    58,    59,    60,
      61,    62,    63,    64,     0,     0,     0,     0,     0,     0,
       0,    89,    82,    87,    91,     0,     0,     0,   155,   156,
       0,     0,     0,   151,   152,   130,     0,   131,   117,   157,
     158,     0,   181,    26,     4,    79,    11,     0,   106,    12,
       0,   112,   113,    16,    17,   115,   116,    14,    15,    13,
      10,     8,     5,     6,     7,     9,    18,    20,    19,    23,
      24,    21,    22,     0,   118,     0,    51,     0,    40,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,    79,     0,     0,     0,    76,     0,
       0,     0,   104,     0,   114,     0,   153,     0,    76,    65,
      80,     0,    79,     0,    93,   174,    52,    53,    41,    49,
      50,    46,    47,    48,   122,    43,    42,    44,    45,    37,
      36,    38,    39,     0,     0,     0,     0,     0,    77,    90,
      88,    92,    74,     0,     0,   108,   111,     0,     0,    77,
     133,   132,    66,     0,    69,     0,     0,     0,     0,     0,
     120,   124,     0,    28,     0,    85,     0,    83,     0,     0,
       0,    94,     0,     0,     0,     0,   135,     0,     0,     0,
       0,     0,    81,   105,   110,   123,     0,   121,     0,   126,
      84,    78,    75,     0,    96,     0,   107,   109,   137,   143,
       0,     0,    73,    68,    67,     0,   125,    95,     0,   101,
       0,     0,   139,   144,   145,   136,     0,   119,     0,     0,
     103,     0,     0,   140,   141,     0,   147,     0,     0,     0,
       0,   138,     0,   134,     0,   148,     0,    97,    98,   127,
     142,   146,   154,     0,    99,   100,   102,   149
};

  /* YYPGOTO[NTERM-NUM].  */
static const yytype_int16 yypgoto[] =
{
    -179,  -179,   -63,  -178,   -41,  -179,  -179,  -179,  -179,  -179,
    -179,  -179,   104,  -154,   123,  -179,  -179,   -68,  -179,  -179,
    -179,  -179,   -30,  -179,  -179,    64,  -179,   247,  -179,  -179,
    -179,  -179,  -179,  -179,  -179,  -179,    65,  -179,  -179,  -179,
    -179,  -179,  -179,  -179,  -179,  -179,  -179,    27,  -179,  -179,
    -179,  -179,  -179,  -179,  -179,  -179,  -179,  -179,  -179,  -117,
    -179,  -179,   -12,   309,  -179,   298,  -179,  -179,  -179,   303,
    -179,  -179
};

  /* YYDEFGOTO[NTERM-NUM].  */
static const yytype_int16 yydefgoto[] =
{
      -1,     2,    63,    64,   210,   117,   253,    65,    66,    67,
     250,   241,   239,   211,   123,   124,   125,   151,   294,   309,
     346,   320,    68,    69,    70,   245,   246,   152,    71,    72,
      73,    74,    75,    76,    77,    78,   260,   261,   262,    79,
      80,    81,    82,    83,    84,    85,    86,   276,   277,   312,
     324,   333,   314,   326,    87,   336,   134,   207,    88,   130,
      89,    90,    21,     9,    10,    26,    27,    31,    32,    33,
      34,     3
};

  /* YYTABLE[YYPACT[STATE-NUM]] -- What to do in state STATE-NUM.  If
     positive, shift that token.  If negative, reduce the rule whose
     number is the opposite.  If YYTABLE_NINF, syntax error.  */
static const yytype_uint16 yytable[] =
{
      22,   143,   116,   118,   198,   122,   155,   224,   269,   236,
     202,   128,    38,    28,   204,   205,    25,   234,   184,   184,
      94,    95,    96,    97,    98,    99,   100,   184,   138,   101,
      36,    46,    15,    16,    17,    18,    13,    19,    14,   148,
     233,   132,   254,   133,   255,   263,   147,   255,   280,    29,
     281,     1,   174,   119,   120,   283,    37,   255,   256,     4,
     176,   173,   299,   342,   300,   343,    11,    12,   344,   345,
      29,   199,     5,   178,   179,     6,     8,     7,    23,   237,
     285,    35,   102,   242,   270,    25,    40,   103,   104,   105,
     106,   107,    41,   108,   109,   110,   111,   186,   186,   112,
     113,   129,    92,    91,    93,   126,   186,   127,   131,   214,
     190,   191,   192,   193,   135,    20,   136,   137,   139,    46,
     192,   193,   141,   114,   140,   121,   144,   317,   145,   150,
     115,   146,   321,   149,   200,   153,   154,   157,   158,   219,
     220,   221,   222,   223,   159,   160,   226,   227,   228,   229,
     230,   231,   232,   292,   175,   235,   161,   162,   122,   163,
     243,   143,   201,   164,   165,   208,   166,   167,   168,   169,
     177,   143,   170,   271,   171,   172,    94,    95,    96,    97,
      98,    99,   100,   194,   316,   101,   196,   197,   203,   206,
     216,   217,   209,   212,   215,   225,   238,    94,    95,    96,
      97,    98,    99,   100,   244,   247,   101,   248,   249,   119,
     120,   257,   252,   266,   282,   267,   268,   273,   286,   274,
     180,   181,   143,   182,   183,   184,   275,   278,   214,   284,
     279,   295,   296,   259,   288,   289,   180,   181,   102,   182,
     183,   184,   290,   103,   104,   105,   106,   107,   213,   108,
     109,   110,   111,   291,   143,   112,   113,   293,   143,   102,
     302,   301,   303,   298,   103,   104,   105,   106,   107,   304,
     108,   109,   110,   111,   306,   307,   112,   113,   311,   114,
     308,   313,   318,   319,   322,   323,   115,    94,    95,    96,
      97,    98,    99,   100,   325,   327,   101,   328,   329,   331,
     114,   332,   335,   338,   186,   337,   339,   115,   340,   334,
     341,   347,   251,   187,   188,   189,   190,   191,   192,   193,
     186,   156,   240,    24,   218,    30,   287,   315,     0,   187,
     188,   189,   190,   191,   192,   193,     0,    39,   297,     0,
     265,     0,     0,     0,     0,     0,     0,     0,     0,   102,
       0,     0,    42,     0,   103,   104,   105,   106,   107,     0,
     108,   109,   110,   111,     0,     0,   112,   113,     0,     0,
       0,    43,     0,   258,   259,     0,    44,    45,    46,     0,
       0,     0,     0,     0,    47,     0,     0,     0,     0,     0,
     114,    48,     0,     0,    49,     0,    50,   115,     0,    51,
       0,     0,     0,     0,     0,     0,     0,     0,     0,    52,
      53,    54,    42,     0,     0,     0,     0,     0,    55,     0,
       0,     0,     0,    56,    57,     0,     0,    58,    59,    60,
     142,    43,    61,     0,     0,     0,    44,    45,    46,     0,
       0,     0,     0,     0,    47,     0,     0,     0,     0,     0,
       0,    48,     0,     0,    49,     0,    50,     0,     0,    51,
      62,     0,     0,     0,     0,     0,     0,     0,     0,    52,
      53,    54,    42,     0,     0,     0,     0,     0,    55,     0,
       0,     0,     0,    56,    57,     0,     0,    58,    59,    60,
     264,    43,    61,     0,     0,     0,    44,    45,    46,     0,
       0,     0,     0,     0,    47,     0,     0,     0,     0,     0,
       0,    48,     0,     0,    49,     0,    50,     0,     0,    51,
      62,     0,     0,     0,     0,     0,     0,     0,     0,    52,
      53,    54,    42,     0,     0,     0,     0,     0,    55,     0,
       0,     0,     0,    56,    57,     0,     0,    58,    59,    60,
     330,    43,    61,     0,     0,     0,    44,    45,    46,     0,
       0,     0,     0,     0,    47,     0,     0,     0,     0,     0,
       0,    48,     0,     0,    49,     0,    50,     0,     0,    51,
      62,   180,   181,     0,   182,   183,   184,     0,     0,    52,
      53,    54,    42,     0,     0,     0,     0,     0,    55,     0,
     185,     0,     0,    56,    57,     0,     0,    58,    59,    60,
       0,    43,    61,     0,     0,     0,    44,    45,    46,     0,
       0,     0,   180,   181,    47,   182,   183,   184,     0,     0,
       0,    48,     0,     0,    49,     0,    50,     0,     0,    51,
      62,     0,     0,     0,   195,     0,     0,     0,     0,    52,
      53,    54,   180,   181,     0,   182,   183,   184,    55,     0,
       0,     0,     0,    56,    57,   186,     0,    58,    59,    60,
       0,     0,    61,     0,   187,   188,   189,   190,   191,   192,
     193,     0,   180,   181,   272,   182,   183,   184,     0,   180,
     181,     0,   182,   183,   184,     0,     0,     0,     0,     0,
      62,   305,     0,     0,     0,     0,   186,     0,     0,   180,
     181,   310,   182,   183,   184,   187,   188,   189,   190,   191,
     192,   193,     0,     0,     0,     0,     0,     0,   182,   183,
     184,     0,     0,     0,     0,     0,   186,     0,     0,     0,
       0,     0,     0,     0,     0,   187,   188,   189,   190,   191,
     192,   193,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,   186,     0,     0,     0,
       0,     0,     0,   186,     0,   187,   188,   189,   190,   191,
     192,   193,   187,   188,   189,   190,   191,   192,   193,     0,
       0,     0,     0,   186,     0,     0,     0,     0,     0,     0,
       0,     0,   187,   188,   189,   190,   191,   192,   193,   186,
       0,     0,     0,     0,     0,     0,     0,     0,   187,   188,
     189,   190,   191,   192,   193
};

static const yytype_int16 yycheck[] =
{
      12,    64,    43,    44,    49,    46,    74,   185,    40,    38,
     127,     9,    26,    25,   131,   132,     9,   195,    15,    15,
       3,     4,     5,     6,     7,     8,     9,    15,    58,    12,
      64,    35,    19,    20,    21,    22,   114,    24,   116,    69,
     194,    57,   114,    59,   116,   114,    50,   116,   114,    63,
     116,    16,    93,    36,    37,   114,    90,   116,   212,     9,
     101,    91,   114,   114,   116,   116,    17,    18,    46,    47,
      63,   116,     0,   114,   115,   112,     9,   113,    25,   108,
     258,   112,    65,   200,   116,     9,     9,    70,    71,    72,
      73,    74,     9,    76,    77,    78,    79,    94,    94,    82,
      83,    99,   112,    25,    62,     9,    94,    49,    39,   150,
     106,   107,   108,   109,     9,   102,     9,     9,    87,    35,
     108,   109,     9,   106,    87,   108,   112,   305,   113,    40,
     113,   112,   310,   112,    39,   112,   112,   112,   112,   180,
     181,   182,   183,   184,   112,   112,   187,   188,   189,   190,
     191,   192,   193,   270,   111,   196,   112,   112,   199,   112,
     201,   224,    17,   112,   112,    49,   112,   112,   112,   112,
     111,   234,   112,   241,   112,   112,     3,     4,     5,     6,
       7,     8,     9,   113,   301,    12,   113,   113,    52,    60,
      69,    69,   113,   113,   112,     5,     9,     3,     4,     5,
       6,     7,     8,     9,     9,   113,    12,    58,     9,    36,
      37,    55,   115,     9,   255,   114,   116,   103,   259,   116,
      10,    11,   285,    13,    14,    15,     9,     9,   269,     9,
     113,   272,   273,    31,    27,    32,    10,    11,    65,    13,
      14,    15,   114,    70,    71,    72,    73,    74,    54,    76,
      77,    78,    79,     9,   317,    82,    83,    41,   321,    65,
     114,    61,   118,   275,    70,    71,    72,    73,    74,   115,
      76,    77,    78,    79,    28,    51,    82,    83,   113,   106,
      91,   100,    17,    44,     3,    88,   113,     3,     4,     5,
       6,     7,     8,     9,   101,   113,    12,    92,    45,   114,
     106,    12,     9,     9,    94,    93,    32,   113,     8,   103,
       3,     9,   208,   103,   104,   105,   106,   107,   108,   109,
      94,    74,   199,    14,   114,    27,   261,   300,    -1,   103,
     104,   105,   106,   107,   108,   109,    -1,    34,   274,    -1,
     114,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    65,
      -1,    -1,     9,    -1,    70,    71,    72,    73,    74,    -1,
      76,    77,    78,    79,    -1,    -1,    82,    83,    -1,    -1,
      -1,    28,    -1,    30,    31,    -1,    33,    34,    35,    -1,
      -1,    -1,    -1,    -1,    41,    -1,    -1,    -1,    -1,    -1,
     106,    48,    -1,    -1,    51,    -1,    53,   113,    -1,    56,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    66,
      67,    68,     9,    -1,    -1,    -1,    -1,    -1,    75,    -1,
      -1,    -1,    -1,    80,    81,    -1,    -1,    84,    85,    86,
      27,    28,    89,    -1,    -1,    -1,    33,    34,    35,    -1,
      -1,    -1,    -1,    -1,    41,    -1,    -1,    -1,    -1,    -1,
      -1,    48,    -1,    -1,    51,    -1,    53,    -1,    -1,    56,
     117,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    66,
      67,    68,     9,    -1,    -1,    -1,    -1,    -1,    75,    -1,
      -1,    -1,    -1,    80,    81,    -1,    -1,    84,    85,    86,
      27,    28,    89,    -1,    -1,    -1,    33,    34,    35,    -1,
      -1,    -1,    -1,    -1,    41,    -1,    -1,    -1,    -1,    -1,
      -1,    48,    -1,    -1,    51,    -1,    53,    -1,    -1,    56,
     117,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    66,
      67,    68,     9,    -1,    -1,    -1,    -1,    -1,    75,    -1,
      -1,    -1,    -1,    80,    81,    -1,    -1,    84,    85,    86,
      27,    28,    89,    -1,    -1,    -1,    33,    34,    35,    -1,
      -1,    -1,    -1,    -1,    41,    -1,    -1,    -1,    -1,    -1,
      -1,    48,    -1,    -1,    51,    -1,    53,    -1,    -1,    56,
     117,    10,    11,    -1,    13,    14,    15,    -1,    -1,    66,
      67,    68,     9,    -1,    -1,    -1,    -1,    -1,    75,    -1,
      29,    -1,    -1,    80,    81,    -1,    -1,    84,    85,    86,
      -1,    28,    89,    -1,    -1,    -1,    33,    34,    35,    -1,
      -1,    -1,    10,    11,    41,    13,    14,    15,    -1,    -1,
      -1,    48,    -1,    -1,    51,    -1,    53,    -1,    -1,    56,
     117,    -1,    -1,    -1,    32,    -1,    -1,    -1,    -1,    66,
      67,    68,    10,    11,    -1,    13,    14,    15,    75,    -1,
      -1,    -1,    -1,    80,    81,    94,    -1,    84,    85,    86,
      -1,    -1,    89,    -1,   103,   104,   105,   106,   107,   108,
     109,    -1,    10,    11,    42,    13,    14,    15,    -1,    10,
      11,    -1,    13,    14,    15,    -1,    -1,    -1,    -1,    -1,
     117,    29,    -1,    -1,    -1,    -1,    94,    -1,    -1,    10,
      11,    32,    13,    14,    15,   103,   104,   105,   106,   107,
     108,   109,    -1,    -1,    -1,    -1,    -1,    -1,    13,    14,
      15,    -1,    -1,    -1,    -1,    -1,    94,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,   103,   104,   105,   106,   107,
     108,   109,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    94,    -1,    -1,    -1,
      -1,    -1,    -1,    94,    -1,   103,   104,   105,   106,   107,
     108,   109,   103,   104,   105,   106,   107,   108,   109,    -1,
      -1,    -1,    -1,    94,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,   103,   104,   105,   106,   107,   108,   109,    94,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,   103,   104,
     105,   106,   107,   108,   109
};

  /* YYSTOS[STATE-NUM] -- The (internal number of the) accessing
     symbol of state STATE-NUM.  */
static const yytype_uint8 yystos[] =
{
       0,    16,   120,   190,     9,     0,   112,   113,     9,   182,
     183,    17,    18,   114,   116,    19,    20,    21,    22,    24,
     102,   181,   181,    25,   182,     9,   184,   185,   181,    63,
     184,   186,   187,   188,   189,   112,    64,    90,    26,   188,
       9,     9,     9,    28,    33,    34,    35,    41,    48,    51,
      53,    56,    66,    67,    68,    75,    80,    81,    84,    85,
      86,    89,   117,   121,   122,   126,   127,   128,   141,   142,
     143,   147,   148,   149,   150,   151,   152,   153,   154,   158,
     159,   160,   161,   162,   163,   164,   165,   173,   177,   179,
     180,    25,   112,    62,     3,     4,     5,     6,     7,     8,
       9,    12,    65,    70,    71,    72,    73,    74,    76,    77,
      78,    79,    82,    83,   106,   113,   123,   124,   123,    36,
      37,   108,   123,   133,   134,   135,     9,    49,     9,    99,
     178,    39,    57,    59,   175,     9,     9,     9,   141,    87,
      87,     9,    27,   121,   112,   113,   112,    50,   141,   112,
      40,   136,   146,   112,   112,   136,   146,   112,   112,   112,
     112,   112,   112,   112,   112,   112,   112,   112,   112,   112,
     112,   112,   112,   141,   123,   111,   123,   111,   123,   123,
      10,    11,    13,    14,    15,    29,    94,   103,   104,   105,
     106,   107,   108,   109,   113,    32,   113,   113,    49,   116,
      39,    17,   178,    52,   178,   178,    60,   176,    49,   113,
     123,   132,   113,    54,   123,   112,    69,    69,   114,   123,
     123,   123,   123,   123,   122,     5,   123,   123,   123,   123,
     123,   123,   123,   132,   122,   123,    38,   108,     9,   131,
     133,   130,   178,   123,     9,   144,   145,   113,    58,     9,
     129,   131,   115,   125,   114,   116,   132,    55,    30,    31,
     155,   156,   157,   114,    27,   114,     9,   114,   116,    40,
     116,   136,    42,   103,   116,     9,   166,   167,     9,   113,
     114,   116,   123,   114,     9,   122,   123,   155,    27,    32,
     114,     9,   178,    41,   137,   123,   123,   144,   181,   114,
     116,    61,   114,   118,   115,    29,    28,    51,    91,   138,
      32,   113,   168,   100,   171,   166,   178,   122,    17,    44,
     140,   122,     3,    88,   169,   101,   172,   113,    92,    45,
      27,   114,    12,   170,   103,     9,   174,    93,     9,    32,
       8,     3,   114,   116,    46,    47,   139,     9
};

  /* YYR1[YYN] -- Symbol number of symbol that rule YYN derives.  */
static const yytype_uint8 yyr1[] =
{
       0,   119,   120,   121,   121,   121,   121,   121,   121,   121,
     121,   121,   121,   121,   121,   121,   121,   121,   121,   121,
     121,   121,   121,   121,   121,   122,   122,   123,   123,   123,
     123,   123,   123,   123,   123,   123,   123,   123,   123,   123,
     123,   123,   123,   123,   123,   123,   123,   123,   123,   123,
     123,   123,   123,   123,   124,   124,   124,   124,   124,   124,
     124,   124,   124,   124,   124,   125,   125,   125,   126,   127,
     128,   128,   128,   129,   130,   130,   131,   131,   131,   132,
     132,   132,   133,   133,   133,   133,   134,   134,   134,   135,
     135,   135,   136,   136,   137,   137,   138,   138,   139,   139,
     139,   140,   140,   141,   142,   143,   143,   144,   145,   145,
     146,   147,   148,   149,   150,   151,   152,   153,   154,   155,
     156,   156,   157,   157,   157,   158,   159,   160,   161,   162,
     163,   164,   165,   165,   166,   167,   167,   168,   168,   169,
     169,   170,   170,   171,   171,   172,   172,   173,   174,   174,
     175,   175,   176,   176,   177,   178,   178,   179,   180,   181,
     181,   181,   181,   181,   181,   182,   182,   183,   183,   183,
     184,   185,   185,   185,   186,   187,   188,   188,   189,   189,
     189,   190
};

  /* YYR2[YYN] -- Number of symbols on the right hand side of rule YYN.  */
static const yytype_uint8 yyr2[] =
{
       0,     2,     2,     1,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     1,     2,     1,     4,     1,
       1,     1,     1,     1,     1,     1,     3,     3,     3,     3,
       2,     3,     3,     3,     3,     3,     3,     3,     3,     3,
       3,     2,     3,     3,     1,     1,     1,     1,     1,     1,
       1,     1,     1,     1,     1,     0,     1,     3,     6,     4,
       1,     1,     1,     3,     1,     3,     0,     1,     3,     0,
       1,     3,     1,     4,     5,     4,     0,     1,     3,     1,
       3,     1,     0,     2,     0,     2,     0,     4,     0,     1,
       1,     0,     4,     8,     3,     5,     2,     3,     1,     3,
       4,     4,     2,     2,     3,     2,     2,     2,     3,     4,
       1,     2,     0,     2,     1,     7,     6,    10,     1,     1,
       2,     2,     4,     4,     5,     1,     3,     0,     3,     0,
       1,     0,     2,     0,     1,     0,     3,     8,     1,     3,
       0,     1,     0,     1,    10,     1,     1,     2,     2,     1,
       1,     1,     1,     1,     1,     3,     3,     0,     1,     3,
       3,     0,     1,     2,     6,     4,     1,     1,     0,     1,
       2,    11
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
#line 191 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = que_node_list_add_last(NULL, (yyvsp[0])); }
#line 1716 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 26:
#line 193 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = que_node_list_add_last((yyvsp[-1]), (yyvsp[0])); }
#line 1722 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 27:
#line 197 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = (yyvsp[0]);}
#line 1728 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 28:
#line 199 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_func((yyvsp[-3]), (yyvsp[-1])); }
#line 1734 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 29:
#line 200 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = (yyvsp[0]);}
#line 1740 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 30:
#line 201 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = (yyvsp[0]);}
#line 1746 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 31:
#line 202 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = (yyvsp[0]);}
#line 1752 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 32:
#line 203 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = (yyvsp[0]);}
#line 1758 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 33:
#line 204 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = (yyvsp[0]);}
#line 1764 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 34:
#line 205 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = (yyvsp[0]);}
#line 1770 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 35:
#line 206 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = (yyvsp[0]);}
#line 1776 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 36:
#line 207 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_op('+', (yyvsp[-2]), (yyvsp[0])); }
#line 1782 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 37:
#line 208 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_op('-', (yyvsp[-2]), (yyvsp[0])); }
#line 1788 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 38:
#line 209 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_op('*', (yyvsp[-2]), (yyvsp[0])); }
#line 1794 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 39:
#line 210 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_op('/', (yyvsp[-2]), (yyvsp[0])); }
#line 1800 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 40:
#line 211 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_op('-', (yyvsp[0]), NULL); }
#line 1806 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 41:
#line 212 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = (yyvsp[-1]); }
#line 1812 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 42:
#line 213 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_op('=', (yyvsp[-2]), (yyvsp[0])); }
#line 1818 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 43:
#line 215 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_op(PARS_LIKE_TOKEN, (yyvsp[-2]), (yyvsp[0])); }
#line 1824 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 44:
#line 216 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_op('<', (yyvsp[-2]), (yyvsp[0])); }
#line 1830 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 45:
#line 217 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_op('>', (yyvsp[-2]), (yyvsp[0])); }
#line 1836 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 46:
#line 218 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_op(PARS_GE_TOKEN, (yyvsp[-2]), (yyvsp[0])); }
#line 1842 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 47:
#line 219 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_op(PARS_LE_TOKEN, (yyvsp[-2]), (yyvsp[0])); }
#line 1848 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 48:
#line 220 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_op(PARS_NE_TOKEN, (yyvsp[-2]), (yyvsp[0])); }
#line 1854 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 49:
#line 221 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_op(PARS_AND_TOKEN, (yyvsp[-2]), (yyvsp[0])); }
#line 1860 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 50:
#line 222 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_op(PARS_OR_TOKEN, (yyvsp[-2]), (yyvsp[0])); }
#line 1866 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 51:
#line 223 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_op(PARS_NOT_TOKEN, (yyvsp[0]), NULL); }
#line 1872 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 52:
#line 225 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_op(PARS_NOTFOUND_TOKEN, (yyvsp[-2]), NULL); }
#line 1878 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 53:
#line 227 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_op(PARS_NOTFOUND_TOKEN, (yyvsp[-2]), NULL); }
#line 1884 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 54:
#line 231 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = &pars_to_char_token; }
#line 1890 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 55:
#line 232 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = &pars_to_number_token; }
#line 1896 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 56:
#line 233 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = &pars_to_binary_token; }
#line 1902 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 57:
#line 235 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = &pars_binary_to_number_token; }
#line 1908 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 58:
#line 236 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = &pars_substr_token; }
#line 1914 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 59:
#line 237 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = &pars_concat_token; }
#line 1920 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 60:
#line 238 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = &pars_instr_token; }
#line 1926 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 61:
#line 239 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = &pars_length_token; }
#line 1932 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 62:
#line 240 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = &pars_sysdate_token; }
#line 1938 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 63:
#line 241 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = &pars_rnd_token; }
#line 1944 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 64:
#line 242 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = &pars_rnd_str_token; }
#line 1950 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 68:
#line 253 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_stored_procedure_call(
					static_cast<sym_node_t*>((yyvsp[-4]))); }
#line 1957 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 69:
#line 259 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_procedure_call((yyvsp[-3]), (yyvsp[-1])); }
#line 1963 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 70:
#line 263 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = &pars_replstr_token; }
#line 1969 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 71:
#line 264 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = &pars_printf_token; }
#line 1975 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 72:
#line 265 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = &pars_assert_token; }
#line 1981 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 73:
#line 269 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = (yyvsp[-2]); }
#line 1987 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 74:
#line 273 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = que_node_list_add_last(NULL, (yyvsp[0])); }
#line 1993 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 75:
#line 275 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = que_node_list_add_last((yyvsp[-2]), (yyvsp[0])); }
#line 1999 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 76:
#line 279 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = NULL; }
#line 2005 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 77:
#line 280 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = que_node_list_add_last(NULL, (yyvsp[0])); }
#line 2011 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 78:
#line 282 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = que_node_list_add_last((yyvsp[-2]), (yyvsp[0])); }
#line 2017 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 79:
#line 286 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = NULL; }
#line 2023 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 80:
#line 287 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = que_node_list_add_last(NULL, (yyvsp[0]));}
#line 2029 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 81:
#line 288 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = que_node_list_add_last((yyvsp[-2]), (yyvsp[0])); }
#line 2035 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 82:
#line 292 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = (yyvsp[0]); }
#line 2041 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 83:
#line 294 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_func(&pars_count_token,
				          que_node_list_add_last(NULL,
					    sym_tab_add_int_lit(
						pars_sym_tab_global, 1))); }
#line 2050 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 84:
#line 299 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_func(&pars_count_token,
					    que_node_list_add_last(NULL,
						pars_func(&pars_distinct_token,
						     que_node_list_add_last(
								NULL, (yyvsp[-1]))))); }
#line 2060 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 85:
#line 305 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_func(&pars_sum_token,
						que_node_list_add_last(NULL,
									(yyvsp[-1]))); }
#line 2068 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 86:
#line 311 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = NULL; }
#line 2074 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 87:
#line 312 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = que_node_list_add_last(NULL, (yyvsp[0])); }
#line 2080 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 88:
#line 314 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = que_node_list_add_last((yyvsp[-2]), (yyvsp[0])); }
#line 2086 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 89:
#line 318 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_select_list(&pars_star_denoter,
								NULL); }
#line 2093 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 90:
#line 321 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_select_list(
					(yyvsp[-2]), static_cast<sym_node_t*>((yyvsp[0]))); }
#line 2100 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 91:
#line 323 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_select_list((yyvsp[0]), NULL); }
#line 2106 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 92:
#line 327 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = NULL; }
#line 2112 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 93:
#line 328 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = (yyvsp[0]); }
#line 2118 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 94:
#line 332 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = NULL; }
#line 2124 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 95:
#line 334 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = &pars_update_token; }
#line 2130 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 96:
#line 338 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = NULL; }
#line 2136 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 97:
#line 340 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = &pars_share_token; }
#line 2142 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 98:
#line 344 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = &pars_asc_token; }
#line 2148 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 99:
#line 345 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = &pars_asc_token; }
#line 2154 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 100:
#line 346 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = &pars_desc_token; }
#line 2160 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 101:
#line 350 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = NULL; }
#line 2166 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 102:
#line 352 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_order_by(
					static_cast<sym_node_t*>((yyvsp[-1])),
					static_cast<pars_res_word_t*>((yyvsp[0]))); }
#line 2174 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 103:
#line 363 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_select_statement(
					static_cast<sel_node_t*>((yyvsp[-6])),
					static_cast<sym_node_t*>((yyvsp[-4])),
					static_cast<que_node_t*>((yyvsp[-3])),
					static_cast<pars_res_word_t*>((yyvsp[-2])),
					static_cast<pars_res_word_t*>((yyvsp[-1])),
					static_cast<order_node_t*>((yyvsp[0]))); }
#line 2186 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 104:
#line 374 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = (yyvsp[0]); }
#line 2192 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 105:
#line 379 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_insert_statement(
					static_cast<sym_node_t*>((yyvsp[-4])), (yyvsp[-1]), NULL); }
#line 2199 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 106:
#line 382 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_insert_statement(
					static_cast<sym_node_t*>((yyvsp[-1])),
					NULL,
					static_cast<sel_node_t*>((yyvsp[0]))); }
#line 2208 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 107:
#line 389 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_column_assignment(
					static_cast<sym_node_t*>((yyvsp[-2])),
					static_cast<que_node_t*>((yyvsp[0]))); }
#line 2216 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 108:
#line 395 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = que_node_list_add_last(NULL, (yyvsp[0])); }
#line 2222 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 109:
#line 397 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = que_node_list_add_last((yyvsp[-2]), (yyvsp[0])); }
#line 2228 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 110:
#line 403 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = (yyvsp[0]); }
#line 2234 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 111:
#line 409 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_update_statement_start(
					FALSE,
					static_cast<sym_node_t*>((yyvsp[-2])),
					static_cast<col_assign_node_t*>((yyvsp[0]))); }
#line 2243 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 112:
#line 417 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_update_statement(
					static_cast<upd_node_t*>((yyvsp[-1])),
					NULL,
					static_cast<que_node_t*>((yyvsp[0]))); }
#line 2252 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 113:
#line 425 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_update_statement(
					static_cast<upd_node_t*>((yyvsp[-1])),
					static_cast<sym_node_t*>((yyvsp[0])),
					NULL); }
#line 2261 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 114:
#line 433 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_update_statement_start(
					TRUE,
					static_cast<sym_node_t*>((yyvsp[0])), NULL); }
#line 2269 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 115:
#line 440 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_update_statement(
					static_cast<upd_node_t*>((yyvsp[-1])),
					NULL,
					static_cast<que_node_t*>((yyvsp[0]))); }
#line 2278 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 116:
#line 448 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_update_statement(
					static_cast<upd_node_t*>((yyvsp[-1])),
					static_cast<sym_node_t*>((yyvsp[0])),
					NULL); }
#line 2287 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 117:
#line 456 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_row_printf_statement(
					static_cast<sel_node_t*>((yyvsp[0]))); }
#line 2294 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 118:
#line 462 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_assignment_statement(
					static_cast<sym_node_t*>((yyvsp[-2])),
					static_cast<que_node_t*>((yyvsp[0]))); }
#line 2302 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 119:
#line 470 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_elsif_element((yyvsp[-2]), (yyvsp[0])); }
#line 2308 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 120:
#line 474 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = que_node_list_add_last(NULL, (yyvsp[0])); }
#line 2314 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 121:
#line 476 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = que_node_list_add_last((yyvsp[-1]), (yyvsp[0])); }
#line 2320 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 122:
#line 480 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = NULL; }
#line 2326 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 123:
#line 482 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = (yyvsp[0]); }
#line 2332 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 124:
#line 483 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = (yyvsp[0]); }
#line 2338 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 125:
#line 490 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_if_statement((yyvsp[-5]), (yyvsp[-3]), (yyvsp[-2])); }
#line 2344 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 126:
#line 496 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_while_statement((yyvsp[-4]), (yyvsp[-2])); }
#line 2350 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 127:
#line 504 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_for_statement(
					static_cast<sym_node_t*>((yyvsp[-8])),
					(yyvsp[-6]), (yyvsp[-4]), (yyvsp[-2])); }
#line 2358 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 128:
#line 510 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_exit_statement(); }
#line 2364 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 129:
#line 514 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_return_statement(); }
#line 2370 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 130:
#line 519 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_open_statement(
						ROW_SEL_OPEN_CURSOR,
						static_cast<sym_node_t*>((yyvsp[0]))); }
#line 2378 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 131:
#line 526 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_open_statement(
						ROW_SEL_CLOSE_CURSOR,
						static_cast<sym_node_t*>((yyvsp[0]))); }
#line 2386 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 132:
#line 533 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_fetch_statement(
					static_cast<sym_node_t*>((yyvsp[-2])),
					static_cast<sym_node_t*>((yyvsp[0])), NULL); }
#line 2394 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 133:
#line 537 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_fetch_statement(
					static_cast<sym_node_t*>((yyvsp[-2])),
					NULL,
					static_cast<sym_node_t*>((yyvsp[0]))); }
#line 2403 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 134:
#line 545 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_column_def(
					static_cast<sym_node_t*>((yyvsp[-4])),
					static_cast<pars_res_word_t*>((yyvsp[-3])),
					static_cast<sym_node_t*>((yyvsp[-2])),
					(yyvsp[-1]), (yyvsp[0])); }
#line 2413 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 135:
#line 553 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = que_node_list_add_last(NULL, (yyvsp[0])); }
#line 2419 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 136:
#line 555 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = que_node_list_add_last((yyvsp[-2]), (yyvsp[0])); }
#line 2425 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 137:
#line 559 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = NULL; }
#line 2431 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 138:
#line 561 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = (yyvsp[-1]); }
#line 2437 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 139:
#line 565 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = NULL; }
#line 2443 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 140:
#line 567 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = &pars_int_token;
					/* pass any non-NULL pointer */ }
#line 2450 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 141:
#line 572 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = NULL; }
#line 2456 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 142:
#line 574 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = &pars_int_token;
					/* pass any non-NULL pointer */ }
#line 2463 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 143:
#line 579 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = NULL; }
#line 2469 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 144:
#line 580 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = &pars_int_token;
					/* pass any non-NULL pointer */ }
#line 2476 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 145:
#line 585 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = NULL; }
#line 2482 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 146:
#line 587 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = (yyvsp[0]); }
#line 2488 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 147:
#line 594 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_create_table(
					static_cast<sym_node_t*>((yyvsp[-5])),
					static_cast<sym_node_t*>((yyvsp[-3])),
					static_cast<sym_node_t*>((yyvsp[-1])),
					static_cast<sym_node_t*>((yyvsp[0]))); }
#line 2498 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 148:
#line 602 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = que_node_list_add_last(NULL, (yyvsp[0])); }
#line 2504 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 149:
#line 604 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = que_node_list_add_last((yyvsp[-2]), (yyvsp[0])); }
#line 2510 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 150:
#line 608 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = NULL; }
#line 2516 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 151:
#line 609 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = &pars_unique_token; }
#line 2522 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 152:
#line 613 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = NULL; }
#line 2528 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 153:
#line 614 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = &pars_clustered_token; }
#line 2534 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 154:
#line 623 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_create_index(
					static_cast<pars_res_word_t*>((yyvsp[-8])),
					static_cast<pars_res_word_t*>((yyvsp[-7])),
					static_cast<sym_node_t*>((yyvsp[-5])),
					static_cast<sym_node_t*>((yyvsp[-3])),
					static_cast<sym_node_t*>((yyvsp[-1]))); }
#line 2545 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 155:
#line 632 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = (yyvsp[0]); }
#line 2551 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 156:
#line 633 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = (yyvsp[0]); }
#line 2557 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 157:
#line 638 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_commit_statement(); }
#line 2563 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 158:
#line 643 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_rollback_statement(); }
#line 2569 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 159:
#line 647 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = &pars_int_token; }
#line 2575 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 160:
#line 648 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = &pars_int_token; }
#line 2581 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 161:
#line 649 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = &pars_bigint_token; }
#line 2587 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 162:
#line 650 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = &pars_char_token; }
#line 2593 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 163:
#line 651 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = &pars_binary_token; }
#line 2599 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 164:
#line 652 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = &pars_blob_token; }
#line 2605 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 165:
#line 657 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_parameter_declaration(
					static_cast<sym_node_t*>((yyvsp[-2])),
					PARS_INPUT,
					static_cast<pars_res_word_t*>((yyvsp[0]))); }
#line 2614 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 166:
#line 662 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_parameter_declaration(
					static_cast<sym_node_t*>((yyvsp[-2])),
					PARS_OUTPUT,
					static_cast<pars_res_word_t*>((yyvsp[0]))); }
#line 2623 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 167:
#line 669 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = NULL; }
#line 2629 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 168:
#line 670 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = que_node_list_add_last(NULL, (yyvsp[0])); }
#line 2635 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 169:
#line 672 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = que_node_list_add_last((yyvsp[-2]), (yyvsp[0])); }
#line 2641 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 170:
#line 677 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_variable_declaration(
					static_cast<sym_node_t*>((yyvsp[-2])),
					static_cast<pars_res_word_t*>((yyvsp[-1]))); }
#line 2649 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 174:
#line 691 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_cursor_declaration(
					static_cast<sym_node_t*>((yyvsp[-3])),
					static_cast<sel_node_t*>((yyvsp[-1]))); }
#line 2657 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 175:
#line 698 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_function_declaration(
					static_cast<sym_node_t*>((yyvsp[-1]))); }
#line 2664 "pars0grm.cc" /* yacc.c:1646  */
    break;

  case 181:
#line 720 "pars0grm.y" /* yacc.c:1646  */
    { (yyval) = pars_procedure_definition(
					static_cast<sym_node_t*>((yyvsp[-9])),
					static_cast<sym_node_t*>((yyvsp[-7])),
					(yyvsp[-1])); }
#line 2673 "pars0grm.cc" /* yacc.c:1646  */
    break;


#line 2677 "pars0grm.cc" /* yacc.c:1646  */
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
#line 726 "pars0grm.y" /* yacc.c:1906  */

