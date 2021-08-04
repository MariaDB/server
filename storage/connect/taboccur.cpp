/************ TabOccur CPP Declares Source Code File (.CPP) ************/
/*  Name: TABOCCUR.CPP   Version 1.2                                   */
/*                                                                     */
/*  (C) Copyright to the author Olivier BERTRAND          2013 - 2021  */
/*                                                                     */
/*  OCCUR: Table that provides a view of a source table where the      */
/*  contain of several columns of the source table is placed in only   */
/*  one column, the OCCUR column, this resulting into several rows.    */
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
#include "xtable.h"
#include "tabext.h"
//#include "reldef.h"
#include "filamtxt.h"
#include "tabdos.h"
#include "tabcol.h"
#include "taboccur.h"
#include "tabmysql.h"
#include "ha_connect.h"

int PrepareColist(char *colist);

/***********************************************************************/
/*  Prepare and count columns in the column list.                      */
/***********************************************************************/
int PrepareColist(char *colist)
{
	char *p, *pn;
	int   n = 0;

	// Count the number of columns and change separator into null char
	for (pn = colist; ; pn += (strlen(pn) + 1))
    // Separator can be ; if colist was specified in the option_list
		if ((p = strchr(pn, ',')) || (p = strchr(pn, ';'))) {
			*p++ = '\0';
			n++;
		} else {
			if (*pn)
				n++;

			break;
		} // endif p

	return n;
} // end of PrepareColist

/************************************************************************/
/*  OcrColumns: constructs the result blocks containing all the columns */
/*  of the object table that will be retrieved by GetData commands.     */
/************************************************************************/
bool OcrColumns(PGLOBAL g, PQRYRES qrp, const char *col, 
                       const char *ocr, const char *rank)
{
  char   *pn, *colist;
  int     i, k, m, n = 0, c = 0, j = qrp->Nblin;
  bool    rk, b = false;
  PCOLRES crp;

  if (!col || !*col) {
    strcpy(g->Message, "Missing colist");
    return true;
    } // endif col

  // Prepare the column list
  colist = PlugDup(g, col);
  m = PrepareColist(colist);

  if ((rk = (rank && *rank))) {
    if (m == 1) {
      strcpy(g->Message, "Cannot handle one column colist and rank");
      return true;
      } // endif m

    for (k = 0, pn = colist; k < m; k++, pn += (strlen(pn) + 1))
      n = MY_MAX(n, (signed)strlen(pn));

    } // endif k
             
  // Default occur column name is the 1st colist column name
  if (!ocr || !*ocr)
    ocr = colist;

  /**********************************************************************/
  /*  Replace the columns of the colist by the rank and occur columns.  */
  /**********************************************************************/
  for (i = 0; i < qrp->Nblin; i++) {
    for (k = 0, pn = colist; k < m; k++, pn += (strlen(pn) + 1))
      if (!stricmp(pn, qrp->Colresp->Kdata->GetCharValue(i)))
        break;

    if (k < m) {
      // This column belongs to colist
      if (rk) {
        // Place the rank column here
        for (crp = qrp->Colresp; crp; crp = crp->Next)
          switch (crp->Fld) {
            case FLD_NAME:  crp->Kdata->SetValue((char*)rank, i); break;
            case FLD_TYPE:  crp->Kdata->SetValue(TYPE_STRING, i); break;
            case FLD_PREC:  crp->Kdata->SetValue(n, i);           break;
            case FLD_SCALE: crp->Kdata->SetValue(0, i);           break;
            case FLD_NULL:  crp->Kdata->SetValue(0, i);           break;
            case FLD_REM:   crp->Kdata->Reset(i);                 break;
            default: ;   // Ignored by CONNECT
            } // endswich Fld
    
        rk = false;
      } else if (!b) {
        // First remaining listed column, will be the occur column
        for (crp = qrp->Colresp; crp; crp = crp->Next)
          switch (crp->Fld) {
            case FLD_NAME: crp->Kdata->SetValue((char*)ocr, i); break;
            case FLD_REM:  crp->Kdata->Reset(i);                break;
            default: ;   // Nothing to do
            } // endswich Fld
    
        b = true;
      } else if (j == qrp->Nblin)
        j = i;     // Column to remove

      c++;
    } else if (j < i) {
      // Move this column in empty spot
      for (crp = qrp->Colresp; crp; crp = crp->Next)
        crp->Kdata->Move(i, j);

      j++;
    } // endif k

    } // endfor i

  // Check whether all columns of the list where found
  if (c < m) {
    strcpy(g->Message, "Some colist columns are not in the source table");
    return true;
    } // endif crp

  /**********************************************************************/
  /*  Set the number of columns of the table.                           */
  /**********************************************************************/
  qrp->Nblin = j;
  return false;
} // end of OcrColumns

