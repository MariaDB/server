/*************** CSort C Program Source Code File (.CPP) ***************/
/* PROGRAM NAME: CSORT                                                 */
/* -------------                                                       */
/*  Version 2.2                                                        */
/*                                                                     */
/* COPYRIGHT:                                                          */
/* ----------                                                          */
/*  (C) Copyright to the author Olivier Bertrand 1995-2016             */
/*                                                                     */
/* WHAT THIS PROGRAM DOES:                                             */
/* -----------------------                                             */
/*  This program is the C++ sorting routines that use qsort/insert     */
/*  algorithm and produces an offset/break table while sorting.        */
/*                                                                     */
/* WHAT YOU NEED TO COMPILE THIS PROGRAM:                              */
/* --------------------------------------                              */
/*                                                                     */
/*  REQUIRED FILES:                                                    */
/*  ---------------                                                    */
/*    csort.cpp      - Source code                                     */
/*                                                                     */
/*  REQUIRED LIBRARIES:                                                */
/*  -------------------                                                */
/*    OS2DEF.LIB     - OS2 libray definition subset.                   */
/*                                                                     */
/*  REQUIRED PROGRAMS:                                                 */
/*  ------------------                                                 */
/*    Microsoft C++ Compiler                                           */
/*    or GNU Compiler/Linker                                           */
/*    or BORLAND 4.5 C++ compiler                                      */
/*                                                                     */
/*  NOTE                                                               */
/*  ----                                                               */
/*    These functions are not 64-bits ready.                           */
/*                                                                     */
/***********************************************************************/

/***********************************************************************/
/*  Include relevant MariaDB header file.                              */
/***********************************************************************/
#include "my_global.h"

/***********************************************************************/
/*  Include application header files                                   */
/***********************************************************************/
#include <stdlib.h>                 /* C standard library              */
#include <string.h>                 /* String manipulation declares    */
#include <stdio.h>                  /* Required for sprintf declare    */
#if defined(_DEBUG)
#include <assert.h>                 /* Assertion routine declares      */
#endif

/***********************************************************************/
/*  Include CSort class header file                                    */
/***********************************************************************/
#include "global.h"
#include "plgdbsem.h"                /* For MBLOCK type definition      */
#include "csort.h"                  /* CSort class definition          */
#include "osutil.h"

#if !defined(BIGSORT)
#define      BIGSORT  200000
#endif   // !BIGSORT

/***********************************************************************/
/*  DB static external variables.                                      */
/***********************************************************************/
extern MBLOCK Nmblk;                /* Used to initialize MBLOCK's     */

/***********************************************************************/
/*  Initialize the CSORT static members.                               */
/***********************************************************************/
int    CSORT::Limit = 0;
double CSORT::Lg2 = log(2.0);
size_t CSORT::Cpn[1000] = {0};          /* Precalculated cmpnum values */

/***********************************************************************/
/*  CSORT constructor.                                                 */
/***********************************************************************/
CSORT::CSORT(bool cns, int th, int mth)
     : Pex((int*&)Index.Memp), Pof((int*&)Offset.Memp)
  {
  G = NULL;
  Dup =NULL;
  Cons = cns;
  Thresh = th;
  Mthresh = mth;
  Nitem = 0;
  Index = Nmblk;
  Offset = Nmblk;
  Swix = NULL;
  Savmax = 0;  
  Savcur = 0;  
  Savstep = NULL;
  } // end of CSORT constructor

