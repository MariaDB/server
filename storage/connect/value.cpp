/************* Value C++ Functions Source Code File (.CPP) *************/
/*  Name: VALUE.CPP  Version 1.9                                       */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2001-2012    */
/*                                                                     */
/*  This file contains the VALUE and derived classes family functions. */
/*  These classes contain values of different types. They are used so  */
/*  new object types can be defined and added to the processing simply */
/*  (hopefully) adding their specific functions in this file.          */
/*  First family is VALUE that represent single typed objects. It is   */
/*  used by columns (COLBLK), SELECT and FILTER (derived) objects.     */
/*  Second family is VALBLK, representing simple suballocated arrays   */
/*  of values treated sequentially by FIX, BIN and VCT tables and      */
/*  columns, as well for min/max blocks as for VCT column blocks.      */
/*  Q&A: why not using only one family ? Simple values are arrays that */
/*  have only one element and arrays could have functions for all kind */
/*  of processing. The answer is a-because historically it was simpler */
/*  to do that way, b-because of performance on single values, and c-  */
/*  to avoid too complicated classes and unuseful duplication of many  */
/*  functions used on one family only. The drawback is that for new    */
/*  types of objects, we shall have more classes to update.            */
/*  Currently the only implemented types are STRING, int, DOUBLE and  */
/*  DATE. Shortly we should add at least int VARCHAR and LONGLONG.    */
/***********************************************************************/

#ifndef __VALUE_H
#define __VALUE_H

/***********************************************************************/
/*  Include relevant MariaDB header file.                  */
/***********************************************************************/
#include "my_global.h"
#if defined(WIN32)
//#include <windows.h>
#include <sql.h>
#include <sqlext.h>
#else   // !WIN32
#include <string.h>
#include "sqlutil.h"
#endif  // !WIN32

#include <math.h>

#undef DOMAIN                           // Was defined in math.h

/***********************************************************************/
/*  Include required application header files                          */
/*  global.h    is header containing all global Plug declarations.     */
/*  plgdbsem.h  is header containing the DB applic. declarations.      */
/***********************************************************************/
#include "global.h"
#include "plgdbsem.h"
#include "preparse.h"                     // For DATPAR
//#include "value.h"
#include "valblk.h"
#define NO_FUNC                           // Already defined in ODBConn
#include "plgcnx.h"                       // For DB types

/***********************************************************************/
/*  Check macro's.                                                     */
/***********************************************************************/
#if defined(_DEBUG)
#define CheckType(V)    if (Type != V->GetType()) { \
    PGLOBAL& g = Global; \
    strcpy(g->Message, MSG(VALTYPE_NOMATCH)); \
    longjmp(g->jumper[g->jump_level], Type); }
#else
#define CheckType(V)
#endif

#define FOURYEARS    126230400    // Four years in seconds (1 leap)

/***********************************************************************/
/*  Static variables.                                                  */
/***********************************************************************/
static char *list =
  " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789/.*-‘abcdefghijklmnopqrstuv"; //wxyzñ'
//" ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789abcdefghijklmnopqrstuvwxyz.";
extern "C" int  trace;

/***********************************************************************/
/*  Initialize the DTVAL static member.                                */
/***********************************************************************/
int DTVAL::Shift = 0;

/***********************************************************************/
/*  Routines called externally.                                        */
/***********************************************************************/
bool PlugEvalLike(PGLOBAL, LPCSTR, LPCSTR, bool);
#if !defined(WIN32)
extern "C" {
PSZ strupr(PSZ s);
PSZ strlwr(PSZ s);
}
#endif   // !WIN32

/***********************************************************************/
/*  Returns the bitmap representing the conditions that must not be    */
/*  met when returning from TestValue for a given operator.            */
/*  Bit one is EQ, bit 2 is LT, and bit 3 is GT.                       */
/***********************************************************************/
BYTE OpBmp(PGLOBAL g, OPVAL opc)
  {
  BYTE bt;

  switch (opc) {
    case OP_IN:
    case OP_EQ: bt = 0x06; break;
    case OP_NE: bt = 0x01; break;
    case OP_GT: bt = 0x03; break;
    case OP_GE: bt = 0x02; break;
    case OP_LT: bt = 0x05; break;
    case OP_LE: bt = 0x04; break;
    case OP_EXIST: bt = 0x00; break;
    default:
      sprintf(g->Message, MSG(BAD_FILTER_OP), opc);
      longjmp(g->jumper[g->jump_level], 777);
    } // endswitch opc

  return bt;
  } // end of OpBmp

/***********************************************************************/
/*  GetTypeName: returns the PlugDB internal type name.                */
/***********************************************************************/
PSZ GetTypeName(int type)
  {
  PSZ name = "UNKNOWN";

  switch (type) {
    case TYPE_STRING: name = "CHAR";     break;
    case TYPE_SHORT:  name = "SMALLINT"; break;
    case TYPE_INT:    name = "INTEGER";  break;
    case TYPE_DATE:   name = "DATE";     break;
    case TYPE_FLOAT:  name = "FLOAT";    break;
    } // endswitch type

  return name;
  } // end of GetTypeName

/***********************************************************************/
/*  GetTypeSize: returns the PlugDB internal type size.                */
/***********************************************************************/
int GetTypeSize(int type, int len)
  {
  switch (type) {
    case TYPE_STRING: len = len * sizeof(char); break;
    case TYPE_SHORT:  len = sizeof(short);      break;
    case TYPE_INT:    len = sizeof(int);       break;
    case TYPE_DATE:   len = sizeof(int);       break;
    case TYPE_FLOAT:  len = sizeof(double);     break;
      break;
    default:          len = 0;
    } // endswitch type

  return len;
  } // end of GetTypeSize

/***********************************************************************/
/*  GetPLGType: returns the PlugDB type corresponding to a DB type.    */
/***********************************************************************/
int GetPLGType(int type)
  {
  int tp;

  switch (type) {
    case DB_CHAR:
    case DB_STRING: tp = TYPE_STRING; break;
    case DB_SHORT:  tp = TYPE_SHORT;  break;
    case DB_INT:    tp = TYPE_INT;   break;
    case DB_DOUBLE: tp = TYPE_FLOAT;  break;
    case DB_DATE:   tp = TYPE_DATE;   break;
    default:        tp = TYPE_ERROR;
    } // endswitch type

  return tp;
  } // end of GetPLGType

/***********************************************************************/
/*  GetDBType: returns the DB type corresponding to a PlugDB type.     */
/***********************************************************************/
int GetDBType(int type)
  {
  int tp;

  switch (type) {
    case TYPE_STRING: tp = DB_CHAR;   break;
    case TYPE_SHORT:  tp = DB_SHORT;  break;
    case TYPE_INT:    tp = DB_INT;   break;
    case TYPE_FLOAT:  tp = DB_DOUBLE; break;
    case TYPE_DATE:   tp = DB_DATE;   break;
    default:          tp = DB_ERROR;
    } // endswitch type

  return tp;
  } // end of GetPLGType

/***********************************************************************/
/*  GetSQLType: returns the SQL_TYPE corresponding to a PLG type.      */
/***********************************************************************/
short GetSQLType(int type)
  {
  short tp = SQL_TYPE_NULL;

  switch (type) {
    case TYPE_STRING: tp = SQL_CHAR;      break;
    case TYPE_SHORT:  tp = SQL_SMALLINT;  break;
    case TYPE_INT:    tp = SQL_INTEGER;   break;
    case TYPE_DATE:   tp = SQL_TIMESTAMP; break;
    case TYPE_FLOAT:  tp = SQL_DOUBLE;    break;
    } // endswitch type

  return tp;
  } // end of GetSQLType

/***********************************************************************/
/*  GetSQLCType: returns the SQL_C_TYPE corresponding to a PLG type.   */
/***********************************************************************/
int GetSQLCType(int type)
  {
  int tp = SQL_TYPE_NULL;

  switch (type) {
    case TYPE_STRING: tp = SQL_C_CHAR;      break;
    case TYPE_SHORT:  tp = SQL_C_SHORT;     break;
    case TYPE_INT:    tp = SQL_C_LONG;      break;
    case TYPE_DATE:   tp = SQL_C_TIMESTAMP; break;
    case TYPE_FLOAT:  tp = SQL_C_DOUBLE;    break;
    } // endswitch type

  return tp;
  } // end of GetSQLCType

/***********************************************************************/
/*  GetFormatType: returns the FORMAT character(s) according to type.  */
/***********************************************************************/
char *GetFormatType(int type)
  {
  char *c = "X";

  switch (type) {
    case TYPE_STRING: c = "C"; break;
    case TYPE_SHORT:  c = "S"; break;
    case TYPE_INT:    c = "N"; break;
    case TYPE_FLOAT:  c = "F"; break;
    case TYPE_DATE:   c = "D"; break;
    } // endswitch type

  return c;
  } // end of GetFormatType

/***********************************************************************/
/*  GetFormatType: returns the FORMAT type according to character.     */
/***********************************************************************/
int GetFormatType(char c)
  {
  int type = TYPE_ERROR;

  switch (c) {
    case 'C': type = TYPE_STRING; break;
    case 'S': type = TYPE_SHORT;  break;
    case 'N': type = TYPE_INT;   break;
    case 'F': type = TYPE_FLOAT;  break;
    case 'D': type = TYPE_DATE;   break;
    } // endswitch type

  return type;
  } // end of GetFormatType

/***********************************************************************/
/*  TranslateSQLType: translate a SQL Type to a PLG type.              */
/***********************************************************************/
int TranslateSQLType(int stp, int prec, int& len)
  {
  int type;

  switch (stp) {
    case SQL_CHAR:                          //    1
    case SQL_VARCHAR:                       //   12
      type = TYPE_STRING;
      break;
    case SQL_LONGVARCHAR:                   //  (-1)
      type = TYPE_STRING;
      len = min(abs(len), 128);
      break;
    case SQL_NUMERIC:                       //    2
    case SQL_DECIMAL:                       //    3
      type = (prec) ? TYPE_FLOAT : TYPE_INT;
      break;
    case SQL_INTEGER:                       //    4
      type = TYPE_INT;
      break;
    case SQL_SMALLINT:                      //    5
    case SQL_TINYINT:                       //  (-6)
    case SQL_BIT:                           //  (-7)
      type = TYPE_SHORT;
      break;
    case SQL_FLOAT:                         //    6
    case SQL_REAL:                          //    7
    case SQL_DOUBLE:                        //    8
      type = TYPE_FLOAT;
      break;
    case SQL_DATETIME:                      //    9
//  case SQL_DATE:                          //    9
      type = TYPE_DATE;
      len = 10;
      break;
    case SQL_INTERVAL:                      //   10
//  case SQL_TIME:                          //   10
      type = TYPE_STRING;
      len = 8 + ((prec) ? (prec+1) : 0);
      break;
    case SQL_TIMESTAMP:                     //   11
      type = TYPE_DATE;
      len = 19 + ((prec) ? (prec+1) : 0);
      break;
    case SQL_UNKNOWN_TYPE:                  //    0
    case SQL_BINARY:                        //  (-2)
    case SQL_VARBINARY:                     //  (-3)
    case SQL_LONGVARBINARY:                 //  (-4)
    case SQL_BIGINT:                        //  (-5)
//  case SQL_BIT:                           //  (-7)
    case SQL_GUID:                          // (-11)
    default:
      type = TYPE_ERROR;
      len = 0;
    } // endswitch type

  return type;
  } // end of TranslateSQLType

/***********************************************************************/
/*  IsTypeChar: returns true for character type(s).                    */
/***********************************************************************/
bool IsTypeChar(int type)
  {
  switch (type) {
    case TYPE_STRING:
      return true;
    } // endswitch type

  return false;
  } // end of IsTypeChar

/***********************************************************************/
/*  IsTypeNum: returns true for numeric types.                         */
/***********************************************************************/
bool IsTypeNum(int type)
  {
  switch (type) {
    case TYPE_INT:
    case TYPE_DATE:
    case TYPE_FLOAT:
    case TYPE_SHORT:
    case TYPE_NUM:
      return true;
    } // endswitch type

  return false;
  } // end of IsTypeNum

/***********************************************************************/
/*  ConvertType: what this function does is to determine the type to   */
/*  which should be converted a value so no precision would be lost.   */
/*  This can be a numeric type if num is true or non numeric if false. */
/*  Note: this is an ultra simplified version of this function that    */
/*  should become more and more complex as new types are added.        */
/*  Not evaluated types (TYPE_VOID or TYPE_UNDEF) return false from    */
/*  IsType... functions so match does not prevent correct setting.     */
/***********************************************************************/
int ConvertType(int target, int type, CONV kind, bool match)
  {
  switch (kind) {
    case CNV_CHAR:
      if (match && (!IsTypeChar(target) || !IsTypeChar(type)))
        return TYPE_ERROR;

      return TYPE_STRING;
    case CNV_NUM:
      if (match && (!IsTypeNum(target) || !IsTypeNum(type)))
        return TYPE_ERROR;

      return (target == TYPE_FLOAT || type == TYPE_FLOAT) ? TYPE_FLOAT
           : (target == TYPE_DATE  || type == TYPE_DATE)  ? TYPE_DATE
           : (target == TYPE_INT   || type == TYPE_INT)   ? TYPE_INT
                                                          : TYPE_SHORT;
    default:
      if (!target || target == type)
        return type;

      if (match && ((IsTypeChar(target) && !IsTypeChar(type)) ||
                    (IsTypeNum(target) && !IsTypeNum(type))))
        return TYPE_ERROR;

      return (target == TYPE_FLOAT  || type == TYPE_FLOAT)  ? TYPE_FLOAT
           : (target == TYPE_DATE   || type == TYPE_DATE)   ? TYPE_DATE
           : (target == TYPE_INT    || type == TYPE_INT)    ? TYPE_INT
           : (target == TYPE_SHORT  || type == TYPE_SHORT)  ? TYPE_SHORT
           : (target == TYPE_STRING || type == TYPE_STRING) ? TYPE_STRING
                                                            : TYPE_ERROR;
    } // endswitch kind

  } // end of ConvertType

/***********************************************************************/
/*  AllocateConstant: allocates a constant Value.                      */
/***********************************************************************/
PVAL AllocateValue(PGLOBAL g, void *value, short type)
  {
  PVAL valp;

	if (trace)
		htrc("AllocateConstant: value=%p type=%hd\n", value, type);

  switch (type) {
    case TYPE_STRING: valp = new(g) STRING((PSZ)value);      break;
    case TYPE_SHORT:  valp = new(g) SHVAL(*(short*)value);   break;
    case TYPE_INT:    valp = new(g) INTVAL(*(int*)value);    break;
    case TYPE_FLOAT:  valp = new(g) DFVAL(*(double *)value); break;
    default:
      sprintf(g->Message, MSG(BAD_VALUE_TYPE), type);
      return NULL;
    } // endswitch Type

  valp->SetGlobal(g);
  return valp;
  } // end of AllocateValue

