/* A Bison parser, made by GNU Bison 3.4.2.  */

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
#define YYBISON_VERSION "3.4.2"

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

#if defined __GNUC__ && (!defined __clang_major__ || __clang_major__ > 11)
#pragma GCC diagnostic ignored "-Wfree-nonheap-object"
#endif

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
#define YYLAST   603

/* YYNTOKENS -- Number of terminals.  */
#define YYNTOKENS  96
/* YYNNTS -- Number of nonterminals.  */
#define YYNNTS  64
/* YYNRULES -- Number of rules.  */
#define YYNRULES  150
/* YYNSTATES -- Number of states.  */
#define YYNSTATES  300

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
     326,   334,   340,   341,   346,   352,   361,   369,   377,   384,
     392,   400,   407,   413,   414,   419,   420,   422,   426,   433,
     439,   449,   453,   457,   464,   471,   475,   483,   492,   493,
     498,   499,   504,   505,   511,   519,   520,   525,   526,   530,
     531,   535,   549,   550,   554,   559,   564,   565,   566,   570,
     576,   578,   579,   583,   591,   597,   598,   601,   603,   604,
     608
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

#define YYPACT_NINF -129

#define yypact_value_is_default(Yystate) \
  (!!((Yystate) == (-129)))

#define YYTABLE_NINF -1

#define yytable_value_is_error(Yytable_value) \
  0

  /* YYPACT[STATE-NUM] -- Index in YYTABLE of the portion describing
     STATE-NUM.  */
static const yytype_int16 yypact[] =
{
       5,    34,    46,   -28,   -41,  -129,  -129,   -12,    45,    57,
      23,  -129,     9,  -129,  -129,  -129,    20,    -9,  -129,  -129,
    -129,  -129,     2,  -129,    83,    87,   278,  -129,    93,    28,
      71,   427,   427,  -129,   335,   105,    85,    -1,   104,   -27,
     129,   132,   133,    76,    77,  -129,   141,  -129,   149,  -129,
      61,    19,    62,   118,    65,    66,   118,    68,    69,    70,
      72,    73,    74,    75,    78,    79,    82,    84,    89,    90,
      91,    94,   138,  -129,   427,  -129,  -129,  -129,  -129,    86,
     427,    96,  -129,  -129,  -129,  -129,  -129,   427,   427,   438,
      92,   454,    95,  -129,     1,  -129,   -24,   130,   157,    -1,
    -129,  -129,   144,    -1,    -1,  -129,   139,  -129,   154,  -129,
    -129,  -129,    98,  -129,  -129,  -129,   108,  -129,  -129,   345,
    -129,  -129,  -129,  -129,  -129,  -129,  -129,  -129,  -129,  -129,
    -129,  -129,  -129,  -129,  -129,  -129,  -129,  -129,  -129,  -129,
    -129,   112,     1,   135,   285,   143,    -8,    15,   427,   427,
     427,   427,   427,   278,   203,   427,   427,   427,   427,   427,
     427,   427,   427,   278,   124,   204,   381,    -1,   427,  -129,
     209,  -129,   120,  -129,   173,   215,   131,   427,   180,     1,
    -129,  -129,  -129,  -129,   285,   285,    30,    30,     1,    10,
    -129,    30,    30,    30,    60,    60,    -8,    -8,     1,   -39,
     192,   137,  -129,   136,  -129,   -13,  -129,   472,   146,  -129,
     147,   225,   227,   151,  -129,   136,  -129,   -21,     0,   229,
     278,   427,  -129,   213,   219,  -129,   427,   220,  -129,   237,
     427,    -1,   214,   427,   427,   209,    23,  -129,    14,   196,
     160,   158,   162,  -129,  -129,   278,   486,  -129,   231,     1,
    -129,  -129,  -129,   218,   194,   517,     1,  -129,   175,  -129,
     225,    -1,  -129,  -129,  -129,   278,  -129,  -129,   251,   234,
     278,   266,   260,  -129,   181,   278,   201,   239,  -129,   235,
     184,   271,  -129,   272,   208,   275,   258,  -129,  -129,  -129,
      17,  -129,    -7,  -129,  -129,   277,  -129,  -129,  -129,  -129
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
      95,     0,     0,    63,   116,   115,    56,     0,     0,     0,
       0,     0,   103,   107,     0,    26,     0,     0,    69,     0,
       0,     0,    78,     0,     0,     0,     0,   118,     0,     0,
       0,     0,     0,    89,    94,   106,     0,   104,     0,    67,
     109,    64,    61,     0,    80,     0,    91,    93,   120,   124,
       0,     0,    59,    58,    57,     0,   108,    79,     0,    85,
       0,     0,   122,   119,     0,   102,     0,     0,    87,     0,
       0,     0,   117,     0,     0,     0,     0,   121,   123,   125,
       0,    81,    82,   110,   131,     0,    83,    84,    86,   126
};

  /* YYPGOTO[NTERM-NUM].  */
