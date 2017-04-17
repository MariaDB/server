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

/***********************************************************************/
/*  This should be an option.                                          */
/***********************************************************************/
#define MAXCOL          200        /* Default max column nb in result  */
#define TYPE_UNKNOWN     12        /* Must be greater than other types */

typedef struct _jncol {
	struct _jncol *Next;
	char *Name;
	char *Fmt;
	int   Type;
	int   Len;
	int   Scale;
	bool  Cbn;
	bool  Found;
} JCOL, *PJCL;

#if 0
/***********************************************************************/
/* JSONColumns: construct the result blocks containing the description */
/* of all the columns of a table contained inside a JSON file.         */
/***********************************************************************/
PQRYRES JSONColumns(PGLOBAL g, char *db, char *dsn, PTOS topt, bool info)
{
	static int  buftyp[] = {TYPE_STRING, TYPE_SHORT, TYPE_STRING, TYPE_INT,
		TYPE_INT, TYPE_SHORT, TYPE_SHORT, TYPE_STRING};
	static XFLD fldtyp[] = {FLD_NAME, FLD_TYPE, FLD_TYPENAME, FLD_PREC,
		FLD_LENGTH, FLD_SCALE, FLD_NULL, FLD_FORMAT};
	static unsigned int length[] = {0, 6, 8, 10, 10, 6, 6, 0};
	char    colname[65], fmt[129];
	int     i, j, lvl, n = 0;
	int     ncol = sizeof(buftyp) / sizeof(int);
	PVAL    valp;
	JCOL    jcol;
	PJCL    jcp, fjcp = NULL, pjcp = NULL;
	PJPR   *jrp, jpp;
	PJSON   jsp;
	PJVAL   jvp;
	PJOB    row;
	PMGODEF   tdp;
	TDBMGO *tjnp = NULL;
	PJTDB   tjsp = NULL;
	PQRYRES qrp;
	PCOLRES crp;

	jcol.Name = jcol.Fmt = NULL;

	if (info) {
		length[0] = 128;
		length[7] = 256;
		goto skipit;
	} // endif info

	if (GetIntegerTableOption(g, topt, "Multiple", 0)) {
		strcpy(g->Message, "Cannot find column definition for multiple table");
		return NULL;
	}	// endif Multiple

		/*********************************************************************/
		/*  Open the input file.                                             */
		/*********************************************************************/
	lvl = GetIntegerTableOption(g, topt, "Level", 0);
	lvl = (lvl < 0) ? 0 : (lvl > 16) ? 16 : lvl;

	tdp = new(g) MGODEF;
#if defined(ZIP_SUPPORT)
	tdp->Entry = GetStringTableOption(g, topt, "Entry", NULL);
	tdp->Zipped = GetBooleanTableOption(g, topt, "Zipped", false);
#endif   // ZIP_SUPPORT
	tdp->Fn = GetStringTableOption(g, topt, "Filename", NULL);

	if (!tdp->Fn && !dsn) {
		strcpy(g->Message, MSG(MISSING_FNAME));
		return NULL;
	} // endif Fn

	tdp->Database = SetPath(g, db);
	tdp->Objname = GetStringTableOption(g, topt, "Object", NULL);
	tdp->Base = GetIntegerTableOption(g, topt, "Base", 0) ? 1 : 0;
	tdp->Pretty = GetIntegerTableOption(g, topt, "Pretty", 2);

	if (trace)
		htrc("File %s objname=%s pretty=%d lvl=%d\n",
			tdp->Fn, tdp->Objname, tdp->Pretty, lvl);

	if (tdp->Uri = dsn) {
#if defined(MONGO_SUPPORT)
		tdp->Collname = GetStringTableOption(g, topt, "Name", NULL);
		tdp->Collname = GetStringTableOption(g, topt, "Tabname", tdp->Collname);
		tdp->Schema = GetStringTableOption(g, topt, "Dbname", "test");
		tdp->Options = GetStringTableOption(g, topt, "Colist", NULL);
		tdp->Pretty = 0;
#else   // !MONGO_SUPPORT
		sprintf(g->Message, MSG(NO_FEAT_SUPPORT), "MONGO");
		return NULL;
#endif  // !MONGO_SUPPORT
	}	// endif Uri

	if (tdp->Pretty == 2) {
		if (tdp->Zipped) {
#if defined(ZIP_SUPPORT)
			tjsp = new(g) TDBJSON(tdp, new(g) UNZFAM(tdp));
#else   // !ZIP_SUPPORT
			sprintf(g->Message, MSG(NO_FEAT_SUPPORT), "ZIP");
			return NULL;
#endif  // !ZIP_SUPPORT
		} else
			tjsp = new(g) TDBJSON(tdp, new(g) MAPFAM(tdp));

		if (tjsp->MakeDocument(g))
			return NULL;

		jsp = (tjsp->GetDoc()) ? tjsp->GetDoc()->GetValue(0) : NULL;
	} else {
		if (!(tdp->Lrecl = GetIntegerTableOption(g, topt, "Lrecl", 0))) {
			sprintf(g->Message, "LRECL must be specified for pretty=%d", tdp->Pretty);
			return NULL;
		} // endif lrecl

		tdp->Ending = GetIntegerTableOption(g, topt, "Ending", CRLF);

		if (tdp->Zipped) {
#if defined(ZIP_SUPPORT)
			tjnp = new(g)TDBMGO(tdp, new(g) UNZFAM(tdp));
#else   // !ZIP_SUPPORT
			sprintf(g->Message, MSG(NO_FEAT_SUPPORT), "ZIP");
			return NULL;
#endif  // !ZIP_SUPPORT
		} else if (tdp->Uri) {
#if defined(MONGO_SUPPORT)
			tjnp = new(g) TDBMGO(tdp, new(g) MGOFAM(tdp));
#else   // !MONGO_SUPPORT
			sprintf(g->Message, MSG(NO_FEAT_SUPPORT), "MONGO");
			return NULL;
#endif  // !MONGO_SUPPORT
		} else
			tjnp = new(g) TDBMGO(tdp, new(g) DOSFAM(tdp));

		tjnp->SetMode(MODE_READ);

#if USE_G
		// Allocate the parse work memory
		PGLOBAL G = (PGLOBAL)PlugSubAlloc(g, NULL, sizeof(GLOBAL));
		memset(G, 0, sizeof(GLOBAL));
		G->Sarea_Size = tdp->Lrecl * 10;
		G->Sarea = PlugSubAlloc(g, NULL, G->Sarea_Size);
		PlugSubSet(G, G->Sarea, G->Sarea_Size);
		G->jump_level = 0;
		tjnp->SetG(G);
#else
		tjnp->SetG(g);
#endif

		if (tjnp->OpenDB(g))
			return NULL;

		switch (tjnp->ReadDB(g)) {
			case RC_EF:
				strcpy(g->Message, "Void json table");
			case RC_FX:
				goto err;
			default:
				jsp = tjnp->GetRow();
		} // endswitch ReadDB

	} // endif pretty

	if (!(row = (jsp) ? jsp->GetObject() : NULL)) {
		strcpy(g->Message, "Can only retrieve columns from object rows");
		goto err;
	} // endif row

	jcol.Next = NULL;
	jcol.Found = true;
	colname[64] = 0;
	fmt[128] = 0;
	jrp = (PJPR*)PlugSubAlloc(g, NULL, sizeof(PJPR) * lvl);

	/*********************************************************************/
	/*  Analyse the JSON tree and define columns.                        */
	/*********************************************************************/
	for (i = 1; ; i++) {
		for (jpp = row->GetFirst(); jpp; jpp = jpp->GetNext()) {
			for (j = 0; j < lvl; j++)
				jrp[j] = NULL;

		more:
			strncpy(colname, jpp->GetKey(), 64);
			*fmt = 0;
			j = 0;
			jvp = jpp->GetVal();

		retry:
			if ((valp = jvp ? jvp->GetValue() : NULL)) {
				jcol.Type = valp->GetType();
				jcol.Len = valp->GetValLen();
				jcol.Scale = valp->GetValPrec();
				jcol.Cbn = valp->IsNull();
			} else if (!jvp || jvp->IsNull()) {
				jcol.Type = TYPE_UNKNOWN;
				jcol.Len = jcol.Scale = 0;
				jcol.Cbn = true;
			} else  if (j < lvl) {
				if (!*fmt)
					strcpy(fmt, colname);

				jsp = jvp->GetJson();

				switch (jsp->GetType()) {
					case TYPE_JOB:
						if (!jrp[j])
							jrp[j] = jsp->GetFirst();

						strncat(strncat(fmt, ":", 128), jrp[j]->GetKey(), 128);
						strncat(strncat(colname, "_", 64), jrp[j]->GetKey(), 64);
						jvp = jrp[j]->GetVal();
						j++;
						break;
					case TYPE_JAR:
						strncat(fmt, ":", 128);
						jvp = jsp->GetValue(0);
						break;
					default:
						sprintf(g->Message, "Logical error after %s", fmt);
						goto err;
				} // endswitch jsp

				goto retry;
			} else {
				jcol.Type = TYPE_STRING;
				jcol.Len = 256;
				jcol.Scale = 0;
				jcol.Cbn = true;
			} // endif's

				// Check whether this column was already found
			for (jcp = fjcp; jcp; jcp = jcp->Next)
				if (!strcmp(colname, jcp->Name))
					break;

			if (jcp) {
				if (jcp->Type != jcol.Type)
					jcp->Type = TYPE_STRING;

				if (*fmt && (!jcp->Fmt || strlen(jcp->Fmt) < strlen(fmt))) {
					jcp->Fmt = PlugDup(g, fmt);
					length[7] = MY_MAX(length[7], strlen(fmt));
				} // endif *fmt

				jcp->Len = MY_MAX(jcp->Len, jcol.Len);
				jcp->Scale = MY_MAX(jcp->Scale, jcol.Scale);
				jcp->Cbn |= jcol.Cbn;
				jcp->Found = true;
			} else {
				// New column
				jcp = (PJCL)PlugSubAlloc(g, NULL, sizeof(JCOL));
				*jcp = jcol;
				jcp->Cbn |= (i > 1);
				jcp->Name = PlugDup(g, colname);
				length[0] = MY_MAX(length[0], strlen(colname));

				if (*fmt) {
					jcp->Fmt = PlugDup(g, fmt);
					length[7] = MY_MAX(length[7], strlen(fmt));
				} else
					jcp->Fmt = NULL;

				if (pjcp) {
					jcp->Next = pjcp->Next;
					pjcp->Next = jcp;
				} else
					fjcp = jcp;

				n++;
			} // endif jcp

			pjcp = jcp;

			for (j = lvl - 1; j >= 0; j--)
				if (jrp[j] && (jrp[j] = jrp[j]->GetNext()))
					goto more;

		} // endfor jpp

			// Missing column can be null
		for (jcp = fjcp; jcp; jcp = jcp->Next) {
			jcp->Cbn |= !jcp->Found;
			jcp->Found = false;
		} // endfor jcp

		if (tdp->Pretty != 2) {
			// Read next record
			switch (tjnp->ReadDB(g)) {
				case RC_EF:
					jsp = NULL;
					break;
				case RC_FX:
					goto err;
				default:
					jsp = tjnp->GetRow();
			} // endswitch ReadDB

		} else
			jsp = tjsp->GetDoc()->GetValue(i);

		if (!(row = (jsp) ? jsp->GetObject() : NULL))
			break;

	} // endor i

	if (tdp->Pretty != 2)
		tjnp->CloseDB(g);

skipit:
	if (trace)
		htrc("CSVColumns: n=%d len=%d\n", n, length[0]);

	/*********************************************************************/
	/*  Allocate the structures used to refer to the result set.         */
	/*********************************************************************/
	qrp = PlgAllocResult(g, ncol, n, IDS_COLUMNS + 3,
		buftyp, fldtyp, length, false, false);

	crp = qrp->Colresp->Next->Next->Next->Next->Next->Next;
	crp->Name = "Nullable";
	crp->Next->Name = "Jpath";

	if (info || !qrp)
		return qrp;

	qrp->Nblin = n;

	/*********************************************************************/
	/*  Now get the results into blocks.                                 */
	/*********************************************************************/
	for (i = 0, jcp = fjcp; jcp; i++, jcp = jcp->Next) {
		if (jcp->Type == TYPE_UNKNOWN)            // Void column
			jcp->Type = TYPE_STRING;

		crp = qrp->Colresp;                    // Column Name
		crp->Kdata->SetValue(jcp->Name, i);
		crp = crp->Next;                       // Data Type
		crp->Kdata->SetValue(jcp->Type, i);
		crp = crp->Next;                       // Type Name
		crp->Kdata->SetValue(GetTypeName(jcp->Type), i);
		crp = crp->Next;                       // Precision
		crp->Kdata->SetValue(jcp->Len, i);
		crp = crp->Next;                       // Length
		crp->Kdata->SetValue(jcp->Len, i);
		crp = crp->Next;                       // Scale (precision)
		crp->Kdata->SetValue(jcp->Scale, i);
		crp = crp->Next;                       // Nullable
		crp->Kdata->SetValue(jcp->Cbn ? 1 : 0, i);
		crp = crp->Next;                       // Field format

		if (crp->Kdata)
			crp->Kdata->SetValue(jcp->Fmt, i);

	} // endfor i

		/*********************************************************************/
		/*  Return the result pointer.                                       */
		/*********************************************************************/
	return qrp;

err:
	if (tdp->Pretty != 2)
		tjnp->CloseDB(g);

	return NULL;
} // end of JSONColumns
#endif // 0

