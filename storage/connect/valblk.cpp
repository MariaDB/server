/************ Valblk C++ Functions Source Code File (.CPP) *************/
/*  Name: VALBLK.CPP  Version 2.3                                      */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2005-2017    */
/*                                                                     */
/*  This file contains the VALBLK and derived classes functions.       */
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
/*  This is why we are now using a template class for many types.      */
/*  Currently the only implemented types are PSZ, chars, int, short,   */
/*  DATE, longlong, double and tiny. Fix numeric ones can be unsigned. */
/***********************************************************************/

/***********************************************************************/
/*  Include relevant MariaDB header file.                              */
/***********************************************************************/
#include "my_global.h"
#if defined(_WIN32)
//#include <windows.h>
#else
#include "osutil.h"
#include "string.h"
#endif

/***********************************************************************/
/*  Include required application header files                          */
/*  global.h    is header containing all global Plug declarations.     */
/*  plgdbsem.h  is header containing the DB applic. declarations.      */
/*  valblk.h    is header containing VALBLK derived classes declares.  */
/***********************************************************************/
#include "global.h"
#include "plgdbsem.h"
#include "valblk.h"

#define CheckBlanks      assert(!Blanks);
#define CheckParms(V, N) ChkIndx(N); ChkTyp(V);

extern MBLOCK Nmblk;                /* Used to initialize MBLOCK's     */

/***********************************************************************/
/*  AllocValBlock: allocate a VALBLK according to type.                */
/***********************************************************************/
PVBLK AllocValBlock(PGLOBAL g, void *mp, int type, int nval, int len,
                               int prec, bool check, bool blank, bool un)
  {
  PVBLK blkp;

  if (trace(1))
    htrc("AVB: mp=%p type=%d nval=%d len=%d check=%u blank=%u\n",
         mp, type, nval, len, check, blank);

  switch (type) {
    case TYPE_STRING:
		case TYPE_BIN:
    case TYPE_DECIM:
      if (len)
        blkp = new(g) CHRBLK(mp, nval, type, len, prec, blank);
      else
        blkp = new(g) STRBLK(g, mp, nval, type);

      break;
    case TYPE_SHORT:
      if (un)
        blkp = new(g) TYPBLK<ushort>(mp, nval, type, 0, true);
      else
        blkp = new(g) TYPBLK<short>(mp, nval, type);

      break;
    case TYPE_INT:
      if (un)
        blkp = new(g) TYPBLK<uint>(mp, nval, type, 0, true);
      else
        blkp = new(g) TYPBLK<int>(mp, nval, type);

      break;
    case TYPE_DATE:        // ?????
      blkp = new(g) DATBLK(mp, nval);
      break;
    case TYPE_BIGINT:
      if (un)
        blkp = new(g) TYPBLK<ulonglong>(mp, nval, type, 0, true);
      else
        blkp = new(g) TYPBLK<longlong>(mp, nval, type);

      break;
    case TYPE_DOUBLE:
      blkp = new(g) TYPBLK<double>(mp, nval, type, prec);
      break;
    case TYPE_TINY:
      if (un)
        blkp = new(g) TYPBLK<uchar>(mp, nval, type, 0, true);
      else
        blkp = new(g) TYPBLK<char>(mp, nval, type);

      break;
    case TYPE_PCHAR:
      blkp = new(g) PTRBLK(g, mp, nval);
      break;
    default:
      sprintf(g->Message, MSG(BAD_VALBLK_TYPE), type);
      return NULL;
    } // endswitch Type

  return (blkp->Init(g, check)) ? NULL : blkp;
  } // end of AllocValBlock

/* -------------------------- Class VALBLK --------------------------- */

/***********************************************************************/
/*  Constructor.                                                       */
/***********************************************************************/
VALBLK::VALBLK(void *mp, int type, int nval, bool un)
  {
  Mblk = Nmblk;
  Blkp = mp;
  To_Nulls = NULL;
  Check = true;
  Nullable = false;
  Unsigned = un;
  Type = type;
  Nval = nval;
  Prec = 0;
  } // end of VALBLK constructor

/***********************************************************************/
/*  Raise error for numeric types.                                     */
/***********************************************************************/
PSZ VALBLK::GetCharValue(int)
  {
  PGLOBAL& g = Global;

  assert(g);
  sprintf(g->Message, MSG(NO_CHAR_FROM), Type);
	throw Type;
	return NULL;
  } // end of GetCharValue

/***********************************************************************/
/*  Set format so formatted dates can be converted on input.           */
/***********************************************************************/
bool VALBLK::SetFormat(PGLOBAL g, PCSZ, int, int)
  {
  sprintf(g->Message, MSG(NO_DATE_FMT), Type);
  return true;
  } // end of SetFormat

/***********************************************************************/
/*  Set the index of the location of value and return true if found.   */
/*  To be used on ascending sorted arrays only.                        */
/*  Currently used by some BLKFIL classes only.                        */
/***********************************************************************/
bool VALBLK::Locate(PVAL vp, int& i)
  {
  ChkTyp(vp);

  int n = 1;

  for (i = 0; i < Nval; i++)
    if ((n = CompVal(vp, i)) <= 0)
      break;

  return (!n);
  } // end of Locate

/***********************************************************************/
/*  Set Nullable and allocate the Null array.                          */
/***********************************************************************/
void VALBLK::SetNullable(bool b)
  {
  if ((Nullable = b)) {
    To_Nulls = (char*)PlugSubAlloc(Global, NULL, Nval);
    memset(To_Nulls, 0, Nval);
  } else
    To_Nulls = NULL;

  } // end of SetNullable

