/* A Bison parser, made by GNU Bison 3.4.1.  */

/* Bison implementation for Yacc-like parsers in C

   Copyright (C) 1984, 1989-1990, 2000-2015, 2018-2019 Free Software Foundation,
   Inc.

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

/* Undocumented macros, especially those whose name start with YY_,
   are private implementation details.  Do not rely on them.  */

/* Identify Bison output.  */
#define YYBISON 1

/* Bison version.  */
#define YYBISON_VERSION "3.4.1"

/* Skeleton name.  */
#define YYSKELETON_NAME "yacc.c"

/* Pure parsers.  */
#define YYPURE 0

/* Push parsers.  */
#define YYPUSH 0

/* Pull parsers.  */
#define YYPULL 1




/* First part of user prologue.  */
#line 29 "pars0grm.y"

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

#line 89 "pars0grm.cc"

# ifndef YY_NULLPTR
#  if defined __cplusplus
#   if 201103L <= __cplusplus
#    define YY_NULLPTR nullptr
#   else
#    define YY_NULLPTR 0
#   endif
#  else
#   define YY_NULLPTR ((void*)0)
#  endif
# endif

/* Enabling verbose error messages.  */
#ifdef YYERROR_VERBOSE
# undef YYERROR_VERBOSE
# define YYERROR_VERBOSE 1
#else
# define YYERROR_VERBOSE 0
#endif

/* Use api.header.include to #include this header
   instead of duplicating it here.  */
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
    PARS_INT_TOKEN = 271,
    PARS_CHAR_TOKEN = 272,
    PARS_IS_TOKEN = 273,
    PARS_BEGIN_TOKEN = 274,
    PARS_END_TOKEN = 275,
    PARS_IF_TOKEN = 276,
    PARS_THEN_TOKEN = 277,
    PARS_ELSE_TOKEN = 278,
    PARS_ELSIF_TOKEN = 279,
    PARS_LOOP_TOKEN = 280,
    PARS_WHILE_TOKEN = 281,
    PARS_RETURN_TOKEN = 282,
    PARS_SELECT_TOKEN = 283,
    PARS_COUNT_TOKEN = 284,
    PARS_FROM_TOKEN = 285,
    PARS_WHERE_TOKEN = 286,
    PARS_FOR_TOKEN = 287,
    PARS_DDOT_TOKEN = 288,
    PARS_ORDER_TOKEN = 289,
    PARS_BY_TOKEN = 290,
    PARS_ASC_TOKEN = 291,
    PARS_DESC_TOKEN = 292,
    PARS_INSERT_TOKEN = 293,
    PARS_INTO_TOKEN = 294,
    PARS_VALUES_TOKEN = 295,
    PARS_UPDATE_TOKEN = 296,
    PARS_SET_TOKEN = 297,
    PARS_DELETE_TOKEN = 298,
    PARS_CURRENT_TOKEN = 299,
    PARS_OF_TOKEN = 300,
    PARS_CREATE_TOKEN = 301,
    PARS_TABLE_TOKEN = 302,
    PARS_INDEX_TOKEN = 303,
    PARS_UNIQUE_TOKEN = 304,
    PARS_CLUSTERED_TOKEN = 305,
    PARS_ON_TOKEN = 306,
    PARS_ASSIGN_TOKEN = 307,
    PARS_DECLARE_TOKEN = 308,
    PARS_CURSOR_TOKEN = 309,
    PARS_SQL_TOKEN = 310,
    PARS_OPEN_TOKEN = 311,
    PARS_FETCH_TOKEN = 312,
    PARS_CLOSE_TOKEN = 313,
    PARS_NOTFOUND_TOKEN = 314,
    PARS_TO_BINARY_TOKEN = 315,
    PARS_SUBSTR_TOKEN = 316,
    PARS_CONCAT_TOKEN = 317,
    PARS_INSTR_TOKEN = 318,
    PARS_LENGTH_TOKEN = 319,
    PARS_COMMIT_TOKEN = 320,
    PARS_ROLLBACK_TOKEN = 321,
    PARS_WORK_TOKEN = 322,
    PARS_EXIT_TOKEN = 323,
    PARS_FUNCTION_TOKEN = 324,
    PARS_LOCK_TOKEN = 325,
    PARS_SHARE_TOKEN = 326,
    PARS_MODE_TOKEN = 327,
    PARS_LIKE_TOKEN = 328,
    PARS_LIKE_TOKEN_EXACT = 329,
    PARS_LIKE_TOKEN_PREFIX = 330,
    PARS_LIKE_TOKEN_SUFFIX = 331,
    PARS_LIKE_TOKEN_SUBSTR = 332,
    PARS_TABLE_NAME_TOKEN = 333,
    PARS_BIGINT_TOKEN = 334,
    NEG = 335
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
typedef unsigned short yytype_uint16;
#endif

#ifdef YYTYPE_INT16
typedef YYTYPE_INT16 yytype_int16;
#else
typedef short yytype_int16;
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
#  define YYSIZE_T unsigned
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

/* Suppress unused-variable warnings by "using" E.  */
#if ! defined lint || defined __GNUC__
# define YYUSE(E) ((void) (E))
#else
# define YYUSE(E) /* empty */
#endif

#if defined __GNUC__ && ! defined __ICC && 407 <= __GNUC__ * 100 + __GNUC_MINOR__
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


#define YY_ASSERT(E) ((void) (0 && (E)))

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
#define YYLAST   623

/* YYNTOKENS -- Number of terminals.  */
#define YYNTOKENS  96
/* YYNNTS -- Number of nonterminals.  */
#define YYNNTS  64
/* YYNRULES -- Number of rules.  */
#define YYNRULES  150
/* YYNSTATES -- Number of states.  */
#define YYNSTATES  301

#define YYUNDEFTOK  2
#define YYMAXUTOK   335

/* YYTRANSLATE(TOKEN-NUM) -- Symbol number corresponding to TOKEN-NUM
   as returned by yylex, with out-of-bounds checking.  */
#define YYTRANSLATE(YYX)                                                \
  ((unsigned) (YYX) <= YYMAXUTOK ? yytranslate[YYX] : YYUNDEFTOK)

/* YYTRANSLATE[TOKEN-NUM] -- Symbol number corresponding to TOKEN-NUM
   as returned by yylex.  */
static const yytype_uint8 yytranslate[] =
{
       0,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,    88,     2,     2,
      90,    91,    85,    84,    93,    83,     2,    86,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,    89,
      81,    80,    82,    92,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,    94,     2,    95,     2,     2,     2,     2,
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
      75,    76,    77,    78,    79,    87
};

#if YYDEBUG
  /* YYRLINE[YYN] -- Source line where rule number YYN was defined.  */
