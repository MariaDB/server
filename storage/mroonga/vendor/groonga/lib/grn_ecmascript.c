/* Driver template for the LEMON parser generator.
** The author disclaims copyright to this source code.
*/
/* First off, code is included that follows the "include" declaration
** in the input grammar file. */
#include <stdio.h>
#line 4 "grn_ecmascript.lemon"

#define assert GRN_ASSERT
#line 11 "grn_ecmascript.c"
/* Next is all token values, in a form suitable for use by makeheaders.
** This section will be null unless lemon is run with the -m switch.
*/
/* 
** These constants (all generated automatically by the parser generator)
** specify the various kinds of tokens (terminals) that the parser
** understands. 
**
** Each symbol here is a terminal symbol in the grammar.
*/
/* Make sure the INTERFACE macro is defined.
*/
#ifndef INTERFACE
# define INTERFACE 1
#endif
/* The next thing included is series of defines which control
** various aspects of the generated parser.
**    YYCODETYPE         is the data type used for storing terminal
**                       and nonterminal numbers.  "unsigned char" is
**                       used if there are fewer than 250 terminals
**                       and nonterminals.  "int" is used otherwise.
**    YYNOCODE           is a number of type YYCODETYPE which corresponds
**                       to no legal terminal or nonterminal number.  This
**                       number is used to fill in empty slots of the hash 
**                       table.
**    YYFALLBACK         If defined, this indicates that one or more tokens
**                       have fall-back values which should be used if the
**                       original value of the token will not parse.
**    YYACTIONTYPE       is the data type used for storing terminal
**                       and nonterminal numbers.  "unsigned char" is
**                       used if there are fewer than 250 rules and
**                       states combined.  "int" is used otherwise.
**    grn_expr_parserTOKENTYPE     is the data type used for minor tokens given 
**                       directly to the parser from the tokenizer.
**    YYMINORTYPE        is the data type used for all minor tokens.
**                       This is typically a union of many types, one of
**                       which is grn_expr_parserTOKENTYPE.  The entry in the union
**                       for base tokens is called "yy0".
**    YYSTACKDEPTH       is the maximum depth of the parser's stack.  If
**                       zero the stack is dynamically sized using realloc()
**    grn_expr_parserARG_SDECL     A static variable declaration for the %extra_argument
**    grn_expr_parserARG_PDECL     A parameter declaration for the %extra_argument
**    grn_expr_parserARG_STORE     Code to store %extra_argument into yypParser
**    grn_expr_parserARG_FETCH     Code to extract %extra_argument from yypParser
**    YYNSTATE           the combined number of states.
**    YYNRULE            the number of rules in the grammar
**    YYERRORSYMBOL      is the code number of the error symbol.  If not
**                       defined, then do no error processing.
*/
#define YYCODETYPE unsigned char
#define YYNOCODE 113
#define YYACTIONTYPE unsigned short int
#define grn_expr_parserTOKENTYPE  int 
typedef union {
  int yyinit;
  grn_expr_parserTOKENTYPE yy0;
  void * yy81;
} YYMINORTYPE;
#ifndef YYSTACKDEPTH
#define YYSTACKDEPTH 100
#endif
#define grn_expr_parserARG_SDECL  efs_info *efsi ;
#define grn_expr_parserARG_PDECL , efs_info *efsi 
#define grn_expr_parserARG_FETCH  efs_info *efsi  = yypParser->efsi 
#define grn_expr_parserARG_STORE yypParser->efsi  = efsi 
#define YYNSTATE 223
#define YYNRULE 131
#define YY_NO_ACTION      (YYNSTATE+YYNRULE+2)
#define YY_ACCEPT_ACTION  (YYNSTATE+YYNRULE+1)
#define YY_ERROR_ACTION   (YYNSTATE+YYNRULE)

/* The yyzerominor constant is used to initialize instances of
** YYMINORTYPE objects to zero. */
static const YYMINORTYPE yyzerominor = { 0 };

/* Define the yytestcase() macro to be a no-op if is not already defined
** otherwise.
**
** Applications can choose to define yytestcase() in the %include section
** to a macro that can assist in verifying code coverage.  For production
** code the yytestcase() macro should be turned off.  But it is useful
** for testing.
*/
#ifndef yytestcase
# define yytestcase(X)
#endif