/***********************************************************************/
/*  CSORT intialization.                                               */
/***********************************************************************/
int CSORT::Qsort(PGLOBAL g, int nb)
  {
  int rc;

#if defined(_DEBUG)
  assert(Index.Size >= nb * sizeof(int));
#endif

  if (nb > BIGSORT) {
    G = g;
    Dup = (PDBUSER)g->Activityp->Aptr;

    if (Dup->Proginfo) {
      Savstep = Dup->Step;
      Savmax  = Dup->ProgMax;
      Savcur  = Dup->ProgCur;

      // Evaluate the number of comparisons that we will do
      Dup->ProgMax = Cmpnum(nb);
      Dup->ProgCur = 0;
      Dup->Step = (char*)PlugSubAlloc(g, NULL, 32);
      sprintf((char*)Dup->Step, MSG(SORTING_VAL), nb);
    } else
      Dup = NULL;

  } else
    Dup = NULL;

  Nitem = nb;

  for (int n = 0; n < Nitem; n++)
    Pex[n] = n;

  rc = (Cons) ? Qsortc() : Qsortx();

  if (Dup) {
    // Restore any change in progress info settings
//  printf("Progcur=%u\n", Dup->ProgCur);

    Dup->Step    = Savstep;
    Dup->ProgMax = Savmax;
    Dup->ProgCur = Savcur;
    } // endif Subcor

  return rc;
  } // end of QSort

#if defined(DEBTRACE)
/***********************************************************************/
/*  Debug routine to be used by sort for specific data (dummy as now)  */
/***********************************************************************/
void CSORT::DebugSort(int ph, int n, int *base, int *mid, int *tmp)
  {
  htrc("phase=%d n=%d base=%p mid=%p tmp=%p\n",
          ph, n, base, mid, tmp);
  } // end of DebugSort
#endif