/***********************************************************************/
/*  Allocate a variable Value according to type, length and precision. */
/***********************************************************************/
PVAL AllocateValue(PGLOBAL g, int type, int len, int prec,
                              PSZ dom, PCATLG cat)
  {
  PVAL valp;

  switch (type) {
    case TYPE_STRING: valp = new(g) STRING(g, (PSZ)NULL, len, prec);
			break;
    case TYPE_DATE:   valp = new(g) DTVAL(g, len, prec, dom);  break;
    case TYPE_INT:    valp = new(g) INTVAL((int)0);            break;
    case TYPE_SHORT:  valp = new(g) SHVAL((short)0);           break;
    case TYPE_FLOAT:  valp = new(g) DFVAL(0.0, prec);          break;
    default:
      sprintf(g->Message, MSG(BAD_VALUE_TYPE), type);
      return NULL;
    } // endswitch type

  valp->SetGlobal(g);
  return valp;
  } // end of AllocateValue

/***********************************************************************/
/*  Allocate a constant Value converted to newtype.                    */
/*  Can also be used to copy a Value eventually converted.             */
/***********************************************************************/
PVAL AllocateValue(PGLOBAL g, PVAL valp, int newtype)
  {
  PSZ p, sp;

  if (newtype == TYPE_VOID)  // Means allocate a value of the same type
    newtype = valp->GetType();

  switch (newtype) {
    case TYPE_STRING:
      p = (PSZ)PlugSubAlloc(g, NULL, 1 + valp->GetValLen());

      if ((sp = valp->GetCharString(p)) != p)
        strcpy (p, sp);

      valp = new(g) STRING(g, p, valp->GetValLen(), valp->GetValPrec());
      break;
    case TYPE_SHORT: valp = new(g) SHVAL(valp->GetShortValue());   break;
    case TYPE_INT:   valp = new(g) INTVAL(valp->GetIntValue());    break;
    case TYPE_DATE:  valp = new(g) DTVAL(g, valp->GetIntValue());  break;
    case TYPE_FLOAT: valp = new(g) DFVAL(valp->GetFloatValue());   break;
    default:
      sprintf(g->Message, MSG(BAD_VALUE_TYPE), newtype);
      return NULL;
    } // endswitch type

  valp->SetGlobal(g);
  return valp;
  } // end of AllocateValue


/* -------------------------- Class VALUE ---------------------------- */

/***********************************************************************/
/*  ShowTypedValue: send back the value formatted according to parms.  */
/*  buf: is a pointer to a buffer large enough for big double values.  */
/*  typ: is the type wanted for the value character representation.    */
/*    n: is the field length (needed for right justification.          */
/*    p: is the precision (for float representations).                 */
/*  Note: this fonction is currently not used anymore.                 */
/***********************************************************************/
char *VALUE::ShowTypedValue(PGLOBAL g, char *buf, int typ, int n, int p)
  {
  switch (typ) {
    case TYPE_STRING:
      buf = GetCharString(buf);
      break;
    case TYPE_INT:
    case TYPE_DATE:
      buf = GetIntString(buf, n);
      break;
    case TYPE_FLOAT:
      buf = GetFloatString(buf, n, p);
      break;
    case TYPE_SHORT:
      buf = GetShortString(buf, n);
      break;
    default:
      // More should be added for additional values.
			if (trace)
				htrc("Invalid col format type %d\n", typ);

      sprintf(g->Message, MSG(BAD_COL_FORMAT), typ);
      longjmp(g->jumper[g->jump_level], 31);
    } // endswitch Type

  return buf;
  } // end of ShowTypedValue

/***********************************************************************/
/*  Returns a BYTE indicating the comparison between two values.       */
/*  Bit 1 indicates equality, Bit 2 less than, and Bit3 greater than.  */
/*  More than 1 bit can be set only in the case of TYPE_LIST.          */
/***********************************************************************/
BYTE VALUE::TestValue(PVAL vp)
  {
  int n = CompareValue(vp);

  return (n > 0) ? 0x04 : (n < 0) ? 0x02 : 0x01;
  } // end of TestValue

/* -------------------------- Class STRING --------------------------- */

/***********************************************************************/
/*  STRING public constructor from a constant string.                  */
/***********************************************************************/
STRING::STRING(PSZ s) : VALUE(TYPE_STRING)
  {
  Strp = s;
  Len = strlen(s);
  Clen = Len;
	Ci = false;
  } // end of STRING constructor

/***********************************************************************/
/*  STRING public constructor from char.                               */
/***********************************************************************/
STRING::STRING(PGLOBAL g, PSZ s, int n, int c) : VALUE(TYPE_STRING)
  {
  Len = n;

  if (!s) {
    Strp = (char *)PlugSubAlloc(g, NULL, Len + 1);
    Strp[Len] = '\0';
  } else
    Strp = s;

  Clen = Len;
	Ci = (c != 0);
  } // end of STRING constructor

/***********************************************************************/
/*  STRING public constructor from short.                              */
/***********************************************************************/
STRING::STRING(PGLOBAL g, short i) : VALUE(TYPE_STRING)
  {
  Strp = (char *)PlugSubAlloc(g, NULL, 12);
  Len = sprintf(Strp, "%hd", i);
  Clen = Len;
	Ci = false;
  } // end of STRING constructor

/***********************************************************************/
/*  STRING public constructor from int.                               */
/***********************************************************************/
STRING::STRING(PGLOBAL g, int n) : VALUE(TYPE_STRING)
  {
  Strp = (char *)PlugSubAlloc(g, NULL, 12);
  Len = sprintf(Strp, "%d", n);
  Clen = Len;
	Ci = false;
  } // end of STRING constructor

/***********************************************************************/
/*  STRING public constructor from double.                             */
/***********************************************************************/
STRING::STRING(PGLOBAL g, double f) : VALUE(TYPE_STRING)
  {
  Strp = (char *)PlugSubAlloc(g, NULL, 32);
  Len = sprintf(Strp, "%lf", f);
  Clen = Len;
	Ci = false;
  } // end of STRING constructor

/***********************************************************************/
/*  STRING SetValue: copy the value of another Value object.           */
/***********************************************************************/
bool STRING::SetValue_pval(PVAL valp, bool chktype)
  {
  if (chktype && (valp->GetType() != Type || valp->GetSize() > Len))
    return true;

  char buf[32];

  strncpy(Strp, valp->GetCharString(buf), Len);
  return false;
  } // end of SetValue_pval

/***********************************************************************/
/*  STRING SetValue: fill string with chars extracted from a line.     */
/***********************************************************************/
void STRING::SetValue_char(char *p, int n)
  {
  n = min(n, Len);
  strncpy(Strp, p, n);

  for (p = Strp + n - 1; (*p == ' ' || *p == '\0') && p >= Strp; p--) ;

  *(++p) = '\0';

	if (trace)
		htrc(" Setting string to: '%s'\n", Strp);

  } // end of SetValue_char

/***********************************************************************/
/*  STRING SetValue: fill string with another string.                  */
/***********************************************************************/
void STRING::SetValue_psz(PSZ s)
  {
  strncpy(Strp, s, Len);
  } // end of SetValue_psz

/***********************************************************************/
/*  STRING SetValue: fill string with a string extracted from a block. */
/***********************************************************************/
void STRING::SetValue_pvblk(PVBLK blk, int n)
  {
  strncpy(Strp, blk->GetCharValue(n), Len);
  } // end of SetValue_pvblk

/***********************************************************************/
/*  STRING SetValue: get the character representation of an integer.   */
/***********************************************************************/
void STRING::SetValue(short n)
  {
  SetValue((int)n);
  } // end of SetValue

/***********************************************************************/
/*  STRING SetValue: get the character representation of an integer.   */
/***********************************************************************/
void STRING::SetValue(int n)
  {
  char     buf[16];
  PGLOBAL& g = Global;
  int      k = sprintf(buf, "%d", n);

  if (k > Len) {
    sprintf(g->Message, MSG(VALSTR_TOO_LONG), buf, Len);
    longjmp(g->jumper[g->jump_level], 138);
  } else
    SetValue_psz(buf);

  } // end of SetValue

/***********************************************************************/
/*  STRING SetValue: get the character representation of a double.     */
/***********************************************************************/
void STRING::SetValue(double f)
  {
  char    *p, buf[32];
  PGLOBAL& g = Global;
  int      k = sprintf(buf, "%lf", f);

  for (p = buf + k - 1; p >= buf; p--)
    if (*p == '0') {
      *p = 0;
      k--;
    } else
      break;

  if (k > Len) {
    sprintf(g->Message, MSG(VALSTR_TOO_LONG), buf, Len);
    longjmp(g->jumper[g->jump_level], 138);
  } else
    SetValue_psz(buf);

  } // end of SetValue

/***********************************************************************/
/*  STRING SetBinValue: fill string with chars extracted from a line.  */
/***********************************************************************/
void STRING::SetBinValue(void *p)
  {
  SetValue_char((char *)p, Len);
  } // end of SetBinValue

/***********************************************************************/
/*  GetBinValue: fill a buffer with the internal binary value.         */
/*  This function checks whether the buffer length is enough and       */
/*  returns true if not. Actual filling occurs only if go is true.     */
/*  Currently used by WriteColumn of binary files.                     */
/***********************************************************************/
bool STRING::GetBinValue(void *buf, int buflen, bool go)
  {
  int len = strlen(Strp);

  if (len > buflen)
    return true;
  else if (go) {
    memset(buf, ' ', buflen);
    memcpy(buf, Strp, len);
    } // endif go

  return false;
  } // end of GetBinValue

/***********************************************************************/
/*  GetBinValue: used by SELECT when called from QUERY and KINDEX.     */
/*  This is a fast implementation that does not do any checking.       */
/***********************************************************************/
void STRING::GetBinValue(void *buf, int buflen)
  {
  assert(buflen >= (signed)strlen(Strp));

  memset(buf, ' ', buflen);
  memcpy(buf, Strp, buflen);
  } // end of GetBinValue

/***********************************************************************/
/*  STRING ShowValue: get string representation of a char value.       */
/***********************************************************************/
char *STRING::ShowValue(char *buf, int len)
  {
  return Strp;
  } // end of ShowValue

/***********************************************************************/
/*  STRING GetCharString: get string representation of a char value.   */
/***********************************************************************/
char *STRING::GetCharString(char *p)
  {
  return Strp;
  } // end of GetCharString

/***********************************************************************/
/*  STRING GetShortString: get short representation of a char value.   */
/***********************************************************************/
char *STRING::GetShortString(char *p, int n)
  {
  sprintf(p, "%*hd", n, (short)atoi(Strp));
  return p;
  } // end of GetShortString

/***********************************************************************/
/*  STRING GetIntString: get int representation of a char value.     */
/***********************************************************************/
char *STRING::GetIntString(char *p, int n)
  {
  sprintf(p, "%*ld", n, atol(Strp));
  return p;
  } // end of GetIntString

/***********************************************************************/
/*  STRING GetFloatString: get double representation of a char value.  */
/***********************************************************************/
char *STRING::GetFloatString(char *p, int n, int prec)
  {
  sprintf(p, "%*.*lf", n, (prec < 0) ? 2 : prec, atof(Strp));
  return p;
  } // end of GetFloatString

/***********************************************************************/
/*  STRING compare value with another Value.                           */
/***********************************************************************/
bool STRING::IsEqual(PVAL vp, bool chktype)
  {
  if (this == vp)
    return true;
  else if (chktype && Type != vp->GetType())
    return false;
  else if (Ci || vp->IsCi())
    return !stricmp(Strp, vp->GetCharValue());
  else // (!Ci)
    return !strcmp(Strp, vp->GetCharValue());

  } // end of IsEqual

/***********************************************************************/
/*  Compare values and returns 1, 0 or -1 according to comparison.     */
/*  This function is used for evaluation of character filters.         */
/***********************************************************************/
int STRING::CompareValue(PVAL vp)
  {
  int n;
//assert(vp->GetType() == Type);

	if (trace)
		htrc(" Comparing: val='%s','%s'\n", Strp, vp->GetCharValue());

  // Process filtering on character strings.
  if (Ci || vp->IsCi())
	  n = stricmp(Strp, vp->GetCharValue());
	else
	  n = strcmp(Strp, vp->GetCharValue());

#if defined(WIN32)
  if (n == _NLSCMPERROR)
    return n;                        // Here we should raise an error
#endif   // WIN32

  return (n > 0) ? 1 : (n < 0) ? -1 : 0;
  } // end of CompareValue

/***********************************************************************/
/*  Returns a BYTE indicating the comparison between two values.       */
/*  Bit 1 indicates equality, Bit 2 less than, and Bit3 greater than.  */
/*  More than 1 bit are set only in the case of error.                 */
/***********************************************************************/
BYTE STRING::TestValue(PVAL vp)
  {
  // Process filtering on character strings.
	bool ci = (Ci || vp->IsCi());
  int  n = (ci) ? stricmp(Strp, vp->GetCharValue())
								: strcmp(Strp, vp->GetCharValue());

#if defined(WIN32)
  if (n == _NLSCMPERROR)
    return 0x07;                     // Here we could raise an error
#endif   // WIN32

  return (n > 0) ? 0x04 : (n < 0) ? 0x02 : 0x01;
  } // end of TestValue

