/**************** tdbvir H Declares Source Code File (.H) **************/
/*  Name: TDBVIR.H  Version 1.1                                        */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2006-2014    */
/*                                                                     */
/*  This file contains the VIR classes declare code.                   */
/***********************************************************************/
typedef class VIRDEF   *PVIRDEF;
typedef class TDBVIR   *PTDBVIR;

/***********************************************************************/
/*  Return the unique column definition to MariaDB.                    */
/***********************************************************************/
PQRYRES VirColumns(PGLOBAL g, bool info);

/* --------------------------- VIR classes --------------------------- */

/***********************************************************************/
/*  VIR: Virtual table used to select constant values.                 */
/***********************************************************************/
class DllExport VIRDEF : public TABDEF {  /* Logical table description */
 public:
  // Constructor
	VIRDEF(void) {}

  // Implementation
  virtual const char *GetType(void) {return "VIRTUAL";}

  // Methods
  virtual bool DefineAM(PGLOBAL, LPCSTR, int) {Pseudo = 3; return false;}
  virtual int  Indexable(void) {return 3;}
  virtual PTDB GetTable(PGLOBAL g, MODE m);

 protected:
  // Members
  }; // end of VIRDEF

/***********************************************************************/
/*  This is the class declaration for the Virtual table.               */
/***********************************************************************/
class DllExport TDBVIR : public TDBASE {
 public:
  // Constructors
	TDBVIR(PVIRDEF tdp);

  // Implementation
  virtual AMT  GetAmType(void) {return TYPE_AM_VIR;}

  // Methods
  virtual int  GetRecpos(void) {return N;}
  virtual bool SetRecpos(PGLOBAL g, int recpos)
               {N = recpos - 2; return false;}
	virtual int  RowNumber(PGLOBAL g, bool b = false) {return N + 1;}
          int  TestFilter(PFIL filp, bool nop);

  // Database routines
	virtual PCOL MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n);
  virtual int  Cardinality(PGLOBAL g) {return (g) ? Size : 1;}
  virtual int  GetMaxSize(PGLOBAL g) {return Size;}
  virtual bool OpenDB(PGLOBAL g);
  virtual int  ReadDB(PGLOBAL g);
  virtual int  WriteDB(PGLOBAL g);
  virtual int  DeleteDB(PGLOBAL g, int irc);
	virtual void CloseDB(PGLOBAL g) {}

 protected:
  // Members
	int     Size;											// Table size
	int     N;                        // The VIR table current position
  }; // end of class TDBVIR

/***********************************************************************/
/*  Class VIRCOL: VIRTUAL access method column descriptor.             */
/***********************************************************************/
class VIRCOL : public COLBLK {
  friend class TDBVIR;
 public:
  // Constructors
  VIRCOL(PCOLDEF cdp, PTDB tdbp, PCOL cprec, int i, PCSZ am = "VIRTUAL");

  // Implementation
  virtual int  GetAmType(void) {return TYPE_AM_VIR;}

  // Methods
  virtual void ReadColumn(PGLOBAL g);

 protected:
  // Default constructor not to be used
  VIRCOL(void) {}

  // No additional members
  }; // end of class VIRCOL

/***********************************************************************/
/*  This is the class declaration for the VIR column catalog table.    */
/***********************************************************************/
class TDBVICL : public TDBCAT {
 public:
  // Constructor
  TDBVICL(PVIRDEF tdp) : TDBCAT(tdp) {}

  // Methods
  virtual int  Cardinality(PGLOBAL g) {return 2;} // Avoid DBUG_ASSERT

 protected:
	// Specific routines
	virtual PQRYRES GetResult(PGLOBAL g);

  // Members
  }; // end of class TDBVICL
