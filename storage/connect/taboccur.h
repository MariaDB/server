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
  virtual const char *GetType(void) {return "OCCUR";}

  // Methods
  virtual bool DefineAM(PGLOBAL g, LPCSTR am, int poff);
  virtual PTDB GetTable(PGLOBAL g, MODE m);

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
  virtual AMT  GetAmType(void) {return TYPE_AM_OCCUR;}
  				void SetTdbp(PTDBASE tdbp) {Tdbp = tdbp;}

  // Methods
	virtual void ResetDB(void) {N = 0; Tdbp->ResetDB();}
	virtual int  RowNumber(PGLOBAL g, bool b = FALSE);
					bool MakeColumnList(PGLOBAL g);
          bool ViewColumnList(PGLOBAL g);

  // Database routines
	virtual PCOL MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n);
  virtual bool InitTable(PGLOBAL g);
  virtual int  GetMaxSize(PGLOBAL g);
  virtual bool OpenDB(PGLOBAL g);
  virtual int  ReadDB(PGLOBAL g);

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
  virtual int  GetAmType(void) {return TYPE_AM_OCCUR;}
					int  GetI(void) {return I;}

  // Methods
  virtual void Reset(void) {}		// Evaluated only by TDBOCCUR
  virtual void ReadColumn(PGLOBAL g);
					void Xreset(void) {I = 0;};

 protected:
  // Default constructor not to be used
  OCCURCOL(void) {}

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
  virtual int  GetAmType(void) {return TYPE_AM_OCCUR;}

  // Methods
  virtual void ReadColumn(PGLOBAL g);

 protected:
  // Default constructor not to be used
  RANKCOL(void) {}

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
