/************* BlkFil C++ Program Source Code File (.CPP) **************/
/* PROGRAM NAME: BLKFIL                                                */
/* -------------                                                       */
/*  Version 2.6                                                        */
/*                                                                     */
/* COPYRIGHT:                                                          */
/* ----------                                                          */
/*  (C) Copyright to the author Olivier BERTRAND          2004-2017    */
/*                                                                     */
/* WHAT THIS PROGRAM DOES:                                             */
/* -----------------------                                             */
/*  This program is the implementation of block indexing classes.      */
/*                                                                     */
/***********************************************************************/

/***********************************************************************/
/*  Include relevant MariaDB header file.                              */
/***********************************************************************/
#include "my_global.h"
#include "sql_class.h"
//#include "sql_time.h"

#if defined(_WIN32)
//#include <windows.h>
#else   // !_WIN32
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#endif  // !_WIN32

/***********************************************************************/
/*  Include application header files:                                  */
/***********************************************************************/
#include "global.h"      // global declarations
#include "plgdbsem.h"    // DB application declarations
#include "xindex.h"      // Key Index class declarations
#include "filamtxt.h"    // File access method dcls
#include "tabdos.h"      // TDBDOS and DOSCOL class dcls
#include "array.h"       // ARRAY classes dcls
#include "blkfil.h"      // Block Filter classes dcls

/* ------------------------ Class BLOCKFILTER ------------------------ */

/***********************************************************************/
/*  BLOCKFILTER constructor.                                           */
/***********************************************************************/
BLOCKFILTER::BLOCKFILTER(PTDBDOS tdbp, int op)
  {
  Tdbp = tdbp;
  Correl = FALSE;
  Opc = op;
  Opm = 0;
  Result = 0;
  } // end of BLOCKFILTER constructor

/***********************************************************************/
/*  Make file output of BLOCKFILTER contents.                          */
/***********************************************************************/
void BLOCKFILTER::Printf(PGLOBAL, FILE *f, uint n)
  {
  char m[64];

  memset(m, ' ', n);                    // Make margin string
  m[n] = '\0';

  fprintf(f, "%sBLOCKFILTER: at %p opc=%d opm=%d result=%d\n",
          m, this, Opc, Opm, Result);
  } // end of Printf

/***********************************************************************/
/*  Make string output of BLOCKFILTER contents.                        */
/***********************************************************************/
void BLOCKFILTER::Prints(PGLOBAL, char *ps, uint z)
  {
  strncat(ps, "BlockFilter(s)", z);
  } // end of Prints


/* ---------------------- Class BLKFILLOG ---------------------------- */

/***********************************************************************/
/*  BLKFILLOG constructor.                                             */
/***********************************************************************/
BLKFILLOG::BLKFILLOG(PTDBDOS tdbp, int op, PBF *bfp, int n)
         : BLOCKFILTER(tdbp, op)
  {
  N = n;
  Fil = bfp;

  for (int i = 0; i < N; i++)
    if (Fil[i])
      Correl |= Fil[i]->Correl;

  } // end of BLKFILLOG constructor

/***********************************************************************/
/*  Reset: this function is used only to check the existence of a      */
/*  BLKFILIN block and have it reset its Bot value for sorted columns. */
/***********************************************************************/
void BLKFILLOG::Reset(PGLOBAL g)
  {
  for (int i = 0; i < N; i++)
    if (Fil[i])
      Fil[i]->Reset(g);

  } // end of Reset

/***********************************************************************/
/*  This function is used for block filter evaluation. We use here a   */
/*  fuzzy logic between the values returned by evaluation blocks:      */
/* -2: the condition will be always false for the rest of the file.    */
/* -1: the condition will be false for the whole group.                */
/*  0: the condition may be true for some of the group values.         */
/*  1: the condition will be true for the whole group.                 */
/*  2: the condition will be always true for the rest of the file.     */
/***********************************************************************/
int BLKFILLOG::BlockEval(PGLOBAL g)
  {
  int  i, rc;

  for (i = 0; i < N; i++) {
    // 0: Means some block filter value may be True
    rc = (Fil[i]) ? Fil[i]->BlockEval(g) : 0;

    if (!i)
      Result = (Opc == OP_NOT) ? -rc : rc;
    else switch (Opc) {
      case OP_AND:
        Result = MY_MIN(Result, rc);
        break;
      case OP_OR:
        Result = MY_MAX(Result, rc);
        break;
      default:
        // Should never happen
        Result = 0;
        return Result;
      } // endswitch Opc

    } // endfor i

  return Result;
  } // end of BlockEval