/* Next are the tables used to determine what action to take based on the
** current state and lookahead token.  These tables are used to implement
** functions that take a state number and lookahead value and return an
** action integer.  
**
** Suppose the action integer is N.  Then the action is determined as
** follows
**
**   0 <= N < YYNSTATE                  Shift N.  That is, push the lookahead
**                                      token onto the stack and goto state N.
**
**   YYNSTATE <= N < YYNSTATE+YYNRULE   Reduce by rule N-YYNSTATE.
**
**   N == YYNSTATE+YYNRULE              A syntax error has occurred.
**
**   N == YYNSTATE+YYNRULE+1            The parser accepts its input.
**
**   N == YYNSTATE+YYNRULE+2            No such action.  Denotes unused
**                                      slots in the yy_action[] table.
**
** The action table is constructed as a single large table named yy_action[].
** Given state S and lookahead X, the action is computed as
**
**      yy_action[ yy_shift_ofst[S] + X ]
**
** If the index value yy_shift_ofst[S]+X is out of range or if the value
** yy_lookahead[yy_shift_ofst[S]+X] is not equal to X or if yy_shift_ofst[S]
** is equal to YY_SHIFT_USE_DFLT, it means that the action is not in the table
** and that yy_default[S] should be used instead.  
**
** The formula above is for computing the action when the lookahead is
** a terminal symbol.  If the lookahead is a non-terminal (as occurs after
** a reduce action) then the yy_reduce_ofst[] array is used in place of
** the yy_shift_ofst[] array and YY_REDUCE_USE_DFLT is used in place of
** YY_SHIFT_USE_DFLT.
**
** The following are the tables generated in this section:
**
**  yy_action[]        A single table containing all actions.
**  yy_lookahead[]     A table containing the lookahead for each entry in
**                     yy_action.  Used to detect hash collisions.
**  yy_shift_ofst[]    For each state, the offset into yy_action for
**                     shifting terminals.
**  yy_reduce_ofst[]   For each state, the offset into yy_action for
**                     shifting non-terminals after a reduce.
**  yy_default[]       Default action for each state.
*/
#define YY_ACTTAB_COUNT (1610)
static const YYACTIONTYPE yy_action[] = {
 /*     0 */     2,   70,   52,   51,   50,  220,    1,   75,   79,  123,
 /*    10 */     4,  219,   69,  355,   76,  107,   78,  150,  219,  189,
 /*    20 */   192,  213,   84,  121,  120,  133,  132,  131,  115,   85,
 /*    30 */    98,  111,   99,  178,  209,  195,   73,  188,  184,  188,
 /*    40 */   184,  220,   71,   25,   79,  138,    9,   32,   69,   64,
 /*    50 */    63,  215,   33,   28,   67,   66,   65,   62,   61,   60,
 /*    60 */    59,   58,   57,  183,  182,  181,  180,  179,    3,   75,
 /*    70 */   113,   34,    6,  219,  189,  192,  213,   84,  121,  120,
 /*    80 */   133,  132,  131,  115,   85,   98,  111,   99,  178,  209,
 /*    90 */   195,   73,  187,  105,  188,  184,  220,    1,   28,   79,
 /*   100 */   123,    4,  122,   69,   31,   30,  189,  192,  213,   84,
 /*   110 */   121,  120,  133,  132,  131,  115,   85,   98,  111,   99,
 /*   120 */   178,  209,  195,   73,  139,  127,  188,  184,   36,   35,
 /*   130 */   110,   68,   56,   55,    8,   24,  129,  197,  196,   29,
 /*   140 */    64,   63,   54,   53,  124,   67,   66,   65,   62,   61,
 /*   150 */    60,   59,   58,   57,  183,  182,  181,  180,  179,    3,
 /*   160 */     7,   26,  165,  185,   83,  142,  127,  176,  189,  166,
 /*   170 */   213,   84,  121,  120,  133,  132,  131,  115,   85,   98,
 /*   180 */   111,   99,  178,  209,  195,   73,  173,  130,  188,  184,
 /*   190 */    11,   82,   81,   80,   77,  220,   71,  148,   79,  138,
 /*   200 */     9,  171,   69,   64,   63,  174,   28,   72,   67,   66,
 /*   210 */    65,   62,   61,   60,   59,   58,   57,  183,  182,  181,
 /*   220 */   180,  179,    3,  177,    7,  194,  193,  185,   83,  106,
 /*   230 */   126,  176,  189,  144,  213,   84,  121,  120,  133,  132,
 /*   240 */   131,  115,   85,   98,  111,   99,  178,  209,  195,   73,
 /*   250 */   164,  224,  188,  184,  141,  171,   23,  171,   10,  110,
 /*   260 */   143,  226,  191,  140,  221,   28,  218,   64,   63,  125,
 /*   270 */   356,  356,   67,   66,   65,   62,   61,   60,   59,   58,
 /*   280 */    57,  183,  182,  181,  180,  179,    3,  170,    7,  122,
 /*   290 */   217,  185,   83,  189,  192,  213,   84,  121,  120,  133,
 /*   300 */   132,  131,  115,   85,   98,  111,   99,  178,  209,  195,
 /*   310 */    73,   74,  216,  188,  184,  225,   49,   48,   47,   46,
 /*   320 */    45,   44,   43,   42,   41,   40,   39,   38,   37,    5,
 /*   330 */   149,   64,   63,  146,  222,  356,   67,   66,   65,   62,
 /*   340 */    61,   60,   59,   58,   57,  183,  182,  181,  180,  179,
 /*   350 */     3,  116,  356,  145,  356,  189,  192,  213,   84,  121,
 /*   360 */   120,  133,  132,  131,  115,   85,   98,  111,   99,  178,
 /*   370 */   209,  195,   73,  113,  356,  188,  184,  189,  192,  213,
 /*   380 */    84,  121,  120,  133,  132,  131,  115,   85,   98,  111,
 /*   390 */    99,  178,  209,  195,   73,  356,  356,  188,  184,  223,
 /*   400 */   356,  356,   81,   80,   77,  220,   71,  356,   79,  138,
 /*   410 */     9,  356,   69,  189,  162,  213,   84,  121,  120,  133,
 /*   420 */   132,  131,  115,   85,   98,  111,   99,  178,  209,  195,
 /*   430 */    73,  356,    7,  188,  184,  185,   83,  356,  356,  167,
 /*   440 */   109,  189,  144,  213,   84,  121,  120,  133,  132,  131,
 /*   450 */   115,   85,   98,  111,   99,  178,  209,  195,   73,  356,
 /*   460 */     7,  188,  184,  185,   83,  356,  356,  356,  356,  147,
 /*   470 */   356,  356,  356,  356,  356,   64,   63,  356,  356,  356,
 /*   480 */    67,   66,   65,   62,   61,   60,   59,   58,   57,  183,
 /*   490 */   182,  181,  180,  179,    3,  356,  356,  356,  356,  356,
 /*   500 */   356,  356,  356,   64,   63,  356,  356,  167,   67,   66,
 /*   510 */    65,   62,   61,   60,   59,   58,   57,  183,  182,  181,
 /*   520 */   180,  179,    3,  189,  214,  213,   84,  121,  120,  133,
 /*   530 */   132,  131,  115,   85,   98,  111,   99,  178,  209,  195,
 /*   540 */    73,  356,  356,  188,  184,  189,  212,  213,   84,  121,
 /*   550 */   120,  133,  132,  131,  115,   85,   98,  111,   99,  178,
 /*   560 */   209,  195,   73,  168,  356,  188,  184,  189,  137,  213,
 /*   570 */    84,  121,  120,  133,  132,  131,  115,   85,   98,  111,
 /*   580 */    99,  178,  209,  195,   73,  356,  356,  188,  184,  356,
 /*   590 */   189,  211,  213,   84,  121,  120,  133,  132,  131,  115,
 /*   600 */    85,   98,  111,   99,  178,  209,  195,   73,  356,  356,
 /*   610 */   188,  184,  189,  172,  213,   84,  121,  120,  133,  132,
 /*   620 */   131,  115,   85,   98,  111,   99,  178,  209,  195,   73,
 /*   630 */   356,  356,  188,  184,  189,  163,  213,   84,  121,  120,
 /*   640 */   133,  132,  131,  115,   85,   98,  111,   99,  178,  209,
 /*   650 */   195,   73,  356,  356,  188,  184,  189,  161,  213,   84,
 /*   660 */   121,  120,  133,  132,  131,  115,   85,   98,  111,   99,
 /*   670 */   178,  209,  195,   73,  356,  356,  188,  184,  189,  160,
 /*   680 */   213,   84,  121,  120,  133,  132,  131,  115,   85,   98,
 /*   690 */   111,   99,  178,  209,  195,   73,  356,  356,  188,  184,
 /*   700 */   189,  159,  213,   84,  121,  120,  133,  132,  131,  115,
 /*   710 */    85,   98,  111,   99,  178,  209,  195,   73,  356,  356,
 /*   720 */   188,  184,  189,  158,  213,   84,  121,  120,  133,  132,
 /*   730 */   131,  115,   85,   98,  111,   99,  178,  209,  195,   73,
 /*   740 */   356,  356,  188,  184,  189,  157,  213,   84,  121,  120,
 /*   750 */   133,  132,  131,  115,   85,   98,  111,   99,  178,  209,
 /*   760 */   195,   73,  356,  356,  188,  184,  189,  156,  213,   84,
 /*   770 */   121,  120,  133,  132,  131,  115,   85,   98,  111,   99,
 /*   780 */   178,  209,  195,   73,  356,  356,  188,  184,  189,  155,
 /*   790 */   213,   84,  121,  120,  133,  132,  131,  115,   85,   98,
 /*   800 */   111,   99,  178,  209,  195,   73,  356,  356,  188,  184,
 /*   810 */   189,  154,  213,   84,  121,  120,  133,  132,  131,  115,
 /*   820 */    85,   98,  111,   99,  178,  209,  195,   73,  356,  356,
 /*   830 */   188,  184,  189,  153,  213,   84,  121,  120,  133,  132,
 /*   840 */   131,  115,   85,   98,  111,   99,  178,  209,  195,   73,
 /*   850 */   356,  356,  188,  184,  189,  152,  213,   84,  121,  120,
 /*   860 */   133,  132,  131,  115,   85,   98,  111,   99,  178,  209,
 /*   870 */   195,   73,  356,  356,  188,  184,  189,  151,  213,   84,
 /*   880 */   121,  120,  133,  132,  131,  115,   85,   98,  111,   99,
 /*   890 */   178,  209,  195,   73,  356,  356,  188,  184,  189,  175,
 /*   900 */   213,   84,  121,  120,  133,  132,  131,  115,   85,   98,
 /*   910 */   111,   99,  178,  209,  195,   73,  356,  356,  188,  184,
 /*   920 */   189,  169,  213,   84,  121,  120,  133,  132,  131,  115,
 /*   930 */    85,   98,  111,   99,  178,  209,  195,   73,  356,  189,
 /*   940 */   188,  184,  117,  356,  108,  133,  132,  131,  115,   85,
 /*   950 */    98,  111,   99,  178,  209,  195,   73,  356,  189,  188,
 /*   960 */   184,  117,  356,  356,  136,  132,  131,  115,   85,   98,
 /*   970 */   111,   99,  178,  209,  195,   73,  356,  356,  188,  184,
 /*   980 */   189,  356,  356,  117,  356,  356,  128,  132,  131,  115,
 /*   990 */    85,   98,  111,   99,  178,  209,  195,   73,  356,  356,
 /*  1000 */   188,  184,  189,  356,  356,  117,  356,  356,  356,  135,
 /*  1010 */   131,  115,   85,   98,  111,   99,  178,  209,  195,   73,
 /*  1020 */   356,  356,  188,  184,  356,   27,   22,   21,   20,   19,
 /*  1030 */    18,   17,   16,   15,   14,   13,   12,  189,  356,  356,
 /*  1040 */   117,  356,  356,  356,  356,  134,  115,   85,   98,  111,
 /*  1050 */    99,  178,  209,  195,   73,  356,  356,  188,  184,  356,
 /*  1060 */   189,  356,  356,  117,  356,  356,  197,  196,  356,  119,
 /*  1070 */    85,   98,  111,   99,  178,  209,  195,   73,  356,  189,
 /*  1080 */   188,  184,  117,    7,  356,  356,  185,   83,  356,   87,
 /*  1090 */    98,  111,   99,  178,  209,  195,   73,  356,  189,  188,
 /*  1100 */   184,  117,  356,  356,  356,  356,  356,  356,   86,   98,
 /*  1110 */   111,   99,  178,  209,  195,   73,  356,  189,  188,  184,
 /*  1120 */   117,  356,  356,  356,  356,  356,  356,  356,  104,  111,
 /*  1130 */    99,  178,  209,  195,   73,  356,  189,  188,  184,  117,
 /*  1140 */   183,  182,  181,  180,  179,    3,  356,  102,  111,   99,
 /*  1150 */   178,  209,  195,   73,  356,  189,  188,  184,  117,  356,
 /*  1160 */   356,  356,  356,  356,  356,  356,  100,  111,   99,  178,
 /*  1170 */   209,  195,   73,  356,  189,  188,  184,  117,  356,  356,
 /*  1180 */   356,  356,  356,  356,  356,   97,  111,   99,  178,  209,
 /*  1190 */   195,   73,  356,  189,  188,  184,  117,  356,  356,  356,
 /*  1200 */   356,  356,  356,  356,   96,  111,   99,  178,  209,  195,
 /*  1210 */    73,  356,  189,  188,  184,  117,  356,  356,  356,  356,
 /*  1220 */   356,  356,  356,   95,  111,   99,  178,  209,  195,   73,
 /*  1230 */   356,  189,  188,  184,  117,  356,  356,  356,  356,  356,
 /*  1240 */   356,  356,   94,  111,   99,  178,  209,  195,   73,  356,
 /*  1250 */   189,  188,  184,  117,  356,  356,  356,  356,  356,  356,
 /*  1260 */   356,   93,  111,   99,  178,  209,  195,   73,  356,  189,
 /*  1270 */   188,  184,  117,  356,  356,  356,  356,  356,  356,  356,
 /*  1280 */    92,  111,   99,  178,  209,  195,   73,  356,  189,  188,
 /*  1290 */   184,  117,  356,  356,  356,  356,  356,  356,  356,   91,
 /*  1300 */   111,   99,  178,  209,  195,   73,  356,  189,  188,  184,
 /*  1310 */   117,  356,  356,  356,  356,  356,  356,  356,   90,  111,
 /*  1320 */    99,  178,  209,  195,   73,  356,  189,  188,  184,  117,
 /*  1330 */   356,  356,  356,  356,  356,  356,  356,   89,  111,   99,
 /*  1340 */   178,  209,  195,   73,  356,  189,  188,  184,  117,  356,
 /*  1350 */   356,  356,  356,  356,  356,  356,   88,  111,   99,  178,
 /*  1360 */   209,  195,   73,  356,  189,  188,  184,  117,  356,  356,
 /*  1370 */   356,  356,  356,  356,  356,  356,  118,   99,  178,  209,
 /*  1380 */   195,   73,  356,  189,  188,  184,  117,  356,  356,  356,
 /*  1390 */   356,  356,  356,  356,  356,  114,   99,  178,  209,  195,
 /*  1400 */    73,  356,  189,  188,  184,  117,  356,  356,  356,  356,
 /*  1410 */   356,  356,  356,  356,  112,   99,  178,  209,  195,   73,
 /*  1420 */   356,  189,  188,  184,  117,  356,  356,  356,  356,  356,
 /*  1430 */   189,  356,  356,  117,  103,  178,  209,  195,   73,  356,
 /*  1440 */   356,  188,  184,  101,  178,  209,  195,   73,  356,  189,
 /*  1450 */   188,  184,  117,  356,  356,  356,  356,  356,  189,  356,
 /*  1460 */   356,  117,  356,  210,  209,  195,   73,  356,  189,  188,
 /*  1470 */   184,  117,  208,  209,  195,   73,  356,  189,  188,  184,
 /*  1480 */   117,  356,  207,  209,  195,   73,  356,  189,  188,  184,
 /*  1490 */   117,  206,  209,  195,   73,  356,  189,  188,  184,  117,
 /*  1500 */   356,  205,  209,  195,   73,  356,  189,  188,  184,  117,
 /*  1510 */   204,  209,  195,   73,  356,  189,  188,  184,  117,  356,
 /*  1520 */   203,  209,  195,   73,  356,  189,  188,  184,  117,  202,
 /*  1530 */   209,  195,   73,  356,  189,  188,  184,  117,  356,  201,
 /*  1540 */   209,  195,   73,  356,  356,  188,  184,  356,  200,  209,
 /*  1550 */   195,   73,  356,  189,  188,  184,  117,  356,  356,  356,
 /*  1560 */   356,  189,  356,  356,  117,  356,  356,  199,  209,  195,
 /*  1570 */    73,  356,  356,  188,  184,  198,  209,  195,   73,  356,
 /*  1580 */   189,  188,  184,  117,  356,  356,  356,  356,  189,  356,
 /*  1590 */   356,  117,  356,  356,  190,  209,  195,   73,  356,  356,
 /*  1600 */   188,  184,  186,  209,  195,   73,  356,  356,  188,  184,
};
static const YYCODETYPE yy_lookahead[] = {
 /*     0 */     1,    2,   47,   48,   49,    6,    7,   76,    9,   10,
 /*    10 */    11,   80,   13,   75,   76,   77,    9,   81,   80,   81,
 /*    20 */    82,   83,   84,   85,   86,   87,   88,   89,   90,   91,
 /*    30 */    92,   93,   94,   95,   96,   97,   98,  101,  102,  101,
 /*    40 */   102,    6,    7,   28,    9,   10,   11,   29,   13,   50,
 /*    50 */    51,   12,   30,   14,   55,   56,   57,   58,   59,   60,
 /*    60 */    61,   62,   63,   64,   65,   66,   67,   68,   69,   76,
 /*    70 */    77,   31,    7,   80,   81,   82,   83,   84,   85,   86,
 /*    80 */    87,   88,   89,   90,   91,   92,   93,   94,   95,   96,
 /*    90 */    97,   98,    8,   79,  101,  102,    6,    7,   14,    9,
 /*   100 */    10,   11,   77,   13,    3,    4,   81,   82,   83,   84,
 /*   110 */    85,   86,   87,   88,   89,   90,   91,   92,   93,   94,
 /*   120 */    95,   96,   97,   98,  110,  111,  101,  102,   32,   33,
 /*   130 */   105,   52,   53,   54,   69,   28,   71,   56,   57,    5,
 /*   140 */    50,   51,   50,   51,   39,   55,   56,   57,   58,   59,
 /*   150 */    60,   61,   62,   63,   64,   65,   66,   67,   68,   69,
 /*   160 */     7,   27,   10,   10,   11,  110,  111,   14,   81,   82,
 /*   170 */    83,   84,   85,   86,   87,   88,   89,   90,   91,   92,
 /*   180 */    93,   94,   95,   96,   97,   98,  106,  107,  101,  102,
 /*   190 */   103,  104,    3,    4,    5,    6,    7,    8,    9,   10,
 /*   200 */    11,   10,   13,   50,   51,   12,   14,   14,   55,   56,
 /*   210 */    57,   58,   59,   60,   61,   62,   63,   64,   65,   66,
 /*   220 */    67,   68,   69,   70,    7,   99,  100,   10,   11,   78,
 /*   230 */    52,   14,   81,   82,   83,   84,   85,   86,   87,   88,
 /*   240 */    89,   90,   91,   92,   93,   94,   95,   96,   97,   98,
 /*   250 */     8,    0,  101,  102,   64,   64,   14,   66,  103,  105,
 /*   260 */   109,    0,   70,   66,   80,   14,   80,   50,   51,   10,
 /*   270 */   112,  112,   55,   56,   57,   58,   59,   60,   61,   62,
 /*   280 */    63,   64,   65,   66,   67,   68,   69,   70,    7,   77,
 /*   290 */    80,   10,   11,   81,   82,   83,   84,   85,   86,   87,
 /*   300 */    88,   89,   90,   91,   92,   93,   94,   95,   96,   97,
 /*   310 */    98,   50,   80,  101,  102,    0,   34,   35,   36,   37,
 /*   320 */    38,   39,   40,   41,   42,   43,   44,   45,   46,   14,
 /*   330 */    80,   50,   51,   52,   80,  112,   55,   56,   57,   58,
 /*   340 */    59,   60,   61,   62,   63,   64,   65,   66,   67,   68,
 /*   350 */    69,   77,  112,   72,  112,   81,   82,   83,   84,   85,
 /*   360 */    86,   87,   88,   89,   90,   91,   92,   93,   94,   95,
 /*   370 */    96,   97,   98,   77,  112,  101,  102,   81,   82,   83,
 /*   380 */    84,   85,   86,   87,   88,   89,   90,   91,   92,   93,
 /*   390 */    94,   95,   96,   97,   98,  112,  112,  101,  102,    0,
 /*   400 */   112,  112,    3,    4,    5,    6,    7,  112,    9,   10,
 /*   410 */    11,  112,   13,   81,   82,   83,   84,   85,   86,   87,
 /*   420 */    88,   89,   90,   91,   92,   93,   94,   95,   96,   97,
 /*   430 */    98,  112,    7,  101,  102,   10,   11,  112,  112,   14,
 /*   440 */   108,   81,   82,   83,   84,   85,   86,   87,   88,   89,
 /*   450 */    90,   91,   92,   93,   94,   95,   96,   97,   98,  112,
 /*   460 */     7,  101,  102,   10,   11,  112,  112,  112,  112,  109,
 /*   470 */   112,  112,  112,  112,  112,   50,   51,  112,  112,  112,
 /*   480 */    55,   56,   57,   58,   59,   60,   61,   62,   63,   64,
 /*   490 */    65,   66,   67,   68,   69,  112,  112,  112,  112,  112,
 /*   500 */   112,  112,  112,   50,   51,  112,  112,   14,   55,   56,
 /*   510 */    57,   58,   59,   60,   61,   62,   63,   64,   65,   66,
 /*   520 */    67,   68,   69,   81,   82,   83,   84,   85,   86,   87,
 /*   530 */    88,   89,   90,   91,   92,   93,   94,   95,   96,   97,
 /*   540 */    98,  112,  112,  101,  102,   81,   82,   83,   84,   85,
 /*   550 */    86,   87,   88,   89,   90,   91,   92,   93,   94,   95,
 /*   560 */    96,   97,   98,   70,  112,  101,  102,   81,   82,   83,
 /*   570 */    84,   85,   86,   87,   88,   89,   90,   91,   92,   93,
 /*   580 */    94,   95,   96,   97,   98,  112,  112,  101,  102,  112,
 /*   590 */    81,   82,   83,   84,   85,   86,   87,   88,   89,   90,
 /*   600 */    91,   92,   93,   94,   95,   96,   97,   98,  112,  112,
 /*   610 */   101,  102,   81,   82,   83,   84,   85,   86,   87,   88,
 /*   620 */    89,   90,   91,   92,   93,   94,   95,   96,   97,   98,
 /*   630 */   112,  112,  101,  102,   81,   82,   83,   84,   85,   86,
 /*   640 */    87,   88,   89,   90,   91,   92,   93,   94,   95,   96,
 /*   650 */    97,   98,  112,  112,  101,  102,   81,   82,   83,   84,
 /*   660 */    85,   86,   87,   88,   89,   90,   91,   92,   93,   94,
 /*   670 */    95,   96,   97,   98,  112,  112,  101,  102,   81,   82,
 /*   680 */    83,   84,   85,   86,   87,   88,   89,   90,   91,   92,
 /*   690 */    93,   94,   95,   96,   97,   98,  112,  112,  101,  102,
 /*   700 */    81,   82,   83,   84,   85,   86,   87,   88,   89,   90,
 /*   710 */    91,   92,   93,   94,   95,   96,   97,   98,  112,  112,
 /*   720 */   101,  102,   81,   82,   83,   84,   85,   86,   87,   88,
 /*   730 */    89,   90,   91,   92,   93,   94,   95,   96,   97,   98,
 /*   740 */   112,  112,  101,  102,   81,   82,   83,   84,   85,   86,
 /*   750 */    87,   88,   89,   90,   91,   92,   93,   94,   95,   96,
 /*   760 */    97,   98,  112,  112,  101,  102,   81,   82,   83,   84,
 /*   770 */    85,   86,   87,   88,   89,   90,   91,   92,   93,   94,
 /*   780 */    95,   96,   97,   98,  112,  112,  101,  102,   81,   82,
 /*   790 */    83,   84,   85,   86,   87,   88,   89,   90,   91,   92,
 /*   800 */    93,   94,   95,   96,   97,   98,  112,  112,  101,  102,
 /*   810 */    81,   82,   83,   84,   85,   86,   87,   88,   89,   90,
 /*   820 */    91,   92,   93,   94,   95,   96,   97,   98,  112,  112,
 /*   830 */   101,  102,   81,   82,   83,   84,   85,   86,   87,   88,
 /*   840 */    89,   90,   91,   92,   93,   94,   95,   96,   97,   98,
 /*   850 */   112,  112,  101,  102,   81,   82,   83,   84,   85,   86,
 /*   860 */    87,   88,   89,   90,   91,   92,   93,   94,   95,   96,
 /*   870 */    97,   98,  112,  112,  101,  102,   81,   82,   83,   84,
 /*   880 */    85,   86,   87,   88,   89,   90,   91,   92,   93,   94,
 /*   890 */    95,   96,   97,   98,  112,  112,  101,  102,   81,   82,
 /*   900 */    83,   84,   85,   86,   87,   88,   89,   90,   91,   92,
 /*   910 */    93,   94,   95,   96,   97,   98,  112,  112,  101,  102,
 /*   920 */    81,   82,   83,   84,   85,   86,   87,   88,   89,   90,
 /*   930 */    91,   92,   93,   94,   95,   96,   97,   98,  112,   81,
 /*   940 */   101,  102,   84,  112,   86,   87,   88,   89,   90,   91,
 /*   950 */    92,   93,   94,   95,   96,   97,   98,  112,   81,  101,
 /*   960 */   102,   84,  112,  112,   87,   88,   89,   90,   91,   92,
 /*   970 */    93,   94,   95,   96,   97,   98,  112,  112,  101,  102,
 /*   980 */    81,  112,  112,   84,  112,  112,   87,   88,   89,   90,
 /*   990 */    91,   92,   93,   94,   95,   96,   97,   98,  112,  112,
 /*  1000 */   101,  102,   81,  112,  112,   84,  112,  112,  112,   88,
 /*  1010 */    89,   90,   91,   92,   93,   94,   95,   96,   97,   98,
 /*  1020 */   112,  112,  101,  102,  112,   15,   16,   17,   18,   19,
 /*  1030 */    20,   21,   22,   23,   24,   25,   26,   81,  112,  112,
 /*  1040 */    84,  112,  112,  112,  112,   89,   90,   91,   92,   93,
 /*  1050 */    94,   95,   96,   97,   98,  112,  112,  101,  102,  112,
 /*  1060 */    81,  112,  112,   84,  112,  112,   56,   57,  112,   90,
 /*  1070 */    91,   92,   93,   94,   95,   96,   97,   98,  112,   81,
 /*  1080 */   101,  102,   84,    7,  112,  112,   10,   11,  112,   91,
 /*  1090 */    92,   93,   94,   95,   96,   97,   98,  112,   81,  101,
 /*  1100 */   102,   84,  112,  112,  112,  112,  112,  112,   91,   92,
 /*  1110 */    93,   94,   95,   96,   97,   98,  112,   81,  101,  102,
 /*  1120 */    84,  112,  112,  112,  112,  112,  112,  112,   92,   93,
 /*  1130 */    94,   95,   96,   97,   98,  112,   81,  101,  102,   84,
 /*  1140 */    64,   65,   66,   67,   68,   69,  112,   92,   93,   94,
 /*  1150 */    95,   96,   97,   98,  112,   81,  101,  102,   84,  112,
 /*  1160 */   112,  112,  112,  112,  112,  112,   92,   93,   94,   95,
 /*  1170 */    96,   97,   98,  112,   81,  101,  102,   84,  112,  112,
 /*  1180 */   112,  112,  112,  112,  112,   92,   93,   94,   95,   96,
 /*  1190 */    97,   98,  112,   81,  101,  102,   84,  112,  112,  112,
 /*  1200 */   112,  112,  112,  112,   92,   93,   94,   95,   96,   97,
 /*  1210 */    98,  112,   81,  101,  102,   84,  112,  112,  112,  112,
 /*  1220 */   112,  112,  112,   92,   93,   94,   95,   96,   97,   98,
 /*  1230 */   112,   81,  101,  102,   84,  112,  112,  112,  112,  112,
 /*  1240 */   112,  112,   92,   93,   94,   95,   96,   97,   98,  112,
 /*  1250 */    81,  101,  102,   84,  112,  112,  112,  112,  112,  112,
 /*  1260 */   112,   92,   93,   94,   95,   96,   97,   98,  112,   81,
 /*  1270 */   101,  102,   84,  112,  112,  112,  112,  112,  112,  112,
 /*  1280 */    92,   93,   94,   95,   96,   97,   98,  112,   81,  101,
 /*  1290 */   102,   84,  112,  112,  112,  112,  112,  112,  112,   92,
 /*  1300 */    93,   94,   95,   96,   97,   98,  112,   81,  101,  102,
 /*  1310 */    84,  112,  112,  112,  112,  112,  112,  112,   92,   93,
 /*  1320 */    94,   95,   96,   97,   98,  112,   81,  101,  102,   84,
 /*  1330 */   112,  112,  112,  112,  112,  112,  112,   92,   93,   94,
 /*  1340 */    95,   96,   97,   98,  112,   81,  101,  102,   84,  112,
 /*  1350 */   112,  112,  112,  112,  112,  112,   92,   93,   94,   95,
 /*  1360 */    96,   97,   98,  112,   81,  101,  102,   84,  112,  112,
 /*  1370 */   112,  112,  112,  112,  112,  112,   93,   94,   95,   96,
 /*  1380 */    97,   98,  112,   81,  101,  102,   84,  112,  112,  112,
 /*  1390 */   112,  112,  112,  112,  112,   93,   94,   95,   96,   97,
 /*  1400 */    98,  112,   81,  101,  102,   84,  112,  112,  112,  112,
 /*  1410 */   112,  112,  112,  112,   93,   94,   95,   96,   97,   98,
 /*  1420 */   112,   81,  101,  102,   84,  112,  112,  112,  112,  112,
 /*  1430 */    81,  112,  112,   84,   94,   95,   96,   97,   98,  112,
 /*  1440 */   112,  101,  102,   94,   95,   96,   97,   98,  112,   81,
 /*  1450 */   101,  102,   84,  112,  112,  112,  112,  112,   81,  112,
 /*  1460 */   112,   84,  112,   95,   96,   97,   98,  112,   81,  101,
 /*  1470 */   102,   84,   95,   96,   97,   98,  112,   81,  101,  102,
 /*  1480 */    84,  112,   95,   96,   97,   98,  112,   81,  101,  102,
 /*  1490 */    84,   95,   96,   97,   98,  112,   81,  101,  102,   84,
 /*  1500 */   112,   95,   96,   97,   98,  112,   81,  101,  102,   84,
 /*  1510 */    95,   96,   97,   98,  112,   81,  101,  102,   84,  112,
 /*  1520 */    95,   96,   97,   98,  112,   81,  101,  102,   84,   95,
 /*  1530 */    96,   97,   98,  112,   81,  101,  102,   84,  112,   95,
 /*  1540 */    96,   97,   98,  112,  112,  101,  102,  112,   95,   96,
 /*  1550 */    97,   98,  112,   81,  101,  102,   84,  112,  112,  112,
 /*  1560 */   112,   81,  112,  112,   84,  112,  112,   95,   96,   97,
 /*  1570 */    98,  112,  112,  101,  102,   95,   96,   97,   98,  112,
 /*  1580 */    81,  101,  102,   84,  112,  112,  112,  112,   81,  112,
 /*  1590 */   112,   84,  112,  112,   95,   96,   97,   98,  112,  112,
 /*  1600 */   101,  102,   95,   96,   97,   98,  112,  112,  101,  102,
};
#define YY_SHIFT_USE_DFLT (-46)
#define YY_SHIFT_COUNT (138)
#define YY_SHIFT_MIN   (-45)
#define YY_SHIFT_MAX   (1076)
static const short yy_shift_ofst[] = {
 /*     0 */    -1,   90,  281,  425,  453,  281,  453,  453,  453,  453,
 /*    10 */   217,  153,  453,  453,  453,  453,  453,  453,  453,  453,
 /*    20 */   453,  453,  453,  453,  453,  453,  453,  453,  453,  453,
 /*    30 */   453,  453,  453,  453,  453,  453,  453,  453,  453,  453,
 /*    40 */   453,  453,  453,  453,  453,  453,  453,  453,  453,  453,
 /*    50 */   453,  453,  453,  453,  453,  453,  453,  453,  453,  453,
 /*    60 */   453,  453,  453,  453,  453,  453,  453,  453,  453, 1076,
 /*    70 */   259,   35,  191,   65,  259,  189,  399,   35,   35,   35,
 /*    80 */    35,   35,  493,  -46, 1010,  282,  282,  282,  -45,  -45,
 /*    90 */   -45,  -45,  -45,  -45,  -45,  -45,  -45,  -45,  -45,   79,
 /*   100 */   -45,   79,  -45,   79,  -45,  261,  315,  251,  101,  242,
 /*   110 */   193,   92,   92,   84,   92,   96,  192,   81,   92,   96,
 /*   120 */   101,  134,   39,    7,  197,  105,  190,  178,   18,  152,
 /*   130 */   107,   40,   22,   18,   40,   22,   18,   15,    7,
};
#define YY_REDUCE_USE_DFLT (-70)
#define YY_REDUCE_COUNT (83)
#define YY_REDUCE_MIN   (-69)
#define YY_REDUCE_MAX   (1507)
static const short yy_reduce_ofst[] = {
 /*     0 */   -62,   -7,  151,   87,   25,  360,  332,  296,  274,  212,
 /*    10 */   839,  817,  795,  773,  751,  729,  707,  685,  663,  641,
 /*    20 */   619,  597,  575,  553,  531,  509,  486,  464,  442,  858,
 /*    30 */   899,  877,  921,  956,  979, 1017,  998, 1264, 1245, 1226,
 /*    40 */  1207, 1188, 1169, 1150, 1131, 1112, 1093, 1074, 1055, 1036,
 /*    50 */  1321, 1302, 1283, 1349, 1340, 1507, 1499, 1480, 1472, 1453,
 /*    60 */  1444, 1434, 1425, 1415, 1406, 1396, 1387, 1377, 1368,  -64,
 /*    70 */    14,  -69,   80,  126,   55,  254,  254,  250,  232,  210,
 /*    80 */   186,  184,  155,  154,
};
static const YYACTIONTYPE yy_default[] = {
 /*     0 */   354,  354,  342,  354,  332,  354,  339,  354,  354,  354,
 /*    10 */   354,  354,  354,  354,  354,  354,  354,  354,  354,  354,
 /*    20 */   354,  354,  354,  354,  354,  354,  354,  354,  354,  354,
 /*    30 */   354,  354,  354,  354,  354,  354,  354,  354,  354,  354,
 /*    40 */   354,  354,  354,  354,  354,  354,  354,  354,  354,  354,
 /*    50 */   354,  354,  354,  354,  354,  354,  354,  354,  354,  354,
 /*    60 */   354,  354,  354,  354,  354,  354,  354,  354,  354,  354,
 /*    70 */   348,  354,  354,  310,  354,  354,  354,  354,  354,  354,
 /*    80 */   354,  354,  354,  332,  306,  266,  268,  267,  282,  281,
 /*    90 */   280,  279,  278,  277,  276,  275,  274,  273,  269,  287,
 /*   100 */   272,  289,  271,  288,  270,  354,  354,  354,  256,  354,
 /*   110 */   354,  283,  286,  354,  285,  264,  354,  306,  284,  265,
 /*   120 */   255,  253,  354,  316,  354,  354,  354,  351,  259,  354,
 /*   130 */   354,  262,  260,  257,  263,  261,  258,  354,  354,  349,
 /*   140 */   353,  352,  350,  343,  347,  346,  345,  344,  233,  231,
 /*   150 */   237,  252,  251,  250,  249,  248,  247,  246,  245,  244,
 /*   160 */   243,  242,  340,  341,  338,  337,  328,  326,  325,  330,
 /*   170 */   324,  335,  334,  333,  331,  329,  327,  323,  290,  322,
 /*   180 */   321,  320,  319,  318,  317,  316,  293,  315,  314,  312,
 /*   190 */   292,  336,  238,  313,  311,  309,  308,  307,  305,  304,
 /*   200 */   303,  302,  301,  300,  299,  298,  297,  296,  295,  294,
 /*   210 */   291,  254,  241,  240,  239,  236,  235,  234,  230,  227,
 /*   220 */   232,  229,  228,
};

