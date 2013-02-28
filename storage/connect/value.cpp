/************* Value C++ Functions Source Code File (.CPP) *************/
/*  Name: VALUE.CPP  Version 2.1                                       */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2001-2013    */
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
/*  Currently the only implemented types are STRING, INT, DOUBLE, DATE */
/*  and LONGLONG. Shortly we should add at least TINY and VARCHAR.     */
/***********************************************************************/

/***********************************************************************/
/*  Include relevant MariaDB header file.                              */
/***********************************************************************/
#include "my_global.h"
#if defined(WIN32)
//#include <windows.h>
#else   // !WIN32
#include <string.h>
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
    case TYPE_BIGINT: name = "BIGINT";   break;
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
    case TYPE_INT:    len = sizeof(int);        break;
    case TYPE_BIGINT: len = sizeof(longlong);   break;
    case TYPE_DATE:   len = sizeof(int);        break;
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
    case TYPE_INT:    tp = DB_INT;    break;
    case TYPE_BIGINT:
    case TYPE_FLOAT:  tp = DB_DOUBLE; break;
    case TYPE_DATE:   tp = DB_DATE;   break;
    default:          tp = DB_ERROR;
    } // endswitch type

  return tp;
  } // end of GetPLGType

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
    case TYPE_BIGINT: c = "L"; break;
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
    case 'N': type = TYPE_INT;    break;
    case 'L': type = TYPE_BIGINT; break;
    case 'F': type = TYPE_FLOAT;  break;
    case 'D': type = TYPE_DATE;   break;
    } // endswitch type

  return type;
  } // end of GetFormatType


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
    case TYPE_BIGINT:
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

      return (target == TYPE_FLOAT  || type == TYPE_FLOAT)  ? TYPE_FLOAT
           : (target == TYPE_DATE   || type == TYPE_DATE)   ? TYPE_DATE
           : (target == TYPE_BIGINT || type == TYPE_BIGINT) ? TYPE_BIGINT
           : (target == TYPE_INT    || type == TYPE_INT)    ? TYPE_INT
                                                            : TYPE_SHORT;
    default:
      if (target == TYPE_ERROR || target == type)
        return type;

      if (match && ((IsTypeChar(target) && !IsTypeChar(type)) ||
                    (IsTypeNum(target) && !IsTypeNum(type))))
        return TYPE_ERROR;

      return (target == TYPE_FLOAT  || type == TYPE_FLOAT)  ? TYPE_FLOAT
           : (target == TYPE_DATE   || type == TYPE_DATE)   ? TYPE_DATE
           : (target == TYPE_BIGINT || type == TYPE_BIGINT) ? TYPE_BIGINT
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
    case TYPE_STRING:
      valp = new(g) TYPVAL<PSZ>((PSZ)value);
      break;
    case TYPE_SHORT:
      valp = new(g) TYPVAL<short>(*(short*)value, TYPE_SHORT);
      break;
    case TYPE_INT: 
      valp = new(g) TYPVAL<int>(*(int*)value, TYPE_INT);
      break;
    case TYPE_BIGINT:
      valp = new(g) TYPVAL<longlong>(*(longlong*)value, TYPE_BIGINT);
      break;
    case TYPE_FLOAT:
      valp = new(g) TYPVAL<double>(*(double *)value, TYPE_FLOAT);
      break;
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
    case TYPE_STRING:
      valp = new(g) TYPVAL<PSZ>(g, (PSZ)NULL, len, prec);
      break;
    case TYPE_DATE: 
      valp = new(g) DTVAL(g, len, prec, dom);
      break;
    case TYPE_INT: 
      valp = new(g) TYPVAL<int>((int)0, TYPE_INT);
      break;
    case TYPE_BIGINT:
      valp = new(g) TYPVAL<longlong>((longlong)0, TYPE_BIGINT);
      break;
    case TYPE_SHORT:
      valp = new(g) TYPVAL<short>((short)0, TYPE_SHORT);
      break;
    case TYPE_FLOAT:
      valp = new(g) TYPVAL<double>(0.0, prec, TYPE_FLOAT);
      break;
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

      valp = new(g) TYPVAL<PSZ>(g, p, valp->GetValLen(), valp->GetValPrec());
      break;
    case TYPE_SHORT:  
      valp = new(g) TYPVAL<short>(valp->GetShortValue(), TYPE_SHORT);
      break;
    case TYPE_INT: 
      valp = new(g) TYPVAL<int>(valp->GetIntValue(), TYPE_INT);
      break;
    case TYPE_BIGINT: 
      valp = new(g) TYPVAL<longlong>(valp->GetBigintValue(), TYPE_BIGINT);
      break;
    case TYPE_DATE:
      valp = new(g) DTVAL(g, valp->GetIntValue());
      break;
    case TYPE_FLOAT:
      valp = new(g) TYPVAL<double>(valp->GetFloatValue(), TYPE_FLOAT);
      break;
    default:
      sprintf(g->Message, MSG(BAD_VALUE_TYPE), newtype);
      return NULL;
    } // endswitch type

  valp->SetGlobal(g);
  return valp;
  } // end of AllocateValue


