/*************** TabDos H Declares Source Code File (.H) ***************/
/*  Name: TABDOS.H    Version 3.2                                      */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          1999-2012    */
/*                                                                     */
/*  This file contains the DOS classes declares.                       */
/***********************************************************************/

#ifndef __TABDOS_H
#define __TABDOS_H

#include "xtable.h"                       // Table  base class declares
#include "colblk.h"                       // Column base class declares
#include "xindex.h"

typedef struct _tabdesc   *PTABD;         // For friend setting
typedef class TXTFAM      *PTXF;

/***********************************************************************/
/*  DOS table.                                                         */
/***********************************************************************/
class DllExport DOSDEF : public TABDEF {  /* Logical table description */
  friend class OEMDEF;
  friend class TDBDOS;
  friend class TDBFIX;
  friend class TXTFAM;
	friend class DBFBASE;
 public:
  // Constructor
  DOSDEF(void);

  // Implementation
	virtual AMT         GetDefType(void) {return TYPE_AM_DOS;}
  virtual const char *GetType(void) {return "DOS";}
	virtual PIXDEF      GetIndx(void) {return To_Indx;}
	virtual void        SetIndx(PIXDEF xdp) {To_Indx = xdp;}
	PSZ     GetOfn(void) {return Ofn;}
  void    SetBlock(int block) {Block = block;}
  int     GetBlock(void) {return Block;}
  int     GetLast(void) {return Last;}
  void    SetLast(int last) {Last = last;}
  int     GetLrecl(void) {return Lrecl;}
  void    SetLrecl(int lrecl) {Lrecl = lrecl;}
  bool    GetPadded(void) {return Padded;}
  bool    GetEof(void) {return Eof;}
  int     GetBlksize(void) {return Blksize;}
  int     GetEnding(void) {return Ending;}
	int    *GetTo_Pos(void) {return To_Pos;}
	virtual bool IsHuge(void) {return Huge;}

  // Methods
  virtual bool DeleteTableFile(PGLOBAL g);
	virtual bool Indexable(void) {return Compressed != 1;}
	virtual bool DeleteIndexFile(PGLOBAL g, PIXDEF pxdf);
  virtual bool DefineAM(PGLOBAL g, LPCSTR am, int poff);
  virtual PTDB GetTable(PGLOBAL g, MODE mode);
					bool InvalidateIndex(PGLOBAL g);

 protected:
  virtual bool Erase(char *filename);

  // Members
  PSZ     Fn;                 /* Path/Name of corresponding file       */
  PSZ     Ofn;                /* Base Path/Name of matching index files*/
	PIXDEF  To_Indx;						/* To index definitions blocks           */
  RECFM   Recfm;              /* 0:VAR, 1:FIX, 2:BIN, 3:VCT, 6:DBF     */
  bool    Mapped;             /* 0: disk file, 1: memory mapped file   */
  bool    Padded;             /* true for padded table file            */
  bool    Huge;               /* true for files larger than 2GB        */
  bool    Accept;             /* true if wrong lines are accepted (DBF)*/
  bool    Eof;								/* true if an EOF (0xA) character exists */
	int    *To_Pos;							/* To array of block starting positions  */
  int     Compressed;         /* 0: No, 1: gz, 2:zlib compressed file  */
  int     Lrecl;              /* Size of biggest record                */
  int     AvgLen;             /* Average size of records               */
  int     Block;              /* Number de blocks of FIX/VCT tables    */
  int     Last;               /* Number of elements of last block      */
  int     Blksize;            /* Size of padded blocks                 */
  int     Maxerr;             /* Maximum number of bad records (DBF)   */
  int     ReadMode;           /* Specific to DBF                       */
  int     Ending;             /* Length of end of lines                */
  }; // end of DOSDEF

/***********************************************************************/
/*  This is the DOS/UNIX Access Method class declaration for files     */
/*  that are standard files with columns starting at fixed offset.     */
/*  The last column (and record) is of variable length.                */
/***********************************************************************/
class DllExport TDBDOS : public TDBASE {
//friend class KINDEX;
	friend class XINDEX;
  friend class DOSCOL;
  friend class MAPCOL;
  friend class TXTFAM;
  friend class DOSFAM;
	friend class VCTCOL;
//friend class TDBMUL;
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
//inline  PCOL *GetKeyCol(void) {return To_Key_Col;}

  // Implementation
  virtual AMT   GetAmType(void) {return Txfp->GetAmType();}
  virtual PSZ   GetFile(PGLOBAL g) {return Txfp->To_File;}
	virtual void  SetFile(PGLOBAL g, PSZ fn) {Txfp->To_File = fn;}
	virtual RECFM GetFtype(void) {return Ftype;}
  virtual bool  SkipHeader(PGLOBAL g) {return false;}
	virtual void  RestoreNrec(void) {Txfp->SetNrec(1);}
  virtual PTDB  Duplicate(PGLOBAL g)
                {return (PTDB)new(g) TDBDOS(g, this);}