/***********************************************************************/
/*  Buffer allocation routine.                                         */
/***********************************************************************/
bool VALBLK::AllocBuff(PGLOBAL g, size_t size)
  {
  Mblk.Size = size;

  if (!(Blkp = PlgDBalloc(g, NULL, Mblk))) {
    sprintf(g->Message, MSG(MEM_ALLOC_ERR), "Blkp", (int) Mblk.Size);
    fprintf(stderr, "%s\n", g->Message);
    return true;
    } // endif Blkp

  return false;
  } // end of AllocBuff

/***********************************************************************/
/*  Check functions.                                                   */
/***********************************************************************/
void VALBLK::ChkIndx(int n)
  {
  if (n < 0 || n >= Nval) {
    PGLOBAL& g = Global;
		xtrc(1, "ChkIndx: n=%d Nval=%d\n", n, Nval);
    strcpy(g->Message, MSG(BAD_VALBLK_INDX));
		throw Type;
	} // endif n

  } // end of ChkIndx

void VALBLK::ChkTyp(PVAL v)
  {
  if (Check && (Type != v->GetType() || Unsigned != v->IsUnsigned())) {
    PGLOBAL& g = Global;
		xtrc(1, "ChkTyp: Type=%d valType=%d\n", Type, v->GetType());
		strcpy(g->Message, MSG(VALTYPE_NOMATCH));
		throw Type;
	} // endif Type

  } // end of ChkTyp

void VALBLK::ChkTyp(PVBLK vb)
  {
  if (Check && (Type != vb->GetType() || Unsigned != vb->IsUnsigned())) {
    PGLOBAL& g = Global;
    strcpy(g->Message, MSG(VALTYPE_NOMATCH));
		throw Type;
	} // endif Type

  } // end of ChkTyp

/* -------------------------- Class TYPBLK --------------------------- */

/***********************************************************************/
/*  Constructor.                                                       */
/***********************************************************************/
template <class TYPE>
TYPBLK<TYPE>::TYPBLK(void *mp, int nval, int type, int prec, bool un)
            : VALBLK(mp, type, nval, un), Typp((TYPE*&)Blkp)
  {
  Prec = prec;
  Fmt = GetFmt(Type);
  } // end of TYPBLK constructor

/***********************************************************************/
/*  Initialization routine.                                            */
/***********************************************************************/
template <class TYPE>
bool TYPBLK<TYPE>::Init(PGLOBAL g, bool check)
  {
  if (!Blkp)
    if (AllocBuff(g, Nval * sizeof(TYPE)))
      return true;

  Check = check;
  Global = g;
  return false;
  } // end of Init

/***********************************************************************/
/*  TYPVAL GetCharString: get string representation of a typed value.  */
/***********************************************************************/
template <class TYPE>
char *TYPBLK<TYPE>::GetCharString(char *p, int n)
  {
  sprintf(p, Fmt, UnalignedRead(n));
  return p;
  } // end of GetCharString

template <>
char *TYPBLK<double>::GetCharString(char *p, int n)
  {
  sprintf(p, Fmt, Prec, UnalignedRead(n));
  return p;
  } // end of GetCharString

/***********************************************************************/
/*  Set one value in a block.                                          */
/***********************************************************************/
template <class TYPE>
void TYPBLK<TYPE>::SetValue(PVAL valp, int n)
  {
  bool b;

  ChkIndx(n);
  ChkTyp(valp);

  if (!(b = valp->IsNull()))
    UnalignedWrite(n, GetTypedValue(valp));
  else
    Reset(n);

  SetNull(n, b && Nullable);
  } // end of SetValue

template <>
int TYPBLK<int>::GetTypedValue(PVAL valp)
  {return valp->GetIntValue();}

template <>
uint TYPBLK<uint>::GetTypedValue(PVAL valp)
  {return valp->GetUIntValue();}

template <>
short TYPBLK<short>::GetTypedValue(PVAL valp)
  {return valp->GetShortValue();}

template <>
ushort TYPBLK<ushort>::GetTypedValue(PVAL valp)
  {return valp->GetUShortValue();}

template <>
longlong TYPBLK<longlong>::GetTypedValue(PVAL valp)
  {return valp->GetBigintValue();}

template <>
ulonglong TYPBLK<ulonglong>::GetTypedValue(PVAL valp)
  {return valp->GetUBigintValue();}

template <>
double TYPBLK<double>::GetTypedValue(PVAL valp)
  {return valp->GetFloatValue();}

template <>
char TYPBLK<char>::GetTypedValue(PVAL valp)
  {return valp->GetTinyValue();}

template <>
uchar TYPBLK<uchar>::GetTypedValue(PVAL valp)
  {return valp->GetUTinyValue();}

/***********************************************************************/
/*  Set one value in a block from a zero terminated string.            */
/***********************************************************************/
template <class TYPE>
void TYPBLK<TYPE>::SetValue(PCSZ p, int n)
  {
  ChkIndx(n);

  if (Check) {
    PGLOBAL& g = Global;
    strcpy(g->Message, MSG(BAD_SET_STRING));
		throw Type;
	} // endif Check

  bool      minus;
  ulonglong maxval = MaxVal();
  ulonglong val = CharToNumber(p, strlen(p), maxval, Unsigned, &minus); 
    
  if (minus && val < maxval)
    UnalignedWrite(n, (TYPE)(-(signed)val));
  else
    UnalignedWrite(n, (TYPE)val);

  SetNull(n, false);
  } // end of SetValue

