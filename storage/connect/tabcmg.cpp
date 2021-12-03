/************** tabcmg C++ Program Source Code File (.CPP) *************/
/* PROGRAM NAME: tabcmg     Version 1.3                                */
/*  (C) Copyright to the author Olivier BERTRAND          2017 - 2021  */
/*  This program are the C MongoDB class DB execution routines.        */
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
#include "tabcmg.h"
#include "tabmul.h"
#include "filter.h"

PQRYRES MGOColumns(PGLOBAL g, PCSZ db, PCSZ uri, PTOS topt, bool info);
bool    Stringified(PCSZ, char*);

/* -------------------------- Class CMGDISC -------------------------- */

/***********************************************************************/
/*  Get document.                                                      */
/***********************************************************************/
void CMGDISC::GetDoc(void)
{
	doc = ((TDBCMG*)tmgp)->Cmgp->Document;
}	// end of GetDoc

/***********************************************************************/
/*  Analyse passed document.                                           */
/***********************************************************************/
//bool CMGDISC::Find(PGLOBAL g, int i, int k, bool b)
bool CMGDISC::Find(PGLOBAL g)
{
	return FindInDoc(g, &iter, doc, NULL, NULL, 0, false);
}	// end of Find

/***********************************************************************/
/*  Analyse passed document.                                           */
/***********************************************************************/
bool CMGDISC::FindInDoc(PGLOBAL g, bson_iter_t *iter, const bson_t *doc,
	                      char *pcn, char *pfmt, int k, bool b)
{
	if (!doc || bson_iter_init(iter, doc)) {
		const char *key;
		char   colname[65];
		char 	 fmt[129];
		bool   newcol;
		size_t n;

		while (bson_iter_next(iter)) {
			key = bson_iter_key(iter);
			newcol = true;

			if (pcn) {
				n = sizeof(colname) - 1;
				strncpy(colname, pcn, n);
				colname[n] = 0;
				n -= strlen(colname);
				strncat(strncat(colname, "_", n), key, n - 1);
			}	else
				strcpy(colname, key);

			if (pfmt) {
				n = sizeof(fmt) - 1;
				strncpy(fmt, pfmt, n);
				fmt[n] = 0;
				n -= strlen(fmt);
				strncat(strncat(fmt, ".", n), key, n - 1);
			} else
				strcpy(fmt, key);

			bcol.Cbn = false;

			switch (bson_iter_type(iter)) {
				case BSON_TYPE_UTF8:
					bcol.Type = TYPE_STRING;
					bcol.Len = strlen(bson_iter_utf8(iter, NULL));
					break;
				case BSON_TYPE_INT32:
					bcol.Type = TYPE_INT;
					bcol.Len = 11; // bson_iter_int32(iter)
					break;
				case BSON_TYPE_INT64:
					bcol.Type = TYPE_BIGINT;
					bcol.Len = 22; // bson_iter_int64(iter)
					break;
				case BSON_TYPE_DOUBLE:
					bcol.Type = TYPE_DOUBLE;
					bcol.Len = 12;
					bcol.Scale = 6; // bson_iter_double(iter)
					break;
				case BSON_TYPE_DATE_TIME:
					bcol.Type = TYPE_DATE;
					bcol.Len = 19; // bson_iter_date_time(iter)
					break;
				case BSON_TYPE_BOOL:
					bcol.Type = TYPE_TINY;
					bcol.Len = 1;
					break;
				case BSON_TYPE_OID:
					bcol.Type = TYPE_STRING;
					bcol.Len = 24; // bson_iter_oid(iter)
					break;
				case BSON_TYPE_DECIMAL128:
					bcol.Type = TYPE_DECIM;
					bcol.Len = 32; // bson_iter_decimal128(iter, &dec)
					break;
				case BSON_TYPE_DOCUMENT:
					if (lvl < 0)
						continue;
					else if (lvl <= k) {
						bcol.Type = TYPE_STRING;
						bcol.Len = 512;
					} else {
						bson_iter_t child;

						if (bson_iter_recurse(iter, &child))
							if (FindInDoc(g, &child, NULL, colname, fmt, k + 1, false))
								return true;

						newcol = false;
					} // endif lvl

					break;
				case BSON_TYPE_ARRAY:
					if (lvl < 0)
						continue;
					else if (lvl <= k) {
						bcol.Type = TYPE_STRING;
						bcol.Len = 512;
					} else {
						bson_t* arr;
						bson_iter_t    itar;
						const uint8_t* data = NULL;
						uint32_t       len = 0;

						bson_iter_array(iter, &len, &data);
						arr = bson_new_from_data(data, len);

						if (FindInDoc(g, &itar, arr, colname, fmt, k + 1, !all))
							return true;

						newcol = false;
					} // endif lvl

					break;
			}	// endswitch iter

			if (newcol)
				AddColumn(g, colname, fmt, k);

			if (b)
				break;		// Test only first element of arrays

		} // endwhile iter

	} // endif doc

	return false;
}	// end of FindInDoc

/* --------------------------- Class TDBCMG -------------------------- */

