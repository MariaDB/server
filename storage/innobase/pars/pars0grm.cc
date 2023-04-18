/* A Bison parser, made by GNU Bison 3.7.6.  */

/* Bison implementation for Yacc-like parsers in C

   Copyright (C) 1984, 1989-1990, 2000-2015, 2018-2021 Free Software Foundation,
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
   along with this program.  If not, see <https://www.gnu.org/licenses/>.  */

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

/* DO NOT RELY ON FEATURES THAT ARE NOT DOCUMENTED in the manual,
   especially those whose name start with YY_ or yy_.  They are
   private implementation details that can be changed or removed.  */

/* All symbols defined below should begin with yy or YY, to avoid
   infringing on user name space.  This should be done even for local
   variables, as they might otherwise be expanded by user macros.
   There are some unavoidable exceptions within include files to
   define necessary library symbols; they are noted "INFRINGES ON
   USER NAME SPACE" below.  */

/* Identify Bison output, and Bison version.  */
#define YYBISON 30706

/* Bison version string.  */
#define YYBISON_VERSION "3.7.6"

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

#line 90 "pars0grm.cc"

# ifndef YY_CAST
#  ifdef __cplusplus
#   define YY_CAST(Type, Val) static_cast<Type> (Val)
#   define YY_REINTERPRET_CAST(Type, Val) reinterpret_cast<Type> (Val)
#  else
#   define YY_CAST(Type, Val) ((Type) (Val))
#   define YY_REINTERPRET_CAST(Type, Val) ((Type) (Val))
#  endif
# endif
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

#include "pars0grm.h"
/* Symbol kind.  */
enum yysymbol_kind_t
{
  YYSYMBOL_YYEMPTY = -2,
  YYSYMBOL_YYEOF = 0,                      /* "end of file"  */
  YYSYMBOL_YYerror = 1,                    /* error  */
  YYSYMBOL_YYUNDEF = 2,                    /* "invalid token"  */
  YYSYMBOL_PARS_INT_LIT = 3,               /* PARS_INT_LIT  */
  YYSYMBOL_PARS_FLOAT_LIT = 4,             /* PARS_FLOAT_LIT  */
  YYSYMBOL_PARS_STR_LIT = 5,               /* PARS_STR_LIT  */
  YYSYMBOL_PARS_NULL_LIT = 6,              /* PARS_NULL_LIT  */
  YYSYMBOL_PARS_ID_TOKEN = 7,              /* PARS_ID_TOKEN  */
  YYSYMBOL_PARS_AND_TOKEN = 8,             /* PARS_AND_TOKEN  */
  YYSYMBOL_PARS_OR_TOKEN = 9,              /* PARS_OR_TOKEN  */
  YYSYMBOL_PARS_NOT_TOKEN = 10,            /* PARS_NOT_TOKEN  */
  YYSYMBOL_PARS_GE_TOKEN = 11,             /* PARS_GE_TOKEN  */
  YYSYMBOL_PARS_LE_TOKEN = 12,             /* PARS_LE_TOKEN  */
  YYSYMBOL_PARS_NE_TOKEN = 13,             /* PARS_NE_TOKEN  */
  YYSYMBOL_PARS_PROCEDURE_TOKEN = 14,      /* PARS_PROCEDURE_TOKEN  */
  YYSYMBOL_PARS_IN_TOKEN = 15,             /* PARS_IN_TOKEN  */
  YYSYMBOL_PARS_INT_TOKEN = 16,            /* PARS_INT_TOKEN  */
  YYSYMBOL_PARS_CHAR_TOKEN = 17,           /* PARS_CHAR_TOKEN  */
  YYSYMBOL_PARS_IS_TOKEN = 18,             /* PARS_IS_TOKEN  */
  YYSYMBOL_PARS_BEGIN_TOKEN = 19,          /* PARS_BEGIN_TOKEN  */
  YYSYMBOL_PARS_END_TOKEN = 20,            /* PARS_END_TOKEN  */
  YYSYMBOL_PARS_IF_TOKEN = 21,             /* PARS_IF_TOKEN  */
  YYSYMBOL_PARS_THEN_TOKEN = 22,           /* PARS_THEN_TOKEN  */
  YYSYMBOL_PARS_ELSE_TOKEN = 23,           /* PARS_ELSE_TOKEN  */
  YYSYMBOL_PARS_ELSIF_TOKEN = 24,          /* PARS_ELSIF_TOKEN  */
  YYSYMBOL_PARS_LOOP_TOKEN = 25,           /* PARS_LOOP_TOKEN  */
  YYSYMBOL_PARS_WHILE_TOKEN = 26,          /* PARS_WHILE_TOKEN  */
  YYSYMBOL_PARS_RETURN_TOKEN = 27,         /* PARS_RETURN_TOKEN  */
  YYSYMBOL_PARS_SELECT_TOKEN = 28,         /* PARS_SELECT_TOKEN  */
  YYSYMBOL_PARS_COUNT_TOKEN = 29,          /* PARS_COUNT_TOKEN  */
  YYSYMBOL_PARS_FROM_TOKEN = 30,           /* PARS_FROM_TOKEN  */
  YYSYMBOL_PARS_WHERE_TOKEN = 31,          /* PARS_WHERE_TOKEN  */
  YYSYMBOL_PARS_FOR_TOKEN = 32,            /* PARS_FOR_TOKEN  */
  YYSYMBOL_PARS_DDOT_TOKEN = 33,           /* PARS_DDOT_TOKEN  */
  YYSYMBOL_PARS_ORDER_TOKEN = 34,          /* PARS_ORDER_TOKEN  */
  YYSYMBOL_PARS_BY_TOKEN = 35,             /* PARS_BY_TOKEN  */
  YYSYMBOL_PARS_ASC_TOKEN = 36,            /* PARS_ASC_TOKEN  */
  YYSYMBOL_PARS_DESC_TOKEN = 37,           /* PARS_DESC_TOKEN  */
  YYSYMBOL_PARS_INSERT_TOKEN = 38,         /* PARS_INSERT_TOKEN  */
  YYSYMBOL_PARS_INTO_TOKEN = 39,           /* PARS_INTO_TOKEN  */
  YYSYMBOL_PARS_VALUES_TOKEN = 40,         /* PARS_VALUES_TOKEN  */
  YYSYMBOL_PARS_UPDATE_TOKEN = 41,         /* PARS_UPDATE_TOKEN  */
  YYSYMBOL_PARS_SET_TOKEN = 42,            /* PARS_SET_TOKEN  */
  YYSYMBOL_PARS_DELETE_TOKEN = 43,         /* PARS_DELETE_TOKEN  */
  YYSYMBOL_PARS_CURRENT_TOKEN = 44,        /* PARS_CURRENT_TOKEN  */
  YYSYMBOL_PARS_OF_TOKEN = 45,             /* PARS_OF_TOKEN  */
  YYSYMBOL_PARS_CREATE_TOKEN = 46,         /* PARS_CREATE_TOKEN  */
  YYSYMBOL_PARS_TABLE_TOKEN = 47,          /* PARS_TABLE_TOKEN  */
  YYSYMBOL_PARS_INDEX_TOKEN = 48,          /* PARS_INDEX_TOKEN  */
  YYSYMBOL_PARS_UNIQUE_TOKEN = 49,         /* PARS_UNIQUE_TOKEN  */
  YYSYMBOL_PARS_CLUSTERED_TOKEN = 50,      /* PARS_CLUSTERED_TOKEN  */
  YYSYMBOL_PARS_ON_TOKEN = 51,             /* PARS_ON_TOKEN  */
  YYSYMBOL_PARS_ASSIGN_TOKEN = 52,         /* PARS_ASSIGN_TOKEN  */
  YYSYMBOL_PARS_DECLARE_TOKEN = 53,        /* PARS_DECLARE_TOKEN  */
  YYSYMBOL_PARS_CURSOR_TOKEN = 54,         /* PARS_CURSOR_TOKEN  */
  YYSYMBOL_PARS_SQL_TOKEN = 55,            /* PARS_SQL_TOKEN  */
  YYSYMBOL_PARS_OPEN_TOKEN = 56,           /* PARS_OPEN_TOKEN  */
  YYSYMBOL_PARS_FETCH_TOKEN = 57,          /* PARS_FETCH_TOKEN  */
  YYSYMBOL_PARS_CLOSE_TOKEN = 58,          /* PARS_CLOSE_TOKEN  */
  YYSYMBOL_PARS_NOTFOUND_TOKEN = 59,       /* PARS_NOTFOUND_TOKEN  */
  YYSYMBOL_PARS_TO_BINARY_TOKEN = 60,      /* PARS_TO_BINARY_TOKEN  */
  YYSYMBOL_PARS_SUBSTR_TOKEN = 61,         /* PARS_SUBSTR_TOKEN  */
  YYSYMBOL_PARS_CONCAT_TOKEN = 62,         /* PARS_CONCAT_TOKEN  */
  YYSYMBOL_PARS_INSTR_TOKEN = 63,          /* PARS_INSTR_TOKEN  */
  YYSYMBOL_PARS_LENGTH_TOKEN = 64,         /* PARS_LENGTH_TOKEN  */
  YYSYMBOL_PARS_COMMIT_TOKEN = 65,         /* PARS_COMMIT_TOKEN  */
  YYSYMBOL_PARS_ROLLBACK_TOKEN = 66,       /* PARS_ROLLBACK_TOKEN  */
  YYSYMBOL_PARS_WORK_TOKEN = 67,           /* PARS_WORK_TOKEN  */
  YYSYMBOL_PARS_EXIT_TOKEN = 68,           /* PARS_EXIT_TOKEN  */
  YYSYMBOL_PARS_FUNCTION_TOKEN = 69,       /* PARS_FUNCTION_TOKEN  */
  YYSYMBOL_PARS_LOCK_TOKEN = 70,           /* PARS_LOCK_TOKEN  */
  YYSYMBOL_PARS_SHARE_TOKEN = 71,          /* PARS_SHARE_TOKEN  */
  YYSYMBOL_PARS_MODE_TOKEN = 72,           /* PARS_MODE_TOKEN  */
  YYSYMBOL_PARS_LIKE_TOKEN = 73,           /* PARS_LIKE_TOKEN  */
  YYSYMBOL_PARS_LIKE_TOKEN_EXACT = 74,     /* PARS_LIKE_TOKEN_EXACT  */
  YYSYMBOL_PARS_LIKE_TOKEN_PREFIX = 75,    /* PARS_LIKE_TOKEN_PREFIX  */
  YYSYMBOL_PARS_LIKE_TOKEN_SUFFIX = 76,    /* PARS_LIKE_TOKEN_SUFFIX  */
  YYSYMBOL_PARS_LIKE_TOKEN_SUBSTR = 77,    /* PARS_LIKE_TOKEN_SUBSTR  */
  YYSYMBOL_PARS_TABLE_NAME_TOKEN = 78,     /* PARS_TABLE_NAME_TOKEN  */
  YYSYMBOL_PARS_BIGINT_TOKEN = 79,         /* PARS_BIGINT_TOKEN  */
  YYSYMBOL_80_ = 80,                       /* '='  */
  YYSYMBOL_81_ = 81,                       /* '<'  */
  YYSYMBOL_82_ = 82,                       /* '>'  */
  YYSYMBOL_83_ = 83,                       /* '-'  */
  YYSYMBOL_84_ = 84,                       /* '+'  */
  YYSYMBOL_85_ = 85,                       /* '*'  */
  YYSYMBOL_86_ = 86,                       /* '/'  */
  YYSYMBOL_NEG = 87,                       /* NEG  */
  YYSYMBOL_88_ = 88,                       /* '%'  */
  YYSYMBOL_89_ = 89,                       /* ';'  */
  YYSYMBOL_90_ = 90,                       /* '('  */
  YYSYMBOL_91_ = 91,                       /* ')'  */
  YYSYMBOL_92_ = 92,                       /* ','  */
  YYSYMBOL_YYACCEPT = 93,                  /* $accept  */
  YYSYMBOL_top_statement = 94,             /* top_statement  */
  YYSYMBOL_statement = 95,                 /* statement  */
  YYSYMBOL_statement_list = 96,            /* statement_list  */
  YYSYMBOL_exp = 97,                       /* exp  */
  YYSYMBOL_function_name = 98,             /* function_name  */
  YYSYMBOL_user_function_call = 99,        /* user_function_call  */
  YYSYMBOL_table_list = 100,               /* table_list  */
  YYSYMBOL_variable_list = 101,            /* variable_list  */
  YYSYMBOL_exp_list = 102,                 /* exp_list  */
  YYSYMBOL_select_item = 103,              /* select_item  */
  YYSYMBOL_select_item_list = 104,         /* select_item_list  */
  YYSYMBOL_select_list = 105,              /* select_list  */
  YYSYMBOL_search_condition = 106,         /* search_condition  */
  YYSYMBOL_for_update_clause = 107,        /* for_update_clause  */
  YYSYMBOL_lock_shared_clause = 108,       /* lock_shared_clause  */
  YYSYMBOL_order_direction = 109,          /* order_direction  */
  YYSYMBOL_order_by_clause = 110,          /* order_by_clause  */
  YYSYMBOL_select_statement = 111,         /* select_statement  */
  YYSYMBOL_insert_statement_start = 112,   /* insert_statement_start  */
  YYSYMBOL_insert_statement = 113,         /* insert_statement  */
  YYSYMBOL_column_assignment = 114,        /* column_assignment  */
  YYSYMBOL_column_assignment_list = 115,   /* column_assignment_list  */
  YYSYMBOL_cursor_positioned = 116,        /* cursor_positioned  */
  YYSYMBOL_update_statement_start = 117,   /* update_statement_start  */
  YYSYMBOL_update_statement_searched = 118, /* update_statement_searched  */
  YYSYMBOL_update_statement_positioned = 119, /* update_statement_positioned  */
  YYSYMBOL_delete_statement_start = 120,   /* delete_statement_start  */
  YYSYMBOL_delete_statement_searched = 121, /* delete_statement_searched  */
  YYSYMBOL_delete_statement_positioned = 122, /* delete_statement_positioned  */
  YYSYMBOL_assignment_statement = 123,     /* assignment_statement  */
  YYSYMBOL_elsif_element = 124,            /* elsif_element  */
  YYSYMBOL_elsif_list = 125,               /* elsif_list  */
  YYSYMBOL_else_part = 126,                /* else_part  */
  YYSYMBOL_if_statement = 127,             /* if_statement  */
  YYSYMBOL_while_statement = 128,          /* while_statement  */
  YYSYMBOL_for_statement = 129,            /* for_statement  */
  YYSYMBOL_exit_statement = 130,           /* exit_statement  */
  YYSYMBOL_return_statement = 131,         /* return_statement  */
  YYSYMBOL_open_cursor_statement = 132,    /* open_cursor_statement  */
  YYSYMBOL_close_cursor_statement = 133,   /* close_cursor_statement  */
  YYSYMBOL_fetch_statement = 134,          /* fetch_statement  */
  YYSYMBOL_column_def = 135,               /* column_def  */
  YYSYMBOL_column_def_list = 136,          /* column_def_list  */
  YYSYMBOL_opt_column_len = 137,           /* opt_column_len  */
  YYSYMBOL_opt_not_null = 138,             /* opt_not_null  */
  YYSYMBOL_create_table = 139,             /* create_table  */
  YYSYMBOL_column_list = 140,              /* column_list  */
  YYSYMBOL_unique_def = 141,               /* unique_def  */
  YYSYMBOL_clustered_def = 142,            /* clustered_def  */
  YYSYMBOL_create_index = 143,             /* create_index  */
  YYSYMBOL_table_name = 144,               /* table_name  */
  YYSYMBOL_commit_statement = 145,         /* commit_statement  */
  YYSYMBOL_rollback_statement = 146,       /* rollback_statement  */
  YYSYMBOL_type_name = 147,                /* type_name  */
  YYSYMBOL_variable_declaration = 148,     /* variable_declaration  */
  YYSYMBOL_variable_declaration_list = 149, /* variable_declaration_list  */
  YYSYMBOL_cursor_declaration = 150,       /* cursor_declaration  */
  YYSYMBOL_function_declaration = 151,     /* function_declaration  */
  YYSYMBOL_declaration = 152,              /* declaration  */
  YYSYMBOL_declaration_list = 153,         /* declaration_list  */
  YYSYMBOL_procedure_definition = 154      /* procedure_definition  */
};
typedef enum yysymbol_kind_t yysymbol_kind_t;




