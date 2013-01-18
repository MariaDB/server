/************ Valblk C++ Functions Source Code File (.CPP) *************/
/*  Name: VALBLK.CPP  Version 1.4                                      */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2005-2012    */
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
/*  Currently the only implemented types are STRING, int and DOUBLE.  */
/*  Shortly we should add at least int VARCHAR and DATE.              */
/***********************************************************************/

/***********************************************************************/
/*  Include relevant MariaDB header file.                  */
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

/***********************************************************************/
/*  Check macro's.                                                     */
/***********************************************************************/
#if defined(_DEBUG) || defined(DEBTRACE)
#define CheckIndex(N)   ChkIndx(N);
void VALBLK::ChkIndx(int n) {
  if (n >= Nval) {
    PGLOBAL& g = Global;
    strcpy(g->Message, MSG(BAD_VALBLK_INDX));
    longjmp(g->jumper[g->jump_level], Type);
    } // endif N
  } // end of ChkIndx
#define CheckParms(V,N) ChkPrm(V,N);
void VALBLK::ChkPrm(PVAL v, int n) {
  ChkIndx(n);
  if (Check && Type != v->GetType()) {
    PGLOBAL& g = Global;
    strcpy(g->Message, MSG(VALTYPE_NOMATCH));
    longjmp(g->jumper[g->jump_level], Type);
    } // endif Check
  } // end of ChkPrm
#define CheckBlanks     assert(!Blanks);
#define CheckType(V)    ChkTyp(V);
void VALBLK::ChkTyp(PVAL v) {
  if (Type != v->GetType()) {
    PGLOBAL& g = Global;
    strcpy(g->Message, MSG(VALTYPE_NOMATCH));
    longjmp(g->jumper[g->jump_level], Type);
    } // endif Type
  } // end of ChkTyp
void VALBLK::ChkTyp(PVBLK vb) {
  if (Type != vb->GetType()) {
    PGLOBAL& g = Global;
    strcpy(g->Message, MSG(VALTYPE_NOMATCH));
    longjmp(g->jumper[g->jump_level], Type);
    } // endif Type
  } // end of ChkTyp
#else
#define CheckIndex(N)
#define CheckParms(V,N)
#define CheckBlanks
#define CheckType(V)
#endif

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
      blkp = new(g) SHRBLK(mp, nval);
      break;
    case TYPE_INT:
      blkp = new(g) LNGBLK(mp, nval);
      break;
    case TYPE_DATE:        // ?????
      blkp = new(g) DATBLK(mp, nval);
      break;
    case TYPE_FLOAT:
      blkp = new(g) DBLBLK(mp, nval, prec);
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
  CheckType(vp)

	int n = 1;

  for (i = 0; i < Nval; i++)
    if ((n = CompVal(vp, i)) <= 0)
      break;

  return (!n);
  } // end of Locate


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
/*  Return the value of the nth element converted to int.             */
/***********************************************************************/
int CHRBLK::GetIntValue(int n)
  {
  return atol((char *)GetValPtrEx(n));
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
  CheckParms(valp, n)

  SetValue((PSZ)valp->GetCharValue(), n);
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

  } // end of SetValue

/***********************************************************************/
/*  Set one value in a block from a value in another block.            */
/***********************************************************************/
void CHRBLK::SetValue(PVBLK pv, int n1, int n2)
  {
#if defined(_DEBUG) || defined(DEBTRACE)
  if (Type != pv->GetType() || Long != ((CHRBLK*)pv)->Long) {
    PGLOBAL& g = Global;
    strcpy(g->Message, MSG(BLKTYPLEN_MISM));
    longjmp(g->jumper[g->jump_level], Type);
    } // endif Type
#endif

  memcpy(Chrp + n1 * Long, ((CHRBLK*)pv)->Chrp + n2 * Long, Long);
  } // end of SetValue

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

/***********************************************************************/
/*  Move one value from i to j.                                        */
/***********************************************************************/
void CHRBLK::Move(int i, int j)
  {
  memcpy(Chrp + j * Long, Chrp + i * Long, Long);
  } // end of Move

/***********************************************************************/
/*  Compare a Value object with the nth value of the block.            */
/***********************************************************************/
int CHRBLK::CompVal(PVAL vp, int n)
  {
  CheckParms(vp, n)
  char *xvp = vp->GetCharValue(); // Get Value zero ended string
	bool ci = Ci || vp->IsCi(); 		// true if is case insensitive

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
  CheckIndex(n)
  return Chrp + n * Long;
  } // end of GetValPtr

