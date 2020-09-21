/* A Bison parser, made by GNU Bison 3.7.  */

/* Bison implementation for Yacc-like parsers in C

   Copyright (C) 1984, 1989-1990, 2000-2015, 2018-2020 Free Software Foundation,
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

/* DO NOT RELY ON FEATURES THAT ARE NOT DOCUMENTED in the manual,
   especially those whose name start with YY_ or yy_.  They are
   private implementation details that can be changed or removed.  */

/* All symbols defined below should begin with yy or YY, to avoid
   infringing on user name space.  This should be done even for local
   variables, as they might otherwise be expanded by user macros.
   There are some unavoidable exceptions within include files to
   define necessary library symbols; they are noted "INFRINGES ON
   USER NAME SPACE" below.  */

/* Identify Bison output.  */
#define YYBISON 1

/* Bison version.  */
#define YYBISON_VERSION "3.7"

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
  YYSYMBOL_92_ = 92,                       /* '?'  */
  YYSYMBOL_93_ = 93,                       /* ','  */
  YYSYMBOL_94_ = 94,                       /* '{'  */
  YYSYMBOL_95_ = 95,                       /* '}'  */
  YYSYMBOL_YYACCEPT = 96,                  /* $accept  */
  YYSYMBOL_top_statement = 97,             /* top_statement  */
  YYSYMBOL_statement = 98,                 /* statement  */
  YYSYMBOL_statement_list = 99,            /* statement_list  */
  YYSYMBOL_exp = 100,                      /* exp  */
  YYSYMBOL_function_name = 101,            /* function_name  */
  YYSYMBOL_question_mark_list = 102,       /* question_mark_list  */
  YYSYMBOL_stored_procedure_call = 103,    /* stored_procedure_call  */
  YYSYMBOL_user_function_call = 104,       /* user_function_call  */
  YYSYMBOL_table_list = 105,               /* table_list  */
  YYSYMBOL_variable_list = 106,            /* variable_list  */
  YYSYMBOL_exp_list = 107,                 /* exp_list  */
  YYSYMBOL_select_item = 108,              /* select_item  */
  YYSYMBOL_select_item_list = 109,         /* select_item_list  */
  YYSYMBOL_select_list = 110,              /* select_list  */
  YYSYMBOL_search_condition = 111,         /* search_condition  */
  YYSYMBOL_for_update_clause = 112,        /* for_update_clause  */
  YYSYMBOL_lock_shared_clause = 113,       /* lock_shared_clause  */
  YYSYMBOL_order_direction = 114,          /* order_direction  */
  YYSYMBOL_order_by_clause = 115,          /* order_by_clause  */
  YYSYMBOL_select_statement = 116,         /* select_statement  */
  YYSYMBOL_insert_statement_start = 117,   /* insert_statement_start  */
  YYSYMBOL_insert_statement = 118,         /* insert_statement  */
  YYSYMBOL_column_assignment = 119,        /* column_assignment  */
  YYSYMBOL_column_assignment_list = 120,   /* column_assignment_list  */
  YYSYMBOL_cursor_positioned = 121,        /* cursor_positioned  */
  YYSYMBOL_update_statement_start = 122,   /* update_statement_start  */
  YYSYMBOL_update_statement_searched = 123, /* update_statement_searched  */
  YYSYMBOL_update_statement_positioned = 124, /* update_statement_positioned  */
  YYSYMBOL_delete_statement_start = 125,   /* delete_statement_start  */
  YYSYMBOL_delete_statement_searched = 126, /* delete_statement_searched  */
  YYSYMBOL_delete_statement_positioned = 127, /* delete_statement_positioned  */
  YYSYMBOL_assignment_statement = 128,     /* assignment_statement  */
  YYSYMBOL_elsif_element = 129,            /* elsif_element  */
  YYSYMBOL_elsif_list = 130,               /* elsif_list  */
  YYSYMBOL_else_part = 131,                /* else_part  */
  YYSYMBOL_if_statement = 132,             /* if_statement  */
  YYSYMBOL_while_statement = 133,          /* while_statement  */
  YYSYMBOL_for_statement = 134,            /* for_statement  */
  YYSYMBOL_exit_statement = 135,           /* exit_statement  */
  YYSYMBOL_return_statement = 136,         /* return_statement  */
  YYSYMBOL_open_cursor_statement = 137,    /* open_cursor_statement  */
  YYSYMBOL_close_cursor_statement = 138,   /* close_cursor_statement  */
  YYSYMBOL_fetch_statement = 139,          /* fetch_statement  */
  YYSYMBOL_column_def = 140,               /* column_def  */
  YYSYMBOL_column_def_list = 141,          /* column_def_list  */
  YYSYMBOL_opt_column_len = 142,           /* opt_column_len  */
  YYSYMBOL_opt_not_null = 143,             /* opt_not_null  */
  YYSYMBOL_create_table = 144,             /* create_table  */
  YYSYMBOL_column_list = 145,              /* column_list  */
  YYSYMBOL_unique_def = 146,               /* unique_def  */
  YYSYMBOL_clustered_def = 147,            /* clustered_def  */
  YYSYMBOL_create_index = 148,             /* create_index  */
  YYSYMBOL_table_name = 149,               /* table_name  */
  YYSYMBOL_commit_statement = 150,         /* commit_statement  */
  YYSYMBOL_rollback_statement = 151,       /* rollback_statement  */
  YYSYMBOL_type_name = 152,                /* type_name  */
  YYSYMBOL_variable_declaration = 153,     /* variable_declaration  */
  YYSYMBOL_variable_declaration_list = 154, /* variable_declaration_list  */
  YYSYMBOL_cursor_declaration = 155,       /* cursor_declaration  */
  YYSYMBOL_function_declaration = 156,     /* function_declaration  */
  YYSYMBOL_declaration = 157,              /* declaration  */
  YYSYMBOL_declaration_list = 158,         /* declaration_list  */
  YYSYMBOL_procedure_definition = 159      /* procedure_definition  */
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
# define YYUSE(E) ((void) (E))
#else
# define YYUSE(E) /* empty */
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
#define YYLAST   623

