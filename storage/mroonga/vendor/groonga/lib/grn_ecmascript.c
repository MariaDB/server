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
#define YYNOCODE 114
#define YYACTIONTYPE unsigned short int
#define grn_expr_parserTOKENTYPE  int 
typedef union {
  int yyinit;
  grn_expr_parserTOKENTYPE yy0;
  void * yy165;
} YYMINORTYPE;
#ifndef YYSTACKDEPTH
#define YYSTACKDEPTH 100
#endif
#define grn_expr_parserARG_SDECL  efs_info *efsi ;
#define grn_expr_parserARG_PDECL , efs_info *efsi 
#define grn_expr_parserARG_FETCH  efs_info *efsi  = yypParser->efsi 
#define grn_expr_parserARG_STORE yypParser->efsi  = efsi 
#define YYNSTATE 225
#define YYNRULE 132
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
#define YY_ACTTAB_COUNT (1639)
static const YYACTIONTYPE yy_action[] = {
 /*     0 */     2,   71,   53,   52,   51,  222,    1,   76,   80,  125,
 /*    10 */     4,  221,   70,  358,   77,  109,   28,  152,  221,  191,
 /*    20 */   194,  215,   88,  123,  122,  135,  134,  133,  117,   85,
 /*    30 */   100,  113,  101,  180,  211,  197,   74,  190,  186,  190,
 /*    40 */   186,  222,   72,   79,   80,  140,    9,  189,   70,   25,
 /*    50 */    65,   64,  217,   28,   28,   68,   67,   66,   63,   62,
 /*    60 */    61,   60,   59,   58,  185,  184,  183,  182,  181,    3,
 /*    70 */    76,  115,    6,  193,  221,  191,  194,  215,   88,  123,
 /*    80 */   122,  135,  134,  133,  117,   85,  100,  113,  101,  180,
 /*    90 */   211,  197,   74,  166,  107,  190,  186,  222,    1,   23,
 /*   100 */    80,  125,    4,  124,   70,   31,   30,  191,  194,  215,
 /*   110 */    88,  123,  122,  135,  134,  133,  117,   85,  100,  113,
 /*   120 */   101,  180,  211,  197,   74,  141,  129,  190,  186,   36,
 /*   130 */    35,  112,   69,   57,   56,    8,   32,  131,   55,   54,
 /*   140 */    34,   29,   65,   64,  176,   33,   73,   68,   67,   66,
 /*   150 */    63,   62,   61,   60,   59,   58,  185,  184,  183,  182,
 /*   160 */   181,    3,    7,   26,  128,  187,   84,  199,  198,  178,
 /*   170 */   191,  168,  215,   88,  123,  122,  135,  134,  133,  117,
 /*   180 */    85,  100,  113,  101,  180,  211,  197,   74,  144,  129,
 /*   190 */   190,  186,   11,   83,   82,   81,   78,  222,   72,  150,
 /*   200 */    80,  140,    9,  173,   70,   24,   65,   64,  228,  169,
 /*   210 */   167,   68,   67,   66,   63,   62,   61,   60,   59,   58,
 /*   220 */   185,  184,  183,  182,  181,    3,  179,    7,  196,  195,
 /*   230 */   187,   84,  108,  143,  178,  191,  146,  215,   88,  123,
 /*   240 */   122,  135,  134,  133,  117,   85,  100,  113,  101,  180,
 /*   250 */   211,  197,   74,  226,  227,  190,  186,  126,  173,   75,
 /*   260 */   173,  175,  132,  145,  142,  112,  170,   28,    5,   10,
 /*   270 */   223,   65,   64,  220,  127,  219,   68,   67,   66,   63,
 /*   280 */    62,   61,   60,   59,   58,  185,  184,  183,  182,  181,
 /*   290 */     3,  172,    7,  124,  218,  187,   84,  191,  194,  215,
 /*   300 */    88,  123,  122,  135,  134,  133,  117,   85,  100,  113,
 /*   310 */   101,  180,  211,  197,   74,  151,  224,  190,  186,  359,
 /*   320 */    50,   49,   48,   47,   46,   45,   44,   43,   42,   41,
 /*   330 */    40,   39,   38,   37,  359,  359,   65,   64,  148,  359,
 /*   340 */   359,   68,   67,   66,   63,   62,   61,   60,   59,   58,
 /*   350 */   185,  184,  183,  182,  181,    3,  118,  359,  147,  359,
 /*   360 */   191,  194,  215,   88,  123,  122,  135,  134,  133,  117,
 /*   370 */    85,  100,  113,  101,  180,  211,  197,   74,  115,  359,
 /*   380 */   190,  186,  191,  194,  215,   88,  123,  122,  135,  134,
 /*   390 */   133,  117,   85,  100,  113,  101,  180,  211,  197,   74,
 /*   400 */   359,  359,  190,  186,  225,  359,  359,   82,   81,   78,
 /*   410 */   222,   72,  359,   80,  140,    9,  359,   70,  359,  191,
 /*   420 */   164,  215,   88,  123,  122,  135,  134,  133,  117,   85,
 /*   430 */   100,  113,  101,  180,  211,  197,   74,  359,    7,  190,
 /*   440 */   186,  187,   84,  359,  359,  169,  111,  191,  146,  215,
 /*   450 */    88,  123,  122,  135,  134,  133,  117,   85,  100,  113,
 /*   460 */   101,  180,  211,  197,   74,  359,    7,  190,  186,  187,
 /*   470 */    84,  359,  359,  359,  359,  149,  359,  359,  359,  359,
 /*   480 */   359,  359,   65,   64,  359,  359,  359,   68,   67,   66,
 /*   490 */    63,   62,   61,   60,   59,   58,  185,  184,  183,  182,
 /*   500 */   181,    3,  359,  359,  359,  359,  359,  359,  359,  359,
 /*   510 */    65,   64,  359,  359,  359,   68,   67,   66,   63,   62,
 /*   520 */    61,   60,   59,   58,  185,  184,  183,  182,  181,    3,
 /*   530 */   191,  216,  215,   88,  123,  122,  135,  134,  133,  117,
 /*   540 */    85,  100,  113,  101,  180,  211,  197,   74,  359,  359,
 /*   550 */   190,  186,  191,  214,  215,   88,  123,  122,  135,  134,
 /*   560 */   133,  117,   85,  100,  113,  101,  180,  211,  197,   74,
 /*   570 */   359,  359,  190,  186,  191,  139,  215,   88,  123,  122,
 /*   580 */   135,  134,  133,  117,   85,  100,  113,  101,  180,  211,
 /*   590 */   197,   74,  359,  359,  190,  186,  359,  359,  191,  213,
 /*   600 */   215,   88,  123,  122,  135,  134,  133,  117,   85,  100,
 /*   610 */   113,  101,  180,  211,  197,   74,  359,  359,  190,  186,
 /*   620 */   191,  174,  215,   88,  123,  122,  135,  134,  133,  117,
 /*   630 */    85,  100,  113,  101,  180,  211,  197,   74,  359,  359,
 /*   640 */   190,  186,  191,  165,  215,   88,  123,  122,  135,  134,
 /*   650 */   133,  117,   85,  100,  113,  101,  180,  211,  197,   74,
 /*   660 */   359,  359,  190,  186,  191,  163,  215,   88,  123,  122,
 /*   670 */   135,  134,  133,  117,   85,  100,  113,  101,  180,  211,
 /*   680 */   197,   74,  359,  359,  190,  186,  191,  162,  215,   88,
 /*   690 */   123,  122,  135,  134,  133,  117,   85,  100,  113,  101,
 /*   700 */   180,  211,  197,   74,  359,  359,  190,  186,  191,  161,
 /*   710 */   215,   88,  123,  122,  135,  134,  133,  117,   85,  100,
 /*   720 */   113,  101,  180,  211,  197,   74,  359,  359,  190,  186,
 /*   730 */   191,  160,  215,   88,  123,  122,  135,  134,  133,  117,
 /*   740 */    85,  100,  113,  101,  180,  211,  197,   74,  359,  359,
 /*   750 */   190,  186,  191,  159,  215,   88,  123,  122,  135,  134,
 /*   760 */   133,  117,   85,  100,  113,  101,  180,  211,  197,   74,
 /*   770 */   359,  359,  190,  186,  191,  158,  215,   88,  123,  122,
 /*   780 */   135,  134,  133,  117,   85,  100,  113,  101,  180,  211,
 /*   790 */   197,   74,  359,  359,  190,  186,  191,  157,  215,   88,
 /*   800 */   123,  122,  135,  134,  133,  117,   85,  100,  113,  101,
 /*   810 */   180,  211,  197,   74,  359,  359,  190,  186,  191,  156,
 /*   820 */   215,   88,  123,  122,  135,  134,  133,  117,   85,  100,
 /*   830 */   113,  101,  180,  211,  197,   74,  359,  359,  190,  186,
 /*   840 */   191,  155,  215,   88,  123,  122,  135,  134,  133,  117,
 /*   850 */    85,  100,  113,  101,  180,  211,  197,   74,  359,  359,
 /*   860 */   190,  186,  191,  154,  215,   88,  123,  122,  135,  134,
 /*   870 */   133,  117,   85,  100,  113,  101,  180,  211,  197,   74,
 /*   880 */   359,  359,  190,  186,  191,  153,  215,   88,  123,  122,
 /*   890 */   135,  134,  133,  117,   85,  100,  113,  101,  180,  211,
 /*   900 */   197,   74,  359,  359,  190,  186,  191,  177,  215,   88,
 /*   910 */   123,  122,  135,  134,  133,  117,   85,  100,  113,  101,
 /*   920 */   180,  211,  197,   74,  359,  359,  190,  186,  191,  171,
 /*   930 */   215,   88,  123,  122,  135,  134,  133,  117,   85,  100,
 /*   940 */   113,  101,  180,  211,  197,   74,  359,  191,  190,  186,
 /*   950 */   119,  359,  110,  135,  134,  133,  117,   85,  100,  113,
 /*   960 */   101,  180,  211,  197,   74,  359,  191,  190,  186,  119,
 /*   970 */   359,  359,  138,  134,  133,  117,   85,  100,  113,  101,
 /*   980 */   180,  211,  197,   74,  359,  359,  190,  186,  191,  359,
 /*   990 */   359,  119,  359,  359,  130,  134,  133,  117,   85,  100,
 /*  1000 */   113,  101,  180,  211,  197,   74,  359,  359,  190,  186,
 /*  1010 */   191,  359,  359,  119,  359,  359,  359,  137,  133,  117,
 /*  1020 */    85,  100,  113,  101,  180,  211,  197,   74,  359,  359,
 /*  1030 */   190,  186,  359,   27,   22,   21,   20,   19,   18,   17,
 /*  1040 */    16,   15,   14,   13,   12,  191,  359,  359,  119,  359,
 /*  1050 */   359,  359,  359,  136,  117,   85,  100,  113,  101,  180,
 /*  1060 */   211,  197,   74,  359,  359,  190,  186,  359,  359,  191,
 /*  1070 */   359,  359,  119,  359,  359,  199,  198,  359,  121,   85,
 /*  1080 */   100,  113,  101,  180,  211,  197,   74,  359,  191,  190,
 /*  1090 */   186,  119,    7,  359,  359,  187,   84,  359,   87,  100,
 /*  1100 */   113,  101,  180,  211,  197,   74,  359,  191,  190,  186,
 /*  1110 */   119,  359,  359,  359,  359,  359,  359,   86,  100,  113,
 /*  1120 */   101,  180,  211,  197,   74,  359,  191,  190,  186,  119,
 /*  1130 */   359,  359,  359,  359,  359,  359,  359,  106,  113,  101,
 /*  1140 */   180,  211,  197,   74,  359,  191,  190,  186,  119,  359,
 /*  1150 */   185,  184,  183,  182,  181,    3,  104,  113,  101,  180,
 /*  1160 */   211,  197,   74,  359,  191,  190,  186,  119,  359,  359,
 /*  1170 */   359,  359,  359,  359,  359,  102,  113,  101,  180,  211,
 /*  1180 */   197,   74,  359,  191,  190,  186,  119,  359,  359,  359,
 /*  1190 */   359,  359,  359,  359,   99,  113,  101,  180,  211,  197,
 /*  1200 */    74,  359,  191,  190,  186,  119,  359,  359,  359,  359,
 /*  1210 */   359,  359,  359,   98,  113,  101,  180,  211,  197,   74,
 /*  1220 */   359,  191,  190,  186,  119,  359,  359,  359,  359,  359,
 /*  1230 */   359,  359,   97,  113,  101,  180,  211,  197,   74,  359,
 /*  1240 */   191,  190,  186,  119,  359,  359,  359,  359,  359,  359,
 /*  1250 */   359,   96,  113,  101,  180,  211,  197,   74,  359,  191,
 /*  1260 */   190,  186,  119,  359,  359,  359,  359,  359,  359,  359,
 /*  1270 */    95,  113,  101,  180,  211,  197,   74,  359,  191,  190,
 /*  1280 */   186,  119,  359,  359,  359,  359,  359,  359,  359,   94,
 /*  1290 */   113,  101,  180,  211,  197,   74,  359,  191,  190,  186,
 /*  1300 */   119,  359,  359,  359,  359,  359,  359,  359,   93,  113,
 /*  1310 */   101,  180,  211,  197,   74,  359,  191,  190,  186,  119,
 /*  1320 */   359,  359,  359,  359,  359,  359,  359,   92,  113,  101,
 /*  1330 */   180,  211,  197,   74,  359,  191,  190,  186,  119,  359,
 /*  1340 */   359,  359,  359,  359,  359,  359,   91,  113,  101,  180,
 /*  1350 */   211,  197,   74,  359,  191,  190,  186,  119,  359,  359,
 /*  1360 */   359,  359,  359,  359,  359,   90,  113,  101,  180,  211,
 /*  1370 */   197,   74,  359,  191,  190,  186,  119,  359,  359,  359,
 /*  1380 */   359,  359,  359,  359,   89,  113,  101,  180,  211,  197,
 /*  1390 */    74,  359,  191,  190,  186,  119,  359,  359,  359,  359,
 /*  1400 */   359,  359,  359,  359,  120,  101,  180,  211,  197,   74,
 /*  1410 */   359,  191,  190,  186,  119,  359,  359,  359,  359,  359,
 /*  1420 */   359,  359,  359,  116,  101,  180,  211,  197,   74,  359,
 /*  1430 */   191,  190,  186,  119,  359,  359,  359,  359,  359,  359,
 /*  1440 */   359,  359,  114,  101,  180,  211,  197,   74,  359,  191,
 /*  1450 */   190,  186,  119,  359,  359,  359,  359,  359,  191,  359,
 /*  1460 */   359,  119,  105,  180,  211,  197,   74,  359,  359,  190,
 /*  1470 */   186,  103,  180,  211,  197,   74,  359,  191,  190,  186,
 /*  1480 */   119,  359,  359,  359,  359,  359,  359,  191,  359,  359,
 /*  1490 */   119,  212,  211,  197,   74,  359,  191,  190,  186,  119,
 /*  1500 */   359,  210,  211,  197,   74,  359,  191,  190,  186,  119,
 /*  1510 */   209,  211,  197,   74,  359,  191,  190,  186,  119,  359,
 /*  1520 */   208,  211,  197,   74,  359,  191,  190,  186,  119,  207,
 /*  1530 */   211,  197,   74,  359,  191,  190,  186,  119,  359,  206,
 /*  1540 */   211,  197,   74,  359,  191,  190,  186,  119,  205,  211,
 /*  1550 */   197,   74,  359,  191,  190,  186,  119,  359,  204,  211,
 /*  1560 */   197,   74,  359,  191,  190,  186,  119,  203,  211,  197,
 /*  1570 */    74,  359,  359,  190,  186,  359,  359,  202,  211,  197,
 /*  1580 */    74,  359,  191,  190,  186,  119,  359,  359,  359,  359,
 /*  1590 */   191,  359,  359,  119,  359,  359,  201,  211,  197,   74,
 /*  1600 */   359,  359,  190,  186,  200,  211,  197,   74,  359,  191,
 /*  1610 */   190,  186,  119,  359,  359,  359,  359,  191,  359,  359,
 /*  1620 */   119,  359,  359,  192,  211,  197,   74,  359,  359,  190,
 /*  1630 */   186,  188,  211,  197,   74,  359,  359,  190,  186,
};
static const YYCODETYPE yy_lookahead[] = {
 /*     0 */     1,    2,   48,   49,   50,    6,    7,   77,    9,   10,
 /*    10 */    11,   81,   13,   76,   77,   78,   14,   82,   81,   82,
 /*    20 */    83,   84,   85,   86,   87,   88,   89,   90,   91,   92,
 /*    30 */    93,   94,   95,   96,   97,   98,   99,  102,  103,  102,
 /*    40 */   103,    6,    7,    9,    9,   10,   11,    8,   13,   28,
 /*    50 */    51,   52,   12,   14,   14,   56,   57,   58,   59,   60,
 /*    60 */    61,   62,   63,   64,   65,   66,   67,   68,   69,   70,
 /*    70 */    77,   78,    7,   71,   81,   82,   83,   84,   85,   86,
 /*    80 */    87,   88,   89,   90,   91,   92,   93,   94,   95,   96,
 /*    90 */    97,   98,   99,    8,   80,  102,  103,    6,    7,   14,
 /*   100 */     9,   10,   11,   78,   13,    3,    4,   82,   83,   84,
 /*   110 */    85,   86,   87,   88,   89,   90,   91,   92,   93,   94,
 /*   120 */    95,   96,   97,   98,   99,  111,  112,  102,  103,   32,
 /*   130 */    33,  106,   53,   54,   55,   70,   29,   72,   51,   52,
 /*   140 */    31,    5,   51,   52,   12,   30,   14,   56,   57,   58,
 /*   150 */    59,   60,   61,   62,   63,   64,   65,   66,   67,   68,
 /*   160 */    69,   70,    7,   27,   53,   10,   11,   57,   58,   14,
 /*   170 */    82,   83,   84,   85,   86,   87,   88,   89,   90,   91,
 /*   180 */    92,   93,   94,   95,   96,   97,   98,   99,  111,  112,
 /*   190 */   102,  103,  104,  105,    3,    4,    5,    6,    7,    8,
 /*   200 */     9,   10,   11,   10,   13,   28,   51,   52,    0,   14,
 /*   210 */    10,   56,   57,   58,   59,   60,   61,   62,   63,   64,
 /*   220 */    65,   66,   67,   68,   69,   70,   71,    7,  100,  101,
 /*   230 */    10,   11,   79,   65,   14,   82,   83,   84,   85,   86,
 /*   240 */    87,   88,   89,   90,   91,   92,   93,   94,   95,   96,
 /*   250 */    97,   98,   99,    0,    0,  102,  103,   39,   65,   51,
 /*   260 */    67,  107,  108,  110,   67,  106,   71,   14,   14,  104,
 /*   270 */    81,   51,   52,   81,   10,   81,   56,   57,   58,   59,
 /*   280 */    60,   61,   62,   63,   64,   65,   66,   67,   68,   69,
 /*   290 */    70,   71,    7,   78,   81,   10,   11,   82,   83,   84,
 /*   300 */    85,   86,   87,   88,   89,   90,   91,   92,   93,   94,
 /*   310 */    95,   96,   97,   98,   99,   81,   81,  102,  103,  113,
 /*   320 */    34,   35,   36,   37,   38,   39,   40,   41,   42,   43,
 /*   330 */    44,   45,   46,   47,  113,  113,   51,   52,   53,  113,
 /*   340 */   113,   56,   57,   58,   59,   60,   61,   62,   63,   64,
 /*   350 */    65,   66,   67,   68,   69,   70,   78,  113,   73,  113,
 /*   360 */    82,   83,   84,   85,   86,   87,   88,   89,   90,   91,
 /*   370 */    92,   93,   94,   95,   96,   97,   98,   99,   78,  113,
 /*   380 */   102,  103,   82,   83,   84,   85,   86,   87,   88,   89,
 /*   390 */    90,   91,   92,   93,   94,   95,   96,   97,   98,   99,
 /*   400 */   113,  113,  102,  103,    0,  113,  113,    3,    4,    5,
 /*   410 */     6,    7,  113,    9,   10,   11,  113,   13,  113,   82,
 /*   420 */    83,   84,   85,   86,   87,   88,   89,   90,   91,   92,
 /*   430 */    93,   94,   95,   96,   97,   98,   99,  113,    7,  102,
 /*   440 */   103,   10,   11,  113,  113,   14,  109,   82,   83,   84,
 /*   450 */    85,   86,   87,   88,   89,   90,   91,   92,   93,   94,
 /*   460 */    95,   96,   97,   98,   99,  113,    7,  102,  103,   10,
 /*   470 */    11,  113,  113,  113,  113,  110,  113,  113,  113,  113,
 /*   480 */   113,  113,   51,   52,  113,  113,  113,   56,   57,   58,
 /*   490 */    59,   60,   61,   62,   63,   64,   65,   66,   67,   68,
 /*   500 */    69,   70,  113,  113,  113,  113,  113,  113,  113,  113,
 /*   510 */    51,   52,  113,  113,  113,   56,   57,   58,   59,   60,
 /*   520 */    61,   62,   63,   64,   65,   66,   67,   68,   69,   70,
 /*   530 */    82,   83,   84,   85,   86,   87,   88,   89,   90,   91,
 /*   540 */    92,   93,   94,   95,   96,   97,   98,   99,  113,  113,
 /*   550 */   102,  103,   82,   83,   84,   85,   86,   87,   88,   89,
 /*   560 */    90,   91,   92,   93,   94,   95,   96,   97,   98,   99,
 /*   570 */   113,  113,  102,  103,   82,   83,   84,   85,   86,   87,
 /*   580 */    88,   89,   90,   91,   92,   93,   94,   95,   96,   97,
 /*   590 */    98,   99,  113,  113,  102,  103,  113,  113,   82,   83,
 /*   600 */    84,   85,   86,   87,   88,   89,   90,   91,   92,   93,
 /*   610 */    94,   95,   96,   97,   98,   99,  113,  113,  102,  103,
 /*   620 */    82,   83,   84,   85,   86,   87,   88,   89,   90,   91,
 /*   630 */    92,   93,   94,   95,   96,   97,   98,   99,  113,  113,
 /*   640 */   102,  103,   82,   83,   84,   85,   86,   87,   88,   89,
 /*   650 */    90,   91,   92,   93,   94,   95,   96,   97,   98,   99,
 /*   660 */   113,  113,  102,  103,   82,   83,   84,   85,   86,   87,
 /*   670 */    88,   89,   90,   91,   92,   93,   94,   95,   96,   97,
 /*   680 */    98,   99,  113,  113,  102,  103,   82,   83,   84,   85,
 /*   690 */    86,   87,   88,   89,   90,   91,   92,   93,   94,   95,
 /*   700 */    96,   97,   98,   99,  113,  113,  102,  103,   82,   83,
 /*   710 */    84,   85,   86,   87,   88,   89,   90,   91,   92,   93,
 /*   720 */    94,   95,   96,   97,   98,   99,  113,  113,  102,  103,
 /*   730 */    82,   83,   84,   85,   86,   87,   88,   89,   90,   91,
 /*   740 */    92,   93,   94,   95,   96,   97,   98,   99,  113,  113,
 /*   750 */   102,  103,   82,   83,   84,   85,   86,   87,   88,   89,
 /*   760 */    90,   91,   92,   93,   94,   95,   96,   97,   98,   99,
 /*   770 */   113,  113,  102,  103,   82,   83,   84,   85,   86,   87,
 /*   780 */    88,   89,   90,   91,   92,   93,   94,   95,   96,   97,
 /*   790 */    98,   99,  113,  113,  102,  103,   82,   83,   84,   85,
 /*   800 */    86,   87,   88,   89,   90,   91,   92,   93,   94,   95,
 /*   810 */    96,   97,   98,   99,  113,  113,  102,  103,   82,   83,
 /*   820 */    84,   85,   86,   87,   88,   89,   90,   91,   92,   93,
 /*   830 */    94,   95,   96,   97,   98,   99,  113,  113,  102,  103,
 /*   840 */    82,   83,   84,   85,   86,   87,   88,   89,   90,   91,
 /*   850 */    92,   93,   94,   95,   96,   97,   98,   99,  113,  113,
 /*   860 */   102,  103,   82,   83,   84,   85,   86,   87,   88,   89,
 /*   870 */    90,   91,   92,   93,   94,   95,   96,   97,   98,   99,
 /*   880 */   113,  113,  102,  103,   82,   83,   84,   85,   86,   87,
 /*   890 */    88,   89,   90,   91,   92,   93,   94,   95,   96,   97,
 /*   900 */    98,   99,  113,  113,  102,  103,   82,   83,   84,   85,
 /*   910 */    86,   87,   88,   89,   90,   91,   92,   93,   94,   95,
 /*   920 */    96,   97,   98,   99,  113,  113,  102,  103,   82,   83,
 /*   930 */    84,   85,   86,   87,   88,   89,   90,   91,   92,   93,
 /*   940 */    94,   95,   96,   97,   98,   99,  113,   82,  102,  103,
 /*   950 */    85,  113,   87,   88,   89,   90,   91,   92,   93,   94,
 /*   960 */    95,   96,   97,   98,   99,  113,   82,  102,  103,   85,
 /*   970 */   113,  113,   88,   89,   90,   91,   92,   93,   94,   95,
 /*   980 */    96,   97,   98,   99,  113,  113,  102,  103,   82,  113,
 /*   990 */   113,   85,  113,  113,   88,   89,   90,   91,   92,   93,
 /*  1000 */    94,   95,   96,   97,   98,   99,  113,  113,  102,  103,
 /*  1010 */    82,  113,  113,   85,  113,  113,  113,   89,   90,   91,
 /*  1020 */    92,   93,   94,   95,   96,   97,   98,   99,  113,  113,
 /*  1030 */   102,  103,  113,   15,   16,   17,   18,   19,   20,   21,
 /*  1040 */    22,   23,   24,   25,   26,   82,  113,  113,   85,  113,
 /*  1050 */   113,  113,  113,   90,   91,   92,   93,   94,   95,   96,
 /*  1060 */    97,   98,   99,  113,  113,  102,  103,  113,  113,   82,
 /*  1070 */   113,  113,   85,  113,  113,   57,   58,  113,   91,   92,
 /*  1080 */    93,   94,   95,   96,   97,   98,   99,  113,   82,  102,
 /*  1090 */   103,   85,    7,  113,  113,   10,   11,  113,   92,   93,
 /*  1100 */    94,   95,   96,   97,   98,   99,  113,   82,  102,  103,
 /*  1110 */    85,  113,  113,  113,  113,  113,  113,   92,   93,   94,
 /*  1120 */    95,   96,   97,   98,   99,  113,   82,  102,  103,   85,
 /*  1130 */   113,  113,  113,  113,  113,  113,  113,   93,   94,   95,
 /*  1140 */    96,   97,   98,   99,  113,   82,  102,  103,   85,  113,
 /*  1150 */    65,   66,   67,   68,   69,   70,   93,   94,   95,   96,
 /*  1160 */    97,   98,   99,  113,   82,  102,  103,   85,  113,  113,
 /*  1170 */   113,  113,  113,  113,  113,   93,   94,   95,   96,   97,
 /*  1180 */    98,   99,  113,   82,  102,  103,   85,  113,  113,  113,
 /*  1190 */   113,  113,  113,  113,   93,   94,   95,   96,   97,   98,
 /*  1200 */    99,  113,   82,  102,  103,   85,  113,  113,  113,  113,
 /*  1210 */   113,  113,  113,   93,   94,   95,   96,   97,   98,   99,
 /*  1220 */   113,   82,  102,  103,   85,  113,  113,  113,  113,  113,
 /*  1230 */   113,  113,   93,   94,   95,   96,   97,   98,   99,  113,
 /*  1240 */    82,  102,  103,   85,  113,  113,  113,  113,  113,  113,
 /*  1250 */   113,   93,   94,   95,   96,   97,   98,   99,  113,   82,
 /*  1260 */   102,  103,   85,  113,  113,  113,  113,  113,  113,  113,
 /*  1270 */    93,   94,   95,   96,   97,   98,   99,  113,   82,  102,
 /*  1280 */   103,   85,  113,  113,  113,  113,  113,  113,  113,   93,
 /*  1290 */    94,   95,   96,   97,   98,   99,  113,   82,  102,  103,
 /*  1300 */    85,  113,  113,  113,  113,  113,  113,  113,   93,   94,
 /*  1310 */    95,   96,   97,   98,   99,  113,   82,  102,  103,   85,
 /*  1320 */   113,  113,  113,  113,  113,  113,  113,   93,   94,   95,
 /*  1330 */    96,   97,   98,   99,  113,   82,  102,  103,   85,  113,
 /*  1340 */   113,  113,  113,  113,  113,  113,   93,   94,   95,   96,
 /*  1350 */    97,   98,   99,  113,   82,  102,  103,   85,  113,  113,
 /*  1360 */   113,  113,  113,  113,  113,   93,   94,   95,   96,   97,
 /*  1370 */    98,   99,  113,   82,  102,  103,   85,  113,  113,  113,
 /*  1380 */   113,  113,  113,  113,   93,   94,   95,   96,   97,   98,
 /*  1390 */    99,  113,   82,  102,  103,   85,  113,  113,  113,  113,
 /*  1400 */   113,  113,  113,  113,   94,   95,   96,   97,   98,   99,
 /*  1410 */   113,   82,  102,  103,   85,  113,  113,  113,  113,  113,
 /*  1420 */   113,  113,  113,   94,   95,   96,   97,   98,   99,  113,
 /*  1430 */    82,  102,  103,   85,  113,  113,  113,  113,  113,  113,
 /*  1440 */   113,  113,   94,   95,   96,   97,   98,   99,  113,   82,
 /*  1450 */   102,  103,   85,  113,  113,  113,  113,  113,   82,  113,
 /*  1460 */   113,   85,   95,   96,   97,   98,   99,  113,  113,  102,
 /*  1470 */   103,   95,   96,   97,   98,   99,  113,   82,  102,  103,
 /*  1480 */    85,  113,  113,  113,  113,  113,  113,   82,  113,  113,
 /*  1490 */    85,   96,   97,   98,   99,  113,   82,  102,  103,   85,
 /*  1500 */   113,   96,   97,   98,   99,  113,   82,  102,  103,   85,
 /*  1510 */    96,   97,   98,   99,  113,   82,  102,  103,   85,  113,
 /*  1520 */    96,   97,   98,   99,  113,   82,  102,  103,   85,   96,
 /*  1530 */    97,   98,   99,  113,   82,  102,  103,   85,  113,   96,
 /*  1540 */    97,   98,   99,  113,   82,  102,  103,   85,   96,   97,
 /*  1550 */    98,   99,  113,   82,  102,  103,   85,  113,   96,   97,
 /*  1560 */    98,   99,  113,   82,  102,  103,   85,   96,   97,   98,
 /*  1570 */    99,  113,  113,  102,  103,  113,  113,   96,   97,   98,
 /*  1580 */    99,  113,   82,  102,  103,   85,  113,  113,  113,  113,
 /*  1590 */    82,  113,  113,   85,  113,  113,   96,   97,   98,   99,
 /*  1600 */   113,  113,  102,  103,   96,   97,   98,   99,  113,   82,
 /*  1610 */   102,  103,   85,  113,  113,  113,  113,   82,  113,  113,
 /*  1620 */    85,  113,  113,   96,   97,   98,   99,  113,  113,  102,
 /*  1630 */   103,   96,   97,   98,   99,  113,  113,  102,  103,
};
#define YY_SHIFT_USE_DFLT (-47)
#define YY_SHIFT_COUNT (140)
#define YY_SHIFT_MIN   (-46)
#define YY_SHIFT_MAX   (1085)
static const short yy_shift_ofst[] = {
 /*     0 */    -1,   91,  285,  431,  459,  285,  459,  459,  459,  459,
 /*    10 */   220,  155,  459,  459,  459,  459,  459,  459,  459,  459,
 /*    20 */   459,  459,  459,  459,  459,  459,  459,  459,  459,  459,
 /*    30 */   459,  459,  459,  459,  459,  459,  459,  459,  459,  459,
 /*    40 */   459,  459,  459,  459,  459,  459,  459,  459,  459,  459,
 /*    50 */   459,  459,  459,  459,  459,  459,  459,  459,  459,  459,
 /*    60 */   459,  459,  459,  459,  459,  459,  459,  459,  459,  459,
 /*    70 */  1085,  264,   35,  193,   65,  264,  191,  404,   35,   35,
 /*    80 */    35,   35,   35,  195,  -47,  286,  286,  286, 1018,  -46,
 /*    90 */   -46,  -46,  -46,  -46,  -46,  -46,  -46,  -46,  -46,  -46,
 /*   100 */   -46,   79,  -46,   79,  -46,   79,  -46,  208,  254,  253,
 /*   110 */   102,   85,  132,   87,   87,   39,   87,   97,    2,  110,
 /*   120 */    87,   97,  102,  136,   40,   34,  197,  218,  168,  111,
 /*   130 */   107,  200,  177,  109,  115,  107,  109,  115,  107,   21,
 /*   140 */    34,
};
#define YY_REDUCE_USE_DFLT (-71)
#define YY_REDUCE_COUNT (84)
#define YY_REDUCE_MIN   (-70)
#define YY_REDUCE_MAX   (1535)
static const short yy_reduce_ofst[] = {
 /*     0 */   -63,   -7,  153,   88,   25,  365,  337,  300,  278,  215,
 /*    10 */   846,  824,  802,  780,  758,  736,  714,  692,  670,  648,
 /*    20 */   626,  604,  582,  560,  538,  516,  492,  470,  448,  865,
 /*    30 */   906,  884,  928,  963,  987, 1025, 1006, 1291, 1272, 1253,
 /*    40 */  1234, 1215, 1196, 1177, 1158, 1139, 1120, 1101, 1082, 1063,
 /*    50 */  1044, 1348, 1329, 1310, 1376, 1367, 1535, 1527, 1508, 1500,
 /*    60 */  1481, 1471, 1462, 1452, 1443, 1433, 1424, 1414, 1405, 1395,
 /*    70 */   -65,   14,  -70,  154,  128,   77,  235,  235,  234,  213,
 /*    80 */   194,  192,  189,  165,  159,
};
static const YYACTIONTYPE yy_default[] = {
 /*     0 */   357,  357,  345,  357,  335,  357,  342,  357,  357,  357,
 /*    10 */   357,  357,  357,  357,  357,  357,  357,  357,  357,  357,
 /*    20 */   357,  357,  357,  357,  357,  357,  357,  357,  357,  357,
 /*    30 */   357,  357,  357,  357,  357,  357,  357,  357,  357,  357,
 /*    40 */   357,  357,  357,  357,  357,  357,  357,  357,  357,  357,
 /*    50 */   357,  357,  357,  357,  357,  357,  357,  357,  357,  357,
 /*    60 */   357,  357,  357,  357,  357,  357,  357,  357,  357,  357,
 /*    70 */   357,  351,  357,  357,  313,  357,  357,  357,  357,  357,
 /*    80 */   357,  357,  357,  357,  335,  268,  270,  269,  309,  285,
 /*    90 */   284,  283,  282,  281,  280,  279,  278,  277,  276,  275,
 /*   100 */   271,  290,  274,  292,  273,  291,  272,  357,  357,  357,
 /*   110 */   258,  357,  357,  286,  289,  357,  288,  266,  357,  309,
 /*   120 */   287,  267,  257,  255,  357,  319,  357,  357,  357,  354,
 /*   130 */   261,  357,  357,  264,  262,  259,  265,  263,  260,  357,
 /*   140 */   357,  352,  356,  355,  353,  346,  350,  349,  348,  347,
 /*   150 */   235,  233,  239,  254,  253,  252,  251,  250,  249,  248,
 /*   160 */   247,  246,  245,  244,  343,  344,  341,  340,  331,  329,
 /*   170 */   328,  333,  327,  338,  337,  336,  334,  332,  330,  326,
 /*   180 */   293,  325,  324,  323,  322,  321,  320,  319,  296,  318,
 /*   190 */   317,  315,  295,  339,  240,  316,  314,  312,  311,  310,
 /*   200 */   308,  307,  306,  305,  304,  303,  302,  301,  300,  299,
 /*   210 */   298,  297,  294,  256,  243,  242,  241,  238,  237,  236,
 /*   220 */   232,  229,  234,  231,  230,
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
  "LCP",           "PREFIX",        "SUFFIX",        "REGEXP",      
  "SHIFTL",        "SHIFTR",        "SHIFTRR",       "PLUS",        
  "MINUS",         "STAR",          "SLASH",         "MOD",         
  "DELETE",        "INCR",          "DECR",          "NOT",         
  "BITWISE_NOT",   "ADJUST",        "EXACT",         "PARTIAL",     
  "UNSPLIT",       "DECIMAL",       "HEX_INTEGER",   "STRING",      
  "BOOLEAN",       "NULL",          "BRACKETL",      "BRACKETR",    
  "DOT",           "NONEXISTENT_COLUMN",  "error",         "suppress_unused_variable_warning",
  "input",         "query",         "expression",    "output_columns",
  "adjuster",      "query_element",  "primary_expression",  "assignment_expression",
  "conditional_expression",  "lefthand_side_expression",  "logical_or_expression",  "logical_and_expression",
  "bitwise_or_expression",  "bitwise_xor_expression",  "bitwise_and_expression",  "equality_expression",
  "relational_expression",  "shift_expression",  "additive_expression",  "multiplicative_expression",
  "unary_expression",  "postfix_expression",  "call_expression",  "member_expression",
  "arguments",     "member_expression_part",  "object_literal",  "array_literal",
  "elision",       "element_list",  "property_name_and_value_list",  "property_name_and_value",
  "property_name",  "argument_list",  "output_column",  "adjust_expression",
  "adjust_match_expression",
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
 /*  60 */ "relational_expression ::= relational_expression REGEXP shift_expression",
 /*  61 */ "shift_expression ::= additive_expression",
 /*  62 */ "shift_expression ::= shift_expression SHIFTL additive_expression",
 /*  63 */ "shift_expression ::= shift_expression SHIFTR additive_expression",
 /*  64 */ "shift_expression ::= shift_expression SHIFTRR additive_expression",
 /*  65 */ "additive_expression ::= multiplicative_expression",
 /*  66 */ "additive_expression ::= additive_expression PLUS multiplicative_expression",
 /*  67 */ "additive_expression ::= additive_expression MINUS multiplicative_expression",
 /*  68 */ "multiplicative_expression ::= unary_expression",
 /*  69 */ "multiplicative_expression ::= multiplicative_expression STAR unary_expression",
 /*  70 */ "multiplicative_expression ::= multiplicative_expression SLASH unary_expression",
 /*  71 */ "multiplicative_expression ::= multiplicative_expression MOD unary_expression",
 /*  72 */ "unary_expression ::= postfix_expression",
 /*  73 */ "unary_expression ::= DELETE unary_expression",
 /*  74 */ "unary_expression ::= INCR unary_expression",
 /*  75 */ "unary_expression ::= DECR unary_expression",
 /*  76 */ "unary_expression ::= PLUS unary_expression",
 /*  77 */ "unary_expression ::= MINUS unary_expression",
 /*  78 */ "unary_expression ::= NOT unary_expression",
 /*  79 */ "unary_expression ::= BITWISE_NOT unary_expression",
 /*  80 */ "unary_expression ::= ADJUST unary_expression",
 /*  81 */ "unary_expression ::= EXACT unary_expression",
 /*  82 */ "unary_expression ::= PARTIAL unary_expression",
 /*  83 */ "unary_expression ::= UNSPLIT unary_expression",
 /*  84 */ "postfix_expression ::= lefthand_side_expression",
 /*  85 */ "postfix_expression ::= lefthand_side_expression INCR",
 /*  86 */ "postfix_expression ::= lefthand_side_expression DECR",
 /*  87 */ "lefthand_side_expression ::= call_expression",
 /*  88 */ "lefthand_side_expression ::= member_expression",
 /*  89 */ "call_expression ::= member_expression arguments",
 /*  90 */ "member_expression ::= primary_expression",
 /*  91 */ "member_expression ::= member_expression member_expression_part",
 /*  92 */ "primary_expression ::= object_literal",
 /*  93 */ "primary_expression ::= PARENL expression PARENR",
 /*  94 */ "primary_expression ::= IDENTIFIER",
 /*  95 */ "primary_expression ::= array_literal",
 /*  96 */ "primary_expression ::= DECIMAL",
 /*  97 */ "primary_expression ::= HEX_INTEGER",
 /*  98 */ "primary_expression ::= STRING",
 /*  99 */ "primary_expression ::= BOOLEAN",
 /* 100 */ "primary_expression ::= NULL",
 /* 101 */ "array_literal ::= BRACKETL elision BRACKETR",
 /* 102 */ "array_literal ::= BRACKETL element_list elision BRACKETR",
 /* 103 */ "array_literal ::= BRACKETL element_list BRACKETR",
 /* 104 */ "elision ::= COMMA",
 /* 105 */ "elision ::= elision COMMA",
 /* 106 */ "element_list ::= assignment_expression",
 /* 107 */ "element_list ::= elision assignment_expression",
 /* 108 */ "element_list ::= element_list elision assignment_expression",
 /* 109 */ "object_literal ::= BRACEL property_name_and_value_list BRACER",
 /* 110 */ "property_name_and_value_list ::=",
 /* 111 */ "property_name_and_value_list ::= property_name_and_value_list COMMA property_name_and_value",
 /* 112 */ "property_name_and_value ::= property_name COLON assignment_expression",
 /* 113 */ "property_name ::= IDENTIFIER|STRING|DECIMAL",
 /* 114 */ "member_expression_part ::= BRACKETL expression BRACKETR",
 /* 115 */ "member_expression_part ::= DOT IDENTIFIER",
 /* 116 */ "arguments ::= PARENL argument_list PARENR",
 /* 117 */ "argument_list ::=",
 /* 118 */ "argument_list ::= assignment_expression",
 /* 119 */ "argument_list ::= argument_list COMMA assignment_expression",
 /* 120 */ "output_columns ::=",
 /* 121 */ "output_columns ::= output_column",
 /* 122 */ "output_columns ::= output_columns COMMA output_column",
 /* 123 */ "output_column ::= STAR",
 /* 124 */ "output_column ::= NONEXISTENT_COLUMN",
 /* 125 */ "output_column ::= assignment_expression",
 /* 126 */ "adjuster ::=",
 /* 127 */ "adjuster ::= adjust_expression",
 /* 128 */ "adjuster ::= adjuster PLUS adjust_expression",
 /* 129 */ "adjust_expression ::= adjust_match_expression",
 /* 130 */ "adjust_expression ::= adjust_match_expression STAR DECIMAL",
 /* 131 */ "adjust_match_expression ::= IDENTIFIER MATCH STRING",
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
    case 75: /* suppress_unused_variable_warning */
{
#line 11 "grn_ecmascript.lemon"

  (void)efsi;

#line 884 "grn_ecmascript.c"
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
  { 76, 1 },
  { 76, 1 },
  { 76, 2 },
  { 76, 2 },
  { 77, 1 },
  { 77, 2 },
  { 77, 3 },
  { 77, 3 },
  { 77, 3 },
  { 81, 1 },
  { 81, 3 },
  { 81, 2 },
  { 81, 3 },
  { 81, 3 },
  { 81, 2 },
  { 78, 1 },
  { 78, 3 },
  { 83, 1 },
  { 83, 3 },
  { 83, 3 },
  { 83, 3 },
  { 83, 3 },
  { 83, 3 },
  { 83, 3 },
  { 83, 3 },
  { 83, 3 },
  { 83, 3 },
  { 83, 3 },
  { 83, 3 },
  { 83, 3 },
  { 84, 1 },
  { 84, 5 },
  { 86, 1 },
  { 86, 3 },
  { 87, 1 },
  { 87, 3 },
  { 87, 3 },
  { 88, 1 },
  { 88, 3 },
  { 89, 1 },
  { 89, 3 },
  { 90, 1 },
  { 90, 3 },
  { 91, 1 },
  { 91, 3 },
  { 91, 3 },
  { 92, 1 },
  { 92, 3 },
  { 92, 3 },
  { 92, 3 },
  { 92, 3 },
  { 92, 3 },
  { 92, 3 },
  { 92, 3 },
  { 92, 3 },
  { 92, 3 },
  { 92, 3 },
  { 92, 3 },
  { 92, 3 },
  { 92, 3 },
  { 92, 3 },
  { 93, 1 },
  { 93, 3 },
  { 93, 3 },
  { 93, 3 },
  { 94, 1 },
  { 94, 3 },
  { 94, 3 },
  { 95, 1 },
  { 95, 3 },
  { 95, 3 },
  { 95, 3 },
  { 96, 1 },
  { 96, 2 },
  { 96, 2 },
  { 96, 2 },
  { 96, 2 },
  { 96, 2 },
  { 96, 2 },
  { 96, 2 },
  { 96, 2 },
  { 96, 2 },
  { 96, 2 },
  { 96, 2 },
  { 97, 1 },
  { 97, 2 },
  { 97, 2 },
  { 85, 1 },
  { 85, 1 },
  { 98, 2 },
  { 99, 1 },
  { 99, 2 },
  { 82, 1 },
  { 82, 3 },
  { 82, 1 },
  { 82, 1 },
  { 82, 1 },
  { 82, 1 },
  { 82, 1 },
  { 82, 1 },
  { 82, 1 },
  { 103, 3 },
  { 103, 4 },
  { 103, 3 },
  { 104, 1 },
  { 104, 2 },
  { 105, 1 },
  { 105, 2 },
  { 105, 3 },
  { 102, 3 },
  { 106, 0 },
  { 106, 3 },
  { 107, 3 },
  { 108, 1 },
  { 101, 3 },
  { 101, 2 },
  { 100, 3 },
  { 109, 0 },
  { 109, 1 },
  { 109, 3 },
  { 79, 0 },
  { 79, 1 },
  { 79, 3 },
  { 110, 1 },
  { 110, 1 },
  { 110, 1 },
  { 80, 0 },
  { 80, 1 },
  { 80, 3 },
  { 111, 1 },
  { 111, 3 },
  { 112, 3 },
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
#line 1313 "grn_ecmascript.c"
        break;
      case 6: /* query ::= query LOGICAL_AND query_element */
      case 35: /* logical_and_expression ::= logical_and_expression LOGICAL_AND bitwise_or_expression */ yytestcase(yyruleno==35);
#line 49 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_AND, 2);
}
#line 1321 "grn_ecmascript.c"
        break;
      case 7: /* query ::= query LOGICAL_AND_NOT query_element */
      case 36: /* logical_and_expression ::= logical_and_expression LOGICAL_AND_NOT bitwise_or_expression */ yytestcase(yyruleno==36);
#line 52 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_AND_NOT, 2);
}
#line 1329 "grn_ecmascript.c"
        break;
      case 8: /* query ::= query LOGICAL_OR query_element */
      case 33: /* logical_or_expression ::= logical_or_expression LOGICAL_OR logical_and_expression */ yytestcase(yyruleno==33);