/* The next table maps tokens into fallback tokens.  If a construct
** like the following:
** 
**      %fallback ID X Y Z.
**
** appears in the grammar, then ID becomes a fallback token for X, Y,
** and Z.  Whenever one of the tokens X, Y, or Z is input to the parser
** but it does not parse, the type of the token is changed to ID and
** the parse is retried before an error is thrown.
*/
#ifdef YYFALLBACK
static const YYCODETYPE yyFallback[] = {
};
#endif /* YYFALLBACK */

/* The following structure represents a single element of the
** parser's stack.  Information stored includes:
**
**   +  The state number for the parser at this level of the stack.
**
**   +  The value of the token stored at this level of the stack.
**      (In other words, the "major" token.)
**
**   +  The semantic value stored at this level of the stack.  This is
**      the information used by the action routines in the grammar.
**      It is sometimes called the "minor" token.
*/
struct yyStackEntry {
  YYACTIONTYPE stateno;  /* The state-number */
  YYCODETYPE major;      /* The major token value.  This is the code
                         ** number for the token at this stack level */
  YYMINORTYPE minor;     /* The user-supplied minor token value.  This
                         ** is the value of the token  */
};
typedef struct yyStackEntry yyStackEntry;

/* The state of the parser is completely contained in an instance of
** the following structure */
struct yyParser {
  int yyidx;                    /* Index of top element in stack */
#ifdef YYTRACKMAXSTACKDEPTH
  int yyidxMax;                 /* Maximum value of yyidx */
#endif
  int yyerrcnt;                 /* Shifts left before out of the error */
  grn_expr_parserARG_SDECL                /* A place to hold %extra_argument */
#if YYSTACKDEPTH<=0
  int yystksz;                  /* Current side of the stack */
  yyStackEntry *yystack;        /* The parser's stack */
#else
  yyStackEntry yystack[YYSTACKDEPTH];  /* The parser's stack */
#endif
};
typedef struct yyParser yyParser;

