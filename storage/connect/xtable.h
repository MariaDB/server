/**************** Table H Declares Source Code File (.H) ***************/
/*  Name: TABLE.H    Version 2.3                                       */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          1999-2015    */
/*                                                                     */
/*  This file contains the TBX, OPJOIN and TDB class definitions.      */
/***********************************************************************/
#if !defined(TABLE_DEFINED)
#define      TABLE_DEFINED


/***********************************************************************/
/*  Include required application header files                          */
/*  block.h      is header containing Block    global declarations.    */
/***********************************************************************/
#include "assert.h"
#include "block.h"
#include "colblk.h"
#include "m_ctype.h"

typedef class CMD *PCMD;
typedef struct st_key_range key_range;

// Commands executed by XDBC and MYX tables
class CMD : public BLOCK {
 public:
  // Constructor
  CMD(PGLOBAL g, char *cmd) {Cmd = PlugDup(g, cmd); Next = NULL;}

  // Members
  PCMD  Next;
  char *Cmd;
}; // end of class CMD

// Condition filter structure
class CONDFIL : public BLOCK {
 public:
	// Constructor
	CONDFIL(const Item *cond, uint idx, AMT type) 
	{
		Cond = cond; Idx = idx; Type = type; Body = NULL; Op = OP_XX; Cmds = NULL;
	}

	// Members
	const Item *Cond;
	AMT   Type;
	uint  Idx;
	char *Body;
  OPVAL Op;
  PCMD  Cmds;
}; // end of class CONDFIL

typedef class CONDFIL *PCFIL;
typedef class TDBCAT *PTDBCAT;
typedef class CATCOL *PCATCOL;

/***********************************************************************/
/*  Definition of class TDB with all its method functions.             */
/***********************************************************************/
class DllExport TDB: public BLOCK {     // Table Descriptor Block.
 public:
  // Constructors
  TDB(PTABDEF tdp = NULL);
  TDB(PTDB tdbp);

  // Implementation
  static  void   SetTnum(int n) {Tnum = n;}
  inline  PTDB   GetOrig(void) {return To_Orig;}
  inline  TUSE   GetUse(void) {return Use;}
  inline  PCFIL  GetCondFil(void) {return To_CondFil;}
  inline  LPCSTR GetName(void) {return Name;}
  inline  PTABLE GetTable(void) {return To_Table;}
  inline  PCOL   GetColumns(void) {return Columns;}
  inline  int    GetDegree(void) {return Degree;}
  inline  MODE   GetMode(void) {return Mode;}
  inline  PFIL   GetFilter(void) {return To_Filter;}
  inline  void   SetFilter(PFIL fp) {To_Filter = fp;}
  inline  void   SetOrig(PTDB txp) {To_Orig = txp;}
  inline  void   SetUse(TUSE n) {Use = n;}
  inline  void   SetCondFil(PCFIL cfp) {To_CondFil = cfp;}
  inline  void   SetNext(PTDB tdbp) {Next = tdbp;}
  inline  void   SetName(LPCSTR name) {Name = name;}
  inline  void   SetTable(PTABLE tablep) {To_Table = tablep;}
  inline  void   SetColumns(PCOL colp) {Columns = colp;}
  inline  void   SetDegree(int degree) {Degree = degree;}
  inline  void   SetMode(MODE mode) {Mode = mode;}

  // Properties
  virtual AMT    GetAmType(void) {return TYPE_AM_ERROR;}
  virtual int    GetTdb_No(void) {return Tdb_No;}
  virtual PTDB   GetNext(void) {return Next;}
  virtual PCATLG GetCat(void) {return NULL;}
  virtual void   SetAbort(bool) {;}