/***********************************************************************/
/*  Get a pointer on a zero ended string equal to nth value.           */
/***********************************************************************/
void *CHRBLK::GetValPtrEx(int n)
  {
  CheckIndex(n)
  memcpy(Valp, Chrp + n * Long, Long);

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
  CheckType(vp)
  int  i;
	bool ci = Ci || vp->IsCi();
  PSZ  s = vp->GetCharValue();

  for (i = 0; i < Nval; i++) {
    GetValPtrEx(i);               // Get a zero ended string in Valp

    if (!((ci) ? strnicmp(s, Valp, Long) : strncmp(s, Valp, Long)))
      break;

    } // endfor i

  return (i < Nval) ? i : (-1);
  } // end of GetValPtr

/***********************************************************************/
/*  Returns the length of the longest string in the block.             */
/***********************************************************************/
int CHRBLK::GetMaxLength(void)
  {
  int i, n;

  for (i = n = 0; i < Nval; i++) {
    GetValPtrEx(i);
    n = max(n, (signed)strlen(Valp));
    } // endfor i

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
  CheckType(pv)

  Strp[n1] = ((STRBLK*)pv)->Strp[n2];
  } // end of SetValue

/***********************************************************************/
/*  Set many values in a block from values in another block.           */
/***********************************************************************/
void STRBLK::SetValues(PVBLK pv, int k, int n)
  {
  CheckType(pv)
  PSZ *sp = ((STRBLK*)pv)->Strp;

  for (register int i = k; i < n; i++)
    Strp[i] = sp[i];

  } // end of SetValues

/***********************************************************************/
/*  Set one value in a block.                                          */
/***********************************************************************/
void STRBLK::SetValue(PVAL valp, int n)
  {
  CheckParms(valp, n)
  SetValue((PSZ)valp->GetCharValue(), n);
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
  CheckParms(vp, n)
  return strcmp(vp->GetCharValue(), Strp[n]);
  } // end of CompVal

/***********************************************************************/
/*  Compare two values of the block.                                   */
/***********************************************************************/
int STRBLK::CompVal(int i1, int i2)
  {
  return (strcmp(Strp[i1], Strp[i2]));
  } // end of CompVal

/***********************************************************************/
/*  Get a pointer on the nth value of the block.                       */
/***********************************************************************/
void *STRBLK::GetValPtr(int n)
  {
  CheckIndex(n)
  return Strp + n;
  } // end of GetValPtr

/***********************************************************************/
/*  Get a pointer on a zero ended string equal to nth value.           */
/***********************************************************************/
void *STRBLK::GetValPtrEx(int n)
  {
  CheckIndex(n)
  return Strp[n];
  } // end of GetValPtrEx

/***********************************************************************/
/*  Returns index of matching value in block or -1.                    */
/***********************************************************************/
int STRBLK::Find(PVAL vp)
  {
  CheckType(vp)
  int i;
  PSZ s = vp->GetCharValue();

  for (i = 0; i < Nval; i++)
    if (!strcmp(s, Strp[i]))
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
    n = max(n, (signed)strlen(Strp[i]));

  return n;
  } // end of GetMaxLength


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
  Shrp[n] = valp->GetShortValue();
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
  } // end of SetValue

/***********************************************************************/
/*  Set one value in a block if val is less than the current value.    */
/***********************************************************************/
void SHRBLK::SetMin(PVAL valp, int n)
  {
  CheckParms(valp, n)
  short  sval = valp->GetShortValue();
  short& smin = Shrp[n];

  if (sval < smin)
    smin = sval;

  } // end of SetMin

/***********************************************************************/
/*  Set one value in a block if val is greater than the current value. */
/***********************************************************************/
void SHRBLK::SetMax(PVAL valp, int n)
  {
  CheckParms(valp, n)
  short  sval = valp->GetShortValue();
  short& smin = Shrp[n];

  if (sval > smin)
    smin = sval;

  } // end of SetMax

/***********************************************************************/
/*  Set one value in a block from a value in another block.            */
/***********************************************************************/
void SHRBLK::SetValue(PVBLK pv, int n1, int n2)
  {
  CheckType(pv)

  Shrp[n1] = ((SHRBLK*)pv)->Shrp[n2];
  } // end of SetValue

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

/***********************************************************************/
/*  This function is used by class RESCOL when calculating COUNT.      */
/***********************************************************************/
void SHRBLK::AddMinus1(PVBLK pv, int n1, int n2)
  {
  assert(Type == pv->GetType());
  Shrp[n1] += (((SHRBLK*)pv)->Shrp[n2] - 1);
  } // end of AddMinus1