#ifndef NDEBUG
#include <stdio.h>
static FILE *yyTraceFILE = 0;
static char *yyTracePrompt = 0;
#endif /* NDEBUG */

#ifndef NDEBUG
/* 
** Turn parser tracing on by giving a stream to which to write the trace
** and a prompt to preface each trace message.  Tracing is turned off
** by making either argument NULL 
**
** Inputs:
** <ul>
** <li> A FILE* to which trace output should be written.
**      If NULL, then tracing is turned off.
** <li> A prefix string written at the beginning of every
**      line of trace output.  If NULL, then tracing is
**      turned off.
** </ul>
**
** Outputs:
** None.
*/
void grn_expr_parserTrace(FILE *TraceFILE, char *zTracePrompt){
  yyTraceFILE = TraceFILE;
  yyTracePrompt = zTracePrompt;
  if( yyTraceFILE==0 ) yyTracePrompt = 0;
  else if( yyTracePrompt==0 ) yyTraceFILE = 0;
}
#endif /* NDEBUG */

#ifndef NDEBUG
/* For tracing shifts, the names of all terminals and nonterminals
** are required.  The following table supplies these names */
static const char *const yyTokenName[] = { 
  "$",             "START_OUTPUT_COLUMNS",  "START_ADJUSTER",  "LOGICAL_AND", 
  "LOGICAL_AND_NOT",  "LOGICAL_OR",    "QSTRING",       "PARENL",      
  "PARENR",        "RELATIVE_OP",   "IDENTIFIER",    "BRACEL",      
  "BRACER",        "EVAL",          "COMMA",         "ASSIGN",      
  "STAR_ASSIGN",   "SLASH_ASSIGN",  "MOD_ASSIGN",    "PLUS_ASSIGN", 
  "MINUS_ASSIGN",  "SHIFTL_ASSIGN",  "SHIFTR_ASSIGN",  "SHIFTRR_ASSIGN",
  "AND_ASSIGN",    "XOR_ASSIGN",    "OR_ASSIGN",     "QUESTION",    
  "COLON",         "BITWISE_OR",    "BITWISE_XOR",   "BITWISE_AND", 
  "EQUAL",         "NOT_EQUAL",     "LESS",          "GREATER",     
  "LESS_EQUAL",    "GREATER_EQUAL",  "IN",            "MATCH",       
  "NEAR",          "NEAR2",         "SIMILAR",       "TERM_EXTRACT",
  "LCP",           "PREFIX",        "SUFFIX",        "SHIFTL",      
  "SHIFTR",        "SHIFTRR",       "PLUS",          "MINUS",       
  "STAR",          "SLASH",         "MOD",           "DELETE",      
  "INCR",          "DECR",          "NOT",           "BITWISE_NOT", 
  "ADJUST",        "EXACT",         "PARTIAL",       "UNSPLIT",     
  "DECIMAL",       "HEX_INTEGER",   "STRING",        "BOOLEAN",     
  "NULL",          "BRACKETL",      "BRACKETR",      "DOT",         
  "NONEXISTENT_COLUMN",  "error",         "suppress_unused_variable_warning",  "input",       
  "query",         "expression",    "output_columns",  "adjuster",    
  "query_element",  "primary_expression",  "assignment_expression",  "conditional_expression",
  "lefthand_side_expression",  "logical_or_expression",  "logical_and_expression",  "bitwise_or_expression",
  "bitwise_xor_expression",  "bitwise_and_expression",  "equality_expression",  "relational_expression",
  "shift_expression",  "additive_expression",  "multiplicative_expression",  "unary_expression",
  "postfix_expression",  "call_expression",  "member_expression",  "arguments",   
  "member_expression_part",  "object_literal",  "array_literal",  "elision",     
  "element_list",  "property_name_and_value_list",  "property_name_and_value",  "property_name",
  "argument_list",  "output_column",  "adjust_expression",  "adjust_match_expression",
};
#endif /* NDEBUG */

#ifndef NDEBUG
/* For tracing reduce actions, the names of all rules are required.
*/
static const char *const yyRuleName[] = {
 /*   0 */ "input ::= query",
 /*   1 */ "input ::= expression",
 /*   2 */ "input ::= START_OUTPUT_COLUMNS output_columns",
 /*   3 */ "input ::= START_ADJUSTER adjuster",
 /*   4 */ "query ::= query_element",
 /*   5 */ "query ::= query query_element",
 /*   6 */ "query ::= query LOGICAL_AND query_element",
 /*   7 */ "query ::= query LOGICAL_AND_NOT query_element",
 /*   8 */ "query ::= query LOGICAL_OR query_element",
 /*   9 */ "query_element ::= QSTRING",
 /*  10 */ "query_element ::= PARENL query PARENR",
 /*  11 */ "query_element ::= RELATIVE_OP query_element",
 /*  12 */ "query_element ::= IDENTIFIER RELATIVE_OP query_element",
 /*  13 */ "query_element ::= BRACEL expression BRACER",
 /*  14 */ "query_element ::= EVAL primary_expression",
 /*  15 */ "expression ::= assignment_expression",
 /*  16 */ "expression ::= expression COMMA assignment_expression",
 /*  17 */ "assignment_expression ::= conditional_expression",
 /*  18 */ "assignment_expression ::= lefthand_side_expression ASSIGN assignment_expression",
 /*  19 */ "assignment_expression ::= lefthand_side_expression STAR_ASSIGN assignment_expression",
 /*  20 */ "assignment_expression ::= lefthand_side_expression SLASH_ASSIGN assignment_expression",
 /*  21 */ "assignment_expression ::= lefthand_side_expression MOD_ASSIGN assignment_expression",
 /*  22 */ "assignment_expression ::= lefthand_side_expression PLUS_ASSIGN assignment_expression",
 /*  23 */ "assignment_expression ::= lefthand_side_expression MINUS_ASSIGN assignment_expression",
 /*  24 */ "assignment_expression ::= lefthand_side_expression SHIFTL_ASSIGN assignment_expression",
 /*  25 */ "assignment_expression ::= lefthand_side_expression SHIFTR_ASSIGN assignment_expression",
 /*  26 */ "assignment_expression ::= lefthand_side_expression SHIFTRR_ASSIGN assignment_expression",
 /*  27 */ "assignment_expression ::= lefthand_side_expression AND_ASSIGN assignment_expression",
 /*  28 */ "assignment_expression ::= lefthand_side_expression XOR_ASSIGN assignment_expression",
 /*  29 */ "assignment_expression ::= lefthand_side_expression OR_ASSIGN assignment_expression",
 /*  30 */ "conditional_expression ::= logical_or_expression",
 /*  31 */ "conditional_expression ::= logical_or_expression QUESTION assignment_expression COLON assignment_expression",
 /*  32 */ "logical_or_expression ::= logical_and_expression",
 /*  33 */ "logical_or_expression ::= logical_or_expression LOGICAL_OR logical_and_expression",
 /*  34 */ "logical_and_expression ::= bitwise_or_expression",
 /*  35 */ "logical_and_expression ::= logical_and_expression LOGICAL_AND bitwise_or_expression",
 /*  36 */ "logical_and_expression ::= logical_and_expression LOGICAL_AND_NOT bitwise_or_expression",
 /*  37 */ "bitwise_or_expression ::= bitwise_xor_expression",
 /*  38 */ "bitwise_or_expression ::= bitwise_or_expression BITWISE_OR bitwise_xor_expression",
 /*  39 */ "bitwise_xor_expression ::= bitwise_and_expression",
 /*  40 */ "bitwise_xor_expression ::= bitwise_xor_expression BITWISE_XOR bitwise_and_expression",
 /*  41 */ "bitwise_and_expression ::= equality_expression",
 /*  42 */ "bitwise_and_expression ::= bitwise_and_expression BITWISE_AND equality_expression",
 /*  43 */ "equality_expression ::= relational_expression",
 /*  44 */ "equality_expression ::= equality_expression EQUAL relational_expression",
 /*  45 */ "equality_expression ::= equality_expression NOT_EQUAL relational_expression",
 /*  46 */ "relational_expression ::= shift_expression",
 /*  47 */ "relational_expression ::= relational_expression LESS shift_expression",
 /*  48 */ "relational_expression ::= relational_expression GREATER shift_expression",
 /*  49 */ "relational_expression ::= relational_expression LESS_EQUAL shift_expression",
 /*  50 */ "relational_expression ::= relational_expression GREATER_EQUAL shift_expression",
 /*  51 */ "relational_expression ::= relational_expression IN shift_expression",
 /*  52 */ "relational_expression ::= relational_expression MATCH shift_expression",
 /*  53 */ "relational_expression ::= relational_expression NEAR shift_expression",
 /*  54 */ "relational_expression ::= relational_expression NEAR2 shift_expression",
 /*  55 */ "relational_expression ::= relational_expression SIMILAR shift_expression",
 /*  56 */ "relational_expression ::= relational_expression TERM_EXTRACT shift_expression",
 /*  57 */ "relational_expression ::= relational_expression LCP shift_expression",
 /*  58 */ "relational_expression ::= relational_expression PREFIX shift_expression",
 /*  59 */ "relational_expression ::= relational_expression SUFFIX shift_expression",
 /*  60 */ "shift_expression ::= additive_expression",
 /*  61 */ "shift_expression ::= shift_expression SHIFTL additive_expression",
 /*  62 */ "shift_expression ::= shift_expression SHIFTR additive_expression",
 /*  63 */ "shift_expression ::= shift_expression SHIFTRR additive_expression",
 /*  64 */ "additive_expression ::= multiplicative_expression",
 /*  65 */ "additive_expression ::= additive_expression PLUS multiplicative_expression",
 /*  66 */ "additive_expression ::= additive_expression MINUS multiplicative_expression",
 /*  67 */ "multiplicative_expression ::= unary_expression",
 /*  68 */ "multiplicative_expression ::= multiplicative_expression STAR unary_expression",
 /*  69 */ "multiplicative_expression ::= multiplicative_expression SLASH unary_expression",
 /*  70 */ "multiplicative_expression ::= multiplicative_expression MOD unary_expression",
 /*  71 */ "unary_expression ::= postfix_expression",
 /*  72 */ "unary_expression ::= DELETE unary_expression",
 /*  73 */ "unary_expression ::= INCR unary_expression",
 /*  74 */ "unary_expression ::= DECR unary_expression",
 /*  75 */ "unary_expression ::= PLUS unary_expression",
 /*  76 */ "unary_expression ::= MINUS unary_expression",
 /*  77 */ "unary_expression ::= NOT unary_expression",
 /*  78 */ "unary_expression ::= BITWISE_NOT unary_expression",
 /*  79 */ "unary_expression ::= ADJUST unary_expression",
 /*  80 */ "unary_expression ::= EXACT unary_expression",
 /*  81 */ "unary_expression ::= PARTIAL unary_expression",
 /*  82 */ "unary_expression ::= UNSPLIT unary_expression",
 /*  83 */ "postfix_expression ::= lefthand_side_expression",
 /*  84 */ "postfix_expression ::= lefthand_side_expression INCR",
 /*  85 */ "postfix_expression ::= lefthand_side_expression DECR",
 /*  86 */ "lefthand_side_expression ::= call_expression",
 /*  87 */ "lefthand_side_expression ::= member_expression",
 /*  88 */ "call_expression ::= member_expression arguments",
 /*  89 */ "member_expression ::= primary_expression",
 /*  90 */ "member_expression ::= member_expression member_expression_part",
 /*  91 */ "primary_expression ::= object_literal",
 /*  92 */ "primary_expression ::= PARENL expression PARENR",
 /*  93 */ "primary_expression ::= IDENTIFIER",
 /*  94 */ "primary_expression ::= array_literal",
 /*  95 */ "primary_expression ::= DECIMAL",
 /*  96 */ "primary_expression ::= HEX_INTEGER",
 /*  97 */ "primary_expression ::= STRING",
 /*  98 */ "primary_expression ::= BOOLEAN",
 /*  99 */ "primary_expression ::= NULL",
 /* 100 */ "array_literal ::= BRACKETL elision BRACKETR",
 /* 101 */ "array_literal ::= BRACKETL element_list elision BRACKETR",
 /* 102 */ "array_literal ::= BRACKETL element_list BRACKETR",
 /* 103 */ "elision ::= COMMA",
 /* 104 */ "elision ::= elision COMMA",
 /* 105 */ "element_list ::= assignment_expression",
 /* 106 */ "element_list ::= elision assignment_expression",
 /* 107 */ "element_list ::= element_list elision assignment_expression",
 /* 108 */ "object_literal ::= BRACEL property_name_and_value_list BRACER",
 /* 109 */ "property_name_and_value_list ::=",
 /* 110 */ "property_name_and_value_list ::= property_name_and_value_list COMMA property_name_and_value",
 /* 111 */ "property_name_and_value ::= property_name COLON assignment_expression",
 /* 112 */ "property_name ::= IDENTIFIER|STRING|DECIMAL",
 /* 113 */ "member_expression_part ::= BRACKETL expression BRACKETR",
 /* 114 */ "member_expression_part ::= DOT IDENTIFIER",
 /* 115 */ "arguments ::= PARENL argument_list PARENR",
 /* 116 */ "argument_list ::=",
 /* 117 */ "argument_list ::= assignment_expression",
 /* 118 */ "argument_list ::= argument_list COMMA assignment_expression",
 /* 119 */ "output_columns ::=",
 /* 120 */ "output_columns ::= output_column",
 /* 121 */ "output_columns ::= output_columns COMMA output_column",
 /* 122 */ "output_column ::= STAR",
 /* 123 */ "output_column ::= NONEXISTENT_COLUMN",
 /* 124 */ "output_column ::= assignment_expression",
 /* 125 */ "adjuster ::=",
 /* 126 */ "adjuster ::= adjust_expression",
 /* 127 */ "adjuster ::= adjuster PLUS adjust_expression",
 /* 128 */ "adjust_expression ::= adjust_match_expression",
 /* 129 */ "adjust_expression ::= adjust_match_expression STAR DECIMAL",
 /* 130 */ "adjust_match_expression ::= IDENTIFIER MATCH STRING",
};
#endif /* NDEBUG */