#line 55 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_OR, 2);
}
#line 1337 "grn_ecmascript.c"
        break;
      case 11: /* query_element ::= RELATIVE_OP query_element */
#line 62 "grn_ecmascript.lemon"
{
  int mode;
  GRN_INT32_POP(&efsi->mode_stack, mode);
}
#line 1345 "grn_ecmascript.c"
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
#line 1372 "grn_ecmascript.c"
        break;
      case 13: /* query_element ::= BRACEL expression BRACER */
      case 14: /* query_element ::= EVAL primary_expression */ yytestcase(yyruleno==14);
#line 89 "grn_ecmascript.lemon"
{
  efsi->flags = efsi->default_flags;
}
#line 1380 "grn_ecmascript.c"
        break;
      case 16: /* expression ::= expression COMMA assignment_expression */
#line 97 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_COMMA, 2);
}
#line 1387 "grn_ecmascript.c"
        break;
      case 18: /* assignment_expression ::= lefthand_side_expression ASSIGN assignment_expression */
#line 102 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_ASSIGN, 2);
}
#line 1394 "grn_ecmascript.c"
        break;
      case 19: /* assignment_expression ::= lefthand_side_expression STAR_ASSIGN assignment_expression */
#line 105 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_STAR_ASSIGN, 2);
}
#line 1401 "grn_ecmascript.c"
        break;
      case 20: /* assignment_expression ::= lefthand_side_expression SLASH_ASSIGN assignment_expression */