static const yytype_uint16 yyrline[] =
{
       0,   140,   140,   143,   144,   145,   146,   147,   148,   149,
     150,   151,   152,   153,   154,   155,   156,   157,   158,   159,
     160,   161,   162,   166,   167,   172,   173,   175,   176,   177,
     178,   179,   180,   181,   182,   183,   184,   185,   186,   187,
     189,   190,   191,   192,   193,   194,   195,   196,   197,   199,
     204,   205,   206,   207,   208,   211,   213,   214,   218,   224,
     228,   229,   234,   235,   236,   241,   242,   243,   247,   248,
     256,   257,   258,   263,   265,   268,   272,   273,   277,   278,
     283,   284,   289,   290,   291,   295,   296,   303,   318,   323,
     327,   336,   342,   343,   348,   354,   363,   371,   379,   386,
     394,   402,   409,   415,   416,   421,   422,   424,   428,   435,
     441,   451,   455,   459,   466,   473,   477,   485,   494,   495,
     500,   501,   506,   507,   513,   521,   522,   527,   528,   532,
     533,   537,   551,   552,   556,   561,   566,   567,   568,   572,
     578,   580,   581,   585,   593,   599,   600,   603,   605,   606,
     610
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
  "PARS_INT_TOKEN", "PARS_CHAR_TOKEN", "PARS_IS_TOKEN", "PARS_BEGIN_TOKEN",
  "PARS_END_TOKEN", "PARS_IF_TOKEN", "PARS_THEN_TOKEN", "PARS_ELSE_TOKEN",
  "PARS_ELSIF_TOKEN", "PARS_LOOP_TOKEN", "PARS_WHILE_TOKEN",
  "PARS_RETURN_TOKEN", "PARS_SELECT_TOKEN", "PARS_COUNT_TOKEN",
  "PARS_FROM_TOKEN", "PARS_WHERE_TOKEN", "PARS_FOR_TOKEN",
  "PARS_DDOT_TOKEN", "PARS_ORDER_TOKEN", "PARS_BY_TOKEN", "PARS_ASC_TOKEN",
  "PARS_DESC_TOKEN", "PARS_INSERT_TOKEN", "PARS_INTO_TOKEN",
  "PARS_VALUES_TOKEN", "PARS_UPDATE_TOKEN", "PARS_SET_TOKEN",
  "PARS_DELETE_TOKEN", "PARS_CURRENT_TOKEN", "PARS_OF_TOKEN",
  "PARS_CREATE_TOKEN", "PARS_TABLE_TOKEN", "PARS_INDEX_TOKEN",
  "PARS_UNIQUE_TOKEN", "PARS_CLUSTERED_TOKEN", "PARS_ON_TOKEN",
  "PARS_ASSIGN_TOKEN", "PARS_DECLARE_TOKEN", "PARS_CURSOR_TOKEN",
  "PARS_SQL_TOKEN", "PARS_OPEN_TOKEN", "PARS_FETCH_TOKEN",
  "PARS_CLOSE_TOKEN", "PARS_NOTFOUND_TOKEN", "PARS_TO_BINARY_TOKEN",
  "PARS_SUBSTR_TOKEN", "PARS_CONCAT_TOKEN", "PARS_INSTR_TOKEN",
  "PARS_LENGTH_TOKEN", "PARS_COMMIT_TOKEN", "PARS_ROLLBACK_TOKEN",
  "PARS_WORK_TOKEN", "PARS_EXIT_TOKEN", "PARS_FUNCTION_TOKEN",
  "PARS_LOCK_TOKEN", "PARS_SHARE_TOKEN", "PARS_MODE_TOKEN",
  "PARS_LIKE_TOKEN", "PARS_LIKE_TOKEN_EXACT", "PARS_LIKE_TOKEN_PREFIX",
  "PARS_LIKE_TOKEN_SUFFIX", "PARS_LIKE_TOKEN_SUBSTR",
  "PARS_TABLE_NAME_TOKEN", "PARS_BIGINT_TOKEN", "'='", "'<'", "'>'", "'-'",
  "'+'", "'*'", "'/'", "NEG", "'%'", "';'", "'('", "')'", "'?'", "','",
  "'{'", "'}'", "$accept", "top_statement", "statement", "statement_list",
  "exp", "function_name", "question_mark_list", "stored_procedure_call",
  "user_function_call", "table_list", "variable_list", "exp_list",
  "select_item", "select_item_list", "select_list", "search_condition",
  "for_update_clause", "lock_shared_clause", "order_direction",
  "order_by_clause", "select_statement", "insert_statement_start",
  "insert_statement", "column_assignment", "column_assignment_list",
  "cursor_positioned", "update_statement_start",
  "update_statement_searched", "update_statement_positioned",
  "delete_statement_start", "delete_statement_searched",
  "delete_statement_positioned", "assignment_statement", "elsif_element",
  "elsif_list", "else_part", "if_statement", "while_statement",
  "for_statement", "exit_statement", "return_statement",
  "open_cursor_statement", "close_cursor_statement", "fetch_statement",
  "column_def", "column_def_list", "opt_column_len", "opt_not_null",
  "create_table", "column_list", "unique_def", "clustered_def",
  "create_index", "table_name", "commit_statement", "rollback_statement",
  "type_name", "variable_declaration", "variable_declaration_list",
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
      61,    60,    62,    45,    43,    42,    47,   335,    37,    59,
      40,    41,    63,    44,   123,   125
};
# endif

#define YYPACT_NINF -162

#define yypact_value_is_default(Yystate) \
  (!!((Yystate) == (-162)))

#define YYTABLE_NINF -1

#define yytable_value_is_error(Yytable_value) \
  0

  /* YYPACT[STATE-NUM] -- Index in YYTABLE of the portion describing
     STATE-NUM.  */
static const yytype_int16 yypact[] =
{
      10,    21,    39,   -47,   -45,  -162,  -162,   -44,    28,    41,
      -5,  -162,     6,  -162,  -162,  -162,   -38,   -37,  -162,  -162,
    -162,  -162,    -4,  -162,    45,    47,   267,  -162,    37,   -28,
       8,   416,   416,  -162,   324,    55,    24,     2,    35,   -24,
      61,    62,    76,    17,    18,  -162,    84,  -162,   129,  -162,
       4,   -10,     5,    69,    12,    14,    69,    15,    19,    23,
      27,    34,    44,    48,    51,    53,    56,    57,    59,    65,
      70,    71,    74,  -162,   416,  -162,  -162,  -162,  -162,    25,
     416,    36,  -162,  -162,  -162,  -162,  -162,   416,   416,   436,
      54,   456,    68,  -162,   537,  -162,   -29,   121,   147,     2,
    -162,  -162,   122,     2,     2,  -162,   113,  -162,   126,  -162,
    -162,  -162,    78,  -162,  -162,  -162,    79,  -162,  -162,   334,
    -162,  -162,  -162,  -162,  -162,  -162,  -162,  -162,  -162,  -162,
    -162,  -162,  -162,  -162,  -162,  -162,  -162,  -162,  -162,  -162,
    -162,    85,   537,   114,    98,   117,     9,   355,   416,   416,
     416,   416,   416,   267,   172,   416,   416,   416,   416,   416,
     416,   416,   416,   267,   104,   183,   370,     2,   416,  -162,
     185,  -162,   103,  -162,   151,   198,   118,   416,   161,   537,
    -162,  -162,  -162,  -162,    98,    98,    13,    13,   537,    49,
    -162,    13,    13,    13,    -7,    -7,     9,     9,   537,   -64,
     181,   120,  -162,   119,  -162,   -26,  -162,   474,   134,  -162,
     123,   208,   210,   128,   183,   119,  -162,   -58,   -57,   213,
     267,   416,  -162,   197,   205,  -162,   416,   201,  -162,   222,
     416,     2,   202,   416,   416,   185,    -5,  -162,   -53,   179,
     142,   119,   140,   144,  -162,  -162,   267,   489,  -162,   219,
     537,  -162,  -162,  -162,   200,   173,   519,   537,  -162,   152,
    -162,   208,     2,  -162,  -162,  -162,   267,  -162,  -162,   233,
     220,   267,   250,   245,  -162,   167,   267,   187,   225,  -162,
     224,   168,   255,  -162,   256,   192,   259,   243,  -162,  -162,
    -162,   -50,  -162,   -17,  -162,  -162,   262,  -162,  -162,  -162,
    -162
};

  /* YYDEFACT[STATE-NUM] -- Default reduction number in state STATE-NUM.
     Performed when YYTABLE does not specify something else to do.  Zero
     means the default is an error.  */