/***********************************************************************/
/* Qsortx:   Version adapted from qsortx.c by O.Bertrand               */
/* This version is specialy adapted for Index sorting, meaning that    */
/* the data is not moved, but the Index only is sorted.                */
/* Index array elements are any 4-byte word (a pointer or a int int   */
/* array index), they are not interpreted except by the user provided  */
/* comparison routine which must works accordingly.                    */
/* In addition, this program takes care of data in which there is a    */
/* high rate of repetitions.                                           */
/* CAUTION: the sort algorithm used here is not conservative. Equal    */
/* values will be internally stored in unpredictable order.            */
/* The THRESHold below is the insertion sort threshold, and also the   */
/* threshold for continuing que quicksort partitioning.                */
/* The MTHREShold is where we stop finding a better median.            */
/* These two quantities should be adjusted dynamically depending upon  */
/* the repetition rate of the data.                                    */
/* Algorithm used:                                                     */
/* First, set up some global parameters for Qstx to share.  Then,      */
/* quicksort with Qstx(), and then a cleanup insertion sort ourselves. */
/* Sound simple? It's not...                                           */
/***********************************************************************/
int CSORT::Qsortx(void)
  {
  int  c;
  int  lo, hi, min;
  int  i, j, rc = 0;
  // To do: rc should be checked for being used uninitialized
  int          *top;
#ifdef DEBTRACE
  int           ncp;

  num_comp = 0;
#endif

  /*********************************************************************/
  /* Prepare the Offset array that will be updated during sorts.       */
  /*********************************************************************/
  if (Pof)
    for (Pof[Nitem] = Nitem, j = 0; j < Nitem; j++)
      Pof[j] = 0;
  else
    j = Nitem + 1;

  /*********************************************************************/
  /* Sort on one or zero element is obvious.                           */
  /*********************************************************************/
  if (Nitem <= 1)
    return Nitem;

  /*********************************************************************/
  /* Thresh seems to be good as (10 * n / rep).  But for testing we    */
  /* set it directly as one parameter of the Xset function call.       */
  /* Note: this should be final as the rep parameter is no more used.  */
  /*********************************************************************/
  top = Pex + Nitem;

#ifdef DEBTRACE
 htrc("Qsortx: nitem=%d thresh=%d mthresh=%d\n",
  Nitem, Thresh, Mthresh);
#endif

  /*********************************************************************/
  /*  If applicable, do a rough preliminary quick sort.                */
  /*********************************************************************/
  if (Nitem >= Thresh)
    Qstx(Pex, top);

#ifdef DEBTRACE
 htrc(" after quick sort num_comp=%d\n", num_comp);
 ncp = num_comp;
 num_comp = 0;
#ifdef DEBUG2
 DebugSort((Pof) ? 1 : 4, Nitem, Pex, NULL, NULL);
#endif
#endif

  if (Thresh > 2) {
    if (Pof)
      /*****************************************************************/
      /*  The preliminary search for the smallest element has been     */
      /*  removed so with no sentinel in place, we must check for x    */
      /*  going below the Pof pointer.  For each remaining element    */
      /*  group from [1] to [n-1], set hi to the index of the element  */
      /*  AFTER which this one goes. Then, do the standard insertion   */
      /*  sort shift on an integer at a time basis for each equal      */
      /*  element group in the frob.                                   */
      /*****************************************************************/
      for (min = hi = 0; min < Nitem; min = hi) {
        if (Pof[hi]) {
          hi += Pof[hi];
          continue;
          } // endif Pof

        Pof[min] = 1;

#ifdef DEBUG2
 htrc("insert from min=%d\n", min);
#endif

        for (lo = hi; !Pof[++hi]; lo = hi) {
          while (lo >= min && (rc = Qcompare(Pex + lo, Pex + hi)) > 0)
            if (Pof[lo] > 0)
              lo -= Pof[lo];
            else
              return -2;

          if (++lo != hi) {
            c = Pex[hi];

            for (i = j = hi; i > 0; i = j)
              if (Pof[i - 1] <= 0)
                return -3;
              else if ((j -= Pof[i - 1]) >= lo) {
                Pex[i] = Pex[j];
                Pof[j + 1] = Pof[i] = Pof[j];
              } else
                break;

            Pex[i] = c;
            } // endif lo

          if (rc)
            Pof[lo] = 1;
          else {
            i = lo - Pof[lo - 1];
            Pof[lo] = ++Pof[i];
            } // endelse

#ifdef DEBUG2
 htrc("rc=%d lo=%d hi=%d trx=%d\n", rc, lo, hi, Pof[lo]);
#endif

          } // endfor hi

        } // endfor min

    else
      /*****************************************************************/
      /*  Call conservative insertion sort not using/setting offset.   */
      /*****************************************************************/
      Istc(Pex, Pex + MY_MIN(Nitem, Thresh), top);

    } // endif Thresh

#ifdef DEBTRACE
 htrc(" after insert sort num_comp=%d\n", num_comp);
 num_comp += ncp;
#endif

  if (Pof)
    /*******************************************************************/
    /* Reduce the Offset array.                                        */
    /*******************************************************************/
    for (i = j = 0; i <= Nitem; j++, i += c) {
#ifdef DEBUG2
 htrc(" trxp(%d)=%d trxp(%d)=%d c=%d\n",
  i, Pof[i], j, Pof[j], c);
#endif
      if ((c = Pof[i]))
        Pof[j] = i;
      else
        return -4;

      } // endfor i

  return (j - 1);
  } // end of Qsortx