/* -------------------------- Class VALUE ---------------------------- */

/***********************************************************************/
/*  Class VALUE protected constructor.                                 */
/***********************************************************************/
VALUE::VALUE(int type) : Type(type)
  {
  Fmt = GetFmt();
  Xfmt = GetXfmt();
  Null = false;
  Nullable = false; 
  Clen = 0;
  Prec = 0;
  } // end of VALUE constructor

/***********************************************************************/
/*  VALUE GetFmt: returns the format to use with typed value.          */
/***********************************************************************/
const char *VALUE::GetFmt(void)
  {
  const char *fmt = "%d";;

  switch (Type) {
    case TYPE_STRING: fmt = "%s";    break;
    case TYPE_SHORT:  fmt = "%hd";   break;
    case TYPE_BIGINT: fmt = "%lld";  break;
    case TYPE_FLOAT:  fmt = "%.*lf"; break;
    } // endswitch Type

  return fmt;
  } // end of GetFmt

/***********************************************************************/
/* VALUE GetXfmt: returns the extended format to use with typed value. */
/***********************************************************************/
const char *VALUE::GetXfmt(void)
  {
  const char *fmt = "%*d";;

  switch (Type) {
    case TYPE_STRING: fmt = "%*s";    break;
    case TYPE_SHORT:  fmt = "%*hd";   break;
    case TYPE_BIGINT: fmt = "%*lld";  break;
    case TYPE_FLOAT:  fmt = "%*.*lf"; break;
    } // endswitch Type

  return fmt;
  } // end of GetFmt

/* -------------------------- Class TYPVAL ---------------------------- */

/***********************************************************************/
/*  TYPVAL  public constructor from a constant typed value.            */
/***********************************************************************/
template <class TYPE>
TYPVAL<TYPE>::TYPVAL(TYPE n, int type) : VALUE(type)
  {
  Tval = n;
  Clen = sizeof(TYPE);
  Prec = (Type == TYPE_FLOAT) ? 2 : 0;
  } // end of TYPVAL constructor

/***********************************************************************/
/*  TYPVAL  public constructor from typed value.                       */
/***********************************************************************/
template <class TYPE>
TYPVAL<TYPE>::TYPVAL(TYPE n, int prec, int type) : VALUE(type)
  {
  assert(Type == TYPE_FLOAT);
  Tval = n;
  Clen = sizeof(TYPE);
  Prec = prec;
  } // end of TYPVAL constructor

/***********************************************************************/
/*  TYPVAL GetValLen: returns the print length of the typed object.    */
/***********************************************************************/
template <class TYPE>
int TYPVAL<TYPE>::GetValLen(void)
  {
  char c[32];

  return sprintf(c, Fmt, Tval);
  } // end of GetValLen

template <>
int TYPVAL<double>::GetValLen(void)
  {
  char c[32];

  return sprintf(c, Fmt, Prec, Tval);
  } // end of GetValLen

/***********************************************************************/
/*  TYPVAL SetValue: copy the value of another Value object.           */
/*  This function allows conversion if chktype is false.               */
/***********************************************************************/
template <class TYPE>
bool TYPVAL<TYPE>::SetValue_pval(PVAL valp, bool chktype)
  {
  if (chktype && Type != valp->GetType())
    return true;

  if (!(Null = valp->IsNull() && Nullable))
//  Tval = (TYPE)valp->GetBigintValue();
    Tval = GetTypedValue(valp);
  else
    Reset();

  return false;
  } // end of SetValue