static const yytype_uint8 yydefact[] =
{
       0,     0,     0,     0,     0,     1,     2,     0,     0,   140,
       0,   141,   147,   136,   138,   137,     0,     0,   142,   145,
     146,   148,     0,   139,     0,     0,     0,   149,     0,     0,
       0,     0,     0,   112,    70,     0,     0,     0,     0,   127,
       0,     0,     0,     0,     0,   111,     0,    23,     0,     3,
       0,     0,     0,    76,     0,     0,    76,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,   144,     0,    27,    28,    29,    30,    25,
       0,    31,    50,    51,    52,    53,    54,     0,     0,     0,
       0,     0,     0,    73,    68,    71,    75,     0,     0,     0,
     132,   133,     0,     0,     0,   128,   129,   113,     0,   114,
     134,   135,     0,   150,    24,    10,     0,    90,    11,     0,
      96,    97,    14,    15,    99,   100,    12,    13,     9,     7,
       4,     5,     6,     8,    16,    18,    17,    21,    22,    19,
      20,     0,   101,     0,    47,     0,    36,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,    65,     0,     0,    62,     0,     0,     0,    88,
       0,    98,     0,   130,     0,    62,    55,    65,     0,    77,
     143,    48,    49,    37,    45,    46,    42,    43,    44,   105,
      39,    38,    40,    41,    33,    32,    34,    35,    66,     0,
       0,     0,    63,    74,    72,    76,    60,     0,     0,    92,
      95,     0,     0,    63,    62,   115,    56,     0,     0,     0,
       0,     0,   103,   107,     0,    26,     0,     0,    69,     0,
       0,     0,    78,     0,     0,     0,     0,   118,     0,     0,
       0,   116,     0,     0,    89,    94,   106,     0,   104,     0,
      67,   109,    64,    61,     0,    80,     0,    91,    93,   120,
     124,     0,     0,    59,    58,    57,     0,   108,    79,     0,
      85,     0,     0,   122,   119,     0,   102,     0,     0,    87,
       0,     0,     0,   117,     0,     0,     0,     0,   121,   123,
     125,     0,    81,    82,   110,   131,     0,    83,    84,    86,
     126
};

  /* YYPGOTO[NTERM-NUM].  */
static const yytype_int16 yypgoto[] =
{
    -162,  -162,   -48,  -132,   -30,  -162,  -162,  -162,  -162,  -162,
    -161,    94,   106,  -162,  -162,   -52,  -162,  -162,  -162,  -162,
     -35,  -162,  -162,    38,  -162,   221,  -162,  -162,  -162,  -162,
    -162,  -162,  -162,    60,  -162,  -162,  -162,  -162,  -162,  -162,
    -162,  -162,  -162,  -162,    26,  -162,  -162,  -162,  -162,  -162,
    -162,  -162,  -162,   -96,  -162,  -162,    40,   266,  -162,  -162,
    -162,   257,  -162,  -162
};

  /* YYDEFGOTO[NTERM-NUM].  */
static const yytype_int16 yydefgoto[] =
{
      -1,     2,    47,    48,    94,    90,   217,    49,   214,   205,
     203,   199,    95,    96,    97,   120,   255,   270,   299,   279,
      50,    51,    52,   209,   210,   121,    53,    54,    55,    56,
      57,    58,    59,   222,   223,   224,    60,    61,    62,    63,
      64,    65,    66,    67,   237,   238,   273,   283,    68,   291,
     106,   174,    69,   102,    70,    71,    16,    11,    12,    19,
      20,    21,    22,     3
};

  /* YYTABLE[YYPACT[STATE-NUM]] -- What to do in state STATE-NUM.  If
     positive, shift that token.  If negative, reduce the rule whose
     number is the opposite.  If YYTABLE_NINF, syntax error.  */
static const yytype_uint16 yytable[] =
{
     114,    89,    91,   169,   124,   230,   152,   171,   172,   100,
     165,    13,    14,    10,   215,    26,   117,    24,    34,   297,
     298,   189,   152,   104,     1,   105,   152,   225,     4,   226,
     116,   200,    25,   242,   244,   243,   226,   141,   260,     5,
     261,   295,     6,   296,   142,     7,     9,     8,    10,    17,
     144,    23,    28,   241,    29,    72,    30,   146,   147,    17,
      74,    73,    98,    99,   166,   103,   154,   231,   107,   108,
      31,   206,   220,   221,    15,    32,    33,    34,   160,   161,
     101,    35,   154,   109,   110,   111,   154,    36,   246,   179,
      37,   112,    38,   115,   118,    39,   158,   159,   160,   161,
     119,   122,    34,   123,   126,    40,    41,    42,   127,   150,
     151,   152,   128,   143,    43,    44,   129,    45,   184,   185,
     186,   187,   188,   130,   145,   191,   192,   193,   194,   195,
     196,   197,   198,   131,   276,   253,    30,   132,   207,   280,
     133,   114,   134,    46,   162,   135,   136,   198,   137,   113,
      31,   167,   114,   232,   138,    32,    33,    34,   164,   139,
     140,    35,   168,   173,   170,   175,   275,    36,   176,   177,
      37,   154,    38,   181,   180,    39,   182,   190,   155,   156,
     157,   158,   159,   160,   161,    40,    41,    42,    30,   201,
     202,   247,   208,   211,    43,    44,   250,    45,   114,   212,
     179,   227,    31,   256,   257,   213,   219,    32,    33,    34,
     216,   228,   229,    35,   234,   236,   235,   239,   240,    36,
     245,   221,    37,    46,    38,   249,   251,    39,   114,   252,
     262,    30,   114,   263,   254,   264,   265,    40,    41,    42,
     267,   268,   272,   269,   287,    31,    43,    44,   277,    45,
      32,    33,    34,   281,   278,   282,    35,   284,   285,   288,
     286,   289,    36,   290,   292,    37,   293,    38,   294,   300,
      39,   218,   204,   258,    30,    46,   259,   125,    18,    27,
      40,    41,    42,   248,     0,     0,     0,   274,    31,    43,
      44,     0,    45,    32,    33,    34,     0,     0,     0,    35,
       0,     0,     0,     0,     0,    36,     0,     0,    37,     0,
      38,     0,     0,    39,     0,     0,     0,     0,    46,     0,
       0,     0,     0,    40,    41,    42,     0,    75,    76,    77,
      78,    79,    43,    44,    80,    45,     0,    75,    76,    77,
      78,    79,     0,     0,    80,     0,     0,     0,     0,     0,
       0,     0,     0,    92,     0,     0,     0,     0,     0,     0,
       0,    46,     0,   148,   149,     0,   150,   151,   152,     0,
       0,     0,     0,    75,    76,    77,    78,    79,   178,    81,
      80,     0,     0,     0,    82,    83,    84,    85,    86,    81,
       0,     0,     0,     0,    82,    83,    84,    85,    86,    92,
       0,     0,     0,     0,     0,     0,     0,    87,     0,    93,
       0,     0,     0,     0,    88,     0,     0,    87,     0,    75,
      76,    77,    78,    79,    88,    81,    80,     0,   154,     0,
      82,    83,    84,    85,    86,   155,   156,   157,   158,   159,
     160,   161,     0,     0,   148,   149,   183,   150,   151,   152,
       0,     0,     0,    87,     0,     0,     0,     0,   153,     0,
      88,     0,     0,     0,   148,   149,     0,   150,   151,   152,
       0,    81,     0,     0,     0,     0,    82,    83,    84,    85,
      86,   163,   148,   149,     0,   150,   151,   152,     0,     0,
       0,     0,     0,     0,     0,     0,     0,   148,   149,    87,
     150,   151,   152,     0,     0,     0,    88,   233,     0,   154,
       0,   266,     0,     0,     0,     0,   155,   156,   157,   158,
     159,   160,   161,     0,     0,     0,     0,   148,   149,   154,
     150,   151,   152,     0,     0,     0,   155,   156,   157,   158,
     159,   160,   161,     0,   271,   148,   149,   154,   150,   151,
     152,     0,     0,     0,   155,   156,   157,   158,   159,   160,
     161,     0,   154,     0,     0,     0,     0,     0,     0,   155,
     156,   157,   158,   159,   160,   161,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,     0,   154,     0,     0,     0,     0,     0,     0,   155,
     156,   157,   158,   159,   160,   161,     0,     0,     0,     0,
     154,     0,     0,     0,     0,     0,     0,   155,   156,   157,
     158,   159,   160,   161
};

