/*************** BlkFil H Declares Source Code File (.H) ***************/
/*  Name: BLKFIL.H    Version 2.1                                      */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2004-2010    */
/*                                                                     */
/*  This file contains the block optimization related classes declares */
/***********************************************************************/
#ifndef __BLKFIL__
#define __BLKFIL__

typedef class BLOCKFILTER *PBF;
typedef class BLOCKINDEX  *PBX;

/***********************************************************************/
/*  Definition of class BLOCKFILTER.                                   */
/***********************************************************************/
class DllExport BLOCKFILTER : public BLOCK {           /* Block Filter */
  friend class BLKFILLOG;
 public:
  // Constructors
  BLOCKFILTER(PTDBDOS tdbp, int op);

  // Implementation
          int  GetResult(void) {return Result;}
          bool Correlated(void) {return Correl;}

  // Methods
  virtual void Reset(PGLOBAL) = 0;
  virtual int  BlockEval(PGLOBAL) = 0;
  virtual void Printf(PGLOBAL g, FILE *f, uint n);
  virtual void Prints(PGLOBAL g, char *ps, uint z);

 protected:
  BLOCKFILTER(void) {}       // Standard constructor not to be used

  // Members
  PTDBDOS Tdbp;         // Owner TDB
  bool    Correl;       // TRUE for correlated subqueries
  int     Opc;          // Comparison operator
  int     Opm;          // Operator modificator
  int     Result;       // Result from evaluation
  }; // end of class BLOCKFILTER

/***********************************************************************/
/*  Definition of class BLKFILLOG (with Op=OP_AND,OP_OR, or OP_NOT)    */
/***********************************************************************/
class DllExport BLKFILLOG : public BLOCKFILTER { /* Logical Op Block Filter */
 public:
  // Constructors
  BLKFILLOG(PTDBDOS tdbp, int op, PBF *bfp, int n);

  // Methods
  virtual void Reset(PGLOBAL g);
  virtual int  BlockEval(PGLOBAL g);

 protected:
  BLKFILLOG(void) {}       // Standard constructor not to be used

  // Members
  PBF *Fil;                // Points to Block filter args
  int N;
  }; // end of class BLKFILLOG

/***********************************************************************/
/*  Definition of class BLKFILARI (with Op=OP_EQ,NE,GT,GE,LT, or LE)   */
/***********************************************************************/
class DllExport BLKFILARI : public BLOCKFILTER { /* Arithm. Op Block Filter */
 public:
  // Constructors
  BLKFILARI(PGLOBAL g, PTDBDOS tdbp, int op, PXOB *xp);

  // Methods
  virtual void Reset(PGLOBAL g);
  virtual int  BlockEval(PGLOBAL g);
  virtual void MakeValueBitmap(void) {}

 protected:
  BLKFILARI(void) {}       // Standard constructor not to be used

  // Members
  PDOSCOL Colp;            // Points to column argument
  PCOL    Cpx;             // Point to subquery "constant" column
  PVAL    Valp;            // Points to constant argument Value
  bool    Sorted;          // True if the column is sorted
  }; // end of class BLKFILARI

/***********************************************************************/
/*  Definition of class BLKFILAR2 (with Op=OP_EQ,NE,GT,GE,LT, or LE)   */
/***********************************************************************/
class DllExport BLKFILAR2 : public BLKFILARI { /* Arithm. Op Block Filter */
 public:
  // Constructors
  BLKFILAR2(PGLOBAL g, PTDBDOS tdbp, int op, PXOB *xp);

  // Methods
  virtual int  BlockEval(PGLOBAL g);
  virtual void MakeValueBitmap(void);

 protected:
  BLKFILAR2(void) {}       // Standard constructor not to be used

  // Members
  uint Bmp;                // The value bitmap used to test blocks
  uint Bxp;                // Bitmap used when Opc = OP_EQ
  }; // end of class BLKFILAR2

/***********************************************************************/
/*  Definition of class BLKFILAR2 (with Op=OP_EQ,NE,GT,GE,LT, or LE)   */
/*  To be used when the bitmap is an array of ULONG bitmaps;           */
/***********************************************************************/
class DllExport BLKFILMR2 : public BLKFILARI { /* Arithm. Op Block Filter */
 public:
  // Constructors
  BLKFILMR2(PGLOBAL g, PTDBDOS tdbp, int op, PXOB *xp);

  // Methods
  virtual int  BlockEval(PGLOBAL g);
  virtual void MakeValueBitmap(void);

 protected:
  BLKFILMR2(void) {}       // Standard constructor not to be used

  // Members
  int    Nbm;              // The number of ULONG bitmaps
  int    N;                // The position of the leftmost ULONG
  bool   Void;             // True if all file blocks can be skipped
  uint  *Bmp;              // The values bitmaps used to test blocks
  uint  *Bxp;              // Bit of values <= max value
  }; // end of class BLKFILMR2

/***********************************************************************/
/*  Definition of class BLKSPCARI (with Op=OP_EQ,NE,GT,GE,LT, or LE)   */
/***********************************************************************/
class DllExport BLKSPCARI : public BLOCKFILTER { /* Arithm. Op Block Filter */
 public:
  // Constructors
  BLKSPCARI(PTDBDOS tdbp, int op, PXOB *xp, int bsize);

  // Methods
  virtual void Reset(PGLOBAL g);
  virtual int  BlockEval(PGLOBAL g);

 protected:
  BLKSPCARI(void) {}       // Standard constructor not to be used