/* -------------------------- Class MGODEF --------------------------- */

MGODEF::MGODEF(void)
{
	Uri = NULL;
	Colist = NULL;
	Filter = NULL;
	Level = 0;
	Base = 0;
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

	Uri = GetStringCatInfo(g, "Connect", NULL);
	Colist = GetStringCatInfo(g, "Colist", NULL);
	Filter = GetStringCatInfo(g, "Filter", NULL);
	Base = GetIntCatInfo("Base", 0) ? 1 : 0;
	return false;
} // end of DefineAM

/***********************************************************************/
/*  GetTable: makes a new Table Description Block.                     */
/***********************************************************************/
PTDB MGODEF::GetTable(PGLOBAL g, MODE m)
{
	//if (Catfunc == FNC_COL)
	//	return new(g)TDBGOL(this);

	return new(g) TDBMGO(this);
} // end of GetTable

/* --------------------------- Class TDBMGO -------------------------- */

/***********************************************************************/
/*  Implementation of the TDBMGO class.                                */
/***********************************************************************/
TDBMGO::TDBMGO(PMGODEF tdp) : TDBEXT(tdp)
{
	Client = NULL;
	Database = NULL;
	Collection = NULL;
	Cursor = NULL;
	Query = NULL;
	Opts = NULL;

	if (tdp) {
		Uristr = tdp->Uri;
		Db_name = tdp->Tabschema;
		Coll_name = tdp->Tabname;
		Options = tdp->Colist;
		Filter = tdp->Filter;
		B = tdp->Base ? 1 : 0;
	} else {
		Uristr = NULL;
		Db_name = NULL;
		Coll_name = NULL;
		Options = NULL;
		Filter = NULL;
		B = 0;
	} // endif tdp

	Fpos = -1;
	N = 0;
	Done = false;
} // end of TDBMGO standard constructor