  // Methods
  virtual PTDB  CopyOne(PTABS t);
  virtual void  ResetDB(void) {Txfp->Reset();}
  virtual bool  IsUsingTemp(PGLOBAL g);
//virtual bool  NeedIndexing(PGLOBAL g);
	virtual void  ResetSize(void) {MaxSize = Cardinal = -1;}
	virtual int   ResetTableOpt(PGLOBAL g, bool dox);
//virtual int   MakeBlockValues(PGLOBAL g);
//virtual bool  SaveBlockValues(PGLOBAL g);
//virtual bool  GetBlockValues(PGLOBAL g);
//virtual PBF   InitBlockFilter(PGLOBAL g, PFIL filp);
//virtual PBX   InitBlockIndex(PGLOBAL g);
//virtual int   TestBlock(PGLOBAL g);
  virtual void  PrintAM(FILE *f, char *m);

  // Database routines
  virtual PCOL  MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n);
  virtual char *GetOpenMode(PGLOBAL g, char *opmode) {return NULL;}
  virtual int   GetFileLength(PGLOBAL g) {return Txfp->GetFileLength(g);}
  virtual int   GetProgMax(PGLOBAL g);
  virtual int   GetProgCur(void);
	virtual int   GetAffectedRows(void) {return Txfp->GetDelRows();}
	virtual int   GetRecpos(void) {return Txfp->GetPos();}
  virtual bool  SetRecpos(PGLOBAL g, int recpos)
								{return Txfp->SetPos(g, recpos);}
  virtual int   RowNumber(PGLOBAL g, bool b = false);
  virtual int   Cardinality(PGLOBAL g);
  virtual int   GetMaxSize(PGLOBAL g);
  virtual bool  OpenDB(PGLOBAL g);
  virtual int   ReadDB(PGLOBAL g);
  virtual int   WriteDB(PGLOBAL g);
  virtual int   DeleteDB(PGLOBAL g, int irc);
  virtual void  CloseDB(PGLOBAL g);
  virtual int   ReadBuffer(PGLOBAL g) {return Txfp->ReadBuffer(g);}

	// Specific routine
	virtual int  EstimatedLength(PGLOBAL g);

	// Optimization routines
//				void  ResetBlockFilter(PGLOBAL g);
					int		MakeIndex(PGLOBAL g, PIXDEF pxdf, bool add);
//				bool  GetDistinctColumnValues(PGLOBAL g, int nrec);

 protected:
//				PBF		CheckBlockFilari(PGLOBAL g, PXOB *arg, int op, bool *cnv);

  // Members
  PTXF    Txfp;              // To the File access method class
//PBX     To_BlkIdx;         // To index test block
//PBF     To_BlkFil;         // To evaluation block filter
//PFIL    SavFil;            // Saved hidden filter
  char   *To_Line;           // Points to current processed line
	int     Cardinal;					 // Table Cardinality
	RECFM   Ftype;             // File type: 0-var 1-fixed 2-binary (VCT)
  int     Lrecl;             // Logical Record Length
  int     AvgLen;            // Logical Record Average Length
//int     Xeval;             // BlockTest return value
//int     Beval;             // BlockEval return value
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
  DOSCOL(PGLOBAL g, PCOLDEF cdp, PTDB tp, PCOL cp, int i, PSZ am = "DOS");
  DOSCOL(DOSCOL *colp, PTDB tdbp); // Constructor used in copy process

  // Implementation
  virtual int    GetAmType(void) {return TYPE_AM_DOS;}
//virtual int    GetClustered(void) {return Clustered;}
//virtual int    IsClustered(void) {return (Clustered &&
//               ((PDOSDEF)(((PTDBDOS)To_Tdb)->To_Def))->IsOptimized());}
//virtual int    IsSorted(void) {return Sorted;}
  virtual void   SetTo_Val(PVAL valp) {To_Val = valp;}
//virtual PVBLK  GetMin(void) {return Min;}
//virtual PVBLK  GetMax(void) {return Max;}
//virtual int    GetNdv(void) {return Ndv;}
//virtual int    GetNbm(void) {return Nbm;}
//virtual PVBLK  GetBmap(void) {return Bmap;}
//virtual PVBLK  GetDval(void) {return Dval;}

  // Methods
	virtual bool	 VarSize(void);
  virtual bool   SetBuffer(PGLOBAL g, PVAL value, bool ok, bool check);
  virtual void   ReadColumn(PGLOBAL g);
  virtual void   WriteColumn(PGLOBAL g);
  virtual void   Print(PGLOBAL g, FILE *, uint);

 protected:
//virtual bool   SetMinMax(PGLOBAL g);
//virtual	bool   SetBitMap(PGLOBAL g);
//				bool	 CheckSorted(PGLOBAL g);
//				bool	 AddDistinctValue(PGLOBAL g);

  // Default constructor not to be used
  DOSCOL(void) {}

  // Members
//PVBLK Min;          // Array of block min values
//PVBLK Max;          // Array of block max values
//PVBLK Bmap;         // Array of block bitmap values
//PVBLK Dval;         // Array of column distinct values
  PVAL  To_Val;       // To value used for Update/Insert
  PVAL  OldVal;       // The previous value of the object.
	char *Buf;          // Buffer used in write operations
	bool  Ldz;					// True if field contains leading zeros
	bool  Nod;					// True if no decimal point
	int   Dcm;					// Last Dcm digits are decimals
//int   Clustered;    // 0:No 1:Yes
//int   Sorted;       // 0:No 1:Asc (2:Desc - NIY)
  int   Deplac;       // Offset in dos_buf
//int		Ndv;					// Number of distinct values
//int   Nbm;					// Number of uint in bitmap
  }; // end of class DOSCOL

#endif // __TABDOS_H
