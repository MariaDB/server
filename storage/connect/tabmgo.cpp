/************** tabmgo C++ Program Source Code File (.CPP) *************/
/* PROGRAM NAME: tabmgo     Version 1.0                                */
/*  (C) Copyright to the author Olivier BERTRAND          2017         */
/*  This program are the MongoDB class DB execution routines.          */
/***********************************************************************/

/***********************************************************************/
/*  Include relevant sections of the MariaDB header file.              */
/***********************************************************************/
#include <my_global.h>

/***********************************************************************/
/*  Include application header files:                                  */
/*  global.h    is header containing all global declarations.          */
/*  plgdbsem.h  is header containing the DB application declarations.  */
/*  tdbdos.h    is header containing the TDBDOS declarations.          */
/*  json.h      is header containing the JSON classes declarations.    */
/***********************************************************************/
#include "global.h"
#include "plgdbsem.h"
#include "xtable.h"
#include "maputil.h"
#include "filamtxt.h"
#include "tabext.h"
#include "tabmgo.h"
#include "tabmul.h"
#include "checklvl.h"
#include "resource.h"
#include "mycat.h"                             // for FNC_COL
#include "filter.h"

/***********************************************************************/
/*  This should be an option.                                          */
/***********************************************************************/
#define MAXCOL          200        /* Default max column nb in result  */
#define TYPE_UNKNOWN     12        /* Must be greater than other types */

bool IsNum(PSZ s);

/***********************************************************************/
/*  MGOColumns: construct the result blocks containing the description */
/*  of all the columns of a document contained inside MongoDB.         */
/***********************************************************************/
PQRYRES MGOColumns(PGLOBAL g, char *db, PTOS topt, bool info)
{
	static int  buftyp[] = {TYPE_STRING, TYPE_SHORT, TYPE_STRING, TYPE_INT,
		                      TYPE_INT, TYPE_SHORT, TYPE_SHORT, TYPE_STRING};
	static XFLD fldtyp[] = {FLD_NAME, FLD_TYPE, FLD_TYPENAME, FLD_PREC,
		                      FLD_LENGTH, FLD_SCALE, FLD_NULL, FLD_FORMAT};
	unsigned int length[] = {0, 6, 8, 10, 10, 6, 6, 0};
	int      ncol = sizeof(buftyp) / sizeof(int);
	int      i, n = 0;
	PBCOL    bcp;
	MGODISC *mgd;
	PQRYRES  qrp;
	PCOLRES  crp;

	if (info) {
		length[0] = 128;
		length[7] = 256;
		goto skipit;
	} // endif info

	/*********************************************************************/
	/*  Open MongoDB.                                                    */
	/*********************************************************************/
	mgd = new(g) MGODISC(g, (int*)length);

	if ((n = mgd->GetColumns(g, db, topt)) < 0)
		goto err;

skipit:
	if (trace)
		htrc("MGOColumns: n=%d len=%d\n", n, length[0]);

	/*********************************************************************/
	/*  Allocate the structures used to refer to the result set.         */
	/*********************************************************************/
	qrp = PlgAllocResult(g, ncol, n, IDS_COLUMNS + 3,
		buftyp, fldtyp, length, false, false);

	crp = qrp->Colresp->Next->Next->Next->Next->Next->Next;
	crp->Name = "Nullable";
	crp->Next->Name = "Bpath";

	if (info || !qrp)
		return qrp;

	qrp->Nblin = n;

	/*********************************************************************/
	/*  Now get the results into blocks.                                 */
	/*********************************************************************/
	for (i = 0, bcp = mgd->fbcp; bcp; i++, bcp = bcp->Next) {
		if (bcp->Type == TYPE_UNKNOWN)            // Void column
			bcp->Type = TYPE_STRING;

		crp = qrp->Colresp;                    // Column Name
		crp->Kdata->SetValue(bcp->Name, i);
		crp = crp->Next;                       // Data Type
		crp->Kdata->SetValue(bcp->Type, i);
		crp = crp->Next;                       // Type Name
		crp->Kdata->SetValue(GetTypeName(bcp->Type), i);
		crp = crp->Next;                       // Precision
		crp->Kdata->SetValue(bcp->Len, i);
		crp = crp->Next;                       // Length
		crp->Kdata->SetValue(bcp->Len, i);
		crp = crp->Next;                       // Scale (precision)
		crp->Kdata->SetValue(bcp->Scale, i);
		crp = crp->Next;                       // Nullable
		crp->Kdata->SetValue(bcp->Cbn ? 1 : 0, i);
		crp = crp->Next;                       // Field format

		if (crp->Kdata)
			crp->Kdata->SetValue(bcp->Fmt, i);

	} // endfor i

	/*********************************************************************/
	/*  Return the result pointer.                                       */
	/*********************************************************************/
	return qrp;

err:
	if (mgd->tmgp)
  	mgd->tmgp->CloseDB(g);

	return NULL;
} // end of MGOColumns