template <class TYPE>
ulonglong TYPBLK<TYPE>::MaxVal(void) {DBUG_ASSERT(false); return 0;}

template <>
ulonglong TYPBLK<short>::MaxVal(void) {return INT_MAX16;}

template <>
ulonglong TYPBLK<ushort>::MaxVal(void) {return UINT_MAX16;}

template <>
ulonglong TYPBLK<int>::MaxVal(void) {return INT_MAX32;}

template <>
ulonglong TYPBLK<uint>::MaxVal(void) {return UINT_MAX32;}

template <>
ulonglong TYPBLK<char>::MaxVal(void) {return INT_MAX8;}

template <>
ulonglong TYPBLK<uchar>::MaxVal(void) {return UINT_MAX8;}

template <>
ulonglong TYPBLK<longlong>::MaxVal(void) {return INT_MAX64;}

template <>
ulonglong TYPBLK<ulonglong>::MaxVal(void) {return ULONGLONG_MAX;}

template <>
void TYPBLK<double>::SetValue(PCSZ p, int n)
  {
  ChkIndx(n);

  if (Check) {
    PGLOBAL& g = Global;
    strcpy(g->Message, MSG(BAD_SET_STRING));
		throw Type;
	} // endif Check

  UnalignedWrite(n, atof(p));
  SetNull(n, false);
  } // end of SetValue

/***********************************************************************/
/*  Set one value in a block from an array of characters.              */
/***********************************************************************/
template <class TYPE>
void TYPBLK<TYPE>::SetValue(PCSZ sp, uint len, int n)
  {
  PGLOBAL& g = Global;
  PSZ spz = (PSZ)PlugSubAlloc(g, NULL, 0);    // Temporary

  if (sp)
    memcpy(spz, sp, len);

  spz[len] = 0;
  SetValue(spz, n);
  } // end of SetValue

/***********************************************************************/
/*  Set one value in a block from a value in another block.            */
/***********************************************************************/
template <class TYPE>
void TYPBLK<TYPE>::SetValue(PVBLK pv, int n1, int n2)
  {
  bool b;

  ChkIndx(n1);
  ChkTyp(pv);

  if (!(b = pv->IsNull(n2) && Nullable))
    UnalignedWrite(n1, GetTypedValue(pv, n2));
  else
    Reset(n1);

  SetNull(n1, b);
  } // end of SetValue

template <>
int TYPBLK<int>::GetTypedValue(PVBLK blk, int n)
  {return blk->GetIntValue(n);}

template <>
uint TYPBLK<uint>::GetTypedValue(PVBLK blk, int n)
  {return blk->GetUIntValue(n);}

template <>
short TYPBLK<short>::GetTypedValue(PVBLK blk, int n)
  {return blk->GetShortValue(n);}

template <>
ushort TYPBLK<ushort>::GetTypedValue(PVBLK blk, int n)
  {return blk->GetUShortValue(n);}

template <>
longlong TYPBLK<longlong>::GetTypedValue(PVBLK blk, int n)
  {return blk->GetBigintValue(n);}

template <>
ulonglong TYPBLK<ulonglong>::GetTypedValue(PVBLK blk, int n)
  {return blk->GetUBigintValue(n);}

template <>
double TYPBLK<double>::GetTypedValue(PVBLK blk, int n)
  {return blk->GetFloatValue(n);}

template <>
char TYPBLK<char>::GetTypedValue(PVBLK blk, int n)
  {return blk->GetTinyValue(n);}

template <>
uchar TYPBLK<uchar>::GetTypedValue(PVBLK blk, int n)
  {return blk->GetUTinyValue(n);}

/***********************************************************************/
/*  Set one value in a block if val is less than the current value.    */
/***********************************************************************/
template <class TYPE>
void TYPBLK<TYPE>::SetMin(PVAL valp, int n)
  {
  CheckParms(valp, n)
  TYPE  tval = GetTypedValue(valp);
  TYPE  tmin = UnalignedRead(n);

  if (tval < tmin)
    UnalignedWrite(n, tval);

  } // end of SetMin

/***********************************************************************/
/*  Set one value in a block if val is greater than the current value. */
/***********************************************************************/
template <class TYPE>
void TYPBLK<TYPE>::SetMax(PVAL valp, int n)
  {
  CheckParms(valp, n)
  TYPE  tval = GetTypedValue(valp);
  TYPE  tmin = UnalignedRead(n);

  if (tval > tmin)
    UnalignedWrite(n, tval);

  } // end of SetMax

#if 0
/***********************************************************************/
/*  Set many values in a block from values in another block.           */
/***********************************************************************/
template <class TYPE>
void TYPBLK<TYPE>::SetValues(PVBLK pv, int k, int n)
  {
  CheckType(pv)
  TYPE *lp = ((TYPBLK*)pv)->Typp;

  memcpy(Typp + k, lp + k, sizeof(TYPE) * n);

  } // end of SetValues
#endif // 0

/***********************************************************************/
/*  Move one value from i to j.                                        */
/***********************************************************************/
template <class TYPE>
void TYPBLK<TYPE>::Move(int i, int j)
  {
  UnalignedWrite(j, UnalignedRead(i));
  MoveNull(i, j);
  } // end of Move

