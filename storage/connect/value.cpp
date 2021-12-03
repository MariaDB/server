/************* Value C++ Functions Source Code File (.CPP) *************/
/*  Name: VALUE.CPP  Version 2.9                                       */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2001-2019    */
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
/*  Currently the only implemented types are STRING, INT, SHORT, TINY, */
/*  DATE and LONGLONG. Recently we added some UNSIGNED types.          */
/***********************************************************************/

/***********************************************************************/
/*  Include relevant MariaDB header file.                              */
/***********************************************************************/
#include "my_global.h"
#include "sql_class.h"
#include "sql_time.h"

#if defined(_WIN32)
//#include <windows.h>
#else   // !_WIN32
#include <string.h>
#endif  // !_WIN32

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
#include "valblk.h"
#define NO_FUNC                           // Already defined in ODBConn
#include "plgcnx.h"                       // For DB types
#include "osutil.h"

/***********************************************************************/
/*  Check macro's.                                                     */
/***********************************************************************/
#if defined(_DEBUG)
#define CheckType(V)    if (Type != V->GetType()) { \
    PGLOBAL& g = Global; \
    strcpy(g->Message, MSG(VALTYPE_NOMATCH)); \
    throw Type;
#else
#define CheckType(V)
#endif

#define FOURYEARS    126230400    // Four years in seconds (1 leap)

/***********************************************************************/
/*  Initialize the DTVAL static member.                                */
/***********************************************************************/
int DTVAL::Shift = 0;

/***********************************************************************/
/*  Routines called externally.                                        */
/***********************************************************************/
bool PlugEvalLike(PGLOBAL, LPCSTR, LPCSTR, bool);

#if !defined(_WIN32)
extern "C" {
PSZ strupr(PSZ s);
PSZ strlwr(PSZ s);
}
#endif   // !_WIN32

/***********************************************************************/
/*  Get a long long number from its character representation.          */
/*  IN  p: Pointer to the numeric string                               */
/*  IN  n: The string length                                           */
/*  IN  maxval: The number max value                                   */
/*  IN  un: True if the number must be unsigned                        */
/*  OUT rc: Set to TRUE for out of range value                         */
/*  OUT minus: Set to true if the number is negative                   */
/*  Returned val: The resulting number                                 */
/***********************************************************************/
ulonglong CharToNumber(const char *p, int n, ulonglong maxval,
                       bool un, bool *minus, bool *rc)
{
  const char *p2;
  uchar       c;
  ulonglong   val;

  if (minus) *minus = false;
  if (rc) *rc = false;
	if (n <= 0) return 0LL;

  // Eliminate leading blanks or 0
  for (p2 = p + n; p < p2 && (*p == ' ' || *p == '0'); p++) ;

  // Get an eventual sign character
  switch (*p) {
    case '-':
      if (un) {
        if (rc) *rc = true;
        return 0;
      } else {
        maxval++;
        if (minus) *minus = true;
      } // endif Unsigned

			// Fall through
    case '+':
      p++;
      break;
  } // endswitch *p

  for (val = 0; p < p2 && (c = (uchar)(*p - '0')) < 10; p++)
    if (val > (maxval - c) / 10) {
      val = maxval;
      if (rc) *rc = true;
      break;
    } else
      val = val * 10 + c;

  return val;
} // end of CharToNumber

/***********************************************************************/
/*  GetTypeName: returns the PlugDB internal type name.                */
/***********************************************************************/
PCSZ GetTypeName(int type)
{
  PCSZ name;

  switch (type) {
    case TYPE_STRING: name = "CHAR";     break;
    case TYPE_SHORT:  name = "SMALLINT"; break;
    case TYPE_INT:    name = "INTEGER";  break;
    case TYPE_BIGINT: name = "BIGINT";   break;
    case TYPE_DATE:   name = "DATE";     break;
    case TYPE_DOUBLE: name = "DOUBLE";   break;
    case TYPE_TINY:   name = "TINY";     break;
    case TYPE_DECIM:  name = "DECIMAL";  break;
    case TYPE_BIN:    name = "BINARY";   break;
    case TYPE_PCHAR:  name = "PCHAR";    break;
    default:          name = "UNKNOWN";  break;
  } // endswitch type

  return name;
} // end of GetTypeName

/***********************************************************************/
/*  GetTypeSize: returns the PlugDB internal type size.                */
/***********************************************************************/
int GetTypeSize(int type, int len)
  {
	switch (type) {
    case TYPE_DECIM:
    case TYPE_BIN:
    case TYPE_STRING: len = len * sizeof(char); break;
    case TYPE_SHORT:  len = sizeof(short);      break;
    case TYPE_INT:    len = sizeof(int);        break;
    case TYPE_BIGINT: len = sizeof(longlong);   break;
    case TYPE_DATE:   len = sizeof(int);        break;
    case TYPE_DOUBLE: len = sizeof(double);     break;
    case TYPE_TINY:   len = sizeof(char);       break;
    case TYPE_PCHAR:  len = sizeof(char*);      break;
    default:          len = -1;
  } // endswitch type

  return len;
} // end of GetTypeSize

/***********************************************************************/
/*  GetFormatType: returns the FORMAT character(s) according to type.  */
/***********************************************************************/
const char *GetFormatType(int type)
{
  const char *c = "X";

  switch (type) {
    case TYPE_STRING: c = "C"; break;
    case TYPE_SHORT:  c = "S"; break;
    case TYPE_INT:    c = "N"; break;
    case TYPE_BIGINT: c = "L"; break;
    case TYPE_DOUBLE: c = "F"; break;
    case TYPE_DATE:   c = "D"; break;
    case TYPE_TINY:   c = "T"; break;
    case TYPE_DECIM:  c = "F"; break;
    case TYPE_BIN:    c = "B"; break;
    case TYPE_PCHAR:  c = "P"; break;
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
    case 'F': type = TYPE_DOUBLE; break;
    case 'D': type = TYPE_DATE;   break;
    case 'T': type = TYPE_TINY;   break;
    case 'M': type = TYPE_DECIM;  break;
    case 'B': type = TYPE_BIN;    break;
    case 'P': type = TYPE_PCHAR;  break;
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
    case TYPE_DECIM:
		case TYPE_BIN:
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
    case TYPE_DOUBLE:
    case TYPE_SHORT:
    case TYPE_NUM:
    case TYPE_TINY:
    case TYPE_DECIM:
      return true;
  } // endswitch type

  return false;
} // end of IsTypeNum

/***********************************************************************/
/*  GetFmt: returns the format to use with a typed value.              */
/***********************************************************************/
const char *GetFmt(int type, bool un)
{
  const char *fmt;

  switch (type) {
    case TYPE_DECIM:
    case TYPE_STRING: fmt = "%s";                   break;
    case TYPE_SHORT:  fmt = (un) ? "%hu" : "%hd";   break;
    case TYPE_BIGINT: fmt = (un) ? "%llu" : "%lld"; break;
    case TYPE_DOUBLE: fmt = "%.*lf";                break;
    case TYPE_BIN:    fmt = "%*x";                  break;
    default:          fmt = (un) ? "%u" : "%d";     break;
  } // endswitch Type

  return fmt;
} // end of GetFmt

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

      return (target == TYPE_DOUBLE || type == TYPE_DOUBLE) ? TYPE_DOUBLE
           : (target == TYPE_DATE   || type == TYPE_DATE)   ? TYPE_DATE
           : (target == TYPE_BIGINT || type == TYPE_BIGINT) ? TYPE_BIGINT
           : (target == TYPE_INT    || type == TYPE_INT)    ? TYPE_INT
           : (target == TYPE_SHORT  || type == TYPE_SHORT)  ? TYPE_SHORT
                                                            : TYPE_TINY;
    default:
      if (target == TYPE_ERROR || target == type)
        return type;

      if (match && ((IsTypeChar(target) && !IsTypeChar(type)) ||
                    (IsTypeNum(target) && !IsTypeNum(type))))
        return TYPE_ERROR;

      return (target == TYPE_DOUBLE || type == TYPE_DOUBLE) ? TYPE_DOUBLE
           : (target == TYPE_DATE   || type == TYPE_DATE)   ? TYPE_DATE
           : (target == TYPE_BIGINT || type == TYPE_BIGINT) ? TYPE_BIGINT
           : (target == TYPE_INT    || type == TYPE_INT)    ? TYPE_INT
           : (target == TYPE_SHORT  || type == TYPE_SHORT)  ? TYPE_SHORT
           : (target == TYPE_STRING || type == TYPE_STRING) ? TYPE_STRING
           : (target == TYPE_TINY   || type == TYPE_TINY)   ? TYPE_TINY
                                                            : TYPE_ERROR;
  } // endswitch kind

} // end of ConvertType