/***********************************************************************/
/*  Compute a function on a string.                                    */
/***********************************************************************/
bool STRING::Compute(PGLOBAL g, PVAL *vp, int np, OPVAL op)
  {
  assert(np <= 3);

  if (op == OP_SUBST) {
    /*******************************************************************/
    /*  SUBSTR: this functions have 1 STRING parameter followed by     */
    /*          1 or 2 int parameters.                                */
    /*******************************************************************/
    char *p, *s, buf[32];
    int   i, n, len;

    assert(np >= 2);

    s = vp[0]->GetCharString(buf);
    i = (int)vp[1]->GetIntValue();          // Starting point
    n = (np > 2) ? (int)vp[2]->GetIntValue(): 0;
    len = strlen(s);
    *Strp = '\0';

    if (i > len || i < -len || i == 0 || n < 0)
      p = NULL;
    else if (i > 0)
      p = s + i - 1;
    else
      p = s + len + i;

    if (p) {
      /******************************************************************/
      /*  This should not happen if the result size has been set        */
      /*  accurately, and this test could be placed under trace.        */
      /******************************************************************/
      if (((n > 0) ? min(n, (signed)strlen(p)) : (signed)strlen(p)) > Len) {
        strcpy(g->Message, MSG(SUB_RES_TOO_LNG));
        return true;
        } // endif

      /******************************************************************/
      /*  Do the actual Substr operation.                               */
      /******************************************************************/
      if (n > 0)
        strncat(Strp, p, n);
      else
        strcpy(Strp, p);

      } // endif p

		if (trace)
			htrc("SUBSTR result=%s val=%s,%d,%d", Strp, s, i, n);

  } else if (op == OP_LTRIM || op == OP_RTRIM) {
    /*******************************************************************/
    /*  Trimming functions have one STRING parameter followed by one   */
    /*          CHAR parameter (one chararacter).                      */
    /*******************************************************************/
    char *p, buf[32], c = ' ';
    PSZ   strg;
    int   len;

    assert(np > 0);

    strg = vp[0]->GetCharString(buf);
    len = strlen(strg);
    strg = strcpy(Strp, strg);

    if (len > 0) {
      if (np > 1) {
        // Character value may have been entered as an integer
        if (vp[1]->GetType() == TYPE_INT)
          c = (char)vp[1]->GetIntValue();
        else if (IsTypeChar(vp[1]->GetType()))
          c = *vp[1]->GetCharValue();
        else {
          strcpy(g->Message, MSG(BAD_TRIM_ARGTYP));
          return true;
          } // endelse

        } // endif 2 args

      if (op == OP_LTRIM) {
        for (p = strg; *p == c; p++) ;

        if (p != strg)
          do {
            *(strg++) = *p;
            } while (*(p++)); /* enddo */

      } else // OP_RTRIM:
        for (p = strg + len - 1; *p == c && p >= strg; p--)
          *p = '\0';

      } // endif len

  } else if (op == OP_LPAD  || op == OP_RPAD  ||
             op == OP_LJUST || op == OP_RJUST || op == OP_CJUST) {
    /*******************************************************************/
    /*  Pad and justify functions have 3 arguments char, NUM and C.    */
    /*******************************************************************/
    PSZ  strg;
    int  i, n1, n2, len;
    int n = 0;
    char buf[32], c = ' ';

    assert(np > 0);

    strg = vp[0]->GetCharString(buf);
    len = strlen(strg);
    strg = strcpy(Strp, strg);

    if (np > 1) {
      n = vp[1]->GetIntValue();

      if (n > Len) {
        sprintf(g->Message, MSG(OP_RES_TOO_LONG), op);
        return true;
        } // endif

      if (np > 2) {
        // Character value may have been entered as an integer
        if (vp[2]->GetType() == TYPE_INT)
          c = (char)vp[2]->GetIntValue();
        else if (IsTypeChar(vp[2]->GetType()))
          c = *vp[2]->GetCharValue();
        else {
          strcpy(g->Message, MSG(BAD_PAD_ARGTYP));
          return true;
          } // endelse

        } // endif 3 args

      } // endif 2 args

    if (n == 0)
      n = Len;

    if ((n = (n - (int)len)) > 0) {
      switch (op) {
        case OP_RPAD:
        case OP_LJUST:
          n1 = 0;
          n2 = (int)n;
          break;
        case OP_LPAD:
        case OP_RJUST:
          n1 = (int)n;
          n2 = 0;
          break;
        case OP_CJUST:
          n1 = (int)n / 2;
          n2 = (int)n - n1;
          break;
        default:
          sprintf(g->Message, MSG(INVALID_OPER), op, "Compute");
          return true;
        } // endswitch op

      if (n1 > 0) {
        for (i = len; i >= 0; i--)
          *(strg + i + n1) = *(strg + i);

        for (i = 0; i < n1; i++)
          *(strg + i) = c;

        len += n1;
        } // endif n1

      if (n2 > 0) {
        for (i = len; i < len + n2; i++)
          *(strg + i) = c;

        *(strg + len + n2) = '\0';
        } // endif n2

      } // endif n

		if (trace)
			htrc(" function result=%s\n", strg);

  } else if (op == OP_SNDX) {
    /*******************************************************************/
    /*  SOUNDEX function: one string argument.                         */
    /*  In addition to Knuth standard algorithm, we accept and ignore  */
    /*  all non alpha characters.                                      */
    /*******************************************************************/
    static int t[27] =
              {0,1,2,3,0,1,2,0,0,2,2,4,5,5,0,1,2,6,2,3,0,1,0,2,0,2,0};
    //         A B C D E F G H I J K L M N O P Q R S T U V W X Y Z [
    char *p, s[65];
    int   i, n;
    bool  b = false;

    assert(np == 1);

    p = vp[0]->GetCharValue();

    for (i = 0; i < 64; p++)
      if (isalpha(*p)) {
        s[i++] = toupper(*p);
        b = true;
      } else if (!*p)
        break;
      else
        s[i++] = 'Z' + 1;

    if (b) {
      s[i] = '\0';
      Strp[0] = *s;
    } else {
      strcpy(Strp, "    ");         // Null string
      return false;
    } // endif i

    for (i = 1, p = s + 1; *p && i < 4; p++)
      if ((n = t[*p - 'A'])) {
        Strp[i] = '0' + n;

        if (!b || Strp[i] != Strp[i - 1]) {
          b = true;
          i++;
          } // endif dup

      } else
        b = false;

    for (; i < 4; i++)
      Strp[i] = '0';

//  Strp[4] = '\0';
  } else {
    /*******************************************************************/
    /*  All other functions have STRING parameter(s).                  */
    /*******************************************************************/
    char *p[3], val[3][32];
    int   i;

    for (i = 0; i < np; i++)
      p[i] = vp[i]->GetCharString(val[i]);

    switch (op) {
      case OP_LOWER:
        assert(np == 1);
        strlwr(strcpy(Strp, p[0]));
        break;
      case OP_UPPER:
        assert(np == 1);
        strupr(strcpy(Strp, p[0]));
        break;
      case OP_CNC:
        assert(np == 2);
        strncat(strncpy(Strp, p[0], Len), p[1], Len);
        break;
      case OP_MIN:
        assert(np == 2);
        strcpy(Strp, (strcmp(p[0], p[1]) < 0) ? p[0] : p[1]);
        break;
      case OP_MAX:
        assert(np == 2);
        strcpy(Strp, (strcmp(p[0], p[1]) > 0) ? p[0] : p[1]);
        break;
      case OP_REPL:
       {char *pp;
        int   i, len;

        if (np == 2) {
          p[2] = "";
          np = 3;
        } else
          assert(np == 3);

        if ((len = strlen(p[1]))) {
          *Strp = '\0';

          do {
            if ((pp = strstr(p[0], p[1]))) {
              i = strlen(Strp) + (pp - p[0]) + strlen(p[2]);

              if (i > Len) {
								if (trace)
									htrc(" error len=%d R_Length=%d\n", i, Len);

                sprintf(g->Message, MSG(OP_RES_TOO_LONG), op);
                return true;
                } // endif

              strncat(Strp, p[0], pp - p[0]);
              strcat(Strp, p[2]);
              p[0] = pp + len;
            } else
              strcat(Strp, p[0]);

            } while (pp); // enddo

        } else
          strcpy(Strp, p[0]);

       }break;
      case OP_TRANSL:
       {unsigned char *p0, *p1, *p2, cp[256];
        unsigned int   k, n = strlen(p[1]);

        assert(np == 3 && n == strlen(p[2]));

        p0 = (unsigned char *)p[0];
        p1 = (unsigned char *)p[1];
        p2 = (unsigned char *)p[2];

        for (k = 0; k < 256; k++)
          cp[k] = k;

        for (k = 0; k < n; k++)
          cp[p1[k]] = p2[k];

        for (k = 0; k < strlen(p[0]); k++)
          Strp[k] = cp[p0[k]];

        Strp[k] = 0;
       }break;
      case OP_FDISK:
      case OP_FPATH:
      case OP_FNAME:
      case OP_FTYPE:
//      if (!ExtractFromPath(g, Strp, p[0], op))
//        return true;

//      break;
      default:
        sprintf(g->Message, MSG(BAD_EXP_OPER), op);
        return true;
      } // endswitch op

		if (trace) {
			htrc("Compute result=%s val=%s", Strp, p[0]);

			for (i = 1; i < np; i++)
				htrc(",%s", p[i]);

			htrc(" op=%d\n", op);
			} // endif trace

  } // endif op

  return false;
  } // end of Compute

/***********************************************************************/
/*  GetTime: extract the time from a string of format hh:mm:ss         */
/***********************************************************************/
int STRING::GetTime(PGLOBAL g, PVAL *vp, int np)
  {
  int hh, mm, ss;

  hh = mm = ss = 0;
  sscanf(Strp, " %d : %d : %d", &hh, &mm, &ss);
  return ((hh * 3600) + (mm * 60) + ss);
  } // end of GetTime

/***********************************************************************/
/*  FormatValue: This function set vp (a STRING value) to the string   */
/*  constructed from its own value formated using the fmt format.      */
/*  This function assumes that the format matches the value type.      */
/***********************************************************************/
bool STRING::FormatValue(PVAL vp, char *fmt)
  {
  char *buf = (char*)vp->GetTo_Val();        // Should be big enough
  int   n = sprintf(buf, fmt, Strp);

  return (n > vp->GetValLen());
  } // end of FormatValue

/***********************************************************************/
/*  SetMin: used by the aggregate function MIN.                        */
/***********************************************************************/
void STRING::SetMin(PVAL vp)
  {
  char *val = vp->GetCharValue();

  assert(strlen(val) <= (unsigned)Len);

  if (((Ci) ? stricmp(val, Strp) : strcmp(val, Strp)) < 0)
    strcpy(Strp, val);

  } // end of SetMin

/***********************************************************************/
/*  SetMin: used by QUERY for the aggregate function MIN.              */
/***********************************************************************/
void STRING::SetMin(PVBLK vbp, int i)
  {
  char *val = vbp->GetCharValue(i);

  if (((Ci) ? stricmp(val, Strp) : strcmp(val, Strp)) < 0)
    strcpy(Strp, val);

  } // end of SetMin

/***********************************************************************/
/*  SetMin: used by QUERY for the aggregate function MIN.              */
/***********************************************************************/
void STRING::SetMin(PVBLK vbp, int j, int k)
  {
  char *val;

  for (register int i = j; i < k; i++) {
    val = vbp->GetCharValue(i);

    if (((Ci) ? stricmp(val, Strp) : strcmp(val, Strp)) < 0)
      strcpy(Strp, val);

    } // endfor i

  } // end of SetMin

/***********************************************************************/
/*  SetMin: used by QUERY for the aggregate function MIN.              */
/***********************************************************************/
void STRING::SetMin(PVBLK vbp, int *x, int j, int k)
  {
  char *val;

  for (register int i = j; i < k; i++) {
    val = vbp->GetCharValue(x[i]);

    if (((Ci) ? stricmp(val, Strp) : strcmp(val, Strp)) < 0)
      strcpy(Strp, val);

    } // endfor i

  } // end of SetMin

/***********************************************************************/
/*  SetMax: used by the aggregate function MAX.                        */
/***********************************************************************/
void STRING::SetMax(PVAL vp)
  {
  char *val = vp->GetCharValue();

  assert(strlen(val) <= (unsigned)Len);

  if (((Ci) ? stricmp(val, Strp) : strcmp(val, Strp)) > 0)
    strcpy(Strp, val);

  } // end of SetMax

/***********************************************************************/
/*  SetMin: used by QUERY for the aggregate function MIN.              */
/***********************************************************************/
void STRING::SetMax(PVBLK vbp, int i)
  {
  char *val = vbp->GetCharValue(i);

  if (((Ci) ? stricmp(val, Strp) : strcmp(val, Strp)) > 0)
    strcpy(Strp, val);

  } // end of SetMin

/***********************************************************************/
/*  SetMin: used by QUERY for the aggregate function MIN.              */
/***********************************************************************/
void STRING::SetMax(PVBLK vbp, int j, int k)
  {
  char *val;

  for (register int i = j; i < k; i++) {
    val = vbp->GetCharValue(i);

	  if (((Ci) ? stricmp(val, Strp) : strcmp(val, Strp)) > 0)
      strcpy(Strp, val);

    } // endfor i

  } // end of SetMin

/***********************************************************************/
/*  SetMin: used by QUERY for the aggregate function MIN.              */
/***********************************************************************/
void STRING::SetMax(PVBLK vbp, int *x, int j, int k)
  {
  char *val;

  for (register int i = j; i < k; i++) {
    val = vbp->GetCharValue(x[i]);

	  if (((Ci) ? stricmp(val, Strp) : strcmp(val, Strp)) > 0)
      strcpy(Strp, val);

    } // endfor i

  } // end of SetMin

/***********************************************************************/
/*  STRING SetFormat function (used to set SELECT output format).      */
/***********************************************************************/
bool STRING::SetConstFormat(PGLOBAL g, FORMAT& fmt)
  {
  fmt.Type[0] = 'C';
  fmt.Length = Len;
  fmt.Prec = 0;
  return false;
  } // end of SetConstFormat

/***********************************************************************/
/*  Make file output of a STRING object.                               */
/***********************************************************************/
void STRING::Print(PGLOBAL g, FILE *f, uint n)
  {
  char m[64];

  memset(m, ' ', n);                             /* Make margin string */
  m[n] = '\0';

  fprintf(f, "%s%s\n", m, Strp);
  } // end of Print

/***********************************************************************/
/*  Make string output of a STRING object.                             */
/***********************************************************************/
void STRING::Print(PGLOBAL g, char *ps, uint z)
  {
  sprintf(ps, "'%.*s'", z-3, Strp);
  } // end of Print

/* -------------------------- Class SHVAL ---------------------------- */

/***********************************************************************/
/*  SHVAL  public constructor from char.                               */
/***********************************************************************/
SHVAL::SHVAL(PSZ s) : VALUE(TYPE_SHORT)
  {
  Sval = atoi(s);
  Clen = sizeof(short);
  } // end of SHVAL constructor

/***********************************************************************/
/*  SHVAL  public constructor from short.                              */
/***********************************************************************/
SHVAL::SHVAL(short i) : VALUE(TYPE_SHORT)
  {
  Sval = i;
  Clen = sizeof(short);
  } // end of SHVAL constructor

/***********************************************************************/
/*  SHVAL  public constructor from int.                               */
/***********************************************************************/
SHVAL::SHVAL(int n) : VALUE(TYPE_SHORT)
  {
  Sval = (short)n;
  Clen = sizeof(short);
  } // end of SHVAL constructor

/***********************************************************************/
/*  SHVAL  public constructor from double.                             */
/***********************************************************************/
SHVAL::SHVAL(double f) : VALUE(TYPE_SHORT)
  {
  Sval = (short)f;
  Clen = sizeof(short);
  } // end of SHVAL constructor

/***********************************************************************/
/*  SHVAL GetValLen: returns the print length of the short object.     */
/***********************************************************************/
int SHVAL::GetValLen(void)
  {
  char c[16];

  return sprintf(c, "%hd", Sval);
  } // end of GetValLen

/***********************************************************************/
/*  SHVAL SetValue: copy the value of another Value object.            */
/*  This function allows conversion if chktype is false.               */
/***********************************************************************/
bool SHVAL::SetValue_pval(PVAL valp, bool chktype)
  {
  if (chktype && Type != valp->GetType())
    return true;

  Sval = valp->GetShortValue();
  return false;
  } // end of SetValue

/***********************************************************************/
/*  SHVAL SetValue: convert chars extracted from a line to short value */
/***********************************************************************/
void SHVAL::SetValue_char(char *p, int n)
  {
  char *p2;
  bool  minus;

//	if (trace)		wrong because p can be not null terminated
//		htrc("SHVAL_char: p='%s' n=%d\n", p, n);

  for (p2 = p + n; p < p2 && *p == ' '; p++) ;

  for (Sval = 0, minus = false; p < p2; p++)
    switch (*p) {
      case '-':
        minus = true;
      case '+':
        break;
      case '0': Sval = Sval * 10;     break;
      case '1': Sval = Sval * 10 + 1; break;
      case '2': Sval = Sval * 10 + 2; break;
      case '3': Sval = Sval * 10 + 3; break;
      case '4': Sval = Sval * 10 + 4; break;
      case '5': Sval = Sval * 10 + 5; break;
      case '6': Sval = Sval * 10 + 6; break;
      case '7': Sval = Sval * 10 + 7; break;
      case '8': Sval = Sval * 10 + 8; break;
      case '9': Sval = Sval * 10 + 9; break;
      default:
        p = p2;
      } // endswitch *p

  if (minus && Sval)
    Sval = - Sval;

	if (trace)
		htrc(" setting short to: %hd\n", Sval);

  } // end of SetValue

/***********************************************************************/
/*  SHVAL SetValue: fill a short value from a string.                  */
/***********************************************************************/
void SHVAL::SetValue_psz(PSZ s)
  {
  Sval = atoi(s);
  } // end of SetValue