static const yytype_int16 yypgoto[] =
{
    -129,  -129,   -48,  -128,   -30,  -129,  -129,  -129,  -129,  -129,
     113,   110,   123,  -129,  -129,   -52,  -129,  -129,  -129,  -129,
     -40,  -129,  -129,    55,  -129,   238,  -129,  -129,  -129,  -129,
    -129,  -129,  -129,    88,  -129,  -129,  -129,  -129,  -129,  -129,
    -129,  -129,  -129,  -129,    35,  -129,  -129,  -129,  -129,  -129,
    -129,  -129,  -129,   -96,  -129,  -129,    81,   290,  -129,  -129,
    -129,   286,  -129,  -129
};

  /* YYDEFGOTO[NTERM-NUM].  */
static const yytype_int16 yydefgoto[] =
{
      -1,     2,    47,    48,    94,    90,   217,    49,   214,   205,
     203,   199,    95,    96,    97,   120,   254,   269,   298,   278,
      50,    51,    52,   209,   210,   121,    53,    54,    55,    56,
      57,    58,    59,   222,   223,   224,    60,    61,    62,    63,
      64,    65,    66,    67,   237,   238,   272,   282,    68,   290,
     106,   174,    69,   102,    70,    71,    16,    11,    12,    19,
      20,    21,    22,     3
};

  /* YYTABLE[YYPACT[STATE-NUM]] -- What to do in state STATE-NUM.  If
     positive, shift that token.  If negative, reduce the rule whose
     number is the opposite.  If YYTABLE_NINF, syntax error.  */
static const yytype_uint16 yytable[] =
{
     114,    89,    91,   169,   124,   152,   100,   171,   172,   148,
     149,   117,   150,   151,   152,   165,    10,    30,   230,     1,
     104,    26,   105,   148,   149,   189,   150,   151,   152,   296,
     297,    31,   141,   220,   221,   200,    32,    33,    34,    13,
      14,     4,    35,   152,   142,    24,     5,    34,    36,     7,
     144,    37,   225,    38,   226,    17,    39,   146,   147,   116,
      25,     6,    17,     9,    10,   154,    40,    41,    42,   166,
     241,   206,   242,   152,   154,    43,    44,   101,    45,     8,
     231,   155,   156,   157,   158,   159,   160,   161,   154,   179,
      28,   243,   245,   226,    29,   155,   156,   157,   158,   159,
     160,   161,    15,   154,    46,   259,   183,   260,   294,    23,
     295,    72,    98,   158,   159,   160,   161,    73,   184,   185,
     186,   187,   188,    74,    99,   191,   192,   193,   194,   195,
     196,   197,   198,   154,   103,   252,   107,   275,   207,   108,
     109,   114,   279,   110,   111,   160,   161,   198,   112,   119,
     115,   118,   114,   232,   122,   123,    30,   126,   127,   128,
     167,   129,   130,   131,   132,   274,    34,   133,   134,   113,
      31,   135,   168,   136,   143,    32,    33,    34,   137,   138,
     139,    35,   162,   140,   145,   164,   170,    36,   176,   173,
      37,   246,    38,   175,   181,    39,   249,   114,   177,    30,
     179,   180,   182,   255,   256,    40,    41,    42,   190,   201,
     211,   202,   227,    31,    43,    44,   208,    45,    32,    33,
      34,   212,   213,   216,    35,   219,   234,   114,   228,   229,
      36,   114,   236,    37,   239,    38,   244,   221,    39,   248,
     235,   240,    30,    46,   251,   250,   253,   261,    40,    41,
      42,   262,   266,   263,   264,   286,    31,    43,    44,   267,
      45,    32,    33,    34,   268,   271,   276,    35,   277,   280,
     281,   283,   284,    36,   285,   287,    37,   288,    38,   289,
     291,    39,   292,   293,   299,    30,    46,   218,   215,   204,
     257,    40,    41,    42,   125,   273,   150,   151,   152,    31,
      43,    44,    18,    45,    32,    33,    34,     0,    27,     0,
      35,   247,     0,     0,     0,     0,    36,   258,     0,    37,
       0,    38,     0,     0,    39,     0,     0,     0,     0,    46,
       0,     0,     0,     0,    40,    41,    42,     0,    75,    76,
      77,    78,    79,    43,    44,    80,    45,     0,    75,    76,
      77,    78,    79,     0,     0,    80,     0,     0,   154,     0,
       0,     0,     0,     0,    92,   155,   156,   157,   158,   159,
     160,   161,    46,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,    75,    76,    77,    78,    79,   178,
      81,    80,     0,     0,     0,    82,    83,    84,    85,    86,
      81,     0,     0,     0,     0,    82,    83,    84,    85,    86,
      92,     0,     0,     0,     0,     0,     0,     0,    87,     0,
      93,     0,     0,     0,     0,    88,     0,     0,    87,     0,
      75,    76,    77,    78,    79,    88,    81,    80,     0,     0,
       0,    82,    83,    84,    85,    86,   148,   149,     0,   150,
     151,   152,     0,     0,     0,     0,     0,     0,     0,     0,
     153,     0,   148,   149,    87,   150,   151,   152,     0,     0,
       0,    88,     0,     0,     0,     0,     0,     0,     0,   163,
     148,   149,    81,   150,   151,   152,     0,    82,    83,    84,
      85,    86,     0,     0,   148,   149,     0,   150,   151,   152,
       0,     0,     0,     0,     0,   233,     0,     0,   265,     0,
      87,   154,     0,     0,     0,     0,     0,    88,   155,   156,
     157,   158,   159,   160,   161,   148,   149,   154,   150,   151,
     152,     0,     0,     0,   155,   156,   157,   158,   159,   160,
     161,     0,   270,     0,     0,   154,     0,     0,     0,     0,
       0,     0,   155,   156,   157,   158,   159,   160,   161,   154,
       0,     0,     0,     0,     0,     0,   155,   156,   157,   158,
     159,   160,   161,     0,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     154,     0,     0,     0,     0,     0,     0,   155,   156,   157,
     158,   159,   160,   161
};