  // Members
  PCOL    Cpx;             // Point to subquery "constant" column
  PVAL    Valp;            // Points to constant argument Value
  int     Val;             // Constant argument Value
  int     Bsize;           // Table block size
  }; // end of class BLKSPCARI

/***********************************************************************/
/*  Definition of class BLKFILIN (with Op=OP_IN)                       */
/***********************************************************************/
class DllExport BLKFILIN : public BLOCKFILTER {  // With array arguments.
 public:
  // Constructors
  BLKFILIN(PGLOBAL g, PTDBDOS tdbp, int op, int opm, PXOB *xp);

  // Methods
  virtual void Reset(PGLOBAL g);
  virtual int  BlockEval(PGLOBAL g);
  virtual void MakeValueBitmap(void) {}

 protected:
  // Member
  PDOSCOL Colp;            // Points to column argument
  PARRAY  Arap;            // Points to array argument
  bool    Sorted;          // True if the column is sorted
  int     Type;            // Type of array elements
  }; // end of class BLKFILIN

/***********************************************************************/
/*  Definition of class BLKFILIN2 (with Op=OP_IN)                      */
/***********************************************************************/
class DllExport BLKFILIN2 : public BLKFILIN {  // With array arguments.
 public:
  // Constructors
  BLKFILIN2(PGLOBAL g, PTDBDOS tdbp, int op, int opm, PXOB *xp);

  // Methods
//virtual void Reset(PGLOBAL g);
  virtual int  BlockEval(PGLOBAL g);
  virtual void MakeValueBitmap(void);

 protected:
  // Member
  int    Nbm;              // The number of ULONG bitmaps
  int    N;                // The position of the leftmost ULONG
//bool   Bitmap;           // True for IN operator (temporary)
  bool   Void;             // True if all file blocks can be skipped
  bool   Invert;           // True when Result must be inverted
  uint  *Bmp;              // The values bitmaps used to test blocks
  uint  *Bxp;              // Bit of values <= max value
  PVAL   Valp;             // Used while building the bitmaps
  }; // end of class BLKFILIN2

/***********************************************************************/
/*  Definition of class BLKSPCIN (with Op=OP_IN) Special column        */
/***********************************************************************/
class DllExport BLKSPCIN : public BLOCKFILTER {  // With array arguments.
 public:
  // Constructors
  BLKSPCIN(PGLOBAL g, PTDBDOS tdbp, int op, int opm, PXOB *xp, int bsize);

  // Methods
  virtual void Reset(PGLOBAL g);
  virtual int  BlockEval(PGLOBAL g);

 protected:
  // Member
  PARRAY  Arap;            // Points to array argument
  int     Bsize;           // Table block size
  }; // end of class BLKSPCIN

// ---------------- Class used in block indexing testing ----------------

#if 0
/***********************************************************************/
/*  Definition of class BLOCKINDEX.                                    */
/*  Used to test the indexing to joined tables when the foreign key is */
/*  a clustered or sorted column. If the table is joined to several    */
/*  tables, blocks will be chained together.                           */
/***********************************************************************/
class DllExport BLOCKINDEX : public BLOCK {     /* Indexing Test Block */
 public:
  // Constructors
  BLOCKINDEX(PBX nx, PDOSCOL cp, PKXBASE kp);

  // Implementation
          PBX  GetNext(void) {return Next;}

  // Methods
          void Reset(void);
  virtual int  BlockEval(PGLOBAL);
  virtual void Printf(PGLOBAL g, FILE *f, UINT n);
  virtual void Prints(PGLOBAL g, char *ps, UINT z);

 protected:
  BLOCKINDEX(void) {}   // Standard constructor not to be used

  // Members
  PBX     Next;         // To next Index Block
  PTDBDOS Tdbp;         // To table description block
  PDOSCOL Colp;         // Clustered foreign key
  PKXBASE Kxp;          // To Kindex of joined table
  bool    Sorted;       // TRUE if column is sorted
  int     Type;         // Col/Index type
  int     Result;       // Result from evaluation
  }; // end of class BLOCKINDEX

/***********************************************************************/
/*  Definition of class BLOCKINDX2.     (XDB2)                         */
/***********************************************************************/
class DllExport BLOCKINDX2 : public BLOCKINDEX { /* Indexing Test Block */
 public:
  // Constructors
  BLOCKINDX2(PBX nx, PDOSCOL cp, PKXBASE kp);

  // Methods
  virtual int  BlockEval(PGLOBAL);

 protected:
  BLOCKINDX2(void) {}   // Standard constructor not to be used

  // Members
  int   Nbm;            // The number of ULONG bitmaps
  PVBLK Dval;           // Array of column distinct values
  PVBLK Bmap;           // Array of block bitmap values
  }; // end of class BLOCKINDX2

/***********************************************************************/
/*  Definition of class BLKSPCINDX.                                    */
/*  Used to test the indexing to joined tables when the foreign key is */
/*  the ROWID special column. If the table is joined to several        */
/*  tables, blocks will be chained together.                           */
/***********************************************************************/
class DllExport BLKSPCINDX : public BLOCKINDEX { /* Indexing Test Block */
 public:
  // Constructors
  BLKSPCINDX(PBX nx, PTDBDOS tp, PKXBASE kp, int bsize);

  // Methods
  virtual int  BlockEval(PGLOBAL);

 protected:
  BLKSPCINDX(void) {}   // Standard constructor not to be used

  // Members
  int     Bsize;        // Table block size
  }; // end of class BLOCKINDEX
#endif // 0

#endif // __BLKFIL__