static const yytype_int16 yycheck[] =
{
      48,    31,    32,    99,    56,    31,    13,   103,   104,     7,
      39,    16,    17,     7,   175,    19,    51,    54,    28,    36,
      37,   153,    13,    47,    14,    49,    13,    91,     7,    93,
      40,   163,    69,    91,    91,    93,    93,    72,    91,     0,
      93,    91,    89,    93,    74,    90,    18,    91,     7,    53,
      80,    89,     7,   214,     7,    18,     7,    87,    88,    53,
      52,    89,     7,    39,    93,    30,    73,    93,     7,     7,
      21,   167,    23,    24,    79,    26,    27,    28,    85,    86,
      78,    32,    73,     7,    67,    67,    73,    38,   220,   119,
      41,     7,    43,    89,    89,    46,    83,    84,    85,    86,
      31,    89,    28,    89,    89,    56,    57,    58,    89,    11,
      12,    13,    89,    88,    65,    66,    89,    68,   148,   149,
     150,   151,   152,    89,    88,   155,   156,   157,   158,   159,
     160,   161,   162,    89,   266,   231,     7,    89,   168,   271,
      89,   189,    89,    94,    90,    89,    89,   177,    89,    20,
      21,    30,   200,   205,    89,    26,    27,    28,    90,    89,
      89,    32,    15,    50,    42,    39,   262,    38,    90,    90,
      41,    73,    43,    59,    89,    46,    59,     5,    80,    81,
      82,    83,    84,    85,    86,    56,    57,    58,     7,    85,
       7,   221,     7,    90,    65,    66,   226,    68,   246,    48,
     230,    20,    21,   233,   234,     7,    45,    26,    27,    28,
      92,    91,    93,    32,    80,     7,    93,     7,    90,    38,
       7,    24,    41,    94,    43,    20,    25,    46,   276,     7,
      51,     7,   280,    91,    32,    95,    92,    56,    57,    58,
      21,    41,    90,    70,    20,    21,    65,    66,    15,    68,
      26,    27,    28,     3,    34,    10,    32,    90,    71,    91,
      35,     6,    38,     7,    72,    41,     7,    43,    25,     7,
      46,   177,   166,   235,     7,    94,   236,    56,    12,    22,
      56,    57,    58,   223,    -1,    -1,    -1,   261,    21,    65,
      66,    -1,    68,    26,    27,    28,    -1,    -1,    -1,    32,
      -1,    -1,    -1,    -1,    -1,    38,    -1,    -1,    41,    -1,
      43,    -1,    -1,    46,    -1,    -1,    -1,    -1,    94,    -1,
      -1,    -1,    -1,    56,    57,    58,    -1,     3,     4,     5,
       6,     7,    65,    66,    10,    68,    -1,     3,     4,     5,
       6,     7,    -1,    -1,    10,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    29,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    94,    -1,     8,     9,    -1,    11,    12,    13,    -1,
      -1,    -1,    -1,     3,     4,     5,     6,     7,    44,    55,
      10,    -1,    -1,    -1,    60,    61,    62,    63,    64,    55,
      -1,    -1,    -1,    -1,    60,    61,    62,    63,    64,    29,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    83,    -1,    85,
      -1,    -1,    -1,    -1,    90,    -1,    -1,    83,    -1,     3,
       4,     5,     6,     7,    90,    55,    10,    -1,    73,    -1,
      60,    61,    62,    63,    64,    80,    81,    82,    83,    84,
      85,    86,    -1,    -1,     8,     9,    91,    11,    12,    13,
      -1,    -1,    -1,    83,    -1,    -1,    -1,    -1,    22,    -1,
      90,    -1,    -1,    -1,     8,     9,    -1,    11,    12,    13,
      -1,    55,    -1,    -1,    -1,    -1,    60,    61,    62,    63,
      64,    25,     8,     9,    -1,    11,    12,    13,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,     8,     9,    83,
      11,    12,    13,    -1,    -1,    -1,    90,    33,    -1,    73,
      -1,    22,    -1,    -1,    -1,    -1,    80,    81,    82,    83,
      84,    85,    86,    -1,    -1,    -1,    -1,     8,     9,    73,
      11,    12,    13,    -1,    -1,    -1,    80,    81,    82,    83,
      84,    85,    86,    -1,    25,     8,     9,    73,    11,    12,
      13,    -1,    -1,    -1,    80,    81,    82,    83,    84,    85,
      86,    -1,    73,    -1,    -1,    -1,    -1,    -1,    -1,    80,
      81,    82,    83,    84,    85,    86,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    73,    -1,    -1,    -1,    -1,    -1,    -1,    80,
      81,    82,    83,    84,    85,    86,    -1,    -1,    -1,    -1,
      73,    -1,    -1,    -1,    -1,    -1,    -1,    80,    81,    82,
      83,    84,    85,    86
};

  /* YYSTOS[STATE-NUM] -- The (internal number of the) accessing
     symbol of state STATE-NUM.  */
static const yytype_uint8 yystos[] =
{
       0,    14,    97,   159,     7,     0,    89,    90,    91,    18,
       7,   153,   154,    16,    17,    79,   152,    53,   153,   155,
     156,   157,   158,    89,    54,    69,    19,   157,     7,     7,
       7,    21,    26,    27,    28,    32,    38,    41,    43,    46,
      56,    57,    58,    65,    66,    68,    94,    98,    99,   103,
     116,   117,   118,   122,   123,   124,   125,   126,   127,   128,
     132,   133,   134,   135,   136,   137,   138,   139,   144,   148,
     150,   151,    18,    89,    52,     3,     4,     5,     6,     7,
      10,    55,    60,    61,    62,    63,    64,    83,    90,   100,
     101,   100,    29,    85,   100,   108,   109,   110,     7,    39,
       7,    78,   149,    30,    47,    49,   146,     7,     7,     7,
      67,    67,     7,    20,    98,    89,    40,   116,    89,    31,
     111,   121,    89,    89,   111,   121,    89,    89,    89,    89,
      89,    89,    89,    89,    89,    89,    89,    89,    89,    89,
      89,   116,   100,    88,   100,    88,   100,   100,     8,     9,
      11,    12,    13,    22,    73,    80,    81,    82,    83,    84,
      85,    86,    90,    25,    90,    39,    93,    30,    15,   149,
      42,   149,   149,    50,   147,    39,    90,    90,    44,   100,
      89,    59,    59,    91,   100,   100,   100,   100,   100,    99,
       5,   100,   100,   100,   100,   100,   100,   100,   100,   107,
      99,    85,     7,   106,   108,   105,   149,   100,     7,   119,
     120,    90,    48,     7,   104,   106,    92,   102,   107,    45,
      23,    24,   129,   130,   131,    91,    93,    20,    91,    93,
      31,    93,   111,    33,    80,    93,     7,   140,   141,     7,
      90,   106,    91,    93,    91,     7,    99,   100,   129,    20,
     100,    25,     7,   149,    32,   112,   100,   100,   119,   152,
      91,    93,    51,    91,    95,    92,    22,    21,    41,    70,
     113,    25,    90,   142,   140,   149,    99,    15,    34,   115,
      99,     3,    10,   143,    90,    71,    35,    20,    91,     6,
       7,   145,    72,     7,    25,    91,    93,    36,    37,   114,
       7
};

  /* YYR1[YYN] -- Symbol number of symbol that rule YYN derives.  */
