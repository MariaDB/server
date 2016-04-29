/***************** Xindex C++ Class Xindex Code (.CPP) *****************/
/*  Name: XINDEX.CPP  Version 2.9                                      */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2004-2015    */
/*                                                                     */
/*  This file contains the class XINDEX implementation code.           */
/***********************************************************************/

/***********************************************************************/
/*  Include relevant sections of the System header files.              */
/***********************************************************************/
#include "my_global.h"
#if defined(__WIN__)
#include <io.h>
#include <fcntl.h>
#include <errno.h>
//#include <windows.h>
#else   // !__WIN__
#if defined(UNIX)
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>
#else   // !UNIX
#include <io.h>
#endif  // !UNIX
#include <fcntl.h>
#endif  // !__WIN__

/***********************************************************************/
/*  Include required application header files                          */
/*  global.h    is header containing all global Plug declarations.     */
/*  plgdbsem.h  is header containing the DB applic. declarations.      */
/*  kindex.h    is header containing the KINDEX class definition.      */
/***********************************************************************/
#include "global.h"
#include "plgdbsem.h"
#include "osutil.h"
#include "maputil.h"
//nclude "filter.h"
#include "tabcol.h"
#include "xindex.h"
#include "xobject.h"
//nclude "scalfnc.h"
//nclude "array.h"
#include "filamtxt.h"
#include "tabdos.h"
#include "tabvct.h"

/***********************************************************************/
/*  Macro or external routine definition                               */
/***********************************************************************/
#define NZ 8
#define NW 5
#define MAX_INDX 10
#ifndef INVALID_SET_FILE_POINTER
#define INVALID_SET_FILE_POINTER  0xFFFFFFFF
#endif

/***********************************************************************/
/*  DB external variables.                                             */
/***********************************************************************/
extern MBLOCK Nmblk;                /* Used to initialize MBLOCK's     */
#if defined(XMAP)
extern my_bool xmap;
#endif   // XMAP

/***********************************************************************/
/*  Last two parameters are true to enable type checking, and last one */
/*  to have rows filled by blanks to be compatible with QRY blocks.    */
/***********************************************************************/
PVBLK AllocValBlock(PGLOBAL, void *, int, int, int, int,
                    bool check = true, bool blank = true, bool un = false);

/***********************************************************************/
/*  Check whether we have to create/update permanent indexes.          */
/***********************************************************************/
int PlgMakeIndex(PGLOBAL g, PSZ name, PIXDEF pxdf, bool add)
  {
  int     rc;
  PTABLE  tablep;
  PTDBASE tdbp;
  PCATLG  cat = PlgGetCatalog(g, true);

  /*********************************************************************/
  /*  Open a new table in mode read and with only the keys columns.    */
  /*********************************************************************/
  tablep = new(g) XTAB(name);

  if (!(tdbp = (PTDBASE)cat->GetTable(g, tablep)))
    rc = RC_NF;
  else if (!tdbp->GetDef()->Indexable()) {
    sprintf(g->Message, MSG(TABLE_NO_INDEX), name);
    rc = RC_NF;
  } else if ((rc = tdbp->MakeIndex(g, pxdf, add)) == RC_INFO)
    rc = RC_OK;            // No or remote index

  return rc;
  } // end of PlgMakeIndex

/* -------------------------- Class INDEXDEF ------------------------- */

/***********************************************************************/
/*  INDEXDEF Constructor.                                              */
/***********************************************************************/
INDEXDEF::INDEXDEF(char *name, bool uniq, int n)
  {
//To_Def = NULL;
  Next = NULL;
  ToKeyParts = NULL;
  Name = name;
  Unique = uniq;
  Invalid = false;
  AutoInc = false;
  Dynamic = false;
  Mapped = false;
  Nparts = 0;
  ID = n;
//Offset = 0;
//Offhigh = 0;
//Size = 0;
  MaxSame = 1;
  } // end of INDEXDEF constructor

/***********************************************************************/
/*  Set the max same values for each colum after making the index.     */
/***********************************************************************/
void INDEXDEF::SetMxsame(PXINDEX x)
  {
  PKPDEF  kdp;
  PXCOL   xcp;

  for (kdp = ToKeyParts, xcp = x->To_KeyCol;
       kdp && xcp; kdp = kdp->Next, xcp = xcp->Next)
    kdp->Mxsame = xcp->Mxs;
  } // end of SetMxsame

/* -------------------------- Class KPARTDEF ------------------------- */

/***********************************************************************/
/*  KPARTDEF Constructor.                                              */
/***********************************************************************/
KPARTDEF::KPARTDEF(PSZ name, int n)
  {
  Next = NULL;
  Name = name;
  Mxsame = 0;
  Ncol = n;
  Klen = 0;
  } // end of KPARTDEF constructor

/* -------------------------- XXBASE Class --------------------------- */

/***********************************************************************/
/*  XXBASE public constructor.                                         */
/***********************************************************************/
XXBASE::XXBASE(PTDBDOS tbxp, bool b) : CSORT(b),
        To_Rec((int*&)Record.Memp)
  {
  Tbxp = tbxp;
  Record = Nmblk;
  Cur_K = -1;
  Old_K = -1;
  Num_K = 0;
  Ndif = 0;
  Bot = Top = Inf = Sup = 0;
  Op = OP_EQ;
  To_KeyCol = NULL;
  Mul = false;
  Srtd = false;
  Dynamic = false;
  Val_K = -1;
  Nblk = Sblk = 0;
  Thresh = 7;
  ID = -1;
  Nth = 0;
  } // end of XXBASE constructor

/***********************************************************************/
/*  Make file output of XINDEX contents.                               */
/***********************************************************************/
void XXBASE::Print(PGLOBAL, FILE *f, uint n)
  {
  char m[64];

  memset(m, ' ', n);                    // Make margin string
  m[n] = '\0';
  fprintf(f, "%sXINDEX: Tbxp=%p Num=%d\n", m, Tbxp, Num_K);
  } // end of Print

/***********************************************************************/
/*  Make string output of XINDEX contents.                             */
/***********************************************************************/
void XXBASE::Print(PGLOBAL, char *ps, uint z)
  {
  *ps = '\0';
  strncat(ps, "Xindex", z);
  } // end of Print

/* -------------------------- XINDEX Class --------------------------- */

/***********************************************************************/
/*  XINDEX public constructor.                                         */
/***********************************************************************/
XINDEX::XINDEX(PTDBDOS tdbp, PIXDEF xdp, PXLOAD pxp, PCOL *cp, PXOB *xp, int k)
      : XXBASE(tdbp, !xdp->IsUnique())
  {
  Xdp = xdp;
  ID = xdp->GetID();
  Tdbp = tdbp;
  X = pxp;
  To_LastCol = NULL;
  To_LastVal = NULL;
  To_Cols = cp;
  To_Vals = xp;
  Mul = !xdp->IsUnique();
  Srtd = false;
  Nk = xdp->GetNparts();
  Nval = (k) ? k : Nk;
  Incr = 0;
//Defoff = xdp->GetOffset();
//Defhigh = xdp->GetOffhigh();
//Size = xdp->GetSize();
  MaxSame = xdp->GetMaxSame();
  } // end of XINDEX constructor

/***********************************************************************/
/*  XINDEX Reset: re-initialize a Xindex block.                        */
/***********************************************************************/
void XINDEX::Reset(void)
  {
  for (PXCOL kp = To_KeyCol; kp; kp = kp->Next)
    kp->Val_K = kp->Ndf;

  Cur_K = Num_K;
  Old_K = -1;  // Needed to avoid not setting CurBlk for Update
  Op = (Op == OP_FIRST  || Op == OP_NEXT)   ? OP_FIRST  :
       (Op == OP_FSTDIF || Op == OP_NXTDIF) ? OP_FSTDIF : OP_EQ;
  Nth = 0;
  } // end of Reset

/***********************************************************************/
/*  XINDEX Close: terminate index and free all allocated data.         */
/*  Do not reset values that are used at return to make.               */
/***********************************************************************/
void XINDEX::Close(void)
  {
  // Close file or view of file
  if (X)
    X->Close();

  // De-allocate data
  PlgDBfree(Record);
  PlgDBfree(Index);
  PlgDBfree(Offset);

  for (PXCOL kcp = To_KeyCol; kcp; kcp = kcp->Next) {
    // Column values cannot be retrieved from key anymore
    if (kcp->Colp)
      kcp->Colp->SetKcol(NULL);

    // De-allocate Key data
    kcp->FreeData();
    } // endfor kcp

  } // end of Close

/***********************************************************************/
/*  XINDEX compare routine for C Quick/Insertion sort.                 */
/***********************************************************************/
int XINDEX::Qcompare(int *i1, int *i2)
  {
  register int  k;
  register PXCOL kcp;

  for (kcp = To_KeyCol, k = 0; kcp; kcp = kcp->Next)
    if ((k = kcp->Compare(*i1, *i2)))
      break;

//num_comp++;
  return k;
  } // end of Qcompare

/***********************************************************************/
/*  AddColumns: here we try to determine whether it is worthwhile to   */
/*  add to the keys the values of the columns selected for this table. */
/*  Sure enough, it is done while records are read and permit to avoid */
/*  reading the table while doing the join (Dynamic index only)        */
/***********************************************************************/
bool XINDEX::AddColumns(void)
  {
  if (!Dynamic)
    return false;     // Not applying to static index
  else if (IsMul())
    return false;     // Not done yet for multiple index
  else if (Tbxp->GetAmType() == TYPE_AM_VCT && ((PTDBVCT)Tbxp)->IsSplit())
    return false;     // This would require to read additional files
  else
    return true;

  } // end of AddColumns

