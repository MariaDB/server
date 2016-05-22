/*************** Xindex H Declares Source Code File (.H) ***************/
/*  Name: XINDEX.H    Version 3.5                                      */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2004 - 2015  */
/*                                                                     */
/*  This file contains the XINDEX class declares.                      */
/***********************************************************************/
#ifndef __XINDEX_H__
#define  __XINDEX_H__
#include "block.h"
#include "csort.h"            /* Base class declares                   */
#include "xtable.h"
#include "valblk.h"
#if defined(XMAP)
#include "maputil.h"
#endif   // XMAP

enum IDT {TYPE_IDX_ERROR = 0,         /* Type not defined              */
          TYPE_IDX_INDX  = 4,         /* Permanent standard index      */
          TYPE_IDX_XROW  = 5};        /* Permanent row index           */

#if defined(XMAP)
typedef        MEMMAP   *MMP;
#endif   // XMAP
typedef class  INDEXDEF *PIXDEF;
typedef class  KPARTDEF *PKPDEF;
typedef class  XINDEX   *PXINDEX;
typedef class  XLOAD    *PXLOAD;
typedef class  KXYCOL   *PXCOL;

/***********************************************************************/
/*  Structures used when checking for possible indexing                */
/***********************************************************************/
typedef struct index_col *PICOL;
typedef struct index_val *PIVAL;
typedef struct index_def *PINDX;
typedef struct indx_used *PXUSED;

typedef struct index_val : public BLOCK {
  index_val(PXOB xp) {Next = NULL; Xval = xp; Kp = NULL;}
  PIVAL  Next;                    // Next value
  PXOB   Xval;                    // To value or array
  int   *Kp;                      // The coordonates in a LSTBLK
  } IVAL;

typedef struct index_col : public BLOCK {
  index_col(PCOL cp)
    {Next = Nxtgrp = NULL; Colp = cp; Ngrp = N = 0; Vals = NULL;}
  PICOL  Next;                    // Next column
  PICOL  Nxtgrp;                  // Next group
  PCOL   Colp;                    // The column
  PIVAL  Vals;                    // To column values
  int    Ngrp;                    // Group  number of values
  int    N;                       // Column number of values
  } ICOL;

typedef struct index_def : public BLOCK {
  index_def(PIXDEF xdp)
    {Next = NULL; Pxdf = xdp; Cols = NULL; Alloc = false;}
  PINDX  Next;
  PIXDEF Pxdf;
  PICOL  Cols;
  bool   Alloc;                   // Must allocate values
  } INDX;

typedef struct index_off {
  union {
#if defined(WORDS_BIGENDIAN)
    struct {int High; int Low;} v;
#else   // !WORDS_BIGENDIAN
    struct {int Low; int High;} v;
#endif   //!WORDS_BIGENDIAN
    longlong Val;                 // File position
    }; // end of union
  } IOFF;

/***********************************************************************/
/*  Index definition block.                                            */
/***********************************************************************/
class DllExport INDEXDEF : public BLOCK { /* Index description block   */
  friend class PLUGCAT;
  friend class DOSDEF;
  friend class ha_connect;
  friend int PlgMakeIndex(PGLOBAL g, PSZ name, PIXDEF pxdf, bool add);
 public:
  // Constructor
  INDEXDEF(char *name, bool uniq = false, int n = 0);