/* ---------------------- Class BLKFILARI----------------------------- */

/***********************************************************************/
/*  BLKFILARI constructor.                                             */
/***********************************************************************/
BLKFILARI::BLKFILARI(PGLOBAL g, PTDBDOS tdbp, int op, PXOB *xp)
         : BLOCKFILTER(tdbp, op)
  {
  Colp = (PDOSCOL)xp[0];

  if (xp[1]->GetType() == TYPE_COLBLK) {
    Cpx = (PCOL)xp[1];      // Subquery pseudo constant column
    Correl = TRUE;
  } else
    Cpx = NULL;

  Sorted = Colp->IsSorted() > 0;

  // Don't remember why this was changed. Anyway it is no good for
  // correlated subqueries because the Value must reflect changes
  if (Cpx)
    Valp = xp[1]->GetValue();
  else
    Valp = AllocateValue(g, xp[1]->GetValue());

  } // end of BLKFILARI constructor

/***********************************************************************/
/*  Reset: re-eval the constant value in the case of pseudo constant   */
/*  column use in a correlated subquery.                               */
/***********************************************************************/
void BLKFILARI::Reset(PGLOBAL g)
  {
  if (Cpx) {
    Cpx->Reset();
    Cpx->Eval(g);
    MakeValueBitmap();      // Does nothing for class BLKFILARI
    } // endif Cpx

  } // end of Reset

/***********************************************************************/
/*  Evaluate block filter for arithmetic operators.                    */
/***********************************************************************/
int BLKFILARI::BlockEval(PGLOBAL)
  {
  int mincmp, maxcmp, n;

#if defined(_DEBUG)
  assert (Colp->IsClustered());
#endif

  n = ((PTDBDOS)Colp->GetTo_Tdb())->GetCurBlk();
  mincmp = Colp->GetMin()->CompVal(Valp, n);
  maxcmp = Colp->GetMax()->CompVal(Valp, n);

  switch (Opc) {
    case OP_EQ:
    case OP_NE:
      if (mincmp < 0)                // Means minval > Val
        Result = (Sorted) ? -2 : -1;
      else if (maxcmp > 0)           // Means maxval < Val
        Result = -1;
      else if (!mincmp && !maxcmp)   // minval = maxval = val
        Result = 1;
      else
        Result = 0;

      break;
    case OP_GT:
    case OP_LE:
      if (mincmp < 0)                // minval > Val
        Result = (Sorted) ? 2 : 1;
      else if (maxcmp < 0)           // maxval > Val
        Result = 0;
      else                           // maxval <= Val
        Result = -1;

      break;
    case OP_GE:
    case OP_LT:
      if (mincmp <= 0)               // minval >= Val
        Result = (Sorted) ? 2 : 1;
      else if (maxcmp <= 0)          // Maxval >= Val
        Result = 0;
      else                           // Maxval < Val
        Result = -1;

      break;
    } // endswitch Opc

  switch (Opc) {
    case OP_NE:
    case OP_LE:
    case OP_LT:
      Result = -Result;
      break;
    } // endswitch Opc

  if (trace(1))
    htrc("BlockEval: op=%d n=%d rc=%d\n", Opc, n, Result);

  return Result;
  } // end of BlockEval

/* ---------------------- Class BLKFILAR2----------------------------- */

/***********************************************************************/
/*  BLKFILAR2 constructor.                                             */
/***********************************************************************/
BLKFILAR2::BLKFILAR2(PGLOBAL g, PTDBDOS tdbp, int op, PXOB *xp)
         : BLKFILARI(g, tdbp, op, xp)
  {
  MakeValueBitmap();
  } // end of BLKFILAR2 constructor

/***********************************************************************/
/*  MakeValueBitmap: Set the constant value bit map. It can be void    */
/*  if the constant value is not in the column distinct values list.   */
/***********************************************************************/
void BLKFILAR2::MakeValueBitmap(void)
  {
  int   i; // ndv = Colp->GetNdv();
  bool  found = FALSE;
  PVBLK dval = Colp->GetDval();

  assert(dval);

  /*********************************************************************/
  /*  Here we cannot use Find because we must get the index            */
  /*  of where to put the value if it is not found in the array.       */
  /*  This is needed by operators other than OP_EQ or OP_NE.           */
  /*********************************************************************/
  found = dval->Locate(Valp, i);

  /*********************************************************************/
  /*  Set the constant value bitmap. The bitmaps are really matching   */
  /*  the OP_EQ, OP_LE, and OP_LT operator but are also used for the   */
  /*  other operators for which the Result will be inverted.           */
  /*  The reason the bitmaps are not directly complemented for them is */
  /*  to be able to test easily the cases of sorted columns with Bxp,  */
  /*  and the case of a void bitmap, which happens if the constant     */
  /*  value is not in the column distinct values list.                 */
  /*********************************************************************/
  if (found) {
    Bmp = 1 << i;               // Bit of the found value
    Bxp = Bmp - 1;              // All smaller values

    if (Opc != OP_LT && Opc != OP_GE)
      Bxp |= Bmp;               // Found value must be included

  } else {
    Bmp = 0;
    Bxp = (1 << i) - 1;
  } // endif found

  if (!(Opc == OP_EQ || Opc == OP_NE))
    Bmp = Bxp;

  } // end of MakeValueBitmap