TDBMGO::TDBMGO(TDBMGO *tdbp) : TDBEXT(tdbp)
{
	Client = tdbp->Client;
	Database = NULL;
	Collection = tdbp->Collection;
	Cursor = tdbp->Cursor;
	Query = tdbp->Query;
	Opts = tdbp->Opts;
	Options = tdbp->Options;
	Filter = tdbp->Filter;
	Fpos = tdbp->Fpos;
	N = tdbp->N;
	B = tdbp->B;
	Done = tdbp->Done;
} // end of TDBMGO copy constructor

	// Used for update
PTDB TDBMGO::Clone(PTABS t)
{
	PTDB    tp;
	PMGOCOL cp1, cp2;
	PGLOBAL g = t->G;

	tp = new(g) TDBMGO(this);

	for (cp1 = (PMGOCOL)Columns; cp1; cp1 = (PMGOCOL)cp1->GetNext()) {
		cp2 = new(g) MGOCOL(cp1, tp);  // Make a copy
		NewPointer(t, cp1, cp2);
	} // endfor cp1

	return tp;
} // end of Clone

/***********************************************************************/
/*  Allocate JSN column description block.                             */
/***********************************************************************/
PCOL TDBMGO::MakeCol(PGLOBAL g, PCOLDEF cdp, PCOL cprec, int n)
{
	PMGOCOL colp = new(g) MGOCOL(g, cdp, this, cprec, n);

//return (colp->ParseJpath(g)) ? NULL : colp;
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
/*  MONGO Cardinality: returns table size in number of rows.           */
/***********************************************************************/
int TDBMGO::Cardinality(PGLOBAL g)
{
	if (!g)
		return 0;
	else if (Cardinal < 0)
		Cardinal = 10;

	return Cardinal;
} // end of Cardinality

/***********************************************************************/
/*  MONGO GetMaxSize: returns file size estimate in number of lines.   */
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

	if (Filter && *Filter) {
		if (trace)
			htrc("filter=%s\n", Filter);

		Query = bson_new_from_json((const uint8_t *)Filter, -1, &Error);

		if (!Query) {
			sprintf(g->Message, "Wrong filter: %s", Error.message);
			return true;
		}	// endif Query

	} else
		Query = bson_new();

	if (Options && *Options) {
		if (trace)
			htrc("options=%s\n", Options);

		Opts = bson_new_from_json((const uint8_t *)Options, -1, &Error);

		if (!Opts) {
			sprintf(g->Message, "Wrong options: %s", Error.message);
			return true;
		} // endif Opts

	} // endif options

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
/*  OpenDB: Data Base open routine for MONGO access method.            */
/***********************************************************************/
bool TDBMGO::OpenDB(PGLOBAL g)
{
	if (Use == USE_OPEN) {
		/*******************************************************************/
		/*  Table already open replace it at its beginning.                */
		/*******************************************************************/
		Fpos = -1;
	} else {
		/*******************************************************************/
		/*  First opening.                                                 */
		/*******************************************************************/
		if (Init(g))
			return true;
	  else if (Mode != MODE_INSERT)
		  Cursor = mongoc_collection_find_with_opts(Collection, Query, Opts, NULL);

	} // endif Use

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
/*  WriteDB: Data Base write routine for DOS access method.            */
/***********************************************************************/
int TDBMGO::WriteDB(PGLOBAL g)
{
	strcpy(g->Message, "MONGO tables are read only");
	return RC_FX;
} // end of WriteDB

/***********************************************************************/
/*  Data Base delete line routine for ODBC access method.              */
/***********************************************************************/
int TDBMGO::DeleteDB(PGLOBAL g, int irc)
{
	strcpy(g->Message, "MONGO tables are read only");
	return RC_FX;
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
/*  SetBuffer: prepare a column block for write operation.             */
/***********************************************************************/
bool MGOCOL::SetBuffer(PGLOBAL g, PVAL value, bool ok, bool check)
{
	return false;
} // end of SetBuffer

/***********************************************************************/
/*  ReadColumn:                                                        */
/***********************************************************************/
void MGOCOL::ReadColumn(PGLOBAL g)
{

	if (bson_iter_init(&Iter, Tmgp->Document) &&
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
		else if (BSON_ITER_HOLDS_OID(&Desc)) {
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
			char          *str = NULL;
			bson_t				*doc;
			const uint8_t *data = NULL;
			uint32_t       len = 0;

			bson_iter_document(&Desc, &len, &data);
			doc = bson_new_from_data(data, len);
			str = bson_as_json(doc, NULL);
			Value->SetValue_psz(str);
			bson_free(str);
			bson_destroy(doc);
		} else if (BSON_ITER_HOLDS_ARRAY(&Iter)) {
			char          *str = NULL;
			bson_t				*arr;
			const uint8_t *data = NULL;
			uint32_t       len = 0;

			bson_iter_array(&Desc, &len, &data);
			arr = bson_new_from_data(data, len);
			str = bson_as_json(arr, NULL);
			Value->SetValue_psz(str);
			bson_free(str);
			bson_destroy(arr);
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
	strcpy(g->Message, "Write MONGO columns not implemented yet");
	throw 666;
} // end of WriteColumn

#if 0
/* ---------------------------TDBGOL class --------------------------- */

/***********************************************************************/
/*  TDBGOL class constructor.                                          */
/***********************************************************************/
TDBJCL::TDBJCL(PMGODEF tdp) : TDBCAT(tdp)
{
	Topt = tdp->GetTopt();
	Db = (char*)tdp->GetDB();
	Dsn = (char*)tdp->Uri;
} // end of TDBJCL constructor

/***********************************************************************/
/*  GetResult: Get the list the JSON file columns.                     */
/***********************************************************************/
PQRYRES TDBGOL::GetResult(PGLOBAL g)
{
	return JSONColumns(g, Db, Dsn, Topt, false);
} // end of GetResult
#endif // 0

/* -------------------------- End of mongo --------------------------- */