/***********************************************************************/
/*  Make: Make and index on key column(s).                             */
/***********************************************************************/
bool XINDEX::Make(PGLOBAL g, PIXDEF sxp)
  {
  /*********************************************************************/
  /*  Table can be accessed through an index.                          */
  /*********************************************************************/
  int     k, nk = Nk, rc = RC_OK;
  int    *bof, i, j, n, ndf, nkey;
  PKPDEF  kdfp = Xdp->GetToKeyParts();
  bool    brc = false;
  PCOL    colp;
  PFIL    filp = Tdbp->GetFilter();
  PXCOL   kp, addcolp, prev = NULL, kcp = NULL;
//PDBUSER dup = (PDBUSER)g->Activityp->Aptr;

#if defined(_DEBUG)
  assert(X || Nk == 1);
#endif   // _DEBUG

  /*********************************************************************/
  /*  Allocate the storage that will contain the keys and the file     */
  /*  positions corresponding to them.                                 */
  /*********************************************************************/
  if ((n = Tdbp->GetMaxSize(g)) < 0)
    return true;
  else if (!n) {
    Num_K = Ndif = 0;
    MaxSame = 1;

    // The if condition was suppressed because this may be an existing
    // index that is now void because all table lines were deleted.
//  if (sxp)
      goto nox;            // Truncate eventually existing index file
//  else
//    return false;

    } // endif n

  if (trace)
    htrc("XINDEX Make: n=%d\n", n);

  // File position must be stored
  Record.Size = n * sizeof(int);

  if (!PlgDBalloc(g, NULL, Record)) {
    sprintf(g->Message, MSG(MEM_ALLOC_ERR), "index", n);
    goto err;    // Error
    } // endif

  /*********************************************************************/
  /*  Allocate the KXYCOL blocks used to store column values.          */
  /*********************************************************************/
  for (k = 0; k < Nk; k++) {
    colp = To_Cols[k];

    if (!kdfp) {
      sprintf(g->Message, MSG(INT_COL_ERROR),
                          (colp) ? colp->GetName() : "???");
      goto err;    // Error
      } // endif kdfp

    kcp = new(g) KXYCOL(this);

    if (kcp->Init(g, colp, n, true, kdfp->Klen))
      goto err;    // Error

    if (prev) {
      kcp->Previous = prev;
      prev->Next = kcp;
    } else
      To_KeyCol = kcp;

    prev = kcp;
    kdfp = kdfp->Next;
    } // endfor k

  To_LastCol = prev;

  if (AddColumns()) {
    PCOL kolp = To_Cols[0];    // Temporary while imposing Nk = 1

    i = 0;

    // Allocate the accompanying
    for (colp = Tbxp->GetColumns(); colp; colp = colp->GetNext()) {
      // Count how many columns to add
//    for (k = 0; k < Nk; k++)
//      if (colp == To_Cols[k])
//        break;

//    if (k == nk)
      if (colp != kolp)
        i++;

      } // endfor colp

    if (i && i < 10)                  // Should be a parameter
      for (colp = Tbxp->GetColumns(); colp; colp = colp->GetNext()) {
//      for (k = 0; k < Nk; k++)
//        if (colp == To_Cols[k])
//          break;

//      if (k < nk)
        if (colp == kolp)
          continue;                   // This is a key column

        kcp = new(g) KXYCOL(this);

        if (kcp->Init(g, colp, n, true, 0))
          return true;

        if (trace)
          htrc("Adding colp=%p Buf_Type=%d size=%d\n",
                colp, colp->GetResultType(), n);

        nk++;
        prev->Next = kcp;
        prev = kcp;
        } // endfor colp

    } // endif AddColumns

#if 0
  /*********************************************************************/
  /*  Get the starting information for progress.                       */
  /*********************************************************************/
  dup->Step = (char*)PlugSubAlloc(g, NULL, 128);
  sprintf((char*)dup->Step, MSG(BUILD_INDEX), Xdp->GetName(), Tdbp->Name);
  dup->ProgMax = Tdbp->GetProgMax(g);
  dup->ProgCur = 0;
#endif // 0

  /*********************************************************************/
  /*  Standard init: read the file and construct the index table.      */
  /*  Note: reading will be sequential as To_Kindex is not set.        */
  /*********************************************************************/
  for (i = nkey = 0; rc != RC_EF; i++) {
#if 0
    if (!dup->Step) {
      strcpy(g->Message, MSG(QUERY_CANCELLED));
      longjmp(g->jumper[g->jump_level], 99);
      } // endif Step
#endif // 0

    /*******************************************************************/
    /*  Read a valid record from table file.                           */
    /*******************************************************************/
    rc = Tdbp->ReadDB(g);

    // Update progress information
//  dup->ProgCur = Tdbp->GetProgCur();

    // Check return code and do whatever must be done according to it
    switch (rc) {
      case RC_OK:
        if (ApplyFilter(g, filp))
          break;

        // passthru
      case RC_NF:
        continue;
      case RC_EF:
        goto end_of_file;
      default:
        sprintf(g->Message, MSG(RC_READING), rc, Tdbp->Name);
        goto err;
      } // endswitch rc

    /*******************************************************************/
    /*  Get and Store the file position of the last read record for    */
    /*  future direct access.                                          */
    /*******************************************************************/
    if (nkey == n) {
      sprintf(g->Message, MSG(TOO_MANY_KEYS), nkey);
      return true;
    } else
      To_Rec[nkey] = Tdbp->GetRecpos();

    if (trace > 1)
      htrc("Make: To_Rec[%d]=%d\n", nkey, To_Rec[nkey]); 

    /*******************************************************************/
    /*  Get the keys and place them in the key blocks.                 */
    /*******************************************************************/
    for (k = 0, kcp = To_KeyCol;
         k < nk && kcp;
         k++, kcp = kcp->Next) {
//    colp = To_Cols[k];
      colp = kcp->Colp;

      if (!colp->GetStatus(BUF_READ))
        colp->ReadColumn(g);
      else
        colp->Reset();

      kcp->SetValue(colp, nkey);
      } // endfor k

    nkey++;                    // A new valid key was found
    } // endfor i

 end_of_file:

  // Update progress information
//dup->ProgCur = Tdbp->GetProgMax(g);

  /*********************************************************************/
  /* Record the Index size and eventually resize memory allocation.    */
  /*********************************************************************/
  if ((Num_K = nkey) < n) {
    PlgDBrealloc(g, NULL, Record, Num_K * sizeof(int));

    for (kcp = To_KeyCol; kcp; kcp = kcp->Next)
      kcp->ReAlloc(g, Num_K);

    } // endif Num_K

  /*********************************************************************/
  /*  Sort the index so we can use an optimized Find algorithm.        */
  /*  Note: for a unique index we use the non conservative sort        */
  /*  version because normally all index values are different.         */
  /*  This was set at CSORT class construction.                        */
  /*  For all indexes, an offset array is made so we can check the     */
  /*  uniqueness of unique indexes.                                    */
  /*********************************************************************/
  Index.Size = Num_K * sizeof(int);

  if (!PlgDBalloc(g, NULL, Index)) {
    sprintf(g->Message, MSG(MEM_ALLOC_ERR), "index", Num_K);
    goto err;    // Error
    } // endif alloc

  Offset.Size = (Num_K + 1) * sizeof(int);

  if (!PlgDBalloc(g, NULL, Offset)) {
    sprintf(g->Message, MSG(MEM_ALLOC_ERR), "offset", Num_K + 1);
    goto err;    // Error
    } // endif alloc

  // We must separate keys and added columns before sorting
  addcolp = To_LastCol->Next;
  To_LastCol->Next = NULL;

  // Call the sort program, it returns the number of distinct values
  if ((Ndif = Qsort(g, Num_K)) < 0)
    goto err;       // Error during sort

  if (trace)
    htrc("Make: Nk=%d n=%d Num_K=%d Ndif=%d addcolp=%p BlkFil=%p X=%p\n",
          Nk, n, Num_K, Ndif, addcolp, Tdbp->To_BlkFil, X);

  // Check whether the unique index is unique indeed
  if (!Mul)
    if (Ndif < Num_K) {
      strcpy(g->Message, MSG(INDEX_NOT_UNIQ));
      brc = true;
      goto err;
    } else
      PlgDBfree(Offset);           // Not used anymore

  // Restore kcp list
  To_LastCol->Next = addcolp;

  // Use the index to physically reorder the xindex
  Srtd = Reorder(g);

  if (Ndif < Num_K) {
    // Resize the offset array
    PlgDBrealloc(g, NULL, Offset, (Ndif + 1) * sizeof(int));

    // Initial value of MaxSame
    MaxSame = Pof[1] - Pof[0];

    // Resize the Key array by only keeping the distinct values
    for (i = 1; i < Ndif; i++) {
      for (kcp = To_KeyCol; kcp; kcp = kcp->Next)
        kcp->Move(i, Pof[i]);

      MaxSame = MY_MAX(MaxSame, Pof[i + 1] - Pof[i]);
      } // endfor i

    for (kcp = To_KeyCol; kcp; kcp = kcp->Next)
      kcp->ReAlloc(g, Ndif);

  } else {
    Mul = false;                   // Current index is unique
    PlgDBfree(Offset);             // Not used anymore
    MaxSame = 1;                   // Reset it when remaking an index
  } // endif Ndif

  /*********************************************************************/
  /*  Now do the reduction of the index. Indeed a multi-column index   */
  /*  can be used for only some of the first columns. For instance if  */
  /*  an index is defined for column A, B, C PlugDB can use it for     */
  /*  only the column A or the columns A, B.                           */
  /*  What we do here is to reduce the data so column A will contain   */
  /*  only the sorted distinct values of A, B will contain data such   */
  /*  as only distinct values of A,B are stored etc.                   */
  /*  This implies that for each column set an offset array is made    */
  /*  except if the subset originally contains unique values.          */
  /*********************************************************************/
  // Update progress information
//dup->Step = STEP(REDUCE_INDEX);

  ndf = Ndif;
  To_LastCol->Mxs = MaxSame;

  for (kcp = To_LastCol->Previous; kcp; kcp = kcp->Previous) {
    if (!(bof = kcp->MakeOffset(g, ndf)))
      goto err;
    else
      *bof = 0;

    for (n = 0, i = j = 1; i < ndf; i++)
      for (kp = kcp; kp; kp = kp->Previous)
        if (kp->Compare(n, i)) {
          // Values are not equal to last ones
          bof[j++] = n = i;
          break;
          } // endif Compare

    if (j < ndf) {
      // Sub-index is multiple
      bof[j] = ndf;
      ndf = j;                  // New number of distinct values

      // Resize the Key array by only keeping the distinct values
      for (kp = kcp; kp; kp = kp->Previous) {
        for (i = 1; i < ndf; i++)
          kp->Move(i, bof[i]);

        kp->ReAlloc(g, ndf);
        } // endif kcp

      // Resize the offset array
      kcp->MakeOffset(g, ndf);

      // Calculate the max same value for this column
      kcp->Mxs = ColMaxSame(kcp);
    } else {
      // Current sub-index is unique
      kcp->MakeOffset(g, 0);   // The offset is not used anymore
      kcp->Mxs = 1;            // Unique
    } // endif j

    } // endfor kcp

  /*********************************************************************/
  /*  For sorted columns and fixed record size, file position can be   */
  /*  calculated, so the Record array can be discarted.                */
  /*  Not true for DBF tables because of eventual soft deleted lines.  */
  /*  Note: for Num_K = 1 any non null value is Ok.                    */
  /*********************************************************************/
  if (Srtd && !filp && Tdbp->Ftype != RECFM_VAR 
                    && Tdbp->Txfp->GetAmType() != TYPE_AM_DBF) {
    Incr = (Num_K > 1) ? To_Rec[1] : Num_K;
    PlgDBfree(Record);
    } // endif Srtd

  /*********************************************************************/
  /*  Check whether a two-tier find algorithm can be implemented.      */
  /*  It is currently implemented only for single key indexes.         */
  /*********************************************************************/
  if (Nk == 1 && ndf >= 65536) {
    // Implement a two-tier find algorithm
    for (Sblk = 256; (Sblk * Sblk * 4) < ndf; Sblk *= 2) ;

    Nblk = (ndf -1) / Sblk + 1;

    if (To_KeyCol->MakeBlockArray(g, Nblk, Sblk))
      goto err;    // Error

    } // endif Num_K

 nox:
  /*********************************************************************/
  /*  No valid record read yet for secondary file.                     */
  /*********************************************************************/
  Cur_K = Num_K;

  /*********************************************************************/
  /*  Save the xindex so it has not to be recalculated.                */
  /*********************************************************************/
  if (X) {
    if (SaveIndex(g, sxp))
      brc = true;

  } else {                     // Dynamic index
    // Indicate that key column values can be found from KEYCOL's
    for (kcp = To_KeyCol; kcp; kcp = kcp->Next)
      kcp->Colp->SetKcol(kcp);

    Tdbp->SetFilter(NULL);     // Not used anymore
  } // endif X

 err:
  // We don't need the index anymore
  if (X || brc)
    Close();

  if (brc)
    printf("%s\n", g->Message);

  return brc;
  } // end of Make

/***********************************************************************/
/*  Return the max size of the intermediate column.                    */
/***********************************************************************/
int XINDEX::ColMaxSame(PXCOL kp)
  {
  int *kof, i, ck1, ck2, ckn = 1;
  PXCOL kcp;

  // Calculate the max same value for this column
  for (i = 0; i < kp->Ndf; i++) {
    ck1 = i;
    ck2 = i + 1;

    for (kcp = kp; kcp; kcp = kcp->Next) {
      if (!(kof = (kcp->Next) ? kcp->Kof : Pof))
        break;

      ck1 = kof[ck1];
      ck2 = kof[ck2];
      } // endfor kcp

    ckn = MY_MAX(ckn, ck2 - ck1);
    } // endfor i

  return ckn;
  } // end of ColMaxSame

/***********************************************************************/
/*  Reorder: use the sort index to reorder the data in storage so      */
/*  it will be physically sorted and sort index can be removed.        */
/***********************************************************************/
bool XINDEX::Reorder(PGLOBAL g __attribute__((unused)))
  {
  register int i, j, k, n;
  bool          sorted = true;
  PXCOL         kcp;
#if 0
  PDBUSER       dup = (PDBUSER)g->Activityp->Aptr;

  if (Num_K > 500000) {
    // Update progress information
    dup->Step = STEP(REORDER_INDEX);
    dup->ProgMax = Num_K;
    dup->ProgCur = 0;
  } else
    dup = NULL;
#endif // 0

  if (!Pex)
    return Srtd;

  for (i = 0; i < Num_K; i++) {
    if (Pex[i] == Num_K) {        // Already moved
      continue;
    } else if (Pex[i] == i) {     // Already placed
//    if (dup)
//      dup->ProgCur++;

      continue;
    } // endif's Pex

    sorted = false;

    for (kcp = To_KeyCol; kcp; kcp = kcp->Next)
      kcp->Save(i);

    n = To_Rec[i];

    for (j = i;; j = k) {
      k = Pex[j];
      Pex[j] = Num_K;           // Mark position as set

      if (k == i) {
        for (kcp = To_KeyCol; kcp; kcp = kcp->Next)
          kcp->Restore(j);

        To_Rec[j] = n;
        break;                  // end of loop
      } else {
        for (kcp = To_KeyCol; kcp; kcp = kcp->Next)
          kcp->Move(j, k);      // Move k to j

        To_Rec[j] = To_Rec[k];
      } // endif k

//    if (dup)
//      dup->ProgCur++;

      } // endfor j

    } // endfor i

  // The index is not used anymore
  PlgDBfree(Index);
  return sorted;
  } // end of Reorder