  // Methods
  virtual bool   IsSame(PTDB tp) {return tp == this;}
  virtual bool   IsSpecial(PSZ name) = 0;
  virtual bool   GetBlockValues(PGLOBAL) {return false;}
  virtual int    Cardinality(PGLOBAL) {return 0;}
  virtual int    GetMaxSize(PGLOBAL) = 0;
  virtual int    GetProgMax(PGLOBAL) = 0;
  virtual int    GetProgCur(void) = 0;
  virtual int    RowNumber(PGLOBAL g, bool b = false);
  virtual bool   IsReadOnly(void) {return true;}
  virtual const CHARSET_INFO *data_charset() {return NULL;}
  virtual PTDB   Duplicate(PGLOBAL) {return NULL;}
  virtual PTDB   CopyOne(PTABS) {return this;}
  virtual PTDB   Copy(PTABS t);
  virtual void   PrintAM(FILE *f, char *m)
                  {fprintf(f, "%s AM(%d)\n",  m, GetAmType());}
  virtual void   Print(PGLOBAL g, FILE *f, uint n);
  virtual void   Print(PGLOBAL g, char *ps, uint z);
  virtual PSZ    GetServer(void) = 0;
  virtual int    GetBadLines(void) {return 0;}

  // Database pure virtual routines
  virtual PCOL   ColDB(PGLOBAL g, PSZ name, int num) = 0;
  virtual void   MarkDB(PGLOBAL, PTDB) = 0;
  virtual bool   OpenDB(PGLOBAL) = 0;
  virtual int    ReadDB(PGLOBAL) = 0;
  virtual int    WriteDB(PGLOBAL) = 0;
  virtual int    DeleteDB(PGLOBAL, int) = 0;
  virtual void   CloseDB(PGLOBAL) = 0;
  virtual int    CheckWrite(PGLOBAL) {return 0;}
  virtual bool   ReadKey(PGLOBAL, OPVAL, const key_range *) = 0;

 protected:
  // Members
  PTDB   To_Orig;      // Pointer to original if it is a copy
  TUSE   Use;
  PFIL   To_Filter;
  PCFIL  To_CondFil;   // To condition filter structure
  static int Tnum;     // Used to generate Tdb_no's
  const  int Tdb_No;   // GetTdb_No() is always 0 for OPJOIN
  PTDB   Next;         // Next in linearized queries
  PTABLE To_Table;     // Points to the XTAB object
  LPCSTR Name;         // Table name
  PCOL   Columns;      // Points to the first column of the table
  MODE   Mode;         // 10 Read, 30 Update, 40 Insert, 50 Delete
  int    Degree;       // Number of columns
  int    Cardinal;     // Table number of rows
  }; // end of class TDB

/***********************************************************************/
/*  This is the base class for all query tables (except decode).       */
/***********************************************************************/
class DllExport TDBASE : public TDB {
  friend class INDEXDEF;
  friend class XINDEX;
  friend class XINDXS;
 public:
  // Constructor
  TDBASE(PTABDEF tdp = NULL);
  TDBASE(PTDBASE tdbp);

  // Implementation
  inline  int     GetKnum(void) {return Knum;}
  inline  PTABDEF GetDef(void) {return To_Def;}
  inline  PKXBASE GetKindex(void) {return To_Kindex;}
  inline  PCOL    GetSetCols(void) {return To_SetCols;}
  inline  void    SetSetCols(PCOL colp) {To_SetCols = colp;}
  inline  void    SetKey_Col(PCOL *cpp) {To_Key_Col = cpp;}
  inline  void    SetXdp(PIXDEF xdp) {To_Xdp = xdp;}
  inline  void    SetKindex(PKXBASE kxp) {To_Kindex = kxp;}

  // Properties
  void    ResetKindex(PGLOBAL g, PKXBASE kxp);
  PCOL    Key(int i) {return (To_Key_Col) ? To_Key_Col[i] : NULL;}

  // Methods
  virtual bool   IsUsingTemp(PGLOBAL) {return false;}
  virtual bool   IsIndexed(void) {return false;}
  virtual bool   IsSpecial(PSZ name);
  virtual PCATLG GetCat(void);
  virtual PSZ    GetPath(void);
  virtual void   PrintAM(FILE *f, char *m);
  virtual RECFM  GetFtype(void) {return RECFM_NAF;}
//virtual int    GetAffectedRows(void) {return -1;}
  virtual int    GetRecpos(void) = 0;
  virtual bool   SetRecpos(PGLOBAL g, int recpos);
  virtual bool   IsReadOnly(void) {return Read_Only;}
  virtual bool   IsView(void) {return FALSE;}
  virtual CHARSET_INFO *data_charset(void);
  virtual int    GetProgMax(PGLOBAL g) {return GetMaxSize(g);}
  virtual int    GetProgCur(void) {return GetRecpos();}
  virtual PSZ    GetFile(PGLOBAL) {return "Not a file";}
  virtual int    GetRemote(void) {return 0;}
  virtual void   SetFile(PGLOBAL, PSZ) {}
  virtual void   ResetDB(void) {}
  virtual void   ResetSize(void) {MaxSize = -1;}
  virtual void   RestoreNrec(void) {}
  virtual int    ResetTableOpt(PGLOBAL g, bool dop, bool dox);
  virtual PSZ    GetServer(void) {return "Current";}

