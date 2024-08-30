/*************** TabDos H Declares Source Code File (.H) ***************/
/*  Name: TABDOS.H    Version 3.3                                      */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          1999-2015    */
/*                                                                     */
/*  This file contains the DOS classes declares.                       */
/***********************************************************************/

#ifndef __TABDOS_H
#define __TABDOS_H

#include "xtable.h"                       // Table  base class declares
#include "colblk.h"                       // Column base class declares
#include "xindex.h"
#include "filter.h"

//pedef struct _tabdesc   *PTABD;         // For friend setting
typedef class TXTFAM      *PTXF;
typedef class BLOCKFILTER *PBF;
typedef class BLOCKINDEX  *PBX;

/***********************************************************************/
/*  DOS table.                                                         */
/***********************************************************************/
class DllExport DOSDEF : public TABDEF {  /* Logical table description */
  friend class OEMDEF;
  friend class TDBDOS;
  friend class TDBFIX;
  friend class TXTFAM;
  friend class DBFBASE;
	friend class UNZIPUTL;
	friend class JSONCOL;
	friend class TDBDCL;
 public:
  // Constructor
  DOSDEF(void);

  // Implementation
  AMT         GetDefType(void) override {return TYPE_AM_DOS;}
  const char *GetType(void) override {return "DOS";}
  PIXDEF      GetIndx(void) override {return To_Indx;}
  void        SetIndx(PIXDEF xdp) override {To_Indx = xdp;}
  bool        IsHuge(void) override {return Huge;}
  PCSZ    GetFn(void) {return Fn;}
  PCSZ    GetOfn(void) {return Ofn;}
	PCSZ    GetEntry(void) {return Entry;}
	bool    GetMul(void) {return Mulentries;}
	bool    GetAppend(void) {return Append;}
	void    SetBlock(int block) { Block = block; }
  int     GetBlock(void) {return Block;}
  int     GetLast(void) {return Last;}
  void    SetLast(int last) {Last = last;}
  int     GetLrecl(void) {return Lrecl;}
  void    SetLrecl(int lrecl) {Lrecl = lrecl;}
  bool    GetPadded(void) {return Padded;}
  bool    GetEof(void) {return Eof;}
  int     GetBlksize(void) {return Blksize;}
  int     GetEnding(void) {return Ending;}
  bool    IsOptimized(void) {return (Optimized == 1);}
  void    SetOptimized(int opt) {Optimized = opt;}
  void    SetAllocBlks(int blks) {AllocBlks = blks;}
  int     GetAllocBlks(void) {return AllocBlks;}
  int    *GetTo_Pos(void) {return To_Pos;}

  // Methods
	int  Indexable(void) override
  	{return (!Multiple && !Mulentries && Compressed != 1) ? 1 : 0;}
	virtual bool DeleteIndexFile(PGLOBAL g, PIXDEF pxdf);
  bool DefineAM(PGLOBAL g, LPCSTR am, int poff) override;
  PTDB GetTable(PGLOBAL g, MODE mode) override;
          bool InvalidateIndex(PGLOBAL g);
          bool GetOptFileName(PGLOBAL g, char *filename);
          void RemoveOptValues(PGLOBAL g);

 protected:
//virtual bool Erase(char *filename);

  // Members
  PCSZ    Fn;                 /* Path/Name of corresponding file       */
  PCSZ    Ofn;                /* Base Path/Name of matching index files*/
	PCSZ    Entry;						  /* Zip entry name or pattern						 */
	PCSZ    Pwd;						    /* Zip password             						 */
	PIXDEF  To_Indx;            /* To index definitions blocks           */
  bool    Mapped;             /* 0: disk file, 1: memory mapped file   */
	bool    Zipped;             /* true for zipped table file            */
	bool    Mulentries;         /* true for multiple entries             */
	bool    Append;             /* Used when creating zipped table       */
	bool    Padded;             /* true for padded table file            */
  bool    Huge;               /* true for files larger than 2GB        */
  bool    Accept;             /* true if wrong lines are accepted      */
  bool    Eof;                /* true if an EOF (0xA) character exists */
  int    *To_Pos;             /* To array of block starting positions  */
  int     Optimized;          /* 0: No, 1:Yes, 2:Redo optimization     */
  int     AllocBlks;          /* Number of suballocated opt blocks     */
  int     Compressed;         /* 0: No, 1: gz, 2:zlib compressed file  */
  int     Lrecl;              /* Size of biggest record                */
  int     AvgLen;             /* Average size of records               */
  int     Block;              /* Number de blocks of FIX/VCT tables    */
  int     Last;               /* Number of elements of last block      */
  int     Blksize;            /* Size of padded blocks                 */
  int     Maxerr;             /* Maximum number of bad records (DBF)   */
  int     ReadMode;           /* Specific to DBF                       */
  int     Ending;             /* Length of end of lines                */
  char    Teds;               /* Binary table default endian setting   */
  }; // end of DOSDEF