  // Implementation
  PIXDEF  GetNext(void) {return Next;}
  void    SetNext(PIXDEF pxdf) {Next = pxdf;}
  PSZ     GetName(void) {return (PSZ)Name;}
  bool    IsUnique(void) {return Unique;}
  bool    IsDynamic(void) {return Dynamic;}
  bool    IsAuto(void) {return AutoInc;}
  bool    IsValid(void) {return !Invalid;}
  void    SetAuto(bool b) {AutoInc = b;}
  void    SetInvalid(bool b) {Invalid = b;}
  int     GetNparts(void) {return Nparts;}
  int     GetID(void) {return ID;}
  void    SetID(int n) {ID = n;}
  PKPDEF  GetToKeyParts(void) {return ToKeyParts;}
  void    SetToKeyParts(PKPDEF kp) {ToKeyParts = kp;}
  void    SetNParts(uint np) {Nparts = (signed)np;}
  void    SetMaxSame(int mxs) {MaxSame = mxs;}
  void    SetMxsame(PXINDEX x);
  int     GetMaxSame(void) {return MaxSame;}
  bool    Define(PGLOBAL g, void *memp, PTABDEF dfp, LPCSTR p);
  PIXDEF  GetIndexOf(PCOL colp, bool hd = false);
  int     IsIndexOf(PCOL colp);
  PKXBASE CheckIndexing(PGLOBAL g, PTDBDOS tdbp);
  PINDX   CheckAND(PGLOBAL g, PINDX pix1, PINDX pix2);
  PINDX   CheckOR(PGLOBAL g, PINDX pix1, PINDX pix2);
  PINDX   CheckEQ(PGLOBAL g, PTDB tdbp, PXOB *arg, int op, int *kp = NULL);
  bool    TestEQ(PGLOBAL g, PTDB tdbp, PXOB *arg, int op, bool b = false);

 protected:
  PIXDEF  Next;               /* To next block                         */
  PKPDEF  ToKeyParts;         /* To the key part definitions           */
  char   *Name;               /* Index name                            */
  bool    Unique;             /* true if defined as unique             */
  bool    Invalid;            /* true if marked as Invalid             */
  bool    AutoInc;            /* true if unique key in auto increment  */
  bool    Dynamic;            /* KINDEX style                          */
  bool    Mapped;             /* Use file mapping                      */
  int     Nparts;             /* Number of key parts                   */
  int     ID;                 /* Index ID number                       */
  int     MaxSame;            /* Max number of same values             */
  }; // end of INDEXDEF

typedef struct indx_used : public BLOCK {
  indx_used(PTDB tp, PIXDEF xdp, PCOL *cp, int k)
  {Tname = (char*)tp->GetName(); Xname = xdp->GetName(); Cp = cp; K = k;}
  PXUSED Next;
  char  *Tname;
  PSZ    Xname;
  PCOL  *Cp;
  int    K;
  } XUSED;

/***********************************************************************/
/*  Index Key Part definition block.                                   */
/***********************************************************************/
class DllExport KPARTDEF : public BLOCK { /* Index Key Part desc block */
  friend class INDEXDEF;
  friend class XINDEX;
  friend class PLUGCAT;
  friend class DOSDEF;
  friend class ha_connect;
  friend int PlgMakeIndex(PGLOBAL g, PSZ name, PIXDEF pxdf, bool add);
 public:
  KPARTDEF(PSZ name, int n);       // Constructor

  // Implementation
  PKPDEF GetNext(void) {return Next;}
  PSZ    GetName(void) {return (PSZ)Name;}
  int    GetNcol(void) {return Ncol;}
  void   SetNext(PKPDEF pkdf) {Next = pkdf;}
  void   SetKlen(int len) {Klen = len;}
  void   SetMxsame(int mxs) {Mxsame = mxs;}

 protected:
  PKPDEF Next;                /* To next block                         */
  PSZ    Name;                /* Field name                            */
  int    Mxsame;              /* Field max same values                 */
  int    Ncol;                /* Field number                          */
  int    Klen;                /* Key length                            */
  }; // end of KPARTDEF

/***********************************************************************/
/*  This is the XDB Index virtual base class declaration.              */
/***********************************************************************/
class DllExport XXBASE : public CSORT, public BLOCK {
  friend class INDEXDEF;
  friend class KXYCOL;
 public:
  // Constructor
  XXBASE(PTDBDOS tbxp, bool b);

  // Implementation
  virtual IDT  GetType(void) = 0;
  virtual void Reset(void) = 0;
  virtual bool IsMul(void) {return false;}
  virtual bool IsRandom(void) {return true;}
  virtual bool IsDynamic(void) {return Dynamic;}
  virtual void SetDynamic(bool dyn) {Dynamic = dyn;}
  virtual bool HaveSame(void) {return false;}
  virtual int  GetCurPos(void) {return Cur_K;}
  virtual void SetNval(int n) {assert(n == 1);}
  virtual void SetOp(OPVAL op) {Op = op;}
          int  GetNdif(void) {return Ndif;}
          int  GetNum_K(void) {return Num_K;}
          int  GetCur_K(void) {return Cur_K;}
          int  GetID(void) {return ID;}
          void SetID(int id) {ID = id;}
          void SetNth(int n) {Nth = n;}
          int *GetPof(void) {return Pof;}
          int *GetPex(void) {return Pex;}
          bool IsSorted(void) {return Srtd;}
          void FreeIndex(void) {PlgDBfree(Index);}

