/************ Valblk C++ Functions Source Code File (.CPP) *************/
/*  Name: VALBLK.CPP  Version 1.6                                      */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2005-2013    */
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
/*  DATE, longlong, and double. Shortly we should add more types.      */
/***********************************************************************/

/***********************************************************************/
/*  Include relevant MariaDB header file.                              */
/***********************************************************************/
#include "my_global.h"
#if defined(WIN32)
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

#define CheckBlanks     assert(!Blanks);

/***********************************************************************/
/*  AllocValBlock: allocate a VALBLK according to type.                */
/***********************************************************************/
PVBLK AllocValBlock(PGLOBAL g, void *mp, int type, int nval, int len,
                                         int prec, bool check, bool blank)
  {
  PVBLK blkp;

#ifdef DEBTRACE
 htrc("AVB: mp=%p type=%d nval=%d len=%d check=%u blank=%u\n",
  mp, type, nval, len, check, blank);
#endif

  switch (type) {
    case TYPE_STRING:
      if (len)
        blkp = new(g) CHRBLK(mp, nval, len, prec, blank);
      else
        blkp = new(g) STRBLK(g, mp, nval);

      break;
    case TYPE_SHORT:
      blkp = new(g) TYPBLK<short>(mp, nval, type);
      break;
    case TYPE_INT:
      blkp = new(g) TYPBLK<int>(mp, nval, type);
      break;
    case TYPE_DATE:        // ?????
      blkp = new(g) DATBLK(mp, nval);
      break;
    case TYPE_BIGINT:
      blkp = new(g) TYPBLK<longlong>(mp, nval, type);
      break;
    case TYPE_FLOAT:
      blkp = new(g) TYPBLK<double>(mp, nval, prec, type);
      break;
    default:
      sprintf(g->Message, MSG(BAD_VALBLK_TYPE), type);
      return NULL;
    } // endswitch Type

  blkp->Init(g, check);
  return blkp;
  } // end of AllocValBlock

/* -------------------------- Class VALBLK --------------------------- */

/***********************************************************************/
/*  Constructor.                                                       */
/***********************************************************************/
VALBLK::VALBLK(void *mp, int type, int nval)
  {
  Blkp = mp;
  To_Nulls = NULL;
  Check = true;
  Nullable = false;
  Type = type;
  Nval = nval;
  Prec = 0;
  } // end of VALBLK constructor

/***********************************************************************/
/*  Raise error for numeric types.                                     */
/***********************************************************************/
PSZ VALBLK::GetCharValue(int n)
  {
  PGLOBAL& g = Global;

  assert(g);
  sprintf(g->Message, MSG(NO_CHAR_FROM), Type);
  longjmp(g->jumper[g->jump_level], Type);
  return NULL;
  } // end of GetCharValue

/***********************************************************************/
/*  Set format so formatted dates can be converted on input.           */
/***********************************************************************/
bool VALBLK::SetFormat(PGLOBAL g, PSZ fmt, int len, int year)
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
/*  Check functions.                                                   */
/***********************************************************************/
void VALBLK::ChkIndx(int n)
  {
  if (n < 0 || n >= Nval) {
    PGLOBAL& g = Global;
    strcpy(g->Message, MSG(BAD_VALBLK_INDX));
    longjmp(g->jumper[g->jump_level], Type);
    } // endif n

  } // end of ChkIndx

void VALBLK::ChkTyp(PVAL v)
  {
  if (Check && Type != v->GetType()) {
    PGLOBAL& g = Global;
    strcpy(g->Message, MSG(VALTYPE_NOMATCH));
    longjmp(g->jumper[g->jump_level], Type);
    } // endif Type

  } // end of ChkTyp

void VALBLK::ChkTyp(PVBLK vb)
  {
  if (Check && Type != vb->GetType()) {
    PGLOBAL& g = Global;
    strcpy(g->Message, MSG(VALTYPE_NOMATCH));
    longjmp(g->jumper[g->jump_level], Type);
    } // endif Type

  } // end of ChkTyp

/* -------------------------- Class TYPBLK --------------------------- */

/***********************************************************************/
/*  Constructors.                                                      */
/***********************************************************************/
template <class TYPE>
TYPBLK<TYPE>::TYPBLK(void *mp, int nval, int type)
            : VALBLK(mp, type, nval), Typp((TYPE*&)Blkp)
  {
  Fmt = GetFmt(Type);
  } // end of TYPBLK constructor