/***********************************************************************/
/*  Compare a Value object with the nth value of the block.            */
/***********************************************************************/
template <class TYPE>
int TYPBLK<TYPE>::CompVal(PVAL vp, int n)
  {
#if defined(_DEBUG)
  ChkIndx(n);
  ChkTyp(vp);
#endif   // _DEBUG
  TYPE mlv = UnalignedRead(n);
  TYPE vlv = GetTypedValue(vp);

  return (vlv > mlv) ? 1 : (vlv < mlv) ? (-1) : 0;
  } // end of CompVal

/***********************************************************************/
/*  Compare two values of the block.                                   */
/***********************************************************************/
template <class TYPE>
int TYPBLK<TYPE>::CompVal(int i1, int i2)
  {
  TYPE lv1 = UnalignedRead(i1);
  TYPE lv2 = UnalignedRead(i2);

  return (lv1 > lv2) ? 1 : (lv1 < lv2) ? (-1) : 0;
  } // end of CompVal

/***********************************************************************/
/*  Get a pointer on the nth value of the block.                       */
/***********************************************************************/
template <class TYPE>
void *TYPBLK<TYPE>::GetValPtr(int n)
  {
  ChkIndx(n);
  return Typp + n;
  } // end of GetValPtr

/***********************************************************************/
/*  Get a pointer on the nth value of the block.                       */
/***********************************************************************/
template <class TYPE>
void *TYPBLK<TYPE>::GetValPtrEx(int n)
  {
  ChkIndx(n);
  return Typp + n;
  } // end of GetValPtrEx

/***********************************************************************/
/*  Returns index of matching value in block or -1.                    */
/***********************************************************************/
template <class TYPE>
int TYPBLK<TYPE>::Find(PVAL vp)
  {
  ChkTyp(vp);

  int  i;
  TYPE n = GetTypedValue(vp);

  for (i = 0; i < Nval; i++)
    if (n == UnalignedRead(i))
      break;

  return (i < Nval) ? i : (-1);
  } // end of Find

/***********************************************************************/
/*  Returns the length of the longest string in the block.             */
/***********************************************************************/
template <class TYPE>
int TYPBLK<TYPE>::GetMaxLength(void)
  {
  char buf[64];
  int i, n, m;

  for (i = n = 0; i < Nval; i++) {
    m = sprintf(buf, Fmt, UnalignedRead(i));
    n = MY_MAX(n, m);
    } // endfor i

  return n;
  } // end of GetMaxLength


/* -------------------------- Class CHRBLK --------------------------- */

/***********************************************************************/
/*  Constructor.                                                       */
/***********************************************************************/
CHRBLK::CHRBLK(void *mp, int nval, int type, int len, int prec, bool blank)
      : VALBLK(mp, type, nval), Chrp((char*&)Blkp)
  {
  Valp = NULL;
  Blanks = blank;
  Ci = (prec != 0);
  Long = len;
  } // end of CHRBLK constructor

/***********************************************************************/
/*  Initialization routine.                                            */
/***********************************************************************/
bool CHRBLK::Init(PGLOBAL g, bool check)
  {
  Valp = (char*)PlugSubAlloc(g, NULL, Long + 1);
  Valp[Long] = '\0';

  if (!Blkp)
    if (AllocBuff(g, Nval * Long))
      return true;

  Check = check;
  Global = g;
  return false;
  } // end of Init

/***********************************************************************/
/*  Reset nth element to a null string.                                */
/***********************************************************************/
void CHRBLK::Reset(int n)
  {
  if (Blanks)
    memset(Chrp + n * Long, ' ', Long);
  else
    *(Chrp + n * Long) = '\0';

  } // end of Reset

/***********************************************************************/
/*  Return the zero ending value of the nth element.                   */
/***********************************************************************/
char *CHRBLK::GetCharValue(int n)
  {
  return (char *)GetValPtrEx(n);
  } // end of GetCharValue

/***********************************************************************/
/*  Return the value of the nth element converted to tiny int.         */
/***********************************************************************/
char CHRBLK::GetTinyValue(int n)
  {
  bool      m;
  ulonglong val = CharToNumber((char*)GetValPtr(n), Long, INT_MAX8,
                                                    false, &m); 
    
  return (m && val < INT_MAX8) ? (char)(-(signed)val) : (char)val;
  } // end of GetTinyValue

/***********************************************************************/
/*  Return the value of the nth element converted to unsigned tiny int.*/
/***********************************************************************/
uchar CHRBLK::GetUTinyValue(int n)
  {
  return (uchar)CharToNumber((char*)GetValPtr(n), Long, UINT_MAX8, true); 
  } // end of GetTinyValue

/***********************************************************************/
/*  Return the value of the nth element converted to short.            */
/***********************************************************************/
short CHRBLK::GetShortValue(int n)
  {
  bool      m;
  ulonglong val = CharToNumber((char*)GetValPtr(n), Long, INT_MAX16,
                                                    false, &m); 
    
  return (m && val < INT_MAX16) ? (short)(-(signed)val) : (short)val;
  } // end of GetShortValue

/***********************************************************************/
/*  Return the value of the nth element converted to ushort.           */
/***********************************************************************/
ushort CHRBLK::GetUShortValue(int n)
  {
  return (ushort)CharToNumber((char*)GetValPtr(n), Long, UINT_MAX16, true); 
  } // end of GetShortValue