/* YYNTOKENS -- Number of terminals.  */
#define YYNTOKENS  96
/* YYNNTS -- Number of nonterminals.  */
#define YYNNTS  64
/* YYNRULES -- Number of rules.  */
#define YYNRULES  150
/* YYNSTATES -- Number of states.  */
#define YYNSTATES  301

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
static const yytype_int16 yyrline[] =
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
  "'('", "')'", "'?'", "','", "'{'", "'}'", "$accept", "top_statement",
  "statement", "statement_list", "exp", "function_name",
  "question_mark_list", "stored_procedure_call", "user_function_call",
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
      40,    41,    63,    44,   123,   125
};
#endif

#define YYPACT_NINF (-162)

#define yypact_value_is_default(Yyn) \
  ((Yyn) == YYPACT_NINF)

#define YYTABLE_NINF (-1)

#define yytable_value_is_error(Yyn) \
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
static const yytype_int16 yytable[] =
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
static const yytype_int8 yyr2[] =
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
  YYUSE (yyoutput);
  if (!yyvaluep)
    return;
# ifdef YYPRINT
  if (yykind < YYNTOKENS)
    YYPRINT (yyo, yytoknum[yykind], *yyvaluep);
# endif
  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  YYUSE (yykind);
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
  YYUSE (yyvaluep);
  if (!yymsg)
    yymsg = "Deleting";
  YY_SYMBOL_PRINT (yymsg, yykind, yyvaluep, yylocationp);

  YY_IGNORE_MAYBE_UNINITIALIZED_BEGIN
  YYUSE (yykind);
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
  case 23: /* statement_list: statement  */
#line 166 "pars0grm.y"
                                { yyval = que_node_list_add_last(NULL, yyvsp[0]); }
#line 1550 "pars0grm.cc"
    break;

  case 24: /* statement_list: statement_list statement  */
#line 168 "pars0grm.y"
                                { yyval = que_node_list_add_last(yyvsp[-1], yyvsp[0]); }
#line 1556 "pars0grm.cc"
    break;

  case 25: /* exp: PARS_ID_TOKEN  */
#line 172 "pars0grm.y"
                                { yyval = yyvsp[0];}
#line 1562 "pars0grm.cc"
    break;

  case 26: /* exp: function_name '(' exp_list ')'  */
#line 174 "pars0grm.y"
                                { yyval = pars_func(yyvsp[-3], yyvsp[-1]); }
#line 1568 "pars0grm.cc"
    break;

  case 27: /* exp: PARS_INT_LIT  */
#line 175 "pars0grm.y"
                                { yyval = yyvsp[0];}
#line 1574 "pars0grm.cc"
    break;

  case 28: /* exp: PARS_FLOAT_LIT  */
#line 176 "pars0grm.y"
                                { yyval = yyvsp[0];}
#line 1580 "pars0grm.cc"
    break;

  case 29: /* exp: PARS_STR_LIT  */
#line 177 "pars0grm.y"
                                { yyval = yyvsp[0];}
#line 1586 "pars0grm.cc"
    break;

  case 30: /* exp: PARS_NULL_LIT  */
#line 178 "pars0grm.y"
                                { yyval = yyvsp[0];}
#line 1592 "pars0grm.cc"
    break;

  case 31: /* exp: PARS_SQL_TOKEN  */
#line 179 "pars0grm.y"
                                { yyval = yyvsp[0];}
#line 1598 "pars0grm.cc"
    break;

  case 32: /* exp: exp '+' exp  */
#line 180 "pars0grm.y"
                                { yyval = pars_op('+', yyvsp[-2], yyvsp[0]); }
#line 1604 "pars0grm.cc"
    break;

  case 33: /* exp: exp '-' exp  */