/***********************************************************************/
/*  Evaluate XDB2 block filter for arithmetic operators.               */
/***********************************************************************/
int BLKFILAR2::BlockEval(PGLOBAL)
  {
#if defined(_DEBUG)
  assert (Colp->IsClustered());
#endif

  int   n = ((PTDBDOS)Colp->GetTo_Tdb())->GetCurBlk();
  uint  bkmp = *(uint*)Colp->GetBmap()->GetValPtr(n);
  uint  bres = Bmp & bkmp;

  // Set result as if Opc were OP_EQ, OP_LT, or OP_LE
  if (!bres) {
    if (!Bmp)
      Result = -2;              // No good block in the table file
    else if (!Sorted)
      Result = -1;              // No good values in this block
    else    // Sorted column, test for no more good blocks in file
      Result = (Bxp & bkmp) ? -1 : -2;

  } else
    // Test whether all block values are good or only some ones
    Result = (bres == bkmp) ? 1 : 0;

  // For OP_NE, OP_GE, and OP_GT the result must be inverted.
  switch (Opc) {
    case OP_NE:
    case OP_GE:
    case OP_GT:
      Result = -Result;
      break;
    } // endswitch Opc

  if (trace(1))
    htrc("BlockEval2: op=%d n=%d rc=%d\n", Opc, n, Result);

  return Result;
  } // end of BlockEval

/* ---------------------- Class BLKFILMR2----------------------------- */

/***********************************************************************/
/*  BLKFILMR2 constructor.                                             */
/***********************************************************************/
BLKFILMR2::BLKFILMR2(PGLOBAL g, PTDBDOS tdbp, int op, PXOB *xp)
         : BLKFILARI(g, tdbp, op, xp)
  {
  Nbm = Colp->GetNbm();
  Bmp = (uint*)PlugSubAlloc(g, NULL, Nbm * sizeof(uint));
  Bxp = (uint*)PlugSubAlloc(g, NULL, Nbm * sizeof(uint));
  MakeValueBitmap();
  } // end of BLKFILMR2 constructor

/***********************************************************************/
/*  MakeValueBitmap: Set the constant value bit map. It can be void    */
/*  if the constant value is not in the column distinct values list.   */
/***********************************************************************/
void BLKFILMR2::MakeValueBitmap(void)
  {
  int   i; // ndv = Colp->GetNdv();
  bool  found = FALSE, noteq = !(Opc == OP_EQ || Opc == OP_NE);
  PVBLK dval = Colp->GetDval();

  assert(dval);

  for (i = 0; i < Nbm; i++)
    Bmp[i] = Bxp[i] = 0;

  /*********************************************************************/
  /*  Here we cannot use Find because we must get the index            */
  /*  of where to put the value if it is not found in the array.       */
  /*  This is needed by operators other than OP_EQ or OP_NE.           */
  /*********************************************************************/
  found = dval->Locate(Valp, i);

  /*********************************************************************/
  /*  For bitmaps larger than a ULONG, we must know where Bmp and Bxp  */
  /*  are positioned in the ULONG bit map block array.                 */
  /*********************************************************************/
  N = i / MAXBMP;
  i %= MAXBMP;

  /*********************************************************************/
  /*  Set the constant value bitmaps. The bitmaps are really matching  */
  /*  the OP_EQ, OP_LE, and OP_LT operator but are also used for the   */
  /*  other operators for which the Result will be inverted.           */
  /*  The reason the bitmaps are not directly complemented for them is */
  /*  to be able to easily test the cases of sorted columns with Bxp,  */
  /*  and the case of a void bitmap, which happens if the constant     */
  /*  value is not in the column distinct values list.                 */
  /*********************************************************************/
  if (found) {
    Bmp[N] = 1 << i;
    Bxp[N] = Bmp[N] - 1;

    if (Opc != OP_LT && Opc != OP_GE)
      Bxp[N] |= Bmp[N];    // Found value must be included

  } else
    Bxp[N] = (1 << i) - 1;

  if (noteq)
    Bmp[N] = Bxp[N];

  Void = !Bmp[N];          // There are no good values in the file

  for (i = 0; i < N; i++) {
    Bxp[i] = ~0;

    if (noteq)
      Bmp[i] = Bxp[i];

    Void = Void && !Bmp[i];
    } // endfor i

  if (!Bmp[N] && !Bxp[N])
    N--;

  } // end of MakeValueBitmap