static const yytype_uint8 yyr1[] =
{
       0,    96,    97,    98,    98,    98,    98,    98,    98,    98,
      98,    98,    98,    98,    98,    98,    98,    98,    98,    98,
      98,    98,    98,    99,    99,   100,   100,   100,   100,   100,
     100,   100,   100,   100,   100,   100,   100,   100,   100,   100,
     100,   100,   100,   100,   100,   100,   100,   100,   100,   100,
     101,   101,   101,   101,   101,   102,   102,   102,   103,   104,
     105,   105,   106,   106,   106,   107,   107,   107,   108,   108,
     109,   109,   109,   110,   110,   110,   111,   111,   112,   112,
     113,   113,   114,   114,   114,   115,   115,   116,   117,   118,
     118,   119,   120,   120,   121,   122,   123,   124,   125,   126,
     127,   128,   129,   130,   130,   131,   131,   131,   132,   133,
     134,   135,   136,   137,   138,   139,   139,   140,   141,   141,
     142,   142,   143,   143,   144,   145,   145,   146,   146,   147,
     147,   148,   149,   149,   150,   151,   152,   152,   152,   153,
     154,   154,   154,   155,   156,   157,   157,   158,   158,   158,
     159
};

  /* YYR2[YYN] -- Number of symbols on the right hand side of rule YYN.  */
static const yytype_uint8 yyr2[] =
{
       0,     2,     2,     1,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     1,     2,     1,     4,     1,     1,     1,
       1,     1,     3,     3,     3,     3,     2,     3,     3,     3,
       3,     3,     3,     3,     3,     3,     3,     2,     3,     3,
       1,     1,     1,     1,     1,     0,     1,     3,     6,     3,
       1,     3,     0,     1,     3,     0,     1,     3,     1,     4,
       0,     1,     3,     1,     3,     1,     0,     2,     0,     2,
       0,     4,     0,     1,     1,     0,     4,     8,     3,     5,
       2,     3,     1,     3,     4,     4,     2,     2,     3,     2,
       2,     3,     4,     1,     2,     0,     2,     1,     7,     6,
      10,     1,     1,     2,     2,     4,     5,     4,     1,     3,
       0,     3,     0,     2,     6,     1,     3,     0,     1,     0,
       1,    10,     1,     1,     2,     2,     1,     1,     1,     3,
       0,     1,     2,     6,     4,     1,     1,     0,     1,     2,
      10
};


#define yyerrok         (yyerrstatus = 0)
#define yyclearin       (yychar = YYEMPTY)
#define YYEMPTY         (-2)
#define YYEOF           0

#define YYACCEPT        goto yyacceptlab
#define YYABORT         goto yyabortlab
#define YYERROR         goto yyerrorlab


#define YYRECOVERING()  (!!yyerrstatus)

#define YYBACKUP(Token, Value)                                    \
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


/*-----------------------------------.
| Print this symbol's value on YYO.  |
`-----------------------------------*/

static void
yy_symbol_value_print (FILE *yyo, int yytype, YYSTYPE const * const yyvaluep)
{
  FILE *yyoutput = yyo;
  YYUSE (yyoutput);
  if (!yyvaluep)
    return;
# ifdef YYPRINT
  if (yytype < YYNTOKENS)
    YYPRINT (yyo, yytoknum[yytype], *yyvaluep);
# endif
  YYUSE (yytype);
}


/*---------------------------.
| Print this symbol on YYO.  |
`---------------------------*/

static void
yy_symbol_print (FILE *yyo, int yytype, YYSTYPE const * const yyvaluep)
{
  YYFPRINTF (yyo, "%s %s (",
             yytype < YYNTOKENS ? "token" : "nterm", yytname[yytype]);

  yy_symbol_value_print (yyo, yytype, yyvaluep);
  YYFPRINTF (yyo, ")");
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
  unsigned long yylno = yyrline[yyrule];
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
                       &yyvsp[(yyi + 1) - (yynrhs)]
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
            else
              goto append;

          append:
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

  return (YYSIZE_T) (yystpcpy (yyres, yystr) - yyres);
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
                  if (yysize <= yysize1 && yysize1 <= YYSTACK_ALLOC_MAXIMUM)
                    yysize = yysize1;
                  else
                    return 2;
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
    default: /* Avoid compiler warnings. */
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
    if (yysize <= yysize1 && yysize1 <= YYSTACK_ALLOC_MAXIMUM)
      yysize = yysize1;
    else
      return 2;
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
| yynewstate -- push a new state, which is found in yystate.  |
`------------------------------------------------------------*/
yynewstate:
  /* In all cases, when you get here, the value and location stacks
     have just been pushed.  So pushing a state here evens the stacks.  */
  yyssp++;


/*--------------------------------------------------------------------.
| yynewstate -- set current state (the top of the stack) to yystate.  |
`--------------------------------------------------------------------*/
yysetstate:
  YYDPRINTF ((stderr, "Entering state %d\n", yystate));
  YY_ASSERT (0 <= yystate && yystate < YYNSTATES);
  *yyssp = (yytype_int16) yystate;

  if (yyss + yystacksize - 1 <= yyssp)
#if !defined yyoverflow && !defined YYSTACK_RELOCATE
    goto yyexhaustedlab;
#else
    {
      /* Get the current used size of the three stacks, in elements.  */
      YYSIZE_T yysize = (YYSIZE_T) (yyssp - yyss + 1);

# if defined yyoverflow
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
# else /* defined YYSTACK_RELOCATE */
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
# undef YYSTACK_RELOCATE
        if (yyss1 != yyssa)
          YYSTACK_FREE (yyss1);
      }
# endif

      yyssp = yyss + yysize - 1;
      yyvsp = yyvs + yysize - 1;

      YYDPRINTF ((stderr, "Stack size increased to %lu\n",
                  (unsigned long) yystacksize));

      if (yyss + yystacksize - 1 <= yyssp)
        YYABORT;
    }
#endif /* !defined yyoverflow && !defined YYSTACK_RELOCATE */

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
| yyreduce -- do a reduction.  |
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
  case 23:
#line 166 "pars0grm.y"
    { yyval = que_node_list_add_last(NULL, yyvsp[0]); }
#line 1621 "pars0grm.cc"
    break;

  case 24:
#line 168 "pars0grm.y"
    { yyval = que_node_list_add_last(yyvsp[-1], yyvsp[0]); }
#line 1627 "pars0grm.cc"
    break;

  case 25:
#line 172 "pars0grm.y"
    { yyval = yyvsp[0];}
#line 1633 "pars0grm.cc"
    break;

  case 26:
#line 174 "pars0grm.y"
    { yyval = pars_func(yyvsp[-3], yyvsp[-1]); }
#line 1639 "pars0grm.cc"
    break;

  case 27:
#line 175 "pars0grm.y"
    { yyval = yyvsp[0];}