  // Database routines
  virtual PCOL ColDB(PGLOBAL g, PSZ name, int num);
  virtual PCOL MakeCol(PGLOBAL, PCOLDEF, PCOL, int)
                      {assert(false); return NULL;}
  virtual PCOL InsertSpecialColumn(PCOL colp);
  virtual PCOL InsertSpcBlk(PGLOBAL g, PCOLDEF cdp);
  virtual void MarkDB(PGLOBAL g, PTDB tdb2);
  virtual int  MakeIndex(PGLOBAL g, PIXDEF, bool)
                {strcpy(g->Message, "Remote index"); return RC_INFO;}
  virtual bool ReadKey(PGLOBAL, OPVAL, const key_range *)
                      {assert(false); return true;}

 protected:
  virtual bool PrepareWriting(PGLOBAL g) {strcpy(g->Message,
    "This function should not be called for this table"); return true;}

  // Members
  PTABDEF  To_Def;            // Points to catalog description block
  PXOB    *To_Link;           // Points to column of previous relations
  PCOL    *To_Key_Col;        // Points to key columns in current file
  PKXBASE  To_Kindex;         // Points to table key index
  PIXDEF   To_Xdp;            // To the index definition block
  PCOL     To_SetCols;        // Points to updated columns
  RECFM    Ftype;             // File type: 0-var 1-fixed 2-binary (VCT)
  int      MaxSize;           // Max size in number of lines
  int      Knum;              // Size of key arrays
  bool     Read_Only;         // True for read only tables
  const CHARSET_INFO *m_data_charset;
  const char *csname;         // Table charset name
  }; // end of class TDBASE

/***********************************************************************/
/*  The abstract base class declaration for the catalog tables.        */
/***********************************************************************/
class DllExport TDBCAT : public TDBASE {
  friend class CATCOL;
 public:
  // Constructor
  TDBCAT(PTABDEF tdp);

  // Implementation
  virtual AMT  GetAmType(void) {return TYPE_AM_CAT;}

  // Methods
  virtual int  GetRecpos(void) {return N;}
  virtual int  GetProgCur(void) {return N;}
  virtual int  RowNumber(PGLOBAL, bool = false) {return N + 1;}
  virtual bool SetRecpos(PGLOBAL g, int recpos);

  // Database routines
  virtual PCOL MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n);
  virtual int  GetMaxSize(PGLOBAL g);
  virtual bool OpenDB(PGLOBAL g);
  virtual int  ReadDB(PGLOBAL g);
  virtual int  WriteDB(PGLOBAL g);
  virtual int  DeleteDB(PGLOBAL g, int irc);
  virtual void CloseDB(PGLOBAL g);

 protected:
  // Specific routines
  virtual PQRYRES GetResult(PGLOBAL g) = 0;
          bool Initialize(PGLOBAL g);
          bool InitCol(PGLOBAL g);

  // Members
  PQRYRES Qrp;
  int     N;                  // Row number
  bool    Init;
  }; // end of class TDBCAT

/***********************************************************************/
/*  Class CATCOL: ODBC info column.                                    */
/***********************************************************************/
class DllExport CATCOL : public COLBLK {
  friend class TDBCAT;
 public:
  // Constructors
  CATCOL(PCOLDEF cdp, PTDB tdbp, int n);

  // Implementation
  virtual int  GetAmType(void) {return TYPE_AM_ODBC;}

  // Methods
  virtual void ReadColumn(PGLOBAL g);

 protected:
  CATCOL(void) {}              // Default constructor not to be used

  // Members
  PTDBCAT Tdbp;                // Points to ODBC table block
  PCOLRES Crp;                // The column data array
  int     Flag;
  }; // end of class CATCOL

#endif  // TABLE_DEFINED