template <>
short TYPVAL<short>::GetTypedValue(PVAL valp)
  {return valp->GetShortValue();}

template <>
int TYPVAL<int>::GetTypedValue(PVAL valp)
  {return valp->GetIntValue();}

template <>
longlong TYPVAL<longlong>::GetTypedValue(PVAL valp)
  {return valp->GetBigintValue();}

template <>
double TYPVAL<double>::GetTypedValue(PVAL valp)
  {return valp->GetFloatValue();}

/***********************************************************************/
/*  TYPVAL SetValue: convert chars extracted from a line to TYPE value.*/
/***********************************************************************/
template <class TYPE>
void TYPVAL<TYPE>::SetValue_char(char *p, int n)
  {
  char *p2, buf[32];
  bool  minus;

  for (p2 = p + n; p < p2 && *p == ' '; p++) ;

  for (Tval = 0, minus = false; p < p2; p++)
    switch (*p) {
      case '-':
        minus = true;
      case '+':
        break;
      case '0': Tval = Tval * 10;     break;
      case '1': Tval = Tval * 10 + 1; break;
      case '2': Tval = Tval * 10 + 2; break;
      case '3': Tval = Tval * 10 + 3; break;
      case '4': Tval = Tval * 10 + 4; break;
      case '5': Tval = Tval * 10 + 5; break;
      case '6': Tval = Tval * 10 + 6; break;
      case '7': Tval = Tval * 10 + 7; break;
      case '8': Tval = Tval * 10 + 8; break;
      case '9': Tval = Tval * 10 + 9; break;
      default:
        p = p2;
      } // endswitch *p

  if (minus && Tval)
    Tval = - Tval;

  if (trace)
    htrc(strcat(strcat(strcpy(buf, " setting %s to: "), Fmt), "\n"),
                              GetTypeName(Type), Tval);

  Null = false;
  } // end of SetValue

template <>
void TYPVAL<double>::SetValue_char(char *p, int n)
  {
  char *p2, buf[32];

  for (p2 = p + n; p < p2 && *p == ' '; p++) ;

  n = min(p2 - p, 31);
  memcpy(buf, p, n);
  buf[n] = '\0';
  Tval = atof(buf);

  if (trace)
    htrc(" setting double: '%s' -> %lf\n", buf, Tval);

  Null = false;
  } // end of SetValue

/***********************************************************************/
/*  TYPVAL SetValue: fill a typed value from a string.                 */
/***********************************************************************/
template <class TYPE>
void TYPVAL<TYPE>::SetValue_psz(PSZ s)
  {
  Tval = GetTypedValue(s);
  Null = false;
  } // end of SetValue

template <>
int TYPVAL<int>::GetTypedValue(PSZ s) {return atol(s);}
template <>
short TYPVAL<short>::GetTypedValue(PSZ s) {return (short)atoi(s);}
template <>
longlong TYPVAL<longlong>::GetTypedValue(PSZ s) {return atoll(s);}
template <>
double TYPVAL<double>::GetTypedValue(PSZ s) {return atof(s);}


/***********************************************************************/
/*  TYPVAL SetValue: set value with a TYPE extracted from a block.     */
/***********************************************************************/
template <class TYPE>
void TYPVAL<TYPE>::SetValue_pvblk(PVBLK blk, int n)
  {
  Tval = GetTypedValue(blk, n);
  Null = false;
  } // end of SetValue

template <>
int TYPVAL<int>::GetTypedValue(PVBLK blk, int n)
  {return blk->GetIntValue(n);}

template <>
short TYPVAL<short>::GetTypedValue(PVBLK blk, int n)
  {return blk->GetShortValue(n);}

template <>
longlong TYPVAL<longlong>::GetTypedValue(PVBLK blk, int n)
  {return blk->GetBigintValue(n);}

template <>
double TYPVAL<double>::GetTypedValue(PVBLK blk, int n)
  {return blk->GetFloatValue(n);}