template <class TYPE>
TYPBLK<TYPE>::TYPBLK(void *mp, int nval, int prec, int type)
            : VALBLK(mp, type, nval), Typp((TYPE*&)Blkp)
  {
  DBUG_ASSERT(Type == TYPE_FLOAT);
  Prec = prec;
  Fmt = GetFmt(Type);
  } // end of DBLBLK constructor

/***********************************************************************/
/*  Initialization routine.                                            */
/***********************************************************************/
template <class TYPE>
void TYPBLK<TYPE>::Init(PGLOBAL g, bool check)
  {
  if (!Blkp)
    Blkp = PlugSubAlloc(g, NULL, Nval * sizeof(TYPE));

  Check = check;
  Global = g;
  } // end of Init

/***********************************************************************/
/*  Set one value in a block.                                          */
/***********************************************************************/
template <class TYPE>
void TYPBLK<TYPE>::SetValue(PVAL valp, int n)
  {
  bool b;

  ChkIndx(n);
  ChkTyp(valp);

  if (!(b = valp->IsNull() && Nullable))
    Typp[n] = GetTypedValue(valp);
  else
    Reset(n);

  SetNull(n, b);
  } // end of SetValue

template <>
int TYPBLK<int>::GetTypedValue(PVAL valp)
  {return valp->GetIntValue();}

template <>
short TYPBLK<short>::GetTypedValue(PVAL valp)
  {return valp->GetShortValue();}

template <>
longlong TYPBLK<longlong>::GetTypedValue(PVAL valp)
  {return valp->GetBigintValue();}

template <>
double TYPBLK<double>::GetTypedValue(PVAL valp)
  {return valp->GetFloatValue();}

/***********************************************************************/
/*  Set one value in a block.                                          */
/***********************************************************************/
template <class TYPE>
void TYPBLK<TYPE>::SetValue(PSZ p, int n)
  {
  ChkIndx(n);

  if (Check) {
    PGLOBAL& g = Global;
    strcpy(g->Message, MSG(BAD_SET_STRING));
    longjmp(g->jumper[g->jump_level], Type);
    } // endif Check

  Typp[n] = GetTypedValue(p);
  SetNull(n, false);
  } // end of SetValue

template <>
int TYPBLK<int>::GetTypedValue(PSZ p) {return atol(p);}
template <>
short TYPBLK<short>::GetTypedValue(PSZ p) {return (short)atoi(p);}
template <>
longlong TYPBLK<longlong>::GetTypedValue(PSZ p) {return atoll(p);}
template <>
double TYPBLK<double>::GetTypedValue(PSZ p) {return atof(p);}

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
    Typp[n1] = GetTypedValue(pv, n2);
  else
    Reset(n1);

  SetNull(n1, b);
  } // end of SetValue

template <>
int TYPBLK<int>::GetTypedValue(PVBLK blk, int n)
  {return blk->GetIntValue(n);}

template <>
short TYPBLK<short>::GetTypedValue(PVBLK blk, int n)
  {return blk->GetShortValue(n);}

template <>
longlong TYPBLK<longlong>::GetTypedValue(PVBLK blk, int n)
  {return blk->GetBigintValue(n);}

template <>
double TYPBLK<double>::GetTypedValue(PVBLK blk, int n)
  {return blk->GetFloatValue(n);}

#if 0
/***********************************************************************/
/*  Set many values in a block from values in another block.           */
/***********************************************************************/
template <class TYPE>
void TYPBLK<TYPE>::SetValues(PVBLK pv, int k, int n)
  {
  CheckType(pv)
  TYPE *lp = ((TYPBLK*)pv)->Typp;

  for (register int i = k; i < n; i++)          // TODO
    Typp[i] = lp[i];

  } // end of SetValues
#endif // 0

/***********************************************************************/
/*  Move one value from i to j.                                        */
/***********************************************************************/
template <class TYPE>
void TYPBLK<TYPE>::Move(int i, int j)
  {
  Typp[j] = Typp[i];
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
  TYPE mlv = Typp[n];
  TYPE vlv = GetTypedValue(vp);

  return (vlv > mlv) ? 1 : (vlv < mlv) ? (-1) : 0;
  } // end of CompVal