/************************************************************************/
/*  OcrSrcCols: constructs the result blocks containing all the columns */
/*  of the object table that will be retrieved by GetData commands.     */
/************************************************************************/
bool OcrSrcCols(PGLOBAL g, PQRYRES qrp, const char *col, 
                       const char *ocr, const char *rank)
{
  char   *pn, *colist;
  int     i, k, m, n = 0, c = 0;
  bool    rk, b = false;
  PCOLRES crp, rcrp, *pcrp;

  if (!col || !*col) {
    strcpy(g->Message, "Missing colist");
    return true;
    } // endif col

  // Prepare the column list
  colist = PlugDup(g, col);
  m = PrepareColist(colist);

  if ((rk = (rank && *rank)))
    for (k = 0, pn = colist; k < m; k++, pn += (strlen(pn) + 1))
      n = MY_MAX(n, (signed)strlen(pn));
             
  // Default occur column name is the 1st colist column name
  if (!ocr || !*ocr)
    ocr = colist;

  /**********************************************************************/
  /*  Replace the columns of the colist by the rank and occur columns.  */
  /**********************************************************************/
  for (i = 0, pcrp = &qrp->Colresp; (crp = *pcrp); ) {
    for (k = 0, pn = colist; k < m; k++, pn += (strlen(pn) + 1))
      if (!stricmp(pn, crp->Name))
        break;

    if (k < m) {
      // This column belongs to colist
      c++;

      if (!b) {
        if (rk) {
          // Add the rank column here
          rcrp = (PCOLRES)PlugSubAlloc(g, NULL, sizeof(COLRES));
          memset(rcrp, 0, sizeof(COLRES));
          rcrp->Next = crp;
          rcrp->Name = (char*)rank;
          rcrp->Type = TYPE_STRING;
          rcrp->Length = n;
          rcrp->Ncol = ++i;
          *pcrp = rcrp;
        } // endif rk

        // First remaining listed column, will be the occur column
        crp->Name = (char*)ocr;
        b = true;
      } else {
        *pcrp = crp->Next;     // Remove this column
        continue;
      } // endif b

      } // endif k

    crp->Ncol = ++i;
    pcrp = &crp->Next;
    } // endfor pcrp

  // Check whether all columns of the list where found
  if (c < m) {
    strcpy(g->Message, "Some colist columns are not in the source table");
    return true;
    } // endif crp

  /**********************************************************************/
  /*  Set the number of columns of the table.                           */
  /**********************************************************************/
  qrp->Nblin = i;
  return false;
} // end of OcrSrcCols

/* -------------- Implementation of the OCCUR classes	---------------- */

/***********************************************************************/
/*  DefineAM: define specific AM block values from OCCUR table.        */
/***********************************************************************/
bool OCCURDEF::DefineAM(PGLOBAL g, LPCSTR am, int poff)
{
  Rcol = GetStringCatInfo(g, "RankCol", "");
  Colist = GetStringCatInfo(g, "Colist", "");
  Xcol = GetStringCatInfo(g, "OccurCol", Colist);
  return PRXDEF::DefineAM(g, am, poff);
} // end of DefineAM

