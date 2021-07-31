/************* TabXcl CPP Declares Source Code File (.CPP) *************/
/*  Name: TABXCL.CPP   Version 1.0                                     */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2013-2017    */
/*                                                                     */
/*  XCOL: Table having one column containing several values            */
/*  comma separated. When creating the table, the name of the X        */
/*  column is given by the Name option.       					               */
/*  This first version has one limitation:                             */
/*  - The X column has the same length than in the physical file.      */
/*  This tables produces as many rows for a physical row than the      */
/*  number of items in the X column (eventually 0).                    */
/***********************************************************************/

/***********************************************************************/
/*  Include relevant section of system dependant header files.         */
/***********************************************************************/
#include "my_global.h"
#include "table.h"       // MySQL table definitions
#if defined(_WIN32)
#include <stdlib.h>
#include <stdio.h>
#if defined(__BORLANDC__)
#define __MFC_COMPAT__                   // To define min/max as macro
#endif
//#include <windows.h>
#else
#if defined(UNIX)
#include <fnmatch.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "osutil.h"
#else
//#include <io.h>
#endif
//#include <fcntl.h>
#endif

/***********************************************************************/
/*  Include application header files:                                  */
/***********************************************************************/
#include "global.h"
#include "plgdbsem.h"
#include "plgcnx.h"                       // For DB types
#include "resource.h"
#include "xtable.h"
#include "tabext.h"
#include "filamtxt.h"
#include "tabdos.h"
#include "tabcol.h"
#include "tabxcl.h"
#include "tabmysql.h"
#include "ha_connect.h"

/* -------------- Implementation of the XCOL classes	---------------- */

/***********************************************************************/
/*  XCLDEF constructor.                                                */
/***********************************************************************/
XCLDEF::XCLDEF(void)
  {
  Xcol = NULL;
  Sep = ',';
  Mult = 10;
} // end of XCLDEF constructor

/***********************************************************************/
/*  DefineAM: define specific AM block values from XCOL table.         */
/***********************************************************************/
bool XCLDEF::DefineAM(PGLOBAL g, LPCSTR am, int poff)
  {
  char buf[8];

  Xcol = GetStringCatInfo(g, "Colname", "");
  GetCharCatInfo("Separator", ",", buf, sizeof(buf));
  Sep = (strlen(buf) == 2 && buf[0] == '\\' && buf[1] == 't') ? '\t' : *buf;
  Mult = GetIntCatInfo("Mult", 10);
  return PRXDEF::DefineAM(g, am, poff);
  } // end of DefineAM

/***********************************************************************/
/*  GetTable: makes a new TDB of the proper type.                      */
/***********************************************************************/
PTDB XCLDEF::GetTable(PGLOBAL g, MODE)
  {
  if (Catfunc == FNC_COL)
    return new(g) TDBTBC(this);
  else
    return new(g) TDBXCL(this);

  } // end of GetTable

/* ------------------------------------------------------------------- */

/***********************************************************************/
/*  Implementation of the TDBXCL class.                                */
/***********************************************************************/
TDBXCL::TDBXCL(PXCLDEF tdp) : TDBPRX(tdp)
  {
	Xcolumn = tdp->Xcol;						// CSV column name     
	Xcolp = NULL;										// To the XCLCOL column
	Mult = tdp->Mult;								// Multiplication factor
	N = 0;													// The current table index
	M = 0;                          // The occurrence rank
	RowFlag = 0;    								// 0: Ok, 1: Same, 2: Skip
	New = TRUE;						          // TRUE for new line
	Sep = tdp->Sep;                 // The Xcol separator
  } // end of TDBXCL constructor

/***********************************************************************/
/*  Allocate XCL column description block.                             */
/***********************************************************************/
PCOL TDBXCL::MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n)
  {
  PCOL colp;
  
  if (!stricmp(cdp->GetName(), Xcolumn)) {
		Xcolp = new(g) XCLCOL(cdp, this, cprec, n);
    colp = Xcolp;
  } else
    colp = new(g) PRXCOL(cdp, this, cprec, n);

  return colp;
  } // end of MakeCol

/***********************************************************************/
/*  XCL GetMaxSize: returns the maximum number of rows in the table.   */
/***********************************************************************/
int TDBXCL::GetMaxSize(PGLOBAL g)
  {
  if (MaxSize < 0) {
    if (InitTable(g))
      return 0;
  
  	MaxSize = Mult * Tdbp->GetMaxSize(g);
    } // endif MaxSize

  return MaxSize;
  } // end of GetMaxSize