/***********************************************************************/
/*  TYPVAL SetBinValue: with bytes extracted from a line.              */
/***********************************************************************/
template <class TYPE>
void TYPVAL<TYPE>::SetBinValue(void *p)
  {
  Tval = *(TYPE *)p;
  Null = false;
  } // end of SetBinValue

/***********************************************************************/
/*  GetBinValue: fill a buffer with the internal binary value.         */
/*  This function checks whether the buffer length is enough and       */
/*  returns true if not. Actual filling occurs only if go is true.     */
/*  Currently used by WriteColumn of binary files.                     */
/***********************************************************************/
template <class TYPE>
bool TYPVAL<TYPE>::GetBinValue(void *buf, int buflen, bool go)
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
    *(TYPE *)buf = Tval;

  Null = false;
  return false;
  } // end of GetBinValue

/***********************************************************************/
/*  TYPVAL ShowValue: get string representation of a typed value.      */
/***********************************************************************/
template <class TYPE>
char *TYPVAL<TYPE>::ShowValue(char *buf, int len)
  {
  sprintf(buf, Xfmt, len, Tval);
  return buf;
  } // end of ShowValue

template <>
char *TYPVAL<double>::ShowValue(char *buf, int len)
  {
  // TODO: use snprintf to avoid possible overflow
  sprintf(buf, Xfmt, len, Prec, Tval);
  return buf;
  } // end of ShowValue

/***********************************************************************/
/*  TYPVAL GetCharString: get string representation of a typed value.  */
/***********************************************************************/
template <class TYPE>
char *TYPVAL<TYPE>::GetCharString(char *p)
  {
  sprintf(p, Fmt, Tval);
  return p;
  } // end of GetCharString

template <>
char *TYPVAL<double>::GetCharString(char *p)
  {
  sprintf(p, Fmt, Prec, Tval);
  return p;
  } // end of GetCharString

/***********************************************************************/
/*  TYPVAL GetShortString: get short representation of a typed value.  */
/***********************************************************************/
template <class TYPE>
char *TYPVAL<TYPE>::GetShortString(char *p, int n)
  {
  sprintf(p, "%*hd", n, (short)Tval);
  return p;
  } // end of GetShortString

/***********************************************************************/
/*  TYPVAL GetIntString: get int representation of a typed value.      */
/***********************************************************************/
template <class TYPE>
char *TYPVAL<TYPE>::GetIntString(char *p, int n)
  {
  sprintf(p, "%*d", n, (int)Tval);
  return p;
  } // end of GetIntString

/***********************************************************************/
/*  TYPVAL GetBigintString: get big int representation of a TYPE value.*/
/***********************************************************************/
template <class TYPE>
char *TYPVAL<TYPE>::GetBigintString(char *p, int n)
  {
  sprintf(p, "%*lld", n, (longlong)Tval);
  return p;
  } // end of GetBigintString

/***********************************************************************/
/*  TYPVAL GetFloatString: get double representation of a typed value. */
/***********************************************************************/
template <class TYPE>
char *TYPVAL<TYPE>::GetFloatString(char *p, int n, int prec)
  {
  sprintf(p, "%*.*lf", n, (prec < 0) ? 2 : prec, (double)Tval);
  return p;
  } // end of GetFloatString

/***********************************************************************/
/*  TYPVAL compare value with another Value.                           */
/***********************************************************************/
template <class TYPE>
bool TYPVAL<TYPE>::IsEqual(PVAL vp, bool chktype)
  {
  if (this == vp)
    return true;
  else if (chktype && Type != vp->GetType())
    return false;
  else if (Null || vp->IsNull())
    return false;
  else
    return (Tval == GetTypedValue(vp));

  } // end of IsEqual

/***********************************************************************/
/*  FormatValue: This function set vp (a STRING value) to the string   */
/*  constructed from its own value formated using the fmt format.      */
/*  This function assumes that the format matches the value type.      */
/***********************************************************************/
template <class TYPE>
bool TYPVAL<TYPE>::FormatValue(PVAL vp, char *fmt)
  {
  char *buf = (char*)vp->GetTo_Val();        // Should be big enough
  int   n = sprintf(buf, fmt, Tval);

  return (n > vp->GetValLen());
  } // end of FormatValue