/***********************************************************************/
/*  Evaluate XDB2 block filter for arithmetic operators.               */
/***********************************************************************/
int BLKFILMR2::BlockEval(PGLOBAL)
  {
#if defined(_DEBUG)
  assert (Colp->IsClustered());
#endif

  int    i, n = ((PTDBDOS)Colp->GetTo_Tdb())->GetCurBlk();
  bool   fnd = FALSE, all = TRUE, gt = TRUE;
  uint   bres;
  uint  *bkmp = (uint*)Colp->GetBmap()->GetValPtr(n * Nbm);

  // Set result as if Opc were OP_EQ, OP_LT, or OP_LE
  for (i = 0; i < Nbm; i++)
    if (i <= N) {
      if ((bres = Bmp[i] & bkmp[i]))
        fnd = TRUE;         // Some good value(s) found in the block

      if (bres != bkmp[i])
        all = FALSE;        // Not all block values are good

      if (Bxp[i] & bkmp[i])
        gt = FALSE;         // Not all block values are > good value(s)

    } else if (bkmp[i]) {
      all = FALSE;
      break;
    } // endif's

  if (!fnd) {
    if (Void || (gt && Sorted))
      Result = -2;          // No (more) good block in file
    else
      Result = -1;          // No good values in this block

  } else
    Result = (all) ? 1 : 0;                 // All block values are good

  // For OP_NE, OP_GE, and OP_GT the result must be inverted.
  switch (Opc) {
    case OP_NE:
    case OP_GE:
    case OP_GT:
      Result = -Result;
      break;
    } // endswitch Opc

  if (trace(1))
    htrc("BlockEval2: op=%d n=%d rc=%d\n", Opc, n, Result);

  return Result;
  } // end of BlockEval

/***********************************************************************/
/*  BLKSPCARI constructor.                                             */
/***********************************************************************/
BLKSPCARI::BLKSPCARI(PTDBDOS tdbp, int op, PXOB *xp, int bsize)
         : BLOCKFILTER(tdbp, op)
  {
  if (xp[1]->GetType() == TYPE_COLBLK) {
    Cpx = (PCOL)xp[1];      // Subquery pseudo constant column
    Correl = TRUE;
  } else
    Cpx = NULL;

  Valp = xp[1]->GetValue();
  Val = (int)xp[1]->GetValue()->GetIntValue();
  Bsize = bsize;
  } // end of BLKFILARI constructor

/***********************************************************************/
/*  Reset: re-eval the constant value in the case of pseudo constant   */
/*  column use in a correlated subquery.                               */
/***********************************************************************/
void BLKSPCARI::Reset(PGLOBAL g)
  {
  if (Cpx) {
    Cpx->Reset();
    Cpx->Eval(g);
    Val = (int)Valp->GetIntValue();
    } // endif Cpx

  } // end of Reset

/***********************************************************************/
/*  Evaluate block filter for arithmetic operators (ROWID)             */
/***********************************************************************/
int BLKSPCARI::BlockEval(PGLOBAL)
  {
  int mincmp, maxcmp, n, m;

  n = Tdbp->GetCurBlk();
  m = n * Bsize + 1;     // Minimum Rowid value for this block
  mincmp = (Val > m) ? 1 : (Val < m) ? (-1) : 0;
  m = (n + 1) * Bsize;   // Maximum Rowid value for this block
  maxcmp = (Val > m) ? 1 : (Val < m) ? (-1) : 0;

  switch (Opc) {
    case OP_EQ:
    case OP_NE:
      if (mincmp < 0)                // Means minval > Val
        Result = -2;                 // Always sorted
      else if (maxcmp > 0)           // Means maxval < Val
        Result = -1;
      else if (!mincmp && !maxcmp)   // minval = maxval = val
        Result = 1;
      else
        Result = 0;

      break;
    case OP_GT:
    case OP_LE:
      if (mincmp < 0)                // minval > Val
        Result = 2;                  // Always sorted
      else if (maxcmp < 0)           // maxval > Val
        Result = 0;
      else                           // maxval <= Val
        Result = -1;

      break;
    case OP_GE:
    case OP_LT:
      if (mincmp <= 0)               // minval >= Val
        Result = 2;                  // Always sorted
      else if (maxcmp <= 0)          // Maxval >= Val
        Result = 0;
      else                           // Maxval < Val
        Result = -1;

      break;
    } // endswitch Opc

  switch (Opc) {
    case OP_NE:
    case OP_LE:
    case OP_LT:
      Result = -Result;
      break;
    } // endswitch Opc

  if (trace(1))
    htrc("BlockEval: op=%d n=%d rc=%d\n", Opc, n, Result);

  return Result;
  } // end of BlockEval

