/************ Valblk C++ Functions Source Code File (.CPP) *************/
/*  Name: VALBLK.CPP  Version 1.7                                      */
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
    case TYPE_TINY:
      blkp = new(g) TYPBLK<char>(mp, nval, type);
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

template <>
char TYPBLK<char>::GetTypedValue(PVAL valp)
  {return valp->GetTinyValue();}

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
template <>
char TYPBLK<char>::GetTypedValue(PSZ p) {return (char)atoi(p);}

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

template <>
char TYPBLK<char>::GetTypedValue(PVBLK blk, int n)
  {return blk->GetTinyValue(n);}

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
  } // end of GetShortValue

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
  } // end of GetBigintValue

/***********************************************************************/
/*  Return the value of the nth element converted to double.           */
/***********************************************************************/
double CHRBLK::GetFloatValue(int n)
  {
  return atof((char *)GetValPtrEx(n));
  } // end of GetFloatValue

/***********************************************************************/
/*  Return the value of the nth element converted to tiny int.         */
/***********************************************************************/
char CHRBLK::GetTinyValue(int n)
  {
  return (char)atoi((char *)GetValPtrEx(n));
  } // end of GetTinyValue

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

/* ------------------------- End of Valblk --------------------------- */