/***********************************************************************/
/*  Compare two values of the block.                                   */
/***********************************************************************/
template <class TYPE>
int TYPBLK<TYPE>::CompVal(int i1, int i2)
  {
  TYPE lv1 = Typp[i1];
  TYPE lv2 = Typp[i2];

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
    if (n == Typp[i])
      break;

  return (i < Nval) ? i : (-1);
  } // end of Find

/***********************************************************************/
/*  Returns the length of the longest string in the block.             */
/***********************************************************************/
template <class TYPE>
int TYPBLK<TYPE>::GetMaxLength(void)
  {
  char buf[12];
  int i, n;

  for (i = n = 0; i < Nval; i++) {
    sprintf(buf, Fmt, Typp[i]);

    n = max(n, (signed)strlen(buf));
    } // endfor i

  return n;
  } // end of GetMaxLength


/* -------------------------- Class CHRBLK --------------------------- */

/***********************************************************************/
/*  Constructor.                                                       */
/***********************************************************************/
CHRBLK::CHRBLK(void *mp, int nval, int len, int prec, bool blank)
      : VALBLK(mp, TYPE_STRING, nval), Chrp((char*&)Blkp)
  {
  Valp = NULL;
  Blanks = blank;
  Ci = (prec != 0);
  Long = len;
  } // end of CHRBLK constructor

/***********************************************************************/
/*  Initialization routine.                                            */
/***********************************************************************/
void CHRBLK::Init(PGLOBAL g, bool check)
  {
  Valp = (char*)PlugSubAlloc(g, NULL, Long + 1);
  Valp[Long] = '\0';

  if (!Blkp)
    Blkp = PlugSubAlloc(g, NULL, Nval * Long);

  Check = check;
  Global = g;
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
/*  Return the value of the nth element converted to short.            */
/***********************************************************************/
short CHRBLK::GetShortValue(int n)
  {
  return (short)atoi((char *)GetValPtrEx(n));
  } // end of GetIntValue

/***********************************************************************/
/*  Return the value of the nth element converted to int.              */
/***********************************************************************/
int CHRBLK::GetIntValue(int n)
  {
  return atol((char *)GetValPtrEx(n));
  } // end of GetIntValue

/***********************************************************************/
/*  Return the value of the nth element converted to big int.          */
/***********************************************************************/
longlong CHRBLK::GetBigintValue(int n)
  {
  return atoll((char *)GetValPtrEx(n));
  } // end of GetIntValue

/***********************************************************************/
/*  Return the value of the nth element converted to double.           */
/***********************************************************************/
double CHRBLK::GetFloatValue(int n)
  {
  return atof((char *)GetValPtrEx(n));
  } // end of GetFloatValue

/***********************************************************************/
/*  Set one value in a block.                                          */
/***********************************************************************/
void CHRBLK::SetValue(PVAL valp, int n)
  {
  bool b;

  ChkIndx(n);
  ChkTyp(valp);

  if (!(b = valp->IsNull() && Nullable))
    SetValue((PSZ)valp->GetCharValue(), n);
  else
    Reset(n);

  SetNull(n, b);
  } // end of SetValue

/***********************************************************************/
/*  Set one value in a block.                                          */
/***********************************************************************/
void CHRBLK::SetValue(PSZ sp, int n)
  {
  size_t len = (sp) ? strlen(sp) : 0;
  char  *p = Chrp + n * Long;

#if defined(_DEBUG) || defined(DEBTRACE)
  if (Check && (signed)len > Long) {
    PGLOBAL& g = Global;
    strcpy(g->Message, MSG(SET_STR_TRUNC));
    longjmp(g->jumper[g->jump_level], Type);
    } // endif Check
#endif

  if (sp)
    strncpy(p, sp, Long);
  else
    *p = '\0';

  if (Blanks)
    // Suppress eventual ending zero and right fill with blanks
    for (register int i = len; i < Long; i++)
      p[i] = ' ';

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
    longjmp(g->jumper[g->jump_level], Type);
    } // endif Type

  if (!(b = pv->IsNull(n2) && Nullable))
    memcpy(Chrp + n1 * Long, ((CHRBLK*)pv)->Chrp + n2 * Long, Long);
  else
    Reset(n1);

  SetNull(n1, b);
  } // end of SetValue