/***********************************************************************/
/*  Save the index values for this table.                              */
/*  The problem here is to avoid name duplication, because more than   */
/*  one data file can have the same name (but different types) and/or  */
/*  the same data file can be used with different block sizes. This is */
/*  why we use Ofn that defaults to the file name but can be set to a  */
/*  different name if necessary.                                       */
/***********************************************************************/
bool XINDEX::SaveIndex(PGLOBAL g, PIXDEF sxp)
  {
  char   *ftype;
  char    fn[_MAX_PATH];
  int     n[NZ], nof = (Mul) ? (Ndif + 1) : 0;
  int     id = -1, size = 0;
  bool    sep, rc = false;
  PXCOL   kcp = To_KeyCol;
  PDOSDEF defp = (PDOSDEF)Tdbp->To_Def;
//PDBUSER dup = PlgGetUser(g);

//dup->Step = STEP(SAVING_INDEX);
//dup->ProgMax = 15 + 16 * Nk;
//dup->ProgCur = 0;

  switch (Tdbp->Ftype) {
    case RECFM_VAR: ftype = ".dnx"; break;
    case RECFM_FIX: ftype = ".fnx"; break;
    case RECFM_BIN: ftype = ".bnx"; break;
    case RECFM_VCT: ftype = ".vnx"; break;
    case RECFM_DBF: ftype = ".dbx"; break;
    default:
      sprintf(g->Message, MSG(INVALID_FTYPE), Tdbp->Ftype);
      return true;
    } // endswitch Ftype

  if ((sep = defp->GetBoolCatInfo("SepIndex", false))) {
    // Index is saved in a separate file
#if defined(__WIN__)
    char drive[_MAX_DRIVE];
#else
    char *drive = NULL;
#endif
    char direc[_MAX_DIR];
    char fname[_MAX_FNAME];

    _splitpath(defp->GetOfn(), drive, direc, fname, NULL);
    strcat(strcat(fname, "_"), Xdp->GetName());
    _makepath(fn, drive, direc, fname, ftype);
    sxp = NULL;
  } else {
    id = ID;
    strcat(PlugRemoveType(fn, strcpy(fn, defp->GetOfn())), ftype);
  } // endif sep

  PlugSetPath(fn, fn, Tdbp->GetPath());

  if (X->Open(g, fn, id, (sxp) ? MODE_INSERT : MODE_WRITE)) {
    printf("%s\n", g->Message);
    return true;
    } // endif Open

  if (!Ndif)
    goto end;                // Void index

  /*********************************************************************/
  /*  Write the index values on the index file.                        */
  /*********************************************************************/
  n[0] = ID + MAX_INDX;       // To check validity
  n[1] = Nk;                  // The number of indexed columns
  n[2] = nof;                 // The offset array size or 0
  n[3] = Num_K;               // The index size
  n[4] = Incr;                // Increment of record positions
  n[5] = Nblk; n[6] = Sblk;
  n[7] = Srtd ? 1 : 0;        // Values are sorted in the file

  if (trace) {
    htrc("Saving index %s\n", Xdp->GetName());
    htrc("ID=%d Nk=%d nof=%d Num_K=%d Incr=%d Nblk=%d Sblk=%d Srtd=%d\n",
          ID, Nk, nof, Num_K, Incr, Nblk, Sblk, Srtd);
    } // endif trace

  size = X->Write(g, n, NZ, sizeof(int), rc);
//dup->ProgCur = 1;

  if (Mul)             // Write the offset array
    size += X->Write(g, Pof, nof, sizeof(int), rc);

//dup->ProgCur = 5;

  if (!Incr)           // Write the record position array(s)
    size += X->Write(g, To_Rec, Num_K, sizeof(int), rc);

//dup->ProgCur = 15;

  for (; kcp; kcp = kcp->Next) {
    n[0] = kcp->Ndf;                 // Number of distinct sub-values
    n[1] = (kcp->Kof) ? kcp->Ndf + 1 : 0;     // 0 if unique
    n[2] = (kcp == To_KeyCol) ? Nblk : 0;
    n[3] = kcp->Klen;                // To be checked later
    n[4] = kcp->Type;                // To be checked later

    size += X->Write(g, n, NW, sizeof(int), rc);
//  dup->ProgCur += 1;

    if (n[2])
      size += X->Write(g, kcp->To_Bkeys, Nblk, kcp->Klen, rc);

//  dup->ProgCur += 5;

    size += X->Write(g, kcp->To_Keys, n[0], kcp->Klen, rc);
//  dup->ProgCur += 5;

    if (n[1])
      size += X->Write(g, kcp->Kof, n[1], sizeof(int), rc);

//  dup->ProgCur += 5;
    } // endfor kcp

  if (trace)
    htrc("Index %s saved, Size=%d\n", Xdp->GetName(), size);

 end:
  X->Close(fn, id);
  return rc;
  } // end of SaveIndex

/***********************************************************************/
/*  Init: Open and Initialize a Key Index.                             */
/***********************************************************************/
bool XINDEX::Init(PGLOBAL g)
  {
#if defined(XMAP)
  if (xmap)
    return MapInit(g);
#endif   // XMAP

  /*********************************************************************/
  /*  Table will be accessed through an index table.                   */
  /*  If sorting is required, this will be done later.                 */
  /*********************************************************************/
  char   *ftype;
  char    fn[_MAX_PATH];
  int     k, n, nv[NZ], id = -1;
  bool    estim = false;
  PCOL    colp;
  PXCOL   prev = NULL, kcp = NULL;
  PDOSDEF defp = (PDOSDEF)Tdbp->To_Def;

  /*********************************************************************/
  /*  Get the estimated table size.                                    */
  /*  Note: for fixed tables we must use cardinality to avoid the call */
  /*  to MaxBlkSize that could reduce the cardinality value.           */
  /*********************************************************************/
  if (Tdbp->Cardinality(NULL)) {
    // For DBF tables, Cardinality includes bad or soft deleted lines
    // that are not included in the index, and can be larger then the
    // index size.
    estim = (Tdbp->Ftype == RECFM_DBF);
    n = Tdbp->Cardinality(g);      // n is exact table size
  } else {
    // Variable table not optimized
    estim = true;                  // n is an estimate of the size
    n = Tdbp->GetMaxSize(g);
  } // endif Cardinality

  if (n <= 0)
    return !(n == 0);             // n < 0 error, n = 0 void table

  /*********************************************************************/
  /*  Get the first key column.                                        */
  /*********************************************************************/
  if (!Nk || !To_Cols || (!To_Vals && Op != OP_FIRST && Op != OP_FSTDIF)) {
    strcpy(g->Message, MSG(NO_KEY_COL));
    return true;    // Error
  } else
    colp = To_Cols[0];

  switch (Tdbp->Ftype) {
    case RECFM_VAR: ftype = ".dnx"; break;
    case RECFM_FIX: ftype = ".fnx"; break;
    case RECFM_BIN: ftype = ".bnx"; break;
    case RECFM_VCT: ftype = ".vnx"; break;
    case RECFM_DBF: ftype = ".dbx"; break;
    default:
      sprintf(g->Message, MSG(INVALID_FTYPE), Tdbp->Ftype);
      return true;
    } // endswitch Ftype

  if (defp->SepIndex()) {
    // Index was saved in a separate file
#if defined(__WIN__)
    char drive[_MAX_DRIVE];
#else
    char *drive = NULL;
#endif
    char direc[_MAX_DIR];
    char fname[_MAX_FNAME];

    _splitpath(defp->GetOfn(), drive, direc, fname, NULL);
    strcat(strcat(fname, "_"), Xdp->GetName());
    _makepath(fn, drive, direc, fname, ftype);
  } else {
    id = ID;
    strcat(PlugRemoveType(fn, strcpy(fn, defp->GetOfn())), ftype);
  } // endif sep

  PlugSetPath(fn, fn, Tdbp->GetPath());

  if (trace)
    htrc("Index %s file: %s\n", Xdp->GetName(), fn);

  /*********************************************************************/
  /*  Open the index file and check its validity.                      */
  /*********************************************************************/
  if (X->Open(g, fn, id, MODE_READ))
    goto err;               // No saved values

  //  Now start the reading process.
  if (X->Read(g, nv, NZ - 1, sizeof(int)))
    goto err;

  if (nv[0] >= MAX_INDX) {
    // New index format
    if (X->Read(g, nv + 7, 1, sizeof(int)))
      goto err;

    Srtd = nv[7] != 0;
    nv[0] -= MAX_INDX;
  } else
    Srtd = false;

  if (trace)
    htrc("nv=%d %d %d %d %d %d %d (%d)\n",
          nv[0], nv[1], nv[2], nv[3], nv[4], nv[5], nv[6], Srtd);

  // The test on ID was suppressed because MariaDB can change an index ID
  // when other indexes are added or deleted
  if (/*nv[0] != ID ||*/ nv[1] != Nk) {
    sprintf(g->Message, MSG(BAD_INDEX_FILE), fn);

    if (trace)
      htrc("nv[0]=%d ID=%d nv[1]=%d Nk=%d\n", nv[0], ID, nv[1], Nk);

    goto err;
    } // endif

  if (nv[2]) {
    Mul = true;
    Ndif = nv[2];

    // Allocate the storage that will contain the offset array
    Offset.Size = Ndif * sizeof(int);

    if (!PlgDBalloc(g, NULL, Offset)) {
      sprintf(g->Message, MSG(MEM_ALLOC_ERR), "offset", Ndif);
      goto err;
      } // endif

    if (X->Read(g, Pof, Ndif, sizeof(int)))
      goto err;

    Ndif--;   // nv[2] is offset size, equal to Ndif + 1
  } else {
    Mul = false;
    Ndif = nv[3];
  } // endif nv[2]

  if (nv[3] < n && estim)
    n = nv[3];              // n was just an evaluated max value

  if (nv[3] != n) {
    sprintf(g->Message, MSG(OPT_NOT_MATCH), fn);
    goto err;
    } // endif

  Num_K = nv[3];
  Incr = nv[4];
  Nblk = nv[5];
  Sblk = nv[6];

  if (!Incr) {
    /*******************************************************************/
    /*  Allocate the storage that will contain the file positions.     */
    /*******************************************************************/
    Record.Size = Num_K * sizeof(int);

    if (!PlgDBalloc(g, NULL, Record)) {
      sprintf(g->Message, MSG(MEM_ALLOC_ERR), "index", Num_K);
      goto err;
      } // endif

    if (X->Read(g, To_Rec, Num_K, sizeof(int)))
      goto err;

  } else
    Srtd = true;    // Sorted positions can be calculated

  /*********************************************************************/
  /*  Allocate the KXYCOL blocks used to store column values.          */
  /*********************************************************************/
  for (k = 0; k < Nk; k++) {
    if (k == Nval)
      To_LastVal = prev;

    if (X->Read(g, nv, NW, sizeof(int)))
      goto err;

    colp = To_Cols[k];

    if (nv[4] != colp->GetResultType() || !colp->GetValue() ||
       (nv[3] != colp->GetValue()->GetClen() && nv[4] != TYPE_STRING)) {
      sprintf(g->Message, MSG(XCOL_MISMATCH), colp->GetName());
      goto err;    // Error
      } // endif GetKey

    kcp = new(g) KXYCOL(this);

    if (kcp->Init(g, colp, nv[0], true, (int)nv[3]))
      goto err;    // Error

    /*******************************************************************/
    /*  Read the index values from the index file.                     */
    /*******************************************************************/
    if (k == 0 && Nblk) {
      if (kcp->MakeBlockArray(g, Nblk, 0))
        goto err;

      // Read block values
      if (X->Read(g, kcp->To_Bkeys, Nblk, kcp->Klen))
        goto err;

      } // endif Nblk

    // Read the entire (small) index
    if (X->Read(g, kcp->To_Keys, nv[0], kcp->Klen))
      goto err;

    if (nv[1]) {
      if (!kcp->MakeOffset(g, nv[1] - 1))
        goto err;

      // Read the offset array
      if (X->Read(g, kcp->Kof, nv[1], sizeof(int)))
        goto err;

      } // endif n[1]

    if (!kcp->Prefix)
      // Indicate that the key column value can be found from KXYCOL
      colp->SetKcol(kcp);

    if (prev) {
      kcp->Previous = prev;
      prev->Next = kcp;
    } else
      To_KeyCol = kcp;

    prev = kcp;
    } // endfor k

  To_LastCol = prev;

  if (Mul && prev) {
    // Last key offset is the index offset
    kcp->Koff = Offset;
    kcp->Koff.Sub = true;
    } // endif Mul

  X->Close();

  /*********************************************************************/
  /*  No valid record read yet for secondary file.                     */
  /*********************************************************************/
  Cur_K = Num_K;
  return false;

err:
  Close();
  return true;
  } // end of Init