#line 181 "pars0grm.y"
                                { yyval = pars_op('-', yyvsp[-2], yyvsp[0]); }
#line 1610 "pars0grm.cc"
    break;

  case 34: /* exp: exp '*' exp  */
#line 182 "pars0grm.y"
                                { yyval = pars_op('*', yyvsp[-2], yyvsp[0]); }
#line 1616 "pars0grm.cc"
    break;

  case 35: /* exp: exp '/' exp  */
#line 183 "pars0grm.y"
                                { yyval = pars_op('/', yyvsp[-2], yyvsp[0]); }
#line 1622 "pars0grm.cc"
    break;

  case 36: /* exp: '-' exp  */
#line 184 "pars0grm.y"
                                { yyval = pars_op('-', yyvsp[0], NULL); }
#line 1628 "pars0grm.cc"
    break;

  case 37: /* exp: '(' exp ')'  */
#line 185 "pars0grm.y"
                                { yyval = yyvsp[-1]; }
#line 1634 "pars0grm.cc"
    break;

  case 38: /* exp: exp '=' exp  */
#line 186 "pars0grm.y"
                                { yyval = pars_op('=', yyvsp[-2], yyvsp[0]); }
#line 1640 "pars0grm.cc"
    break;

  case 39: /* exp: exp PARS_LIKE_TOKEN PARS_STR_LIT  */
#line 188 "pars0grm.y"
                                { yyval = pars_op(PARS_LIKE_TOKEN, yyvsp[-2], yyvsp[0]); }
#line 1646 "pars0grm.cc"
    break;

  case 40: /* exp: exp '<' exp  */
#line 189 "pars0grm.y"
                                { yyval = pars_op('<', yyvsp[-2], yyvsp[0]); }
#line 1652 "pars0grm.cc"
    break;

  case 41: /* exp: exp '>' exp  */
#line 190 "pars0grm.y"
                                { yyval = pars_op('>', yyvsp[-2], yyvsp[0]); }
#line 1658 "pars0grm.cc"
    break;

  case 42: /* exp: exp PARS_GE_TOKEN exp  */
#line 191 "pars0grm.y"
                                { yyval = pars_op(PARS_GE_TOKEN, yyvsp[-2], yyvsp[0]); }
#line 1664 "pars0grm.cc"
    break;

  case 43: /* exp: exp PARS_LE_TOKEN exp  */
#line 192 "pars0grm.y"
                                { yyval = pars_op(PARS_LE_TOKEN, yyvsp[-2], yyvsp[0]); }
#line 1670 "pars0grm.cc"
    break;

  case 44: /* exp: exp PARS_NE_TOKEN exp  */
#line 193 "pars0grm.y"
                                { yyval = pars_op(PARS_NE_TOKEN, yyvsp[-2], yyvsp[0]); }
#line 1676 "pars0grm.cc"
    break;

  case 45: /* exp: exp PARS_AND_TOKEN exp  */
#line 194 "pars0grm.y"
                                { yyval = pars_op(PARS_AND_TOKEN, yyvsp[-2], yyvsp[0]); }
#line 1682 "pars0grm.cc"
    break;

  case 46: /* exp: exp PARS_OR_TOKEN exp  */
#line 195 "pars0grm.y"
                                { yyval = pars_op(PARS_OR_TOKEN, yyvsp[-2], yyvsp[0]); }
#line 1688 "pars0grm.cc"
    break;

  case 47: /* exp: PARS_NOT_TOKEN exp  */
#line 196 "pars0grm.y"
                                { yyval = pars_op(PARS_NOT_TOKEN, yyvsp[0], NULL); }
#line 1694 "pars0grm.cc"
    break;

  case 48: /* exp: PARS_ID_TOKEN '%' PARS_NOTFOUND_TOKEN  */
#line 198 "pars0grm.y"
                                { yyval = pars_op(PARS_NOTFOUND_TOKEN, yyvsp[-2], NULL); }
#line 1700 "pars0grm.cc"
    break;

  case 49: /* exp: PARS_SQL_TOKEN '%' PARS_NOTFOUND_TOKEN  */
#line 200 "pars0grm.y"
                                { yyval = pars_op(PARS_NOTFOUND_TOKEN, yyvsp[-2], NULL); }
#line 1706 "pars0grm.cc"
    break;

  case 50: /* function_name: PARS_TO_BINARY_TOKEN  */
#line 204 "pars0grm.y"
                                { yyval = &pars_to_binary_token; }
#line 1712 "pars0grm.cc"
    break;

  case 51: /* function_name: PARS_SUBSTR_TOKEN  */
#line 205 "pars0grm.y"
                                { yyval = &pars_substr_token; }
#line 1718 "pars0grm.cc"
    break;

  case 52: /* function_name: PARS_CONCAT_TOKEN  */
#line 206 "pars0grm.y"
                                { yyval = &pars_concat_token; }
#line 1724 "pars0grm.cc"
    break;

  case 53: /* function_name: PARS_INSTR_TOKEN  */