#if 0
/***********************************************************************/
/*  Set many values in a block from values in another block.           */
/***********************************************************************/
void CHRBLK::SetValues(PVBLK pv, int k, int n)
  {
#if defined(_DEBUG) || defined(DEBTRACE)
  if (Type != pv->GetType() || Long != ((CHRBLK*)pv)->Long) {
    PGLOBAL& g = Global;
    strcpy(g->Message, MSG(BLKTYPLEN_MISM));
    longjmp(g->jumper[g->jump_level], Type);
    } // endif Type
#endif
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
  memcpy(Chrp + j * Long, Chrp + i * Long, Long);
  MoveNull(i, j);
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
    return "";

  if (Blanks) {
    // The (fast) way this is done works only for blocks such
    // as Min and Max where strings are stored with the ending 0
    // except for those whose length is equal to Len.
    // For VCT blocks we must remove rightmost blanks.
    char *p = Valp + Long;

    for (p--; *p == ' ' && p >= Valp; p--) ;

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
      n = max(n, (signed)strlen(Valp));
      } // endif null

  return n;
  } // end of GetMaxLength


/* -------------------------- Class STRBLK --------------------------- */

/***********************************************************************/
/*  Constructor.                                                       */
/***********************************************************************/
STRBLK::STRBLK(PGLOBAL g, void *mp, int nval)
      : VALBLK(mp, TYPE_STRING, nval), Strp((PSZ*&)Blkp)
  {
  Global = g;
  Nullable = true;
  } // end of STRBLK constructor

/***********************************************************************/
/*  Initialization routine.                                            */
/***********************************************************************/
void STRBLK::Init(PGLOBAL g, bool check)
  {
  if (!Blkp)
    Blkp = PlugSubAlloc(g, NULL, Nval * sizeof(PSZ));

  Check = check;
  Global = g;
  } // end of Init

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

  for (register int i = k; i < n; i++)
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
/*  Set one value in a block.                                          */
/***********************************************************************/
void STRBLK::SetValue(PSZ p, int n)
  {
  Strp[n] = (PSZ)PlugSubAlloc(Global, NULL, strlen(p) + 1);
  strcpy(Strp[n], p);
  } // end of SetValue

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
  return (Strp[n]) ? Strp[n] : "";
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
      n = max(n, (signed)strlen(Strp[i]));

  return n;
  } // end of GetMaxLength

#if 0
/* -------------------------- Class SHRBLK --------------------------- */

/***********************************************************************/
/*  Constructor.                                                       */
/***********************************************************************/
SHRBLK::SHRBLK(void *mp, int nval)
      : VALBLK(mp, TYPE_SHORT, nval), Shrp((short*&)Blkp)
  {
  } // end of SHRBLK constructor

/***********************************************************************/
/*  Initialization routine.                                            */
/***********************************************************************/
void SHRBLK::Init(PGLOBAL g, bool check)
  {
  if (!Blkp)
    Blkp = PlugSubAlloc(g, NULL, Nval * sizeof(short));

  Check = check;
  Global = g;
  } // end of Init

/***********************************************************************/
/*  Set one value in a block.                                          */
/***********************************************************************/
void SHRBLK::SetValue(PVAL valp, int n)
  {
  CheckParms(valp, n)
  bool b;

  if (!(b = valp->IsNull() && Nullable))
    Shrp[n] = valp->GetShortValue();
  else
    Reset(n);

  SetNull(n, b);
  } // end of SetValue

/***********************************************************************/
/*  Set one value in a block.                                          */
/***********************************************************************/
void SHRBLK::SetValue(PSZ p, int n)
  {
#if defined(_DEBUG) || defined(DEBTRACE)
  if (Check) {
    PGLOBAL& g = Global;
    strcpy(g->Message, MSG(BAD_SET_STRING));
    longjmp(g->jumper[g->jump_level], Type);
    } // endif Check
#endif
  Shrp[n] = (short)atoi(p);
  SetNull(n, false);
  } // end of SetValue

/***********************************************************************/
/*  Set one value in a block from a value in another block.            */
/***********************************************************************/
void SHRBLK::SetValue(PVBLK pv, int n1, int n2)
  {
  CheckType(pv)
  bool b;

  if (!(b = pv->IsNull(n2) && Nullable))
    Shrp[n1] = ((SHRBLK*)pv)->Shrp[n2];
  else
    Reset(n1);

  SetNull(n1, b);
  } // end of SetValue

#if 0
/***********************************************************************/
/*  Set many values in a block from values in another block.           */
/***********************************************************************/
void SHRBLK::SetValues(PVBLK pv, int k, int n)
  {
  CheckType(pv)
  short *sp = ((SHRBLK*)pv)->Shrp;

  for (register int i = k; i < n; i++)
    Shrp[i] = sp[i];

  } // end of SetValues
