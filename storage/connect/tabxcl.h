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
  const char *GetType(void) override {return "XCL";}

  // Methods
  bool DefineAM(PGLOBAL g, LPCSTR am, int poff) override;
  PTDB GetTable(PGLOBAL g, MODE mode) override;

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
  AMT   GetAmType(void) override {return TYPE_AM_XCOL;}

  // Methods
	void  ResetDB(void) override {N = 0; Tdbp->ResetDB();}
	int   RowNumber(PGLOBAL g, bool b = FALSE) override;

  // Database routines
	PCOL  MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n) override;
  int   GetMaxSize(PGLOBAL g) override;
  bool  OpenDB(PGLOBAL g) override;
  int   ReadDB(PGLOBAL g) override;

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
  void Reset(void) override {}	  // Evaluated only by TDBXCL
  void ReadColumn(PGLOBAL g) override;
  bool Init(PGLOBAL g, PTDB tp = NULL) override;

 protected:
  // Default constructor not to be used
  XCLCOL(void) = default;

  // Members
	char   *Cbuf;					        // The column buffer
	char   *Cp;						        // Pointer to current position
	char    Sep;					        // The separator
  }; // end of class XCLCOL
