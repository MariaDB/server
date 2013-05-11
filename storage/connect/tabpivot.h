/************** TabPivot H Declares Source Code File (.H) **************/
/*  Name: TABPIVOT.H    Version 1.3                                    */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2005-2012    */
/*                                                                     */
/*  This file contains the PIVOT classes declares.                     */
/***********************************************************************/
typedef class TDBPIVOT *PTDBPIVOT;
typedef class FNCCOL   *PFNCCOL;
typedef class SRCCOL   *PSRCCOL;
typedef class TDBQRS   *PTDBQRS;
typedef class QRSCOL   *PQRSCOL;

/* -------------------------- PIVOT classes -------------------------- */

/***********************************************************************/
/*  PIVOT: table that provides a view of a source table where the      */
/*  pivot column is expended in as many columns as there are distinct  */
/*  values in it and containing the function value matching other cols.*/
/***********************************************************************/

/***********************************************************************/
/*  PIVOT table.                                                       */
/***********************************************************************/
//ass DllExport PIVOTDEF : public PRXDEF {/* Logical table description */
class PIVOTDEF : public PRXDEF {          /* Logical table description */
  friend class TDBPIVOT;
 public:
  // Constructor
  PIVOTDEF(void);

  // Implementation
  virtual const char *GetType(void) {return "PIVOT";}

  // Methods
  virtual bool DefineAM(PGLOBAL g, LPCSTR am, int poff);
  virtual PTDB GetTable(PGLOBAL g, MODE m);

 protected:
  // Members
  char  *Host;               /* Host machine to use                    */
  char  *User;               /* User logon info                        */
  char  *Pwd;                /* Password logon info                    */
  char  *DB;                 /* Database to be used by server          */
  char  *Tabname;            /* Name of source table                   */
  char  *Tabsrc;             /* The source table SQL description       */
  char  *Picol;              /* The pivot column                       */
  char  *Fncol;              /* The function column                    */
  char  *Function;           /* The function applying to group by      */
  bool   GBdone;             /* True if tabname as group by format     */
  bool   Accept;             /* TRUE if no match is accepted           */
  int    Port;               /* MySQL port number                      */
  }; // end of PIVOTDEF

/***********************************************************************/
/*  This is the class declaration for the PIVOT table.                 */
/***********************************************************************/
//ass DllExport TDBPIVOT : public TDBASE, public CSORT {
class TDBPIVOT : public TDBPRX {
  friend class FNCCOL;
//friend class SRCCOL;
 public:
  // Constructor
  TDBPIVOT(PPIVOTDEF tdp);
//TDBPIVOT(PTDBPIVOT tdbp);

  // Implementation
  virtual AMT  GetAmType(void) {return TYPE_AM_PIVOT;}
//virtual PTDB Duplicate(PGLOBAL g) {return (PTDB)new(g) TDBPIVOT(this);}
//        void SetTdbp(PTDB tdbp) {Tdbp = tdbp;}

  // Methods
//virtual PTDB CopyOne(PTABS t);
  virtual int  GetRecpos(void) {return N;}
  virtual void ResetDB(void) {N = 0;}
  virtual int  RowNumber(PGLOBAL g, bool b = FALSE);

  // Database routines
  virtual PCOL MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n);
  virtual int  GetMaxSize(PGLOBAL g);
  virtual bool OpenDB(PGLOBAL g);
  virtual int  ReadDB(PGLOBAL g);
  virtual int  WriteDB(PGLOBAL g);
  virtual int  DeleteDB(PGLOBAL g, int irc);
  virtual void CloseDB(PGLOBAL g);

  // The sorting function
//virtual int  Qcompare(int *, int *);

 protected:
  bool    GetSourceTable(PGLOBAL g);
//int     MakePivotColumns(PGLOBAL g);
//bool    UpdateTableFields(PGLOBAL g, int n);

  // Members
//MYSQLC  Myc;                    // MySQL connection class
//PTDBQRS Tqrp;                   // To the source table result
  char   *Host;                   // Host machine to use
  char   *User;                   // User logon info
  char   *Pwd;                    // Password logon info
  char   *Database;               // Database to be used by server
//PQRYRES Qryp;                   // Points to Query result block
  char   *Tabname;                // Name of source table
  char   *Tabsrc;                 // SQL of source table
  char   *Picol;                  // Pivot column  name
  char   *Fncol;                  // Function column name
  char   *Function;               // The function applying to group by
  PCOL    Fcolp;                  // To the function column in source
  PCOL    Xcolp;                  // To the pivot column in source
  PCOL    Dcolp;                  // To the dump column