#line 207 "pars0grm.y"
                                { yyval = &pars_instr_token; }
#line 1730 "pars0grm.cc"
    break;

  case 54: /* function_name: PARS_LENGTH_TOKEN  */
#line 208 "pars0grm.y"
                                { yyval = &pars_length_token; }
#line 1736 "pars0grm.cc"
    break;

  case 58: /* stored_procedure_call: '{' PARS_ID_TOKEN '(' question_mark_list ')' '}'  */
#line 219 "pars0grm.y"
                                { yyval = pars_stored_procedure_call(
					static_cast<sym_node_t*>(yyvsp[-4])); }
#line 1743 "pars0grm.cc"
    break;

  case 59: /* user_function_call: PARS_ID_TOKEN '(' ')'  */
#line 224 "pars0grm.y"
                                { yyval = yyvsp[-2]; }
#line 1749 "pars0grm.cc"
    break;

  case 60: /* table_list: table_name  */
#line 228 "pars0grm.y"
                                { yyval = que_node_list_add_last(NULL, yyvsp[0]); }
#line 1755 "pars0grm.cc"
    break;

  case 61: /* table_list: table_list ',' table_name  */
#line 230 "pars0grm.y"
                                { yyval = que_node_list_add_last(yyvsp[-2], yyvsp[0]); }
#line 1761 "pars0grm.cc"
    break;

  case 62: /* variable_list: %empty  */
#line 234 "pars0grm.y"
                                { yyval = NULL; }
#line 1767 "pars0grm.cc"
    break;

  case 63: /* variable_list: PARS_ID_TOKEN  */
#line 235 "pars0grm.y"
                                { yyval = que_node_list_add_last(NULL, yyvsp[0]); }
#line 1773 "pars0grm.cc"
    break;

  case 64: /* variable_list: variable_list ',' PARS_ID_TOKEN  */
#line 237 "pars0grm.y"
                                { yyval = que_node_list_add_last(yyvsp[-2], yyvsp[0]); }
#line 1779 "pars0grm.cc"
    break;

  case 65: /* exp_list: %empty  */
#line 241 "pars0grm.y"
                                { yyval = NULL; }
#line 1785 "pars0grm.cc"
    break;

  case 66: /* exp_list: exp  */
#line 242 "pars0grm.y"
                                { yyval = que_node_list_add_last(NULL, yyvsp[0]);}
#line 1791 "pars0grm.cc"
    break;

  case 67: /* exp_list: exp_list ',' exp  */
#line 243 "pars0grm.y"
                                { yyval = que_node_list_add_last(yyvsp[-2], yyvsp[0]); }
#line 1797 "pars0grm.cc"
    break;

  case 68: /* select_item: exp  */
#line 247 "pars0grm.y"
                                { yyval = yyvsp[0]; }
#line 1803 "pars0grm.cc"
    break;

  case 69: /* select_item: PARS_COUNT_TOKEN '(' '*' ')'  */
#line 249 "pars0grm.y"
                                { yyval = pars_func(&pars_count_token,
					  que_node_list_add_last(NULL,
					    sym_tab_add_int_lit(
						pars_sym_tab_global, 1))); }
#line 1812 "pars0grm.cc"
    break;

  case 70: /* select_item_list: %empty  */
#line 256 "pars0grm.y"
                                { yyval = NULL; }
#line 1818 "pars0grm.cc"
    break;

  case 71: /* select_item_list: select_item  */
#line 257 "pars0grm.y"
                                { yyval = que_node_list_add_last(NULL, yyvsp[0]); }
#line 1824 "pars0grm.cc"
    break;

  case 72: /* select_item_list: select_item_list ',' select_item  */
#line 259 "pars0grm.y"
                                { yyval = que_node_list_add_last(yyvsp[-2], yyvsp[0]); }
#line 1830 "pars0grm.cc"
    break;

  case 73: /* select_list: '*'  */
#line 263 "pars0grm.y"
                                { yyval = pars_select_list(&pars_star_denoter,
								NULL); }
#line 1837 "pars0grm.cc"
    break;

  case 74: /* select_list: select_item_list PARS_INTO_TOKEN variable_list  */
#line 266 "pars0grm.y"
                                { yyval = pars_select_list(
					yyvsp[-2], static_cast<sym_node_t*>(yyvsp[0])); }
#line 1844 "pars0grm.cc"
    break;

  case 75: /* select_list: select_item_list  */
#line 268 "pars0grm.y"
                                { yyval = pars_select_list(yyvsp[0], NULL); }
#line 1850 "pars0grm.cc"
    break;

  case 76: /* search_condition: %empty  */
#line 272 "pars0grm.y"
                                { yyval = NULL; }
#line 1856 "pars0grm.cc"
    break;

  case 77: /* search_condition: PARS_WHERE_TOKEN exp  */
#line 273 "pars0grm.y"
                                { yyval = yyvsp[0]; }
