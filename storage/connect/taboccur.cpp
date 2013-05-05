/************ TabOccur CPP Declares Source Code File (.CPP) ************/
/*  Name: TABOCCUR.CPP   Version 1.0                                   */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2013         */
/*                                                                     */
/*  OCCUR: Table that provides a view of a source table where the      */
/*  contain of several columns of the source table is placed in only   */
/*  one column, the OCCUR column, this resulting into several rows.    */
/***********************************************************************/

/***********************************************************************/
/*  Include relevant section of system dependant header files.         */
/***********************************************************************/
#include "my_global.h"
#if defined(WIN32)
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
#include "table.h"       // MySQL table definitions
#include "global.h"
#include "plgdbsem.h"
#include "reldef.h"
#include "filamtxt.h"
#include "tabdos.h"
#include "tabcol.h"
#include "taboccur.h"
#include "xtable.h"
#if defined(MYSQL_SUPPORT)
#include "tabmysql.h"
#endif   // MYSQL_SUPPORT
#include "ha_connect.h"
#include "mycat.h"

extern "C" int trace;

/* -------------- Implementation of the OCCUR classes	---------------- */

/***********************************************************************/
/*  DefineAM: define specific AM block values from OCCUR table.        */
/***********************************************************************/
bool OCCURDEF::DefineAM(PGLOBAL g, LPCSTR am, int poff)
  {
  Xcol = Cat->GetStringCatInfo(g, "OccurCol", "");
  Rcol = Cat->GetStringCatInfo(g, "RankCol", "");
  Colist = Cat->GetStringCatInfo(g, "Colist", "");
  return PRXDEF::DefineAM(g, am, poff);
  } // end of DefineAM

/***********************************************************************/
/*  GetTable: makes a new TDB of the proper type.                      */
/***********************************************************************/
PTDB OCCURDEF::GetTable(PGLOBAL g, MODE m)
  {
  if (Catfunc != FNC_COL)
  	return new(g) TDBOCCUR(this);
  else
    return new(g) TDBTBC(this);

  } // end of GetTable

/* ------------------------------------------------------------------- */

/***********************************************************************/
/*  Implementation of the TDBOCCUR class.                              */
/***********************************************************************/
TDBOCCUR::TDBOCCUR(POCCURDEF tdp) : TDBPRX(tdp)
  {
//Tdbp = NULL;      			// Source table
  Tabname = tdp->Tablep->GetName();	 // Name of source table
	Colist = tdp->Colist;		// List of source columns
	Xcolumn = tdp->Xcol;		// Occur column name     
	Rcolumn = tdp->Rcol;		// Rank column name     
	Xcolp = NULL;						// To the OCCURCOL column
	Col = NULL;             // To source column blocks array
	Mult = -1;      				// Multiplication factor
	N = 0;									// The current table index
	M = 0;                  // The occurence rank
	RowFlag = 0;    				// 0: Ok, 1: Same, 2: Skip
  } // end of TDBOCCUR constructor

/***********************************************************************/
/*  Allocate OCCUR/SRC column description block.                       */
/***********************************************************************/
PCOL TDBOCCUR::MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n)
	{
	PCOL colp = NULL;

	if (!stricmp(cdp->GetName(), Rcolumn)) {
		// Allocate a RANK column
		colp = new(g) RANKCOL(cdp, this, n);
	} else if (!stricmp(cdp->GetName(), Xcolumn)) {
		// Allocate the OCCUR column
		colp = Xcolp = new(g) OCCURCOL(cdp, this, n);
	} else {
		colp = new(g) PRXCOL(cdp, this, cprec, n);

		if (((PPRXCOL)colp)->Init(g))
			return NULL;

    return colp;
	} //endif name

	if (cprec) {
		colp->SetNext(cprec->GetNext());
		cprec->SetNext(colp);
	} else {
		colp->SetNext(Columns);
		Columns = colp;
	} // endif cprec

	return colp;
	} // end of MakeCol

/***********************************************************************/
/*  Initializes the table.                                             */
/***********************************************************************/
bool TDBOCCUR::InitTable(PGLOBAL g)
  {
  if (!Tdbp) {
    // Get the table description block of this table
    if (!(Tdbp = (PTDBASE)GetSubTable(g, ((POCCURDEF)To_Def)->Tablep)))
      return TRUE;

		if (MakeColumnList(g) < 0)
      return TRUE;

    } // endif Tdbp

  return FALSE;
  } // end of InitTable

/***********************************************************************/
/*  Allocate OCCUR column description block.                           */
/***********************************************************************/
int TDBOCCUR::MakeColumnList(PGLOBAL g)
	{
	if (Mult < 0) {
		char   *p, *pn;
		int     i;
		int    n = 0;

		// Count the number of columns and change separator into null char
		for (pn = Colist; ; pn += (strlen(pn) + 1))
			if ((p = strchr(pn, ',')) || (p = strchr(pn, ';'))) {
				*p++ = '\0';
				n++;
			} else {
				if (*pn)
					n++;

				break;
			} // endif p

		Col = (PCOL*)PlugSubAlloc(g, NULL, n * sizeof(PCOL));

		for (i = 0, pn = Colist; i < n; i++, pn += (strlen(pn) + 1)) {
			if (!(Col[i] = Tdbp->ColDB(g, pn, 0))) {
			  // Column not found in table                                       
			  sprintf(g->Message, MSG(COL_ISNOT_TABLE), pn, Tabname);
				return -1;
				} // endif Col

			if (Col[i]->InitValue(g)) {
		    strcpy(g->Message, "OCCUR InitValue failed");
				return -1;
		    } // endif InitValue

			} // endfor i

    // OCCUR column name defaults to the name of the list first column 
    if (!Xcolumn)
      Xcolumn = Colist;

		Mult = n;
		} // endif Mult

	return Mult;
	} // end of MakeColumnList