#line 108 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_SLASH_ASSIGN, 2);
}
#line 1408 "grn_ecmascript.c"
        break;
      case 21: /* assignment_expression ::= lefthand_side_expression MOD_ASSIGN assignment_expression */
#line 111 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_MOD_ASSIGN, 2);
}
#line 1415 "grn_ecmascript.c"
        break;
      case 22: /* assignment_expression ::= lefthand_side_expression PLUS_ASSIGN assignment_expression */
#line 114 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_PLUS_ASSIGN, 2);
}
#line 1422 "grn_ecmascript.c"
        break;
      case 23: /* assignment_expression ::= lefthand_side_expression MINUS_ASSIGN assignment_expression */
#line 117 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_MINUS_ASSIGN, 2);
}
#line 1429 "grn_ecmascript.c"
        break;
      case 24: /* assignment_expression ::= lefthand_side_expression SHIFTL_ASSIGN assignment_expression */
#line 120 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_SHIFTL_ASSIGN, 2);
}
#line 1436 "grn_ecmascript.c"
        break;
      case 25: /* assignment_expression ::= lefthand_side_expression SHIFTR_ASSIGN assignment_expression */
#line 123 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_SHIFTR_ASSIGN, 2);
}
#line 1443 "grn_ecmascript.c"
        break;
      case 26: /* assignment_expression ::= lefthand_side_expression SHIFTRR_ASSIGN assignment_expression */