#if defined(XMAP)
/***********************************************************************/
/*  Init: Open and Initialize a Key Index.                             */
/***********************************************************************/
bool XINDEX::MapInit(PGLOBAL g)
  {
  /*********************************************************************/
  /*  Table will be accessed through an index table.                   */
  /*  If sorting is required, this will be done later.                 */
  /*********************************************************************/
  const char *ftype;
  BYTE   *mbase;
  char    fn[_MAX_PATH];
  int    *nv, k, n, id = -1;
  bool    estim;
  PCOL    colp;
  PXCOL   prev = NULL, kcp = NULL;
  PDOSDEF defp = (PDOSDEF)Tdbp->To_Def;
  PDBUSER dup = PlgGetUser(g);

  /*********************************************************************/
  /*  Get the estimated table size.                                    */
  /*  Note: for fixed tables we must use cardinality to avoid the call */
  /*  to MaxBlkSize that could reduce the cardinality value.           */
  /*********************************************************************/
  if (Tdbp->Cardinality(NULL)) {
    // For DBF tables, Cardinality includes bad or soft deleted lines
    // that are not included in the index, and can be larger then the
    // index size.
    estim = (Tdbp->Ftype == RECFM_DBF);
    n = Tdbp->Cardinality(g);      // n is exact table size
  } else {
    // Variable table not optimized
    estim = true;                  // n is an estimate of the size
    n = Tdbp->GetMaxSize(g);
  } // endif Cardinality

  if (n <= 0)
    return !(n == 0);             // n < 0 error, n = 0 void table

  /*********************************************************************/
  /*  Get the first key column.                                        */
  /*********************************************************************/
  if (!Nk || !To_Cols || (!To_Vals && Op != OP_FIRST && Op != OP_FSTDIF)) {
    strcpy(g->Message, MSG(NO_KEY_COL));
    return true;    // Error
  } else
    colp = To_Cols[0];

  switch (Tdbp->Ftype) {
    case RECFM_VAR: ftype = ".dnx"; break;
    case RECFM_FIX: ftype = ".fnx"; break;
    case RECFM_BIN: ftype = ".bnx"; break;
    case RECFM_VCT: ftype = ".vnx"; break;
    case RECFM_DBF: ftype = ".dbx"; break;
    default:
      sprintf(g->Message, MSG(INVALID_FTYPE), Tdbp->Ftype);
      return true;
    } // endswitch Ftype

  if (defp->SepIndex()) {
    // Index was save in a separate file
#if defined(__WIN__)
    char drive[_MAX_DRIVE];
#else
    char *drive = NULL;
#endif
    char direc[_MAX_DIR];
    char fname[_MAX_FNAME];

    _splitpath(defp->GetOfn(), drive, direc, fname, NULL);
    strcat(strcat(fname, "_"), Xdp->GetName());
    _makepath(fn, drive, direc, fname, ftype);
  } else {
    id = ID;
    strcat(PlugRemoveType(fn, strcpy(fn, defp->GetOfn())), ftype);
  } // endif SepIndex

  PlugSetPath(fn, fn, Tdbp->GetPath());

  if (trace)
    htrc("Index %s file: %s\n", Xdp->GetName(), fn);

  /*********************************************************************/
  /*  Get a view on the part of the index file containing this index.  */
  /*********************************************************************/
  if (!(mbase = (BYTE*)X->FileView(g, fn)))
    goto err;

  if (id >= 0) {
    // Get offset from the header
    IOFF *noff = (IOFF*)mbase;

    // Position the memory base at the offset of this index
    mbase += noff[id].v.Low;
    } // endif id

  //  Now start the mapping process.
  nv = (int*)mbase;

  if (nv[0] >= MAX_INDX) {
    // New index format
    Srtd = nv[7] != 0;
    nv[0] -= MAX_INDX;
    mbase += NZ * sizeof(int);
  } else {
    Srtd = false;
    mbase += (NZ - 1) * sizeof(int);
  } // endif nv

  if (trace)
    htrc("nv=%d %d %d %d %d %d %d %d\n",
          nv[0], nv[1], nv[2], nv[3], nv[4], nv[5], nv[6], Srtd);

  // The test on ID was suppressed because MariaDB can change an index ID
  // when other indexes are added or deleted
  if (/*nv[0] != ID ||*/ nv[1] != Nk) {
    // Not this index
    sprintf(g->Message, MSG(BAD_INDEX_FILE), fn);

    if (trace)
      htrc("nv[0]=%d ID=%d nv[1]=%d Nk=%d\n", nv[0], ID, nv[1], Nk);

    goto err;
    } // endif nv

  if (nv[2]) {
    // Set the offset array memory block
    Offset.Memp = mbase;
    Offset.Size = nv[2] * sizeof(int);
    Offset.Sub = true;
    Mul = true;
    Ndif = nv[2] - 1;
    mbase += Offset.Size;
  } else {
    Mul = false;
    Ndif = nv[3];
  } // endif nv[2]

  if (nv[3] < n && estim)
    n = nv[3];              // n was just an evaluated max value

  if (nv[3] != n) {
    sprintf(g->Message, MSG(OPT_NOT_MATCH), fn);
    goto err;
    } // endif

  Num_K = nv[3];
  Incr = nv[4];
  Nblk = nv[5];
  Sblk = nv[6];

  if (!Incr) {
    /*******************************************************************/
    /*  Point to the storage that contains the file positions.         */
    /*******************************************************************/
    Record.Size = Num_K * sizeof(int);
    Record.Memp = mbase;
    Record.Sub = true;
    mbase += Record.Size;
  } else
    Srtd = true;    // Sorted positions can be calculated

  /*********************************************************************/
  /*  Allocate the KXYCOL blocks used to store column values.          */
  /*********************************************************************/
  for (k = 0; k < Nk; k++) {
    if (k == Nval)
      To_LastVal = prev;

    nv = (int*)mbase;
    mbase += (NW * sizeof(int));

    colp = To_Cols[k];

    if (nv[4] != colp->GetResultType() || !colp->GetValue() ||
       (nv[3] != colp->GetValue()->GetClen() && nv[4] != TYPE_STRING)) {
      sprintf(g->Message, MSG(XCOL_MISMATCH), colp->GetName());
      goto err;    // Error
      } // endif GetKey

    kcp = new(g) KXYCOL(this);

    if (!(mbase = kcp->MapInit(g, colp, nv, mbase)))
      goto err;

    if (!kcp->Prefix)
      // Indicate that the key column value can be found from KXYCOL
      colp->SetKcol(kcp);

    if (prev) {
      kcp->Previous = prev;
      prev->Next = kcp;
    } else
      To_KeyCol = kcp;

    prev = kcp;
    } // endfor k

  To_LastCol = prev;

  if (Mul && prev)
    // Last key offset is the index offset
    kcp->Koff = Offset;

  /*********************************************************************/
  /*  No valid record read yet for secondary file.                     */
  /*********************************************************************/
  Cur_K = Num_K;
  return false;

err:
  Close();
  return true;
  } // end of MapInit
#endif   // XMAP

/***********************************************************************/
/*  Get Ndif and Num_K from the index file.                            */
/***********************************************************************/
bool XINDEX::GetAllSizes(PGLOBAL g,/* int &ndif,*/ int &numk)
  {
  char   *ftype;
  char    fn[_MAX_PATH];
  int     nv[NZ], id = -1; // n
//bool    estim = false;
  bool    rc = true;
  PDOSDEF defp = (PDOSDEF)Tdbp->To_Def;

//  ndif = numk = 0;
  numk = 0;

#if 0
  /*********************************************************************/
  /*  Get the estimated table size.                                    */
  /*  Note: for fixed tables we must use cardinality to avoid the call */
  /*  to MaxBlkSize that could reduce the cardinality value.           */
  /*********************************************************************/
  if (Tdbp->Cardinality(NULL)) {
    // For DBF tables, Cardinality includes bad or soft deleted lines
    // that are not included in the index, and can be larger then the
    // index size.
    estim = (Tdbp->Ftype == RECFM_DBF);
    n = Tdbp->Cardinality(g);      // n is exact table size
  } else {
    // Variable table not optimized
    estim = true;                  // n is an estimate of the size
    n = Tdbp->GetMaxSize(g);
  } // endif Cardinality

  if (n <= 0)
    return !(n == 0);             // n < 0 error, n = 0 void table

  /*********************************************************************/
  /*  Check the key part number.                                       */
  /*********************************************************************/
  if (!Nk) {
    strcpy(g->Message, MSG(NO_KEY_COL));
    return true;    // Error
    } // endif Nk
#endif // 0

  switch (Tdbp->Ftype) {
    case RECFM_VAR: ftype = ".dnx"; break;
    case RECFM_FIX: ftype = ".fnx"; break;
    case RECFM_BIN: ftype = ".bnx"; break;
    case RECFM_VCT: ftype = ".vnx"; break;
    case RECFM_DBF: ftype = ".dbx"; break;
    default:
      sprintf(g->Message, MSG(INVALID_FTYPE), Tdbp->Ftype);
      return true;
    } // endswitch Ftype

  if (defp->SepIndex()) {
    // Index was saved in a separate file
#if defined(__WIN__)
    char drive[_MAX_DRIVE];
#else
    char *drive = NULL;
#endif
    char direc[_MAX_DIR];
    char fname[_MAX_FNAME];

    _splitpath(defp->GetOfn(), drive, direc, fname, NULL);
    strcat(strcat(fname, "_"), Xdp->GetName());
    _makepath(fn, drive, direc, fname, ftype);
  } else {
    id = ID;
    strcat(PlugRemoveType(fn, strcpy(fn, defp->GetOfn())), ftype);
  } // endif sep

  PlugSetPath(fn, fn, Tdbp->GetPath());

  if (trace)
    htrc("Index %s file: %s\n", Xdp->GetName(), fn);

  /*********************************************************************/
  /*  Open the index file and check its validity.                      */
  /*********************************************************************/
  if (X->Open(g, fn, id, MODE_READ))
    goto err;               // No saved values

  // Get offset from XDB file
//if (X->Seek(g, Defoff, Defhigh, SEEK_SET))
//  goto err;

  //  Now start the reading process.
  if (X->Read(g, nv, NZ, sizeof(int)))
    goto err;

  if (trace)
    htrc("nv=%d %d %d %d\n", nv[0], nv[1], nv[2], nv[3]);

  // The test on ID was suppressed because MariaDB can change an index ID
  // when other indexes are added or deleted
  if (/*nv[0] != ID ||*/ nv[1] != Nk) {
    sprintf(g->Message, MSG(BAD_INDEX_FILE), fn);

    if (trace)
      htrc("nv[0]=%d ID=%d nv[1]=%d Nk=%d\n", nv[0], ID, nv[1], Nk);

    goto err;
    } // endif

#if 0
  if (nv[2]) {
    Mul = true;
    Ndif = nv[2] - 1;  // nv[2] is offset size, equal to Ndif + 1
  } else {
    Mul = false;
    Ndif = nv[3];
  } // endif nv[2]

  if (nv[3] < n && estim)
    n = nv[3];              // n was just an evaluated max value

  if (nv[3] != n) {
    sprintf(g->Message, MSG(OPT_NOT_MATCH), fn);
    goto err;
    } // endif
#endif // 0

  Num_K = nv[3];

#if 0
  if (Nk > 1) {
    if (nv[2] && X->Seek(g, nv[2] * sizeof(int), 0, SEEK_CUR))
      goto err;

    if (!nv[4] && X->Seek(g, Num_K * sizeof(int), 0, SEEK_CUR))
      goto err;

    if (X->Read(g, nv, NW, sizeof(int)))
      goto err;

    PCOL colp = *To_Cols;

    if (nv[4] != colp->GetResultType()  ||
       (nv[3] != colp->GetValue()->GetClen() && nv[4] != TYPE_STRING)) {
      sprintf(g->Message, MSG(XCOL_MISMATCH), colp->GetName());
      goto err;    // Error
      } // endif GetKey

    Ndif = nv[0];
    } // endif Nk
#endif // 0

  /*********************************************************************/
  /*  Set size values.                                                 */
  /*********************************************************************/
//ndif = Ndif;
  numk = Num_K;
  rc = false;

err:
  X->Close();
  return rc;
  } // end of GetAllSizes

/***********************************************************************/
/*  RANGE: Tell how many records exist for a given value, for an array */
/*  of values, or in a given value range.                              */
/***********************************************************************/
int XINDEX::Range(PGLOBAL g, int limit, bool incl)
  {
  int  i, k, n = 0;
  PXOB *xp = To_Vals;
  PXCOL kp = To_KeyCol;
  OPVAL op = Op;

  switch (limit) {
    case 1: Op = (incl) ? OP_GE : OP_GT; break;
    case 2: Op = (incl) ? OP_GT : OP_GE; break;
    default: return 0;
    } // endswitch limit

  /*********************************************************************/
  /*  Currently only range of constant values with an EQ operator is   */
  /*  implemented.  Find the number of rows for each given values.     */
  /*********************************************************************/
  if (xp[0]->GetType() == TYPE_CONST) {
    for (i = 0; kp; kp = kp->Next) {
      kp->Valp->SetValue_pval(xp[i]->GetValue(), !kp->Prefix);
      if (++i == Nval) break;
      } // endfor kp

    if ((k = FastFind()) < Num_K)
      n = k;
//      if (limit)
//        n = (Mul) ? k : kp->Val_K;
//      else
//        n = (Mul) ? Pof[kp->Val_K + 1] - k : 1;

  } else {
    strcpy(g->Message, MSG(RANGE_NO_JOIN));
    n = -1;                        // Logical error
  } // endif'f Type

  Op = op;
  return n;
  } // end of Range