#if YYSTACKDEPTH<=0
/*
** Try to increase the size of the parser stack.
*/
static void yyGrowStack(yyParser *p){
  int newSize;
  yyStackEntry *pNew;

  newSize = p->yystksz*2 + 100;
  pNew = realloc(p->yystack, newSize*sizeof(pNew[0]));
  if( pNew ){
    p->yystack = pNew;
    p->yystksz = newSize;
#ifndef NDEBUG
    if( yyTraceFILE ){
      fprintf(yyTraceFILE,"%sStack grows to %d entries!\n",
              yyTracePrompt, p->yystksz);
    }
#endif
  }
}
#endif

/* 
** This function allocates a new parser.
** The only argument is a pointer to a function which works like
** malloc.
**
** Inputs:
** A pointer to the function used to allocate memory.
**
** Outputs:
** A pointer to a parser.  This pointer is used in subsequent calls
** to grn_expr_parser and grn_expr_parserFree.
*/
void *grn_expr_parserAlloc(void *(*mallocProc)(size_t)){
  yyParser *pParser;
  pParser = (yyParser*)(*mallocProc)( (size_t)sizeof(yyParser) );
  if( pParser ){
    pParser->yyidx = -1;
#ifdef YYTRACKMAXSTACKDEPTH
    pParser->yyidxMax = 0;
#endif
#if YYSTACKDEPTH<=0
    pParser->yystack = NULL;
    pParser->yystksz = 0;
    yyGrowStack(pParser);
#endif
  }
  return pParser;
}

/* The following function deletes the value associated with a
** symbol.  The symbol can be either a terminal or nonterminal.
** "yymajor" is the symbol code, and "yypminor" is a pointer to
** the value.
*/
static void yy_destructor(
  yyParser *yypParser,    /* The parser */
  YYCODETYPE yymajor,     /* Type code for object to destroy */
  YYMINORTYPE *yypminor   /* The object to be destroyed */
){
  grn_expr_parserARG_FETCH;
  switch( yymajor ){
    /* Here is inserted the actions which take place when a
    ** terminal or non-terminal is destroyed.  This can happen
    ** when the symbol is popped from the stack during a
    ** reduce or during error processing or when a parser is 
    ** being destroyed before it is finished parsing.
    **
    ** Note: during a reduce, the only symbols destroyed are those
    ** which appear on the RHS of the rule, but which are not used
    ** inside the C code.
    */
    case 74: /* suppress_unused_variable_warning */
{
#line 11 "grn_ecmascript.lemon"

  (void)efsi;

#line 875 "grn_ecmascript.c"
}
      break;
    default:  break;   /* If no destructor action specified: do nothing */
  }
}

/*
** Pop the parser's stack once.
**
** If there is a destructor routine associated with the token which
** is popped from the stack, then call it.
**
** Return the major token number for the symbol popped.
*/
static int yy_pop_parser_stack(yyParser *pParser){
  YYCODETYPE yymajor;
  yyStackEntry *yytos = &pParser->yystack[pParser->yyidx];

  if( pParser->yyidx<0 ) return 0;
#ifndef NDEBUG
  if( yyTraceFILE && pParser->yyidx>=0 ){
    fprintf(yyTraceFILE,"%sPopping %s\n",
      yyTracePrompt,
      yyTokenName[yytos->major]);
  }
#endif
  yymajor = yytos->major;
  yy_destructor(pParser, yymajor, &yytos->minor);
  pParser->yyidx--;
  return yymajor;
}

/* 
** Deallocate and destroy a parser.  Destructors are all called for
** all stack elements before shutting the parser down.
**
** Inputs:
** <ul>
** <li>  A pointer to the parser.  This should be a pointer
**       obtained from grn_expr_parserAlloc.
** <li>  A pointer to a function used to reclaim memory obtained
**       from malloc.
** </ul>
*/
void grn_expr_parserFree(
  void *p,                    /* The parser to be deleted */
  void (*freeProc)(void*)     /* Function used to reclaim memory */
){
  yyParser *pParser = (yyParser*)p;
  if( pParser==0 ) return;
  while( pParser->yyidx>=0 ) yy_pop_parser_stack(pParser);
#if YYSTACKDEPTH<=0
  free(pParser->yystack);
#endif
  (*freeProc)((void*)pParser);
}

/*
** Return the peak depth of the stack for a parser.
*/
#ifdef YYTRACKMAXSTACKDEPTH
int grn_expr_parserStackPeak(void *p){
  yyParser *pParser = (yyParser*)p;
  return pParser->yyidxMax;
}
#endif

/*
** Find the appropriate action for a parser given the terminal
** look-ahead token iLookAhead.
**
** If the look-ahead token is YYNOCODE, then check to see if the action is
** independent of the look-ahead.  If it is, return the action, otherwise
** return YY_NO_ACTION.
*/
static int yy_find_shift_action(
  yyParser *pParser,        /* The parser */
  YYCODETYPE iLookAhead     /* The look-ahead token */
){
  int i;
  int stateno = pParser->yystack[pParser->yyidx].stateno;
 
  if( stateno>YY_SHIFT_COUNT
   || (i = yy_shift_ofst[stateno])==YY_SHIFT_USE_DFLT ){
    return yy_default[stateno];
  }
  assert( iLookAhead!=YYNOCODE );
  i += iLookAhead;
  if( i<0 || i>=YY_ACTTAB_COUNT || yy_lookahead[i]!=iLookAhead ){
    if( iLookAhead>0 ){
#ifdef YYFALLBACK
      YYCODETYPE iFallback;            /* Fallback token */
      if( iLookAhead<sizeof(yyFallback)/sizeof(yyFallback[0])
             && (iFallback = yyFallback[iLookAhead])!=0 ){
#ifndef NDEBUG
        if( yyTraceFILE ){
          fprintf(yyTraceFILE, "%sFALLBACK %s => %s\n",
             yyTracePrompt, yyTokenName[iLookAhead], yyTokenName[iFallback]);
        }
#endif
        return yy_find_shift_action(pParser, iFallback);
      }
#endif
#ifdef YYWILDCARD
      {
        int j = i - iLookAhead + YYWILDCARD;
        if( 
#if YY_SHIFT_MIN+YYWILDCARD<0
          j>=0 &&
#endif
#if YY_SHIFT_MAX+YYWILDCARD>=YY_ACTTAB_COUNT
          j<YY_ACTTAB_COUNT &&
#endif
          yy_lookahead[j]==YYWILDCARD
        ){
#ifndef NDEBUG
          if( yyTraceFILE ){
            fprintf(yyTraceFILE, "%sWILDCARD %s => %s\n",
               yyTracePrompt, yyTokenName[iLookAhead], yyTokenName[YYWILDCARD]);
          }
#endif /* NDEBUG */
          return yy_action[j];
        }
      }
#endif /* YYWILDCARD */
    }
    return yy_default[stateno];
  }else{
    return yy_action[i];
  }
}

/*
** Find the appropriate action for a parser given the non-terminal
** look-ahead token iLookAhead.
**
** If the look-ahead token is YYNOCODE, then check to see if the action is
** independent of the look-ahead.  If it is, return the action, otherwise
** return YY_NO_ACTION.
*/
static int yy_find_reduce_action(
  int stateno,              /* Current state number */
  YYCODETYPE iLookAhead     /* The look-ahead token */
){
  int i;
#ifdef YYERRORSYMBOL
  if( stateno>YY_REDUCE_COUNT ){
    return yy_default[stateno];
  }
#else
  assert( stateno<=YY_REDUCE_COUNT );
#endif
  i = yy_reduce_ofst[stateno];
  assert( i!=YY_REDUCE_USE_DFLT );
  assert( iLookAhead!=YYNOCODE );
  i += iLookAhead;
#ifdef YYERRORSYMBOL
  if( i<0 || i>=YY_ACTTAB_COUNT || yy_lookahead[i]!=iLookAhead ){
    return yy_default[stateno];
  }
#else
  assert( i>=0 && i<YY_ACTTAB_COUNT );
  assert( yy_lookahead[i]==iLookAhead );
#endif
  return yy_action[i];
}

/*
** The following routine is called if the stack overflows.
*/
static void yyStackOverflow(yyParser *yypParser, YYMINORTYPE *yypMinor){
   grn_expr_parserARG_FETCH;
   yypParser->yyidx--;
#ifndef NDEBUG
   if( yyTraceFILE ){
     fprintf(yyTraceFILE,"%sStack Overflow!\n",yyTracePrompt);
   }
#endif
   while( yypParser->yyidx>=0 ) yy_pop_parser_stack(yypParser);
   /* Here code is inserted which will execute if the parser
   ** stack every overflows */
   grn_expr_parserARG_STORE; /* Suppress warning about unused %extra_argument var */
}

/*
** Perform a shift action.
*/
static void yy_shift(
  yyParser *yypParser,          /* The parser to be shifted */
  int yyNewState,               /* The new state to shift in */
  int yyMajor,                  /* The major token to shift in */
  YYMINORTYPE *yypMinor         /* Pointer to the minor token to shift in */
){
  yyStackEntry *yytos;
  yypParser->yyidx++;
#ifdef YYTRACKMAXSTACKDEPTH
  if( yypParser->yyidx>yypParser->yyidxMax ){
    yypParser->yyidxMax = yypParser->yyidx;
  }
#endif
#if YYSTACKDEPTH>0 
  if( yypParser->yyidx>=YYSTACKDEPTH ){
    yyStackOverflow(yypParser, yypMinor);
    return;
  }
#else
  if( yypParser->yyidx>=yypParser->yystksz ){
    yyGrowStack(yypParser);
    if( yypParser->yyidx>=yypParser->yystksz ){
      yyStackOverflow(yypParser, yypMinor);
      return;
    }
  }
#endif
  yytos = &yypParser->yystack[yypParser->yyidx];
  yytos->stateno = (YYACTIONTYPE)yyNewState;
  yytos->major = (YYCODETYPE)yyMajor;
  yytos->minor = *yypMinor;
#ifndef NDEBUG
  if( yyTraceFILE && yypParser->yyidx>0 ){
    int i;
    fprintf(yyTraceFILE,"%sShift %d\n",yyTracePrompt,yyNewState);
    fprintf(yyTraceFILE,"%sStack:",yyTracePrompt);
    for(i=1; i<=yypParser->yyidx; i++)
      fprintf(yyTraceFILE," %s",yyTokenName[yypParser->yystack[i].major]);
    fprintf(yyTraceFILE,"\n");
  }
#endif
}