/***********************************************************************/
/* Qstx:  Do a quicksort on index elements (just one int int).        */
/* First, find the median element, and put that one in the first place */
/* as the discriminator.  (This "median" is just the median of the     */
/* first, last and middle elements).  (Using this median instead of    */
/* the first element is a big win).  Then, the usual partitioning/     */
/* swapping, followed by moving the discriminator into the right place.*/
/* Element equal to the discriminator are placed against it, so the    */
/* mid (discriminator) block grows when equal elements exist. This is  */
/* a huge win in case of repartitions with few different elements.     */
/* The mid block being at its final position, its first and last       */
/* elements are marked in the offset list (used to make break list).   */
/* Then, figure out the sizes of the two partitions, do the smaller    */
/* one recursively and the larger one via a repeat of this code.       */
/* Stopping when there are less than THRESH elements in a partition    */
/* and cleaning up with an insertion sort (in our caller) is a huge    */
/* win(?). All data swaps are done in-line, which is space-losing but  */
/* time-saving. (And there are only three places where this is done).  */
/***********************************************************************/
void CSORT::Qstx(int *base, int *max)
  {
  int *i, *j, *jj, *mid, *him, c;
  int          *tmp;
  int           lo, hi, rc;
  size_t        zlo, zhi, cnm;

  zlo = zhi = cnm = 0;                  // Avoid warning message

  lo = (int)(max - base);                      // Number of elements as longs

  if (Dup)
    cnm = Cmpnum(lo);

  do {
    /*******************************************************************/
    /* At the top here, lo is the number of integers of elements in    */
    /* the current partition.  (Which should be max - base).           */
    /* Find the median of the first, last, and middle element and make */
    /* that the middle element.  Set j to largest of first and middle. */
    /* If max is larger than that guy, then it's that guy, else        */
    /* compare max with loser of first and take larger.  Things are    */
    /* set up to prefer the middle, then the first in case of ties.    */
    /* In addition, hi and rc are set to comparison results. So if hi  */
    /* is null, the two high values are equal and if rc is null, the   */
    /* two low values are equal. This was used to set which test will  */
    /* be made by LE and which one by LT (does not apply anymore).     */
    /*******************************************************************/
    him = mid = i = base + (lo >> 1);
    hi = rc = 0;

#ifdef DEBTRACE
 tmp = max - 1;
 htrc("--> block base=%d size=%d\n", base - Pex, lo);
 DebugSort(2, 0, base, mid, tmp);
#endif

    if (lo >= Mthresh) {
      rc = Qcompare((jj = base), i);
      j = (rc > 0) ? jj : i;
      hi = Qcompare(j, (tmp = max - 1));

      if (hi > 0 && rc) {
        j = (j == jj) ? i : jj;               // switch to first loser

        if ((rc = Qcompare(j, tmp)) < 0)
          j = tmp;

        } // endif

      if (j != i) {
        c = *i;
        *i = *j;
        *j = c;
        } // endif j

    } else if (lo == 2) {
      /*****************************************************************/
      /*  Small group. Do special quicker processing.                  */
      /*****************************************************************/
      if ((rc = Qcompare(base, (him = base + 1))) > 0)
        c = *base, *base = *him, *him = c;

      if (Pof)
        Pof[base - Pex] = Pof[him - Pex] = (rc) ? 1 : 2;

      break;
    } // endif lo

#ifdef DEBTRACE
 DebugSort(3, hi, NULL, mid, &rc);
#endif

    /*******************************************************************/
    /*  Semi-standard quicksort partitioning/swapping.  Added here is  */
    /*  a test on equality.  All values equal to the mid element are   */
    /*  placed under or over it.  Mid block can be also moved when it  */
    /*  is necessary because the other partition is full.  At the end  */
    /*  of the for loop the mid block is definitely positionned.       */
    /*******************************************************************/
    for (i = base, j = max - 1; ;) {
     CONT:
      while (i < mid)
        if ((rc = Qcompare(i, mid)) < 0)
          i++;
        else if (!rc) {
          c = *i;
          *i = *(--mid);
          *mid = c;
        } else
          break;

      while (j > him)
        if ((rc = Qcompare(him, j)) < 0)
          j--;
        else if (!rc) {
          c = *j;
          *j = *(++him);
          *him = c;
        } else if (i == mid) {              // Triple move:
          c = *j;                           // j goes under mid block
          *j = *(++him);                    // val over mid block -> j
          *him = *mid++;                    // and mid block goes one
          *i++ = c;                         // position higher.
        } else {                            // i <-> j
          c = *i;
          *i++ = *j;
          *j-- = c;
          goto CONT;
        } // endif's

      if (i == mid)
        break;
      else {                                // Triple move:
        c = *i;                             // i goes over mid block
        *i = *(--mid);                      // val under mid block -> i
        *mid = *him--;                      // and mid block goes one
        *j-- = c;                           // position lower.
        } // endelse

      } // endfor i

    /*******************************************************************/
    /* The mid block being placed at its final position we can now set */
    /* the offset array values indicating break point and block size.  */
    /*******************************************************************/
    j = mid;
    i = him + 1;

    if (Pof)
      Pof[him - Pex] = Pof[mid - Pex] = (int)(i - j);

    /*******************************************************************/
    /* Look at sizes of the two partitions, do the smaller one first   */
    /* by recursion, then do the larger one by making sure lo is its   */
    /* size, base and max are update correctly, and branching back.    */
    /* But only repeat (recursively or by branching) if the partition  */
    /* is of at least size THRESH.                                     */
    /*******************************************************************/
    lo = (int)(j - base);
    hi = (int)(max - i);

    if (Dup) {                         // Update progress information
      zlo = Cmpnum(lo);
      zhi = Cmpnum(hi);
      Dup->ProgCur += cnm - (zlo + zhi);
      } // endif Dup

#ifdef DEBTRACE
 htrc(" done lo=%d sep=%d hi=%d\n", lo, i - j, hi);
#endif

    if (lo <= hi) {
      if (lo >= Thresh)
        Qstx(base, j);
      else if (lo == 1 && Pof)
        Pof[base - Pex] = 1;

      base = i;
      lo = hi;
      cnm = zhi;
    } else {
      if (hi >= Thresh)
        Qstx(i, max);
      else if (hi == 1 && Pof)
        Pof[i - Pex] = 1;

      max = j;
      cnm = zlo;
    } // endif

    if (lo == 1 && Pof)
      Pof[base - Pex] = 1;

    } while (lo >= Thresh); // enddo

  } // end of Qstx

