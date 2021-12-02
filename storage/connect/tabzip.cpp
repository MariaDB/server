/************* TabZip C++ Program Source Code File (.CPP) **************/
/* PROGRAM NAME: TABZIP     Version 1.0                                */
/*  (C) Copyright to the author Olivier BERTRAND          2016         */
/*  This program are the TABZIP class DB execution routines.           */
/***********************************************************************/

/***********************************************************************/
/*  Include relevant sections of the MariaDB header file.              */
/***********************************************************************/
#include <my_global.h>

/***********************************************************************/
/*  Include application header files:                                  */
/*  global.h    is header containing all global declarations.          */
/*  plgdbsem.h  is header containing the DB application declarations.  */
/*  (x)table.h  is header containing the TDBASE declarations.          */
/*  tabzip.h    is header containing the TABZIP classes declarations.  */
/***********************************************************************/
#include "global.h"
#include "plgdbsem.h"
#include "xtable.h"
#include "filamtxt.h"
#include "filamzip.h"
#include "resource.h"                        // for IDS_COLUMNS
#include "tabdos.h"
#include "tabmul.h"
#include "tabzip.h"

/* -------------------------- Class ZIPDEF --------------------------- */

/************************************************************************/
/*  DefineAM: define specific AM block values.                          */
/************************************************************************/
bool ZIPDEF::DefineAM(PGLOBAL g, LPCSTR am, int poff)
{
//target = GetStringCatInfo(g, "Target", NULL);
	return DOSDEF::DefineAM(g, "ZIP", poff);
} // end of DefineAM

/***********************************************************************/
/*  GetTable: makes a new Table Description Block.                     */
/***********************************************************************/
PTDB ZIPDEF::GetTable(PGLOBAL g, MODE m)
{
	PTDB tdbp = NULL;

	tdbp = new(g) TDBZIP(this);

	if (Multiple)
		tdbp = new(g) TDBMUL(tdbp);

	return tdbp;
} // end of GetTable

/* ------------------------------------------------------------------- */

/***********************************************************************/
/*  Implementation of the TDBZIP class.                                */
/***********************************************************************/
TDBZIP::TDBZIP(PZIPDEF tdp) : TDBASE(tdp)
{
	zipfile = NULL;
	zfn = tdp->Fn;
//target = tdp->target;
	nexterr = UNZ_OK;
} // end of TDBZIP standard constructor

/***********************************************************************/
/*  Allocate ZIP column description block.                             */
/***********************************************************************/
PCOL TDBZIP::MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n)
{
	return new(g) ZIPCOL(cdp, this, cprec, n);
} // end of MakeCol

/***********************************************************************/
/*  open a zip file.																									 */
/*  param: filename	path and the filename of the zip file to open.		 */
/*  return:	true if open, false otherwise.														 */
/***********************************************************************/
bool TDBZIP::open(PGLOBAL g, const char *fn)
{
	char filename[_MAX_PATH];

	PlugSetPath(filename, fn, GetPath());

	if (!zipfile && !(zipfile = unzOpen64(filename)))
		sprintf(g->Message, "Zipfile open error");

	return (zipfile == NULL);
}	// end of open

/***********************************************************************/
/*  Close the zip file.																								 */
/***********************************************************************/
void TDBZIP::close()
{
	if (zipfile) {
		unzClose(zipfile);
		zipfile = NULL;
	}	// endif zipfile

}	// end of close

/***********************************************************************/
/*  ZIP Cardinality: returns table size in number of rows.             */
/***********************************************************************/
int TDBZIP::Cardinality(PGLOBAL g)
{
	if (!g)
		return 1;
	else if (Cardinal < 0) {
		if (!open(g, zfn)) {
			unz_global_info64 ginfo;
			int err = unzGetGlobalInfo64(zipfile, &ginfo);

			Cardinal = (err == UNZ_OK) ? (int)ginfo.number_entry : 0;
		} else
			Cardinal = 10;    // Dummy for multiple tables

	} // endif Cardinal

	return Cardinal;
} // end of Cardinality