/***********************************************************************/
/*  This is the DOS/UNIX Access Method class declaration for files     */
/*  that are standard files with columns starting at fixed offset.     */
/*  The last column (and record) is of variable length.                */
/***********************************************************************/
class DllExport TDBDOS : public TDBASE {
  friend class XINDEX;
  friend class DOSCOL;
  friend class MAPCOL;
  friend class TXTFAM;
  friend class DOSFAM;
  friend class VCTCOL;
  friend RCODE CntDeleteRow(PGLOBAL, PTDB, bool);
 public:
  // Constructors
  TDBDOS(PDOSDEF tdp, PTXF txfp);
  TDBDOS(PGLOBAL g, PTDBDOS tdbp);

  // Inline functions
  inline  void  SetTxfp(PTXF txfp) {Txfp = txfp; Txfp->SetTdbp(this);}
  inline  PTXF  GetTxfp(void) {return Txfp;}
  inline  char *GetLine(void) {return To_Line;}
  inline  int   GetCurBlk(void) {return Txfp->GetCurBlk();}
  inline  void  SetLine(char *toline) {To_Line = toline;}
  inline  void  IncLine(int inc) {To_Line += inc;}
  inline  bool  IsRead(void) {return Txfp->IsRead;}
  inline  PXOB *GetLink(void) {return To_Link;}

  // Implementation
  AMT   GetAmType(void) override {return Txfp->GetAmType();}
  PCSZ  GetFile(PGLOBAL) override {return Txfp->To_File;}
  void  SetFile(PGLOBAL, PCSZ fn) override {Txfp->To_File = fn;}
  void  SetAbort(bool b) override {Abort = b;}
  RECFM GetFtype(void) override {return Ftype;}
  virtual bool  SkipHeader(PGLOBAL) {return false;}
  void  RestoreNrec(void) override {Txfp->SetNrec(1);}
  PTDB  Duplicate(PGLOBAL g) override
                {return (PTDB)new(g) TDBDOS(g, this);}

  // Methods
  PTDB  Clone(PTABS t) override;
  void  ResetDB(void) override {Txfp->Reset();}
  bool  IsUsingTemp(PGLOBAL g) override;
  bool  IsIndexed(void) override {return Indxd;}
  void  ResetSize(void) override {MaxSize = Cardinal = -1;}
  int   ResetTableOpt(PGLOBAL g, bool dop, bool dox) override;
  virtual int   MakeBlockValues(PGLOBAL g);
  virtual bool  SaveBlockValues(PGLOBAL g);
  bool  GetBlockValues(PGLOBAL g) override;
  virtual PBF   InitBlockFilter(PGLOBAL g, PFIL filp);
//virtual PBX   InitBlockIndex(PGLOBAL g);
  virtual int   TestBlock(PGLOBAL g);
  void  PrintAM(FILE *f, char *m) override;

  // Database routines
  PCOL  MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n) override;
  virtual char *GetOpenMode(PGLOBAL, char*) {return NULL;}
  virtual int   GetFileLength(PGLOBAL g) {return Txfp->GetFileLength(g);}
  int   GetProgMax(PGLOBAL g) override;
  int   GetProgCur(void) override;