/***********************************************************************/
/*  Qsortc.c:   Version adapted from qsort.c by O.Bertrand             */
/*  This version is specialy adapted for Index sorting, meaning that   */
/*  the data is not moved, but the Index only is sorted.               */
/*  Index array elements are any 4-byte word (a pointer or a int int  */
/*  array index), they are not interpreted except by the user provided */
/*  comparison routine which must works accordingly.                   */
/*  In addition, this program takes care of data in which there is a   */
/*  high rate of repetitions.                                          */
/*  NOTE: the sort algorithm used here is conservative. Equal and      */
/*  greater than values are internally stored in additional work area. */
/*  The THRESHold below is the insertion sort threshold, and also the  */
/*  threshold for continuing que quicksort partitioning.               */
/*  The MTHREShold is where we stop finding a better median.           */
/*  These two quantities should be adjusted dynamically depending upon */
/*  the repetition rate of the data.                                   */
/*  Algorithm used:                                                    */
/*  First, set up some global parameters for Qstc to share.  Then,     */
/*  quicksort with Qstc(), and then a cleanup insertion sort ourselves.*/
/*  Sound simple? It's not...                                          */
/***********************************************************************/
int CSORT::Qsortc(void)
  {
  int  c;
  int  lo, hi, min;
  int  i, j, k, m, rc = 0;
  // To do: rc should be checked for being used uninitialized
  int          *max;
#ifdef DEBTRACE
  int           ncp;

  num_comp = 0;
#endif

  /*********************************************************************/
  /* Prepare the Offset array that will be updated during sorts.       */
  /*********************************************************************/
  if (Pof)
    for (Pof[Nitem] = Nitem, j = 0; j < Nitem; j++)
      Pof[j] = 0;
  else
    j = Nitem + 1;

  /*********************************************************************/
  /* Sort on one or zero element is obvious.                           */
  /*********************************************************************/
  if (Nitem <= 1)
    return Nitem;

  /*********************************************************************/
  /* Thresh seems to be good as (10 * n / rep).  But for testing we    */
  /* set it directly as one parameter of the Xset function call.       */
  /* Note: this should be final as the rep parameter is no more used.  */
  /*********************************************************************/
  max = Pex + Nitem;

#ifdef DEBTRACE
 htrc("Qsortc: nitem=%d thresh=%d mthresh=%d\n",
  Nitem, Thresh, Mthresh);
#endif

  /*********************************************************************/
  /*  If applicable, do a rough preliminary conservative quick sort.   */
  /*********************************************************************/
  if (Nitem >= Thresh) {
    if (!(Swix = (int *)malloc(Nitem * sizeof(int))))
      return -1;

    Qstc(Pex, max);

    free(Swix);
    Swix = NULL;
    } // endif n

#ifdef DEBTRACE
 htrc(" after quick sort num_comp=%d\n", num_comp);
 ncp = num_comp;
 num_comp = 0;
#ifdef DEBUG2
 DebugSort((Pof) ? 1 : 4, Nitem, Pex, NULL, NULL);
#endif
#endif

  if (Thresh > 2) {
    if (Pof)
      /*****************************************************************/
      /*  The preliminary search for the smallest element has been     */
      /*  removed so with no sentinel in place, we must check for x    */
      /*  going below the Pof pointer.  For each remaining element    */
      /*  group from [1] to [n-1], set hi to the index of the element  */
      /*  AFTER which this one goes. Then, do the standard insertion   */
      /*  sort shift on an integer at a time basis for each equal      */
      /*  element group in the frob.                                   */
      /*****************************************************************/
      for (min = hi = 0; min < Nitem; min = hi) {
        if (Pof[hi]) {
          hi += Pof[hi];
          continue;
          } // endif

        Pof[min] = 1;

#ifdef DEBUG2
 htrc("insert from min=%d\n", min);
#endif

        for (lo = hi; !Pof[++hi]; lo = hi) {
          while (lo >= min && (rc = Qcompare(Pex + lo, Pex + hi)) > 0)
            if (Pof[lo] > 0)
              lo -= Pof[lo];
            else
              return -2;

          if (++lo != hi) {
            c = Pex[hi];

            for (i = j = hi; i > 0; i = j)
              if (Pof[i - 1] <= 0)
                return -3;
              else if ((j -= Pof[i - 1]) >= lo) {
                for (k = m = i; --m >= j; k--)    // Move intermediate
                  Pex[k] = Pex[m];              // for conservation.

                Pof[j + 1] = Pof[i] = Pof[j];
              } else
                break;

            Pex[i] = c;
            } // endif

          if (rc)
            Pof[lo] = 1;
          else {
            i = lo - Pof[lo - 1];
            Pof[lo] = ++Pof[i];
            } // endelse

#ifdef DEBUG2
 htrc("rc=%d lo=%d hi=%d ofx=%d\n", rc, lo, hi, Pof[lo]);
#endif

          } // endfor hi

        } // endfor min

    else
      /*****************************************************************/
      /*  Call conservative insertion sort not using/setting offset.   */
      /*****************************************************************/
      Istc(Pex, Pex + MY_MIN(Nitem, Thresh), max);

    } // endif Thresh

#ifdef DEBTRACE
 htrc(" after insert sort num_comp=%d\n", num_comp);
 num_comp += ncp;
#endif

  if (Pof)
    /*******************************************************************/
    /* Reduce the Offset array.                                        */
    /*******************************************************************/
    for (i = j = 0; i <= Nitem; j++, i += c) {
#ifdef DEBUG2
 htrc(" Pof(%d)=%d Pof(%d)=%d c=%d\n",
  i, Pof[i], j, Pof[j], c);
#endif
      if ((c = Pof[i]))
        Pof[j] = i;
      else
        return -4;

      } // endfor i

  return (j - 1);
  } // end of Qsortc

