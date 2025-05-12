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
	VIRDEF(void) = default;

  // Implementation
  const char *GetType(void) override {return "VIRTUAL";}

  // Methods
  bool DefineAM(PGLOBAL, LPCSTR, int) override {Pseudo = 3; return false;}
  int  Indexable(void) override {return 3;}
  PTDB GetTable(PGLOBAL g, MODE m) override;

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
  AMT  GetAmType(void) override {return TYPE_AM_VIR;}

  // Methods
  int  GetRecpos(void) override {return N;}
  bool SetRecpos(PGLOBAL g, int recpos) override
               {N = recpos - 2; return false;}
	int  RowNumber(PGLOBAL g, bool b = false) override {return N + 1;}
          int  TestFilter(PFIL filp, bool nop);

  // Database routines
	PCOL MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n) override;
  int  Cardinality(PGLOBAL g) override {return (g) ? Size : 1;}
  int  GetMaxSize(PGLOBAL g) override {return Size;}
  bool OpenDB(PGLOBAL g) override;
  int  ReadDB(PGLOBAL g) override;
  int  WriteDB(PGLOBAL g) override;
  int  DeleteDB(PGLOBAL g, int irc) override;
	void CloseDB(PGLOBAL g) override {}

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
  int  GetAmType(void) override {return TYPE_AM_VIR;}

  // Methods
  void ReadColumn(PGLOBAL g) override;

 protected:
  // Default constructor not to be used
  VIRCOL(void) = default;

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
  int  Cardinality(PGLOBAL g) override {return 2;} // Avoid DBUG_ASSERT

 protected:
	// Specific routines
	PQRYRES GetResult(PGLOBAL g) override;

  // Members
  }; // end of class TDBVICL