/***********************************************************************/
/*  Move one value from i to j.                                        */
/***********************************************************************/
void SHRBLK::Move(int i, int j)
  {
  Shrp[j] = Shrp[i];
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
  CheckIndex(n)
  return Shrp + n;
  } // end of GetValPtr

/***********************************************************************/
/*  Get a pointer on the nth value of the block.                       */
/***********************************************************************/
void *SHRBLK::GetValPtrEx(int n)
  {
  CheckIndex(n)
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
  Lngp[n] = valp->GetIntValue();
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
/*  Set one value in a block if val is less than the current value.    */
/***********************************************************************/
void LNGBLK::SetMin(PVAL valp, int n)
  {
  CheckParms(valp, n)
  int  lval = valp->GetIntValue();
  int& lmin = Lngp[n];

  if (lval < lmin)
    lmin = lval;

  } // end of SetMin

/***********************************************************************/
/*  Set one value in a block if val is greater than the current value. */
/***********************************************************************/
void LNGBLK::SetMax(PVAL valp, int n)
  {
  CheckParms(valp, n)
  int  lval = valp->GetIntValue();
  int& lmax = Lngp[n];

  if (lval > lmax)
    lmax = lval;

  } // end of SetMax

/***********************************************************************/
/*  Set one value in a block from a value in another block.            */
/***********************************************************************/
void LNGBLK::SetValue(PVBLK pv, int n1, int n2)
  {
  CheckType(pv)

  Lngp[n1] = ((LNGBLK*)pv)->Lngp[n2];
  } // end of SetValue

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

/***********************************************************************/
/*  This function is used by class RESCOL when calculating COUNT.      */
/***********************************************************************/
void LNGBLK::AddMinus1(PVBLK pv, int n1, int n2)
  {
  assert(Type == pv->GetType());
  Lngp[n1] += (((LNGBLK*)pv)->Lngp[n2] - 1);
  } // end of AddMinus1

/***********************************************************************/
/*  Move one value from i to j.                                        */
/***********************************************************************/
void LNGBLK::Move(int i, int j)
  {
  Lngp[j] = Lngp[i];
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
  CheckIndex(n)
  return Lngp + n;
  } // end of GetValPtr

/***********************************************************************/
/*  Get a pointer on the nth value of the block.                       */
/***********************************************************************/
void *LNGBLK::GetValPtrEx(int n)
  {
  CheckIndex(n)
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


/* -------------------------- Class DATBLK --------------------------- */

/***********************************************************************/
/*  Constructor.                                                       */
/***********************************************************************/
DATBLK::DATBLK(void *mp, int nval) : LNGBLK(mp, nval)
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
    Lngp[n] = Dvalp->GetIntValue();
  } else
    LNGBLK::SetValue(p, n);

  } // end of SetValue


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

  Dblp[n1] = ((DBLBLK*)pv)->Dblp[n2];
  } // end of SetValue

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

/***********************************************************************/
/*  Set one value in a block.                                          */
/***********************************************************************/
void DBLBLK::SetValue(PVAL valp, int n)
  {
  CheckParms(valp, n)
  Dblp[n] = valp->GetFloatValue();
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

/***********************************************************************/
/*  Set one value in a block if val is less than the current value.    */
/***********************************************************************/
void DBLBLK::SetMin(PVAL valp, int n)
  {
  CheckParms(valp, n)
  double  fval = valp->GetFloatValue();
  double& fmin = Dblp[n];

  if (fval < fmin)
    fmin = fval;

  } // end of SetMin

/***********************************************************************/
/*  Set one value in a block if val is greater than the current value. */
/***********************************************************************/
void DBLBLK::SetMax(PVAL valp, int n)
  {
  CheckParms(valp, n)
  double  fval = valp->GetFloatValue();
  double& fmax = Dblp[n];

  if (fval > fmax)
    fmax = fval;

  } // end of SetMax

/***********************************************************************/
/*  Move one value from i to j.                                        */
/***********************************************************************/
void DBLBLK::Move(int i, int j)
  {
  Dblp[j] = Dblp[i];
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
  CheckIndex(n)
  return Dblp + n;
  } // end of GetValPtr

/***********************************************************************/
/*  Get a pointer on the nth value of the block.                       */
/***********************************************************************/
void *DBLBLK::GetValPtrEx(int n)
  {
  CheckIndex(n)
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

/* ------------------------- End of Valblk --------------------------- */