/* ------------------------ Class BLKFILIN --------------------------- */

/***********************************************************************/
/*  BLKFILIN constructor.                                              */
/***********************************************************************/
BLKFILIN::BLKFILIN(PGLOBAL g, PTDBDOS tdbp, int op, int opm, PXOB *xp)
        : BLOCKFILTER(tdbp, op)
  {
  if (op == OP_IN) {
    Opc = OP_EQ;
    Opm = 1;
  } else {
    Opc = op;
    Opm = opm;
  } // endif op

  Colp = (PDOSCOL)xp[0];
  Arap = (PARRAY)xp[1];
  Type = Arap->GetResultType();

  if (Colp->GetResultType() != Type) {
    sprintf(g->Message, "BLKFILIN: %s", MSG(VALTYPE_NOMATCH));
		throw g->Message;
	} else if (Colp->GetValue()->IsCi())
    Arap->SetPrecision(g, 1);        // Case insensitive

  Sorted = Colp->IsSorted() > 0;
  } // end of BLKFILIN constructor

/***********************************************************************/
/*  Reset: have the sorted array reset its Bot value to -1 (bottom).   */
/***********************************************************************/
void BLKFILIN::Reset(PGLOBAL)
  {
  Arap->Reset();
//  MakeValueBitmap();      // Does nothing for class BLKFILIN
  } // end of Reset

/***********************************************************************/
/*  Evaluate block filter for a IN operator on a constant array.       */
/*  Note: here we need to use the GetValPtrEx function to get a zero   */
/*  ended string in case of string argument. This is because the ARRAY */
/*  can have a different width than the char column.                   */
/***********************************************************************/
int BLKFILIN::BlockEval(PGLOBAL g)
  {
  int   n = ((PTDBDOS)Colp->GetTo_Tdb())->GetCurBlk();
  void *minp = Colp->GetMin()->GetValPtrEx(n);
  void *maxp = Colp->GetMax()->GetValPtrEx(n);

  Result = Arap->BlockTest(g, Opc, Opm, minp, maxp, Sorted);
  return Result;
  } // end of BlockEval

/* ------------------------ Class BLKFILIN2 -------------------------- */

/***********************************************************************/
/*  BLKFILIN2 constructor.                                             */
/*  New version that takes care of all operators and modificators.     */
/*  It is also ready to handle the case of correlated sub-selects.     */
/***********************************************************************/
BLKFILIN2::BLKFILIN2(PGLOBAL g, PTDBDOS tdbp, int op, int opm, PXOB *xp)
         : BLKFILIN(g, tdbp, op, opm, xp)
  {
  Nbm = Colp->GetNbm();
  Valp = AllocateValue(g, Colp->GetValue());
  Invert = (Opc == OP_NE || Opc == OP_GE || Opc ==OP_GT);
  Bmp = (uint*)PlugSubAlloc(g, NULL, Nbm * sizeof(uint));
  Bxp = (uint*)PlugSubAlloc(g, NULL, Nbm * sizeof(uint));
  MakeValueBitmap();
  } // end of BLKFILIN2 constructor