/***********************************************************************/
/*  Return the value of the nth element converted to int.              */
/***********************************************************************/
int CHRBLK::GetIntValue(int n)
  {
  bool      m;
  ulonglong val = CharToNumber((char*)GetValPtr(n), Long, INT_MAX32,
                                                    false, &m); 
    
  return (m && val < INT_MAX32) ? (int)(-(signed)val) : (int)val;
  } // end of GetIntValue

/***********************************************************************/
/*  Return the value of the nth element converted to uint.             */
/***********************************************************************/
uint CHRBLK::GetUIntValue(int n)
  {
  return (uint)CharToNumber((char*)GetValPtr(n), Long, UINT_MAX32, true); 
  } // end of GetIntValue

/***********************************************************************/
/*  Return the value of the nth element converted to big int.          */
/***********************************************************************/
longlong CHRBLK::GetBigintValue(int n)
  {
  bool      m;
  ulonglong val = CharToNumber((char*)GetValPtr(n), Long, INT_MAX64,
                                                    false, &m); 
    
  return (m && val < INT_MAX64) ? (longlong)(-(signed)val) : (longlong)val;
  } // end of GetBigintValue

/***********************************************************************/
/*  Return the value of the nth element converted to unsigned big int. */
/***********************************************************************/
ulonglong CHRBLK::GetUBigintValue(int n)
  {
  return CharToNumber((char*)GetValPtr(n), Long, ULONGLONG_MAX, true); 
  } // end of GetUBigintValue

/***********************************************************************/
/*  Return the value of the nth element converted to double.           */
/***********************************************************************/
double CHRBLK::GetFloatValue(int n)
  {
  return atof((char *)GetValPtrEx(n));
  } // end of GetFloatValue

/***********************************************************************/
/*  STRING GetCharString: get string representation of a char value.   */
/***********************************************************************/
char *CHRBLK::GetCharString(char *, int n)
  {
  return (char *)GetValPtrEx(n);
  } // end of GetCharString

/***********************************************************************/
/*  Set one value in a block.                                          */
/***********************************************************************/
void CHRBLK::SetValue(PVAL valp, int n)
  {
  bool b;

  ChkIndx(n);
  ChkTyp(valp);

  if (!(b = valp->IsNull()))
    SetValue((PSZ)valp->GetCharValue(), n);
  else
    Reset(n);

  SetNull(n, b && Nullable);
  } // end of SetValue

/***********************************************************************/
/*  Set one value in a block from a zero terminated string.            */
/***********************************************************************/
void CHRBLK::SetValue(PCSZ sp, int n)
  {
  uint len = (sp) ? strlen(sp) : 0;
  SetValue(sp, len, n);
  } // end of SetValue

/***********************************************************************/
/*  Set one value in a block from an array of characters.              */
/***********************************************************************/
void CHRBLK::SetValue(const char *sp, uint len, int n)
  {
  char  *p = Chrp + n * Long;

#if defined(_DEBUG)
  if (Check && (signed)len > Long) {
    PGLOBAL& g = Global;
    strcpy(g->Message, MSG(SET_STR_TRUNC));
		throw Type;
	} // endif Check
#endif   // _DEBUG

  if (sp)
    memcpy(p, sp, MY_MIN((unsigned)Long, len));

  if (Blanks) {
    // Suppress eventual ending zero and right fill with blanks
    for (int i = len; i < Long; i++)
      p[i] = ' ';

  } else if ((signed)len < Long)
    p[len] = 0;

  SetNull(n, false);
  } // end of SetValue

/***********************************************************************/
/*  Set one value in a block from a value in another block.            */
/***********************************************************************/
void CHRBLK::SetValue(PVBLK pv, int n1, int n2)
  {
  bool b;

  if (Type != pv->GetType() || Long != ((CHRBLK*)pv)->Long) {
    PGLOBAL& g = Global;
    strcpy(g->Message, MSG(BLKTYPLEN_MISM));
		throw Type;
	} // endif Type

  if (!(b = pv->IsNull(n2)))
    memcpy(Chrp + n1 * Long, ((CHRBLK*)pv)->Chrp + n2 * Long, Long);
  else
    Reset(n1);

  SetNull(n1, b && Nullable);
  } // end of SetValue

/***********************************************************************/
/*  Set one value in a block if val is less than the current value.    */
/***********************************************************************/
void CHRBLK::SetMin(PVAL valp, int n)
  {
  CheckParms(valp, n)
  CheckBlanks
  char *vp = valp->GetCharValue();
  char *bp = Chrp + n * Long;

  if (((Ci) ? strnicmp(vp, bp, Long) : strncmp(vp, bp, Long)) < 0)
    memcpy(bp, vp, Long);

  } // end of SetMin

/***********************************************************************/
/*  Set one value in a block if val is greater than the current value. */
/***********************************************************************/
void CHRBLK::SetMax(PVAL valp, int n)
  {
  CheckParms(valp, n)
  CheckBlanks
  char *vp = valp->GetCharValue();
  char *bp = Chrp + n * Long;

  if (((Ci) ? strnicmp(vp, bp, Long) : strncmp(vp, bp, Long)) > 0)
    memcpy(bp, vp, Long);

  } // end of SetMax

#if 0
/***********************************************************************/
/*  Set many values in a block from values in another block.           */
/***********************************************************************/
void CHRBLK::SetValues(PVBLK pv, int k, int n)
  {
#if defined(_DEBUG)
  if (Type != pv->GetType() || Long != ((CHRBLK*)pv)->Long) {
    PGLOBAL& g = Global;
    strcpy(g->Message, MSG(BLKTYPLEN_MISM));
		throw Type;
	} // endif Type
#endif   // _DEBUG
  char *p = ((CHRBLK*)pv)->Chrp;

  if (!k)
    memcpy(Chrp, p, Long * n);
  else
    memcpy(Chrp + k * Long, p + k * Long, Long * (n - k));

  } // end of SetValues