/***********************************************************************/
/*  TYPVAL  SetFormat function (used to set SELECT output format).     */
/***********************************************************************/
template <class TYPE>
bool TYPVAL<TYPE>::SetConstFormat(PGLOBAL g, FORMAT& fmt)
  {
  char c[32];

  fmt.Type[0] = *GetFormatType(Type);
  fmt.Length = sprintf(c, Fmt, Tval);
  fmt.Prec = Prec;
  return false;
  } // end of SetConstFormat

/***********************************************************************/
/*  Make file output of a typed object.                                */
/***********************************************************************/
template <class TYPE>
void TYPVAL<TYPE>::Print(PGLOBAL g, FILE *f, uint n)
  {
  char m[64], buf[12];

  memset(m, ' ', n);                             /* Make margin string */
  m[n] = '\0';

  if (Null)
    fprintf(f, "%s<null>\n", m);
  else
    fprintf(f, strcat(strcat(strcpy(buf, "%s"), Fmt), "\n"), m, Tval);

  } /* end of Print */

/***********************************************************************/
/*  Make string output of a int object.                                */
/***********************************************************************/
template <class TYPE>
void TYPVAL<TYPE>::Print(PGLOBAL g, char *ps, uint z)
  {
  if (Null)
    strcpy(ps, "<null>");
  else
    sprintf(ps, Fmt, Tval);

  } /* end of Print */

/* -------------------------- Class STRING --------------------------- */

/***********************************************************************/
/*  STRING  public constructor from a constant string.                 */
/***********************************************************************/
TYPVAL<PSZ>::TYPVAL(PSZ s) : VALUE(TYPE_STRING)
  {
  Strp = s;
  Len = strlen(s);
  Clen = Len;
  Ci = false;
  } // end of STRING constructor

/***********************************************************************/
/*  STRING public constructor from char.                               */
/***********************************************************************/
TYPVAL<PSZ>::TYPVAL(PGLOBAL g, PSZ s, int n, int c)
           : VALUE(TYPE_STRING)
  {
  assert(Type == TYPE_STRING && (g || s));
  Len = (g) ? n : strlen(s);

  if (g && !s) {
    Strp = (char *)PlugSubAlloc(g, NULL, Len + 1);
    Strp[Len] = '\0';
  } else
    Strp = s;

  Clen = Len;
  Ci = (c != 0);
  } // end of STRING constructor

/***********************************************************************/
/*  STRING SetValue: copy the value of another Value object.           */
/***********************************************************************/
bool TYPVAL<PSZ>::SetValue_pval(PVAL valp, bool chktype)
  {
  if (chktype && (valp->GetType() != Type || valp->GetSize() > Len))
    return true;

  char buf[32];

  if (!(Null = valp->IsNull() && Nullable))
    strncpy(Strp, valp->GetCharString(buf), Len);
  else
    Reset();

  return false;
  } // end of SetValue_pval

/***********************************************************************/
/*  STRING SetValue: fill string with chars extracted from a line.     */
/***********************************************************************/
void TYPVAL<PSZ>::SetValue_char(char *p, int n)
  {
  n = min(n, Len);
  strncpy(Strp, p, n);

  for (p = Strp + n - 1; (*p == ' ' || *p == '\0') && p >= Strp; p--) ;

  *(++p) = '\0';

  if (trace)
    htrc(" Setting string to: '%s'\n", Strp);

  Null = false;
  } // end of SetValue_char

/***********************************************************************/
/*  STRING SetValue: fill string with another string.                  */
/***********************************************************************/
void TYPVAL<PSZ>::SetValue_psz(PSZ s)
  {
  strncpy(Strp, s, Len);
  Null = false;
  } // end of SetValue_psz

/***********************************************************************/
/*  STRING SetValue: fill string with a string extracted from a block. */
/***********************************************************************/
void TYPVAL<PSZ>::SetValue_pvblk(PVBLK blk, int n)
  {
  strncpy(Strp, blk->GetCharValue(n), Len);
  } // end of SetValue_pvblk

/***********************************************************************/
/*  STRING SetValue: get the character representation of an integer.   */
/***********************************************************************/
void TYPVAL<PSZ>::SetValue(int n)
  {
  char     buf[16];
  PGLOBAL& g = Global;
  int      k = sprintf(buf, "%d", n);

  if (k > Len) {
    sprintf(g->Message, MSG(VALSTR_TOO_LONG), buf, Len);
    longjmp(g->jumper[g->jump_level], 138);
  } else
    SetValue_psz(buf);

  Null = false;
  } // end of SetValue

