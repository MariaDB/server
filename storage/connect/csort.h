/*************** Csort H Declares Source Code File (.H) ****************/
/*  Name: CSORT.H    Version 1.2                                       */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2000-2012    */
/*                                                                     */
/*  This file contains the CSORT class declares (not 64-bits ready)    */
/*                                                                     */
/*  Note on use of this class: This class is meant to be used as a     */
/*  base class by the calling class. This is because the comparison    */
/*  routine must belong to both CSORT and the calling class.           */
/*  This avoids to pass explicitly to it the calling "this" pointer.   */
/***********************************************************************/
#if !defined(CSORT_DEFINED)
#define      CSORT_DEFINED

#include <math.h>                   /* Required for log function       */
#undef DOMAIN                            // Was defined in math.h

/***********************************************************************/
/*  Constant and external definitions.                                 */
/***********************************************************************/
#define  THRESH    4                /* Threshold for insertion (was 4) */
#define  MTHRESH   6                /* Threshold for median            */

//extern  FILE   *debug;              /* Debug file                      */

typedef int* const    CPINT;

/***********************************************************************/
/*  This is the CSORT base class declaration.                          */
/***********************************************************************/
class DllExport CSORT {
 public:
  // Constructor
  CSORT(bool cns, int th = THRESH, int mth = MTHRESH);
  virtual ~CSORT() {}
 protected:
  // Implementation
  /*********************************************************************/
  /*  qsortx/qstx are NOT conservative but use less storage space.     */
  /*  qsortc/qstc ARE     conservative but use more storage space.     */
  /*********************************************************************/
  int  Qsortx(void);                        /* Index quick/insert sort */
  void Qstx(int *base, int *max);            /* Preliminary quick sort  */
  int  Qsortc(void);                        /* Conservative q/ins sort */
  void Qstc(int *base, int *max);            /* Preliminary quick sort  */
  void Istc(int *base, int *hi, int *max);  /* Insertion sort routine  */

 public:
  // Methods
  int Qsort(PGLOBAL g, int n);            /* Sort calling routine    */
//virtual void Printf(PGLOBAL g, FILE *f, uint n);
//virtual void Prints(PGLOBAL g, char *ps, uint z);
#ifdef DEBTRACE
  int GetNcmp(void) {return num_comp;}
#endif

 protected:
  // Overridable
  virtual int Qcompare(int *, int *) = 0;   /* Item compare routine */
#ifdef DEBTRACE
  virtual void DebugSort(int ph, int n, int *base, int *mid, int *tmp);
#endif

 public:
  // Utility
  static void SetCmpNum(void)    
    {for (int i = 1; i < 1000; i++) Cpn[i] = Cmpnum(i); Limit = 1000;}
 protected:
  static size_t Cmpnum(int n)
#if defined(AIX)
    {return (n < Limit) ? Cpn[n]
          : (size_t)round(1.0 + (double)n * (log2((double)n) - 1.0));}
#else   // !AIX
    {return (n < Limit) ? Cpn[n] 
          : (size_t)(1.5 + (double)n * (log((double)n)/Lg2 - 1.0));}
#endif  // !AIX


  // Members
  static int    Limit;                  /* Size of precalculated array */
  static size_t Cpn[1000];               /* Precalculated cmpnum values */
  static double Lg2;                    /* Precalculated log(2) value  */
  PGLOBAL G;
  PDBUSER Dup;                          /* Used for progress info      */
  bool    Cons;                         /* true for conservative sort  */
  int     Thresh;                       /* Threshold for using qsort   */
  int     Mthresh;                      /* Threshold for median find   */
  int     Nitem;                        /* Number of items to sort     */
  MBLOCK  Index;                        /* Index  allocation block     */
  MBLOCK  Offset;                        /* Offset allocation block     */
  CPINT  &Pex;                          /* Reference to sort index     */
  CPINT  &Pof;                          /* Reference to offset array   */
  int    *Swix;                         /* Pointer on EQ/GT work area  */
  int     Savmax;                        /* Saved ProgMax value         */
  int     Savcur;                        /* Saved ProgCur value         */
  LPCSTR  Savstep;                      /* Saved progress step          */
#ifdef DEBTRACE
  int     num_comp;                     /* Number of quick sort calls  */
#endif
  }; // end of class CSORT

#endif  // CSORT_DEFINED