#endif // 0

/***********************************************************************/
/*  Move one value from i to j.                                        */
/***********************************************************************/
void SHRBLK::Move(int i, int j)
  {
  Shrp[j] = Shrp[i];
  MoveNull(i, j);
  } // end of Move

/***********************************************************************/
/*  Compare a Value object with the nth value of the block.            */
/***********************************************************************/
int SHRBLK::CompVal(PVAL vp, int n)
  {
  CheckParms(vp, n)
  short msv = Shrp[n];
  short vsv = vp->GetShortValue();

  return (vsv > msv) ? 1 : (vsv < msv) ? (-1) : 0;
  } // end of CompVal

/***********************************************************************/
/*  Compare two values of the block.                                   */
/***********************************************************************/
int SHRBLK::CompVal(int i1, int i2)
  {
  short sv1 = Shrp[i1];
  short sv2 = Shrp[i2];

  return (sv1 > sv2) ? 1 : (sv1 < sv2) ? (-1) : 0;
  } // end of CompVal

/***********************************************************************/
/*  Get a pointer on the nth value of the block.                       */
/***********************************************************************/
void *SHRBLK::GetValPtr(int n)
  {
  ChkIndx(n);
  return Shrp + n;
  } // end of GetValPtr

/***********************************************************************/
/*  Get a pointer on the nth value of the block.                       */
/***********************************************************************/
void *SHRBLK::GetValPtrEx(int n)
  {
  ChkIndx(n);
  return Shrp + n;
  } // end of GetValPtrEx

/***********************************************************************/
/*  Returns index of matching value in block or -1.                    */
/***********************************************************************/
int SHRBLK::Find(PVAL vp)
  {
  CheckType(vp)
  int   i;
  short n = vp->GetShortValue();

  for (i = 0; i < Nval; i++)
    if (n == Shrp[i])
      break;

  return (i < Nval) ? i : (-1);
  } // end of Find

/***********************************************************************/
/*  Returns the length of the longest string in the block.             */
/***********************************************************************/
int SHRBLK::GetMaxLength(void)
  {
  char buf[12];
  int i, n;

  for (i = n = 0; i < Nval; i++) {
    sprintf(buf, "%hd", Shrp[i]);

    n = max(n, (signed)strlen(buf));
    } // endfor i

  return n;
  } // end of GetMaxLength


/* -------------------------- Class LNGBLK --------------------------- */

/***********************************************************************/
/*  Constructor.                                                       */
/***********************************************************************/
LNGBLK::LNGBLK(void *mp, int nval)
      : VALBLK(mp, TYPE_INT, nval), Lngp((int*&)Blkp)
  {
  } // end of LNGBLK constructor

/***********************************************************************/
/*  Initialization routine.                                            */
/***********************************************************************/
void LNGBLK::Init(PGLOBAL g, bool check)
  {
  if (!Blkp)
    Blkp = PlugSubAlloc(g, NULL, Nval * sizeof(int));

  Check = check;
  Global = g;
  } // end of Init

/***********************************************************************/
/*  Set one value in a block.                                          */
/***********************************************************************/
void LNGBLK::SetValue(PVAL valp, int n)
  {
  CheckParms(valp, n)
  bool b;

  if (!(b = valp->IsNull() && Nullable))
    Lngp[n] = valp->GetIntValue();
  else
    Reset(n);

  SetNull(n, b);
  } // end of SetValue

/***********************************************************************/
/*  Set one value in a block.                                          */
/***********************************************************************/
void LNGBLK::SetValue(PSZ p, int n)
  {
#if defined(_DEBUG) || defined(DEBTRACE)
  if (Check) {
    PGLOBAL& g = Global;
    strcpy(g->Message, MSG(BAD_SET_STRING));
    longjmp(g->jumper[g->jump_level], Type);
    } // endif Check
#endif

  Lngp[n] = atol(p);
  } // end of SetValue

/***********************************************************************/
/*  Set one value in a block from a value in another block.            */
/***********************************************************************/
void LNGBLK::SetValue(PVBLK pv, int n1, int n2)
  {
  CheckType(pv)
  bool b;

  if (!(b = pv->IsNull(n2) && Nullable))
    Lngp[n1] = ((LNGBLK*)pv)->Lngp[n2];
  else
    Reset(n1);

  SetNull(n1, b);
  } // end of SetValue