#line 1862 "pars0grm.cc"
    break;

  case 78: /* for_update_clause: %empty  */
#line 277 "pars0grm.y"
                                { yyval = NULL; }
#line 1868 "pars0grm.cc"
    break;

  case 79: /* for_update_clause: PARS_FOR_TOKEN PARS_UPDATE_TOKEN  */
#line 279 "pars0grm.y"
                                { yyval = &pars_update_token; }
#line 1874 "pars0grm.cc"
    break;

  case 80: /* lock_shared_clause: %empty  */
#line 283 "pars0grm.y"
                                { yyval = NULL; }
#line 1880 "pars0grm.cc"
    break;

  case 81: /* lock_shared_clause: PARS_LOCK_TOKEN PARS_IN_TOKEN PARS_SHARE_TOKEN PARS_MODE_TOKEN  */
#line 285 "pars0grm.y"
                                { yyval = &pars_share_token; }
#line 1886 "pars0grm.cc"
    break;

  case 82: /* order_direction: %empty  */
#line 289 "pars0grm.y"
                                { yyval = &pars_asc_token; }
#line 1892 "pars0grm.cc"
    break;

  case 83: /* order_direction: PARS_ASC_TOKEN  */
#line 290 "pars0grm.y"
                                { yyval = &pars_asc_token; }
#line 1898 "pars0grm.cc"
    break;

  case 84: /* order_direction: PARS_DESC_TOKEN  */
#line 291 "pars0grm.y"
                                { yyval = &pars_desc_token; }
#line 1904 "pars0grm.cc"
    break;

  case 85: /* order_by_clause: %empty  */
#line 295 "pars0grm.y"
                                { yyval = NULL; }
#line 1910 "pars0grm.cc"
    break;

  case 86: /* order_by_clause: PARS_ORDER_TOKEN PARS_BY_TOKEN PARS_ID_TOKEN order_direction  */
#line 297 "pars0grm.y"
                                { yyval = pars_order_by(
					static_cast<sym_node_t*>(yyvsp[-1]),
					static_cast<pars_res_word_t*>(yyvsp[0])); }
#line 1918 "pars0grm.cc"
    break;

  case 87: /* select_statement: PARS_SELECT_TOKEN select_list PARS_FROM_TOKEN table_list search_condition for_update_clause lock_shared_clause order_by_clause  */
#line 308 "pars0grm.y"
                                { yyval = pars_select_statement(
					static_cast<sel_node_t*>(yyvsp[-6]),
					static_cast<sym_node_t*>(yyvsp[-4]),
					static_cast<que_node_t*>(yyvsp[-3]),
					static_cast<pars_res_word_t*>(yyvsp[-2]),
					static_cast<pars_res_word_t*>(yyvsp[-1]),
					static_cast<order_node_t*>(yyvsp[0])); }
#line 1930 "pars0grm.cc"
    break;

  case 88: /* insert_statement_start: PARS_INSERT_TOKEN PARS_INTO_TOKEN table_name  */
#line 319 "pars0grm.y"
                                { yyval = yyvsp[0]; }
#line 1936 "pars0grm.cc"
    break;

  case 89: /* insert_statement: insert_statement_start PARS_VALUES_TOKEN '(' exp_list ')'  */
#line 324 "pars0grm.y"
                                { if (!(yyval = pars_insert_statement(
					static_cast<sym_node_t*>(yyvsp[-4]), yyvsp[-1], NULL)))
					YYABORT; }
#line 1944 "pars0grm.cc"
    break;

  case 90: /* insert_statement: insert_statement_start select_statement  */
#line 328 "pars0grm.y"
                                { if (!(yyval = pars_insert_statement(
					static_cast<sym_node_t*>(yyvsp[-1]),
					NULL,
					static_cast<sel_node_t*>(yyvsp[0]))))
					YYABORT; }
#line 1954 "pars0grm.cc"
    break;

  case 91: /* column_assignment: PARS_ID_TOKEN '=' exp  */
#line 336 "pars0grm.y"
                                { yyval = pars_column_assignment(
					static_cast<sym_node_t*>(yyvsp[-2]),
					static_cast<que_node_t*>(yyvsp[0])); }
#line 1962 "pars0grm.cc"
    break;

  case 92: /* column_assignment_list: column_assignment  */
#line 342 "pars0grm.y"
                                { yyval = que_node_list_add_last(NULL, yyvsp[0]); }
#line 1968 "pars0grm.cc"
    break;

  case 93: /* column_assignment_list: column_assignment_list ',' column_assignment  */
#line 344 "pars0grm.y"
                                { yyval = que_node_list_add_last(yyvsp[-2], yyvsp[0]); }
#line 1974 "pars0grm.cc"
    break;

  case 94: /* cursor_positioned: PARS_WHERE_TOKEN PARS_CURRENT_TOKEN PARS_OF_TOKEN PARS_ID_TOKEN  */
#line 350 "pars0grm.y"
                                { yyval = yyvsp[0]; }