#endif // 0

/***********************************************************************/
/*  Move one value from i to j.                                        */
/***********************************************************************/
void CHRBLK::Move(int i, int j)
  {
  if (i != j) {
    memcpy(Chrp + j * Long, Chrp + i * Long, Long);
    MoveNull(i, j);
    } // endif i

  } // end of Move

/***********************************************************************/
/*  Compare a Value object with the nth value of the block.            */
/***********************************************************************/
int CHRBLK::CompVal(PVAL vp, int n)
  {
  ChkIndx(n);
  ChkTyp(vp);

  char *xvp = vp->GetCharValue(); // Get Value zero ended string
  bool ci = Ci || vp->IsCi();     // true if is case insensitive

  GetValPtrEx(n);                 // Get a zero ended string in Valp
  return (ci) ? stricmp(xvp, Valp) : strcmp(xvp, Valp);
  } // end of CompVal

/***********************************************************************/
/*  Compare two values of the block.                                   */
/***********************************************************************/
int CHRBLK::CompVal(int i1, int i2)
  {
  return (Ci) ? strnicmp(Chrp + i1 * Long, Chrp + i2 * Long, Long)
              : strncmp(Chrp + i1 * Long, Chrp + i2 * Long, Long);
  } // end of CompVal

/***********************************************************************/
/*  Get a pointer on the nth value of the block.                       */
/***********************************************************************/
void *CHRBLK::GetValPtr(int n)
  {
  ChkIndx(n);
  return Chrp + n * Long;
  } // end of GetValPtr

/***********************************************************************/
/*  Get a pointer on a zero ended string equal to nth value.           */
/***********************************************************************/
void *CHRBLK::GetValPtrEx(int n)
  {
  ChkIndx(n);
  memcpy(Valp, Chrp + n * Long, Long);

  if (IsNull(n))
    return const_cast<char *>("");

  if (Blanks) {
    // The (fast) way this is done works only for blocks such
    // as Min and Max where strings are stored with the ending 0
    // except for those whose length is equal to Len.
    // For VCT blocks we must remove rightmost blanks.
    char *p = Valp + Long;

    for (p--; p >= Valp && *p == ' '; p--) ;

    *(++p) = '\0';
    } // endif Blanks

  return Valp;
  } // end of GetValPtrEx

/***********************************************************************/
/*  Returns index of matching value in block or -1.                    */
/***********************************************************************/
int CHRBLK::Find(PVAL vp)
  {
  ChkTyp(vp);

  int  i;
  bool ci = Ci || vp->IsCi();
  PSZ  s = vp->GetCharValue();

  if (vp->IsNull())
    return -1;

  for (i = 0; i < Nval; i++) {
    if (IsNull(i))
      continue;

    GetValPtrEx(i);               // Get a zero ended string in Valp

    if (!((ci) ? strnicmp(s, Valp, Long) : strncmp(s, Valp, Long)))
      break;

    } // endfor i

  return (i < Nval) ? i : (-1);
  } // end of Find

/***********************************************************************/
/*  Returns the length of the longest string in the block.             */
/***********************************************************************/
int CHRBLK::GetMaxLength(void)
  {
  int i, n;

  for (i = n = 0; i < Nval; i++)
    if (!IsNull(i)) {
      GetValPtrEx(i);
      n = MY_MAX(n, (signed)strlen(Valp));
      } // endif null

  return n;
  } // end of GetMaxLength


/* -------------------------- Class STRBLK --------------------------- */

/***********************************************************************/
/*  Constructor.                                                       */
/***********************************************************************/
STRBLK::STRBLK(PGLOBAL g, void *mp, int nval, int type)
      : VALBLK(mp, type, nval), Strp((PSZ*&)Blkp)
  {
  Global = g;
  Nullable = true;
  Sorted = false;
  } // end of STRBLK constructor

/***********************************************************************/
/*  Initialization routine.                                            */
/***********************************************************************/
bool STRBLK::Init(PGLOBAL g, bool check)
  {
  if (!Blkp)
    if (AllocBuff(g, Nval * sizeof(PSZ)))
      return true;

  Check = check;
  Global = g;
  return false;
  } // end of Init

/***********************************************************************/
/*  Get the tiny value represented by the Strp string.                 */
/***********************************************************************/
char STRBLK::GetTinyValue(int n)
  {
  bool      m;
  ulonglong val = CharToNumber(Strp[n], strlen(Strp[n]), INT_MAX8, 
                                                         false, &m); 
    
  return (m && val < INT_MAX8) ? (char)(-(signed)val) : (char)val;
  } // end of GetTinyValue

/***********************************************************************/
/*  Get the unsigned tiny value represented by the Strp string.        */
/***********************************************************************/
uchar STRBLK::GetUTinyValue(int n)
  {
  return (uchar)CharToNumber(Strp[n], strlen(Strp[n]), UINT_MAX8, true); 
  } // end of GetUTinyValue

/***********************************************************************/
/*  Get the short value represented by the Strp string.                */
/***********************************************************************/
short STRBLK::GetShortValue(int n)
  {
  bool      m;
  ulonglong val = CharToNumber(Strp[n], strlen(Strp[n]), INT_MAX16,
                                                         false, &m); 
    
  return (m && val < INT_MAX16) ? (short)(-(signed)val) : (short)val;
  } // end of GetShortValue