/***********************************************************************/
/*  Implementation of the TDBCMG class.                                */
/***********************************************************************/
TDBCMG::TDBCMG(MGODEF *tdp) : TDBEXT(tdp)
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
		Pcg.Line = NULL;
		Pcg.Pipe = tdp->Pipe && tdp->Colist != NULL;
		B = tdp->Base ? 1 : 0;
		Strfy = tdp->Strfy;
	} else {
		Pcg.Uristr = NULL;
		Pcg.Db_name = NULL;
		Pcg.Coll_name = NULL;
		Pcg.Options = NULL;
		Pcg.Filter = NULL;
		Pcg.Line = NULL;
		Pcg.Pipe = false;
		Strfy = NULL;
		B = 0;
	} // endif tdp

	Fpos = -1;
	N = 0;
	Done = false;
} // end of TDBCMG standard constructor

TDBCMG::TDBCMG(TDBCMG *tdbp) : TDBEXT(tdbp)
{
	Cmgp = tdbp->Cmgp;
	Cnd = tdbp->Cnd;
	Pcg = tdbp->Pcg;
	Strfy = tdbp->Strfy;
	B = tdbp->B;
	Fpos = tdbp->Fpos;
	N = tdbp->N;
	Done = tdbp->Done;
} // end of TDBCMG copy constructor

// Used for update
PTDB TDBCMG::Clone(PTABS t)
{
	PTDB    tp;
	PMGOCOL cp1, cp2;
	PGLOBAL g = t->G;

	tp = new(g) TDBCMG(this);

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
PCOL TDBCMG::MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n)
{
	PMGOCOL colp = new(g) MGOCOL(g, cdp, this, cprec, n);

	return colp;
} // end of MakeCol

/***********************************************************************/
/*  InsertSpecialColumn: Put a special column ahead of the column list.*/
/***********************************************************************/
PCOL TDBCMG::InsertSpecialColumn(PCOL colp)
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
bool TDBCMG::Init(PGLOBAL g)
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
int TDBCMG::Cardinality(PGLOBAL g)
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
int TDBCMG::GetMaxSize(PGLOBAL g)
{
	if (MaxSize < 0)
		MaxSize = Cardinality(g);

	return MaxSize;
} // end of GetMaxSize

/***********************************************************************/
/*  OpenDB: Data Base open routine for MONGO access method.            */
/***********************************************************************/
bool TDBCMG::OpenDB(PGLOBAL g)
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

	Use = USE_OPEN;       // Do it now in case we are recursively called

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
bool TDBCMG::ReadKey(PGLOBAL g, OPVAL op, const key_range *kr)
{
	strcpy(g->Message, "MONGO tables are not indexable");
	return true;
} // end of ReadKey

/***********************************************************************/
/*  ReadDB: Get next document from a collection.                       */
/***********************************************************************/
int TDBCMG::ReadDB(PGLOBAL g)
{
	return Cmgp->ReadNext(g);
} // end of ReadDB

/***********************************************************************/
/*  WriteDB: Data Base write routine for MGO access method.            */
/***********************************************************************/
int TDBCMG::WriteDB(PGLOBAL g)
{
	return Cmgp->Write(g);
} // end of WriteDB

/***********************************************************************/
/*  Data Base delete line routine for MGO access method.               */
/***********************************************************************/
int TDBCMG::DeleteDB(PGLOBAL g, int irc)
{
	return (irc == RC_OK) ? WriteDB(g) : RC_OK;
} // end of DeleteDB

/***********************************************************************/
/*  Table close routine for MONGO tables.                              */
/***********************************************************************/
void TDBCMG::CloseDB(PGLOBAL g)
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
	Tmgp = (PTDBCMG)(tdbp->GetOrig() ? tdbp->GetOrig() : tdbp);
	Sgfy = Stringified(Tmgp->Strfy, Name);

	if ((Jpath = cdp->GetFmt())) {
		int n = strlen(Jpath) - 1;

		if (Jpath[n] == '*') {
			Jpath = PlugDup(g, cdp->GetFmt());
			if (Jpath[n - 1] == '.') n--;
			Jpath[n] = 0;
			Sgfy = true;
		}	// endif Jpath

	}	else
	  Jpath = cdp->GetName();

} // end of MGOCOL constructor

/***********************************************************************/
/*  MGOCOL constructor used for copying columns.                       */
/*  tdbp is the pointer to the new table descriptor.                   */
/***********************************************************************/
MGOCOL::MGOCOL(MGOCOL *col1, PTDB tdbp) : EXTCOL(col1, tdbp)
{
	Tmgp = col1->Tmgp;
	Jpath = col1->Jpath;
	Sgfy = col1->Sgfy;
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

				if (*(p2 - 1) == '.')
					p2--;

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
	Uri = tdp->Uri;
	Db = tdp->GetTabschema();
} // end of TDBJCL constructor

/***********************************************************************/
/*  GetResult: Get the list the JSON file columns.                     */
/***********************************************************************/
PQRYRES TDBGOL::GetResult(PGLOBAL g)
{
	return MGOColumns(g, Db, Uri, Topt, false);
} // end of GetResult

/* -------------------------- End of mongo --------------------------- */