#line 1980 "pars0grm.cc"
    break;

  case 95: /* update_statement_start: PARS_UPDATE_TOKEN table_name PARS_SET_TOKEN column_assignment_list  */
#line 356 "pars0grm.y"
                                { yyval = pars_update_statement_start(
					FALSE,
					static_cast<sym_node_t*>(yyvsp[-2]),
					static_cast<col_assign_node_t*>(yyvsp[0])); }
#line 1989 "pars0grm.cc"
    break;

  case 96: /* update_statement_searched: update_statement_start search_condition  */
#line 364 "pars0grm.y"
                                { yyval = pars_update_statement(
					static_cast<upd_node_t*>(yyvsp[-1]),
					NULL,
					static_cast<que_node_t*>(yyvsp[0])); }
#line 1998 "pars0grm.cc"
    break;

  case 97: /* update_statement_positioned: update_statement_start cursor_positioned  */
#line 372 "pars0grm.y"
                                { yyval = pars_update_statement(
					static_cast<upd_node_t*>(yyvsp[-1]),
					static_cast<sym_node_t*>(yyvsp[0]),
					NULL); }
#line 2007 "pars0grm.cc"
    break;

  case 98: /* delete_statement_start: PARS_DELETE_TOKEN PARS_FROM_TOKEN table_name  */
#line 380 "pars0grm.y"
                                { yyval = pars_update_statement_start(
					TRUE,
					static_cast<sym_node_t*>(yyvsp[0]), NULL); }
#line 2015 "pars0grm.cc"
    break;

  case 99: /* delete_statement_searched: delete_statement_start search_condition  */
#line 387 "pars0grm.y"
                                { yyval = pars_update_statement(
					static_cast<upd_node_t*>(yyvsp[-1]),
					NULL,
					static_cast<que_node_t*>(yyvsp[0])); }
#line 2024 "pars0grm.cc"
    break;

  case 100: /* delete_statement_positioned: delete_statement_start cursor_positioned  */
#line 395 "pars0grm.y"
                                { yyval = pars_update_statement(
					static_cast<upd_node_t*>(yyvsp[-1]),
					static_cast<sym_node_t*>(yyvsp[0]),
					NULL); }
#line 2033 "pars0grm.cc"
    break;

  case 101: /* assignment_statement: PARS_ID_TOKEN PARS_ASSIGN_TOKEN exp  */
#line 403 "pars0grm.y"
                                { yyval = pars_assignment_statement(
					static_cast<sym_node_t*>(yyvsp[-2]),
					static_cast<que_node_t*>(yyvsp[0])); }
#line 2041 "pars0grm.cc"
    break;

  case 102: /* elsif_element: PARS_ELSIF_TOKEN exp PARS_THEN_TOKEN statement_list  */
#line 411 "pars0grm.y"
                                { yyval = pars_elsif_element(yyvsp[-2], yyvsp[0]); }
#line 2047 "pars0grm.cc"
    break;

  case 103: /* elsif_list: elsif_element  */
#line 415 "pars0grm.y"
                                { yyval = que_node_list_add_last(NULL, yyvsp[0]); }
#line 2053 "pars0grm.cc"
    break;

  case 104: /* elsif_list: elsif_list elsif_element  */
#line 417 "pars0grm.y"
                                { yyval = que_node_list_add_last(yyvsp[-1], yyvsp[0]); }
#line 2059 "pars0grm.cc"
    break;

  case 105: /* else_part: %empty  */
#line 421 "pars0grm.y"
                                { yyval = NULL; }
#line 2065 "pars0grm.cc"
    break;

  case 106: /* else_part: PARS_ELSE_TOKEN statement_list  */
#line 423 "pars0grm.y"
                                { yyval = yyvsp[0]; }
#line 2071 "pars0grm.cc"
    break;

  case 107: /* else_part: elsif_list  */
#line 424 "pars0grm.y"
                                { yyval = yyvsp[0]; }
#line 2077 "pars0grm.cc"
    break;

  case 108: /* if_statement: PARS_IF_TOKEN exp PARS_THEN_TOKEN statement_list else_part PARS_END_TOKEN PARS_IF_TOKEN  */
#line 431 "pars0grm.y"
                                { yyval = pars_if_statement(yyvsp[-5], yyvsp[-3], yyvsp[-2]); }
#line 2083 "pars0grm.cc"
    break;

  case 109: /* while_statement: PARS_WHILE_TOKEN exp PARS_LOOP_TOKEN statement_list PARS_END_TOKEN PARS_LOOP_TOKEN  */
#line 437 "pars0grm.y"
                                { yyval = pars_while_statement(yyvsp[-4], yyvsp[-2]); }
#line 2089 "pars0grm.cc"
    break;

  case 110: /* for_statement: PARS_FOR_TOKEN PARS_ID_TOKEN PARS_IN_TOKEN exp PARS_DDOT_TOKEN exp PARS_LOOP_TOKEN statement_list PARS_END_TOKEN PARS_LOOP_TOKEN  */