/***********************************************************************/
/*  MakeValueBitmap: Set the constant values bit map. It can be void   */
/*  if the constant values are not in the column distinct values list. */
/*  The bitmaps are prepared for the EQ, LE, and LT operators and      */
/*  takes care of the ALL and ANY modificators. If the operators are   */
/*  NE, GE, or GT the modificator is inverted and the result will be.  */
/***********************************************************************/
void BLKFILIN2::MakeValueBitmap(void)
  {
  int   i, k, n, ndv = Colp->GetNdv();
  bool  found, noteq = !(Opc == OP_EQ || Opc == OP_NE);
  bool  all = (!Invert) ? (Opm == 2) : (Opm != 2);
  uint  btp;
  PVBLK dval = Colp->GetDval();

  N = -1;

  // Take care of special cases
  if (!(n = Arap->GetNval())) {
    // Return TRUE for ALL because it means that there are no item that
    // does not verify the condition, which is true indeed.
    // Return FALSE for ANY because TRUE means that there is at least
    // one item that verifies the condition, which is false.
    Result = (Opm == 2) ? 2 : -2;
    return;
  } else if (!noteq && all && n > 1) {
    // An item cannot be equal to all different values
    // or an item is always unequal to any different values
    Result = (Opc == OP_EQ) ? -2 : 2;
    return;
  } // endif's

  for (i = 0; i < Nbm; i++)
    Bmp[i] = Bxp[i] = 0;

  for (k = 0; k < n; k++) {
    Arap->GetNthValue(Valp, k);
    found = dval->Locate(Valp, i);
    N = i / MAXBMP;
    btp = 1 << (i % MAXBMP);

    if (found)
      Bmp[N] |= btp;

    // For LT and LE if ALL the condition applies to the smallest item
    // if ANY it applies to the largest item. In the case of EQ we come
    // here only if ANY or if n == 1, so it does applies to the largest.
    if ((!k && all) || (k == n - 1 && !all)) {
      Bxp[N] = btp - 1;

      if (found && Opc != OP_LT && Opc != OP_GE)
        Bxp[N] |= btp;     // Found value must be included

      } // endif k, opm

    } // endfor k

  if (noteq)
    Bmp[N] = Bxp[N];

  Void = !Bmp[N];          // There are no good values in the file

  for (i = 0; i < N; i++) {
    Bxp[i] = ~0;

    if (noteq) {
      Bmp[i] = Bxp[i];
      Void = FALSE;
      } // endif noteq

    } // endfor i

  if (!Bmp[N] && !Bxp[N]) {
    if (--N < 0)
      // All array values are smaller than block values
      Result = (Invert) ? 2 : -2;

  } else if (N == Nbm - 1 && (signed)Bmp[N] == (1 << (ndv % MAXBMP)) - 1) {
    // Condition will be always TRUE or FALSE for the whole file
    Result = (Invert) ? -2 : 2;
    N = -1;
  } // endif's

  } // end of MakeValueBitmap

/***********************************************************************/
/*  Evaluate block filter for set operators on a constant array.       */
/*  Note: here we need to use the GetValPtrEx function to get a zero   */
/*  ended string in case of string argument. This is because the ARRAY */
/*  can have a different width than the char column.                   */
/***********************************************************************/
int BLKFILIN2::BlockEval(PGLOBAL)
  {
  if (N < 0)
    return Result;                  // Was set in MakeValueBitmap

  int    i, n = ((PTDBDOS)Colp->GetTo_Tdb())->GetCurBlk();
  bool   fnd = FALSE, all = TRUE, gt = TRUE;
  uint   bres;
  uint  *bkmp = (uint*)Colp->GetBmap()->GetValPtr(n * Nbm);

  // Set result as if Opc were OP_EQ, OP_LT, or OP_LE
  // The difference between ALL or ANY was handled in MakeValueBitmap
  for (i = 0; i < Nbm; i++)
    if (i <= N) {
      if ((bres = Bmp[i] & bkmp[i]))
        fnd = TRUE;

      if (bres != bkmp[i])
        all = FALSE;

      if (Bxp[i] & bkmp[i])
        gt = FALSE;

    } else if (bkmp[i]) {
      all = FALSE;
      break;
    } // endif's

  if (!fnd) {
    if (Void || (Sorted && gt))
      Result = -2;              // No more good block in file
    else
      Result = -1;              // No good values in this block

  } else if (all)
    Result = 1;                 // All block values are good
  else
    Result = 0;                 // Block contains some good values

  // For OP_NE, OP_GE, and OP_GT the result must be inverted.
  switch (Opc) {
    case OP_NE:
    case OP_GE:
    case OP_GT:
      Result = -Result;
      break;
    } // endswitch Opc

  return Result;
  } // end of BlockEval