  // Methods
  virtual void Print(PGLOBAL g, FILE *f, uint n);
  virtual void Print(PGLOBAL g, char *ps, uint z);
  virtual bool Init(PGLOBAL g) = 0;
  virtual bool Make(PGLOBAL g, PIXDEF sxp) = 0;
#if defined(XMAP)
  virtual bool MapInit(PGLOBAL g) = 0;
#endif   // XMAP
  virtual int  MaxRange(void) {return 1;}
  virtual int  Fetch(PGLOBAL g) = 0;
  virtual bool NextVal(bool) {return true;}
  virtual bool PrevVal(void) {return true;}
  virtual int  FastFind(void) = 0;
  virtual bool Reorder(PGLOBAL) {return true;}
  virtual int  Range(PGLOBAL, int = 0, bool = true) {return -1;} // Means error
  virtual int  Qcompare(int *, int *) = 0;
  virtual int  GroupSize(void) {return 1;}
  virtual void Close(void) = 0;

 protected:
  // Members
  PTDBASE Tbxp;             // Points to calling table TDB
  PXCOL   To_KeyCol;        // To KeyCol class list
  MBLOCK  Record;           // Record allocation block
  int*   &To_Rec;           // We are using ftell, fseek
  int     Cur_K;            // Index of current record
  int     Old_K;            // Index of last record
  int     Num_K;            // Size of Rec_K pointer array
  int     Ndif;             // Number of distinct values
  int     Bot;              // Bottom of research index
  int     Top;              // Top    of research index
  int     Inf, Sup;         // Used for block optimization
  OPVAL   Op;               // Search operator
  bool    Mul;              // true if multiple
  bool    Srtd;             // true for sorted column
  bool    Dynamic;          // true when dynamically made
  int     Val_K;            // Index of current value
  int     Nblk;             // Number of blocks
  int     Sblk;             // Block size
  int     Thresh;           // Thresh for sorting join indexes
  int     ID;               // Index ID number
  int     Nth;              // Nth constant to fetch
  }; // end of class XXBASE

/***********************************************************************/
/*  This is the standard (multicolumn) Index class declaration.        */
/***********************************************************************/
class DllExport XINDEX : public XXBASE {
  friend class KXYCOL;
 public:
  // Constructor
  XINDEX(PTDBDOS tdbp, PIXDEF xdp, PXLOAD pxp,
                       PCOL *cp, PXOB *xp = NULL, int k = 0);

  // Implementation
  virtual IDT  GetType(void) {return TYPE_IDX_INDX;}
  virtual bool IsMul(void) {return (Nval < Nk) ? true : Mul;}
  virtual bool HaveSame(void) {return Op == OP_SAME;}
  virtual int  GetCurPos(void) {return (Pex) ? Pex[Cur_K] : Cur_K;}
  virtual void SetNval(int n) {Nval = n;}
          int  GetMaxSame(void) {return MaxSame;}

  // Methods
  virtual void Reset(void);
  virtual bool Init(PGLOBAL g);
#if defined(XMAP)
  virtual bool MapInit(PGLOBAL g);
#endif   // XMAP
  virtual int  Qcompare(int *, int *);
  virtual int  Fetch(PGLOBAL g);
  virtual int  FastFind(void);
  virtual int  GroupSize(void);
  virtual int  Range(PGLOBAL g, int limit = 0, bool incl = true);
  virtual int  MaxRange(void) {return MaxSame;}
  virtual int  ColMaxSame(PXCOL kp);
  virtual void Close(void);
  virtual bool NextVal(bool eq);
  virtual bool PrevVal(void);
  virtual bool Make(PGLOBAL g, PIXDEF sxp);
  virtual bool SaveIndex(PGLOBAL g, PIXDEF sxp);
  virtual bool Reorder(PGLOBAL g);
          bool GetAllSizes(PGLOBAL g,/* int &ndif,*/ int &numk);