/***********************************************************************/
/*  Qstc:  Do a quicksort on index elements (just one int int).       */
/*  First, find the median element, and set it as the discriminator.   */
/*  (This "median" is just the median of the first, last and middle    */
/*  elements).  (Using this median instead of the first element is a   */
/*  big win).  Then, the special partitioning/swapping, where elements */
/*  smaller than the discriminator are placed in the sorted block,     */
/*  elements equal to the discriminator are placed backward from the   */
/*  top of the work area and elements greater than *j (discriminator)  */
/*  are placed in the work area from its bottom. Then the elements in  */
/*  the work area are placed back in the sort area in natural order,   */
/*  making the sort conservative. Non equal blocks shrink faster when  */
/*  equal elements exist. This is a huge win in case of repartitions   */
/*  with few different elements. The mid block being at its final      */
/*  position, its first and last elements are marked in the offset     */
/*  list (used to make break list). Then, figure out the sizes of the  */
/*  two partitions, do the smaller one recursively and the larger one  */
/*  via a repeat of this code. Stopping when there are less than       */
/*  THRESH elements in a partition and cleaning up with an insertion   */
/*  sort (in our caller) is a huge win (yet to be proved?).            */
/***********************************************************************/
void CSORT::Qstc(int *base, int *max)
  {
  int *i, *j, *jj, *lt, *eq, *gt, *mid;
  int           c = 0, lo, hi, rc;
  size_t        zlo, zhi, cnm;

  zlo = zhi = cnm = 0;                  // Avoid warning message

  lo = (int)(max - base);                      // Number of elements as longs

  if (Dup)
    cnm = Cmpnum(lo);

  do {
    /*******************************************************************/
    /*  At the top here, lo is the number of integers of elements in   */
    /*  the current partition. (Which should be max - base). Find the  */
    /*  median of the first, last, and middle element and make that    */
    /*  the compare element. Set jj to smallest of middle and last.    */
    /*  If base is smaller or equal than that guy, then it's that guy, */
    /*  else compare base with loser of first and take smaller. Things */
    /*  are set up to prefer the top, then the middle in case of ties. */
    /*******************************************************************/
    i = base + (lo >> 1);
    jj = mid = max - 1;

#ifdef DEBTRACE
 htrc("--> block base=%d size=%d\n", base - Pex, lo);
 DebugSort(2, 0, base, i, mid);
#endif

    if (lo >= Mthresh) {
      jj = ((rc = Qcompare(i, mid)) < 0) ? i : mid;

      if (rc && Qcompare(base, jj) > 0) {
        jj = (jj == mid) ? i : mid;           // switch to first loser

        if (Qcompare(base, jj) < 0)
          jj = base;

        } // endif

      if (jj != mid) {
        /***************************************************************/
        /*  The compare element must be at the top of the block so it  */
        /*  cannot be overwritten while making the partitioning.  So   */
        /*  save the last block value which will be compared later.    */
        /***************************************************************/
        c = *mid;
        *mid = *jj;
        } // endif

    } else if (lo == 2) {
      /*****************************************************************/
      /*  Small group. Do special quicker processing.                  */
      /*****************************************************************/
			if ((rc = Qcompare(base, (i = base + 1))) > 0) {
				c = *base;
				*base = *i;
				*i = c;
			}	// endif rc

      if (Pof)
        Pof[base - Pex] = Pof[i - Pex] = (rc) ? 1 : 2;

      break;
    } // endif lo

#ifdef DEBTRACE
 DebugSort(3, lo, NULL, jj, &rc);
#endif

    /*******************************************************************/
    /*  Non-standard quicksort partitioning using additional storage   */
    /*  to store values less than, equal or greater than the middle    */
    /*  element. This uses more memory but provides conservation of    */
    /*  the equal elements order.                                      */
    /*******************************************************************/
    lt = base;
    eq = Swix + lo;
    gt = Swix;

    if (jj == mid) {
      /*****************************************************************/
      /*  Compare element was last.  No problem.                       */
      /*****************************************************************/
      for (i = base; i < max; i++)
        if ((rc = Qcompare(i, mid)) < 0)
          *lt++ = *i;
        else if (rc > 0)
          *gt++ = *i;
        else
          *--eq = *i;

    } else {
      /*****************************************************************/
      /*  Compare element was not last and was copied to top of block. */
      /*****************************************************************/
      for (i = base; i < mid; i++)
        if ((rc = Qcompare(i, mid)) < 0)
          *lt++ = *i;
        else if (rc > 0)
          *gt++ = *i;
        else
          *--eq = *i;

      /*****************************************************************/
      /*  Restore saved last value and do the comparison from there.   */
      /*****************************************************************/
      *--i = c;

      if ((rc = Qcompare(i, mid)) < 0)
        *lt++ = *i;
      else if (rc > 0)
        *gt++ = *i;
      else
        *--eq = *i;

    } // endif

    /*******************************************************************/
    /* Now copy the equal and greater values back in the main array in */
    /* the same order they have been placed in the work area.          */
    /*******************************************************************/
    for (j = Swix + lo, i = lt; j > eq; )
      *i++ = *--j;

    for (j = Swix, jj = i; j < gt; )
      *i++ = *j++;

    /*******************************************************************/
    /* The mid block being placed at its final position we can now set */
    /* the offset array values indicating break point and block size.  */
    /*******************************************************************/
    if (Pof)
      Pof[lt - Pex] = Pof[(jj - 1) - Pex] = (int)(jj - lt);

    /*******************************************************************/
    /* Look at sizes of the two partitions, do the smaller one first   */
    /* by recursion, then do the larger one by making sure lo is its   */
    /* size, base and max are update correctly, and branching back.    */
    /* But only repeat (recursively or by branching) if the partition  */
    /* is of at least size THRESH.                                     */
    /*******************************************************************/
    lo = (int)(lt - base);
    hi = (int)(gt - Swix);

    if (Dup) {                         // Update progress information
      zlo = Cmpnum(lo);
      zhi = Cmpnum(hi);
      Dup->ProgCur += cnm - (zlo + zhi);
      } // endif Dup

#ifdef DEBTRACE
 htrc(" done lo=%d hi=%d\n",
   lo, /*Swix + lt - base - eq,*/ hi);
#endif

    if (lo <= hi) {
      if (lo >= Thresh)
        Qstc(base, lt);
      else if (lo == 1 && Pof)
        Pof[base - Pex] = 1;

      base = jj;
      lo = hi;
      cnm = zhi;
    } else {
      if (hi >= Thresh)
        Qstc(jj, max);
      else if (hi == 1 && Pof)
        Pof[jj - Pex] = 1;

      max = lt;
      cnm = zlo;
    } // endif

    if (lo == 1 && Pof)
      Pof[base - Pex] = 1;

    } while (lo >= Thresh); // enddo

  } // end of Qstc