#line 126 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_SHIFTRR_ASSIGN, 2);
}
#line 1450 "grn_ecmascript.c"
        break;
      case 27: /* assignment_expression ::= lefthand_side_expression AND_ASSIGN assignment_expression */
#line 129 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_AND_ASSIGN, 2);
}
#line 1457 "grn_ecmascript.c"
        break;
      case 28: /* assignment_expression ::= lefthand_side_expression XOR_ASSIGN assignment_expression */
#line 132 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_XOR_ASSIGN, 2);
}
#line 1464 "grn_ecmascript.c"
        break;
      case 29: /* assignment_expression ::= lefthand_side_expression OR_ASSIGN assignment_expression */
#line 135 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_OR_ASSIGN, 2);
}
#line 1471 "grn_ecmascript.c"
        break;
      case 31: /* conditional_expression ::= logical_or_expression QUESTION assignment_expression COLON assignment_expression */
#line 140 "grn_ecmascript.lemon"
{
  grn_expr *e = (grn_expr *)efsi->e;
  e->codes[yymsp[-3].minor.yy0].nargs = yymsp[-1].minor.yy0 - yymsp[-3].minor.yy0;
  e->codes[yymsp[-1].minor.yy0].nargs = e->codes_curr - yymsp[-1].minor.yy0 - 1;
}
#line 1480 "grn_ecmascript.c"
        break;
      case 38: /* bitwise_or_expression ::= bitwise_or_expression BITWISE_OR bitwise_xor_expression */