/***********************************************************************/
/*  Return the size of the group (equal values) of the current value.  */
/***********************************************************************/
int XINDEX::GroupSize(void)
  {
#if defined(_DEBUG)
  assert(To_LastCol->Val_K >= 0 && To_LastCol->Val_K < Ndif);
#endif   // _DEBUG

  if (Nval == Nk)
    return (Pof) ? Pof[To_LastCol->Val_K + 1] - Pof[To_LastCol->Val_K]
                 : 1;

#if defined(_DEBUG)
  assert(To_LastVal);
#endif   // _DEBUG

  // Index whose only some columns are used
  int ck1, ck2;

  ck1 = To_LastVal->Val_K;
  ck2 = ck1 + 1;

#if defined(_DEBUG)
  assert(ck1 >= 0 && ck1 < To_LastVal->Ndf);
#endif   // _DEBUG

  for (PXCOL kcp = To_LastVal; kcp; kcp = kcp->Next) {
    ck1 = (kcp->Kof) ? kcp->Kof[ck1] : ck1;
    ck2 = (kcp->Kof) ? kcp->Kof[ck2] : ck2;
    } // endfor kcp

  return ck2 - ck1;
  } // end of GroupSize

/***********************************************************************/
/*  Find Cur_K and Val_K's of the next distinct value of the index.    */
/*  Returns false if Ok, true if there are no more different values.   */
/***********************************************************************/
bool XINDEX::NextValDif(void)
  {
  int  curk;
  PXCOL kcp = (To_LastVal) ? To_LastVal : To_LastCol;

  if (++kcp->Val_K < kcp->Ndf) {
    Cur_K = curk = kcp->Val_K;

    // (Cur_K return is currently not used by SQLGBX)
    for (PXCOL kp = kcp; kp; kp = kp->Next)
      Cur_K = (kp->Kof) ? kp->Kof[Cur_K] : Cur_K;

  } else
    return true;

  for (kcp = kcp->Previous; kcp; kcp = kcp->Previous) {
    if (kcp->Kof && curk < kcp->Kof[kcp->Val_K + 1])
      break;                  // all previous columns have same value

    curk = ++kcp->Val_K;      // This is a break, get new column value
    } // endfor kcp

  return false;
  } // end of NextValDif

/***********************************************************************/
/*  XINDEX: Find Cur_K and Val_K's of next index entry.                */
/*  If eq is true next values must be equal to last ones up to Nval.   */
/*  Returns false if Ok, true if there are no more (equal) values.     */
/***********************************************************************/
bool XINDEX::NextVal(bool eq)
  {
  int  n, neq = Nk + 1, curk;
  PXCOL kcp;

  if (Cur_K == Num_K)
    return true;
  else
    curk = ++Cur_K;

  for (n = Nk, kcp = To_LastCol; kcp; n--, kcp = kcp->Previous) {
    if (kcp->Kof) {
      if (curk == kcp->Kof[kcp->Val_K + 1])
        neq = n;

    } else {
#ifdef _DEBUG
      assert(curk == kcp->Val_K + 1);
#endif // _DEBUG
      neq = n;
    } // endif Kof

#ifdef _DEBUG
    assert(kcp->Val_K < kcp->Ndf);
#endif // _DEBUG

    // If this is not a break...
    if (neq > n)
      break;                  // all previous columns have same value

    curk = ++kcp->Val_K;      // This is a break, get new column value
    } // endfor kcp

  // Return true if no more values or, in case of "equal" values,
  // if the last used column value has changed
  return (Cur_K == Num_K || (eq && neq <= Nval));
  } // end of NextVal

/***********************************************************************/
/*  XINDEX: Find Cur_K and Val_K's of previous index entry.            */
/*  Returns false if Ok, true if there are no more values.             */
/***********************************************************************/
bool XINDEX::PrevVal(void)
  {
  int  n, neq = Nk + 1, curk;
  PXCOL kcp;

  if (Cur_K == 0)
    return true;
  else
    curk = --Cur_K;

  for (n = Nk, kcp = To_LastCol; kcp; n--, kcp = kcp->Previous) {
    if (kcp->Kof) {
      if (curk < kcp->Kof[kcp->Val_K])
        neq = n;

    } else {
#ifdef _DEBUG
      assert(curk == kcp->Val_K -1);
#endif // _DEBUG
      neq = n;
    } // endif Kof

#ifdef _DEBUG
    assert(kcp->Val_K >= 0);
#endif // _DEBUG

    // If this is not a break...
    if (neq > n)
      break;                  // all previous columns have same value

    curk = --kcp->Val_K;      // This is a break, get new column value
    } // endfor kcp

  return false;
  } // end of PrevVal

/***********************************************************************/
/*  XINDEX: Fetch a physical or logical record.                        */
/***********************************************************************/
int XINDEX::Fetch(PGLOBAL g)
  {
  int  n;
  PXCOL kp;

  if (Num_K == 0)
    return -1;                   // means end of file

  if (trace > 1)
    htrc("XINDEX Fetch: Op=%d\n", Op);

  /*********************************************************************/
  /*  Table read through a sorted index.                               */
  /*********************************************************************/
  switch (Op) {
    case OP_NEXT:                 // Read next
      if (NextVal(false))
        return -1;                // End of indexed file

      break;
    case OP_FIRST:                // Read first
      for (Cur_K = 0, kp = To_KeyCol; kp; kp = kp->Next)
        kp->Val_K = 0;

      Op = OP_NEXT;
      break;
    case OP_SAME:                 // Read next same
      // Logically the key values should be the same as before
      if (NextVal(true)) {
        Op = OP_EQ;
        return -2;                // no more equal values
        } // endif NextVal

      break;
    case OP_NXTDIF:               // Read next dif
//      while (!NextVal(true)) ;

//      if (Cur_K >= Num_K)
//        return -1;              // End of indexed file
      if (NextValDif())
        return -1;                // End of indexed file

      break;
    case OP_FSTDIF:               // Read first diff
      for (Cur_K = 0, kp = To_KeyCol; kp; kp = kp->Next)
        kp->Val_K = 0;

      Op = (Mul || Nval < Nk) ? OP_NXTDIF : OP_NEXT;
      break;
    case OP_LAST:                 // Read last key
      for (Cur_K = Num_K - 1, kp = To_KeyCol; kp; kp = kp->Next)
        kp->Val_K = kp->Kblp->GetNval() - 1;

      Op = OP_NEXT;
      break;
    case OP_PREV:                 // Read previous
      if (PrevVal())
        return -1;                // End of indexed file

      break;
    default:                      // Should be OP_EQ
//    if (Tbxp->Key_Rank < 0) {
        /***************************************************************/
        /*  Look for the first key equal to the link column values     */
        /*  and return its rank whithin the index table.               */
        /***************************************************************/
        for (n = 0, kp = To_KeyCol; n < Nval && kp; n++, kp = kp->Next)
          if (kp->InitFind(g, To_Vals[n]))
            return -1;               // No more constant values

        Nth++;

        if (trace > 1)
          htrc("Fetch: Looking for new value Nth=%d\n", Nth);

        Cur_K = FastFind();

        if (Cur_K >= Num_K)
          /*************************************************************/
          /* Rank not whithin index table, signal record not found.    */
          /*************************************************************/
          return -2;

        else if (Mul || Nval < Nk)
          Op = OP_SAME;

    } // endswitch Op

  /*********************************************************************/
  /*  If rank is equal to stored rank, record is already there.        */
  /*********************************************************************/
  if (Cur_K == Old_K)
    return -3;                   // Means record already there
  else
    Old_K = Cur_K;                // Store rank of newly read record

  /*********************************************************************/
  /*  Return the position of the required record.                      */
  /*********************************************************************/
  return (Incr) ? Cur_K * Incr : To_Rec[Cur_K];
  } // end of Fetch

/***********************************************************************/
/*  FastFind: Returns the index of matching record in a join using an  */
/*  optimized algorithm based on dichotomie and optimized comparing.   */
/***********************************************************************/
int XINDEX::FastFind(void)
  {
  register int  curk, sup, inf, i= 0, k, n = 2;
  register PXCOL kp, kcp;

//assert((int)nv == Nval);

  if (Nblk && Op == OP_EQ) {
    // Look in block values to find in which block to search
    sup = Nblk;
    inf = -1;

    while (n && sup - inf > 1) {
      i = (inf + sup) >> 1;

      n = To_KeyCol->CompBval(i);

      if (n < 0)
        sup = i;
      else
        inf = i;

      } // endwhile

    if (inf < 0)
      return Num_K;

//  i = inf;
    inf *= Sblk;

    if ((sup = inf + Sblk) > To_KeyCol->Ndf)
      sup = To_KeyCol->Ndf;

    inf--;
  } else {
    inf = -1;
    sup = To_KeyCol->Ndf;
  } // endif Nblk

  if (trace > 2)
    htrc("XINDEX FastFind: Nblk=%d Op=%d inf=%d sup=%d\n",
                           Nblk, Op, inf, sup); 

  for (k = 0, kcp = To_KeyCol; kcp; kcp = kcp->Next) {
    while (sup - inf > 1) {
      i = (inf + sup) >> 1;

      n = kcp->CompVal(i);

      if      (n < 0)
        sup = i;
      else if (n > 0)
        inf = i;
      else
        break;

      } // endwhile

    if (n) {
      if (Op != OP_EQ) {
        // Currently only OP_GT or OP_GE
        kcp->Val_K = curk = sup;

        // Check for value changes in previous key parts
        for (kp = kcp->Previous; kp; kp = kp->Previous)
          if (kp->Kof && curk < kp->Kof[kp->Val_K + 1])
            break;
          else
            curk = ++kp->Val_K;

        n = 0;
        } // endif Op

      break;
      } // endif n

    kcp->Val_K = i;

    if (++k == Nval) {
      if (Op == OP_GT) {            // n is always 0
        curk = ++kcp->Val_K;        // Increment value by 1

        // Check for value changes in previous key parts
        for (kp = kcp->Previous; kp; kp = kp->Previous)
          if (kp->Kof && curk < kp->Kof[kp->Val_K + 1])
            break;                  // Not changed
          else
            curk = ++kp->Val_K;

        } // endif Op

      break;      // So kcp remains pointing the last tested block
      } // endif k

    if (kcp->Kof) {
      inf = kcp->Kof[i] - 1;
      sup = kcp->Kof[i + 1];
    } else {
      inf = i - 1;
      sup = i + 1;
    } // endif Kof

    } // endfor k, kcp

  if (n) {
    // Record not found
    for (kcp = To_KeyCol; kcp; kcp = kcp->Next)
      kcp->Val_K = kcp->Ndf;       // Not a valid value

    return Num_K;
    } // endif n

  for (curk = kcp->Val_K; kcp; kcp = kcp->Next) {
    kcp->Val_K = curk;
    curk = (kcp->Kof) ? kcp->Kof[kcp->Val_K] : kcp->Val_K;
    } // endfor kcp

  if (trace > 2)
    htrc("XINDEX FastFind: curk=%d\n", curk);

  return curk;
  } // end of FastFind

/* -------------------------- XINDXS Class --------------------------- */

/***********************************************************************/
/*  XINDXS public constructor.                                         */
/***********************************************************************/
XINDXS::XINDXS(PTDBDOS tdbp, PIXDEF xdp, PXLOAD pxp, PCOL *cp, PXOB *xp)
      : XINDEX(tdbp, xdp, pxp, cp, xp)
  {
  Srtd = To_Cols[0]->GetOpt() == 2;
  } // end of XINDXS constructor

/***********************************************************************/
/*  XINDXS compare routine for C Quick/Insertion sort.                 */
/***********************************************************************/
int XINDXS::Qcompare(int *i1, int *i2)
  {
//num_comp++;
  return To_KeyCol->Compare(*i1, *i2);
  } // end of Qcompare

/***********************************************************************/
/*  Range: Tell how many records exist for given value(s):             */
/*  If limit=0 return range for these values.                          */
/*  If limit=1 return the start of range.                              */
/*  If limit=2 return the end of range.                                */
/***********************************************************************/
int XINDXS::Range(PGLOBAL g, int limit, bool incl)
  {
  int  k, n = 0;
  PXOB  xp = To_Vals[0];
  PXCOL kp = To_KeyCol;
  OPVAL op = Op;

  switch (limit) {
    case 1: Op = (incl) ? OP_GE : OP_GT; break;
    case 2: Op = (incl) ? OP_GT : OP_GE; break;
    default: Op = OP_EQ;
    } // endswitch limit

  /*********************************************************************/
  /*  Currently only range of constant values with an EQ operator is   */
  /*  implemented.  Find the number of rows for each given values.     */
  /*********************************************************************/
  if (xp->GetType() == TYPE_CONST) {
    kp->Valp->SetValue_pval(xp->GetValue(), !kp->Prefix);
    k = FastFind();

    if (k < Num_K || Op != OP_EQ)
      if (limit)
        n = (Mul) ? k : kp->Val_K;
      else
        n = (Mul) ? Pof[kp->Val_K + 1] - k : 1;

  } else {
    strcpy(g->Message, MSG(RANGE_NO_JOIN));
    n = -1;                        // Logical error
  } // endif'f Type

  Op = op;
  return n;
  } // end of Range

