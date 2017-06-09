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
				doc = tmgp->Document;
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

/* -------------------------- Class MGODEF --------------------------- */

MGODEF::MGODEF(void)
{
	Uri = NULL;
	Colist = NULL;
	Filter = NULL;
	Level = 0;
	Base = 0;
	Pipe = false;
} // end of MGODEF constructor

/***********************************************************************/
/*  DefineAM: define specific AM block values.                         */
/***********************************************************************/
bool MGODEF::DefineAM(PGLOBAL g, LPCSTR, int poff)
{
	if (EXTDEF::DefineAM(g, "MGO", poff))
		return true;
	else if (!Tabschema)
		Tabschema = GetStringCatInfo(g, "Dbname", "*");

	Uri = GetStringCatInfo(g, "Connect", "mongodb://localhost:27017");
	Colist = GetStringCatInfo(g, "Colist", NULL);
	Filter = GetStringCatInfo(g, "Filter", NULL);
	Base = GetIntCatInfo("Base", 0) ? 1 : 0;
	Pipe = GetBoolCatInfo("Pipeline", false);
	return false;
} // end of DefineAM

/***********************************************************************/
/*  GetTable: makes a new Table Description Block.                     */
/***********************************************************************/
PTDB MGODEF::GetTable(PGLOBAL g, MODE m)
{
	if (Catfunc == FNC_COL)
		return new(g)TDBGOL(this);

	return new(g) TDBMGO(this);
} // end of GetTable

/* --------------------------- Class INCOL --------------------------- */

/***********************************************************************/
/*  Add a column in the column list.                                   */
/***********************************************************************/
void INCOL::AddCol(PGLOBAL g, PCOL colp, char *jp)
{
	char *p;
	PKC   kp, kcp;

	if ((p = strchr(jp, '.'))) {
		PINCOL icp;

		*p = 0;

		for (kp = Klist; kp; kp = kp->Next)
			if (kp->Incolp && !strcmp(jp, kp->Key))
				break;

		if (!kp) {
			icp = new(g) INCOL;
			kcp = (PKC)PlugSubAlloc(g, NULL, sizeof(KEYCOL));
			kcp->Next = NULL;
			kcp->Incolp = icp;
			kcp->Colp = NULL;
			kcp->Key = PlugDup(g, jp);

			if (Klist) {
				for (kp = Klist; kp->Next; kp = kp->Next);

				kp->Next = kcp;
			} else
				Klist = kcp;

		} else
			icp = kp->Incolp;

		*p = '.';
		icp->AddCol(g, colp, p + 1);
	} else {
		kcp = (PKC)PlugSubAlloc(g, NULL, sizeof(KEYCOL));

		kcp->Next = NULL;
		kcp->Incolp = NULL;
		kcp->Colp = colp;
		kcp->Key = jp;

		if (Klist) {
			for (kp = Klist; kp->Next; kp = kp->Next);

			kp->Next = kcp;
		} else
			Klist = kcp;

	} // endif jp

}	// end of AddCol

/* --------------------------- Class TDBMGO -------------------------- */

/***********************************************************************/
/*  Implementation of the TDBMGO class.                                */
/***********************************************************************/
TDBMGO::TDBMGO(PMGODEF tdp) : TDBEXT(tdp)
{
	G = NULL;
	Uri = NULL;
	Pool = NULL;
	Client = NULL;
	Database = NULL;
	Collection = NULL;
	Cursor = NULL;
	Query = NULL;
	Opts = NULL;
	Fpc = NULL;
	Cnd = NULL;

	if (tdp) {
		Uristr = tdp->Uri;
		Db_name = tdp->Tabschema;
		Coll_name = tdp->Tabname;
		Options = tdp->Colist;
		Filter = tdp->Filter;
		B = tdp->Base ? 1 : 0;
		Pipe = tdp->Pipe && Options != NULL;
	} else {
		Uristr = NULL;
		Db_name = NULL;
		Coll_name = NULL;
		Options = NULL;
		Filter = NULL;
		B = 0;
		Pipe = false;
	} // endif tdp

	Fpos = -1;
	N = 0;
	Done = false;
} // end of TDBMGO standard constructor