//PCOLRES Xresp;                  // To the pivot result column
//PCOLRES To_Sort;                // Saved Qryp To_Sort pointer
//PVBLK   Rblkp;                  // The value block of the pivot column
  bool    GBdone;                 // True when subtable is "Group by"
  bool    Accept;                 // TRUE if no match is accepted
  int     Mult;                   // Multiplication factor
  int     Ncol;                   // The number of generated columns
  int     N;                      // The current table index
  int     M;                      // The occurence rank
  int     Port;                   // MySQL port number 
  BYTE    FileStatus;             // 0: First 1: Rows 2: End-of-File
  BYTE    RowFlag;                // 0: Ok, 1: Same, 2: Skip
  }; // end of class TDBPIVOT

/***********************************************************************/
/*  Class FNCCOL: for the multiple generated column.                   */
/***********************************************************************/
class FNCCOL : public COLBLK {
  friend class TDBPIVOT;
 public:
  // Constructor
  FNCCOL(PCOLDEF cdp, PTDB tdbp, PCOL cprec, int i);

  // Implementation
  virtual int  GetAmType(void) {return TYPE_AM_FNC;}

  // Methods
  virtual void Reset(void) {}
          bool InitColumn(PGLOBAL g);
          bool CompareColumn(void);

 protected:
  // Member
  PVAL Hval;      // The value containing the header
  PCOL Xcolp;
  }; // end of class FNCCOL

/***********************************************************************/
/*  Class SRCCOL: for other source columns.                            */
/***********************************************************************/
class SRCCOL : public PRXCOL {
  friend class TDBPIVOT;
 public:
  // Constructors
  SRCCOL(PCOLDEF cdp, PTDB tdbp, PCOL cprec, int n);

  // Implementation
  virtual int  GetAmType(void) {return TYPE_AM_SRC;}

  // Methods
  virtual void Reset(void) {}
          void SetColumn(void);
          bool Init(PGLOBAL g);
          bool CompareLast(void);

 protected:
  // Default constructor not to be used
  SRCCOL(void) {}

  // Members
//PVAL    Cnval;
  }; // end of class SRCCOL

/***********************************************************************/
/*  TDBQRS: This is the Access Method class declaration for the Query  */
/*  Result stored in memory in the current work area (volatil).        */
/***********************************************************************/
class DllExport TDBQRS : public TDBASE {
  friend class QRSCOL;
 public:
  // Constructor
  TDBQRS(PQRYRES qrp) : TDBASE() {Qrp = qrp; CurPos = 0;}
  TDBQRS(PTDBQRS tdbp);

  // Implementation
  virtual AMT  GetAmType(void) {return TYPE_AM_QRS;}
  virtual PTDB Duplicate(PGLOBAL g)
    {return (PTDB)new(g) TDBQRS(this);}
          PQRYRES GetQrp(void) {return Qrp;}

  // Methods
  virtual PTDB   CopyOne(PTABS t);
  virtual int    RowNumber(PGLOBAL g, BOOL b = FALSE);
  virtual int    GetRecpos(void);
//virtual PCATLG GetCat(void);
//virtual PSZ    GetPath(void);
  virtual int    GetBadLines(void) {return Qrp->BadLines;}

  // Database routines
  virtual PCOL ColDB(PGLOBAL g, PSZ name, int num);
  virtual int  GetMaxSize(PGLOBAL g);
  virtual bool OpenDB(PGLOBAL g);
  virtual int  ReadDB(PGLOBAL g);
  virtual int  WriteDB(PGLOBAL g);
  virtual int  DeleteDB(PGLOBAL g, int irc);
  virtual void CloseDB(PGLOBAL g);

 private:
  TDBQRS(void) : TDBASE() {}   // Standard constructor not to be used

 protected:
  // Members
  PQRYRES Qrp;                 // Points to Query Result block
  int     CurPos;              // Current line position
  }; // end of class TDBQRS

/***********************************************************************/
/*  Class QRSCOL: QRS access method column descriptor.                 */
/***********************************************************************/
class DllExport QRSCOL : public COLBLK {
  friend class TDBQRS;
 public:
  // Constructors
  QRSCOL(PGLOBAL g, PCOLRES crp, PTDB tdbp, PCOL cprec, int i);
  QRSCOL(QRSCOL *colp, PTDB tdbp);  // Constructor used in copy process

  // Implementation
  virtual int     GetAmType(void) {return TYPE_AM_QRS;}
          PCOLRES GetCrp(void) {return Crp;}
          void   *GetQrsData(void) {return Crp->Kdata;}

  // Methods
  virtual void    ReadColumn(PGLOBAL g);
  virtual void    Print(PGLOBAL g, FILE *, uint);

 protected:
  QRSCOL(void) {}    // Default constructor not to be used

  // Members
  PCOLRES Crp;
  }; // end of class QRSCOL