#line 160 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_BITWISE_OR, 2);
}
#line 1487 "grn_ecmascript.c"
        break;
      case 40: /* bitwise_xor_expression ::= bitwise_xor_expression BITWISE_XOR bitwise_and_expression */
#line 165 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_BITWISE_XOR, 2);
}
#line 1494 "grn_ecmascript.c"
        break;
      case 42: /* bitwise_and_expression ::= bitwise_and_expression BITWISE_AND equality_expression */
#line 170 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_BITWISE_AND, 2);
}
#line 1501 "grn_ecmascript.c"
        break;
      case 44: /* equality_expression ::= equality_expression EQUAL relational_expression */
#line 175 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_EQUAL, 2);
}
#line 1508 "grn_ecmascript.c"
        break;
      case 45: /* equality_expression ::= equality_expression NOT_EQUAL relational_expression */
#line 178 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_NOT_EQUAL, 2);
}
#line 1515 "grn_ecmascript.c"
        break;
      case 47: /* relational_expression ::= relational_expression LESS shift_expression */
#line 183 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_LESS, 2);
}
#line 1522 "grn_ecmascript.c"
        break;
      case 48: /* relational_expression ::= relational_expression GREATER shift_expression */
#line 186 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_GREATER, 2);
}
#line 1529 "grn_ecmascript.c"
        break;
      case 49: /* relational_expression ::= relational_expression LESS_EQUAL shift_expression */