TDBMGO::TDBMGO(TDBMGO *tdbp) : TDBEXT(tdbp)
{
	G = tdbp->G;
	Uri = tdbp->Uri;
	Pool = tdbp->Pool;
	Client = tdbp->Client;
	Database = NULL;
	Collection = tdbp->Collection;
	Cursor = tdbp->Cursor;
	Query = tdbp->Query;
	Opts = tdbp->Opts;
	Fpc = tdbp->Fpc;
	Cnd = tdbp->Cnd;
	Uristr = tdbp->Uristr;
	Db_name = tdbp->Db_name;;
	Coll_name = tdbp->Coll_name;
	Options = tdbp->Options;
	Filter = tdbp->Filter;
	B = tdbp->B;
	Fpos = tdbp->Fpos;
	N = tdbp->N;
	Done = tdbp->Done;
	Pipe = tdbp->Pipe;
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

	colp->Mbuf = (char*)PlugSubAlloc(g, NULL, colp->Long + 1);
	return colp;
	//return (colp->ParseJpath(g)) ? NULL : colp;
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
/*  MONGO Cardinality: returns table size in number of rows.           */
/***********************************************************************/
int TDBMGO::Cardinality(PGLOBAL g)
{
	if (!g)
		return 1;
	else if (Cardinal < 0)
		if (!Init(g)) {
			bson_t     *query;
			const char *jf = NULL;

			if (Pipe)
				return 10;
			else if (Filter)
				jf = Filter;

			if (jf) {
				query = bson_new_from_json((const uint8_t *)jf, -1, &Error);

				if (!query) {
					htrc("Wrong filter: %s", Error.message);
					return 10;
				}	// endif Query

			} else
				query = bson_new();

			Cardinal = (int)mongoc_collection_count(Collection,
				MONGOC_QUERY_NONE, query, 0, 0, NULL, &Error);

			if (Cardinal < 0) {
				htrc("Collection count: %s", Error.message);
				Cardinal = 10;
			} // endif Cardinal

			bson_destroy(query);
		} else
			return 10;

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
/*  Init: initialize MongoDB processing.                               */
/***********************************************************************/
bool TDBMGO::Init(PGLOBAL g)
{
	if (Done)
		return false;

	G = g;

	Uri = mongoc_uri_new(Uristr);

	if (!Uri) {
		sprintf(g->Message, "Failed to parse URI: \"%s\"", Uristr);
		return true;
	}	// endif Uri

	// Create a new client pool instance
	Pool = mongoc_client_pool_new(Uri);
	mongoc_client_pool_set_error_api(Pool, 2);

	// Register the application name so we can track it in the profile logs
	// on the server. This can also be done from the URI.
	mongoc_client_pool_set_appname(Pool, "Connect");

	// Create a new client instance
	Client = mongoc_client_pool_pop(Pool);
	//Client = mongoc_client_new(uristr);

	if (!Client) {
		sprintf(g->Message, "Failed to get Client");
		return true;
	}	// endif Client

	//mongoc_client_set_error_api(Client, 2);

	// Register the application name so we can track it in the profile logs
	// on the server. This can also be done from the URI.
	//mongoc_client_set_appname(Client, "Connect");

	// Get a handle on the database Db_name and collection Coll_name
	//	Database = mongoc_client_get_database(Client, Db_name);
	//	Collection = mongoc_database_get_collection(Database, Coll_name);
	Collection = mongoc_client_get_collection(Client, Db_name, Coll_name);

	if (!Collection) {
		sprintf(g->Message, "Failed to get Collection %s.%s", Db_name, Coll_name);
		return true;
	}	// endif Collection

	Done = true;
	return false;
} // end of Init

/***********************************************************************/
/*  On update the filter can be made by Cond_Push after MakeCursor.    */
/***********************************************************************/
void TDBMGO::SetFilter(PFIL fp)
{
	To_Filter = fp;

	if (fp && Cursor && Cnd != Cond) {
		mongoc_cursor_t *cursor = MakeCursor(G);

		if (cursor) {
			mongoc_cursor_destroy(Cursor);
			Cursor = cursor;
		} else
			htrc("SetFilter: %s\n", G->Message);

	} // endif Cursor

} // end of SetFilter

/***********************************************************************/
/*  OpenDB: Data Base open routine for MONGO access method.            */
/***********************************************************************/
mongoc_cursor_t *TDBMGO::MakeCursor(PGLOBAL g)
{
	const char      *p;
	bool             b = false, id = (Mode != MODE_READ), all = false;
	mongoc_cursor_t *cursor;
	PCOL             cp;
	PSTRG            s = NULL;

	if (Options && !stricmp(Options, "all")) {
		Options = NULL;
		all = true;
	} // endif Options

	for (cp = Columns; cp; cp = cp->GetNext())
		if (!strcmp(cp->GetName(), "_id"))
			id = true;
		else if (cp->GetFmt() && !strcmp(cp->GetFmt(), "*") && !Options)
			all = true;

	if (Pipe) {
		if (trace)
			htrc("Pipeline: %s\n", Options);

		p = strrchr(Options, ']');

		if (!p) {
			strcpy(g->Message, "Missing ] in pipeline");
			return NULL;
		} else
			*(char*)p = 0;

		s = new(g) STRING(g, 1023, (PSZ)Options);

		if (To_Filter) {
			s->Append(",{\"$match\":");

			if (To_Filter->MakeSelector(g, s, true)) {
				strcpy(g->Message, "Failed making selector");
				return NULL;
			} else
				s->Append('}');

			To_Filter = NULL;   // Not needed anymore
		} // endif To_Filter

		if (!all) {
			// Project list
			if (Columns) {
				s->Append(",{\"$project\":{\"");

				if (!id)
					s->Append("_id\":0,\"");

				for (cp = Columns; cp; cp = cp->GetNext()) {
					if (b)
						s->Append(",\"");
					else
						b = true;

					s->Append(((PMGOCOL)cp)->GetProjPath(g));
					s->Append("\":1");
				} // endfor cp

				s->Append("}}");
			} else
				s->Append(",{\"$project\":{\"_id\":1}}");

		} // endif all

		s->Append("]}");
		s->Resize(s->GetLength() + 1);
		p = s->GetStr();

		if (trace)
			htrc("New Pipeline: %s\n", p);

		Query = bson_new_from_json((const uint8_t *)p, -1, &Error);

		if (!Query) {
			sprintf(g->Message, "Wrong pipeline: %s", Error.message);
			return NULL;
		}	// endif Query

		cursor = mongoc_collection_aggregate(Collection, MONGOC_QUERY_NONE,
			Query, NULL, NULL);

		if (mongoc_cursor_error(cursor, &Error)) {
			sprintf(g->Message, "Mongo aggregate Failure: %s", Error.message);
			return NULL;
		} // endif cursor

	} else {
		if (Filter || To_Filter) {
			if (trace) {
				if (Filter)
					htrc("Filter: %s\n", Filter);

				if (To_Filter) {
					char buf[512];

					To_Filter->Prints(g, buf, 511);
					htrc("To_Filter: %s\n", buf);
				} // endif To_Filter

			}	// endif trace

			s = new(g) STRING(g, 1023, (PSZ)Filter);

			if (To_Filter) {
				if (Filter)
					s->Append(',');

				if (To_Filter->MakeSelector(g, s, true)) {
					strcpy(g->Message, "Failed making selector");
					return NULL;
				}	// endif Selector

				To_Filter = NULL;   // Not needed anymore
			} // endif To_Filter

			if (trace)
				htrc("selector: %s\n", s->GetStr());

			s->Resize(s->GetLength() + 1);
			Query = bson_new_from_json((const uint8_t *)s->GetStr(), -1, &Error);

			if (!Query) {
				sprintf(g->Message, "Wrong filter: %s", Error.message);
				return NULL;
			}	// endif Query

		} else
			Query = bson_new();

		if (!all) {
			if (Options && *Options) {
				if (trace)
					htrc("options=%s\n", Options);

				p = Options;
			} else if (Columns) {
				// Projection list
				if (s)
					s->Set("{\"projection\":{\"");
				else
					s = new(g) STRING(g, 511, "{\"projection\":{\"");

				if (!id)
					s->Append("_id\":0,\"");

				for (cp = Columns; cp; cp = cp->GetNext()) {
					if (b)
						s->Append(",\"");
					else
						b = true;

					s->Append(((PMGOCOL)cp)->GetProjPath(g));
					s->Append("\":1");
				} // endfor cp

				s->Append("}}");
				s->Resize(s->GetLength() + 1);
				p = s->GetStr();
			} else {
				// count(*)	?
				p = "{\"projection\":{\"_id\":1}}";
			} // endif Options

			Opts = bson_new_from_json((const uint8_t *)p, -1, &Error);

			if (!Opts) {
				sprintf(g->Message, "Wrong options: %s", Error.message);
				return NULL;
			} // endif Opts

		} // endif all

		cursor = mongoc_collection_find_with_opts(Collection, Query, Opts, NULL);
	} // endif Pipe

	return cursor;
} // end of MakeCursor

/***********************************************************************/
/*  OpenDB: Data Base open routine for MONGO access method.            */
/***********************************************************************/
bool TDBMGO::OpenDB(PGLOBAL g)
{
	if (Use == USE_OPEN) {
		/*******************************************************************/
		/*  Table already open replace it at its beginning.                */
		/*******************************************************************/
		mongoc_cursor_t *cursor = mongoc_cursor_clone(Cursor);

		mongoc_cursor_destroy(Cursor);
		Cursor = cursor;
		Fpos = -1;
		return false;
	} // endif Use

	/*********************************************************************/
	/*  First opening.                                                   */
	/*********************************************************************/
	if (Pipe && Mode != MODE_READ) {
		strcpy(g->Message, "Pipeline tables are read only");
		return true;
	}	// endif Pipe

	if (Init(g))
		return true;

	if (Mode == MODE_DELETE && !Next) {
		// Delete all documents
		Query = bson_new();

		if (!mongoc_collection_remove(Collection, MONGOC_REMOVE_NONE,
			Query, NULL, &Error)) {
			sprintf(g->Message, "Mongo remove all: %s", Error.message);
			return true;
		}	// endif remove

	} else if (Mode == MODE_INSERT)
		MakeColumnGroups(g);
	else if (!(Cursor = MakeCursor(g)))
		return true;

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
	int rc = RC_OK;

	if (mongoc_cursor_next(Cursor, &Document)) {

		if (trace > 1) {
			bson_iter_t iter;
			ShowDocument(&iter, Document, "");
		} else if (trace == 1) {
			char *str = bson_as_json(Document, NULL);
			htrc("%s\n", str);
			bson_free(str);
		}	// endif trace

	} else if (mongoc_cursor_error(Cursor, &Error)) {
		sprintf(g->Message, "Mongo Cursor Failure: %s", Error.message);
		rc = RC_FX;
	} else {
		//mongoc_cursor_destroy(Cursor);
		rc = RC_EF;
	}	// endif's Cursor

	return rc;
} // end of ReadDB

/***********************************************************************/
/*  Use to trace restaurants document contains.                        */
/***********************************************************************/
void TDBMGO::ShowDocument(bson_iter_t *iter, const bson_t *doc, const char *k)
{
	if (!doc || bson_iter_init(iter, doc)) {
		const char *key;

		while (bson_iter_next(iter)) {
			key = bson_iter_key(iter);
			htrc("Found element key: \"%s\"\n", key);

			if (BSON_ITER_HOLDS_UTF8(iter))
				htrc("%s.%s=\"%s\"\n", k, key, bson_iter_utf8(iter, NULL));
			else if (BSON_ITER_HOLDS_INT32(iter))
				htrc("%s.%s=%d\n", k, key, bson_iter_int32(iter));
			else if (BSON_ITER_HOLDS_INT64(iter))
				htrc("%s.%s=%lld\n", k, key, bson_iter_int64(iter));
			else if (BSON_ITER_HOLDS_DOUBLE(iter))
				htrc("%s.%s=%g\n", k, key, bson_iter_double(iter));
			else if (BSON_ITER_HOLDS_DATE_TIME(iter))
				htrc("%s.%s=date(%lld)\n", k, key, bson_iter_date_time(iter));
			else if (BSON_ITER_HOLDS_OID(iter)) {
				char str[25];

				bson_oid_to_string(bson_iter_oid(iter), str);
				htrc("%s.%s=%s\n", k, key, str);
			} else if (BSON_ITER_HOLDS_DECIMAL128(iter)) {
				char             *str = NULL;
				bson_decimal128_t dec;

				bson_iter_decimal128(iter, &dec);
				bson_decimal128_to_string(&dec, str);
				htrc("%s.%s=%s\n", k, key, str);
			} else if (BSON_ITER_HOLDS_DOCUMENT(iter)) {
				bson_iter_t child;

				if (bson_iter_recurse(iter, &child))
					ShowDocument(&child, NULL, key);

			} else if (BSON_ITER_HOLDS_ARRAY(iter)) {
				bson_t				*arr;
				bson_iter_t    itar;
				const uint8_t *data = NULL;
				uint32_t       len = 0;

				bson_iter_array(iter, &len, &data);
				arr = bson_new_from_data(data, len);
				ShowDocument(&itar, arr, key);
			}	// endif's

		}	// endwhile bson_iter_next

	} // endif bson_iter_init

} // end of ShowDocument

/***********************************************************************/
/*  Group columns for inserting or updating.                           */
/***********************************************************************/
void TDBMGO::MakeColumnGroups(PGLOBAL g)
{
	Fpc = new(g) INCOL;

	for (PCOL colp = Columns; colp; colp = colp->GetNext())
		if (!colp->IsSpecial())
			Fpc->AddCol(g, colp, ((PMGOCOL)colp)->Jpath);

} // end of MakeColumnGroups

/***********************************************************************/
/*  DocWrite.                                                          */
/***********************************************************************/
bool TDBMGO::DocWrite(PGLOBAL g, PINCOL icp)
{
	for (PKC kp = icp->Klist; kp; kp = kp->Next)
		if (kp->Incolp) {
			BSON_APPEND_DOCUMENT_BEGIN(&icp->Child, kp->Key, &kp->Incolp->Child);

			if (DocWrite(g, kp->Incolp))
				return true;

			bson_append_document_end(&icp->Child, &kp->Incolp->Child);
		} else if (((PMGOCOL)kp->Colp)->AddValue(g, &icp->Child, kp->Key, false))
			return true;

	return false;
} // end of DocWrite

/***********************************************************************/
/*  WriteDB: Data Base write routine for DOS access method.            */
/***********************************************************************/
int TDBMGO::WriteDB(PGLOBAL g)
{
	int rc = RC_OK;

	if (Mode == MODE_INSERT) {
		bson_init(&Fpc->Child);

		if (DocWrite(g, Fpc))
			return RC_FX;

		if (trace) {
			char *str = bson_as_json(&Fpc->Child, NULL);
			htrc("Inserting: %s\n", str);
			bson_free(str);
		} // endif trace

		if (!mongoc_collection_insert(Collection, MONGOC_INSERT_NONE,
			                            &Fpc->Child, NULL, &Error)) {
			sprintf(g->Message, "Mongo insert: %s", Error.message);
			rc = RC_FX;
		} // endif insert

	} else {
		bool        b = false;
		bson_iter_t iter;
		bson_t     *query = bson_new();

		bson_iter_init(&iter, Document);

		if (bson_iter_find(&iter, "_id")) {
			if (BSON_ITER_HOLDS_OID(&iter))
				b = BSON_APPEND_OID(query, "_id", bson_iter_oid(&iter));
			else if (BSON_ITER_HOLDS_INT32(&iter))
				b = BSON_APPEND_INT32(query, "_id", bson_iter_int32(&iter));
			else if (BSON_ITER_HOLDS_INT64(&iter))
				b = BSON_APPEND_INT64(query, "_id", bson_iter_int64(&iter));
			else if (BSON_ITER_HOLDS_DOUBLE(&iter))
				b = BSON_APPEND_DOUBLE(query, "_id", bson_iter_double(&iter));
			else if (BSON_ITER_HOLDS_UTF8(&iter))
				b = BSON_APPEND_UTF8(query, "_id", bson_iter_utf8(&iter, NULL));

		} // endif iter

		if (b) {
			if (trace) {
				char *str = bson_as_json(query, NULL);
				htrc("update query: %s\n", str);
				bson_free(str);
			}	// endif trace

			if (Mode == MODE_UPDATE) {
				bson_t  child;
				bson_t *update = bson_new();

				BSON_APPEND_DOCUMENT_BEGIN(update, "$set", &child);

				for (PCOL colp = To_SetCols; colp; colp = colp->GetNext())
					if (((PMGOCOL)colp)->AddValue(g, &child, ((PMGOCOL)colp)->Jpath, true))
						rc = RC_FX;

				bson_append_document_end(update, &child);

				if (rc == RC_OK)
					if (!mongoc_collection_update(Collection, MONGOC_UPDATE_NONE,
						query, update, NULL, &Error)) {
						sprintf(g->Message, "Mongo update: %s", Error.message);
						rc = RC_FX;
					} // endif update

				bson_destroy(update);
			} else if (!mongoc_collection_remove(Collection,
				MONGOC_REMOVE_SINGLE_REMOVE, query, NULL, &Error)) {
				sprintf(g->Message, "Mongo delete: %s", Error.message);
				rc = RC_FX;
			} // endif remove

		} else {
			strcpy(g->Message, "Mongo update: cannot find _id");
			rc = RC_FX;
		}	// endif b

		bson_destroy(query);
	} // endif Mode

	return rc;
} // end of WriteDB

/***********************************************************************/
/*  Data Base delete line routine for ODBC access method.              */
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
	if (Query) bson_destroy(Query);
	if (Opts) bson_destroy(Opts);
	if (Cursor)	mongoc_cursor_destroy(Cursor);
	if (Collection) mongoc_collection_destroy(Collection);
	//	mongoc_database_destroy(Database);
	//	mongoc_client_destroy(Client);
	if (Client) mongoc_client_pool_push(Pool, Client);
	if (Pool) mongoc_client_pool_destroy(Pool);
	if (Uri) mongoc_uri_destroy(Uri);
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
	Mbuf = NULL;
} // end of MGOCOL constructor

/***********************************************************************/
/*  MGOCOL constructor used for copying columns.                       */
/*  tdbp is the pointer to the new table descriptor.                   */
/***********************************************************************/
MGOCOL::MGOCOL(MGOCOL *col1, PTDB tdbp) : EXTCOL(col1, tdbp)
{
	Tmgp = col1->Tmgp;
	Jpath = col1->Jpath;
	Mbuf = col1->Mbuf;
} // end of MGOCOL copy constructor

/***********************************************************************/
/*  Get projection path.                                               */
/***********************************************************************/
char *MGOCOL::GetProjPath(PGLOBAL g)
{
	if (Jpath) {
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
		return NULL;

} // end of GetProjPath

/***********************************************************************/
/*  Mini: used to suppress blanks to json strings.                     */
/***********************************************************************/
char *MGOCOL::Mini(PGLOBAL g, const bson_t *bson, bool b)
{
	char *s, *str = NULL;
	int   i, k = 0;
	bool  ok = true;

	if (b)
		s = str = bson_array_as_json(bson, NULL);
	else
		s = str = bson_as_json(bson, NULL);

	for (i = 0; i < Long && s[i]; i++) {
		switch (s[i]) {
			case ' ':
				if (ok) continue;
			case '"':
				ok = !ok;
			default:
				break;
		} // endswitch s[i]

		Mbuf[k++] = s[i];
	} // endfor i

	bson_free(str);

	if (i >= Long) {
		sprintf(g->Message, "Value too long for column %s", Name);
		throw(TYPE_AM_MGO);
	}	// endif i

	Mbuf[k] = 0;
	return Mbuf;
} // end of Mini

/***********************************************************************/
/*  ReadColumn:                                                        */
/***********************************************************************/
void MGOCOL::ReadColumn(PGLOBAL g)
{

	if (!strcmp(Jpath, "*")) {
		Value->SetValue_psz(Mini(g, Tmgp->Document, false));
	}	else if (bson_iter_init(&Iter, Tmgp->Document) &&
		  bson_iter_find_descendant(&Iter, Jpath, &Desc)) {
		if (BSON_ITER_HOLDS_UTF8(&Desc))
			Value->SetValue_psz((PSZ)bson_iter_utf8(&Desc, NULL));
		else if (BSON_ITER_HOLDS_INT32(&Desc))
			Value->SetValue(bson_iter_int32(&Desc));
		else if (BSON_ITER_HOLDS_INT64(&Desc))
			Value->SetValue(bson_iter_int64(&Desc));
		else if (BSON_ITER_HOLDS_DOUBLE(&Desc))
			Value->SetValue(bson_iter_double(&Desc));
		else if (BSON_ITER_HOLDS_DATE_TIME(&Desc))
			Value->SetValue(bson_iter_date_time(&Desc) / 1000);
		else if (BSON_ITER_HOLDS_BOOL(&Desc)) {
			bool b = bson_iter_bool(&Desc);

			if (Value->IsTypeNum())
				Value->SetValue(b ? 1 : 0);
			else
				Value->SetValue_psz(b ? "true" : "false");

		} else if (BSON_ITER_HOLDS_OID(&Desc)) {
			char str[25];

			bson_oid_to_string(bson_iter_oid(&Desc), str);
			Value->SetValue_psz(str);
		} else if (BSON_ITER_HOLDS_DECIMAL128(&Desc)) {
			char *str = NULL;
			bson_decimal128_t dec;

			bson_iter_decimal128(&Desc, &dec);
			bson_decimal128_to_string(&dec, str);
			Value->SetValue_psz(str);
			bson_free(str);
		} else if (BSON_ITER_HOLDS_DOCUMENT(&Iter)) {
			bson_t				*doc;
			const uint8_t *data = NULL;
			uint32_t       len = 0;

			bson_iter_document(&Desc, &len, &data);

			if (data) {
				doc = bson_new_from_data(data, len);
				Value->SetValue_psz(Mini(g, doc, false));
				bson_destroy(doc);
			} else
				Value->Reset();

		} else if (BSON_ITER_HOLDS_ARRAY(&Iter)) {
			bson_t				*arr;
			const uint8_t *data = NULL;
			uint32_t       len = 0;

			bson_iter_array(&Desc, &len, &data);

			if (data) {
				arr = bson_new_from_data(data, len);
				Value->SetValue_psz(Mini(g, arr, true));
				bson_destroy(arr);
			} else {
				// This is a bug in returning the wrong type
				// This fix is only for document items
				bson_t *doc;

				bson_iter_document(&Desc, &len, &data);

				if (data) {
					doc = bson_new_from_data(data, len);
					Value->SetValue_psz(Mini(g, doc, false));
					bson_destroy(doc);
				} else {
					//strcpy(g->Message, "bson_iter_array failed (data is null)");
					//throw(TYPE_AM_MGO);
					Value->Reset();
				}	// endif data

			} // endif data

		} else
			Value->Reset();

	} else
		Value->Reset();

	// Set null when applicable
	if (Nullable)
		Value->SetNull(Value->IsZero());

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

/***********************************************************************/
/*  AddValue: Add column value to the document to insert or update.    */
/***********************************************************************/
bool MGOCOL::AddValue(PGLOBAL g, bson_t *doc, char *key, bool upd)
{
	bool rc = false;

	if (Value->IsNull()) {
		if (upd)
			rc = BSON_APPEND_NULL(doc, key);
		else
		  return false;

	} else switch (Buf_Type) {
		case TYPE_STRING:
			rc = BSON_APPEND_UTF8(doc, key, Value->GetCharValue());
			break;
		case TYPE_INT:
		case TYPE_SHORT:
			rc = BSON_APPEND_INT32(doc, key, Value->GetIntValue());
			break;
		case TYPE_TINY:
			rc = BSON_APPEND_BOOL(doc, key, Value->GetIntValue());
			break;
		case TYPE_BIGINT:
			rc = BSON_APPEND_INT64(doc, key, Value->GetBigintValue());
			break;
		case TYPE_DOUBLE:
			rc = BSON_APPEND_DOUBLE(doc, key, Value->GetFloatValue());
			break;
		case TYPE_DECIM:
		 {bson_decimal128_t dec;

		  if (bson_decimal128_from_string(Value->GetCharValue(), &dec))
			  rc = BSON_APPEND_DECIMAL128(doc, key, &dec);

		  } break;
		case TYPE_DATE:
			rc = BSON_APPEND_DATE_TIME(doc, key, Value->GetBigintValue() * 1000);
			break;
		default:
			sprintf(g->Message, "Type %d not supported yet", Buf_Type);
			return true;
	} // endswitch Buf_Type

	if (!rc) {
		strcpy(g->Message, "Adding value failed");
		return true;
	} else
		return false;

} // end of AddValue

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