/***********************************************************************/
/*  Return the size of the group (equal values) of the current value.  */
/***********************************************************************/
int XINDXS::GroupSize(void)
  {
#if defined(_DEBUG)
  assert(To_KeyCol->Val_K >= 0 && To_KeyCol->Val_K < Ndif);
#endif   // _DEBUG
  return (Pof) ? Pof[To_KeyCol->Val_K + 1] - Pof[To_KeyCol->Val_K] : 1;
  } // end of GroupSize

/***********************************************************************/
/*  XINDXS: Find Cur_K and Val_K of previous index value.              */
/*  Returns false if Ok, true if there are no more values.             */
/***********************************************************************/
bool XINDXS::PrevVal(void)
  {
  if (--Cur_K < 0)
    return true;

  if (Mul) {
    if (Cur_K < Pof[To_KeyCol->Val_K])
      To_KeyCol->Val_K--;

  } else
    To_KeyCol->Val_K = Cur_K;

  return false;
  } // end of PrevVal

/***********************************************************************/
/*  XINDXS: Find Cur_K and Val_K of next index value.                  */
/*  If b is true next value must be equal to last one.                 */
/*  Returns false if Ok, true if there are no more (equal) values.     */
/***********************************************************************/
bool XINDXS::NextVal(bool eq)
  {
  bool rc;

  if (To_KeyCol->Val_K == Ndif)
    return true;

  if (Mul) {
    int limit = Pof[To_KeyCol->Val_K + 1];

#ifdef _DEBUG
    assert(Cur_K < limit);
    assert(To_KeyCol->Val_K < Ndif);
#endif // _DEBUG

    if (++Cur_K == limit) {
      To_KeyCol->Val_K++;
      rc = (eq || limit == Num_K);
    } else
      rc = false;

  } else
    rc = (To_KeyCol->Val_K = ++Cur_K) == Num_K || eq;

  return rc;
  } // end of NextVal

/***********************************************************************/
/*  XINDXS: Fetch a physical or logical record.                        */
/***********************************************************************/
int XINDXS::Fetch(PGLOBAL g)
  {
  if (Num_K == 0)
    return -1;                   // means end of file

  if (trace > 1)
    htrc("XINDXS Fetch: Op=%d\n", Op);

  /*********************************************************************/
  /*  Table read through a sorted index.                               */
  /*********************************************************************/
  switch (Op) {
    case OP_NEXT:                // Read next
      if (NextVal(false))
        return -1;               // End of indexed file

      break;
    case OP_FIRST:               // Read first
      To_KeyCol->Val_K = Cur_K = 0;
      Op = OP_NEXT;
      break;
    case OP_SAME:                 // Read next same
      if (!Mul || NextVal(true)) {
        Op = OP_EQ;
        return -2;               // No more equal values
        } // endif Mul

      break;
    case OP_NXTDIF:              // Read next dif
      if (++To_KeyCol->Val_K == Ndif)
        return -1;               // End of indexed file

      Cur_K = Pof[To_KeyCol->Val_K];
      break;
    case OP_FSTDIF:               // Read first diff
      To_KeyCol->Val_K = Cur_K = 0;
      Op = (Mul) ? OP_NXTDIF : OP_NEXT;
      break;
    case OP_LAST:                // Read first
      Cur_K = Num_K - 1;
      To_KeyCol->Val_K = Ndif - 1;
      Op = OP_PREV;
      break;
    case OP_PREV:                // Read previous
      if (PrevVal())
        return -1;               // End of indexed file

      break;
    default:                     // Should be OP_EQ
      /*****************************************************************/
      /*  Look for the first key equal to the link column values       */
      /*  and return its rank whithin the index table.                 */
      /*****************************************************************/
      if (To_KeyCol->InitFind(g, To_Vals[0]))
        return -1;                 // No more constant values
      else
        Nth++;

      if (trace > 1)
        htrc("Fetch: Looking for new value Nth=%d\n", Nth);

      Cur_K = FastFind();

      if (Cur_K >= Num_K)
        // Rank not whithin index table, signal record not found
        return -2;
      else if (Mul)
        Op = OP_SAME;

    } // endswitch Op

  /*********************************************************************/
  /*  If rank is equal to stored rank, record is already there.        */
  /*********************************************************************/
  if (Cur_K == Old_K)
    return -3;                   // Means record already there
  else
    Old_K = Cur_K;                // Store rank of newly read record

  /*********************************************************************/
  /*  Return the position of the required record.                      */
  /*********************************************************************/
  return (Incr) ? Cur_K * Incr : To_Rec[Cur_K];
  } // end of Fetch

/***********************************************************************/
/*  FastFind: Returns the index of matching indexed record using an    */
/*  optimized algorithm based on dichotomie and optimized comparing.   */
/***********************************************************************/
int XINDXS::FastFind(void)
  {
  register int   sup, inf, i= 0, n = 2;
  register PXCOL kcp = To_KeyCol;

  if (Nblk && Op == OP_EQ) {
    // Look in block values to find in which block to search
    sup = Nblk;
    inf = -1;

    while (n && sup - inf > 1) {
      i = (inf + sup) >> 1;

      n = kcp->CompBval(i);

      if (n < 0)
        sup = i;
      else
        inf = i;

      } // endwhile

    if (inf < 0)
      return Num_K;

    inf *= Sblk;

    if ((sup = inf + Sblk) > Ndif)
      sup = Ndif;

    inf--;
  } else {
    inf = -1;
    sup = Ndif;
  } // endif Nblk

  if (trace > 2)
    htrc("XINDXS FastFind: Nblk=%d Op=%d inf=%d sup=%d\n",
                           Nblk, Op, inf, sup); 

  while (sup - inf > 1) {
    i = (inf + sup) >> 1;

    n = kcp->CompVal(i);

    if      (n < 0)
      sup = i;
    else if (n > 0)
      inf = i;
    else
      break;

    } // endwhile

  if (!n && Op == OP_GT) {
    ++i;
  } else if (n && Op != OP_EQ) {
    // Currently only OP_GT or OP_GE
    i = sup;
    n = 0;
  } // endif sup

  if (trace > 2)
    htrc("XINDXS FastFind: n=%d i=%d\n", n, i);

  // Loop on kcp because of dynamic indexing
  for (; kcp; kcp = kcp->Next)
    kcp->Val_K = i;                 // Used by FillValue

  return ((n) ? Num_K : (Mul) ? Pof[i] : i);
  } // end of FastFind

/* -------------------------- XLOAD Class --------------------------- */

/***********************************************************************/
/*  XLOAD constructor.                                                 */
/***********************************************************************/
XLOAD::XLOAD(void)
  {
  Hfile = INVALID_HANDLE_VALUE;
  NewOff.Val = 0LL;
} // end of XLOAD constructor

/***********************************************************************/
/*  Close the index huge file.                                         */
/***********************************************************************/
void XLOAD::Close(void)
  {
  if (Hfile != INVALID_HANDLE_VALUE) {
    CloseFileHandle(Hfile);
    Hfile = INVALID_HANDLE_VALUE;
    } // endif Hfile

  } // end of Close

/* --------------------------- XFILE Class --------------------------- */

/***********************************************************************/
/*  XFILE constructor.                                                 */
/***********************************************************************/
XFILE::XFILE(void) : XLOAD()
  {
  Xfile = NULL;
#if defined(XMAP)
  Mmp = NULL;
#endif   // XMAP
  } // end of XFILE constructor

/***********************************************************************/
/*  Xopen function: opens a file using native API's.                   */
/***********************************************************************/
bool XFILE::Open(PGLOBAL g, char *filename, int id, MODE mode)
  {
  char *pmod;
  bool  rc;
  IOFF  noff[MAX_INDX];

  /*********************************************************************/
  /*  Open the index file according to mode.                           */
  /*********************************************************************/
  switch (mode) {
    case MODE_READ:   pmod = "rb"; break;
    case MODE_WRITE:  pmod = "wb"; break;
    case MODE_INSERT: pmod = "ab"; break;
    default:
      sprintf(g->Message, MSG(BAD_FUNC_MODE), "Xopen", mode);
      return true;
    } // endswitch mode

  if (!(Xfile= global_fopen(g, MSGID_OPEN_ERROR_AND_STRERROR, filename, pmod))) {
    if (trace)
      htrc("Open: %s\n", g->Message);

    return true;
    } // endif Xfile

  if (mode == MODE_INSERT) {
    /*******************************************************************/
    /* Position the cursor at end of file so ftell returns file size.  */
    /*******************************************************************/
    if (fseek(Xfile, 0, SEEK_END)) {
      sprintf(g->Message, MSG(FUNC_ERRNO), errno, "Xseek");
      return true;
      } // endif

    NewOff.v.Low = (int)ftell(Xfile);

    if (trace)
      htrc("XFILE Open: NewOff.v.Low=%d\n", NewOff.v.Low);

  } else if (mode == MODE_WRITE) {
    if (id >= 0) {
      // New not sep index file. Write the header.
      memset(noff, 0, sizeof(noff));
      Write(g, noff, sizeof(IOFF), MAX_INDX, rc);
      fseek(Xfile, 0, SEEK_END);
      NewOff.v.Low = (int)ftell(Xfile);

      if (trace)
        htrc("XFILE Open: NewOff.v.Low=%d\n", NewOff.v.Low);

      } // endif id

  } else if (mode == MODE_READ && id >= 0) {
    // Get offset from the header
    if (fread(noff, sizeof(IOFF), MAX_INDX, Xfile) != MAX_INDX) {
      sprintf(g->Message, MSG(XFILE_READERR), errno);
      return true;
      } // endif MAX_INDX

      if (trace)
        htrc("XFILE Open: noff[%d].v.Low=%d\n", id, noff[id].v.Low);

    // Position the cursor at the offset of this index
    if (fseek(Xfile, noff[id].v.Low, SEEK_SET)) {
      sprintf(g->Message, MSG(FUNC_ERRNO), errno, "Xseek");
      return true;
      } // endif

  } // endif mode

  return false;
  } // end of Open

/***********************************************************************/
/*  Move into an index file.                                           */
/***********************************************************************/
bool XFILE::Seek(PGLOBAL g, int low, int high __attribute__((unused)),
                            int origin)
  {
#if defined(_DEBUG)
  assert(high == 0);
#endif  // !_DEBUG

  if (fseek(Xfile, low, origin)) {
    sprintf(g->Message, MSG(FUNC_ERRNO), errno, "Xseek");
    return true;
    } // endif

  return false;
  } // end of Seek

/***********************************************************************/
/*  Read from the index file.                                          */
/***********************************************************************/
bool XFILE::Read(PGLOBAL g, void *buf, int n, int size)
  {
  if (fread(buf, size, n, Xfile) != (size_t)n) {
    sprintf(g->Message, MSG(XFILE_READERR), errno);
    return true;
    } // endif size

  return false;
  } // end of Read

/***********************************************************************/
/*  Write on index file, set rc and return the number of bytes written */
/***********************************************************************/
int XFILE::Write(PGLOBAL g, void *buf, int n, int size, bool& rc)
  {
  int niw = (int)fwrite(buf, size, n, Xfile);

  if (niw != n) {
    sprintf(g->Message, MSG(XFILE_WRITERR), strerror(errno));
    rc = true;
    } // endif size

  return niw * size;
  } // end of Write

/***********************************************************************/
/*  Update the file header and close the index file.                   */
/***********************************************************************/
void XFILE::Close(char *fn, int id)
  {
  if (id >= 0 && fn && Xfile) {
    fclose(Xfile);

    if ((Xfile = fopen(fn, "r+b")))
      if (!fseek(Xfile, id * sizeof(IOFF), SEEK_SET))
        fwrite(&NewOff,  sizeof(int), 2, Xfile);

    } // endif id

  Close();
  } // end of Close

/***********************************************************************/
/*  Close the index file.                                              */
/***********************************************************************/
void XFILE::Close(void)
  {
  XLOAD::Close();

  if (Xfile) {
    fclose(Xfile);
    Xfile = NULL;
    } // endif Xfile

#if defined(XMAP)
  if (Mmp && CloseMemMap(Mmp->memory, Mmp->lenL))
    printf("Error closing mapped index\n");
#endif   // XMAP
  } // end of Close

#if defined(XMAP)
  /*********************************************************************/
  /*  Map the entire index file.                                       */
  /*********************************************************************/
void *XFILE::FileView(PGLOBAL g, char *fn)
  {
  HANDLE  h;

  Mmp = (MMP)PlugSubAlloc(g, NULL, sizeof(MEMMAP));
  h = CreateFileMap(g, fn, Mmp, MODE_READ, false);

  if (h == INVALID_HANDLE_VALUE || (!Mmp->lenH && !Mmp->lenL)) {
    if (!(*g->Message))
      strcpy(g->Message, MSG(FILE_MAP_ERR));

    CloseFileHandle(h);                    // Not used anymore
    return NULL;               // No saved values
    } // endif h

  CloseFileHandle(h);                    // Not used anymore
  return Mmp->memory;
  } // end of FileView