#line 1645 "pars0grm.cc"
    break;

  case 28:
#line 176 "pars0grm.y"
    { yyval = yyvsp[0];}
#line 1651 "pars0grm.cc"
    break;

  case 29:
#line 177 "pars0grm.y"
    { yyval = yyvsp[0];}
#line 1657 "pars0grm.cc"
    break;

  case 30:
#line 178 "pars0grm.y"
    { yyval = yyvsp[0];}
#line 1663 "pars0grm.cc"
    break;

  case 31:
#line 179 "pars0grm.y"
    { yyval = yyvsp[0];}
#line 1669 "pars0grm.cc"
    break;

  case 32:
#line 180 "pars0grm.y"
    { yyval = pars_op('+', yyvsp[-2], yyvsp[0]); }
#line 1675 "pars0grm.cc"
    break;

  case 33:
#line 181 "pars0grm.y"
    { yyval = pars_op('-', yyvsp[-2], yyvsp[0]); }
#line 1681 "pars0grm.cc"
    break;

  case 34:
#line 182 "pars0grm.y"
    { yyval = pars_op('*', yyvsp[-2], yyvsp[0]); }
#line 1687 "pars0grm.cc"
    break;

  case 35:
#line 183 "pars0grm.y"
    { yyval = pars_op('/', yyvsp[-2], yyvsp[0]); }
#line 1693 "pars0grm.cc"
    break;

  case 36:
#line 184 "pars0grm.y"
    { yyval = pars_op('-', yyvsp[0], NULL); }
#line 1699 "pars0grm.cc"
    break;

  case 37:
#line 185 "pars0grm.y"
    { yyval = yyvsp[-1]; }
#line 1705 "pars0grm.cc"
    break;

  case 38:
#line 186 "pars0grm.y"
    { yyval = pars_op('=', yyvsp[-2], yyvsp[0]); }
#line 1711 "pars0grm.cc"
    break;

  case 39:
#line 188 "pars0grm.y"
    { yyval = pars_op(PARS_LIKE_TOKEN, yyvsp[-2], yyvsp[0]); }
#line 1717 "pars0grm.cc"
    break;

  case 40:
#line 189 "pars0grm.y"
    { yyval = pars_op('<', yyvsp[-2], yyvsp[0]); }
#line 1723 "pars0grm.cc"
    break;

  case 41:
#line 190 "pars0grm.y"
    { yyval = pars_op('>', yyvsp[-2], yyvsp[0]); }
#line 1729 "pars0grm.cc"
    break;

  case 42:
#line 191 "pars0grm.y"
    { yyval = pars_op(PARS_GE_TOKEN, yyvsp[-2], yyvsp[0]); }
#line 1735 "pars0grm.cc"
    break;

  case 43:
#line 192 "pars0grm.y"
    { yyval = pars_op(PARS_LE_TOKEN, yyvsp[-2], yyvsp[0]); }
#line 1741 "pars0grm.cc"
    break;

  case 44:
#line 193 "pars0grm.y"
    { yyval = pars_op(PARS_NE_TOKEN, yyvsp[-2], yyvsp[0]); }
#line 1747 "pars0grm.cc"
    break;

  case 45:
#line 194 "pars0grm.y"
    { yyval = pars_op(PARS_AND_TOKEN, yyvsp[-2], yyvsp[0]); }
#line 1753 "pars0grm.cc"
    break;

  case 46:
#line 195 "pars0grm.y"
    { yyval = pars_op(PARS_OR_TOKEN, yyvsp[-2], yyvsp[0]); }
#line 1759 "pars0grm.cc"
    break;

  case 47:
#line 196 "pars0grm.y"
    { yyval = pars_op(PARS_NOT_TOKEN, yyvsp[0], NULL); }
#line 1765 "pars0grm.cc"
    break;

  case 48:
#line 198 "pars0grm.y"
    { yyval = pars_op(PARS_NOTFOUND_TOKEN, yyvsp[-2], NULL); }
#line 1771 "pars0grm.cc"
    break;

  case 49:
#line 200 "pars0grm.y"
    { yyval = pars_op(PARS_NOTFOUND_TOKEN, yyvsp[-2], NULL); }
#line 1777 "pars0grm.cc"
    break;

  case 50:
#line 204 "pars0grm.y"
    { yyval = &pars_to_binary_token; }
#line 1783 "pars0grm.cc"
    break;

  case 51:
#line 205 "pars0grm.y"
    { yyval = &pars_substr_token; }
#line 1789 "pars0grm.cc"
    break;

  case 52:
#line 206 "pars0grm.y"
    { yyval = &pars_concat_token; }
#line 1795 "pars0grm.cc"
    break;

  case 53:
#line 207 "pars0grm.y"
    { yyval = &pars_instr_token; }
#line 1801 "pars0grm.cc"
    break;

  case 54:
#line 208 "pars0grm.y"
    { yyval = &pars_length_token; }
#line 1807 "pars0grm.cc"
    break;

  case 58:
#line 219 "pars0grm.y"
    { yyval = pars_stored_procedure_call(
					static_cast<sym_node_t*>(yyvsp[-4])); }
#line 1814 "pars0grm.cc"
    break;

  case 59:
#line 224 "pars0grm.y"
    { yyval = yyvsp[-2]; }
#line 1820 "pars0grm.cc"
    break;

  case 60:
#line 228 "pars0grm.y"
    { yyval = que_node_list_add_last(NULL, yyvsp[0]); }
#line 1826 "pars0grm.cc"
    break;

  case 61:
#line 230 "pars0grm.y"
    { yyval = que_node_list_add_last(yyvsp[-2], yyvsp[0]); }
#line 1832 "pars0grm.cc"
    break;

  case 62:
#line 234 "pars0grm.y"
    { yyval = NULL; }
#line 1838 "pars0grm.cc"
    break;

  case 63:
#line 235 "pars0grm.y"
    { yyval = que_node_list_add_last(NULL, yyvsp[0]); }
#line 1844 "pars0grm.cc"
    break;

  case 64:
#line 237 "pars0grm.y"
    { yyval = que_node_list_add_last(yyvsp[-2], yyvsp[0]); }
#line 1850 "pars0grm.cc"
    break;

  case 65:
#line 241 "pars0grm.y"
    { yyval = NULL; }
#line 1856 "pars0grm.cc"
    break;

  case 66:
#line 242 "pars0grm.y"
    { yyval = que_node_list_add_last(NULL, yyvsp[0]);}
#line 1862 "pars0grm.cc"
    break;

  case 67:
#line 243 "pars0grm.y"
    { yyval = que_node_list_add_last(yyvsp[-2], yyvsp[0]); }
#line 1868 "pars0grm.cc"
    break;

  case 68:
#line 247 "pars0grm.y"
    { yyval = yyvsp[0]; }
#line 1874 "pars0grm.cc"
    break;

  case 69:
#line 249 "pars0grm.y"
    { yyval = pars_func(&pars_count_token,
					  que_node_list_add_last(NULL,
					    sym_tab_add_int_lit(
						pars_sym_tab_global, 1))); }
#line 1883 "pars0grm.cc"
    break;

  case 70:
#line 256 "pars0grm.y"
    { yyval = NULL; }
#line 1889 "pars0grm.cc"
    break;

  case 71:
#line 257 "pars0grm.y"
    { yyval = que_node_list_add_last(NULL, yyvsp[0]); }
#line 1895 "pars0grm.cc"
    break;

  case 72:
#line 259 "pars0grm.y"
    { yyval = que_node_list_add_last(yyvsp[-2], yyvsp[0]); }
#line 1901 "pars0grm.cc"
    break;

  case 73:
#line 263 "pars0grm.y"
    { yyval = pars_select_list(&pars_star_denoter,
								NULL); }
#line 1908 "pars0grm.cc"
    break;

  case 74:
#line 266 "pars0grm.y"
    { yyval = pars_select_list(
					yyvsp[-2], static_cast<sym_node_t*>(yyvsp[0])); }
#line 1915 "pars0grm.cc"
    break;

  case 75:
#line 268 "pars0grm.y"
    { yyval = pars_select_list(yyvsp[0], NULL); }
#line 1921 "pars0grm.cc"
    break;

  case 76:
#line 272 "pars0grm.y"
    { yyval = NULL; }
#line 1927 "pars0grm.cc"
    break;

  case 77:
#line 273 "pars0grm.y"
    { yyval = yyvsp[0]; }
#line 1933 "pars0grm.cc"
    break;

  case 78:
#line 277 "pars0grm.y"
    { yyval = NULL; }
#line 1939 "pars0grm.cc"
    break;

  case 79:
#line 279 "pars0grm.y"
    { yyval = &pars_update_token; }
#line 1945 "pars0grm.cc"
    break;

  case 80:
#line 283 "pars0grm.y"
    { yyval = NULL; }
#line 1951 "pars0grm.cc"
    break;

  case 81:
#line 285 "pars0grm.y"
    { yyval = &pars_share_token; }
#line 1957 "pars0grm.cc"
    break;

  case 82:
#line 289 "pars0grm.y"
    { yyval = &pars_asc_token; }
#line 1963 "pars0grm.cc"
    break;

  case 83:
#line 290 "pars0grm.y"
    { yyval = &pars_asc_token; }
#line 1969 "pars0grm.cc"
    break;

  case 84:
#line 291 "pars0grm.y"
    { yyval = &pars_desc_token; }
#line 1975 "pars0grm.cc"
    break;

  case 85:
#line 295 "pars0grm.y"
    { yyval = NULL; }
#line 1981 "pars0grm.cc"
    break;

  case 86:
#line 297 "pars0grm.y"
    { yyval = pars_order_by(
					static_cast<sym_node_t*>(yyvsp[-1]),
					static_cast<pars_res_word_t*>(yyvsp[0])); }
#line 1989 "pars0grm.cc"
    break;

  case 87:
#line 308 "pars0grm.y"
    { yyval = pars_select_statement(
					static_cast<sel_node_t*>(yyvsp[-6]),
					static_cast<sym_node_t*>(yyvsp[-4]),
					static_cast<que_node_t*>(yyvsp[-3]),
					static_cast<pars_res_word_t*>(yyvsp[-2]),
					static_cast<pars_res_word_t*>(yyvsp[-1]),
					static_cast<order_node_t*>(yyvsp[0])); }
#line 2001 "pars0grm.cc"
    break;

  case 88:
#line 319 "pars0grm.y"
    { yyval = yyvsp[0]; }
#line 2007 "pars0grm.cc"
    break;

  case 89:
#line 324 "pars0grm.y"
    { if (!(yyval = pars_insert_statement(
					static_cast<sym_node_t*>(yyvsp[-4]), yyvsp[-1], NULL)))
					YYABORT; }
#line 2015 "pars0grm.cc"
    break;

  case 90:
#line 328 "pars0grm.y"
    { if (!(yyval = pars_insert_statement(
					static_cast<sym_node_t*>(yyvsp[-1]),
					NULL,
					static_cast<sel_node_t*>(yyvsp[0]))))
					YYABORT; }
#line 2025 "pars0grm.cc"
    break;

  case 91:
#line 336 "pars0grm.y"
    { yyval = pars_column_assignment(
					static_cast<sym_node_t*>(yyvsp[-2]),
					static_cast<que_node_t*>(yyvsp[0])); }
#line 2033 "pars0grm.cc"
    break;

  case 92:
#line 342 "pars0grm.y"
    { yyval = que_node_list_add_last(NULL, yyvsp[0]); }
#line 2039 "pars0grm.cc"
    break;

  case 93:
#line 344 "pars0grm.y"
    { yyval = que_node_list_add_last(yyvsp[-2], yyvsp[0]); }
#line 2045 "pars0grm.cc"
    break;

  case 94:
#line 350 "pars0grm.y"
    { yyval = yyvsp[0]; }
#line 2051 "pars0grm.cc"
    break;

  case 95:
#line 356 "pars0grm.y"
    { yyval = pars_update_statement_start(
					FALSE,
					static_cast<sym_node_t*>(yyvsp[-2]),
					static_cast<col_assign_node_t*>(yyvsp[0])); }
#line 2060 "pars0grm.cc"
    break;

  case 96:
#line 364 "pars0grm.y"
    { yyval = pars_update_statement(
					static_cast<upd_node_t*>(yyvsp[-1]),
					NULL,
					static_cast<que_node_t*>(yyvsp[0])); }
#line 2069 "pars0grm.cc"
    break;

  case 97:
#line 372 "pars0grm.y"
    { yyval = pars_update_statement(
					static_cast<upd_node_t*>(yyvsp[-1]),
					static_cast<sym_node_t*>(yyvsp[0]),
					NULL); }
#line 2078 "pars0grm.cc"
    break;

  case 98:
#line 380 "pars0grm.y"
    { yyval = pars_update_statement_start(
					TRUE,
					static_cast<sym_node_t*>(yyvsp[0]), NULL); }
#line 2086 "pars0grm.cc"
    break;

  case 99:
#line 387 "pars0grm.y"
    { yyval = pars_update_statement(
					static_cast<upd_node_t*>(yyvsp[-1]),
					NULL,
					static_cast<que_node_t*>(yyvsp[0])); }
#line 2095 "pars0grm.cc"
    break;

  case 100:
#line 395 "pars0grm.y"
    { yyval = pars_update_statement(
					static_cast<upd_node_t*>(yyvsp[-1]),
					static_cast<sym_node_t*>(yyvsp[0]),
					NULL); }
#line 2104 "pars0grm.cc"
    break;

  case 101:
#line 403 "pars0grm.y"
    { yyval = pars_assignment_statement(
					static_cast<sym_node_t*>(yyvsp[-2]),
					static_cast<que_node_t*>(yyvsp[0])); }
#line 2112 "pars0grm.cc"
    break;

  case 102:
#line 411 "pars0grm.y"
    { yyval = pars_elsif_element(yyvsp[-2], yyvsp[0]); }
#line 2118 "pars0grm.cc"
    break;

  case 103:
#line 415 "pars0grm.y"
    { yyval = que_node_list_add_last(NULL, yyvsp[0]); }
#line 2124 "pars0grm.cc"
    break;

  case 104:
#line 417 "pars0grm.y"
    { yyval = que_node_list_add_last(yyvsp[-1], yyvsp[0]); }
#line 2130 "pars0grm.cc"
    break;

  case 105:
#line 421 "pars0grm.y"
    { yyval = NULL; }
#line 2136 "pars0grm.cc"
    break;

  case 106:
#line 423 "pars0grm.y"
    { yyval = yyvsp[0]; }
#line 2142 "pars0grm.cc"
    break;

  case 107:
#line 424 "pars0grm.y"
    { yyval = yyvsp[0]; }
#line 2148 "pars0grm.cc"
    break;

  case 108:
#line 431 "pars0grm.y"
    { yyval = pars_if_statement(yyvsp[-5], yyvsp[-3], yyvsp[-2]); }
#line 2154 "pars0grm.cc"
    break;

  case 109:
#line 437 "pars0grm.y"
    { yyval = pars_while_statement(yyvsp[-4], yyvsp[-2]); }