/***********************************************************************/
/*  Class used to get the columns of a mongo collection.               */
/***********************************************************************/
// Constructor
MGODISC::MGODISC(PGLOBAL g, int *lg) {
	length = lg;
	fbcp = NULL;
	pbcp = NULL;
	tmgp = NULL;
	n = k = lvl = 0;
	all = false;
}	// end of MGODISC constructor

/***********************************************************************/
/*  Class used to get the columns of a mongo collection.               */
/***********************************************************************/
int MGODISC::GetColumns(PGLOBAL g, char *db, PTOS topt)
{
	PCSZ          level;
	bson_iter_t   iter;
	const bson_t *doc;
	PMGODEF       tdp;
	TDBMGO       *tmgp = NULL;

	level = GetStringTableOption(g, topt, "Level", NULL);

	if (level) {
		lvl = atoi(level);
		lvl = (lvl > 16) ? 16 : lvl;
	} else
		lvl = 0;

	all = GetBooleanTableOption(g, topt, "Fullarray", false);

	/*********************************************************************/
	/*  Open the MongoDB collection.                                     */
	/*********************************************************************/
	tdp = new(g) MGODEF;
	tdp->Uri = GetStringTableOption(g, topt, "Connect", "mongodb://localhost:27017");
	tdp->Tabname = GetStringTableOption(g, topt, "Name", NULL);
	tdp->Tabname = GetStringTableOption(g, topt, "Tabname", tdp->Tabname);
	tdp->Tabschema = GetStringTableOption(g, topt, "Dbname", db);
	tdp->Base = GetIntegerTableOption(g, topt, "Base", 0) ? 1 : 0;
	tdp->Colist = GetStringTableOption(g, topt, "Colist", "all");
	tdp->Filter = GetStringTableOption(g, topt, "Filter", NULL);
	tdp->Pipe = GetBooleanTableOption(g, topt, "Pipeline", false);

	if (trace)
		htrc("Uri %s coll=%s db=%s colist=%s filter=%s lvl=%d\n",
			tdp->Uri, tdp->Tabname, tdp->Tabschema, tdp->Colist, tdp->Filter, lvl);

	tmgp = new(g) TDBMGO(tdp);
	tmgp->SetMode(MODE_READ);

	if (tmgp->OpenDB(g))
		return -1;

	bcol.Next = NULL;
	bcol.Name = bcol.Fmt = NULL;
	bcol.Type = TYPE_UNKNOWN;
	bcol.Len = bcol.Scale = 0;
	bcol.Found = true;
	bcol.Cbn = false;

	/*********************************************************************/
	/*  Analyse the BSON tree and define columns.                        */
	/*********************************************************************/
	for (int i = 1; ; i++) {
		switch (tmgp->ReadDB(g)) {
			case RC_EF:
				return n;
			case RC_FX:
				return -1;
			default:
				doc = tmgp->Cmgp->Document;
		} // endswitch ReadDB

		if (FindInDoc(g, &iter, doc, NULL, NULL, i, k, false))
			return -1;

		// Missing columns can be null
		for (bcp = fbcp; bcp; bcp = bcp->Next) {
			bcp->Cbn |= !bcp->Found;
			bcp->Found = false;
		} // endfor bcp

	} // endfor i

	return n;
} // end of GetColumns