/***********************************************************************/
/*  GetTable: makes a new TDB of the proper type.                      */
/***********************************************************************/
PTDB OCCURDEF::GetTable(PGLOBAL g, MODE)
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
//Tdbp = NULL;      			           // Source table (in TDBPRX)
  Tabname = tdp->Tablep->GetName();	 // Name of source table
	Colist = tdp->Colist;							 // List of source columns
	Xcolumn = tdp->Xcol;							 // Occur column name     
	Rcolumn = tdp->Rcol;							 // Rank column name     
	Xcolp = NULL;											 // To the OCCURCOL column
	Col = NULL;                        // To source column blocks array
	Mult = PrepareColist(Colist);      // Multiplication factor
	N = 0;									           // The current table index
	M = 0;                             // The occurrence rank
	RowFlag = 0;    				           // 0: Ok, 1: Same, 2: Skip
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
	} else
		return new(g) PRXCOL(cdp, this, cprec, n);

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
  if (!Tdbp)
    // Get the table description block of this table
    if (!(Tdbp = GetSubTable(g, ((POCCURDEF)To_Def)->Tablep, TRUE)))
      return TRUE;

  if (!Tdbp->IsView())
  	if (MakeColumnList(g))
      return TRUE;

  return FALSE;
} // end of InitTable

/***********************************************************************/
/*  Allocate OCCUR column description block.                           */
/***********************************************************************/
bool TDBOCCUR::MakeColumnList(PGLOBAL g)
{
	char *pn;
	int   i;
  PCOL  colp;

  for (colp = Columns; colp; colp = colp->GetNext())
    if (colp->GetAmType() == TYPE_AM_PRX)
		  if (((PPRXCOL)colp)->Init(g, NULL))
  			return true;

	Col = (PCOL*)PlugSubAlloc(g, NULL, Mult * sizeof(PCOL));

	for (i = 0, pn = Colist; i < Mult; i++, pn += (strlen(pn) + 1)) {
		if (!(Col[i] = Tdbp->ColDB(g, pn, 0))) {
		  // Column not found in table                                       
		  sprintf(g->Message, MSG(COL_ISNOT_TABLE), pn, Tabname);
			return true;
			} // endif Col

		if (Col[i]->InitValue(g)) {
	    strcpy(g->Message, "OCCUR InitValue failed");
			return true;
	    } // endif InitValue

		} // endfor i

	return false;
} // end of MakeColumnList

/***********************************************************************/
/*  Allocate OCCUR column description block for a view.                */
/***********************************************************************/
bool TDBOCCUR::ViewColumnList(PGLOBAL g)
{
	char *pn;
	int   i;
  PCOL  colp, cp;
  PTDBMY tdbp;

  if (!Tdbp->IsView())
    return false;

  if (Tdbp->GetAmType() != TYPE_AM_MYSQL) {
    strcpy(g->Message, "View is not MySQL");
    return true;
  } else
    tdbp = (PTDBMY)Tdbp;

  for (cp = Columns; cp; cp = cp->GetNext())
    if (cp->GetAmType() == TYPE_AM_PRX) {
      if ((colp = tdbp->MakeFieldColumn(g, cp->GetName()))) {
        ((PPRXCOL)cp)->Colp = colp;        
        ((PPRXCOL)cp)->To_Val = colp->GetValue();
      } else
  			return true;

      } // endif Type

	Col = (PCOL*)PlugSubAlloc(g, NULL, Mult * sizeof(PCOL));

	for (i = 0, pn = Colist; i < Mult; i++, pn += (strlen(pn) + 1))
		if (!(Col[i] = tdbp->MakeFieldColumn(g, pn))) {
		  // Column not found in table                                       
		  sprintf(g->Message, MSG(COL_ISNOT_TABLE), pn, Tabname);
			return true;
			} // endif Col

	return false;
} // end of ViewColumnList

/***********************************************************************/
/*  OCCUR GetMaxSize: returns the maximum number of rows in the table. */
/***********************************************************************/
int TDBOCCUR::GetMaxSize(PGLOBAL g)
{
  if (MaxSize < 0) {
    if (!(Tdbp = GetSubTable(g, ((POCCURDEF)To_Def)->Tablep, TRUE)))
      return 0;
  
		MaxSize = Mult * Tdbp->GetMaxSize(g);
		} // endif MaxSize

  return MaxSize;
} // end of GetMaxSize

/***********************************************************************/
/*  In this sample, ROWID will be the (virtual) row number,            */
/*  while ROWNUM will be the occurrence rank in the multiple column.    */
/***********************************************************************/
int TDBOCCUR::RowNumber(PGLOBAL, bool b)
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
    return TRUE;

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
	if (Tdbp->OpenDB(g))
    return TRUE;

  Use = USE_OPEN;
  return ViewColumnList(g);
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
void RANKCOL::ReadColumn(PGLOBAL)
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