#line 189 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_LESS_EQUAL, 2);
}
#line 1536 "grn_ecmascript.c"
        break;
      case 50: /* relational_expression ::= relational_expression GREATER_EQUAL shift_expression */
#line 192 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_GREATER_EQUAL, 2);
}
#line 1543 "grn_ecmascript.c"
        break;
      case 51: /* relational_expression ::= relational_expression IN shift_expression */
#line 195 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_IN, 2);
}
#line 1550 "grn_ecmascript.c"
        break;
      case 52: /* relational_expression ::= relational_expression MATCH shift_expression */
      case 131: /* adjust_match_expression ::= IDENTIFIER MATCH STRING */ yytestcase(yyruleno==131);
#line 198 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_MATCH, 2);
}
#line 1558 "grn_ecmascript.c"
        break;
      case 53: /* relational_expression ::= relational_expression NEAR shift_expression */
#line 201 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_NEAR, 2);
}
#line 1565 "grn_ecmascript.c"
        break;
      case 54: /* relational_expression ::= relational_expression NEAR2 shift_expression */
#line 204 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_NEAR2, 2);
}
#line 1572 "grn_ecmascript.c"
        break;
      case 55: /* relational_expression ::= relational_expression SIMILAR shift_expression */
#line 207 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_SIMILAR, 2);
}
#line 1579 "grn_ecmascript.c"
        break;
      case 56: /* relational_expression ::= relational_expression TERM_EXTRACT shift_expression */