/* The following table contains information about every rule that
** is used during the reduce.
*/
static const struct {
  YYCODETYPE lhs;         /* Symbol on the left-hand side of the rule */
  unsigned char nrhs;     /* Number of right-hand side symbols in the rule */
} yyRuleInfo[] = {
  { 75, 1 },
  { 75, 1 },
  { 75, 2 },
  { 75, 2 },
  { 76, 1 },
  { 76, 2 },
  { 76, 3 },
  { 76, 3 },
  { 76, 3 },
  { 80, 1 },
  { 80, 3 },
  { 80, 2 },
  { 80, 3 },
  { 80, 3 },
  { 80, 2 },
  { 77, 1 },
  { 77, 3 },
  { 82, 1 },
  { 82, 3 },
  { 82, 3 },
  { 82, 3 },
  { 82, 3 },
  { 82, 3 },
  { 82, 3 },
  { 82, 3 },
  { 82, 3 },
  { 82, 3 },
  { 82, 3 },
  { 82, 3 },
  { 82, 3 },
  { 83, 1 },
  { 83, 5 },
  { 85, 1 },
  { 85, 3 },
  { 86, 1 },
  { 86, 3 },
  { 86, 3 },
  { 87, 1 },
  { 87, 3 },
  { 88, 1 },
  { 88, 3 },
  { 89, 1 },
  { 89, 3 },
  { 90, 1 },
  { 90, 3 },
  { 90, 3 },
  { 91, 1 },
  { 91, 3 },
  { 91, 3 },
  { 91, 3 },
  { 91, 3 },
  { 91, 3 },
  { 91, 3 },
  { 91, 3 },
  { 91, 3 },
  { 91, 3 },
  { 91, 3 },
  { 91, 3 },
  { 91, 3 },
  { 91, 3 },
  { 92, 1 },
  { 92, 3 },
  { 92, 3 },
  { 92, 3 },
  { 93, 1 },
  { 93, 3 },
  { 93, 3 },
  { 94, 1 },
  { 94, 3 },
  { 94, 3 },
  { 94, 3 },
  { 95, 1 },
  { 95, 2 },
  { 95, 2 },
  { 95, 2 },
  { 95, 2 },
  { 95, 2 },
  { 95, 2 },
  { 95, 2 },
  { 95, 2 },
  { 95, 2 },
  { 95, 2 },
  { 95, 2 },
  { 96, 1 },
  { 96, 2 },
  { 96, 2 },
  { 84, 1 },
  { 84, 1 },
  { 97, 2 },
  { 98, 1 },
  { 98, 2 },
  { 81, 1 },
  { 81, 3 },
  { 81, 1 },
  { 81, 1 },
  { 81, 1 },
  { 81, 1 },
  { 81, 1 },
  { 81, 1 },
  { 81, 1 },
  { 102, 3 },
  { 102, 4 },
  { 102, 3 },
  { 103, 1 },
  { 103, 2 },
  { 104, 1 },
  { 104, 2 },
  { 104, 3 },
  { 101, 3 },
  { 105, 0 },
  { 105, 3 },
  { 106, 3 },
  { 107, 1 },
  { 100, 3 },
  { 100, 2 },
  { 99, 3 },
  { 108, 0 },
  { 108, 1 },
  { 108, 3 },
  { 78, 0 },
  { 78, 1 },
  { 78, 3 },
  { 109, 1 },
  { 109, 1 },
  { 109, 1 },
  { 79, 0 },
  { 79, 1 },
  { 79, 3 },
  { 110, 1 },
  { 110, 3 },
  { 111, 3 },
};

static void yy_accept(yyParser*);  /* Forward Declaration */

/*
** Perform a reduce action and the shift that must immediately
** follow the reduce.
*/
static void yy_reduce(
  yyParser *yypParser,         /* The parser */
  int yyruleno                 /* Number of the rule by which to reduce */
){
  int yygoto;                     /* The next state */
  int yyact;                      /* The next action */
  YYMINORTYPE yygotominor;        /* The LHS of the rule reduced */
  yyStackEntry *yymsp;            /* The top of the parser's stack */
  int yysize;                     /* Amount to pop the stack */
  grn_expr_parserARG_FETCH;
  yymsp = &yypParser->yystack[yypParser->yyidx];
#ifndef NDEBUG
  if( yyTraceFILE && yyruleno>=0 
        && yyruleno<(int)(sizeof(yyRuleName)/sizeof(yyRuleName[0])) ){
    fprintf(yyTraceFILE, "%sReduce [%s].\n", yyTracePrompt,
      yyRuleName[yyruleno]);
  }
#endif /* NDEBUG */

  /* Silence complaints from purify about yygotominor being uninitialized
  ** in some cases when it is copied into the stack after the following
  ** switch.  yygotominor is uninitialized when a rule reduces that does
  ** not set the value of its left-hand side nonterminal.  Leaving the
  ** value of the nonterminal uninitialized is utterly harmless as long
  ** as the value is never used.  So really the only thing this code
  ** accomplishes is to quieten purify.  
  **
  ** 2007-01-16:  The wireshark project (www.wireshark.org) reports that
  ** without this code, their parser segfaults.  I'm not sure what there
  ** parser is doing to make this happen.  This is the second bug report
  ** from wireshark this week.  Clearly they are stressing Lemon in ways
  ** that it has not been previously stressed...  (SQLite ticket #2172)
  */
  /*memset(&yygotominor, 0, sizeof(yygotominor));*/
  yygotominor = yyzerominor;


  switch( yyruleno ){
  /* Beginning here are the reduction cases.  A typical example
  ** follows:
  **   case 0:
  **  #line <lineno> <grammarfile>
  **     { ... }           // User supplied code
  **  #line <lineno> <thisfile>
  **     break;
  */
      case 5: /* query ::= query query_element */
#line 46 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, grn_int32_value_at(&efsi->op_stack, -1), 2);
}
#line 1303 "grn_ecmascript.c"
        break;
      case 6: /* query ::= query LOGICAL_AND query_element */
      case 35: /* logical_and_expression ::= logical_and_expression LOGICAL_AND bitwise_or_expression */ yytestcase(yyruleno==35);
#line 49 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_AND, 2);
}
#line 1311 "grn_ecmascript.c"
        break;
      case 7: /* query ::= query LOGICAL_AND_NOT query_element */
      case 36: /* logical_and_expression ::= logical_and_expression LOGICAL_AND_NOT bitwise_or_expression */ yytestcase(yyruleno==36);
#line 52 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_AND_NOT, 2);
}
#line 1319 "grn_ecmascript.c"
        break;
      case 8: /* query ::= query LOGICAL_OR query_element */
      case 33: /* logical_or_expression ::= logical_or_expression LOGICAL_OR logical_and_expression */ yytestcase(yyruleno==33);
#line 55 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_OR, 2);
}
#line 1327 "grn_ecmascript.c"
        break;
      case 11: /* query_element ::= RELATIVE_OP query_element */
#line 62 "grn_ecmascript.lemon"
{
  int mode;
  GRN_INT32_POP(&efsi->mode_stack, mode);
}
#line 1335 "grn_ecmascript.c"
        break;
      case 12: /* query_element ::= IDENTIFIER RELATIVE_OP query_element */
#line 66 "grn_ecmascript.lemon"
{
  int mode;
  grn_obj *c;
  GRN_PTR_POP(&efsi->column_stack, c);
  GRN_INT32_POP(&efsi->mode_stack, mode);
  switch (mode) {
  case GRN_OP_NEAR :
  case GRN_OP_NEAR2 :
    {
      int max_interval;
      GRN_INT32_POP(&efsi->max_interval_stack, max_interval);
    }
    break;
  case GRN_OP_SIMILAR :
    {
      int similarity_threshold;
      GRN_INT32_POP(&efsi->similarity_threshold_stack, similarity_threshold);
    }
    break;
  default :
    break;
  }
}
#line 1362 "grn_ecmascript.c"
        break;
      case 13: /* query_element ::= BRACEL expression BRACER */
      case 14: /* query_element ::= EVAL primary_expression */ yytestcase(yyruleno==14);
#line 89 "grn_ecmascript.lemon"
{
  efsi->flags = efsi->default_flags;
}
#line 1370 "grn_ecmascript.c"
        break;
      case 16: /* expression ::= expression COMMA assignment_expression */
#line 97 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_COMMA, 2);
}
#line 1377 "grn_ecmascript.c"
        break;
      case 18: /* assignment_expression ::= lefthand_side_expression ASSIGN assignment_expression */
#line 102 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_ASSIGN, 2);
}
#line 1384 "grn_ecmascript.c"
        break;
      case 19: /* assignment_expression ::= lefthand_side_expression STAR_ASSIGN assignment_expression */
#line 105 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_STAR_ASSIGN, 2);
}
#line 1391 "grn_ecmascript.c"
        break;
      case 20: /* assignment_expression ::= lefthand_side_expression SLASH_ASSIGN assignment_expression */
#line 108 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_SLASH_ASSIGN, 2);
}
#line 1398 "grn_ecmascript.c"
        break;
      case 21: /* assignment_expression ::= lefthand_side_expression MOD_ASSIGN assignment_expression */
#line 111 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_MOD_ASSIGN, 2);
}
#line 1405 "grn_ecmascript.c"
        break;
      case 22: /* assignment_expression ::= lefthand_side_expression PLUS_ASSIGN assignment_expression */
#line 114 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_PLUS_ASSIGN, 2);
}
#line 1412 "grn_ecmascript.c"
        break;
      case 23: /* assignment_expression ::= lefthand_side_expression MINUS_ASSIGN assignment_expression */
#line 117 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_MINUS_ASSIGN, 2);
}
#line 1419 "grn_ecmascript.c"
        break;
      case 24: /* assignment_expression ::= lefthand_side_expression SHIFTL_ASSIGN assignment_expression */
#line 120 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_SHIFTL_ASSIGN, 2);
}
#line 1426 "grn_ecmascript.c"
        break;
      case 25: /* assignment_expression ::= lefthand_side_expression SHIFTR_ASSIGN assignment_expression */
#line 123 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_SHIFTR_ASSIGN, 2);
}
#line 1433 "grn_ecmascript.c"
        break;
      case 26: /* assignment_expression ::= lefthand_side_expression SHIFTRR_ASSIGN assignment_expression */
#line 126 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_SHIFTRR_ASSIGN, 2);
}
#line 1440 "grn_ecmascript.c"
        break;
      case 27: /* assignment_expression ::= lefthand_side_expression AND_ASSIGN assignment_expression */
#line 129 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_AND_ASSIGN, 2);
}
#line 1447 "grn_ecmascript.c"
        break;
      case 28: /* assignment_expression ::= lefthand_side_expression XOR_ASSIGN assignment_expression */
#line 132 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_XOR_ASSIGN, 2);
}
#line 1454 "grn_ecmascript.c"
        break;
      case 29: /* assignment_expression ::= lefthand_side_expression OR_ASSIGN assignment_expression */
#line 135 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_OR_ASSIGN, 2);
}
#line 1461 "grn_ecmascript.c"
        break;
      case 31: /* conditional_expression ::= logical_or_expression QUESTION assignment_expression COLON assignment_expression */
#line 140 "grn_ecmascript.lemon"
{
  grn_expr *e = (grn_expr *)efsi->e;
  e->codes[yymsp[-3].minor.yy0].nargs = yymsp[-1].minor.yy0 - yymsp[-3].minor.yy0;
  e->codes[yymsp[-1].minor.yy0].nargs = e->codes_curr - yymsp[-1].minor.yy0 - 1;
}
#line 1470 "grn_ecmascript.c"
        break;
      case 38: /* bitwise_or_expression ::= bitwise_or_expression BITWISE_OR bitwise_xor_expression */
#line 160 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_BITWISE_OR, 2);
}
#line 1477 "grn_ecmascript.c"
        break;
      case 40: /* bitwise_xor_expression ::= bitwise_xor_expression BITWISE_XOR bitwise_and_expression */
#line 165 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_BITWISE_XOR, 2);
}
#line 1484 "grn_ecmascript.c"
        break;
      case 42: /* bitwise_and_expression ::= bitwise_and_expression BITWISE_AND equality_expression */
#line 170 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_BITWISE_AND, 2);
}
#line 1491 "grn_ecmascript.c"
        break;
      case 44: /* equality_expression ::= equality_expression EQUAL relational_expression */
#line 175 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_EQUAL, 2);
}
#line 1498 "grn_ecmascript.c"
        break;
      case 45: /* equality_expression ::= equality_expression NOT_EQUAL relational_expression */
#line 178 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_NOT_EQUAL, 2);
}
#line 1505 "grn_ecmascript.c"
        break;
      case 47: /* relational_expression ::= relational_expression LESS shift_expression */
#line 183 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_LESS, 2);
}
#line 1512 "grn_ecmascript.c"
        break;
      case 48: /* relational_expression ::= relational_expression GREATER shift_expression */
#line 186 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_GREATER, 2);
}
#line 1519 "grn_ecmascript.c"
        break;
      case 49: /* relational_expression ::= relational_expression LESS_EQUAL shift_expression */
#line 189 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_LESS_EQUAL, 2);
}
#line 1526 "grn_ecmascript.c"
        break;
      case 50: /* relational_expression ::= relational_expression GREATER_EQUAL shift_expression */
#line 192 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_GREATER_EQUAL, 2);
}
#line 1533 "grn_ecmascript.c"
        break;
      case 51: /* relational_expression ::= relational_expression IN shift_expression */
#line 195 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_IN, 2);
}
#line 1540 "grn_ecmascript.c"
        break;
      case 52: /* relational_expression ::= relational_expression MATCH shift_expression */
      case 130: /* adjust_match_expression ::= IDENTIFIER MATCH STRING */ yytestcase(yyruleno==130);