/***********************************************************************/
/*  SHVAL SetValue: set value with a short extracted from a block.     */
/***********************************************************************/
void SHVAL::SetValue_pvblk(PVBLK blk, int n)
  {
  Sval = blk->GetShortValue(n);
  } // end of SetValue

/***********************************************************************/
/*  SHVAL SetBinValue: with bytes extracted from a line.               */
/***********************************************************************/
void SHVAL::SetBinValue(void *p)
  {
  Sval = *(short *)p;
  } // end of SetBinValue

/***********************************************************************/
/*  GetBinValue: fill a buffer with the internal binary value.         */
/*  This function checks whether the buffer length is enough and       */
/*  returns true if not. Actual filling occurs only if go is true.     */
/*  Currently used by WriteColumn of binary files.                     */
/***********************************************************************/
bool SHVAL::GetBinValue(void *buf, int buflen, bool go)
  {
  // Test on length was removed here until a variable in column give the
  // real field length. For BIN files the field length logically cannot
  // be different from the variable length because no conversion is done.
  // Therefore this test is useless anyway.
//#if defined(_DEBUG)
//  if (sizeof(short) > buflen)
//    return true;
//#endif

  if (go)
    *(short *)buf = Sval;

  return false;
  } // end of GetBinValue

/***********************************************************************/
/*  GetBinValue: used by SELECT when called from QUERY and KINDEX.     */
/*  This is a fast implementation that does not do any checking.       */
/***********************************************************************/
void SHVAL::GetBinValue(void *buf, int buflen)
  {
  assert(buflen == sizeof(short));

  *(short *)buf = Sval;
  } // end of GetBinValue

/***********************************************************************/
/*  SHVAL ShowValue: get string representation of a short value.       */
/***********************************************************************/
char *SHVAL::ShowValue(char *buf, int len)
  {
  sprintf(buf, "%*hd", len, Sval);
  return buf;
  } // end of ShowValue

/***********************************************************************/
/*  SHVAL GetCharString: get string representation of a short value.   */
/***********************************************************************/
char *SHVAL::GetCharString(char *p)
  {
  sprintf(p, "%hd", Sval);
  return p;
  } // end of GetCharString

/***********************************************************************/
/*  SHVAL GetShortString: get short representation of a short value.   */
/***********************************************************************/
char *SHVAL::GetShortString(char *p, int n)
  {
  sprintf(p, "%*hd", n, Sval);
  return p;
  } // end of GetShortString

/***********************************************************************/
/*  SHVAL GetIntString: get int representation of a short value.     */
/***********************************************************************/
char *SHVAL::GetIntString(char *p, int n)
  {
  sprintf(p, "%*ld", n, (int)Sval);
  return p;
  } // end of GetIntString

/***********************************************************************/
/*  SHVAL GetFloatString: get double representation of a short value.  */
/***********************************************************************/
char *SHVAL::GetFloatString(char *p, int n, int prec)
  {
  sprintf(p, "%*.*lf", n, (prec < 0) ? 2 : prec, (double)Sval);
  return p;
  } // end of GetFloatString

/***********************************************************************/
/*  SHVAL compare value with another Value.                            */
/***********************************************************************/
bool SHVAL::IsEqual(PVAL vp, bool chktype)
  {
  if (this == vp)
    return true;
  else if (chktype && Type != vp->GetType())
    return false;
  else
    return (Sval == vp->GetShortValue());

  } // end of IsEqual

/***********************************************************************/
/*  Compare values and returns 1, 0 or -1 according to comparison.     */
/*  This function is used for evaluation of short integer filters.     */
/***********************************************************************/
int SHVAL::CompareValue(PVAL vp)
  {
//assert(vp->GetType() == Type);

  // Process filtering on short integers.
  short n = vp->GetShortValue();

	if (trace > 1)
		htrc(" Comparing: val=%hd,%hd\n", Sval, n);

  return (Sval > n) ? 1 : (Sval < n) ? (-1) : 0;
  } // end of CompareValue

/***********************************************************************/
/*  SafeAdd: adds a value and test whether overflow/underflow occured. */
/***********************************************************************/
short SHVAL::SafeAdd(short n1, short n2)
  {
  PGLOBAL& g = Global;
  short    n = n1 + n2;

  if ((n2 > 0) && (n < n1)) {
    // Overflow
    strcpy(g->Message, MSG(FIX_OVFLW_ADD));
    longjmp(g->jumper[g->jump_level], 138);
  } else if ((n2 < 0) && (n > n1)) {
    // Underflow
    strcpy(g->Message, MSG(FIX_UNFLW_ADD));
    longjmp(g->jumper[g->jump_level], 138);
  } // endif's n2

  return n;
  } // end of SafeAdd

/***********************************************************************/
/*  SafeMult: multiply values and test whether overflow occured.       */
/***********************************************************************/
short SHVAL::SafeMult(short n1, short n2)
  {
  PGLOBAL& g = Global;
  double   n = (double)n1 * (double)n2;

  if (n > 32767.0) {
    // Overflow
    strcpy(g->Message, MSG(FIX_OVFLW_TIMES));
    longjmp(g->jumper[g->jump_level], 138);
  } else if (n < -32768.0) {
    // Underflow
    strcpy(g->Message, MSG(FIX_UNFLW_TIMES));
    longjmp(g->jumper[g->jump_level], 138);
  } // endif's n2

  return (short)n;
  } // end of SafeMult

/***********************************************************************/
/*  Compute a function on a int integers.                             */
/***********************************************************************/
bool SHVAL::Compute(PGLOBAL g, PVAL *vp, int np, OPVAL op)
  {
  if (op == OP_LEN) {
    assert(np == 1);
    char buf[32];
    char *p = vp[0]->GetCharString(buf);

    Sval = strlen(p);

		if (trace)
			htrc("Compute result=%d val=%s op=%d\n", Sval, p, op);

  } else if (op == OP_INSTR || op == OP_LIKE || op == OP_CNTIN) {
    char *p, *tp = g->Message;
    char *p1, val1[32];
    char *p2, val2[32];
		bool  b = (vp[0]->IsCi() || vp[1]->IsCi());

    assert(np == 2);

    p1 = vp[0]->GetCharString(val1);
    p2 = vp[1]->GetCharString(val2);

    if (op != OP_LIKE) {
      if (!strcmp(p2, "\\t"))
        p2 = "\t";

			if (b) {		                    // Case insensitive
				if (strlen(p1) + strlen(p2) + 1 >= MAX_STR &&
	          !(tp = new char[strlen(p1) + strlen(p2) + 2])) {
          strcpy(g->Message, MSG(NEW_RETURN_NULL));
          return true;
					} // endif p
	  
				// Make a lower case copy of p1 and p2
		    p1 = strlwr(strcpy(tp, p1));     
		    p2 = strlwr(strcpy(tp + strlen(p1) + 1, p2));
				} // endif Ci

      if (op == OP_CNTIN) {
        size_t t2 = strlen(p2);

        for (Sval = 0; (p = strstr(p1, p2)); Sval++, p1 = p + t2) ;

      } else                 // OP_INSTR
        Sval = (p = strstr(p1, p2)) ? 1 + (short)(p - p1) : 0;

		  if (tp != g->Message)  // If working space was obtained
		    delete [] tp;        // by the use of new, delete it.

    } else                   // OP_LIKE
      Sval = (PlugEvalLike(g, p1, p2, b)) ? 1 : 0;


		if (trace)
			htrc("Compute result=%hd val=%s,%s op=%d\n", Sval, p1, p2, op);

  } else {
    short val[2];

    assert(np <= 2);

    for (int i = 0; i < np; i++)
      val[i] = vp[i]->GetShortValue();

    switch (op) {
      case OP_ABS:
        assert(np == 1);
        Sval = abs(*val);
        break;
      case OP_SIGN:
        assert(np == 1);
        Sval = (*val < 0) ? (-1) : 1;
        break;
      case OP_CEIL:
      case OP_FLOOR:
        assert(np == 1);
        Sval = *val;
        break;
      case OP_ADD:
        assert(np == 2);
        Sval = SafeAdd(val[0], val[1]);
        break;
      case OP_SUB:
        assert(np == 2);
        Sval = SafeAdd(val[0], -val[1]);
        break;
      case OP_MULT:
        assert(np == 2);
        Sval = SafeMult(val[0], val[1]);
        break;
      case OP_MIN:
        assert(np == 2);
        Sval = min(val[0], val[1]);
        break;
      case OP_MAX:
        assert(np == 2);
        Sval = max(val[0], val[1]);
        break;
      case OP_DIV:
        assert(np == 2);

        if (!val[1]) {
          strcpy(g->Message, MSG(ZERO_DIVIDE));
          return true;
          } // endif

        Sval = val[0] / val[1];
        break;
      case OP_MOD:
        assert(np == 2);

        if (!val[1]) {
          strcpy(g->Message, MSG(ZERO_DIVIDE));
          return true;
          } // endif

        Sval = val[0] % val[1];
        break;
      case OP_BITAND:
        assert(np == 2);
        Sval = val[0] & val[1];
        break;
      case OP_BITOR:
        assert(np == 2);
        Sval = val[0] | val[1];
        break;
      case OP_BITXOR:
        assert(np == 2);
        Sval = val[0] ^ val[1];
        break;
      case OP_BITNOT:
        assert(np == 1);
        Sval = ~val[0];
        break;
      case OP_DELTA:
//      assert(np == 1);
        Sval = val[0] - Sval;
        break;
      default:
        sprintf(g->Message, MSG(BAD_EXP_OPER), op);
        return true;
      } // endswitch op

		if (trace)
			if (np = 1)
			  htrc(" result=%hd val=%hd op=%d\n", Sval, val[0], op);
			else
			  htrc(" result=%hd val=%hd,%hd op=%d\n", 
						   Sval, val[0], val[1], op);

  } // endif op

  return false;
  } // end of Compute

/***********************************************************************/
/*  Divide: used by aggregate functions when calculating average.      */
/***********************************************************************/
void SHVAL::Divide(int cnt)
  {
  Sval /= (short)cnt;
  } // end of Divide

/***********************************************************************/
/*  StdVar: used by aggregate functions for Stddev and Variance.       */
/***********************************************************************/
void SHVAL::StdVar(PVAL vp, int cnt, bool b)
  {
  short lv2 = vp->GetShortValue(), scnt = (short)cnt;

  Sval = (scnt == 1) ? 0
       : (SafeAdd(lv2, -(SafeMult(Sval, Sval) / scnt)) / (scnt - 1));

  if (b) // Builtin == FNC_STDDEV
    Sval = (short)sqrt((double)Sval);

  } // end of StdVar

/***********************************************************************/
/*  Times: used by aggregate functions for Stddev and Variance.        */
/***********************************************************************/
void SHVAL::Times(PVAL vp)
  {
  Sval = SafeMult(Sval, vp->GetShortValue());
  } // end of Times

/***********************************************************************/
/*  Add: used by aggregate functions for Sum and other functions.      */
/***********************************************************************/
void SHVAL::Add(PVAL vp)
  {
  Sval = SafeAdd(Sval, vp->GetShortValue());
  } // end of Add

/***********************************************************************/
/*  Add: used by QUERY for function Sum and other functions.           */
/***********************************************************************/
void SHVAL::Add(PVBLK vbp, int i)
  {
  Sval = SafeAdd(Sval, vbp->GetShortValue(i));
  } // end of Add

/***********************************************************************/
/*  Add: used by QUERY for function Sum and other functions.           */
/***********************************************************************/
void SHVAL::Add(PVBLK vbp, int j, int k)
  {
  CheckType(vbp)
  short *lp = (short *)vbp->GetValPointer();

  for (register int i = j; i < k; i++)
    Sval = SafeAdd(Sval, lp[i]);

  } // end of Add

/***********************************************************************/
/*  Add: used by QUERY for function Sum and other functions.           */
/***********************************************************************/
void SHVAL::Add(PVBLK vbp, int *x, int j, int k)
  {
  CheckType(vbp)
  short *lp = (short *)vbp->GetValPointer();

  for (register int i = j; i < k; i++)
    Sval = SafeAdd(Sval, lp[x[i]]);

  } // end of Add

/***********************************************************************/
/*  AddSquare: used by aggregate functions for Stddev and Variance.    */
/***********************************************************************/
void SHVAL::AddSquare(PVAL vp)
  {
  short val = vp->GetShortValue();

  Sval = SafeAdd(Sval, SafeMult(val, val));
  } // end of AddSquare

/***********************************************************************/
/*  AddSquare: used by QUERY for functions Stddev and Variance.        */
/***********************************************************************/
void SHVAL::AddSquare(PVBLK vbp, int i)
  {
  short val = vbp->GetShortValue(i);

  Sval = SafeAdd(Sval, SafeMult(val, val));
  } // end of AddSquare

/***********************************************************************/
/*  AddSquare: used by QUERY for functions Stddev and Variance.        */
/***********************************************************************/
void SHVAL::AddSquare(PVBLK vbp, int j, int k)
  {
  CheckType(vbp)
  short *lp = (short *)vbp->GetValPointer();

  for (register int i = j; i < k; i++)
    Sval = SafeAdd(Sval, SafeMult(lp[i], lp[i]));

  } // end of AddSquare

/***********************************************************************/
/*  FormatValue: This function set vp (a STRING value) to the string   */
/*  constructed from its own value formated using the fmt format.      */
/*  This function assumes that the format matches the value type.      */
/***********************************************************************/
bool SHVAL::FormatValue(PVAL vp, char *fmt)
  {
  char *buf = (char*)vp->GetTo_Val();        // Should be big enough
  int   n = sprintf(buf, fmt, Sval);

  return (n > vp->GetValLen());
  } // end of FormatValue

/***********************************************************************/
/*  SetMin: used by the aggregate function MIN.                        */
/***********************************************************************/
void SHVAL::SetMin(PVAL vp)
  {
  short val = vp->GetShortValue();

  if (val < Sval)
    Sval = val;

  } // end of SetMin

/***********************************************************************/
/*  SetMin: used by QUERY for the aggregate function MIN.              */
/***********************************************************************/
void SHVAL::SetMin(PVBLK vbp, int i)
  {
  short val = vbp->GetShortValue(i);

  if (val < Sval)
    Sval = val;

  } // end of SetMin

/***********************************************************************/
/*  SetMin: used by QUERY for the aggregate function MIN.              */
/***********************************************************************/
void SHVAL::SetMin(PVBLK vbp, int j, int k)
  {
  CheckType(vbp)
  short *lp = (short *)vbp->GetValPointer();

  for (register int i = j; i < k; i++)
    if (lp[i] < Sval)
      Sval = lp[i];

  } // end of SetMin

/***********************************************************************/
/*  SetMin: used by QUERY for the aggregate function MIN.              */
/***********************************************************************/
void SHVAL::SetMin(PVBLK vbp, int *x, int j, int k)
  {
  CheckType(vbp)
  short  val;
  short *lp = (short *)vbp->GetValPointer();

  for (register int i = j; i < k; i++) {
    val = lp[x[i]];

    if (val < Sval)
      Sval = val;

    } // endfor i

  } // end of SetMin

/***********************************************************************/
/*  SetMax: used by the aggregate function MAX.                        */
/***********************************************************************/
void SHVAL::SetMax(PVAL vp)
  {
  short val = vp->GetShortValue();

  if (val > Sval)
    Sval = val;

  } // end of SetMax