#line 445 "pars0grm.y"
                                { yyval = pars_for_statement(
					static_cast<sym_node_t*>(yyvsp[-8]),
					yyvsp[-6], yyvsp[-4], yyvsp[-2]); }
#line 2097 "pars0grm.cc"
    break;

  case 111: /* exit_statement: PARS_EXIT_TOKEN  */
#line 451 "pars0grm.y"
                                { yyval = pars_exit_statement(); }
#line 2103 "pars0grm.cc"
    break;

  case 112: /* return_statement: PARS_RETURN_TOKEN  */
#line 455 "pars0grm.y"
                                { yyval = pars_return_statement(); }
#line 2109 "pars0grm.cc"
    break;

  case 113: /* open_cursor_statement: PARS_OPEN_TOKEN PARS_ID_TOKEN  */
#line 460 "pars0grm.y"
                                { yyval = pars_open_statement(
						ROW_SEL_OPEN_CURSOR,
						static_cast<sym_node_t*>(yyvsp[0])); }
#line 2117 "pars0grm.cc"
    break;

  case 114: /* close_cursor_statement: PARS_CLOSE_TOKEN PARS_ID_TOKEN  */
#line 467 "pars0grm.y"
                                { yyval = pars_open_statement(
						ROW_SEL_CLOSE_CURSOR,
						static_cast<sym_node_t*>(yyvsp[0])); }
#line 2125 "pars0grm.cc"
    break;

  case 115: /* fetch_statement: PARS_FETCH_TOKEN PARS_ID_TOKEN PARS_INTO_TOKEN variable_list  */
#line 474 "pars0grm.y"
                                { yyval = pars_fetch_statement(
					static_cast<sym_node_t*>(yyvsp[-2]),
					static_cast<sym_node_t*>(yyvsp[0]), NULL); }
#line 2133 "pars0grm.cc"
    break;

  case 116: /* fetch_statement: PARS_FETCH_TOKEN PARS_ID_TOKEN PARS_INTO_TOKEN user_function_call variable_list  */
#line 478 "pars0grm.y"
                                { yyval = pars_fetch_statement(
					static_cast<sym_node_t*>(yyvsp[-3]),
					static_cast<sym_node_t*>(yyvsp[0]),
					static_cast<sym_node_t*>(yyvsp[-1])); }
#line 2142 "pars0grm.cc"
    break;

  case 117: /* column_def: PARS_ID_TOKEN type_name opt_column_len opt_not_null  */
#line 486 "pars0grm.y"
                                { yyval = pars_column_def(
					static_cast<sym_node_t*>(yyvsp[-3]),
					static_cast<pars_res_word_t*>(yyvsp[-2]),
					static_cast<sym_node_t*>(yyvsp[-1]),
					yyvsp[0]); }
#line 2152 "pars0grm.cc"
    break;

  case 118: /* column_def_list: column_def  */
#line 494 "pars0grm.y"
                                { yyval = que_node_list_add_last(NULL, yyvsp[0]); }
#line 2158 "pars0grm.cc"
    break;

  case 119: /* column_def_list: column_def_list ',' column_def  */
#line 496 "pars0grm.y"
                                { yyval = que_node_list_add_last(yyvsp[-2], yyvsp[0]); }
#line 2164 "pars0grm.cc"
    break;

  case 120: /* opt_column_len: %empty  */
#line 500 "pars0grm.y"
                                { yyval = NULL; }
#line 2170 "pars0grm.cc"
    break;

  case 121: /* opt_column_len: '(' PARS_INT_LIT ')'  */
#line 502 "pars0grm.y"
                                { yyval = yyvsp[-1]; }
#line 2176 "pars0grm.cc"
    break;

  case 122: /* opt_not_null: %empty  */
#line 506 "pars0grm.y"
                                { yyval = NULL; }
#line 2182 "pars0grm.cc"
    break;

  case 123: /* opt_not_null: PARS_NOT_TOKEN PARS_NULL_LIT  */
#line 508 "pars0grm.y"
                                { yyval = &pars_int_token;
					/* pass any non-NULL pointer */ }
#line 2189 "pars0grm.cc"
    break;

  case 124: /* create_table: PARS_CREATE_TOKEN PARS_TABLE_TOKEN table_name '(' column_def_list ')'  */
#line 515 "pars0grm.y"
                                { yyval = pars_create_table(
					static_cast<sym_node_t*>(yyvsp[-3]),
					static_cast<sym_node_t*>(yyvsp[-1])); }
#line 2197 "pars0grm.cc"
    break;

  case 125: /* column_list: PARS_ID_TOKEN  */
#line 521 "pars0grm.y"
                                { yyval = que_node_list_add_last(NULL, yyvsp[0]); }
#line 2203 "pars0grm.cc"
    break;

  case 126: /* column_list: column_list ',' PARS_ID_TOKEN  */