#line 198 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_MATCH, 2);
}
#line 1548 "grn_ecmascript.c"
        break;
      case 53: /* relational_expression ::= relational_expression NEAR shift_expression */
#line 201 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_NEAR, 2);
}
#line 1555 "grn_ecmascript.c"
        break;
      case 54: /* relational_expression ::= relational_expression NEAR2 shift_expression */
#line 204 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_NEAR2, 2);
}
#line 1562 "grn_ecmascript.c"
        break;
      case 55: /* relational_expression ::= relational_expression SIMILAR shift_expression */
#line 207 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_SIMILAR, 2);
}
#line 1569 "grn_ecmascript.c"
        break;
      case 56: /* relational_expression ::= relational_expression TERM_EXTRACT shift_expression */
#line 210 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_TERM_EXTRACT, 2);
}
#line 1576 "grn_ecmascript.c"
        break;
      case 57: /* relational_expression ::= relational_expression LCP shift_expression */
#line 213 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_LCP, 2);
}
#line 1583 "grn_ecmascript.c"
        break;
      case 58: /* relational_expression ::= relational_expression PREFIX shift_expression */
#line 216 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_PREFIX, 2);
}
#line 1590 "grn_ecmascript.c"
        break;
      case 59: /* relational_expression ::= relational_expression SUFFIX shift_expression */
#line 219 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_SUFFIX, 2);
}
#line 1597 "grn_ecmascript.c"
        break;
      case 61: /* shift_expression ::= shift_expression SHIFTL additive_expression */
#line 224 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_SHIFTL, 2);
}
#line 1604 "grn_ecmascript.c"
        break;
      case 62: /* shift_expression ::= shift_expression SHIFTR additive_expression */
#line 227 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_SHIFTR, 2);
}
#line 1611 "grn_ecmascript.c"
        break;
      case 63: /* shift_expression ::= shift_expression SHIFTRR additive_expression */
#line 230 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_SHIFTRR, 2);
}
#line 1618 "grn_ecmascript.c"
        break;
      case 65: /* additive_expression ::= additive_expression PLUS multiplicative_expression */
      case 127: /* adjuster ::= adjuster PLUS adjust_expression */ yytestcase(yyruleno==127);
#line 235 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_PLUS, 2);
}
#line 1626 "grn_ecmascript.c"
        break;
      case 66: /* additive_expression ::= additive_expression MINUS multiplicative_expression */
#line 238 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_MINUS, 2);
}
#line 1633 "grn_ecmascript.c"
        break;
      case 68: /* multiplicative_expression ::= multiplicative_expression STAR unary_expression */
      case 129: /* adjust_expression ::= adjust_match_expression STAR DECIMAL */ yytestcase(yyruleno==129);
#line 243 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_STAR, 2);
}
#line 1641 "grn_ecmascript.c"
        break;
      case 69: /* multiplicative_expression ::= multiplicative_expression SLASH unary_expression */
#line 246 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_SLASH, 2);
}
#line 1648 "grn_ecmascript.c"
        break;
      case 70: /* multiplicative_expression ::= multiplicative_expression MOD unary_expression */
#line 249 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_MOD, 2);
}
#line 1655 "grn_ecmascript.c"
        break;
      case 72: /* unary_expression ::= DELETE unary_expression */
#line 254 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_DELETE, 1);
}
#line 1662 "grn_ecmascript.c"
        break;
      case 73: /* unary_expression ::= INCR unary_expression */
#line 257 "grn_ecmascript.lemon"
{
  grn_ctx *ctx = efsi->ctx;
  grn_expr *e = (grn_expr *)(efsi->e);
  grn_expr_dfi *dfi_;
  unsigned int const_p;

  DFI_POP(e, dfi_);
  const_p = CONSTP(dfi_->code->value);
  DFI_PUT(e, dfi_->type, dfi_->domain, dfi_->code);
  if (const_p) {
    ERR(GRN_SYNTAX_ERROR,
        "constant can't be incremented (%.*s)",
        (int)(efsi->str_end - efsi->str), efsi->str);
  } else {
    grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_INCR, 1);
  }
}
#line 1683 "grn_ecmascript.c"
        break;
      case 74: /* unary_expression ::= DECR unary_expression */
#line 274 "grn_ecmascript.lemon"
{
  grn_ctx *ctx = efsi->ctx;
  grn_expr *e = (grn_expr *)(efsi->e);
  grn_expr_dfi *dfi_;
  unsigned int const_p;

  DFI_POP(e, dfi_);
  const_p = CONSTP(dfi_->code->value);
  DFI_PUT(e, dfi_->type, dfi_->domain, dfi_->code);
  if (const_p) {
    ERR(GRN_SYNTAX_ERROR,
        "constant can't be decremented (%.*s)",
        (int)(efsi->str_end - efsi->str), efsi->str);
  } else {
    grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_DECR, 1);
  }
}
#line 1704 "grn_ecmascript.c"
        break;
      case 75: /* unary_expression ::= PLUS unary_expression */
#line 291 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_PLUS, 1);
}
#line 1711 "grn_ecmascript.c"
        break;
      case 76: /* unary_expression ::= MINUS unary_expression */
#line 294 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_MINUS, 1);
}
#line 1718 "grn_ecmascript.c"
        break;
      case 77: /* unary_expression ::= NOT unary_expression */
#line 297 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_NOT, 1);
}
#line 1725 "grn_ecmascript.c"
        break;
      case 78: /* unary_expression ::= BITWISE_NOT unary_expression */
#line 300 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_BITWISE_NOT, 1);
}
#line 1732 "grn_ecmascript.c"
        break;
      case 79: /* unary_expression ::= ADJUST unary_expression */
#line 303 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_ADJUST, 1);
}
#line 1739 "grn_ecmascript.c"
        break;
      case 80: /* unary_expression ::= EXACT unary_expression */
#line 306 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_EXACT, 1);
}
#line 1746 "grn_ecmascript.c"
        break;
      case 81: /* unary_expression ::= PARTIAL unary_expression */
#line 309 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_PARTIAL, 1);
}
#line 1753 "grn_ecmascript.c"
        break;
      case 82: /* unary_expression ::= UNSPLIT unary_expression */
#line 312 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_UNSPLIT, 1);
}
#line 1760 "grn_ecmascript.c"
        break;
      case 84: /* postfix_expression ::= lefthand_side_expression INCR */
#line 317 "grn_ecmascript.lemon"
{
  grn_ctx *ctx = efsi->ctx;
  grn_expr *e = (grn_expr *)(efsi->e);
  grn_expr_dfi *dfi_;
  unsigned int const_p;

  DFI_POP(e, dfi_);
  const_p = CONSTP(dfi_->code->value);
  DFI_PUT(e, dfi_->type, dfi_->domain, dfi_->code);
  if (const_p) {
    ERR(GRN_SYNTAX_ERROR,
        "constant can't be incremented (%.*s)",
        (int)(efsi->str_end - efsi->str), efsi->str);
  } else {
    grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_INCR_POST, 1);
  }
}
#line 1781 "grn_ecmascript.c"
        break;
      case 85: /* postfix_expression ::= lefthand_side_expression DECR */
#line 334 "grn_ecmascript.lemon"
{
  grn_ctx *ctx = efsi->ctx;
  grn_expr *e = (grn_expr *)(efsi->e);
  grn_expr_dfi *dfi_;
  unsigned int const_p;

  DFI_POP(e, dfi_);
  const_p = CONSTP(dfi_->code->value);
  DFI_PUT(e, dfi_->type, dfi_->domain, dfi_->code);
  if (const_p) {
    ERR(GRN_SYNTAX_ERROR,
        "constant can't be decremented (%.*s)",
        (int)(efsi->str_end - efsi->str), efsi->str);
  } else {
    grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_DECR_POST, 1);
  }
}
#line 1802 "grn_ecmascript.c"
        break;
      case 88: /* call_expression ::= member_expression arguments */
#line 355 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_CALL, yymsp[0].minor.yy0);
}
#line 1809 "grn_ecmascript.c"
        break;
      case 113: /* member_expression_part ::= BRACKETL expression BRACKETR */
#line 391 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_GET_MEMBER, 2);
}
#line 1816 "grn_ecmascript.c"
        break;
      case 115: /* arguments ::= PARENL argument_list PARENR */
#line 396 "grn_ecmascript.lemon"
{ yygotominor.yy0 = yymsp[-1].minor.yy0; }
#line 1821 "grn_ecmascript.c"
        break;
      case 116: /* argument_list ::= */
#line 397 "grn_ecmascript.lemon"
{ yygotominor.yy0 = 0; }
#line 1826 "grn_ecmascript.c"
        break;
      case 117: /* argument_list ::= assignment_expression */
#line 398 "grn_ecmascript.lemon"
{ yygotominor.yy0 = 1; }
#line 1831 "grn_ecmascript.c"
        break;
      case 118: /* argument_list ::= argument_list COMMA assignment_expression */
#line 399 "grn_ecmascript.lemon"
{ yygotominor.yy0 = yymsp[-2].minor.yy0 + 1; }
#line 1836 "grn_ecmascript.c"
        break;
      case 119: /* output_columns ::= */
#line 401 "grn_ecmascript.lemon"
{
  yygotominor.yy0 = 0;
}
#line 1843 "grn_ecmascript.c"
        break;
      case 120: /* output_columns ::= output_column */
#line 404 "grn_ecmascript.lemon"
{
  if (yymsp[0].minor.yy0) {
    yygotominor.yy0 = 0;
  } else {
    yygotominor.yy0 = 1;
  }
}
#line 1854 "grn_ecmascript.c"
        break;
      case 121: /* output_columns ::= output_columns COMMA output_column */
#line 412 "grn_ecmascript.lemon"
{
  if (yymsp[0].minor.yy0) {
    yygotominor.yy0 = yymsp[-2].minor.yy0;
  } else {
    if (yymsp[-2].minor.yy0 == 1) {
      grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_COMMA, 2);
    }
    yygotominor.yy0 = 1;
  }
}
#line 1868 "grn_ecmascript.c"
        break;
      case 122: /* output_column ::= STAR */
#line 423 "grn_ecmascript.lemon"
{
  grn_ctx *ctx = efsi->ctx;
  grn_obj *expr = efsi->e;
  grn_expr *e = (grn_expr *)expr;
  grn_obj *variable = grn_expr_get_var_by_offset(ctx, expr, 0);
  if (variable) {
    grn_id table_id = GRN_OBJ_GET_DOMAIN(variable);
    grn_obj *table = grn_ctx_at(ctx, table_id);
    grn_obj columns_buffer;
    grn_obj **columns;
    int i, n_columns;

    GRN_PTR_INIT(&columns_buffer, GRN_OBJ_VECTOR, GRN_ID_NIL);
    grn_obj_columns(ctx, table, "*", strlen("*"), &columns_buffer);
    n_columns = GRN_BULK_VSIZE(&columns_buffer) / sizeof(grn_obj *);
    columns = (grn_obj **)GRN_BULK_HEAD(&columns_buffer);

    for (i = 0; i < n_columns; i++) {
      if (i > 0) {
        grn_expr_append_op(ctx, expr, GRN_OP_COMMA, 2);
      }
      grn_expr_append_const(ctx, expr, columns[i], GRN_OP_GET_VALUE, 1);
      GRN_PTR_PUT(ctx, &e->objs, columns[i]);
    }

    GRN_OBJ_FIN(ctx, &columns_buffer);

    if (n_columns > 0) {
      yygotominor.yy0 = GRN_FALSE;
    } else {
      yygotominor.yy0 = GRN_TRUE;
    }
  } else {
    /* TODO: report error */
    yygotominor.yy0 = GRN_TRUE;
  }
}
#line 1909 "grn_ecmascript.c"
        break;
      case 123: /* output_column ::= NONEXISTENT_COLUMN */
#line 460 "grn_ecmascript.lemon"
{
  yygotominor.yy0 = GRN_TRUE;
}
#line 1916 "grn_ecmascript.c"
        break;
      case 124: /* output_column ::= assignment_expression */