/***********************************************************************/
/*  Get the unsigned short value represented by the Strp string.       */
/***********************************************************************/
ushort STRBLK::GetUShortValue(int n)
  {
  return (ushort)CharToNumber(Strp[n], strlen(Strp[n]), UINT_MAX16, true); 
  } // end of GetUshortValue

/***********************************************************************/
/*  Get the integer value represented by the Strp string.              */
/***********************************************************************/
int STRBLK::GetIntValue(int n)
  {
  bool      m;
  ulonglong val = CharToNumber(Strp[n], strlen(Strp[n]), INT_MAX32,
                                                         false, &m); 
    
  return (m && val < INT_MAX32) ? (int)(-(signed)val) : (int)val;
  } // end of GetIntValue

/***********************************************************************/
/*  Get the unsigned integer value represented by the Strp string.     */
/***********************************************************************/
uint STRBLK::GetUIntValue(int n)
  {
  return (uint)CharToNumber(Strp[n], strlen(Strp[n]), UINT_MAX32, true); 
  } // end of GetUintValue

/***********************************************************************/
/*  Get the big integer value represented by the Strp string.          */
/***********************************************************************/
longlong STRBLK::GetBigintValue(int n)
  {
  bool      m;
  ulonglong val = CharToNumber(Strp[n], strlen(Strp[n]), INT_MAX64,
                                                         false, &m); 
    
  return (m && val < INT_MAX64) ? (-(signed)val) : (longlong)val;
  } // end of GetBigintValue

/***********************************************************************/
/*  Get the unsigned big integer value represented by the Strp string. */
/***********************************************************************/
ulonglong STRBLK::GetUBigintValue(int n)
  {
  return CharToNumber(Strp[n], strlen(Strp[n]), ULONGLONG_MAX, true); 
  } // end of GetUBigintValue

/***********************************************************************/
/*  Set one value in a block from a value in another block.            */
/***********************************************************************/
void STRBLK::SetValue(PVBLK pv, int n1, int n2)
  {
  ChkTyp(pv);
  Strp[n1] = (!pv->IsNull(n2)) ? ((STRBLK*)pv)->Strp[n2] : NULL;
  } // end of SetValue

#if 0
/***********************************************************************/
/*  Set many values in a block from values in another block.           */
/***********************************************************************/
void STRBLK::SetValues(PVBLK pv, int k, int n)
  {
  CheckType(pv)
  PSZ *sp = ((STRBLK*)pv)->Strp;

  for (int i = k; i < n; i++)
    Strp[i] = (!pv->IsNull(i)) ? sp[i] : NULL;

  } // end of SetValues
#endif // 0

/***********************************************************************/
/*  Set one value in a block.                                          */
/***********************************************************************/
void STRBLK::SetValue(PVAL valp, int n)
  {
  ChkIndx(n);
  ChkTyp(valp);

  if (!valp->IsNull())
    SetValue((PSZ)valp->GetCharValue(), n);
  else
    Strp[n] = NULL;

  } // end of SetValue

/***********************************************************************/
/*  Set one value in a block from a zero terminated string.            */
/***********************************************************************/
void STRBLK::SetValue(PCSZ p, int n)
  {
  if (p) {
    if (!Sorted || !n || !Strp[n-1] || strcmp(p, Strp[n-1]))
      Strp[n] = (PSZ)PlugDup(Global, p);
    else
      Strp[n] = Strp[n-1];

  } else
    Strp[n] = NULL;

  } // end of SetValue

/***********************************************************************/
/*  Set one value in a block from an array of characters.              */
/***********************************************************************/
void STRBLK::SetValue(const char *sp, uint len, int n)
  {
  PSZ p;

  if (sp) {
    if (!Sorted || !n || !Strp[n-1] || strlen(Strp[n-1]) != len ||
                                       strncmp(sp, Strp[n-1], len)) {
      p = (PSZ)PlugSubAlloc(Global, NULL, len + 1);
      memcpy(p, sp, len);
      p[len] = 0;
    } else
      p = Strp[n-1];

  } else
    p = NULL;

  Strp[n] = p;
  } // end of SetValue

/***********************************************************************/
/*  Set one value in a block if val is less than the current value.    */
/***********************************************************************/
void STRBLK::SetMin(PVAL valp, int n)
  {
  CheckParms(valp, n)
  char *vp = valp->GetCharValue();
  char *bp = Strp[n];

  if (strcmp(vp, bp) < 0)
    SetValue(valp, n);

  } // end of SetMin

/***********************************************************************/
/*  Set one value in a block if val is greater than the current value. */
/***********************************************************************/
void STRBLK::SetMax(PVAL valp, int n)
  {
  CheckParms(valp, n)
  char *vp = valp->GetCharValue();
  char *bp = Strp[n];

  if (strcmp(vp, bp) > 0)
    SetValue(valp, n);

  } // end of SetMax

/***********************************************************************/
/*  Move one value from i to j.                                        */
/***********************************************************************/
void STRBLK::Move(int i, int j)
  {
  Strp[j] = Strp[i];
  } // end of Move

/***********************************************************************/
/*  Compare a Value object with the nth value of the block.            */
/***********************************************************************/
int STRBLK::CompVal(PVAL vp, int n)
  {
  ChkIndx(n);
  ChkTyp(vp);

  if (vp->IsNull() || !Strp[n])
    DBUG_ASSERT(false);

  return strcmp(vp->GetCharValue(), Strp[n]);
  } // end of CompVal