/***********************************************************************/
/*  ZIP GetMaxSize: returns file size estimate in number of lines.     */
/***********************************************************************/
int TDBZIP::GetMaxSize(PGLOBAL g)
{
	if (MaxSize < 0)
		MaxSize = Cardinality(g);

	return MaxSize;
} // end of GetMaxSize

/***********************************************************************/
/*  ZIP Access Method opening routine.                                 */
/***********************************************************************/
bool TDBZIP::OpenDB(PGLOBAL g)
{
	if (Use == USE_OPEN)
		// Table already open
		return false;

	Use = USE_OPEN;       // To be clean
  return open(g, zfn);
} // end of OpenDB

/***********************************************************************/
/*  ReadDB: Data Base read routine for ZIP access method.              */
/***********************************************************************/
int TDBZIP::ReadDB(PGLOBAL g)
{
	if (nexterr == UNZ_END_OF_LIST_OF_FILE)
		return RC_EF;
	else if (nexterr != UNZ_OK) {
		sprintf(g->Message, "unzGoToNextFile error %d", nexterr);
		return RC_FX;
	}	// endif nexterr

	int err = unzGetCurrentFileInfo64(zipfile, &finfo, fn,
		sizeof(fn), NULL, 0, NULL, 0);

	if (err != UNZ_OK) {
		sprintf(g->Message, "unzGetCurrentFileInfo64 error %d", err);
		return RC_FX;
	}	// endif err

	nexterr = unzGoToNextFile(zipfile);
	return RC_OK;
} // end of ReadDB

/***********************************************************************/
/*  WriteDB: Data Base write routine for ZIP access method.            */
/***********************************************************************/
int TDBZIP::WriteDB(PGLOBAL g)
{
	strcpy(g->Message, "ZIP tables are read only");
	return RC_FX;
} // end of WriteDB

/***********************************************************************/
/*  Data Base delete line routine for ZIP access method.               */
/***********************************************************************/
int TDBZIP::DeleteDB(PGLOBAL g, int irc)
{
	strcpy(g->Message, "Delete not enabled for ZIP tables");
	return RC_FX;
} // end of DeleteDB

/***********************************************************************/
/*  Data Base close routine for ZIP access method.                     */
/***********************************************************************/
void TDBZIP::CloseDB(PGLOBAL g)
{
	close();
	nexterr = UNZ_OK;               // For multiple tables
	Use = USE_READY;                // Just to be clean
} // end of CloseDB

/* ---------------------------- ZIPCOL ------------------------------- */

/***********************************************************************/
/*  ZIPCOL public constructor.                                         */
/***********************************************************************/
ZIPCOL::ZIPCOL(PCOLDEF cdp, PTDB tdbp, PCOL cprec, int i, PCSZ am)
	    : COLBLK(cdp, tdbp, i)
{
	if (cprec) {
		Next = cprec->GetNext();
		cprec->SetNext(this);
	} else {
		Next = tdbp->GetColumns();
		tdbp->SetColumns(this);
	} // endif cprec

	Tdbz = (TDBZIP*)tdbp;
	flag = cdp->GetOffset();
} // end of ZIPCOL constructor

/***********************************************************************/
/*  ReadColumn:                                                        */
/***********************************************************************/
void ZIPCOL::ReadColumn(PGLOBAL g)
{
	switch (flag) {
	case 1:
		Value->SetValue(Tdbz->finfo.compressed_size);
		break;
	case 2:
		Value->SetValue(Tdbz->finfo.uncompressed_size);
		break;
	case 3:
		Value->SetValue((int)Tdbz->finfo.compression_method);
		break;
	case 4:
		Tdbz->finfo.tmu_date.tm_year -= 1900;

		if (((DTVAL*)Value)->MakeTime((tm*)&Tdbz->finfo.tmu_date))
			Value->SetNull(true);

		Tdbz->finfo.tmu_date.tm_year += 1900;
		break;
	default:
		Value->SetValue_psz((PSZ)Tdbz->fn);
	}	// endswitch flag

} // end of ReadColumn

/* -------------------------- End of tabzip -------------------------- */