 protected:
          bool AddColumns(void);
          bool NextValDif(void);

  // Members
  PIXDEF  Xdp;              // To index definition
  PTDBDOS Tdbp;             // Points to calling table TDB
  PXLOAD  X;                // To XLOAD class
  PXCOL   To_LastCol;       // To the last key part block
  PXCOL   To_LastVal;       // To the last used key part block
  PCOL   *To_Cols;          // To array of indexed columns
  PXOB   *To_Vals;          // To array of column values
  int     Nk;               // The number of indexed columns
  int     Nval;             // The number of used columns
  int     Incr;             // Increment of record position
  int     MaxSame;          // Max number of same values
  }; // end of class XINDEX

/***********************************************************************/
/*  This is the fast single column index class declaration.            */
/***********************************************************************/
class DllExport XINDXS : public XINDEX {
  friend class KXYCOL;
 public:
  // Constructor
  XINDXS(PTDBDOS tdbp, PIXDEF xdp, PXLOAD pxp, PCOL *cp, PXOB *xp = NULL);

  // Implementation
  virtual void SetNval(int n) {assert(n == 1);}

  // Methods
  virtual int  Qcompare(int *, int *);
  virtual int  Fetch(PGLOBAL g);
  virtual int  FastFind(void);
  virtual bool NextVal(bool eq);
  virtual bool PrevVal(void);
  virtual int  Range(PGLOBAL g, int limit = 0, bool incl = true);
  virtual int  GroupSize(void);

 protected:
  // Members
  }; // end of class XINDXS

/***********************************************************************/
/*  This is the saving/loading index utility base class.               */
/***********************************************************************/
class DllExport XLOAD : public BLOCK {
  friend class XINDEX;
  friend class XBIGEX;
  friend class XBIGXS;
 public:
  // Constructor
  XLOAD(void);

  // Methods
  virtual bool  Open(PGLOBAL g, char *filename, int id, MODE mode) = 0;
  virtual bool  Seek(PGLOBAL g, int low, int high, int origin) = 0;
  virtual bool  Read(PGLOBAL g, void *buf, int n, int size) = 0;
  virtual int   Write(PGLOBAL g, void *buf, int n,
                                            int size, bool& rc) = 0;
  virtual void  Close(char *fn, int id) = 0;
  virtual void  Close(void);
#if defined(XMAP)
  virtual void *FileView(PGLOBAL g, char *fn) = 0;
#endif   // XMAP

 protected:
  // Members
#if defined(__WIN__)
  HANDLE  Hfile;                // Handle to file or map
#else    // UNIX
  int     Hfile;                // Descriptor to file or map
#endif   // UNIX
  IOFF    NewOff;               // New offset
  }; // end of class XLOAD

/***********************************************************************/
/*  This is the saving/loading indexes utility class.                  */
/***********************************************************************/
class DllExport XFILE : public XLOAD {
 public:
  // Constructor
  XFILE(void);

  // Methods
  virtual bool  Open(PGLOBAL g, char *filename, int id, MODE mode);
  virtual bool  Seek(PGLOBAL g, int low, int high, int origin);
  virtual bool  Read(PGLOBAL g, void *buf, int n, int size);
  virtual int   Write(PGLOBAL g, void *buf, int n, int size, bool& rc);
  virtual void  Close(char *fn, int id);
  virtual void  Close(void);
#if defined(XMAP)
  virtual void *FileView(PGLOBAL g, char *fn);
#endif   // XMAP

 protected:
  // Members
  FILE   *Xfile;                // Index stream file
#if defined(XMAP)
  MMP     Mmp;                  // Mapped view base address and length
#endif   // XMAP
  }; // end of class XFILE

/***********************************************************************/
/*  This is the saving/loading huge indexes utility class.             */
/***********************************************************************/
class DllExport XHUGE : public XLOAD {
 public:
  // Constructor
  XHUGE(void) : XLOAD() {}

  // Methods
  using XLOAD::Close;
  virtual bool  Open(PGLOBAL g, char *filename, int id, MODE mode);
  virtual bool  Seek(PGLOBAL g, int low, int high, int origin);
  virtual bool  Read(PGLOBAL g, void *buf, int n, int size);
  virtual int   Write(PGLOBAL g, void *buf, int n, int size, bool& rc);
  virtual void  Close(char *fn, int id);
#if defined(XMAP)
  virtual void *FileView(PGLOBAL g, char *fn);
#endif   // XMAP