/***********************************************************************/
/*  STRING SetValue: get the character representation of a short int.  */
/***********************************************************************/
void TYPVAL<PSZ>::SetValue(short i)
  {
  SetValue((int)i);
  Null = false;
  } // end of SetValue

/***********************************************************************/
/*  STRING SetValue: get the character representation of a big integer.*/
/***********************************************************************/
void TYPVAL<PSZ>::SetValue(longlong n)
  {
  char     buf[24];
  PGLOBAL& g = Global;
  int      k = sprintf(buf, "%lld", n);

  if (k > Len) {
    sprintf(g->Message, MSG(VALSTR_TOO_LONG), buf, Len);
    longjmp(g->jumper[g->jump_level], 138);
  } else
    SetValue_psz(buf);

  Null = false;
  } // end of SetValue

/***********************************************************************/
/*  STRING SetValue: get the character representation of a double.     */
/***********************************************************************/
void TYPVAL<PSZ>::SetValue(double f)
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

  Null = false;
  } // end of SetValue

/***********************************************************************/
/*  STRING SetBinValue: fill string with chars extracted from a line.  */
/***********************************************************************/
void TYPVAL<PSZ>::SetBinValue(void *p)
  {
  SetValue_char((char *)p, Len);
  Null = false;
  } // end of SetBinValue

/***********************************************************************/
/*  GetBinValue: fill a buffer with the internal binary value.         */
/*  This function checks whether the buffer length is enough and       */
/*  returns true if not. Actual filling occurs only if go is true.     */
/*  Currently used by WriteColumn of binary files.                     */
/***********************************************************************/
bool TYPVAL<PSZ>::GetBinValue(void *buf, int buflen, bool go)
  {
  int len = (Null) ? 0 : strlen(Strp);

  if (len > buflen)
    return true;
  else if (go) {
    memset(buf, ' ', buflen);
    memcpy(buf, Strp, len);
    } // endif go

  return false;
  } // end of GetBinValue

/***********************************************************************/
/*  STRING ShowValue: get string representation of a char value.       */
/***********************************************************************/
char *TYPVAL<PSZ>::ShowValue(char *buf, int len)
  {
  return Strp;
  } // end of ShowValue

/***********************************************************************/
/*  STRING GetCharString: get string representation of a char value.   */
/***********************************************************************/
char *TYPVAL<PSZ>::GetCharString(char *p)
  {
  return Strp;
  } // end of GetCharString

/***********************************************************************/
/*  STRING GetShortString: get short representation of a char value.   */
/***********************************************************************/
char *TYPVAL<PSZ>::GetShortString(char *p, int n)
  {
  sprintf(p, "%*hd", n, (short)(Null ? 0 : atoi(Strp)));
  return p;
  } // end of GetShortString

/***********************************************************************/
/*  STRING GetIntString: get int representation of a char value.       */
/***********************************************************************/
char *TYPVAL<PSZ>::GetIntString(char *p, int n)
  {
  sprintf(p, "%*ld", n, (Null) ? 0 : atol(Strp));
  return p;
  } // end of GetIntString

/***********************************************************************/
/*  STRING GetBigintString: get big int representation of a char value.*/
/***********************************************************************/
char *TYPVAL<PSZ>::GetBigintString(char *p, int n)
  {
  sprintf(p, "%*lld", n, (Null) ? 0 : atoll(Strp));
  return p;
  } // end of GetBigintString

/***********************************************************************/
/*  STRING GetFloatString: get double representation of a char value.  */
/***********************************************************************/
char *TYPVAL<PSZ>::GetFloatString(char *p, int n, int prec)
  {
  sprintf(p, "%*.*lf", n, (prec < 0) ? 2 : prec, Null ? 0 : atof(Strp));
  return p;
  } // end of GetFloatString