static const yytype_int16 yycheck[] =
{
      48,    31,    32,    99,    56,    13,     7,   103,   104,     8,
       9,    51,    11,    12,    13,    39,     7,     7,    31,    14,
      47,    19,    49,     8,     9,   153,    11,    12,    13,    36,
      37,    21,    72,    23,    24,   163,    26,    27,    28,    16,
      17,     7,    32,    13,    74,    54,     0,    28,    38,    90,
      80,    41,    91,    43,    93,    53,    46,    87,    88,    40,
      69,    89,    53,    18,     7,    73,    56,    57,    58,    93,
      91,   167,    93,    13,    73,    65,    66,    78,    68,    91,
      93,    80,    81,    82,    83,    84,    85,    86,    73,   119,
       7,    91,   220,    93,     7,    80,    81,    82,    83,    84,
      85,    86,    79,    73,    94,    91,    91,    93,    91,    89,
      93,    18,     7,    83,    84,    85,    86,    89,   148,   149,
     150,   151,   152,    52,    39,   155,   156,   157,   158,   159,
     160,   161,   162,    73,    30,   231,     7,   265,   168,     7,
       7,   189,   270,    67,    67,    85,    86,   177,     7,    31,
      89,    89,   200,   205,    89,    89,     7,    89,    89,    89,
      30,    89,    89,    89,    89,   261,    28,    89,    89,    20,
      21,    89,    15,    89,    88,    26,    27,    28,    89,    89,
      89,    32,    90,    89,    88,    90,    42,    38,    90,    50,
      41,   221,    43,    39,    59,    46,   226,   245,    90,     7,
     230,    89,    59,   233,   234,    56,    57,    58,     5,    85,
      90,     7,    20,    21,    65,    66,     7,    68,    26,    27,
      28,    48,     7,    92,    32,    45,    80,   275,    91,    93,
      38,   279,     7,    41,     7,    43,     7,    24,    46,    20,
      93,    90,     7,    94,     7,    25,    32,    51,    56,    57,
      58,    91,    21,    95,    92,    20,    21,    65,    66,    41,
      68,    26,    27,    28,    70,    90,    15,    32,    34,     3,
      10,    90,    71,    38,    35,    91,    41,     6,    43,     7,
      72,    46,     7,    25,     7,     7,    94,   177,   175,   166,
     235,    56,    57,    58,    56,   260,    11,    12,    13,    21,
      65,    66,    12,    68,    26,    27,    28,    -1,    22,    -1,
      32,   223,    -1,    -1,    -1,    -1,    38,   236,    -1,    41,
      -1,    43,    -1,    -1,    46,    -1,    -1,    -1,    -1,    94,
      -1,    -1,    -1,    -1,    56,    57,    58,    -1,     3,     4,
       5,     6,     7,    65,    66,    10,    68,    -1,     3,     4,
       5,     6,     7,    -1,    -1,    10,    -1,    -1,    73,    -1,
      -1,    -1,    -1,    -1,    29,    80,    81,    82,    83,    84,
      85,    86,    94,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,     3,     4,     5,     6,     7,    44,
      55,    10,    -1,    -1,    -1,    60,    61,    62,    63,    64,
      55,    -1,    -1,    -1,    -1,    60,    61,    62,    63,    64,
      29,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    83,    -1,
      85,    -1,    -1,    -1,    -1,    90,    -1,    -1,    83,    -1,
       3,     4,     5,     6,     7,    90,    55,    10,    -1,    -1,
      -1,    60,    61,    62,    63,    64,     8,     9,    -1,    11,
      12,    13,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      22,    -1,     8,     9,    83,    11,    12,    13,    -1,    -1,
      -1,    90,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    25,
       8,     9,    55,    11,    12,    13,    -1,    60,    61,    62,
      63,    64,    -1,    -1,     8,     9,    -1,    11,    12,    13,
      -1,    -1,    -1,    -1,    -1,    33,    -1,    -1,    22,    -1,
      83,    73,    -1,    -1,    -1,    -1,    -1,    90,    80,    81,
      82,    83,    84,    85,    86,     8,     9,    73,    11,    12,
      13,    -1,    -1,    -1,    80,    81,    82,    83,    84,    85,
      86,    -1,    25,    -1,    -1,    73,    -1,    -1,    -1,    -1,
      -1,    -1,    80,    81,    82,    83,    84,    85,    86,    73,
      -1,    -1,    -1,    -1,    -1,    -1,    80,    81,    82,    83,
      84,    85,    86,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
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
      90,    91,    93,    91,     7,    99,   100,   129,    20,   100,
      25,     7,   149,    32,   112,   100,   100,   119,   152,    91,
      93,    51,    91,    95,    92,    22,    21,    41,    70,   113,
      25,    90,   142,   140,   149,    99,    15,    34,   115,    99,
       3,    10,   143,    90,    71,    35,    20,    91,     6,     7,
     145,    72,     7,    25,    91,    93,    36,    37,   114,     7
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
      10,     1,     1,     2,     2,     4,     4,     4,     1,     3,
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
  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  YYUSE (yytype);
  YY_IGNORE_MAYBE_UNINITIALIZED_END
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
#line 1616 "pars0grm.cc"
    break;

  case 24:
#line 168 "pars0grm.y"
    { yyval = que_node_list_add_last(yyvsp[-1], yyvsp[0]); }
#line 1622 "pars0grm.cc"
    break;

  case 25:
#line 172 "pars0grm.y"
    { yyval = yyvsp[0];}
#line 1628 "pars0grm.cc"
    break;

  case 26:
#line 174 "pars0grm.y"
    { yyval = pars_func(yyvsp[-3], yyvsp[-1]); }
#line 1634 "pars0grm.cc"
    break;

  case 27:
#line 175 "pars0grm.y"
    { yyval = yyvsp[0];}
#line 1640 "pars0grm.cc"
    break;

  case 28:
#line 176 "pars0grm.y"
    { yyval = yyvsp[0];}
#line 1646 "pars0grm.cc"
    break;

  case 29:
#line 177 "pars0grm.y"
    { yyval = yyvsp[0];}
#line 1652 "pars0grm.cc"
    break;

  case 30:
#line 178 "pars0grm.y"
    { yyval = yyvsp[0];}
#line 1658 "pars0grm.cc"
    break;

  case 31:
#line 179 "pars0grm.y"
    { yyval = yyvsp[0];}
#line 1664 "pars0grm.cc"
    break;

  case 32:
#line 180 "pars0grm.y"
    { yyval = pars_op('+', yyvsp[-2], yyvsp[0]); }
#line 1670 "pars0grm.cc"
    break;

  case 33:
#line 181 "pars0grm.y"
    { yyval = pars_op('-', yyvsp[-2], yyvsp[0]); }
#line 1676 "pars0grm.cc"
    break;

  case 34:
#line 182 "pars0grm.y"
    { yyval = pars_op('*', yyvsp[-2], yyvsp[0]); }
#line 1682 "pars0grm.cc"
    break;

  case 35:
#line 183 "pars0grm.y"
    { yyval = pars_op('/', yyvsp[-2], yyvsp[0]); }
#line 1688 "pars0grm.cc"
    break;

  case 36:
#line 184 "pars0grm.y"
    { yyval = pars_op('-', yyvsp[0], NULL); }
#line 1694 "pars0grm.cc"
    break;

  case 37:
#line 185 "pars0grm.y"
    { yyval = yyvsp[-1]; }
#line 1700 "pars0grm.cc"
    break;

  case 38:
#line 186 "pars0grm.y"
    { yyval = pars_op('=', yyvsp[-2], yyvsp[0]); }
#line 1706 "pars0grm.cc"
    break;

  case 39:
#line 188 "pars0grm.y"
    { yyval = pars_op(PARS_LIKE_TOKEN, yyvsp[-2], yyvsp[0]); }
#line 1712 "pars0grm.cc"
    break;

  case 40:
#line 189 "pars0grm.y"
    { yyval = pars_op('<', yyvsp[-2], yyvsp[0]); }
#line 1718 "pars0grm.cc"
    break;

  case 41:
#line 190 "pars0grm.y"
    { yyval = pars_op('>', yyvsp[-2], yyvsp[0]); }
#line 1724 "pars0grm.cc"
    break;

  case 42:
#line 191 "pars0grm.y"
    { yyval = pars_op(PARS_GE_TOKEN, yyvsp[-2], yyvsp[0]); }
#line 1730 "pars0grm.cc"
    break;

  case 43:
#line 192 "pars0grm.y"
    { yyval = pars_op(PARS_LE_TOKEN, yyvsp[-2], yyvsp[0]); }
#line 1736 "pars0grm.cc"
    break;

  case 44:
#line 193 "pars0grm.y"
    { yyval = pars_op(PARS_NE_TOKEN, yyvsp[-2], yyvsp[0]); }
#line 1742 "pars0grm.cc"
    break;

  case 45:
#line 194 "pars0grm.y"
    { yyval = pars_op(PARS_AND_TOKEN, yyvsp[-2], yyvsp[0]); }
#line 1748 "pars0grm.cc"
    break;

  case 46:
#line 195 "pars0grm.y"
    { yyval = pars_op(PARS_OR_TOKEN, yyvsp[-2], yyvsp[0]); }
#line 1754 "pars0grm.cc"
    break;

  case 47:
#line 196 "pars0grm.y"
    { yyval = pars_op(PARS_NOT_TOKEN, yyvsp[0], NULL); }
#line 1760 "pars0grm.cc"
    break;

  case 48:
#line 198 "pars0grm.y"
    { yyval = pars_op(PARS_NOTFOUND_TOKEN, yyvsp[-2], NULL); }
#line 1766 "pars0grm.cc"
    break;

  case 49:
#line 200 "pars0grm.y"
    { yyval = pars_op(PARS_NOTFOUND_TOKEN, yyvsp[-2], NULL); }
#line 1772 "pars0grm.cc"
    break;

  case 50:
#line 204 "pars0grm.y"
    { yyval = &pars_to_binary_token; }
#line 1778 "pars0grm.cc"
    break;

  case 51:
#line 205 "pars0grm.y"
    { yyval = &pars_substr_token; }
#line 1784 "pars0grm.cc"
    break;

  case 52:
#line 206 "pars0grm.y"
    { yyval = &pars_concat_token; }
#line 1790 "pars0grm.cc"
    break;

  case 53:
#line 207 "pars0grm.y"
    { yyval = &pars_instr_token; }
#line 1796 "pars0grm.cc"
    break;

  case 54:
#line 208 "pars0grm.y"
    { yyval = &pars_length_token; }
#line 1802 "pars0grm.cc"
    break;

  case 58:
#line 219 "pars0grm.y"
    { yyval = pars_stored_procedure_call(
					static_cast<sym_node_t*>(yyvsp[-4])); }
#line 1809 "pars0grm.cc"
    break;

  case 59:
#line 224 "pars0grm.y"
    { yyval = yyvsp[-2]; }
#line 1815 "pars0grm.cc"
    break;

  case 60:
#line 228 "pars0grm.y"
    { yyval = que_node_list_add_last(NULL, yyvsp[0]); }
#line 1821 "pars0grm.cc"
    break;

  case 61:
#line 230 "pars0grm.y"
    { yyval = que_node_list_add_last(yyvsp[-2], yyvsp[0]); }
#line 1827 "pars0grm.cc"
    break;

  case 62:
#line 234 "pars0grm.y"
    { yyval = NULL; }
#line 1833 "pars0grm.cc"
    break;

  case 63:
#line 235 "pars0grm.y"
    { yyval = que_node_list_add_last(NULL, yyvsp[0]); }
#line 1839 "pars0grm.cc"
    break;

  case 64:
#line 237 "pars0grm.y"
    { yyval = que_node_list_add_last(yyvsp[-2], yyvsp[0]); }
#line 1845 "pars0grm.cc"
    break;

  case 65:
#line 241 "pars0grm.y"
    { yyval = NULL; }
#line 1851 "pars0grm.cc"
    break;

  case 66:
#line 242 "pars0grm.y"
    { yyval = que_node_list_add_last(NULL, yyvsp[0]);}
#line 1857 "pars0grm.cc"
    break;

  case 67:
#line 243 "pars0grm.y"
    { yyval = que_node_list_add_last(yyvsp[-2], yyvsp[0]); }
#line 1863 "pars0grm.cc"
    break;

  case 68:
#line 247 "pars0grm.y"
    { yyval = yyvsp[0]; }
#line 1869 "pars0grm.cc"
    break;

  case 69:
#line 249 "pars0grm.y"
    { yyval = pars_func(&pars_count_token,
					  que_node_list_add_last(NULL,
					    sym_tab_add_int_lit(
						pars_sym_tab_global, 1))); }
#line 1878 "pars0grm.cc"
    break;

  case 70:
#line 256 "pars0grm.y"
    { yyval = NULL; }
#line 1884 "pars0grm.cc"
    break;

  case 71:
#line 257 "pars0grm.y"
    { yyval = que_node_list_add_last(NULL, yyvsp[0]); }
#line 1890 "pars0grm.cc"
    break;

  case 72:
#line 259 "pars0grm.y"
    { yyval = que_node_list_add_last(yyvsp[-2], yyvsp[0]); }
#line 1896 "pars0grm.cc"
    break;

  case 73:
#line 263 "pars0grm.y"
    { yyval = pars_select_list(&pars_star_denoter,
								NULL); }
#line 1903 "pars0grm.cc"
    break;

  case 74:
#line 266 "pars0grm.y"
    { yyval = pars_select_list(
					yyvsp[-2], static_cast<sym_node_t*>(yyvsp[0])); }
#line 1910 "pars0grm.cc"
    break;

  case 75:
#line 268 "pars0grm.y"
    { yyval = pars_select_list(yyvsp[0], NULL); }
#line 1916 "pars0grm.cc"
    break;

  case 76:
#line 272 "pars0grm.y"
    { yyval = NULL; }
#line 1922 "pars0grm.cc"
    break;

  case 77:
#line 273 "pars0grm.y"
    { yyval = yyvsp[0]; }
#line 1928 "pars0grm.cc"
    break;

  case 78:
#line 277 "pars0grm.y"
    { yyval = NULL; }
#line 1934 "pars0grm.cc"
    break;

  case 79:
#line 279 "pars0grm.y"
    { yyval = &pars_update_token; }
#line 1940 "pars0grm.cc"
    break;

  case 80:
#line 283 "pars0grm.y"
    { yyval = NULL; }
#line 1946 "pars0grm.cc"
    break;

  case 81:
#line 285 "pars0grm.y"
    { yyval = &pars_share_token; }
#line 1952 "pars0grm.cc"
    break;

  case 82:
#line 289 "pars0grm.y"
    { yyval = &pars_asc_token; }
#line 1958 "pars0grm.cc"
    break;

  case 83:
#line 290 "pars0grm.y"
    { yyval = &pars_asc_token; }
#line 1964 "pars0grm.cc"
    break;

  case 84:
#line 291 "pars0grm.y"
    { yyval = &pars_desc_token; }
#line 1970 "pars0grm.cc"
    break;

  case 85:
#line 295 "pars0grm.y"
    { yyval = NULL; }
#line 1976 "pars0grm.cc"
    break;

  case 86:
#line 297 "pars0grm.y"
    { yyval = pars_order_by(
					static_cast<sym_node_t*>(yyvsp[-1]),
					static_cast<pars_res_word_t*>(yyvsp[0])); }
#line 1984 "pars0grm.cc"
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
#line 1996 "pars0grm.cc"
    break;

  case 88:
#line 319 "pars0grm.y"
    { yyval = yyvsp[0]; }
#line 2002 "pars0grm.cc"
    break;

  case 89:
#line 324 "pars0grm.y"
    { yyval = pars_insert_statement(
					static_cast<sym_node_t*>(yyvsp[-4]), yyvsp[-1], NULL); }
#line 2009 "pars0grm.cc"
    break;

  case 90:
#line 327 "pars0grm.y"
    { yyval = pars_insert_statement(
					static_cast<sym_node_t*>(yyvsp[-1]),
					NULL,
					static_cast<sel_node_t*>(yyvsp[0])); }
#line 2018 "pars0grm.cc"
    break;

  case 91:
#line 334 "pars0grm.y"
    { yyval = pars_column_assignment(
					static_cast<sym_node_t*>(yyvsp[-2]),
					static_cast<que_node_t*>(yyvsp[0])); }
#line 2026 "pars0grm.cc"
    break;

  case 92:
#line 340 "pars0grm.y"
    { yyval = que_node_list_add_last(NULL, yyvsp[0]); }
#line 2032 "pars0grm.cc"
    break;

  case 93:
#line 342 "pars0grm.y"
    { yyval = que_node_list_add_last(yyvsp[-2], yyvsp[0]); }
#line 2038 "pars0grm.cc"
    break;

  case 94:
#line 348 "pars0grm.y"
    { yyval = yyvsp[0]; }
#line 2044 "pars0grm.cc"
    break;

  case 95:
#line 354 "pars0grm.y"
    { yyval = pars_update_statement_start(
					FALSE,
					static_cast<sym_node_t*>(yyvsp[-2]),
					static_cast<col_assign_node_t*>(yyvsp[0])); }
#line 2053 "pars0grm.cc"
    break;

  case 96:
#line 362 "pars0grm.y"
    { yyval = pars_update_statement(
					static_cast<upd_node_t*>(yyvsp[-1]),
					NULL,
					static_cast<que_node_t*>(yyvsp[0])); }
#line 2062 "pars0grm.cc"
    break;

  case 97:
#line 370 "pars0grm.y"
    { yyval = pars_update_statement(
					static_cast<upd_node_t*>(yyvsp[-1]),
					static_cast<sym_node_t*>(yyvsp[0]),
					NULL); }
#line 2071 "pars0grm.cc"
    break;

  case 98:
#line 378 "pars0grm.y"
    { yyval = pars_update_statement_start(
					TRUE,
					static_cast<sym_node_t*>(yyvsp[0]), NULL); }
#line 2079 "pars0grm.cc"
    break;

  case 99:
#line 385 "pars0grm.y"
    { yyval = pars_update_statement(
					static_cast<upd_node_t*>(yyvsp[-1]),
					NULL,
					static_cast<que_node_t*>(yyvsp[0])); }
#line 2088 "pars0grm.cc"
    break;

  case 100:
#line 393 "pars0grm.y"
    { yyval = pars_update_statement(
					static_cast<upd_node_t*>(yyvsp[-1]),
					static_cast<sym_node_t*>(yyvsp[0]),
					NULL); }
#line 2097 "pars0grm.cc"
    break;

  case 101:
#line 401 "pars0grm.y"
    { yyval = pars_assignment_statement(
					static_cast<sym_node_t*>(yyvsp[-2]),
					static_cast<que_node_t*>(yyvsp[0])); }
#line 2105 "pars0grm.cc"
    break;

  case 102:
#line 409 "pars0grm.y"
    { yyval = pars_elsif_element(yyvsp[-2], yyvsp[0]); }
#line 2111 "pars0grm.cc"
    break;

  case 103:
#line 413 "pars0grm.y"
    { yyval = que_node_list_add_last(NULL, yyvsp[0]); }
#line 2117 "pars0grm.cc"
    break;

  case 104:
#line 415 "pars0grm.y"
    { yyval = que_node_list_add_last(yyvsp[-1], yyvsp[0]); }
#line 2123 "pars0grm.cc"
    break;

  case 105:
#line 419 "pars0grm.y"
    { yyval = NULL; }
#line 2129 "pars0grm.cc"
    break;

  case 106:
#line 421 "pars0grm.y"
    { yyval = yyvsp[0]; }
#line 2135 "pars0grm.cc"
    break;

  case 107:
#line 422 "pars0grm.y"
    { yyval = yyvsp[0]; }
#line 2141 "pars0grm.cc"
    break;

  case 108:
#line 429 "pars0grm.y"
    { yyval = pars_if_statement(yyvsp[-5], yyvsp[-3], yyvsp[-2]); }
#line 2147 "pars0grm.cc"
    break;

  case 109:
#line 435 "pars0grm.y"
    { yyval = pars_while_statement(yyvsp[-4], yyvsp[-2]); }
#line 2153 "pars0grm.cc"
    break;

  case 110:
#line 443 "pars0grm.y"
    { yyval = pars_for_statement(
					static_cast<sym_node_t*>(yyvsp[-8]),
					yyvsp[-6], yyvsp[-4], yyvsp[-2]); }
#line 2161 "pars0grm.cc"
    break;

  case 111:
#line 449 "pars0grm.y"
    { yyval = pars_exit_statement(); }
#line 2167 "pars0grm.cc"
    break;

  case 112:
#line 453 "pars0grm.y"
    { yyval = pars_return_statement(); }
#line 2173 "pars0grm.cc"
    break;

  case 113:
#line 458 "pars0grm.y"
    { yyval = pars_open_statement(
						ROW_SEL_OPEN_CURSOR,
						static_cast<sym_node_t*>(yyvsp[0])); }
#line 2181 "pars0grm.cc"
    break;

  case 114:
#line 465 "pars0grm.y"
    { yyval = pars_open_statement(
						ROW_SEL_CLOSE_CURSOR,
						static_cast<sym_node_t*>(yyvsp[0])); }
#line 2189 "pars0grm.cc"
    break;

  case 115:
#line 472 "pars0grm.y"
    { yyval = pars_fetch_statement(
					static_cast<sym_node_t*>(yyvsp[-2]),
					static_cast<sym_node_t*>(yyvsp[0]), NULL); }
#line 2197 "pars0grm.cc"
    break;

  case 116:
#line 476 "pars0grm.y"
    { yyval = pars_fetch_statement(
					static_cast<sym_node_t*>(yyvsp[-2]),
					NULL,
					static_cast<sym_node_t*>(yyvsp[0])); }
#line 2206 "pars0grm.cc"
    break;

  case 117:
#line 484 "pars0grm.y"
    { yyval = pars_column_def(
					static_cast<sym_node_t*>(yyvsp[-3]),
					static_cast<pars_res_word_t*>(yyvsp[-2]),
					static_cast<sym_node_t*>(yyvsp[-1]),
					yyvsp[0]); }
#line 2216 "pars0grm.cc"
    break;

  case 118:
#line 492 "pars0grm.y"
    { yyval = que_node_list_add_last(NULL, yyvsp[0]); }
#line 2222 "pars0grm.cc"
    break;

  case 119:
#line 494 "pars0grm.y"
    { yyval = que_node_list_add_last(yyvsp[-2], yyvsp[0]); }
#line 2228 "pars0grm.cc"
    break;

  case 120:
#line 498 "pars0grm.y"
    { yyval = NULL; }
#line 2234 "pars0grm.cc"
    break;

  case 121:
#line 500 "pars0grm.y"
    { yyval = yyvsp[-1]; }
#line 2240 "pars0grm.cc"
    break;

  case 122:
#line 504 "pars0grm.y"
    { yyval = NULL; }
#line 2246 "pars0grm.cc"
    break;

  case 123:
#line 506 "pars0grm.y"
    { yyval = &pars_int_token;
					/* pass any non-NULL pointer */ }
#line 2253 "pars0grm.cc"
    break;

  case 124:
#line 513 "pars0grm.y"
    { yyval = pars_create_table(
					static_cast<sym_node_t*>(yyvsp[-3]),
					static_cast<sym_node_t*>(yyvsp[-1])); }
#line 2261 "pars0grm.cc"
    break;

  case 125:
#line 519 "pars0grm.y"
    { yyval = que_node_list_add_last(NULL, yyvsp[0]); }
#line 2267 "pars0grm.cc"
    break;

  case 126:
#line 521 "pars0grm.y"
    { yyval = que_node_list_add_last(yyvsp[-2], yyvsp[0]); }
#line 2273 "pars0grm.cc"
    break;

  case 127:
#line 525 "pars0grm.y"
    { yyval = NULL; }
#line 2279 "pars0grm.cc"
    break;

  case 128:
#line 526 "pars0grm.y"
    { yyval = &pars_unique_token; }
#line 2285 "pars0grm.cc"
    break;

  case 129:
#line 530 "pars0grm.y"
    { yyval = NULL; }
#line 2291 "pars0grm.cc"
    break;

  case 130:
#line 531 "pars0grm.y"
    { yyval = &pars_clustered_token; }
#line 2297 "pars0grm.cc"
    break;

  case 131:
#line 540 "pars0grm.y"
    { yyval = pars_create_index(
					static_cast<pars_res_word_t*>(yyvsp[-8]),
					static_cast<pars_res_word_t*>(yyvsp[-7]),
					static_cast<sym_node_t*>(yyvsp[-5]),
					static_cast<sym_node_t*>(yyvsp[-3]),
					static_cast<sym_node_t*>(yyvsp[-1])); }
#line 2308 "pars0grm.cc"
    break;

  case 132:
#line 549 "pars0grm.y"
    { yyval = yyvsp[0]; }
#line 2314 "pars0grm.cc"
    break;

  case 133:
#line 550 "pars0grm.y"
    { yyval = yyvsp[0]; }
#line 2320 "pars0grm.cc"
    break;

  case 134:
#line 555 "pars0grm.y"
    { yyval = pars_commit_statement(); }
#line 2326 "pars0grm.cc"
    break;

  case 135:
#line 560 "pars0grm.y"
    { yyval = pars_rollback_statement(); }
#line 2332 "pars0grm.cc"
    break;

  case 136:
#line 564 "pars0grm.y"
    { yyval = &pars_int_token; }
#line 2338 "pars0grm.cc"
    break;

  case 137:
#line 565 "pars0grm.y"
    { yyval = &pars_bigint_token; }
#line 2344 "pars0grm.cc"
    break;

  case 138:
#line 566 "pars0grm.y"
    { yyval = &pars_char_token; }
#line 2350 "pars0grm.cc"
    break;

  case 139:
#line 571 "pars0grm.y"
    { yyval = pars_variable_declaration(
					static_cast<sym_node_t*>(yyvsp[-2]),
					static_cast<pars_res_word_t*>(yyvsp[-1])); }
#line 2358 "pars0grm.cc"
    break;

  case 143:
#line 585 "pars0grm.y"
    { yyval = pars_cursor_declaration(
					static_cast<sym_node_t*>(yyvsp[-3]),
					static_cast<sel_node_t*>(yyvsp[-1])); }
#line 2366 "pars0grm.cc"
    break;

  case 144:
#line 592 "pars0grm.y"
    { yyval = pars_function_declaration(
					static_cast<sym_node_t*>(yyvsp[-1])); }
#line 2373 "pars0grm.cc"
    break;

  case 150:
#line 614 "pars0grm.y"
    { yyval = pars_procedure_definition(
					static_cast<sym_node_t*>(yyvsp[-8]), yyvsp[-1]); }
#line 2380 "pars0grm.cc"
    break;


#line 2384 "pars0grm.cc"

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
#line 618 "pars0grm.y"