/***********************************************************************/
/*  OCCUR GetMaxSize: returns the maximum number of rows in the table. */
/***********************************************************************/
int TDBOCCUR::GetMaxSize(PGLOBAL g)
  {
  if (MaxSize < 0) {
    if (InitTable(g))
      return NULL;
  
		MaxSize = Mult * Tdbp->GetMaxSize(g);
		} // endif MaxSize

  return MaxSize;
  } // end of GetMaxSize

/***********************************************************************/
/*  In this sample, ROWID will be the (virtual) row number,            */
/*  while ROWNUM will be the occurence rank in the multiple column.    */
/***********************************************************************/
int TDBOCCUR::RowNumber(PGLOBAL g, bool b)
	{
	return (b) ? M : N;
	} // end of RowNumber
 
/***********************************************************************/
/*  OCCUR Access Method opening routine.                               */
/***********************************************************************/
bool TDBOCCUR::OpenDB(PGLOBAL g)
  {
  if (Use == USE_OPEN) {
    /*******************************************************************/
    /*  Table already open, just replace it at its beginning.          */
    /*******************************************************************/
		N = M = 0;
		RowFlag = 0;

    if (Xcolp)
  		Xcolp->Xreset();

		return Tdbp->OpenDB(g);
    } // endif use

  
  if (Mode != MODE_READ) {
    /*******************************************************************/
    /* Currently OCCUR tables cannot be modified.                      */
    /*******************************************************************/
    strcpy(g->Message, "OCCUR tables are read only");
    return TRUE;
    } // endif Mode

  /*********************************************************************/
  /*  Do it here if not done yet.                                      */
  /*********************************************************************/
  if (InitTable(g))
    return NULL;

  if (Xcolp)
	  // Lock this column so it is evaluated by its table only
  	Xcolp->AddStatus(BUF_READ);

  if (To_Key_Col || To_Kindex) {
    /*******************************************************************/
    /* Direct access of OCCUR tables is not implemented yet.           */
    /*******************************************************************/
    strcpy(g->Message, "No direct access to OCCUR tables");
    return TRUE;
    } // endif To_Key_Col

  /*********************************************************************/
  /*  Do open the source table.                                        */
  /*********************************************************************/
	return Tdbp->OpenDB(g);
  } // end of OpenDB

/***********************************************************************/
/*  Data Base read routine for OCCUR access method.                    */
/***********************************************************************/
int TDBOCCUR::ReadDB(PGLOBAL g)
  {
	int rc = RC_OK;

  /*********************************************************************/
  /*  Now start the multi reading process.                             */
  /*********************************************************************/
	do {
		if (RowFlag != 1)
			if ((rc = Tdbp->ReadDB(g)) != RC_OK)
				break;

    if (Xcolp) {
  		RowFlag = 0;
	  	Xcolp->ReadColumn(g);
		  M = Xcolp->GetI();
      } // endif Xcolp

		} while (RowFlag == 2);

	N++;
	return rc;
  } // end of ReadDB

// ------------------------ OCCURCOL functions ----------------------------

/***********************************************************************/
/*  OCCURCOL public constructor.                                       */
/***********************************************************************/
OCCURCOL::OCCURCOL(PCOLDEF cdp, PTDBOCCUR tdbp, int n)
				: COLBLK(cdp, tdbp, n)
  {
  // Set additional OCCUR access method information for column.
	I = 0;
  } // end of OCCURCOL constructor

/***********************************************************************/
/*  ReadColumn: what this routine does is to access the columns of     */
/*  list, extract their value and convert it to buffer type.           */
/***********************************************************************/
void OCCURCOL::ReadColumn(PGLOBAL g)
  {
	PTDBOCCUR tdbp = (PTDBOCCUR)To_Tdb;
	PCOL     *col = tdbp->Col;

	for (; I < tdbp->Mult; I++) {
		col[I]->ReadColumn(g);

		if (Nullable || !col[I]->GetValue()->IsZero())
			break;

		} // endfor I

	if (I == tdbp->Mult) {
		// No more values, go to next source row
		tdbp->RowFlag = 2;
		I = 0;
		return;
		} // endif I

	// Set the OCCUR column value from the Ith source column value
	Value->SetValue_pval(col[I++]->GetValue());
	tdbp->RowFlag = 1;
  } // end of ReadColumn


// ------------------------ RANKCOL functions ---------------------------

/***********************************************************************/
/*  ReadColumn: what this routine does is to access the Mth columns of */
/*  list, extract its name and set to it the rank column value.        */
/***********************************************************************/
void RANKCOL::ReadColumn(PGLOBAL g)
  {
	PTDBOCCUR tdbp = (PTDBOCCUR)To_Tdb;
	PCOL     *col = tdbp->Col;

	// Set the RANK column value from the Mth source column name
  if (tdbp->M)
  	Value->SetValue_psz(col[tdbp->M - 1]->GetName());
  else {
    Value->Reset();

    if (Nullable)
      Value->SetNull(true);

    } // endelse

  } // end of ReadColumn