//virtual int   GetAffectedRows(void) {return Txfp->GetDelRows();}
  int   GetRecpos(void) override {return Txfp->GetPos();}
  bool  SetRecpos(PGLOBAL g, int recpos) override
                {return Txfp->SetPos(g, recpos);}
  int   RowNumber(PGLOBAL g, bool b = false) override;
  int   Cardinality(PGLOBAL g) override;
  int   GetMaxSize(PGLOBAL g) override;
  bool  OpenDB(PGLOBAL g) override;
  int   ReadDB(PGLOBAL g) override;
  int   WriteDB(PGLOBAL g) override;
  int   DeleteDB(PGLOBAL g, int irc) override;
  void  CloseDB(PGLOBAL g) override;
  virtual int   ReadBuffer(PGLOBAL g) {return Txfp->ReadBuffer(g);}

  // Specific routine
  virtual int   EstimatedLength(void);

  // Optimization routines
  int   MakeIndex(PGLOBAL g, PIXDEF pxdf, bool add) override;
          bool  InitialyzeIndex(PGLOBAL g, PIXDEF xdp, bool sorted);
          void  ResetBlockFilter(PGLOBAL g);
          bool  GetDistinctColumnValues(PGLOBAL g, int nrec);

 protected:
  bool  PrepareWriting(PGLOBAL g) override;
          PBF   CheckBlockFilari(PGLOBAL g, PXOB *arg, int op, bool *cnv);

  // Members
  PTXF    Txfp;              // To the File access method class
//PBX     To_BlkIdx;         // To index test block
  PBF     To_BlkFil;         // To evaluation block filter
  PFIL    SavFil;            // Saved hidden filter
  char   *To_Line;           // Points to current processed line
  bool    Abort;             // TRUE when aborting UPDATE/DELETE
  bool    Indxd;             // TRUE for indexed UPDATE/DELETE
  int     Lrecl;             // Logical Record Length
  int     AvgLen;            // Logical Record Average Length
//int     Xeval;             // BlockTest return value
  int     Beval;             // BlockEval return value
  }; // end of class TDBDOS

/***********************************************************************/
/*  Class DOSCOL: DOS access method column descriptor.                 */
/*  This A.M. is used for text file tables under operating systems     */
/*  DOS, OS2, UNIX, WIN16 and WIN32.                                   */
/***********************************************************************/
class DllExport DOSCOL : public COLBLK {
  friend class TDBDOS;
  friend class TDBFIX;
 public:
  // Constructors
  DOSCOL(PGLOBAL g, PCOLDEF cdp, PTDB tp, PCOL cp, int i, PCSZ am = "DOS");
  DOSCOL(DOSCOL *colp, PTDB tdbp); // Constructor used in copy process

  // Implementation
  int    GetAmType(void) override {return TYPE_AM_DOS;}
  void   SetTo_Val(PVAL valp) override {To_Val = valp;}
  int    GetClustered(void) override {return Clustered;}
  int    IsClustered(void) override {return (Clustered &&
                 ((PDOSDEF)(((PTDBDOS)To_Tdb)->To_Def))->IsOptimized());}
  virtual int    IsSorted(void) {return Sorted;}
  virtual PVBLK  GetMin(void) {return Min;}
  virtual PVBLK  GetMax(void) {return Max;}
  virtual int    GetNdv(void) {return Ndv;}
  virtual int    GetNbm(void) {return Nbm;}
  virtual PVBLK  GetBmap(void) {return Bmap;}
  virtual PVBLK  GetDval(void) {return Dval;}

  // Methods
  bool   VarSize(void) override;
  bool   SetBuffer(PGLOBAL g, PVAL value, bool ok, bool check) override;
  void   ReadColumn(PGLOBAL g) override;
  void   WriteColumn(PGLOBAL g) override;

 protected:
  virtual bool   SetMinMax(PGLOBAL g);
  virtual bool   SetBitMap(PGLOBAL g);
          bool   CheckSorted(PGLOBAL g);
          bool   AddDistinctValue(PGLOBAL g);

  // Default constructor not to be used
  DOSCOL(void) = default;

  // Members
  PVBLK Min;          // Array of block min values
  PVBLK Max;          // Array of block max values
  PVBLK Bmap;         // Array of block bitmap values
  PVBLK Dval;         // Array of column distinct values
  PVAL  To_Val;       // To value used for Update/Insert
  PVAL  OldVal;       // The previous value of the object.
  char *Buf;          // Buffer used in read/write operations
  char  Dsp;          // The decimal separator
  bool  Ldz;          // True if field contains leading zeros
  bool  Nod;          // True if no decimal point
  int   Dcm;          // Last Dcm digits are decimals
  int   Deplac;       // Offset in dos_buf
  int   Clustered;    // 0:No 1:Yes
  int   Sorted;       // 0:No 1:Asc (2:Desc - NIY)
  int   Ndv;          // Number of distinct values
  int   Nbm;          // Number of uint in bitmap
  }; // end of class DOSCOL

#endif // __TABDOS_H