/***********************************************************************/
/*  SetMax: used by QUERY for the aggregate function MAX.              */
/***********************************************************************/
void SHVAL::SetMax(PVBLK vbp, int i)
  {
  short val = vbp->GetShortValue(i);

  if (val > Sval)
    Sval = val;

  } // end of SetMax

/***********************************************************************/
/*  SetMax: used by QUERY for the aggregate function MAX.              */
/***********************************************************************/
void SHVAL::SetMax(PVBLK vbp, int j, int k)
  {
  CheckType(vbp)
  short *lp = (short *)vbp->GetValPointer();

  for (register int i = j; i < k; i++)
    if (lp[i] > Sval)
      Sval = lp[i];

  } // end of SetMax

/***********************************************************************/
/*  SetMax: used by QUERY for the aggregate function MIN.              */
/***********************************************************************/
void SHVAL::SetMax(PVBLK vbp, int *x, int j, int k)
  {
  CheckType(vbp)
  short  val;
  short *lp = (short *)vbp->GetValPointer();

  for (register int i = j; i < k; i++) {
    val = lp[x[i]];

    if (val > Sval)
      Sval = val;

    } // endfor i

  } // end of SetMax

/***********************************************************************/
/*  SHVAL  SetFormat function (used to set SELECT output format).      */
/***********************************************************************/
bool SHVAL::SetConstFormat(PGLOBAL g, FORMAT& fmt)
  {
  char c[16];

  fmt.Type[0] = 'N';
  fmt.Length = sprintf(c, "%hd", Sval);
  fmt.Prec = 0;
  return false;
  } // end of SetConstFormat

/***********************************************************************/
/*  Make file output of a short object.                                */
/***********************************************************************/
void SHVAL::Print(PGLOBAL g, FILE *f, uint n)
  {
  char m[64];

  memset(m, ' ', n);                             /* Make margin string */
  m[n] = '\0';

  fprintf(f, "%s%hd\n", m, Sval);
  } /* end of Print */

/***********************************************************************/
/*  Make string output of a short object.                              */
/***********************************************************************/
void SHVAL::Print(PGLOBAL g, char *ps, uint z)
  {
  sprintf(ps, "%hd", Sval);
  } /* end of Print */

/* -------------------------- Class INTVAL ---------------------------- */

/***********************************************************************/
/*  INTVAL  public constructor from char.                               */
/***********************************************************************/
INTVAL::INTVAL(PSZ s) : VALUE(TYPE_INT)
  {
  Ival = atol(s);
  Clen = sizeof(int);
  } // end of INTVAL constructor

/***********************************************************************/
/*  INTVAL  public constructor from short.                              */
/***********************************************************************/
INTVAL::INTVAL(short n) : VALUE(TYPE_INT)
  {
  Ival = (int)n;
  Clen = sizeof(int);
  } // end of INTVAL constructor

/***********************************************************************/
/*  INTVAL  public constructor from int.                               */
/***********************************************************************/
INTVAL::INTVAL(int n) : VALUE(TYPE_INT)
  {
  Ival = n;
  Clen = sizeof(int);
  } // end of INTVAL constructor

/***********************************************************************/
/*  INTVAL  public constructor from double.                             */
/***********************************************************************/
INTVAL::INTVAL(double f) : VALUE(TYPE_INT)
  {
  Ival = (int)f;
  Clen = sizeof(int);
  } // end of INTVAL constructor

/***********************************************************************/
/*  INTVAL GetValLen: returns the print length of the int object.      */
/***********************************************************************/
int INTVAL::GetValLen(void)
  {
  char c[16];

  return sprintf(c, "%d", Ival);
  } // end of GetValLen

/***********************************************************************/
/*  INTVAL SetValue: copy the value of another Value object.            */
/*  This function allows conversion if chktype is false.               */
/***********************************************************************/
bool INTVAL::SetValue_pval(PVAL valp, bool chktype)
  {
  if (chktype && Type != valp->GetType())
    return true;

  Ival = valp->GetIntValue();
  return false;
  } // end of SetValue

/***********************************************************************/
/*  INTVAL SetValue: convert chars extracted from a line to int value. */
/***********************************************************************/
void INTVAL::SetValue_char(char *p, int n)
  {
  char *p2;
  bool  minus;

  for (p2 = p + n; p < p2 && *p == ' '; p++) ;

  for (Ival = 0, minus = false; p < p2; p++)
    switch (*p) {
      case '-':
        minus = true;
      case '+':
        break;
      case '0': Ival = Ival * 10;      break;
      case '1': Ival = Ival * 10 + 1; break;
      case '2': Ival = Ival * 10 + 2; break;
      case '3': Ival = Ival * 10 + 3; break;
      case '4': Ival = Ival * 10 + 4; break;
      case '5': Ival = Ival * 10 + 5; break;
      case '6': Ival = Ival * 10 + 6; break;
      case '7': Ival = Ival * 10 + 7; break;
      case '8': Ival = Ival * 10 + 8; break;
      case '9': Ival = Ival * 10 + 9; break;
      default:
        p = p2;
      } // endswitch *p

  if (minus && Ival)
    Ival = - Ival;

	if (trace)
		htrc(" setting int to: %d\n", Ival);

  } // end of SetValue

/***********************************************************************/
/*  INTVAL SetValue: fill a int value from a string.                   */
/***********************************************************************/
void INTVAL::SetValue_psz(PSZ s)
  {
  Ival = atol(s);
  } // end of SetValue

/***********************************************************************/
/*  INTVAL SetValue: set value with a int extracted from a block.      */
/***********************************************************************/
void INTVAL::SetValue_pvblk(PVBLK blk, int n)
  {
  Ival = blk->GetIntValue(n);
  } // end of SetValue

/***********************************************************************/
/*  INTVAL SetBinValue: with bytes extracted from a line.               */
/***********************************************************************/
void INTVAL::SetBinValue(void *p)
  {
  Ival = *(int *)p;
  } // end of SetBinValue

/***********************************************************************/
/*  GetBinValue: fill a buffer with the internal binary value.         */
/*  This function checks whether the buffer length is enough and       */
/*  returns true if not. Actual filling occurs only if go is true.     */
/*  Currently used by WriteColumn of binary files.                     */
/***********************************************************************/
bool INTVAL::GetBinValue(void *buf, int buflen, bool go)
  {
  // Test on length was removed here until a variable in column give the
  // real field length. For BIN files the field length logically cannot
  // be different from the variable length because no conversion is done.
  // Therefore this test is useless anyway.
//#if defined(_DEBUG)
//  if (sizeof(int) > buflen)
//    return true;
//#endif

  if (go)
    *(int *)buf = Ival;

  return false;
  } // end of GetBinValue

/***********************************************************************/
/*  GetBinValue: used by SELECT when called from QUERY and KINDEX.     */
/*  This is a fast implementation that does not do any checking.       */
/***********************************************************************/
void INTVAL::GetBinValue(void *buf, int buflen)
  {
  assert(buflen == sizeof(int));

  *(int *)buf = Ival;
  } // end of GetBinValue

/***********************************************************************/
/*  INTVAL ShowValue: get string representation of a int value.        */
/***********************************************************************/
char *INTVAL::ShowValue(char *buf, int len)
  {
  sprintf(buf, "%*ld", len, Ival);
  return buf;
  } // end of ShowValue

/***********************************************************************/
/*  INTVAL GetCharString: get string representation of a int value.    */
/***********************************************************************/
char *INTVAL::GetCharString(char *p)
  {
  sprintf(p, "%d", Ival);
  return p;
  } // end of GetCharString

/***********************************************************************/
/*  INTVAL GetShortString: get short representation of a int value.    */
/***********************************************************************/
char *INTVAL::GetShortString(char *p, int n)
  {
  sprintf(p, "%*hd", n, (short)Ival);
  return p;
  } // end of GetShortString

/***********************************************************************/
/*  INTVAL GetIntString: get int representation of a int value.      */
/***********************************************************************/
char *INTVAL::GetIntString(char *p, int n)
  {
  sprintf(p, "%*ld", n, Ival);
  return p;
  } // end of GetIntString

/***********************************************************************/
/*  INTVAL GetFloatString: get double representation of a int value.   */
/***********************************************************************/
char *INTVAL::GetFloatString(char *p, int n, int prec)
  {
  sprintf(p, "%*.*lf", n, (prec < 0) ? 2 : prec, (double)Ival);
  return p;
  } // end of GetFloatString

/***********************************************************************/
/*  INTVAL compare value with another Value.                            */
/***********************************************************************/
bool INTVAL::IsEqual(PVAL vp, bool chktype)
  {
  if (this == vp)
    return true;
  else if (chktype && Type != vp->GetType())
    return false;
  else
    return (Ival == vp->GetIntValue());

  } // end of IsEqual

/***********************************************************************/
/*  Compare values and returns 1, 0 or -1 according to comparison.     */
/*  This function is used for evaluation of int integer filters.      */
/***********************************************************************/
int INTVAL::CompareValue(PVAL vp)
  {
//assert(vp->GetType() == Type);

  // Process filtering on int integers.
  int n = vp->GetIntValue();

	if (trace > 1)
		htrc(" Comparing: val=%d,%d\n", Ival, n);

  return (Ival > n) ? 1 : (Ival < n) ? (-1) : 0;
  } // end of CompareValue

/***********************************************************************/
/*  SafeAdd: adds a value and test whether overflow/underflow occured. */
/***********************************************************************/
int INTVAL::SafeAdd(int n1, int n2)
  {
  PGLOBAL& g = Global;
  int     n = n1 + n2;

  if ((n2 > 0) && (n < n1)) {
    // Overflow
    strcpy(g->Message, MSG(FIX_OVFLW_ADD));
    longjmp(g->jumper[g->jump_level], 138);
  } else if ((n2 < 0) && (n > n1)) {
    // Underflow
    strcpy(g->Message, MSG(FIX_UNFLW_ADD));
    longjmp(g->jumper[g->jump_level], 138);
  } // endif's n2

  return n;
  } // end of SafeAdd

/***********************************************************************/
/*  SafeMult: multiply values and test whether overflow occured.       */
/***********************************************************************/
int INTVAL::SafeMult(int n1, int n2)
  {
  PGLOBAL& g = Global;
  double   n = (double)n1 * (double)n2;

  if (n > 2147483647.0) {
    // Overflow
    strcpy(g->Message, MSG(FIX_OVFLW_TIMES));
    longjmp(g->jumper[g->jump_level], 138);
  } else if (n < -2147483648.0) {
    // Underflow
    strcpy(g->Message, MSG(FIX_UNFLW_TIMES));
    longjmp(g->jumper[g->jump_level], 138);
  } // endif's n2

  return (int)n;
  } // end of SafeMult

/***********************************************************************/
/*  Compute a function on a int integers.                             */
/***********************************************************************/
bool INTVAL::Compute(PGLOBAL g, PVAL *vp, int np, OPVAL op)
  {
  if (op == OP_LEN) {
    assert(np == 1);
    char buf[32];
    char *p = vp[0]->GetCharString(buf);

    Ival = strlen(p);

		if (trace)
			htrc("Compute result=%d val=%s op=%d\n", Ival, p, op);

  } else if (op == OP_INSTR || op == OP_LIKE || op == OP_CNTIN) {
    char *p, *tp = g->Message;
    char *p1, val1[32];
    char *p2, val2[32];
		bool  b = (vp[0]->IsCi() || vp[1]->IsCi());

    assert(np == 2);

    p1 = vp[0]->GetCharString(val1);
    p2 = vp[1]->GetCharString(val2);

    if (op != OP_LIKE) {
      if (!strcmp(p2, "\\t"))
        p2 = "\t";

			if (b) {		                      // Case insensitive
				if (strlen(p1) + strlen(p2) + 1 >= MAX_STR &&
	          !(tp = new char[strlen(p1) + strlen(p2) + 2])) {
          strcpy(g->Message, MSG(NEW_RETURN_NULL));
          return true;
					} // endif p
	  
				// Make a lower case copy of p1 and p2
		    p1 = strlwr(strcpy(tp, p1));     
		    p2 = strlwr(strcpy(tp + strlen(p1) + 1, p2));
				} // endif b

      if (op == OP_CNTIN) {
        size_t t2 = strlen(p2);

        for (Ival = 0; (p = strstr(p1, p2)); Ival++, p1 = p + t2) ;

      } else                 // OP_INSTR
        Ival = (p = strstr(p1, p2)) ? 1 + (int)(p - p1) : 0;

		  if (tp != g->Message)  // If working space was obtained
		    delete [] tp;        // by the use of new, delete it.

    } else                   // OP_LIKE
      Ival = (PlugEvalLike(g, p1, p2, b)) ? 1 : 0;

		if (trace)
			htrc("Compute result=%d val=%s,%s op=%d\n", Ival, p1, p2, op);

  } else if (op == OP_MDAY || op == OP_MONTH || op == OP_YEAR ||
             op == OP_WDAY || op == OP_QUART || op == OP_YDAY) {
    assert(np == 1 && vp[0]->GetType() == TYPE_DATE);

    if (((DTVAL*)vp[0])->GetTmMember(op, Ival)) {
      sprintf(g->Message, MSG(COMPUTE_ERROR), op);
      return true;
      } // endif

  } else if (op == OP_NWEEK) {
    // Week number of the year for the internal date value
    assert((np == 1 || np == 2) && vp[0]->GetType() == TYPE_DATE);

    // Start of the week SUN=0, MON=1, etc.
    Ival = (np == 2) ? vp[1]->GetIntValue() : 1;

    // This function sets Ival to nweek
    if (((DTVAL*)vp[0])->WeekNum(g, Ival))
      return true;

  } else if (op == OP_DBTWN || op == OP_MBTWN || op == OP_YBTWN) {
    assert(np == 2 && vp[0]->GetType() == TYPE_DATE
                   && vp[1]->GetType() == TYPE_DATE);

    if (((DTVAL*)vp[0])->DateDiff((DTVAL*)vp[1], op, Ival)) {
      sprintf(g->Message, MSG(COMPUTE_ERROR), op);
      return true;
      } // endif

  } else if (op == OP_TIME) {
    Ival = vp[0]->GetTime(g, (np == 1) ? NULL : vp + 1, np - 1);
  } else {
    int val[2];

    assert(np <= 2);

    for (int i = 0; i < np; i++)
      val[i] = vp[i]->GetIntValue();

    switch (op) {
      case OP_ABS:
        assert(np == 1);
        Ival = labs(*val);
        break;
      case OP_SIGN:
        assert(np == 1);
        Ival = (*val < 0) ? (-1) : 1;
        break;
      case OP_CEIL:
      case OP_FLOOR:
        assert(np == 1);
        Ival = *val;
        break;
      case OP_ADD:
        assert(np == 2);
        Ival = SafeAdd(val[0], val[1]);
        break;
      case OP_SUB:
        assert(np == 2);
        Ival = SafeAdd(val[0], -val[1]);
        break;
      case OP_MULT:
        assert(np == 2);
        Ival = SafeMult(val[0], val[1]);
        break;
      case OP_MIN:
        assert(np == 2);
        Ival = min(val[0], val[1]);
        break;
      case OP_MAX:
        assert(np == 2);
        Ival = max(val[0], val[1]);
        break;
      case OP_DIV:
        assert(np == 2);

        if (!val[1]) {
          strcpy(g->Message, MSG(ZERO_DIVIDE));
          return true;
          } // endif

        Ival = val[0] / val[1];
        break;
      case OP_MOD:
        assert(np == 2);

        if (!val[1]) {
          strcpy(g->Message, MSG(ZERO_DIVIDE));
          return true;
          } // endif

        Ival = val[0] % val[1];
        break;
      case OP_BITAND:
        assert(np == 2);
        Ival = val[0] & val[1];
        break;
      case OP_BITOR:
        assert(np == 2);
        Ival = val[0] | val[1];
        break;
      case OP_BITXOR:
        assert(np == 2);
        Ival = val[0] ^ val[1];
        break;
      case OP_BITNOT:
        assert(np == 1);
        Ival = ~val[0];
        break;
      case OP_DELTA:
//      assert(np == 1);
        Ival = val[0] - Ival;
        break;
      default:
        sprintf(g->Message, MSG(BAD_EXP_OPER), op);
        return true;
      } // endswitch op

		if (trace)
			if (np = 1)
				htrc(" result=%d val=%d op=%d\n", Ival, val[0], op);
			else
				htrc(" result=%d val=%d,%d op=%d\n",
							 Ival, val[0], val[1], op);

  } // endif op

  return false;
  } // end of Compute