#endif // XMAP

/* -------------------------- XHUGE Class --------------------------- */

/***********************************************************************/
/*  Xopen function: opens a file using native API's.                   */
/***********************************************************************/
bool XHUGE::Open(PGLOBAL g, char *filename, int id, MODE mode)
  {
  IOFF noff[MAX_INDX];

  if (Hfile != INVALID_HANDLE_VALUE) {
    sprintf(g->Message, MSG(FILE_OPEN_YET), filename);
    return true;
    } // endif

  if (trace)
    htrc(" Xopen: filename=%s id=%d mode=%d\n", filename, id, mode);

#if defined(__WIN__)
  LONG  high = 0;
  DWORD rc, drc, access, share, creation;

  /*********************************************************************/
  /*  Create the file object according to access mode                  */
  /*********************************************************************/
  switch (mode) {
    case MODE_READ:
      access = GENERIC_READ;
      share = FILE_SHARE_READ;
      creation = OPEN_EXISTING;
      break;
    case MODE_WRITE:
      access = GENERIC_WRITE;
      share = 0;
      creation = CREATE_ALWAYS;
      break;
    case MODE_INSERT:
      access = GENERIC_WRITE;
      share = 0;
      creation = OPEN_EXISTING;
      break;
    default:
      sprintf(g->Message, MSG(BAD_FUNC_MODE), "Xopen", mode);
      return true;
    } // endswitch

  Hfile = CreateFile(filename, access, share, NULL, creation,
                               FILE_ATTRIBUTE_NORMAL, NULL);

  if (Hfile == INVALID_HANDLE_VALUE) {
    rc = GetLastError();
    sprintf(g->Message, MSG(OPEN_ERROR), rc, mode, filename);
    FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM |
                  FORMAT_MESSAGE_IGNORE_INSERTS, NULL, rc, 0,
                  (LPTSTR)filename, sizeof(filename), NULL);
    strcat(g->Message, filename);
    return true;
    } // endif Hfile

  if (trace)
    htrc(" access=%p share=%p creation=%d handle=%p fn=%s\n",
         access, share, creation, Hfile, filename);

  if (mode == MODE_INSERT) {
    /*******************************************************************/
    /* In Insert mode we must position the cursor at end of file.      */
    /*******************************************************************/
    rc = SetFilePointer(Hfile, 0, &high, FILE_END);

    if (rc == INVALID_SET_FILE_POINTER && (drc = GetLastError()) != NO_ERROR) {
      sprintf(g->Message, MSG(ERROR_IN_SFP), drc);
      CloseHandle(Hfile);
      Hfile = INVALID_HANDLE_VALUE;
      return true;
      } // endif

    NewOff.v.Low = (int)rc;
    NewOff.v.High = (int)high;
  } else if (mode == MODE_WRITE) {
    if (id >= 0) {
      // New not sep index file. Write the header.
      memset(noff, 0, sizeof(noff));
      rc = WriteFile(Hfile, noff, sizeof(noff), &drc, NULL);
      NewOff.v.Low = (int)drc;
      } // endif id

  } else if (mode == MODE_READ && id >= 0) {
    // Get offset from the header
    rc = ReadFile(Hfile, noff, sizeof(noff), &drc, NULL);

    if (!rc) {
      sprintf(g->Message, MSG(XFILE_READERR), GetLastError());
      return true;
      } // endif rc

    // Position the cursor at the offset of this index
    rc = SetFilePointer(Hfile, noff[id].v.Low,
                       (PLONG)&noff[id].v.High, FILE_BEGIN);

    if (rc == INVALID_SET_FILE_POINTER) {
      sprintf(g->Message, MSG(FUNC_ERRNO), GetLastError(), "SetFilePointer");
      return true;
      } // endif

  } // endif Mode

#else   // UNIX
  int    oflag = O_LARGEFILE;         // Enable file size > 2G
  mode_t pmod = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH;

  /*********************************************************************/
  /*  Create the file object according to access mode                  */
  /*********************************************************************/
  switch (mode) {
    case MODE_READ:
      oflag |= O_RDONLY;
      break;
    case MODE_WRITE:
      oflag |= O_WRONLY | O_CREAT | O_TRUNC;
//    pmod = S_IREAD | S_IWRITE;
      break;
    case MODE_INSERT:
      oflag |= (O_WRONLY | O_APPEND);
      break;
    default:
      sprintf(g->Message, MSG(BAD_FUNC_MODE), "Xopen", mode);
      return true;
    } // endswitch

  Hfile= global_open(g, MSGID_OPEN_ERROR_AND_STRERROR, filename, oflag, pmod);

  if (Hfile == INVALID_HANDLE_VALUE) {
    /*rc = errno;*/
    if (trace)
      htrc("Open: %s\n", g->Message);

    return true;
    } // endif Hfile

  if (trace)
    htrc(" oflag=%p mode=%d handle=%d fn=%s\n", 
           oflag, mode, Hfile, filename);

  if (mode == MODE_INSERT) {
    /*******************************************************************/
    /* Position the cursor at end of file so ftell returns file size.  */
    /*******************************************************************/
    if (!(NewOff.Val = (longlong)lseek64(Hfile, 0LL, SEEK_END))) {
      sprintf(g->Message, MSG(FUNC_ERRNO), errno, "Seek");
      return true;
      } // endif

    if (trace)
      htrc("INSERT: NewOff=%lld\n", NewOff.Val);

  } else if (mode == MODE_WRITE) {
    if (id >= 0) {
      // New not sep index file. Write the header.
      memset(noff, 0, sizeof(noff));
      NewOff.v.Low = write(Hfile, &noff, sizeof(noff));
      } // endif id

    if (trace)
      htrc("WRITE: NewOff=%lld\n", NewOff.Val);

  } else if (mode == MODE_READ && id >= 0) {
    // Get offset from the header
    if (read(Hfile, noff, sizeof(noff)) != sizeof(noff)) {
      sprintf(g->Message, MSG(READ_ERROR), "Index file", strerror(errno));
      return true;
      } // endif read
      
	  if (trace)
      htrc("noff[%d]=%lld\n", id, noff[id].Val);

    // Position the cursor at the offset of this index
    if (lseek64(Hfile, noff[id].Val, SEEK_SET) < 0) {
      sprintf(g->Message, "(XHUGE)lseek64: %s (%lld)", strerror(errno), noff[id].Val);
      printf("%s\n", g->Message);
//    sprintf(g->Message, MSG(FUNC_ERRNO), errno, "Hseek");
      return true;
      } // endif lseek64

  } // endif mode
#endif  // UNIX

  return false;
  } // end of Open

/***********************************************************************/
/*  Go to position in a huge file.                                     */
/***********************************************************************/
bool XHUGE::Seek(PGLOBAL g, int low, int high, int origin)
  {
#if defined(__WIN__)
  LONG  hi = high;
  DWORD rc = SetFilePointer(Hfile, low, &hi, origin);

  if (rc == INVALID_SET_FILE_POINTER && GetLastError() != NO_ERROR) {
    sprintf(g->Message, MSG(FUNC_ERROR), "Xseek");
    return true;
    } // endif

#else // UNIX
  off64_t pos = (off64_t)low
              + (off64_t)high * ((off64_t)0x100 * (off64_t)0x1000000);

  if (lseek64(Hfile, pos, origin) < 0) {
    sprintf(g->Message, MSG(ERROR_IN_LSK), errno);

    if (trace)
      htrc("lseek64 error %d\n", errno);

    return true;
    } // endif lseek64

  if (trace)
    htrc("Seek: low=%d high=%d\n", low, high);
#endif // UNIX

  return false;
  } // end of Seek

/***********************************************************************/
/*  Read from a huge index file.                                       */
/***********************************************************************/
bool XHUGE::Read(PGLOBAL g, void *buf, int n, int size)
  {
  bool rc = false;

#if defined(__WIN__)
  bool    brc;
  DWORD   nbr, count = (DWORD)(n * size);

  brc = ReadFile(Hfile, buf, count, &nbr, NULL);

  if (brc) {
    if (nbr != count) {
      strcpy(g->Message, MSG(EOF_INDEX_FILE));
      rc = true;
      } // endif nbr

  } else {
    char *buf[256];
    DWORD drc = GetLastError();

    FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM |
                  FORMAT_MESSAGE_IGNORE_INSERTS, NULL, drc, 0,
                  (LPTSTR)buf, sizeof(buf), NULL);
    sprintf(g->Message, MSG(READ_ERROR), "index file", buf);
    rc = true;
  } // endif brc
#else    // UNIX
  ssize_t count = (ssize_t)(n * size);

  if (trace)
    htrc("Hfile=%d n=%d size=%d count=%d\n", Hfile, n, size, count);

  if (read(Hfile, buf, count) != count) {
    sprintf(g->Message, MSG(READ_ERROR), "Index file", strerror(errno));

    if (trace)
      htrc("read error %d\n", errno);

    rc = true;
    } // endif nbr
#endif   // UNIX

  return rc;
  } // end of Read

/***********************************************************************/
/*  Write on a huge index file.                                        */
/***********************************************************************/
int XHUGE::Write(PGLOBAL g, void *buf, int n, int size, bool& rc)
  {
#if defined(__WIN__)
  bool    brc;
  DWORD   nbw, count = (DWORD)n * (DWORD) size;

  brc = WriteFile(Hfile, buf, count, &nbw, NULL);

  if (!brc) {
    char msg[256];
    DWORD drc = GetLastError();

    FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM |
                  FORMAT_MESSAGE_IGNORE_INSERTS, NULL, drc, 0,
                  (LPTSTR)msg, sizeof(msg), NULL);
    sprintf(g->Message, MSG(WRITING_ERROR), "index file", msg);
    rc = true;
    } // endif size

  return (int)nbw;
#else    // UNIX
  ssize_t nbw;
  size_t  count = (size_t)n * (size_t)size;

  nbw = write(Hfile, buf, count);

  if (nbw != (signed)count) {
    sprintf(g->Message, MSG(WRITING_ERROR),
                        "index file", strerror(errno));
    rc = true;
    } // endif nbw

  return (int)nbw;
#endif   // UNIX
  } // end of Write