/*********************************************************************/
/*  Analyse passed document.                                         */
/*********************************************************************/
bool MGODISC::FindInDoc(PGLOBAL g, bson_iter_t *iter, const bson_t *doc,
	                      char *pcn, char *pfmt, int i, int k, bool b)
{
	if (!doc || bson_iter_init(iter, doc)) {
		const char *key;
		char  colname[65];
		char 	fmt[129];
		bool newcol;

		while (bson_iter_next(iter)) {
			key = bson_iter_key(iter);
			newcol = true;

			if (pcn) {
				strncpy(colname, pcn, 64);
				colname[64] = 0;
				strncat(strncat(colname, "_", 65), key, 65);
			}	else
				strcpy(colname, key);

			if (pfmt) {
				strncpy(fmt, pfmt, 128);
				fmt[128] = 0;
				strncat(strncat(fmt, ".", 129), key, 129);
			} else
				strcpy(fmt, key);

			bcol.Cbn = false;

			if (BSON_ITER_HOLDS_UTF8(iter)) {
				bcol.Type = TYPE_STRING;
				bcol.Len = strlen(bson_iter_utf8(iter, NULL));
			} else if (BSON_ITER_HOLDS_INT32(iter)) {
				bcol.Type = TYPE_INT;
				bcol.Len = 11; // bson_iter_int32(iter)
			} else if (BSON_ITER_HOLDS_INT64(iter)) {
				bcol.Type = TYPE_BIGINT;
				bcol.Len = 22; // bson_iter_int64(iter)
			} else if (BSON_ITER_HOLDS_DOUBLE(iter)) {
				bcol.Type = TYPE_DOUBLE;
				bcol.Len = 12;
				bcol.Scale = 6; // bson_iter_double(iter)
			} else if (BSON_ITER_HOLDS_DATE_TIME(iter)) {
				bcol.Type = TYPE_DATE;
				bcol.Len = 19; // bson_iter_date_time(iter)
			} else if (BSON_ITER_HOLDS_BOOL(iter)) {
				bcol.Type = TYPE_TINY;
				bcol.Len = 1;
			} else if (BSON_ITER_HOLDS_OID(iter)) {
				bcol.Type = TYPE_STRING;
				bcol.Len = 24; // bson_iter_oid(iter)
			} else if (BSON_ITER_HOLDS_DECIMAL128(iter)) {
				bcol.Type = TYPE_DECIM;
				bcol.Len = 32; // bson_iter_decimal128(iter, &dec)
			} else if (BSON_ITER_HOLDS_DOCUMENT(iter)) {
				if (lvl < 0)
					continue;
				else if (lvl <= k) {
					bcol.Type = TYPE_STRING;
					bcol.Len = 512;
				} else {
					bson_iter_t child;

					if (bson_iter_recurse(iter, &child))
						if (FindInDoc(g, &child, NULL, colname, fmt, i, k + 1, false))
							return true;

					newcol = false;
				} // endif lvl

			} else if (BSON_ITER_HOLDS_ARRAY(iter)) {
				if (lvl < 0)
					continue;
				else if (lvl <= k) {
					bcol.Type = TYPE_STRING;
					bcol.Len = 512;
				} else {
					bson_t				*arr;
					bson_iter_t    itar;
					const uint8_t *data = NULL;
					uint32_t       len = 0;

					bson_iter_array(iter, &len, &data);
					arr = bson_new_from_data(data, len);

					if (FindInDoc(g, &itar, arr, colname, fmt, i, k + 1, !all))
						return true;

					newcol = false;
				} // endif lvl

		  }	// endif's

			if (newcol) {
				// Check whether this column was already found
				for (bcp = fbcp; bcp; bcp = bcp->Next)
					if (!strcmp(colname, bcp->Name))
						break;

				if (bcp) {
					if (bcp->Type != bcol.Type)
						bcp->Type = TYPE_STRING;

					if (k && *fmt && (!bcp->Fmt || strlen(bcp->Fmt) < strlen(fmt))) {
						bcp->Fmt = PlugDup(g, fmt);
						length[7] = MY_MAX(length[7], strlen(fmt));
					} // endif *fmt

					bcp->Len = MY_MAX(bcp->Len, bcol.Len);
					bcp->Scale = MY_MAX(bcp->Scale, bcol.Scale);
					bcp->Cbn |= bcol.Cbn;
					bcp->Found = true;
				} else {
					// New column
					bcp = (PBCOL)PlugSubAlloc(g, NULL, sizeof(BCOL));
					*bcp = bcol;
					bcp->Cbn |= (i > 1);
					bcp->Name = PlugDup(g, colname);
					length[0] = MY_MAX(length[0], strlen(colname));

					if (k) {
						bcp->Fmt = PlugDup(g, fmt);
						length[7] = MY_MAX(length[7], strlen(fmt));
					} else
						bcp->Fmt = NULL;

					if (pbcp) {
						bcp->Next = pbcp->Next;
						pbcp->Next = bcp;
					} else
						fbcp = bcp;

					n++;
				} // endif jcp

				pbcp = bcp;
			} // endif newcol

			if (b)
				break;		// Test only first element of arrays

		} // endwhile iter

	} // endif doc

	return false;
}	// end of FindInDoc

/* --------------------------- Class TDBMGO -------------------------- */