#ifdef short
# undef short
#endif

/* On compilers that do not define __PTRDIFF_MAX__ etc., make sure
   <limits.h> and (if available) <stdint.h> are included
   so that the code can choose integer types of a good width.  */

#ifndef __PTRDIFF_MAX__
# include <limits.h> /* INFRINGES ON USER NAME SPACE */
# if defined __STDC_VERSION__ && 199901 <= __STDC_VERSION__
#  include <stdint.h> /* INFRINGES ON USER NAME SPACE */
#  define YY_STDINT_H
# endif
#endif

/* Narrow types that promote to a signed type and that can represent a
   signed or unsigned integer of at least N bits.  In tables they can
   save space and decrease cache pressure.  Promoting to a signed type
   helps avoid bugs in integer arithmetic.  */

#ifdef __INT_LEAST8_MAX__
typedef __INT_LEAST8_TYPE__ yytype_int8;
#elif defined YY_STDINT_H
typedef int_least8_t yytype_int8;
#else
typedef signed char yytype_int8;
#endif

#ifdef __INT_LEAST16_MAX__
typedef __INT_LEAST16_TYPE__ yytype_int16;
#elif defined YY_STDINT_H
typedef int_least16_t yytype_int16;
#else
typedef short yytype_int16;
#endif

/* Work around bug in HP-UX 11.23, which defines these macros
   incorrectly for preprocessor constants.  This workaround can likely
   be removed in 2023, as HPE has promised support for HP-UX 11.23
   (aka HP-UX 11i v2) only through the end of 2022; see Table 2 of
   <https://h20195.www2.hpe.com/V2/getpdf.aspx/4AA4-7673ENW.pdf>.  */
#ifdef __hpux
# undef UINT_LEAST8_MAX
# undef UINT_LEAST16_MAX
# define UINT_LEAST8_MAX 255
# define UINT_LEAST16_MAX 65535
#endif

#if defined __UINT_LEAST8_MAX__ && __UINT_LEAST8_MAX__ <= __INT_MAX__
typedef __UINT_LEAST8_TYPE__ yytype_uint8;
#elif (!defined __UINT_LEAST8_MAX__ && defined YY_STDINT_H \
       && UINT_LEAST8_MAX <= INT_MAX)
typedef uint_least8_t yytype_uint8;
#elif !defined __UINT_LEAST8_MAX__ && UCHAR_MAX <= INT_MAX
typedef unsigned char yytype_uint8;
#else
typedef short yytype_uint8;
#endif

#if defined __UINT_LEAST16_MAX__ && __UINT_LEAST16_MAX__ <= __INT_MAX__
typedef __UINT_LEAST16_TYPE__ yytype_uint16;
#elif (!defined __UINT_LEAST16_MAX__ && defined YY_STDINT_H \
       && UINT_LEAST16_MAX <= INT_MAX)
typedef uint_least16_t yytype_uint16;
#elif !defined __UINT_LEAST16_MAX__ && USHRT_MAX <= INT_MAX
typedef unsigned short yytype_uint16;
#else
typedef int yytype_uint16;
#endif

#ifndef YYPTRDIFF_T
# if defined __PTRDIFF_TYPE__ && defined __PTRDIFF_MAX__
#  define YYPTRDIFF_T __PTRDIFF_TYPE__
#  define YYPTRDIFF_MAXIMUM __PTRDIFF_MAX__
# elif defined PTRDIFF_MAX
#  ifndef ptrdiff_t
#   include <stddef.h> /* INFRINGES ON USER NAME SPACE */
#  endif
#  define YYPTRDIFF_T ptrdiff_t
#  define YYPTRDIFF_MAXIMUM PTRDIFF_MAX
# else
#  define YYPTRDIFF_T long
#  define YYPTRDIFF_MAXIMUM LONG_MAX
# endif
#endif

#ifndef YYSIZE_T
# ifdef __SIZE_TYPE__
#  define YYSIZE_T __SIZE_TYPE__
# elif defined size_t
#  define YYSIZE_T size_t
# elif defined __STDC_VERSION__ && 199901 <= __STDC_VERSION__
#  include <stddef.h> /* INFRINGES ON USER NAME SPACE */
#  define YYSIZE_T size_t
# else
#  define YYSIZE_T unsigned
# endif
#endif

#define YYSIZE_MAXIMUM                                  \
  YY_CAST (YYPTRDIFF_T,                                 \
           (YYPTRDIFF_MAXIMUM < YY_CAST (YYSIZE_T, -1)  \
            ? YYPTRDIFF_MAXIMUM                         \
            : YY_CAST (YYSIZE_T, -1)))

#define YYSIZEOF(X) YY_CAST (YYPTRDIFF_T, sizeof (X))


/* Stored state numbers (used for stacks). */
typedef yytype_int16 yy_state_t;

/* State numbers in computations.  */
typedef int yy_state_fast_t;

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


#ifndef YY_ATTRIBUTE_PURE
# if defined __GNUC__ && 2 < __GNUC__ + (96 <= __GNUC_MINOR__)
#  define YY_ATTRIBUTE_PURE __attribute__ ((__pure__))
# else
#  define YY_ATTRIBUTE_PURE
# endif
#endif

#ifndef YY_ATTRIBUTE_UNUSED
# if defined __GNUC__ && 2 < __GNUC__ + (7 <= __GNUC_MINOR__)
#  define YY_ATTRIBUTE_UNUSED __attribute__ ((__unused__))
# else
#  define YY_ATTRIBUTE_UNUSED
# endif
#endif

/* Suppress unused-variable warnings by "using" E.  */
#if ! defined lint || defined __GNUC__
# define YY_USE(E) ((void) (E))
#else
# define YY_USE(E) /* empty */
#endif

#if defined __GNUC__ && ! defined __ICC && 407 <= __GNUC__ * 100 + __GNUC_MINOR__
/* Suppress an incorrect diagnostic about yylval being uninitialized.  */
# define YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN                            \
    _Pragma ("GCC diagnostic push")                                     \
    _Pragma ("GCC diagnostic ignored \"-Wuninitialized\"")              \
    _Pragma ("GCC diagnostic ignored \"-Wmaybe-uninitialized\"")
# define YY_IGNORE_MAYBE_UNINITIALIZED_END      \
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

#if defined __cplusplus && defined __GNUC__ && ! defined __ICC && 6 <= __GNUC__
# define YY_IGNORE_USELESS_CAST_BEGIN                          \
    _Pragma ("GCC diagnostic push")                            \
    _Pragma ("GCC diagnostic ignored \"-Wuseless-cast\"")
# define YY_IGNORE_USELESS_CAST_END            \
    _Pragma ("GCC diagnostic pop")
#endif
#ifndef YY_IGNORE_USELESS_CAST_BEGIN
# define YY_IGNORE_USELESS_CAST_BEGIN
# define YY_IGNORE_USELESS_CAST_END
#endif


#define YY_ASSERT(E) ((void) (0 && (E)))

#if !defined yyoverflow

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
#endif /* !defined yyoverflow */

#if (! defined yyoverflow \
     && (! defined __cplusplus \
         || (defined YYSTYPE_IS_TRIVIAL && YYSTYPE_IS_TRIVIAL)))