#line 463 "grn_ecmascript.lemon"
{
  yygotominor.yy0 = GRN_FALSE;
}
#line 1923 "grn_ecmascript.c"
        break;
      default:
      /* (0) input ::= query */ yytestcase(yyruleno==0);
      /* (1) input ::= expression */ yytestcase(yyruleno==1);
      /* (2) input ::= START_OUTPUT_COLUMNS output_columns */ yytestcase(yyruleno==2);
      /* (3) input ::= START_ADJUSTER adjuster */ yytestcase(yyruleno==3);
      /* (4) query ::= query_element */ yytestcase(yyruleno==4);
      /* (9) query_element ::= QSTRING */ yytestcase(yyruleno==9);
      /* (10) query_element ::= PARENL query PARENR */ yytestcase(yyruleno==10);
      /* (15) expression ::= assignment_expression */ yytestcase(yyruleno==15);
      /* (17) assignment_expression ::= conditional_expression */ yytestcase(yyruleno==17);
      /* (30) conditional_expression ::= logical_or_expression */ yytestcase(yyruleno==30);
      /* (32) logical_or_expression ::= logical_and_expression */ yytestcase(yyruleno==32);
      /* (34) logical_and_expression ::= bitwise_or_expression */ yytestcase(yyruleno==34);
      /* (37) bitwise_or_expression ::= bitwise_xor_expression */ yytestcase(yyruleno==37);
      /* (39) bitwise_xor_expression ::= bitwise_and_expression */ yytestcase(yyruleno==39);
      /* (41) bitwise_and_expression ::= equality_expression */ yytestcase(yyruleno==41);
      /* (43) equality_expression ::= relational_expression */ yytestcase(yyruleno==43);
      /* (46) relational_expression ::= shift_expression */ yytestcase(yyruleno==46);
      /* (60) shift_expression ::= additive_expression */ yytestcase(yyruleno==60);
      /* (64) additive_expression ::= multiplicative_expression */ yytestcase(yyruleno==64);
      /* (67) multiplicative_expression ::= unary_expression */ yytestcase(yyruleno==67);
      /* (71) unary_expression ::= postfix_expression */ yytestcase(yyruleno==71);
      /* (83) postfix_expression ::= lefthand_side_expression */ yytestcase(yyruleno==83);
      /* (86) lefthand_side_expression ::= call_expression */ yytestcase(yyruleno==86);
      /* (87) lefthand_side_expression ::= member_expression */ yytestcase(yyruleno==87);
      /* (89) member_expression ::= primary_expression */ yytestcase(yyruleno==89);
      /* (90) member_expression ::= member_expression member_expression_part */ yytestcase(yyruleno==90);
      /* (91) primary_expression ::= object_literal */ yytestcase(yyruleno==91);
      /* (92) primary_expression ::= PARENL expression PARENR */ yytestcase(yyruleno==92);
      /* (93) primary_expression ::= IDENTIFIER */ yytestcase(yyruleno==93);
      /* (94) primary_expression ::= array_literal */ yytestcase(yyruleno==94);
      /* (95) primary_expression ::= DECIMAL */ yytestcase(yyruleno==95);
      /* (96) primary_expression ::= HEX_INTEGER */ yytestcase(yyruleno==96);
      /* (97) primary_expression ::= STRING */ yytestcase(yyruleno==97);
      /* (98) primary_expression ::= BOOLEAN */ yytestcase(yyruleno==98);
      /* (99) primary_expression ::= NULL */ yytestcase(yyruleno==99);
      /* (100) array_literal ::= BRACKETL elision BRACKETR */ yytestcase(yyruleno==100);
      /* (101) array_literal ::= BRACKETL element_list elision BRACKETR */ yytestcase(yyruleno==101);
      /* (102) array_literal ::= BRACKETL element_list BRACKETR */ yytestcase(yyruleno==102);
      /* (103) elision ::= COMMA */ yytestcase(yyruleno==103);
      /* (104) elision ::= elision COMMA */ yytestcase(yyruleno==104);
      /* (105) element_list ::= assignment_expression */ yytestcase(yyruleno==105);
      /* (106) element_list ::= elision assignment_expression */ yytestcase(yyruleno==106);
      /* (107) element_list ::= element_list elision assignment_expression */ yytestcase(yyruleno==107);
      /* (108) object_literal ::= BRACEL property_name_and_value_list BRACER */ yytestcase(yyruleno==108);
      /* (109) property_name_and_value_list ::= */ yytestcase(yyruleno==109);
      /* (110) property_name_and_value_list ::= property_name_and_value_list COMMA property_name_and_value */ yytestcase(yyruleno==110);
      /* (111) property_name_and_value ::= property_name COLON assignment_expression */ yytestcase(yyruleno==111);
      /* (112) property_name ::= IDENTIFIER|STRING|DECIMAL */ yytestcase(yyruleno==112);
      /* (114) member_expression_part ::= DOT IDENTIFIER */ yytestcase(yyruleno==114);
      /* (125) adjuster ::= */ yytestcase(yyruleno==125);
      /* (126) adjuster ::= adjust_expression */ yytestcase(yyruleno==126);
      /* (128) adjust_expression ::= adjust_match_expression */ yytestcase(yyruleno==128);
        break;
  };
  yygoto = yyRuleInfo[yyruleno].lhs;
  yysize = yyRuleInfo[yyruleno].nrhs;
  yypParser->yyidx -= yysize;
  yyact = yy_find_reduce_action(yymsp[-yysize].stateno,(YYCODETYPE)yygoto);
  if( yyact < YYNSTATE ){
#ifdef NDEBUG
    /* If we are not debugging and the reduce action popped at least
    ** one element off the stack, then we can push the new element back
    ** onto the stack here, and skip the stack overflow test in yy_shift().
    ** That gives a significant speed improvement. */
    if( yysize ){
      yypParser->yyidx++;
      yymsp -= yysize-1;
      yymsp->stateno = (YYACTIONTYPE)yyact;
      yymsp->major = (YYCODETYPE)yygoto;
      yymsp->minor = yygotominor;
    }else
#endif
    {
      yy_shift(yypParser,yyact,yygoto,&yygotominor);
    }
  }else{
    assert( yyact == YYNSTATE + YYNRULE + 1 );
    yy_accept(yypParser);
  }
}

/*
** The following code executes when the parse fails
*/
#ifndef YYNOERRORRECOVERY
static void yy_parse_failed(
  yyParser *yypParser           /* The parser */
){
  grn_expr_parserARG_FETCH;
#ifndef NDEBUG
  if( yyTraceFILE ){
    fprintf(yyTraceFILE,"%sFail!\n",yyTracePrompt);
  }
#endif
  while( yypParser->yyidx>=0 ) yy_pop_parser_stack(yypParser);
  /* Here code is inserted which will be executed whenever the
  ** parser fails */
  grn_expr_parserARG_STORE; /* Suppress warning about unused %extra_argument variable */
}
#endif /* YYNOERRORRECOVERY */

/*
** The following code executes when a syntax error first occurs.
*/
static void yy_syntax_error(
  yyParser *yypParser,           /* The parser */
  int yymajor,                   /* The major type of the error token */
  YYMINORTYPE yyminor            /* The minor type of the error token */
){
  grn_expr_parserARG_FETCH;
#define TOKEN (yyminor.yy0)
#line 17 "grn_ecmascript.lemon"

  {
    grn_ctx *ctx = efsi->ctx;
    if (ctx->rc == GRN_SUCCESS) {
      grn_obj message;
      GRN_TEXT_INIT(&message, 0);
      GRN_TEXT_PUT(ctx, &message, efsi->str, efsi->cur - efsi->str);
      GRN_TEXT_PUTC(ctx, &message, '|');
      if (efsi->cur < efsi->str_end) {
        GRN_TEXT_PUTC(ctx, &message, efsi->cur[0]);
        GRN_TEXT_PUTC(ctx, &message, '|');
        GRN_TEXT_PUT(ctx, &message,
                     efsi->cur + 1, efsi->str_end - (efsi->cur + 1));
      } else {
        GRN_TEXT_PUTC(ctx, &message, '|');
      }
      ERR(GRN_SYNTAX_ERROR, "Syntax error: <%.*s>",
          (int)GRN_TEXT_LEN(&message), GRN_TEXT_VALUE(&message));
      GRN_OBJ_FIN(ctx, &message);
    }
  }
#line 2059 "grn_ecmascript.c"
  grn_expr_parserARG_STORE; /* Suppress warning about unused %extra_argument variable */
}

/*
** The following is executed when the parser accepts
*/
static void yy_accept(
  yyParser *yypParser           /* The parser */
){
  grn_expr_parserARG_FETCH;
#ifndef NDEBUG
  if( yyTraceFILE ){
    fprintf(yyTraceFILE,"%sAccept!\n",yyTracePrompt);
  }
#endif
  while( yypParser->yyidx>=0 ) yy_pop_parser_stack(yypParser);
  /* Here code is inserted which will be executed whenever the
  ** parser accepts */
  grn_expr_parserARG_STORE; /* Suppress warning about unused %extra_argument variable */
}

/* The main parser program.
** The first argument is a pointer to a structure obtained from
** "grn_expr_parserAlloc" which describes the current state of the parser.
** The second argument is the major token number.  The third is
** the minor token.  The fourth optional argument is whatever the
** user wants (and specified in the grammar) and is available for
** use by the action routines.
**
** Inputs:
** <ul>
** <li> A pointer to the parser (an opaque structure.)
** <li> The major token number.
** <li> The minor token number.
** <li> An option argument of a grammar-specified type.
** </ul>
**
** Outputs:
** None.
*/
void grn_expr_parser(
  void *yyp,                   /* The parser */
  int yymajor,                 /* The major token code number */
  grn_expr_parserTOKENTYPE yyminor       /* The value for the token */
  grn_expr_parserARG_PDECL               /* Optional %extra_argument parameter */
){
  YYMINORTYPE yyminorunion;
  int yyact;            /* The parser action. */
  int yyendofinput;     /* True if we are at the end of input */
#ifdef YYERRORSYMBOL
  int yyerrorhit = 0;   /* True if yymajor has invoked an error */
#endif
  yyParser *yypParser;  /* The parser */

  /* (re)initialize the parser, if necessary */
  yypParser = (yyParser*)yyp;
  if( yypParser->yyidx<0 ){
#if YYSTACKDEPTH<=0
    if( yypParser->yystksz <=0 ){
      /*memset(&yyminorunion, 0, sizeof(yyminorunion));*/
      yyminorunion = yyzerominor;
      yyStackOverflow(yypParser, &yyminorunion);
      return;
    }
#endif
    yypParser->yyidx = 0;
    yypParser->yyerrcnt = -1;
    yypParser->yystack[0].stateno = 0;
    yypParser->yystack[0].major = 0;
  }
  yyminorunion.yy0 = yyminor;
  yyendofinput = (yymajor==0);
  grn_expr_parserARG_STORE;

#ifndef NDEBUG
  if( yyTraceFILE ){
    fprintf(yyTraceFILE,"%sInput %s\n",yyTracePrompt,yyTokenName[yymajor]);
  }
#endif

  do{
    yyact = yy_find_shift_action(yypParser,(YYCODETYPE)yymajor);
    if( yyact<YYNSTATE ){
      assert( !yyendofinput );  /* Impossible to shift the $ token */
      yy_shift(yypParser,yyact,yymajor,&yyminorunion);
      yypParser->yyerrcnt--;
      yymajor = YYNOCODE;
    }else if( yyact < YYNSTATE + YYNRULE ){
      yy_reduce(yypParser,yyact-YYNSTATE);
    }else{
      assert( yyact == YY_ERROR_ACTION );
#ifdef YYERRORSYMBOL
      int yymx;
#endif
#ifndef NDEBUG
      if( yyTraceFILE ){
        fprintf(yyTraceFILE,"%sSyntax Error!\n",yyTracePrompt);
      }
#endif
#ifdef YYERRORSYMBOL
      /* A syntax error has occurred.
      ** The response to an error depends upon whether or not the
      ** grammar defines an error token "ERROR".  
      **
      ** This is what we do if the grammar does define ERROR:
      **
      **  * Call the %syntax_error function.
      **
      **  * Begin popping the stack until we enter a state where
      **    it is legal to shift the error symbol, then shift
      **    the error symbol.
      **
      **  * Set the error count to three.
      **
      **  * Begin accepting and shifting new tokens.  No new error
      **    processing will occur until three tokens have been
      **    shifted successfully.
      **
      */
      if( yypParser->yyerrcnt<0 ){
        yy_syntax_error(yypParser,yymajor,yyminorunion);
      }
      yymx = yypParser->yystack[yypParser->yyidx].major;
      if( yymx==YYERRORSYMBOL || yyerrorhit ){
#ifndef NDEBUG
        if( yyTraceFILE ){
          fprintf(yyTraceFILE,"%sDiscard input token %s\n",
             yyTracePrompt,yyTokenName[yymajor]);
        }
#endif
        yy_destructor(yypParser, (YYCODETYPE)yymajor,&yyminorunion);
        yymajor = YYNOCODE;
      }else{
         while(
          yypParser->yyidx >= 0 &&
          yymx != YYERRORSYMBOL &&
          (yyact = yy_find_reduce_action(
                        yypParser->yystack[yypParser->yyidx].stateno,
                        YYERRORSYMBOL)) >= YYNSTATE
        ){
          yy_pop_parser_stack(yypParser);
        }
        if( yypParser->yyidx < 0 || yymajor==0 ){
          yy_destructor(yypParser,(YYCODETYPE)yymajor,&yyminorunion);
          yy_parse_failed(yypParser);
          yymajor = YYNOCODE;
        }else if( yymx!=YYERRORSYMBOL ){
          YYMINORTYPE u2;
          u2.YYERRSYMDT = 0;
          yy_shift(yypParser,yyact,YYERRORSYMBOL,&u2);
        }
      }
      yypParser->yyerrcnt = 3;
      yyerrorhit = 1;
#elif defined(YYNOERRORRECOVERY)
      /* If the YYNOERRORRECOVERY macro is defined, then do not attempt to
      ** do any kind of error recovery.  Instead, simply invoke the syntax
      ** error routine and continue going as if nothing had happened.
      **
      ** Applications can set this macro (for example inside %include) if
      ** they intend to abandon the parse upon the first syntax error seen.
      */
      yy_syntax_error(yypParser,yymajor,yyminorunion);
      yy_destructor(yypParser,(YYCODETYPE)yymajor,&yyminorunion);
      yymajor = YYNOCODE;
      
#else  /* YYERRORSYMBOL is not defined */
      /* This is what we do if the grammar does not define ERROR:
      **
      **  * Report an error message, and throw away the input token.
      **
      **  * If the input token is $, then fail the parse.
      **
      ** As before, subsequent error messages are suppressed until
      ** three input tokens have been successfully shifted.
      */
      if( yypParser->yyerrcnt<=0 ){
        yy_syntax_error(yypParser,yymajor,yyminorunion);
      }
      yypParser->yyerrcnt = 3;
      yy_destructor(yypParser,(YYCODETYPE)yymajor,&yyminorunion);
      if( yyendofinput ){
        yy_parse_failed(yypParser);
      }
      yymajor = YYNOCODE;
#endif
    }
  }while( yymajor!=YYNOCODE && yypParser->yyidx>=0 );
  return;
}