#line 210 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_TERM_EXTRACT, 2);
}
#line 1586 "grn_ecmascript.c"
        break;
      case 57: /* relational_expression ::= relational_expression LCP shift_expression */
#line 213 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_LCP, 2);
}
#line 1593 "grn_ecmascript.c"
        break;
      case 58: /* relational_expression ::= relational_expression PREFIX shift_expression */
#line 216 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_PREFIX, 2);
}
#line 1600 "grn_ecmascript.c"
        break;
      case 59: /* relational_expression ::= relational_expression SUFFIX shift_expression */
#line 219 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_SUFFIX, 2);
}
#line 1607 "grn_ecmascript.c"
        break;
      case 60: /* relational_expression ::= relational_expression REGEXP shift_expression */
#line 222 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_REGEXP, 2);
}
#line 1614 "grn_ecmascript.c"
        break;
      case 62: /* shift_expression ::= shift_expression SHIFTL additive_expression */
#line 227 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_SHIFTL, 2);
}
#line 1621 "grn_ecmascript.c"
        break;
      case 63: /* shift_expression ::= shift_expression SHIFTR additive_expression */
#line 230 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_SHIFTR, 2);
}
#line 1628 "grn_ecmascript.c"
        break;
      case 64: /* shift_expression ::= shift_expression SHIFTRR additive_expression */
#line 233 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_SHIFTRR, 2);
}
#line 1635 "grn_ecmascript.c"
        break;
      case 66: /* additive_expression ::= additive_expression PLUS multiplicative_expression */
      case 128: /* adjuster ::= adjuster PLUS adjust_expression */ yytestcase(yyruleno==128);
#line 238 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_PLUS, 2);
}
#line 1643 "grn_ecmascript.c"
        break;
      case 67: /* additive_expression ::= additive_expression MINUS multiplicative_expression */
#line 241 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_MINUS, 2);
}
#line 1650 "grn_ecmascript.c"
        break;
      case 69: /* multiplicative_expression ::= multiplicative_expression STAR unary_expression */
      case 130: /* adjust_expression ::= adjust_match_expression STAR DECIMAL */ yytestcase(yyruleno==130);
#line 246 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_STAR, 2);
}
#line 1658 "grn_ecmascript.c"
        break;
      case 70: /* multiplicative_expression ::= multiplicative_expression SLASH unary_expression */
#line 249 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_SLASH, 2);
}
#line 1665 "grn_ecmascript.c"
        break;
      case 71: /* multiplicative_expression ::= multiplicative_expression MOD unary_expression */
#line 252 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_MOD, 2);
}
#line 1672 "grn_ecmascript.c"
        break;
      case 73: /* unary_expression ::= DELETE unary_expression */