/* A type that is properly aligned for any stack member.  */
union yyalloc
{
  yy_state_t yyss_alloc;
  YYSTYPE yyvs_alloc;
};

/* The size of the maximum gap between one aligned stack and the next.  */
# define YYSTACK_GAP_MAXIMUM (YYSIZEOF (union yyalloc) - 1)

/* The size of an array large to enough to hold all stacks, each with
   N elements.  */
# define YYSTACK_BYTES(N) \
     ((N) * (YYSIZEOF (yy_state_t) + YYSIZEOF (YYSTYPE)) \
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
        YYPTRDIFF_T yynewbytes;                                         \
        YYCOPY (&yyptr->Stack_alloc, Stack, yysize);                    \
        Stack = &yyptr->Stack_alloc;                                    \
        yynewbytes = yystacksize * YYSIZEOF (*Stack) + YYSTACK_GAP_MAXIMUM; \
        yyptr += yynewbytes / YYSIZEOF (*yyptr);                        \
      }                                                                 \
    while (0)

#endif

#if defined YYCOPY_NEEDED && YYCOPY_NEEDED
/* Copy COUNT objects from SRC to DST.  The source and destination do
   not overlap.  */
# ifndef YYCOPY
#  if defined __GNUC__ && 1 < __GNUC__
#   define YYCOPY(Dst, Src, Count) \
      __builtin_memcpy (Dst, Src, YY_CAST (YYSIZE_T, (Count)) * sizeof (*(Src)))