/***********************************************************************/
/*  Compare two values of the block.                                   */
/***********************************************************************/
int STRBLK::CompVal(int i1, int i2)
  {
  if (!Strp[i1] || !Strp[i2])
    DBUG_ASSERT(false);

  return (strcmp(Strp[i1], Strp[i2]));
  } // end of CompVal

/***********************************************************************/
/*  Get a pointer on the nth value of the block.                       */
/***********************************************************************/
void *STRBLK::GetValPtr(int n)
  {
  ChkIndx(n);
  return Strp + n;
  } // end of GetValPtr

/***********************************************************************/
/*  Get a pointer on a zero ended string equal to nth value.           */
/***********************************************************************/
void *STRBLK::GetValPtrEx(int n)
  {
  ChkIndx(n);
  return (Strp[n]) ? Strp[n] : const_cast<char*>("");
  } // end of GetValPtrEx

/***********************************************************************/
/*  Returns index of matching value in block or -1.                    */
/***********************************************************************/
int STRBLK::Find(PVAL vp)
  {
  int i;
  PSZ s;

  ChkTyp(vp);
  
  if (vp->IsNull())
    return -1;
  else
    s = vp->GetCharValue();

  for (i = 0; i < Nval; i++)
    if (Strp[i] && !strcmp(s, Strp[i]))
      break;

  return (i < Nval) ? i : (-1);
  } // end of Find

/***********************************************************************/
/*  Returns the length of the longest string in the block.             */
/***********************************************************************/
int STRBLK::GetMaxLength(void)
  {
  int i, n;

  for (i = n = 0; i < Nval; i++)
    if (Strp[i])
      n = MY_MAX(n, (signed)strlen(Strp[i]));

  return n;
  } // end of GetMaxLength

/* -------------------------- Class DATBLK --------------------------- */

/***********************************************************************/
/*  Constructor.                                                       */
/***********************************************************************/
DATBLK::DATBLK(void *mp, int nval) : TYPBLK<int>(mp, nval, TYPE_INT)
  {
  Type = TYPE_DATE;
  Dvalp = NULL;
  } // end of DATBLK constructor

/***********************************************************************/
/*  Set format so formatted dates can be converted on input.           */
/***********************************************************************/
bool DATBLK::SetFormat(PGLOBAL g, PCSZ fmt, int len, int year)
  {
  if (!(Dvalp = AllocateValue(g, TYPE_DATE, len, year, false, fmt)))
    return true;

  return false;
  } // end of SetFormat

/***********************************************************************/
/*  DTVAL GetCharString: get string representation of a date value.    */
/***********************************************************************/
char *DATBLK::GetCharString(char *p, int n)
  {
  char *vp;

  if (Dvalp) {
    Dvalp->SetValue(UnalignedRead(n));
    vp = Dvalp->GetCharString(p);
  } else
    vp = TYPBLK<int>::GetCharString(p, n);

  return vp;
  } // end of GetCharString

/***********************************************************************/
/*  Set one value in a block from a char string.                       */
/***********************************************************************/
void DATBLK::SetValue(PCSZ p, int n)
  {
  if (Dvalp) {
    // Decode the string according to format
    Dvalp->SetValue_psz(p);
    UnalignedWrite(n, Dvalp->GetIntValue());
  } else
    TYPBLK<int>::SetValue(p, n);

  } // end of SetValue


/* -------------------------- Class PTRBLK --------------------------- */

/***********************************************************************/
/*  Compare two values of the block.                                   */
/***********************************************************************/
int PTRBLK::CompVal(int i1, int i2)
  {
  return (Strp[i1] > Strp[i2]) ? 1 : (Strp[i1] < Strp[i2]) ? (-1) : 0;
  } // end of CompVal


/* -------------------------- Class MBVALS --------------------------- */

/***********************************************************************/
/*  Allocate a value block according to type,len, and nb of values.    */
/***********************************************************************/
PVBLK MBVALS::Allocate(PGLOBAL g, int type, int len, int prec,
																	int n, bool sub)
  {
  Mblk.Sub = sub;
  Mblk.Size = n * GetTypeSize(type, len);

  if (!PlgDBalloc(g, NULL, Mblk)) {
    sprintf(g->Message, MSG(ALLOC_ERROR), "MBVALS::Allocate");
    return NULL;
  } else
    Vblk = AllocValBlock(g, Mblk.Memp, type, n, len, prec, 
                            TRUE, TRUE, FALSE);

  return Vblk;
  } // end of Allocate

/***********************************************************************/
/*  Reallocate the value block according to the new size.              */
/***********************************************************************/
bool MBVALS::ReAllocate(PGLOBAL g, int n)
  {
  if (!PlgDBrealloc(g, NULL, Mblk, n * Vblk->GetVlen())) {
    sprintf(g->Message, MSG(ALLOC_ERROR), "MBVALS::ReAllocate");
    return TRUE;
  } else
    Vblk->ReAlloc(Mblk.Memp, n);

  return FALSE;
  } // end of ReAllocate

/***********************************************************************/
/*  Free the value block.                                              */
/***********************************************************************/
void MBVALS::Free(void)
  {
  PlgDBfree(Mblk);
  Vblk = NULL;
  } // end of Free

/* ------------------------- End of Valblk --------------------------- */

