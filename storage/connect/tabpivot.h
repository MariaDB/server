/************** TabPivot H Declares Source Code File (.H) **************/
/*  Name: TABPIVOT.H    Version 1.5                                    */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2005-2013    */
/*                                                                     */
/*  This file contains the PIVOT classes declares.                     */
/***********************************************************************/
typedef class PIVOTDEF *PPIVOTDEF;
typedef class TDBPIVOT *PTDBPIVOT;
typedef class FNCCOL   *PFNCCOL;
typedef class SRCCOL   *PSRCCOL;

/***********************************************************************/
/*  This class is used to generate PIVOT table column definitions.     */
/***********************************************************************/
class PIVAID : public CSORT {
  friend class FNCCOL;
  friend class SRCCOL;
 public:
  // Constructor
  PIVAID(const char *tab,   const char *src,   const char *picol,
         const char *fncol, const char *skcol, const char *host, 
         const char *db,    const char *user,  const char *pwd,  int port);

  // Methods
  PQRYRES MakePivotColumns(PGLOBAL g);
  bool    SkipColumn(PCOLRES crp, char *skc);

  // The sorting function
  virtual int  Qcompare(int *, int *);

 protected:
  // Members
  MYSQLC  Myc;                      // MySQL connection class
  PCSZ    Host;                     // Host machine to use
	PCSZ    User;                     // User logon info
	PCSZ    Pwd;                      // Password logon info
	PCSZ    Database;                 // Database to be used by server
  PQRYRES Qryp;                     // Points to Query result block
	PCSZ    Tabname;                  // Name of source table
	PCSZ    Tabsrc;                   // SQL of source table
	PCSZ    Picol;                    // Pivot column name
	PCSZ    Fncol;                    // Function column name
	PCSZ    Skcol;                    // Skipped columns
  PVBLK   Rblkp;                    // The value block of the pivot column
  int     Port;                     // MySQL port number
  }; // end of class PIVAID

/* -------------------------- PIVOT classes -------------------------- */

/***********************************************************************/
/*  PIVOT: table that provides a view of a source table where the      */
/*  pivot column is expended in as many columns as there are distinct  */
/*  values in it and containing the function value matching other cols.*/
/***********************************************************************/

/***********************************************************************/
/*  PIVOT table.                                                       */
/***********************************************************************/
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
class TDBPIVOT : public TDBPRX {
  friend class FNCCOL;
 public:
  // Constructor
  TDBPIVOT(PPIVOTDEF tdp);

  // Implementation
  virtual AMT  GetAmType(void) {return TYPE_AM_PIVOT;}

  // Methods
  virtual int  GetRecpos(void) {return N;}
  virtual void ResetDB(void) {N = 0;}
  virtual int  RowNumber(PGLOBAL g, bool b = FALSE);

  // Database routines
  virtual PCOL MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n);
  virtual int  Cardinality(PGLOBAL g) {return (g) ? 10 : 0;}
  virtual int  GetMaxSize(PGLOBAL g);
  virtual bool OpenDB(PGLOBAL g);
  virtual int  ReadDB(PGLOBAL g);
  virtual int  WriteDB(PGLOBAL g);
  virtual int  DeleteDB(PGLOBAL g, int irc);
  virtual void CloseDB(PGLOBAL g);

 protected:
  // Internal routines
          bool FindDefaultColumns(PGLOBAL g);
          bool GetSourceTable(PGLOBAL g);
          bool MakePivotColumns(PGLOBAL g);
          bool MakeViewColumns(PGLOBAL g);

  // Members
  char   *Host;                   // Host machine to use
  char   *User;                   // User logon info
  char   *Pwd;                    // Password logon info
  char   *Database;               // Database to be used by server
  char   *Tabname;                // Name of source table
  char   *Tabsrc;                 // SQL of source table
  char   *Picol;                  // Pivot column  name
  char   *Fncol;                  // Function column name
  char   *Function;               // The function applying to group by
  PCOL    Fcolp;                  // To the function column in source
  PCOL    Xcolp;                  // To the pivot column in source
  PCOL    Dcolp;                  // To the dump column
  bool    GBdone;                 // True when subtable is "Group by"
  bool    Accept;                 // TRUE if no match is accepted
  int     Mult;                   // Multiplication factor
  int     Ncol;                   // The number of generated columns
  int     N;                      // The current table index
  int     M;                      // The occurrence rank
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
  using PRXCOL::Init;
  virtual void Reset(void) {}
          void SetColumn(void);
  virtual bool Init(PGLOBAL g, PTDB tp);
          bool CompareLast(void);

 protected:
  // Default constructor not to be used
  SRCCOL(void) {}

  // Members
  }; // end of class SRCCOL

PQRYRES PivotColumns(PGLOBAL g, const char *tab,   const char *src,
                                const char *picol, const char *fncol,
                                const char *skcol, const char *host,
                                const char *db,    const char *user,
                                const char *pwd,   int port);
