/*************** TabSys H Declares Source Code File (.H) ***************/
/*  Name: TABSYS.H    Version 2.3                                      */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2004-2014    */
/*                                                                     */
/*  This file contains the XDB system tables classes declares.         */
/***********************************************************************/
typedef class INIDEF *PINIDEF;
typedef class TDBINI *PTDBINI;
typedef class INICOL *PINICOL;
typedef class TDBXIN *PTDBXIN;
typedef class XINCOL *PXINCOL;

/* --------------------------- INI classes --------------------------- */

/***********************************************************************/
/*  INI, XDB and XCL tables.                                           */
/***********************************************************************/
class DllExport INIDEF : public TABDEF {      /* INI table description */
  friend class TDBINI;
  friend class TDBXIN;
  friend class TDBXTB;
  friend class TDBRTB;
  friend class TDBXCL;
 public:
  // Constructor
  INIDEF(void);

  // Implementation
  const char *GetType(void) override {return "INI";}

  // Methods
  bool DefineAM(PGLOBAL g, LPCSTR am, int poff) override;
  PTDB GetTable(PGLOBAL g, MODE m) override;

 protected:
  // Members
  char   *Fn;                 /* Path/Name of corresponding file       */
  char   *Xname;              /* The eventual table name               */
  char    Layout;             /* R: Row, C: Column                     */
  int     Ln;                 /* Length of section list buffer         */
  }; // end of INIDEF

/***********************************************************************/
/*  This is the class declaration for the INI tables.                  */
/*  These are tables represented by a INI like file.                   */
/***********************************************************************/
class TDBINI : public TDBASE {
  friend class INICOL;
 public:
  // Constructor
  TDBINI(PINIDEF tdp);
  TDBINI(PTDBINI tdbp);

  // Implementation
  AMT   GetAmType(void) override {return TYPE_AM_INI;}
  PTDB  Duplicate(PGLOBAL g) override {return (PTDB)new(g) TDBINI(this);}

  // Methods
  PTDB  Clone(PTABS t) override;
  int   GetRecpos(void) override {return N;}
  int   GetProgCur(void) override {return N;}
//virtual int   GetAffectedRows(void) {return 0;}
  PCSZ  GetFile(PGLOBAL g) override {return Ifile;}
  void  SetFile(PGLOBAL g, PCSZ fn) override {Ifile = fn;}
  void  ResetDB(void) override {Seclist = Section = NULL; N = 0;}
  void  ResetSize(void) override {MaxSize = -1; Seclist = NULL;}
  int   RowNumber(PGLOBAL g, bool b = false) override {return N;}
          char *GetSeclist(PGLOBAL g);

  // Database routines
  PCOL  MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n) override;
  int   Cardinality(PGLOBAL g) override;
  int   GetMaxSize(PGLOBAL g) override;
  bool  OpenDB(PGLOBAL g) override;
  int   ReadDB(PGLOBAL g) override;
  int   WriteDB(PGLOBAL g) override;
  int   DeleteDB(PGLOBAL g, int irc) override;
  void  CloseDB(PGLOBAL g) override;

 protected:
  // Members
  PCSZ  Ifile;                               // The INI file
  char *Seclist;                             // The section list
  char *Section;                             // The current section
  int   Seclen;                              // Length of seclist buffer
  int   N;                                   // The current section index
  }; // end of class TDBINI

/***********************************************************************/
/*  Class INICOL: XDB table access method column descriptor.           */
/***********************************************************************/
class INICOL : public COLBLK {
 public:
  // Constructors
  INICOL(PCOLDEF cdp, PTDB tdbp, PCOL cprec, int i, PCSZ am = "INI");
  INICOL(INICOL *colp, PTDB tdbp); // Constructor used in copy process

  // Implementation
  int  GetAmType(void) override {return TYPE_AM_INI;}
  void SetTo_Val(PVAL valp) override {To_Val = valp;}

  // Methods
  bool SetBuffer(PGLOBAL g, PVAL value, bool ok, bool check) override;
  void ReadColumn(PGLOBAL g) override;
  void WriteColumn(PGLOBAL g) override;
  virtual void AllocBuf(PGLOBAL g);

 protected:
  // Default constructor not to be used
  INICOL(void) = default;

  // Members
  char *Valbuf;                   // To the key value buffer
  int   Flag;                     // Tells what set in value
  int   Long;                     // Buffer length
  PVAL  To_Val;                  // To value used for Update/Insert
  }; // end of class INICOL

/* --------------------------- XINI class ---------------------------- */

/***********************************************************************/
/*  This is the class declaration for the XINI tables.                 */
/*  These are tables represented by a INI like file                    */
/*  having 3 columns Section, Key, and Value.                          */
/***********************************************************************/
class TDBXIN : public TDBINI {
  friend class XINCOL;
 public:
  // Constructor
  TDBXIN(PINIDEF tdp);
  TDBXIN(PTDBXIN tdbp);

  // Implementation
  AMT   GetAmType(void) override {return TYPE_AM_INI;}
  PTDB  Duplicate(PGLOBAL g) override {return (PTDB)new(g) TDBXIN(this);}

  // Methods
  PTDB  Clone(PTABS t) override;
  int   GetRecpos(void) override;
  bool  SetRecpos(PGLOBAL g, int recpos) override;
  void  ResetDB(void) override
                {Seclist = Section = Keycur = NULL; N = 0; Oldsec = -1;}
          char *GetKeylist(PGLOBAL g, char *sec);

  // Database routines
  PCOL  MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n) override;
  int   Cardinality(PGLOBAL g) override;
  bool  OpenDB(PGLOBAL g) override;
  int   ReadDB(PGLOBAL g) override;
  int   WriteDB(PGLOBAL g) override;
  int   DeleteDB(PGLOBAL g, int irc) override;

 protected:
  // Members
  char *Keylist;                             // The key list
  char *Keycur;                              // The current key
  int   Keylen;                               // Length of keylist buffer
  short Oldsec;                               // Last current section
  }; // end of class TDBXIN

/***********************************************************************/
/*  Class XINCOL: XIN table access method column descriptor.           */
/***********************************************************************/
class XINCOL : public INICOL {
 public:
  // Constructors
  XINCOL(PCOLDEF cdp, PTDB tdbp, PCOL cprec, int i, PCSZ am = "INI");
  XINCOL(XINCOL *colp, PTDB tdbp); // Constructor used in copy process

  // Implementation

  // Methods
  void ReadColumn(PGLOBAL g) override;
  void WriteColumn(PGLOBAL g) override;

 protected:
  // Default constructor not to be used
  XINCOL(void) = default;

  // Members
  }; // end of class XINICOL