#if 0
/***********************************************************************/
/*  Set many values in a block from values in another block.           */
/***********************************************************************/
void LNGBLK::SetValues(PVBLK pv, int k, int n)
  {
  CheckType(pv)
  int *lp = ((LNGBLK*)pv)->Lngp;

  for (register int i = k; i < n; i++)
    Lngp[i] = lp[i];

  } // end of SetValues
#endif // 0

/***********************************************************************/
/*  Move one value from i to j.                                        */
/***********************************************************************/
void LNGBLK::Move(int i, int j)
  {
  Lngp[j] = Lngp[i];
  MoveNull(i, j);
  } // end of Move

/***********************************************************************/
/*  Compare a Value object with the nth value of the block.            */
/***********************************************************************/
int LNGBLK::CompVal(PVAL vp, int n)
  {
  CheckParms(vp, n)
  register int mlv = Lngp[n];
  register int vlv = vp->GetIntValue();

  return (vlv > mlv) ? 1 : (vlv < mlv) ? (-1) : 0;
  } // end of CompVal

/***********************************************************************/
/*  Compare two values of the block.                                   */
/***********************************************************************/
int LNGBLK::CompVal(int i1, int i2)
  {
  register int lv1 = Lngp[i1];
  register int lv2 = Lngp[i2];

  return (lv1 > lv2) ? 1 : (lv1 < lv2) ? (-1) : 0;
  } // end of CompVal

/***********************************************************************/
/*  Get a pointer on the nth value of the block.                       */
/***********************************************************************/
void *LNGBLK::GetValPtr(int n)
  {
  ChkIndx(n);
  return Lngp + n;
  } // end of GetValPtr

/***********************************************************************/
/*  Get a pointer on the nth value of the block.                       */
/***********************************************************************/
void *LNGBLK::GetValPtrEx(int n)
  {
  ChkIndx(n);
  return Lngp + n;
  } // end of GetValPtrEx

/***********************************************************************/
/*  Returns index of matching value in block or -1.                    */
/***********************************************************************/
int LNGBLK::Find(PVAL vp)
  {
  CheckType(vp)
  int  i;
  int n = vp->GetIntValue();

  for (i = 0; i < Nval; i++)
    if (n == Lngp[i])
      break;

  return (i < Nval) ? i : (-1);
  } // end of Find

/***********************************************************************/
/*  Returns the length of the longest string in the block.             */
/***********************************************************************/
int LNGBLK::GetMaxLength(void)
  {
  char buf[12];
  int i, n;

  for (i = n = 0; i < Nval; i++) {
    sprintf(buf, "%d", Lngp[i]);

    n = max(n, (signed)strlen(buf));
    } // endfor i

  return n;
  } // end of GetMaxLength
#endif // 0

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
bool DATBLK::SetFormat(PGLOBAL g, PSZ fmt, int len, int year)
  {
  if (!(Dvalp = AllocateValue(g, TYPE_DATE, len, year, fmt)))
    return true;

  return false;
  } // end of SetFormat

/***********************************************************************/
/*  Set one value in a block from a char string.                       */
/***********************************************************************/
void DATBLK::SetValue(PSZ p, int n)
  {
  if (Dvalp) {
    // Decode the string according to format
    Dvalp->SetValue_psz(p);
    Typp[n] = Dvalp->GetIntValue();
  } else
    TYPBLK<int>::SetValue(p, n);

  } // end of SetValue

#if 0
/* -------------------------- Class BIGBLK --------------------------- */

/***********************************************************************/
/*  Constructor.                                                       */
/***********************************************************************/
BIGBLK::BIGBLK(void *mp, int nval)
      : VALBLK(mp, TYPE_BIGINT, nval), Lngp((longlong*&)Blkp)
  {
  } // end of BIGBLK constructor

/***********************************************************************/
/*  Initialization routine.                                            */
/***********************************************************************/
void BIGBLK::Init(PGLOBAL g, bool check)
  {
  if (!Blkp)
    Blkp = PlugSubAlloc(g, NULL, Nval * sizeof(longlong));

  Check = check;
  Global = g;
  } // end of Init