#if 0
/***********************************************************************/
/*  BLKFILIN2 constructor.                                             */
/***********************************************************************/
BLKFILIN2::BLKFILIN2(PGLOBAL g, PTDBDOS tdbp, int op, int opm, PXOB *xp)
         : BLKFILIN(g, tdbp, op, opm, xp)
  {
  // Currently, bitmap matching is only implemented for the IN operator
  if (!(Bitmap = (op == OP_IN || (op == OP_EQ && opm != 2)))) {
    Nbm = Colp->GetNbm();
    N = 0;
    return;           // Revert to standard minmax method
    } // endif minmax

  int   i, n;
  ULONG btp;
  PVAL  valp = AllocateValue(g, Colp->GetValue());
  PVBLK dval = Colp->GetDval();

  Nbm = Colp->GetNbm();
  N = -1;
  Bmp = (PULONG)PlugSubAlloc(g, NULL, Nbm * sizeof(ULONG));
  Bxp = (PULONG)PlugSubAlloc(g, NULL, Nbm * sizeof(ULONG));

  for (i = 0; i < Nbm; i++)
    Bmp[i] = Bxp[i] = 0;

  for (n = 0; n < Arap->GetNval(); n++) {
    Arap->GetNthValue(valp, n);

    if ((i = dval->Find(valp)) >= 0)
      Bmp[i / MAXBMP] |= 1 << (i % MAXBMP);

    } // endfor n

  for (i = Nbm - 1; i >= 0; i--)
    if (Bmp[i]) {
      for (btp = Bmp[i]; btp; btp >>= 1)
        Bxp[i] |= btp;

      for (N = i--; i >= 0; i--)
        Bxp[i] = ~0;

      break;
      } // endif Bmp

  } // end of BLKFILIN2 constructor

/***********************************************************************/
/*  Evaluate block filter for a IN operator on a constant array.       */
/*  Note: here we need to use the GetValPtrEx function to get a zero   */
/*  ended string in case of string argument. This is because the ARRAY */
/*  can have a different width than the char column.                   */
/***********************************************************************/
int BLKFILIN2::BlockEval(PGLOBAL g)
  {
  if (N < 0)
    return -2;                  // IN list contains no good values

  int    i, n = ((PTDBDOS)Colp->GetTo_Tdb())->GetCurBlk();
  bool   fnd = FALSE, all = TRUE, gt = TRUE;
  ULONG  bres;
  PULONG bkmp = (PULONG)Colp->GetBmap()->GetValPtr(n * Nbm);

  if (Bitmap) {
    // For IN operator use the bitmap method
    for (i = 0; i < Nbm; i++)
      if (i <= N) {
        if ((bres = Bmp[i] & bkmp[i]))
          fnd = TRUE;

        if (bres != bkmp[i])
          all = FALSE;

        if (Bxp[i] & bkmp[i])
          gt = FALSE;

      } else if (bkmp[i]) {
        all = FALSE;
        break;
      } // endif's

    if (!fnd) {
      if (Sorted && gt)
        Result = -2;              // No more good block in file
      else
        Result = -1;              // No good values in this block

    } else if (all)
      Result = 1;                 // All block values are good
    else
      Result = 0;                 // Block contains some good values

  } else {
    // For other than IN operators, revert to standard minmax method
    int   n = 0, ndv = Colp->GetNdv();
    void *minp = NULL;
    void *maxp = NULL;
    ULONG btp;
    PVBLK dval = Colp->GetDval();

    for (i = 0; i < Nbm; i++)
      for (btp = 1; btp && n < ndv; btp <<= 1, n++)
        if (btp & bkmp[i]) {
          if (!minp)
            minp = dval->GetValPtrEx(n);

          maxp = dval->GetValPtrEx(n);
          } // endif btp

    Result = Arap->BlockTest(g, Opc, Opm, minp, maxp, Colp->IsSorted());
  } // endif Bitmap

  return Result;
  } // end of BlockEval
#endif // 0

/* ------------------------ Class BLKSPCIN --------------------------- */

/***********************************************************************/
/*  BLKSPCIN constructor.                                              */
/***********************************************************************/
BLKSPCIN::BLKSPCIN(PGLOBAL, PTDBDOS tdbp, int op, int opm,
                   PXOB *xp, int bsize)
        : BLOCKFILTER(tdbp, op)
  {
  if (op == OP_IN) {
    Opc = OP_EQ;
    Opm = 1;
  } else
    Opm = opm;

  Arap = (PARRAY)xp[1];
#if defined(_DEBUG)
  assert (Opm);
  assert (Arap->GetResultType() == TYPE_INT);
#endif
  Bsize = bsize;
  } // end of BLKSPCIN constructor

/***********************************************************************/
/*  Reset: have the sorted array reset its Bot value to -1 (bottom).   */
/***********************************************************************/
void BLKSPCIN::Reset(PGLOBAL)
  {
  Arap->Reset();
  } // end of Reset

/***********************************************************************/
/*  Evaluate block filter for a IN operator on a constant array.       */
/***********************************************************************/
int BLKSPCIN::BlockEval(PGLOBAL g)
  {
  int  n = Tdbp->GetCurBlk();
  int minrow = n * Bsize + 1;   // Minimum Rowid value for this block
  int maxrow = (n + 1) * Bsize; // Maximum Rowid value for this block

  Result = Arap->BlockTest(g, Opc, Opm, &minrow, &maxrow, TRUE);
  return Result;
  } // end of BlockEval