 protected:
  // Members
  }; // end of class XHUGE

/***********************************************************************/
/*  This is the XDB index for columns containing ROWID values.         */
/***********************************************************************/
class DllExport XXROW : public XXBASE {
  friend class KXYCOL;
 public:
  // Constructor
  XXROW(PTDBDOS tbxp);

  // Implementation
  virtual IDT  GetType(void) {return TYPE_IDX_XROW;}
  virtual void Reset(void);

  // Methods
  virtual bool Init(PGLOBAL g);
#if defined(XMAP)
  virtual bool MapInit(PGLOBAL) {return true;}
#endif   // XMAP
  virtual int  Fetch(PGLOBAL g);
  virtual int  FastFind(void);
  virtual int  MaxRange(void) {return 1;}
  virtual int  Range(PGLOBAL g, int limit = 0, bool incl = true);
  virtual int  Qcompare(int *, int *) {assert(false); return 0;}
  virtual bool Make(PGLOBAL, PIXDEF) {return false;}
  virtual void Close(void) {}

 protected:
  // Members
  PTDBDOS Tdbp;             // Points to calling table TDB
  PVAL    Valp;             // The value to match in index
  }; // end of class XXROW

/***********************************************************************/
/*  Definition of class KXYCOL used to store values of indexed columns */
/***********************************************************************/
class KXYCOL: public BLOCK {
  friend class INDEXDEF;
  friend class XINDEX;
  friend class XINDXS;
  friend class XBIGEX;
  friend class XBIGXS;
  friend class TDBDOS;
 public:
  // Constructors
  KXYCOL(PKXBASE kp);

  // Implementation
  int  GetType(void) {return Type;}
  void SetValue(PCOL colp, int i);

 public:
  // Methods
  virtual bool Init(PGLOBAL g, PCOL colp, int n, bool sm, int kln);
  virtual bool InitFind(PGLOBAL g, PXOB xp);
  virtual void ReAlloc(PGLOBAL g, int n);
  virtual void FreeData(void);
  virtual void FillValue(PVAL valp);
  virtual int  CompVal(int i);
//        void InitBinFind(void *vp);
          bool MakeBlockArray(PGLOBAL g, int nb, int size);
          int  Compare(int i1, int i2);
          int  CompBval(int i);
          void Save(int i) {Valp->SetBinValue(Kblp->GetValPtr(i));}
          void Restore(int j) {Kblp->SetValue(Valp, j);}
          void Move(int j, int k) {Kblp->Move(k, j);}

  // Specific functions
#if defined(XMAP)
          BYTE *MapInit(PGLOBAL g, PCOL colp, int *n, BYTE *m);
#endif   // XMAP
          int *MakeOffset(PGLOBAL g, int n);

 protected:
  // Members
  PXCOL   Next;            // To next in the key part list
  PXCOL   Previous;        // To previous in the key part list
  PKXBASE Kxp;             // To the INDEX class block
  PCOL    Colp;            // To matching object if a column
  bool    IsSorted;        // true if column is already sorted
  bool    Asc;             // true for ascending sort, false for Desc
  MBLOCK  Keys;            // Data array allocation block
  void*  &To_Keys;         // To data array
  PVBLK   Kblp;            // To Valblock of the data array
  MBLOCK  Bkeys;           // Block array allocation block
  void*  &To_Bkeys;        // To block array
  PVBLK   Blkp;            // To Valblock of the block array
  PVAL    Valp;            // Value use by Find
  int     Klen;            // Length of character string or num value
  int     Kprec;           // The Value(s) precision or CI
  int     Type;            // The Value(s) type
  bool    Prefix;          // Key on CHAR column prefix
  MBLOCK  Koff;            // Offset allocation block
  CPINT  &Kof;             // Reference to offset array
  int     Val_K;           // Index of current column value
  int     Ndf;             // Number of stored values
  int     Mxs;             // Max same for this column
  }; // end of class KXYCOL

#endif // __XINDEX_H__
