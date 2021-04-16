// TABXCL.H     Olivier Bertrand    2013
// Defines the XCOL tables

#include "tabutil.h"

typedef class XCLDEF  *PXCLDEF;
typedef class TDBXCL  *PTDBXCL;
typedef class XCLCOL  *PXCLCOL;

/* -------------------------- XCOL classes --------------------------- */

/***********************************************************************/
/*  XCOL: table having one column containing several values comma      */
/*  (or any other character) separated. When creating the table, the   */
/*  name of the X column is given by the NAME option.                  */
/*  This sample has a limitation:                                      */
/*  - The X column has the same length than in the physical file.      */
/*  This tables produces as many rows for a physical row than the      */
/*  number of items in the X column (eventually 0).                    */
/***********************************************************************/

/***********************************************************************/
/*  XCL table.                                                         */
/***********************************************************************/
class XCLDEF : public PRXDEF {            /* Logical table description */
	friend class TDBXCL;
 public:
  // Constructor
	 XCLDEF(void);

  // Implementation
  virtual const char *GetType(void) {return "XCL";}

  // Methods
  virtual bool DefineAM(PGLOBAL g, LPCSTR am, int poff);
  virtual PTDB GetTable(PGLOBAL g, MODE mode);

 protected:
  // Members
  char   *Xcol;              /* The column containing separated fields */
	char    Sep;							 /* The field separator, defaults to comma */
  int     Mult;              /* Multiplication factor                  */
  }; // end of XCLDEF

/***********************************************************************/
/*  This is the class declaration for the XCOL table.                  */
/***********************************************************************/
class TDBXCL : public TDBPRX {
  friend class XCLDEF;
  friend class PRXCOL;
  friend class XCLCOL;
 public:
  // Constructor
  TDBXCL(PXCLDEF tdp);

  // Implementation
  virtual AMT   GetAmType(void) {return TYPE_AM_XCOL;}

  // Methods
	virtual void  ResetDB(void) {N = 0; Tdbp->ResetDB();}
	virtual int   RowNumber(PGLOBAL g, bool b = FALSE);

  // Database routines
	virtual PCOL  MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n);
  virtual int   GetMaxSize(PGLOBAL g);
  virtual bool  OpenDB(PGLOBAL g);
  virtual int   ReadDB(PGLOBAL g);

 protected:
  // Members
	char   *Xcolumn;								// Multiple column name
	PXCLCOL Xcolp;									// To the XCVCOL column
	int     Mult;										// Multiplication factor
	int     N;											// The current table index
	int			M;                      // The occurrence rank
	BYTE    RowFlag;								// 0: Ok, 1: Same, 2: Skip
	bool    New;						        // TRUE for new line
	char    Sep;										// The Xcol separator
  }; // end of class TDBXCL

/***********************************************************************/
/*  Class XCLCOL: for the multiple CSV column.                         */
/***********************************************************************/
class XCLCOL : public PRXCOL {
  friend class TDBXCL;
 public:
  // Constructors
  XCLCOL(PCOLDEF cdp, PTDB tdbp, PCOL cprec, int i);

  // Methods
  using PRXCOL::Init;
  virtual void Reset(void) {}	  // Evaluated only by TDBXCL
  virtual void ReadColumn(PGLOBAL g);
  virtual bool Init(PGLOBAL g, PTDB tp = NULL);

 protected:
  // Default constructor not to be used
  XCLCOL(void) {}

  // Members
	char   *Cbuf;					        // The column buffer
	char   *Cp;						        // Pointer to current position
	char    Sep;					        // The separator
  }; // end of class XCLCOL