#line 523 "pars0grm.y"
                                { yyval = que_node_list_add_last(yyvsp[-2], yyvsp[0]); }
#line 2209 "pars0grm.cc"
    break;

  case 127: /* unique_def: %empty  */
#line 527 "pars0grm.y"
                                { yyval = NULL; }
#line 2215 "pars0grm.cc"
    break;

  case 128: /* unique_def: PARS_UNIQUE_TOKEN  */
#line 528 "pars0grm.y"
                                { yyval = &pars_unique_token; }
#line 2221 "pars0grm.cc"
    break;

  case 129: /* clustered_def: %empty  */
#line 532 "pars0grm.y"
                                { yyval = NULL; }
#line 2227 "pars0grm.cc"
    break;

  case 130: /* clustered_def: PARS_CLUSTERED_TOKEN  */
#line 533 "pars0grm.y"
                                { yyval = &pars_clustered_token; }
#line 2233 "pars0grm.cc"
    break;

  case 131: /* create_index: PARS_CREATE_TOKEN unique_def clustered_def PARS_INDEX_TOKEN PARS_ID_TOKEN PARS_ON_TOKEN table_name '(' column_list ')'  */
#line 542 "pars0grm.y"
                                { yyval = pars_create_index(
					static_cast<pars_res_word_t*>(yyvsp[-8]),
					static_cast<pars_res_word_t*>(yyvsp[-7]),
					static_cast<sym_node_t*>(yyvsp[-5]),
					static_cast<sym_node_t*>(yyvsp[-3]),
					static_cast<sym_node_t*>(yyvsp[-1])); }
#line 2244 "pars0grm.cc"
    break;

  case 132: /* table_name: PARS_ID_TOKEN  */
#line 551 "pars0grm.y"
                                { yyval = yyvsp[0]; }
#line 2250 "pars0grm.cc"
    break;

  case 133: /* table_name: PARS_TABLE_NAME_TOKEN  */
#line 552 "pars0grm.y"
                                { yyval = yyvsp[0]; }
#line 2256 "pars0grm.cc"
    break;

  case 134: /* commit_statement: PARS_COMMIT_TOKEN PARS_WORK_TOKEN  */
#line 557 "pars0grm.y"
                                { yyval = pars_commit_statement(); }
#line 2262 "pars0grm.cc"
    break;

  case 135: /* rollback_statement: PARS_ROLLBACK_TOKEN PARS_WORK_TOKEN  */
#line 562 "pars0grm.y"
                                { yyval = pars_rollback_statement(); }
#line 2268 "pars0grm.cc"
    break;

  case 136: /* type_name: PARS_INT_TOKEN  */
#line 566 "pars0grm.y"
                                { yyval = &pars_int_token; }
#line 2274 "pars0grm.cc"
    break;

  case 137: /* type_name: PARS_BIGINT_TOKEN  */
#line 567 "pars0grm.y"
                                { yyval = &pars_bigint_token; }
#line 2280 "pars0grm.cc"
    break;

  case 138: /* type_name: PARS_CHAR_TOKEN  */
#line 568 "pars0grm.y"
                                { yyval = &pars_char_token; }
#line 2286 "pars0grm.cc"
    break;

  case 139: /* variable_declaration: PARS_ID_TOKEN type_name ';'  */
#line 573 "pars0grm.y"
                                { yyval = pars_variable_declaration(
					static_cast<sym_node_t*>(yyvsp[-2]),
					static_cast<pars_res_word_t*>(yyvsp[-1])); }
#line 2294 "pars0grm.cc"
    break;

  case 143: /* cursor_declaration: PARS_DECLARE_TOKEN PARS_CURSOR_TOKEN PARS_ID_TOKEN PARS_IS_TOKEN select_statement ';'  */
#line 587 "pars0grm.y"
                                { yyval = pars_cursor_declaration(
					static_cast<sym_node_t*>(yyvsp[-3]),
					static_cast<sel_node_t*>(yyvsp[-1])); }
#line 2302 "pars0grm.cc"
    break;

  case 144: /* function_declaration: PARS_DECLARE_TOKEN PARS_FUNCTION_TOKEN PARS_ID_TOKEN ';'  */
#line 594 "pars0grm.y"
                                { yyval = pars_function_declaration(
					static_cast<sym_node_t*>(yyvsp[-1])); }
#line 2309 "pars0grm.cc"
    break;

  case 150: /* procedure_definition: PARS_PROCEDURE_TOKEN PARS_ID_TOKEN '(' ')' PARS_IS_TOKEN variable_declaration_list declaration_list PARS_BEGIN_TOKEN statement_list PARS_END_TOKEN  */
#line 616 "pars0grm.y"
                                { yyval = pars_procedure_definition(
					static_cast<sym_node_t*>(yyvsp[-8]), yyvsp[-1]); }
#line 2316 "pars0grm.cc"
    break;


#line 2320 "pars0grm.cc"

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

#line 620 "pars0grm.y"