/***********************************************************************/
/*  Implementation of the TDBMGO class.                                */
/***********************************************************************/
TDBMGO::TDBMGO(MGODEF *tdp) : TDBEXT(tdp)
{
	Cmgp = NULL;
	Cnd = NULL;
	Pcg.Tdbp = this;

	if (tdp) {
		Pcg.Uristr = tdp->Uri;
		Pcg.Db_name = tdp->Tabschema;
		Pcg.Coll_name = tdp->Tabname;
		Pcg.Options = tdp->Colist;
		Pcg.Filter = tdp->Filter;
		Pcg.Pipe = tdp->Pipe && Options != NULL;
		B = tdp->Base ? 1 : 0;
	} else {
		Pcg.Uristr = NULL;
		Pcg.Db_name = NULL;
		Pcg.Coll_name = NULL;
		Pcg.Options = NULL;
		Pcg.Filter = NULL;
		Pcg.Pipe = false;
		B = 0;
	} // endif tdp

	Fpos = -1;
	N = 0;
	Done = false;
} // end of TDBMGO standard constructor

TDBMGO::TDBMGO(TDBMGO *tdbp) : TDBEXT(tdbp)
{
	Cmgp = tdbp->Cmgp;
	Cnd = tdbp->Cnd;
	Pcg = tdbp->Pcg;
	B = tdbp->B;
	Fpos = tdbp->Fpos;
	N = tdbp->N;
	Done = tdbp->Done;
} // end of TDBMGO copy constructor

// Used for update
PTDB TDBMGO::Clone(PTABS t)
{
	PTDB    tp;
	PMGOCOL cp1, cp2;
	PGLOBAL g = t->G;

	tp = new(g) TDBMGO(this);

	for (cp1 = (PMGOCOL)Columns; cp1; cp1 = (PMGOCOL)cp1->GetNext())
		if (!cp1->IsSpecial()) {
			cp2 = new(g) MGOCOL(cp1, tp);  // Make a copy
		  NewPointer(t, cp1, cp2);
	  } // endif cp1

	return tp;
} // end of Clone

/***********************************************************************/
/*  Allocate JSN column description block.                             */
/***********************************************************************/
PCOL TDBMGO::MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n)
{
	PMGOCOL colp = new(g) MGOCOL(g, cdp, this, cprec, n);

	return colp;
} // end of MakeCol

/***********************************************************************/
/*  InsertSpecialColumn: Put a special column ahead of the column list.*/
/***********************************************************************/
PCOL TDBMGO::InsertSpecialColumn(PCOL colp)
{
	if (!colp->IsSpecial())
		return NULL;

	colp->SetNext(Columns);
	Columns = colp;
	return colp;
} // end of InsertSpecialColumn

/***********************************************************************/
/*  Init: initialize MongoDB processing.                               */
/***********************************************************************/
bool TDBMGO::Init(PGLOBAL g)
{
	if (Done)
		return false;

	/*********************************************************************/
	/*  Open an C connection for this table.                             */
	/*********************************************************************/
	if (!Cmgp)
		Cmgp = new(g) CMgoConn(g, &Pcg);
	else if (Cmgp->IsConnected())
		Cmgp->Close();

	if (Cmgp->Connect(g))
		return true;

	Done = true;
	return false;
} // end of Init

/***********************************************************************/
/*  MONGO Cardinality: returns table size in number of rows.           */
/***********************************************************************/
int TDBMGO::Cardinality(PGLOBAL g)
{
	if (!g)
		return 1;
	else if (Cardinal < 0)
		Cardinal = (!Init(g)) ? Cmgp->CollSize(g) : 0;

	return Cardinal;
} // end of Cardinality

/***********************************************************************/
/*  MONGO GetMaxSize: returns collection size estimate.                */
/***********************************************************************/
int TDBMGO::GetMaxSize(PGLOBAL g)
{
	if (MaxSize < 0)
		MaxSize = Cardinality(g);

	return MaxSize;
} // end of GetMaxSize

/***********************************************************************/
/*  OpenDB: Data Base open routine for MONGO access method.            */
/***********************************************************************/
bool TDBMGO::OpenDB(PGLOBAL g)
{
	if (Use == USE_OPEN) {
		/*******************************************************************/
		/*  Table already open replace it at its beginning.                */
		/*******************************************************************/
		Cmgp->Rewind();
		Fpos = -1;
		return false;
	} // endif Use

	/*********************************************************************/
	/*  First opening.                                                   */
	/*********************************************************************/
	if (Pcg.Pipe && Mode != MODE_READ) {
		strcpy(g->Message, "Pipeline tables are read only");
		return true;
	}	// endif Pipe

	if (Init(g))
		return true;

	if (Mode == MODE_DELETE && !Next)
		// Delete all documents
		return Cmgp->DocDelete(g);
	else if (Mode == MODE_INSERT)
		Cmgp->MakeColumnGroups(g);

	return false;
} // end of OpenDB