/***********************************************************************/
/*  Set one value in a block.                                          */
/***********************************************************************/
void BIGBLK::SetValue(PVAL valp, int n)
  {
  CheckParms(valp, n)
  bool b;

  if (!(b = valp->IsNull() && Nullable))
    Lngp[n] = valp->GetBigintValue();
  else
    Reset(n);

  SetNull(n, b);
  } // end of SetValue

/***********************************************************************/
/*  Set one value in a block.                                          */
/***********************************************************************/
void BIGBLK::SetValue(PSZ p, int n)
  {
#if defined(_DEBUG) || defined(DEBTRACE)
  if (Check) {
    PGLOBAL& g = Global;
    strcpy(g->Message, MSG(BAD_SET_STRING));
    longjmp(g->jumper[g->jump_level], Type);
    } // endif Check
#endif

  Lngp[n] = atoll(p);
  } // end of SetValue

/***********************************************************************/
/*  Set one value in a block from a value in another block.            */
/***********************************************************************/
void BIGBLK::SetValue(PVBLK pv, int n1, int n2)
  {
  CheckType(pv)
  bool b;

  if (!(b = pv->IsNull(n2) && Nullable))
    Lngp[n1] = ((BIGBLK*)pv)->Lngp[n2];
  else
    Reset(n1);

  SetNull(n1, b);
  } // end of SetValue

#if 0
/***********************************************************************/
/*  Set many values in a block from values in another block.           */
/***********************************************************************/
void BIGBLK::SetValues(PVBLK pv, int k, int n)
  {
  CheckType(pv)
  longlong *lp = ((BIGBLK*)pv)->Lngp;

  for (register int i = k; i < n; i++)
    Lngp[i] = lp[i];

  } // end of SetValues
#endif // 0

/***********************************************************************/
/*  Move one value from i to j.                                        */
/***********************************************************************/
void BIGBLK::Move(int i, int j)
  {
  Lngp[j] = Lngp[i];
  MoveNull(i, j);
  } // end of Move

/***********************************************************************/
/*  Compare a Value object with the nth value of the block.            */
/***********************************************************************/
int BIGBLK::CompVal(PVAL vp, int n)
  {
  CheckParms(vp, n)
  longlong mlv = Lngp[n];
  longlong vlv = vp->GetBigintValue();

  return (vlv > mlv) ? 1 : (vlv < mlv) ? (-1) : 0;
  } // end of CompVal

/***********************************************************************/
/*  Compare two values of the block.                                   */
/***********************************************************************/
int BIGBLK::CompVal(int i1, int i2)
  {
  longlong lv1 = Lngp[i1];
  longlong lv2 = Lngp[i2];

  return (lv1 > lv2) ? 1 : (lv1 < lv2) ? (-1) : 0;
  } // end of CompVal

/***********************************************************************/
/*  Get a pointer on the nth value of the block.                       */
/***********************************************************************/
void *BIGBLK::GetValPtr(int n)
  {
  ChkIndx(n);
  return Lngp + n;
  } // end of GetValPtr

/***********************************************************************/
/*  Get a pointer on the nth value of the block.                       */
/***********************************************************************/
void *BIGBLK::GetValPtrEx(int n)
  {
  ChkIndx(n);
  return Lngp + n;
  } // end of GetValPtrEx

/***********************************************************************/
/*  Returns index of matching value in block or -1.                    */
/***********************************************************************/
int BIGBLK::Find(PVAL vp)
  {
  CheckType(vp)
  int      i;
  longlong n = vp->GetBigintValue();

  for (i = 0; i < Nval; i++)
    if (n == Lngp[i])
      break;

  return (i < Nval) ? i : (-1);
  } // end of Find

/***********************************************************************/
/*  Returns the length of the longest string in the block.             */
/***********************************************************************/
int BIGBLK::GetMaxLength(void)
  {
  char buf[24];
  int i, n;

  for (i = n = 0; i < Nval; i++) {
    sprintf(buf, "%lld", Lngp[i]);

    n = max(n, (signed)strlen(buf));
    } // endfor i

  return n;
  } // end of GetMaxLength


/* -------------------------- Class DBLBLK --------------------------- */

/***********************************************************************/
/*  Constructor.                                                       */
/***********************************************************************/
DBLBLK::DBLBLK(void *mp, int nval, int prec)
      : VALBLK(mp, TYPE_FLOAT, nval), Dblp((double*&)Blkp)
  {
  Prec = prec;
  } // end of DBLBLK constructor

