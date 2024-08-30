// TABOCCUR.H     Olivier Bertrand    2013
// Defines the OCCUR tables

#include "tabutil.h"

typedef class OCCURDEF *POCCURDEF;
typedef class TDBOCCUR *PTDBOCCUR;
typedef class OCCURCOL *POCCURCOL;
typedef class RANKCOL *PRANKCOL;

/* -------------------------- OCCUR classes -------------------------- */

/***********************************************************************/
/*  OCCUR: Table that provides a view of a source table where the      */
/*  contain of several columns of the source table is placed in only   */
/*  one column, the OCCUR column, this resulting into several rows.    */
/***********************************************************************/

/***********************************************************************/
/*  OCCUR table.                                                       */
/***********************************************************************/
class OCCURDEF : public PRXDEF {          /* Logical table description */
	friend class TDBOCCUR;
 public:
  // Constructor
	OCCURDEF(void) {Pseudo = 3; Colist = Xcol = NULL;}

  // Implementation
  const char *GetType(void) override {return "OCCUR";}

  // Methods
  bool DefineAM(PGLOBAL g, LPCSTR am, int poff) override;
  PTDB GetTable(PGLOBAL g, MODE m) override;

 protected:
  // Members
	char   *Colist;						 /* The source column list                 */
  char   *Xcol;              /* The multiple occurrence column          */
  char   *Rcol;              /* The rank column                        */
  }; // end of OCCURDEF

/***********************************************************************/
/*  This is the class declaration for the OCCUR table.                 */
/***********************************************************************/
class TDBOCCUR : public TDBPRX {
  friend class OCCURCOL;
  friend class RANKCOL;
 public:
  // Constructor
  TDBOCCUR(POCCURDEF tdp);

  // Implementation
  AMT  GetAmType(void) override {return TYPE_AM_OCCUR;}
  				void SetTdbp(PTDBASE tdbp) {Tdbp = tdbp;}

  // Methods
	void ResetDB(void) override {N = 0; Tdbp->ResetDB();}
	int  RowNumber(PGLOBAL g, bool b = FALSE) override;
					bool MakeColumnList(PGLOBAL g);
          bool ViewColumnList(PGLOBAL g);

  // Database routines
	PCOL MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n) override;
  bool InitTable(PGLOBAL g) override;
  int  GetMaxSize(PGLOBAL g) override;
  bool OpenDB(PGLOBAL g) override;
  int  ReadDB(PGLOBAL g) override;

 protected:
  // Members
  LPCSTR    Tabname;								// Name of source table
	char     *Colist;                 // Source column list
	char     *Xcolumn;								// Occurence column name
	char     *Rcolumn;								// Rank column name
	POCCURCOL Xcolp;									// To the OCCURCOL column
	PCOL     *Col;									  // To source multiple columns
	int       Mult;										// Multiplication factor
	int       N;											// The current table index
	int		    M;                      // The occurrence rank
	BYTE      RowFlag;								// 0: Ok, 1: Same, 2: Skip
  }; // end of class TDBOCCUR

/***********************************************************************/
/*  Class OCCURCOL: for the multiple occurrence column.                 */
/***********************************************************************/
class OCCURCOL : public COLBLK {
 public:
  // Constructors
  OCCURCOL(PCOLDEF cdp, PTDBOCCUR tdbp, int n);

  // Implementation
  int  GetAmType(void) override {return TYPE_AM_OCCUR;}
					int  GetI(void) {return I;}

  // Methods
  void Reset(void) override {}		// Evaluated only by TDBOCCUR
  void ReadColumn(PGLOBAL g) override;
					void Xreset(void) {I = 0;};

 protected:
  // Default constructor not to be used
  OCCURCOL(void) = default;

  // Members
	int    I;
  }; // end of class OCCURCOL

/***********************************************************************/
/*  Class RANKCOL: for the multiple occurrence column ranking.          */
/***********************************************************************/
class RANKCOL : public COLBLK {
 public:
  // Constructors
	RANKCOL(PCOLDEF cdp, PTDBOCCUR tdbp, int n) : COLBLK(cdp, tdbp, n) {}

  // Implementation
  int  GetAmType(void) override {return TYPE_AM_OCCUR;}

  // Methods
  void ReadColumn(PGLOBAL g) override;

 protected:
  // Default constructor not to be used
  RANKCOL(void) = default;

  // Members
  }; // end of class RANKCOL

/***********************************************************************/
/*  Definition of class XCOLDEF.                                       */
/*  This class purpose is just to access COLDEF protected items!       */
/***********************************************************************/
class XCOLDEF: public COLDEF {
	friend class TDBOCCUR;
	}; // end of class XCOLDEF


bool OcrColumns(PGLOBAL g, PQRYRES qrp, const char *col, 
                       const char *ocr, const char *rank);

bool OcrSrcCols(PGLOBAL g, PQRYRES qrp, const char *col, 
                       const char *ocr, const char *rank);