/***********************************************************************/
/*  AllocateConstant: allocates a constant Value.                      */
/***********************************************************************/
PVAL AllocateValue(PGLOBAL g, void *value, short type, short prec)
{
  PVAL valp;

  if (trace(1))
    htrc("AllocateConstant: value=%p type=%hd\n", value, type);

  switch (type) {
    case TYPE_STRING:
      valp = new(g) TYPVAL<PSZ>((PSZ)value, prec);
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
    case TYPE_DOUBLE:
      valp = new(g) TYPVAL<double>(*(double *)value, TYPE_DOUBLE, prec);
      break;
    case TYPE_TINY:
      valp = new(g) TYPVAL<char>(*(char *)value, TYPE_TINY);
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
                   bool uns, PCSZ fmt)
{
  PVAL valp;

  switch (type) {
    case TYPE_STRING:
      valp = new(g) TYPVAL<PSZ>(g, (PSZ)NULL, len, prec);
      break;
		case TYPE_DATE:
			valp = new(g) DTVAL(g, len, prec, fmt);
      break;
    case TYPE_INT:
      if (uns)
        valp = new(g) TYPVAL<uint>((uint)0, TYPE_INT, 0, true);
      else
        valp = new(g) TYPVAL<int>((int)0, TYPE_INT);

      break;
    case TYPE_BIGINT:
      if (uns)
        valp = new(g) TYPVAL<ulonglong>((ulonglong)0, TYPE_BIGINT, 0, true);
      else
        valp = new(g) TYPVAL<longlong>((longlong)0, TYPE_BIGINT);

      break;
    case TYPE_SHORT:
      if (uns)
        valp = new(g) TYPVAL<ushort>((ushort)0, TYPE_SHORT, 0, true);
      else
        valp = new(g) TYPVAL<short>((short)0, TYPE_SHORT);

      break;
    case TYPE_DOUBLE:
      valp = new(g) TYPVAL<double>(0.0, TYPE_DOUBLE, prec);
      break;
    case TYPE_TINY:
      if (uns)
        valp = new(g) TYPVAL<uchar>((uchar)0, TYPE_TINY, 0, true);
      else
        valp = new(g) TYPVAL<char>((char)0, TYPE_TINY);

      break;
    case TYPE_DECIM:
      valp = new(g) DECVAL(g, (PSZ)NULL, len, prec, uns);
      break;
    case TYPE_BIN:
      valp = new(g) BINVAL(g, (void*)NULL, len, prec);
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
PVAL AllocateValue(PGLOBAL g, PVAL valp, int newtype, int uns)
{
  PSZ  p, sp;
  bool un = (uns < 0) ? false : (uns > 0) ? true : valp->IsUnsigned();
  PVAL vp;

  if (!valp)
    return NULL;
    
  if (newtype == TYPE_VOID)  // Means allocate a value of the same type
    newtype = valp->GetType();

  switch (newtype) {
    case TYPE_STRING:
      p = (PSZ)PlugSubAlloc(g, NULL, 1 + valp->GetValLen());

      if ((sp = valp->GetCharString(p)) != p && sp)
        strcpy(p, sp);

      vp = new(g) TYPVAL<PSZ>(g, p, valp->GetValLen(), valp->GetValPrec());
      break;
    case TYPE_SHORT:
      if (un)
        vp = new(g) TYPVAL<ushort>(valp->GetUShortValue(),
                                   TYPE_SHORT, 0, true);
      else
        vp = new(g) TYPVAL<short>(valp->GetShortValue(), TYPE_SHORT);

      break;
    case TYPE_INT:
      if (un)
        vp = new(g) TYPVAL<uint>(valp->GetUIntValue(), TYPE_INT, 0, true);
      else
        vp = new(g) TYPVAL<int>(valp->GetIntValue(), TYPE_INT);

      break;
    case TYPE_BIGINT:
      if (un)
        vp = new(g) TYPVAL<ulonglong>(valp->GetUBigintValue(),
                                      TYPE_BIGINT, 0, true);
      else
        vp = new(g) TYPVAL<longlong>(valp->GetBigintValue(), TYPE_BIGINT);

      break;
    case TYPE_DATE:
      vp = new(g) DTVAL(valp->GetIntValue());
      break;
    case TYPE_DOUBLE:
      vp = new(g) TYPVAL<double>(valp->GetFloatValue(), TYPE_DOUBLE,
                   (uns) ? uns : valp->GetValPrec());
      break;
    case TYPE_TINY:
      if (un)
        vp = new(g) TYPVAL<uchar>(valp->GetUTinyValue(),
                                    TYPE_TINY, 0, true);
      else
        vp = new(g) TYPVAL<char>(valp->GetTinyValue(), TYPE_TINY);

      break;
    default:
      sprintf(g->Message, MSG(BAD_VALUE_TYPE), newtype);
      return NULL;
  } // endswitch type
  
  vp->SetNullable(valp->GetNullable());
  vp->SetNull(valp->IsNull());
  vp->SetGlobal(g);
  return vp;
} // end of AllocateValue

/* -------------------------- Class VALUE ---------------------------- */

/***********************************************************************/
/*  Class VALUE protected constructor.                                 */
/***********************************************************************/
VALUE::VALUE(int type, bool un) : Type(type)
{
  Null = false;
  Nullable = false;
  Unsigned = un;
  Clen = 0;
  Prec = 0;
  Fmt = GetFmt(Type, Unsigned);
  Xfmt = GetXfmt();
} // end of VALUE constructor

/***********************************************************************/
/* VALUE GetXfmt: returns the extended format to use with typed value. */
/***********************************************************************/
const char *VALUE::GetXfmt(void)
{
  const char *fmt;

  switch (Type) {
    case TYPE_DECIM:
    case TYPE_STRING: fmt = "%*s";                          break;
    case TYPE_SHORT:  fmt = (Unsigned) ? "%*hu" : "%*hd";   break;
    case TYPE_BIGINT: fmt = (Unsigned) ? "%*llu" : "%*lld"; break;
    case TYPE_DOUBLE: fmt = "%*.*lf";                       break;
    case TYPE_BIN:    fmt = "%*x";                          break;
    default:          fmt = (Unsigned) ? "%*u" : "%*d";     break;
    } // endswitch Type

  return fmt;
} // end of GetXFmt

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

/***********************************************************************/
/*  Compute a function on a string.                                    */
/***********************************************************************/
bool VALUE::Compute(PGLOBAL g, PVAL *, int, OPVAL)
{
  strcpy(g->Message, "Compute not implemented for this value type");
  return true;
} // end of Compute

/***********************************************************************/
/*  Make file output of an object value.                               */
/***********************************************************************/
void VALUE::Printf(PGLOBAL g, FILE *f, uint n)
{
	char m[64], buf[64];

	memset(m, ' ', n);                             /* Make margin string */
	m[n] = '\0';

	if (Null)
		fprintf(f, "%s<null>\n", m);
	else
		fprintf(f, "%s%s\n", m, GetCharString(buf));

} /* end of Printf */

/***********************************************************************/
/*  Make string output of an object value.                             */
/***********************************************************************/
void VALUE::Prints(PGLOBAL g, char *ps, uint z)
{
	char *p, buf[64];

	if (Null)
		p = strcpy(buf, "<null>");
	else
		p = GetCharString(buf);

  strncpy(ps, p, z);
} // end of Prints

/* -------------------------- Class TYPVAL ---------------------------- */

/***********************************************************************/
/*  TYPVAL  public constructor from a constant typed value.            */
/***********************************************************************/
template <class TYPE>
TYPVAL<TYPE>::TYPVAL(TYPE n, int type, int prec, bool un)
            : VALUE(type, un)
{
  Tval = n;
  Clen = sizeof(TYPE);
  Prec = prec;
} // end of TYPVAL constructor

/***********************************************************************/
/*  Return unsigned max value for the type.                            */
/***********************************************************************/
template <class TYPE>
ulonglong TYPVAL<TYPE>::MaxVal(void) {DBUG_ASSERT(false); return 0;}

template <>
ulonglong TYPVAL<short>::MaxVal(void) {return INT_MAX16;}

template <>
ulonglong TYPVAL<ushort>::MaxVal(void) {return UINT_MAX16;}

template <>
ulonglong TYPVAL<int>::MaxVal(void) {return INT_MAX32;}

template <>
ulonglong TYPVAL<uint>::MaxVal(void) {return UINT_MAX32;}

template <>
ulonglong TYPVAL<char>::MaxVal(void) {return INT_MAX8;}

template <>
ulonglong TYPVAL<uchar>::MaxVal(void) {return UINT_MAX8;}

template <>
ulonglong TYPVAL<longlong>::MaxVal(void) {return INT_MAX64;}

template <>
ulonglong TYPVAL<ulonglong>::MaxVal(void) {return ULONGLONG_MAX;}

/***********************************************************************/
/*  TYPVAL GetValLen: returns the print length of the typed object.    */
/***********************************************************************/
template <class TYPE>
int TYPVAL<TYPE>::GetValLen(void)
{
  char c[32];

  return snprintf(c, 32, Fmt, Tval);
} // end of GetValLen

template <>
int TYPVAL<double>::GetValLen(void)
{
  char c[32];

  return snprintf(c, 32, Fmt, Prec, Tval);
} // end of GetValLen

/***********************************************************************/
/*  TYPVAL SetValue: copy the value of another Value object.           */
/*  This function allows conversion if chktype is false.               */
/***********************************************************************/
template <class TYPE>
bool TYPVAL<TYPE>::SetValue_pval(PVAL valp, bool chktype)
{
  if (valp != this) {
    if (chktype && Type != valp->GetType())
      return true;

    if (!(Null = (valp->IsNull() && Nullable)))
      Tval = GetTypedValue(valp);
    else
      Reset();

  } // endif valp

  return false;
} // end of SetValue

template <>
short TYPVAL<short>::GetTypedValue(PVAL valp)
  {return valp->GetShortValue();}

template <>
ushort TYPVAL<ushort>::GetTypedValue(PVAL valp)
  {return valp->GetUShortValue();}

template <>
int TYPVAL<int>::GetTypedValue(PVAL valp)
  {return valp->GetIntValue();}

template <>
uint TYPVAL<uint>::GetTypedValue(PVAL valp)
  {return valp->GetUIntValue();}

template <>
longlong TYPVAL<longlong>::GetTypedValue(PVAL valp)
  {return valp->GetBigintValue();}

template <>
ulonglong TYPVAL<ulonglong>::GetTypedValue(PVAL valp)
  {return valp->GetUBigintValue();}

template <>
double TYPVAL<double>::GetTypedValue(PVAL valp)
  {return valp->GetFloatValue();}

template <>
char TYPVAL<char>::GetTypedValue(PVAL valp)
  {return valp->GetTinyValue();}

template <>
uchar TYPVAL<uchar>::GetTypedValue(PVAL valp)
  {return valp->GetUTinyValue();}

/***********************************************************************/
/*  TYPVAL SetValue: convert chars extracted from a line to TYPE value.*/
/***********************************************************************/
template <class TYPE>
bool TYPVAL<TYPE>::SetValue_char(const char *p, int n)
{
  bool      rc, minus;
  ulonglong maxval = MaxVal();
  ulonglong val = CharToNumber(p, n, maxval, Unsigned, &minus, &rc);

  if (minus && val < maxval)
    Tval = (TYPE)(-(signed)val);
  else
    Tval = (TYPE)val;

  if (trace(2)) {
    char buf[64];
    htrc(strcat(strcat(strcpy(buf, " setting %s to: "), Fmt), "\n"),
                              GetTypeName(Type), Tval);
  } // endif trace

  Null = false;
  return rc;
} // end of SetValue

template <>
bool TYPVAL<double>::SetValue_char(const char *p, int n)
{
  if (p && n > 0) {
    char buf[64];

    for (; n > 0 && *p == ' '; p++)
      n--;

    memcpy(buf, p, MY_MIN(n, 31));
    buf[n] = '\0';
    Tval = atof(buf);

    if (trace(2))
      htrc(" setting double: '%s' -> %lf\n", buf, Tval);

    Null = false;
  } else {
    Reset();
    Null = Nullable;
  } // endif p

  return false;
} // end of SetValue

/***********************************************************************/
/*  TYPVAL SetValue: fill a typed value from a string.                 */
/***********************************************************************/
template <class TYPE>
void TYPVAL<TYPE>::SetValue_psz(PCSZ s)
{
  if (s) {
    SetValue_char(s, (int)strlen(s));
    Null = false;
  } else {
    Reset();
    Null = Nullable;
  } // endif p

} // end of SetValue

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
uint TYPVAL<uint>::GetTypedValue(PVBLK blk, int n)
  {return blk->GetUIntValue(n);}

template <>
short TYPVAL<short>::GetTypedValue(PVBLK blk, int n)
  {return blk->GetShortValue(n);}

template <>
ushort TYPVAL<ushort>::GetTypedValue(PVBLK blk, int n)
  {return blk->GetUShortValue(n);}

template <>
longlong TYPVAL<longlong>::GetTypedValue(PVBLK blk, int n)
  {return blk->GetBigintValue(n);}

template <>
ulonglong TYPVAL<ulonglong>::GetTypedValue(PVBLK blk, int n)
  {return blk->GetUBigintValue(n);}

template <>
double TYPVAL<double>::GetTypedValue(PVBLK blk, int n)
  {return blk->GetFloatValue(n);}

template <>
char TYPVAL<char>::GetTypedValue(PVBLK blk, int n)
  {return blk->GetTinyValue(n);}

template <>
uchar TYPVAL<uchar>::GetTypedValue(PVBLK blk, int n)
  {return blk->GetUTinyValue(n);}

/***********************************************************************/
/*  TYPVAL SetBinValue: with bytes extracted from a line.              */
/*  Currently only used reading column of binary files.                */
/***********************************************************************/
template <class TYPE>
void TYPVAL<TYPE>::SetBinValue(void *p)
{
#if defined(UNALIGNED_OK)
	// x86 can cast non-aligned memory directly
	Tval = *(TYPE *)p;
#else
	// Prevent unaligned memory access on MIPS and ArmHF platforms.
	// Make use of memcpy instead of straight pointer dereferencing.
	// Currently only used by WriteColumn of binary files.
	// From original author: Vicentiu Ciorbaru <vicentiu@mariadb.org>
	memcpy(&Tval, p, sizeof(TYPE));
#endif
	Null = false;
} // end of SetBinValue

/***********************************************************************/
/*  GetBinValue: fill a buffer with the internal binary value.         */
/*  This function checks whether the buffer length is enough and       */
/*  returns true if not. Actual filling occurs only if go is true.     */
/*  Currently only used writing column of binary files.                */
/***********************************************************************/
template <class TYPE>
bool TYPVAL<TYPE>::GetBinValue(void *buf, int buflen, bool go)
{
  // Test on length was removed here until a variable in column give the
  // real field length. For BIN files the field length logically cannot
  // be different from the variable length because no conversion is done.
  // Therefore this test is useless anyway.
//#if defined(_DEBUG)
//  if (sizeof(TYPE) > buflen)
//    return true;
//#endif

  if (go)
#if defined(UNALIGNED_OK)
		// x86 can cast non-aligned memory directly
		*(TYPE *)buf = Tval;
#else
		// Prevent unaligned memory access on MIPS and ArmHF platforms.
		// Make use of memcpy instead of straight pointer dereferencing.
		// Currently only used by WriteColumn of binary files.
		// From original author: Vicentiu Ciorbaru <vicentiu@mariadb.org>
		memcpy(buf, &Tval, sizeof(TYPE));
#endif

  Null = false;
  return false;
} // end of GetBinValue

/***********************************************************************/
/*  TYPVAL ShowValue: get string representation of a typed value.      */
/***********************************************************************/
template <class TYPE>
int TYPVAL<TYPE>::ShowValue(char *buf, int len)
{
  return snprintf(buf, len + 1, Xfmt, len, Tval);
} // end of ShowValue

template <>
int TYPVAL<double>::ShowValue(char *buf, int len)
{
  // TODO: use a more appropriate format to avoid possible truncation
  return snprintf(buf, len + 1, Xfmt, len, Prec, Tval);
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
	// Most callers use a 32 long buffer
  snprintf(p, 32, Fmt, Prec, Tval);
  return p;
} // end of GetCharString

#if 0
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
/*  TYPVAL GetTinyString: get char representation of a typed value.    */
/***********************************************************************/
template <class TYPE>
char *TYPVAL<TYPE>::GetTinyString(char *p, int n)
{
  sprintf(p, "%*d", n, (int)(char)Tval);
  return p;
} // end of GetIntString
#endif // 0

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
  else if (chktype && Unsigned != vp->IsUnsigned())
    return false;
  else if (Null || vp->IsNull())
    return false;
  else
    return (Tval == GetTypedValue(vp));

} // end of IsEqual

/***********************************************************************/
/*  Compare values and returns 1, 0 or -1 according to comparison.     */
/*  This function is used for evaluation of numeric filters.           */
/***********************************************************************/
template <class TYPE>
int TYPVAL<TYPE>::CompareValue(PVAL vp)
{
//assert(vp->GetType() == Type);

  // Process filtering on numeric values.
  TYPE n = GetTypedValue(vp);

//if (trace(1))
//  htrc(" Comparing: val=%d,%d\n", Tval, n);

  return (Tval > n) ? 1 : (Tval < n) ? (-1) : 0;
} // end of CompareValue

/***********************************************************************/
/*  Return max type value if b is true, else min type value.           */
/***********************************************************************/
template <>
short TYPVAL<short>::MinMaxVal(bool b)
  {return (b) ? INT_MAX16 : INT_MIN16;}

template <>
ushort TYPVAL<ushort>::MinMaxVal(bool b)
  {return (b) ? UINT_MAX16 : 0;}

template <>
int TYPVAL<int>::MinMaxVal(bool b)
  {return (b) ? INT_MAX32 : INT_MIN32;}

template <>
uint TYPVAL<uint>::MinMaxVal(bool b)
  {return (b) ? UINT_MAX32 : 0;}

template <>
longlong TYPVAL<longlong>::MinMaxVal(bool b)
  {return (b) ? INT_MAX64 : INT_MIN64;}

template <>
ulonglong TYPVAL<ulonglong>::MinMaxVal(bool b)
  {return (b) ? 0xFFFFFFFFFFFFFFFFLL : 0;}

template <>
double TYPVAL<double>::MinMaxVal(bool)
  {assert(false); return 0.0;}

template <>
char TYPVAL<char>::MinMaxVal(bool b)
  {return (b) ? INT_MAX8 : INT_MIN8;}

template <>
uchar TYPVAL<uchar>::MinMaxVal(bool b)
  {return (b) ? UINT_MAX8 : 0;}

/***********************************************************************/
/*  SafeAdd: adds a value and test whether overflow/underflow occurred. */
/***********************************************************************/
template <class TYPE>
TYPE TYPVAL<TYPE>::SafeAdd(TYPE n1, TYPE n2)
{
  PGLOBAL& g = Global;
  TYPE     n = n1 + n2;

  if ((n2 > 0) && (n < n1)) {
    // Overflow
    strcpy(g->Message, MSG(FIX_OVFLW_ADD));
		throw 138;
	} else if ((n2 < 0) && (n > n1)) {
    // Underflow
    strcpy(g->Message, MSG(FIX_UNFLW_ADD));
		throw 138;
	} // endif's n2

  return n;
} // end of SafeAdd

template <>
inline double TYPVAL<double>::SafeAdd(double n1, double n2)
{
  return n1 + n2;
} // end of SafeAdd

/***********************************************************************/
/*  SafeMult: multiply values and test whether overflow occurred.       */
/***********************************************************************/
template <class TYPE>
TYPE TYPVAL<TYPE>::SafeMult(TYPE n1, TYPE n2)
{
  PGLOBAL& g = Global;
  double   n = (double)n1 * (double)n2;

  if (n > MinMaxVal(true)) {
    // Overflow
    strcpy(g->Message, MSG(FIX_OVFLW_TIMES));
		throw 138;
	} else if (n < MinMaxVal(false)) {
    // Underflow
    strcpy(g->Message, MSG(FIX_UNFLW_TIMES));
		throw 138;
	} // endif's n2

  return (TYPE)n;
} // end of SafeMult

template <>
inline double TYPVAL<double>::SafeMult(double n1, double n2)
{
  return n1 * n2;
} // end of SafeMult

/***********************************************************************/
/*  Compute defined functions for the type.                            */
/***********************************************************************/
template <class TYPE>
bool TYPVAL<TYPE>::Compute(PGLOBAL g, PVAL *vp, int np, OPVAL op)
{
  bool rc = false;
  TYPE val[2];

  assert(np == 2);

  for (int i = 0; i < np; i++)
    val[i] = GetTypedValue(vp[i]);

  switch (op) {
    case OP_ADD:
      Tval = SafeAdd(val[0], val[1]);
      break;
    case OP_MULT:
      Tval = SafeMult(val[0], val[1]);
      break;
    case OP_DIV:
      if (!val[1]) {
        strcpy(g->Message, MSG(ZERO_DIVIDE));
        return true;
        } // endif

      Tval = val[0] / val[1];
      break;
    default:
      rc = Compall(g, vp, np, op);
      break;
  } // endswitch op

  return rc;
} // end of Compute

template <>
bool TYPVAL<double>::Compute(PGLOBAL g, PVAL *vp, int np, OPVAL op)
{
  bool   rc = false;
  double val[2];

  assert(np == 2);

  for (int i = 0; i < np; i++)
    val[i] = vp[i]->GetFloatValue();

  switch (op) {
    case OP_ADD:
      Tval = val[0] + val[1];
      break;
    case OP_MULT:
      Tval = val[0] * val[1];
      break;
    default:
      rc = Compall(g, vp, np, op);
  } // endswitch op

  return rc;
} // end of Compute

/***********************************************************************/
/*  Compute a function for all types.                                  */
/***********************************************************************/
template <class TYPE>
bool TYPVAL<TYPE>::Compall(PGLOBAL g, PVAL *vp, int np, OPVAL op)
{
  TYPE val[2];

  for (int i = 0; i < np; i++)
    val[i] = GetTypedValue(vp[i]);

  switch (op) {
    case OP_DIV:
      if (val[0]) {
        if (!val[1]) {
          strcpy(g->Message, MSG(ZERO_DIVIDE));
          return true;
          } // endif
    
        Tval = val[0] / val[1];
      } else
        Tval = 0;

      break;
    case OP_MIN:
      Tval = MY_MIN(val[0], val[1]);
      break;
    case OP_MAX:
      Tval = MY_MAX(val[0], val[1]);
      break;
    default:
//    sprintf(g->Message, MSG(BAD_EXP_OPER), op);
      strcpy(g->Message, "Function not supported");
      return true;
  } // endswitch op

  return false;
} // end of Compall

/***********************************************************************/
/*  FormatValue: This function set vp (a STRING value) to the string   */
/*  constructed from its own value formated using the fmt format.      */
/*  This function assumes that the format matches the value type.      */
/***********************************************************************/
template <class TYPE>
bool TYPVAL<TYPE>::FormatValue(PVAL vp, PCSZ fmt)
{
	// This function is wrong and should never be called
	assert(false);
  char *buf = (char*)vp->GetTo_Val();        // Not big enough
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

/* -------------------------- Class STRING --------------------------- */

/***********************************************************************/
/*  STRING  public constructor from a constant string.                 */
/***********************************************************************/
TYPVAL<PSZ>::TYPVAL(PSZ s, short c) : VALUE(TYPE_STRING)
{
  Strp = s;
  Len = strlen(s);
  Clen = Len;
  Ci = (c == 1);
} // end of STRING constructor

/***********************************************************************/
/*  STRING public constructor from char.                               */
/***********************************************************************/
TYPVAL<PSZ>::TYPVAL(PGLOBAL g, PSZ s, int n, int c)
           : VALUE(TYPE_STRING)
{
  Len = (g) ? n : (s) ? strlen(s) : 0;

  if (!s) {
    if (g) {
      if ((Strp = (char *)PlgDBSubAlloc(g, NULL, Len + 1)))
        memset(Strp, 0, Len + 1);
      else
        Len = 0;

    } else
      assert(false);

  } else
    Strp = s;

  Clen = Len;
  Ci = (c != 0);
} // end of STRING constructor

/***********************************************************************/
/*  Get the tiny value represented by the Strp string.                 */
/***********************************************************************/
char TYPVAL<PSZ>::GetTinyValue(void)
{
  bool      m;
  ulonglong val = CharToNumber(Strp, strlen(Strp), INT_MAX8, false, &m);

  return (m && val < INT_MAX8) ? (char)(-(signed)val) : (char)val;
} // end of GetTinyValue

/***********************************************************************/
/*  Get the unsigned tiny value represented by the Strp string.        */
/***********************************************************************/
uchar TYPVAL<PSZ>::GetUTinyValue(void)
{
  return (uchar)CharToNumber(Strp, strlen(Strp), UINT_MAX8, true);
} // end of GetUTinyValue

/***********************************************************************/
/*  Get the short value represented by the Strp string.                */
/***********************************************************************/
short TYPVAL<PSZ>::GetShortValue(void)
{
  bool      m;
  ulonglong val = CharToNumber(Strp, strlen(Strp), INT_MAX16, false, &m);

  return (m && val < INT_MAX16) ? (short)(-(signed)val) : (short)val;
} // end of GetShortValue

/***********************************************************************/
/*  Get the unsigned short value represented by the Strp string.       */
/***********************************************************************/
ushort TYPVAL<PSZ>::GetUShortValue(void)
{
  return (ushort)CharToNumber(Strp, strlen(Strp), UINT_MAX16, true);
} // end of GetUshortValue

/***********************************************************************/
/*  Get the integer value represented by the Strp string.              */
/***********************************************************************/
int TYPVAL<PSZ>::GetIntValue(void)
{
  bool      m;
  ulonglong val = CharToNumber(Strp, strlen(Strp), INT_MAX32, false, &m);

  return (m && val < INT_MAX32) ? (int)(-(signed)val) : (int)val;
} // end of GetIntValue

/***********************************************************************/
/*  Get the unsigned integer value represented by the Strp string.     */
/***********************************************************************/
uint TYPVAL<PSZ>::GetUIntValue(void)
{
  return (uint)CharToNumber(Strp, strlen(Strp), UINT_MAX32, true);
} // end of GetUintValue

/***********************************************************************/
/*  Get the big integer value represented by the Strp string.          */
/***********************************************************************/
longlong TYPVAL<PSZ>::GetBigintValue(void)
{
  bool      m;
  ulonglong val = CharToNumber(Strp, strlen(Strp), INT_MAX64, false, &m);

  return (m && val < INT_MAX64) ? (-(signed)val) : (longlong)val;
} // end of GetBigintValue

/***********************************************************************/
/*  Get the unsigned big integer value represented by the Strp string. */
/***********************************************************************/
ulonglong TYPVAL<PSZ>::GetUBigintValue(void)
{
  return CharToNumber(Strp, strlen(Strp), ULONGLONG_MAX, true);
} // end of GetUBigintValue

/***********************************************************************/
/*  STRING SetValue: copy the value of another Value object.           */
/***********************************************************************/
bool TYPVAL<PSZ>::SetValue_pval(PVAL valp, bool chktype)
{
  if (valp != this) {
    if (chktype && (valp->GetType() != Type || valp->GetSize() > Len))
      return true;

    char buf[64];

    if (!(Null = (valp->IsNull() && Nullable)))
      strncpy(Strp, valp->GetCharString(buf), Len);
    else
      Reset();

  } // endif valp

  return false;
} // end of SetValue_pval

/***********************************************************************/
/*  STRING SetValue: fill string with chars extracted from a line.     */
/***********************************************************************/
bool TYPVAL<PSZ>::SetValue_char(const char *cp, int n)
{
  bool rc = false;

  if (!cp || n == 0) {
		Reset();
		Null = (cp) ? false : Nullable;
	} else if (cp != Strp) {
		const char *p = cp + n - 1;

		for (; p >= cp; p--, n--)
			if (*p && *p != ' ')
				break;

		rc = n > Len;

    if ((n = MY_MIN(n, Len))) {
    	strncpy(Strp, cp, n);
	    Strp[n] = '\0';

      if (trace(2))
        htrc(" Setting string to: '%s'\n", Strp);

    } else
      Reset();

    Null = false;
  } // endif cp

  return rc;
} // end of SetValue_char

/***********************************************************************/
/*  STRING SetValue: fill string with another string.                  */
/***********************************************************************/
void TYPVAL<PSZ>::SetValue_psz(PCSZ s)
{
  if (!s) {
		Reset();
		Null = Nullable;
	} else if (s != Strp) {
		strncpy(Strp, s, Len);
    Null = false;
  } // endif s

} // end of SetValue_psz

/***********************************************************************/
/*  STRING SetValue: fill string with a string extracted from a block. */
/***********************************************************************/
void TYPVAL<PSZ>::SetValue_pvblk(PVBLK blk, int n)
{
  // STRBLK's can return a NULL pointer
  PSZ vp = blk->GetCharString(Strp, n);

  if (vp != Strp)
    SetValue_psz(vp);

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
		throw 138;
	} else
    SetValue_psz(buf);

  Null = false;
} // end of SetValue

/***********************************************************************/
/*  STRING SetValue: get the character representation of an uint.      */
/***********************************************************************/
void TYPVAL<PSZ>::SetValue(uint n)
{
  char     buf[16];
  PGLOBAL& g = Global;
  int      k = sprintf(buf, "%u", n);

  if (k > Len) {
    sprintf(g->Message, MSG(VALSTR_TOO_LONG), buf, Len);
    throw 138;
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
/*  STRING SetValue: get the character representation of a ushort int. */
/***********************************************************************/
void TYPVAL<PSZ>::SetValue(ushort i)
{
  SetValue((uint)i);
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
		throw 138;
	} else
    SetValue_psz(buf);

  Null = false;
} // end of SetValue

/***********************************************************************/
/*  STRING SetValue: get the character representation of a big integer.*/
/***********************************************************************/
void TYPVAL<PSZ>::SetValue(ulonglong n)
{
  char     buf[24];
  PGLOBAL& g = Global;
  int      k = sprintf(buf, "%llu", n);

  if (k > Len) {
    sprintf(g->Message, MSG(VALSTR_TOO_LONG), buf, Len);
    throw 138;
  } else
    SetValue_psz(buf);

  Null = false;
} // end of SetValue

/***********************************************************************/
/*  STRING SetValue: get the character representation of a double.     */
/***********************************************************************/
void TYPVAL<PSZ>::SetValue(double f)
{
  char    *p, buf[64];
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
		throw 138;
	} else
    SetValue_psz(buf);

  Null = false;
} // end of SetValue

/***********************************************************************/
/*  STRING SetValue: get the character representation of a tiny int.   */
/***********************************************************************/
void TYPVAL<PSZ>::SetValue(char c)
{
  SetValue((int)c);
  Null = false;
} // end of SetValue

/***********************************************************************/
/*  STRING SetValue: get the character representation of a tiny int.   */
/***********************************************************************/
void TYPVAL<PSZ>::SetValue(uchar c)
{
  SetValue((uint)c);
  Null = false;
} // end of SetValue

/***********************************************************************/
/*  STRING SetBinValue: fill string with chars extracted from a line.  */
/***********************************************************************/
void TYPVAL<PSZ>::SetBinValue(void *p)
{
  SetValue_char((const char *)p, Len);
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
int TYPVAL<PSZ>::ShowValue(char *buf, int buflen)
{
	int len = (Null) ? 0 : strlen(Strp);

	if (buf && buf != Strp) {
		memset(buf, ' ', (size_t)buflen + 1);
		memcpy(buf, Strp, MY_MIN(len, buflen));
	} // endif buf

	return len;
} // end of ShowValue

/***********************************************************************/
/*  STRING GetCharString: get string representation of a char value.   */
/***********************************************************************/
char *TYPVAL<PSZ>::GetCharString(char *)
{
  return Strp;
} // end of GetCharString

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

  char buf[64];

  if (Ci || vp->IsCi())
    return !stricmp(Strp, vp->GetCharString(buf));
  else // (!Ci)
    return !strcmp(Strp, vp->GetCharString(buf));

} // end of IsEqual

/***********************************************************************/
/*  Compare values and returns 1, 0 or -1 according to comparison.     */
/*  This function is used for evaluation of numeric filters.           */
/***********************************************************************/
int TYPVAL<PSZ>::CompareValue(PVAL vp)
{
  int n;
//assert(vp->GetType() == Type);

  if (trace(1))
    htrc(" Comparing: val='%s','%s'\n", Strp, vp->GetCharValue());

  // Process filtering on character strings.
  if (Ci || vp->IsCi())
    n = stricmp(Strp, vp->GetCharValue());
  else
    n = strcmp(Strp, vp->GetCharValue());

#if defined(_WIN32)
  if (n == _NLSCMPERROR)
    return n;                        // Here we should raise an error
#endif   // _WIN32

  return (n > 0) ? 1 : (n < 0) ? -1 : 0;
} // end of CompareValue

/***********************************************************************/
/*  Compute a function on a string.                                    */
/***********************************************************************/
bool TYPVAL<PSZ>::Compute(PGLOBAL g, PVAL *vp, int np, OPVAL op)
{
  char *p[2], val[2][32];
  int   i;

	if (trace(1))
		htrc("Compute: np=%d op=%d\n", np, op);

	for (i = 0; i < np; i++)
		if (!vp[i]->IsNull()) {
			p[i] = vp[i]->GetCharString(val[i]);

			if (trace(1))
				htrc("p[%d]=%s\n", i, p[i]);

		} else
			return false;

	switch (op) {
		case OP_CNC:
			assert(np == 1 || np == 2);

			if (np == 2)
				SetValue_psz(p[0]);

			if ((i = Len - (signed)strlen(Strp)) > 0)
				strncat(Strp, p[np - 1], i);

			if (trace(1))
				htrc("Strp=%s\n", Strp);

			break;
		case OP_MIN:
			assert(np == 2);
			SetValue_psz((strcmp(p[0], p[1]) < 0) ? p[0] : p[1]);
			break;
		case OP_MAX:
			assert(np == 2);
			SetValue_psz((strcmp(p[0], p[1]) > 0) ? p[0] : p[1]);
			break;
		default:
			//    sprintf(g->Message, MSG(BAD_EXP_OPER), op);
			strcpy(g->Message, "Function not supported");
			return true;
	} // endswitch op

	Null = false;
  return false;
} // end of Compute

/***********************************************************************/
/*  FormatValue: This function set vp (a STRING value) to the string   */
/*  constructed from its own value formated using the fmt format.      */
/*  This function assumes that the format matches the value type.      */
/***********************************************************************/
bool TYPVAL<PSZ>::FormatValue(PVAL vp, PCSZ fmt)
{
  char *buf = (char*)vp->GetTo_Val();        // Should be big enough
  int   n = sprintf(buf, fmt, Strp);

  return (n > vp->GetValLen());
} // end of FormatValue

/***********************************************************************/
/*  STRING SetFormat function (used to set SELECT output format).      */
/***********************************************************************/
bool TYPVAL<PSZ>::SetConstFormat(PGLOBAL, FORMAT& fmt)
{
  fmt.Type[0] = 'C';
  fmt.Length = Len;
  fmt.Prec = 0;
  return false;
} // end of SetConstFormat

/***********************************************************************/
/*  Make string output of an object value.                             */
/***********************************************************************/
void TYPVAL<PSZ>::Prints(PGLOBAL g, char *ps, uint z)
{
	if (Null)
		strncpy(ps, "null", z);
	else
		strcat(strncat(strncpy(ps, "\"", z), Strp, z-2), "\"");

} // end of Prints

/* -------------------------- Class DECIMAL -------------------------- */

/***********************************************************************/
/*  DECIMAL public constructor from a constant string.                 */
/***********************************************************************/
DECVAL::DECVAL(PSZ s) : TYPVAL<PSZ>(s)
{
  if (s) {
    char *p = strchr(Strp, '.');

    Prec = (p) ? (int)(Len - (p - Strp)) : 0;
  } // endif s

  Type = TYPE_DECIM;
} // end of DECVAL constructor

/***********************************************************************/
/*  DECIMAL public constructor from char.                              */
/***********************************************************************/
DECVAL::DECVAL(PGLOBAL g, PSZ s, int n, int prec, bool uns)
      : TYPVAL<PSZ>(g, s, n + (prec ? 1 : 0) + (uns ? 0 : 1), 0)
{
  Prec = prec;
  Unsigned = uns;
  Type = TYPE_DECIM;
} // end of DECVAL constructor

/***********************************************************************/
/*  DECIMAL: Check whether the numerica value is equal to 0.           */
/***********************************************************************/
bool DECVAL::IsZero(void)
{
  for (int i = 0; Strp[i]; i++)
    if (!strchr("0 +-.", Strp[i]))
      return false;

  return true;
} // end of IsZero

/***********************************************************************/
/*  DECIMAL: Reset value to zero.                                      */
/***********************************************************************/
void DECVAL::Reset(void)
{
  int i = 0;

  Strp[i++] = '0';

  if (Prec) {
    Strp[i++] = '.';

    do {
      Strp[i++] = '0';
      } while (i < Prec + 2);

  } // endif Prec

  Strp[i] = 0;
} // end of Reset

/***********************************************************************/
/*  DECIMAL ShowValue: get string representation right justified.      */
/***********************************************************************/
int DECVAL::ShowValue(char *buf, int len)
{
  return snprintf(buf, len + 1, Xfmt, len, Strp);
} // end of ShowValue

/***********************************************************************/
/*  GetBinValue: fill a buffer with the internal binary value.         */
/*  This function checks whether the buffer length is enough and       */
/*  returns true if not. Actual filling occurs only if go is true.     */
/*  Currently used by WriteColumn of binary files.                     */
/***********************************************************************/
bool DECVAL::GetBinValue(void *buf, int buflen, bool go)
{
  int len = (Null) ? 0 : strlen(Strp);

  if (len > buflen)
    return true;
  else if (go) {
    memset(buf, ' ', buflen - len);
    memcpy((char*)buf + buflen - len, Strp, len);
  } // endif go

  return false;
} // end of GetBinValue

/***********************************************************************/
/*  DECIMAL compare value with another Value.                          */
/***********************************************************************/
bool DECVAL::IsEqual(PVAL vp, bool chktype)
{
  if (this == vp)
    return true;
  else if (chktype && Type != vp->GetType())
    return false;
  else if (Null || vp->IsNull())
    return false;

  char buf[64];

  return !strcmp(Strp, vp->GetCharString(buf));
} // end of IsEqual

/***********************************************************************/
/*  Compare values and returns 1, 0 or -1 according to comparison.     */
/*  This function is used for evaluation of numeric filters.           */
/***********************************************************************/
int DECVAL::CompareValue(PVAL vp)
{
//assert(vp->GetType() == Type);

  // Process filtering on numeric values.
  double f = atof(Strp), n = vp->GetFloatValue();

//if (trace(1))
//  htrc(" Comparing: val=%d,%d\n", f, n);

  return (f > n) ? 1 : (f < n) ? (-1) : 0;
} // end of CompareValue

/* -------------------------- Class BINVAL --------------------------- */

/***********************************************************************/
/*  BINVAL public constructor from bytes.                              */
/***********************************************************************/
BINVAL::BINVAL(PGLOBAL g, void *p, int cl, int n) : VALUE(TYPE_BIN)
{
  assert(g);
	Len = n;
	Clen = cl;
	Binp = PlugSubAlloc(g, NULL, Clen + 1);
  memset(Binp, 0, Clen + 1);

  if (p)
    memcpy(Binp, p, MY_MIN(Len,Clen));

  Chrp = NULL;
} // end of BINVAL constructor

/***********************************************************************/
/*  BINVAL: Check whether the hexadecimal value is equal to 0.         */
/***********************************************************************/
bool BINVAL::IsZero(void)
{
  for (int i = 0; i < Len; i++)
    if (((char*)Binp)[i] != 0)
      return false;

  return true;
} // end of IsZero

/***********************************************************************/
/*  BINVAL: Reset value to zero.                                       */
/***********************************************************************/
void BINVAL::Reset(void) 
{
  memset(Binp, 0, Clen);
  Len = 0;
} // end of Reset

/***********************************************************************/
/*  Get the tiny value pointed by Binp.                                */
/***********************************************************************/
char BINVAL::GetTinyValue(void)
{
  return *(char*)Binp;
} // end of GetTinyValue

/***********************************************************************/
/*  Get the unsigned tiny value pointed by Binp.                       */
/***********************************************************************/
uchar BINVAL::GetUTinyValue(void)
{
  return *(uchar*)Binp;
} // end of GetUTinyValue

/***********************************************************************/
/*  Get the short value pointed by Binp.                               */
/***********************************************************************/
short BINVAL::GetShortValue(void)
{
  if (Len >= 2)
    return *(short*)Binp;
  else
    return (short)GetTinyValue();

} // end of GetShortValue

/***********************************************************************/
/*  Get the unsigned short value pointed by Binp.                      */
/***********************************************************************/
ushort BINVAL::GetUShortValue(void)
{
  return (ushort)GetShortValue(); 
} // end of GetUshortValue

/***********************************************************************/
/*  Get the integer value pointed by Binp.                             */
/***********************************************************************/
int BINVAL::GetIntValue(void)
{
  if (Len >= 4)
    return *(int*)Binp;
  else
    return (int)GetShortValue();

} // end of GetIntValue

/***********************************************************************/
/*  Get the unsigned integer value pointed by Binp.                    */
/***********************************************************************/
uint BINVAL::GetUIntValue(void)
{
  return (uint)GetIntValue(); 
} // end of GetUintValue

/***********************************************************************/
/*  Get the big integer value pointed by Binp.                         */
/***********************************************************************/
longlong BINVAL::GetBigintValue(void)
{
  if (Len >= 8)
    return *(longlong*)Binp;
  else
    return (longlong)GetIntValue();

} // end of GetBigintValue

/***********************************************************************/
/*  Get the unsigned big integer value pointed by Binp.                */
/***********************************************************************/
ulonglong BINVAL::GetUBigintValue(void)
{
  return (ulonglong)GetBigintValue(); 
} // end of GetUBigintValue

/***********************************************************************/
/*  Get the double value pointed by Binp.                              */
/***********************************************************************/
double BINVAL::GetFloatValue(void)
{
  if (Len >= 8) 
    return *(double*)Binp;
  else if (Len >= 4)
    return (double)(*(float*)Binp);
  else
    return 0.0;

} // end of GetFloatValue

/***********************************************************************/
/*  BINVAL SetValue: copy the value of another Value object.           */
/***********************************************************************/
bool BINVAL::SetValue_pval(PVAL valp, bool chktype)
{
  bool rc = false;
      
  if (valp != this) {
    if (chktype && (valp->GetType() != Type || valp->GetSize() > Clen))
      return true;

    if (!(Null = valp->IsNull() && Nullable)) {
			int len = Len;

			if ((rc = (Len = valp->GetSize()) > Clen))
        Len = Clen;
			else if (len > Len)
				memset(Binp, 0, len);

      memcpy(Binp, valp->GetTo_Val(), Len);
			((char*)Binp)[Len] = 0;
    } else
      Reset();

  } // endif valp

  return rc;
} // end of SetValue_pval

/***********************************************************************/
/*  BINVAL SetValue: fill value with chars extracted from a line.      */
/***********************************************************************/
bool BINVAL::SetValue_char(const char *p, int n)
{
  bool rc;

  if (p && n > 0) {
		int len = Len;

    if (len > (Len = MY_MIN(n, Clen)))
			memset(Binp, 0, len);

		memcpy(Binp, p, Len);
		((char*)Binp)[Len] = 0;
		rc = n > Clen;
		Null = false;
  } else {
    rc = false;
    Reset();
    Null = Nullable;
  } // endif p

  return rc;
} // end of SetValue_char

/***********************************************************************/
/*  BINVAL SetValue: fill value with another string.                   */
/***********************************************************************/
void BINVAL::SetValue_psz(PCSZ s)
{
  if (s) {
		int len = Len;

		if (len > (Len = MY_MIN(Clen, (signed)strlen(s))))
			memset(Binp, 0, len);

		memcpy(Binp, s, Len);
		((char*)Binp)[Len] = 0;
		Null = false;
  } else {
    Reset();
    Null = Nullable;
  } // endif s

} // end of SetValue_psz

/***********************************************************************/
/*  BINVAL SetValue: fill value with bytes extracted from a block.     */
/***********************************************************************/
void BINVAL::SetValue_pvblk(PVBLK blk, int n)
{
  // STRBLK's can return a NULL pointer
  void *vp = blk->GetValPtrEx(n);

  if (!vp || blk->IsNull(n)) {
    Reset();
    Null = Nullable;
  } else if (vp != Binp) {
		int len = Len;

    if (blk->GetType() == TYPE_STRING)
      Len = strlen((char*)vp);
    else
      Len = blk->GetVlen();

    if (len > (Len = MY_MIN(Clen, Len)))
			memset(Binp, 0, len);

    memcpy(Binp, vp, Len);
		((char*)Binp)[Len] = 0;
		Null = false;
  } // endif vp

} // end of SetValue_pvblk

/***********************************************************************/
/*  BINVAL SetValue: get the binary representation of an integer.      */
/***********************************************************************/
void BINVAL::SetValue(int n)
{
  if (Clen >= 4) {
		if (Len > 4)
			memset(Binp, 0, Len);

		*((int*)Binp) = n;
    Len = 4;
  } else
    SetValue((short)n);

} // end of SetValue

/***********************************************************************/
/*  BINVAL SetValue: get the binary representation of an uint.         */
/***********************************************************************/
void BINVAL::SetValue(uint n)
{
  if (Clen >= 4) {
		if (Len > 4)
			memset(Binp, 0, Len);

		*((uint*)Binp) = n;
    Len = 4;
  } else
    SetValue((ushort)n);

} // end of SetValue

/***********************************************************************/
/*  BINVAL SetValue: get the binary representation of a short int.     */
/***********************************************************************/
void BINVAL::SetValue(short i)
{
  if (Clen >= 2) {
		if (Len > 2)
			memset(Binp, 0, Len);

		*((int*)Binp) = i;
    Len = 2;
  } else
    SetValue((char)i);

} // end of SetValue

/***********************************************************************/
/*  BINVAL SetValue: get the binary representation of a ushort int.    */
/***********************************************************************/
void BINVAL::SetValue(ushort i)
{
  if (Clen >= 2) {
		if (Len > 2)
			memset(Binp, 0, Len);

		*((uint*)Binp) = i;
    Len = 2;
  } else
    SetValue((uchar)i);

} // end of SetValue

/***********************************************************************/
/*  BINVAL SetValue: get the binary representation of a big integer.   */
/***********************************************************************/
void BINVAL::SetValue(longlong n)
{
  if (Clen >= 8) {
		if (Len > 8)
			memset(Binp, 0, Len);

		*((longlong*)Binp) = n;
    Len = 8;
  } else
    SetValue((int)n);

} // end of SetValue

/***********************************************************************/
/*  BINVAL SetValue: get the binary representation of a big integer.   */
/***********************************************************************/
void BINVAL::SetValue(ulonglong n)
{
  if (Clen >= 8) {
		if (Len > 8)
			memset(Binp, 0, Len);

		*((ulonglong*)Binp) = n;
    Len = 8;
  } else
    SetValue((uint)n);

} // end of SetValue

/***********************************************************************/
/*  BINVAL SetValue: get the binary representation of a double.        */
/***********************************************************************/
void BINVAL::SetValue(double n)
{
	if (Len > 8)
	  memset(Binp, 0, Len);

  if (Clen >= 8) {
    *((double*)Binp) = n;
    Len = 8;
  } else if (Clen >= 4) {
    *((float*)Binp) = (float)n;
    Len = 4;
  } else
    Len = 0;
  
} // end of SetValue

/***********************************************************************/
/*  BINVAL SetValue: get the character binary of a tiny int.           */
/***********************************************************************/
void BINVAL::SetValue(char c)
{
	if (Len > 1)
		memset(Binp, 0, Len);

	*((char*)Binp) = c;
  Len = 1;
} // end of SetValue

/***********************************************************************/
/*  BINVAL SetValue: get the binary representation of a tiny int.      */
/***********************************************************************/
void BINVAL::SetValue(uchar c)
{
	if (Len > 1)
		memset(Binp, 0, Len);

	*((uchar*)Binp) = c;
  Len = 1;
} // end of SetValue

/***********************************************************************/
/*  BINVAL SetBinValue: fill string with bytes extracted from a line.  */
/***********************************************************************/
void BINVAL::SetBinValue(void *p)
{
  memcpy(Binp, p, Clen);
	Len = Clen;
} // end of SetBinValue

/***********************************************************************/
/*  BINVAL SetBinValue: fill string with len bytes.                    */
/***********************************************************************/
void BINVAL::SetBinValue(void* p, ulong len)
{
	memcpy(Binp, p, len);
	Len = len;
} // end of SetBinValue

/***********************************************************************/
/*  GetBinValue: fill a buffer with the internal binary value.         */
/*  This function checks whether the buffer length is enough and       */
/*  returns true if not. Actual filling occurs only if go is true.     */
/*  Currently used by WriteColumn of binary files.                     */
/***********************************************************************/
bool BINVAL::GetBinValue(void *buf, int buflen, bool go)
{
  if (Len > buflen)
    return true;
  else if (go) {
    memset(buf, 0, buflen);
    memcpy(buf, Binp, Len);
  } // endif go

  return false;
} // end of GetBinValue

/***********************************************************************/
/*  BINVAL ShowValue: get string representation of a binary value.     */
/***********************************************************************/
int BINVAL::ShowValue(char *buf, int len)
{
	memset(buf, 0, len + 1);
	memcpy(buf, Binp, MY_MIN(len, Len));
	return Len;
} // end of ShowValue

/***********************************************************************/
/*  BINVAL GetCharString: get string representation of a binary value. */
/***********************************************************************/
char *BINVAL::GetCharString(char *)
{
  if (!Chrp)
    Chrp = (char*)PlugSubAlloc(Global, NULL, Clen * 2 + 1);

  sprintf(Chrp, GetXfmt(), Len, Binp); 
  return Chrp;
} // end of GetCharString

/***********************************************************************/
/*  BINVAL compare value with another Value.                           */
/***********************************************************************/
bool BINVAL::IsEqual(PVAL vp, bool chktype)
{
  if (this == vp)
    return true;
  else if (chktype && Type != vp->GetType())
    return false;
  else if (Null || vp->IsNull())
    return false;
  else if (Len != vp->GetSize())
    return false;

  char *v1 = (char*)Binp;
  char *v2 = (char*)vp->GetTo_Val();

  for (int i = 0; i < Len; i++)
    if (v1[i] != v2[i])
      return false;

  return true;
} // end of IsEqual

/***********************************************************************/
/*  FormatValue: This function set vp (a STRING value) to the string   */
/*  constructed from its own value formated using the fmt format.      */
/*  This function assumes that the format matches the value type.      */
/***********************************************************************/
bool BINVAL::FormatValue(PVAL vp, PCSZ fmt)
{
  char *buf = (char*)vp->GetTo_Val();        // Should be big enough
  int   n = sprintf(buf, fmt, Len, Binp);

  return (n > vp->GetValLen());
} // end of FormatValue

/***********************************************************************/
/*  BINVAL SetFormat function (used to set SELECT output format).      */
/***********************************************************************/
bool BINVAL::SetConstFormat(PGLOBAL, FORMAT& fmt)
{
  fmt.Type[0] = 'B';
  fmt.Length = Clen;
  fmt.Prec = 0;
  return false;
} // end of SetConstFormat

/* -------------------------- Class DTVAL ---------------------------- */

/***********************************************************************/
/*  DTVAL  public constructor for new void values.                     */
/***********************************************************************/
DTVAL::DTVAL(PGLOBAL g, int n, int prec, PCSZ fmt)
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
DTVAL::DTVAL(int n) : TYPVAL<int>(n, TYPE_DATE)
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
bool DTVAL::SetFormat(PGLOBAL g, PCSZ fmt, int len, int year)
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
  struct tm dtm;
  memset(&dtm, 0, sizeof(dtm));
  dtm.tm_mday=2;
  dtm.tm_mon=0;
  dtm.tm_year=70;

  Shift = (int)mktime(&dtm) - 86400;

  if (trace(1))
    htrc("DTVAL Shift=%d\n", Shift);

} // end of SetTimeShift

// Added by Alexander Barkov
static void TIME_to_localtime(struct tm *tm, const MYSQL_TIME *ltime)
{
  bzero(tm, sizeof(*tm));
  tm->tm_year= ltime->year - 1900;
  tm->tm_mon=  ltime->month - 1;
  tm->tm_mday= ltime->day;
  mktime(tm);  // set tm->tm_wday tm->yday fields to get proper day name (OB)
  tm->tm_hour= ltime->hour;
  tm->tm_min=  ltime->minute;
  tm->tm_sec=  ltime->second;
} // end of TIME_to_localtime

// Added by Alexander Barkov
static struct tm *gmtime_mysql(const time_t *timep, struct tm *tm)
{
  MYSQL_TIME ltime;
  thd_gmt_sec_to_TIME(current_thd, &ltime, (my_time_t) *timep);
  TIME_to_localtime(tm, &ltime);
  return tm;
} // end of gmtime_mysql

/***********************************************************************/
/*  GetGmTime: returns a pointer to a static tm structure obtained     */
/*  though the gmtime C function. The purpose of this function is to   */
/*  extend the range of valid dates by accepting negative time values. */
/***********************************************************************/
struct tm *DTVAL::GetGmTime(struct tm *tm_buffer)
{
  struct tm *datm;
  time_t t = (time_t)Tval;

  if (Tval < 0) {
    int    n;

    for (n = 0; t < 0; n += 4)
      t += FOURYEARS;

    datm = gmtime_mysql(&t, tm_buffer);

    if (datm)
      datm->tm_year -= n;

  } else
    datm = gmtime_mysql(&t, tm_buffer);

  return datm;
} // end of GetGmTime

// Added by Alexander Barkov
static time_t mktime_mysql(struct tm *ptm)
{
  MYSQL_TIME ltime;
  localtime_to_TIME(&ltime, ptm);
  ltime.time_type= MYSQL_TIMESTAMP_DATETIME;
  uint error_code;
  time_t t= TIME_to_timestamp(current_thd, &ltime, &error_code);
  return error_code ? (time_t) -1 : t;
}

/***********************************************************************/
/*  MakeTime: calculates a date value from a tm structures using the   */
/*  mktime C function. The purpose of this function is to extend the   */
/*  range of valid dates by accepting to set negative time values.     */
/***********************************************************************/
bool DTVAL::MakeTime(struct tm *ptm)
{
  int    n, y = ptm->tm_year;
  time_t t = mktime_mysql(ptm);

  if (trace(2))
    htrc("MakeTime from (%d,%d,%d,%d,%d,%d)\n",
          ptm->tm_year, ptm->tm_mon, ptm->tm_mday,
          ptm->tm_hour, ptm->tm_min, ptm->tm_sec);

  if (t == -1) {
    if (y < 1 || y > 71)
      return true;

    for (n = 0; t == -1 && n < 20; n++) {
      ptm->tm_year += 4;
      t = mktime_mysql(ptm);
    } // endfor t

    if (t == -1)
      return true;

    if ((t -= (n * FOURYEARS)) > 2000000000)
      return true;

  } // endif t

  Tval= (int) t;

  if (trace(2))
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
  struct tm datm;
  bzero(&datm, sizeof(datm));
  datm.tm_mday=1;
  datm.tm_mon=0;
  datm.tm_year=70;

  if (trace(2))
    htrc("MakeDate from(%d,%d,%d,%d,%d,%d) nval=%d\n",
    val[0], val[1], val[2], val[3], val[4], val[5], nval);

  for (i = 0; i < nval; i++) {
    n = val[i];

//    if (trace(2))
//      htrc("i=%d n=%d\n", i, n);

    switch (i) {
      case 0:
        if (n >= 1900)
          n -= 1900;

        datm.tm_year = n;

//        if (trace(2))
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

//        if (trace(2))
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

//        if (trace(2))
//          htrc("n=%d m=%d tm_year=%d tm_mon=%d\n", n, m, datm.tm_year, datm.tm_mon);

       break;
      case 3: datm.tm_hour = n; break;
      case 4: datm.tm_min  = n; break;
      case 5: datm.tm_sec  = n; break;
    } // endswitch i

  } // endfor i

  if (trace(2))
    htrc("MakeDate datm=(%d,%d,%d,%d,%d,%d)\n",
    datm.tm_year, datm.tm_mon, datm.tm_mday,
    datm.tm_hour, datm.tm_min, datm.tm_sec);

  // Pass g to have an error return or NULL to set invalid dates to 0
  if (MakeTime(&datm))
  {
    if (g) {
      strcpy(g->Message, MSG(BAD_DATETIME));
      rc = true;
    } else
      Tval = 0;
  }
  return rc;
} // end of MakeDate

/***********************************************************************/
/*  DTVAL SetValue: copy the value of another Value object.            */
/*  This function allows conversion if chktype is false.               */
/***********************************************************************/
bool DTVAL::SetValue_pval(PVAL valp, bool chktype)
{
  if (valp != this) {
    if (chktype && Type != valp->GetType())
      return true;

    if (!(Null = valp->IsNull() && Nullable)) {
      if (Pdtp && !valp->IsTypeNum()) {
        int   ndv;
        int  dval[6];

        ndv = ExtractDate(valp->GetCharValue(), Pdtp, DefYear, dval);
        MakeDate(NULL, dval, ndv);
			} else if (valp->GetType() == TYPE_BIGINT &&
				       !(valp->GetBigintValue() % 1000)) {
				// Assuming that this timestamp is in milliseconds
				SetValue((int)(valp->GetBigintValue() / 1000));
			}	else
        SetValue(valp->GetIntValue());

    } else
      Reset();

  } // endif valp

  return false;
} // end of SetValue

/***********************************************************************/
/*  SetValue: convert chars extracted from a line to date value.       */
/***********************************************************************/
bool DTVAL::SetValue_char(const char *p, int n)
{
  bool rc= 0;

  if (Pdtp) {
    const char *p2;
    int ndv;
    int dval[6];

		if (n > 0) {
			// Trim trailing blanks
			for (p2 = p + n -1; p < p2 && *p2 == ' '; p2--);

			if ((rc = (n = (int)(p2 - p + 1)) > Len))
				n = Len;

			memcpy(Sdate, p, n);
		} // endif n

    Sdate[n] = '\0';

    ndv = ExtractDate(Sdate, Pdtp, DefYear, dval);
    MakeDate(NULL, dval, ndv);

    if (trace(2))
      htrc(" setting date: '%s' -> %d\n", Sdate, Tval);

    Null = (Nullable && ndv == 0);
  } else {
    rc = TYPVAL<int>::SetValue_char(p, n);
    Null = (Nullable && Tval == 0);
  } // endif Pdtp

  return rc;
} // end of SetValue

/***********************************************************************/
/*  SetValue: convert a char string to date value.                     */
/***********************************************************************/
void DTVAL::SetValue_psz(PCSZ p)
{
  if (Pdtp) {
    int ndv;
    int dval[6];

    strncpy(Sdate, p, Len);
    Sdate[Len] = '\0';

    ndv = ExtractDate(Sdate, Pdtp, DefYear, dval);
    MakeDate(NULL, dval, ndv);

    if (trace(2))
      htrc(" setting date: '%s' -> %d\n", Sdate, Tval);

    Null = (Nullable && ndv == 0);
  } else {
    TYPVAL<int>::SetValue_psz(p);
    Null = (Nullable && Tval == 0);
  } // endif Pdtp

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
/*  DTVAL SetValue: get date as an integer.                            */
/***********************************************************************/
void DTVAL::SetValue(int n)
{
  Tval = n;

  if (Pdtp) {
    size_t slen = (size_t)Len + 1;
    struct tm tm, *ptm= GetGmTime(&tm);

    if (ptm)
      strftime(Sdate, slen, Pdtp->OutFmt, ptm);

  } // endif Pdtp

} // end of SetValue

/***********************************************************************/
/*  DTVAL GetCharString: get string representation of a date value.    */
/***********************************************************************/
char *DTVAL::GetCharString(char *p)
{
  if (Pdtp) {
    size_t n = 0, slen = (size_t)Len + 1;
    struct tm tm, *ptm= GetGmTime(&tm);

    if (ptm)
      n = strftime(Sdate, slen, Pdtp->OutFmt, ptm);

    if (!n) {
      *Sdate = '\0';
      strncat(Sdate, "Error", slen);
      } // endif n

    return Sdate;
  } else
    sprintf(p, "%d", Tval);

//Null = false;                      ??????????????
  return p;
} // end of GetCharString

/***********************************************************************/
/*  DTVAL ShowValue: get string representation of a date value.        */
/***********************************************************************/
int DTVAL::ShowValue(char *buf, int len)
{
	int rv = 0;

  if (Pdtp) {
    if (!Null) {
      size_t n = 0, m = len + 1;
      struct tm tm, *ptm = GetGmTime(&tm);
  
      if (ptm)
        n = strftime(buf, m, Pdtp->OutFmt, ptm);
  
      if (!n) {
        *buf = '\0';
        strncat(buf, "Error", m);
				rv = 5;
			} else 
				rv = (int)n;

		} else
			*buf = '\0';               // DEFAULT VALUE ???

  } else
    rv = TYPVAL<int>::ShowValue(buf, len);

	return rv;
} // end of ShowValue

#if 0           // Not used by CONNECT
/***********************************************************************/
/*  Returns a member of the struct tm representation of the date.      */
/***********************************************************************/
bool DTVAL::GetTmMember(OPVAL op, int& mval)
{
  bool       rc = false;
  struct tm tm, *ptm = GetGmTime(&tm);

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
  struct tm tm, *ptm = GetGmTime(&tm);

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
#endif // 0

/***********************************************************************/
/*  FormatValue: This function set vp (a STRING value) to the string   */
/*  constructed from its own value formated using the fmt format.      */
/*  This function assumes that the format matches the value type.      */
/***********************************************************************/
bool DTVAL::FormatValue(PVAL vp, PCSZ fmt)
{
  char     *buf = (char*)vp->GetTo_Val();       // Should be big enough
  struct tm tm, *ptm = GetGmTime(&tm);

  if (trace(2))
    htrc("FormatValue: ptm=%p len=%d\n", ptm, vp->GetValLen());

  if (ptm) {
    size_t n = strftime(buf, vp->GetValLen(), fmt, ptm);

    if (trace(2))
      htrc("strftime: n=%d buf=%s\n", n, (n) ? buf : "???");

    return (n == 0);
  } else
    return true;

} // end of FormatValue

/* -------------------------- End of Value --------------------------- */