/***********************************************************************/
/*  GetTime: convert HR/MIN/SEC in a number of seconds.                */
/***********************************************************************/
int INTVAL::GetTime(PGLOBAL g, PVAL *vp, int np)
  {
  int sec = Ival;

  for (int i = 0; i < 2; i++) {
    sec *= 60;

    if (np > i)
      sec += vp[i]->GetIntValue();

    } // endfor i

  return sec;
  } // end of GetTime

/***********************************************************************/
/*  Divide: used by aggregate functions when calculating average.      */
/***********************************************************************/
void INTVAL::Divide(int cnt)
  {
  Ival /= cnt;
  } // end of Divide

/***********************************************************************/
/*  StdVar: used by aggregate functions for Stddev and Variance.       */
/***********************************************************************/
void INTVAL::StdVar(PVAL vp, int cnt, bool b)
  {
  int lv2 = vp->GetIntValue();

  Ival = (cnt == 1) ? 0
       : (SafeAdd(lv2, -(SafeMult(Ival, Ival) / cnt)) / (cnt - 1));

  if (b)    // Builtin == FNC_STDDEV
    Ival = (int)sqrt((double)Ival);

  } // end of StdVar

/***********************************************************************/
/*  Times: used by aggregate functions for Stddev and Variance.        */
/***********************************************************************/
void INTVAL::Times(PVAL vp)
  {
  Ival = SafeMult(Ival, vp->GetIntValue());
  } // end of Times

/***********************************************************************/
/*  Add: used by aggregate functions for Sum and other functions.      */
/***********************************************************************/
void INTVAL::Add(PVAL vp)
  {
  Ival = SafeAdd(Ival, vp->GetIntValue());
  } // end of Add

/***********************************************************************/
/*  Add: used by QUERY for function Sum and other functions.           */
/***********************************************************************/
void INTVAL::Add(PVBLK vbp, int i)
  {
  Ival = SafeAdd(Ival, vbp->GetIntValue(i));
  } // end of Add

/***********************************************************************/
/*  Add: used by QUERY for function Sum and other functions.           */
/***********************************************************************/
void INTVAL::Add(PVBLK vbp, int j, int k)
  {
  CheckType(vbp)
  int *lp = (int *)vbp->GetValPointer();

  for (register int i = j; i < k; i++)
    Ival = SafeAdd(Ival, lp[i]);

  } // end of Add

/***********************************************************************/
/*  Add: used by QUERY for function Sum and other functions.           */
/***********************************************************************/
void INTVAL::Add(PVBLK vbp, int *x, int j, int k)
  {
  CheckType(vbp)
  int *lp = (int *)vbp->GetValPointer();

  for (register int i = j; i < k; i++)
    Ival = SafeAdd(Ival, lp[x[i]]);

  } // end of Add

/***********************************************************************/
/*  AddSquare: used by aggregate functions for Stddev and Variance.    */
/***********************************************************************/
void INTVAL::AddSquare(PVAL vp)
  {
  int val = vp->GetIntValue();

  Ival = SafeAdd(Ival, SafeMult(val, val));
  } // end of AddSquare

/***********************************************************************/
/*  AddSquare: used by QUERY for functions Stddev and Variance.        */
/***********************************************************************/
void INTVAL::AddSquare(PVBLK vbp, int i)
  {
  int val = vbp->GetIntValue(i);

  Ival = SafeAdd(Ival, SafeMult(val, val));
  } // end of AddSquare

/***********************************************************************/
/*  AddSquare: used by QUERY for functions Stddev and Variance.        */
/***********************************************************************/
void INTVAL::AddSquare(PVBLK vbp, int j, int k)
  {
  CheckType(vbp)
  int *lp = (int *)vbp->GetValPointer();

  for (register int i = j; i < k; i++)
    Ival = SafeAdd(Ival, SafeMult(lp[i], lp[i]));

  } // end of AddSquare

/***********************************************************************/
/*  FormatValue: This function set vp (a STRING value) to the string   */
/*  constructed from its own value formated using the fmt format.      */
/*  This function assumes that the format matches the value type.      */
/***********************************************************************/
bool INTVAL::FormatValue(PVAL vp, char *fmt)
  {
  char *buf = (char*)vp->GetTo_Val();        // Should be big enough
  int   n = sprintf(buf, fmt, Ival);

  return (n > vp->GetValLen());
  } // end of FormatValue

/***********************************************************************/
/*  SetMin: used by the aggregate function MIN.                        */
/***********************************************************************/
void INTVAL::SetMin(PVAL vp)
  {
  int val = vp->GetIntValue();

  if (val < Ival)
    Ival = val;

  } // end of SetMin

/***********************************************************************/
/*  SetMin: used by QUERY for the aggregate function MIN.              */
/***********************************************************************/
void INTVAL::SetMin(PVBLK vbp, int i)
  {
  int val = vbp->GetIntValue(i);

  if (val < Ival)
    Ival = val;

  } // end of SetMin

/***********************************************************************/
/*  SetMin: used by QUERY for the aggregate function MIN.              */
/***********************************************************************/
void INTVAL::SetMin(PVBLK vbp, int j, int k)
  {
  CheckType(vbp)
  int *lp = (int *)vbp->GetValPointer();

  for (register int i = j; i < k; i++)
    if (lp[i] < Ival)
      Ival = lp[i];

  } // end of SetMin

/***********************************************************************/
/*  SetMin: used by QUERY for the aggregate function MIN.              */
/***********************************************************************/
void INTVAL::SetMin(PVBLK vbp, int *x, int j, int k)
  {
  CheckType(vbp)
  register int val;
  int *lp = (int *)vbp->GetValPointer();

  for (register int i = j; i < k; i++) {
    val = lp[x[i]];

    if (val < Ival)
      Ival = val;

    } // endfor i

  } // end of SetMin

/***********************************************************************/
/*  SetMax: used by the aggregate function MAX.                        */
/***********************************************************************/
void INTVAL::SetMax(PVAL vp)
  {
  int val = vp->GetIntValue();

  if (val > Ival)
    Ival = val;

  } // end of SetMax

/***********************************************************************/
/*  SetMax: used by QUERY for the aggregate function MAX.              */
/***********************************************************************/
void INTVAL::SetMax(PVBLK vbp, int i)
  {
  int val = vbp->GetIntValue(i);

  if (val > Ival)
    Ival = val;

  } // end of SetMax

/***********************************************************************/
/*  SetMax: used by QUERY for the aggregate function MAX.              */
/***********************************************************************/
void INTVAL::SetMax(PVBLK vbp, int j, int k)
  {
  CheckType(vbp)
  int *lp = (int *)vbp->GetValPointer();

  for (register int i = j; i < k; i++)
    if (lp[i] > Ival)
      Ival = lp[i];

  } // end of SetMax

/***********************************************************************/
/*  SetMax: used by QUERY for the aggregate function MIN.              */
/***********************************************************************/
void INTVAL::SetMax(PVBLK vbp, int *x, int j, int k)
  {
  CheckType(vbp)
  register int val;
  int *lp = (int *)vbp->GetValPointer();

  for (register int i = j; i < k; i++) {
    val = lp[x[i]];

    if (val > Ival)
      Ival = val;

    } // endfor i

  } // end of SetMax

/***********************************************************************/
/*  INTVAL  SetFormat function (used to set SELECT output format).      */
/***********************************************************************/
bool INTVAL::SetConstFormat(PGLOBAL g, FORMAT& fmt)
  {
  char c[16];

  fmt.Type[0] = 'N';
  fmt.Length = sprintf(c, "%d", Ival);
  fmt.Prec = 0;
  return false;
  } // end of SetConstFormat

/***********************************************************************/
/*  Make file output of a int object.                                 */
/***********************************************************************/
void INTVAL::Print(PGLOBAL g, FILE *f, uint n)
  {
  char m[64];

  memset(m, ' ', n);                             /* Make margin string */
  m[n] = '\0';

  fprintf(f, "%s%d\n", m, Ival);
  } /* end of Print */

/***********************************************************************/
/*  Make string output of a int object.                               */
/***********************************************************************/
void INTVAL::Print(PGLOBAL g, char *ps, uint z)
  {
  sprintf(ps, "%d", Ival);
  } /* end of Print */

/* -------------------------- Class DTVAL ---------------------------- */

/***********************************************************************/
/*  DTVAL  public constructor for new void values.                     */
/***********************************************************************/
DTVAL::DTVAL(PGLOBAL g, int n, int prec, PSZ fmt) : INTVAL((int)0)
  {
  if (!fmt) {
    Pdtp = NULL;
    Sdate = NULL;
    DefYear = 0;
    Len = n;
  } else
    SetFormat(g, fmt, n, prec);

  Type = TYPE_DATE;
  } // end of DTVAL constructor

/***********************************************************************/
/*  DTVAL  public constructor from char.                               */
/***********************************************************************/
DTVAL::DTVAL(PGLOBAL g, PSZ s, int n) : INTVAL((s) ? s : (char *)"0")
  {
  Pdtp = NULL;
  Len = n;
  Type = TYPE_DATE;
  Sdate = NULL;
  DefYear = 0;
  } // end of DTVAL constructor

/***********************************************************************/
/*  DTVAL  public constructor from short.                              */
/***********************************************************************/
DTVAL::DTVAL(PGLOBAL g, short n) : INTVAL((int)n)
  {
  Pdtp = NULL;
  Len = 19;
  Type = TYPE_DATE;
  Sdate = NULL;
  DefYear = 0;
  } // end of DTVAL constructor

/***********************************************************************/
/*  DTVAL  public constructor from int.                               */
/***********************************************************************/
DTVAL::DTVAL(PGLOBAL g, int n) : INTVAL(n)
  {
  Pdtp = NULL;
  Len = 19;
  Type = TYPE_DATE;
  Sdate = NULL;
  DefYear = 0;
  } // end of DTVAL constructor

/***********************************************************************/
/*  DTVAL  public constructor from double.                             */
/***********************************************************************/
DTVAL::DTVAL(PGLOBAL g, double f) : INTVAL(f)
  {
  Pdtp = NULL;
  Len = 19;
  Type = TYPE_DATE;
  Sdate = NULL;
  DefYear = 0;
  } // end of DTVAL constructor

/***********************************************************************/
/*  Set format so formatted dates can be converted on input/output.    */
/***********************************************************************/
bool DTVAL::SetFormat(PGLOBAL g, PSZ fmt, int len, int year)
  {
  Pdtp = MakeDateFormat(g, fmt, true, true, (year > 9999) ? 1 : 0);
  Sdate = (char*)PlugSubAlloc(g, NULL, len + 1);
  DefYear = (int)((year > 9999) ? (year - 10000) : year);
  Len = len;
  return false;
  } // end of SetFormat

/***********************************************************************/
/*  Set format from the format of another date value.                  */
/***********************************************************************/
bool DTVAL::SetFormat(PGLOBAL g, PVAL valp)
  {
  DTVAL *vp;

  if (valp->GetType() != TYPE_DATE) {
    sprintf(g->Message, MSG(NO_FORMAT_TYPE), valp->GetType());
    return true;
  } else
    vp = (DTVAL*)valp;

  Len = vp->Len;
  Pdtp = vp->Pdtp;
  Sdate = (char*)PlugSubAlloc(g, NULL, Len + 1);
  DefYear = vp->DefYear;
  return false;
  } // end of SetFormat

/***********************************************************************/
/*  We need TimeShift because the mktime C function does a correction  */
/*  for local time zone that we want to override for DB operations.    */
/***********************************************************************/
void DTVAL::SetTimeShift(void)
  {
  struct tm dtm = {0,0,0,2,0,70,0,0,0};

  Shift = (int)mktime(&dtm) - 86400;

  if (trace)
    htrc("DTVAL Shift=%d\n", Shift);

  } // end of SetTimeShift

/***********************************************************************/
/*  GetGmTime: returns a pointer to a static tm structure obtained     */
/*  though the gmtime C function. The purpose of this function is to   */
/*  extend the range of valid dates by accepting negative time values. */
/***********************************************************************/
struct tm *DTVAL::GetGmTime(void)
  {
  struct tm *datm;
  time_t t = (time_t)Ival;

  if (Ival < 0) {
    int    n;

    for (n = 0; t < 0; n += 4)
      t += FOURYEARS;

    datm = gmtime(&t);

    if (datm)
      datm->tm_year -= n;

  } else
    datm = gmtime((const time_t *)&t);

  return datm;
  } // end of GetGmTime

/***********************************************************************/
/*  MakeTime: calculates a date value from a tm structures using the   */
/*  mktime C function. The purpose of this function is to extend the   */
/*  range of valid dates by accepting to set negative time values.     */
/***********************************************************************/
bool DTVAL::MakeTime(struct tm *ptm)
  {
  int    n, y = ptm->tm_year;
  time_t t = mktime(ptm);

	if (trace)
		htrc("MakeTime from (%d,%d,%d,%d,%d,%d)\n", 
					ptm->tm_year, ptm->tm_mon, ptm->tm_mday,
					ptm->tm_hour, ptm->tm_min, ptm->tm_sec);

  if (t == -1) {
    if (y < 1 || y > 71)
      return true;

    for (n = 0; t == -1 && n < 20; n++) {
      ptm->tm_year += 4;
      t = mktime(ptm);
      } // endfor t

    if (t == -1)
      return true;

    if ((t -= (n * FOURYEARS + Shift)) > 2000000000)
      return true;

    Ival = (int)t;
  } else
    Ival = (int)t - Shift;

	if (trace)
		htrc("MakeTime Ival=%d\n", Ival); 

  return false;
  } // end of MakeTime