#line 257 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_DELETE, 1);
}
#line 1679 "grn_ecmascript.c"
        break;
      case 74: /* unary_expression ::= INCR unary_expression */
#line 260 "grn_ecmascript.lemon"
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
#line 1700 "grn_ecmascript.c"
        break;
      case 75: /* unary_expression ::= DECR unary_expression */
#line 277 "grn_ecmascript.lemon"
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
#line 1721 "grn_ecmascript.c"
        break;
      case 76: /* unary_expression ::= PLUS unary_expression */
#line 294 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_PLUS, 1);
}
#line 1728 "grn_ecmascript.c"
        break;
      case 77: /* unary_expression ::= MINUS unary_expression */
#line 297 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_MINUS, 1);
}
#line 1735 "grn_ecmascript.c"
        break;
      case 78: /* unary_expression ::= NOT unary_expression */
#line 300 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_NOT, 1);
}
#line 1742 "grn_ecmascript.c"
        break;
      case 79: /* unary_expression ::= BITWISE_NOT unary_expression */
#line 303 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_BITWISE_NOT, 1);
}
#line 1749 "grn_ecmascript.c"
        break;
      case 80: /* unary_expression ::= ADJUST unary_expression */
#line 306 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_ADJUST, 1);
}
#line 1756 "grn_ecmascript.c"
        break;
      case 81: /* unary_expression ::= EXACT unary_expression */
#line 309 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_EXACT, 1);
}
#line 1763 "grn_ecmascript.c"
        break;
      case 82: /* unary_expression ::= PARTIAL unary_expression */
#line 312 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_PARTIAL, 1);
}
#line 1770 "grn_ecmascript.c"
        break;
      case 83: /* unary_expression ::= UNSPLIT unary_expression */
#line 315 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_UNSPLIT, 1);
}
#line 1777 "grn_ecmascript.c"
        break;
      case 85: /* postfix_expression ::= lefthand_side_expression INCR */
#line 320 "grn_ecmascript.lemon"
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
#line 1798 "grn_ecmascript.c"
        break;
      case 86: /* postfix_expression ::= lefthand_side_expression DECR */
#line 337 "grn_ecmascript.lemon"
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
#line 1819 "grn_ecmascript.c"
        break;
      case 89: /* call_expression ::= member_expression arguments */
#line 358 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_CALL, yymsp[0].minor.yy0);
}
#line 1826 "grn_ecmascript.c"
        break;
      case 114: /* member_expression_part ::= BRACKETL expression BRACKETR */
#line 394 "grn_ecmascript.lemon"
{
  grn_expr_append_op(efsi->ctx, efsi->e, GRN_OP_GET_MEMBER, 2);
}
#line 1833 "grn_ecmascript.c"
        break;
      case 116: /* arguments ::= PARENL argument_list PARENR */
#line 399 "grn_ecmascript.lemon"
{ yygotominor.yy0 = yymsp[-1].minor.yy0; }
#line 1838 "grn_ecmascript.c"
        break;
      case 117: /* argument_list ::= */
#line 400 "grn_ecmascript.lemon"
{ yygotominor.yy0 = 0; }
#line 1843 "grn_ecmascript.c"
        break;
      case 118: /* argument_list ::= assignment_expression */
#line 401 "grn_ecmascript.lemon"
{ yygotominor.yy0 = 1; }
#line 1848 "grn_ecmascript.c"
        break;
      case 119: /* argument_list ::= argument_list COMMA assignment_expression */
#line 402 "grn_ecmascript.lemon"
{ yygotominor.yy0 = yymsp[-2].minor.yy0 + 1; }
#line 1853 "grn_ecmascript.c"
        break;
      case 120: /* output_columns ::= */
#line 404 "grn_ecmascript.lemon"
{
  yygotominor.yy0 = 0;
}
#line 1860 "grn_ecmascript.c"
        break;
      case 121: /* output_columns ::= output_column */
#line 407 "grn_ecmascript.lemon"
{
  if (yymsp[0].minor.yy0) {
    yygotominor.yy0 = 0;
  } else {
    yygotominor.yy0 = 1;
  }
}
#line 1871 "grn_ecmascript.c"
        break;
      case 122: /* output_columns ::= output_columns COMMA output_column */
#line 415 "grn_ecmascript.lemon"
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
#line 1885 "grn_ecmascript.c"
        break;
      case 123: /* output_column ::= STAR */
#line 426 "grn_ecmascript.lemon"
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
#line 1926 "grn_ecmascript.c"
        break;
      case 124: /* output_column ::= NONEXISTENT_COLUMN */
#line 463 "grn_ecmascript.lemon"
{
  yygotominor.yy0 = GRN_TRUE;
}
#line 1933 "grn_ecmascript.c"
        break;
      case 125: /* output_column ::= assignment_expression */
#line 466 "grn_ecmascript.lemon"
{
  yygotominor.yy0 = GRN_FALSE;
}
#line 1940 "grn_ecmascript.c"
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
      /* (61) shift_expression ::= additive_expression */ yytestcase(yyruleno==61);
      /* (65) additive_expression ::= multiplicative_expression */ yytestcase(yyruleno==65);
      /* (68) multiplicative_expression ::= unary_expression */ yytestcase(yyruleno==68);
      /* (72) unary_expression ::= postfix_expression */ yytestcase(yyruleno==72);
      /* (84) postfix_expression ::= lefthand_side_expression */ yytestcase(yyruleno==84);
      /* (87) lefthand_side_expression ::= call_expression */ yytestcase(yyruleno==87);
      /* (88) lefthand_side_expression ::= member_expression */ yytestcase(yyruleno==88);
      /* (90) member_expression ::= primary_expression */ yytestcase(yyruleno==90);
      /* (91) member_expression ::= member_expression member_expression_part */ yytestcase(yyruleno==91);
      /* (92) primary_expression ::= object_literal */ yytestcase(yyruleno==92);
      /* (93) primary_expression ::= PARENL expression PARENR */ yytestcase(yyruleno==93);
      /* (94) primary_expression ::= IDENTIFIER */ yytestcase(yyruleno==94);
      /* (95) primary_expression ::= array_literal */ yytestcase(yyruleno==95);
      /* (96) primary_expression ::= DECIMAL */ yytestcase(yyruleno==96);
      /* (97) primary_expression ::= HEX_INTEGER */ yytestcase(yyruleno==97);
      /* (98) primary_expression ::= STRING */ yytestcase(yyruleno==98);
      /* (99) primary_expression ::= BOOLEAN */ yytestcase(yyruleno==99);
      /* (100) primary_expression ::= NULL */ yytestcase(yyruleno==100);
      /* (101) array_literal ::= BRACKETL elision BRACKETR */ yytestcase(yyruleno==101);
      /* (102) array_literal ::= BRACKETL element_list elision BRACKETR */ yytestcase(yyruleno==102);
      /* (103) array_literal ::= BRACKETL element_list BRACKETR */ yytestcase(yyruleno==103);
      /* (104) elision ::= COMMA */ yytestcase(yyruleno==104);
      /* (105) elision ::= elision COMMA */ yytestcase(yyruleno==105);
      /* (106) element_list ::= assignment_expression */ yytestcase(yyruleno==106);
      /* (107) element_list ::= elision assignment_expression */ yytestcase(yyruleno==107);
      /* (108) element_list ::= element_list elision assignment_expression */ yytestcase(yyruleno==108);
      /* (109) object_literal ::= BRACEL property_name_and_value_list BRACER */ yytestcase(yyruleno==109);
      /* (110) property_name_and_value_list ::= */ yytestcase(yyruleno==110);
      /* (111) property_name_and_value_list ::= property_name_and_value_list COMMA property_name_and_value */ yytestcase(yyruleno==111);
      /* (112) property_name_and_value ::= property_name COLON assignment_expression */ yytestcase(yyruleno==112);
      /* (113) property_name ::= IDENTIFIER|STRING|DECIMAL */ yytestcase(yyruleno==113);
      /* (115) member_expression_part ::= DOT IDENTIFIER */ yytestcase(yyruleno==115);
      /* (126) adjuster ::= */ yytestcase(yyruleno==126);
      /* (127) adjuster ::= adjust_expression */ yytestcase(yyruleno==127);
      /* (129) adjust_expression ::= adjust_match_expression */ yytestcase(yyruleno==129);
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
#line 2076 "grn_ecmascript.c"
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
