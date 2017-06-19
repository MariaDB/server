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
  virtual const char *GetType(void) {return "INI";}

  // Methods
  virtual bool DefineAM(PGLOBAL g, LPCSTR am, int poff);
  virtual PTDB GetTable(PGLOBAL g, MODE m);

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
  virtual AMT   GetAmType(void) {return TYPE_AM_INI;}
  virtual PTDB  Duplicate(PGLOBAL g) {return (PTDB)new(g) TDBINI(this);}

  // Methods
  virtual PTDB  Clone(PTABS t);
  virtual int   GetRecpos(void) {return N;}
  virtual int   GetProgCur(void) {return N;}
//virtual int   GetAffectedRows(void) {return 0;}
  virtual PCSZ  GetFile(PGLOBAL g) {return Ifile;}
  virtual void  SetFile(PGLOBAL g, PCSZ fn) {Ifile = fn;}
  virtual void  ResetDB(void) {Seclist = Section = NULL; N = 0;}
  virtual void  ResetSize(void) {MaxSize = -1; Seclist = NULL;}
  virtual int   RowNumber(PGLOBAL g, bool b = false) {return N;}
          char *GetSeclist(PGLOBAL g);

  // Database routines
  virtual PCOL  MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n);
  virtual int   Cardinality(PGLOBAL g);
  virtual int   GetMaxSize(PGLOBAL g);
  virtual bool  OpenDB(PGLOBAL g);
  virtual int   ReadDB(PGLOBAL g);
  virtual int   WriteDB(PGLOBAL g);
  virtual int   DeleteDB(PGLOBAL g, int irc);
  virtual void  CloseDB(PGLOBAL g);

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
  virtual int  GetAmType(void) {return TYPE_AM_INI;}
  virtual void SetTo_Val(PVAL valp) {To_Val = valp;}

  // Methods
  virtual bool SetBuffer(PGLOBAL g, PVAL value, bool ok, bool check);
  virtual void ReadColumn(PGLOBAL g);
  virtual void WriteColumn(PGLOBAL g);
  virtual void AllocBuf(PGLOBAL g);

 protected:
  // Default constructor not to be used
  INICOL(void) {}

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
  virtual AMT   GetAmType(void) {return TYPE_AM_INI;}
  virtual PTDB  Duplicate(PGLOBAL g) {return (PTDB)new(g) TDBXIN(this);}

  // Methods
  virtual PTDB  Clone(PTABS t);
  virtual int   GetRecpos(void);
  virtual bool  SetRecpos(PGLOBAL g, int recpos);
  virtual void  ResetDB(void)
                {Seclist = Section = Keycur = NULL; N = 0; Oldsec = -1;}
          char *GetKeylist(PGLOBAL g, char *sec);

  // Database routines
  virtual PCOL  MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n);
  virtual int   Cardinality(PGLOBAL g);
  virtual bool  OpenDB(PGLOBAL g);
  virtual int   ReadDB(PGLOBAL g);
  virtual int   WriteDB(PGLOBAL g);
  virtual int   DeleteDB(PGLOBAL g, int irc);

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
  virtual void ReadColumn(PGLOBAL g);
  virtual void WriteColumn(PGLOBAL g);

 protected:
  // Default constructor not to be used
  XINCOL(void) {}

  // Members
  }; // end of class XINICOL