/***********************************************************************/
/* Make a time_t datetime from its components (YY, MM, DD, hh, mm, ss) */
/***********************************************************************/
bool DTVAL::MakeDate(PGLOBAL g, int *val, int nval)
  {
  int       i, m;
  int      n;
  bool      rc = false;
  struct tm datm = {0,0,0,1,0,70,0,0,0};

  if (trace)
    htrc("MakeDate from(%d,%d,%d,%d,%d,%d) nval=%d\n",
	  val[0], val[1], val[2], val[3], val[4], val[5], nval);

  for (i = 0; i < nval; i++) {
    n = val[i];

//    if (trace > 1)
//      htrc("i=%d n=%d\n", i, n);

    switch (i) {
      case 0:
        if (n >= 1900)
          n -= 1900;

        datm.tm_year = n;

//        if (trace > 1)
//          htrc("n=%d tm_year=%d\n", n, datm.tm_year);

        break;
      case 1:
        // If mktime handles apparently correctly large or negative
        // day values, it is not the same for months. Therefore we
        // do the ajustment here, thus mktime has not to do it.
        if (n > 0) {
          m = (n - 1) % 12;
          n = (n - 1) / 12;
        } else {
          m = 11 + n % 12;
          n = n / 12 - 1;
        } // endfi n

        datm.tm_mon = m;
        datm.tm_year += n;

//        if (trace > 1)
//          htrc("n=%d m=%d tm_year=%d tm_mon=%d\n", n, m, datm.tm_year, datm.tm_mon);

        break;
      case 2:
        // For days, big or negative values may also cause problems
        m = n % 1461;
        n = 4 * (n / 1461);

        if (m < 0) {
          m += 1461;
          n -= 4;
          } // endif m

        datm.tm_mday = m;
        datm.tm_year += n;

//        if (trace > 1)
//          htrc("n=%d m=%d tm_year=%d tm_mon=%d\n", n, m, datm.tm_year, datm.tm_mon);

       break;
      case 3: datm.tm_hour = n; break;
      case 4: datm.tm_min  = n; break;
      case 5: datm.tm_sec  = n; break;
      } // endswitch i

    } // endfor i

  if (trace)
    htrc("MakeDate datm=(%d,%d,%d,%d,%d,%d)\n", 
	  datm.tm_year, datm.tm_mon, datm.tm_mday,
	  datm.tm_hour, datm.tm_min, datm.tm_sec);

  // Pass g to have an error return or NULL to set invalid dates to 0
  if (MakeTime(&datm))
    if (g) {
      strcpy(g->Message, MSG(BAD_DATETIME));
      rc = true;
		} else
			Ival = 0;

  return rc;
  } // end of MakeDate

/***********************************************************************/
/*  DTVAL SetValue: copy the value of another Value object.            */
/*  This function allows conversion if chktype is false.               */
/***********************************************************************/
bool DTVAL::SetValue_pval(PVAL valp, bool chktype)
  {
  if (chktype && Type != valp->GetType())
    return true;

  if (Pdtp && !valp->IsTypeNum()) {
    int   ndv;
    int  dval[6];

    ndv = ExtractDate(valp->GetCharValue(), Pdtp, DefYear, dval);
    MakeDate(NULL, dval, ndv);
  } else
    Ival = valp->GetIntValue();

  return false;
  } // end of SetValue

/***********************************************************************/
/*  SetValue: convert chars extracted from a line to date value.       */
/***********************************************************************/
void DTVAL::SetValue_char(char *p, int n)
  {
  if (Pdtp) {
    char *p2;
    int   ndv;
    int  dval[6];

    // Trim trailing blanks
    for (p2 = p + n -1; p < p2 && *p2 == ' '; p2--) ;

    n = min(p2 - p + 1, Len);
    memcpy(Sdate, p, n);
    Sdate[n] = '\0';

    ndv = ExtractDate(Sdate, Pdtp, DefYear, dval);
    MakeDate(NULL, dval, ndv);

		if (trace)
			htrc(" setting date: '%s' -> %d\n", Sdate, Ival);

  } else
    INTVAL::SetValue_char(p, n);

  } // end of SetValue

/***********************************************************************/
/*  SetValue: convert a char string to date value.                     */
/***********************************************************************/
void DTVAL::SetValue_psz(PSZ p)
  {
  if (Pdtp) {
    int   ndv;
    int  dval[6];

    strncpy(Sdate, p, Len);
    Sdate[Len] = '\0';

    ndv = ExtractDate(Sdate, Pdtp, DefYear, dval);
    MakeDate(NULL, dval, ndv);

		if (trace)
			htrc(" setting date: '%s' -> %d\n", Sdate, Ival);

  } else
    INTVAL::SetValue_psz(p);

  } // end of SetValue

/***********************************************************************/
/*  DTVAL SetValue: set value with a value extracted from a block.     */
/***********************************************************************/
void DTVAL::SetValue_pvblk(PVBLK blk, int n)
  {
  if (Pdtp && !::IsTypeNum(blk->GetType())) {
    int   ndv;
    int  dval[6];

    ndv = ExtractDate(blk->GetCharValue(n), Pdtp, DefYear, dval);
    MakeDate(NULL, dval, ndv);
  } else
    Ival = blk->GetIntValue(n);

  } // end of SetValue

/***********************************************************************/
/*  DTVAL GetCharString: get string representation of a date value.    */
/***********************************************************************/
char *DTVAL::GetCharString(char *p)
  {
  if (Pdtp) {
    size_t n = 0;
    struct tm *ptm = GetGmTime();

    if (ptm)
      n = strftime(Sdate, Len + 1, Pdtp->OutFmt, ptm);

    if (!n) {
      *Sdate = '\0';
      strncat(Sdate, "Error", Len + 1);
      } // endif n

    return Sdate;
  } else
    sprintf(p, "%d", Ival);

  return p;
  } // end of GetCharString

/***********************************************************************/
/*  DTVAL ShowValue: get string representation of a date value.        */
/***********************************************************************/
char *DTVAL::ShowValue(char *buf, int len)
  {
  if (Pdtp) {
    char  *p;
    size_t m, n = 0;
    struct tm *ptm = GetGmTime();

    if (Len < len) {
      p = buf;
      m = len;
    } else {
      p = Sdate;
      m = Len + 1;
    } // endif Len

    if (ptm)
      n = strftime(p, m, Pdtp->OutFmt, ptm);

    if (!n) {
      *p = '\0';
      strncat(p, "Error", m);
      } // endif n

    return p;
  } else
    return INTVAL::ShowValue(buf, len);

  } // end of ShowValue

/***********************************************************************/
/*  Compute a function on a date time stamp.                           */
/***********************************************************************/
bool DTVAL::Compute(PGLOBAL g, PVAL *vp, int np, OPVAL op)
  {
  bool rc = false;

  if (op == OP_DATE) {
    int val[6];
    int  nval = min(np, 6);

    for (int i = 0; i < nval; i++)
      val[i] = vp[i]->GetIntValue();

    rc = MakeDate(g, val, nval);
  } else if (op == OP_ADDAY || op == OP_ADDMTH ||
             op == OP_ADDYR || op == OP_NXTDAY) {
    struct tm *ptm;
    int        n = (op != OP_NXTDAY) ? (int)vp[1]->GetIntValue() : 1;

    INTVAL::SetValue_pval(vp[0], true);
    Ival -= Shift;
    ptm = GetGmTime();

    switch (op) {
      case OP_ADDAY:
      case OP_NXTDAY:
        ptm->tm_mday += n;
        break;
      case OP_ADDMTH:
        ptm->tm_mon += n;
        break;
      case OP_ADDYR:
        ptm->tm_year += n;
        break;
      default:
        sprintf(g->Message, MSG(BAD_DATE_OPER), op);
        return true;
      } // endswitch op

    if (MakeTime(ptm)) {
      strcpy(g->Message, MSG(BAD_DATETIME));
      rc = true;
      } // endif MakeTime

  } else if (op == OP_SYSDT) {
		Ival = (int)time(NULL) - Shift;
  } else if (op == OP_CURDT) {
		Ival = (((int)time(NULL) - Shift) / 86400) * 86400;
  } else
    rc = INTVAL::Compute(g, vp, np, op);

  return rc;
  } // end of Compute

/***********************************************************************/
/*  GetTime: extract the time info from a date stamp.                  */
/***********************************************************************/
int DTVAL::GetTime(PGLOBAL g, PVAL *vp, int np)
  {
  return (Ival % 86400);
  } // end of GetTime

/***********************************************************************/
/*  Returns a member of the struct tm representation of the date.      */
/***********************************************************************/
bool DTVAL::GetTmMember(OPVAL op, int& mval)
  {
  bool       rc = false;
  struct tm *ptm = GetGmTime();

  switch (op) {
    case OP_MDAY:  mval = ptm->tm_mday;        break;
    case OP_MONTH: mval = ptm->tm_mon  + 1;    break;
    case OP_YEAR:  mval = ptm->tm_year + 1900; break;
    case OP_WDAY:  mval = ptm->tm_wday + 1;    break;
    case OP_YDAY:  mval = ptm->tm_yday + 1;    break;
    case OP_QUART: mval = ptm->tm_mon / 3 + 1; break;
    default:
      rc = true;
    } // endswitch op

  return rc;
  } // end of GetTmMember

/***********************************************************************/
/*  Calculates the week number of the year for the internal date value.*/
/*  The International Standard ISO 8601 has decreed that Monday shall  */
/*  be the first day of the week. A week that lies partly in one year  */
/*  and partly in another is assigned a number in the year in which    */
/*  most of its days lie. That means that week number 1 of any year is */
/*  the week that contains the January 4th.                            */
/***********************************************************************/
bool DTVAL::WeekNum(PGLOBAL g, int& nval)
  {
  // w is the start of the week SUN=0, MON=1, etc.
  int        m, n, w = nval % 7;
  struct tm *ptm = GetGmTime();

  // Which day is January 4th of this year?
  m = (367 + ptm->tm_wday - ptm->tm_yday) % 7;

  // When does the first week begins?
  n = 3 - (7 + m - w) % 7;

  // Now calculate the week number
  if (!(nval = (7 + ptm->tm_yday - n) / 7))
    nval = 52;

  // Everything should be Ok
  return false;
  } // end of WeekNum

/***********************************************************************/
/*  This covers days, months and years between two dates.              */
/***********************************************************************/
bool DTVAL::DateDiff(DTVAL *dtp, OPVAL op, int& tdif)
  {
  bool      rc = false;
  int      lv1, lv2, t1, t2;
  int       s = CompareValue(dtp);
  struct tm dat1, dat2, *ptm = dtp->GetGmTime();

  if (!ptm)
    return true;

  if (s == 0) {
    // Dates are equal
    tdif = 0;
    return rc;
  } else if (s > 0) {
    // This Date is greater than dtp->Date
    dat1 = *ptm;
    lv1 = dtp->GetIntValue();
    lv2 = Ival;

    if ((ptm = GetGmTime()))
      dat2 = *ptm;

  } else {
    // This Date is less than dtp->Date
    dat2 = *ptm;
    lv2 = dtp->GetIntValue();
    lv1 = Ival;

    if ((ptm = GetGmTime()))
      dat1 = *ptm;

  } // endif's s

  if (!ptm)
    return true;

  // Both dates are valid and dat2 is greater than dat1
  t1 = lv1 % 86400; if (t1 < 0) t1 += 86400;
  t2 = lv2 % 86400; if (t2 < 0) t2 += 86400;

  if (t1 > t2) {
    lv1 += 86400;
    dat1.tm_mday++;
    } // endif

  if (dat1.tm_mday > dat2.tm_mday)
    dat1.tm_mon++;

  switch (op) {
    case OP_DBTWN:
      tdif = (lv2 / 86400) - (lv1 / 86400);
      break;
    case OP_MBTWN:
      tdif = (dat2.tm_year - dat1.tm_year) * 12
           + (dat2.tm_mon  - dat1.tm_mon);
      break;
    case OP_YBTWN:
      if (dat1.tm_mon > dat2.tm_mon)
        dat1.tm_year++;

      tdif = dat2.tm_year - dat1.tm_year;
      break;
    default:
      rc = true;
    } // endswitch op

  if (!rc && s < 0)
    tdif = -tdif;

  return rc;
  } // end of DateDiff

/***********************************************************************/
/*  FormatValue: This function set vp (a STRING value) to the string   */
/*  constructed from its own value formated using the fmt format.      */
/*  This function assumes that the format matches the value type.      */
/***********************************************************************/
bool DTVAL::FormatValue(PVAL vp, char *fmt)
  {
  char      *buf = (char*)vp->GetTo_Val();       // Should be big enough
  struct tm *ptm = GetGmTime();

	if (trace)
		htrc("FormatValue: ptm=%p len=%d\n", ptm, vp->GetValLen());

	if (ptm) {
		size_t n = strftime(buf, vp->GetValLen(), fmt, ptm);

		if (trace)
			htrc("strftime: n=%d buf=%s\n", n, (n) ? buf : "???");

		return (n == 0);
	} else
	  return true;

  } // end of FormatValue


/* -------------------------- Class DFVAL ---------------------------- */

/***********************************************************************/
/*  DFVAL  public constructor from char.                               */
/***********************************************************************/
DFVAL::DFVAL(PSZ s, int prec) : VALUE(TYPE_FLOAT)
  {
  Fval = atof(s);
  Prec = prec;
  Clen = sizeof(double);
  } // end of DFVAL constructor

/***********************************************************************/
/*  DFVAL  public constructor from short.                               */
/***********************************************************************/
DFVAL::DFVAL(short n, int prec) : VALUE(TYPE_FLOAT)
  {
  Fval = (double)n;
  Prec = prec;
  Clen = sizeof(double);
  } // end of DFVAL constructor

/***********************************************************************/
/*  DFVAL  public constructor from int.                               */
/***********************************************************************/
DFVAL::DFVAL(int n, int prec) : VALUE(TYPE_FLOAT)
  {
  Fval = (double)n;
  Prec = prec;
  Clen = sizeof(double);
  } // end of DFVAL constructor

/***********************************************************************/
/*  DFVAL  public constructor from double.                             */
/***********************************************************************/
DFVAL::DFVAL(double f, int prec) : VALUE(TYPE_FLOAT)
  {
  Fval = f;
  Prec = prec;
  Clen = sizeof(double);
  } // end of DFVAL constructor

/***********************************************************************/
/*  DFVAL GetValLen: returns the print length of the double object.    */
/***********************************************************************/
int DFVAL::GetValLen(void)
  {
  char c[32];

  return sprintf(c, "%.*lf", Prec, Fval);
  } // end of GetValLen

/***********************************************************************/
/*  DFVAL SetValue: copy the value of another Value object.            */
/*  This function allows conversion if chktype is false.               */
/***********************************************************************/
bool DFVAL::SetValue_pval(PVAL valp, bool chktype)
  {
  if (chktype && Type != valp->GetType())
    return true;

  Fval = valp->GetFloatValue();
  return false;
  } // end of SetValue

/***********************************************************************/
/*  SetValue: convert chars extracted from a line to double value.     */
/***********************************************************************/
void DFVAL::SetValue_char(char *p, int n)
  {
  char *p2, buf[32];

  for (p2 = p + n; p < p2 && *p == ' '; p++) ;

  n = min(p2 - p, 31);
  memcpy(buf, p, n);
  buf[n] = '\0';
  Fval = atof(buf);

	if (trace)
		htrc(" setting double: '%s' -> %lf\n", buf, Fval);

  } // end of SetValue

/***********************************************************************/
/*  DFVAL SetValue: fill a double float value from a string.           */
/***********************************************************************/
void DFVAL::SetValue_psz(PSZ s)
  {
  Fval = atof(s);
  } // end of SetValue

/***********************************************************************/
/*  DFVAL SetValue: set value with a double extracted from a block.    */
/***********************************************************************/
void DFVAL::SetValue_pvblk(PVBLK blk, int n)
  {
  Fval = blk->GetFloatValue(n);
  } // end of SetValue