/***********************************************************************/
/*  STRING compare value with another Value.                           */
/***********************************************************************/
bool TYPVAL<PSZ>::IsEqual(PVAL vp, bool chktype)
  {
  if (this == vp)
    return true;
  else if (chktype && Type != vp->GetType())
    return false;
  else if (Null || vp->IsNull())
    return false;
  else if (Ci || vp->IsCi())
    return !stricmp(Strp, vp->GetCharValue());
  else // (!Ci)
    return !strcmp(Strp, vp->GetCharValue());

  } // end of IsEqual

/***********************************************************************/
/*  FormatValue: This function set vp (a STRING value) to the string   */
/*  constructed from its own value formated using the fmt format.      */
/*  This function assumes that the format matches the value type.      */
/***********************************************************************/
bool TYPVAL<PSZ>::FormatValue(PVAL vp, char *fmt)
  {
  char *buf = (char*)vp->GetTo_Val();        // Should be big enough
  int   n = sprintf(buf, fmt, Strp);

  return (n > vp->GetValLen());
  } // end of FormatValue

/***********************************************************************/
/*  STRING SetFormat function (used to set SELECT output format).      */
/***********************************************************************/
bool TYPVAL<PSZ>::SetConstFormat(PGLOBAL g, FORMAT& fmt)
  {
  fmt.Type[0] = 'C';
  fmt.Length = Len;
  fmt.Prec = 0;
  return false;
  } // end of SetConstFormat

/* -------------------------- Class DTVAL ---------------------------- */

/***********************************************************************/
/*  DTVAL  public constructor for new void values.                     */
/***********************************************************************/
DTVAL::DTVAL(PGLOBAL g, int n, int prec, PSZ fmt)
     : TYPVAL<int>((int)0, TYPE_DATE)
  {
  if (!fmt) {
    Pdtp = NULL;
    Sdate = NULL;
    DefYear = 0;
    Len = n;
  } else
    SetFormat(g, fmt, n, prec);

//Type = TYPE_DATE;
  } // end of DTVAL constructor

/***********************************************************************/
/*  DTVAL  public constructor from int.                                */
/***********************************************************************/
DTVAL::DTVAL(PGLOBAL g, int n) : TYPVAL<int>(n, TYPE_DATE)
  {
  Pdtp = NULL;
  Len = 19;
//Type = TYPE_DATE;
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
#if defined(WIN32)
  struct tm dtm = {0,0,0,2,0,70,0,0,0};
#else   // !WIN32
  struct tm dtm = {0,0,0,2,0,70,0,0,0,0,0};
#endif  // !WIN32

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
  time_t t = (time_t)Tval;

  if (Tval < 0) {
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

    Tval = (int)t;
  } else
    Tval = (int)t - Shift;

  if (trace)
    htrc("MakeTime Ival=%d\n", Tval); 

  return false;
  } // end of MakeTime

/***********************************************************************/
/* Make a time_t datetime from its components (YY, MM, DD, hh, mm, ss) */
/***********************************************************************/
bool DTVAL::MakeDate(PGLOBAL g, int *val, int nval)
  {
  int       i, m;
  int       n;
  bool      rc = false;
#if defined(WIN32)
  struct tm datm = {0,0,0,2,0,70,0,0,0};
#else   // !WIN32
  struct tm datm = {0,0,0,2,0,70,0,0,0,0,0};
#endif  // !WIN32

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
      Tval = 0;

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

  if (!(Null = valp->IsNull() && Nullable)) {
    if (Pdtp && !valp->IsTypeNum()) {
      int   ndv;
      int  dval[6];

      ndv = ExtractDate(valp->GetCharValue(), Pdtp, DefYear, dval);
      MakeDate(NULL, dval, ndv);
    } else
      Tval = valp->GetIntValue();

  } else
    Reset();

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
      htrc(" setting date: '%s' -> %d\n", Sdate, Tval);

    Null = false;
  } else
    TYPVAL<int>::SetValue_char(p, n);

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
      htrc(" setting date: '%s' -> %d\n", Sdate, Tval);

    Null = false;
  } else
    TYPVAL<int>::SetValue_psz(p);

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
    Tval = blk->GetIntValue(n);

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
    sprintf(p, "%d", Tval);

  Null = false;
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
    return TYPVAL<int>::ShowValue(buf, len);

  } // end of ShowValue

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

/* -------------------------- End of Value --------------------------- */