#  else
#   define YYCOPY(Dst, Src, Count)              \
      do                                        \
        {                                       \
          YYPTRDIFF_T yyi;                      \
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
#define YYLAST   611

/* YYNTOKENS -- Number of terminals.  */
#define YYNTOKENS  93
/* YYNNTS -- Number of nonterminals.  */
#define YYNNTS  62
/* YYNRULES -- Number of rules.  */
#define YYNRULES  145
/* YYNSTATES -- Number of states.  */
#define YYNSTATES  290

/* YYMAXUTOK -- Last valid token kind.  */
#define YYMAXUTOK   335


/* YYTRANSLATE(TOKEN-NUM) -- Symbol number corresponding to TOKEN-NUM
   as returned by yylex, with out-of-bounds checking.  */
#define YYTRANSLATE(YYX)                                \
  (0 <= (YYX) && (YYX) <= YYMAXUTOK                     \
   ? YY_CAST (yysymbol_kind_t, yytranslate[YYX])        \
   : YYSYMBOL_YYUNDEF)

/* YYTRANSLATE[TOKEN-NUM] -- Symbol number corresponding to TOKEN-NUM
   as returned by yylex.  */
static const yytype_int8 yytranslate[] =
{
       0,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,    88,     2,     2,
      90,    91,    85,    84,    92,    83,     2,    86,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,    89,
      81,    80,    82,     2,     2,     2,     2,     2,     2,     2,
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
static const yytype_int16 yyrline[] =
{
       0,   140,   140,   143,   144,   145,   146,   147,   148,   149,
     150,   151,   152,   153,   154,   155,   156,   157,   158,   159,
     160,   161,   165,   166,   171,   172,   174,   175,   176,   177,
     178,   179,   180,   181,   182,   183,   184,   185,   186,   188,
     189,   190,   191,   192,   193,   194,   195,   196,   198,   203,
     204,   205,   206,   207,   211,   215,   216,   221,   222,   223,
     228,   229,   230,   234,   235,   243,   244,   245,   250,   252,
     255,   259,   260,   264,   265,   270,   271,   276,   277,   278,
     282,   283,   290,   305,   310,   313,   321,   327,   328,   333,
     339,   348,   356,   364,   371,   379,   387,   394,   400,   401,
     406,   407,   409,   413,   420,   426,   436,   440,   444,   451,
     458,   462,   470,   479,   480,   485,   486,   491,   492,   498,
     506,   507,   512,   513,   517,   518,   522,   536,   537,   541,
     546,   551,   552,   553,   557,   563,   565,   566,   570,   578,
     584,   585,   588,   590,   591,   595
};
#endif

/** Accessing symbol of state STATE.  */
#define YY_ACCESSING_SYMBOL(State) YY_CAST (yysymbol_kind_t, yystos[State])

#if YYDEBUG || 0
/* The user-facing name of the symbol whose (internal) number is
   YYSYMBOL.  No bounds checking.  */
static const char *yysymbol_name (yysymbol_kind_t yysymbol) YY_ATTRIBUTE_UNUSED;

/* YYTNAME[SYMBOL-NUM] -- String name of the symbol SYMBOL-NUM.
   First, the terminals, then, starting at YYNTOKENS, nonterminals.  */
static const char *const yytname[] =
{
  "\"end of file\"", "error", "\"invalid token\"", "PARS_INT_LIT",
  "PARS_FLOAT_LIT", "PARS_STR_LIT", "PARS_NULL_LIT", "PARS_ID_TOKEN",
  "PARS_AND_TOKEN", "PARS_OR_TOKEN", "PARS_NOT_TOKEN", "PARS_GE_TOKEN",
  "PARS_LE_TOKEN", "PARS_NE_TOKEN", "PARS_PROCEDURE_TOKEN",
  "PARS_IN_TOKEN", "PARS_INT_TOKEN", "PARS_CHAR_TOKEN", "PARS_IS_TOKEN",
  "PARS_BEGIN_TOKEN", "PARS_END_TOKEN", "PARS_IF_TOKEN", "PARS_THEN_TOKEN",
  "PARS_ELSE_TOKEN", "PARS_ELSIF_TOKEN", "PARS_LOOP_TOKEN",
  "PARS_WHILE_TOKEN", "PARS_RETURN_TOKEN", "PARS_SELECT_TOKEN",
  "PARS_COUNT_TOKEN", "PARS_FROM_TOKEN", "PARS_WHERE_TOKEN",
  "PARS_FOR_TOKEN", "PARS_DDOT_TOKEN", "PARS_ORDER_TOKEN", "PARS_BY_TOKEN",
  "PARS_ASC_TOKEN", "PARS_DESC_TOKEN", "PARS_INSERT_TOKEN",
  "PARS_INTO_TOKEN", "PARS_VALUES_TOKEN", "PARS_UPDATE_TOKEN",
  "PARS_SET_TOKEN", "PARS_DELETE_TOKEN", "PARS_CURRENT_TOKEN",
  "PARS_OF_TOKEN", "PARS_CREATE_TOKEN", "PARS_TABLE_TOKEN",
  "PARS_INDEX_TOKEN", "PARS_UNIQUE_TOKEN", "PARS_CLUSTERED_TOKEN",
  "PARS_ON_TOKEN", "PARS_ASSIGN_TOKEN", "PARS_DECLARE_TOKEN",
  "PARS_CURSOR_TOKEN", "PARS_SQL_TOKEN", "PARS_OPEN_TOKEN",
  "PARS_FETCH_TOKEN", "PARS_CLOSE_TOKEN", "PARS_NOTFOUND_TOKEN",
  "PARS_TO_BINARY_TOKEN", "PARS_SUBSTR_TOKEN", "PARS_CONCAT_TOKEN",
  "PARS_INSTR_TOKEN", "PARS_LENGTH_TOKEN", "PARS_COMMIT_TOKEN",
  "PARS_ROLLBACK_TOKEN", "PARS_WORK_TOKEN", "PARS_EXIT_TOKEN",
  "PARS_FUNCTION_TOKEN", "PARS_LOCK_TOKEN", "PARS_SHARE_TOKEN",
  "PARS_MODE_TOKEN", "PARS_LIKE_TOKEN", "PARS_LIKE_TOKEN_EXACT",
  "PARS_LIKE_TOKEN_PREFIX", "PARS_LIKE_TOKEN_SUFFIX",
  "PARS_LIKE_TOKEN_SUBSTR", "PARS_TABLE_NAME_TOKEN", "PARS_BIGINT_TOKEN",
  "'='", "'<'", "'>'", "'-'", "'+'", "'*'", "'/'", "NEG", "'%'", "';'",
  "'('", "')'", "','", "$accept", "top_statement", "statement",
  "statement_list", "exp", "function_name", "user_function_call",
  "table_list", "variable_list", "exp_list", "select_item",
  "select_item_list", "select_list", "search_condition",
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

static const char *
yysymbol_name (yysymbol_kind_t yysymbol)
{
  return yytname[yysymbol];
}
#endif

#ifdef YYPRINT
/* YYTOKNUM[NUM] -- (External) token number corresponding to the
   (internal) symbol number NUM (which must be that of a token).  */
static const yytype_int16 yytoknum[] =
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
      40,    41,    44
};
#endif

#define YYPACT_NINF (-146)

#define yypact_value_is_default(Yyn) \
  ((Yyn) == YYPACT_NINF)

#define YYTABLE_NINF (-1)

#define yytable_value_is_error(Yyn) \
  0

  /* YYPACT[STATE-NUM] -- Index in YYTABLE of the portion describing
     STATE-NUM.  */
static const yytype_int16 yypact[] =
{
       8,    25,    40,   -44,   -43,  -146,  -146,   -41,    37,    54,
       9,  -146,    44,  -146,  -146,  -146,   -24,   -30,  -146,  -146,
    -146,  -146,    -5,  -146,    63,    97,   543,  -146,    93,    24,
      79,   148,   148,  -146,    13,   126,    98,     0,   111,    -3,
     135,   136,   138,    80,    83,  -146,  -146,   414,    67,    -7,
      70,   130,    84,    85,   130,    86,    87,    88,    89,    90,
      91,    92,    94,    95,    99,   100,   104,   105,   107,   108,
     141,  -146,   148,  -146,  -146,  -146,  -146,   112,   148,   119,
    -146,  -146,  -146,  -146,  -146,   148,   148,   193,   123,   208,
     124,  -146,   304,  -146,   -26,   152,   172,     0,  -146,  -146,
     181,     0,     0,  -146,   174,  -146,   186,  -146,  -146,  -146,
    -146,  -146,  -146,   137,  -146,  -146,   102,  -146,  -146,  -146,
    -146,  -146,  -146,  -146,  -146,  -146,  -146,  -146,  -146,  -146,
    -146,  -146,  -146,  -146,  -146,  -146,  -146,  -146,   139,   304,
     167,    -1,   170,    17,   159,   148,   148,   148,   148,   148,
     543,   225,   148,   148,   148,   148,   148,   148,   148,   148,
     543,   149,   228,    31,     0,   148,  -146,   229,  -146,   147,
    -146,   198,   240,   148,   203,   304,  -146,  -146,  -146,  -146,
      -1,    -1,    16,    16,   304,   371,  -146,    16,    16,    16,
      49,    49,    17,    17,   304,   -64,   457,   158,  -146,   160,
    -146,   -25,  -146,   247,   171,  -146,   161,   250,   254,   164,
    -146,   160,   -38,   255,   543,   148,  -146,   239,   244,  -146,
     148,   242,  -146,   258,   148,     0,   236,   148,   148,   229,
       9,  -146,   -33,   218,   179,  -146,  -146,   543,   274,  -146,
     251,   304,  -146,  -146,  -146,   230,   214,   289,   304,  -146,
     205,  -146,   250,     0,  -146,   543,  -146,  -146,   284,   269,
     543,   301,   295,  -146,   216,   543,   237,   272,  -146,   500,
     219,   303,  -146,   311,   249,   312,   286,  -146,  -146,  -146,
     -28,  -146,   103,  -146,  -146,   315,  -146,  -146,  -146,  -146
};

  /* YYDEFACT[STATE-NUM] -- Default reduction number in state STATE-NUM.
     Performed when YYTABLE does not specify something else to do.  Zero
     means the default is an error.  */
static const yytype_uint8 yydefact[] =
{
       0,     0,     0,     0,     0,     1,     2,     0,     0,   135,
       0,   136,   142,   131,   133,   132,     0,     0,   137,   140,
     141,   143,     0,   134,     0,     0,     0,   144,     0,     0,
       0,     0,     0,   107,    65,     0,     0,     0,     0,   122,
       0,     0,     0,     0,     0,   106,    22,     0,     0,     0,
       0,    71,     0,     0,    71,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
       0,   139,     0,    26,    27,    28,    29,    24,     0,    30,
      49,    50,    51,    52,    53,     0,     0,     0,     0,     0,
       0,    68,    63,    66,    70,     0,     0,     0,   127,   128,
       0,     0,     0,   123,   124,   108,     0,   109,   129,   130,
     145,    23,     9,     0,    85,    10,     0,    91,    92,    13,
      14,    94,    95,    11,    12,     8,     6,     3,     4,     5,
       7,    15,    17,    16,    20,    21,    18,    19,     0,    96,
       0,    46,     0,    35,     0,     0,     0,     0,     0,     0,
       0,     0,     0,     0,     0,     0,     0,     0,     0,    60,
       0,     0,    57,     0,     0,     0,    83,     0,    93,     0,
     125,     0,    57,    60,     0,    72,   138,    47,    48,    36,
      44,    45,    41,    42,    43,   100,    38,    37,    39,    40,
      32,    31,    33,    34,    61,     0,     0,     0,    58,    69,
      67,    71,    55,     0,     0,    87,    90,     0,     0,    58,
     111,   110,     0,     0,     0,     0,    98,   102,     0,    25,
       0,     0,    64,     0,     0,     0,    73,     0,     0,     0,
       0,   113,     0,     0,     0,    84,    89,   101,     0,    99,
       0,    62,   104,    59,    56,     0,    75,     0,    86,    88,
     115,   119,     0,     0,    54,     0,   103,    74,     0,    80,
       0,     0,   117,   114,     0,    97,     0,     0,    82,     0,
       0,     0,   112,     0,     0,     0,     0,   116,   118,   120,
       0,    76,    77,   105,   126,     0,    78,    79,    81,   121
};

  /* YYPGOTO[NTERM-NUM].  */
static const yytype_int16 yypgoto[] =
{
    -146,  -146,   -47,  -145,   -29,  -146,  -146,  -146,   151,   153,
     162,  -146,  -146,   -53,  -146,  -146,  -146,  -146,   -18,  -146,
    -146,   106,  -146,   270,  -146,  -146,  -146,  -146,  -146,  -146,
    -146,   117,  -146,  -146,  -146,  -146,  -146,  -146,  -146,  -146,
    -146,  -146,    96,  -146,  -146,  -146,  -146,  -146,  -146,  -146,
    -146,   -93,  -146,  -146,   109,   324,  -146,  -146,  -146,   316,
    -146,  -146
};

  /* YYDEFGOTO[NTERM-NUM].  */
static const yytype_int16 yydefgoto[] =
{
       0,     2,    46,    47,    92,    88,   210,   201,   199,   195,
      93,    94,    95,   117,   246,   259,   288,   268,    48,    49,
      50,   205,   206,   118,    51,    52,    53,    54,    55,    56,
      57,   216,   217,   218,    58,    59,    60,    61,    62,    63,
      64,    65,   231,   232,   262,   272,    66,   280,   104,   171,
      67,   100,    68,    69,    16,    11,    12,    19,    20,    21,
      22,     3
};

  /* YYTABLE[YYPACT[STATE-NUM]] -- What to do in state STATE-NUM.  If
     positive, shift that token.  If negative, reduce the rule whose
     number is the opposite.  If YYTABLE_NINF, syntax error.  */
static const yytype_int16 yytable[] =
{
     111,   121,    87,    89,   166,   185,   224,    98,   168,   169,
     147,   148,   149,   162,    26,   196,    73,    74,    75,    76,
      77,    34,     1,    78,    24,    13,    14,   219,   220,   149,
     149,   114,     4,   113,    73,    74,    75,    76,    77,    25,
       5,    78,    90,   139,   102,     6,   103,     7,    17,   141,
       8,    10,   138,   235,   220,     9,   143,   144,   251,   252,
      90,    10,   149,   284,   285,    23,   163,   225,    79,   237,
      28,   202,   151,    80,    81,    82,    83,    84,    99,   152,
     153,   154,   155,   156,   157,   158,    79,   175,    15,   151,
     151,    80,    81,    82,    83,    84,    85,    17,    91,   155,
     156,   157,   158,    86,    29,    73,    74,    75,    76,    77,
     265,    70,    78,    71,    85,   269,   180,   181,   182,   183,
     184,    86,   151,   187,   188,   189,   190,   191,   192,   193,
     194,    72,   244,    96,   157,   158,   203,    97,   111,   286,
     287,   101,   105,   106,   194,   107,   174,   108,   226,   111,
     109,    73,    74,    75,    76,    77,   112,    79,    78,   115,
     264,   116,    80,    81,    82,    83,    84,   145,   146,    34,
     147,   148,   149,   119,   120,   123,   124,   125,   126,   127,
     128,   129,   164,   130,   131,    85,   238,   165,   132,   133,
     111,   241,    86,   134,   135,   175,   136,   137,   247,   248,
     140,   145,   146,    79,   147,   148,   149,   142,    80,    81,
      82,    83,    84,   159,   161,   150,   145,   146,   111,   147,
     148,   149,   111,   167,   170,   172,   177,   173,   176,   178,
     186,    85,   151,   160,   197,   198,   204,   207,    86,   152,
     153,   154,   155,   156,   157,   158,   208,   209,   213,   222,
     179,   228,   223,   229,   234,   145,   146,   230,   147,   148,
     149,   233,   236,   215,   240,   243,   151,   242,   245,   253,
     254,   257,   256,   152,   153,   154,   155,   156,   157,   158,
     227,   151,   145,   146,   258,   147,   148,   149,   152,   153,
     154,   155,   156,   157,   158,   261,   255,   145,   146,   266,
     147,   148,   149,   267,   270,   271,   273,   275,   274,   278,
     277,   283,   145,   146,   260,   147,   148,   149,   279,   282,
     151,   281,   289,   211,   122,   200,   212,   152,   153,   154,
     155,   156,   157,   158,   239,   249,    18,     0,    27,   250,
       0,     0,     0,     0,     0,     0,     0,   151,   263,     0,
       0,     0,     0,     0,   152,   153,   154,   155,   156,   157,
     158,     0,   151,     0,     0,     0,     0,     0,     0,   152,
     153,   154,   155,   156,   157,   158,     0,   151,    30,     0,
       0,     0,     0,     0,   152,   153,   154,   155,   156,   157,
     158,     0,    31,     0,   214,   215,     0,    32,    33,    34,
       0,     0,     0,    35,     0,     0,     0,     0,     0,    36,
       0,     0,    37,     0,    38,     0,     0,    39,     0,     0,
       0,    30,     0,     0,     0,     0,     0,    40,    41,    42,
       0,     0,     0,     0,   110,    31,    43,    44,     0,    45,
      32,    33,    34,     0,     0,     0,    35,     0,     0,     0,
       0,     0,    36,     0,     0,    37,     0,    38,     0,     0,
      39,     0,     0,     0,    30,     0,     0,     0,     0,     0,
      40,    41,    42,     0,     0,     0,     0,   221,    31,    43,
      44,     0,    45,    32,    33,    34,     0,     0,     0,    35,
       0,     0,     0,     0,     0,    36,     0,     0,    37,     0,
      38,     0,     0,    39,     0,     0,     0,    30,     0,     0,
       0,     0,     0,    40,    41,    42,     0,     0,     0,     0,
     276,    31,    43,    44,     0,    45,    32,    33,    34,     0,
       0,     0,    35,     0,     0,     0,     0,     0,    36,     0,
       0,    37,     0,    38,     0,     0,    39,     0,     0,     0,
      30,     0,     0,     0,     0,     0,    40,    41,    42,     0,
       0,     0,     0,     0,    31,    43,    44,     0,    45,    32,
      33,    34,     0,     0,     0,    35,     0,     0,     0,     0,
       0,    36,     0,     0,    37,     0,    38,     0,     0,    39,
       0,     0,     0,     0,     0,     0,     0,     0,     0,    40,
      41,    42,     0,     0,     0,     0,     0,     0,    43,    44,
       0,    45
};

static const yytype_int16 yycheck[] =
{
      47,    54,    31,    32,    97,   150,    31,     7,   101,   102,
      11,    12,    13,    39,    19,   160,     3,     4,     5,     6,
       7,    28,    14,    10,    54,    16,    17,    91,    92,    13,
      13,    49,     7,    40,     3,     4,     5,     6,     7,    69,
       0,    10,    29,    72,    47,    89,    49,    90,    53,    78,
      91,     7,    70,    91,    92,    18,    85,    86,    91,    92,
      29,     7,    13,    91,    92,    89,    92,    92,    55,   214,
       7,   164,    73,    60,    61,    62,    63,    64,    78,    80,
      81,    82,    83,    84,    85,    86,    55,   116,    79,    73,
      73,    60,    61,    62,    63,    64,    83,    53,    85,    83,
      84,    85,    86,    90,     7,     3,     4,     5,     6,     7,
     255,    18,    10,    89,    83,   260,   145,   146,   147,   148,
     149,    90,    73,   152,   153,   154,   155,   156,   157,   158,
     159,    52,   225,     7,    85,    86,   165,    39,   185,    36,
      37,    30,     7,     7,   173,     7,    44,    67,   201,   196,
      67,     3,     4,     5,     6,     7,    89,    55,    10,    89,
     253,    31,    60,    61,    62,    63,    64,     8,     9,    28,
      11,    12,    13,    89,    89,    89,    89,    89,    89,    89,
      89,    89,    30,    89,    89,    83,   215,    15,    89,    89,
     237,   220,    90,    89,    89,   224,    89,    89,   227,   228,
      88,     8,     9,    55,    11,    12,    13,    88,    60,    61,
      62,    63,    64,    90,    90,    22,     8,     9,   265,    11,
      12,    13,   269,    42,    50,    39,    59,    90,    89,    59,
       5,    83,    73,    25,    85,     7,     7,    90,    90,    80,
      81,    82,    83,    84,    85,    86,    48,     7,    45,    91,
      91,    80,    92,    92,    90,     8,     9,     7,    11,    12,
      13,     7,     7,    24,    20,     7,    73,    25,    32,    51,
      91,    41,    21,    80,    81,    82,    83,    84,    85,    86,
      33,    73,     8,     9,    70,    11,    12,    13,    80,    81,
      82,    83,    84,    85,    86,    90,    22,     8,     9,    15,
      11,    12,    13,    34,     3,    10,    90,    35,    71,     6,
      91,    25,     8,     9,    25,    11,    12,    13,     7,     7,
      73,    72,     7,   172,    54,   163,   173,    80,    81,    82,
      83,    84,    85,    86,   217,   229,    12,    -1,    22,   230,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    73,   252,    -1,
      -1,    -1,    -1,    -1,    80,    81,    82,    83,    84,    85,
      86,    -1,    73,    -1,    -1,    -1,    -1,    -1,    -1,    80,
      81,    82,    83,    84,    85,    86,    -1,    73,     7,    -1,
      -1,    -1,    -1,    -1,    80,    81,    82,    83,    84,    85,
      86,    -1,    21,    -1,    23,    24,    -1,    26,    27,    28,
      -1,    -1,    -1,    32,    -1,    -1,    -1,    -1,    -1,    38,
      -1,    -1,    41,    -1,    43,    -1,    -1,    46,    -1,    -1,
      -1,     7,    -1,    -1,    -1,    -1,    -1,    56,    57,    58,
      -1,    -1,    -1,    -1,    20,    21,    65,    66,    -1,    68,
      26,    27,    28,    -1,    -1,    -1,    32,    -1,    -1,    -1,
      -1,    -1,    38,    -1,    -1,    41,    -1,    43,    -1,    -1,
      46,    -1,    -1,    -1,     7,    -1,    -1,    -1,    -1,    -1,
      56,    57,    58,    -1,    -1,    -1,    -1,    20,    21,    65,
      66,    -1,    68,    26,    27,    28,    -1,    -1,    -1,    32,
      -1,    -1,    -1,    -1,    -1,    38,    -1,    -1,    41,    -1,
      43,    -1,    -1,    46,    -1,    -1,    -1,     7,    -1,    -1,
      -1,    -1,    -1,    56,    57,    58,    -1,    -1,    -1,    -1,
      20,    21,    65,    66,    -1,    68,    26,    27,    28,    -1,
      -1,    -1,    32,    -1,    -1,    -1,    -1,    -1,    38,    -1,
      -1,    41,    -1,    43,    -1,    -1,    46,    -1,    -1,    -1,
       7,    -1,    -1,    -1,    -1,    -1,    56,    57,    58,    -1,
      -1,    -1,    -1,    -1,    21,    65,    66,    -1,    68,    26,
      27,    28,    -1,    -1,    -1,    32,    -1,    -1,    -1,    -1,
      -1,    38,    -1,    -1,    41,    -1,    43,    -1,    -1,    46,
      -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    56,
      57,    58,    -1,    -1,    -1,    -1,    -1,    -1,    65,    66,
      -1,    68
};

  /* YYSTOS[STATE-NUM] -- The (internal number of the) accessing
     symbol of state STATE-NUM.  */
static const yytype_uint8 yystos[] =
{
       0,    14,    94,   154,     7,     0,    89,    90,    91,    18,
       7,   148,   149,    16,    17,    79,   147,    53,   148,   150,
     151,   152,   153,    89,    54,    69,    19,   152,     7,     7,
       7,    21,    26,    27,    28,    32,    38,    41,    43,    46,
      56,    57,    58,    65,    66,    68,    95,    96,   111,   112,
     113,   117,   118,   119,   120,   121,   122,   123,   127,   128,
     129,   130,   131,   132,   133,   134,   139,   143,   145,   146,
      18,    89,    52,     3,     4,     5,     6,     7,    10,    55,
      60,    61,    62,    63,    64,    83,    90,    97,    98,    97,
      29,    85,    97,   103,   104,   105,     7,    39,     7,    78,
     144,    30,    47,    49,   141,     7,     7,     7,    67,    67,
      20,    95,    89,    40,   111,    89,    31,   106,   116,    89,
      89,   106,   116,    89,    89,    89,    89,    89,    89,    89,
      89,    89,    89,    89,    89,    89,    89,    89,   111,    97,
      88,    97,    88,    97,    97,     8,     9,    11,    12,    13,
      22,    73,    80,    81,    82,    83,    84,    85,    86,    90,
      25,    90,    39,    92,    30,    15,   144,    42,   144,   144,
      50,   142,    39,    90,    44,    97,    89,    59,    59,    91,
      97,    97,    97,    97,    97,    96,     5,    97,    97,    97,
      97,    97,    97,    97,    97,   102,    96,    85,     7,   101,
     103,   100,   144,    97,     7,   114,   115,    90,    48,     7,
      99,   101,   102,    45,    23,    24,   124,   125,   126,    91,
      92,    20,    91,    92,    31,    92,   106,    33,    80,    92,
       7,   135,   136,     7,    90,    91,     7,    96,    97,   124,
      20,    97,    25,     7,   144,    32,   107,    97,    97,   114,
     147,    91,    92,    51,    91,    22,    21,    41,    70,   108,
      25,    90,   137,   135,   144,    96,    15,    34,   110,    96,
       3,    10,   138,    90,    71,    35,    20,    91,     6,     7,
     140,    72,     7,    25,    91,    92,    36,    37,   109,     7
};

  /* YYR1[YYN] -- Symbol number of symbol that rule YYN derives.  */
static const yytype_uint8 yyr1[] =
{
       0,    93,    94,    95,    95,    95,    95,    95,    95,    95,
      95,    95,    95,    95,    95,    95,    95,    95,    95,    95,
      95,    95,    96,    96,    97,    97,    97,    97,    97,    97,
      97,    97,    97,    97,    97,    97,    97,    97,    97,    97,
      97,    97,    97,    97,    97,    97,    97,    97,    97,    98,
      98,    98,    98,    98,    99,   100,   100,   101,   101,   101,
     102,   102,   102,   103,   103,   104,   104,   104,   105,   105,
     105,   106,   106,   107,   107,   108,   108,   109,   109,   109,
     110,   110,   111,   112,   113,   113,   114,   115,   115,   116,
     117,   118,   119,   120,   121,   122,   123,   124,   125,   125,
     126,   126,   126,   127,   128,   129,   130,   131,   132,   133,
     134,   134,   135,   136,   136,   137,   137,   138,   138,   139,
     140,   140,   141,   141,   142,   142,   143,   144,   144,   145,
     146,   147,   147,   147,   148,   149,   149,   149,   150,   151,
     152,   152,   153,   153,   153,   154
};

  /* YYR2[YYN] -- Number of symbols on the right hand side of rule YYN.  */
static const yytype_int8 yyr2[] =
{
       0,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
       2,     2,     1,     2,     1,     4,     1,     1,     1,     1,
       1,     3,     3,     3,     3,     2,     3,     3,     3,     3,
       3,     3,     3,     3,     3,     3,     2,     3,     3,     1,
       1,     1,     1,     1,     3,     1,     3,     0,     1,     3,
       0,     1,     3,     1,     4,     0,     1,     3,     1,     3,
       1,     0,     2,     0,     2,     0,     4,     0,     1,     1,
       0,     4,     8,     3,     5,     2,     3,     1,     3,     4,
       4,     2,     2,     3,     2,     2,     3,     4,     1,     2,
       0,     2,     1,     7,     6,    10,     1,     1,     2,     2,
       4,     4,     4,     1,     3,     0,     3,     0,     2,     6,
       1,     3,     0,     1,     0,     1,    10,     1,     1,     2,
       2,     1,     1,     1,     3,     0,     1,     2,     6,     4,
       1,     1,     0,     1,     2,    10
};


enum { YYENOMEM = -2 };

#define yyerrok         (yyerrstatus = 0)
#define yyclearin       (yychar = YYEMPTY)

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

/* Backward compatibility with an undocumented macro.
   Use YYerror or YYUNDEF. */
#define YYERRCODE YYUNDEF


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
# ifndef YY_LOCATION_PRINT
#  define YY_LOCATION_PRINT(File, Loc) ((void) 0)
# endif


# define YY_SYMBOL_PRINT(Title, Kind, Value, Location)                    \
do {                                                                      \
  if (yydebug)                                                            \
    {                                                                     \
      YYFPRINTF (stderr, "%s ", Title);                                   \
      yy_symbol_print (stderr,                                            \
                  Kind, Value); \
      YYFPRINTF (stderr, "\n");                                           \
    }                                                                     \
} while (0)


/*-----------------------------------.
| Print this symbol's value on YYO.  |
`-----------------------------------*/

static void
yy_symbol_value_print (FILE *yyo,
                       yysymbol_kind_t yykind, YYSTYPE const * const yyvaluep)
{
  FILE *yyoutput = yyo;
  YY_USE (yyoutput);
  if (!yyvaluep)
    return;
# ifdef YYPRINT
  if (yykind < YYNTOKENS)
    YYPRINT (yyo, yytoknum[yykind], *yyvaluep);
# endif
  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  YY_USE (yykind);
  YY_IGNORE_MAYBE_UNINITIALIZED_END
}


/*---------------------------.
| Print this symbol on YYO.  |
`---------------------------*/

static void
yy_symbol_print (FILE *yyo,
                 yysymbol_kind_t yykind, YYSTYPE const * const yyvaluep)
{
  YYFPRINTF (yyo, "%s %s (",
             yykind < YYNTOKENS ? "token" : "nterm", yysymbol_name (yykind));

  yy_symbol_value_print (yyo, yykind, yyvaluep);
  YYFPRINTF (yyo, ")");
}

/*------------------------------------------------------------------.
| yy_stack_print -- Print the state stack from its BOTTOM up to its |
| TOP (included).                                                   |
`------------------------------------------------------------------*/

static void
yy_stack_print (yy_state_t *yybottom, yy_state_t *yytop)
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
yy_reduce_print (yy_state_t *yyssp, YYSTYPE *yyvsp,
                 int yyrule)
{
  int yylno = yyrline[yyrule];
  int yynrhs = yyr2[yyrule];
  int yyi;
  YYFPRINTF (stderr, "Reducing stack by rule %d (line %d):\n",
             yyrule - 1, yylno);
  /* The symbols being reduced.  */
  for (yyi = 0; yyi < yynrhs; yyi++)
    {
      YYFPRINTF (stderr, "   $%d = ", yyi + 1);
      yy_symbol_print (stderr,
                       YY_ACCESSING_SYMBOL (+yyssp[yyi + 1 - yynrhs]),
                       &yyvsp[(yyi + 1) - (yynrhs)]);
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
# define YYDPRINTF(Args) ((void) 0)
# define YY_SYMBOL_PRINT(Title, Kind, Value, Location)
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






/*-----------------------------------------------.
| Release the memory associated to this symbol.  |
`-----------------------------------------------*/

static void
yydestruct (const char *yymsg,
            yysymbol_kind_t yykind, YYSTYPE *yyvaluep)
{
  YY_USE (yyvaluep);
  if (!yymsg)
    yymsg = "Deleting";
  YY_SYMBOL_PRINT (yymsg, yykind, yyvaluep, yylocationp);

  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  YY_USE (yykind);
  YY_IGNORE_MAYBE_UNINITIALIZED_END
}


/* Lookahead token kind.  */
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
    yy_state_fast_t yystate = 0;
    /* Number of tokens to shift before error messages enabled.  */
    int yyerrstatus = 0;

    /* Refer to the stacks through separate pointers, to allow yyoverflow
       to reallocate them elsewhere.  */

    /* Their size.  */
    YYPTRDIFF_T yystacksize = YYINITDEPTH;

    /* The state stack: array, bottom, top.  */
    yy_state_t yyssa[YYINITDEPTH];
    yy_state_t *yyss = yyssa;
    yy_state_t *yyssp = yyss;

    /* The semantic value stack: array, bottom, top.  */
    YYSTYPE yyvsa[YYINITDEPTH];
    YYSTYPE *yyvs = yyvsa;
    YYSTYPE *yyvsp = yyvs;

  int yyn;
  /* The return value of yyparse.  */
  int yyresult;
  /* Lookahead symbol kind.  */
  yysymbol_kind_t yytoken = YYSYMBOL_YYEMPTY;
  /* The variables used to return semantic value and location from the
     action routines.  */
  YYSTYPE yyval;



#define YYPOPSTACK(N)   (yyvsp -= (N), yyssp -= (N))

  /* The number of symbols on the RHS of the reduced rule.
     Keep to zero when no symbol should be popped.  */
  int yylen = 0;

  YYDPRINTF ((stderr, "Starting parse\n"));

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
| yysetstate -- set current state (the top of the stack) to yystate.  |
`--------------------------------------------------------------------*/
yysetstate:
  YYDPRINTF ((stderr, "Entering state %d\n", yystate));
  YY_ASSERT (0 <= yystate && yystate < YYNSTATES);
  YY_IGNORE_USELESS_CAST_BEGIN
  *yyssp = YY_CAST (yy_state_t, yystate);
  YY_IGNORE_USELESS_CAST_END
  YY_STACK_PRINT (yyss, yyssp);

  if (yyss + yystacksize - 1 <= yyssp)
#if !defined yyoverflow && !defined YYSTACK_RELOCATE
    goto yyexhaustedlab;
#else
    {
      /* Get the current used size of the three stacks, in elements.  */
      YYPTRDIFF_T yysize = yyssp - yyss + 1;

# if defined yyoverflow
      {
        /* Give user a chance to reallocate the stack.  Use copies of
           these so that the &'s don't force the real ones into
           memory.  */
        yy_state_t *yyss1 = yyss;
        YYSTYPE *yyvs1 = yyvs;

        /* Each stack pointer address is followed by the size of the
           data in use in that stack, in bytes.  This used to be a
           conditional around just the two extra args, but that might
           be undefined if yyoverflow is a macro.  */
        yyoverflow (YY_("memory exhausted"),
                    &yyss1, yysize * YYSIZEOF (*yyssp),
                    &yyvs1, yysize * YYSIZEOF (*yyvsp),
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
        yy_state_t *yyss1 = yyss;
        union yyalloc *yyptr =
          YY_CAST (union yyalloc *,
                   YYSTACK_ALLOC (YY_CAST (YYSIZE_T, YYSTACK_BYTES (yystacksize))));
        if (! yyptr)
          goto yyexhaustedlab;
        YYSTACK_RELOCATE (yyss_alloc, yyss);
        YYSTACK_RELOCATE (yyvs_alloc, yyvs);
#  undef YYSTACK_RELOCATE
        if (yyss1 != yyssa)
          YYSTACK_FREE (yyss1);
      }
# endif

      yyssp = yyss + yysize - 1;
      yyvsp = yyvs + yysize - 1;

      YY_IGNORE_USELESS_CAST_BEGIN
      YYDPRINTF ((stderr, "Stack size increased to %ld\n",
                  YY_CAST (long, yystacksize)));
      YY_IGNORE_USELESS_CAST_END

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

  /* YYCHAR is either empty, or end-of-input, or a valid lookahead.  */
  if (yychar == YYEMPTY)
    {
      YYDPRINTF ((stderr, "Reading a token\n"));
      yychar = yylex ();
    }

  if (yychar <= YYEOF)
    {
      yychar = YYEOF;
      yytoken = YYSYMBOL_YYEOF;
      YYDPRINTF ((stderr, "Now at end of input.\n"));
    }
  else if (yychar == YYerror)
    {
      /* The scanner already issued an error message, process directly
         to error recovery.  But do not keep the error token as
         lookahead, it is too special and may lead us to an endless
         loop in error recovery. */
      yychar = YYUNDEF;
      yytoken = YYSYMBOL_YYerror;
      goto yyerrlab1;
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
  yystate = yyn;
  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  *++yyvsp = yylval;
  YY_IGNORE_MAYBE_UNINITIALIZED_END

  /* Discard the shifted token.  */
  yychar = YYEMPTY;
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
  case 22: /* statement_list: statement  */
#line 165 "pars0grm.y"
                                { yyval = que_node_list_add_last(NULL, yyvsp[0]); }
#line 1545 "pars0grm.cc"
    break;

  case 23: /* statement_list: statement_list statement  */
#line 167 "pars0grm.y"
                                { yyval = que_node_list_add_last(yyvsp[-1], yyvsp[0]); }
#line 1551 "pars0grm.cc"
    break;

  case 24: /* exp: PARS_ID_TOKEN  */
#line 171 "pars0grm.y"
                                { yyval = yyvsp[0];}
#line 1557 "pars0grm.cc"
    break;

  case 25: /* exp: function_name '(' exp_list ')'  */
#line 173 "pars0grm.y"
                                { yyval = pars_func(yyvsp[-3], yyvsp[-1]); }
#line 1563 "pars0grm.cc"
    break;

  case 26: /* exp: PARS_INT_LIT  */
#line 174 "pars0grm.y"
                                { yyval = yyvsp[0];}
#line 1569 "pars0grm.cc"
    break;

  case 27: /* exp: PARS_FLOAT_LIT  */
#line 175 "pars0grm.y"
                                { yyval = yyvsp[0];}
#line 1575 "pars0grm.cc"
    break;

  case 28: /* exp: PARS_STR_LIT  */
#line 176 "pars0grm.y"
                                { yyval = yyvsp[0];}
#line 1581 "pars0grm.cc"
    break;

  case 29: /* exp: PARS_NULL_LIT  */
#line 177 "pars0grm.y"
                                { yyval = yyvsp[0];}
#line 1587 "pars0grm.cc"
    break;

  case 30: /* exp: PARS_SQL_TOKEN  */
#line 178 "pars0grm.y"
                                { yyval = yyvsp[0];}
#line 1593 "pars0grm.cc"
    break;

  case 31: /* exp: exp '+' exp  */
#line 179 "pars0grm.y"
                                { yyval = pars_op('+', yyvsp[-2], yyvsp[0]); }
#line 1599 "pars0grm.cc"
    break;

  case 32: /* exp: exp '-' exp  */
#line 180 "pars0grm.y"
                                { yyval = pars_op('-', yyvsp[-2], yyvsp[0]); }
#line 1605 "pars0grm.cc"
    break;

  case 33: /* exp: exp '*' exp  */
#line 181 "pars0grm.y"
                                { yyval = pars_op('*', yyvsp[-2], yyvsp[0]); }
#line 1611 "pars0grm.cc"
    break;

  case 34: /* exp: exp '/' exp  */
#line 182 "pars0grm.y"
                                { yyval = pars_op('/', yyvsp[-2], yyvsp[0]); }
#line 1617 "pars0grm.cc"
    break;

  case 35: /* exp: '-' exp  */
#line 183 "pars0grm.y"
                                { yyval = pars_op('-', yyvsp[0], NULL); }
#line 1623 "pars0grm.cc"
    break;

  case 36: /* exp: '(' exp ')'  */
#line 184 "pars0grm.y"
                                { yyval = yyvsp[-1]; }
#line 1629 "pars0grm.cc"
    break;

  case 37: /* exp: exp '=' exp  */
#line 185 "pars0grm.y"
                                { yyval = pars_op('=', yyvsp[-2], yyvsp[0]); }
#line 1635 "pars0grm.cc"
    break;

  case 38: /* exp: exp PARS_LIKE_TOKEN PARS_STR_LIT  */
#line 187 "pars0grm.y"
                                { yyval = pars_op(PARS_LIKE_TOKEN, yyvsp[-2], yyvsp[0]); }
#line 1641 "pars0grm.cc"
    break;

  case 39: /* exp: exp '<' exp  */
#line 188 "pars0grm.y"
                                { yyval = pars_op('<', yyvsp[-2], yyvsp[0]); }
#line 1647 "pars0grm.cc"
    break;

  case 40: /* exp: exp '>' exp  */
#line 189 "pars0grm.y"
                                { yyval = pars_op('>', yyvsp[-2], yyvsp[0]); }
#line 1653 "pars0grm.cc"
    break;

  case 41: /* exp: exp PARS_GE_TOKEN exp  */
#line 190 "pars0grm.y"
                                { yyval = pars_op(PARS_GE_TOKEN, yyvsp[-2], yyvsp[0]); }
#line 1659 "pars0grm.cc"
    break;

  case 42: /* exp: exp PARS_LE_TOKEN exp  */
#line 191 "pars0grm.y"
                                { yyval = pars_op(PARS_LE_TOKEN, yyvsp[-2], yyvsp[0]); }
#line 1665 "pars0grm.cc"
    break;

  case 43: /* exp: exp PARS_NE_TOKEN exp  */
#line 192 "pars0grm.y"
                                { yyval = pars_op(PARS_NE_TOKEN, yyvsp[-2], yyvsp[0]); }
#line 1671 "pars0grm.cc"
    break;

  case 44: /* exp: exp PARS_AND_TOKEN exp  */
#line 193 "pars0grm.y"
                                { yyval = pars_op(PARS_AND_TOKEN, yyvsp[-2], yyvsp[0]); }
#line 1677 "pars0grm.cc"
    break;

  case 45: /* exp: exp PARS_OR_TOKEN exp  */
#line 194 "pars0grm.y"
                                { yyval = pars_op(PARS_OR_TOKEN, yyvsp[-2], yyvsp[0]); }
#line 1683 "pars0grm.cc"
    break;

  case 46: /* exp: PARS_NOT_TOKEN exp  */
#line 195 "pars0grm.y"
                                { yyval = pars_op(PARS_NOT_TOKEN, yyvsp[0], NULL); }
#line 1689 "pars0grm.cc"
    break;

  case 47: /* exp: PARS_ID_TOKEN '%' PARS_NOTFOUND_TOKEN  */
#line 197 "pars0grm.y"
                                { yyval = pars_op(PARS_NOTFOUND_TOKEN, yyvsp[-2], NULL); }
#line 1695 "pars0grm.cc"
    break;

  case 48: /* exp: PARS_SQL_TOKEN '%' PARS_NOTFOUND_TOKEN  */
#line 199 "pars0grm.y"
                                { yyval = pars_op(PARS_NOTFOUND_TOKEN, yyvsp[-2], NULL); }
#line 1701 "pars0grm.cc"
    break;

  case 49: /* function_name: PARS_TO_BINARY_TOKEN  */
#line 203 "pars0grm.y"
                                { yyval = &pars_to_binary_token; }
#line 1707 "pars0grm.cc"
    break;

  case 50: /* function_name: PARS_SUBSTR_TOKEN  */
#line 204 "pars0grm.y"
                                { yyval = &pars_substr_token; }
#line 1713 "pars0grm.cc"
    break;

  case 51: /* function_name: PARS_CONCAT_TOKEN  */
#line 205 "pars0grm.y"
                                { yyval = &pars_concat_token; }
#line 1719 "pars0grm.cc"
    break;

  case 52: /* function_name: PARS_INSTR_TOKEN  */
#line 206 "pars0grm.y"
                                { yyval = &pars_instr_token; }
#line 1725 "pars0grm.cc"
    break;

  case 53: /* function_name: PARS_LENGTH_TOKEN  */
#line 207 "pars0grm.y"
                                { yyval = &pars_length_token; }
#line 1731 "pars0grm.cc"
    break;

  case 54: /* user_function_call: PARS_ID_TOKEN '(' ')'  */
#line 211 "pars0grm.y"
                                { yyval = yyvsp[-2]; }
#line 1737 "pars0grm.cc"
    break;

  case 55: /* table_list: table_name  */
#line 215 "pars0grm.y"
                                { yyval = que_node_list_add_last(NULL, yyvsp[0]); }
#line 1743 "pars0grm.cc"
    break;

  case 56: /* table_list: table_list ',' table_name  */
#line 217 "pars0grm.y"
                                { yyval = que_node_list_add_last(yyvsp[-2], yyvsp[0]); }
#line 1749 "pars0grm.cc"
    break;

  case 57: /* variable_list: %empty  */
#line 221 "pars0grm.y"
                                { yyval = NULL; }
#line 1755 "pars0grm.cc"
    break;

  case 58: /* variable_list: PARS_ID_TOKEN  */
#line 222 "pars0grm.y"
                                { yyval = que_node_list_add_last(NULL, yyvsp[0]); }
#line 1761 "pars0grm.cc"
    break;

  case 59: /* variable_list: variable_list ',' PARS_ID_TOKEN  */
#line 224 "pars0grm.y"
                                { yyval = que_node_list_add_last(yyvsp[-2], yyvsp[0]); }
#line 1767 "pars0grm.cc"
    break;

  case 60: /* exp_list: %empty  */
#line 228 "pars0grm.y"
                                { yyval = NULL; }
#line 1773 "pars0grm.cc"
    break;

  case 61: /* exp_list: exp  */
#line 229 "pars0grm.y"
                                { yyval = que_node_list_add_last(NULL, yyvsp[0]);}
#line 1779 "pars0grm.cc"
    break;

  case 62: /* exp_list: exp_list ',' exp  */
#line 230 "pars0grm.y"
                                { yyval = que_node_list_add_last(yyvsp[-2], yyvsp[0]); }
#line 1785 "pars0grm.cc"
    break;

  case 63: /* select_item: exp  */
#line 234 "pars0grm.y"
                                { yyval = yyvsp[0]; }
#line 1791 "pars0grm.cc"
    break;

  case 64: /* select_item: PARS_COUNT_TOKEN '(' '*' ')'  */
#line 236 "pars0grm.y"
                                { yyval = pars_func(&pars_count_token,
					  que_node_list_add_last(NULL,
					    sym_tab_add_int_lit(
						pars_sym_tab_global, 1))); }
#line 1800 "pars0grm.cc"
    break;

  case 65: /* select_item_list: %empty  */
#line 243 "pars0grm.y"
                                { yyval = NULL; }
#line 1806 "pars0grm.cc"
    break;

  case 66: /* select_item_list: select_item  */
#line 244 "pars0grm.y"
                                { yyval = que_node_list_add_last(NULL, yyvsp[0]); }
#line 1812 "pars0grm.cc"
    break;

  case 67: /* select_item_list: select_item_list ',' select_item  */
#line 246 "pars0grm.y"
                                { yyval = que_node_list_add_last(yyvsp[-2], yyvsp[0]); }
#line 1818 "pars0grm.cc"
    break;

  case 68: /* select_list: '*'  */
#line 250 "pars0grm.y"
                                { yyval = pars_select_list(&pars_star_denoter,
								NULL); }
#line 1825 "pars0grm.cc"
    break;

  case 69: /* select_list: select_item_list PARS_INTO_TOKEN variable_list  */
#line 253 "pars0grm.y"
                                { yyval = pars_select_list(
					yyvsp[-2], static_cast<sym_node_t*>(yyvsp[0])); }
#line 1832 "pars0grm.cc"
    break;

  case 70: /* select_list: select_item_list  */
#line 255 "pars0grm.y"
                                { yyval = pars_select_list(yyvsp[0], NULL); }
#line 1838 "pars0grm.cc"
    break;

  case 71: /* search_condition: %empty  */
#line 259 "pars0grm.y"
                                { yyval = NULL; }
#line 1844 "pars0grm.cc"
    break;

  case 72: /* search_condition: PARS_WHERE_TOKEN exp  */
#line 260 "pars0grm.y"
                                { yyval = yyvsp[0]; }
#line 1850 "pars0grm.cc"
    break;

  case 73: /* for_update_clause: %empty  */
#line 264 "pars0grm.y"
                                { yyval = NULL; }
#line 1856 "pars0grm.cc"
    break;

  case 74: /* for_update_clause: PARS_FOR_TOKEN PARS_UPDATE_TOKEN  */
#line 266 "pars0grm.y"
                                { yyval = &pars_update_token; }
#line 1862 "pars0grm.cc"
    break;

  case 75: /* lock_shared_clause: %empty  */
#line 270 "pars0grm.y"
                                { yyval = NULL; }
#line 1868 "pars0grm.cc"
    break;

  case 76: /* lock_shared_clause: PARS_LOCK_TOKEN PARS_IN_TOKEN PARS_SHARE_TOKEN PARS_MODE_TOKEN  */
#line 272 "pars0grm.y"
                                { yyval = &pars_share_token; }
#line 1874 "pars0grm.cc"
    break;

  case 77: /* order_direction: %empty  */
#line 276 "pars0grm.y"
                                { yyval = &pars_asc_token; }
#line 1880 "pars0grm.cc"
    break;

  case 78: /* order_direction: PARS_ASC_TOKEN  */
#line 277 "pars0grm.y"
                                { yyval = &pars_asc_token; }
#line 1886 "pars0grm.cc"
    break;

  case 79: /* order_direction: PARS_DESC_TOKEN  */
#line 278 "pars0grm.y"
                                { yyval = &pars_desc_token; }
#line 1892 "pars0grm.cc"
    break;

  case 80: /* order_by_clause: %empty  */
#line 282 "pars0grm.y"
                                { yyval = NULL; }
#line 1898 "pars0grm.cc"
    break;

  case 81: /* order_by_clause: PARS_ORDER_TOKEN PARS_BY_TOKEN PARS_ID_TOKEN order_direction  */
#line 284 "pars0grm.y"
                                { yyval = pars_order_by(
					static_cast<sym_node_t*>(yyvsp[-1]),
					static_cast<pars_res_word_t*>(yyvsp[0])); }
#line 1906 "pars0grm.cc"
    break;

  case 82: /* select_statement: PARS_SELECT_TOKEN select_list PARS_FROM_TOKEN table_list search_condition for_update_clause lock_shared_clause order_by_clause  */
#line 295 "pars0grm.y"
                                { yyval = pars_select_statement(
					static_cast<sel_node_t*>(yyvsp[-6]),
					static_cast<sym_node_t*>(yyvsp[-4]),
					static_cast<que_node_t*>(yyvsp[-3]),
					static_cast<pars_res_word_t*>(yyvsp[-2]),
					static_cast<pars_res_word_t*>(yyvsp[-1]),
					static_cast<order_node_t*>(yyvsp[0])); }
#line 1918 "pars0grm.cc"
    break;

  case 83: /* insert_statement_start: PARS_INSERT_TOKEN PARS_INTO_TOKEN table_name  */
#line 306 "pars0grm.y"
                                { yyval = yyvsp[0]; }
#line 1924 "pars0grm.cc"
    break;

  case 84: /* insert_statement: insert_statement_start PARS_VALUES_TOKEN '(' exp_list ')'  */
#line 311 "pars0grm.y"
                                { yyval = pars_insert_statement(
					static_cast<sym_node_t*>(yyvsp[-4]), yyvsp[-1], NULL); }
#line 1931 "pars0grm.cc"
    break;

  case 85: /* insert_statement: insert_statement_start select_statement  */
#line 314 "pars0grm.y"
                                { yyval = pars_insert_statement(
					static_cast<sym_node_t*>(yyvsp[-1]),
					NULL,
					static_cast<sel_node_t*>(yyvsp[0])); }
#line 1940 "pars0grm.cc"
    break;

  case 86: /* column_assignment: PARS_ID_TOKEN '=' exp  */
#line 321 "pars0grm.y"
                                { yyval = pars_column_assignment(
					static_cast<sym_node_t*>(yyvsp[-2]),
					static_cast<que_node_t*>(yyvsp[0])); }
#line 1948 "pars0grm.cc"
    break;

  case 87: /* column_assignment_list: column_assignment  */
#line 327 "pars0grm.y"
                                { yyval = que_node_list_add_last(NULL, yyvsp[0]); }
#line 1954 "pars0grm.cc"
    break;

  case 88: /* column_assignment_list: column_assignment_list ',' column_assignment  */
#line 329 "pars0grm.y"
                                { yyval = que_node_list_add_last(yyvsp[-2], yyvsp[0]); }
#line 1960 "pars0grm.cc"
    break;

  case 89: /* cursor_positioned: PARS_WHERE_TOKEN PARS_CURRENT_TOKEN PARS_OF_TOKEN PARS_ID_TOKEN  */
#line 335 "pars0grm.y"
                                { yyval = yyvsp[0]; }
#line 1966 "pars0grm.cc"
    break;

  case 90: /* update_statement_start: PARS_UPDATE_TOKEN table_name PARS_SET_TOKEN column_assignment_list  */
#line 341 "pars0grm.y"
                                { yyval = pars_update_statement_start(
					FALSE,
					static_cast<sym_node_t*>(yyvsp[-2]),
					static_cast<col_assign_node_t*>(yyvsp[0])); }
#line 1975 "pars0grm.cc"
    break;

  case 91: /* update_statement_searched: update_statement_start search_condition  */
#line 349 "pars0grm.y"
                                { yyval = pars_update_statement(
					static_cast<upd_node_t*>(yyvsp[-1]),
					NULL,
					static_cast<que_node_t*>(yyvsp[0])); }
#line 1984 "pars0grm.cc"
    break;

  case 92: /* update_statement_positioned: update_statement_start cursor_positioned  */
#line 357 "pars0grm.y"
                                { yyval = pars_update_statement(
					static_cast<upd_node_t*>(yyvsp[-1]),
					static_cast<sym_node_t*>(yyvsp[0]),
					NULL); }
#line 1993 "pars0grm.cc"
    break;

  case 93: /* delete_statement_start: PARS_DELETE_TOKEN PARS_FROM_TOKEN table_name  */
#line 365 "pars0grm.y"
                                { yyval = pars_update_statement_start(
					TRUE,
					static_cast<sym_node_t*>(yyvsp[0]), NULL); }
#line 2001 "pars0grm.cc"
    break;

  case 94: /* delete_statement_searched: delete_statement_start search_condition  */
#line 372 "pars0grm.y"
                                { yyval = pars_update_statement(
					static_cast<upd_node_t*>(yyvsp[-1]),
					NULL,
					static_cast<que_node_t*>(yyvsp[0])); }
#line 2010 "pars0grm.cc"
    break;

  case 95: /* delete_statement_positioned: delete_statement_start cursor_positioned  */
#line 380 "pars0grm.y"
                                { yyval = pars_update_statement(
					static_cast<upd_node_t*>(yyvsp[-1]),
					static_cast<sym_node_t*>(yyvsp[0]),
					NULL); }
#line 2019 "pars0grm.cc"
    break;

  case 96: /* assignment_statement: PARS_ID_TOKEN PARS_ASSIGN_TOKEN exp  */
#line 388 "pars0grm.y"
                                { yyval = pars_assignment_statement(
					static_cast<sym_node_t*>(yyvsp[-2]),
					static_cast<que_node_t*>(yyvsp[0])); }
#line 2027 "pars0grm.cc"
    break;

  case 97: /* elsif_element: PARS_ELSIF_TOKEN exp PARS_THEN_TOKEN statement_list  */
#line 396 "pars0grm.y"
                                { yyval = pars_elsif_element(yyvsp[-2], yyvsp[0]); }
#line 2033 "pars0grm.cc"
    break;

  case 98: /* elsif_list: elsif_element  */
#line 400 "pars0grm.y"
                                { yyval = que_node_list_add_last(NULL, yyvsp[0]); }
#line 2039 "pars0grm.cc"
    break;

  case 99: /* elsif_list: elsif_list elsif_element  */
#line 402 "pars0grm.y"
                                { yyval = que_node_list_add_last(yyvsp[-1], yyvsp[0]); }
#line 2045 "pars0grm.cc"
    break;

  case 100: /* else_part: %empty  */
#line 406 "pars0grm.y"
                                { yyval = NULL; }
#line 2051 "pars0grm.cc"
    break;

  case 101: /* else_part: PARS_ELSE_TOKEN statement_list  */
#line 408 "pars0grm.y"
                                { yyval = yyvsp[0]; }
#line 2057 "pars0grm.cc"
    break;

  case 102: /* else_part: elsif_list  */
#line 409 "pars0grm.y"
                                { yyval = yyvsp[0]; }
#line 2063 "pars0grm.cc"
    break;

  case 103: /* if_statement: PARS_IF_TOKEN exp PARS_THEN_TOKEN statement_list else_part PARS_END_TOKEN PARS_IF_TOKEN  */
#line 416 "pars0grm.y"
                                { yyval = pars_if_statement(yyvsp[-5], yyvsp[-3], yyvsp[-2]); }
#line 2069 "pars0grm.cc"
    break;

  case 104: /* while_statement: PARS_WHILE_TOKEN exp PARS_LOOP_TOKEN statement_list PARS_END_TOKEN PARS_LOOP_TOKEN  */
#line 422 "pars0grm.y"
                                { yyval = pars_while_statement(yyvsp[-4], yyvsp[-2]); }
#line 2075 "pars0grm.cc"
    break;

  case 105: /* for_statement: PARS_FOR_TOKEN PARS_ID_TOKEN PARS_IN_TOKEN exp PARS_DDOT_TOKEN exp PARS_LOOP_TOKEN statement_list PARS_END_TOKEN PARS_LOOP_TOKEN  */
#line 430 "pars0grm.y"
                                { yyval = pars_for_statement(
					static_cast<sym_node_t*>(yyvsp[-8]),
					yyvsp[-6], yyvsp[-4], yyvsp[-2]); }
#line 2083 "pars0grm.cc"
    break;

  case 106: /* exit_statement: PARS_EXIT_TOKEN  */
#line 436 "pars0grm.y"
                                { yyval = pars_exit_statement(); }
#line 2089 "pars0grm.cc"
    break;

  case 107: /* return_statement: PARS_RETURN_TOKEN  */
#line 440 "pars0grm.y"
                                { yyval = pars_return_statement(); }
#line 2095 "pars0grm.cc"
    break;

  case 108: /* open_cursor_statement: PARS_OPEN_TOKEN PARS_ID_TOKEN  */
#line 445 "pars0grm.y"
                                { yyval = pars_open_statement(
						ROW_SEL_OPEN_CURSOR,
						static_cast<sym_node_t*>(yyvsp[0])); }
#line 2103 "pars0grm.cc"
    break;

  case 109: /* close_cursor_statement: PARS_CLOSE_TOKEN PARS_ID_TOKEN  */
#line 452 "pars0grm.y"
                                { yyval = pars_open_statement(
						ROW_SEL_CLOSE_CURSOR,
						static_cast<sym_node_t*>(yyvsp[0])); }
#line 2111 "pars0grm.cc"
    break;

  case 110: /* fetch_statement: PARS_FETCH_TOKEN PARS_ID_TOKEN PARS_INTO_TOKEN variable_list  */
#line 459 "pars0grm.y"
                                { yyval = pars_fetch_statement(
					static_cast<sym_node_t*>(yyvsp[-2]),
					static_cast<sym_node_t*>(yyvsp[0]), NULL); }
#line 2119 "pars0grm.cc"
    break;

  case 111: /* fetch_statement: PARS_FETCH_TOKEN PARS_ID_TOKEN PARS_INTO_TOKEN user_function_call  */
#line 463 "pars0grm.y"
                                { yyval = pars_fetch_statement(
					static_cast<sym_node_t*>(yyvsp[-2]),
					NULL,
					static_cast<sym_node_t*>(yyvsp[0])); }
#line 2128 "pars0grm.cc"
    break;

  case 112: /* column_def: PARS_ID_TOKEN type_name opt_column_len opt_not_null  */
#line 471 "pars0grm.y"
                                { yyval = pars_column_def(
					static_cast<sym_node_t*>(yyvsp[-3]),
					static_cast<pars_res_word_t*>(yyvsp[-2]),
					static_cast<sym_node_t*>(yyvsp[-1]),
					yyvsp[0]); }
#line 2138 "pars0grm.cc"
    break;

  case 113: /* column_def_list: column_def  */
#line 479 "pars0grm.y"
                                { yyval = que_node_list_add_last(NULL, yyvsp[0]); }
#line 2144 "pars0grm.cc"
    break;

  case 114: /* column_def_list: column_def_list ',' column_def  */
#line 481 "pars0grm.y"
                                { yyval = que_node_list_add_last(yyvsp[-2], yyvsp[0]); }
#line 2150 "pars0grm.cc"
    break;

  case 115: /* opt_column_len: %empty  */
#line 485 "pars0grm.y"
                                { yyval = NULL; }
#line 2156 "pars0grm.cc"
    break;

  case 116: /* opt_column_len: '(' PARS_INT_LIT ')'  */
#line 487 "pars0grm.y"
                                { yyval = yyvsp[-1]; }
#line 2162 "pars0grm.cc"
    break;

  case 117: /* opt_not_null: %empty  */
#line 491 "pars0grm.y"
                                { yyval = NULL; }
#line 2168 "pars0grm.cc"
    break;

  case 118: /* opt_not_null: PARS_NOT_TOKEN PARS_NULL_LIT  */
#line 493 "pars0grm.y"
                                { yyval = &pars_int_token;
					/* pass any non-NULL pointer */ }
#line 2175 "pars0grm.cc"
    break;

  case 119: /* create_table: PARS_CREATE_TOKEN PARS_TABLE_TOKEN table_name '(' column_def_list ')'  */
#line 500 "pars0grm.y"
                                { yyval = pars_create_table(
					static_cast<sym_node_t*>(yyvsp[-3]),
					static_cast<sym_node_t*>(yyvsp[-1])); }
#line 2183 "pars0grm.cc"
    break;

  case 120: /* column_list: PARS_ID_TOKEN  */
#line 506 "pars0grm.y"
                                { yyval = que_node_list_add_last(NULL, yyvsp[0]); }
#line 2189 "pars0grm.cc"
    break;

  case 121: /* column_list: column_list ',' PARS_ID_TOKEN  */
#line 508 "pars0grm.y"
                                { yyval = que_node_list_add_last(yyvsp[-2], yyvsp[0]); }
#line 2195 "pars0grm.cc"
    break;

  case 122: /* unique_def: %empty  */
#line 512 "pars0grm.y"
                                { yyval = NULL; }
#line 2201 "pars0grm.cc"
    break;

  case 123: /* unique_def: PARS_UNIQUE_TOKEN  */
#line 513 "pars0grm.y"
                                { yyval = &pars_unique_token; }
#line 2207 "pars0grm.cc"
    break;

  case 124: /* clustered_def: %empty  */
#line 517 "pars0grm.y"
                                { yyval = NULL; }
#line 2213 "pars0grm.cc"
    break;

  case 125: /* clustered_def: PARS_CLUSTERED_TOKEN  */
#line 518 "pars0grm.y"
                                { yyval = &pars_clustered_token; }
#line 2219 "pars0grm.cc"
    break;

  case 126: /* create_index: PARS_CREATE_TOKEN unique_def clustered_def PARS_INDEX_TOKEN PARS_ID_TOKEN PARS_ON_TOKEN table_name '(' column_list ')'  */
#line 527 "pars0grm.y"
                                { yyval = pars_create_index(
					static_cast<pars_res_word_t*>(yyvsp[-8]),
					static_cast<pars_res_word_t*>(yyvsp[-7]),
					static_cast<sym_node_t*>(yyvsp[-5]),
					static_cast<sym_node_t*>(yyvsp[-3]),
					static_cast<sym_node_t*>(yyvsp[-1])); }
#line 2230 "pars0grm.cc"
    break;

  case 127: /* table_name: PARS_ID_TOKEN  */
#line 536 "pars0grm.y"
                                { yyval = yyvsp[0]; }
#line 2236 "pars0grm.cc"
    break;

  case 128: /* table_name: PARS_TABLE_NAME_TOKEN  */
#line 537 "pars0grm.y"
                                { yyval = yyvsp[0]; }
#line 2242 "pars0grm.cc"
    break;

  case 129: /* commit_statement: PARS_COMMIT_TOKEN PARS_WORK_TOKEN  */
#line 542 "pars0grm.y"
                                { yyval = pars_commit_statement(); }
#line 2248 "pars0grm.cc"
    break;

  case 130: /* rollback_statement: PARS_ROLLBACK_TOKEN PARS_WORK_TOKEN  */
#line 547 "pars0grm.y"
                                { yyval = pars_rollback_statement(); }
#line 2254 "pars0grm.cc"
    break;

  case 131: /* type_name: PARS_INT_TOKEN  */
#line 551 "pars0grm.y"
                                { yyval = &pars_int_token; }
#line 2260 "pars0grm.cc"
    break;

  case 132: /* type_name: PARS_BIGINT_TOKEN  */
#line 552 "pars0grm.y"
                                { yyval = &pars_bigint_token; }
#line 2266 "pars0grm.cc"
    break;

  case 133: /* type_name: PARS_CHAR_TOKEN  */
#line 553 "pars0grm.y"
                                { yyval = &pars_char_token; }
#line 2272 "pars0grm.cc"
    break;

  case 134: /* variable_declaration: PARS_ID_TOKEN type_name ';'  */
#line 558 "pars0grm.y"
                                { yyval = pars_variable_declaration(
					static_cast<sym_node_t*>(yyvsp[-2]),
					static_cast<pars_res_word_t*>(yyvsp[-1])); }
#line 2280 "pars0grm.cc"
    break;

  case 138: /* cursor_declaration: PARS_DECLARE_TOKEN PARS_CURSOR_TOKEN PARS_ID_TOKEN PARS_IS_TOKEN select_statement ';'  */
#line 572 "pars0grm.y"
                                { yyval = pars_cursor_declaration(
					static_cast<sym_node_t*>(yyvsp[-3]),
					static_cast<sel_node_t*>(yyvsp[-1])); }
#line 2288 "pars0grm.cc"
    break;

  case 139: /* function_declaration: PARS_DECLARE_TOKEN PARS_FUNCTION_TOKEN PARS_ID_TOKEN ';'  */
#line 579 "pars0grm.y"
                                { yyval = pars_function_declaration(
					static_cast<sym_node_t*>(yyvsp[-1])); }
#line 2295 "pars0grm.cc"
    break;

  case 145: /* procedure_definition: PARS_PROCEDURE_TOKEN PARS_ID_TOKEN '(' ')' PARS_IS_TOKEN variable_declaration_list declaration_list PARS_BEGIN_TOKEN statement_list PARS_END_TOKEN  */
#line 601 "pars0grm.y"
                                { yyval = pars_procedure_definition(
					static_cast<sym_node_t*>(yyvsp[-8]), yyvsp[-1]); }
#line 2302 "pars0grm.cc"
    break;


#line 2306 "pars0grm.cc"

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
  YY_SYMBOL_PRINT ("-> $$ =", YY_CAST (yysymbol_kind_t, yyr1[yyn]), &yyval, &yyloc);

  YYPOPSTACK (yylen);
  yylen = 0;

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
  yytoken = yychar == YYEMPTY ? YYSYMBOL_YYEMPTY : YYTRANSLATE (yychar);
  /* If not already recovering from an error, report this error.  */
  if (!yyerrstatus)
    {
      ++yynerrs;
      yyerror (YY_("syntax error"));
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

  /* Pop stack until we find a state that shifts the error token.  */
  for (;;)
    {
      yyn = yypact[yystate];
      if (!yypact_value_is_default (yyn))
        {
          yyn += YYSYMBOL_YYerror;
          if (0 <= yyn && yyn <= YYLAST && yycheck[yyn] == YYSYMBOL_YYerror)
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
                  YY_ACCESSING_SYMBOL (yystate), yyvsp);
      YYPOPSTACK (1);
      yystate = *yyssp;
      YY_STACK_PRINT (yyss, yyssp);
    }

  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  *++yyvsp = yylval;
  YY_IGNORE_MAYBE_UNINITIALIZED_END


  /* Shift the error token.  */
  YY_SYMBOL_PRINT ("Shifting", YY_ACCESSING_SYMBOL (yyn), yyvsp, yylsp);

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


#if !defined yyoverflow
/*-------------------------------------------------.
| yyexhaustedlab -- memory exhaustion comes here.  |
`-------------------------------------------------*/
yyexhaustedlab:
  yyerror (YY_("memory exhausted"));
  yyresult = 2;
  goto yyreturn;
#endif


/*-------------------------------------------------------.
| yyreturn -- parsing is finished, clean up and return.  |
`-------------------------------------------------------*/
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
                  YY_ACCESSING_SYMBOL (+*yyssp), yyvsp);
      YYPOPSTACK (1);
    }
#ifndef yyoverflow
  if (yyss != yyssa)
    YYSTACK_FREE (yyss);
#endif

  return yyresult;
}

#line 605 "pars0grm.y"