/***********************************************************************/
/*  Initialization routine.                                            */
/***********************************************************************/
void DBLBLK::Init(PGLOBAL g, bool check)
  {
  if (!Blkp)
    Blkp = PlugSubAlloc(g, NULL, Nval * sizeof(double));

  Check = check;
  Global = g;
  } // end of Init

/***********************************************************************/
/*  Set one value in a block from a value in another block.            */
/***********************************************************************/
void DBLBLK::SetValue(PVBLK pv, int n1, int n2)
  {
  CheckType(pv)
  bool b;

  if (!(b = pv->IsNull(n2) && Nullable))
    Dblp[n1] = ((DBLBLK*)pv)->Dblp[n2];
  else
    Reset(n1);

  SetNull(n1, b);
  } // end of SetValue

/***********************************************************************/
/*  Set one value in a block.                                          */
/***********************************************************************/
void DBLBLK::SetValue(PVAL valp, int n)
  {
  CheckParms(valp, n)
  bool b;

  if (!(b = valp->IsNull() && Nullable))
    Dblp[n] = valp->GetFloatValue();
  else
    Reset(n);

  SetNull(n, b);
  } // end of SetValue

/***********************************************************************/
/*  Set one value in a block.                                          */
/***********************************************************************/
void DBLBLK::SetValue(PSZ p, int n)
  {
#if defined(_DEBUG) || defined(DEBTRACE)
  if (Check) {
    PGLOBAL& g = Global;
    strcpy(g->Message, MSG(BAD_SET_STRING));
    longjmp(g->jumper[g->jump_level], Type);
    } // endif Check
#endif

  Dblp[n] = atof(p);
  } // end of SetValue

#if 0
/***********************************************************************/
/*  Set many values in a block from values in another block.           */
/***********************************************************************/
void DBLBLK::SetValues(PVBLK pv, int k, int n)
  {
  CheckType(pv)
  double *dp = ((DBLBLK*)pv)->Dblp;

  for (register int i = k; i < n; i++)
    Dblp[i] = dp[i];

  } // end of SetValues
#endif // 0

/***********************************************************************/
/*  Move one value from i to j.                                        */
/***********************************************************************/
void DBLBLK::Move(int i, int j)
  {
  Dblp[j] = Dblp[i];
  MoveNull(i, j);
  } // end of Move

/***********************************************************************/
/*  Compare a Value object with the nth value of the block.            */
/***********************************************************************/
int DBLBLK::CompVal(PVAL vp, int n)
  {
  CheckParms(vp, n)
  double mfv = Dblp[n];
  double vfv = vp->GetFloatValue();

  return (vfv > mfv) ? 1 : (vfv < mfv) ? (-1) : 0;
  } // end of CompVal

/***********************************************************************/
/*  Compare two values of the block.                                   */
/***********************************************************************/
int DBLBLK::CompVal(int i1, int i2)
  {
  register double dv1 = Dblp[i1];
  register double dv2 = Dblp[i2];

  return (dv1 > dv2) ? 1 : (dv1 < dv2) ? (-1) : 0;
  } // end of CompVal

/***********************************************************************/
/*  Get a pointer on the nth value of the block.                       */
/***********************************************************************/
void *DBLBLK::GetValPtr(int n)
  {
  ChkIndx(n);
  return Dblp + n;
  } // end of GetValPtr

/***********************************************************************/
/*  Get a pointer on the nth value of the block.                       */
/***********************************************************************/
void *DBLBLK::GetValPtrEx(int n)
  {
  ChkIndx(n);
  return Dblp + n;
  } // end of GetValPtrEx

/***********************************************************************/
/*  Returns index of matching value in block or -1.                    */
/***********************************************************************/
int DBLBLK::Find(PVAL vp)
  {
  CheckType(vp)
  int    i;
  double d = vp->GetFloatValue();

  for (i = 0; i < Nval; i++)
    if (d == Dblp[i])
      break;

  return (i < Nval) ? i : (-1);
  } // end of Find

/***********************************************************************/
/*  Returns the length of the longest string in the block.             */
/***********************************************************************/
int DBLBLK::GetMaxLength(void)
  {
  char buf[32];
  int i, n;

  for (i = n = 0; i < Nval; i++) {
    sprintf(buf, "%lf", Dblp[i]);

    n = max(n, (signed)strlen(buf));
    } // endfor i

  return n;
  } // end of GetMaxLength
#endif // 0

/* ------------------------- End of Valblk --------------------------- */