/***********************************************************************/
/*  Conservative insertion sort not using/setting offset array.        */
/***********************************************************************/
void CSORT::Istc(int *base, int *hi, int *max)
  {
  int  c = 0;
  int *lo;
  int *i, *j;

  /*********************************************************************/
  /*  First put smallest element, which must be in the first THRESH,   */
  /*  in the first position as a sentinel.  This is done just by       */
  /*  searching the 1st THRESH elements (or the 1st n if n < THRESH)   */
  /*  finding the min, and shifting it into the first position.        */
  /*********************************************************************/
  for (j = lo = base; ++lo < hi; )
    if (Qcompare(j, lo) > 0)
      j = lo;

  if (j != base) {                               // shift j into place
    c = *j;

    for (i = j; --j >= base; i = j)
      *i = *j;

    *base = c;
    } // endif j

#ifdef DEBTRACE
 htrc("sentinel %d in place, base=%p hi=%p max=%p\n",
  c, base, hi, max);
#endif

  /*********************************************************************/
  /*  With our sentinel in place, we now run the following hyper-      */
  /*  fast insertion sort.  For each remaining element, lo, from [1]   */
  /*  to [n-1], set hi to the index of the element AFTER which this    */
  /*  one goes. Then, do the standard insertion sort shift for each    */
  /*  element in the frob.                                             */
  /*********************************************************************/
  for (lo = base; (hi = ++lo) < max;) {
    while (Qcompare(--hi, lo) > 0) ;

#ifdef DEBUG2
 htrc("after while: hi(%p)=%d lo(%p)=%d\n",
  hi, *hi, lo, *lo);
#endif

    if (++hi != lo) {
      c = *lo;

      for (i = j = lo; --j >= hi; i = j)
        *i = *j;

      *i = c;
      } // endif hi

    } // endfor lo

  } // end of Istc

/* -------------------------- End of CSort --------------------------- */