/* ------------------------------------------------------------------- */

#if 0
/***********************************************************************/
/*  Implementation of the BLOCKINDEX class.                            */
/***********************************************************************/
BLOCKINDEX::BLOCKINDEX(PBX nx, PDOSCOL cp, PKXBASE kp)
  {
  Next = nx;
  Tdbp = (cp) ? (PTDBDOS)cp->GetTo_Tdb() : NULL;
  Colp = cp;
  Kxp = kp;
  Type = (cp) ? cp->GetResultType() : TYPE_ERROR;
  Sorted = (cp) ? cp->IsSorted() > 0 : FALSE;
  Result = 0;
  } // end of BLOCKINDEX constructor

/***********************************************************************/
/*  Reset Bot and Top values of optimized Kindex blocks.               */
/***********************************************************************/
void BLOCKINDEX::Reset(void)
  {
  if (Next)
    Next->Reset();

  Kxp->Reset();
  } // end of Reset

/***********************************************************************/
/*  Evaluate block indexing test.                                      */
/***********************************************************************/
int BLOCKINDEX::BlockEval(PGLOBAL g)
  {
#if defined(_DEBUG)
  assert (Tdbp && Colp);
#endif
  int   n = Tdbp->GetCurBlk();
  void *minp = Colp->GetMin()->GetValPtr(n);
  void *maxp = Colp->GetMax()->GetValPtr(n);

  Result = Kxp->BlockTest(g, minp, maxp, Type, Sorted);
  return Result;
  } // end of BlockEval

/***********************************************************************/
/*  Make file output of BLOCKINDEX contents.                           */
/***********************************************************************/
void BLOCKINDEX::Printf(PGLOBAL g, FILE *f, UINT n)
  {
  char m[64];

  memset(m, ' ', n);                    // Make margin string
  m[n] = '\0';

  fprintf(f, "%sBLOCKINDEX: at %p next=%p col=%s kxp=%p result=%d\n",
    m, this, Next, (Colp) ? Colp->GetName() : "Rowid", Kxp, Result);

  if (Next)
    Next->Printf(g, f, n);

  } // end of Printf

/***********************************************************************/
/*  Make string output of BLOCKINDEX contents.                         */
/***********************************************************************/
void BLOCKINDEX::Prints(PGLOBAL g, char *ps, UINT z)
  {
  strncat(ps, "BlockIndex(es)", z);
  } // end of Prints

/* ------------------------------------------------------------------- */

/***********************************************************************/
/*  Implementation of the BLOCKINDX2 class.                            */
/***********************************************************************/
BLOCKINDX2::BLOCKINDX2(PBX nx, PDOSCOL cp, PKXBASE kp)
          : BLOCKINDEX(nx, cp, kp)
  {
  Nbm = Colp->GetNbm();
  Dval = Colp->GetDval();
  Bmap = Colp->GetBmap();
#if defined(_DEBUG)
  assert(Dval && Bmap);
#endif
  } // end of BLOCKINDX2 constructor

/***********************************************************************/
/*  Evaluate block indexing test.                                      */
/***********************************************************************/
int BLOCKINDX2::BlockEval(PGLOBAL g)
  {
  int   n = Tdbp->GetCurBlk();
  PUINT bmp = (PUINT)Bmap->GetValPtr(n * Nbm);

  Result = Kxp->BlockTst2(g, Dval, bmp, Nbm, Type, Sorted);
  return Result;
  } // end of BlockEval

/* ------------------------------------------------------------------- */

/***********************************************************************/
/*  Implementation of the BLKSPCINDX class.                            */
/***********************************************************************/
BLKSPCINDX::BLKSPCINDX(PBX nx, PTDBDOS tp, PKXBASE kp, int bsize)
          : BLOCKINDEX(nx, NULL, kp)
  {
  Tdbp = tp;
  Bsize = bsize;
  Type = TYPE_INT;
  Sorted = TRUE;
  } // end of BLKSPCINDX constructor

/***********************************************************************/
/*  Evaluate block indexing test.                                      */
/***********************************************************************/
int BLKSPCINDX::BlockEval(PGLOBAL g)
  {
  int  n = Tdbp->GetCurBlk();
  int minrow = n * Bsize + 1;   // Minimum Rowid value for this block
  int maxrow = (n + 1) * Bsize; // Maximum Rowid value for this block

  Result = Kxp->BlockTest(g, &minrow, &maxrow, TYPE_INT, TRUE);
  return Result;
  } // end of BlockEval
#endif // 0