/***********************************************************************/
/*  SetBinValue: with bytes extracted from a line.                     */
/***********************************************************************/
void DFVAL::SetBinValue(void *p)
  {
  Fval = *(double *)p;
  } // end of SetBinValue

/***********************************************************************/
/*  GetBinValue: fill a buffer with the internal binary value.         */
/*  This function checks whether the buffer length is enough and       */
/*  returns true if not. Actual filling occurs only if go is true.     */
/*  Currently used by WriteColumn of binary files.                     */
/***********************************************************************/
bool DFVAL::GetBinValue(void *buf, int buflen, bool go)
  {
  // Test on length was removed here until a variable in column give the
  // real field length. For BIN files the field length logically cannot
  // be different from the variable length because no conversion is done.
  // Therefore this test is useless anyway.
//#if defined(_DEBUG)
//  if (sizeof(double) > buflen)
//    return true;
//#endif

  if (go)
    *(double *)buf = Fval;

  return false;
  } // end of GetBinValue

/***********************************************************************/
/*  GetBinValue: used by SELECT when called from QUERY and KINDEX.     */
/*  This is a fast implementation that does not do any checking.       */
/*  Note: type is not needed here and just kept for compatibility.     */
/***********************************************************************/
void DFVAL::GetBinValue(void *buf, int buflen)
  {
  assert(buflen == sizeof(double));

  *(double *)buf = Fval;
  } // end of GetBinValue

/***********************************************************************/
/*  DFVAL ShowValue: get string representation of a double value.      */
/***********************************************************************/
char *DFVAL::ShowValue(char *buf, int len)
  {
	// TODO: use snprintf to avoid possible overflow
  sprintf(buf, "%*.*lf", len, Prec, Fval);
  return buf;
  } // end of ShowValue

/***********************************************************************/
/*  DFVAL GetCharString: get string representation of a double value.  */
/***********************************************************************/
char *DFVAL::GetCharString(char *p)
  {
  sprintf(p, "%.*lf", Prec, Fval);
  return p;
  } // end of GetCharString

/***********************************************************************/
/*  DFVAL GetShortString: get short representation of a double value.  */
/***********************************************************************/
char *DFVAL::GetShortString(char *p, int n)
  {
  sprintf(p, "%*hd", n, (short)Fval);
  return p;
  } // end of GetShortString

/***********************************************************************/
/*  DFVAL GetIntString: get int representation of a double value.    */
/***********************************************************************/
char *DFVAL::GetIntString(char *p, int n)
  {
  sprintf(p, "%*ld", n, (int)Fval);
  return p;
  } // end of GetIntString

/***********************************************************************/
/*  DFVAL GetFloatString: get double representation of a double value. */
/***********************************************************************/
char *DFVAL::GetFloatString(char *p, int n, int prec)
  {
  sprintf(p, "%*.*lf", n, (prec < 0) ? Prec : prec, Fval);
  return p;
  } // end of GetFloatString

/***********************************************************************/
/*  DFVAL compare value with another Value.                            */
/***********************************************************************/
bool DFVAL::IsEqual(PVAL vp, bool chktype)
  {
  if (this == vp)
    return true;
  else if (chktype && Type != vp->GetType())
    return false;
  else
    return (Fval == vp->GetFloatValue());

  } // end of IsEqual

/***********************************************************************/
/*  Compare values and returns 1, 0 or -1 according to comparison.     */
/*  This function is used for evaluation of double float filters.      */
/***********************************************************************/
int DFVAL::CompareValue(PVAL vp)
  {
//assert(vp->GetType() == Type);

  // Process filtering on int integers.
  double d = vp->GetFloatValue();

	if (trace)
		htrc(" Comparing: val=%.2f,%.2f\n", Fval, d);

  return (Fval > d) ? 1 : (Fval < d) ? (-1) : 0;
  } // end of CompareValue

/***********************************************************************/
/*  Compute a function on double floats.                               */
/***********************************************************************/
bool DFVAL::Compute(PGLOBAL g, PVAL *vp, int np, OPVAL op)
  {
  double val[2];

  assert(np <= 2);

  for (int i = 0; i < np; i++)
    val[i] = vp[i]->GetFloatValue();

  switch (op) {
    case OP_ABS:
      assert(np == 1);
      Fval = fabs(*val);
      break;
    case OP_CEIL:
      assert(np == 1);
      Fval = ceil(*val);
      break;
    case OP_FLOOR:
      assert(np == 1);
      Fval = floor(*val);
      break;
    case OP_SIGN:
      assert(np == 1);
      Fval = (*val < 0.0) ? (-1.0) : 1.0;
      break;
    case OP_ADD:
      assert(np == 2);
      Fval = val[0] + val[1];
      break;
    case OP_SUB:
      assert(np == 2);
      Fval = val[0] - val[1];
      break;
    case OP_MULT:
      assert(np == 2);
      Fval = val[0] * val[1];
      break;
    case OP_MIN:
      assert(np == 2);
      Fval = min(val[0], val[1]);
      break;
    case OP_MAX:
      assert(np == 2);
      Fval = max(val[0], val[1]);
      break;
    case OP_DIV:
      assert(np == 2);
      if (!val[1]) {
        strcpy(g->Message, MSG(ZERO_DIVIDE));
        return true;
        } // endif

      Fval = val[0] / val[1];
      break;
    case OP_MOD:
      assert(np == 2);
      Fval = fmod(val[0], val[1]);
      break;
    case OP_SQRT:
      assert(np == 1);
      Fval = sqrt(*val);
      break;
    case OP_LN:
      assert(np == 1);
      Fval = log(*val);
      break;
    case OP_EXP:
      assert(np == 1);
      Fval = exp(*val);
      break;
    case OP_COS:
      assert(np == 1);
      Fval = cos(*val);
      break;
    case OP_SIN:
      assert(np == 1);
      Fval = sin(*val);
      break;
    case OP_TAN:
      assert(np == 1);
      Fval = tan(*val);
      break;
    case OP_COSH:
      assert(np == 1);
      Fval = cosh(*val);
      break;
    case OP_SINH:
      assert(np == 1);
      Fval = sinh(*val);
      break;
    case OP_TANH:
      assert(np == 1);
      Fval = tanh(*val);
      break;
    case OP_LOG:
      assert(np > 0);

      if (np > 1 && val[1] != 10.0) {
        strcpy(g->Message, MSG(ONLY_LOG10_IMPL));
        return true;
        } // endif Numarg

      Fval = log10(val[0]);
      break;
    case OP_POWER:
      assert(np == 2);
      Fval = pow(val[0], val[1]);
      break;
    case OP_ROUND:
      assert(np > 0);

      if (np > 1) {
        double dx, dy = val[1];

        modf(dy, &dx);                 // Get integral part of arg
        dx = pow(10.0, dx);
        modf(val[0] * dx + 0.5, &dy);
        Fval = dy / dx;
      } else
        modf(val[0] + 0.5, &Fval);

      break;
    case OP_DELTA:
//    assert(np == 1);
      Fval = val[0] - Fval;
      break;
    default:
      sprintf(g->Message, MSG(BAD_EXP_OPER), op);
      return true;
    } // endswitch op

	if (trace)
		if (np == 1)
			htrc("Compute result=%lf val=%lf op=%d\n", Fval, val[0], op);
		else
			htrc("Compute result=%lf val=%lf,%lf op=%d\n",
						Fval, val[0], val[1], op);

  return false;
  } // end of Compute

/***********************************************************************/
/*  GetTime: convert HR/MIN/SEC in a number of seconds.                */
/***********************************************************************/
int DFVAL::GetTime(PGLOBAL g, PVAL *vp, int np)
  {
  double sec = Fval;

  for (int i = 0; i < 2; i++) {
    sec *= 60.0;

    if (np > i)
      sec += vp[i]->GetFloatValue();

    } // endfor i

  return (int)sec;
  } // end of GetTime

/***********************************************************************/
/*  Divide: used by aggregate functions when calculating average.      */
/***********************************************************************/
void DFVAL::Divide(int cnt)
  {
  Fval /= (double)cnt;
  } // end of Divide

/***********************************************************************/
/*  StdVar: used by aggregate functions for Stddev and Variance.       */
/***********************************************************************/
void DFVAL::StdVar(PVAL vp, int cnt, bool b)
  {
  double fv2 = vp->GetFloatValue();
  double cnd = (double)cnt;

  Fval = (cnt == 1) ? 0.0 : ((fv2 - (Fval * Fval) / cnd) / (cnd - 1.0));

  if (b)    // Builtin == FNC_STDDEV
    Fval = sqrt(Fval);

  } // end of StdVar

/***********************************************************************/
/*  Times: used by aggregate functions for Stddev and Variance.        */
/***********************************************************************/
void DFVAL::Times(PVAL vp)
  {
  Fval *= vp->GetFloatValue();
  } // end of Times

/***********************************************************************/
/*  Add: used by aggregate functions for Sum and other functions.      */
/***********************************************************************/
void DFVAL::Add(PVAL vp)
  {
  Fval += vp->GetFloatValue();
  } // end of Add

/***********************************************************************/
/*  Add: used by aggregate functions for Sum and other functions.      */
/***********************************************************************/
void DFVAL::Add(PVBLK vbp, int i)
  {
  Fval += vbp->GetFloatValue(i);
  } // end of Add

/***********************************************************************/
/*  Add: used by aggregate functions for Sum and other functions.      */
/***********************************************************************/
void DFVAL::Add(PVBLK vbp, int j, int k)
  {
  CheckType(vbp)
  double *dp = (double *)vbp->GetValPointer();

  for (register int i = j; i < k; i++)
    Fval += dp[i];

  } // end of Add

/***********************************************************************/
/*  Add: used by aggregate functions for Sum and other functions.      */
/***********************************************************************/
void DFVAL::Add(PVBLK vbp, int *x, int j, int k)
  {
  CheckType(vbp)
  double *dp = (double *)vbp->GetValPointer();

  for (register int i = j; i < k; i++)
    Fval += dp[x[i]];

  } // end of Add

/***********************************************************************/
/*  AddSquare: used by aggregate functions for Stddev and Variance.    */
/***********************************************************************/
void DFVAL::AddSquare(PVAL vp)
  {
  double val = vp->GetFloatValue();

  Fval += (val * val);
  } // end of AddSquare

/***********************************************************************/
/*  AddSquare: used by aggregate functions for Stddev and Variance.    */
/***********************************************************************/
void DFVAL::AddSquare(PVBLK vbp, int i)
  {
  double val = vbp->GetFloatValue(i);

  Fval += (val * val);
  } // end of AddSquare

/***********************************************************************/
/*  AddSquare: used by aggregate functions for Stddev and Variance.    */
/***********************************************************************/
void DFVAL::AddSquare(PVBLK vbp, int j, int k)
  {
  CheckType(vbp)
  double *dp = (double *)vbp->GetValPointer();

  for (register int i = j; i < k; i++)
    Fval += (dp[i] * dp[i]);

  } // end of AddSquare

/***********************************************************************/
/*  FormatValue: This function set vp (a STRING value) to the string   */
/*  constructed from its own value formated using the fmt format.      */
/*  This function assumes that the format matches the value type.      */
/***********************************************************************/
bool DFVAL::FormatValue(PVAL vp, char *fmt)
  {
  char *buf = (char*)vp->GetTo_Val();        // Should be big enough
  int   n = sprintf(buf, fmt, Fval);

  return (n > vp->GetValLen());
  } // end of FormatValue

/***********************************************************************/
/*  SetMin: used by the aggregate function MIN.                        */
/***********************************************************************/
void DFVAL::SetMin(PVAL vp)
  {
  double val = vp->GetFloatValue();

  if (val < Fval)
    Fval = val;

  } // end of SetMin

/***********************************************************************/
/*  SetMin: used by QUERY for the aggregate function MIN.              */
/***********************************************************************/
void DFVAL::SetMin(PVBLK vbp, int i)
  {
  double val = vbp->GetFloatValue(i);

  if (val < Fval)
    Fval = val;

  } // end of SetMin

/***********************************************************************/
/*  SetMin: used by QUERY for the aggregate function MIN.              */
/***********************************************************************/
void DFVAL::SetMin(PVBLK vbp, int j, int k)
  {
  CheckType(vbp)
  double *dp = (double *)vbp->GetValPointer();

  for (register int i = j; i < k; i++)
    if (dp[i] < Fval)
      Fval = dp[i];

  } // end of SetMin

/***********************************************************************/
/*  SetMax: used by QUERY for the aggregate function MIN.              */
/***********************************************************************/
void DFVAL::SetMin(PVBLK vbp, int *x, int j, int k)
  {
  CheckType(vbp)
  register double val;
  double *dp = (double *)vbp->GetValPointer();

  for (register int i = j; i < k; i++) {
    val = dp[x[i]];

    if (val < Fval)
      Fval = val;

    } // endfor i

  } // end of SetMin

/***********************************************************************/
/*  SetMax: used by the aggregate function MAX.                        */
/***********************************************************************/
void DFVAL::SetMax(PVAL vp)
  {
  double val = vp->GetFloatValue();

  if (val > Fval)
    Fval = val;

  } // end of SetMax

/***********************************************************************/
/*  SetMax: used by QUERY for the aggregate function MAX.              */
/***********************************************************************/
void DFVAL::SetMax(PVBLK vbp, int i)
  {
  double val = vbp->GetFloatValue(i);

  if (val > Fval)
    Fval = val;

  } // end of SetMax

/***********************************************************************/
/*  SetMax: used by QUERY for the aggregate function MIN.              */
/***********************************************************************/
void DFVAL::SetMax(PVBLK vbp, int j, int k)
  {
  CheckType(vbp)
  double *dp = (double *)vbp->GetValPointer();

  for (register int i = j; i < k; i++)
    if (dp[i] > Fval)
      Fval = dp[i];

  } // end of SetMax

/***********************************************************************/
/*  SetMax: used by QUERY for the aggregate function MIN.              */
/***********************************************************************/
void DFVAL::SetMax(PVBLK vbp, int *x, int j, int k)
  {
  CheckType(vbp)
  register double val;
  double *dp = (double *)vbp->GetValPointer();

  for (register int i = j; i < k; i++) {
    val = dp[x[i]];

    if (val > Fval)
      Fval = val;

    } // endfor i

  } // end of SetMax

/***********************************************************************/
/*  DFVAL  SetFormat function (used to set SELECT output format).      */
/***********************************************************************/
bool DFVAL::SetConstFormat(PGLOBAL g, FORMAT& fmt)
  {
  char c[32];

  fmt.Type[0] = 'F';
  fmt.Length = sprintf(c, "%.*lf", Prec, Fval);
  fmt.Prec = Prec;
  return false;
  } // end of SetConstFormat

/***********************************************************************/
/*  Make file output of a double object.                               */
/***********************************************************************/
void DFVAL::Print(PGLOBAL g, FILE *f, uint n)
  {
  char m[64];

  memset(m, ' ', n);                             /* Make margin string */
  m[n] = '\0';

  fprintf(f, "%s%.*lf\n", m, Prec, Fval);
  } /* end of Print */

/***********************************************************************/
/*  Make string output of a double object.                             */
/***********************************************************************/
void DFVAL::Print(PGLOBAL g, char *ps, uint z)
  {
  sprintf(ps, "%.*lf", Prec, Fval);
  } /* end of Print */

#endif // __VALUE_H

/* -------------------------- End of Value --------------------------- */