/***********************************************************************/
/*  Data Base indexed read routine for ODBC access method.             */
/***********************************************************************/
bool TDBMGO::ReadKey(PGLOBAL g, OPVAL op, const key_range *kr)
{
	strcpy(g->Message, "MONGO tables are not indexable");
	return true;
} // end of ReadKey

/***********************************************************************/
/*  ReadDB: Get next document from a collection.                       */
/***********************************************************************/
int TDBMGO::ReadDB(PGLOBAL g)
{
	return Cmgp->ReadNext(g);
} // end of ReadDB

/***********************************************************************/
/*  WriteDB: Data Base write routine for MGO access method.            */
/***********************************************************************/
int TDBMGO::WriteDB(PGLOBAL g)
{
	return Cmgp->Write(g);
} // end of WriteDB

/***********************************************************************/
/*  Data Base delete line routine for MGO access method.               */
/***********************************************************************/
int TDBMGO::DeleteDB(PGLOBAL g, int irc)
{
	return (irc == RC_OK) ? WriteDB(g) : RC_OK;
} // end of DeleteDB

/***********************************************************************/
/*  Table close routine for MONGO tables.                              */
/***********************************************************************/
void TDBMGO::CloseDB(PGLOBAL g)
{
	Cmgp->Close();
	Done = false;
} // end of CloseDB

/* ----------------------------- MGOCOL ------------------------------ */

/***********************************************************************/
/*  MGOCOL public constructor.                                         */
/***********************************************************************/
MGOCOL::MGOCOL(PGLOBAL g, PCOLDEF cdp, PTDB tdbp, PCOL cprec, int i)
	    : EXTCOL(cdp, tdbp, cprec, i, "MGO")
{
	Tmgp = (PTDBMGO)(tdbp->GetOrig() ? tdbp->GetOrig() : tdbp);
	Jpath = cdp->GetFmt() ? cdp->GetFmt() : cdp->GetName();
} // end of MGOCOL constructor

/***********************************************************************/
/*  MGOCOL constructor used for copying columns.                       */
/*  tdbp is the pointer to the new table descriptor.                   */
/***********************************************************************/
MGOCOL::MGOCOL(MGOCOL *col1, PTDB tdbp) : EXTCOL(col1, tdbp)
{
	Tmgp = col1->Tmgp;
	Jpath = col1->Jpath;
} // end of MGOCOL copy constructor

/***********************************************************************/
/*  Get path when proj is false or projection path when proj is true.  */
/***********************************************************************/
PSZ MGOCOL::GetJpath(PGLOBAL g, bool proj)
{
	if (Jpath) {
		if (proj) {
			char *p1, *p2, *projpath = PlugDup(g, Jpath);
			int   i = 0;

			for (p1 = p2 = projpath; *p1; p1++)
				if (*p1 == '.') {
					if (!i)
						*p2++ = *p1;

					i = 1;
				} else if (i) {
					if (!isdigit(*p1)) {
						*p2++ = *p1;
						i = 0;
					} // endif p1

				} else
					*p2++ = *p1;

				*p2 = 0;
				return projpath;
		} else
			return Jpath;

	} else
		return Name;

} // end of GetJpath

/***********************************************************************/
/*  ReadColumn:                                                        */
/***********************************************************************/
void MGOCOL::ReadColumn(PGLOBAL g)
{
	Tmgp->Cmgp->GetColumnValue(g, this);
} // end of ReadColumn

/***********************************************************************/
/*  WriteColumn:                                                       */
/***********************************************************************/
void MGOCOL::WriteColumn(PGLOBAL g)
{
	// Check whether this node must be written
	if (Value != To_Val)
		Value->SetValue_pval(To_Val, FALSE);    // Convert the updated value

} // end of WriteColumn

/* ---------------------------TDBGOL class --------------------------- */

/***********************************************************************/
/*  TDBGOL class constructor.                                          */
/***********************************************************************/
TDBGOL::TDBGOL(PMGODEF tdp) : TDBCAT(tdp)
{
	Topt = tdp->GetTopt();
	Db = (char*)tdp->GetTabschema();
} // end of TDBJCL constructor

/***********************************************************************/
/*  GetResult: Get the list the JSON file columns.                     */
/***********************************************************************/
PQRYRES TDBGOL::GetResult(PGLOBAL g)
{
	return MGOColumns(g, Db, Topt, false);
} // end of GetResult

/* -------------------------- End of mongo --------------------------- */