#line 2160 "pars0grm.cc"
    break;

  case 110:
#line 445 "pars0grm.y"
    { yyval = pars_for_statement(
					static_cast<sym_node_t*>(yyvsp[-8]),
					yyvsp[-6], yyvsp[-4], yyvsp[-2]); }
#line 2168 "pars0grm.cc"
    break;

  case 111:
#line 451 "pars0grm.y"
    { yyval = pars_exit_statement(); }
#line 2174 "pars0grm.cc"
    break;

  case 112:
#line 455 "pars0grm.y"
    { yyval = pars_return_statement(); }
#line 2180 "pars0grm.cc"
    break;

  case 113:
#line 460 "pars0grm.y"
    { yyval = pars_open_statement(
						ROW_SEL_OPEN_CURSOR,
						static_cast<sym_node_t*>(yyvsp[0])); }
#line 2188 "pars0grm.cc"
    break;

  case 114:
#line 467 "pars0grm.y"
    { yyval = pars_open_statement(
						ROW_SEL_CLOSE_CURSOR,
						static_cast<sym_node_t*>(yyvsp[0])); }
#line 2196 "pars0grm.cc"
    break;

  case 115:
#line 474 "pars0grm.y"
    { yyval = pars_fetch_statement(
					static_cast<sym_node_t*>(yyvsp[-2]),
					static_cast<sym_node_t*>(yyvsp[0]), NULL); }
#line 2204 "pars0grm.cc"
    break;

  case 116:
#line 478 "pars0grm.y"
    { yyval = pars_fetch_statement(
					static_cast<sym_node_t*>(yyvsp[-3]),
					static_cast<sym_node_t*>(yyvsp[0]),
					static_cast<sym_node_t*>(yyvsp[-1])); }
#line 2213 "pars0grm.cc"
    break;

  case 117:
#line 486 "pars0grm.y"
    { yyval = pars_column_def(
					static_cast<sym_node_t*>(yyvsp[-3]),
					static_cast<pars_res_word_t*>(yyvsp[-2]),
					static_cast<sym_node_t*>(yyvsp[-1]),
					yyvsp[0]); }
#line 2223 "pars0grm.cc"
    break;

  case 118:
#line 494 "pars0grm.y"
    { yyval = que_node_list_add_last(NULL, yyvsp[0]); }
#line 2229 "pars0grm.cc"
    break;

  case 119:
#line 496 "pars0grm.y"
    { yyval = que_node_list_add_last(yyvsp[-2], yyvsp[0]); }
#line 2235 "pars0grm.cc"
    break;

  case 120:
#line 500 "pars0grm.y"
    { yyval = NULL; }
#line 2241 "pars0grm.cc"
    break;

  case 121:
#line 502 "pars0grm.y"
    { yyval = yyvsp[-1]; }
#line 2247 "pars0grm.cc"
    break;

  case 122:
#line 506 "pars0grm.y"
    { yyval = NULL; }
#line 2253 "pars0grm.cc"
    break;

  case 123:
#line 508 "pars0grm.y"
    { yyval = &pars_int_token;
					/* pass any non-NULL pointer */ }
#line 2260 "pars0grm.cc"
    break;

  case 124:
#line 515 "pars0grm.y"
    { yyval = pars_create_table(
					static_cast<sym_node_t*>(yyvsp[-3]),
					static_cast<sym_node_t*>(yyvsp[-1])); }
#line 2268 "pars0grm.cc"
    break;

  case 125:
#line 521 "pars0grm.y"
    { yyval = que_node_list_add_last(NULL, yyvsp[0]); }
#line 2274 "pars0grm.cc"
    break;

  case 126:
#line 523 "pars0grm.y"
    { yyval = que_node_list_add_last(yyvsp[-2], yyvsp[0]); }
#line 2280 "pars0grm.cc"
    break;

  case 127:
#line 527 "pars0grm.y"
    { yyval = NULL; }
#line 2286 "pars0grm.cc"
    break;

  case 128:
#line 528 "pars0grm.y"
    { yyval = &pars_unique_token; }
#line 2292 "pars0grm.cc"
    break;

  case 129:
#line 532 "pars0grm.y"
    { yyval = NULL; }
#line 2298 "pars0grm.cc"
    break;

  case 130:
#line 533 "pars0grm.y"
    { yyval = &pars_clustered_token; }
#line 2304 "pars0grm.cc"
    break;

  case 131:
#line 542 "pars0grm.y"
    { yyval = pars_create_index(
					static_cast<pars_res_word_t*>(yyvsp[-8]),
					static_cast<pars_res_word_t*>(yyvsp[-7]),
					static_cast<sym_node_t*>(yyvsp[-5]),
					static_cast<sym_node_t*>(yyvsp[-3]),
					static_cast<sym_node_t*>(yyvsp[-1])); }
#line 2315 "pars0grm.cc"
    break;

  case 132:
#line 551 "pars0grm.y"
    { yyval = yyvsp[0]; }
#line 2321 "pars0grm.cc"
    break;

  case 133:
#line 552 "pars0grm.y"
    { yyval = yyvsp[0]; }
#line 2327 "pars0grm.cc"
    break;

  case 134:
#line 557 "pars0grm.y"
    { yyval = pars_commit_statement(); }
#line 2333 "pars0grm.cc"
    break;

  case 135:
#line 562 "pars0grm.y"
    { yyval = pars_rollback_statement(); }
#line 2339 "pars0grm.cc"
    break;

  case 136:
#line 566 "pars0grm.y"
    { yyval = &pars_int_token; }
#line 2345 "pars0grm.cc"
    break;

  case 137:
#line 567 "pars0grm.y"
    { yyval = &pars_bigint_token; }
#line 2351 "pars0grm.cc"
    break;

  case 138:
#line 568 "pars0grm.y"
    { yyval = &pars_char_token; }
#line 2357 "pars0grm.cc"
    break;

  case 139:
#line 573 "pars0grm.y"
    { yyval = pars_variable_declaration(
					static_cast<sym_node_t*>(yyvsp[-2]),
					static_cast<pars_res_word_t*>(yyvsp[-1])); }
#line 2365 "pars0grm.cc"
    break;

  case 143:
#line 587 "pars0grm.y"
    { yyval = pars_cursor_declaration(
					static_cast<sym_node_t*>(yyvsp[-3]),
					static_cast<sel_node_t*>(yyvsp[-1])); }
#line 2373 "pars0grm.cc"
    break;

  case 144:
#line 594 "pars0grm.y"
    { yyval = pars_function_declaration(
					static_cast<sym_node_t*>(yyvsp[-1])); }
#line 2380 "pars0grm.cc"
    break;

  case 150:
#line 616 "pars0grm.y"
    { yyval = pars_procedure_definition(
					static_cast<sym_node_t*>(yyvsp[-8]), yyvsp[-1]); }
#line 2387 "pars0grm.cc"
    break;


#line 2391 "pars0grm.cc"

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
  {
    const int yylhs = yyr1[yyn] - YYNTOKENS;
    const int yyi = yypgoto[yylhs] + *yyssp;
    yystate = (0 <= yyi && yyi <= YYLAST && yycheck[yyi] == *yyssp
               ? yytable[yyi]
               : yydefgoto[yylhs]);
  }

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
  /* Pacify compilers when the user code never invokes YYERROR and the
     label yyerrorlab therefore never appears in user code.  */
  if (0)
    YYERROR;

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


/*-----------------------------------------------------.
| yyreturn -- parsing is finished, return the result.  |
`-----------------------------------------------------*/
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
#line 620 "pars0grm.y"