/***********************************************************************/
/*  For this table type, ROWID is the (virtual) row number,            */
/*  while ROWNUM is be the occurrence rank in the multiple column.      */
/***********************************************************************/
int TDBXCL::RowNumber(PGLOBAL, bool b)
	{
	return (b) ? M : N;
	} // end of RowNumber
 
/***********************************************************************/
/*  XCL Access Method opening routine.                                 */
/***********************************************************************/
bool TDBXCL::OpenDB(PGLOBAL g)
  {
  if (Use == USE_OPEN) {
    /*******************************************************************/
    /*  Table already open, just replace it at its beginning.          */
    /*******************************************************************/
		M = N = 0;
		RowFlag = 0;
    New = TRUE;
		return Tdbp->OpenDB(g);
    } // endif use

  if (Mode != MODE_READ) {
    /*******************************************************************/
    /* Currently XCOL tables cannot be modified.                       */
    /*******************************************************************/
    strcpy(g->Message, "XCOL tables are read only");
    return TRUE;
    } // endif Mode

  if (InitTable(g))
    return TRUE;
  
  /*********************************************************************/
  /*  Check and initialize the subtable columns.                       */
  /*********************************************************************/
  for (PCOL cp = Columns; cp; cp = cp->GetNext())
    if (!cp->IsSpecial())
      if (((PPRXCOL)cp)->Init(g, NULL))
        return TRUE;

  /*********************************************************************/
  /*  Physically open the object table.                                */
  /*********************************************************************/
	if (Tdbp->OpenDB(g))
		return TRUE;

  Use = USE_OPEN;
	return FALSE;
  } // end of OpenDB

/***********************************************************************/
/*  Data Base read routine for XCL access method.                      */
/***********************************************************************/
int TDBXCL::ReadDB(PGLOBAL g)
  {
	int rc = RC_OK;

  /*********************************************************************/
  /*  Now start the multi reading process.                             */
  /*********************************************************************/
	do {
		if (RowFlag != 1) {
			if ((rc = Tdbp->ReadDB(g)) != RC_OK)
				break;

      New = TRUE;
			M = 1;
    } else {
      New = FALSE;
			M++;
    } // endif RowFlag

    if (Xcolp) {
  		RowFlag = 0;
	  	Xcolp->ReadColumn(g);
      } // endif Xcolp

  	N++;
		} while (RowFlag == 2);

	return rc;
  } // end of ReadDB


// ------------------------ XCLCOL functions ----------------------------

/***********************************************************************/
/*  XCLCOL public constructor.                                         */
/***********************************************************************/
XCLCOL::XCLCOL(PCOLDEF cdp, PTDB tdbp, PCOL cprec, int i)
			: PRXCOL(cdp, tdbp, cprec, i, "XCL")
  {
  // Set additional XXL access method information for column.
  Cbuf = NULL;                // Will be allocated later
	Cp = NULL;						      // Pointer to current position in Cbuf
	Sep = ((PTDBXCL)tdbp)->Sep;
	AddStatus(BUF_READ);	      // Only evaluated from TDBXCL::ReadDB
  } // end of XCLCOL constructor

/***********************************************************************/
/*  XCLCOL initialization routine.                                     */
/*  Allocate Cbuf that will contain the Colp value.                    */
/***********************************************************************/
bool XCLCOL::Init(PGLOBAL g, PTDB tp)
  {
  if (PRXCOL::Init(g, tp))
    return true;

  Cbuf = (char*)PlugSubAlloc(g, NULL, Colp->GetLength() + 1);
  return false;
  } // end of Init

/***********************************************************************/
/*  What this routine does is to get the comma-separated string        */
/*  from the source table column, extract the single values and        */
/*  set the flag for the table ReadDB function.                        */
/***********************************************************************/
void XCLCOL::ReadColumn(PGLOBAL g)
  {
	if (((PTDBXCL)To_Tdb)->New) {
    Colp->Reset();           // Moved here in case of failed filtering
		Colp->Eval(g);
		strncpy(Cbuf, To_Val->GetCharValue(), Colp->GetLength());
    Cbuf[Colp->GetLength()] = 0;
		Cp = Cbuf;
		} // endif New

	if (*Cp) {
		PSZ p;

    // Trim left
    for (p = Cp; *p == ' '; p++)
      ;

		if ((Cp = strchr(Cp, Sep)))
			// Separator is found
			*Cp++ = '\0';

		Value->SetValue_psz(p);
  } else if (Nullable) {
    Value->Reset();
    Value->SetNull(true);
  } else {
    // Skip that row
		((PTDBXCL)To_Tdb)->RowFlag = 2;
    Colp->Reset();
  } // endif Cp
	
	if (Cp && *Cp)
		// More to come from the same row
		((PTDBXCL)To_Tdb)->RowFlag = 1;

  } // end of ReadColumn