/***********************************************************************/
/*  Update the file header and close the index file.                   */
/***********************************************************************/
void XHUGE::Close(char *fn, int id)
  {
  if (trace)
    htrc("XHUGE::Close: fn=%s id=%d NewOff=%lld\n", fn, id, NewOff.Val);

#if defined(__WIN__)
  if (id >= 0 && fn) {
    CloseFileHandle(Hfile);
    Hfile = CreateFile(fn, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

    if (Hfile != INVALID_HANDLE_VALUE)
      if (SetFilePointer(Hfile, id * sizeof(IOFF), NULL, FILE_BEGIN)
              != INVALID_SET_FILE_POINTER) {
        DWORD nbw;

        WriteFile(Hfile, &NewOff, sizeof(IOFF), &nbw, NULL);
        } // endif SetFilePointer

    } // endif id
#else   // !__WIN__
  if (id >= 0 && fn) {
    if (Hfile != INVALID_HANDLE_VALUE) {
      if (lseek64(Hfile, id * sizeof(IOFF), SEEK_SET) >= 0) {
        ssize_t nbw = write(Hfile, &NewOff, sizeof(IOFF));
			  
        if (nbw != (signed)sizeof(IOFF))
          htrc("Error writing index file header: %s\n", strerror(errno));
		    
      } else
        htrc("(XHUGE::Close)lseek64: %s (%d)\n", strerror(errno), id);
			
    } else
      htrc("(XHUGE)error reopening %s: %s\n", fn, strerror(errno));

    } // endif id
#endif  // !__WIN__

  XLOAD::Close();
  } // end of Close

#if defined(XMAP)
/***********************************************************************/
/*  Don't know whether this is possible for huge files.                */
/***********************************************************************/
void *XHUGE::FileView(PGLOBAL g, char *)
  {
  strcpy(g->Message, MSG(NO_PART_MAP));
  return NULL;
  } // end of FileView
#endif   // XMAP

/* -------------------------- XXROW Class --------------------------- */

/***********************************************************************/
/*  XXROW Public Constructor.                                          */
/***********************************************************************/
XXROW::XXROW(PTDBDOS tdbp) : XXBASE(tdbp, false)
  {
  Srtd = true;
  Tdbp = tdbp;
  Valp = NULL;
  } // end of XXROW constructor

/***********************************************************************/
/*  XXROW Reset: re-initialize a Kindex block.                         */
/***********************************************************************/
void XXROW::Reset(void)
  {
#if defined(_DEBUG)
  assert(Tdbp->GetLink());                // This a join index
#endif   // _DEBUG
  } // end of Reset

/***********************************************************************/
/*  Init: Open and Initialize a Key Index.                             */
/***********************************************************************/
bool XXROW::Init(PGLOBAL g)
  {
  /*********************************************************************/
  /*  Table will be accessed through an index table.                   */
  /*  To_Link should not be NULL.                                      */
  /*********************************************************************/
  if (!Tdbp->GetLink() || Tbxp->GetKnum() != 1)
    return true;

  if ((*Tdbp->GetLink())->GetResultType() != TYPE_INT) {
    strcpy(g->Message, MSG(TYPE_MISMATCH));
    return true;
  } else
    Valp = (*Tdbp->GetLink())->GetValue();

  if ((Num_K = Tbxp->Cardinality(g)) < 0)
    return true;                   // Not a fixed file

  /*********************************************************************/
  /*  The entire table is indexed, no need to construct the index.     */
  /*********************************************************************/
  Cur_K = Num_K;
  return false;
  } // end of Init

/***********************************************************************/
/*  RANGE: Tell how many record exist in a given value range.          */
/***********************************************************************/
int XXROW::Range(PGLOBAL, int limit, bool incl)
  {
  int  n = Valp->GetIntValue();

  switch (limit) {
    case 1: n += ((incl) ? 0 : 1); break;
    case 2: n += ((incl) ? 1 : 0); break;
    default: n = 1;
    } // endswitch limit

  return n;
  } // end of Range

/***********************************************************************/
/*  XXROW: Fetch a physical or logical record.                         */
/***********************************************************************/
int XXROW::Fetch(PGLOBAL)
  {
  if (Num_K == 0)
    return -1;       // means end of file

  /*********************************************************************/
  /*  Look for a key equal to the link column of previous table,       */
  /*  and return its rank whithin the index table.                     */
  /*********************************************************************/
  Cur_K = FastFind();

  if (Cur_K >= Num_K)
    /*******************************************************************/
    /* Rank not whithin index table, signal record not found.          */
    /*******************************************************************/
    return -2;      // Means record not found

  /*********************************************************************/
  /*  If rank is equal to stored rank, record is already there.        */
  /*********************************************************************/
  if (Cur_K == Old_K)
    return -3;                   // Means record already there
  else
    Old_K = Cur_K;                // Store rank of newly read record

  return Cur_K;
  } // end of Fetch

/***********************************************************************/
/*  FastFind: Returns the index of matching record in a join.          */
/***********************************************************************/
int XXROW::FastFind(void)
  {
  int n = Valp->GetIntValue();

  if (n < 0)
    return (Op == OP_EQ) ? (-1) : 0;
  else if (n > Num_K)
    return Num_K;
  else
    return (Op == OP_GT) ? n : (n - 1);

  } // end of FastFind

/* ------------------------- KXYCOL Classes -------------------------- */

/***********************************************************************/
/*  KXYCOL public constructor.                                         */
/***********************************************************************/
KXYCOL::KXYCOL(PKXBASE kp) : To_Keys(Keys.Memp),
        To_Bkeys(Bkeys.Memp), Kof((CPINT&)Koff.Memp)
  {
  Next = NULL;
  Previous = NULL;
  Kxp = kp;
  Colp = NULL;
  IsSorted = false;
  Asc = true;
  Keys = Nmblk;
  Kblp = NULL;
  Bkeys = Nmblk;
  Blkp = NULL;
  Valp = NULL;
  Klen = 0;
  Kprec = 0;
  Type = TYPE_ERROR;
  Prefix = false;
  Koff = Nmblk;
  Val_K = 0;
  Ndf = 0;
  Mxs = 0;
  } // end of KXYCOL constructor

/***********************************************************************/
/*  KXYCOL Init: initialize and allocate storage.                      */
/*  Key length kln can be smaller than column length for CHAR columns. */
/***********************************************************************/
bool KXYCOL::Init(PGLOBAL g, PCOL colp, int n, bool sm, int kln)
  {
  int len = colp->GetLength(), prec = colp->GetScale();

  // Currently no indexing on NULL columns
  if (colp->IsNullable() && kln) {
    sprintf(g->Message, "Cannot index nullable column %s", colp->GetName());
    return true;
    } // endif nullable

  if (kln && len > kln && colp->GetResultType() == TYPE_STRING) {
    len = kln;
    Prefix = true;
    } // endif kln

  if (trace)
    htrc("KCOL(%p) Init: col=%s n=%d type=%d sm=%d\n",
         this, colp->GetName(), n, colp->GetResultType(), sm);

  // Allocate the Value object used when moving items
  Type = colp->GetResultType();

  if (!(Valp = AllocateValue(g, Type, len, prec, colp->IsUnsigned())))
    return true;

  Klen = Valp->GetClen();
  Keys.Size = n * Klen;

  if (!PlgDBalloc(g, NULL, Keys)) {
    sprintf(g->Message, MSG(KEY_ALLOC_ERROR), Klen, n);
    return true;    // Error
    } // endif

  // Allocate the Valblock. The last parameter is to have rows filled
  // by blanks (if true) or keep the zero ending char (if false).
  // Currently we set it to true to be compatible with QRY blocks,
  // and the one before last is to enable length/type checking, set to
  // true if not a prefix key.
  Kblp = AllocValBlock(g, To_Keys, Type, n, len, prec, !Prefix, true);
  Asc = sm;                    // Sort mode: Asc=true  Desc=false
  Ndf = n;

  // Store this information to avoid sorting when already done
  if (Asc)
    IsSorted = colp->GetOpt() == 2;

//SetNulls(colp->IsNullable()); for when null columns will be indexable
  Colp = colp;
  return false;
  } // end of Init

#if defined(XMAP)
/***********************************************************************/
/*  KXYCOL MapInit: initialize and address storage.                    */
/*  Key length kln can be smaller than column length for CHAR columns. */
/***********************************************************************/
BYTE* KXYCOL::MapInit(PGLOBAL g, PCOL colp, int *n, BYTE *m)
  {
  int len = colp->GetLength(), prec = colp->GetScale();

  if (n[3] && colp->GetLength() > n[3]
           && colp->GetResultType() == TYPE_STRING) {
    len = n[3];
    Prefix = true;
    } // endif kln

  Type = colp->GetResultType();

  if (trace)
    htrc("MapInit(%p): colp=%p type=%d n=%d len=%d m=%p\n",
         this, colp, Type, n[0], len, m);

  // Allocate the Value object used when moving items
  Valp = AllocateValue(g, Type, len, prec, colp->IsUnsigned());
  Klen = Valp->GetClen();

  if (n[2]) {
    Bkeys.Size = n[2] * Klen;
    Bkeys.Memp = m;
    Bkeys.Sub = true;

    // Allocate the Valblk containing initial block key values
    Blkp = AllocValBlock(g, To_Bkeys, Type, n[2], len, prec, true, true);
    } // endif nb

  Keys.Size = n[0] * Klen;
  Keys.Memp = m + Bkeys.Size;
  Keys.Sub = true;

  // Allocate the Valblock. Last two parameters are to have rows filled
  // by blanks (if true) or keep the zero ending char (if false).
  // Currently we set it to true to be compatible with QRY blocks,
  // and last one to enable type checking (no conversion).
  Kblp = AllocValBlock(g, To_Keys, Type, n[0], len, prec, !Prefix, true);

  if (n[1]) {
    Koff.Size = n[1] * sizeof(int);
    Koff.Memp = m + Bkeys.Size + Keys.Size;
    Koff.Sub = true;
    } // endif n[1]

  Ndf = n[0];
//IsSorted = colp->GetOpt() < 0;
  IsSorted = false;
  Colp = colp;
  return m + Bkeys.Size + Keys.Size + Koff.Size;
  } // end of MapInit
#endif // XMAP

/***********************************************************************/
/*  Allocate the offset block used by intermediate key columns.        */
/***********************************************************************/
int *KXYCOL::MakeOffset(PGLOBAL g, int n)
  {
  if (!Kof) {
    // Calculate the initial size of the offset
    Koff.Size = (n + 1) * sizeof(int);

    // Allocate the required memory
    if (!PlgDBalloc(g, NULL, Koff)) {
      strcpy(g->Message, MSG(KEY_ALLOC_ERR));
      return NULL;    // Error
     } // endif

  } else if (n) {
    // This is a reallocation call
    PlgDBrealloc(g, NULL, Koff, (n + 1) * sizeof(int));
  } else
    PlgDBfree(Koff);

  return (int*)Kof;
  } // end of MakeOffset

/***********************************************************************/
/*  Make a front end array of key values that are the first value of   */
/*  each blocks (of size n). This to reduce paging in FastFind.        */
/***********************************************************************/
bool KXYCOL::MakeBlockArray(PGLOBAL g, int nb, int size)
  {
  int i, k;

  // Calculate the size of the block array in the index
  Bkeys.Size = nb * Klen;

  // Allocate the required memory
  if (!PlgDBalloc(g, NULL, Bkeys)) {
    sprintf(g->Message, MSG(KEY_ALLOC_ERROR), Klen, nb);
    return true;    // Error
    } // endif

  // Allocate the Valblk used to contains initial block key values
  Blkp = AllocValBlock(g, To_Bkeys, Type, nb, Klen, Kprec);

  // Populate the array with values
  for (i = k = 0; i < nb; i++, k += size)
    Blkp->SetValue(Kblp, i, k);

  return false;
  } // end of MakeBlockArray

/***********************************************************************/
/*  KXYCOL SetValue: read column value for nth array element.           */
/***********************************************************************/
void KXYCOL::SetValue(PCOL colp, int i)
  {
#if defined(_DEBUG)
  assert (Kblp != NULL);
#endif

  Kblp->SetValue(colp->GetValue(), i);
  } // end of SetValue

/***********************************************************************/
/*  InitFind: initialize finding the rank of column value in index.    */
/***********************************************************************/
bool KXYCOL::InitFind(PGLOBAL g, PXOB xp)
  {
  if (xp->GetType() == TYPE_CONST) {
    if (Kxp->Nth)
      return true;

    Valp->SetValue_pval(xp->GetValue(), !Prefix);
  } else {
    xp->Reset();
    xp->Eval(g);
    Valp->SetValue_pval(xp->GetValue(), false);
  } // endif Type

  if (trace > 1) {
    char buf[32];

    htrc("KCOL InitFind: value=%s\n", Valp->GetCharString(buf));
    } // endif trace

  return false;
  } // end of InitFind

#if 0
/***********************************************************************/
/*  InitBinFind: initialize Value to the value pointed by vp.          */
/***********************************************************************/
void KXYCOL::InitBinFind(void *vp)
  {
  Valp->SetBinValue(vp);
  } // end of InitBinFind
#endif // 0

/***********************************************************************/
/*  KXYCOL FillValue: called by COLBLK::Eval when a column value is    */
/*  already in storage in the corresponding KXYCOL.                    */
/***********************************************************************/
void KXYCOL::FillValue(PVAL valp)
  {
  valp->SetValue_pvblk(Kblp, Val_K);

  // Set null when applicable (NIY)
//if (valp->GetNullable())
//  valp->SetNull(valp->IsZero());

  } // end of FillValue

/***********************************************************************/
/*  KXYCOL: Compare routine for one numeric value.                     */
/***********************************************************************/
int KXYCOL::Compare(int i1, int i2)
  {
  // Do the actual comparison between values.
  register int k = Kblp->CompVal(i1, i2);

  if (trace > 2)
    htrc("Compare done result=%d\n", k);

  return (Asc) ? k : -k;
  } // end of Compare

/***********************************************************************/
/*  KXYCOL: Compare the ith key to the stored Value.                   */
/***********************************************************************/
int KXYCOL::CompVal(int i)
  {
  // Do the actual comparison between numerical values.
  if (trace > 2) {
    register int k = (int)Kblp->CompVal(Valp, (int)i);

    htrc("Compare done result=%d\n", k);
    return k;
  } else
    return Kblp->CompVal(Valp, i);

  } // end of CompVal

/***********************************************************************/
/*  KXYCOL: Compare the key to the stored block value.                 */
/***********************************************************************/
int KXYCOL::CompBval(int i)
  {
  // Do the actual comparison between key values.
  return Blkp->CompVal(Valp, i);
  } // end of CompBval

/***********************************************************************/
/*  KXYCOL ReAlloc: ReAlloc To_Data if it is not suballocated.         */
/***********************************************************************/
void KXYCOL::ReAlloc(PGLOBAL g, int n)
  {
  PlgDBrealloc(g, NULL, Keys, n * Klen);
  Kblp->ReAlloc(To_Keys, n);
  Ndf = n;
  } // end of ReAlloc

/***********************************************************************/
/*  KXYCOL FreeData: Free To_Keys if it is not suballocated.           */
/***********************************************************************/
void KXYCOL::FreeData(void)
  {
  PlgDBfree(Keys);
  Kblp = NULL;
  PlgDBfree(Bkeys);
  Blkp = NULL;
  PlgDBfree(Koff);
  Ndf = 0;
  } // end of FreeData
